// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "channel_equalizer_metal.h"
#include "ocudu_equalizer_metal_engine.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/ocuduvec/fill.h"
#include "ocudu/ocuduvec/zero.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>

using namespace ocudu;

namespace {

/// True when the buffer can be handed to the Metal no-copy wrap: its base address is page
/// aligned. The engine maps a page-rounded length, so the caller's allocation must cover the
/// rounded tail (page_aligned_allocator does); only the RE range within the span is touched.
bool is_page_aligned_buffer(const void* ptr)
{
  return (ptr != nullptr) && ((reinterpret_cast<uintptr_t>(ptr) % compat::page_size()) == 0);
}

} // namespace

struct channel_equalizer_metal::impl {
  metal::equalizer_metal_engine engine;
  bool                           engine_ok = false;
  bool                           mmse      = false;

  // One-shot diagnostic: reports whether the caller's buffers allow the in-place path.
  bool path_logged = false;

  // Staging buffers (page-aligned, grown on demand and reused across calls).
  void* h_buf      = nullptr;
  void* s_buf      = nullptr;
  void* y_buf      = nullptr;
  void* eq_buf     = nullptr;
  void* nv_buf     = nullptr;
  size_t h_cap     = 0; // bytes
  size_t s_cap     = 0;
  size_t y_cap     = 0;
  size_t eq_cap    = 0;
  size_t nv_cap    = 0;

  ~impl()
  {
    compat::aligned_free(h_buf);
    compat::aligned_free(s_buf);
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
  // Same acceptance set as the CPU generic implementation.
  if ((nof_ports != 1) && (nof_ports != 2) && (nof_ports != 4) && (nof_ports != 8)) {
    return false;
  }
  return (nof_layers >= 1) && (nof_layers <= 4) && (nof_layers <= nof_ports) && impl_->engine_ok;
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

  // Multi-layer path: single noise variance (the most pessimistic one, as the CPU m x n
  // path does) and H pre-scaled by tx_scaling.
  // Single-layer path: per-port noise variances and UNSCALED H (the 1 x n CPU path folds
  // tx_scaling into the pseudo-inverse denominator). Ports with a non-positive or
  // non-finite noise variance are dropped, replicating the CPU port reduction.
  const bool     single_layer = (nof_layers == 1);
  float          noise_var    = 0.0F;
  unsigned       nof_used_ports = nof_rx_ports;
  std::array<unsigned, metal::equalizer_metal_engine::max_ports> port_map{};

  if (single_layer) {
    unsigned nof_valid = 0;
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      const float nvar = noise_var_estimates[i_port];
      // CPU validity predicate of equalize_zf_single_tx_layer_reduction.
      if ((nvar > 0.0F) && (nvar < std::numeric_limits<float>::infinity())) {
        port_map[nof_valid++] = i_port;
      }
    }
    if (nof_valid == 0) {
      // CPU semantics: no valid noise variance, fill the output with invalid data.
      ocuduvec::zero(eq_symbols);
      std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
      return;
    }
    nof_used_ports = nof_valid;
  } else {
    noise_var = *std::max_element(noise_var_estimates.begin(), noise_var_estimates.end());
    // Skip processing if the noise variance is NaN, infinity or negative (CPU semantics).
    if (!std::isnormal(noise_var) || (noise_var < 0.0F)) {
      ocuduvec::zero(eq_symbols);
      std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
      return;
    }
    for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
      port_map[i_port] = i_port;
    }
  }

  // Stage the cbf16 inputs (the kernel widens them) and pick the output buffers: when the
  // caller's spans satisfy the Metal no-copy requirements (page-aligned, page-multiple
  // length) the kernel writes them directly, otherwise the reusable staging buffers are used
  // and copied back afterwards.
  const size_t h_bytes  = static_cast<size_t>(nof_used_ports) * nof_layers * nof_re * sizeof(cbf16_t);
  const size_t y_bytes  = static_cast<size_t>(nof_used_ports) * nof_re * sizeof(cbf16_t);
  const size_t s_bytes  = static_cast<size_t>(nof_used_ports) * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);

  auto*        h_ptr    = static_cast<cbf16_t*>(impl::ensure(impl_->h_buf, impl_->h_cap, h_bytes));
  auto*        y_ptr    = static_cast<cbf16_t*>(impl::ensure(impl_->y_buf, impl_->y_cap, y_bytes));
  auto*        s_ptr    = static_cast<float*>(impl::ensure(impl_->s_buf, impl_->s_cap, s_bytes));
  const bool   eq_direct = is_page_aligned_buffer(eq_symbols.data());
  const bool   nv_direct = is_page_aligned_buffer(eq_noise_vars.data());
  void*        eq_ptr    = eq_direct ? static_cast<void*>(eq_symbols.data())
                                     : impl::ensure(impl_->eq_buf, impl_->eq_cap, eq_bytes);
  void*        nv_ptr    = nv_direct ? static_cast<void*>(eq_noise_vars.data())
                                     : impl::ensure(impl_->nv_buf, impl_->nv_cap, nv_bytes);

  if (!impl_->path_logged) {
    impl_->path_logged = true;
    ocudulog::fetch_basic_logger("PHY").debug(
        "Metal equalizer: outputs {} (eq {} nv {}), inputs staged as cbf16 ({} bytes/call)",
        (eq_direct && nv_direct) ? "written in place (zero copy)" : "written to staging and copied back",
        eq_direct ? "direct" : "staged",
        nv_direct ? "direct" : "staged",
        h_bytes + y_bytes);
  }

  for (unsigned i_used = 0; i_used != nof_used_ports; ++i_used) {
    const unsigned i_port = port_map[i_used];
    std::memcpy(y_ptr + static_cast<size_t>(i_used) * nof_re,
                ch_symbols.get_slice(i_port).data(),
                static_cast<size_t>(nof_re) * sizeof(cbf16_t));
    for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
      std::memcpy(h_ptr + (static_cast<size_t>(i_used) * nof_layers + i_layer) * nof_re,
                  ch_estimates.get_channel(i_port, i_layer).data(),
                  static_cast<size_t>(nof_re) * sizeof(cbf16_t));
    }
    if (single_layer) {
      s_ptr[i_used] = noise_var_estimates[i_port];
    }
  }
  // The single-layer kernel reads a per-port noise variance array; keep it defined (and
  // cached) even when the multi-layer path does not use it.
  if (!single_layer) {
    for (unsigned i_port = 0; i_port != nof_used_ports; ++i_port) {
      s_ptr[i_port] = noise_var;
    }
  }

  const bool ok = impl_->engine.equalize(h_ptr,
                                        y_ptr,
                                        s_ptr,
                                        eq_ptr,
                                        nv_ptr,
                                        nof_re,
                                        nof_used_ports,
                                        nof_layers,
                                        impl_->mmse,
                                        noise_var,
                                        tx_scaling,
                                        single_layer ? 1.0F : tx_scaling);
  if (!ok) {
    // Engine failure: mirror the CPU invalid-input semantics instead of leaving stale data.
    ocuduvec::zero(eq_symbols);
    std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
    return;
  }

  if (!eq_direct) {
    std::memcpy(eq_symbols.data(), eq_ptr, eq_bytes);
  }
  if (!nv_direct) {
    std::memcpy(eq_noise_vars.data(), nv_ptr, nv_bytes);
  }
}

double channel_equalizer_metal::engine_gpu_wait_us() const
{
  return impl_->engine.last_gpu_wait_us();
}
