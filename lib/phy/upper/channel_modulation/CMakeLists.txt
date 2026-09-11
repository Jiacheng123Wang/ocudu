# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set(SOURCES
        channel_modulation_factories.cpp
        demodulation_mapper_impl.cpp
        demodulation_mapper_qam16.cpp
        demodulation_mapper_qam64.cpp
        demodulation_mapper_qam256.cpp
        demodulation_mapper_qpsk.cpp
        evm_calculator_generic_impl.cpp
        modulation_mapper_lut_impl.cpp)

if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
    list(APPEND SOURCES modulation_mapper_avx512_impl.cpp)
    set_source_files_properties(modulation_mapper_avx512_impl.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512vbmi;")
    list(APPEND SOURCES modulation_mapper_avx2_impl.cpp)
    set_source_files_properties(modulation_mapper_avx2_impl.cpp PROPERTIES
            COMPILE_OPTIONS "-mavx2;")
endif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64|arm64")
    list(APPEND SOURCES modulation_mapper_neon_impl.cpp)
endif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64|arm64")

add_library(ocudu_channel_modulation STATIC ${SOURCES})
target_link_libraries(ocudu_channel_modulation PRIVATE ocudu_support log_likelihood_ratio)

# Metal GPU soft demapper (Apple Silicon): the factory selection in upper_phy_factories
# is gated by the OCUDU_METAL_DEMODULATION compile definition propagated through this target.
if(ENABLE_METAL_DEMODULATION)
    add_subdirectory(metal)
    target_link_libraries(ocudu_channel_modulation INTERFACE ocudu_channel_modulation_metal)
    target_compile_definitions(ocudu_channel_modulation INTERFACE OCUDU_METAL_DEMODULATION)
endif()

add_to_exported_libs(ocudu_channel_modulation)
