// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/synchronization/futex_util.h"

#if !defined(__APPLE__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <cerrno>
#include <cstdint>
#endif

#if defined(__APPLE__)
// macOS provides ulock syscalls (in libsystem_kernel) as the native equivalent of Linux futexes:
// UL_COMPARE_AND_WAIT atomically compares the 32-bit value at addr with the expected value and blocks while they
// match. It provides the same protection against lost wake-ups as FUTEX_WAIT_PRIVATE.
// These declarations and constants mirror <ulock.h> from the macOS SDK, which is not shipped with the
// CommandLineTools SDK.
extern "C" {
int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
}

constexpr uint32_t UL_COMPARE_AND_WAIT = 1;
constexpr uint32_t ULF_WAKE_ALL        = 0x00000100;
constexpr uint32_t ULF_NO_ERRNO        = 0x01000000;
#endif

using namespace ocudu;

long futex_util::wait(std::atomic<uint32_t>& state, uint32_t expected)
{
#if !defined(__APPLE__)
  // The kernel will only sleep if *addr == expected; otherwise returns -1/EAGAIN.
  // Note: Futex requires int*.
  // Note: No C++ aliasing is happening, as the kernel will just copy the uint32 bytes.
  auto* addr = reinterpret_cast<int*>(&state);
  return ::syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
#else
  // The kernel will only sleep if *addr == expected; otherwise it returns immediately. The comparison is atomic
  // with respect to __ulock_wake, so a wake issued between the caller's state check and this syscall is not lost.
  // Note: No C++ aliasing is happening, as the kernel will just copy the uint32 bytes.
  auto* addr = reinterpret_cast<void*>(&state);
  // With ULF_NO_ERRNO the syscall returns 0 on success and the negated error number on failure.
  int status = ::__ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, addr, expected, 0);
  if (status == 0 or status == -EWOULDBLOCK) {
    // Woken up, or the state no longer matches the expected value (equivalent to success).
    return 0;
  }
  errno = -status;
  return -1;
#endif
}

long futex_util::wake_all(std::atomic<uint32_t>& state)
{
#if !defined(__APPLE__)
  // Note: Futex requires int*.
  // Note: No C++ aliasing is happening, as the kernel will just copy the uint32 bytes.
  auto* addr = reinterpret_cast<int*>(&state);
  return ::syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
#else
  auto* addr = reinterpret_cast<void*>(&state);
  int   status = ::__ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL | ULF_NO_ERRNO, addr, 0);
  if (status == 0 or status == -ENOENT) {
    // Success, or no threads were waiting. The syscall does not report the number of woken threads.
    return 0;
  }
  errno = -status;
  return -1;
#endif
}
