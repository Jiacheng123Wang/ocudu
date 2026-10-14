// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/span.h"
#include <cstdint>

namespace ocudu {

/// \brief The chain's measurements, computed from the network's own reporting heads.
///
/// The names are the receiver's, not the contract's: `model_network_design.md` section 0.2(e) says what
/// each head reports and which measurement kernel consumes it, and this is where head 3 and head 4 are
/// actually turned into what a scheduler reads.
struct depth3_measurements {
  /// Average received power at the DM-RS resource elements, linear scale (`get_epre()`).
  float epre_lin = 0.0F;
  /// Average power of the channel estimate at the DM-RS resource elements, linear scale (`get_rsrp()`).
  float rsrp_lin = 0.0F;
  /// Signal to noise ratio, linear scale (`get_snr()`).
  float snr_lin = 0.0F;
  /// Noise variance in the grid's units, linear scale (`get_noise_variance()`).
  float noise_variance_lin = 0.0F;
  /// DM-RS resource elements the averages were taken over. Zero means the reception was not measurable,
  /// and then every value above is zero rather than a fabricated one.
  unsigned nof_pilot_re = 0;
};

/// \brief Turns heads 3 and 4 into the quantities `dmrs_pusch_estimator_results` publishes.
///
/// ---- Why this is a unit of its own ----
/// The contract keeps the head -> quantity map OUTSIDE the graph (section 0.2(f)), which is what makes it
/// calibratable without retraining -- and it also means the map has exactly one implementation per side, the
/// way `depth3.llr_from_logits` is the one implementation of the LLR map on the training side. A map written
/// twice is a map that can disagree with itself, and the disagreement would show up as a scheduler reading a
/// channel quality that no receiver measured.
///
/// ---- What it reads, and why only that ----
/// Everything comes from the plane the arm already assembled plus the two reporting heads, so the
/// measurement costs one pass over the pilot resource elements and no second look at the grid:
///
/// - EPRE is the average received power at the DM-RS REs. The plane carries the pilot least-squares
///   estimate divided by the reference amplitude (channel 5/6) and the DM-RS-to-data scaling (channel 11),
///   so the received pilot is `LS * sequence * scaling` and its power is `|LS|^2 * scaling^2`, because the
///   sequence is unit power. The reference amplitude is multiplied back, which is what turns the plane's
///   units into the grid's.
/// - RSRP is the average power of the channel estimate at the same REs. Head 3 carries `H/r` (contract
///   section 0.2(d)), so it is multiplied back by `r` for the same reason and in the same place.
/// - The noise variance is head 4 through the contract's model-external map. Head 4 is trained against the
///   variance in the PLANE's units, so the published value is `r^2` times it.
/// - The SNR is the estimator's own definition, `rsrp_avg * nof_layers / scaling^2 / noise_variance`, read
///   from the plane's scalar channels rather than assumed, so the two sides cannot disagree about the
///   direction the DM-RS-to-data ratio points.
///
/// \param[in] plane               The contract's plane, `[612, 14, 12]` fp16, as `assemble()` wrote it.
/// \param[in] channel_head        Head 3, `[612, 14, 2]` fp16, carrying `Re/Im(H/r)` per RE.
/// \param[in] reference_amplitude The `r` `assemble()` reported for the same plane.
/// \param[in] noise_shape         Head 4's scalar output, the `u` of the noise map.
/// \param[in] noise_scale_decades The map's `A_d` (`depth3_loss.NOISE_SCALE_DECADES`).
/// \param[in] noise_scale_center  The map's `B_d` (`depth3_loss.NOISE_SCALE_CENTER`).
/// \return The measurements, with \c nof_pilot_re equal to zero when the plane or the head does not have
///         the contract's shape, or when the allocation carries no pilot at all.
///
/// \note Time alignment has no head. The contract lists TA among the consumers of head 3, but a network
/// that estimates a channel per resource element has no time-domain observable to report it from, so TA
/// keeps coming from the classical estimator and this unit deliberately does not invent one.
depth3_measurements measure_depth3(span<const __fp16> plane,
                                   span<const __fp16> channel_head,
                                   float               reference_amplitude,
                                   float               noise_shape,
                                   float               noise_scale_decades,
                                   float               noise_scale_center);

} // namespace ocudu
