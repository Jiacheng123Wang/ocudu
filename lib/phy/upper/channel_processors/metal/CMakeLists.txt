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
    PUBLIC ocudulog ocudu_macos_compat ocudu_support ocudu_metal_queue
    PRIVATE "-framework Metal" "-framework Foundation")

# ---- Dispatch-pattern probe (S2 design input, not part of the test suite) ----
add_executable(metal_dispatch_probe test/metal_dispatch_probe.cpp)
target_include_directories(metal_dispatch_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../equalization)
target_link_libraries(metal_dispatch_probe PRIVATE ocudu_channel_processors_metal ocudu_channel_equalizer ocudu_support)

# ---- Deferred-chain cost probe (A-2 route 1 design input, not part of the test suite) ----
add_executable(metal_chain_probe test/metal_chain_probe.cpp)
target_include_directories(metal_chain_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../equalization)
target_link_libraries(metal_chain_probe PRIVATE
    ocudu_channel_processors_metal ocudu_channel_modulation_metal ocudu_channel_equalizer ocudu_support)

# ---- Unit-level validation: A/B vs the CPU generic equalizer ----
add_executable(channel_equalizer_metal_unit_test test/channel_equalizer_metal_unit_test.cpp)
target_include_directories(channel_equalizer_metal_unit_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../equalization)
target_link_libraries(channel_equalizer_metal_unit_test PRIVATE
    ocudu_channel_processors_metal ocudu_channel_equalizer ocudu_support)

# ---- Offline replay of a recorded PUSCH reception (A/B/C of one test vector) ----
add_executable(ul_chain_replay test/ul_chain_replay.cpp)
target_include_directories(ul_chain_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pusch)
target_link_libraries(ul_chain_replay PRIVATE
    ocudu_pusch_processor ocudu_pusch_demodulator
    ocudu_channel_processors_metal ocudu_channel_modulation_metal
    ocudu_channel_equalizer ocudu_channel_modulation ocudu_channel_coding
    ocudu_uci_decoder ocudu_upper_phy_support ocudu_dft ocudu_transform_precoding
    ocudu_dmrs_pusch ocudu_sequence_generators ocudu_channel_estimator
    ocudu_lower_phy_modulation
    ocudu_lower_phy_modulation
    ocudu_phy_support ocudu_support ocudu_macos_compat)

# AddressSanitizer twin of the replay tool: it pinpoints a wild write in the receiver with a real
# stack, which the plain build cannot show (the crash corrupts it).
add_executable(ul_chain_replay_asan test/ul_chain_replay.cpp)
target_include_directories(ul_chain_replay_asan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pusch)
target_compile_options(ul_chain_replay_asan PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
target_link_options(ul_chain_replay_asan PRIVATE -fsanitize=address)
target_link_libraries(ul_chain_replay_asan PRIVATE
    ocudu_pusch_processor ocudu_pusch_demodulator
    ocudu_channel_processors_metal ocudu_channel_modulation_metal
    ocudu_channel_equalizer ocudu_channel_modulation ocudu_channel_coding
    ocudu_uci_decoder ocudu_upper_phy_support ocudu_dft ocudu_transform_precoding
    ocudu_dmrs_pusch ocudu_sequence_generators ocudu_channel_estimator
    ocudu_lower_phy_modulation
    ocudu_lower_phy_modulation
    ocudu_phy_support ocudu_support ocudu_macos_compat)
