// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU channel equalizer unit test: random-channel A/B comparison against the
/// CPU generic implementation (ZF and MMSE, multi-layer topologies), the composite-factory
/// fallback semantics, and steady-state latency prints.

#include "../channel_equalizer_metal.h"
#include "../channel_equalizer_metal_factory.h"
#include "channel_equalizer_generic_impl.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/adt/format.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

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

double max_rel_err(span<const float> x, span<const float> y)
{
  double worst = 0.0;
  for (unsigned i = 0; i != x.size(); ++i) {
    if (std::isinf(x[i])) {
      continue;
    }
    worst = std::max(worst, static_cast<double>(std::abs(x[i] - y[i]) / std::max(std::abs(x[i]), 1e-6F)));
  }
  return worst;
}

struct topology {
  unsigned ports;
  unsigned layers;
};

bool run_topology(const topology& topo, channel_equalizer_algorithm_type algo, std::mt19937& rng,
                  float tx_scaling = 1.0F)
{
  const unsigned nof_re = 128;
  auto           dist   = std::normal_distribution<float>(0.0F, 0.01F);

  // Random channel, symbols and per-port noise variances.
  std::vector<std::vector<cbf16_t>> h(topo.ports * topo.layers, std::vector<cbf16_t>(nof_re));
  std::vector<std::vector<cbf16_t>> y(topo.ports, std::vector<cbf16_t>(nof_re));
  std::vector<float>                nv_est(topo.ports);
  for (auto& hp : h) {
    for (auto& v : hp) {
      v = cbf16_t(dist(rng), dist(rng));
    }
  }
  for (auto& yp : y) {
    for (auto& v : yp) {
      v = cbf16_t(dist(rng), dist(rng));
    }
  }
  for (auto& n : nv_est) {
    n = 0.01F + 0.001F * std::abs(dist(rng));
  }

  modular_re_buffer_reader<cbf16_t, 8> ch_symbols(topo.ports, nof_re);
  for (unsigned p = 0; p != topo.ports; ++p) {
    ch_symbols.set_slice(p, y[p]);
  }
  modular_ch_est_list<8 * 4> ch_est(nof_re, topo.ports, topo.layers);
  for (unsigned p = 0; p != topo.ports; ++p) {
    for (unsigned l = 0; l != topo.layers; ++l) {
      ch_est.set_channel(h[p * topo.layers + l], p, l);
    }
  }

  std::vector<cf_t>  eq_metal(nof_re * topo.layers);
  std::vector<float> nv_metal(nof_re * topo.layers);
  std::vector<cf_t>  eq_ref(nof_re * topo.layers);
  std::vector<float> nv_ref(nof_re * topo.layers);

  channel_equalizer_metal        metal(algo == channel_equalizer_algorithm_type::mmse);
  channel_equalizer_generic_impl ref(algo);

  if (!metal.is_supported(topo.ports, topo.layers)) {
    std::fprintf(stderr, "FAIL: metal is_supported(%u,%u) returned false\n", topo.ports, topo.layers);
    return false;
  }
  metal.equalize(eq_metal, nv_metal, ch_symbols, ch_est, nv_est, tx_scaling);
  ref.equalize(eq_ref, nv_ref, ch_symbols, ch_est, nv_est, tx_scaling);

  const double nmse = nmse_db(span<const cf_t>(eq_ref), span<const cf_t>(eq_metal));
  const double nver = max_rel_err(span<const float>(nv_ref), span<const float>(nv_metal));
  std::printf("[A/B] %ux%u %-4s nmse=%8.2f dB nv_max_rel_err=%.2e", topo.ports, topo.layers,
              algo == channel_equalizer_algorithm_type::zf ? "zf" : "mmse", nmse, nver);

  // Noise-variance gate: the CPU and GPU pipelines are both float32; on ill-conditioned
  // square ZF topologies (4x4) the Gram inverse diagonal is amplified by the condition
  // number, so cross-ISA ulp differences reach ~1e-4. Allow 1e-3 (0.1%), far below any
  // LLR impact. Symbols keep the tight -60 dB gate.
  if (nmse > -60.0 || nver > 1e-3) {
    std::printf("  -> FAIL (gates: nmse <= -60 dB, nv rel err <= 1e-3)\n");
    return false;
  }
  std::printf("  -> OK\n");
  return true;
}

} // namespace

