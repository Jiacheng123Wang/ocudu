// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ul_capture.h"

#include "ocudu/phy/support/resource_grid_writer.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace ocudu;

// Compiled out by default (ENABLE_UL_CAPTURE): a release build then contains none of the capture
// machinery - no buffers, no file I/O, no environment lookups - only the no-op API at the bottom of
// this file, so that no call site changes. The host touches a capture would cause therefore cannot
// happen, which is why the pipeline contract never has to reason about them (see
// phy_pipeline_crossings::scoped_debug_touches for the builds where the capture IS compiled in).
#if defined(OCUDU_UL_CAPTURE)

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

/// \brief Allocation-width filter (OCUDU_UL_DUMP_MAX_RB): only receptions whose allocation is at
/// most this many PRBs are candidates. Unset (or 0) captures any width.
///
/// It exists because WHO built a hop's correlation matrices depends on that width: the estimator
/// works in blocks of block_prb PRBs (3), and a hop NARROWER than one block takes a different route
/// through the Metal estimator (S13-P1's leg measured 0.10 host writes per hop on those, and the
/// 2 PRB rows of every air leg decode at 0.0%). The budget-based selection ("the first N
/// receptions") essentially never picks one of them out of a data-heavy leg, so the route could not
/// be recorded at all - which is what this filter fixes.
unsigned capture_max_rb()
{
  const char* env = std::getenv("OCUDU_UL_DUMP_MAX_RB");
  return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 0U;
}

/// \brief Allocation-width floor (OCUDU_UL_DUMP_MIN_RB): only receptions at least this many PRBs wide
/// are candidates. Unset (or 0) captures any width.
///
/// The mirror of capture_max_rb(), and it exists for the same reason the model's precision envelope does:
/// the depth-3 network describes allocations of MIN_PRB (4) and wider, and the criterion-1 pool needs the
/// WIDE ones -- the scheduler hands big grants to high-throughput traffic and small ones to the traffic
/// that succeeds, so a budget that counts both fills up with receptions no measurement can use. In the
/// corpus collected without this filter, 357 of 1956 captures were narrower than 43 PRB and every one of
/// them was a slot the budget could have spent on a usable one. A rejected reception does not spend the
/// budget, exactly as with the ceiling.
unsigned capture_min_rb()
{
  const char* env = std::getenv("OCUDU_UL_DUMP_MIN_RB");
  return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 0U;
}

/// \brief Redundancy-version filter (OCUDU_UL_DUMP_RV): empty captures every version, otherwise only
/// receptions whose codeword carries this one.
///
/// It is here because a criterion-1 sample has to decode from ONE reception: a retransmission is
/// decodable only with the HARQ soft buffer the live chain holds and the offline replay does not
/// reproduce, so an rv=2 capture that the chain decoded is not a sample the comparison can use. With the
/// budget bounded, spending it on retransmissions is spending it on nothing.
std::optional<unsigned> capture_rv()
{
  const char* env = std::getenv("OCUDU_UL_DUMP_RV");
  if (env == nullptr) {
    return std::nullopt;
  }
  return static_cast<unsigned>(std::strtoul(env, nullptr, 10));
}

std::string make_key(slot_point slot, rnti_t rnti)
{
  return capture_prefix() + "_" + std::to_string(slot.count()) + "_" + std::to_string(to_underlying(rnti));
}

/// \brief Reports, ONCE, that a capture cannot be written -- on stderr, and not through a logger.
///
/// A capture that silently writes nothing is the worst outcome a leg can have: every counter is green, the
/// rate is normal, the run looks healthy, and the corpus is empty when someone finally looks at it. It has
/// happened: on 2026-10-14 a leg ran for eight minutes and 99672 receptions with
/// `OCUDU_UL_DUMP=/Users/.../aillr_cap003/C` whose directory did not exist -- `fopen` does not create
/// directories -- and the gNB printed nothing at all, so the loss was only discovered when the audit was
/// run. stderr rather than the logger, for the reason F20 gives: whether a corpus exists must not depend on
/// whether a log level happened to be enabled.
void report_capture_failure(const char* path, const char* why)
{
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) {
    return;
  }
  std::fprintf(stderr,
               "[ul_capture] NOT CAPTURING: cannot write '%s': %s. Every capture of this leg is being "
               "DROPPED. Create the directory the prefix names and re-run.\n",
               path,
               why);
}

/// \brief True when the directory the prefix names exists, checked ONCE before the first capture.
///
/// `fopen` does not create directories, so a prefix whose directory is missing turns every capture into a
/// silent no-op -- and it costs nothing to find out before the budget is spent.
bool capture_directory_exists()
{
  const std::string prefix = capture_prefix();
  const std::size_t slash  = prefix.find_last_of('/');
  if (slash == std::string::npos) {
    // A bare prefix writes into the working directory, which exists by construction.
    return true;
  }
  struct stat info{};
  return (::stat(prefix.substr(0, slash).c_str(), &info) == 0) && S_ISDIR(info.st_mode);
}

