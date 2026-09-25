// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_dft_metal_engine.h"
#include "ocudu_metal_lane_probe.h"

#include "ocudu_metal_burst.h"
#include "ocudu_metal_queue.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu/phy/phy_pipeline_report.h"
#include "ocudu/phy/phy_pipeline_crossings.h"
#include "ocudu/phy/phy_pipeline_grid_ready.h"

#include "ocudu/support/executors/ul_pipeline_probe.h"
#include "ocudu/support/macos_compat.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef OCUDU_DFT_METALLIB_PATH
#define OCUDU_DFT_METALLIB_PATH "ocudu_dft.metallib"
#endif

using namespace ocudu;

/// \brief P0-2's clock: the same one the lane probe and the estimator's lane clock read
/// (`std::chrono::steady_clock`, see ocudu_metal_lane_clock.h), named here so a token's hold is comparable
/// with the [ul_gpu_lane] series without this translation unit taking a dependency on that header.
using hold_clock = std::chrono::steady_clock;

namespace ocudu {
namespace metal {

namespace {

// ---- Process-wide dispatch/wait statistics (same accounting as the LDPC/MMSE engines) ----
// Compile-time debug aid (ENABLE_METAL_STATS=ON defines OCUDU_METAL_STATS); off by default
// with zero overhead. Reported at process exit.
#if defined(OCUDU_METAL_STATS)
struct dft_stats_t {
  std::atomic<uint64_t> commits{0};
  /// Transforms encoded. Equal to commits while every transform gets its own command buffer; larger
  /// once a block of them shares one (see begin_block()), which is why the contract check counts
  /// transforms and not commits.
  std::atomic<uint64_t> transforms{0};
  std::atomic<uint64_t> waits{0};
  /// Peak PIPELINE DEPTH: how many of the batch slots held an un-waited transform at once. Tracked by
  /// dft_stats_note_depth() from the engine's own slot_pending[] flags - never from commits minus waits,
  /// which grows without bound now that the wait policy does not wait for every transform.
  std::atomic<uint64_t> in_flight_max{0};
  /// Transforms whose input came straight from the radio's int16 buffer instead of the engine's
  /// float2 ring (see grid_write::time_samples). Zero means every transform is staging its input on
  /// the host - either the caller never asks for it, or the engine refused the samples and the
  /// caller fell back, which it warns about once.
  std::atomic<uint64_t> radio_inputs{0};
  /// Wraps of this engine that could not be zero-copy and were staged instead (see wrap_buffer): the
  /// engine's own tables when they are not page aligned, or a caller's buffer the registry does not
  /// describe. Counted because a silent copy here is exactly how "the transform reads the radio
  /// buffer" stops being true without any counter saying so.
  std::atomic<uint64_t> wrap_copies{0};
  /// WHY a plain-route transform got a command buffer of its own (5.9.113). The plain route (`submit_at`)
  /// either joins an OPEN BLOCK - one command buffer per slot, which is the batching the block API exists
  /// for - or commits a buffer carrying just its own transform. Measuring which of the two happens, and
  /// whether the engine had even been told a slot, is what separates "nobody told the front end which slot
  /// these samples belong to" from "a block was open and the transform did not join it": two different
  /// defects with two different fixes, and no way to tell them apart from the commit count alone.
  std::atomic<uint64_t> plain_with_block{0};
  std::atomic<uint64_t> plain_without_block{0};
  /// Of the above, the ones submitted before any slot was ever told (see set_lane_slot()).
  ///
  /// MEASURED, and it settles an item that was open for two sessions (design document 5.9.99 -> 5.9.111 ->
  /// 5.9.119, leg s69-a12-n78 on the n78 cell): the plain route on a TDD cell is populated ENTIRELY by the
  /// PRACH demodulator's OWN engine instances, which are never told a lane slot, plus the one process-wide
  /// warm-up run() - `plain_without_block == plain_without_lane_slot == 12 x (radio frames) + 1`, the 12
  /// being PRACH format B4 (index 159: one occasion per 10 ms radio frame, 12 symbols each; the demodulator
  /// calls run() once per symbol). The puxch instance is told its slot BEFORE its first submit, so not one of
  /// its transforms can land in this bucket - and none does: every one of them rides the slot-grid-write
  /// route (`radio_inputs == handed x 14` on that leg, including the slots no hop ever claimed).
  ///
  /// So a large value here is NOT the coverage gap 5.9.99 once suspected, and it is not a defect to fix by
  /// "letting the hop-less UL slots into the block path": they are already there. Read it as "another engine
  /// instance is doing synchronous transforms by design" - and only a SMALL plain_without_lane_slot next to a
  /// large plain_without_block would mean a slotted front end failed to open its block.
  std::atomic<uint64_t> plain_without_lane_slot{0};
  /// Blocks HANDED OVER instead of committed (see release_block()). Zero on every run that does not arm
  /// OCUDU_DFT_RELEASE_BLOCK, which is what makes "the factory path never takes this route" a counter and
  /// not a reading of the code.
  std::atomic<uint64_t> released{0};
  /// wait_slot() calls that named a slot whose transform went out with a released block. A correctly
  /// wired release run reads 0 here: the host wait it removes is the point of the change, so a non-zero
  /// value is a wiring defect (the wait was skipped, and the caller's data may not be there yet).
  std::atomic<uint64_t> released_waits{0};
  /// Tokens the receiving chain attached to a block so its INPUT outlives the dispatches that read it, and
  /// how many of them have been released (see dft_metal_engine::retain_for_block()).
  ///
  /// READING RULE (measured 2026-09-23, milestone audit 5.9.120): `attached - released` is the number of
  /// tokens held by blocks STILL IN FLIGHT, not a leak - they are released by the adopting command
  /// buffer's completion handler, which runs on the GPU's clock, so any single reading of the pair is a
  /// snapshot. On the one leg whose build printed the pair every hop (s47-d1default, 884 prints) the gap
  /// OSCILLATES between 0 and 84 tokens (histogram 14:375, 28:114, 42:57, 56:292, 70:41, 84:4), with the
  /// same maximum in the first half of the run as in the second - a leak would grow monotonically with
  /// the hop count instead. So "the two must end equal" (this comment used to say exactly that) holds
  /// only when the shutdown happened to drain the pipeline: s62/s64b end 384846/384846 and 301658/301658
  /// (equal, nothing in flight), while s69 ends 368186/368200 (one slot in flight). The distinguishing
  /// number is therefore the MAXIMUM GAP over the run - printed next to the pair - and not the pair.
  std::atomic<uint64_t> keepalives{0};
  std::atomic<uint64_t> keepalives_released{0};
  /// Largest `keepalives - keepalives_released` seen at any attach: the in-flight high-water mark. A leak
  /// makes the gap grow without bound, so this is what tells "in flight" from "leaked" (see above).
  std::atomic<uint64_t> keepalives_in_flight_max{0};
  /// \name P2-E: WHEN the input tokens come back.
  ///
  /// The pair above says whether they come back; these three say from WHICH end of the command buffer, which
  /// is the whole of P2-E: with `OCUDU_DFT_RELEASE_TOKENS_EARLY=1` the block signals a shared event right
  /// after its last input-reading dispatch, and the tokens are released by that event instead of by the
  /// command buffer's completion - which on the fused lane is the END OF THE WHOLE HOP (design document
  /// 5.9.129 (2)), i.e. an input held for a span in which only its first dispatch reads it.
  ///
  /// `token_early_signals` counts the signals ENCODED, `token_sets_by_event` the sets the event actually
  /// released, `token_sets_by_complete` the sets a completion handler released instead. The last two make the
  /// mechanism readable on a leg: switch on with `by_event == 0` means the signal was encoded and never
  /// reached the tokens (a wiring defect the counters say out loud, instead of leaving it to the pool numbers
  /// to imply), while `by_event > 0` is the direct evidence that the release really moved to the front end.
  std::atomic<uint64_t> token_early_signals{0};
  std::atomic<uint64_t> token_sets_by_event{0};
  std::atomic<uint64_t> token_sets_by_complete{0};

