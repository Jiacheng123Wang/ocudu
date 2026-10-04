// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/ul_stall_watchdog.h"
#include "ocudu/support/executors/stall_site.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(OCUDU_FLOW_PROBES) && defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <unistd.h>

namespace ocudu {

namespace {
constexpr int64_t PERIOD_NS      = 1000000; ///< 1 ms: a 8-14.7 ms stall contains ~10 samples.
constexpr int64_t SUSPICION_NS   = 500000;  ///< file a record when the watchdog itself is this late
constexpr int     NBUCKETS       = 40;      ///< log2 buckets of lateness, in ns
constexpr size_t  WORST_KEPT     = 16;      ///< the records worth printing
constexpr int64_t STALL_FLOOR_US = 1000;    ///< a series event this slow is worth classifying
constexpr int64_t SAMPLE_GAP_NS  = 5000000; ///< at most one series-triggered sample per 5 ms

/// WHICH THREADS THE VERDICT IS ABOUT. The first version counted EVERY thread of the process, so `blocked > 0`
/// was true almost always - an epoll thread and a parked pool worker are in a wait state by design - and the
/// class it produced (`DRIVER_BLOCK`) therefore named nothing. What a stall is about is the threads that carry
/// the PHY's real-time work, so they are the only ones the verdict looks at.
bool is_watched_thread(const char* name)
{
  static const char* const PREFIXES[] = {"radio", "lower_phy", "main_pool", "pusch_lane", "lane_commit", "phy_worker"};
  for (const char* prefix : PREFIXES) {
    const size_t n = std::strlen(prefix);
    if (std::strncmp(name, prefix, n) == 0) {
      return true;
    }
  }
  return false;
}

constexpr size_t SITE_KINDS = 16; ///< distinct blocking sites the report can carry
} // namespace

struct ul_stall_watchdog::impl {
  struct record {
    int64_t     late_ns = 0;
    int         sys_busy_pct = -1;
    int         running = -1;
    int         runnable = -1;
    int         blocked = -1;
    int         watched_blocked = -1;
    int         watched_starved = -1;
    int         watched_running = -1;
    char        site[24] = {}; ///< where the first blocked WATCHED thread was waiting
    char        worst_blocked[24] = {};
    const char* verdict = "";
  };

  /// Stalls filed per blocking site - THE table this instrument exists for: a leg reads which wait point the
  /// PHY threads were inside when the tail events happened, which is the question the class counters could not
  /// answer.
  struct site_tally {
    char     name[24] = {};
    uint64_t count    = 0;
  };
  site_tally sites[SITE_KINDS];
  /// A watched thread WAS blocked but had published no scope: a blocking path that still needs instrumenting.
  /// Counted apart from the next one, because they ask for opposite actions.
  uint64_t   site_uninstrumented = 0;
  /// No watched thread was blocked at all when the stall was filed: there is no site to attribute.
  uint64_t   site_none = 0;

  std::mutex           mutex;
  std::thread          waker;
  std::atomic<bool>    started{false};
  uint64_t             ticks = 0;
  int64_t              late_max_ns = 0;
  uint64_t             buckets[NBUCKETS] = {};
  uint64_t             verdicts[5] = {}; ///< suspended / saturated / driver block / cpu stolen / work slow
  std::vector<record>  worst;
  std::atomic<int64_t> injected_late_ns{0};
  /// The probe's report() is invoked more than once per process (a loopback run printed this block twice,
  /// 5 s apart, with different tick counts). The instrument reports once: a diagnostic that repeats itself
  /// makes a reader wonder which copy is the leg's.
  std::atomic<bool>    reported{false};
  int64_t              last_sample_ns = 0;

  // host_statistics gives per-CPU cumulative ticks; the delta over the interval is millisecond-scale system
  // busyness. load1 cannot do this job (a 60 s average cannot see a 10 ms event, discipline 78).
  host_cpu_load_info_data_t prev_cpu{};
  bool                      have_prev_cpu = false;

  /// Per-thread reading. The STATE ALONE CANNOT SEPARATE THE TWO INTERESTING CASES: macOS reports
  /// TH_STATE_RUNNING both for a thread on a CPU and for one sitting on the run queue (there is no
  /// "runnable" state - the states are RUNNING / STOPPED / WAITING / UNINTERRUPTIBLE / HALTED), so
  /// "runnable but starved" and "running, work itself slow" look identical. What separates them is whether
  /// the thread's CPU ADVANCED: a thread that stayed RUNNING while its CPU did not move was never scheduled.
  struct thread_obs {
    uint64_t id = 0;
    int64_t  cpu_ns = 0;
    int32_t  state = 0;
    char     name[24] = {};
  };
  std::vector<thread_obs> prev_obs;

