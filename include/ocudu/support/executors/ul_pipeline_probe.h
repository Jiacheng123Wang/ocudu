// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_mode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#if defined(OCUDU_FLOW_PROBES)
#include "ocudu/ocudulog/ocudulog.h"
#endif

namespace ocudu {

/// Per-PUSCH RX phase-segment durations, assembled by the UL pipeline probe once all the per-slot timestamps
/// (IQ reception, FFT completion, channel estimation completion and LDPC decode start) are available.
struct ul_phase_durations {
  /// Time-frequency transform: IQ samples received -> whole-slot frequency-domain symbols ready.
  std::chrono::nanoseconds time_frequency;
  /// Channel estimation: frequency-domain symbols ready -> data-symbol channel estimates ready.
  std::chrono::nanoseconds channel_estimation;
  /// Equalization + demodulation: channel estimates ready -> per-bit LLRs ready (start of the LDPC decode).
  std::chrono::nanoseconds equalization_demod;
};

/// \brief The slot a received block of samples COMPLETES, from the stream position alone.
///
/// Slot S's samples are complete once its LAST sample has arrived, i.e. once the stream covers `S + 1`
/// slots' worth of samples. Written that way the rule needs no boundary arithmetic and no case split:
///
///     completed = (block_begin + nof_samples) / nof_samples_per_slot - 1     (when that is >= 0)
///
/// The rule this replaces (5.9.68, 5.9.69) asked instead whether the next slot boundary fell STRICTLY
/// INSIDE the block, and credited the slot that STARTS at that boundary. On the production path - a block is
/// exactly one slot and starts on the grid - both halves were wrong, and measurably so:
///
///  * the test became `7680 < 7680`, false for EVERY block, so the per-slot timeline recorded almost nothing
///    (`rows=1` against a bound of 64, and that one row came from the single UNALIGNED block of the run);
///  * the one time it did fire it named the slot AFTER the one the block had completed: the block
///    [29767687, 29775367) carries slot 3876's last sample (29775359) and the old arithmetic answered 3877.
///
/// \param[in]  block_begin          Stream position of the block's first sample.
/// \param[in]  nof_samples          Samples the block carries.
/// \param[in]  nof_samples_per_slot Samples per slot.
/// \param[out] done_slot            Completed slot INDEX (absolute, NOT reduced modulo the slots of an SFN
///                                  cycle) when the function returns true.
/// \return True when the block completes a slot; false when it ends before the first slot does. A block that
///         spans several slots completes only the NEWEST one: the caller announces one slot per block, and in
///         a contiguous stream the older ones were announced by earlier blocks.
inline bool ul_slot_completed_by_block(uint64_t  block_begin,
                                       unsigned  nof_samples,
                                       unsigned  nof_samples_per_slot,
                                       uint64_t& done_slot)
{
  if ((nof_samples_per_slot == 0) || (nof_samples == 0)) {
    return false;
  }
  const uint64_t completed = (block_begin + nof_samples) / nof_samples_per_slot;
  if (completed == 0) {
    return false;
  }
  done_slot = completed - 1;
  return true;
}

#if defined(OCUDU_FLOW_PROBES)

/// \brief Measures the UL compute pipeline latency (IQ samples received -> LDPC decoded with CRC OK).
///
/// - record_start() is called by the lower PHY baseband processor right after the IQ samples of a slot are received.
/// - record_end_crc_ok() is called by the PUSCH decoder notifier when the transport block CRC check passes.
///
/// Starts and ends are matched in order (FIFO): with the light traffic of a single UE there is one PUSCH in flight
/// at a time, so the first uncompleted start corresponds to the next CRC-OK completion. Latencies are accumulated in
/// memory; report() prints the statistics through the logging system (call it during the application shutdown).
///
/// A second series measures the pure LDPC decoder latency (first codeblock decode invocation -> the same CRC-OK
/// completion as above): record_ldpc_start() is called by the PUSCH codeblock task right before the LDPC decoder is
/// invoked. Starts and ends are matched by exact slot number: both ends carry the same FAPI slot reference
/// (pdu.slot), so no offset tolerance is needed - unlike the pipeline series, whose start key is derived from
/// the lower-PHY sample timestamp and may be offset by a slot or two. A start left behind by a CRC-failed TB or
/// a retransmission that needed no decode is simply left unmatched (and eventually evicted). A generous
/// time-based staleness gate (max_entry_age, two seconds) rejects
/// entries that survived a quiet stretch: without it, a stale entry from a previous slot-count wrap cycle (the
/// count wraps every 10.24 s for any numerology) could match a fresh completion carrying the same slot key and
/// produce bogus latencies of one or more whole wrap cycles (observed in a long run: 92.16 s = 9 cycles plus the
/// genuine 386 us decode time). The gate is orders of magnitude above any legitimate start -> completion span
/// (the slowest decoders observed are tens of milliseconds), so it never truncates the series.
///
/// A third series records the size in bytes of each CRC-OK MAC PDU (the data burst), in lockstep with the LDPC
/// latency series, so its sample count always matches [ul_ldpc_decode]; report() prints its distribution and the
/// total number of bytes on separate lines.
///
/// A fourth series measures the FAPI->MAC tail latency (transport-block CRC OK -> MAC UL task enqueue, covering
/// the FAPI P7 fastpath translation and the byte_buffer copy): record_fapi_mac_end() is called by the MAC UL
/// processor right after the per-PDU enqueue. Starts are the CRC-OK completion timestamps (record_end_crc_ok,
/// which is only ever called for CRC-OK TBs) and are matched by exact slot number, with the same staleness gate.
/// A start left behind when the PDU is dropped (e.g. the per-UE queue full) is simply evicted later.
///
/// A fifth series, [ul_rx_wait], measures how long the RECEIVE blocked (record_rx_wait(), called by the lower PHY
/// baseband processor around its receiver.receive()). It is the one series with no pairing at all: the two clock
/// reads bracket a single call, so nothing about slot keys or staleness can go wrong in it.
///
/// WHY IT EXISTS, AND WHY IT IS NOT REDUNDANT WITH [ul_time_frequency]. Since the UL pipeline series starts when the
/// samples START ARRIVING (record_start() before receive()), it now includes the wait for them - so a series whose
/// name says "time-frequency" reports a whole slot of receive time plus the transforms. That was the deliberate
/// price of making the series comparable across receive policies (see lower_phy_baseband_processor::ul_process and
/// the design document), and this series is what makes the split visible instead of implied:
///
///     [ul_time_frequency] = [ul_rx_wait] + (the front end's own work on the samples)
///
/// It also measures the host's LEAD OR LAG against the radio's sample timeline, which is otherwise invisible: the
/// samples are produced by the ADC at a fixed rate and the host consumes them as fast as it can, so a host that is
/// ahead BLOCKS for the samples to exist, and one that is behind returns immediately with data the radio had
/// already buffered. Under the whole-slot receive policy a block is a whole slot, so the wait cannot be shorter
/// than "until the slot's last sample exists" - which is the structural latency that a symbol-grained receive
/// policy exists to remove (S-7g-13). Read together with the receive policy in force.
///
/// The phase-segment series (time-frequency / channel estimation / equalization+demodulation) measure the CPU side of
/// the module boundaries, so they are meaningless once the whole IQ -> LLR chain runs inside the fused device-side
/// lane: in phy_pipeline_mode::gpu neither their recording nor their report happens (the lane reports its own
/// 'into the GPU -> out of the GPU' residency and busy/gap split instead). What the fused lane reports instead is
/// their TOTAL, as a single series: [ul_gpu_pipeline], from the arrival of the slot's first IQ samples to the moment
/// the LLRs are handed to the decoder - the span the lane owns end to end (IQ upload, per-symbol transforms, channel
/// estimation, equalization and demapping, and the LLR transfer back). Its two ends are the same two instants that
/// bound the phase segments (record_start() and the first codeblock decode invocation), so it equals their sum by
/// construction, and [ul_pipeline] - [ul_gpu_pipeline] is what follows the LLRs (rate matching, LDPC decode, CRC
/// check and the FAPI completion).
///
/// \note The two edge series do not have the same population: [ul_pipeline] is completed only by a CRC-OK transport
///       block (see record_end_crc_ok), while [ul_gpu_pipeline] is recorded at every decode attempt, so the fused
///       series stays visible in a run whose decodes all fail. In a healthy run the difference is the failed
///       attempts (e.g. ~21% more samples at a 79% CRC-OK rate), which is a caveat on comparing their means, not on
///       either series.
///
/// \note [ul_gpu_pipeline] is a wall-clock window, not device execution time: it covers the host submission work and
///       any queueing between the lane's stages. Read it together with the gpu_lane_probe report, whose residency /
///       busy / gap split says how much of it the device was actually executing.
///
/// \note [ul_slot_trace] (see record_slot_samples_complete() and the OCUDU_UL_SLOT_TRACE switch) is a PER-SLOT
///       TIMELINE, not a distribution, and it exists because every series above is one. A distribution cannot
///       answer "when did THIS slot's samples all arrive, and when did the work that reads them start": their
///       medians come from different populations, and their slot attribution differs when a receive block
///       straddles a slot boundary (record_start() keeps the FIRST block's sample timestamp, so a block carrying
///       the tail of slot N and the head of slot N+1 is charged to slot N+1). The trace records the one instant
///       the other series take for granted - the arrival of the samples that COMPLETE a slot - and prints the
///       deltas from it to those same landmarks.
///
/// \note THE TWO PROBES DO NOT SHARE A SAMPLE POPULATION, and until P0-5 nothing said so. The series above are
///       keyed by SLOT and completed only for a CRC-OK transport block; the gpu_lane_probe's residency/busy are
///       keyed by LANE (one thread's chained command buffers) and exist for every hop, PUSCH or not. On the leg
///       that exposed it (`s85-p0phases`) the phase segments had 60389 samples against 142022 lanes - a ratio of
///       0.425 - so "95% of the residency is busy" and "eq_demap is the residency" were readings across two
///       populations, not about one hop. set_phase_sample_observer() is the fix: the probe hands every sample it
///       FINALIZES (the same instant it pushes it into the three series above) to an observer that can pair it
///       with its own per-slot data, which is how the lane probe recomputes those two ratios on matched samples
///       and reports how many samples it could match at all.
class ul_pipeline_probe
{
public:
  static ul_pipeline_probe& get()
  {
    // NEVER DESTROYED ON PURPOSE, and this one is load-bearing for a SECOND reason: gpu_lane_probe's report is
    // an atexit handler and it READS this probe (`phase_samples_recorded()`, the pairing account of P0-5), so
    // this object has to outlive the static destructors - a function-local static does not, and locking its
    // (destroyed) mutex is `libc++abi: terminating ... mutex lock failed: Invalid argument` at exit. Measured
    // on leg `q9-conc2` (2026-09-25): the last two report lines were lost to exactly that. Same rule and same
    // deliberate leak as the lane probe's stats() and the DFT engine's dft_stats().
    static ul_pipeline_probe* instance = new ul_pipeline_probe();
    return *instance;
  }

