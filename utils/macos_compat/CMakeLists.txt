# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# Cross-platform compat layer (Linux / macOS). Compiled on every platform:
# the platform-specific API mapping (Mach vs POSIX) lives inside the .cpp
# files below, so business code never needs #ifdef __APPLE__.
# The public header lives in include/ocudu/support/macos_compat.h, which is on
# the global include path (see the top-level include_directories(include)).
#
# cpu_architecture_info.cpp lives here too: CPU topology discovery is exactly
# platform-abstraction code (sysfs/NUMA on Linux vs hardware_concurrency on
# macOS), and this target is the home where the Linux implementation can be
# reused by the compat wrappers without a link cycle with ocudu_support.
add_library(ocudu_macos_compat STATIC macos_compat.cpp cpu_architecture_info.cpp)
target_include_directories(ocudu_macos_compat PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ocudu_macos_compat PUBLIC ocudulog)
add_to_exported_libs(ocudu_macos_compat)
