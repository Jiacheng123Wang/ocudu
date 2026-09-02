// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "fmt/chrono.h"
#include <cmath>
#include <type_traits>
#include <chrono>

namespace ocudu {
namespace app_helpers {

/// Helper template: safely converts an arbitrary Clock/Duration time_point into a system_clock::time_point
template <typename Clock, typename Duration>
inline std::chrono::system_clock::time_point to_system_time_point(std::chrono::time_point<Clock, Duration> tp)
{
  if constexpr (std::is_same_v<Clock, std::chrono::system_clock>) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(tp);
  } else {
    return std::chrono::system_clock::now() +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(tp - Clock::now());
  }
}

/// Returns the current UTC time and date with millisecond precision.
inline std::string get_time_stamp()
{
  auto tp     = std::chrono::high_resolution_clock::now();
  auto sys_tp = to_system_time_point(tp);

  std::time_t tt       = std::chrono::system_clock::to_time_t(sys_tp);
  std::tm     current_time = fmt::gmtime(tt);
  auto        ms_fraction  = std::chrono::duration_cast<std::chrono::milliseconds>(sys_tp.time_since_epoch()).count() % 1000u;
  return fmt::format("{:%F}T{:%H:%M:%S}.{:03}", current_time, current_time, ms_fraction);
}

/// Return the given value if it is not a NaN or Inf, otherwise returns 0.
inline double validate_fp_value(double value)
{
  if (!std::isnan(value) && !std::isinf(value)) {
    return value;
  }
  return 0.0;
}

} // namespace app_helpers
} // namespace ocudu
