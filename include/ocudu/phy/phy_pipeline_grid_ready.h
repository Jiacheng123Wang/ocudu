// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ocudu {

/// \brief What the hand-over actually did, as the registry that performs it counts it.
///
/// A hand-over is INVISIBLE when it works: the grid comes out right whether a block was handed over and
/// claimed, or committed by the front end as it always was. That is what makes it possible for a run to
/// arm the hand-over, exercise nothing at all, and still compare two correct grids - which happened (the
/// armed section of ofdm_demodulator_metal_batch_test ran with a constant `grid_has_host_consumers()`
/// and judged two host-written grids; design document 5.9.19, withdrawn). Anything that means to JUDGE
/// the hand-over therefore has to read these numbers instead of trusting its own output, and that is what
/// this accessor is for: a harness asserts `handed > 0` and `unproduced == 0` before it believes a single
/// one of its own comparisons.
struct grid_handover_counts {
  /// Whether these numbers come from an implementation at all. FALSE in a build or a run without the
  /// hand-over (no Metal engine), where the rest of the fields are all zero - and where arming the
  /// hand-over is a misconfiguration rather than a hand-over that did nothing.
  bool     installed = false;
  /// Deposits made: one per receiving slot's block that was handed over uncommitted.
  uint64_t handed = 0;
  /// Deposits claimed by the device consumer that reads that grid - the healthy path.
  uint64_t taken = 0;
  /// Deposits replaced because the same storage was deposited again (the grid came back through the pool).
  uint64_t superseded = 0;
  /// Deposits dropped for being more than the registry's bound: consumers falling behind producers.
  uint64_t evicted = 0;
  /// Entries evicted BEFORE anyone claimed or produced them. 0 by construction since 5.9.62 - the registry
  /// only erases produced entries - so anything but 0 means that invariant broke.
  uint64_t evicted_unproduced = 0;
  /// Times the registry was over its bound with nothing safe to reclaim (the bound is soft, see 5.9.62).
  uint64_t over_bound = 0;
  /// Deposits a HOST reader found still unclaimed and had to commit itself (grid_ready_hook::wait).
  uint64_t fallback_commits = 0;
  /// Blocks the registry itself committed late, because nobody ever claimed them.
  uint64_t late_commits = 0;
  /// The part of \c late_commits that the TIME deadline claimed, i.e. the blocks the SLOT window did not
  /// cover (Q9, design document 6.10/6.11). The slot window is evaluated first and fires at 2 slots, so on a
  /// leg whose slot counter behaves this stays near zero; a NON-zero value is the registry saying "the slot
  /// rule could not have caught these", which is the shape of a deposit made just before the hyperframe wrap.
  uint64_t late_commits_time = 0;
  /// Reads that found no record for their (storage, slot): they cannot wait, so they prove nothing.
  uint64_t not_found = 0;
  /// Records whose grid has not been produced yet: the blocks still waiting for a consumer or the sweep.
  uint64_t unproduced = 0;
  /// Host waits that timed out - the caller was about to read a grid that was NOT ready.
  uint64_t ready_timeouts = 0;
};

/// \brief Tells a HOST reader of the resource grid to wait until that grid has been produced (D1-A, 5.9.13).
///
/// With the block hand-over the resource grid is produced at the LANE's commit instead of at the end of the
/// receiving slot, so the consumers that read it on the HOST - the PUCCH (format 0/2/3/4 and the format-1
/// collection) and the SRS, none of which has a device view - would read memory nobody has written yet.
/// Measured before this existed: every PUCCH report came out `metric=nan sinr=-inf` and the attach never
/// completed (design document 5.9.12).
///
/// The wait covers BOTH shapes a slot can have, which is why it is not just a fence:
///  * a slot whose grid a hop claimed: the lane commits it, and the wait is for that commit's completion;
///  * a slot NOBODY claimed (a PUCCH-only slot has no PUSCH hop at all): nothing would ever commit it, so
///    the implementation commits it here - the fallback a hand-over owes.
///
/// \note This is a HOOK and not a direct call: the implementation lives with the Metal engines, while the
///       consumers live in the upper PHY, which a build without Metal must still link. No hook installed
///       (every build without the hand-over) means "nothing to wait for", which is exactly the behaviour
///       those builds had before: the grid is produced where it always was.
///
/// \note Call it on the consumer's OWN thread and PROMPTLY - the consumer that reads the grid on the host
///       is already on an executor of its own for this reason. The implementation is non-blocking when
///       nothing is pending, and bounded when something is, so a generation nobody signals cannot hang the
///       caller for good.
class grid_ready_hook
{
public:
  /// \param[in] storage Base of the grid's storage (resource_grid_device_view::base).
  /// \param[in] slot    The RECEIVING SLOT the grid belongs to. It is half of the key, and the half that
  ///                    makes it unambiguous: the storage address is handed back by the grid pool as soon
  ///                    as the next slot's grid arrives, so the address alone would let a late reader be
  ///                    served the NEXT slot's block (design document 5.9.15).
  ///
  /// Waits for the grid at \p storage / \p slot, at most \p timeout_ms. True when the grid is ready (or nothing was
  /// pending), false when the wait timed out - the caller must then NOT trust the grid.
  using wait_fn = bool (*)(const void* storage, uint64_t slot, uint32_t timeout_ms);

