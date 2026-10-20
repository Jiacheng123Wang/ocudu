// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "lib/phy/upper/channel_processors/pusch/pusch_depth3_input_plane.h"
#include "lib/phy/upper/channel_processors/pusch/pusch_depth3_measurements.h"
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;

namespace {

/// Planes are fp16, so a recovered CFO cannot be asserted to more digits than fp16 holds in the LS values.
constexpr float CFO_TOLERANCE = 2.0e-4F;

/// Builds a plane whose pilot LS rotates by `cfo` cycles per symbol, and whose data REs carry nothing.
std::vector<__fp16> make_plane(std::initializer_list<unsigned> dmrs_symbols, float cfo, float amplitude = 0.5F)
{
  constexpr unsigned nof_channels = pusch_depth3_input_plane::NOF_CHANNELS;
  std::vector<__fp16> plane(static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) *
                                pusch_depth3_input_plane::NOF_SYMBOLS * nof_channels,
                            static_cast<__fp16>(0.0F));
  for (unsigned k = 0; k != pusch_depth3_input_plane::NOF_SUBCARRIERS; ++k) {
    // Every sixth subcarrier carries a pilot, like DM-RS type 1 of one CDM group.
    if ((k % 6) != 0) {
      continue;
    }
    for (unsigned sym : dmrs_symbols) {
      __fp16* re = plane.data() +
                   (static_cast<size_t>(k) * pusch_depth3_input_plane::NOF_SYMBOLS + sym) * nof_channels;
      const float phase = 2.0F * static_cast<float>(M_PI) * cfo * static_cast<float>(sym);
      re[pusch_depth3_input_plane::CH_LS_RE]   = static_cast<__fp16>(amplitude * std::cos(phase));
      re[pusch_depth3_input_plane::CH_LS_IM]   = static_cast<__fp16>(amplitude * std::sin(phase));
      re[pusch_depth3_input_plane::CH_DMRS_MASK] = static_cast<__fp16>(1.0F);
    }
  }
  return plane;
}

} // namespace

TEST(Depth3Cfo, recovers_a_known_rotation)
{
  for (float expected : {0.0F, 0.001F, -0.001F, 0.005F, -0.02F}) {
    std::vector<__fp16> plane = make_plane({2, 11}, expected);
    const float         cfo =
        estimate_cfo_from_plane(span<const __fp16>(plane), pusch_depth3_input_plane::NOF_CHANNELS);
    EXPECT_FALSE(std::isnan(cfo)) << "expected " << expected;
    EXPECT_NEAR(cfo, expected, CFO_TOLERANCE) << "expected " << expected;
  }
}

TEST(Depth3Cfo, uses_the_first_two_dmrs_symbols)
{
  // The pair is 2 and 7 here, so the recovered value is the rotation over five symbols, not nine: an
  // estimator that took the LAST pair would return the same number for a RATE that is linear in the symbol
  // index, but would differ once the rotation is not exactly linear in the pair spacing -- and, more
  // importantly, the CPU chain uses the first pair and the two must agree.
  std::vector<__fp16> plane = make_plane({2, 7, 11}, 0.004F);
  const float         cfo =
      estimate_cfo_from_plane(span<const __fp16>(plane), pusch_depth3_input_plane::NOF_CHANNELS);
  EXPECT_NEAR(cfo, 0.004F, CFO_TOLERANCE);
}

TEST(Depth3Cfo, refuses_a_plane_with_one_dmrs_symbol)
{
  std::vector<__fp16> plane = make_plane({2}, 0.01F);
  // The helper above writes the same symbol twice, which is one DM-RS symbol: the estimator has no spacing
  // to divide by and must say so rather than invent one.
  const float cfo = estimate_cfo_from_plane(span<const __fp16>(plane), pusch_depth3_input_plane::NOF_CHANNELS);
  EXPECT_TRUE(std::isnan(cfo));
}

TEST(Depth3Cfo, refuses_a_plane_of_the_wrong_size)
{
  std::vector<__fp16> plane(16, static_cast<__fp16>(0.0F));
  EXPECT_TRUE(std::isnan(estimate_cfo_from_plane(span<const __fp16>(plane), 12)));
}
