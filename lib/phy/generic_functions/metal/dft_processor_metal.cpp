// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "dft_processor_metal.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;

bool dft_processor_metal::is_supported_size(unsigned size)
{
  return (size >= 2) && (size <= metal::dft_metal_engine::max_size) && ((size & (size - 1)) == 0);
}

dft_processor_metal::dft_processor_metal(const configuration& config) : cfg(config), dir(config.dir)
{
  if (!is_supported_size(config.size)) {
    return; // Invalid: is_valid() stays false and the factory falls back.
  }

  // Page-aligned input/output buffers (the zero-copy wrap contract); compat::aligned_alloc
  // rounds the size up to a page multiple.
  const size_t page = compat::page_size();
  void*        in   = compat::aligned_alloc(page, static_cast<size_t>(config.size) * sizeof(cf_t));
  void*        out  = compat::aligned_alloc(page, static_cast<size_t>(config.size) * sizeof(cf_t));
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
  report_fatal_error_if_not(engine->run(input.get(), output.get()), "Metal DFT run failed.");
  return {output.get(), cfg.size};
}
