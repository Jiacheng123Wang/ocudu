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
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace ocudu;

namespace {

/// True when the buffer can be handed to the Metal no-copy wrap: its base address is page
/// aligned. The engine maps a page-rounded length, so the caller's allocation must cover the
/// rounded tail (page_aligned_allocator does); only the RE range within the span is touched.
bool is_page_aligned_buffer(const void* ptr)
{
  return (ptr != nullptr) && ((reinterpret_cast<uintptr_t>(ptr) % compat::page_size()) == 0);
}

/// \name Which source the channel estimates of a dispatch were read from (S-6c-2, diagnostics).
///
/// `device` counts the dispatches that bound the buffer the estimator produced the estimates in,
/// `staged` the ones that gathered them into the equalizer's own staging first. The device path is
/// what keeps the estimates off the host, so a regression to `staged` shows up here - including the
/// RE-count guard of the demodulator falling back when the two layouts disagree. The counts are
/// kept in every build (one relaxed atomic increment per dispatch) because the fallback is
/// otherwise unobservable: both sources produce the same soft bits.
///@{
struct ch_est_source_counters {
  std::atomic<uint64_t> device{0};
  std::atomic<uint64_t> staged{0};
};

ch_est_source_counters& ch_est_source()
{
  static ch_est_source_counters c;
  return c;
}

#if defined(OCUDU_METAL_STATS)
const bool ch_est_source_registered = []() {
  std::atexit([]() {
    std::fprintf(stderr,
                 "[metal_stats] equalizer ch_est device=%llu staged=%llu\n",
                 static_cast<unsigned long long>(ch_est_source().device.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(ch_est_source().staged.load(std::memory_order_relaxed)));
  });
  return true;
}();
#endif
///@}

/// \name Where the received symbols (the y input) of a deferred submit came from (diagnostics).
///
/// `device` counts the submits whose symbols the GPU gathers off the resource grid, `host` the ones
/// that staged them with a memcpy per port. The two produce the same soft bits, so a silent fallback
/// to the host path is otherwise unobservable - and the fallback is legitimate (a plan the backend
/// cannot use, or the synchronous route).
///@{
struct ch_re_source_counters {
  std::atomic<uint64_t> device{0};
  std::atomic<uint64_t> host{0};
};

ch_re_source_counters& ch_re_source()
{
  static ch_re_source_counters c;
  return c;
}

#if defined(OCUDU_METAL_STATS)
const bool ch_re_source_registered = []() {
  std::atexit([]() {
    std::fprintf(stderr,
                 "[metal_stats] equalizer ch_re device=%llu host=%llu\n",
                 static_cast<unsigned long long>(ch_re_source().device.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(ch_re_source().host.load(std::memory_order_relaxed)));
  });
  return true;
}();
#endif
///@}

} // namespace

void channel_equalizer_metal::staging::swap(staging& other) noexcept
{
  std::swap(ptr, other.ptr);
  std::swap(cap, other.cap);
}

channel_equalizer_metal::staging& channel_equalizer_metal::staging::operator=(staging&& other) noexcept
{
  if (this != &other) {
    compat::aligned_free(ptr);
    ptr       = other.ptr;
    cap       = other.cap;
    other.ptr = nullptr;
    other.cap = 0;
  }
  return *this;
}

channel_equalizer_metal::staging::~staging()
{
  compat::aligned_free(ptr);
}

void* channel_equalizer_metal::staging::ensure(size_t needed)
{
  if (cap >= needed) {
    return ptr;
  }
  compat::aligned_free(ptr);
  ptr = compat::aligned_alloc(compat::page_size(), needed);
  ocudu_assert(ptr != nullptr, "Equalizer staging allocation failed.");
  cap = needed;
  return ptr;
}

struct channel_equalizer_metal::impl {
  metal::equalizer_metal_engine engine;
  bool                           engine_ok = false;
  bool                           mmse      = false;

  // One-shot diagnostic: reports whether the caller's buffers allow the in-place path.
  bool path_logged = false;

  /// Device gather plan of the symbol the next submit() covers, or nullptr when the received
  /// symbols have to be staged on the host (see channel_equalizer::set_device_grid()). The plan
  /// belongs to the caller and outlives the submit, so only the pointer and the symbol are kept.
  const ch_gather_desc* device_grid        = nullptr;
  unsigned              device_grid_symbol = 0;

  /// In-flight deferred submits, oldest first. All of them are committed to the shared back-end
  /// queue in order, so a single wait on the newest command buffer covers the whole FIFO.
  std::vector<std::unique_ptr<pending_entry>> pending;
  /// Entries released by wait(), kept so their staging buffers stay warm.
  std::vector<std::unique_ptr<pending_entry>> pool;
  /// Staging for the synchronous path (never in flight).
  pending_entry scratch;

  std::unique_ptr<pending_entry> acquire()
  {
    if (pool.empty()) {
      return std::make_unique<pending_entry>();
    }
    std::unique_ptr<pending_entry> entry = std::move(pool.back());
    pool.pop_back();
    return entry;
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

bool channel_equalizer_metal::consumes_device_estimates(unsigned nof_ports, unsigned nof_layers) const
{
  // One receive port (hence one layer) is the shape a single dispatch can read straight out of the
  // estimator's buffer: more ports means one buffer per port, which one base pointer cannot
  // describe. The kernel also applies the noise-variance validity predicate the host would apply,
  // so the variances it reads off the device need no host check either.
  return (nof_ports == 1) && (nof_layers == 1) && impl_->engine_ok;
}

void channel_equalizer_metal::equalize(span<cf_t>                       eq_symbols,
                                       span<float>                      eq_noise_vars,
                                       const re_buffer_reader<cbf16_t>& ch_symbols,
                                       const ch_est_list&               ch_estimates,
                                       span<const float>                noise_var_estimates,
                                       float                            tx_scaling)
{
  run_equalize(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling, impl_->scratch, false);
}

void channel_equalizer_metal::submit(span<cf_t>                       eq_symbols,
                                     span<float>                      eq_noise_vars,
                                     const re_buffer_reader<cbf16_t>& ch_symbols,
                                     const ch_est_list&               ch_estimates,
                                     span<const float>                noise_var_estimates,
                                     float                            tx_scaling)
{
  // The dispatches of the whole burst (this stage and the next one) are appended to the shared
  // command buffer, which wait() closes: see shared_burst.
  std::unique_ptr<pending_entry> entry = impl_->acquire();
  run_equalize(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling, *entry, true);
  impl_->pending.push_back(std::move(entry));
}

void channel_equalizer_metal::submit_batch_run(span<const group_symbol> run, const symbol_plan& plan)
{
  // One dispatch for the whole run. The inputs are staged into one contiguous buffer using the
  // uniform per-symbol strides the kernel expects (h: [port][layer][re], y: [port][re]); the
  // outputs are written in place, which submit_group() guarantees for the runs sent here.
  const unsigned n_sym    = run.size();
  const unsigned nof_re   = plan.nof_re;
  const size_t   h_stride = static_cast<size_t>(plan.nof_used_ports) * plan.nof_layers * nof_re;
  const size_t   y_stride = static_cast<size_t>(plan.nof_used_ports) * nof_re;
  const auto     eq_stride = static_cast<unsigned>(run[1].eq_symbols.data() - run[0].eq_symbols.data());
  const auto     nv_stride = static_cast<unsigned>(run[1].eq_noise_vars.data() - run[0].eq_noise_vars.data());

  std::unique_ptr<pending_entry> entry = impl_->acquire();

  auto* h_ptr = static_cast<cbf16_t*>(entry->h.ensure(h_stride * n_sym * sizeof(cbf16_t)));
  auto* y_ptr = static_cast<cbf16_t*>(entry->y.ensure(y_stride * n_sym * sizeof(cbf16_t)));
  auto* s_ptr = static_cast<float*>(entry->s.ensure(static_cast<size_t>(plan.nof_used_ports) * sizeof(float)));

  for (unsigned i_sym = 0; i_sym != n_sym; ++i_sym) {
    const group_symbol& symbol = run[i_sym];
    cbf16_t*            h_sym  = h_ptr + i_sym * h_stride;
    cbf16_t*            y_sym  = y_ptr + i_sym * y_stride;
    for (unsigned i_used = 0; i_used != plan.nof_used_ports; ++i_used) {
      const unsigned i_port = plan.port_map[i_used];
      std::memcpy(y_sym + static_cast<size_t>(i_used) * nof_re,
                  symbol.ch_symbols->get_slice(i_port).data(),
                  static_cast<size_t>(nof_re) * sizeof(cbf16_t));
      for (unsigned i_layer = 0; i_layer != plan.nof_layers; ++i_layer) {
        std::memcpy(h_sym + (static_cast<size_t>(i_used) * plan.nof_layers + i_layer) * nof_re,
                    symbol.ch_estimates->get_channel(i_port, i_layer).data(),
                    static_cast<size_t>(nof_re) * sizeof(cbf16_t));
      }
    }
  }
  // Single-layer: the per-port noise variances the kernel reads (the run predicate checked that
  // every symbol of the run carries the same ones). Multi-layer: the shared noise variance.
  for (unsigned i_used = 0; i_used != plan.nof_used_ports; ++i_used) {
    s_ptr[i_used] = plan.single_layer ? run.front().noise_var_estimates[plan.port_map[i_used]] : plan.noise_var;
  }

  const bool ok = impl_->engine.enqueue_burst_batch(h_ptr,
                                                    y_ptr,
                                                    s_ptr,
                                                    run.front().eq_symbols.data(),
                                                    run.front().eq_noise_vars.data(),
                                                    nof_re,
                                                    n_sym,
                                                    static_cast<unsigned>(h_stride),
                                                    static_cast<unsigned>(y_stride),
                                                    eq_stride,
                                                    nv_stride,
                                                    plan.nof_used_ports,
                                                    plan.nof_layers,
                                                    impl_->mmse,
                                                    plan.noise_var,
                                                    run.front().tx_scaling,
                                                    plan.single_layer ? 1.0F : run.front().tx_scaling);
  if (!ok) {
    // Engine failure: mirror the invalid-input semantics of the per-symbol path.
    for (const group_symbol& symbol : run) {
      ocuduvec::zero(symbol.eq_symbols);
      std::fill(symbol.eq_noise_vars.begin(), symbol.eq_noise_vars.end(), std::numeric_limits<float>::infinity());
    }
    return;
  }
  // Every output of the run is written in place by this dispatch, so there is nothing to copy back.
  entry->eq_direct = true;
  entry->nv_direct = true;
  for (const group_symbol& symbol : run) {
    entry->batch_outs.emplace_back(symbol.eq_symbols, symbol.eq_noise_vars);
  }
  impl_->pending.push_back(std::move(entry));
}

void channel_equalizer_metal::submit_group(span<const group_symbol> group)
{
  // The group is split into maximal runs sharing a geometry (nof_re), a port reduction / noise
  // path, the same noise variance inputs and uniformly strided page-aligned outputs; each run
  // becomes ONE dispatch. A single-symbol run (a run boundary, an odd geometry, or a caller whose
  // outputs cannot be wrapped) goes through the per-symbol submit() path unchanged.
  unsigned i = 0;
  while (i != group.size()) {
    const group_symbol& head = group[i];
    const symbol_plan   plan = resolve_plan(*head.ch_symbols, *head.ch_estimates, head.noise_var_estimates);
    if (plan.invalid_input) {
      // CPU semantics of the per-symbol path for an ill-formed noise variance.
      ocuduvec::zero(head.eq_symbols);
      std::fill(head.eq_noise_vars.begin(), head.eq_noise_vars.end(), std::numeric_limits<float>::infinity());
      ++i;
      continue;
    }

    const bool head_wrappable = is_page_aligned_buffer(head.eq_symbols.data()) &&
                                is_page_aligned_buffer(head.eq_noise_vars.data());
    unsigned   n_run = 1;
    if (head_wrappable) {
      const auto eq_stride = (i + 1 != group.size())
                                 ? group[i + 1].eq_symbols.data() - group[i].eq_symbols.data()
                                 : ptrdiff_t{0};
      const auto nv_stride = (i + 1 != group.size())
                                 ? group[i + 1].eq_noise_vars.data() - group[i].eq_noise_vars.data()
                                 : ptrdiff_t{0};
      while (i + n_run != group.size()) {
        const group_symbol& next = group[i + n_run];
        const group_symbol& prev = group[i + n_run - 1];
        const symbol_plan   next_plan =
            resolve_plan(*next.ch_symbols, *next.ch_estimates, next.noise_var_estimates);
        const bool same_plan = !next_plan.invalid_input && (next_plan.nof_re == plan.nof_re) &&
                               (next_plan.nof_layers == plan.nof_layers) &&
                               (next_plan.nof_used_ports == plan.nof_used_ports) &&
                               (next_plan.single_layer == plan.single_layer) &&
                               (next_plan.port_map == plan.port_map) && (next_plan.noise_var == plan.noise_var) &&
                               std::equal(next.noise_var_estimates.begin(),
                                          next.noise_var_estimates.end(),
                                          head.noise_var_estimates.begin());
        const bool same_layout = (next.eq_symbols.size() == head.eq_symbols.size()) &&
                                 (next.eq_noise_vars.size() == head.eq_noise_vars.size()) &&
                                 (next.eq_symbols.data() - prev.eq_symbols.data() == eq_stride) &&
                                 (next.eq_noise_vars.data() - prev.eq_noise_vars.data() == nv_stride) &&
                                 (next.tx_scaling == head.tx_scaling) &&
                                 is_page_aligned_buffer(next.eq_symbols.data()) &&
                                 is_page_aligned_buffer(next.eq_noise_vars.data());
        if (!same_plan || !same_layout) {
          break;
        }
        ++n_run;
      }
    }

    if (n_run == 1) {
      submit(head.eq_symbols,
             head.eq_noise_vars,
             *head.ch_symbols,
             *head.ch_estimates,
             head.noise_var_estimates,
             head.tx_scaling);
    } else {
      submit_batch_run(group.subspan(i, n_run), plan);
    }
    i += n_run;
  }
}

bool channel_equalizer_metal::consumes_gathered_symbols(unsigned nof_ports, unsigned nof_layers) const
{
  // A single receive port (hence a single layer) is the shape one gather dispatch reads and one plan
  // describes; the deferred route is the one that has a command buffer to share the gather with.
  return (nof_ports == 1) && (nof_layers == 1) && impl_->engine_ok;
}

void channel_equalizer_metal::set_device_grid(const ch_gather_desc& grid, unsigned symbol)
{
  // The plan is used by the next submit() only, and only when it is a deferred one: the gather is a
  // dispatch, so it needs the command buffer the burst opens. The synchronous path and the batched
  // group path keep staging on the host.
  impl_->device_grid        = grid.is_valid() ? &grid : nullptr;
  impl_->device_grid_symbol = symbol;
}

void channel_equalizer_metal::wait()
{
  // Close the shared burst opened by the submits of this group and wait for it. The demapping of
  // the same group usually closed it already, in which case both calls are no-ops.
  if (impl_->engine.burst_open()) {
    (void)impl_->engine.burst_commit();
  }
  if (impl_->pending.empty()) {
    (void)impl_->engine.burst_wait_committed();
    return;
  }
  (void)impl_->engine.burst_wait_committed();
  for (std::unique_ptr<pending_entry>& entry : impl_->pending) {
    finish_symbol(*entry);
    impl_->pool.push_back(std::move(entry));
  }
  impl_->pending.clear();
}

channel_equalizer_metal::symbol_plan channel_equalizer_metal::resolve_plan(
    const re_buffer_reader<cbf16_t>& ch_symbols,
    const ch_est_list&               ch_estimates,
    span<const float>                noise_var_estimates,
    bool                             device_noise_variance)
{
  symbol_plan plan;
  plan.nof_re       = ch_estimates.get_nof_re();
  plan.nof_rx_ports = ch_estimates.get_nof_rx_ports();
  plan.nof_layers   = ch_estimates.get_nof_tx_layers();

  ocudu_assert(ch_symbols.get_nof_re() == plan.nof_re, "Invalid channel symbols size.");
  ocudu_assert(ch_symbols.get_nof_slices() == plan.nof_rx_ports, "Invalid channel symbols ports.");
  ocudu_assert(noise_var_estimates.size() == plan.nof_rx_ports, "Invalid noise variance estimates size.");
  ocudu_assert(is_supported(plan.nof_rx_ports, plan.nof_layers), "Unsupported equalizer topology.");

  // Multi-layer path: single noise variance (the most pessimistic one, as the CPU m x n path does)
  // and H pre-scaled by tx_scaling. Single-layer path: per-port noise variances and UNSCALED H (the
  // 1 x n CPU path folds tx_scaling into the pseudo-inverse denominator). Ports with a non-positive
  // or non-finite noise variance are dropped, replicating the CPU port reduction.
  plan.single_layer   = (plan.nof_layers == 1);
  plan.nof_used_ports = plan.nof_rx_ports;
  if (device_noise_variance) {
    // The kernel reads the variances off the device, where they are current, and applies the same
    // per-port validity predicate and the same "no valid port" outcome. So the values must NOT be
    // read here: every port takes part and the dispatch geometry does not depend on them.
    ocudu_assert(plan.single_layer, "Device noise variances are only offered for a single layer.");
    for (unsigned i_port = 0; i_port != plan.nof_rx_ports; ++i_port) {
      plan.port_map[i_port] = i_port;
    }
    return plan;
  }
  if (plan.single_layer) {
    unsigned nof_valid = 0;
    for (unsigned i_port = 0; i_port != plan.nof_rx_ports; ++i_port) {
      const float nvar = noise_var_estimates[i_port];
      // CPU validity predicate of equalize_zf_single_tx_layer_reduction.
      if ((nvar > 0.0F) && (nvar < std::numeric_limits<float>::infinity())) {
        plan.port_map[nof_valid++] = i_port;
      }
    }
    if (nof_valid == 0) {
      // CPU semantics: no valid noise variance, fill the output with invalid data.
      plan.invalid_input = true;
      return plan;
    }
    plan.nof_used_ports = nof_valid;
  } else {
    plan.noise_var = *std::max_element(noise_var_estimates.begin(), noise_var_estimates.end());
    // Skip processing if the noise variance is NaN, infinity or negative (CPU semantics).
    if (!std::isnormal(plan.noise_var) || (plan.noise_var < 0.0F)) {
      plan.invalid_input = true;
      return plan;
    }
    for (unsigned i_port = 0; i_port != plan.nof_rx_ports; ++i_port) {
      plan.port_map[i_port] = i_port;
    }
  }
  return plan;
}

void channel_equalizer_metal::run_equalize(span<cf_t>                       eq_symbols,
                                           span<float>                      eq_noise_vars,
                                           const re_buffer_reader<cbf16_t>& ch_symbols,
                                           const ch_est_list&               ch_estimates,
                                           span<const float>                noise_var_estimates,
                                           float                            tx_scaling,
                                           pending_entry&                   entry,
                                           bool                             defer)
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

  // Device slice of the estimates, when the estimator published one for a shape this backend can
  // read in place: one receive port, hence one layer (see consumes_device_estimates()). Its noise
  // variance, when it carries one, is what the kernel scales the soft bits with - so this pass never
  // reads either of them on the host.
  std::optional<ch_est_list::device_slice> device_slice;
  if ((nof_rx_ports == 1) && (nof_layers == 1)) {
    std::optional<ch_est_list::device_slice> slice = ch_estimates.get_device_slice(0);
    if (slice.has_value() && (slice->nof_layers == 1) && (slice->base != nullptr)) {
      device_slice = slice;
    }
  }
  const bool device_noise_variance = device_slice.has_value() && (device_slice->noise_var != nullptr);

  // Port reduction, validity and noise path of this symbol (shared with submit_group()).
  const symbol_plan plan = resolve_plan(ch_symbols, ch_estimates, noise_var_estimates, device_noise_variance);
  if (plan.invalid_input) {
    // CPU semantics: fill the output with invalid data for an ill-formed noise variance.
    ocuduvec::zero(eq_symbols);
    std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
    return;
  }
  const bool     single_layer   = plan.single_layer;
  const float    noise_var      = plan.noise_var;
  const unsigned nof_used_ports = plan.nof_used_ports;
  const std::array<unsigned, metal::equalizer_metal_engine::max_ports>& port_map = plan.port_map;

  // Stage the cbf16 inputs (the kernel widens them) and pick the output buffers: when the
  // caller's spans satisfy the Metal no-copy requirements (page-aligned, page-multiple
  // length) the kernel writes them directly, otherwise the reusable staging buffers are used
  // and copied back afterwards.
  const size_t h_bytes  = static_cast<size_t>(nof_used_ports) * nof_layers * nof_re * sizeof(cbf16_t);
  const size_t y_bytes  = static_cast<size_t>(nof_used_ports) * nof_re * sizeof(cbf16_t);
  const size_t s_bytes  = static_cast<size_t>(nof_used_ports) * sizeof(float);
  const size_t eq_bytes = static_cast<size_t>(nof_layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(nof_layers) * nof_re * sizeof(float);

  // Channel estimates and noise variance: one dispatch reads the estimates of every used port and
  // layer. When there is a single used port and the estimator published them as a device slice of
  // the buffer it produced them in, bind that buffer - the inputs are then read where the GPU wrote
  // them, and the pass never touches them on the host. Otherwise gather them into the staging
  // buffers as before.
  metal::equalizer_metal_engine::ch_est_binding h_binding;
  bool                                         h_device = false;
  if (device_slice.has_value() && (nof_used_ports == 1) && single_layer) {
    h_binding = metal::equalizer_metal_engine::ch_est_binding(device_slice->base,
                                                              static_cast<unsigned>(device_slice->offset),
                                                              device_slice->layer_stride);
    h_device  = true;
  }

  auto*        h_ptr    = h_device ? nullptr : static_cast<cbf16_t*>(entry.h.ensure(h_bytes));
  auto*        y_ptr    = static_cast<cbf16_t*>(entry.y.ensure(y_bytes));
  const float* s_dev    = device_noise_variance ? device_slice->noise_var : nullptr;
  auto*        s_ptr    = (s_dev != nullptr) ? nullptr : static_cast<float*>(entry.s.ensure(s_bytes));
  const bool   eq_direct = is_page_aligned_buffer(eq_symbols.data());
  const bool   nv_direct = is_page_aligned_buffer(eq_noise_vars.data());
  void*        eq_ptr    = eq_direct ? static_cast<void*>(eq_symbols.data()) : entry.eq_stage.ensure(eq_bytes);
  void*        nv_ptr    = nv_direct ? static_cast<void*>(eq_noise_vars.data()) : entry.nv_stage.ensure(nv_bytes);

  // The received symbols: the deferred route reads them off the device grid when the caller
  // announced a plan (see set_device_grid()), which is the whole point - the host no longer copies
  // them, and the caller does not have to gather them either. The plan covers every receive port of
  // the hop and the equalizer uses all of them (the kernel applies the port reduction), so the two
  // agree on the port runs. Every other route stages them here as before.
  const auto gather_plan = [&]() -> const ch_gather_desc* {
    if (!defer || (impl_->device_grid == nullptr)) {
      return nullptr;
    }
    if ((nof_used_ports != nof_rx_ports) || (impl_->device_grid->nof_ports != nof_rx_ports)) {
      return nullptr;
    }
    return impl_->device_grid;
  }();
  if (gather_plan == nullptr) {
    ch_re_source().host.fetch_add(1, std::memory_order_relaxed);
  }

  for (unsigned i_used = 0; i_used != nof_used_ports; ++i_used) {
    const unsigned i_port = port_map[i_used];
    if (gather_plan == nullptr) {
      std::memcpy(y_ptr + static_cast<size_t>(i_used) * nof_re,
                  ch_symbols.get_slice(i_port).data(),
                  static_cast<size_t>(nof_re) * sizeof(cbf16_t));
    }
    if (!h_device) {
      for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
        std::memcpy(h_ptr + (static_cast<size_t>(i_used) * nof_layers + i_layer) * nof_re,
                    ch_estimates.get_channel(i_port, i_layer).data(),
                    static_cast<size_t>(nof_re) * sizeof(cbf16_t));
      }
    }
    if (single_layer && (s_ptr != nullptr)) {
      s_ptr[i_used] = noise_var_estimates[i_port];
    }
  }
  if (!h_device) {
    h_binding = metal::equalizer_metal_engine::ch_est_binding(h_ptr);
  }
  (h_device ? ch_est_source().device : ch_est_source().staged).fetch_add(1, std::memory_order_relaxed);
  // The single-layer kernel reads a per-port noise variance array; keep it defined (and
  // cached) even when the multi-layer path does not use it.
  if (!single_layer) {
    for (unsigned i_port = 0; i_port != nof_used_ports; ++i_port) {
      s_ptr[i_port] = noise_var;
    }
  }
  // Where the kernel reads the per-port noise variances from: the estimator's device buffer when it
  // published one, the local staging array otherwise.
  const void* s_binding = (s_dev != nullptr) ? static_cast<const void*>(s_dev) : static_cast<const void*>(s_ptr);

  if (defer) {
    // Append the dispatch to the shared burst of this group: every stage of the burst ends up in
    // one command buffer, with a memory barrier where the pipeline changes (see shared_burst).
    const bool ok = impl_->engine.enqueue_burst(h_binding,
                                                h_device,
                                                y_ptr,
                                                s_binding,
                                                eq_ptr,
                                                nv_ptr,
                                                nof_re,
                                                nof_used_ports,
                                                nof_layers,
                                                impl_->mmse,
                                                noise_var,
                                                tx_scaling,
                                                single_layer ? 1.0F : tx_scaling,
                                                metal::equalizer_metal_engine::gather_binding(
                                                    gather_plan, impl_->device_grid_symbol));
    if (ok && (gather_plan != nullptr)) {
      ch_re_source().device.fetch_add(1, std::memory_order_relaxed);
    }
    if (!ok) {
      // Engine failure: mirror the CPU invalid-input semantics instead of leaving stale data. The
      // outputs are written in place, so the entry must not copy staging data over them later.
      ocuduvec::zero(eq_symbols);
      std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
      entry.eq_direct = true;
      entry.nv_direct = true;
      entry.eq        = eq_symbols;
      entry.nv        = eq_noise_vars;
      entry.eq_ptr    = nullptr;
      entry.nv_ptr    = nullptr;
      return;
    }
    entry.eq        = eq_symbols;
    entry.nv        = eq_noise_vars;
    entry.eq_ptr    = eq_ptr;
    entry.nv_ptr    = nv_ptr;
    entry.eq_direct = eq_direct;
    entry.nv_direct = nv_direct;
    return;
  }

  // Synchronous path: never share a command buffer with an unfinished burst.
  if (impl_->engine.burst_open()) {
    (void)impl_->engine.burst_commit();
    (void)impl_->engine.burst_wait_committed();
  }
  (void)impl_->engine.begin_batch();
  const bool ok = impl_->engine.enqueue(h_binding,
                                       y_ptr,
                                       s_binding,
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
    // Engine failure: mirror the CPU invalid-input semantics instead of leaving stale data. The
    // outputs are written in place, so the entry must not copy staging data over them later.
    (void)impl_->engine.flush_batch();
    ocuduvec::zero(eq_symbols);
    std::fill(eq_noise_vars.begin(), eq_noise_vars.end(), std::numeric_limits<float>::infinity());
    entry.eq_direct = true;
    entry.nv_direct = true;
    entry.eq        = eq_symbols;
    entry.nv        = eq_noise_vars;
    entry.eq_ptr    = nullptr;
    entry.nv_ptr    = nullptr;
    return;
  }
  // Remember what has to be copied back once the command buffer completes (deferred path only).
  entry.eq                 = eq_symbols;
  entry.nv                 = eq_noise_vars;
  entry.eq_ptr             = eq_ptr;
  entry.nv_ptr             = nv_ptr;
  entry.eq_direct          = eq_direct;
  entry.nv_direct          = nv_direct;

  (void)impl_->engine.flush_batch();
  finish_symbol(entry);
}
void channel_equalizer_metal::finish_symbol(pending_entry& entry)
{
  // Copies the staged outputs back after the command buffer completed (no-op for the in-place
  // path). The one-shot routing diagnostic is emitted here, once the wrap outcome is known.
  if (!impl_->path_logged) {
    impl_->path_logged = true;
    ocudulog::fetch_basic_logger("PHY").info("Metal equalizer: outputs {}, engine no-copy wrap {}",
                                             entry.eq_direct && entry.nv_direct ? "written in place"
                                                                                : "written to staging and copied back",
                                             impl_->engine.last_call_used_no_copy() ? "OK" : "FELL BACK TO COPY");
  }
  if (!entry.eq_direct) {
    std::memcpy(entry.eq.data(), entry.eq_ptr, entry.eq.size() * sizeof(cf_t));
  }
  if (!entry.nv_direct) {
    std::memcpy(entry.nv.data(), entry.nv_ptr, entry.nv.size() * sizeof(float));
  }
}

double channel_equalizer_metal::engine_gpu_wait_us() const
{
  return impl_->engine.last_gpu_wait_us();
}

unsigned channel_equalizer_metal::engine_batch_dispatch_count() const
{
  return impl_->engine.batch_dispatch_count();
}

metal::equalizer_metal_engine::batch_diag channel_equalizer_metal::engine_batch_diagnostics() const
{
  return impl_->engine.batch_diagnostics();
}

void channel_equalizer_metal::reset_engine_batch_diagnostics()
{
  impl_->engine.reset_batch_diagnostics();
}

unsigned channel_equalizer_metal::nof_device_ch_est_dispatches()
{
  return static_cast<unsigned>(ch_est_source().device.load(std::memory_order_relaxed));
}

unsigned channel_equalizer_metal::nof_staged_ch_est_dispatches()
{
  return static_cast<unsigned>(ch_est_source().staged.load(std::memory_order_relaxed));
}
