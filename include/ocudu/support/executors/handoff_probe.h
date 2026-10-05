// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Measures the INTER-THREAD HAND-OFF: from the instant a producer hands a task to an executor to the
///        instant the consumer starts running it.
///
/// WHY IT EXISTS (dev doc 11.48). Every reading this workflow had measured a thread's *span* - `t2f`, `ce`, `eqd`,
/// `defer_wait` - or the sum of them inside a slot (`[ul_pipeline]`). None of them measured the interval BETWEEN two
/// threads, and that interval is the one that decides how a periodic pipeline is declared: a thread that must act
/// within a slot has to wake early enough to absorb the uncertainty of (i) its own wake-up and (ii) the producer's
/// hand-over. Without the number the only honest answer to "how early?" was "unknown", and the window was set by
/// guess. The measurement is the whole point: `lead` for the upstream jitter, a spin window for this one.
///
/// WHAT IT MEASURES. The producer calls handoff_probe_now_ns() and passes the value with the task (a plain uint64_t
/// capture - no plumbing, no wrapper type). The consumer's FIRST statement calls handoff_probe_note(), which reads the
/// clock again and books the difference. So the sample covers exactly: queue push + whatever wait policy the consumer
/// uses (a sleep-poll phase, or a condition-variable wake) + the scheduling of the consumer thread.
///
/// The HISTOGRAM, not just the percentiles, is the reading that matters: a consumer that polls every 10 us shows a
/// roughly flat 0-10 us distribution whose width IS the poll period, while a consumer that is woken by a push shows a
/// spike of a few microseconds - the two are indistinguishable in p50 alone, and they call for opposite designs.
///
/// TWO-KEY CONTRACT (same as every other probe of this workflow). Compiled in whenever this port is built, and gated
/// at run time by `OCUDU_UL_HANDOFF_PROBE`; with the variable unset, handoff_probe_now_ns() returns 0, the callers
/// capture 0 and skip the note, and no clock is read on the hot path.
///
/// \note The clock is a monotonic host counter (mach_absolute_time() on macOS), never adjusted, so the differences are
///       meaningful to the tick (41.67 ns on this host); it is NOT the radio clock.

#pragma once

#include <cstdint>

namespace ocudu {

/// \brief The hand-off sites this workflow measures. One tag per executor boundary that a slot crosses.
enum class handoff_site : unsigned {
  /// lower_phy_rx#0 -> lower_phy_ul#0: the uplink block handed to the uplink processor (see
  /// lower_phy_baseband_processor, the uplink_executor.defer() call). This is the hand-off that gates `t2f`.
  rx_to_ul = 0,
  /// lower_phy_ul#0 -> the UL lane executor (the pool): the PUSCH processing handed to the worker pool.
  ul_to_lane = 1,
  /// Number of sites; keep last.
  nof_sites = 2,
};

/// \brief True when the probe was switched on by OCUDU_UL_HANDOFF_PROBE (read once, cached).
///
/// The caller uses it to register handoff_probe_report() on its own exit paths (this layer cannot: the exit-report
/// registry belongs to the PHY layer, and a support-layer probe must not depend on it).
bool handoff_probe_enabled();

/// \brief Monotonic nanosecond timestamp for a hand-off, or 0 when the probe is disabled.
///
/// Call it once, just before handing the task over, and pass the value into the task. A 0 return means "do not
/// measure this one" (and costs one predictable branch - no clock read).
uint64_t handoff_probe_now_ns();

/// \brief Books a hand-off whose push instant was \p pushed_ns. Called as the consumer's first statement.
///
/// \param[in] site      Which boundary this sample belongs to.
/// \param[in] pushed_ns The value handoff_probe_now_ns() returned in the producer (0 disables the sample).
void handoff_probe_note(handoff_site site, uint64_t pushed_ns);

/// \brief Prints the hand-off readings (count, percentiles, max, and the coarse histogram).
///
/// Printed nothing when no sample was taken. Register it at the call site with register_exit_report() so the readings
/// appear on BOTH exit paths, exactly as the rest of this workflow's readings do.
void handoff_probe_report();

} // namespace ocudu
