// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// (Derives from the upstream port_channel_estimator_average_impl base class.)

#include "port_channel_estimator_metal_mmse_impl.h"
#include "../port_channel_estimator_helpers.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_crossings.h"
#include "ocudu_metal_lane_clock.h"
#include "ocudu/support/math/math_utils.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
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

/// The device's pilot-extraction stage (K0-a). OCUDU_CE_CPU_LS=1 forces the host pre-stage: the
/// escape hatch, and the A/B of the tolerance probe.
bool device_ls_enabled()
{
  static const bool value = (std::getenv("OCUDU_CE_CPU_LS") == nullptr);
  return value;
}

/// \brief Whether the correlation stage loads A's diagonal from the DEVICE's noise-to-pilot-power
/// ratio instead of the host's (S-7g-20, the first half of the K0-a fusion).
///
/// DEFAULT ON since the fast-math flag was fixed. The extraction's own command buffer computes the
/// ratio (mmse_pilots_power's out[2]) in the host's own two operations on the host's own two operands,
/// and that is now bit-identical - but only because ocudu_mmse_pilots.metal is compiled with
/// -fno-fast-math (see CMakeLists.txt): under Metal's default fast math the divide came out
/// differently rounded from the host's in 298151 of 2^20 random (sigma2-like, power-like) pairs
/// (28.4%, both directions), which changed A's diagonal loading - amplified by cond_2(A) ~ 2e4 - on
/// 27 of 27 corpus captures and flipped LLR decisions on all of them (32613 soft bits). With the flag
/// the same sweep is 0 of 2^20 and the device quotient matches the host's on every corpus hop.
///
/// The lesson is in the numbers, not the hardware: this GPU's divide is correctly rounded when the
/// compiler is not allowed to reassociate it. A new float expression on this path must therefore be
/// measured (OCUDU_CE_K0A_RATIO_CHECK=1 prints both quotients per hop), never argued.
///
/// OCUDU_CE_K0A_RATIO_DEV=0 keeps the host's quotient: it is the A/B that produced the numbers above,
/// and the escape hatch for a metallib built without the strict flag (the kernel would then read a
/// differently rounded value - not a wrong route, but not the measured one either).
/// \brief Whether the edge-slot comparison (OCUDU_CE_EDGE_CHECK) is armed.
///
/// A diagnostic with no effect on any published value: it builds the edge group's matrices on the
/// host as well and compares them element by element against the device's slots (see
/// check_edge_slots()). Unset is the product.
bool edge_slot_check_enabled()
{
  static const bool value = []() {
    return std::getenv("OCUDU_CE_EDGE_CHECK") != nullptr;
  }();
  return value;
}

