// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/macos_compat.h"
#include "ocudu/support/scheduling/darwin_thread_scheduling.h" // ocudu::affinity_tag_from_cpu_mask / _from_thread_name
#include "ocudu/support/scheduling/thread_sched_snapshot.h"    // this_thread_sched_snapshot / log_this_thread_scheduling
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ocudu;

namespace {

class macos_compat_memory_test : public ::testing::Test
{
protected:
  void TearDown() override
  {
    for (void* ptr : pending) {
      compat::aligned_free(ptr);
    }
    pending.clear();
  }

  std::vector<void*> pending;
};

} // namespace

TEST_F(macos_compat_memory_test, alloc_alignment_covers_simd_and_page_sizes)
{
  // The acceptance criteria: the allocator must satisfy the SIMD alignment
  // (64-byte cache line) and both page sizes (4 KiB on x86 Linux, 16 KiB on
  // Apple Silicon macOS).
  const std::vector<size_t> alignments{64, 4096, 16384};
  const std::vector<size_t> sizes{1, 7, 123, 4096, 4097, 65536 + 13};

  for (size_t alignment : alignments) {
    for (size_t size : sizes) {
      void* ptr = compat::aligned_alloc(alignment, size);
      ASSERT_NE(ptr, nullptr) << "alignment=" << alignment << " size=" << size;
      pending.push_back(ptr);

      EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0)
          << "alignment=" << alignment << " size=" << size;

      // Touch every byte of the requested range: with ASAN enabled, any
      // under-allocation is reported as a heap-buffer-overflow here.
      std::memset(ptr, 0xAB, size);
    }
  }
}

TEST_F(macos_compat_memory_test, small_alignment_is_promoted_to_cache_line)
{
  void* ptr = compat::aligned_alloc(1, 1000);
  ASSERT_NE(ptr, nullptr);
  pending.push_back(ptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % 64, 0);
}

TEST(macos_compat_alloc_test, non_power_of_two_alignment_fails)
{
  EXPECT_EQ(compat::aligned_alloc(96, 128), nullptr);
  EXPECT_EQ(compat::aligned_alloc(192, 128), nullptr);
}

TEST_F(macos_compat_memory_test, many_blocks_do_not_overlap)
{
  constexpr size_t NOF_BLOCKS  = 256;
  constexpr size_t BLOCK_SIZE  = 64;
  constexpr size_t ALIGNMENT   = 4096;

  std::vector<void*> blocks(NOF_BLOCKS, nullptr);
  for (size_t i = 0; i != NOF_BLOCKS; ++i) {
    blocks[i] = compat::aligned_alloc(ALIGNMENT, BLOCK_SIZE);
    ASSERT_NE(blocks[i], nullptr);
    pending.push_back(blocks[i]);
    std::memset(blocks[i], static_cast<int>(i), BLOCK_SIZE);
  }

  // Every block must keep its own contents: overlapping blocks would corrupt
  // the neighbouring pattern.
  for (size_t i = 0; i != NOF_BLOCKS; ++i) {
    const auto* data = static_cast<const unsigned char*>(blocks[i]);
    for (size_t b = 0; b != BLOCK_SIZE; ++b) {
      ASSERT_EQ(data[b], static_cast<unsigned char>(i)) << "block=" << i << " byte=" << b;
    }
  }
}

TEST(macos_compat_alloc_test, page_size_is_a_power_of_two_of_at_least_4kib)
{
  const size_t ps = compat::page_size();
  EXPECT_GE(ps, 4096U);
  EXPECT_EQ(ps & (ps - 1), 0U);

#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
  // Apple Silicon macOS uses 16 KiB pages.
  EXPECT_EQ(ps, 16384U);
#endif
}

TEST(macos_compat_time_test, monotonic_clock_advances_with_sleep)
{
  const uint64_t t0 = compat::get_monotonic_time_us();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const uint64_t t1 = compat::get_monotonic_time_us();
  EXPECT_GT(t1, t0);
}

TEST(macos_compat_time_test, monotonic_clock_never_goes_backwards)
{
  uint64_t previous = compat::get_monotonic_time_us();
  for (unsigned i = 0; i != 100000; ++i) {
    const uint64_t current = compat::get_monotonic_time_us();
    EXPECT_GE(current, previous);
    previous = current;
  }
}

