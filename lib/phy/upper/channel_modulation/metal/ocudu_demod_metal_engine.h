// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief C++ front end of the Metal soft-demapper engine (Objective-C++ implementation in
/// ocudu_demod_metal_engine.mm). One engine instance per demapper object; device/queue/
/// pipeline shared process-wide, following the equalizer engine conventions: zero-copy
/// wraps of page-aligned staging buffers with a length-checked pointer cache, one command
/// buffer per call, and the compile-time [metal_stats] probe.

#pragma once

#include <cstdint>

namespace ocudu {
namespace metal {

class demod_metal_engine
{
public:
  demod_metal_engine()  = default;
  ~demod_metal_engine();

  demod_metal_engine(const demod_metal_engine&)            = delete;
  demod_metal_engine& operator=(const demod_metal_engine&) = delete;

  /// \brief One-shot setup (shared device/queue/pipeline).
  /// \return True on success.
  bool init();

  /// \brief Synchronous soft demodulation of one symbol batch.
  /// \param[in]  symbols     Staged modulation symbols (float2), page-aligned.
  /// \param[in]  noise_var   Staged noise variances (float), page-aligned.
  /// \param[out] llrs        Demodulated LLRs (int8, [symbol][bit]), page-aligned.
  /// \param[in]  nof_symbols Number of symbols.
  /// \param[in]  mod         Kernel modulation id (0 = QPSK, 1 = 16QAM, 2 = 64QAM, 3 = 256QAM).
  /// \return True on success.
  bool demodulate(const void* symbols,
                  const void* noise_var,
                  void*       llrs,
                  unsigned    nof_symbols,
                  unsigned    mod);

  /// True when every buffer of the last dispatch was wrapped without a copy. False means the
  /// platform refused the no-copy wrap and the engine staged through an owned MTLBuffer.
  bool last_call_used_no_copy() const;

  /// \name Deferrable batch API (used by the fused PUSCH demodulation chain).
  ///@{
  /// \brief Opens a batch: every dispatch enqueued until commit_batch() shares one command buffer.
  bool begin_batch();

  /// \brief Enqueues one demodulation dispatch into the open batch (no wait).
  bool enqueue(const void* symbols, const void* noise_var, void* llrs, unsigned nof_symbols, unsigned mod);

  /// True when a batch opened by begin_batch() is still open (dispatches enqueued but not
  /// committed yet).
  bool batch_open() const;

  /// \name Shared burst: the dispatch is appended to the command buffer that the previous stages
  /// of the same demodulation opened, with a memory barrier at the pipeline change.
  ///@{
  bool enqueue_burst(const void* symbols, const void* noise_var, void* llrs, unsigned nof_symbols, unsigned mod);

  /// \brief Accumulates one OFDM symbol of a group instead of dispatching it.
  ///
  /// The deferred chain submits one symbol per call, and one dispatch per symbol costs about 10us on
  /// this hardware while the kernel work of one 25 PRB symbol is a couple of microseconds. The
  /// symbols are accumulated here and handed over through the shared burst's flush hook (exactly
  /// like the equalizer's group), which encodes a run of symbols sharing the modulation, the element
  /// count and the per-symbol array strides as ONE dispatch - the equalized symbols of a group live
  /// in page-aligned per-symbol slots, and the kernel is told those strides, so nothing is staged.
  ///
  /// The caller's buffers must stay alive and unchanged until the burst is committed and waited for,
  /// the same contract the per-symbol encoding has.
  bool enqueue_burst_deferred(const void* symbols, const void* noise_var, void* llrs, unsigned nof_re, unsigned mod);

  /// True when the thread-local burst has dispatches encoded but not committed yet.
  static bool burst_open();

  /// Commits the thread-local burst without waiting.
  static bool burst_commit();

  /// Waits for the command buffers committed through the thread-local burst.
  static bool burst_wait_committed();
  ///@}

  /// \brief Commits the batch without waiting (pairs with wait_committed()).
  bool commit_batch();

  /// \brief Waits for the batch committed last (no-op when nothing is pending).
  bool wait_committed();
  ///@}

  /// GPU-side duration of the last call in microseconds (0 when unavailable).
  double last_gpu_wait_us() const;

private:
  void* impl = nullptr;
};

} // namespace metal
} // namespace ocudu
