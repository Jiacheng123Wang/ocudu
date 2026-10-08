// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "radio_uhd_rx_stream.h"
#include "ocudu/support/executors/stall_site.h"
#include "ocudu/ocuduvec/zero.h"

using namespace ocudu;

/// Receive timeout in seconds.
static constexpr double RECEIVE_TIMEOUT_S = 0.2f;

/// Receive calls in a row that may deliver no sample before the block is abandoned (see no_progress_trials).
static constexpr unsigned MAX_NO_PROGRESS_TRIALS = 10;
/// Set to true for receiving data in a single packet.
static constexpr bool ONE_PACKET = false;

bool radio_uhd_rx_stream::receive_block(unsigned&                       nof_rxd_samples,
                                        baseband_gateway_buffer_writer& data,
                                        unsigned                        offset,
                                        uhd::rx_metadata_t&             md)
{
  // Extract number of samples.
  unsigned num_samples = data.get_nof_samples() - offset;

  // Make sure the number of channels is equal.
  ocudu_assert(data.get_nof_channels() == nof_channels, "Number of channels does not match.");

  // Flatten buffers.
  static_vector<void*, RADIO_MAX_NOF_CHANNELS> buffs_flat_ptr;
  for (unsigned channel = 0; channel != nof_channels; ++channel) {
    buffs_flat_ptr.emplace_back(reinterpret_cast<void*>(data[channel].subspan(offset, num_samples).data()));
  }

  uhd::rx_streamer::buffs_type buffs_cpp(buffs_flat_ptr.data(), nof_channels);

  return safe_execution([this, buffs_cpp, num_samples, &md, &nof_rxd_samples]() {
    nof_rxd_samples = stream->recv(buffs_cpp, num_samples, md, RECEIVE_TIMEOUT_S, ONE_PACKET);
  });
}

radio_uhd_rx_stream::radio_uhd_rx_stream(uhd::usrp::multi_usrp::sptr& usrp,
                                         const stream_description&    description,
                                         radio_event_notifier&        notifier_) :
  id(description.id), srate_Hz(description.srate_Hz), notifier(notifier_)
{
  ocudu_assert(std::isnormal(srate_Hz) && (srate_Hz > 0.0), "Invalid sampling rate {}.", srate_Hz);

  // Build stream arguments.
  uhd::stream_args_t stream_args = {};
  stream_args.cpu_format         = "sc16";
  switch (description.otw_format) {
    case radio_configuration::over_the_wire_format::DEFAULT:
    case radio_configuration::over_the_wire_format::SC16:
      stream_args.otw_format = "sc16";
      break;
    case radio_configuration::over_the_wire_format::SC12:
      stream_args.otw_format = "sc12";
      break;
    case radio_configuration::over_the_wire_format::SC8:
      stream_args.otw_format = "sc8";
      break;
  }
  stream_args.args     = description.args;
  stream_args.channels = description.ports;

  if (!safe_execution([this, usrp, &stream_args]() {
        stream          = usrp->get_rx_stream(stream_args);
        max_packet_size = stream->get_max_num_samps();
        nof_channels    = stream->get_num_channels();
      })) {
    return;
  }

  is_init_successful = true;
}

bool radio_uhd_rx_stream::start(const uhd::time_spec_t& time_spec)
{
  if (!is_init_successful) {
    return false;
  }

  stop_control.reset();

  if (!safe_execution([this, &time_spec]() {
        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        stream_cmd.time_spec  = time_spec;
        stream_cmd.stream_now = (time_spec.get_real_secs() == uhd::time_spec_t());

        stream->issue_stream_cmd(stream_cmd);
      })) {
    fmt::println("Error: failed to start receive stream {}. {}.", id, get_error_message().c_str());
  }

  return true;
}

