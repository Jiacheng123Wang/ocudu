# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_subdirectory(downlink)
add_subdirectory(uplink)

add_executable(baseband_cfo_processor_test baseband_cfo_processor_test.cpp)
target_include_directories(baseband_cfo_processor_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/phy/lower/processors)
target_link_libraries(baseband_cfo_processor_test ocudu_lower_phy_uplink_processor ocuduvec GTest::gtest GTest::gtest_main)
add_test(baseband_cfo_processor_test baseband_cfo_processor_test)

add_executable(resource_request_pool_test resource_request_pool_test.cpp)
target_include_directories(resource_request_pool_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/phy/lower/processors)
target_link_libraries(resource_request_pool_test ocudu_ran ocudulog ocudu_support GTest::gtest GTest::gtest_main)
add_test(resource_request_pool_test resource_request_pool_test)