  static int64_t now_ns()
  {
    static const double ns_per_tick = [] {
      mach_timebase_info_data_t tb{};
      mach_timebase_info(&tb);
      return (tb.denom != 0) ? (static_cast<double>(tb.numer) / static_cast<double>(tb.denom)) : 1.0;
    }();
    return static_cast<int64_t>(static_cast<double>(mach_absolute_time()) * ns_per_tick);
  }

  static int64_t ns_from_ticks(uint64_t ticks)
  {
    static const double ns_per_tick = [] {
      mach_timebase_info_data_t tb{};
      mach_timebase_info(&tb);
      return (tb.denom != 0) ? (static_cast<double>(tb.numer) / static_cast<double>(tb.denom)) : 1.0;
    }();
    return static_cast<int64_t>(static_cast<double>(ticks) * ns_per_tick);
  }

  static uint64_t ticks_from_ns(int64_t ns)
  {
    static const double ticks_per_ns = [] {
      mach_timebase_info_data_t tb{};
      mach_timebase_info(&tb);
      return (tb.numer != 0) ? (static_cast<double>(tb.denom) / static_cast<double>(tb.numer)) : 1.0;
    }();
    return static_cast<uint64_t>(static_cast<double>(ns) * ticks_per_ns);
  }

  /// THE one place a lateness reading becomes state, shared by the real waker and the test hook - a test with
  /// its own copy of this arithmetic would test the copy (the lesson of dev doc 10.31).
  void file_tick(int64_t late_ns)
  {
    if (late_ns < 0) {
      late_ns = 0;
    }
    std::lock_guard<std::mutex> lock(mutex);
    ++ticks;
    if (late_ns > late_max_ns) {
      late_max_ns = late_ns;
    }
    unsigned bucket = 0;
    for (int64_t v = late_ns >> 1; (v != 0) && (bucket + 1 < NBUCKETS); v >>= 1) {
      ++bucket;
    }
    ++buckets[bucket];
  }

  static const char* classify(bool watchdog_late, int sys_busy_pct, int running, int runnable, int blocked)
  {
    // The decision table of plan doc D.1, in one place. `running` = threads whose CPU advanced (executing),
    // `runnable` = frozen while in state RUNNING (starved), `blocked` = frozen while WAITING/UNINTERRUPTIBLE.
    if (watchdog_late) {
      // We did not get the CPU either: either nobody was running us (suspension) or the whole machine was
      // saturated. The system's own busyness is what separates those two.
      return (sys_busy_pct >= 25) ? "SATURATED" : "SUSPENDED";
    }
    if (blocked > 0) {
      return "DRIVER_BLOCK"; // a thread with no runnable object: no priority can help it
    }
    if (runnable > 0) {
      return "CPU_STOLEN"; // the one class a time constraint is theoretically able to fix
    }
    return "WORK_SLOW"; // the threads were running: the work itself took that long
  }

