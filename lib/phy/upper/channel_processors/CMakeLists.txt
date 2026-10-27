# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_subdirectory(pdcch)
add_subdirectory(pdsch)
add_subdirectory(prach)
add_subdirectory(pucch)
add_subdirectory(pusch)
add_subdirectory(ssb)
add_subdirectory(uci)

add_library(ocudu_channel_processors INTERFACE)
target_link_libraries(ocudu_channel_processors INTERFACE
        ocudu_channel_coding
        ocudu_channel_modulation
        ocudu_channel_precoder
        ocudu_pdcch_processor
        ocudu_pdsch_processor
        ocudu_phy_support
        ocudu_prach_detector
        ocudu_pucch_demodulator
        ocudu_pucch_detector
        ocudu_pucch_processor
        ocudu_pusch_processor
        ocudu_sequence_generators
        ocudu_signal_processors
        ocudu_ssb_tx_processor
        ocudu_uci_decoder)

# Metal GPU channel equalizer (Apple Silicon): the factory selection in upper_phy_factories
# is gated by the OCUDU_METAL_EQUALIZER compile definition propagated through this target.
if(ENABLE_METAL_EQUALIZER)
    add_subdirectory(metal)
    target_link_libraries(ocudu_channel_processors INTERFACE ocudu_channel_processors_metal)
    target_compile_definitions(ocudu_channel_processors INTERFACE OCUDU_METAL_EQUALIZER)
endif()

# ---- CPU-only build of the offline PUSCH replay (portable: Linux and macOS) ----
#
# The Apple build gets `ul_chain_replay` from the metal/ subdirectory, which is only added when a Metal back end is
# enabled. The replay itself is useful without any of them: its --cpu path is the classical chain over a recorded grid,
# and that is what produces the transport-block bits for the training corpus. Building it here -- outside the
# ENABLE_METAL_* gates, against the portable libraries only -- is what lets a Linux machine dump TBs and replay captures
# (design section 19.628). The Metal flags are refused at run time by the binary itself when the back ends are absent.
add_executable(ul_chain_replay_cpu metal/test/ul_chain_replay.cpp)
target_include_directories(ul_chain_replay_cpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pusch)
target_link_libraries(ul_chain_replay_cpu PRIVATE
    ocudu_pusch_processor ocudu_pusch_demodulator
    ocudu_channel_equalizer ocudu_channel_modulation ocudu_channel_coding
    ocudu_uci_decoder ocudu_upper_phy_support ocudu_dft ocudu_transform_precoding
    ocudu_dmrs_pusch ocudu_sequence_generators ocudu_channel_estimator
    ocudu_lower_phy_modulation
    ocudu_phy_support ocudu_support ocudu_macos_compat)

add_to_exported_libs(ocudu_channel_processors
        ocudu_pdcch_processor
        ocudu_pdsch_processor
        ocudu_prach_detector
        ocudu_pucch_demodulator
        ocudu_pucch_detector
        ocudu_pucch_processor
        ocudu_pusch_demodulator
        ocudu_pusch_processor
        ocudu_ssb_tx_processor
        ocudu_uci_decoder)
