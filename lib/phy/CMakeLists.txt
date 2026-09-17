# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
# Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.


add_subdirectory(generic_functions)
# Shared Metal queue/burst infrastructure (Apple Silicon only, and only when at least one Metal
# offload is compiled in). It is Objective-C++, so on every other platform the directory must not
# be entered at all: its enable_language(OBJCXX) would otherwise make the configure step require an
# Objective-C++ compiler that Linux/GCC does not ship by default, even though no Metal target is
# being built. Every other Metal directory in the tree is already gated on its ENABLE_METAL_* option.
if(METAL_OFFLOADS_ENABLED)
    add_subdirectory(metal)
endif()
add_subdirectory(lower)
add_subdirectory(metrics)
add_subdirectory(support)
add_subdirectory(upper)
