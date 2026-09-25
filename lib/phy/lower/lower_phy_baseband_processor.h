// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/blocking_queue.h"
#include "ocudu/gateways/baseband/baseband_gateway_receiver.h"
#include "ocudu/gateways/baseband/baseband_gateway_timestamp.h"
#include "ocudu/gateways/baseband/baseband_gateway_transmitter.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/phy/lower/lower_phy_controller.h"
#include "ocudu/phy/phy_pipeline_grid_ready.h"
#include "ocudu/phy/lower/processors/downlink/downlink_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h"
#include "ocudu/phy/lower/sampling_rate.h"
#include "ocudu/support/executors/flow_probe.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/macos_compat.h"
#include <future>

namespace ocudu {
/// Collects the parameters necessary to initialize the baseband adaptor.
struct lower_phy_baseband_processor_configuration {
  /// Sampling rate.
  sampling_rate srate;
  /// Subcarrier spacing.
  subcarrier_spacing scs;
  /// Number of transmit ports.
  unsigned nof_tx_ports;
  /// Number of receive ports.
  unsigned nof_rx_ports;
  /// Receive to transmit delay in samples.
  baseband_gateway_timestamp tx_time_offset;
  /// Maximum number of samples between the last received sample and the next sample to transmit time instants.
  baseband_gateway_timestamp rx_to_tx_max_delay;
  /// Receive buffers size.
  unsigned rx_buffer_size;
  /// Number of receive buffers of size \c rx_buffer_size.
  unsigned nof_rx_buffers;
  /// System time-based throttling. See \ref lower_phy_configuration::system_time_throttling.
  float system_time_throttling;
  /// Number of slots to execute before a complete stop after requesting to stop.
  unsigned stop_nof_slots;
};

/// Collects the necessary dependencies to initialize the baseband adaptor.
struct lower_phy_baseband_processor_dependencies {
  /// \brief Receive task executor.
  ///
  /// Receives baseband samples from the \ref baseband_gateway_receiver, reserves baseband buffers and pushes
  /// tasks to the other executors.
  task_executor& rx_task_executor;
  /// \brief Transmit task executor.
  ///
  /// Transmits baseband samples and releases the downlink baseband processing buffer to the pool.
  task_executor& tx_task_executor;
  /// \brief Uplink task executor.
  ///
  /// Notifies uplink-related time boundaries, runs the baseband demodulation and notifies availability of data.
  task_executor& ul_task_executor;
  /// Baseband receiver gateway.
  baseband_gateway_receiver& receiver;
  /// Baseband transmitter gateway.
  baseband_gateway_transmitter& transmitter;
  /// Uplink baseband processor.
  uplink_processor_baseband& ul_bb_proc;
  /// Downlink processor baseband.
  downlink_processor_baseband& dl_bb_proc;
};

/// \brief Implements the lower physical layer baseband processing core.
///
/// This class interfaces and manages the baseband data flow between the baseband gateways and the processors. This
/// class is agnostic to the sampling rate and radio frame timing.
class lower_phy_baseband_processor : public lower_phy_controller
{
public:
  /// Constructs a baseband adaptor.
  lower_phy_baseband_processor(const lower_phy_baseband_processor_configuration& config,
                               const lower_phy_baseband_processor_dependencies&  deps);

  // See interface for documentation.
  void start(baseband_gateway_timestamp init_time, baseband_gateway_timestamp sfn0_ref_time) override;

  // See interface for documentation.
  void stop() override;

private:
  /// Internal finite state machine to control the internal state.
  class internal_fsm
  {
  public:
    /// Initialize the internal FSM with the number of processing slots required to close the lower PHY.
    explicit internal_fsm(unsigned stop_count) : state_stopped(state_wait_stop + stop_count) {}

    /// Default destructor - It reports a fatal error if the state is \c running or \c wait_stop.
    ~internal_fsm()
    {
      uint32_t current_state = state.load();
      report_fatal_error_if_not((current_state == state_idle) || (current_state >= state_stopped), "Unexpected state.");
    }

