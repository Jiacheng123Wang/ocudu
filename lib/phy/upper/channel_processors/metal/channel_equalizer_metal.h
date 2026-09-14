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
#include "ocudu_equalizer_metal_engine.h"
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
  /// One receive port (hence one layer) is the shape a single dispatch can read straight out of the
  /// estimator's buffer: more ports means one buffer per port, which one base pointer cannot
  /// describe. The kernel also applies the noise-variance validity predicate the host would apply,
  /// so the variances it reads off the device need no host check either.
  bool consumes_device_estimates(unsigned nof_ports, unsigned nof_layers) const override;

  // See interface for documentation.
  void equalize(span<cf_t>                       eq_symbols,
                span<float>                      eq_noise_vars,
                const re_buffer_reader<cbf16_t>& ch_symbols,
                const ch_est_list&               ch_estimates,
                span<const float>                noise_var_estimates,
                float                            tx_scaling) override;

  // See interface for documentation.
  void submit(span<cf_t>                       eq_symbols,
              span<float>                      eq_noise_vars,
              const re_buffer_reader<cbf16_t>& ch_symbols,
              const ch_est_list&               ch_estimates,
              span<const float>                noise_var_estimates,
              float                            tx_scaling) override;

  // See interface for documentation.
  void submit_group(span<const group_symbol> group) override;

  // See interface for documentation.
  void wait() override;

  // See interface for documentation.
  /// The Metal backend accepts the plan: the deferred route reads the received symbols off the
  /// device grid with a gather dispatch, and the synchronous route ignores the plan and uses the
  /// staged input (it has no command buffer to share the gather with).
  void set_device_grid(const ch_gather_desc& grid, unsigned symbol) override;

  // See interface for documentation.
  /// True for the same topology consumes_device_estimates() accepts: the gather reads the grid of
  /// every receive port of the hop in one dispatch, which is the shape a single device plan can
  /// describe.
  bool consumes_gathered_symbols(unsigned nof_ports, unsigned nof_layers) const override;

  // See interface for documentation.
  bool supports_deferred_chain() const override { return true; }

  /// GPU-side duration of the last call in microseconds (0 when unavailable / invalid).
  double engine_gpu_wait_us() const;

  /// Number of batched group dispatches encoded so far (diagnostics; see submit_group()).
  unsigned engine_batch_dispatch_count() const;

  /// \brief How the deferred burst was cut into dispatches (diagnostics; see submit_group()).
  ///
  /// submit_group() batches inside the caller, enqueue_burst() batches inside the engine: this
  /// reports the latter, which is the form the PUSCH chain reaches without changing the caller.
  metal::equalizer_metal_engine::batch_diag engine_batch_diagnostics() const;

  /// Clears the engine's burst diagnostics (start of a measurement).
  void reset_engine_batch_diagnostics();

  /// \name Channel-estimate source accounting (diagnostics; see run_equalize()).
  ///
  /// The counts separate the two sources a dispatch can read its channel estimates from: the buffer
  /// the channel estimator produced them in (bound directly, so they never reach the host) and the
  /// equalizer's own staging buffer. They are kept in every build - the cost is one relaxed atomic
  /// increment per dispatch, against a dispatch that costs microseconds - because a fallback to
  /// staging is otherwise invisible: it produces the same soft bits.
  ///@{
  /// Dispatches that bound the estimator's own buffer for the estimates.
  static unsigned nof_device_ch_est_dispatches();
  /// Dispatches that gathered the estimates into the equalizer staging buffer first.
  static unsigned nof_staged_ch_est_dispatches();
  ///@}

private:
  /// \brief Page-aligned staging buffer, grown on demand and kept across calls.
  ///
  /// One instance per in-flight dispatch: a deferred submit is committed without waiting, so the
  /// buffers it hands to the kernel must stay untouched until its command buffer completes. A
  /// single shared set of buffers would be overwritten by the next submit of the same burst.
  struct staging {
    void*  ptr = nullptr;
    size_t cap = 0; // bytes

    staging() noexcept                 = default;
    staging(const staging&)            = delete;
    staging& operator=(const staging&) = delete;

    staging(staging&& other) noexcept { swap(other); }
    staging& operator=(staging&& other) noexcept;

    ~staging();

    void swap(staging& other) noexcept;

    /// Returns a buffer of at least \c needed bytes, reallocating it when it is too small.
    void* ensure(size_t needed);
  };

  /// \brief One submit() awaiting wait(): where its outputs must be copied back, plus the inputs it
  /// staged and that the kernel reads until its command buffer completes.
  struct pending_entry {
    span<cf_t>  eq = {};
    span<float> nv = {};
    void*       eq_ptr    = nullptr;
    void*       nv_ptr    = nullptr;
    bool        eq_direct = false;
    bool        nv_direct = false;
    staging     h;
    staging     y;
    staging     s;
    staging     eq_stage;
    staging     nv_stage;

    /// Batched path (submit_group): the outputs of every symbol of the run, in group order. Empty
    /// on the per-symbol path. The batched path only takes runs whose outputs are all written in
    /// place by the kernel, so these are recorded for bookkeeping rather than for a copy back.
    std::vector<std::pair<span<cf_t>, span<float>>> batch_outs;
  };

  /// \brief Resolved per-symbol equalization plan (port reduction, validity and noise path).
  ///
  /// A group is split into runs that share a plan - and a geometry, and the output strides - so
  /// that one batched dispatch can serve every symbol of the run.
  struct symbol_plan {
    unsigned                                                       nof_re          = 0;
    unsigned                                                       nof_rx_ports    = 0;
    unsigned                                                       nof_layers      = 0;
    unsigned                                                       nof_used_ports  = 0;
    bool                                                           single_layer    = false;
    bool                                                           invalid_input   = false;
    float                                                          noise_var       = 0.0F;
    std::array<unsigned, metal::equalizer_metal_engine::max_ports> port_map{};
  };

  /// Resolves the plan of one symbol, applying the same validity rules and CPU semantics as
  /// run_equalize() (which uses it as well). With \c device_noise_variance the caller states that
  /// the kernel reads the noise variances off the device, so this must not read them (see
  /// consumes_device_estimates()).
  symbol_plan resolve_plan(const re_buffer_reader<cbf16_t>& ch_symbols,
                                  const ch_est_list&               ch_estimates,
                                  span<const float>                noise_var_estimates,
                                  bool                             device_noise_variance = false);

  /// Encodes one batched dispatch for a run of symbols that share \c plan, geometry and output
  /// strides, and registers the entry in the shared burst.
  void submit_batch_run(span<const group_symbol> run, const symbol_plan& plan);

  /// \brief Shared implementation of equalize() and submit(): stages the inputs into \c entry and
  /// either waits for the command buffer (defer = false) or only commits it (defer = true).
  void run_equalize(span<cf_t>                       eq_symbols,
                    span<float>                      eq_noise_vars,
                    const re_buffer_reader<cbf16_t>& ch_symbols,
                    const ch_est_list&               ch_estimates,
                    span<const float>                noise_var_estimates,
                    float                            tx_scaling,
                    pending_entry&                   entry,
                    bool                             defer);

  /// \brief Copies the staged outputs of \c entry back to the caller after its command buffer
  /// completed (no-op for the in-place path).
  void finish_symbol(pending_entry& entry);

  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace ocudu
