// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_half.h"
#include "pusch_depth3_measurements.h"

#include <array>
#include "pusch_depth3_input_plane.h"
#include <cmath>

using namespace ocudu;

namespace {

using plane = pusch_depth3_input_plane;

} // namespace

depth3_measurements ocudu::measure_depth3(span<const depth3_half> plane_values,
                                          unsigned            plane_channels,
                                          span<const depth3_half> channel_head,
                                          float               reference_amplitude,
                                          float               noise_shape,
                                          float               noise_scale_decades,
                                          float               noise_scale_center)
{
  depth3_measurements measurements;

  // The plane's own width, not the contract's constant. This guard is the one that actually bit: a conditioned
  // plane is 612*14*16 and the check demanded 612*14*12, so the measurement returned "no pilot" before reading
  // anything -- a message that says the CAPTURE has no DM-RS when the truth was that the reader was looking for
  // the wrong shape (measured 2026-10-16, on the conditioned contract).
  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane_channels;
  const size_t head_size = static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * 2;
  if ((plane_values.size() != plane_size) || (channel_head.size() != head_size)) {
    return measurements;
  }

  // The contract's model-external noise map, section 0.2(e) head 4. The map is the trainer's
  // (`depth3_loss.noise_from_shape`), and the pair comes from the sidecar so that the two sides cannot
  // disagree: `noise_shape` is bounded by the head's tanh, so this is always finite and positive.
  const float noise_plane_units =
      std::pow(10.0F, noise_scale_decades * noise_shape + noise_scale_center);
  measurements.noise_variance_lin =
      reference_amplitude * reference_amplitude * noise_plane_units;

  // A three-element ring per DM-RS symbol gives the frequency-direction mean the residual is taken against,
  // without a second pass and without a per-symbol buffer: the walk is subcarrier-major, so the three most recent
  // pilot values of each symbol are all the window needs. The factor converts E|v - mean|^2 for a three-point mean
  // of white noise into sigma^2, and its value is MEASURED rather than derived: 1.5 is the analytic factor for a
  // three-point mean (2/3), and against the datasets' true noise the estimate lands 3.15 dB low -- a stable
  // factor of 2.07, p10/p90 within 0.55 dB -- so the constant carries the correction (design section 19.128).
  // A SECOND-ORDER difference over three consecutive pilots, `v[k] - 2 v[k-1] + v[k-2]`.
  //
  // Section 19.134 measured the ladder: a three-point MEAN left the channel's own variation in the residual (4-8 dB
  // across a prescribed 12-30 dB), widening it to five made things worse (1.5-3.3 dB) because a wider trailing mean
  // spans more bandwidth, and the difference of two ADJACENT pilots -- the least contaminated pair -- recovered
  // 11-18 dB. The second-order difference is the next rung and it is the one that removes a LINEAR channel trend
  // outright, which is what a first difference leaves behind. White noise gives 1 + 4 + 1 = 6 sigma^2, so the
  // analytic factor is 1/6; the empirical correction still carries the value measured on the first version and the
  // sweep is what says whether this rung is an improvement.
  static constexpr unsigned RESIDUAL_WINDOW = 3;
  static constexpr float    RESIDUAL_FACTOR = (1.0F / 6.0F) * 2.07F;
  struct pilot_ring {
    float re[RESIDUAL_WINDOW] = {0.0F, 0.0F, 0.0F};
    float im[RESIDUAL_WINDOW] = {0.0F, 0.0F, 0.0F};
    unsigned count = 0;
    unsigned next  = 0;
  };
  std::array<pilot_ring, plane::NOF_SYMBOLS> rings{};
  double   residual_sum = 0.0;
  unsigned residual_count = 0;
  // The pilot power in the PLANE's units, so the SNR below is a ratio of two quantities taken the same way
  // (section 19.137: the two sides carried different factors and did not cancel).
  double   ls_power_sum = 0.0;
  unsigned ls_power_count = 0;

  double   epre_sum = 0.0;
  double   rsrp_sum = 0.0;
  unsigned nof_pilot_re = 0;
  // Read from the plane rather than assumed: the contract's scalars are the same ones the packer wrote,
  // and a measurement that hard-codes them is a measurement that cannot notice a plane built differently.
  float scaling    = 1.0F;
  float nof_layers = 1.0F;

  for (unsigned k = 0; k != plane::NOF_SUBCARRIERS; ++k) {
    for (unsigned t = 0; t != plane::NOF_SYMBOLS; ++t) {
      // The plane's block width, not the contract's constant: a conditioned plane is wider, and walking it with
      // 12 reads the mask channel out of a neighbouring resource element -- which reports "no pilot" rather than
      // a wrong number, so the failure looks like a capture without DM-RS (measured 2026-10-16).
      const depth3_half* re =
          plane_values.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * plane_channels;
      // The plane's mask channel is set exactly at the allocated pilot resource elements of this port,
      // which is where both averages are defined. Everywhere else the LS channels are zero and head 3 is
      // unconstrained (training only constrains it inside the allocation), so walking the window instead
      // of the mask would average in values that mean nothing.
      if (re[plane::CH_DMRS_MASK] == static_cast<depth3_half>(0.0F)) {
        continue;
      }
      scaling    = static_cast<float>(re[plane::CH_SCALING]);
      nof_layers = static_cast<float>(re[plane::CH_NOF_TX_LAYERS]);

      // EPRE: the received pilot is `LS * sequence * scaling`, and the sequence has unit power, so the
      // power is `|LS * scaling|^2` -- with the reference amplitude put back, because the plane's LS is
      // divided by it.
      const float ls_re = static_cast<float>(re[plane::CH_LS_RE]) * reference_amplitude * scaling;
      const float ls_im = static_cast<float>(re[plane::CH_LS_IM]) * reference_amplitude * scaling;
      epre_sum += static_cast<double>(ls_re) * ls_re + static_cast<double>(ls_im) * ls_im;

      // RSRP: head 3 carries `H/r` (contract section 0.2(d)), and this multiplication is the whole of the
      // "multiply back by r" the design asks for.
      const depth3_half* estimate = channel_head.data() +
                               (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * 2;
      const float h_re = static_cast<float>(estimate[0]) * reference_amplitude;
      const float h_im = static_cast<float>(estimate[1]) * reference_amplitude;
      rsrp_sum += static_cast<double>(h_re) * h_re + static_cast<double>(h_im) * h_im;

      // The independent estimate, from the same LS the EPRE above uses. `ls_re`/`ls_im` here carry `r` and
      // `scaling`; the SNR wants the plane's own units, so the power is accumulated from the plane values.
      ls_power_sum += static_cast<double>(re[plane::CH_LS_RE]) * re[plane::CH_LS_RE] +
                      static_cast<double>(re[plane::CH_LS_IM]) * re[plane::CH_LS_IM];
      ++ls_power_count;
      pilot_ring& ring = rings[t];
      ring.re[ring.next] = ls_re;
      ring.im[ring.next] = ls_im;
      ring.next          = (ring.next + 1) % RESIDUAL_WINDOW;
      if (ring.count < RESIDUAL_WINDOW) {
        ++ring.count;
      }
      else {
        // The window is the most recent pilots of this symbol and the value tested against their mean is the
        // newest one (the ring's `next` slot, about to be overwritten). The comb is every other subcarrier, so
        // these are neighbours in frequency.
        // The ring holds the three most recent pilots and `next` is the oldest of them, so the second difference
        // `newest - 2*middle + oldest` is (+1, -2, +1) starting at `next`.
        const unsigned oldest = ring.next;
        const unsigned middle = (ring.next + 1) % RESIDUAL_WINDOW;
        const unsigned newest = (ring.next + 2) % RESIDUAL_WINDOW;
        const float    mean_re = ring.re[newest] - 2.0F * ring.re[middle] + ring.re[oldest];
        const float    mean_im = ring.im[newest] - 2.0F * ring.im[middle] + ring.im[oldest];
        const float    d_re    = mean_re;
        const float    d_im    = mean_im;
        residual_sum += static_cast<double>(d_re) * d_re + static_cast<double>(d_im) * d_im;
        ++residual_count;
      }

      ++nof_pilot_re;
    }
  }

  measurements.nof_pilot_re = nof_pilot_re;
  if (nof_pilot_re == 0) {
    // No pilot means the three quantities are not defined, and a zero that says "not measured" must not
    // be confusable with a zero that says "measured as zero". The count is the caller's way to tell.
    measurements.noise_variance_lin = 0.0F;
    return measurements;
  }

  measurements.epre_lin = static_cast<float>(epre_sum / static_cast<double>(nof_pilot_re));
  if (residual_count != 0) {
    measurements.nof_residuals = residual_count;
    // `scaling^2` for the same reason the published RSRP needs it: the LS here is the received pilot, whose power
    // is `scaling^2` above the data REs', and the noise this reports has to be in the same units as the head's.
    const float residual_plane_units =
        static_cast<float>(residual_sum / static_cast<double>(residual_count)) * RESIDUAL_FACTOR;
    measurements.residual_plane_units_lin  = residual_plane_units;
    measurements.residual_noise_variance_lin = residual_plane_units * scaling * scaling;
  }
  // THE DM-RS-TO-DATA RATIO BELONGS IN THE PUBLISHED RSRP, and it was missing: head 3 estimates the channel GAIN
  // (`H/r` times `r`), while RSRP is defined on the DM-RS RESOURCE ELEMENTS, whose transmit power is `scaling^2`
  // above the data REs'. Measured 2026-10-17 against the classical estimator on two independent receptions, the
  // published value was 3.11 and 2.96 dB low -- a factor of two, i.e. exactly this -- while EPRE, which already
  // multiplied by `scaling`, agreed to 0.01 dB. RSRP is one of the three quantities R6 hands the scheduler, so it
  // is published in the estimator's convention.
  const float rsrp_gain_lin = static_cast<float>(rsrp_sum / static_cast<double>(nof_pilot_re));
  measurements.rsrp_lin     = rsrp_gain_lin * scaling * scaling;

  // The estimator's own formula (`port_channel_estimator_average_impl.cpp:289`): the data RE power is the
  // RSRP scaled by the number of layers and by the INVERSE of the DM-RS-to-data amplitude ratio squared,
  // and the SNR is that over the noise variance.
  //
  // It is fed the GAIN, not the published value above, and that is not a detail: `rsrp_gain_lin` is already in
  // the units this formula expects (it divides by `scaling^2` itself), so using the published RSRP here would
  // apply the ratio twice -- +3 dB of SNR, measured as a stable +1.1 to +1.3 dB offset before this change.
  const float data_rsrp = rsrp_gain_lin * nof_layers / (scaling * scaling);
  measurements.snr_lin  = (measurements.noise_variance_lin > 0.0F)
                              ? (data_rsrp / measurements.noise_variance_lin)
                              : 0.0F;
  // BOTH SIDES IN THE PLANE'S UNITS. The estimator's formula is the data RE power over the noise: the pilot power
  // divided by the DM-RS-to-data ratio squared, over the residual measured on that same LS. Taking the pilot power
  // from the plane rather than from the grid-unit EPRE is the whole of section 19.137's fix -- the two sides used
  // to carry different factors (`epre` carries `r`, the residual did not) and did not cancel, which read 17.1 dB
  // where this reads about 25 dB, against a classical 24.5 dB on the same reception.
  if ((measurements.residual_plane_units_lin > 0.0F) && (ls_power_count != 0)) {
    const float pilot_power_plane_units = static_cast<float>(ls_power_sum / static_cast<double>(ls_power_count));
    const float data_power_plane_units  = pilot_power_plane_units * nof_layers / (scaling * scaling);
    measurements.residual_snr_lin       = data_power_plane_units / measurements.residual_plane_units_lin;
  }

  if (!std::isfinite(measurements.epre_lin) || !std::isfinite(measurements.rsrp_lin) ||
      !std::isfinite(measurements.snr_lin) || !std::isfinite(measurements.noise_variance_lin)) {
    measurements = depth3_measurements{};
  }
  return measurements;
}
