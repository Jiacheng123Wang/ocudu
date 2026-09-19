// CPU-side algorithmic check of the S-7f-5w kernels (fd smoothing + classical noise variance).
//
// Side A is the REAL host implementation (linked: apply_fd_smoothing() + estimate_noise()), driven by
// the same caller structure as port_channel_estimator_metal_mmse_impl::estimate_sigma2().
// Side B is a mechanical C++ transcription of the two MSL kernels in ocudu_mmse_pilots.metal.
// No GPU is touched: this validates the SEMANTICS (indexing, edge extrapolation, the enlarged-array
// convention, the CDM pairing, the scaling order) before anything is dispatched.
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h"
#include "port_channel_estimator_average_impl.h"
#include "port_channel_estimator_helpers.h"   // lives in the estimator's own directory
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ocudu;

/// Stand-in for MSL's f2 (the kernel port below is a mechanical transcription and keeps its shape).
struct f2 {
  float x, y;
  f2() : x(0.0F), y(0.0F) {}
  f2(float a, float b) : x(a), y(b) {}
};

static constexpr unsigned MAXV   = port_channel_estimator_average_impl::MAX_V_PILOTS;         // 12
static constexpr unsigned MAXP   = port_channel_estimator_average_impl::MAX_NOF_PILOTS_SYMBOL; // 3324
static constexpr unsigned MAXSYM = pusch_constants::MAX_NOF_DMRS_SYMBOLS;                      // 4
static constexpr unsigned MAXLAY = pusch_constants::MAX_NOF_LAYERS;                            // 4

// ---------------------------------------------------------------- side B: the kernel port -----
struct dims {
  unsigned nof_dmrs_symb, nof_layers, nof_pilots, nof_v_pilots, filter_len, nof_cdm;
};

// mmse_sigma2_clamp()
static dims clamp_dims(unsigned nd, unsigned nl, unsigned np, unsigned nv, unsigned fl, unsigned nc)
{
  dims d;
  d.nof_dmrs_symb = std::min(nd, 4u);
  d.nof_layers    = std::min(nl, 4u);
  d.nof_pilots    = std::min(np, 3324u);
  d.nof_v_pilots  = std::min(nv, 12u);
  d.filter_len    = std::min(fl, 31u);
  d.nof_cdm       = std::min(nc, 2u);
  return d;
}

// mmse_sigma2_extended(): lse is the hop layout [symb][layer][pilot]
static cf_t extended(const std::vector<cf_t>& lse, const float* fit, const dims& d, unsigned i_base, int j)
{
  const int npf = (int)d.nof_pilots;
  const int nv  = (int)d.nof_v_pilots;
  if ((j >= nv) && (j < nv + npf)) {
    return lse[(std::size_t)i_base * d.nof_pilots + (unsigned)(j - nv)];
  }
  float x = 0.0F;
  unsigned f = 0;
  if (j < nv) {
    x = (float)(j - nv);
    f = 0;
  } else if (j < nv + npf + nv) {
    x = (float)(j - npf);
    f = 4;
  } else {
    return cf_t(0.0F, 0.0F);
  }
  const float rho   = fit[f + 0] * x + fit[f + 1];
  const float phase = fit[f + 2] * x + fit[f + 3] + ((rho > 0.0F) ? 0.0F : 3.14159265358979323846F);
  return std::polar(std::fabs(rho), phase);
}