    /// \brief Notifies the start of the processing.
    /// \remark A fatal error is reported if start() is called while the processor is not in \c idle state.
    void start()
    {
      uint32_t expected_state = state_idle;
      bool     success        = state.compare_exchange_strong(expected_state, state_running);
      report_fatal_error_if_not(
          success, "The starting expected state is 0x{:08x} (idle) but found 0x{:08x}.", state_idle, expected_state);
    }

    /// \brief Requests all asynchronous processing to stop.
    /// \remark A fatal error is reported if request_stop() is called more than once.
    void request_stop()
    {
      uint32_t previous_state = state.fetch_xor(state_wait_stop);
      report_fatal_error_if_not((previous_state & state_wait_stop) == 0, "Stopping has been requested more than once.");
    }

    /// \brief Waits for all asynchronous processing to stop.
    /// \remark A fatal error is reported if wait_stop() is called while it is \c idle or \c running without calling
    /// first request_stop().
    void wait_stop()
    {
      report_fatal_error_if_not((state.load() & state_wait_stop) != 0, "Unexpected state.");

      // Wait for the state to transition to stop.
      stop_control.get_future().wait();
    }

    /// \brief Call on the event of processing.
    /// \return \c true if the state is running, otherwise \c false.
    bool on_process()
    {
      // Detect stop mask.
      if ((state.load() & state_wait_stop) != 0) {
        // Increment the process count before considering stopped.
        uint32_t current_state = state.fetch_add(1) + 1;
        if (current_state >= state_stopped) {
          // Platform mapping lives in the compat layer: Linux completes the stop here (upstream behaviour);
          // macOS defers it to on_process_end(), where the task has actually finished.
          compat::lower_phy_stop_chain_end(stop_control);
          return false;
        }
      }
      return true;
    }

    /// \brief Call when the processing task finishes.
    void on_process_end()
    {
      // Platform mapping lives in the compat layer (no-op on Linux). On macOS only the task that ended the
      // sequential processing chain observes the state at or past the stop threshold: signal the completion of
      // the stop. This guarantees that stop() waits until all processing tasks have finished, not merely started.
      const uint32_t current_state = state.load(std::memory_order_relaxed);
      compat::lower_phy_stop_task_end(current_state,
                                      (current_state & state_wait_stop) != 0,
                                      state_stopped,
                                      stop_control);
    }

  private:
    /// State value in idle.
    static constexpr uint32_t state_idle = 0x7fffffff;
    /// State value while running.
    static constexpr uint32_t state_running = 0x00000000;
    /// State mask while the lower PHY is stopping.
    static constexpr uint32_t state_wait_stop = 0x80000000;
    /// Stopped state, depends on the maximum processing delay number of slots.
    const uint32_t state_stopped;

    /// Actual state.
    std::atomic<uint32_t> state{state_idle};
    /// Promise for controlling the stop sequence.
    std::promise<void> stop_control;
  };

  /// \brief Processes downlink baseband.
  /// \param[in] timestamp Current processing time.
  void dl_process(baseband_gateway_timestamp timestamp);

  /// Processes uplink baseband.
  void ul_process();

  /// \brief Subtracts the System Frame Number (SFN) Zero reference time to a given timestamp.
  ///
  /// To avoid an overflow in the substraction, a number of samples is added to the timestamp that results in the same
  /// hyper-SFN, SFN and slot.
  baseband_gateway_timestamp apply_timestamp_sfn0_ref(baseband_gateway_timestamp timestamp) const
  {
    // Add the time of all the hyper frames to avoid overflow and keep the SLOT.indication continuous in hyper-SFN
    // number.
    if (timestamp < start_time_sfn0) {
      timestamp += divide_ceil(start_time_sfn0, nof_samples_in_all_hyper_frames) * nof_samples_in_all_hyper_frames;
    }

    return timestamp - start_time_sfn0;
  }

