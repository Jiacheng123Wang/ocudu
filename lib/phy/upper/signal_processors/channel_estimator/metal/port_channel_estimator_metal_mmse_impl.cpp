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
#include <mutex>

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
  std::atomic<uint64_t> sigma2_us{0};
  std::atomic<uint64_t> corr_us{0};
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
                   "mean total=%.1fus sigma2=%.1fus corr=%.1fus gpu_path=%.1fus (gpu_wait=%.1fus) "
                   "cpu_blocks=%.1fus | device_hops=%llu max total=%lluus\n",
                   static_cast<unsigned long long>(n),
                   static_cast<unsigned long long>(s.hops_gpu.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.hops_no_gpu.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.hops_nn.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(s.fallback_blocks.load(std::memory_order_relaxed)),
                   avg(s.total_us),
                   avg(s.sigma2_us),
                   avg(s.corr_us),
                   avg(s.gpu_path_us),
                   avg(s.gpu_wait_us),
                   avg(s.cpu_blocks_us),
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

void mmse_stats_accumulate(bool     hop_gpu,
                           bool     hop_nn,
                           unsigned fallback_blocks,
                           double   sigma2_us,
                           double   corr_us,
                           double   gpu_path_us,
                           double   gpu_wait_us,
                           double   cpu_blocks_us,
                           double   total_us)
{
  mmse_stats_register_atexit();
  mmse_time_stats& s = mmse_stats();
  s.calls.fetch_add(1, std::memory_order_relaxed);
  (hop_gpu ? s.hops_gpu : s.hops_no_gpu).fetch_add(1, std::memory_order_relaxed);
  if (hop_nn) {
    s.hops_nn.fetch_add(1, std::memory_order_relaxed);
  }
  s.fallback_blocks.fetch_add(fallback_blocks, std::memory_order_relaxed);
  s.sigma2_us.fetch_add(static_cast<uint64_t>(sigma2_us), std::memory_order_relaxed);
  s.corr_us.fetch_add(static_cast<uint64_t>(corr_us), std::memory_order_relaxed);
  s.gpu_path_us.fetch_add(static_cast<uint64_t>(gpu_path_us), std::memory_order_relaxed);
  s.gpu_wait_us.fetch_add(static_cast<uint64_t>(gpu_wait_us), std::memory_order_relaxed);
  s.cpu_blocks_us.fetch_add(static_cast<uint64_t>(cpu_blocks_us), std::memory_order_relaxed);
  s.total_us.fetch_add(static_cast<uint64_t>(total_us), std::memory_order_relaxed);
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
  gpu_ce    = alloc_aligned<uint16_t>(static_cast<std::size_t>(MAX_LAYERS) * MAX_NOF_PRBS *
                                   NOF_SUBCARRIERS_PER_RB * MAX_NSYMB_PER_SLOT * 2);

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
  // debug line.
#if defined(OCUDU_CE_TIME)
  const bool time_en = true;
#else
  const bool time_en = false;
#endif
  using steady_clock = std::chrono::steady_clock;
  const auto t_begin = steady_clock::now();

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
  const auto  t_sigma2 = steady_clock::now();

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

  // Weight matrices of the standard blocks (shared by all layers in v1).
  unsigned L_std    = 0;
  unsigned nout_std = 0;
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
  const auto t_corr_std = steady_clock::now();

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
  // Largest tail block order (L = pilots per block) the CPU reference path is known to be fast for
  // (a 36x36 Gauss-Jordan is ~23k FLOPs). Only consulted by the OCUDU_CE_TAIL_CPU A/B knob.
  static constexpr unsigned MAX_CPU_TAIL_ORDER = 36;
  bool       hop_gpu   = false; // engine processed this hop (any block)
  bool       hop_nn    = false; // the simdgroup 8x8 (matrix) kernels were the ones used
  unsigned   hop_pad   = 0;     // ceil8(L) - L of the last matrix batch (A/B pad overhead)
  unsigned   tail_L    = 0;     // block L of the tail batch (log aid for std-less hops)
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
    metal::mmse_engine::reformat_stage reformat{};
    reformat.dst         = gpu_ce;
    reformat.offsets     = re_offsets.data();
    reformat.nof_symbols = MAX_NSYMB_PER_SLOT;
    reformat.drpp          = gpu_ce_drpp;
    reformat.drpp_dmrs     = gpu_ce_drpp_dmrs;
    reformat.dmrs_re_bits  = gpu_ce_dmrs_re_bits;
    reformat.dmrs_sym_bits = gpu_ce_dmrs_sym_bits;
    reformat.total_re    = nof_re_total;
    reformat.dc_sc       = dc_sc;
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
      static constexpr unsigned MAX_GPU_INVERT_ORDER = 36;
      const bool gpu_invert = (std::getenv("OCUDU_CE_CPU_INVERT") == nullptr) && (L_std <= MAX_GPU_INVERT_ORDER);
      // 1) Stage the standard group while w_r_pp / w_r_hp still hold the standard matrices.
      stage_engine_group(args, 0, n_std_blocks, block_prb, npt, nout_std, L_std, 0, st, matrix_on, gpu_invert);
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
      // 4) ONE engine call over both groups, then unpack both.
      const bool merged_ok = engine_run(nout_std,
                                        L_std,
                                        2 * nof_layers,
                                        n_std_blocks,
                                        false,
                                        gpu_invert,
                                        reformat_for(block_prb * NOF_SUBCARRIERS_PER_RB,
                                                     rem_prb * NOF_SUBCARRIERS_PER_RB,
                                                     nof_layers));
      std_blocks_ok        = merged_ok;
      tail_ok              = merged_ok;
      hop_gpu              = merged_ok;
      hop_nn               = false;
      gpu_ce_ready         = merged_ok && (nof_re_total != 0);
      gpu_ce_layers        = nof_layers;
      gpu_ce_total_re      = nof_re_total;
      if (merged_ok) {
        unpack_engine_group(0, n_std_blocks, block_prb, nout_std, nof_layers, 0, st);
        unpack_engine_group(n_std_blocks * block_prb, 1, rem_prb, nout_e, nof_layers, nof_layers, st);
      }
    } else {
      if (n_std_blocks != 0) {
        // Standard blocks: correlation matrices already built above (w_r_pp/w_r_hp, L_std/nout_std).
        // A hop without an edge block (or with its tail on the CPU) is covered by this single
        // batch, so K3 can be attached to it.
        const bool covers_hop = (rem_prb == 0);
        std_blocks_ok         = run_engine_blocks(args,
                                         0,
                                         n_std_blocks,
                                         block_prb,
                                         nout_std,
                                         L_std,
                                         npt,
                                         matrix_on,
                                         covers_hop ? reformat_for(block_prb * NOF_SUBCARRIERS_PER_RB, 0, 0)
                                                    : nullptr);
        hop_gpu               = std_blocks_ok;
        hop_nn                = std_blocks_ok && matrix_on;
        hop_pad               = (std_blocks_ok && matrix_on) ? static_cast<unsigned>(((L_std + 7u) & ~7u) - L_std) : 0;
        gpu_ce_ready          = std_blocks_ok && covers_hop && (nof_re_total != 0);
        gpu_ce_layers         = nof_layers;
        gpu_ce_total_re       = nof_re_total;
      }
      if (rem_prb != 0) {
        // Tail/edge block - and when nof_prb < block_prb this is the WHOLE hop (single block).
        //
        // The tail block runs on the ENGINE, like every other block of the hop: PHY compute
        // belongs to the Metal path as a whole. The second commit+wait the chain used to pay for
        // it is what the merged batch above removes; OCUDU_CE_SPLIT_TAIL=1 keeps the split form
        // for the A/B of that merge.
        if (!tail_on_cpu) {
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
                                      covers_hop ? reformat_for(rem_prb * NOF_SUBCARRIERS_PER_RB, 0, 0) : nullptr);
          hop_gpu               = hop_gpu || tail_ok;
          hop_nn                = hop_nn || (tail_ok && matrix_on);
          hop_pad               = (tail_ok && matrix_on) ? static_cast<unsigned>(((L_e + 7u) & ~7u) - L_e) : hop_pad;
          if (covers_hop) {
            gpu_ce_ready    = tail_ok && (nof_re_total != 0);
            gpu_ce_layers   = nof_layers;
            gpu_ce_total_re = nof_re_total;
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

  const auto t_gpu_end = steady_clock::now();

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
  unsigned cpu_fallback_blocks = 0;

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

  const auto t_cpu_end = steady_clock::now();

  // Fill the filtered-pilots buffer (the estimated pilot REs, already in the data domain because the
  // pilots were scaled above) for RSrp / noise / TA, and the classical frequency response with the
  // DM-RS symbol slices of the grid. An extra 1 / beta here inflated RSrp by 1 / beta^2 and the
  // noise variance by about 1 / beta^4, i.e. 17 dB of missing soft bits with the 0.708 of a real
  // cell.
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    const auto& re_pattern = args.dmrs_patterns[i_layer].re_pattern;
    for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
      const unsigned slot_sym = dmrs_sym[i_symbol];
      span<const cf_t> src    = grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + slot_sym);
      span<cf_t>       dst    = args.filtered_pilots_lse_view.get_symbol(i_symbol, i_layer);
      unsigned         j      = 0;
      for (unsigned prb = 0; prb != nof_prb; ++prb) {
        re_pattern.for_each(0, re_pattern.size(), [&](unsigned pos) {
          dst[j++] = src[prb * NOF_SUBCARRIERS_PER_RB + pos];
        });
      }
    }
    for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
      const unsigned slot_sym = dmrs_sym[i_symbol];
      ocuduvec::copy(args.freq_response.get_symbol(i_symbol, i_layer),
                     grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + slot_sym));
    }
  }

  if (time_en) {
    const auto t_finish = steady_clock::now();
    const auto us       = [](auto d) { return std::chrono::duration<double, std::micro>(d).count(); };
    logger.debug("[mmse_time] prb={} npt={} L={} n_std={} gpu={} nn={} pad={} fb={} | sigma2={:.1f}us corr_std={:.1f}us "
                 "gpu_path={:.1f}us (gpu_wait={:.1f}us) cpu_blocks={:.1f}us finish={:.1f}us | total={:.1f}us",
                 nof_prb,
                 npt,
                 (n_std_blocks != 0 || tail_L == 0) ? L_std : tail_L,
                 n_std_blocks,
                 hop_gpu ? 1 : 0,
                 hop_nn ? 1 : 0,
                 hop_pad,
                 cpu_fallback_blocks,
                 us(t_sigma2 - t_begin),
                 us(t_corr_std - t_sigma2),
                 us(t_gpu_end - t_corr_std),
                 engine_ready ? engine->last_gpu_wait_us() : 0.0,
                 us(t_cpu_end - t_gpu_end),
                 us(t_finish - t_cpu_end),
                 us(t_finish - t_begin));

    // Aggregated summary (printed once at exit, stderr) - the debug line above is only usable
    // interactively because it emits tens of lines per slot.
    mmse_stats_accumulate(hop_gpu,
                          hop_nn,
                          cpu_fallback_blocks,
                          us(t_sigma2 - t_begin),
                          us(t_corr_std - t_sigma2),
                          us(t_gpu_end - t_corr_std),
                          engine_ready ? engine->last_gpu_wait_us() : 0.0,
                          us(t_cpu_end - t_gpu_end),
                          us(t_finish - t_begin));
  }
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
                                                                bool                               gpu_invert)
{
  const unsigned nof_layers = args.dmrs_patterns.size();
  const unsigned npf        = b_prb * args.dmrs_patterns.front().re_pattern.count();
  const unsigned Ls         = st.L;
  const unsigned Ns         = st.nout;
  ocudu_assert((L <= Ls) && (nout <= Ns), "Engine slot strides must cover the block geometry.");

  // A per system: K1 inverts it in place (gpu_invert), otherwise the CPU inverse is staged.
  for (unsigned sys = 0; sys != nof_layers; ++sys) {
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
              args.pilots_lse_view.get_symbol(i_symbol, i_layer).subspan((gb_start + b) * npf, npf);
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
              args.pilots_lse_view.get_symbol(i_symbol, i_layer).subspan((gb_start + b) * npf, npf);
          for (unsigned j = 0; j != npf; ++j) {
            yp[2 * (i_symbol * npf + j)]     = src[j].real();
            yp[2 * (i_symbol * npf + j) + 1] = src[j].imag();
          }
        }
      }
    }
  }
}

