// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace ocudu {

/// \brief Which bound model computes which modulation.
///
/// The arm binds what the command line asked for -- a SLOT per modulation plus a fallback that serves whatever
/// modulation its own bundle declares -- and this decides who SERVES what once the bundles are loaded and have
/// declared their modulation:
///
///  * a slot wins over the fallback for its own modulation;
///  * the fallback serves the modulations no slot claims AND that it declares itself;
///  * a modulation nobody declares is served by nobody: the reception degrades to the classical chain and the
///    missing modulation is named, in the reception's log line and in the startup coverage line.
///
/// WHY IT IS A SEPARATE OBJECT. It is the part of the selection that can be tested without Core ML, and the two
/// mistakes it exists to catch are both silent ones: a slot whose bundle declares a DIFFERENT modulation would
/// otherwise serve nothing while looking configured, and a bundle that declares a modulation outside the four the
/// arm can serve (QPSK, 16QAM, 64QAM, 256QAM) would otherwise sit there looking bound. The context is measured:
/// the default model was called `d3_mixed` and carried 256QAM alone, so a name is not a declaration and a slot is
/// a promise the bundle has to keep.
class depth3_model_map
{
public:
  /// The modulations a depth-3 model can declare, ascending: QPSK, 16QAM, 64QAM, 256QAM.
  static constexpr std::array<unsigned, 4> candidate_qm{2, 4, 6, 8};

  /// Name of a modulation given its bits per symbol, for the lines a human reads. Empty when unrecognised.
  static const char* name(unsigned qm)
  {
    switch (qm) {
      case 2:
        return "QPSK";
      case 4:
        return "16QAM";
      case 6:
        return "64QAM";
      case 8:
        return "256QAM";
      default:
        return "";
    }
  }

  /// One bound model and what it serves.
  struct entry {
    /// The modulation the model declares (its main head's width divided by the slot's symbols).
    unsigned    qm = 0;
    /// The path it was bound from, as given.
    std::string path;
    /// True when this model was bound from the fallback knob rather than from a per-modulation slot.
    bool        fallback = false;
    /// True when this model is the MODULATION-CONDITIONED contract: one artifact serving several modulations,
    /// whose Qm arrives as extra input channels. The coverage line says so, because "one bundle computes all
    /// four" and "four bundles compute one each" are different deployments with the same coverage.
    bool        conditioned = false;
  };

  /// Whether a model was kept, refused, or dropped as redundant.
  enum class status {
    /// The model serves at least one modulation.
    kept,
    /// The model was not kept and the arm has to say why (a slot that declares another modulation, a bundle that
    /// declares a modulation the arm cannot serve, a fallback that serves nothing).
    refused,
    /// The model was not kept because a slot already serves every modulation it declares. Not an error: the slot
    /// wins and a line says so, because "I passed the fallback and it was not used" must not become a hunt.
    redundant
  };

  /// Outcome of adding one model, with the sentence to print for a refusal or a redundancy.
  struct outcome {
    status      result = status::kept;
    std::string reason;
  };

  /// \brief Adds a per-modulation slot.
  /// \param[in] slot_qm     The modulation the SLOT asked for.
  /// \param[in] declared_qm The modulation the bundle declares (0 when the bundle did not load).
  /// \param[in] path        The bundle's path.
  outcome add_slot(unsigned slot_qm, unsigned declared_qm, std::string path);

  /// \brief Adds a model that DECLARES a set of modulations: the modulation-conditioned contract.
  ///
  /// The bundle is one artifact -- a shared trunk, the Qm as four extra input channels, one shared reporting head
  /// set -- and it can only be told apart from a per-modulation bundle by its declaration, never by its shape.
  /// Each declared modulation gets an entry that points at the same path, so the per-reception lookup is
  /// unchanged; a slot that already serves one of them keeps it (the slot is the more specific request) and the
  /// outcome reports how many the conditioned model actually serves.
  ///
  /// \param[in] declared_qm The modulations the bundle declares, in bits per symbol (2, 4, 6, 8).
  /// \param[in] path        The bundle's path.
  outcome add_conditioned(const std::vector<unsigned>& declared_qm, std::string path);

  /// \brief Adds the fallback model.
  /// \param[in] declared_qm The modulation the bundle declares (0 when the bundle did not load).
  /// \param[in] path        The bundle's path.
  outcome add_fallback(unsigned declared_qm, std::string path);

