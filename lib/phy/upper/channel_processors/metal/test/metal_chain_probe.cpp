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
#include "../ocudu_equalizer_metal_engine.h"

#include <chrono>
#include <cstring>
#include <vector>
#include "demodulation_mapper_metal.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include "ocudu/phy/upper/equalization/view_ch_est_list.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h"
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
  /// Pattern G: the equalization of a group submitted in ONE call, the demapping one per symbol.
  unsigned           nof_group_llr_mismatch = 0;
  unsigned           nof_nv_mismatch = 0;
  size_t             first_bad_eq    = 0;

  double   us_c          = 0.0;
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

    // Pattern C: the safe grouping (what the caller does): an explicit wait between the equalization
    // burst and the demapping burst, so the hand-off does not rely on the command queue ordering.
    const auto t_c0 = std::chrono::steady_clock::now();
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
    us_c += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_c0).count();
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

    // Pattern G: exactly what the demodulator's deferred group does - the equalization of the whole
    // group in ONE submit_group() call, the demapping still one submit() per symbol, both in the
    // same shared burst with a single wait at the end.
    aligned_buffer llr_g;
    llr_g.allocate(llr_stride * nof_symbols);
    // OCUDU_PROBE_GAPPED=1 reproduces the over-the-air layout: a page-aligned GAPPED stride of 4096
    // elements (32 KiB) per symbol, which is what temp_eq_re hands the engine. With that layout the
    // group submit and the per-symbol demapping bind DIFFERENT Metal buffer objects to the same
    // memory (the group wraps the allocation base once, the demapper wraps base + s*32KiB per
    // symbol), and the demapping then reads the symbols beyond the first one before the equalization
    // has written them: 21480 differing LLR bytes, first bad symbol 1. Without the flag the outputs
    // are contiguous, both stages wrap the same pointers, and the pattern passes.
    const bool   g_gapped = (std::getenv("OCUDU_PROBE_GAPPED") != nullptr);
    const size_t g_eq_gap = g_gapped ? 4096 : eq_stride / sizeof(cf_t); // cf_t elements between symbols
    const size_t g_nv_gap = g_gapped ? 4096 : nv_stride / sizeof(float);
    aligned_buffer g_eq;
    aligned_buffer g_nv;
    g_eq.allocate(g_eq_gap * nof_symbols * sizeof(cf_t));
    g_nv.allocate(g_nv_gap * nof_symbols * sizeof(float));
    const auto g_eq_of = [&](unsigned s) {
      return span<cf_t>(static_cast<cf_t*>(g_eq.ptr) + static_cast<size_t>(s) * g_eq_gap,
                        static_cast<size_t>(nof_re) * layers);
    };
    const auto g_nv_of = [&](unsigned s) {
      return span<float>(static_cast<float*>(g_nv.ptr) + static_cast<size_t>(s) * g_nv_gap,
                         static_cast<size_t>(nof_re) * layers);
    };
    // Without the flag the outputs stay the probe's own buffers, i.e. exactly the layout patterns
    // A/B/C use, so this pattern only differs from them in HOW the equalization is submitted.
    const auto g_out_eq = [&](unsigned s) { return g_gapped ? g_eq_of(s) : eq_of(eq_buf, s); };
    const auto g_out_nv = [&](unsigned s) { return g_gapped ? g_nv_of(s) : nv_of(s); };
    const auto t_g0 = std::chrono::steady_clock::now();
    for (unsigned group_begin = 0; group_begin < nof_symbols; group_begin += group_size) {
      const unsigned group_end = std::min(group_begin + group_size, nof_symbols);
      std::vector<channel_equalizer::group_symbol> run;
      run.reserve(group_end - group_begin);
      for (unsigned s = group_begin; s != group_end; ++s) {
        run.push_back(
            channel_equalizer::group_symbol{g_out_eq(s), g_out_nv(s), &ch_symbols, &ch_est, noise_var_estimates, 1.0F});
      }
      equalizer.submit_group(run);
      for (unsigned s = group_begin; s != group_end; ++s) {
        demapper.submit(llr_of(llr_g, s), g_out_eq(s), g_out_nv(s), mod);
      }
      demapper.wait();
      equalizer.wait();
    }
    const double us_g = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_g0).count();
    {
      unsigned bad_g       = 0;
      int      first_bad_g = -1;
      for (unsigned s = 0; s != nof_symbols; ++s) {
        const auto*    pa      = static_cast<const int8_t*>(llr_a.ptr) + s * llr_stride;
        const auto*    pg      = static_cast<const int8_t*>(llr_g.ptr) + s * llr_stride;
        const unsigned sym_len = static_cast<unsigned>(nof_re) * layers * bps;
        for (unsigned i = 0; i != sym_len; ++i) {
          if (pa[i] != pg[i]) {
            ++bad_g;
            if (first_bad_g < 0) {
              first_bad_g = static_cast<int>(s);
            }
          }
        }
      }
      nof_group_llr_mismatch = bad_g;
      std::printf("[chain] G group submit + per-symbol demap in one burst (%s): %u differing LLR bytes, first bad "
                  "symbol %d (%.1f us/slot vs per-symbol %.1f us/slot) -> %s\n",
                  g_gapped ? "gapped outputs, the air layout" : "contiguous outputs",
                  bad_g,
                  first_bad_g,
                  us_g,
                  us_b,
                  (bad_g == 0) ? "OK" : "MISMATCH");
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
  us_c /= nof_iters;

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
    std::printf("[chain] C safe deferred groups (K=%u): %.1f us/slot (%.1f us/symbol), %.2fx\n",
              group_size,
              us_c,
              us_c / nof_symbols,
              us_a / us_c);
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

  // ---------------------------------------------------------------------------------------------
  // Pattern D: one dispatch for the whole group of symbols against one dispatch per symbol.
  //
  // A dispatch costs about 10 us while a 25 PRB symbol takes a couple of microseconds of GPU time,
  // so `ul_equalization_demod` is mostly dispatch overhead. Batching the symbols into a single
  // dispatch (equalize_mxn_batch, one thread per resource element and symbol) must produce
  // bit-identical outputs, and this pattern measures what it saves per slot.
  // ---------------------------------------------------------------------------------------------
  unsigned nof_batch_mismatch     = 0;
  {
    metal::equalizer_metal_engine engine;
    if (!engine.init()) {
      std::printf("[chain] D SKIPPED: no Metal device\n");
    } else {
      // Group inputs: one contiguous buffer per quantity, uniformly strided by symbol.
      const size_t h_stride  = static_cast<size_t>(ports) * layers * nof_re;
      const size_t y_stride  = static_cast<size_t>(ports) * nof_re;
      const size_t eq_el     = eq_stride / sizeof(cf_t);   // float2 elements per symbol
      const size_t nv_el     = nv_stride / sizeof(float);  // float elements per symbol

      std::vector<cbf16_t> h_group(h_stride * nof_symbols);
      std::vector<cbf16_t> y_group(y_stride * nof_symbols);
      std::vector<float>   sigma2(ports, 0.02F);
      for (cbf16_t& v : h_group) {
        v = cbf16_t(1.0F + ch_dist(rgen), ch_dist(rgen));
      }
      for (cbf16_t& v : y_group) {
        v = cbf16_t(ch_dist(rgen), ch_dist(rgen));
      }

      aligned_buffer eq_sym;
      aligned_buffer nv_sym;
      aligned_buffer eq_bat;
      aligned_buffer nv_bat;
      eq_sym.allocate(eq_stride * nof_symbols);
      nv_sym.allocate(nv_stride * nof_symbols);
      eq_bat.allocate(eq_stride * nof_symbols);
      nv_bat.allocate(nv_stride * nof_symbols);

      // Per-symbol path: one dispatch per symbol, one commit and wait at the end (what the deferred
      // chain does today).
      auto run_per_symbol = [&]() {
        for (unsigned s = 0; s != nof_symbols; ++s) {
          engine.enqueue_burst(h_group.data() + s * h_stride,
                               y_group.data() + s * y_stride,
                               sigma2.data(),
                               static_cast<char*>(eq_sym.ptr) + s * eq_stride,
                               static_cast<char*>(nv_sym.ptr) + s * nv_stride,
                               nof_re,
                               ports,
                               layers,
                               true,
                               0.02F,
                               1.0F,
                               1.0F);
        }
        engine.burst_commit();
        engine.burst_wait_committed();
      };
      // Batched path: ONE dispatch for every symbol of the group (the API the deferred chain does
      // not use yet - see the D-section conclusion).
      auto run_batch = [&]() {
        engine.burst_open();
        bool b_ok = engine.enqueue_burst_batch(h_group.data(),
                                              y_group.data(),
                                              sigma2.data(),
                                              eq_bat.ptr,
                                              nv_bat.ptr,
                                              nof_re,
                                              nof_symbols,
                                              static_cast<unsigned>(h_stride),
                                              static_cast<unsigned>(y_stride),
                                              static_cast<unsigned>(eq_el),
                                              static_cast<unsigned>(nv_el),
                                              ports,
                                              layers,
                                              true,
                                              0.02F,
                                              1.0F,
                                              1.0F);
        engine.burst_commit();
        return engine.burst_wait_committed() && b_ok;
      };

      // Warm BOTH patterns before timing them. The first dispatch of a freshly bound pipeline pays
      // its setup, and timing a warm pattern against a cold one is how the batch path came out
      // "twice as slow" in the first run of this probe. Repeat as well: one sample cannot separate
      // a 10us-per-dispatch difference from machine noise.
      run_per_symbol();
      bool ok = run_batch();

      double us_sym = 1e9;
      double us_bat = 1e9;
      for (unsigned rep = 0; rep != 10; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        run_per_symbol();
        const auto t1 = std::chrono::steady_clock::now();
        ok           = run_batch() && ok;
        const auto t2 = std::chrono::steady_clock::now();
        us_sym        = std::min(us_sym, std::chrono::duration<double, std::micro>(t1 - t0).count());
        us_bat        = std::min(us_bat, std::chrono::duration<double, std::micro>(t2 - t1).count());
      }

      const auto* pa = static_cast<const cf_t*>(eq_sym.ptr);
      const auto* pb = static_cast<const cf_t*>(eq_bat.ptr);
      const auto* na = static_cast<const float*>(nv_sym.ptr);
      const auto* nb = static_cast<const float*>(nv_bat.ptr);
      const size_t eq_total = eq_el * nof_symbols;
      const size_t nv_total = nv_el * nof_symbols;
      for (size_t i = 0; i != eq_total; ++i) {
        nof_batch_mismatch += (std::memcmp(&pa[i], &pb[i], sizeof(cf_t)) != 0) ? 1 : 0;
      }
      for (size_t i = 0; i != nv_total; ++i) {
        nof_batch_mismatch += (std::memcmp(&na[i], &nb[i], sizeof(float)) != 0) ? 1 : 0;
      }
      std::printf("[chain] D equalizer: per symbol %.1f us/slot (%.1f us/symbol) against batched "
                  "%.1f us/slot (%.1f us/symbol), %.2fx; %u differing eq/nv values -> %s\n",
                  us_sym,
                  us_sym / nof_symbols,
                  us_bat,
                  us_bat / nof_symbols,
                  (us_bat > 0.0) ? us_sym / us_bat : 0.0,
                  nof_batch_mismatch,
                  (nof_batch_mismatch == 0) ? "OK" : "MISMATCH");
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Pattern E: the over-the-air shape - TWO ports, 12 symbols, 204 REs and a page-aligned GAPPED
  // output stride (what the demodulator's temp_eq_re layout hands the engine). Pattern D above
  // covers one port with contiguous strides, and wiring the batch path into the receiving chain
  // exposed that the batch kernel does not hold outside that combination.
  // ---------------------------------------------------------------------------------------------
  unsigned nof_ota_mismatch = 0;
  {
    metal::equalizer_metal_engine engine;
    if (engine.init()) {
      const unsigned e_ports  = 2;
      const unsigned e_layers = 1;
      const unsigned e_nof_re = 204;
      const unsigned e_syms   = 12;
      const unsigned e_eq_gap = 4096; // cf_t elements between symbols (page multiple)
      const unsigned e_nv_gap = 4096; // float elements between symbols
      const size_t   e_h_str  = e_ports * e_layers * e_nof_re;
      const size_t   e_y_str  = e_ports * e_nof_re;

      std::vector<cbf16_t>            h_group(e_h_str * e_syms);
      std::vector<cbf16_t>            y_group(e_y_str * e_syms);
      std::mt19937                    r2(7);
      std::normal_distribution<float> d2(0.0F, 0.4F);
      for (cbf16_t& v : h_group) {
        v = cbf16_t(1.0F + d2(r2), d2(r2));
      }
      for (cbf16_t& v : y_group) {
        v = cbf16_t(d2(r2), d2(r2));
      }
      std::vector<float> sigma2(e_ports, 0.02F);

      aligned_buffer eq_sym;
      aligned_buffer nv_sym;
      aligned_buffer eq_bat;
      aligned_buffer nv_bat;
      eq_sym.allocate(static_cast<size_t>(e_eq_gap) * e_syms * sizeof(cf_t));
      nv_sym.allocate(static_cast<size_t>(e_nv_gap) * e_syms * sizeof(float));
      eq_bat.allocate(static_cast<size_t>(e_eq_gap) * e_syms * sizeof(cf_t));
      nv_bat.allocate(static_cast<size_t>(e_nv_gap) * e_syms * sizeof(float));

      auto per_symbol = [&]() {
        for (unsigned s = 0; s != e_syms; ++s) {
          engine.enqueue_burst(h_group.data() + s * e_h_str,
                               y_group.data() + s * e_y_str,
                               sigma2.data(),
                               static_cast<char*>(eq_sym.ptr) + static_cast<size_t>(s) * e_eq_gap * sizeof(cf_t),
                               static_cast<char*>(nv_sym.ptr) + static_cast<size_t>(s) * e_nv_gap * sizeof(float),
                               e_nof_re,
                               e_ports,
                               e_layers,
                               true,
                               0.02F,
                               1.0F,
                               1.0F);
        }
        engine.burst_commit();
        engine.burst_wait_committed();
      };
      auto batched = [&]() {
        engine.burst_open();
        bool ok = engine.enqueue_burst_batch(h_group.data(),
                                            y_group.data(),
                                            sigma2.data(),
                                            eq_bat.ptr,
                                            nv_bat.ptr,
                                            e_nof_re,
                                            e_syms,
                                            static_cast<unsigned>(e_h_str),
                                            static_cast<unsigned>(e_y_str),
                                            e_eq_gap,
                                            e_nv_gap,
                                            e_ports,
                                            e_layers,
                                            true,
                                            0.02F,
                                            1.0F,
                                            1.0F);
        engine.burst_commit();
        return engine.burst_wait_committed() && ok;
      };
      per_symbol();
      const bool ok = batched();

      const auto* pa = static_cast<const cf_t*>(eq_sym.ptr);
      const auto* pb = static_cast<const cf_t*>(eq_bat.ptr);
      const auto* na = static_cast<const float*>(nv_sym.ptr);
      const auto* nb = static_cast<const float*>(nv_bat.ptr);
      unsigned    first_bad = ~0u;
      for (unsigned s = 0; s != e_syms; ++s) {
        for (unsigned i = 0; i != e_nof_re; ++i) {
          const size_t off = static_cast<size_t>(s) * e_eq_gap + i;
          if (std::memcmp(&pa[off], &pb[off], sizeof(cf_t)) != 0) {
            nof_ota_mismatch += 1;
            first_bad = std::min(first_bad, s);
          }
          const size_t noff = static_cast<size_t>(s) * e_nv_gap + i;
          nof_ota_mismatch += (std::memcmp(&na[noff], &nb[noff], sizeof(float)) != 0) ? 1 : 0;
        }
      }
      std::printf("[chain] E OTA shape (2 ports, %u symbols, %u REs, gapped stride %u): %u differing eq/nv, "
                  "first bad symbol %d -> %s (engine ok=%d)\n",
                  e_syms,
                  e_nof_re,
                  e_eq_gap,
                  nof_ota_mismatch,
                  (first_bad == ~0u) ? -1 : static_cast<int>(first_bad),
                  (nof_ota_mismatch == 0) ? "OK" : "MISMATCH",
                  ok ? 1 : 0);
    }
  }

  // ---------------------------------------------------------------------------------------------
  // Pattern F: the ADAPTER (channel_equalizer_metal), not the engine - per-symbol submit() against
  // one submit_group() call, with the over-the-air layout: page-aligned gapped output strides and
  // one independent set of inputs per symbol (what the demodulator's deferred group hands over).
  // Pattern E proved the kernel and the engine encoding correct for this geometry, so a mismatch
  // here is in the run splitting / staging / in-place write-back of the adapter.
  // ---------------------------------------------------------------------------------------------
  unsigned nof_adapter_mismatch = 0;
  {
    const unsigned f_ports  = 2;
    const unsigned f_layers = 1;
    const unsigned f_nof_re = 204;
    const unsigned f_syms   = 12;
    const size_t   f_eq_gap = 4096; // cf_t elements between symbols (page multiple)
    const size_t   f_nv_gap = 4096; // float elements between symbols

    channel_equalizer_metal f_equalizer(false);

    std::mt19937                    r3(11);
    std::normal_distribution<float> d3(0.0F, 0.3F);
    std::uniform_real_distribution<float> c3(-0.5F, 0.5F);

    // One set of inputs per symbol, alive for the whole group (the demodulator's group slots).
    std::vector<modular_re_buffer_reader<cbf16_t, 8>> f_ch_symbols;
    std::vector<modular_ch_est_list<8 * 4>>           f_ch_est;
    std::vector<std::vector<cbf16_t>>                 f_y;
    std::vector<std::vector<cbf16_t>>                 f_h;
    f_ch_symbols.reserve(f_syms);
    f_ch_est.reserve(f_syms);
    for (unsigned s = 0; s != f_syms; ++s) {
      f_y.emplace_back(static_cast<size_t>(f_ports) * f_nof_re);
      f_h.emplace_back(static_cast<size_t>(f_ports) * f_layers * f_nof_re);
      for (cbf16_t& v : f_y.back()) {
        v = cbf16_t(d3(r3), d3(r3));
      }
      for (cbf16_t& v : f_h.back()) {
        v = cbf16_t(1.0F + c3(r3), c3(r3));
      }
      f_ch_symbols.emplace_back(f_ports, f_nof_re);
      for (unsigned p = 0; p != f_ports; ++p) {
        f_ch_symbols.back().set_slice(
            p, span<const cbf16_t>(f_y.back()).subspan(static_cast<size_t>(p) * f_nof_re, f_nof_re));
      }
      f_ch_est.emplace_back(f_nof_re, f_ports, f_layers);
      for (unsigned p = 0; p != f_ports; ++p) {
        for (unsigned l = 0; l != f_layers; ++l) {
          f_ch_est.back().set_channel(
              span<const cbf16_t>(f_h.back()).subspan((static_cast<size_t>(p) * f_layers + l) * f_nof_re, f_nof_re),
              p,
              l);
        }
      }
    }
    const std::vector<float> f_nv(f_ports, 0.02F);

    aligned_buffer f_eq_a;
    aligned_buffer f_nv_a;
    aligned_buffer f_eq_b;
    aligned_buffer f_nv_b;
    f_eq_a.allocate(f_eq_gap * f_syms * sizeof(cf_t));
    f_nv_a.allocate(f_nv_gap * f_syms * sizeof(float));
    f_eq_b.allocate(f_eq_gap * f_syms * sizeof(cf_t));
    f_nv_b.allocate(f_nv_gap * f_syms * sizeof(float));

    const auto f_eq = [&](aligned_buffer& buf, unsigned s) {
      return span<cf_t>(static_cast<cf_t*>(buf.ptr) + static_cast<size_t>(s) * f_eq_gap,
                        static_cast<size_t>(f_nof_re) * f_layers);
    };
    const auto f_nvv = [&](aligned_buffer& buf, unsigned s) {
      return span<float>(static_cast<float*>(buf.ptr) + static_cast<size_t>(s) * f_nv_gap,
                         static_cast<size_t>(f_nof_re) * f_layers);
    };

    // Path A: one submit() per symbol, exactly what the deferred chain does today.
    for (unsigned s = 0; s != f_syms; ++s) {
      f_equalizer.submit(f_eq(f_eq_a, s), f_nvv(f_nv_a, s), f_ch_symbols[s], f_ch_est[s], f_nv, 1.0F);
    }
    f_equalizer.wait();

    // Path B: one submit_group() for the whole group.
    std::vector<channel_equalizer::group_symbol> f_group;
    f_group.reserve(f_syms);
    for (unsigned s = 0; s != f_syms; ++s) {
      f_group.push_back(channel_equalizer::group_symbol{
          f_eq(f_eq_b, s), f_nvv(f_nv_b, s), &f_ch_symbols[s], &f_ch_est[s], f_nv, 1.0F});
    }
    f_equalizer.submit_group(f_group);
    f_equalizer.wait();

    const auto* fa = static_cast<const cf_t*>(f_eq_a.ptr);
    const auto* fb = static_cast<const cf_t*>(f_eq_b.ptr);
    const auto* fna = static_cast<const float*>(f_nv_a.ptr);
    const auto* fnb = static_cast<const float*>(f_nv_b.ptr);
    int         f_first_bad = -1;
    for (unsigned s = 0; s != f_syms; ++s) {
      for (unsigned i = 0; i != f_nof_re; ++i) {
        const size_t ea = static_cast<size_t>(s) * f_eq_gap + i;
        const size_t eb = ea;
        if (std::memcmp(&fa[ea], &fb[eb], sizeof(cf_t)) != 0) {
          nof_adapter_mismatch += 1;
          if (f_first_bad < 0) {
            f_first_bad = static_cast<int>(s);
          }
        }
        const size_t nva = static_cast<size_t>(s) * f_nv_gap + i;
        nof_adapter_mismatch += (std::memcmp(&fna[nva], &fnb[nva], sizeof(float)) != 0) ? 1 : 0;
      }
    }
    std::printf("[chain] F adapter (2 ports, %u symbols, %u REs, gap %zu): %u differing eq/nv, first bad symbol %d "
                "-> %s\n",
                f_syms,
                f_nof_re,
                f_eq_gap,
                nof_adapter_mismatch,
                f_first_bad,
                (nof_adapter_mismatch == 0) ? "OK" : "MISMATCH");
  }

  // ---------------------------------------------------------------------------------------------
  // Pattern H: the DEVICE-slice form of the deferred group - one Rx port and one Tx layer, so the
  // adapter reads the estimates where the estimator produced them instead of gathering them (the
  // only shape that takes that path). Every symbol of the hop points into the SAME device buffer at
  // its own offset, which is exactly the layout the estimator publishes, and the equalization of
  // the group is expected to become ONE batched dispatch.
  //
  // The per-symbol reference is produced by the synchronous equalize() of the same adapter over the
  // same device slices, so a mismatch is the batched kernel or its staging - not the device path,
  // which both sides share.
  // ---------------------------------------------------------------------------------------------
  unsigned nof_device_mismatch = 0;
  {
    const unsigned h_ports  = 1;
    const unsigned h_layers = 1;
    const unsigned h_nof_re = 204;
    const unsigned h_syms   = 12;
    const size_t   h_gap    = 4096; // cf_t elements between symbols (page multiple)

    std::mt19937                    r4(23);
    std::normal_distribution<float> d4(0.0F, 0.3F);
    std::uniform_real_distribution<float> c4(-0.5F, 0.5F);

    // Device estimate buffer: [symbol][re], one layer, so the layer stride is the symbol stride
    // (with a single layer the adapter never steps it anyway). The noise variance the estimator
    // publishes lives in its own device float, handed over the way the demodulator does it.
    std::vector<cbf16_t>                   h_device(static_cast<size_t>(h_syms) * h_nof_re);
    std::vector<std::vector<cbf16_t>>      h_y(h_ports, std::vector<cbf16_t>(h_nof_re));
    std::vector<modular_re_buffer_reader<cbf16_t, 8>> h_ch_symbols;
    std::vector<view_ch_est_list>                     h_ch_est;
    std::vector<float>                                h_device_nv(h_ports, 0.02F);
    for (cbf16_t& v : h_device) {
      v = cbf16_t(1.0F + c4(r4), c4(r4));
    }
    for (auto& slice : h_y) {
      for (cbf16_t& v : slice) {
        v = cbf16_t(d4(r4), d4(r4));
      }
    }
    h_ch_symbols.reserve(h_syms);
    h_ch_est.reserve(h_syms);
    for (unsigned s = 0; s != h_syms; ++s) {
      h_ch_symbols.emplace_back(h_ports, h_nof_re);
      h_ch_symbols.back().set_slice(0, h_y[0]);

      ch_est_device_view view;
      view.data       = h_device.data();
      view.offset     = s * h_nof_re;
      view.nof_re     = h_nof_re;
      view.total_re   = h_syms * h_nof_re;
      view.nof_layers = 1;

      h_ch_est.emplace_back();
      h_ch_est.back().reset(h_nof_re, h_ports, h_layers);
      h_ch_est.back().set_channel(0, 0, view.get_layer(0), view.data);
      h_ch_est.back().set_device_noise_variance(0, h_device_nv.data());
    }
    const std::vector<float> h_nv(h_ports, 0.02F);

    channel_equalizer_metal h_equalizer(false);
    if (!h_equalizer.consumes_device_estimates(h_ports, h_layers)) {
      std::fprintf(stderr, "FAIL: the adapter refuses the device estimates of the 1x1 shape\n");
      return 1;
    }

    aligned_buffer h_eq_b;
    aligned_buffer h_nv_b;
    aligned_buffer h_eq_ref_buf;
    aligned_buffer h_nv_ref_buf;
    h_eq_b.allocate(h_gap * h_syms * sizeof(cf_t));
    h_nv_b.allocate(h_gap * h_syms * sizeof(float));
    h_eq_ref_buf.allocate(h_gap * h_syms * sizeof(cf_t));
    h_nv_ref_buf.allocate(h_gap * h_syms * sizeof(float));
    const auto h_eq = [&](aligned_buffer& buf, unsigned s) {
      return span<cf_t>(static_cast<cf_t*>(buf.ptr) + static_cast<size_t>(s) * h_gap, h_nof_re * h_layers);
    };
    const auto h_nvv = [&](aligned_buffer& buf, unsigned s) {
      return span<float>(static_cast<float*>(buf.ptr) + static_cast<size_t>(s) * h_gap, h_nof_re * h_layers);
    };

    // Reference: the synchronous per-symbol chain over the same device slices. It needs its own
    // outputs - the deferred pass below writes the group's regions in place, so sharing them would
    // overwrite the reference (the device estimates themselves are read by both).
    for (unsigned s = 0; s != h_syms; ++s) {
      h_equalizer.equalize(h_eq(h_eq_ref_buf, s), h_nvv(h_nv_ref_buf, s), h_ch_symbols[s], h_ch_est[s], h_nv, 1.0F);
    }
    std::vector<cf_t>  h_eq_ref(static_cast<size_t>(h_gap) * h_syms);
    std::vector<float> h_nv_ref(static_cast<size_t>(h_gap) * h_syms);
    std::memcpy(h_eq_ref.data(), h_eq_ref_buf.ptr, h_eq_ref.size() * sizeof(cf_t));
    std::memcpy(h_nv_ref.data(), h_nv_ref_buf.ptr, h_nv_ref.size() * sizeof(float));

    // Oracle: the same arithmetic over HOST-staged estimates of the same values, i.e. the form the
    // 2-port path always uses. It says which of the two sides above is the defective one when they
    // disagree. Its comparison happens after the deferred pass, when the batch outputs exist.
    aligned_buffer h_eq_host;
    aligned_buffer h_nv_host;
    h_eq_host.allocate(h_gap * h_syms * sizeof(cf_t));
    h_nv_host.allocate(h_gap * h_syms * sizeof(float));
    std::vector<modular_ch_est_list<8 * 4>> h_ch_est_host;
    h_ch_est_host.reserve(h_syms);
    for (unsigned s = 0; s != h_syms; ++s) {
      h_ch_est_host.emplace_back(h_nof_re, h_ports, h_layers);
      h_ch_est_host.back().set_channel(
          span<const cbf16_t>(h_device).subspan(static_cast<size_t>(s) * h_nof_re, h_nof_re), 0, 0);
      h_equalizer.equalize(
          h_eq(h_eq_host, s), h_nvv(h_nv_host, s), h_ch_symbols[s], h_ch_est_host[s], h_nv, 1.0F);
    }

    h_equalizer.reset_engine_batch_diagnostics();
    for (unsigned s = 0; s != h_syms; ++s) {
      h_equalizer.submit(h_eq(h_eq_b, s), h_nvv(h_nv_b, s), h_ch_symbols[s], h_ch_est[s], h_nv, 1.0F);
    }
    h_equalizer.wait();
    const auto h_diag = h_equalizer.engine_batch_diagnostics();

    // Diagnostic dump of the first elements of a few symbols: enough to recompute one RE by hand
    // when the two paths disagree.
    for (unsigned s = 0; s != 4 && s != h_syms; ++s) {
      const cbf16_t h0 = h_device[static_cast<size_t>(s) * h_nof_re];
      std::printf("[device] sym %u re0 y=(%.6f,%.6f) h=(%.6f,%.6f) ref=(%.6f,%.6f) batch=(%.6f,%.6f)\n",
                  s,
                  to_float(h_y[0][0].real),
                  to_float(h_y[0][0].imag),
                  to_float(h0.real),
                  to_float(h0.imag),
                  h_eq_ref[static_cast<size_t>(s) * h_gap].real(),
                  h_eq_ref[static_cast<size_t>(s) * h_gap].imag(),
                  static_cast<const cf_t*>(h_eq_b.ptr)[static_cast<size_t>(s) * h_gap].real(),
                  static_cast<const cf_t*>(h_eq_b.ptr)[static_cast<size_t>(s) * h_gap].imag());
    }

    unsigned h_bad_eq = 0;
    unsigned h_bad_nv = 0;
    unsigned h_bad_oracle_ref = 0;
    unsigned h_bad_oracle_batch = 0;
    for (unsigned s = 0; s != h_syms; ++s) {
      for (unsigned i = 0; i != h_nof_re; ++i) {
        const size_t idx = static_cast<size_t>(s) * h_gap + i;
        const bool   eq_diff =
            (std::memcmp(&static_cast<const cf_t*>(h_eq_b.ptr)[idx], &h_eq_ref[idx], sizeof(cf_t)) != 0);
        const bool nv_diff =
            (std::memcmp(&static_cast<const float*>(h_nv_b.ptr)[idx], &h_nv_ref[idx], sizeof(float)) != 0);
        h_bad_eq += eq_diff ? 1 : 0;
        h_bad_nv += nv_diff ? 1 : 0;
        h_bad_oracle_ref +=
            (std::memcmp(&static_cast<const cf_t*>(h_eq_host.ptr)[idx], &h_eq_ref[idx], sizeof(cf_t)) != 0) ? 1 : 0;
        h_bad_oracle_batch +=
            (std::memcmp(&static_cast<const cf_t*>(h_eq_host.ptr)[idx], &static_cast<const cf_t*>(h_eq_b.ptr)[idx],
                         sizeof(cf_t)) != 0)
                ? 1
                : 0;
        if (eq_diff || nv_diff) {
          if (nof_device_mismatch == 0) {
            std::fprintf(stderr,
                         "[device] sym %u re %u: eq %s got (%.6f,%.6f) want (%.6f,%.6f) | nv %s got %.6f want %.6f\n",
                         s,
                         i,
                         eq_diff ? "BAD" : "ok",
                         static_cast<const cf_t*>(h_eq_b.ptr)[idx].real(),
                         static_cast<const cf_t*>(h_eq_b.ptr)[idx].imag(),
                         h_eq_ref[idx].real(),
                         h_eq_ref[idx].imag(),
                         nv_diff ? "BAD" : "ok",
                         static_cast<const float*>(h_nv_b.ptr)[idx],
                         h_nv_ref[idx]);
          }
          nof_device_mismatch += 1;
        }
      }
    }
    // A device-slice group is the shape that reaches the batched kernel once the deferred encoding
    // is selected: a silent fallback to one dispatch per symbol would still produce the right
    // values, so - when the deferred form is what this process asked for - the counters are part of
    // the verdict. The oracle pinpoints which side is wrong when the two disagree.
    const bool expect_batch = (std::getenv("OCUDU_EQ_DEFER_ENCODE") != nullptr) &&
                              (std::strtoul(std::getenv("OCUDU_EQ_DEFER_ENCODE"), nullptr, 10) != 0);
    const bool h_batched = (h_diag.batched_runs == 1) && (h_diag.max_run == h_syms);
    std::printf("[chain] H device slices (1 port, %u symbols, %u REs, gap %zu): %u differing eq (%u) / nv (%u), "
                "flushes=%llu runs=%llu batched=%llu max_run=%u first_break=%s; oracle (host-staged): reference "
                "differs in %u, batch in %u -> %s\n",
                h_syms,
                h_nof_re,
                h_gap,
                nof_device_mismatch,
                h_bad_eq,
                h_bad_nv,
                static_cast<unsigned long long>(h_diag.flushes),
                static_cast<unsigned long long>(h_diag.runs),
                static_cast<unsigned long long>(h_diag.batched_runs),
                h_diag.max_run,
                h_diag.first_break,
                h_bad_oracle_ref,
                h_bad_oracle_batch,
                (nof_device_mismatch == 0 && h_bad_oracle_ref == 0 && h_bad_oracle_batch == 0 &&
                 (h_batched || !expect_batch))
                    ? "OK"
                    : "MISMATCH");
    if (expect_batch && !h_batched) {
      std::fprintf(stderr, "FAIL: the device-slice group did not become one batched dispatch\n");
      return 1;
    }
  }

  return (nof_br_mismatch == 0 && nof_cr_mismatch == 0 && nof_batch_mismatch == 0 && nof_ota_mismatch == 0 && nof_adapter_mismatch == 0 && nof_group_llr_mismatch == 0 && nof_device_mismatch == 0) ? 0 : 1;
}
