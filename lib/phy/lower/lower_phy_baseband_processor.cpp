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

using namespace ocudu;

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
  // [zmq-probe] instrumentation (compiled only with ENABLE_FLOW_PROBES).
  {
    auto recv_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_recv_begin).count();
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
