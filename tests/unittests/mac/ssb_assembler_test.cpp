// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/mac/mac_dl/ssb_assembler.h"
#include "mac_test_helpers.h"
#include "ocudu/mac/mac_cell_result.h"
#include "ocudu/scheduler/result/pdsch_info.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

class ssb_assembler_test : public ::testing::Test
{
protected:
  ssb_assembler_test() : cell_cfg(test_helpers::make_default_mac_cell_config()), assembler(cell_cfg) {}

  dl_ssb_pdu assemble()
  {
    dl_ssb_pdu      pdu{};
    ssb_information info{};
    info.ssb_index = 0;
    assembler.assemble_ssb(pdu, info);
    return pdu;
  }

  mac_cell_creation_request cell_cfg;
  ssb_assembler             assembler;
};

} // namespace

TEST_F(ssb_assembler_test, assembled_mib_reflects_configured_flags)
{
  // make_default_mac_cell_config leaves the MIB flag defaults of mac_cell_creation_request:
  // cellBarred=notBarred, intraFreqReselection=allowed (TS 38.331 Section 6.2.2).
  dl_ssb_pdu pdu = assemble();
  EXPECT_FALSE(pdu.mib_data.cell_barred);
  EXPECT_TRUE(pdu.mib_data.intra_freq_reselection);
}

TEST_F(ssb_assembler_test, set_cell_barred_takes_effect_on_next_assembly)
{
  assembler.set_cell_barred(true);
  EXPECT_TRUE(assemble().mib_data.cell_barred);

  assembler.set_cell_barred(false);
  EXPECT_FALSE(assemble().mib_data.cell_barred);
}

TEST_F(ssb_assembler_test, set_intra_freq_reselection_takes_effect_on_next_assembly)
{
  assembler.set_intra_freq_reselection(false);
  EXPECT_FALSE(assemble().mib_data.intra_freq_reselection);

  assembler.set_intra_freq_reselection(true);
  EXPECT_TRUE(assemble().mib_data.intra_freq_reselection);
}
