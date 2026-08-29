# Build wiring for the Metal MMSE channel estimator (see PLAN.md in this directory).
#
# The library is only built when ENABLE_METAL_CHEST is ON (default ON for Apple Silicon,
# OFF elsewhere - Linux keeps the upstream byte-for-byte). The MSL shaders are compiled
# into ocudu_mmse.metallib at build time (xcrun metal/metallib) and loaded at runtime via
# newLibraryWithURL; the absolute path is baked in as OCUDU_MMSE_METALLIB_PATH.

enable_language(OBJCXX)

# ---- Shader library (LDPC-style: pre-compiled .metallib lives in the source tree) ----
# After editing a .metal file, re-compile the .metallib (and re-run cmake so the baked-in
# absolute path stays current):
#   xcrun -sdk macosx metal -c ocudu_mmse_inv.metal -o /tmp/ocudu_mmse_inv.air
#   xcrun -sdk macosx metal -c ocudu_mmse_apply.metal -o /tmp/ocudu_mmse_apply.air
#   xcrun -sdk macosx metal -c ocudu_mmse_weights.metal -o /tmp/ocudu_mmse_weights.air
#   xcrun -sdk macosx metallib /tmp/ocudu_mmse_{inv,apply,weights}.air -o ocudu_mmse.metallib
set(MMSE_METALLIB ${CMAKE_CURRENT_SOURCE_DIR}/ocudu_mmse.metallib)

# ---- Static library: engine driver + 2D MMSE adapter + statistics provider ----
add_library(ocudu_channel_estimator_metal STATIC
    channel_statistics_estimator.h
    port_channel_estimator_metal_mmse_impl.cpp
    ocudu_metal_mmse_engine.mm)
set_source_files_properties(ocudu_metal_mmse_engine.mm PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_channel_estimator_metal PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_channel_estimator_metal PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_channel_estimator_metal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..)   # port_channel_estimator_average_impl.h
target_compile_definitions(ocudu_channel_estimator_metal PUBLIC
    OCUDU_METAL_CHEST
    OCUDU_MMSE_METALLIB_PATH="${MMSE_METALLIB}")
add_dependencies(ocudu_channel_estimator_metal ocudu_mmse_metallib)
target_link_libraries(ocudu_channel_estimator_metal PUBLIC ocuduvec ocudu_phy_support)
target_link_libraries(ocudu_channel_estimator_metal PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation (inversion golden + GPU/CPU seam + NMSE vs the cpu estimator) ----
add_executable(port_channel_estimator_metal_mmse_unit_test test/port_channel_estimator_metal_mmse_unit_test.cpp)
target_link_libraries(port_channel_estimator_metal_mmse_unit_test PRIVATE ocudu_channel_estimator_metal ocudu_channel_estimator ocudu_phy_support ocuduvec ocudu_dft)
