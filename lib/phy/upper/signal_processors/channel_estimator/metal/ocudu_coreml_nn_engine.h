// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Core ML (ANE/GPU/CPU) inference engine for the AI channel estimator.
///
/// Loads a compiled .mlmodelc bundle and runs a single fixed-shape prediction
/// ([1, 612, 14, 2] fp32 -> [1, 612, 14, 2] fp32, the HELENA grid shape) with
/// zero-copy MLMultiArray wrappers over the host buffers. The synchronous
/// prediction executes on the Apple Neural Engine / GPU as selected by Core ML
/// (compute units ALL); the wall clock is exposed via last_predict_us().
///
/// \ref init_depth3 adds the depth-3 receiver's shape instead: one fp16 plane of
/// [1, 612, 14, 12] and four named outputs, which is the contract in
/// `model_network_design.md` section 0.2. The symbol axis is SPATIAL there, so a 3x3 kernel mixes
/// time as well as frequency; the superseded folded layout is not accepted. It is a separate entry
/// point rather than a generalisation of \ref init, because the depth-1 refiner's callers keep the
/// layout they were trained in.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

class coreml_nn_engine
{
public:
  /// \brief The depth-3 input plane, as the contract freezes it.
  ///
  /// A host assembles the plane from the resource grid, the pilot LS, the masks and the metadata,
  /// which is what `depth3.pack_input_spatial` does in the training tree. The layout is
  /// [1, nof_subcarriers, 14, 12]: subcarrier axis outermost, then the SPATIAL symbol axis, then
  /// the twelve per-symbol channels, contiguous, in IEEE half precision.
  struct depth3_input {
    /// [1, nof_subcarriers, 14, 12] fp16 values in their bit patterns.
    const uint16_t* plane = nullptr;
    /// Subcarrier count of the window, 612 for the 51 PRB bandwidth part.
    unsigned nof_subcarriers = 612;
  };

  /// \brief The depth-3 outputs, one buffer per head.
  ///
  /// Every pointer receives fp16 values for its head and stays owned by the caller, so the forward
  /// pass writes into the buffers the receiver reads and no copy is needed.
  struct depth3_output {
    /// Main head: `[1, nsc, 14 * bits_per_symbol]`, channel index `symbol * Qm + bit`, tanh bounded.
    uint16_t* llr = nullptr;
    /// Auxiliary head 1: `[1, nsc, 14]` post-equalization noise, tanh bounded.
    uint16_t* post_eq_noise = nullptr;
    /// Auxiliary head 2: `[1, nsc, 28]`, channel `2 * symbol + {0, 1}` real and imaginary parts.
    uint16_t* ch_est = nullptr;
    /// Auxiliary head 2b: one scalar, the DM-RS domain noise variance, tanh bounded.
    uint16_t* dmrs_noise = nullptr;
    /// Bits per resource element of the main head, which selects `14 * bits_per_symbol`.
    unsigned bits_per_symbol = 8;
  };

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

  /// \brief Loads a compiled depth-3 model: the contract's input plane and its four heads.
  ///
  /// Checks that the bundle carries what the contract asks for, and refuses one that does not:
  /// a fp16 input named `x` of shape [1, nof_subcarriers, 1, 64], and the outputs `llr`,
  /// `post_eq_noise`, `ch_est` and `dmrs_noise`. A model missing a head is rejected rather than
  /// bound partially, because an arm that reported a head it never computed would be worse than
  /// one that refused to run.
  ///
  /// \param[in] modelc_path Filesystem path of the .mlmodelc bundle.
  /// \return True when the bundle matches the contract.
  bool init_depth3(const char* modelc_path);

  /// \brief Runs one depth-3 forward pass into caller-owned buffers.
  /// \param[in]  in  The contract's input plane.
  /// \param[out] out The four head buffers, each written by this call.
  /// \return True on success.
  bool predict_depth3(const depth3_input& in, const depth3_output& out);

  /// \brief Bits per resource element the bound depth-3 model's main head was built for, or 0 if none.
  ///
  /// The contract trains ONE MODEL PER MODULATION (section 0.2(h)), so the arm has to know which one it bound
  /// before it runs: a 256QAM model fed a 64QAM reception produces 112 channels where the caller reads 84, and
  /// the only honest outcome is a refusal that NAMES the reason. Without this the engine still refuses -- it
  /// checks the caller's `bits_per_symbol` against the model -- but the receiver can only report "the forward
  /// pass failed", which is the kind of message that costs an afternoon.
  unsigned depth3_bits_per_symbol() const;

  /// \brief Whether the bound bundle declares the MODULATION-CONDITIONED contract (`depth3_layout=modcond`).
  ///
  /// A conditioned bundle serves every modulation from one artifact, with the Qm carried as extra input channels
  /// and a main head whose stride is the widest modulation. It CANNOT be told apart from a per-modulation bundle
  /// by shape -- both carry 14 * 8 = 112 main-head channels -- so the bundle declares which contract it follows
  /// and this reads the declaration.
  bool depth3_is_modulation_conditioned() const;

  /// How many modulations a conditioned bundle declares it serves (0 for a per-modulation bundle).
  unsigned depth3_nof_declared_qm() const;

  /// The \p index -th declared modulation, as bits per symbol, ascending; 0 when out of range.
  unsigned depth3_declared_qm(unsigned index) const;

  /// \brief Input plane channels the bound bundle expects.
  ///
  /// 12 for the per-modulation contract; 12 + the four conditioning channels for a conditioned one. The caller
  /// sizes its plane from this rather than from a constant, so the two contracts cannot be confused at the seam.
  unsigned depth3_input_channels() const;

  /// Wall-clock duration of the last predict() in microseconds.
  double last_predict_us() const { return last_predict_us_; }

private:
  void*  impl           = nullptr;
  double last_predict_us_ = 0.0;
};

} // namespace metal
} // namespace ocudu
