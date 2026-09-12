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
  bool supports_deferred_chain() const override { return true; }

  /// GPU-side duration of the last call in microseconds (0 when unavailable / invalid).
  double engine_gpu_wait_us() const;

  /// Number of batched group dispatches encoded so far (diagnostics; see submit_group()).
  unsigned engine_batch_dispatch_count() const;

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
  /// run_equalize() (which uses it as well).
  symbol_plan resolve_plan(const re_buffer_reader<cbf16_t>& ch_symbols,
                                  const ch_est_list&               ch_estimates,
                                  span<const float>                noise_var_estimates);

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