int main()
{
  std::mt19937 rng(20260912);
  bool         ok = true;

  const topology topos[] = {{2, 2}, {4, 2}, {4, 3}, {4, 4}, {8, 2}, {8, 3}, {8, 4}};
  for (const auto& topo : topos) {
    ok = run_topology(topo, channel_equalizer_algorithm_type::zf, rng) && ok;
    ok = run_topology(topo, channel_equalizer_algorithm_type::mmse, rng) && ok;
  }

  // Tx-scaling consistency: the host applies tx_scaling to H while the CPU's dedicated
  // 2-layer path folds it into the determinant - both must agree for scaling != 1.
  ok = run_topology({2, 2}, channel_equalizer_algorithm_type::zf, rng, 2.0F) && ok;
  ok = run_topology({2, 2}, channel_equalizer_algorithm_type::mmse, rng, 2.0F) && ok;
  ok = run_topology({4, 4}, channel_equalizer_algorithm_type::zf, rng, 0.5F) && ok;
  ok = run_topology({4, 4}, channel_equalizer_algorithm_type::mmse, rng, 0.5F) && ok;

  // Composite-factory fallback: single-layer topologies stay on the CPU implementation
  // (the factory must never reject them).
  {
    auto factory = create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::zf);
    if (factory == nullptr) {
      std::fprintf(stderr, "FAIL: metal factory unavailable\n");
      return 1;
    }
    auto eq = factory->create();
    if (eq == nullptr || !eq->is_supported(1, 1) || !eq->is_supported(2, 2)) {
      std::fprintf(stderr, "FAIL: composite factory acceptance set broken\n");
      ok = false;
    } else {
      std::printf("[size] composite factory fallback OK (1x1 stays CPU, 2x2 supported)\n");
    }
  }

  // Steady-state latency (audit data): 100 calls per backend at 4x4.
  {
    const unsigned nof_re = 128;
    std::normal_distribution<float> dist(0.0F, 0.01F);
    std::vector<std::vector<cbf16_t>> h(16, std::vector<cbf16_t>(nof_re));
    std::vector<std::vector<cbf16_t>> y(4, std::vector<cbf16_t>(nof_re));
    std::vector<float>                nv_est(4, 0.01F);
    for (auto& hp : h) {
      for (auto& v : hp) {
        v = cbf16_t(dist(rng), dist(rng));
      }
    }
    for (auto& yp : y) {
      for (auto& v : yp) {
        v = cbf16_t(dist(rng), dist(rng));
      }
    }
    modular_re_buffer_reader<cbf16_t, 8> ch_symbols(4, nof_re);
    for (unsigned p = 0; p != 4; ++p) {
      ch_symbols.set_slice(p, y[p]);
    }
    modular_ch_est_list<32> ch_est(nof_re, 4, 4);
    for (unsigned p = 0; p != 4; ++p) {
      for (unsigned l = 0; l != 4; ++l) {
        ch_est.set_channel(h[p * 4 + l], p, l);
      }
    }
    std::vector<cf_t>  eq(nof_re * 4);
    std::vector<float> nv(nof_re * 4);

    channel_equalizer_metal        metal(false);
    channel_equalizer_generic_impl ref(channel_equalizer_algorithm_type::zf);
    constexpr unsigned             iters = 100;
    const auto                     t0    = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      metal.equalize(eq, nv, ch_symbols, ch_est, nv_est, 1.0F);
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      ref.equalize(eq, nv, ch_symbols, ch_est, nv_est, 1.0F);
    }
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("[time] 4x4 zf metal=%.1fus cpu=%.1fus (gpu-only last: %.1fus)\n",
                std::chrono::duration<double, std::micro>(t1 - t0).count() / iters,
                std::chrono::duration<double, std::micro>(t2 - t1).count() / iters,
                metal.engine_gpu_wait_us());
  }

  if (ok) {
    std::printf("ALL OK\n");
    return 0;
  }
  std::fprintf(stderr, "FAILED\n");
  return 1;
}
