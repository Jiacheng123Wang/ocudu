// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief C++ front end of the Metal channel equalizer engine (Objective-C++
/// implementation in ocudu_equalizer_metal_engine.mm). One engine instance per equalizer
/// object; device/queue/pipeline shared process-wide, following the DFT/LDPC engine
/// conventions: zero-copy wraps of page-aligned staging buffers with a length-checked
/// pointer cache, one command buffer per call, and the compile-time [metal_stats] probe.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

class equalizer_metal_engine
{
public:
  /// Maximum dimensions supported by the kernel.
  static constexpr unsigned max_ports  = 8;
  static constexpr unsigned max_layers = 4;

  /// \brief Where one dispatch reads its channel estimates from.
  ///
  /// The engine wraps \c buffer in the process-wide zero-copy cache and the kernel starts reading
  /// \c offset elements into it, stepping \c layer_stride elements per transmission layer (a zero
  /// \c layer_stride means "packed", i.e. nof_re). A caller that staged the estimates itself passes
  /// its own pointer at offset zero; a caller consuming estimates another stage produced - the
  /// channel estimator's device output - passes the very pointer that stage wrapped, so the two
  /// bind the same Metal resource for the same memory instead of copying it through the host.
  struct ch_est_binding {
    /// Buffer base: the pointer the producing stage wrapped.
    const void* buffer = nullptr;
    /// First element of the packed layout, in cbf16_t elements from \c buffer.
    unsigned offset = 0;
    /// Elements between two consecutive transmission layers (0 = packed, i.e. nof_re).
    unsigned layer_stride = 0;

    ch_est_binding() = default;
    /// Packed layout starting at \c ptr (the staging path).
    ch_est_binding(const void* ptr) : buffer(ptr) {} // NOLINT(google-explicit-constructor)
    /// Layout of a slice produced elsewhere: \c first_element from \c ptr, \c stride elements apart.
    ch_est_binding(const void* ptr, unsigned first_element, unsigned stride) :
      buffer(ptr), offset(first_element), layer_stride(stride)
    {
    }
  };

  equalizer_metal_engine()  = default;
  ~equalizer_metal_engine();

  equalizer_metal_engine(const equalizer_metal_engine&)            = delete;
  equalizer_metal_engine& operator=(const equalizer_metal_engine&) = delete;

  /// \brief One-shot setup (shared device/queue/pipeline + warm-up dispatch).
  /// \return True on success.
  bool init();

  /// \brief Synchronous equalization of one symbol batch.
  /// \param[in]  h          Channel estimates, layout [port][layer][re], cbf16. tx_scaling is
  ///                        applied by the caller on the multi-layer path and left out on the
  ///                        single-layer path. A staged buffer is page-aligned; estimates another
  ///                        stage produced are passed as the slice that stage wrapped, see
  ///                        ch_est_binding.
  /// \param[in]  y          Staged received symbols, layout [port][re], cbf16, page-aligned.
  /// \param[in]  sigma2     Staged per-port noise variances, float, page-aligned (used by
  ///                        the single-layer path only).
  /// \param[out] eq         Equalized symbols, layout [re][layer] interleaved, float2,
  ///                        page-aligned.
  /// \param[out] nv         Post-equalization noise variances, layout [re][layer], float,
  ///                        page-aligned.
  /// \param[in]  nof_re     Number of resource elements.
  /// \param[in]  nof_ports  Receive ports (1..8; 2/4/8 on the multi-layer path).
  /// \param[in]  nof_layers Transmit layers (1..4, <= nof_ports).
  /// \param[in]  mmse       True for the MMSE algorithm (false = ZF).
  /// \param[in]  noise_var  Noise variance estimate (max across ports, multi-layer path).
  /// \param[in]  tx_scaling Transmission gain scaling factor (single-layer path).
  /// \param[in]  h_scaling  Channel estimate scaling applied in-kernel (multi-layer path;
  ///                        use 1 on the single-layer path).
  /// \return True on success.
  bool equalize(const ch_est_binding& h,
                const void* y,
                const void* sigma2,
                void*       eq,
                void*       nv,
                unsigned    nof_re,
                unsigned    nof_ports,
                unsigned    nof_layers,
                bool        mmse,
                float       noise_var,
                float       tx_scaling,
                float       h_scaling);

  /// \brief Batched enqueue API (S2 groundwork): every dispatch enqueued between begin_batch()
  /// and flush_batch() shares a single command buffer, so a symbol batch pays one commit/wait
  /// round trip instead of one per dispatch. The caller owns the buffers: they must stay alive
  /// and untouched until flush_batch() returns.
  /// \return True on success.
  bool begin_batch();
  bool enqueue(const ch_est_binding& h,
               const void* y,
               const void* sigma2,
               void*       eq,
               void*       nv,
               unsigned    nof_re,
               unsigned    nof_ports,
               unsigned    nof_layers,
               bool        mmse,
               float       noise_var,
               float       tx_scaling,
               float       h_scaling);
  bool flush_batch();

