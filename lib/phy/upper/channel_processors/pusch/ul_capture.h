// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Debug capture of PUSCH receptions into files (OCUDU_UL_DUMP).
///
/// The capture writes, for the first OCUDU_UL_DUMP_COUNT (default 8) receptions carrying a
/// codeword, one file per stage of the receive chain:
///  - <prefix>_<slot>_<rnti>.txt / .bin   PDU configuration and the received resource grid,
///  - <prefix>_<slot>_<rnti>_ce.txt       channel estimator scalars (noise variance, SINR, ...),
///  - <prefix>_<slot>_<rnti>_llr.bin      demodulated soft bits (OCUDU_UL_DUMP_LLR).
///
/// The stages are the ones that separate the configurations worth comparing: the grid separates
/// the DFT and its pipelining, the estimator scalars separate the channel estimation, and the soft
/// bits separate equalization, demapping and the soft-bit scale.  The first file that differs
/// between two runs localizes the defect.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include "ocudu/ran/rnti.h"

namespace ocudu {
namespace ul_capture {

/// True when OCUDU_UL_DUMP is set (grid and channel estimator captures).
bool enabled();

/// True when OCUDU_UL_DUMP_LLR is set (soft-bit capture).
bool llr_enabled();

/// Captures the PDU configuration and the received grid of one PUSCH reception.
void capture_grid(const resource_grid_reader& grid, const pusch_processor::pdu_t& pdu);

/// Captures the channel estimator scalars of one PUSCH reception.
void capture_ce(const dmrs_pusch_estimator_results& est_results, const pusch_processor::pdu_t& pdu);

/// \brief Marks the reception the following captures belong to (the demodulator receives no PDU,
/// so the key is carried per thread).
void set_current(slot_point slot, rnti_t rnti);

/// Appends the soft bits of one codeblock to the current reception's file.
void capture_llr(span<const log_likelihood_ratio> llr);

} // namespace ul_capture
} // namespace ocudu
