// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/support/synchronization/stop_event.h"
#include <atomic>

namespace ocudu {

class resource_grid;

/// \brief Uplink resource grid controller.
///
/// Wraps a reference to a resource grid with a reference counter and a stop token to control the lifetime of resource
/// grids in the uplink processing chain.
class uplink_resource_grid_controller : private shared_resource_grid::pool_interface
{
public:
  uplink_resource_grid_controller(resource_grid& grid_, std::atomic<unsigned>& ref_counter_) :
    grid(grid_), ref_counter(ref_counter_)
  {
  }

  /// Creates a shared grid in exchange for a stop token that will be released when the grid is released.
  shared_resource_grid create_shared_grid(rt_stop_event_token stop_token_)
  {
    stop_token = std::move(stop_token_);

    [[maybe_unused]] unsigned prev_ref_counter = ref_counter.exchange(1, std::memory_order_release);
    ocudu_assert(prev_ref_counter == 0, "Resource grid was already alive in a scope.");

    return shared_resource_grid(*this, ref_counter);
  }

  /// Returns true if the resource grid is not present in any scope.
  bool is_available() const { return ref_counter.load(std::memory_order_acquire) == 0; }

  /// \brief Gives up waiting for the grid's users and releases the stop token.
  ///
  /// The stop path waits for the token (\ref stop_event_source_impl::stop_for) so that a grid still being written is
  /// not torn down under its writer's feet. When that wait expires the shutdown has to continue anyway, and it must
  /// not then hang a second time in the destructor -- \ref stop_event_source_impl::~stop_event_source_impl calls the
  /// UNBOUNDED stop(). This is the escape: the token is reset, so the token count reaches zero and the destruction
  /// of the processor can proceed. It does not free or invalidate the grid: the memory stays owned by the processor
  /// and a late release from a still-running writer is still honoured (which is the window this trades a hang for).
  void force_release_stop_token() { stop_token.reset(); }

private:
  // See the shared_resource_grid::pool_interface interface for documentation.
  resource_grid& get() override { return grid; }

  // See the shared_resource_grid::pool_interface interface for documentation.
  void notify_release_scope() override { stop_token.reset(); }

  /// Reference to the actual resource grid to control.
  resource_grid& grid;
  /// Resource grid reference counter.
  std::atomic<unsigned>& ref_counter;
  /// Stop token acquired for the slot processing.
  rt_stop_event_token stop_token;
};

} // namespace ocudu
