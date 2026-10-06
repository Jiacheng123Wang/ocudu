// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU channel equalizer unit test: random-channel A/B comparison against the
/// CPU generic implementation (ZF and MMSE, multi-layer topologies), the composite-factory
/// fallback semantics, and steady-state latency prints.

#include "../channel_equalizer_metal.h"
#include "../channel_equalizer_metal_factory.h"
#include "demodulation_mapper_metal_factory.h"
#include "channel_equalizer_generic_impl.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/adt/format.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/channel_modulation/demodulation_mapper.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <random>
#include <string>
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

double rel_err(const float& x, const float& y)
{
  return static_cast<double>(std::abs(x - y) / std::max(std::abs(x), 1e-6F));
}

double max_rel_err(span<const float> x, span<const float> y)
{
  double worst = 0.0;
  for (unsigned i = 0; i != x.size(); ++i) {
    if (std::isinf(x[i])) {
      continue;
    }
    worst = std::max(worst, rel_err(x[i], y[i]));
  }
  return worst;
}

/// Number of RE/layer noise variances whose relative error exceeds the strict gate (used to
/// quantify the ill-conditioned ZF outliers instead of hiding them).
unsigned count_rel_err_above(span<const float> x, span<const float> y, double thr)
{
  unsigned count = 0;
  for (unsigned i = 0; i != x.size(); ++i) {
    if (!std::isinf(x[i]) && (rel_err(x[i], y[i]) > thr)) {
      ++count;
    }
  }
  return count;
}

struct topology {
  unsigned ports;
  unsigned layers;
};

/// RAII page-aligned region used to exercise the in-place (no staging) output path.
struct aligned_region {
  void*  ptr = nullptr;
  size_t cap = 0;

  explicit aligned_region(size_t bytes)
  {
    if (bytes == 0) {
      return;
    }
    const size_t page = compat::page_size();
    cap               = ((bytes + page - 1) / page) * page;
    ptr               = compat::aligned_alloc(page, cap);
  }
  ~aligned_region() { compat::aligned_free(ptr); }
  aligned_region(const aligned_region&)            = delete;
  aligned_region& operator=(const aligned_region&) = delete;
};

