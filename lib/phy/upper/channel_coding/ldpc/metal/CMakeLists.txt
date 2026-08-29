# Build wiring for the Metal GPU LDPC decoder (layered normalized min-sum, see PLAN.md).
#
# The shaders are pre-compiled offline with the macOS Metal toolchain into .metallib
# files and loaded at runtime with newLibraryWithURL - no runtime shader compilation.
# After editing a .metal file, re-compile its .metallib (and re-run cmake so the
# baked-in absolute paths stay current):
#   xcrun -sdk macosx metal -c ocudu_nms_layered_decoder.metal -o ocudu_nms_layered_decoder.air
#   xcrun -sdk macosx metallib ocudu_nms_layered_decoder.air -o ocudu_nms_layered_decoder.metallib
#   (same for ocudu_nms_flooding_decoder.metal, ocudu_nms_async_decoder.metal and
#    ocudu_lls_decoder.metal)

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
# The adapter generates the packed H matrix from the ocudu ldpc_graph implementation.
target_link_libraries(ocudu_ldpc_metal PUBLIC ocudu_ldpc ocudulog)
target_link_libraries(ocudu_ldpc_metal PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation binary (golden H check + decode parity vs the CPU decoder) ----
add_executable(ldpc_metal_unit_test test/ldpc_metal_unit_test.cpp)
target_link_libraries(ldpc_metal_unit_test PRIVATE ocudu_channel_coding ocudu_ldpc_metal)

# ---- Pure LDPC BLER benchmark (CPU vs GPU across SNRs and code rates) ----
add_executable(ldpc_metal_bler_test test/ldpc_metal_bler_test.cpp)
target_link_libraries(ldpc_metal_bler_test PRIVATE ocudu_channel_coding ocudu_ldpc_metal)
