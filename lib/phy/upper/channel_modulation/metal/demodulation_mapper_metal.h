// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU soft demapper (QPSK, 16/64/256 QAM) behind the demodulation_mapper
/// interface. The per-call staging copies the equalized symbols, noise variances and the
/// output LLRs through page-aligned reusable buffers - the zero-copy boundaries arrive with
/// the chained pipeline work. BPSK and pi/2-BPSK keep the CPU implementation.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/upper/channel_modulation/demodulation_mapper.h"
#include <memory>

namespace ocudu {

class demodulation_mapper_metal : public demodulation_mapper
{
public:
  demodulation_mapper_metal();
  ~demodulation_mapper_metal() override;

  demodulation_mapper_metal(const demodulation_mapper_metal&)            = delete;
  demodulation_mapper_metal& operator=(const demodulation_mapper_metal&) = delete;

  /// True when the engine is available and the scheme is handled by the Metal kernel.
  bool is_supported(modulation_scheme mod) const;

  // See interface for documentation.
  void demodulate_soft(span<log_likelihood_ratio> llrs,
                       span<const cf_t>           symbols,
                       span<const float>          noise_vars,
                       modulation_scheme          mod) override;

  // See interface for documentation.
  void submit(span<log_likelihood_ratio> llrs,
              span<const cf_t>           symbols,
              span<const float>          noise_vars,
              modulation_scheme          mod) override;

  // See interface for documentation.
  void wait() override;

  // See interface for documentation.
  bool supports_deferred_chain() const override { return true; }

  /// GPU-side duration of the last call in microseconds (0 when unavailable / invalid).
  double engine_gpu_wait_us() const;

private:
  /// \brief Page-aligned staging buffer, grown on demand and kept across calls.
  ///
  /// One instance per in-flight dispatch: a deferred submit is committed without waiting, so the
  /// buffers it hands to the kernel must stay untouched until its command buffer completes.
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

  /// \brief One submit() awaiting wait(): where the staged LLRs must be copied back, plus the
  /// inputs it staged and that the kernel reads until its command buffer completes.
  struct pending_entry {
    span<log_likelihood_ratio> llrs       = {};
    void*                      llr_ptr    = nullptr;
    size_t                     llr_sz     = 0;
    bool                       llr_direct = false;
    staging                    sym;
    staging                    nv;
    staging                    llr;
  };

  /// \brief Shared implementation of demodulate_soft() and submit().
  void run_demodulate(span<log_likelihood_ratio> llrs,
                      span<const cf_t>           symbols,
                      span<const float>          noise_vars,
                      modulation_scheme          mod,
                      bool                       defer);

  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace ocudu
