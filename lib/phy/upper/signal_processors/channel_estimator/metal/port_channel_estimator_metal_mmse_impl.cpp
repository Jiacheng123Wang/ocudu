// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// (Derives from the upstream port_channel_estimator_average_impl base class.)

#include "port_channel_estimator_metal_mmse_impl.h"
#include "../port_channel_estimator_helpers.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/math/math_utils.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <algorithm>
#include <mutex>
#include <vector>

#if defined(OCUDU_CE_TIME)
namespace {

/// \brief Aggregated channel-estimation phase statistics (ENABLE_CE_TIME build only).
///
/// The per-hop [mmse_time] debug line is far too verbose to run under load (tens of lines per
/// slot, which perturbs what it measures) and needs all_level: debug on top. This accumulator
/// keeps the same fields and prints one summary line per process, to stderr, like [metal_stats].
struct mmse_time_stats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> hops_gpu{0};
  std::atomic<uint64_t> hops_no_gpu{0};
  std::atomic<uint64_t> hops_nn{0};
  std::atomic<uint64_t> fallback_blocks{0};
  std::atomic<uint64_t> total_us{0};
  std::atomic<uint64_t> gpu_path_us{0};
  std::atomic<uint64_t> gpu_wait_us{0};
  std::atomic<uint64_t> cpu_blocks_us{0};
  std::atomic<uint64_t> device_hops{0};
  std::atomic<uint64_t> pre_stage_ns{0};
  std::atomic<uint64_t> stage_ns{0};
  std::atomic<uint64_t> submit_ns{0};
  std::atomic<uint64_t> unpack_ns{0};
  std::atomic<uint64_t> completion_wait_ns{0};
  std::atomic<uint64_t> completion_unpack_ns{0};
  std::atomic<uint64_t> completion_fill_ns{0};
  std::atomic<uint64_t> sigma2_us{0};
  std::atomic<uint64_t> corr_us{0};
  std::atomic<uint64_t> deferred_wait_us{0};
  std::atomic<uint64_t> max_total_us{0};
};

mmse_time_stats& mmse_stats()
{
  static mmse_time_stats s;
  return s;
}