bool run_topology(const topology& topo,
                  channel_equalizer_algorithm_type algo,
                  std::mt19937&                  rng,
                  float                          tx_scaling       = 1.0F,
                  span<const float>              nv_override      = {},
                  bool                           aligned_outputs  = false)
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
  if (!nv_override.empty()) {
    ocudu_assert(nv_override.size() == nv_est.size(), "Invalid noise variance override size.");
    std::copy(nv_override.begin(), nv_override.end(), nv_est.begin());
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

  const unsigned     nof_eq = nof_re * topo.layers;
  std::vector<cf_t>  eq_staged(nof_eq);
  std::vector<float> nv_staged(nof_eq);
  aligned_region     eq_mem(aligned_outputs ? nof_eq * sizeof(cf_t) : 0);
  aligned_region     nv_mem(aligned_outputs ? nof_eq * sizeof(float) : 0);
  span<cf_t>  eq_metal = aligned_outputs ? span<cf_t>(static_cast<cf_t*>(eq_mem.ptr), nof_eq) : span<cf_t>(eq_staged);
  span<float> nv_metal =
      aligned_outputs ? span<float>(static_cast<float*>(nv_mem.ptr), nof_eq) : span<float>(nv_staged);
  std::vector<cf_t>  eq_ref(nof_eq);
  std::vector<float> nv_ref(nof_eq);

  channel_equalizer_metal        metal(algo == channel_equalizer_algorithm_type::mmse);
  channel_equalizer_generic_impl ref(algo);

  if (!metal.is_supported(topo.ports, topo.layers)) {
    std::fprintf(stderr, "FAIL: metal is_supported(%u,%u) returned false\n", topo.ports, topo.layers);
    return false;
  }
  metal.equalize(eq_metal, nv_metal, ch_symbols, ch_est, nv_est, tx_scaling);
  ref.equalize(eq_ref, nv_ref, ch_symbols, ch_est, nv_est, tx_scaling);

  const double nmse   = nmse_db(span<const cf_t>(eq_ref), span<const cf_t>(eq_metal));
  const double nver   = max_rel_err(span<const float>(nv_ref), span<const float>(nv_metal));
  const unsigned nover = count_rel_err_above(span<const float>(nv_ref), span<const float>(nv_metal), 1e-3);
  std::printf("[A/B] %ux%u %-4s %-9s nmse=%8.2f dB nv_max_rel_err=%.2e (re>1e-3: %u/%u)",
              topo.ports,
              topo.layers,
              algo == channel_equalizer_algorithm_type::zf ? "zf" : "mmse",
              aligned_outputs ? "zero-copy" : "staging",
              nmse,
              nver,
              nover,
              static_cast<unsigned>(nv_ref.size()));

  // Noise-variance gates. Both pipelines are float32: for the multi-layer path the Gram
  // inverse diagonal is amplified by the condition number of H^H*H, so on ill-conditioned
  // square ZF topologies (4x4) cross-ISA ulp differences reach ~1e-3 on the few near-singular
  // REs (the CPU itself deviates from a double-precision reference by the same order there).
  // Those topologies therefore use a 1e-2 gate; the symbols - the quantity that actually
  // carries the information - keep the tight -60 dB gate everywhere.
  const bool   ill_conditioned_square_zf =
      (topo.ports == topo.layers) && (topo.layers >= 3) && (algo == channel_equalizer_algorithm_type::zf);
  const double nv_gate = ill_conditioned_square_zf ? 1e-2 : 1e-3;
  if (nmse > -60.0 || nver > nv_gate) {
    std::printf("  -> FAIL (gates: nmse <= -60 dB, nv rel err <= %.0e)\n", nv_gate);
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

  const topology topos[] = {{1, 1}, {2, 1}, {4, 1}, {8, 1}, {2, 2}, {4, 2}, {4, 3}, {4, 4}, {8, 2}, {8, 3}, {8, 4}};
  for (const auto& topo : topos) {
    ok = run_topology(topo, channel_equalizer_algorithm_type::zf, rng) && ok;
    ok = run_topology(topo, channel_equalizer_algorithm_type::mmse, rng) && ok;
  }

  // Single-layer port reduction: ports with a non-positive or non-finite noise variance are
  // dropped before equalization (CPU semantics), including the all-invalid case.
  {
    const float nv_reduced[4] = {0.02F, 0.0F, std::numeric_limits<float>::quiet_NaN(), 0.05F};
    const float nv_inf[4]     = {0.02F, std::numeric_limits<float>::infinity(), 0.0F, -1.0F};
    const float nv_none[4]    = {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity()};
    ok = run_topology({4, 1}, channel_equalizer_algorithm_type::zf, rng, 1.0F, span<const float>(nv_reduced, 4)) && ok;
    ok = run_topology({4, 1}, channel_equalizer_algorithm_type::mmse, rng, 1.0F, span<const float>(nv_reduced, 4)) &&
         ok;
    ok = run_topology({4, 1}, channel_equalizer_algorithm_type::zf, rng, 2.0F, span<const float>(nv_inf, 4)) && ok;
    ok = run_topology({2, 1}, channel_equalizer_algorithm_type::zf, rng, 1.0F, span<const float>(nv_none, 2)) && ok;
    ok = run_topology({1, 1}, channel_equalizer_algorithm_type::mmse, rng, 1.0F, span<const float>(nv_none, 1)) && ok;
  }

  // In-place (page-aligned) outputs must be bit-identical to the staging path: same kernel,
  // the only difference is whether the results are copied back.
  ok = run_topology({1, 1}, channel_equalizer_algorithm_type::zf, rng, 1.0F, {}, true) && ok;
  ok = run_topology({4, 1}, channel_equalizer_algorithm_type::mmse, rng, 1.0F, {}, true) && ok;
  ok = run_topology({4, 4}, channel_equalizer_algorithm_type::zf, rng, 1.0F, {}, true) && ok;
  ok = run_topology({8, 3}, channel_equalizer_algorithm_type::mmse, rng, 2.0F, {}, true) && ok;

  // Tx-scaling consistency: the host applies tx_scaling to H while the CPU's dedicated
  // 2-layer path folds it into the determinant - both must agree for scaling != 1.
  ok = run_topology({2, 2}, channel_equalizer_algorithm_type::zf, rng, 2.0F) && ok;
  ok = run_topology({2, 2}, channel_equalizer_algorithm_type::mmse, rng, 2.0F) && ok;
  ok = run_topology({4, 4}, channel_equalizer_algorithm_type::zf, rng, 0.5F) && ok;
  ok = run_topology({4, 4}, channel_equalizer_algorithm_type::mmse, rng, 0.5F) && ok;
  // Single-layer tx_scaling: the CPU 1 x n path folds it into the pseudo-inverse denominator.
  ok = run_topology({4, 1}, channel_equalizer_algorithm_type::zf, rng, 2.0F) && ok;
  ok = run_topology({4, 1}, channel_equalizer_algorithm_type::mmse, rng, 0.25F) && ok;

  // Composite-factory fallback: single-layer topologies stay on the CPU implementation
  // (the factory must never reject them).
  {
    auto factory = create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::zf);
    if (factory == nullptr) {
      std::fprintf(stderr, "FAIL: metal factory unavailable\n");
      return 1;
    }
    auto eq = factory->create();
    if (eq == nullptr || !eq->is_supported(1, 1) || !eq->is_supported(2, 1) || !eq->is_supported(2, 2) ||
        eq->is_supported(16, 1)) {
      std::fprintf(stderr, "FAIL: composite factory acceptance set broken\n");
      ok = false;
    } else {
      std::printf("[size] composite factory acceptance set OK (1x1..8x4 supported, 16 ports rejected)\n");
    }
  }

  // Deferred chain (A-1): submitting the equalization without waiting and synchronizing later
  // through wait() must be bit-identical to the synchronous call.
  {
    const unsigned nof_re   = 128;
    const unsigned ports    = 1;
    const unsigned layers   = 1;
    auto           dist     = std::normal_distribution<float>(0.0F, 0.01F);
    std::vector<std::vector<cbf16_t>> h(ports * layers, std::vector<cbf16_t>(nof_re));
    std::vector<std::vector<cbf16_t>> y(ports, std::vector<cbf16_t>(nof_re));
    std::vector<float>                nv_est(ports, 0.01F);
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
    modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re);
    for (unsigned p = 0; p != ports; ++p) {
      ch_symbols.set_slice(p, y[p]);
    }
    modular_ch_est_list<8 * 4> ch_est(nof_re, ports, layers);
    ch_est.set_channel(h[0], 0, 0);

    std::vector<cf_t>  eq_sync(nof_re * layers);
    std::vector<float> nv_sync(nof_re * layers);
    std::vector<cf_t>  eq_deferred(nof_re * layers);
    std::vector<float> nv_deferred(nof_re * layers);

    channel_equalizer_metal metal(false);
    if (!metal.supports_deferred_chain()) {
      std::fprintf(stderr, "FAIL: deferred chain not advertised\n");
      ok = false;
    }
    metal.equalize(eq_sync, nv_sync, ch_symbols, ch_est, nv_est, 1.0F);
    metal.submit(eq_deferred, nv_deferred, ch_symbols, ch_est, nv_est, 1.0F);
    metal.wait();

    const bool same = (std::memcmp(eq_sync.data(), eq_deferred.data(), nof_re * layers * sizeof(cf_t)) == 0) &&
                      (std::memcmp(nv_sync.data(), nv_deferred.data(), nof_re * layers * sizeof(float)) == 0);
    std::printf("[chain]  submit()+wait() bit-identical to equalize(): %s\n", same ? "OK" : "MISMATCH");
    if (!same) {
      std::fprintf(stderr, "FAIL: deferred equalization differs from the synchronous path\n");
      ok = false;
    }
  }

  // Deferred chain with several submits in flight (A-2): the whole burst is committed before a
  // single wait, so every submit must keep its own staged inputs and outputs. Distinct inputs per
  // submit catch a shared staging buffer.
  {
    const unsigned nof_re = 128;
    const unsigned ports  = 2;
    const unsigned layers = 2;
    const unsigned nof_submits = 5;
    std::normal_distribution<float> dist(0.0F, 0.01F);

    std::vector<cf_t>  eq_sync(nof_re * layers * nof_submits);
    std::vector<float> nv_sync(nof_re * layers * nof_submits);
    std::vector<cf_t>  eq_deferred(nof_re * layers * nof_submits);
    std::vector<float> nv_deferred(nof_re * layers * nof_submits);

    channel_equalizer_metal metal(false);
    if (!metal.supports_deferred_chain()) {
      std::fprintf(stderr, "FAIL: deferred chain not advertised (burst)\n");
      ok = false;
    }

    // Each submit uses a different channel realization and noise variance, so a shared staging
    // buffer would make the earlier outputs match the last one.
    std::vector<std::vector<std::vector<cbf16_t>>> h(nof_submits,
                                                     std::vector<std::vector<cbf16_t>>(ports * layers,
                                                                                       std::vector<cbf16_t>(nof_re)));
    std::vector<std::vector<std::vector<cbf16_t>>> y(nof_submits,
                                                     std::vector<std::vector<cbf16_t>>(ports,
                                                                                       std::vector<cbf16_t>(nof_re)));
    std::vector<std::vector<float>> nv_est(nof_submits, std::vector<float>(ports));
    for (unsigned s = 0; s != nof_submits; ++s) {
      for (auto& slice : h[s]) {
        for (auto& v : slice) {
          v = cbf16_t(dist(rng), dist(rng));
        }
      }
      for (auto& slice : y[s]) {
        for (auto& v : slice) {
          v = cbf16_t(dist(rng), dist(rng));
        }
      }
      for (unsigned p = 0; p != ports; ++p) {
        nv_est[s][p] = 0.01F * static_cast<float>(s + 1);
      }
    }

    // Two destination layouts: page-aligned per-submit regions (what the PUSCH demodulator
    // allocates, so the kernel writes them in place) and plain vectors (staged outputs copied back
    // at wait()). Both must survive a burst: the staged path needs one staging buffer per submit.
    for (unsigned variant = 0; variant != 2; ++variant) {
      const bool        in_place = (variant == 0);
      const size_t      page     = compat::page_size();
      const size_t      eq_bytes = ((nof_re * layers * sizeof(cf_t) + page - 1) / page) * page;
      const size_t      nv_bytes = ((nof_re * layers * sizeof(float) + page - 1) / page) * page;

      std::vector<cf_t>  eq_plain(in_place ? 0 : nof_re * layers * nof_submits);
      std::vector<float> nv_plain(in_place ? 0 : nof_re * layers * nof_submits);
      auto*              eq_page = in_place ? static_cast<cf_t*>(compat::aligned_alloc(page, eq_bytes * nof_submits))
                                            : nullptr;
      auto*              nv_page = in_place ? static_cast<float*>(compat::aligned_alloc(page, nv_bytes * nof_submits))
                                            : nullptr;

      for (unsigned s = 0; s != nof_submits; ++s) {
        modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re);
        for (unsigned p = 0; p != ports; ++p) {
          ch_symbols.set_slice(p, y[s][p]);
        }
        modular_ch_est_list<8 * 4> ch_est(nof_re, ports, layers);
        for (unsigned p = 0; p != ports; ++p) {
          for (unsigned l = 0; l != layers; ++l) {
            ch_est.set_channel(h[s][p * layers + l], p, l);
          }
        }
        span<cf_t>  eq_s = span<cf_t>(eq_sync).subspan(s * nof_re * layers, nof_re * layers);
        span<float> nv_s = span<float>(nv_sync).subspan(s * nof_re * layers, nof_re * layers);
        metal.equalize(eq_s, nv_s, ch_symbols, ch_est, nv_est[s], 1.0F);

        span<cf_t>  eq_d = in_place ? span<cf_t>(eq_page + s * eq_bytes / sizeof(cf_t), nof_re * layers)
                                    : span<cf_t>(eq_plain).subspan(s * nof_re * layers, nof_re * layers);
        span<float> nv_d = in_place ? span<float>(nv_page + s * nv_bytes / sizeof(float), nof_re * layers)
                                    : span<float>(nv_plain).subspan(s * nof_re * layers, nof_re * layers);
        metal.submit(eq_d, nv_d, ch_symbols, ch_est, nv_est[s], 1.0F);
      }
      // Single wait for the whole burst.
      metal.wait();

      if (in_place) {
        // Gather the page-aligned regions into the comparison buffer.
        for (unsigned s = 0; s != nof_submits; ++s) {
          std::memcpy(eq_deferred.data() + s * nof_re * layers,
                      eq_page + s * eq_bytes / sizeof(cf_t),
                      nof_re * layers * sizeof(cf_t));
          std::memcpy(nv_deferred.data() + s * nof_re * layers,
                      nv_page + s * nv_bytes / sizeof(float),
                      nof_re * layers * sizeof(float));
        }
      } else {
        std::memcpy(eq_deferred.data(), eq_plain.data(), eq_plain.size() * sizeof(cf_t));
        std::memcpy(nv_deferred.data(), nv_plain.data(), nv_plain.size() * sizeof(float));
      }
      compat::aligned_free(eq_page);
      compat::aligned_free(nv_page);

      const bool same = (std::memcmp(eq_sync.data(), eq_deferred.data(), eq_sync.size() * sizeof(cf_t)) == 0) &&
                        (std::memcmp(nv_sync.data(), nv_deferred.data(), nv_sync.size() * sizeof(float)) == 0);
      std::printf("[chain]  %u submits in flight + single wait (%s) bit-identical to equalize(): %s\n",
                  nof_submits,
                  in_place ? "in place" : "staged",
                  same ? "OK" : "MISMATCH");
      if (!same) {
        std::fprintf(stderr, "FAIL: deferred equalization burst differs from the synchronous path\n");
        ok = false;
      }
    }
  }

  // Group submit (S-5): submit_group() must equalize a whole PUSCH-like group with the batched
  // kernel. The group mixes geometries the way a slot does (symbols carrying DM-RS have fewer
  // active RE), so it is encoded as runs of equal geometry: with nof_re {128,128,96,128,128} the
  // group must become TWO batched dispatches plus the single-symbol DM-RS one - which is what makes
  // this a test of the batching and not of a silent per-symbol fallback - and every output must be
  // bit-identical to the synchronous per-symbol equalization.
  {
    const unsigned       ports     = 2;
    const unsigned       layers    = 2;
    const unsigned       nof_sym   = 5;
    const unsigned       nof_re[]  = {128, 128, 96, 128, 128};
    std::normal_distribution<float> dist(0.0F, 0.01F);

    // Per-symbol inputs (each symbol a different realization, so a shared staging buffer shows up).
    std::vector<std::vector<std::vector<cbf16_t>>> h(nof_sym,
                                                     std::vector<std::vector<cbf16_t>>(ports * layers));
    std::vector<std::vector<std::vector<cbf16_t>>> y(nof_sym, std::vector<std::vector<cbf16_t>>(ports));
    std::vector<std::vector<float>>                nv_est(nof_sym, std::vector<float>(ports, 0.01F));
    for (unsigned s = 0; s != nof_sym; ++s) {
      for (auto& slice : h[s]) {
        slice.resize(nof_re[s]);
        for (auto& v : slice) {
          v = cbf16_t(dist(rng), dist(rng));
        }
      }
      for (auto& slice : y[s]) {
        slice.resize(nof_re[s]);
        for (auto& v : slice) {
          v = cbf16_t(dist(rng), dist(rng));
        }
      }
    }

    // Page-aligned per-symbol output regions with the uniform stride of the PUSCH group buffers.
    const size_t page       = compat::page_size();
    const size_t eq_stride  = ((128 * layers * sizeof(cf_t) + page - 1) / page) * page;
    const size_t nv_stride  = ((128 * layers * sizeof(float) + page - 1) / page) * page;
    auto*        eq_group   = static_cast<cf_t*>(compat::aligned_alloc(page, eq_stride * nof_sym));
    auto*        nv_group   = static_cast<float*>(compat::aligned_alloc(page, nv_stride * nof_sym));
    std::vector<cf_t>  eq_ref(128 * layers * nof_sym);
    std::vector<float> nv_ref(128 * layers * nof_sym);

    channel_equalizer_metal metal(false);
    std::vector<channel_equalizer::group_symbol> group;
    for (unsigned s = 0; s != nof_sym; ++s) {
      modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re[s]);
      for (unsigned p = 0; p != ports; ++p) {
        ch_symbols.set_slice(p, y[s][p]);
      }
      modular_ch_est_list<8 * 4> ch_est(nof_re[s], ports, layers);
      for (unsigned p = 0; p != ports; ++p) {
        for (unsigned l = 0; l != layers; ++l) {
          ch_est.set_channel(h[s][p * layers + l], p, l);
        }
      }
      // Reference: the synchronous per-symbol path into its own buffers.
      span<cf_t>  eq_s = span<cf_t>(eq_ref).subspan(s * 128 * layers, nof_re[s] * layers);
      span<float> nv_s = span<float>(nv_ref).subspan(s * 128 * layers, nof_re[s] * layers);
      metal.equalize(eq_s, nv_s, ch_symbols, ch_est, nv_est[s], 1.0F);

      // Group entry: same inputs, page-aligned region of the group buffers.
      group.push_back({span<cf_t>(eq_group + s * eq_stride / sizeof(cf_t), nof_re[s] * layers),
                       span<float>(nv_group + s * nv_stride / sizeof(float), nof_re[s] * layers),
                       nullptr,
                       nullptr,
                       nv_est[s],
                       1.0F});
    }

    // The views must stay alive for the whole submit_group() call, exactly as in the demodulator.
    std::vector<modular_re_buffer_reader<cbf16_t, 8>> ch_symbols_keep;
    std::vector<modular_ch_est_list<8 * 4>>           ch_est_keep;
    ch_symbols_keep.reserve(nof_sym);
    ch_est_keep.reserve(nof_sym);
    for (unsigned s = 0; s != nof_sym; ++s) {
      modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re[s]);
      for (unsigned p = 0; p != ports; ++p) {
        ch_symbols.set_slice(p, y[s][p]);
      }
      modular_ch_est_list<8 * 4> ch_est(nof_re[s], ports, layers);
      for (unsigned p = 0; p != ports; ++p) {
        for (unsigned l = 0; l != layers; ++l) {
          ch_est.set_channel(h[s][p * layers + l], p, l);
        }
      }
      ch_symbols_keep.push_back(std::move(ch_symbols));
      ch_est_keep.push_back(std::move(ch_est));
    }
    for (unsigned s = 0; s != nof_sym; ++s) {
      group[s].ch_symbols   = &ch_symbols_keep[s];
      group[s].ch_estimates = &ch_est_keep[s];
    }

    const unsigned batches_before = metal.engine_batch_dispatch_count();
    metal.submit_group(group);
    metal.wait();
    const unsigned batches = metal.engine_batch_dispatch_count() - batches_before;

    bool same = (batches == 2);
    for (unsigned s = 0; s != nof_sym; ++s) {
      const cf_t*  eq_g = eq_group + s * eq_stride / sizeof(cf_t);
      const float* nv_g = nv_group + s * nv_stride / sizeof(float);
      same = same && (std::memcmp(eq_g, eq_ref.data() + s * 128 * layers, nof_re[s] * layers * sizeof(cf_t)) == 0);
      same = same &&
             (std::memcmp(nv_g, nv_ref.data() + s * 128 * layers, nof_re[s] * layers * sizeof(float)) == 0);
    }
    std::printf("[chain]  submit_group(): %u symbols, %u batched dispatches, bit-identical to per-symbol: %s\n",
                nof_sym,
                batches,
                same ? "OK" : "MISMATCH");
    if (!same) {
      std::fprintf(stderr, "FAIL: submit_group() differs from the per-symbol chain (%u batches)\n", batches);
      ok = false;
    }
    compat::aligned_free(eq_group);
    compat::aligned_free(nv_group);
  }

  // Deferred burst whose submits share geometry, noise path and output strides: the backend turns
  // the whole burst into ONE batched dispatch (not one per symbol) and must still match the
  // synchronous per-symbol path bit for bit. This is the PUSCH shape (17 PRB -> 204 RE, 2 ports,
  // 1 layer, 12 data symbols).
  //
  // Runs only when the batched encoding is selected (OCUDU_EQ_DEFER_ENCODE=1): the default encodes
  // each dispatch where it is submitted, and with it there is nothing to batch.
  if (std::getenv("OCUDU_EQ_DEFER_ENCODE") == nullptr) {
    std::printf("[chain]  batched burst: skipped (per-symbol encoding is the default; set OCUDU_EQ_DEFER_ENCODE=1)\n");
  } else {
    const unsigned       nof_re  = 204;
    const unsigned       ports   = 2;
    const unsigned       layers  = 1;
    const unsigned       nof_sym = 12;
    std::normal_distribution<float> dist(0.0F, 0.01F);

    std::vector<std::vector<std::vector<cbf16_t>>> h(nof_sym, std::vector<std::vector<cbf16_t>>(ports * layers));
    std::vector<std::vector<std::vector<cbf16_t>>> y(nof_sym, std::vector<std::vector<cbf16_t>>(ports));
    for (unsigned si = 0; si != nof_sym; ++si) {
      for (auto& slice : h[si]) {
        slice.resize(nof_re);
        for (auto& v : slice) { v = cbf16_t(dist(rng), dist(rng)); }
      }
      for (auto& slice : y[si]) {
        slice.resize(nof_re);
        for (auto& v : slice) { v = cbf16_t(dist(rng), dist(rng)); }
      }
    }
    std::vector<float> nv_est(ports, 0.02F);

    std::vector<modular_re_buffer_reader<cbf16_t, 8>> readers;
    std::vector<modular_ch_est_list<8 * 4>>           ests;
    readers.reserve(nof_sym);
    ests.reserve(nof_sym);
    for (unsigned si = 0; si != nof_sym; ++si) {
      modular_re_buffer_reader<cbf16_t, 8> reader(ports, nof_re);
      for (unsigned p = 0; p != ports; ++p) { reader.set_slice(p, y[si][p]); }
      modular_ch_est_list<8 * 4> est(nof_re, ports, layers);
      for (unsigned p = 0; p != ports; ++p) {
        for (unsigned l = 0; l != layers; ++l) { est.set_channel(h[si][p * layers + l], p, l); }
      }
      readers.push_back(std::move(reader));
      ests.push_back(std::move(est));
    }

    const size_t page      = compat::page_size();
    const size_t eq_stride = ((nof_re * layers * sizeof(cf_t) + page - 1) / page) * page;
    const size_t nv_stride = ((nof_re * layers * sizeof(float) + page - 1) / page) * page;
    auto*        eq_group  = static_cast<cf_t*>(compat::aligned_alloc(page, eq_stride * nof_sym));
    auto*        nv_group  = static_cast<float*>(compat::aligned_alloc(page, nv_stride * nof_sym));
    std::vector<cf_t>  eq_ref(nof_re * layers * nof_sym);
    std::vector<float> nv_ref(nof_re * layers * nof_sym);

    channel_equalizer_metal metal(false);
    if (!metal.supports_deferred_chain()) {
      std::fprintf(stderr, "FAIL: deferred chain not advertised (batched burst)\n");
      ok = false;
    }
    // All references first, then all submits, then one wait: exactly how the PUSCH chain uses the
    // burst (a synchronous call in between would flush the accumulated submits one by one).
    for (unsigned si = 0; si != nof_sym; ++si) {
      span<cf_t>  eq_s = span<cf_t>(eq_ref).subspan(si * nof_re * layers, nof_re * layers);
      span<float> nv_s = span<float>(nv_ref).subspan(si * nof_re * layers, nof_re * layers);
      metal.equalize(eq_s, nv_s, readers[si], ests[si], nv_est, 1.0F);
    }
    for (unsigned si = 0; si != nof_sym; ++si) {
      span<cf_t>  eq_d(eq_group + si * eq_stride / sizeof(cf_t), nof_re * layers);
      span<float> nv_d(nv_group + si * nv_stride / sizeof(float), nof_re * layers);
      metal.submit(eq_d, nv_d, readers[si], ests[si], nv_est, 1.0F);
    }
    const unsigned batches_before = metal.engine_batch_dispatch_count();
    metal.reset_engine_batch_diagnostics();
    metal.wait();
    const unsigned batches = metal.engine_batch_dispatch_count() - batches_before;
    const auto     diag    = metal.engine_batch_diagnostics();

    bool same = (batches == 1);
    unsigned bad_sym = 0;
    for (unsigned si = 0; si != nof_sym; ++si) {
      const bool eq_ok = (std::memcmp(eq_group + si * eq_stride / sizeof(cf_t),
                                      eq_ref.data() + si * nof_re * layers,
                                      nof_re * layers * sizeof(cf_t)) == 0);
      const bool nv_ok = (std::memcmp(nv_group + si * nv_stride / sizeof(float),
                                      nv_ref.data() + si * nof_re * layers,
                                      nof_re * layers * sizeof(float)) == 0);
      if (!eq_ok || !nv_ok) {
        ++bad_sym;
        if (bad_sym <= 2) {
          const cf_t* got = eq_group + si * eq_stride / sizeof(cf_t);
          const cf_t* exp = eq_ref.data() + si * nof_re * layers;
          std::fprintf(stderr,
                       "[batched] sym %u: eq %s nv %s | re0 got (%.4f,%.4f) want (%.4f,%.4f) | nv0 got %.5f want %.5f\n",
                       si,
                       eq_ok ? "ok" : "BAD",
                       nv_ok ? "ok" : "BAD",
                       got[0].real(),
                       got[0].imag(),
                       exp[0].real(),
                       exp[0].imag(),
                       nv_group[si * nv_stride / sizeof(float)],
                       nv_ref[si * nof_re * layers]);
        }
      }
      same = same && eq_ok && nv_ok;
    }
    std::fprintf(stderr, "[batched] bad symbols: %u of %u\n", bad_sym, nof_sym);
    std::printf("[chain]  batched burst of %u submits: batches=%u, bit-identical to equalize(): %s\n",
                nof_sym,
                batches,
                same ? "OK" : "MISMATCH");
    std::printf("[chain]  engine burst: flushes=%llu symbols=%llu runs=%llu batched=%llu max_run=%u first_break=%s\n",
                static_cast<unsigned long long>(diag.flushes),
                static_cast<unsigned long long>(diag.symbols),
                static_cast<unsigned long long>(diag.runs),
                static_cast<unsigned long long>(diag.batched_runs),
                diag.max_run,
                diag.first_break);
    if (!same) {
      std::fprintf(stderr, "FAIL: batched deferred burst differs from the per-symbol path\n");
      ok = false;
    }
    compat::aligned_free(eq_group);
    compat::aligned_free(nv_group);
  }

  // Production wiring: the composite factory adapter (Metal or generic) must keep the deferred
  // chain available, otherwise the PUSCH demodulator silently falls back to the per-symbol chain.
  {
    const unsigned nof_re = 128;
    const unsigned ports  = 1;
    const unsigned layers = 1;
    auto           dist   = std::normal_distribution<float>(0.0F, 0.01F);
    std::vector<std::vector<cbf16_t>> h(ports * layers, std::vector<cbf16_t>(nof_re));
    std::vector<std::vector<cbf16_t>> y(ports, std::vector<cbf16_t>(nof_re));
    std::vector<float>                nv_est(ports, 0.01F);
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
    modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re);
    for (unsigned p = 0; p != ports; ++p) {
      ch_symbols.set_slice(p, y[p]);
    }
    modular_ch_est_list<8 * 4> ch_est(nof_re, ports, layers);
    ch_est.set_channel(h[0], 0, 0);

    std::shared_ptr<channel_equalizer_factory> factory =
        create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::zf);
    std::unique_ptr<channel_equalizer> composite = factory->create();
    if (!composite->supports_deferred_chain()) {
      std::fprintf(stderr, "FAIL: the composite equalizer factory hides the deferred chain\n");
      ok = false;
    }
    std::vector<cf_t>  eq_sync(nof_re * layers);
    std::vector<float> nv_sync(nof_re * layers);
    std::vector<cf_t>  eq_deferred(nof_re * layers);
    std::vector<float> nv_deferred(nof_re * layers);
    composite->equalize(eq_sync, nv_sync, ch_symbols, ch_est, nv_est, 1.0F);
    composite->submit(eq_deferred, nv_deferred, ch_symbols, ch_est, nv_est, 1.0F);
    composite->wait();
    const bool same = (std::memcmp(eq_sync.data(), eq_deferred.data(), nof_re * layers * sizeof(cf_t)) == 0) &&
                      (std::memcmp(nv_sync.data(), nv_deferred.data(), nof_re * layers * sizeof(float)) == 0);
    std::printf("[chain]  composite factory submit()+wait() bit-identical to equalize(): %s\n",
                same ? "OK" : "MISMATCH");
    if (!same) {
      std::fprintf(stderr, "FAIL: the composite equalizer factory breaks the deferred chain\n");
      ok = false;
    }
  }

  // Fused equalization + demapping (metal_kernel_fusion M1). Two things are checked, and the first
  // one is the reason this section exists at all: the route predicate has to come back TRUE through
  // the COMPOSITE factory the PUSCH demodulator actually holds. It did not, once - the wrapper
  // re-declares the interface methods it forwards, so an unforwarded predicate answered "no" while
  // the Metal engine behind it had the kernel, and the knob's whole effect was to change nothing.
  // Then the arithmetic: one fused dispatch must produce the very soft bits AND noise variances the
  // two-stage route produces for the same inputs, symbol by symbol.
  {
    const unsigned nof_re  = 128;
    const unsigned nof_sym = 4;
    const unsigned ports   = 1;
    const unsigned layers  = 1;
    // The two modulations the fused kernel has branches for. 64QAM is not decoration: the first air
    // pair measured that the PUSCH carries 64QAM (MCS table 2, index 13), so it is the one the legs
    // judge - see the implementation doc's memo.
    const modulation_scheme mods[2] = {modulation_scheme::QAM16, modulation_scheme::QAM64};

    std::normal_distribution<float>   dist(0.0F, 0.01F);
    std::vector<std::vector<cbf16_t>> y_sym(nof_sym, std::vector<cbf16_t>(ports * nof_re));
    std::vector<std::vector<cbf16_t>> h_sym(nof_sym, std::vector<cbf16_t>(ports * layers * nof_re));
    for (unsigned s = 0; s != nof_sym; ++s) {
      for (auto& v : y_sym[s]) {
        v = cbf16_t(dist(rng), dist(rng));
      }
      for (auto& v : h_sym[s]) {
        v = cbf16_t(dist(rng), dist(rng));
      }
    }
    std::vector<float> nv_est(ports, 0.01F);

    std::shared_ptr<channel_equalizer_factory> factory =
        create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::mmse);
    std::unique_ptr<channel_equalizer> composite = factory->create();
    std::shared_ptr<demodulation_mapper_factory> demapper_factory = create_demodulation_mapper_metal_factory();
    std::unique_ptr<demodulation_mapper>         demapper         = demapper_factory->create();

    const bool route_ok = composite->supports_fused_demapping(modulation_scheme::QAM16, ports, layers) &&
                          composite->supports_fused_demapping(modulation_scheme::QAM64, ports, layers) &&
                          !composite->supports_fused_demapping(modulation_scheme::QPSK, ports, layers) &&
                          !composite->supports_fused_demapping(modulation_scheme::QAM256, ports, layers) &&
                          !composite->supports_fused_demapping(modulation_scheme::QAM16, ports, layers + 1);
    std::printf("[fused] composite factory offers the fused route for 16QAM/64QAM at 1 layer only: %s\n",
                route_ok ? "OK" : "NO");
    if (!route_ok) {
      std::fprintf(stderr, "FAIL: the composite equalizer factory hides the fused route\n");
      ok = false;
    }

    // One page-aligned slot per symbol - the layout the PUSCH demodulator's deferred chain hands over,
    // and the layout the fused kernel's own per-symbol strides are derived from.
    const size_t eq_stride = ((nof_re * layers * sizeof(cf_t)) + compat::page_size() - 1) / compat::page_size() *
                             compat::page_size();
    const size_t nv_stride = ((nof_re * layers * sizeof(float)) + compat::page_size() - 1) / compat::page_size() *
                             compat::page_size();

    for (modulation_scheme mod : mods) {
      const unsigned bits     = get_bits_per_symbol(mod);
      const size_t   llr_stride = ((nof_re * bits) + compat::page_size() - 1) / compat::page_size() * compat::page_size();
      aligned_region eq_ref(nof_sym * eq_stride);
      aligned_region nv_ref(nof_sym * nv_stride);
      aligned_region llr_ref(nof_sym * llr_stride);
      aligned_region eq_fused(nof_sym * eq_stride);
      aligned_region nv_fused(nof_sym * nv_stride);
      aligned_region llr_fused(nof_sym * llr_stride);
      std::memset(eq_fused.ptr, 0, nof_sym * eq_stride);
      std::memset(nv_fused.ptr, 0, nof_sym * nv_stride);
      std::memset(llr_fused.ptr, 0, nof_sym * llr_stride);

      for (unsigned s = 0; s != nof_sym; ++s) {
        modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re);
        ch_symbols.set_slice(0, y_sym[s]);
        modular_ch_est_list<8 * 4> ch_est(nof_re, ports, layers);
        ch_est.set_channel(h_sym[s], 0, 0);

        auto*              eq_ref_sym  = reinterpret_cast<cf_t*>(static_cast<char*>(eq_ref.ptr) + s * eq_stride);
        auto*              nv_ref_sym  = reinterpret_cast<float*>(static_cast<char*>(nv_ref.ptr) + s * nv_stride);
        auto*              llr_ref_sym = reinterpret_cast<log_likelihood_ratio*>(static_cast<char*>(llr_ref.ptr) +
                                                                    s * llr_stride);
        auto*              eq_f_sym    = reinterpret_cast<cf_t*>(static_cast<char*>(eq_fused.ptr) + s * eq_stride);
        auto*              nv_f_sym    = reinterpret_cast<float*>(static_cast<char*>(nv_fused.ptr) + s * nv_stride);
        auto*              llr_f_sym   = reinterpret_cast<log_likelihood_ratio*>(static_cast<char*>(llr_fused.ptr) +
                                                                      s * llr_stride);
        span<cf_t>         eq_ref_span(eq_ref_sym, nof_re * layers);
        span<float>        nv_ref_span(nv_ref_sym, nof_re * layers);
        span<cf_t>         eq_f_span(eq_f_sym, nof_re * layers);
        span<float>        nv_f_span(nv_f_sym, nof_re * layers);
        span<log_likelihood_ratio> llr_ref_span(llr_ref_sym, nof_re * bits);
        span<log_likelihood_ratio> llr_f_span(llr_f_sym, nof_re * bits);

        // Reference: the two-stage route of the deferred chain - equalize, then demap what it wrote.
        composite->equalize(eq_ref_span, nv_ref_span, ch_symbols, ch_est, nv_est, 1.0F);
        demapper->demodulate_soft(llr_ref_span, eq_ref_span, nv_ref_span, mod);

        // Arm: one dispatch, the equalized symbol never leaving the kernel.
        composite->submit_fused(llr_f_span, eq_f_span, nv_f_span, ch_symbols, ch_est, nv_est, 1.0F, mod);
        composite->wait();
      }

      // Compare the ELEMENTS each route writes, slot by slot. The page-aligned slots are the layout the
      // lane uses, and their padding is written by neither route - comparing it would compare heap, which
      // is what the first version of this check did (and reported as a MISMATCH).
      bool llr_same = true;
      bool nv_same  = true;
      for (unsigned s = 0; s != nof_sym; ++s) {
        const auto* llr_a = static_cast<const char*>(llr_ref.ptr) + s * llr_stride;
        const auto* llr_b = static_cast<const char*>(llr_fused.ptr) + s * llr_stride;
        llr_same          = llr_same && (std::memcmp(llr_a, llr_b, nof_re * bits) == 0);
        const auto* nv_a = static_cast<const char*>(nv_ref.ptr) + s * nv_stride;
        const auto* nv_b = static_cast<const char*>(nv_fused.ptr) + s * nv_stride;
        nv_same          = nv_same && (std::memcmp(nv_a, nv_b, nof_re * layers * sizeof(float)) == 0);
      }
      std::printf("[fused] %s: %u symbols, one dispatch each, soft bits bit-identical to equalize+demap: %s\n",
                  to_string(mod).c_str(),
                  nof_sym,
                  llr_same ? "OK" : "MISMATCH");
      std::printf("[fused] %s: noise variances bit-identical (the post-eq SINR reduction reads them): %s\n",
                  to_string(mod).c_str(),
                  nv_same ? "OK" : "MISMATCH");
      if (!llr_same || !nv_same) {
        std::fprintf(stderr, "FAIL: the fused route differs from the two-stage route\n");
        ok = false;
      }
      // The equalized symbols are NOT written by the fused dispatch, and the route's contract says so:
      // checked rather than assumed, because a caller that reads them would otherwise read zeros.
      bool eq_untouched = true;
      for (size_t i = 0; i != nof_sym * eq_stride; ++i) {
        eq_untouched = eq_untouched && (static_cast<const char*>(eq_fused.ptr)[i] == 0);
      }
      std::printf("[fused] %s: equalized symbols left untouched as documented: %s\n",
                  to_string(mod).c_str(),
                  eq_untouched ? "OK" : "NO");
      if (!eq_untouched) {
        std::fprintf(stderr, "FAIL: the fused route wrote the equalized symbols it does not own\n");
        ok = false;
      }
    } // for (modulation_scheme mod : mods)
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
