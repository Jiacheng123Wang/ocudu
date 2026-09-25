# Build wiring for the Metal channel estimators. The design records are the Chinese documents the
# port kept as it went: doc_chinese/metal_ce/Metal_MMSE_Channel_Estimator_PLAN.md (this estimator)
# and doc_chinese/ai_ce/ (the AI/HELENA estimator); doc_chinese/phy_pipeline_gpu/ is the lane.
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
    SOURCES ocudu_mmse_inv.metal ocudu_mmse_apply.metal ocudu_mmse_weights.metal ocudu_mmse_rsrp.metal ocudu_mmse_ta.metal
            ocudu_mmse_weights_matrix.metal ocudu_mmse_apply_matrix.metal ocudu_mmse_reformat.metal
            ocudu_mmse_corr.metal ocudu_mmse_pilots.metal ocudu_mmse_pilots_power.metal
            ocudu_mmse_apply_lse.metal
    # The correlation kernels are the one place with a BIT-EXACTNESS contract against a host float
    # expression (build_correlation_matrices()): K0-d is only a drop-in for the host staging if A and
    # R_hp come out identical, and a 1-ulp difference is amplified by cond_2(A) ~ 2e4 into ~1% of W
    # and h (measured 0.4-0.9% on a 256QAM capture, which the LLR byte comparison sees). Metal's
    # default fast math is free to contract `1 + x * x` into an fma and to reassociate the products,
    # and that is exactly the 1 ulp - so this file is compiled with strict IEEE semantics.
    #
    # ocudu_mmse_pilots_power.metal (K0-a) is strict for the same reason, and it is a file of its own
    # to keep the strictness to ONE kernel (S-7g-20): its sigma2_rel = out[0] / out[1] is the diagonal
    # loading of A, so it carries the same cond_2(A) ~ 2e4 amplification as the correlation kernels -
    # and under the default fast math that divide comes out DIFFERENTLY ROUNDED from the host's in
    # 28.4% of 2^20 random (sigma2-like, power-like) pairs (298151 differences, both directions),
    # which flipped LLR decisions on 27 of 27 corpus captures (32613 soft bits). Recompiled with
    # -fno-fast-math the same sweep is 0 of 2^20: on this GPU the flag is the whole difference, not
    # the hardware. The flag is per FILE, and ocudu_mmse_pilots.metal's other kernels have no such
    # contract (the pilots enter h = W . y linearly) - putting them under it as well changed the LSE's
    # last bits for no benefit, so the mean-power kernel was split out instead.
    #   - ocudu_mmse_apply_lse.metal (K2 over the LSE, lever C of dev doc 6.61) is strict for a
    #     third reason: its `lse * inv_beta` product must be ROUNDED where the pilot scatter rounded
    #     it, i.e. before the weight multiply. Under fast math the compiler reassociated the chain into
    #     round(round(w * inv_beta) * lse), which is a different float - measured: _h.bin, _llr.bin and
    #     the grid stayed byte-identical over the 27-capture corpus while the float32 reductions over
    #     h (noise_variance in K4, rsrp in K5) moved on 13 of them (40 bytes of _ce.txt). It is a file
    #     of its own so that ocudu_mmse_apply.metal keeps the default fast math and mmse_apply()'s code
    #     generation stays untouched by this route.
    IEEE_MATH_SOURCES ocudu_mmse_corr.metal ocudu_mmse_pilots_power.metal ocudu_mmse_apply_lse.metal)

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
    # The batch 5b time-alignment port encodes the DFT kernel into the estimator's own command
    # buffer, so the estimator loads that metallib too (see load_ta_pipelines()).
    OCUDU_DFT_METALLIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../../../generic_functions/metal/ocudu_dft.metallib"
    OCUDU_HELENA_MODEL_PATH="${HELENA_MODELC}"
    OCUDU_HELENA_MODEL_PATH_52="${HELENA_MODELC_52}"
    OCUDU_HELENA_MODEL_PATH_106="${HELENA_MODELC_106}")
add_dependencies(ocudu_channel_estimator_metal ocudu_mmse_metallib ocudu_metallib_dft)
target_link_libraries(ocudu_channel_estimator_metal PUBLIC ocuduvec ocudu_phy_support ocudulog ocudu_metal_queue)
target_link_libraries(ocudu_channel_estimator_metal PRIVATE "-framework Metal" "-framework Foundation" "-framework CoreML")

# ---- Unit-level validation (inversion golden + GPU/CPU seam + NMSE vs the cpu estimator) ----

# ---- ctest registration --------------------------------------------------------------------------
# Apple-only directory (ENABLE_METAL_CHEST), so this cannot affect a Linux build. The binary has its
# own main and returns non-zero on failure: the positional add_test form, not gtest_discover_tests.
# "phy" for the directory keeps "ctest -L phy" meaning the PHY suite on this platform too.
set_directory_properties(PROPERTIES LABELS "phy")

add_test(port_channel_estimator_metal_mmse_unit_test port_channel_estimator_metal_mmse_unit_test)
# The fused time-alignment chain (batch 5d: placement + IDFT + profile + peak in ONE dispatch) and the
# per-size IDFT profile checks are gated behind OCUDU_CE_TA_CHAIN, because they are the deepest GPU
# tests in this binary and were originally run one kernel at a time while K7 was being made safe. The
# default run covers the rest (including the exhaustive (cp, scs) epoch sweep, which is unconditional);
# this second entry is what the hand gate used to be, so "make test" now covers it too.
add_test(port_channel_estimator_metal_mmse_unit_test_ta_chain port_channel_estimator_metal_mmse_unit_test)
set_tests_properties(port_channel_estimator_metal_mmse_unit_test_ta_chain PROPERTIES
    ENVIRONMENT "OCUDU_CE_TA_CHAIN=1")
add_executable(port_channel_estimator_metal_mmse_unit_test test/port_channel_estimator_metal_mmse_unit_test.cpp)
target_link_libraries(port_channel_estimator_metal_mmse_unit_test PRIVATE ocudu_channel_estimator_metal ocudu_channel_estimator ocudu_phy_support ocuduvec ocudu_dft)

# ---- G2 head-to-head: metal_mmse / cpu / HELENA vs the AI-CE synthetic test set (.npy) ----
add_executable(helena_head2head_bench test/helena_head2head_bench.cpp)
target_link_libraries(helena_head2head_bench PRIVATE ocudu_channel_estimator_metal ocudu_channel_estimator ocudu_phy_support ocuduvec ocudu_dft)

# ---- Standalone check of the inversion variants against a CPU reference (not in the suite) ----
add_executable(k1_check test/k1_check.cpp)
target_include_directories(k1_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(k1_check PRIVATE ocudu_channel_estimator_metal ocudu_support)
