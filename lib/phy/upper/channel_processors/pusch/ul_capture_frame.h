// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// A frame number that keeps counting past the 10.24-second wrap.
//
// WHY IT IS SHARED. `slot_point::count()` and `system_slot()` both restart with the system frame number, so over a long
// capture the same numeric slot appears again and again -- three captures named `..._27_17923` from three different
// frames were found in one leg (2026-10-27). The capture name already fixed this for the FILES, by counting wraps in
// `extended_frame()`. The live transport-block dump needs the same number for the same reason: it has to name the
// reception a decoded block belongs to, and a wrapping slot cannot do that.
//
// The counter is one variable for the whole process (an inline variable, so every translation unit sees the same one),
// which is what makes the capture side and the decoder side agree.

#pragma once

#include "ocudu/ran/slot_point.h"
#include <atomic>
#include <cstdint>

namespace ocudu {
namespace detail {

inline std::atomic<uint32_t> extended_frame_base{0};      //!< Cycles counted so far, times 1024.
inline std::atomic<uint32_t> extended_frame_last_sfn{0xFFFFFFFFu};

} // namespace detail

/// \brief Frame number that keeps counting past the wrap, so (frame, slot) identifies one reception for a whole leg.
///
/// A wrap is a big step backwards, not any step: `sfn` is read from several threads, so consecutive observations can
/// arrive out of order by a few frames and "went backwards" would then fire constantly (an 18:35 leg reported a frame
/// span of 678 833 in sixty seconds, about 650 wraps too many). Half a cycle of hysteresis tells a wrap from a race.
inline uint32_t extended_frame(slot_point slot)
{
  const uint32_t sfn  = slot.sfn();
  uint32_t       base = detail::extended_frame_base.load(std::memory_order_relaxed);
  const uint32_t prev = detail::extended_frame_last_sfn.load(std::memory_order_relaxed);
  if ((prev != 0xFFFFFFFFu) && (sfn < prev) && ((prev - sfn) > 512u)) {
    base = detail::extended_frame_base.fetch_add(1024u, std::memory_order_relaxed) + 1024u;
  }
  detail::extended_frame_last_sfn.store(sfn, std::memory_order_relaxed);
  return base + sfn;
}

} // namespace ocudu