void mmse_stats_register_atexit()
{
  static std::once_flag flag;
  std::call_once(flag, []() {
    std::atexit([]() {
      const mmse_time_stats& s = mmse_stats();
      const uint64_t          n = s.calls.load(std::memory_order_relaxed);
      if (n == 0) {
        return;
      }
      const auto avg = [n](const std::atomic<uint64_t>& v) {
        return static_cast<double>(v.load(std::memory_order_relaxed)) / static_cast<double>(n);
      };
      std::fprintf(stderr,
                   "[mmse_time_sum] calls=%llu hops_gpu=%llu hops_no_gpu=%llu hops_nn=%llu fb_blocks=%llu | "
                   "mean total=%.1fus pre=%.2fus stage=%.2fus submit=%.2fus unpack=%.2fus cpl_wait=%.1fus cpl_unpack=%.1fus cpl_fill=%.1fus sigma2=%.1fus corr=%.1fus gpu_path=%.1fus (gpu_wait=%.1fus) "
                   "cpu_blocks=%.1fus defer_wait=%.1fus | device_hops=%llu max total=%lluus\n",
                   static_cast<unsigned long long>(n),
                   static_cast<unsigned long long>(s.hops_gpu.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.hops_no_gpu.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.hops_nn.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.fallback_blocks.load(std::memory_order_relaxed)),
                   avg(s.total_us),
                   static_cast<double>(s.pre_stage_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   static_cast<double>(s.stage_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   static_cast<double>(s.submit_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   static_cast<double>(s.unpack_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   static_cast<double>(s.completion_wait_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   static_cast<double>(s.completion_unpack_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   static_cast<double>(s.completion_fill_ns.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1e3,
                   avg(s.sigma2_us),
                   avg(s.corr_us),
                   avg(s.gpu_path_us),
                   avg(s.gpu_wait_us),
                   avg(s.cpu_blocks_us),
                   avg(s.deferred_wait_us),
                   static_cast<unsigned long long>(s.device_hops.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.max_total_us.load(std::memory_order_relaxed)));

    });
  });
}

/// Counts the hops whose device-side estimates (K3) were produced: a consumer that reads them is
/// what makes the device path observable, and a hop that does not produce them falls back silently.
void mmse_stats_device_hop()
{
  mmse_stats_register_atexit();
  mmse_stats().device_hops.fetch_add(1, std::memory_order_relaxed);
}

/// \param deferred_wait_us Wall time between the end of a deferred stage and the completion of its
///        batch. The stage returns before its batch is done, so this wait happens outside the window
///        the other measurements cover - and, because the estimator is deferred, the rest of the
///        receiving chain (the equalization and the demapping) runs inside it. It is therefore
///        reported on its own instead of being folded into gpu_path/total, which stay the stage's
///        own window and remain comparable with the non-deferred measurements.
void mmse_stats_accumulate(unsigned nof_prb,
                           unsigned nof_dmrs_symbols,
                           bool     hop_gpu,
                           bool     hop_nn,
                           unsigned fallback_blocks,
                           double   pre_stage_us,
                           double   stage_us,
                           double   submit_us,
                           double   unpack_us,
                           double   sigma2_us,
                           double   corr_us,
                           double   gpu_path_us,
                           double   gpu_wait_us,
                           double   cpu_blocks_us,
                           double   total_us,
                           double   deferred_wait_us = 0.0)
{
  mmse_stats_register_atexit();
  mmse_time_stats& s = mmse_stats();
  s.calls.fetch_add(1, std::memory_order_relaxed);
  (hop_gpu ? s.hops_gpu : s.hops_no_gpu).fetch_add(1, std::memory_order_relaxed);
  if (hop_nn) {
    s.hops_nn.fetch_add(1, std::memory_order_relaxed);
  }
  s.fallback_blocks.fetch_add(fallback_blocks, std::memory_order_relaxed);
  s.pre_stage_ns.fetch_add(static_cast<uint64_t>(pre_stage_us * 1e3), std::memory_order_relaxed);
  s.stage_ns.fetch_add(static_cast<uint64_t>(stage_us * 1e3), std::memory_order_relaxed);
  s.submit_ns.fetch_add(static_cast<uint64_t>(submit_us * 1e3), std::memory_order_relaxed);
  s.unpack_ns.fetch_add(static_cast<uint64_t>(unpack_us * 1e3), std::memory_order_relaxed);
  s.sigma2_us.fetch_add(static_cast<uint64_t>(sigma2_us), std::memory_order_relaxed);
  s.corr_us.fetch_add(static_cast<uint64_t>(corr_us), std::memory_order_relaxed);
  s.gpu_path_us.fetch_add(static_cast<uint64_t>(gpu_path_us), std::memory_order_relaxed);
  s.gpu_wait_us.fetch_add(static_cast<uint64_t>(gpu_wait_us), std::memory_order_relaxed);
  s.cpu_blocks_us.fetch_add(static_cast<uint64_t>(cpu_blocks_us), std::memory_order_relaxed);
  s.total_us.fetch_add(static_cast<uint64_t>(total_us), std::memory_order_relaxed);
  s.deferred_wait_us.fetch_add(static_cast<uint64_t>(deferred_wait_us), std::memory_order_relaxed);
  uint64_t prev = s.max_total_us.load(std::memory_order_relaxed);
  const auto cur = static_cast<uint64_t>(total_us);
  while (cur > prev && !s.max_total_us.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {
  }
}

} // namespace
#endif // OCUDU_CE_TIME

#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

using namespace ocudu;

namespace {

/// Real part of the exponential-PDP frequency correlation: 1 / (1 + (2 pi df tau)^2).
inline float rf_corr(float delta_f_hz, float tau_rms_s)
{
  const float x = TWOPI * delta_f_hz * tau_rms_s;
  return 1.0F / (1.0F + x * x);
}

/// Jakes time correlation: J0(2 pi f_d delta_t).
inline float rt_corr(float delta_t_s, float fd_hz)
{
  return static_cast<float>(::j0(static_cast<double>(TWOPI * fd_hz * delta_t_s)));
}

/// 4KB-aligned allocation (Metal zero-copy requires page alignment).
template <typename T>
T* alloc_aligned(std::size_t n)
{
  return new (std::align_val_t(4096)) T[n]();
}

/// \brief 4KB-aligned allocation that owns a whole number of pages.
///
/// Required by the buffers another engine consumes: the process-wide zero-copy mapping rounds the
/// length up to a whole page, so a buffer that only owns part of its last page would be mapped past
/// its end (see mmse_engine::reserve_shared_buffer()).
template <typename T>
T* alloc_aligned_pages(std::size_t n)
{
  constexpr std::size_t page = 4096;
  const std::size_t     bytes = ((n * sizeof(T) + page - 1) / page) * page;
  return new (std::align_val_t(4096)) T[bytes / sizeof(T)]();
}

template <typename T>
void free_aligned(T* p)
{
  if (p != nullptr) {
    ::operator delete[](p, std::align_val_t(4096));
  }
}

} // namespace

port_channel_estimator_metal_mmse_impl::port_channel_estimator_metal_mmse_impl(
    std::unique_ptr<interpolator>                        interp,
    std::unique_ptr<time_alignment_estimator>            ta_estimator_,
    std::shared_ptr<const channel_statistics_estimator>  stats_estimator_,
    unsigned                                             block_prb_,
    bool                                                 compensate_cfo_,
    bool                                                 use_matrix_engine_,
    bool                                                 force_cpu_path) :
  port_channel_estimator_average_impl(std::move(interp),
                                      std::move(ta_estimator_),
                                      port_channel_estimator_fd_smoothing_strategy::none,
                                      // Force the interpolate strategy so the LSE pilots are kept per
                                      // DM-RS symbol (the 2D estimator handles time by itself).
                                      port_channel_estimator_td_interpolation_strategy::interpolate,
                                      compensate_cfo_),
  use_matrix_engine(use_matrix_engine_),
  max_blocks(divide_ceil(MAX_NOF_PRBS, std::min(std::max(block_prb_, 1U), MAX_BLOCK_PRB))),
  stats_estimator(std::move(stats_estimator_)),
  block_prb(std::min(std::max(block_prb_, 1U), MAX_BLOCK_PRB))
{
  ocudu_assert(stats_estimator, "Invalid channel statistics estimator.");

  // Metal compute engine (K1/K2); the CPU reference math below is the automatic fallback
  // when the engine is unavailable (init failure / stale metallib). Forcing the whole
  // estimator onto the CPU path from the outside is the expert_phy knob:
  // --pusch_channel_estimator_algo cpu (force_cpu_path is the test-only hook used by the
  // head-to-head benchmark).
  engine       = std::make_unique<metal::mmse_engine>();
  engine_ready = !force_cpu_path && engine->init();
  gpu_a        = alloc_aligned<float>(static_cast<std::size_t>(MAX_LAYERS) * MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS);
  gpu_r_hp     = alloc_aligned<float>(static_cast<std::size_t>(MAX_LAYERS) * MAX_BLOCK_OUT * MAX_BLOCK_PILOTS);
  gpu_w        = alloc_aligned<float>(static_cast<std::size_t>(MAX_LAYERS) * MAX_BLOCK_OUT * MAX_BLOCK_PILOTS);
  gpu_y        = alloc_aligned<float>(static_cast<std::size_t>(MAX_LAYERS) * max_blocks * 2 * MAX_BLOCK_PILOTS);
  gpu_h        = alloc_aligned<float>(static_cast<std::size_t>(MAX_LAYERS) * max_blocks * 2 * MAX_BLOCK_OUT);

  // K3 (S-6a): the equalizer's per-symbol estimates. The masks are staged per hop, the destination
  // holds every layer of the hop - the merged batch keeps at most MAX_LAYERS / 2 of them (see the
  // merge gate), the split batches up to MAX_LAYERS.
  // The demodulator consumes these estimates (S-6b), so the device path is the default;
  // OCUDU_CE_CPU_CE=1 keeps both sides on the per-symbol host gather for A/B.
  device_ce_enabled = (std::getenv("OCUDU_CE_CPU_CE") == nullptr);
  gpu_ce    = alloc_aligned_pages<uint16_t>(static_cast<std::size_t>(MAX_LAYERS) * MAX_NOF_PRBS *
                                         NOF_SUBCARRIERS_PER_RB * MAX_NSYMB_PER_SLOT * 2);
  // K4 (S-6c-0): the device noise variance and its pilot inputs (2 floats per complex sample).
  gpu_nv        = alloc_aligned_pages<float>(1);
  gpu_pilots    = alloc_aligned<float>(2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * MAX_LAYERS *
                                    MAX_NOF_PILOTS_SYMBOL);
  // One CDM group per pair of layers (type-1 DM-RS), so at most MAX_LAYERS / 2 of them.
  static constexpr unsigned MAX_CDM_GROUPS = MAX_LAYERS / 2;
  gpu_rx_pilots = alloc_aligned<float>(2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * MAX_CDM_GROUPS *
                                       MAX_NOF_PILOTS_SYMBOL);
  gpu_epochs    = alloc_aligned<float>(MAX_NSYMB_PER_SLOT);

  // metal_nn_mmse flavor: compile the simdgroup_matrix 8x8 pipelines and stage the
  // quad-packed pilot matrix qy (zero-initialized: tail-quad columns of non-existent
  // blocks stay zero, which the apply kernel needs for NaN-free accumulation).
  matrix_ready = false;
  if (engine_ready && use_matrix_engine) {
    const std::size_t qy_quads = (static_cast<std::size_t>(max_blocks) + 3u) / 4u;
    gpu_qy = alloc_aligned<float>(static_cast<std::size_t>(MAX_LAYERS) * qy_quads * MAX_BLOCK_PILOTS * 8);
    matrix_ready = engine->init_matrix_pipelines();
    // INFO level (no env needed): confirms which GPU kernels the A/B flavor runs. The nn
    // path is taken on EVERY standard-block hop - nout/L that are not multiples of 8 are
    // zero-padded to ceil8 automatically (see the [mmse_time] pad= field); nn=0 only shows
    // up when this line reports UNAVAILABLE (stale metallib).
    logger.info("[mmse_ce] metal_nn_mmse: matrix pipelines {} - non-8-aligned dims are zero-padded, "
                "per-slot selection: [mmse_time] nn=",
                matrix_ready ? "READY (simdgroup 8x8)" : "UNAVAILABLE (falling back to legacy kernels)");
  }

  // Warm-up dispatch: Metal JIT-compiles the kernels and pays the first command-buffer
  // commit on first use (~3 ms on Apple Silicon, see PLAN.md 7.0.11/7.0.12). Running it
  // here, with the FULL staging-buffer capacities, moves that cost off the slot critical
  // path AND populates the zero-copy buffer cache with entries large enough for every
  // later call (the cache is keyed by pointer). Buffers are zero-initialized.
  if (engine_ready) {
    (void)engine->run_weights_only(
        gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, MAX_BLOCK_OUT, MAX_BLOCK_PILOTS, MAX_LAYERS, max_blocks);
  }
  // The device-side stages (K3/K4) read and write staging buffers whose size follows the
  // allocation, so reserve their zero-copy mappings at capacity here: without this, every hop that
  // needs more than the first allocation seen re-wraps a Metal buffer on the hot path.
  // K3's output (the estimates) and K4's output (the noise variance) are exported: the equalizer
  // binds them, so they are mapped in the process-wide cache every engine shares and the consuming
  // stage binds the same Metal buffer object these kernels wrote through.
  if (engine_ready) {
    (void)engine->reserve_shared_buffer(gpu_ce,
                                        static_cast<std::size_t>(MAX_LAYERS) * MAX_NOF_PRBS *
                                            NOF_SUBCARRIERS_PER_RB * MAX_NSYMB_PER_SLOT * 2 * sizeof(uint16_t));
    (void)engine->reserve_buffer(
        gpu_pilots,
        2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * MAX_LAYERS * MAX_NOF_PILOTS_SYMBOL * sizeof(float));
    (void)engine->reserve_buffer(
        gpu_rx_pilots,
        2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * (MAX_LAYERS / 2) * MAX_NOF_PILOTS_SYMBOL * sizeof(float));
    (void)engine->reserve_shared_buffer(gpu_nv, sizeof(float));
  }

  // The matrix kernels need their own warm-up (JIT + zero-copy cache entry sized
  // MAX_LAYERS * ceil(max_blocks/4) * 72 * 8 floats - larger than any later call).
  if (matrix_ready) {
    (void)engine->run_nn(
        gpu_a, gpu_r_hp, gpu_w, gpu_qy, gpu_h, MAX_BLOCK_OUT, MAX_BLOCK_PILOTS, MAX_LAYERS, max_blocks);
  }
}

port_channel_estimator_metal_mmse_impl::~port_channel_estimator_metal_mmse_impl()
{
  free_aligned(gpu_a);
  free_aligned(gpu_r_hp);
  free_aligned(gpu_w);
  free_aligned(gpu_y);
  free_aligned(gpu_qy);
  free_aligned(gpu_h);
  free_aligned(gpu_ce);
  free_aligned(gpu_nv);
  free_aligned(gpu_pilots);
  free_aligned(gpu_rx_pilots);
  free_aligned(gpu_epochs);
}

float port_channel_estimator_metal_mmse_impl::estimate_sigma2(const fd_td_estimation_stage_args& args)
{
  const unsigned nof_layers        = args.dmrs_patterns.size();
  const unsigned nof_symbol_pilots = args.nof_symbol_pilots;
  const unsigned nof_dmrs_symbols  = args.nof_dmrs_symbols;
  const auto&    hop_rb_mask =
      (args.hop == 0) ? args.dmrs_patterns.front().rb_mask : args.dmrs_patterns.front().rb_mask2;
  const unsigned nof_prb = hop_rb_mask.count();
  const unsigned stride  = configure_interpolator(args.dmrs_patterns.front().re_pattern).stride;

  const unsigned enlarged_size = nof_symbol_pilots + 2 * MAX_V_PILOTS;
  tmp_lse_enlarged.resize({.nof_subc = enlarged_size, .nof_symbols = nof_dmrs_symbols, .nof_slices = nof_layers});
  tmp_filtered_enlarged.resize({.nof_subc = enlarged_size, .nof_symbols = nof_dmrs_symbols, .nof_slices = nof_layers});
  modular_re_measurement<cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> tmp_lse(tmp_lse_enlarged);
  modular_re_measurement<cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> tmp_filtered(tmp_filtered_enlarged);
  tmp_lse.assign(tmp_lse_enlarged, MAX_V_PILOTS, nof_symbol_pilots);
  tmp_filtered.assign(tmp_filtered_enlarged, MAX_V_PILOTS, nof_symbol_pilots);

  // The caller has already applied the DM-RS to data scaling (1 / beta) to the LSE pilots, exactly
  // like the classical FD stage, so here they only need smoothing. estimate_noise() reconstructs the
  // received (unscaled) pilots from this buffer and beta, and returns the residual in the received
  // domain.
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned i_symbol = 0; i_symbol != nof_dmrs_symbols; ++i_symbol) {
      ocuduvec::copy(tmp_lse.get_symbol(i_symbol, i_layer), args.pilots_lse_view.get_symbol(i_symbol, i_layer));
      apply_fd_smoothing(tmp_filtered_enlarged.get_symbol(i_symbol, i_layer),
                         tmp_lse_enlarged.get_symbol(i_symbol, i_layer),
                         nof_prb,
                         stride,
                         port_channel_estimator_fd_smoothing_strategy::filter);
    }
  }

  // Noise variance from the existing classical estimator, averaged over the CDM layer pairs.
  float    sigma2  = 0.0F;
  unsigned n_pairs = 0;
  for (unsigned i_layer = 0; i_layer < nof_layers; i_layer += 2U) {
    const unsigned stop_layer = std::min(i_layer + 2U, nof_layers);
    const float    energy     = ocudu::estimate_noise(args.pilots,
                                                   args.rx_pilots,
                                                   tmp_filtered,
                                                   args.beta_scaling,
                                                   args.pattern_symbols,
                                                   args.cfo_hop,
                                                   args.symbol_start_epochs,
                                                   args.compensate_cfo_flag,
                                                   args.first_symbol,
                                                   args.last_symbol,
                                                   args.hop_offset,
                                                   i_layer,
                                                   stop_layer);
    sigma2 += energy / static_cast<float>(nof_symbol_pilots * nof_dmrs_symbols * (stop_layer - i_layer));
    ++n_pairs;
  }
  return n_pairs == 0 ? 0.0F : sigma2 / static_cast<float>(n_pairs);
}

metal::mmse_engine::corr_stage port_channel_estimator_metal_mmse_impl::correlation_stage(
    const channel_statistics&                     stats,
    const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
    unsigned                                       b_prb,
    span<const unsigned>                           dmrs_slot_symbols,
    unsigned                                       scs_khz,
    unsigned                                       sys_offset,
    unsigned&                                      nout,
    unsigned&                                      L,
    unsigned                                       a_stride,
    unsigned                                       r_stride)
{
  const unsigned nf  = b_prb * NOF_SUBCARRIERS_PER_RB;
  const unsigned npt = dmrs_slot_symbols.size();
  const unsigned npf = b_prb * re_pattern.count();
  L                  = npt * npf;
  nout               = nf * MAX_NSYMB_PER_SLOT;

  // Slots are addressed by the SLOT's strides, the block by its own L / nout. They differ when this
  // block is tucked into another geometry's slots (the merged edge block: slot stride L_std, block
  // order L_e). ocudu_assert rather than a silent clamp: a stride smaller than the block would have
  // the kernel write rows on top of each other.
  ocudu_assert((a_stride >= L) && (r_stride >= nout), "Slot strides must cover the block geometry.");

  metal::mmse_engine::corr_stage c{};
  c.a            = gpu_a + static_cast<std::size_t>(sys_offset) * a_stride * a_stride;
  c.r_hp         = gpu_r_hp + static_cast<std::size_t>(sys_offset) * r_stride * a_stride;
  c.a_l_stride   = a_stride;
  c.r_stride     = r_stride;
  c.a_sys_stride = a_stride * a_stride;
  c.r_sys_stride = r_stride * a_stride;
  c.l            = L;
  c.nf         = nf;
  c.npf        = npf;
  c.ncomb      = re_pattern.count();
  c.ts         = 1.0F / (static_cast<float>(scs_khz) * 1000.0F * MAX_NSYMB_PER_SLOT);
  c.scs_hz     = static_cast<float>(scs_khz) * 1000.0F;
  c.fd_hz      = stats.fd_hz;
  c.tau_rms_s  = stats.tau_rms_s;
  c.sigma2     = stats.sigma2;
  for (unsigned k = 0; k != npt; ++k) {
    c.dmrs_slots[k] = dmrs_slot_symbols[k];
  }
  {
    unsigned n = 0;
    for (unsigned pos = 0; (pos != NOF_SUBCARRIERS_PER_RB) && (n != c.ncomb); ++pos) {
      if (re_pattern.test(pos)) {
        c.pilot_re[n++] = pos;
      }
    }
  }
  return c;
}

bool port_channel_estimator_metal_mmse_impl::build_correlation_matrices_device(
    const channel_statistics&                     stats,
    const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
    unsigned                                       b_prb,
    unsigned                                       gb_start,
    span<const unsigned>                           dmrs_slot_symbols,
    unsigned                                       scs_khz,
    unsigned                                       sys_offset,
    unsigned                                       n_layers,
    unsigned&                                      nout,
    unsigned&                                      L)
{
  const unsigned nf  = b_prb * NOF_SUBCARRIERS_PER_RB;
  const unsigned npt = dmrs_slot_symbols.size();
  const unsigned npf = b_prb * re_pattern.count();
  L                  = npt * npf;
  nout               = nf * MAX_NSYMB_PER_SLOT;

  metal::mmse_engine::corr_stage c{};
  // The slots are addressed exactly as stage_engine_group() writes them, so a system built here is
  // the one K1 (or the weights kernels) reads.
  c.a                = gpu_a + static_cast<std::size_t>(sys_offset) * L * L;
  c.r_hp             = gpu_r_hp + static_cast<std::size_t>(sys_offset) * nout * L;
  // The kernel writes the PACKED L x L and nout x L regions, one system after another, exactly
  // where the host staging copies used to put them: the row stride it steps by is L, not the slot's
  // stride (the pad beyond L is the caller's, and the device build never touches it).
  c.a_l_stride       = L;
  c.r_stride         = nout;
  c.l                = L;
  c.nf               = nf;
  c.npf              = npf;
  c.ncomb            = re_pattern.count();
  c.ts               = 1.0F / (static_cast<float>(scs_khz) * 1000.0F * MAX_NSYMB_PER_SLOT);
  c.scs_hz           = static_cast<float>(scs_khz) * 1000.0F;
  c.fd_hz            = stats.fd_hz;
  c.tau_rms_s        = stats.tau_rms_s;
  c.sigma2           = stats.sigma2;
  for (unsigned k = 0; k != npt; ++k) {
    c.dmrs_slots[k] = dmrs_slot_symbols[k];
  }
  // Pilot positions within a PRB, ascending - the same walk the host's pilot_sc[] does.
  {
    unsigned n = 0;
    for (unsigned pos = 0; (pos != NOF_SUBCARRIERS_PER_RB) && (n != c.ncomb); ++pos) {
      if (re_pattern.test(pos)) {
        c.pilot_re[n++] = pos;
      }
    }
  }
  (void)gb_start;
  const bool ok = engine->build_correlation(c, n_layers);
  return ok;
}

void port_channel_estimator_metal_mmse_impl::build_correlation_matrices(
    const channel_statistics&                     stats,
    const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
    unsigned                                       n_prb,
    span<const unsigned>                           dmrs_slot_symbols,
    unsigned                                       scs_khz,
    span<float>                                    a_out,
    span<float>                                    r_hp_out,
    unsigned&                                      nout,
    unsigned&                                      L)
{
  const unsigned nf  = n_prb * NOF_SUBCARRIERS_PER_RB;
  const unsigned npt = dmrs_slot_symbols.size();
  const unsigned npf = n_prb * re_pattern.count();
  L                  = npt * npf;
  nout               = nf * MAX_NSYMB_PER_SLOT;

  // Pilot subcarrier positions within the block (symbol-major pilot ordering).
  static_vector<unsigned, MAX_BLOCK_PILOTS> pilot_sc;
  for (unsigned prb = 0; prb != n_prb; ++prb) {
    re_pattern.for_each(0, re_pattern.size(), [&](unsigned pos) {
      pilot_sc.push_back(prb * NOF_SUBCARRIERS_PER_RB + pos);
    });
  }

  const float scs_hz = static_cast<float>(scs_khz) * 1000.0F;
  const float ts     = 1.0F / (scs_hz * MAX_NSYMB_PER_SLOT);
  // Diagonal ridge: a singularity guard only. It is NOT what sets A's conditioning - sigma2 (the
  // noise-to-pilot-power ratio, ~1e-3) dominates it, and both are far below the smallest eigenvalue
  // the correlation model produces. That is why cond_2(A) reaches ~2e4 on a real capture, and why
  // float32 inversion of A is the accuracy limit of this estimator: measured, the host Gauss-Jordan
  // reaches 1.7e-1 element-wise relative error there and the device kernel 9.7e-1, while a float64
  // factorization reaches 4.6e-10. Raising this ridge buys accuracy back (at 1e-2 the device error
  // drops to 1.8e-2), but that is NOT a free accuracy budget: on 240 staged captures, raising it to
  // 1e-2 changed the published LLR in 221 of them (SINR moved only 0.01-0.31 dB, which is why the
  // knob looked harmless at first). It therefore changes what the receiver decides, and is not a
  // way to buy the device inversion back - see the plan.
  const float ridge  = 1e-6F;

  // --- A = R_pp + sigma2_rel I + ridge I = kron(R_t_pp, R_f_pp) + (sigma2_rel + ridge) I ---
  // R_pp is the correlation of unit-power pilots (its diagonal is one), while the pilots the
  // weights are applied to carry the received power. The diagonal loading must therefore be the
  // noise-to-pilot-power RATIO: with an absolute noise power the regularization follows the radio
  // gain, and away from the nominal level the weights are under-regularized and blow up (the
  // channel estimates and the reported noise variance explode) or over-smooth the channel.
  std::fill(a_out.begin(), a_out.begin() + L * L, 0.0F);
  for (unsigned t1 = 0; t1 != npt; ++t1) {
    for (unsigned t2 = 0; t2 != npt; ++t2) {
      const float rt = rt_corr(std::abs(static_cast<int>(dmrs_slot_symbols[t1]) -
                                        static_cast<int>(dmrs_slot_symbols[t2])) *
                                   ts,
                               stats.fd_hz);
      for (unsigned f1 = 0; f1 != npf; ++f1) {
        for (unsigned f2 = 0; f2 != npf; ++f2) {
          const float rf = rf_corr(std::abs(static_cast<int>(pilot_sc[f1]) - static_cast<int>(pilot_sc[f2])) * scs_hz,
                                   stats.tau_rms_s);
          a_out[(t1 * npf + f1) * L + (t2 * npf + f2)] = rt * rf;
        }
      }
    }
  }
  for (unsigned i = 0; i != L; ++i) {
    a_out[i * L + i] += stats.sigma2 + ridge;
  }

  // --- R_hp = kron(R_t_hp, R_f_hp) ---
  for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
    for (unsigned sc = 0; sc != nf; ++sc) {
      const unsigned o = sym * nf + sc;
      for (unsigned t2 = 0; t2 != npt; ++t2) {
        const float rt = rt_corr(std::abs(static_cast<int>(sym) - static_cast<int>(dmrs_slot_symbols[t2])) * ts,
                                 stats.fd_hz);
        for (unsigned f2 = 0; f2 != npf; ++f2) {
          const float rf = rf_corr(std::abs(static_cast<int>(sc) - static_cast<int>(pilot_sc[f2])) * scs_hz,
                                   stats.tau_rms_s);
          r_hp_out[o * L + (t2 * npf + f2)] = rt * rf;
        }
      }
    }
  }
}

bool port_channel_estimator_metal_mmse_impl::gauss_jordan_invert(span<float> a, unsigned n)
{
  // Row-major [A | I] with 2n columns; partial pivoting.
  const unsigned n2 = 2 * n;
  for (unsigned col = 0; col != n; ++col) {
    unsigned pivot = col;
    float    pv    = std::abs(a[col * n2 + col]);
    for (unsigned r = col + 1; r != n; ++r) {
      const float v = std::abs(a[r * n2 + col]);
      if (v > pv) {
        pv    = v;
        pivot = r;
      }
    }
    if (pv < 1e-12F) {
      return false;
    }
    if (pivot != col) {
      for (unsigned c = 0; c != n2; ++c) {
        std::swap(a[col * n2 + c], a[pivot * n2 + c]);
      }
    }
    const float inv = 1.0F / a[col * n2 + col];
    for (unsigned c = 0; c != n2; ++c) {
      a[col * n2 + c] *= inv;
    }
    for (unsigned r = 0; r != n; ++r) {
      if (r == col) {
        continue;
      }
      const float factor = a[r * n2 + col];
      if (factor == 0.0F) {
        continue;
      }
      for (unsigned c = 0; c != n2; ++c) {
        a[r * n2 + c] -= factor * a[col * n2 + c];
      }
    }
  }
  return true;
}

void port_channel_estimator_metal_mmse_impl::apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args)
{
  const unsigned nof_layers = args.dmrs_patterns.size();
  const auto&    hop_rb_mask =
      (args.hop == 0) ? args.dmrs_patterns.front().rb_mask : args.dmrs_patterns.front().rb_mask2;
  const unsigned nof_prb = hop_rb_mask.count();
  const unsigned scs_khz = scs_to_khz(args.scs);

  // DM-RS slot symbol indices of the current hop (ascending).
  static_vector<unsigned, MAX_NOF_DMRS_SYMBOLS> dmrs_sym;
  args.pattern_symbols.for_each(args.first_symbol, args.last_symbol, [&](unsigned s) { dmrs_sym.push_back(s); });
  const unsigned npt = dmrs_sym.size();

  // Per-phase timing (compile-time debug aid, ENABLE_CE_TIME=ON defines OCUDU_CE_TIME):
  // sigma2 / corr-build / GPU / CPU-blocks / finish, printed through the [mmse_time]
  // debug line. The measurements below are recorded unconditionally; only the reporting block at
  // the end of this function is compiled out when the probe is disabled.
#if defined(OCUDU_CE_TIME)
  using steady_clock = std::chrono::steady_clock;
  const auto t_begin = steady_clock::now();
#endif

  // Mean power of the received DM-RS pilots, measured BEFORE the DM-RS to data scaling below: the
  // classical noise estimator returns the residual in the received domain, so this is the reference
  // that turns sigma2 into the noise-to-signal ratio the unit-normalized correlation model needs.
  float pilots_power = 0.0F;
  {
    size_t nof_pilots = 0;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned i_symbol = 0; i_symbol != args.nof_dmrs_symbols; ++i_symbol) {
        span<const cf_t> sym = args.pilots_lse_view.get_symbol(i_symbol, i_layer);
        for (const cf_t& x : sym) {
          pilots_power += std::norm(x);
        }
        nof_pilots += sym.size();
      }
    }
    pilots_power = (nof_pilots == 0) ? 0.0F : pilots_power / static_cast<float>(nof_pilots);
  }

  // Mirror the classical FD stage: scale the least-squares pilots by 1 / beta so that the estimator
  // produces the DATA-domain channel, which is the domain the equalizer and the demapper expect.
  // Skipping this made every channel estimate 1 / beta too small whenever the PUSCH processor sets
  // a scaling other than one - which it always does in a real cell (0.708 for two CDM groups
  // without data, the configuration this cell runs) and never does in a lab test that leaves the
  // CDM group count at one.
  const float inv_beta = 1.0F / args.beta_scaling;
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned i_symbol = 0; i_symbol != args.nof_dmrs_symbols; ++i_symbol) {
      span<cf_t> sym = args.pilots_lse_view.get_symbol(i_symbol, i_layer);
      ocuduvec::sc_prod(sym, sym, inv_beta);
    }
  }

  // Classical noise variance (reuses the existing noise estimator).
  const float sigma2 = estimate_sigma2(args);
  const float sigma2_rel   = sigma2 / std::max(pilots_power, 1e-30F);
  // Rate-limited diagnostic (OCUDU_CE_DEBUG=1): an absolute sigma2 makes the MMSE weights - and
  // with them the channel estimates and the equalizer's noise variance that scales the soft bits -
  // follow the input level instead of the SNR. One line every 1000 hops.
  if (std::getenv("OCUDU_CE_DEBUG") != nullptr) {
    static std::atomic<uint32_t> debug_counter{0};
    if ((debug_counter.fetch_add(1, std::memory_order_relaxed) % 1000U) == 0U) {
      std::fprintf(stderr, "[ce_debug] pilots_power=%.6e sigma2=%.6e sigma2_rel=%.6e\n",
                   static_cast<double>(pilots_power),
                   static_cast<double>(sigma2),
                   static_cast<double>(sigma2_rel));
    }
  }
