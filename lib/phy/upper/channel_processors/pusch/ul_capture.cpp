// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ul_capture.h"

#include "ocudu/phy/support/resource_grid_writer.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

std::string capture_prefix()
{
  const char* env = std::getenv("OCUDU_UL_DUMP");
  return (env != nullptr) ? std::string(env) : std::string();
}

unsigned capture_budget()
{
  const char* env = std::getenv("OCUDU_UL_DUMP_COUNT");
  return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 8U;
}

std::string make_key(slot_point slot, rnti_t rnti)
{
  return capture_prefix() + "_" + std::to_string(slot.count()) + "_" + std::to_string(to_value(rnti));
}

/// Receptions selected for capture. The decision is taken once, by the grid capture, and the
/// later stages (which may run on other threads) only check membership, so all three stage files
/// of a reception exist together and the budget bounds the whole set.
std::mutex&                      selected_mutex()
{
  static std::mutex m;
  return m;
}

std::set<std::string>& selected()
{
  static std::set<std::string> keys;
  return keys;
}

bool select(slot_point slot, rnti_t rnti)
{
  static std::atomic<unsigned> count{0};
  std::lock_guard              lock(selected_mutex());
  if (count.fetch_add(1, std::memory_order_relaxed) >= capture_budget()) {
    return false;
  }
  selected().insert(make_key(slot, rnti));
  return true;
}

bool is_selected(slot_point slot, rnti_t rnti)
{
  std::lock_guard lock(selected_mutex());
  return selected().count(make_key(slot, rnti)) != 0;
}

/// Reception the captures belong to (the demodulator has no access to the PDU).
struct current_pdu {
  slot_point slot;
  rnti_t     rnti = to_rnti(0);
  bool       valid = false;
};

current_pdu& current()
{
  static thread_local current_pdu key;
  return key;
}

std::string make_key()
{
  const current_pdu& key = current();
  return make_key(key.slot, key.rnti);
}

} // namespace

bool ocudu::ul_capture::enabled()
{
  static const bool on = (std::getenv("OCUDU_UL_DUMP") != nullptr);
  return on;
}

bool ocudu::ul_capture::llr_enabled()
{
  static const bool on = []() {
    const char* env = std::getenv("OCUDU_UL_DUMP_LLR");
    return (env != nullptr) && (std::string(env) != "0");
  }();
  return on;
}

void ocudu::ul_capture::capture_grid(const resource_grid_reader& grid, const pusch_processor::pdu_t& pdu)
{
  if (!enabled() || !pdu.codeword.has_value() || !select(pdu.slot, pdu.rnti)) {
    return;
  }
  const std::string name = make_key(pdu.slot, pdu.rnti);

  if (FILE* f = std::fopen((name + ".txt").c_str(), "w")) {
    std::fprintf(f, "slot=%u\n", pdu.slot.count());
    std::fprintf(f, "scs_khz=%u\n", scs_to_khz(pdu.slot.scs()));
    std::fprintf(f, "cp=%s\n", pdu.cp.to_string().c_str());
    std::fprintf(f, "rnti=%u\n", to_value(pdu.rnti));
    std::fprintf(f, "harq_id=%u\n", static_cast<unsigned>(pdu.harq_id));
    std::fprintf(f, "bwp_size_rb=%u\n", pdu.bwp_size_rb);
    std::fprintf(f, "bwp_start_rb=%u\n", pdu.bwp_start_rb);
    std::fprintf(f, "n_id=%u\n", pdu.n_id);
    std::fprintf(f, "nof_tx_layers=%u\n", pdu.nof_tx_layers);
    std::fprintf(f, "start_symbol_index=%u\n", pdu.start_symbol_index);
    std::fprintf(f, "nof_symbols=%u\n", pdu.nof_symbols);
    std::fprintf(f, "modulation=%s\n", to_string(pdu.mcs_descr.modulation).c_str());
    std::fprintf(f, "target_code_rate=%u\n", static_cast<unsigned>(std::lround(pdu.mcs_descr.target_code_rate)));
    std::fprintf(f, "rv=%u\n", static_cast<unsigned>(pdu.codeword->rv));
    std::fprintf(f, "ldpc_base_graph=%u\n", static_cast<unsigned>(pdu.codeword->ldpc_base_graph));
    std::fprintf(f, "new_data=%d\n", static_cast<int>(pdu.codeword->new_data));
    std::fprintf(f, "tbs_lbrm=%u\n", pdu.tbs_lbrm.value());
    std::fprintf(f, "nof_harq_ack=%u\n", pdu.uci.nof_harq_ack);
    std::fprintf(f, "dc_position=%d\n", pdu.dc_position.has_value() ? static_cast<int>(*pdu.dc_position) : -1);
    if (std::holds_alternative<pusch_processor::dmrs_configuration>(pdu.dmrs)) {
      const auto& dmrs = std::get<pusch_processor::dmrs_configuration>(pdu.dmrs);
      std::fprintf(f, "dmrs_type=%u\n", static_cast<unsigned>(dmrs.dmrs));
      std::fprintf(f, "dmrs_scrambling_id=%u\n", dmrs.scrambling_id);
      std::fprintf(f, "dmrs_n_scid=%d\n", static_cast<int>(dmrs.n_scid));
      std::fprintf(f, "dmrs_nof_cdm_groups_without_data=%u\n", dmrs.nof_cdm_groups_without_data);
    }
    std::fprintf(f, "rx_ports=");
    for (unsigned i_port = 0; i_port != pdu.rx_ports.size(); ++i_port) {
      std::fprintf(f, "%s%u", (i_port == 0) ? "" : ",", pdu.rx_ports[i_port]);
    }
    std::fprintf(f, "\ndmrs_symbols=");
    bool first = true;
    pdu.dmrs_symbol_mask.for_each(0, pdu.dmrs_symbol_mask.size(), [&](unsigned i_symbol) {
      std::fprintf(f, "%s%u", first ? "" : ",", i_symbol);
      first = false;
    });
    std::fprintf(f, "\nalloc_nof_rb=%u\n", pdu.freq_alloc.get_nof_rb());
    std::fprintf(f, "alloc_prb=");
    const crb_bitmap mask = pdu.freq_alloc.get_crb_mask(pdu.bwp_start_rb, pdu.bwp_size_rb);
    first                 = true;
    mask.for_each(pdu.bwp_start_rb, pdu.bwp_start_rb + pdu.bwp_size_rb, [&](unsigned i_prb) {
      std::fprintf(f, "%s%u", first ? "" : ",", i_prb - pdu.bwp_start_rb);
      first = false;
    });
    std::fprintf(f, "\n");
    std::fclose(f);
  }

  const unsigned    nof_subc  = pdu.bwp_size_rb * NOF_SUBCARRIERS_PER_RB;
  const unsigned    nof_ports = pdu.rx_ports.size();
  if (FILE* f = std::fopen((name + ".bin").c_str(), "wb")) {
    std::vector<cf_t> symbol(nof_subc);
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
        grid.get(symbol, pdu.rx_ports[i_port], i_symbol, pdu.bwp_start_rb * NOF_SUBCARRIERS_PER_RB);
        std::fwrite(symbol.data(), sizeof(cf_t), symbol.size(), f);
      }
    }
    std::fclose(f);
  }
}

