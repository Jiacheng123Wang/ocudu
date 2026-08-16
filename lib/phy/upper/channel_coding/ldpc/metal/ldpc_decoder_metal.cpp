// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU LDPC decoder: ocudu ldpc_decoder interface over the SynchroPlus
/// LLS kernels. The packed H / H^T matrices are generated in-memory from the ocudu
/// ldpc_graph (the same 3GPP protograph the CPU decoder runs), covering BG1, BG2 and
/// every lifting size without relying on the SynchroPlus .bin matrix files.

#include "ldpc_decoder_metal.h"
#include "ldpc_graph_impl.h"
#include "ocudu_metal_decoder_engine.h"
#include "ocudu/support/ocudu_assert.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace ocudu;

namespace {

#if !defined(__arm64__)
/// Generic float -> IEEE 754 half conversion fallback (non-arm64 builds).
static uint16_t float_to_fp16_soft(float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  uint32_t       exp  = (bits >> 23) & 0xffu;
  uint32_t       mant = bits & 0x7fffffu;

  if (exp == 0xffu) { // NaN / Inf
    return static_cast<uint16_t>(sign | 0x7c00u | (mant != 0 ? 0x200u : 0u));
  }
  int32_t e = static_cast<int32_t>(exp) - 127 + 15;
  if (e >= 31) { // overflow
    return static_cast<uint16_t>(sign | 0x7c00u);
  }
  if (e <= 0) { // subnormal / zero
    if (e < -10) {
      return static_cast<uint16_t>(sign);
    }
    mant |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - e);
    uint16_t       out   = static_cast<uint16_t>(mant >> shift);
    if ((mant >> (shift - 1)) & 1u) {
      out++;
    }
    return static_cast<uint16_t>(sign | out);
  }
  // Normal: round to nearest even.
  const uint32_t round_bit = 1u << 12;
  mant += round_bit - 1u + ((mant >> 13) & 1u);
  if (mant & 0x800000u) { // mantissa overflow
    e++;
    if (e >= 31) {
      return static_cast<uint16_t>(sign | 0x7c00u);
    }
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | ((mant >> 13) & 0x3ffu));
}
#endif // !defined(__arm64__)

/// int8 LLR -> fp16 bit pattern, clamped to the same range as the CPU soft bits.
static uint16_t llr_to_fp16(int8_t llr)
{
  const float value = static_cast<float>(std::clamp<int>(llr, -64, 64));
#if defined(__arm64__)
  __fp16    half = static_cast<__fp16>(value);
  uint16_t  out;
  std::memcpy(&out, &half, sizeof(out));
  return out;
#else
  return float_to_fp16_soft(value);
#endif
}

/// Free deleter (a functor, so that unique_ptr stays default-constructible).
struct free_deleter {
  template <typename T>
  void operator()(T* p) const
  {
    std::free(p);
  }
};

/// 4KB-aligned allocation (required by the zero-copy MTLBuffer wrapping).
template <typename T>
static std::unique_ptr<T, free_deleter> aligned_alloc(size_t count)
{
  void* p = nullptr;
  if (::posix_memalign(&p, 4096, count * sizeof(T)) != 0 || p == nullptr) {
    return std::unique_ptr<T, free_deleter>(nullptr);
  }
  return std::unique_ptr<T, free_deleter>(static_cast<T*>(p));
}

} // namespace

struct ldpc_decoder_metal::engine_slot
{
  unsigned z             = 0;
  unsigned n_aligned     = 0;
  unsigned m_aligned     = 0;
  unsigned n_h_chunks    = 0;
  unsigned h_pred_len    = 0;

