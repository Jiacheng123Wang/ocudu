// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU LDPC decoder: ocudu ldpc_decoder interface over the layered
/// normalized min-sum kernel. The packed H matrix and the CSR edge layout are
/// generated in-memory from the ocudu ldpc_graph (the same 3GPP protograph the
/// CPU decoder runs), covering BG1, BG2 and every lifting size.

#include "ldpc_decoder_metal.h"
#include "ldpc_graph_impl.h"
#include "ocudu_metal_decoder_engine.h"
#include "ocudu/support/ocudu_assert.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace ocudu;

namespace {

// The adapter fills a native int8 buffer with a plain memcpy; the int8->fp16
// conversion happens inside the GPU (one preprocessing dispatch per decode).

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

  std::unique_ptr<metal::decoder_engine> engine;
  std::unique_ptr<uint32_t, free_deleter> h;
  std::unique_ptr<uint32_t, free_deleter> ht; // flooding only (packed H^T)
  std::unique_ptr<int8_t, free_deleter> llr_i8;
  std::vector<uint8_t>                   hard_bits;
  // CSR edge layout (built once per slot). The fused CN+VN kernel needs no
  // per-layer column tables (see ocudu_nms_layered_decoder.metal).
  std::unique_ptr<uint32_t, free_deleter> row_start;
  std::unique_ptr<uint32_t, free_deleter> edge_vn;
  metal::decoder_engine::layered_info   layered_info;
};

