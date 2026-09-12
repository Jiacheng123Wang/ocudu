// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal GPU soft-demapper unit test: bit-exact A/B comparison against the CPU
/// demodulation_mapper_impl over QPSK/16/64/256 QAM (random data, special input cases and
/// clipping behaviour), the composite-factory fallback semantics for BPSK/pi/2-BPSK, and
/// steady-state latency prints. The symbol counts are multiples of the CPU SIMD batch sizes
/// so the reference runs entirely on the NEON path the kernel mirrors.

#include "../demodulation_mapper_metal.h"
#include "../demodulation_mapper_metal_factory.h"
#include "demodulation_mapper_impl.h"
#include "ocudu/adt/format.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

unsigned bits_per_scheme(modulation_scheme mod)
{
  return get_bits_per_symbol(mod);
}

// Bit-exact LLR comparison (the kernel is required to replicate the CPU NEON output).
bool bit_exact(span<const log_likelihood_ratio> a, span<const log_likelihood_ratio> b)
{
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(int8_t)) == 0;
}

bool run_random(modulation_scheme mod, std::mt19937& rng)
{
  const unsigned nof_symbols = 256;
  const unsigned nof_bits    = nof_symbols * bits_per_scheme(mod);
  auto           dist        = std::normal_distribution<float>(0.0F, 0.3F);

  std::vector<cf_t>   symbols(nof_symbols);
  std::vector<float> noise_vars(nof_symbols);
  for (auto& z : symbols) {
    z = {dist(rng), dist(rng)};
  }
  for (auto& n : noise_vars) {
    n = 0.05F + 0.05F * std::abs(dist(rng));
  }

  std::vector<log_likelihood_ratio> llrs_metal(nof_bits);
  std::vector<log_likelihood_ratio> llrs_ref(nof_bits);

  demodulation_mapper_metal metal;
  demodulation_mapper_impl  ref;

  if (!metal.is_supported(mod)) {
    std::fprintf(stderr, "FAIL: metal is_supported(%u) returned false\n", static_cast<unsigned>(mod));
    return false;
  }
  metal.demodulate_soft(llrs_metal, symbols, noise_vars, mod);
  ref.demodulate_soft(llrs_ref, symbols, noise_vars, mod);

  if (!bit_exact(llrs_ref, llrs_metal)) {
    for (unsigned i = 0; i != nof_bits; ++i) {
      if (llrs_ref[i] != llrs_metal[i]) {
        std::fprintf(stderr,
                     "  first mismatch at bit %u (symbol %u bit %u): cpu=%d metal=%d\n",
                     i,
                     i / bits_per_scheme(mod),
                     i % bits_per_scheme(mod),
                     llrs_ref[i].to_int(),
                     llrs_metal[i].to_int());
        break;
      }
    }
    return false;
  }
  return true;
}

// Special input semantics shared by both backends: zeroed LLRs for non-positive/NaN noise
// variances, near-zero symbol masking, and clipping at +/-LLR_MAX for extreme symbols.
bool run_special(modulation_scheme mod)
{
  const unsigned nof_symbols = 24; // multiple of the SIMD batch sizes
  const unsigned nof_bits    = nof_symbols * bits_per_scheme(mod);

  std::vector<cf_t>   symbols(nof_symbols, {1.0F, -1.0F});
  std::vector<float> noise_vars(nof_symbols, 0.1F);

  symbols[0]  = {0.0F, 0.0F};                         // fully near zero
  symbols[1]  = {1e-10F, 0.5F};                       // one component near zero
  symbols[2]  = {100.0F, -100.0F};                    // extreme clipping
  noise_vars[3] = 0.0F;                               // zero noise variance
  noise_vars[4] = -0.1F;                              // negative noise variance
  noise_vars[5] = std::numeric_limits<float>::quiet_NaN();  // NaN noise variance
  noise_vars[6] = std::numeric_limits<float>::infinity();   // infinite noise variance
  symbols[7]  = {0.05F, -0.05F};                      // small but above near-zero

  std::vector<log_likelihood_ratio> llrs_metal(nof_bits);
  std::vector<log_likelihood_ratio> llrs_ref(nof_bits);

  demodulation_mapper_metal metal;
  demodulation_mapper_impl  ref;
  metal.demodulate_soft(llrs_metal, symbols, noise_vars, mod);
  ref.demodulate_soft(llrs_ref, symbols, noise_vars, mod);

  if (!bit_exact(llrs_ref, llrs_metal)) {
    for (unsigned i = 0; i != nof_bits; ++i) {
      if (llrs_ref[i] != llrs_metal[i]) {
        std::fprintf(stderr,
                     "  first mismatch at bit %u (symbol %u bit %u): cpu=%d metal=%d\n",
                     i,
                     i / bits_per_scheme(mod),
                     i % bits_per_scheme(mod),
                     llrs_ref[i].to_int(),
                     llrs_metal[i].to_int());
        break;
      }
    }
    return false;
  }
  return true;
}

