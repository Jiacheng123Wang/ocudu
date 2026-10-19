# Build wiring for the Metal GPU soft demapper (GPU-PHY S1b part 2).

include(ocudu_metal)

# ---- Shader library: the LANE's, shared with the estimator and the equalizer (metal_kernel_fusion M3) ----
#
# The demapper used to build `ocudu_demod.metallib`; it now loads `ocudu_lane.metallib` (defined in
# lib/phy/metal, which the build enters first - see the note there). Same kernels, one library.
include(ocudu_metal)

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
    OCUDU_LANE_METALLIB_PATH="${OCUDU_LANE_METALLIB_PATH}")
add_dependencies(ocudu_channel_modulation_metal ocudu_metallib_lane)
target_link_libraries(ocudu_channel_modulation_metal
    PUBLIC ocudulog ocudu_macos_compat ocudu_support ocudu_metal_queue
    PRIVATE "-framework Metal" "-framework Foundation")

# ---- Unit-level validation: bit-exact A/B vs the CPU generic demapper ----
# ---- ctest registration ------------------------------------------------------------------------
# These binaries are the macOS/Metal half of the PHY test suite: the directories that build them are
# Apple-only (the ENABLE_METAL_* gates), so registering them here cannot affect a Linux build - which
# is exactly why they used to be run by hand and were missing from "make test" altogether.
#
# They are NOT gtest binaries - each carries its own main and returns non-zero on failure - so this
# is the positional add_test(<name> <command>) form, not gtest_discover_tests(). The "phy" label is
# set for the directory so that "ctest -L phy" keeps meaning "the PHY suite" on this platform too.
set_directory_properties(PROPERTIES LABELS "phy")

add_test(demodulation_mapper_metal_unit_test demodulation_mapper_metal_unit_test)

# Apple-only test binaries live under lib/, and lib/ is EXCLUDE_FROM_ALL by upstream's design ("do not build by
# default"), so `cmake --build build` never produced them and `--target test` reported them as "Not Run" -- a
# registered test with no binary (found on macOS 2026-10-20). Each one registers itself here, and the aggregate
# target in the top-level CMakeLists pulls them into ALL on Apple builds only, so Ubuntu is untouched.
set_property(GLOBAL APPEND PROPERTY OCUDU_METAL_TEST_TARGETS demodulation_mapper_metal_unit_test)
add_executable(demodulation_mapper_metal_unit_test test/demodulation_mapper_metal_unit_test.cpp)
target_include_directories(demodulation_mapper_metal_unit_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(demodulation_mapper_metal_unit_test PRIVATE
    ocudu_channel_modulation_metal ocudu_channel_modulation ocudu_support)
