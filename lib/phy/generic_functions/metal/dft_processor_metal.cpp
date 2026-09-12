// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "dft_processor_metal.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;

bool dft_processor_metal::is_supported_size(unsigned size)
{
  if (size < 2 || size > metal::dft_metal_engine::max_size) {
    return false;
  }
  // The kernel covers the 2^k * 3^m family (every NR OFDM FFT size, e.g. 384/512/768/1024/
  // 1536/2048/3072); anything else (e.g. the PRACH FFT sizes) stays on the CPU implementation.
  while (size % 2 == 0) {
    size /= 2;
  }
  while (size % 3 == 0) {
    size /= 3;
  }
  return size == 1;
}

dft_processor_metal::dft_processor_metal(const configuration& config) : cfg(config), dir(config.dir)
{
  if (!is_supported_size(config.size)) {
    return; // Invalid: is_valid() stays false and the factory falls back.
  }

  // Page-aligned input/output buffers (the zero-copy wrap contract); compat::aligned_alloc
  // rounds the size up to a page multiple. They hold up to max_batch transforms so a whole
  // slot's symbols can be filled first and executed by a single run_batch() dispatch.
  const size_t page  = compat::page_size();
  const size_t bytes = static_cast<size_t>(config.size) * max_batch * sizeof(cf_t);
  void*        in    = compat::aligned_alloc(page, bytes);
  void*        out   = compat::aligned_alloc(page, bytes);
  if (in == nullptr || out == nullptr) {
    compat::aligned_free(in);
    compat::aligned_free(out);
    return;
  }
  input.reset(static_cast<cf_t*>(in));
  output.reset(static_cast<cf_t*>(out));

  engine = std::make_unique<metal::dft_metal_engine>();
  if (!engine->init(config.size, config.dir == direction::INVERSE)) {
    // Keep the buffers (harmless); is_valid() reports the failure and the factory falls back.
    return;
  }
  valid = true;
}

span<const cf_t> dft_processor_metal::run()
{
  report_fatal_error_if_not(valid, "Metal DFT processor is not valid (unsupported size or engine init failed).");
  report_fatal_error_if_not(engine->run(input.get(), output.get(), 1), "Metal DFT run failed.");
  return {output.get(), cfg.size};
}

span<const cf_t> dft_processor_metal::run_batch(unsigned nof_transforms)
{
  report_fatal_error_if_not(valid, "Metal DFT processor is not valid (unsupported size or engine init failed).");
  report_fatal_error_if_not(nof_transforms >= 1 && nof_transforms <= max_batch,
                            "Invalid Metal DFT batch size ({}), must be in [1, {}].",
                            nof_transforms,
                            max_batch);
  if (nof_transforms == 1) {
    return run();
  }
  report_fatal_error_if_not(engine->run(input.get(), output.get(), nof_transforms), "Metal DFT batch run failed.");
  return {output.get(), static_cast<size_t>(cfg.size) * nof_transforms};
}
