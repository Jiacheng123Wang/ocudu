// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_mode.h"
#include <cstdio>
#include <optional>
#include <vector>

namespace ocudu {

/// \brief One requirement the uplink pipeline mode puts on this build's data path.
///
/// A check is registered by the probe that owns the counters it needs, and evaluated once, at exit,
/// by the reporter below. Each one states a property the SELECTED pipeline mode claims, in terms of
/// what the probes measured - so the claim is verified on every run instead of being argued in a
/// design document:
///
///   * mode cpu      - the whole chain really ran on the CPU (no device counter moved);
///   * mode cpu_gpu  - the offloaded modules really ran on the device, and the host did not also do
///                     their work (the failure the S-7f-6a leg produced: device=0 with the host
///                     silently covering for it);
///   * mode gpu      - the above, plus no host pass over the samples at all.
///
/// A check whose probe is not in the path (no Metal DFT in this build, no device estimator in this
/// configuration) reports "not applicable" rather than passing: a contract with nothing to check is
/// not evidence of anything.
struct phy_pipeline_check {
  /// Short label, printed before the evidence, e.g. "dft radio inputs".
  const char* name;
  /// \brief Evaluates the requirement and prints its evidence as a line fragment (no newline).
  ///
  /// \return True when the requirement is met, false when the mode's claim is broken, or an empty
  /// optional when the requirement does not apply to this run.
  std::optional<bool> (*evaluate)();
};

/// \brief Requirements registered so far, in registration order (process-wide).
inline std::vector<phy_pipeline_check>& phy_pipeline_checks()
{
  static std::vector<phy_pipeline_check> checks;
  return checks;
}

/// \brief Evaluates every registered requirement against what the probes measured, once, at exit.
inline void report_phy_pipeline_contract()
{
  std::vector<phy_pipeline_check>& checks = phy_pipeline_checks();
  if (checks.empty()) {
    return;
  }

  const phy_pipeline_mode mode = phy_pipeline_mode_registry::get();
  unsigned                failed = 0;
  unsigned                checked = 0;

  std::fprintf(stderr, "[phy_pipeline] contract (mode=%s):\n", to_string(mode));
  for (const phy_pipeline_check& check : checks) {
    // The label comes first, then the probe prints its evidence, then the verdict: one line per
    // requirement, readable in a log without cross-referencing anything.
    std::fprintf(stderr, "[phy_pipeline]   %s: ", check.name);
    std::optional<bool> result = check.evaluate();
    if (!result.has_value()) {
      std::fprintf(stderr, " -> not applicable\n");
      continue;
    }
    ++checked;
    if (*result) {
      std::fprintf(stderr, " -> OK\n");
    } else {
      ++failed;
      std::fprintf(stderr, " -> FAILED\n");
    }
  }

  if (failed == 0) {
    std::fprintf(stderr,
                 "[phy_pipeline] contract MET (%u of %zu checks applicable)\n",
                 checked,
                 checks.size());
  } else {
    // Not an abort: the report runs at exit, and the checks that can fail today are the host passes
    // the campaign has not removed yet (see the design document's ledger). A caller that wants the
    // mode to be self-enforcing checks this at startup - which is where an abort belongs.
    std::fprintf(stderr,
                 "[phy_pipeline] contract NOT MET: %u of %u applicable checks failed (mode=%s)\n",
                 failed,
                 checked,
                 to_string(mode));
  }
}

/// \brief Registers a requirement, and - the first time - the at-exit reporter that evaluates them.
inline void register_phy_pipeline_check(const phy_pipeline_check& check)
{
  phy_pipeline_checks().push_back(check);
  static const bool reporter_registered = []() {
    std::atexit(report_phy_pipeline_contract);
    return true;
  }();
  (void)reporter_registered;
}

} // namespace ocudu
