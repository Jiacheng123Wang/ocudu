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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <regex>
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
  const auto old_rule = [](uint64_t block_begin, unsigned nof_samples, unsigned samples_per_slot,
                           uint64_t& out) -> bool {
    const uint64_t offset           = block_begin % samples_per_slot;
    const uint64_t to_next_boundary = samples_per_slot - offset;
    if (to_next_boundary < nof_samples) {
      out = (block_begin + to_next_boundary) / samples_per_slot;
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

/// The DFT-window overlap counters of a report: {slow seen, wait hits, commit->end hits, gpu hits, gpu-clock
/// samples}. Shared by the two M2 cases, because a COUNT is what they assert on: the probe is a process-wide
/// singleton, ctest runs each case in its own process and a direct run of this binary does not, so absolute values
/// hold in one mode and not the other - deltas hold in both.
std::array<long long, 5> dft_window_counts(const std::string& report)
{
  auto num = [&](const char* pat) {
    std::smatch m;
    return std::regex_search(report, m, std::regex(pat)) ? std::strtoll(m[1].str().c_str(), nullptr, 10) : -1LL;
  };
  return {num(R"((\d+) slow of )"), num(R"(wait (\d+)/)"), num(R"(commit->end (\d+)/)"), num(R"(gpu (\d+)/)"),
          num(R"(offset last -?\d+us over (\d+) sample)")};
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

  // ---- the start-up receive wait is reported APART, and the hop-scoped wait is its own population ------------
  //
  // The first receive() of a run spans the radio's stream start (the RU starts that stream 100 ms ahead BY DESIGN,
  // ru_controller_sdr_impl: delay_s = 0.1), so what it measures is that start-up offset and not the link: it
  // appears exactly once per run, and it used to become the series' `max` on every leg - the first number a reader
  // looks at - and cost three write-ups and one wrong attribution before it was explained (dev doc 6.146). The
  // probe is therefore TOLD which call it is, keeps it out of the distribution and prints it as its own field.
  {
    const int before = samples(capture_report(), "ul_rx_wait");
    probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(101)).count(), /*spans_stream_start=*/true);
    probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(5)).count());
    const std::string report = capture_report();
    // The start-up sample did NOT enter the distribution, while the ordinary one did.
    EXPECT_EQ(samples(report, "ul_rx_wait"), before + 1) << report;
    // ... and it is not silently dropped either: it is named, so the report accounts for the call the distribution
    // does not have. A silent filter here would be worse than the artifact it removes.
    EXPECT_NE(report.find("[ul_rx_wait] startup=101000.0us"), std::string::npos) << report;

    // THE HOP-SCOPED SERIES. A wait is bound to the slot it completed (record_slot_rx_wait) and consumed by the
    // hop's own landmark (record_ldpc_start), so the series has the HOPS' population and not the blocks': that is
    // what lets "the wait for this hop's samples" be read next to "the span of this hop". A block whose wait is
    // never consumed stays in the bounded registry and contributes nothing - by design, and it is also why the
    // per-block series was NOT narrowed to this population (a hiccup on an idle slot leaves a trace there only).
    constexpr uint64_t hop_slot     = 600;
    constexpr uint64_t idle_slot    = 601;
    constexpr uint64_t startup_slot = 602;
    probe.record_start(hop_slot);
    probe.record_slot_rx_wait(hop_slot, std::chrono::nanoseconds(std::chrono::milliseconds(3)).count());
    probe.record_ldpc_start(hop_slot); // the hop that consumes it
    probe.record_slot_rx_wait(idle_slot, std::chrono::nanoseconds(std::chrono::milliseconds(4)).count());
    probe.record_start(startup_slot);
    probe.record_slot_rx_wait(startup_slot, std::chrono::nanoseconds(std::chrono::milliseconds(101)).count(),
                              /*spans_stream_start=*/true);
    probe.record_ldpc_start(startup_slot);
    const std::string hop_report = capture_report();
    // Exactly ONE sample: the idle slot's wait belongs to no hop, and the start-up block belongs to no hop either -
    // which is the same rule the distribution follows, applied to the second population.
    EXPECT_EQ(samples(hop_report, "ul_rx_wait_hop"), 1) << hop_report;
    EXPECT_NEAR(mean_us(hop_report, "ul_rx_wait_hop"), 3000.0, 300.0) << hop_report;
  }
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

