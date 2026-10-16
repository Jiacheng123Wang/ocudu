// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/signal_handling.h"
#include "fmt/core.h"
#include <atomic>
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
        handler(signal);
        ::alarm(TERMINATION_TIMEOUT_S);
        return;
      }
      // EVERY LATER SIGNAL FORCES THE EXIT. Measured 2026-10-16: an operator whose Ctrl+C did not stop the gNB
      // pressed it again, and the second press did NOTHING - the graceful handler had already been consumed above,
      // so the only escape left was the timer, and when that did not fire either the process had to be killed from
      // another terminal. A stop that hangs is exactly the situation in which an operator presses again, so the
      // second press must be the forcible one: dump what the leg was flown for (the cleanup handler does that),
      // flush, and go. SIGTERM, SIGHUP and SIGINT all take this path, so `kill <pid>` is a real escape too.
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
