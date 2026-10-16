// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/signal_handling.h"
#include "fmt/core.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <unistd.h>

using namespace ocudu;

/// Termination timeout value measured in seconds.
static constexpr unsigned TERMINATION_TIMEOUT_S =
#ifndef TERM_TIMEOUT_S
    5;
#else
    TERM_TIMEOUT_S;
#endif

/// How long after the first signal a later one is still read as a DUPLICATE rather than as a deliberate second
/// press. See the force path below: Ctrl+C reaches a process twice in the ordinary runner setup, and treating that
/// duplicate as "the operator pressed again" SIGKILLs a shutdown that was working.
static constexpr auto FORCE_GRACE_S = std::chrono::seconds{2};

/// When the first signal was handled, in nanoseconds on the steady clock. Zero means "no stop has begun".
static std::atomic<int64_t> stop_began_ns{0};

static int64_t steady_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// Handler called after the user interrupts the program.
static std::atomic<ocudu_signal_handler> interrupt_handler = nullptr;
/// Handler called just before forcing application exit.
static std::atomic<ocudu_signal_handler> cleanup_handler = nullptr;

static void signal_handler(int signal)
{
  switch (signal) {
    case SIGALRM:
      fmt::print(stderr, "Could not stop application after {} seconds. Forcing exit.\n", TERMINATION_TIMEOUT_S);
      if (auto handler = cleanup_handler.exchange(nullptr)) {
        handler(signal);
      }
      std::raise(SIGKILL);
      [[fallthrough]];
    default:
      // The first registered signal tries to stop the application gracefully, and arms a timer so that a stop that
      // hangs still ends: the timer is the only thing that used to be able to do it.
      if (auto handler = interrupt_handler.exchange(nullptr)) {
        stop_began_ns.store(steady_ns(), std::memory_order_relaxed);
        handler(signal);
        ::alarm(TERMINATION_TIMEOUT_S);
        return;
      }
      // A DELIBERATE SECOND SIGNAL FORCES THE EXIT -- and a duplicate one must not. Measured 2026-10-16, twice, in
      // opposite directions, and both readings matter here:
      //
      //  * an operator whose Ctrl+C did not stop the gNB pressed it again, and the second press did NOTHING: the
      //    graceful handler had already been consumed above, so the only escape left was the timer;
      //  * and once that was fixed, a NORMAL Ctrl+C started killing working shutdowns. The runner starts the gNB in
      //    its own background job but in the SAME process group, so the terminal delivers SIGINT to both the runner
      //    and the gNB, and the runner's own trap sends it a second one -- two signals, microseconds apart, from ONE
      //    key press. The leg `aillr-256qam-ai_1016_1804` was killed that way: the report block was truncated and
      //    the runner printed "THIS LEG HAS NO COMPLETE REPORT".
      //
      // So the force path needs the later signal to be at least FORCE_GRACE_S after the first. Within that window a
      // signal is a duplicate and is ignored (the graceful stop is running and will be given its time); after it,
      // the operator has waited, seen nothing happen, and pressed again -- which is exactly the escape that must
      // work. SIGTERM, SIGHUP and SIGINT all take this path, so `kill <pid>` remains a real escape too.
      if ((steady_ns() - stop_began_ns.load(std::memory_order_relaxed)) <
          std::chrono::duration_cast<std::chrono::nanoseconds>(FORCE_GRACE_S).count()) {
        fmt::print(stderr,
                   "Ignoring a duplicate interrupt that arrived within {} s of the first: the graceful stop is "
                   "still running. Press again (or kill) if it stays stuck.\n",
                   FORCE_GRACE_S.count());
        return;
      }
      fmt::print(stderr, "Interrupted again while stopping. Forcing exit.\n");
      if (auto handler = cleanup_handler.exchange(nullptr)) {
        handler(signal);
      }
      std::raise(SIGKILL);
      return;
  }
}

void ocudu::register_interrupt_signal_handler(ocudu_signal_handler handler)
{
  interrupt_handler.store(handler);

  // INSTALLED WITH SA_NODEFER, and that flag is the whole point for the operator this path exists for.
  //
  // POSIX blocks the signal being handled for the duration of its own handler, and the stop the handler performs
  // runs INSIDE the handler. With std::signal (which sets no flags) a second Ctrl+C is therefore only delivered
  // after the first one returns -- i.e. never, when the first one is the one that hangs -- and the force branch
  // below could only ever be reached by a DIFFERENT signal (SIGTERM from `kill`, whose handler is not masked, or
  // the SIGALRM timer). Measured 2026-10-16 with a unit test that hangs the graceful handler on purpose
  // (tests/unittests/support/signal_handling_test.cpp): without SA_NODEFER the second SIGINT stayed pending for
  // the whole 60-second handler and the test had to kill the child itself. An operator whose Ctrl+C did not stop
  // the gNB presses it again -- that press is the escape, so it must not be deferred.
  struct sigaction action = {};
  action.sa_handler       = signal_handler;
  action.sa_flags         = SA_NODEFER | SA_RESTART;
  sigemptyset(&action.sa_mask);
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGHUP, &action, nullptr);
  ::sigaction(SIGALRM, &action, nullptr);
  std::signal(SIGPIPE, SIG_IGN);
}

void ocudu::register_cleanup_signal_handler(ocudu_signal_handler handler)
{
  cleanup_handler.store(handler);
}