#if defined(OCUDU_CE_TIME)
  const auto t_sigma2 = steady_clock::now();
#endif

  // Estimated full grid and classical frequency-response buffers.
  grid_est.resize(nof_layers * MAX_NSYMB_PER_SLOT, nof_prb * NOF_SUBCARRIERS_PER_RB);
  args.freq_response.resize(
      {.nof_subc = static_cast<unsigned>(nof_prb * NOF_SUBCARRIERS_PER_RB),
       .nof_symbols = args.nof_dmrs_symbols,
       .nof_slices = nof_layers});

  const unsigned n_std_blocks = nof_prb / block_prb;
  const unsigned rem_prb      = nof_prb - n_std_blocks * block_prb;

  // Assemble the statistics input (v1: the same statistics for every layer; per-layer
  // statistics arrive with the v2 estimation-backed provider).
  channel_statistics_input stats_in{};
  stats_in.sigma2            = sigma2_rel;
  stats_in.nof_symbol_pilots = args.nof_symbol_pilots;
  stats_in.nof_dmrs_symbols  = args.nof_dmrs_symbols;
  stats_in.scs               = args.scs;
  stats_in.dmrs_symbol_0     = npt > 0 ? dmrs_sym[0] : 0;
  stats_in.dmrs_symbol_1     = npt > 1 ? dmrs_sym[1] : 0;
  span<cf_t> pilots_span(stats_pilots.data(), args.nof_dmrs_symbols * args.nof_symbol_pilots);
  for (unsigned i_symbol = 0; i_symbol != args.nof_dmrs_symbols; ++i_symbol) {
    ocuduvec::copy(pilots_span.subspan(i_symbol * args.nof_symbol_pilots, args.nof_symbol_pilots),
                   args.pilots_lse_view.get_symbol(i_symbol, 0));
  }
  stats_in.pilots_lse = pilots_span;
  // \note \c stats_in.sigma2 carries the noise-to-pilot-power ratio, not an absolute power: see
  // build_correlation_matrices().
  const channel_statistics stats = stats_estimator->estimate(stats_in);

  // Weight matrices of the standard blocks (shared by all layers in v1). They are built ON THE
  // DEVICE when the engine offers the correlation stage: the product is analytic, so the host only
  // hands the geometry over and the slots are filled where the inversion and the weights read them
  // (K0-d). The host arrays remain the fallback for a metallib without the kernel.
  unsigned L_std    = 0;
  unsigned nout_std = 0;
  // Geometry of the standard blocks, from the host arrays: it is needed either way, and the device
  // build below only replaces the VALUES (see the gate after merge_tail).
  if (n_std_blocks != 0) {
    build_correlation_matrices(stats,
                               args.dmrs_patterns.front().re_pattern,
                               block_prb,
                               span<const unsigned>(dmrs_sym.begin(), npt),
                               scs_khz,
                               span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS),
                               span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS),
                               nout_std,
                               L_std);
  }
  // Whether the standard blocks' slots may be filled by the device instead. It is decided after
  // merge_tail: a merged batch carries the TAIL block as an extra system whose geometry differs, and
  // the device build only knows the standard one (see below).
  bool std_slots_filled = false;
