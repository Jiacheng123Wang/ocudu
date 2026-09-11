// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "channel_equalizer_metal.h"
#include "ocudu_equalizer_metal_engine.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/ocuduvec/fill.h"
#include "ocudu/ocuduvec/zero.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace ocudu;

struct channel_equalizer_metal::impl {
  metal::equalizer_metal_engine engine;
  bool                           engine_ok = false;
  bool                           mmse      = false;

  // Staging buffers (page-aligned, grown on demand and reused across calls).
  void* h_buf      = nullptr;
  void* y_buf      = nullptr;
  void* eq_buf     = nullptr;
  void* nv_buf     = nullptr;
  size_t h_cap     = 0; // bytes
  size_t y_cap     = 0;
  size_t eq_cap    = 0;
  size_t nv_cap    = 0;

  ~impl()
  {
    compat::aligned_free(h_buf);
    compat::aligned_free(y_buf);
    compat::aligned_free(eq_buf);
    compat::aligned_free(nv_buf);
  }

  static void* ensure(void*& buf, size_t& cap, size_t needed)
  {
    if (cap >= needed) {
      return buf;
    }
    compat::aligned_free(buf);
    buf = compat::aligned_alloc(compat::page_size(), needed);
    ocudu_assert(buf != nullptr, "Equalizer staging allocation failed.");
    cap = needed;
    return buf;
  }
};

channel_equalizer_metal::channel_equalizer_metal(bool mmse) : impl_(std::make_unique<impl>())
{
  impl_->mmse      = mmse;
  impl_->engine_ok = impl_->engine.init();
}

channel_equalizer_metal::~channel_equalizer_metal() = default;

bool channel_equalizer_metal::is_supported(unsigned nof_ports, unsigned nof_layers)
{
  // Multi-layer topologies only: the single-layer path (per-port noise-validity reduction)
  // keeps the CPU implementation.
  if ((nof_ports != 2) && (nof_ports != 4) && (nof_ports != 8)) {
    return false;
  }
  return (nof_layers >= 2) && (nof_layers <= 4) && (nof_layers <= nof_ports) && impl_->engine_ok;
}

void channel_equalizer_metal::equalize(span<cf_t>                       eq_symbols,
                                       span<float>                      eq_noise_vars,
                                       const re_buffer_reader<cbf16_t>& ch_symbols,
                                       const ch_est_list&               ch_estimates,
                                       span<const float>                noise_var_estimates,
                                       float                            tx_scaling)
{
  const unsigned nof_re       = ch_estimates.get_nof_re();
  const unsigned nof_rx_ports = ch_estimates.get_nof_rx_ports();
  const unsigned nof_layers   = ch_estimates.get_nof_tx_layers();

  ocudu_assert(ch_symbols.get_nof_re() == nof_re, "Invalid channel symbols size.");
  ocudu_assert(ch_symbols.get_nof_slices() == nof_rx_ports, "Invalid channel symbols ports.");
  ocudu_assert(noise_var_estimates.size() == nof_rx_ports, "Invalid noise variance estimates size.");
  ocudu_assert(eq_symbols.size() == nof_re * nof_layers, "Invalid equalized symbols size.");
  ocudu_assert(eq_noise_vars.size() == nof_re * nof_layers, "Invalid equalized noise variances size.");
  ocudu_assert(tx_scaling > 0, "Tx scaling factor must be positive.");
  ocudu_assert(is_supported(nof_rx_ports, nof_layers), "Unsupported equalizer topology.");

  // Select the most pessimistic noise variance (CPU convention for the m x n path).
  const float noise_var = *std::max_element(noise_var_estimates.begin(), noise_var_estimates.end());

  // Skip processing if the noise variance is NaN, infinity or negative (CPU semantics).
  if (!std::isnormal(noise_var) || (noise_var < 0.0F)) {
    ocuduvec::zero(eq_symbols);
    std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
    return;
  }

  // Stage H (bf16 -> float2, tx_scaling applied) and y (bf16 -> float2).
  const size_t h_bytes  = static_cast<size_t>(nof_rx_ports) * nof_layers * nof_re * 2 * sizeof(float);
  const size_t y_bytes  = static_cast<size_t>(nof_rx_ports) * nof_re * 2 * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);
  auto*        h_ptr    = static_cast<float*>(impl::ensure(impl_->h_buf, impl_->h_cap, h_bytes));
  auto*        y_ptr    = static_cast<float*>(impl::ensure(impl_->y_buf, impl_->y_cap, y_bytes));
  auto*        eq_ptr   = static_cast<float*>(impl::ensure(impl_->eq_buf, impl_->eq_cap, eq_bytes));
  auto*        nv_ptr   = static_cast<float*>(impl::ensure(impl_->nv_buf, impl_->nv_cap, nv_bytes));

  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    span<const cbf16_t> y_view = ch_symbols.get_slice(i_port);
    for (unsigned i_re = 0; i_re != nof_re; ++i_re) {
      y_ptr[2 * (i_port * nof_re + i_re)]     = to_float(y_view[i_re].real);
      y_ptr[2 * (i_port * nof_re + i_re) + 1] = to_float(y_view[i_re].imag);
    }
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      span<const cbf16_t> h_view = ch_estimates.get_channel(i_port, i_layer);
      for (unsigned i_re = 0; i_re != nof_re; ++i_re) {
        const size_t idx          = ((static_cast<size_t>(i_port) * nof_layers + i_layer) * nof_re + i_re) * 2;
        h_ptr[idx]                = to_float(h_view[i_re].real) * tx_scaling;
        h_ptr[idx + 1]            = to_float(h_view[i_re].imag) * tx_scaling;
      }
    }
  }

  const bool ok = impl_->engine.equalize(
      h_ptr, y_ptr, eq_ptr, nv_ptr, nof_re, nof_rx_ports, nof_layers, impl_->mmse, noise_var);
  if (!ok) {
    // Engine failure: mirror the CPU invalid-input semantics instead of leaving stale data.
    ocuduvec::zero(eq_symbols);
    std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
    return;
  }

  std::memcpy(eq_symbols.data(), eq_ptr, eq_bytes);
  std::memcpy(eq_noise_vars.data(), nv_ptr, nv_bytes);
}

double channel_equalizer_metal::engine_gpu_wait_us() const
{
  return impl_->engine.last_gpu_wait_us();
}
