// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU LDPC decoder (ocudu ldpc_decoder interface) running the
/// layered normalized min-sum algorithm with GPU-internal early termination.

#pragma once

#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_decoder.h"
#include "ocudu_metal_decoder_engine.h"

#include <map>
#include <memory>
#include <mutex>

namespace ocudu {

/// \brief Layered-NMS LDPC decoder running on the Apple GPU.
///
/// One instance serves any (base graph, lifting size) combination; a GPU engine with the
/// packed H matrix and the CSR edge layout is created lazily per combination and cached
/// for the lifetime of the decoder.
class ldpc_decoder_metal : public ldpc_decoder
{
public:
  /// \param[in] force_decoding      Force decoding even if the codeblock appears too short.
  /// \param[in] early_stop_syndrome Early stop on syndrome convergence (no-CRC path only).
  /// \param[in] factor_override     Normalization factor override (-1 = default 0.7).
  /// \param[in] beta_override       Offset min-sum parameter override (-1 = default 0.5).
  /// \param[in] enable_et           GPU-internal early termination (false for A/B).
  ldpc_decoder_metal(bool force_decoding, bool early_stop_syndrome, float factor_override = -1.0F,
                     float beta_override = -1.0F, bool enable_et = true);

  // Out-of-line: the engine slots are defined in the implementation file only.
  ~ldpc_decoder_metal() override;

  // See interface for documentation.
  std::optional<unsigned> decode(bit_buffer&                    output,
                                 span<const log_likelihood_ratio> input,
                                 crc_calculator*                crc,
                                 const configuration&           cfg) override;

  /// GPU-side duration of the last decode in microseconds (0 when unavailable).
  double last_gpu_wait_us() const { return last_gpu_wait_us_; }

private:
  /// Per-(base graph, lifting size) GPU engine and its host-side buffers.
  struct engine_slot;

  engine_slot& get_slot(ldpc_base_graph_type bg, ldpc::lifting_size_t ls);

  bool force_decoding;
  bool early_stop_syndrome;
  float factor_override;
  float beta_override;
  bool enable_et;

  /// GPU-side duration of the last decode (set by decode(); see last_gpu_wait_us()).
  double last_gpu_wait_us_ = 0.0;

  /// Serializes decode(): the engine slots (lazy map insertion), the per-slot
  /// scratch buffers and the zero-copy wrappers are all single-client state.
  /// The gNB runtime already guarantees exclusivity per instance through the
  /// codeblock-decoder pool; this is defense-in-depth for shared-instance paths.
  std::mutex decode_mtx;

  /// Key: (base graph index, lifting size). Owns the engine slots.
  std::map<std::pair<unsigned, unsigned>, std::unique_ptr<engine_slot>> slots;
};

} // namespace ocudu
