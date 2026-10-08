# The OFH integration test drives a real Ethernet controller through Linux AF_PACKET sockets
# (linux/if_packet.h), which are not available on macOS.
if (NOT APPLE)
    add_executable(ofh_integration_test
            ofh_integration_test.cpp
            ofh_integration_test_config.cpp
            ofh_integration_test_non_rt_ru_factory.cpp
            ofh_integration_test_helpers_cat_b.cpp)
    target_include_directories(ofh_integration_test PRIVATE ${CMAKE_SOURCE_DIR}/lib/ofh ${CMAKE_SOURCE_DIR}/lib/ru/ofh)
    target_link_libraries(ofh_integration_test
            ocudulog
            ocudu_ru_ofh
            ocudu_phy_support
            ocudu_channel_precoder
            ocudu_support
            ocudu_yaml_util)

    add_test(ofh_integration_test_non_rt ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            --dl_port_id 0 1)
    add_test(ofh_integration_test_non_rt_4x2 ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200)
    add_test(ofh_integration_test_non_rt_8x8 ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            --dl_port_id 0 1 2 3 4 5 6 7 --ul_port_id 0 1 2 3 4 5 6 7 --prach_port_id 8 9)
    add_test(ofh_integration_test_non_rt_8x8_100MHz ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            --channel_bandwidth_MHz 100 --dl_port_id 0 1 2 3 4 5 6 7 --ul_port_id 0 1 2 3 4 5 6 7 --prach_port_id 8 9)
    add_test(ofh_integration_test_non_rt_custom_eaxc ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            --dl_port_id 5 7 --ul_port_id 9 3 --prach_port_id 1 2)
    add_test(ofh_integration_test_non_rt_fragmented_no_compr ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            --mtu 1500 --scs 15 --channel_bandwidth_MHz 50 --tdd_pattern 6d3u
            --compr_method_data none --compr_bitwidth_data 16 --enable_dl_static_compr_hdr true)

    # Category B
    add_test(ofh_integration_test_non_rt_cat_b_4x2 ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            beamforming --enable true)
    add_test(ofh_integration_test_non_rt_cat_b_8x8 ofh_integration_test --non_realtime true --non_rt_time_scale 20 --nof_test_slots 200
            --dl_port_id 0 1 2 3 4 5 6 7 --ul_port_id 0 1 2 3 4 5 6 7 --prach_port_id 8 9
            beamforming --enable true --compr_method_bfw bfp --compr_bitwidth_bfw 9)

    # There is no bare `ofh_integration_test` case: upstream replaced the `-d '{0,1}'` form (which CLI11
    # rejects, because CMake passes that argument literally) and then dropped the case altogether once
    # `ofh_integration_test_non_rt` carried its `--dl_port_id 0 1` coverage. Keeping the old line would
    # have registered a test that can only fail, which is what this branch did until the Ubuntu run
    # reported it.

    set_tests_properties(ofh_integration_test_non_rt
                         ofh_integration_test_non_rt_4x2
                         ofh_integration_test_non_rt_8x8
                         ofh_integration_test_non_rt_8x8_100MHz
                         ofh_integration_test_non_rt_custom_eaxc
                         ofh_integration_test_non_rt_fragmented_no_compr
                         ofh_integration_test_non_rt_cat_b_4x2
                         ofh_integration_test_non_rt_cat_b_8x8
                         PROPERTIES LABELS "tsan;NO_MEMCHECK")

    set_property(TEST ofh_integration_test_non_rt_8x8 ofh_integration_test_non_rt_8x8_100MHz
                 APPEND PROPERTY LABELS "MVP-FUNC-PHY-16-9")
    set_property(TEST ofh_integration_test_non_rt_cat_b_4x2 ofh_integration_test_non_rt_cat_b_8x8
                 APPEND PROPERTY LABELS "RU-OFH-CATB-WDBF")
else ()
    # macOS: the test cannot be built, but the ctest cases are still registered and marked DISABLED so that
    # "make test" reports them as "Not Run (Disabled)" instead of silently dropping them from the test list.
    #
    # One placeholder per case the Linux branch registers, under the SAME names: a single placeholder named
    # after the binary would leave eight Linux names with no macOS counterpart, which is the silently-dropped
    # coverage this pattern exists to prevent - and the name it used to carry (`ofh_integration_test`) is not a
    # case on Linux at all, so it only ever appeared in a macOS list.
    foreach (ofh_case
            ofh_integration_test_non_rt
            ofh_integration_test_non_rt_4x2
            ofh_integration_test_non_rt_8x8
            ofh_integration_test_non_rt_8x8_100MHz
            ofh_integration_test_non_rt_custom_eaxc
            ofh_integration_test_non_rt_fragmented_no_compr
            ofh_integration_test_non_rt_cat_b_4x2
            ofh_integration_test_non_rt_cat_b_8x8)
        add_test(NAME ${ofh_case} COMMAND ${CMAKE_COMMAND} -E true)
        set_tests_properties(${ofh_case} PROPERTIES DISABLED TRUE
                             LABELS "tsan;NO_MEMCHECK;macos_unsupported")
    endforeach ()

    # The labels the Linux branch appends on top of the two above, so a `ctest -L` filter selects the same
    # cases on both platforms and a macOS run cannot look like it covered them.
    set_property(TEST ofh_integration_test_non_rt_8x8 ofh_integration_test_non_rt_8x8_100MHz
                 APPEND PROPERTY LABELS "MVP-FUNC-PHY-16-9")
    set_property(TEST ofh_integration_test_non_rt_cat_b_4x2 ofh_integration_test_non_rt_cat_b_8x8
                 APPEND PROPERTY LABELS "RU-OFH-CATB-WDBF")
endif ()
