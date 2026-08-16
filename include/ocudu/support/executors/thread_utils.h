// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <thread>

namespace ocudu {

/// Get caller thread name.
const char* this_thread_name();

/// \brief Portable pause hint for short backoff/spin loops.
///
/// On arm64 (Apple Silicon) this emits the YIELD hint (__builtin_arm_yield()) instead of the syscall-heavy
/// sched_yield(), avoiding scheduler thrash in tight retry loops. On x86 it emits PAUSE. Use only in bounded loops
/// whose termination is driven by another thread; long waits should use condition variables or sleep.
inline void cpu_relax()
{
#if defined(__aarch64__) || defined(__arm__)
  __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

} // namespace ocudu
