// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief One command buffer shared by the consecutive stages of a deferred burst.
///
/// The stages of a burst (equalization and then demapping) write and read the same memory through
/// different no-copy wrapped buffers. Metal's hazard tracking cannot relate two buffers that alias
/// the same memory, and the command buffers of one queue only start in submission order - they may
/// overlap - so a consumer must not rely on the queue to observe the producer's writes. Encoding
/// every dispatch of the burst into a single command buffer and inserting an explicit memory
/// barrier when the stage (the compute pipeline) changes orders the stages without an intermediate
/// CPU wait and without one command buffer commit per stage.
///
/// The state is thread local on purpose: the stages of one demodulation run on the same thread,
/// while several demodulations run at the same time on different threads and must not share a
/// command buffer.

#pragma once

#if !defined(__OBJC__)
#error "ocudu_metal_burst.h is only available to Objective-C++ translation units."
#endif

#import <Metal/Metal.h>

namespace ocudu {
namespace metal {

/// Shared command buffer of the stages of one deferred burst.
class shared_burst
{
public:
  /// \brief Opens the burst if needed and returns its encoder, switching to \c pipeline.
  ///
  /// A memory barrier is inserted when the pipeline changes, i.e. between the stages of a burst, so
  /// the writes of the previous stage are visible to the dispatches of the next one.
  /// \return The encoder of the burst, or nil when the command buffer could not be created.
  static id<MTLComputeCommandEncoder> encoder(id<MTLComputePipelineState> pipeline);

  /// True when a burst is open (dispatches encoded, not committed yet).
  static bool open();

  /// \brief Ends the encoder and commits the burst without waiting.
  /// \return True when a burst was committed.
  static bool commit();

  /// \brief Waits for every command buffer committed by this thread's bursts.
  /// \return True when all of them completed successfully.
  static bool wait_committed();

  /// Number of dispatches encoded in the open burst (diagnostics).
  static unsigned size();

  /// Accounts one dispatch appended to the burst (diagnostics).
  static void count_dispatch();
};

} // namespace metal
} // namespace ocudu
