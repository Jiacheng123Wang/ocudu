// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Is the B200's receive transport healthy, and does a device argument break it? (dev doc 6.54, arm C)
///
/// WHY IT EXISTS. The fused lane's remaining discontinuities are RX ring overflows (dev doc 6.51/6.53): the
/// radio drops samples because nobody drained it in time, and the per-event context says the stall is inside
/// the transport call, not in our receive thread (dev doc 6.53 (2)). Every question after that is about the
/// RADIO and the HOST, not about the PHY - and answering it with a leg costs a phone test, a UE attach and
/// four minutes of air time, which is how arm C1 (`recv_frame_size=16384`) "failed": the phone never attached
/// and the leg could not say whether the arm or the radio was at fault.
///
/// This tool asks the radio directly, with no gNB, no UE and no PHY: it opens the B200 with the given device
/// arguments, streams RX at the given rate and format, and reports the same continuity and error readings the
/// lower PHY counts (`[ul_rx]`/`[ul_rx_timing]`). So a device argument can be A/B'd in fifteen seconds, and a
/// suspect radio can be declared healthy or not BEFORE a leg is spent on it.
///
/// WHAT IT DOES NOT DO: it does not transmit, so it does not reproduce the TX side's load (the leg runs both
/// directions over one USB link); a clean run here therefore does not prove a clean leg. It is a filter, not a
/// verdict.
///
/// Usage:
///   uhd_rx_health [--args "type=b200,num_recv_frames=256,num_send_frames=64"]
///                 [--seconds 10] [--rate 23.04e6] [--freq 3.13632e9] [--gain 70]
///                 [--otw sc12] [--block-ms 0.5] [--quiet-device]
/// Output (stderr, one line per reading):
///   [uhd_rx_health] opened: <mboard/device summary>
///   [uhd_rx_health] args=... rate=... freq=... gain=... otw=... block=N samples
///   [uhd_rx_health] blocks=N samples=N recv(max=us mean=us over1ms=N) gaps=N gap_samples=N max_gap_us=N
///   [uhd_rx_health] radio errors: none=N overflow=N late=N timeout=N broken_chain=N alignment=N bad_packet=N
///   [uhd_rx_health] VERDICT: ...
/// Build (macOS, Homebrew UHD):
///   clang++ -std=c++17 -O2 -I /opt/homebrew/include uhd_rx_health.cpp -L /opt/homebrew/lib -luhd \
///     -Wl,-rpath,/opt/homebrew/lib -o /tmp/uhd_rx_health

#include <uhd/types/stream_cmd.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

/// One error bucket per UHD receive error code, so the report separates the shapes a host can act on.
struct error_counts {
  unsigned long long none = 0;
  unsigned long long overflow = 0;
  unsigned long long late = 0;
  unsigned long long timeout = 0;
  unsigned long long broken_chain = 0;
  unsigned long long alignment = 0;
  unsigned long long bad_packet = 0;

  void note(uhd::rx_metadata_t::error_code_t code)
  {
    switch (code) {
      case uhd::rx_metadata_t::ERROR_CODE_NONE:
        ++none;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
        ++overflow;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
        ++late;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
        ++timeout;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
        ++broken_chain;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
        ++alignment;
        break;
      case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
        ++bad_packet;
        break;
    }
  }
};

} // namespace

