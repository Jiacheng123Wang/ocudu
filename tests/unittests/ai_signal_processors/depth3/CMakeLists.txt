# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# The depth-3 receiver's contract tests.
#
# They are Python because the thing under test is the produce-side contract: how a capture becomes the
# packed plane, how coded bits are scattered into the label plane, and what a shipped bundle declares.
# The receive side reads the same contract from C++, and these pin the producer to it. They need numpy
# only -- no TensorFlow, no Core ML, no radio, no captures -- so they run everywhere the build runs,
# including machines with no Apple silicon.
#
# Each test returns 77 when its dependencies are missing, which CTest records as a skip rather than a
# failure (the SKIP_RETURN_CODE property below).
#
# The heavy siblings stay out of here on purpose: replaying captures through the arm
# (doc_chinese/llr_ai_detection/probe/attach_ab.py) needs a bundle, a capture set and Core ML, and the
# flight analysis needs a radio. The test's own README says where those live.

find_package(Python3 COMPONENTS Interpreter)

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "depth-3 contract tests skipped: no Python3 interpreter")
    return()
endif()

set(DEPTH3_TEST_DIR ${CMAKE_SOURCE_DIR}/lib/phy/upper/ai_signal_processors/depth3/test)

set(DEPTH3_CONTRACT_TESTS
    test_plane_contract          # pack_input/unpack_input round trip, both DM-RS patterns
    test_label_plane             # label scatter/gather/mask, both DM-RS patterns
    test_shipped_bundle_contract # what the default and rollback bundles declare
)

foreach(test_name IN LISTS DEPTH3_CONTRACT_TESTS)
    add_test(NAME ai_signal_processors_depth3_${test_name}
             COMMAND ${Python3_EXECUTABLE} ${DEPTH3_TEST_DIR}/${test_name}.py)
    set_tests_properties(ai_signal_processors_depth3_${test_name} PROPERTIES
                         SKIP_RETURN_CODE 77
                         TIMEOUT 120
                         LABELS "ai_signal_processors;depth3;python")
endforeach()