TEST(macos_compat_time_test, clock_rate_matches_steady_clock)
{
  // Both clocks must advance at the same wall rate (they may have different
  // epochs, so only the deltas are compared).
  const auto     c0 = std::chrono::steady_clock::now();
  const uint64_t u0 = compat::get_monotonic_time_us();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto     c1 = std::chrono::steady_clock::now();
  const uint64_t u1 = compat::get_monotonic_time_us();

  const auto   steady_us = std::chrono::duration_cast<std::chrono::microseconds>(c1 - c0).count();
  const int64_t drift_us = static_cast<int64_t>(u1 - u0) - steady_us;
  EXPECT_NEAR(drift_us, 0, 10000); // 10 ms tolerance over a 100 ms window.
}

TEST(macos_compat_cpu_test, current_cpu_returns_a_sane_index)
{
  const unsigned cpu = compat::get_current_cpu();
  // A CPU index within the first 4096 processors covers any real machine;
  // macOS reports the fixed offset 0.
  EXPECT_LT(cpu, 4096U);
}

// ==== Phase 2: thread & core scheduling ====

TEST(macos_compat_cpu_test, available_cpu_ids_are_sane)
{
  const auto ids = compat::get_available_cpu_ids();
  ASSERT_FALSE(ids.empty());
  for (size_t id : ids) {
    EXPECT_LT(id, os_sched_affinity_bitmask::MAX_CPUS);
  }

  // No duplicates and every ID ends up in the public available_cpus() mask.
  std::vector<bool> seen(os_sched_affinity_bitmask::MAX_CPUS, false);
  for (size_t id : ids) {
    ASSERT_FALSE(seen[id]) << "duplicated CPU id " << id;
    seen[id] = true;
    EXPECT_TRUE(os_sched_affinity_bitmask::available_cpus().test(id));
  }
  EXPECT_EQ(os_sched_affinity_bitmask::available_cpus().count(), ids.size());
}

TEST(macos_compat_sched_test, affinity_tag_from_cpu_mask_follows_the_rules)
{
  // Empty mask: no hint.
  os_sched_affinity_bitmask empty;
  EXPECT_EQ(affinity_tag_from_cpu_mask(empty), 0);

  // Up to 4 CPUs: lowest CPU index + 1.
  os_sched_affinity_bitmask one_cpu(1);
  EXPECT_EQ(affinity_tag_from_cpu_mask(one_cpu), 2);

  os_sched_affinity_bitmask few_cpus;
  few_cpus.set(2);
  few_cpus.set(3);
  few_cpus.set(4);
  EXPECT_EQ(affinity_tag_from_cpu_mask(few_cpus), 3);

  // Wider than 4 CPUs: "any of these cores", no co-location intent.
  os_sched_affinity_bitmask wide;
  for (unsigned i = 0; i != 5; ++i) {
    wide.set(i);
  }
  EXPECT_EQ(affinity_tag_from_cpu_mask(wide), 0);
}

TEST(macos_compat_sched_test, affinity_tag_from_thread_name_is_stable_and_pool_wise)
{
  const int tag = affinity_tag_from_thread_name("main_pool#2");
  EXPECT_GE(tag, 1);
  EXPECT_LE(tag, 4095);
  EXPECT_EQ(tag, affinity_tag_from_thread_name("main_pool#2"));
  // All workers of a pool share one tag (the "#<idx>" suffix is stripped).
  EXPECT_EQ(tag, affinity_tag_from_thread_name("main_pool#0"));
  EXPECT_EQ(tag, affinity_tag_from_thread_name("main_pool"));
}

TEST(macos_compat_sched_test, set_thread_name_roundtrip)
{
  std::string read_back;
  std::thread t([&read_back]() {
    ASSERT_TRUE(compat::set_thread_name(::pthread_self(), "compat_name_t"));
    char buf[64] = {};
    ASSERT_EQ(::pthread_getname_np(::pthread_self(), buf, sizeof(buf)), 0);
    read_back = buf;
  });
  t.join();
  EXPECT_EQ(read_back, "compat_name_t");
}

TEST(macos_compat_sched_test, configure_worker_thread_attributes_stack_size)
{
  ::pthread_attr_t attr;
  ::pthread_attr_init(&attr);
  compat::configure_worker_thread_attributes(attr);
  size_t stack_size = 0;
  ASSERT_EQ(::pthread_attr_getstacksize(&attr, &stack_size), 0);
#if defined(__APPLE__)
  // The compat layer enlarges the macOS default (512 KiB) to 16 MiB.
  EXPECT_EQ(stack_size, 16U * 1024U * 1024U);
#else
  // Linux keeps its default; just make sure the call did not corrupt the attr.
  EXPECT_GT(stack_size, 0);
#endif
  ::pthread_attr_destroy(&attr);
}

