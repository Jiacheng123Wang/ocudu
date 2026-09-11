// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Unit tests of baseband_gateway_buffer_dynamic_aligned: the page-aligned baseband
/// buffer whose storage can be wrapped with MTLDevice::newBufferWithBytesNoCopy for the
/// zero-copy GPU FFT path.

#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/support/macos_compat.h"
#include <cstdint>
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

// Fills every sample of the buffer with a channel/sample-dependent pattern.
void fill_pattern(baseband_gateway_buffer_dynamic_aligned& buffer)
{
  for (unsigned i_channel = 0; i_channel != buffer.get_nof_channels(); ++i_channel) {
    span<ci16_t> channel = buffer[i_channel];
    for (unsigned i = 0; i != channel.size(); ++i) {
      const int16_t re = static_cast<int16_t>(((i_channel + 1) * (i + 1) * 7) % 30000);
      const int16_t im = static_cast<int16_t>(-(((i_channel + 1) * (i + 3) * 11) % 30000));
      channel[i]        = ci16_t(re, im);
    }
  }
}

// Verifies the pattern written by fill_pattern().
void verify_pattern(const baseband_gateway_buffer_dynamic_aligned& buffer)
{
  for (unsigned i_channel = 0; i_channel != buffer.get_nof_channels(); ++i_channel) {
    span<const ci16_t> channel = buffer[i_channel];
    for (unsigned i = 0; i != channel.size(); ++i) {
      const int16_t re = static_cast<int16_t>(((i_channel + 1) * (i + 1) * 7) % 30000);
      const int16_t im = static_cast<int16_t>(-(((i_channel + 1) * (i + 3) * 11) % 30000));
      ASSERT_EQ(channel[i].real(), re) << "channel " << i_channel << " sample " << i;
      ASSERT_EQ(channel[i].imag(), im) << "channel " << i_channel << " sample " << i;
    }
  }
}

} // namespace

TEST(baseband_gateway_buffer_dynamic_aligned_test, storage_is_page_aligned)
{
  baseband_gateway_buffer_dynamic_aligned buffer(2, 512);

  // Both halves of the newBufferWithBytesNoCopy contract: page-aligned base and a
  // page-multiple length.
  const auto page = compat::page_size();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(buffer.data()) % page, 0);
  EXPECT_EQ(buffer.capacity_bytes() % page, 0);
  // The whole logical contents must fit in the capacity.
  EXPECT_GE(buffer.capacity_bytes(), buffer.get_nof_samples() * buffer.get_nof_channels() * sizeof(ci16_t));
}

TEST(baseband_gateway_buffer_dynamic_aligned_test, writer_reader_roundtrip)
{
  baseband_gateway_buffer_dynamic_aligned buffer(2, 512);
  EXPECT_EQ(buffer.get_nof_channels(), 2);
  EXPECT_EQ(buffer.get_nof_samples(), 512);

  // Write through the writer interface (as baseband_gateway_receiver::receive does).
  baseband_gateway_buffer_writer& writer = buffer.get_writer();
  ASSERT_EQ(writer.get_nof_channels(), 2);
  ASSERT_EQ(writer.get_nof_samples(), 512);
  fill_pattern(buffer);

  // Read through the reader interface (as the uplink processor does).
  const baseband_gateway_buffer_reader& reader = buffer.get_reader();
  ASSERT_EQ(reader.get_nof_channels(), 2);
  ASSERT_EQ(reader.get_nof_samples(), 512);
  for (unsigned i_channel = 0; i_channel != 2; ++i_channel) {
    span<const ci16_t> channel = reader.get_channel_buffer(i_channel);
    ASSERT_EQ(channel.size(), 512);
  }
  verify_pattern(buffer);
}

TEST(baseband_gateway_buffer_dynamic_aligned_test, resize_never_reallocates)
{
  baseband_gateway_buffer_dynamic_aligned buffer(2, 512);
  const ci16_t* base = buffer.data();

  buffer.resize(128);
  EXPECT_EQ(buffer.get_nof_samples(), 128);
  EXPECT_EQ(buffer.data(), base); // Storage untouched: the zero-copy wrap stays valid.
  EXPECT_EQ(buffer.get_reader().get_nof_samples(), 128);
  EXPECT_EQ(buffer.get_writer().get_nof_samples(), 128);

  buffer.resize(512);
  EXPECT_EQ(buffer.get_nof_samples(), 512);

#if !defined(NDEBUG)
  // Out-of-range resizes are rejected by the assertion (debug builds only: the release
  // build compiles the assertion out, so the death test would never fire).
  EXPECT_DEATH(buffer.resize(0), "");
  EXPECT_DEATH(buffer.resize(513), "");
#endif
}

TEST(baseband_gateway_buffer_dynamic_aligned_test, move_rebinds_adapters)
{
  baseband_gateway_buffer_dynamic_aligned buffer(2, 256);
  fill_pattern(buffer);

  // The move must transfer the storage and re-bind the reader/writer to the new owner
  // (the baseband processor moves the buffer through the queue and into the task lambda).
  baseband_gateway_buffer_dynamic_aligned moved(std::move(buffer));
  EXPECT_EQ(moved.get_nof_channels(), 2);
  EXPECT_EQ(moved.get_nof_samples(), 256);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(moved.data()) % compat::page_size(), 0);
  verify_pattern(moved);
  EXPECT_EQ(moved.get_writer().get_nof_samples(), 256);

  // The moved-from object must not free the transferred storage.
  EXPECT_EQ(buffer.data(), nullptr);
}
