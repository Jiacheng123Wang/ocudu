// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Core ML (ANE/GPU/CPU) inference engine for the AI channel estimator.
///
/// Loads a compiled .mlmodelc bundle and runs a single fixed-shape prediction
/// ([1, 612, 14, 2] fp32 -> [1, 612, 14, 2] fp32, the HELENA grid shape) with
/// zero-copy MLMultiArray wrappers over the host buffers. The synchronous
/// prediction executes on the Apple Neural Engine / GPU as selected by Core ML
/// (compute units ALL); the wall clock is exposed via last_predict_us().

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

class coreml_nn_engine
{
public:
  coreml_nn_engine()  = default;
  ~coreml_nn_engine();

  coreml_nn_engine(const coreml_nn_engine&)            = delete;
  coreml_nn_engine& operator=(const coreml_nn_engine&) = delete;

  /// \brief Loads the compiled Core ML model (.mlmodelc directory).
  /// \param[in] modelc_path Filesystem path of the .mlmodelc bundle.
  /// \return True on success.
  bool init(const char* modelc_path);

  /// \brief Runs one prediction over the HELENA grid.
  /// \param[in]  in       Input grid [1, nof_subc, 14, 2] fp32 (subcarrier, symbol, re/im).
  /// \param[out] out      Output grid, same shape.
  /// \param[in]  nof_subc Grid width in subcarriers (612 = 51 PRB, 624 = 52 PRB).
  /// \return True on success.
  bool predict(const float* in, float* out, unsigned nof_subc);

  /// Wall-clock duration of the last predict() in microseconds.
  double last_predict_us() const { return last_predict_us_; }

private:
  void*  impl           = nullptr;
  double last_predict_us_ = 0.0;
};

} // namespace metal
} // namespace ocudu
