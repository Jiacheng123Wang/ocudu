// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ocudu {

/// \brief How many PUSCH hops each receiving slot carries - the count that decides whether open item #1
///        ("one grid serves the K hops of a slot") is worth doing.
///
/// ---- Why this exists ----
/// The hand-over's registry hands a slot's grid to ONE hop (design document 5.9.40), so a slot with K
/// PUSCH hops costs K times what a single-hop slot costs, and merging them needs a change to the
/// receiving chain rather than the four lines 5.9.43 proposed (5.9.44). Whether that is worth doing is a
/// TRAFFIC question, and nothing measured it: every counter on the lane counts HOPS - `[ul_gpu_lane]
/// lanes`, `[metal_stats] burst commits`, `[mmse_time_sum] calls` - so hops per slot, the quantity #1
/// would remove, was invisible. The design document's rule for exactly this situation is the one item #9
/// already carries ("whether a PRACH-only slot does a wasted FFT: COUNT IT FIRST, then decide"), and
/// this is that count for item #1.
///
/// ---- What counts, and where the slot aggregation lives ----
/// One call per PUSCH hop, from the place that publishes the slot's hop plan
/// (slot_hop_plan_hook::set(), uplink_processor_impl). The slot's running hop count is kept by the
/// CALLER - uplink_processor_impl, per instance - because that is the only place that knows the hops
/// belong to one slot: the PUSCH PDUs of a slot are published in as many calls as they have distinct
/// LAST SYMBOLS (process_symbol_pdus() runs per end symbol), so counting `pusch_pdus.size()` at a single
/// call site reports a two-hop slot as two one-hop slots. It would understate, by construction, the
/// exact case this exists to find.
///
/// ---- Why the bucket moves per hop instead of being written once ----
/// A slot's hop count is only final when its LAST hop arrives, so "record it when the slot ends" is both
/// late (the run's final slot would need a thread-exit destructor, whose ordering against the at-exit
/// report is not guaranteed on every platform) and invisible while the run is going. This one keeps the
/// histogram correct after EVERY hop: the slot enters bucket 1 on its first hop and moves up one bucket
/// per further hop, so counts() is a true reading at any instant.
///
/// \note Each slot is fed by exactly one instance holding the state machine's handle_rx_symbol lock, so
///       the bucket transitions of different slots never interleave on the same slot. A hop moves its
///       slot between two buckets with two atomics, so a reader concurrent with a hop could see that one
///       slot in neither bucket for that instant - the report runs at exit, when no hop is in flight.
///
/// \note This is a MEASUREMENT and changes no behaviour: it sits next to the hop-plan hook that already
///       runs, and it is compiled into every build (the counters are plain atomics).
class ul_slot_hop_counts
{
public:
  /// Hops-per-slot counted exactly; a slot with more lands in the "or more" bucket.
  static constexpr unsigned kMaxTracked = 4;

