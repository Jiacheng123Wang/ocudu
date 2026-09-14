// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief How long a slot's data occupies the GPU, and how much of that time the GPU was executing.
///
/// The staged probes ([ul_time_frequency], [ul_channel_estimation], [ul_equalization_demod]) measure
/// CPU-side boundaries. They cannot answer whether the GPU is being fed: a segment that covers the
/// arrival of fourteen symbols (rather than the compute of one) grows with the pipeline's fill, and
/// a stage whose work is deferred measures the deferral. What the fused pipeline needs is the
/// opposite view - the data's own timeline on the device:
///
///  - \c residency: first command buffer of the lane starts -> last one ends (pure GPU timestamps),
///  - \c busy: sum of the command buffers' GPU execution times,
///  - \c gap: \c residency - \c busy, i.e. the time the lane's data was on the device while the
///    device was NOT executing it - the GPU waiting for the CPU to feed it,
///  - \c period: distance between two consecutive lanes' ends (the throughput cadence).
///
/// A *lane* is one slot's worth of chained work on one thread: it opens at the first command buffer
/// committed after the previous lane closed and closes when the burst that produces the LLRs
/// completes (see shared_burst, which owns the boundary). Stages register their command buffers at
/// commit time - the timestamps are only meaningful once a command buffer completed - and the
/// registration is by thread, because the chained stages of one slot run on the same thread.
///
/// \note The per-symbol DFTs are not part of the lane yet: they are submitted by the radio thread on
///       the front-end queue, so they need a lane identity that crosses threads (the slot). They are
///       also the stage that moves onto the lane's queue when the fused pipeline lands.

#pragma once

#if !defined(__OBJC__)
#error "ocudu_metal_lane_probe.h is only available to Objective-C++ translation units."
#endif

#import <Metal/Metal.h>

namespace ocudu {
namespace metal {

#if defined(OCUDU_METAL_STATS)

/// GPU-side residency accounting of the deferred burst chain (see the file comment).
class gpu_lane_probe
{
public:
  /// Stage a command buffer belongs to. The list is the pipeline order, so a report that prints the
  /// busy time per stage reads as the lane's timeline.
  enum class stage : unsigned {
    /// Time-frequency transform (per-symbol DFTs; not registered yet, see the file comment).
    dft,
    /// Channel estimator (weights, apply and the optional equalizer estimates).
    channel_estimator,
    /// Equalization and demapping, which share one command buffer per burst.
    equalizer_demapper,
    /// Anything else committed inside the lane.
    other,
    count
  };

  /// \brief Registers a command buffer committed by the calling thread with its open lane.
  ///
  /// Must be called right after commit(), when the command buffer is still pending: the GPU
  /// timestamps are read when the lane closes, and they are only valid once it completed.
  static void register_commit(id<MTLCommandBuffer> cb, stage which);

  /// \brief Closes the calling thread's lane and accumulates its metrics.
  ///
  /// Called once the lane's last command buffer completed (shared_burst::wait_committed()). Command
  /// buffers that did not complete yet (or that carry no GPU timestamps) are left registered for the
  /// next lane instead of being reported as gap.
  static void close_lane();

  /// Prints the accumulated statistics to stderr (registered with atexit).
  static void report();
};

#else

class gpu_lane_probe
{
public:
  enum class stage : unsigned { dft, channel_estimator, equalizer_demapper, other, count };

  static void register_commit(id<MTLCommandBuffer>, stage) {}
  static void close_lane() {}
  static void report() {}
};

#endif

} // namespace metal
} // namespace ocudu
