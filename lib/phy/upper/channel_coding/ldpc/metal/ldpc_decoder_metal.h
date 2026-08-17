// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU LDPC decoder (ocudu ldpc_decoder interface).

#pragma once

#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_decoder.h"
#include "ocudu_metal_decoder_engine.h"

#include <map>
#include <memory>

namespace ocudu {

/// \brief LDPC decoder running on the Apple GPU (SynchroPlus LLS kernels).
///
/// One instance serves any (base graph, lifting size) combination; a GPU engine with the
/// packed H and H^T matrices is created lazily per combination and cached for the lifetime
/// of the decoder.
class ldpc_decoder_metal : public ldpc_decoder
{
public:
  /// \param[in] force_decoding      Force decoding even if the codeblock appears too short.
  /// \param[in] early_stop_syndrome Early stop on syndrome convergence (no-CRC path only).
  /// \param[in] mode                GPU algorithm (LLS heuristic or normalized min-sum).
  /// \param[in] factor_override     NMS normalization factor override (-1 = per-mode default).
  /// \param[in] sat_override        Soft-bit saturation magnitude override (-1 = 0, disabled).
  /// \param[in] enable_et           nms_layered only: GPU-internal early termination (false for A/B).
  ldpc_decoder_metal(bool force_decoding, bool early_stop_syndrome,
                     metal::decoder_engine::algo mode = metal::decoder_engine::algo::lls,
                     float factor_override = -1.0F, float sat_override = -1.0F,
                     bool enable_et = true);

  // Out-of-line: the engine slots are defined in the implementation file only.
  ~ldpc_decoder_metal() override;

  // See interface for documentation.
  std::optional<unsigned> decode(bit_buffer&                    output,
                                 span<const log_likelihood_ratio> input,
                                 crc_calculator*                crc,
                                 const configuration&           cfg) override;

private:
  /// Per-(base graph, lifting size) GPU engine and its host-side buffers.
  struct engine_slot;

  engine_slot& get_slot(ldpc_base_graph_type bg, ldpc::lifting_size_t ls);

  bool force_decoding;
  bool early_stop_syndrome;
  metal::decoder_engine::algo mode;
  float factor_override;
  float sat_override;
  bool enable_et;

  /// Key: (base graph index, lifting size). Owns the engine slots.
  std::map<std::pair<unsigned, unsigned>, std::unique_ptr<engine_slot>> slots;
};

} // namespace ocudu
