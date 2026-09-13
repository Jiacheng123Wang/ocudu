# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set_directory_properties(PROPERTIES LABELS "o_du_low;apps")

add_executable(du_low_phy_pipeline_test du_low_phy_pipeline_test.cpp)
target_include_directories(du_low_phy_pipeline_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(du_low_phy_pipeline_test
        ocudu_o_du_low_unit_helpers
        ocudu_app_helpers
        ocudu_support
        GTest::gtest
        ocudu_gtest_main)
add_test(du_low_phy_pipeline_test du_low_phy_pipeline_test)