  /// Installs the implementation (called by the Metal engines once, on first use).
  static void install(wait_fn fn) { fn_ref().store(fn, std::memory_order_release); }

  /// Whether an implementation is installed (diagnostics and tests).
  static bool installed() { return fn_ref().load(std::memory_order_acquire) != nullptr; }

  /// See the class documentation.
  static bool wait(const void* storage, uint64_t slot, uint32_t timeout_ms = 200)
  {
    wait_fn fn = fn_ref().load(std::memory_order_acquire);
    return (fn == nullptr) || fn(storage, slot, timeout_ms);
  }

  /// Fills \p out with what the hand-over did so far (see grid_handover_counts). Installed by the same
  /// implementation as wait(), and read by whatever has to PROVE the hand-over happened - a harness that
  /// compares its own output cannot tell, because the output is correct either way.
  using counts_fn = void (*)(grid_handover_counts& out);

  static void install_counts(counts_fn fn) { counts_fn_ref().store(fn, std::memory_order_release); }

  static void counts(grid_handover_counts& out)
  {
    counts_fn fn = counts_fn_ref().load(std::memory_order_acquire);
    out          = grid_handover_counts{};
    if (fn != nullptr) {
      fn(out);
    }
  }

  /// \brief The NON-BLOCKING half of wait(): take responsibility for the grid's production and return the
  ///        generation that will mark its completion, WITHOUT waiting for it. 0 = nothing to wait for.
  ///
  /// This is what a DEVICE consumer does - it claims the block (committing it when nobody else will) and
  /// encodes the wait into its own command buffer rather than blocking a thread on it. Exposed here for the
  /// same reason wait() is: the implementation lives with the Metal engines and a harness has to be able to
  /// reach it.
  ///
  /// \note It exists so a harness can reproduce the ONE shape in which the device-side wait is
  ///       load-bearing: a consumer that claims a block another reader is about to read, while the claim's
  ///       commit is still in flight. A host reader that WAITS (wait()) has, by the time it returns,
  ///       already had the production complete - so no ordering is left for anybody else to provide, and an
  ///       arm built on wait() cannot falsify the device-side wait at all (measured, design document
  ///       5.9.39).
  using claim_fn = uint64_t (*)(const void* storage, uint64_t slot);

  static void install_claim(claim_fn fn) { claim_fn_ref().store(fn, std::memory_order_release); }

  static uint64_t claim(const void* storage, uint64_t slot)
  {
    claim_fn fn = claim_fn_ref().load(std::memory_order_acquire);
    return (fn == nullptr) ? 0 : fn(storage, slot);
  }

private:
  static std::atomic<wait_fn>& fn_ref()
  {
    static std::atomic<wait_fn> fn{nullptr};
    return fn;
  }

  static std::atomic<counts_fn>& counts_fn_ref()
  {
    static std::atomic<counts_fn> fn{nullptr};
    return fn;
  }

  static std::atomic<claim_fn>& claim_fn_ref()
  {
    static std::atomic<claim_fn> fn{nullptr};
    return fn;
  }
};