/// The slowest rows survive the age bound.
///
/// The timeline used to keep the NEWEST slots that carried a PUSCH, which is blind to the thing it is now used
/// for: measured on `s58-trace64` (2026-09-23), its 64 rows held no slow hop at all (largest pipeline 3115us)
/// while that same leg reported p95 = 5590us, and `s57-trace` had caught the 60 ms stall only because 512 rows
/// happened to reach back far enough to contain it. A quarter of the budget is reserved for the slowest rows now.
///
/// This case is the reverse arm for that policy: ONE slow row early, then enough fast rows to overflow a bound of
/// four. Under the age-only policy the slow row is the first thing evicted and this case fails; with the reserve
/// it is still in the report. The slow row is made slow through the probe's own series (a real span between
/// record_start() and the CRC-OK completion), not by writing into the row.
TEST(ul_slot_trace_test, the_slowest_rows_survive_the_age_bound)
{
  ::setenv("OCUDU_UL_SLOT_TRACE", "4", 1);
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();

  const auto trace_one = [&probe](uint64_t slot, std::chrono::milliseconds span) {
    uint64_t done = 0;
    ASSERT_TRUE(ocudu::ul_slot_completed_by_block(slot * 7680, 7680, 7680, done)) << "slot " << slot;
    probe.record_start(slot);
    std::this_thread::sleep_for(span);
    probe.record_slot_samples_complete(done, 7680, 0, std::chrono::high_resolution_clock::now());
    probe.record_ldpc_start(slot);   // creates the row
    probe.record_end_crc_ok(slot, 100); // ... and gives it its pipeline span
  };

  // The slow row must be slower than ANY row an earlier case left behind, because the probe is a process-wide
  // singleton and this case runs last (the same cross-case hazard the file header warns about). The case above
  // this one records spans of 30 ms on purpose, so 200 ms is unambiguous - with 20 ms this test passed alone and
  // failed in the binary, which is exactly that hazard and not the policy.
  trace_one(3000, std::chrono::milliseconds(200)); // the slow one, first, so age alone would evict it
  for (uint64_t slot = 3001; slot != 3011; ++slot) {
    trace_one(slot, std::chrono::milliseconds(0));
  }

  const std::string report = capture_report();
  EXPECT_NE(report.find(" 3000 "), std::string::npos)
      << "the slow row was evicted by age - the transient this instrument exists to catch cannot be seen that way:\n"
      << report;
  ::unsetenv("OCUDU_UL_SLOT_TRACE");
}

namespace {

/// What the phase-sample observer was handed (P0-5). One entry per announcement, in order.
struct phase_observer_log {
  std::vector<uint64_t>               slots;
  std::vector<std::array<int64_t, 3>> ns;
  void clear()
  {
    slots.clear();
    ns.clear();
  }
};

phase_observer_log& observer_log()
{
  static phase_observer_log log;
  return log;
}

void record_phase_sample(uint64_t slot, int64_t t2f_ns, int64_t ce_ns, int64_t eqdem_ns)
{
  observer_log().slots.push_back(slot);
  observer_log().ns.push_back({t2f_ns, ce_ns, eqdem_ns});
}

} // namespace

