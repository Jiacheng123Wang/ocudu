// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_measurements.h"
#include "pusch_depth3_input_plane.h"
#include <cmath>

using namespace ocudu;

namespace {

using plane = pusch_depth3_input_plane;

} // namespace

depth3_measurements ocudu::measure_depth3(span<const __fp16> plane_values,
                                          span<const __fp16> channel_head,
                                          float               reference_amplitude,
                                          float               noise_shape,
                                          float               noise_scale_decades,
                                          float               noise_scale_center)
{
  depth3_measurements measurements;

  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane::NOF_CHANNELS;
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

  double   epre_sum = 0.0;
  double   rsrp_sum = 0.0;
  unsigned nof_pilot_re = 0;
  // Read from the plane rather than assumed: the contract's scalars are the same ones the packer wrote,
  // and a measurement that hard-codes them is a measurement that cannot notice a plane built differently.
  float scaling    = 1.0F;
  float nof_layers = 1.0F;

  for (unsigned k = 0; k != plane::NOF_SUBCARRIERS; ++k) {
    for (unsigned t = 0; t != plane::NOF_SYMBOLS; ++t) {
      const __fp16* re = plane_values.data() +
                         (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * plane::NOF_CHANNELS;
      // The plane's mask channel is set exactly at the allocated pilot resource elements of this port,
      // which is where both averages are defined. Everywhere else the LS channels are zero and head 3 is
      // unconstrained (training only constrains it inside the allocation), so walking the window instead
      // of the mask would average in values that mean nothing.
      if (re[plane::CH_DMRS_MASK] == static_cast<__fp16>(0.0F)) {
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
      const __fp16* estimate = channel_head.data() +
                               (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * 2;
      const float h_re = static_cast<float>(estimate[0]) * reference_amplitude;
      const float h_im = static_cast<float>(estimate[1]) * reference_amplitude;
      rsrp_sum += static_cast<double>(h_re) * h_re + static_cast<double>(h_im) * h_im;

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
  measurements.rsrp_lin = static_cast<float>(rsrp_sum / static_cast<double>(nof_pilot_re));

  // The estimator's own formula (`port_channel_estimator_average_impl.cpp:289`): the data RE power is the
  // RSRP scaled by the number of layers and by the INVERSE of the DM-RS-to-data amplitude ratio squared,
  // and the SNR is that over the noise variance.
  const float data_rsrp = measurements.rsrp_lin * nof_layers / (scaling * scaling);
  measurements.snr_lin  = (measurements.noise_variance_lin > 0.0F)
                              ? (data_rsrp / measurements.noise_variance_lin)
                              : 0.0F;

  if (!std::isfinite(measurements.epre_lin) || !std::isfinite(measurements.rsrp_lin) ||
      !std::isfinite(measurements.snr_lin) || !std::isfinite(measurements.noise_variance_lin)) {
    measurements = depth3_measurements{};
  }
  return measurements;
}