  sampling_rate                                                              srate;
  uint64_t                                                                   nof_samples_in_all_hyper_frames;
  unsigned                                                                   rx_buffer_size;
  std::chrono::microseconds                                                  slot_duration;
  float                                                                      system_time_throttling_ratio;
  task_executor&                                                             rx_executor;
  task_executor&                                                             tx_executor;
  task_executor&                                                             uplink_executor;
  baseband_gateway_receiver&                                                 receiver;
  baseband_gateway_transmitter&                                              transmitter;
  uplink_processor_baseband&                                                 uplink_processor;
  downlink_processor_baseband&                                               downlink_processor;
  /// \brief The receive buffers, and the rule that returns one to them.
  ///
  /// A separate object, held by shared_ptr: a buffer comes back here when the uplink processor drops
  /// the last reference it kept for an in-flight transform (see
  /// uplink_processor_baseband::rx_buffer_handle), and that can happen after this processor has been
  /// destroyed - a handle outliving its pool must not touch it. The deleter holds a WEAK reference for
  /// exactly that reason: the pool does not keep its buffers alive through it (that would be a cycle),
  /// and a release that arrives too late frees the buffer instead of deadlocking on a queue that is
  /// being destroyed - which is what a strong reference here did: the queue's destructor destroyed its
  /// buffers, whose deleter pushed them back into the same queue.
  struct rx_buffer_pool {
    explicit rx_buffer_pool(unsigned nof_buffers) : buffers(nof_buffers) {}

    blocking_queue<std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>> buffers;

    struct deleter {
      std::weak_ptr<rx_buffer_pool> pool;

      void operator()(baseband_gateway_buffer_dynamic_aligned* buffer) const
      {
        if (std::shared_ptr<rx_buffer_pool> alive = pool.lock()) {
          lower_phy_baseband_processor::rx_pool_note_return();
          alive->buffers.push_blocking(
              std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>(buffer, *this));
          return;
        }
        delete buffer;
      }
    };
  };

  /// \brief Receive-buffer accounting (D1 diagnostics): how many the radio took, and how many came back.
  ///
  /// What it exists for: with the block hand-over armed the uplink holds a receive buffer until the command
  /// buffer carrying its transforms COMPLETES (that is what the input's lifetime token does, design document
  /// 5.9.7-5.9.10), and the pool is the backpressure the receive loop blocks on. `taken - returned` is
  /// therefore the number of buffers the whole chain is holding: if it climbs to the pool size and stays
  /// there, the references are not coming back at all - a completely different defect from a long hold, and
  /// the two look identical from the outside (real-time failures, a stalled radio).
  static void rx_pool_note_taken(size_t free_buffers, size_t pool_size);
  /// \brief Takes a receive buffer, asking the hand-over to reap while the pool is DRY (Q9-A, dev doc 6.13).
  ///
  /// The plain `pop_blocking()` parks this thread until a buffer comes back, and that is exactly the state in
  /// which the hand-over's registry cannot help itself: with the receive thread parked there is no deposit, so
  /// none of the registry's entry points runs, and a block nobody claimed keeps the buffer this call is
  /// waiting for out of the pool. So before parking, this asks the registry to reap (`handover_reap_hook`),
  /// and it waits in bounded slices so a stall that outlives that one reap asks again. Measured on leg
  /// `p08-conc2`: the worst block's claim came 5.945 s after its deposit and its command buffer then
  /// completed in 2.6 ms - the whole 5.9 s was the registry waiting for a caller that never came.
  ///
  /// \note The healthy path is untouched: a pool that has a buffer hands it out through `try_pop()` and pays
  ///       nothing - not even one hook call. A stopped queue returns a null buffer, exactly as the plain
  ///       `pop_blocking()` does.
  std::shared_ptr<baseband_gateway_buffer_dynamic_aligned> pop_rx_buffer_blocking();

  /// \brief How long a dry pool waits before it asks the hand-over to reap again (see pop_rx_buffer_blocking()).
  ///
  /// 10 ms is two orders of magnitude below the stall this exists for (5 s) and far above a reaped block's
  /// commit + completion (~ms), so a healthy pool is never asked twice for one stall and a stalled one is
  /// asked often enough that its buffers come back as soon as the registry can give them.
  static constexpr std::chrono::milliseconds rx_reap_slice{10};

