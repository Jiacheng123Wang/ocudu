// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/du/du_high/du_manager/converters/asn1_sys_info_packer.h"
#include "ocudu/adt/format.h"
#include "ocudu/asn1/rrc_nr/sys_info.h"
#include "ocudu/du/du_cell_config_helpers.h"
#include "ocudu/du/du_cell_config_validation.h"
#include "ocudu/pcap/mac_pcap.h"
#include "ocudu/ran/sib/system_info_config.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace odu;

// Global variables for PCAP support, required by du_manager_converters.
bool             g_enable_pcap = false;
ocudu::mac_pcap* g_pcap        = nullptr;

/// A cell configured with a reserved (dormant) SI-message occasion for SIB6, but no etws_cfg content -- i.e. the
/// SI-message requires activation and has no matching entry in si_config->sibs yet.
static du_cell_config make_cell_config_with_dormant_pws_si_message()
{
  du_cell_config cfg = config_helpers::make_default_du_cell_config();

  cfg.si.si_config.emplace();
  cfg.si.si_config->si_window_len_slots = 10;

  si_message_sched_info si_msg;
  si_msg.sib_mapping_info       = {sib_type::sib6};
  si_msg.si_period_radio_frames = 32;
  cfg.si.si_config->si_sched_info.push_back(si_msg);
  // Note: si_config->sibs is left empty -- no content configured for the dormant SIB6, which is what makes this SI
  // message require activation.

  return cfg;
}

TEST(asn1_sib1_sched_info_test, dormant_pws_si_message_is_still_advertised_in_scheduling_info_list)
{
  du_cell_config cell_cfg = make_cell_config_with_dormant_pws_si_message();

  byte_buffer buf = asn1_packer::pack_sib1(cell_cfg);

  asn1::cbit_ref       bref{buf};
  asn1::rrc_nr::sib1_s sib1;
  ASSERT_EQ(sib1.unpack(bref), asn1::OCUDUASN_SUCCESS);

  // Regression test: a dormant, unconfigured PWS SI-message (SIB6/7/8 with no matching si_config->sibs entry) must
  // still get a schedulingInfoList entry -- the UE has no way to discover SIB6/7/8 exist otherwise, even once the
  // P-RNTI short-message notification correctly wakes it up.
  ASSERT_TRUE(sib1.si_sched_info_present) << "schedulingInfoList must not be dropped for a dormant PWS SI-message";
  ASSERT_EQ(sib1.si_sched_info.sched_info_list.size(), 1);

  const auto& sched_info = sib1.si_sched_info.sched_info_list[0];
  ASSERT_EQ(sched_info.sib_map_info.size(), 1);
  EXPECT_EQ(sched_info.sib_map_info[0].type.value, asn1::rrc_nr::sib_type_info_s::type_opts::sib_type6);
  EXPECT_FALSE(sched_info.sib_map_info[0].value_tag_present) << "Dormant SIB has no content, hence no value tag";

  // A PWS SI-message is only broadcast while a warning is on air. Advertising it as broadcasting while dormant makes
  // UEs try to acquire an SI-message that is not being transmitted.
  EXPECT_EQ(sched_info.si_broadcast_status.value,
            asn1::rrc_nr::sched_info_s::si_broadcast_status_opts::not_broadcasting);
}

TEST(asn1_sib1_sched_info_test, non_pws_si_message_is_advertised_as_broadcasting)
{
  du_cell_config cell_cfg                                       = make_cell_config_with_dormant_pws_si_message();
  cell_cfg.si.si_config->si_sched_info.front().sib_mapping_info = {sib_type::sib2};

  sib2_info sib2;
  sib2.q_hyst                    = q_hyst_t::db4;
  sib2.thresh_serving_low_p      = reselection_threshold_t{14};
  sib2.cell_reselection_priority = cell_reselection_priority_t{4};
  sib2.q_rx_lev_min              = q_rx_lev_min_t{-70};
  sib2.s_intra_search_p          = reselection_threshold_t{31};
  sib2.t_reselection_nr          = t_reselection_t{1};
  cell_cfg.si.si_config->sibs.push_back(sib_type_info{sib2, value_tag_t{0}});

  byte_buffer buf = asn1_packer::pack_sib1(cell_cfg);

  asn1::cbit_ref       bref{buf};
  asn1::rrc_nr::sib1_s sib1;
  ASSERT_EQ(sib1.unpack(bref), asn1::OCUDUASN_SUCCESS);

  ASSERT_TRUE(sib1.si_sched_info_present);
  ASSERT_EQ(sib1.si_sched_info.sched_info_list.size(), 1);
  EXPECT_EQ(sib1.si_sched_info.sched_info_list[0].si_broadcast_status.value,
            asn1::rrc_nr::sched_info_s::si_broadcast_status_opts::broadcasting);
}

TEST(asn1_sib1_sched_info_test, pws_sib_mixed_with_other_sibs_in_one_si_message_is_rejected)
{
  // si-BroadcastStatus applies to a whole SI message, so appending a non-PWS SIB to a PWS SI-message would take that
  // SIB off the air while no warning is on-going.
  du_cell_config cell_cfg = make_cell_config_with_dormant_pws_si_message();
  cell_cfg.si.si_config->si_sched_info[0].sib_mapping_info.push_back(sib_type::sib2);

  ASSERT_FALSE(is_du_cell_config_valid(cell_cfg).has_value());
}

TEST(asn1_sib1_sched_info_test, dormant_pws_si_message_alone_in_its_si_message_is_accepted)
{
  ASSERT_TRUE(is_du_cell_config_valid(make_cell_config_with_dormant_pws_si_message()).has_value());
}

int main(int argc, char** argv)
{
  ocudulog::init();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