ldpc_decoder_metal::ldpc_decoder_metal(bool force_decoding_, bool early_stop_syndrome_,
                                       metal::decoder_engine::algo mode_, float factor_override_,
                                       float beta_override_, bool enable_et_) :
  force_decoding(force_decoding_),
  early_stop_syndrome(early_stop_syndrome_),
  mode(mode_),
  factor_override(factor_override_),
  beta_override(beta_override_),
  enable_et(enable_et_)
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

  const bool     is_bg1    = bg == ldpc_base_graph_type::BG1;
  const unsigned n_full    = is_bg1 ? 68 : 52;
  const unsigned bg_m      = is_bg1 ? 46 : 42;
  const unsigned n         = n_full * z;
  const unsigned m         = bg_m * z;
  const unsigned n_aligned = ((n + 31) / 32) * 32;
  const unsigned m_aligned = ((m + 31) / 32) * 32;

  auto slot        = std::make_unique<engine_slot>();
  slot->z          = z;
  slot->n_aligned  = n_aligned;
  slot->m_aligned  = m_aligned;
  slot->n_h_chunks = n_aligned / 32;

  slot->h        = aligned_alloc<uint32_t>(static_cast<size_t>(m_aligned) * slot->n_h_chunks);
  slot->llr_i8 = aligned_alloc<int8_t>(n_aligned);
  slot->hard_bits.resize(n_full * z - m);
  ocudu_assert(slot->h && slot->llr_i8, "Metal LDPC: aligned allocation failed.");

  // Build the packed parity-check matrix (and its transpose in flooding mode)
  // from the 3GPP protograph.
  const ldpc_graph_impl graph(bg, ls);
  {
    const size_t h_size = static_cast<size_t>(m_aligned) * slot->n_h_chunks;
    std::memset(slot->h.get(), 0, h_size * sizeof(uint32_t));
    if (mode == metal::decoder_engine::algo::flooding) {
      const unsigned h_pred_len = m_aligned / 32;
      slot->ht = aligned_alloc<uint32_t>(static_cast<size_t>(n_aligned) * h_pred_len);
      ocudu_assert(slot->ht, "Metal LDPC: H^T allocation failed.");
      std::memset(slot->ht.get(), 0, static_cast<size_t>(n_aligned) * h_pred_len * sizeof(uint32_t));
    }

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
          if (mode == metal::decoder_engine::algo::flooding) {
            const unsigned h_pred_len = m_aligned / 32;
            slot->ht.get()[lifted_col * h_pred_len + lifted_row / 32] |= 1u << (lifted_row % 32);
          }
        }
      }
    }
  }

  // CSR edge layout (row offsets + edge VN indices), layered mode only.
  const unsigned n_layers = graph.get_nof_BG_check_nodes();
  if (mode == metal::decoder_engine::algo::flooding) {
    slot->layered_info.n_layers = n_layers;
    slot->layered_info.z        = z;
    slot->layered_info.row_start = nullptr;
    slot->layered_info.edge_vn   = nullptr;
    slot->layered_info.no_edges  = 0;
    slot->engine = std::make_unique<metal::decoder_engine>();
    // Flooding defaults: norm 0.45 (fp16-era flooding optimum; the layered
    // damping does not apply to the parallel schedule).
    const float factor = (factor_override >= 0.0F) ? factor_override : 0.45F;
    const float beta   = (beta_override >= 0.0F) ? beta_override : 0.0F;
    if (!slot->engine->init(n, m, factor, beta, slot->h.get(), slot->ht.get(), slot->layered_info,
                            metal::decoder_engine::algo::flooding, enable_et)) {
      ocudu_assert(false, "Metal LDPC: GPU engine initialization failed.");
    }
    engine_slot& ref = *slot;
    slots.emplace(std::move(key), std::move(slot));
    return ref;
  }
  uint32_t       no_edges = 0;
  for (unsigned r = 0; r != m_aligned; ++r) {
    for (unsigned c = 0; c != slot->n_h_chunks; ++c) {
      no_edges += static_cast<uint32_t>(__builtin_popcount(slot->h.get()[r * slot->n_h_chunks + c]));
    }
  }
  slot->row_start = aligned_alloc<uint32_t>(static_cast<size_t>(m_aligned) + 1);
  slot->edge_vn   = aligned_alloc<uint32_t>(no_edges);
  ocudu_assert(slot->row_start && slot->edge_vn, "Metal LDPC: CSR allocation failed.");

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

  slot->layered_info.row_start = slot->row_start.get();
  slot->layered_info.edge_vn   = slot->edge_vn.get();
  slot->layered_info.no_edges  = no_edges;
  slot->layered_info.n_layers  = n_layers;
  slot->layered_info.z         = z;

  slot->engine = std::make_unique<metal::decoder_engine>();
  // Defaults from the BLER benchmark sweeps (see PLAN.md 4.8/4.9): the layered
  // schedule's inherent damping allows a high norm, and the offset min-sum pair
  // (0.7, beta 0.5) closes the residual waterfall points to CPU parity. The
  // persistent variant (metal_persistent) runs the same schedule as one GPU
  // resident dispatch and shares the CSR layout, defaults and buffers. The
  // asynchronous delta-BP variant (metal_async) is flooding-family and uses
  // the flooding-tuned defaults (norm 0.45, beta 0).
  const bool is_async = mode == metal::decoder_engine::algo::async_delta;
  const float factor = (factor_override >= 0.0F) ? factor_override : (is_async ? 0.45F : 0.7F);
  const float beta   = (beta_override >= 0.0F) ? beta_override : (is_async ? 0.0F : 0.5F);
  const metal::decoder_engine::algo init_mode =
      (mode == metal::decoder_engine::algo::async_delta)        ? metal::decoder_engine::algo::async_delta
      : (mode == metal::decoder_engine::algo::layered_persistent) ? metal::decoder_engine::algo::layered_persistent
                                                                  : metal::decoder_engine::algo::layered;
  if (!slot->engine->init(n, m, factor, beta, slot->h.get(), nullptr, slot->layered_info, init_mode,
                          enable_et)) {
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

  // Single-client guard: covers the lazy slot-map insertion (get_slot), the per-slot
  // scratch buffers and the zero-copy wrapper cache. Contention is nil in the gNB
  // (the codeblock-decoder pool hands out one instance per task).
  std::lock_guard<std::mutex> lock(decode_mtx);

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

  // Lay out the full codeblock in native int8: [2Z punctured][input][tail] with
  // a single memcpy (the int8->fp16 conversion runs inside the GPU). The
  // structural erasures get a weak +1 bias as before.
  static_assert(sizeof(log_likelihood_ratio) == 1, "LLR storage must be a single byte");
  std::memset(slot.llr_i8.get(), 0, static_cast<size_t>(slot.n_aligned) * sizeof(int8_t));
  std::fill(slot.llr_i8.get(), slot.llr_i8.get() + 2 * z, int8_t{1});
  std::fill(slot.llr_i8.get() + 2 * z + input.size(), slot.llr_i8.get() + slot.n_aligned, int8_t{1});
  std::memcpy(slot.llr_i8.get() + 2 * z, input.data(), input.size() * sizeof(int8_t));

  uint32_t error_count = 0;
  const int iters = slot.engine->decode(slot.llr_i8.get(), slot.hard_bits.data(),
                                        static_cast<int>(cfg.max_iterations), &error_count);
  if (iters < 0) {
    return std::nullopt;
  }
  last_gpu_wait_us_ = slot.engine->last_gpu_wait_us();

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
