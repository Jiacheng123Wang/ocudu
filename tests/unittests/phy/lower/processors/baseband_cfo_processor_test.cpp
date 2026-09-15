// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief The two properties that let the uplink processor skip its CFO pass.
///
/// uplink_processor_impl wraps the compensation in an int16 -> float -> int16 round trip and only
/// runs it when baseband_cfo_processor::applies_compensation() is true. With no offset in effect the
/// pass is therefore a pure round trip, and skipping it is correct only if
///
///   1. the round trip is the identity - convert(ci16 -> cf_t, 1/32767) followed by
///      convert(cf_t -> ci16, 32767) gives back the input for every int16, and
///   2. applies_compensation() is true exactly when process() modifies the samples, so a run that
///      skips the pass cannot silently drop a compensation that was due.
///
/// Both are tested here rather than argued: property 1 depends on the rounding of the platform's
/// SIMD conversion (vcvtnq_s32_f32 on Apple Silicon, _mm_cvtps_epi32 / _mm512_cvt_roundps_epi32 on
/// x86), and property 2 is the contract between the two functions.

#include "ocudu/ocuduvec/conversion.h"
#include "baseband_cfo_processor.h"
#include <gtest/gtest.h>
#include <random>

using namespace ocudu;

namespace {

/// All the values an int16 sample can take, plus the extremes first: the identity has to hold at
/// -32768 (which the float mapping of a full-scale int16 reaches exactly) as well as at 0.
std::vector<ci16_t> all_int16_values()
{
  std::vector<ci16_t> values;
  values.reserve(65536);
  values.emplace_back(static_cast<int16_t>(-32768), static_cast<int16_t>(-32768));
  values.emplace_back(static_cast<int16_t>(32767), static_cast<int16_t>(32767));
  values.emplace_back(static_cast<int16_t>(-32768), static_cast<int16_t>(32767));
  for (int32_t re = -32768; re != 32768; ++re) {
    values.emplace_back(static_cast<int16_t>(re), static_cast<int16_t>(-re - 1));
  }
  return values;
}

} // namespace

TEST(BasebandCfoProcessorTest, TheInt16RoundTripIsTheIdentity)
{
  const std::vector<ci16_t> samples = all_int16_values();

  std::vector<cf_t>  as_float(samples.size());
  std::vector<ci16_t> back(samples.size());

  // Exactly what uplink_processor_impl does around the compensation.
  ocuduvec::convert(span<cf_t>(as_float), span<const ci16_t>(samples), ocuduvec::scaling_factor_ci16_to_cf);
  ocuduvec::convert(span<ci16_t>(back), span<const cf_t>(as_float), ocuduvec::scaling_factor_cf_to_ci16);

  for (size_t i = 0; i != samples.size(); ++i) {
    ASSERT_EQ(back[i], samples[i]) << "the round trip changed sample " << i << " (" << samples[i].real() << ","
                                   << samples[i].imag() << ")";
  }
}

TEST(BasebandCfoProcessorTest, CompensationIsAppliedExactlyWhenItIsAnnounced)
{
  const sampling_rate srate = sampling_rate::from_MHz(7.68);
  baseband_cfo_processor cfo(srate);

  // Fresh processor: nothing scheduled, so nothing to apply and nothing may change.
  ASSERT_FALSE(cfo.applies_compensation());
  {
    std::vector<cf_t>  samples(64, cf_t(0.25F, -0.5F));
    std::vector<cf_t>  before = samples;
    cfo.process(span<cf_t>(samples));
    ASSERT_EQ(samples, before) << "process() modified the samples although it announced no compensation";
  }

  // A command of 0 Hz leaves no offset in effect either.
  ASSERT_TRUE(cfo.schedule_cfo_command(std::chrono::system_clock::now(), 0.0F));
  cfo.next_cfo_command();
  ASSERT_FALSE(cfo.applies_compensation());

  // A non-zero command is announced and applied.
  const float cfo_hz = 100.0F;
  ASSERT_TRUE(cfo.schedule_cfo_command(std::chrono::system_clock::now(), cfo_hz));
  cfo.next_cfo_command();
  ASSERT_TRUE(cfo.applies_compensation());
  {
    std::vector<cf_t> samples(64);
    std::vector<cf_t> expected(64);
    for (size_t i = 0; i != samples.size(); ++i) {
      // First sample of the block is always rotated by exp(j*2*pi*cfo*0) = 1, the rest advance.
      float phase = TWOPI * (cfo_hz / srate.to_Hz<float>()) * static_cast<float>(i);
      samples[i]  = cf_t(0.0F, 0.0F);
      expected[i] = cf_t(std::cos(phase), std::sin(phase));
    }
    std::vector<cf_t> input(64, cf_t(1.0F, 0.0F));
    cfo.process(span<cf_t>(input));
    for (size_t i = 0; i != input.size(); ++i) {
      ASSERT_NEAR(input[i].real(), expected[i].real(), 1e-5F);
      ASSERT_NEAR(input[i].imag(), expected[i].imag(), 1e-5F);
    }
  }
}
