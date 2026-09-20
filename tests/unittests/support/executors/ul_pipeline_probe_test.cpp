// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief The UL pipeline probe's two mutually exclusive report shapes, and the span the fused lane reports.
///
/// The probe is only ever read by a human looking at a shutdown log after an over-the-air leg - the most
/// expensive measurement this line has. An instrument that reports nothing, or reports a series that does
/// not mean what its name says, therefore costs a phone test to discover. This test forces both modes
/// offline and reads the report exactly as the operator does (stderr), so that:
///
///  - outside the fused lane the three phase segments are reported and [ul_gpu_pipeline] is not, and
///  - inside it (phy_pipeline_mode::gpu) the single IQ -> LLR span replaces them, paired across the
///    record_start() ... record_ldpc_start() calls that bracket it.
///
/// \note The two modes are checked in ONE case, in this order, on purpose: phy_pipeline_mode_registry::set()
///       publishes a process-wide mode and there is no way back, so the non-fused half has to run while the
///       registry is still at its unpublished default - and \c gtest_discover_tests runs every case as its own
///       ctest entry (its own process), so splitting them would make each half assert about a probe singleton the
///       other half never touched.

#include "ocudu/phy/phy_pipeline_mode.h"
#include "ocudu/support/executors/ul_pipeline_probe.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#if !defined(OCUDU_FLOW_PROBES)

TEST(ul_pipeline_probe_test, compiled_out_without_flow_probes)
{
  // ENABLE_FLOW_PROBES=OFF: every record_*() call and report() are no-ops (the zero-overhead build), so there is
  // nothing to report about. Say so instead of passing silently, which would look like the probe had been checked.
  GTEST_SKIP() << "built without ENABLE_FLOW_PROBES: the UL pipeline probe compiles to no-ops";
}

#else

namespace {

/// Runs probe.report() with stderr redirected into a temporary file and returns what it printed.
std::string capture_report()
{
  FILE* capture = std::tmpfile();
  EXPECT_NE(capture, nullptr);
  if (capture == nullptr) {
    return {};
  }
  std::fflush(stderr);
  const int saved = dup(fileno(stderr));
  EXPECT_NE(saved, -1);
  dup2(fileno(capture), fileno(stderr));

  ocudu::ul_pipeline_probe::get().report();

  // The probe reports through stdio: flush the (redirected) stream before restoring the descriptor, otherwise the
  // buffered text lands in the console instead of the capture.
  std::fflush(stderr);
  dup2(saved, fileno(stderr));
  close(saved);

  std::rewind(capture);
  std::string out;
  char        buf[512];
  size_t      nof_read = 0;
  while ((nof_read = std::fread(buf, 1, sizeof(buf), capture)) > 0) {
    out.append(buf, nof_read);
  }
  std::fclose(capture);
  return out;
}

/// Offset of the "<name>] samples=" header of a series, or npos when the report carries no line for it.
size_t series_pos(const std::string& report, const std::string& name)
{
  return report.find("[" + name + "] samples=");
}

/// True when the report carries a line for the given series.
bool has_series(const std::string& report, const std::string& name)
{
  return series_pos(report, name) != std::string::npos;
}

/// Number of samples of a series, or -1 when the report carries no line for it.
int samples(const std::string& report, const std::string& name)
{
  const std::string prefix = "[" + name + "] samples=";
  const size_t      pos    = series_pos(report, name);
  if (pos == std::string::npos) {
    return -1;
  }
  return std::atoi(report.c_str() + pos + prefix.size());
}

/// The mean of a series in us, or -1 when the report carries no line for it.
double mean_us(const std::string& report, const std::string& name)
{
  const size_t pos = series_pos(report, name);
  if (pos == std::string::npos) {
    return -1.0;
  }
  const size_t mean_pos = report.find("mean=", pos);
  if (mean_pos == std::string::npos) {
    return -1.0;
  }
  return std::strtod(report.c_str() + mean_pos + 5, nullptr);
}

} // namespace

