// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which are subject to additional licensing requirements.

#pragma once

#include "ocudu/phy/generic_functions/dft_processor.h"
#include <vector>

namespace ocudu {

/// \brief DFT processor backed by Apple's vDSP (Accelerate framework).
///
/// WHY IT EXISTS (dev doc 6.231). The in-tree chain (FFTW -> AOCL-FFTZ -> generic) ends in a hand-written
/// mixed-radix implementation that the compiler vectorises with NEON, and the tree has never used
/// Accelerate: `grep -rn Accelerate lib cmake` was empty before this file. Measured on this machine for
/// the 768-point transform the RX front end runs (23.04 Msps / 30 kHz), the in-tree generic path costs
/// 2.88 us and vDSP_DFT costs 1.54 us (1.87x), and vDSP's FFT is executed by Apple's matrix units, which
/// a pure NEON loop cannot reach (arXiv 2609.32237: 203 GFLOPS single-threaded, and it does NOT scale
/// with thread count).
///
/// WHAT IT IS NOT: a latency lever. Under the per-symbol receive policy the front end computes each
/// symbol as it arrives, so thirteen of the fourteen transforms of a slot are hidden behind the 35.7 us
/// symbol cadence and only the last one is exposed (user ruling, dev doc 6.229 (2)); the expected gain
/// is therefore ~1.3 us per hop, not 14 x 1.34 us. It is adopted because it is not a regression, not
/// because it is worth much.
///
/// CONVENTIONS, matched to dft_processor_generic_impl: both directions are UNNORMALISED (no 1/N), and
/// DIRECT is the negative-exponent transform. vDSP_DFT is unnormalised in both directions as well, so
/// the two are interchangeable without a scale factor.
///
/// SIZE COVERAGE: vDSP_DFT accepts lengths of the form f * 2^n with f in {1, 3, 5, 15}. The OFDM sizes
/// this tree uses include 18432 = 9 * 2^11 (the odd part 9 is not one of them), so the factory falls back
/// per size instead of refusing the configuration.
class dft_processor_vdsp : public dft_processor
{
public:
  /// \brief Determines whether the vDSP DFT covers \c size, i.e. whether it is 2^n, 3*2^n, 5*2^n or 15*2^n.
  static bool is_supported_size(unsigned size);

  /// \brief Constructs a vDSP-backed DFT processor.
  /// \param[in] dft_config Provides the DFT processor parameters.
  explicit dft_processor_vdsp(const configuration& dft_config);

  ~dft_processor_vdsp() override;

  /// Determines whether the initialization was successful.
  bool is_valid() const { return setup != nullptr; }

  // See interface for documentation.
  direction get_direction() const override { return dir; }

  // See interface for documentation.
  unsigned get_size() const override { return size; }

  // See interface for documentation.
  span<cf_t> get_input() override { return input; }

  // See interface for documentation.
  span<const cf_t> run() override;

private:
  /// Stores the DFT direction.
  direction dir;
  /// Stores the DFT size.
  unsigned size;
  /// DFT input buffer ownership.
  std::vector<cf_t> input;
  /// DFT output buffer ownership.
  std::vector<cf_t> output;
  /// vDSP_DFT_Interleaved_Setup, held as an opaque pointer so that this header does not require Accelerate.
  void* setup = nullptr;
};

} // namespace ocudu
