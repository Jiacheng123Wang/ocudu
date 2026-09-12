// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Cost probe for the Metal equalizer path (S-1b.2 design input).
///
/// Part 1 measures dispatch patterns on the engine: A) one command buffer with an immediate
/// wait per dispatch (the pipeline pattern) against B) N dispatches in one command buffer
/// with one wait (the S2 pattern).
/// Part 2 measures the adapter (channel_equalizer_metal, the real entry point) with staging
/// outputs versus page-aligned outputs that the kernel writes in place, at a realistic
/// 1 x 1 / 1272-RE payload (override with OCUDU_PROBE_RE).

#include "../channel_equalizer_metal.h"
#include "ocudu_equalizer_metal_engine.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include "ocudu/support/macos_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

struct aligned_buffer {
  void*  ptr = nullptr;
  size_t cap = 0;

  void allocate(size_t bytes)
  {
    cap = ((bytes + compat::page_size() - 1) / compat::page_size()) * compat::page_size();
    ptr = compat::aligned_alloc(compat::page_size(), cap);
    std::memset(ptr, 0, cap);
  }
  ~aligned_buffer() { compat::aligned_free(ptr); }
};

double us_since(const std::chrono::steady_clock::time_point& t0)
{
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main()
{
  const char*    re_env = std::getenv("OCUDU_PROBE_RE");
  const unsigned nof_re = (re_env != nullptr) ? static_cast<unsigned>(std::strtoul(re_env, nullptr, 10)) : 1272;
  const unsigned ports  = 1;
  const unsigned layers = 1;

  std::mt19937                      rng(20260912);
  std::normal_distribution<float>   dist(0.0F, 0.01F);
  std::vector<std::vector<cbf16_t>> h(ports * layers, std::vector<cbf16_t>(nof_re));
  std::vector<std::vector<cbf16_t>> y(ports, std::vector<cbf16_t>(nof_re));
  for (auto& slice : h) {
    for (auto& v : slice) {
      v = cbf16_t(dist(rng), dist(rng));
    }
  }
  for (auto& slice : y) {
    for (auto& v : slice) {
      v = cbf16_t(dist(rng), dist(rng));
    }
  }
  std::vector<float> nv_est(ports, 0.01F);

  modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re);
  for (unsigned p = 0; p != ports; ++p) {
    ch_symbols.set_slice(p, y[p]);
  }
  modular_ch_est_list<8 * 4> ch_est(nof_re, ports, layers);
  for (unsigned p = 0; p != ports; ++p) {
    for (unsigned l = 0; l != layers; ++l) {
      ch_est.set_channel(h[p * layers + l], p, l);
    }
  }

  const size_t eq_bytes = static_cast<size_t>(layers) * nof_re * 2 * sizeof(float);
  const size_t nv_bytes = static_cast<size_t>(layers) * nof_re * sizeof(float);

  // Plain (heap) outputs -> staging path.
  std::vector<cf_t>  eq_staged(layers * nof_re);
  std::vector<float> nv_staged(layers * nof_re);
  // Page-aligned outputs -> in-place (zero-copy) path.
  aligned_buffer eq_direct;
  aligned_buffer nv_direct;
  eq_direct.allocate(eq_bytes);
  nv_direct.allocate(nv_bytes);
  span<cf_t>  eq_direct_span(static_cast<cf_t*>(eq_direct.ptr), layers * nof_re);
  span<float> nv_direct_span(static_cast<float*>(nv_direct.ptr), layers * nof_re);

  channel_equalizer_metal eq(false);

  constexpr unsigned iters = 400;
  for (unsigned i = 0; i != 20; ++i) { // warm-up (buffer wraps, pipeline residency)
    eq.equalize(eq_staged, nv_staged, ch_symbols, ch_est, nv_est, 1.0F);
    eq.equalize(eq_direct_span, nv_direct_span, ch_symbols, ch_est, nv_est, 1.0F);
  }

  // ---- Deferred-wait pattern on the engine: one command buffer per dispatch, committed
  // back to back, with a single wait at the end of a burst. This separates the per-commit
  // cost from the per-wait cost and decides whether S2 needs one giant command buffer or
  // only deferred waits.
  {
    constexpr unsigned              max_burst = 16;
    metal::equalizer_metal_engine   eng;
    if (!eng.init()) {
      std::fprintf(stderr, "FAIL: engine init\n");
      return 1;
    }
    std::vector<std::unique_ptr<aligned_buffer>> hb(max_burst);
    std::vector<std::unique_ptr<aligned_buffer>> yb(max_burst);
    std::vector<std::unique_ptr<aligned_buffer>> sb(max_burst);
    std::vector<std::unique_ptr<aligned_buffer>> eb(max_burst);
    std::vector<std::unique_ptr<aligned_buffer>> nb(max_burst);
    for (unsigned i = 0; i != max_burst; ++i) {
      hb[i] = std::make_unique<aligned_buffer>();
      yb[i] = std::make_unique<aligned_buffer>();
      sb[i] = std::make_unique<aligned_buffer>();
      eb[i] = std::make_unique<aligned_buffer>();
      nb[i] = std::make_unique<aligned_buffer>();
      hb[i]->allocate(static_cast<size_t>(ports) * layers * nof_re * sizeof(cbf16_t) + 4096);
      yb[i]->allocate(static_cast<size_t>(ports) * nof_re * sizeof(cbf16_t) + 4096);
      sb[i]->allocate(ports * sizeof(float) + 4096);
      eb[i]->allocate(static_cast<size_t>(layers) * nof_re * 2 * sizeof(float) + 4096);
      nb[i]->allocate(static_cast<size_t>(layers) * nof_re * sizeof(float) + 4096);
      std::memcpy(hb[i]->ptr, h[0].data(), nof_re * sizeof(cbf16_t));
      std::memcpy(yb[i]->ptr, y[0].data(), nof_re * sizeof(cbf16_t));
      static_cast<float*>(sb[i]->ptr)[0] = 0.01F;
    }
    for (unsigned i = 0; i != 8; ++i) { // warm-up
      eng.equalize(hb[0]->ptr, yb[0]->ptr, sb[0]->ptr, eb[0]->ptr, nb[0]->ptr, nof_re, ports, layers, false, 0.01F,
                   1.0F, 1.0F);
    }
    std::printf("\n[engine dispatch patterns, %ux%u, nof_re=%u]\n", ports, layers, nof_re);
    for (unsigned burst : {4u, 16u}) {
      std::vector<double> samples;
      constexpr unsigned  rounds = 40;
      for (unsigned r = 0; r != rounds; ++r) {
        auto t = std::chrono::steady_clock::now();
        for (unsigned k = 0; k != burst; ++k) {
          eng.begin_batch();
          eng.enqueue(hb[k]->ptr, yb[k]->ptr, sb[k]->ptr, eb[k]->ptr, nb[k]->ptr, nof_re, ports, layers, false,
                      0.01F, 1.0F, 1.0F);
          eng.commit_batch();
        }
        eng.wait_committed();
        samples.push_back(us_since(t) / burst);
      }
      std::sort(samples.begin(), samples.end());
      std::printf("  burst %2u: commit-per-dispatch, 1 wait  : min %7.1f  p50 %7.1f us/dispatch\n",
                  burst, samples.front(), samples[samples.size() / 2]);
    }
    {
      std::vector<double> samples;
      constexpr unsigned  rounds = 400;
      for (unsigned r = 0; r != rounds; ++r) {
        auto t = std::chrono::steady_clock::now();
        eng.equalize(hb[0]->ptr, yb[0]->ptr, sb[0]->ptr, eb[0]->ptr, nb[0]->ptr, nof_re, ports, layers, false, 0.01F,
                     1.0F, 1.0F);
        samples.push_back(us_since(t));
      }
      std::sort(samples.begin(), samples.end());
      std::printf("  commit+wait per dispatch (sync)        : min %7.1f  p50 %7.1f us/dispatch\n",
                  samples.front(), samples[samples.size() / 2]);
    }
    {
      std::vector<double> samples;
      constexpr unsigned  rounds = 40;
      constexpr unsigned  burst  = 16;
      for (unsigned r = 0; r != rounds; ++r) {
        auto t = std::chrono::steady_clock::now();
        eng.begin_batch();
        for (unsigned k = 0; k != burst; ++k) {
          eng.enqueue(hb[k]->ptr, yb[k]->ptr, sb[k]->ptr, eb[k]->ptr, nb[k]->ptr, nof_re, ports, layers, false,
                      0.01F, 1.0F, 1.0F);
        }
        eng.flush_batch();
        samples.push_back(us_since(t) / burst);
      }
      std::sort(samples.begin(), samples.end());
      std::printf("  burst 16 in ONE command buffer         : min %7.1f  p50 %7.1f us/dispatch\n",
                  samples.front(), samples[samples.size() / 2]);
    }
  }

  std::printf("\n[adapter, %ux%u, nof_re=%u]\n", ports, layers, nof_re);

  // The host dispatch round trip is noisy (virtualised GPU), so report the minimum and the
  // median on top of the mean: the minimum isolates the CPU-side cost of each variant.
  auto measure = [&](bool direct) {
    std::vector<double> samples;
    samples.reserve(iters);
    for (unsigned i = 0; i != iters; ++i) {
      auto t = std::chrono::steady_clock::now();
      if (direct) {
        eq.equalize(eq_direct_span, nv_direct_span, ch_symbols, ch_est, nv_est, 1.0F);
      } else {
        eq.equalize(eq_staged, nv_staged, ch_symbols, ch_est, nv_est, 1.0F);
      }
      samples.push_back(us_since(t));
    }
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (double v : samples) {
      sum += v;
    }
    std::array<double, 3> out{samples.front(), samples[samples.size() / 2], sum / samples.size()};
    return out;
  };

  const auto staged = measure(false);
  const auto direct = measure(true);

  std::printf("  staging outputs (heap)  : min %7.1f  p50 %7.1f  mean %7.1f us/call\n", staged[0], staged[1], staged[2]);
  std::printf("  page-aligned (in-place) : min %7.1f  p50 %7.1f  mean %7.1f us/call\n", direct[0], direct[1], direct[2]);
  std::printf("  CPU-side delta (min)    : %+.1f us/call  (%.0f%%)\n",
              direct[0] - staged[0],
              100.0 * (direct[0] - staged[0]) / staged[0]);
  std::printf("  gpu-side last call      : %8.1f us\n", eq.engine_gpu_wait_us());

  // Correctness: both paths must produce the same equalized symbols and noise variances.
  std::vector<cf_t>  eq_check(layers * nof_re);
  std::vector<float> nv_check(layers * nof_re);
  eq.equalize(eq_check, nv_check, ch_symbols, ch_est, nv_est, 1.0F);
  const bool eq_same = (std::memcmp(eq_check.data(), eq_direct.ptr, eq_bytes) == 0);
  const bool nv_same = (std::memcmp(nv_check.data(), nv_direct.ptr, nv_bytes) == 0);
  std::printf("  zero-copy == staging    : eq %s, nv %s\n", eq_same ? "OK" : "MISMATCH", nv_same ? "OK" : "MISMATCH");

  return (eq_same && nv_same) ? 0 : 1;
}