bool port_channel_estimator_metal_mmse_impl::engine_run(unsigned nout,
                                                        unsigned L,
                                                        unsigned nof_systems,
                                                        unsigned nof_blocks,
                                                        bool     matrix,
                                                        bool     gpu_invert,
                                                        const metal::mmse_engine::reformat_stage* reformat)
{
  // Weight (W = R_hp . A^-1) + apply (h = W . y) in ONE engine command buffer, with the inversion
  // (K1) prepended in the same buffer when the A slots hold A itself, and the equalizer's
  // per-symbol estimates (K3) appended to the same buffer when the caller asked for them. The
  // engine return value is checked (S-1 audit fix): on failure the caller falls back to the CPU
  // reference math for these blocks instead of unpacking stale gpu_h contents.
  const bool engine_ok =
      matrix ? engine->run_nn(gpu_a, gpu_r_hp, gpu_w, gpu_qy, gpu_h, nout, L, nof_systems, nof_blocks)
             : (gpu_invert ? engine->run(gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, nout, L, nof_systems, nof_blocks, reformat)
                           : engine->run_weights_only(
                                 gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, nout, L, nof_systems, nof_blocks, reformat));
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
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned b = 0; b != n_blk; ++b) {
      // Slot addressing uses the BATCH strides: they may exceed the block geometry (the merged
      // tail system keeps the standard Ls/Ns/n_blk slots while its own block is narrower), and
      // reading it with the block geometry silently unpacks the wrong rows.
      const float* hp = gpu_h + (static_cast<std::size_t>(sys_offset + i_layer) * st.n_blk + b) * 2 * st.nout;
      for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
        span<cf_t> dst = grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + sym)
                             .subspan(static_cast<std::size_t>(gb_start + b) * nf, nf);
        for (unsigned sc = 0; sc != nf; ++sc) {
          dst[sc] = {hp[2 * (sym * nf + sc)], hp[2 * (sym * nf + sc) + 1]};
        }
      }
    }
  }
}

