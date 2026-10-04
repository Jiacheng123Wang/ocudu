// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/stall_site.h"
#include "ocudu/support/ocudu_assert.h"

#include <atomic>

#if defined(OCUDU_FLOW_PROBES) && defined(__APPLE__)
#include <pthread.h>
#include <mach/mach.h>
#endif

using namespace ocudu;

#if defined(OCUDU_FLOW_PROBES) && defined(__APPLE__)

namespace {

/// The table. Fixed and lock-free for the reason the header gives: the reader (the watchdog) runs when the
/// process is already in trouble, and a reader that can block or allocate there is a reader that can turn a
/// stall into a hang.
constexpr size_t MAX_THREADS = 64;

struct slot {
  std::atomic<uint64_t>    thread_id{0};
  std::atomic<const char*> site{nullptr};
};

slot table[MAX_THREADS];

/// The slot this thread uses, found once. `kNoSlot` means the table is full: the thread then simply is not
/// instrumented, which is a missing reading and never a wrong one.
constexpr int kNoSlot = -1;

int my_slot()
{
  static thread_local int slot_index = []() -> int {
    const uint64_t tid = static_cast<uint64_t>(pthread_mach_thread_np(pthread_self()));
    for (size_t i = 0; i != MAX_THREADS; ++i) {
      uint64_t expected = 0;
      if (table[i].thread_id.load(std::memory_order_relaxed) == tid) {
        return static_cast<int>(i);
      }
      if (table[i].thread_id.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
        return static_cast<int>(i);
      }
    }
    return kNoSlot;
  }();
  return slot_index;
}

} // namespace

void stall_site::enter(const char* site)
{
  const int i = my_slot();
  if (i != kNoSlot) {
    table[i].site.store(site, std::memory_order_release);
  }
}

void stall_site::leave()
{
  const int i = my_slot();
  if (i != kNoSlot) {
    table[i].site.store(nullptr, std::memory_order_release);
  }
}

const char* stall_site::of(uint64_t thread_id)
{
  for (size_t i = 0; i != MAX_THREADS; ++i) {
    if (table[i].thread_id.load(std::memory_order_relaxed) == thread_id) {
      return table[i].site.load(std::memory_order_acquire);
    }
  }
  return nullptr;
}

size_t stall_site::snapshot(site_count* out, size_t max)
{
  size_t n = 0;
  for (size_t i = 0; i != MAX_THREADS; ++i) {
    const char* site = table[i].site.load(std::memory_order_acquire);
    if (site == nullptr) {
      continue;
    }
    // Only entries WE wrote are read back, so a caller may hand in uninitialised storage: reading `out[at]`
    // for an unwritten index would be reading whatever the caller had there (and a garbage name would then be
    // compared and possibly reported as a "site").
    bool found = false;
    for (size_t at = 0; at != n; ++at) {
      if (out[at].name == site) {
        ++out[at].count;
        found = true;
        break;
      }
    }
    if (!found && (n < max)) {
      out[n].name  = site;
      out[n].count = 1;
      ++n;
    }
  }
  return n;
}

#else // not (OCUDU_FLOW_PROBES && __APPLE__)

void stall_site::enter(const char*) {}
void stall_site::leave() {}
const char* stall_site::of(uint64_t)
{
  return nullptr;
}
size_t stall_site::snapshot(site_count*, size_t)
{
  return 0;
}

#endif
