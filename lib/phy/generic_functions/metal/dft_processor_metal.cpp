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

bool dft_processor_metal::supports_grid_write(const resource_grid_device_view& view) const
{
  if (!valid || (engine == nullptr) || !view.is_valid()) {
    return false;
  }
  // The engine rotates the transform output into the grid, so the grid cannot be wider than the transform, and the
  // layout has to be the one the kernel addresses (contiguous subcarriers within a symbol).
  return (view.nof_subc != 0) && (view.nof_subc <= cfg.size) && (view.subc_stride == 1) &&
         (view.symb_stride >= view.nof_subc) && (view.port_stride >= view.nof_symb * view.symb_stride) && (view.nof_ports != 0);
}

bool dft_processor_metal::set_grid_write_window(span<const cf_t> window)
{
  if (engine == nullptr) {
    return false;
  }
  return engine->set_grid_write_window(window.empty() ? nullptr : window.data(), window.size());
}

bool dft_processor_metal::submit_grid_write(unsigned slot, const dft_grid_write_params& params)
{
  if (!supports_grid_write(params.view) || (params.nof_subc > cfg.size) || (params.port >= params.view.nof_ports) ||
      (params.symbol >= params.view.nof_symb)) {
    return false;
  }

  metal::dft_metal_engine::grid_write write;
  write.grid_base   = params.view.base;
  write.grid_bytes  = static_cast<size_t>(params.view.nof_ports) * params.view.port_stride * sizeof(cbf16_t);
  write.dst_offset  = params.view.get_symbol_offset(params.port, params.symbol);
  write.nof_subc    = params.nof_subc;
  write.map_offset  = params.map_offset % cfg.size;
  write.phase_re    = params.coefficient.real();
  write.phase_im    = params.coefficient.imag();
  write.apply_window = params.apply_window;
  // The transform input, when the caller hands over the radio's own int16 buffer instead of filling
  // the engine's float2 ring (see dft_grid_write_params::time_samples).
  write.time_samples       = params.time_samples;
  write.time_samples_bytes = params.time_samples_bytes;
  write.time_window_start  = params.time_window_start;
  write.time_gain          = params.time_gain;
  return engine->submit_slot_grid_write(input.get(), output.get(), slot, write);
}
