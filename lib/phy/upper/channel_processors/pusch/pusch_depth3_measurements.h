// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/span.h"
#include "pusch_ul_slow_measurements.h"
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

  /// \brief Noise variance from the plane's own DM-RS least-squares values, in the SAME units as
  /// \ref noise_variance_lin, and estimated WITHOUT the network and WITHOUT the classical estimator.
  ///
  /// Why it exists (design sections 19.127, 19.128): R6 has to know when its own measurements may be trusted, and
  /// a ruler taken from the network's head 4 would be the network judging itself. This one subtracts a three-point
  /// mean from the pilot LS along frequency and scales the residual: measured offline against the datasets' true
  /// noise, it correlates at +0.999 in the log domain with a p10/p90 spread of 0.23 dB, against +0.593 and +-7 dB
  /// for head 4.
  float residual_noise_variance_lin = 0.0F;

  /// The same estimate BEFORE the `r` and `scaling` conversions, i.e. in the PLANE's own units -- which is what
  /// the training datasets store as `sigma2_over_r2`, and therefore the only form in which the implementation can
  /// be checked against the truth it was designed on (design section 19.129's first follow-up).
  float residual_plane_units_lin = 0.0F;

  /// \brief SNR built ENTIRELY from the plane: the pilot LS for the signal and the residual for the noise.
  ///
  /// It borrows nothing from the classical estimator and nothing from the network's heads, which is what a gate
  /// that has to judge both needs. Its convention is its own, so it can only replace the estimator's SNR once the
  /// declared training range is expressed in the same one -- which is why it is MEASURED against the estimator's
  /// SNR first (design sections 19.127, 19.132).
  float residual_snr_lin = 0.0F;

  /// How many pilot residuals the estimate above averages over; zero means it was not computed.
  unsigned nof_residuals = 0;
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
/// \param[in] plane               The contract's plane, fp16, as `assemble()` wrote it: `[612, 14, plane_channels]`.
/// \param[in] plane_channels      The plane's block width: the contract's 12, plus the one-hot conditioning
///                                channels when a MODULATION-CONDITIONED bundle is bound. It is a parameter and not
///                                the constant because every consumer of the plane has to walk it with the width it
///                                was written with -- a hard-coded 12 reads the mask channel from the wrong element
///                                and the measurement silently finds no pilot at all (measured 2026-10-16, the
///                                fourth consumer of this plane to make the same mistake).
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
/// \brief Carrier frequency offset from the PLANE's own DM-RS least-squares values, in CYCLES PER SYMBOL.
///
/// Why it exists (design section 19.239): the arm has no CFO compensation at all, while the classical chain
/// estimates it from the pilots, rotates each symbol by it before combining, and even carries the previous
/// slot's value forward. Anything that pools receivers across time -- and R7's gate does -- needs that phase
/// removed first: DM-RS sequences differ per slot, LS removes them, but the channel's phase still drifts.
///
/// The estimator is the traditional one, and deliberately the same one the CPU chain uses
/// (port_channel_estimator_average_impl::preprocess_pilots_and_estimate_cfo) so the two can be compared
/// numerically: over the plane's pilot resource elements, `acc = sum ls[sym1] * conj(ls[sym0])` between the
/// FIRST TWO DM-RS symbols, then `cfo = arg(acc) / (2*pi*(sym1 - sym0))`. The plane carries the LS at the pilot
/// REs and the mask that says which symbols carry them, so this needs neither the grid nor the network.
///
/// \param[in] plane           The contract's plane, fp16, as `assemble()` wrote it.
/// \param[in] plane_channels  The plane's block width (12, plus the one-hot channels of a conditioned bundle).
/// \return The CFO in cycles per symbol, or NaN when the plane carries fewer than two DM-RS symbols.
float estimate_cfo_from_plane(span<const __fp16> plane, unsigned plane_channels);

/// \param[in,out] scratch  Room for one pilot sample per resource element, so the hot path allocates nothing.
///                          The caller owns it and it may be reused between calls; its size decides how many pilots
///                          can be measured, and a plane that carries more is measured up to that many.
depth3_measurements measure_depth3(span<const __fp16>       plane,
                                   unsigned                  plane_channels,
                                   span<const __fp16>        channel_head,
                                   float                     reference_amplitude,
                                   float                     noise_shape,
                                   float                     noise_scale_decades,
                                   float                     noise_scale_center,
                                   span<ul_pilot_sample>     scratch);

} // namespace ocudu
