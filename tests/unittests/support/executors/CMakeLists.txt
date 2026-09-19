# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set_directory_properties(PROPERTIES LABELS "support")

add_executable(task_worker_test
        sync_task_executor_test.cpp
        task_worker_test.cpp
        strand_executor_test.cpp
        task_execution_manager_test.cpp
        metrics_executor_test.cpp
        fork_limit_executor_test.cpp)
target_link_libraries(task_worker_test ocudulog ocudu_support GTest::gtest ocudu_gtest_main)
gtest_discover_tests(task_worker_test PROPERTIES LABELS "tsan")

# The UL pipeline probe's report shapes (see the test's file comment). Header-only and inert unless
# ENABLE_FLOW_PROBES is on: the test skips itself in a build without the probes rather than asserting
# on no-ops.
add_executable(ul_pipeline_probe_test ul_pipeline_probe_test.cpp)
target_link_libraries(ul_pipeline_probe_test ocudulog ocudu_support GTest::gtest ocudu_gtest_main)
gtest_discover_tests(ul_pipeline_probe_test PROPERTIES LABELS "support")
