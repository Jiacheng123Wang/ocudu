# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set(SOURCES
            du_low_config_cli11_schema.cpp
            du_low_config_translator.cpp
            du_low_config_validator.cpp
            du_low_config_yaml_writer.cpp
            du_low_hal_factory.cpp
            du_low_phy_pipeline.cpp
            o_du_low_unit_factory.cpp)

add_library(ocudu_o_du_low_unit_helpers STATIC ${SOURCES})
target_include_directories(ocudu_o_du_low_unit_helpers PRIVATE ${CMAKE_SOURCE_DIR})
# Availability of the Metal offload backends is routed here, at the CMake level, so the uplink PHY pipeline resolver
# (see du_low_phy_pipeline.h) can select the effective per-module backend without any platform macro (0/1 compile-time
# constants, folded by the compiler).
# The workflow's mixed model is the default because it is the only one that covers every modulation of the A/B
# (256QAM, 64QAM, 16QAM and QPSK) with one set of weights; the per-modulation models stay selectable by path. A
# deployment overrides this cache variable; a build whose file does not exist refuses every "ai" request that does not
# name one, with the path in the message.
set(OCUDU_DEPTH3_DEFAULT_MODEL
    "${CMAKE_SOURCE_DIR}/doc_chinese/llr_ai_detection/work_tmp/probe/d3_mixed.mlmodelc"
    CACHE FILEPATH "Model the PUSCH ai receiver binds when --pusch_receiver_model is not given")

target_compile_definitions(ocudu_o_du_low_unit_helpers PRIVATE
        OCUDU_METAL_DFT_AVAILABLE=$<BOOL:${ENABLE_METAL_DFT}>
        OCUDU_METAL_CHEST_AVAILABLE=$<BOOL:${ENABLE_METAL_CHEST}>
        OCUDU_METAL_EQUALIZER_AVAILABLE=$<BOOL:${ENABLE_METAL_EQUALIZER}>
        OCUDU_METAL_DEMODULATION_AVAILABLE=$<BOOL:${ENABLE_METAL_DEMODULATION}>
        OCUDU_METAL_LDPC_AVAILABLE=$<BOOL:${ENABLE_METAL_LDPC}>
        # The AI depth-3 receiver itself is an object inside the PUSCH processor library, which is built
        # unconditionally; what it needs in order to COMPUTE is the Metal receive stack (its estimator is the Metal
        # one) and a Core ML model at run time. The Metal estimator is therefore the honest capability test: without
        # it the arm could only forward to the classical chain, and the resolver would be right to substitute it.
        OCUDU_DEPTH3_AI_AVAILABLE=$<BOOL:${ENABLE_METAL_CHEST}>
        # The model an "ai" run binds when --pusch_receiver_model is not given. Overridable so a deployment can point
        # the default at its own model without touching the command line; a build whose file does not exist refuses
        # every "ai" request that does not name one, with the path in the message.
        OCUDU_DEPTH3_DEFAULT_MODEL="${OCUDU_DEPTH3_DEFAULT_MODEL}")

set(DU_LOW_UNIT_HELPERS_LIBRARIES ocudu_upper_phy ocudu_cpu_affinities_helper ocudu_o_du_low ocudu_app_helpers)

# Hardware acceleration for both PUSCH and PDSCH is enabled by default when using DPDK.
if (DPDK_FOUND)
   set_source_files_properties(
           du_low_config_cli11_schema.cpp
           du_low_hal_factory.cpp
           PROPERTIES COMPILE_DEFINITIONS "DPDK_FOUND; HWACC_PDSCH_ENABLED; HWACC_PUSCH_ENABLED")
   list(APPEND DU_LOW_UNIT_HELPERS_LIBRARIES ocudu_hal_pusch
        ocudu_hal_pdsch
        ocudu_hal_bbdev)
endif (DPDK_FOUND)

target_link_libraries(ocudu_o_du_low_unit_helpers PUBLIC ${DU_LOW_UNIT_HELPERS_LIBRARIES})
