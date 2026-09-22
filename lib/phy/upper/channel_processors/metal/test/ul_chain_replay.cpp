// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Replays a recorded PUSCH reception through a selected receiver configuration.
///
/// The PUSCH processor can capture its input (OCUDU_UL_DUMP): the PDU configuration and the
/// received resource grid, see lib/phy/upper/channel_processors/pusch/ul_capture.cpp.  This tool
/// reads one of those captures, rebuilds the grid and pushes it through a receiver whose back ends
/// are chosen on the command line, writing its own staged capture (OCUDU_UL_DUMP) on the way.
///
/// Because every configuration consumes the very same recorded vector, the staged captures can be
/// compared bit by bit: run the tool once per configuration and diff the outputs with
/// scripts/ul_stage_diff.py.  The first stage that differs localizes the defect - the grid covers
/// the DFT (a grid capture is post-DFT, so a vector recorded before it replays the upper chain
/// only), the estimator scalars cover the channel estimation and the soft bits cover equalization,
/// demapping and the soft-bit scale.
///
/// Usage:
///   ul_chain_replay <capture-prefix> --out <prefix> [--slot N --rnti R] [--index N]
///                   [--cpu | --metal | --metal-cpu-ldpc | --metal-cpu-ldpc-cpu-demod]
///                   [--dft [--dft-metal] [--device-grid] [--synth N] [--synth-first-slot S] [--reuse-grid]]
///                                             replay a RECORDED TIME-DOMAIN capture (OCUDU_UL_DUMP_TD)
///                                             through the OFDM demodulator and dump the grids: the
///                                             same IQ through two builds is the A/B of the DFT path,
///                                             and --device-grid switches the grid write to the device.
///
/// --dft is the L1 OFFLINE HARNESS for the D1 hand-over (design document 5.9.37). This tool is the grid's
/// CONSUMER there: it asks for the grid's production (grid_ready_hook::wait) before it reads one, exactly
/// as the PUCCH does in the receiving chain, and it ASSERTS that a hand-over actually happened. Run it
/// twice - with and without OCUDU_DFT_RELEASE_BLOCK=1 - and the two grid dumps must be byte-identical;
/// the arm that drops the consumer's wait must differ. --synth makes the corpus itself, so the mechanism
/// can be judged with no recorded capture, no radio and no UE.
///
/// Example (the A/B/C comparison of one recorded over-the-air reception):
///   ul_chain_replay /tmp/C --cpu              --out /tmp/replay_cpu
///   ul_chain_replay /tmp/C --metal            --out /tmp/replay_gpu
///   scripts/ul_stage_diff.py /tmp/replay_cpu /tmp/replay_gpu
///
/// \warning DO NOT TRUST A DIFFERENCE FOUND UNDER HEAVY PARALLELISM (S-7f-4j). Run many instances at
/// once - ten was enough - and this tool intermittently produces WRONG results, not merely missing
/// ones: measured, 39 of 980 captures came out different between two runs of the SAME configuration,
/// and the SINR of an unchanged configuration moved between 7.3 and 48.9 dB. Every one of those
/// captures was clean when re-run serially, and the same corruption can also strike a re-run that
/// still has other instances running. So: re-check any difference serially (with nothing else on the
/// GPU) before believing it, and prefer a low instance count for anything whose result is a
/// measurement. The known intermittent failure rx_buffer_impl::get_codeblock_data_bits is the
/// visible half of this; the silent half is what this warning is about.

#include "ocudu/adt/span.h"
#include "ocudu/phy/lower/modulation/modulation_factories.h"
#include "ocudu/phy/lower/modulation/ofdm_demodulator.h"
// The D1 hand-over as the CONSUMER sees it (see --dft and write_grid()): the hook is a no-op in a build
// without Metal, which is what keeps this tool usable as a plain replay.
#include "ocudu/phy/phy_pipeline_grid_ready.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor_result_notifier.h"
#include "ocudu/phy/upper/rx_buffer_pool.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include "ocudu/ran/pusch/ulsch_info.h"
#include "ocudu/ran/sch/tbs_calculator.h"
#include "ocudu/support/executors/task_worker_pool.h"

#include "channel_equalizer_metal_factory.h"
#include "demodulation_mapper_metal_factory.h"

#include <random>

#include <algorithm>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <set>
#include <tuple>
#include <vector>

using namespace ocudu;

namespace {

/// Upper bound used to size the soft buffer pool (the largest transport block of the widest BWP).
static constexpr unsigned MAX_NOF_CODEBLOCKS = 64;


/// Parsed capture of one reception.
struct capture_t {
  std::map<std::string, std::string> fields;
  std::string                        bin_path;
  unsigned                           nof_ports = 1;
  unsigned                           nof_subc  = 0;

  unsigned get_unsigned(const std::string& key, unsigned fallback = 0) const
  {
    auto it = fields.find(key);
    return (it != fields.end()) ? static_cast<unsigned>(std::strtoul(it->second.c_str(), nullptr, 10)) : fallback;
  }

  std::string get_string(const std::string& key, const std::string& fallback = "") const
  {
    auto it = fields.find(key);
    return (it != fields.end()) ? it->second : fallback;
  }

  std::vector<unsigned> get_list(const std::string& key) const
  {
    std::vector<unsigned> out;
    std::string           value = get_string(key);
    std::stringstream     stream(value);
    std::string           item;
    while (std::getline(stream, item, ',')) {
      if (!item.empty()) {
        out.push_back(static_cast<unsigned>(std::strtoul(item.c_str(), nullptr, 10)));
      }
    }
    return out;
  }
};

modulation_scheme to_modulation(const std::string& name)
{
  if (name == "QPSK") return modulation_scheme::QPSK;
  if (name == "16QAM") return modulation_scheme::QAM16;
  if (name == "64QAM") return modulation_scheme::QAM64;
  if (name == "256QAM") return modulation_scheme::QAM256;
  if (name == "BPSK") return modulation_scheme::BPSK;
  return modulation_scheme::PI_2_BPSK;
}

subcarrier_spacing to_scs(unsigned khz)
{
  switch (khz) {
    case 15: return subcarrier_spacing::kHz15;
    case 30: return subcarrier_spacing::kHz30;
    case 60: return subcarrier_spacing::kHz60;
    default: return subcarrier_spacing::kHz120;
  }
}

bool parse_capture(const std::string& base, capture_t& out)
{
  const std::string txt = base + ".txt";
  std::ifstream     file(txt);
  if (!file.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    auto pos = line.find('=');
    if ((pos == std::string::npos) || (pos == 0)) {
      continue;
    }
    out.fields[line.substr(0, pos)] = line.substr(pos + 1);
  }
  return true;
}

/// \brief The receiving chain's front end, for the L1b harness (design document 5.9.38).
///
/// The upper half of the L1 harness (`--dft`) judges the FRONT END: it produces a slot's grid and, as the
/// grid's host consumer, asks for its production. It never builds a PUSCH receiver, so the other half of
/// the hand-over - the HOP that ADOPTS the front end's block and appends its own dispatches to the very
/// same command buffer - is not in it at all. That half is what makes D1's headline property (front end,
/// channel estimation, equalization and demapping are ONE submission), and until it can be replayed
/// offline the property can only be judged on air.
///
/// This class is that front end for a replay: it takes SYNTHETIC time-domain input (the criterion is "the
/// same input through the same pipeline twice", so the content is irrelevant - see --synth) and produces
/// each slot's resource grid on the DEVICE, handing the slot's block over uncommitted when the hand-over
/// is armed, exactly as the lower PHY does - with no radio, no slot FSM and no capture file.
class hop_front_end
{
public:
  /// \param[in] dft_metal     Run the transform on the device (the hand-over needs the Metal engine).
  /// \param[in] device_grid   Write the grid from the device (the shape the hand-over exists for).
  /// \param[in] nof_prb       Width of the grid, in PRBs.
  /// \param[in] scs           Subcarrier spacing of the corpus.
  bool init(bool dft_metal, bool device_grid, unsigned nof_prb, subcarrier_spacing scs)
  {
    dft_size = 1;
    while (dft_size < (nof_prb * NOF_SUBCARRIERS_PER_RB)) {
      dft_size *= 2;
    }
    sampling_rate_Hz = to_sampling_rate_Hz(scs, dft_size);
    bw_rb            = nof_prb;

    ofdm_factory_generic_configuration ofdm_config = {
        .dft_factory = dft_metal ? create_dft_processor_factory_metal() : create_dft_processor_factory()};
    std::shared_ptr<ofdm_demodulator_factory> factory = create_ofdm_demodulator_factory_generic(ofdm_config);
    if (factory == nullptr) {
      return false;
    }
    ofdm_demodulator_configuration config = {};
    config.numerology                = 0;
    config.bw_rb                     = nof_prb;
    config.dft_size                  = dft_size;
    config.cp                        = cyclic_prefix::NORMAL;
    config.nof_samples_window_offset = 0;
    config.scale                     = 1.0F;
    config.center_freq_Hz            = 0.0;
    config.device_grid_write         = device_grid;
    // The same declaration the receiving chain makes: the grid's consumers read it on the DEVICE, and a
    // host reader of it waits. Without it the release path is not even reached.
    config.grid_consumed_on_device   = true;
    demodulator                      = factory->create_ofdm_symbol_demodulator(config);
    grid_factory                     = create_resource_grid_factory();
    return (demodulator != nullptr) && (grid_factory != nullptr);
  }

