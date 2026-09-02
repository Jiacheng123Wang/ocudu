// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// (Derives from the upstream port_channel_estimator_average_impl base class.)

#include "port_channel_estimator_metal_mmse_impl.h"
#include "../port_channel_estimator_helpers.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/math/math_utils.h"
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
float* alloc_aligned(std::size_t n)
{
  return new (std::align_val_t(4096)) float[n]();
}

void free_aligned(float* p)
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
    bool                                                 compensate_cfo_) :
  port_channel_estimator_average_impl(std::move(interp),
                                      std::move(ta_estimator_),
                                      port_channel_estimator_fd_smoothing_strategy::none,
                                      // Force the interpolate strategy so the LSE pilots are kept per
                                      // DM-RS symbol (the 2D estimator handles time by itself).
                                      port_channel_estimator_td_interpolation_strategy::interpolate,
                                      compensate_cfo_),
  max_blocks(divide_ceil(MAX_NOF_PRBS, std::min(std::max(block_prb_, 1U), MAX_BLOCK_PRB))),
  stats_estimator(std::move(stats_estimator_)),
  block_prb(std::min(std::max(block_prb_, 1U), MAX_BLOCK_PRB))
{
  ocudu_assert(stats_estimator, "Invalid channel statistics estimator.");

  // Metal compute engine (K1/K2); the CPU reference math below is the automatic fallback.
  // OCUDU_MMSE_NOGPU=1 forces the CPU path (diagnostics).
  engine       = std::make_unique<metal::mmse_engine>();
  engine_ready = (std::getenv("OCUDU_MMSE_NOGPU") == nullptr) && engine->init();
  if (std::getenv("OCUDU_MMSE_DBG") != nullptr) {
    logger.debug("[mmse_ce] engine {} (NOGPU={})",
                 engine_ready ? "READY - GPU hot path active" : "UNAVAILABLE - CPU fallback path",
                 std::getenv("OCUDU_MMSE_NOGPU") != nullptr ? 1 : 0);
  }
  gpu_a        = alloc_aligned(static_cast<std::size_t>(MAX_LAYERS) * MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS);
  gpu_r_hp     = alloc_aligned(static_cast<std::size_t>(MAX_LAYERS) * MAX_BLOCK_OUT * MAX_BLOCK_PILOTS);
  gpu_w        = alloc_aligned(static_cast<std::size_t>(MAX_LAYERS) * MAX_BLOCK_OUT * MAX_BLOCK_PILOTS);
  gpu_y        = alloc_aligned(static_cast<std::size_t>(MAX_LAYERS) * max_blocks * 2 * MAX_BLOCK_PILOTS);
  gpu_h        = alloc_aligned(static_cast<std::size_t>(MAX_LAYERS) * max_blocks * 2 * MAX_BLOCK_OUT);

  // Warm-up dispatch: Metal JIT-compiles the kernels and pays the first command-buffer
  // commit on first use (~3 ms on Apple Silicon, see PLAN.md 7.0.11/7.0.12). Running it
  // here, with the FULL staging-buffer capacities, moves that cost off the slot critical
  // path AND populates the zero-copy buffer cache with entries large enough for every
  // later call (the cache is keyed by pointer). Buffers are zero-initialized.
  if (engine_ready) {
    (void)engine->run_weights_only(
        gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, MAX_BLOCK_OUT, MAX_BLOCK_PILOTS, MAX_LAYERS, max_blocks);
  }
}

