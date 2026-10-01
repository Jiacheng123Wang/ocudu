# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


set_directory_properties(PROPERTIES LABELS "phy")

add_subdirectory(precoding)

add_executable(dft_processor_test dft_processor_test.cpp)
# The private generic_functions dir is needed because the Apple vDSP case asserts the accelerator's SIZE
# CONTRACT (f * 2^n, f in {1,3,5,15}) against dft_processor_vdsp::is_supported_size, i.e. the rule the
# transparent fallback hangs on. Everywhere else the public factory header is enough.
target_include_directories(dft_processor_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/unittests
    ${CMAKE_SOURCE_DIR}/lib/phy/generic_functions)
target_link_libraries(dft_processor_test ocudulog ocudu_dft GTest::gtest GTest::gtest_main)
add_test(dft_processor_test dft_processor_test)

if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")
    add_executable(dft_processor_ci16_test dft_processor_ci16_test.cpp)
    target_include_directories(dft_processor_ci16_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/unittests)
    target_link_libraries(dft_processor_ci16_test ocudulog ocudu_dft GTest::gtest GTest::gtest_main)
    add_test(dft_processor_ci16_test dft_processor_ci16_test)
elseif (APPLE)
    # The ci16 DFT processor is an x86_64-only implementation, so the test cannot be built on Apple Silicon.
    # Register the ctest case anyway and mark it DISABLED, so that "make test" reports it as
    # "Not Run (Disabled)" and the total test count matches the Linux/x86_64 reference run.
    add_test(NAME dft_processor_ci16_test COMMAND ${CMAKE_COMMAND} -E true)
    set_tests_properties(dft_processor_ci16_test PROPERTIES DISABLED TRUE LABELS "phy;macos_unsupported")
endif (${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64")

if ((ENABLE_MKL AND MKL_FOUND) OR (ENABLE_ARMPL AND ARMPL_FOUND) OR (ENABLE_FFTW AND FFTW3F_FOUND))
    target_compile_definitions(dft_processor_test PRIVATE HAVE_FFTW)
endif ((ENABLE_MKL AND MKL_FOUND) OR (ENABLE_ARMPL AND ARMPL_FOUND) OR (ENABLE_FFTW AND FFTW3F_FOUND))

if (ENABLE_FFTZ AND FFTZ_FOUND)
    target_compile_definitions(dft_processor_test PRIVATE HAVE_FFTZ)
endif (ENABLE_FFTZ AND FFTZ_FOUND)