  /// Which landmark of a slot a timestamp belongs to (see trace_slot()).
  enum class slot_trace_what { t2f, ce, ldpc_start, crc_ok };

  /// Bound on the traced slots: the trace is for reading single slots by eye, so a small map is the right size
  /// and an unbounded one on the hot path would not be.
  static constexpr size_t max_slot_trace = 512;

  /// One traced slot: every landmark as a DELTA from the arrival of the samples that completed it (µs).
  /// A field left at NaN means "this landmark was not reached for this slot" (a CRC failure has no crc_ok, a
  /// slot with no PUSCH has no ce/ldpc_start) - which is why they default to NaN rather than to 0.
  struct slot_trace_entry {
    uint64_t        slot   = 0;
    double          t2f_us        = std::numeric_limits<double>::quiet_NaN();
    double          ce_us         = std::numeric_limits<double>::quiet_NaN();
    double          ldpc_start_us = std::numeric_limits<double>::quiet_NaN();
    double          crc_ok_us     = std::numeric_limits<double>::quiet_NaN();
    double          rx_wait_us    = std::numeric_limits<double>::quiet_NaN();
    double          pipeline_us   = std::numeric_limits<double>::quiet_NaN();
    /// \brief The MAC PDU (transport block) this slot delivered, in bytes - the SIZE of the slot's work.
    ///
    /// This is the descriptor a reader needs to tell a slow slot that had a lot to do from a slow slot that had
    /// almost nothing: without it, a tail in the timeline cannot be attributed (5.9.67 (4) asked for exactly
    /// that attribution). It is the TRANSPORT BLOCK SIZE rather than (MCS, nof_prb) because that is the quantity
    /// the probe already receives, at the CRC-OK completion of the same slot - the product the two would give,
    /// and the one that says how many LLRs the demapper produced and how many codeblocks the decoder ran.
    /// NaN for a slot whose entry was created but whose PDU never completed (the trace is keyed on the first
    /// codeblock decode invocation, so this can only be an aborted or failed hop).
    double          tb_bytes      = std::numeric_limits<double>::quiet_NaN();
    /// \brief The two raw instants this row's deltas were computed against, SNAPSHOTTED at the row's last
    /// update rather than looked up when the report is printed.
    ///
    /// They exist so a reader can tell a real span from a difference between two unrelated origins (see
    /// print_slot_trace). Looking them up at print time broke exactly that: the row is keyed by the MODULAR
    /// slot, a later SFN cycle completes the same key again - overwriting slot_samples_done[slot] - and if that
    /// repeat carries no PUSCH there is no landmark update to refresh the row, so the printed base belonged to
    /// the NEW frame while the deltas beside it belonged to the old one. Measured on `s58-trace64`: 40 of 64
    /// rows, every one of them off by 10.238 s, which is the 10.24 s SFN cycle of this configuration.
    double          base_epoch_s  = std::numeric_limits<double>::quiet_NaN();
    double          mark_epoch_s  = std::numeric_limits<double>::quiet_NaN();
  };

  /// Records the start of the UL processing of a slot (call from the lower PHY baseband processor).
  /// \param[in] slot Slot number (SFN-referenced slot count, matching the FAPI slot indications).
  ///
  /// \note One slot may arrive in SEVERAL blocks: the receive policy decides the block size (see
  ///       lower_phy_baseband_processor::ul_process), and a symbol-grained policy calls this once per
  ///       symbol. The start of the series is the arrival of the slot's FIRST samples, so the earliest
  ///       call wins. Overwriting it with a later block - which is what this did while every block was
  ///       a whole slot - would shorten [ul_pipeline] and, through record_ldpc_start(),
  ///       [ul_time_frequency] by an amount that grows as the blocks get smaller: a receive-policy
  ///       change would report a latency win that no part of the pipeline earned.
  void record_start(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::high_resolution_clock::now();
    if (pending_starts.find(slot) == pending_starts.end()) {
      pending_starts[slot] = {now, next_start_seq++};
    }
    // Bound the registry by INSERTION ORDER (the slot count wraps every SFN cycle, so the key order is not a
    // valid age order): unmatched entries belong to idle slots (no PUSCH), drop the oldest insertion.
    evict_oldest(pending_starts);
  }

  /// Records the start of the LDPC decoder (call right before the first codeblock decode of a transport block).
  /// \param[in] slot Slot number of the PUSCH (same reference as record_end_crc_ok).
  ///
  /// \note In the fused-lane mode this call is ALSO the end of the [ul_gpu_pipeline] series (the LLRs are ready
  ///       here); outside it, it is the end of the equalization+demodulation phase segment. See the class comment.
  void record_ldpc_start(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::high_resolution_clock::now();
    trace_slot(slot, slot_trace_what::ldpc_start, now);
    pending_ldpc_starts[slot] = {now, next_start_seq++};
    // Bound the registry by insertion order (see record_start): unmatched entries belong to TBs that ended
    // without a CRC-OK completion (or with one in a shifted slot).
    evict_oldest(pending_ldpc_starts);

    if (in_fused_lane()) {
      // Fused lane (phy_pipeline_mode::gpu): the module boundaries the phase segments measure do not exist here, so
      // record the span they would have covered together instead - IQ arrival -> LLR ready. It ends at this very
      // instant (the LLRs are what this decode is about to consume), which is the boundary [ul_equalization_demod]
      // ends at outside the lane. The start entry is NOT consumed: the pipeline series reads the same one at the
      // CRC-OK completion, so both series pair the same start. Every decode attempt is recorded, CRC-OK or not (see
      // the class comment for the population difference against [ul_pipeline]).
      const auto start_it = find_fresh(pending_starts, slot, now);
      if (start_it != pending_starts.end()) {
        const int64_t iq_to_llr_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_it->second.tp).count();
        // Negative durations can only come from a mismatched (shifted-slot) pairing: drop the sample.
        if (iq_to_llr_ns >= 0) {
          const double iq_to_llr_us = static_cast<double>(iq_to_llr_ns) / 1e3;
          // Too late to be used (see stale_after_us): counted apart so that `max` stays "the slowest packet
          // that still mattered" instead of being the one number an idle stretch can move by 10x.
          if (iq_to_llr_us > static_cast<double>(stale_after_us())) {
            stale_gpu_pipeline_us.push_back(iq_to_llr_us);
          } else {
            gpu_pipeline_latencies_us.push_back(iq_to_llr_us);
          }
          // Handed to this slot's CRC-OK completion, which is where the transport block SIZE arrives - the
          // dimension [ul_by_size] stratifies by (see iq2llr_by_size_us). Bounded like the other registries: an
          // attempt whose slot never completes would otherwise leave one entry behind per attempt.
          pending_iq2llr_us[slot] = iq_to_llr_us;
          if (pending_iq2llr_us.size() > 256) {
            pending_iq2llr_us.erase(pending_iq2llr_us.begin());
          }
        }
      }
      // ... and the segments too when the diagnostic switch asks for them (OCUDU_UL_PHASE_SEGMENTS=1): the
      // total above is what the lane's criteria read, the segments are its decomposition.
      if (!phase_segments_forced()) {
        return;
      }
    }