  std::unique_ptr<metal::decoder_engine> engine;
  std::unique_ptr<uint32_t, free_deleter> h;
  std::unique_ptr<uint32_t, free_deleter> ht;
  std::unique_ptr<uint32_t, free_deleter> col_weights;
  std::unique_ptr<uint16_t, free_deleter> llr_fp16;
  std::vector<uint8_t>                   hard_bits;
  // Layered-NMS CSR edge layout and per-layer column tables (built once per slot).
  std::unique_ptr<uint32_t, free_deleter> row_start;
  std::unique_ptr<uint32_t, free_deleter> edge_vn;
  std::unique_ptr<uint32_t, free_deleter> layer_descs; // n_layers x 22 uint32
  metal::decoder_engine::layered_info   layered_info;
};

ldpc_decoder_metal::ldpc_decoder_metal(bool force_decoding_, bool early_stop_syndrome_,
                                       metal::decoder_engine::algo mode_, float factor_override_,
                                       float sat_override_) :
  force_decoding(force_decoding_),
  early_stop_syndrome(early_stop_syndrome_),
  mode(mode_),
  factor_override(factor_override_),
  sat_override(sat_override_)
{
}

ldpc_decoder_metal::~ldpc_decoder_metal() = default;

ldpc_decoder_metal::engine_slot& ldpc_decoder_metal::get_slot(ldpc_base_graph_type bg, ldpc::lifting_size_t ls)
{
  const unsigned z = static_cast<unsigned>(ls);

  auto key = std::make_pair(static_cast<unsigned>(bg), z);
  auto it  = slots.find(key);
  if (it != slots.end()) {
    return *it->second;
  }

  const bool     is_bg1  = bg == ldpc_base_graph_type::BG1;
  const unsigned n_full  = is_bg1 ? 68 : 52;
  const unsigned bg_m    = is_bg1 ? 46 : 42;
  const unsigned n       = n_full * z;
  const unsigned m       = bg_m * z;
  const unsigned n_aligned = ((n + 31) / 32) * 32;
  const unsigned m_aligned = ((m + 31) / 32) * 32;

  auto slot       = std::make_unique<engine_slot>();
  slot->z         = z;
  slot->n_aligned = n_aligned;
  slot->m_aligned = m_aligned;
  slot->n_h_chunks = n_aligned / 32;
  slot->h_pred_len = m_aligned / 32;

  slot->h          = aligned_alloc<uint32_t>(static_cast<size_t>(m_aligned) * slot->n_h_chunks);
  slot->ht         = aligned_alloc<uint32_t>(static_cast<size_t>(n_aligned) * slot->h_pred_len);
  slot->col_weights = aligned_alloc<uint32_t>(n_aligned);
  slot->llr_fp16   = aligned_alloc<uint16_t>(n_aligned);
  slot->hard_bits.resize(n_full * z - m);
  ocudu_assert(slot->h && slot->ht && slot->col_weights && slot->llr_fp16, "Metal LDPC: aligned allocation failed.");

  // Build the packed parity-check matrix and its transpose from the 3GPP protograph.
  const ldpc_graph_impl graph(bg, ls);
  {
    const size_t          h_size  = static_cast<size_t>(m_aligned) * slot->n_h_chunks;
    const size_t          ht_size = static_cast<size_t>(n_aligned) * slot->h_pred_len;
    std::memset(slot->h.get(), 0, h_size * sizeof(uint32_t));
    std::memset(slot->ht.get(), 0, ht_size * sizeof(uint32_t));

    for (unsigned row = 0; row != graph.get_nof_BG_check_nodes(); ++row) {
      for (unsigned col = 0; col != graph.get_nof_BG_var_nodes_full(); ++col) {
        const uint16_t shift = graph.get_lifted_node(row, col);
        if (shift == ldpc::NO_EDGE) {
          continue;
        }
        for (unsigned k = 0; k != z; ++k) {
          const unsigned lifted_row = row * z + k;
          const unsigned lifted_col = col * z + ((k + shift) % z);
          slot->h.get()[lifted_row * slot->n_h_chunks + lifted_col / 32] |= 1u << (lifted_col % 32);
          slot->ht.get()[lifted_col * slot->h_pred_len + lifted_row / 32] |= 1u << (lifted_row % 32);
        }
      }
    }
  }

  // Layered-NMS CSR edge layout and per-layer tables.
  if (mode == metal::decoder_engine::algo::nms_layered) {
    const unsigned n_layers = graph.get_nof_BG_check_nodes();
    uint32_t       no_edges = 0;
    for (unsigned r = 0; r != m_aligned; ++r) {
      for (unsigned c = 0; c != slot->n_h_chunks; ++c) {
        no_edges += static_cast<uint32_t>(__builtin_popcount(slot->h.get()[r * slot->n_h_chunks + c]));
      }
    }
    slot->row_start = aligned_alloc<uint32_t>(static_cast<size_t>(m_aligned) + 1);
    slot->edge_vn   = aligned_alloc<uint32_t>(no_edges);
    slot->layer_descs = aligned_alloc<uint32_t>(static_cast<size_t>(n_layers) * 22);
    ocudu_assert(slot->row_start && slot->edge_vn && slot->layer_descs, "Metal LDPC: CSR allocation failed.");

    uint32_t cursor = 0;
    slot->row_start.get()[0] = 0;
    for (unsigned r = 0; r != m_aligned; ++r) {
      for (unsigned c = 0; c != slot->n_h_chunks; ++c) {
        uint32_t mask = slot->h.get()[r * slot->n_h_chunks + c];
        while (mask != 0) {
          const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
          slot->edge_vn.get()[cursor++] = c * 32 + bit;
          mask &= (mask - 1);
        }
      }
      slot->row_start.get()[r + 1] = cursor;
    }

    for (unsigned l = 0; l != n_layers; ++l) {
      uint32_t* desc = slot->layer_descs.get() + static_cast<size_t>(l) * 22;
      desc[0] = l;
      unsigned nof_cols = 0;
      for (uint16_t k : graph.get_adjacency_row(l)) {
        if (k == ldpc::NO_EDGE) {
          break;
        }
        desc[2 + nof_cols++] = k;
      }
      desc[1] = nof_cols;
    }

    slot->layered_info.row_start   = slot->row_start.get();
    slot->layered_info.edge_vn     = slot->edge_vn.get();
    slot->layered_info.no_edges    = no_edges;
    slot->layered_info.n_layers    = n_layers;
    slot->layered_info.z           = z;
    slot->layered_info.layer_descs = slot->layer_descs.get();
  }

  slot->engine = std::make_unique<metal::decoder_engine>();
  // LLS step size (0.45 is the SynchroPlus reference; 0.8 measured slightly better on
  // ocudu-quantized int8 LLRs). NMS normalization (BLER benchmark, see PLAN.md 4.8):
  // flooding optimum 0.45; the layered schedule's inherent damping allows 0.6, which
  // closes the residual waterfall gap to the CPU (the CPU generic uses 1.0, which
  // oscillates on fp16 GPU arithmetic).
  const float factor = (factor_override >= 0.0F)
                           ? factor_override
                           : ((mode == metal::decoder_engine::algo::lls)          ? 0.8F
                              : (mode == metal::decoder_engine::algo::nms_layered) ? 0.6F
                                                                                   : 0.45F);
  uint32_t*       col_weights = (mode == metal::decoder_engine::algo::lls) ? slot->col_weights.get() : nullptr;
  const float     sat = (sat_override >= 0.0F) ? sat_override : 0.0F;
  const metal::decoder_engine::layered_info* layered =
      (mode == metal::decoder_engine::algo::nms_layered) ? &slot->layered_info : nullptr;
  if (!slot->engine->init(n, m, factor, slot->h.get(), slot->ht.get(), col_weights, mode, sat, layered)) {
    ocudu_assert(false, "Metal LDPC: GPU engine initialization failed.");
  }

  engine_slot& ref = *slot;
  slots.emplace(std::move(key), std::move(slot));
  return ref;
}