  /// \brief P0-2: records how long the take BLOCKED (the `pop_blocking()` above it), in microseconds.
  ///
  /// The one wait the probe report cannot see: `[ul_rx_wait]` brackets `receiver.receive()`, and the take
  /// happens before it, so a receive thread parked on an empty pool is invisible in every other series -
  /// measured on `s88-laneconc2`: EMPTY pool, a 5.002 s park, the USRP queue overflowed and the leg lost
  /// 2 x ~5 s of samples, while `[ul_rx_wait]` read its usual ~101 ms maximum.
  static void rx_pool_note_wait(int64_t wait_us);
  static void rx_pool_note_return();

  /// The receive buffers of this sector (see rx_buffer_pool), sized by the configuration.
  std::shared_ptr<rx_buffer_pool> rx_pool;

  baseband_gateway_timestamp                                                 tx_time_offset;
  baseband_gateway_timestamp                                                 rx_to_tx_max_delay;
  baseband_gateway_timestamp                                                 start_time_sfn0;
  internal_fsm                                                               tx_state;
  internal_fsm                                                               rx_state;
  std::atomic<baseband_gateway_timestamp>                                    last_rx_timestamp;
  /// \brief Set by stop(): the receive chain is going down (see ul_process).
  ///
  /// Used to tell a task refused because the application is taking the sector down - expected, and not a
  /// reason to abort the process during its own shutdown - from one refused while the stream was supposed
  /// to be running, which is a defect and stays fatal.
  std::atomic<bool>                                                          rx_stop_requested{false};
  /// \brief Whether the receive blocks have been slot aligned since the stream started.
  ///
  /// The radio starts streaming at a sample the PHY does not choose, so the first blocks of a stream do
  /// not start on a slot boundary and are not processed (see ul_process): a block that holds a whole
  /// slot but starts mid-slot is the one whose last symbol is cut in half by its end, and assembling
  /// that symbol is the one host copy of the samples the receive side can force. Set once a block is
  /// received that holds exactly one slot and starts on its boundary; cleared by start().
  bool                                                                       rx_slot_aligned = false;
  /// Number of blocks dropped while the stream establishes its phase (see ul_process). Bounded: a
  /// stream that is still not aligned after that many blocks is not one the phase logic understands,
  /// and the uplink processor then handles its blocks as before (assembling what straddles).
  static constexpr unsigned                                                  max_phase_blocks = 2;
  unsigned                                                                   nof_phase_blocks = 0;
  /// \brief Symbol-grained receive policy (see ul_process): whether the uplink processor describes the
  /// OFDM symbol grid, i.e. whether the receive side can ask for whole symbols instead of whole slots.
  bool                                                                       rx_symbol_grid_known = false;
  /// Symbols one receive block covers under the symbol-grained policy. 0 keeps the historical
  /// whole-slot blocks; 1 starts the front end as early as the samples allow.
  unsigned                                                                   nof_symbols_per_block = 1;
  /// \brief Slot buffer the symbol-grained policy is filling, or null when it is not in use.
  ///
  /// A receive block may hold fewer samples than a slot, so one buffer is filled across several calls and
  /// retired when it holds a whole slot's worth of samples (see ul_process). The buffer returns to the
  /// pool when its last reference is dropped: the one held here, and one per in-flight transform reading
  /// it (see the handle the uplink processor is given).
  std::shared_ptr<baseband_gateway_buffer_dynamic_aligned>                    rx_fill_buffer;
  /// Samples of rx_fill_buffer written by the radio so far.
  unsigned                                                                   rx_fill = 0;
  std::optional<std::chrono::time_point<std::chrono::steady_clock>> last_tx_time;
  unsigned                                                                   last_tx_buffer_size = 0;
  /// Flow instrumentation probe for the DL production rate (debug aid for cross-platform comparison).
  flow_probe dl_probe{"dl_proc"};
  /// Inter-slot gap jitter statistics for the DL production (debug aid for cross-platform comparison).
  flow_interval_probe dl_jitter_probe{"dl_proc"};
};
} // namespace ocudu
