// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/resource_usage/resource_usage_utils.h"
#include <fstream>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

using namespace ocudu;
using namespace resource_usage_utils;

/// \brief Reads the current resident set size of the calling process (in kilobytes).
static long current_rss_kb()
{
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    return static_cast<long>(info.resident_size / 1024);
  }
  return -1;
#else
  std::ifstream status_file("/proc/self/status");
  std::string   line;
  while (std::getline(status_file, line)) {
    if (line.compare(0, 6, "VmRSS:") == 0) {
      std::istringstream iss(line.substr(6));
      long               kb = -1;
      iss >> kb;
      return kb;
    }
  }
  return -1;
#endif
}

/// \brief Reads the total system memory (in kilobytes).
static long total_system_memory_kb()
{
  static const long total_kb = [] {
#if defined(__APPLE__)
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctl(mib, 2, &memsize, &len, NULL, 0) == 0) {
      return static_cast<long>(memsize / 1024);
    }
    return -1L;
#else
    std::ifstream status_file("/proc/meminfo");
    std::string   line;
    while (std::getline(status_file, line)) {
      if (line.compare(0, 9, "MemTotal:") == 0) {
        std::istringstream iss(line.substr(9));
        long               kb = -1;
        iss >> kb;
        return kb;
      }
    }
    return -1L;
#endif
  }();

  return total_kb;
}

/// Converts rusage struct to the snapshot.
static cpu_snapshot to_snapshot(const ::rusage& rusg)
{
  cpu_snapshot s;
  s.tp          = rusage_meas_clock::now();
  s.user_time   = std::chrono::seconds{rusg.ru_utime.tv_sec} + std::chrono::microseconds{rusg.ru_utime.tv_usec};
  s.system_time = std::chrono::seconds{rusg.ru_stime.tv_sec} + std::chrono::microseconds{rusg.ru_stime.tv_usec};
  s.current_rss = current_rss_kb();

  return s;
}

expected<cpu_snapshot, int> resource_usage_utils::cpu_usage_now(rusage_measurement_type type)
{
  ::rusage ret;
  if (::getrusage(static_cast<int>(type), &ret) == 0) {
    return to_snapshot(ret);
  }
  return make_unexpected(errno);
}

measurements resource_usage_utils::operator+(const measurements& lhs, const measurements& rhs)
{
  measurements sum;
  sum.duration    = lhs.duration + lhs.duration;
  sum.user_time   = lhs.user_time + rhs.user_time;
  sum.system_time = lhs.system_time + lhs.system_time;
  sum.current_rss = std::max(lhs.current_rss, rhs.current_rss);

  return sum;
}

resource_usage_metrics resource_usage_utils::res_usage_measurements_to_metrics(measurements              measurements,
                                                                               std::chrono::microseconds period)
{
  static constexpr unsigned BYTES_IN_KB = 1024;

  auto total_cpu_time_used = measurements.user_time + measurements.system_time;

  resource_usage_metrics metrics = {};
  metrics.cpu_stats.cpu_usage_percentage =
      static_cast<double>(total_cpu_time_used.count()) / static_cast<double>(period.count());
  metrics.cpu_stats.cpu_utilization_nof_cpus =
      static_cast<double>(total_cpu_time_used.count()) / static_cast<double>(measurements.duration.count());
  metrics.memory_stats.memory_usage = units::bytes(BYTES_IN_KB * measurements.current_rss);

  long total_mem_kb = total_system_memory_kb();
  metrics.memory_stats.memory_usage_percentage =
      total_mem_kb > 0 ? static_cast<float>(measurements.current_rss) / static_cast<float>(total_mem_kb) * 100.0F
                       : 0.0F;

  return metrics;
}