/// \brief P0-5: one announcement per FINALIZED phase sample, carrying the slot and the three durations.
///
/// The pairing between this probe's per-slot phase segments and gpu_lane_probe's per-lane residency/busy rests on
/// this contract, and a hook that got it wrong would be silent on air: it fires from inside the recording path, so
/// a sample announced twice, not at all, or with another sample's numbers produces a plausible-looking paired
/// population rather than a failure. The two ways it can be wrong are therefore checked here, offline:
///
///  * the COUNT: one announcement per sample that reaches [ul_time_frequency] / [ul_channel_estimation] /
///    [ul_equalization_demod], which is the equality the paired sample count is judged against (a leg's phase
///    samples and its paired samples are supposed to be the same number);
///  * the VALUES: the three durations announced are the ones the series received - not the total, not a cumulative
///    sum, and not another slot's.
///
/// \note The fused-lane mode is published for the whole process and cannot be taken back, so this case sets it at
///       its start and depends on no other case (gtest_discover_tests runs each case as its own process).
TEST(ul_pipeline_probe_test, phase_samples_are_announced_once_each_for_the_pairing)
{
  // The spans below are a test device, not a claim about a useful hop (same reason as the case above): the cutoff
  // is moved out of the way so a mis-paired value stays visible in its series.
  ::setenv("OCUDU_UL_STALE_US", "60000000", 1);
  // Inside the fused lane the segments are recorded only when the diagnostic switch asks for them, and that is the
  // arm the pairing exists for (the lane is the mode whose residency those ratios are about).
  ::setenv("OCUDU_UL_PHASE_SEGMENTS", "1", 1);
  ocudu::phy_pipeline_mode_registry::set(ocudu::phy_pipeline_mode::gpu);

  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();
  observer_log().clear();
  ocudu::ul_pipeline_probe::set_phase_sample_observer(&record_phase_sample);

  // The probe is a process-wide singleton and the cases above recorded samples of their own, so every reading
  // below is a DELTA - the same rule the late-sample case follows and the reason the file header gives for it.
  // The series are read through (count, mean), which is enough to recover the sample a section just added:
  // x = mean_after * n_after - mean_before * n_before.
  const auto series_state = [](const std::string& report, const std::string& series) {
    const int    n = samples(report, series);
    const double mean = mean_us(report, series);
    return std::pair<int, double>{std::max(n, 0), (n > 0) ? mean * n : 0.0};
  };
  const std::string baseline_report = capture_report();
  const auto [base_t2f_n, base_t2f_sum]     = series_state(baseline_report, "ul_time_frequency");
  const auto [base_ce_n, base_ce_sum]       = series_state(baseline_report, "ul_channel_estimation");
  const auto [base_eqdem_n, base_eqdem_sum] = series_state(baseline_report, "ul_equalization_demod");

  // ---- a complete hop: announced exactly once, with this slot's three sub-spans -------------------------
  constexpr uint64_t slot_a = 2101;
  probe.record_start(slot_a);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  probe.record_t2f_end(slot_a);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  probe.record_ce_end(slot_a);
  std::this_thread::sleep_for(std::chrono::milliseconds(4));
  probe.record_ldpc_start(slot_a);
  probe.record_end_crc_ok(slot_a, 42);

  ASSERT_EQ(observer_log().slots.size(), 1u)
      << "a finalized phase sample must be announced exactly once (0 = the pairing would see no samples at all)";
  EXPECT_EQ(observer_log().slots[0], slot_a) << "the announcement must carry the slot the sample is keyed by";
  // The three SUB-spans, not the total. Each bound leaves the scheduler room, as the case above explains: what it
  // catches is an announcement that hands over the ~9 ms TOTAL for each of the three.
  // ... in NANOSECONDS, the unit the probe records in (the series convert to us at the report).
  EXPECT_GE(observer_log().ns[0][0], 1500000) << "t2f: 2 ms slept";
  EXPECT_LT(observer_log().ns[0][0], 7000000);
  EXPECT_GE(observer_log().ns[0][1], 2500000) << "ce: 3 ms slept";
  EXPECT_LT(observer_log().ns[0][1], 8000000);
  EXPECT_GE(observer_log().ns[0][2], 3500000) << "eqdem: 4 ms slept";
  EXPECT_LT(observer_log().ns[0][2], 10000000);
  // ... and they are the SAME numbers the series received: the sample these three series grew by, recovered from
  // their (count, mean), is the one the observer was handed. This is what makes "announced" and "recorded" one
  // event instead of two that can drift apart - and a hook that handed over the total, or another slot's numbers,
  // lands outside the tolerance whatever the absolute offset of the series is.
  {
    const std::string report = capture_report();
    const auto [t2f_n, t2f_sum]     = series_state(report, "ul_time_frequency");
    const auto [ce_n, ce_sum]       = series_state(report, "ul_channel_estimation");
    const auto [eqdem_n, eqdem_sum] = series_state(report, "ul_equalization_demod");
    ASSERT_EQ(t2f_n, base_t2f_n + 1) << report;
    ASSERT_EQ(ce_n, base_ce_n + 1) << report;
    ASSERT_EQ(eqdem_n, base_eqdem_n + 1) << report;
    // 2 us of tolerance: each mean is printed with one decimal, so the recovered sample carries at most
    // 0.05 * n of rounding, and n is a handful of samples here (in a leg it is tens of thousands - which is why
    // the leg reads the paired MEDIANS and not a recovered single sample).
    EXPECT_NEAR(t2f_sum - base_t2f_sum, static_cast<double>(observer_log().ns[0][0]) / 1e3, 2.0) << report;
    EXPECT_NEAR(ce_sum - base_ce_sum, static_cast<double>(observer_log().ns[0][1]) / 1e3, 2.0) << report;
    EXPECT_NEAR(eqdem_sum - base_eqdem_sum, static_cast<double>(observer_log().ns[0][2]) / 1e3, 2.0) << report;
  }

  // ---- a hop whose FFT landmark never came: the segments cannot be assembled, so nothing is announced -----
  constexpr uint64_t slot_b = 2102;
  probe.record_start(slot_b);
  probe.record_ce_end(slot_b); // no record_t2f_end(): the assembly needs all three landmarks
  probe.record_ldpc_start(slot_b);
  probe.record_end_crc_ok(slot_b, 42);
  EXPECT_EQ(observer_log().slots.size(), 1u) << "a hop with no time-frequency landmark announced a sample";

  // ---- a hop whose decode start never came: the phases are assembled but never finalized ----------------
  constexpr uint64_t slot_c = 2103;
  probe.record_start(slot_c);
  probe.record_t2f_end(slot_c);
  probe.record_ce_end(slot_c);
  probe.record_end_crc_ok(slot_c, 42); // no record_ldpc_start(): the sample never reaches the series
  EXPECT_EQ(observer_log().slots.size(), 1u) << "a hop with no decode start announced a sample";

  // The count equality the pairing is judged by, stated as this test can see it: the three series grew by exactly
  // as many samples as the observer was handed - no more (a sample recorded but not announced would pair nothing)
  // and no fewer (an announcement with no sample would pair a lane with a phase sample that does not exist).
  {
    const std::string report = capture_report();
    EXPECT_EQ(samples(report, "ul_time_frequency") - base_t2f_n,
              static_cast<int>(observer_log().slots.size()))
        << report;
    EXPECT_EQ(samples(report, "ul_channel_estimation") - base_ce_n,
              static_cast<int>(observer_log().slots.size()))
        << report;
    EXPECT_EQ(samples(report, "ul_equalization_demod") - base_eqdem_n,
              static_cast<int>(observer_log().slots.size()))
        << report;
  }

  // ---- the count the LANE probe reads at exit (P0-5's account line) ---------------------------------------
  // `report()` prints a snapshot taken when it ran (early in the shutdown) while the observer keeps counting:
  // the lane probe asks for THIS count at exit so the account compares numbers taken at one instant. With one
  // announced sample in this process it must read 1 - and it must track the series, not the observer.
  EXPECT_EQ(probe.phase_samples_recorded(), static_cast<size_t>(base_t2f_n) + observer_log().slots.size())
      << "the count read at exit must be the number of samples the three series hold";

  // ---- unregistering stops the announcements (and does not stop the recording) ---------------------------
  ocudu::ul_pipeline_probe::set_phase_sample_observer(nullptr);
  constexpr uint64_t slot_d = 2104;
  probe.record_start(slot_d);
  probe.record_t2f_end(slot_d);
  probe.record_ce_end(slot_d);
  probe.record_ldpc_start(slot_d);
  probe.record_end_crc_ok(slot_d, 42);
  EXPECT_EQ(observer_log().slots.size(), 1u) << "an unregistered observer was still called";
  {
    const std::string report = capture_report();
    // The series grew by the sample that was NOT announced: announcing and recording are separate effects, which is
    // why the count comparison above has to be made with the observer still registered.
    EXPECT_EQ(samples(report, "ul_time_frequency") - base_t2f_n, static_cast<int>(observer_log().slots.size()) + 1)
        << report;
  }
  ::unsetenv("OCUDU_UL_PHASE_SEGMENTS");
}

