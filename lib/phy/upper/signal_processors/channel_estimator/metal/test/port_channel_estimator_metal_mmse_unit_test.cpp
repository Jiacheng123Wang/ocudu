// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Unit-level validation of the Metal MMSE channel estimator (v1 CPU reference math):
///   1. Gauss-Jordan inversion golden test vs a double-precision reference.
///   2. Fixed channel-statistics provider test.
///   3. End-to-end NMSE comparison against the classical estimator (port_channel_estimator_average_impl)
///      on synthetic Vehicular-A channels.

#include "../port_channel_estimator_metal_mmse_impl.h"
#include "../ocudu_metal_mmse_engine.h"
#include "port_channel_estimator_helpers.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/ran/resource_allocation/rb_bitmap.h"
#include "ocudu/support/math/math_utils.h"
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

// ---------------------------------------------------------------------------------------
// Minimal resource grid fake: holds one OFDM symbol per (port, symbol) as cbf16 samples.
// ---------------------------------------------------------------------------------------
class grid_fake : public resource_grid_reader
{
public:
  explicit grid_fake(unsigned nof_subc) : symbols(nof_subc) {}

  void set_symbol(unsigned symbol, span<const cf_t> data)
  {
    for (unsigned k = 0; k != data.size(); ++k) {
      symbols[k] = to_cbf16(data[k]);
    }
    has_symbol = true;
  }