  /// \name P0-2: HOW LONG the input was held (attach -> release), per token.
  ///
  /// The quantity the receive pool actually feels. One token keeps one SYMBOL's samples (and a whole slot's
  /// buffer is referenced by its 14 tokens, released together), so the hold distribution is what says whether
  /// the pool was drained by "the hop's own span" (the multi-hold the design accepted, ~ms) or by something
  /// that kept a block from completing for SECONDS (the stall Q9 is about).
  ///
  /// Samples are kept (not only a histogram) so the report can print real percentiles in the same shape as
  /// every other series here; a 200 s leg at 1 ms slots attaches ~2.8 M tokens, i.e. ~22 MB, which is the
  /// price of reading the tail by eye instead of guessing it.
  ///@{
  std::mutex           token_hold_mutex;
  std::vector<float>   token_hold_us;
  double               token_hold_sum_us = 0.0;
  double               token_hold_max_us = 0.0;
  uint64_t             token_hold_max_slot = 0;
  bool                 token_hold_max_has_slot = false;
  const void*          token_hold_max_cb = nullptr;
  ///@}
};

static dft_stats_t& dft_stats()
{
  // NEVER DESTROYED ON PURPOSE, and that is now load-bearing: dft_stats_report() is an atexit handler, so it
  // runs AFTER this translation unit's static destructors - and P0-2 put a `std::mutex` (and vectors) in this
  // struct, whose destructor would run first. Measured the moment it was introduced: the metal test aborted at
  // exit with `mutex lock failed: Invalid argument`. The same reasoning, and the same deliberate leak, as the
  // lane probe's stats() (lib/phy/metal/ocudu_metal_lane_probe.mm).
  static dft_stats_t* s = new dft_stats_t();
  return *s;
}

static void dft_stats_note_depth(uint64_t depth)
{
  dft_stats_t& s = dft_stats();
  uint64_t     prev = s.in_flight_max.load(std::memory_order_relaxed);
  while (depth > prev && !s.in_flight_max.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {
  }
}

static void dft_stats_commit(uint64_t nof_transforms)
{
  dft_stats_t& s = dft_stats();
  s.commits.fetch_add(1, std::memory_order_relaxed);
  s.transforms.fetch_add(nof_transforms, std::memory_order_relaxed);
}

static void dft_stats_wait()
{
  dft_stats_t& s = dft_stats();
  s.waits.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one block handed over instead of committed (see release_block()).
static void dft_stats_release()
{
  dft_stats().released.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one wait that named a slot whose transform went out with a released block: the wait was NOT
/// honoured, and the caller has to be told (see release_block()).
static void dft_stats_released_wait()
{
  dft_stats().released_waits.fetch_add(1, std::memory_order_relaxed);
}

/// Counts the tokens attached to a block (see retain_for_block()), and notes how many are in flight.
static void dft_stats_keepalive()
{
  dft_stats_t& s = dft_stats();
  const uint64_t attached = s.keepalives.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t held     = attached - s.keepalives_released.load(std::memory_order_relaxed);
  uint64_t       prev     = s.keepalives_in_flight_max.load(std::memory_order_relaxed);
  while (held > prev && !s.keepalives_in_flight_max.compare_exchange_weak(prev, held, std::memory_order_relaxed)) {
  }
}

/// Counts the tokens released - by the block's completion, or by the drop of a handover nobody claimed.
static void dft_stats_keepalives_released(uint64_t nof)
{
  dft_stats().keepalives_released.fetch_add(nof, std::memory_order_relaxed);
}

/// \brief P0-2: folds one block's token holds (attach -> release) into the statistics.
///
/// \param[in] attached_at When each token of the set was attached (parallel to the set's token list); a set
///            whose times do not line up with its tokens is skipped rather than paired by guess - the two are
///            built together and MUST have the same length.
/// \param[in] slot        Lane slot the block belonged to (0 when the block never named one).
///
/// Called from the release path, which runs on a Metal completion thread or on whoever dropped the block, so
/// the statistics have their own mutex (the release itself stays outside it - see release_block_tokens()).
static void
dft_stats_token_holds(const std::vector<hold_clock::time_point>& attached_at,
                      uint64_t                                                        slot,
                      bool                                                            has_slot)
{
  if (attached_at.empty()) {
    return;
  }
  const auto                                     now = hold_clock::now();
  std::vector<float>                             holds;
  holds.reserve(attached_at.size());
  double                                         sum = 0.0;
  double                                         max = 0.0;
  for (const auto& at : attached_at) {
    const double us = std::chrono::duration<double, std::micro>(now - at).count();
    // A negative hold is impossible (monotonic clock, attach before release): clamped rather than dropped, so
    // a clock mistake shows up as a pile at 0 instead of quietly changing the sample count.
    const float h = static_cast<float>(us < 0.0 ? 0.0 : us);
    holds.push_back(h);
    sum += h;
    max = (h > max) ? h : max;
  }
  dft_stats_t& s = dft_stats();
  std::lock_guard<std::mutex> lock(s.token_hold_mutex);
  s.token_hold_us.insert(s.token_hold_us.end(), holds.begin(), holds.end());
  s.token_hold_sum_us += sum;
  if (max > s.token_hold_max_us) {
    s.token_hold_max_us       = max;
    s.token_hold_max_slot     = slot;
    s.token_hold_max_has_slot = has_slot;
  }
}

/// Counts one early token-release signal ENCODED into a block (P2-E, see release_tokens_early_requested()).
static void dft_stats_token_early_signal()
{
  dft_stats().token_early_signals.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one token set released, and from which end: the front end's event (P2-E) or a command buffer's
/// completion. Counted per SET rather than per token because the question is which of the two paths won the
/// race for a block, and every token of a block travels together.
static void dft_stats_token_set_released(bool by_event)
{
  dft_stats_t& s = dft_stats();
  if (by_event) {
    s.token_sets_by_event.fetch_add(1, std::memory_order_relaxed);
  } else {
    s.token_sets_by_complete.fetch_add(1, std::memory_order_relaxed);
  }
}

/// Counts one transform whose input came straight from the radio's int16 buffer (the zero-copy
/// path). Wrapped like the commit/wait counters so the call site never names the struct: the
/// accessor only exists when the probe is compiled in.
static void dft_stats_radio_input()
{
  dft_stats().radio_inputs.fetch_add(1, std::memory_order_relaxed);
}

/// Counts one plain-route transform by WHY it did or did not get a command buffer of its own (5.9.113).
static void dft_stats_plain_submit(bool with_block, bool with_lane_slot)
{
  dft_stats_t& s = dft_stats();
  if (with_block) {
    s.plain_with_block.fetch_add(1, std::memory_order_relaxed);
  } else {
    s.plain_without_block.fetch_add(1, std::memory_order_relaxed);
    if (!with_lane_slot) {
      s.plain_without_lane_slot.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

/// Counts one wrap that had to stage a copy (see dft_stats_t::wrap_copies).
static void dft_stats_wrap_copy()
{
  dft_stats().wrap_copies.fetch_add(1, std::memory_order_relaxed);
}

static void dft_stats_report()
{
  // NOT const: P0-2's hold statistics live behind a mutex (the release path runs on Metal's completion threads),
  // and a const reference cannot be locked. Nothing else here writes to the statistics.
  dft_stats_t& s = dft_stats();
  std::fprintf(stderr,
               // slots_in_flight is the DEPTH OF THE PIPELINE (how many of the max_pipeline_depth slots
               // hold an un-waited transform), not commits minus waits: the wait policy stopped waiting for
               // every transform (see ofdm_demodulator_impl::finish_symbol()), so that difference grows
               // without bound and would read like a backlog that is not there.
               "[metal_stats] dft commits=%llu transforms=%llu waits=%llu slots_in_flight=%llu radio_inputs=%llu "
               "wrap_copies=%llu released=%llu released_waits=%llu\n",
               static_cast<unsigned long long>(s.commits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.transforms.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.in_flight_max.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.radio_inputs.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.wrap_copies.load(std::memory_order_relaxed)),
               // released / released_waits: the release path of D1 step 1. Both are 0 unless the run armed
               // OCUDU_DFT_RELEASE_BLOCK, and released_waits must stay 0 even then (see the struct).
               static_cast<unsigned long long>(s.released.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(s.released_waits.load(std::memory_order_relaxed)));

  // D1 step 2: the handover's own counters, printed by the ENGINE rather than by the registry's own
  // translation unit: the engine is in every build that can arm the release, so an armed leg always sees
  // the line - and a line with handed=0 is then a finding (the demodulator's guard refused, or no slot
  // reached its last symbol), not an absent instrument. A run that did not arm it and never handed
  // anything over stays silent.
  //
  // superseded and evicted are kept apart on purpose (see handed_counters): the first is the expected
  // "that slot had no hop, and its grid came back through the pool", the second is a backlog.
  const metal::shared_burst::handed_counters   hand = metal::shared_burst::handed_stats();
  const metal::shared_burst::handshake_counters shake = metal::shared_burst::handshake_stats();
  const bool                                   release_armed = grid_handover_armed();
  if (release_armed || (hand.handed != 0) || (hand.taken != 0)) {
    std::fprintf(stderr,
                 "[metal_stats] dft handover handed=%llu taken=%llu superseded=%llu evicted=%llu "
                 "evicted_unproduced=%llu over_bound=%llu unproduced=%zu "
                 "fallback=%llu late=%llu late_time=%llu not_found=%llu timeouts=%llu keepalives=%llu/%llu (max in flight "
                 "%llu) (armed=%d) tokens_early=signals:%llu,by_event:%llu,by_complete:%llu "
                 "handshake=waits:%llu,timeouts:%llu,max:%lluus\n",
                 static_cast<unsigned long long>(hand.handed),
                 static_cast<unsigned long long>(hand.taken),
                 static_cast<unsigned long long>(hand.superseded),
                 static_cast<unsigned long long>(hand.evicted),
                 static_cast<unsigned long long>(hand.evicted_unproduced),
                 static_cast<unsigned long long>(hand.over_bound),
                 hand.unproduced,
                 static_cast<unsigned long long>(hand.fallback_commits),
                 static_cast<unsigned long long>(hand.late_commits),
                 // Q9: the part of `late` the TIME deadline claimed, i.e. what the SLOT window did not cover -
                 // printed next to the total on purpose, because that pair is what says whether the 10 ms
                 // deadline is doing work or the slot rule is still the one reaping (see handed_counters).
                 static_cast<unsigned long long>(hand.late_commits_time),
                 static_cast<unsigned long long>(hand.grid_not_found),
                 static_cast<unsigned long long>(hand.ready_timeouts),
                 // keepalives = released/attached, plus the in-flight HIGH-WATER MARK. The gap between the
                 // two is the number of tokens held by blocks still in flight (they are released by the
                 // adopting buffer's completion handler), NOT a leak: on s47 the pair was printed every hop
                 // and the gap oscillated 0..84 with the same maximum in both halves of the run, while a
                 // leak would grow with the hop count. The high-water mark is what separates the two, and
                 // "equal at exit" only holds when the shutdown happened to drain the pipeline.
                 static_cast<unsigned long long>(s.keepalives_released.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.keepalives.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.keepalives_in_flight_max.load(std::memory_order_relaxed)),
                 static_cast<int>(release_armed),
                 // P2-E: WHEN the input came back. Printed on this line rather than on one of its own so a
                 // reader comparing two legs sees the release site next to the release count it belongs to.
                 static_cast<unsigned long long>(s.token_early_signals.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.token_sets_by_event.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(s.token_sets_by_complete.load(std::memory_order_relaxed)),
                 // Dev doc 6.20: the commit handshake. `waits` > 0 says the Q9-G window IS reached on air (a
                 // consumer was handed a generation whose carrier had not been committed yet, and the wait is
                 // what closed it); `timeouts` must stay 0 (a non-zero value means a consumer had to be
                 // ordered on the HOST because the commit never came).
                 static_cast<unsigned long long>(shake.waits),
                 static_cast<unsigned long long>(shake.timeouts),
                 static_cast<unsigned long long>(shake.wait_max_us));
    // ... and, when the switch was on and the event did NOT win, say what that means, because the numbers
    // above are the only thing that separates "the release moved to the front end" from "the input is still
    // held for the whole hop" - and a reader who takes a leg's pool numbers as evidence either way without
    // this line would be reading a mechanism that never fired (measured on macOS 26.6.2: a signal encoded
    // after an encoder has been created is published only when the command buffer completes).
    // P0-2: how long the input was actually held, and the longest one with the slot it belonged to. Printed
    // with its OWN percentiles (not folded into the handover line) because it is the one reading that says
    // whether the pool was drained by the hop's own span or by a block that would not complete.
    {
      std::vector<float> holds;
      {
        std::lock_guard<std::mutex> lock(s.token_hold_mutex);
        holds = s.token_hold_us;
      }
      if (!holds.empty()) {
        std::sort(holds.begin(), holds.end());
        const auto pct = [&holds](double p) { return holds[static_cast<size_t>((holds.size() - 1) * p)]; };
        const double total = s.token_hold_sum_us;
        std::fprintf(stderr,
                     "[metal_stats] input hold (P0-2): tokens=%zu mean=%.1fus median=%.1fus p95=%.1fus "
                     "p99=%.1fus max=%.1fus at slot=%llu (slot named=%d)\n",
                     holds.size(),
                     total / static_cast<double>(holds.size()),
                     pct(0.5),
                     pct(0.95),
                     pct(0.99),
                     holds.back(),
                     static_cast<unsigned long long>(s.token_hold_max_slot),
                     static_cast<int>(s.token_hold_max_has_slot));
        // A hold of seconds is the stall: say it in words, with the count, so a leg cannot be read as
        // "the hold is the hop's span" when a handful of tokens were held for whole seconds.
        size_t over_100ms = 0;
        size_t over_1s    = 0;
        for (float h : holds) {
          over_100ms += (h > 100000.0F) ? 1U : 0U;
          over_1s += (h > 1000000.0F) ? 1U : 0U;
        }
        std::fprintf(stderr,
                     "[metal_stats] input hold (P0-2) tail: over 100ms=%zu (%.4f%%), over 1s=%zu (%.4f%%) of "
                     "%zu token(s)%s\n",
                     over_100ms,
                     100.0 * static_cast<double>(over_100ms) / static_cast<double>(holds.size()),
                     over_1s,
                     100.0 * static_cast<double>(over_1s) / static_cast<double>(holds.size()),
                     holds.size(),
                     (over_1s != 0) ? " - tokens held for SECONDS: that is the stall, not the hop's span" : "");
      }
    }
    // ---- P0-7: the block's OWN timeline (deposit -> claim -> completion) --------------------------------
    //
    // P0-2's `input hold` says how long an input was kept out of the pool; this says WHO kept it: a deposit
    // that no hop claims has only two ways out (the sweep commits it once the chain has moved on, or the
    // eviction loop drops it - and only PRODUCED entries are ever evicted), and until then its transforms'
    // tokens are held. So "the oldest unclaimed block reached N seconds" is the reading that decides whether
    // the hand-over itself is where the uplink parks (measured on `q9-conc2`: holds up to 61.4 s).
    if (hand.handed != 0) {
      const double claimed_n = static_cast<double>(std::max<uint64_t>(1, hand.claim_count));
      const double produced_n = static_cast<double>(std::max<uint64_t>(1, hand.produced_count));
      std::fprintf(stderr,
                   "[metal_stats] block lifecycle (P0-7): claimed=%llu wait max=%.1fus mean=%.1fus; produced=%llu "
                   "deposit->completion max=%.1fus mean=%.1fus; unclaimed at once max=%llu, oldest unclaimed "
                   "age max=%.1fus at slot=%llu; registry commit->completion=%llu max=%.1fus mean=%.1fus "
                   "(Q9-B); dry-pool reaps=%llu recovering %llu block(s); handler lag=%llu max=%.1fus mean=%.1fus "
                   "at slot=%llu (Q9-E: GPU done -> handler ran)\n",
                   static_cast<unsigned long long>(hand.claim_count),
                   static_cast<double>(hand.claim_wait_max_us),
                   static_cast<double>(hand.claim_wait_sum_us) / claimed_n,
                   static_cast<unsigned long long>(hand.produced_count),
                   static_cast<double>(hand.produced_wait_max_us),
                   static_cast<double>(hand.produced_wait_sum_us) / produced_n,
                   static_cast<unsigned long long>(hand.unclaimed_now_max),
                   static_cast<double>(hand.unclaimed_age_max_us),
                   static_cast<unsigned long long>(hand.unclaimed_age_max_slot),
                   // Q9-B: the half of the completion wait that came after the registry had committed the
                   // block - see handed_counters. Read against `deposit->completion` above: equal maxima mean
                   // the commit was never the problem and the command buffer's own life was.
                   static_cast<unsigned long long>(hand.commit_count),
                   static_cast<double>(hand.commit_wait_max_us),
                   static_cast<double>(hand.commit_wait_sum_us) / static_cast<double>(std::max<uint64_t>(1, hand.commit_count)),
                   // Q9-A: how often a DRY receive pool drove the sweep itself, and how many unclaimed
                   // blocks that recovered. Events without blocks = the stall was not an unclaimed block.
                   static_cast<unsigned long long>(hand.reaped_by_park_events),
                   static_cast<unsigned long long>(hand.reaped_by_park_blocks),
                   // Q9-E: the host's own lag after the GPU finished. Read against `deposit->completion`: equal
                   // maxima mean the seconds were the HOST's, not the queue's.
                   static_cast<unsigned long long>(hand.handler_lag_count),
                   static_cast<double>(hand.handler_lag_max_us),
                   static_cast<double>(hand.handler_lag_sum_us) /
                       static_cast<double>(std::max<uint64_t>(1, hand.handler_lag_count)),
                   static_cast<unsigned long long>(hand.handler_lag_max_slot));
      bool printed_header = false;
      for (const auto& slow : hand.slowest) {
        if (slow.used == 0) {
          continue;
        }
        if (!printed_header) {
          std::fprintf(stderr,
                       "[metal_stats] block lifecycle (P0-7) slowest deposit->completion "
                       "(claimed=by a hop, swept=by the registry's sweep):\n");
          printed_header = true;
        }
        std::fprintf(stderr,
                     "[metal_stats]   slot=%llu claimed=%d swept=%d wait_for_a_claim=%.1fus "
                     "deposit->completion=%.1fus registry_commit=%d commit->completion=%.1fus\n",
                     static_cast<unsigned long long>(slow.slot),
                     static_cast<int>(slow.claimed),
                     static_cast<int>(slow.swept),
                     static_cast<double>(slow.claim_wait_us),
                     static_cast<double>(slow.produced_wait_us),
                     static_cast<int>(slow.registry_commit),
                     static_cast<double>(slow.commit_wait_us));
      }
    }
    const uint64_t early_signals = s.token_early_signals.load(std::memory_order_relaxed);
    const uint64_t by_event      = s.token_sets_by_event.load(std::memory_order_relaxed);
    if ((early_signals != 0) && (by_event == 0)) {
      std::fprintf(stderr,
                   "[metal_stats] P2-E: %llu early token-release signal(s) encoded and NOT ONE released a "
                   "block: this platform publishes a mid-command-buffer signal only at its completion, so the "
                   "input is still held for the whole hop. Read the pool numbers as UNCHANGED, not as "
                   "'the hold does not matter'\n",
                   static_cast<unsigned long long>(early_signals));
    }
  }
}
/// \brief Registers the transform input requirement: the transforms of this run read the radio's
/// int16 samples instead of a host-staged copy (S-7f-6f).
static void register_dft_contract_check()
{
  // Audited: takes its transform input from the radio's buffer by zero-copy mapping when it can,
  // stages a copy (counted below as a host -> device write) when it cannot, and never reads device
  // data back.
  phy_pipeline_crossings::declare_reporter("dft");

  register_phy_pipeline_check(
      {"dft radio inputs", []() -> std::optional<bool> {
         const dft_stats_t& s = dft_stats();
         // TRANSFORMS, not command buffers: a block of them shares one command buffer (begin_block()),
         // so counting commits here printed "240016 of 17145 transforms" on the first batched leg.
         //
         // TWO populations, and this check used to compare them: `radio` counts the transforms that came
         // in through the slot-grid-write (hand-over) route and `committed` the ones committed through the
         // plain per-transform route, while BOTH wrap the radio's buffer zero-copy. The claim - "the
         // transform input is the radio's samples, not a host-staged copy" - is therefore neither of those
         // counts: it is `staged == 0`, incremented exactly where wrap_buffer() has to fall back to
         // newBufferWithBytes() (and counted as a host write by the crossings audit). Measured before the
         // correction (5.9.98): an n1 leg printed "392714 of 1 ... -> OK", i.e. it passed by comparing
         // against a counter only the OTHER route feeds, and an n78 leg printed FAILED while its 158845
         // plain submits were as zero-copy as the rest. The hand-over SHARE is printed because it is worth
         // watching, not because it is a verdict: on a TDD cell the slots the lane does not claim take the
         // plain route by construction (5.9.98 (2)).
         const uint64_t radio     = s.radio_inputs.load(std::memory_order_relaxed);
         const uint64_t committed = s.transforms.load(std::memory_order_relaxed);
         const uint64_t staged    = s.wrap_copies.load(std::memory_order_relaxed);
         const uint64_t total     = radio + committed;
         // UNIT CARE: `staged` counts WRAPS (one wrap_buffer() call each), not transforms - a run wraps its
         // input and its output - so it is reported apart from the transform counts instead of being
         // subtracted from them. The old form mixed populations; mixing units would be the same mistake.
         const uint64_t plain_none  = s.plain_without_block.load(std::memory_order_relaxed);
         const uint64_t plain_block = s.plain_with_block.load(std::memory_order_relaxed);
         const uint64_t plain_no_slot = s.plain_without_lane_slot.load(std::memory_order_relaxed);
         std::fprintf(stderr,
                      "%llu transform(s) went through the two submit routes: the hand-over route carried %llu "
                      "(%.1f%%), the plain route %llu; %llu buffer wrap(s) had to stage a host copy. "
                      "Plain route by why: %llu got their own command buffer (%llu of those before any slot "
                      "was told to the engine), %llu joined an open block",
                      static_cast<unsigned long long>(total),
                      static_cast<unsigned long long>(radio),
                      (total == 0) ? 0.0
                                   : (100.0 * static_cast<double>(radio) / static_cast<double>(total)),
                      static_cast<unsigned long long>(committed),
                      static_cast<unsigned long long>(staged),
                      static_cast<unsigned long long>(plain_none),
                      static_cast<unsigned long long>(plain_no_slot),
                      static_cast<unsigned long long>(plain_block));
         if ((total == 0) || !phy_pipeline_mode_registry::is_published() ||
             (phy_pipeline_mode_registry::get() == phy_pipeline_mode::cpu)) {
           // No Metal transform in this run, or a run that never claimed the offloaded pipeline (a
           // unit test or a tool exercises the engine directly): nothing to require of it.
           return std::nullopt;
         }
         // Stated in terms of the COPIES, because that is what the contract claims and what the engine
         // counts where the decision is made. The reverse arm lives in
         // dft_processor_metal_unit_test.cpp: an input whose zero-copy wrap is refused (a pointer Metal
         // will not take without a copy) has to turn this red.
         return staged == 0;
       }});
}

/// \brief Commits a handed-over block the registry had to drop (see shared_burst::set_drop_committer()).
///
/// The engine knows what a commit owes and the registry does not: the GPU-time probe must be armed right
/// before the commit. A late block is deliberately NOT published on the front-end chain: it is the receiving
/// chain's work, whose consumer is gone, and the chain's waiters must not be made to wait for it.
void commit_late_handed_block(void* command_buffer)
{
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  if (cb == nil) {
    return;
  }
  metal::shared_queue::arm_gpu_time(cb, metal::shared_queue::queue_kind::back_end, "late_handed");
  // Q9-F: this is the commit the header's blind spot is about - the registry claims a block under its lock and
  // commits it here, on whichever thread runs the sweep, while the hop that missed the hand-over may already be
  // encoding a wait for the grid this very buffer produces.
  metal::shared_queue::note_commit_order(cb);
  [cb commit];
}

/// Registered once, on first use of the engine (see register_dft_contract_check()).
static const bool dft_contract_registered = []() {
  register_dft_contract_check();
  metal::shared_burst::set_drop_committer(&commit_late_handed_block);
  return true;
}();

/// \brief One line of the handover's counters, every so many blocks - because an arm that BREAKS the chain
///        does not exit cleanly, and the exit-time report is then exactly what is missing (5.9.7, and the
///        second time this happened, see the S18 note on instruments that print at exit).
///
/// The two pairs that matter: `taken` against `handed` (is the hop claiming what the receiving chain hands
/// over?), and `keepalives released` against `attached` (is the input coming back? a leak here starves the
/// radio's receive pool, which is a hard stall - the pool is the backpressure the receive loop blocks on).
///
/// It goes to the "PHY" logger at DEBUG level rather than to stderr. At the period below, and one commit plus
/// one release per hop, it wrote NINE lines a second for the whole of a leg - measured 890 lines over the 99 s
/// of `s53-poolfix` (2026-09-22), 1205 on `s52-residency` - which is the one thing on a console an operator
/// cannot read through.
///
/// Nothing is lost on a CLEAN leg: the same fields are in the exit report's
/// `[metal_stats] dft handover ... (armed=...)` line, which has been part of every leg report all along. That
/// is also why there is deliberately NO second "[dft_handover] final" line here: two format strings carrying
/// the same twelve counters is how a reader ends up comparing two different quantities without noticing.
///
/// The level is checked BEFORE anything is formatted, so a run at the default level pays a relaxed increment
/// per hop and nothing else. `--log.phy_level debug` brings the timeline back - and that is the recipe for the
/// case this heartbeat was written for, a leg whose chain breaks and which therefore has no exit report: turn
/// the level up for the DIAGNOSING leg, rather than flooding every leg to keep the option open.
void dft_handover_heartbeat(const char* where)
{
  static std::atomic<uint64_t> counter{0};
  constexpr uint64_t           period = 32;
  if ((counter.fetch_add(1, std::memory_order_relaxed) % period) != 0) {
    return;
  }
  auto& logger = ocudulog::fetch_basic_logger("PHY");
  if (!logger.debug.enabled()) {
    return;
  }
  const metal::shared_burst::handed_counters hand = metal::shared_burst::handed_stats();
  const dft_stats_t&                         s    = dft_stats();
  logger.debug("[dft_handover] {} handed={} taken={} superseded={} evicted={} "
               "evicted_unproduced={} over_bound={} unproduced={} "
               "fallback={} late={} late_time={} not_found={} timeouts={} keepalives={}/{} (max in flight {})",
               where,
               hand.handed,
               hand.taken,
               hand.superseded,
               hand.evicted,
               hand.evicted_unproduced,
               hand.over_bound,
               hand.unproduced,
               hand.fallback_commits,
               hand.late_commits,
               hand.late_commits_time,
               hand.grid_not_found,
               hand.ready_timeouts,
               s.keepalives_released.load(std::memory_order_relaxed),
               s.keepalives.load(std::memory_order_relaxed),
               s.keepalives_in_flight_max.load(std::memory_order_relaxed));
}
#else  // OCUDU_METAL_STATS
static void dft_stats_note_depth(uint64_t /*depth*/) {}
static void dft_stats_commit(uint64_t /*nof_transforms*/ = 1) {}
static void dft_stats_wait() {}
static void dft_stats_wrap_copy() {}
static void dft_stats_plain_submit(bool /*with_block*/, bool /*with_lane_slot*/) {}
static void dft_stats_radio_input() {}
static void dft_stats_release() {}
static void dft_stats_released_wait() {}
static void dft_stats_keepalive() {}
static void dft_stats_keepalives_released(uint64_t /*nof*/) {}
static void dft_stats_token_early_signal() {}
static void dft_stats_token_set_released(bool /*by_event*/) {}
static void dft_stats_token_holds(const std::vector<hold_clock::time_point>&,
                                  uint64_t,
                                  bool)
{
}
void dft_handover_heartbeat(const char* /*where*/) {}
#endif // OCUDU_METAL_STATS

// ---- Process-wide Metal resources: one device, one queue, one pipeline for all sizes ----
struct dft_resources_t {
  id<MTLDevice>              device   = nil;
  id<MTLCommandQueue>        queue    = nil;
  id<MTLComputePipelineState> pipeline = nil;
};

static dft_resources_t& dft_resources()
{
  static dft_resources_t r;
  return r;
}

static std::mutex& dft_resources_mutex()
{
  static std::mutex m;
  return m;
}

NSString* resolve_dft_metallib_path()
{
  NSMutableArray<NSString*>* candidates = [NSMutableArray arrayWithCapacity:3];
  [candidates addObject:[NSString stringWithUTF8String:OCUDU_DFT_METALLIB_PATH]];
  NSArray<NSString*>* args = [[NSProcessInfo processInfo] arguments];
  if (args.count > 0) {
    [candidates
        addObject:[[args[0] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"ocudu_dft.metallib"]];
  }
  [candidates addObject:[[[NSFileManager defaultManager] currentDirectoryPath]
                            stringByAppendingPathComponent:@"ocudu_dft.metallib"]];

  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* path in candidates) {
    if ([fm fileExistsAtPath:path]) {
      return path;
    }
  }
  return nil;
}

/// Transform slots covered by the input/output buffers (dft_processor_metal::max_batch).
static constexpr unsigned max_batch_slots = 16;

struct dft_engine_impl {
  id<MTLCommandBuffer> last_committed_cb = nil;
  /// Receiving slot the transforms being submitted belong to (see set_lane_slot()), and whether it was
  /// ever told: without a slot there is nothing to group the transforms by, and the probe is not fed
  /// (the offline tools submit transforms without a receiving slot at all).
  uint64_t lane_slot     = 0;
  bool     has_lane_slot = false;

  /// \name One command buffer for a block of transforms (see commit_open()).
  ///
  /// A caller whose samples arrive a BLOCK at a time - the receiving chain under the whole-slot policy
  /// gets a whole slot per receive call - hands the transforms of that block over here and they are
  /// encoded into ONE command buffer, committed when the block ends or when a wait forces it. What it
  /// saves is the per-command-buffer cost, measured at ~12.7us of GPU time whether the buffer carries
  /// one transform or fourteen (see the DFT unit test): the transform itself is under a microsecond.
  /// What it must never do is make a transform wait for samples that have not arrived: the caller opens
  /// and commits the block around the samples it already holds (see set_block_transforms()).
  ///@{
  id<MTLCommandBuffer>         open_cb  = nil;
  id<MTLComputeCommandEncoder> open_enc = nil;
  uint64_t                     open_transforms = 0;
  ///@}

  /// \name D1 step 1: the block handed over instead of committed (see release_block()).
  ///
  /// The released buffer is held STRONG until this engine releases the next block: the handle the caller
  /// gets is a +0 reference (the cast is a __bridge one), so without this the block would be deallocated
  /// between the release and the adoption that is supposed to take it over.
  ///@{
  id<MTLCommandBuffer> released_cb = nil;
  /// Slots whose transform went out with a released block. \c slot_pending stays SET (the host still must
  /// not read those outputs), which is what lets wait_slot() recognise the request and refuse it loudly
  /// rather than return as a satisfied wait.
  bool slot_released[16] = {};
  ///@}

  /// \name D1 step 3: the input's lifetime travels with the block (see retain_for_block()).
  ///
  /// Tokens attached to the block that is open now. They are handed to whoever ends the block - the commit
  /// path or the handover - which releases them EXACTLY ONCE, on the buffer's completion or when the block
  /// is definitively dropped.
  ///@{
  std::vector<dft_metal_engine::keep_alive> open_tokens;
  /// \brief P0-2: when each of \c open_tokens was attached, PARALLEL to it (same index).
  ///
  /// The hold - attach -> release - is the quantity the receive pool feels (a token keeps a whole slot's
  /// samples out of the pool, 14 of them per slot), and until now it had no direct reading at all: the plan
  /// could only infer it from the hop's span. It is recorded per TOKEN rather than per block because the
  /// tokens of one slot are attached symbol by symbol, and the interesting number is the longest hold, not
  /// the block's total.
  std::vector<hold_clock::time_point> open_token_times;
  ///@}

  /// Command buffer of the newest submission per transform slot (ring pipelining).
  id<MTLCommandBuffer> slot_cb[max_batch_slots <= 16 ? 16 : max_batch_slots] = {};
  bool                 slot_pending[16]                                  = {};

  uint32_t n       = 0;
  uint32_t radix2  = 0; // number of radix-2 stages (k in N = 2^k * 3^m)
  uint32_t radix3  = 0; // number of radix-3 stages (m)
  uint32_t inverse = 0;
  double   last_gpu_us = 0.0;

  // Zero-copy wrappers, cached by host pointer with the cached length stored alongside
  // (the S-1 audit hardening: a larger request re-wraps instead of silently truncating).
  std::unordered_map<const void*, std::pair<id<MTLBuffer>, size_t>> buffer_cache;

  id<MTLBuffer> buf_tw   = nil; // zero-copy wrap of the host twiddle table (N/2 float2)
  id<MTLBuffer> buf_perm = nil; // zero-copy wrap of the digit-reversal permutation table (N uint32)

  // Optional per-element table of the grid write (the demodulator's DFT window phase compensation), copied into an
  // engine-owned page-aligned buffer so the engine controls its lifetime.
  id<MTLBuffer> buf_window   = nil;
  bool          has_window   = false;
  void*         window_mem   = nullptr;
  size_t        window_bytes = 0;

  // Warm-up scratch (page-aligned, engine lifetime; freed by the destructor).
  void* warmup_mem = nullptr;
};

/// \brief Whether this run asks for a block of transforms to share one command buffer.
///
/// DEFAULT ON since its leg confirmed it (48.191(g)): the front end's GPU time per slot went from 531.6
/// to 424.0us and the end-to-end [ul_pipeline] moved with it (-105us), with the contract, the red lines
/// and the back-end lane unchanged. OCUDU_DFT_OPEN_BLOCK=0 is the escape hatch.
static bool block_batching_requested()
{
  const char* env = std::getenv("OCUDU_DFT_OPEN_BLOCK");
  return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
}

/// \brief Whether a block is OPEN, i.e. the transforms being submitted belong to one command buffer.
static bool block_accumulating(const dft_engine_impl* e)
{
  return (e != nullptr) && (e->open_cb != nil);
}

/// \brief Records that \p cb carries the transform of \p slot (the per-slot ring bookkeeping).
///
/// One place because the release path adds a third piece of state to the pair: a slot whose transform went
/// out with a released block has to be recognised by wait_slot() (see release_block()), and a NEW
/// submission into that slot is precisely what ends that state - the slot no longer names the buffer the
/// engine handed over.
static void note_slot_submission(dft_engine_impl* e, unsigned slot, id<MTLCommandBuffer> cb)
{
  e->slot_cb[slot]       = cb;
  e->slot_pending[slot]  = true;
  e->slot_released[slot] = false;
}

/// \brief Whether this run asks the open block to be HANDED OVER instead of committed (D1 step 1).
///
/// The decision is grid_handover_armed()'s, so that this engine, the counter line it prints, the OFDM
/// demodulator's startup warning and the burst's trace cannot drift apart. **DEFAULT ON** since the s46
/// leg pair (design document 5.9.49); \c OCUDU_DFT_RELEASE_BLOCK=0 is the one-line retreat. Read on every
/// call rather than cached, so the unit test can arm and disarm it around the arms it compares - which is
/// also what keeps the caller from having to know that the decision is taken at begin_block() time.
static bool block_release_requested()
{
  return grid_handover_armed();
}

/// \brief P2-E: whether the block's INPUT tokens are released at its last input-reading dispatch instead of at
/// the command buffer's completion (`OCUDU_DFT_RELEASE_TOKENS_EARLY=1`, **DEFAULT OFF**).
///
/// WHY IT EXISTS. The tokens exist because the front end's transforms run later than the call that hands the
/// samples over (see retain_for_block()). They are released by the completion handler, and after D1 the
/// command buffer they are armed on is the WHOLE HOP: the front-end transforms, the estimator, the
/// equalization and the demapping are ONE submission, so the input is held for the whole hop's span
/// (design document 5.9.129 (2)). But only the front end READS it: the estimator and everything after it read
/// the resource GRID the front end wrote. On n1 that is a hold of ~5.25 ms instead of the front end's own few
/// tens of microseconds, and the receiving chain pays for it - the radio's receive pool is the backpressure the
/// receive loop blocks on (measured: the pool drained to `held_max == pool` with `starved_events` in the
/// thousands, and the 2.85x n1 regression carries a ~5 s receive stall).
///
/// HOW. With the switch on, the block encodes a signal on a shared event right after its encoder is closed
/// (i.e. after every dispatch that reads the input, and before the adopter's first dispatch), and the tokens
/// are released by a notification on that event. The completion handler STAYS as the fallback: a block whose
/// signal never arrives - the buffer failed, or the handover was dropped before anyone committed it - must not
/// leak its input, and releasing twice is impossible by construction (see block_token_set::released).
///
/// Read on every call rather than cached, like the two switches above, so the metal test can arm and disarm it
/// around the arms it compares.
static bool release_tokens_early_requested()
{
  const char* env = std::getenv("OCUDU_DFT_RELEASE_TOKENS_EARLY");
  return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
}

/// \brief The event a block signals once its last input-reading dispatch has run (P2-E).
///
/// One event for the whole process, and one monotonically increasing generation per armed block - the same
/// shape `shared_queue`'s grid-ready and stage-fence events use, and for the same reason: a generation that
/// has been handed out always has a signaller on its way, so a notification registered for it can never hang.
///
/// Created under its own once_flag: two threads that each created their own event would signal one and
/// listen on the other, and the tokens would then wait for their completion-handler fallback instead.
static id<MTLSharedEvent> token_release_event()
{
  static id<MTLSharedEvent> event = nil;
  static std::once_flag     once;
  std::call_once(once, []() {
    id<MTLDevice> device = dft_resources().device;
    if (device == nil) {
      return;
    }
    event = [device newSharedEvent];
  });
  return event;
}

/// \brief The dispatch queue the token-release notifications are delivered on.
///
/// `init` (rather than `initWithDispatchQueue:`) gives the listener a serial queue of its own, which is what
/// the release needs: the callback returns a receive buffer to the radio's pool, and doing that from a queue
/// shared with anything else would put this work behind it.
static MTLSharedEventListener* token_release_listener()
{
  static MTLSharedEventListener* listener = [[MTLSharedEventListener alloc] init];
  return listener;
}

/// \brief Encodes the early token-release signal into \p cb and returns its generation (0 = not encoded).
///
/// MUST be called with NO encoder open on \p cb: `encodeSignalEvent:value:` is a command-buffer-level call and
/// Metal aborts the process if one is encoded while an encoder is active (MTLCommandBuffer.h). Encoding it
/// after the block's encoder is closed is exactly what makes it fire after the block's dispatches: a signal is
/// ordered after everything encoded before it in the same buffer.
static uint64_t encode_token_release_signal(id<MTLCommandBuffer> cb)
{
  id<MTLSharedEvent> event = token_release_event();
  if ((cb == nil) || (event == nil)) {
    return 0;
  }
  static std::atomic<uint64_t> generation{0};
  const uint64_t               value = generation.fetch_add(1, std::memory_order_relaxed) + 1;
  [cb encodeSignalEvent:event value:value];
  dft_stats_token_early_signal();
  return value;
}

/// \brief The tokens of one block, released exactly once (see dft_metal_engine::retain_for_block()).
///
/// Shared because two things race to release them and must not both win: the command buffer's completion
/// handler (the normal end), and whoever DROPS the block (a handover nobody claimed - the majority case in
/// practice: the air leg had ~2 blocks per hop, so most blocks have no consumer at all).
struct block_token_set {
  std::mutex                                mutex;
  bool                                      released = false;
  std::vector<dft_metal_engine::keep_alive> tokens;
  /// The command buffer these tokens belong to, as an opaque id: the lifecycle trace below is what says
  /// WHICH block's input never came back when one does not (5.9.11).
  const void* cb = nullptr;
  /// Whether this set was armed for the EARLY release too (P2-E): it is then racing three ways - the event, the
  /// completion handler and the drop hook - and the counters say which of them won.
  bool early_armed = false;
  /// \brief P0-2: when each of \c tokens was attached (parallel to it), and the lane slot the block belonged
  /// to - so the release can account the HOLD and name the slot whose input was held longest.
  ///
  /// The hold is the quantity the receive pool feels (one token keeps a whole slot's samples out of the pool),
  /// and the longest one is what a leg has to be read on: a hold near the hop's span is the multi-hold the
  /// design accepted, a hold of SECONDS is the stall.
  std::vector<hold_clock::time_point> attached_at;
  uint64_t                                                     slot = 0;
  bool                                                         has_slot = false;
};

/// Runs every token's release() exactly once, on the calling thread (a Metal completion thread, or the
/// thread that dropped the block). The callbacks run with the lock RELEASED: a release that calls back into
/// the engine is legitimate, and holding the mutex across it would deadlock.
///
/// \param[in] by_event Whether the caller is the front end's release event (P2-E, see
///            release_tokens_early_requested()) rather than a completion or a drop. Only used for the
///            counters: the release itself is the same and happens exactly once whichever end wins.
static void release_block_tokens(const std::shared_ptr<block_token_set>& set, bool by_event = false)
{
  if (set == nullptr) {
    return;
  }
  std::vector<dft_metal_engine::keep_alive> tokens;
  {
    std::lock_guard<std::mutex> lock(set->mutex);
    if (set->released) {
      return;
    }
    set->released = true;
    tokens.swap(set->tokens);
  }
  // P0-2: account the HOLD (attach -> release) of every token of this set, and remember the longest one with
  // the slot it belonged to. Done BEFORE the callbacks run: a release callback returns the samples to the
  // radio's pool and may take locks of its own, and the measurement must not include that.
  dft_stats_token_holds(set->attached_at, set->slot, set->has_slot);
  for (const dft_metal_engine::keep_alive& token : tokens) {
    if (token.release != nullptr) {
      token.release(token.context);
    }
  }
  dft_stats_keepalives_released(tokens.size());
  dft_stats_token_set_released(by_event);
  static std::atomic<unsigned> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
    // DEBUG, like the rest of the D1 handshake lines (see d1_trace() in ocudu_metal_burst.mm).
    auto& logger = ocudulog::fetch_basic_logger("PHY");
    if (logger.debug.enabled()) {
      logger.debug("[d1_handover] done cb={} tokens={}", fmt::ptr(set->cb), tokens.size());
    }
  }
}

/// Moves \p tokens into a set and arms it on \p cb: they are released when that command buffer COMPLETES.
/// Must be called before the commit (Metal asserts on a handler added afterwards).
///
/// \param[in] early_generation The value of the early release signal encoded in \p cb (P2-E, see
///            encode_token_release_signal()), or 0 when this block has no early signal. With one, the tokens
///            are released by that event as well - and the completion handler below STAYS as the fallback,
///            because a block whose signal never arrives (a failed buffer, or a handover nobody commits) must
///            not leak the input it holds. Both paths funnel into release_block_tokens(), which is idempotent,
///            so "whichever end comes first" is the whole of the race and no ordering assumption is made.
///
/// \return The set, so the caller can keep it (the handover hands it to the registry as its drop hook).
static std::shared_ptr<block_token_set>
arm_tokens_on_complete(id<MTLCommandBuffer>                                              cb,
                       std::vector<dft_metal_engine::keep_alive>&&                      tokens,
                       std::vector<hold_clock::time_point>&&  attached_at,
                       uint64_t                                                         slot,
                       bool                                                             has_slot,
                       uint64_t                                                         early_generation = 0)
{
  auto set     = std::make_shared<block_token_set>();
  set->cb      = (__bridge const void*)cb;
  set->slot    = slot;
  set->has_slot = has_slot;
  if (tokens.empty()) {
    return set;
  }
  set->tokens      = std::move(tokens);
  set->attached_at = std::move(attached_at);
  set->early_armed = (early_generation != 0);
  [cb addCompletedHandler:^(id<MTLCommandBuffer> /*completed*/) {
    release_block_tokens(set);
  }];
  if (early_generation != 0) {
    id<MTLSharedEvent>         event    = token_release_event();
    MTLSharedEventListener*    listener = token_release_listener();
    if ((event != nil) && (listener != nil)) {
      [event notifyListener:listener
                    atValue:early_generation
                      block:^(id<MTLSharedEvent> /*signalled*/, uint64_t /*value*/) {
                        release_block_tokens(set, /*by_event=*/true);
                      }];
    }
  }
  return set;
}

/// \brief Closes an open block's encoder without committing it, for the paths that drop the engine.
///
/// Releasing a command encoder without endEncoding ABORTS in the Metal validation layer, and that is not
/// hypothetical: the first on-air leg of the block batching crashed on ^C with "Command encoder released
/// without endEncoding", because stopping the stream leaves the block of the interrupted slot open. The
/// command buffer is dropped rather than committed - at this point nothing is going to read its grid -
/// which is the same choice shared_burst's thread state makes for an open burst.
static void discard_open_block(dft_engine_impl* e)
{
  if ((e != nullptr) && (e->open_enc != nil)) {
    [e->open_enc endEncoding];
  }
  if (e != nullptr) {
    e->open_enc         = nil;
    e->open_cb          = nil;
    e->open_transforms  = 0;
    // The block is dropped, so the dispatches that would have read its input never run: the tokens go back
    // to their owners NOW rather than at a completion that will never come.
    if (!e->open_tokens.empty()) {
      std::vector<dft_metal_engine::keep_alive>                     tokens;
      std::vector<hold_clock::time_point> times;
      tokens.swap(e->open_tokens);
      times.swap(e->open_token_times);
      release_block_tokens(
          arm_tokens_on_complete(nil, std::move(tokens), std::move(times), e->lane_slot, e->has_lane_slot));
    }
  }
}

/// \brief Encoder for the next dispatch: the open command buffer when one is accumulating, a fresh one otherwise.
/// \return False when no encoder could be created (the caller must not commit anything).
static bool encode_into(dft_engine_impl*                                 e,
                        id<MTLCommandBuffer> __strong*                   cb_out,
                        id<MTLComputeCommandEncoder> __strong*           enc_out)
{
  if (block_accumulating(e)) {
    *cb_out  = e->open_cb;
    *enc_out = e->open_enc;
    return true;
  }
  id<MTLCommandBuffer> cmd_buf = [dft_resources().queue commandBuffer];
  if (cmd_buf == nil) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = [cmd_buf computeCommandEncoder];
  if (enc == nil) {
    return false;
  }
  *cb_out  = cmd_buf;
  *enc_out = enc;
  return true;
}

/// \brief Closes and commits a command buffer of this engine, with everything a front-end commit owes.
///
/// One place on purpose: the GPU-time probe must be armed before the commit, the front-end fence signal
/// is a command-buffer level API that has to be encoded with the encoder already closed, and the commit
/// has to be published on the front-end chain so wait_all_committed() can drain it (a commit published
/// on the wrong chain would make the wait target another queue's command buffer).
static void commit_front_end(dft_engine_impl* e, id<MTLCommandBuffer> cb, uint64_t nof_transforms)
{
  // The block's input tokens are armed BEFORE the commit (Metal asserts on a handler added afterwards) and
  // released when this buffer completes: a block that is committed rather than handed over holds its input
  // for exactly as long as the dispatches that read it, which is the behaviour every run had before the
  // handover existed.
  if (!e->open_tokens.empty()) {
    std::vector<dft_metal_engine::keep_alive>                     tokens;
    std::vector<hold_clock::time_point> times;
    tokens.swap(e->open_tokens);
    times.swap(e->open_token_times);
    (void)arm_tokens_on_complete(cb, std::move(tokens), std::move(times), e->lane_slot, e->has_lane_slot);
  }
  dft_handover_heartbeat("commit");
  metal::shared_queue::arm_gpu_time(cb, metal::shared_queue::queue_kind::front_end, "dft_front_end");
  metal::shared_queue::note_commit_order(cb);
  [cb commit];
  dft_stats_commit(nof_transforms);
  if (e->has_lane_slot) {
    metal::gpu_lane_probe::register_front_end_commit(cb, e->lane_slot);
  }
  metal::shared_queue::notify_commit(cb, metal::shared_queue::queue_kind::front_end);
  e->last_committed_cb = cb;
}


/// Reports a refused radio-input request ONCE and tells the caller to stage its own input.
///
/// Not an error: the caller (the OFDM demodulator) falls back to filling the engine's float2 ring,
/// which is what it did before the input could come from the radio buffer. It is worth one warning
/// because it means the zero-copy input is not happening for the whole run.
bool refuse_time_input(dft_engine_impl* engine, const dft_metal_engine::grid_write& write, const char* reason)
{
  static bool warned = false;
  if (!warned) {
    warned = true;
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal DFT: the transform input cannot be taken from the radio buffer ({}); the caller stages it on the host",
        reason);
  }
  (void)engine;
  (void)write;
  return false;
}

id<MTLBuffer> wrap_buffer(dft_engine_impl* engine, const void* ptr, size_t length)
{
  auto it = engine->buffer_cache.find(ptr);
  if (it != engine->buffer_cache.end()) {
    if (length <= it->second.second) {
      return it->second.first;
    }
    ocudulog::fetch_basic_logger("PHY").warning(
        "Metal DFT: zero-copy cache hit with a larger request ({} > cached {}): re-wrapping the buffer",
        length,
        it->second.second);
  }
  // The mapping may never cover more than the allocation it starts in, and it must start on a page:
  // the length is rounded with the RUNTIME page size (4 KiB on Linux, 16 KiB on Apple Silicon - a
  // hard-coded 4096 both fails the alignment and overstates the block), and when the process-wide
  // registry knows which aligned_alloc block the pointer belongs to, the rounded length is clamped to
  // what is left of it. A pointer the registry does not describe keeps the historical behaviour (the
  // caller's length is taken at face value).
  void*  alloc_base = nullptr;
  size_t alloc_size = 0;
  bool   alloc_known = compat::describe_aligned_allocation(ptr, &alloc_base, &alloc_size);
  const size_t page = compat::page_size();
  size_t       aligned = 0;
  if (alloc_known) {
    const size_t offset = static_cast<size_t>(static_cast<const char*>(ptr) - static_cast<const char*>(alloc_base));
    const size_t usable = (alloc_size > offset) ? (alloc_size - offset) : 0;
    // The allocation is page rounded by construction, so the largest page multiple that fits in it is
    // its own remainder: round DOWN to it. Rounding UP past the allocation is the over-map the old code
    // did, and refusing instead is worse than that: a refusal falls back to a COPY, and this engine
    // hands out the grid - a buffer the GPU keeps writing to - so a cached copy is a grid the
    // demodulator reads but nobody ever writes (measured on air: garbage symbols, negative SINR, RLF).
    aligned = (usable / page) * page;
    if (aligned < length) {
      aligned = 0; // the request really does not fit in the allocation: stage a copy
    }
  } else {
    // A pointer the registry does not describe: the caller owns the contract (historical behaviour).
    aligned = ((length + page - 1) / page) * page;
  }
  id<MTLBuffer> buf = nil;
  if (aligned != 0) {
    buf = [dft_resources().device newBufferWithBytesNoCopy:(void*)ptr
                                                    length:aligned
                                                   options:MTLResourceStorageModeShared
                                               deallocator:nil];
  }
  size_t mapped = aligned;
  if (buf == nil) {
    dft_stats_wrap_copy();
    // The same event in the lane-wide counter. The local one stays because the "dft radio inputs"
    // check is stated in terms of it.
    phy_pipeline_crossings::count_host_write_site("dft: input copied to the device (wrap refused)", length);
    buf = [dft_resources().device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
    // The COPY holds `length` bytes, not the page-rounded length: recording `aligned` here would let a
    // later, larger request (<= aligned) hit this cache entry and bind a buffer shorter than it reads -
    // the kernel would then read past the copy and produce garbage without anything failing.
    mapped = length;
  }
  engine->buffer_cache[ptr] = std::make_pair(buf, mapped);
  return buf;
}

/// \brief Maps the grid a transform writes: the process-wide cache on the release path, this engine's own
///        private one otherwise.
///
/// Why the release path may not use the private cache: two \c MTLBuffer objects over one address are
/// UNORDERED to Metal - a buffer-scope barrier does not relate them and neither does the encoder boundary
/// (measured 200/200: cases C..E and case G of wip/metal_alias_order.mm). The stages that adopt a released
/// block read the grid through \c shared_queue::wrap_no_copy(), which is keyed by address and hands out
/// the mapping that covers the request, so mapping the grid there too is what makes the adopter bind the
/// SAME object - and one object with an encoder boundary between the producer and the consumers IS ordered
/// (case F, 200/200). With the private cache the DFT would write an object nobody reads: the P0 signature,
/// silent wrong data.
///
/// \param[out] offset Byte offset of \p grid_base inside the returned mapping: the shared cache hands out
///             the mapping of the whole allocation, which may start below the grid.
/// \return The mapping, or nil when it could not be made - the release path REFUSES the dispatch then
///         rather than staging a copy, because a copied grid is a grid the GPU writes and the host reads
///         through a different memory (see wrap_buffer's note on the same trap).
static id<MTLBuffer> wrap_grid(dft_engine_impl* engine, const void* grid_base, size_t grid_bytes, size_t* offset)
{
  *offset = 0;
  if (!block_release_requested()) {
    return wrap_buffer(engine, grid_base, grid_bytes);
  }
  return metal::shared_queue::wrap_no_copy(metal::shared_queue::device(), grid_base, grid_bytes, offset);
}

} // namespace

dft_metal_engine::~dft_metal_engine()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine != nullptr) {
    discard_open_block(engine);
    // A block that was handed over is NOT ours to close: the adopter commits it, and its dispatches are
    // already encoded (release_block() ended the encoder). Dropping the strong reference here is what the
    // engine owes - the ladder that adopted it holds it for as long as it needs it.
    engine->released_cb = nil;
    engine->buffer_cache.clear();
    std::free(engine->warmup_mem);
    std::free(engine->window_mem);
    delete engine;
    impl = nullptr;
  }
}

bool dft_metal_engine::init(unsigned size, bool inverse)
{
  // Register the process-exit stats report exactly once (the counters live for the process).
#if defined(OCUDU_METAL_STATS)
  static std::once_flag stats_atexit_flag;
  std::call_once(stats_atexit_flag, []() {
    std::atexit(dft_stats_report);
    register_p0_report(dft_stats_report); // dev doc 6.24: joins the on-demand dump
  });
#endif

  if (size < 2 || size > max_size) {
    return false;
  }
  // Factor N = 2^k * 3^m (the kernel's supported family); anything else is rejected here
  // (the factory then falls back per configuration).
  {
    unsigned rem = size;
    unsigned k   = 0;
    unsigned m   = 0;
    while (rem % 2 == 0) {
      rem /= 2;
      ++k;
    }
    while (rem % 3 == 0) {
      rem /= 3;
      ++m;
    }
    if (rem != 1) {
      return false;
    }
    auto* engine   = new dft_engine_impl();
    impl           = engine;
    engine->n      = size;
    engine->radix2 = k;
    engine->radix3 = m;
  }
  auto* engine      = static_cast<dft_engine_impl*>(impl);
  engine->inverse   = inverse ? 1u : 0u;

  // Device, queue and pipeline are shared process-wide (they are size-independent).
  {
    std::lock_guard<std::mutex> lock(dft_resources_mutex());
    dft_resources_t& res = dft_resources();
    if (res.device == nil) {
      res.device = MTLCreateSystemDefaultDevice();
      if (res.device == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: no Metal device available");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      // rief Which queue the front-end DFT commits on.
      ///
      /// DEFAULT: the front-end queue (shared_queue::queue()), which is what this engine has always used - the
      /// per-symbol producers have a queue of their own so that the DFT of a slot overlaps the back-end lane of
      /// the previous one.
      ///
      /// OCUDU_DFT_BACKEND_QUEUE=1 commits them on the BACK-END queue instead, which is the queue the receiving
      /// chain's late stages (the estimator and the lane burst) use. That is an EXPERIMENT, not a candidate: with
      /// both stages on one queue, submission order alone orders the DFT before the lane's burst, so the
      /// cross-queue fence the burst used to encode stops being what provides the
      /// ordering - while still being encoded, so the two arms differ ONLY in whether the ordering crosses a
      /// queue. Its purpose is to price the fence, which is the precondition for design document 5.9's step 1:
      /// D1 wants the DFT on the lane's queue, and that change is only worth its cost if the cross-queue relation
      /// is what is expensive. If the two arms read the same, the fence is free and D1 becomes purely about the
      /// 1.83 CPU commits per hop it removes.
      auto dft_queue = []() {
        // D1 step 1: a block that may be RELEASED belongs to whoever commits it, and that is the lane, on
        // the back-end queue - a command buffer is bound to the queue that created it, so a block created
        // on the front-end queue could not be adopted into the lane's chain. Arming the release therefore
        // selects the queue as well; the two are one decision, not two knobs to keep in step.
        if (block_release_requested()) {
          std::fprintf(stderr,
                       "[dft_release] D1 step 1: the DFT's open block is handed over uncommitted "
                       "(the default since 5.9.49), so it is created on the BACK-END queue - the queue the "
                       "lane commits on\n");
          return metal::shared_queue::backend_queue();
        }
        const char* env = std::getenv("OCUDU_DFT_BACKEND_QUEUE");
        if ((env != nullptr) && (std::strtoul(env, nullptr, 10) != 0)) {
          std::fprintf(stderr,
                       "[dft_queue] EXPERIMENT: the front-end DFT commits on the BACK-END queue "
                       "(OCUDU_DFT_BACKEND_QUEUE=1)\n");
          return metal::shared_queue::backend_queue();
        }
        return metal::shared_queue::queue();
      };
      res.queue = dft_queue();

      NSString* lib_path = resolve_dft_metallib_path();
      if (lib_path == nil) {
        ocudulog::fetch_basic_logger("PHY").error(
            "Metal DFT: pre-compiled shader library 'ocudu_dft.metallib' not found (searched the configure-time "
            "path, next to the executable, and the working directory)");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      NSError*       error   = nil;
      id<MTLLibrary> library = [res.device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&error];
      if (library == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: failed to load the shader library {}: {}",
                                                  lib_path.UTF8String,
                                                  error != nil ? error.localizedDescription.UTF8String : "nil error");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      id<MTLFunction> fn = [library newFunctionWithName:@"dft_dit"];
      if (fn == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: kernel 'dft_dit' not found in the shader library");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }
      res.pipeline = [res.device newComputePipelineStateWithFunction:fn error:&error];
      if (res.pipeline == nil) {
        ocudulog::fetch_basic_logger("PHY").error("Metal DFT: pipeline creation failed: {}",
                                                  error != nil ? error.localizedDescription.UTF8String : "nil error");
        discard_open_block(engine);
        delete engine;
        impl = nullptr;
        return false;
      }

      ocudulog::fetch_basic_logger("PHY").debug("Metal DFT: loaded pre-compiled shader library {}", lib_path.UTF8String);
    }
  }

  // Host-side twiddle table: N/2 entries of exp(-2*pi*i*k/N), page-aligned, zero-copy wrapped. The
  // alignment is the RUNTIME page size: a 4 KiB-aligned pointer is not page aligned where the page is
  // 16 KiB (Apple Silicon), and newBufferWithBytesNoCopy then refuses it - the table used to be copied
  // silently for that reason (the [metal_stats] dft wrap_copies counter now says so if it happens).
  const size_t page     = compat::page_size();
  const size_t tw_bytes = ((static_cast<size_t>(size / 2) * 2 * sizeof(float)) + page - 1) / page * page;
  void*        tw_mem   = nullptr;
  if (::posix_memalign(&tw_mem, page, tw_bytes) != 0 || tw_mem == nullptr) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: twiddle allocation failed");
    delete engine;
    impl = nullptr;
    return false;
  }
  auto* tw = static_cast<float*>(tw_mem);
  for (uint32_t k = 0; k != size / 2; ++k) {
    const double ang = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(size);
    tw[2 * k]        = static_cast<float>(std::cos(ang));
    tw[2 * k + 1]    = static_cast<float>(std::sin(ang));
  }
  engine->buf_tw = wrap_buffer(engine, tw_mem, tw_bytes);
  if (engine->buf_tw == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: twiddle buffer wrap failed");
    std::free(tw_mem);
    delete engine;
    impl = nullptr;
    return false;
  }

  // Host-side mixed-radix digit-reversal permutation table (the DIT input order), page-aligned,
  // zero-copy wrapped. Factors are processed radix-2 first, then radix-3, matching the kernel.
  const size_t perm_bytes = static_cast<size_t>(size) * sizeof(uint32_t);
  const size_t perm_bytes_rounded = (static_cast<size_t>(perm_bytes) + page - 1) / page * page;
  void*        perm_mem   = nullptr;
  if (::posix_memalign(&perm_mem, page, perm_bytes_rounded) != 0 || perm_mem == nullptr) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: permutation table allocation failed");
    std::free(tw_mem);
    delete engine;
    impl = nullptr;
    return false;
  }
  {
    auto* perm = static_cast<uint32_t*>(perm_mem);
    for (uint32_t i = 0; i != size; ++i) {
      uint32_t rem        = i;
      uint32_t rev        = 0;
      uint32_t remaining  = size;
      // Radix-2 digits (least significant first).
      for (uint32_t q = 0; q != engine->radix2; ++q) {
        remaining /= 2;
        rev += (rem % 2) * remaining;
        rem /= 2;
      }
      // Radix-3 digits.
      for (uint32_t q = 0; q != engine->radix3; ++q) {
        remaining /= 3;
        rev += (rem % 3) * remaining;
        rem /= 3;
      }
      perm[i] = rev;
    }
  }
  engine->buf_perm = wrap_buffer(engine, perm_mem, perm_bytes_rounded);
  if (engine->buf_perm == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: permutation buffer wrap failed");
    std::free(perm_mem);
    std::free(tw_mem);
    delete engine;
    impl = nullptr;
    return false;
  }

  // Warm-up dispatch (once per process): the first command-buffer commit of each pipeline
  // pays the Metal driver's lazy compile; paying it here keeps it off the packet path.
  {
    static std::atomic<int> warmed{0};
    if (warmed.fetch_add(1, std::memory_order_acq_rel) == 0) {
      const size_t warmup_bytes = ((static_cast<size_t>(size) * 2 * sizeof(float)) + page - 1) / page * page;
      if (::posix_memalign(&engine->warmup_mem, page, warmup_bytes) == 0) {
        std::memset(engine->warmup_mem, 0, warmup_bytes);
        (void)run(engine->warmup_mem, engine->warmup_mem, 1);
      }
    }
  }

  return true;
}

bool dft_metal_engine::submit_slot(const void* in, void* out, unsigned slot)
{
  if (slot >= max_batch_slots) {
    return false;
  }
  bool ok = submit_at(in, out, 1, slot, false);
  if (ok) {
    // Remember the slot's command buffer so a pipelined caller can wait for this transform only
    // (wait_all() would also wait for the newer submissions and flatten the pipeline).
    dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
    // While a block accumulates, the transform is in the OPEN buffer and nothing was committed yet.
    note_slot_submission(engine, slot, block_accumulating(engine) ? engine->open_cb : engine->last_committed_cb);
    // The pipeline depth the diagnostic reports: the slots that hold an un-waited transform. It is what
    // "in flight" means for this engine, and it stays bounded by max_batch_slots however few waits the
    // caller pays (see dft_stats_note_depth()).
    uint64_t depth = 0;
    for (unsigned i = 0; i != max_batch_slots; ++i) {
      depth += engine->slot_pending[i] ? 1u : 0u;
    }
    dft_stats_note_depth(depth);
  }
  return ok;
}

bool dft_metal_engine::begin_block()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || !block_batching_requested()) {
    return false;
  }
  if (engine->open_cb != nil) {
    return true; // already open
  }
  // The block's command buffer is created when the caller says the block starts: from here until
  // commit_open() every transform is encoded into it.
  engine->open_cb = [dft_resources().queue commandBuffer];
  if (engine->open_cb == nil) {
    return false;
  }
  engine->open_enc = [engine->open_cb computeCommandEncoder];
  if (engine->open_enc == nil) {
    engine->open_cb = nil;
    return false;
  }
  engine->open_transforms = 0;
  return true;
}

bool dft_metal_engine::commit_open()
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || (engine->open_cb == nil) || (engine->open_enc == nil)) {
    return false;
  }
  id<MTLCommandBuffer>         cb  = engine->open_cb;
  id<MTLComputeCommandEncoder> enc = engine->open_enc;
  const uint64_t               nof = engine->open_transforms;
  engine->open_cb         = nil;
  engine->open_enc        = nil;
  engine->open_transforms = 0;
  [enc endEncoding];
  if (nof == 0) {
    // Nothing was encoded: the block produced no work, so there is nothing to commit. (The command
    // buffer is dropped; a transform that was refused by the caller never reached the engine.)
    return true;
  }
  commit_front_end(engine, cb, nof);
  return true;
}

bool dft_metal_engine::has_open() const
{
  const auto* engine = static_cast<const dft_engine_impl*>(impl);
  return (engine != nullptr) && (engine->open_cb != nil);
}

bool dft_metal_engine::block_release_enabled()
{
  return block_release_requested();
}

bool dft_metal_engine::early_token_release_enabled()
{
  return release_tokens_early_requested();
}

dft_metal_engine::token_release_stats_t dft_metal_engine::token_release_stats()
{
  token_release_stats_t out;
#if defined(OCUDU_METAL_STATS)
  out.early_signals = dft_stats().token_early_signals.load(std::memory_order_relaxed);
  out.by_event      = dft_stats().token_sets_by_event.load(std::memory_order_relaxed);
  out.by_complete   = dft_stats().token_sets_by_complete.load(std::memory_order_relaxed);
#endif
  return out;
}

bool dft_metal_engine::retain_for_block(const keep_alive& token)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  // Nothing is retained without an open block: the caller keeps ownership, which is what leaves every path
  // that does not batch (and every caller that does not know about tokens) exactly as it was.
  if ((engine == nullptr) || (engine->open_cb == nil) || (token.release == nullptr)) {
    return false;
  }
  engine->open_tokens.push_back(token);
  engine->open_token_times.push_back(hold_clock::now());
  dft_stats_keepalive();
  return true;
}

void* dft_metal_engine::release_block(const void* grid_base)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  // DEFAULT OFF: without the knob this answers nullptr and NOTHING else happens - not even the encoder
  // is touched, so the factory path cannot be disturbed by the release path existing (D1 step 1's
  // criterion). The check comes first for exactly that reason.
  if ((engine == nullptr) || !block_release_requested() || (engine->open_cb == nil)) {
    return nullptr;
  }
  id<MTLCommandBuffer> cb = engine->open_cb;
  const uint64_t       nof = engine->open_transforms;
  // Close the encoder before handing over: the adopter opens its own (shared_burst::take_released() takes
  // the buffer only), and the boundary between the two encoders is what orders the adopter's first dispatch
  // after this block's dispatches for the SAME buffer object (see wrap_grid()).
  if (engine->open_enc != nil) {
    [engine->open_enc endEncoding];
  }
  engine->open_enc        = nil;
  engine->open_cb         = nil;
  engine->open_transforms = 0;
  if (nof == 0) {
    // Nothing was encoded: there is no work to hand over and the buffer is dropped, exactly as
    // commit_open() drops an empty block.
    return nullptr;
  }
  // The slots keep slot_pending set - the host still must not read their output - and are marked as
  // released, so a later wait_slot() is recognised and refused instead of quietly satisfied. The command
  // buffer itself is held STRONG: the handle below is a +0 reference (see the impl struct).
  for (unsigned i = 0; i != max_batch_slots; ++i) {
    if (engine->slot_cb[i] == cb) {
      engine->slot_released[i] = true;
    }
  }
  engine->released_cb = cb;
  dft_stats_release();
  // NOT commit_front_end(): no commit, no front-end fence signal, no front-end chain publication, no
  // dft commit counter. The caller submits this buffer, and everything a commit owes moves with it
  // (see the header).
  //
  // The deposit is what actually carries the buffer to its consumer, which runs on another thread and
  // looks it up by the grid it is about to read (shared_burst::deposit_released()).
  //
  // The input tokens travel with it: armed on the completion (the adopter commits the buffer, and that is
  // when the transforms finally read the radio's samples), and released by the registry if the deposit is
  // DROPPED instead - a handover nobody claimed is never committed, so its completion would never come.
  //
  // P2-E (OCUDU_DFT_RELEASE_TOKENS_EARLY=1): the buffer also SIGNALS the moment its last input-reading
  // dispatch has run, and the tokens are released there instead - because with the hand-over the completion
  // is the end of the WHOLE HOP, while only this block's transforms read the input (the estimator and
  // everything after it read the grid they wrote). The signal is encoded HERE, after the encoder was closed
  // above and before the buffer is handed over: `encodeSignalEvent:` is a command-buffer-level call that
  // Metal refuses while an encoder is active, and a signal is ordered after everything encoded before it -
  // which is what puts it after the transforms and before the adopter's first dispatch. Nothing is added to
  // the submission itself: one extra command in a buffer that already exists (V4: cbs/lane unchanged).
  const uint64_t early_generation =
      (!engine->open_tokens.empty() && release_tokens_early_requested()) ? encode_token_release_signal(cb) : 0;
  const size_t nof_tokens = engine->open_tokens.size();
  std::vector<hold_clock::time_point> token_times;
  token_times.swap(engine->open_token_times);
  std::shared_ptr<block_token_set> tokens = arm_tokens_on_complete(cb,
                                                                  std::move(engine->open_tokens),
                                                                  std::move(token_times),
                                                                  engine->lane_slot,
                                                                  engine->has_lane_slot,
                                                                  early_generation);
  // The grid-production fence (D1-A, 5.9.13): a HOST reader of this grid - the PUCCH - waits on this
  // generation, because with the hand-over the grid is produced at the LANE's commit and a host read is not
  // ordered against it at all. Armed here, on the buffer that will carry the grid, before it is handed over.
  const uint64_t generation = metal::shared_queue::grid_ready_signal(cb);
  // The SLOT is half of the key (5.9.15): the grid's storage address alone is reused by the pool from one
  // slot to the next, and a consumer served the wrong slot's block reads a grid nobody wrote.
  metal::shared_burst::deposit_released(
      grid_base, engine->lane_slot, cb, generation, [tokens]() { release_block_tokens(tokens); });
  // Q9-F2 (dev doc 6.19): the deposit gets a front-end record of its own. Until this call the front-end series
  // was fed by commit_front_end() alone, and with the hand-over armed (the default) that function is NOT the
  // one that commits these blocks - the lane does, or the registry's sweep - so an air leg printed no
  // front-end timeline at all, exactly for the blocks whose GPU window the stall investigations needed. The
  // registration is by (slot), i.e. one group per slot as before, and the probe reads the timestamps when the
  // block completes - which may be long after its slot's group closed (see its carried-block readings).
  if (engine->has_lane_slot) {
    metal::gpu_lane_probe::register_front_end_commit(cb, engine->lane_slot);
  }
  // Per-deposit line, keyed by the grid the hop will look up: this and the take-side line in the estimator
  // are what say whether the two ends name the SAME address (D1 diagnostics, 5.9.11). Rate-limited, because
  // a healthy run has one per slot.
  {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
      // DEBUG, like the rest of the D1 handshake lines (see d1_trace() in ocudu_metal_burst.mm).
      //
      // NOTE: the line printed three fields from four arguments - `nof` was passed and never consumed by the
      // old %p/%p/%zu. The fields are kept exactly as they were printed (cb, grid, tokens=nof_tokens) rather
      // than quietly gaining a fourth, because fmt refuses an argument without a placeholder and a reader
      // comparing this line across legs must see the same three numbers.
      auto& logger = ocudulog::fetch_basic_logger("PHY");
      if (logger.debug.enabled()) {
        logger.debug("[d1_handover] deposit cb={} grid={} tokens={}", fmt::ptr((__bridge const void*)cb), fmt::ptr(grid_base), nof_tokens);
      }
    }
  }
  dft_handover_heartbeat("release");
  return (__bridge void*) cb;
}

/// Reports a wait that cannot be honoured because the slot's transform was handed over. Once, loudly:
/// the caller that released the block promised the host would not read its output, so this is a wiring
/// defect and the data the caller is about to read may not be there yet.
static void report_released_wait(unsigned slot)
{
  dft_stats_released_wait();
  static bool reported = false;
  if (!reported) {
    reported = true;
    ocudulog::fetch_basic_logger("PHY").error(
        "Metal DFT: wait_slot({}) cannot be honoured - this slot's transform went out with a block that was "
        "handed over uncommitted (release_block()), so this engine no longer owns its submission. The caller "
        "that released the block must guarantee nothing on the host reads the slot's output before the "
        "adopter commits it; the wait is skipped, not satisfied",
        slot);
  }
}

bool dft_metal_engine::wait_slot(unsigned slot)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if ((engine == nullptr) || (slot >= max_batch_slots) || !engine->slot_pending[slot]) {
    return true;
  }
  if (engine->slot_released[slot]) {
    // Handed over (release_block()): there is no command buffer of this engine's to wait for - the
    // adopter commits it. Refuse instead of returning as a satisfied wait, which is what would let the
    // caller read a grid the GPU has not written yet without anything saying so.
    report_released_wait(slot);
    return false;
  }
  // The transform of this slot may still be sitting in the OPEN command buffer of its block: commit it
  // first, or the wait below would target a buffer that has not been committed at all.
  if (engine->open_cb != nil) {
    (void)commit_open();
  }
  id<MTLCommandBuffer> cmd_buf = engine->slot_cb[slot];
  engine->slot_pending[slot]   = false;
  engine->slot_cb[slot]        = nil;
  // Account for the slot wait so [metal_stats] reports the real in-flight depth (the ring keeps
  // up to `pipeline depth` transforms in flight instead of one).
  dft_stats_wait();
  // [ul_dft_wait]: the host time this wait costs. Records the WHOLE waitUntilCompleted, which is the instant the
  // synchronization actually occupies the caller - not the GPU span (last_gpu_us) of the buffer it waits for.
  const auto wait_begin = std::chrono::steady_clock::now();
  [cmd_buf waitUntilCompleted];
  ul_pipeline_probe::get().record_dft_wait(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_begin).count());
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: slot {} command buffer failed with status {}",
                                              slot,
                                              static_cast<unsigned long>(cmd_buf.status));
    return false;
  }
  if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
    engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
  }
  return true;
}