/// \brief The slot's HOP PLAN: how many PUSCH hops read a slot's grid, and which one is starting
///        (D1 multi-PUSCH, design document 5.9.43).
///
/// WHY IT IS NEEDED: the hand-over's registry is keyed by (storage, slot) and a block is committed at the
/// end of the hop that claimed it, so of K hops on one slot's grid only the FIRST can merge with the front
/// end's block and the other K-1 must open a command buffer of their own - a slot's CPU submissions grow
/// from 1 to K. Merging them needs one thing nobody has today: the number of hops that share the slot,
/// known before the first of them commits. The upper PHY is the only place that knows it (`pusch_pdus`
/// in uplink_processor_impl), and the hops of a slot run IN ORDER ON ONE LANE THREAD
/// (max_pusch_and_srs_concurrency is 1), so a plain per-thread record is enough.
///
/// \note This is the same hook pattern as grid_ready_hook, and for the same reason: the upper PHY is plain
///       C++ and a build without Metal must still link. No implementation installed - every build without
///       the hand-over - makes this a no-op.
class slot_hop_plan_hook
{
public:
  /// \param[in] slot      The RECEIVING slot all these hops read (the same half of the key the registry uses).
  /// \param[in] hop_count How many PUSCH hops read this slot's grid.
  /// \param[in] hop_index Which one is about to start, 0-based.
  using set_fn = void (*)(uint64_t slot, unsigned hop_count, unsigned hop_index);

  static void install(set_fn fn) { fn_ref().store(fn, std::memory_order_release); }

  static bool installed() { return fn_ref().load(std::memory_order_acquire) != nullptr; }

  static void set(uint64_t slot, unsigned hop_count, unsigned hop_index)
  {
    set_fn fn = fn_ref().load(std::memory_order_acquire);
    if (fn != nullptr) {
      fn(slot, hop_count, hop_index);
    }
  }

private:
  static std::atomic<set_fn>& fn_ref()
  {
    static std::atomic<set_fn> fn{nullptr};
    return fn;
  }
};

/// \brief Whether this run asks the receiving chain to HAND OVER its block instead of committing it
///        (D1's knob, \c OCUDU_DFT_RELEASE_BLOCK).
///
/// ONE definition, because four places ask this question and must never disagree: the DFT engine's own
/// decision (dft_metal_engine's block_release_requested()), the \c armed= field of the counter line that
/// engine prints, the OFDM demodulator's startup warning (the operator's half - it compares the knob
/// against the chain's permission so that a leg cannot spend an OTA cycle exercising nothing), and the
/// burst's D1 trace. Before this they each read the variable themselves: four copies of one default, i.e.
/// four chances to flip three of them.
///
/// ---- The DEFAULT is ON, and moved here from OFF ----
/// Judged by the \c s46 controlled leg pair (same load, back to back; design document 5.9.49): armed, a
/// hop costs **1.00** CPU submissions instead of **2.674** and its MEDIAN end-to-end latency improves
/// (2009.4 -> 1878.3 us), at the price of a longer tail (p95 2865.7 -> 5303.5 us). The submission count is
/// this line's criterion (README 2.1), so the hand-over is the production path; the tail is a by-product
/// that has to stay acceptable rather than a reason to refuse - and it did: \c dropped=0 and zero RF
/// real-time failures on both legs of that pair.
///
/// \c OCUDU_DFT_RELEASE_BLOCK=0 is the one-line retreat, and it is what the CONTROL arm of every A/B must
/// set: leaving it unset no longer means "off", so an unset control arm would silently become a second
/// candidate arm.
///
/// \return True to hand the block over.
///
/// \note Still read on EVERY call rather than cached, because the unit tests arm and disarm it around the
///       arms they compare (see dft_release_adopt_metal_test).
inline bool grid_handover_armed()
{
  const char* env = std::getenv("OCUDU_DFT_RELEASE_BLOCK");
  if (env == nullptr) {
    return true;
  }
  // A value that is not a number is a TYPO, and a typo must not silently pick a path - the precedent is
  // OCUDU_CE_LANE_ORDER's own warning. It is said once, and the default is used rather than the value.
  char*               end   = nullptr;
  const unsigned long value = std::strtoul(env, &end, 10);
  if ((end == env) || (*end != '\0')) {
    static const bool warned = []() {
      std::fprintf(stderr,
                   "[phy_pipeline] OCUDU_DFT_RELEASE_BLOCK is not a number - using the default (armed). "
                   "Use 0 to run the control arm.\n");
      return true;
    }();
    (void)warned;
    return true;
  }
  return value != 0;
}

} // namespace ocudu
