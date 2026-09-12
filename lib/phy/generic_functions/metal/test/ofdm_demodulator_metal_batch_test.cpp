// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Slot-level A/B of the batched Metal DFT in the OFDM demodulator.
///
/// Demodulates the same slot twice with the generic OFDM demodulator: once with the Metal DFT
/// processor (which executes all the symbols of the slot in a single dispatch through
/// dft_processor::run_batch()) and once with the CPU reference DFT (one transform per call).
/// The two resource grids must match within an NMSE gate, which validates the batched
/// fill/run/process path end to end, plus the slot latency of both paths.

#include "ocudu/adt/span.h"
#include "ocudu/phy/generic_functions/dft_processor.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/lower/modulation/modulation_factories.h"
#include "ocudu/phy/lower/modulation/ofdm_demodulator.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
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
  return (den > 0.0) ? 10.0 * std::log10(num / den) : -300.0;
}

/// Copies the whole resource grid into a flat vector for comparison.
std::vector<cf_t> grid_to_vector(const resource_grid_reader& grid, unsigned nof_ports, unsigned nof_symbols, unsigned rg_size)
{
  std::vector<cf_t> out(static_cast<size_t>(nof_ports) * nof_symbols * rg_size);
  std::vector<cf_t> symbol(rg_size);
  for (unsigned p = 0; p != nof_ports; ++p) {
    for (unsigned s = 0; s != nof_symbols; ++s) {
      grid.get(symbol, p, s, 0);
      std::copy(symbol.begin(), symbol.end(), out.begin() + (static_cast<size_t>(p) * nof_symbols + s) * rg_size);
    }
  }
  return out;
}

} // namespace

int main()
{
  // Valid size for both backends: 2048 = 2^11, 106 RB grid.
  const unsigned          dft_size = 2048;
  const unsigned          bw_rb    = 106;
  const subcarrier_spacing scs     = subcarrier_spacing::kHz30;
  const cyclic_prefix      cp      = cyclic_prefix::NORMAL;
  const unsigned           rg_size = bw_rb * NOF_SUBCARRIERS_PER_RB;
  const unsigned           nsymb   = get_nsymb_per_slot(cp);
  const unsigned           slot_index = 0;
  const float              scale      = 1.0e-4F;

  // Random time-domain samples for the whole slot.
  unsigned sampling_rate_Hz = to_sampling_rate_Hz(scs, dft_size);
  unsigned slot_size        = 0;
  std::vector<unsigned> symbol_sizes(nsymb);
  for (unsigned s = 0; s != nsymb; ++s) {
    symbol_sizes[s] = cp.get_length(nsymb * slot_index + s, scs).to_samples(sampling_rate_Hz) + dft_size;
    slot_size += symbol_sizes[s];
  }
  std::vector<ci16_t> time_data(slot_size);
  std::mt19937        rng(20260912);
  std::uniform_int_distribution<int> dist(-2000, 2000);
  for (auto& sample : time_data) {
    sample = ci16_t(static_cast<int16_t>(dist(rng)), static_cast<int16_t>(dist(rng)));
  }

  ofdm_demodulator_configuration config = {};
  config.numerology               = static_cast<unsigned>(scs);
  config.bw_rb                    = bw_rb;
  config.dft_size                 = dft_size;
  config.cp                       = cp;
  config.nof_samples_window_offset = 0;
  config.scale                    = scale;
  config.center_freq_Hz           = 3.5e9;

  // Metal DFT factory (batching) and CPU reference DFT factory.
  std::shared_ptr<dft_processor_factory> metal_dft_factory = create_dft_processor_factory_metal();
  std::shared_ptr<dft_processor_factory> cpu_dft_factory   = create_dft_processor_factory_fftz();
  if (cpu_dft_factory == nullptr) {
    cpu_dft_factory = create_dft_processor_factory_generic();
  }
  auto grid_factory      = create_resource_grid_factory();
  if ((metal_dft_factory == nullptr) || (cpu_dft_factory == nullptr) || (grid_factory == nullptr)) {
    std::fprintf(stderr, "FAIL: factory creation\n");
    return 1;
  }

  ofdm_factory_generic_configuration metal_common;
  metal_common.dft_factory = metal_dft_factory;
  ofdm_factory_generic_configuration cpu_common;
  cpu_common.dft_factory = cpu_dft_factory;

  auto metal_factory = create_ofdm_demodulator_factory_generic(metal_common);
  auto cpu_factory   = create_ofdm_demodulator_factory_generic(cpu_common);
  if ((metal_factory == nullptr) || (cpu_factory == nullptr)) {
    std::fprintf(stderr, "FAIL: demodulator factory creation\n");
    return 1;
  }

  auto metal_demod = metal_factory->create_ofdm_slot_demodulator(config);
  auto cpu_demod   = cpu_factory->create_ofdm_slot_demodulator(config);
  if ((metal_demod == nullptr) || (cpu_demod == nullptr)) {
    std::fprintf(stderr, "FAIL: demodulator creation\n");
    return 1;
  }

  auto grid_metal = grid_factory->create(1, nsymb, rg_size);
  auto grid_cpu   = grid_factory->create(1, nsymb, rg_size);
  if ((grid_metal == nullptr) || (grid_cpu == nullptr)) {
    std::fprintf(stderr, "FAIL: grid creation\n");
    return 1;
  }

  metal_demod->demodulate(grid_metal->get_writer(), time_data, 0, slot_index);
  cpu_demod->demodulate(grid_cpu->get_writer(), time_data, 0, slot_index);

  std::vector<cf_t> out_metal = grid_to_vector(grid_metal->get_reader(), 1, nsymb, rg_size);
  std::vector<cf_t> out_cpu   = grid_to_vector(grid_cpu->get_reader(), 1, nsymb, rg_size);

  const double nmse = nmse_db(span<const cf_t>(out_cpu), span<const cf_t>(out_metal));
  std::printf("[A/B] slot demodulation (metal batched vs cpu per-symbol): nmse=%.2f dB\n", nmse);

  bool ok = (nmse <= -60.0);

  // Latency: one batched slot versus the per-symbol path.
  constexpr unsigned rounds = 20;
  auto t0 = std::chrono::steady_clock::now();
  for (unsigned r = 0; r != rounds; ++r) {
    metal_demod->demodulate(grid_metal->get_writer(), time_data, 0, slot_index);
  }
  auto t1 = std::chrono::steady_clock::now();
  for (unsigned r = 0; r != rounds; ++r) {
    cpu_demod->demodulate(grid_cpu->get_writer(), time_data, 0, slot_index);
  }
  auto         t2       = std::chrono::steady_clock::now();
  const double metal_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / rounds;
  const double cpu_us   = std::chrono::duration<double, std::micro>(t2 - t1).count() / rounds;
  std::printf("[time] slot demodulation: metal(batched)=%.1fus cpu(per-symbol)=%.1fus (%.1fx)\n",
              metal_us,
              cpu_us,
              cpu_us / metal_us);

  if (ok) {
    std::printf("ALL OK\n");
    return 0;
  }
  std::fprintf(stderr, "FAILED\n");
  return 1;
}
