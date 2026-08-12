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

/// Returns the current UTC time and date with millisecond precision.
inline std::string get_time_stamp()
{
  auto    tp           = std::chrono::high_resolution_clock::now();
  std::time_t tt;
  using ClockType = typename std::decay_t<decltype(tp)>::clock;

  if constexpr (std::is_same_v<ClockType, std::chrono::system_clock>) {
    tt = std::chrono::system_clock::to_time_t(tp);
  } else {
    auto sys_tp = std::chrono::system_clock::now() +
                  std::chrono::duration_cast<std::chrono::system_clock::duration>(
                      tp - ClockType::now());
    tt = std::chrono::system_clock::to_time_t(sys_tp);
  }

  std::tm current_time = fmt::gmtime(tt);
  auto    ms_fraction  = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000u;
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