bool k0a_ratio_from_device_enabled()
{
  static const bool value = []() {
    const char* env = std::getenv("OCUDU_CE_K0A_RATIO_DEV");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();
  return value;
}

/// \brief The host <-> device data crossing this estimator used to make (OCUDU_CE_HOST_SCALARS).
///
/// Unset or non-zero: the host reads the hop's CFO, noise variance and pilots' power sum out of the
/// command buffer the extraction wrote, exactly as it always has. This is now the ESCAPE HATCH, kept
/// because it is the reference the default is judged against and because it isolates a device
/// regression to one side.
///
/// Zero (THE DEFAULT, and what this line ships): it does not, and those three scalars are taken from
/// the device or recomputed on the host instead. These reads were the whole of the crossing the `gpu`
/// pipeline mode forbids - they are what phy_pipeline_crossings counts, and on air they measured 3.00
/// per device hop against a requirement of 0. The reason they can go is that the device no longer
/// needs what the host computes from them:
///
///   * the correlation kernel loads A's diagonal from the DEVICE's own quotient whenever
///     corr_stage::sigma2_dev is set (ocudu_mmse_corr.metal: `(p.sigma2_from_device != 0u) ?
///     scalars[p.sigma2_slot] : p.sigma2`), and OCUDU_CE_K0A_RATIO_DEV is on by default, so the
///     host's sigma2 never reaches the kernel on that route;
///   * the reformat reads the hop's CFO from the device since the rotating-slot change, so the host's
///     value only feeds statistics and the host-route kernel parameter.
///
/// What was left was the possibility that some route still consumed the host's copies. That was
/// measured, not argued, before this default was flipped: with the device building the merged edge
/// block too (the TAIL_DEV default), OCUDU_CE_HOST_SCALARS=0 against the reading route is
/// _llr.bin 0 bytes and _h.bin 0 bytes over all 27 corpus captures, with the whole difference 409
/// bytes of _ce.txt naming exactly the three scalars this flag stops reading (noise_variance and the
/// snr derived from it move, cfo_hz becomes na, while rsrp, epre and ta_us do not). The published LLR
/// does not move, which is what makes the skip safe rather than merely quiet.
///
/// \note The values become a fixed zero rather than an uninitialised read, so a run stays
/// deterministic and comparing dumps still means something.
/// \brief The last correlation build still on the host: the merged edge block
/// (OCUDU_CE_TAIL_DEV).
///
/// Unset or non-zero (THE DEFAULT, and what this line ships): the device builds the edge block's
/// correlation matrices, in the standard group's own engine call.
///
/// Zero: the host builds them and stages them - the route every hop took before. It is kept because it
/// is the reference the device build is judged against (TAIL_DEV=0 vs 1 must be byte-identical, see
/// the gate below), and an escape hatch if a device regression ever needs to be isolated.
///
/// \note What this used to be, and why the flip is safe NOW. The device build was previously opt-in
/// because it did not agree with the host's: 6043 differing bytes, confined to the edge region of _h.
/// The cause was NOT the geometry, the pads or the inversion - it was that encode_corr() sized the
/// correlation slots' zero-copy mapping by the PACKED block size (l * l and nout * l) while both
/// kernels write with the SLOT's row stride (Ls), so a block narrower than its slot - exactly the edge
/// group, L_e into the L_std slots - lost every write past the mapped end. A escaped it by being
/// square; R_hp did not. Fixed in 93964256aa; see doc_chinese/phy_pipeline_gpu/wip/S7_s4_root_cause.md
/// for the measurement.
///
/// Gate at the flip: 27 of 27 captures byte-identical between TAIL_DEV=0 and the new default, 0
/// differing bytes (it was 6043); the estimator unit tests pass; the strict and CPU-LS nets are clean.
///
/// This is also the last correlation build out of the host, and with it the host's last reason to read
/// the extraction's scalars: with this on, OCUDU_CE_HOST_SCALARS=0 produces byte-identical dumps on
/// EVERY shape (the S3 A/B), which is what turns the crossing count from 3.00 per hop to 0.
///
/// \note Why the edge could not simply be handed to the existing device path. The edge rides in the
/// STANDARD group's slots (strides L_std / nout_std) while its block order is L_e / nout_e, so its
/// slot is OVERSIZED, and run_engine_blocks() only reports the slots as filled when st.L == L. The
/// pads - A to blockdiag(A, I) and R_hp to zero outside [0,nout)x[0,L) - were therefore written by
/// stage_engine_group(), which skips the whole staging when the slots are filled. They are geometry,
/// not matrix data, so they move into build_slots_on_device() and the host stays out of the matrix
/// business.
/// \brief A/B for the SECOND correlation prefix of a merged batch (OCUDU_CE_EDGE_FUSE).
///
/// Zero (the default, and what this line ships): the edge group's correlation is built by the device
/// in a command buffer of its OWN (mmse_engine::build_correlation(), the form S4 introduced), and the
/// merged hop's engine call then runs in a second command buffer.
///
/// One: the same kernels, over the same slots, encoded as the merged batch's second corr prefix -
/// inside the engine's own command buffer, before K1 (see encode_run's corr_edge). That is the fused
/// form the `gpu` mode's second clause asks for: one command buffer per hop instead of two, and one
/// host commit/wait pair less. Measured on air, the standalone form costs the lane 0.28 command
/// buffers per hop (cbs/lane 3.00 -> 3.28).
///
/// It is an A/B rather than a default because the two forms must produce BYTE-IDENTICAL dumps, and
/// the gate that says so has to be able to see both: a fused form that silently computes something
/// else would otherwise be indistinguishable from a win. \c k0d is the analogue for the standard
/// group's prefix.
bool edge_fuse_enabled()
{
  static const bool value = []() {
    const char* env = std::getenv("OCUDU_CE_EDGE_FUSE");
    return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
  }();
  return value;
}

bool edge_build_on_device()
{
  static const bool value = []() {
    const char* env = std::getenv("OCUDU_CE_TAIL_DEV");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();
  return value;
}

bool host_reads_device_scalars()
{
  static const bool value = []() {
    const char* env = std::getenv("OCUDU_CE_HOST_SCALARS");
    return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
  }();
  return value;
}

/// \brief Slots of the estimator's sigma2 buffer (gpu_ls_sigma2), all written by the extraction's own
/// command buffer: the noise variance, the pilots' power sum, their ratio and the mean the ratio is
/// derived from (see mmse_pilots_power).
enum sigma2_slot : unsigned {
  /// Classical noise variance of the hop (S-7f-5w).
  kSigma2 = 0,
  /// Sum of |LS pilot|^2 over the hop: the host divides it by nof_power_pilots.
  kPowerSum = 1,
  /// sigma2 / max(kPowerSum / nof_power_pilots, 1e-30F) - the diagonal loading A wants. Handing this
  /// slot to the kernels (corr_stage::sigma2_slot) is what keeps the host out of the received-grid ->
  /// A chain; it is the host's own float because ocudu_mmse_pilots.metal is compiled with
  /// -fno-fast-math (see k0a_ratio_from_device_enabled()).
  kRatioSlot = 2,
  /// kPowerSum / nof_power_pilots: the mean the ratio is derived from, kept so that a discrepancy
  /// between the two quotients could be attributed to an operand instead of guessed at.
  kPowerMean = 3,
  /// Capacity. The engine wraps the buffer with this length, so every slot above must be below it.
  kSigma2Slots = 4,
};
static_assert(kRatioSlot > 0, "The ratio slot must not be 0: 0 is corr_stage::sigma2_slot's 'host value'.");
static_assert(kSigma2Slots > kPowerMean, "Every sigma2 slot must fit in the buffer the engine wraps.");

/// \brief How many of those blocks the buffer holds, i.e. how many hops' scalars can be outstanding at
/// once. The same reason as the CFO's rotating slot (see kCfoSlots in the header), and the same count:
/// a pooled estimator instance is handed to a later hop while the lane of the hop it just submitted is
/// still in flight - the estimator's own code says so, it completes the previous batch at the START of
/// the next hop because "it reads the very staging slots this hop is about to overwrite". The
/// correlation stage reads this block from the WEIGHTS command buffer, one command buffer after the
/// extraction wrote it, so with a single block a later hop's extraction overwrites it first.
static constexpr unsigned kSigma2Blocks = 8;
static_assert((kSigma2Blocks & (kSigma2Blocks - 1)) == 0, "the block rotation assumes a power of two");

/// OCUDU_CE_LS_CHECK=1 compares the device's least-squares pilots against the host's, so both have
/// to exist: it needs the host pre-stage (see stage_produces_ls_pilots()).
bool ls_check_enabled()
{
  static const bool value = (std::getenv("OCUDU_CE_LS_CHECK") != nullptr);
  return value;
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

  // The goal of the gpu pipeline mode is a crossing count; this is where the crossings that are left
  // are counted and reported (see phy_pipeline_crossings.h). Registered here because this estimator
  // owns the reads: one per concurrent PUSCH thread, and the registration is idempotent.
  register_phy_pipeline_crossing_check();

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
  gpu_ls_ref    = alloc_aligned<float>(k_ls_floats);
  gpu_ls_out    = alloc_aligned<float>(k_ls_floats);
  // The hop's CFO, in kCfoSlots rotating slots (see the member's note): the extraction writes the
  // current one, the noise reformat reads it back on the device, and the slot outlives both because a
  // later hop on this same pooled instance must not overwrite it before that read happens.
  gpu_ls_cfo    = alloc_aligned<float>(kCfoSlots);
  // S-7f-5w: the frequency-smoothed copy of the hop's pilots and the noise variance the device
  // leaves behind. Both are read in the SAME command buffer that produces the LSE.
  gpu_ls_smoothed = alloc_aligned<float>(k_ls_floats);
  // Four floats: [0] the noise variance, [1] the sum of the hop's |LS pilot|^2, [2] the ratio
  // sigma2 / max([1] / nof_power_pilots, 1e-30F) and [3] the mean [1] / nof_power_pilots it is derived
  // from - all four written by the extraction's own command buffer (mmse_pilots_power). The host
  // derives the ratio it uses from [0] and [1]; the device may load A's diagonal straight from
  // kRatioSlot, which is the same float as long as ocudu_mmse_pilots.metal keeps -fno-fast-math.
  gpu_ls_sigma2   = alloc_aligned<float>(kSigma2Blocks * kSigma2Slots);

  // Glue #2 (S-7f-5u): the DEVICE writes the engine's pilot vectors y out of the pilots it just
  // produced (gpu_ls_out), which removes the host's copy of them into the y slots - the last CPU
  // step between K0-a and the weights. DEFAULT ON, like the device inversion and the device LSE:
  // the scatter only re-indexes and applies the same inv_beta product the host applies, so the
  // published output is byte-identical either way, which is what OCUDU_CE_DEV_Y=0 (the host
  // staging) is for: it is the A/B that proves the device writer (capture_gates.sh ydev).
  static const bool device_y_default_on = []() {
    const char* env = std::getenv("OCUDU_CE_DEV_Y");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();
  device_y_enabled = device_y_default_on;

  // S-7f-5w: the device computes the hop's noise variance (FD smoothing + the classical estimator)
  // inside the extraction's command buffer, and the host reads the scalar it leaves. ON by default
  // like the other device stages; OCUDU_CE_DEV_SIGMA2=0 keeps estimate_sigma2() on the host, which
  // is the A/B of the sig2 gate. It is a TOLERANCE path (see ocudu_mmse_pilots.metal): the value
  // enters A's diagonal with a weight of ~1e-3, so the two computations are not bit-identical.
  static const bool device_sigma2_default_on = []() {
    const char* env = std::getenv("OCUDU_CE_DEV_SIGMA2");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();
  device_sigma2_enabled = device_sigma2_default_on;

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
  free_aligned(gpu_ls_ref);
  free_aligned(gpu_ls_out);
  free_aligned(gpu_ls_cfo);
  free_aligned(gpu_ls_smoothed);
  free_aligned(gpu_ls_sigma2);
}

unsigned port_channel_estimator_metal_mmse_impl::stage_device_noise_inputs(const fd_td_estimation_stage_args& args,
                                                                             unsigned                           npt)
{
  const unsigned npf         = args.nof_symbol_pilots;
  const unsigned nof_cdm_hop = args.rx_pilots.size().nof_slices;
  // \note The caps are the BUFFERS' (gpu_rx_pilots is allocated for MAX_LAYERS / 2 CDM groups), and
  //       they are a superset of K4's own gate - which now keys on this call's answer instead of
  //       re-deriving it, so a geometry that does not fit cannot leave K4 reading unstaged pilots.
  if ((npf == 0) || (nof_cdm_hop == 0) || (npt == 0) || (npt > MAX_DMRS_SYMBOLS) ||
      (nof_cdm_hop > MAX_LAYERS / 2) || (npf > MAX_NOF_PILOTS_SYMBOL)) {
    return 0;
  }
  for (unsigned i_dmrs = 0; i_dmrs != npt; ++i_dmrs) {
    for (unsigned i_group = 0; i_group != nof_cdm_hop; ++i_group) {
      span<const cf_t> src = args.rx_pilots.get_symbol(i_dmrs, i_group);
      float* dst = gpu_rx_pilots + ((static_cast<std::size_t>(i_dmrs) * nof_cdm_hop + i_group) * npf) * 2;
      for (unsigned j = 0; j != npf; ++j) {
        dst[2 * j]     = src[j].real();
        dst[2 * j + 1] = src[j].imag();
      }
    }
  }
  // Symbol start times: the CFO phasors of both the noise reduction and K4 read them.
  for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
    gpu_epochs[sym] = (sym < args.symbol_start_epochs.size()) ? args.symbol_start_epochs[sym] : 0.0F;
  }
  return nof_cdm_hop;
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
  // domain. ls_pilot() applies that scaling per element, from whichever side built the pilots.
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned i_symbol = 0; i_symbol != nof_dmrs_symbols; ++i_symbol) {
      span<cf_t> dst = tmp_lse.get_symbol(i_symbol, i_layer);
      for (unsigned j = 0; j != dst.size(); ++j) {
        dst[j] = ls_pilot(args, i_symbol, i_layer, j, /*scaled=*/true);
      }
      apply_fd_smoothing(tmp_filtered_enlarged.get_symbol(i_symbol, i_layer),
                         tmp_lse_enlarged.get_symbol(i_symbol, i_layer),
                         nof_prb,
                         stride,
                         port_channel_estimator_fd_smoothing_strategy::filter);
    }
  }

  // \brief CFO the residual below is rotated with: the one that belongs to the LSE being smoothed.
  //
  // K0-a OVERWRITES pilots_lse_view with the device's own LSE and applies the DEVICE's CFO to it in
  // that same command buffer, so smoothing that buffer and then rotating the residual with the host's
  // estimate (args.cfo_hop) mixes two phase ramps. That is not a sigma2 difference - it is the
  // difference between the two CFO estimators - and on a 4-layer capture whose estimates differ by
  // 5e-3 it made the device disagree with this reference by 1.4e-02, while agreeing to 1.3e-07 once
  // the kernel's own CFO was used. On a host-built LSE the host's estimate is the matching one.
  if (device_ls_valid && host_reads_device_scalars()) {
    phy_pipeline_crossings::count_host_read(); // CROSSING: device-produced, read by the fallback path.
  }
  const std::optional<float> cfo_ref = (device_ls_valid && host_reads_device_scalars())
                                           ? std::optional<float>(gpu_ls_cfo[cfo_slot_])
                                           : args.cfo_hop;

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
                                                   cfo_ref,
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
  // S-7g-20 (K0-a fusion), first half: A's diagonal is loaded from the extraction's own command buffer
  // output instead of from \c stats.sigma2 (see device_sigma2_rel and k0a_ratio_from_device_enabled()).
  // The BASE goes over, with the slot the ratio lives in - the kernel indexes the base, so passing the
  // element's own address would read past the buffer (that mistake loaded A with no noise at all).
  // \c c.sigma2 still carries the HOST's ratio, so every caller that reads it - the matrix flavor's
  // host build, the merged edge block, a fallback staging - keeps using the host's float, which is the
  // value that must not move.
  c.sigma2_dev  = device_sigma2_rel;
  c.sigma2_slot = kRatioSlot;
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

port_channel_estimator_metal_mmse_impl::ls_geometry
port_channel_estimator_metal_mmse_impl::ls_geometry_of(const fd_td_estimation_stage_args& args) const
{
  ls_geometry geom;

  const unsigned nof_layers = args.dmrs_patterns.size();
  if ((nof_layers == 0) || (nof_layers > MAX_LAYERS)) {
    return geom;
  }

  const auto& hop_rb_mask =
      (args.hop == 0) ? args.dmrs_patterns.front().rb_mask : args.dmrs_patterns.front().rb_mask2;
  const unsigned nof_prb = hop_rb_mask.count();
  const unsigned comb    = args.dmrs_patterns.front().re_pattern.count();
  geom.nof_prb           = nof_prb;
  geom.ncomb             = comb;
  geom.nof_pilots        = nof_prb * comb;

  // DM-RS symbols of the hop, counted exactly like the stage's own loop.
  unsigned npt = 0;
  args.pattern_symbols.for_each(args.first_symbol, args.last_symbol, [&](unsigned) { ++npt; });

  // A contiguous allocation only: the kernels index the hop as first_prb + k/ncomb, which is the
  // same restriction the device gather of the equalizer works under. The rest is the kernels'
  // compile-time contract and the staging buffers' capacity.
  const bool contiguous =
      (nof_prb != 0) && ((hop_rb_mask.find_highest() + 1 - hop_rb_mask.find_lowest()) == nof_prb);
  geom.ok = (npt != 0) && (npt == args.nof_dmrs_symbols) && contiguous && (comb != 0) &&
            (geom.nof_pilots == args.nof_symbol_pilots) && (geom.nof_pilots <= MAX_NOF_PILOTS_SYMBOL) &&
            args.grid.get_device_view().is_valid();
  return geom;
}

bool port_channel_estimator_metal_mmse_impl::stage_produces_ls_pilots(const fd_td_estimation_stage_args& args) const
{
  if (!device_ls_enabled() || ls_check_enabled() || !engine_ready) {
    return false;
  }
  return ls_geometry_of(args).ok;
}

cf_t port_channel_estimator_metal_mmse_impl::ls_pilot(const fd_td_estimation_stage_args& args,
                                                     unsigned                        i_symbol,
                                                     unsigned                        i_layer,
                                                     unsigned                        j,
                                                     bool                            scaled) const
{
  const float scale = scaled ? (1.0F / args.beta_scaling) : 1.0F;

  if (device_ls_valid) {
    // [symbol][layer][pilot], real/imag interleaved, nof_symbol_pilots per (symbol, layer) - the
    // layout K0-a staged (its gate makes its nof_pilots == args.nof_symbol_pilots).
    const float* p = gpu_ls_out +
                     ((static_cast<std::size_t>(i_symbol) * args.dmrs_patterns.size() + i_layer) *
                          args.nof_symbol_pilots +
                      j) * 2;
    return cf_t(p[0] * scale, p[1] * scale);
  }

  // No device result for this hop: the host pre-stage filled pilots_lse_view, in the received domain.
  cf_t v = args.pilots_lse_view.get_symbol(i_symbol, i_layer)[j];
  return scaled ? v * scale : v;
}

metal::ce_lane_order port_channel_estimator_metal_mmse_impl::ce_lane_order_from_env()
{
  // DEFAULT event (S-7g-19, Step 1'): the estimator's deferred hop commits its own command buffer as soon
  // as its dispatches are encoded, and the lane burst waits for it through the back-end stage fence. That
  // is the fusion the goal asks for - nothing on the host waits in the middle of the lane any more - while
  // the estimator's GPU work still overlaps the host encoding the equalization and the demapping.
  //
  // The two other orders are the escape hatches, and they are what the earlier legs measured:
  //   * host_wait - the estimator's own command buffer, waited for by the host right after the commit. This
  //     was the route until S-7g-16 and it is why the lane used to pay ~125us of [ul_equalization_demod]:
  //     the host sat waiting for the estimator instead of encoding the equalization;
  //   * burst - the estimator's dispatches ride the lane's shared command buffer (S-7g-16, Step 1b). One
  //     submission for the whole lane, but the estimator cannot start before the group is fully encoded,
  //     which cost the same ~125us as a LATENCY DEBT (the design document's 48.188(i).3 called for paying
  //     it back; this order is where that happened).
  // All three are byte-identical by construction (the estimator's unit test compares them), so the choice
  // is purely "how much of the estimator's GPU work overlaps the host's encoding".
  const char* order = std::getenv("OCUDU_CE_LANE_ORDER");
  if (order != nullptr) {
    const std::string value(order);
    if (value == "event") {
      return metal::ce_lane_order::event;
    }
    if ((value == "wait") || (value == "host_wait")) {
      return metal::ce_lane_order::host_wait;
    }
    if (value == "burst") {
      return metal::ce_lane_order::burst;
    }
    // A typo must not silently select a route: say so once, loudly, and run the default.
    static const bool warned = []() {
      ocudulog::fetch_basic_logger("PHY").error(
          "PUSCH: unknown OCUDU_CE_LANE_ORDER value (expected event, wait or burst) - using event");
      return true;
    }();
    (void)warned;
    return metal::ce_lane_order::event;
  }
  // Deprecated numeric alias of the S-7g-16 knob, kept because the legs, the A/B script and the design
  // document refer to it: 1 meant "the estimator's dispatches ride the lane burst", 0 "the estimator keeps
  // its own command buffer". Read ONLY when the new name is unset, so a leg can set either one.
  if (const char* legacy = std::getenv("OCUDU_CE_FUSED_BURST"); legacy != nullptr) {
    return (std::strtoul(legacy, nullptr, 10) != 0) ? metal::ce_lane_order::burst : metal::ce_lane_order::host_wait;
  }
  return metal::ce_lane_order::event;
}

void port_channel_estimator_metal_mmse_impl::apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args)
{
  // Diagnostics (see ocudu_metal_lane_clock.h): the earliest host reading of this lane. Paired with
  // the shared "front end finished" reading and with the extraction's commit, it splits the lane's
  // GPU gap into "the host had not handed the lane over yet" and "the burst waited on the fence".
  metal::lane_clock.mark_stage_entry();
  // ---- S-7g-19 (fused lane), Step 1': WHOSE command buffer this hop's dispatches go into -------------
  // The order is decided per hop and handed to the engine (see ce_lane_order in the engine's header):
  // event commits the estimator's own command buffer early and lets the lane burst wait for it through the
  // back-end stage fence, host_wait waits for it here, burst encodes it into the lane's shared command
  // buffer. The gate is the base class's answer to "will this hop be completed AFTER the rest of the chain
  // has had its turn" (args.deferred); it is FALSE for a hop that completes inside its own submit, where
  // nobody would commit a burst before complete_fd_td_estimation_stage() reads the hop's scalars back.
  //
  // Set on EVERY hop, both ways: the adapter outlives the hop, and an engine left in burst order would
  // silently dispatch a later synchronous hop into a burst nobody owns.
  //
  // Read per hop instead of cached in a static because the A/B has to be switchable inside one process:
  // the Metal estimator's unit test compares the orders on the same input.
  const metal::ce_lane_order order = ce_lane_order_from_env();
  fused_burst_hop                  = args.deferred && (order == metal::ce_lane_order::burst);
  if (engine != nullptr) {
    // A hop the caller left running takes the selected order. A hop that COMPLETES INSIDE ITS OWN SUBMIT
    // never joins the lane's burst, whatever the knob says, and the engine cannot make that call: its
    // asynchronous entry point serves both (the inline route submits without waiting and completes the
    // stage in the same call, see stage_pending), so "which entry point ran" does not answer "may these
    // dispatches live in the lane's burst". Only this adapter knows whether the hop is complete, so the
    // gate is here. Handing a non-deferred hop to the burst leaves its dispatches in a command buffer
    // nobody commits: measured as an all-zero estimator output on every hop of the inline route.
    engine->set_lane_order(args.deferred ? order : metal::ce_lane_order::host_wait);
  }

  const unsigned nof_layers = args.dmrs_patterns.size();
  const auto&    hop_rb_mask =
      (args.hop == 0) ? args.dmrs_patterns.front().rb_mask : args.dmrs_patterns.front().rb_mask2;
  const unsigned nof_prb = hop_rb_mask.count();
  const unsigned scs_khz = scs_to_khz(args.scs);

  // DM-RS slot symbol indices of the current hop (ascending).
  static_vector<unsigned, MAX_NOF_DMRS_SYMBOLS> dmrs_sym;
  args.pattern_symbols.for_each(args.first_symbol, args.last_symbol, [&](unsigned s) { dmrs_sym.push_back(s); });
  const unsigned npt = dmrs_sym.size();

  // Per-hop state of the deferred unpack, set HERE and not in the device-LS branch below: it decides
  // which symbols the completion publishes, which is needed even when the device builds nothing
  // (OCUDU_CE_CPU_LS=1 and the other host routes) - the hop statistics read the same DM-RS pilots
  // either way. The symbols are the DM-RS ones (the statistics' input) plus the whole grid when the
  // slot hops, where hop 0's estimates must be out before hop 1's batch overwrites the device
  // buffers they would be read from (see complete_fd_td_estimation_stage()).
  unpack_npt     = npt;
  unpack_hopping = args.dmrs_patterns.front().hopping_symbol_index.has_value();
  for (unsigned k = 0; k != npt; ++k) {
    unpack_dmrs_sym[k] = dmrs_sym[k];
  }
  // This hop's batches are about to overwrite the ones the previous hop's host grid would be
  // materialized from: drop that pending work (a hopping hop published it at its completion).
  host_grid_pending = false;
  nof_host_unpacks  = 0;

  // ---- S-7f-5w: the arrays the device noise variance reads ---------------------------------------
  // Staged BEFORE the extraction, because the noise reduction rides that same command buffer; K4
  // reads the very same buffers later, and keys its own gate on this call's answer.
  const unsigned staged_cdm_groups = stage_device_noise_inputs(args, npt);

  // ---- K0-a: the estimator's INPUT stage, on the device --------------------------------
  // The pilots are recomputed HERE, on the device, when the hop qualifies, and every host consumer
  // of them reads them where the device left them (see ls_pilot()): nothing is copied back into
  // pilots_lse_view, which is now only the fallback's buffer. The host pre-stage is what this
  // replaces, so it is SKIPPED for those hops (the base class asks stage_produces_ls_pilots(), which
  // reads the same geometry this gate does through ls_geometry_of()); the CFO is then the device's
  // too (see account_hop_cfo() below). The device path is DEFAULT ON, like the device inversion;
  // OCUDU_CE_CPU_LS=1 forces the host pre-stage (the escape hatch, and the A/B for the tolerance
  // probe), and OCUDU_CE_LS_CHECK=1 needs both sides.
  //
  // The per-hop state of the two device-side consumers of this result lives here: the validity flag
  // the y scatter (glue #2) is gated on, and the descriptors the staging records. Both are reset on
  // EVERY hop, before K0-a decides, so a hop that fails K0-a cannot inherit the previous hop's
  // descriptors - which would make the engine write y from a buffer that no longer holds its pilots.
  device_ls_valid     = false;
  device_sigma2_valid = false;
  // S-7g-20: and the device ratio of the previous hop, for the same reason - it addresses the
  // extraction's own output slot, which this hop has not filled yet.
  device_sigma2_rel   = nullptr;
  nof_device_y_stage  = 0;
  const ls_geometry geom = ls_geometry_of(args);
  if (device_ls_enabled() && geom.ok) {
    const resource_grid_device_view dv         = args.grid.get_device_view();
    const unsigned                  comb       = geom.ncomb;
    const unsigned                  nof_pilots = geom.nof_pilots;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned i_symb = 0; i_symb != npt; ++i_symb) {
          span<const cf_t> src = args.pilots.get_symbol(args.hop_offset + i_symb, i_layer);
          float*           dst = gpu_ls_ref + (static_cast<std::size_t>(i_symb) * nof_layers + i_layer) * nof_pilots * 2;
          for (unsigned j = 0; j != nof_pilots; ++j) {
            dst[2 * j]     = src[j].real();
            dst[2 * j + 1] = src[j].imag();
          }
        }
      }
      for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
        gpu_epochs[sym] = (sym < args.symbol_start_epochs.size()) ? args.symbol_start_epochs[sym] : 0.0F;
      }

      // The raised-cosine coefficients of the FD smoothing: they depend on the hop's geometry only, so
      // the host hands them over (with how many virtual pilots the edges take, including
      // apply_fd_smoothing()'s nof_rb == 1 special case).
      const unsigned stride = configure_interpolator(args.dmrs_patterns.front().re_pattern).stride;
      fd_filter_len         = get_fd_smoothing_filter(span<float>(fd_filter), nof_prb, stride);
      unsigned nof_v_pilots = std::min<unsigned>(MAX_V_PILOTS, fd_filter_len / 2);
      if (nof_prb == 1) {
        nof_v_pilots = nof_pilots;
      }

      // ---- This hop's CFO slot (see kCfoSlots in the header) --------------------------------------
      //
      // Advance the rotation and carry the previous slot's value forward, BOTH on the host and BOTH
      // before the extraction is submitted: the kernel writes this slot only when the hop has two or
      // more DM-RS symbols (mmse_pilots_cfo returns early otherwise), so a hop that cannot estimate a
      // CFO must find the last one that could - which is precisely what the single buffer this
      // replaces held, and what the copy reproduces deterministically instead of implicitly.
      cfo_slot_                   = (cfo_slot_ + 1) % kCfoSlots;
      gpu_ls_cfo[cfo_slot_]       = gpu_ls_cfo[(cfo_slot_ + kCfoSlots - 1) % kCfoSlots];
      // The sigma2 block rotates with it, and for the same reason (see kSigma2Blocks). Unlike the CFO
      // it needs no carry-forward: every slot of the block is written unconditionally when the stage
      // runs at all, and when it does not the caller is told (pilots_stage::sigma2_done) and reads
      // neither the host's copy nor the device's.
      sigma2_base_                = (sigma2_base_ + kSigma2Slots) % (kSigma2Blocks * kSigma2Slots);

      metal::mmse_engine::pilots_stage st{};
      // The engine skips the noise-variance stage - leaving the destination untouched - for a geometry
      // outside the kernels' contract, and the build still succeeds: see pilots_stage::sigma2_done.
      bool sigma2_done = false;
      st.sigma2_done   = &sigma2_done;
      st.grid              = dv.base;
      st.grid_bytes        = (static_cast<std::size_t>(dv.nof_ports - 1) * dv.port_stride +
                       static_cast<std::size_t>(dv.nof_symb - 1) * dv.symb_stride +
                       static_cast<std::size_t>(dv.nof_subc - 1) * dv.subc_stride + 1) * sizeof(cbf16_t);
      st.grid_subc_stride  = dv.subc_stride;
      st.grid_symb_stride  = dv.symb_stride;
      st.grid_port_stride  = dv.port_stride;
      st.ref               = gpu_ls_ref;
      st.buf_bytes         = k_ls_floats * sizeof(float);
      st.epochs            = gpu_epochs;
      st.lse               = gpu_ls_out;
      st.cfo               = &gpu_ls_cfo[cfo_slot_];
      // S-7f-5w: the noise variance of this hop, computed in this same command buffer when the host
      // is not the one computing it (OCUDU_CE_DEV_SIGMA2=0).
      st.rx_pilots         = gpu_rx_pilots;
      st.rx_bytes          = 2 * static_cast<std::size_t>(MAX_DMRS_SYMBOLS) * (MAX_LAYERS / 2) *
                             MAX_NOF_PILOTS_SYMBOL * sizeof(float);
      st.smoothed          = gpu_ls_smoothed;
      st.sigma2            = device_sigma2_enabled ? (gpu_ls_sigma2 + sigma2_base_) : nullptr;
      // S-7g-20: the divisor of that stage's mean power, i.e. the host's own nof_power_pilots - the
      // kernel turns it into out[3] (the mean) and out[2] (the device's ratio; see
      // mmse_pilots_power). Only meaningful when the sigma2 block runs at all, and 0 is its "neither
      // wanted" value.
      st.nof_power_pilots  = (st.sigma2 != nullptr)
                                 ? static_cast<unsigned>(static_cast<std::size_t>(nof_layers) *
                                                         args.nof_dmrs_symbols * args.nof_symbol_pilots)
                                 : 0;
      st.fd_filter         = fd_filter.data();
      st.fd_filter_bytes   = sizeof(fd_filter);
      st.fd_filter_len     = fd_filter_len;
      st.nof_v_pilots      = nof_v_pilots;
      st.nof_cdm           = staged_cdm_groups;
      st.beta              = args.beta_scaling;
      st.inv_beta          = 1.0F / args.beta_scaling;
      st.compensate_cfo    = args.compensate_cfo_flag;
      st.nof_dmrs_symb     = npt;
      st.nof_layers        = nof_layers;
      st.nof_pilots        = nof_pilots;
      st.ncomb             = comb;
      st.nof_prb           = nof_prb;
      st.first_prb         = hop_rb_mask.find_lowest();
      st.port              = args.port;
      for (unsigned k = 0; k != npt; ++k) {
        st.dmrs_symb[k] = dmrs_sym[k];
      }
      {
        unsigned n = 0;
        const auto& re_pattern = args.dmrs_patterns.front().re_pattern;
        for (unsigned pos = 0; (pos != NOF_SUBCARRIERS_PER_RB) && (n != comb); ++pos) {
          if (re_pattern.test(pos)) {
            st.pilot_re[n++] = pos;
          }
        }
      }

      if (engine->build_pilots_lse(st)) {
        // K0-a produced THIS hop's pilots: from here on the device may also write the engine's
        // pilot vectors out of them (glue #2, see record_device_y_stage()).
        device_ls_valid = true;
        // One device hop, so the crossing total above can be read per hop (see phy_pipeline_crossings).
        phy_pipeline_crossings::count_device_hop();
        // The CFO that goes with those pilots comes from the device too: the host pre-stage that
        // estimated it did not run for this hop. It is the rotation the statistics have to use with
        // the device's filtered pilots (see account_hop_cfo()), what the caller reports, and - as
        // args.cfo_hop - what the noise reformat below compensates when it reduces the variance the
        // equalizer reads. The kernel reproduces the host's estimate bit for bit (measured: both are
        // the same float on every capture tried), but the host value is not available here.
        // CROSSING: the device produced this scalar in the extraction's command buffer; taking it
        // back to the host is one of the legs the fused lane is supposed to remove. The A/B
        // (OCUDU_CE_HOST_SCALARS=0) reports it as "not measured" instead of reading it, which is what
        // the mode's own accounting would do - see host_reads_device_scalars().
        if (host_reads_device_scalars()) {
          phy_pipeline_crossings::count_host_read();
          args.cfo_hop = std::optional<float>(gpu_ls_cfo[cfo_slot_]);
        } else {
          args.cfo_hop = std::nullopt;
        }
        account_hop_cfo(args.cfo_hop);
        // S-7f-5w: and it computed this hop's noise variance, in the command buffer that just
        // completed - so the scalar is valid now, with no extra synchronisation. NOT "sigma2 != nullptr":
        // that pointer stays non-null when the engine skipped the stage, and the buffer then holds the
        // previous hop's value (or nothing). The engine reports it through sigma2_done.
        device_sigma2_valid = (st.sigma2 != nullptr) && sigma2_done;
        // Tolerance probe (OCUDU_CE_LS_CHECK=1): the device LSE against the host's, BEFORE the
        // overwrite. It is what makes stage_produces_ls_pilots() keep the host pre-stage when it is
        // on. Tolerance, not bit-exactness: the pilots enter h = W . y linearly, so a relative error
        // carries no amplification factor (see ocudu_mmse_pilots.metal).
        if (ls_check_enabled()) {
          double   max_rel = 0.0;
          unsigned nof_bad = 0;
          for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
            for (unsigned i_symb = 0; i_symb != npt; ++i_symb) {
              span<const cf_t> ref_lse = args.pilots_lse_view.get_symbol(i_symb, i_layer);
              const float*     d = gpu_ls_out + (static_cast<std::size_t>(i_symb) * nof_layers + i_layer) * nof_pilots * 2;
              for (unsigned j = 0; j != nof_pilots; ++j) {
                const double dr = static_cast<double>(d[2 * j]) - ref_lse[j].real();
                const double di = static_cast<double>(d[2 * j + 1]) - ref_lse[j].imag();
                const double mag = static_cast<double>(std::abs(ref_lse[j]));
                const double rel = (mag > 1e-12) ? std::sqrt(dr * dr + di * di) / mag : std::sqrt(dr * dr + di * di);
                if (rel > 1e-5) {
                  ++nof_bad;
                }
                max_rel = std::max(max_rel, rel);
              }
            }
          }
          std::fprintf(stderr,
                       "[ls_check] L=%u symb=%u pilots=%u layers=%u max_rel=%.3e bad(>1e-5)=%u cfo=%g host_cfo=%g\n",
                       nof_pilots,
                       npt,
                       nof_pilots,
                       nof_layers,
                       max_rel,
                       nof_bad,
                       static_cast<double>(gpu_ls_cfo[cfo_slot_]),
                       args.cfo_hop.has_value() ? static_cast<double>(*args.cfo_hop) : 0.0);
          // Per-symbol/per-pilot detail: a small relative error on EVERY pilot is the signature of a
          // neighbouring-subcarrier read (adjacent channel values are similar), while a rotation-like
          // error points at the CFO phasors. Printed for the first few pilots of each symbol.
          for (unsigned i_symb = 0; i_symb != npt; ++i_symb) {
            span<const cf_t> ref_lse = args.pilots_lse_view.get_symbol(i_symb, 0);
            const float*     d = gpu_ls_out + static_cast<std::size_t>(i_symb) * nof_layers * nof_pilots * 2;
            std::fprintf(stderr,
                         "[ls_sym] symb=%u slot_sym=%u ep=%.6g dev[0]=(%.6g,%.6g) host[0]=(%.6g,%.6g) dev[1]=(%.6g,%.6g) "
                         "host[1]=(%.6g,%.6g)\n",
                         i_symb,
                         dmrs_sym[i_symb],
                         static_cast<double>(gpu_epochs[dmrs_sym[i_symb]]),
                         static_cast<double>(d[0]),
                         static_cast<double>(d[1]),
                         static_cast<double>(ref_lse[0].real()),
                         static_cast<double>(ref_lse[0].imag()),
                         static_cast<double>(d[2]),
                         static_cast<double>(d[3]),
                         static_cast<double>(ref_lse[1].real()),
                         static_cast<double>(ref_lse[1].imag()));
          }
        }

        // The device result is NOT copied back: the host consumers read it where the device left it
        // (see ls_pilot()). What used to be here was a full copy of the hop into pilots_lse_view
        // plus an in-place 1/beta scaling of it, for readers that each need the domain they need.
      } else {
        // Cold path: this hop qualified for the device build, so the base class SKIPPED the host
        // pre-stage (stage_produces_ls_pilots()) and the least-squares pilots and the CFO have to
        // come from the host now - exactly as if the hop had not qualified. It cannot run every
        // hop: the answer above must not depend on this call's outcome.
        logger.warning("[mmse_ce] device LSE build failed: running the host pre-stage for this hop");
        std::optional<float> host_cfo = run_ls_pre_stage(args);
        account_hop_cfo(host_cfo);
        args.cfo_hop = host_cfo;
      }
  }

  // Per-phase timing (compile-time debug aid, ENABLE_CE_TIME=ON defines OCUDU_CE_TIME):
  // sigma2 / corr-build / GPU / CPU-blocks / finish, printed through the [mmse_time]
  // debug line. The measurements below are recorded unconditionally; only the reporting block at
  // the end of this function is compiled out when the probe is disabled.
