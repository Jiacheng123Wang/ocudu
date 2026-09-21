// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief dft_processor adapter over the Metal GPU DFT engine (dft_processor_metal).

#pragma once

#include "ocudu_dft_metal_engine.h"
#include "ocudu/phy/generic_functions/dft_processor.h"
#include "ocudu/phy/generic_functions/dft_processor_grid_write.h"
#include "ocudu/support/macos_compat.h"
#include <cstdint>
#include <memory>

namespace ocudu {

/// DFT processor using the Metal GPU (iterative mixed-radix 2^k*3^m DIT, sizes up to
/// metal::dft_metal_engine::max_size: every NR OFDM FFT size of that family, e.g. 384/512/768/
/// 1024/1536/2048/3072). Input/output buffers are page-aligned and zero-copy
/// wrapped into MTLBuffers: the GPU reads/writes the host memory directly.
///
/// The buffers hold up to max_batch transforms and run_batch() executes them in a single
/// dispatch - one threadgroup (one GPU core) per transform - instead of paying a command
/// buffer round trip and a single-core transform per symbol.
///
/// The processor also implements dft_processor_grid_write: its engine can write a demodulated symbol straight into the
/// resource grid (the "FFT phase 2" kernel), in the same command buffer as the transform and bit-identically to the
/// host post-processing, so the OFDM demodulator can fill a device-addressable grid without the host touching the
/// transform output.
class dft_processor_metal : public dft_processor, public dft_processor_grid_write
{
public:
  /// Returns whether the Metal implementation supports the given size (2^k*3^m,
  /// 2..max_size). The factory falls back to the default DFT implementation otherwise.
  static bool is_supported_size(unsigned size);

  /// \brief Constructs a Metal DFT processor.
  /// \param[in] config DFT processor configuration parameters.
  explicit dft_processor_metal(const configuration& config);

  /// Returns whether the Metal engine initialized successfully (unsupported sizes and
  /// engine failures leave the processor invalid; the factory then falls back).
  bool is_valid() const { return valid; }

  // See interface for documentation.
  direction get_direction() const override { return dir; }

  // See interface for documentation.
  unsigned get_size() const override { return cfg.size; }

  // See interface for documentation.
  span<cf_t> get_input() override { return {input.get(), static_cast<size_t>(cfg.size) * max_batch}; }

  // See interface for documentation.
  span<const cf_t> run() override;

  /// Number of transforms executed by one run_batch() dispatch (one threadgroup each, so they
  /// run concurrently on different GPU cores). Covers a whole slot worth of OFDM symbols.
  ///
  /// \note Not used by the radio-paced RX OFDM demodulation, which dispatches one transform per
  /// symbol (see dft_processor::get_max_batch()). The batched entry point targets independent
  /// transform streams: the Rx ports of one symbol (no added latency) and the same symbol of
  /// several carriers / sectors. Multiple PUSCH allocations or UEs of one cell share one
  /// per-symbol transform, so they add no transform to batch.
  /// \todo Re-tune max_batch (and the buffer size) when the multi-port / multi-carrier batching
  ///       defines how many transforms are gathered per dispatch.
  static constexpr unsigned max_batch = 16;

  // See interface for documentation.
  unsigned get_max_batch() const override { return max_batch; }

  // See interface for documentation.
  span<const cf_t> run_batch(unsigned nof_transforms) override;

  // See interface for documentation.
  void run_async(unsigned slot) override
  {
    report_fatal_error_if_not(slot < max_batch, "Invalid Metal DFT slot {} (max {}).", slot, max_batch);
    (void)engine->submit_slot(input.get(), output.get(), slot);
  }

  // See interface for documentation.
  void wait() override
  {
    // The open block of transforms is part of "everything submitted": commit it before draining, or the
    // drain would leave it behind (wait_all() is static and cannot do it).
    if (engine != nullptr) {
      (void)engine->commit_open();
    }
    (void)metal::dft_metal_engine::wait_all();
  }

  bool begin_block() override { return (engine != nullptr) && engine->begin_block(); }

  bool end_block() override { return (engine != nullptr) && engine->commit_open(); }

  bool release_block(const void* grid_base) override
  {
    // The engine deposits the buffer under the grid it wrote (shared_burst::deposit_released()), which is
    // how the consumer - on another thread - finds it (see dft_metal_engine::release_block()).
    return (engine != nullptr) && (engine->release_block(grid_base) != nullptr);
  }

  void set_lane_slot(uint64_t slot_index) override
  {
    if (engine != nullptr) {
      engine->set_lane_slot(slot_index);
    }
  }

  // See interface for documentation.
  void wait_slot(unsigned slot) override { (void)engine->wait_slot(slot); }

  // See interface for documentation.
  span<const cf_t> get_output_batch() override
  {
    return {output.get(), static_cast<size_t>(cfg.size) * max_batch};
  }

  // See interface for documentation.
  dft_processor_grid_write* get_grid_write() override { return valid ? this : nullptr; }

  // See interface for documentation.
  bool supports_grid_write(const resource_grid_device_view& view) const override;

  // See interface for documentation.
  bool set_grid_write_window(span<const cf_t> window) override;

  // See interface for documentation.
  bool submit_grid_write(unsigned slot, const dft_grid_write_params& params) override;

  /// GPU-side duration of the last transform in microseconds (0 when unavailable).
  double engine_gpu_wait_us() const { return engine != nullptr ? engine->last_gpu_wait_us() : 0.0; }

private:
  struct aligned_free {
    void operator()(cf_t* p) const { compat::aligned_free(p); }
  };

  configuration                              cfg;
  direction                                  dir;
  std::unique_ptr<cf_t, aligned_free>        input;
  std::unique_ptr<cf_t, aligned_free>        output;
  std::unique_ptr<metal::dft_metal_engine>   engine;
  bool                                       valid = false;
};

} // namespace ocudu
