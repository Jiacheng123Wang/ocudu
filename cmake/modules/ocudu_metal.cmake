# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# ocudu_add_metallib: compile Metal (.metal) shader sources into a .metallib
# library as a regular part of the build, with the same automatic dependency
# checking and incremental recompilation that C/C++ sources get.
#
#   ocudu_add_metallib(
#       TARGET <custom-target-name>
#       OUTPUT <metallib-output-path>
#       SOURCES <metal-source...>
#       [IEEE_MATH_SOURCES <metal-source...>])
#
# IEEE_MATH_SOURCES names the sources among SOURCES that must be compiled
# with -fno-fast-math. The Metal compiler enables fast math by default, which
# lets it contract and reassociate float expressions; a kernel whose contract is
# to reproduce a HOST float expression bit for bit (ocudu_mmse_corr.metal) needs
# the strict IEEE semantics instead, while the rest of the library keeps the
# (faster) default. Note this is per SOURCE, not per target: the flag changes
# the rounding of those kernels only.
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
    set(multi_value_keywords SOURCES IEEE_MATH_SOURCES)
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
    # Absolute paths of the strict-IEEE sources, to match the loop's ${shader_src_abs}.
    set(OCUDU_METALLIB_IEEE_MATH_SOURCES_ABS "")
    foreach(ieee_src IN LISTS OCUDU_METALLIB_IEEE_MATH_SOURCES)
        get_filename_component(ieee_src_abs ${ieee_src} ABSOLUTE)
        list(APPEND OCUDU_METALLIB_IEEE_MATH_SOURCES_ABS ${ieee_src_abs})
    endforeach()
    foreach(shader_src IN LISTS OCUDU_METALLIB_SOURCES)
        get_filename_component(shader_src_abs ${shader_src} ABSOLUTE)
        get_filename_component(shader_name ${shader_src} NAME_WE)
        set(air_file "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.air")
        set(dep_file "${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.air.d")
        # Strict IEEE float semantics for the sources that have to match a host expression bit for
        # bit: the default fast math would be free to contract/reassociate them (see the header).
        set(math_flag "")
        if(shader_src_abs IN_LIST OCUDU_METALLIB_IEEE_MATH_SOURCES_ABS)
            set(math_flag "-fno-fast-math")
        endif()
        add_custom_command(
            OUTPUT ${air_file}
            COMMAND xcrun -sdk macosx metal ${math_flag} -c ${shader_src_abs} -o ${air_file} -MMD -MF ${dep_file}
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

    # ---- Freshness check -------------------------------------------------
    # A stale .metallib is silently wrong rather than loudly broken: the engines load the library from
    # the path baked in at configure time and look kernels up by name, so a library built from an older
    # revision of a shader returns wrong numbers with no error anywhere. `cmake --build --target test`
    # does not build, which is how a tree reaches that state, so this is a TEST and not a build step:
    # a build step would always pass, because it would have just regenerated the library.
    #
    # What counts as "the shader" is the .metal file AND every user header it includes: the butterflies
    # live in ocudu_dft_butterflies.h, which two metallibs share, so a check that watched only the .metal
    # files would call a library fresh while a header it compiles from had moved on - the same silent
    # wrongness it exists to catch. The build already knows those headers (metal -MMD writes them into
    # the depfiles, see the header of this module), so they are read from there rather than scanned for.
    # Only dependencies inside the source tree are compared: the toolchain's own headers would make an
    # Xcode update fail the check on a library that is not stale with respect to anything we wrote.
    set(freshness_script "${CMAKE_CURRENT_BINARY_DIR}/metallib_freshness_${OCUDU_METALLIB_TARGET}.cmake")
    set(freshness_sources "")
    set(freshness_depfiles "")
    foreach(shader_src IN LISTS OCUDU_METALLIB_SOURCES)
        get_filename_component(shader_src_abs ${shader_src} ABSOLUTE)
        get_filename_component(shader_name ${shader_src} NAME_WE)
        string(APPEND freshness_sources "  \"${shader_src_abs}\"\n")
        string(APPEND freshness_depfiles "  \"${CMAKE_CURRENT_BINARY_DIR}/${shader_name}.air.d\"\n")
    endforeach()
    file(WRITE ${freshness_script}
         "set(metallib \"${OCUDU_METALLIB_OUTPUT}\")\n"
         "set(source_root \"${CMAKE_SOURCE_DIR}\")\n"
         "set(sources\n${freshness_sources})\n"
         "set(depfiles\n${freshness_depfiles})\n")
    file(APPEND ${freshness_script} [==[
if(NOT EXISTS "${metallib}")
  message(FATAL_ERROR
      "metallib freshness: ${metallib} does not exist. Run a full build (cmake --build <build-dir>).")
endif()

# The .metal files themselves, plus every in-tree header the depfiles name (they list the whole
# transitive closure - a shared header included by another header is already in there).
set(checked ${sources})
foreach(depfile IN LISTS depfiles)
  if(EXISTS "${depfile}")
    file(READ "${depfile}" depfile_content)
    string(REPLACE "\\\n" " " depfile_content "${depfile_content}")
    string(FIND "${depfile_content}" ":" colon_at)
    if(colon_at GREATER -1)
      math(EXPR after_colon "${colon_at} + 1")
      string(SUBSTRING "${depfile_content}" ${after_colon} -1 depfile_deps)
      separate_arguments(depfile_deps UNIX_COMMAND "${depfile_deps}")
      foreach(dep IN LISTS depfile_deps)
        string(FIND "${dep}" "${source_root}/" in_tree)
        if((in_tree EQUAL 0) AND (NOT dep IN_LIST checked))
          list(APPEND checked "${dep}")
        endif()
      endforeach()
    endif()
  endif()
endforeach()

foreach(src IN LISTS checked)
  if(NOT EXISTS "${src}")
    message(FATAL_ERROR "metallib freshness: shader source ${src} does not exist.")
  endif()
  if("${src}" IS_NEWER_THAN "${metallib}")
    message(FATAL_ERROR
        "metallib freshness: ${metallib} is older than ${src}, the shader it is built from. The kernels "
        "that load it would run an earlier revision of that shader and return wrong numbers without "
        "reporting anything. Run a full build (cmake --build <build-dir>): the test target does not build.")
  endif()
endforeach()
list(LENGTH checked nof_checked)
message(STATUS "metallib freshness: ${metallib} is newer than all ${nof_checked} of its shader inputs")
]==])

    if(BUILD_TESTING)
        add_test(NAME metallib_freshness_${OCUDU_METALLIB_TARGET}
                 COMMAND ${CMAKE_COMMAND} -P ${freshness_script})
        set_tests_properties(metallib_freshness_${OCUDU_METALLIB_TARGET} PROPERTIES LABELS "phy")
    endif()
endfunction()
