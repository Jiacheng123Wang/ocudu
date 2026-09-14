// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU LDPC decoder: ocudu ldpc_decoder interface over the layered
/// normalized min-sum kernel. The packed H matrix and the CSR edge layout are
/// generated in-memory from the ocudu ldpc_graph (the same 3GPP protograph the
/// CPU decoder runs), covering BG1, BG2 and every lifting size.
///
/// Note: the decode() orchestration skeleton (assertions, tail trimming, early
/// return, CRC/syndrome verdict) derives from the upstream srsRAN
/// ldpc_decoder_impl.cpp; the GPU packing, dispatch and result-mapping logic is
/// new.

#include "ldpc_decoder_metal.h"
#include "ocudu/support/executors/phy_shutdown_report.h"
#include "ldpc_graph_impl.h"
#include "ocudu_metal_decoder_engine.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/ocudu_assert.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

using namespace ocudu;

namespace {

// The NMS kernels consume a native int8 buffer with a plain memcpy; the int8->fp16
// conversion happens inside the GPU (one preprocessing dispatch per decode). The LLS
// kernels consume fp16 directly, so the adapter converts on the host (LLS mode only).

// ---- Process-exit cost split of decode() --------------------------------------------------------
// The uplink segment probe ([ul_ldpc_decode]) reports the CPU wall time of a decode, which mixes
// three very different things: the GPU kernels, the submission queue in front of them, and the
// host-side LLR layout / hard-bit repacking. The command buffer carries its own GPU window
// (GPUStartTime..GPUEndTime, exposed by the engine as last_gpu_wait_us()), so splitting the wall
// time separates them: a decode whose wall is mostly GPU time is a kernel problem, one whose wall is
// mostly gap (wall - gpu) is a queueing problem - the whole uplink chain submits on the same
// back-end queue, so the difference is also the ordering cost between the stages.
struct ldpc_time_stats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> ok{0};        // decode() returned a value (CRC OK / clean syndrome)
  std::atomic<uint64_t> ko{0};        // no convergence, or the engine call failed
  std::atomic<uint64_t> wall_ns{0};   // whole decode()
  std::atomic<uint64_t> pack_ns{0};   // LLR layout (memset + erasure bias + payload copy)
  std::atomic<uint64_t> submit_ns{0}; // engine call: commit + waitUntilCompleted + result extraction
  std::atomic<uint64_t> gpu_ns{0};    // command buffer GPU window (0 when the timestamps are unavailable)
  std::atomic<uint64_t> unpack_ns{0}; // hard-bit repacking + CRC / syndrome verdict
  std::atomic<uint64_t> max_wall_ns{0};
  std::atomic<uint64_t> max_gap_ns{0}; // max(wall - gpu) over the calls
  std::atomic<uint64_t> iters_sum{0};
  std::atomic<uint64_t> cap_sum{0};
  std::atomic<uint64_t> iters_hist[64]{}; // index = iterations actually run (0 = engine failure)
  std::atomic<uint64_t> cap_hist[64]{};   // index = requested max_iterations
};

/// Per-(algorithm, base graph, lifting size) split: the aggregate above mixes geometries whose cost
/// differs by orders of magnitude. The dispatch-per-layer path pays per layer AND per iteration, so
/// the iteration histogram of the actual gNB geometry is what a tuning decision needs.
struct ldpc_shape_stats {
  uint64_t calls     = 0;
  uint64_t wall_ns   = 0;
  uint64_t gpu_ns    = 0;
  uint64_t iters_sum = 0;
  uint64_t ko        = 0;
};

/// \note Leaked for the same reason as the aggregate above.
std::mutex& ldpc_shapes_mutex()
{
  static std::mutex* m = new std::mutex();
  return *m;
}

std::map<uint64_t, ldpc_shape_stats>& ldpc_shapes()
{
  static std::map<uint64_t, ldpc_shape_stats>* m = new std::map<uint64_t, ldpc_shape_stats>();
  return *m;
}

