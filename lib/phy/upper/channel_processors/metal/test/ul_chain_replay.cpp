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
///                   [--dft [--device-grid]]   replay a RECORDED TIME-DOMAIN capture (OCUDU_UL_DUMP_TD)
///                                             through the OFDM demodulator and dump the grids: the
///                                             same IQ through two builds is the A/B of the DFT path,
///                                             and --device-grid switches the grid write to the device.
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
      dft_mode = true;
    } else if (arg == "--dft-metal") {
      dft_mode = true;
      dft_metal = true;
    } else if (arg == "--device-grid") {
      device_grid = true;
    } else if ((arg == "--td-strategy") && (i + 1 < argc)) {
      td_strategy_average = (std::string(argv[++i]) == "average");
    } else if (arg == "--cpu") {
      use_metal_ce = use_metal_demod = use_metal_decoder = false;
    } else if (arg == "--metal") {
      use_metal_ce = use_metal_demod = use_metal_decoder = true;
    } else if (arg == "--metal-cpu-ldpc") {
      use_metal_ce = use_metal_demod = true;
      use_metal_decoder              = false;
    } else if (arg == "--metal-cpu-demod") {
      use_metal_ce  = true;
      use_metal_demod = false;
      use_metal_decoder = false;
    } else if (arg == "-h" || arg == "--help") {
      std::printf("usage: %s <capture-base> --out <prefix> [--cpu|--metal|--metal-cpu-ldpc|--metal-cpu-demod]\n"
                  "  <capture-base> is one reception of a capture, e.g. /tmp/C_4352_17921\n",
                  argv[0]);
      return 0;
    } else if (arg[0] != '-') {
      prefix = arg;
    } else {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      return 1;
    }
  }

  if (prefix.empty() || out_prefix.empty()) {
    std::fprintf(stderr, "usage: %s <capture-base> --out <prefix> [--cpu|--metal]\n", argv[0]);
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
  setenv("OCUDU_UL_DUMP_COUNT", "1", 1);

  // --------------------------------------------------------------------------------------------
  // Time-domain replay: rebuild the resource grid of each recorded slot with the selected DFT
  // back end and pipeline depth (OCUDU_DFT_PIPELINE_DEPTH), which is the only way to compare the
  // pipelined front end with the serial one on the same samples.
  // --------------------------------------------------------------------------------------------
  if (dft_mode) {
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

    struct entry_t {
      unsigned slot;
      unsigned symbol;
      unsigned port;
      size_t   size;
    };
    std::vector<entry_t> entries;
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
        entries.push_back(entry);
      }
    }
    if (entries.empty()) {
      std::fprintf(stderr, "%s_td.txt holds no transform\n", prefix.c_str());
      return 1;
    }

    // All the samples of the capture, read once.
    std::vector<ci16_t> samples;
    {
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
    const unsigned               first_cp_len =
        dft_cp.get_length(entries.front().symbol, dft_scs).to_samples(dft_sampling_rate_Hz);
    if (entries.front().size <= first_cp_len) {
      std::fprintf(stderr,
                   "%s_td.txt: symbol size %zu does not cover its cyclic prefix %u\n",
                   prefix.c_str(),
                   entries.front().size,
                   first_cp_len);
      return 1;
    }
    const unsigned inferred_dft_size = entries.front().size - first_cp_len;

    ofdm_demodulator_configuration demod_config = {};
    demod_config.numerology                = 0;
    demod_config.bw_rb                     = nof_prb;
    demod_config.dft_size                  = inferred_dft_size;
    demod_config.cp                        = dft_cp;
    demod_config.nof_samples_window_offset = 0;
    demod_config.scale                     = 1.0F;
    demod_config.center_freq_Hz            = 0.0;
    demod_config.device_grid_write         = device_grid;
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
                prefix.c_str(),
                out_prefix.c_str(),
                dft_metal ? "metal" : "cpu",
                depth,
                nof_prb,
                entries.size(),
                device_grid ? "device" : "host");

    unsigned                cursor = 0;
    std::vector<unsigned>   ring_slots;
    std::shared_ptr<resource_grid> grid;
    unsigned                current_slot = std::numeric_limits<unsigned>::max();
    unsigned                ring         = 0;
    unsigned                drained      = 0;

    auto write_grid = [&](unsigned slot) {
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
    };

    for (const entry_t& entry : entries) {
      if (entry.slot != current_slot) {
        // Close the previous slot: finish every transform still in flight.
        if (grid != nullptr) {
          while (drained != ring_slots.size()) {
            demodulator->finish_symbol(grid->get_writer(), ring_slots[drained++]);
          }
          write_grid(current_slot);
        }
        current_slot = entry.slot;
        ring_slots.clear();
        drained = 0;
        ring    = 0;
        grid    = dft_grid_factory->create(1, MAX_NSYMB_PER_SLOT, nof_prb * NOF_SUBCARRIERS_PER_RB);
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
    if (grid != nullptr) {
      while (drained != ring_slots.size()) {
        demodulator->finish_symbol(grid->get_writer(), ring_slots[drained++]);
      }
      write_grid(current_slot);
    }
    std::printf("dft replay done: grid dumps written as %s_<slot>_dft{.txt,.bin}\n", out_prefix.c_str());
    return 0;
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
  for (const std::string& capture_prefix : prefixes) {
    for (unsigned rep = 0; rep != repeat; ++rep) {
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

    report("rebuilding the grid");
    // Rebuild the grid.
    std::shared_ptr<resource_grid> grid = grid_factory->create(nof_ports, MAX_NSYMB_PER_SLOT, MAX_NOF_SUBCARRIERS);
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

    report("rebuilding the PDU");
    // Rebuild the PDU.
    pusch_processor::pdu_t pdu = {};
    pdu.slot                   = slot_point(to_scs(capture.get_unsigned("scs_khz", 15)), capture.get_unsigned("slot"));
    // A repetition is a NEW reception: it must not reuse the recorded slot, or the receive buffer
    // pool would hand the same HARQ slot to a second reservation and the two would collide.
    pdu.slot += rep * 40;
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
    // Frequency allocation: a type 1 allocation of the recorded RBs (contiguous, as the capture
    // writes them relative to the BWP).
    const std::vector<unsigned> alloc_prb = capture.get_list("alloc_prb");
    const unsigned              rb_start  = alloc_prb.empty() ? bwp_start : bwp_start + alloc_prb.front();
    pdu.freq_alloc = rb_allocation::make_type1(rb_start, static_cast<unsigned>(alloc_prb.size()), std::nullopt);

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

    // Run the receiver and report. The processor is asynchronous, so it stays alive until its
    // notifier fires.
    result_spy       spy;
    unique_rx_buffer buffer =
        buffer_pool->get_pool().reserve(pdu.slot, trx_buffer_identifier(pdu.rnti, 0), nof_codeblocks, true);
    if (!buffer) {
      std::fprintf(stderr, "cannot reserve a receive buffer\n");
      return 1;
    }
    std::unique_ptr<pusch_processor> receiver = proc_factory->create();
    check(receiver, "pusch processor");
    receiver->process(data, std::move(buffer), spy, grid->get_reader(), pdu);
    for (unsigned wait = 0; (wait != 5000) && !spy.done; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("  [%u] tbs=%u slot=%u rnti=%u %s: crc=%s iterations=%u sinr=%.2f dB epre=%.2f dB rsrp=%.2f dB\n",
                i,
                tbs,
                pdu.slot.count(),
                pdu.rnti,
                capture.get_string("modulation", "?").c_str(),
                spy.crc_ok ? "OK" : "KO",
                spy.nof_iters,
                spy.sinr_db,
                spy.epre_db,
                spy.rsrp_db);
    ++replayed;
    }
  }

  std::printf("replayed %u reception(s); staged capture written to %s_<slot>_<rnti>{,.bin,_ce.txt,_llr.bin}\n",
              replayed,
              out_prefix.c_str());
  worker_pool->stop();
  return (replayed == 0) ? 1 : 0;
}