// mmse_pilots_fd_smooth(): one (symbol, layer) at a time; also returns the fit coefficients and the
// virtual pilots it computed, so they can be compared with the host's enlarged array.
static std::vector<cf_t> fd_smooth(const std::vector<cf_t>& lse,
                                   const dims&              d,
                                   const float*             filt,
                                   std::array<float, 8>&    fit_out,
                                   std::vector<cf_t>&       v_start_out,
                                   std::vector<cf_t>&       v_end_out)
{
  const unsigned npt = d.nof_dmrs_symb, nlay = d.nof_layers, npf = d.nof_pilots, nv = d.nof_v_pilots;
  std::vector<cf_t> sm((std::size_t)npt * nlay * npf);
  std::vector<cf_t> fit_store((std::size_t)npt * nlay * 8);
  for (unsigned s = 0; s != npt; ++s) {
    for (unsigned l = 0; l != nlay; ++l) {
      const unsigned i_base = s * nlay + l;
      float          fit[8] = {0};
      // thread 0: the two least-squares fits (identical arithmetic to the kernel's)
      for (unsigned which = 0; which != 2u; ++which) {
        const unsigned first  = (which == 0u) ? 0u : (npf - nv);
        const float    nvf    = (float)nv;
        const float    mean_x = nvf * (nvf - 1.0F) / 2.0F / nvf;
        const float    nxsq   = (nvf - 1.0F) * nvf * (2.0F * nvf - 1.0F) / 6.0F;
        const float    denom  = nxsq - nvf * mean_x * mean_x;
        float          sum_abs = 0, sum_arg = 0, dot_abs = 0, dot_arg = 0, arg_prev = 0, k_unwrap = 0;
        for (unsigned t = 0; t != 12u; ++t) {
          if (t >= nv) {
            break;
          }
          const cf_t  v  = lse[(std::size_t)i_base * npf + first + t];
          const float re = v.real(), im = v.imag();
          const float a  = std::atan2(im, re);
          if (t != 0u) {
            const float jump = a - arg_prev;
            if (std::fabs(jump) > 3.14159265358979323846F) {
              k_unwrap -= std::copysign(1.0F, jump);
            }
          }
          const float arg = a + 2.0F * k_unwrap * 3.14159265358979323846F;
          arg_prev        = a;
          const float ab  = std::sqrt(re * re + im * im);
          sum_abs += ab;
          sum_arg += arg;
          dot_abs += ab * (float)t;
          dot_arg += arg * (float)t;
        }
        const float mean_abs  = sum_abs / nvf;
        const float mean_arg  = sum_arg / nvf;
        const float slope_abs = (denom != 0.0F) ? ((dot_abs - mean_x * mean_abs * nvf) / denom) : 0.0F;
        const float slope_arg = (denom != 0.0F) ? ((dot_arg - mean_x * mean_arg * nvf) / denom) : 0.0F;
        fit[which * 4 + 0]    = slope_abs;
        fit[which * 4 + 1]    = mean_abs - slope_abs * mean_x;
        fit[which * 4 + 2]    = slope_arg;
        fit[which * 4 + 3]    = mean_arg - slope_arg * mean_x;
      }
      for (unsigned i = 0; i != 8; ++i) {
        fit_store[(std::size_t)i_base * 8 + i] = fit[i];
      }
      if (i_base == 0) {
        for (unsigned i = 0; i != 8; ++i) {
          fit_out[i] = fit[i];
        }
      }
      // virtual pilots, exactly as mmse_sigma2_extended() would evaluate them
      for (int j = 0; j != (int)nv; ++j) {
        v_start_out.push_back(extended(lse, fit, d, i_base, j));
      }
      for (int j = (int)(nv + npf); j != (int)(nv + npf + nv); ++j) {
        v_end_out.push_back(extended(lse, fit, d, i_base, j));
      }
      // the convolution
      const int center = (int)d.filter_len / 2;
      for (unsigned m = 0; m != npf; ++m) {
        f2 acc = f2(0, 0);
        for (unsigned k = 0; k != 31u; ++k) {
          if (k >= d.filter_len) {
            break;
          }
          const cf_t v = extended(lse, fit, d, i_base, (int)m + (int)nv + center - (int)k);
          acc.x += v.real() * filt[k];
          acc.y += v.imag() * filt[k];
        }
        sm[(std::size_t)i_base * npf + m] = cf_t(acc.x, acc.y);
      }
    }
  }
  return sm;
}

// mmse_pilots_sigma2(): the whole hop, one thread
static std::vector<float> g_port_pair_energy;
static std::vector<float> g_host_pair_energy;

