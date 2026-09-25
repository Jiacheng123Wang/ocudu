# Shared Metal infrastructure for the PHY GPU chain (GPU-PHY S2).
#
# This directory is entered only when at least one Metal offload is enabled (see the
# METAL_OFFLOADS_ENABLED gate in lib/phy/CMakeLists.txt). Reaching it anywhere else would send the
# configure step looking for an Objective-C++ compiler (cc1objplus) that is not part of a default
# Linux/GCC install, and the failure would surface as a broken-compiler error rather than as the
# real cause. Say the real cause here.
if(NOT (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64"))
    message(FATAL_ERROR
        "lib/phy/metal (the shared Metal command queue) is Objective-C++ and builds on Apple "
        "Silicon only. This is ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. It must not be "
        "added unless METAL_OFFLOADS_ENABLED is set; see lib/phy/CMakeLists.txt.")
endif()

enable_language(OBJCXX)
add_library(ocudu_metal_queue STATIC
    ocudu_metal_queue.mm        # shared device + command queue
    ocudu_metal_burst.mm        # shared command buffer of the stages of a deferred burst
    ocudu_metal_lane_probe.mm)  # GPU-side lane accounting (residency / busy / gap / period)
set_source_files_properties(ocudu_metal_queue.mm ocudu_metal_burst.mm ocudu_metal_lane_probe.mm
    PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_metal_queue PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_metal_queue PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_metal_queue PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ocudu_metal_queue
    PUBLIC ocudulog ocudu_macos_compat ocudu_phy_support
    PRIVATE "-framework Metal" "-framework Foundation")