TEST(macos_compat_sched_test, apply_worker_thread_scheduling_smoke)
{
  // Non-real-time intent on the calling thread: must not crash on either
  // platform (Linux no-op, macOS QoS USER_INITIATED + affinity tag).
  compat::apply_worker_thread_scheduling(os_thread_realtime_priority::no_realtime(), {}, "compat_smoke_test");

  // Real-time intent on a short-lived thread. NOTE (2026-10-01): this no longer requests a Mach time constraint -
  // that was the 2026-09-01 arm, reverted, and it is now reachable only through OCUDU_SCHED_TIME_CONSTRAINT
  // (tested below). What the call does here is request the QoS class and the affinity tag.
  std::thread rt_thread([]() {
    compat::apply_worker_thread_scheduling(os_thread_realtime_priority::max(), {}, "compat_rt_smoke");
  });
  rt_thread.join();
}

// The P4 arm (dev doc 10.29): OCUDU_SCHED_TIME_CONSTRAINT decides WHICH worker gets a Mach time constraint, and
// the parameters come from the environment - so one binary flies both arms.
//
// Every case runs on its OWN thread. That is not stylistic: a time constraint is a property of the thread, it
// cannot be undone, and applying one ERASES the QoS class of that thread for good (pthread_set_qos_class_self_np
// then returns EPERM). A test that reused one thread would therefore report the previous case's arm.
TEST(macos_compat_sched_test, time_constraint_env_knob)
{
  struct tc_case_result {
    bool    constrained = false;
    int64_t period_us   = -1;
    double  duty        = 0.0;
    int32_t qos_class   = -1;
  };

  // spec == nullptr means "the knob is unset". rt flags a real-time scheduling intent, which the blanket arm
  // ("1"/"default") requires - it reproduces 2026-09-01 including its scope.
  const auto run_case = [](const char* spec, const char* thread_name, bool rt) {
    if (spec == nullptr) {
      ::unsetenv("OCUDU_SCHED_TIME_CONSTRAINT");
    }
    else {
      ::setenv("OCUDU_SCHED_TIME_CONSTRAINT", spec, 1);
    }
    tc_case_result res{};
    std::thread    t([&]() {
      compat::apply_worker_thread_scheduling(
          rt ? os_thread_realtime_priority::max() : os_thread_realtime_priority::no_realtime(), {}, thread_name);
      const thread_sched_snapshot snap = this_thread_sched_snapshot();
      res.constrained                  = snap.time_constrained();
      res.period_us                    = snap.tc_period_ns / 1000;
      res.duty                         = snap.declared_duty();
      res.qos_class                    = snap.qos_class;
    });
    t.join();
    return res;
  };

  // The default arm: no knob, no constraint. (This is the reverse arm of every assertion below: if the readback
  // could not tell "no constraint" from "constraint", the whole switch would be untestable.)
  {
    const tc_case_result r = run_case(nullptr, "tc_case_off", true);
    EXPECT_FALSE(r.constrained) << "the knob is unset: no thread may be put under a time constraint";
#if defined(__APPLE__)
    EXPECT_EQ(r.period_us, 0) << "a successful readback of 'no constraint' must be 0, not -1 (which means n/a)";
#else
    EXPECT_EQ(r.period_us, -1)
        << "Linux has no such notion, so the field must stay at its 'not applicable' value - a 0 here would claim a "
           "reading this platform never took (the rule the whole struct follows)";
#endif
  }

#if !defined(__APPLE__)
  // Linux has no such mechanism. The knob must be inert, and the readback must refuse to invent one (-1 = the
  // platform has no such notion), which is this project's rule that "no reading" is never printed as "zero".
  {
    const tc_case_result r = run_case("1", "tc_case_linux", true);
    EXPECT_FALSE(r.constrained) << "Linux must not apply a Mach time constraint";
    EXPECT_EQ(r.period_us, -1) << "Linux has no time-constraint readback: it must report -1, not 0";
  }
#else
  // (1) An exact NAME selects exactly one thread.
  {
    const tc_case_result selected = run_case("tc_case_exact=500/200/400", "tc_case_exact", true);
    EXPECT_TRUE(selected.constrained);
    EXPECT_EQ(selected.period_us, 500);
    EXPECT_DOUBLE_EQ(selected.duty, 0.4);

    // ...and does NOT select another one. This is the arm the single-thread experiment depends on: if the knob
    // leaked to every worker we would be flying the 2026-09-01 blanket arm without saying so.
    const tc_case_result other = run_case("tc_case_exact=500/200/400", "tc_case_other", true);
    EXPECT_FALSE(other.constrained) << "an entry that names another thread must not constrain this one";
    // The un-constrained thread is the one that keeps its QoS class - the measured price of the arm above.
    EXPECT_EQ(selected.qos_class, static_cast<int32_t>(QOS_CLASS_UNSPECIFIED))
        << "applying a Mach time constraint must erase the QoS class (measured 2026-10-01)";
    EXPECT_EQ(other.qos_class, static_cast<int32_t>(darwin_qos_class_for_prio(os_thread_realtime_priority::max())))
        << "a thread the knob did not select must keep the class it was given";
  }

  // (2) "*" is the blanket arm, and an exact NAME beats it in either order (a default must not shadow a thread's
  // own parameters, whatever the order in the string).
  {
    const tc_case_result any = run_case("*=500/200/400", "tc_case_any", true);
    EXPECT_TRUE(any.constrained);
    EXPECT_EQ(any.period_us, 500);

    const tc_case_result exact_first = run_case("tc_case_spec=800/100/200;*=500/200/400", "tc_case_spec", true);
    const tc_case_result exact_last  = run_case("*=500/200/400;tc_case_spec=800/100/200", "tc_case_spec", true);
    EXPECT_EQ(exact_first.period_us, 800) << "an exact match must win over '*'";
    EXPECT_EQ(exact_last.period_us, 800) << "an exact match must win over '*' regardless of the order";
  }

  // (3) "1"/"default" is the 2026-09-01 arm in one word - including its scope (real-time intent only).
  {
    const tc_case_result rt_worker = run_case("1", "tc_case_blanket_rt", true);
    EXPECT_TRUE(rt_worker.constrained);
    EXPECT_EQ(rt_worker.period_us, 1000);
    EXPECT_DOUBLE_EQ(rt_worker.duty, 1.0) << "'1' must reproduce the historical shape, 100% duty included";

    const tc_case_result non_rt = run_case("1", "tc_case_blanket_non_rt", false);
    EXPECT_FALSE(non_rt.constrained) << "the blanket arm must not reach threads without a real-time intent";
  }

  // (4) Malformed specifications are REJECTED, never half-applied. The first one is the only shape measured to be
  // catastrophic (dev doc 10.29(2)); the others are ill-formed or contradict themselves.
  {
    const tc_case_result bad_order = run_case("tc_case_bad=500/400/100", "tc_case_bad", true);
    EXPECT_FALSE(bad_order.constrained) << "constraint < computation was measured harmful: it must be refused";

    const tc_case_result beyond_period = run_case("tc_case_bad2=500/300/600", "tc_case_bad2", true);
    EXPECT_FALSE(beyond_period.constrained) << "a deadline beyond its own period must be refused";

    const tc_case_result no_equals = run_case("tc_case_bad3-500/100/200", "tc_case_bad3", true);
    EXPECT_FALSE(no_equals.constrained) << "an entry without '=' must be refused";

    const tc_case_result unparsable = run_case("tc_case_bad4=500/100", "tc_case_bad4", true);
    EXPECT_FALSE(unparsable.constrained) << "an entry without all three values must be refused";

    const tc_case_result zero = run_case("tc_case_bad5=0/100/200", "tc_case_bad5", true);
    EXPECT_FALSE(zero.constrained) << "a zero period must be refused";
  }

  // (5) The leg-visible `[sched]` line must be able to show BOTH states of the arm: `tc=none` for a thread the
  // knob did not select, and the parameters for one it did. A field that can only ever print one of the two is
  // not an instrument - this is the reverse-arm rule every new readback field in this project has to pass.
  {
    const auto capture_line = [](const char* spec, const char* name) {
      if (spec == nullptr) {
        ::unsetenv("OCUDU_SCHED_TIME_CONSTRAINT");
      }
      else {
        ::setenv("OCUDU_SCHED_TIME_CONSTRAINT", spec, 1);
      }
      std::string line;
      std::thread t([&]() {
        compat::apply_worker_thread_scheduling(os_thread_realtime_priority::max(), {}, name);
        FILE* capture = std::tmpfile();
        std::fflush(stderr);
        const int saved = dup(fileno(stderr));
        dup2(fileno(capture), fileno(stderr));
        ::setenv("OCUDU_SCHED_VERBOSE", "1", 1);
        log_this_thread_scheduling(os_thread_realtime_priority::max(), name);
        std::fflush(stderr);
        dup2(saved, fileno(stderr));
        close(saved);
        ::unsetenv("OCUDU_SCHED_VERBOSE");
        std::rewind(capture);
        char   buf[512];
        size_t nof_read = 0;
        while ((nof_read = std::fread(buf, 1, sizeof(buf), capture)) > 0) {
          line.append(buf, nof_read);
        }
        std::fclose(capture);
      });
      t.join();
      return line;
    };
#if defined(OCUDU_FLOW_PROBES)
    const std::string unselected = capture_line(nullptr, "tc_line_off");
    const std::string selected   = capture_line("tc_line_on=500/200/400", "tc_line_on");
    EXPECT_NE(unselected.find(" tc=none "), std::string::npos)
        << "a thread with no constraint must say so, not print nothing: " << unselected;
    EXPECT_NE(selected.find(" tc=500/200/400us(duty=40%) "), std::string::npos)
        << "a constrained thread must report the parameters it is under: " << selected;
    // The two fields have to agree with each other: a thread under a constraint cannot be in a QoS class.
    EXPECT_NE(selected.find("eff=UNSPECIFIED"), std::string::npos)
        << "the readback must show the erased class next to the constraint: " << selected;
    EXPECT_NE(unselected.find("eff=USER_INTERACTIVE"), std::string::npos)
        << "control: the unconstrained thread keeps its class: " << unselected;
#endif
  }
#endif

  ::unsetenv("OCUDU_SCHED_TIME_CONSTRAINT");
}

