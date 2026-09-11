// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/adt/tensor.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_writer.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/ocudu_assert.h"

namespace ocudu {
namespace detail {

enum class baseband_gateway_buffer_dims : unsigned { sample = 0, channel, all };

/// Implements a baseband gateway buffer reader based on a tensor.
class baseband_gateway_buffer_reader_tensor : public baseband_gateway_buffer_reader
{
public:
  using storage_type = tensor<static_cast<unsigned>(detail::baseband_gateway_buffer_dims::all),
                              ci16_t,
                              detail::baseband_gateway_buffer_dims>;

  /// Creates a gateway buffer reader based on a tensor storage type.
  explicit baseband_gateway_buffer_reader_tensor(const storage_type& data_) : data(data_) {}

  // See interface for documentation.
  unsigned get_nof_channels() const override { return data.get_dimension_size(baseband_gateway_buffer_dims::channel); }

  // See interface for documentation.
  unsigned get_nof_samples() const override { return data.get_dimension_size(baseband_gateway_buffer_dims::sample); }

  // See interface for documentation.
  span<const ci16_t> get_channel_buffer(unsigned i_channel) const override { return data.get_view({i_channel}); }

private:
  const storage_type& data;
};

/// Implements a baseband gateway buffer writer based on a tensor.
class baseband_gateway_buffer_writer_tensor : public baseband_gateway_buffer_writer
{
public:
  using storage_type = tensor<static_cast<unsigned>(detail::baseband_gateway_buffer_dims::all),
                              ci16_t,
                              detail::baseband_gateway_buffer_dims>;

  /// Creates a gateway buffer writer based on a tensor storage type.
  explicit baseband_gateway_buffer_writer_tensor(storage_type& data_) : data(data_) {}

  // See interface for documentation.
  unsigned get_nof_channels() const override { return data.get_dimension_size(baseband_gateway_buffer_dims::channel); }

  // See interface for documentation.
  unsigned get_nof_samples() const override { return data.get_dimension_size(baseband_gateway_buffer_dims::sample); }

  // See interface for documentation.
  span<ci16_t> get_channel_buffer(unsigned i_channel) override { return data.get_view({i_channel}); }

private:
  storage_type& data;
};

} // namespace detail

/// \brief Describes a baseband buffer implementation that comprises a fix number of channels that can be dynamically
/// resized.
///
/// It contains a fixed get_nof_channels() number of channels that contain the same number of get_nof_samples() samples.
/// The number of samples can be changed at runtime without re-allocating memory using resize(). The number samples
/// shall never exceed the maximum number of samples indicated in the constructor.
class baseband_gateway_buffer_dynamic
{
public:
  /// Gets the number of channels.
  unsigned get_nof_channels() const { return data.get_dimension_size(detail::baseband_gateway_buffer_dims::channel); }

  /// Gets the current number of samples.
  unsigned get_nof_samples() const { return data.get_dimension_size(detail::baseband_gateway_buffer_dims::sample); }

  /// Gets the reader interface.
  const baseband_gateway_buffer_reader& get_reader() const { return reader; }

  /// Gets the writer interface.
  baseband_gateway_buffer_writer& get_writer() { return writer; }

  /// Gets a data view of a channel.
  span<ci16_t> operator[](unsigned i_channel) { return writer.get_channel_buffer(i_channel); }

  /// \brief Resize buffer.
  /// \param[in] new_nof_samples Indicates the new number of samples per channel.
  /// \note The new number of samples must be greater than 0 and must not exceed the maximum number of samples.
  void resize(unsigned new_nof_samples) { data.resize({new_nof_samples, get_nof_channels()}); }

  /// \brief Default constructor.
  /// \param[in] nof_channels    Indicates the number of channels to create.
  /// \param[in] max_nof_samples Indicates the maximum number of samples.
  baseband_gateway_buffer_dynamic(unsigned nof_channels, unsigned max_nof_samples) :
    data({max_nof_samples, nof_channels}), reader(data), writer(data)
  {
  }

