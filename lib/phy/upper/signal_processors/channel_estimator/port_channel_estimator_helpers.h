// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/support/interpolator.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h"

namespace ocudu {

/// \brief Extracts channel observations corresponding to DM-RS pilots from the resource grid for one layer, one hop
/// and for the selected port.
/// \param[out] rx_symbols  Symbol buffer destination.
/// \param[in]  grid        Resource grid.
/// \param[in]  port        Port index.
/// \param[in]  cfg         Configuration parameters of the current context.
/// \param[in]  hop         Intra-slot frequency hopping index: 0 for first position (before hopping), 1 for second
///                         position (after hopping).
/// \param[in] i_layer      Layer index.
/// \return The number of OFDM symbols containing DM-RS for the given layer and hop.
unsigned extract_layer_hop_rx_pilots(dmrs_symbol_list&                            rx_symbols,
                                     const resource_grid_reader&                  grid,
                                     unsigned                                     port,
                                     const port_channel_estimator::configuration& cfg,
                                     unsigned                                     hop,
                                     unsigned                                     i_layer = 0);

/// \brief Applies frequency domain smoothing strategy.
/// \param[out] enlarged_filtered_pilots_out   Smoothed pilots estimates.
/// \param[in]  enlarged_pilots_in             Pilots estimates.
/// \param[in]  nof_rb                         Number of resource blocks.
/// \param[in]  stride                         Reference signals stride in frequency domain.
/// \param[in]  fd_smoothing_strategy          Frequency domain smoothing strategy.
void apply_fd_smoothing(span<cf_t>                                   enlarged_filtered_pilots_out,
                        span<cf_t>                                   enlarged_pilots_in,
                        unsigned                                     nof_rb,
                        unsigned                                     stride,
                        port_channel_estimator_fd_smoothing_strategy fd_smoothing_strategy);

/// \brief Writes the raised-cosine filter coefficients apply_fd_smoothing() uses for a hop into
/// \p out.
///
/// The coefficients depend only on the hop's geometry (its PRB count and the pilot stride), so a
/// device backend that has to run the same convolution can be handed them instead of reproducing
/// filter_type()'s table and resampling - which is the part of the smoothing that is not worth
/// moving.
/// \return The number of coefficients written, or 0 when they do not fit in \p out.
unsigned get_fd_smoothing_filter(span<float> out, unsigned nof_rb, unsigned stride);

/// \brief Estimates the time alignment based on one hop.
///
/// \param[in] pilots_lse   The estimated channel (only for REs carrying DM-RS).
/// \param[in] pattern      DM-RS pattern for the current layer.
/// \param[in] hop          Intra-slot frequency hopping index: 0 for first position (before hopping), 1 for second
///                         position (after hopping).
/// \param[in] scs          Subcarrier spacing.
/// \param[in] ta_estimator Time alignment estimator.
/// \return The estimated time alignment as a number of samples (the sampling frequency is given by the DFT processor).
float estimate_time_alignment(const re_measurement<cf_t>&                       pilots_lse,
                              const port_channel_estimator::layer_dmrs_pattern& pattern,
                              unsigned                                          hop,
                              subcarrier_spacing                                scs,
                              time_alignment_estimator&                         ta_estimator);

/// \brief Estimates the noise energy of one hop and a given range of layers.
///
/// The receiver error is estimated as the difference between the received pilots and the pilots regenerated from the
/// channel estimates. The channel estimates are expected to be scaled by the same factor the FD processing applies
/// (i.e., \c 1/beta for the interpolate TD strategy), see \ref port_channel_estimator_average_impl.
/// \param[in]  pilots               Transmitted pilots.
/// \param[in]  rx_pilots            Received pilots.
/// \param[in]  estimates            Channel estimates for the pilot REs (scaled, see above).
/// \param[in]  beta                 DM-RS to data amplitude scaling.
/// \param[in]  dmrs_mask            Boolean mask identifying the OFDM symbols carrying DM-RS within the slot.
/// \param[in]  cfo                  Estimated CFO (empty when unavailable).
/// \param[in]  symbol_start_epochs  Starting time of the symbols inside the slot, in units of OFDM symbol duration.
/// \param[in]  compensate_cfo       Whether the CFO is compensated in the channel estimates.
/// \param[in]  first_hop_symbol     Index of the first OFDM symbol of the current hop.
/// \param[in]  last_hop_symbol      Index of the last OFDM symbol of the current hop (not included).
/// \param[in]  hop_offset           Number of OFDM symbols carrying DM-RS in the previous hop.
/// \param[in]  start_layer          Index of the first transmission layer to be processed.
/// \param[in]  stop_layer           Index of the last transmission layer (not included) to be processed.
/// \return The noise energy for the current hop and the given range of layers.
float estimate_noise(const dmrs_symbol_list&                   pilots,
                     const dmrs_symbol_list&                   rx_pilots,
                     const re_measurement<cf_t>&               estimates,
                     float                                     beta,
                     const bounded_bitset<MAX_NSYMB_PER_SLOT>& dmrs_mask,
                     std::optional<float>                      cfo,
                     span<const float>                         symbol_start_epochs,
                     bool                                      compensate_cfo,
                     unsigned                                  first_hop_symbol,
                     unsigned                                  last_hop_symbol,
                     unsigned                                  hop_offset,
                     unsigned                                  start_layer,
                     unsigned                                  stop_layer);

// Returns the interpolator configuration for the given RE pattern.
interpolator::configuration configure_interpolator(const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_mask);

/// \brief Extract resource elements from an OFDM symbol view.
/// \param[out] dmrs_symbols     Extracted resource elements.
/// \param[in]  ofdm_symbol_view Resource grid OFDM symbol view.
/// \param[in]  hop_rb_mask      Resource block selector mask.
/// \param[in]  re_pattern       Resource element pattern within resource blocks.
void extract_dmrs_grid(span<cf_t>                                    dmrs_symbols,
                       span<const cbf16_t>                           ofdm_symbol_view,
                       const bounded_bitset<MAX_NOF_PRBS>&           hop_rb_mask,
                       const bounded_bitset<NOF_SUBCARRIERS_PER_RB>& re_pattern);

} // namespace ocudu
