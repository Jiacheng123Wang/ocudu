// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/support/math/stats.h"
#include <chrono>
#include <optional>
#include <string>

namespace ocudu {

/// PUSCH decoding statistics.
struct pusch_decoder_result {
  /// Denotes whether the received transport block passed the CRC.
  bool tb_crc_ok = false;
  /// Total number of codeblocks in the current codeword.
  unsigned nof_codeblocks_total = 0;
  /// \brief LDPC decoding statistics.
  ///
  /// Provides access to LDPC decoding statistics such as the number of decoded codeblocks (via
  /// <tt>ldpc_stats->get_nof_observations()</tt>) or the average number of iterations for correctly decoded
  /// codeblocks (via <tt>ldpc_stats->get_mean()</tt>).
  sample_statistics<unsigned> ldpc_decoder_stats;
  /// \brief LDPC decoder implementation type, as configured (e.g. "auto", "neon", "metal").
  ///
  /// Empty when the decoding statistics do not carry a type (e.g. empty or hardware-accelerated decoders).
  std::string ldpc_decoder_type;
  /// \brief Wall-clock time spent in the LDPC decode block.
  ///
  /// Measured from the first codeblock decode invocation to the completion of the last one. It covers the whole
  /// decoding attempt, whether it converged to a valid CRC or ran until the maximum number of iterations.
  std::chrono::nanoseconds ldpc_decode_elapsed{0};
  /// \brief Metal library total call duration of the LDPC decode block, when the decoder runs on a Metal GPU.
  ///
  /// Sum of the Metal GPU-side durations reported by each codeblock decode call (each call is one Metal command
  /// buffer carrying all the decoding iterations). Nullopt for decoders without a Metal backend (auto/neon/
  /// generic/...): the measurement is not applicable and the log prints "na".
  std::optional<std::chrono::nanoseconds> ldpc_metal_elapsed;
  /// \brief Size in bytes of the uncoded payload of the decoded code block(s), i.e. the MAC PDU (transport block)
  /// size.
  unsigned mac_pdu_bytes = 0;
  /// \brief RX phase segment: time-frequency transform (FFT) elapsed time of this PUSCH, from the IQ samples
  /// received to the whole-slot frequency-domain symbols ready.
  ///
  /// Assembled by the UL pipeline probe from the per-slot timestamps; nullopt when not available (e.g. the decode
  /// was skipped because the codeblock CRC was already OK from a previous transmission).
  std::optional<std::chrono::nanoseconds> t2f_elapsed;
  /// \brief RX phase segment: channel estimation elapsed time, from the whole-slot frequency-domain symbols to
  /// the channel estimates of all the data symbols ready. Nullopt semantics as t2f_elapsed.
  std::optional<std::chrono::nanoseconds> ce_elapsed;
  /// \brief RX phase segment: equalization + demodulation elapsed time, from the data-symbol channel estimates
  /// ready to the per-bit LLRs ready (start of the LDPC decode). Nullopt semantics as t2f_elapsed.
  std::optional<std::chrono::nanoseconds> eqdem_elapsed;
};

} // namespace ocudu
