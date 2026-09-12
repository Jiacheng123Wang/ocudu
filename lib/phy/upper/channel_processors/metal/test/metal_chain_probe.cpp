// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Cost probe for the deferred PUSCH chain (A-2 route 1 design input).
///
/// Runs the real Metal equalizer and demapper adapters back to back over a whole slot, in the two
/// patterns the PUSCH demodulator can use:
///   A) the per-symbol chain: submit the equalization of one OFDM symbol, then demap it
///      synchronously (one wait per symbol and stage);
///   B) the deferred group: submit the equalization of K OFDM symbols, submit their demapping, and
///      synchronize once per group.
/// It also checks that both patterns produce bit-identical LLRs, which covers the page-aligned
/// in-place equalizer output, the in-place LLR destination and the group bookkeeping.
///
/// Environment overrides: OCUDU_PROBE_RE (active RE per OFDM symbol, default 300 = 25 PRB),
/// OCUDU_PROBE_SYMBOLS (default 14), OCUDU_PROBE_K (group size, default 7), OCUDU_PROBE_ITERS
/// (slots, default 20), OCUDU_PROBE_LAYERS (default 1, 1..2) and OCUDU_PROBE_PORTS (default 1).

#include "../channel_equalizer_metal.h"
#include "demodulation_mapper_metal.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include "ocudu/support/macos_compat.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

/// Page-aligned buffer, rounded up to whole pages (what the Metal no-copy wrap needs).
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

unsigned env_unsigned(const char* name, unsigned fallback)
{
  const char* env = std::getenv(name);
  return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : fallback;
}

} // namespace

