// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/du/du_low/du_low_executor_mapper.h"
#include "ocudu/phy/upper/upper_phy_execution_configuration.h"
#include "ocudu/support/executors/inline_task_executor.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace odu;

// P0-6 of the latency workstream (doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md 6.1).
//
// The resolved PUSCH/SRS concurrency decides whether the uplink LANE's executor is a serialising
// STRAND or an N-way task fork limiter, and that distinction is the first question the latency work
// opens with: a long channel-estimation segment is lane QUEUEING only if the lane really serves one
// hop at a time. Two things were true before this file existed:
//
//   * the rule lived only in source - create_task_fork_limiter() returns a strand when the limit is
//     <= 1, and a fork limiter of that many threads otherwise;
//   * the resolved VALUE was in no leg and no dump (the config default is `auto`, and the YAML dump
//     writes the sentinel back out), so the only readings that ever named a "single lane" came from
//     n1 legs (5.9.33/5.9.35) - which need not describe an n78 cell, because the derivation scales
//     with bandwidth and with the TDD uplink slot ratio.
//
// The mapper now prints both (see du_low_config_translator.cpp and du_low_executor_mapper.cpp, tag
// [ul_lane_exec]). These cases pin the rule so a future edit cannot quiet-changed it, and they are
// what shows the print firing offline; the on-air confirmation is the next leg's stderr.

namespace {

du_low_executor_mapper_config make_config(task_executor&                    base,
                                          unsigned                          max_pusch_and_srs_concurrency,
                                          unsigned                          medium_pool_concurrency)
{
  du_low_executor_mapper_flexible_exec_config flexible;
  flexible.rt_hi_prio_exec          = {&base, 1};
  flexible.non_rt_hi_prio_exec      = {&base, 1};
  flexible.non_rt_medium_prio_exec  = {&base, medium_pool_concurrency};
  flexible.non_rt_low_prio_exec     = {&base, 1};
  flexible.max_pucch_concurrency    = 0;
  flexible.max_pusch_and_srs_concurrency = max_pusch_and_srs_concurrency;
  flexible.max_pdsch_concurrency    = 0;

  du_low_executor_mapper_config config;
  config.executors                = flexible;
  config.exec_metrics_channel_registry = nullptr;
  config.executor_tracing_enable      = false;
  return config;
}

} // namespace

// A limit of one is not "at most one task" - it is a STRAND: the lane serialises, one hop at a time.
TEST(du_low_executor_mapper_test, a_limit_of_one_makes_the_pusch_lane_a_serialising_strand)
{
  inline_task_executor base;
  auto                 mapper = create_du_low_executor_mapper(make_config(base, 1, 12));
  ASSERT_NE(mapper, nullptr);

  const upper_phy_execution_configuration& phy = mapper->get_upper_phy_execution_config();
  EXPECT_EQ(phy.pusch_executor.max_concurrency, 1U);
  EXPECT_EQ(phy.pusch_ch_estimator_executor.max_concurrency, 1U);
}

// Above one it is a task fork limiter of that many threads, and every view shares the same limit.
TEST(du_low_executor_mapper_test, a_limit_above_one_makes_the_pusch_lane_an_n_way_fork_limiter)
{
  inline_task_executor base;
  auto                 mapper = create_du_low_executor_mapper(make_config(base, 3, 12));
  ASSERT_NE(mapper, nullptr);

  const upper_phy_execution_configuration& phy = mapper->get_upper_phy_execution_config();
  EXPECT_EQ(phy.pusch_executor.max_concurrency, 3U);
  EXPECT_EQ(phy.pusch_ch_estimator_executor.max_concurrency, 3U);
}

// The pool is the hard ceiling, and the mapper REFUSES a limit above it (report_error_if_not: "Maximum
// PUSCH and SRS concurrency (i.e., N) exceeds the number of main pool threads (i.e., M)") rather than
// clamping - so a limit equal to the pool is the largest accepted value and yields exactly that size.
TEST(du_low_executor_mapper_test, a_limit_equal_to_the_medium_pool_is_accepted_as_the_ceiling)
{
  inline_task_executor base;
  auto                 mapper = create_du_low_executor_mapper(make_config(base, 12, 12));
  ASSERT_NE(mapper, nullptr);

  const upper_phy_execution_configuration& phy = mapper->get_upper_phy_execution_config();
  EXPECT_EQ(phy.pusch_executor.max_concurrency, 12U);
}

// A zero limit means "no limit" and is clamped to the pool as well - never to one.
TEST(du_low_executor_mapper_test, a_zero_limit_means_no_limit_and_becomes_the_pool_concurrency)
{
  inline_task_executor base;
  auto                 mapper = create_du_low_executor_mapper(make_config(base, 0, 6));
  ASSERT_NE(mapper, nullptr);

  const upper_phy_execution_configuration& phy = mapper->get_upper_phy_execution_config();
  EXPECT_EQ(phy.pusch_executor.max_concurrency, 6U);
}
