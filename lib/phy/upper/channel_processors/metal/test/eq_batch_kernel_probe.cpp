// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Controlled A/B of the two encodings of the Metal equalizer, on a layout this file owns
// completely: one device-resident estimate buffer holding N uniformly strided symbols, one y
// buffer, one sigma2, and per-symbol eq/nv outputs whose spacing the caller chooses.
//
// The point is to take the deferred chain's real buffers - whose per-symbol spacing is set by the
// channel estimator and the demodulator - out of the picture, and answer one question on its own:
// does equalize_mxn_batch() compute the same thing as one equalize_mxn() per symbol?
//
// Usage: eq_batch_kernel_probe [nof_symbols] [nof_re] [spacing_elems] [--dmrs]
//   spacing_elems overrides the eq/nv per-symbol spacing (default: nof_re * 2 for eq).
//   --dmrs makes the per-symbol starts follow the estimator's DMRS pattern (a symbol carrying
//   DM-RS has fewer data REs, so the starts are NOT evenly spaced) - the layout the real chain has.

#include "ocudu_equalizer_metal_engine.h"

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <random>
#include <vector>

using namespace ocudu;
using ocudu::metal::equalizer_metal_engine;

namespace {

/// Deterministic pseudo-random cbf16 so both runs see bit-identical inputs.
cbf16_t make_sample(std::mt19937& rng, float scale)
{
  std::uniform_real_distribution<float> dist(-scale, scale);
  return cbf16_t(dist(rng), dist(rng));
}

bool nearly_equal(float a, float b, float tol)
{
  if (std::isinf(a) && std::isinf(b)) {
    return true;
  }
  return std::fabs(a - b) <= tol;
}

} // namespace

