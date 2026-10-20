// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_ul_slow_measurements.h"
#include <array>
#include <cmath>
#include <complex>
#include <limits>

using namespace ocudu;

namespace {

/// Second difference over three consecutive pilots of one symbol (`v[k] - 2 v[k-1] + v[k-2]`), which removes a
/// linear channel trend and leaves `1 + 4 + 1 = 6` times the noise variance.
///
/// THE ANALYTIC FACTOR, WITH NO EMPIRICAL CORRECTION. The 2.07 this tree carried was measured against the
/// THREE-POINT MEAN the estimator used before the second difference replaced it, and that mean under-estimated
/// white noise by 3.15 dB. Applying the same 2.07 to the second difference -- which has no such bias -- reports
/// the noise 3.15 dB HIGH and therefore the SNR 3.15 dB LOW. Measured by
/// depth3_slow_measurements_test against a flat channel with a known sigma^2: 0.02066 against a true 0.01, i.e.
/// exactly 10*log10(2.07) = 3.15 dB (design section 19.246).
constexpr float RESIDUAL_SECOND_DIFFERENCE = 6.0F;

constexpr unsigned WINDOW = 3;

} // namespace

ul_slow_measurements ocudu::measure_ul_slow(span<const ul_pilot_sample> pilots, float scaling, float nof_layers)
{
  ul_slow_measurements out;
  if (pilots.empty()) {
    return out;
  }

  double   epre_sum      = 0.0;
  double   residual_sum  = 0.0;
  unsigned residual_count = 0;

  // Per symbol a three-sample ring, so the second difference never spans two symbols. WINDOW is small and the
  // symbol count of a slot is fixed, so a fixed array indexed by the symbol's position in the stream is enough.
  struct ring_t {
    float  re[WINDOW] = {};
    float  im[WINDOW] = {};
    unsigned next     = 0;
    unsigned count    = 0;
  };
  std::array<ring_t, 16> rings{};

  // The first two distinct symbols, for the CFO.
  unsigned first_symbol  = std::numeric_limits<unsigned>::max();
  unsigned second_symbol = std::numeric_limits<unsigned>::max();
  std::array<std::complex<double>, 1024> first_ls{};
  std::array<std::complex<double>, 1024> second_ls{};
  unsigned                               first_count  = 0;
  unsigned                               second_count = 0;

  unsigned previous_symbol = std::numeric_limits<unsigned>::max();
  unsigned symbol_ordinal  = 0;
  for (const ul_pilot_sample& sample : pilots) {
    if (sample.symbol != previous_symbol) {
      // A new symbol: its ring starts empty, and the first two distinct symbols are what the CFO pairs.
      if (sample.symbol != first_symbol) {
        if (first_symbol == std::numeric_limits<unsigned>::max()) {
          first_symbol = sample.symbol;
        }
        else if (second_symbol == std::numeric_limits<unsigned>::max()) {
          second_symbol = sample.symbol;
        }
      }
      previous_symbol = sample.symbol;
      symbol_ordinal  = (symbol_ordinal + 1) % rings.size();
    }

    const float re = sample.ls_re;
    const float im = sample.ls_im;
    epre_sum += static_cast<double>(re) * re + static_cast<double>(im) * im;
    ++out.nof_pilot_re;

    const bool is_first  = (sample.symbol == first_symbol);
    const bool is_second = (sample.symbol == second_symbol);
    if (is_first && (first_count != first_ls.size())) {
      first_ls[first_count++] = {re, im};
    }
    else if (is_second && (second_count != second_ls.size())) {
      second_ls[second_count++] = {re, im};
    }

    ring_t& ring = rings[symbol_ordinal];
    ring.re[ring.next] = re;
    ring.im[ring.next] = im;
    ring.next          = (ring.next + 1) % WINDOW;
    if (ring.count < WINDOW) {
      ++ring.count;
    }
    else {
      const unsigned oldest = ring.next;
      const unsigned middle = (ring.next + 1) % WINDOW;
      const unsigned newest = (ring.next + 2) % WINDOW;
      const float    d_re   = ring.re[newest] - 2.0F * ring.re[middle] + ring.re[oldest];
      const float    d_im   = ring.im[newest] - 2.0F * ring.im[middle] + ring.im[oldest];
      residual_sum += static_cast<double>(d_re) * d_re + static_cast<double>(d_im) * d_im;
      ++residual_count;
    }
  }

  if (out.nof_pilot_re == 0) {
    return out;
  }
  out.epre_lin = static_cast<float>(epre_sum / static_cast<double>(out.nof_pilot_re));

  if (residual_count != 0) {
    out.nof_residuals = residual_count;
    out.noise_lin =
        static_cast<float>(residual_sum / static_cast<double>(residual_count)) / RESIDUAL_SECOND_DIFFERENCE;
  }

  // The estimator's definition, both sides in the grid's units: the signal power at the DM-RS resource elements
  // is `scaling^2` above the data REs', so the data power is `(epre - noise) / scaling^2`, times the number of
  // layers. Subtracting the noise is what keeps this an SNR rather than a signal-plus-noise-to-noise ratio, and
  // it is why the pilot power here is not simply `epre`.
  if (out.noise_lin > 0.0F) {
    const float signal_lin = out.epre_lin - out.noise_lin;
    out.snr_lin            = (signal_lin > 0.0F) ? (signal_lin * nof_layers / (scaling * scaling) / out.noise_lin)
                                                  : 0.0F;
  }

  // CFO from the first two symbols, pairing their k-th samples: the pilots of a symbol are on the same comb, so
  // the k-th sample of one symbol and the k-th of the next are the same subcarrier.
  if ((first_symbol != std::numeric_limits<unsigned>::max()) &&
      (second_symbol != std::numeric_limits<unsigned>::max()) && (first_count != 0) && (second_count != 0)) {
    const unsigned pairs = (first_count < second_count) ? first_count : second_count;
    std::complex<double> acc{0.0, 0.0};
    for (unsigned k = 0; k != pairs; ++k) {
      acc += second_ls[k] * std::conj(first_ls[k]);
    }
    const double delta_symbols = static_cast<double>(second_symbol) - static_cast<double>(first_symbol);
    if ((std::abs(acc) > 0.0) && (delta_symbols != 0.0)) {
      out.cfo_cycles_per_symbol =
          static_cast<float>(std::arg(acc) / (2.0 * M_PI * delta_symbols));
    }
  }
  return out;
}