  /// \brief Produces \p slot 's grid from synthetic samples, on the device, and returns it.
  ///
  /// The samples are generated from a seed derived from the slot, so the corpus is reproducible and both
  /// arms of an A/B get byte-identical input. The slot is handed to the device backend
  /// (set_lane_slot) because it is HALF OF THE HAND-OVER KEY: the hop looks the block up by
  /// (grid storage, receiving slot), and a producer that did not say which slot it was depositing for
  /// would leave the hop looking for a key nobody used (see the harness's assertions).
  std::shared_ptr<resource_grid> produce(uint64_t slot)
  {
    std::shared_ptr<resource_grid> grid = grid_factory->create(1, MAX_NSYMB_PER_SLOT, bw_rb * NOF_SUBCARRIERS_PER_RB);
    if (grid == nullptr) {
      return nullptr;
    }
    demodulator->set_lane_slot(slot);

    std::mt19937                       rng(20260921 + slot);
    std::uniform_int_distribution<int> dist(-2000, 2000);
    const unsigned                     depth    = demodulator->get_pipeline_depth();
    const cyclic_prefix                dft_cp   = cyclic_prefix::NORMAL;
    std::vector<ci16_t>                samples;
    for (unsigned symbol = 0; symbol != MAX_NSYMB_PER_SLOT; ++symbol) {
      const unsigned cp_len = dft_cp.get_length(symbol, subcarrier_spacing::kHz15).to_samples(sampling_rate_Hz);
      samples.resize(cp_len + dft_size);
      for (ci16_t& sample : samples) {
        sample = ci16_t(static_cast<int16_t>(dist(rng)), static_cast<int16_t>(dist(rng)));
      }
      if (depth > 1) {
        // The ring is kept at `depth` transforms in flight, exactly as the receiving chain does: the slot
        // about to be reused holds the oldest one.
        if (in_flight.size() == depth) {
          demodulator->finish_symbol(grid->get_writer(), in_flight.front());
          in_flight.erase(in_flight.begin());
        }
        const unsigned ring_slot = ring++ % depth;
        demodulator->submit_symbol(grid->get_writer(), samples, 0, symbol, ring_slot);
        in_flight.push_back(ring_slot);
      } else {
        demodulator->demodulate(grid->get_writer(), samples, 0, symbol);
      }
    }
    // Close the slot: the last finish_symbol() is what ends the block - and, when the hand-over is armed,
    // what hands it over. Nothing is committed here; the hop that reads this grid commits it.
    while (!in_flight.empty()) {
      demodulator->finish_symbol(grid->get_writer(), in_flight.front());
      in_flight.erase(in_flight.begin());
    }
    ring = 0;
    return grid;
  }

  /// The slot's grid storage, i.e. the first half of the hand-over key (the other half is the slot).
  static const void* storage(const std::shared_ptr<resource_grid>& grid)
  {
    const resource_grid_device_view view = grid->get_writer().get_device_view();
    return view.is_valid() ? view.base : nullptr;
  }

private:
  std::unique_ptr<ofdm_symbol_demodulator> demodulator;
  std::shared_ptr<resource_grid_factory>   grid_factory;
  std::vector<unsigned>                    in_flight;
  unsigned                                 ring             = 0;
  unsigned                                 dft_size         = 0;
  unsigned                                 bw_rb            = 0;
  unsigned                                 sampling_rate_Hz = 0;
};

/// Captures the result of one PUSCH processing.
class result_spy : public pusch_processor_result_notifier
{
public:
  void on_uci(const pusch_processor_result_control& uci) override
  {
    if (std::getenv("OCUDU_REPLAY_TRACE") != nullptr) {
      std::fprintf(stderr, "[replay] on_uci\n");
    }
    uci_ok = uci.harq_ack.status == uci_status::valid;
  }

  void on_sch(const pusch_processor_result_data& sch) override
  {
    if (std::getenv("OCUDU_REPLAY_TRACE") != nullptr) {
      std::fprintf(stderr, "[replay] on_sch crc=%d\n", static_cast<int>(sch.data.tb_crc_ok));
    }
    crc_ok      = sch.data.tb_crc_ok;
    nof_bits    = sch.data.nof_codeblocks_total;
    nof_iters   = static_cast<unsigned>(sch.data.ldpc_decoder_stats.get_nof_observations() != 0
                                            ? sch.data.ldpc_decoder_stats.get_mean()
                                            : 0.0F);
    nof_cbs     = sch.data.nof_codeblocks_total;
    sinr_db     = sch.csi.get_sinr_dB().has_value() ? *sch.csi.get_sinr_dB() : 0.0F;
    epre_db     = sch.csi.get_epre_dB().has_value() ? *sch.csi.get_epre_dB() : 0.0F;
    rsrp_db     = sch.csi.get_rsrp_dB().has_value() ? *sch.csi.get_rsrp_dB() : 0.0F;
    done        = true;
  }

  bool  done      = false;
  bool  crc_ok    = false;
  bool  uci_ok    = false;
  unsigned nof_bits = 0;
  unsigned nof_iters = 0;
  unsigned nof_cbs = 0;
  float sinr_db = 0.0F;
  float epre_db = 0.0F;
  float rsrp_db = 0.0F;
};

} // namespace

