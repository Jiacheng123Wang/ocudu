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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

/// The rule that keys the per-slot timeline: which slot a received block of samples COMPLETES.
///
/// This is the one place where a wrong answer is SILENT - the timeline records other slots, or nothing at all,
/// and both look exactly like a leg in which nothing happened. It was wrong in both halves on the production
/// path (5.9.68, 5.9.69), and the numbers below are taken from the leg that exposed it (`s52-residency`,
/// 7680 samples per slot, a stream aligned to the slot grid). The free function is checked here rather than
/// through a leg, because a leg costs a phone test and this costs nothing.
TEST(ul_slot_completion_test, the_rule_and_the_one_it_replaced)
{
  constexpr unsigned sps = 7680;

  // 1. A whole-slot block that starts ON the grid completes the slot it starts in. This is the PRODUCTION case,
  //    and the rule it replaces answers "no slot at all" for it: it tested `to_next_boundary < nof_samples`,
  //    which here is `7680 < 7680`. Measured consequence: the timeline recorded 1 row out of a bound of 64.
  uint64_t done = 0;
  ASSERT_TRUE(ocudu::ul_slot_completed_by_block(3876ull * sps, sps, sps, done));
  EXPECT_EQ(done, 3876u);

  // 2. The measured unaligned block of that leg, [29767687, 29775367), which carries slot 3876's LAST sample
  //    (29775359). The rule it replaces credits 3877 - the slot that STARTS at the boundary inside the block.
  ASSERT_TRUE(ocudu::ul_slot_completed_by_block(29767687ull, sps, sps, done));
  EXPECT_EQ(done, 3876u);

  // 3. A block that ends before the first slot does completes nothing: without this guard a partial block would
  //    announce slot 0.
  EXPECT_FALSE(ocudu::ul_slot_completed_by_block(0, 100, sps, done));
  EXPECT_FALSE(ocudu::ul_slot_completed_by_block(7, 100, sps, done));

  // 4. A block spanning several slots completes only the NEWEST one; the caller announces one slot per block.
  ASSERT_TRUE(ocudu::ul_slot_completed_by_block(10ull * sps, 3 * sps, sps, done));
  EXPECT_EQ(done, 12u);

  // ★ REVERSE ARM. The rule this replaced, written out, must DISAGREE with the one above on BOTH measured
  // cases - so an edit that reintroduces it fails here instead of quietly emptying a leg's timeline.
  const auto old_rule = [](uint64_t block_begin, unsigned nof_samples, unsigned sps, uint64_t& out) -> bool {
    const uint64_t offset           = block_begin % sps;
    const uint64_t to_next_boundary = sps - offset;
    if (to_next_boundary < nof_samples) {
      out = (block_begin + to_next_boundary) / sps;
      return true;
    }
    return false;
  };
  uint64_t old_done = 0;
  EXPECT_FALSE(old_rule(3876ull * sps, sps, sps, old_done))
      << "the old rule fired on an aligned whole-slot block, so this test can no longer tell them apart";
  ASSERT_TRUE(old_rule(29767687ull, sps, sps, old_done));
  EXPECT_EQ(old_done, 3877u) << "the old rule did not mis-attribute the unaligned block";
}

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
///
/// \note The staleness cutoff (OCUDU_UL_STALE_US) is pinned OUT OF THE WAY here: this case records spans of
///       30 ms and 20 ms on purpose, to make a mis-paired value unmistakable, and both exceed the uplink HARQ
///       round trip the probe splits on. The span is a test device, not a claim that such a hop is useful, so
///       it must stay in the series - otherwise this case would be asserting the shape of the wrong series.
///       The split has its own case, which moves the cutoff the other way on purpose.
TEST(ul_pipeline_probe_test, one_report_shape_per_pipeline_mode)
{
  ::setenv("OCUDU_UL_STALE_US", "60000000", 1);
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
  // Samples complete FIRST, then the landmarks - the order the receiving chain has by construction, and now a
  // requirement: a landmark that arrives before its slot's samples-complete instant cannot be part of a timeline
  // (trace_slot() drops it), because the instant the deltas are measured from does not exist yet.
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
    EXPECT_NE(report.find("OCUDU_UL_SLOT_TRACE=4"), std::string::npos) << report;
    EXPECT_NE(report.find("rows=1"), std::string::npos) << report;
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
    // ... and it must not be NaN. crc_ok is the LAST landmark of a slot, so a rule that only writes the columns
    // of the landmark that CREATED the row leaves this one empty in every row - which is exactly what an early
    // `what != ldpc_start -> return` in front of the entry write did, for two review rounds, while t2f/ce/ldpc
    // all looked right. Asserting the ORDER (above) is not enough on its own: NaN fails it, but a NaN column in
    // a row the test does not inspect would not. This asserts the value is there.
    EXPECT_FALSE(std::isnan(nums[5])) << "the CRC landmark must land in an entry created earlier" << line;
    EXPECT_GE(nums[5], 0.0) << line;
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
    // \note The absolute bounds below leave real HEADROOM, and that is a fix, not a relaxation: they were
    //       4000/12000 us against sub-spans of 4 ms and 2 ms, i.e. no room for the scheduler at all, and the
    //       case failed about four runs in five once the machine was busy (measured: 4083 us against a 4000 us
    //       bound). What they are here to catch is a report that printed the TOTAL for each segment - about
    //       11 ms - so a bound at 6 ms catches it just as well while a 4 ms sleep can overrun. The structural
    //       assertions further down (the segment sum against the total, and the segments against each other)
    //       carry the discrimination that must not depend on wall-clock at all.
    const double eqdem_us = mean_us(report, "ul_equalization_demod");
    EXPECT_GE(eqdem_us, 2000.0) << report;
    EXPECT_LT(eqdem_us, 6000.0) << report;
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
    EXPECT_LT(t2f_us, 16000.0) << report;
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
  // ---- the bounded ring keeps the NEWEST slots, and an untraced slot produces no row -----------------------
  //
  // Both halves are regressions for the same on-air failure. The cap used to REFUSE new keys once full, so the
  // maps held the run's first milliseconds - the attach phase, where most slots are idle - while the landmarks
  // came from the whole run; and fill_slot_trace() used to fall back to a default-constructed time_point for a
  // missing base, which is not a sentinel on a clock whose epoch is boot. Together they printed a confident
  // "one slot" of microseconds (71680) for slots that were never traced at all.
  {
    // A slot whose landmarks were recorded but whose samples-complete never was must NOT appear as a row.
    constexpr uint64_t untraced_slot = 900;
    probe.record_start(untraced_slot);
    probe.record_t2f_end(untraced_slot);
    probe.record_ldpc_start(untraced_slot);
    probe.record_end_crc_ok(untraced_slot, 42);
    const std::string after = capture_report();
    EXPECT_EQ(after.find("  900 "), std::string::npos)
        << "a slot with no samples-complete instant must not be reported as a timeline row";

  // ---- the bounded ring, LAST: it EVICTS the rows every section above recorded -----------------------------
  //
  // It has to run after them: exceeding the cap deliberately drops the oldest traced slots, base and all, so any
  // assertion about an earlier slot must come first. (It did not, and the symptom was a row whose CRC column read
  // NaN - because the slot's samples-complete instant had been evicted by this very test, while the row itself
  // survives as an entry with no base left to measure from.)
    // The cap keeps the NEWEST completions: trace MORE slots than it allows and the first must fall out while
    // the last stays. Refusing new keys instead (the on-air defect) leaves exactly the opposite - the run's first
    // few milliseconds, which on an air leg is the attach phase. The number here must exceed the real cap, or the
    // assertion is vacuous (which is how this test was written the first time).
    constexpr uint64_t first_slot = 1000;
    constexpr unsigned overflow   = 520; // > max_slot_trace (512), so eviction definitely happens
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (uint64_t i = first_slot; i != first_slot + overflow; ++i) {
      probe.record_start(i);
      probe.record_slot_samples_complete(i, 3840, 100, std::chrono::high_resolution_clock::now());
      probe.record_t2f_end(i);
      probe.record_ldpc_start(i); // this is what makes the slot a TRACED one (it carries a PUSCH)
    }
    const std::string ring = capture_report();
    EXPECT_EQ(ring.find(" 1000 "), std::string::npos)
        << "the oldest traced slot must be evicted once the cap is exceeded";
    EXPECT_NE(ring.find(" 1519 "), std::string::npos) << "the newest traced slot must be kept";
  }

  unsetenv("OCUDU_UL_SLOT_TRACE");
  unsetenv("OCUDU_UL_PHASE_SEGMENTS");
}

