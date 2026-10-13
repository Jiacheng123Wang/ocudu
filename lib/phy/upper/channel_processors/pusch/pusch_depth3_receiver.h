// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief The DEPTH-3 RECEIVER seam: one interface for the unit that turns a received grid into soft bits.
///
/// WHY THIS INTERFACE EXISTS (not a style preference)
/// -------------------------------------------------
/// The AI receiver replaces channel estimation + equalization + demapping as ONE unit, never one module at a
/// time. A per-module backend knob would silently be a depth-2 experiment (a learned demapper in front of a
/// classical channel estimate), which is a different, easier experiment that would be reported as if it were
/// this one. So the choice is made at the granularity of the whole unit, and that is what this expresses.
///
/// WHERE IT SITS, AND WHY THE TWO CALLS ARE SPLIT ACROSS TWO PROCESSOR METHODS
/// --------------------------------------------------------------------------
/// The PUSCH processor reaches the depth-3 unit at exactly two host-side entry points:
///   * estimate():    grid + configuration -> channel estimates and their statistics;
///   * demodulate():  grid + those estimates -> soft bits.
/// They are NOT adjacent in the processor. Estimation runs in \c process_estimate, and the estimates come
/// BACK to the processor through the DM-RS estimator notifier, which is what lets a deferred backend complete
/// later. \c process_data then runs the demodulation. The seam therefore hands the estimates ACROSS the two
/// methods as an ordinary parameter, exactly as the classical path already does -- an abstraction that hid
/// them would have to re-route that callback, which is a bigger change than the substitution itself.
///
/// Everything around the unit (DFT, demultiplex, descrambling, rate matching, LDPC, CRC) stays where it is.
/// These two entry points are the same ones the seam probe (\c OCUDU_RECEIVER_PROBE) brackets, so a
/// measurement and a substitution cannot drift apart.
///
/// TWO IMPLEMENTATIONS, AND THE DIFFERENCE IS A MEASUREMENT
/// -------------------------------------------------------
/// \c ai_identity delegates both calls to the classical units and is bit-identical to the classical arm BY
/// CONSTRUCTION; it is what runs when the arm is selected without a model. \c ai_network runs the network and
/// answers \ref pusch_depth3_receiver::computes_itself with whether the forward pass it ran produced the soft
/// bits. Whether an "ai" run was an AI result is therefore read off the arm and never off the request. That is deliberate, and it is the point of
/// doing it first: it proves the seam has every input the unit needs, that the control variable is inert when
/// not asked for, and that an AI arm which produces nothing cannot disturb the link -- because at this stage
/// it produces nothing at all. Its output may NOT be reported as an AI result until a real forward pass
/// replaces the delegation; \ref pusch_depth3_receiver::is_identity reports which one is running.
#pragma once

#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"

namespace ocudu {

/// \brief Which implementation computes the depth-3 unit.
enum class pusch_depth3_kind {
  /// The classical chain: the estimator, equalizer and demapper selected by the per-module backends.
  classic,
  /// The AI arm with no model to run: stage S-1. It DELEGATES to the classical units and is bit-identical to
  /// \c classic. It is a real, separately constructed object with its own call path, so a measurement taken
  /// through it says something about the substitution itself -- but its output is NOT an AI result, and the
  /// configuration path says so at startup.
  ai_identity,
  /// \brief The AI arm with a bound model: the network computes the unit.
  ///
  /// \c demodulate() assembles the contract's plane from the grid, runs the forward pass on the ANE and
  /// delivers the network's own soft bits; the classical demodulator does not run. \c estimate() still runs the
  /// classical estimator, because the chain's measurements (RSRP, EPRE, SNR, timing advance) are read from its
  /// results until the reporting heads replace them (`design` section 17.2, R6).
  ai_network
};

/// \brief The depth-3 unit: channel estimation + equalization + demapping, as one replaceable thing.
class pusch_depth3_receiver
{
public:
  virtual ~pusch_depth3_receiver() = default;

  /// \brief Which implementation this is.
  ///
  /// Exists so that a run reporting the AI arm while a classical computation happened can be caught. The
  /// stage-1 lesson of this workstream is that a value carried but not honoured is worse than a missing one,
  /// because it looks like a result.
  virtual pusch_depth3_kind kind() const = 0;

  /// \brief Whether this arm recomputes the unit or forwards to the classical one.
  ///
  /// True for \c ai_identity. A consumer that would otherwise report "the AI receiver ran" must consult this
  /// and report the substitution instead.
  bool is_identity() const { return kind() == pusch_depth3_kind::ai_identity; }

  /// \brief Whether this arm will actually COMPUTE the unit itself rather than forward it.
  ///
  /// \ref is_identity answers "which arm is this"; this answers "did anything compute", and the two differ
  /// exactly in the case this workstream has to catch: an arm that was selected and cannot run. The stage-S-1
  /// lesson is that a value carried but not honoured is worse than a missing one, because it looks like a
  /// result - so every report of an AI arm consults this, not the selection.
  ///
  /// For an arm that runs a network this is about the LAST reception, not about the binding: a degraded
  /// reception produced classical soft bits however well the model loaded. Use \ref is_bound for the question
  /// a startup report asks.
  virtual bool computes_itself() const { return !is_identity(); }

  /// \brief Whether this arm has what it needs to compute, asked before any reception has run.
  ///
  /// The distinction matters exactly once and it is the difference between two true statements: "the model is
  /// bound, so this arm will compute" and "the last reception was computed by the network". A startup line that
  /// used the second would report INACTIVE on a perfectly bound arm simply because no slot had arrived yet.
  virtual bool is_bound() const { return computes_itself(); }

  /// \brief Runs the channel estimation entry point of the unit.
  /// \param[in] notifier Notifier to communicate the end of the estimation process.
  /// \param[in] grid     Received resource grid.
  /// \param[in] config   DM-RS configuration parameters.
  virtual void estimate(dmrs_pusch_estimator_notifier&            notifier,
                        const resource_grid_reader&                grid,
                        const dmrs_pusch_estimator::configuration& config) = 0;

  /// \brief Runs the demodulation entry point of the unit.
  /// \param[out] codeword_buffer Demodulated soft bits.
  /// \param[in]  notifier        Demodulation statistics notifier.
  /// \param[in]  grid            Received resource grid for the current slot.
  /// \param[in]  est_results     Channel estimates, from THIS arm. In stage S-1 they are the classical
  ///                             estimator's; an AI arm that computes its own passes its own.
  /// \param[in]  config          Demodulation configuration parameters.
  virtual void demodulate(pusch_codeword_buffer&                 codeword_buffer,
                          pusch_demodulator_notifier&            notifier,
                          const resource_grid_reader&            grid,
                          const dmrs_pusch_estimator_results&    est_results,
                          const pusch_demodulator::configuration& config) = 0;
};

} // namespace ocudu