/// THE SAME-LEG TEST for the receive tail: a slow receive that falls INSIDE a blocking DFT window.
///
/// Across arms the stalls appear exactly where the host blocks on a Metal completion (0.000-0.001% where it does
/// not, 0.06-0.11% where it does) - but a cross-arm correlation cannot say causality, and the null hypothesis has
/// a number: if the two are independent, slow receives overlap the blocking windows at their duty cycle (~4% on
/// the measured legs). This case pins the accounting that tells them apart, including the two ways it could lie:
/// an overlap miss (a window that should match) and a false positive (a receive that ends before the window
/// starts).
TEST(ul_pipeline_probe_test, slow_receives_are_tested_against_the_dft_blocking_windows)
{
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();
  const auto                ns    = [](std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
  };
  const auto t0 = std::chrono::steady_clock::now();

  const auto before = dft_window_counts(capture_report());

  // One blocking window, 400 us long: the shape the device-grid arms show (384 us median).
  const auto win_begin = t0;
  const auto win_end   = t0 + std::chrono::microseconds(400);
  probe.record_dft_wait(std::chrono::nanoseconds(std::chrono::microseconds(400)).count(), ns(win_begin), ns(win_end));

  // (a) a SLOW receive whose window straddles the blocking one...
  probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(2)).count(),
                       /*spans_stream_start=*/false,
                       ns(win_begin + std::chrono::microseconds(100)),
                       ns(win_begin + std::chrono::microseconds(2100)));
  // (b) ... and a fast one that cannot overlap ANY window this process has recorded: an hour before now. Placing
  // it microseconds before the window was not enough - in a direct (single-process) run the ring still holds the
  // windows of the cases that ran before this one, and the "no overlap" half of the test then fails for a reason
  // that has nothing to do with the code under test (measured while writing the M2b case below).
  probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::microseconds(500)).count(),
                       /*spans_stream_start=*/false,
                       ns(win_begin - std::chrono::hours(1)),
                       ns(win_begin - std::chrono::hours(1) + std::chrono::microseconds(500)));

  const std::string report = capture_report();
  const auto        after  = dft_window_counts(report);
  ASSERT_GE(after[0], 0LL) << report;
  // One slow receive was recorded and it overlapped the wait; this case records NO commit and NO gpu window, so
  // those two columns must stay where they were - which is the check that the wait column is not simply "everything
  // that is slow".
  EXPECT_EQ(after[0] - before[0], 1) << report;
  EXPECT_EQ(after[1] - before[1], 1) << "the slow receive overlapped the wait" << report;
  EXPECT_EQ(after[2] - before[2], 0) << "no commit window was recorded" << report;
  EXPECT_EQ(after[3] - before[3], 0) << "no GPU window was recorded" << report;
  // The duty cycle is printed next to each rate, because it is the number the coincidence reading predicts.
  EXPECT_NE(report.find("vs duty"), std::string::npos) << report;
}

