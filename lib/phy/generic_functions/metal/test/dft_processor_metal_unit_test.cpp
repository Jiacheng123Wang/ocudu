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
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/ocuduvec/prod.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/support/macos_compat.h"
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

  // Batched execution (Phase 1): a batch of one slot worth of transforms must be bit-identical
  // to executing the same transforms one by one, and must be much faster than the per-symbol
  // dispatch pattern.
  for (unsigned size : {512U, 768U, 1024U, 2048U}) {
    constexpr unsigned nof_transforms = 14; // OFDM symbols per slot
    auto               metal          = dft_processor_metal({size, dft_processor::direction::DIRECT});
    if (!metal.is_valid()) {
      continue;
    }
    if (metal.get_max_batch() < nof_transforms) {
      std::fprintf(stderr, "FAIL: size=%u max_batch=%u < %u\n", size, metal.get_max_batch(), nof_transforms);
      ok = false;
      continue;
    }

    // Random input for the whole batch.
    std::vector<cf_t> batch_in(static_cast<size_t>(size) * nof_transforms);
    for (auto& v : batch_in) {
      v = cf_t(dist(rng), dist(rng));
    }
    std::copy(batch_in.begin(), batch_in.end(), metal.get_input().begin());
    // Copy the batch result out: the reference runs below reuse the same output buffer (slot 0).
    span<const cf_t>  batch_view = metal.run_batch(nof_transforms);
    std::vector<cf_t> batch_out(batch_view.begin(), batch_view.end());

    // Reference: one transform at a time, with the input of each transform in slot 0.
    std::vector<cf_t> ref_out(static_cast<size_t>(size) * nof_transforms);
    for (unsigned i = 0; i != nof_transforms; ++i) {
      std::copy(batch_in.begin() + static_cast<size_t>(i) * size,
                batch_in.begin() + static_cast<size_t>(i + 1) * size,
                metal.get_input().begin());
      span<const cf_t> single = metal.run();
      std::copy(single.begin(), single.end(), ref_out.begin() + static_cast<size_t>(i) * size);
    }

    bool identical = true;
    for (unsigned i = 0; i != nof_transforms * size; ++i) {
      if ((batch_out[i] != ref_out[i])) {
        identical = false;
        break;
      }
    }
    std::printf("[batch] size=%4u n=%2u bit-identical=%s\n", size, nof_transforms, identical ? "OK" : "MISMATCH");
    if (!identical) {
      std::fprintf(stderr, "FAIL: batched DFT differs from single-transform runs (size=%u)\n", size);
      ok = false;
    }

    // Latency: 14 symbols batched versus 14 single dispatches.
    constexpr unsigned rounds = 20;
    auto               t0     = std::chrono::steady_clock::now();
    for (unsigned r = 0; r != rounds; ++r) {
      (void)metal.run_batch(nof_transforms);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (unsigned r = 0; r != rounds; ++r) {
      for (unsigned i = 0; i != nof_transforms; ++i) {
        (void)metal.run();
      }
    }
    auto         t2       = std::chrono::steady_clock::now();
    const double batch_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / rounds;
    const double per_sym_us =
        std::chrono::duration<double, std::micro>(t2 - t1).count() / (rounds * nof_transforms);
    std::printf("[time]  size=%4u slot batch(14)=%.1fus  per-symbol dispatch=%.1fus  (%.1fx)\n",
                size,
                batch_us,
                per_sym_us,
                per_sym_us * nof_transforms / batch_us);
  }

  // Ring submission (CPU/GPU pipelining, S2): each transform of a slot is submitted into its own
  // slot without waiting and the whole ring is synchronized once. The result must be bit-identical
  // to the synchronous path, and the CPU must not have waited between submissions.
  for (unsigned size : {512U, 1024U, 2048U}) {
    constexpr unsigned nof_transforms = 8; // ring depth (<= max_batch)
    auto               metal          = dft_processor_metal({size, dft_processor::direction::DIRECT});
    if (!metal.is_valid()) {
      continue;
    }

    std::vector<cf_t> batch_in(static_cast<size_t>(size) * nof_transforms);
    for (auto& v : batch_in) {
      v = cf_t(dist(rng), dist(rng));
    }

    // Submit every transform into its own slot, then synchronize once.
    for (unsigned i = 0; i != nof_transforms; ++i) {
      std::copy(batch_in.begin() + static_cast<size_t>(i) * size,
                batch_in.begin() + static_cast<size_t>(i + 1) * size,
                metal.get_input().begin() + static_cast<size_t>(i) * size);
      metal.run_async(i);
    }
    metal.wait();

    // Copy the ring results out first: the reference below reuses slot 0 of the output buffer.
    span<const cf_t>  ring_view = metal.get_output_batch().first(static_cast<size_t>(size) * nof_transforms);
    std::vector<cf_t> ring_out(ring_view.begin(), ring_view.end());

    // Reference: the same transforms, one synchronous run() each (slot 0).
    std::vector<cf_t> ref_out(static_cast<size_t>(size) * nof_transforms);
    for (unsigned i = 0; i != nof_transforms; ++i) {
      std::copy(batch_in.begin() + static_cast<size_t>(i) * size,
                batch_in.begin() + static_cast<size_t>(i + 1) * size,
                metal.get_input().begin());
      span<const cf_t> single = metal.run();
      std::copy(single.begin(), single.end(), ref_out.begin() + static_cast<size_t>(i) * size);
    }

    bool identical = true;
    for (unsigned i = 0; i != nof_transforms * size; ++i) {
      if (ring_out[i] != ref_out[i]) {
        identical = false;
        break;
      }
    }
    std::printf("[ring]  size=%4u depth=%u bit-identical=%s\n", size, nof_transforms, identical ? "OK" : "MISMATCH");
    if (!identical) {
      std::fprintf(stderr, "FAIL: ring-submitted DFT differs from synchronous runs (size=%u)\n", size);
      ok = false;
    }
  }


  // Grid write (S-7b, "FFT phase 2"): the transform and the write of one grid symbol are encoded in ONE command buffer,
  // and the result must be identical to the host post-processing of the same transform output - the same complex
  // products in the same order and the same bf16 rounding, including the upper/lower band mapping. A single
  // mismatching element fails the check (the grid feeds the channel estimator and the equalizer, so a drift here would
  // surface as a numeric regression much later).
  {
    constexpr unsigned size     = 512; // 5 MHz cell: 25 PRB = 300 subcarriers
    constexpr unsigned nof_subc = 300;
    constexpr unsigned nof_symb = 14;
    constexpr unsigned port     = 0;
    constexpr unsigned symbol   = 7;
    constexpr unsigned slot     = 3;

    dft_processor_metal metal({size, dft_processor::direction::DIRECT});
    if (!metal.is_valid()) {
      std::fprintf(stderr, "FAIL: Metal DFT invalid for the grid-write check (size=%u)\n", size);
      return 1;
    }

    std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
    if (grid_factory == nullptr) {
      std::fprintf(stderr, "FAIL: no resource grid factory\n");
      return 1;
    }
    std::unique_ptr<resource_grid> device_grid = grid_factory->create(1, nof_symb, nof_subc);
    std::unique_ptr<resource_grid> host_grid   = grid_factory->create(1, nof_symb, nof_subc);
    if (device_grid == nullptr || host_grid == nullptr) {
      std::fprintf(stderr, "FAIL: resource grid creation failed\n");
      return 1;
    }

    const resource_grid_device_view view = device_grid->get_writer().get_device_view();
    auto*                           grid_writer = static_cast<dft_processor_grid_write*>(&metal);
    if (!view.is_valid() || !grid_writer->supports_grid_write(view)) {
      std::fprintf(stderr, "FAIL: the Metal DFT cannot write the resource grid (view valid=%d)\n", view.is_valid());
      return 1;
    }

    // Input shared by the reference transform and the grid write.
    std::vector<cf_t> input(size);
    for (unsigned i = 0; i != size; ++i) {
      input[i] = cf_t(static_cast<float>(dist(rng)), static_cast<float>(dist(rng)));
    }

    // Reference transform: run it synchronously (slot 0) and keep its output.
    std::copy(input.begin(), input.end(), metal.get_input().begin());
    span<const cf_t> reference_transform = metal.run();
    std::vector<cf_t> transform_out(reference_transform.begin(), reference_transform.end());

    // Compensation of the grid write: a per-symbol coefficient (phase compensation times the output scaling) and, in
    // the second round, the DFT window table.
    const cf_t coefficient              = cf_t(0.937F, -0.349F);
    const unsigned map_offset           = size - nof_subc / 2;

    for (bool with_window : {false, true}) {
      std::vector<cf_t> window;
      if (with_window) {
        window.resize(size);
        const float omega = 2.0F * static_cast<float>(M_PI) * 3.0F / static_cast<float>(size);
        for (unsigned i = 0; i != size; ++i) {
          window[i] = std::polar(1.0F, omega * static_cast<float>(i));
        }
      }
      if (!grid_writer->set_grid_write_window(window)) {
        std::fprintf(stderr, "FAIL: publishing the grid-write window failed (window=%d)\n", with_window);
        return 1;
      }

      // Device path: fill slot `slot` with the same input, then submit the transform and the grid write together.
      // Slot 0 is refilled with DIFFERENT samples first: the grid write must consume the transform of its own slot, and
      // a slot mix-up would then be caught by the comparison instead of passing with the leftover reference transform.
      for (unsigned i = 0; i != size; ++i) {
        metal.get_input()[i] = cf_t(static_cast<float>(dist(rng)), static_cast<float>(dist(rng)));
      }
      std::copy(input.begin(), input.end(), metal.get_input().begin() + static_cast<size_t>(slot) * size);
      dft_grid_write_params params;
      params.view         = view;
      params.port         = port;
      params.symbol       = symbol;
      params.nof_subc     = nof_subc;
      params.map_offset   = map_offset;
      params.coefficient  = coefficient;
      params.apply_window = with_window;
      if (!grid_writer->submit_grid_write(slot, params)) {
        std::fprintf(stderr, "FAIL: submit_grid_write rejected the request (window=%d)\n", with_window);
        return 1;
      }
      // The processor's wait_slot() is the plain synchronous wait (no result); a failed command buffer would leave the
      // grid untouched and the comparison below would report it.
      metal.wait_slot(slot);

      // Host reference: exactly what the demodulator does with the transform output it reads back
      // (ofdm_demodulator_impl::process_dft_output with the same coefficient and window).
      std::vector<cf_t> compensated(size);
      ocuduvec::sc_prod(compensated, span<const cf_t>(transform_out), coefficient);
      if (!window.empty()) {
        ocuduvec::prod(compensated, span<const cf_t>(window), compensated);
      }
      {
        resource_grid_writer& writer = host_grid->get_writer();
        writer.put(port, symbol, 0, span<const cf_t>(&compensated[size - nof_subc / 2], nof_subc / 2));
        writer.put(port, symbol, nof_subc / 2, span<const cf_t>(&compensated[0], nof_subc / 2));
      }

      const span<const cbf16_t> got  = device_grid->get_reader().get_view(port, symbol).first(nof_subc);
      const span<const cbf16_t> want = host_grid->get_reader().get_view(port, symbol).first(nof_subc);
      unsigned                  mismatches = 0;
      for (unsigned k = 0; k != nof_subc; ++k) {
        if ((got[k] != want[k])) {
          if (mismatches == 0) {
            std::fprintf(stderr,
                         "  first mismatch at subcarrier %u: device=(%f,%f) host=(%f,%f)\n",
                         k,
                         to_cf(got[k]).real(),
                         to_cf(got[k]).imag(),
                         to_cf(want[k]).real(),
                         to_cf(want[k]).imag());
          }
          ++mismatches;
        }
      }
      std::printf("[grid]  size=%4u window=%d subcarriers=%u mismatching=%u\n",
                  size,
                  with_window ? 1 : 0,
                  nof_subc,
                  mismatches);
      if (mismatches != 0) {
        std::fprintf(stderr, "FAIL: the device grid write differs from the host reference (window=%d)\n", with_window);
        ok = false;
      }
    }

    // Input straight from the radio buffer: the same symbol handed over as int16 samples in a
    // page-aligned allocation (what baseband_gateway_buffer_dynamic_aligned gives the DFT), read by
    // the kernel instead of the staged float2 input. The values are chosen so that the kernel's
    // conversion (float(sample) * gain) reproduces the staged input EXACTLY, which isolates what
    // this test is about: the offset, the wrap and the conversion plumbing, not the arithmetic.
    {
      const float gain = 1.0F / ocuduvec::scaling_factor_ci16_to_cf;

      // A cyclic prefix in front of the transform, and the window offset the demodulator applies.
      const unsigned cp_len = 64;
      const unsigned offset = cp_len - 8;
      const size_t   page   = compat::page_size();
      const size_t   bytes  = ((static_cast<size_t>(cp_len + size) * sizeof(ci16_t) + page - 1) / page) * page;
      ci16_t*        samples = static_cast<ci16_t*>(compat::aligned_alloc(page, bytes));
      if (samples == nullptr) {
        std::fprintf(stderr, "FAIL: the aligned sample allocation failed\n");
        return 1;
      }
      std::vector<cf_t> time_input(size);
      for (unsigned i = 0; i != size; ++i) {
        // Round numbers so that the float product below is the value the kernel computes exactly.
        const auto qi = static_cast<int16_t>(static_cast<int>(dist(rng)) % 30000);
        const auto qq = static_cast<int16_t>(static_cast<int>(dist(rng)) % 30000);
        samples[offset + i] = ci16_t(qi, qq);
        time_input[i]       = cf_t(static_cast<float>(qi) * gain, static_cast<float>(qq) * gain);
      }
      // Reference: the staged path on those very values. run() transforms slot 0, so the reference
      // goes there - and then the whole ring is filled with garbage on purpose: the radio-buffer
      // dispatch below must produce the reference grid anyway, which proves it reads the samples and
      // not the engine's own input buffer.
      std::copy(time_input.begin(), time_input.end(), metal.get_input().begin());
      span<const cf_t> staged_transform = metal.run();
      std::vector<cf_t> staged_out(staged_transform.begin(), staged_transform.end());
      for (cf_t& x : metal.get_input()) {
        x = cf_t(1234.5F, -6789.0F);
      }

      if (!grid_writer->set_grid_write_window({})) {
        std::fprintf(stderr, "FAIL: clearing the grid-write window failed\n");
        return 1;
      }
      dft_grid_write_params params;
      params.view               = view;
      params.port               = port;
      params.symbol             = symbol;
      params.nof_subc           = nof_subc;
      params.map_offset         = map_offset;
      params.coefficient        = coefficient;
      params.apply_window       = false;
      params.time_samples       = samples;
      params.time_samples_bytes = (cp_len + size) * sizeof(ci16_t);
      params.time_window_start  = offset;
      params.time_gain          = gain;
      if (!grid_writer->submit_grid_write(slot, params)) {
        std::fprintf(stderr, "FAIL: the engine refused the radio-buffer input\n");
        compat::aligned_free(samples);
        return 1;
      }
      metal.wait_slot(slot);

      // The device grid must equal the host post-processing of the STAGED transform of the same values.
      std::vector<cf_t> compensated(size);
      ocuduvec::sc_prod(compensated, span<const cf_t>(staged_out), coefficient);
      {
        resource_grid_writer& writer = host_grid->get_writer();
        writer.put(port, symbol, 0, span<const cf_t>(&compensated[size - nof_subc / 2], nof_subc / 2));
        writer.put(port, symbol, nof_subc / 2, span<const cf_t>(&compensated[0], nof_subc / 2));
      }
      const span<const cbf16_t> got  = device_grid->get_reader().get_view(port, symbol).first(nof_subc);
      const span<const cbf16_t> want = host_grid->get_reader().get_view(port, symbol).first(nof_subc);
      unsigned                  mismatches = 0;
      for (unsigned k = 0; k != nof_subc; ++k) {
        if (got[k] != want[k]) {
          if (mismatches == 0) {
            std::fprintf(stderr,
                         "  ci16 first mismatch at subcarrier %u: device=(%f,%f) host=(%f,%f)\n",
                         k,
                         to_cf(got[k]).real(),
                         to_cf(got[k]).imag(),
                         to_cf(want[k]).real(),
                         to_cf(want[k]).imag());
          }
          ++mismatches;
        }
      }
      std::printf("[ci16]  size=%4u window=0 subcarriers=%u mismatching=%u\n", size, nof_subc, mismatches);
      if (mismatches != 0) {
        std::fprintf(stderr, "FAIL: the radio-buffer input differs from the staged path\n");
        ok = false;
      }
      compat::aligned_free(samples);
    }
  }

  // Cost of the fused grid write: the final store of the transform kernel writes the subcarriers of the grid with the
  // compensation applied, so it replaces the plain store instead of adding a dispatch. Measured warm (the first
  // submission of a grid pays the one-off mapping of its buffer, which a ring reuses for the whole run).
  {
    constexpr unsigned size     = 2048;
    constexpr unsigned nof_subc = 1272;
    dft_processor_metal metal({size, dft_processor::direction::DIRECT});
    if (!metal.is_valid()) {
      std::fprintf(stderr, "FAIL: Metal DFT invalid for the grid-write cost probe (size=%u)\n", size);
      return 1;
    }
    auto       grid = create_resource_grid_factory()->create(1, 14, nof_subc);
    const auto view = grid->get_writer().get_device_view();
    auto*      gw   = static_cast<dft_processor_grid_write*>(&metal);
    if (!gw->supports_grid_write(view) || !gw->set_grid_write_window({})) {
      std::fprintf(stderr, "FAIL: the Metal DFT cannot write the resource grid\n");
      return 1;
    }

    dft_grid_write_params params;
    params.view        = view;
    params.nof_subc    = nof_subc;
    params.map_offset  = size - nof_subc / 2;
    params.coefficient = cf_t(0.9F, -0.3F);

    constexpr unsigned iters   = 200;
    constexpr unsigned nof_warmup = 8;
    for (unsigned i = 0; i != nof_warmup; ++i) {
      metal.run_async(0);
      metal.wait_slot(0);
      gw->submit_grid_write(0, params);
      metal.wait_slot(0);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iters; ++i) {
      metal.run_async(0);
      metal.wait_slot(0);
    }
    auto         t1        = std::chrono::steady_clock::now();
    const double plain_gpu = metal.engine_gpu_wait_us();
    for (unsigned i = 0; i != iters; ++i) {
      gw->submit_grid_write(0, params);
      metal.wait_slot(0);
    }
    auto         t2       = std::chrono::steady_clock::now();
    const double grid_gpu = metal.engine_gpu_wait_us();
    std::printf("[grid-time] size=%u subcarriers=%u plain=%.1fus/transform (gpu %.1f) with-grid=%.1fus/transform (gpu %.1f)\n",
                size,
                nof_subc,
                std::chrono::duration<double, std::micro>(t1 - t0).count() / iters,
                plain_gpu,
                std::chrono::duration<double, std::micro>(t2 - t1).count() / iters,
                grid_gpu);
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