/// The samples that finish too late to be used are COUNTED APART (5.9.54 item 7), and that is only worth
/// having if the split actually fires - which is what this arm shows, with the cutoff moved out of the way
/// (OCUDU_UL_STALE_US) so that a span this test can afford to sleep for is "stale".
TEST(ul_pipeline_probe_test, late_samples_are_counted_apart_from_the_series)
{
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();
  // The series accumulates for the whole process (report() does not clear it), so every assertion below is a
  // DELTA - the same reason the paired values are read as differences and not as absolutes.
  auto count_of = [](const std::string& report, const std::string& series, const std::string& field) -> long {
    const std::string line_marker = "[" + series + "] ";
    const std::size_t line_at     = report.find(line_marker);
    if (line_at == std::string::npos) {
      return -1; // the SERIES is missing: that is an error, and the assertions below say so.
    }
    const std::size_t field_at = report.find(field, line_at);
    if (field_at == std::string::npos) {
      // The line is there but the field is not - "no CRC-OK samples recorded", or a report written before the
      // split existed. Both read as ZERO of that thing, which is what makes a delta meaningful.
      return 0;
    }
    return std::strtol(report.c_str() + field_at + field.size(), nullptr, 10);
  };

  const std::string before   = capture_report();
  const long        samples0 = count_of(before, "ul_pipeline", "samples=");
  const long        stale0   = count_of(before, "ul_pipeline", "stale=");
  ASSERT_GE(samples0, 0L) << before;
  ASSERT_GE(stale0, 0L) << before;

  // Under the cutoff: a normal hop, which must land in the SERIES.
  constexpr uint64_t fresh_slot = 8100;
  probe.record_start(fresh_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_ldpc_start(fresh_slot);
  probe.record_end_crc_ok(fresh_slot, 42);

  // Over the cutoff: the same shape with the cutoff moved below it, which must land in `stale`.
  ::setenv("OCUDU_UL_STALE_US", "1", 1);
  constexpr uint64_t late_slot = 8200;
  probe.record_start(late_slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_ldpc_start(late_slot);
  probe.record_end_crc_ok(late_slot, 42);
  ::unsetenv("OCUDU_UL_STALE_US");

  const std::string after   = capture_report();
  const long        samples1 = count_of(after, "ul_pipeline", "samples=");
  const long        stale1   = count_of(after, "ul_pipeline", "stale=");
  ASSERT_GE(samples1, 0L) << after;
  ASSERT_GE(stale1, 0L) << after;

  EXPECT_EQ(samples1 - samples0, 1L) << after; // only the fresh one joined the series
  EXPECT_EQ(stale1 - stale0, 1L) << after;     // and only the late one was counted apart
  // The report has to say WHERE the cutoff was, or a `stale=` count cannot be read: it is the HARQ round trip,
  // and it is configuration-dependent.
  EXPECT_NE(after.find("the uplink HARQ round trip"), std::string::npos) << after;
}

/// The per-slot timeline honours the bound the OPERATOR asked for.
///
/// `OCUDU_UL_SLOT_TRACE=64` is a request to spend 64 rows of a log, and it used to bound nothing: the eviction
/// compared against the INTERNAL maximum (512) instead of the requested limit, so the run printed 512 rows while
/// its own header line said "bound now OCUDU_UL_SLOT_TRACE=64" (measured on s57-trace, 2026-09-22). The check
/// below FAILS under that behaviour - with a bound of 2 and five PUSCH slots it gets 5 rows - which is the point:
/// an instrument that reports a bound it does not apply is worse than one that reports none.
TEST(ul_slot_trace_test, the_requested_bound_is_the_one_enforced)
{
  ::setenv("OCUDU_UL_SLOT_TRACE", "2", 1);
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();

  // Five slots that carry a PUSCH, in the PRODUCTION shape: one whole slot per block, starting on the grid. The
  // samples-complete instant comes first (it is what makes a slot eligible for a timeline at all) and the first
  // codeblock decode start is what creates the row.
  for (uint64_t slot = 1000; slot != 1005; ++slot) {
    uint64_t done = 0;
    ASSERT_TRUE(ocudu::ul_slot_completed_by_block(slot * 7680, 7680, 7680, done)) << "slot " << slot;
    EXPECT_EQ(done, slot);
    probe.record_slot_samples_complete(done, 7680, 0, std::chrono::high_resolution_clock::now());
    probe.record_ldpc_start(slot);
  }

  const std::string report = capture_report();
  static constexpr const char* tag = "[ul_slot_trace] rows=";
  const size_t                 pos = report.find(tag);
  ASSERT_NE(pos, std::string::npos) << report;
  const long rows = std::strtol(report.c_str() + pos + sizeof("[ul_slot_trace] rows=") - 1, nullptr, 10);
  EXPECT_LE(rows, 2) << "the requested bound was not enforced (rows=" << rows << "):\n" << report;

  ::unsetenv("OCUDU_UL_SLOT_TRACE");
}

/// A row's raw instants describe the SAME frame as the deltas printed beside them.
///
/// The row is keyed by the MODULAR slot, so a later SFN cycle completes that key again. When the repeat carries no
/// PUSCH there is no landmark update to refresh the row, and a report that looked the instants up AT PRINT TIME
/// then printed a base one SFN cycle away from the deltas it was supposed to validate. Measured on
/// `s58-trace64` (2026-09-23): 40 of 64 rows, every one of them off by 10.238 s - the 10.24 s SFN cycle of that
/// configuration. The instants are snapshotted into the row now, and this case reproduces the wrap: two
/// completions of one slot key, one landmark, and the base column must still precede the landmark column, because
/// a landmark is always measured FROM the samples that came before it.
TEST(ul_slot_trace_test, the_raw_instants_describe_the_same_frame_as_the_deltas)
{
  ::setenv("OCUDU_UL_SLOT_TRACE", "64", 1);
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();
  constexpr uint64_t        slot  = 2000;

  uint64_t done = 0;
  ASSERT_TRUE(ocudu::ul_slot_completed_by_block(slot * 7680, 7680, 7680, done));
  probe.record_slot_samples_complete(done, 7680, 0, std::chrono::high_resolution_clock::now());
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  // Creates the row; its deltas are measured from the completion above.
  probe.record_ldpc_start(slot);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  // The next SFN cycle completes the same modular slot with nothing to decode: this overwrites the probe's
  // per-slot completion instant and must NOT touch the row that is already on record.
  probe.record_slot_samples_complete(done, 7680, 0, std::chrono::high_resolution_clock::now());

  const std::string report = capture_report();
  bool              found  = false;
  double            base   = 0.0;
  double            mark   = 0.0;
  std::istringstream lines(report);
  std::string        line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::vector<std::string> tok;
    std::string              field;
    while (fields >> field) {
      tok.push_back(field);
    }
    if ((tok.size() == 10) && (tok.front() == std::to_string(slot))) {
      base  = std::strtod(tok[tok.size() - 2].c_str(), nullptr);
      mark  = std::strtod(tok.back().c_str(), nullptr);
      found = true;
    }
  }
  ASSERT_TRUE(found) << "no row for slot " << slot << " in the report:\n" << report;
  EXPECT_LE(base, mark) << "the row's base (" << base << ") is later than its own landmark (" << mark
                        << "): the two columns are from different frames\n"
                        << report;

  ::unsetenv("OCUDU_UL_SLOT_TRACE");
}

#endif // OCUDU_FLOW_PROBES