// Page-aligned inputs exercise the in-place (no staging) input path; the LLR output stays
// staged in both cases. Results must be bit-identical to the CPU reference.
bool run_aligned(modulation_scheme mod, std::mt19937& rng)
{
  const unsigned nof_symbols = 256;
  const unsigned nof_bits    = nof_symbols * bits_per_scheme(mod);
  const size_t   page        = compat::page_size();
  const size_t   sym_bytes   = nof_symbols * sizeof(cf_t);
  const size_t   nv_bytes    = nof_symbols * sizeof(float);

  void* sym_mem = compat::aligned_alloc(page, ((sym_bytes + page - 1) / page) * page);
  void* nv_mem  = compat::aligned_alloc(page, ((nv_bytes + page - 1) / page) * page);
  if ((sym_mem == nullptr) || (nv_mem == nullptr)) {
    compat::aligned_free(sym_mem);
    compat::aligned_free(nv_mem);
    return false;
  }
  span<cf_t>  symbols(static_cast<cf_t*>(sym_mem), nof_symbols);
  span<float> noise_vars(static_cast<float*>(nv_mem), nof_symbols);

  auto dist = std::normal_distribution<float>(0.0F, 0.3F);
  for (auto& z : symbols) {
    z = {dist(rng), dist(rng)};
  }
  for (auto& n : noise_vars) {
    n = 0.05F + 0.05F * std::abs(dist(rng));
  }

  std::vector<log_likelihood_ratio> llrs_metal(nof_bits);
  std::vector<log_likelihood_ratio> llrs_ref(nof_bits);
  std::vector<cf_t>                 symbols_copy(symbols.begin(), symbols.end());
  std::vector<float>                nv_copy(noise_vars.begin(), noise_vars.end());

  demodulation_mapper_metal metal;
  demodulation_mapper_impl  ref;
  metal.demodulate_soft(llrs_metal, symbols, noise_vars, mod);
  ref.demodulate_soft(llrs_ref, symbols_copy, nv_copy, mod);

  compat::aligned_free(sym_mem);
  compat::aligned_free(nv_mem);
  return bit_exact(llrs_ref, llrs_metal);
}

} // namespace

