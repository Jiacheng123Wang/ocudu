// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_receiver_impl.h"

using namespace ocudu;

std::unique_ptr<pusch_depth3_receiver> ocudu::create_pusch_depth3_receiver(
    pusch_depth3_kind     kind,
    dmrs_pusch_estimator& estimator,
    pusch_demodulator&    demodulator)
{
  switch (kind) {
    case pusch_depth3_kind::classic:
      return std::make_unique<pusch_depth3_receiver_classic>(estimator, demodulator);
    case pusch_depth3_kind::ai_identity:
      return std::make_unique<pusch_depth3_receiver_ai_identity>(estimator, demodulator);
  }
  return nullptr;
}