std::optional<unsigned> ldpc_decoder_metal::decode(bit_buffer&                    output,
                                                   span<const log_likelihood_ratio> input,
                                                   crc_calculator*                crc,
                                                   const configuration&           cfg)
{
  ocudu_assert(cfg.max_iterations != 0, "Max iterations must be different to 0");

  const bool     is_bg1 = cfg.base_graph == ldpc_base_graph_type::BG1;
  const unsigned n_full = is_bg1 ? 68 : 52;
  const unsigned bg_k   = is_bg1 ? 22 : 10;
  const unsigned z      = static_cast<unsigned>(cfg.lifting_size);

  const unsigned message_length = bg_k * z;
  ocudu_assert(output.size() == message_length,
               "The output size {} is not equal to the message length {}.",
               output.size(),
               message_length);
  ocudu_assert(input.size() <= (n_full - 2) * z,
               "The input size {} exceeds the maximum message length {}.",
               input.size(),
               (n_full - 2) * z);
  ocudu_assert(input.size() >= message_length + 2 * z,
               "The input length {} does not reach minimum {}",
               input.size(),
               message_length + 2 * z);

  // Trim the un-received tail (zero LLRs) to mirror the CPU early-exit semantics.
  const log_likelihood_ratio* last =
      std::find_if(input.rbegin(), input.rend(), [](const log_likelihood_ratio& in) { return in != 0; }).base();
  const unsigned input_size = static_cast<unsigned>(std::distance(input.begin(), last));

  if ((input_size < message_length) && !force_decoding) {
    // If the codeblock CRC check is external, set all bits to one (so that the CRC will fail).
    if (crc == nullptr) {
      output.one();
    }
    return std::nullopt;
  }

  engine_slot& slot = get_slot(cfg.base_graph, cfg.lifting_size);

  // Lay out the full codeblock in fp16: [2Z punctured][input][tail].
  // The GPU kernels update the LLRs in place, so the whole buffer is refilled every call.
  // The structural erasures (punctured 2Z columns and the un-transmitted tail) get a weak
  // positive bias (+1): the LLS update treats an exact 0 as "bit 0 with sign +", which
  // corrupts the erasure handling (the SynchroPlus sims never exercised puncturing).
  std::memset(slot.llr_fp16.get(), 0, static_cast<size_t>(slot.n_aligned) * sizeof(uint16_t));
  const uint16_t erasure_bias = llr_to_fp16(1);
  std::fill(slot.llr_fp16.get(), slot.llr_fp16.get() + 2 * z, erasure_bias);
  std::fill(slot.llr_fp16.get() + 2 * z + input.size(), slot.llr_fp16.get() + slot.n_aligned, erasure_bias);
  uint16_t* fp16 = slot.llr_fp16.get() + 2 * z;
  for (const log_likelihood_ratio llr : input) {
    *fp16++ = llr_to_fp16(llr.to_int());
  }

  uint32_t error_count = 0;
  const int iters = slot.engine->decode(slot.llr_fp16.get(), slot.hard_bits.data(),
                                        static_cast<int>(cfg.max_iterations), &error_count);
  if (iters < 0) {
    return std::nullopt;
  }

  // Repack the K message bits (filler bits included; the segmenter strips them).
  for (unsigned i = 0; i != message_length; ++i) {
    output.insert(slot.hard_bits[i], i, 1);
  }

  const unsigned nof_significant_bits = message_length - cfg.nof_filler_bits;

  if (crc != nullptr) {
    if (crc->calculate(output.first(nof_significant_bits)) == 0) {
      return static_cast<unsigned>(iters);
    }
    return std::nullopt;
  }

  // No CRC: fall back to the syndrome check (GPU-side early stop always applies internally).
  if (error_count == 0) {
    return early_stop_syndrome ? static_cast<unsigned>(iters) : cfg.max_iterations;
  }
  return std::nullopt;
}
