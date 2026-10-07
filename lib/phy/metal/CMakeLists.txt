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

# ---- The LANE's shader library: ONE metallib for the estimator, the equalizer and the demapper ----
#
# WHY IT LIVES HERE (metal_kernel_fusion M3, 2026-10-06). The three engines that make up the
# DFT-grid -> LLR segment used to build a library each (ocudu_mmse / ocudu_equalizer / ocudu_demod)
# and load them separately: three files to keep in sync, three loads at startup, and - the reason it
# is part of that workstream's goal rather than mere tidiness - three places for a kernel to be
# missing while a knob silently falls back to another route. The segment now has one library, built
# from every .metal source of the segment, and every engine loads it.
#
# This directory is the right home for it because it is entered BEFORE lib/phy/upper (see
# lib/phy/CMakeLists.txt), so the three engine targets below in the tree can depend on the target
# and read the path variable it publishes. The sources stay where they are: a shader belongs next to
# the engine that documents it, and this target only decides what travels together.
#
# \note Duplicate file-scope names across the sources are EXPECTED and harmless: `xcrun metal`
#       compiles each source into its own .air (like an object file), so ocudu_lane_fused.metal and
#       ocudu_demod.metal may each define LLR_MAX_F, and the equalizer and the fused kernel may each
#       define eq_max_run_symbols. The metallib links them into one library of entry points, which is
#       what the engines look kernels up in.
#
# \note The per-source strictness is preserved: the correlation / diagonal-loading kernels of the
#       estimator have a bit-exactness contract against a host float expression and are compiled with
#       -fno-fast-math (see ocudu_add_metallib and the estimator's CMakeLists for the measurement).
ocudu_add_metallib(
    TARGET ocudu_metallib_lane
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/ocudu_lane.metallib
    SOURCES
        # Channel estimator (the weights stage of the lane).
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_apply.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_weights.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_rsrp.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_ta.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_weights_matrix.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_apply_matrix.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_reformat.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_corr.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_pilots.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_pilots_power.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_apply_lse.metal
        # Equalizer, and the fused equalizer + demapper of metal_kernel_fusion M1.
        ../upper/channel_processors/metal/ocudu_equalizer.metal
        ../upper/channel_processors/metal/ocudu_lane_fused.metal
        # Soft demapper.
        ../upper/channel_modulation/metal/ocudu_demod.metal
    IEEE_MATH_SOURCES
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_corr.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_pilots_power.metal
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_apply_lse.metal
        # ocudu_mmse_weights.metal BELONGS HERE AND WAS MISSING (metal_kernel_fusion, 2026-10-07).
        #
        # The kernel's own comment states the contract this list implements: "`acc += rp[k] * a_smem[...]`,
        # NOT fma(): the kernel above and the CPU reference both round the multiply and then the add, and
        # the metallib is compiled with -fno-fast-math exactly so the contraction does not happen behind
        # our back ... Writing fma() here would be a deliberate difference in the last bit of every
        # element." That was the claim; this list did not carry it, so mmse_weights and mmse_weights_tile
        # compiled under Metal's default fast math and their bit-exactness with the CPU reference rested
        # on the compiler happening not to contract `acc += rp[k]*a`. Found while planning a CE fusion:
        # a fusion moves kernels into a NEW compilation context, so bits that are an artifact of the
        # present one cannot be assumed to survive - the claim has to be enforced by the build, not by
        # the compiler's mood.
        ../upper/signal_processors/channel_estimator/metal/ocudu_mmse_weights.metal)
# The three engine CMakeLists read this instead of naming the path themselves (the directory is
# entered before theirs, so it is set by the time they run).
set(OCUDU_LANE_METALLIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/ocudu_lane.metallib" CACHE INTERNAL
    "Path of the lane's single Metal shader library (metal_kernel_fusion M3)")

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
    # ocudu_support: the PACED COMMIT GATE (paced_task_executor, plan doc 11.26) - the gate is a task executor
    # with a grid tick, and it is created here because this file owns the lane's commit.
    PRIVATE ocudu_support
    PRIVATE "-framework Metal" "-framework Foundation")
