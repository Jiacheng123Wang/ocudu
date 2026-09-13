// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "du_low_phy_pipeline.h"

// Availability of the Metal offload backends, routed here at the CMake level (see this directory's CMakeLists.txt) so
// that no platform macro leaks into the resolver or its callers. This is the only translation unit that sees them:
// every other TU links the function below.
#ifndef OCUDU_METAL_DFT_AVAILABLE
#define OCUDU_METAL_DFT_AVAILABLE 0
#endif
#ifndef OCUDU_METAL_CHEST_AVAILABLE
#define OCUDU_METAL_CHEST_AVAILABLE 0
#endif
#ifndef OCUDU_METAL_EQUALIZER_AVAILABLE
#define OCUDU_METAL_EQUALIZER_AVAILABLE 0
#endif
#ifndef OCUDU_METAL_DEMODULATION_AVAILABLE
#define OCUDU_METAL_DEMODULATION_AVAILABLE 0
#endif
#ifndef OCUDU_METAL_LDPC_AVAILABLE
#define OCUDU_METAL_LDPC_AVAILABLE 0
#endif

using namespace ocudu;

phy_backend_availability ocudu::query_phy_backend_availability()
{
  return phy_backend_availability{OCUDU_METAL_DFT_AVAILABLE != 0,
                                  OCUDU_METAL_CHEST_AVAILABLE != 0,
                                  OCUDU_METAL_EQUALIZER_AVAILABLE != 0,
                                  OCUDU_METAL_DEMODULATION_AVAILABLE != 0,
                                  OCUDU_METAL_LDPC_AVAILABLE != 0};
}