static float sigma2_port(const std::vector<cf_t>& sm,
                         const std::vector<cf_t>& ref,
                         const std::vector<cf_t>& rx,
                         const std::vector<float>& epochs,
                         const unsigned*          dmrs_symb,
                         const dims&              d,
                         float                    beta,
                         bool                     compensate_cfo,
                         float                    cfo)
{
  const float  scaling   = (d.nof_dmrs_symb != 0u) ? (beta / (float)d.nof_dmrs_symb) : 0.0F;
  const unsigned nof_pairs = (d.nof_layers + 1u) / 2u;
  float sigma2 = 0.0F;
  for (unsigned pair = 0; pair != 4u / 2u; ++pair) {
    if ((pair >= nof_pairs) || (d.nof_pilots == 0u) || (d.nof_dmrs_symb == 0u)) {
      break;
    }
    const unsigned l0    = 2u * pair;
    const unsigned l1    = ((l0 + 2u) < d.nof_layers) ? (l0 + 2u) : d.nof_layers;
    const unsigned i_cdm = (d.nof_cdm != 0u) ? std::min(pair, d.nof_cdm - 1u) : 0u;
    const bool     pair2 = (l1 - l0) == 2u;
    float          energy = 0.0F;
    for (unsigned i = 0; i != d.nof_pilots; ++i) {
      f2 scaled0 = f2(0, 0), scaled1 = f2(0, 0);
      for (unsigned s = 0; s != 4u; ++s) {
        if (s >= d.nof_dmrs_symb) {
          break;
        }
        const std::size_t base = ((std::size_t)s * d.nof_layers + l0) * d.nof_pilots + i;
        const cf_t  f0 = sm[base] * scaling;
        scaled0 = (s == 0u) ? f2(f0.real(), f0.imag())
                            : f2(f0.real() + scaled0.x, f0.imag() + scaled0.y);
        if (pair2) {
          const cf_t f1 = sm[base + d.nof_pilots] * scaling;
          scaled1 = (s == 0u) ? f2(f1.real(), f1.imag())
                              : f2(f1.real() + scaled1.x, f1.imag() + scaled1.y);
        }
      }
      for (unsigned s = 0; s != 4u; ++s) {
        if (s >= d.nof_dmrs_symb) {
          break;
        }
        const std::size_t ib = ((std::size_t)s * d.nof_layers + l0) * d.nof_pilots + i;
        f2 ph(1.0F, 0.0F);
        if (compensate_cfo) {
          const unsigned i_slot = std::min(dmrs_symb[s], 14u - 1u);
          const float    theta  = 6.283185307179586F * epochs[i_slot] * cfo;
          ph = f2(std::cos(theta), std::sin(theta));
        }
        const cf_t r0 = ref[ib];
        f2 predicted(r0.real() * scaled0.x - r0.imag() * scaled0.y,
                         r0.real() * scaled0.y + r0.imag() * scaled0.x);
        predicted = f2(predicted.x * ph.x - predicted.y * ph.y, predicted.x * ph.y + predicted.y * ph.x);
        if (pair2) {
          const cf_t r1 = ref[ib + d.nof_pilots];
          f2 p1(r1.real() * scaled1.x - r1.imag() * scaled1.y,
                    r1.real() * scaled1.y + r1.imag() * scaled1.x);
          p1 = f2(p1.x * ph.x - p1.y * ph.y, p1.x * ph.y + p1.y * ph.x);
          predicted = f2(p1.x + predicted.x, p1.y + predicted.y);
        }
        const std::size_t irx = ((std::size_t)s * d.nof_cdm + i_cdm) * d.nof_pilots + i;
        const cf_t        r   = rx[irx];
        const float       nx  = r.real() - predicted.x, ny = r.imag() - predicted.y;
        energy += nx * nx + ny * ny;
      }
    }
    const bool ok = std::isfinite(energy) && (energy != 0.0F);
    g_port_pair_energy.push_back(energy);
    sigma2 += ok ? (energy / (float)(d.nof_pilots * d.nof_dmrs_symb * (l1 - l0))) : 0.0F;
  }
  return (nof_pairs == 0u) ? 0.0F : (sigma2 / (float)nof_pairs);
}