int main(int argc, char** argv)
{
  std::string prefix;
  std::string out_prefix;
  bool        dft_mode         = false;
  bool        dft_metal        = false;
  // Write the resource grid from the device (--device-grid): the RX chain's default when
  // the pipeline keeps the grid on the device (see --expert_phy.device_resource_grid on).
  bool device_grid = false;
  /// \brief Number of slots of SYNTHETIC time-domain input (--synth N), 0 = read a recorded capture.
  ///
  /// The D1 hand-over is judged by comparing two runs of the SAME input, so the input's content is
  /// irrelevant - only that both runs get the same one. Random samples are therefore a complete corpus,
  /// and making it here is what frees the harness from needing a recorded capture, a radio and a UE
  /// (which is what the L1 harness exists for). The samples are seeded, so the corpus is reproducible.
  unsigned synth_slots = 0;
  /// First slot number of a synthetic corpus (--synth-first-slot). The slot is HALF OF THE HAND-OVER KEY
  /// (storage, slot), so a harness that got the numbering wrong would ask about a slot nobody deposited
  /// and be told "ready" (the hook FAILS OPEN) - which is why the run asserts on the registry's counters
  /// instead of trusting the wait's return value. Deliberately not 0 by default: 0 is the value an
  /// unset slot has, so a mix-up would be invisible.
  unsigned synth_first_slot = 1;
  /// \brief Serve every slot from ONE grid (--reuse-grid) instead of one grid per slot.
  ///
  /// This is what the receiving chain does: each uplink_processor_impl owns one grid and a cell has
  /// nof_ul_rg of them (20 in the air configuration), so one allocation carries ~20 slots' worth of
  /// receptions. A fresh allocation per slot - the default here, and what a capture replay wants -
  /// gives every slot a distinct storage address and therefore hides the whole reason the hand-over key
  /// needs a slot half at all.
  bool reuse_grid = false;
  /// \brief Number of slots the L1b harness replays through the FRONT END and a real receiver
  ///        (--hop-td N). 0 = the ordinary replay, whose grid comes from the capture.
  ///
  /// The capture then supplies the PDU CONFIGURATION only (its .bin is ignored): the grid is produced by
  /// the front end from synthetic time-domain input, so the hop under test adopts a block the receiving
  /// chain really deposited. That is what puts the fused chain (front end + channel estimation +
  /// equalization + demapping in ONE submission) inside an offline A/B.
  unsigned hop_td_slots = 0;
  /// \brief How many hops read ONE slot's grid (--hop-pdus K, default 1): the multi-PUSCH shape, i.e. one
  ///        slot carrying several UEs' allocations.
  ///
  /// The hand-over's registry is keyed by (storage, slot) and `take_released()` refuses a block that is
  /// already claimed, so of K hops on one grid exactly ONE can adopt the front end's block and the other
  /// K-1 MISS - structurally, not as a race. Two things follow, and both are what this switch exists for:
  /// the cliff is measurable (a slot's CPU submissions go from 1 to K), and the MISS path's device-side
  /// wait becomes LOAD-BEARING, because the consumer the missing hop has to wait for is now a DEVICE
  /// consumer whose commit may still be in flight. With one hop it never is (5.9.39).
  unsigned hop_pdus = 1;
  bool        use_metal_ce     = false;
  bool        use_metal_demod  = false;
  bool        use_metal_decoder = false;
  unsigned    nof_prb           = 25;
  /// \brief How many times the same reception is replayed through one process (a soak).
  ///
  /// The receiver, the grid, the buffer pool and every GPU engine outlive a single reception, so a
  /// defect in the per-hop state (a cached plan, a recycled staging region, a wrap mapping) only
  /// shows up when several receptions run one after another. Default 1 keeps every existing use.
  unsigned    repeat            = 1;
  /// \brief Extra capture prefixes to rotate through in one process (soak across ALLOCATIONS).
  ///
  /// A soak that repeats ONE capture never changes the allocation, and an allocation-keyed cache that
  /// went stale across hops would still hold the right tables - which is how a cross-hop cache defect
  /// stayed invisible until it reached the air. Rotating captures of different shapes in one process
  /// is what makes the per-hop state visible.
  std::vector<std::string> rotate;
  // Default to the strategy the gNB app configures (pusch_channel_estimator_td_strategy), so that a
  // replay compares like with like: the classical estimator divides the least-squares pilots by the
  // number of DM-RS symbols only under "average", while the Metal one always runs its own MMSE.
  bool td_strategy_average = false;
  /// \brief Whether the caller named a back end (--cpu / --metal / --metal-cpu-ldpc / ...).
  ///
  /// The three \c use_metal_* flags default to FALSE, i.e. to the CPU chain. That is the right default
  /// for an unadorned run, but it also means a wrapper that DROPS the flag silently replays on the CPU
  /// while the caller believes it is looking at the device route - and the two agree often enough for
  /// the mistake to survive a byte comparison. That is not hypothetical: the S13-P2c four-arm table
  /// carried an arm named "cpu" that never passed \c --cpu, it was a duplicate of the host-built-A arm,
  /// and the design document's "the CPU reference also fails on every narrow hop" note was that
  /// artifact. The mode is therefore REQUIRED: a run has to say which back ends it means.
  bool mode_given = false;

  for (int i = 1; i != argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--out" && (i + 1 < argc)) {
      out_prefix = argv[++i];
    } else if ((arg == "--nof-prb") && (i + 1 < argc)) {
      nof_prb = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--repeat") && (i + 1 < argc)) {
      repeat = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--also") && (i + 1 < argc)) {
      rotate.emplace_back(argv[++i]);
    } else if (arg == "--dft") {
      dft_mode   = true;
      // --dft names its back end as much as --cpu/--metal do (the generic CPU DFT, line below), so it
      // satisfies the "say which chain you mean" rule on its own.
      mode_given = true;
    } else if (arg == "--dft-metal") {
      dft_mode   = true;
      dft_metal  = true;
      mode_given = true;
    } else if ((arg == "--synth") && (i + 1 < argc)) {
      synth_slots = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--synth-first-slot") && (i + 1 < argc)) {
      synth_first_slot = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--reuse-grid") {
      reuse_grid = true;
    } else if (arg == "--device-grid") {
      device_grid = true;
    } else if ((arg == "--hop-td") && (i + 1 < argc)) {
      hop_td_slots = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--hop-pdus") && (i + 1 < argc)) {
      hop_pdus = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--td-strategy") && (i + 1 < argc)) {
      td_strategy_average = (std::string(argv[++i]) == "average");
    } else if (arg == "--cpu") {
      use_metal_ce = use_metal_demod = use_metal_decoder = false;
      mode_given                     = true;
    } else if (arg == "--metal") {
      use_metal_ce = use_metal_demod = use_metal_decoder = true;
      mode_given                     = true;
    } else if (arg == "--metal-cpu-ldpc") {
      use_metal_ce = use_metal_demod = true;
      use_metal_decoder              = false;
      mode_given                     = true;
    } else if (arg == "--metal-cpu-demod") {
      use_metal_ce  = true;
      use_metal_demod = false;
      use_metal_decoder = false;
      mode_given = true;
    } else if (arg == "-h" || arg == "--help") {
      std::printf("usage: %s <capture-base> --out <prefix> [--cpu|--metal|--metal-cpu-ldpc|--metal-cpu-demod]\n"
                  "  <capture-base> is one reception of a capture, e.g. /tmp/C_4352_17921\n"
                  "  --dft [--dft-metal] [--device-grid] [--synth N] [--synth-first-slot S] [--reuse-grid]\n"
                  "    replays TIME-DOMAIN input through the OFDM demodulator; with --synth N the input is\n"
                  "    generated (N slots) instead of read, so no capture is needed.\n",
                  argv[0]);
      return 0;
    } else if (arg[0] != '-') {
      prefix = arg;
    } else {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      return 1;
    }
  }

  // A synthetic corpus has no capture base: the tool makes the input itself (see --synth).
  if (out_prefix.empty() || (prefix.empty() && (synth_slots == 0))) {
    std::fprintf(stderr,
                 "usage: %s <capture-base> --out <prefix> [--cpu|--metal]\n"
                 "       %s --out <prefix> --dft [--dft-metal] --synth <slots>   (no capture needed)\n",
                 argv[0],
                 argv[0]);
    return 1;
  }
  if ((synth_slots != 0) && !dft_mode) {
    std::fprintf(stderr, "%s: --synth feeds the time-domain replay only; add --dft or --dft-metal.\n", argv[0]);
    return 1;
  }

  // A run that does not say which back ends it means would silently be the CPU chain (see mode_given):
  // refuse it, because the whole point of a replay is to know WHICH chain produced the numbers.
  if (!mode_given) {
    std::fprintf(stderr,
                 "%s: no back end selected. Pass --cpu, --metal, --metal-cpu-ldpc or --metal-cpu-demod.\n"
                 "        (The back ends default to the CPU chain, so an omitted flag makes a device-route\n"
                 "         replay run on the host and look plausible.)\n",
                 argv[0]);
    return 1;
  }

  // The staged capture of this replay: the tool sets the environment before the first PUSCH
  // processing (the capture reads it once).
  setenv("OCUDU_UL_DUMP", out_prefix.c_str(), 1);
  // The soft-bit capture can be disabled from the environment (OCUDU_UL_DUMP_LLR=0), which is how
  // a crash inside it can be told apart from one in the receiver.
  if (std::getenv("OCUDU_UL_DUMP_LLR") == nullptr) {
    setenv("OCUDU_UL_DUMP_LLR", "1", 1);
  }
  // The capture budget: one reception is what a single-reception replay wants (its stages are the point),
  // but the L1b harness replays a slot sequence and compares the LLRs of every slot, so the budget has to
  // cover it. It counts RECEPTIONS (hops), not slots - with --hop-pdus K there are K per slot, and a budget
  // of one-per-slot silently captured only the first slots of the corpus.
  setenv("OCUDU_UL_DUMP_COUNT",
         std::to_string((hop_td_slots != 0) ? hop_td_slots * hop_pdus : 1U).c_str(),
         1);

  // --------------------------------------------------------------------------------------------
  // Time-domain replay: rebuild the resource grid of each recorded slot with the selected DFT
  // back end and pipeline depth (OCUDU_DFT_PIPELINE_DEPTH), which is the only way to compare the
  // pipelined front end with the serial one on the same samples.
  // --------------------------------------------------------------------------------------------
  if (dft_mode) {
    struct entry_t {
      unsigned slot;
      unsigned symbol;
      unsigned port;
      size_t   size;
    };
    std::vector<entry_t> entries;
    // All the samples of the corpus, in the order the entries name them.
    std::vector<ci16_t> samples;
    // Transform size. A recorded capture does not carry it (its lines give the SYMBOL size), so it is
    // inferred below from the numerology's cyclic prefix; a synthetic corpus picks it here, which is why
    // it is a variable and not a constant.
    unsigned inferred_dft_size = 0;

    if (synth_slots != 0) {
      // ---- Synthetic corpus (--synth) --------------------------------------------------------------
      // The hand-over is judged by running the SAME input twice and comparing the grids, so what the
      // samples ARE is irrelevant - only that both arms get the same ones. Random samples are therefore
      // a complete corpus for this harness, and generating them here is what removes the harness's
      // dependency on a recorded capture, a radio and a UE.
      //
      // The transform size is the next power of two at or above the occupied bandwidth: the air
      // configuration is 25 PRB / 5 MHz / 15 kHz / 7.68 Msps, i.e. 300 subcarriers in a 512-point
      // transform, and 512 = 2^9 is the same choice. Everything else follows from it, so the synthetic
      // corpus has exactly the shape a capture of that configuration would have.
      inferred_dft_size = 1;
      while (inferred_dft_size < (nof_prb * NOF_SUBCARRIERS_PER_RB)) {
        inferred_dft_size *= 2;
      }
      const unsigned sampling_rate_Hz = to_sampling_rate_Hz(subcarrier_spacing::kHz15, inferred_dft_size);
      const cyclic_prefix cp          = cyclic_prefix::NORMAL;

      std::mt19937                       rng(20260921);
      std::uniform_int_distribution<int> dist(-2000, 2000);
      for (unsigned i_slot = 0; i_slot != synth_slots; ++i_slot) {
        for (unsigned symbol = 0; symbol != MAX_NSYMB_PER_SLOT; ++symbol) {
          const unsigned cp_len = cp.get_length(symbol, subcarrier_spacing::kHz15).to_samples(sampling_rate_Hz);
          entry_t        entry  = {.slot   = synth_first_slot + i_slot,
                                   .symbol = symbol,
                                   .port   = 0,
                                   .size   = cp_len + inferred_dft_size};
          entries.push_back(entry);
          for (size_t i = 0; i != entry.size; ++i) {
            samples.emplace_back(static_cast<int16_t>(dist(rng)), static_cast<int16_t>(dist(rng)));
          }
        }
      }
      std::printf("synth corpus: %u slots from slot %u (%u transforms, %zu samples)\n",
                  synth_slots,
                  synth_first_slot,
                  static_cast<unsigned>(entries.size()),
                  samples.size());
      if (reuse_grid) {
        std::printf("synth corpus: ONE grid serves every slot (--reuse-grid), as the receiving chain does\n");
      }
    } else {
      // ---- A recorded time-domain capture (OCUDU_UL_DUMP_TD) ---------------------------------------
      std::ifstream list(prefix + "_td.txt");
      if (!list.is_open()) {
        std::fprintf(stderr, "cannot read %s_td.txt\n", prefix.c_str());
        return 1;
      }
      std::ifstream bin(prefix + "_td.bin", std::ios::binary);
      if (!bin.is_open()) {
        std::fprintf(stderr, "cannot read %s_td.bin\n", prefix.c_str());
        return 1;
      }

      std::set<std::tuple<unsigned, unsigned, unsigned>> seen_transforms;
      {
        std::string line;
        while (std::getline(list, line)) {
          entry_t      entry = {};
          std::string  key;
          std::stringstream stream(line);
          while (std::getline(stream, key, ' ')) {
            auto pos = key.find('=');
            if (pos == std::string::npos) {
              continue;
            }
            const std::string name  = key.substr(0, pos);
            const unsigned    value = static_cast<unsigned>(std::strtoul(key.substr(pos + 1).c_str(), nullptr, 10));
            if (name == "slot") entry.slot = value;
            if (name == "symbol") entry.symbol = value;
            if (name == "port") entry.port = value;
            if (name == "size") entry.size = value;
          }
          // One entry per (slot, symbol, port), the FIRST one recorded: the capture appends the
          // symbols of a slot every time its slot number comes round again (the counter wraps), so a
          // long recording of four slots holds each of them several times over, with different
          // samples. Replaying those as if they were one slot wrote every grid symbol twice and made
          // the per-slot dump meaningless - the two rounds are different transmissions and only the
          // first belongs to the slot this capture is about.
          const auto key_slot = std::make_tuple(entry.slot, entry.symbol, entry.port);
          if (!seen_transforms.insert(key_slot).second) {
            continue;
          }
          entries.push_back(entry);
        }
      }
      if (entries.empty()) {
        std::fprintf(stderr, "%s_td.txt holds no transform\n", prefix.c_str());
        return 1;
      }

      // All the samples of the capture, read once.
      bin.seekg(0, std::ios::end);
      const size_t bytes = static_cast<size_t>(bin.tellg());
      bin.seekg(0);
      samples.resize(bytes / sizeof(ci16_t));
      bin.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(bytes));
    }

    // One DFT back end, one demodulator: the depth comes from OCUDU_DFT_PIPELINE_DEPTH.
    ofdm_factory_generic_configuration ofdm_config = {
        // The default CPU DFT factory is the one the lower PHY uses when the Metal one is not
        // selected; the generic variant refuses the cell configurations the radio produces.
        .dft_factory = dft_metal ? create_dft_processor_factory_metal() : create_dft_processor_factory()};
    std::shared_ptr<ofdm_demodulator_factory> ofdm_factory = create_ofdm_demodulator_factory_generic(ofdm_config);
    if (ofdm_factory == nullptr) {
      std::fprintf(stderr, "cannot create the OFDM demodulator factory\n");
      return 1;
    }
    // The capture records one line per transform with the SYMBOL size (cyclic prefix + transform),
    // not the transform size. Taking that as dft_size made the replay transform 552 points instead
    // of 512 - a size outside the mixed-radix family, and not what the gNB computes - which is why
    // its grid could not be compared with anything. The transform size is derived here instead, from
    // the standard cyclic-prefix lengths of the numerology, and stays consistent across symbols.
    const subcarrier_spacing     dft_scs = subcarrier_spacing::kHz15;
    constexpr unsigned           dft_sampling_rate_Hz = 7680000;
    const unsigned               first_slot           = entries.front().slot;
    const cyclic_prefix          dft_cp               = cyclic_prefix::NORMAL;
    if (synth_slots == 0) {
      // A recorded capture carries only the SYMBOL size, so the transform size is inferred from it: the
      // numerology's cyclic prefix for that symbol is standard, and what is left over is the transform.
      const unsigned first_cp_len =
          dft_cp.get_length(entries.front().symbol, dft_scs).to_samples(dft_sampling_rate_Hz);
      if (entries.front().size <= first_cp_len) {
        std::fprintf(stderr,
                     "%s_td.txt: symbol size %zu does not cover its cyclic prefix %u\n",
                     prefix.c_str(),
                     entries.front().size,
                     first_cp_len);
        return 1;
      }
      inferred_dft_size = entries.front().size - first_cp_len;
    }

    ofdm_demodulator_configuration demod_config = {};
    demod_config.numerology                = 0;
    demod_config.bw_rb                     = nof_prb;
    demod_config.dft_size                  = inferred_dft_size;
    demod_config.cp                        = dft_cp;
    demod_config.nof_samples_window_offset = 0;
    demod_config.scale                     = 1.0F;
    demod_config.center_freq_Hz            = 0.0;
    demod_config.device_grid_write         = device_grid;
    // The declaration the hand-over needs: the consumers of this grid read it on the DEVICE (that is why
    // --device-grid exists), and the host reader of this harness WAITS for the production - which is
    // exactly what write_grid() below does. Without it the release path is not even reached
    // (`wait_per_slot` is false) and the run would arm the knob and exercise nothing.
    demod_config.grid_consumed_on_device   = true;
    std::shared_ptr<resource_grid_factory> dft_grid_factory = create_resource_grid_factory();
    if (dft_grid_factory == nullptr) {
      std::fprintf(stderr, "cannot create the resource grid factory\n");
      return 1;
    }
    std::unique_ptr<ofdm_symbol_demodulator> demodulator = ofdm_factory->create_ofdm_symbol_demodulator(demod_config);
    if (demodulator == nullptr) {
      std::fprintf(stderr, "cannot create the OFDM symbol demodulator\n");
      return 1;
    }
    (void) first_slot;
    const unsigned depth = demodulator->get_pipeline_depth();

    std::printf("dft replay %s -> %s (%s DFT, pipeline depth %u, %u PRB, %zu transforms, grid write %s)\n",
                synth_slots != 0 ? "<synth>" : prefix.c_str(),
                out_prefix.c_str(),
                dft_metal ? "metal" : "cpu",
                depth,
                nof_prb,
                entries.size(),
                device_grid ? "device" : "host");
    // Whether this run was ASKED to exercise the hand-over. Read from the same variable the engine reads,
    // so the report below cannot disagree with what the pipeline did.
    // grid_handover_armed() is the knob's ONE definition (5.9.49 moved its default to ON), so the report
    // cannot disagree with what the pipeline did. The CONTROL arm is now `OCUDU_DFT_RELEASE_BLOCK=0`, not
    // "unset" - see the arm scripts.
    const bool release_armed = ocudu::grid_handover_armed();
    /// The falsification arm of this harness (design document 5.9.37): with this set the harness does NOT
    /// ask for the grid's production before reading it, i.e. it commits the very mistake the wait exists to
    /// prevent. It is a switch and not a source edit on purpose - an arm that requires patching the tool is
    /// an arm nobody re-runs, and this one has to keep proving that the comparison below CAN fail.
    const bool drop_consumer_wait = []() {
      const char* env = std::getenv("OCUDU_L1_DROP_CONSUMER_WAIT");
      return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
    }();
    /// \brief Deliberately ask about a DIFFERENT slot than the one deposited (--consumer slot skew).
    ///
    /// The slot is half of the hand-over key, and the two halves are written by two different pieces of
    /// code: the producer takes it from the receiving chain's slot (set_lane_slot), the consumer from
    /// whatever numbering its own input uses. If those two bases ever disagree, the consumer is told
    /// "nothing is pending" and reads the grid anyway - the hook FAILS OPEN. This arm reproduces that, and
    /// it is the reason the run asserts on the registry's counters (handed / not_found / unproduced) rather
    /// than on the wait's return value: those counters are what turn a silent fail-open into a red run.
    const unsigned consumer_slot_skew = []() {
      const char* env = std::getenv("OCUDU_L1_CONSUMER_SLOT_SKEW");
      return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 0U;
    }();

    unsigned                cursor = 0;
    std::vector<unsigned>   ring_slots;
    std::shared_ptr<resource_grid> grid;
    unsigned                current_slot = std::numeric_limits<unsigned>::max();
    unsigned                ring         = 0;
    unsigned                drained      = 0;

    /// Dumps one slot's grid - and is, for the hand-over, the grid's CONSUMER.
    ///
    /// With OCUDU_DFT_RELEASE_BLOCK=1 the slot's transforms are handed over UNCOMMITTED: nothing has run
    /// when the receiving slot ends, and the block is produced at whoever claims it. This tool is that
    /// whoever - it reads the grid on the host, exactly as the PUCCH does in the receiving chain, and the
    /// registration it performs here is the same one (grid_ready_hook::wait): it claims an unclaimed block
    /// and commits it (the fallback a hand-over owes), or waits for the generation of the block that was
    /// claimed. Reading without this call reads memory the GPU has not written yet - which is not a
    /// hypothesis but the arm OCUDU_L1_DROP_CONSUMER_WAIT=1 reproduces.
    ///
    /// The SLOT is half of the hand-over key (storage, slot), and it must be the very number the producer
    /// was told (set_lane_slot, in the loop below). A harness that got this wrong would ask about a slot
    /// nobody deposited and be told "ready" - the hook FAILS OPEN - so the return value alone proves
    /// nothing: report_handover() below asserts on the registry's own counters instead.
    auto write_grid = [&](unsigned slot) -> bool {
      const resource_grid_device_view view = grid->get_writer().get_device_view();
      if (view.is_valid() && !drop_consumer_wait) {
        if (!grid_ready_hook::wait(view.base, slot + consumer_slot_skew)) {
          std::fprintf(stderr,
                       "slot %u: the grid was not produced in time - refusing to dump what nobody wrote\n",
                       slot);
          return false;
        }
      }
      const std::string base = out_prefix + "_" + std::to_string(slot) + "_dft";
      if (FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
        std::fprintf(f,
                     "slot=%u\nscs_khz=15\ncp=normal\nrnti=0\nbwp_size_rb=%u\nbwp_start_rb=0\n"
                     "rx_ports=0\nnof_dft=%u\ndepth=%u\n",
                     slot,
                     nof_prb,
                     demod_config.dft_size,
                     depth);
        std::fclose(f);
      }
      if (FILE* f = std::fopen((base + ".bin").c_str(), "wb")) {
        std::vector<cf_t> symbol(nof_prb * NOF_SUBCARRIERS_PER_RB);
        for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
          grid->get_reader().get(symbol, 0, i_symbol, 0);
          std::fwrite(symbol.data(), sizeof(cf_t), symbol.size(), f);
        }
        std::fclose(f);
      }
      return true;
    };

    // One grid per slot, or ONE grid for all of them (--reuse-grid): the receiving chain does the latter,
    // and it is what makes the slot half of the hand-over key necessary - the storage address comes back
    // with the next reception, so the address alone cannot say which slot a block belongs to.
    if (reuse_grid) {
      grid = dft_grid_factory->create(1, MAX_NSYMB_PER_SLOT, nof_prb * NOF_SUBCARRIERS_PER_RB);
      if (grid == nullptr) {
        std::fprintf(stderr, "cannot create the resource grid\n");
        return 1;
      }
    }
    bool dumps_ok = true;

    for (const entry_t& entry : entries) {
      if (entry.slot != current_slot) {
        // Close the previous slot: finish every transform still in flight. Guarded on a slot having been
        // STARTED and not merely on the grid existing: with --reuse-grid the grid is created before this
        // loop, and the first entry would otherwise "close" a slot that was never opened and dump it (its
        // slot number is still the sentinel).
        if ((grid != nullptr) && (current_slot != std::numeric_limits<unsigned>::max())) {
          while (drained != ring_slots.size()) {
            demodulator->finish_symbol(grid->get_writer(), ring_slots[drained++]);
          }
          dumps_ok = write_grid(current_slot) && dumps_ok;
        }
        current_slot = entry.slot;
        ring_slots.clear();
        drained = 0;
        ring    = 0;
        if (!reuse_grid) {
          grid = dft_grid_factory->create(1, MAX_NSYMB_PER_SLOT, nof_prb * NOF_SUBCARRIERS_PER_RB);
        }
        // Tell the device backend which slot the transforms it is about to receive belong to. This is the
        // producer's half of the hand-over key, and the receiving chain sets it exactly here - when the
        // slot changes (puxch_processor_impl::process_symbol).
        demodulator->set_lane_slot(entry.slot);
      }
      const span<const ci16_t> input(samples.data() + cursor, entry.size);
      cursor += entry.size;
      if (depth > 1) {
        if (ring_slots.size() == depth) {
          demodulator->finish_symbol(grid->get_writer(), ring_slots[drained++]);
        }
        const unsigned ring_slot = ring++ % depth;
        demodulator->submit_symbol(grid->get_writer(), input, entry.port, entry.symbol, ring_slot);
        ring_slots.push_back(ring_slot);
      } else {
        demodulator->demodulate(grid->get_writer(), input, entry.port, entry.symbol);
      }
    }
    if ((grid != nullptr) && (current_slot != std::numeric_limits<unsigned>::max())) {
      while (drained != ring_slots.size()) {
        demodulator->finish_symbol(grid->get_writer(), ring_slots[drained++]);
      }
      dumps_ok = write_grid(current_slot) && dumps_ok;
    }
    // ----------------------------------------------------------------------------------------------
    // The assertion that keeps this harness honest (design document 5.9.37).
    //
    // Every comparison this harness makes is "the same input through the same pipeline twice" - and that
    // comparison PASSES when the mechanism under test did nothing at all. It already happened once in this
    // project: ofdm_demodulator_metal_batch_test's armed section ran with the knob set and the hand-over
    // refused (grid_has_host_consumers() was a constant true), so it compared two host-written grids and
    // reported success (5.9.19, withdrawn). A harness is therefore only allowed to report success when the
    // hand-over's OWN counters say it happened, and this is where that is decided. They are read through
    // grid_ready_hook - the accessor that exists for exactly this - and not by parsing the exit-time
    // `[metal_stats] dft handover` line, which prints after this function has already returned.
    // ----------------------------------------------------------------------------------------------
    grid_handover_counts hs;
    grid_ready_hook::counts(hs);
    // Every slot of a synthetic corpus deposits exactly once: one block per receiving slot, at its last
    // symbol. A recorded capture is not required to cover whole slots, so it is not counted here.
    const uint64_t expected_deposits = (synth_slots != 0) ? synth_slots : 0;
    std::printf("[l1_handover] installed=%d armed=%d drop_consumer_wait=%d slot_skew=%u slots=%llu handed=%llu "
                "taken=%llu superseded=%llu evicted=%llu evicted_unproduced=%llu fallback=%llu late=%llu not_found=%llu unproduced=%llu "
                "ready_timeouts=%llu\n",
                hs.installed ? 1 : 0,
                release_armed ? 1 : 0,
                drop_consumer_wait ? 1 : 0,
                consumer_slot_skew,
                static_cast<unsigned long long>(expected_deposits),
                static_cast<unsigned long long>(hs.handed),
                static_cast<unsigned long long>(hs.taken),
                static_cast<unsigned long long>(hs.superseded),
                static_cast<unsigned long long>(hs.evicted),
                static_cast<unsigned long long>(hs.evicted_unproduced),
                static_cast<unsigned long long>(hs.fallback_commits),
                static_cast<unsigned long long>(hs.late_commits),
                static_cast<unsigned long long>(hs.not_found),
                static_cast<unsigned long long>(hs.unproduced),
                static_cast<unsigned long long>(hs.ready_timeouts));

    bool ok = dumps_ok;
    if (release_armed) {
      if (!hs.installed) {
        std::fprintf(stderr,
                     "FAIL: OCUDU_DFT_RELEASE_BLOCK is set but this run has no hand-over at all (no Metal "
                     "DFT engine) - the knob did nothing\n");
        ok = false;
      } else if (hs.handed == 0) {
        std::fprintf(stderr,
                     "FAIL: OCUDU_DFT_RELEASE_BLOCK is set but NOTHING was handed over - this run judged "
                     "nothing (check the startup warning 'will NOT exercise D1')\n");
        ok = false;
      } else if ((expected_deposits != 0) && (hs.handed != expected_deposits)) {
        std::fprintf(stderr,
                     "FAIL: %llu slot(s) of corpus but %llu deposit(s) - the corpus and the hand-over "
                     "disagree about how many blocks there are\n",
                     static_cast<unsigned long long>(expected_deposits),
                     static_cast<unsigned long long>(hs.handed));
        ok = false;
      }
      if (drop_consumer_wait) {
        // This arm exists to fail the byte comparison: the harness read grids it never asked for. The
        // deposits are still counted - they were made - they simply have no producer of their grid.
        std::printf("[l1_handover] falsification arm: the consumer wait was dropped on purpose\n");
      } else {
        // A wait that finds NO record cannot wait: it is told "nothing pending" and reads the grid anyway
        // (the hook fails open). Every wait here is prompt and its (storage, slot) was deposited a moment
        // ago, so a single not-found means the two ends do NOT name the same key - a slot-numbering
        // mismatch, which is the one way this harness could silently stop testing anything.
        if (hs.not_found != 0) {
          std::fprintf(stderr,
                       "FAIL: %llu consumer wait(s) found no deposit for their (storage, slot) - the producer "
                       "and the consumer disagree about the key%s\n",
                       static_cast<unsigned long long>(hs.not_found),
                       (consumer_slot_skew != 0) ? " (this arm skews the consumer's slot on purpose)" : "");
          ok = false;
        }
        if (hs.unproduced != 0) {
          std::fprintf(stderr, "FAIL: %llu handed-over block(s) were never produced\n",
                       static_cast<unsigned long long>(hs.unproduced));
          ok = false;
        }
        if (hs.ready_timeouts != 0) {
          std::fprintf(stderr, "FAIL: %llu host wait(s) timed out\n",
                       static_cast<unsigned long long>(hs.ready_timeouts));
          ok = false;
        }
        // In a front-end-only harness there is no PUSCH hop, so nothing TAKES a deposit: every one of them
        // has to be committed by this tool (the fallback), which is the same path the PUCCH takes in a
        // PUCCH-only slot. taken==0 here is therefore the correct answer and not a failure - what would be
        // a failure is a deposit nobody produced, which the two checks above already catch.
        if ((hs.fallback_commits + hs.late_commits) < hs.handed) {
          std::fprintf(stderr,
                       "FAIL: %llu deposit(s) were never committed by anyone (fallback=%llu late=%llu)\n",
                       static_cast<unsigned long long>(hs.handed - hs.fallback_commits - hs.late_commits),
                       static_cast<unsigned long long>(hs.fallback_commits),
                       static_cast<unsigned long long>(hs.late_commits));
          ok = false;
        }
      }
    } else if (hs.handed != 0) {
      std::fprintf(stderr,
                   "FAIL: the hand-over happened (%llu deposit(s)) while this arm asked for the CONTROL "
                   "(OCUDU_DFT_RELEASE_BLOCK=0) - this arm is not the reference it claims to be\n",
                   static_cast<unsigned long long>(hs.handed));
      ok = false;
    }

    std::printf("dft replay done: grid dumps written as %s_<slot>_dft{.txt,.bin}\n", out_prefix.c_str());
    return ok ? 0 : 1;
  }

  // Shared infrastructure. Every factory is checked: a null one means the tool was built without
  // the matching back end and the failure has to be visible instead of a crash.
  auto check = [](auto& factory, const char* name) {
    if (factory == nullptr) {
      std::fprintf(stderr, "cannot create %s\n", name);
      std::exit(1);
    }
  };
  auto report = [](const char* step) {
    if (std::getenv("OCUDU_REPLAY_TRACE") != nullptr) {
      std::fprintf(stderr, "[replay] %s\n", step);
    }
  };

  std::unique_ptr<task_worker_pool<concurrent_queue_policy::locking_mpmc>> worker_pool =
      std::make_unique<task_worker_pool<concurrent_queue_policy::locking_mpmc>>("replay", 2, 1024);
  check(worker_pool, "worker_pool");
  report("created worker_pool");
  std::unique_ptr<task_worker_pool_executor<concurrent_queue_policy::locking_mpmc>> executor =
      std::make_unique<task_worker_pool_executor<concurrent_queue_policy::locking_mpmc>>(*worker_pool);
  std::shared_ptr<resource_grid_factory> grid_factory = create_resource_grid_factory();
  check(grid_factory, "grid_factory");
  report("created grid_factory");

  std::shared_ptr<crc_calculator_factory> crc_factory = create_crc_calculator_factory_sw("auto");
  check(crc_factory, "crc_factory");
  report("created crc_factory");
  ldpc_decoder_factory::ldpc_decoder_factory_configuration ldpc_cfg = {.force_decoding      = false,
                                                                       .early_stop_syndrome = true,
                                                                       .ldpc_decoder_offset = -1.0F};
  std::shared_ptr<ldpc_decoder_factory> ldpc_factory = create_ldpc_decoder_factory_sw(
      use_metal_decoder ? "metal" : "auto", ldpc_cfg);
  check(ldpc_factory, "ldpc_factory");
  report("created ldpc_factory");
  std::shared_ptr<ldpc_rate_dematcher_factory> dematcher_factory = create_ldpc_rate_dematcher_factory_sw("auto");
  check(dematcher_factory, "dematcher_factory");
  report("created dematcher_factory");
  std::shared_ptr<ldpc_segmenter_rx_factory>   segmenter_factory = create_ldpc_segmenter_rx_factory_sw();
  check(segmenter_factory, "segmenter_factory");
  report("created segmenter_factory");

  pusch_decoder_factory_sw_configuration dec_cfg;
  dec_cfg.crc_factory               = crc_factory;
  dec_cfg.decoder_factory           = ldpc_factory;
  dec_cfg.dematcher_factory         = dematcher_factory;
  dec_cfg.segmenter_factory         = segmenter_factory;
  dec_cfg.nof_pusch_decoder_threads = 2;
  dec_cfg.executor                  = executor.get();
  dec_cfg.nof_prb                   = MAX_NOF_PRBS;
  dec_cfg.nof_layers                = pusch_constants::MAX_NOF_LAYERS;
  std::shared_ptr<pusch_decoder_factory> decoder_factory = create_pusch_decoder_factory_sw(dec_cfg);
  check(decoder_factory, "decoder_factory");
  report("created decoder_factory");

  std::shared_ptr<pseudo_random_generator_factory> prg_factory = create_pseudo_random_generator_sw_factory();
  check(prg_factory, "prg_factory");
  report("created prg_factory");
  std::shared_ptr<low_papr_sequence_generator_factory> papr_factory =
      create_low_papr_sequence_generator_sw_factory();
  check(papr_factory, "papr_factory");
  report("created papr_factory");
  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_generic();
  check(dft_factory, "dft_factory");
  report("created dft_factory");
  std::shared_ptr<time_alignment_estimator_factory> ta_factory =
      create_time_alignment_estimator_dft_factory(dft_factory);
  check(ta_factory, "ta_factory");
  report("created ta_factory");

  // Channel estimator: the Metal MMSE or the classical one.
  std::shared_ptr<port_channel_estimator_factory> port_estimator_factory =
      use_metal_ce ? create_port_channel_estimator_factory_sw(ta_factory, port_channel_estimator_algorithm::metal_mmse,
                                                              370e-9F, 0.0F)
                   : create_port_channel_estimator_factory_sw(ta_factory);
  check(port_estimator_factory, "port_estimator_factory");
  report("created port_estimator_factory");
  std::shared_ptr<dmrs_pusch_estimator_factory> estimator_factory = create_dmrs_pusch_estimator_factory_sw(
      prg_factory,
      papr_factory,
      port_estimator_factory,
      *executor,
      pusch_constants::MAX_NOF_RX_PORTS,
      port_channel_estimator_fd_smoothing_strategy::filter,
      td_strategy_average ? port_channel_estimator_td_interpolation_strategy::average
                          : port_channel_estimator_td_interpolation_strategy::interpolate,
      true);
  check(estimator_factory, "estimator_factory");
  report("created estimator_factory");

  // Equalizer and demapper: Metal or generic.
  std::shared_ptr<channel_equalizer_factory> equalizer_factory =
      use_metal_demod ? create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::mmse)
                      : create_channel_equalizer_generic_factory(channel_equalizer_algorithm_type::mmse);
  check(equalizer_factory, "equalizer_factory");
  report("created equalizer_factory");
  std::shared_ptr<demodulation_mapper_factory> demapper_factory =
      use_metal_demod ? create_demodulation_mapper_metal_factory() : create_demodulation_mapper_factory();
  check(demapper_factory, "demapper_factory");
  report("created demapper_factory");

  std::shared_ptr<pusch_demodulator_factory> demodulator_factory =
      create_pusch_demodulator_factory_sw(equalizer_factory,
                                          create_dft_transform_precoder_factory(create_dft_processor_factory_generic(),
                                                                                MAX_NOF_PRBS),
                                          demapper_factory,
                                          create_evm_calculator_factory(),
                                          prg_factory,
                                          MAX_NOF_PRBS,
                                          true);
  check(demodulator_factory, "demodulator_factory");
  report("created demodulator_factory");

  std::shared_ptr<uci_decoder_factory> uci_factory =
      create_uci_decoder_factory_generic(create_short_block_detector_factory_sw(),
                                         create_polar_factory_sw(),
                                         crc_factory);
  check(uci_factory, "uci_factory");
  report("created uci_factory");

  pusch_processor_factory_sw_configuration proc_cfg;
  proc_cfg.estimator_factory  = estimator_factory;
  proc_cfg.demodulator_factory = demodulator_factory;
  proc_cfg.demux_factory      = create_ulsch_demultiplex_factory_sw();
  proc_cfg.decoder_factory    = decoder_factory;
  proc_cfg.uci_dec_factory    = uci_factory;
  proc_cfg.ch_estimate_dimensions = {.nof_prb       = MAX_NOF_PRBS,
                                     .nof_symbols   = MAX_NSYMB_PER_SLOT,
                                     .nof_rx_ports  = pusch_constants::MAX_NOF_RX_PORTS,
                                     .nof_tx_layers = pusch_constants::MAX_NOF_LAYERS};
  proc_cfg.dec_nof_iterations    = 10;
  proc_cfg.dec_enable_early_stop = true;
  proc_cfg.max_nof_concurrent_threads = 2;
  proc_cfg.csi_sinr_calc_method = channel_state_information::sinr_type::post_equalization;
  std::shared_ptr<pusch_processor_factory> proc_factory = create_pusch_processor_factory_sw(proc_cfg);
  check(proc_factory, "proc_factory");
  report("created proc_factory");

  rx_buffer_pool_config buffer_pool_config;
  buffer_pool_config.max_codeblock_size   = ldpc::MAX_CODEBLOCK_SIZE;
  buffer_pool_config.nof_buffers          = 4;
  buffer_pool_config.nof_codeblocks       = MAX_NOF_CODEBLOCKS;
  buffer_pool_config.expire_timeout_slots = 16;
  buffer_pool_config.external_soft_bits   = false;
  std::unique_ptr<rx_buffer_pool_controller> buffer_pool = create_rx_buffer_pool(buffer_pool_config);
  check(buffer_pool, "buffer_pool");
  report("created buffer_pool");
  if (!buffer_pool) {
    std::fprintf(stderr, "cannot create the receive buffer pool\n");
    return 1;
  }

  std::printf("replay %s -> %s  (%s CE, %s equalizer/demapper, %s LDPC)\n",
              prefix.c_str(),
              out_prefix.c_str(),
              use_metal_ce ? "metal" : "cpu",
              use_metal_demod ? "metal" : "cpu",
              use_metal_decoder ? "metal" : "cpu");

  unsigned                 replayed = 0;
  std::vector<std::string> prefixes;
  prefixes.push_back(prefix);
  for (const std::string& extra : rotate) {
    prefixes.push_back(extra);
  }
  // ------------------------------------------------------------------------------------------------
  // L1b: the front end that feeds the hop (see --hop-td). Created before the loop because it outlives
  // one reception: it is the receiving chain's front end, and its device state is per slot.
  // ------------------------------------------------------------------------------------------------
  hop_front_end front_end;
  if (hop_td_slots != 0) {
    if (!front_end.init(use_metal_demod, device_grid, nof_prb, subcarrier_spacing::kHz15)) {
      std::fprintf(stderr, "cannot create the front end for --hop-td\n");
      return 1;
    }
    std::printf("hop-td: %u slot(s) from slot %u through the front end (%s DFT, grid write %s), PDU from %s\n",
                hop_td_slots,
                synth_first_slot,
                use_metal_demod ? "metal" : "cpu",
                device_grid ? "device" : "host",
                prefix.c_str());
  }
  /// Whether the HOST reads the grid BEFORE the hop does (OCUDU_L1_HOST_FIRST=1, design document 5.9.38).
  /// It decides which half of the hand-over the hop exercises: normally the hop is the first consumer and
  /// ADOPTS the block (taken>0), while with this set the host consumer claims and commits it first and the
  /// hop MISSES - taking the device-side wait path instead (not_found/fallback, see the MMSE engine's
  /// pending_grid_wait). Both orders have to produce the same LLRs.
  const bool host_first = []() {
    const char* env = std::getenv("OCUDU_L1_HOST_FIRST");
    return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
  }();
  /// \brief The host consumer CLAIMS the grid but does NOT wait for its production
  ///        (OCUDU_L1_CLAIM_ONLY=1, design document 5.9.39).
  ///
  /// This is the one construction in which the MISS path's device-side wait is LOAD-BEARING, and the reason
  /// a host_first arm built on grid_ready_hook::wait() cannot falsify it: wait() returns only after the
  /// production has completed, so by the time the hop starts there is no ordering left for anybody to
  /// provide and dropping the device-side wait changes nothing. Here the host claims the block (committing
  /// it) and returns at once, so the commit is STILL IN FLIGHT when the hop runs: the hop MISSES, reads the
  /// grid from its own command buffer, and only the device-side wait orders that read after the commit.
  /// It is the same shape as a DEVICE consumer that claimed the block - which is what happens for real when
  /// two hops read one slot's grid - reached without having to build the second hop.
  const bool claim_only = []() {
    const char* env = std::getenv("OCUDU_L1_CLAIM_ONLY");
    return (env != nullptr) && (std::strtoul(env, nullptr, 10) != 0);
  }();

  for (const std::string& capture_prefix : prefixes) {
    // In hop mode the round index IS the slot offset: one round per slot the front end produces.
    for (unsigned rep = 0; rep != ((hop_td_slots != 0) ? hop_td_slots : repeat); ++rep) {
    unsigned  i       = 0;
    capture_t capture;
    report("parsing");
    if (!parse_capture(capture_prefix, capture)) {
      std::fprintf(stderr, "cannot read %s.txt\n", capture_prefix.c_str());
      return 1;
    }
    const unsigned    nof_ports = capture.get_list("rx_ports").size();
    const unsigned    bwp_size  = capture.get_unsigned("bwp_size_rb");
    const unsigned    bwp_start = capture.get_unsigned("bwp_start_rb");
    const unsigned    nof_subc  = bwp_size * NOF_SUBCARRIERS_PER_RB;
    const std::string bin_path  = capture_prefix + ".bin";

    if (hop_td_slots != 0) {
      if ((bwp_start + bwp_size) > nof_prb) {
        std::fprintf(stderr,
                     "--hop-td: the PDU needs PRB %u..%u but the front end produces %u PRB "
                     "(raise --nof-prb)\n",
                     bwp_start,
                     bwp_start + bwp_size,
                     nof_prb);
        return 1;
      }
    }

    report("rebuilding the grid");
    std::shared_ptr<resource_grid> grid;
    /// The RECEIVING SLOT this grid belongs to: half of the hand-over key, and in hop mode the number the
    /// front end was told (set_lane_slot) and the PDU is stamped with. All three have to be the same
    /// number or the hop looks up a block nobody deposited (the fail-open trap of memo 4.2).
    unsigned receiving_slot = 0;
    if (hop_td_slots != 0) {
      // The grid comes from the FRONT END, not from the capture: the capture supplies the PDU only.
      receiving_slot = synth_first_slot + rep;
      grid           = front_end.produce(receiving_slot);
      if (grid == nullptr) {
        std::fprintf(stderr, "cannot produce the grid of slot %u\n", receiving_slot);
        return 1;
      }
      if (claim_only) {
        // Claim + commit, but do NOT wait: the production is still in flight when the hop below runs, so the
        // hop's own device-side wait is the only thing ordering its read against that commit. The generation
        // is dropped on purpose - this arm reads nothing from the grid, it only takes the block away.
        (void)grid_ready_hook::claim(hop_front_end::storage(grid), receiving_slot);
      } else if (host_first) {
        // The host consumer goes FIRST and WAITS, so the hop finds the block already claimed and has to take
        // the MISS path. Its wait is the one the PUCCH makes in the receiving chain. (Note: this arm cannot
        // falsify the device-side wait - see claim_only above.)
        if (!grid_ready_hook::wait(hop_front_end::storage(grid), receiving_slot)) {
          std::fprintf(stderr, "slot %u: the grid was not produced in time\n", receiving_slot);
          return 1;
        }
      }
    } else {
      // Rebuild the grid from the capture.
      grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
      {
        std::ifstream bin(bin_path, std::ios::binary);
        if (!bin.is_open()) {
          std::fprintf(stderr, "cannot open %s\n", bin_path.c_str());
          return 1;
        }
        std::vector<cf_t> symbol(nof_subc);
        const auto        ports = capture.get_list("rx_ports");
        for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
          for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
            bin.read(reinterpret_cast<char*>(symbol.data()), static_cast<std::streamsize>(symbol.size() * sizeof(cf_t)));
            grid->get_writer().put(ports[i_port], i_symbol, bwp_start * NOF_SUBCARRIERS_PER_RB, symbol);
          }
        }
      }
    }

    report("rebuilding the PDU");
    // Rebuild the PDU.
    pusch_processor::pdu_t pdu = {};
    if (hop_td_slots != 0) {
      // In hop mode the capture supplies the CONFIGURATION and the front end supplies the grid, so the
      // slot is the front end's - not the one the capture was recorded at. This is also the value the PDU
      // hands the channel estimator (ch_est_config.slot = pdu.slot), i.e. the key the hop looks its block
      // up by, so it MUST be the number the front end was told (set_lane_slot, front_end.produce()).
      pdu.slot = slot_point(to_scs(capture.get_unsigned("scs_khz", 15)), receiving_slot);
    } else {
      pdu.slot = slot_point(to_scs(capture.get_unsigned("scs_khz", 15)), capture.get_unsigned("slot"));
      // A repetition is a NEW reception: it must not reuse the recorded slot, or the receive buffer
      // pool would hand the same HARQ slot to a second reservation and the two would collide.
      pdu.slot += rep * 40;
    }
    pdu.rnti                   = to_rnti(static_cast<uint16_t>(capture.get_unsigned("rnti")));
    pdu.harq_id                = static_cast<harq_id_t>(capture.get_unsigned("harq_id"));
    pdu.bwp_size_rb            = bwp_size;
    pdu.bwp_start_rb           = bwp_start;
    pdu.cp                     = (capture.get_string("cp") == "extended") ? cyclic_prefix::EXTENDED : cyclic_prefix::NORMAL;
    pdu.n_id                   = capture.get_unsigned("n_id");
    pdu.nof_tx_layers          = capture.get_unsigned("nof_tx_layers", 1);
    pdu.start_symbol_index     = capture.get_unsigned("start_symbol_index");
    pdu.nof_symbols            = capture.get_unsigned("nof_symbols", MAX_NSYMB_PER_SLOT);
    pdu.tbs_lbrm               = units::bytes(capture.get_unsigned("tbs_lbrm", tbs_lbrm_default.value()));
    pdu.mcs_descr.modulation   = to_modulation(capture.get_string("modulation", "QPSK"));
    pdu.mcs_descr.target_code_rate = static_cast<float>(capture.get_unsigned("target_code_rate"));
    pdu.codeword = pusch_processor::codeword_description{
        .rv              = capture.get_unsigned("rv"),
        .ldpc_base_graph = static_cast<ldpc_base_graph_type>(capture.get_unsigned("ldpc_base_graph", 2)),
        .new_data        = capture.get_unsigned("new_data", 1) != 0};
    pdu.uci.nof_harq_ack       = capture.get_unsigned("nof_harq_ack");
    pdu.dc_position            = (capture.get_unsigned("dc_position", 0xFFFFFFFF) == 0xFFFFFFFF)
                                     ? std::nullopt
                                     : std::optional<unsigned>(capture.get_unsigned("dc_position"));
    const auto rx_ports        = capture.get_list("rx_ports");
    for (unsigned port : rx_ports) {
      pdu.rx_ports.push_back(static_cast<uint8_t>(port));
    }
    pdu.dmrs_symbol_mask = symbol_slot_mask(MAX_NSYMB_PER_SLOT);
    for (unsigned i_symbol : capture.get_list("dmrs_symbols")) {
      pdu.dmrs_symbol_mask.set(i_symbol);
    }
    pdu.dmrs = pusch_processor::dmrs_configuration{.dmrs      = static_cast<dmrs_config_type>(capture.get_unsigned("dmrs_type", 1)),
                                                   .scrambling_id = capture.get_unsigned("dmrs_scrambling_id"),
                                                   .n_scid        = capture.get_unsigned("dmrs_n_scid") != 0,
                                                   .nof_cdm_groups_without_data =
                                                       capture.get_unsigned("dmrs_nof_cdm_groups_without_data", 2)};
    // Frequency allocation, rebuilt from the PRB LIST the capture writes (ul_capture.cpp:173-179 writes
    // them relative to the BWP). A list that is a contiguous run - every recorded reception of this line
    // so far - becomes the type-1 allocation it came from, exactly as before. A list with a HOLE cannot
    // be written as type 1, and reinterpreting it as "first PRB + count" (what this line used to do)
    // silently replays a DIFFERENT allocation: nothing downstream ever sees the hole, every gate passes,
    // and the run looks clean - a vacuous pass of exactly the kind the sparse-RB-mask work (S13 G3a, and
    // the `ta_stride` / `ls_geometry` refusals) is about. Such a capture becomes a type-0 bitmap and is
    // announced, so a sparse probe cannot be mistaken for a contiguous one again.
    const std::vector<unsigned> alloc_prb  = capture.get_list("alloc_prb");
    const unsigned              rb_start   = alloc_prb.empty() ? bwp_start : bwp_start + alloc_prb.front();
    const bool                  has_a_hole = [&alloc_prb]() {
      for (size_t i = 1; i != alloc_prb.size(); ++i) {
        if (alloc_prb[i] != alloc_prb[i - 1] + 1) {
          return true;
        }
      }
      return false;
    }();
    if (!has_a_hole) {
      pdu.freq_alloc = rb_allocation::make_type1(rb_start, static_cast<unsigned>(alloc_prb.size()), std::nullopt);
    } else {
      const unsigned max_prb = *std::max_element(alloc_prb.begin(), alloc_prb.end());
      vrb_bitmap     bits(bwp_start + max_prb + 1);
      for (unsigned i_prb : alloc_prb) {
        bits.set(bwp_start + i_prb);
      }
      pdu.freq_alloc = rb_allocation::make_type0(bits, std::nullopt);
      std::printf(
          "allocation: %zu PRB WITH A HOLE (type-0 bitmap, first=%u last=%u) - the device kernels are "
          "expected to REFUSE this hop and fall back to the host\n",
          alloc_prb.size(),
          alloc_prb.front(),
          alloc_prb.back());
    }

    // Transport block size, as the processor derives it from the MCS and the allocation.
    tbs_calculator_configuration tbs_config = {};
    tbs_config.mcs_descr                    = pdu.mcs_descr;
    tbs_config.n_prb                        = pdu.freq_alloc.get_nof_rb();
    tbs_config.nof_layers                   = pdu.nof_tx_layers;
    tbs_config.nof_symb_sh                  = pdu.nof_symbols;
    tbs_config.nof_dmrs_prb = get_nof_re_per_prb(dmrs_config_type::type1) * pdu.dmrs_symbol_mask.count() * 2;
    const unsigned tbs        = tbs_calculator_calculate(tbs_config).value();
    // Number of codeblocks of this transport block: the pool, the reservation and the decoder all
    // have to agree on it, otherwise the rate dematcher writes past the soft buffer.
    // The processor takes the transport block size from the size of the buffer it is given and the
    // LDPC base graph from the codeword, and it asserts that the number of codeblocks it derives
    // matches the reservation (a Release build then corrupts memory silently), so all three have to
    // agree here. Captures written before the base graph was recorded fall back to the value
    // derived from the code rate.
    const ldpc_base_graph_type base_graph =
        capture.fields.count("ldpc_base_graph") != 0
            ? static_cast<ldpc_base_graph_type>(capture.get_unsigned("ldpc_base_graph"))
            : get_ldpc_base_graph(pdu.mcs_descr.get_normalised_target_code_rate(), units::bits(tbs));
    pdu.codeword->ldpc_base_graph = base_graph;
    const unsigned       nof_codeblocks = compute_nof_codeblocks(units::bits(tbs), base_graph);
    std::vector<uint8_t> data(tbs);

    // Run the receiver(s) and report. The processor is asynchronous, so each one stays alive until its
    // notifier fires.
    //
    // ALL of this slot's hops are SUBMITTED BEFORE ANY OF THEM IS WAITED FOR, and that is the whole point
    // of the multi-PUSCH arm (--hop-pdus K). Waiting in between would let the first hop's commit complete
    // before the second hop looks the block up, and a MISS whose commit has already finished needs no
    // ordering at all - which is exactly why every single-hop arm failed to falsify the device-side wait
    // (5.9.39). Submitted together, the second hop MISSES while the first hop's commit is still in flight,
    // so the wait it encodes is the only thing ordering its read against a DEVICE consumer's writes.
    const unsigned nof_pdus = (hop_td_slots != 0) ? hop_pdus : 1U;
    // One pdu_t PER HOP, reserved up front so nothing reallocates under a reference the processor may be
    // holding: the hops differ only in their RNTI, which the receive buffer pool keys its reservations by
    // (and the staged capture keys its files by) - K hops of one slot need K identifiers to stay apart.
    std::vector<pusch_processor::pdu_t>               hop_pdu;
    std::vector<std::unique_ptr<result_spy>>          spies;
    std::vector<std::unique_ptr<pusch_processor>>     receivers;
    hop_pdu.reserve(nof_pdus);
    spies.reserve(nof_pdus);
    receivers.reserve(nof_pdus);
    const uint16_t base_rnti = to_value(pdu.rnti);
    for (unsigned k = 0; k != nof_pdus; ++k) {
      hop_pdu.push_back(pdu);
      hop_pdu.back().rnti = to_rnti(static_cast<uint16_t>(base_rnti + k));
      auto spy            = std::make_unique<result_spy>();
      unique_rx_buffer buffer = buffer_pool->get_pool().reserve(
          hop_pdu.back().slot, trx_buffer_identifier(hop_pdu.back().rnti, 0), nof_codeblocks, true);
      if (!buffer) {
        std::fprintf(stderr, "cannot reserve a receive buffer\n");
        return 1;
      }
      auto receiver = proc_factory->create();
      check(receiver, "pusch processor");
      receiver->process(data, std::move(buffer), *spy, grid->get_reader(), hop_pdu.back());
      spies.push_back(std::move(spy));
      receivers.push_back(std::move(receiver));
    }
    for (std::unique_ptr<result_spy>& spy : spies) {
      for (unsigned wait = 0; (wait != 5000) && !spy->done; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    for (unsigned k = 0; k != nof_pdus; ++k) {
      const result_spy& spy = *spies[k];
      std::printf("  [%u] tbs=%u slot=%u rnti=%u %s: crc=%s iterations=%u sinr=%.2f dB epre=%.2f dB rsrp=%.2f dB\n",
                  i,
                  tbs,
                  hop_pdu[k].slot.count(),
                  hop_pdu[k].rnti,
                  capture.get_string("modulation", "?").c_str(),
                  spy.crc_ok ? "OK" : "KO",
                  spy.nof_iters,
                  spy.sinr_db,
                  spy.epre_db,
                  spy.rsrp_db);
    }
    ++replayed;
    }
  }

  std::printf("replayed %u reception(s); staged capture written to %s_<slot>_<rnti>{,.bin,_ce.txt,_llr.bin}\n",
              replayed,
              out_prefix.c_str());

  // ------------------------------------------------------------------------------------------------
  // L1b: what the hop half of the hand-over did (see --hop-td). The comparison the driver script makes
  // is between the LLRs of the arms; these counters are what say each arm exercised what it meant to -
  // a hop that MISSED when it should have ADOPTED still produces plausible LLRs (see 5.9.19).
  // ------------------------------------------------------------------------------------------------
  if (hop_td_slots != 0) {
    grid_handover_counts hs;
    grid_ready_hook::counts(hs);
    // grid_handover_armed() is the knob's ONE definition (5.9.49 moved its default to ON), so the report
    // cannot disagree with what the pipeline did. The CONTROL arm is now `OCUDU_DFT_RELEASE_BLOCK=0`, not
    // "unset" - see the arm scripts.
    const bool release_armed = ocudu::grid_handover_armed();
    std::printf("[l1_hop] installed=%d armed=%d host_first=%d claim_only=%d pdus=%u slots=%u handed=%llu "
                "taken=%llu fallback=%llu late=%llu not_found=%llu unproduced=%llu ready_timeouts=%llu\n",
                hs.installed ? 1 : 0,
                release_armed ? 1 : 0,
                host_first ? 1 : 0,
                claim_only ? 1 : 0,
                hop_pdus,
                hop_td_slots,
                static_cast<unsigned long long>(hs.handed),
                static_cast<unsigned long long>(hs.taken),
                static_cast<unsigned long long>(hs.fallback_commits),
                static_cast<unsigned long long>(hs.late_commits),
                static_cast<unsigned long long>(hs.not_found),
                static_cast<unsigned long long>(hs.unproduced),
                static_cast<unsigned long long>(hs.ready_timeouts));
    if (!release_armed) {
      // The CONTROL arm (OCUDU_DFT_RELEASE_BLOCK=0 since 5.9.49 moved the default to ON): the front end
      // commits its own block, so the hop adopts nothing and is right not to. A hand-over here would mean
      // this arm is not the reference it claims to be.
      if (hs.handed != 0) {
        std::fprintf(stderr,
                     "FAIL: the hand-over happened (%llu) without OCUDU_DFT_RELEASE_BLOCK - this arm is not "
                     "the reference it claims to be\n",
                     static_cast<unsigned long long>(hs.handed));
        return 1;
      }
    } else {
      if (hs.handed == 0) {
        std::fprintf(stderr, "FAIL: --hop-td handed NOTHING over - the hop read a grid the front end "
                             "committed itself, so the fused chain was not exercised\n");
        return 1;
      }
      // Which half each order MUST exercise: the hop that goes first adopts the block, and the hop that
      // runs after another consumer claimed it cannot (the block is taken) and has to take the MISS path.
      if (host_first || claim_only) {
        if (hs.taken != 0) {
          std::fprintf(stderr,
                       "FAIL: another consumer took the grid first yet the hop still ADOPTED (%llu) - the MISS "
                       "path was not exercised\n",
                       static_cast<unsigned long long>(hs.taken));
          return 1;
        }
        if (hs.fallback_commits == 0) {
          std::fprintf(stderr, "FAIL: the other consumer committed nothing - no fallback happened\n");
          return 1;
        }
      } else if (hs.taken == 0) {
        std::fprintf(stderr, "FAIL: the hop ADOPTED nothing (%llu handed, %llu taken) - the fused single-"
                             "submission chain was not exercised\n",
                     static_cast<unsigned long long>(hs.handed),
                     static_cast<unsigned long long>(hs.taken));
        return 1;
      }
    }
    if (hs.ready_timeouts != 0) {
      std::fprintf(stderr, "FAIL: %llu wait(s) timed out\n", static_cast<unsigned long long>(hs.ready_timeouts));
      return 1;
    }
    // The multi-PUSCH arm: of K hops on one grid, the registry lets exactly ONE adopt the front end's block
    // (take_released() refuses a claimed one), so the other K-1 MUST miss. That is the structural cliff -
    // not a race - and this is where it is measured instead of argued about.
    if (hop_pdus > 1) {
      const uint64_t expected_taken = release_armed && !host_first && !claim_only ? hop_td_slots : 0;
      if (hs.taken != expected_taken) {
        std::fprintf(stderr,
                     "FAIL: %u hops per slot but %llu adopted (expected %llu) - the multi-PUSCH cliff did "
                     "not behave structurally\n",
                     hop_pdus,
                     static_cast<unsigned long long>(hs.taken),
                     static_cast<unsigned long long>(expected_taken));
        return 1;
      }
      // `handed` counts DEPOSITS, one per receiving slot, not hops: the hops that missed are the ones no
      // deposit was left for.
      const uint64_t nof_hops = static_cast<uint64_t>(hop_pdus) * hop_td_slots;
      const uint64_t misses   = nof_hops - hs.taken;
      std::printf("[l1_multi] %u hops per slot x %u slot(s): adopted=%llu missed=%llu "
                  "(a slot's submissions go from 1 to %u unless the rest can share the block)\n",
                  hop_pdus,
                  hop_td_slots,
                  static_cast<unsigned long long>(hs.taken),
                  static_cast<unsigned long long>(misses),
                  hop_pdus);
      if (release_armed && (misses == 0)) {
        std::fprintf(stderr, "FAIL: no hop missed, so the multi-PUSCH shape was not exercised\n");
        return 1;
      }
    }
  }

  worker_pool->stop();
  return (replayed == 0) ? 1 : 0;
}
