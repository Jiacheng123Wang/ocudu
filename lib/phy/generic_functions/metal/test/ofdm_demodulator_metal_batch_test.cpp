// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Slot-level A/B of the batched Metal DFT in the OFDM demodulator.
///
/// \note This exercises the batched entry point (dft_processor::run_batch()), which the gNB RX
/// path does not use today: the radio-paced puxch path demodulates one symbol per call. The A/B is
/// kept so the batched path stays validated until the planned multi-port and multi-carrier
/// batching starts using it (see the TODOs in dft_processor.h and ofdm_demodulator.h). It also
/// documents why batching several symbols of one stream is not a latency win: the samples of a
/// stream arrive symbol by symbol.
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
  // The RX chain's samples live in a page-aligned, page-multiple allocation
  // (baseband_gateway_buffer_dynamic_aligned), which is what lets the DFT engine read them zero-copy
  // (see dft_grid_write_params::time_samples). A plain vector would make the engine refuse and stage
  // the input on the host, so the test would not exercise that path at all.
  const size_t page      = compat::page_size();
  const size_t data_page = ((static_cast<size_t>(slot_size) * sizeof(ci16_t) + page - 1) / page) * page;
  ci16_t*      aligned_time_data = static_cast<ci16_t*>(compat::aligned_alloc(page, data_page));
  if (aligned_time_data == nullptr) {
    std::fprintf(stderr, "FAIL: the aligned sample allocation failed\n");
    return 1;
  }
  span<ci16_t> time_data(aligned_time_data, slot_size);
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


  // Device grid write (S-7b-3): the pipelined path the gNB actually uses (submit_symbol / finish_symbol, one transform
  // per symbol) with the grid written from the device must produce EXACTLY the grid the host path produces - the same
  // transform values, the same compensation and the same bf16 rounding. A single differing resource element fails the
  // check: the grid feeds the channel estimator and the equalizer, so a drift here would only surface much later.
  {
    ofdm_demodulator_configuration device_config = config;
    device_config.device_grid_write               = true;

    auto device_demod = metal_factory->create_ofdm_symbol_demodulator(device_config);
    auto host_demod   = metal_factory->create_ofdm_symbol_demodulator(config);
    if ((device_demod == nullptr) || (host_demod == nullptr)) {
      std::fprintf(stderr, "FAIL: demodulator creation for the device grid write\n");
      return 1;
    }
    if (device_demod->get_pipeline_depth() <= 1) {
      std::fprintf(stderr, "FAIL: the Metal DFT must expose a transform pipeline\n");
      return 1;
    }

    auto grid_device = grid_factory->create(1, nsymb, rg_size);
    auto grid_host   = grid_factory->create(1, nsymb, rg_size);
    if ((grid_device == nullptr) || (grid_host == nullptr)) {
      std::fprintf(stderr, "FAIL: grid creation for the device grid write\n");
      return 1;
    }

    // Runs a whole slot through the pipelined path of one demodulator, with the ring depth the implementation reports.
    auto run_pipelined = [&](ofdm_symbol_demodulator& demod, resource_grid& grid) {
      const unsigned        depth = demod.get_pipeline_depth();
      std::vector<unsigned> in_flight;
      unsigned              offset = 0;
      for (unsigned s = 0; s != nsymb; ++s) {
        // Keep the ring at `depth` transforms in flight: the slot about to be reused holds the oldest one.
        if (in_flight.size() == depth) {
          demod.finish_symbol(grid.get_writer(), in_flight.front());
          in_flight.erase(in_flight.begin());
        }
        span<const ci16_t> symbol_samples = time_data.subspan(offset, symbol_sizes[s]);
        const unsigned slot = s % depth;
        demod.submit_symbol(grid.get_writer(), symbol_samples, 0, s, slot);
        in_flight.push_back(slot);
        offset += symbol_sizes[s];
      }
      while (!in_flight.empty()) {
        demod.finish_symbol(grid.get_writer(), in_flight.front());
        in_flight.erase(in_flight.begin());
      }
    };

    run_pipelined(*device_demod, *grid_device);
    run_pipelined(*host_demod, *grid_host);

    std::vector<cf_t> device_out = grid_to_vector(grid_device->get_reader(), 1, nsymb, rg_size);
    std::vector<cf_t> host_out   = grid_to_vector(grid_host->get_reader(), 1, nsymb, rg_size);

    unsigned mismatching = 0;
    for (unsigned i = 0; i != device_out.size(); ++i) {
      if ((device_out[i] != host_out[i])) {
        if (mismatching == 0) {
          std::fprintf(stderr,
                       "  first mismatch at RE %u: device=(%f,%f) host=(%f,%f)\n",
                       i,
                       device_out[i].real(),
                       device_out[i].imag(),
                       host_out[i].real(),
                       host_out[i].imag());
        }
        ++mismatching;
      }
    }
    std::printf("[grid]  pipelined device write vs host write: REs=%zu mismatching=%u\n", device_out.size(), mismatching);
    if (mismatching != 0) {
      std::fprintf(stderr, "FAIL: the device grid write differs from the host grid write\n");
      ok = false;
    }

    // Latency of the two paths (the device one must not pay a round trip per symbol for the grid). Informational only:
    // the first rounds of a process carry the GPU's clock ramp-up on this machine, so the numbers are averaged over
    // enough rounds to smooth it (the authoritative cost of the grid write is measured warm in
    // dft_processor_metal_unit_test, where it comes out the same as the plain store).
    run_pipelined(*device_demod, *grid_device);
    run_pipelined(*host_demod, *grid_host);
    constexpr unsigned grid_rounds = 50;
    auto               g0          = std::chrono::steady_clock::now();
    for (unsigned r = 0; r != grid_rounds; ++r) {
      run_pipelined(*device_demod, *grid_device);
    }
    auto g1 = std::chrono::steady_clock::now();
    for (unsigned r = 0; r != grid_rounds; ++r) {
      run_pipelined(*host_demod, *grid_host);
    }
    auto         g2            = std::chrono::steady_clock::now();
    const double device_slot_us = std::chrono::duration<double, std::micro>(g1 - g0).count() / grid_rounds;
    const double host_slot_us   = std::chrono::duration<double, std::micro>(g2 - g1).count() / grid_rounds;
    std::printf("[time] slot pipelined: device-grid=%.1fus host-grid=%.1fus\n", device_slot_us, host_slot_us);
  }

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
