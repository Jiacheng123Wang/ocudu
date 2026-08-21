# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

set_directory_properties(PROPERTIES LABELS "ofh;MVP-ARCH-INTF-4")

# The OFH integration test drives a real Ethernet controller through Linux AF_PACKET sockets
# (linux/if_packet.h), which are not available on macOS.
if (NOT APPLE)
    add_executable(ofh_integration_test ofh_integration_test.cpp)
    target_link_libraries(ofh_integration_test
            ocudulog
            ocudu_ru_ofh
            ocudu_phy_support
            ocudu_channel_precoder
            ocudu_support)
    add_test(ofh_integration_test ofh_integration_test -d \'{0,1}\')
    set_tests_properties(ofh_integration_test PROPERTIES LABELS "tsan;NO_MEMCHECK")
else ()
    # macOS: the test cannot be built, but the ctest case is still registered and marked DISABLED so that
    # "make test" reports it as "Not Run (Disabled)" instead of silently dropping it from the test list.
    add_test(NAME ofh_integration_test COMMAND ${CMAKE_COMMAND} -E true)
    set_tests_properties(ofh_integration_test PROPERTIES DISABLED TRUE LABELS "tsan;NO_MEMCHECK;macos_unsupported")
endif ()