bool port_channel_estimator_metal_mmse_impl::run_engine_blocks(const fd_td_estimation_stage_args& args,
                                                               unsigned                           gb_start,
                                                               unsigned                           n_blk,
                                                               unsigned                           b_prb,
                                                               unsigned                           nout,
                                                               unsigned                           L,
                                                               unsigned                           npt,
                                                               bool                               matrix,
                                                               const metal::mmse_engine::reformat_stage* reformat)
{
  const unsigned nof_layers = args.dmrs_patterns.size();
  // Slot strides: the legacy kernels use the compact L / nout layout; the matrix kernels use the
  // zero-padded ceil8 strides (Lp/Np) - the staging zeroes the pad rows/columns below so the
  // kernels tile 8x8 seamlessly and the pad regions stay exactly zero.
  const engine_strides st{matrix ? ((L + 7u) & ~7u) : L, matrix ? ((nout + 7u) & ~7u) : nout, n_blk};

  // A is symmetric positive definite (see ocudu_mmse_inv.metal), so K1 needs no pivoting; it is
  // fixed to n <= 36 by its threadgroup memory and unused by the nn flavor (ceil8-padded layout).
  // The inversion runs on the GPU: K1 (ocudu_mmse_inv.metal) is a blocked Gauss-Jordan (b=8, no
  // pivoting) and costs 24.5 us for one 36x36 system, down from 91.3 us as a per-pivot barrier
  // chain (S-5a). It is encoded in the SAME command buffer as the weights and the apply
  // (engine_run() below), so the host never reads the inverse back and pays no extra round trip.
  //
  // It is still a few us slower end to end than the ~10 us of host-side CPU Gauss-Jordan in the
  // chain as wired today (the local A/B: hop mean 169.0 -> 182.4 us), and it stays the default
  // anyway: PHY compute belongs to the Metal path as a whole, so the remaining gap is a kernel
  // engineering item (the 72 pivot phases of the diagonal blocks, see the kernel header), not a
  // reason to move the work back to the CPU. OCUDU_CE_CPU_INVERT=1 forces the CPU path for A/B.
  static constexpr unsigned MAX_GPU_INVERT_ORDER = 36;
  const bool cpu_invert_forced = (std::getenv("OCUDU_CE_CPU_INVERT") != nullptr);
  const bool gpu_invert        = !matrix && !cpu_invert_forced && (L <= MAX_GPU_INVERT_ORDER);

  stage_engine_group(args, gb_start, n_blk, b_prb, npt, nout, L, 0, st, matrix, gpu_invert);
  if (!engine_run(nout, L, nof_layers, n_blk, matrix, gpu_invert, reformat)) {
    return false;
  }
  unpack_engine_group(gb_start, n_blk, b_prb, nout, nof_layers, 0, st);
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
