// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <functional>

namespace ocudu {
namespace phy_shutdown_report {

/// \brief Run at the end of the application, while the logging system is still alive.
///
/// The PHY probes summarize themselves at shutdown ([metal_stats], [mmse_time_*], [ldpc_time_*],
/// [ul_*]). They used to register those summaries with std::atexit, which forced them to print
/// straight to stderr: by the time the atexit handlers run, the static objects behind
/// ocudulog::fetch_basic_logger() have already been destroyed, and asking for a logger there reads
/// freed memory (a crash, not a warning - the logger registry is gone).
///
/// A probe registers its summary here instead, and the application runs the registry on its way
/// out - after the workers and the services have stopped, but before the log files are flushed.
/// The summaries then reach the logging system like every other message: their level decides
/// whether they appear on the console or only in the log file.
///
/// The functions are stored by value so a caller can register a lambda that captures what it needs.
using report_fn = std::function<void()>;

/// Registers \p fn to be run once by run_all().
void add(report_fn fn);

/// Runs every registered summary, in registration order. Running it twice reports twice, so the
/// application calls it exactly once, at the point of its shutdown where the logging system is
/// still usable.
void run_all();

} // namespace phy_shutdown_report
} // namespace ocudu