#if defined(OCUDU_CE_TIME)
  using steady_clock = std::chrono::steady_clock;
  const auto t_begin = steady_clock::now();
#endif

  // Mean power of the received DM-RS pilots, in the RECEIVED domain: the classical noise estimator
  // returns the residual in that domain, so this is the reference that turns sigma2 into the
  // noise-to-signal ratio the unit-normalized correlation model needs. It is a reduction over the
  // device's own pilots when the device built them (no copy, see ls_pilot()).
  const size_t nof_power_pilots =
      static_cast<size_t>(nof_layers) * args.nof_dmrs_symbols * args.nof_symbol_pilots;
  float pilots_power = 0.0F;
  if (device_sigma2_valid && device_sigma2_enabled && (nof_power_pilots != 0)) {
    // The device reduced the pilots it extracted (mmse_pilots_power rides the sigma2 block), so the
    // host turns the sum into the mean instead of reading every pilot back: the loop below is the
    // fallback for the routes without a device reduction (no device LSE, OCUDU_CE_DEV_SIGMA2=0, or a
    // metallib without the kernel), and the tolerance probe's reference.
    if (host_reads_device_scalars()) {
      phy_pipeline_crossings::count_host_read(); // CROSSING: device-produced (mmse_pilots_power's out[1]).
      pilots_power = gpu_ls_sigma2[sigma2_base_ + kPowerSum] / static_cast<float>(nof_power_pilots);
    }
  }
  if (!(device_sigma2_valid && device_sigma2_enabled) || (std::getenv("OCUDU_CE_PP_CHECK") != nullptr)) {
    float host_pilots_power = 0.0F;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned i_symbol = 0; i_symbol != args.nof_dmrs_symbols; ++i_symbol) {
        for (unsigned j = 0; j != args.nof_symbol_pilots; ++j) {
          host_pilots_power += std::norm(ls_pilot(args, i_symbol, i_layer, j, /*scaled=*/false));
        }
      }
    }
    host_pilots_power =
        (nof_power_pilots == 0) ? 0.0F : host_pilots_power / static_cast<float>(nof_power_pilots);
    if (std::getenv("OCUDU_CE_PP_CHECK") != nullptr) {
      // Tolerance probe (like OCUDU_CE_SIGMA2_CHECK): the two reductions differ only in their
      // summation order, so the difference is expected in the last bits, not in kind.
      const double rel = (host_pilots_power != 0.0F)
                             ? (static_cast<double>(pilots_power) - host_pilots_power) / host_pilots_power
                             : 0.0;
      std::fprintf(stderr,
                   "[pp_check] dev=%.9e host=%.9e rel=%.3e device=%d\n",
                   static_cast<double>(pilots_power),
                   static_cast<double>(host_pilots_power),
                   rel,
                   (device_sigma2_valid && device_sigma2_enabled) ? 1 : 0);
    }
    pilots_power = host_pilots_power;
  }

  // The DATA domain is what the estimator publishes (and what the equalizer and the demapper
  // expect): the classical FD stage scales the least-squares pilots by 1 / beta, and so does the
  // device's own y scatter (see pilots_stage::inv_beta). Skipping the scaling made every channel
  // estimate 1 / beta too small whenever the PUSCH processor sets a scaling other than one - which
  // it always does in a real cell (0.708 for two CDM groups without data, the configuration this
  // cell runs) and never does in a lab test that leaves the CDM group count at one.
  //
  // It is applied by the consumers, per element, through ls_pilot(..., /*scaled=*/true): the hop is
  // no longer scaled in place here, because after S-7f-5z the device's own pilots are the source
  // and the host buffer is only the fallback's.

  // Classical noise variance: computed by the DEVICE inside the extraction's command buffer when
  // that path ran (S-7f-5w), by the host otherwise (OCUDU_CE_DEV_SIGMA2=0, no device LSE, or a
  // metallib without the kernels - in which case this is also the CPU-block fallback's value).
  // A/B (OCUDU_CE_HOST_SCALARS=0): a fixed zero, NOT an uninitialised read - the run has to stay
  // deterministic or the dump comparison stops meaning anything.
  const bool take_device_sigma2 = device_sigma2_valid && device_sigma2_enabled && host_reads_device_scalars();
  if (take_device_sigma2) {
    phy_pipeline_crossings::count_host_read(); // CROSSING: device-produced (out[0]).
  }
  float sigma2 = take_device_sigma2 ? gpu_ls_sigma2[sigma2_base_ + kSigma2]
                 : (device_sigma2_valid && device_sigma2_enabled) ? 0.0F
                                                                  : estimate_sigma2(args);
  if (std::getenv("OCUDU_CE_SIGMA2_CHECK") != nullptr) {
    // Tolerance probe: sigma2 enters A's diagonal with a weight of ~1e-3, so the two are expected to
    // differ in the last bits, not in kind (see ocudu_mmse_pilots.metal).
    const float host_sigma2 = estimate_sigma2(args);
    const double rel        = (host_sigma2 != 0.0F) ? (static_cast<double>(sigma2) - host_sigma2) / host_sigma2 : 0.0;
    std::fprintf(stderr,
                 "[sigma2_check] dev=%g host=%g rel=%.3e device=%d\n",
                 static_cast<double>(sigma2),
                 static_cast<double>(host_sigma2),
                 rel,
                 device_sigma2_valid ? 1 : 0);
  }
  float sigma2_rel_perturbed = sigma2 / std::max(pilots_power, 1e-30F);
  // TEMPORARY EXPERIMENT: does a relative change of the size a device-side reduction would introduce
  // (tree summation instead of the host's sequential one, ~1e-7 in float) reach the published dumps?
  if (const char* pert = std::getenv("OCUDU_CE_PP_PERTURB"); pert != nullptr) {
    sigma2_rel_perturbed *= (1.0F + static_cast<float>(std::strtod(pert, nullptr)));
  }
  const float sigma2_rel = sigma2_rel_perturbed;
  // S-7g-20 (K0-a fusion), first half: the ratio above is the HOST's, and it stays the host's by
  // default. The device computes its own in the extraction's command buffer (mmse_pilots_power's
  // out[2]) and that value is NOT bit-identical - the Apple GPU's float divide is not correctly
  // rounded (28.4% of 2^20 measured pairs differ, both directions), and A's diagonal loading is
  // amplified by cond_2(A) ~ 2e4, so the device quotient flips LLR decisions on every capture of the
  // corpus. See k0a_ratio_from_device_enabled() for the measurement and wip/ab_tol.sh for the gate
  // that refuses it. OCUDU_CE_K0A_RATIO_DEV=1 selects the device route anyway: it is what produced
  // those numbers and what an A/B would use, never a default.
  //
  // Set BEFORE the correlation stage is built and reset on every hop: the pointer addresses the
  // per-hop slot the extraction's command buffer fills, and a hop the extraction skipped must not
  // load A with the previous hop's noise. device_sigma2_valid is exactly "this hop's extraction ran
  // and its sigma2 block completed".
  //
  // OCUDU_CE_PP_PERTURB forces the host route: it perturbs the host's ratio for a sensitivity probe,
  // and a perturbed value exists nowhere on the device.
  const bool sigma2_from_device = device_sigma2_valid && device_sigma2_enabled &&
                                  (nof_power_pilots != 0) && k0a_ratio_from_device_enabled() &&
                                  (std::getenv("OCUDU_CE_PP_PERTURB") == nullptr);
  // The BASE, not the element: the kernel indexes it with corr_stage::sigma2_slot.
  // The base of THIS hop's block, not of the whole buffer: the kernel indexes it with
  // corr_stage::sigma2_slot, which is an offset within one block.
  device_sigma2_rel = sigma2_from_device ? (gpu_ls_sigma2 + sigma2_base_) : nullptr;
  // OCUDU_CE_K0A_RATIO_CHECK=1: the two quotients, in bits, with every operand they were computed
  // from - the device's slot against the host's own two operations on the same two scalars. It is
  // what turned "same expression, same operands, so the same float" into the refutation above, and it
  // is the only way to see the device value at all (no host consumer reads it on the device route).
  if (device_sigma2_valid && device_sigma2_enabled && (nof_power_pilots != 0) &&
      (std::getenv("OCUDU_CE_K0A_RATIO_CHECK") != nullptr)) {
    static std::atomic<uint32_t> ratio_checked{0};
    static std::atomic<uint32_t> ratio_mismatch{0};
    const float                  dev = gpu_ls_sigma2[sigma2_base_ + kRatioSlot];
    const uint32_t               n   = ratio_checked.fetch_add(1, std::memory_order_relaxed) + 1U;
    if (dev != sigma2_rel) {
      ratio_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
    // The first hop and then one line per 1000, so a short replay is not silent. Every operand is
    // printed: the HOST's final pilots_power and the two scalars it derives the ratio from, next to
    // the device's own mean and ratio - so a mismatch is attributed to an operand or to the quotient,
    // never guessed at. (A probe that recomputed the host's expression from out[1] instead of reading
    // pilots_power got this wrong once: OCUDU_CE_PP_CHECK above makes them differ on purpose.)
    if ((n == 1U) || ((n % 1000U) == 0U)) {
      const float host_ratio = sigma2 / std::max(pilots_power, 1e-30F);
      const auto  bits       = [](float v) {
        uint32_t u = 0;
        std::memcpy(&u, &v, sizeof(u));
        return u;
      };
      std::fprintf(stderr,
                   "[k0a_ratio] hops=%u mismatch=%u npow=%zu | sum %08x | power host %08x | mean dev "
                   "%08x | sigma2 %08x | ratio host %08x dev %08x (%s)\n",
                   n,
                   ratio_mismatch.load(std::memory_order_relaxed),
                   nof_power_pilots,
                   bits(gpu_ls_sigma2[sigma2_base_ + kPowerSum]),
                   bits(pilots_power),
                   bits(gpu_ls_sigma2[sigma2_base_ + kPowerMean]),
                   bits(gpu_ls_sigma2[sigma2_base_ + kSigma2]),
                   bits(host_ratio),
                   bits(dev),
                   (host_ratio == dev) ? "same" : "DIFF");
    }
  }
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

  // ---- WHO builds the weight matrices of the standard blocks (S-7f-5v) ---------------------------
  // These flags are computed HERE, before the host build they control, because the answer decides
  // whether that build happens at all. They used to be derived after it, which is why the host kept
  // building the standard block's A / R_hp (measured on air: 11.0 us of the 35.3 us per hop) on
  // routes where the DEVICE builds those very slots and the host never reads its own arrays:
  //   - the non-merged batch (a hop without a remainder, or the split form): run_engine_blocks()
  //     hands the slots to the device build and skips its own staging when the strides match;
  //   - the MERGED batch (the default on air, 74% of the hops in the S-7f-5u leg): the device builds
  //     the standard group as a PREFIX of the engine's own command buffer while the host stages only
  //     the edge group. That is new here - see the corr_stage::nof_systems note for why the earlier
  //     attempt at it corrupted the edge block's slots.
  // The host's arrays stay the fallback: they are what stage_engine_group() copies from whenever the
  // slots are NOT device-filled, and the CPU block path rebuilds its own.
  const unsigned comb_std      = args.dmrs_patterns.front().re_pattern.count();
  const unsigned L_std_geom    = npt * block_prb * comb_std;
  const unsigned nout_std_geom = block_prb * NOF_SUBCARRIERS_PER_RB * MAX_NSYMB_PER_SLOT;
  const bool     matrix_on     = engine_ready && use_matrix_engine && matrix_ready;
  const bool     defer         = !matrix_on;
  // Largest tail block order (L = pilots per block) the CPU reference path is known to be fast for
  // (a 36x36 Gauss-Jordan is ~23k FLOPs). Only consulted by the OCUDU_CE_TAIL_CPU A/B knob.
  static constexpr unsigned MAX_CPU_TAIL_ORDER = 36;
  const unsigned            tail_L_est         = rem_prb * 6U * npt;
  const bool                tail_on_cpu        = (rem_prb != 0) && (tail_L_est <= MAX_CPU_TAIL_ORDER) &&
                                 (std::getenv("OCUDU_CE_TAIL_CPU") != nullptr);
  const bool merge_tail = (rem_prb != 0) && (n_std_blocks != 0) && !matrix_on && !tail_on_cpu &&
                          (2 * nof_layers <= MAX_LAYERS) && (std::getenv("OCUDU_CE_SPLIT_TAIL") == nullptr);
  static const bool device_corr_enabled = []() {
    const char* env = std::getenv("OCUDU_CE_CORR_DEV");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();
  // The device fills the standard group's slots on the non-merged routes through run_engine_blocks()
  // (prefix when the device also inverts, standalone build otherwise). Unchanged.
  const bool std_slots_filled = device_corr_enabled && !merge_tail && (n_std_blocks != 0);
  // ... and on the merged route, which needs the device to invert for the same reason every prefix
  // does: with a host inversion the host has to READ the device's A before K1, so the build cannot
  // ride the engine's command buffer.
  const bool gpu_invert_std = !matrix_on && device_inverts(L_std_geom);
  const bool dev_corr_std_merged = device_corr_enabled && merge_tail && gpu_invert_std;
  // Whether the host still builds the standard matrices: only when it also stages them. The matrix
  // flavor always does (its slots are ceil8-padded, so the device's packed block is not the whole
  // slot), and any route without the device build does.
  const bool host_builds_std = !((std_slots_filled && !matrix_on) || dev_corr_std_merged);

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
  // Only for a provider that reads them: the v1 fixed-constants estimator takes the noise variance
  // and nothing else (see channel_statistics_estimator::consumes_pilots()), and these pilots are the
  // device's, so filling them is a per-hop readback that nobody would look at. Measured on air
  // before this guard: layer 0 of every DM-RS symbol copied, for a reader that ignores it.
  if (!stats_estimator->consumes_pilots()) {
    stats_in.pilots_lse = {}; // explicitly "not provided", not "provided and empty"
  } else {
    for (unsigned i_symbol = 0; i_symbol != args.nof_dmrs_symbols; ++i_symbol) {
      // Layer 0 only, in the DATA domain: the correlation model is built from the pilot estimates the
      // estimator publishes, and this is where they were copied from when the device result was
      // published into pilots_lse_view (see ls_pilot()).
      span<cf_t> dst = pilots_span.subspan(i_symbol * args.nof_symbol_pilots, args.nof_symbol_pilots);
      for (unsigned j = 0; j != dst.size(); ++j) {
        dst[j] = ls_pilot(args, i_symbol, 0, j, /*scaled=*/true);
      }
    }
    stats_in.pilots_lse = pilots_span;
  }
  // \note \c stats_in.sigma2 carries the noise-to-pilot-power ratio, not an absolute power: see
  // build_correlation_matrices().
  const channel_statistics stats = stats_estimator->estimate(stats_in);

  // Weight matrices of the standard blocks (shared by all layers in v1). They are built ON THE
  // DEVICE when the routes above say so - the product is analytic, so the host only hands the
  // geometry over and the slots are filled where the inversion and the weights read them (K0-d).
  // Only when the host is the one that will stage these slots does it also build them.
  unsigned                                      L_std    = 0;
  unsigned                                      nout_std = 0;
  std::optional<metal::mmse_engine::corr_stage> std_corr_prefix;
  if (n_std_blocks != 0) {
    if (host_builds_std) {
      build_correlation_matrices(stats,
                                 args.dmrs_patterns.front().re_pattern,
                                 block_prb,
                                 span<const unsigned>(dmrs_sym.begin(), npt),
                                 scs_khz,
                                 span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS),
                                 span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS),
                                 nout_std,
                                 L_std);
    } else if (dev_corr_std_merged) {
      // Merged batch: the device builds the standard group as a prefix of the engine's own command
      // buffer (encoded by engine_run() below), and its geometry comes back from the same helper the
      // non-merged device build uses. The two derivations must agree - the host's arithmetic above
      // against correlation_stage()'s - and an assert is enough here because the k0d gate compares
      // the device-built matrices against the host-built ones byte for byte on every capture.
      unsigned nout_c = 0;
      unsigned L_c    = 0;
      std_corr_prefix = correlation_stage(stats,
                                          args.dmrs_patterns.front().re_pattern,
                                          block_prb,
                                          span<const unsigned>(dmrs_sym.begin(), npt),
                                          scs_khz,
                                          0,
                                          nout_c,
                                          L_c,
                                          L_std_geom,
                                          nout_std_geom);
      // Only THIS group's systems: the edge block occupies the systems after it, in the same slots
      // but as a different geometry (corr_stage::nof_systems).
      std_corr_prefix->nof_systems = nof_layers;
      ocudu_assert((L_c == L_std_geom) && (nout_c == nout_std_geom),
                   "The block geometry must not depend on who derives it.");
      L_std    = L_c;
      nout_std = nout_c;
    } else {
      // Non-merged device build: run_engine_blocks() owns it, the host only needs the geometry.
      L_std    = L_std_geom;
      nout_std = nout_std_geom;
    }
  }
  // Whether the standard blocks' slots are filled by the device. It is decided after
  // merge_tail: a merged batch carries the TAIL block as an extra system whose geometry differs, and
  // the device build only knows the standard one (see below).
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
  // batch as an extra padded system (see merge_tail above), so a hop is one command buffer.
  // \note matrix_on / defer / tail_on_cpu / merge_tail / device_corr_enabled / std_slots_filled are
  //       computed further up: the standard block's host build is skipped when the device fills
  //       those slots, so those decisions have to exist before the build (S-7f-5v).
  nof_pending_unpacks = 0;
  stage_pending       = false;
  bool     hop_gpu = false; // engine processed this hop (any block)
  bool     hop_nn  = false; // the simdgroup 8x8 (matrix) kernels were the ones used
  unsigned hop_pad = 0;     // ceil8(L) - L of the last matrix batch (A/B pad overhead)
  // Written on the data path, read only by the timing report below (hence maybe_unused).
  [[maybe_unused]] unsigned tail_L = 0; // block L of the tail batch (log aid for std-less hops)
  bool       std_blocks_ok = true; // standard-block engine batch succeeded (CPU fallback otherwise)
  bool       tail_ok       = true; // tail/edge-block engine batch succeeded
  if (engine_ready) {
    // Merged batch (S-5c): the standard blocks AND the tail block in ONE engine call, i.e. one
    // command buffer and one wait per hop instead of two. The tail rides as an extra SYSTEM of the
    // batch, not as an extra block: the engine takes one A / R_hp per system and one block count
    // per batch, so the tail's narrower geometry is padded to the standard one - A becomes
    // blockdiag(A_e, I) and R_hp becomes [R_hp_e | 0] - which leaves W = [R_hp_e . A_e^-1 | 0] and
    // h = W . y the very same computation on the tail's own entries (the pad columns of W are
    // exactly zero). It therefore estimates what the split path estimates, at the cost of a few us
    // of extra GPU work (each tail system carries n_std_blocks block slots, of which one is real)
    // against the ~100 us of host round trip it removes.
    //
    // The device build fills the slots of ONE geometry, and a merged batch holds TWO (the standard
    // blocks in the layer systems and the tail block in the extra ones), so it is a PREFIX that
    // covers the standard systems only (corr_stage::nof_systems) - the edge group keeps the host
    // build and staging. See dev_corr_std_merged above.
    //
    // device_corr_enabled is ON by default since the equivalence defect was found and fixed: the
    // host batch stages A^-1 itself, so K1 must NOT run on it, and run_engine_blocks() keys that
    // decision on this stage's presence instead of on the order (see the gpu_invert note there).
    // OCUDU_CE_CORR_DEV=0 keeps the host build for A/B.
    // The matrices are built by the DEVICE and then inverted by the HOST in the slots they were
    // written to, so no order limit applies here: the correlation kernels
    // take whatever geometry the estimator hands them, and the host Gauss-Jordan has no limit either,
    // which is what lets the OTA geometry (order 72) use this path.
    //
    // S-7f-5v: the MERGED batch is covered now, but by a DIFFERENT mechanism - the device builds the
    // standard group as a prefix of the engine's own command buffer (dev_corr_std_merged, decided
    // above the host build) and the edge group keeps the host build. The two-geometry problem that
    // defeated the first attempt is handled by corr_stage::nof_systems: the prefix covers the
    // standard systems only, so it cannot write the edge block's slots. std_slots_filled therefore
    // still means "the non-merged batch's slots are the device's", exactly as before.
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
      // The received pilots were staged before the extraction (S-7f-5w); use that answer rather than
      // re-deriving it, or a geometry the staging refused would leave K4 reading unstaged pilots.
      nof_cdm_groups     = staged_cdm_groups;
      if ((npf != 0) && (nof_cdm_groups != 0) && (npt <= MAX_DMRS_SYMBOLS)) {
        // Only the TRANSMITTED pilots here: the received ones and the epochs were staged before the
        // extraction (S-7f-5w), which is what the device noise variance reads.
        for (unsigned i_dmrs = 0; i_dmrs != npt; ++i_dmrs) {
          for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
            span<const cf_t> src = args.pilots.get_symbol(args.hop_offset + i_dmrs, i_layer);
            float* dst = gpu_pilots + ((static_cast<std::size_t>(i_dmrs) * nof_layers + i_layer) * npf) * 2;
            // The kernel reads the reference as interleaved re/im floats, which is bit for bit the
            // layout of a contiguous array of cf_t (the standard guarantees it), so the staging is one
            // bulk copy per (symbol, layer) instead of a copy per element: measured 0.129 -> 0.081 us
            // per hop for this cell's largest allocation (14 PRB, 3 DM-RS symbols, one layer). Same
            // bytes, so nothing downstream can change.
            ocudu_assert(src.size() >= npf, "The hop's pilot list is shorter than the symbol's pilots.");
            std::memcpy(dst, src.data(), npf * sizeof(cf_t));
          }
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
      // Where K4's rotation takes its CFO from (see noise_stage_t). The extraction wrote this hop's
      // own estimate into the slot reserved for it, and the kernel reads it there - which is what
      // keeps the scalar out of the host's hands and lets the weights command buffer be encoded
      // without waiting for the extraction first. When the device did not build the pilots (the cold
      // path, or a route without the device extraction) the host pre-stage's answer is the matching
      // one and travels as a parameter.
      reformat.noise.cfo_dev         = &gpu_ls_cfo[cfo_slot_];
      reformat.noise.cfo_from_device = device_ls_valid;
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
      // The gate is device_inverts(): default ON, bounded by the kernel's own MAX_N. See its comment
      // for why the earlier performance-based retreat to the host was reversed (S-7f-3s).
      const bool gpu_invert = device_inverts(L_std);
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
      // K0-d: the DEVICE builds A and R_hp of the standard group; the host then inverts what it
      // wrote. The edge block is built separately below, into these same slots.
      {
        // The standard group's A/R_hp must be staged with the SAME inversion decision as the tail
        // below and as run_engine_blocks(): when the device inverts, both must leave A in the slots
        // for K1. A hardcoded false here wrote A^-1 into the very slots the tail filled with A
        // (S-7f-4a), which is what "one geometry written with two semantics" looked like.
        //
        // S-7f-5v: slots_filled = dev_corr_std_merged. When the device builds this group as a
        // prefix of the engine's own command buffer (below), the host must not touch these slots at
        // all - not even to stage them - or the write would land on the device's matrices. The y
        // descriptors are recorded either way: they are not part of that gate.
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
                           dev_corr_std_merged);
      }
#if defined(OCUDU_CE_TIME)
      stage_us_local += std::chrono::duration<double, std::micro>(steady_clock::now() - t_stage_begin).count();
#endif
      // 2) Only NOW build the tail matrices: they overwrite w_r_pp / w_r_hp, so the reverse order
      //    silently stages the tail's matrices for the standard blocks.
      unsigned nout_e = 0;
      unsigned L_e    = 0;
      // K0-d does not cover this branch yet. The edge block rides as EXTRA SYSTEMS of the standard
      // batch, so its slots are the standard ones - slot stride L_std / nout_std while its own block
      // order is L_e / nout_e, which is what the correlation kernels' system stride was added for.
      // What defeated the first attempt at covering this branch was that the device prefix built the
      // standard geometry for the WHOLE batch and so wrote over the edge block's slots; the prefix
      // now covers the standard systems only (corr_stage::nof_systems), and the edge group keeps the
      // host build and staging below - one geometry per writer.
      // Who builds the edge block's matrices. On the default route it is the host, as it always has
      // been; with OCUDU_CE_TAIL_DEV=1 the device builds them in its own command buffer and the host
      // writes only the pads of the oversized slot (see build_slots_on_device) - which is what takes
      // the host out of the matrix business for good, and with it the reason it reads the extraction's
      // scalars (see edge_build_on_device()).
      bool edge_on_device = false;
      // The edge group's correlation, to be encoded into the MERGED batch's own command buffer
      // (see build_slots_on_device). Only filled on the gpu_invert route; the merged engine call
      // below is handed it as the second prefix.
      std::optional<metal::mmse_engine::corr_stage> edge_corr;
      if (edge_build_on_device()) {
        static_vector<unsigned, MAX_NOF_DMRS_SYMBOLS> edge_dmrs;
        args.pattern_symbols.for_each(args.first_symbol, args.last_symbol,
                                      [&](unsigned s) { edge_dmrs.push_back(s); });
        // Geometry first: correlation_stage() is pure packing, and nout_e / L_e are needed for the
        // staging and the unpack whether the device builds or the host does. The stride assert inside
        // it is also the cheapest way to refuse a geometry the slots cannot hold.
        (void)correlation_stage(stats,
                                args.dmrs_patterns.front().re_pattern,
                                rem_prb,
                                span<const unsigned>(edge_dmrs.begin(), edge_dmrs.size()),
                                scs_khz,
                                nof_layers,
                                nout_e,
                                L_e,
                                st.L,
                                st.nout);
        // The place in the merged command buffer this group's correlation will be encoded into, when
        // the device is the one that inverts it (see build_slots_on_device and corr_stage::nof_systems).
        metal::mmse_engine::corr_stage* fused_edge =
            (gpu_invert && edge_fuse_enabled()) ? &edge_corr.emplace() : nullptr;
        edge_on_device = build_slots_on_device(stats,
                                               args.dmrs_patterns.front().re_pattern,
                                               rem_prb,
                                               span<const unsigned>(edge_dmrs.begin(), edge_dmrs.size()),
                                               scs_khz,
                                               nof_layers,
                                               nof_layers,
                                               st.L,
                                               st.nout,
                                               L_e,
                                               gpu_invert,
                                               fused_edge);
      }
      if (!edge_on_device) {
        // Host construction and staging for the tail systems (the route that has always run).
        build_correlation_matrices(stats,
                                   args.dmrs_patterns.front().re_pattern,
                                   rem_prb,
                                   span<const unsigned>(dmrs_sym.begin(), npt),
                                   scs_khz,
                                   span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS),
                                   span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS),
                                   nout_e,
                                   L_e);
      }
      tail_L = L_e;
      // 3) The tail systems carry n_std_blocks block slots but only block 0 is real: clear the
      //    group first so the pad blocks hold zeros instead of a previous hop's pilots.
      std::memset(gpu_y + static_cast<std::size_t>(nof_layers) * n_std_blocks * 2 * L_std,
                  0,
                  static_cast<std::size_t>(nof_layers) * n_std_blocks * 2 * L_std * sizeof(float));
      stage_engine_group(args,
                         n_std_blocks * block_prb,
                         1,
                         rem_prb,
                         npt,
                         nout_e,
                         L_e,
                         nof_layers,
                         st,
                         matrix_on,
                         gpu_invert,
                         /*slots_filled=*/edge_on_device);
      // 3b) OCUDU_CE_EDGE_CHECK=1: compare the edge group's slots against the host's build of the same
      //     geometry, at the LAST moment before the engine consumes them. Same hop, same inputs, one
      //     process - so a disagreement is a property of the build, not of the harness. Only meaningful
      //     when the DEVICE built them (with edge_on_device false the slots hold the host's own
      //     staging). It reads the slots, so it must run after every writer: for the device route that
      //     is build_slots_on_device() above (whose standalone command buffer has already completed),
      //     and for the staged route stage_engine_group() just below - hence the placement here.
      //     It does NOT wait for anything and does not touch the published output.
      if (edge_on_device && edge_slot_check_enabled()) {
        (void)check_edge_slots(stats,
                               args.dmrs_patterns.front().re_pattern,
                               rem_prb,
                               span<const unsigned>(dmrs_sym.begin(), npt),
                               scs_khz,
                               nout_e,
                               L_e,
                               nof_layers,
                               nof_layers,
                               st.L,
                               st.nout);
      }

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
      const bool merged_ok = engine_run(std_corr_prefix.has_value() ? &std_corr_prefix.value() : nullptr,
                                        nout_std,
                                        L_std,
                                        2 * nof_layers,
                                        n_std_blocks,
                                        false,
                                        gpu_invert,
                                        reformat_for(block_prb * NOF_SUBCARRIERS_PER_RB,
                                                     rem_prb * NOF_SUBCARRIERS_PER_RB,
                                                     nof_layers),
                                        merged_defer,
                                        st,
                                        0,
                                        edge_corr.has_value() ? &edge_corr.value() : nullptr);
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
          // Inline (non-deferred) route: this is the synchronous fallback, where the caller wants
          // the results now and the host consumers are the point - so the whole grid is unpacked,
          // exactly as before S-7f-6a. Only the deferred completion below is lazy.
          unpack_engine_group(0, n_std_blocks, block_prb, nout_std, nof_layers, 0, st, /*all_symbols=*/true);
          unpack_engine_group(n_std_blocks * block_prb, 1, rem_prb, nout_e, nof_layers, nof_layers, st, /*all_symbols=*/true);
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
    // Diagnostic (OCUDU_CE_Y_CHECK=1): did the DEVICE write the y slot values the host staging
    // would have written? See probe_device_y_stage(). Costs a completion of the batch, so it is
    // debug-only; the gate for this path is the byte-identical A/B (OCUDU_CE_DEV_Y=0).
    static const bool y_check = (std::getenv("OCUDU_CE_Y_CHECK") != nullptr);
    if (y_check) {
      (void)probe_device_y_stage(args);
    }
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
      // Pack the block pilot vector (symbol-major, real/imag interleaved), in the DATA domain.
      for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
        const unsigned base = b_start_prb * 6;
        for (unsigned j = 0; j != b_npf; ++j) {
          const cf_t v                            = ls_pilot(args, i_symbol, i_layer, base + j, /*scaled=*/true);
          y_block[2 * (i_symbol * b_npf + j)]     = v.real();
          y_block[2 * (i_symbol * b_npf + j) + 1] = v.imag();
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

bool port_channel_estimator_metal_mmse_impl::probe_device_y_stage(const fd_td_estimation_stage_args& args)
{
  if (nof_device_y_stage_last == 0) {
    return true;
  }
  // The scatter runs inside the engine's command buffer, which the deferred path has not waited
  // for: complete it before reading what it wrote.
  (void)complete_fd_td_estimation_stage();

  unsigned nof_bad  = 0;
  unsigned nof_slot = 0;
  double   max_err  = 0.0;
  for (unsigned g = 0; g != nof_device_y_stage_last; ++g) {
    const metal::mmse_engine::pilots_scatter& s = device_y_stage_last[g];
    for (unsigned i_layer = 0; i_layer != s.nof_layers; ++i_layer) {
      for (unsigned b = 0; b != s.n_blk_slots; ++b) {
        const float* d = s.y + (static_cast<std::size_t>(i_layer) * s.n_blk_slots + b) * 2 * s.Ls;
        for (unsigned k = 0; k != s.Ls; ++k) {
          float er = 0.0F;
          float ei = 0.0F;
          if ((b < s.n_blk_real) && (k < s.nof_symb * s.npf)) {
            const unsigned i_symb = k / s.npf;
            const unsigned j      = k - i_symb * s.npf;
            // The device's y slots carry the DATA domain (it applies 1 / beta itself, see
            // pilots_stage::inv_beta), so the expectation is the scaled LS pilot.
            const cf_t v = ls_pilot(args, i_symb, i_layer, s.pilot_base + b * s.npf + j, /*scaled=*/true);
            er           = v.real();
            ei           = v.imag();
          }
          ++nof_slot;
          const double dr = static_cast<double>(d[2 * k]) - er;
          const double di = static_cast<double>(d[2 * k + 1]) - ei;
          const double e  = std::max(std::abs(dr), std::abs(di));
          if (e != 0.0) {
            if (nof_bad < 12) {
              std::fprintf(stderr,
                           "[y_check] group=%u layer=%u block=%u row=%u dev=(%g,%g) host=(%g,%g)\n",
                           g,
                           i_layer,
                           b,
                           k,
                           static_cast<double>(d[2 * k]),
                           static_cast<double>(d[2 * k + 1]),
                           static_cast<double>(er),
                           static_cast<double>(ei));
            }
            ++nof_bad;
          }
          max_err = std::max(max_err, e);
        }
      }
    }
  }
  std::fprintf(stderr,
               "[y_check] groups=%u slots=%u mismatched=%u max_abs=%.3e\n",
               nof_device_y_stage_last,
               nof_slot,
               nof_bad,
               max_err);
  return nof_bad == 0;
}

bool port_channel_estimator_metal_mmse_impl::record_device_y_stage(const fd_td_estimation_stage_args& args,
                                                                   unsigned                           gb_start,
                                                                   unsigned                           n_blk,
                                                                   unsigned                           b_prb,
                                                                   unsigned                           npt,
                                                                   unsigned                           L,
                                                                   unsigned                           sys_offset,
                                                                   const engine_strides&              st)
{
  // The gates, in the order in which they can fail. Every one of them falls back to the host
  // staging, which is the pre-glue-#2 behaviour and stays bit-for-bit equivalent, so a false here
  // is never a correctness risk - only a missed removal.
  if (!device_y_enabled || !device_ls_valid || !engine_ready || (engine == nullptr) ||
      !engine->scatter_available()) {
    return false;
  }
  const unsigned nof_layers = args.dmrs_patterns.size();
  const auto&    hop_rb_mask =
      (args.hop == 0) ? args.dmrs_patterns.front().rb_mask : args.dmrs_patterns.front().rb_mask2;
  const unsigned comb = args.dmrs_patterns.front().re_pattern.count();
  // The HOP layout of gpu_ls_out, as K0-a built it: nof_prb * comb pilots per (symbol, layer).
  // K0-a refuses a geometry whose pilot count disagrees with the caller's, so this is the same
  // number it produced - but it is checked again rather than assumed, because a mismatch here
  // would read past the hop's pilots instead of failing.
  const unsigned nof_hop_pilots = hop_rb_mask.count() * comb;
  const unsigned npf            = b_prb * comb;
  if ((comb == 0) || (nof_hop_pilots == 0) || (nof_hop_pilots != args.nof_symbol_pilots) || (npf == 0) ||
      (npt == 0) || (npt > MAX_DMRS_SYMBOLS) || (nof_layers == 0) || (nof_layers > MAX_LAYERS) ||
      (n_blk == 0) || (st.n_blk == 0) || (st.L == 0) || (L == 0) || (L > MAX_BLOCK_PILOTS) ||
      (st.L > MAX_BLOCK_PILOTS) || (npt * npf > st.L) || (nof_device_y_stage >= device_y_stage.size())) {
    return false;
  }
  // The destination must fit the buffer the engine call binds: the group's systems start at
  // sys_offset and each carries st.n_blk block slots of 2 * st.L floats.
  const std::size_t y_floats =
      static_cast<std::size_t>(MAX_LAYERS) * max_blocks * 2 * MAX_BLOCK_PILOTS;
  if ((static_cast<std::size_t>(sys_offset) + nof_layers) * st.n_blk * 2 * st.L > y_floats) {
    return false;
  }

  metal::mmse_engine::pilots_scatter& s = device_y_stage[nof_device_y_stage];
  s                                     = {};
  s.lse                                 = gpu_ls_out;
  s.lse_bytes                           = k_ls_floats * sizeof(float);
  // The group's system offset goes into the POINTER, derived from the same (sys_offset, st) the
  // engine call derives its own y base from - so the writer and the reader cannot disagree about
  // which system they mean (the split-tail defect of S-7f-5l was exactly that, in the other
  // direction). run_async() binds this pointer as given and adds no offset of its own.
  s.y          = gpu_y + static_cast<std::size_t>(sys_offset) * st.n_blk * 2 * st.L;
  s.nof_layers = nof_layers;
  s.nof_symb   = npt;
  s.nof_pilots = nof_hop_pilots;
  s.npf        = npf;
  s.pilot_base = gb_start * comb;
  // The SLOT count and the FILLED count differ in exactly one place: a merged batch puts the edge
  // block into the standard group's slots, so its tail group has st.n_blk slots of which one is
  // real (n_blk == 1 there). The kernel zeroes the rest, which is what the host's memset of the
  // tail's y region does before staging it.
  s.n_blk_slots = st.n_blk;
  s.n_blk_real  = n_blk;
  s.Ls          = st.L;
  // The DM-RS to data scaling the host applies to the pilots it stages. The device reads
  // gpu_ls_out, which does NOT carry it (that is the received domain K0-a produces), so the kernel
  // applies it - one multiply per component, the same one the host's staging loop applies through
  // ls_pilot(..., /*scaled=*/true).
  s.inv_beta = 1.0F / args.beta_scaling;
  ++nof_device_y_stage;
  return true;
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
  // There is exactly ONE gate here, and it means one thing: \c slots_filled - the device already
  // wrote A AND R_hp into these slots (K0-d), so the host has nothing left to write. It is NOT a
  // per-flag decision: A and R_hp are written TOGETHER or not at all. K1 only ever touches A, so a
  // "skip A but not R_hp" flag (the \c a_rhp_filled this used to take, driven by dev_inv_now) left
  // R_hp holding the previous hop's residue on the device-inversion path - 2916 of 27216 entries
  // valid, h 1440 of 8064, SINR -23 dB. The historical (working) shape is exactly this one: the
  // if/else selects A or A^-1, and everything else is unconditional.
  //
  // The pilot staging below is outside the gate: y/qy is the host's either way (the device build
  // knows the matrices, not the received pilots).
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
          const unsigned base = (gb_start + b * b_prb) * comb;
          for (unsigned j = 0; j != npf; ++j) {
            const cf_t v                     = ls_pilot(args, i_symbol, i_layer, base + j, /*scaled=*/true);
            qp[(i_symbol * npf + j) * 8]     = v.real();
            qp[(i_symbol * npf + j) * 8 + 1] = v.imag();
          }
        }
      }
    }
  } else {
    // Glue #2 (S-7f-5u): the DEVICE is the writer of these slots whenever it can be - it re-indexes
    // the pilots K0-a already produced into exactly this layout, inside the command buffer whose
    // weights read them, so nothing is copied through the host at all. record_device_y_stage()
    // answers for the whole decision (device LSE valid, kernel present, knob, geometry) and is the
    // reason the loop below is skipped; the loop itself is the fallback and must keep producing
    // byte-identical values, because OCUDU_CE_DEV_Y=0 selects it as the A/B.
    if (!record_device_y_stage(args, gb_start, n_blk, b_prb, npt, L, sys_offset, st)) {
      for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
        for (unsigned b = 0; b != n_blk; ++b) {
          float* yp = gpu_y + (static_cast<std::size_t>(sys_offset + i_layer) * st.n_blk + b) * 2 * Ls;
          // Pad rows k in [L, Ls) stay zero: their weights are exactly zero, but a non-finite value
          // left there would reach h through 0 * inf = NaN.
          if (Ls > L) {
            std::memset(yp + 2 * L, 0, static_cast<std::size_t>(Ls - L) * 2 * sizeof(float));
          }
          for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
            const unsigned base = (gb_start + b * b_prb) * comb;
            for (unsigned j = 0; j != npf; ++j) {
              const cf_t v                     = ls_pilot(args, i_symbol, i_layer, base + j, /*scaled=*/true);
              yp[2 * (i_symbol * npf + j)]     = v.real();
              yp[2 * (i_symbol * npf + j) + 1] = v.imag();
            }
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
                                                        bool     defer,
                                                        const engine_strides& st,
                                                        unsigned sys_offset,
                                                        const metal::mmse_engine::corr_stage* corr_edge)
{
  // The engine addresses the systems of a batch from the BASE of each staging buffer, while the
  // caller stages them (stage_engine_group()) and unpacks them (unpack_engine_group()) at
  // `sys_offset` - a batch tucked into the slots AFTER another one, which is what the split form's
  // tail block is (sys_offset = nof_layers). Handing the engine the base pointers made it read and
  // write systems [0, nof_systems) instead of [sys_offset, sys_offset + nof_systems): the tail was
  // computed from the standard group's slots, its results landed in the wrong h slots, and the
  // unpack then read stale data. That is the defect the channel-estimator unit test's Test 11
  // (merged vs split) has been reporting all along.
  float* a_slot = gpu_a + static_cast<std::size_t>(sys_offset) * st.L * st.L;
  float* r_slot = gpu_r_hp + static_cast<std::size_t>(sys_offset) * st.nout * st.L;
  float* w_slot = gpu_w + static_cast<std::size_t>(sys_offset) * st.nout * st.L;
  float* y_slot = gpu_y + static_cast<std::size_t>(sys_offset) * st.n_blk * 2 * st.L;
  float* h_slot = gpu_h + static_cast<std::size_t>(sys_offset) * st.n_blk * 2 * st.nout;
  float* q_slot = gpu_qy + static_cast<std::size_t>(sys_offset) * ((nof_blocks + 3u) / 4u) * st.L * 8;
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

  // Glue #2: the y descriptors stage_engine_group() recorded while staging this batch. They are
  // CONSUMED here - the count is cleared before the call, so a failure cannot leave them to be
  // encoded into a later batch (whose slots they do not describe), and a hop that stages nothing
  // cannot inherit them. The matrix flavor never records any (qy is still host-packed, by design).
  const metal::mmse_engine::pilots_scatter* y_scatter   = device_y_stage.data();
  const unsigned                            nof_y_scatter = nof_device_y_stage;
  nof_device_y_stage                                    = 0;
  // Keep a copy for the OCUDU_CE_Y_CHECK probe (see probe_device_y_stage()): the descriptors are
  // the only record of what the device was told to write, and the batch may be deferred.
  device_y_stage_last      = device_y_stage;
  nof_device_y_stage_last  = nof_y_scatter;

  // Two flavors, selected by WHERE A IS INVERTED - not by which entry point happens to exist:
  //   gpu_invert: the slots hold A, and K1 (plus the correlation prefix, when corr is given) runs in
  //               THIS command buffer, before the weights. This is the form the full-GPU-path goal
  //               asks for, and it is the default.
  //   otherwise:  the caller already staged A^-1 (host Gauss-Jordan), so the weights read it as is.
  // The matrix (nn) flavor is the only structurally different one.
  // TEMPORARY experiment: OCUDU_CE_INVERT_FIRST=1 inverts A with the ENGINE'S OWN standalone
  // entry point (mmse_engine::invert(), its own command buffer - the path k1_check has always
  // used successfully) right before the weights call, instead of leaving K1 inside that call's
  // buffer. Isolates "K1 does not work" from "K1 does not work INSIDE run_async()'s buffer".
  if (gpu_invert && (std::getenv("OCUDU_CE_INVERT_FIRST") != nullptr)) {
    if (!engine->invert(gpu_a, L, nof_systems)) {
      logger.error("[mmse_ce] standalone invert() failed (L={}, systems={})", L, nof_systems);
    }
  }
  const bool k1_inline = gpu_invert && (std::getenv("OCUDU_CE_INVERT_FIRST") == nullptr);
  const bool engine_ok =
      matrix ? engine->run_nn(a_slot, r_slot, w_slot, q_slot, h_slot, nout, L, nof_systems, nof_blocks)
             : (k1_inline
                    ? (defer ? engine->run_async(a_slot,
                                                 r_slot,
                                                 w_slot,
                                                 y_slot,
                                                 h_slot,
                                                 nout,
                                                 L,
                                                 nof_systems,
                                                 nof_blocks,
                                                 reformat,
                                                 corr,
                                                 y_scatter,
                                                 nof_y_scatter,
                                                 corr_edge)
                             : engine->run(a_slot,
                                           r_slot,
                                           w_slot,
                                           y_slot,
                                           h_slot,
                                           nout,
                                           L,
                                           nof_systems,
                                           nof_blocks,
                                           reformat,
                                           y_scatter,
                                           nof_y_scatter))
                    : (defer ? engine->run_weights_only_async(a_slot,
                                                              r_slot,
                                                              w_slot,
                                                              y_slot,
                                                              h_slot,
                                                              nout,
                                                              L,
                                                              nof_systems,
                                                              nof_blocks,
                                                              reformat,
                                                              corr,
                                                              y_scatter,
                                                              nof_y_scatter)
                             : engine->run_weights_only(a_slot,
                                                        r_slot,
                                                        w_slot,
                                                        y_slot,
                                                        h_slot,
                                                        nout,
                                                        L,
                                                        nof_systems,
                                                        nof_blocks,
                                                        reformat,
                                                        corr,
                                                        y_scatter,
                                                        nof_y_scatter)));
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
                                                                 const engine_strides& st,
                                                                 bool                  all_symbols) const
{
  const unsigned nf = b_prb * NOF_SUBCARRIERS_PER_RB;
  // gb_start counts PRBs while b counts blocks of b_prb PRBs, so block b starts at subcarrier
  // (gb_start + b * b_prb) * 12. Using (gb_start + b) * nf instead (nf being the block's
  // subcarriers) is right only while gb_start is zero or a block is one PRB wide, and it lands
  // past the end of the grid for the edge block of a hop that is not a multiple of the block
  // size - the same mistake the pilot staging had, in the host half of the estimator.
  // Only the symbols a consumer asked for: the hop statistics read the DM-RS ones, and the rest of
  // the grid is materialized on demand by materialize_host_grid(). Unpacking all fourteen symbols
  // eagerly was most of this function's cost, for a host copy the air path never reads.
  const unsigned nof_unpack_symbols = all_symbols ? MAX_NSYMB_PER_SLOT : unpack_npt;
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned b = 0; b != n_blk; ++b) {
      // Slot addressing uses the BATCH strides: they may exceed the block geometry (the merged
      // tail system keeps the standard Ls/Ns/n_blk slots while its own block is narrower), and
      // reading it with the block geometry silently unpacks the wrong rows.
      const float* hp = gpu_h + (static_cast<std::size_t>(sys_offset + i_layer) * st.n_blk + b) * 2 * st.nout;
      for (unsigned i = 0; i != nof_unpack_symbols; ++i) {
        const unsigned sym = all_symbols ? i : unpack_dmrs_sym[i];
        span<cf_t>     dst = grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + sym)
                             .subspan(static_cast<std::size_t>(gb_start + b * b_prb) * NOF_SUBCARRIERS_PER_RB, nf);
        for (unsigned sc = 0; sc != nf; ++sc) {
          dst[sc] = {hp[2 * (sym * nf + sc)], hp[2 * (sym * nf + sc) + 1]};
        }
      }
    }
  }
}