#if defined(OCUDU_CE_TIME)
  const auto t_corr_std = steady_clock::now();
#endif
#if defined(OCUDU_CE_TIME)
  // Time spent copying the precomputed coefficient matrices and pilot vectors into the engine slots: this is what a
  // device-side build of the correlation matrices (the K0-d step of the fused-lane work) removes.
  double stage_us_local  = 0.0;
  double submit_us_local = 0.0;
  double unpack_us_local = 0.0;
#endif

  // ---- Engine (GPU) stage ---------------------------------------------------------------
  // metal_nn_mmse: when the flavor is enabled and the simdgroup 8x8 pipelines are compiled,
  // the matrix kernels ALWAYS process the hop; metal_mmse: the legacy kernels do. Both engines
  // now cover EVERY block of the hop - the standard block_prb-wide batch plus the tail/edge
  // block (a whole hop narrower than block_prb runs as its own single-block batch) - so with
  // the engine ready there is no CPU fallback per hop. Matrix dims that are not multiples of 8
  // are zero-padded to ceil8 in the staging buffers (Lp/Np row strides); the kernels truncate
  // the outputs back to the real geometry. nn=0 therefore only means the matrix engine itself
  // is unavailable (stale metallib): the legacy kernels (metal_mmse) or the CPU loop
  // then take over.
  // S-5c: the tail block no longer costs a second engine call - it is merged into the standard
  // batch as an extra padded system (see merge_tail below), so a hop is one command buffer.
  const bool matrix_on = engine_ready && use_matrix_engine && matrix_ready;
  // Deferring the batch is what lets the rest of the receiving chain run while the GPU works on it
  // (see port_channel_estimator::submit()); the matrix flavor and the CPU-inversion knob keep
  // completing their batches here, so a stage that cannot defer reports stage_pending = false.
  const bool defer = !matrix_on;
  nof_pending_unpacks = 0;
  stage_pending       = false;
  // Largest tail block order (L = pilots per block) the CPU reference path is known to be fast for
  // (a 36x36 Gauss-Jordan is ~23k FLOPs). Only consulted by the OCUDU_CE_TAIL_CPU A/B knob.
  static constexpr unsigned MAX_CPU_TAIL_ORDER = 36;
  bool       hop_gpu   = false; // engine processed this hop (any block)
  bool       hop_nn    = false; // the simdgroup 8x8 (matrix) kernels were the ones used
  unsigned   hop_pad   = 0;     // ceil8(L) - L of the last matrix batch (A/B pad overhead)
  // Written on the data path, read only by the timing report below (hence maybe_unused).
  [[maybe_unused]] unsigned tail_L = 0; // block L of the tail batch (log aid for std-less hops)
  bool       std_blocks_ok = true; // standard-block engine batch succeeded (CPU fallback otherwise)
  bool       tail_ok       = true; // tail/edge-block engine batch succeeded
  if (engine_ready) {
    // Tail-block A/B knob (research only, not a supported configuration): OCUDU_CE_TAIL_CPU=1
    // computes the tail with the CPU reference math of the fallback loop below. It also disables
    // the merged batch below, which would otherwise stage the tail.
    //
    // The CPU block path inverts its A with a serial O(L^3) Gauss-Jordan, so the knob is honoured
    // only up to the order that path is fast for (L<=36; a large block_prb would make the tail
    // milliseconds there) - above it, the engine runs the tail regardless.
    const unsigned tail_L_est  = rem_prb * 6U * npt;
    const bool     tail_on_cpu = (rem_prb != 0) && (tail_L_est <= MAX_CPU_TAIL_ORDER) &&
                                 (std::getenv("OCUDU_CE_TAIL_CPU") != nullptr);
    // Merged batch (S-5c): the standard blocks AND the tail block in ONE engine call, i.e. one
    // command buffer and one wait per hop instead of two. The tail rides as an extra SYSTEM of the
    // batch, not as an extra block: the engine takes one A / R_hp per system and one block count
    // per batch, so the tail's narrower geometry is padded to the standard one - A becomes
    // blockdiag(A_e, I) and R_hp becomes [R_hp_e | 0] - which leaves W = [R_hp_e . A_e^-1 | 0] and
    // h = W . y the very same computation on the tail's own entries (the pad columns of W are
    // exactly zero). It therefore estimates what the split path estimates, at the cost of a few us
    // of extra GPU work (each tail system carries n_std_blocks block slots, of which one is real)
    // against the ~100 us of host round trip it removes.
    const bool merge_tail = (rem_prb != 0) && (n_std_blocks != 0) && !matrix_on && !tail_on_cpu &&
                            (2 * nof_layers <= MAX_LAYERS) && (std::getenv("OCUDU_CE_SPLIT_TAIL") == nullptr);
    // The device build fills the slots of ONE geometry, and a merged batch holds TWO (the standard
    // blocks in the layer systems and the tail block in the extra ones). So it is used only when the
    // standard geometry is the whole batch: otherwise the tail system would keep whatever the slot
    // held before, and the weights would be built from a stale matrix. With no tail, or with the
    // split form (two batches), the standard batch is the single geometry the kernel describes.
    //
    // ON by default since the equivalence defect was found and fixed: the host batch stages A^-1
    // itself, so K1 must NOT run on it, and run_engine_blocks() now keys that decision on this
    // stage's presence instead of on the order (see the gpu_invert note there). OCUDU_CE_CORR_DEV=0
    // keeps the host build for A/B.
    static const bool device_corr_enabled = []() {
      const char* env = std::getenv("OCUDU_CE_CORR_DEV");
      return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
    }();
    // The matrices are built by the DEVICE (build_correlation(), below) and then inverted by the
    // HOST in the slots they were written to, so no order limit applies here: the correlation kernels
    // take whatever geometry the estimator hands them, and the host Gauss-Jordan has no limit either,
    // which is what lets the OTA geometry (order 72) use this path. What the device build cannot do
    // is a MERGED batch, because a merged batch holds two different geometries in one slot array.
    std_slots_filled = device_corr_enabled && !merge_tail && (n_std_blocks != 0);
    // K3 (S-6a): the equalizer's per-symbol estimates, built on the GPU inside the engine call. The
    // destination can only be filled by a call that covers the WHOLE allocation with the legacy
    // kernels - the merged batch, or a hop whose single batch is everything - otherwise the blocks
    // the call did not compute would keep stale entries. The masks are the demodulator's data-RE
    // layout, so a consumer must check the RE count before using them.
    gpu_ce_ready          = false;
    unsigned nof_re_total = 0;
    if (device_ce_enabled && (nof_prb != 0) && !matrix_on) {
      nof_re_total = stage_re_masks(args, nof_prb, hop_rb_mask.find_lowest());
    }
    // The DC subcarrier carries no data: the equalizer erases that resource element, so the
    // device buffer must hold a zero there (K3 writes it - the host path zeroes the value it
    // gathers instead).
    unsigned dc_sc = ~0u;
    if (args.dc_position.has_value()) {
      const unsigned first_sc = hop_rb_mask.find_lowest() * NOF_SUBCARRIERS_PER_RB;
      if ((*args.dc_position >= first_sc) &&
          (*args.dc_position < first_sc + nof_prb * NOF_SUBCARRIERS_PER_RB)) {
        dc_sc = *args.dc_position - first_sc;
      }
    }
    // K4 (S-6c-0): the equalizer's noise variance is reduced on the device out of the same h, from
    // the estimates at the pilot positions and the transmitted/received pilots. The latter two are
    // inputs the host already holds, copied into the staging buffers in the [npt][slices][npf]
    // layout the kernel indexes by.
    gpu_nv_ready          = false;
    unsigned nof_cdm_groups = 0;
    if (nof_re_total != 0) {
      const unsigned npf = args.nof_symbol_pilots;
      nof_cdm_groups     = args.rx_pilots.size().nof_slices;
      if ((npf != 0) && (nof_cdm_groups != 0) && (npt <= MAX_DMRS_SYMBOLS)) {
        for (unsigned i_dmrs = 0; i_dmrs != npt; ++i_dmrs) {
          for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
            span<const cf_t> src = args.pilots.get_symbol(args.hop_offset + i_dmrs, i_layer);
            float* dst = gpu_pilots + ((static_cast<std::size_t>(i_dmrs) * nof_layers + i_layer) * npf) * 2;
            for (unsigned j = 0; j != npf; ++j) {
              dst[2 * j]     = src[j].real();
              dst[2 * j + 1] = src[j].imag();
            }
          }
          for (unsigned i_group = 0; i_group != nof_cdm_groups; ++i_group) {
            span<const cf_t> src = args.rx_pilots.get_symbol(i_dmrs, i_group);
            float* dst = gpu_rx_pilots + ((static_cast<std::size_t>(i_dmrs) * nof_cdm_groups + i_group) * npf) * 2;
            for (unsigned j = 0; j != npf; ++j) {
              dst[2 * j]     = src[j].real();
              dst[2 * j + 1] = src[j].imag();
            }
          }
        }
        // Symbol start times (the CFO rotation of the reduction) and the comb geometry.
        for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
          gpu_epochs[sym] = (sym < args.symbol_start_epochs.size()) ? args.symbol_start_epochs[sym] : 0.0F;
        }
      } else {
        nof_cdm_groups = 0;
      }
    }

    metal::mmse_engine::reformat_stage reformat{};
    reformat.dst         = gpu_ce;
    reformat.offsets     = re_offsets.data();
    reformat.nof_symbols = MAX_NSYMB_PER_SLOT;
    reformat.drpp          = gpu_ce_drpp;
    reformat.drpp_dmrs     = gpu_ce_drpp_dmrs;
    reformat.dmrs_re_bits  = gpu_ce_dmrs_re_bits;
    reformat.dmrs_sym_bits = gpu_ce_dmrs_sym_bits;

    // K4 is attached only when the whole hop is covered (same rule as K3) and the pilot inputs were
    // staged: it reads the estimates at the pilot positions out of the same h.
    if ((nof_cdm_groups != 0) && (nof_re_total != 0)) {
      reformat.noise.nv                  = gpu_nv;
      reformat.noise.pilots              = gpu_pilots;
      reformat.noise.rx_pilots           = gpu_rx_pilots;
      reformat.noise.symbol_start_epochs = gpu_epochs;
      reformat.noise.npt                 = npt;
      reformat.noise.nof_cdm_groups      = nof_cdm_groups;
      reformat.noise.npf                 = args.nof_symbol_pilots;
      reformat.noise.nof_prb             = nof_prb;
      reformat.noise.comb_size           = static_cast<unsigned>(__builtin_popcount(gpu_ce_dmrs_re_bits));
      reformat.noise.dmrs_re_bits        = gpu_ce_dmrs_re_bits;
      reformat.noise.beta                = args.beta_scaling;
      reformat.noise.compensate_cfo      = args.compensate_cfo_flag;
      // The same normalization and SINR ceiling the host applies to the variance it reports.
      reformat.noise.nof_dmrs_pilots     = args.nof_symbol_pilots * npt;
      reformat.noise.nof_cdm             = divide_ceil(nof_layers, 2);
      reformat.noise.min_snr_power       = convert_dB_to_power(MAX_SINR_DB);
      if (args.cfo_hop.has_value() && args.compensate_cfo_flag) {
        reformat.noise.cfo = *args.cfo_hop;
      }
      for (unsigned i_dmrs = 0; i_dmrs != npt; ++i_dmrs) {
        reformat.noise.dmrs_slots[i_dmrs] = dmrs_sym[i_dmrs];
      }
      // The reduction needs the comb geometry, which stage_re_masks() has just published.
      if (reformat.noise.comb_size == 0) {
        reformat.noise.nv = nullptr;
      }
    }
    reformat.total_re       = nof_re_total;
    reformat.dc_sc          = dc_sc;
    // Standard blocks cover subcarriers [0, nf_std * nof_blocks) of the batch; the edge block, when
    // the batch carries one, sits in the systems [sys_tail, ...) at block 0.
    const auto reformat_for = [&](unsigned nf_std, unsigned nf_tail, unsigned sys_tail) {
      reformat.nf_std   = nf_std;
      reformat.nf_tail  = nf_tail;
      reformat.sys_tail = sys_tail;
      reformat.has_tail = (nf_tail != 0);
      reformat.nof_layers = nof_layers;
      return (nof_re_total != 0) ? &reformat : nullptr;
    };
    if (merge_tail) {
      // Both groups share the standard block's slot geometry.
      const engine_strides st{L_std, nout_std, n_std_blocks};
      // K1 inverts the padded (L_std x L_std) systems, so the kernel's order limit applies to the
      // padded order, not to the tail's.
      // 36, not the kernel's own 54: at order 54 the inversion is latency-bound (measured this
      // round: gpu_wait 85 -> 555us on a 14 PRB hop, i.e. ~470us MORE than the ~10us of host CPU
      // Gauss-Jordan it replaces), and the K1 header says why - the elimination walks 72 pivot
      // phases, two barriered steps each. Raising the limit is therefore gated on making K1 fast
      // (its S-5a blocked/simdgroup_matrix form), and only then can the device correlation ride the
      // same command buffer as the inversion.
      static constexpr unsigned MAX_GPU_INVERT_ORDER = 36;
      const bool gpu_invert = (std::getenv("OCUDU_CE_CPU_INVERT") == nullptr) && (L_std <= MAX_GPU_INVERT_ORDER);
      // A batch of the previous hop may still be outstanding: it reads the very staging slots this hop is
      // about to overwrite (the split-tail path runs two batches per hop, and a deferred hop keeps its last
      // batch alive until its consumer collects it), so complete it before touching them. Only ever the last
      // batch of a hop stays outstanding, which is what the caller's completion then unpacks.
      if (nof_pending_unpacks != 0) {
        (void)complete_fd_td_estimation_stage();
      }
      // 1) Stage the standard group while w_r_pp / w_r_hp still hold the standard matrices.
#if defined(OCUDU_CE_TIME)
      const auto t_stage_begin = steady_clock::now();
#endif
      if (std_slots_filled) {
        // K0-d: the DEVICE builds A and R_hp of the standard group (the merged batch's tail system
        // keeps whatever the tail staging below puts in it), and the host then inverts what the
        // device wrote. The device build cannot describe both geometries of a merged batch, so this
        // is the standard group's slots only - exactly what a non-merged hop gets from
        // run_engine_blocks() with device_stats.
        const std::optional<metal::mmse_engine::corr_stage> merged_corr = build_slots_on_device(
            stats,
            args.dmrs_patterns.front().re_pattern,
            block_prb,
            span<const unsigned>(dmrs_sym.begin(), npt),
            scs_khz,
            0,
            nof_layers,
            st.L,
            nout_std,
            L_std);
        // In the device-inversion experiment the merged batch cannot ride one command buffer (two
        // geometries), so the descriptor is dropped and the host staging below fills the slots.
        (void)merged_corr;
      } else {
        stage_engine_group(args,
                           0,
                           n_std_blocks,
                           block_prb,
                           npt,
                           nout_std,
                           L_std,
                           0,
                           st,
                           matrix_on,
                           gpu_invert,
                           false);
      }
#if defined(OCUDU_CE_TIME)
      stage_us_local += std::chrono::duration<double, std::micro>(steady_clock::now() - t_stage_begin).count();
#endif
      // 2) Only NOW build the tail matrices: they overwrite w_r_pp / w_r_hp, so the reverse order
      //    silently stages the tail's matrices for the standard blocks.
      unsigned nout_e = 0;
      unsigned L_e    = 0;
      build_correlation_matrices(stats,
                                 args.dmrs_patterns.front().re_pattern,
                                 rem_prb,
                                 span<const unsigned>(dmrs_sym.begin(), npt),
                                 scs_khz,
                                 span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS),
                                 span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS),
                                 nout_e,
                                 L_e);
      tail_L = L_e;
      // 3) The tail systems carry n_std_blocks block slots but only block 0 is real: clear the
      //    group first so the pad blocks hold zeros instead of a previous hop's pilots.
      std::memset(gpu_y + static_cast<std::size_t>(nof_layers) * n_std_blocks * 2 * L_std,
                  0,
                  static_cast<std::size_t>(nof_layers) * n_std_blocks * 2 * L_std * sizeof(float));
      stage_engine_group(
          args, n_std_blocks * block_prb, 1, rem_prb, npt, nout_e, L_e, nof_layers, st, matrix_on, gpu_invert);
      // 4) ONE engine call over both groups, then unpack both. The call is submitted without
      //    waiting when the kernels allow it, so the unpack moves to the completion of the stage
      //    (see complete_fd_td_estimation_stage()).
      // Both inversion flavors defer now: the weights-only pipeline (block order above the inversion kernel's
      // limit, which is the OTA geometry) used to be synchronous, and waiting for its batch inside the stage
      // cost ~150-230us per hop of pure host time.
      // NOTE: merged_defer has to be PASSED here. engine_run()'s defer argument used to have a
      // defaulted false, so this call - the merged standard+tail path, which is what every wide hop
      // (the air interface's 13 PRB among them) takes - was submitted synchronously while the unpack
      // right below already used merged_defer: the wait showed up as submit=~280us per hop on the
      // air, and every deferred-batch probe (cpl_wait, defer_wait) stayed at zero for it.
      const bool merged_defer = defer;
