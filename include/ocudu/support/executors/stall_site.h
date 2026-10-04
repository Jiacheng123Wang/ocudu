// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <cstdint>
#include <cstddef>

namespace ocudu {

/// \brief The blocking site a thread is inside, published so that ANOTHER thread can read it.
///
/// WHY THIS EXISTS (2026-10-04). The stall watchdog classified 13 legs of tail events and its verdicts turned out
/// not to be diagnostic: `DRIVER_BLOCK` fired whenever ANY thread of the process was parked in a wait state -
/// and an epoll thread is parked by design, so the bucket was nearly always true, with the printed name being
/// whatever `task_threads()` happened to return first (`first_blocked=io_broker_epoll`, every time). The number
/// that was supposed to localise the tail could not name a wait site at all.
///
/// WHAT IT IS. A fixed, lock-free table of "this thread is waiting HERE", keyed by the MACH THREAD ID - the same
/// identifier the watchdog already reads through THREAD_IDENTIFIER_INFO, so the two sides match without sharing
/// a pointer or a registry of our own. A thread is supposed to be inside at most a few of these at a time, so
/// the table is small, bounded, and never allocates: the reader runs while the process is in trouble.
///
/// COST, because it sits in the receive path: one thread_local lookup and two relaxed stores per scope. A scope
/// is opened around a call that BLOCKS for microseconds or more (a radio receive, a GPU completion wait), never
/// around anything hot.
///
/// It is compiled in only with the flow probes (OCUDU_FLOW_PROBES) and does nothing otherwise, so a delivery
/// build carries no table and no stores.
class stall_site
{
public:
  /// Publishes \p site as the calling thread's current blocking site.
  static void enter(const char* site);
  /// Clears it (the calling thread is running again).
  static void leave();
  /// What \p thread_id is waiting at, or nullptr when it is not inside a scope (or is not instrumented).
  static const char* of(uint64_t thread_id);

  /// \brief The LAST site \p thread_id was inside, with the instant it left it.
  ///
  /// WHY THE LAST ONE IS NEEDED. A PHY series (the receive wait, a channel-estimation segment) is reported AFTER
  /// the span it measures has ended - by which time the scope has been left, so asking "what is the thread
  /// waiting at NOW" always answers "nothing". The useful question is "what was it waiting at during the window
  /// that was slow", and that is what this answers: the caller compares \p end_ns against its own window.
  /// \return The site name, or nullptr if the thread never entered one.
  static const char* last_of(uint64_t thread_id, int64_t* end_ns);

  /// \name For the report: the sites the process is inside right now, with how many threads are at each.
  ///@{
  static constexpr size_t MAX_SITES = 16;
  struct site_count {
    const char* name = nullptr;
    unsigned    count = 0;
  };
  /// Fills \p out with up to MAX_SITES entries and returns how many were written. Never allocates.
  static size_t snapshot(site_count* out, size_t max);
  ///@}
};

/// \brief RAII scope: the calling thread is waiting at \p site until this object dies.
///
/// Every use must be a real blocking call whose duration we care about, and the name must say WHICH call it is
/// (`radio.rx`, `metal.burst_wait`, `exec.park`): the whole value of the table is that a leg can read a site
/// name and know which subsystem held the thread.
class stall_site_scope
{
public:
  explicit stall_site_scope(const char* site) { stall_site::enter(site); }
  ~stall_site_scope() { stall_site::leave(); }

  stall_site_scope(const stall_site_scope&)            = delete;
  stall_site_scope& operator=(const stall_site_scope&) = delete;
};

} // namespace ocudu