void port_channel_estimator_metal_mmse_impl::materialize_host_grid() const
{
  if (!host_grid_pending) {
    return;
  }
  host_grid_pending = false;
  for (unsigned i = 0; i != nof_host_unpacks; ++i) {
    const pending_unpack& u = host_unpacks[i];
    unpack_engine_group(
        u.gb_start, u.n_blk, u.b_prb, u.nout, u.nof_layers, u.sys_offset, u.st, /*all_symbols=*/true);
  }
}

bool port_channel_estimator_metal_mmse_impl::check_edge_slots(
    const channel_statistics&                     stats,
    const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
    unsigned                                       b_prb,
    span<const unsigned>                           dmrs_slots,
    unsigned                                       scs_khz,
    unsigned                                       nout_e,
    unsigned                                       L_e,
    unsigned                                       sys_offset,
    unsigned                                       nof_systems,
    unsigned                                       a_stride,
    unsigned                                       r_stride)
{
  // The host's own build of THIS geometry, into scratch the device route does not use.
  unsigned    nout_h = 0;
  unsigned    L_h    = 0;
  const auto  a_host = span<float>(w_r_pp.data(), MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS);
  const auto  r_host = span<float>(w_r_hp.data(), MAX_BLOCK_OUT * MAX_BLOCK_PILOTS);
  build_correlation_matrices(stats, re_pattern, b_prb, dmrs_slots, scs_khz, a_host, r_host, nout_h, L_h);

  const auto  bits = [](float v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    return u;
  };
  // Read the device's slots EXACTLY as the kernels wrote them: the slot footprint is the whole
  // [a_stride][a_stride] / [r_stride][a_stride] region, pads included, so a pad that the host and
  // the device disagree about is a difference like any other.
  std::fprintf(stderr,
               "[edge_check] geometry: L_e=%u (host %u) nout_e=%u (host %u) a_stride=%u r_stride=%u "
               "sys_offset=%u n_sys=%u\n",
               L_e, L_h, nout_e, nout_h, a_stride, r_stride, sys_offset, nof_systems);
  if ((L_e != L_h) || (nout_e != nout_h)) {
    std::fprintf(stderr, "[edge_check] GEOMETRY MISMATCH - the two builds do not even describe the same block\n");
    return false;
  }

  unsigned nof_a = 0, nof_r = 0, bad_a = 0, bad_r = 0, shown = 0;
  double   worst = 0.0;
  for (unsigned sys = 0; sys != nof_systems; ++sys) {
    const float* a_slot = gpu_a + static_cast<std::size_t>(sys_offset + sys) * a_stride * a_stride;
    const float* r_slot = gpu_r_hp + static_cast<std::size_t>(sys_offset + sys) * r_stride * a_stride;
    for (unsigned r = 0; r != a_stride; ++r) {
      for (unsigned c = 0; c != a_stride; ++c) {
        const float dev = a_slot[static_cast<std::size_t>(r) * a_stride + c];
        // Outside [0,L_e) x [0,L_e) the host's scratch has no value (it is packed L_e x L_e), so the
        // device's pad is compared against what the contract says it must BE instead: zero, or one on
        // the diagonal of the [L_e, a_stride) block. Anything else is a real disagreement and is
        // reported as one.
        float host = 0.0F;
        if ((r < L_e) && (c < L_e)) {
          host = a_host[static_cast<std::size_t>(r) * L_e + c];
        } else if (r == c) {
          host = 1.0F;
        }
        ++nof_a;
        if (bits(dev) != bits(host)) {
          ++bad_a;
          worst = std::max(worst, std::fabs(static_cast<double>(dev) - static_cast<double>(host)));
          if (shown < 12) {
            ++shown;
            std::fprintf(stderr,
                         "[edge_check] A   sys=%u r=%u c=%u host=%08x (%.9g) dev=%08x (%.9g)%s\n",
                         sys, r, c, bits(host), host, bits(dev), dev,
                         ((r < L_e) && (c < L_e)) ? "" : "  <- PAD");
          }
        }
      }
    }
    for (unsigned o = 0; o != r_stride; ++o) {
      for (unsigned j = 0; j != a_stride; ++j) {
        const float dev  = r_slot[static_cast<std::size_t>(o) * a_stride + j];
        const float host = ((o < nout_e) && (j < L_e)) ? r_host[static_cast<std::size_t>(o) * L_e + j] : 0.0F;
        ++nof_r;
        if (bits(dev) != bits(host)) {
          ++bad_r;
          worst = std::max(worst, std::fabs(static_cast<double>(dev) - static_cast<double>(host)));
          if (shown < 12) {
            ++shown;
            std::fprintf(stderr,
                         "[edge_check] R   sys=%u o=%u j=%u host=%08x (%.9g) dev=%08x (%.9g)%s\n",
                         sys, o, j, bits(host), host, bits(dev), dev,
                         ((o < nout_e) && (j < L_e)) ? "" : "  <- PAD");
          }
        }
      }
    }
  }
  // Where the device's data actually STOPS, over the whole slot: the highest non-zero float. A build
  // that wrote only part of the slot leaves the rest at zero, and this is the number that says how
  // much of it was written - which distinguishes "never dispatched" from "written elsewhere".
  long last_nonzero = -1;
  for (unsigned sys = 0; sys != nof_systems; ++sys) {
    const float* r_slot = gpu_r_hp + static_cast<std::size_t>(sys_offset + sys) * r_stride * a_stride;
    for (unsigned o = 0; o != r_stride; ++o) {
      for (unsigned j = 0; j != a_stride; ++j) {
        if (r_slot[static_cast<std::size_t>(o) * a_stride + j] != 0.0F) {
          last_nonzero = static_cast<long>(o) * a_stride + j;
        }
      }
    }
  }
  std::fprintf(stderr,
               "[edge_check] checked A=%u (%u differ) R=%u (%u differ) | worst |dev-host| = %.9g | %s\n",
               nof_a, bad_a, nof_r, bad_r, worst, ((bad_a == 0) && (bad_r == 0)) ? "IDENTICAL" : "DIFFERENT");
  std::fprintf(stderr,
               "[edge_check] R slot: last non-zero element at offset %ld (= row %ld, col %ld) | slot is "
               "%u rows x %u cols; block wants %u rows x %u cols; packed block size = %u floats\n",
               last_nonzero,
               (last_nonzero < 0) ? -1L : last_nonzero / static_cast<long>(a_stride),
               (last_nonzero < 0) ? -1L : last_nonzero % static_cast<long>(a_stride),
               r_stride, a_stride, nout_e, L_e, nout_e * L_e);
  return (bad_a == 0) && (bad_r == 0);
}