int main(int argc, char** argv)
{
  std::string args     = "type=b200,num_recv_frames=256,num_send_frames=64";
  double      seconds  = 10.0;
  double      rate     = 23.04e6;
  double      freq     = 3.13632e9; // band n78, ARFCN 627264 (the leg config's DL ARFCN)
  double      gain     = 70.0;      // the leg config's rx_gain
  std::string otw      = "sc12";    // the leg config's otw_format
  double      block_ms = 0.5;       // one slot at 30 kHz
  bool        with_tx  = false;     // stream TX as well (the leg runs both directions over one USB link)

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto        next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "--args") {
      args = next();
    } else if (a == "--seconds") {
      seconds = std::strtod(next().c_str(), nullptr);
    } else if (a == "--rate") {
      rate = std::strtod(next().c_str(), nullptr);
    } else if (a == "--freq") {
      freq = std::strtod(next().c_str(), nullptr);
    } else if (a == "--gain") {
      gain = std::strtod(next().c_str(), nullptr);
    } else if (a == "--otw") {
      otw = next();
    } else if (a == "--tx") {
      with_tx = true;
    } else if (a == "--block-ms") {
      block_ms = std::strtod(next().c_str(), nullptr);
    } else if ((a == "-h") || (a == "--help")) {
      std::printf("usage: %s [--args <device args>] [--seconds N] [--rate Hz] [--freq Hz] [--gain dB] "
                  "[--otw sc8|sc12|sc16] [--block-ms N] [--tx]\n",
                  argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }

  const unsigned block_samples = static_cast<unsigned>(rate * block_ms / 1000.0);

  uhd::device_addr_t dev_addr(args);
  uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(dev_addr);
  usrp->set_rx_rate(rate);
  usrp->set_rx_freq(uhd::tune_request_t(freq));
  usrp->set_rx_gain(gain);

  std::fprintf(stderr,
               "[uhd_rx_health] opened: %s (%s, %zu channel(s))\n",
               usrp->get_mboard_name().c_str(),
               usrp->get_usrp_rx_info()["mboard_serial"].c_str(),
               usrp->get_rx_num_channels());
  std::fprintf(stderr,
               "[uhd_rx_health] args=%s rate=%.2fMsps freq=%.3fMHz gain=%.1fdB otw=%s block=%u samples "
               "(%.3fms)\n",
               args.c_str(),
               usrp->get_rx_rate() / 1e6,
               usrp->get_rx_freq() / 1e6,
               usrp->get_rx_gain(),
               otw.c_str(),
               block_samples,
               block_ms);

  uhd::stream_args_t stream_args("sc16", otw);
  stream_args.channels = {0};
  stream_args.args     = dev_addr;
  uhd::rx_streamer::sptr stream = usrp->get_rx_stream(stream_args);

  std::vector<std::complex<short>> buff(block_samples);
  std::vector<std::complex<short>*> buffs{&buff[0]};

  // The transmit side is WHAT THE LEG ADDS and this probe otherwise does not: both directions share one USB
  // link, so an RX path that is perfect alone can still overflow while the radio is being fed. Zeros are
  // enough - the question is the transport's load, not the waveform.
  uhd::tx_streamer::sptr                  tx_stream;
  std::atomic<bool>                       tx_run{false};
  std::thread                             tx_thread;
  std::vector<std::complex<short>>        tx_buff;
  std::vector<std::complex<short>*>       tx_buffs;
  if (with_tx) {
    tx_stream = usrp->get_tx_stream(stream_args);
    tx_buff.assign(block_samples, std::complex<short>(0, 0));
    tx_buffs = {&tx_buff[0]};
    // A transmit stream needs no stream command: the first send with start_of_burst starts it.
    tx_run = true;
    // Its own thread, as the radio's own transmit path has: sending from the receive loop would serialize the
    // two directions and manufacture the very overflow this probe is looking for.
    tx_thread = std::thread([&]() {
      bool first = true;
      while (tx_run.load(std::memory_order_relaxed)) {
        uhd::tx_metadata_t tx_md;
        tx_md.start_of_burst = first;
        tx_md.end_of_burst   = false;
        tx_md.has_time_spec  = false;
        first                = false;
        tx_stream->send(tx_buffs, block_samples, tx_md, 1.0);
      }
      uhd::tx_metadata_t tx_md;
      tx_md.start_of_burst = false;
      tx_md.end_of_burst   = true;
      tx_md.has_time_spec  = false;
      tx_stream->send(tx_buffs, 0, tx_md, 1.0);
    });
  }

  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  cmd.stream_now = true;
  stream->issue_stream_cmd(cmd);

  error_counts       errors;
  unsigned long long blocks = 0;
  unsigned long long samples = 0;
  unsigned long long gaps = 0;
  unsigned long long gap_samples = 0;
  unsigned long long max_gap_samples = 0;
  unsigned long long recv_over_1ms = 0;
  long long          recv_max_us = 0;
  double             recv_sum_us = 0.0;
  long long          expected_ticks = -1;

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    uhd::rx_metadata_t md;
    const auto         begin = std::chrono::steady_clock::now();
    const size_t       n     = stream->recv(buffs, block_samples, md, 1.0, false);
    const auto         end   = std::chrono::steady_clock::now();
    const long long    recv_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    recv_sum_us += static_cast<double>(recv_us);
    recv_max_us = std::max(recv_max_us, recv_us);
    if (recv_us > 1000) {
      ++recv_over_1ms;
    }
    errors.note(md.error_code);
    if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
      continue; // an errored block is not part of the stream (same rule the lower PHY applies)
    }

    // NOTE: a difference of ONE tick is not a discontinuity: `to_ticks()` converts a time_spec held as a
    //       double, so the rounding of a multi-second second-count can land one tick either side of the exact
    //       sample count. The lower PHY never sees this - its gateway timestamp is an integer the radio driver
    //       produces - so this tolerance exists to keep the probe's own arithmetic from reporting a fault the
    //       PHY would not (measured: 2 "gaps" of 1 sample in an otherwise perfect 8 s run at 100% duty).
    const long long ticks = static_cast<long long>(md.time_spec.to_ticks(rate));
    if ((expected_ticks >= 0) && (std::llabs(ticks - expected_ticks) > 1)) {
      const unsigned long long gap = static_cast<unsigned long long>(std::llabs(ticks - expected_ticks));
      ++gaps;
      gap_samples += gap;
      max_gap_samples = std::max(max_gap_samples, gap);
    }
    expected_ticks = ticks + static_cast<long long>(n);
    ++blocks;
    samples += n;
  }

  cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
  cmd.stream_now  = true;
  stream->issue_stream_cmd(cmd);

  if (tx_run.load()) {
    tx_run = false;
    tx_thread.join();
  }

  std::fprintf(stderr,
               "[uhd_rx_health] blocks=%llu samples=%llu recv(max=%.0fus mean=%.0fus over1ms=%llu) gaps=%llu "
               "gap_samples=%llu max_gap_us=%.0f\n",
               blocks,
               samples,
               static_cast<double>(recv_max_us),
               (blocks != 0) ? (recv_sum_us / static_cast<double>(blocks)) : 0.0,
               recv_over_1ms,
               gaps,
               gap_samples,
               static_cast<double>(max_gap_samples) * 1e6 / rate);
  std::fprintf(stderr,
               "[uhd_rx_health] radio errors: none=%llu overflow=%llu late=%llu timeout=%llu broken_chain=%llu "
               "alignment=%llu bad_packet=%llu\n",
               errors.none,
               errors.overflow,
               errors.late,
               errors.timeout,
               errors.broken_chain,
               errors.alignment,
               errors.bad_packet);

  const bool healthy = (errors.overflow == 0) && (errors.late == 0) && (errors.timeout == 0) &&
                       (errors.broken_chain == 0) && (errors.bad_packet == 0) && (gaps == 0) && (blocks != 0);
  std::fprintf(stderr,
               "[uhd_rx_health] VERDICT: %s (%.1f%% of the requested time produced stream: %.1fs of %.1fs)\n",
               healthy ? "the RX transport kept up - no overflow, no late, no gap"
                       : "THE RX TRANSPORT DID NOT KEEP UP - read the counters above",
               100.0 * static_cast<double>(samples) / (rate * seconds),
               static_cast<double>(samples) / rate,
               seconds);
  return healthy ? 0 : 1;
}
