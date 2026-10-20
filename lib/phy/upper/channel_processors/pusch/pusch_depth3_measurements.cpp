// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_measurements.h"

#include <array>
#include "pusch_depth3_input_plane.h"
#include <cmath>
#include <limits>

using namespace ocudu;

namespace {

using plane = pusch_depth3_input_plane;

} // namespace

depth3_measurements ocudu::measure_depth3(span<const __fp16>       plane_values,
                                          unsigned                  plane_channels,
                                          span<const __fp16>        channel_head,
                                          float                     reference_amplitude,
                                          float                     noise_shape,
                                          float                     noise_scale_decades,
                                          float                     noise_scale_center,
                                          span<ul_pilot_sample>     scratch)
{
  depth3_measurements measurements;

  // The plane's own width, not the contract's constant. This guard is the one that actually bit: a conditioned
  // plane is 612*14*16 and the check demanded 612*14*12, so the measurement returned "no pilot" before reading
  // anything -- a message that says the CAPTURE has no DM-RS when the truth was that the reader was looking for
  // the wrong shape (measured 2026-10-16, on the conditioned contract).
  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane_channels;
  const size_t head_size = static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * 2;
  // AN EMPTY HEAD IS A LEGAL CALL: the plane-only part of this measurement is what the DOMAIN GATE needs, and
  // R7/S2 evaluates it BEFORE the forward pass, precisely so that an out-of-domain reception costs no inference
  // (design section 20.4). The head-derived fields are left at zero in that mode, which is why the finiteness
  // check below treats them as optional.
  const bool has_head = !channel_head.empty();
  if ((plane_values.size() != plane_size) || (has_head && (channel_head.size() != head_size))) {
    return measurements;
  }

  // The contract's model-external noise map, section 0.2(e) head 4. The map is the trainer's
  // (`depth3_loss.noise_from_shape`), and the pair comes from the sidecar so that the two sides cannot
  // disagree: `noise_shape` is bounded by the head's tanh, so this is always finite and positive.
  // Skipped entirely when there is no head to read, i.e. on the pre-forward gate path.
  const float noise_plane_units =
      has_head ? std::pow(10.0F, noise_scale_decades * noise_shape + noise_scale_center) : 0.0F;
  measurements.noise_variance_lin = reference_amplitude * reference_amplitude * noise_plane_units;

  // ---- what the NETWORK's heads and the plane's geometry are needed for: EPRE, RSRP, and the pilot stream ----
  // The residual, the noise and the CFO are NOT computed here: they come from `measure_ul_slow`, the one
  // implementation of that arithmetic, the one a network with a different plane contract calls, and the one
  // depth3_slow_measurements_test checks against a flat channel's closed form (design section 19.246).
  double   epre_sum     = 0.0;
  double   rsrp_sum     = 0.0;
  unsigned nof_pilot_re = 0;
  unsigned pilot_count  = 0;
  // The plane's copies of the DM-RS to data ratio and of the layer count. They are plane-wide constants written
  // into the scalar channels of every RE, so one read is enough; they are taken from the first pilot.
  float    scaling      = 1.0F;
  float    nof_layers   = 1.0F;
  bool     have_scalars = false;

  for (unsigned t = 0; t != plane::NOF_SYMBOLS; ++t) {
    for (unsigned k = 0; k != plane::NOF_SUBCARRIERS; ++k) {
      const __fp16* re = plane_values.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * plane_channels;

      // Only pilot resource elements are measured; the mask is per RE, not per symbol.
      if (re[plane::CH_DMRS_MASK] <= static_cast<__fp16>(0.0F)) {
        continue;
      }
      if (!have_scalars) {
        scaling      = static_cast<float>(re[plane::CH_SCALING]);
        nof_layers   = static_cast<float>(re[plane::CH_NOF_TX_LAYERS]);
        have_scalars = true;
      }

      // EPRE: the received pilot is `LS * sequence * scaling`, and the sequence has unit power, so the power is
      // `|LS * scaling|^2` -- with the reference amplitude put back, because the plane's LS is divided by it.
      const float ls_re = static_cast<float>(re[plane::CH_LS_RE]) * reference_amplitude * scaling;
      const float ls_im = static_cast<float>(re[plane::CH_LS_IM]) * reference_amplitude * scaling;
      epre_sum += static_cast<double>(ls_re) * ls_re + static_cast<double>(ls_im) * ls_im;

      // The shared layer's input: the same pilot in the GRID's units (the plane stores `LS/r`, so `r` puts it
      // back) and tagged with its symbol.
      if (pilot_count < scratch.size()) {
        scratch[pilot_count] = {.symbol = t,
                                .ls_re  = static_cast<float>(re[plane::CH_LS_RE]) * reference_amplitude,
                                .ls_im  = static_cast<float>(re[plane::CH_LS_IM]) * reference_amplitude};
        ++pilot_count;
      }

      // RSRP: head 3 carries `H/r` (contract section 0.2(d)), and this multiplication is the whole of the
      // "multiply back by r" the design asks for.
      if (has_head) {
        const __fp16* estimate = channel_head.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * 2;
        const float   h_re     = static_cast<float>(estimate[0]) * reference_amplitude;
        const float   h_im     = static_cast<float>(estimate[1]) * reference_amplitude;
        rsrp_sum += static_cast<double>(h_re) * h_re + static_cast<double>(h_im) * h_im;
      }

      ++nof_pilot_re;
    }
  }

  measurements.nof_pilot_re = nof_pilot_re;
  if (nof_pilot_re == 0) {
    // No pilot means the quantities are not defined, and a zero that says "not measured" must not be confusable
    // with a zero that says "measured as zero". The count is the caller's way to tell.
    measurements.noise_variance_lin = 0.0F;
    return measurements;
  }

  measurements.epre_lin = static_cast<float>(epre_sum / static_cast<double>(nof_pilot_re));

  // THE RESIDUAL, THE SNR AND THE CFO, from the shared layer.
  const ul_slow_measurements shared =
      measure_ul_slow(span<const ul_pilot_sample>(scratch.first(pilot_count)), scaling, nof_layers);
  measurements.residual_plane_units_lin    = shared.noise_lin;
  measurements.residual_noise_variance_lin = shared.noise_lin;
  measurements.residual_snr_lin            = shared.snr_lin;

  // THE DM-RS-TO-DATA RATIO BELONGS IN THE PUBLISHED RSRP, and it was missing: head 3 estimates the channel GAIN
  // (`H/r` times `r`), while RSRP is defined on the DM-RS RESOURCE ELEMENTS, whose transmit power is `scaling^2`
  // above the data REs'. Measured 2026-10-17 against the classical estimator on two independent receptions, the
  // published value was 3.11 and 2.96 dB low -- a factor of two, i.e. exactly this -- while EPRE, which already
  // multiplied by `scaling`, agreed to 0.01 dB. RSRP is one of the three quantities R6 hands the scheduler, so it
  // is published in the estimator's convention.
  const float rsrp_gain_lin = static_cast<float>(rsrp_sum / static_cast<double>(nof_pilot_re));
  measurements.rsrp_lin     = rsrp_gain_lin * scaling * scaling;

  // The estimator's own formula (`port_channel_estimator_average_impl.cpp:289`): the data RE power is the RSRP
  // scaled by the number of layers and by the INVERSE of the DM-RS-to-data amplitude ratio squared, and the SNR
  // is that over the noise variance. It is fed the GAIN, not the published value above, because `rsrp_gain_lin`
  // is already in the units this formula expects (it divides by `scaling^2` itself).
  const float data_rsrp = rsrp_gain_lin * nof_layers / (scaling * scaling);
  measurements.snr_lin  = (measurements.noise_variance_lin > 0.0F)
                              ? (data_rsrp / measurements.noise_variance_lin)
                              : 0.0F;

  if (!std::isfinite(measurements.epre_lin) || !std::isfinite(measurements.rsrp_lin) ||
      !std::isfinite(measurements.snr_lin) || !std::isfinite(measurements.noise_variance_lin)) {
    measurements = depth3_measurements{};
  }
  return measurements;
}

