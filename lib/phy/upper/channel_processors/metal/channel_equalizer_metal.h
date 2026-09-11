// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU channel equalizer (ZF and MMSE) behind the channel_equalizer interface.
/// Covers the 1..4 Tx layer topologies (1/2/4/8 Rx ports): the single-layer path replicates
/// the CPU 1 x n combiner including its per-port noise-validity port reduction, and the
/// multi-layer path replicates the CPU Gram-inverse plus matched filter. The per-call staging
/// converts the bf16 grid/channel data to float and copies the results back - the zero-copy
/// boundaries arrive with the chained pipeline work.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/upper/equalization/channel_equalizer.h"
#include <memory>
#include <vector>

namespace ocudu {

class channel_equalizer_metal : public channel_equalizer
{
public:
  /// \param[in] mmse True for the MMSE algorithm (false = ZF).
  explicit channel_equalizer_metal(bool mmse);
  ~channel_equalizer_metal() override;

  channel_equalizer_metal(const channel_equalizer_metal&)            = delete;
  channel_equalizer_metal& operator=(const channel_equalizer_metal&) = delete;

  // See interface for documentation.
  bool is_supported(unsigned nof_ports, unsigned nof_layers) override;

  // See interface for documentation.
  void equalize(span<cf_t>                       eq_symbols,
                span<float>                      eq_noise_vars,
                const re_buffer_reader<cbf16_t>& ch_symbols,
                const ch_est_list&               ch_estimates,
                span<const float>                noise_var_estimates,
                float                            tx_scaling) override;

  /// GPU-side duration of the last call in microseconds (0 when unavailable / invalid).
  double engine_gpu_wait_us() const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace ocudu