#if defined(OCUDU_CE_TIME)
      const auto t_submit_begin = steady_clock::now();
#endif
      const bool merged_ok = engine_run(nullptr,
                                        nout_std,
                                        L_std,
                                        2 * nof_layers,
                                        n_std_blocks,
                                        false,
                                        gpu_invert,
                                        reformat_for(block_prb * NOF_SUBCARRIERS_PER_RB,
                                                     rem_prb * NOF_SUBCARRIERS_PER_RB,
                                                     nof_layers),
                                        merged_defer);
#if defined(OCUDU_CE_TIME)
      submit_us_local += std::chrono::duration<double, std::micro>(steady_clock::now() - t_submit_begin).count();
#endif
      std_blocks_ok        = merged_ok;
      tail_ok              = merged_ok;
      hop_gpu              = merged_ok;
      hop_nn               = false;
      gpu_ce_ready         = merged_ok && (nof_re_total != 0);
      gpu_ce_layers        = nof_layers;
      gpu_ce_total_re      = nof_re_total;
      gpu_nv_ready         = gpu_ce_ready && (reformat.noise.nv != nullptr);
      if (merged_ok) {
#if defined(OCUDU_CE_TIME)
        const auto t_unpack_begin = steady_clock::now();
#endif
        if (merged_defer) {
          defer_unpack(0, n_std_blocks, block_prb, nout_std, nof_layers, 0, st);
          defer_unpack(n_std_blocks * block_prb, 1, rem_prb, nout_e, nof_layers, nof_layers, st);
        } else {
          unpack_engine_group(0, n_std_blocks, block_prb, nout_std, nof_layers, 0, st);
          unpack_engine_group(n_std_blocks * block_prb, 1, rem_prb, nout_e, nof_layers, nof_layers, st);
        }
#if defined(OCUDU_CE_TIME)
        unpack_us_local += std::chrono::duration<double, std::micro>(steady_clock::now() - t_unpack_begin).count();
#endif
      }
    } else {
      if (n_std_blocks != 0) {
        // Standard blocks: correlation matrices already built above (w_r_pp/w_r_hp, L_std/nout_std).
        // A hop without an edge block (or with its tail on the CPU) is covered by this single
        // batch, so K3 can be attached to it.
        const bool covers_hop = (rem_prb == 0);
        // K0-d: the correlation matrices of this group were built by the DEVICE before this call
        // (see the standard-group branch of the merged path above, and the device build in
        // run_engine_blocks() for a hop narrower than the block size), and the host then inverted
        // them in place. So this call carries no correlation stage: the slots already hold A^-1 and
        // the weights read exactly that.
        std_blocks_ok         = run_engine_blocks(args,
                                         0,
                                         n_std_blocks,
                                         block_prb,
                                         nout_std,
                                         L_std,
                                         npt,
                                         matrix_on,
                                         covers_hop ? reformat_for(block_prb * NOF_SUBCARRIERS_PER_RB, 0, 0)
                                                    : nullptr,
                                         defer,
                                         std_slots_filled ? &stats : nullptr);
        hop_gpu               = std_blocks_ok;
        hop_nn                = std_blocks_ok && matrix_on;
        hop_pad               = (std_blocks_ok && matrix_on) ? static_cast<unsigned>(((L_std + 7u) & ~7u) - L_std) : 0;
        gpu_ce_ready          = std_blocks_ok && covers_hop && (nof_re_total != 0);
        gpu_ce_layers         = nof_layers;
        gpu_ce_total_re       = nof_re_total;
        gpu_nv_ready          = gpu_ce_ready && (reformat.noise.nv != nullptr);
       }
      if (rem_prb != 0) {
        // Tail/edge block - and when nof_prb < block_prb this is the WHOLE hop (single block).
        //
        // The tail block runs on the ENGINE, like every other block of the hop: PHY compute
        // belongs to the Metal path as a whole. The second commit+wait the chain used to pay for
        // it is what the merged batch above removes; OCUDU_CE_SPLIT_TAIL=1 keeps the split form
        // for the A/B of that merge.
        if (!tail_on_cpu) {
          // K0-d: the edge/tail block is a batch of ONE geometry, so the device can build its
          // matrices like the standard group's. Its slots start at nof_layers (the standard group
          // owns [0, nof_layers) in the split form), which is why run_engine_blocks() takes
          // sys_offset.
          //
          // NOTE: this is the SPLIT form only (OCUDU_CE_SPLIT_TAIL, or a whole hop narrower than a
          // standard block). Hops with a remainder take the MERGED branch instead, where the tail
          // rides as extra systems sharing the standard slots - a different geometry in the same
          // buffer, which one correlation build cannot describe. That is why the device build
          // covered 37.5% of the hops on the air (device_corr_builds=26761 of 71384 in the b22
          // leg): the rest had a remainder and merged.
          const bool tail_slots_on_device = std_slots_filled;
          unsigned nout_e = 0;
          unsigned L_e    = 0;
          build_correlation_matrices(stats,
                                     args.dmrs_patterns.front().re_pattern,
                                     rem_prb,
                                     span<const unsigned>(dmrs_sym.begin(), npt),
                                     scs_khz,
                                     span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS),
                                     span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS),
                                     nout_e,
                                     L_e);
          tail_L  = L_e;
          // A hop narrower than block_prb is this single narrow-block batch, so it covers the whole
          // allocation too (see the standard-block batch above).
          const bool covers_hop = (n_std_blocks == 0);
          tail_ok               = run_engine_blocks(args,
                                      n_std_blocks * block_prb,
                                      1,
                                      rem_prb,
                                      nout_e,
                                      L_e,
                                      npt,
                                      matrix_on,
                                      covers_hop ? reformat_for(rem_prb * NOF_SUBCARRIERS_PER_RB, 0, 0) : nullptr,
                                      defer,
                                      tail_slots_on_device ? &stats : nullptr,
                                      nof_layers);
          hop_gpu               = hop_gpu || tail_ok;
          hop_nn                = hop_nn || (tail_ok && matrix_on);
          hop_pad               = (tail_ok && matrix_on) ? static_cast<unsigned>(((L_e + 7u) & ~7u) - L_e) : hop_pad;
          if (covers_hop) {
            gpu_ce_ready    = tail_ok && (nof_re_total != 0);
            gpu_ce_layers   = nof_layers;
            gpu_ce_total_re = nof_re_total;
            gpu_nv_ready    = gpu_ce_ready && (reformat.noise.nv != nullptr);
          }
        } else {
          // Route the block to the CPU fallback loop below. tail_ok MUST be cleared: leaving it
          // true while skipping the engine batch makes block_gpu_done() report the block as done
          // and keeps the stale grid content there (it cost 7 dB of NMSE while chasing S-4d).
          tail_ok = false;
        }
      }
    }
    // A/B observability: whether the last stage engaged the matrix kernels (nn=1) - or the
    // legacy kernels (nn=0, only when the matrix pipelines are unavailable/stale).
    last_stage_nn     = matrix_on;
    last_stage_merged = merge_tail;
    last_estimate_hopping = (args.hop != 0);
    // The batches have been submitted: if any of them was not waited for, the stage is pending and
    // complete_fd_td_estimation_stage() must unpack it before its results are read.
    stage_pending = (nof_pending_unpacks != 0);
#if defined(OCUDU_CE_TIME)
    if (gpu_ce_ready) {
      mmse_stats_device_hop();
    }
#endif
  } else {
    // Engine unavailable (init failure / stale metallib): the CPU loop below handles the
    // whole hop (standard blocks included).
    last_stage_nn     = false;
    last_stage_merged = false;
  }

#if defined(OCUDU_CE_TIME)
  const auto t_gpu_end = steady_clock::now();
