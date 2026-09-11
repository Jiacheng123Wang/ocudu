// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief dft_processor adapter over the Metal GPU DFT engine (dft_processor_metal).

#pragma once

#include "ocudu_dft_metal_engine.h"
#include "ocudu/phy/generic_functions/dft_processor.h"
#include "ocudu/support/macos_compat.h"
#include <memory>

namespace ocudu {

/// DFT processor using the Metal GPU (iterative radix-2 DIT, power-of-two sizes up to
/// metal::dft_metal_engine::max_size). Input/output buffers are page-aligned and zero-copy
/// wrapped into MTLBuffers: the GPU reads/writes the host memory directly.
class dft_processor_metal : public dft_processor
{
public:
  /// Returns whether the Metal implementation supports the given size (power of two,
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
  span<cf_t> get_input() override { return {input.get(), cfg.size}; }

  // See interface for documentation.
  span<const cf_t> run() override;

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
