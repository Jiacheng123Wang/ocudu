// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
/// \file
/// \brief The 16-bit float the depth-3 plane carries, named once so the shared files compile everywhere.
///
/// WHY THIS EXISTS. The plane is IEEE binary16 because that is what the Core ML input takes and what the ANE
/// path wants. On Apple platforms that type is spelled `__fp16`, which is an ARM/Clang extension: the shared
/// files that mention it do not build on an x86 toolchain at all, and that is exactly what the Linux CI build
/// caught on 2026-10-19 (`‘__fp16’ was not declared in this scope`).
///
/// So the shared files use `depth3_half` and this header decides what it is:
///   * Apple: `__fp16`, unchanged, so the store into the Core ML input buffer keeps its existing codegen;
///   * everywhere else: `_Float16`, the standard type (GCC 12+ on x86-64, and Clang), which is the same
///     binary16 layout.
///
/// The Metal-only translation units keep spelling `__fp16` directly: they are only ever compiled on Apple
/// platforms, and pretending otherwise would suggest the depth-3 arm runs without Core ML.
#pragma once

#if defined(__APPLE__)
using depth3_half = __fp16;
#else
using depth3_half = _Float16;
#endif
