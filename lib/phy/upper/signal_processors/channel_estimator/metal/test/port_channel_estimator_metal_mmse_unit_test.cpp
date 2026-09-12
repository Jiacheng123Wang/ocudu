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
#include <cstdio>
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
  std::mt19937 rng(1234);

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
      const unsigned nof_sys    = 4;
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
      engine.invert(a.data(), n, nof_sys);
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
      std::printf("Test 4a PASS: GPU batched inversion (max err %.2e, gpu %.1f us)\n",
                  max_err, engine.last_gpu_wait_us());

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
      std::printf("Test 6 (%u PRB, %u DMRS): metal_mmse NMSE %.2f dB (no crash)\n",
                  n_prb, n_sym, 10.0 * std::log10(err / sig));
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

  std::printf("All tests PASSED\n");
  return 0;
}
