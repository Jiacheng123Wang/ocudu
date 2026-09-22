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
    /// The estimator's INPUT stage: the pilot extraction, its noise variance and the ratio.
    channel_estimator,
    /// The estimator's WEIGHTS stage: the correlation matrices, the inversion, the weights and the
    /// y scatter, i.e. the second of the two command buffers one deferred hop commits.
    ///
    /// Kept apart from \c channel_estimator on purpose: the lane's GPU gap is the burst waiting for
    /// the estimator to finish, and the two stages are very different amounts of work, so "ch_est is
    /// 85% of the busy time" cannot say WHICH of them the 95% dependency share is waiting for. That
    /// question decides what to shorten next, and the answer has to come from the timeline.
    channel_estimator_weights,
    /// Equalization and demapping, which share one command buffer per burst.
    equalizer_demapper,
    /// \brief The MERGED route's single command buffer: the whole hop (the front end's transforms that wrote
    /// the grid, the estimator, the equalization and the demapping) in ONE submission (5.9.61/5.9.66).
    ///
    /// It is a stage of its own because the busy split attributes a command buffer's WHOLE GPU span to one
    /// stage, and Metal gives no encoder- or dispatch-level timestamps to divide it further. Calling that
    /// buffer \c equalizer_demapper made `eq_demap` the sum of four stages, which is exactly the number an
    /// optimization would aim at - so the label has to say what the buffer is.
    merged_hop,
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

  /// \brief Registers a front-end (DFT) command buffer with the slot it belongs to.
  ///
  /// The transforms of one slot are submitted by the radio thread, on the front-end queue, while the
  /// lane of the same slot (estimator, equalizer, demapper) is filled by another thread - so the two
  /// are accounted as two series instead of being merged: the front-end one answers what the transforms
  /// cost the device and how much of that time they waited for it, and the lane one is the same
  /// question for the back end. Together they cover the whole IQ -> LLR path.
  ///
  /// Called right after commit(), which is also when the slot must be told (set_lane_slot()): a slot
  /// change closes the previous group.
  static void register_front_end_commit(id<MTLCommandBuffer> cb, uint64_t slot_index);

  /// Prints the accumulated statistics to stderr (registered with atexit).
  static void report();
};

#else

class gpu_lane_probe
{
public:
  enum class stage : unsigned { dft, channel_estimator, channel_estimator_weights, equalizer_demapper, other, count };

  static void register_commit(id<MTLCommandBuffer>, stage) {}
  static void close_lane() {}
  static void register_front_end_commit(id<MTLCommandBuffer>, uint64_t) {}
  static void report() {}
};

#endif

} // namespace metal
} // namespace ocudu
