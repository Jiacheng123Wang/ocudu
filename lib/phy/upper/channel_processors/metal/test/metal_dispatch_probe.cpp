// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Dispatch-pattern probe for the Metal equalizer engine (S2 design input).
///
/// Measures, on the same kernel and payload, the cost of
///   A) one dispatch per command buffer with an immediate wait (the current pipeline
///      pattern: one round trip per per-symbol call), and
///   B) N dispatches accumulated in a single command buffer with one wait (the S2
///      single-command-buffer pattern),
/// for several batch sizes. The comparison separates the commit/wait round-trip cost from
/// the in-command-buffer dispatch cost, which decides whether batching command buffers (S2)
/// or fusing kernels is the lever that matters.

#include "ocudu_equalizer_metal_engine.h"
#include "ocudu/support/macos_compat.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

struct buffer_set {
  void* h    = nullptr;
  void* y    = nullptr;
  void* s2   = nullptr;
  void* eq   = nullptr;
  void* nv   = nullptr;
  size_t cap = 0;

  void allocate(size_t bytes)
  {
    const size_t page = compat::page_size();
    cap               = ((bytes + page - 1) / page) * page;
    h                 = compat::aligned_alloc(page, cap);
    y                 = compat::aligned_alloc(page, cap);
    s2                = compat::aligned_alloc(page, cap);
    eq                = compat::aligned_alloc(page, cap);
    nv                = compat::aligned_alloc(page, cap);
    std::memset(h, 0, cap);
    std::memset(y, 0, cap);
    std::memset(s2, 0, cap);
    std::memset(eq, 0, cap);
    std::memset(nv, 0, cap);
  }

  ~buffer_set()
  {
    compat::aligned_free(h);
    compat::aligned_free(y);
    compat::aligned_free(s2);
    compat::aligned_free(eq);
    compat::aligned_free(nv);
  }
};

double us_since(const std::chrono::steady_clock::time_point& t0)
{
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main()
{
  constexpr unsigned max_batch = 16;
  const unsigned     ports     = 1;
  const unsigned     layers    = 1;

  metal::equalizer_metal_engine engine;
  if (!engine.init()) {
    std::fprintf(stderr, "FAIL: engine init\n");
    return 1;
  }

  for (unsigned nof_re : {1u, 128u}) {
    // Per-batch-size buffer sets so that batched dispatches never share memory.
    std::vector<std::unique_ptr<buffer_set>> sets(max_batch);
    std::mt19937                            rng(20260912);
    std::normal_distribution<float>          dist(0.0F, 0.01F);
    for (auto& set : sets) {
      set = std::make_unique<buffer_set>();
      set->allocate(static_cast<size_t>(ports) * layers * nof_re * 2 * sizeof(float) + 4096);
      auto* h  = static_cast<float*>(set->h);
      auto* y  = static_cast<float*>(set->y);
      auto* s2 = static_cast<float*>(set->s2);
      for (unsigned i = 0; i != ports * layers * nof_re * 2; ++i) {
        h[i] = dist(rng);
      }
      for (unsigned i = 0; i != ports * nof_re * 2; ++i) {
        y[i] = dist(rng);
      }
      for (unsigned i = 0; i != ports; ++i) {
        s2[i] = 0.01F;
      }
    }

    std::printf("\n[nof_re=%u, %ux%u topology]\n", nof_re, ports, layers);
    std::printf("%6s | %11s | %11s | %9s | %9s | %9s | %9s | %9s\n",
                "N",
                "A total us",
                "B total us",
                "A us/disp",
                "B us/disp",
                "B encode",
                "B flush",
                "B gpu us");
    for (unsigned n : {1u, 2u, 4u, 8u, 16u}) {
      // Warm-up (first touch of each buffer set, pipeline residency).
      for (unsigned i = 0; i != n; ++i) {
        engine.equalize(sets[i]->h, sets[i]->y, sets[i]->s2, sets[i]->eq, sets[i]->nv, nof_re, ports, layers,
                        false, 0.01F, 1.0F);
      }

      // Pattern A: one command buffer (and one wait) per dispatch.
      auto  t0 = std::chrono::steady_clock::now();
      float a_gpu = 0.0F;
      for (unsigned i = 0; i != n; ++i) {
        engine.equalize(sets[i]->h, sets[i]->y, sets[i]->s2, sets[i]->eq, sets[i]->nv, nof_re, ports, layers,
                        false, 0.01F, 1.0F);
        a_gpu += static_cast<float>(engine.last_gpu_wait_us());
      }
      const double a_us = us_since(t0);

      // Pattern B: all dispatches in one command buffer, one wait (encode + flush measured).
      engine.begin_batch();
      auto t1 = std::chrono::steady_clock::now();
      for (unsigned i = 0; i != n; ++i) {
        engine.enqueue(sets[i]->h, sets[i]->y, sets[i]->s2, sets[i]->eq, sets[i]->nv, nof_re, ports, layers,
                       false, 0.01F, 1.0F);
      }
      auto t2 = std::chrono::steady_clock::now();
      engine.flush_batch();
      auto         t3   = std::chrono::steady_clock::now();
      const double b_enc = std::chrono::duration<double, std::micro>(t2 - t1).count();
      const double b_flush = std::chrono::duration<double, std::micro>(t3 - t2).count();
      const double b_us    = b_enc + b_flush;
      const double b_gpu   = engine.last_gpu_wait_us();

      std::printf("%6u | %11.1f | %11.1f | %9.1f | %9.1f | %9.1f | %9.1f | %9.1f\n",
                  n,
                  a_us,
                  b_us,
                  a_us / n,
                  b_us / n,
                  b_enc,
                  b_flush,
                  b_gpu);
      std::printf("%6s | %11s | %11s | %9s | %9s | %9s | %9s | %9.1f\n", "", "A gpu-sum", "", "", "", "", "", a_gpu);
    }
  }

  return 0;
}
