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
# "support" is the directory's label and groups this binary with its neighbours; "phy" is the label the
# ACCEPTANCE command filters on (`ctest -L phy`), and this binary is the offline proof of the UL pipeline probe:
# the report shapes, the slot-completion rule, the [ul_slot_trace] cases and the phase-segment hook contract.
#
# It used to carry "support" only - measured 2026-09-28: `ctest -N -L phy` listed 0 of its 8 cases - so the
# acceptance run this workstream quotes in every milestone, and the offline half of "prove it offline before
# flying a leg", never executed it. Same class as the two gate gaps closed on 2026-09-27 (the knob whitelist and
# the CRC floor): a check that EXISTS but is not in the path the criteria are read from.
# NOTE the ESCAPED semicolon: gtest_discover_tests() takes its PROPERTIES as a flat <name> <value> sequence, so
# an unescaped "support;phy" arrives as "LABELS support" PLUS a stray property named "phy" with an empty value -
# which ctest reports as `LABELS=['support']` and `phy=''` (measured 2026-09-28, `ctest --show-only=json-v1`).
gtest_discover_tests(ul_pipeline_probe_test PROPERTIES LABELS "support\;phy")
