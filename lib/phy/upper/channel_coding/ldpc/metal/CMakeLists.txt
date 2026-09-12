# Build wiring for the Metal GPU LDPC decoder (layered normalized min-sum, see PLAN.md).
#
# The .metal shaders are compiled into .metallib libraries as part of the build by
# ocudu_add_metallib (cmake/modules/ocudu_metal.cmake): `xcrun metal` -> `xcrun metallib`,
# one .metallib per .metal source, generated IN PLACE at the source-tree location the
# runtime engine loads (the absolute paths baked into the engine at configure time point
# here, so the runtime code is untouched). CMake tracks the shader sources (DEPENDS) and
# their user-header includes (DEPFILE via `metal -MMD`), so editing a .metal file
# recompiles only the affected .metallib - no manual xcrun steps and no re-run of cmake
# needed. Only the .air/.air.d intermediates live in the build directory; the generated
# .metallib files are git-ignored (*.metallib, see .gitignore).

include(ocudu_metal)

# ---- Shader libraries: one .metallib per .metal source, generated in the source tree ----
set(METALLIB_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
ocudu_add_metallib(
    TARGET ocudu_metallib_layered
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_nms_layered_decoder.metallib
    SOURCES ocudu_nms_layered_decoder.metal)
ocudu_add_metallib(
    TARGET ocudu_metallib_flooding
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_nms_flooding_decoder.metallib
    SOURCES ocudu_nms_flooding_decoder.metal)
ocudu_add_metallib(
    TARGET ocudu_metallib_async
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_nms_async_decoder.metallib
    SOURCES ocudu_nms_async_decoder.metal)
ocudu_add_metallib(
    TARGET ocudu_metallib_lls
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_lls_decoder.metallib
    SOURCES ocudu_lls_decoder.metal)
add_custom_target(ocudu_ldpc_metallib ALL)
add_dependencies(ocudu_ldpc_metallib
    ocudu_metallib_layered ocudu_metallib_flooding ocudu_metallib_async ocudu_metallib_lls)

# ---- Static library: engine driver + ldpc_decoder adapter ----
enable_language(OBJCXX)
add_library(ocudu_ldpc_metal STATIC
    ocudu_metal_decoder_engine.mm       # engine driver (ARC)
    ldpc_decoder_metal.cpp)             # ocudu ldpc_decoder adapter
set_source_files_properties(ocudu_metal_decoder_engine.mm PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_ldpc_metal PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_ldpc_metal PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_ldpc_metal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}       # ldpc_decoder_metal.h / ocudu_metal_decoder_engine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/..)   # ldpc_graph_impl.h (H-matrix generation from the protograph)
# Absolute .metallib paths baked in at configure time; at runtime the engine also
# falls back to a copy next to the executable or in the working directory.
target_compile_definitions(ocudu_ldpc_metal PRIVATE
    OCUDU_METAL_LAYERED_LIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_nms_layered_decoder.metallib"
    OCUDU_METAL_FLOODING_LIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_nms_flooding_decoder.metallib"
    OCUDU_METAL_ASYNC_LIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_nms_async_decoder.metallib"
    OCUDU_METAL_LLS_LIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_lls_decoder.metallib")
# Ensure the shaders are (re)built whenever the engine library is built.
add_dependencies(ocudu_ldpc_metal ocudu_ldpc_metallib)
# The adapter generates the packed H matrix from the ocudu ldpc_graph implementation.
target_link_libraries(ocudu_ldpc_metal PUBLIC ocudu_ldpc ocudulog ocudu_metal_queue)
target_link_libraries(ocudu_ldpc_metal PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation binary (golden H check + decode parity vs the CPU decoder) ----
add_executable(ldpc_metal_unit_test test/ldpc_metal_unit_test.cpp)
target_link_libraries(ldpc_metal_unit_test PRIVATE ocudu_channel_coding ocudu_ldpc_metal)

# ---- Pure LDPC BLER benchmark (CPU vs GPU across SNRs and code rates) ----
add_executable(ldpc_metal_bler_test test/ldpc_metal_bler_test.cpp)
target_link_libraries(ldpc_metal_bler_test PRIVATE ocudu_channel_coding ocudu_ldpc_metal)
