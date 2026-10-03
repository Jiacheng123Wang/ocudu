// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/phy/phy_pipeline_report.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

using namespace ocudu;

namespace {

/// The registry. A fixed-size array rather than a vector: registration happens a handful of times, on the
/// threads that build the PHY, and a dump must never allocate (it runs from a thread that is already in
/// trouble).
///
/// THE CAPACITY IS A REAL LIMIT AND OVERFLOWING IT USED TO BE SILENT. It was: a loopback meant to verify the
/// paced lane's report simply printed no such line, and the reason was not the pacing but that the registry had
/// filled up and `register_p0_report` dropped the last registrations on the floor without a word. An instrument
/// that disappears quietly is worse than one that was never built, because the reading is then taken from a
/// number that is not there. The capacity is now checked out loud.
constexpr size_t          max_reports = 64;
void (*                   reports[max_reports])() = {};
std::atomic<size_t>       nof_registered{0};
std::mutex                dump_mutex;
std::atomic<uint64_t>     dump_count{0};
std::atomic<int64_t>      last_dump_ns{0};

int64_t steady_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

void ocudu::register_p0_report(void (*fn)())
{
  if (fn == nullptr) {
    return;
  }
  const size_t slot = nof_registered.fetch_add(1, std::memory_order_acq_rel);
  if (slot < max_reports) {
    reports[slot] = fn;
    return;
  }
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true, std::memory_order_relaxed)) {
    std::fprintf(stderr,
                 "[phy_pipeline] WARNING: the p0 report registry is FULL (%zu entries): the report registered "
                 "as #%zu has been DROPPED, so it will be missing from every p0 dump and every exit report. "
                 "Raise max_reports in phy_pipeline_report.cpp - an instrument that vanishes silently makes the "
                 "reading be taken from a number that is not there\n",
                 max_reports,
                 slot + 1);
  }
}

bool ocudu::p0_dump_reports(const char* reason, uint64_t min_interval_ms)
{
  // The rate limit is taken FIRST and without the lock, so a storm of calls (a parked thread looping every
  // few hundred microseconds) cannot serialize on the dump mutex.
  const int64_t now = steady_ns();
  const int64_t previous = last_dump_ns.load(std::memory_order_relaxed);
  if ((min_interval_ms != 0) && (previous != 0) &&
      ((now - previous) < static_cast<int64_t>(min_interval_ms) * 1000000)) {
    return false;
  }
  last_dump_ns.store(now, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(dump_mutex);
  const uint64_t              n = dump_count.fetch_add(1, std::memory_order_relaxed) + 1;
  std::fprintf(stderr,
               "\n[metal_stats] p0 dump #%llu (%s): the readings below are a SNAPSHOT taken while the leg was "
               "running - the same lines an exit report carries, printed because the run is in trouble (dev doc "
               "6.24). The final report, if the process exits cleanly, follows this one.\n",
               static_cast<unsigned long long>(n),
               (reason != nullptr) ? reason : "?");
  const size_t count = nof_registered.load(std::memory_order_acquire);
  for (size_t i = 0; (i != count) && (i != max_reports); ++i) {
    if (reports[i] != nullptr) {
      reports[i]();
    }
  }
  std::fflush(stderr);
  return true;
}

uint64_t ocudu::nof_p0_dumps()
{
  return dump_count.load(std::memory_order_relaxed);
}
