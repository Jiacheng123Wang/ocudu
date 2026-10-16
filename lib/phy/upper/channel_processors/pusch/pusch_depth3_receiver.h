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

/// One model the arm has bound, as the startup coverage line reports it.
struct pusch_depth3_bound_model {
  /// The modulation the bundle declares (its main head's width divided by the slot's symbols).
  unsigned    qm = 0;
  /// The path it was bound from.
  std::string path;
  /// True when it came from the fallback knob rather than from a per-modulation slot.
  bool        fallback = false;
  /// Whether the bundle declares the SNR range its reporting heads were trained on (`depth3_snr_range`).
  bool        has_snr_range = false;
  /// That range, in dB; meaningless when \c has_snr_range is false.
  float       snr_min_db = 0.0F;
  float       snr_max_db = 0.0F;
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

  /// \brief The modulation the BOUND model carries, in bits per symbol; 0 when no model is bound.
  ///
  /// It is the MODEL's declaration (its main head's width divided by the slot's symbol count), not the
  /// reception's: the arm refuses every reception whose modulation differs from this one, so a startup line that
  /// names the model must name this too. WHY IT EXISTS (2026-10-16): the default model was called `d3_mixed` and
  /// carried 256QAM alone; a reader of a leg log had to know that the name was wrong to know that a QPSK
  /// reception would degrade. A name is not a declaration -- this is.
  virtual unsigned bound_qm() const { return 0; }

  /// \brief Every model this arm has bound, with the modulation each declares and where it came from.
  ///
  /// This is what the startup coverage line is printed from: a run may bind several models (a slot per modulation
  /// plus a fallback), and "which modulations does this run actually compute" must be read rather than inferred --
  /// the fallback is filled by the build's default, so the command line alone does not answer it.
  virtual std::vector<pusch_depth3_bound_model> bound_models() const { return {}; }

  /// \brief The modulations no bound model serves.
  ///
  /// Those are the receptions that degrade to the classical chain, and the arm says so per reception too (counted,
  /// with the modulation named). Kept here as well because the coverage belongs in one line at startup, before any
  /// reception has arrived.
  virtual std::vector<unsigned> unserved_modulations() const { return {}; }

  /// Whether the fallback this arm bound came from the BUILD rather than from the command line.
  virtual bool fallback_is_the_build_default() const { return false; }

  /// \brief One line per model the arm tried to bind: kept, dropped as redundant, or refused with the reason.
  ///
  /// The pool builds one arm per concurrent PUSCH thread, so a per-arm print would repeat the same binding seven
  /// times; the factory prints these once, for the pool, before the coverage. It is also the only place a REFUSED
  /// slot is visible: a slot whose bundle declares another modulation is refused while everything else keeps
  /// working, which is exactly the kind of half-configured state that must not pass unread.
  virtual std::vector<std::string> binding_notes() const { return {}; }

  /// \brief Whether the bound bundle computes every modulation it declares from ONE set of weights, conditioned
  /// on the modulation order (design section 19.102) instead of specialised per modulation.
  ///
  /// It is the BUNDLE's declaration and not a mode the arm chooses: two bundles with an identical 112-channel
  /// main head -- a conditioned one and a per-modulation 256QAM one -- differ in nothing observable except what
  /// they declare, so this is what tells a reader which contract a bound model speaks.
  virtual bool is_modulation_conditioned() const { return false; }

  /// \brief The plane width the bound bundle expects; 0 when no model is bound.
  ///
  /// A conditioned bundle takes the contract's twelve channels PLUS its one-hot conditioning ones, so the width
  /// belongs to the bundle: a startup line reporting the constant while a conditioned bundle is bound would be
  /// describing a different input than the one the network is fed.
  virtual unsigned bound_plane_channels() const { return 0; }

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