    // Assemble the per-slot phase durations now that all the timestamps of this PUSCH are available (the
    // equalization+demodulation segment ends right here, at the first codeblock decode invocation). The lower
    // PHY-derived keys (start, t2f) are matched with the same small offset tolerance used at completion time;
    // the channel-estimation key carries the same FAPI slot reference as this call and is matched exactly.
    // Stale entries from previous slot-count wrap cycles are skipped (see max_entry_age).
    const auto start_it = find_fresh(pending_starts, slot, now);
    const auto t2f_it   = find_fresh(pending_t2f_ends, slot, now);
    const auto ce_it    = find_fresh_exact(pending_ce_ends, slot, now);
    if (start_it != pending_starts.end() && t2f_it != pending_t2f_ends.end() && ce_it != pending_ce_ends.end()) {
      const int64_t t2f_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(t2f_it->second.tp - start_it->second.tp).count();
      const int64_t ce_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(ce_it->second.tp - t2f_it->second.tp).count();
      const int64_t eqdem_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - ce_it->second.tp).count();
      // Negative durations can only come from a mismatched (shifted-slot) pairing: drop the entry.
      if (t2f_ns >= 0 && ce_ns >= 0 && eqdem_ns >= 0) {
        pending_phases[slot] = {now, t2f_ns, ce_ns, eqdem_ns, next_start_seq++};
        evict_oldest(pending_phases);
      }
    }
  }

  /// Records the completion of the OFDM demodulation (FFT) of the whole slot: the frequency-domain symbols of
  /// the slot are ready (call from the lower PHY PUxCH processor after the last symbol of the slot).
  /// \param[in] slot Slot number (same reference as record_start).
  void record_t2f_end(uint64_t slot)
  {
    if (!records_phase_segments()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto now         = std::chrono::high_resolution_clock::now();
    pending_t2f_ends[slot] = {now, next_start_seq++};
    evict_oldest(pending_t2f_ends);
    trace_slot(slot, slot_trace_what::t2f, now);
  }

  /// Records the completion of the PUSCH channel estimation: the channel estimates of all the data symbols of
  /// the slot are ready (call from the PUSCH processor at the start of the data processing).
  /// \param[in] slot Slot number of the PUSCH (same reference as record_end_crc_ok).
  void record_ce_end(uint64_t slot)
  {
    if (!records_phase_segments()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto now        = std::chrono::high_resolution_clock::now();
    pending_ce_ends[slot] = {now, next_start_seq++};
    evict_oldest(pending_ce_ends);
    trace_slot(slot, slot_trace_what::ce, now);
  }

  /// Returns the phase-segment durations assembled for a PUSCH (see record_ldpc_start()), if any.
  /// \param[in] slot Slot number of the PUSCH (same reference as record_ldpc_start()).
  std::optional<ul_phase_durations> get_phase_durations(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::high_resolution_clock::now();
    // Exact key: the phases entry was assembled for the same FAPI slot reference the caller carries.
    auto it = pending_phases.find(slot);
    if (it == pending_phases.end() || now - it->second.tp > max_entry_age) {
      // No entry, or a stale one left over from a previous slot-count wrap cycle (see max_entry_age).
      return std::nullopt;
    }
    return ul_phase_durations{std::chrono::nanoseconds(it->second.t2f_ns),
                              std::chrono::nanoseconds(it->second.ce_ns),
                              std::chrono::nanoseconds(it->second.eqdem_ns)};
  }

  /// Records the completion of the UL processing of a transport block whose CRC check passed.
  /// \param[in] slot Slot number of the PUSCH (same reference as record_start; a small offset is tolerated).
  /// \param[in] mac_pdu_bytes Size of the decoded MAC PDU in bytes (8-bit-granular); recorded only when the LDPC
  ///            latency sample is recorded, so the MAC-PDU-size series has the same sample count as the
  ///            [ul_ldpc_decode] series.
  void record_end_crc_ok(uint64_t slot, size_t mac_pdu_bytes)
  {
    std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();

    // The phase sample this call FINALIZES, if any, announced to the observer once the lock is released (see
    // set_phase_sample_observer()): the announcement must not happen under this probe's mutex, and it must
    // happen exactly for the samples that reach the three series below - which is what makes the observer's
    // count equal to their sample count by construction.
    int64_t observed_ns[3] = {0, 0, 0};
    bool    observed       = false;
    // unique_lock rather than lock_guard: the announcement at the end of this function has to happen with the
    // mutex RELEASED (see the comment above), and this is the one place in the probe that hands anything out.
    std::unique_lock<std::mutex> lock(mutex);
    trace_slot(slot, slot_trace_what::crc_ok, now);
    // CRC-OK completion timestamp for the FAPI->MAC tail-latency series (see record_fapi_mac_end): recorded
    // unconditionally, this method is only ever called for CRC-OK TBs.
    pending_crc_ok_ends[slot] = {now, next_start_seq++};
    evict_oldest(pending_crc_ok_ends);
    auto it = find_fresh(pending_starts, slot, now);
    if (it != pending_starts.end()) {
      double latency_us =
          static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.tp).count());
      // Carry the span into the slot's timeline while the start is still in hand (the trace's whole point is to
      // print it next to the landmark deltas, for the SAME slot - see print_slot_trace()).
      auto trace_it = slot_trace.find(slot);
      if (trace_it != slot_trace.end()) {
        trace_it->second.pipeline_us = latency_us;
        // The slot's SIZE, recorded here because this is the only instant the probe holds both the slot and the
        // transport block it carried (see slot_trace_entry::tb_bytes).
        trace_it->second.tb_bytes = static_cast<double>(mac_pdu_bytes);
      }
      pending_starts.erase(it);
      if (latency_us > static_cast<double>(stale_after_us())) {
        stale_pipeline_us.push_back(latency_us);
      } else {
        latencies_us.push_back(latency_us);
      }
      // [ul_by_size]: this is the ONE instant the probe holds both the hop's spans and the size of the work it
      // carried (see iq2llr_by_size_us). The IQ -> LLR span was recorded at the decode start and is waiting in
      // pending_iq2llr_us under this exact slot.
      {
        const size_t bucket = size_bucket_of(mac_pdu_bytes);
        pipe_by_size_us[bucket].push_back(latency_us);
        auto iq_it = pending_iq2llr_us.find(slot);
        if (iq_it != pending_iq2llr_us.end()) {
          iq2llr_by_size_us[bucket].push_back(iq_it->second);
          pending_iq2llr_us.erase(iq_it);
        }
      }
    }
    // LDPC decoder latency: match the start recorded for this TB by EXACT slot number - both ends of this
    // series carry the same FAPI slot reference (pdu.slot), so the offset tolerance of the pipeline series is
    // unnecessary here. It would only mis-pair a completion whose decode was skipped (codeblock CRC already OK)
    // with the orphan start of a failed attempt a few milliseconds earlier in a neighbouring slot (HARQ-RTT
    // scale, well inside max_entry_age). The staleness gate still rejects previous-cycle leftovers.
    auto ldpc_it = find_fresh_exact(pending_ldpc_starts, slot, now);
    if (ldpc_it != pending_ldpc_starts.end()) {
      const auto ldpc_us =
          std::chrono::duration_cast<std::chrono::microseconds>(now - ldpc_it->second.tp);
      pending_ldpc_starts.erase(ldpc_it);
      ldpc_latencies_us.push_back(static_cast<double>(ldpc_us.count()));
      mac_pdu_sizes_bytes.push_back(static_cast<double>(mac_pdu_bytes));

      // Phase-segment durations (time-frequency / channel estimation / equalization+demodulation), recorded
      // only together with an LDPC latency sample, so the sample counts of the series always match
      // [ul_ldpc_decode] (and only outside the fused lane: see records_phase_segments()).
      // Exact key, same FAPI slot reference as the assembly (see the LDPC comment above).
      auto phases_it = pending_phases.find(slot);
      if (phases_it != pending_phases.end() && now - phases_it->second.tp > max_entry_age) {
        // Stale entry from a previous slot-count wrap cycle: drop it instead of recording its (plausible-looking
        // but wrong-slot) durations.
        pending_phases.erase(phases_it);
        phases_it = pending_phases.end();
      }
      if (phases_it != pending_phases.end()) {
        t2f_latencies_us.push_back(static_cast<double>(phases_it->second.t2f_ns) / 1e3);
        ce_latencies_us.push_back(static_cast<double>(phases_it->second.ce_ns) / 1e3);
        eqdem_latencies_us.push_back(static_cast<double>(phases_it->second.eqdem_ns) / 1e3);
        // Handed to the observer below, once the lock is gone. Taken HERE, in the branch that pushes the three
        // series, so that "announced" and "recorded" cannot drift apart.
        observed_ns[0] = phases_it->second.t2f_ns;
        observed_ns[1] = phases_it->second.ce_ns;
        observed_ns[2] = phases_it->second.eqdem_ns;
        observed       = true;
        pending_phases.erase(phases_it);
      }
    }

    // The announcement, with the mutex RELEASED: an observer that took this probe's mutex would deadlock, and one
    // whose own lock is held across the call would put its latency inside this probe's.
    lock.unlock();
    if (observed) {
      if (phase_sample_observer_t observer = phase_sample_observer().load(std::memory_order_acquire);
          observer != nullptr) {
        observer(slot, observed_ns[0], observed_ns[1], observed_ns[2]);
      }
    }
  }

  /// Records the arrival of a CRC-OK transport block at the MAC UL task enqueue point (call from the MAC UL
  /// processor right after the per-PDU executor enqueue). Matched against the CRC-OK completion timestamp of the
  /// same slot (see record_end_crc_ok) to produce the FAPI->MAC tail latency.
  /// \param[in] slot Slot number of the PUSCH (same reference as record_end_crc_ok).
  void record_fapi_mac_end(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::high_resolution_clock::now();
    auto       it  = find_fresh_exact(pending_crc_ok_ends, slot, now);
    if (it == pending_crc_ok_ends.end()) {
      // No CRC-OK completion on record for this slot (already consumed, dropped at the FAPI gate, or a stale
      // entry from a previous slot-count wrap cycle).
      return;
    }
    const int64_t fapi_mac_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - it->second.tp).count();
    pending_crc_ok_ends.erase(it);
    // Negative durations can only come from a mismatched pairing: drop the sample.
    if (fapi_mac_ns >= 0) {
      fapi_mac_latencies_us.push_back(static_cast<double>(fapi_mac_ns) / 1e3);
    }
  }

  /// Records how long the receive blocked: the two calls bracket ONE receiver.receive(), so unlike every other
  /// series here there is no pairing to get wrong (no slot key, no staleness gate, no negative-duration case).
  /// \param[in] wait Nanoseconds the host spent inside receive(). Negative values are dropped (they can only come
  ///                 from a caller that mixed the two ends up).
  ///
  /// \note Counted per BLOCK, not per slot: the block size is the receive policy's (see ul_process), so under the
  ///       whole-slot policy this series has one sample per slot and under the symbol-grained one it has one per
  ///       block. Compare its counts against the policy in force, not against [ul_pipeline]'s.
  void record_rx_wait(int64_t wait_ns)
  {
    if (wait_ns < 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    rx_wait_us.push_back(static_cast<double>(wait_ns) / 1e3);
  }

  /// \brief Records that a received block completed \p slot, i.e. its samples carry the slot's LAST sample.
  ///
  /// \param[in] slot       Slot whose samples are now all in.
  /// \param[in] block_size Number of samples the block delivered.
  /// \param[in] wait_ns    How long that block's receive blocked.
  /// \param[in] now        Host timestamp of that arrival.
  ///
  /// Only recorded while the [ul_slot_trace] switch is on (OCUDU_UL_SLOT_TRACE=N), because it is the ONE instant
  /// none of the other series needs: they all start earlier, at the first sample of the slot. With it, the trace
  /// separates "the front end's work on the samples" from "waiting for the samples", which is exactly the pair
  /// the operator of §5.8.30 could not read out of the distributions.
  ///
  /// The block's SAMPLE RANGE is taken as the input rather than a "which block completed the slot" answer,
  /// because the caller cannot always tell: the receiving chain's stream carries a fixed offset from the slot
  /// grid (measured on air: 7 samples, matching the symbol-block size), so no block ever ENDS on a slot boundary
  /// - the slot's last sample merely falls INSIDE one. Asking the caller to test "ends on a boundary" is what
  /// made a half-slot leg capture nothing at all while looking perfectly reasonable in review.
  void record_slot_samples_complete(uint64_t                                      slot,
                                    unsigned                                      block_size,
                                    int64_t                                       wait_ns,
                                    std::chrono::high_resolution_clock::time_point now)
  {
    if (!slot_trace_enabled() || (block_size == 0)) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    // NOTE: this map is deliberately NOT capped here. The budget belongs to the slots that CARRY a PUSCH, and
    // which those are is only known later (see trace_slot) - capping on completion keeps the run's first
    // milliseconds, which is what an earlier version did and why every printed row had no PUSCH. It grows one
    // entry per slot for the length of a trace that is opt-in anyway; the traced rows themselves are capped.
    slot_samples_done[slot] = now;
    if (wait_ns >= 0) {
      slot_trace_pre_wait[slot] = static_cast<double>(wait_ns) / 1e3;
    }
  }

  /// \brief Whether the per-slot timeline is on (OCUDU_UL_SLOT_TRACE=N, N bounded), and its bound.
  ///
  /// Off by default: the trace keeps a per-slot timestamp map, which the hot path must not pay for in a normal
  /// run. This is the one switch here whose absence is not silent - report() prints a line saying whether the
  /// trace was on and how many slots it captured - because a leg is expensive and "the trace was off" must not
  /// look like "the trace found nothing".
  static unsigned slot_trace_limit()
  {
    const char* env = std::getenv("OCUDU_UL_SLOT_TRACE");
    if (env == nullptr) {
      return 0;
    }
    // "OCUDU_UL_SLOT_TRACE=1" (or any non-numeric value) means "on, with the default number of slots": an
    // operator asking for the trace should not have to know a good count, and strtoul() would read "1" as one
    // slot - which is not enough to see a pattern and looks like a broken instrument.
    const unsigned v = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
    if (v <= 1) {
      return (max_slot_trace <= 128) ? static_cast<unsigned>(max_slot_trace) : 128U;
    }
    return (v > max_slot_trace) ? static_cast<unsigned>(max_slot_trace) : v;
  }
  /// \brief How much of the timeline's budget is reserved for the SLOWEST rows (see trace_eviction_victim()).
  static size_t slot_trace_slow_reserve() { return std::max<size_t>(1, slot_trace_limit() / 4); }

  /// \brief Which row to drop when the timeline is full.
  ///
  /// The policy used to be "the oldest", i.e. the timeline kept the NEWEST slots that carried a PUSCH - and that
  /// is blind to the very thing it is now used to find. Measured on `s58-trace64` (2026-09-23): its 64 rows held
  /// NO slow hop at all (largest pipeline 3115 us) while the same leg reported p95 = 5590 us, and `s57-trace` had
  /// caught the 60 ms stall only because its 512 rows happened to reach back far enough to still contain it.
  /// A tail is a TRANSIENT: keeping only the newest rows throws the evidence away as the run goes on.
  ///
  /// So a quarter of the budget (`slot_trace_slow_reserve()`, at least one row) is reserved for the slowest rows,
  /// and the victim is the OLDEST row that is not among them. A slow row can still be displaced - by a slower one,
  /// which is the point - and if every row is protected (a very small bound) the oldest goes anyway, so the bound
  /// is never exceeded.
  uint64_t trace_eviction_victim()
  {
    // The reserve, by pipeline span, oldest first on a tie, and rows with no span YET (created but not completed)
    // last: an incomplete row is not evidence of anything slow.
    std::vector<std::pair<double, uint64_t>> by_span;
    by_span.reserve(slot_trace_order.size());
    for (uint64_t slot : slot_trace_order) {
      auto         it   = slot_trace.find(slot);
      const double span = ((it == slot_trace.end()) || std::isnan(it->second.pipeline_us))
                              ? -std::numeric_limits<double>::infinity()
                              : it->second.pipeline_us;
      by_span.emplace_back(span, slot);
    }
    // stable_sort keeps the age order on a tie, so the older of two equally slow rows is the one dropped.
    std::stable_sort(by_span.begin(), by_span.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.first > rhs.first;
    });
    std::vector<uint64_t> kept;
    for (size_t i = 0; (i != by_span.size()) && (kept.size() != slot_trace_slow_reserve()); ++i) {
      kept.push_back(by_span[i].second);
    }
    for (uint64_t slot : slot_trace_order) {
      if (std::find(kept.begin(), kept.end(), slot) == kept.end()) {
        return slot;
      }
    }
    return slot_trace_order.front();
  }

  /// Whether the per-slot timeline is on (public so the lower PHY can gate its own diagnostics on it).
  static bool slot_trace_enabled() { return slot_trace_limit() != 0; }

  /// \brief How long the host BLOCKED waiting for the front-end DFT of a slot (call from the DFT engine's
  /// wait_slot(), around its waitUntilCompleted).
  ///
  /// This is the one host synchronization D1 exists to remove. In the lane route the grid is device-resident and
  /// the consumer waits for it on the DEVICE, so the demodulator takes its host wait once per slot instead of once
  /// per symbol - which makes this series the price of that arrangement, measured rather than assumed:
  ///
  ///   * if it is ~0, the host is never actually held up (the DFT is always done by the time the slot's last
  ///     symbol arrives) and D1's "one submission per hop" is worth only the CPU time it saves;
  ///   * if it is a large fraction of a slot, the host IS held up every slot, and removing the wait is worth
  ///     real latency.
  ///
  /// Recorded per WAIT, not per slot: a route that waits per symbol records one per symbol (see the demodulator).
  void record_dft_wait(int64_t wait_ns)
  {
    if (wait_ns < 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    dft_wait_us.push_back(static_cast<double>(wait_ns) / 1e3);
  }

  /// \brief Remembers one landmark instant of a slot, and CREATES its timeline entry when the landmark proves
  /// the slot carries a PUSCH.
  ///
  /// Why the entry is created here and not at the samples-complete instant: a slot whose samples are complete is
  /// not necessarily a slot anyone transmits in. On air the FFT (and so record_t2f_end) runs for slots that carry
  /// no PUSCH at all, and a trace keyed on completion spends its whole budget on those - measured: the first leg
  /// with this trace reported 50 rows, ALL of them with no channel estimation, no decode and no CRC, covering
  /// half a second at the start of the run. Keying on the FIRST CODEBLOCK DECODE INVOCATION instead
  /// (record_ldpc_start(), which the PUSCH processor calls for every decode attempt) makes the rows the slots the
  /// operator is asking about, and `ldpc_start_us` cannot be NaN in any of them by construction.
  ///
  /// The landmarks of such a slot that arrived BEFORE this one (the FFT end in particular) are already in hand,
  /// so they are backfilled rather than lost.
  void trace_slot(uint64_t slot, slot_trace_what what, std::chrono::high_resolution_clock::time_point at)
  {
    if (slot_samples_done.find(slot) == slot_samples_done.end()) {
      return; // no samples-complete instant on record: this slot cannot be a timeline
    }
    slot_landmarks[{slot, what}] = at;

    // A slot becomes a timeline at the landmark that proves it carries a PUSCH - the first codeblock decode
    // invocation - and STAYS one afterwards: crc_ok arrives last, so a rule that only handled the creating
    // landmark would leave the CRC column NaN in every row. (It did: an early `what != ldpc_start -> return`
    // sat in front of the entry write, so crc_ok stored its instant in the landmark map and then returned
    // before anything read it back. The guard below is the same rule written so that it cannot do that.)
    //
    // NO lock here: every caller (record_t2f_end, record_ce_end, record_ldpc_start, record_end_crc_ok) already
    // holds `mutex` when it calls this. Taking it again deadlocks - the first version of this refactor did, and
    // the probe test hung instead of failing, which is the failure mode a non-recursive mutex gives you.
    const bool is_new = (slot_trace.find(slot) == slot_trace.end());
    if (is_new && (what != slot_trace_what::ldpc_start)) {
      return; // the landmark that does NOT prove a PUSCH, for a slot that has no timeline to update
    }
    // The budget applies to the slots that CARRY a PUSCH: evict the oldest of those when full.
    //
    // The bound is the one the OPERATOR asked for, not the internal maximum. It used to compare against
    // max_slot_trace, so OCUDU_UL_SLOT_TRACE bounded nothing: `=64` printed 512 rows (measured on s57-trace,
    // 2026-09-22) and the header line then reported a bound it had not applied - "TRACE=64, rows=512" is the
    // contradiction the printer's own comment says a reader would be right to flag.
    // A `while`, not an `if`: one eviction per insertion holds the bound steady while it does not move, but it
    // cannot bring an ALREADY collected set down to a bound that was lowered afterwards, and the trace is a
    // process-wide singleton whose bound is read from the environment on every call (the test below toggles it
    // inside one process, which is how this was found). The order list is what says which slot is oldest; if it
    // were ever empty while the map is not, adding the row is the safe direction to fail in.
    const auto drop_slot = [&](uint64_t victim) {
      slot_samples_done.erase(victim);
      slot_trace_pre_wait.erase(victim);
      slot_trace.erase(victim);
      for (auto it = slot_landmarks.begin(); it != slot_landmarks.end();) {
        it = (it->first.first == victim) ? slot_landmarks.erase(it) : std::next(it);
      }
      auto pos = std::find(slot_trace_order.begin(), slot_trace_order.end(), victim);
      if (pos != slot_trace_order.end()) {
        slot_trace_order.erase(pos);
      }
    };
    while (is_new && !slot_trace_order.empty() && (slot_trace.size() >= slot_trace_limit())) {
      drop_slot(trace_eviction_victim());
    }
    slot_trace_entry& e = slot_trace[slot];
    e.slot              = slot;
    if (is_new) {
      slot_trace_order.push_back(slot);
    }

    // Backfill every landmark of this slot, each measured from its samples-complete instant.
    const auto base = slot_samples_done.at(slot);
    auto       us   = [&base](const std::chrono::high_resolution_clock::time_point& tp) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(tp - base).count() / 1e3;
    };
    auto assign = [&](slot_trace_what w, double& field) {
      auto it = slot_landmarks.find({slot, w});
      if (it != slot_landmarks.end()) {
        field = us(it->second);
      }
    };
    assign(slot_trace_what::t2f, e.t2f_us);
    assign(slot_trace_what::ce, e.ce_us);
    assign(slot_trace_what::ldpc_start, e.ldpc_start_us);
    assign(slot_trace_what::crc_ok, e.crc_ok_us);
    auto wait_it = slot_trace_pre_wait.find(slot);
    if (wait_it != slot_trace_pre_wait.end()) {
      e.rx_wait_us = wait_it->second;
    }
    // The raw instants, taken here so that they always describe the same frame as the deltas above (see
    // slot_trace_entry::base_epoch_s).
    e.base_epoch_s = std::chrono::duration<double>(base.time_since_epoch()).count();
    e.mark_epoch_s = std::chrono::duration<double>(at.time_since_epoch()).count();
  }

  /// \brief Prints the per-slot timelines captured by OCUDU_UL_SLOT_TRACE.
  ///
  /// One line per slot, every landmark expressed as a delta in microseconds from "the slot's last sample
  /// arrived". The line also carries the slot's [ul_time_frequency] and, when it was recorded, the receive wait
  /// of the block that completed it, so the two decompositions can be read against each other for the SAME slot
  /// rather than across populations.
  void print_slot_trace()
  {
    std::vector<slot_trace_entry> trace;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& kv : slot_trace) {
        trace.push_back(kv.second);
      }
    }
    if (!slot_trace_enabled() && trace.empty()) {
      return; // the switch was off and nothing was captured: stay silent (the run does not ask for this)
    }
    // The slot list is printed whenever it is non-empty, even if the switch has been turned off since (the report
    // runs at shutdown, and the environment it echoes is the CURRENT one - the test toggles it inside one
    // process). Saying "OCUDU_UL_SLOT_TRACE=0, slots captured=1" would read as a contradiction; naming the
    // captured count and, when the switch is still on, the bound, says what actually happened.
    // The count printed is the number of ROWS, and the bound printed is the current one: the two can disagree
    // (the switch can be changed, and the test toggles it inside one process), so both are named rather than
    // presented as a pair. A reader who takes "TRACE=64, captured=512" for a defect is reading it correctly as a
    // contradiction - so say which is which.
    if (slot_trace_enabled()) {
      std::fprintf(stderr,
                   "[ul_slot_trace] rows=%zu (bound now OCUDU_UL_SLOT_TRACE=%u; rows are the slots that carried"
                   " a PUSCH, and the slowest %zu of them are kept against age - see trace_eviction_victim())\n",
                   trace.size(),
                   slot_trace_limit(),
                   slot_trace_slow_reserve());
    } else {
      std::fprintf(stderr, "[ul_slot_trace] rows=%zu (switch now off)\n", trace.size());
    }
    if (trace.empty()) {
      std::fprintf(stderr, "[ul_slot_trace] no slot completed on record (see record_slot_samples_complete())\n");
      return;
    }
    std::fprintf(stderr,
                 "  %-8s %10s %10s %10s %10s %10s %10s %10s %12s %12s\n",
                 "slot",
                 "rxwait",
                 "t2f",
                 "ce",
                 "ldpc",
                 "crc_ok",
                 "tb_bytes",
                 "pipeline",
                 "base_since_boot",
                 "landmark_since_boot");
    for (const auto& e : trace) {
      // The two raw instants, in seconds since the CLOCK's own epoch: they are what says whether a delta is a
      // real span or a difference between two unrelated origins. A delta of "one slot" that shows up next to a
      // base of 0 is a base that was never set, and no amount of staring at the delta will reveal that.
      //
      // They come from the ROW, not from a lookup here: the row is keyed by the modular slot, and a later SFN
      // cycle can overwrite slot_samples_done[slot] without refreshing this row (the repeat carried no PUSCH, so
      // no landmark update came), which printed a base one SFN cycle away from the deltas beside it. See
      // slot_trace_entry::base_epoch_s - measured: 40 of 64 rows, all off by 10.238 s.
      const double base_s = e.base_epoch_s;
      const double mark_s = e.mark_epoch_s;
      std::fprintf(stderr,
                   "  %-8llu %10.1f %10.1f %10.1f %10.1f %10.1f %10.0f %10.1f %12.3f %12.3f\n",
                   static_cast<unsigned long long>(e.slot),
                   e.rx_wait_us,
                   e.t2f_us,
                   e.ce_us,
                   e.ldpc_start_us,
                   e.crc_ok_us,
                   // NOT e.t2f_us a second time: this column used to print that, so two of the row's ten
                   // columns were the same number and a reader comparing them "agreed" for free.
                   e.tb_bytes,
                   e.pipeline_us,
                   base_s,
                   mark_s);
    }
  }

  /// \brief Prints the two per-hop spans stratified by the size of the hop's work (see iq2llr_by_size_us).
  ///
  /// Silent when nothing was recorded - a build outside the fused lane has no IQ -> LLR span to stratify, and a
  /// leg that decoded nothing has no transport blocks, so there is no table to print rather than a table of
  /// zeroes that reads like a measurement.
  void print_by_size()
  {
    size_t total = 0;
    for (const std::vector<double>& v : pipe_by_size_us) {
      total += v.size();
    }
    if (total == 0) {
      return;
    }
    std::fprintf(stderr,
                 "[ul_by_size] the hop's own spans (us) against the transport block it carried - %zu CRC-OK hop(s)\n",
                 total);
    std::fprintf(stderr,
                 "  %-10s %8s %12s %12s %12s %12s\n",
                 "tb_bytes",
                 "samples",
                 "iq2llr_med",
                 "iq2llr_p95",
                 "pipe_med",
                 "pipe_p95");
    auto pct = [](const std::vector<double>& sorted, double p) {
      return sorted[static_cast<size_t>((sorted.size() - 1) * p)];
    };
    const double nan          = std::numeric_limits<double>::quiet_NaN();
    double       first_iq_med = nan;
    double       last_iq_med  = nan;
    for (size_t i = 0; i != nof_size_buckets; ++i) {
      std::vector<double> iq = iq2llr_by_size_us[i];
      std::vector<double> pp = pipe_by_size_us[i];
      if (pp.empty()) {
        std::fprintf(stderr, "  %-10s %8s %12s %12s %12s %12s\n", size_bucket_name(i), "0", "-", "-", "-", "-");
        continue;
      }
      std::sort(iq.begin(), iq.end());
      std::sort(pp.begin(), pp.end());
      const double iq_med = iq.empty() ? nan : pct(iq, 0.5);
      if (std::isnan(first_iq_med)) {
        first_iq_med = iq_med;
      }
      last_iq_med = iq_med;
      std::fprintf(stderr,
                   "  %-10s %8zu %12.1f %12.1f %12.1f %12.1f\n",
                   size_bucket_name(i),
                   pp.size(),
                   iq_med,
                   iq.empty() ? nan : pct(iq, 0.95),
                   pct(pp, 0.5),
                   pct(pp, 0.95));
    }
    // One line for the question the table exists for, as a FACT rather than a verdict: how far the median
    // IQ -> LLR span moved from the smallest bucket that has samples to the largest. Near 1.0 says the tail is
    // not this hop's own work; clearly above 1 says it is, and the fix belongs in the lane's dispatch count.
    if (!std::isnan(first_iq_med) && (first_iq_med > 0.0) && !std::isnan(last_iq_med)) {
      std::fprintf(stderr,
                   "[ul_by_size] iq2llr median: smallest bucket %.1fus -> largest bucket %.1fus (x%.2f)\n",
                   first_iq_med,
                   last_iq_med,
                   last_iq_med / first_iq_med);
    }
  }

  /// \brief Observer of every FINALIZED phase sample (P0-5: the pairing key this probe and the lane probe share).
  ///
  /// It is called once per sample that enters [ul_time_frequency] / [ul_channel_estimation] /
  /// [ul_equalization_demod], with the slot the sample belongs to and the three durations, so a probe that
  /// measures the same hops from another side (gpu_lane_probe: residency/busy per LANE) can pair its own
  /// numbers with a sample that is known to describe the SAME hop. Without it the two reports can only be
  /// compared across populations, which is what made "residency is ~95% busy" and "eq_demap is the residency"
  /// indicatory rather than measured (see the class comment).
  ///
  /// Called with this probe's mutex RELEASED: an observer that wants to take it back would deadlock, and one
  /// whose own lock is held across the call would put its latency inside this probe's. The call therefore
  /// happens after the sample is recorded, and every phase sample this probe accepts is announced exactly once.
  using phase_sample_observer_t = void (*)(uint64_t slot, int64_t t2f_ns, int64_t ce_ns, int64_t eqdem_ns);

  /// Registers the observer (nullptr unregisters). Called once, by the probe that wants the samples.
  static void set_phase_sample_observer(phase_sample_observer_t observer)
  {
    phase_sample_observer().store(observer, std::memory_order_release);
  }

  /// \brief How many samples the three phase-segment series hold RIGHT NOW.
  ///
  /// Exists for the PAIRING ACCOUNT (P0-5, see gpu_lane_probe::note_phase_sample()): report() prints a
  /// SNAPSHOT of those series taken when it ran, early in the shutdown, while the observer that feeds the lane
  /// probe keeps counting until the process exits. Measured on `p05-pair`: the series line printed 73528 and
  /// the lane report (at exit) had 73529 - one sample finalized in between - and a reader comparing those two
  /// numbers reads a mismatch where there is none. The lane probe therefore asks for this count AT EXIT and
  /// prints it next to what it paired, so the account can be compared against numbers taken at one instant.
  size_t phase_samples_recorded()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return t2f_latencies_us.size();
  }

  /// Prints the statistics of the recorded latencies. Called once during the application shutdown.
  void report()
  {
    std::vector<double> sorted_pipeline;
    std::vector<double> sorted_ldpc;
    std::vector<double> sorted_pdu_sizes;
    std::vector<double> sorted_t2f;
    std::vector<double> sorted_ce;
    std::vector<double> sorted_eqdem;
    std::vector<double> sorted_gpu_pipeline;
    std::vector<double> sorted_stale_pipeline;
    std::vector<double> sorted_stale_gpu_pipeline;
    std::vector<double> sorted_fapi_mac;
    std::vector<double> sorted_rx_wait;
    std::vector<double> sorted_dft_wait;
    {
      std::lock_guard<std::mutex> lock(mutex);
      sorted_pipeline     = latencies_us;
      sorted_ldpc         = ldpc_latencies_us;
      sorted_pdu_sizes    = mac_pdu_sizes_bytes;
      sorted_t2f          = t2f_latencies_us;
      sorted_ce           = ce_latencies_us;
      sorted_eqdem        = eqdem_latencies_us;
      sorted_gpu_pipeline = gpu_pipeline_latencies_us;
      sorted_stale_pipeline     = stale_pipeline_us;
      sorted_stale_gpu_pipeline = stale_gpu_pipeline_us;
      sorted_fapi_mac     = fapi_mac_latencies_us;
      sorted_rx_wait      = rx_wait_us;
      sorted_dft_wait     = dft_wait_us;
    }
    auto pct = [](const std::vector<double>& sorted, double p) {
      return sorted[static_cast<size_t>((sorted.size() - 1) * p)];
    };
    auto print_series = [&pct](const char* name, std::vector<double>& sorted) {
      if (sorted.empty()) {
        std::fprintf(stderr, "[%s] no samples recorded\n", name);
        return;
      }
      std::sort(sorted.begin(), sorted.end());
      double series_sum = 0;
      for (double v : sorted) {
        series_sum += v;
      }
      std::fprintf(stderr,
                   "[%s] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
                   name,
                   sorted.size(),
                   series_sum / static_cast<double>(sorted.size()),
                   pct(sorted, 0.5),
                   sorted.front(),
                   sorted.back(),
                   pct(sorted, 0.95),
                   pct(sorted, 0.99));
    };

    /// \brief The samples the series above LEFT OUT because they are too late to be used.
    ///
    /// Printed next to its series rather than folded in, because the two answer different questions: the
    /// series is "how long does a useful hop take", this is "how often did a hop finish after the MAC had
    /// already given up on it". A run with a large `stale` count is a run with a stall, and the series'
    /// `max` is then still the slowest USEFUL packet - which is what makes `max` readable at all.
    auto print_stale = [](const char* series, const std::vector<double>& stale) {
      if (stale.empty()) {
        std::fprintf(stderr, "[%s] stale=0\n", series);
        return;
      }
      double stale_sum = 0;
      double stale_max = 0;
      for (double v : stale) {
        stale_sum += v;
        stale_max = (v > stale_max) ? v : stale_max;
      }
      std::fprintf(stderr,
                   "[%s] stale=%zu (span > %llu us, the uplink HARQ round trip) mean=%.1fus max_stale=%.1fus\n",
                   series,
                   stale.size(),
                   static_cast<unsigned long long>(stale_after_us()),
                   stale_sum / static_cast<double>(stale.size()),
                   stale_max);
    };

    double sum = 0;
    // Report to stderr (guaranteed to be visible at the shutdown, unlike the logging backend) and to the logs.
    // The series are independent: a run with no CRC-OK transport block still reports the ones it did record (the
    // fused-lane span below in particular, which is recorded per decode attempt rather than per successful one).
    if (sorted_pipeline.empty()) {
      std::fprintf(stderr, "[ul_pipeline] no CRC-OK samples recorded\n");
    } else {
      std::sort(sorted_pipeline.begin(), sorted_pipeline.end());
      sum = 0;
      for (double v : sorted_pipeline) {
        sum += v;
      }
      std::fprintf(stderr,
                   "[ul_pipeline] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus "
                   "p99=%.1fus\n",
                   sorted_pipeline.size(),
                   sum / static_cast<double>(sorted_pipeline.size()),
                   pct(sorted_pipeline, 0.5),
                   sorted_pipeline.front(),
                   sorted_pipeline.back(),
                   pct(sorted_pipeline, 0.95),
                   pct(sorted_pipeline, 0.99));
      print_stale("ul_pipeline", sorted_stale_pipeline);
    }

    // Outside the fused lane: the phase-segment series, printed in pipeline order. Recorded in lockstep with the
    // [ul_ldpc_decode] series (CRC-OK completions only), so their sample counts always match it. Their sum is the
    // same span the fused lane reports as one number below.
    // Inside the fused lane: that single span instead. The per-module boundaries the three segments measure do not
    // exist there, so their numbers would be CPU-side artifacts; what the lane does cover end to end is exactly
    // 'IQ samples in -> LLRs out' (read it together with the gpu_lane_probe residency / busy / gap split, which
    // says how much of that window the device was actually executing).
    if (in_fused_lane()) {
      print_series("ul_gpu_pipeline", sorted_gpu_pipeline);
      print_stale("ul_gpu_pipeline", sorted_stale_gpu_pipeline);
    }
    if (records_phase_segments()) {
      print_series("ul_time_frequency", sorted_t2f);
      print_series("ul_channel_estimation", sorted_ce);
      print_series("ul_equalization_demod", sorted_eqdem);
    }
    // The receive's own series, and the only one whose count is per BLOCK rather than per slot or per TB (see
    // record_rx_wait). Printed next to the pipeline it is part of, because [ul_time_frequency] includes it.
    print_series("ul_rx_wait", sorted_rx_wait);
    print_series("ul_dft_wait", sorted_dft_wait);
    print_slot_trace();
    print_by_size();
    // The series printed below cross both modes unchanged.
    // FAPI->MAC tail (CRC-OK -> MAC UL task enqueue): recorded in lockstep with the CRC-OK completions, so its
    // sample count tracks [ul_ldpc_decode] (minus PDUs dropped at the per-UE queue).
    print_series("ul_fapi_mac", sorted_fapi_mac);

    if (sorted_ldpc.empty()) {
      std::fprintf(stderr, "[ul_ldpc_decode] no samples recorded\n");
      return;
    }
    std::sort(sorted_ldpc.begin(), sorted_ldpc.end());
    sum = 0;
    for (double v : sorted_ldpc) {
      sum += v;
    }
    std::fprintf(stderr,
                 "[ul_ldpc_decode] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
                 sorted_ldpc.size(),
                 sum / static_cast<double>(sorted_ldpc.size()),
                 pct(sorted_ldpc, 0.5),
                 sorted_ldpc.front(),
                 sorted_ldpc.back(),
                 pct(sorted_ldpc, 0.95),
                 pct(sorted_ldpc, 0.99));
    // MAC PDU size (CRC-OK data bursts): recorded in the same branch as the LDPC latency samples, so the sample
    // count matches [ul_ldpc_decode]. Printed after it, plus a second line with the total number of bytes.
    if (sorted_pdu_sizes.empty()) {
      std::fprintf(stderr, "[ul_mac_pdu_size] no samples recorded\n");
      return;
    }
    std::sort(sorted_pdu_sizes.begin(), sorted_pdu_sizes.end());
    sum = 0;
    for (double v : sorted_pdu_sizes) {
      sum += v;
    }
    std::fprintf(stderr,
                 "[ul_mac_pdu_size] samples=%zu mean=%.1fB median=%.1fB min=%.1fB max=%.1fB p95=%.1fB p99=%.1fB\n",
                 sorted_pdu_sizes.size(),
                 sum / static_cast<double>(sorted_pdu_sizes.size()),
                 pct(sorted_pdu_sizes, 0.5),
                 sorted_pdu_sizes.front(),
                 sorted_pdu_sizes.back(),
                 pct(sorted_pdu_sizes, 0.95),
                 pct(sorted_pdu_sizes, 0.99));
    std::fprintf(stderr, "[ul_mac_pdu_size] total=%.1fB\n", sum);
  }

