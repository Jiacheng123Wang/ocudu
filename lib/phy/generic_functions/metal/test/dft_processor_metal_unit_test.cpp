// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU DFT unit test: random-IQ A/B comparison against the CPU reference
/// (FFTZ when available, the generic DFT otherwise) with an NMSE gate, a direct/inverse
/// roundtrip check, the per-size factory fallback semantics, and steady-state latency
/// prints for both backends.
///
/// The A/B checks construct dft_processor_metal directly (the factory may fall back to
/// the CPU implementation per size, and the build has no RTTI to type-test its output).

#include "../dft_processor_metal.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

/// NMSE in dB between two complex vectors: 10*log10(sum|x-y|^2 / sum|x|^2).
double nmse_db(span<const cf_t> x, span<const cf_t> y)
{
  double num = 0.0;
  double den = 0.0;
  for (unsigned i = 0; i != x.size(); ++i) {
    num += std::norm(x[i] - y[i]);
    den += std::norm(x[i]);
  }
  if (den == 0.0) {
    return num == 0.0 ? -300.0 : 300.0;
  }
  return 10.0 * std::log10(num / den);
}

/// Maximum relative roundtrip error: |IDFT(DFT(x)) - N*x| / (N*|x|), element-wise max.
double roundtrip_max_rel_err(span<const cf_t> x, span<const cf_t> y, unsigned size)
{
  double worst = 0.0;
  for (unsigned i = 0; i != x.size(); ++i) {
    const cf_t  ref  = x[i] * static_cast<float>(size);
    const double err = std::abs(y[i] - ref);
    const double mag = std::abs(ref);
    if (mag > 0.0) {
      worst = std::max(worst, err / mag);
    }
  }
  return worst;
}

} // namespace

int main()
{
  std::mt19937                          rng(20260911);
  std::uniform_real_distribution<float> dist(-1000.0F, 1000.0F);

  // CPU reference factory: FFTZ first, the generic DFT otherwise.
  std::shared_ptr<dft_processor_factory> ref_factory = create_dft_processor_factory_fftz();
  const char*                            ref_name    = "fftz";
  if (ref_factory == nullptr) {
    ref_factory = create_dft_processor_factory_generic();
    ref_name    = "generic";
  }
  if (ref_factory == nullptr) {
    std::fprintf(stderr, "FAIL: no CPU reference DFT factory available\n");
    return 1;
  }
  std::printf("CPU reference: %s\n", ref_name);

  bool ok = true;

  // A/B comparison over the OFDM-relevant 2^k * 3^m sizes, both directions.
  const unsigned sizes[] = {128, 384, 512, 768, 1024, 1536, 2048, 3072, 4096};
  for (unsigned size : sizes) {
    for (auto dir : {dft_processor::direction::DIRECT, dft_processor::direction::INVERSE}) {
      const dft_processor::configuration config{size, dir};

      dft_processor_metal metal(config);
      if (!metal.is_valid()) {
        std::fprintf(stderr, "FAIL: Metal DFT processor invalid (size=%u dir=%s)\n",
                     size,
                     dft_processor::direction_to_string(dir).c_str());
        ok = false;
        continue;
      }
      auto ref = ref_factory->create(config);
      if (ref == nullptr) {
        std::fprintf(stderr, "FAIL: reference processor creation failed (size=%u dir=%s)\n",
                     size,
                     dft_processor::direction_to_string(dir).c_str());
        ok = false;
        continue;
      }

      // Random input, shared by both processors.
      std::vector<cf_t> input_copy(size);
      for (unsigned i = 0; i != size; ++i) {
        const cf_t sample{dist(rng), dist(rng)};
        metal.get_input()[i] = sample;
        ref->get_input()[i]   = sample;
        input_copy[i]         = sample;
      }

      const auto metal_out = metal.run();
      const auto ref_out   = ref->run();

      const double nmse = nmse_db(ref_out, metal_out);
      std::printf("[A/B] size=%4u dir=%-7s nmse=%8.2f dB", size, dft_processor::direction_to_string(dir).c_str(), nmse);
      if (nmse > -60.0) {
        std::printf("  -> FAIL (gate: <= -60 dB)\n");
        ok = false;
      } else {
        std::printf("  -> OK\n");
      }

      // Direct -> inverse roundtrip: y = IDFT(DFT(x)) ~= N*x (both unnormalized).
      if (dir == dft_processor::direction::DIRECT) {
        dft_processor_metal inv_metal({size, dft_processor::direction::INVERSE});
        if (!inv_metal.is_valid()) {
          std::fprintf(stderr, "FAIL: Metal inverse DFT processor invalid (size=%u)\n", size);
          ok = false;
          continue;
        }
        for (unsigned i = 0; i != size; ++i) {
          inv_metal.get_input()[i] = metal_out[i];
        }
        const auto   roundtrip   = inv_metal.run();
        const double max_rel_err = roundtrip_max_rel_err(span<const cf_t>(input_copy), roundtrip, size);
        std::printf("[RT ] size=%4u max_rel_err=%.3e", size, max_rel_err);
        if (max_rel_err > 1e-3) {
          std::printf("  -> FAIL (gate: <= 1e-3)\n");
          ok = false;
        } else {
          std::printf("  -> OK\n");
        }
      }
    }
  }

  // Unsupported-size semantics: the static predicate rejects sizes outside the 2^k*3^m
  // family (or beyond the kernel maximum), and the metal factory falls back transparently
  // (never returns null).
  {
    std::shared_ptr<dft_processor_factory> metal_factory = create_dft_processor_factory_metal();
    if (metal_factory == nullptr) {
      std::fprintf(stderr, "FAIL: Metal DFT factory unavailable (built without OCUDU_METAL_DFT?)\n");
      return 1;
    }
    for (unsigned bad : {1U, 5U, 1000U, 5000U, 8192U, 12288U}) {
      if (dft_processor_metal::is_supported_size(bad)) {
        std::fprintf(stderr, "FAIL: is_supported_size(%u) returned true\n", bad);
        ok = false;
      }
      auto fallback = metal_factory->create({bad, dft_processor::direction::DIRECT});
      if (fallback == nullptr || fallback->get_size() != bad) {
        std::fprintf(stderr, "FAIL: factory fallback broken for unsupported size %u\n", bad);
        ok = false;
      }
    }
    std::printf("[size] unsupported-size fallback OK\n");
  }

  // Steady-state latency (audit data): 100 runs per backend at the OFDM sizes.
  for (unsigned size : {512U, 768U, 1024U, 2048U}) {
    dft_processor_metal metal({size, dft_processor::direction::DIRECT});
    if (!metal.is_valid()) {
      continue;
    }
    auto ref = ref_factory->create({size, dft_processor::direction::DIRECT});

    constexpr unsigned iters = 100;
    const auto         t0    = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      (void)metal.run();
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      (void)ref->run();
    }
    const auto   t2       = std::chrono::steady_clock::now();
    const double metal_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / static_cast<double>(iters);
    const double ref_us   = std::chrono::duration<double, std::micro>(t2 - t1).count() / static_cast<double>(iters);
    std::printf("[time] size=%4u metal=%.1fus %s=%.1fus (gpu-only last: %.1fus)\n",
                size,
                metal_us,
                ref_name,
                ref_us,
                metal.engine_gpu_wait_us());
  }

  if (ok) {
    std::printf("ALL OK\n");
    return 0;
  }
  std::fprintf(stderr, "FAILED\n");
  return 1;
}
