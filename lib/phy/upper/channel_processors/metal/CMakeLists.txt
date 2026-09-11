# Build wiring for the Metal GPU channel equalizer (GPU-PHY S1b).

include(ocudu_metal)

# ---- Shader library: generated in the source tree ----
set(METALLIB_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
ocudu_add_metallib(
    TARGET ocudu_metallib_equalizer
    OUTPUT ${METALLIB_OUT_DIR}/ocudu_equalizer.metallib
    SOURCES ocudu_equalizer.metal)

# ---- Static library: engine driver (ARC) + adapters + factory ----
enable_language(OBJCXX)
add_library(ocudu_channel_processors_metal STATIC
    ocudu_equalizer_metal_engine.mm   # engine driver (ARC)
    channel_equalizer_metal.cpp       # channel_equalizer adapter
    channel_equalizer_metal_factory.cpp)
set_source_files_properties(ocudu_equalizer_metal_engine.mm PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_channel_processors_metal PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_channel_processors_metal PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_channel_processors_metal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ocudu_channel_processors_metal PRIVATE
    OCUDU_EQUALIZER_METALLIB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocudu_equalizer.metallib")
add_dependencies(ocudu_channel_processors_metal ocudu_metallib_equalizer)
target_link_libraries(ocudu_channel_processors_metal
    PUBLIC ocudulog ocudu_macos_compat ocudu_support
    PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation: A/B vs the CPU generic equalizer ----
add_executable(channel_equalizer_metal_unit_test test/channel_equalizer_metal_unit_test.cpp)
target_include_directories(channel_equalizer_metal_unit_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../equalization)
target_link_libraries(channel_equalizer_metal_unit_test PRIVATE
    ocudu_channel_processors_metal ocudu_channel_equalizer ocudu_support)
