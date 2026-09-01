// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lower_phy_baseband_processor.h"
#include "ocudu/adt/format.h"
#include "ocudu/adt/interval.h"
#include "ocudu/instrumentation/traces/ru_traces.h"
#if defined(OCUDU_FLOW_PROBES)
#include "ocudu/ocudulog/ocudulog.h" // [zmq-probe] temporary: fetch_basic_logger
#endif
#include "ocudu/ran/slot_point_extended.h"
#include "ocudu/support/executors/thread_utils.h" // cpu_relax()
#include "ocudu/support/executors/ul_pipeline_probe.h"
#include <ctime>

using namespace ocudu;

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
  rx_buffers(config.nof_rx_buffers),
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

  // Create queue of receive buffers.
  while (!rx_buffers.full()) {
    rx_buffers.push_blocking(std::make_unique<baseband_gateway_buffer_dynamic>(config.nof_rx_ports, rx_buffer_size));
  }
}

void lower_phy_baseband_processor::start(baseband_gateway_timestamp init_time, baseband_gateway_timestamp sfn0_ref_time)
{
  // If it is required to start with system frame number 0, then set a time offset to start an SFN earlier.
  start_time_sfn0   = sfn0_ref_time;
  last_rx_timestamp = init_time;

  rx_state.start();
  report_fatal_error_if_not(rx_executor.defer([this]() { ul_process(); }), "Failed to execute initial uplink task.");

  tx_state.start();
  report_fatal_error_if_not(tx_executor.defer([this, init_time]() { dl_process(init_time + rx_to_tx_max_delay); }),
                            "Failed to execute initial downlink task.");
}

void lower_phy_baseband_processor::stop()
{
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
  std::unique_ptr<baseband_gateway_buffer_dynamic> rx_buffer = rx_buffers.pop_blocking();

  // Receive baseband.
  trace_point tp = ru_tracer.now();
#if defined(OCUDU_FLOW_PROBES)
  const auto t_recv_begin = std::chrono::steady_clock::now();
#endif
  baseband_gateway_receiver::metadata rx_metadata = receiver.receive(rx_buffer->get_writer());
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

  // T_start of the UL compute pipeline measurement (IQ samples just received, UL processing about to start).
  // Use the same slot reference the FAPI slot_point carries to the PUSCH completion (the sample-timestamp-derived
  // count modulo the SFN cycle, as computed by the uplink processor): with the plain absolute count the pairing
  // only matched during the first SFN cycle of the run.
  {
    const uint64_t samples_per_slot        = srate.to_kHz() * static_cast<uint64_t>(slot_duration.count()) / 1000;
    const uint64_t nof_slots_per_sfn_cycle = (nof_samples_in_all_hyper_frames / NOF_HYPER_SFNS) / samples_per_slot;
    ul_pipeline_probe::get().record_start((apply_timestamp_sfn0_ref(rx_metadata.ts) / samples_per_slot) %
                                          nof_slots_per_sfn_cycle);
  }

  // Update last timestamp.
  last_rx_timestamp.store(rx_metadata.ts + rx_buffer->get_nof_samples(), std::memory_order_release);

  // Queue uplink buffer processing.
  report_fatal_error_if_not(uplink_executor.defer([this, ul_buffer = std::move(rx_buffer), rx_metadata]() mutable {
    trace_point ul_tp = ru_tracer.now();

    // Process UL.
    uplink_processor.process(ul_buffer->get_reader(), apply_timestamp_sfn0_ref(rx_metadata.ts));

    // Return buffer to receive.
    rx_buffers.push_blocking(std::move(ul_buffer));

    ru_tracer << trace_event("uplink_baseband", ul_tp);
  }),
                            "Failed to execute uplink processing task.");

  // Enqueue next iteration if it is running.
  report_fatal_error_if_not(rx_executor.defer([this]() { ul_process(); }), "Failed to execute receive task.");

  rx_state.on_process_end();
}