  /// The model that serves \c qm, or nullptr when no bound model does.
  const entry* find(unsigned qm) const;

  /// Every model that serves at least one modulation, in binding order (the slots, then the fallback).
  const std::vector<entry>& entries() const { return kept; }

  /// The candidate modulations no bound model serves: the reception list that degrades to the classical chain.
  std::vector<unsigned> missing() const;

private:
  std::vector<entry> kept;
};

inline depth3_model_map::outcome depth3_model_map::add_slot(unsigned    slot_qm,
                                                            unsigned    declared_qm,
                                                            std::string path)
{
  if (declared_qm == 0) {
    return {status::refused,
            "the bundle did not load, so it declares no modulation and cannot be bound to a slot"};
  }
  if (declared_qm != slot_qm) {
    return {status::refused,
            "the slot asks for Qm=" + std::to_string(slot_qm) + " but the bundle declares Qm=" +
                std::to_string(declared_qm) + " (the arm's envelope is the bundle's own declaration)"};
  }
  if (std::none_of(candidate_qm.begin(), candidate_qm.end(), [declared_qm](unsigned qm) {
        return qm == declared_qm;
      })) {
    return {status::refused, "the bundle declares Qm=" + std::to_string(declared_qm) +
                                 ", which is not one of the four modulations the arm serves"};
  }
  if (find(declared_qm) != nullptr) {
    return {status::redundant, "another slot already serves Qm=" + std::to_string(declared_qm)};
  }
  kept.push_back(entry{declared_qm, std::move(path), false});
  return {};
}

inline depth3_model_map::outcome depth3_model_map::add_conditioned(const std::vector<unsigned>& declared_qm,
                                                                   std::string                  path)
{
  if (declared_qm.empty()) {
    return {status::refused, "the bundle declares the conditioned contract but names no modulation"};
  }
  unsigned added = 0;
  for (unsigned qm : declared_qm) {
    if (std::none_of(candidate_qm.begin(), candidate_qm.end(), [qm](unsigned c) { return c == qm; })) {
      return {status::refused,
              "the conditioned bundle declares Qm=" + std::to_string(qm) +
                  ", which is not one of the four modulations the arm serves"};
    }
    if (find(qm) != nullptr) {
      // A slot (or an earlier declaration) already serves this modulation: the more specific request wins, and
      // the caller's coverage line still shows the modulation as computed.
      continue;
    }
    kept.push_back(entry{qm, path, false, true});
    ++added;
  }
  if (added == 0) {
    return {status::redundant, "every modulation this bundle declares is already served by a slot"};
  }
  if (added != declared_qm.size()) {
    // Partly redundant: not an error, but silent partial use is what this report exists to prevent.
    return {status::redundant, "the bundle declares " + std::to_string(declared_qm.size()) +
                                   " modulations and serves " + std::to_string(added) +
                                   "; the rest are already bound to slots"};
  }
  return {};
}

inline depth3_model_map::outcome depth3_model_map::add_fallback(unsigned declared_qm, std::string path)
{
  if (declared_qm == 0) {
    return {status::refused, "the fallback bundle did not load, so it declares no modulation"};
  }
  if (std::none_of(candidate_qm.begin(), candidate_qm.end(), [declared_qm](unsigned qm) {
        return qm == declared_qm;
      })) {
    return {status::refused, "the fallback declares Qm=" + std::to_string(declared_qm) +
                                 ", which is not one of the four modulations the arm serves"};
  }
  if (find(declared_qm) != nullptr) {
    // The slots win, and this is said rather than refused: the fallback is filled by the build's default, so it
    // can end up declaring a modulation a slot already serves without the operator having asked for anything.
    return {status::redundant, "a slot already serves Qm=" + std::to_string(declared_qm) +
                                   ", so the fallback model is not used"};
  }
  kept.push_back(entry{declared_qm, std::move(path), true});
  return {};
}

inline const depth3_model_map::entry* depth3_model_map::find(unsigned qm) const
{
  auto it = std::find_if(kept.begin(), kept.end(), [qm](const entry& e) { return e.qm == qm; });
  return (it == kept.end()) ? nullptr : &*it;
}

inline std::vector<unsigned> depth3_model_map::missing() const
{
  std::vector<unsigned> out;
  for (unsigned qm : candidate_qm) {
    if (find(qm) == nullptr) {
      out.push_back(qm);
    }
  }
  return out;
}

} // namespace ocudu
