// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_mode.h"
#include <cstdlib>

namespace ocudu {

/// \brief Whether a hop the DEVICE could not serve must fail instead of being covered by the host.
///
/// The multi-module `cpu_gpu` pipeline runs both sides and keeps a host route behind every device
/// stage for robustness. The fused lane (`mode=gpu`) claims the opposite: the CPU submits once and
/// waits at the exit, so a stage the device refuses is not "covered", it is the CPU re-entering the
/// loop - which is the defect (user ruling, 2026-09-20, see the design document).
///
/// The consequence implemented by the consumers: in `mode=gpu`, a hop whose device results do not
/// cover it FAILS the transport block (an ERROR and a CRC KO, so the MAC retransmits) instead of
/// being computed on the host. A hop refused by a KNOB is exempt: the A/B arms
/// (`OCUDU_CE_CPU_LS=1`, `OCUDU_CE_DEV_Y=0`, `OCUDU_CE_CORR_DEV=0`, `OCUDU_CE_DEV_TA=0`,
/// `OCUDU_CE_DEV_SIGMA2=0`, `OCUDU_CE_CPU_CE=1`) exist to take the host route, and refusing to serve
/// them would delete the arms rather than enforce the claim (see is_knob_refusal()).
///
/// \note `OCUDU_GPU_STRICT` overrides the decision. It exists because the offline harness
///       (`ul_chain_replay`) deliberately never publishes a pipeline mode, so without the override
///       the fused lane's own policy could not be exercised offline at all - and a policy that
///       cannot be tested is the failure mode this whole line keeps correcting.
inline bool phy_pipeline_strict_enabled()
{
  if (const char* env = std::getenv("OCUDU_GPU_STRICT")) {
    return env[0] != '0';
  }
  return phy_pipeline_mode_registry::is_published() &&
         (phy_pipeline_mode_registry::get() == phy_pipeline_mode::gpu);
}

} // namespace ocudu
