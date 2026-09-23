# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


# "phy" is not decoration: this is THE pipeline-mode resolver test (phy_pipeline_mode cpu/cpu_gpu/gpu,
# metal_mmse, device_grid, and the mode-vs-knob conflict rules), and the milestone gate is
# `ctest -L phy`. Without the label it was invisible to that gate while the old name-regex gate only
# caught it by accident (`du_low` matched) - the 5.9.120 audit found exactly that asymmetry.
set_directory_properties(PROPERTIES LABELS "o_du_low;apps;phy")

add_executable(du_low_phy_pipeline_test du_low_phy_pipeline_test.cpp)
target_include_directories(du_low_phy_pipeline_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(du_low_phy_pipeline_test
        ocudu_o_du_low_unit_helpers
        ocudu_app_helpers
        ocudu_support
        GTest::gtest
        ocudu_gtest_main)
add_test(du_low_phy_pipeline_test du_low_phy_pipeline_test)
