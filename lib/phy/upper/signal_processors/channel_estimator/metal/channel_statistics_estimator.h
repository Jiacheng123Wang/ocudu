// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Channel statistics provider for the 2D MMSE channel estimator.
///
/// The 2D MMSE estimator needs three channel statistics: the noise variance (sigma2), the
/// RMS delay spread (tau_rms, driving the frequency correlation) and the maximum Doppler
/// shift (f_d, driving the time correlation). In v1 the noise variance comes from the
/// existing classical noise estimator while tau_rms and f_d are FIXED configurable
/// constants; an estimation-backed implementation (PDP / Doppler estimation, see PLAN.md
/// §3.2) plugs in behind this same interface without touching the estimator.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h"
#include <cstdint>

namespace ocudu {

/// Channel statistics consumed by the 2D MMSE estimator.
struct channel_statistics {
  /// Noise variance (linear scale).
  float sigma2 = 0.0F;
  /// RMS delay spread in seconds.
  float tau_rms_s = 0.0F;
  /// Maximum Doppler shift in hertz.
  float fd_hz = 0.0F;
};

/// Inputs available to a channel statistics provider.
struct channel_statistics_input {
  /// Noise variance from the existing (classical) noise estimation (linear scale).
  float sigma2;
  /// Least-squares channel estimates at the DM-RS pilot REs, per DM-RS symbol, for one layer
  /// and one hop (symbol-major concatenation, no virtual pilots).
  span<const cf_t> pilots_lse;
  /// Number of pilots in a single OFDM symbol carrying DM-RS.
  unsigned nof_symbol_pilots;
  /// Number of OFDM symbols carrying DM-RS in the hop.
  unsigned nof_dmrs_symbols;
  /// Subcarrier spacing of the current transmission.
  subcarrier_spacing scs;
  /// Slot OFDM symbol index of the first DM-RS symbol in the hop.
  unsigned dmrs_symbol_0;
  /// Slot OFDM symbol index of the second DM-RS symbol in the hop (valid when
  /// \c nof_dmrs_symbols is 2).
  unsigned dmrs_symbol_1;
};

/// Channel statistics provider interface.
///
/// v1 implementation: \c channel_statistics_estimator_fixed (fixed constants).
/// v2 (planned): estimation-backed implementation (PDP / Doppler estimation).
class channel_statistics_estimator
{
public:
  virtual ~channel_statistics_estimator() = default;

  /// \brief Returns the channel statistics for the current estimation context.
  virtual channel_statistics estimate(const channel_statistics_input& input) const = 0;
};

/// Fixed-constants implementation (v1): tau_rms and f_d are configurable constants and the
/// noise variance is taken from the existing noise estimator.
class channel_statistics_estimator_fixed : public channel_statistics_estimator
{
public:
  /// \param[in] tau_rms_s_ RMS delay spread in seconds (fixed constant).
  /// \param[in] fd_hz_     Maximum Doppler shift in hertz (fixed constant).
  channel_statistics_estimator_fixed(float tau_rms_s_, float fd_hz_) : tau_rms_s(tau_rms_s_), fd_hz(fd_hz_) {}

  // See interface for documentation.
  channel_statistics estimate(const channel_statistics_input& input) const override
  {
    return channel_statistics{.sigma2 = input.sigma2, .tau_rms_s = tau_rms_s, .fd_hz = fd_hz};
  }

private:
  float tau_rms_s;
  float fd_hz;
};

} // namespace ocudu