port_channel_estimator_metal_mmse_impl::~port_channel_estimator_metal_mmse_impl()
{
  free_aligned(gpu_a);
  free_aligned(gpu_r_hp);
  free_aligned(gpu_w);
  free_aligned(gpu_y);
  free_aligned(gpu_h);
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

  // Copy and scale the LSE pilots the same way the classical FD stage does (1 / beta).
  const float total_scaling = 1.0F / args.beta_scaling;
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned i_symbol = 0; i_symbol != nof_dmrs_symbols; ++i_symbol) {
      ocuduvec::copy(tmp_lse.get_symbol(i_symbol, i_layer), args.pilots_lse_view.get_symbol(i_symbol, i_layer));
      ocuduvec::sc_prod(tmp_lse.get_symbol(i_symbol, i_layer), tmp_lse.get_symbol(i_symbol, i_layer), total_scaling);
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

  // --- A = R_pp + sigma2 I + ridge I = kron(R_t_pp, R_f_pp) + (sigma2 + ridge) I ---
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

  // Per-phase timing (OCUDU_CE_TIME=1; OCUDU_MMSE_TIME kept as legacy alias):
  // sigma2 / corr-build / GPU / CPU-blocks / finish.
  const bool           time_en  = std::getenv("OCUDU_CE_TIME") != nullptr ||
                               std::getenv("OCUDU_MMSE_TIME") != nullptr;
  using steady_clock            = std::chrono::steady_clock;
  const auto t_begin            = steady_clock::now();

  // Classical noise variance (reuses the existing noise estimator).
  const float sigma2 = estimate_sigma2(args);
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
  stats_in.sigma2            = sigma2;
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

  const bool gpu_std = engine_ready && (n_std_blocks != 0);
  if (gpu_std) {
    // K1 -> K1b -> K2 in ONE command buffer (per-layer slots are ready for the v2 statistics).
    // NOTE: the system-slot strides MUST be L_std (a_inv) and nout_std*L_std (r_hp/w) - the GPU
    // kernels index the slots with strides p.L and p.nout*p.L, NOT the MAX_BLOCK_* capacities.
    for (unsigned sys = 0; sys != nof_layers; ++sys) {
      std::memcpy(gpu_a + static_cast<std::size_t>(sys) * L_std * L_std,
                  w_r_pp.data(),
                  static_cast<std::size_t>(L_std) * L_std * sizeof(float));
      std::memcpy(gpu_r_hp + static_cast<std::size_t>(sys) * nout_std * L_std,
                  w_r_hp.data(),
                  static_cast<std::size_t>(nout_std) * L_std * sizeof(float));
    }

    // Pack the pilot vectors of all layers and standard blocks.
    const unsigned npf_std = block_prb * args.dmrs_patterns.front().re_pattern.count();
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned b = 0; b != n_std_blocks; ++b) {
        float* yp = gpu_y + (static_cast<std::size_t>(i_layer) * n_std_blocks + b) * 2 * L_std;
        for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
          span<const cf_t> src =
              args.pilots_lse_view.get_symbol(i_symbol, i_layer).subspan(b * block_prb * 6, block_prb * 6);
          for (unsigned j = 0; j != npf_std; ++j) {
            yp[2 * (i_symbol * npf_std + j)]     = src[j].real();
            yp[2 * (i_symbol * npf_std + j) + 1] = src[j].imag();
          }
        }
      }
    }

    // v1 hot path: A^-1 on the CPU (us-level; the batched 36x36 GPU Gauss-Jordan kernel is
    // barrier-bound, see PLAN.md 7.0.6), then K1b (weights) + K2 (apply) in ONE GPU command buffer.
    for (unsigned sys = 0; sys != nof_layers; ++sys) {
      // CPU Gauss-Jordan on a copy of the system's A (gpu_a slot keeps the input for the engine).
      std::array<float, 2 * MAX_BLOCK_PILOTS * MAX_BLOCK_PILOTS> gj;
      std::fill(gj.begin(), gj.end(), 0.0F);
      for (unsigned r = 0; r != L_std; ++r) {
        for (unsigned col = 0; col != L_std; ++col) {
          gj[r * 2 * L_std + col] = w_r_pp[r * L_std + col];
        }
        gj[r * 2 * L_std + L_std + r] = 1.0F;
      }
      gauss_jordan_invert(span<float>(gj.data(), 2 * L_std * L_std), L_std);
      for (unsigned r = 0; r != L_std; ++r) {
        for (unsigned col = 0; col != L_std; ++col) {
          gpu_a[static_cast<std::size_t>(sys) * L_std * L_std + r * L_std + col] =
              gj[r * 2 * L_std + L_std + col];
        }
      }
    }
    engine->run_weights_only(gpu_a, gpu_r_hp, gpu_w, gpu_y, gpu_h, nout_std, L_std, nof_layers, n_std_blocks);

    // Unpack the block outputs into the full grid (symbol-major within the block).
    const unsigned nf_std = block_prb * NOF_SUBCARRIERS_PER_RB;
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      for (unsigned b = 0; b != n_std_blocks; ++b) {
        const float* hp = gpu_h + (static_cast<std::size_t>(i_layer) * n_std_blocks + b) * 2 * nout_std;
        for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
          span<cf_t> dst =
              grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + sym).subspan(b * block_prb * 12, nf_std);
          for (unsigned sc = 0; sc != nf_std; ++sc) {
            dst[sc] = {hp[2 * (sym * nf_std + sc)], hp[2 * (sym * nf_std + sc) + 1]};
          }
        }
      }
    }
  }

  const auto t_gpu_end = steady_clock::now();

  // CPU path for the standard blocks (engine unavailable) and for the edge block.
  const unsigned cpu_first = gpu_std ? n_std_blocks : 0U;
  const unsigned n_blocks  = n_std_blocks + (rem_prb == 0 ? 0U : 1U);
  for (unsigned b = cpu_first; b != n_blocks; ++b) {
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
      if (std::getenv("OCUDU_MMSE_DBG")) {
        float py = 0.0F, pw = 0.0F;
        for (unsigned j = 0; j != 2 * L; ++j) py += y_block[j] * y_block[j];
        for (unsigned i = 0; i != nout * L; ++i) pw += w_mat[i] * w_mat[i];
        logger.debug("[dbgblk] b={} nout={} L={} sigma2={:.4f} |y|2={:.3f} |W|2={:.3f}", b, nout, L, sigma2, py, pw);
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

  // Fill the filtered-pilots buffer (pilot REs scaled by 1 / beta) for RSrp / noise / TA, and the
  // classical frequency response with the DM-RS symbol slices of the grid.
  const float inv_beta = 1.0F / args.beta_scaling;
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    const auto& re_pattern = args.dmrs_patterns[i_layer].re_pattern;
    for (unsigned i_symbol = 0; i_symbol != npt; ++i_symbol) {
      const unsigned slot_sym = dmrs_sym[i_symbol];
      span<const cf_t> src    = grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + slot_sym);
      span<cf_t>       dst    = args.filtered_pilots_lse_view.get_symbol(i_symbol, i_layer);
      unsigned         j      = 0;
      for (unsigned prb = 0; prb != nof_prb; ++prb) {
        re_pattern.for_each(0, re_pattern.size(), [&](unsigned pos) {
          dst[j++] = src[prb * NOF_SUBCARRIERS_PER_RB + pos] * inv_beta;
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
    logger.debug("[mmse_time] prb={} npt={} L={} n_std={} gpu={} | sigma2={:.1f}us corr_std={:.1f}us "
                 "gpu_path={:.1f}us (gpu_wait={:.1f}us) cpu_blocks={:.1f}us finish={:.1f}us | total={:.1f}us",
                 nof_prb,
                 npt,
                 L_std,
                 n_std_blocks,
                 gpu_std ? 1 : 0,
                 us(t_sigma2 - t_begin),
                 us(t_corr_std - t_sigma2),
                 us(t_gpu_end - t_corr_std),
                 engine_ready ? engine->last_gpu_wait_us() : 0.0,
                 us(t_cpu_end - t_gpu_end),
                 us(t_finish - t_cpu_end),
                 us(t_finish - t_begin));
  }
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