bool dft_metal_engine::wait_all()
{
  // \note Static by design (it drains the whole front-end chain, not one engine's work), so it cannot
  //       commit an open block itself: the caller does it first (see dft_processor_metal::wait()).
  // Every DFT instance commits on the front-end queue and publishes there, so this drains this
  // engine's own work (and every other front-end commit) - not the back-end stages' command
  // buffers, which run on a queue of their own (see shared_queue::queue_kind).
  return metal::shared_queue::wait_all_committed(metal::shared_queue::queue_kind::front_end);
}

bool dft_metal_engine::set_grid_write_window(const void* window, unsigned nof_entries)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine == nullptr) {
    return false;
  }

  // Clear the table (no write applies a window).
  if (window == nullptr || nof_entries == 0) {
    engine->buf_window = nil;
    engine->has_window = false;
    std::free(engine->window_mem);
    engine->window_mem   = nullptr;
    engine->window_bytes = 0;
    return true;
  }

  // The table is constant for the lifetime of the demodulator, so it is copied into an engine-owned page-aligned
  // buffer (a no-copy wrap needs a page-aligned base covering whole pages) and wrapped once.
  const size_t page     = compat::page_size();
  const size_t bytes    = static_cast<size_t>(nof_entries) * 2 * sizeof(float);
  const size_t rounded  = ((bytes + page - 1) / page) * page;
  void*        previous = engine->window_mem;
  void*        mem      = nullptr;
  if (::posix_memalign(&mem, page, rounded) != 0 || mem == nullptr) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: grid-write window allocation failed");
    return false;
  }
  std::memcpy(mem, window, bytes);
  engine->window_mem   = mem;
  engine->window_bytes = rounded;
  std::free(previous);

  engine->buf_window = wrap_buffer(engine, mem, rounded);
  if (engine->buf_window == nil) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: grid-write window buffer wrap failed");
    engine->has_window = false;
    return false;
  }
  engine->has_window = true;
  return true;
}

