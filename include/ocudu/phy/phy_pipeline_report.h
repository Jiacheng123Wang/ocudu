// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief On-demand P0 report dump: a leg whose process cannot stop cleanly still yields its readings.
///
/// WHY IT EXISTS (dev doc 6.24). Every P0 reading of this workflow is printed by an atexit handler, and an
/// atexit handler only runs when the process leaves through a clean exit. Leg `p14-conc2` did not: the stall
/// it was flown to catch also held the SHUTDOWN, the application's five-second stop grace expired, and the
/// logger was flushed and the process left - so the leg that mattered most produced no readings at all
/// (`[APP] [E] Emergency flush of the logger` and nothing after it). Two more (p15, p16) only produced theirs
/// because the operator waited the process out.
///
/// WHAT IT IS. A registry of the same report functions the atexit handlers run, plus `p0_dump_reports()`,
/// which runs them all NOW. The caller that matters is the receive thread PARKED on a dry buffer pool
/// (`lower_phy_baseband_processor::pop_rx_buffer_blocking()`): that park is the epicentre of every stall this
/// workflow has chased - the pool is empty because the completion that releases the input tokens is late, the
/// receive thread stops consuming the radio, and the whole slot loop stops with it. Dumping from there means
/// the readings describe the stall WHILE IT IS HAPPENING, which is something no leg has ever captured
/// (p13/p14 could only be read after it was over, and p14 not even then).
///
/// \note Rate-limited by construction and never recursive: the dump takes the same locks the reporters take,
///       from an ORDINARY THREAD - no signal handler, so no async-signal-safety hazard and no lock held across
///       the park.
/// \note The reports are printed on stderr, exactly as at exit, so `p0_gate.sh` reads them with the line it
///       already knows; the dump line itself carries the reason and the number of dumps so a reader can tell
///       a stall dump from the exit report.

#pragma once

#include <cstdint>

namespace ocudu {

/// \brief Registers a report function to be run at exit by its owner AND on demand by p0_dump_reports().
///
/// Called once per reading family, next to the `std::atexit(...)` line it joins. `fn` must be callable from
/// any thread and must not throw.
void register_p0_report(void (*fn)());

/// \brief Runs every registered report NOW, unless one ran less than \c min_interval_ms ago.
///
/// \param[in] reason Short tag printed with the dump (`stall`, `manual`), so a reader can tell which.
/// \param[in] min_interval_ms Minimum distance to the previous dump; 0 forces one.
/// \return True when a dump was printed.
bool p0_dump_reports(const char* reason, uint64_t min_interval_ms = 0);

/// How many dumps have been printed so far (diagnostics).
uint64_t nof_p0_dumps();

} // namespace ocudu