/// \brief Says, ONCE, where the captures are going and how many are budgeted.
///
/// The positive half of report_capture_failure(): an operator watching an eight-minute leg has to be able
/// to see that files are landing, and "no news" is indistinguishable from "the directory is missing".
void report_first_capture(const std::string& name)
{
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) {
    return;
  }
  std::fprintf(stderr,
               "[ul_capture] capturing: writing %s.* (budget %u reception(s) that pass the filters)\n",
               name.c_str(),
               capture_budget());
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

bool select(const pusch_processor::pdu_t& pdu)
{
  // The destination is checked ONCE, and before the budget is touched: a missing directory would otherwise
  // turn every selected reception into a file that is not written, silently and for the whole leg.
  static const bool directory_ok = []() {
    if (capture_directory_exists()) {
      return true;
    }
    report_capture_failure(capture_prefix().c_str(), "its directory does not exist");
    return false;
  }();
  if (!directory_ok) {
    return false;
  }
  // The width filters run FIRST and do not spend the budget: a reception they reject is not a candidate,
  // so a leg whose traffic is mostly wide still records the narrow hops it is looking for, and a leg
  // looking for wide hops still records them when the scheduler interleaves small grants.
  const unsigned max_rb = capture_max_rb();
  if ((max_rb != 0) && (pdu.freq_alloc.get_nof_rb() > max_rb)) {
    return false;
  }
  const unsigned min_rb = capture_min_rb();
  if ((min_rb != 0) && (pdu.freq_alloc.get_nof_rb() < min_rb)) {
    return false;
  }
  // Same for the redundancy version, and it has to be read from the codeword rather than from the PDU's
  // absence: `codeword` is checked by the caller, and an rv the caller did not ask for is a reception the
  // offline comparison cannot use.
  if (const std::optional<unsigned> rv = capture_rv(); rv.has_value()) {
    if (!pdu.codeword.has_value() || (pdu.codeword->rv != *rv)) {
      return false;
    }
  }
  static std::atomic<unsigned> count{0};
  std::lock_guard              lock(selected_mutex());
  if (count.fetch_add(1, std::memory_order_relaxed) >= capture_budget()) {
    return false;
  }
  selected().insert(make_key(pdu.slot, pdu.rnti));
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
  if (!enabled() || !pdu.codeword.has_value() || !select(pdu)) {
    return;
  }
  const std::string name = make_key(pdu.slot, pdu.rnti);
  report_first_capture(name);

  if (FILE* f = std::fopen((name + ".txt").c_str(), "w")) {
    std::fprintf(f, "slot=%u\n", pdu.slot.count());
    std::fprintf(f, "scs_khz=%u\n", scs_to_khz(pdu.slot.scs()));
    std::fprintf(f, "cp=%s\n", pdu.cp.to_string().c_str());
    std::fprintf(f, "rnti=%u\n", to_underlying(pdu.rnti));
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
  else {
    // The directory exists (checked in select()) but the file still could not be opened: a permission, a
    // full disk, a filename the filesystem refuses. Reported rather than swallowed, because the reception
    // has already spent the budget and the operator has no other way to find out.
    report_capture_failure((name + ".txt").c_str(), std::strerror(errno));
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

#else // !OCUDU_UL_CAPTURE

// Compiled out (ENABLE_UL_CAPTURE, off by default): a release build has no capture machinery at all -
// no buffers, no file I/O, no environment lookups - and the API stays so that no call site changes.
// The host touches a capture would cause therefore cannot happen, which is why the pipeline contract
// never has to reason about them (see phy_pipeline_crossings::scoped_debug_touches for the builds
// where the capture IS compiled in).

bool ocudu::ul_capture::enabled()
{
  return false;
}

bool ocudu::ul_capture::llr_enabled()
{
  return false;
}

void ocudu::ul_capture::capture_grid(const resource_grid_reader& grid, const pusch_processor::pdu_t& pdu) {}

void ocudu::ul_capture::capture_ce(const dmrs_pusch_estimator_results& est_results,
                                   const pusch_processor::pdu_t&      pdu)
{
}

void ocudu::ul_capture::set_current(slot_point slot, rnti_t rnti) {}

void ocudu::ul_capture::capture_h(const dmrs_pusch_estimator_results& est_results,
                                  const pusch_processor::pdu_t&      pdu)
{
}

void ocudu::ul_capture::capture_llr(span<const log_likelihood_ratio> llr) {}

#endif // OCUDU_UL_CAPTURE