#endif

  // CPU reference path: blocks whose engine batch failed (or the whole hop when the engine is
  // unavailable). The per-batch fallback is the S-1 audit fix: an engine failure must never
  // leave stale estimates in the grid while hop_gpu=1 is reported.
  const unsigned n_blocks = n_std_blocks + (rem_prb == 0 ? 0U : 1U);
  const auto     block_gpu_done = [&](unsigned b) {
    if (!engine_ready) {
      return false;
    }
    return (b < n_std_blocks) ? std_blocks_ok : tail_ok;
  };
  // Written on the data path, read only by the timing report below (hence maybe_unused).
  [[maybe_unused]] unsigned cpu_fallback_blocks = 0;

  for (unsigned b = 0; b != n_blocks; ++b) {
    if (block_gpu_done(b)) {
      continue;
    }
    ++cpu_fallback_blocks;
    const unsigned b_prb       = (b < n_std_blocks) ? block_prb : rem_prb;
    const unsigned b_start_prb = (b < n_std_blocks) ? b * block_prb : n_std_blocks * block_prb;
    unsigned       nout        = 0;
    unsigned       L           = 0;
    build_correlation_matrices(stats,
                               args.dmrs_patterns.front().re_pattern,
                               b_prb,
                               span<const unsigned>(dmrs_sym.begin(), npt),
                               scs_khz,
                               span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS),
                               span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS),
                               nout,
                               L);
    // A^-1 via CPU Gauss-Jordan.
    {
      std::array<float, 2 * MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS> gj;
      std::fill(gj.begin(), gj.end(), 0.0F);
      for (unsigned r = 0; r != L; ++r) {
        for (unsigned c = 0; c != L; ++c) {
          gj[r * 2 * L + c] = w_r_pp[r * L + c];
        }
        gj[r * 2 * L + L + r] = 1.0F;
      }
      const bool ok = gauss_jordan_invert(span<float>(gj.data(), 2 * L * L), L);
      for (unsigned r = 0; r != L; ++r) {
        for (unsigned c = 0; c != L; ++c) {
          w_a_inv[r * L + c] = ok ? gj[r * 2 * L + L + c] : ((r == c) ? 1.0F : 0.0F);
        }
      }
    }
    // W = R_hp . A^-1.
    for (unsigned o = 0; o != nout; ++o) {
      for (unsigned col = 0; col != L; ++col) {
        float acc = 0.0F;
        for (unsigned k = 0; k != L; ++k) {
          acc += w_r_hp[o * L + k] * w_a_inv[k * L + col];
        }
        w_mat[o * L + col] = acc;
      }
    }

    const unsigned b_npf = b_prb * args.dmrs_patterns.front().re_pattern.count();
    const unsigned b_nf  = b_prb * NOF_SUBCARRIERS_PER_RB;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      // Pack the block pilot vector (symbol-major, real/imag interleaved).
      for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
        span<const cf_t> src =
            args.pilots_lse_view.get_symbol(i_symbol, i_layer).subspan(b_start_prb * 6, b_prb * 6);
        for (unsigned j = 0; j != b_npf; ++j) {
          y_block[2 * (i_symbol * b_npf + j)]     = src[j].real();
          y_block[2 * (i_symbol * b_npf + j) + 1] = src[j].imag();
        }
      }
      // h = W . y (real weights, complex vector): two real matrix-vector products.
      std::fill(h_block.begin(), h_block.begin() + 2 * nout, 0.0F);
      for (unsigned o = 0; o != nout; ++o) {
        float acc_re = 0.0F;
        float acc_im = 0.0F;
        for (unsigned j = 0; j != L; ++j) {
          const float wj = w_mat[o * L + j];
          acc_re += wj * y_block[2 * j];
          acc_im += wj * y_block[2 * j + 1];
        }
        h_block[2 * o]     = acc_re;
        h_block[2 * o + 1] = acc_im;
      }
      // Write the block into the full grid (symbol-major within the block).
      for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
        span<cf_t> dst =
            grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + sym).subspan(b_start_prb * 12, b_nf);
        for (unsigned sc = 0; sc != b_nf; ++sc) {
          dst[sc] = {h_block[2 * (sym * b_nf + sc)], h_block[2 * (sym * b_nf + sc) + 1]};
        }
      }
    }
  }

#if defined(OCUDU_CE_TIME)
  const auto t_cpu_end = steady_clock::now();
#endif

  // Fill the pilot-derived buffers (the estimated pilot REs for RSrp / noise / TA, and the classical
  // frequency response) out of the estimated grid. The grid only holds the estimates once the batch
  // that produced them has completed, so a deferred stage records what to fill and lets its
  // completion do it.
  if (stage_pending) {
    deferred_fill.valid      = true;
    deferred_fill.nof_prb    = nof_prb;
    deferred_fill.npt        = npt;
    deferred_fill.nof_layers = nof_layers;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      deferred_fill.re_pattern[i_layer] = args.dmrs_patterns[i_layer].re_pattern;
    }
    for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
      deferred_fill.dmrs_sym[i_symbol] = dmrs_sym[i_symbol];
    }
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
        deferred_fill.filtered_dst[i_layer * MAX_NOF_DMRS_SYMBOLS + i_symbol] =
            args.filtered_pilots_lse_view.get_symbol(i_symbol, i_layer);
        deferred_fill.freq_dst[i_layer * MAX_NOF_DMRS_SYMBOLS + i_symbol] =
            args.freq_response.get_symbol(i_symbol, i_layer);
      }
    }
  } else {
    pending_fill fill;
    fill.nof_prb    = nof_prb;
    fill.npt        = npt;
    fill.nof_layers = nof_layers;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      fill.re_pattern[i_layer] = args.dmrs_patterns[i_layer].re_pattern;
      for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
        fill.dmrs_sym[i_symbol]                                                       = dmrs_sym[i_symbol];
        fill.filtered_dst[i_layer * MAX_NOF_DMRS_SYMBOLS + i_symbol] =
            args.filtered_pilots_lse_view.get_symbol(i_symbol, i_layer);
        fill.freq_dst[i_layer * MAX_NOF_DMRS_SYMBOLS + i_symbol] = args.freq_response.get_symbol(i_symbol, i_layer);
      }
    }
    fill.fill(grid_est);
  }

#if defined(OCUDU_CE_TIME)
  {
    const auto t_finish = steady_clock::now();
    const auto us       = [](auto d) { return std::chrono::duration<double, std::micro>(d).count(); };
    logger.debug("[mmse_time] prb={} npt={} L={} n_std={} gpu={} nn={} pad={} fb={} | pre={:.1f}us stage={:.1f}us sigma2={:.1f}us corr_std={:.1f}us "
                 "gpu_path={:.1f}us (gpu_wait={:.1f}us) cpu_blocks={:.1f}us finish={:.1f}us | total={:.1f}us",
                 nof_prb,
                 npt,
                 (n_std_blocks != 0 || tail_L == 0) ? L_std : tail_L,
                 n_std_blocks,
                 hop_gpu ? 1 : 0,
                 hop_nn ? 1 : 0,
                 hop_pad,
                 cpu_fallback_blocks,
                 args.pre_stage_us,
                 stage_us_local,
                 submit_us_local,
                 unpack_us_local,
                 us(t_sigma2 - t_begin),
                 us(t_corr_std - t_sigma2),
                 us(t_gpu_end - t_corr_std),
                 engine_ready ? engine->last_gpu_wait_us() : 0.0,
                 us(t_cpu_end - t_gpu_end),
                 us(t_finish - t_cpu_end),
                 us(t_finish - t_begin));

    // Aggregated summary (printed once at exit, stderr) - the debug line above is only usable
    // interactively because it emits tens of lines per slot. A deferred batch is waited for outside
    // this call and its GPU busy time is only known then, so the accounting is finished by
    // complete_fd_td_estimation_stage(); until it runs, the measurements wait here.
    if (stage_pending) {
      deferred_stats = {true,
                        nof_prb,
                        npt,
                        hop_gpu,
                        hop_nn,
                        cpu_fallback_blocks,
                        args.pre_stage_us,
                        stage_us_local,
                        submit_us_local,
                        unpack_us_local,
                        us(t_sigma2 - t_begin),
                        us(t_corr_std - t_sigma2),
                        us(t_gpu_end - t_corr_std),
                        us(t_cpu_end - t_gpu_end),
                        us(t_finish - t_begin)};
      deferred_wait_begin = steady_clock::now();
    } else {
      mmse_stats_accumulate(nof_prb,
                            npt,
                            hop_gpu,
                            hop_nn,
                            cpu_fallback_blocks,
                            args.pre_stage_us,
                            stage_us_local,
                            submit_us_local,
                            unpack_us_local,
                            us(t_sigma2 - t_begin),
                            us(t_corr_std - t_sigma2),
                            us(t_gpu_end - t_corr_std),
                            engine_ready ? engine->last_gpu_wait_us() : 0.0,
                            us(t_cpu_end - t_gpu_end),
                            us(t_finish - t_begin));
    }
  }
#endif
}

void port_channel_estimator_metal_mmse_impl::stage_engine_group(const fd_td_estimation_stage_args& args,
                                                                unsigned                           gb_start,
                                                                unsigned                           n_blk,
                                                                unsigned                           b_prb,
                                                                unsigned                           npt,
                                                                unsigned                           nout,
                                                                unsigned                           L,
                                                                unsigned                           sys_offset,
                                                                const engine_strides&              st,
                                                                bool                               matrix,
                                                                bool                               gpu_invert,
                                                                bool                               slots_filled)
{
  const unsigned nof_layers = args.dmrs_patterns.size();
  // Pilots of one PRB, and the pilots this group carries per DM-RS symbol. The pilot view of a
  // (symbol, layer) is [prb][comb], one PRB after another, so the slice of block b starts at
  // (gb_start + b * b_prb) * comb: gb_start counts PRBs while b counts blocks of b_prb PRBs. Using
  // gb_start * npf instead (npf being the block's pilot count) reads the wrong pilots whenever the
  // group starts past PRB 0 AND carries more than one PRB - the edge block of a hop whose
  // allocation is not a multiple of the block size, which is where the air interface spent most of
  // its failed grants.
  const unsigned comb       = args.dmrs_patterns.front().re_pattern.count();
  const unsigned npf        = b_prb * comb;
  const unsigned Ls         = st.L;
  const unsigned Ns         = st.nout;
  ocudu_assert((L <= Ls) && (nout <= Ns), "Engine slot strides must cover the block geometry.");

  // A per system: K1 inverts it in place (gpu_invert), otherwise the CPU inverse is staged.
  //
  // When the device built the matrices (K0-d), the packed L x L region of every slot already holds
  // A (or the identity-padded form the inversion expects) and the rest of the slot is zero from
  // construction: writing it again here is the ~170KB memcpy this stage used to be, so it is
  // skipped entirely - the pad only has to stay zero, which it does because the kernel never
  // touches it and the device build happens on every hop that reaches this point.
  for (unsigned sys = 0; !slots_filled && (sys != nof_layers); ++sys) {
    float* a_slot = gpu_a + static_cast<std::size_t>(sys_offset + sys) * Ls * Ls;
    if (gpu_invert) {
      if (Ls == L) {
        std::memcpy(a_slot, w_r_pp.data(), static_cast<std::size_t>(L) * L * sizeof(float));
      } else {
        // Oversized slot: pad A to blockdiag(A, I). K1 then inverts one invertible Ls x Ls system
        // and W = [R_hp | 0] . blockdiag(A^-1, I) = [R_hp . A^-1 | 0]: the pad columns of W are
        // exactly zero, so h = W . y keeps the values of the unpadded system.
        std::memset(a_slot, 0, static_cast<std::size_t>(Ls) * Ls * sizeof(float));
        for (unsigned r = 0; r != L; ++r) {
          std::memcpy(a_slot + static_cast<std::size_t>(r) * Ls,
                      w_r_pp.data() + static_cast<std::size_t>(r) * L,
                      static_cast<std::size_t>(L) * sizeof(float));
        }
        for (unsigned k = L; k != Ls; ++k) {
          a_slot[static_cast<std::size_t>(k) * Ls + k] = 1.0F;
        }
      }
    } else {
      std::array<float, 2 * MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS> gj;
      std::fill(gj.begin(), gj.end(), 0.0F);
      for (unsigned r = 0; r != L; ++r) {
        for (unsigned c = 0; c != L; ++c) {
          gj[r * 2 * L + c] = w_r_pp[r * L + c];
        }
        gj[r * 2 * L + L + r] = 1.0F;
      }
      gauss_jordan_invert(span<float>(gj.data(), 2 * L * L), L);
      // The pad region stays zero: the pad columns of W are R_hp_pad . A^-1 = 0 either way, and
      // the nn kernels require exact zeros there.
      std::memset(a_slot, 0, static_cast<std::size_t>(Ls) * Ls * sizeof(float));
      for (unsigned r = 0; r != L; ++r) {
        std::memcpy(a_slot + static_cast<std::size_t>(r) * Ls,
                    &gj[static_cast<std::size_t>(r) * 2 * L + L],
                    static_cast<std::size_t>(L) * sizeof(float));
      }
    }

    // R_hp: real values in rows o < nout and columns k < L, zero everywhere else (the pad rows and
    // columns of an oversized slot are what keeps the pad columns of W zero).
    float* rp_slot = gpu_r_hp + static_cast<std::size_t>(sys_offset + sys) * Ns * Ls;
    for (unsigned o = 0; o != Ns; ++o) {
      float* row = rp_slot + static_cast<std::size_t>(o) * Ls;
      if (o < nout) {
        std::memcpy(row,
                    w_r_hp.data() + static_cast<std::size_t>(o) * L,
                    static_cast<std::size_t>(L) * sizeof(float));
        if (Ls > L) {
          std::memset(row + L, 0, static_cast<std::size_t>(Ls - L) * sizeof(float));
        }
      } else {
        std::memset(row, 0, static_cast<std::size_t>(Ls) * sizeof(float));
      }
    }
  }

  // Pilot vectors of all layers and blocks [gb_start, gb_start + n_blk).
  // Matrix flavor: quad-interleaved qy [layer][nquads][Ls][8] (cols = 2*(b%4)+{re,im}; pad rows
  // k >= L and the tail-quad columns of non-existent blocks zeroed). Legacy: per-block
  // real/imag interleaved y [layer][n_blk][2*Ls].
  if (matrix) {
    const unsigned nquads = (n_blk + 3u) / 4u;
    const unsigned n_tail = n_blk & 3u;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned quad = 0; quad != nquads; ++quad) {
        float* qbase = gpu_qy + ((static_cast<std::size_t>(sys_offset + i_layer) * nquads + quad) * Ls) * 8;
        if (quad + 1 == nquads && n_tail != 0) {
          // Tail quad: full clear (missing-block columns and pad rows in one memset).
          std::memset(qbase, 0, static_cast<std::size_t>(Ls) * 8 * sizeof(float));
        } else if (Ls > L) {
          // Full quad: only the pad rows k in [L, Ls) are not written by the pack.
          std::memset(qbase + static_cast<std::size_t>(L) * 8,
                      0,
                      static_cast<std::size_t>(Ls - L) * 8 * sizeof(float));
        }
      }
      for (unsigned b = 0; b != n_blk; ++b) {
        const unsigned quad = b / 4u;
        const unsigned bl   = b % 4u;
        float* qp = gpu_qy + ((static_cast<std::size_t>(sys_offset + i_layer) * nquads + quad) * Ls) * 8 + 2 * bl;
        for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
          span<const cf_t> src =
              args.pilots_lse_view.get_symbol(i_symbol, i_layer).subspan((gb_start + b * b_prb) * comb, npf);
          for (unsigned j = 0; j != npf; ++j) {
            qp[(i_symbol * npf + j) * 8]     = src[j].real();
            qp[(i_symbol * npf + j) * 8 + 1] = src[j].imag();
          }
        }
      }
    }
  } else {
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned b = 0; b != n_blk; ++b) {
        float* yp = gpu_y + (static_cast<std::size_t>(sys_offset + i_layer) * st.n_blk + b) * 2 * Ls;
        // Pad rows k in [L, Ls) stay zero: their weights are exactly zero, but a non-finite value
        // left there would reach h through 0 * inf = NaN.
        if (Ls > L) {
          std::memset(yp + 2 * L, 0, static_cast<std::size_t>(Ls - L) * 2 * sizeof(float));
        }
        for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
          span<const cf_t> src =
              args.pilots_lse_view.get_symbol(i_symbol, i_layer).subspan((gb_start + b * b_prb) * comb, npf);
          for (unsigned j = 0; j != npf; ++j) {
            yp[2 * (i_symbol * npf + j)]     = src[j].real();
            yp[2 * (i_symbol * npf + j) + 1] = src[j].imag();
          }
        }
      }
    }
  }
}

