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
// fetch_basic_logger(): used by the shutdown path of ul_process (a refused uplink task) as well as by the
// flow probe, so the include is not tied to OCUDU_FLOW_PROBES any more. It used to be, and the macOS build
// still compiled because the logger header arrived transitively there - the Linux/GCC build is the one that
// caught it (the warning did not exist before the S-7g-13 shutdown path).
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/slot_point_extended.h"
#include "ocudu/support/executors/thread_utils.h" // cpu_relax()
#include "ocudu/support/executors/ul_pipeline_probe.h"
#include <cstdlib>
#include <ctime>
#include <limits>

using namespace ocudu;

namespace {

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
  /// Largest `held` and smallest free count seen. For ONE pool the two agree by construction - the queue holds
  /// `free` of the `pool_size` buffers it was filled with, so `held = taken - returned = pool_size - free` - and
  /// printing both lets a reader check that identity instead of trusting it. It does NOT hold across pools:
  /// these counters are process-global while the pool is per-sector (and, in `lower_phy_test`, per fixture), so
  /// that test reports `held_max` far above `pool` with tens of buffers never returned, which is the fixture
  /// swapping pools under one set of counters and not a leak. Measured there: held_max=241, pool=8, free_min=5.
  std::atomic<uint64_t> held_max{0};
  std::atomic<size_t>   free_min{std::numeric_limits<size_t>::max()};
  std::atomic<size_t>   pool_size{0};
};

rx_pool_accounting& rx_pool_accounts()
{
  static rx_pool_accounting accounts;
  return accounts;
}

/// Prints the receive-buffer pool accounting ONCE, next to the other receive counters (see ul_rx_stats_report).
///
/// Registered unconditionally, because rx_pool_note_taken() is not behind a build flag, and silent for a run
/// that never received a block - a leg that never started on air must not print a line of zeroes that reads
/// like a measurement.
void rx_pool_report()
{
  const rx_pool_accounting& a     = rx_pool_accounts();
  const uint64_t            taken = a.taken.load(std::memory_order_relaxed);
  if (taken == 0) {
    return;
  }
  const uint64_t back     = a.returned.load(std::memory_order_relaxed);
  const size_t   free_min = a.free_min.load(std::memory_order_relaxed);
  std::fprintf(stderr,
               "[ul_rx_pool] taken=%llu returned=%llu held_end=%lld held_max=%llu pool=%zu free_min=%lld "
               "starved_takes=%llu starved_events=%llu\n",
               static_cast<unsigned long long>(taken),
               static_cast<unsigned long long>(back),
               static_cast<long long>(taken - back),
               static_cast<unsigned long long>(a.held_max.load(std::memory_order_relaxed)),
               a.pool_size.load(std::memory_order_relaxed),
               (free_min == std::numeric_limits<size_t>::max()) ? -1LL : static_cast<long long>(free_min),
               static_cast<unsigned long long>(a.starved_takes.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(a.starved_events.load(std::memory_order_relaxed)));
}

const bool rx_pool_report_registered = []() {
  std::atexit(rx_pool_report);
  return true;
}();

} // namespace

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
};

ul_rx_stats& ul_rx_counters()
{
  static ul_rx_stats s;
  return s;
}

void ul_rx_stats_report()
{
  const ul_rx_stats& c = ul_rx_counters();
  if (c.blocks.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::fprintf(stderr,
               "[ul_rx] blocks=%llu samples=%llu gaps=%llu gap_samples=%llu ts0_blocks=%llu\n",
               static_cast<unsigned long long>(c.blocks.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.samples.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.gaps.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.gap_samples.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(c.ts0_blocks.load(std::memory_order_relaxed)));
}

const bool ul_rx_stats_registered = []() {
  std::atexit(ul_rx_stats_report);
  register_phy_pipeline_check(
      {"radio sample continuity", []() -> std::optional<bool> {
         const ul_rx_stats& c = ul_rx_counters();
         std::fprintf(stderr,
                      "%llu gaps over %llu blocks (%llu samples missing or repeated), %llu timestamp-0 blocks",
                      static_cast<unsigned long long>(c.gaps.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.blocks.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.gap_samples.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(c.ts0_blocks.load(std::memory_order_relaxed)));
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
    std::chrono::time_point<std::chrono::high_resolution_clock> now     = std::chrono::high_resolution_clock::now();
    std::chrono::nanoseconds                                    elapsed = now - *last_tx_time;

    // Calculate the number of samples from the previous transmission to the next one and convert it seconds.
    float expected_elapsed_s = static_cast<double>(last_tx_buffer_size) / srate.to_Hz<float>();

    // Calculate the minimum elapsed time required to satisfy the throttling time.
    std::chrono::nanoseconds minimum_elapsed(
        static_cast<uint64_t>(expected_elapsed_s * 1e9 * system_time_throttling_ratio));

    if (elapsed < minimum_elapsed) {
      std::this_thread::sleep_until(*last_tx_time + minimum_elapsed);
    }
  }
  last_tx_time.emplace(std::chrono::high_resolution_clock::now());

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

  // Transmit buffer.
  transmitter.transmit(result.buffer->get_reader(), result.metadata);

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

void lower_phy_baseband_processor::ul_process()
{
  // Check if it is running, notify stop and return without enqueueing more tasks.
  if (!rx_state.on_process()) {
    rx_state.on_process_end();
    return;
  }

  // Get receive buffer.
  std::shared_ptr<baseband_gateway_buffer_dynamic_aligned> rx_buffer = rx_pool->buffers.pop_blocking();
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
        rx_fill_buffer = rx_pool->buffers.pop_blocking();
        rx_fill        = 0;
        rx_buffer      = rx_fill_buffer;
        rx_offset      = 0;
        position       = uplink_processor.locate_symbols(next_ts, nof_symbols_per_block);
      }
      nof_samples = position.nof_samples;
    }
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
  baseband_gateway_receiver::metadata rx_metadata = receiver.receive(rx_writer);
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