TEST(macos_compat_sched_test, radio_worker_realtime_priority_platform_contract)
{
#if defined(__APPLE__)
  // The radio channel loop is elevated to the real-time QoS class.
  EXPECT_NE(compat::radio_worker_realtime_priority(), os_thread_realtime_priority::no_realtime());
  EXPECT_EQ(compat::radio_worker_realtime_priority(), os_thread_realtime_priority::max() - 1);
#else
  // Linux keeps the upstream non-realtime priority.
  EXPECT_EQ(compat::radio_worker_realtime_priority(), os_thread_realtime_priority::no_realtime());
#endif
}

TEST(macos_compat_sched_test, set_thread_affinity_with_an_available_cpu)
{
  // Pinning the calling thread to an available CPU must succeed on Linux
  // (pthread_setaffinity_np) and is a trivially-true no-op on macOS.
  const auto ids = compat::get_available_cpu_ids();
  ASSERT_FALSE(ids.empty());
  os_sched_affinity_bitmask mask(ids.front());
  EXPECT_TRUE(compat::set_thread_affinity(::pthread_self(), mask, "compat_affinity_test"));
}

TEST(macos_compat_sched_test, bind_and_realtime_priority_entry_points_do_not_crash)
{
  // The public entry points of the spec (utils/macos_compat.h): both are
  // no-ops on Linux and apply the QoS / Mach policies on macOS.
  compat::set_thread_realtime_priority();
  compat::bind_thread_to_performance_core();
}

