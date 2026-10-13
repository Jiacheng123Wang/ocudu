// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief The two depth-3 receiver implementations: the classical chain, and the AI arm's stage-S-1 head.
///
/// The AI head here is an IDENTITY: it forwards both entry points to the classical units. See the seam header
/// for why that is the first thing to build rather than a placeholder to be ashamed of -- it proves the seam
/// has every input the unit needs and that an arm producing nothing cannot disturb the link.
///
/// Both classes are the SAME delegation with a different \ref kind(). That is not duplication for its own
/// sake: it is what makes the two arms two separately constructed objects with two call paths, so that a
/// measurement taken through the AI arm measures the substitution rather than an alias. When a real forward
/// pass arrives it replaces the body of \c pusch_depth3_receiver_ai_identity alone, and nothing else moves.
#pragma once

#include "pusch_depth3_receiver.h"

#include <memory>

namespace ocudu {

/// \brief The classical depth-3 unit: the estimator and demodulator built by the per-module backends.
class pusch_depth3_receiver_classic : public pusch_depth3_receiver
{
public:
  /// The units are REFERENCED, not owned: the dependencies own them, so that the strict policy interrogates
  /// the very modules the unit runs. A second, owned instance would let the policy ask about a module the unit
  /// never ran -- a silent divergence that S-1 exists to rule out, and the reason ownership stays with the
  /// dependencies.
  pusch_depth3_receiver_classic(dmrs_pusch_estimator& estimator_, pusch_demodulator& demodulator_) :
    estimator(estimator_), demodulator(demodulator_)
  {
    // No null checks: these are references, and the dependencies that own them already asserted them.
  }

  pusch_depth3_kind kind() const override { return pusch_depth3_kind::classic; }

  void estimate(dmrs_pusch_estimator_notifier&            notifier,
                const resource_grid_reader&                grid,
                const dmrs_pusch_estimator::configuration& config) override
  {
    estimator.estimate(notifier, grid, config);
  }

  void demodulate(pusch_codeword_buffer&                 codeword_buffer,
                  pusch_demodulator_notifier&            notifier,
                  const resource_grid_reader&            grid,
                  const dmrs_pusch_estimator_results&    est_results,
                  const pusch_demodulator::configuration& config) override
  {
    demodulator.demodulate(codeword_buffer, notifier, grid, est_results, config);
  }

private:
  dmrs_pusch_estimator& estimator;
  pusch_demodulator&    demodulator;
};

/// \brief The AI arm's stage-S-1 head: a real object on its own call path that DELEGATES to the classical
/// units, and is therefore bit-identical to \ref pusch_depth3_receiver_classic by construction.
///
/// It is reported as identity, never as an AI result, and the configuration path logs that in as many words.
class pusch_depth3_receiver_ai_identity : public pusch_depth3_receiver
{
public:
  pusch_depth3_receiver_ai_identity(dmrs_pusch_estimator& estimator_,
                                    pusch_demodulator&    demodulator_,
                                    const std::string&    model_path_) :
    estimator(estimator_), demodulator(demodulator_)
  {
    // No null checks on the references: the dependencies that own them already asserted them.
    //
    // The model is bound HERE, at construction, and the return value is kept, because the arm's honesty
    // depends on it: an arm that was SELECTED but cannot compute must not be reported as an AI result. The
    // estimator answers whether it will refine, and a backend that cannot refine says so rather than
    // silently forwarding (see port_channel_estimator::set_ai_refinement).
    refinement_bound = estimator.set_ai_refinement(true, model_path_);
  }

  pusch_depth3_kind kind() const override { return pusch_depth3_kind::ai_identity; }

  /// See the seam interface. True only when the estimator accepted the model, because only then does anything
  /// beyond the classical chain run.
  bool computes_itself() const override { return refinement_bound; }

  /// For this arm the binding IS the answer to both questions: it computes nothing of its own either way.
  bool is_bound() const override { return refinement_bound; }

  /// \brief Whether this arm will actually compute a refined estimate (the same answer, named for this arm).
  bool is_refinement_bound() const { return refinement_bound; }

  void estimate(dmrs_pusch_estimator_notifier&            notifier,
                const resource_grid_reader&                grid,
                const dmrs_pusch_estimator::configuration& config) override
  {
    // The forward pass runs INSIDE the estimator, next to the grid it rewrites, and it is activated by the
    // binding made in the constructor. Forwarding the call is therefore all this arm has to do, and the
    // refinement it causes is the one this arm selected - not a side effect of an environment variable.
    estimator.estimate(notifier, grid, config);
  }

  void demodulate(pusch_codeword_buffer&                 codeword_buffer,
                  pusch_demodulator_notifier&            notifier,
                  const resource_grid_reader&            grid,
                  const dmrs_pusch_estimator_results&    est_results,
                  const pusch_demodulator::configuration& config) override
  {
    // STAGE S-1: and HERE. Both entry points are present so that the substitution is COMPLETE rather than
    // half-wired: an arm that took over only one of the two would be a different experiment.
    demodulator.demodulate(codeword_buffer, notifier, grid, est_results, config);
  }

private:
  dmrs_pusch_estimator& estimator;
  pusch_demodulator&    demodulator;
  /// Whether the estimator accepted the refinement binding (see the constructor).
  bool refinement_bound = false;
};

/// \brief Builds the depth-3 arm the configuration asked for.
/// \param[in] kind        Which arm.
/// \param[in] estimator   The classical estimator, already built by the per-module backends.
/// \param[in] demodulator The classical demodulator, already built by the per-module backends.
std::unique_ptr<pusch_depth3_receiver> create_pusch_depth3_receiver(pusch_depth3_kind     kind,
                                                                    dmrs_pusch_estimator& estimator,
                                                                    pusch_demodulator&    demodulator,
                                                                    const std::string&    model_path = {});

} // namespace ocudu
