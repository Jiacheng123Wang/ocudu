// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief slot_point_extender_adaptor keeps slot_point_extended continuous across an SFN wrap.

#include "apps/units/flexible_o_du/split_6/o_du_high/slot_point_extender_adaptor.h"
#include "ocudu/fapi/p7/messages/slot_indication.h"
#include "ocudu/ran/slot_point_extended.h"
#include "fmt/format.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;

namespace {

class slot_indication_recorder : public fapi::p7_slot_indication_notifier
{
public:
  std::vector<slot_point_extended> slots;

  void on_slot_indication(const fapi::slot_indication& msg) override { slots.push_back(msg.slot); }
};

class slot_point_extender_adaptor_test : public ::testing::Test
{
protected:
  static constexpr subcarrier_spacing scs = subcarrier_spacing::kHz30;

  slot_indication_recorder    recorder;
  slot_point_extender_adaptor adaptor{std::chrono::microseconds{500}, recorder};

  static uint32_t slots_per_hyper_frame() { return slot_point(scs, 0).nof_slots_per_hyper_system_frame(); }

  void feed(uint32_t raw_slot_count)
  {
    adaptor.on_slot_indication(
        fapi::slot_indication{.slot = slot_point_extended(slot_point(scs, raw_slot_count), 0), .time_point = {}});
  }
};

} // namespace

TEST_F(slot_point_extender_adaptor_test, timeline_is_continuous_across_hyper_sfn_wrap)
{
  const uint32_t slots_per_hyper = slots_per_hyper_frame();

  for (uint32_t i = 0; i != 6; ++i) {
    feed((slots_per_hyper - 3 + i) % slots_per_hyper);
  }

  ASSERT_EQ(recorder.slots.size(), 6);
  for (unsigned i = 1; i != recorder.slots.size(); ++i) {
    const int advance = static_cast<int>(recorder.slots[i] - recorder.slots[i - 1]);
    EXPECT_EQ(advance, 1) << "extended slot jumped by " << advance << " between "
                          << fmt::format("{}", recorder.slots[i - 1]) << " and "
                          << fmt::format("{}", recorder.slots[i]);
  }
}

TEST_F(slot_point_extender_adaptor_test, hyper_sfn_advances_once_per_wrap)
{
  const uint32_t slots_per_hyper = slots_per_hyper_frame();
  const uint32_t first_raw_slot  = slots_per_hyper - 3;
  const uint32_t nof_indications = 2 * slots_per_hyper + 6;
  const uint32_t nof_wraps       = (first_raw_slot + nof_indications - 1) / slots_per_hyper;

  for (uint32_t i = 0; i != nof_indications; ++i) {
    feed((first_raw_slot + i) % slots_per_hyper);
  }

  ASSERT_EQ(recorder.slots.size(), nof_indications);
  for (unsigned i = 1; i != recorder.slots.size(); ++i) {
    ASSERT_EQ(static_cast<int>(recorder.slots[i] - recorder.slots[i - 1]), 1)
        << "extended slot is not continuous at " << fmt::format("{}", recorder.slots[i]);
  }

  const uint32_t first_hyper_sfn = recorder.slots.front().hyper_sfn();
  const uint32_t last_hyper_sfn  = recorder.slots.back().hyper_sfn();
  EXPECT_EQ((last_hyper_sfn - first_hyper_sfn + NOF_HYPER_SFNS) % NOF_HYPER_SFNS, nof_wraps);
}

/// 10 s of slots at a 1 s period, starting three slots before the SFN wrap, produces 10 reports.
TEST_F(slot_point_extender_adaptor_test, ten_seconds_of_slots_produces_ten_metric_reports)
{
  constexpr unsigned period_slots    = 2000;  // 1 s at 30 kHz
  constexpr unsigned nof_indications = 20000; // 10 s
  const uint32_t     slots_per_hyper = slots_per_hyper_frame();
  const uint32_t     first_raw_slot  = slots_per_hyper - 3;

  slot_indication_recorder    last_only;
  slot_point_extender_adaptor under_test{std::chrono::microseconds{500}, last_only};

  slot_point_extended next_report_end;
  unsigned            nof_reports = 0;

  for (uint32_t i = 0; i != nof_indications; ++i) {
    under_test.on_slot_indication(fapi::slot_indication{
        .slot = slot_point_extended(slot_point(scs, (first_raw_slot + i) % slots_per_hyper), 0), .time_point = {}});
    const slot_point_extended sl = last_only.slots.back();
    last_only.slots.clear();

    if (not next_report_end.valid()) {
      const unsigned slot_mod = sl.count() % period_slots;
      next_report_end         = sl + period_slots - slot_mod;
    }
    if (sl >= next_report_end - 1) {
      ++nof_reports;
      next_report_end += period_slots;
    }
  }

  EXPECT_EQ(nof_reports, 10) << "10 s at a 1 s metrics period should print 10 UE rows; a hyper-frame wrap jump "
                                "silences the aggregator for ~10.24 s instead";
}