  /// Move constructor.
  baseband_gateway_buffer_dynamic(baseband_gateway_buffer_dynamic&& other) noexcept :
    data(std::move(other.data)), reader(data), writer(data)
  {
  }

private:
  dynamic_tensor<static_cast<unsigned>(detail::baseband_gateway_buffer_dims::all),
                 ci16_t,
                 detail::baseband_gateway_buffer_dims>
                                                data;
  detail::baseband_gateway_buffer_reader_tensor reader;
  detail::baseband_gateway_buffer_writer_tensor writer;
};

namespace detail {

/// Reader adapter of baseband_gateway_buffer_dynamic_aligned: views into the owner's page-aligned
/// storage. Holds a pointer to the owner's current-sample counter so that resize() is observed.
class baseband_gateway_buffer_reader_aligned : public baseband_gateway_buffer_reader
{
public:
  baseband_gateway_buffer_reader_aligned() = default;

  baseband_gateway_buffer_reader_aligned(const ci16_t* base_, unsigned channels_, const unsigned* samples_) :
    base(base_), channels(channels_), samples(samples_)
  {
  }

  unsigned get_nof_channels() const override { return channels; }
  unsigned get_nof_samples() const override { return *samples; }
  span<const ci16_t> get_channel_buffer(unsigned i_channel) const override
  {
    return {base + static_cast<std::size_t>(i_channel) * (*samples), *samples};
  }

private:
  const ci16_t*   base = nullptr;
  unsigned        channels = 0;
  const unsigned* samples = nullptr;
};

/// Writer adapter of baseband_gateway_buffer_dynamic_aligned (same view semantics as the reader).
class baseband_gateway_buffer_writer_aligned : public baseband_gateway_buffer_writer
{
public:
  baseband_gateway_buffer_writer_aligned() = default;

  baseband_gateway_buffer_writer_aligned(ci16_t* base_, unsigned channels_, const unsigned* samples_) :
    base(base_), channels(channels_), samples(samples_)
  {
  }

  unsigned get_nof_channels() const override { return channels; }
  unsigned get_nof_samples() const override { return *samples; }
  span<ci16_t> get_channel_buffer(unsigned i_channel) override
  {
    return {base + static_cast<std::size_t>(i_channel) * (*samples), *samples};
  }

private:
  ci16_t*         base = nullptr;
  unsigned        channels = 0;
  const unsigned* samples = nullptr;
};

} // namespace detail

/// \brief Page-aligned variant of baseband_gateway_buffer_dynamic (S-0 prerequisite).
///
/// Identical channel/sample semantics, but the storage is a single page-aligned, page-multiple
/// block allocated through compat::aligned_alloc(compat::page_size(), ...): the buffer can
/// therefore be wrapped with MTLDevice::newBufferWithBytesNoCopy
/// (MTLResourceStorageModeShared) and consumed by the GPU zero-copy (branch-one FFT path).
/// resize() never reallocates: the logical sample count only moves within the construction-time
/// maximum, so the zero-copy wrap (created once over the whole capacity) stays valid.
///
/// Move-only: the reader/writer adapters are re-bound to the new owner on move, mirroring
/// baseband_gateway_buffer_dynamic.
class baseband_gateway_buffer_dynamic_aligned
{
public:
  /// Creates a buffer with \c nof_channels channels and capacity for \c max_nof_samples samples
  /// per channel.
  baseband_gateway_buffer_dynamic_aligned(unsigned nof_channels, unsigned max_nof_samples)
  {
    ocudu_assert(nof_channels != 0, "Invalid number of channels.");
    ocudu_assert(max_nof_samples != 0, "Invalid maximum number of samples.");
    channels    = nof_channels;
    max_samples = max_nof_samples;
    cur_samples = max_nof_samples;
    // Round the byte length up to a page multiple: the zero-copy wrap contract (page-aligned
    // base + page-multiple length) then holds by construction. compat::aligned_alloc rounds
    // up again internally, which is idempotent.
    const std::size_t raw_bytes = static_cast<std::size_t>(max_nof_samples) * nof_channels * sizeof(ci16_t);
    const std::size_t page      = compat::page_size();
    storage_bytes               = (raw_bytes + page - 1) & ~(page - 1);
    // compat::aligned_alloc(page, ...) returns a page-aligned block covering at least
    // storage_bytes: both halves of the newBufferWithBytesNoCopy contract hold without any
    // extra rounding in the callers.
    void* mem = compat::aligned_alloc(page, storage_bytes);
    ocudu_assert(mem != nullptr, "Aligned baseband buffer allocation failed.");
    storage = static_cast<ci16_t*>(mem);
    reader  = detail::baseband_gateway_buffer_reader_aligned(storage, channels, &cur_samples);
    writer  = detail::baseband_gateway_buffer_writer_aligned(storage, channels, &cur_samples);
  }