baseband_gateway_receiver::metadata radio_uhd_rx_stream::receive(baseband_gateway_buffer_writer& buffs)
{
  baseband_gateway_receiver::metadata ret = {};
  uhd::rx_metadata_t                  md;

  auto token = stop_control.get_token();
  if (OCUDU_UNLIKELY(token.is_stop_requested())) {
    for (unsigned i = 0, e = buffs.get_nof_channels(); i != e; ++i) {
      ocuduvec::zero(buffs[i]);
    }
    ret.ts = md.time_spec.to_ticks(srate_Hz);
    return ret;
  }

  unsigned nsamples            = buffs[0].size();
  unsigned rxd_samples_total   = 0;
  unsigned timeout_trial_count = 0;
  /// Receive calls in a row that delivered NO sample, whatever their error code.
  ///
  /// The timeout counter above bounds one error code, and it is not the one a struggling host sees: a receive
  /// ring that fills because the host cannot drain it fast enough comes back as OVERFLOW, a late command as
  /// LATE_COMMAND, and both of those went back to the top of this loop with \c rxd_samples_total untouched. A
  /// stream that keeps answering that way spins here forever, and the task spinning is the one the lower PHY's
  /// stop waits for - so the gNB never stops and an interrupt ends in the 5-second alarm and SIGKILL.
  ///
  /// Measured on a B210 with `--expert_phy.phy_pipeline cpu`, whose log was full of `Real-time failure in RF:
  /// underflow` and `late`: with the PHY on the host the receive ring is drained slower than the radio fills
  /// it, so the block arrives as OVERFLOW and this loop never advances. The GPU pipeline keeps up and shuts
  /// down cleanly on the same radio, which is why the mode mattered.
  unsigned no_progress_trials = 0;

  // The radio receive is the call the whole uplink waits on, and the one a USB/driver stall would hold.
  ocudu::stall_site_scope waiting("radio.rx");

  // Receive stream in multiple blocks.
  while (rxd_samples_total < nsamples) {
    const unsigned samples_before_call = rxd_samples_total;
    unsigned       rxd_samples         = 0;
    if (!receive_block(rxd_samples, buffs, rxd_samples_total, md)) {
      fmt::println("Error: failed receiving packet. {}.", get_error_message().c_str());
      return {};
    }

    // Save timespec for first block only if the last timestamp is unknown.
    if (rxd_samples_total == 0) {
      ret.ts = md.time_spec.to_ticks(srate_Hz);
      // ... and the ABSOLUTE time of that same sample when the radio reports one (see metadata::absolute_ns).
      // `get_frac_secs()` is a double in [0, 1), so converting the two parts separately keeps the pair's
      // resolution at well under a microsecond; a single `get_real_secs() * 1e9` would leave ~100 ns of
      // rounding at GPSDO-sized epochs because the double has to carry ~19 significant digits.
      if (md.time_spec.get_real_secs() != 0.0) {
        ret.absolute_ns = static_cast<int64_t>(md.time_spec.get_full_secs()) * 1000000000LL +
                          static_cast<int64_t>(md.time_spec.get_frac_secs() * 1e9);
      }
    }

    // Increase the total amount of received samples.
    rxd_samples_total += rxd_samples;

    // Prepare notification event.
    radio_event_notifier::event_description event = {.stream_id  = id,
                                                     .channel_id = std::nullopt,
                                                     .source     = radio_event_source::RECEIVE,
                                                     .type       = radio_event_type::UNDEFINED,
                                                     .timestamp  = std::nullopt};

    // Handle error.
    switch (md.error_code) {
      case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
        ++timeout_trial_count;
        if (timeout_trial_count >= 10) {
          fmt::println("Error: exceeded maximum number of timed out receive calls.");
          return ret;
        }
        break;
      case uhd::rx_metadata_t::ERROR_CODE_NONE:
        // Ignored.
        break;
      case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
        event.type = radio_event_type::LATE;
        ret.error  = baseband_gateway_receiver::rx_error::late;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
        event.type = radio_event_type::OVERFLOW;
        // Dev doc 6.51: the receive ring filled and the radio DROPPED samples. The host cannot repair this,
        // and it is the event the lower PHY's continuity check sees as a gap - so it travels with the block.
        ret.error = baseband_gateway_receiver::rx_error::overflow;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
      case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
      case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
        event.type = radio_event_type::OTHER;
        ret.error  = baseband_gateway_receiver::rx_error::other;
        break;
    }

    // Notify if the event type was set.
    if (event.type != radio_event_type::UNDEFINED) {
      notifier.on_radio_rt_event(event);
    }

    // Bound the calls that deliver nothing, whatever their error code (see no_progress_trials). A healthy
    // stream always advances the total, so the counter resets on every block that carries samples and this
    // fires only when the radio has stopped answering with data.
    if (rxd_samples_total == samples_before_call) {
      if (++no_progress_trials >= MAX_NO_PROGRESS_TRIALS) {
        fmt::println("Error: {} receive calls in a row delivered no sample (last error code {}); giving up on "
                     "this block.",
                     MAX_NO_PROGRESS_TRIALS,
                     static_cast<int>(md.error_code));
        // The block did not arrive: say so rather than passing off whatever the buffer holds as a delivery.
        // The lower PHY drops a block carrying this code, exactly as it drops one that arrives while it is
        // stopping (see baseband_gateway_receiver::rx_error::no_data).
        ret.error = baseband_gateway_receiver::rx_error::no_data;
        return ret;
      }
    } else {
      no_progress_trials = 0;
    }
  }

  // If it reaches here, there is no error.
  return ret;
}

bool radio_uhd_rx_stream::stop()
{
  stop_control.stop();

  // Try to stop the stream.
  if (!safe_execution([this]() {
        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        stream_cmd.time_spec  = uhd::time_spec_t();
        stream_cmd.stream_now = true;

        stream->issue_stream_cmd(stream_cmd);
      })) {
    fmt::println("Error: failed to stop stream {}. {}.", id, get_error_message().c_str());
    return false;
  }

  return true;
}
