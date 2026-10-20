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

depth3_plane_measurements ocudu::measure_depth3_plane(span<const __fp16>   plane_values,
                                                       unsigned             plane_channels,
                                                       float                reference_amplitude,
                                                       span<ul_pilot_sample> scratch)
{
  depth3_plane_measurements out;

  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane_channels;
  if (plane_values.size() != plane_size) {
    return out;
  }

  // EPRE accumulates over the pilot REs; the pilot stream is what the shared layer consumes.
  double   epre_sum     = 0.0;
  unsigned nof_pilot_re = 0;
  unsigned pilot_count  = 0;
  bool     have_scalars = false;

  for (unsigned t = 0; t != plane::NOF_SYMBOLS; ++t) {
    for (unsigned k = 0; k != plane::NOF_SUBCARRIERS; ++k) {
      const __fp16* re = plane_values.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * plane_channels;

      if (re[plane::CH_DMRS_MASK] <= static_cast<__fp16>(0.0F)) {
        continue;
      }
      if (!have_scalars) {
        out.scaling    = static_cast<float>(re[plane::CH_SCALING]);
        out.nof_layers = static_cast<float>(re[plane::CH_NOF_TX_LAYERS]);
        have_scalars   = true;
      }

      const float ls_re = static_cast<float>(re[plane::CH_LS_RE]) * reference_amplitude * out.scaling;
      const float ls_im = static_cast<float>(re[plane::CH_LS_IM]) * reference_amplitude * out.scaling;
      epre_sum += static_cast<double>(ls_re) * ls_re + static_cast<double>(ls_im) * ls_im;

      if (pilot_count < scratch.size()) {
        scratch[pilot_count] = {.symbol = t,
                                .ls_re  = static_cast<float>(re[plane::CH_LS_RE]) * reference_amplitude,
                                .ls_im  = static_cast<float>(re[plane::CH_LS_IM]) * reference_amplitude};
        ++pilot_count;
      }
      ++nof_pilot_re;
    }
  }

  out.nof_pilot_re = nof_pilot_re;
  if (nof_pilot_re == 0) {
    return out;
  }
  out.epre_lin = static_cast<float>(epre_sum / static_cast<double>(nof_pilot_re));

  const ul_slow_measurements shared =
      measure_ul_slow(span<const ul_pilot_sample>(scratch.first(pilot_count)), out.scaling, out.nof_layers);
  out.noise_lin      = shared.noise_lin;
  out.snr_lin        = shared.snr_lin;
  out.nof_residuals  = shared.nof_residuals;
  out.cfo_cycles     = estimate_cfo_from_plane(plane_values, plane_channels);
  return out;
}

depth3_measurements ocudu::measure_depth3_heads(const depth3_plane_measurements& plane_half,
                                                span<const __fp16>               plane_values,
                                                unsigned                         plane_channels,
                                                span<const __fp16>               channel_head,
                                                float                            reference_amplitude,
                                                float                            noise_shape,
                                                float                            noise_scale_decades,
                                                float                            noise_scale_center)
{
  depth3_measurements measurements;

  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane_channels;
  const size_t head_size = static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * 2;
  if ((plane_values.size() != plane_size) || (channel_head.size() != head_size)) {
    return measurements;
  }
  if (plane_half.nof_pilot_re == 0) {
    return measurements;
  }

  // The plane half, carried over: the publication must judge on the number the gate judged on (19.263).
  measurements.nof_pilot_re               = plane_half.nof_pilot_re;
  measurements.epre_lin                   = plane_half.epre_lin;
  measurements.residual_plane_units_lin   = plane_half.noise_lin;
  measurements.residual_noise_variance_lin = plane_half.noise_lin;
  measurements.residual_snr_lin           = plane_half.snr_lin;
  measurements.nof_residuals              = plane_half.nof_residuals;

  // The contract's model-external noise map, section 0.2(e) head 4.
  const float noise_plane_units =
      std::pow(10.0F, noise_scale_decades * noise_shape + noise_scale_center);
  measurements.noise_variance_lin = reference_amplitude * reference_amplitude * noise_plane_units;

  // RSRP: head 3 carries `H/r` (contract section 0.2(d)).
  double rsrp_sum = 0.0;
  for (unsigned t = 0; t != plane::NOF_SYMBOLS; ++t) {
    for (unsigned k = 0; k != plane::NOF_SUBCARRIERS; ++k) {
      const __fp16* re = plane_values.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * plane_channels;
      if (re[plane::CH_DMRS_MASK] <= static_cast<__fp16>(0.0F)) {
        continue;
      }
      const __fp16* estimate = channel_head.data() + (static_cast<size_t>(k) * plane::NOF_SYMBOLS + t) * 2;
      const float   h_re     = static_cast<float>(estimate[0]) * reference_amplitude;
      const float   h_im     = static_cast<float>(estimate[1]) * reference_amplitude;
      rsrp_sum += static_cast<double>(h_re) * h_re + static_cast<double>(h_im) * h_im;
    }
  }

  const float scaling    = plane_half.scaling;
  const float nof_layers = plane_half.nof_layers;
  const float rsrp_gain_lin = static_cast<float>(rsrp_sum / static_cast<double>(plane_half.nof_pilot_re));
  measurements.rsrp_lin     = rsrp_gain_lin * scaling * scaling;

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

depth3_measurements ocudu::measure_depth3(span<const __fp16>   plane_values,
                                          unsigned             plane_channels,
                                          span<const __fp16>   channel_head,
                                          float                reference_amplitude,
                                          float                noise_shape,
                                          float                noise_scale_decades,
                                          float                noise_scale_center,
                                          span<ul_pilot_sample> scratch)
{
  // The composition, kept so existing callers and tests keep their one call: the plane half first, then the
  // heads on top of it (design section 19.263).
  depth3_measurements measurements;

  const size_t plane_size =
      static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * plane_channels;
  const size_t head_size = static_cast<size_t>(plane::NOF_SUBCARRIERS) * plane::NOF_SYMBOLS * 2;
  const bool   has_head  = !channel_head.empty();
  if ((plane_values.size() != plane_size) || (has_head && (channel_head.size() != head_size))) {
    return measurements;
  }

  const depth3_plane_measurements plane_half =
      measure_depth3_plane(plane_values, plane_channels, reference_amplitude, scratch);
  if (!has_head) {
    // The pre-forward gate path: the plane half is the whole answer.
    measurements.nof_pilot_re                = plane_half.nof_pilot_re;
    measurements.epre_lin                    = plane_half.epre_lin;
    measurements.residual_plane_units_lin    = plane_half.noise_lin;
    measurements.residual_noise_variance_lin = plane_half.noise_lin;
    measurements.residual_snr_lin            = plane_half.snr_lin;
    measurements.nof_residuals               = plane_half.nof_residuals;
    return measurements;
  }
  return measure_depth3_heads(plane_half,
                              plane_values,
                              plane_channels,
                              channel_head,
                              reference_amplitude,
                              noise_shape,
                              noise_scale_decades,
                              noise_scale_center);
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
