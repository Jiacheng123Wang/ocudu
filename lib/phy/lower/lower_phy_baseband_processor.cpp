// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lower_phy_baseband_processor.h"
#include "ocudu/adt/format.h"
#include "ocudu/adt/interval.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_reader_view.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_writer_view.h"
#include "ocudu/instrumentation/traces/ru_traces.h"
#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu/phy/phy_pipeline_report.h"
// fetch_basic_logger(): used by the shutdown path of ul_process (a refused uplink task) as well as by the
// flow probe, so the include is not tied to OCUDU_FLOW_PROBES any more. It used to be, and the macOS build
// still compiled because the logger header arrived transitively there - the Linux/GCC build is the one that
// caught it (the warning did not exist before the S-7g-13 shutdown path).
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/slot_point_extended.h"
#include "ocudu/support/executors/thread_utils.h" // cpu_relax()
#include "ocudu/support/executors/ul_pipeline_probe.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <mutex>
#include <numeric>
#include <vector>

using namespace ocudu;

namespace {

/// \brief dev doc 6.41 (V3's S2 step): HOW MUCH TIME THE TRANSMIT HAND-OVER HAD LEFT, in host microseconds.
///
/// WHY IT EXISTS. V3's criterion ("RF real-time failures <= 10") is the only one still red, and its 700-1500
/// events per leg are UHD's TX-side real-time failures (`Real-time failure in RF: underflow` / `late`). They
/// scale with load and have nothing to do with the uplink's receive pool (`gaps = 0`, `pop_blocking` max 23 us,
/// `starved_events = 0` on the same leg, dev doc 6.40). The uplink side has a whole family of probes for its own
/// lateness; the transmit side had NONE, so any change there could only be judged by "the failure count fell" -
/// and that count moves 700-1500 across identical recipes.
///
/// WHAT IT MEASURES, AND WHY IT TAKES TWO CLOCKS. dl_process() hands the radio one slot's samples with
/// `metadata.ts = timestamp + tx_time_offset`, and an `underflow` means the host did that too late in HOST time.
/// Radio time alone cannot say that: `(due_ts - last_rx_ts)` is a radio interval, but "how long do I still
/// have" is a host interval. The probe therefore keeps the map between the two clocks from the receive path -
/// the newest (radio timestamp, host instant) pair `receiver.receive()` produced - and measures
///
///     margin_us = (due_ts - last_rx_ts) / rate  -  (host_now - last_rx_host)
///
/// i.e. the radio time still left before those samples must be on the air, converted to host microseconds, with
/// the radio's own receive buffering removed (both terms contain it, so it cancels). `margin <= 0` means the
/// hand-over happened AT OR AFTER the deadline: the host-side shape of an `underflow`.
///
/// \note The distribution is the point, not the minimum: "the transmit path lives 200 us from the deadline all
///       the time" and "it is usually 2 ms ahead and occasionally 3 ms late" are different defects, and only the
///       percentiles tell them apart.
///
/// \note WHAT A UNIT FIXTURE READS IS NOT A READING OF THIS QUANTITY. In `lower_phy_test` the "radio" is a mock
///       whose receive timestamps and whose host pacing have no fixed relation (there is no sample clock to be
///       late against), so its margins come out negative in bulk - measured: mean -1173 us, 651 of 999 at or
///       below 0. The probe is there to be read ON AIR, against the same leg's `Real-time failure in RF` count.
struct tx_slack_accounting {
  std::atomic<uint64_t> transmissions{0};
  /// Hand-overs whose remaining margin was below these thresholds (the tail at a glance).
  std::atomic<uint64_t> below_2ms{0};
  std::atomic<uint64_t> below_1ms{0};
  std::atomic<uint64_t> below_500us{0};
  /// Hand-overs made AT OR AFTER their deadline. This is the number an `underflow` should correspond to: a leg
  /// with `AT/BELOW 0 = 0` here and UHD failures in its log says the lateness is INSIDE the radio or its
  /// driver, not in this hand-over.
  std::atomic<uint64_t> late{0};
  std::atomic<int64_t>  min_us{std::numeric_limits<int64_t>::max()};
  /// \name dev doc 6.42 (4), S2b: HOW LONG THE transmit() CALL ITSELF TAKES.
  ///
  /// The margin above says the hand-over is EARLY (median ~1.5 ms), yet the leg reports ~1000 UHD underflows:
  /// so the lateness is after the hand-over, and the next question is WHOSE. If `transmit()` itself blocks for
  /// milliseconds, the radio or the USB link is pushing back inside the call (and no host scheduling change can
  /// help); if it returns at once, the samples are sitting in UHD's own queue and its worker thread is the one
  /// that is late - which is what CPU contention from the fused lane would look like.
  ///@{
  std::atomic<uint64_t> tx_over_1ms{0};
  std::atomic<uint64_t> tx_over_5ms{0};
  std::atomic<int64_t>  tx_call_max_us{0};
  std::mutex            tx_call_mutex;
  std::vector<float>    tx_call_us;
  ///@}
  /// The transmit timestamp the smallest margin belonged to (the metadata carries no slot index).
  std::atomic<uint64_t> min_due_ts{0};
  /// \name The clock map: the newest (radio timestamp, host instant) pair the receive path delivered.
  /// Written by the receive thread once per receive, read by the transmit thread once per transmit.
  ///@{
  std::atomic<uint64_t> rx_ts{0};
  std::atomic<int64_t>  rx_host_ns{0};
  std::atomic<bool>     rx_valid{false};
  ///@}
  /// The samples of the distribution (capped: 2 M slots is ~17 minutes at 30 kHz, far beyond any leg).
  std::mutex         mutex;
  std::vector<float> us;
  static constexpr size_t max_samples = 2u * 1000u * 1000u;
};

tx_slack_accounting& tx_slack_accounts()
{
  // NEVER DESTROYED ON PURPOSE, for the reason rx_pool_accounts() spells out: the report is an atexit handler.
  static tx_slack_accounting* a = new tx_slack_accounting();
  return *a;
}

/// Records the clock map from the receive path (see tx_slack_accounting). Called after receiver.receive().
void tx_slack_note_receive(uint64_t radio_ts, int64_t host_ns)
{
  tx_slack_accounting& a = tx_slack_accounts();
  a.rx_host_ns.store(host_ns, std::memory_order_relaxed);
  a.rx_ts.store(radio_ts, std::memory_order_relaxed);
  a.rx_valid.store(true, std::memory_order_release);
}

/// Records one transmit hand-over (dev doc 6.41). Called from dl_process() on the TX executor.
void tx_slack_note_transmit(int64_t margin_us, uint64_t due_ts)
{
  tx_slack_accounting& a = tx_slack_accounts();
  a.transmissions.fetch_add(1, std::memory_order_relaxed);
  if (margin_us < 2000) {
    a.below_2ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (margin_us < 1000) {
    a.below_1ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (margin_us < 500) {
    a.below_500us.fetch_add(1, std::memory_order_relaxed);
  }
  if (margin_us <= 0) {
    a.late.fetch_add(1, std::memory_order_relaxed);
  }
  int64_t prev = a.min_us.load(std::memory_order_relaxed);
  while ((margin_us < prev) && !a.min_us.compare_exchange_weak(prev, margin_us, std::memory_order_relaxed)) {
  }
  if (margin_us == a.min_us.load(std::memory_order_relaxed)) {
    a.min_due_ts.store(due_ts, std::memory_order_relaxed);
  }
  std::lock_guard<std::mutex> lock(a.mutex);
  if (a.us.size() < tx_slack_accounting::max_samples) {
    a.us.push_back(static_cast<float>(margin_us));
  }
}

/// Records how long the transmit() call took (dev doc 6.42 (4)).
void tx_slack_note_call_us(int64_t call_us)
{
  tx_slack_accounting& a = tx_slack_accounts();
  if (call_us > 1000) {
    a.tx_over_1ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (call_us > 5000) {
    a.tx_over_5ms.fetch_add(1, std::memory_order_relaxed);
  }
  int64_t prev = a.tx_call_max_us.load(std::memory_order_relaxed);
  while ((call_us > prev) && !a.tx_call_max_us.compare_exchange_weak(prev, call_us, std::memory_order_relaxed)) {
  }
  std::lock_guard<std::mutex> lock(a.tx_call_mutex);
  if (a.tx_call_us.size() < tx_slack_accounting::max_samples) {
    a.tx_call_us.push_back(static_cast<float>(call_us));
  }
}

/// The report: printed at exit and on demand with the other P0 readings (see register_p0_report).
void tx_slack_report()
{
  tx_slack_accounting& a = tx_slack_accounts();
  const uint64_t       n = a.transmissions.load(std::memory_order_relaxed);
  if (n == 0) {
    return; // a run that never transmitted (the unit fixtures) stays silent
  }
  std::vector<float> us;
  {
    std::lock_guard<std::mutex> lock(a.mutex);
    us = a.us;
  }
  std::sort(us.begin(), us.end());
  const auto pct = [&us](double p) { return us.empty() ? 0.0F : us[static_cast<size_t>((us.size() - 1) * p)]; };
  const double mean =
      us.empty() ? 0.0 : std::accumulate(us.begin(), us.end(), 0.0) / static_cast<double>(us.size());
  std::fprintf(stderr,
               "[dl_tx_slack] transmissions=%llu mean=%.1fus median=%.1fus p1=%.1fus p5=%.1fus p25=%.1fus "
               "min=%lldus (due_ts=%llu); below 2ms=%llu, below 1ms=%llu, below 500us=%llu, AT/BELOW 0=%llu\n",
               static_cast<unsigned long long>(n),
               mean,
               static_cast<double>(pct(0.5)),
               static_cast<double>(pct(0.01)),
               static_cast<double>(pct(0.05)),
               static_cast<double>(pct(0.25)),
               static_cast<long long>(a.min_us.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.min_due_ts.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.below_2ms.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.below_1ms.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.below_500us.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.late.load(std::memory_order_relaxed)));
  // S2b (dev doc 6.42 (4)): the CALL's own duration, so "the radio pushed back inside transmit()" and "UHD's
  // worker was late after an instant return" are told apart by one number.
  std::vector<float> calls;
  {
    std::lock_guard<std::mutex> lock(a.tx_call_mutex);
    calls = a.tx_call_us;
  }
  std::sort(calls.begin(), calls.end());
  const auto cpct = [&calls](double p) {
    return calls.empty() ? 0.0F : calls[static_cast<size_t>((calls.size() - 1) * p)];
  };
  std::fprintf(stderr,
               "[dl_tx_call] calls=%llu median=%.1fus p95=%.1fus p99=%.1fus max=%lldus; over 1ms=%llu, "
               "over 5ms=%llu\n",
               static_cast<unsigned long long>(calls.size()),
               static_cast<double>(cpct(0.5)),
               static_cast<double>(cpct(0.95)),
               static_cast<double>(cpct(0.99)),
               static_cast<long long>(a.tx_call_max_us.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.tx_over_1ms.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.tx_over_5ms.load(std::memory_order_relaxed)));
}

const bool tx_slack_report_registered = []() {
  std::atexit(tx_slack_report);
  register_p0_report(tx_slack_report); // dev doc 6.24: joins the on-demand stall dump
  return true;
}();

/// Receive-buffer accounting (see lower_phy_baseband_processor::rx_pool_note_taken).
///
/// The number that matters is `held = taken - returned`: a run that starves the radio holds it at the pool size.
/// That happens for a WHOLE LEG, not for an instant - measured on `s52-residency` (2026-09-22): 18,409 takes of
/// 179,495 found the pool at 1 or 0 free buffers, held at 7 of 8 for the last 28% of the run. Printing one line
/// per take while "nearly dry" therefore degenerated into 1.1 MB of stderr, and made the receive thread pay a
/// write to a pipe per take at the exact moment it was furthest behind.
///
/// So the events are COUNTED here and summarised ONCE at shutdown (rx_pool_report), and the per-take timeline
/// goes to the "PHY" logger at DEBUG level: off at the default info level, and available with
/// `--log.phy_level debug` when the timeline itself is the question.
struct rx_pool_accounting {
  std::atomic<uint64_t> taken{0};
  std::atomic<uint64_t> returned{0};
  /// Takes that found the pool nearly dry (one or zero free buffers).
  std::atomic<uint64_t> starved_takes{0};
  /// Entries INTO that state, i.e. runs of starvation rather than takes. This is the count that separates "the
  /// pool was starved once, for a minute" from "it was starved a thousand times for an instant" - the summary
  /// above cannot tell those apart, and the two say different things about the pipeline behind the pool.
  std::atomic<uint64_t> starved_events{0};
  /// Whether the previous take was nearly dry, for starved_events. Only this thread's transitions matter, and a
  /// relaxed atomic is enough: the counter is read at shutdown, not used to order anything.
  std::atomic<bool> was_starved{false};
  /// Whether the pool has ever been EMPTY (zero free buffers), for the one-shot warning below. Empty is a
  /// different event from "nearly dry": the next take has nothing to take, so pop_blocking() blocks the receive
  /// thread and the radio is LATE rather than merely tight (5.9.100).
  std::atomic<bool> warned_empty{false};
  /// \name Fix B (dev doc 6.26): the blocks a DRY pool dropped instead of parking the radio.
  ///
  /// `dropped` counts them; `drop_park_max_us` is the longest wait that ended in a drop. Both are read with the
  /// pool summary at exit: a leg with `dropped > 0` is a leg whose pipeline went dry (the same event
  /// `starved_events` counts), and the drop is what kept that from becoming lost samples (`gaps`).
  ///@{
  std::atomic<uint64_t> dropped{0};
  std::atomic<uint64_t> drop_park_max_us{0};
  ///@}
  /// Largest `held` and smallest free count seen. For ONE pool the two agree by construction - the queue holds
  /// `free` of the `pool_size` buffers it was filled with, so `held = taken - returned = pool_size - free` - and
  /// printing both lets a reader check that identity instead of trusting it. It does NOT hold across pools:
  /// these counters are process-global while the pool is per-sector (and, in `lower_phy_test`, per fixture), so
  /// that test reports `held_max` far above `pool` with tens of buffers never returned, which is the fixture
  /// swapping pools under one set of counters and not a leak. Measured there: held_max=241, pool=8, free_min=5.
  std::atomic<uint64_t> held_max{0};
  std::atomic<size_t>   free_min{std::numeric_limits<size_t>::max()};
  std::atomic<size_t>   pool_size{0};

  /// \brief P0-2: HOW LONG the receive thread waited for a buffer (`pop_blocking()`), per take.
  ///
  /// The one wait `[ul_rx_wait]` cannot see: that series brackets `receiver.receive()` and the take happens
  /// BEFORE it (it is the first line of ul_process()), so a receive thread parked on an empty pool shows up
  /// nowhere in the probe report today - measured on `s88-laneconc2`: the pool went EMPTY, the take blocked
  /// for 5.002 s, the USRP's queue overflowed and the leg lost 2 x ~5 s of samples, while `[ul_rx_wait]` read
  /// its usual ~101 ms maximum. Sampled per take (a 200 s leg takes ~200k times, ~1.6 MB) so the report can
  /// print real percentiles and the tail counts that matter: how many takes waited more than 1 ms / 10 ms /
  /// 100 ms / 1 s.
  std::mutex         wait_mutex;
  std::vector<float> wait_us;
  /// Takes that waited longer than the thresholds above, accumulated as they happen (the vector is for the
  /// percentiles; these are the counters a reader scans for first).
  std::atomic<uint64_t> waits_over_1ms{0};
  std::atomic<uint64_t> waits_over_10ms{0};
  std::atomic<uint64_t> waits_over_100ms{0};
  std::atomic<uint64_t> waits_over_1s{0};
};

rx_pool_accounting& rx_pool_accounts()
{
  // NEVER DESTROYED ON PURPOSE, for the reason the DFT engine's stats() spells out: rx_pool_report() is an
  // atexit handler and runs AFTER the static destructors, and P0-2 put a `std::mutex` (and a vector) in this
  // struct - locking a destroyed mutex is `mutex lock failed: Invalid argument`, i.e. an abort at exit.
  static rx_pool_accounting* accounts = new rx_pool_accounting();
  return *accounts;
}

/// Prints the receive-buffer pool accounting ONCE, next to the other receive counters (see ul_rx_stats_report).
///
/// Registered unconditionally, because rx_pool_note_taken() is not behind a build flag, and silent for a run
/// that never received a block - a leg that never started on air must not print a line of zeroes that reads
/// like a measurement.
void rx_pool_report()
{
  // NOT const: P0-2's wait distribution lives behind a mutex (the takes happen on the radio thread) and a const
  // reference cannot be locked. Nothing here writes to the accounting.
  rx_pool_accounting&       a     = rx_pool_accounts();
  const uint64_t            taken = a.taken.load(std::memory_order_relaxed);
  if (taken == 0) {
    return;
  }
  const uint64_t back     = a.returned.load(std::memory_order_relaxed);
  const size_t   free_min = a.free_min.load(std::memory_order_relaxed);
  std::fprintf(stderr,
               "[ul_rx_pool] taken=%llu returned=%llu held_end=%lld held_max=%llu pool=%zu free_min=%lld "
               "starved_takes=%llu starved_events=%llu dropped=%llu drop_park_max=%lluus\n",
               static_cast<unsigned long long>(taken),
               static_cast<unsigned long long>(back),
               static_cast<long long>(taken - back),
               static_cast<unsigned long long>(a.held_max.load(std::memory_order_relaxed)),
               a.pool_size.load(std::memory_order_relaxed),
               (free_min == std::numeric_limits<size_t>::max()) ? -1LL : static_cast<long long>(free_min),
               static_cast<unsigned long long>(a.starved_takes.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.starved_events.load(std::memory_order_relaxed)),
               // Fix B (dev doc 6.26): blocks a DRY pool dropped instead of parking the radio.
               static_cast<unsigned long long>(a.dropped.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.drop_park_max_us.load(std::memory_order_relaxed)));

  // P0-2: the wait for a buffer. Its OWN line, because the pool's line above is a census and this one is a
  // distribution - and because the number that matters (the longest park on an empty pool) has to be readable
  // without arithmetic. `over 1s=` is the one to look at first: a non-zero count there is a receive stall that
  // nothing else in the report shows.
  {
    std::vector<float> waits;
    {
      std::lock_guard<std::mutex> lock(a.wait_mutex);
      waits = a.wait_us;
    }
    if (!waits.empty()) {
      std::sort(waits.begin(), waits.end());
      const auto pct = [&waits](double p) { return waits[static_cast<size_t>((waits.size() - 1) * p)]; };
      double     sum = 0.0;
      for (float w : waits) {
        sum += w;
      }
      std::fprintf(stderr,
                   "[ul_rx_pool] pop_blocking wait (P0-2): takes=%zu mean=%.1fus median=%.1fus p95=%.1fus "
                   "p99=%.1fus max=%.1fus; over 1ms=%llu, over 10ms=%llu, over 100ms=%llu, over 1s=%llu\n",
                   waits.size(),
                   sum / static_cast<double>(waits.size()),
                   pct(0.5),
                   pct(0.95),
                   pct(0.99),
                   waits.back(),
                   static_cast<unsigned long long>(a.waits_over_1ms.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(a.waits_over_10ms.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(a.waits_over_100ms.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(a.waits_over_1s.load(std::memory_order_relaxed)));
    }
  }
}

const bool rx_pool_report_registered = []() {
  std::atexit(rx_pool_report);
  register_p0_report(rx_pool_report); // dev doc 6.24: joins the on-demand stall dump
  return true;
}();

} // namespace

void lower_phy_baseband_processor::rx_pool_note_wait(int64_t wait_us)
{
  if (wait_us < 0) {
    return;
  }
  rx_pool_accounting& a = rx_pool_accounts();
  {
    std::lock_guard<std::mutex> lock(a.wait_mutex);
    a.wait_us.push_back(static_cast<float>(wait_us));
  }
  const double us = static_cast<double>(wait_us);
  if (us > 1000.0) {
    a.waits_over_1ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (us > 10000.0) {
    a.waits_over_10ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (us > 100000.0) {
    a.waits_over_100ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (us > 1000000.0) {
    a.waits_over_1s.fetch_add(1, std::memory_order_relaxed);
  }
}

void lower_phy_baseband_processor::rx_pool_note_dropped(uint64_t park_us)
{
  rx_pool_accounting& a = rx_pool_accounts();
  a.dropped.fetch_add(1, std::memory_order_relaxed);
  uint64_t prev = a.drop_park_max_us.load(std::memory_order_relaxed);
  while ((park_us > prev) && !a.drop_park_max_us.compare_exchange_weak(prev, park_us, std::memory_order_relaxed)) {
  }
}

void lower_phy_baseband_processor::rx_pool_note_taken(size_t free_buffers, size_t pool_size)
{
  rx_pool_accounting& a    = rx_pool_accounts();
  const uint64_t      took = a.taken.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t      back = a.returned.load(std::memory_order_relaxed);
  const uint64_t      held = took - back;

  // Nearly dry: one free buffer or none. The state is COUNTED (see rx_pool_accounting) - the interesting number
  // is `held = taken - returned`, and a run that starves the radio holds it at the pool size.
  const bool nearly_dry = (free_buffers <= 1);
  if (nearly_dry) {
    a.starved_takes.fetch_add(1, std::memory_order_relaxed);
    if (!a.was_starved.exchange(true, std::memory_order_relaxed)) {
      a.starved_events.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    a.was_starved.store(false, std::memory_order_relaxed);
  }
  a.pool_size.store(pool_size, std::memory_order_relaxed);
  if (held > a.held_max.load(std::memory_order_relaxed)) {
    a.held_max.store(held, std::memory_order_relaxed);
  }
  if (free_buffers < a.free_min.load(std::memory_order_relaxed)) {
    a.free_min.store(free_buffers, std::memory_order_relaxed);
  }

  // EMPTY, not "nearly dry": with zero free buffers the next take blocks the receive thread, which is the
  // event that turns pool pressure into a LATE radio rather than a tight one. Warned once per run at WARNING
  // level, because the leg this was written for (s63-heavy-ul) drained the pool completely and nothing in the
  // run said so until the shutdown summary - the summary carries the counts, this carries the ALARM
  // (5.9.100: 155 nearly-dry takes in 51 episodes, free_min=0, held_max=pool=8).
  if ((free_buffers == 0) && !a.warned_empty.exchange(true, std::memory_order_relaxed)) {
    ocudulog::fetch_basic_logger("PHY").warning(
        "[ul_rx_pool] the receive pool is EMPTY (held={}/{}): the next take blocks the receive thread",
        held,
        pool_size);
  }

  // The same events as before - every 1024 pops, and every pop while the pool is nearly dry - but on the logger
  // at DEBUG level rather than on stderr. `enabled()` is checked first so that a run at the default level pays
  // one load per pop and nothing else. NOTE: while the pool IS nearly dry this is still one line per pop (that
  // is what a debug timeline is), which at one pop per millisecond is a megabyte a minute - ask for it on
  // purpose, and read the summary in rx_pool_report() otherwise.
  if (!nearly_dry && ((took % 1024) != 0)) {
    return;
  }
  auto& logger = ocudulog::fetch_basic_logger("PHY");
  if (logger.debug.enabled()) {
    logger.debug("[ul_rx_pool] taken={} returned={} held={} free={}/{}", took, back, held, free_buffers, pool_size);
  }
}

void lower_phy_baseband_processor::rx_pool_note_return()
{
  rx_pool_accounts().returned.fetch_add(1, std::memory_order_relaxed);
}

#if defined(OCUDU_METAL_STATS)
/// \brief Continuity of the sample stream the radio delivers (see ul_process).
///
/// Consecutive receive blocks must be adjacent in time: the second block starts exactly where the first
/// ended. A gap means the radio lost (or repeated) samples, and it is the one measurement that says
/// whether the receive side may ask for the block sizes it asks for at all - the size of a request is
/// an implementation detail of the transport, the continuity of what comes back is not. It is also what
/// a slice-based uplink depends on: a gap makes the timestamps disagree, the FSM re-aligns, and the
/// symbols that straddle it are assembled (see the "host sample assembly" check).
struct ul_rx_stats {
  std::atomic<uint64_t> blocks{0};
  std::atomic<uint64_t> samples{0};
  std::atomic<uint64_t> gaps{0};
  std::atomic<uint64_t> gap_samples{0};
  /// Blocks the radio returned with timestamp 0 (its stop/error path, see ul_process): not part of the
  /// stream, and not a discontinuity.
  std::atomic<uint64_t> ts0_blocks{0};
  /// \name dev doc 6.51: the radio's OWN verdict on the blocks it delivered (see rx_error).
  ///
  /// Why it is here. The continuity check above can say THAT the stream broke, not whether the radio dropped
  /// the samples or the host merely received them out of order. UHD knows: it classifies every receive, and
  /// on the air legs the classification and the gap count are the same event - `overflow` is the receive ring
  /// filling up because nobody drained it in time, and across eleven legs its count equalled the gap count
  /// event for event. The verdict used to produce a warning line and nothing else, which left the attribution
  /// to whoever was willing to line up log timestamps by hand.
  ///@{
  std::atomic<uint64_t> rx_overflows{0};
  std::atomic<uint64_t> rx_lates{0};
  std::atomic<uint64_t> rx_other{0};
  ///@}
  /// \name The discontinuities themselves, in µs and in order.
  ///
  /// A gap is a RARE event (0-3 per leg), so the list is kept whole instead of summarised: the sizes are what
  /// separates "one slot's worth" from "the ring drained", and only the FIRST one is logged when it happens.
  ///@{
  static constexpr unsigned max_gaps = 16;
  std::atomic<uint64_t>     gap_us[max_gaps]{};
  std::atomic<unsigned>     gap_us_n{0};
  ///@}
  /// \name The receive-side timing (dev doc 6.51): where the margin went, and what the host was doing when the
  /// radio gave up. These are the receive twin of `[dl_tx_slack]`/`[dl_tx_call]`, and they exist to tell the
  /// two hypotheses apart with one leg: a transport that blocks INSIDE the call (USB/radio push-back) against
  /// a host that is late to ASK (its own scheduling, the fused lane's host threads included).
  ///@{
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> recv_over_1ms{0};
  std::atomic<uint64_t> recv_over_5ms{0};
  std::atomic<int64_t>  recv_max_us{0};
  std::atomic<uint64_t> loop_over_1ms{0};
  std::atomic<uint64_t> loop_over_5ms{0};
  std::atomic<int64_t>  loop_max_us{0};
  std::atomic<uint64_t> slip_over_1ms{0};
  std::atomic<int64_t>  slip_max_us{0};
  /// The host instant the previous receive() returned, i.e. where the LOOP time is measured from.
  std::atomic<int64_t> last_return_ns{0};
  /// The context of the first overflows: the transport call and the loop that ended in one (see
  /// ul_rx_note_call). THIS is the reading that decides host against USB.
  static constexpr unsigned max_ovf_ctx = 8;
  std::atomic<uint64_t>     ovf_recv_us[max_ovf_ctx]{};
  std::atomic<uint64_t>     ovf_loop_us[max_ovf_ctx]{};
  /// The system load average (1 minute, x100) at the moment of each of those overflows, and the largest load
  /// seen at ANY tail event of the leg (dev doc 6.54, arm C).
  ///
  /// Why the load is part of an event's context: the receive path's own thread is never late (loop_us is ~2 us
  /// at every overflow, dev doc 6.53), so when the transport call blocks for milliseconds the suspicion moves
  /// to the threads this code does not own - UHD's own receive worker and the USB stack. A saturated host is
  /// what would starve them, and a device-level stall is what would not; the load at the moment of the event is
  /// therefore the cheapest thing that tells the two apart, and it rides in the event's own log line instead of
  /// needing an external sampler correlated by hand. Sampled ONLY on a tail event (a millisecond call, a
  /// millisecond loop or a radio error), so the steady-state cost is zero.
  std::atomic<uint64_t>     ovf_load1_x100[max_ovf_ctx]{};
  std::atomic<uint64_t>     load1_at_tail_max_x100{0};
  std::atomic<unsigned>     ovf_ctx_n{0};
  ///@}
};

ul_rx_stats& ul_rx_counters()
{
  static ul_rx_stats s;
  return s;
}

/// The host's 1-minute load average, in hundredths (dev doc 6.54, arm C).
///
/// It is read only where an event needs a context (see ul_rx_note_call), and it is the reading that separates
/// the two owners a millisecond transport call can have: a saturated host starves the threads this code does
/// not own (UHD's own receive worker, the USB stack), while a device or wire stall happens with the host idle.
int64_t ul_rx_load1_x100()
{
  double loads[1] = {0.0};
  if (getloadavg(loads, 1) != 1) {
    return -1;
  }
  return static_cast<int64_t>(loads[0] * 100.0);
}

/// Records one `receiver.receive()` call (dev doc 6.51): how long the transport call took, how long the receive
/// thread spent OUTSIDE it since the previous one, the drift the two add up to, and the radio's own verdict.
///
/// \param[in] begin_ns   Host instant the call was issued.
/// \param[in] return_ns  Host instant it returned.
/// \param[in] air_us     Air time of the samples the call asked for (the block length at the sample rate).
/// \param[in] error      What the radio reported about the block (see baseband_gateway_receiver::rx_error).
///
/// The three derived numbers, and what each one rules in:
///   * RECV = \p return_ns - \p begin_ns. Large (milliseconds) means the call itself blocked - the radio or
///     the USB link pushed back inside it, and no host scheduling change can help - which is exactly the
///     question `[dl_tx_call]` answered on the transmit side.
///   * LOOP = this call's begin minus the PREVIOUS call's return: the host's own work plus whatever the OS did
///     to the thread. Large means the host was late to ASK, i.e. scheduling (the fused lane's host threads).
///   * SLIP = LOOP + RECV - \p air_us: the drift of the host against the sample timeline for this iteration.
///     Positive and sustained means the host is falling behind the radio, which is the state that ends in the
///     ring overflowing.
///
/// \note WHAT A UNIT FIXTURE READS IS NOT A READING OF THIS QUANTITY, for the reason the transmit-side probe
///       spells out: in `lower_phy_test` the "radio" is a mock driven by hand, so its blocks arrive when the
///       test says so and its `current_timestamp` has no relation to a sample clock - measured there: recv
///       max 696 us with loop max 1503 us over 318 blocks, which says what the fixture does, nothing about a
///       radio. The probe is to be read ON AIR, next to the same leg's `[RF] ... overflow` count.
void ul_rx_note_call(int64_t begin_ns, int64_t return_ns, int64_t air_us, baseband_gateway_receiver::rx_error error)
{
  ul_rx_stats& c = ul_rx_counters();
  c.calls.fetch_add(1, std::memory_order_relaxed);

  const int64_t recv_us = (return_ns - begin_ns) / 1000;
  if (recv_us > 1000) {
    c.recv_over_1ms.fetch_add(1, std::memory_order_relaxed);
  }
  if (recv_us > 5000) {
    c.recv_over_5ms.fetch_add(1, std::memory_order_relaxed);
  }
  int64_t prev = c.recv_max_us.load(std::memory_order_relaxed);
  while ((recv_us > prev) && !c.recv_max_us.compare_exchange_weak(prev, recv_us, std::memory_order_relaxed)) {
  }

  // The loop time needs the previous call's return instant. exchange() makes it one atomic per call, and the
  // first call of a run has no predecessor (its loop time is not a measurement and is not recorded).
  const int64_t last_return_ns = c.last_return_ns.exchange(return_ns, std::memory_order_relaxed);
  if (last_return_ns != 0) {
    const int64_t loop_us = (begin_ns - last_return_ns) / 1000;
    if (loop_us > 1000) {
      c.loop_over_1ms.fetch_add(1, std::memory_order_relaxed);
    }
    if (loop_us > 5000) {
      c.loop_over_5ms.fetch_add(1, std::memory_order_relaxed);
    }
    prev = c.loop_max_us.load(std::memory_order_relaxed);
    while ((loop_us > prev) && !c.loop_max_us.compare_exchange_weak(prev, loop_us, std::memory_order_relaxed)) {
    }
    const int64_t slip_us = loop_us + recv_us - air_us;
    if (slip_us > 1000) {
      c.slip_over_1ms.fetch_add(1, std::memory_order_relaxed);
    }
    prev = c.slip_max_us.load(std::memory_order_relaxed);
    while ((slip_us > prev) && !c.slip_max_us.compare_exchange_weak(prev, slip_us, std::memory_order_relaxed)) {
    }
  }

  switch (error) {
    case baseband_gateway_receiver::rx_error::none:
      break;
    case baseband_gateway_receiver::rx_error::late:
      c.rx_lates.fetch_add(1, std::memory_order_relaxed);
      break;
    case baseband_gateway_receiver::rx_error::overflow: {
      c.rx_overflows.fetch_add(1, std::memory_order_relaxed);
      // The context of the first few: what the call and the loop looked like when the radio dropped samples,
      // and how loaded the host was (see ovf_load1_x100).
      const unsigned idx = c.ovf_ctx_n.fetch_add(1, std::memory_order_relaxed);
      if (idx < ul_rx_stats::max_ovf_ctx) {
        c.ovf_recv_us[idx].store(static_cast<uint64_t>(recv_us), std::memory_order_relaxed);
        c.ovf_loop_us[idx].store(static_cast<uint64_t>((last_return_ns != 0) ? (begin_ns - last_return_ns) / 1000 : 0),
                                 std::memory_order_relaxed);
        c.ovf_load1_x100[idx].store(static_cast<uint64_t>(ul_rx_load1_x100()), std::memory_order_relaxed);
      }
      break;
    }
    case baseband_gateway_receiver::rx_error::other:
      c.rx_other.fetch_add(1, std::memory_order_relaxed);
      break;
  }

  // The host's own load at a tail event, for the reason ovf_load1_x100 gives.
  if ((recv_us > 1000) || (error != baseband_gateway_receiver::rx_error::none) ||
      (c.loop_over_1ms.load(std::memory_order_relaxed) != 0)) {
    const uint64_t load_x100 = static_cast<uint64_t>(ul_rx_load1_x100());
    uint64_t       load_prev = c.load1_at_tail_max_x100.load(std::memory_order_relaxed);
    while ((load_x100 > load_prev) &&
           !c.load1_at_tail_max_x100.compare_exchange_weak(load_prev, load_x100, std::memory_order_relaxed)) {
    }
  }
}

/// Records the size of one discontinuity, in µs (see ul_rx_stats::gap_us).
void ul_rx_note_gap(int64_t gap_us)
{
  ul_rx_stats& c = ul_rx_counters();
  const unsigned idx = c.gap_us_n.fetch_add(1, std::memory_order_relaxed);
  if (idx < ul_rx_stats::max_gaps) {
    c.gap_us[idx].store(static_cast<uint64_t>(gap_us), std::memory_order_relaxed);
  }
}

void ul_rx_stats_report()
{
  const ul_rx_stats& c = ul_rx_counters();
  if (c.blocks.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::fprintf(stderr,
               "[ul_rx] blocks=%llu samples=%llu gaps=%llu gap_samples=%llu ts0_blocks=%llu "
               "rx_overflows=%llu rx_lates=%llu rx_other=%llu\n",
               static_cast<unsigned long long>(c.blocks.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.samples.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.gaps.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.gap_samples.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.ts0_blocks.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.rx_overflows.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.rx_lates.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.rx_other.load(std::memory_order_relaxed)));
  // The discontinuities themselves: the sizes say which failure it was ("one slot's worth" against "the ring
  // drained"), and only the first one is logged when it happens.
  const unsigned n_gaps = c.gap_us_n.load(std::memory_order_relaxed);
  if (n_gaps != 0) {
    std::fprintf(stderr, "[ul_rx] gap_us=[");
    for (unsigned i = 0; (i != n_gaps) && (i != ul_rx_stats::max_gaps); ++i) {
      std::fprintf(stderr,
                   "%s%llu",
                   (i == 0) ? "" : ",",
                   static_cast<unsigned long long>(c.gap_us[i].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "] (in order%s)\n", (n_gaps > ul_rx_stats::max_gaps) ? ", first 16" : "");
  }
  // Where the receive margin went: the twin of [dl_tx_slack]/[dl_tx_call] (see ul_rx_note_call).
  if (c.calls.load(std::memory_order_relaxed) != 0) {
    std::fprintf(stderr,
                 "[ul_rx_timing] calls=%llu recv(max=%lldus over 1ms=%llu over 5ms=%llu) "
                 "loop(max=%lldus over 1ms=%llu over 5ms=%llu) slip(max=%lldus over 1ms=%llu) "
                 "load1(max at a tail event=%lld.%02lld)\n",
                 static_cast<unsigned long long>(c.calls.load(std::memory_order_relaxed)),
                 static_cast<long long>(c.recv_max_us.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.recv_over_1ms.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.recv_over_5ms.load(std::memory_order_relaxed)),
                 static_cast<long long>(c.loop_max_us.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.loop_over_1ms.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.loop_over_5ms.load(std::memory_order_relaxed)),
                 static_cast<long long>(c.slip_max_us.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c.slip_over_1ms.load(std::memory_order_relaxed)),
                 static_cast<long long>(c.load1_at_tail_max_x100.load(std::memory_order_relaxed) / 100),
                 static_cast<long long>(c.load1_at_tail_max_x100.load(std::memory_order_relaxed) % 100));
    const unsigned n_ctx = c.ovf_ctx_n.load(std::memory_order_relaxed);
    if (n_ctx != 0) {
      // The decisive reading: a large loop_us here says the host was late to ASK (its own scheduling, the
      // fused lane's host threads); a large recv_us says the transport blocked INSIDE the call (USB/radio).
      std::fprintf(stderr, "[ul_rx_timing] overflow_ctx=[");
      for (unsigned i = 0; (i != n_ctx) && (i != ul_rx_stats::max_ovf_ctx); ++i) {
        std::fprintf(stderr,
                     "%srecv_us=%llu,loop_us=%llu,load1=%lld.%02lld",
                     (i == 0) ? "" : " ",
                     static_cast<unsigned long long>(c.ovf_recv_us[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(c.ovf_loop_us[i].load(std::memory_order_relaxed)),
                     static_cast<long long>(c.ovf_load1_x100[i].load(std::memory_order_relaxed) / 100),
                     static_cast<long long>(c.ovf_load1_x100[i].load(std::memory_order_relaxed) % 100));
      }
      std::fprintf(stderr, "] (the call that ended in each radio overflow, in order)\n");
    }
  }
}

const bool ul_rx_stats_registered = []() {
  std::atexit(ul_rx_stats_report);
  register_p0_report(ul_rx_stats_report);
  register_phy_pipeline_check(
      {"radio sample continuity", []() -> std::optional<bool> {
         const ul_rx_stats& c = ul_rx_counters();
         std::fprintf(stderr,
                      "%llu gaps over %llu blocks (%llu samples missing or repeated), %llu timestamp-0 blocks, "
                      "%llu radio receive overflow(s)",
                      static_cast<unsigned long long>(c.gaps.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.blocks.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.gap_samples.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.ts0_blocks.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.rx_overflows.load(std::memory_order_relaxed)));
         if (c.blocks.load(std::memory_order_relaxed) < 2) {
           return std::nullopt;
         }
         return c.gaps.load(std::memory_order_relaxed) == 0;
       }});
  return true;
}();
#endif // OCUDU_METAL_STATS

lower_phy_baseband_processor::lower_phy_baseband_processor(const lower_phy_baseband_processor_configuration& config,
                                                           const lower_phy_baseband_processor_dependencies&  deps) :
  srate(config.srate),
  nof_samples_in_all_hyper_frames(config.srate.to_kHz() * NOF_HYPER_SFNS * NOF_SFNS * NOF_SUBFRAMES_PER_FRAME),
  rx_buffer_size(config.rx_buffer_size),
  slot_duration(1000 / pow2(to_numerology_value(config.scs))),
  system_time_throttling_ratio(config.system_time_throttling),
  rx_executor(deps.rx_task_executor),
  tx_executor(deps.tx_task_executor),
  uplink_executor(deps.ul_task_executor),
  receiver(deps.receiver),
  transmitter(deps.transmitter),
  uplink_processor(deps.ul_bb_proc),
  downlink_processor(deps.dl_bb_proc),
  rx_pool(std::make_shared<rx_buffer_pool>(config.nof_rx_buffers)),
  tx_time_offset(config.tx_time_offset),
  rx_to_tx_max_delay(config.rx_to_tx_max_delay),
  tx_state(config.stop_nof_slots),
  rx_state(config.stop_nof_slots)
{
  static constexpr interval<float> system_time_throttling_range(0, 20);

  ocudu_assert(rx_buffer_size, "Invalid buffer size.");
  ocudu_assert(system_time_throttling_range.contains(config.system_time_throttling),
               "System time throttling (i.e., {}) is out of the range {}.",
               config.system_time_throttling,
               system_time_throttling_range);
  ocudu_assert(config.nof_rx_ports != 0, "Invalid number of receive ports.");
  ocudu_assert(config.nof_tx_ports != 0, "Invalid number of transmit ports.");

  // Create queue of receive buffers. Page-aligned storage: the GPU zero-copy FFT path
  // wraps these buffers with newBufferWithBytesNoCopy (MTLResourceStorageModeShared) and
  // reads the I/Q samples without any host-side copy.
  // The pool is created with room for exactly the configured number of buffers, and every handle it
  // hands out carries the rule that returns it here (see rx_buffer_pool).
  {
    auto& buffers = rx_pool->buffers;
    std::weak_ptr<rx_buffer_pool> pool = rx_pool;
    while (!buffers.full()) {
      buffers.push_blocking(std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>(
          new baseband_gateway_buffer_dynamic_aligned(config.nof_rx_ports, rx_buffer_size),
          rx_buffer_pool::deleter{pool}));
    }
  }
  // Fix B (dev doc 6.26): ONE buffer of the same shape that is deliberately NOT in the pool. It is where a block
  // goes when the pool is dry and the wait has run out - so the radio is still consumed and the samples are
  // dropped, instead of the receive thread parking and the radio's ring overflowing behind it.
  rx_reserve_buffer = std::make_shared<baseband_gateway_buffer_dynamic_aligned>(config.nof_rx_ports, rx_buffer_size);
}

void lower_phy_baseband_processor::start(baseband_gateway_timestamp init_time, baseband_gateway_timestamp sfn0_ref_time)
{
  // If it is required to start with system frame number 0, then set a time offset to start an SFN earlier.
  start_time_sfn0   = sfn0_ref_time;
  last_rx_timestamp = init_time;
  // Whether this stream's blocks can hold whole OFDM symbols (S-7g-13, see ul_process): the grid belongs
  // to the uplink processor, and one that does not describe it (a test double, or a build without one)
  // keeps the historical whole-slot blocks. Decided per stream, when the configuration is final.
  rx_symbol_grid_known = uplink_processor.locate_symbols(0, 1).nof_samples != 0;
  // How many symbols one block covers: 0 keeps the whole-slot blocks, which is the DEFAULT and the only
  // policy whose shutdown has been seen to complete cleanly on air. The symbol-grained policy (S-7g-13) is
  // experimental and opt-in through OCUDU_UL_RX_SYMBOLS=N: it is functionally green (contract MET 7/7,
  // assembled=0, gaps=0) but its shutdown still trips a DU teardown race, and its latency benefit cannot be
  // judged with [ul_pipeline]/[ul_time_frequency] because those series start at the first RECEIVED block -
  // the slot's END under the whole-slot policy and its BEGINNING under the symbol one. Set the default back
  // to 14 (= a slot's worth of symbols, i.e. the same request the whole-slot policy makes) only after the
  // DU race is fixed and the policy has a judge whose endpoints do not move.
  {
    const char* env       = std::getenv("OCUDU_UL_RX_SYMBOLS");
    nof_symbols_per_block = (env == nullptr) ? 0U : static_cast<unsigned>(std::strtoul(env, nullptr, 10));
  }
  // A stream that starts here has to establish its phase again: the first block only closes the gap to
  // the next slot (whole-slot policy) or symbol (symbol-grained policy) boundary and is not processed
  // (see ul_process).
  rx_slot_aligned   = false;
  // A stream that starts here is running again (see ul_process).
  rx_stop_requested.store(false, std::memory_order_release);
  // Blocks the stream may drop while it establishes its phase: the first block of a stream, and the
  // partial one that follows it when the start time of the RU and the radio disagree (see ul_process).
  nof_phase_blocks  = 0;
  // The slot buffer the symbol-grained policy was filling belongs to the previous stream: drop our
  // reference (the transforms that still read it keep it alive and return it to the pool themselves).
  rx_fill_buffer.reset();
  rx_fill = 0;

  rx_state.start();
  report_fatal_error_if_not(rx_executor.defer([this]() { ul_process(); }), "Failed to execute initial uplink task.");

  tx_state.start();
  report_fatal_error_if_not(tx_executor.defer([this, init_time]() { dl_process(init_time + rx_to_tx_max_delay); }),
                            "Failed to execute initial downlink task.");
}

void lower_phy_baseband_processor::stop()
{
  // Read by ul_process() to tell "the executor refused a task because we are going down" (expected, the
  // application may stop the executor before this receive chain has drained) from "it refused while the
  // stream was supposed to be running" (a defect that must not pass silently).
  rx_stop_requested.store(true, std::memory_order_release);
  rx_state.request_stop();
  tx_state.request_stop();
  rx_state.wait_stop();
  tx_state.wait_stop();

  // Flush the processing executors (platform mapping lives in the compat layer; no-op on Linux): the FSM
  // counters only track the self-deferred processing chains, while tasks deferred right before the stop was
  // requested are not covered by them. The compat layer defers a sentinel task and waits for its completion, so
  // every previously enqueued task has finished when stop() returns and the processor can be safely destroyed.
  report_fatal_error_if_not(compat::drain_executor_on_stop(rx_executor), "Failed to execute downlink flush task.");
  report_fatal_error_if_not(compat::drain_executor_on_stop(tx_executor), "Failed to execute downlink flush task.");
  report_fatal_error_if_not(compat::drain_executor_on_stop(uplink_executor),
                            "Failed to execute uplink processing flush task.");
}

void lower_phy_baseband_processor::dl_process(baseband_gateway_timestamp timestamp)
{
  // Check if it is running, notify stop and return without enqueueing more tasks.
  if (!tx_state.on_process()) {
    tx_state.on_process_end();
    return;
  }

#if defined(OCUDU_FLOW_PROBES)
  // [zmq-probe] instrumentation (compiled only with ENABLE_FLOW_PROBES).
  const auto t_entry = std::chrono::steady_clock::now();
#endif

  // Throttling mechanism to keep a maximum latency of one millisecond in the transmit buffer based on the latest
  // received timestamp.
  {
    // Calculate maximum waiting time to avoid deadlock.
    std::chrono::microseconds timeout_duration = 2 * slot_duration;
    // Maximum time point to wait for.
    std::chrono::time_point<std::chrono::steady_clock> wait_until_tp =
        std::chrono::steady_clock::now() + timeout_duration;
    // Wait until one of these conditions is met:
    // - The reception timestamp reaches the desired value;
    // - The system time reaches the maximum waiting time; or
    // - The lower PHY was stopped.
    while ((timestamp > (last_rx_timestamp.load(std::memory_order_acquire) + rx_to_tx_max_delay)) &&
           (std::chrono::steady_clock::now() < wait_until_tp)) {
      // Platform mapping lives in the compat layer: 10 us sleep on Linux (upstream), YIELD-hint spin on macOS
      // (short sleeps are coalesced by the Darwin scheduler and overshoot the 2 ms deadline).
      compat::wait_for_tx_timestamp();
    }
  }

#if defined(OCUDU_FLOW_PROBES)
  // [zmq-probe] instrumentation (compiled only with ENABLE_FLOW_PROBES).
  const auto t_after_rx_wait = std::chrono::steady_clock::now();
#endif

  // Throttling mechanism to slow down the baseband processing.
  if ((system_time_throttling_ratio > 0.0) && (last_tx_time.has_value()) && (last_tx_buffer_size != 0)) {
    // Get current time and calculate the elapsed time since the last call.
    std::chrono::time_point<std::chrono::steady_clock> now     = std::chrono::steady_clock::now();
    std::chrono::nanoseconds                           elapsed = now - *last_tx_time;

    // Calculate the number of samples from the previous transmission to the next one and convert it seconds.
    float expected_elapsed_s = static_cast<double>(last_tx_buffer_size) / srate.to_Hz<float>();

    // Calculate the minimum elapsed time required to satisfy the throttling time.
    std::chrono::nanoseconds minimum_elapsed(
        static_cast<uint64_t>(expected_elapsed_s * 1e9 * system_time_throttling_ratio));

    if (elapsed < minimum_elapsed) {
      std::this_thread::sleep_until(*last_tx_time + minimum_elapsed);
    }
  }
  last_tx_time.emplace(std::chrono::steady_clock::now());

  // Process downlink buffer.
  downlink_processor_baseband::processing_result result =
      downlink_processor.process(apply_timestamp_sfn0_ref(timestamp));
  ocudu_assert(result.buffer, "The buffer must be valid.");

  // Set transmission timestamp.
  result.metadata.ts = timestamp + tx_time_offset;

  // Enqueue transmission.
#if defined(OCUDU_FLOW_PROBES)
  const auto t_after_process = std::chrono::steady_clock::now();
#endif
  trace_point tx_tp          = ru_tracer.now();

  // dev doc 6.41 (V3's S2): the host's remaining margin at the transmit hand-over, in the radio's own time
  // base - the radio time still left before these samples must be on the air (see tx_slack_accounting). It is
  // measured HERE, after the throttling wait and immediately before the hand-over, because that is the instant
  // whose lateness UHD reports as `underflow`.
  if (tx_slack_accounts().rx_valid.load(std::memory_order_acquire)) {
    const tx_slack_accounting& a          = tx_slack_accounts();
    const auto                 host_now   = std::chrono::steady_clock::now();
    const int64_t              radio_us   = static_cast<int64_t>(
        (static_cast<double>(result.metadata.ts) - static_cast<double>(a.rx_ts.load(std::memory_order_relaxed))) * 1e6 /
        srate.to_Hz<double>());
    const int64_t host_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(host_now.time_since_epoch()).count() -
        a.rx_host_ns.load(std::memory_order_relaxed) / 1000;
    tx_slack_note_transmit(radio_us - host_elapsed_us, static_cast<uint64_t>(result.metadata.ts));
  }

  // Transmit buffer (timed: dev doc 6.42 (4), S2b - see tx_slack_accounting::tx_over_1ms).
  const auto tx_call_begin = std::chrono::steady_clock::now();
  transmitter.transmit(result.buffer->get_reader(), result.metadata);
  tx_slack_note_call_us(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                              tx_call_begin)
                            .count());

#if defined(OCUDU_FLOW_PROBES)
  // [zmq-probe] instrumentation (compiled only with ENABLE_FLOW_PROBES).
  {
    const auto t_done = std::chrono::steady_clock::now();
    auto       wait_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t_after_rx_wait - t_entry).count();
    auto proc_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t_after_process - t_after_rx_wait).count();
    auto tx_us = std::chrono::duration_cast<std::chrono::microseconds>(t_done - t_after_process).count();
    if (wait_us > 2000 || proc_us > 5000 || tx_us > 5000) {
      static auto& probe_log = ocudulog::fetch_basic_logger("ALL");
      probe_log.info("[zmq-probe] dl slot={} rx-wait={}us process={}us transmit={}us",
                     timestamp / srate.to_kHz(),
                     wait_us,
                     proc_us,
                     tx_us);
    }
    static unsigned probe_slot_count = 0;
    static auto     probe_last       = std::chrono::steady_clock::now();
    if ((++probe_slot_count & 63) == 0) {
      static auto& probe_log = ocudulog::fetch_basic_logger("ALL");
      auto         now       = std::chrono::steady_clock::now();
      auto         elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - probe_last).count();
      probe_log.info("[zmq-probe] dl rate: 64 slots in {}ms = {:.1f} slots/s",
                     elapsed_ms,
                     elapsed_ms > 0 ? 64000.0 / elapsed_ms : 0.0);
      probe_last = now;
    }
  }
#endif

  ru_tracer << trace_event("transmit_baseband", tx_tp);

  // Update last buffer size.
  last_tx_buffer_size = result.buffer->get_nof_samples();

  dl_probe.event(last_tx_buffer_size);
  dl_probe.tick();
  dl_jitter_probe.event();
  dl_jitter_probe.tick();

  // Per-slot timestamp log for the internal chain latency correlation (slot indication -> DL production).
  // Compiled in only with ENABLE_FLOW_PROBES; reported through the asynchronous logging system.
#if defined(OCUDU_FLOW_PROBES)
  {
    static auto& slot_logger = ocudulog::fetch_basic_logger("ALL");
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    slot_logger.debug("[dl_slot] {}.{:09} {}",
                      static_cast<long long>(ts.tv_sec),
                      ts.tv_nsec,
                      timestamp / srate.to_kHz());
  }
#endif

  // Enqueue DL process task.
  report_fatal_error_if_not(
      tx_executor.defer([this, new_timestamp = timestamp + last_tx_buffer_size]() { dl_process(new_timestamp); }),
      "Failed to execute downlink processing task");

  tx_state.on_process_end();
}

bool lower_phy_baseband_processor::rx_pool_drop_enabled()
{
  static const bool enabled = []() {
    const char* v = std::getenv("OCUDU_UL_RX_POOL_DROP");
    return (v == nullptr) || (std::atoi(v) != 0);
  }();
  return enabled;
}

bool lower_phy_baseband_processor::rx_pool_drop_forced()
{
  // \brief DIAGNOSTIC ARM (like OCUDU_D1_HANDED_BOUND): force the next `OCUDU_UL_RX_POOL_DROP_FORCE=<n>` takes
  // to be treated as DRY, so the drop path itself is exercised on a leg whose pool never goes dry. A leg flown
  // with it reports `dropped=<n>` and its `gaps`/contract/decode sentinels then say whether dropping a block is
  // as harmless as it is meant to be - which is the half no quiet leg can answer.
  static std::atomic<unsigned> remaining = []() {
    const char* v = std::getenv("OCUDU_UL_RX_POOL_DROP_FORCE");
    return (v != nullptr) ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
  }();
  if (remaining.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  unsigned prev = remaining.load(std::memory_order_relaxed);
  while ((prev != 0) && !remaining.compare_exchange_weak(prev, prev - 1, std::memory_order_relaxed)) {
  }
  return prev != 0;
}

std::shared_ptr<baseband_gateway_buffer_dynamic_aligned> lower_phy_baseband_processor::pop_rx_buffer_or_reserve(
    bool& dropped)
{
  dropped = false;
  std::shared_ptr<baseband_gateway_buffer_dynamic_aligned> buffer{};
  // The DIAGNOSTIC ARM makes the pool look dry for the next N takes WITHOUT taking the buffer, so the whole
  // path below runs for real: the reap, the sliced wait and (with the drop enabled) the drop after the budget.
  // Returning the reserve immediately would exercise only the drop's bookkeeping, not the timing that decides it.
  bool force_timeout = rx_pool_drop_forced();
  if (!force_timeout && rx_pool->buffers.try_pop(buffer)) {
    // The healthy path: nothing to wait for. It DOES ask the registry to sweep, though, at a bounded
    // cadence - dev doc 6.34: the sweep's other two entry points (a deposit, and the park below) both need
    // either new work or a dry pool, so in a UL-QUIET window neither fires and a block nobody claimed keeps
    // its whole-slot buffer out of this pool for as long as the quiet lasts (measured: 19.9 ms on p23,
    // 96.4 ms on p24, 2.79 s on p19, 10.4 s on p22). The sweep's RULES are untouched - a block is reaped
    // only once the chain has moved its window past its slot, or past the 10 ms deadline - so this cannot
    // take a block from a hop that is still entitled to claim it; what changes is only WHEN the rules are
    // evaluated. Throttled to rx_sweep_interval (1 ms, i.e. one sweep per couple of slots at 30 kHz) so a
    // healthy run pays one registry mutex per millisecond and not one per take.
    const auto now = std::chrono::steady_clock::now();
    if ((now - rx_last_sweep) >= rx_sweep_interval) {
      rx_last_sweep = now;
      handover_reap_hook::reap(handover_reap_hook::reap_reason::take);
    }
    return buffer;
  }
  // The pool is DRY, which is the only state in which the hand-over has something to give back - and the
  // only state in which this thread is about to become unreachable by every registry entry point (see the
  // header). Ask, then wait in bounded slices so a reaper that was not enough the first time is asked again.
  //
  // ★ AND THIS IS WHERE A STALL DUMPS THE READINGS (dev doc 6.24). Every P0 reading of this workflow is printed
  // by an atexit handler, so a leg whose shutdown the stall also holds - `p14-conc2`, whose five-second stop
  // grace expired and whose whole report was lost - produces no evidence at all. THIS thread is the one that
  // knows the stall is happening (it is parked here because the completion that releases the input tokens is
  // late, and while it is parked the radio is not consumed, so no slot indication is produced and the whole
  // slot loop stops), so it is the right place to dump. The dump is rate-limited: a 5 s stall prints one or two
  // snapshots instead of thousands, and the ordinary park (measured: 486 us at 12.9 Mbit/s, 22 us on the n1
  // leg) never reaches the threshold at all.
  constexpr auto stall_dump_after = std::chrono::milliseconds(20);
  constexpr uint64_t stall_dump_min_interval_ms = 2000;
  const auto         parked_since = std::chrono::steady_clock::now();
  // ★ THE WAIT IS SLICED AT THE BUDGET, and that is not a detail: `pop_wait_for` waits for the WHOLE slice, so a
  // slice longer than rx_park_budget makes the budget unreachable - the first wait would already have parked for
  // the slice, and by the time the drop was decided the radio's ring (which overflows at ~4.4 ms of park,
  // measured) would have lost its samples anyway. With the drop enabled the slice is therefore min(reap slice,
  // budget): the reap runs ten times as often while the pool is dry (which is what makes the hand-over give its
  // buffers back sooner) and the drop lands inside the ring. With the drop disabled - the A/B arm - the slice
  // stays 10 ms, i.e. exactly the behaviour that shipped before this fix.
  const std::chrono::milliseconds wait_slice =
      rx_pool_drop_enabled() ? std::min(rx_reap_slice, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      rx_park_budget + std::chrono::microseconds(1)))
                             : rx_reap_slice;
  for (;;) {
    handover_reap_hook::reap(handover_reap_hook::reap_reason::dry_pool);
    // The DIAGNOSTIC arm (OCUDU_UL_RX_POOL_DROP_FORCE) has already "timed out": it skips the wait so the budget
    // and the drop below run for real, on a pool that is in fact healthy.
    const bool simulated_timeout = force_timeout;
    force_timeout                = false;
    if (!simulated_timeout) {
      const blocking_queue<std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>>::result ret =
          rx_pool->buffers.pop_wait_for(buffer, wait_slice);
      if (ret == decltype(ret)::success) {
        return buffer;
      }
      if (ret == decltype(ret)::failed) {
        // The queue was stopped: the caller gets the same null buffer the plain pop_blocking() would give it.
        return std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>{};
      }
    }
    const auto parked_us =
        simulated_timeout
            ? (std::chrono::duration_cast<std::chrono::microseconds>(rx_park_budget) + std::chrono::microseconds(1))
            : std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - parked_since);
    if ((std::chrono::steady_clock::now() - parked_since) > stall_dump_after) {
      (void)p0_dump_reports("dry-pool park", stall_dump_min_interval_ms);
    }
    // ★ FIX B (dev doc 6.26): the wait is BOUNDED. Past the budget the caller gets the RESERVE buffer and is
    // told to drop the block: the radio keeps being consumed, so its ring does not overflow and the samples of
    // every LATER slot survive - the price is this one block's samples, i.e. one HARQ retransmission.
    if (rx_pool_drop_enabled() && (parked_us > rx_park_budget) && (rx_reserve_buffer != nullptr)) {
      rx_pool_note_dropped(static_cast<uint64_t>(parked_us.count()));
      dropped = true;
      return rx_reserve_buffer;
    }
  }
}

void lower_phy_baseband_processor::ul_process()
{
  // Check if it is running, notify stop and return without enqueueing more tasks.
  if (!rx_state.on_process()) {
    rx_state.on_process_end();
    return;
  }

  // Get receive buffer. The wait is measured (P0-2): this is the one place the receive can be parked by the
  // pool, and it is BEFORE receiver.receive(), so [ul_rx_wait] does not cover it. Since fix B (dev doc 6.26) the
  // wait is BOUNDED: `dropped` says this call was handed the RESERVE buffer because the pool stayed dry past
  // rx_park_budget, and the block it receives must be DISCARDED.
  const auto rx_take_t0 = std::chrono::steady_clock::now();
  bool       dropped  = false;
  std::shared_ptr<baseband_gateway_buffer_dynamic_aligned> rx_buffer = pop_rx_buffer_or_reserve(dropped);
  rx_pool_note_wait(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - rx_take_t0).count());
  rx_pool_note_taken(rx_pool->buffers.size(), rx_pool->buffers.max_size());

  // \brief Samples to receive in this call.
  ///
  /// The radio is told how many samples to receive by the size of the buffer it is given
  /// (baseband_gateway_receiver::receive), so the block size is ours to choose, and it decides when the
  /// front end can start:
  ///
  ///  - \b whole \b slots (the historical policy, kept when the buffer cannot hold a slot or when the
  ///    uplink processor does not describe the symbol grid): ask for the samples that complete the
  ///    current slot, so every block ends on a slot boundary - and a slot is a whole number of OFDM
  ///    symbols, so no symbol straddles two blocks. The front end then waits for the slot's LAST
  ///    samples before it can transform the first symbol.
  ///  - \b whole \b symbols (S-7g-13, the default): ask for an integer number of OFDM symbols, filled
  ///    into one slot buffer across several calls, so a symbol is transformed as soon as it arrived and
  ///    the front end overlaps the arrival of the rest of the slot. A symbol still lies wholly inside
  ///    one block, which is what keeps it readable where the radio put it (see
  ///    lower_phy_uplink_processor_impl::process_symbol_boundary).
  ///
  /// The phase is only unknown for the first block of a stream (the pool hands out buffers, not a
  /// timeline): the timestamp of the next sample to be received is last_rx_timestamp. Until the stream
  /// is symbol aligned, a block asks for the samples that close the gap to the next symbol boundary and
  /// is dropped - at most one block, before any UE can be transmitting (see max_phase_blocks).
  const unsigned nof_samples_per_slot =
      srate.to_kHz() * static_cast<uint64_t>(slot_duration.count()) / 1000;
  const bool slot_capable  = rx_buffer_size >= nof_samples_per_slot;
  const bool symbol_blocks = slot_capable && rx_symbol_grid_known && (nof_symbols_per_block != 0);

  unsigned rx_offset   = 0;
  unsigned nof_samples = 0;
  if (!symbol_blocks) {
    nof_samples = rx_buffer->get_nof_samples();
    if (slot_capable) {
      const unsigned phase =
          static_cast<unsigned>(last_rx_timestamp.load(std::memory_order_acquire) % nof_samples_per_slot);
      nof_samples = (phase != 0) ? (nof_samples_per_slot - phase) : nof_samples_per_slot;
    }
  } else {
    // A slot buffer holds a whole number of symbols and is filled in order, so the samples of a symbol
    // are contiguous in it and never split between two buffers.
    if ((rx_fill_buffer == nullptr) || (rx_fill == nof_samples_per_slot)) {
      rx_fill_buffer = std::move(rx_buffer);
      rx_fill        = 0;
    }
    rx_buffer = rx_fill_buffer;
    rx_offset = rx_fill;

    const baseband_gateway_timestamp                      next_ts = last_rx_timestamp.load(std::memory_order_acquire);
    uplink_processor_baseband::symbol_grid_position       position = uplink_processor.locate_symbols(next_ts, nof_symbols_per_block);
    if (position.nof_samples_to_boundary != 0) {
      // Not symbol aligned: ask for what is left of the straddled symbol and drop it (its beginning is
      // already gone, so no transform can use it).
      nof_samples = position.nof_samples_to_boundary;
    } else {
      // Whole symbols, reduced to the room the slot buffer still has so that a symbol is never cut.
      while ((position.nof_symbols > 1) && ((rx_offset + position.nof_samples) > nof_samples_per_slot)) {
        position = uplink_processor.locate_symbols(next_ts, position.nof_symbols - 1);
      }
      if ((position.nof_samples == 0) || ((rx_offset + position.nof_samples) > nof_samples_per_slot)) {
        // The window has no room left for a whole symbol (a grid whose period does not tile it): retire
        // the buffer and start the next one at this boundary instead of splitting a symbol.
        const auto rx_fill_t0 = std::chrono::steady_clock::now();
        bool       retire_dropped = false;
        rx_fill_buffer        = pop_rx_buffer_or_reserve(retire_dropped);
        if (retire_dropped) {
          // Fix B: this path needs a REAL buffer to fill the next window with, and the pool has none. Drop this
          // block instead (the reserve is not a slot buffer - it must never become one).
          rx_fill_buffer = nullptr;
          rx_buffer      = rx_reserve_buffer;
          dropped        = true;
        }
        rx_pool_note_wait(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                                rx_fill_t0)
                              .count());
        rx_fill        = 0;
        rx_buffer      = rx_fill_buffer;
        rx_offset      = 0;
        position       = uplink_processor.locate_symbols(next_ts, nof_symbols_per_block);
      }
      nof_samples = position.nof_samples;
    }
  }
  // ★ FIX B (dev doc 6.26): a block the pool could not serve is RECEIVED AND DROPPED, never left in the radio.
  //
  // What used to happen: the receive thread parked here (in pop_rx_buffer_or_reserve()), and while it was parked
  // the radio's samples were not consumed - its ring overflowed, every sample in it was lost (`Receive stream
  // discontinuity`, one 100938-sample gap on leg p17) and, in the long form of the stall (dev doc 6.20), no slot
  // indication was produced and the whole slot loop starved with it. What happens now: the block is received into
  // the reserve buffer and thrown away. The radio keeps streaming, the ring stays drained, the LATER slots are
  // untouched, and the cost is exactly one block's samples - one HARQ retransmission.
  //
  // The bookkeeping that must survive the drop is the TIMESTAMP: `last_rx_timestamp` advances by what the radio
  // actually delivered (below), so the next call computes the same kind of block it would have, and the
  // symbol/phase alignment is unchanged. The sample count is computed HERE, without a pool buffer, because the
  // reserve is a buffer of the same size and the two policies only need the timestamp and the symbol grid.
  if (dropped) {
    unsigned drop_samples = rx_buffer->get_nof_samples();
    if (!symbol_blocks) {
      if (slot_capable) {
        const unsigned phase =
            static_cast<unsigned>(last_rx_timestamp.load(std::memory_order_acquire) % nof_samples_per_slot);
        drop_samples = (phase != 0) ? (nof_samples_per_slot - phase) : nof_samples_per_slot;
      }
    } else {
      const baseband_gateway_timestamp                next_ts = last_rx_timestamp.load(std::memory_order_acquire);
      const uplink_processor_baseband::symbol_grid_position pos =
          uplink_processor.locate_symbols(next_ts, nof_symbols_per_block);
      drop_samples = (pos.nof_samples_to_boundary != 0) ? pos.nof_samples_to_boundary : pos.nof_samples;
      // The reserve holds one block: never ask the radio for more than it can take (a symbol-grained request is
      // always smaller than a slot, but the clamp keeps the contract local rather than assumed).
      drop_samples = std::min(drop_samples, rx_buffer->get_nof_samples());
    }
    baseband_gateway_buffer_writer_view drop_writer(rx_buffer->get_writer(), 0, drop_samples);
    baseband_gateway_receiver::metadata    drop_metadata = receiver.receive(drop_writer);
    last_rx_timestamp.store(drop_metadata.ts + drop_samples, std::memory_order_release);
    if (symbol_blocks && (rx_fill_buffer != nullptr)) {
      // Keep the slot buffer's offsets consistent with the timeline the timestamp describes: the dropped
      // samples' room is SKIPPED, so everything after them lands where it belongs. Without this the rest of the
      // slot would be written one drop too early and the whole slot would be garbage; with it the slot has a
      // hole of stale samples (the samples the radio never gave us) and the rest of the slot is placed
      // correctly - which is what a radio-side lost block already produces on this path.
      rx_fill = std::min(rx_fill + drop_samples, nof_samples_per_slot);
    }
    // The readings taken while the leg is in trouble: this is the state that used to end in lost samples, so a
    // drop dumps them (rate-limited, dev doc 6.24) - the samples themselves are discarded by not processing them.
    (void)p0_dump_reports("dry-pool drop", 2000);
    report_fatal_error_if_not(rx_executor.defer([this]() { ul_process(); }), "Failed to execute receive task.");
    rx_state.on_process_end();
    return;
  }

  // T_start of the UL compute pipeline measurement: the moment the samples of this block START arriving, so
  // the series measures "first sample of the slot in -> CRC OK out" no matter how the receive side asks for
  // them. Use the same slot reference the FAPI slot_point carries to the PUSCH completion (the
  // sample-timestamp-derived count modulo the SFN cycle, as computed by the uplink processor): with the plain
  // absolute count the pairing only matched during the first SFN cycle of the run.
  //
  // It used to be recorded AFTER receive() returned, which measures that same thing only for a policy whose
  // block arrives with its first samples. With whole-slot blocks the radio hands the block over at the slot's
  // END, so the series started there and excluded the wait for the samples - which is why the symbol-grained
  // policy (S-7g-13) looked ~400us SLOWER in these series while it is ~600us faster end to end. Series recorded
  // before this change are not comparable with the ones after it (see the design document).
  {
    const uint64_t nof_slots_per_sfn_cycle =
        (nof_samples_in_all_hyper_frames / NOF_HYPER_SFNS) / nof_samples_per_slot;
    ul_pipeline_probe::get().record_start(
        (apply_timestamp_sfn0_ref(last_rx_timestamp.load(std::memory_order_acquire)) / nof_samples_per_slot) %
        nof_slots_per_sfn_cycle);
  }

  baseband_gateway_buffer_writer_view rx_writer(rx_buffer->get_writer(), rx_offset, nof_samples);

  // Receive baseband.
  trace_point tp = ru_tracer.now();
#if defined(OCUDU_FLOW_PROBES)
  const auto t_recv_begin = std::chrono::steady_clock::now();
#endif
  // dev doc 6.51: the receive side gets the timing probe the transmit side already had ([dl_tx_slack] and
  // [dl_tx_call]). Two clock reads and one relaxed-store block per slot, because the question - "is the host
  // late to ASK, or does the transport block INSIDE the call?" - cannot be answered from the timestamps alone,
  // and it is the question that decides whether the remaining millisecond discontinuities are ours to fix.
  const auto rx_call_begin = std::chrono::steady_clock::now();
  baseband_gateway_receiver::metadata rx_metadata = receiver.receive(rx_writer);
  const auto rx_call_end = std::chrono::steady_clock::now();
  // dev doc 6.41: the clock map the transmit-side margin needs - the radio timestamp just delivered and the
  // host instant it was delivered at. One relaxed store each per receive, on the receive thread.
  tx_slack_note_receive(static_cast<uint64_t>(rx_metadata.ts),
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            rx_call_end.time_since_epoch())
                            .count());
  // The air time of the block the call asked for: the reference the receive timing is read against (see
  // ul_rx_note_call). `srate` is in kHz, so samples * 1000 / kHz is microseconds.
  ul_rx_note_call(std::chrono::duration_cast<std::chrono::nanoseconds>(rx_call_begin.time_since_epoch()).count(),
                  std::chrono::duration_cast<std::chrono::nanoseconds>(rx_call_end.time_since_epoch()).count(),
                  static_cast<int64_t>(nof_samples) * 1000 / static_cast<int64_t>(srate.to_kHz()),
                  rx_metadata.error);
#if defined(OCUDU_FLOW_PROBES)
  // [zmq-probe] instrumentation (compiled only with ENABLE_FLOW_PROBES), plus the [ul_rx_wait] series.
  //
  // The wait is recorded from THIS call's own two clock reads - the same ones the 20 ms notice above uses -
  // so the series costs one more conversion and nothing else. It answers a question no other series can:
  // [ul_pipeline] and [ul_time_frequency] start before this receive (see record_start()), so they INCLUDE
  // this wait, and a name like "time-frequency" that reports a whole slot is only readable next to it.
  //
  // What the number means: the radio produces samples at the ADC's fixed rate and the host consumes them as
  // fast as it can, so a host that is AHEAD of the sample timeline blocks here for the samples to exist, and
  // one that is behind returns immediately with data the radio had already buffered. Under the whole-slot
  // policy this block is a whole slot, so the wait cannot be shorter than "until the slot's last sample
  // exists" - the structural latency a symbol-grained receive policy exists to remove (S-7g-13).
  {
    const auto recv_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_recv_begin).count();
    ul_pipeline_probe::get().record_rx_wait(recv_us * 1000);
    // The per-slot timeline (OCUDU_UL_SLOT_TRACE) needs the ONE instant the other series take for granted: the
    // arrival of the samples that COMPLETE a slot. Everything else in the probe starts at the slot's FIRST
    // sample, which is a different instant whenever the block carrying a slot's tail is not the block that
    // starts it - and that difference is why per-slot questions cannot be answered from those series.
    //
    // NOTE (2 of 2, 2026-09-21): this test is right only for a stream whose blocks END on the slot grid, which
    // the whole-slot policy satisfies. The symbol-grained policy does NOT: measured on air, its timestamp
    // advances 7680 samples per block while only 3840 arrive, so NO block carries a slot's last sample and this
    // captures nothing. That inconsistency is in the receive path, not here - see design document 5.8.31 (8).
    if (nof_samples != 0) {
      // ── FRAME-INDEPENDENT slot completion ──────────────────────────────────────────────────────────────
      // A block completes a slot iff the first slot boundary at or after its start lies INSIDE it. Written that
      // way, the test needs no knowledge of where the frame begins: `a` is the block's own offset inside a slot,
      // so the next boundary is `S - a` samples ahead (or 0 when the block starts on one). Three earlier forms
      // failed on air because they derived a position from the SFN0-referenced slot index and then compared it
      // against absolute timestamps - a frame mix whose difference is start_time_sfn0, which is not zero and not
      // knowable from here. This form cannot have that bug: the only absolute quantity left is the block length.
      const uint64_t slots_per_sfn_cycle = (nof_samples_in_all_hyper_frames / NOF_HYPER_SFNS) / nof_samples_per_slot;
      const uint64_t block_begin_ref     = apply_timestamp_sfn0_ref(rx_metadata.ts);
      const uint64_t offset_in_slot      = block_begin_ref % nof_samples_per_slot;
      // Kept for the diagnostic line below, which reports the stream's ALIGNMENT: it is what tells an operator
      // whether the receive policy is delivering whole slots on the grid (offset 0) or blocks that straddle one.
      const uint64_t to_next_boundary = nof_samples_per_slot - offset_in_slot;
      // ★ THE COMPLETION RULE. Slot S's samples are complete once its LAST sample has arrived - see
      // ul_slot_completed_by_block() for the rule itself and for what the form it replaces got
      // wrong on this very path (it tested `to_next_boundary < nof_samples`, which is `7680 < 7680` - false for
      // every whole-slot block - and, when it did fire, named the slot AFTER the completed one).
      uint64_t   completed  = 0;
      const bool completes  = ul_slot_completed_by_block(
          block_begin_ref, nof_samples, nof_samples_per_slot, completed);
      if (ul_pipeline_probe::slot_trace_enabled()) {
        static std::atomic<unsigned> diag_blocks{0};
        static std::atomic<int64_t>  diag_last_s{-1};
        const unsigned               dn  = diag_blocks.fetch_add(1, std::memory_order_relaxed);
        const int64_t                sec = recv_us / 1000000;
        if ((dn < 8) || (sec != diag_last_s.exchange(sec, std::memory_order_relaxed))) {
          std::fprintf(stderr,
                       "[ul_slot_diag] blk=%u ts=%llu begin_ref=%llu sfn0=%llu off_in_slot=%llu to_boundary=%llu "
                       "n=%u sps=%u completes=%s slot_ref=%llu\n",
                       dn,
                       static_cast<unsigned long long>(rx_metadata.ts),
                       static_cast<unsigned long long>(block_begin_ref),
                       static_cast<unsigned long long>(start_time_sfn0),
                       static_cast<unsigned long long>(offset_in_slot),
                       static_cast<unsigned long long>(to_next_boundary),
                       nof_samples,
                       nof_samples_per_slot,
                       completes ? "YES" : "no",
                       static_cast<unsigned long long>(completes ? completed : 0));
          std::fflush(stderr);
        }
      }
      if (completes) {
        // Announced ONCE per completed slot. That guard is what the rule alone cannot give: a receive policy
        // whose blocks are shorter than a slot ends several of them inside the same one, and only the block that
        // first covers its last sample completes it. A whole-slot stream advances this by one per block, so the
        // test costs a relaxed load and one CAS on the path the real-time uplink depends on.
        static std::atomic<uint64_t> newest_completed{0};
        uint64_t                     prev = newest_completed.load(std::memory_order_relaxed);
        if ((completed > prev) &&
            newest_completed.compare_exchange_strong(prev, completed, std::memory_order_relaxed)) {
          ul_pipeline_probe::get().record_slot_samples_complete(
              completed % slots_per_sfn_cycle, nof_samples, recv_us * 1000, std::chrono::high_resolution_clock::now());
        }
      }
    }
    if (recv_us > 20000) {
      static auto& probe_log = ocudulog::fetch_basic_logger("ALL");
      probe_log.info("[zmq-probe] ul recv-wait={}us", recv_us);
    }
  }
#endif
  ru_tracer << trace_event("receive_baseband", tp);

#if defined(OCUDU_METAL_STATS)
  // Continuity of the stream (see ul_rx_stats): the first block of a stream may legitimately start
  // wherever the radio's timeline starts (the RU rounds its start time to a subframe, the radio does
  // not), so continuity is measured from the second block on.
  {
    ul_rx_stats&                     c        = ul_rx_counters();
    const baseband_gateway_timestamp expected = last_rx_timestamp.load(std::memory_order_acquire);
    // A block that reports timestamp 0 is not a block of the stream: it is the radio's stop/error
    // return (UHD hands back a zeroed buffer with an empty time_spec, and its timeout path returns the
    // same). The stop sequence produces a burst of them, so counting those as discontinuities turned
    // the shutdown into 362 "gaps" and hid the one thing this check is for. They are counted apart.
    if (rx_metadata.ts == 0) {
      c.ts0_blocks.fetch_add(1, std::memory_order_relaxed);
    } else if ((c.blocks.load(std::memory_order_relaxed) != 0) && (rx_metadata.ts != expected)) {
      const baseband_gateway_timestamp gap = (rx_metadata.ts > expected) ? (rx_metadata.ts - expected)
                                                                        : (expected - rx_metadata.ts);
      c.gaps.fetch_add(1, std::memory_order_relaxed);
      c.gap_samples.fetch_add(gap, std::memory_order_relaxed);
      // dev doc 6.51: keep the SIZE of every discontinuity, not just the first one's warning below - the sizes
      // are what separates "a slot's worth" from "the radio's ring drained", and they belong in the report
      // next to the radio's own `overflow` count.
      ul_rx_note_gap(static_cast<int64_t>(gap) * 1000 / static_cast<int64_t>(srate.to_kHz()));
      static std::atomic<bool> gap_logged{false};
      bool                     log_expected = false;
      if (gap_logged.compare_exchange_strong(log_expected, true)) {
        ocudulog::fetch_basic_logger("PHY").warning(
            "Receive stream discontinuity: block at timestamp {} where {} was expected ({} samples)",
            rx_metadata.ts,
            expected,
            gap);
      }
    }
    c.blocks.fetch_add(1, std::memory_order_relaxed);
    c.samples.fetch_add(nof_samples, std::memory_order_relaxed);
  }
#endif

  // Update last timestamp: the timestamp of the next sample to be received, i.e. the end of the block
  // just received (\c nof_samples of them - the receiver fills the buffer it was given).
  last_rx_timestamp.store(rx_metadata.ts + nof_samples, std::memory_order_release);

  // A block that the uplink processor can read symbol by symbol: it holds exactly one slot and it
  // starts on a slot boundary. This is measured on the block that was actually received - NOT on the
  // phase the last timestamp predicted, because the two disagree about the first block of a stream:
  // the RU rounds the start time it gives the lower PHY to a subframe (see ru_controller_sdr_impl),
  // while the radio starts streaming at a sample of its own. A block that holds a whole slot but does
  // not start on a boundary is the one that costs a copy: the uplink processor aligns to the first
  // subframe boundary inside it and its LAST symbol is then cut in half by the end of the block (which
  // does not fall on a boundary). Dropping such a block - at most the first two of a stream, before any
  // UE can be transmitting - is what makes "no uplink sample is copied on the host" absolute.
  const bool slot_aligned_block =
      slot_capable && (nof_samples == nof_samples_per_slot) && ((rx_metadata.ts % nof_samples_per_slot) == 0);
  bool establishes_phase = false;
  if (!symbol_blocks) {
    establishes_phase = slot_capable && !slot_aligned_block && !rx_slot_aligned && (nof_phase_blocks < max_phase_blocks);
    if (establishes_phase) {
      ++nof_phase_blocks;
    }
    if (slot_aligned_block) {
      // The stream is slot aligned from here on: a later loss of alignment (a late or lost block) keeps
      // the historical behaviour, where the uplink processor assembles the symbol the loss split in two
      // instead of dropping samples (see process_symbol_boundary).
      rx_slot_aligned = true;
    }
  } else {
    // Symbol-grained policy: the block is worth processing when the samples it brought start on a symbol
    // boundary, and that is measured on the block that was ACTUALLY received - not on the phase the last
    // timestamp predicted, because the two disagree about the first block of a stream (the RU rounds the
    // start time it gives the lower PHY to a subframe, the radio starts at a sample of its own; see
    // S-7g-10). A block that starts mid-symbol holds the tail of a symbol whose beginning is already
    // gone, so it is dropped - at most max_phase_blocks of them, before any UE can be transmitting.
    const bool symbol_aligned = uplink_processor.locate_symbols(rx_metadata.ts, 1).nof_samples_to_boundary == 0;
    if (!symbol_aligned && (nof_phase_blocks < max_phase_blocks)) {
      ++nof_phase_blocks;
      establishes_phase = true;
    }
    if (!establishes_phase) {
      // The samples stay in the slot buffer, where the transforms of the symbols they complete read them
      // (the buffer is not handed over yet: rx_offset says where this block landed in it).
      rx_fill += nof_samples;
    }
  }


  // Queue uplink buffer processing. A block that only establishes the phase is not queued: its buffer
  // goes out of scope here and returns to the pool (see rx_buffer_pool) - under the symbol-grained policy
  // the slot buffer stays ours and the dropped samples are simply overwritten by the next block.
  // While the processor is stopping, the samples are NOT handed to the uplink processor: it is what feeds
  // the MAC's slot indications, and the upper layers are being taken down at that very moment - a slot
  // indication delivered in the middle of the DU's teardown is what makes that teardown race (observed
  // 2026-09-17 twice, on consecutive runs: intra_slice_scheduler::update_used_dl_vrbs while a slot was
  // being scheduled, and odu::du_ue_drb::stop() on a UE task strand). Those slots belong to a MAC that is
  // going away, so the samples are dropped instead; the receive chain itself still runs to the end of the
  // FSM's countdown (see stop() and wait_stop()), and the blocks already enqueued still run.
  if (!establishes_phase && rx_stop_requested.load(std::memory_order_acquire)) {
    static std::atomic<bool> stop_drop_logged{false};
    bool                     expected = false;
    if (stop_drop_logged.compare_exchange_strong(expected, true)) {
      ocudulog::fetch_basic_logger("PHY").info(
          "Uplink processing stopped: blocks received while stopping are not handed over ({} samples)",
          nof_samples);
    }
  } else if (!establishes_phase) {
    const bool deferred =
        uplink_executor.defer([this,
                               ul_buffer = std::move(rx_buffer),
                               rx_metadata,
                               rx_offset,
                               nof_samples]() mutable {
          trace_point ul_tp = ru_tracer.now();

          // Process UL. The handle travels with the samples: the processor keeps the buffer alive for as
          // long as a transform reads it, and it comes back to this pool when the last of those references
          // is dropped (see return_receive_buffer_to_pool). Nothing here returns it - that is the point.
          // The view is what was actually received: the buffer may be longer than the block the radio was
          // asked for (the pool hands out buffers of one size, the receive asks for less than one), and the
          // samples beyond it are stale.
          baseband_gateway_buffer_reader_view ul_samples(ul_buffer->get_reader(), rx_offset, nof_samples);
          uplink_processor.process(ul_samples, apply_timestamp_sfn0_ref(rx_metadata.ts), std::move(ul_buffer));

          ru_tracer << trace_event("uplink_baseband", ul_tp);
        });
    if (!deferred) {
      // A refused task is never a reason to take the process down. It used to be fatal, on the theory that
      // the executor could only refuse if it were gone - and the shutdown showed that theory is wrong in
      // both directions: the application stops the uplink executor while this receive chain is still
      // running (the S-7g-13 leg aborted here before the lower PHY had even been asked to stop, because a
      // symbol-grained chain enqueues fourteen times as often and meets that window), and the abort then
      // lands in the middle of the shutdown, taking every report with it. The loss the old check guarded
      // against - a block that reaches no transform, silently - is covered instead by counting it and
      // saying so at error level, which is visible in the log and in the pipeline's own counters. The chain
      // is not cut short either way: it ends through the FSM's countdown, which is what wait_stop() waits
      // for, and the blocks already enqueued still run.
      static std::atomic<bool> refused_logged{false};
      bool                     expected = false;
      if (refused_logged.compare_exchange_strong(expected, true)) {
        ocudulog::fetch_basic_logger("PHY").error(
            "Uplink processing task refused: {} samples dropped (the executor is stopping)", nof_samples);
      }
    }
  }

  // Enqueue next iteration if it is running.
  report_fatal_error_if_not(rx_executor.defer([this]() { ul_process(); }), "Failed to execute receive task.");

  rx_state.on_process_end();
}
