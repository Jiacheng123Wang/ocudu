// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief The two properties behind the uplink processor's decision to skip the CFO pass.
///
/// uplink_processor_impl hands the assembled symbol to baseband_cfo_processor::process() only when
/// applies_compensation() is true, so a sector with no offset in effect passes the samples to the PRACH
/// and PUxCH processors exactly as the radio delivered them. Two properties make that safe:
///
///   1. the int16 -> float -> int16 scaling the ci16 path applies to every sample is the identity -
///      convert(ci16 -> cf_t, 1/32767) followed by convert(cf_t -> ci16, 32767) gives back the input for
///      every int16 - so the pass itself cannot change the samples, and
///   2. applies_compensation() is true exactly when process() modifies the samples, so a run that
///      skips the pass cannot silently drop a compensation that was due.
///
/// Both are tested here rather than argued: property 1 depends on the rounding of the platform's
/// SIMD conversion (vcvtnq_s32_f32 on Apple Silicon, _mm_cvtps_epi32 / _mm512_cvt_roundps_epi32 on
/// x86), and property 2 is the contract between the two functions.

#include "baseband_cfo_processor.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/ocuduvec/conversion.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <random>

using namespace ocudu;

namespace {

/// A single-channel buffer of \c nof_samples integer samples, all set to \p value.
baseband_gateway_buffer_dynamic make_buffer(unsigned nof_samples, ci16_t value)
{
  baseband_gateway_buffer_dynamic buffer(1, nof_samples);
  span<ci16_t>                    channel = buffer.get_writer().get_channel_buffer(0);
  std::fill(channel.begin(), channel.end(), value);
  return buffer;
}

/// Copies the samples of the first channel of \p buffer.
std::vector<ci16_t> read_channel(const baseband_gateway_buffer_dynamic& buffer)
{
  span<const ci16_t> channel = buffer.get_reader().get_channel_buffer(0);
  return {channel.begin(), channel.end()};
}

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

  // Fresh processor: nothing scheduled, so nothing to apply and nothing may change. The probe has to
  // be able to say that nothing asked for a compensation at all (see [ul_cfo]).
  ASSERT_FALSE(cfo.applies_compensation());
  ASSERT_EQ(cfo.get_nof_scheduled_commands(), 0) << "a fresh processor reports a command that was never scheduled";
  ASSERT_FLOAT_EQ(cfo.get_cfo_hz(), 0.0F);
  {
    baseband_gateway_buffer_dynamic samples = make_buffer(64, ci16_t(1000, -2000));
    const std::vector<ci16_t>       before  = read_channel(samples);
    cfo.process(samples.get_writer());
    ASSERT_EQ(read_channel(samples), before) << "process() modified the samples although it announced no compensation";
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
  ASSERT_EQ(cfo.get_nof_scheduled_commands(), 2) << "both accepted commands must be counted (0 Hz included)";
  ASSERT_FLOAT_EQ(cfo.get_cfo_hz(), cfo_hz);
  {
    const unsigned                  nof_samples = 64;
    baseband_gateway_buffer_dynamic samples     = make_buffer(nof_samples, ci16_t(32767, 0));
    cfo.process(samples.get_writer());

    const std::vector<ci16_t> rotated = read_channel(samples);
    for (unsigned i = 0; i != nof_samples; ++i) {
      // The first sample of the block is always rotated by exp(j*2*pi*cfo*0) = 1, the rest advance by the normalized
      // frequency. A full-scale int16 sample is what the ci16 <-> cf_t scaling maps 1.0 to, so the analytic result is
      // cos/sin scaled by 32767; the tolerance leaves room for the rounding of the SIMD rotation.
      const float phase = TWOPI * (cfo_hz / srate.to_Hz<float>()) * static_cast<float>(i);
      EXPECT_NEAR(rotated[i].real(), std::lround(std::cos(phase) * 32767.0F), 2) << "sample " << i;
      EXPECT_NEAR(rotated[i].imag(), std::lround(std::sin(phase) * 32767.0F), 2) << "sample " << i;
    }
  }
}
