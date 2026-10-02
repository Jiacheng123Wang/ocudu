// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_mode.h"
#include "ocudu/support/scheduling/thread_sched_snapshot.h" // this_thread_sched_snapshot (P1: per-thread CPU)
#include "ocudu/support/executors/unique_thread.h"          // this_thread_name()
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <cstring>
#include <vector>
#if !defined(_WIN32)
#include <sys/resource.h> // getrusage: the only reading that says whether THIS PROCESS got the CPU
#endif

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
/// - record_start() is called by the lower PHY baseband processor when the samples of a slot START ARRIVING, i.e.
///   BEFORE receiver.receive() returns (lower_phy_baseband_processor::ul_process). It used to be recorded after the
///   call returned, which excluded the wait for the samples - and with whole-slot blocks, whose hand-over happens at
///   the slot's END, that made this series read "the transforms" rather than "the transforms plus a slot of waiting".
///   The move was deliberate (it is what makes the series comparable across receive policies, S-7g-13), and the price
///   is that the NAME no longer describes the CONTENT: [ul_time_frequency] = [ul_rx_wait] + the front end's own work
///   (see the [ul_rx_wait] note below). Measured on an all-CPU n1 leg, 2026-09-26: 1111.7us median against
///   [ul_rx_wait] 1054.0us, i.e. 57.7us of transforms - the "tens of microseconds" the pre-move series used to read.
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
/// TWO THINGS THE [ul_rx_wait] DISTRIBUTION DELIBERATELY EXCLUDES OR SPLITS OFF (dev doc 6.146/6.147), because a
/// series has to describe the population it names:
///
///  * THE START-UP CALL IS REPORTED APART. The very first receive() of a run spans the radio's stream start: the RU
///    controller starts that stream 100 ms in the future by design (ru_controller_sdr_impl, delay_s = 0.1), while
///    the receive thread asks for samples immediately - so the first call blocks for ~101 ms in EVERY run, whatever
///    the mode or load, and it is one sample, not a link behaviour. It is therefore NOT pushed into the
///    distribution (where it used to become `max`, the first number a reader looks at - and where it cost three
///    separate write-ups and one wrong attribution before it was explained): it is kept in rx_wait_startup_us and
///    printed as its own field, so the exclusion is visible rather than silent. The caller marks it, because the
///    caller is what knows the call had no predecessor (the same rule [ul_rx_timing] applies to loop/slip).
///  * THE HOP-SCOPED COMPANION IS A SEPARATE SERIES. [ul_rx_wait] counts BLOCKS, while the pipeline series count
///    HOPS, so the two populations differ (an idle slot contributes a wait sample and no hop). [ul_rx_wait_hop]
///    carries the wait of the block that COMPLETED the slot, but only for the slots a hop was recorded on - i.e.
///    the same population as [ul_gpu_pipeline]/[ul_pipeline] - which is what makes the decomposition
///    "wait + (everything after the samples)" add up inside ONE population. The per-block series is NOT narrowed
///    to that population on purpose: a transport hiccup that lands on an idle slot leaves a trace there and nowhere
///    else (see 4.1.1's blind spots).
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
    // The baseline for the LDPC window, which starts at this instant (dev doc 10.15 (3)).
    pending_ldpc_starts[slot] = {now, next_start_seq++, phase_baseline_for(to_ns(now))};
    // Bound the registry by insertion order (see record_start): unmatched entries belong to TBs that ended
    // without a CRC-OK completion (or with one in a shifted slot).
    evict_oldest(pending_ldpc_starts);

    // The wait this hop's own samples took, for the hops that reach this landmark (see record_slot_rx_wait):
    // consumed here rather than reported per block, so [ul_rx_wait_hop] has the same population as the hop
    // series it decomposes. Recorded before the mode split on purpose - it is about the RECEIVE side, which is
    // the same code in every mode. One entry per recorded hop: a second decode attempt for the same slot (a
    // retransmission) finds nothing and contributes nothing, which is the same rule [ul_gpu_pipeline] follows.
    auto rx_wait_it = slot_rx_wait_us.find(slot);
    if (rx_wait_it != slot_rx_wait_us.end()) {
      rx_wait_hop_us.push_back(rx_wait_it->second);
      slot_rx_wait_us.erase(rx_wait_it);
    }

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
        // ★ The three instants are captured ONLY when the worst-K list is on, and the guard is not cosmetic:
        // to_ns() rebases through two clock reads of its own, so filling them unconditionally cost six clock
        // reads per PUSCH hop even with the knob unset - the same "unset = no clock, no cost" rule the check
        // inside record_phase_timing_event_locked() enforces (dev doc 10.13). Nothing else reads these fields
        // (get_phase_durations() returns the three DURATIONS), so 0 here means exactly "not captured".
        //
        // They are what lets a phase event quote a window instead of only a duration, and by the time the
        // CRC-OK completion records the segments the landmarks are gone from the pending maps.
        const bool    want_phase_events = timing_events_limit() != 0;
        const int64_t t2f_begin_ns      = want_phase_events ? to_ns(start_it->second.tp) : 0;
        const int64_t t2f_end_ns        = want_phase_events ? to_ns(t2f_it->second.tp) : 0;
        const int64_t ce_end_ns         = want_phase_events ? to_ns(ce_it->second.tp) : 0;
        const int64_t now_ns            = want_phase_events ? to_ns(now) : 0;
        pending_phases[slot] = {now, t2f_ns, ce_ns, eqdem_ns, t2f_begin_ns, t2f_end_ns, ce_end_ns, next_start_seq++};
        evict_oldest(pending_phases);
        // ... and the three segments that END here are recorded as tail events immediately: their durations are
        // final at this instant (t2f ends at the FFT completion, ce at the channel-estimation completion, and
        // eqdem at this call - the first codeblock decode invocation). TWO of them therefore end on THIS thread
        // and one on another; the event says which thread completed it, which is what a reader needs in order to
        // know whose stack to look at.
        //
        // The lock is already held (see record_phase_timing_event_locked): taking it again would deadlock.
        // `ce` starts at the t2f end and `eqdem` at the ce end, so each carries the snapshot its own start
        // landmark took (see start_entry::base). `t2f` deliberately passes none: its start is record_start, on
        // the receive path's per-BLOCK hot path, where a snapshot per block is not worth its cost for a series
        // whose floor (2 ms) the legs barely reach - it keeps the shared baseline and prints `cpu=-`.
        record_phase_timing_event_locked(timing_event_kind::phase_t2f, t2f_ns / 1000, t2f_begin_ns, t2f_end_ns);
        record_phase_timing_event_locked(timing_event_kind::phase_ce,
                                         ce_ns / 1000,
                                         window_start_ns(t2f_it->second.base, t2f_it->second.tp),
                                         ce_end_ns,
                                         &t2f_it->second.base);
        record_phase_timing_event_locked(timing_event_kind::phase_eqdem,
                                         eqdem_ns / 1000,
                                         window_start_ns(ce_it->second.base, ce_it->second.tp),
                                         now_ns,
                                         &ce_it->second.base);
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
    // ... and the baseline for the CE window, which starts at this instant.
    pending_t2f_ends[slot] = {now, next_start_seq++, phase_baseline_for(to_ns(now))};
    evict_oldest(pending_t2f_ends);
    trace_slot(slot, slot_trace_what::t2f, now);
  }

  /// \brief Files one per-slot CPU window for the CALLING thread (see thread_cpu_accounting).
  ///
  /// Call it from a landmark that runs on the pool thread and carries a slot index. Several calls within the same
  /// slot are harmless: a window is opened once per slot CHANGE, and a later call that sees the same slot leaves
  /// the open window where it is.
  void record_thread_cpu_boundary(uint64_t slot)
  {
#if defined(OCUDU_FLOW_PROBES)
    if (!thread_cpu_accounting_enabled()) {
      return; // one getenv and out: with the knob off there is no clock read and no state, on either platform
    }
    thread_cpu_accounting& acc    = this_thread_cpu_accounting();
    const int64_t          cpu_ns = this_thread_cpu_ns();
    if (cpu_ns < 0) {
      return; // the platform refused the reading: it must not be filed as a zero-length window
    }
    file_thread_cpu_boundary(acc, slot, cpu_ns);
#else
    (void)slot;
#endif
  }

  /// Test hook: file a window with a GIVEN CPU value instead of a clock reading. The parts that can be wrong are
  /// the counting, the buckets and the quantile, and all three are testable without a clock.
  ///
  /// \note It goes through the SAME file_thread_cpu_boundary() the production path uses. The first version of this
  ///       hook duplicated the slot test, and the reverse arm for it ("file on every call instead of on every slot
  ///       change") did NOT go red - the test was exercising a copy of the logic. One implementation, or the test
  ///       is decoration.
  void record_thread_cpu_boundary_for_test(uint64_t slot, int64_t cpu_ns)
  {
    file_thread_cpu_boundary(this_thread_cpu_accounting(), slot, cpu_ns);
  }

  /// Test hook: zero the calling thread's accounting and make it the only registered one, so a case cannot read
  /// another case's numbers (the probe is a process-wide singleton and the test binary runs many cases in one
  /// process - the same trap the window-stability test hit on 2026-10-01).
  void reset_thread_cpu_accounting_for_test()
  {
    thread_cpu_accounting& own = this_thread_cpu_accounting();
    const uint64_t         tid = own.thread_id;
    char                   nm[sizeof(own.name)] = {};
    std::snprintf(nm, sizeof(nm), "%s", own.name);
    own = thread_cpu_accounting{};
    std::snprintf(own.name, sizeof(own.name), "%s", nm);
    own.thread_id = tid;
    std::lock_guard<std::mutex> lock(mutex);
    thread_cpu_accounts.clear();
    thread_cpu_accounts.push_back(&own);
  }

  size_t thread_cpu_accounts_size_for_test()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return thread_cpu_accounts.size();
  }

  /// Test hook: {slots, sum_ns, max_ns, p99.9 from the histogram} of one registered account. It returns VALUES
  /// rather than the block itself because the block type is private: a test that could name it would also be able
  /// to depend on its layout, and the arithmetic under test is exactly these four numbers.
  std::array<int64_t, 4> thread_cpu_account_values_for_test(size_t index)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const thread_cpu_accounting& acc = *thread_cpu_accounts.at(index);
    return {static_cast<int64_t>(acc.slots), acc.sum_ns, acc.max_ns, thread_cpu_quantile_ns(acc, 0.999)};
  }

  /// \brief Prints one line per thread that filed windows, with the number to declare (see thread_cpu_accounting).
  ///
  /// It prints NOTHING when the knob is off, so a delivery leg's report stays byte-identical, and it is called
  /// from report() next to the series it belongs with.
  void print_thread_cpu_accounting()
  {
    if (!thread_cpu_accounting_enabled()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (thread_cpu_accounts.empty()) {
      return;
    }
    std::fprintf(stderr,
                 "[ul_thread_cpu] OCUDU_UL_THREAD_CPU=1: CPU each thread burned between two consecutive slot "
                 "changes IT saw - the figure a Mach time constraint's `computation` has to cover\n");
    for (const thread_cpu_accounting* acc : thread_cpu_accounts) {
      if (acc->slots == 0) {
        std::fprintf(stderr,
                     "[ul_thread_cpu]   thread=%-16s id=%llu no closed window (fewer than two slot changes seen)\n",
                     acc->name,
                     static_cast<unsigned long long>(acc->thread_id));
        continue;
      }
      std::fprintf(stderr,
                   "[ul_thread_cpu]   thread=%-16s id=%llu slots=%llu mean=%.1fus p99.9<=%.1fus max=%.1fus "
                   "-> declare computation >= %.1fus\n",
                   acc->name,
                   static_cast<unsigned long long>(acc->thread_id),
                   static_cast<unsigned long long>(acc->slots),
                   static_cast<double>(acc->sum_ns) / static_cast<double>(acc->slots) / 1000.0,
                   static_cast<double>(thread_cpu_quantile_ns(*acc, 0.999)) / 1000.0,
                   static_cast<double>(acc->max_ns) / 1000.0,
                   static_cast<double>(acc->max_ns) / 1000.0);
    }
  }

  /// Records the completion of the PUSCH channel estimation: the channel estimates of all the data symbols of
  /// the slot are ready (call from the PUSCH processor at the start of the data processing).
  /// \param[in] slot Slot number of the PUSCH (same reference as record_end_crc_ok).
  void record_ce_end(uint64_t slot)
  {
    // The per-slot CPU window of THIS thread is closed here, before the phase machinery below can return early:
    // it is a different question from the phase segments (dev doc 10.30(8)) and must not inherit their gates.
    record_thread_cpu_boundary(slot);
    if (!records_phase_segments()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto now        = std::chrono::high_resolution_clock::now();
    // ... and the baseline for the EQUALIZATION+DEMOD window, which starts at this instant.
    pending_ce_ends[slot] = {now, next_start_seq++, phase_baseline_for(to_ns(now))};
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
      // The decode's own tail event (P1): both ends of this segment are recorded on a POOL thread (the codeblock
      // task starts the decoder, the notifier completes it), so this is the one series whose window a single
      // thread owns end to end - and therefore the one whose `tcpu=` is a real per-thread reading whenever the
      // baseline happens to have been taken by that same thread.
      //
      // The gate is repeated HERE because its ARGUMENTS are expensive: to_ns() costs two clock reads each, and C++
      // evaluates them before the call can refuse (the body's own check then never sees the cost). The other
      // phase call site guards its instants with the same reasoning.
      if (timing_event_wanted_phase(timing_event_kind::phase_ldpc, ldpc_us.count())) {
        record_phase_timing_event_locked(timing_event_kind::phase_ldpc,
                                         ldpc_us.count(),
                                         window_start_ns(ldpc_it->second.base, ldpc_it->second.tp),
                                         to_ns(now),
                                         &ldpc_it->second.base);
      }
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
  /// \param[in] spans_stream_start True for the ONE call that spans the radio's stream start, i.e. the first
  ///                 receive() of the run - the caller knows it because that call has no predecessor (the rule
  ///                 [ul_rx_timing] applies to loop/slip). Such a call is reported apart (rx_wait_startup_us) and
  ///                 never enters the distribution or a hop: it measures the radio's start-up offset
  ///                 (ru_controller_sdr_impl starts the stream 100 ms ahead by design), not the link.
  ///
  /// \note Counted per BLOCK, not per slot: the block size is the receive policy's (see ul_process), so under the
  ///       whole-slot policy this series has one sample per slot and under the symbol-grained one it has one per
  ///       block. Compare its counts against the policy in force, not against [ul_pipeline]'s. For the hop-scoped
  ///       companion see record_slot_rx_wait().
  // ---- the worst timing events, with host instants (OCUDU_UL_TIMING_EVENTS, dev doc 6.240/6.241) ------------
  /// Which series an event belongs to. The first two existed from the beginning (6.240/6.241) and were told
  /// apart by the list they lived in; P1 added the four phase segments, whose tails have the same shape as the
  /// receive wait's (measured over the 7 newest n78/gpu legs: `ul_channel_estimation` p99 163..166 us against a
  /// max of 1228..4792 us) but which had NO event record at all before - only an aggregate.
  enum class timing_event_kind : unsigned {
    rx_wait    = 0, ///< [ul_rx_wait]: how long the receive blocked (floor 1 ms).
    dl_handover = 1, ///< [dl_tx_slack]: a hand-over's margin (floor 500 us, ranked downwards).
    phase_t2f  = 2, ///< [ul_time_frequency]
    phase_ce   = 3, ///< [ul_channel_estimation]
    phase_eqdem = 4, ///< [ul_equalization_demod]
    phase_ldpc = 5, ///< [ul_ldpc_decode]
    count      = 6
  };

  /// One kept event: the quantity it is ranked by, its own window on the steady clock, the WALL clock at the
  /// instant it was kept, the load average then, and (for a receive) the block's air time.
  ///
  /// Both clocks are kept on purpose. The steady instants order the events inside this process; the wall clock is
  /// what lines them up against the `.log`'s `[RF] Real-time failure in RF: ...` lines, which carry only wall
  /// timestamps and are written by a different thread in a different logger. Printing the epoch milliseconds as
  /// well as the ISO string means the alignment survives any timezone or format difference between the two files.
  struct timing_event {
    int64_t  value_us   = 0;  ///< the ranked quantity: the receive wait, or a hand-over's margin (negative = late)
    /// WHICH SERIES this event belongs to. Until P1 the list only had two kinds and the kind was implicit in the
    /// list it lived in; the phase segments (t2f/ce/eqdem/ldpc) are four more, and a reader must be able to tell
    /// "the receive blocked 12 ms" from "the channel-estimation segment took 12 ms" without knowing which block
    /// of the report it came from.
    timing_event_kind kind = timing_event_kind::rx_wait;
    int64_t  begin_ns   = 0;  ///< steady-clock instant the measured window began (0 when the caller had none)
    int64_t  end_ns     = 0;  ///< ... and ended
    int64_t  wall_ms    = 0;  ///< system_clock at the moment the event was KEPT (see record_timing_event_rx)
    int64_t  air_us     = 0;  ///< the block's air time, receive events only (0 = not reported)
    int64_t  due_ts     = 0;  ///< the radio's due timestamp, hand-over events only
    int64_t  load1_x100 = -1; ///< getloadavg(1) at that instant, x100 (-1 = the caller had none)
    /// WHICH THREAD recorded the event (dev doc P1, "可归因"). The name is a bounded COPY, not a pointer: the
    /// probe keeps events for the whole run and hands them out at report time, so a pointer into the recording
    /// thread's storage would dangle (or, worse, name the reporting thread).
    uint64_t thread_id       = 0;
    char     thread_name[16] = {};
    /// THIS THREAD'S OWN CPU over the event's window (P1). It is the reading the process-wide `cpu=` below
    /// cannot give: `cpu` says "the process got the CPU", this says "THIS thread got it". A thread that was
    /// descheduled inside a 12 ms wait shows ~0 here while the process shows 12 ms - which is the difference
    /// between a scheduling problem and a work/IO problem, and the whole reason this workstream exists.
    ///
    /// -1 means NO READING, never zero, and it has two causes worth telling apart in the report's footnote:
    ///   * the platform does not expose per-thread CPU (none today: both do), or
    ///   * the baseline was taken by ANOTHER thread. A cumulative counter can only be subtracted from a reading
    ///     of the SAME thread, and the receive path is the only caller that maintains a baseline, so a
    ///     hand-over event (recorded on the transmit thread) always prints `tcpu=-`. That is deliberate: the
    ///     window's process-wide reading is still valid there, and inventing a thread delta across two threads
    ///     would be exactly the kind of plausible-looking wrong number this project keeps writing post-mortems
    ///     about (dev doc phy_latency 6.245).
    int64_t tcpu_ns = -1;
    /// THE PROCESS'S OWN CPU TIME AND INVOLUNTARY SWITCHES over the event's window (dev doc 6.243), measured
    /// against a baseline the caller takes just before the window (see timing_event_snapshot_wanted). This is what
    /// separates the two owners a long wait can have, and `load1` CANNOT do it: a 60 s average is blind to a
    /// 12 ms stall (measured on p178: 3.61 and 4.03 on 14 cores, neither supporting nor refuting contention).
    ///   * cpu_ns ~= the window  => the process kept its cores; the samples were late on the RADIO/USB side;
    ///   * cpu_ns ~= 0 (and/or ivcsw > 0) => the process was NOT scheduled; that is host scheduling.
    int64_t  cpu_ns     = -1; ///< (utime + stime) consumed by the whole process during the window (-1 = no baseline)
    int64_t  ivcsw      = -1; ///< involuntary context switches of the process during the window (-1 = no baseline)
    int64_t  nvcsw      = -1; ///< voluntary ones, for the same window (a blocked thread switches voluntarily)
    int64_t  base_age_us = -1;///< how long BEFORE the window began the baseline was taken (the reading's slack)
    /// The WIDTH of the window the CPU delta covers. For a receive event that is its wait; for a hand-over it is
    /// the distance back to the baseline the receive path last took (~1 ms in steady state, the whole stall when
    /// the receive path is stalled). It is printed because `cpu` without it cannot be read: measured on
    /// `p180-n78-stress`, the transmit lines printed `cpu=0.00ms base_age=0us` - and `base_age` is 0 for a
    /// hand-over BY CONSTRUCTION (its window IS the baseline), so the pair said nothing at all (dev doc 6.245).
    int64_t  win_us     = -1;
  };

  /// The baseline for the deltas above: process-wide CPU time and switch counts, taken by the caller just before
  /// the measured call, at most once per timing_event_cpu_period_ns() so the instrument stays cheap.
  struct cpu_snapshot {
    int64_t  ns            = 0;
    int64_t  cpu_ns        = 0;
    int64_t  nvcsw         = 0;
    int64_t  ivcsw         = 0;
    /// THE SAME READING FOR THE THREAD THAT TOOK IT (P1). Both halves are needed and they answer different
    /// questions: cpu_ns is RUSAGE_SELF (every thread), thread_cpu_ns is the calling thread alone. Keeping the
    /// thread id beside the value is what makes the subtraction legal - see timing_event::tcpu_ns.
    uint64_t thread_id     = 0;
    int64_t  thread_cpu_ns = -1;
    bool     valid         = false;
  };
  cpu_snapshot cpu_base{};
  /// The FIRST baseline of the run, which is what turns the per-event deltas into a leg-wide RATE: the switch
  /// counts in between are meaningless per event without knowing the window, and meaningless across events
  /// without knowing this leg's own base rate. Taken once (the first snapshot of the leg) and never refreshed.
  cpu_snapshot leg_base{};
  bool         leg_base_valid = false;
  /// 1 ms: the baseline is then at most 1 ms older than the window it is subtracted from, i.e. <=8% of a 12 ms
  /// stall, while the cost is bounded at <=1000 getrusage calls/s on the receive thread (it runs at ~28k
  /// receive calls/s and spends ~96% of that time blocked, so this is a few percent of its own CPU).
  static constexpr int64_t timing_event_cpu_period_ns = 1000000;
  /// Bounded so a leg's report cannot grow without limit: 64 events is already far more than the ~0-20 a leg has.
  static constexpr unsigned max_timing_events     = 64;
  static constexpr unsigned default_timing_events = 8;
  /// The floors that make the instrument free when nothing is wrong: a receive wait below 1 ms and a hand-over
  /// margin at or above 500 us are not kept at all (the report already counts them in its buckets).
  static constexpr int64_t timing_event_rx_floor_ns = 1000000;
  static constexpr int64_t timing_event_tx_floor_us = 500;
  /// ... and the same idea for the four PHASE segments, whose floors are read off the same distribution the
  /// thresholds come from (P0's census over the 7 newest n78/gpu legs): each floor sits ~6x above that series'
  /// own p99, so a healthy leg keeps nothing and a spike is kept with plenty of margin to spare.
  ///   series        p99 (per leg)      max (per leg)        floor
  ///   t2f           619.9 .. 622.5 us  686 .. 21235 us      2 ms
  ///   ce            162.9 .. 166.1 us  966 .. 13267 us      1 ms
  ///   eqdem         898.3 .. 913.6 us  3157 .. 20977 us     3 ms
  ///   ldpc          115 .. 148 us      441 .. 961 us        500 us
  static constexpr int64_t timing_event_phase_floor_us[4] = {2000, 1000, 3000, 500};
  std::vector<timing_event> worst_rx_events{};
  std::vector<timing_event> worst_tx_events{};
  /// One worst-K list per phase series, indexed by timing_event_kind minus phase_t2f (see the floors above).
  std::array<std::vector<timing_event>, 4> worst_phase_events{};
  /// Candidates SEEN above the floor, kept or not (the list is bounded, this is not): it is what separates "the
  /// instrument was on and nothing was slow" from "the instrument was never called", which otherwise look alike.
  uint64_t rx_event_candidates{0};
  uint64_t tx_event_candidates{0};
  std::array<uint64_t, 4> phase_event_candidates{};
  /// Process+thread snapshots taken by the PER-HOP path (the phase landmarks and the phase events). It is the
  /// observable half of the "knob unset => no clock, no cost" rule for that path: the candidate counters only
  /// prove the RECORDER did not run, and this one proves the SNAPSHOT was not taken either (dev doc 10.13/10.15).
  uint64_t phase_baselines_taken{0};
  /// Events whose baseline was taken INSIDE their own window and was therefore refused (`cpu=-`). It is counted and
  /// printed because the alternative is what happened on `p179-n78-stress`: the receive path stamped the baseline
  /// AFTER the window's start, every event printed `-`, and the leg's question stayed unanswered with nothing on
  /// the report to say why (dev doc 6.244).
  ///
  /// \note Phase events are NOT counted here even when their window starts before the baseline: the baseline
  ///       belongs to the receive path (~1 ms cadence) and a phase segment legitimately begins inside that
  ///       cadence, so a refusal there is arithmetic, not a caller's ordering mistake. Counting both in one
  ///       number would turn a routine `cpu=-` into a warning that says "fix the call site".
  uint64_t late_baselines{0};
  /// Phase events refused a process-wide delta because the maintained baseline did not precede their window.
  /// Printed apart from late_baselines for the reason above.
  uint64_t phase_baseline_misses{0};

  void record_rx_wait(int64_t wait_ns,
                      bool    spans_stream_start = false,
                      int64_t begin_ns           = 0,
                      int64_t end_ns             = 0,
                      int64_t air_us             = 0,
                      int64_t load1_x100         = -1)
  {
    if (wait_ns < 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (spans_stream_start) {
      // Reported, never distributed: it is ONE sample per run and it belongs to the radio's start-up, not to the
      // uplink. Kept (not dropped) so the report can account for the sample the distribution does not have.
      rx_wait_startup_us = static_cast<double>(wait_ns) / 1e3;
      return;
    }
    rx_wait_us.push_back(static_cast<double>(wait_ns) / 1e3);

    // The worst-K list, on the SAME sample this call just pushed: the ranking never removes anything from the
    // distribution (the two answer different questions - "what is the tail" and "when did it happen").
    if (timing_event_wanted_rx(wait_ns)) {
      ++rx_event_candidates;
      timing_event ev;
      ev.value_us   = wait_ns / 1000;
      ev.begin_ns   = begin_ns;
      ev.end_ns     = end_ns;
      ev.air_us     = air_us;
      ev.load1_x100 = load1_x100;
      attach_cpu_delta(ev, begin_ns, end_ns);
      take_rx_timing_event(ev);
    }

    // The SAME-LEG TEST (dev doc 6.150 (6)): does this receive's own window overlap one of the submission's
    // windows, per kind? Three rings of 16 walked in full every receive - 48 compares at ~2 kHz, nothing - and the
    // report prints every rate next to the duty cycle of the window it was measured against, because that duty
    // cycle is exactly what the coincidence reading predicts.
    if ((begin_ns != 0) && (end_ns >= begin_ns) && (dft_block_count[0] != 0)) {
      ++rx_overlap_seen;
      const bool slow = (wait_ns >= rx_slow_ns);
      if (slow) {
        ++rx_slow_seen;
      }
      for (size_t k = 0; k != static_cast<size_t>(dft_window_kind::count); ++k) {
        bool hit = false;
        for (size_t i = 0; (i != max_dft_block_windows) && !hit; ++i) {
          const dft_block_window& w = dft_block_windows[k][i];
          hit                       = (w.end_ns > w.begin_ns) && (begin_ns < w.end_ns) && (w.begin_ns < end_ns);
        }
        if (hit) {
          ++rx_overlap_total[k];
          if (slow) {
            ++rx_overlap_slow[k];
          }
        }
      }
    }
    if ((begin_ns != 0) && (end_ns >= begin_ns)) {
      if (leg_first_ns == 0) {
        leg_first_ns = begin_ns;
      }
      leg_last_ns = end_ns;
      leg_span_ns = leg_last_ns - leg_first_ns;
    }
  }

  /// \brief Records a hand-over whose margin is at or below the floor, for the worst-K list beside the receive waits.
  ///
  /// Called from the DL accounting (tx_slack_note_transmit) for the events the report already counts as late or
  /// nearly late: this adds only WHEN, not what - the aggregates stay the criterion, and this is the alignment
  /// key against the receive stalls and the `[RF]` lines (dev doc 6.240/6.241).
  ///
  /// \param[in] margin_us  due_ts/rate - host_now, the same quantity [dl_tx_slack] is built from (negative = late).
  /// \param[in] due_ts     The radio timestamp the hand-over was due on (kept so a leg can be tied to the map).
  /// \param[in] end_ns     Host steady instant of the hand-over when the caller has it, else 0.
  /// \param[in] load1_x100 getloadavg(1) x100 at that instant, else -1.
  void record_tx_timing_event(int64_t margin_us, uint64_t due_ts, int64_t end_ns = 0, int64_t load1_x100 = -1)
  {
    if (!timing_event_wanted_tx(margin_us)) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    ++tx_event_candidates;
    timing_event ev;
    ev.value_us   = margin_us;
    ev.end_ns     = end_ns;
    ev.due_ts     = static_cast<int64_t>(due_ts);
    ev.load1_x100 = load1_x100;
    attach_cpu_delta(ev, /*begin_ns=*/cpu_base.ns, end_ns);
    take_tx_timing_event(ev);
  }

  /// \brief Fills \p ev's CPU fields from the baselines. Caller holds the lock.
  ///
  /// The window is [\p begin_ns, \p end_ns] and the baseline must PRECEDE it; when it does not (or was never
  /// taken) the fields stay -1 and the report prints `cpu=-`, because "no reading" must not look like "zero CPU".
  ///
  /// \param[in] count_late Whether a baseline that falls INSIDE the window is a caller's ordering mistake (true
  ///            for the receive path, which owns the baseline; false for a phase event, whose window legitimately
  ///            starts inside the receive path's ~1 ms cadence - see phase_baseline_misses).
  void attach_cpu_delta(timing_event&       ev,
                        int64_t             begin_ns,
                        int64_t             end_ns,
                        bool                count_late = true,
                        const cpu_snapshot* explicit_base = nullptr)
  {
    // An event may carry its own baseline (the phase landmarks; see start_entry::base). The rules below are the
    // SAME for both sources - in particular "a baseline that does not precede the window is refused, and the
    // refusal is counted" - so a phase event can never print a delta that covers more than its own window.
    const cpu_snapshot& base = (explicit_base != nullptr) ? *explicit_base : cpu_base;
    // The identity of the recording thread is filled FIRST and unconditionally: it costs two calls that are
    // already made once per event above the ranking bar, and an event whose thread is unknown cannot be
    // attributed at all - which is the only thing this line is for.
    const thread_sched_snapshot self = this_thread_sched_snapshot();
    ev.thread_id                     = self.thread_id;
    std::snprintf(ev.thread_name, sizeof(ev.thread_name), "%s", this_thread_name());
    // The width is known even when there is no baseline, so it is set first and always printed.
    ev.win_us = (end_ns >= begin_ns) ? ((end_ns - begin_ns) / 1000) : -1;
    if (!base.valid || (base.ns > begin_ns)) {
      if (base.valid) {
        if (count_late) {
          ++late_baselines; // stamped inside the window: the caller's ordering is wrong, and the report says so
        } else {
          ++phase_baseline_misses;
        }
      }
      return;
    }
    // THIS thread's own CPU over the window, and only when the baseline was taken by this same thread: two
    // cumulative counters can be subtracted only when they describe the same thread (see timing_event::tcpu_ns).
    if (self.valid() && (base.thread_cpu_ns >= 0) && (base.thread_id == self.thread_id)) {
      ev.tcpu_ns = self.cpu_ns - base.thread_cpu_ns;
    }
#if !defined(_WIN32)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
      return;
    }
    const int64_t cpu_ns =
        (static_cast<int64_t>(ru.ru_utime.tv_sec) + static_cast<int64_t>(ru.ru_stime.tv_sec)) * 1000000000LL +
        (static_cast<int64_t>(ru.ru_utime.tv_usec) + static_cast<int64_t>(ru.ru_stime.tv_usec)) * 1000LL;
    ev.cpu_ns      = cpu_ns - base.cpu_ns;
    ev.ivcsw       = static_cast<int64_t>(ru.ru_nivcsw) - base.ivcsw;
    ev.nvcsw       = static_cast<int64_t>(ru.ru_nvcsw) - base.nvcsw;
    ev.base_age_us = (begin_ns - base.ns) / 1000;
#else
    (void)begin_ns;
    (void)end_ns;
#endif
  }

  /// \brief Keeps \p ev if it is among the worst `timing_events_limit()` receive waits. Caller holds the lock.
  ///
  /// The list is kept sorted DESCENDING, so `back()` is the current admission bar: an event that cannot beat it
  /// costs one comparison, and the wall-clock read below therefore happens only for events that are actually kept,
  /// which is what makes "the instrument is on and nothing is wrong" as cheap as "off".
  void take_rx_timing_event(timing_event& ev)
  {
    const size_t limit = timing_events_limit();
    if (limit == 0) {
      return;
    }
    if (worst_rx_events.size() >= limit) {
      if (ev.value_us <= worst_rx_events.back().value_us) {
        return;
      }
      worst_rx_events.pop_back();
    }
    stamp_wall_clock(ev);
    worst_rx_events.push_back(ev);
    std::sort(worst_rx_events.begin(), worst_rx_events.end(),
              [](const timing_event& lhs, const timing_event& rhs) { return lhs.value_us > rhs.value_us; });
  }

  /// \brief The same for hand-over margins, where the quantity goes wrong DOWNWARDS: the worst is the MOST
  /// NEGATIVE margin, so this list is kept sorted ASCENDING and `back()` is again the admission bar.
  void take_tx_timing_event(timing_event& ev)
  {
    const size_t limit = timing_events_limit();
    if (limit == 0) {
      return;
    }
    if (worst_tx_events.size() >= limit) {
      if (ev.value_us >= worst_tx_events.back().value_us) {
        return;
      }
      worst_tx_events.pop_back();
    }
    stamp_wall_clock(ev);
    worst_tx_events.push_back(ev);
    std::sort(worst_tx_events.begin(), worst_tx_events.end(),
              [](const timing_event& lhs, const timing_event& rhs) { return lhs.value_us < rhs.value_us; });
  }

  /// The WALL clock is read HERE, i.e. only for an event that enters a list, and at the instant the event is
  /// recorded - the same instant a concurrent `[RF]` line in the `.log` would carry. Nothing else reads it, so the
  /// instrument costs one system_clock call per kept event and zero when the knob is unset.
  static void stamp_wall_clock(timing_event& ev)
  {
    ev.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
  }

  /// Index of a phase series in the per-series arrays (0..3), or 4 when \p kind is not a phase series.
  static constexpr size_t timing_event_phase_floor_us_size = 4;
  static constexpr size_t phase_index(timing_event_kind kind)
  {
    return static_cast<size_t>(kind) - static_cast<size_t>(timing_event_kind::phase_t2f);
  }

  /// Whether a phase segment is worth keeping: the floor is what keeps a healthy leg's list empty and its cost
  /// zero (see timing_event_phase_floor_us).
  static bool timing_event_wanted_phase(timing_event_kind kind, int64_t value_us)
  {
    if (timing_events_limit() == 0) {
      return false;
    }
    const size_t idx = phase_index(kind);
    return (idx < timing_event_phase_floor_us_size) && (value_us >= timing_event_phase_floor_us[idx]);
  }

  /// \brief Records one PHASE-SEGMENT tail event (P1): [ul_time_frequency], [ul_channel_estimation],
  /// [ul_equalization_demod] and [ul_ldpc_decode] get the worst-K list the receive and hand-over series already had.
  ///
  /// WHY THIS IS A SEPARATE ENTRY POINT AND NOT record_rx_wait(). The two ends of a phase segment are NOT on the
  /// same thread - a hop is handed from the receive thread to the uplink thread to a pool thread - so the probe
  /// cannot cite "the thread it measured" for the whole span, and the windows are assembled from timestamps the
  /// caller already holds. What it CAN say, and what this records, is the segment's duration, its two instants,
  /// the thread that COMPLETED it, and the process-wide CPU/switches over its window (which is the same reading
  /// the receive events carry, so the two are comparable on the same leg).
  ///
  /// \param[in] kind      Which series (phase_t2f / phase_ce / phase_eqdem / phase_ldpc).
  /// \param[in] value_us  The segment's duration in microseconds (what the series' aggregate distribution holds).
  /// \param[in] begin_ns  Steady instant the segment's window began (its own start landmark).
  /// \param[in] end_ns    Steady instant it ended (the call site's `now`).
  void record_phase_timing_event(timing_event_kind kind,
                                 int64_t           value_us,
                                 int64_t           begin_ns,
                                 int64_t           end_ns,
                                 const cpu_snapshot* base = nullptr)
  {
    // The gate lives in the body (see _locked); this wrapper only avoids taking the lock when the answer is
    // already known to be "no".
    if (!timing_event_wanted_phase(kind, value_us)) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    record_phase_timing_event_locked(kind, value_us, begin_ns, end_ns, base);
  }

  /// The body of the above for a caller that already holds \p mutex (record_ldpc_start assembles the three
  /// segment durations under the lock, and this probe's mutex is a plain std::mutex: re-entering it deadlocks).
  ///
  /// ★ THE KNOB IS CHECKED HERE, IN THE BODY, and that is not redundancy: the first version checked it only in the
  /// public wrapper, while the two callers INSIDE this class (record_ldpc_start, record_end_crc_ok) call this
  /// function directly - so a leg with `OCUDU_UL_TIMING_EVENTS` UNSET still built an event per segment and called
  /// attach_cpu_delta(), i.e. took a Mach thread snapshot (thread_info + pthread_threadid_np +
  /// pthread_getschedparam) four times per PUSCH hop, on the pool threads, against this project's rule that an
  /// unset probe reads no clock and costs nothing. Found on 2026-10-01 by reading leg `p183-n78-default`, which
  /// flew without the knob (dev doc 10.13). One check, in the one place every caller goes through.
  void record_phase_timing_event_locked(timing_event_kind   kind,
                                        int64_t             value_us,
                                        int64_t             begin_ns,
                                        int64_t             end_ns,
                                        const cpu_snapshot* base = nullptr)
  {
    if (!timing_event_wanted_phase(kind, value_us)) {
      return;
    }
    const size_t idx = phase_index(kind);
    ++phase_event_candidates[idx];
    timing_event ev;
    ev.kind     = kind;
    ev.value_us = value_us;
    ev.begin_ns = begin_ns;
    ev.end_ns   = end_ns;
    // The baseline is the one the landmark that STARTED this window took (`base`), which is why these events
    // finally carry a `cpu=`; a null `base` falls back to the receive path's shared one, and that one is usually
    // inside the window - the case the first two radio legs hit (dev doc 10.15 (3)).
    //
    // count_late=false: a phase window may legitimately begin inside the receive path's own baseline cadence
    // (see the parameter's comment); the refusal is counted separately and the line still prints `cpu=-`.
    attach_cpu_delta(ev, begin_ns, end_ns, /*count_late=*/false, base);
    take_phase_timing_event(idx, ev);
  }

  /// \brief Keeps \p ev if it is among the worst `timing_events_limit()` events of its phase series.
  ///
  /// Sorted DESCENDING like the receive list (the worst is the LARGEST duration), so `back()` is the admission bar
  /// and the wall-clock read happens only for an event that is actually kept.
  void take_phase_timing_event(size_t idx, timing_event& ev)
  {
    std::vector<timing_event>& list = worst_phase_events[idx];
    const size_t                limit = timing_events_limit();
    if (limit == 0) {
      return;
    }
    if (list.size() >= limit) {
      if (ev.value_us <= list.back().value_us) {
        return;
      }
      list.pop_back();
    }
    stamp_wall_clock(ev);
    list.push_back(ev);
    std::sort(list.begin(), list.end(), [](const timing_event& lhs, const timing_event& rhs) {
      return lhs.value_us > rhs.value_us;
    });
  }

  /// \brief Records the receive wait of the block that COMPLETED \p slot, for the hop-scoped [ul_rx_wait_hop].  ///
  /// \param[in] slot Slot whose samples are now all in (same reference as record_slot_samples_complete).
  /// \param[in] wait_ns How long that block's receive blocked.
  /// \param[in] spans_stream_start True for the start-up call (see record_rx_wait): it belongs to no hop.
  ///
  /// WHY IT IS SEPARATE FROM [ul_rx_wait]. The two answer different questions and have different populations:
  ///   * [ul_rx_wait] is per BLOCK and covers every block, including the slots that carry no PUSCH - which is what
  ///     makes it the series that catches a transport hiccup landing on an idle slot;
  ///   * [ul_rx_wait_hop] is per HOP - the wait is stored here keyed by slot, and consumed by record_ldpc_start(),
  ///     so it only ends up in the distribution for slots a hop was actually recorded on. That is the population
  ///     [ul_gpu_pipeline]/[ul_pipeline] use, so "the wait for this hop's samples" and "the span of this hop" can
  ///     be read from the same hops. Under the whole-slot policy the completing block IS the hop's block.
  ///
  /// \note The registry is bounded by insertion order like the other pending maps: an entry is normally consumed
  ///       within one hop (~1.5 ms), so the bound only matters when hops stop being recorded (idle slots).
  void record_slot_rx_wait(uint64_t slot, int64_t wait_ns, bool spans_stream_start = false)
  {
    if ((wait_ns < 0) || spans_stream_start) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    slot_rx_wait_us[slot] = static_cast<double>(wait_ns) / 1e3;
    while (slot_rx_wait_us.size() > max_slot_rx_wait) {
      slot_rx_wait_us.erase(slot_rx_wait_us.begin());
    }
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
    //
    // A BASE REFRESH REBASES THE SLOT (measured 2026-09-28, legs p87/p88). The key is the MODULAR slot, so a
    // later SFN cycle completes the same key again - and any landmark still stored under it belongs to the cycle
    // that just ended. That matters because not every landmark is re-recorded every cycle: PUXCH completes the
    // symbols of every slot it processes, so `t2f` DOES arrive again, while `ce`/`ldpc_start`/`crc_ok` only
    // arrive when the slot carries a grant whose transport block is processed and decoded. A slot that carried a
    // PUSCH one SFN cycle ago and none now therefore used to pair THIS cycle's base and t2f with the PREVIOUS
    // cycle's three instants, and printed them as deltas of MINUS one SFN cycle (10.24 s): measured 4/17/42 such
    // rows in ce/ldpc/crc_ok on p87 and 77/77/80 on p88. Dropping them here is what makes a row describe ONE
    // cycle, and it cannot lose anything of the cycle this base belongs to: a landmark is measured on samples
    // that this very arrival completed, so this cycle's landmarks all arrive after this point.
    for (auto it = slot_landmarks.lower_bound({slot, slot_trace_what::t2f});
         (it != slot_landmarks.end()) && (it->first.first == slot);) {
      it = slot_landmarks.erase(it);
      ++slot_trace_rebased;
    }
    // ... AND THE ROW'S OWN DELTAS BELONG TO THAT SAME CYCLE, so they go with the landmarks. This second half is
    // not optional: dropping only the map entries leaves the row printing last cycle's spans next to this cycle's
    // base - and those look PLAUSIBLE (measured in the unit fixture written for this fix: ce/ldpc/crc_ok came back
    // as ~2 us, i.e. last cycle's values), which is worse than the negative numbers the map leak produced. The
    // fields go back to NaN, their documented value for "this landmark was not reached for this slot": the new
    // cycle fills in the ones it does reach (see trace_slot), and a slot with no grant this time round reports
    // exactly that instead of a remembered hop.
    if (auto row_it = slot_trace.find(slot); row_it != slot_trace.end()) {
      row_it->second.t2f_us        = std::numeric_limits<double>::quiet_NaN();
      row_it->second.ce_us         = std::numeric_limits<double>::quiet_NaN();
      row_it->second.ldpc_start_us = std::numeric_limits<double>::quiet_NaN();
      row_it->second.crc_ok_us     = std::numeric_limits<double>::quiet_NaN();
      row_it->second.tb_bytes      = std::numeric_limits<double>::quiet_NaN();
      row_it->second.pipeline_us   = std::numeric_limits<double>::quiet_NaN();
      // The two raw epochs are NOT touched here: they are the base and the landmark the row's deltas were last
      // computed against, and the row's next landmark update (trace_slot) rewrites both. Moving the base alone
      // would break the invariant the report is read with (base <= landmark, pinned by
      // ul_slot_trace_test.the_raw_instants_describe_the_same_frame_as_the_deltas) and would claim a base no
      // delta was ever measured from.
    }
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

  /// \brief How many of the WORST timing events to print with their host instants (OCUDU_UL_TIMING_EVENTS=N).
  ///
  /// THE QUESTION THIS ANSWERS (dev doc 6.240). A leg can read a receive wait of 12 ms against a mean of 34 us, and
  /// the aggregates cannot say WHEN it happened - so it cannot be lined up against the two independent witnesses a
  /// leg already has: the `[RF] Real-time failure in RF: underflow|late` lines (which carry wall-clock timestamps)
  /// and the DL hand-overs that missed their due time. Three series, one time axis, is the only way to tell
  /// "the radio/USB path stalled" from "the host was descheduled" from "something in this process blocked".
  ///
  /// WHY IT NEEDS A KEY OF ITS OWN, AND WHY IT IS CHEAP. It is off by default (0 when unset) and only RANKS events
  /// above a floor (1 ms for a receive wait, 500 us of margin for a hand-over), so a healthy leg keeps ~0-20 of
  /// them: the wall-clock read and the load average are taken ONLY for an event that enters the list, and the
  /// per-call cost when the knob is on is one integer compare. With the knob unset nothing is stored, nothing is
  /// printed and no clock is read - the "off is byte-identical" half of the probe contract (dev doc 6.145 (3)).
  ///
  /// `OCUDU_UL_TIMING_EVENTS=1` (or any non-numeric value) means "on, with the default count", for the reason
  /// slot_trace_limit() gives: an operator asking for the instrument should not have to guess a good number.
  static unsigned timing_events_limit()
  {
    const char* env = std::getenv("OCUDU_UL_TIMING_EVENTS");
    if (env == nullptr) {
      return 0;
    }
    if ((env[0] == '\0') || (std::strtoul(env, nullptr, 10) == 0)) {
      // "0" is the explicit OFF, and a non-numeric value is a request for the default rather than a silent 0.
      const bool numeric = (env[0] >= '0') && (env[0] <= '9');
      return numeric ? 0U : default_timing_events;
    }
    const unsigned v = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
    return (v > max_timing_events) ? max_timing_events : v;
  }

  static bool timing_events_enabled() { return timing_events_limit() != 0; }

  /// Whether a receive wait is worth keeping: the floor is what keeps a healthy leg's list empty and its cost zero.
  static bool timing_event_wanted_rx(int64_t wait_ns)
  {
    return (timing_events_limit() != 0) && (wait_ns >= timing_event_rx_floor_ns);
  }

  /// The same question for a hand-over: its healthy margin is ~1011 us, so the floor is the "below 500 us" bucket
  /// the report already counts, and a late hand-over (margin <= 0) is always kept.
  static bool timing_event_wanted_tx(int64_t margin_us)
  {
    return (timing_events_limit() != 0) && (margin_us < timing_event_tx_floor_us);
  }

  /// Whether the caller should take a fresh process-wide baseline now (throttled, and only with the knob on).
  bool timing_event_snapshot_wanted(int64_t now_ns) const
  {
    if (timing_events_limit() == 0) {
      return false;
    }
    return !cpu_base.valid || ((now_ns - cpu_base.ns) >= timing_event_cpu_period_ns);
  }

  /// Takes the process-wide AND thread-local baseline. Called from the receive path just before the measured
  /// call, so the snapshot always PRECEDES the window it will be subtracted from (its age is reported as
  /// base_age_us).
  ///
  /// getrusage(RUSAGE_SELF) is the whole process (every thread), which is the question: "did this process get the
  /// CPU while the call was outstanding". The THREAD half (P1) is the sharper one and it is a different question
  /// again: "did THIS thread get the CPU". The blocked receive thread burns no CPU either way, so `cpu=` alone
  /// cannot tell "this thread was descheduled while its siblings ran" from "nothing in this process ran", and
  /// those two have different fixes.
  ///
  /// \note RUSAGE_THREAD does not exist on macOS (checked on this SDK); the thread half comes from Mach there
  ///       (THREAD_BASIC_INFO) and from RUSAGE_THREAD on Linux, behind one interface - see
  ///       ocudu/support/scheduling/thread_sched_snapshot.h. Both are taken while holding this probe's lock: they
  ///       are pure reads with no callback into it.
  void timing_event_snapshot(int64_t now_ns)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const cpu_snapshot fresh = read_cpu_snapshot(now_ns);
    if (!fresh.valid) {
      return;
    }
    cpu_base = fresh;
    // The leg's own base rate, kept from the FIRST baseline of the run: the per-event switch deltas below are
    // only readable against it ("+412 involuntary switches" means nothing without "this process switches 194k
    // times a second", which is what taskinfo measured on the reference leg).
    if (!leg_base_valid) {
      leg_base       = cpu_base;
      leg_base_valid = true;
    }
  }

  /// \brief Reads a process+thread snapshot WITHOUT publishing it as the shared baseline.
  ///
  /// Split out of timing_event_snapshot() so the phase landmarks can take their own (dev doc 10.15 (3)): a phase
  /// window needs a baseline that precedes ITS start, and the receive path's shared one is refreshed on a ~1 ms
  /// cadence - usually inside the window, which is why those events used to print `cpu=-`.
  ///
  /// \note Returns an INVALID snapshot (not a zeroed one) when the knob is off or the platform refuses: the
  ///       caller stores it in a registry entry either way, and "no reading" must not become "zero CPU".
  cpu_snapshot read_cpu_snapshot(int64_t now_ns)
  {
    cpu_snapshot snap;
#if !defined(_WIN32)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
      return snap;
    }
    const thread_sched_snapshot self = this_thread_sched_snapshot();
    snap.ns     = now_ns;
    snap.cpu_ns = (static_cast<int64_t>(ru.ru_utime.tv_sec) + static_cast<int64_t>(ru.ru_stime.tv_sec)) * 1000000000LL +
                  (static_cast<int64_t>(ru.ru_utime.tv_usec) + static_cast<int64_t>(ru.ru_stime.tv_usec)) * 1000LL;
    snap.nvcsw  = static_cast<int64_t>(ru.ru_nvcsw);
    snap.ivcsw  = static_cast<int64_t>(ru.ru_nivcsw);
    snap.thread_id     = self.thread_id;
    snap.thread_cpu_ns = self.cpu_ns;
    snap.valid         = true;
#else
    (void)now_ns;
#endif
    // Counted so the reverse arm has something to observe: with the knob unset this must stay 0, because the
    // snapshot is a getrusage plus a Mach call on a per-hop path (see phase_baselines_taken).
    ++phase_baselines_taken;
    return snap;
  }

  /// \brief The snapshot a phase landmark attaches to its registry entry, or an invalid one when the knob is off.
  ///
  /// The gate is HERE rather than at the three call sites so the expensive part cannot be reached by accident -
  /// the same mistake the phase-event recorder itself made once (dev doc 10.13).
  cpu_snapshot phase_baseline_for(int64_t now_ns)
  {
    if (timing_events_limit() == 0) {
      return cpu_snapshot{};
    }
    return read_cpu_snapshot(now_ns);
  }

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
  /// \param[in] begin_ns/end_ns The window itself, on the same steady clock record_rx_wait() timestamps with,
  ///                 when the caller can supply it: that is what turns this series into the same-leg test for the
  ///                 receive tail (see the overlap account below). 0 means "duration only".
  void record_dft_wait(int64_t wait_ns,
                       int64_t begin_ns         = 0,
                       int64_t end_ns           = 0,
                       int64_t commit_begin_ns  = 0,
                       int64_t gpu_begin_ns     = 0,
                       int64_t gpu_end_ns       = 0,
                       int64_t gpu_offset_ns    = 0)
  {
    if (wait_ns < 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    dft_wait_us.push_back(static_cast<double>(wait_ns) / 1e3);
    const auto place = [&](dft_window_kind kind, int64_t b, int64_t e) {
      if ((b == 0) || (e <= b)) {
        return;
      }
      const size_t k                       = static_cast<size_t>(kind);
      dft_block_windows[k][dft_block_next[k]] = dft_block_window{b, e};
      dft_block_next[k]                    = (dft_block_next[k] + 1) % max_dft_block_windows;
      ++dft_block_count[k];
      dft_block_total_ns[k] += static_cast<uint64_t>(e - b);
    };
    place(dft_window_kind::wait, begin_ns, end_ns);
    // The commit window runs from just before the command buffer is committed to the end of the wait, so it
    // contains whatever the driver does at submission time - the interval M2's wait-only window could not see.
    place(dft_window_kind::commit_to_end, commit_begin_ns, end_ns);
    place(dft_window_kind::gpu, gpu_begin_ns, gpu_end_ns);
    if (gpu_offset_ns != 0) {
      gpu_clock_offset_ns = gpu_offset_ns;
      ++gpu_clock_samples;
    }
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
    auto       us   = [&base, this](const std::chrono::high_resolution_clock::time_point& tp) {
      // A landmark OLDER than the base cannot be a span of this slot: it is a leftover of a previous SFN cycle
      // (see record_slot_samples_complete, which now drops those, so this should never fire) or a base that
      // moved under it. Report NaN - a negative span is a confident wrong number, which is the one thing this
      // trace must not print - and COUNT it, so the invariant stays visible in the header line instead of
      // becoming a silent filter that a later reader would trust.
      if (tp < base) {
        ++slot_trace_negative;
        return std::numeric_limits<double>::quiet_NaN();
      }
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
    // The two invariant counters travel with the rows they describe: `rebased` says how many stale instants the
    // rebase dropped (see record_slot_samples_complete - expected to be large on an air leg, one per traced slot
    // per SFN cycle), and `negative` must read 0. A reader who sees a delta of -10.24 s and a `negative=0` beside
    // it knows the row is describing one cycle.
    std::fprintf(stderr,
                 "[ul_slot_trace] rebased=%llu landmark(s) dropped with a refreshed base; negative deltas refused=%llu\n",
                 static_cast<unsigned long long>(slot_trace_rebased),
                 static_cast<unsigned long long>(slot_trace_negative));
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

  /// \brief Forgets every recorded SAMPLE (the series' distributions), leaving the counters and the ranked event
  /// lists alone.
  ///
  /// WHY IT EXISTS. The within-run stability view is about the ORDER of a series' samples, so a unit case that
  /// wants to state "this run drifts" and "that run does not" has to say which samples it is talking about - and
  /// the probe is a process-wide singleton whose vectors are private. Appending two shapes to one stream and
  /// asserting on the printout would give DIFFERENT lines depending on whether the binary ran the cases in one
  /// process or one process each (gtest_discover_tests does the latter, a direct run the former), i.e. the run
  /// mode would decide the verdict - the trap the ranked-list case already hit once. A reset makes the two arms
  /// independent by construction, in either mode.
  ///
  /// \note Test hook, not production API: nothing in the pipeline calls it, and it is named so that a reader
  ///       cannot mistake it for one.
  void reset_samples_for_test()
  {
    std::lock_guard<std::mutex> lock(mutex);
    latencies_us.clear();
    stale_pipeline_us.clear();
    stale_gpu_pipeline_us.clear();
    t2f_latencies_us.clear();
    ce_latencies_us.clear();
    eqdem_latencies_us.clear();
    gpu_pipeline_latencies_us.clear();
    ldpc_latencies_us.clear();
    mac_pdu_sizes_bytes.clear();
    fapi_mac_latencies_us.clear();
    rx_wait_us.clear();
    rx_wait_hop_us.clear();
    dft_wait_us.clear();
    // ★ THE COUNTERS TOO, not only the vectors (2026-10-02). The per-series floor lines are a numerator COUNTER
    // over a denominator VECTOR, so clearing one and not the other makes the two describe different runs - which
    // is exactly how the first version of `window_counts_agree_with_the_floor_line` passed when ctest ran it in
    // its own process and failed when the whole binary ran every case in one: an earlier case's above-floor
    // samples were still in the numerator. A test hook that leaves the instrument half-cleared is worse than none.
    rx_event_candidates   = 0;
    tx_event_candidates   = 0;
    late_baselines        = 0;
    phase_baseline_misses = 0;
    for (auto& count : phase_event_candidates) {
      count = 0;
    }
    for (auto& list : worst_phase_events) {
      list.clear();
    }
    worst_rx_events.clear();
    worst_tx_events.clear();
  }

  /// How many equal-sample windows the within-run stability view cuts each series into (0 = the view is off).
  ///
  /// `OCUDU_UL_STABILITY_WINDOWS=K`: K windows of equal SAMPLE count, in time order. 1 is refused (one window is
  /// the whole run and would report "perfectly stable" for anything); a non-numeric value means the default 8,
  /// for the reason slot_trace_limit() gives - an operator asking for the view should not have to guess a count.
  static unsigned stability_windows()
  {
    const char* env = std::getenv("OCUDU_UL_STABILITY_WINDOWS");
    if (env == nullptr) {
      return 0;
    }
    const unsigned v = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
    if (v <= 1) {
      return (env[0] >= '0' && env[0] <= '9' && v == 1) ? 0U : 8U;
    }
    return (v > 64) ? 64U : v;
  }

  /// \brief One line per series: what each K-th of the run looked like, and how far the worst one strayed.
  ///
  /// \param[in] time_ordered The series' samples IN THE ORDER THEY WERE RECORDED (which is what makes the windows
  ///            mean something); the whole-run reference values are computed from the same vector, so the
  ///            comparison cannot drift from the series it is about.
  static void print_window_stability(const char*            name,
                                     const std::vector<double>& time_ordered,
                                     unsigned               windows,
                                     double                 tail_floor_us)
  {
    if (time_ordered.size() < static_cast<size_t>(windows) * 8) {
      // Too few samples for the cut to say anything: SAY so rather than print a table of 3-sample windows, whose
      // deviations would be sampling noise dressed as instability.
      std::fprintf(stderr,
                   "  %-22s only %zu sample(s): fewer than %u per window, not cut\n",
                   name,
                   time_ordered.size(),
                   windows * 8);
      return;
    }
    const auto stat_of = [](std::vector<double> v, double p) {
      std::sort(v.begin(), v.end());
      return v[static_cast<size_t>((v.size() - 1) * p)];
    };
    const double ref_median = stat_of(time_ordered, 0.5);
    const double ref_p95    = stat_of(time_ordered, 0.95);
    const size_t per_window = time_ordered.size() / windows;
    std::string  medians;
    std::string  p95s;
    std::string  tails;
    double       worst_dev = 0.0;
    for (unsigned w = 0; w != windows; ++w) {
      const size_t begin = w * per_window;
      const size_t end   = (w + 1 == windows) ? time_ordered.size() : begin + per_window;
      const std::vector<double> slice(time_ordered.begin() + begin, time_ordered.begin() + end);
      const double              med = stat_of(slice, 0.5);
      const double              p95 = stat_of(slice, 0.95);
      char                      buf[32];
      std::snprintf(buf, sizeof(buf), " %.1f", med);
      medians += buf;
      std::snprintf(buf, sizeof(buf), " %.1f", p95);
      p95s += buf;
      // ... and HOW MANY of this window's samples crossed the tail floor. This is what makes a WITHIN-LEG A/B
      // decidable (dev doc 10.23): the tier/priority being tested can be changed half-way through one run, which
      // controls the environment, and the reading is then a count per window rather than one `max` per leg.
      // `>=`, NOT `>`: this count and the per-series floor line ("N of M sample(s) above the F us floor") are two
      // views of the same quantity, and the phase durations arrive quantized to whole microseconds (they come from
      // a us-resolution source), so samples sitting EXACTLY on the floor are common - `t2f` 6 vs 2 and `ce` 353 vs
      // 314 on the 2026-10-02 long legs, purely from that boundary. The floor line is the established reading (the
      // registered bounds and every derivation quoted it), so the window view is the one that moves. The test
      // `window_counts_agree_with_the_floor_line` pins the two together.
      const size_t over = static_cast<size_t>(std::count_if(slice.begin(), slice.end(), [tail_floor_us](double v) {
        return v >= tail_floor_us;
      }));
      std::snprintf(buf, sizeof(buf), " %zu", over);
      tails += buf;
      if (ref_median > 0) {
        worst_dev = std::max(worst_dev, std::fabs(med - ref_median) / ref_median * 100.0);
        worst_dev = std::max(worst_dev, std::fabs(p95 - ref_p95) / ref_p95 * 100.0);
      }
    }
    std::fprintf(stderr,
                 "  %-22s n=%-8zu median[%s ] p95[%s ] over %lluus[%s ]  worst window vs whole run: %.1f%%\n",
                 name,
                 time_ordered.size(),
                 medians.c_str(),
                 p95s.c_str(),
                 static_cast<unsigned long long>(tail_floor_us),
                 tails.c_str(),
                 worst_dev);
  }

  /// \brief Prints the worst receive waits and hand-over margins WITH their host wall clocks (dev doc 6.240/6.241).
  ///
  /// WHAT IT IS FOR. The three things a leg can see about a stall live in three files and two clocks: the receive
  /// tail is an aggregate here, the DL hand-overs that missed their due time are an aggregate here, and the radio's
  /// own `[RF] Real-time failure in RF: underflow|late` lines are wall-clock timestamps in the `.log`. This block
  /// is the missing half - WHEN each of the worst events happened - so the three can be put on one axis.
  ///
  /// It is a READING, never a criterion (the aggregates and the judged rows stay what they were), and it is
  /// printed ONLY when `OCUDU_UL_TIMING_EVENTS` asked for it: with the knob unset the report is byte-identical.
  /// When it IS on and nothing was above the floor, the block still prints - one line saying so, because "the
  /// instrument was on and saw nothing" and "the instrument was never built in" must not look alike (the same rule
  /// the slot-trace line follows).
  void print_timing_events()
  {
    const unsigned limit = timing_events_limit();
    if (limit == 0) {
      return;
    }
    std::fprintf(stderr,
                 "[ul_timing_events] limit=%u (OCUDU_UL_TIMING_EVENTS=%u): the worst receive waits, hand-over "
                 "margins and phase segments, with the host wall clock\n",
                 limit,
                 limit);
    std::fprintf(stderr,
                 "  line them up against the .log's `[RF] Real-time failure in RF: ...` lines; wall= is UTC, "
                 "epoch_ms= is the same instant as an integer\n");
    // WHAT EACH CPU COLUMN IS, printed once because the two are read together and mean different things:
    // `cpu=` is the PROCESS over the window (every thread) and `tcpu=` is the RECORDING THREAD alone. A stall with
    // `cpu=12.00ms tcpu=0.00ms` is "this thread lost the core while its siblings ran" - a scheduling problem -
    // while `cpu=0.00ms` is "the process lost the core" and `cpu≈win tcpu≈win` is "this thread ran the whole time
    // and the work/IO itself took that long". `tcpu=-` means no reading (the baseline was another thread's).
    std::fprintf(stderr,
                 "  cpu= is the PROCESS over the event's window (win=); tcpu= is the RECORDING THREAD alone; "
                 "ivcsw_rate= is the process's involuntary switches per ms over that window (compare it against "
                 "the leg rate below)\n");
    if (leg_base_valid && (leg_last_ns > leg_first_ns)) {
      // The leg-wide base rate, measured over the SAME counters the per-event deltas use, so the two are
      // commensurable by construction (an outside tool's csw/s is a different measurement of the same process).
      const int64_t span_ms = (leg_last_ns - leg_first_ns) / 1000000;
#if !defined(_WIN32)
      rusage ru{};
      if ((span_ms > 0) && (getrusage(RUSAGE_SELF, &ru) == 0)) {
        const int64_t ivcsw = static_cast<int64_t>(ru.ru_nivcsw) - leg_base.ivcsw;
        const int64_t nvcsw = static_cast<int64_t>(ru.ru_nvcsw) - leg_base.nvcsw;
        std::fprintf(stderr,
                     "  leg : over %.1fs of receive activity the process made %lld involuntary and %lld voluntary "
                     "switch(es) = %.2f/ms and %.2f/ms\n",
                     static_cast<double>(span_ms) / 1000.0,
                     static_cast<long long>(ivcsw),
                     static_cast<long long>(nvcsw),
                     static_cast<double>(ivcsw) / static_cast<double>(span_ms),
                     static_cast<double>(nvcsw) / static_cast<double>(span_ms));
      }
#endif
    }
    if (late_baselines != 0) {
      std::fprintf(stderr,
                   "  ⚠ %llu event(s) had their CPU baseline stamped INSIDE the window and were refused "
                   "(cpu=-): the caller's ordering is wrong, fix the call site rather than reading the `-` as "
                   "zero CPU\n",
                   static_cast<unsigned long long>(late_baselines));
    }
    if (phase_baseline_misses != 0) {
      std::fprintf(stderr,
                   "  note: %llu phase event(s) started before the receive path's last baseline and print "
                   "cpu=- tcpu=- (arithmetic, not a caller's mistake: the baseline is refreshed every ~1 ms by "
                   "the receive path, and a phase window can begin inside that cadence)\n",
                   static_cast<unsigned long long>(phase_baseline_misses));
    }
    // THE TAIL AS A RATE, ALWAYS - not only when the list is empty (dev doc 10.23). `max` is one draw from a
    // heavy tail, so two legs cannot be compared by it: the decidable quantity is HOW OFTEN the floor is crossed
    // out of how many samples. The numerator was already counted; the denominator is the series' own population,
    // which is what makes the number a rate (and the two are taken from the same object, so they cannot drift).
    std::fprintf(stderr,
                 "  rx  : %llu of %zu receive(s) above the %lld us floor = %s\n",
                 static_cast<unsigned long long>(rx_event_candidates),
                 rx_wait_us.size(),
                 static_cast<long long>(timing_event_rx_floor_ns / 1000),
                 rate_pct(rx_event_candidates, rx_wait_us.size()).c_str());
    unsigned rank = 0;
    for (const timing_event& ev : worst_rx_events) {
      char wall[32];
      format_wall_utc(ev.wall_ms, wall, sizeof(wall));
      // BOTH ENDS of the window are printed: `wall=` is the instant the wait ENDED (the samples were in hand),
      // so a reader aligning this against a `[RF]` line must know where the stall BEGAN - `began_ms=` is
      // `epoch_ms - wait`, computed here rather than stored, so the two cannot drift apart.
      std::fprintf(stderr,
                   "  rx  #%u wait=%lldus air=%lldus wall=%s epoch_ms=%lld began_ms=%lld steady_end_ns=%lld "
                   "thread=%s#%llu load1=%s cpu=%s tcpu=%s ivcsw=%s ivcsw_rate=%s nvcsw=%s win=%s base_age=%s\n",
                   ++rank,
                   static_cast<long long>(ev.value_us),
                   static_cast<long long>(ev.air_us),
                   wall,
                   static_cast<long long>(ev.wall_ms),
                   static_cast<long long>(ev.wall_ms) - static_cast<long long>(ev.value_us) / 1000,
                   static_cast<long long>(ev.end_ns),
                   ev.thread_name,
                   static_cast<unsigned long long>(ev.thread_id),
                   load1_str(ev.load1_x100).c_str(),
                   cpu_str(ev.cpu_ns).c_str(),
                   cpu_str(ev.tcpu_ns).c_str(),
                   delta_str(ev.ivcsw).c_str(),
                   rate_str(ev.ivcsw, ev.win_us).c_str(),
                   delta_str(ev.nvcsw).c_str(),
                   age_str(ev.win_us).c_str(),
                   age_str(ev.base_age_us).c_str());
    }
    std::fprintf(stderr,
                 "  dl  : %llu hand-over(s) below the %lld us margin floor (rate against the leg's own "
                 "transmission count, printed by [dl_tx_slack] as `transmissions=`)\n",
                 static_cast<unsigned long long>(tx_event_candidates),
                 static_cast<long long>(timing_event_tx_floor_us));
    rank = 0;
    for (const timing_event& ev : worst_tx_events) {
      char wall[32];
      format_wall_utc(ev.wall_ms, wall, sizeof(wall));
      // `due_ms=` is when the hand-over SHOULD have happened (`epoch_ms + margin`, negative margin = in the
      // past), i.e. the same two ends for the transmit direction.
      //
      // `tcpu=-` HERE ALWAYS, and by construction: this event is recorded on the TRANSMIT thread while the only
      // baseline is the one the RECEIVE path maintains, and two different threads' cumulative CPU counters
      // cannot be subtracted (see timing_event::tcpu_ns). The process-wide `cpu=` is still a reading.
      std::fprintf(stderr,
                   "  dl  #%u margin=%lldus due_ts=%lld wall=%s epoch_ms=%lld due_ms=%lld steady_end_ns=%lld "
                   "thread=%s#%llu load1=%s cpu=%s tcpu=%s ivcsw=%s ivcsw_rate=%s nvcsw=%s win=%s\n",
                   ++rank,
                   static_cast<long long>(ev.value_us),
                   static_cast<long long>(ev.due_ts),
                   wall,
                   static_cast<long long>(ev.wall_ms),
                   static_cast<long long>(ev.wall_ms) + static_cast<long long>(ev.value_us) / 1000,
                   static_cast<long long>(ev.end_ns),
                   ev.thread_name,
                   static_cast<unsigned long long>(ev.thread_id),
                   load1_str(ev.load1_x100).c_str(),
                   cpu_str(ev.cpu_ns).c_str(),
                   cpu_str(ev.tcpu_ns).c_str(),
                   delta_str(ev.ivcsw).c_str(),
                   rate_str(ev.ivcsw, ev.win_us).c_str(),
                   delta_str(ev.nvcsw).c_str(),
                   age_str(ev.win_us).c_str());
    }
    // The four PHASE series, one block each, in the same shape as the two above: this is what turns "the CE max is
    // 20x its median" (the observation this whole workstream started from) into "THAT slot's CE segment took 1.2 ms
    // and the thread that completed it is main_pool#3".
    for (size_t i = 0; i != timing_event_phase_floor_us_size; ++i) {
      const timing_event_kind kind =
          static_cast<timing_event_kind>(static_cast<size_t>(timing_event_kind::phase_t2f) + i);
      const char* name = phase_series_name(kind);
      // The series' own sample count is the denominator: the same series the aggregate lines print below, so a
      // reader can divide one by the other and get a rate that means something (`ce` 3 of 38274 = 0.008%).
      const size_t population = phase_series_population(kind);
      std::fprintf(stderr,
                   "  %-4s: %llu of %zu sample(s) above the %lld us floor = %s\n",
                   name,
                   static_cast<unsigned long long>(phase_event_candidates[i]),
                   population,
                   static_cast<long long>(timing_event_phase_floor_us[i]),
                   rate_pct(phase_event_candidates[i], population).c_str());
      if (worst_phase_events[i].empty()) {
        continue;
      }
      rank = 0;
      for (const timing_event& ev : worst_phase_events[i]) {
        char wall[32];
        format_wall_utc(ev.wall_ms, wall, sizeof(wall));
        std::fprintf(stderr,
                     "  %-4s#%u took=%lldus wall=%s epoch_ms=%lld steady_end_ns=%lld thread=%s#%llu cpu=%s "
                     "tcpu=%s ivcsw=%s ivcsw_rate=%s nvcsw=%s win=%s base_age=%s\n",
                     name,
                     ++rank,
                     static_cast<long long>(ev.value_us),
                     wall,
                     static_cast<long long>(ev.wall_ms),
                     static_cast<long long>(ev.end_ns),
                     ev.thread_name,
                     static_cast<unsigned long long>(ev.thread_id),
                     cpu_str(ev.cpu_ns).c_str(),
                     cpu_str(ev.tcpu_ns).c_str(),
                     delta_str(ev.ivcsw).c_str(),
                     rate_str(ev.ivcsw, ev.win_us).c_str(),
                     delta_str(ev.nvcsw).c_str(),
                     age_str(ev.win_us).c_str(),
                     age_str(ev.base_age_us).c_str());
      }
    }
  }

  /// The short report tag of a phase series ("t2f", "ce", "eqd", "ldpc"), i.e. the same words the aggregate lines
  /// are read with, shortened to keep the event lines inside a terminal.
  static const char* phase_series_name(timing_event_kind kind)
  {
    switch (kind) {
      case timing_event_kind::phase_t2f:
        return "t2f";
      case timing_event_kind::phase_ce:
        return "ce";
      case timing_event_kind::phase_eqdem:
        return "eqd";
      case timing_event_kind::phase_ldpc:
        return "ldpc";
      default:
        break;
    }
    return "?";
  }

  /// \brief A count as a percentage of its population, or `-` when the population is empty.
  ///
  /// Printed with four decimals because these tails are TENS of events out of tens of thousands: "0.0%" would
  /// hide the difference the whole exercise is about (3 of 38274 is 0.0078%, and 30 of 38274 is 0.078% - the same
  /// "0.0%" to one decimal).
  static std::string rate_pct(uint64_t count, size_t population)
  {
    if (population == 0) {
      return "-";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f%%", static_cast<double>(count) * 100.0 / static_cast<double>(population));
    return buf;
  }

  /// The number of samples the series behind \p kind recorded (the denominator of its tail rate).
  size_t phase_series_population(timing_event_kind kind) const
  {
    switch (kind) {
      case timing_event_kind::phase_t2f:
        return t2f_latencies_us.size();
      case timing_event_kind::phase_ce:
        return ce_latencies_us.size();
      case timing_event_kind::phase_eqdem:
        return eqdem_latencies_us.size();
      case timing_event_kind::phase_ldpc:
        return ldpc_latencies_us.size();
      default:
        break;
    }
    return 0;
  }

  /// The process CPU time consumed over the window, in ms with two decimals, or `-` when there was no baseline.
  /// Read it AGAINST the window: `cpu` close to `wait` says the process kept its cores (the radio/USB side was
  /// late), `cpu` near zero says it did not run at all (host scheduling).
  /// \note These formatters return std::string, NOT a pointer into a static buffer. The first version returned
  /// `static thread_local char buf[]` and every call site printed TWO of them in one fprintf (win and base_age,
  /// ivcsw and nvcsw): both arguments are the same pointer, the second call overwrites the first, and BOTH fields
  /// print the same number. Measured on `p180-n78-stress`'s report plumbing: `win=0us base_age=0us` for an event
  /// whose window was 18 ms long, with the value itself correct inside the struct - i.e. a silent corruption that
  /// only shows when the two numbers differ (dev doc 6.245).
  static std::string cpu_str(int64_t cpu_ns)
  {
    if (cpu_ns < 0) {
      return "-";
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld.%02lldms", static_cast<long long>(cpu_ns / 1000000),
                  static_cast<long long>((cpu_ns % 1000000) / 10000));
    return buf;
  }

  /// A switch-count delta, or `-` when there was no baseline.
  static std::string delta_str(int64_t v)
  {
    if (v < 0) {
      return "-";
    }
    // 24 bytes, not 16: a long long prints up to 20 characters, and GCC's -Werror=format-truncation refuses the
    // smaller buffer - which it only ever saw on Linux, where this probe had never been built with
    // ENABLE_FLOW_PROBES=ON until 2026-10-01 (the bench runs the project default, probes OFF).
    char buf[24];
    std::snprintf(buf, sizeof(buf), "+%lld", static_cast<long long>(v));
    return buf;
  }

  /// The same delta NORMALIZED by the window it was measured over, in switches per millisecond.
  ///
  /// WHY (P1). An unnormalized delta cannot be read: the receive events' windows are ~1 ms in steady state but can
  /// be a whole stall wide, and a hand-over's window is whatever the receive path's last baseline was - so
  /// "+412 involuntary switches" is large for one event and small for another purely because of the window. The
  /// rate is what makes two events, and two legs, comparable; it is printed next to the delta rather than
  /// replacing it, because the delta is what the counters actually said.
  static std::string rate_str(int64_t switches, int64_t win_us)
  {
    if ((switches < 0) || (win_us <= 0)) {
      return "-";
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f/ms", static_cast<double>(switches) * 1000.0 / static_cast<double>(win_us));
    return buf;
  }

  /// How stale the baseline was, in us.
  static std::string age_str(int64_t us)
  {
    if (us < 0) {
      return "-";
    }
    // 24 bytes, not 16: a long long prints up to 20 characters, and GCC's -Werror=format-truncation refuses the
    // smaller buffer - which it only ever saw on Linux, where this probe had never been built with
    // ENABLE_FLOW_PROBES=ON until 2026-10-01 (the bench runs the project default, probes OFF).
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lldus", static_cast<long long>(us));
    return buf;
  }

  /// The load average as a fixed-point string, or `-` when the caller had no reading (see the platform note in
  /// lower_phy_baseband_processor.cpp: getloadavg is POSIX and the callers gate it themselves).
  static std::string load1_str(int64_t load1_x100)
  {
    if (load1_x100 < 0) {
      return "-";
    }
    // 24 bytes, not 16: a long long prints up to 20 characters, and GCC's -Werror=format-truncation refuses the
    // smaller buffer - which it only ever saw on Linux, where this probe had never been built with
    // ENABLE_FLOW_PROBES=ON until 2026-10-01 (the bench runs the project default, probes OFF).
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%lld.%02lld", static_cast<long long>(load1_x100 / 100),
                  static_cast<long long>(load1_x100 % 100));
    return buf;
  }

  /// UTC so it can be compared with a log line without knowing this process's timezone, and fractional to the
  /// millisecond because a stall and the `[RF]` line it belongs to are milliseconds apart.
  static void format_wall_utc(int64_t epoch_ms, char* out, size_t n)
  {
    const std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm           tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    std::strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
    const size_t len = std::strlen(out);
    std::snprintf(out + len, (n > len) ? (n - len) : 0, ".%03lld", static_cast<long long>(epoch_ms % 1000));
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
    std::vector<double> sorted_rx_wait_hop;
    double              startup_rx_wait_us = std::numeric_limits<double>::quiet_NaN();
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
      sorted_rx_wait_hop  = rx_wait_hop_us;
      startup_rx_wait_us  = rx_wait_startup_us;
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

    // ---- the WITHIN-RUN stability view (dev doc 10.20) --------------------------------------------------------
    // WHAT THE USER MEANS BY "running stability" (2026-10-01, and it is a definition, not a preference): when the
    // PHY threads run THE SAME TASK the time it takes should barely change - and because DIFFERENT RUNS may
    // legitimately differ (the radio environment and the traffic type change between them), the quantity to
    // measure is the one INSIDE one run: are the run's own statistics the same in its first tenth as in its last?
    //
    // It is computed here, at report time, from data this probe ALREADY keeps, and that is why it is free: every
    // series vector is appended in TIME ORDER, so splitting it into K equal-count windows is a slice of the same
    // vector - no per-sample timestamp, no histogram, nothing on the hot path. Each window's statistic is then
    // compared with the whole run's, and the widest deviation is printed per series.
    //
    // Gate: OCUDU_UL_STABILITY_WINDOWS=K (0/unset = no output at all, so every existing report is byte-identical;
    // it is a print-only probe and belongs in the gate's whitelist - it changes no delivery decision).
    // The per-thread per-slot CPU accounting (dev doc 10.30(8)) is printed here, next to the series it belongs
    // with: it is the report P4's constraint parameters are read from, and it prints nothing when its knob is off
    // - so every existing leg report stays byte-identical.
    print_thread_cpu_accounting();

    const unsigned stability_window_count = stability_windows();
    if (stability_window_count > 1) {
      std::fprintf(stderr,
                   "[ul_stability] OCUDU_UL_STABILITY_WINDOWS=%u: the run cut into %u equal-sample windows IN TIME "
                   "ORDER; a stable run repeats its own statistics\n",
                   stability_window_count,
                   stability_window_count);
      print_window_stability("ul_pipeline", sorted_pipeline, stability_window_count, 2000.0);
      print_window_stability("ul_gpu_pipeline", sorted_gpu_pipeline, stability_window_count, 2000.0);
      print_window_stability("ul_time_frequency", sorted_t2f, stability_window_count, 2000.0);
      print_window_stability("ul_channel_estimation", sorted_ce, stability_window_count, 1000.0);
      print_window_stability("ul_equalization_demod", sorted_eqdem, stability_window_count, 3000.0);
      print_window_stability("ul_ldpc_decode", sorted_ldpc, stability_window_count, 500.0);
      print_window_stability("ul_rx_wait", sorted_rx_wait, stability_window_count, 1000.0);
      std::fprintf(stderr,
                   "  read: each window is a K-th of the run's SAMPLES (time order), median/p95 in us, and the "
                   "last column is the widest deviation of a window from the whole-run value\n");
    }

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
    // The ONE sample the distribution above does not have, reported rather than dropped: the first receive() of
    // the run spans the radio's stream start (ru_controller_sdr_impl starts it 100 ms ahead by design), so it
    // measures that start-up offset and not the link. It is named here so a reader can account for the counts of
    // the two lines, and so that "the max is a constant ~101 ms on every leg" cannot come back unnoticed.
    if (!std::isnan(startup_rx_wait_us)) {
      std::fprintf(stderr,
                   "[ul_rx_wait] startup=%.1fus excluded (1 sample: the first receive() of the run spans the "
                   "radio's stream start; see dev doc 6.146)\n",
                   startup_rx_wait_us);
    }
    // The same wait, but only for the slots a hop was recorded on: the population the hop series use, so the
    // decomposition "wait for this hop's samples + the span of this hop" adds up inside one population.
    print_series("ul_rx_wait_hop", sorted_rx_wait_hop);
    print_series("ul_dft_wait", sorted_dft_wait);
    print_timing_events();
    // THE SAME-LEG TEST of the receive tail (dev doc 6.150 (6), the M2 hypothesis). Across arms the stalls appear
    // exactly where the host BLOCKS on a Metal completion (0.000-0.001% where it does not, 0.06-0.11% where it
    // does, at the same traffic and with the same device grid present in both). What a cross-arm correlation
    // cannot say is CAUSALITY, and this line is the test: if the blocking is the cause, the slow receives should
    // fall inside those windows almost always; if it is a coincidence, they should overlap at the rate the
    // windows' duty cycle predicts (measured on the blocking arms: ~4%).
    {
      // Each array has its OWN denominator: the slow rates are out of the slow receives, the "all receives" ones
      // out of every receive the account saw. Mixing them is the same mistake this account already made once (it
      // read 50% for a leg whose only slow receive did overlap), so the two are computed by two lambdas.
      const auto rate = [&](size_t k, const std::array<uint64_t, 3>& c, uint64_t den) {
        return (den != 0) ? (100.0 * static_cast<double>(c[k]) / static_cast<double>(den)) : 0.0;
      };
      const auto duty = [&](size_t k) {
        return (leg_span_ns > 0) ? (100.0 * static_cast<double>(dft_block_total_ns[k]) / leg_span_ns) : 0.0;
      };
      const auto kind_name = [](size_t k) {
        switch (static_cast<dft_window_kind>(k)) {
          case dft_window_kind::wait:
            return "wait";
          case dft_window_kind::commit_to_end:
            return "commit->end";
          case dft_window_kind::gpu:
            return "gpu";
          default:
            return "?";
        }
      };
      // THE LINE TO READ: each overlap rate next to the duty cycle of the window it was measured against.
      // Coincidence predicts rate == duty; that window being the cause predicts rate ~ 100% and duty ~ whatever.
      // The NUMERATORS are printed, not only the rates: a rate alone cannot be checked (or compared between two
      // legs whose receive counts differ), and a reader who wants "how many of the slow ones fell in the GPU span"
      // should not have to reconstruct it from two percentages.
      std::fprintf(stderr, "[ul_rx_wait] DFT-window overlap: %llu slow of %llu receive(s) accounted",
                   static_cast<unsigned long long>(rx_slow_seen),
                   static_cast<unsigned long long>(rx_overlap_seen));
      for (size_t k = 0; k != static_cast<size_t>(dft_window_kind::count); ++k) {
        std::fprintf(stderr,
                     "; %s %llu/%llu=%.0f%% vs duty %.1f%% (%llu win, all %llu/%llu=%.0f%%)",
                     kind_name(k),
                     static_cast<unsigned long long>(rx_overlap_slow[k]),
                     static_cast<unsigned long long>(rx_slow_seen),
                     rate(k, rx_overlap_slow, rx_slow_seen),
                     duty(k),
                     static_cast<unsigned long long>(dft_block_count[k]),
                     static_cast<unsigned long long>(rx_overlap_total[k]),
                     static_cast<unsigned long long>(rx_overlap_seen),
                     rate(k, rx_overlap_total, rx_overlap_seen));
      }
      std::fprintf(stderr, "; gpu-clock offset last %.0fus over %llu sample(s)\n",
                   static_cast<double>(gpu_clock_offset_ns) / 1e3,
                   static_cast<unsigned long long>(gpu_clock_samples));
    }
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

  /// \brief Per-thread, per-slot CPU accounting: the number a Mach time constraint's `computation` has to cover.
  ///
  /// WHY IT EXISTS (dev doc 10.30(8)): P4 declares to the kernel "this thread needs `computation` of CPU every
  /// `period`", and the only honest source for that number is how much CPU the thread actually burns per slot.
  /// Nothing in this probe could produce it. The timing events' `tcpu=` is filled only when the baseline and the
  /// event belong to the SAME thread, and the UL pool steals work - a `ce` window is opened by whichever thread
  /// finished `t2f` and closed by whichever finished `ce` - so on a pool thread that field is structurally `-`
  /// (measured on leg p194, 2026-10-01). The process-wide `cpu=` is not a substitute: over a 1.3-1.8 ms window it
  /// reads 4.4-6.9 ms, because it counts every thread in the process.
  ///
  /// WHAT IT MEASURES: on each boundary call a thread reads its OWN cumulative CPU and, when the slot index it
  /// sees changes, files the delta since its own previous boundary. One sample is therefore "CPU this thread
  /// burned between two consecutive slot changes", and over a leg that is its CPU per slot. It is deliberately
  /// NOT a claim about one hop: a work-stealing thread's slot boundary is the only period it can be held to.
  ///
  /// KEYS: compiled only with OCUDU_FLOW_PROBES, and it takes its reading only when OCUDU_UL_THREAD_CPU is set to
  /// something other than 0. With either key off it reads one environment variable per boundary and returns
  /// before any clock, and the report prints nothing - so a delivery leg stays byte-identical.
  struct thread_cpu_accounting {
    uint64_t thread_id = 0;
    char     name[24]  = {};
    /// Windows closed so far (one per slot change this thread observed).
    uint64_t slots = 0;
    int64_t  sum_ns = 0;
    int64_t  max_ns = 0;
    /// The slot whose window is open, -1 when none, and this thread's CPU at the moment it was opened.
    int64_t open_slot   = -1;
    int64_t open_cpu_ns = -1;
    /// Log2 histogram of the closed windows, in ns: bucket i counts samples in [2^i, 2^(i+1)). It is what makes a
    /// TAIL readable out of a bounded amount of state - the declaration wants a p99.9, and keeping every sample of
    /// every pool thread for a whole leg is not something a hot path may do.
    uint64_t buckets[40] = {};
  };

  /// The knob for the accounting above. Read per call rather than cached, so a test can move it.
  static bool thread_cpu_accounting_enabled()
  {
    const char* env = std::getenv("OCUDU_UL_THREAD_CPU");
    return (env != nullptr) && (env[0] != '\0') && !((env[0] == '0') && (env[1] == '\0'));
  }

  /// Returns this thread's accounting block, registering it on first use.
  ///
  /// The block is allocated and NEVER freed on purpose: it is registered in `thread_cpu_accounts`, which the
  /// shutdown report walks, while a plain thread_local would be destroyed when its thread exits (a test thread, a
  /// respawned worker) and leave the registry pointing at freed memory. The leak is one small block per thread
  /// that ever called this, bounded by the worker count.
  thread_cpu_accounting& this_thread_cpu_accounting()
  {
    static thread_local thread_cpu_accounting* acc = nullptr;
    if (acc == nullptr) {
      acc = new thread_cpu_accounting();
      const thread_sched_snapshot self = this_thread_sched_snapshot();
      acc->thread_id                   = self.thread_id;
      std::snprintf(acc->name, sizeof(acc->name), "%s", this_thread_name());
      std::lock_guard<std::mutex> lock(mutex);
      thread_cpu_accounts.push_back(acc);
    }
    return *acc;
  }

  /// Files the window that ends when the calling thread observes \p slot, given its own reading \p cpu_ns.
  ///
  /// This is the ONE place the windowing rule lives, and both the production boundary and its test hook call it -
  /// see the note on record_thread_cpu_boundary_for_test() for what happened when it was written twice.
  static void file_thread_cpu_boundary(thread_cpu_accounting& acc, uint64_t slot, int64_t cpu_ns)
  {
    if ((acc.open_slot >= 0) && (static_cast<int64_t>(slot) != acc.open_slot) && (cpu_ns >= acc.open_cpu_ns)) {
      file_thread_cpu_window(acc, cpu_ns - acc.open_cpu_ns);
    }
    acc.open_slot   = static_cast<int64_t>(slot);
    acc.open_cpu_ns = cpu_ns;
  }

  /// Files one closed window. Only the OWNING thread ever writes a block, so no lock is needed here; the registry
  /// itself is what `mutex` protects, and it is touched once per thread (at registration).
  static void file_thread_cpu_window(thread_cpu_accounting& acc, int64_t cpu_ns)
  {
    ++acc.slots;
    acc.sum_ns += cpu_ns;
    if (cpu_ns > acc.max_ns) {
      acc.max_ns = cpu_ns;
    }
    unsigned  bucket = 0;
    for (int64_t v = cpu_ns >> 1; (v != 0) && (bucket + 1 < 40); v >>= 1) {
      ++bucket;
    }
    ++acc.buckets[bucket];
  }

  /// \brief The smallest value that covers \p quantile of the filed windows, from the histogram (-1 if empty).
  static int64_t thread_cpu_quantile_ns(const thread_cpu_accounting& acc, double quantile)
  {
    if (acc.slots == 0) {
      return -1;
    }
    const uint64_t target = static_cast<uint64_t>(quantile * static_cast<double>(acc.slots) + 0.999999);
    uint64_t       seen   = 0;
    for (unsigned i = 0; i != 40; ++i) {
      seen += acc.buckets[i];
      if (seen >= target) {
        // The bucket's upper edge, in ns: bucket i holds [2^i, 2^(i+1)), so the edge is 2^(i+1) - 1.
        return (i >= 62) ? acc.max_ns : ((static_cast<int64_t>(1) << (i + 1)) - 1);
      }
    }
    return acc.max_ns;
  }

  /// Registry entry: start timestamp plus a monotonic insertion sequence (the slot key wraps every SFN cycle,
  /// so it cannot serve as the age order for the bounded-registry eviction).
  ///
  /// \note `base` is the PROCESS+THREAD snapshot taken at the instant this landmark was recorded, and it exists
  ///       for the PHASE tail events only (dev doc 10.15 (3)): a phase window's baseline has to PRECEDE that
  ///       window's start, and the only baseline a probe could otherwise reach is the receive path's - refreshed
  ///       on a ~1 ms cadence, hence usually INSIDE a 0.5-3.7 ms phase window, which is why every phase event
  ///       printed `cpu=- tcpu=-` on the first two radio legs. A landmark IS the start of the next window
  ///       (t2f_end starts `ce`, ce_end starts `eqdem`, ldpc_start starts `ldpc`), so the snapshot rides with the
  ///       entry the landmark already creates and no extra registry is needed. It stays invalid when the knob is
  ///       off: taking it costs a getrusage plus a Mach call, so it is behind the same gate as the list itself.
  struct start_entry {
    std::chrono::time_point<std::chrono::high_resolution_clock> tp;
    uint64_t                                                     seq;
    cpu_snapshot                                                 base;
  };

  using start_registry = std::map<uint64_t, start_entry>;

  /// Registry entry: the three phase-segment durations of one PUSCH, the assembly timestamp (for the staleness
  /// gate) and the same insertion sequence as above. The three instants are the segment BOUNDARIES on the steady
  /// clock (P1): they are what lets the phase-tail events quote a window instead of only a duration, and they
  /// cannot be recovered later because the landmark maps have moved on by the time the completion is recorded.
  struct phases_entry {
    std::chrono::time_point<std::chrono::high_resolution_clock> tp;
    int64_t  t2f_ns;
    int64_t  ce_ns;
    int64_t  eqdem_ns;
    int64_t  t2f_begin_ns; ///< the slot's first samples arrived (record_start).
    int64_t  t2f_end_ns;   ///< the FFT of the whole slot completed (record_t2f_end).
    int64_t  ce_end_ns;    ///< the channel estimates were ready (record_ce_end).
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

  /// How many completed-but-not-yet-consumed slots [ul_rx_wait_hop]'s registry may hold (see
  /// record_slot_rx_wait). An entry is normally consumed within one hop (~1.5 ms), so this only bounds the case
  /// where hops stop being recorded - an idle stretch, where the oldest entries are the safe ones to drop.
  static constexpr size_t max_slot_rx_wait = 512;

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

  /// A registry instant, expressed on the SAME clock the receive-side events are stamped with (steady nanoseconds).
  ///
  /// The probe keeps its windows in two forms on purpose: the registries use \c high_resolution_clock time_points
  /// (they subtract them, and duration arithmetic is clearer that way), while an event carries plain nanoseconds
  /// because it has to be comparable with the receive path's `begin_ns`/`end_ns` and with the `tcpu` snapshot's
  /// instant. One conversion helper keeps the two from drifting into different epochs.
  ///
  /// \note The rebasing is not cosmetic: `high_resolution_clock` IS `steady_clock` on libc++ (macOS) but is an
  ///       alias of `system_clock` on libstdc++ (Linux), whose epoch is unrelated to the steady clock's. Printing
  ///       a registry instant raw would therefore put the phase events on a different axis from the receive events
  ///       on Linux and on the same axis on macOS - a platform difference inside a reading, which is exactly what
  ///       the Linux-unchanged invariant forbids. The residual error is the interval between the two `now()`
  ///       reads (~tens of nanoseconds), and it is documented rather than chased.
  static int64_t to_ns(const std::chrono::high_resolution_clock::time_point& tp)
  {
    const auto hr_now = std::chrono::high_resolution_clock::now();
    const auto st_now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(st_now.time_since_epoch()).count() +
           std::chrono::duration_cast<std::chrono::nanoseconds>(tp - hr_now).count();
  }

  /// \brief The start instant to stamp on an event that carries \p base: the BASELINE'S OWN, when there is one.
  ///
  /// Not a cosmetic choice. `to_ns()` re-reads both clocks on every call, so two conversions of the SAME
  /// time_point can differ by nanoseconds in either direction - and the rule that refuses a baseline which does
  /// not precede its window (`base.ns > begin_ns`) would then fire on a baseline that was taken AT the window's
  /// start, printing `cpu=-` for exactly the phase events this mechanism was added to give a reading to.
  /// MEASURED by the unit case written for it (`phase_events_carry_the_baseline_of_their_own_window`): the first
  /// version stamped `to_ns(landmark.tp)` beside a baseline taken at the same landmark, and every `ce` line still
  /// read `cpu=-`. Using the baseline's instant for both makes the comparison exact, and keeps the window's own
  /// duration in the time_points where it was always measured.
  static int64_t window_start_ns(const cpu_snapshot&                                  base,
                                 const std::chrono::high_resolution_clock::time_point& tp)
  {
    return base.valid ? base.ns : to_ns(tp);
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
  /// The per-thread CPU accounting blocks (see thread_cpu_accounting). Guarded by `mutex` for the registry
  /// itself, which is only touched when a thread registers; each block is written by its owning thread alone.
  std::vector<thread_cpu_accounting*> thread_cpu_accounts;
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
  /// The ONE receive wait of the run that spans the radio's stream start (µs; NaN = not seen, see
  /// record_rx_wait). Reported as its own field instead of entering rx_wait_us: it is the radio's start-up
  /// offset, it appears once per run, and it used to be the series' `max` on every leg.
  double rx_wait_startup_us = std::numeric_limits<double>::quiet_NaN();
  /// Receive waits of the blocks that completed a slot, keyed by slot, awaiting the hop that will consume them
  /// (see record_slot_rx_wait / record_ldpc_start). Bounded by insertion order: std::map is ordered by slot, and
  /// with max_slot_rx_wait entries the oldest key is the safe one to drop.
  std::map<uint64_t, double> slot_rx_wait_us;
  /// The hop-scoped receive waits (µs), one per recorded hop (see record_slot_rx_wait): the same population the
  /// hop series use, so the wait can be read next to the span it is part of.
  std::vector<double> rx_wait_hop_us;
  /// Host time blocked inside the front-end DFT's wait_slot() (µs), one sample per WAIT (see record_dft_wait).
  std::vector<double> dft_wait_us;

  /// \brief One [cmd_buf waitUntilCompleted] window the host spent blocked on a Metal completion.
  ///
  /// WHY THE TIMESTAMPS EXIST (dev doc 6.150 (6), the M2 test). Across arms the receive stalls appear in exactly
  /// those configurations where this wait BLOCKS - median 0.1 us with a host-resident grid (the call returns at
  /// once), 384 us with a device-resident one - and nowhere else: the device grid alone (p97, and p106/p107 with a
  /// device-side consumer and no block) reads 0.000-0.001% while the blocking arms read 0.06-0.11%. A cross-arm
  /// correlation is not a cause, so the question became same-leg: does a slow receive happen INSIDE one of these
  /// windows, or merely as often as their duty cycle predicts? The windows (a short ring) and the duration total
  /// are what answer it, and the duty cycle is the null hypothesis's prediction - ~4% on the blocking arms.
  struct dft_block_window {
    int64_t begin_ns = 0;
    int64_t end_ns   = 0;
  };
  /// WHICH WINDOW of a submission the timestamps describe (dev doc 6.150 (6), the M2b step).
  ///
  /// M2 asked whether a slow receive falls inside the host's waitUntilCompleted and answered NO: 7% of the slow
  /// receives overlapped it, exactly like 7% of ALL receives (duty cycle 4%) - no enrichment. But that test could
  /// only see the WAIT, which is neither the driver's work at COMMIT time nor the cb's GPU EXECUTION, and those are
  /// the two remaining candidates: a stall inside the GPU span is memory/device contention, one inside the commit
  /// span is driver submission work, and one in neither means the two share an upstream cause instead.
  enum class dft_window_kind : unsigned { wait = 0, commit_to_end = 1, gpu = 2, count = 3 };
  static constexpr size_t max_dft_block_windows = 16;

  std::array<std::array<dft_block_window, max_dft_block_windows>, static_cast<size_t>(dft_window_kind::count)>
      dft_block_windows{};
  std::array<size_t, static_cast<size_t>(dft_window_kind::count)>   dft_block_next{};
  std::array<uint64_t, static_cast<size_t>(dft_window_kind::count)> dft_block_count{};
  std::array<uint64_t, static_cast<size_t>(dft_window_kind::count)> dft_block_total_ns{};
  /// The last (host at wait end) - GPUEndTime offset. It exists to VALIDATE the comparison instead of assuming it:
  /// the GPU timestamps share the host steady clock's epoch (ocudu_metal_burst.mm already computes
  /// `now - cb.GPUEndTime`), and this number is what says so on a leg - small and positive means the window is
  /// placed correctly, a huge value would mean the two clocks disagree and the gpu column is meaningless.
  int64_t  gpu_clock_offset_ns = 0;
  uint64_t gpu_clock_samples   = 0;
  /// Receive calls the account could be applied to at all, then those whose window overlapped one of the
  /// submission's windows, per kind, and of those the slow (>1 ms) ones.
  uint64_t rx_overlap_seen = 0;
  std::array<uint64_t, static_cast<size_t>(dft_window_kind::count)> rx_overlap_total{};
  std::array<uint64_t, static_cast<size_t>(dft_window_kind::count)> rx_overlap_slow{};
  /// Every slow receive, whether it overlapped or not: without this denominator the ratio above says nothing
  /// (measured while writing this account: printing rx_overlap_slow over rx_overlap_seen gave "1 (50%)" for a leg
  /// where the only slow receive WAS the one that overlapped - the 50% was the fast receive next to it).
  uint64_t rx_slow_seen = 0;
  /// One slow receive for the count above: the threshold is the same 1 ms the timing series counts over.
  static constexpr int64_t rx_slow_ns = 1000000;
  /// Leg span for the duty cycle: first -> last receive window seen.
  int64_t leg_first_ns = 0;
  int64_t leg_last_ns  = 0;
  int64_t leg_span_ns  = 0;
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
  /// Landmarks dropped because their slot's base was refreshed by a later SFN cycle (see
  /// record_slot_samples_complete) - i.e. how many stale instants the rebase kept out of the rows.
  uint64_t slot_trace_rebased = 0;
  /// Landmarks refused for being OLDER than their row's base. Must stay 0 (the rebase above is what makes it
  /// so); it is counted and printed rather than silently mapped to NaN, because a negative span that comes back
  /// must be visible in the report before anyone reads a row.
  uint64_t slot_trace_negative = 0;
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
  void record_rx_wait(int64_t /*wait_ns*/, bool /*spans_stream_start*/ = false, int64_t /*begin_ns*/ = 0,
                      int64_t /*end_ns*/ = 0, int64_t /*air_us*/ = 0, int64_t /*load1_x100*/ = -1)
  {
  }
  /// The worst-K timing-event list is part of the probe, so its interface must exist in BOTH arms with the same
  /// shape: with the probes compiled out the instrument is off (limit 0, nothing wanted), which is also what the
  /// `#if defined(OCUDU_FLOW_PROBES)` guard at the call sites relies on.
  static unsigned timing_events_limit() { return 0; }
  static bool     timing_event_wanted_rx(int64_t /*wait_ns*/) { return false; }
  static bool     timing_event_wanted_tx(int64_t /*margin_us*/) { return false; }
  bool            timing_event_snapshot_wanted(int64_t /*now_ns*/) const { return false; }
  void            timing_event_snapshot(int64_t /*now_ns*/) {}
  void record_tx_timing_event(int64_t /*margin_us*/, uint64_t /*due_ts*/, int64_t /*end_ns*/ = 0,
                              int64_t /*load1_x100*/ = -1)
  {
  }
  void record_slot_rx_wait(uint64_t /*slot*/, int64_t /*wait_ns*/, bool /*spans_stream_start*/ = false) {}
  void record_dft_wait(int64_t /*wait_ns*/,
                       int64_t /*begin_ns*/        = 0,
                       int64_t /*end_ns*/          = 0,
                       int64_t /*commit_begin_ns*/ = 0,
                       int64_t /*gpu_begin_ns*/    = 0,
                       int64_t /*gpu_end_ns*/      = 0,
                       int64_t /*gpu_offset_ns*/   = 0)
  {
  }
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

/// \brief The pipeline probe's report as a plain function, so it can join the on-demand P0 dump (dev doc 6.24).
inline void report_ul_pipeline_probe()
{
  ul_pipeline_probe::get().report();
}

} // namespace ocudu
