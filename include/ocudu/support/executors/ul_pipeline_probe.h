// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
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
class ul_pipeline_probe
{
public:
  static ul_pipeline_probe& get()
  {
    static ul_pipeline_probe instance;
    return instance;
  }

  /// Records the start of the UL processing of a slot (call from the lower PHY baseband processor).
  /// \param[in] slot Slot number (SFN-referenced slot count, matching the FAPI slot indications).
  void record_start(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending_starts[slot] = {std::chrono::high_resolution_clock::now(), next_start_seq++};
    // Bound the registry by INSERTION ORDER (the slot count wraps every SFN cycle, so the key order is not a
    // valid age order): unmatched entries belong to idle slots (no PUSCH), drop the oldest insertion.
    evict_oldest(pending_starts);
  }

  /// Records the start of the LDPC decoder (call right before the first codeblock decode of a transport block).
  /// \param[in] slot Slot number of the PUSCH (same reference as record_end_crc_ok).
  void record_ldpc_start(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::high_resolution_clock::now();
    pending_ldpc_starts[slot] = {now, next_start_seq++};
    // Bound the registry by insertion order (see record_start): unmatched entries belong to TBs that ended
    // without a CRC-OK completion (or with one in a shifted slot).
    evict_oldest(pending_ldpc_starts);

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
    std::lock_guard<std::mutex> lock(mutex);
    pending_t2f_ends[slot] = {std::chrono::high_resolution_clock::now(), next_start_seq++};
    evict_oldest(pending_t2f_ends);
  }

  /// Records the completion of the PUSCH channel estimation: the channel estimates of all the data symbols of
  /// the slot are ready (call from the PUSCH processor at the start of the data processing).
  /// \param[in] slot Slot number of the PUSCH (same reference as record_end_crc_ok).
  void record_ce_end(uint64_t slot)
  {
    std::lock_guard<std::mutex> lock(mutex);
    pending_ce_ends[slot] = {std::chrono::high_resolution_clock::now(), next_start_seq++};
    evict_oldest(pending_ce_ends);
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

    std::lock_guard<std::mutex> lock(mutex);
    // CRC-OK completion timestamp for the FAPI->MAC tail-latency series (see record_fapi_mac_end): recorded
    // unconditionally, this method is only ever called for CRC-OK TBs.
    pending_crc_ok_ends[slot] = {now, next_start_seq++};
    evict_oldest(pending_crc_ok_ends);
    auto it = find_fresh(pending_starts, slot, now);
    if (it != pending_starts.end()) {
      double latency_us =
          static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.tp).count());
      pending_starts.erase(it);
      latencies_us.push_back(latency_us);
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
      // [ul_ldpc_decode].
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
        pending_phases.erase(phases_it);
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

  /// Prints the statistics of the recorded latencies. Called once during the application shutdown.
  void report()
  {
    std::vector<double> sorted_pipeline;
    std::vector<double> sorted_ldpc;
    std::vector<double> sorted_pdu_sizes;
    std::vector<double> sorted_t2f;
    std::vector<double> sorted_ce;
    std::vector<double> sorted_eqdem;
    std::vector<double> sorted_fapi_mac;
    {
      std::lock_guard<std::mutex> lock(mutex);
      sorted_pipeline  = latencies_us;
      sorted_ldpc      = ldpc_latencies_us;
      sorted_pdu_sizes = mac_pdu_sizes_bytes;
      sorted_t2f       = t2f_latencies_us;
      sorted_ce        = ce_latencies_us;
      sorted_eqdem     = eqdem_latencies_us;
      sorted_fapi_mac  = fapi_mac_latencies_us;
    }
    if (sorted_pipeline.empty()) {
      std::fprintf(stderr, "[ul_pipeline] no CRC-OK samples recorded\n");
      return;
    }
    std::sort(sorted_pipeline.begin(), sorted_pipeline.end());
    double sum = 0;
    for (double v : sorted_pipeline) {
      sum += v;
    }
    auto pct = [](const std::vector<double>& sorted, double p) {
      return sorted[static_cast<size_t>((sorted.size() - 1) * p)];
    };
    // Report to stderr (guaranteed to be visible at the shutdown, unlike the logging backend) and to the logs.
    std::fprintf(stderr,
                 "[ul_pipeline] samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
                 sorted_pipeline.size(),
                 sum / static_cast<double>(sorted_pipeline.size()),
                 pct(sorted_pipeline, 0.5),
                 sorted_pipeline.front(),
                 sorted_pipeline.back(),
                 pct(sorted_pipeline, 0.95),
                 pct(sorted_pipeline, 0.99));

    // Phase-segment series, printed in pipeline order. Recorded in lockstep with the [ul_ldpc_decode] series
    // (CRC-OK completions only), so their sample counts always match it.
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
    print_series("ul_time_frequency", sorted_t2f);
    print_series("ul_channel_estimation", sorted_ce);
    print_series("ul_equalization_demod", sorted_eqdem);
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
  /// FAPI->MAC tail latencies of the CRC-OK completions (µs): CRC-OK -> MAC UL task enqueue.
  std::vector<double> fapi_mac_latencies_us;
};

#else // not OCUDU_FLOW_PROBES: no-op implementation with zero overhead.

class ul_pipeline_probe
{
public:
  static ul_pipeline_probe& get()
  {
    static ul_pipeline_probe instance;
    return instance;
  }
  void record_start(uint64_t /*slot*/) {}
  void record_ldpc_start(uint64_t /*slot*/) {}
  void record_t2f_end(uint64_t /*slot*/) {}
  void record_ce_end(uint64_t /*slot*/) {}
  void record_end_crc_ok(uint64_t /*slot*/, size_t /*mac_pdu_bytes*/) {}
  void record_fapi_mac_end(uint64_t /*slot*/) {}
  std::optional<ul_phase_durations> get_phase_durations(uint64_t /*slot*/) { return std::nullopt; }
  void report() {}

private:
  ul_pipeline_probe() = default;
};

#endif

} // namespace ocudu