uint64_t ldpc_shape_key(metal::decoder_engine::algo mode, unsigned bg, unsigned z)
{
  return (static_cast<uint64_t>(mode) << 32) | (static_cast<uint64_t>(bg) << 24) | z;
}

const char* ldpc_mode_name(metal::decoder_engine::algo mode)
{
  switch (mode) {
    case metal::decoder_engine::algo::layered:
      return "layered";
    case metal::decoder_engine::algo::flooding:
      return "flooding";
    case metal::decoder_engine::algo::lls:
      return "lls";
    case metal::decoder_engine::algo::layered_persistent:
      return "persistent";
    case metal::decoder_engine::algo::async_delta:
      return "async";
  }
  return "unknown";
}

/// \note Intentionally leaked: the atexit report runs while the process statics are already being
/// destroyed, and a function-local static with a destructor would be gone before it (see the
/// estimator's probe for the same pitfall).
ldpc_time_stats& ldpc_time_stats_get()
{
  static ldpc_time_stats* s = new ldpc_time_stats();
  return *s;
}

void ldpc_time_bump_max(std::atomic<uint64_t>& v, uint64_t candidate)
{
  uint64_t prev = v.load(std::memory_order_relaxed);
  while (candidate > prev && !v.compare_exchange_weak(prev, candidate, std::memory_order_relaxed)) {
  }
}

