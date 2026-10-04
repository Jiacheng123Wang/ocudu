// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/stall_site.h"
#include "ocudu/support/ocudu_assert.h"

#include <atomic>

#if defined(OCUDU_FLOW_PROBES) && defined(__APPLE__)
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

using namespace ocudu;

#if defined(OCUDU_FLOW_PROBES) && defined(__APPLE__)

namespace {

/// The table. Fixed and lock-free for the reason the header gives: the reader (the watchdog) runs when the
/// process is already in trouble, and a reader that can block or allocate there is a reader that can turn a
/// stall into a hang.
constexpr size_t MAX_THREADS = 64;

int64_t now_ns()
{
  static const double ns_per_tick = [] {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    return (tb.denom != 0) ? (static_cast<double>(tb.numer) / static_cast<double>(tb.denom)) : 1.0;
  }();
  return static_cast<int64_t>(static_cast<double>(mach_absolute_time()) * ns_per_tick);
}

struct slot {
  std::atomic<uint64_t>    thread_id{0};
  std::atomic<const char*> site{nullptr};
  /// The site the thread left last, and the instant it left it (0 = never). Two more relaxed stores per scope,
  /// and they are what lets a POST-HOC series report name its own wait point.
  std::atomic<const char*> last_site{nullptr};
  std::atomic<int64_t>     last_end_ns{0};
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
    const char* site = table[i].site.load(std::memory_order_relaxed);
    if (site != nullptr) {
      table[i].last_site.store(site, std::memory_order_relaxed);
      table[i].last_end_ns.store(now_ns(), std::memory_order_relaxed);
    }
    table[i].site.store(nullptr, std::memory_order_release);
  }
}

const char* stall_site::last_of(uint64_t thread_id, int64_t* end_ns)
{
  for (size_t i = 0; i != MAX_THREADS; ++i) {
    if (table[i].thread_id.load(std::memory_order_relaxed) == thread_id) {
      if (end_ns != nullptr) {
        *end_ns = table[i].last_end_ns.load(std::memory_order_relaxed);
      }
      return table[i].last_site.load(std::memory_order_relaxed);
    }
  }
  if (end_ns != nullptr) {
    *end_ns = 0;
  }
  return nullptr;
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
const char* stall_site::last_of(uint64_t, int64_t* end_ns)
{
  if (end_ns != nullptr) {
    *end_ns = 0;
  }
  return nullptr;
}
size_t stall_site::snapshot(site_count*, size_t)
{
  return 0;
}

#endif