  int sys_busy_pct()
  {
    host_cpu_load_info_data_t cpu{};
    mach_msg_type_number_t    count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count) !=
        KERN_SUCCESS) {
      return -1;
    }
    if (!have_prev_cpu) {
      prev_cpu      = cpu;
      have_prev_cpu = true;
      return -1;
    }
    const uint64_t busy = (cpu.cpu_ticks[CPU_STATE_USER] - prev_cpu.cpu_ticks[CPU_STATE_USER]) +
                          (cpu.cpu_ticks[CPU_STATE_SYSTEM] - prev_cpu.cpu_ticks[CPU_STATE_SYSTEM]) +
                          (cpu.cpu_ticks[CPU_STATE_NICE] - prev_cpu.cpu_ticks[CPU_STATE_NICE]);
    const uint64_t idle = cpu.cpu_ticks[CPU_STATE_IDLE] - prev_cpu.cpu_ticks[CPU_STATE_IDLE];
    prev_cpu            = cpu;
    if ((busy + idle) == 0) {
      return -1;
    }
    return static_cast<int>(100ULL * busy / (busy + idle));
  }

  /// Reads every thread of this process once: one thread_extended_info call each (it carries the run state,
  /// the name AND the CPU times) plus one thread_identifier_info for the id the next sample matches on.
  void read_threads(std::vector<thread_obs>& out)
  {
    out.clear();
    thread_act_array_t     threads   = nullptr;
    mach_msg_type_number_t n_threads = 0;
    if (task_threads(mach_task_self(), &threads, &n_threads) != KERN_SUCCESS) {
      return;
    }
    for (mach_msg_type_number_t i = 0; i != n_threads; ++i) {
      thread_extended_info_data_t info{};
      mach_msg_type_number_t      count = THREAD_EXTENDED_INFO_COUNT;
      if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
          KERN_SUCCESS) {
        continue;
      }
      thread_obs obs;
      obs.cpu_ns = static_cast<int64_t>(info.pth_user_time + info.pth_system_time);
      obs.state  = static_cast<int32_t>(info.pth_run_state);
      std::snprintf(obs.name, sizeof(obs.name), "%s", info.pth_name);
      thread_identifier_info_data_t id_info{};
      mach_msg_type_number_t        id_count = THREAD_IDENTIFIER_INFO_COUNT;
      if (thread_info(threads[i],
                      THREAD_IDENTIFIER_INFO,
                      reinterpret_cast<thread_info_t>(&id_info),
                      &id_count) == KERN_SUCCESS) {
        obs.id = id_info.thread_id;
      }
      out.push_back(obs);
      mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), n_threads * sizeof(thread_t));
  }

  /// Samples the state of our own threads. Only called on suspicion: ~48 Mach calls per sample is nothing
  /// once in a while, and an observer that ran every millisecond would not be (dev doc 2.4).
  void sample(bool watchdog_late, int64_t late_ns)
  {
    const int busy = sys_busy_pct();
    std::vector<thread_obs> cur;
    read_threads(cur);

    // How many threads that existed in BOTH samples did not accumulate CPU, split by their state. That split
    // is the answer: CPU frozen while WAITING/UNINTERRUPTIBLE is a thread with no runnable object (a driver
    // or kernel block - no priority can help it), while CPU frozen in RUNNING is a thread that was on the run
    // queue and never got a processor (the one class a time constraint is meant to fix).
    int  frozen_blocked = 0, frozen_runnable = 0, advanced = 0;
    int  w_blocked = 0, w_starved = 0, w_running = 0;
    char first_frozen[24] = {};
    char first_site[24]   = {};
    for (const thread_obs& c : cur) {
      for (const thread_obs& pv : prev_obs) {
        if ((pv.id != 0) && (pv.id == c.id)) {
          const bool ran     = (c.cpu_ns - pv.cpu_ns) > 100000; // >100 us of CPU: it ran
          const bool waiting = (c.state == TH_STATE_WAITING) || (c.state == TH_STATE_UNINTERRUPTIBLE);
          if (ran) {
            ++advanced;
          }
          else if (waiting) {
            ++frozen_blocked;
            if (first_frozen[0] == '\0') {
              std::snprintf(first_frozen, sizeof(first_frozen), "%s", c.name);
            }
          }
          else if (c.state == TH_STATE_RUNNING) {
            ++frozen_runnable;
            if (first_frozen[0] == '\0') {
              std::snprintf(first_frozen, sizeof(first_frozen), "%s", c.name);
            }
          }
          // The watched subset, and - for a watched thread frozen in a wait - WHAT IT WAS WAITING ON. This is
          // the pair the class counters could never produce: a name that is a PHY thread AND a site that is a
          // real blocking call, not "some thread of this process is parked somewhere".
          if (is_watched_thread(c.name)) {
            if (ran) {
              ++w_running;
            }
            else if (waiting) {
              ++w_blocked;
              if (first_site[0] == '\0') {
                const char* site = stall_site::of(c.id);
                // An empty string means "blocked, but the path has no scope yet" - kept DISTINCT from a named
                // site so the report can say which of the two happened instead of merging them.
                std::snprintf(first_site, sizeof(first_site), "%s", (site != nullptr) ? site : "");
              }
            }
            else if (c.state == TH_STATE_RUNNING) {
              ++w_starved;
            }
          }
          break;
        }
      }
    }
    // The FIRST sample has no previous thread reading, so the CPU deltas do not exist yet and no verdict can
    // be computed. That sample is used to establish the baseline and is NOT filed: counting it (the first
    // version labelled it UNKNOWN and let it fall into the work_slow tally) would put a reading into the
    // verdict counts that was never taken - the same mistake the -1 sentinels exist to prevent.
    if (prev_obs.empty()) {
      prev_obs = cur;
      return;
    }
    const char* verdict = classify(watchdog_late, busy, advanced, frozen_runnable, frozen_blocked);
    prev_obs            = cur;

    std::lock_guard<std::mutex> lock(mutex);
    if (std::strcmp(verdict, "SUSPENDED") == 0) {
      ++verdicts[0];
    }
    else if (std::strcmp(verdict, "SATURATED") == 0) {
      ++verdicts[1];
    }
    else if (std::strcmp(verdict, "DRIVER_BLOCK") == 0) {
      ++verdicts[2];
    }
    else if (std::strcmp(verdict, "CPU_STOLEN") == 0) {
      ++verdicts[3];
    }
    else {
      ++verdicts[4];
    }
    if ((w_blocked > 0) && (first_site[0] == '\0')) {
      ++site_uninstrumented;
    }
    else if (w_blocked == 0) {
      ++site_none;
    }
    if (first_site[0] != '\0') {
      bool tallied = false;
      for (site_tally& t : sites) {
        if ((t.name[0] != '\0') && (std::strcmp(t.name, first_site) == 0)) {
          ++t.count;
          tallied = true;
          break;
        }
      }
      if (!tallied) {
        for (site_tally& t : sites) {
          if (t.name[0] == '\0') {
            std::snprintf(t.name, sizeof(t.name), "%s", first_site);
            t.count = 1;
            tallied = true;
            break;
          }
        }
      }
      if (!tallied) {
        // More distinct sites than the report can carry: counted, never silently merged into another name.
        ++site_uninstrumented;
      }
    }
    record rec;
    rec.late_ns      = late_ns;
    rec.sys_busy_pct = busy;
    rec.running      = advanced;        // threads that did get CPU (were executing)
    rec.runnable     = frozen_runnable; // RUNNING state, no CPU: starved
    rec.blocked      = frozen_blocked;  // WAITING/UNINTERRUPTIBLE, no CPU: blocked
    rec.watched_blocked = w_blocked;
    rec.watched_starved = w_starved;
    rec.watched_running = w_running;
    std::snprintf(rec.site,
                  sizeof(rec.site),
                  "%s",
                  (first_site[0] != '\0') ? first_site : ((w_blocked > 0) ? "uninstrumented" : "-"));
    std::snprintf(rec.worst_blocked, sizeof(rec.worst_blocked), "%s", first_frozen);
    rec.verdict = verdict;
    worst.push_back(rec);
    if (worst.size() > WORST_KEPT) {
      worst.erase(worst.begin());
    }
  }

  void run()
  {
    uint64_t deadline = mach_absolute_time() + ticks_from_ns(PERIOD_NS);
    for (;;) {
      mach_wait_until(deadline);
      // ONE unit in this loop (mach ticks); the lateness is the distance past the deadline we asked for.
      const uint64_t now_tick = mach_absolute_time();
      int64_t        late_ns  = (now_tick > deadline) ? ns_from_ticks(now_tick - deadline) : 0;
      // Test hook: make the waker late on purpose, through the SAME path a real stall takes.
      const int64_t injected = injected_late_ns.exchange(0);
      if (injected != 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(injected));
        late_ns += injected;
      }
      file_tick(late_ns);
      if (late_ns >= SUSPICION_NS) {
        sample(true, late_ns);
      }
      deadline += ticks_from_ns(PERIOD_NS);
      if (deadline < mach_absolute_time()) {
        deadline = mach_absolute_time() + ticks_from_ns(PERIOD_NS);
      }
    }
  }
};

} // namespace ocudu

