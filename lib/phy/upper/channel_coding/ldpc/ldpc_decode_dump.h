// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// This file implements ... (depth4 data capture, see the .cpp for the reasoning).

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include <cstdint>

namespace ocudu {

/// \brief Records what the LDPC decoder saw and what it converged to, for the depth4 network's training and analysis.
///
/// WHY IT EXISTS (depth4 design, survey section 10.5). depth4's training target is the TRANSMITTED bits, which the
/// tooling already recovers by replaying captures, so nothing here is on the training path. What is missing is the
/// decoder's own view: the LLRs it was handed, the soft values it held at each iteration, the syndrome weight, and the
/// iteration at which it stopped. Those are the reference for distillation and the only way to check what a learned
/// layer should reproduce against what belief propagation actually does.
///
/// It is OFF unless OCUDU_LDPC_DUMP names a directory, and the cost when off is one predictable branch per decode and
/// per iteration -- a program that does not ask for the dump runs the same instructions as before.
class ldpc_decode_dump
{
public:
  /// Process-wide instance, so the configuration is read once.
  static ldpc_decode_dump& instance();

  /// Whether anything should be written at all.
  bool enabled() const { return dumping_enabled; }

  /// Whether per-iteration soft values are wanted as well as the final ones
  /// (OCUDU_LDPC_DUMP_ITERS=1). Off by default: an iteration's worth of soft bits per iteration per block is a lot.
  bool iterations_wanted() const { return dump_iterations; }

  /// \brief Records one decode.
  ///
  /// \param[in] elapsed_us      How long the decode took, so the cost of one iteration can be MEASURED on the real
  ///                            graph rather than inferred from a random-topology benchmark (survey section 10.2).
  /// \param[in] input           The LLRs the decoder was handed, in RATE-MATCHED order.
  /// \param[in] soft_input      The same information in CODEWORD order, i.e. the decoder's own soft buffer right after
  ///                            derate matching and before the first iteration. depth4's message passing works on this
  ///                            domain -- the rate-matching permutation belongs to the assembler, not to the decoder --
  ///                            so a training set that only holds the rate-matched order would force every experiment to
  ///                            reimplement the dematcher.
  /// \param[in] soft_final      The soft values after the last iteration it ran, in the decoder's own sign convention.
  /// \param[in] soft_per_iter   One entry per iteration, each \c input.size() long, or empty when not requested.
  /// \param[in] syndrome_weight One entry per iteration.
  /// \param[in] iterations      Iterations the decoder ran.
  /// \param[in] success         Whether it declared success (CRC or syndrome, as configured).
  /// \param[in] lifting_size    The block's lifting size.
  /// \param[in] bg_K            The base graph's Kb.
  void record(unsigned                         elapsed_us,
              span<const log_likelihood_ratio> input,
              span<const log_likelihood_ratio> soft_input,
              span<const log_likelihood_ratio> soft_final,
              span<const int8_t>               soft_per_iter,
              span<const uint16_t>             syndrome_weight,
              unsigned                         iterations,
              bool                             success,
              unsigned                         lifting_size,
              unsigned                         bg_K);

private:
  ldpc_decode_dump();

  bool          dumping_enabled = false;
  bool          dump_iterations = false;
  unsigned      max_blocks      = 0;
  unsigned      written         = 0;
  char          directory[512]  = {};

  /// One predictable branch in the hot path: while false, nothing else is evaluated.
  static bool enabled_flag;
};

} // namespace ocudu
