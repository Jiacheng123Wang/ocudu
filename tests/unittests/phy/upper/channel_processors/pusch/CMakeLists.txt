# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy")

add_executable(pusch_processor_validator_test pusch_processor_validator_test.cpp)
target_link_libraries(pusch_processor_validator_test
        ocudu_channel_equalizer
        ocudu_channel_processors
        ocudu_phy_support
        ocudulog
        ocudu_upper_phy_support
        ocudu_transform_precoding
        GTest::gtest
        GTest::gtest_main)
add_test(pusch_processor_validator_test pusch_processor_validator_test)

add_executable(pusch_processor_unittest pusch_processor_unittest.cpp)
target_include_directories(pusch_processor_unittest PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
target_link_libraries(pusch_processor_unittest ocudu_channel_processors ocudulog GTest::gtest GTest::gtest_main)
add_test(pusch_processor_unittest pusch_processor_unittest)

add_executable(pusch_demodulator_deferred_chain_test pusch_demodulator_deferred_chain_test.cpp)
target_include_directories(pusch_demodulator_deferred_chain_test PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_processors/pusch
        ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_processors/metal
        ${CMAKE_SOURCE_DIR}/lib/phy/upper/channel_modulation/metal)
target_link_libraries(pusch_demodulator_deferred_chain_test
        ocudu_channel_equalizer
        ocudu_channel_modulation
        ocudu_channel_processors
        ocudu_phy_support
        ocudu_sequence_generators
        ocudu_transform_precoding
        ocuduvec
        ocudulog
        GTest::gtest
        GTest::gtest_main)
# The Metal back ends and their tests only exist in an Apple Silicon build; without them the test
# keeps running against the CPU back ends (see the OCUDU_HAS_METAL_PUSCH_CHAIN guards in the file).
if(ENABLE_METAL_EQUALIZER AND ENABLE_METAL_DEMODULATION)
    target_link_libraries(pusch_demodulator_deferred_chain_test
            ocudu_channel_processors_metal
            ocudu_channel_modulation_metal)
endif()
add_test(pusch_demodulator_deferred_chain_test pusch_demodulator_deferred_chain_test)
