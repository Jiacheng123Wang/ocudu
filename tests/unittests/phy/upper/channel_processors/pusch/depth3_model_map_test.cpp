// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "lib/phy/upper/channel_processors/pusch/pusch_depth3_model_map.h"
#include <algorithm>
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

/// The four modulations the arm can serve, with the slot index the command line gives them.
constexpr unsigned qpsk   = 2;
constexpr unsigned qam16  = 4;
constexpr unsigned qam64  = 6;
constexpr unsigned qam256 = 8;

} // namespace

TEST(depth3_model_map_test, a_slot_serves_its_own_modulation)
{
  depth3_model_map models;
  EXPECT_EQ(models.add_slot(qpsk, qpsk, "/m/d3_qpsk.mlmodelc").result, depth3_model_map::status::kept);

  const depth3_model_map::entry* entry = models.find(qpsk);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->qm, qpsk);
  EXPECT_EQ(entry->path, "/m/d3_qpsk.mlmodelc");
  EXPECT_FALSE(entry->fallback);

  // And nothing else is served: the coverage line has to be able to say which modulations degrade.
  EXPECT_EQ(models.find(qam256), nullptr);
  EXPECT_EQ(models.missing(), (std::vector<unsigned>{qam16, qam64, qam256}));
}

TEST(depth3_model_map_test, a_slot_whose_bundle_declares_another_modulation_is_refused)
{
  // The mistake this catches: `--pusch_receiver_model_qpsk=<a 64QAM bundle>` looks configured and would serve
  // nothing, because the arm's envelope is the bundle's own declaration -- and only binding can know it.
  depth3_model_map models;
  const auto       outcome = models.add_slot(qpsk, qam64, "/m/d3_64qam.mlmodelc");

  EXPECT_EQ(outcome.result, depth3_model_map::status::refused);
  EXPECT_NE(outcome.reason.find("Qm=2"), std::string::npos) << outcome.reason;
  EXPECT_NE(outcome.reason.find("Qm=6"), std::string::npos) << outcome.reason;
  EXPECT_EQ(models.find(qpsk), nullptr);
  EXPECT_TRUE(models.entries().empty());
}

TEST(depth3_model_map_test, the_fallback_serves_what_no_slot_claims)
{
  depth3_model_map models;
  EXPECT_EQ(models.add_slot(qpsk, qpsk, "/m/d3_qpsk.mlmodelc").result, depth3_model_map::status::kept);
  EXPECT_EQ(models.add_fallback(qam256, "/m/d3_256qam.mlmodelc").result, depth3_model_map::status::kept);

  EXPECT_EQ(models.find(qpsk)->path, "/m/d3_qpsk.mlmodelc");
  EXPECT_EQ(models.find(qam256)->path, "/m/d3_256qam.mlmodelc");
  EXPECT_TRUE(models.find(qam256)->fallback);
  EXPECT_EQ(models.missing(), (std::vector<unsigned>{qam16, qam64}));
}

TEST(depth3_model_map_test, a_fallback_a_slot_already_covers_is_redundant_not_an_error)
{
  // Measured context for this rule: the fallback is filled by the build's default, so `--pusch_receiver_model`
  // and a slot can end up declaring the same modulation through no fault of the operator's. It must be said and
  // not refused -- and the slot wins, so the coverage does not change.
  depth3_model_map models;
  EXPECT_EQ(models.add_slot(qam256, qam256, "/m/d3_256qam_slot.mlmodelc").result, depth3_model_map::status::kept);

  const auto outcome = models.add_fallback(qam256, "/m/d3_256qam.mlmodelc");
  EXPECT_EQ(outcome.result, depth3_model_map::status::redundant);
  EXPECT_FALSE(outcome.reason.empty());
  EXPECT_EQ(models.find(qam256)->path, "/m/d3_256qam_slot.mlmodelc");
  EXPECT_FALSE(models.find(qam256)->fallback);
  EXPECT_EQ(models.entries().size(), 1);
}

TEST(depth3_model_map_test, a_bundle_declaring_a_modulation_the_arm_cannot_serve_is_refused)
{
  depth3_model_map models;
  const auto       outcome = models.add_fallback(1, "/m/d3_bpsk.mlmodelc");

  EXPECT_EQ(outcome.result, depth3_model_map::status::refused);
  EXPECT_NE(outcome.reason.find("Qm=1"), std::string::npos) << outcome.reason;
  EXPECT_EQ(models.find(1), nullptr);
}