int main(int argc, char** argv)
{
  const unsigned nof_symbols = (argc > 1) ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 6;
  const unsigned nof_re      = (argc > 2) ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 72;
  const unsigned nof_ports   = 1;
  const unsigned nof_layers  = 1;
  constexpr float noise_var  = 0.5F;
  constexpr float tx_scaling = 1.0F;
  constexpr float h_scaling  = 1.0F;

  // Per-symbol spacing of the outputs: the equalizer writes nof_re * nof_layers * 2 floats for eq
  // and nof_re * nof_layers for nv, so a "tight" layout is the natural baseline. The override lets
  // the caller reproduce the widened spacing the deferred chain's per-symbol buffers have.
  const unsigned eq_spacing_elems =
      (argc > 3) ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : (nof_re * nof_layers * 2);
  // The real chain's nv stride is not exactly eq/2 (its per-symbol buffers are page-aligned
  // independently), so allow overriding it.
  const unsigned nv_spacing_elems =
      (argc > 5) ? static_cast<unsigned>(std::strtoul(argv[5], nullptr, 10)) : (eq_spacing_elems / 2);

  const bool dmrs_pattern = (argc > 4) && (std::string(argv[4]) == "--dmrs");

  // Inputs, laid out exactly as the batched entry documents: [symbol][port][layer][re].
  // With --dmrs the symbols do NOT sit at a uniform distance: a DM-RS symbol carries only half the
  // data REs, so its slot is shorter and every start shifts. This mirrors what the channel
  // estimator publishes (offsets 0, 72, 180, 252, ...) and is what the batched kernel has to cope
  // with - or reject.
  std::vector<unsigned> h_start(nof_symbols);
  if (dmrs_pattern) {
    unsigned at = 0;
    for (unsigned s = 0; s != nof_symbols; ++s) {
      h_start[s] = at;
      // Symbols 2, 6, 10 of a slot carry DM-RS (the pattern the estimator uses at this geometry):
      // half the data REs, so half the slot.
      const bool is_dmrs = ((s % 4) == 2);
      at += is_dmrs ? (nof_re / 2) : nof_re;
    }
  } else {
    for (unsigned s = 0; s != nof_symbols; ++s) {
      h_start[s] = s * nof_re;
    }
  }
  const unsigned h_total = h_start[nof_symbols - 1] + nof_re;

  std::printf("probe: symbols=%u nof_re=%u ports=%u layers=%u eq_spacing=%u (float2) nv_spacing=%u (float) "
              "layout=%s\n",
              nof_symbols, nof_re, nof_ports, nof_layers, eq_spacing_elems / 2, nv_spacing_elems,
              dmrs_pattern ? "dmrs(non-uniform)" : "uniform");
  std::printf("       per-symbol estimate starts:");
  for (unsigned s = 0; s != nof_symbols; ++s) {
    std::printf(" %u", h_start[s]);
  }
  std::printf("\n");

  const size_t h_elems = static_cast<size_t>(h_total) * nof_ports * nof_layers;
  const size_t y_elems = static_cast<size_t>(nof_symbols) * nof_ports * nof_re;
  const size_t eq_elems = static_cast<size_t>(nof_symbols) * eq_spacing_elems;
  const size_t nv_elems = static_cast<size_t>(nof_symbols) * nv_spacing_elems;

  std::vector<cbf16_t> h(h_elems);
  std::vector<cbf16_t> y(y_elems);
  std::vector<float>   sigma2(nof_ports);
  std::mt19937         rng(1234);
  for (cbf16_t& v : h) {
    v = make_sample(rng, 2.0F);
  }
  for (cbf16_t& v : y) {
    v = make_sample(rng, 3.0F);
  }
  for (float& v : sigma2) {
    v = 0.7F;
  }

  // Page-aligned device storage so the engine's zero-copy wrap path is exercised (the engine
  // falls back to a staging copy otherwise, which is a different code path).
  constexpr size_t page    = 16384;
  const auto       aligned = [](size_t bytes) {
    const size_t rounded = ((bytes + page - 1) / page) * page;
    return static_cast<void*>(::operator new(rounded, std::align_val_t(page)));
  };
  auto* h_dev    = static_cast<cbf16_t*>(aligned(sizeof(cbf16_t) * h_elems));
  auto* y_dev    = static_cast<cbf16_t*>(aligned(sizeof(cbf16_t) * y_elems));
  auto* eq_a     = static_cast<float*>(aligned(sizeof(float) * eq_elems));
  auto* nv_a     = static_cast<float*>(aligned(sizeof(float) * nv_elems));
  auto* eq_b     = static_cast<float*>(aligned(sizeof(float) * eq_elems));
  auto* nv_b     = static_cast<float*>(aligned(sizeof(float) * nv_elems));
  std::copy(h.begin(), h.end(), h_dev);
  std::copy(y.begin(), y.end(), y_dev);
  std::fill(eq_a, eq_a + eq_elems, 0.0F);
  std::fill(nv_a, nv_a + nv_elems, 0.0F);
  std::fill(eq_b, eq_b + eq_elems, 0.0F);
  std::fill(nv_b, nv_b + nv_elems, 0.0F);

  equalizer_metal_engine engine;
  if (!engine.init()) {
    std::printf("FAIL: engine init\n");
    return 1;
  }

  const auto h_at = [&](unsigned s) {
    return equalizer_metal_engine::ch_est_binding(
        h_dev + static_cast<size_t>(h_start[s]) * nof_ports * nof_layers);
  };
  const auto y_at  = [&](unsigned s) { return y_dev + static_cast<size_t>(s) * nof_ports * nof_re; };
  const auto eq_at = [&](float* base, unsigned s) { return base + static_cast<size_t>(s) * eq_spacing_elems; };
  const auto nv_at = [&](float* base, unsigned s) { return base + static_cast<size_t>(s) * nv_spacing_elems; };

  // --- Reference: one dispatch per symbol, through the batched entry with nof_symbols = 1 ---
  for (unsigned s = 0; s != nof_symbols; ++s) {
    if (!engine.enqueue_burst_batch(h_at(s),
                                    y_at(s),
                                    sigma2.data(),
                                    eq_at(eq_a, s),
                                    nv_at(nv_a, s),
                                    nof_re,
                                    1,
                                    0,
                                    0,
                                    0,
                                    0,
                                    nof_ports,
                                    nof_layers,
                                    false,
                                    noise_var,
                                    tx_scaling,
                                    h_scaling)) {
      std::printf("FAIL: reference enqueue at symbol %u\n", s);
      return 1;
    }
  }
  if (!engine.burst_commit() || !engine.burst_wait_committed()) {
    std::printf("FAIL: reference commit/wait\n");
    return 1;
  }

  // --- Candidate: ONE dispatch for the whole group ---
  // The batched entry with the per-symbol starts: the estimator's slices are not evenly spaced, so
  // this is the entry the real chain uses.
  if (!engine.enqueue_burst_batch_at(equalizer_metal_engine::ch_est_binding(h_dev, h_start[0], 0),
                                     span<const unsigned>(h_start.data(), nof_symbols),
                                     y_dev,
                                     sigma2.data(),
                                     eq_b,
                                     nv_b,
                                     nof_re,
                                     nof_ports * nof_re,
                                     eq_spacing_elems / 2,
                                     nv_spacing_elems,
                                     nof_ports,
                                     nof_layers,
                                     false,
                                     noise_var,
                                     tx_scaling,
                                     h_scaling)) {
    std::printf("FAIL: batched enqueue\n");
    return 1;
  }
  if (!engine.burst_commit() || !engine.burst_wait_committed()) {
    std::printf("FAIL: batched commit/wait\n");
    return 1;
  }

  // --- Compare ---
  unsigned bad = 0;
  for (unsigned s = 0; s != nof_symbols; ++s) {
    const float* ea = eq_at(eq_a, s);
    const float* eb = eq_at(eq_b, s);
    const float* na = nv_at(nv_a, s);
    const float* nb = nv_at(nv_b, s);
    unsigned     bad_eq = 0;
    unsigned     bad_nv = 0;
    for (unsigned i = 0; i != nof_re * nof_layers * 2; ++i) {
      if (!nearly_equal(ea[i], eb[i], 1e-5F)) {
        ++bad_eq;
      }
    }
    for (unsigned i = 0; i != nof_re * nof_layers; ++i) {
      if (!nearly_equal(na[i], nb[i], 1e-5F)) {
        ++bad_nv;
      }
    }
    if ((bad_eq != 0) || (bad_nv != 0)) {
      std::printf("  symbol %u: eq differs in %u/%u floats, nv in %u/%u\n",
                  s, bad_eq, nof_re * nof_layers * 2, bad_nv, nof_re * nof_layers);
      // First differing element with both values, and the inputs at that RE - enough to tell a
      // wrong input address from a wrong output address.
      for (unsigned i = 0; i != nof_re * nof_layers * 2; ++i) {
        if (!nearly_equal(ea[i], eb[i], 1e-5F)) {
          const unsigned re = i / 2;
          std::printf("    first diff i=%u (re=%u): single=(%g) batched=(%g)\n",
                      i, re, static_cast<double>(ea[i]), static_cast<double>(eb[i]));
          // The inputs of that RE, read through the SAME start table the kernel got.
          const size_t h_off = static_cast<size_t>(h_start[s]) * nof_ports * nof_layers + re;
          const size_t y_off = static_cast<size_t>(s) * nof_ports * nof_re + re;
          std::printf("    inputs at start=%u re=%u: h=(%g,%g) y=(%g,%g)\n",
                      h_start[s], re,
                      static_cast<double>(to_cf(h[h_off]).real()), static_cast<double>(to_cf(h[h_off]).imag()),
                      static_cast<double>(to_cf(y[y_off]).real()), static_cast<double>(to_cf(y[y_off]).imag()));
          break;
        }
      }
      ++bad;
    }
  }

  if (bad == 0) {
    std::printf("OK: the batched dispatch matches the per-symbol dispatch on every symbol\n");
    return 0;
  }
  std::printf("MISMATCH: %u of %u symbols differ\n", bad, nof_symbols);
  return 2;
}