// ---------------------------------------------------------------- the driver --------------------
static int run_case(const char* name, unsigned nof_prb, unsigned npt, unsigned ncomb, unsigned comb_step, unsigned nof_layers, bool cfo_on)
{
  bounded_bitset<NOF_SUBCARRIERS_PER_RB> re_pattern(NOF_SUBCARRIERS_PER_RB);
  for (unsigned i = 0; i != ncomb; ++i) {
    re_pattern.set(i * comb_step);
  }
  const unsigned npf = nof_prb * ncomb; // pilots per symbol
  std::mt19937   rng(1234 + nof_prb * 31 + npt * 7 + nof_layers);
  std::normal_distribution<float> nd(0.0F, 1.0F);

  std::vector<cf_t> pilots((std::size_t)npt * nof_layers * npf);
  std::vector<cf_t> rx((std::size_t)npt * std::max(1u, nof_layers / 2 + nof_layers % 2) * npf);
  std::vector<cf_t> lse((std::size_t)npt * nof_layers * npf);
  for (auto& v : pilots) {
    v = std::polar(1.0F, nd(rng) * 3.14159F);
  }
  for (auto& v : lse) {
    v = cf_t(1.0F + 0.3F * nd(rng), 0.2F * nd(rng));
  }
  for (auto& v : rx) {
    v = cf_t(0.5F * nd(rng), 0.5F * nd(rng));
  }
  std::vector<float> epochs(14);
  for (unsigned i = 0; i != 14; ++i) {
    epochs[i] = 0.07F * (float)i;
  }
  const unsigned cdm   = std::max(1u, (nof_layers + 1u) / 2u);
  const float    beta  = 0.708F;
  const float    cfo   = cfo_on ? 13.5F : 0.0F;
  const unsigned stride = configure_interpolator(re_pattern).stride;

  // --- filter + virtual pilot count, as apply_fd_smoothing() derives them
  std::array<float, 40> filt{};
  const unsigned        flen = get_fd_smoothing_filter(span<float>(filt.data(), filt.size()), nof_prb, stride);
  unsigned              nv   = std::min<unsigned>(MAXV, flen / 2);
  if (nof_prb == 1) {
    nv = npf;
  }
  const dims d = clamp_dims(npt, nof_layers, npf, nv, flen, cdm);

  // --- side A: the host reference (real library functions)
  static_re_measurement<cf_t, MAXP, MAXSYM, MAXLAY> enlarged_in, enlarged_filt;
  enlarged_in.resize({.nof_subc = npf + 2 * MAXV, .nof_symbols = npt, .nof_slices = nof_layers});
  enlarged_filt.resize({.nof_subc = npf + 2 * MAXV, .nof_symbols = npt, .nof_slices = nof_layers});
  modular_re_measurement<cf_t, MAXSYM, MAXLAY> m_in(enlarged_in), m_filt(enlarged_filt);
  m_in.assign(enlarged_in, MAXV, npf);
  m_filt.assign(enlarged_filt, MAXV, npf);
  for (unsigned s = 0; s != npt; ++s) {
    for (unsigned l = 0; l != nof_layers; ++l) {
      for (unsigned j = 0; j != npf; ++j) {
        m_in.get_symbol(s, l)[j] = lse[(std::size_t)(s * nof_layers + l) * npf + j];
      }
      apply_fd_smoothing(enlarged_filt.get_symbol(s, l),
                         enlarged_in.get_symbol(s, l),
                         nof_prb,
                         stride,
                         port_channel_estimator_fd_smoothing_strategy::filter);
    }
  }
  // the host's noise estimator, driven like estimate_sigma2()
  dmrs_symbol_list host_pilots, host_rx;
  host_pilots.resize({.nof_subc = npf, .nof_symbols = npt, .nof_slices = nof_layers});
  host_rx.resize({.nof_subc = npf, .nof_symbols = npt, .nof_slices = cdm});
  for (unsigned s = 0; s != npt; ++s) {
    for (unsigned l = 0; l != nof_layers; ++l) {
      for (unsigned j = 0; j != npf; ++j) {
        host_pilots.get_symbol(s, l)[j] = pilots[(std::size_t)(s * nof_layers + l) * npf + j];
      }
    }
    for (unsigned c = 0; c != cdm; ++c) {
      for (unsigned j = 0; j != npf; ++j) {
        host_rx.get_symbol(s, c)[j] = rx[(std::size_t)(s * cdm + c) * npf + j];
      }
    }
  }
  bounded_bitset<MAX_NSYMB_PER_SLOT> dmrs_mask;
  for (unsigned s = 0; s != npt; ++s) {
    dmrs_mask.set(s);
  }
  float    host_sigma2 = 0.0F;
  unsigned n_pairs     = 0;
  for (unsigned l = 0; l < nof_layers; l += 2U) {
    const unsigned stop = std::min(l + 2U, nof_layers);
    const float    e    = estimate_noise(host_pilots,
                                      host_rx,
                                      m_filt,
                                      beta,
                                      dmrs_mask,
                                      cfo_on ? std::optional<float>(cfo) : std::nullopt,
                                      span<const float>(epochs),
                                      cfo_on,
                                      0,
                                      npt,
                                      0,
                                      l,
                                      stop);
    g_host_pair_energy.push_back(e);
    host_sigma2 += e / (float)(npf * npt * (stop - l));
    ++n_pairs;
  }
  host_sigma2 = (n_pairs == 0) ? 0.0F : host_sigma2 / (float)n_pairs;

  // --- side B: the kernel port
  std::array<float, 8> fit{};
  std::vector<cf_t>     vs, ve, sm;
  sm = fd_smooth(lse, d, filt.data(), fit, vs, ve);
  const std::vector<cf_t> ref_port(pilots.begin(), pilots.end());
  const unsigned          dmrs_symb[4] = {0, 1, 2, 3};
  const float             port_sigma2  = sigma2_port(sm, ref_port, rx, epochs, dmrs_symb, d, beta, cfo_on, cfo);

  // --- compare
  auto rel = [](cf_t a, cf_t b) {
    const double m = std::max(std::abs(a), std::abs(b));
    return (m > 1e-20) ? (std::abs(a - b) / m) : 0.0;
  };
  double max_v = 0.0, max_sm = 0.0;
  for (unsigned s = 0; s != npt; ++s) {
    for (unsigned l = 0; l != nof_layers; ++l) {
      for (unsigned j = 0; j != npf; ++j) {
        max_sm = std::max(max_sm, rel(m_filt.get_symbol(s, l)[j], sm[(std::size_t)(s * nof_layers + l) * npf + j]));
      }
      for (unsigned t = 0; t != nv; ++t) {
        max_v = std::max(max_v, rel(enlarged_in.get_symbol(s, l)[MAXV - nv + t], vs[(std::size_t)(s * nof_layers + l) * nv + t]));
        max_v = std::max(max_v, rel(enlarged_in.get_symbol(s, l)[MAXV + npf + t], ve[(std::size_t)(s * nof_layers + l) * nv + t]));
      }
    }
  }
  const double rel_sigma2 = (std::abs(host_sigma2) > 1e-20) ? std::abs(port_sigma2 - host_sigma2) / std::abs(host_sigma2) : 0.0;
  printf("%-34s prb=%2u npt=%u comb=%u lay=%u stride=%u flen=%2u nv=%2u | virtual max_rel=%.2e | smoothed max_rel=%.2e | "
         "sigma2 host=%.6e port=%.6e rel=%.2e\n",
         name,
         nof_prb,
         npt,
         ncomb,
         nof_layers,
         stride,
         flen,
         nv,
         max_v,
         max_sm,
         host_sigma2,
         port_sigma2,
         rel_sigma2);
  const bool ok = (max_v < 1e-4) && (max_sm < 1e-4) && (rel_sigma2 < 1e-4);
  return ok ? 0 : 1;
}