  unsigned     get_nof_ports() const override { return 1; }
  unsigned     get_nof_subc() const override { return static_cast<unsigned>(symbols.size()); }
  unsigned     get_nof_symbols() const override { return 1; }
  bool         is_empty(unsigned) const override { return !has_symbol; }
  bool         is_empty() const override { return !has_symbol; }
  crb_interval get_allocation_range(unsigned, unsigned) const override { return {0, 0}; }
  span<cf_t>   get(span<cf_t> symbols_, unsigned, unsigned, unsigned, const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override
  {
    return symbols_;
  }
  span<cbf16_t> get(span<cbf16_t> symbols_, unsigned, unsigned, unsigned, const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override
  {
    return symbols_;
  }
  void get(span<cf_t> symbols_, unsigned, unsigned, unsigned, unsigned = 1) const override
  {
    for (unsigned k = 0; k != symbols_.size(); ++k) {
      symbols_[k] = to_cf(symbols[k]);
    }
  }
  void get(span<cbf16_t> symbols_, unsigned, unsigned, unsigned) const override
  {
    for (unsigned k = 0; k != symbols_.size(); ++k) {
      symbols_[k] = symbols[k];
    }
  }
  span<const cbf16_t> get_view(unsigned, unsigned) const override { return symbols; }

private:
  std::vector<cbf16_t> symbols;
  bool                 has_symbol = false;
};

// ---------------------------------------------------------------------------------------
// Synthetic Vehicular-A channel (ITU-R M.1225 Vehicular-A tap profile, published
// standard parameters): quasi-static over the slot, Rayleigh taps.
// ---------------------------------------------------------------------------------------
struct veha_channel {
  static constexpr std::array<float, 6> DELAYS_NS = {0, 310, 710, 1090, 1730, 2510};
  static constexpr std::array<float, 6> POWERS_DB  = {0, -1, -9, -10, -15, -20};

  explicit veha_channel(std::mt19937& rng)
  {
    std::normal_distribution<float> gauss(0.0F, 1.0F);
    float norm = 0.0F;
    for (unsigned i = 0; i != 6; ++i) {
      pow_lin[i] = std::pow(10.0F, POWERS_DB[i] / 10.0F);
      norm += pow_lin[i];
    }
    for (unsigned i = 0; i != 6; ++i) {
      pow_lin[i] /= norm;
      taps[i] = {gauss(rng), gauss(rng)};
    }
  }

  /// Channel response at subcarrier k (15 kHz SCS, no Doppler).
  cf_t operator()(unsigned k) const
  {
    std::complex<float> acc(0.0F, 0.0F);
    for (unsigned i = 0; i != 6; ++i) {
      const float phase = -TWOPI * DELAYS_NS[i] * 1e-9F * 15000.0F * static_cast<float>(k);
      acc += std::sqrt(pow_lin[i]) * std::complex<float>(taps[i].first, taps[i].second) *
             std::complex<float>(std::cos(phase), std::sin(phase));
    }
    return {acc.real(), acc.imag()};
  }

  std::array<float, 6>                 pow_lin;
  std::array<std::pair<float, float>, 6> taps;
};

// ---------------------------------------------------------------------------------------
// Test fixture utilities.
// ---------------------------------------------------------------------------------------
struct estimator_bundle {
  std::unique_ptr<port_channel_estimator>             est;
  dmrs_symbol_list                                    pilots;
  port_channel_estimator::configuration               cfg;
  std::unique_ptr<time_alignment_estimator>           ta_est;
};

std::unique_ptr<time_alignment_estimator> make_ta_estimator()
{
  auto dft_factory = create_dft_processor_factory_generic();
  auto ta_factory  = create_time_alignment_estimator_dft_factory(dft_factory);
  return ta_factory->create();
}

port_channel_estimator::configuration make_config(unsigned n_prb              = 51,
                                                   bool    two_dmrs_symbols   = true,
                                                   unsigned crb_offset        = 0,
                                                   bool    three_dmrs_symbols = false,
                                                   bool    four_dmrs_symbols  = false)
{
  port_channel_estimator::configuration cfg;
  cfg.scs          = subcarrier_spacing::kHz15;
  cfg.cp           = cyclic_prefix::NORMAL;
  cfg.first_symbol = 0;
  cfg.nof_symbols  = MAX_NSYMB_PER_SLOT;
  cfg.rx_ports.emplace_back(0);
  cfg.scaling      = 1.0F;

  port_channel_estimator::layer_dmrs_pattern pattern;
  pattern.symbols.resize(MAX_NSYMB_PER_SLOT);
  pattern.symbols.set(2);
  if (four_dmrs_symbols) {
    // A/B nn configuration: L = 4 DM-RS symbols x 6 RE x 3 PRB = 72 (8-aligned, simdgroup 8x8 path).
    pattern.symbols.set(7);
    pattern.symbols.set(11);
    pattern.symbols.set(12);
  } else {
    if (two_dmrs_symbols) {
      pattern.symbols.set(11);
    }
    if (three_dmrs_symbols) {
      // E2E cell config: pos2 + additional position 2 -> symbols {2, 7, 11}.
      pattern.symbols.set(7);
    }
  }
  crb_bitmap mask;
  mask.resize(MAX_NOF_PRBS);
  mask |= crb_interval{crb_offset, crb_offset + n_prb};
  pattern.rb_mask = mask;
  pattern.re_pattern.resize(NOF_SUBCARRIERS_PER_RB);
  for (unsigned k = 0; k != 12; k += 2) {
    pattern.re_pattern.set(k);
  }
  cfg.dmrs_pattern.emplace_back(pattern);
  return cfg;
}

dmrs_symbol_list make_pilots(unsigned n_prb = 51, unsigned nof_symbols = 2)
{
  dmrs_symbol_list pilots;
  pilots.resize({.nof_subc = n_prb * 6, .nof_symbols = nof_symbols, .nof_slices = 1});
  for (unsigned s = 0; s != nof_symbols; ++s) {
    span<cf_t> sym = pilots.get_symbol(s, 0);
    for (unsigned i = 0; i != sym.size(); ++i) {
      sym[i] = {M_SQRT1_2, M_SQRT1_2};
    }
  }
  return pilots;
}

} // namespace

int main()
{
  // The fused lane is the DEFAULT route now (OCUDU_CE_FUSED_BURST=0 is the escape hatch), and Test 13
  // pins the knob in both directions: clearing it here keeps every other test on the default whatever
  // the caller's environment says. It changes nothing for them - they all go through compute(), which
  // completes the hop it submits and therefore never fuses.
  unsetenv("OCUDU_CE_FUSED_BURST");

  std::mt19937 rng(1234);

  unsigned n_bad = 0;
  // Engine inversion of every matrix size the estimator can ask for: the same kernel serves the
  // 6x6 of a one-PRB hop and the 36x36 of the production hop, and a size-dependent defect (a block
  // that is only partially filled, a lane outside the last block) shows up as a catastrophic error
  // rather than a few ulp. The reference is a pivoted Gauss-Jordan in double; the tolerance is
  // loose because these random SPD matrices are far worse conditioned than the estimator's (the
  // ridge is 1e-3 against a unit-diagonal correlation), which is what the bound below allows.
  {
    metal::mmse_engine               engine;
    std::normal_distribution<float>  nd(0.0F, 1.0F);
    if (engine.init()) {
      for (unsigned n : {36u, 32u, 24u, 18u, 16u, 12u, 8u, 6u}) {
        std::vector<float> a(static_cast<size_t>(n) * n, 0.0F);
        std::vector<float> r(static_cast<size_t>(n) * n);
        for (auto& v : r) {
          v = nd(rng);
        }
        for (unsigned i = 0; i != n; ++i) {
          for (unsigned j = 0; j != n; ++j) {
            float acc = 0.0F;
            for (unsigned k = 0; k != n; ++k) {
              acc += r[k * n + i] * r[k * n + j];
            }
            a[i * n + j] = acc + ((i == j) ? 1e-3F : 0.0F);
          }
        }
        std::vector<double> m(static_cast<size_t>(2) * n * n, 0.0);
        for (unsigned i = 0; i != n; ++i) {
          for (unsigned j = 0; j != n; ++j) {
            m[i * 2 * n + j] = a[i * n + j];
          }
          m[i * 2 * n + n + i] = 1.0;
        }
        for (unsigned col = 0; col != n; ++col) {
          // Partial pivoting: without it this reference is not accurate enough to judge the kernel.
          unsigned pivot = col;
          for (unsigned i = col + 1; i != n; ++i) {
            if (std::abs(m[i * 2 * n + col]) > std::abs(m[pivot * 2 * n + col])) {
              pivot = i;
            }
          }
          for (unsigned c = 0; c != 2 * n; ++c) {
            std::swap(m[col * 2 * n + c], m[pivot * 2 * n + c]);
          }
          const double piv = m[col * 2 * n + col];
          for (unsigned c = 0; c != 2 * n; ++c) {
            m[col * 2 * n + c] /= piv;
          }
          for (unsigned i = 0; i != n; ++i) {
            if (i == col) {
              continue;
            }
            const double f = m[i * 2 * n + col];
            for (unsigned c = 0; c != 2 * n; ++c) {
              m[i * 2 * n + c] -= f * m[col * 2 * n + c];
            }
          }
        }
        const bool ok  = engine.invert(a.data(), n, 1);
        double     err = 0.0;
        for (unsigned i = 0; i != n; ++i) {
          for (unsigned j = 0; j != n; ++j) {
            err = std::max(err, std::abs(static_cast<double>(a[i * n + j]) - m[i * 2 * n + n + j]));
          }
        }
        std::printf("Test 10 (inversion by size): n=%2u ok=%d err=%.3e %s\n",
                    n,
                    ok ? 1 : 0,
                    err,
                    (!ok || !(err < 1e-1)) ? "FAIL" : "");
        if (!ok || !(err < 1e-1)) {
          std::printf("Test 10 FAIL: inversion of size %u is wrong (err %.3e)\n", n, err);
          n_bad++;
        }
      }
    }
    if (n_bad != 0) {
      std::printf("Test 10 FAIL: %u sizes inverted incorrectly\n", n_bad);
      return -1;
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 1: Gauss-Jordan inversion golden.
  // -----------------------------------------------------------------------------------
  {
    unsigned n_ok = 0;
    for (unsigned trial = 0; trial != 20; ++trial) {
      const unsigned              n = (trial % 2 == 0) ? 12U : 36U;
      std::uniform_real_distribution<float> uni(-1.0F, 1.0F);
      // Random SPD matrix A = M M^T + n I.
      std::vector<double> a_ref(2 * n * n, 0.0);
      std::vector<float>  a(2 * n * n, 0.0F);
      for (unsigned r = 0; r != n; ++r) {
        for (unsigned c = 0; c != n; ++c) {
          double acc = 0.0;
          for (unsigned k = 0; k != n; ++k) {
            const float mr = uni(rng);
            const float mc = uni(rng);
            acc += static_cast<double>(mr) * static_cast<double>(mc);
          }
          acc += (r == c) ? static_cast<double>(n) : 0.0;
          a_ref[r * 2 * n + c]     = acc;
          a[r * 2 * n + c]         = static_cast<float>(acc);
          a_ref[r * 2 * n + n + r] = 1.0;
          a[r * 2 * n + n + r]     = 1.0F;
        }
      }
      const bool ok = port_channel_estimator_metal_mmse_impl::gauss_jordan_invert(span<float>(a.data(), 2 * n * n), n);
      if (!ok) {
        std::printf("Test 1: inversion failed for trial %u\n", trial);
        return -1;
      }
      // Double-precision reference.
      for (unsigned col = 0; col != n; ++col) {
        unsigned pivot = col;
        double   pv    = std::abs(a_ref[col * 2 * n + col]);
        for (unsigned r = col + 1; r != n; ++r) {
          if (std::abs(a_ref[r * 2 * n + col]) > pv) {
            pv    = std::abs(a_ref[r * 2 * n + col]);
            pivot = r;
          }
        }
        for (unsigned c = 0; c != 2 * n; ++c) {
          std::swap(a_ref[col * 2 * n + c], a_ref[pivot * 2 * n + c]);
        }
        const double inv = 1.0 / a_ref[col * 2 * n + col];
        for (unsigned c = 0; c != 2 * n; ++c) {
          a_ref[col * 2 * n + c] *= inv;
        }
        for (unsigned r = 0; r != n; ++r) {
          if (r == col) {
            continue;
          }
          const double f = a_ref[r * 2 * n + col];
          for (unsigned c = 0; c != 2 * n; ++c) {
            a_ref[r * 2 * n + c] -= f * a_ref[col * 2 * n + c];
          }
        }
      }
      double max_err = 0.0;
      for (unsigned r = 0; r != n; ++r) {
        for (unsigned c = 0; c != n; ++c) {
          max_err = std::max(max_err, std::abs(static_cast<double>(a[r * 2 * n + n + c]) - a_ref[r * 2 * n + n + c]));
        }
      }
      if (max_err > 1e-4) {
        std::printf("Test 1: inversion error %.3e (trial %u)\n", max_err, trial);
        return -1;
      }
      ++n_ok;
    }
    std::printf("Test 1 PASS: Gauss-Jordan golden (%u trials)\n", n_ok);
  }

  // -----------------------------------------------------------------------------------
  // Test 2: fixed channel-statistics provider.
  // -----------------------------------------------------------------------------------
  {
    channel_statistics_estimator_fixed fixed(370e-9F, 7.0F);
    channel_statistics_input           in{};
    in.sigma2 = 0.25F;
    const channel_statistics st = fixed.estimate(in);
    if (std::abs(st.tau_rms_s - 370e-9F) > 1e-12F || std::abs(st.fd_hz - 7.0F) > 1e-12F ||
        std::abs(st.sigma2 - 0.25F) > 1e-12F) {
      std::printf("Test 2 FAIL: unexpected fixed statistics\n");
      return -1;
    }
    std::printf("Test 2 PASS: fixed statistics provider\n");
  }

  // -----------------------------------------------------------------------------------
  // Test 3: end-to-end NMSE comparison vs the classical estimator.
  // -----------------------------------------------------------------------------------
  {
    const std::array<double, 4> snrs_db = {-5.0, 0.0, 10.0, 20.0};
    const unsigned              n_real  = 200;

    auto cfg    = make_config();
    auto pilots = make_pilots();

    auto cpu = std::make_unique<port_channel_estimator_average_impl>(
        create_interpolator(),
        make_ta_estimator(),
        port_channel_estimator_fd_smoothing_strategy::filter,
        port_channel_estimator_td_interpolation_strategy::average,
        true);
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);

    for (double snr_db : snrs_db) {
      const double sigma2 = std::pow(10.0, -snr_db / 10.0);
      std::normal_distribution<float> gauss(0.0F, static_cast<float>(std::sqrt(sigma2 / 2.0)));

      double nmse_cpu  = 0.0;
      double nmse_mmse = 0.0;
      double pwr       = 0.0;

      for (unsigned r = 0; r != n_real; ++r) {
        veha_channel ch(rng);

        // Truth grid (612 x 14).
        std::vector<std::vector<cf_t>> h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(612));
        grid_fake grid(612);
        std::vector<cf_t> rx_sym(612, {0.0F, 0.0F});
        for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
          for (unsigned k = 0; k != 612; ++k) {
            h_true[l][k] = ch(k);
          }
        }
        // Fill the pilot REs of symbols 2 and 11.
        for (unsigned s = 0; s != 2; ++s) {
          const unsigned l = (s == 0) ? 2U : 11U;
          std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
          unsigned j = 0;
          for (unsigned prb = 0; prb != 51; ++prb) {
            for (unsigned pos = 0; pos != 12; pos += 2) {
              const unsigned k   = prb * 12 + pos;
              const cf_t     x   = pilots.get_symbol(s, 0)[j];
              const cf_t     n   = {gauss(rng), gauss(rng)};
              rx_sym[k]         = h_true[l][k] * x + n;
              ++j;
            }
          }
          grid.set_symbol(l, rx_sym);
        }

        auto run_estimator = [&](port_channel_estimator& est) {
          const auto& res = est.compute(grid, 0, pilots, cfg);
          double      err = 0.0;
          double      sig = 0.0;
          for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
            std::vector<cbf16_t> est_sym(612);
            res.get_symbol_ch_estimate(est_sym, l, 0);
            for (unsigned k = 0; k != 612; ++k) {
              const cf_t e = to_cf(est_sym[k]) - h_true[l][k];
              err += std::real(e) * std::real(e) + std::imag(e) * std::imag(e);
              sig += std::real(h_true[l][k]) * std::real(h_true[l][k]) + std::imag(h_true[l][k]) * std::imag(h_true[l][k]);
            }
          }
          return std::pair<double, double>{err, sig};
        };

        auto [err_cpu, sig_cpu]  = run_estimator(*cpu);
        auto [err_mmse, sig_mmse] = run_estimator(*mmse);
        nmse_cpu += err_cpu;
        nmse_mmse += err_mmse;
        pwr += sig_cpu;
      }

      const double nmse_cpu_db  = 10.0 * std::log10(nmse_cpu / pwr);
      const double nmse_mmse_db = 10.0 * std::log10(nmse_mmse / pwr);
      std::printf("SNR %+5.1f dB: cpu %7.2f dB, metal_mmse %7.2f dB (delta %+6.2f dB)\n",
                  snr_db, nmse_cpu_db, nmse_mmse_db, nmse_mmse_db - nmse_cpu_db);
      // The gate is 1.5 dB: the model statistics (tau_rms, f_d) are fixed constants, so at high
      // SNR the MMSE trades a fraction of a dB of NMSE for weights that do not depend on the
      // radio gain (Test 9).  Unit-power pilots are the only case where an absolute sigma2
      // happens to give the same loading.
      if (nmse_mmse_db > nmse_cpu_db + 1.5) {
        std::printf("Test 3 FAIL: metal_mmse regresses by more than 1.5 dB at SNR %.1f dB\n", snr_db);
        return -1;
      }
    }
    std::printf("Test 3 PASS: end-to-end NMSE vs the classical estimator\n");
  }

  // -----------------------------------------------------------------------------------
  // Test 4: Metal engine golden (K1 batched inversion + K2 batched block matmul).
  // -----------------------------------------------------------------------------------
  {
    metal::mmse_engine engine;
    if (!engine.init()) {
      std::printf("Test 4 SKIPPED: Metal device unavailable\n");
    } else {
      const unsigned n          = 36;
      // The estimator inverts one system per layer and hop: a single system is the production
      // case, four systems show how the kernel scales when several layers are configured.
      const unsigned nof_sys    = []() {
        const char* env = std::getenv("OCUDU_INV_SYSTEMS");
        return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 4U;
      }();
      const unsigned nout       = 504;
      const unsigned L          = 36;
      const unsigned nof_blocks = 17;

      // Well-conditioned MMSE-like A = Toeplitz(decaying r) + sigma2 I.
      std::vector<float> a(static_cast<std::size_t>(nof_sys) * n * n);
      for (unsigned s = 0; s != nof_sys; ++s) {
        for (unsigned r = 0; r != n; ++r) {
          for (unsigned c = 0; c != n; ++c) {
            const float d  = static_cast<float>(std::abs(static_cast<int>(r) - static_cast<int>(c)));
            const float rv = std::exp(-d * 0.12F);
            a[(static_cast<std::size_t>(s) * n + r) * n + c] = rv + ((r == c) ? 0.5F : 0.0F);
          }
        }
      }
      std::vector<float> a_ref = a;
      // Wall time of the call against the GPU execution it reports: the difference is the host-side
      // cost of issuing the operation (buffer wrapping, encoding, submission), which dominates the
      // estimator in the OTA statistics ([mmse_time_sum] gpu_path against gpu_wait).
      const auto t_inv0 = std::chrono::steady_clock::now();
      const bool inv_ok = engine.invert(a.data(), n, nof_sys);
      const auto t_inv1 = std::chrono::steady_clock::now();
      const double inv_wall_us = std::chrono::duration<double, std::micro>(t_inv1 - t_inv0).count();
      if (!inv_ok) {
        std::printf("Test 4 FAIL: invert() refused the system\n");
        return -1;
      }
      // CPU double reference.
      double max_err = 0.0;
      for (unsigned s = 0; s != nof_sys; ++s) {
        // rebuild A in double and invert
        std::vector<double> m(2 * n * n, 0.0);
        for (unsigned r = 0; r != n; ++r) {
          for (unsigned c = 0; c != n; ++c) {
            m[r * 2 * n + c] = a_ref[(static_cast<std::size_t>(s) * n + r) * n + c];
          }
          m[r * 2 * n + n + r] = 1.0;
        }
        for (unsigned col = 0; col != n; ++col) {
          unsigned pivot = col;
          double   pv    = std::abs(m[col * 2 * n + col]);
          for (unsigned r = col + 1; r != n; ++r) {
            if (std::abs(m[r * 2 * n + col]) > pv) {
              pv    = std::abs(m[r * 2 * n + col]);
              pivot = r;
            }
          }
          for (unsigned c = 0; c != 2 * n; ++c) {
            std::swap(m[col * 2 * n + c], m[pivot * 2 * n + c]);
          }
          const double inv = 1.0 / m[col * 2 * n + col];
          for (unsigned c = 0; c != 2 * n; ++c) {
            m[col * 2 * n + c] *= inv;
          }
          for (unsigned r = 0; r != n; ++r) {
            if (r == col) {
              continue;
            }
            const double f = m[r * 2 * n + col];
            for (unsigned c = 0; c != 2 * n; ++c) {
              m[r * 2 * n + c] -= f * m[col * 2 * n + c];
            }
          }
        }
        for (unsigned r = 0; r != n; ++r) {
          for (unsigned c = 0; c != n; ++c) {
            max_err = std::max(max_err,
                               std::abs(static_cast<double>(a[(static_cast<std::size_t>(s) * n + r) * n + c]) -
                                        m[r * 2 * n + n + c]));
          }
        }
      }
      if (max_err > 1e-3) {
        std::printf("Test 4 FAIL: GPU inversion error %.3e\n", max_err);
        return -1;
      }
      std::printf("Test 4a PASS: GPU batched inversion (max err %.2e, GPU %.1f us, host %.1f us of the "
                  "%.1f us call)\n",
                  max_err,
                  engine.last_gpu_wait_us(),
                  inv_wall_us - engine.last_gpu_wait_us(),
                  inv_wall_us);

      // K2 golden.
      std::uniform_real_distribution<float> uni(-1.0F, 1.0F);
      std::vector<float> w(static_cast<std::size_t>(nof_sys) * nout * L);
      std::vector<float> y(static_cast<std::size_t>(nof_sys) * nof_blocks * 2 * L);
      std::vector<float> h(static_cast<std::size_t>(nof_sys) * nof_blocks * 2 * nout);
      for (auto& v : w) {
        v = uni(rng);
      }
      for (auto& v : y) {
        v = uni(rng);
      }
      std::vector<float> h_ref(h.size());
      for (unsigned s = 0; s != nof_sys; ++s) {
        for (unsigned b = 0; b != nof_blocks; ++b) {
          for (unsigned o = 0; o != nout; ++o) {
            float re = 0.0F;
            float im = 0.0F;
            for (unsigned j = 0; j != L; ++j) {
              const float wj = w[(static_cast<std::size_t>(s) * nout + o) * L + j];
              re += wj * y[(static_cast<std::size_t>(s) * nof_blocks + b) * 2 * L + 2 * j];
              im += wj * y[(static_cast<std::size_t>(s) * nof_blocks + b) * 2 * L + 2 * j + 1];
            }
            h_ref[(static_cast<std::size_t>(s) * nof_blocks + b) * 2 * nout + 2 * o]     = re;
            h_ref[(static_cast<std::size_t>(s) * nof_blocks + b) * 2 * nout + 2 * o + 1] = im;
          }
        }
      }
      engine.apply(w.data(), y.data(), h.data(), nout, L, nof_sys, nof_blocks);
      double max_err2 = 0.0;
      for (std::size_t i = 0; i != h.size(); ++i) {
        max_err2 = std::max(max_err2, std::abs(static_cast<double>(h[i]) - h_ref[i]));
      }
      if (max_err2 > 1e-2) {
        std::printf("Test 4 FAIL: GPU apply error %.3e\n", max_err2);
        return -1;
      }
      std::printf("Test 4b PASS: GPU batched block matmul (max err %.2e, gpu %.1f us)\n",
                  max_err2, engine.last_gpu_wait_us());
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 4c: the asynchronous submission (commit now, wait later) stays bounded and correct.
  // The point of run_async() is to overlap the GPU with the host's preparation of the consumer, so
  // the wait is deliberately deferred - which is also exactly the shape that stalled in the field
  // once (many committed-but-unwaited command buffers exhausting the queue's slots). This hammers
  // it for far longer than that stall took to appear, checks that at most one submission is ever
  // outstanding, and compares the result with the synchronous path.
  // -----------------------------------------------------------------------------------
  {
    metal::mmse_engine engine;
    if (engine.init()) {
      const unsigned nof_sys = 2, nout = 504, L = 36, nof_blocks = 17;
      std::uniform_real_distribution<float> uni(-1.0F, 1.0F);
      std::vector<float> a(static_cast<std::size_t>(nof_sys) * L * L);
      std::vector<float> rp(static_cast<std::size_t>(nof_sys) * nout * L);
      std::vector<float> w(static_cast<std::size_t>(nof_sys) * nout * L);
      std::vector<float> y(static_cast<std::size_t>(nof_sys) * nof_blocks * 2 * L);
      std::vector<float> h(static_cast<std::size_t>(nof_sys) * nof_blocks * 2 * nout);
      std::vector<float> h_ref(h.size());
      for (auto& v : a) {
        v = uni(rng);
      }
      for (auto& v : rp) {
        v = uni(rng);
      }
      for (auto& v : y) {
        v = uni(rng);
      }
      // A is A^T A + ridge: symmetric positive definite, as the real one is.
      for (unsigned s = 0; s != nof_sys; ++s) {
        float* am = a.data() + static_cast<std::size_t>(s) * L * L;
        for (unsigned r = 0; r != L; ++r) {
          for (unsigned c = r + 1; c != L; ++c) {
            am[c * L + r] = am[r * L + c];
          }
          am[r * L + r] += 10.0F;
        }
      }
      std::vector<float> a_orig(a);
      if (!engine.run(a.data(), rp.data(), w.data(), y.data(), h_ref.data(), nout, L, nof_sys, nof_blocks)) {
        std::printf("Test 4c FAIL: the synchronous reference call failed\n");
        return -1;
      }

      // 3000 deferred submissions. The synchronous call above waited, so the first async call has
      // nothing outstanding; every later one must wait for its predecessor before it can overwrite
      // the staging buffers, which is what keeps the queue bounded.
      const unsigned nof_reps       = 3000;
      unsigned       nof_outstanding = 0;
      for (unsigned rep = 0; rep != nof_reps; ++rep) {
        a = a_orig;
        if (!engine.run_async(a.data(), rp.data(), w.data(), y.data(), h.data(), nout, L, nof_sys, nof_blocks)) {
          std::printf("Test 4c FAIL: run_async() failed at repetition %u\n", rep);
          return -1;
        }
        nof_outstanding = std::max(nof_outstanding, engine.has_pending() ? 1U : 0U);
        if (!engine.wait_pending()) {
          std::printf("Test 4c FAIL: wait_pending() failed at repetition %u\n", rep);
          return -1;
        }
      }
      if (engine.has_pending()) {
        std::printf("Test 4c FAIL: a submission is still pending at the end\n");
        return -1;
      }
      double max_err = 0.0;
      for (std::size_t i = 0; i != h.size(); ++i) {
        max_err = std::max(max_err, std::abs(static_cast<double>(h[i]) - h_ref[i]));
      }
      if (max_err > 1e-3) {
        std::printf("Test 4c FAIL: the asynchronous path deviates by %.3e\n", max_err);
        return -1;
      }
      std::printf("Test 4c PASS: %u deferred submissions, at most %u in flight, result matches the "
                  "synchronous path (max err %.2e)\n",
                  nof_reps,
                  nof_outstanding,
                  max_err);
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 5: steady-state latency of compute() (cpu vs metal_mmse), single thread.
  // -----------------------------------------------------------------------------------
  {
    const unsigned n_slots = 200;
    auto           cfg     = make_config();
    auto           pilots  = make_pilots();
    std::normal_distribution<float> gauss(0.0F, 1.0F);
    grid_fake grid(612);
    std::vector<cf_t> rx_sym(612, {0.0F, 0.0F});

    auto cpu = std::make_unique<port_channel_estimator_average_impl>(
        create_interpolator(),
        make_ta_estimator(),
        port_channel_estimator_fd_smoothing_strategy::filter,
        port_channel_estimator_td_interpolation_strategy::average,
        true);
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);

    auto time_estimator = [&](port_channel_estimator& est) {
      for (unsigned r = 0; r != 5; ++r) {
        est.compute(grid, 0, pilots, cfg); // warm-up (pipeline creation etc.)
      }
      const auto t0 = std::chrono::steady_clock::now();
      for (unsigned r = 0; r != n_slots; ++r) {
        est.compute(grid, 0, pilots, cfg);
      }
      const auto t1 = std::chrono::steady_clock::now();
      return std::chrono::duration<double, std::micro>(t1 - t0).count() / n_slots;
    };

    double us_cpu  = time_estimator(*cpu);
    double us_mmse = time_estimator(*mmse);

    // Component breakdown: standalone engine run() steady-state timing.
    {
      metal::mmse_engine eng;
      if (eng.init()) {
        const unsigned          L = 36, nout = 504, sys = 1, blocks = 17;
        static std::vector<float> a(sys * L * L), rp(sys * nout * L), w(sys * nout * L);
        static std::vector<float> y(sys * blocks * 2 * L), h(sys * blocks * 2 * nout);
        std::mt19937               rr(7);
        std::uniform_real_distribution<float> uni(-1.0F, 1.0F);
        for (auto& v : a) v = uni(rr);
        for (auto& v : rp) v = uni(rr);
        for (auto& v : y) v = uni(rr);
        for (unsigned k = 0; k != 20; ++k) {
          eng.run(a.data(), rp.data(), w.data(), y.data(), h.data(), nout, L, sys, blocks);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (unsigned k = 0; k != 500; ++k) {
          eng.run(a.data(), rp.data(), w.data(), y.data(), h.data(), nout, L, sys, blocks);
        }
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("        engine.run() steady-state: %.1f us/call (gpu %.1f us)\n",
                    std::chrono::duration<double, std::micro>(t1 - t0).count() / 500, eng.last_gpu_wait_us());
        // K1 alone
        for (unsigned k = 0; k != 20; ++k) {
          eng.invert(a.data(), L, sys);
        }
        const auto t2 = std::chrono::steady_clock::now();
        for (unsigned k = 0; k != 500; ++k) {
          eng.invert(a.data(), L, sys);
        }
        const auto t3 = std::chrono::steady_clock::now();
        std::printf("        engine.invert() steady-state: %.1f us/call (gpu %.1f us)\n",
                    std::chrono::duration<double, std::micro>(t3 - t2).count() / 500, eng.last_gpu_wait_us());
        // K2 alone
        for (unsigned k = 0; k != 20; ++k) {
          eng.apply(w.data(), y.data(), h.data(), nout, L, sys, blocks);
        }
        const auto t4 = std::chrono::steady_clock::now();
        for (unsigned k = 0; k != 500; ++k) {
          eng.apply(w.data(), y.data(), h.data(), nout, L, sys, blocks);
        }
        const auto t5 = std::chrono::steady_clock::now();
        std::printf("        engine.apply() steady-state: %.1f us/call (gpu %.1f us)\n",
                    std::chrono::duration<double, std::micro>(t5 - t4).count() / 500, eng.last_gpu_wait_us());
        // L = 54 (3 DMRS symbols x 3 PRB, the E2E Msg3 case) weights+apply check.
        {
          const unsigned          L54 = 54, nout54 = 504, sys54 = 1, blocks54 = 1;
          std::vector<float> a54(sys54 * L54 * L54), rp54(sys54 * nout54 * L54), w54(sys54 * nout54 * L54);
          std::vector<float> y54(sys54 * blocks54 * 2 * L54), h54(sys54 * blocks54 * 2 * nout54);
          std::mt19937                 rrr(11);
          std::uniform_real_distribution<float> uu(-1.0F, 1.0F);
          for (auto& v : a54) v = uu(rrr);
          for (auto& v : rp54) v = uu(rrr);
          for (auto& v : y54) v = uu(rrr);
          const bool ok54 = eng.run_weights_only(a54.data(), rp54.data(), w54.data(), y54.data(), h54.data(),
                                                 nout54, L54, sys54, blocks54);
          // CPU reference: W = R_hp . A^-1 needs A^-1; here just check W vs a direct product with A^-1 computed on CPU.
          // Build A^-1 via double GJ.
          std::vector<double> m(2 * L54 * L54, 0.0);
          for (unsigned r = 0; r != L54; ++r) {
            for (unsigned c = 0; c != L54; ++c) {
              m[r * 2 * L54 + c] = a54[r * L54 + c];
            }
            m[r * 2 * L54 + L54 + r] = 1.0;
          }
          for (unsigned col = 0; col != L54; ++col) {
            unsigned pivot = col;
            double   pv    = std::abs(m[col * 2 * L54 + col]);
            for (unsigned r = col + 1; r != L54; ++r) {
              if (std::abs(m[r * 2 * L54 + col]) > pv) { pv = std::abs(m[r * 2 * L54 + col]); pivot = r; }
            }
            for (unsigned c = 0; c != 2 * L54; ++c) std::swap(m[col * 2 * L54 + c], m[pivot * 2 * L54 + c]);
            const double inv = 1.0 / m[col * 2 * L54 + col];
            for (unsigned c = 0; c != 2 * L54; ++c) m[col * 2 * L54 + c] *= inv;
            for (unsigned r = 0; r != L54; ++r) {
              if (r == col) continue;
              const double f = m[r * 2 * L54 + col];
              for (unsigned c = 0; c != 2 * L54; ++c) m[r * 2 * L54 + c] -= f * m[col * 2 * L54 + c];
            }
          }
          double maxerr = 0.0;
          for (unsigned o = 0; o != nout54; ++o) {
            for (unsigned col = 0; col != L54; ++col) {
              double acc = 0.0;
              for (unsigned k = 0; k != L54; ++k) acc += rp54[o * L54 + k] * m[k * 2 * L54 + L54 + col];
              maxerr = std::max(maxerr, std::abs(static_cast<double>(w54[o * L54 + col]) - acc));
            }
          }
          std::fprintf(stderr, "[dbgL54] run_weights_only ok=%d W maxerr=%.3e\n", ok54, maxerr);
        }
      }
    }
    std::printf("Test 5: compute() latency: cpu %.1f us, metal_mmse %.1f us (x%.2f)\n",
                us_cpu, us_mmse, us_mmse / std::max(us_cpu, 1e-9));
    if (mmse->last_gpu_wait_us() >= 0.0) {
      std::printf("        metal_mmse engine GPU time (last call): %.1f us\n", mmse->last_gpu_wait_us());
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 6: real-chain parameter combinations (10 MHz = 52 PRB with the edge block,
  // Msg3-style single DM-RS symbol, 25 PRB) - crash regression for the E2E config.
  // -----------------------------------------------------------------------------------
  {
    const std::array<std::pair<unsigned, unsigned>, 3> combos = {{{52, 2}, {52, 1}, {25, 2}}};
    for (const auto& [n_prb, n_sym] : combos) {
      auto cfg    = make_config(n_prb, n_sym == 2);
      auto pilots = make_pilots(n_prb, n_sym);
      veha_channel ch(rng);
      std::vector<std::vector<cf_t>> h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(n_prb * 12));
      grid_fake grid(n_prb * 12);
      std::vector<cf_t> rx_sym(n_prb * 12, {0.0F, 0.0F});
      std::normal_distribution<float> gauss(0.0F, 0.4F);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          h_true[l][k] = ch(k);
        }
      }
      for (unsigned s = 0; s != n_sym; ++s) {
        const unsigned l = (s == 0) ? 2U : 11U;
        std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
        unsigned j = 0;
        for (unsigned prb = 0; prb != n_prb; ++prb) {
          for (unsigned pos = 0; pos != 12; pos += 2) {
            const unsigned k = prb * 12 + pos;
            const cf_t     x = pilots.get_symbol(s, 0)[j];
            rx_sym[k]        = h_true[l][k] * x + cf_t{gauss(rng), gauss(rng)};
            ++j;
          }
        }
        grid.set_symbol(l, rx_sym);
      }
      auto cpu = std::make_unique<port_channel_estimator_average_impl>(
          create_interpolator(),
          make_ta_estimator(),
          port_channel_estimator_fd_smoothing_strategy::filter,
          port_channel_estimator_td_interpolation_strategy::average,
          true);
      auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
          create_interpolator(),
          make_ta_estimator(),
          std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
          3,
          true);
      double err = 0.0, sig = 0.0;
      // Per-shape steady-state latency of the production hop (the aggregate [mmse_time_sum] mixes
      // every shape of the sweep, so the shapes with an edge/tail block need their own number).
      const auto t_lat0 = std::chrono::steady_clock::now();
      for (unsigned rep = 0; rep != 50; ++rep) {
        const auto& res = mmse->compute(grid, 0, pilots, cfg);
        for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
          std::vector<cbf16_t> est(n_prb * 12);
          res.get_symbol_ch_estimate(est, l, 0);
          for (unsigned k = 0; k != n_prb * 12; ++k) {
            const cf_t e = to_cf(est[k]) - h_true[l][k];
            err += std::real(e) * std::real(e) + std::imag(e) * std::imag(e);
            sig += std::real(h_true[l][k]) * std::real(h_true[l][k]) + std::imag(h_true[l][k]) * std::imag(h_true[l][k]);
          }
        }
        (void)cpu;
      }
      const auto t_lat1  = std::chrono::steady_clock::now();
      const double lat_us =
          std::chrono::duration<double, std::micro>(t_lat1 - t_lat0).count() / 50.0;
      std::printf("Test 6 (%u PRB, %u DMRS): metal_mmse NMSE %.2f dB, %.1f us/hop (no crash)\n",
                  n_prb,
                  n_sym,
                  10.0 * std::log10(err / sig),
                  lat_us);
      if (n_sym == 1) {
        const auto& res = mmse->compute(grid, 0, pilots, cfg);
        std::vector<cbf16_t> est(n_prb * 12);
        res.get_symbol_ch_estimate(est, 2, 0);
        std::fprintf(stderr, "[dbg1sym] est[0]=(%f,%f) truth[0]=(%f,%f) est[10]=(%f,%f) truth[10]=(%f,%f)\n",
                     to_cf(est[0]).real(), to_cf(est[0]).imag(), h_true[2][0].real(), h_true[2][0].imag(),
                     to_cf(est[10]).real(), to_cf(est[10]).imag(), h_true[2][10].real(), h_true[2][10].imag());
      }
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 6b: EXACT E2E Msg3 configuration (4 PRB at CRB offset 8, 3 DM-RS symbols {2,7,11})
  // - the crash reproducer from the first E2E run (ul_dmrs_symb_pos=2180).
  // -----------------------------------------------------------------------------------
  {
    const unsigned n_prb = 4, crb_offset = 8;
    // The grid must span the whole BWP from CRB 0 (the extractor indexes it by the absolute CRB
    // offset), i.e. (crb_offset + n_prb) * 12 subcarriers - not just n_prb * 12.
    const unsigned nof_subc = (crb_offset + n_prb) * NOF_SUBCARRIERS_PER_RB;
    auto cfg    = make_config(n_prb, true, crb_offset, /*three_dmrs_symbols=*/true);
    auto pilots = make_pilots(n_prb, 3);
    veha_channel ch(rng);
    std::vector<std::vector<cf_t>> h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(n_prb * 12));
    grid_fake grid(nof_subc);
    std::vector<cf_t> rx_sym(nof_subc, {0.0F, 0.0F});
    std::normal_distribution<float> gauss(0.0F, 0.4F);
    for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
      for (unsigned k = 0; k != n_prb * 12; ++k) {
        h_true[l][k] = ch(crb_offset * 12 + k);
      }
    }
    const std::array<unsigned, 3> dmrs_l = {2, 7, 11};
    for (unsigned s = 0; s != 3; ++s) {
      const unsigned l = dmrs_l[s];
      std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
      unsigned j = 0;
      for (unsigned prb = 0; prb != n_prb; ++prb) {
        for (unsigned pos = 0; pos != 12; pos += 2) {
          const unsigned k = crb_offset * 12 + prb * 12 + pos;
          rx_sym[k]        = h_true[l][prb * 12 + pos] * pilots.get_symbol(s, 0)[j] + cf_t{gauss(rng), gauss(rng)};
          ++j;
        }
      }
      grid.set_symbol(l, rx_sym);
    }
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);
    double err = 0.0, sig = 0.0;
    for (unsigned rep = 0; rep != 50; ++rep) {
      const auto& res = mmse->compute(grid, 0, pilots, cfg);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        std::vector<cbf16_t> est(n_prb * 12);
        res.get_symbol_ch_estimate(est, l, 0);
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          const cf_t e = to_cf(est[k]) - h_true[l][k];
          err += std::real(e) * std::real(e) + std::imag(e) * std::imag(e);
          sig += std::real(h_true[l][k]) * std::real(h_true[l][k]) + std::imag(h_true[l][k]) * std::imag(h_true[l][k]);
        }
      }
    }
    std::printf("Test 6b (Msg3: 4 PRB @ CRB 8, 3 DMRS): metal_mmse NMSE %.2f dB (no crash)\n",
                10.0 * std::log10(err / sig));
    {
      const auto& res = mmse->compute(grid, 0, pilots, cfg);
      std::vector<cbf16_t> est(n_prb * 12);
      res.get_symbol_ch_estimate(est, 2, 0);
      std::fprintf(stderr, "[dbg6b] est[0]=(%.3f,%.3f) truth=(%.3f,%.3f) est[20]=(%.3f,%.3f) truth=(%.3f,%.3f)\n",
                   to_cf(est[0]).real(), to_cf(est[0]).imag(), h_true[2][0].real(), h_true[2][0].imag(),
                   to_cf(est[20]).real(), to_cf(est[20]).imag(), h_true[2][20].real(), h_true[2][20].imag());
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 7: stress - alternating Msg3-style / data-style allocations on the same instance
  // (mimics the real attach traffic: small grant, then full-BW data, retransmissions).
  // -----------------------------------------------------------------------------------
  {
    // Includes 1-2 PRB whole hops: narrower than block_prb (3 PRB), they must still run on
    // the engine (single tail block) instead of the CPU reference path.
    const std::array<unsigned, 8> prb_seq    = {6, 52, 4, 52, 12, 25, 1, 2};
    const std::array<unsigned, 8> sym_seq    = {1, 2, 1, 2, 1, 2, 2, 1};
    std::mt19937                  rr(99);
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);
    std::normal_distribution<float> gauss(0.0F, 0.5F);
    for (unsigned rep = 0; rep != 200; ++rep) {
      const unsigned n_prb = prb_seq[rep % prb_seq.size()];
      const unsigned n_sym = sym_seq[rep % sym_seq.size()];
      auto           cfg   = make_config(n_prb, n_sym == 2);
      auto           pilots = make_pilots(n_prb, n_sym);
      grid_fake grid(n_prb * 12);
      std::vector<cf_t> rx_sym(n_prb * 12, {0.0F, 0.0F});
      for (unsigned s = 0; s != n_sym; ++s) {
        const unsigned l = (s == 0) ? 2U : 11U;
        std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
        unsigned j = 0;
        for (unsigned prb = 0; prb != n_prb; ++prb) {
          for (unsigned pos = 0; pos != 12; pos += 2) {
            rx_sym[prb * 12 + pos] = pilots.get_symbol(s, 0)[j] + cf_t{gauss(rr), gauss(rr)};
            ++j;
          }
        }
        grid.set_symbol(l, rx_sym);
      }
      const auto& res = mmse->compute(grid, 0, pilots, cfg);
      std::vector<cbf16_t> est(n_prb * 12);
      res.get_symbol_ch_estimate(est, 2, 0);
      if (std::isnan(to_cf(est[0]).real())) {
        std::printf("Test 7 FAIL: NaN at rep %u (%u PRB, %u sym)\n", rep, n_prb, n_sym);
        return -1;
      }
    }
    std::printf("Test 7 PASS: 200-slot alternating-config stress (no crash, no NaN)\n");
  }

  // -----------------------------------------------------------------------------------
  // Test 8: metal_nn_mmse A/B parity vs metal_mmse (identical host math, different GPU
  // kernels). 51 PRB, block 3 PRB, 4 DM-RS symbols -> nout_std = 504, L_std = 72: the
  // simdgroup 8x8 path must be engaged (matrix_accel_ready) and reproduce metal_mmse up
  // to the cbf16 output quantization. 3 DM-RS symbols -> L_std = 54 (NOT 8-aligned): the
  // nn flavor must degrade to the legacy kernels without crashing and keep a sane NMSE.
  // -----------------------------------------------------------------------------------
  {
    const unsigned n_prb = 51; // 17 standard blocks of 3 PRB: 4 full quads + 1-block tail quad
    auto cfg4 = make_config(n_prb, /*two_dmrs_symbols=*/true, 0, /*three=*/false, /*four=*/true);
    auto pil4 = make_pilots(n_prb, 4);
    auto cfg3 = make_config(n_prb, /*two_dmrs_symbols=*/true, 0, /*three=*/true);
    auto pil3 = make_pilots(n_prb, 3);

    auto make_est = [](bool use_nn) {
      return std::make_unique<port_channel_estimator_metal_mmse_impl>(
          create_interpolator(),
          make_ta_estimator(),
          std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
          3,
          true,
          use_nn);
    };
    auto legacy = make_est(false);
    auto nn     = make_est(true);
    if (!nn->matrix_accel_ready()) {
      std::printf("Test 8 FAIL: simdgroup 8x8 pipelines not ready (stale ocudu_mmse.metallib?)\n");
      return -1;
    }

    const double                            sigma2 = 0.1; // 10 dB SNR
    std::normal_distribution<float>         gauss(0.0F, static_cast<float>(std::sqrt(sigma2 / 2.0)));
    const std::vector<unsigned>             syms4 = {2, 7, 11, 12};
    const std::vector<unsigned>             syms3 = {2, 7, 11};
    std::vector<std::vector<cf_t>>          a(MAX_NSYMB_PER_SLOT), b(MAX_NSYMB_PER_SLOT);
    std::vector<std::vector<cf_t>>          h_true(MAX_NSYMB_PER_SLOT);
    std::vector<cf_t>                       rx_sym(n_prb * 12, {0.0F, 0.0F});
    grid_fake                               grid(n_prb * 12);

    auto build_scene = [&](const std::vector<unsigned>& syms, dmrs_symbol_list& pilots) {
      veha_channel ch(rng);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        h_true[l].resize(n_prb * 12);
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          h_true[l][k] = ch(k);
        }
      }
      unsigned s = 0;
      for (unsigned l : syms) {
        std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
        unsigned j = 0;
        for (unsigned prb = 0; prb != n_prb; ++prb) {
          for (unsigned pos = 0; pos != 12; pos += 2) {
            rx_sym[prb * 12 + pos] = h_true[l][prb * 12 + pos] * pilots.get_symbol(s, 0)[j] + cf_t{gauss(rng), gauss(rng)};
            ++j;
          }
        }
        grid.set_symbol(l, rx_sym);
        ++s;
      }
    };

    auto collect = [&](port_channel_estimator& est, dmrs_symbol_list& pilots,
                       port_channel_estimator::configuration& cfg, std::vector<std::vector<cf_t>>& out) {
      const auto& res = est.compute(grid, 0, pilots, cfg);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        std::vector<cbf16_t> sym(n_prb * 12);
        res.get_symbol_ch_estimate(sym, l, 0);
        out[l].resize(n_prb * 12);
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          out[l][k] = to_cf(sym[k]);
        }
      }
    };

    // (a) Parity on the 8-aligned configuration (nn engaged, tail quad exercised).
    double diff = 0.0, pwr = 0.0;
    for (unsigned r = 0; r != 16; ++r) {
      build_scene(syms4, pil4);
      collect(*legacy, pil4, cfg4, a);
      collect(*nn, pil4, cfg4, b);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          const cf_t d = a[l][k] - b[l][k];
          diff += std::real(d) * std::real(d) + std::imag(d) * std::imag(d);
          pwr += std::real(a[l][k]) * std::real(a[l][k]) + std::imag(a[l][k]) * std::imag(a[l][k]);
        }
      }
    }
    const double parity_db = 10.0 * std::log10(diff / pwr);
    std::printf("Test 8: metal_nn_mmse vs metal_mmse parity (4 DMRS, L=72): %.2f dB\n", parity_db);
    if (parity_db > -30.0) {
      std::printf("Test 8 FAIL: nn deviates from metal_mmse beyond the bf16 quantization floor\n");
      return -1;
    }

    // (b) Non-8-aligned dims (3 DM-RS -> L=54): the nn flavor must STILL engage the matrix
    // kernels (zero-padding to Lp=56 inside the staging buffers) and match metal_mmse, and
    // nn_engaged_last() must report the matrix path - nn=0 is never allowed here.
    double diff3 = 0.0, pwr3 = 0.0, err3 = 0.0, sig3 = 0.0;
    bool   engaged = false;
    for (unsigned r = 0; r != 8; ++r) {
      build_scene(syms3, pil3);
      collect(*legacy, pil3, cfg3, a);
      collect(*nn, pil3, cfg3, b);
      engaged = engaged || nn->nn_engaged_last();
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          const cf_t d = a[l][k] - b[l][k];
          diff3 += std::real(d) * std::real(d) + std::imag(d) * std::imag(d);
          pwr3 += std::real(a[l][k]) * std::real(a[l][k]) + std::imag(a[l][k]) * std::imag(a[l][k]);
          const cf_t e = b[l][k] - h_true[l][k];
          err3 += std::real(e) * std::real(e) + std::imag(e) * std::imag(e);
          sig3 += std::real(h_true[l][k]) * std::real(h_true[l][k]) + std::imag(h_true[l][k]) * std::imag(h_true[l][k]);
          if (std::isnan(std::real(b[l][k]))) {
            std::printf("Test 8 FAIL: NaN in the padded L=54 path\n");
            return -1;
          }
        }
      }
    }
    const double parity3_db = 10.0 * std::log10(diff3 / pwr3);
    const double nmse3_db   = 10.0 * std::log10(err3 / sig3);
    std::printf("Test 8: nn parity with L=54 zero-padded to 56 (3 DMRS): %.2f dB | NMSE %.2f dB | nn engaged %d\n",
                parity3_db, nmse3_db, engaged ? 1 : 0);
    if (!engaged || parity3_db > -30.0 || nmse3_db > -3.0) {
      std::printf("Test 8 FAIL: L=54 must run the padded matrix kernels (nn=1) and match metal_mmse\n");
      return -1;
    }

    // (c) Engine-level parity on ODD dims (nout=37, L=13 -> Np=40, Lp=16): exercises the
    // ceil8 tiling, pad-zeroing and the output truncation guards directly on the kernels,
    // against a CPU double reference and the legacy engine.
    {
      const unsigned nout = 37, L = 13, nsys = 2, nblk = 5;
      const unsigned Np = (nout + 7) & ~7u, Lp = (L + 7) & ~7u;
      const unsigned nquads = (nblk + 3) / 4;
      std::uniform_real_distribution<float> uni(-0.5F, 0.5F);
      std::vector<float> r_hp_pad(static_cast<std::size_t>(nsys) * Np * Lp, 0.0F);
      std::vector<float> a_inv_pad(static_cast<std::size_t>(nsys) * Lp * Lp, 0.0F);
      std::vector<float> w_pad(static_cast<std::size_t>(nsys) * Np * Lp, 0.0F);
      std::vector<float> y(static_cast<std::size_t>(nsys) * nblk * 2 * L, 0.0F);
      std::vector<float> qy(static_cast<std::size_t>(nsys) * nquads * Lp * 8, 0.0F);
      std::vector<float> h_nn(static_cast<std::size_t>(nsys) * nblk * 2 * nout, 0.0F);
      std::vector<float> r_hp_c(static_cast<std::size_t>(nsys) * nout * L);
      std::vector<float> a_inv_c(static_cast<std::size_t>(nsys) * L * L);
      std::vector<float> w_c(static_cast<std::size_t>(nsys) * nout * L);
      std::vector<float> h_c(static_cast<std::size_t>(nsys) * nblk * 2 * nout, 0.0F);
      for (unsigned s = 0; s != nsys; ++s) {
        for (unsigned o = 0; o != nout; ++o) {
          for (unsigned k = 0; k != L; ++k) {
            const float v = uni(rng);
            r_hp_pad[(static_cast<std::size_t>(s) * Np + o) * Lp + k] = v;
            r_hp_c[(static_cast<std::size_t>(s) * nout + o) * L + k]  = v;
          }
        }
        for (unsigned r = 0; r != L; ++r) {
          for (unsigned c = 0; c != L; ++c) {
            const float v = uni(rng);
            a_inv_pad[(static_cast<std::size_t>(s) * Lp + r) * Lp + c] = v;
            a_inv_c[(static_cast<std::size_t>(s) * L + r) * L + c]     = v;
          }
        }
      }
      for (unsigned s = 0; s != nsys; ++s) {
        for (unsigned blk = 0; blk != nblk; ++blk) {
          for (unsigned k = 0; k != L; ++k) {
            const float re = uni(rng), im = uni(rng);
            y[(static_cast<std::size_t>(s) * nblk + blk) * 2 * L + 2 * k]     = re;
            y[(static_cast<std::size_t>(s) * nblk + blk) * 2 * L + 2 * k + 1] = im;
            const unsigned quad = blk / 4, bl = blk % 4;
            float* qrow = &qy[((static_cast<std::size_t>(s) * nquads + quad) * Lp + k) * 8 + 2 * bl];
            qrow[0] = re;
            qrow[1] = im;
          }
        }
      }
      // CPU double reference.
      std::vector<double> w_ref(static_cast<std::size_t>(nsys) * nout * L), h_ref(static_cast<std::size_t>(nsys) * nblk * 2 * nout, 0.0);
      for (unsigned s = 0; s != nsys; ++s) {
        for (unsigned o = 0; o != nout; ++o) {
          for (unsigned c = 0; c != L; ++c) {
            double acc = 0.0;
            for (unsigned k = 0; k != L; ++k) {
              acc += r_hp_c[(static_cast<std::size_t>(s) * nout + o) * L + k] * a_inv_c[(static_cast<std::size_t>(s) * L + k) * L + c];
            }
            w_ref[(static_cast<std::size_t>(s) * nout + o) * L + c] = acc;
          }
        }
      }
      for (unsigned s = 0; s != nsys; ++s) {
        for (unsigned blk = 0; blk != nblk; ++blk) {
          for (unsigned o = 0; o != nout; ++o) {
            double re = 0.0, im = 0.0;
            for (unsigned k = 0; k != L; ++k) {
              const double wv = w_ref[(static_cast<std::size_t>(s) * nout + o) * L + k];
              re += wv * y[(static_cast<std::size_t>(s) * nblk + blk) * 2 * L + 2 * k];
              im += wv * y[(static_cast<std::size_t>(s) * nblk + blk) * 2 * L + 2 * k + 1];
            }
            h_ref[(static_cast<std::size_t>(s) * nblk + blk) * 2 * nout + 2 * o]     = re;
            h_ref[(static_cast<std::size_t>(s) * nblk + blk) * 2 * nout + 2 * o + 1] = im;
          }
        }
      }
      metal::mmse_engine eng;
      if (!eng.init() || !eng.init_matrix_pipelines()) {
        std::printf("Test 8c SKIPPED: Metal/matrix pipelines unavailable\n");
      } else {
        const bool ok_nn = eng.run_nn(a_inv_pad.data(), r_hp_pad.data(), w_pad.data(), qy.data(), h_nn.data(), nout, L,
                                      nsys, nblk);
        const bool ok_legacy = eng.run_weights_only(a_inv_c.data(), r_hp_c.data(), w_c.data(), y.data(), h_c.data(),
                                                    nout, L, nsys, nblk);
        double wmax = 0.0, hmax = 0.0, hleg = 0.0;
        unsigned wbad = 0, hbad = 0, hlegbad = 0;
        for (unsigned s = 0; s != nsys; ++s) {
          for (unsigned o = 0; o != nout; ++o) {
            for (unsigned c = 0; c != L; ++c) {
              const double g = w_pad[(static_cast<std::size_t>(s) * Np + o) * Lp + c];
              const double d = std::abs(g - w_ref[(static_cast<std::size_t>(s) * nout + o) * L + c]);
              wmax = std::max(wmax, d);
              if (d > 1e-3) wbad++;
            }
          }
        }
        for (std::size_t i = 0; i != h_nn.size(); ++i) {
          const double d  = std::abs(static_cast<double>(h_nn[i]) - h_ref[i]);
          const double dl = std::abs(static_cast<double>(h_c[i]) - h_ref[i]);
          hmax = std::max(hmax, d);
          hleg = std::max(hleg, dl);
          if (d > 1e-3) hbad++;
          if (dl > 1e-3) hlegbad++;
        }
        std::printf("Test 8c: odd dims nout=%u L=%u (padded %ux%u): nn W err %.2e h err %.2e | legacy h err %.2e\n",
                    nout, L, Np, Lp, wmax, hmax, hleg);
        if (!ok_nn || !ok_legacy || wbad != 0 || hbad != 0 || hlegbad != 0) {
          std::printf("Test 8c FAIL: padded kernels deviate from the reference (wbad=%u hbad=%u)\n", wbad, hbad);
          return -1;
        }
      }
    }
    std::printf("Test 8 PASS: metal_nn_mmse parity + zero-padding (nn always engaged)\n");
  }

  // -----------------------------------------------------------------------------------
  // Test 9: absolute-level consistency (the CE has never been in the LLR comparison loop).
  //
  // The uplink runs at whatever level the radio gain and the UE power control produce, and the
  // equalizer's per-RE noise variance - built from the CE's noise variance - is what scales the
  // soft bits: the demapper computes LLR ~ f(equalized symbol) / nv.  A common scaling of the
  // received signal and of the noise (same SNR, different level) must therefore leave the CE's
  // noise variance, its SNR, its RSRP and its channel estimates proportional to the level only
  // (level^2 for the powers), for BOTH the classical and the metal estimator, and the two must
  // agree with each other at every level.  If they do not, the two paths quantize the soft bits
  // differently at the level the OTA link happens to run at: too large an LLR saturates (high
  // order modulation then decodes worse than QPSK), too small a one rounds to zero and the
  // decoder trims the block and skips the decode.
  // -----------------------------------------------------------------------------------
  {
    const double                snr_db = 20.0;
    // The uplink runs its DFT output far below the unit-power levels a lab sweep usually uses:
    // the OTA reports DM-RS EPRE around -70 dBFS, i.e. pilot amplitudes near 1e-3.5. The sweep
    // therefore has to reach -100 dB, where a fixed absolute ridge (let alone an absolute sigma2)
    // would dominate the correlation model.
    const std::array<double, 8> levels = {1.0, 0.3, 0.1, 0.03, 0.01, 1e-3, 1e-4, 1e-5};
    const unsigned              n_real = 40;

    // Production shapes: the E2E cell uses three DM-RS symbols (pos2 + additional position 2) and
    // the PUSCH processor sets scaling = 10^(-SCH_to_DMRS/20), i.e. 0.708 for two CDM groups
    // without data. Neither was part of the lab sweeps, so sweep them here.
    struct shape_t {
      bool        three_dmrs;
      float       scaling;
      const char* name;
    };
    const std::array<shape_t, 4> shapes = {{{false, 1.0F, "2 DMRS beta=1"},
                                            {true, 1.0F, "3 DMRS beta=1"},
                                            {true, 0.708F, "3 DMRS beta=0.708"},
                                            {false, 2.0F, "2 DMRS beta=2"}}};

    auto cpu = std::make_unique<port_channel_estimator_average_impl>(
        create_interpolator(),
        make_ta_estimator(),
        port_channel_estimator_fd_smoothing_strategy::filter,
        port_channel_estimator_td_interpolation_strategy::average,
        true);
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);

    for (const shape_t& shape : shapes) {
      auto cfg    = make_config(51, true, 0, shape.three_dmrs, false);
      cfg.scaling = shape.scaling;
      auto pilots = make_pilots(51, shape.three_dmrs ? 3 : 2);

    std::printf("Test 9 [%s]: level consistency (SNR %.0f dB), values normalized by level^2 / "
                "level\n",
                shape.name,
                snr_db);
    std::printf("  %-7s | %-13s %-13s %-9s %-9s %-9s %-9s %-11s %-11s\n",
                "level",
                "cpu nv/l^2",
                "mmse nv/l^2",
                "cpu snr",
                "mmse snr",
                "cpu rsrp/l^2",
                "mmse rsrp/l^2",
                "cpu |h|/l",
                "mmse |h|/l");

    // Level invariance is the invariant that matters here: each quantity, normalized by its own
    // level dependence (level^2 for powers, level for amplitudes), must be constant across the
    // sweep. The two estimators are different algorithms and may legitimately differ from each
    // other by a fraction of a dB, so the cross-path difference is reported, not asserted.
    struct drift_t {
      double lo = std::numeric_limits<double>::infinity();
      double hi = 0.0;
      void   add(double v)
      {
        if (v > 0.0) {
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      }
      double ratio() const { return (lo > 0.0) ? hi / lo : std::numeric_limits<double>::infinity(); }
    };
    drift_t dr_nv_cpu;
    drift_t dr_nv_mmse;
    drift_t dr_snr_cpu;
    drift_t dr_snr_mmse;
    drift_t dr_h_cpu;
    drift_t dr_h_mmse;
    double  worst_cross_db = 0.0;
    double  worst_cross_h  = 0.0;

    for (double level : levels) {
      const double sigma2 = level * level * std::pow(10.0, -snr_db / 10.0);
      std::normal_distribution<float> gauss(0.0F, static_cast<float>(std::sqrt(sigma2 / 2.0)));

      double nv_cpu = 0.0;
      double nv_mmse = 0.0;
      double snr_cpu = 0.0;
      double snr_mmse = 0.0;
      double rsrp_cpu = 0.0;
      double rsrp_mmse = 0.0;
      double h_cpu = 0.0;
      double h_mmse = 0.0;

      for (unsigned r = 0; r != n_real; ++r) {
        veha_channel                   ch(rng);
        grid_fake                      grid(612);
        std::vector<cf_t>              rx_sym(612, cf_t{0.0F, 0.0F});
        std::vector<std::vector<cf_t>> h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(612));
        for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
          for (unsigned k = 0; k != 612; ++k) {
            h_true[l][k] = ch(k);
          }
        }
        for (unsigned sym = 0; sym != 2; ++sym) {
          const unsigned l = (sym == 0) ? 2U : 11U;
          std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
          unsigned j = 0;
          for (unsigned prb = 0; prb != 51; ++prb) {
            for (unsigned pos = 0; pos != 12; pos += 2) {
              const unsigned k = prb * 12 + pos;
              rx_sym[k]        = h_true[l][k] * pilots.get_symbol(sym, 0)[j] * static_cast<float>(level) +
                          cf_t{gauss(rng), gauss(rng)};
              ++j;
            }
          }
          grid.set_symbol(l, rx_sym);
        }

        auto measure = [&](port_channel_estimator& est, double& nv_out, double& snr_out, double& rsrp_out,
                           double& h_out) {
          const port_channel_estimator_results& res = est.compute(grid, 0, pilots, cfg);
          nv_out += static_cast<double>(res.get_noise_variance());
          snr_out += static_cast<double>(res.get_snr());
          rsrp_out += static_cast<double>(res.get_rsrp(0));
          std::vector<cbf16_t> est_sym(612);
          res.get_symbol_ch_estimate(est_sym, 7, 0);
          double acc = 0.0;
          for (unsigned k = 0; k != 612; ++k) {
            const cf_t e = to_cf(est_sym[k]);
            acc += static_cast<double>(std::abs(e));
          }
          h_out += acc / 612.0;
        };

        measure(*cpu, nv_cpu, snr_cpu, rsrp_cpu, h_cpu);
        measure(*mmse, nv_mmse, snr_mmse, rsrp_mmse, h_mmse);
      }

      const double n = static_cast<double>(n_real);
      nv_cpu /= n;
      nv_mmse /= n;
      snr_cpu /= n;
      snr_mmse /= n;
      rsrp_cpu /= n;
      rsrp_mmse /= n;
      h_cpu /= n;
      h_mmse /= n;

      const double l2 = level * level;
      std::printf("  %-7.3f | %-13.3e %-13.3e %-9.2f %-9.2f %-9.3e %-9.3e %-11.4f %-11.4f\n",
                  level,
                  nv_cpu / l2,
                  nv_mmse / l2,
                  snr_cpu,
                  snr_mmse,
                  rsrp_cpu / l2,
                  rsrp_mmse / l2,
                  h_cpu / level,
                  h_mmse / level);

      // The equalizer turns the CE's noise variance into the demapper's per-RE noise variance, so
      // a relative difference between the two paths multiplies the soft bits by 1 + that error.
      dr_nv_cpu.add(nv_cpu / l2);
      dr_nv_mmse.add(nv_mmse / l2);
      dr_snr_cpu.add(snr_cpu);
      dr_snr_mmse.add(snr_mmse);
      dr_h_cpu.add(h_cpu / level);
      dr_h_mmse.add(h_mmse / level);
      if ((nv_cpu > 0.0) && (nv_mmse > 0.0)) {
        worst_cross_db = std::max(worst_cross_db, std::abs(10.0 * std::log10(nv_mmse / nv_cpu)));
      }
      if (h_cpu > 0.0) {
        worst_cross_h = std::max(worst_cross_h, std::abs(h_mmse / h_cpu - 1.0));
      }
    }

    // A level-dependent estimator shows up as a huge drift of the normalized values (the defect
    // fixed here made nv/level^2 move by five orders of magnitude over this sweep).
    const double drift_nv_mmse = dr_nv_mmse.ratio();
    const double drift_h_mmse  = dr_h_mmse.ratio();
    std::printf("Test 9: drift across the sweep (max/min): nv/l^2 cpu %.3f mmse %.3f | snr cpu %.3f "
                "mmse %.3f | |h|/l cpu %.3f mmse %.3f\n",
                dr_nv_cpu.ratio(),
                drift_nv_mmse,
                dr_snr_cpu.ratio(),
                dr_snr_mmse.ratio(),
                dr_h_cpu.ratio(),
                drift_h_mmse);
    std::printf("Test 9: worst cross-path noise variance difference %.2f dB on the soft-bit scale "
                "(the two estimators are different algorithms; this is not the invariant)\n",
                worst_cross_db);
    // The |h| drift of the two paths is identical by construction (the input realizations are the
    // same at every level), so only the noise variance has to be flat AND the two paths have to
    // agree with each other at every level.
    std::printf("Test 9: worst cross-path |h| difference %.3f%%\n", 100.0 * worst_cross_h);
    if ((drift_nv_mmse > 1.5) || (dr_nv_cpu.ratio() > 1.5) || (worst_cross_h > 0.02)) {
      std::printf("Test 9 FAIL: the estimator results follow the input level instead of the SNR, so "
                  "the soft-bit scale of the uplink depends on the radio gain\n");
      return -1;
    }
      std::printf("Test 9 [%s] PASS: level-consistent channel estimator results\n", shape.name);
    }
    std::printf("Test 9 PASS: level-consistent channel estimator results\n");
  }

  // -----------------------------------------------------------------------------------
  // Test 11: the merged standard+tail batch estimates what the split path estimates (S-5c).
  // Both paths run over identical pilots, so the reference is the OTHER estimator and not the
  // true channel: every mistake the merge can make - the tail taking the standard geometry, the
  // unpack reading with the block geometry instead of the batch strides, or the tail matrices
  // overwriting the standard ones - shows up as a large difference on the blocks it touches.
  // The knob is read per call, so one binary runs both paths back to back.
  // -----------------------------------------------------------------------------------
  {
    struct merge_shape {
      unsigned n_prb;
      unsigned n_sym;
      bool     expect_merge; // the batch must be merged: an edge block and enough layers fit
    };
    // block_prb = 3: 52 and 25 PRB leave a ONE-PRB edge block, 51 does not, and a 2 PRB hop is a
    // single narrow block (no standard block to merge with). 23 and 14 PRB leave a TWO-PRB edge
    // block, which is the geometry the pilot staging used to read from the wrong PRB: the merged
    // and the split path agreed with each other there (they share the staging), so this shape is
    // only caught by the NMSE against the synthetic truth below.
    const std::array<merge_shape, 10> shapes = {{{52, 2, true}, {52, 1, true}, {25, 2, true},
                                                {4, 3, true}, {51, 2, false}, {2, 2, false},
                                                {23, 2, true}, {14, 2, true},
                                                {23, 3, true}, {14, 3, true}}};
    double   worst_rel_h  = 0.0;
    double   worst_dn_db  = 0.0;
    double   worst_nmse_edge_db = -1000.0;
    unsigned n_tail_re    = 0;

    for (const merge_shape& shape : shapes) {
      const unsigned n_prb = shape.n_prb;
      const unsigned n_sym = shape.n_sym;
      auto           cfg   = make_config(n_prb, n_sym != 1, 0, n_sym == 3, n_sym == 4);
      auto           pilots = make_pilots(n_prb, n_sym);
      veha_channel   ch(rng);

      // DM-RS slot symbols of each supported shape (mirrors make_config()).
      std::vector<unsigned> dmrs_l;
      if (n_sym == 4) {
        dmrs_l = {2, 7, 11, 12};
      } else {
        dmrs_l = {2};
        if (n_sym >= 2) {
          dmrs_l.push_back(11);
        }
        if (n_sym == 3) {
          dmrs_l.push_back(7);
        }
      }

      std::vector<std::vector<cf_t>> h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(n_prb * 12));
      grid_fake                      grid(n_prb * 12);
      std::vector<cf_t>              rx_sym(n_prb * 12, {0.0F, 0.0F});
      std::normal_distribution<float> gauss(0.0F, 0.4F);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          h_true[l][k] = ch(k);
        }
      }
      for (unsigned s = 0; s != n_sym; ++s) {
        const unsigned l = dmrs_l[s];
        std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
        unsigned j = 0;
        for (unsigned prb = 0; prb != n_prb; ++prb) {
          for (unsigned pos = 0; pos != 12; pos += 2) {
            const unsigned k = prb * 12 + pos;
            rx_sym[k]        = h_true[l][k] * pilots.get_symbol(s, 0)[j] + cf_t{gauss(rng), gauss(rng)};
            ++j;
          }
        }
        grid.set_symbol(l, rx_sym);
      }

      const auto make_est = []() {
        return std::make_unique<port_channel_estimator_metal_mmse_impl>(
            create_interpolator(),
            make_ta_estimator(),
            std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
            3,
            true);
      };
      auto est_merged = make_est();
      auto est_split  = make_est();

      const auto run = [&](port_channel_estimator_metal_mmse_impl& est, std::vector<std::vector<cf_t>>& out) {
        const auto& res = est.compute(grid, 0, pilots, cfg);
        for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
          std::vector<cbf16_t> e(n_prb * 12);
          res.get_symbol_ch_estimate(e, l, 0);
          for (unsigned k = 0; k != n_prb * 12; ++k) {
            out[l][k] = to_cf(e[k]);
          }
        }
      };
      std::vector<std::vector<cf_t>> h_merged(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(n_prb * 12));
      std::vector<std::vector<cf_t>> h_split(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(n_prb * 12));

      unsetenv("OCUDU_CE_SPLIT_TAIL");
      run(*est_merged, h_merged);
      const bool merged_engaged = est_merged->merged_batch_last();
      setenv("OCUDU_CE_SPLIT_TAIL", "1", 1);
      run(*est_split, h_split);
      const bool split_engaged = est_split->merged_batch_last();
      unsetenv("OCUDU_CE_SPLIT_TAIL");

      // A path that silently takes the other branch would make this comparison vacuous.
      // OCUDU_CE_TAIL_CPU=1 is the A/B knob that deliberately routes the tail to the CPU and so
      // disables the merge: it relaxes the expectation, the comparison stays valid either way.
      const bool tail_on_cpu  = (std::getenv("OCUDU_CE_TAIL_CPU") != nullptr);
      const bool expect_merge = shape.expect_merge && !tail_on_cpu;
      const bool engaged_ok   = (merged_engaged == expect_merge) && !split_engaged;

      double err_m = 0.0, err_s = 0.0, sig = 0.0, worst = 0.0;
      // Error of the merged path against the truth, split by region: the standard blocks and the
      // edge block. The two are compared with each other instead of with an absolute level,
      // because the achievable NMSE depends on the bf16 output and on the synthetic noise.
      double err_m_edge = 0.0, err_m_std = 0.0, sig_edge = 0.0, sig_std = 0.0;
      const unsigned tail_sc  = (n_prb - (n_prb / 3) * 3) * 12;
      const unsigned edge_first_sc = n_prb * 12 - tail_sc;
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          const cf_t e = h_merged[l][k] - h_split[l][k];
          const double ad = std::sqrt(std::norm(e));
          // Only the edge block can differ at all: the standard blocks run the identical batch in
          // both paths. The gate below is a fraction of the signal RMS, so it is independent of
          // the channel gain.
          worst = std::max(worst, ad);
          const cf_t dm = h_merged[l][k] - h_true[l][k];
          const cf_t ds = h_split[l][k] - h_true[l][k];
          err_m += std::norm(dm);
          err_s += std::norm(ds);
          sig += std::norm(h_true[l][k]);
          if ((tail_sc != 0) && (k >= edge_first_sc)) {
            err_m_edge += std::norm(dm);
            sig_edge += std::norm(h_true[l][k]);
          } else {
            err_m_std += std::norm(dm);
            sig_std += std::norm(h_true[l][k]);
          }
        }
      }
      if ((tail_sc != 0) && (std::getenv("OCUDU_CE_EDGE_DIAG") != nullptr)) {
        for (unsigned prb = 0; prb != n_prb; ++prb) {
          double e2 = 0.0, s2 = 0.0;
          for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
            for (unsigned k = prb * 12; k != (prb + 1) * 12; ++k) {
              e2 += std::norm(h_merged[l][k] - h_true[l][k]);
              s2 += std::norm(h_true[l][k]);
            }
          }
          std::printf("[edge_diag] %2u PRB: NMSE %7.2f dB%s\n", n_prb, 10.0 * std::log10(e2 / std::max(s2, 1e-30)),
                      (prb * 12 >= static_cast<int>(edge_first_sc)) ? "  <- edge" : "");
        }
      }
      const double nmse_m_edge = 10.0 * std::log10(err_m_edge / std::max(sig_edge, 1e-30));
      const double nmse_m_std  = 10.0 * std::log10(err_m_std / std::max(sig_std, 1e-30));
      const double rms      = std::sqrt(sig / (MAX_NSYMB_PER_SLOT * n_prb * 12));
      const double rel      = (rms > 0.0) ? worst / rms : 0.0;
      const double nmse_m   = 10.0 * std::log10(err_m / sig);
      const double nmse_s   = 10.0 * std::log10(err_s / sig);
      const double d_nmse   = std::abs(nmse_m - nmse_s);
      // The public estimate is bf16 (8-bit mantissa), so the comparison floor is one quantization
      // step: the two paths agree exactly unless a value lands on a rounding boundary.
      const double bf16_step = std::ldexp(1.0, -8);
      worst_rel_h            = std::max(worst_rel_h, rel);
      worst_dn_db            = std::max(worst_dn_db, d_nmse);
      if (shape.expect_merge) {
        n_tail_re += tail_sc * MAX_NSYMB_PER_SLOT;
      }
      std::printf("Test 11 (%2u PRB, %u DMRS): merged=%d split=%d | max|dh|/rms %.2e (%.0f dB, bf16 step %.2e) "
                  "| NMSE merged %.2f dB split %.2f dB (d %.3f dB)\n",
                  n_prb,
                  n_sym,
                  merged_engaged ? 1 : 0,
                  split_engaged ? 1 : 0,
                  rel,
                  20.0 * std::log10(std::max(rel, 1e-12)),
                  bf16_step,
                  nmse_m,
                  nmse_s,
                  d_nmse);
      if (!engaged_ok) {
        std::printf("Test 11 FAIL: merged_batch_last() is %d (expected %d) / split path %d for %u PRB\n",
                    merged_engaged ? 1 : 0,
                    expect_merge ? 1 : 0,
                    split_engaged ? 1 : 0,
                    n_prb);
        return -1;
      }
      // Accuracy gate against the synthetic truth, edge block against standard blocks: the
      // merged-vs-split comparison above is blind to anything the two paths share - the pilot
      // staging among it - so the edge block is asserted here. block_prb is 3, so the two-PRB edge
      // of a 23 or 14 PRB hop is the geometry a wrong pilot offset corrupts (it reads the pilots of
      // another PRB, which a smooth synthetic channel hides in the time domain but not here).
      // With no standard block at all (a hop narrower than block_prb) the comparison has no
      // reference, and the split path is the only one taken anyway.
      const bool has_std = (n_prb / 3) != 0;
      if (tail_sc != 0 && has_std) {
        worst_nmse_edge_db = std::max(worst_nmse_edge_db, nmse_m_edge - nmse_m_std);
      }
      // The merged-vs-split comparison above cannot see anything the two paths share, and they
      // share both halves of the edge block's plumbing: the pilot staging of block b, and the host
      // unpack that places the block's estimates in the grid. A two-PRB edge block (a hop whose PRB
      // count is 2 mod block_prb, i.e. 23 and 14 PRB here) is the geometry both of those used to
      // get wrong - reading and writing the pilots of another PRB, and, in the unpack, addressing
      // past the end of the grid - so the edge block is asserted against the synthetic truth,
      // relative to the standard blocks of the same hop (the absolute NMSE depends on the bf16
      // output and on the synthetic noise, the ratio does not).
      if (tail_sc != 0 && has_std && (nmse_m_edge > nmse_m_std + 10.0)) {
        std::printf("Test 11 FAIL: the merged batch does not estimate the edge block "
                    "(%u PRB, %u DMRS: edge NMSE %.2f dB vs standard %.2f dB)\n",
                    n_prb,
                    n_sym,
                    nmse_m_edge,
                    nmse_m_std);
        return -1;
      }
      if ((rel > 0.01) || (d_nmse > 0.05)) {
        std::printf("Test 11 FAIL: the merged batch does not estimate what the split path estimates "
                    "(%u PRB, %u DMRS: max|dh|/rms %.3e, dNMSE %.3f dB)\n",
                    n_prb,
                    n_sym,
                    rel,
                    d_nmse);
        return -1;
      }
    }
    std::printf("Test 11 PASS: the merged standard+tail batch matches the split path "
                "(worst max|dh|/rms %.2e over %u tail REs, worst dNMSE %.3f dB) and estimates the "
                "true channel (worst edge-minus-standard NMSE %.2f dB)\n",
                worst_rel_h,
                n_tail_re,
                worst_dn_db,
                worst_nmse_edge_db);
  }

  // -----------------------------------------------------------------------------------
  // Test 12: the device-side per-symbol estimates (K3) are exactly what the host path produces.
  // K3 does the relayout, the mask gather and the float -> bfloat16 rounding on the GPU, so this
  // compares its output RE by RE against the gather the demodulator performs on the CPU today, over
  // the same masks - built here the way the demodulator builds them (the allocation expanded to
  // subcarriers, minus the DM-RS comb of the DM-RS symbols) rather than the way the estimator
  // derives them, so a divergence between the two mask rules fails here instead of in the
  // equalizer.
  // -----------------------------------------------------------------------------------
  {
    struct device_shape {
      unsigned                n_prb;
      unsigned                n_sym;
      /// DC subcarrier of the hop (relative to its first subcarrier), when the allocation has one.
      std::optional<unsigned> dc;
    };
    const std::array<device_shape, 6> shapes = {{{52, 2, {}},
                                                 {25, 2, {}},
                                                 {4, 3, {}},
                                                 {51, 2, {}},
                                                 {2, 2, {}},
                                                 // The DC subcarrier carries no data: the producer
                                                 // must write a zero estimate there for the
                                                 // equalizer (see the host path's own erasure).
                                                 {25, 2, 137}}};
    unsigned                          total_checked = 0;
    float                             worst_nv_rel  = 0.0F;

    // One instance for every shape: the estimator memoizes the staged masks per allocation, so this
    // also exercises the invalidation when the allocation changes from hop to hop.
    unsetenv("OCUDU_CE_CPU_CE");
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);

    for (const device_shape& shape : shapes) {
      const unsigned                  n_prb  = shape.n_prb;
      const unsigned                  n_sym  = shape.n_sym;
      auto                            cfg    = make_config(n_prb, n_sym != 1, 0, n_sym == 3, n_sym == 4);
      cfg.dc_position                        = shape.dc;
      auto                            pilots = make_pilots(n_prb, n_sym);
      veha_channel                    ch(rng);
      grid_fake                       grid(n_prb * 12);
      std::vector<cf_t>               rx_sym(n_prb * 12, {0.0F, 0.0F});
      std::normal_distribution<float> gauss(0.0F, 0.4F);
      std::vector<std::vector<cf_t>>  h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(n_prb * 12));
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        for (unsigned k = 0; k != n_prb * 12; ++k) {
          h_true[l][k] = ch(k);
        }
      }
      std::vector<unsigned> dmrs_l;
      if (n_sym == 4) {
        dmrs_l = {2, 7, 11, 12};
      } else {
        dmrs_l = {2};
        if (n_sym >= 2) {
          dmrs_l.push_back(11);
        }
        if (n_sym == 3) {
          dmrs_l.push_back(7);
        }
      }
      for (unsigned s = 0; s != n_sym; ++s) {
        const unsigned l = dmrs_l[s];
        std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
        unsigned j = 0;
        for (unsigned prb = 0; prb != n_prb; ++prb) {
          for (unsigned pos = 0; pos != 12; pos += 2) {
            const unsigned k = prb * 12 + pos;
            rx_sym[k]        = h_true[l][k] * pilots.get_symbol(s, 0)[j] + cf_t{gauss(rng), gauss(rng)};
            ++j;
          }
        }
        grid.set_symbol(l, rx_sym);
      }

      const auto& res = mmse->compute(grid, 0, pilots, cfg);

      if (!mmse->device_estimates_ready_last()) {
        std::printf("Test 12 FAIL: the device estimates were not produced (%u PRB, %u DMRS)\n", n_prb, n_sym);
        return -1;
      }

      // DM-RS REs within a PRB and the DM-RS symbols of the slot, as the demodulator sees them for
      // this configuration (the estimator's RE pattern is the type-1 single-CDM-group comb).
      const re_prb_mask         dmrs_prb  = get_dmrs_prb_mask(dmrs_config_type::type1, 1);
      const auto&               slot_dmrs = cfg.dmrs_pattern.front().symbols;
      const span<const unsigned> offs     = mmse->device_estimate_offsets();
      unsigned                  bad       = 0;
      unsigned                  checked   = 0;

      for (unsigned i_layer = 0; i_layer != mmse->device_estimate_layers(); ++i_layer) {
        const cbf16_t* dev = mmse->device_estimate_layer(i_layer);
        for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
          std::vector<cbf16_t> dense(n_prb * 12);
          res.get_symbol_ch_estimate(dense, sym, i_layer);

          // Host gather: the demodulator's data-RE mask, in ascending subcarrier order.
          std::vector<cbf16_t> ref;
          const bool           is_dmrs = slot_dmrs.test(sym);
          for (unsigned sc = 0; sc != n_prb * 12; ++sc) {
            if (is_dmrs && dmrs_prb.test(sc % NOF_SUBCARRIERS_PER_RB)) {
              continue;
            }
            // The host path erases the DC resource element when it gathers the estimates (the
            // device path must have written the zero itself).
            ref.push_back((cfg.dc_position.has_value() && (*cfg.dc_position == sc)) ? cbf16_t() : dense[sc]);
          }

          const unsigned nof_re = offs[sym + 1] - offs[sym];
          if (nof_re != ref.size()) {
            std::printf("Test 12 FAIL: symbol %u of %u PRB/%u DMRS has %u device REs, while the demodulator's "
                        "mask has %u\n",
                        sym,
                        n_prb,
                        n_sym,
                        nof_re,
                        static_cast<unsigned>(ref.size()));
            return -1;
          }
          for (unsigned j = 0; j != nof_re; ++j) {
            if (dev[offs[sym] + j] != ref[j]) {
              ++bad;
            }
          }
          checked += nof_re;
        }
      }
      total_checked += checked;

      // K4 (S-6c-0): the noise variance the equalizer scales its soft bits with is reduced on the
      // device out of the same h. The reduction order is not the host's, so this compares the two
      // values numerically - a bit-for-bit match is not achievable for a sum over hundreds of
      // terms, and the quantity is a statistic.
      const float* dev_nv  = mmse->device_noise_variance();
      const float  host_nv = res.get_noise_variance();
      if (dev_nv == nullptr) {
        std::printf("Test 12 FAIL: the device noise variance was not produced (%u PRB)\n", n_prb);
        return -1;
      }
      const float nv_rel = std::abs(*dev_nv - host_nv) / std::max(std::abs(host_nv), 1e-30F);
      worst_nv_rel       = std::max(worst_nv_rel, nv_rel);
      if (!(nv_rel < 1e-5F)) {
        std::printf("Test 12 FAIL: device noise variance %.9e vs host %.9e (relative %.3e)\n",
                    static_cast<double>(*dev_nv),
                    static_cast<double>(host_nv),
                    static_cast<double>(nv_rel));
        return -1;
      }

      std::printf("Test 12 (%2u PRB, %u DMRS%s): device estimates %s (%u REs checked, %u mismatching), "
                  "noise variance %s (relative %.2e)\n",
                  n_prb,
                  n_sym,
                  cfg.dc_position.has_value() ? ", DC" : "",
                  (bad == 0) ? "match the host path" : "DEVIATE",
                  checked,
                  bad,
                  (nv_rel < 1e-5F) ? "matches" : "DEVIATES",
                  static_cast<double>(nv_rel));
      if (bad != 0) {
        std::printf("Test 12 FAIL: %u of %u device REs differ from the host gather\n", bad, checked);
        return -1;
      }
    }
    std::printf("Test 12 PASS: K3 reproduces the host gather bit for bit (%u REs over %u shapes, one "
                "instance); K4 noise variance matches the host (worst relative %.2e)\n",
                total_checked,
                static_cast<unsigned>(shapes.size()),
                static_cast<double>(worst_nv_rel));

    // Negative case: the destination index is arithmetic, which needs a contiguous allocation. A
    // non-contiguous one must produce NO device estimates - the consumer then gathers them on the
    // host, which is always correct. Silently producing a shifted layout here would corrupt the
    // equalizer's input, which is exactly what the RE-count guard in the consumer catches.
    {
      auto cfg = make_config(12, true);
      cfg.dmrs_pattern.front().rb_mask.reset(5);
      auto                            pilots = make_pilots(12, 2);
      grid_fake                       grid(12 * 12);
      std::vector<cf_t>               rx_sym(12 * 12, {0.0F, 0.0F});
      std::normal_distribution<float> gauss(0.0F, 0.4F);
      for (unsigned k = 0; k != rx_sym.size(); ++k) {
        rx_sym[k] = cf_t{gauss(rng), gauss(rng)};
      }
      grid.set_symbol(2, rx_sym);
      grid.set_symbol(11, rx_sym);

      (void)mmse->compute(grid, 0, pilots, cfg);
      if (mmse->device_estimates_ready_last()) {
        std::printf("Test 12 FAIL: a non-contiguous allocation produced device estimates\n");
        return -1;
      }
      std::printf("Test 12 (12 PRB with a hole): no device estimates, as expected\n");
    }
  }

  // -----------------------------------------------------------------------------------
  // Test 13 (S-7g-16, Step 1b): the FUSED lane produces the synchronous result, byte for byte.
  //
  // With OCUDU_CE_FUSED_BURST=1 a hop the caller leaves running (port_channel_estimator::submit())
  // encodes its dispatches into the command buffer the equalizer and the demapper share, and reads
  // two scalars back out of it when the hop completes (the device's sigma2 and pilots_power). Both
  // halves can fail SILENTLY - dispatches nobody committed are read as the previous hop's memory,
  // and a completion that ran before its command buffer did is indistinguishable from a good one -
  // so the gate is that every published value is byte-identical to the synchronous route the air
  // path was verified with. This test also pins the two completion orders the receiving chain has:
  //   * the lane commits the shared burst first, then the estimator completes the hop - the
  //     in-place route, where the equalizer reads the estimates where the GPU wrote them;
  //   * the estimator completes before anyone committed - the host-read route (OCUDU_CE_CPU_CE, the
  //     debug capture): the completion has to commit the burst itself;
  // and that a hop that must NOT fuse (compute(), which completes what it submits) runs on the
  // engine's own command buffer even after a fused hop has switched the adapter over.
  // -----------------------------------------------------------------------------------
  {
    // One instance for the whole test: the fused/synchronous decision is per-hop state of the
    // adapter, and a stale one is precisely what the last part of this test looks for.
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(),
        make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F),
        3,
        true);

    /// Everything a hop publishes, in a form that a byte comparison can judge: the whole estimated
    /// grid (every symbol and layer, which is what the equalizer and the dump read) plus the
    /// scalars the completion reads back from the command buffer.
    struct published {
      std::vector<cbf16_t> grid;
      float                noise_var = 0.0F;
      float                snr       = 0.0F;
      float                epre      = 0.0F;
      float                rsrp      = 0.0F;
      float                cfo       = 0.0F;
      long                 ta_ps     = 0;
      bool                 has_cfo   = false;

      bool operator==(const published& o) const
      {
        return (grid == o.grid) && (noise_var == o.noise_var) && (snr == o.snr) && (epre == o.epre) &&
               (rsrp == o.rsrp) && (cfo == o.cfo) && (ta_ps == o.ta_ps) && (has_cfo == o.has_cfo);
      }
    };

    const auto read_back = [](const port_channel_estimator_results& res, unsigned nof_subc, unsigned nof_layers) {
      published out;
      out.grid.resize(static_cast<std::size_t>(nof_layers) * MAX_NSYMB_PER_SLOT * nof_subc);
      for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
        for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
          res.get_symbol_ch_estimate(
              span<cbf16_t>(out.grid).subspan((static_cast<std::size_t>(i_layer) * MAX_NSYMB_PER_SLOT + sym) * nof_subc,
                                              nof_subc),
              sym,
              i_layer);
        }
      }
      out.noise_var = res.get_noise_variance();
      out.snr       = res.get_snr();
      out.epre      = res.get_epre();
      out.rsrp      = res.get_rsrp(0);
      if (std::optional<float> cfo = res.get_cfo_Hz(); cfo.has_value()) {
        out.has_cfo = true;
        out.cfo     = *cfo;
      }
      out.ta_ps = res.get_time_alignment().to_seconds() * 1e12;
      return out;
    };

    /// The input of one hop, built once per shape so that every route sees the same samples.
    struct hop_input {
      port_channel_estimator::configuration cfg;
      dmrs_symbol_list                     pilots;
      grid_fake                            grid;
      unsigned                             nof_subc;

      explicit hop_input(unsigned n_prb, unsigned n_sym, std::mt19937& rng_) :
        cfg(make_config(n_prb, n_sym != 1, 0, n_sym == 3, n_sym == 4)),
        pilots(make_pilots(n_prb, n_sym)),
        grid(n_prb * 12),
        nof_subc(n_prb * 12)
      {
        veha_channel                    ch(rng_);
        std::normal_distribution<float> gauss(0.0F, 0.4F);
        std::vector<std::vector<cf_t>>  h_true(MAX_NSYMB_PER_SLOT, std::vector<cf_t>(nof_subc));
        for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
          for (unsigned k = 0; k != nof_subc; ++k) {
            h_true[l][k] = ch(k);
          }
        }
        std::vector<cf_t> rx_sym(nof_subc, cf_t{0.0F, 0.0F});
        const std::array<unsigned, 4> dmrs_l = (n_sym == 4) ? std::array<unsigned, 4>{2, 7, 11, 12}
                                               : (n_sym == 3) ? std::array<unsigned, 4>{2, 7, 11, 0}
                                                              : std::array<unsigned, 4>{2, 11, 0, 0};
        for (unsigned s = 0; s != n_sym; ++s) {
          const unsigned l = dmrs_l[s];
          std::fill(rx_sym.begin(), rx_sym.end(), cf_t{0.0F, 0.0F});
          unsigned j = 0;
          for (unsigned prb = 0; prb != n_prb; ++prb) {
            for (unsigned pos = 0; pos != 12; pos += 2) {
              const unsigned k = prb * 12 + pos;
              rx_sym[k]        = h_true[l][k] * pilots.get_symbol(s, 0)[j] + cf_t{gauss(rng_), gauss(rng_)};
              ++j;
            }
          }
          grid.set_symbol(l, rx_sym);
        }
      }
    };

    /// The knob the adapter reads per hop, so both routes can be compared inside this one process.
    /// "off" is an explicit 0, not an unset variable: the fusion is the DEFAULT now, and the routes
    /// below are the A/B of it.
    const auto set_knob = [](bool on) { setenv("OCUDU_CE_FUSED_BURST", on ? "1" : "0", 1); };

    const std::array<std::pair<unsigned, unsigned>, 4> shapes = {{{51, 2}, {25, 2}, {4, 3}, {12, 4}}};
    unsigned                                           n_checked = 0;
    for (const auto& [n_prb, n_sym] : shapes) {
      // A hopping slot is not covered here: only the LAST hop is left running, hop 0 completes
      // inside do_submit() and therefore never fuses (see do_submit()), which is exactly the
      // behaviour the air path shows.
      const unsigned    nof_layers = 1;
      const std::string label      = std::to_string(n_prb) + " PRB " + std::to_string(n_sym) + " DMRS";

      // One input per shape, reused by every route: the comparison is between ROUTES, so all of them
      // have to estimate the very same samples. (Nothing here mutates it - the grid and the pilots
      // are read-only inputs of a const-correct interface.)
      hop_input in(n_prb, n_sym, rng);

      set_knob(false);
      const auto ref = read_back(mmse->compute(in.grid, 0, in.pilots, in.cfg), in.nof_subc, nof_layers);

      // --- Route 0 (the control): the deferred entry point WITHOUT the knob, i.e. exactly the route the
      // air path runs today (submit() ... finish(), every stage in its own command buffer). It has to
      // match the synchronous route as well, and keeping it here separates "the deferred route is wrong"
      // from "the fusion is wrong" when one of the comparisons below fails.
      {
        const port_channel_estimator_results& res_ctl = mmse->submit(in.grid, 0, in.pilots, in.cfg);
        if (metal::mmse_engine::burst_is_open()) {
          std::printf("Test 13 FAIL (%s): the deferred route opened a burst with the knob off\n", label.c_str());
          return -1;
        }
        const bool      ctl_ok = mmse->finish(in.pilots);
        const published control = read_back(res_ctl, in.nof_subc, nof_layers);
        if (!ctl_ok || !(control == ref)) {
          std::printf("Test 13 FAIL (%s): the deferred route WITHOUT the knob already differs from the "
                      "synchronous one (noise variance %.9e vs %.9e) - this is not the fusion\n",
                      label.c_str(),
                      static_cast<double>(control.noise_var),
                      static_cast<double>(ref.noise_var));
          return -1;
        }
      }

      // --- Route 1: the synchronous entry point. It must not fuse, and it must still produce the
      // same values with the knob OFF as with it ON (the knob may only change WHOSE command buffer
      // carries the work) and after a fused hop has switched the adapter over.
      set_knob(true);
      const published      sync_on_knob = read_back(mmse->compute(in.grid, 0, in.pilots, in.cfg), in.nof_subc, nof_layers);
      if (!(sync_on_knob == ref)) {
        std::printf("Test 13 FAIL (%s): compute() changed with OCUDU_CE_FUSED_BURST set - the "
                    "synchronous route must never hand its work to the shared burst\n",
                    label.c_str());
        return -1;
      }

      // --- Route 2: deferred, completed by the estimator BEFORE the lane commits. This is the
      // host-read route, and the completion has to commit the burst: without that, the two scalars
      // below come from memory the GPU has not written.
      // submit() hands back the results interface, which is the only handle to it (the results base
      // class is private).
      const port_channel_estimator_results& res_c = mmse->submit(in.grid, 0, in.pilots, in.cfg);
      if (!metal::mmse_engine::burst_is_open()) {
        std::printf("Test 13 FAIL (%s): the fused hop left no burst open - the knob did not reach "
                    "the engine, so this test would prove nothing\n",
                    label.c_str());
        return -1;
      }
      const unsigned fused_dispatches = metal::mmse_engine::burst_dispatch_count();
      const bool     finish_ok        = mmse->finish(in.pilots);
      const published host_read       = read_back(res_c, in.nof_subc, nof_layers);
      if (!finish_ok) {
        std::printf("Test 13 FAIL (%s): the fused hop's completion reported a failed command buffer\n", label.c_str());
        return -1;
      }
      if (!(host_read == ref)) {
        const auto sum = [](const published& p) {
          double a = 0.0;
          for (cbf16_t v : p.grid) {
            a += static_cast<double>(to_float(v.real)) + static_cast<double>(to_float(v.imag));
          }
          return a;
        };
        std::printf("Test 13 FAIL (%s): the fused hop that completed before the lane committed does "
                    "not match the synchronous route\n"
                    "  ref    : nv %.9e snr %.6f epre %.6e rsrp %.6e grid %.6e\n"
                    "  fused  : nv %.9e snr %.6f epre %.6e rsrp %.6e grid %.6e\n",
                    label.c_str(),
                    static_cast<double>(ref.noise_var),
                    static_cast<double>(ref.snr),
                    static_cast<double>(ref.epre),
                    static_cast<double>(ref.rsrp),
                    sum(ref),
                    static_cast<double>(host_read.noise_var),
                    static_cast<double>(host_read.snr),
                    static_cast<double>(host_read.epre),
                    static_cast<double>(host_read.rsrp),
                    sum(host_read));
        return -1;
      }

      // --- Route 3: deferred, with the lane's commit in between - what the demodulator does when
      // the equalizer reads the estimates where the GPU wrote them. The completion must then find
      // nothing to do (a commit of its own would cut the lane's single command buffer in two).
      const port_channel_estimator_results& res_d = mmse->submit(in.grid, 0, in.pilots, in.cfg);
      const unsigned lane_dispatches = metal::mmse_engine::burst_dispatch_count();
      const bool     lane_committed  = metal::mmse_engine::burst_commit_and_wait();
      if (!lane_committed || (lane_dispatches == 0)) {
        std::printf("Test 13 FAIL (%s): the lane found no burst to commit (%u dispatches)\n",
                    label.c_str(),
                    lane_dispatches);
        return -1;
      }
      const bool     finish_ok2  = mmse->finish(in.pilots);
      const published in_place = read_back(res_d, in.nof_subc, nof_layers);
      if (!finish_ok2) {
        std::printf("Test 13 FAIL (%s): the completion after the lane's commit failed\n", label.c_str());
        return -1;
      }
      if (!(in_place == ref)) {
        std::printf("Test 13 FAIL (%s): the fused hop completed after the lane's commit does not "
                    "match the synchronous route (noise variance %.9e vs %.9e)\n",
                    label.c_str(),
                    static_cast<double>(in_place.noise_var),
                    static_cast<double>(ref.noise_var));
        return -1;
      }

      // --- Route 4: a synchronous hop AFTER a fused one. The adapter must have put the engine back
      // on its own command buffer, or this hop's work would sit in a burst nobody commits.
      const published sync_after = read_back(mmse->compute(in.grid, 0, in.pilots, in.cfg), in.nof_subc, nof_layers);
      if (!(sync_after == ref) || metal::mmse_engine::burst_is_open()) {
        std::printf("Test 13 FAIL (%s): a synchronous hop after a fused one did not return to the "
                    "engine's own command buffer\n",
                    label.c_str());
        return -1;
      }

      // --- Route 5: the DEFAULT. With the environment untouched the deferred hop must fuse: the fused
      // lane is the route the chain takes now (S-7g-16's goal), and OCUDU_CE_FUSED_BURST=0 - what the
      // routes above set - is the escape hatch. A default that silently drifted back to the unfused
      // route would leave every other check in this test green, so it is pinned here.
      unsetenv("OCUDU_CE_FUSED_BURST");
      const port_channel_estimator_results& res_def = mmse->submit(in.grid, 0, in.pilots, in.cfg);
      if (!metal::mmse_engine::burst_is_open()) {
        std::printf("Test 13 FAIL (%s): the DEFAULT route did not fuse (OCUDU_CE_FUSED_BURST unset)\n",
                    label.c_str());
        return -1;
      }
      const bool      def_ok     = mmse->finish(in.pilots);
      const published by_default = read_back(res_def, in.nof_subc, nof_layers);
      if (!def_ok || !(by_default == ref)) {
        std::printf("Test 13 FAIL (%s): the default (fused) route does not match the synchronous one\n",
                    label.c_str());
        return -1;
      }

      std::printf("Test 13 (%s): fused hop matches the synchronous route bit for bit "
                  "(%u dispatches in the burst, host-read and in-place orders, and by default)\n",
                  label.c_str(),
                  fused_dispatches);
      n_checked += fused_dispatches;
    }
    set_knob(false);   // leave the process on the explicit escape-hatch value; the default is on anyway
    std::printf("Test 13 PASS: the fused lane reproduces the synchronous result byte for byte over "
                "%u shapes (%u estimator dispatches carried by the shared burst), for both completion "
                "orders, and a synchronous hop still owns its command buffer\n",
                static_cast<unsigned>(shapes.size()),
                n_checked);
  }

  std::printf("All tests PASSED\n");
  return 0;
}