float ocudu::estimate_cfo_from_plane(span<const __fp16> plane_values, unsigned plane_channels)
{
  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane_channels;
  if (plane_values.size() != plane_size) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  // Which symbols carry DM-RS, from the mask channel. The first two are the pair the CPU chain uses: the
  // largest usable spacing is not the point (it would wrap earlier), the FIRST two are, because a channel that
  // drifts within the slot would otherwise fold the drift into the estimate.
  unsigned symbol_index[2] = {plane::NOF_SYMBOLS, plane::NOF_SYMBOLS};
  unsigned nof_dmrs        = 0;
  for (unsigned t = 0; t != plane::NOF_SYMBOLS; ++t) {
    const __fp16* first = plane_values.data() + static_cast<size_t>(t) * plane_channels;
    if (first[plane::CH_DMRS_MASK] > static_cast<__fp16>(0.0F)) {
      if (nof_dmrs < 2) {
        symbol_index[nof_dmrs] = t;
      }
      ++nof_dmrs;
    }
  }
  if (nof_dmrs < 2) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  const unsigned sym0       = symbol_index[0];
  const unsigned sym1       = symbol_index[1];
  const float    delta_syms = static_cast<float>(sym1 - sym0);

  // acc = sum over pilot REs of ls[sym1] * conj(ls[sym0]). CF (not double) is enough: the sum is over at most
  // a few thousand terms of bounded magnitude, and the argument is what matters, not the magnitude.
  float acc_re = 0.0F;
  float acc_im = 0.0F;
  for (unsigned k = 0; k != plane::NOF_SUBCARRIERS; ++k) {
    const __fp16* re0 = plane_values.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + sym0) * plane_channels;
    const __fp16* re1 = plane_values.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + sym1) * plane_channels;
    // Both symbols must carry a pilot at this subcarrier: the mask is per RE, not per symbol.
    if ((re0[plane::CH_DMRS_MASK] <= static_cast<__fp16>(0.0F)) ||
        (re1[plane::CH_DMRS_MASK] <= static_cast<__fp16>(0.0F))) {
      continue;
    }
    const float a_re = static_cast<float>(re1[plane::CH_LS_RE]);
    const float a_im = static_cast<float>(re1[plane::CH_LS_IM]);
    const float b_re = static_cast<float>(re0[plane::CH_LS_RE]);
    const float b_im = static_cast<float>(re0[plane::CH_LS_IM]);
    acc_re += a_re * b_re + a_im * b_im;   // ls1 * conj(ls0)
    acc_im += a_im * b_re - a_re * b_im;
  }
  if ((acc_re == 0.0F) && (acc_im == 0.0F)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  return std::atan2(acc_im, acc_re) / (2.0F * static_cast<float>(M_PI) * delta_syms);
}