bool dft_metal_engine::submit_slot_grid_write(const void* in, void* out, unsigned slot, const grid_write& write)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine == nullptr || dft_resources().pipeline == nil) {
    return false;
  }
  if (slot >= max_batch_slots || write.grid_base == nullptr || write.nof_subc == 0) {
    return false;
  }

  // The buffers cover the whole batch (all slots), as in submit_at(): a ring caller reuses them without re-wrapping.
  const size_t  bytes  = static_cast<size_t>(engine->n) * max_batch_slots * 2 * sizeof(float);
  id<MTLBuffer> b_in   = wrap_buffer(engine, in, bytes);
  id<MTLBuffer> b_out  = wrap_buffer(engine, out, bytes);
  // The grid goes through the cache its CONSUMERS use whenever the block may be handed over (see
  // wrap_grid): the offset is where the grid starts inside that mapping, and it travels to the kernel as
  // the buffer binding's offset - the kernel's own dst_offset stays relative to the grid.
  size_t        grid_off = 0;
  id<MTLBuffer> b_grid   = wrap_grid(engine, write.grid_base, write.grid_bytes, &grid_off);
  if (b_in == nil || b_out == nil || b_grid == nil) {
    return false;
  }

  // Input straight from the radio buffer (see grid_write::time_samples): the WHOLE allocation is
  // wrapped - it is the same pointer and the same length for every symbol of the slot, so the
  // mapping is created once - and the slice's offset travels to the kernel. A slice whose
  // allocation is unknown, or that does not fit in it, is refused: the caller stages its own input.
  struct {
    uint32_t is_ci16;
    uint32_t offset;
    float    gain;
    uint32_t pad;
  } input   = {0u, 0u, 1.0F, 0u};
  id<MTLBuffer> b_in16 = b_in; // a stand-in: the kernel only reads it when is_ci16 = 0
  if (write.time_samples != nullptr) {
    void*  alloc_base = nullptr;
    size_t alloc_size = 0;
    if (!compat::describe_aligned_allocation(write.time_samples, &alloc_base, &alloc_size)) {
      return refuse_time_input(engine, write, "the samples are not in a page-aligned allocation");
    }
    const size_t offset_bytes = static_cast<size_t>(static_cast<const char*>(write.time_samples) -
                                                    static_cast<const char*>(alloc_base));
    const size_t end_bytes =
        offset_bytes + std::max<size_t>(write.time_samples_bytes,
                                        (static_cast<size_t>(write.time_window_start) + engine->n) * 2 * sizeof(int16_t));
    if (end_bytes > alloc_size) {
      return refuse_time_input(engine, write, "the symbol does not fit in the allocation");
    }
    id<MTLBuffer> b = wrap_buffer(engine, alloc_base, alloc_size);
    if (b == nil) {
      return refuse_time_input(engine, write, "wrapping the radio buffer failed");
    }
    b_in16        = b;
    input.is_ci16 = 1u;
    dft_stats_radio_input();
    // The kernel reads from the ALLOCATION base it was handed, so the offset is the slice's own
    // offset plus the window start within it (the cyclic prefix the transform skips).
    input.offset = static_cast<uint32_t>(offset_bytes / (2 * sizeof(int16_t))) + write.time_window_start;
    input.gain   = write.time_gain;
  }

  id<MTLCommandBuffer>         cmd_buf = nil;
  id<MTLComputeCommandEncoder> enc     = nil;
  if (!encode_into(engine, &cmd_buf, &enc)) {
    return false;
  }
  [enc setComputePipelineState:dft_resources().pipeline];
  [enc setBuffer:b_in offset:0 atIndex:0];
  [enc setBuffer:b_out offset:0 atIndex:1];
  [enc setBuffer:engine->buf_tw offset:0 atIndex:2];
  [enc setBuffer:engine->buf_perm offset:0 atIndex:3];
  [enc setBytes:&engine->radix2 length:sizeof(uint32_t) atIndex:4];
  [enc setBytes:&engine->radix3 length:sizeof(uint32_t) atIndex:5];
  [enc setBytes:&engine->inverse length:sizeof(uint32_t) atIndex:6];
  // Element offset of the transform within the input it reads: the ring slot when the input is the
  // engine's float2 batch, and ZERO when it is the radio's buffer - that one holds this transform's
  // samples alone (the RX chain dispatches one transform per symbol), so the slot index does not
  // apply to it.
  const uint32_t base = (input.is_ci16 != 0u) ? 0u : (slot * engine->n);
  [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
  // The grid and its per-element table are only read when the write is active; Metal still requires every buffer the
  // kernel names to be bound, so the transform output and the twiddle table stand in when there is none.
  [enc setBuffer:b_grid offset:grid_off atIndex:8];
  [enc setBuffer:(engine->buf_window != nil ? engine->buf_window : engine->buf_tw) offset:0 atIndex:9];
  [enc setBuffer:b_in16 offset:0 atIndex:11];
  [enc setBytes:&input length:sizeof(input) atIndex:12];

  struct {
    uint32_t active;
    uint32_t nof_subc;
    uint32_t dst_offset;
    uint32_t map_offset;
    float    phase_re;
    float    phase_im;
    uint32_t apply_window;
    uint32_t pad;
  } params = {1u,
              write.nof_subc,
              write.dst_offset,
              write.map_offset % engine->n,
              write.phase_re,
              write.phase_im,
              (write.apply_window && engine->has_window) ? 1u : 0u,
              0u};
  [enc setBytes:&params length:sizeof(params) atIndex:10];

  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min(engine->n, 1024u), 1, 1)];
  if (block_accumulating(engine)) {
    // The block's command buffer stays open: the transforms that arrive with it are encoded together and
    // the commit happens when the block ends (see commit_open()). The slot still records WHICH command
    // buffer carries its transform, so a wait for it commits the block first.
    ++engine->open_transforms;
    note_slot_submission(engine, slot, cmd_buf);
    return true;
  }
  [enc endEncoding];
  commit_front_end(engine, cmd_buf, 1);
  note_slot_submission(engine, slot, cmd_buf);
  return true;
}

