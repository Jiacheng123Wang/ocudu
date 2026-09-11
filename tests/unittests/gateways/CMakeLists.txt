# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


set_directory_properties(PROPERTIES LABELS "gateways")

add_executable(sctp_socket_test sctp_socket_test.cpp)
target_link_libraries(sctp_socket_test ocudu_gateway ocudulog gtest gtest_main)
gtest_discover_tests(sctp_socket_test PROPERTIES "LABELS;sctp")

add_executable(sctp_network_gateway_test
        sctp_network_server_test.cpp
        sctp_network_server_peer_test.cpp
        sctp_network_client_test.cpp
        sctp_network_link_test.cpp)
target_link_libraries(sctp_network_gateway_test ocudu_gateway ocudu_support ocudulog gtest gtest_main)
gtest_discover_tests(sctp_network_gateway_test PROPERTIES "LABELS;sctp" TIMEOUT 300)

add_executable(udp_network_gateway_test udp_network_gateway_test.cpp)
target_link_libraries(udp_network_gateway_test ocudu_gateway ocudu_support ocudulog gtest gtest_main)
gtest_discover_tests(udp_network_gateway_test PROPERTIES "LABELS;tsan")

add_executable(udp_network_gateway_pool_depletion_test udp_network_gateway_pool_depletion_test.cpp)
target_link_libraries(udp_network_gateway_pool_depletion_test ocudu_gateway ocudu_support ocudulog gtest gtest_main)
gtest_discover_tests(udp_network_gateway_pool_depletion_test PROPERTIES "LABELS;tsan")

# Page-aligned baseband buffer (zero-copy GPU FFT prerequisite): interface + alignment tests.
add_executable(baseband_gateway_buffer_dynamic_aligned_test baseband_gateway_buffer_dynamic_aligned_test.cpp)
target_link_libraries(baseband_gateway_buffer_dynamic_aligned_test ocudu_macos_compat ocudu_support ocudulog gtest gtest_main)
gtest_discover_tests(baseband_gateway_buffer_dynamic_aligned_test PROPERTIES "LABELS;gateways")

# Metal zero-copy wrap smoke of the aligned buffer (Apple platforms only).
if (APPLE)
    enable_language(OBJCXX)
    add_executable(baseband_gateway_buffer_metal_smoke_test baseband_gateway_buffer_metal_smoke_test.mm)
    set_source_files_properties(baseband_gateway_buffer_metal_smoke_test.mm
        PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(baseband_gateway_buffer_metal_smoke_test
        ocudu_macos_compat ocudu_support ocudulog gtest gtest_main
        "-framework Metal" "-framework Foundation")
    gtest_discover_tests(baseband_gateway_buffer_metal_smoke_test PROPERTIES "LABELS;gateways")
endif()
