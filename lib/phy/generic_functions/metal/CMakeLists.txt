# Build wiring for the Metal GPU DFT (FFT) processor (GPU-PHY S1 operator).
#
# The .metal shader is compiled into a .metallib library as part of the build by
# ocudu_add_metallib (cmake/modules/ocudu_metal.cmake): `xcrun metal` -> `xcrun metallib`,
# generated IN PLACE at the source-tree location the runtime engine loads (the absolute
# path baked into the engine at configure time points here). Only the .air/.air.d
# intermediates live in the build directory; the generated .metallib is git-ignored.

include(ocudu_metal)

# ---- Shader library: generated in the source tree ----
set(METALLIB_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
ocudu_add_metallib(
    TARGET ocudu_metallib_dft
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_dft.metallib
    SOURCES ocudu_dft.metal)

# ---- Static library: engine driver (ARC) + dft_processor adapter ----
enable_language(OBJCXX)
add_library(ocudu_dft_metal STATIC
    ocudu_dft_metal_engine.mm       # engine driver (ARC)
    dft_processor_metal.cpp)        # ocudu dft_processor adapter
set_source_files_properties(ocudu_dft_metal_engine.mm PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_dft_metal PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_dft_metal PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_dft_metal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Absolute .metallib path baked in at configure time; at runtime the engine also falls
# back to a copy next to the executable or in the working directory.
target_compile_definitions(ocudu_dft_metal PRIVATE
    OCUDU_DFT_METALLIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_dft.metallib")
# Ensure the shader is (re)built whenever the engine library is built.
add_dependencies(ocudu_dft_metal ocudu_metallib_dft)
target_link_libraries(ocudu_dft_metal PUBLIC ocudulog ocudu_macos_compat)
target_link_libraries(ocudu_dft_metal PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation: random-IQ A/B vs the CPU reference (NMSE gate) ----
add_executable(dft_processor_metal_unit_test test/dft_processor_metal_unit_test.cpp)
target_link_libraries(dft_processor_metal_unit_test PRIVATE ocudu_dft ocudu_dft_metal ocudu_support)

# ---- Slot-level A/B: batched Metal DFT inside the OFDM demodulator ----
add_executable(ofdm_demodulator_metal_batch_test test/ofdm_demodulator_metal_batch_test.cpp)
target_link_libraries(ofdm_demodulator_metal_batch_test PRIVATE
    ocudu_dft ocudu_dft_metal ocudu_lower_phy_modulation ocudu_phy_support ocudu_support)