void dft_metal_engine::set_lane_slot(uint64_t slot_index)
{
  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine != nullptr) {
    engine->lane_slot     = slot_index;
    engine->has_lane_slot = true;
  }
}

/// NOTE (5.9.65, user ruling A): the front-end fence's accessors and its self-test lived here. They were
/// retired with the mechanism - see the note in ocudu_metal_queue.h. What orders a grid consumer now is the
/// GRID generation (dft_metal_engine::release_block + shared_queue::grid_ready_*), which is a different
/// event with its own counters, printed on the [metal_stats] dft handover line.



bool dft_metal_engine::submit_at(
    const void* in, void* out, unsigned nof_transforms, unsigned first_slot, bool wait_for_completion)
{

  dft_engine_impl* engine = static_cast<dft_engine_impl*>(impl);
  if (engine == nullptr || dft_resources().pipeline == nil) {
    return false;
  }

  // The buffers cover the whole batch (all slots): a ring caller reuses them without re-wrapping.
  const size_t bytes      = static_cast<size_t>(engine->n) * max_batch_slots * 2 * sizeof(float);
  const size_t byte_begin = static_cast<size_t>(engine->n) * first_slot * 2 * sizeof(float);
  id<MTLBuffer> b_in  = wrap_buffer(engine, in, bytes);
  id<MTLBuffer> b_out = wrap_buffer(engine, out, bytes);
  (void)byte_begin;
  if (b_in == nil || b_out == nil) {
    return false;
  }

  id<MTLCommandBuffer>         cmd_buf = nil;
  id<MTLComputeCommandEncoder> enc     = nil;
  if (!encode_into(engine, &cmd_buf, &enc)) {
    return false;
  }
  [enc setComputePipelineState:dft_resources().pipeline];
  [enc setBuffer:b_in offset:0 atIndex:0];
  [enc setBuffer:b_out offset:0 atIndex:1];
  [enc setBuffer:engine->buf_tw offset:0 atIndex:2];
  [enc setBuffer:engine->buf_perm offset:0 atIndex:3];
  [enc setBytes:&engine->radix2 length:sizeof(uint32_t) atIndex:4];
  [enc setBytes:&engine->radix3 length:sizeof(uint32_t) atIndex:5];
  [enc setBytes:&engine->inverse length:sizeof(uint32_t) atIndex:6];
  const uint32_t base = first_slot * engine->n;
  [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
  // The kernel names the radio-input arguments, so every dispatch has to bind them: the float2 input
  // stands in for the int16 one and the flag is off, which is exactly the pre-S-7f-6c behaviour.
  const struct {
    uint32_t is_ci16;
    uint32_t offset;
    float    gain;
    uint32_t pad;
  } input = {0u, 0u, 1.0F, 0u};
  [enc setBuffer:b_in offset:0 atIndex:11];
  [enc setBytes:&input length:sizeof(input) atIndex:12];
  [enc dispatchThreadgroups:MTLSizeMake(nof_transforms, 1, 1)
      threadsPerThreadgroup:MTLSizeMake(std::min(engine->n, 1024u), 1, 1)];
  if (block_accumulating(engine)) {
    // Part of a block: the encoder stays OPEN with the command buffer until the block ends (see
    // commit_open() - ending it here would close the buffer the next transform still has to encode
    // into). A caller that asked for accumulation must not ask for a completion wait on a single
    // transform either: there is nothing committed to wait for yet.
    dft_stats_plain_submit(/*with_block=*/true, engine->has_lane_slot);
    ++engine->open_transforms;
    return true;
  }
  // Its own command buffer: the instrument asks WHY (see dft_stats_t::plain_without_block).
  dft_stats_plain_submit(/*with_block=*/false, engine->has_lane_slot);
  [enc endEncoding];
  commit_front_end(engine, cmd_buf, nof_transforms);
  if (!wait_for_completion) {
    return true;
  }
  [cmd_buf waitUntilCompleted];
  dft_stats_wait();
  if (cmd_buf.status != MTLCommandBufferStatusCompleted) {
    ocudulog::fetch_basic_logger("PHY").error("Metal DFT: command buffer failed with status {}",
                                              static_cast<unsigned long>(cmd_buf.status));
    return false;
  }
  if (cmd_buf.GPUStartTime > 0.0 && cmd_buf.GPUEndTime > 0.0) {
    engine->last_gpu_us = (cmd_buf.GPUEndTime - cmd_buf.GPUStartTime) * 1e6;
  } else {
    engine->last_gpu_us = 0.0;
  }
  return true;
}

bool dft_metal_engine::submit(const void* in, void* out, unsigned nof_transforms)
{
  return submit_at(in, out, nof_transforms, 0, false);
}

bool dft_metal_engine::run(const void* in, void* out, unsigned nof_transforms)
{
  return submit_at(in, out, nof_transforms, 0, true);
}

double dft_metal_engine::last_gpu_wait_us() const
{
  const dft_engine_impl* engine = static_cast<const dft_engine_impl*>(impl);
  return engine != nullptr ? engine->last_gpu_us : 0.0;
}

} // namespace metal
} // namespace ocudu