/// EACH SUBMISSION WINDOW IS COUNTED APART: the wait, the commit-to-end span and the GPU execution span.
///
/// M2 could only see the wait, and answered "no" (7% overlap against a 7% baseline). The two it could not see are
/// the driver's commit-time work and the cb's GPU execution, and telling them apart is the whole point of M2b: a
/// stall inside the GPU span is device/memory contention, one inside the commit span is driver submission work,
/// and one in neither means the two share an upstream cause. This case builds a receive that overlaps ONLY the GPU
/// window and asserts it lands in that column and not in the others - the mistake that would make the whole test
/// say "gpu" for every stall would be a window placed on the wrong clock.
TEST(ul_pipeline_probe_test, the_three_submission_windows_are_counted_apart)
{
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();
  const auto                ns    = [](std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
  };
  const auto t0 = std::chrono::steady_clock::now();

  // The counters are process-wide (the probe is a singleton) and ctest runs each case in its own process while a
  // direct run of this binary does not, so the assertions are DELTAS: the same case then holds either way. This is
  // the second time this file has been bitten by that difference (the rebase counter case was the first).
  const auto before = dft_window_counts(capture_report());

  // commit at t0, the wait runs t0+200us..t0+600us, and the GPU executes inside it (t0+250us..t0+450us).
  const auto commit   = t0;
  const auto w_begin  = t0 + std::chrono::microseconds(200);
  const auto w_end    = t0 + std::chrono::microseconds(600);
  const auto g_begin  = t0 + std::chrono::microseconds(250);
  const auto g_end    = t0 + std::chrono::microseconds(450);
  probe.record_dft_wait(std::chrono::nanoseconds(std::chrono::microseconds(400)).count(),
                        ns(w_begin),
                        ns(w_end),
                        ns(commit),
                        ns(g_begin),
                        ns(g_end),
                        std::chrono::nanoseconds(std::chrono::microseconds(5)).count());

  // (a) A slow receive INSIDE the GPU span - which is also inside the wait and inside commit->end, exactly as a
  // real submission nests. It must be counted in all three.
  probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(2)).count(),
                       /*spans_stream_start=*/false,
                       ns(g_begin + std::chrono::microseconds(10)),
                       ns(g_end - std::chrono::microseconds(10)));
  // (a2) A SECOND submission whose windows are deliberately NOT nested: its GPU span sits 5 ms BEFORE its commit
  // and wait. A receive inside that span must then be claimed by the gpu column ONLY - which is what makes the
  // columns demonstrably separate rather than merely present (a single shared ring would count it everywhere, and a
  // gpu ring that is never filled would count it nowhere).
  const auto g2_begin = t0 - std::chrono::microseconds(5000);
  const auto g2_end   = t0 - std::chrono::microseconds(4000);
  probe.record_dft_wait(std::chrono::nanoseconds(std::chrono::microseconds(300)).count(),
                        ns(w_begin),
                        ns(w_end),
                        ns(commit),
                        ns(g2_begin),
                        ns(g2_end),
                        std::chrono::nanoseconds(std::chrono::microseconds(5)).count());
  probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(2)).count(),
                       /*spans_stream_start=*/false,
                       ns(g2_begin + std::chrono::microseconds(10)),
                       ns(g2_end - std::chrono::microseconds(10)));

  // (b) A slow receive that NO window may claim: an hour AFTER now, so it cannot intersect the windows of this
  // case nor those a previous case left in the process-wide ring. This is the discriminating half - a window
  // placed on the wrong clock, or an intersection test that is too loose, would count this one too.
  probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(2)).count(),
                       /*spans_stream_start=*/false,
                       ns(commit + std::chrono::hours(1)),
                       ns(commit + std::chrono::hours(1) + std::chrono::milliseconds(2)));

  const std::string report = capture_report();
  const auto        after  = dft_window_counts(report);
  ASSERT_GE(before[0], 0LL) << report;
  ASSERT_GE(after[0], 0LL) << report;
  EXPECT_EQ(after[0] - before[0], 3) << "three slow receives were recorded ((a), (a2), (b))" << report;
  EXPECT_EQ(after[1] - before[1], 1) << "only (a) overlaps the wait" << report;
  EXPECT_EQ(after[2] - before[2], 1) << "only (a) overlaps commit->end" << report;
  EXPECT_EQ(after[3] - before[3], 2) << "(a) and (a2) overlap the GPU span, (b) does not" << report;
  EXPECT_EQ(after[4] - before[4], 2) << "the GPU clock offset was sampled once per submission" << report;
  EXPECT_NE(report.find("gpu-clock offset last 5us over"), std::string::npos) << report;
}

/// A LATER SFN CYCLE REBASES A SLOT INSTEAD OF REUSING THE PREVIOUS CYCLE'S LANDMARKS.
///
/// The trace is keyed by the MODULAR slot, and not every landmark is re-recorded every cycle: PUXCH completes the
/// symbols of every slot it processes, so `t2f` DOES arrive again, while ce/ldpc_start/crc_ok need a grant whose
/// transport block is processed and decoded. A slot that carried a PUSCH one SFN cycle ago and none now therefore
/// used to pair THIS cycle's base and t2f with the PREVIOUS cycle's three instants, and printed them as deltas of
/// MINUS one SFN cycle - a confident wrong number, and the one thing this instrument must not produce. Measured on
/// air before the fix (2026-09-28, legs p87/p88): 4/17/42 such rows in ce/ldpc/crc_ok, and 77/77/80 on the second
/// leg.
TEST(ul_slot_trace_test, a_new_cycle_rebases_a_slot_instead_of_reusing_last_cycles_landmarks)
{
  ::setenv("OCUDU_UL_SLOT_TRACE", "4", 1);
  // ... AND the phase segments are FORCED on, for the same reason the air legs carry the switch: `t2f` and `ce`
  // are recorded through record_t2f_end()/record_ce_end(), and both return immediately when
  // records_phase_segments() is false - which is the case in the fused lane unless OCUDU_UL_PHASE_SEGMENTS=1
  // forces it. Without the switch this case would trace only ldpc_start/crc_ok in mode=gpu and only four in
  // mode=cpu, i.e. it would assert different things depending on which cases ran before it (measured: rebased=2
  // after the mode-setting case, 4 when run alone). The two switches are what the p87/p88 legs carried.
  ::setenv("OCUDU_UL_PHASE_SEGMENTS", "1", 1);
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();
  constexpr uint64_t        slot  = 12345;

  // Reads the rebase counter out of a report. The COUNT is asserted as a difference (below) rather than against a
  // constant: the probe is a process-wide singleton and the other cases in this binary trace slots too, so an
  // absolute value would assert about the order the cases ran in.
  const auto rebased_count = [](const std::string& r) -> unsigned long long {
    const size_t pos = r.find("rebased=");
    return (pos == std::string::npos) ? 0ull : std::strtoull(r.c_str() + pos + 8, nullptr, 10);
  };

  // Cycle 1: a full hop, all four landmarks on one row.
  probe.record_start(slot);
  probe.record_slot_samples_complete(slot, 3840, 500000, std::chrono::high_resolution_clock::now());
  probe.record_t2f_end(slot);
  probe.record_ce_end(slot);
  probe.record_ldpc_start(slot);
  probe.record_end_crc_ok(slot, 42);

  // Cycle 2 (one SFN cycle later in the field; here a few ms, the arithmetic is what matters): the slot's samples
  // complete again and PUXCH completes its symbols, but this time the slot carries no grant - so only t2f arrives,
  // and the three PUSCH-only landmarks never do.
  const unsigned long long rebased_before = rebased_count(capture_report());
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  probe.record_slot_samples_complete(slot, 3840, 250000, std::chrono::high_resolution_clock::now());
  probe.record_t2f_end(slot);

  const std::string report = capture_report();
  // Both invariant counts are on the report, and neither may be silent: the rebase says what it dropped (exactly
  // the four landmarks of the cycle that ended), and the refused-negative count must read 0 - a negative span
  // coming back has to be visible before anyone reads a row.
  EXPECT_NE(report.find("rebased="), std::string::npos) << report;
  EXPECT_EQ(rebased_count(report) - rebased_before, 4ull)
      << "the rebase must drop exactly the four landmarks of the cycle that ended\n"
      << report;
  EXPECT_NE(report.find("negative deltas refused=0"), std::string::npos) << report;

  const size_t row = report.find("  12345 ");
  ASSERT_NE(row, std::string::npos) << report;
  const std::string   line = report.substr(row, report.find('\n', row) - row);
  std::istringstream  is(line);
  std::string         tok;
  std::vector<double> nums;
  while (is >> tok) {
    try {
      nums.push_back(std::stod(tok));
    } catch (...) {
    }
  }
  ASSERT_GE(nums.size(), 8u) << line;
  // The row IS the new cycle's: its base was refreshed, so the wait carried in it is the second one (250 us), not
  // the first (500 us). Without this the assertions below could pass on a row that was never touched at all.
  EXPECT_NEAR(nums[1], 250.0, 50.0) << line;
  // t2f is this cycle's (small and positive)...
  EXPECT_GE(nums[2], 0.0) << line;
  EXPECT_LT(nums[2], 3000.0) << line;
  // ... while the three that never arrived are GONE rather than carried over: a negative value here is exactly the
  // defect this case exists for, and a value near +10.24 s is the same defect the other way round.
  EXPECT_TRUE(std::isnan(nums[3])) << "ce was carried over from the previous cycle: " << line;
  EXPECT_TRUE(std::isnan(nums[4])) << "ldpc was carried over from the previous cycle: " << line;
  EXPECT_TRUE(std::isnan(nums[5])) << "crc_ok was carried over from the previous cycle: " << line;

  ::unsetenv("OCUDU_UL_SLOT_TRACE");
  ::unsetenv("OCUDU_UL_PHASE_SEGMENTS");
}