bool port_channel_estimator_metal_mmse_impl::engine_run(const metal::mmse_engine::corr_stage* corr,
                                                        unsigned nout,
                                                        unsigned L,
                                                        unsigned nof_systems,
                                                        unsigned nof_blocks,
                                                        bool     matrix,
                                                        bool     gpu_invert,
                                                        const metal::mmse_engine::reformat_stage* reformat,
                                                        bool     defer)
{
  // Weight (W = R_hp . A^-1) + apply (h = W . y) in ONE engine command buffer, with the inversion
  // (K1) prepended in the same buffer when the A slots hold A itself, and the equalizer's
  // per-symbol estimates (K3) appended to the same buffer when the caller asked for them. The
  // engine return value is checked (S-1 audit fix): on failure the caller falls back to the CPU
  // reference math for these blocks instead of unpacking stale gpu_h contents.
  // Safety net (run_engine_blocks() has to do this before its staging): a batch may not be left
  // outstanding while another one is submitted, because they share the gpu_h staging buffer.
  if (nof_pending_unpacks != 0) {
    (void)complete_fd_td_estimation_stage();
  }

  // One entry point for the batch: the weights pipeline always. The K1 inversion is part of it when
  // the slots hold A itself (see invert_first below), so there is no second flavor to dispatch to -
  // the "inverted" form of run() is that same pipeline with K1 prepended. The matrix (nn) flavor is
  // the only genuinely different one.
  const bool engine_ok =
      matrix ? engine->run_nn(gpu_a, gpu_r_hp, gpu_w, gpu_qy, gpu_h, nout, L, nof_systems, nof_blocks)
             : (defer ? engine->run_weights_only_async(
                            gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, nout, L, nof_systems, nof_blocks, reformat, corr)
                      : engine->run_weights_only(
                            gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, nout, L, nof_systems, nof_blocks, reformat, corr));
  if (!engine_ok) {
    logger.error("[mmse_ce] engine call failed (systems={} blocks={} nout={} L={} matrix={}): falling back to the "
                 "CPU path for these blocks",
                 nof_systems,
                 nof_blocks,
                 nout,
                 L,
                 matrix ? 1 : 0);
  }
  return engine_ok;
}

unsigned port_channel_estimator_metal_mmse_impl::stage_re_masks(const fd_td_estimation_stage_args& args,
                                                                unsigned                           nof_prb,
                                                                unsigned                           first_prb)
{
  const auto& hop_rb_mask = (args.hop == 0) ? args.dmrs_patterns.front().rb_mask : args.dmrs_patterns.front().rb_mask2;

  // The destination index is arithmetic (see ocudu_mmse_reformat.metal), which requires a
  // contiguous allocation: every PRB then contributes the same data REs. A non-contiguous
  // allocation simply does not get device estimates - the consumer falls back to the host gather.
  if ((hop_rb_mask.find_highest() + 1 - first_prb) != nof_prb) {
    return 0;
  }

  // DM-RS resource elements of one PRB: the union over the layers' RE patterns. For the type-1
  // pattern these are the per-layer combs, so the union is exactly the comb set that the
  // demodulator's CDM group count selects for the DM-RS symbols.
  unsigned dmrs_re_bits = 0;
  for (const auto& pattern : args.dmrs_patterns) {
    pattern.re_pattern.for_each(0, pattern.re_pattern.size(), [&](unsigned pos) {
      dmrs_re_bits |= 1u << (pos % NOF_SUBCARRIERS_PER_RB);
    });
  }
  const unsigned dmrs_per_prb = static_cast<unsigned>(__builtin_popcount(dmrs_re_bits));

  // DM-RS symbols of the slot (all layers of a PUSCH share them).
  const auto& slot_dmrs = args.dmrs_patterns.front().symbols;
  unsigned    dmrs_sym_bits = 0;
  for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
    dmrs_sym_bits |= (slot_dmrs.test(sym) ? 1u : 0u) << sym;
  }

  // Size and per-symbol offsets: a DM-RS symbol contributes the data REs of its comb, the others
  // all twelve.
  unsigned total = 0;
  for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
    re_offsets[sym] = total;
    const unsigned drpp = slot_dmrs.test(sym) ? (NOF_SUBCARRIERS_PER_RB - dmrs_per_prb) : NOF_SUBCARRIERS_PER_RB;
    total += nof_prb * drpp;
  }
  re_offsets[MAX_NSYMB_PER_SLOT] = total;

  gpu_ce_drpp          = NOF_SUBCARRIERS_PER_RB;
  gpu_ce_drpp_dmrs     = NOF_SUBCARRIERS_PER_RB - dmrs_per_prb;
  gpu_ce_dmrs_re_bits  = dmrs_re_bits;
  gpu_ce_dmrs_sym_bits = dmrs_sym_bits;

  return total;
}

void port_channel_estimator_metal_mmse_impl::unpack_engine_group(unsigned              gb_start,
                                                                 unsigned              n_blk,
                                                                 unsigned              b_prb,
                                                                 unsigned              nout,
                                                                 unsigned              nof_layers,
                                                                 unsigned              sys_offset,
                                                                 const engine_strides& st)
{
  const unsigned nf = b_prb * NOF_SUBCARRIERS_PER_RB;
  // gb_start counts PRBs while b counts blocks of b_prb PRBs, so block b starts at subcarrier
  // (gb_start + b * b_prb) * 12. Using (gb_start + b) * nf instead (nf being the block's
  // subcarriers) is right only while gb_start is zero or a block is one PRB wide, and it lands
  // past the end of the grid for the edge block of a hop that is not a multiple of the block
  // size - the same mistake the pilot staging had, in the host half of the estimator.
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned b = 0; b != n_blk; ++b) {
      // Slot addressing uses the BATCH strides: they may exceed the block geometry (the merged
      // tail system keeps the standard Ls/Ns/n_blk slots while its own block is narrower), and
      // reading it with the block geometry silently unpacks the wrong rows.
      const float* hp = gpu_h + (static_cast<std::size_t>(sys_offset + i_layer) * st.n_blk + b) * 2 * st.nout;
      for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
        span<cf_t> dst = grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + sym)
                             .subspan(static_cast<std::size_t>(gb_start + b * b_prb) * NOF_SUBCARRIERS_PER_RB, nf);
        for (unsigned sc = 0; sc != nf; ++sc) {
          dst[sc] = {hp[2 * (sym * nf + sc)], hp[2 * (sym * nf + sc) + 1]};
        }
      }
    }
  }
}

std::optional<metal::mmse_engine::corr_stage> port_channel_estimator_metal_mmse_impl::build_slots_on_device(
    const channel_statistics&                     stats,
    const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
    unsigned                                      b_prb,
    span<const unsigned>                          dmrs_slots,
    unsigned                                      scs_khz,
    unsigned                                      sys_offset,
    unsigned                                      nof_systems,
    unsigned                                      a_stride,
    unsigned                                      r_stride,
    unsigned                                      L)
{
  unsigned nout_c = 0;
  unsigned L_c    = 0;
  const metal::mmse_engine::corr_stage corr_std =
      correlation_stage(stats, re_pattern, b_prb, dmrs_slots, scs_khz, sys_offset, nout_c, L_c, a_stride, r_stride);

  // The device-inversion experiment does not build anything here: its build has to ride the weights
  // command buffer as a prefix (so K1 can invert in that same buffer, one round trip less). The
  // caller gets the descriptor back and dispatches it itself.
  if (device_invert_on_device()) {
    return corr_std;
  }

  if (!engine->build_correlation(corr_std, nof_systems)) {
    return std::nullopt;
  }
  // Finish what the weights read: A^-1 in place, row by row through a scratch buffer because the
  // source and the destination are the same memory. The inversion stays on the HOST on purpose: the
  // float32 device kernel's element-wise error on a real A is 9.7e-1 against this Gauss-Jordan's
  // 1.7e-1 (measured, k1_check), and Metal has no double type to close that gap - so a device
  // inversion puts the 256QAM capture at -18.7 dB of SINR instead of 24 dB (measured on air data).
  for (unsigned sys = 0; sys != nof_systems; ++sys) {
    float* a_slot = gpu_a + static_cast<std::size_t>(sys_offset + sys) * a_stride * a_stride;
    std::array<float, 2 * MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS> gj;
    std::fill(gj.begin(), gj.end(), 0.0F);
    for (unsigned r = 0; r != L; ++r) {
      for (unsigned c = 0; c != L; ++c) {
        gj[r * 2 * L + c] = a_slot[static_cast<std::size_t>(r) * a_stride + c];
      }
      gj[r * 2 * L + L + r] = 1.0F;
    }
    gauss_jordan_invert(span<float>(gj.data(), 2 * L * L), L);
    for (unsigned r = 0; r != L; ++r) {
      std::memcpy(a_slot + static_cast<std::size_t>(r) * a_stride,
                  &gj[static_cast<std::size_t>(r) * 2 * L + L],
                  static_cast<std::size_t>(L) * sizeof(float));
    }
    // The identity pad of an oversized slot: the correlation kernel wrote the L x L block only.
    for (unsigned k = L; k != a_stride; ++k) {
      a_slot[static_cast<std::size_t>(k) * a_stride + k] = 1.0F;
    }
  }
  return std::nullopt;
}