  /// True when a batch opened by begin_batch() is still open (dispatches enqueued but not
  /// committed yet).
  bool batch_open() const;

  /// \name Shared burst: the dispatch is appended to the command buffer that the following stages
  /// of the same demodulation share, so a whole burst costs one command buffer and one commit.
  ///@{
  /// \param[in] h_on_device True when \p h points into the buffer the channel estimator produced
  ///            the estimates in. The batched encoding then reads them where they are, with the
  ///            dispatch, instead of copying them to the host: their producer may still be running,
  ///            and only a GPU read ordered through the queue sees its writes.
  bool enqueue_burst(const ch_est_binding& h,
                     bool                  h_on_device,
                     const void* y,
                     const void* sigma2,
                     void*       eq,
                     void*       nv,
                     unsigned    nof_re,
                     unsigned    nof_ports,
                     unsigned    nof_layers,
                     bool        mmse,
                     float       noise_var,
                     float       tx_scaling,
                     float       h_scaling);

  /// True when the thread-local burst has dispatches encoded but not committed yet.
  /// \brief Encodes ONE dispatch that equalizes \c nof_symbols symbols of a group whose buffers
  /// are uniformly strided (the layout the deferred chain allocates). The arithmetic is identical
  /// to enqueue_burst(), which is dispatched once per symbol.
  /// \param[in] strides Per-symbol element strides: h and y in cbf16 elements, eq in float2
  ///            elements, nv in floats.
  bool enqueue_burst_batch(const ch_est_binding& h,
                           const void* y,
                           const void* sigma2,
                           void*       eq,
                           void*       nv,
                           unsigned    nof_re,
                           unsigned    nof_symbols,
                           unsigned    h_symbol_stride,
                           unsigned    y_symbol_stride,
                           unsigned    eq_symbol_stride,
                           unsigned    nv_symbol_stride,
                           unsigned    nof_ports,
                           unsigned    nof_layers,
                           bool        mmse,
                           float       noise_var,
                           float       tx_scaling,
                           float       h_scaling);

  static bool burst_open();

  /// Commits the thread-local burst without waiting.
  static bool burst_commit();

  /// Waits for the command buffers committed through the thread-local burst.
  static bool burst_wait_committed();
  ///@}

  /// \brief Commits the batch without waiting (pairs with wait_committed()).
  ///
  /// Command buffers of one queue complete in submission order, so a single
  /// wait_committed() at the end of a burst covers every commit_batch() issued before it.
  bool commit_batch();

  /// \brief Waits for the command buffers committed so far (no-op when none are pending).
  bool wait_committed();

  /// Number of dispatches enqueued in the batch in progress (diagnostics).
  unsigned batch_size() const;

  /// True when every buffer of the last dispatch was wrapped without a copy. False means the
  /// platform refused the no-copy wrap and the engine staged through an owned MTLBuffer.
  bool last_call_used_no_copy() const;

  /// GPU-side duration of the last call in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

  /// Number of batched group dispatches encoded so far (diagnostics).
  unsigned batch_dispatch_count() const;

  /// \brief Why the accumulated burst did not become one dispatch per group (diagnostics).
  ///
  /// The deferred burst encodes its groups in the engine (see enqueue_burst()), so a group that
  /// stays one dispatch per symbol is invisible from the outside: the dispatch counters of the
  /// burst look the same as the ones of the caller-side group submit. These counters say whether
  /// the flush ran at all and, when a run stopped extending, which predicate stopped it.
  struct batch_diag {
    /// Flush hooks that found accumulated symbols and encoded them (i.e. reached the run loop).
    uint64_t flushes = 0;
    /// Symbols handed to those flushes.
    uint64_t symbols = 0;
    /// Runs encoded by them (one pipeline dispatch each, batched or per symbol).
    uint64_t runs = 0;
    /// Runs encoded with the batched kernel (more than one symbol).
    uint64_t batched_runs = 0;
    /// Longest run seen since the counters were reset.
    unsigned max_run = 0;
    /// First predicate that ever stopped a run from extending: "" (never), "geometry",
    /// "estimates" (a different estimate buffer), "strides" or "sigma2".
    const char* first_break = "";
  };

  /// Diagnostics of the deferred burst encoding (see batch_diag). Cheap enough for every build:
  /// a handful of relaxed increments per flush.
  batch_diag batch_diagnostics() const;

  /// Clears batch_diagnostics() counters (start of a measurement).
  void reset_batch_diagnostics();

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