/// \brief P1: the per-thread scheduling reading is real on both platforms, and its FIELDS obey the "no reading is
///        not zero" rule where a platform cannot provide them.
///
/// WHY IT IS TESTED HERE AND NOT ONLY THROUGH A LEG. This reading is what separates the two owners of a stall
/// ("this thread lost the core" vs "the process lost the core"), and it is the first per-thread CPU reading this
/// project has had - every earlier instrument asked getrusage(RUSAGE_SELF) and therefore described the process.
/// A field that silently reads 0 on one platform would make a leg's report say "this thread got no CPU", which is
/// the strongest possible claim of host interference, so the unavailable fields are asserted to be -1 (macOS has
/// no per-thread context-switch counter at all; the SDK was checked).
TEST(macos_compat_sched_test, thread_sched_snapshot_is_a_per_thread_reading)
{
  const thread_sched_snapshot before = this_thread_sched_snapshot();
  ASSERT_TRUE(before.valid()) << "both platforms expose the calling thread's CPU time (Mach / RUSAGE_THREAD)";
  EXPECT_GT(before.wall_ns, 0);
  EXPECT_NE(before.thread_id, 0u) << "an unidentifiable snapshot cannot be compared with another one";

  // Burn CPU on THIS thread and read again: the counter must move, and by an amount bounded by the wall time that
  // passed (a thread cannot consume more CPU than the interval it ran in - with slack, because the interval is
  // measured by two separate clock reads and the machine may be fast).
  volatile double sink = 0.0;
  for (int i = 0; i != 5000000; ++i) {
    sink += static_cast<double>(i) * 1e-9;
  }
  (void)sink;
  const thread_sched_snapshot after = this_thread_sched_snapshot();
  ASSERT_TRUE(after.valid());
  EXPECT_EQ(after.thread_id, before.thread_id) << "the same thread must read back the same id";
  EXPECT_GT(after.cpu_ns, before.cpu_ns) << "a busy thread's CPU time must increase";
  EXPECT_LT(after.cpu_ns - before.cpu_ns, after.wall_ns - before.wall_ns + 50000000LL)
      << "a thread cannot burn more CPU than the wall time it had";

  // ... and a DIFFERENT thread has its own counter, which is the property the probe's `tcpu=` depends on.
  int64_t other_cpu = -1;
  std::thread other([&other_cpu]() {
    other_cpu = this_thread_sched_snapshot().cpu_ns;
  });
  other.join();
  EXPECT_GE(other_cpu, 0);

#if defined(__APPLE__)
  // macOS: the QoS class is readable (that is the open question of high level 4), and the context-switch counters
  // are NOT available on any Mach thread-info flavor - reported as -1, never as 0.
  EXPECT_GE(this_thread_sched_snapshot().qos_class, static_cast<int32_t>(QOS_CLASS_UNSPECIFIED));
  EXPECT_EQ(before.nvcsw, -1) << "macOS exposes no per-thread voluntary switch count; -1 is 'no reading'";
  EXPECT_EQ(before.ivcsw, -1) << "macOS exposes no per-thread involuntary switch count; -1 is 'no reading'";
#else
  // Linux: RUSAGE_THREAD carries both, and gettid() identifies the thread.
  EXPECT_GE(before.nvcsw, 0);
  EXPECT_GE(before.ivcsw, 0);
  EXPECT_EQ(before.qos_class, -1) << "Linux has no QoS class notion; -1 is 'not applicable'";
#endif
  // The readback names the class it found - the string is what a leg's `[sched]` line prints.
  EXPECT_STRNE(qos_class_name(-2), nullptr);
}

