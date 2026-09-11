// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Factory for the Metal channel equalizer: creates a composite instance that uses
/// the Metal GPU kernel for the supported multi-layer topologies and transparently falls
/// back to the CPU generic implementation otherwise (e.g. single-layer cells), so a valid
/// configuration is never rejected.

#pragma once

#include "ocudu/phy/upper/equalization/channel_equalizer_algorithm_type.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"
#include <memory>

namespace ocudu {

/// \brief Creates a channel equalizer factory that prefers the Metal GPU implementation
/// (ZF/MMSE, 2..4 Tx layers x 2/4/8 Rx ports) and falls back to the CPU generic
/// implementation per topology.
std::shared_ptr<channel_equalizer_factory>
create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type type);

} // namespace ocudu