/// \note ONE case for both modes, deliberately. \c gtest_discover_tests registers every case as its own ctest
///       entry, i.e. as its own PROCESS: two cases would each start from a fresh probe singleton, so anything one
///       of them asserted about what the other had recorded would hold when the binary is run whole and fail under
///       ctest. (That is not hypothetical: it is how this test failed its first ctest run.) The mode is published
///       process-wide and cannot be taken back either, so the non-fused half has to come first inside one case.
TEST(ul_pipeline_probe_test, one_report_shape_per_pipeline_mode)
{
  constexpr uint64_t cpu_slot = 100;
  constexpr uint64_t gpu_slot = 200;
  // A span long enough to be unmistakable in the report (and to make a mis-paired value near zero visible).
  constexpr auto span = std::chrono::milliseconds(30);

  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();

  // ---- outside the fused lane -------------------------------------------------------------------------------
  // The unpublished default mode resolves to phy_pipeline_mode::cpu: the per-module boundaries exist, so the phase
  // segments are the series that describe this pipeline.
  probe.record_start(cpu_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_t2f_end(cpu_slot);
  probe.record_ce_end(cpu_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_ldpc_start(cpu_slot);
  probe.record_end_crc_ok(cpu_slot, 42);
  // The receive wait is the one series with no pairing at all - the caller brackets a single receive() - so it is
  // recorded and reported in EVERY mode, the fused lane included (a receive still waits for samples there).
  // Recorded as a whole span, so a report that echoed another series into it would show up.
  probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(7)).count());

  {
    const std::string report = capture_report();
    EXPECT_EQ(samples(report, "ul_pipeline"), 1) << report;
    EXPECT_EQ(samples(report, "ul_time_frequency"), 1) << report;
    EXPECT_EQ(samples(report, "ul_channel_estimation"), 1) << report;
    EXPECT_EQ(samples(report, "ul_equalization_demod"), 1) << report;
    // The fused-lane series is a property of the fused lane only: the mode that has module boundaries must not
    // report a span whose name claims it does not.
    EXPECT_FALSE(has_series(report, "ul_gpu_pipeline")) << report;
    // Reported next to the segments, and it is ITS OWN number: [ul_time_frequency] is a few ms here because it
    // includes the wait, while [ul_rx_wait] is the 7 ms recorded above and nothing else.
    EXPECT_EQ(samples(report, "ul_rx_wait"), 1) << report;
    EXPECT_NEAR(mean_us(report, "ul_rx_wait"), 7000.0, 1000.0) << report;
  }

  // ---- the per-slot timeline (OCUDU_UL_SLOT_TRACE), and the one thing it exists to separate ----------------
  //
  // The other series all start at the slot's FIRST sample, so none of them can say how much of the span was the
  // front end working and how much was waiting for the samples to exist. The trace adds the OTHER instant - the
  // arrival of the samples that COMPLETE the slot - and every landmark is reported as a delta from it. This
  // section records both instants deliberately far apart (the series' start 20 ms before the completion, the
  // landmarks a few ms after it), so a report that measured from the wrong base is unmistakable.
  setenv("OCUDU_UL_SLOT_TRACE", "4", 1);
  constexpr uint64_t traced_slot = 400;
  probe.record_start(traced_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  probe.record_slot_samples_complete(traced_slot, 3840, 9000000, std::chrono::high_resolution_clock::now());
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  probe.record_t2f_end(traced_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_ce_end(traced_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  probe.record_ldpc_start(traced_slot);
  probe.record_end_crc_ok(traced_slot, 42);
  {
    const std::string report = capture_report();
    // The switch is echoed, so "off" can never be read as "found nothing".
    EXPECT_NE(report.find("[ul_slot_trace] OCUDU_UL_SLOT_TRACE=4"), std::string::npos) << report;
    EXPECT_NE(report.find("slots captured=1"), std::string::npos) << report;
    // The traced slot's own values, parsed from its line: the landmark deltas must be measured from the SAMPLES
    // COMPLETE instant and not from the slot's start, i.e. the time-frequency landmark must be ~1 ms and NOT
    // ~21 ms (which is what measuring from record_start() would give).
    const size_t pos = report.find("[ul_slot_trace]");
    const size_t row = report.find("  400 ", pos);
    EXPECT_NE(row, std::string::npos) << report;
    const std::string line = report.substr(row, report.find('\n', row) - row);
    std::istringstream    is(line);
    std::string           tok;
    std::vector<double>   nums;
    while (is >> tok) {
      try {
        nums.push_back(std::stod(tok));
      } catch (...) {
      }
    }
    // columns: slot rxwait t2f ce ldpc crc_ok tf_from_done pipeline
    ASSERT_GE(nums.size(), 8u) << line;
    EXPECT_NEAR(nums[0], 400.0, 1.0) << line;      // slot
    EXPECT_NEAR(nums[1], 9000.0, 500.0) << line;   // rx wait of the block that completed it (~9 ms)
    EXPECT_GE(nums[2], 500.0) << line;             // t2f after completion (>= the 1 ms sleep)
    EXPECT_LT(nums[2], 5000.0) << line;            // ... and NOT the 21 ms from record_start()
    EXPECT_GE(nums[3], nums[2]) << line;           // ce is later than t2f
    EXPECT_GE(nums[4], nums[3]) << line;           // ldpc start is later than ce
    EXPECT_GE(nums[5], nums[4]) << line;           // crc ok is later than the decode start
    EXPECT_GE(nums[7], 20000.0) << line;           // the series span really is ~21 ms from record_start()
  }
  unsetenv("OCUDU_UL_SLOT_TRACE");

  // ---- inside the fused lane ---------------------------------------------------------------------------------
  // Now the three phase segments have no meaning - their boundaries do not exist in that mode - and the span they
  // add up to is what the lane owns: IQ samples in -> LLRs out, which ends at the first codeblock decode
  // invocation. (The slot differs from the one above so the two can never match each other's pending entries.)
  ocudu::phy_pipeline_mode_registry::set(ocudu::phy_pipeline_mode::gpu);

  probe.record_start(gpu_slot);
  std::this_thread::sleep_for(span);
  probe.record_ldpc_start(gpu_slot);
  probe.record_end_crc_ok(gpu_slot, 42);

  {
    const std::string report = capture_report();
    // One sample per DECODE ATTEMPT, whether or not the transport block decodes (this one did), while
    // [ul_pipeline] is completed only by a CRC-OK transport block: the two series therefore have different
    // populations, which is what keeps the fused span visible in a run whose decodes all fail.
    // [ul_pipeline] counts 3 by now (the non-fused half, the traced slot - both in cpu mode - and this one),
    // while the fused series counts only 1: the traced slot's decode start happened BEFORE the mode was set, so
    // it never entered the fused series. The two populations differ by construction, which is the point of the
    // note above; asserting the trace had added one here would be asserting the opposite.
    EXPECT_EQ(samples(report, "ul_gpu_pipeline"), 1) << report;
    EXPECT_EQ(samples(report, "ul_pipeline"), 3) << report;
    // The fused mode replaces the segments instead of adding a fourth series next to them.
    EXPECT_FALSE(has_series(report, "ul_time_frequency")) << report;
    EXPECT_FALSE(has_series(report, "ul_channel_estimation")) << report;
    EXPECT_FALSE(has_series(report, "ul_equalization_demod")) << report;
    // ... but NOT the receive wait: a receive still blocks for samples in the fused lane, and this series is the
    // only one that says for how long. It survives the mode change (the segments do not).
    EXPECT_EQ(samples(report, "ul_rx_wait"), 1) << report;

    // The reported span is the one between the two timestamps above, not an artifact: it has to be at least the
    // sleep, and it cannot be a whole slot's worth of something else.
    const double measured_us = mean_us(report, "ul_gpu_pipeline");
    EXPECT_GE(measured_us, 30000.0) << report;
    EXPECT_LT(measured_us, 1000000.0) << report;
  }

  // ---- inside the fused lane, with the diagnostic decomposition switched on (OCUDU_UL_PHASE_SEGMENTS=1) --------
  // The question "which part of the IQ -> LLR span is which" is the same one inside the lane, and there the three
  // segments are the ONLY decomposition of that span the probe can produce (their ends are the instants that bound
  // [ul_gpu_pipeline], so they add up to it by construction). The switch must therefore ADD the segments without
  // replacing the total the lane's criteria read - and the values have to be the sub-spans, not the total again.
  setenv("OCUDU_UL_PHASE_SEGMENTS", "1", 1);
  constexpr uint64_t forced_slot = 300;
  probe.record_start(forced_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_t2f_end(forced_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  probe.record_ce_end(forced_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
  probe.record_ldpc_start(forced_slot);
  probe.record_end_crc_ok(forced_slot, 42);
  {
    const std::string report = capture_report();
    // BOTH: the lane's total (two decode attempts by now) and the three segments. The segment series ACCUMULATE
    // over the process (they are a shutdown report, not a per-slot one): the non-fused half, the traced slot and
    // this section each add one, so the counts are 3 here - asserting about a single section's contribution would
    // be asserting about the report's history.
    EXPECT_EQ(samples(report, "ul_gpu_pipeline"), 2) << report;
    EXPECT_EQ(samples(report, "ul_time_frequency"), 3) << report;
    EXPECT_EQ(samples(report, "ul_channel_estimation"), 3) << report;
    EXPECT_EQ(samples(report, "ul_equalization_demod"), 3) << report;
    // The sleeps above are the SUB-spans and they must not be the whole span again: this section slept 2 / 3 / 4 ms
    // around a span of ~9 ms, while the non-fused half slept 2 / 0 / 2 ms. So the mean of the equalization+
    // demodulation segment over the two samples has to sit at ~3 ms - a report that printed the total (or the sum)
    // three times lands far above, and one that printed the same number three times lands at ~4.5 ms or ~0.
    const double eqdem_us = mean_us(report, "ul_equalization_demod");
    EXPECT_GE(eqdem_us, 2500.0) << report;
    EXPECT_LT(eqdem_us, 4000.0) << report;
    // The forced PUSCH's segments are SUB-spans, and their positions are what the numbers have to show. The
    // absolute bounds below therefore have to account for the TRACED slot recorded earlier in this same case:
    // the segment series accumulate over the process, so its ~21 ms contribution is in this mean too (that is
    // the price of asserting both features in one process - the probe is a singleton and the pipeline mode
    // cannot be set back, see the file comment). What the bounds still catch is the failure they were written
    // for: a report that printed the TOTAL for each segment (or the same number three times) lands outside the
    // RATIOS asserted here, whatever the absolute offset is.
    const double t2f_us   = mean_us(report, "ul_time_frequency");
    const double ce_us    = mean_us(report, "ul_channel_estimation");
    const double total_us = mean_us(report, "ul_gpu_pipeline");
    // The forced section slept 2 / 3 / 4 ms between the three landmarks; the earlier sections slept 2 / 0 / 2
    // (non-fused) and 20 / 1 / 2 (traced), all of which are in these means.
    EXPECT_GE(t2f_us, 2000.0) << report;
    EXPECT_LT(t2f_us, 12000.0) << report;
    EXPECT_GT(ce_us, 0.0) << report;
    // The three segments must not be the same number three times: the forced PUSCH alone separates them by
    // 2/3/4 ms, while every earlier section left two of them equal, so a report that echoed one value would make
    // this sum fail.
    EXPECT_GT(mean_us(report, "ul_time_frequency") + ce_us + eqdem_us, t2f_us * 1.4) << report;
    // ... and the fused total spans well beyond the three of them together (it is the whole IQ -> LLR window).
    EXPECT_GT(total_us, t2f_us + ce_us + eqdem_us) << report;
  }
  unsetenv("OCUDU_UL_PHASE_SEGMENTS");

  // ---- the offset stream, which is what made the first half-slot leg capture nothing ------------------------
  //
  // On air the stream carries a fixed offset from the slot grid (measured 2026-09-21: seven samples, exactly the
  // configured symbol-block size), so NO block ever ends on a slot boundary - the slot's last sample merely falls
  // INSIDE one. A completion test written as "the block ends on a boundary" therefore captures nothing at all,
  // on the whole leg, while looking correct in review. This case pins the shape that does work: the caller says
  // "this block carries the slot's last sample", which it decides from the slot's span, not from the block's end.
  setenv("OCUDU_UL_SLOT_TRACE", "4", 1);
  // The phase pieces are RECORDED only outside the fused lane unless the decomposition is forced - the mode is
  // already gpu by now (it cannot be set back, see the file comment), so without this the decode start would be
  // the only landmark and this case would assert about a t2f that the probe deliberately never took.
  setenv("OCUDU_UL_PHASE_SEGMENTS", "1", 1);
  constexpr uint64_t offset_slot = 500;
  probe.record_start(offset_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  // 3840 samples carrying the slot's LAST sample, i.e. a block that crosses the slot boundary.
  probe.record_slot_samples_complete(offset_slot, 3840, 1500000, std::chrono::high_resolution_clock::now());
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  probe.record_t2f_end(offset_slot);
  probe.record_ldpc_start(offset_slot);
  probe.record_end_crc_ok(offset_slot, 42);
  {
    const std::string report = capture_report();
    const size_t      pos    = report.find("  500 ");
    ASSERT_NE(pos, std::string::npos) << report;
    const std::string line = report.substr(pos, report.find('\n', pos) - pos);
    std::istringstream    is(line);
    std::string           tok;
    std::vector<double>   nums;
    while (is >> tok) {
      try {
        nums.push_back(std::stod(tok));
      } catch (...) {
      }
    }
    ASSERT_GE(nums.size(), 8u) << line;
    EXPECT_NEAR(nums[0], 500.0, 1.0) << line;     // the slot
    EXPECT_NEAR(nums[1], 1500.0, 300.0) << line;  // its receive wait, carried by the same call
    EXPECT_GE(nums[2], 500.0) << line;            // t2f from the completion, not from the slot's start
    EXPECT_LT(nums[2], 5000.0) << line;
  }
  unsetenv("OCUDU_UL_SLOT_TRACE");
  unsetenv("OCUDU_UL_PHASE_SEGMENTS");
}

#endif // OCUDU_FLOW_PROBES
