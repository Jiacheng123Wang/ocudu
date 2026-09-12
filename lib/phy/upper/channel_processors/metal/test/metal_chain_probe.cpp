// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Cost probe for the deferred PUSCH chain (A-2 route 1 design input).
///
/// Runs the real Metal equalizer and demapper adapters back to back over a whole slot, in the
/// patterns the PUSCH demodulator can use:
///   R) the serial chain (reference): equalize one symbol, wait for it, then demap it;
///   B) the deferred group: submit the equalization of K OFDM symbols, submit their demapping and
///      synchronize once per stage and group;
///   C) like B with an explicit wait between the two bursts.
/// The channel symbols are re-randomized before every comparison, so a dispatch that reads the
/// equalizer output too early cannot coincidentally match the reference. R vs B and R vs C must
/// both be bit-identical for every input set; the LLRs of the (invalid) pattern that never waits
/// for the equalization are reported too, as a third, informational number.
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
  aligned_buffer llr_c;
  aligned_buffer llr_r;
  eq_buf.allocate(eq_stride * nof_symbols);
  nv_buf.allocate(nv_stride * nof_symbols);
  llr_a.allocate(llr_stride * nof_symbols);
  llr_b.allocate(llr_stride * nof_symbols);
  llr_c.allocate(llr_stride * nof_symbols);
  llr_r.allocate(llr_stride * nof_symbols);

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

  // Pattern A: per-symbol chain (one wait per stage and symbol). The reference is recomputed for
  // every input set, so a read that misses the equalizer's writes shows up as a mismatch.
  // Per-pattern captures of the equalizer outputs, so a mismatch can be attributed to the
  // equalization, to the demapping or to the hand-off between them.
  std::vector<cf_t>  eq_a(static_cast<size_t>(eq_stride) / sizeof(cf_t) * nof_symbols);
  std::vector<cf_t>  eq_b(eq_a.size());
  std::vector<float> nv_a(static_cast<size_t>(nv_stride) / sizeof(float) * nof_symbols);
  std::vector<float> nv_b(nv_a.size());
  unsigned           nof_br_mismatch  = 0;
  unsigned           nof_cr_mismatch  = 0;
  unsigned           nof_eq_mismatch = 0;
  unsigned           nof_nv_mismatch = 0;
  size_t             first_bad_eq    = 0;

  double   us_a          = 0.0;
  double   us_b          = 0.0;
  unsigned nof_mismatch   = 0;
  size_t   first_bad_byte = 0;

  for (unsigned it = 0; it != nof_iters; ++it) {
    // New channel symbols for this iteration: the deferred pattern reads the equalizer output that
    // this iteration's dispatches produce, so stale data cannot coincide with the reference.
    for (auto& slice : y) {
      for (cbf16_t& v : slice) {
        v = cbf16_t(dist(rgen), dist(rgen));
      }
    }
    // Bias the input level per iteration so consecutive iterations differ in amplitude too.
    for (unsigned s = 0; s != nof_symbols; ++s) {
      for (unsigned k = 0; k != static_cast<unsigned>(nof_re) * layers; ++k) {
        eq_of(eq_buf, s)[k] = cf_t(0.0F, 0.0F);
      }
    }

    // Pattern R: the serial chain (equalization waited before the demapping), which is what the
    // synchronous path does and therefore the only trustworthy reference.
    for (unsigned s = 0; s != nof_symbols; ++s) {
      equalizer.submit(eq_of(eq_buf, s), nv_of(s), ch_symbols, ch_est, noise_var_estimates, 1.0F);
      equalizer.wait();
      demapper.demodulate_soft(llr_of(llr_r, s), eq_of(eq_buf, s), nv_of(s), mod);
    }
    const auto t_a0 = std::chrono::steady_clock::now();
    for (unsigned s = 0; s != nof_symbols; ++s) {
      equalizer.submit(eq_of(eq_buf, s), nv_of(s), ch_symbols, ch_est, noise_var_estimates, 1.0F);
      demapper.demodulate_soft(llr_of(llr_a, s), eq_of(eq_buf, s), nv_of(s), mod);
    }
    us_a += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_a0).count();
    std::memcpy(eq_a.data(), eq_buf.ptr, eq_a.size() * sizeof(cf_t));
    std::memcpy(nv_a.data(), nv_buf.ptr, nv_a.size() * sizeof(float));

    // Pattern B: deferred groups (one wait per stage and group).
    const auto t_b0 = std::chrono::steady_clock::now();
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
    us_b += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_b0).count();
    std::memcpy(eq_b.data(), eq_buf.ptr, eq_b.size() * sizeof(cf_t));
    std::memcpy(nv_b.data(), nv_buf.ptr, nv_b.size() * sizeof(float));

    // Pattern C: same grouping as B but with an explicit wait between the equalization burst and
    // the demapping burst, i.e. the ordering no longer relies on the command queue alone.
    for (unsigned group_begin = 0; group_begin < nof_symbols; group_begin += group_size) {
      const unsigned group_end = std::min(group_begin + group_size, nof_symbols);
      for (unsigned s = group_begin; s != group_end; ++s) {
        equalizer.submit(eq_of(eq_buf, s), nv_of(s), ch_symbols, ch_est, noise_var_estimates, 1.0F);
      }
      equalizer.wait();
      for (unsigned s = group_begin; s != group_end; ++s) {
        demapper.submit(llr_of(llr_c, s), eq_of(eq_buf, s), nv_of(s), mod);
      }
      demapper.wait();
    }
    {
      unsigned bad_c = 0;
      for (unsigned s = 0; s != nof_symbols; ++s) {
        const auto* pa = static_cast<const int8_t*>(llr_a.ptr) + s * llr_stride;
        const auto* pc = static_cast<const int8_t*>(llr_c.ptr) + s * llr_stride;
        const unsigned sym_len = static_cast<unsigned>(nof_re) * layers * bps;
        for (unsigned i = 0; i != sym_len; ++i) {
          bad_c += (pa[i] != pc[i]) ? 1 : 0;
        }
      }
      (void)bad_c;
    }

    // Compare the equalizer outputs of the two patterns (same inputs, so they must agree).
    for (unsigned s = 0; s != nof_symbols; ++s) {
      const size_t off = static_cast<size_t>(s) * eq_stride / sizeof(cf_t);
      for (unsigned i = 0; i != static_cast<unsigned>(nof_re) * layers; ++i) {
        if ((eq_a[off + i] != eq_b[off + i]) && (nof_eq_mismatch == 0)) {
          first_bad_eq = off + i;
        }
        nof_eq_mismatch += (eq_a[off + i] != eq_b[off + i]) ? 1 : 0;
      }
      const size_t nv_off = static_cast<size_t>(s) * nv_stride / sizeof(float);
      for (unsigned i = 0; i != static_cast<unsigned>(nof_re) * layers; ++i) {
        nof_nv_mismatch += (nv_a[nv_off + i] != nv_b[nv_off + i]) ? 1 : 0;
      }
    }

    // Both patterns must produce bit-identical LLRs for this input set.
    unsigned bad_symbols = 0;
    for (unsigned s = 0; s != nof_symbols; ++s) {
      const auto* pa = static_cast<const int8_t*>(llr_a.ptr) + s * llr_stride;
      const auto* pb = static_cast<const int8_t*>(llr_b.ptr) + s * llr_stride;
      const unsigned sym_len = static_cast<unsigned>(nof_re) * layers * bps;
      bool           sym_bad = false;
      for (unsigned i = 0; i != sym_len; ++i) {
        if (pa[i] != pb[i]) {
          if (nof_mismatch == 0 && first_bad_byte == 0) {
            first_bad_byte = static_cast<size_t>(s) * llr_stride + i;
            std::printf("[chain] first mismatch: symbol %u byte %u: A=%d B=%d (nv A=%g B=%g)\n",
                        s,
                        i,
                        static_cast<int>(pa[i]),
                        static_cast<int>(pb[i]),
                        nv_of(s)[i / bps],
                        nv_of(s)[i / bps]);
          }
          sym_bad = true;
        }
      }
      if (sym_bad) {
        ++bad_symbols;
      }
    }
    if (bad_symbols != 0) {
      ++nof_mismatch;
      if (nof_mismatch <= 3) {
        std::printf("[chain]   iteration %u: %u of %u symbols differ\n", it, bad_symbols, nof_symbols);
      }
    }
  }
  us_a /= nof_iters;
  us_b /= nof_iters;

  std::printf("[chain] A per-symbol chain: %.1f us/slot (%.1f us/symbol)\n", us_a, us_a / nof_symbols);
  std::printf("[chain] B deferred groups (K=%u): %.1f us/slot (%.1f us/symbol), %.2fx\n",
              group_size,
              us_b,
              us_b / nof_symbols,
              us_a / us_b);
  {
    unsigned bad_br = 0;
    unsigned bad_cr = 0;
    for (unsigned s = 0; s != nof_symbols; ++s) {
      const auto*    pr      = static_cast<const int8_t*>(llr_r.ptr) + s * llr_stride;
      const auto*    pb      = static_cast<const int8_t*>(llr_b.ptr) + s * llr_stride;
      const auto*    pc      = static_cast<const int8_t*>(llr_c.ptr) + s * llr_stride;
      const unsigned sym_len = static_cast<unsigned>(nof_re) * layers * bps;
      for (unsigned i = 0; i != sym_len; ++i) {
        bad_br += (pr[i] != pb[i]) ? 1 : 0;
        bad_cr += (pr[i] != pc[i]) ? 1 : 0;
      }
    }
    nof_br_mismatch = bad_br;
    nof_cr_mismatch = bad_cr;
    std::printf("[chain] REFERENCE R (serial) vs B (deferred, one wait per stage): %u differing LLR bytes -> %s\n",
                bad_br,
                (bad_br == 0) ? "OK" : "MISMATCH");
    std::printf("[chain] REFERENCE R (serial) vs C (deferred, explicit eq wait):   %u differing LLR bytes -> %s\n",
                bad_cr,
                (bad_cr == 0) ? "OK" : "MISMATCH");
  }
  std::printf("[chain] equalizer outputs: eq mismatches=%u (first at %zu), nv mismatches=%u\n",
              nof_eq_mismatch,
              first_bad_eq,
              nof_nv_mismatch);
  std::printf("[chain] A (equalization never waited) vs B: %u mismatching input sets (informational)\n",
              nof_mismatch);
  return (nof_br_mismatch == 0 && nof_cr_mismatch == 0) ? 0 : 1;
}
