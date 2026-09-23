# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_subdirectory(prach)
add_subdirectory(puxch)

add_executable(lower_phy_uplink_processor_test lower_phy_uplink_processor_test.cpp)
target_include_directories(lower_phy_uplink_processor_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
target_link_libraries(lower_phy_uplink_processor_test ocudu_lower_phy_uplink_processor ocudulog GTest::gtest GTest::gtest_main)
add_test(lower_phy_uplink_processor_test lower_phy_uplink_processor_test)
# The `host sample assembly` contract arm needs a process whose assembly counters are still clean (it has
# to move that check's verdict on its own), so it gets its own ctest entry - its own process. Only ONE
# parametrization can own that clean state: the first one that runs. The rest would find the counters
# already moved and SKIP themselves with that reason (design document 5.9.120; same shape as
# pusch_demodulator_ce_contract_arm in 5.9.112), so the entry names /0 and the unfiltered binary keeps
# skipping the others - which is the honest outcome, not a silent pass.
add_test(lower_phy_uplink_processor_assembly_arm lower_phy_uplink_processor_test
         --gtest_filter=*HostSampleAssemblyContractArms/0)
# A build without the probes (ENABLE_METAL_STATS=OFF, the default configure) does not register that check,
# so the arm has nothing to move and SKIPS itself with that reason. Make ctest show it as skipped rather
# than as a pass: "the arm did not run here" must not look like "the arm passed here".
set_tests_properties(lower_phy_uplink_processor_assembly_arm PROPERTIES
                     SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