private:
  ul_pipeline_probe() = default;

  /// \brief Whether the phase segments are FORCED on (OCUDU_UL_PHASE_SEGMENTS=1), for diagnostics.
  ///
  /// The question the three segments answer - "which part of the IQ -> LLR span is which" - is the same
  /// one inside the fused lane, and inside it they are the only decomposition of that span this probe can
  /// produce: their ends are the same instants that bound [ul_gpu_pipeline], so the three add up to it by
  /// construction. What they do NOT mean in the lane is "CPU work": the lane's segments contain device
  /// execution and queueing, and the module boundaries they are named after do not exist there (see
  /// records_phase_segments()). So the switch is off by default and this is a diagnostic, to be read
  /// together with the gpu_lane_probe residency / busy / gap report.
  /// Whether the effective mode is the fused lane. Read where a decision has to follow the MODE rather than
  /// records_phase_segments(): the diagnostic switch changes the latter but must not change what the lane records.
  static bool in_fused_lane() { return phy_pipeline_mode_registry::get() == phy_pipeline_mode::gpu; }

  static bool phase_segments_forced()
  {
    // Read per call rather than cached in a static: the switch is consulted a few times per slot - not per
    // dispatch - and the unit test has to be able to toggle it inside one process (the same reason
    // shared_queue::front_end_fence_enabled() reads its own switch on every call).
    const char* env = std::getenv("OCUDU_UL_PHASE_SEGMENTS");
    return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
  }

  /// The registered phase-sample observer (see set_phase_sample_observer()). Atomic because it is written once
  /// at startup by one probe and read on the recording path by another thread: a plain function pointer would be
  /// a data race the tools that compile this header with a sanitizer would (rightly) refuse.
  static std::atomic<phase_sample_observer_t>& phase_sample_observer()
  {
    static std::atomic<phase_sample_observer_t> observer{nullptr};
    return observer;
  }

  /// Whether the per-module phase segments (time-frequency / channel estimation / equalization+demodulation) are
  /// recorded. They measure the CPU side of the module boundaries, which the fused lane (phy_pipeline_mode::gpu)
  /// removes altogether: recording them there would only add probe overhead to the lane, and reporting them would
  /// revive the "it got faster" illusion (the work merely moved out of the measured window). What replaces them there
  /// is the single span they add up to ([ul_gpu_pipeline], see record_ldpc_start()). The mode is published once at
  /// startup by the application (see phy_pipeline_mode_registry).
  ///
  /// \note In the lane the override does NOT replace [ul_gpu_pipeline]: both series are recorded and both are
  ///       printed (see record_ldpc_start() and report()), because the total is what the lane's criteria read and
  ///       the segments are only its decomposition.
  static bool records_phase_segments()
  {
    return phase_segments_forced() || (phy_pipeline_mode_registry::get() != phy_pipeline_mode::gpu);
  }

  /// Registry entry: start timestamp plus a monotonic insertion sequence (the slot key wraps every SFN cycle,
  /// so it cannot serve as the age order for the bounded-registry eviction).
  struct start_entry {
    std::chrono::time_point<std::chrono::high_resolution_clock> tp;
    uint64_t                                                     seq;
  };

  using start_registry = std::map<uint64_t, start_entry>;

  /// Registry entry: the three phase-segment durations of one PUSCH, the assembly timestamp (for the staleness
  /// gate) and the same insertion sequence as above.
  struct phases_entry {
    std::chrono::time_point<std::chrono::high_resolution_clock> tp;
    int64_t  t2f_ns;
    int64_t  ce_ns;
    int64_t  eqdem_ns;
    uint64_t seq;
  };

  using phases_registry = std::map<uint64_t, phases_entry>;

  /// Maximum age of a pending registry entry to remain eligible for matching. Any legitimate start ->
  /// completion span (pipeline or decode) stays orders of magnitude below this (the slowest decoders observed
  /// are tens of milliseconds), while the slot-count pairing key wraps every 10.24 s for any numerology, so a
  /// wrap collision is at least that old. Without the gate, an entry that survived a quiet stretch (the
  /// registries are bounded by insertion count, not by time) would match a fresh completion with the same slot
  /// key and produce bogus latencies of one or more whole wrap cycles.
  static constexpr std::chrono::seconds max_entry_age{2};

  /// The span beyond which a completion is too late to be used: the configuration's uplink HARQ round trip,
  /// after which the transport block has been retransmitted anyway. Default 8 ms, which is what the n78/n1
  /// configurations this line runs measure (k1 + k2 + the retransmission timers); another configuration
  /// overrides it with OCUDU_UL_STALE_US rather than editing this.
  ///
  /// \note The slot DISTANCE is deliberately not reported next to it: find_fresh() only ever pairs the
  ///       completion slot with slot, slot-1 or slot-2, so the distance is bounded at two slots by
  ///       construction and a distribution of it would carry no information. What varies - and what made
  ///       the 71.6 ms sample - is the SPAN, which is what this splits on.
  /// \note Read on every call rather than cached in a static: the unit test has to be able to move it, and a
  ///       probe that reads it once would silently ignore the override for the rest of the process.
  static uint64_t stale_after_us()
  {
    const char* env = std::getenv("OCUDU_UL_STALE_US");
    return ((env != nullptr) && (std::strtoul(env, nullptr, 10) != 0)) ? std::strtoul(env, nullptr, 10) : 8000UL;
  }

  /// Finds the entry of \c registry for \c slot with the completion-time tolerance (slot, slot-1, slot-2),
  /// skipping entries older than max_entry_age (stale entries from previous slot-count wrap cycles).
  static start_registry::iterator
  find_fresh(start_registry&                                        registry,
             uint64_t                                               slot,
             const std::chrono::high_resolution_clock::time_point& now)
  {
    auto consider = [&](uint64_t s) {
      auto it = registry.find(s);
      if (it != registry.end() && now - it->second.tp > max_entry_age) {
        return registry.end();
      }
      return it;
    };
    auto it = consider(slot);
    if (it == registry.end() && slot > 0) {
      it = consider(slot - 1);
    }
    if (it == registry.end() && slot > 1) {
      it = consider(slot - 2);
    }
    return it;
  }

  /// Finds the entry of \c registry whose key equals \c slot exactly, skipping entries older than
  /// max_entry_age. Used for the series whose start and completion both carry the same slot reference (the FAPI
  /// pdu.slot), where the offset tolerance of find_fresh() would only enable same-cycle mis-pairings.
  static start_registry::iterator
  find_fresh_exact(start_registry&                                        registry,
                   uint64_t                                               slot,
                   const std::chrono::high_resolution_clock::time_point& now)
  {
    auto it = registry.find(slot);
    if (it != registry.end() && now - it->second.tp > max_entry_age) {
      return registry.end();
    }
    return it;
  }

  /// Keeps a registry bounded by evicting the entry with the lowest insertion sequence (FIFO by insertion,
  /// safe against the periodic slot-count wrap).
  template <typename Registry>
  static void evict_oldest(Registry& registry)
  {
    if (registry.size() <= 256) {
      return;
    }
    auto oldest = std::min_element(
        registry.begin(), registry.end(), [](const auto& lhs, const auto& rhs) { return lhs.second.seq < rhs.second.seq; });
    registry.erase(oldest);
  }

  std::mutex       mutex;
  start_registry   pending_starts;
  uint64_t         next_start_seq = 0;
  std::vector<double> latencies_us;
  /// Slot-keyed timestamps of the current TBs' LDPC decoder starts (see record_ldpc_start()).
  start_registry   pending_ldpc_starts;
  std::vector<double> ldpc_latencies_us;
  /// Sizes in bytes of the CRC-OK MAC PDUs (data bursts), recorded together with the LDPC latency samples.
  std::vector<double> mac_pdu_sizes_bytes;

  /// \brief The two spans a hop contributes, bucketed by the SIZE of the work that hop had to do.
  ///
  /// WHY THIS EXISTS, and why it is not the per-slot timeline. #13's tail question is "is a slow hop slow because
  /// of what IT had to do, or because of something shared (queueing, another process)?", and the two answers
  /// point at different fixes. The per-slot timeline cannot answer it: it is capped at 64 rows, and - measured
  /// 5.9.68 - it carried neither the lane's residency nor any size descriptor, `tf_from_done` being a second copy
  /// of `t2f`. These two series can, and over EVERY hop rather than 64 of them:
  ///
  ///  * `iq2llr` is the fused lane's own span (IQ arrival -> LLRs ready), the closest thing the host has to the
  ///    device-side `residency` the question names;
  ///  * `pipe` is the end-to-end span of the same hop.
  ///
  /// If the medians rise with the bucket, a slow hop is a hop with a lot to do, and the fix is in the lane's own
  /// work. If they are flat, the tail is NOT this hop's work, and the fix is elsewhere (device contention,
  /// another process, the machine). The SIZE is the transport block in bytes - the product of MCS and bandwidth
  /// a reader would ask for, and the quantity the probe already receives at the CRC-OK completion of the same
  /// slot. Population: CRC-OK hops only (a failed decode reports no transport block and would bucket a hop by a
  /// size it never carried).
  /// \brief Which size bucket a transport block of \p bytes falls in, and the bucket names the report prints.
  ///
  /// Boundaries chosen around what a 5 MHz cell actually shows (measured on the bridge config: median 157 B,
  /// p95 640 B, max ~1.1 kB), so each bucket collects enough hops to have a median worth reading.
  static constexpr size_t  nof_size_buckets = 4;
  static size_t            size_bucket_of(uint64_t bytes)
  {
    if (bytes < 128) {
      return 0;
    }
    if (bytes < 384) {
      return 1;
    }
    if (bytes < 768) {
      return 2;
    }
    return 3;
  }
  static const char* size_bucket_name(size_t i)
  {
    static constexpr const char* names[nof_size_buckets] = {"<128B", "128-383B", "384-767B", ">=768B"};
    return (i < nof_size_buckets) ? names[i] : "?";
  }

  std::array<std::vector<double>, nof_size_buckets> iq2llr_by_size_us;
  std::array<std::vector<double>, nof_size_buckets> pipe_by_size_us;
  /// Per-slot IQ -> LLR span, waiting for the CRC-OK completion that carries its slot's transport block size
  /// (see iq2llr_by_size_us). Bounded by insertion order like the other pending registries.
  std::map<uint64_t, double> pending_iq2llr_us;

  /// Slot-keyed timestamps of the whole-slot FFT completions (see record_t2f_end()).
  start_registry   pending_t2f_ends;
  /// Slot-keyed timestamps of the PUSCH channel estimation completions (see record_ce_end()).
  start_registry   pending_ce_ends;
  /// Slot-keyed timestamps of the CRC-OK completions (see record_end_crc_ok()); consumed by
  /// record_fapi_mac_end() to produce the FAPI->MAC tail-latency series.
  start_registry   pending_crc_ok_ends;
  /// Slot-keyed phase-segment durations assembled at the LDPC decode start (see record_ldpc_start()); erased
  /// when the matching CRC-OK completion records them into the summary series.
  phases_registry  pending_phases;
  /// Phase-segment latencies of the CRC-OK PUSCH completions (µs), in lockstep with [ul_ldpc_decode].
  std::vector<double> t2f_latencies_us;
  std::vector<double> ce_latencies_us;
  std::vector<double> eqdem_latencies_us;
  /// Fused-lane IQ -> LLR spans (µs): the arrival of the slot's IQ samples -> the LLRs are ready for the decoder.
  /// Recorded instead of the three segments above when the effective mode is phy_pipeline_mode::gpu, at every
  /// decode attempt (see record_ldpc_start()).
  std::vector<double> gpu_pipeline_latencies_us;
  /// Samples of the two pipeline series whose span exceeds stale_after_us(): a decode that completes later
  /// than the uplink HARQ round trip is a REAL measurement and a USELESS packet - the MAC has already
  /// retransmitted it - so it is counted apart instead of stretching `max` into a number that describes
  /// nothing. Measured on s41: [ul_pipeline] max 71.6 ms against a control's 13.7 ms, with p95 at 5.57 ms
  /// and the UL HARQ RTT at about 8 ms for this configuration (5.9.28/5.9.29).
  std::vector<double> stale_pipeline_us;
  std::vector<double> stale_gpu_pipeline_us;
  /// FAPI->MAC tail latencies of the CRC-OK completions (µs): CRC-OK -> MAC UL task enqueue.
  std::vector<double> fapi_mac_latencies_us;
  /// Receive wait times (µs), one per received BLOCK (see record_rx_wait): the span the host spent blocked inside
  /// receiver.receive(). The only series with no pairing: the two clock reads bracket a single call.
  std::vector<double> rx_wait_us;
  /// Host time blocked inside the front-end DFT's wait_slot() (µs), one sample per WAIT (see record_dft_wait).
  std::vector<double> dft_wait_us;
  /// The instant the samples completing each traced slot arrived (see record_slot_samples_complete()).
  std::map<uint64_t, std::chrono::high_resolution_clock::time_point> slot_samples_done;
  /// One entry per traced slot, keyed by slot: the per-slot timeline printed by print_slot_trace().
  std::map<uint64_t, slot_trace_entry> slot_trace;
  /// Receive waits reported before their slot had a trace entry (see record_rx_wait_for_slot): the receive
  /// happens before any landmark of the slot it completes, so the wait always arrives first.
  std::map<uint64_t, double> slot_trace_pre_wait;
  /// Traced slots in ARRIVAL order, so the bounded maps above can evict the oldest instead of refusing the
  /// newest (see record_slot_samples_complete): refusing means keeping the run's first milliseconds, which on an
  /// air leg is the attach phase, i.e. exactly the slots that carry no PUSCH.
  std::deque<uint64_t> slot_trace_order;
  /// Every landmark instant seen so far, keyed by (slot, which). A traced slot's entry is built from these, so
  /// the landmarks that arrive before the one that proves it carries a PUSCH are not lost.
  std::map<std::pair<uint64_t, slot_trace_what>, std::chrono::high_resolution_clock::time_point> slot_landmarks;
};

