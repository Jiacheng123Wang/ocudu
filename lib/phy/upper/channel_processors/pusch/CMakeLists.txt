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
target_link_libraries(ocudu_pusch_demodulator PUBLIC ocudu_support ocudu_macos_compat)

add_library(ocudu_pusch_processor STATIC
        processor_factories.cpp
        pusch_codeblock_decoder.cpp
        pusch_decoder_empty_impl.cpp
        pusch_decoder_hw_impl.cpp
        pusch_decoder_impl.cpp
        pusch_processor_impl.cpp
        pusch_processor_validator_impl.cpp
        ulsch_demultiplex_impl.cpp)
set(OCUDU_PUSCH_LIBS ocudu_upper_phy_support ocudu_ran ocudu_pusch_demodulator)

target_link_libraries(ocudu_pusch_processor ${OCUDU_PUSCH_LIBS})
