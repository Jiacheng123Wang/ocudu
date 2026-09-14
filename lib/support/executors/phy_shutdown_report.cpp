// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/phy_shutdown_report.h"

#include <mutex>
#include <vector>

using namespace ocudu;

namespace {

/// Function-local static: a probe may register from a static initializer, so the registry must not
/// depend on the initialization order of translation units.
std::vector<phy_shutdown_report::report_fn>& registry()
{
  static std::vector<phy_shutdown_report::report_fn> entries;
  return entries;
}

std::mutex& registry_mutex()
{
  static std::mutex m;
  return m;
}

} // namespace

void phy_shutdown_report::add(report_fn fn)
{
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().push_back(std::move(fn));
}

void phy_shutdown_report::run_all()
{
  // Copy first: a summary must not observe a registry that grows while it runs.
  std::vector<report_fn> entries;
  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    entries = registry();
  }
  for (report_fn& fn : entries) {
    if (fn) {
      fn();
    }
  }
}