void ldpc_time_stats_report()
{
  const ldpc_time_stats& s = ldpc_time_stats_get();
  const uint64_t         n = s.calls.load(std::memory_order_relaxed);
  if (n == 0) {
    return;
  }
  const auto mean = [n](const std::atomic<uint64_t>& v) {
    return static_cast<double>(v.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3;
  };
  // The summary and its two histograms are one logical record: assemble them and log once, so the
  // line is not split across fragments (which also left the histograms on the console).
  fmt::memory_buffer hist;
  fmt::format_to(std::back_inserter(hist), " iters hist:");
  for (unsigned i = 0; i != 64; ++i) {
    const uint64_t c = s.iters_hist[i].load(std::memory_order_relaxed);
    if (c != 0) {
      fmt::format_to(std::back_inserter(hist), " {}={}", i, c);
    }
  }
  fmt::format_to(std::back_inserter(hist), " | cap hist:");
  for (unsigned i = 0; i != 64; ++i) {
    const uint64_t c = s.cap_hist[i].load(std::memory_order_relaxed);
    if (c != 0) {
      fmt::format_to(std::back_inserter(hist), " {}={}", i, c);
    }
  }
  ocudulog::fetch_basic_logger("PHY").debug(
      "[ldpc_time_sum] calls={} ok={} ko={} | mean wall={:.1f}us pack={:.1f}us submit={:.1f}us gpu={:.1f}us "
      "gap={:.1f}us unpack={:.1f}us | iters mean={:.2f} cap mean={:.2f} | max wall={:.1f}us max gap={:.1f}us |{}",
      n,
      s.ok.load(std::memory_order_relaxed),
      s.ko.load(std::memory_order_relaxed),
      mean(s.wall_ns),
      mean(s.pack_ns),
      mean(s.submit_ns),
      mean(s.gpu_ns),
      (mean(s.wall_ns) > mean(s.gpu_ns)) ? (mean(s.wall_ns) - mean(s.gpu_ns)) : 0.0,
      mean(s.unpack_ns),
      static_cast<double>(s.iters_sum.load(std::memory_order_relaxed)) / static_cast<double>(n),
      static_cast<double>(s.cap_sum.load(std::memory_order_relaxed)) / static_cast<double>(n),
      static_cast<double>(s.max_wall_ns.load(std::memory_order_relaxed)) / 1e3,
      static_cast<double>(s.max_gap_ns.load(std::memory_order_relaxed)) / 1e3,
      fmt::to_string(hist));

  // Per-geometry breakdown, most frequent first.
  std::vector<std::pair<uint64_t, ldpc_shape_stats>> shapes;
  {
    std::lock_guard<std::mutex> lock(ldpc_shapes_mutex());
    shapes.assign(ldpc_shapes().begin(), ldpc_shapes().end());
  }
  std::sort(shapes.begin(), shapes.end(),
            [](const auto& a, const auto& b) { return a.second.calls > b.second.calls; });
  for (const auto& [key, sh] : shapes) {
    if (sh.calls == 0) {
      continue;
    }
    const double cn = static_cast<double>(sh.calls);
    ocudulog::fetch_basic_logger("PHY").info("[ldpc_time_shape] mode=%s bg=%llu z=%llu calls=%llu ko=%llu | mean wall=%.1fus gpu=%.1fus "
                 "gap=%.1fus iters=%.2f",
                 ldpc_mode_name(static_cast<metal::decoder_engine::algo>(key >> 32)),
                 static_cast<unsigned long long>((key >> 24) & 0xff),
                 static_cast<unsigned long long>(key & 0xff),
                 static_cast<unsigned long long>(sh.calls),
                 static_cast<unsigned long long>(sh.ko),
                 static_cast<double>(sh.wall_ns) / cn / 1e3,
                 static_cast<double>(sh.gpu_ns) / cn / 1e3,
                 (static_cast<double>(sh.wall_ns) > static_cast<double>(sh.gpu_ns))
                     ? ((static_cast<double>(sh.wall_ns) - static_cast<double>(sh.gpu_ns)) / cn / 1e3)
                     : 0.0,
                 static_cast<double>(sh.iters_sum) / cn);
  }
}

void ldpc_time_stats_register()
{
  static std::once_flag flag;
  std::call_once(flag, []() { ocudu::phy_shutdown_report::add(ldpc_time_stats_report); });
}

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

struct ldpc_decoder_metal::slot_matrices
{
  unsigned z          = 0;
  unsigned n_aligned  = 0;
  unsigned m_aligned  = 0;
  unsigned n_h_chunks = 0;
  unsigned no_edges   = 0;
  unsigned n_layers   = 0;

  std::unique_ptr<uint32_t, free_deleter> h;
  std::unique_ptr<uint32_t, free_deleter> ht; // flooding / LLS (packed H^T)
  std::unique_ptr<uint32_t, free_deleter> row_start; // layered CSR offsets (m_aligned + 1)
  std::unique_ptr<uint32_t, free_deleter> edge_vn;   // layered CSR edge VN indices
};

struct ldpc_decoder_metal::engine_slot
{
  /// Packed matrices, shared process-wide with the other decoder instances
  /// (built once per (mode, base graph, lifting size), see slot_matrices_cache).
  std::shared_ptr<slot_matrices> m;

  std::unique_ptr<int8_t, free_deleter>   llr_i8;
  std::unique_ptr<uint16_t, free_deleter> llr_fp16; // LLS only (host-filled fp16)
  std::vector<uint8_t>                    hard_bits;
  metal::decoder_engine::layered_info     layered_info;
  std::unique_ptr<metal::decoder_engine>  engine;
};

namespace {

/// Process-wide cache of the packed matrices, keyed by (mode, base graph, z).
/// The pool creates several decoder instances; without the sharing each one
/// rebuilt H/H^T/CSR on its first use of every TB size (a several-ms stall on
/// the live chain per instance per size).
std::mutex                                                        slot_matrices_mtx;
std::unordered_map<uint64_t, std::shared_ptr<ldpc_decoder_metal::slot_matrices>> slot_matrices_cache;

uint64_t slot_matrices_key(metal::decoder_engine::algo mode, ldpc_base_graph_type bg, unsigned z)
{
  return (static_cast<uint64_t>(static_cast<unsigned>(mode)) << 40) |
         (static_cast<uint64_t>(static_cast<unsigned>(bg)) << 32) | z;
}

/// Builds the packed parity-check matrix (and its transpose / CSR layout where the mode
/// needs them) for one (base graph, z) combination from the 3GPP protograph.
std::shared_ptr<ldpc_decoder_metal::slot_matrices> get_shared_matrices(metal::decoder_engine::algo mode,
                                                                       ldpc_base_graph_type       bg,
                                                                       unsigned                   z)
{
  const uint64_t key = slot_matrices_key(mode, bg, z);
  {
    std::lock_guard<std::mutex> lock(slot_matrices_mtx);
    auto                       it = slot_matrices_cache.find(key);
    if (it != slot_matrices_cache.end()) {
      return it->second;
    }
  }

  const auto t_build_begin = std::chrono::steady_clock::now();
  auto       m             = std::make_shared<ldpc_decoder_metal::slot_matrices>();
  m->z                     = z;

  const bool     is_bg1    = bg == ldpc_base_graph_type::BG1;
  const unsigned n_full    = is_bg1 ? 68 : 52;
  const unsigned bg_m      = is_bg1 ? 46 : 42;
  const unsigned n         = n_full * z;
  const unsigned m_rows    = bg_m * z;
  m->n_aligned             = ((n + 31) / 32) * 32;
  m->m_aligned             = ((m_rows + 31) / 32) * 32;
  m->n_h_chunks            = m->n_aligned / 32;

  const bool is_layered_family = (mode == metal::decoder_engine::algo::layered) ||
                                 (mode == metal::decoder_engine::algo::layered_persistent);
  // The layered family computes the syndrome from the CSR edge lists, so the packed H
  // matrix is never materialized for it (order-of-magnitude memory saving, which is what
  // makes the all-size pre-build affordable on unified memory). Flooding / LLS / async
  // kernels walk the packed rows and keep H (and H^T where their kernels need it).
  const bool build_h  = !is_layered_family;
  const bool build_ht = (mode == metal::decoder_engine::algo::flooding) || (mode == metal::decoder_engine::algo::lls);

  const ldpc_graph_impl graph(bg, static_cast<ldpc::lifting_size_t>(z));
  m->n_layers = graph.get_nof_BG_check_nodes();
  if (build_h) {
    m->h = aligned_alloc<uint32_t>(static_cast<size_t>(m->m_aligned) * m->n_h_chunks);
    ocudu_assert(m->h, "Metal LDPC: H allocation failed.");
    std::memset(m->h.get(), 0, static_cast<size_t>(m->m_aligned) * m->n_h_chunks * sizeof(uint32_t));
    if (build_ht) {
      const unsigned h_pred_len = m->m_aligned / 32;
      m->ht = aligned_alloc<uint32_t>(static_cast<size_t>(m->n_aligned) * h_pred_len);
      ocudu_assert(m->ht, "Metal LDPC: H^T allocation failed.");
      std::memset(m->ht.get(), 0, static_cast<size_t>(m->n_aligned) * h_pred_len * sizeof(uint32_t));
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
          m->h.get()[lifted_row * m->n_h_chunks + lifted_col / 32] |= 1u << (lifted_col % 32);
          if (build_ht) {
            const unsigned h_pred_len = m->m_aligned / 32;
            m->ht.get()[lifted_col * h_pred_len + lifted_row / 32] |= 1u << (lifted_row % 32);
          }
        }
      }
    }
  }

  // CSR edge layout: built for the layered family (and async, which also injects over
  // edges). For the layered family it is built DIRECTLY from the protograph - per lifted
  // row the edges come out in ascending column order, exactly the order the previous
  // packed-H scan produced, so the decode stays bit-exact.
  if (mode == metal::decoder_engine::algo::layered || mode == metal::decoder_engine::algo::layered_persistent ||
      mode == metal::decoder_engine::algo::async_delta) {
    uint32_t no_edges = 0;
    for (unsigned row = 0; row != graph.get_nof_BG_check_nodes(); ++row) {
      for (unsigned col = 0; col != graph.get_nof_BG_var_nodes_full(); ++col) {
        if (graph.get_lifted_node(row, col) != ldpc::NO_EDGE) {
          no_edges += z;
        }
      }
    }
    m->no_edges  = no_edges;
    m->row_start = aligned_alloc<uint32_t>(static_cast<size_t>(m->m_aligned) + 1);
    m->edge_vn   = aligned_alloc<uint32_t>(m->no_edges);
    ocudu_assert(m->row_start && m->edge_vn, "Metal LDPC: CSR allocation failed.");
    uint32_t cursor      = 0;
    m->row_start.get()[0] = 0;
    if (is_layered_family) {
      // Direct protograph enumeration (bit-exact edge order, no packed-H scan).
      for (unsigned row = 0; row != graph.get_nof_BG_check_nodes(); ++row) {
        for (unsigned k = 0; k != z; ++k) {
          for (unsigned col = 0; col != graph.get_nof_BG_var_nodes_full(); ++col) {
            const uint16_t shift = graph.get_lifted_node(row, col);
            if (shift == ldpc::NO_EDGE) {
              continue;
            }
            m->edge_vn.get()[cursor++] = col * z + ((k + shift) % z);
          }
          m->row_start.get()[row * z + k + 1] = cursor;
        }
      }
      for (unsigned r = graph.get_nof_BG_check_nodes() * z; r != m->m_aligned; ++r) {
        m->row_start.get()[r + 1] = cursor; // padded rows carry no edges
      }
    } else {
      // Async: scan the packed H as before (it is materialized for the async kernels).
      for (unsigned r = 0; r != m->m_aligned; ++r) {
        for (unsigned c = 0; c != m->n_h_chunks; ++c) {
          uint32_t mask = m->h.get()[r * m->n_h_chunks + c];
          while (mask != 0) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(mask));
            m->edge_vn.get()[cursor++] = c * 32 + bit;
            mask &= (mask - 1);
          }
        }
        m->row_start.get()[r + 1] = cursor;
      }
    }
  }

  const auto t_build_end = std::chrono::steady_clock::now();
  std::size_t cache_size = 0;
  {
    std::lock_guard<std::mutex> lock(slot_matrices_mtx);
    auto [it, inserted] = slot_matrices_cache.emplace(key, m);
    if (!inserted) {
      // Another thread built it first; keep its entry (ours is discarded).
      m = it->second;
    }
    cache_size = slot_matrices_cache.size();
  }
  ocudulog::fetch_basic_logger("PHY").debug(
      "Metal LDPC: built matrices bg={} z={} in {:.1f}us (m={} n={}, edges={}, cache={})",
      static_cast<unsigned>(bg),
      z,
      std::chrono::duration<double, std::micro>(t_build_end - t_build_begin).count(),
      m->m_aligned,
      m->n_aligned,
      m->no_edges,
      cache_size);
  return m;
}

} // namespace

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
  // "Provisions before the troops march" (docs/apple_silicon_heterogeneous_gnb_plan_english.md): everything
  // preparable is prepared at construction (gnb startup) - engine and matrix
  // initialization must never land on the packet path. For the layered family the
  // CSR-only representation makes the ALL-SIZE pre-build affordable (~40 MB shared
  // matrices + ~37 MB engine buffers per instance on unified memory); the
  // flooding/LLS/async kernels still need the packed H (GB-scale across all sizes),
  // so those modes keep the lazy build and only the family JIT is pre-warmed here.
  if (mode == metal::decoder_engine::algo::layered || mode == metal::decoder_engine::algo::layered_persistent) {
    for (unsigned bg_idx = 0; bg_idx != 2; ++bg_idx) {
      for (unsigned zi = 0; zi != ldpc::NOF_LIFTING_SIZES; ++zi) {
        (void)get_slot(bg_idx == 0 ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2,
                       ldpc::all_lifting_sizes[zi]);
      }
    }
  } else {
    (void)get_slot(ldpc_base_graph_type::BG2, ldpc::lifting_size_t::LS2);
  }
}

