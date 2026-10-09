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
/// STAGE S-1: THE AI HEAD EXISTS BUT COMPUTES NOTHING
/// -------------------------------------------------
/// The first implementation behind this interface delegates both calls to the classical units and is
/// therefore bit-identical to the classical arm BY CONSTRUCTION. That is deliberate, and it is the point of
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
  /// The AI arm. Stage S-1: it DELEGATES to the classical units and is bit-identical to \c classic. It is a
  /// real, separately constructed object with its own call path, so a measurement taken through it says
  /// something about the substitution itself -- but its output is NOT an AI result yet, and the configuration
  /// path says so at startup.
  ai_identity
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
