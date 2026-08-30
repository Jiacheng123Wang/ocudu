// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief HELENA AI channel estimator adapter (Core ML / ANE).
///
/// The stage runs the CLASSICAL LS + FD smoothing + TD interpolation first (same
/// input semantics as the training set: the interpolated-LS grid), then refines
/// the full time-frequency grid with the HELENA network (one Core ML prediction
/// per port per slot). When the network is unavailable or the configuration is
/// outside the v1 envelope (51 PRB at CRB 0, hop 0), the classical estimates are
/// served unchanged - graceful fallback.
///
/// Per-site online adaptation (AI plan 9.0): the training sidecar atomically
/// swaps the model file and calls reload(); the new weights take effect from the
/// next slot. The data-collection hook (decision-directed labels on crc-OK
/// slots) is part of the next milestone.

#pragma once

#include "../port_channel_estimator_average_impl.h"
#include "ocudu_coreml_nn_engine.h"

#include <array>
#include <memory>
#include <string>

namespace ocudu {

class port_channel_estimator_helena_impl : public port_channel_estimator_average_impl
{
public:
  /// \param[in] interp         Interpolator (the classical pre-stage).
  /// \param[in] ta_estimator   Time alignment estimator (base class).
  /// \param[in] modelc_path    Compiled Core ML model (.mlmodelc bundle path).
  /// \param[in] compensate_cfo Whether the classical pre-stage compensates CFO.
  port_channel_estimator_helena_impl(std::unique_ptr<interpolator>             interp,
                                     std::unique_ptr<time_alignment_estimator> ta_estimator,
                                     std::string                              modelc_path,
                                     bool                                     compensate_cfo_ = true);
  ~port_channel_estimator_helena_impl() override;

  /// \brief Hot-reloads the Core ML model (per-site fine-tuned weights).
  ///
  /// The training sidecar writes the new bundle and swaps it in atomically;
  /// on failure the incumbent model stays active. Call during low load.
  /// \return True when the new model is active.
  bool reload(const std::string& modelc_path);

  /// Wall-clock duration of the last NN forward pass in microseconds (0 when the
  /// NN was not used on the last slot).
  double last_predict_us() const { return last_predict_us_; }

  /// The NN input grid of the last slot (the exact classical-interpolated LS grid the
  /// network consumed; [612, 14, 2] subcarrier-major). Used by the Phase C dump mode
  /// to build the input-aligned fine-tuning set. Valid until the next compute().
  const float* last_nn_input() const { return nn_in.data(); }

private:
  // See the base class documentation.
  void apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args) override;
  void get_symbol_ch_estimate(span<cbf16_t> symbol, unsigned i_symbol, unsigned tx_layer) const override;
  void get_symbol_ch_estimate(span<cbf16_t>                              symbol,
                              unsigned                                   i_symbol,
                              unsigned                                   tx_layer,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const override;

  /// True when the NN produced the grid of the current slot (else the classical
  /// path serves get_symbol_ch_estimate).
  mutable bool nn_grid_valid = false;

  std::unique_ptr<ocudu::metal::coreml_nn_engine> engine;
  std::string                                    modelc_path;
  double                                         last_predict_us_ = 0.0;

  /// NN input/output grids [612, 14, 2] fp32, subcarrier-major (the training layout).
  std::array<float, 612 * 14 * 2> nn_in;
  std::array<float, 612 * 14 * 2> nn_out;

  /// Estimated full time-frequency grid (layer x symbol slices, cbf16).
  static_re_buffer<MAX_LAYERS * MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS> grid_est;

  /// TD-interpolation scratch for the classical per-symbol estimates.
  std::array<cbf16_t, MAX_NOF_SUBCARRIERS> td_scratch;
};

} // namespace ocudu
