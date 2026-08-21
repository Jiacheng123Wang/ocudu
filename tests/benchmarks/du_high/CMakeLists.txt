# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "du_high|tsan|rtsan")

include_directories(../../..)

# The DU-high benchmark pins its threads with the Linux CPU-affinity APIs (cpu_set_t, CPU_ZERO, CPU_SET,
# pthread_setaffinity_np), which are not available on macOS.
if (NOT APPLE)
    add_executable(du_high_benchmark du_high_benchmark.cpp)
    target_link_libraries(du_high_benchmark
      ocudu_du_high
      f1ap_du_test_helpers
      pdcp_test_doubles
      sched_test_doubles
      mac_test_doubles
      du_test_doubles
      ocudu_pcap
      gtest
    )
    add_test(du_high_benchmark du_high_benchmark)
else ()
    # macOS: the benchmark cannot be built, but the ctest case is still registered and marked DISABLED so that
    # "make test" reports it as "Not Run (Disabled)" instead of silently dropping it from the test list.
    add_test(NAME du_high_benchmark COMMAND ${CMAKE_COMMAND} -E true)
    set_tests_properties(du_high_benchmark PROPERTIES DISABLED TRUE LABELS "macos_unsupported")
endif ()
