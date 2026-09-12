# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

set_directory_properties(PROPERTIES LABELS "phy")

add_executable(phy_metrics_deferred_chain_test phy_metrics_deferred_chain_test.cpp)
target_include_directories(phy_metrics_deferred_chain_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/phy/metrics)
target_link_libraries(phy_metrics_deferred_chain_test
        ocudu_phy_metrics
        ocudu_channel_equalizer
        ocudu_phy_support
        ocudulog
        GTest::gtest
        GTest::gtest_main)
add_test(phy_metrics_deferred_chain_test phy_metrics_deferred_chain_test)