#else // not OCUDU_FLOW_PROBES: no-op implementation with zero overhead.

class ul_pipeline_probe
{
public:
  static ul_pipeline_probe& get()
  {
    // Leaked for the same reason as the real probe above (symmetry: this variant has no state to lose, but the
    // rule must not depend on which arm is compiled).
    static ul_pipeline_probe* instance = new ul_pipeline_probe();
    return *instance;
  }
  void record_start(uint64_t /*slot*/) {}
  void record_ldpc_start(uint64_t /*slot*/) {}
  void record_t2f_end(uint64_t /*slot*/) {}
  void record_ce_end(uint64_t /*slot*/) {}
  void record_end_crc_ok(uint64_t /*slot*/, size_t /*mac_pdu_bytes*/) {}
  void record_fapi_mac_end(uint64_t /*slot*/) {}
  void record_rx_wait(int64_t /*wait_ns*/) {}
  void record_dft_wait(int64_t /*wait_ns*/) {}
  std::optional<ul_phase_durations> get_phase_durations(uint64_t /*slot*/) { return std::nullopt; }
  /// P0-5's pairing hook, compiled out with the rest of the probe: there are no phase samples to announce, so a
  /// caller that registers an observer is told nothing - which is also what the lane probe's report says (it
  /// counts the samples it was handed, and reports zero rather than inventing a pairing).
  using phase_sample_observer_t = void (*)(uint64_t slot, int64_t t2f_ns, int64_t ce_ns, int64_t eqdem_ns);
  static void set_phase_sample_observer(phase_sample_observer_t /*observer*/) {}
  /// No phase samples were ever recorded (the probe is compiled out), so the account reads zero - which is
  /// also what the lane probe reports for a leg whose segments were off.
  size_t phase_samples_recorded() { return 0; }
  void report() {}

private:
  ul_pipeline_probe() = default;
};

#endif

} // namespace ocudu
