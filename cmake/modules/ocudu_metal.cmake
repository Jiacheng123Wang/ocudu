# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# ocudu_add_metallib: compile Metal (.metal) shader sources into a .metallib
# library as a regular part of the build, with the same automatic dependency
# checking and incremental recompilation that C/C++ sources get.
#
#   ocudu_add_metallib(
#       TARGET <custom-target-name>
#       OUTPUT <metallib-output-path>
#       SOURCES <metal-source...>)
#
# On Apple platforms every .metal source is compiled with `xcrun metal` into a
# .air file, the .air files are linked with `xcrun metallib` into the OUTPUT
# .metallib, and an ALL custom target named <custom-target-name> depends on it
# (so `cmake --build` produces it by default). CMake tracks the shader sources
# through DEPENDS and, through DEPFILE (`metal -MMD -MF`), every user header
# they #include, so only shaders whose inputs changed are recompiled: the
# .metallib is regenerated only when a .metal file or an included user header
# is newer than the output.
#
# The .air intermediates and the depfiles are written into the current binary
# directory; the final .metallib goes wherever OUTPUT says. In ocudu the
# callers point OUTPUT at the source-tree location the runtime engines load
# (the absolute paths baked into them at configure time), so the runtime code
# stays unchanged and simply picks up the freshly generated library.
#
# On non-Apple platforms the function only creates an empty custom target of
# the same name (so add_dependencies() calls keep working); no shader is built.

function(ocudu_add_metallib)
    set(one_value_keywords TARGET OUTPUT)
    set(multi_value_keywords SOURCES)
    cmake_parse_arguments(OCUDU_METALLIB "" "${one_value_keywords}" "${multi_value_keywords}" ${ARGN})

    if(NOT OCUDU_METALLIB_TARGET OR NOT OCUDU_METALLIB_OUTPUT OR NOT OCUDU_METALLIB_SOURCES)
        message(FATAL_ERROR "ocudu_add_metallib: TARGET, OUTPUT and SOURCES are required")
    endif()

    if(NOT APPLE)
        message(STATUS "Skipping Metal shader library '${OCUDU_METALLIB_TARGET}' (Apple platforms only)")
        add_custom_target(${OCUDU_METALLIB_TARGET})
        return()
    endif()

    get_filename_component(metallib_dir ${OCUDU_METALLIB_OUTPUT} DIRECTORY)
    get_filename_component(metallib_name ${OCUDU_METALLIB_OUTPUT} NAME)

    set(air_files "")
    foreach(shader_src IN LISTS OCUDU_METALLIB_SOURCES)
        get_filename_component(shader_src_abs ${shader_src} ABSOLUTE)
        get_filename_component(shader_name ${shader_src} NAME_WE)
        set(air_file "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.air")
        set(dep_file "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.air.d")
        add_custom_command(
            OUTPUT ${air_file}
            COMMAND xcrun -sdk macosx metal -c ${shader_src_abs} -o ${air_file} -MMD -MF ${dep_file}
            DEPENDS ${shader_src_abs}
            DEPFILE ${dep_file}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Compiling Metal shader ${shader_name}.metal"
            VERBATIM)
        list(APPEND air_files ${air_file})
    endforeach()

    add_custom_command(
        OUTPUT ${OCUDU_METALLIB_OUTPUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${metallib_dir}
        COMMAND xcrun -sdk macosx metallib ${air_files} -o ${OCUDU_METALLIB_OUTPUT}
        DEPENDS ${air_files}
        COMMENT "Linking Metal library ${metallib_name}"
        VERBATIM)

    add_custom_target(${OCUDU_METALLIB_TARGET} ALL DEPENDS ${OCUDU_METALLIB_OUTPUT})
endfunction()