void ocudu::ul_capture::capture_ce(const dmrs_pusch_estimator_results& est_results,
                                   const pusch_processor::pdu_t&       pdu)
{
  if (!enabled() || !pdu.codeword.has_value() || !is_selected(pdu.slot, pdu.rnti)) {
    return;
  }
  FILE* f = std::fopen((make_key(pdu.slot, pdu.rnti) + "_ce.txt").c_str(), "w");
  if (f == nullptr) {
    return;
  }
  for (unsigned i_port = 0; i_port != pdu.rx_ports.size(); ++i_port) {
    const unsigned       port = pdu.rx_ports[i_port];
    std::optional<float> cfo  = est_results.get_cfo_Hz(port);
    std::fprintf(f,
                 "port=%u noise_variance=%.9e snr=%.6f rsrp=%.9e epre=%.9e ta_us=%.6f cfo_hz=%s\n",
                 port,
                 static_cast<double>(est_results.get_noise_variance(port)),
                 static_cast<double>(est_results.get_snr(port)),
                 static_cast<double>(est_results.get_rsrp(port)),
                 static_cast<double>(est_results.get_epre(port)),
                 est_results.get_time_alignment(port).to_seconds() * 1e6,
                 cfo.has_value() ? std::to_string(*cfo).c_str() : "na");
  }
  std::fclose(f);
}

void ocudu::ul_capture::set_current(slot_point slot, rnti_t rnti)
{
  current().slot  = slot;
  current().rnti  = rnti;
  current().valid = true;
}

void ocudu::ul_capture::capture_h(const dmrs_pusch_estimator_results& est_results,
                                  const pusch_processor::pdu_t&       pdu)
{
  if (!enabled() || !pdu.codeword.has_value() || !is_selected(pdu.slot, pdu.rnti)) {
    return;
  }
  const unsigned nof_subc = pdu.bwp_size_rb * NOF_SUBCARRIERS_PER_RB;
  const unsigned nof_layers = pdu.nof_tx_layers;
  const unsigned nof_ports  = pdu.rx_ports.size();

  FILE* f = std::fopen((make_key(pdu.slot, pdu.rnti) + "_h.bin").c_str(), "wb");
  if (f == nullptr) {
    return;
  }
  bounded_bitset<MAX_NOF_SUBCARRIERS> mask(nof_subc);
  mask.fill(0, nof_subc);
  std::vector<cbf16_t> h(nof_subc);
  std::vector<cf_t>    out(nof_subc);
  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
        est_results.get_symbol_ch_estimate(h, i_symbol, pdu.rx_ports[i_port], i_layer, mask);
        for (unsigned i = 0; i != nof_subc; ++i) {
          out[i] = to_cf(h[i]);
        }
        std::fwrite(out.data(), sizeof(cf_t), out.size(), f);
      }
    }
  }
  std::fclose(f);
}

void ocudu::ul_capture::capture_llr(span<const log_likelihood_ratio> llr)
{
  if (!llr_enabled() || !current().valid || !is_selected(current().slot, current().rnti)) {
    return;
  }
  static std::mutex mutex;
  std::lock_guard   lock(mutex);
  FILE*             f = std::fopen((make_key() + "_llr.bin").c_str(), "ab");
  if (f == nullptr) {
    return;
  }
  const uint32_t      size = static_cast<uint32_t>(llr.size());
  std::vector<int8_t> data(llr.size());
  for (size_t i = 0; i != llr.size(); ++i) {
    data[i] = static_cast<int8_t>(llr[i].to_int());
  }
  std::fwrite(&size, sizeof(size), 1, f);
  std::fwrite(data.data(), sizeof(int8_t), data.size(), f);
  std::fclose(f);
}