TEST(depth3_model_map_test, a_bundle_that_did_not_load_declares_nothing)
{
  // declared_qm == 0 is the arm's "the bundle did not load": the engine reports 0 and there is no modulation to
  // serve. It must be refused with that reason, not bound as a model that serves nothing.
  depth3_model_map models;
  const auto       slot = models.add_slot(qam16, 0, "/m/not_a_bundle");
  EXPECT_EQ(slot.result, depth3_model_map::status::refused);
  EXPECT_FALSE(slot.reason.empty());

  const auto fallback = models.add_fallback(0, "/m/not_a_bundle");
  EXPECT_EQ(fallback.result, depth3_model_map::status::refused);
  EXPECT_FALSE(fallback.reason.empty());
}

TEST(depth3_model_map_test, all_four_slots_serve_all_four_modulations)
{
  depth3_model_map models;
  EXPECT_EQ(models.add_slot(qpsk, qpsk, "/m/q").result, depth3_model_map::status::kept);
  EXPECT_EQ(models.add_slot(qam16, qam16, "/m/16").result, depth3_model_map::status::kept);
  EXPECT_EQ(models.add_slot(qam64, qam64, "/m/64").result, depth3_model_map::status::kept);
  EXPECT_EQ(models.add_slot(qam256, qam256, "/m/256").result, depth3_model_map::status::kept);

  for (unsigned qm : depth3_model_map::candidate_qm) {
    ASSERT_NE(models.find(qm), nullptr) << "Qm=" << qm;
  }
  EXPECT_TRUE(models.missing().empty());
  EXPECT_EQ(models.entries().size(), 4);
}

TEST(depth3_model_map_test, a_conditioned_bundle_serves_every_modulation_it_declares)
{
  // One artifact, four modulations: the per-reception lookup is unchanged, and what the bundle serves comes from
  // its DECLARATION -- the only thing that can say it, since its main head is as wide as a 256QAM model's.
  depth3_model_map models;
  EXPECT_EQ(models.add_conditioned({qpsk, qam16, qam64, qam256}, "/m/d3_modcond.mlmodelc").result,
            depth3_model_map::status::kept);
  for (unsigned qm : depth3_model_map::candidate_qm) {
    ASSERT_NE(models.find(qm), nullptr) << "Qm=" << qm;
    EXPECT_EQ(models.find(qm)->path, "/m/d3_modcond.mlmodelc");
    EXPECT_TRUE(models.find(qm)->conditioned);
  }
  EXPECT_TRUE(models.missing().empty());
}

TEST(depth3_model_map_test, a_slot_wins_over_the_conditioned_bundle_and_partial_use_is_reported)
{
  depth3_model_map models;
  EXPECT_EQ(models.add_slot(qam256, qam256, "/m/d3_256qam.mlmodelc").result, depth3_model_map::status::kept);
  const auto outcome = models.add_conditioned({qpsk, qam16, qam64, qam256}, "/m/d3_modcond.mlmodelc");

  // Not an error, but it must not pass unread: the bundle serves three of the four it declares.
  EXPECT_EQ(outcome.result, depth3_model_map::status::redundant);
  EXPECT_NE(outcome.reason.find("4"), std::string::npos) << outcome.reason;
  EXPECT_NE(outcome.reason.find("3"), std::string::npos) << outcome.reason;
  EXPECT_EQ(models.find(qam256)->path, "/m/d3_256qam.mlmodelc");
  EXPECT_FALSE(models.find(qam256)->conditioned);
  EXPECT_TRUE(models.find(qpsk)->conditioned);
}

TEST(depth3_model_map_test, a_conditioned_bundle_naming_a_modulation_the_arm_cannot_serve_is_refused)
{
  depth3_model_map models;
  const auto       outcome = models.add_conditioned({qpsk, 3}, "/m/bogus.mlmodelc");
  EXPECT_EQ(outcome.result, depth3_model_map::status::refused);
  EXPECT_NE(outcome.reason.find("Qm=3"), std::string::npos) << outcome.reason;
  EXPECT_EQ(models.add_conditioned({}, "/m/empty.mlmodelc").result, depth3_model_map::status::refused);
}