int main()
{
  std::mt19937 rng(20260912);
  bool         ok = true;

  const modulation_scheme schemes[] = {modulation_scheme::QPSK,
                                       modulation_scheme::QAM16,
                                       modulation_scheme::QAM64,
                                       modulation_scheme::QAM256};
  const char*            names[]   = {"qpsk", "qam16", "qam64", "qam256"};
  for (unsigned i = 0; i != 4; ++i) {
    const bool rnd  = run_random(schemes[i], rng);
    const bool spc  = run_special(schemes[i]);
    const bool algn = run_aligned(schemes[i], rng);
    std::printf("[A/B] %-6s random=%s special=%s aligned=%s\n",
                names[i],
                rnd ? "OK" : "FAIL",
                spc ? "OK" : "FAIL",
                algn ? "OK" : "FAIL");
    ok = ok && rnd && spc && algn;
  }

  // Composite-factory fallback: BPSK and pi/2-BPSK stay on the CPU implementation.
  {
    auto factory = create_demodulation_mapper_metal_factory();
    if (factory == nullptr) {
      std::fprintf(stderr, "FAIL: metal demapper factory unavailable\n");
      return 1;
    }
    auto metal_composite = factory->create();
    auto generic         = create_demodulation_mapper_factory()->create();
    if (metal_composite == nullptr || generic == nullptr) {
      std::fprintf(stderr, "FAIL: demapper factory create() failed\n");
      ok = false;
    } else {
      std::vector<cf_t>               symbols(16, {0.5F, -0.25F});
      std::vector<float>              noise_vars(16, 0.1F);
      std::vector<log_likelihood_ratio> llrs_a(16), llrs_b(16);
      metal_composite->demodulate_soft(llrs_a, symbols, noise_vars, modulation_scheme::BPSK);
      generic->demodulate_soft(llrs_b, symbols, noise_vars, modulation_scheme::BPSK);
      if (!bit_exact(llrs_b, llrs_a)) {
        std::fprintf(stderr, "FAIL: BPSK composite fallback mismatch\n");
        ok = false;
      }
      metal_composite->demodulate_soft(llrs_a, symbols, noise_vars, modulation_scheme::PI_2_BPSK);
      generic->demodulate_soft(llrs_b, symbols, noise_vars, modulation_scheme::PI_2_BPSK);
      if (!bit_exact(llrs_b, llrs_a)) {
        std::fprintf(stderr, "FAIL: pi/2-BPSK composite fallback mismatch\n");
        ok = false;
      }
      std::printf("[size] composite factory fallback OK (BPSK / pi/2-BPSK stay CPU)\n");
    }
  }

  // Deferred chain (A-2): submit() + wait() must be bit-identical to demodulate_soft().
  {
    const unsigned nof_symbols = 256;
    const unsigned nof_bits    = nof_symbols * 6;
    auto           dist        = std::normal_distribution<float>(0.0F, 0.3F);
    std::vector<cf_t>               symbols(nof_symbols);
    std::vector<float>              noise_vars(nof_symbols);
    for (auto& z : symbols) {
      z = {dist(rng), dist(rng)};
    }
    for (auto& n : noise_vars) {
      n = 0.05F + 0.05F * std::abs(dist(rng));
    }
    std::vector<log_likelihood_ratio> llrs_sync(nof_bits);
    std::vector<log_likelihood_ratio> llrs_deferred(nof_bits);

    demodulation_mapper_metal metal;
    if (!metal.supports_deferred_chain()) {
      std::fprintf(stderr, "FAIL: deferred chain not advertised by the demapper\n");
      ok = false;
    }
    metal.demodulate_soft(llrs_sync, symbols, noise_vars, modulation_scheme::QAM64);
    metal.submit(llrs_deferred, symbols, noise_vars, modulation_scheme::QAM64);
    metal.wait();
    const bool same = bit_exact(llrs_sync, llrs_deferred);
    std::printf("[chain]  submit()+wait() bit-identical to demodulate_soft(): %s\n", same ? "OK" : "MISMATCH");
    if (!same) {
      std::fprintf(stderr, "FAIL: deferred demapping differs from the synchronous path\n");
      ok = false;
    }
  }

  // Deferred chain with several submits in flight (A-2): the whole burst is committed before a
  // single wait. Distinct inputs per submit catch a shared staging buffer, and the split of one
  // symbol into blocks must give the same LLRs as a single whole-symbol dispatch (the PUSCH
  // demodulator demaps a whole symbol and replays the codeword block split afterwards).
  {
    const unsigned    nof_blocks  = 3;
    const unsigned    block_syms  = 256;
    const unsigned    nof_symbols = nof_blocks * block_syms;
    const unsigned    bps         = 6;
    auto              dist        = std::normal_distribution<float>(0.0F, 0.3F);
    std::vector<cf_t>  symbols(nof_symbols);
    std::vector<float> noise_vars(nof_symbols);
    for (auto& z : symbols) {
      z = {dist(rng), dist(rng)};
    }
    for (auto& n : noise_vars) {
      n = 0.05F + 0.05F * std::abs(dist(rng));
    }

    // Reference: one synchronous dispatch per block.
    std::vector<log_likelihood_ratio> llrs_ref(nof_symbols * bps);
    {
      demodulation_mapper_metal metal;
      for (unsigned b = 0; b != nof_blocks; ++b) {
        metal.demodulate_soft(span<log_likelihood_ratio>(llrs_ref).subspan(b * block_syms * bps, block_syms * bps),
                              span<const cf_t>(symbols).subspan(b * block_syms, block_syms),
                              span<const float>(noise_vars).subspan(b * block_syms, block_syms),
                              modulation_scheme::QAM64);
      }
    }

    // Deferred burst: one dispatch per block, staged destinations, a single wait at the end.
    {
      std::vector<log_likelihood_ratio> llrs_def(nof_symbols * bps);
      demodulation_mapper_metal        metal;
      for (unsigned b = 0; b != nof_blocks; ++b) {
        metal.submit(span<log_likelihood_ratio>(llrs_def).subspan(b * block_syms * bps, block_syms * bps),
                     span<const cf_t>(symbols).subspan(b * block_syms, block_syms),
                     span<const float>(noise_vars).subspan(b * block_syms, block_syms),
                     modulation_scheme::QAM64);
      }
      metal.wait();
      const bool same = bit_exact(llrs_ref, llrs_def);
      std::printf("[chain]  %u staged submits in flight + single wait bit-identical: %s\n",
                  nof_blocks,
                  same ? "OK" : "MISMATCH");
      if (!same) {
        std::fprintf(stderr, "FAIL: deferred demapping burst differs from the synchronous path\n");
        ok = false;
      }
    }

    // Deferred burst into one page-aligned destination (the PUSCH demodulator layout): the kernel
    // writes the LLRs in place, so a whole symbol is one dispatch and the block split is only a
    // view of the result.
    {
      const size_t page      = compat::page_size();
      const size_t llr_bytes = ((static_cast<size_t>(nof_symbols) * bps + page - 1) / page) * page;
      auto* llr_buf = static_cast<log_likelihood_ratio*>(compat::aligned_alloc(page, llr_bytes));
      std::memset(llr_buf, 0, llr_bytes);

      demodulation_mapper_metal metal;
      metal.submit(span<log_likelihood_ratio>(llr_buf, nof_symbols * bps), symbols, noise_vars, modulation_scheme::QAM64);
      metal.wait();

      const bool same =
          bit_exact(llrs_ref, span<const log_likelihood_ratio>(llr_buf, nof_symbols * bps));
      std::printf("[chain]  page-aligned in-place LLRs + block split bit-identical: %s\n", same ? "OK" : "MISMATCH");
      if (!same) {
        std::fprintf(stderr, "FAIL: in-place demapping or the block split differs from the synchronous path\n");
        ok = false;
      }
      compat::aligned_free(llr_buf);
    }
  }

  // Steady-state latency (audit data): 100 calls per backend at 64QAM.
  {
    const unsigned nof_symbols = 256;
    const unsigned nof_bits    = nof_symbols * 6;
    std::vector<cf_t>               symbols(nof_symbols, {0.4F, -0.3F});
    std::vector<float>              noise_vars(nof_symbols, 0.1F);
    std::vector<log_likelihood_ratio> llrs(nof_bits);

    demodulation_mapper_metal metal;
    demodulation_mapper_impl  ref;
    constexpr unsigned        iters = 100;
    const auto                t0    = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      metal.demodulate_soft(llrs, symbols, noise_vars, modulation_scheme::QAM64);
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      ref.demodulate_soft(llrs, symbols, noise_vars, modulation_scheme::QAM64);
    }
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("[time] qam64 metal=%.1fus cpu=%.1fus (gpu-only last: %.1fus)\n",
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
