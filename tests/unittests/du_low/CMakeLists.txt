# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

# LABELS deliberately do NOT include "phy": the milestone audit's label gate counts `ctest -L phy`
# (193 tests at the merged HEAD), and this is a DU-low executor-configuration test, not a PHY test.
set_directory_properties(PROPERTIES LABELS "du_low;du_high")

include_directories(../../..)

add_executable(du_low_executor_mapper_test du_low_executor_mapper_test.cpp)
target_link_libraries(du_low_executor_mapper_test ocudu_o_du_low ocudu_support ocudulog gtest ocudu_gtest_main)
gtest_discover_tests(du_low_executor_mapper_test)
