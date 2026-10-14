# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_library(ocudu_pusch_demodulator STATIC
        pusch_demodulator_impl.cpp
        demodulator_factories.cpp
        ul_capture.cpp)
# The demodulator's zero-copy buffers use the shared page-aligned allocator
# (ocudu/support/page_aligned_allocator.h), which relies on the macos_compat aligned allocation shims
# (link both ocudu_support and ocudu_macos_compat because of their mutual dependency).
# ul_capture.cpp is the debug capture of the PUSCH reception (grid, estimates and the soft bits the
# demodulator produces), so it lives here: the demodulator library uses it, and the processor
# library - which links this one - uses it as well. Keeping it in the processor instead made the two
# libraries depend on each other, which links only by accident of the order on the linker command
# line (and not at all for a consumer that links just the demodulator).
# The demodulator builds the device gather plan of the equalizer's received symbols
# (ch_gather_desc, see pusch_demodulator_impl.h), whose only out-of-line member is defined in
# ocudu_channel_equalizer. Declaring the dependency here is what makes that symbol reach every
# consumer of this library: before, it resolved only where something else in the same link line
# happened to pull the equalizer in, which held on Apple Silicon and failed to link on Ubuntu.
target_link_libraries(ocudu_pusch_demodulator PUBLIC ocudu_support ocudu_macos_compat ocudu_channel_equalizer)

add_library(ocudu_pusch_processor STATIC
        processor_factories.cpp
        pusch_codeblock_decoder.cpp
        pusch_decoder_empty_impl.cpp
        pusch_decoder_hw_impl.cpp
        pusch_decoder_impl.cpp
        pusch_depth3_input_plane.cpp
        pusch_depth3_measurements.cpp
        pusch_depth3_receiver_impl.cpp
        pusch_processor_impl.cpp
        pusch_processor_validator_impl.cpp
        ulsch_demultiplex_impl.cpp)
# ocudu_dmrs_helper generates the DM-RS reference symbols the depth-3 arm's pilot LS is
# measured against; the classical demodulator never needs them because it reads the
# estimator's interpolated result instead of a grid.
# ocudu_dmrs_helper generates the DM-RS reference symbols the depth-3 arm's pilot LS is measured
# against (the classical demodulator never needs them: it reads the estimator's interpolated result
# instead of a grid), and ocudu_sequence_generators builds the pseudo-random generator that seeds
# them.
set(OCUDU_PUSCH_LIBS ocudu_upper_phy_support ocudu_ran ocudu_pusch_demodulator ocudu_dmrs_helper
        ocudu_sequence_generators)

target_link_libraries(ocudu_pusch_processor ${OCUDU_PUSCH_LIBS})

# The depth-3 arm's forward pass runs on the Core ML engine, which only exists where the Metal estimator is
# built. The dependency is a LINK dependency and a compile definition rather than an include in the seam's
# header: nothing above the arm knows which engine computes it, and a build without Core ML gets the identity
# arm, which says so at startup instead of silently forwarding.
if(ENABLE_METAL_CHEST)
    target_link_libraries(ocudu_pusch_processor ocudu_channel_estimator_metal)
    # The engine's header is private to the metal estimator's directory (its PUBLIC include is its parent,
    # for the average estimator's header), and widening that would put a Metal path in front of every
    # consumer of the estimator. The one file that needs it asks for it here.
    target_include_directories(ocudu_pusch_processor PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../signal_processors/channel_estimator/metal)
    target_compile_definitions(ocudu_pusch_processor PRIVATE OCUDU_HAVE_DEPTH3_ENGINE=1)
endif()

# ---- Depth-3 input plane check: compares the assembler against the Python packer's fixture ----
add_executable(pusch_depth3_input_plane_check test/pusch_depth3_input_plane_check.cpp)
target_include_directories(pusch_depth3_input_plane_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pusch_depth3_input_plane_check PRIVATE ocudu_pusch_processor ocudu_phy_support
        ocudu_support)
