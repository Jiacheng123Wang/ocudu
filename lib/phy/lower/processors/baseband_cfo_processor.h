// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/expected.h"
#include "ocudu/adt/ring_buffer.h"
#include "ocudu/adt/span.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_writer.h"
#include "ocudu/ocuduvec/prod.h"
#include "ocudu/phy/lower/processors/lower_phy_cfo_controller.h"
#include "ocudu/phy/lower/sampling_rate.h"
#include "ocudu/support/math/math_utils.h"
#include <atomic>
#include <chrono>

namespace ocudu {

/// \brief Baseband carrier frequency offset processor.
///
/// Applies the configured carrier frequency offset to baseband signals.
class baseband_cfo_processor : public lower_phy_cfo_controller
{
public:
  explicit baseband_cfo_processor(sampling_rate srate_) : srate(srate_) {}

  /// \brief Notifies a new CFO command.
  /// \param time Time at which the new CFO value is used.
  /// \param cfo_Hz New CFO value in Hertz.
  /// \param cfo_drift_Hz_s New CFO drift value in Hertz per second.
  bool schedule_cfo_command(time_point time_, float cfo_Hz_, float cfo_drift_Hz_s_ = 0) override
  {
    cfo_command command{time_, cfo_Hz_, cfo_drift_Hz_s_};
    bool        accepted = cfo_command_queue.try_push(command);
    if (accepted) {
      // One atomic add per command, i.e. per Doppler update of an NTN cell or per `cfo` console
      // command - never per sample. It is what lets the [ul_cfo] report tell "no controller ever
      // asked for a compensation" apart from "a controller asked for exactly 0 Hz" (see get_cfo_hz()).
      nof_scheduled_commands.fetch_add(1, std::memory_order_relaxed);
    }
    return accepted;
  }

  /// Reset sample offset and update the CFO if any command is queued.
  void next_cfo_command()
  {
    // Reset the sample offset.
    sample_offset = 0;

    // Skip if there are no commands.
    if (cfo_command_queue.empty()) {
      return;
    }

    // Get the reference of the next command.
    const cfo_command& command             = *cfo_command_queue.begin();
    auto [cfo_start_ts, cfo_Hz, cfo_drift] = command;

    // Get the current time.
    auto now = std::chrono::system_clock::now();

    // If the time for the command has come...
    if (now >= cfo_start_ts) {
      // Update the current normalized CFO.
      initial_cfo         = cfo_Hz / srate.to_Hz<float>();
      current_cfo         = initial_cfo;
      cfo_start_timestamp = cfo_start_ts;
      cfo_drift_hz_s      = cfo_drift;

      // Pop the current command.
      cfo_command_queue.pop();
    }

    if (std::isnormal(cfo_drift_hz_s)) {
      auto elapsed_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - cfo_start_timestamp).count();
      current_cfo          = initial_cfo + (cfo_drift_hz_s * elapsed_time_ms / 1e3) / srate.to_Hz<float>();
    }
  }

  /// Increments the CFO sample offset by a number of samples.
  void advance(unsigned nof_samples) { sample_offset += nof_samples; }

  /// \brief Offset currently in effect, in hertz.
  ///
  /// Zero while nothing has scheduled a command, which is the state of a terrestrial cell: the only
  /// writers of this queue are the NTN Doppler adapter (wired only when a cell configures NTN) and the
  /// `cfo` console command of the application.
  float get_cfo_hz() const { return current_cfo * srate.to_Hz<float>(); }

  /// Number of commands accepted since construction (probe, see schedule_cfo_command()).
  uint64_t get_nof_scheduled_commands() const { return nof_scheduled_commands.load(std::memory_order_relaxed); }

  /// \brief Whether \ref process() would modify the samples it is given.
  ///
  /// False while no usable offset is in effect: the processor's initial state, and what a scheduled
  /// command of 0 Hz leaves behind. Callers use it to skip the int16 -> float -> int16 round trip
  /// they wrap around \ref process(): with no offset to apply that round trip would be the only
  /// thing happening to the samples, and it is the exact identity (float(x) / 32767 * 32767 rounds
  /// back to x for every int16 - see baseband_cfo_processor_test), so skipping it leaves the samples
  /// exactly as the radio delivered them.
  bool applies_compensation() const { return std::isnormal(current_cfo); }

  /// Applies carrier frequency offset in-place to a baseband buffer.
  void process(baseband_gateway_buffer_writer& buffer) const
  {
    // Skip CFO process if the current CFO is zero, NaN or infinity.
    if (!applies_compensation()) {
      return;
    }

    // Calculate the initial phase of the block in radians.
    float initial_phase = TWOPI * current_cfo * static_cast<float>(sample_offset);

    // Apply CFO to each channel.
    for (unsigned i_port = 0, i_port_end = buffer.get_nof_channels(); i_port != i_port_end; ++i_port) {
      span<ci16_t> buff = buffer.get_channel_buffer(i_port);
      ocuduvec::prod_cexp(buff, buff, current_cfo, initial_phase);
    }
  }

private:
  /// CFO command data type.
  using cfo_command = std::tuple<time_point, float, float>;
  /// Maximum number of commands that can be enqueued.
  static constexpr unsigned max_nof_commands = 128;

  /// Baseband sampling rate
  sampling_rate srate;
  /// Queue of CFO commands.
  static_ring_buffer<cfo_command, max_nof_commands> cfo_command_queue;
  /// Current sample count for keeping the phase coherent between calls.
  unsigned sample_offset = 0;
  /// Current normalized CFO.
  float current_cfo = 0.0;
  /// Commands accepted since construction (probe only, see schedule_cfo_command()).
  std::atomic<uint64_t> nof_scheduled_commands{0};
  /// Normalized CFO at the start time.
  float initial_cfo = 0.0;
  /// Current CFO start timestamp.
  time_point cfo_start_timestamp;
  /// Current CFO drift.
  float cfo_drift_hz_s = 0.0;
};

} // namespace ocudu
