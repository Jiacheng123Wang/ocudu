// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <optional>

namespace ocudu {

/// PUSCH demodulator interface to notify the demodulation statistics of a codeword.
class pusch_demodulator_notifier
{
public:
  /// Collects PUSCH demodulation statistics.
  struct demodulation_stats {
    /// Estimated Signal-to-Interference-plus-Noise Ratio (SINR) at the output of the equalizer.
    std::optional<float> sinr_dB;
    /// Measured EVM.
    std::optional<float> evm;
    /// \brief Reference signal received power, in dB, when the DEMODULATOR measured it itself.
    ///
    /// R6: the AI arm's reporting heads measure RSRP and EPRE on the same reception, in the window before
    /// `on_end_codeword()` that the arm already uses to publish its SINR -- which is the only window in which
    /// anything can reach the scheduler's channel state, because `estimate()` completes before `demodulate()` and
    /// the heads do not exist until the forward pass has run (design section 19.123).
    ///
    /// EMPTY ON THE CLASSICAL PATH, and that is the contract: a demodulator that did not measure these leaves
    /// them unset, and the estimator's own values stay in force. A value here means the number came from the
    /// demodulator, in the estimator's units (section 19.122 brought the two to within 0.04 dB).
    std::optional<float> rsrp_dB;
    /// \brief Energy per resource element, in dB, when the DEMODULATOR measured it itself. See \ref rsrp_dB.
    std::optional<float> epre_dB;
  };

  /// Default destructor.
  virtual ~pusch_demodulator_notifier() = default;

  /// \brief Notifies intermediate PUSCH demodulator statistics.
  /// \param i_symbol OFDM symbol index within the slot.
  /// \param stats    OFDM symbol statistics.
  virtual void on_provisional_stats(unsigned i_symbol, const demodulation_stats& stats) = 0;

  /// Notifies the end of PUSCH processing and the final demodulator statistics.
  virtual void on_end_stats(const demodulation_stats& stats) = 0;
};

} // namespace ocudu