  ~baseband_gateway_buffer_dynamic_aligned()
  {
    if (storage != nullptr) {
      compat::aligned_free(storage);
    }
  }

  baseband_gateway_buffer_dynamic_aligned(const baseband_gateway_buffer_dynamic_aligned&) = delete;
  baseband_gateway_buffer_dynamic_aligned&
  operator=(const baseband_gateway_buffer_dynamic_aligned&) = delete;

  baseband_gateway_buffer_dynamic_aligned(baseband_gateway_buffer_dynamic_aligned&& other) noexcept :
    channels(other.channels),
    max_samples(other.max_samples),
    cur_samples(other.cur_samples),
    storage(other.storage),
    storage_bytes(other.storage_bytes),
    reader(storage, channels, &cur_samples),
    writer(storage, channels, &cur_samples)
  {
    other.storage = nullptr;
  }

  baseband_gateway_buffer_dynamic_aligned&
  operator=(baseband_gateway_buffer_dynamic_aligned&& other) noexcept
  {
    if (this != &other) {
      if (storage != nullptr) {
        compat::aligned_free(storage);
      }
      channels      = other.channels;
      max_samples   = other.max_samples;
      cur_samples   = other.cur_samples;
      storage       = other.storage;
      storage_bytes = other.storage_bytes;
      other.storage = nullptr;
      reader        = detail::baseband_gateway_buffer_reader_aligned(storage, channels, &cur_samples);
      writer        = detail::baseband_gateway_buffer_writer_aligned(storage, channels, &cur_samples);
    }
    return *this;
  }

  /// Gets the number of channels.
  unsigned get_nof_channels() const { return channels; }

  /// Gets the current number of samples per channel.
  unsigned get_nof_samples() const { return cur_samples; }

  /// \brief Resize buffer.
  /// \param[in] new_nof_samples Indicates the new number of samples per channel.
  /// \note The new number of samples must be greater than 0 and must not exceed the maximum
  /// number of samples (the storage is never reallocated: the zero-copy wrap stays valid).
  void resize(unsigned new_nof_samples)
  {
    ocudu_assert(new_nof_samples != 0 && new_nof_samples <= max_samples,
                 "Invalid number of samples ({}), maximum is {}.",
                 new_nof_samples,
                 max_samples);
    cur_samples = new_nof_samples;
  }

  /// Gets a data view of a channel.
  span<ci16_t> operator[](unsigned i_channel) { return writer.get_channel_buffer(i_channel); }

  /// Gets a read-only data view of a channel.
  span<const ci16_t> operator[](unsigned i_channel) const
  {
    return reader.get_channel_buffer(i_channel);
  }

  /// Gets the writer adapter.
  baseband_gateway_buffer_writer& get_writer() { return writer; }

  /// Gets the reader adapter.
  baseband_gateway_buffer_reader& get_reader() { return reader; }

  /// Page-aligned base pointer of the storage (zero-copy wrap source).
  ci16_t* data() { return storage; }
  const ci16_t* data() const { return storage; }

  /// Byte length of the whole storage block: a page multiple (the zero-copy wrap length).
  std::size_t capacity_bytes() const { return storage_bytes; }

private:
  unsigned                                   channels;
  unsigned                                   max_samples;
  unsigned                                   cur_samples;
  ci16_t*                                    storage = nullptr;
  std::size_t                                storage_bytes = 0;
  detail::baseband_gateway_buffer_reader_aligned reader;
  detail::baseband_gateway_buffer_writer_aligned writer;
};

} // namespace ocudu
