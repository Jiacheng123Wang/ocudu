// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <atomic>
#include <cstdint>

namespace ocudu {

/// \brief Tells a HOST reader of the resource grid to wait until that grid has been produced (D1-A, 5.9.13).
///
/// With the block hand-over the resource grid is produced at the LANE's commit instead of at the end of the
/// receiving slot, so the consumers that read it on the HOST - the PUCCH (format 0/2/3/4 and the format-1
/// collection) and the SRS, none of which has a device view - would read memory nobody has written yet.
/// Measured before this existed: every PUCCH report came out `metric=nan sinr=-inf` and the attach never
/// completed (design document 5.9.12).
///
/// The wait covers BOTH shapes a slot can have, which is why it is not just a fence:
///  * a slot whose grid a hop claimed: the lane commits it, and the wait is for that commit's completion;
///  * a slot NOBODY claimed (a PUCCH-only slot has no PUSCH hop at all): nothing would ever commit it, so
///    the implementation commits it here - the fallback a hand-over owes.
///
/// \note This is a HOOK and not a direct call: the implementation lives with the Metal engines, while the
///       consumers live in the upper PHY, which a build without Metal must still link. No hook installed
///       (every build without the hand-over) means "nothing to wait for", which is exactly the behaviour
///       those builds had before: the grid is produced where it always was.
///
/// \note Call it on the consumer's OWN thread and PROMPTLY - the consumer that reads the grid on the host
///       is already on an executor of its own for this reason. The implementation is non-blocking when
///       nothing is pending, and bounded when something is, so a generation nobody signals cannot hang the
///       caller for good.
class grid_ready_hook
{
public:
  /// \param[in] storage Base of the grid's storage (resource_grid_device_view::base).
  /// \param[in] slot    The RECEIVING SLOT the grid belongs to. It is half of the key, and the half that
  ///                    makes it unambiguous: the storage address is handed back by the grid pool as soon
  ///                    as the next slot's grid arrives, so the address alone would let a late reader be
  ///                    served the NEXT slot's block (design document 5.9.15).
  ///
  /// Waits for the grid at \p storage / \p slot, at most \p timeout_ms. True when the grid is ready (or nothing was
  /// pending), false when the wait timed out - the caller must then NOT trust the grid.
  using wait_fn = bool (*)(const void* storage, uint64_t slot, uint32_t timeout_ms);

  /// Installs the implementation (called by the Metal engines once, on first use).
  static void install(wait_fn fn) { fn_ref().store(fn, std::memory_order_release); }

  /// Whether an implementation is installed (diagnostics and tests).
  static bool installed() { return fn_ref().load(std::memory_order_acquire) != nullptr; }

  /// See the class documentation.
  static bool wait(const void* storage, uint64_t slot, uint32_t timeout_ms = 200)
  {
    wait_fn fn = fn_ref().load(std::memory_order_acquire);
    return (fn == nullptr) || fn(storage, slot, timeout_ms);
  }

private:
  static std::atomic<wait_fn>& fn_ref()
  {
    static std::atomic<wait_fn> fn{nullptr};
    return fn;
  }
};

} // namespace ocudu