int main()
{
  const unsigned nof_re      = env_unsigned("OCUDU_PROBE_RE", 300);
  const unsigned nof_symbols = env_unsigned("OCUDU_PROBE_SYMBOLS", 14);
  const unsigned group_size  = std::max(1U, env_unsigned("OCUDU_PROBE_K", 7));
  const unsigned nof_iters   = env_unsigned("OCUDU_PROBE_ITERS", 20);
  const unsigned layers      = env_unsigned("OCUDU_PROBE_LAYERS", 1);
  const unsigned ports       = env_unsigned("OCUDU_PROBE_PORTS", 1);
  const modulation_scheme mod = modulation_scheme::QAM64;
  const unsigned          bps = get_bits_per_symbol(mod);

  std::printf("[chain] re=%u symbols=%u layers=%u ports=%u K=%u iters=%u\n",
              nof_re,
              nof_symbols,
              layers,
              ports,
              group_size,
              nof_iters);

  std::mt19937                          rgen(1234);
  std::normal_distribution<float>       dist(0.0F, 0.3F);
  std::uniform_real_distribution<float> ch_dist(-0.5F, 0.5F);

  // Channel symbols and estimates, as the PUSCH demodulator stages them per OFDM symbol.
  modular_re_buffer_reader<cbf16_t, 8> ch_symbols(ports, nof_re);
  std::vector<std::vector<cbf16_t>>    y(ports, std::vector<cbf16_t>(nof_re));
  for (auto& slice : y) {
    for (cbf16_t& v : slice) {
      v = cbf16_t(dist(rgen), dist(rgen));
    }
  }
  for (unsigned p = 0; p != ports; ++p) {
    ch_symbols.set_slice(p, y[p]);
  }

  modular_ch_est_list<8 * 4> ch_est(nof_re, ports, layers);
  std::vector<cbf16_t>       h(static_cast<size_t>(ports) * layers * nof_re);
  for (cbf16_t& v : h) {
    v = cbf16_t(1.0F + ch_dist(rgen), ch_dist(rgen));
  }
  for (unsigned p = 0; p != ports; ++p) {
    for (unsigned l = 0; l != layers; ++l) {
      ch_est.set_channel(span<const cbf16_t>(h).subspan((static_cast<size_t>(p) * layers + l) * nof_re, nof_re),
                         p,
                         l);
    }
  }

  std::vector<float> noise_var_estimates(ports, 0.02F);

  // One page-aligned region per OFDM symbol and buffer, so the kernels read and write in place.
  const size_t page      = compat::page_size();
  const size_t eq_stride = ((static_cast<size_t>(nof_re) * layers * sizeof(cf_t) + page - 1) / page) * page;
  const size_t nv_stride = ((static_cast<size_t>(nof_re) * layers * sizeof(float) + page - 1) / page) * page;
  const size_t llr_stride = ((static_cast<size_t>(nof_re) * layers * bps + page - 1) / page) * page;

  aligned_buffer eq_buf;
  aligned_buffer nv_buf;
  aligned_buffer llr_a;
  aligned_buffer llr_b;
  eq_buf.allocate(eq_stride * nof_symbols);
  nv_buf.allocate(nv_stride * nof_symbols);
  llr_a.allocate(llr_stride * nof_symbols);
  llr_b.allocate(llr_stride * nof_symbols);

  auto eq_of  = [&](aligned_buffer& buf, unsigned s) {
    return span<cf_t>(static_cast<cf_t*>(buf.ptr) + s * eq_stride / sizeof(cf_t), static_cast<size_t>(nof_re) * layers);
  };
  auto nv_of  = [&](unsigned s) {
    return span<float>(static_cast<float*>(nv_buf.ptr) + s * nv_stride / sizeof(float), static_cast<size_t>(nof_re) * layers);
  };
  auto llr_of = [&](aligned_buffer& buf, unsigned s) {
    return span<log_likelihood_ratio>(static_cast<log_likelihood_ratio*>(buf.ptr) + s * llr_stride,
                                      static_cast<size_t>(nof_re) * layers * bps);
  };

  channel_equalizer_metal  equalizer(false);
  demodulation_mapper_metal demapper;
  if (!equalizer.supports_deferred_chain() || !demapper.supports_deferred_chain()) {
    std::fprintf(stderr, "FAIL: the deferred chain is not advertised\n");
    return 1;
  }

  // Pattern A: per-symbol chain (one wait per stage and symbol).
  const auto t_a0 = std::chrono::steady_clock::now();
  for (unsigned it = 0; it != nof_iters; ++it) {
    for (unsigned s = 0; s != nof_symbols; ++s) {
      equalizer.submit(eq_of(eq_buf, s), nv_of(s), ch_symbols, ch_est, noise_var_estimates, 1.0F);
      demapper.demodulate_soft(llr_of(llr_a, s), eq_of(eq_buf, s), nv_of(s), mod);
    }
  }
  const double us_a = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_a0).count() / nof_iters;

  // Pattern B: deferred groups (one wait per group).
  const auto t_b0 = std::chrono::steady_clock::now();
  for (unsigned it = 0; it != nof_iters; ++it) {
    for (unsigned group_begin = 0; group_begin < nof_symbols; group_begin += group_size) {
      const unsigned group_end = std::min(group_begin + group_size, nof_symbols);
      for (unsigned s = group_begin; s != group_end; ++s) {
        equalizer.submit(eq_of(eq_buf, s), nv_of(s), ch_symbols, ch_est, noise_var_estimates, 1.0F);
      }
      for (unsigned s = group_begin; s != group_end; ++s) {
        demapper.submit(llr_of(llr_b, s), eq_of(eq_buf, s), nv_of(s), mod);
      }
      demapper.wait();
      equalizer.wait();
    }
  }
  const double us_b = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_b0).count() / nof_iters;

  // Both patterns must produce bit-identical LLRs.
  const bool same = std::memcmp(llr_a.ptr, llr_b.ptr, llr_stride * nof_symbols) == 0;

  std::printf("[chain] A per-symbol chain: %.1f us/slot (%.1f us/symbol)\n", us_a, us_a / nof_symbols);
  std::printf("[chain] B deferred groups (K=%u): %.1f us/slot (%.1f us/symbol), %.2fx\n",
              group_size,
              us_b,
              us_b / nof_symbols,
              us_a / us_b);
  std::printf("[chain] A and B LLRs bit-identical: %s\n", same ? "OK" : "MISMATCH");
  return same ? 0 : 1;
}