#else // not (OCUDU_FLOW_PROBES && __APPLE__)

namespace ocudu {
struct ul_stall_watchdog::impl {};
} // namespace ocudu

#endif

using namespace ocudu;

ul_stall_watchdog& ul_stall_watchdog::get()
{
  static ul_stall_watchdog inst;
  return inst;
}

#if defined(OCUDU_FLOW_PROBES) && defined(__APPLE__)

namespace {
bool watchdog_enabled()
{
  const char* env = std::getenv("OCUDU_UL_WATCHDOG");
  return (env != nullptr) && (env[0] != '\0') && !((env[0] == '0') && (env[1] == '\0'));
}
} // namespace

void ul_stall_watchdog::start_if_enabled()
{
  if (!watchdog_enabled()) {
    return;
  }
  if (p == nullptr) {
    p = new impl();
  }
  bool expected = false;
  if (!p->started.compare_exchange_strong(expected, true)) {
    return;
  }
  p->waker = std::thread([this]() { p->run(); });
  p->waker.detach();
}

void ul_stall_watchdog::notify_series_stall(int64_t value_us)
{
  if ((p == nullptr) || !watchdog_enabled() || (value_us < STALL_FLOOR_US)) {
    return;
  }
  const int64_t now = impl::now_ns();
  {
    std::lock_guard<std::mutex> lock(p->mutex);
    if ((now - p->last_sample_ns) < SAMPLE_GAP_NS) {
      return;
    }
    p->last_sample_ns = now;
  }
  p->sample(false, value_us * 1000);
}

