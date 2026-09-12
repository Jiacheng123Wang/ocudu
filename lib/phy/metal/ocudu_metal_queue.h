// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Process-wide Metal device and command queue shared by every PHY GPU engine.
///
/// \note Metal only serializes the *start* of the command buffers of one queue and is free to let
/// them overlap, and a buffer wrapped no-copy by one engine is a different resource than the same
/// memory wrapped by another one, so hazard tracking does not relate them either. A producer and a
/// consumer that share memory through the queue therefore have to synchronize explicitly: waiting
/// for the consumer's own command buffer does *not* guarantee that the producer's writes are
/// visible. Waiting for the newest command buffer of a burst does not even drain the older ones of
/// that same burst. The reliable options are a CPU-side wait (wait_all_committed() before touching
/// the memory, or the engine's own wait_committed(), which waits for every command buffer it has
/// committed) or a single command buffer with an explicit barrier between the encoders.
///
/// The intended usage for the CPU/GPU pipelining of the RX chain is:
///  - producer stage: submit work with the engine's non-waiting entry point (e.g.
///    dft_metal_engine::submit()) and keep the CPU busy with the next symbol,
///  - consumer stage: call wait_all_committed() before it reads the produced data from the CPU
///    (e.g. before staging the resource grid for the channel estimator), then dispatch normally.
///
/// \todo Move the remaining engines (channel estimator, equalizer, demapper, LDPC) onto this
///       queue when their stages are chained, and add the cross-stage command-buffer batching
///       planned in the pipeline decoupling work.

#pragma once

#if !defined(__OBJC__)
#error "ocudu_metal_queue.h is only available to Objective-C++ translation units."
#endif

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>

namespace ocudu {
namespace metal {

/// \brief Shared device and command queue, created on first use.
class shared_queue
{
public:
  /// Returns the process-wide Metal device, or nil when the platform has no Metal device.
  static id<MTLDevice> device();

  /// Returns the front-end command queue, used by the asynchronous producers of the RX chain (the
  /// per-symbol DFTs). nil when the device is unavailable or the queue could not be created.
  static id<MTLCommandQueue> queue();

  /// \brief Returns the back-end command queue, used by the late stages (channel estimator,
  /// equalizer, demapper, LDPC).
  ///
  /// The two queues keep the steadily fed front-end stream (a slot's worth of symbol DFTs submitted
  /// by the radio thread) from blocking the back-end stages: with one queue the back-end waits
  /// queue behind the in-flight FFTs and their measured latency exploded. Ordering across the two
  /// queues is provided by the explicit synchronization points instead of the queue itself - the
  /// RX chain drains and waits for a slot's DFTs before the grid is consumed, and every back-end
  /// stage waits for its own command buffer before its output is read on the CPU.
  static id<MTLCommandQueue> backend_queue();

  /// \brief Registers a command buffer committed through the shared queue.
  ///
  /// Called by the engines right after commit() so that wait_all_committed() knows what to wait
  /// for and the [metal_stats] probe can account for the shared dispatches.
  static void notify_commit(id<MTLCommandBuffer> command_buffer);

  /// \brief Waits for every command buffer committed through the shared queue so far.
  ///
  /// \note Command buffers of one queue complete in submission order, so waiting for the most
  /// recently committed one drains the whole chain. Returns false when a command buffer failed.
  static bool wait_all_committed();

  /// Number of command buffers committed through the shared queue (diagnostics).
  static uint64_t nof_commits();

  /// Number of command buffers still not waited for (diagnostics).
  static uint64_t nof_pending();
};

} // namespace metal
} // namespace ocudu
