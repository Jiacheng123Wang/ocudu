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
#include "ocudu/phy/phy_pipeline_grid_ready.h"
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
  // D1 (design document 5.9.17): the hand-over has to be ARMED BEFORE the first Metal DFT engine exists -
  // the engine picks its queue once - so it is armed here for the whole binary. It only changes what the
  // sections that DECLARE the grid device-consumed do (see the armed section below); every other section
  // commits normally and compares the same values.
  ::setenv("OCUDU_DFT_RELEASE_BLOCK", "1", 1);
  ::setenv("OCUDU_GPU_STRICT", "1", 1);

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
      // The receiving slot, as the lower PHY's FSM tells it (set_lane_slot()): it is what the device
      // probe groups this slot's transforms by, so this test exercises that half of the device
      // timeline too - its report is the "[ul_gpu_lane] dft ..." series on stderr.
      demod.set_lane_slot(1);
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

    // ---- The ARMED hand-over (D1, 5.9.17): the grid is produced by a block this test, as the host consumer,
    // must ask for ---------------------------------------------------------------------------------------
    //
    // This is the offline judge the air legs could not be: the receiving chain hands the slot's block over
    // UNCOMMITTED instead of committing it (OCUDU_DFT_RELEASE_BLOCK, armed above) and the grid is then
    // produced at whoever claims it. Here nobody claims it - there is no PUSCH hop in a demodulator test -
    // so the CONSUMER commits it, exactly as the PUCCH path does in the receiving chain
    // (grid_ready_hook::wait()), and only then is the grid read and compared, byte for byte, against the
    // host path's. A single differing resource element is a defect in the hand-over itself, with the radio,
    // the UE and the lane taken out of the picture.
    {
      ofdm_demodulator_configuration armed_config = device_config;
      // The declaration the hand-over needs (it is what says the grid's consumers read it on the device -
      // and a host reader waits, which is what this section performs).
      armed_config.grid_consumed_on_device = true;

      auto armed_demod = metal_factory->create_ofdm_symbol_demodulator(armed_config);
      auto ref_demod   = metal_factory->create_ofdm_symbol_demodulator(config);
      if ((armed_demod == nullptr) || (ref_demod == nullptr)) {
        std::fprintf(stderr, "FAIL: demodulator creation for the armed hand-over\n");
        return 1;
      }
      auto grid_armed = grid_factory->create(1, nsymb, rg_size);
      auto grid_ref   = grid_factory->create(1, nsymb, rg_size);
      if ((grid_armed == nullptr) || (grid_ref == nullptr)) {
        std::fprintf(stderr, "FAIL: grid creation for the armed hand-over\n");
        return 1;
      }
      run_pipelined(*armed_demod, *grid_armed);
      run_pipelined(*ref_demod, *grid_ref);

      // The consumer: the host is about to read this grid, so it asks for its production first. This is the
      // call the PUCCH and the SRS make in the receiving chain, and its whole point is that nobody else has
      // committed the block.
      const resource_grid_device_view armed_view = grid_armed->get_writer().get_device_view();
      if (!armed_view.is_valid()) {
        std::fprintf(stderr, "FAIL: the armed section's grid has no device view\n");
        return 1;
      }
      if (!grid_ready_hook::wait(armed_view.base, 1)) {
        std::fprintf(stderr, "FAIL: the armed hand-over's grid was not produced in time\n");
        ok = false;
      }

      std::vector<cf_t> armed_out = grid_to_vector(grid_armed->get_reader(), 1, nsymb, rg_size);
      std::vector<cf_t> ref_out   = grid_to_vector(grid_ref->get_reader(), 1, nsymb, rg_size);
      unsigned          armed_mismatching = 0;
      for (unsigned i = 0; i != armed_out.size(); ++i) {
        if (armed_out[i] != ref_out[i]) {
          if (armed_mismatching == 0) {
            std::fprintf(stderr,
                         "  armed hand-over: first mismatch at RE %u: armed=(%f,%f) reference=(%f,%f)\n",
                         i,
                         armed_out[i].real(),
                         armed_out[i].imag(),
                         ref_out[i].real(),
                         ref_out[i].imag());
          }
          ++armed_mismatching;
        }
      }
      std::printf("[armed] hand-over grid vs host write: REs=%zu mismatching=%u\n", armed_out.size(), armed_mismatching);
      if (armed_mismatching != 0) {
        std::fprintf(stderr, "FAIL: the grid of the armed hand-over differs from the host grid write\n");
        ok = false;
      }
    }

    // The RX pipeline does NOT copy the samples: the transform reads the buffer the caller assembled
    // the symbol in (see dft_grid_write_params::time_samples). The caller - uplink_processor_impl -
    // therefore owns a ring with one buffer per symbol the pipeline can keep in flight and only
    // overwrites a buffer whose transforms have been finished (acquire_symbol_buffer()). The two
    // blocks below check that contract from both sides.
    auto alloc_aligned = [](size_t nof_samples) {
      const size_t page  = compat::page_size();
      const size_t bytes = ((nof_samples * sizeof(ci16_t) + page - 1) / page) * page;
      return static_cast<ci16_t*>(compat::aligned_alloc(page, bytes));
    };

    {
      // (a) The ring, as the RX chain uses it: every symbol is assembled in the buffer of its own
      // slot while the previous symbols are still in flight, and the buffer of a slot is only
      // rewritten after the transform that read it has been finished. Every symbol must come out
      // right - which can only happen if the transform reads the caller's memory and the caller
      // keeps it valid for as long as the transform runs.
      const unsigned depth          = device_demod->get_pipeline_depth();
      const size_t   max_symbol_len = *std::max_element(symbol_sizes.begin(), symbol_sizes.end());
      std::vector<span<ci16_t>> slot_buffer(depth);
      for (span<ci16_t>& buffer : slot_buffer) {
        ci16_t* mem = alloc_aligned(max_symbol_len);
        if (mem == nullptr) {
          std::fprintf(stderr, "FAIL: the aligned symbol-buffer allocation failed\n");
          return 1;
        }
        buffer = span<ci16_t>(mem, max_symbol_len);
      }

      auto run_pipelined_with_ring = [&](ofdm_symbol_demodulator& demod, resource_grid& grid) {
        std::vector<unsigned> in_flight;
        unsigned              offset = 0;
        for (unsigned s = 0; s != nsymb; ++s) {
          // The slot about to be reused holds the oldest in-flight transform: it is finished before
          // the buffer is written again, exactly like acquire_symbol_buffer() does it.
          if (in_flight.size() == depth) {
            demod.finish_symbol(grid.get_writer(), in_flight.front());
            in_flight.erase(in_flight.begin());
          }
          const unsigned slot         = s % depth;
          span<ci16_t>   slot_samples = slot_buffer[slot].first(symbol_sizes[s]);
          std::copy_n(time_data.begin() + offset, symbol_sizes[s], slot_samples.begin());
          demod.submit_symbol(grid.get_writer(), slot_samples, 0, s, slot);
          in_flight.push_back(slot);
          offset += symbol_sizes[s];
        }
        while (!in_flight.empty()) {
          demod.finish_symbol(grid.get_writer(), in_flight.front());
          in_flight.erase(in_flight.begin());
        }
      };

      auto grid_ring = grid_factory->create(1, nsymb, rg_size);
      if (grid_ring == nullptr) {
        std::fprintf(stderr, "FAIL: grid creation for the symbol-buffer ring case\n");
        return 1;
      }
      run_pipelined_with_ring(*device_demod, *grid_ring);
      std::vector<cf_t> ring_out = grid_to_vector(grid_ring->get_reader(), 1, nsymb, rg_size);
      unsigned          ring_mismatching = 0;
      for (unsigned i = 0; i != ring_out.size(); ++i) {
        if (ring_out[i] != host_out[i]) {
          ++ring_mismatching;
        }
      }
      std::printf("[reuse] one buffer per symbol in flight: REs=%zu mismatching=%u\n",
                  ring_out.size(),
                  ring_mismatching);
      if (ring_mismatching != 0) {
        std::fprintf(stderr,
                     "FAIL: a symbol came out wrong although its buffer was kept valid until its transform was "
                     "finished\n");
        ok = false;
      }
    }

    {
      // (b) Negative control: overwriting the samples of a transform that is STILL IN FLIGHT has to
      // corrupt the grid it produces. If it does not, the input is being staged (copied) somewhere
      // on the host and the zero-copy input is vacuous - which is the trap this whole path was
      // rewritten twice for. The samples are overwritten the moment they are submitted, exactly like
      // the single-buffer RX chain used to do it (S-7f-6c, the leg that made the phone fail to
      // attach).
      ci16_t* probe_mem = alloc_aligned(slot_size);
      if (probe_mem == nullptr) {
        std::fprintf(stderr, "FAIL: the aligned probe allocation failed\n");
        return 1;
      }
      span<ci16_t> probe(probe_mem, slot_size);
      std::copy(time_data.begin(), time_data.end(), probe.begin());

      auto run_pipelined_reusing_buffer = [&](ofdm_symbol_demodulator& demod, resource_grid& grid) {
        const unsigned        depth = demod.get_pipeline_depth();
        std::vector<unsigned> in_flight;
        unsigned              offset = 0;
        for (unsigned s = 0; s != nsymb; ++s) {
          if (in_flight.size() == depth) {
            demod.finish_symbol(grid.get_writer(), in_flight.front());
            in_flight.erase(in_flight.begin());
          }
          span<ci16_t> symbol_samples = probe.subspan(offset, symbol_sizes[s]);
          const unsigned slot         = s % depth;
          demod.submit_symbol(grid.get_writer(), symbol_samples, 0, s, slot);
          // The next symbol is assembled into the same buffer right away.
          for (ci16_t& sample : symbol_samples) {
            sample = ci16_t(-32768, 32767);
          }
          in_flight.push_back(slot);
          offset += symbol_sizes[s];
        }
        while (!in_flight.empty()) {
          demod.finish_symbol(grid.get_writer(), in_flight.front());
          in_flight.erase(in_flight.begin());
        }
      };

      auto grid_reuse = grid_factory->create(1, nsymb, rg_size);
      if (grid_reuse == nullptr) {
        std::fprintf(stderr, "FAIL: grid creation for the buffer-reuse case\n");
        return 1;
      }
      run_pipelined_reusing_buffer(*device_demod, *grid_reuse);
      std::vector<cf_t> reuse_out = grid_to_vector(grid_reuse->get_reader(), 1, nsymb, rg_size);
      unsigned          reuse_mismatching = 0;
      for (unsigned i = 0; i != reuse_out.size(); ++i) {
        if (reuse_out[i] != host_out[i]) {
          ++reuse_mismatching;
        }
      }
      std::printf("[reuse] samples overwritten right after submit: REs=%zu mismatching=%u\n",
                  reuse_out.size(),
                  reuse_mismatching);
      if (reuse_mismatching == 0) {
        std::fprintf(stderr,
                     "FAIL: overwriting the samples of an in-flight transform did not corrupt its grid, so the "
                     "transform is not reading the buffer it was given (the input is still staged)\n");
        ok = false;
      }
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
