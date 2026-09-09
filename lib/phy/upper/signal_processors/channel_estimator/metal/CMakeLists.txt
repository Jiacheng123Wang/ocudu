# Build wiring for the Metal channel estimators (see Metal_MMSE_Channel_Estimator_PLAN.md
# and AI_channel_estimation_implementation_plan.md in this directory).
#
# The library is only built when ENABLE_METAL_CHEST is ON (default ON for Apple Silicon,
# OFF elsewhere - Linux keeps the upstream byte-for-byte). The MSL shaders are compiled
# into ocudu_mmse.metallib as part of the build by ocudu_add_metallib (cmake/modules/
# ocudu_metal.cmake): `xcrun metal` -> `xcrun metallib`, generated IN PLACE at the
# source-tree location the engine loads (the absolute path baked in as
# OCUDU_MMSE_METALLIB_PATH at configure time points here, so the runtime code is
# untouched). CMake tracks the shader sources (DEPENDS) and their user-header includes
# (DEPFILE via `metal -MMD`), so editing a .metal file recompiles the .metallib
# incrementally - no manual xcrun steps and no re-run of cmake needed. Only the
# .air/.air.d intermediates live in the build directory; the generated .metallib is
# git-ignored (*.metallib, see .gitignore). The AI estimator loads the compiled Core ML
# bundle (ai_assets/helena_pusch_ml.mlmodelc) whose absolute source-tree path is baked
# in as OCUDU_HELENA_MODEL_PATH.

enable_language(OBJCXX)

include(ocudu_metal)

# ---- Shader library: generated in the source tree, authoritative for the runtime load ----
set(MMSE_METALLIB ${CMAKE_CURRENT_SOURCE_DIR}/ocudu_mmse.metallib)
ocudu_add_metallib(
    TARGET ocudu_mmse_metallib
    OUTPUT ${MMSE_METALLIB}
    SOURCES ocudu_mmse_inv.metal ocudu_mmse_apply.metal ocudu_mmse_weights.metal
            ocudu_mmse_weights_matrix.metal ocudu_mmse_apply_matrix.metal)

# ---- Compiled Core ML model (AI channel estimator; see the training memo) ----
set(HELENA_MODELC ${CMAKE_CURRENT_SOURCE_DIR}/ai_assets/helena_pusch_ml.mlmodelc)
set(HELENA_MODELC_52 ${CMAKE_CURRENT_SOURCE_DIR}/ai_assets/helena_pusch52_ml.mlmodelc)
set(HELENA_MODELC_106 ${CMAKE_CURRENT_SOURCE_DIR}/ai_assets/helena_pusch106_ml.mlmodelc)

# ---- Static library: MMSE engine + AI (Core ML) engine + adapters ----
add_library(ocudu_channel_estimator_metal STATIC
    channel_statistics_estimator.h
    port_channel_estimator_metal_mmse_impl.cpp
    port_channel_estimator_helena_impl.cpp
    ocudu_metal_mmse_engine.mm
    ocudu_coreml_nn_engine.mm)
set_source_files_properties(ocudu_metal_mmse_engine.mm ocudu_coreml_nn_engine.mm
    PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_channel_estimator_metal PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_channel_estimator_metal PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_channel_estimator_metal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..)   # port_channel_estimator_average_impl.h
target_compile_definitions(ocudu_channel_estimator_metal PUBLIC
    OCUDU_METAL_CHEST
    OCUDU_MMSE_METALLIB_PATH="${MMSE_METALLIB}"
    OCUDU_HELENA_MODEL_PATH="${HELENA_MODELC}"
    OCUDU_HELENA_MODEL_PATH_52="${HELENA_MODELC_52}"
    OCUDU_HELENA_MODEL_PATH_106="${HELENA_MODELC_106}")
add_dependencies(ocudu_channel_estimator_metal ocudu_mmse_metallib)
target_link_libraries(ocudu_channel_estimator_metal PUBLIC ocuduvec ocudu_phy_support ocudulog)
target_link_libraries(ocudu_channel_estimator_metal PRIVATE "-framework Metal" "-framework Foundation" "-framework CoreML")

# ---- Unit-level validation (inversion golden + GPU/CPU seam + NMSE vs the cpu estimator) ----
add_executable(port_channel_estimator_metal_mmse_unit_test test/port_channel_estimator_metal_mmse_unit_test.cpp)
target_link_libraries(port_channel_estimator_metal_mmse_unit_test PRIVATE ocudu_channel_estimator_metal ocudu_channel_estimator ocudu_phy_support ocuduvec ocudu_dft)

# ---- G2 head-to-head: metal_mmse / cpu / HELENA vs the AI-CE synthetic test set (.npy) ----
add_executable(helena_head2head_bench test/helena_head2head_bench.cpp)
target_link_libraries(helena_head2head_bench PRIVATE ocudu_channel_estimator_metal ocudu_channel_estimator ocudu_phy_support ocuduvec ocudu_dft)
