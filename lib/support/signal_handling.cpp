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

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGHUP, signal_handler);
  std::signal(SIGALRM, signal_handler);
  std::signal(SIGPIPE, SIG_IGN);
}

void ocudu::register_cleanup_signal_handler(ocudu_signal_handler handler)
{
  cleanup_handler.store(handler);
}
