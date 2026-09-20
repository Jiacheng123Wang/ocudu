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
    EXPECT_EQ(samples(report, "ul_gpu_pipeline"), 1) << report;
    EXPECT_EQ(samples(report, "ul_pipeline"), 2) << report;
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
    // over the process (they are a shutdown report, not a per-slot one), so this section adds one sample each to
    // the two the non-fused half recorded - asserting 1 here would be asserting about the report's history.
    EXPECT_EQ(samples(report, "ul_gpu_pipeline"), 2) << report;
    EXPECT_EQ(samples(report, "ul_time_frequency"), 2) << report;
    EXPECT_EQ(samples(report, "ul_channel_estimation"), 2) << report;
    EXPECT_EQ(samples(report, "ul_equalization_demod"), 2) << report;
    // The sleeps above are the SUB-spans and they must not be the whole span again: this section slept 2 / 3 / 4 ms
    // around a span of ~9 ms, while the non-fused half slept 2 / 0 / 2 ms. So the mean of the equalization+
    // demodulation segment over the two samples has to sit at ~3 ms - a report that printed the total (or the sum)
    // three times lands far above, and one that printed the same number three times lands at ~4.5 ms or ~0.
    const double eqdem_us = mean_us(report, "ul_equalization_demod");
    EXPECT_GE(eqdem_us, 2500.0) << report;
    EXPECT_LT(eqdem_us, 4000.0) << report;
    // The three segments of the forced PUSCH add up to a span well above any single one of them.
    const double t2f_us   = mean_us(report, "ul_time_frequency");
    const double total_us = mean_us(report, "ul_gpu_pipeline");
    EXPECT_GE(t2f_us, 2000.0) << report;
    EXPECT_LT(t2f_us, 6000.0) << report;
    EXPECT_GT(total_us, t2f_us + 6000.0) << report;
  }
  unsetenv("OCUDU_UL_PHASE_SEGMENTS");
}

#endif // OCUDU_FLOW_PROBES