bool port_channel_estimator_metal_mmse_impl::build_slots_on_device(
    const channel_statistics&                     stats,
    const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern,
    unsigned                                      b_prb,
    span<const unsigned>                          dmrs_slots,
    unsigned                                      scs_khz,
    unsigned                                      sys_offset,
    unsigned                                      nof_systems,
    unsigned                                      a_stride,
    unsigned                                      r_stride,
    unsigned                                      L,
    bool                                          gpu_invert,
    metal::mmse_engine::corr_stage*               fused_corr)
{
  unsigned nout_c = 0;
  unsigned L_c    = 0;
  const metal::mmse_engine::corr_stage corr_std =
      correlation_stage(stats, re_pattern, b_prb, dmrs_slots, scs_khz, sys_offset, nout_c, L_c, a_stride, r_stride);

  // Leaves A and R_hp (the L x L and nout x L blocks, slot strides a_stride / a_stride) in the
  // slots. The pads of an oversized slot are the CALLER's: stage_engine_group() writes them when it
  // stages the group, and a caller that skips it (device-inverted, unpadded slot) has no pad to
  // write.
  // K0-a fusion, the SECOND group of a merged batch: when the caller offers a place in its own
  // command buffer (fused_corr) AND the device is the one that inverts these slots, the correlation
  // does not need a command buffer of its own. Handing the stage back instead of submitting it here
  // keeps the merged hop in ONE engine command buffer - the standalone form paid a commit and a wait
  // on every hop that had an edge (measured on air: cbs/lane 3.00 -> 3.28). The pads below are still
  // written here: they are geometry, and the region they cover is disjoint from the one the kernels
  // write, so the host and the device never touch the same bytes.
  //
  // The device must NOT be left to invert what it has not built, so the fused form is taken only on
  // the gpu_invert route; a host inversion (below) needs the A slots to hold A BEFORE this function
  // returns, which is what the standalone build gives it.
  if (fused_corr != nullptr && gpu_invert) {
    *fused_corr = corr_std;
    // NOT optional: corr_stage::nof_systems is 0 here (correlation_stage() cannot know how many
    // systems its caller will build) and 0 means "the WHOLE batch" to encode_corr - which for a
    // merged batch is 2 * nof_layers systems of the EDGE geometry, written over the standard
    // group's slots. Measured: 51810 differing bytes instead of the host build's 6043, 36527 of
    // them in the LLR. build_correlation() takes the count as an ARGUMENT for this reason, and the
    // fused form has to carry it in the stage.
    fused_corr->nof_systems = nof_systems;
  } else if (!engine->build_correlation(corr_std, nof_systems)) {
    return false;
  }
  // The pads of an OVERSIZED slot are written here, not by stage_engine_group(): a caller that
  // reports the slots as filled skips that staging entirely, and the pad region would otherwise still
  // hold the previous hop. Geometry, not matrix data - which is why the host can write it without
  // being back in the matrix business. Both loops are no-ops when the slot is exact (a_stride == L,
  // r_stride == nout_c), which is every route that existed before this.
  //
  // A becomes blockdiag(A, I): K1 then inverts one invertible Ls x Ls system and
  // W = [R_hp | 0] . blockdiag(A^-1, I) = [R_hp . A^-1 | 0], so h = W . y keeps the values of the
  // unpadded system (the same construction stage_engine_group() makes).
  for (unsigned sys = 0; sys != nof_systems; ++sys) {
    float* a_slot = gpu_a + static_cast<std::size_t>(sys_offset + sys) * a_stride * a_stride;
    float* r_slot = gpu_r_hp + static_cast<std::size_t>(sys_offset + sys) * r_stride * a_stride;
    for (unsigned r = 0; r != L; ++r) {
      std::memset(a_slot + static_cast<std::size_t>(r) * a_stride + L, 0,
                  static_cast<std::size_t>(a_stride - L) * sizeof(float));
    }
    for (unsigned k = L; k != a_stride; ++k) {
      std::memset(a_slot + static_cast<std::size_t>(k) * a_stride, 0,
                  static_cast<std::size_t>(a_stride) * sizeof(float));
      a_slot[static_cast<std::size_t>(k) * a_stride + k] = 1.0F;
    }
    // R_hp: real values in rows [0, nout) and columns [0, L), zero everywhere else - including the
    // pad ROWS, which is why the second loop clears whole rows.
    for (unsigned o = 0; o != nout_c; ++o) {
      std::memset(r_slot + static_cast<std::size_t>(o) * a_stride + L, 0,
                  static_cast<std::size_t>(a_stride - L) * sizeof(float));
    }
    for (unsigned o = nout_c; o != r_stride; ++o) {
      std::memset(r_slot + static_cast<std::size_t>(o) * a_stride, 0,
                  static_cast<std::size_t>(a_stride) * sizeof(float));
    }
  }

  if (gpu_invert) {
    // The device inverts these slots in the weights command buffer (K1), so this call must leave A
    // in them. Inverting here as well would have K1 invert an A^-1 - the S-7f-3i defect.
    return true;
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
  return true;
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
  // WHICH of the two inverts is decided by device_inverts(): when the device does it, (a)/(b) must
  // leave A in the slots and K1 inverts in the weights command buffer; otherwise the host writes
  // A^-1 here. Getting that backwards is the S-7f-3i defect (an inverse inverted again).
  //
  // NOTE the order: the device must write BEFORE the host reads. A device build that runs as a
  // command-buffer PREFIX of the weights call (the form run_weights_only() supports for the orders
  // K1 can handle) would land AFTER this host loop and leave the weights reading a raw A - which is
  // exactly the 1640x blow-up of S-7f-3g. Hence the standalone, synchronously completed
  // build_correlation() here.
  //
  // Whether THIS batch's inversion runs on the device (see device_inverts()): the slots must then
  // still hold A when K1 reads them, and the engine call routes to the K1 pipeline.
  const bool gpu_invert = !matrix && device_inverts(L);
  // K0-d: the device builds A and R_hp of this batch. The RETURN VALUE is the success flag (never a
  // descriptor, see the header).
  bool device_built = false;
  // K0-d as a prefix of the engine's own command buffer. From the truth table (S-7f-5j) that is
  // correct in exactly ONE cell: the device builds AND the device inverts, because that is the only
  // route on which the host neither reads nor writes A or R_hp between the two command buffers. The
  // other three keep the standalone build - with a host inversion the host must read the device's A
  // to invert it in place, so the build has to complete first.
  std::optional<metal::mmse_engine::corr_stage> corr_prefix;
  if (device_stats != nullptr) {
    // DM-RS slot symbols, rebuilt from the stage's own pattern: this function works from the block
    // geometry (npt), while the descriptor needs the slot INDICES (the time correlation depends on
    // them, not only on their count).
    static_vector<unsigned, MAX_NOF_DMRS_SYMBOLS> dmrs_slots;
    args.pattern_symbols.for_each(args.first_symbol, args.last_symbol, [&](unsigned s) { dmrs_slots.push_back(s); });
    // The device build writes the L x L / nout x L blocks with the SLOT strides (st.L / st.nout), so
    // the batch's matrices land exactly where the host staging and every consumer expect them.
    if (gpu_invert) {
      unsigned nout_c = 0;
      unsigned L_c    = 0;
      corr_prefix     = correlation_stage(*device_stats,
                                      args.dmrs_patterns.front().re_pattern,
                                      b_prb,
                                      span<const unsigned>(dmrs_slots.begin(), dmrs_slots.size()),
                                      scs_to_khz(args.scs),
                                      sys_offset,
                                      nout_c,
                                      L_c,
                                      st.L,
                                      st.nout);
      // The slots WILL hold A and R_hp: the device writes them in this batch's command buffer, so
      // the host must not stage them (it would race the device).
      device_built = true;
    } else {
      device_built = build_slots_on_device(*device_stats,
                                         args.dmrs_patterns.front().re_pattern,
                                         b_prb,
                                         span<const unsigned>(dmrs_slots.begin(), dmrs_slots.size()),
                                         scs_to_khz(args.scs),
                                         sys_offset,
                                         nof_layers,
                                         st.L,
                                           st.nout,
                                           L,
                                           gpu_invert);
    }
  }
  // The host staging runs EXACTLY when the device did not fill these slots: A and R_hp are written
  // together (see stage_engine_group()). The extra stride test is what makes the device build
  // equivalent when it fills them: the kernel writes the L x L / nout x L blocks only, so a slot
  // WIDER than the block (the matrix flavor's ceil8 pad) also needs the host's pad rows/columns in
  // their final state - and with st.L == L and st.nout == nout there is no pad to speak of.
  const bool slots_filled = device_built && (st.L == L) && (st.nout == nout);
  if (device_stats != nullptr && !device_built && !matrix) {
    // S-7f-5v: on this route the caller did NOT build the host arrays - it skipped them precisely
    // because this call was going to fill those slots - so staging "what the host has" would copy
    // the PREVIOUS geometry's matrices, or nothing at all. Fail the batch instead: the caller's CPU
    // block path rebuilds its own matrices and computes these blocks itself.
    logger.warning("[mmse_ce] device correlation build failed (systems={} L={}): the blocks go to the CPU path",
                   nof_layers,
                   L);
    return false;
  }
  stage_engine_group(args,
                     gb_start,
                     n_blk,
                     b_prb,
                     npt,
                     nout,
                     L,
                     sys_offset,
                     st,
                     matrix,
                     gpu_invert,
                     slots_filled);
  const bool deferred = defer && !matrix;
  // gpu_invert is THE decision (device_inverts()); it must be passed through, not re-derived from
  // whether the device built the slots. Deriving it from `slots_filled` was a defect: the two are
  // independent (a device build happens whether or not the device also inverts), so the staging was
  // told to leave A in the slots while the engine call was told the slots held A^-1 - the weights
  // then read a raw A (SINR 24 -> 5.4 dB).
  if (!engine_run(corr_prefix.has_value() ? &corr_prefix.value() : nullptr,
                  nout,
                  L,
                  nof_layers,
                  n_blk,
                  matrix,
                  gpu_invert,
                  reformat,
                  deferred,
                  st,
                  sys_offset)) {
    return false;
  }
  if (deferred) {
    defer_unpack(gb_start, n_blk, b_prb, nout, nof_layers, sys_offset, st);
  } else {
    // Inline route: see the note at the other inline unpack - the whole grid, as before S-7f-6a.
    unpack_engine_group(gb_start, n_blk, b_prb, nout, nof_layers, sys_offset, st, /*all_symbols=*/true);
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
  // ... and WHICH command buffer it went into, so that the completion takes the matching route: the
  // batch is in the shared burst for a fused hop, in the engine's own command buffer otherwise
  // (fused_burst_hop is constant for the whole hop, see apply_fd_td_estimation_stage()).
  pending_fused_burst = fused_burst_hop;
}

void port_channel_estimator_metal_mmse_impl::pending_fill::fill(
    const static_re_buffer<MAX_LAYERS * MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS>& grid) const
{
  // An extra 1 / beta in this path inflated RSrp by 1 / beta^2 and the noise variance by about
  // 1 / beta^4, i.e. 17 dB of missing soft bits with the 0.708 of a real cell, so the values are
  // taken as they are (the pilots were already scaled in the data domain).
  //
  // Only the DM-RS pilots are derived: freq_response used to be copied here as well, and it is dead
  // in this backend - the classical get_symbol_ch_estimate() that reads it is overridden by this
  // class (which serves its consumers out of grid_est), so the copy only wrote a buffer nobody
  // read (S-7f-6a).
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
  const bool ok = [&] {
    if (engine == nullptr) {
      return true;
    }
    // The route this hop's dispatches took decides what completes them (S-7g-19):
    //  * burst: they are in the command buffer the equalizer and the demapper share. Normally the lane
    //    committed it before the estimator is asked to complete the hop, and both calls below are then
    //    no-ops. A caller that reads the estimates on the HOST is the exception, and the reason this is
    //    not just a wait: the demodulator completes the estimation BEFORE it submits the equalization on
    //    its not-in-place route (OCUDU_CE_CPU_CE, a missing device view), and so does the debug capture -
    //    nobody has committed the burst yet, and completing it here is what makes the two scalars below
    //    (sigma2, pilots_power) come from memory the GPU has written instead of from the previous hop's.
    //    Those dispatches are this engine's own - no other stage has encoded into the burst at either
    //    call site - so the early commit costs the overlap, not the ordering;
    //  * event and host_wait: they are in the engine's OWN command buffer, committed as soon as they were
    //    encoded. wait_pending() collects it, and on the in-place route (the lane burst waited for it
    //    through the back-end stage fence) it has long completed: the two scalars are then read from
    //    memory the GPU wrote, without the host having waited for it in the middle of the lane.
    return pending_fused_burst ? engine->complete_fused_burst() : engine->wait_pending();
  }();
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
  // Which symbols have to be in the host grid NOW: all of them whenever a host consumer may read
  // it, which is exactly when the DEVICE estimates do not cover this hop - the same signal the
  // demodulator reads (device_results_cover_last_estimate()), and measured: the split-tail route
  // takes the host route for every estimate it binds (ch_est device=0 staged=11), while the merged
  // route binds the device's (device=10593 host=0). The slot hopping is the other case: hop 0's
  // estimates must be out before hop 1's batch overwrites the device buffers they would come from.
  //
  // Otherwise only the DM-RS symbols are unpacked (the hop statistics read their pilots) and the
  // rest waits for the first get_symbol_ch_estimate() call.
  const bool publish_all_grid = unpack_hopping || !gpu_ce_ready;
  for (unsigned i = 0; i != nof_pending_unpacks; ++i) {
    const pending_unpack& u = pending_unpacks[i];
    unpack_engine_group(
        u.gb_start, u.n_blk, u.b_prb, u.nout, u.nof_layers, u.sys_offset, u.st, /*all_symbols=*/publish_all_grid);
  }
  nof_host_unpacks  = nof_pending_unpacks;
  host_unpacks      = pending_unpacks;
  host_grid_pending = !publish_all_grid;
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
  // A host consumer asked for the estimates: materialize the part of the grid the completion left
  // to whoever needed it (see materialize_host_grid()).
  materialize_host_grid();
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
  // See the other overload: this is a host consumer of the estimates.
  materialize_host_grid();
  span<const cf_t> src = grid_est.get_slice(tx_layer * MAX_NSYMB_PER_SLOT + i_symbol);
  unsigned         j   = 0;
  re_mask.for_each(0, re_mask.size(), [&](unsigned i_re) { symbol[j++] = to_cbf16(src[i_re]); });
}
