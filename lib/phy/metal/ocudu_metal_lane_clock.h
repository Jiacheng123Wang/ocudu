// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Host-side timestamps of one lane, shared by the stages that produce it.
///
/// ---- Why this exists ----
/// The lane probe reports three GPU-side numbers per lane (see ocudu_metal_lane_probe.h): residency
/// (first command buffer starts -> last one ends), busy (the command buffers' execution time) and
/// gap (residency - busy: the device held the lane's data without executing it). On air the gap was
/// 229.9us of a 735.7us lane - it is on the lane's critical path, so it is latency, not an occupancy
/// statistic - and it has two candidate sources that the GPU timestamps alone cannot tell apart:
///
///   1. the HOST: the time between the estimator's stage entry and the commit of the command buffer
///      that carries it (the lane's first). Until that commit exists, the back end has nothing to run
///      for this lane, no matter how idle it is;
///   2. the FENCE the lane burst encodes when it opens (backend_stage_wait): the
///      burst cannot start before the estimator's own command buffer has completed.
///
/// With a per-hop pair of host timestamps next to the lane's GPU start time, (1) and (2) separate:
/// compare the extraction command buffer's GPUStartTime against the host's stage entry (how long the
/// host took to hand the lane over) and against the lane burst's own start (how long the burst then
/// waited on the fence).
///
/// The state is thread local: one lane's stages run on one thread, and several slots are processed
/// concurrently on different threads.

#pragma once

#include <chrono>
#include <cstdint>

namespace ocudu {
namespace metal {

/// The clock every reading in this header uses. One alias so the helpers below can name it without
/// depending on the struct that carries the readings.
using steady_clock_t = std::chrono::steady_clock;

/// \brief Host clock readings of the lane being assembled on this thread.
///
/// Plain data, written by the stages and read by the diagnostics - it carries no policy and no
/// ownership, which is what lets the estimator (a .cpp) and the burst (a .mm) share it without
/// either owning the other.
struct lane_host_clock {
  using clock = steady_clock_t;

  /// When the estimator's stage for this lane was entered (the earliest host work of the lane).
  clock::time_point stage_entry{};
  /// When the command buffer carrying the estimator's work was committed (the lane's first).
  clock::time_point extraction_commit{};
  /// Host-side reading of this lane, in microseconds. -1 means "not measured" and is left out of the
  /// statistics rather than counted as zero.
  ///
  /// \note Why there is only ONE host reading here, and why it is this one. The first version of this
  /// header also carried "how long ago did the front end finish" (a cross-thread reading the DFT
  /// engine published). It measured 576.8us against a lane gap of 217.2us on air - an impossible
  /// decomposition, because the DFT engine commits SEVERAL command buffers per slot, so "the last
  /// front-end completion" belongs to whatever batch happened to commit most recently and has no
  /// relation to the slot of the lane being measured. The slot-matched version of that quantity is
  /// already published as the [ul_channel_estimation] phase (record_t2f_end -> record_ce_end, see
  /// ul_pipeline_probe.h), on a clock that pairs the readings by slot. Measure it there, not here.
  double handover_us = -1.0;  ///< this stage's entry -> the extraction's commit
  /// When the LANE's own command buffer was committed - the hop's last host act before the CPU can stand
  /// aside (the fused lane's single submission, see shared_burst::commit()).
  clock::time_point lane_commit{};
  /// This stage's entry -> the lane's commit, in microseconds (-1 = not measured).
  ///
  /// WHY IT EXISTS (dev doc 6.95). `handover_us` covers only the FIRST half of the host's participation in
  /// a hop (entry -> extraction commit); the tail - encoding the weights and the burst up to the lane's own
  /// commit - was invisible on the delivery route, because the fused route registers ONE command buffer and
  /// the probe's five host segments are therefore dark by construction (7.6.0b). This is the total: what the
  /// CPU spends on a hop from entering it to handing the whole thing over, which is the quantity G2 (the
  /// high-level doc's second goal: one-shot host participation) is about.
  double entry_to_lane_commit_us = -1.0;

  /// \brief Receiving slot of the lane being assembled on this thread, and whether it was ever told (P0-5).
  ///
  /// This is the KEY the lane probe pairs on. Its residency/busy are per LANE (one thread's chained command
  /// buffers, one hop), while the phase-segment probe's segments are per SLOT and exist only for a CRC-OK
  /// transport block - so the two reports describe different populations (measured on `s85-p0phases`: 60389
  /// phase samples against 142022 lanes) and any ratio read across them is indicatory. The slot is the one
  /// thing both sides know about the same hop, and the estimator's stage entry is where the lane learns it
  /// (see mark_stage_entry(), called by the adapter with the slot its configuration was built for).
  ///
  /// \c has_lane_slot false means "this thread never named its lane's slot" (a tool that drives the burst
  /// directly, or a route with no estimator hop): the lane probe reports those lanes apart instead of pairing
  /// them against slot 0, which is a key a real slot also takes.
  uint64_t lane_slot     = 0;
  bool     has_lane_slot = false;

  void mark_stage_entry(uint64_t slot)
  {
    stage_entry       = clock::now();
    extraction_commit = {};
    handover_us       = -1.0;
    // The tail belongs to ONE lane: a thread that starts a new lane must not inherit the previous one's
    // commit (a route whose lane is committed elsewhere would otherwise report a stale span as this lane's).
    lane_commit             = {};
    entry_to_lane_commit_us = -1.0;
    lane_slot               = slot;
    has_lane_slot           = true;
  }

  void mark_extraction_commit()
  {
    extraction_commit = clock::now();
    handover_us       = delta_us(stage_entry, extraction_commit);
  }

  /// Called by the lane's own commit (shared_burst::commit(), immediately before [cb commit]): the last
  /// host act of the hop. Only a thread that entered this lane's stage can measure the total, so a thread
  /// that never did leaves it at -1 (not measured) rather than reporting a partial span.
  void mark_lane_commit()
  {
    lane_commit             = clock::now();
    entry_to_lane_commit_us = delta_us(stage_entry, lane_commit);
  }

  /// Microseconds between the stage entry and the extraction's commit, or -1 when either is unset.
  double entry_to_commit_us() const { return delta_us(stage_entry, extraction_commit); }

private:
  static double delta_us(clock::time_point from, clock::time_point to)
  {
    if ((from == clock::time_point{}) || (to == clock::time_point{})) {
      return -1.0;
    }
    return std::chrono::duration<double, std::micro>(to - from).count();
  }
};

/// The calling thread's clock (see the header).
inline thread_local lane_host_clock lane_clock;

} // namespace metal
} // namespace ocudu
