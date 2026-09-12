# Shared Metal infrastructure for the PHY GPU chain (GPU-PHY S2).

enable_language(OBJCXX)
add_library(ocudu_metal_queue STATIC
    ocudu_metal_queue.mm        # shared device + command queue
    ocudu_metal_burst.mm)       # shared command buffer of the stages of a deferred burst
set_source_files_properties(ocudu_metal_queue.mm ocudu_metal_burst.mm PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
set_property(TARGET ocudu_metal_queue PROPERTY OBJCXX_STANDARD 17)
target_compile_options(ocudu_metal_queue PRIVATE -Wno-deprecated-declarations)
target_include_directories(ocudu_metal_queue PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ocudu_metal_queue
    PUBLIC ocudulog ocudu_macos_compat
    PRIVATE "-framework Metal" "-framework Foundation")