/// \brief P1: the `[sched]` self-read answers "did the QoS class we requested actually take effect?".
///
/// THE OPEN QUESTION IT CLOSES (high level 4). Every worker this project creates asks for a QoS class and a POSIX
/// priority, and until this instrument nothing ever read back what the kernel granted. `taskinfo` on the reference
/// leg showed USER_INTERACTIVE/USER_INITIATED billing of 0.000 s with an effective ceiling of THREAD_QOS_LEGACY -
/// which is consistent with "the request was clamped" AND with three other stories (the billing is per-run
/// accounting, not per-class; the class was granted but the thread never ran under it; the process-level boost is
/// what put 99.55% of the time on the P cores). Only a readback taken inside the thread separates them.
///
/// ARMS: with the variable unset the function must print NOTHING on either platform (the byte-identical half);
/// with it set it must print one line naming the thread and carrying both the REQUESTED and the EFFECTIVE class,
/// so a clamp shows up as a difference between two fields rather than as an absence.
TEST(macos_compat_sched_test, sched_self_read_is_env_gated_and_reports_requested_vs_effective)
{
  const auto capture_line = [](const os_thread_realtime_priority& prio) {
    FILE* capture = std::tmpfile();
    EXPECT_NE(capture, nullptr);
    if (capture == nullptr) {
      return std::string();
    }
    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    dup2(fileno(capture), fileno(stderr));
    log_this_thread_scheduling(prio, "compat_sched_self");
    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    std::rewind(capture);
    std::string out;
    char        buf[512];
    size_t      nof_read = 0;
    while ((nof_read = std::fread(buf, 1, sizeof(buf), capture)) > 0) {
      out.append(buf, nof_read);
    }
    std::fclose(capture);
    return out;
  };

  ::unsetenv("OCUDU_SCHED_VERBOSE");
  EXPECT_TRUE(capture_line(os_thread_realtime_priority::max()).empty())
      << "with the variable unset the readback must print nothing at all (the byte-identical half of the "
         "two-key contract)";

  ::setenv("OCUDU_SCHED_VERBOSE", "1", 1);
  const std::string line = capture_line(os_thread_realtime_priority::max());
  ::unsetenv("OCUDU_SCHED_VERBOSE");
#if defined(OCUDU_FLOW_PROBES)
  EXPECT_NE(line.find("[sched] thread=compat_sched_self id="), std::string::npos) << line;
  EXPECT_NE(line.find(" rt_intent=1 "), std::string::npos) << line;
  // Both halves of the question, on one line: what we asked for and what we got.
  EXPECT_NE(line.find(" req="), std::string::npos) << line;
  EXPECT_NE(line.find(" eff="), std::string::npos) << line;
  EXPECT_NE(line.find(" posix="), std::string::npos) << line;
#if defined(__APPLE__)
  EXPECT_NE(line.find("req=USER_INTERACTIVE"), std::string::npos)
      << "a real-time intent maps to the interactive class on macOS: " << line;
  EXPECT_EQ(line.find("eff=n/a"), std::string::npos)
      << "macOS must report the EFFECTIVE class it read back, not 'n/a': " << line;
#else
  // Linux has no QoS notion and enforces the POSIX priority instead; the readback says so rather than inventing a
  // class, and the priority it prints is the one the thread wrapper applied.
  EXPECT_NE(line.find("req=posix"), std::string::npos) << line;
  EXPECT_NE(line.find("eff=n/a"), std::string::npos) << line;
#endif
#else
  // ENABLE_FLOW_PROBES=OFF: the instrument is not compiled in at all, so even with the variable set there is no
  // line - the compile-time key, checked here rather than assumed.
  EXPECT_TRUE(line.empty()) << "built without ENABLE_FLOW_PROBES, the readback must not exist: " << line;
#endif
}