/// The worst receive waits and hand-over margins are printed WITH THEIR HOST WALL CLOCK, bounded, and only when
/// asked for (dev doc 6.240/6.241).
///
/// WHY THIS ARM EXISTS. The instrument's whole value is that its numbers can be lined up against a DIFFERENT file's
/// timestamps: the `.log`'s `[RF] Real-time failure in RF: underflow|late` lines, and the DL hand-overs that missed
/// their due time. A leg is the only place those three meet, and a leg costs a phone test - so the properties that
/// make the alignment possible (a wall clock that is actually printed, a list that is actually bounded, ranking in
/// the direction each quantity goes wrong) are pinned here instead. The three arms below are therefore:
///
///  * OFF by default: the report contains no `[ul_timing_events]` block at all, which is the "the knob is the only
///    thing that turns it on" half of the probe contract (dev doc 6.145 (3));
///  * ON: the worst TWO survive out of three candidates, worst first, each with `wall=` and `epoch_ms=`, and the
///    block reports how many candidates it saw so an empty list cannot be confused with a missing instrument;
///  * the DISTRIBUTION IS NOT TOUCHED: the ranked list is a second view of the same samples, so `[ul_rx_wait]`
///    still counts every one of them - a ranking that ate samples would silently change the series it annotates.
TEST(ul_pipeline_probe_test, worst_timing_events_carry_the_wall_clock_and_stay_bounded)
{
  ocudu::ul_pipeline_probe& probe = ocudu::ul_pipeline_probe::get();

  // ---- OFF (the default): the block does not exist, whatever the events look like ---------------------------------
  {
    ::unsetenv("OCUDU_UL_TIMING_EVENTS");
    EXPECT_EQ(capture_report().find("[ul_timing_events]"), std::string::npos)
        << "the instrument must be silent unless OCUDU_UL_TIMING_EVENTS asks for it";
    // The counts are DELTAS, not absolutes: gtest_discover_tests gives this case its own process, but the whole
    // binary can also be run in one, and the earlier cases in this file record waits of their own.
    const int before_off = std::max(0, samples(capture_report(), "ul_rx_wait"));
    // ... and the SNAPSHOT is off with it: with the knob unset the receive path must not pay a getrusage call per
    // window, which is what this predicate is (the negative control below breaks exactly this line).
    EXPECT_FALSE(probe.timing_event_snapshot_wanted(std::chrono::nanoseconds(std::chrono::steady_clock::now().time_since_epoch()).count()));
    probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(12)).count());
    probe.record_tx_timing_event(-900, 1234, 0, 55);
    const std::string report = capture_report();
    EXPECT_EQ(report.find("[ul_timing_events]"), std::string::npos)
        << "a slow event with the knob unset must not print anything either";
    // The series itself is unconditional (it is the delivery instrument); only the ranked list has a key.
    EXPECT_EQ(samples(report, "ul_rx_wait"), before_off + 1);
  }

  // ---- ON with a bound of 2: ranked, worst first, bounded, and the floor respected ---------------------------------
  {
    ::setenv("OCUDU_UL_TIMING_EVENTS", "2", 1);
    const int before = std::max(0, samples(capture_report(), "ul_rx_wait"));

    // Four receive waits above the 1 ms floor, in a deliberately unsorted order, plus one below it.
    const auto ms = [](int v) { return std::chrono::nanoseconds(std::chrono::milliseconds(v)).count(); };
    probe.record_rx_wait(ms(3), false, 100, 200, /*air_us=*/35, /*load1_x100=*/468);
    probe.record_rx_wait(ms(12), false, 300, 400, /*air_us=*/35, /*load1_x100=*/468);
    probe.record_rx_wait(ms(1) / 10, false, 500, 600);  // BELOW the floor: must not be kept
    probe.record_rx_wait(ms(5), false, 700, 800, /*air_us=*/36, /*load1_x100=*/120);
    const std::string report = capture_report();

    // The DISTRIBUTION has all five (four above the floor plus the small one): the list is an annotation.
    EXPECT_EQ(samples(report, "ul_rx_wait"), before + 4) << report;

    // ... and the LIST has exactly the two worst, worst first, with the air time and the wall clock.
    const size_t block = report.find("[ul_timing_events] limit=2");
    ASSERT_NE(block, std::string::npos) << report;
    const std::string events = report.substr(block);
    const size_t      first  = events.find("  rx  #1 wait=12000us air=35us wall=");
    const size_t      second = events.find("  rx  #2 wait=5000us air=36us wall=");
    EXPECT_NE(first, std::string::npos) << events;
    EXPECT_NE(second, std::string::npos) << events;
    EXPECT_LT(first, second) << "the list must be printed worst first";
    EXPECT_EQ(events.find("wait=3000us"), std::string::npos)
        << "the third-worst event must be dropped: the list is bounded by the knob";
    EXPECT_EQ(events.find("wait=100us"), std::string::npos) << "an event below the floor must never enter the list";
    // The wall clock is the point of the instrument: without BOTH forms the alignment against the .log cannot be
    // done (the log carries the ISO string, a cross-check wants the integer).
    EXPECT_NE(events.find("epoch_ms="), std::string::npos) << events;
    // ... and it must be a REAL reading, not just a printed label: without the stamp the line says
    // "1970-01-01T00:00:00.000" / "epoch_ms=0", which the label-only check above accepts. The assertion is scoped to
    // THE LINE IT IS ABOUT: the first version searched the whole block and found the DL line's stamp, so breaking
    // the receive stamp alone left it green (found by exactly that negative control, 2026-10-01 - the ARM was
    // patched, the control was not dropped).
    const auto rx_line = (events.find("  rx  #1 ") == std::string::npos)
                             ? std::string()
                             : events.substr(events.find("  rx  #1 "), events.find('\n', events.find("  rx  #1 ")) -
                                                                          events.find("  rx  #1 "));
    EXPECT_TRUE(std::regex_search(rx_line, std::regex(R"(wall=20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3})")))
        << rx_line;
    EXPECT_TRUE(std::regex_search(rx_line, std::regex(R"(epoch_ms=[1-9][0-9]{12,})"))) << rx_line;
    // Both ends of the stall, and the arithmetic between them, so an alignment against the `.log` cannot be off by
    // the stall's own duration: began = end - wait (12 ms here).
    EXPECT_TRUE(std::regex_search(rx_line, std::regex(R"(began_ms=[1-9][0-9]{12,})"))) << rx_line;
    // THE CPU READING (dev doc 6.243). No baseline has been taken in this case yet, so the line must SAY SO with
    // "-" rather than print a zero: "the process used no CPU" and "nobody measured" are different statements, and
    // a zero here would read as the strongest possible evidence of host scheduling.
    EXPECT_NE(rx_line.find(" cpu=- ivcsw=- nvcsw=- base_age=-"), std::string::npos) << rx_line;
    {
      const std::regex  ends(R"(epoch_ms=([0-9]+) began_ms=([0-9]+))");
      std::smatch       m;
      ASSERT_TRUE(std::regex_search(rx_line, m, ends)) << rx_line;
      EXPECT_EQ(std::stoll(m[1].str()) - std::stoll(m[2].str()), 12) << rx_line;
    }
    EXPECT_NE(events.find("load1=4.68"), std::string::npos) << events;
    EXPECT_NE(events.find("load1=1.20"), std::string::npos) << events;
    // A candidate count, so "nothing was slow" and "the instrument never ran" cannot look alike.
    EXPECT_NE(events.find("candidate check(s)"), std::string::npos) << events;
  }

  // ---- the DL side ranks the other way round: the MOST NEGATIVE margin is the worst ---------------------------------
  {
    ::setenv("OCUDU_UL_TIMING_EVENTS", "2", 1);
    probe.record_tx_timing_event(-100, 11, 0, 468);
    probe.record_tx_timing_event(-5000, 22, 0, 468);
    probe.record_tx_timing_event(-900, 33, 0, 468);
    probe.record_tx_timing_event(700, 44, 0, 468);  // above the 500 us floor: not a candidate
    const std::string report = capture_report();
    const size_t      block  = report.find("[ul_timing_events] limit=2");
    ASSERT_NE(block, std::string::npos) << report;
    const std::string events = report.substr(block);
    const size_t      first  = events.find("  dl  #1 margin=-5000us");
    const size_t      second = events.find("  dl  #2 margin=-900us");
    EXPECT_NE(first, std::string::npos) << events;
    EXPECT_NE(second, std::string::npos) << events;
    EXPECT_LT(first, second) << "a hand-over margin goes wrong downwards, so the worst is the most negative";
    EXPECT_EQ(events.find("margin=-100us"), std::string::npos) << "only two are kept";
    EXPECT_EQ(events.find("margin=700us"), std::string::npos) << "an event above the floor must never enter";
  }

  // ---- the CPU delta: with a baseline, a kept event carries what the PROCESS did during its window ------------
  //
  // This is the reading that separates "the radio/USB delivered late" from "this process did not get the CPU", and
  // it exists because the first attempt at that question used load1, which a 12 ms stall cannot move (measured on
  // p178: 3.61 and 4.03 on 14 cores). The arm below therefore pins the WIRING: a baseline taken here, real CPU
  // burned between it and the event, and a positive delta with a bounded age on the printed line. The negative
  // control for it is the "-" assertion in the arms above (no baseline => no reading, never a zero).
  {
    ::setenv("OCUDU_UL_TIMING_EVENTS", "2", 1);
    const auto steady_now = []() {
      return std::chrono::nanoseconds(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    // The baseline FIRST, then the window it is subtracted from - the same order the receive path uses, and the
    // order matters: attach_cpu_delta() refuses a baseline that does not precede the window (a delta against a
    // snapshot taken inside the window would understate the CPU and could be misread as host scheduling).
    // The THROTTLE, before the baseline is used: the first ask is granted, and the next one inside the period is
    // not - that is what bounds the instrument at <=1000 getrusage pairs per second on a thread that may be called
    // 28k times a second.
    EXPECT_TRUE(probe.timing_event_snapshot_wanted(steady_now()));
    const int64_t begin_ns = steady_now();
    probe.timing_event_snapshot(begin_ns);
    EXPECT_FALSE(probe.timing_event_snapshot_wanted(begin_ns + 1));
    EXPECT_TRUE(probe.timing_event_snapshot_wanted(begin_ns + ocudu::ul_pipeline_probe::timing_event_cpu_period_ns));
    volatile double sink = 0.0;
    for (int i = 0; i != 20000000; ++i) {
      sink += static_cast<double>(i) * 1e-9;
    }
    (void)sink;
    const int64_t end_ns = steady_now();
    // A NOMINAL 100 ms wait, so RANKING cannot depend on how fast this machine burns CPU (the first version used the
    // burn's own duration and failed under ctest when it came out below the 12 ms the arms above had recorded - the
    // assertion then read the wrong line). The burn above is what the CPU delta measures, and it is really inside
    // the window [begin_ns, end_ns].
    probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(100)).count(), false, begin_ns, end_ns,
                         /*air_us=*/35, /*load1_x100=*/468);
    const std::string report = capture_report();
    // THE WIRING WARNING (dev doc 6.244). A baseline taken INSIDE the window cannot produce a delta and is refused;
    // on `p179-n78-stress` that happened to every receive event (the call site stamped it after the window began)
    // and the leg's report said only `cpu=-`. The refusal is now counted and printed, and this arm drives it: an
    // event whose window begins BEFORE the baseline must raise the line, so the next leg says why.
    {
      // Deterministic by construction: a FRESH baseline, then an event whose window began before it.
      const int64_t fresh = steady_now();
      probe.timing_event_snapshot(fresh);
      probe.record_rx_wait(std::chrono::nanoseconds(std::chrono::milliseconds(100)).count(), false,
                           /*begin_ns=*/fresh - 1000000, fresh, 35, 468);
      const std::string warned = capture_report();
      EXPECT_NE(warned.find("stamped INSIDE the window"), std::string::npos)
          << "a baseline inside the window must be reported, not silently turned into `-`: " << warned;
    }
    const size_t      at     = report.find("  rx  #1 wait=100000us");
    ASSERT_NE(at, std::string::npos) << report;
    const std::string line = report.substr(at, report.find('\n', at) - at);
    EXPECT_TRUE(std::regex_search(line, std::regex(R"(cpu=[0-9]+\.[0-9]{2}ms)")))
        << "a baseline plus real CPU work must produce a delta: " << line;
    // The delta must also be PLAUSIBLE against its window: a 10-20 ms burn cannot read as 200 ms of CPU.
    {
      std::smatch m;
      const std::regex  parts(R"(wait=([0-9]+)us.*cpu=([0-9]+)\.([0-9]{2})ms)");
      ASSERT_TRUE(std::regex_search(line, m, parts)) << line;
      const double wait_ms = std::stod(m[1].str()) / 1000.0;
      const double cpu_ms  = std::stod(m[2].str() + "." + m[3].str());
      // A 20M-iteration volatile burn is milliseconds of CPU on any machine this runs on; the upper bound is the
      // window plus slack, because the delta covers the WHOLE process and other threads may contribute to it.
      EXPECT_GT(cpu_ms, 0.2) << line;
      EXPECT_LT(cpu_ms, wait_ms + 5.0) << "the process cannot have used more CPU than the window plus 5 ms: " << line;
    }
    EXPECT_TRUE(std::regex_search(line, std::regex(R"(ivcsw=\+[0-9]+ nvcsw=\+[0-9]+ base_age=[0-9]+us)")))
        << "the switch counts and the baseline's age must be printed, not implied: " << line;
  }

  ::unsetenv("OCUDU_UL_TIMING_EVENTS");
  ::unsetenv("OCUDU_UL_PHASE_SEGMENTS");
}

#endif // OCUDU_FLOW_PROBES
