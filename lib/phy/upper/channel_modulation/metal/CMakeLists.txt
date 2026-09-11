# Build wiring for the Metal GPU soft demapper (GPU-PHY S1b part 2).

include(ocudu_metal)

# ---- Shader library: generated in the source tree ----
set(METALLIB_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
ocudu_add_metallib(
    TARGET ocudu_metallib_demod
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_demod.metallib
    SOURCES ocudu_demod.metal)

# ---- Static library: engine driver (ARC) + adapter + factory ----
enable_language(OBJCXX)
add_library(ocudu_channel_modulation_metal STATIC
    ocudu_demod_metal_engine.mm       # engine driver (ARC)
    demodulation_mapper_metal.cpp     # demodulation_mapper adapter
    demodulation_mapper_metal_factory.cpp)
set_source_files_properties(ocudu_demod_metal_engine.mm PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_channel_modulation_metal PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_channel_modulation_metal PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_channel_modulation_metal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ocudu_channel_modulation_metal PRIVATE
    OCUDU_DEMOD_METALLIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_demod.metallib")
add_dependencies(ocudu_channel_modulation_metal ocudu_metallib_demod)
target_link_libraries(ocudu_channel_modulation_metal
    PUBLIC ocudulog ocudu_macos_compat ocudu_support
    PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation: bit-exact A/B vs the CPU generic demapper ----
add_executable(demodulation_mapper_metal_unit_test test/demodulation_mapper_metal_unit_test.cpp)
target_include_directories(demodulation_mapper_metal_unit_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(demodulation_mapper_metal_unit_test PRIVATE
    ocudu_channel_modulation_metal ocudu_channel_modulation ocudu_support)
