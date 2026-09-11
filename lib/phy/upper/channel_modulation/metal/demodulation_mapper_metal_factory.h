// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Composite demodulation mapper factory: Metal for QPSK/16/64/256 QAM, the CPU
/// generic implementation for the remaining schemes (BPSK and pi/2-BPSK).

#pragma once

#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include <memory>

namespace ocudu {

/// Creates a factory of demodulation mappers backed by the Metal GPU implementation with a
/// transparent per-scheme fallback to the CPU generic implementation.
std::shared_ptr<demodulation_mapper_factory> create_demodulation_mapper_metal_factory();

} // namespace ocudu