  /// \brief The \p hops_in_slot -th hop of a slot is about to run (1-based).
  ///
  /// \param[in] hops_in_slot How many hops this slot has now, counting this one. It is the caller's
  ///            running count, which is what makes a slot's hops aggregate into ONE slot.
  static void note(uint64_t hops_in_slot)
  {
    install_report();
    total_hops().fetch_add(1, std::memory_order_relaxed);
    if (hops_in_slot <= 1) {
      total_slots().fetch_add(1, std::memory_order_relaxed);
      bucket(1).fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // This slot was counted in the bucket for (hops_in_slot - 1) hops; move it to the one for
    // hops_in_slot. Above kMaxTracked both indices are the "or more" bucket, so the move is a no-op and
    // the slot stays counted exactly once.
    bucket(bucket_index(hops_in_slot - 1)).fetch_sub(1, std::memory_order_relaxed);
    bucket(bucket_index(hops_in_slot)).fetch_add(1, std::memory_order_relaxed);
  }

  struct counts_t {
    /// Slots that carried at least one PUSCH hop.
    uint64_t slots = 0;
    /// PUSCH hops over those slots.
    uint64_t hops = 0;
    /// `by_hops[k]` for k = 1..kMaxTracked: slots with exactly k hops.
    /// `by_hops[0]`: slots with MORE than kMaxTracked hops.
    uint64_t by_hops[kMaxTracked + 1] = {};

    /// Slots with more than one hop: the ones item #1 would collapse. Derived from the buckets so the
    /// fraction printed next to the histogram cannot disagree with it.
    uint64_t multi_hop_slots() const
    {
      uint64_t multi = by_hops[0];
      for (unsigned k = 2; k <= kMaxTracked; ++k) {
        multi += by_hops[k];
      }
      return multi;
    }
  };

  static counts_t counts()
  {
    counts_t c;
    c.slots = total_slots().load(std::memory_order_relaxed);
    c.hops  = total_hops().load(std::memory_order_relaxed);
    for (unsigned k = 0; k != kMaxTracked + 1; ++k) {
      c.by_hops[k] = bucket(k).load(std::memory_order_relaxed);
    }
    return c;
  }

  /// \brief The at-exit report: the histogram, and the two numbers the decision turns on.
  ///
  /// `multi-hop slots` is the fraction of PUSCH-carrying slots that carry MORE THAN ONE hop - the
  /// fraction of slots item #1 would make cheaper. A SMALL fraction is a finding that says "do not
  /// restructure the receiving chain for this", which is a result and not a failure to report.
  static void print_report(std::FILE* out = stderr)
  {
    const counts_t c = counts();
    if (c.slots == 0) {
      // Nothing to say, and saying it anyway would put a line in every run that has no uplink.
      return;
    }
    const double   mean  = static_cast<double>(c.hops) / static_cast<double>(c.slots);
    const uint64_t multi = c.multi_hop_slots();
    std::fprintf(out, "[phy_pipeline] ul slots by PUSCH hops:");
    for (unsigned k = 1; k <= kMaxTracked; ++k) {
      std::fprintf(out, " %u:%llu", k, static_cast<unsigned long long>(c.by_hops[k]));
    }
    std::fprintf(out,
                 " >%u:%llu  (pusch slots=%llu hops=%llu mean=%.2f hops/slot, multi-hop slots=%llu = %.1f%%)\n",
                 kMaxTracked,
                 static_cast<unsigned long long>(c.by_hops[0]),
                 static_cast<unsigned long long>(c.slots),
                 static_cast<unsigned long long>(c.hops),
                 mean,
                 static_cast<unsigned long long>(multi),
                 (100.0 * static_cast<double>(multi)) / static_cast<double>(c.slots));
  }

private:
  static unsigned bucket_index(uint64_t hops)
  {
    return (hops > kMaxTracked) ? 0U : static_cast<unsigned>(hops);
  }

  /// Registers the at-exit report once, on the first hop (idempotent).
  static void install_report()
  {
    static const bool registered = []() {
      std::atexit(report_at_exit);
      return true;
    }();
    (void)registered;
  }

  static void report_at_exit()
  {
    print_report(stderr);
  }

  // Heap-allocated and never destroyed, for the reason phy_pipeline_crossings gives: the report runs
  // from an atexit handler, which runs after the static destructors of this translation unit.
  static std::atomic<uint64_t>& total_slots()
  {
    static std::atomic<uint64_t>* p = new std::atomic<uint64_t>(0);
    return *p;
  }
  static std::atomic<uint64_t>& total_hops()
  {
    static std::atomic<uint64_t>* p = new std::atomic<uint64_t>(0);
    return *p;
  }
  static std::atomic<uint64_t>& bucket(unsigned k)
  {
    static std::atomic<uint64_t>* p = new std::atomic<uint64_t>[kMaxTracked + 1]();
    return p[k];
  }
};

} // namespace ocudu