ldpc_decoder_metal::~ldpc_decoder_metal() = default;

void ldpc_decoder_metal::set_lls_params(const metal::decoder_engine::lls_params& p)
{
  std::lock_guard<std::mutex> lock(decode_mtx);
  lls_params_     = p;
  lls_params_set_ = true;
  // Drop the LLS engine slots so the next decode rebuilds them with the new
  // parameters (the NMS-mode slots are unaffected).
  for (auto it = slots.begin(); it != slots.end();) {
    if (mode == metal::decoder_engine::algo::lls) {
      it = slots.erase(it);
    } else {
      ++it;
    }
  }
}

ldpc_decoder_metal::engine_slot& ldpc_decoder_metal::get_slot(ldpc_base_graph_type bg, ldpc::lifting_size_t ls)
{
  const unsigned z = static_cast<unsigned>(ls);

  auto key = std::make_pair(static_cast<unsigned>(bg), z);
  auto it  = slots.find(key);
  if (it != slots.end()) {
    return *it->second;
  }

  const auto t_begin = std::chrono::steady_clock::now();

  const bool     is_bg1 = bg == ldpc_base_graph_type::BG1;
  const unsigned n_full = is_bg1 ? 68 : 52;
  const unsigned bg_m   = is_bg1 ? 46 : 42;
  const unsigned n      = n_full * z;
  const unsigned m      = bg_m * z;

  auto slot = std::make_unique<engine_slot>();
  // Packed matrices: built once per (mode, bg, z) PROCESS-WIDE and shared by every
  // decoder instance (the per-instance lazy build used to stall each new TB size).
  slot->m = get_shared_matrices(mode, bg, z);

  slot->llr_i8 = aligned_alloc<int8_t>(slot->m->n_aligned);
  slot->hard_bits.resize(n_full * z - m);
  if (mode == metal::decoder_engine::algo::lls) {
    slot->llr_fp16 = aligned_alloc<uint16_t>(slot->m->n_aligned);
  }
  ocudu_assert(slot->llr_i8 && (mode != metal::decoder_engine::algo::lls || slot->llr_fp16),
               "Metal LDPC: aligned allocation failed.");

  slot->layered_info.n_layers  = slot->m->n_layers;
  slot->layered_info.z         = z;
  slot->layered_info.row_start = slot->m->row_start.get();
  slot->layered_info.edge_vn   = slot->m->edge_vn.get();
  slot->layered_info.no_edges  = slot->m->no_edges;

  // Per-mode defaults from the BLER benchmark sweeps (PLAN.md 4.8/4.9/4.15): the
  // layered schedule's inherent damping allows a high norm and the (0.7, 0.5) pair
  // closes the residual waterfall points to CPU parity; flooding/async use the
  // flooding-tuned defaults; LLS uses alpha 1.5 (Phase 1 champion) unless overridden.
  float factor = 0.0F;
  float beta   = 0.0F;
  if (mode == metal::decoder_engine::algo::flooding) {
    factor = (factor_override >= 0.0F) ? factor_override : 0.45F;
    beta   = (beta_override >= 0.0F) ? beta_override : 0.0F;
  } else if (mode == metal::decoder_engine::algo::lls) {
    factor = lls_params_set_ ? lls_params_.alpha : (factor_override >= 0.0F ? factor_override : 1.5F);
    beta   = (beta_override >= 0.0F) ? beta_override : 0.0F;
  } else {
    const bool is_async = mode == metal::decoder_engine::algo::async_delta;
    factor              = (factor_override >= 0.0F) ? factor_override : (is_async ? 0.45F : 0.7F);
    beta                = (beta_override >= 0.0F) ? beta_override : (is_async ? 0.0F : 0.5F);
  }
  const metal::decoder_engine::algo init_mode =
      (mode == metal::decoder_engine::algo::async_delta)          ? metal::decoder_engine::algo::async_delta
      : (mode == metal::decoder_engine::algo::layered_persistent) ? metal::decoder_engine::algo::layered_persistent
      : (mode == metal::decoder_engine::algo::flooding)           ? metal::decoder_engine::algo::flooding
      : (mode == metal::decoder_engine::algo::lls)                ? metal::decoder_engine::algo::lls
                                                                  : metal::decoder_engine::algo::layered;
  slot->engine = std::make_unique<metal::decoder_engine>();
  if (!slot->engine->init(n,
                          m,
                          factor,
                          beta,
                          slot->m->h.get(),
                          slot->m->ht.get(),
                          slot->layered_info,
                          init_mode,
                          enable_et,
                          (init_mode == metal::decoder_engine::algo::lls && lls_params_set_) ? &lls_params_ : nullptr)) {
    ocudu_assert(false, "Metal LDPC: GPU engine initialization failed.");
  }

  ocudulog::fetch_basic_logger("PHY").debug(
      "Metal LDPC: engine slot bg={} z={} ready in {:.1f}us",
      static_cast<unsigned>(bg),
      z,
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_begin).count());

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

  ldpc_time_stats_register();
  const auto t_begin = std::chrono::steady_clock::now();

  // Reset the per-call Metal library duration: it is set again only when the decode
  // actually dispatches to the GPU, so early returns (e.g. a too-short input) do not
  // leak the previous decode's measurement through get_last_decode_metal_elapsed().
  last_gpu_wait_us_ = 0.0;

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

  // Stamped around the packing and around the engine call, then folded by record() below.
  std::chrono::steady_clock::time_point t_packed{};
  std::chrono::steady_clock::time_point t_engine{};

  uint32_t error_count = 0;
  int      iters       = -1;
  if (mode == metal::decoder_engine::algo::lls) {
    // LLS: lay out the full codeblock in fp16: [2Z punctured][input][tail]. The GPU kernels
    // update the LLRs in place, so the whole buffer is refilled every call. The structural
    // erasures (punctured 2Z columns and the un-transmitted tail) get a weak +1 bias: the LLS
    // update treats an exact 0 as "bit 0 with sign +", which corrupts the erasure handling
    // (see PLAN.md 4.6).
    std::memset(slot.llr_fp16.get(), 0, static_cast<size_t>(slot.m->n_aligned) * sizeof(uint16_t));
    const uint16_t erasure_bias = llr_to_fp16(1);
    std::fill(slot.llr_fp16.get(), slot.llr_fp16.get() + 2 * z, erasure_bias);
    std::fill(slot.llr_fp16.get() + 2 * z + input.size(), slot.llr_fp16.get() + slot.m->n_aligned, erasure_bias);
    uint16_t* fp16 = slot.llr_fp16.get() + 2 * z;
    for (const log_likelihood_ratio llr : input) {
      *fp16++ = llr_to_fp16(llr.to_int());
    }
    t_packed = std::chrono::steady_clock::now();
    iters = slot.engine->decode(slot.llr_fp16.get(), slot.hard_bits.data(),
                                static_cast<int>(cfg.max_iterations), &error_count);
  } else {
    // NMS: lay out the full codeblock in native int8: [2Z punctured][input][tail] with
    // a single memcpy (the int8->fp16 conversion runs inside the GPU). The
    // structural erasures get a weak +1 bias as before.
    static_assert(sizeof(log_likelihood_ratio) == 1, "LLR storage must be a single byte");
    std::memset(slot.llr_i8.get(), 0, static_cast<size_t>(slot.m->n_aligned) * sizeof(int8_t));
    std::fill(slot.llr_i8.get(), slot.llr_i8.get() + 2 * z, int8_t{1});
    std::fill(slot.llr_i8.get() + 2 * z + input.size(), slot.llr_i8.get() + slot.m->n_aligned, int8_t{1});
    std::memcpy(slot.llr_i8.get() + 2 * z, input.data(), input.size() * sizeof(int8_t));

    t_packed = std::chrono::steady_clock::now();
    iters = slot.engine->decode(slot.llr_i8.get(), slot.hard_bits.data(),
                                static_cast<int>(cfg.max_iterations), &error_count);
  }
  /// Folds one decode() into the process-exit split. \c verdict is what the caller gets back (empty
  /// on no convergence), \c gpu_us the command buffer's GPU window (0 when unavailable).
  const auto record = [&](std::optional<unsigned> verdict, double gpu_us) {
    const auto t_end = std::chrono::steady_clock::now();
    const auto ns    = [](auto a, auto b) {
      return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    };
    ldpc_time_stats& s    = ldpc_time_stats_get();
    const uint64_t   wall = ns(t_begin, t_end);
    const uint64_t   gpu  = static_cast<uint64_t>(gpu_us * 1e3);
    s.calls.fetch_add(1, std::memory_order_relaxed);
    (verdict.has_value() ? s.ok : s.ko).fetch_add(1, std::memory_order_relaxed);
    s.wall_ns.fetch_add(wall, std::memory_order_relaxed);
    s.pack_ns.fetch_add(ns(t_begin, t_packed), std::memory_order_relaxed);
    s.submit_ns.fetch_add(ns(t_packed, t_engine), std::memory_order_relaxed);
    s.gpu_ns.fetch_add(gpu, std::memory_order_relaxed);
    s.unpack_ns.fetch_add(ns(t_engine, t_end), std::memory_order_relaxed);
    ldpc_time_bump_max(s.max_wall_ns, wall);
    ldpc_time_bump_max(s.max_gap_ns, (wall > gpu) ? (wall - gpu) : 0);
    s.iters_sum.fetch_add((iters > 0) ? static_cast<uint64_t>(iters) : 0, std::memory_order_relaxed);
    s.cap_sum.fetch_add(static_cast<uint64_t>(cfg.max_iterations), std::memory_order_relaxed);
    s.iters_hist[(iters > 0) ? std::min<unsigned>(static_cast<unsigned>(iters), 63u) : 0u].fetch_add(
        1, std::memory_order_relaxed);
    s.cap_hist[std::min<unsigned>(cfg.max_iterations, 63u)].fetch_add(1, std::memory_order_relaxed);

    // Per-geometry copy of the same split (the gNB geometry is what a tuning decision reads).
    const uint64_t key = ldpc_shape_key(mode, (cfg.base_graph == ldpc_base_graph_type::BG1) ? 1u : 2u,
                                        static_cast<unsigned>(cfg.lifting_size));
    std::lock_guard<std::mutex> lock(ldpc_shapes_mutex());
    ldpc_shape_stats&           sh = ldpc_shapes()[key];
    sh.calls += 1;
    sh.wall_ns += wall;
    sh.gpu_ns += gpu;
    sh.iters_sum += (iters > 0) ? static_cast<uint64_t>(iters) : 0;
    sh.ko += verdict.has_value() ? 0 : 1;
  };

  t_engine = std::chrono::steady_clock::now();
  if (iters < 0) {
    record(std::nullopt, 0.0);
    return std::nullopt;
  }
  last_gpu_wait_us_ = slot.engine->last_gpu_wait_us();

  // Repack the K message bits (filler bits included; the segmenter strips them).
  for (unsigned i = 0; i != message_length; ++i) {
    output.insert(slot.hard_bits[i], i, 1);
  }

  const unsigned nof_significant_bits = message_length - cfg.nof_filler_bits;

  // Single exit so that every path folds its cost into the split (the verdict itself is unchanged).
  std::optional<unsigned> verdict;
  if (crc != nullptr) {
    if (crc->calculate(output.first(nof_significant_bits)) == 0) {
      verdict = static_cast<unsigned>(iters);
    }
  } else if (error_count == 0) {
    // No CRC: fall back to the syndrome check (GPU-side early stop always applies internally).
    verdict = early_stop_syndrome ? std::optional<unsigned>(static_cast<unsigned>(iters))
                                  : std::optional<unsigned>(cfg.max_iterations);
  }
  record(verdict, last_gpu_wait_us_);
  return verdict;
}

std::optional<std::chrono::nanoseconds> ldpc_decoder_metal::get_last_decode_metal_elapsed() const
{
  // 0 means the last decode did not invoke the GPU (reset at the start of decode()) or that the
  // GPU timestamps were unavailable.
  if (last_gpu_wait_us_ <= 0.0) {
    return std::nullopt;
  }
  return std::chrono::nanoseconds(static_cast<long long>(last_gpu_wait_us_ * 1e3));
}