bool port_channel_estimator_metal_mmse_impl::run_engine_blocks(const fd_td_estimation_stage_args& args,
                                                               unsigned                           gb_start,
                                                               unsigned                           n_blk,
                                                               unsigned                           b_prb,
                                                               unsigned                           nout,
                                                               unsigned                           L,
                                                               unsigned                           npt,
                                                               bool                               matrix,
                                                               const metal::mmse_engine::reformat_stage* reformat,
                                                               bool                               defer,
                                                               const channel_statistics*          device_stats,
                                                               unsigned                           sys_offset)
{
  const unsigned nof_layers = args.dmrs_patterns.size();

  // A batch of this hop may still be outstanding - the split-tail path runs two per hop. Its
  // estimates are in gpu_h and its kernels are still reading the staging slots this call is about
  // to overwrite, so complete it (wait and unpack) before touching them. Only ever the LAST batch
  // of a hop stays outstanding, which is what the caller's completion then unpacks.
  if (nof_pending_unpacks != 0) {
    (void)complete_fd_td_estimation_stage();
  }

  // Slot strides: the legacy kernels use the compact L / nout layout; the matrix kernels use the
  // zero-padded ceil8 strides (Lp/Np) - the staging zeroes the pad rows/columns below so the
  // kernels tile 8x8 seamlessly and the pad regions stay exactly zero.
  const engine_strides st{matrix ? ((L + 7u) & ~7u) : L, matrix ? ((nout + 7u) & ~7u) : nout, n_blk};

  // ---- Slots of this batch ---------------------------------------------------------------------
  // The legacy weights kernels read A^-1, so the slots must HOLD A^-1 when the engine call below
  // runs. Two ways to get there:
  //   (a) the host builds A and inverts it in place (stage_engine_group(), the fallback), or
  //   (b) the DEVICE builds A and R_hp (K0-d, this is the full-GPU-path form) and the host then
  //       inverts what the device wrote - the device build removes the host's construction of the
  //       matrices, which is the expensive half.
  // The inversion stays on the HOST either way, and that is deliberate: K1 (the device inversion)
  // is not accurate enough at this block order (measured 1.3e-5 relative error at order 54 - see
  // k1_check and the plan), while the host Gauss-Jordan is exact to float32 and costs ~10us.
  //
  // NOTE the order: the device must write BEFORE the host reads. A device build that runs as a
  // command-buffer PREFIX of the weights call (the form run_weights_only() supports for the orders
  // K1 can handle) would land AFTER this host loop and leave the weights reading a raw A - which is
  // exactly the 1640x blow-up of S-7f-3g. Hence the standalone, synchronously completed
  // build_correlation() here.
  std::optional<metal::mmse_engine::corr_stage> device_corr;
  if (device_stats != nullptr) {
    // DM-RS slot symbols, rebuilt from the stage's own pattern: this function works from the block
    // geometry (npt), while the descriptor needs the slot INDICES (the time correlation depends on
    // them, not only on their count).
    static_vector<unsigned, MAX_NOF_DMRS_SYMBOLS> dmrs_slots;
    args.pattern_symbols.for_each(args.first_symbol, args.last_symbol, [&](unsigned s) { dmrs_slots.push_back(s); });
    device_corr = build_slots_on_device(*device_stats,
                                        args.dmrs_patterns.front().re_pattern,
                                        b_prb,
                                        span<const unsigned>(dmrs_slots.begin(), dmrs_slots.size()),
                                        scs_to_khz(args.scs),
                                        sys_offset,
                                        nof_layers,
                                        st.L,
                                        nout,
                                        L);
  }
  const bool dev_inv_now = device_corr.has_value();
  // stage_engine_group() runs EVERY time: besides A and R_hp it also stages the pilot vectors (y or
  // qy), which every path needs - skipping the call left y zero and the apply produced infinities
  // (measured). slots_filled only tells it to leave the A/R_hp slots alone, which is what the device
  // build needs: it is about to fill them itself, and a host write of A^-1 there would be inverted
  // again by K1 (the S-7f-3i defect).
  stage_engine_group(args, gb_start, n_blk, b_prb, npt, nout, L, sys_offset, st, matrix, false, dev_inv_now);
  const bool deferred = defer && !matrix;
  // No correlation prefix (corr == nullptr) and no K1: this call's slots already hold A^-1, either
  // from the host staging or from the device build finished above.
  // The device-inversion experiment hands the descriptor to this call, so the correlation kernels run
  // as a prefix of the SAME command buffer and K1 inverts in it; the default path passed no
  // descriptor and the slots already hold A^-1.
  if (!engine_run(device_corr.has_value() ? &device_corr.value() : nullptr,
                  nout,
                  L,
                  nof_layers,
                  n_blk,
                  matrix,
                  device_corr.has_value(),
                  reformat,
                  deferred)) {
    return false;
  }
  if (deferred) {
    defer_unpack(gb_start, n_blk, b_prb, nout, nof_layers, sys_offset, st);
  } else {
    unpack_engine_group(gb_start, n_blk, b_prb, nout, nof_layers, sys_offset, st);
  }
  return true;
}

void port_channel_estimator_metal_mmse_impl::defer_unpack(unsigned              gb_start,
                                                          unsigned              n_blk,
                                                          unsigned              b_prb,
                                                          unsigned              nout,
                                                          unsigned              nof_layers,
                                                          unsigned              sys_offset,
                                                          const engine_strides& st)
{
  ocudu_assert(nof_pending_unpacks < max_pending_unpacks,
               "A hop has at most {} batches pending, this one already has {}.",
               max_pending_unpacks,
               nof_pending_unpacks);
  pending_unpacks[nof_pending_unpacks++] = pending_unpack{gb_start, n_blk, b_prb, nout, nof_layers, sys_offset, st};
  // The stage is outstanding from this point (not only from the end of the stage call): a batch of
  // the same hop that is submitted later - the split-tail path - must complete this one first.
  stage_pending = true;
}

void port_channel_estimator_metal_mmse_impl::pending_fill::fill(
    const static_re_buffer<MAX_LAYERS * MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS>& grid) const
{
  // An extra 1 / beta in this path inflated RSrp by 1 / beta^2 and the noise variance by about
  // 1 / beta^4, i.e. 17 dB of missing soft bits with the 0.708 of a real cell, so the values are
  // taken as they are (the pilots were already scaled in the data domain).
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    const auto& pattern = re_pattern[i_layer];
    for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
      const unsigned   slot_sym = dmrs_sym[i_symbol];
      span<const cf_t> src      = grid.get_slice(i_layer * MAX_NSYMB_PER_SLOT + slot_sym);
      span<cf_t>       dst      = filtered_dst[i_layer * MAX_NOF_DMRS_SYMBOLS + i_symbol];
      unsigned         j        = 0;
      for (unsigned prb = 0; prb != nof_prb; ++prb) {
        pattern.for_each(0, pattern.size(), [&](unsigned pos) { dst[j++] = src[prb * NOF_SUBCARRIERS_PER_RB + pos]; });
      }
      ocuduvec::copy(freq_dst[i_layer * MAX_NOF_DMRS_SYMBOLS + i_symbol], src);
    }
  }
}

bool port_channel_estimator_metal_mmse_impl::complete_fd_td_estimation_stage()
{
  if (!stage_pending) {
    return true;
  }
  stage_pending = false;

#if defined(OCUDU_CE_TIME)
  const auto t_wait_begin = std::chrono::steady_clock::now();
#endif
  const bool ok = (engine == nullptr) || engine->wait_pending();
  // Temporary experiment (OCUDU_CE_NV_OVERRIDE): replace the device noise variance with a known
  // value, to tell "the estimates are wrong" apart from "only the noise scale is wrong".
  if (const char* nv_env = std::getenv("OCUDU_CE_NV_OVERRIDE"); (nv_env != nullptr) && (gpu_nv != nullptr)) {
    gpu_nv[0] = std::strtof(nv_env, nullptr);
  }
#if defined(OCUDU_CE_TIME)
  mmse_stats().completion_wait_ns.fetch_add(
      static_cast<uint64_t>(
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_wait_begin).count() * 1e3),
      std::memory_order_relaxed);
#endif
#if defined(OCUDU_CE_TIME)
  // The wait above is the rest of the GPU phase of a deferred hop: count it with everything the
  // stage measured before returning (see mmse_stats_accumulate()).
  if (deferred_stats.valid) {
    const auto   now     = std::chrono::steady_clock::now();
    const double wait_us = std::chrono::duration<double, std::micro>(now - deferred_wait_begin).count();
    mmse_stats_accumulate(deferred_stats.nof_prb,
                          deferred_stats.npt,
                          deferred_stats.hop_gpu,
                          deferred_stats.hop_nn,
                          deferred_stats.fallback_blocks,
                          deferred_stats.pre_stage_us,
                          deferred_stats.stage_us,
                          deferred_stats.submit_us,
                          deferred_stats.unpack_us,
                          deferred_stats.sigma2_us,
                          deferred_stats.corr_us,
                          deferred_stats.gpu_path_us,
                          engine_ready ? engine->last_gpu_wait_us() : 0.0,
                          deferred_stats.cpu_blocks_us,
                          deferred_stats.total_us,
                          wait_us);
    deferred_stats.valid = false;
  }
#endif
  if (!ok) {
    // A command buffer that failed after a successful submission cannot be recomputed here: the CPU
    // reference math that backs up a failed engine call would need the hop state this stage no
    // longer holds. Report it instead of leaving the consumer with the previous hop's estimates.
    logger.error("[mmse_ce] the deferred engine batch of this hop failed: its estimates are not valid");
    nof_pending_unpacks = 0;
    deferred_fill.valid = false;
    return false;
  }
#if defined(OCUDU_CE_TIME)
  const auto t_unpack2_begin = std::chrono::steady_clock::now();
#endif
  for (unsigned i = 0; i != nof_pending_unpacks; ++i) {
    const pending_unpack& u = pending_unpacks[i];
    unpack_engine_group(u.gb_start, u.n_blk, u.b_prb, u.nout, u.nof_layers, u.sys_offset, u.st);
  }
#if defined(OCUDU_CE_TIME)
  mmse_stats().completion_unpack_ns.fetch_add(
      static_cast<uint64_t>(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                                     t_unpack2_begin)
                                .count() *
                            1e3),
      std::memory_order_relaxed);
#endif
  nof_pending_unpacks = 0;

  // The grid is complete now: derive the buffers the hop statistics are computed from.
  if (deferred_fill.valid) {
#if defined(OCUDU_CE_TIME)
    const auto t_fill_begin = std::chrono::steady_clock::now();
#endif
    deferred_fill.fill(grid_est);
    deferred_fill.valid = false;
#if defined(OCUDU_CE_TIME)
    mmse_stats().completion_fill_ns.fetch_add(
        static_cast<uint64_t>(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                                       t_fill_begin)
                                  .count() *
                              1e3),
        std::memory_order_relaxed);
#endif
  }
  return true;
}

std::optional<ch_est_device_view> port_channel_estimator_metal_mmse_impl::get_device_ch_estimates(
    unsigned i_symbol,
    unsigned tx_layer) const
{
  if (!gpu_ce_ready || (i_symbol >= MAX_NSYMB_PER_SLOT) || (tx_layer >= gpu_ce_layers)) {
    return std::nullopt;
  }
  ch_est_device_view view;
  view.data       = reinterpret_cast<const cbf16_t*>(gpu_ce);
  view.offset     = re_offsets[i_symbol];
  view.nof_re     = re_offsets[i_symbol + 1] - re_offsets[i_symbol];
  view.total_re   = gpu_ce_total_re;
  view.nof_layers = gpu_ce_layers;
  return view;
}

void port_channel_estimator_metal_mmse_impl::get_symbol_ch_estimate(span<cbf16_t> symbol,
                                                                    unsigned      i_symbol,
                                                                    unsigned      tx_layer) const
{
  span<const cf_t> src = grid_est.get_slice(tx_layer * MAX_NSYMB_PER_SLOT + i_symbol);
  ocudu_assert(symbol.size() == src.size(), "Invalid symbol buffer size.");
  for (unsigned i = 0; i != src.size(); ++i) {
    symbol[i] = to_cbf16(src[i]);
  }
}

void port_channel_estimator_metal_mmse_impl::get_symbol_ch_estimate(
    span<cbf16_t>                              symbol,
    unsigned                                   i_symbol,
    unsigned                                   tx_layer,
    const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const
{
  span<const cf_t> src = grid_est.get_slice(tx_layer * MAX_NSYMB_PER_SLOT + i_symbol);
  unsigned         j   = 0;
  re_mask.for_each(0, re_mask.size(), [&](unsigned i_re) { symbol[j++] = to_cbf16(src[i_re]); });
}