/// \brief P1/P3: the attributes carry the requested QoS class when (and only when) OCUDU_SCHED_ATTR_QOS asks.
///
/// The switch exists because declaring the class at creation is a BEHAVIOUR CHANGE on macOS (the thread starts on
/// the requested class instead of acquiring it a moment later) and the historical port never did it. The two arms
/// are therefore the point: with the variable unset the attribute must be untouched, and with it set the class
/// must be on the attribute - which is what makes the A/B possible on ONE binary.
///
/// Linux: the call must be a no-op that does not even look at the variable, so this test asserts that the
/// attributes are unchanged in BOTH arms there.
TEST(macos_compat_sched_test, attr_qos_is_opt_in_and_platform_gated)
{
  const auto read_attr_qos = [](::pthread_attr_t& attr) {
#if defined(__APPLE__)
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int         rel = 0;
    if (::pthread_attr_get_qos_class_np(&attr, &qos, &rel) != 0) {
      return -1;
    }
    return static_cast<int>(qos);
#else
    // Linux: there is no attribute QoS to read, and THIS ARM HAS TO COMPILE there. The first version declared
    // `qos_class_t qos` above the #if and the Ubuntu bench refused it (2026-10-01, dev doc 10.10) - the type
    // exists on Darwin only. That is the whole reason the Linux check is run against the same commit.
    (void)attr;
    return -1;
#endif
  };

  {
    ::unsetenv("OCUDU_SCHED_ATTR_QOS");
    ::pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    const int before = read_attr_qos(attr);
    compat::configure_worker_thread_attributes_qos(attr, os_thread_realtime_priority::max());
    EXPECT_EQ(read_attr_qos(attr), before) << "with the variable unset the attributes must not change";
    ::pthread_attr_destroy(&attr);
  }
#if defined(__APPLE__)
  {
    ::setenv("OCUDU_SCHED_ATTR_QOS", "1", 1);
    ::pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    compat::configure_worker_thread_attributes_qos(attr, os_thread_realtime_priority::max());
    EXPECT_EQ(read_attr_qos(attr), static_cast<int>(QOS_CLASS_USER_INTERACTIVE))
        << "with the variable set, a real-time worker must carry the interactive class on its attributes";
    ::pthread_attr_destroy(&attr);
    ::unsetenv("OCUDU_SCHED_ATTR_QOS");
  }
#endif
}
