// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/support/executors/thread_utils.h" // cpu_relax()
#include <zmq.h>
#include <chrono>
#include <thread>

namespace ocudu {

/// \brief Darwin-friendly backoff for the ZMQ circular buffer retry loops.
///
/// A 1µs sleep_for does not actually sleep on macOS (clock granularity) and still issues a syscall per iteration:
/// retrying on a full/empty ring buffer would otherwise storm the kernel with nanosleep/psynch calls and show up as
/// system time in ps. Spin briefly with the YIELD hint instead, and only fall back to a short sleep after ~1 ms of
/// spinning.
inline void zmq_circ_buffer_backoff(unsigned& nof_spins)
{
  constexpr unsigned SPIN_BUDGET  = 1024;
  constexpr auto     SLEEP_PERIOD = std::chrono::microseconds(100);

  if (nof_spins < SPIN_BUDGET) {
    ++nof_spins;
    cpu_relax();
    return;
  }
  std::this_thread::sleep_for(SLEEP_PERIOD);
}

/// \brief Waits briefly for a socket to become readable.
///
/// Used when a non-blocking recv found no data. The upstream implementation spins at full speed, which on Linux
/// yields a reply latency of ~0.15 ms but on macOS floods the kernel with syscalls (the request/response pairs of
/// the pull loop are the hot path). Hybrid: spin briefly (cheap user-space YIELD hints, checking the socket events
/// without syscalls) to keep the wakeup latency comparable to upstream; only fall back to a 1 ms kernel poll after
/// the spin window, so an idle channel still stops burning CPU.
inline void zmq_wait_for_socket(void* sock)
{
  constexpr std::chrono::microseconds SPIN_WINDOW{200};
  constexpr long                       IDLE_POLL_TIMEOUT_MS = 1;

  auto   spin_deadline = std::chrono::steady_clock::now() + SPIN_WINDOW;
  int    events        = 0;
  size_t events_len    = sizeof(events);
  do {
    if (::zmq_getsockopt(sock, ZMQ_EVENTS, &events, &events_len) == 0 && (events & ZMQ_POLLIN)) {
      return;
    }
    cpu_relax();
  } while (std::chrono::steady_clock::now() < spin_deadline);

  ::zmq_pollitem_t items[] = {{sock, 0, ZMQ_POLLIN, 0}};
  ::zmq_poll(items, 1, IDLE_POLL_TIMEOUT_MS);
}

} // namespace ocudu