int main()
{
  int fails = 0;
  // comb 6 = the air configuration (re_pattern {0,2,4,6,8,10}, pilot stride 2)
  fails += run_case("air 25PRB/3sym/comb6/1lay", 25, 3, 6, 2, 1, false);
  fails += run_case("air + cfo compensated", 25, 3, 6, 2, 1, true);
  fails += run_case("two layers (CDM pair)", 25, 3, 6, 2, 2, true);
  fails += run_case("four layers (2 CDM pairs)", 25, 3, 6, 2, 4, true);
  fails += run_case("2 DMRS symbols", 13, 2, 6, 2, 1, true);
  fails += run_case("4 DMRS symbols", 25, 4, 6, 2, 1, true);
  fails += run_case("single PRB (nv = all pilots)", 1, 2, 6, 2, 1, false);
  fails += run_case("comb 4 (stride 3)", 25, 3, 4, 3, 1, false);
  fails += run_case("comb 3 (stride 4)", 25, 3, 3, 4, 1, false);
  if (getenv("DUMP_PAIRS") != nullptr) {
    printf("per-pair noise energy: host=[");
    for (float e : g_host_pair_energy) printf("%.6e ", e);
    printf("] port=[");
    for (float e : g_port_pair_energy) printf("%.6e ", e);
    printf("]\n");
  }
  printf("%s (%d failing case(s))\n", fails == 0 ? "CPU-SIDE ALGORITHM CHECK: PASS" : "CPU-SIDE ALGORITHM CHECK: FAIL", fails);
  return fails == 0 ? 0 : 1;
}