void ul_stall_watchdog::report()
{
  if ((p == nullptr) || !watchdog_enabled()) {
    return;
  }
  bool expected = false;
  if (!p->reported.compare_exchange_strong(expected, true)) {
    return;
  }
  std::lock_guard<std::mutex> lock(p->mutex);
  if (p->ticks == 0) {
    return;
  }
  std::fprintf(stderr,
               "[ul_watchdog] OCUDU_UL_WATCHDOG=1: a 1 ms waker, %llu tick(s), late max=%.1fus; "
               "classified stalls: suspended=%llu saturated=%llu driver_block=%llu cpu_stolen=%llu "
               "work_slow=%llu\n",
               static_cast<unsigned long long>(p->ticks),
               static_cast<double>(p->late_max_ns) / 1000.0,
               static_cast<unsigned long long>(p->verdicts[0]),
               static_cast<unsigned long long>(p->verdicts[1]),
               static_cast<unsigned long long>(p->verdicts[2]),
               static_cast<unsigned long long>(p->verdicts[3]),
               static_cast<unsigned long long>(p->verdicts[4]));
  // THE SITE TABLE, first: which wait point the PHY threads were inside when the tail events were filed. A
  // leg reads this one line to know whether the tail is a radio receive, a GPU completion wait, an executor
  // park - or something that has no scope yet (`unknown`, counted so it cannot hide).
  std::fprintf(stderr,
               "[ul_watchdog] stall sites (watched threads: radio/lower_phy/main_pool/pusch_lane/lane_commit):");
  for (const impl::site_tally& t : p->sites) {
    if (t.name[0] != '\0') {
      std::fprintf(stderr, " %s=%llu", t.name, static_cast<unsigned long long>(t.count));
    }
  }
  std::fprintf(stderr,
               " | uninstrumented=%llu none=%llu\n",
               static_cast<unsigned long long>(p->site_uninstrumented),
               static_cast<unsigned long long>(p->site_none));
  for (const impl::record& r : p->worst) {
    std::fprintf(stderr,
                 "[ul_watchdog]   late=%7.1fus sys_busy=%3d%% watched[running=%d starved=%d blocked=%d] "
                 "site=%-14s all[running=%d runnable=%d blocked=%d] first_blocked=%-20s -> %s\n",
                 static_cast<double>(r.late_ns) / 1000.0,
                 r.sys_busy_pct,
                 r.watched_running,
                 r.watched_starved,
                 r.watched_blocked,
                 r.site,
                 r.running,
                 r.runnable,
                 r.blocked,
                 r.worst_blocked,
                 r.verdict);
  }
}

void ul_stall_watchdog::reset_for_test()
{
  if (p == nullptr) {
    p = new impl();
  }
  std::lock_guard<std::mutex> lock(p->mutex);
  p->ticks       = 0;
  p->late_max_ns = 0;
  p->worst.clear();
  std::memset(p->buckets, 0, sizeof(p->buckets));
  std::memset(p->verdicts, 0, sizeof(p->verdicts));
  for (impl::site_tally& t : p->sites) {
    t = impl::site_tally{};
  }
  p->site_uninstrumented = 0;
  p->site_none           = 0;
}

void ul_stall_watchdog::tick_for_test(int64_t lateness_ns)
{
  if (p == nullptr) {
    p = new impl();
  }
  p->file_tick(lateness_ns);
}

const char* ul_stall_watchdog::classify_for_test(bool watchdog_late, int sys_busy_pct, int n_running, int n_runnable, int n_blocked)
{
  return impl::classify(watchdog_late, sys_busy_pct, n_running, n_runnable, n_blocked);
}

bool ul_stall_watchdog::is_watched_thread_for_test(const char* thread_name)
{
  return (thread_name != nullptr) && is_watched_thread(thread_name);
}

void ul_stall_watchdog::inject_late_for_test(int64_t extra_ns)
{
  if (p == nullptr) {
    p = new impl();
  }
  p->injected_late_ns.store(extra_ns);
}

#else

void ul_stall_watchdog::start_if_enabled() {}
void ul_stall_watchdog::notify_series_stall(int64_t) {}
void ul_stall_watchdog::report() {}
void ul_stall_watchdog::reset_for_test() {}
void ul_stall_watchdog::tick_for_test(int64_t) {}
const char* ul_stall_watchdog::classify_for_test(bool, int, int, int, int) { return ""; }
void ul_stall_watchdog::inject_late_for_test(int64_t) {}
bool ul_stall_watchdog::is_watched_thread_for_test(const char*) { return false; }

#endif
