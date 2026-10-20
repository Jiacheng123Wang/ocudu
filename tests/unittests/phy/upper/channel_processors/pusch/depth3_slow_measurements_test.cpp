// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "lib/phy/upper/channel_processors/pusch/pusch_ul_slow_measurements.h"
#include <cmath>
#include <gtest/gtest.h>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

/// Pilots of a flat channel with white noise: the case whose answer is known in closed form, which is the whole
/// point of this file -- the unit bookkeeping and the residual's correction factor cannot be settled by reading
/// the expression, only by asking it for a number that was chosen in advance.
struct scenario {
  float channel_power;    // |H|^2
  float noise_variance;   // sigma^2 per complex sample
  float cfo_cycles;       // cycles per symbol
  float scaling;          // DM-RS to data amplitude ratio
  unsigned nof_layers;
};

std::vector<ul_pilot_sample> make_pilots(const scenario& s, unsigned nof_subcarriers, unsigned nof_symbols)
{
  std::mt19937                          rng(20261020);
  std::normal_distribution<float>       gaussian(0.0F, std::sqrt(s.noise_variance / 2.0F));
  std::vector<ul_pilot_sample>          pilots;
  pilots.reserve(nof_subcarriers * nof_symbols);
  const float amplitude = std::sqrt(s.channel_power);
  for (unsigned sym = 0; sym != nof_symbols; ++sym) {
    // Two DM-RS symbols, two apart in the stream; the symbol INDEX is what the CFO divides by, so it is set to
    // the real positions of a normal-CP pattern.
    const unsigned symbol_index = (sym == 0) ? 2U : 11U;
    const float    phase        = 2.0F * static_cast<float>(M_PI) * s.cfo_cycles * static_cast<float>(symbol_index);
    for (unsigned k = 0; k != nof_subcarriers; ++k) {
      pilots.push_back({.symbol = symbol_index,
                        .ls_re  = amplitude * std::cos(phase) + gaussian(rng),
                        .ls_im  = amplitude * std::sin(phase) + gaussian(rng)});
    }
  }
  return pilots;
}

} // namespace

TEST(UlSlowMeasurements, epre_is_the_received_power_at_the_pilot_resource_elements)
{
  const scenario              s{.channel_power = 1.0F, .noise_variance = 0.1F, .cfo_cycles = 0.0F,
                                .scaling = 1.0F, .nof_layers = 1};
  std::vector<ul_pilot_sample> pilots = make_pilots(s, 512, 2);
  const ul_slow_measurements   m      = measure_ul_slow(pilots, s.scaling, s.nof_layers);
  // EPRE is signal plus noise, which is what arrives at the resource elements.
  EXPECT_NEAR(m.epre_lin, s.channel_power + s.noise_variance, 0.02F);
}

TEST(UlSlowMeasurements, noise_is_recovered_from_the_pilots_alone)
{
  const scenario              s{.channel_power = 1.0F, .noise_variance = 0.01F, .cfo_cycles = 0.0F,
                                .scaling = 1.0F, .nof_layers = 1};
  std::vector<ul_pilot_sample> pilots = make_pilots(s, 2048, 2);
  const ul_slow_measurements   m      = measure_ul_slow(pilots, s.scaling, s.nof_layers);

  // THE NUMBER THIS TEST EXISTS FOR. A second difference over three consecutive pilots of a FLAT channel leaves
  // 1 + 4 + 1 = 6 sigma^2, so the estimate must come out at sigma^2. It does not if the correction factor still
  // carries the 2.07 measured for the three-point MEAN this estimator used before the second difference replaced
  // it: that factor corrects a bias the second difference does not have, and applying it anyway reports the noise
  // 3.15 dB high, which is an SNR 3.15 dB LOW.
  EXPECT_NEAR(10.0F * std::log10(m.noise_lin / s.noise_variance), 0.0F, 0.5F)
      << "measured " << m.noise_lin << " against a true " << s.noise_variance;
}

TEST(UlSlowMeasurements, snr_is_the_data_power_over_the_noise)
{
  const scenario s{.channel_power = 1.0F, .noise_variance = 0.01F, .cfo_cycles = 0.0F, .scaling = 1.0F,
                   .nof_layers = 2};
  std::vector<ul_pilot_sample> pilots = make_pilots(s, 2048, 2);
  const ul_slow_measurements   m      = measure_ul_slow(pilots, s.scaling, s.nof_layers);

  // 1/0.01 = 20 dB, times two layers, with the DM-RS to data ratio at one.
  const float expected_db = 10.0F * std::log10(s.channel_power * s.nof_layers / s.noise_variance);
  EXPECT_NEAR(10.0F * std::log10(m.snr_lin), expected_db, 0.7F);
}

TEST(UlSlowMeasurements, snr_follows_the_dmrs_to_data_ratio)
{
  const scenario s{.channel_power = 1.0F, .noise_variance = 0.01F, .cfo_cycles = 0.0F, .scaling = 0.65F,
                   .nof_layers = 1};
  std::vector<ul_pilot_sample> pilots = make_pilots(s, 2048, 2);
  const ul_slow_measurements   m      = measure_ul_slow(pilots, s.scaling, s.nof_layers);

  // The pilot REs are `scaling^2` stronger than the data ones, so the data SNR is |H|^2 / (scaling^2 sigma^2).
  const float expected_db = 10.0F * std::log10(s.channel_power / (s.scaling * s.scaling * s.noise_variance));
  EXPECT_NEAR(10.0F * std::log10(m.snr_lin), expected_db, 0.7F);
}

TEST(UlSlowMeasurements, cfo_is_recovered)
{
  for (float expected : {0.0F, 0.001F, -0.004F}) {
    const scenario              s{.channel_power = 1.0F, .noise_variance = 0.01F, .cfo_cycles = expected,
                                  .scaling = 1.0F, .nof_layers = 1};
    std::vector<ul_pilot_sample> pilots = make_pilots(s, 1024, 2);
    const ul_slow_measurements   m      = measure_ul_slow(pilots, s.scaling, s.nof_layers);
    EXPECT_FALSE(std::isnan(m.cfo_cycles_per_symbol));
    EXPECT_NEAR(m.cfo_cycles_per_symbol, expected, 3.0e-4F) << "expected " << expected;
  }
}

TEST(UlSlowMeasurements, reports_nothing_for_an_empty_input)
{
  const ul_slow_measurements m = measure_ul_slow(span<const ul_pilot_sample>(), 1.0F, 1.0F);
  EXPECT_EQ(m.nof_pilot_re, 0U);
  EXPECT_EQ(m.nof_residuals, 0U);
  EXPECT_EQ(m.snr_lin, 0.0F);
}
