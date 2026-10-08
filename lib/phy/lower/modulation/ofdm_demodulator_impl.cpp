// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ofdm_demodulator_impl.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ocuduvec/conversion.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ocuduvec/prod.h"
#include "ocudu/ocuduvec/sc_prod.h"
#include "ocudu/ocuduvec/zero.h"
#include "ocudu/phy/phy_pipeline_grid_ready.h"
#include "ocudu/phy/phy_pipeline_strict.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/ran/frame_types.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/support/error_handling.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ocudu;

namespace {

/// \brief Whether some stage of the receiving chain gathers the resource grid on the HOST.
///
/// The block handover (finish_symbol()) makes the slot's command buffer the HOP's own: the grid's producer
/// is inside it and nothing commits it before the hop ends. A stage that reads the grid on the HOST would
/// therefore read memory nobody has written yet - silently, because the host read is not ordered against a
/// buffer that is committed later - so the handover is refused while one of these arms is on.
///
/// This is the counterpart of the estimator's own `hold_extraction_for_weights()`: that predicate is the
/// same statement for the extraction's PRODUCTS (the host must not read them inside the hop either), and
/// the two share their reasons - a host least-squares pre-stage reads the received pilots out of the grid,
/// the LS comparison needs that same host build, and the CPU estimator is a host reader by definition.
/// None of them may be combined with the handover; each is an A/B arm an operator turns ON PURPOSE.
bool host_reads_the_grid()
{
  static const char* const host_route_arms[] = {
      "OCUDU_CE_CPU_CE",   // the whole channel estimation on the host: it gathers the grid
      "OCUDU_CE_CPU_LS",   // the host pre-stage gathers the received pilots out of the grid
      "OCUDU_CE_LS_CHECK", // needs the host's own least-squares build to compare the device's against
  };
  for (const char* arm : host_route_arms) {
    const char* env = std::getenv(arm);
    if ((env != nullptr) && (std::strtoul(env, nullptr, 10) != 0)) {
      return true;
    }
  }
  return false;
}

/// \brief Whether the slot's block may be handed over at all (the knob is a separate, later answer).
///
/// Both conditions are about the RUN, not about this slot: the deployment must have declared that the grid
/// is consumed on the device (the same declaration the front-end fence route needs, and the reason
/// `wait_per_slot` is what calls this), no host reader may be armed, and the run must claim that the device
/// serves every hop (phy_pipeline_strict_enabled()) - in mode=gpu a hop the device cannot serve FAILS the
/// grant instead of being covered by the host, so a device refusal cannot silently become a host read of a
/// grid nobody wrote.
/// \brief Whether the resource grid has consumers that read it on the HOST and cannot be ordered after the
///        hand-over's commit. FALSE since 5.9.20: the consumers wait, and both reasons that kept this true
///        have been withdrawn.
///
/// THE PUCCH IS ONE, and it is configured in every deployment that carries control information:
/// pucch_processor_impl reads the grid through resource_grid_reader and has NO device view at all, and the
/// upper PHY processes it as soon as the slot is handed over. A hand-over produces the grid at the LANE's
/// commit instead - a slot later - so a host reader would read memory nobody has written yet. Measured on
/// air when nothing waited: every PUCCH report came out `metric=nan sinr=-inf`, the attach never completed,
/// and 53135 real-time failures followed (design document 5.9.12).
///
/// That is answered by the WAIT, not by refusing the hand-over: the host consumers call
/// grid_ready_hook::wait() on their own executor, and the registry answers them by committing a block nobody
/// claimed (the fallback a hand-over owes) or by waiting for the generation of the block that was claimed.
/// The hand-over is refused only while a route that would read the grid on the host WITHOUT waiting is
/// armed (see host_reads_the_grid()), which in mode=gpu is not a configuration but an A/B arm.
bool grid_has_host_consumers()
{
  // FALSE since 5.9.20. The two reasons that kept it true are both withdrawn, and the paragraphs are kept
  // so the record of what was believed is not lost:
  //
  // (1) "the registry is keyed by the grid's STORAGE ADDRESS, so a consumer asking one slot late is served
  //      the NEXT slot's block" (5.9.15) - FIXED. The key is (storage, slot), and the case that used to
  //      break is the one the key refuses: a reader that asks about ITS OWN slot can no longer be handed
  //      another slot's block, and an unclaimed block is committed by whoever needs the grid first.
  //
  // (2) "the upper PHY's PUCCH/SRS tasks capture [this, &pdu] and read the slot-scoped member `grid` WHEN
  //      THEY RUN, so a task that has to WAIT for the production runs past its slot boundary and reads the
  //      NEXT slot's grid" (5.9.18) - WITHDRAWN, and the reason is in the FSM, not in the hand-over:
  //
  //        * uplink_processor_fsm::start_new_slot() only transitions out of IDLE, and the pending-PDU count
  //          is zero only when every PDU of the previous slot has called on_finish_processing_pdu() - which
  //          each task does at the END of its body, after its wait. So get_pdu_slot_repository() cannot
  //          configure a new slot on this processor (and pdu_repository.clear_queues() cannot run) while a
  //          task of the previous slot is still in flight: `&pdu` names storage that is alive by the FSM's
  //          own invariant, and the wait is INSIDE the task, so the invariant covers the wait too.
  //        * the grid is not slot-scoped either: each uplink_processor_impl owns ONE grid
  //          (uplink_processor_impl.cpp) and a cell has nof_ul_rg of them (upper_phy_factories.cpp), so a
  //          processor's grid is rewritten only when THAT processor is given another slot - which the FSM
  //          above forbids until the tasks are done. Measured window: the slot-to-slot reuse of one grid is
  //          ~nof_ul_rg UL requests (20 in this configuration, ~10 ms), not one slot. (This is the same
  //          misreading that 5.9.19 made and 5.9.20 withdrew.)
  //
  // What the armed leg DID find, and what the refusal was really hiding, is the object-identity defect of
  // 5.9.20: the estimator's DMRS extraction - the first device reader of the handed block - bound an
  // MTLBuffer object of its own while the front end wrote the grid through the process-wide one, and Metal
  // relates two accesses through the object, never through the address. That is fixed (wrap_grid()), and it
  // is what made the armed arm read `crc=KO` at 20-38 dB of sinr instead of failing structurally.
  //
  // The grid production itself is proven correct offline: the armed section of
  // ofdm_demodulator_metal_batch_test hands a slot over uncommitted, has the test act as the host consumer,
  // and the resulting grid is byte-identical to the host reference (0 of 17808 RE mismatching).
  return false;
}

bool handover_allowed()
{
  return !grid_has_host_consumers() && !host_reads_the_grid() && phy_pipeline_strict_enabled();
}

/// \brief Whether this run ARMED the hand-over knob (see grid_handover_armed(), the knob's owner).
///
/// The engine reads the same predicate for its own decision; this is the operator's half, read here so
/// that the two answers can be compared at startup without the lower PHY depending on a Metal header - a
/// build without Metal has no engine and therefore no hand-over at all. **The knob's default is ON** since
/// the s46 leg pair (design document 5.9.49), so this is now true on an ordinary run - which is exactly
/// what makes the warning below useful without anyone asking for it.
bool block_release_armed()
{
  return grid_handover_armed();
}

/// \brief Diagnostic arm: restore the historical "one host wait per slot" policy (OCUDU_DFT_WAIT_PER_SLOT=1).
///
/// WHY IT EXISTS (2026-09-28, design document 6.151⑨). `wait_per_slot` now also requires
/// handover_allowed(), because the per-slot wait only covers transforms that share the slot's single
/// command buffer. That is provably true of the zero-copy (radio-input) route - each symbol's submission
/// registers its own ring slot against the slot's open block (ocudu_dft_metal_engine.mm:2401) and
/// wait_slot() commits that block before it waits (:2157) - but NOT of the staged route, whose per-transform
/// buffers end every group (:2366) and whose thirteen earlier symbols were therefore never waited for.
/// The split/debug mode paid the per-symbol wait for both routes; this arm gives an operator the historical
/// policy back on the route where it is covered, so that two questions can be answered without a rebuild:
///  * whether the afternoons' wrap-arm storms (p100/p102/p108/p109) were the wait policy or the time of day
///    (the evening legs p112/p114/p115 are clean at the SAME link margin), and
///  * what the policy costs in the split mode (measured with it: the wrap arm's uplink hop went 993 -> 1393us
///    and its batching 14 -> 8 transforms per dispatch).
///
/// It is honoured ONLY while the transform input is NOT staged: the coverage argument above is the
/// radio-input route's, and a staged transform destroys exactly the batching it rests on. Asking for both
/// together cannot recreate the hole this arm exists to test, so the per-symbol wait is kept and the refusal
/// is reported once.
bool per_slot_wait_requested()
{
  const char* arm = std::getenv("OCUDU_DFT_WAIT_PER_SLOT");
  if ((arm == nullptr) || (std::strtoul(arm, nullptr, 10) == 0)) {
    return false;
  }
  const char* stage_env = std::getenv("OCUDU_DFT_STAGE_INPUT");
  // ... unless the AUDIT is watching: that combination is then the positive control the audit needs (it
  // recreates the staged route's hole, and the audit reports it as EARLY != LATE - see grid_audit()).
  const char* audit_env = std::getenv("OCUDU_DFT_GRID_AUDIT");
  const bool  audited   = (audit_env != nullptr) && (std::strtoul(audit_env, nullptr, 10) != 0);
  if (!audited && (stage_env != nullptr) && (std::strtoul(stage_env, nullptr, 10) != 0)) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: OCUDU_DFT_WAIT_PER_SLOT is ignored while OCUDU_DFT_STAGE_INPUT is on - the "
          "per-slot wait does not cover a staged transform (its own command buffer ends every batch), so "
          "the per-symbol wait is kept");
    }
    return false;
  }
  static bool reported_once = false;
  if (!reported_once) {
    reported_once = true;
    ocudulog::fetch_basic_logger("PHY").info(
        "OFDM demodulator: OCUDU_DFT_WAIT_PER_SLOT is on - this run waits once per slot (at its last "
        "symbol) as every leg did before 6.151⑨, which is a DIAGNOSTIC arm and not a delivery policy");
  }
  return true;
}

/// \brief Diagnostic arm OCUDU_DFT_GRID_AUDIT=<stride>: is the slot's grid COMPLETE when the host is told it is?
///
/// WHY IT EXISTS (2026-09-28, design document 6.151⑨⑭). The split mode's uplink collapses when the per-slot
/// wait is in force and comes back when every symbol is waited for - measured with the two arms
/// (OCUDU_DFT_WAIT_PER_SLOT): p118 storms (PRACH 2472, the phone's SR down to 125, PHR +9 -> +2, the downlink
/// queue 1 -> 7) while p115 is clean at the same link margin. The code reading says the per-slot wait SHOULD
/// cover the slot (each symbol registers its ring slot against the slot's one open block, and wait_slot()
/// commits that block before it waits), so the mechanism is still open - and a fix whose reason is unknown
/// can be undone by the next unrelated change.
///
/// WHAT IT MEASURES, and why that is decisive: after the end-of-slot wait returns, the grid is supposed to be
/// final. This arm snapshots it there (EARLY), drains every submission the engine has committed
/// (dft_processor::wait()), snapshots the SAME resource elements again (LATE) and compares.
///  * EARLY != LATE => the wait that just returned did NOT cover the whole slot's grid writes, and the host
///    readers (the CE on the host in this arm, and the PUCCH) run right after it. The per-symbol difference
///    counts are the fingerprint: they name which OFDM symbols were still being written.
///  * EARLY == LATE => the grid is complete when the slot is reported, and the hole is elsewhere (the
///    reader's generation, or something downstream of the grid).
///
/// A DIAGNOSTIC ARM that perturbs the pipeline on purpose (one full drain per audited slot flattens the
/// transform pipeline): it must not be flown as a delivery leg, and its own report line says how many slots
/// it audited. \c stride samples one slot out of \c stride (1 = every slot).
///
/// \note It also gates the otherwise-refused OCUDU_DFT_STAGE_INPUT + OCUDU_DFT_WAIT_PER_SLOT combination (see
///       per_slot_wait_requested()): with this arm watching, that combination is the POSITIVE CONTROL - it
///       recreates the staged route's hole, which the audit must then report as EARLY != LATE.
struct grid_audit_state {
  unsigned stride     = 0;
  bool     parsed     = false;
  uint64_t seen       = 0;
  uint64_t audited    = 0;
  uint64_t with_diff  = 0;
  uint64_t total_diff = 0;
  uint64_t max_diff   = 0;
  /// Differing resource elements per OFDM symbol of the slot: the fingerprint of an uncovered write.
  std::array<uint64_t, NOF_OFDM_SYM_PER_SLOT_NORMAL_CP> symbol_diff = {};
  /// EARLY snapshot, with the shape it was taken with.
  std::vector<uint32_t> early;
  unsigned              subc = 0;
  unsigned              symb = 0;
  unsigned              ports = 0;
};

grid_audit_state& grid_audit()
{
  static grid_audit_state state;
  if (!state.parsed) {
    state.parsed     = true;
    const char* env  = std::getenv("OCUDU_DFT_GRID_AUDIT");
    state.stride     = (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 0;
    static const bool registered = []() {
      std::atexit([]() {
        grid_audit_state& s = grid_audit();
        if (s.stride == 0) {
          return;
        }
        std::fprintf(stderr,
                     "[grid_audit] slots: audited=%llu of %llu seen, EARLY!=LATE in %llu, differing REs=%llu "
                     "(max %llu in one slot); per-symbol=[",
                     static_cast<unsigned long long>(s.audited),
                     static_cast<unsigned long long>(s.seen),
                     static_cast<unsigned long long>(s.with_diff),
                     static_cast<unsigned long long>(s.total_diff),
                     static_cast<unsigned long long>(s.max_diff));
        for (unsigned i = 0; i != s.symbol_diff.size(); ++i) {
          std::fprintf(stderr, "%s%llu", (i == 0) ? "" : ",", static_cast<unsigned long long>(s.symbol_diff[i]));
        }
        std::fprintf(stderr,
                     "] (stride=%u; EARLY = right after the slot's wait, LATE = after draining every "
                     "committed submission)\n",
                     s.stride);
      });
      return true;
    }();
    (void)registered;
  }
  return state;
}

/// Whether this slot is sampled by the audit arm (and counts it).
bool grid_audit_due()
{
  grid_audit_state& s = grid_audit();
  if (s.stride == 0) {
    return false;
  }
  bool due = (s.seen % s.stride) == 0;
  ++s.seen;
  return due;
}

/// Copies every resource element of \p view (all ports and symbols) as raw cbf16 words (4 bytes each).
///
/// The device view is the one grid layout the demodulator already knows (submit_grid_write() builds it), and
/// with unified memory its base is the grid's own buffer - so this is a plain host read of the storage the
/// kernel writes, at the moment the caller chooses (see grid_audit()).
void grid_audit_take(const resource_grid_device_view& view, std::vector<uint32_t>& into)
{
  grid_audit_state& s = grid_audit();
  s.subc              = view.nof_subc;
  s.symb              = view.nof_symb;
  s.ports             = view.nof_ports;
  into.assign(static_cast<size_t>(view.nof_ports) * view.nof_symb * view.nof_subc, 0);
  const auto* base = static_cast<const uint32_t*>(view.base);
  for (unsigned p = 0; p != view.nof_ports; ++p) {
    for (unsigned l = 0; l != view.nof_symb; ++l) {
      uint32_t* dst = into.data() + (static_cast<size_t>(p) * view.nof_symb + l) * view.nof_subc;
      std::memcpy(dst, base + view.get_symbol_offset(p, l), static_cast<size_t>(view.nof_subc) * sizeof(uint32_t));
    }
  }
}

/// Compares the grid against the EARLY snapshot and accumulates the per-symbol fingerprint (see grid_audit()).
void grid_audit_compare(const resource_grid_device_view& view)
{
  grid_audit_state& s = grid_audit();
  if ((s.subc != view.nof_subc) || (s.symb != view.nof_symb) || (s.ports != view.nof_ports) ||
      (s.early.size() != static_cast<size_t>(view.nof_ports) * view.nof_symb * view.nof_subc)) {
    // The shape changed under us: nothing to compare with, and the next audited slot re-takes the snapshot.
    return;
  }
  const auto* base     = static_cast<const uint32_t*>(view.base);
  uint64_t    slot_diff = 0;
  for (unsigned p = 0; p != view.nof_ports; ++p) {
    for (unsigned l = 0; l != view.nof_symb; ++l) {
      const uint32_t* src = base + view.get_symbol_offset(p, l);
      const uint32_t* ref = s.early.data() + (static_cast<size_t>(p) * view.nof_symb + l) * view.nof_subc;
      uint64_t        diff = 0;
      for (unsigned k = 0; k != view.nof_subc; ++k) {
        diff += (src[k] != ref[k]) ? 1u : 0u;
      }
      if (l < s.symbol_diff.size()) {
        s.symbol_diff[l] += diff;
      }
      slot_diff += diff;
    }
  }
  ++s.audited;
  s.total_diff += slot_diff;
  s.max_diff = std::max(s.max_diff, slot_diff);
  if (slot_diff != 0) {
    ++s.with_diff;
  }
}

} // namespace

ofdm_symbol_demodulator_impl::ofdm_symbol_demodulator_impl(const ofdm_demodulator_configuration& ofdm_config,
                                                           ofdm_demodulator_dependencies         dependencies) :
  dft_size(ofdm_config.dft_size),
  rg_size(ofdm_config.bw_rb * NOF_SUBCARRIERS_PER_RB),
  half_rg_size(rg_size / 2),
  cp(ofdm_config.cp),
  nof_samples_window_offset(ofdm_config.nof_samples_window_offset),
  scs(to_subcarrier_spacing(ofdm_config.numerology)),
  sampling_rate_Hz(to_sampling_rate_Hz(scs, dft_size)),
  scale(ofdm_config.scale),
  dft(std::move(dependencies.dft)),
  phase_compensation_table(to_subcarrier_spacing(ofdm_config.numerology),
                           ofdm_config.cp,
                           ofdm_config.dft_size,
                           ofdm_config.center_freq_Hz,
                           false),
  next_center_freq_Hz(ofdm_config.center_freq_Hz),
  current_center_freq_Hz(ofdm_config.center_freq_Hz)
{
  report_fatal_error_if_not(std::isnormal(scale), "Invalid scaling factor {}.", scale);
  report_fatal_error_if_not(
      dft_size > rg_size, "The DFT size ({}) must be greater than the resource grid size ({}).", dft_size, rg_size);

  // Fill DFT input with zeros.
  ocuduvec::zero(dft->get_input());

  if (ofdm_config.nof_samples_window_offset != 0) {
    // Verify the window is valid.
    ocudu_assert(ofdm_config.nof_samples_window_offset < (144 * ofdm_config.dft_size) / 2048,
                 "The DFT window offset (i.e., {}) must be lower than {}.",
                 ofdm_config.nof_samples_window_offset,
                 (144 * ofdm_config.dft_size) / 2048);

    // Prepare phase compensation vector.
    window_phase_compensation.resize(dft_size);

    // Discrete frequency of the complex exponential.
    float omega = static_cast<float>(ofdm_config.nof_samples_window_offset) * static_cast<float>(2.0 * M_PI) /
                  static_cast<float>(dft_size);
    for (unsigned i = 0; i != dft_size; ++i) {
      window_phase_compensation[i] = std::polar(1.0F, omega * static_cast<float>(i));
    }
  }

  // Device grid write: the engine that can do it writes both the transform and the grid in one command buffer, so the
  // transform output never travels back to the host. The table is constant, so it is published once.
  // Symbols per slot of this numerology: normal CP carries 14, extended 12. The wait policy in
  // finish_symbol() uses it to recognize the slot's last symbol (see there).
  nof_symbols_per_slot = (ofdm_config.cp == cyclic_prefix::NORMAL) ? NOF_OFDM_SYM_PER_SLOT_NORMAL_CP
                                                                  : NOF_OFDM_SYM_PER_SLOT_EXTENDED_CP;
  device_grid_write        = ofdm_config.device_grid_write;
  grid_consumed_on_device  = ofdm_config.grid_consumed_on_device;
  if (device_grid_write) {
    grid_write = dft->get_grid_write();
    if (grid_write == nullptr) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: the device grid write was requested but the DFT engine does not provide it; the grid will "
          "be written from the host");
      device_grid_write = false;
    } else if (!grid_write->set_grid_write_window(window_phase_compensation)) {
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: publishing the DFT window table failed; the grid will be written from the host");
      device_grid_write = false;
      grid_write         = nullptr;
    }
  }

  // The KNOB and the CHAIN'S PERMISSION are two different answers, and a leg that asks for the hand-over
  // while the chain refuses it exercises NOTHING: it reads exactly like the control arm, its counters come
  // out `handed=0 released=0`, and the mistake is only visible after the OTA cycle has been spent on it (it
  // was: one whole pair, s37). Said HERE, once per demodulator, before the radio starts - the log is the
  // only place an operator can still see it in time, and the predicates that refused are named.
  //
  // \note Since the knob's default moved to ON (5.9.49) this fires on an ordinary run, not only on one that
  //       set a variable - so a chain that cannot hand over now says so without being asked, which is the
  //       version of this warning that is actually wanted: the run MEANT to exercise D1.
  if (block_release_armed() && !handover_allowed()) {
    ocudulog::fetch_basic_logger("PHY").warning(
        "OFDM demodulator: the block hand-over is armed (OCUDU_DFT_RELEASE_BLOCK unset means armed since "
        "5.9.49) but this receiving chain refuses it (grid_has_host_consumers={}, host_reads_the_grid={}, "
        "strict={}) - this run will NOT exercise D1 (expect handed=0 released=0)",
        grid_has_host_consumers(),
        host_reads_the_grid(),
        phy_pipeline_strict_enabled());
  }
}

bool ofdm_symbol_demodulator_impl::submit_grid_write(resource_grid_writer& grid,
                                                     unsigned              port_index,
                                                     unsigned              symbol_index,
                                                     unsigned              slot,
                                                     span<const ci16_t>    time_input)
{
  if (!device_grid_write || (grid_write == nullptr)) {
    return false;
  }

  const resource_grid_device_view view = grid.get_device_view();
  if (!grid_write->supports_grid_write(view) || (view.nof_subc != rg_size)) {
    // The grid cannot be written from the device (its storage is not device-addressable, or it does not match this
    // demodulator). Report it once: falling back silently would hide a configuration mistake for the whole run.
    if (!device_grid_write_failed) {
      device_grid_write_failed = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: the resource grid cannot be written from the device (view valid={}, subcarriers={} vs "
          "{}); falling back to the host grid write",
          view.is_valid(),
          view.nof_subc,
          rg_size);
    }
    return false;
  }

  // The phase compensation table may have been rebuilt by fill_dft_input() (center frequency change), so the
  // coefficient is taken here, after the input was filled.
  dft_grid_write_params params;
  params.view         = view;
  params.port         = port_index;
  params.symbol       = symbol_index % get_nsymb_per_slot(cp);
  params.nof_subc     = rg_size;
  params.map_offset   = dft_size - rg_size / 2;
  params.coefficient  = phase_compensation_table.get_coefficient(symbol_index) * scale;
  params.apply_window = !window_phase_compensation.empty();
  if (!time_input.empty()) {
    // The transform reads the radio's int16 samples instead of the engine's float2 ring: the cyclic
    // prefix is skipped by the offset and the per-component scaling is the one the host would apply
    // (ocuduvec::convert with ocuduvec::scaling_factor_ci16_to_cf). The engine refuses the request,
    // and the caller stages the input, when the samples are not in a registered page-aligned
    // allocation or the symbol does not fit in it.
    params.time_samples      = time_input.data();
    params.time_samples_bytes = time_input.size() * sizeof(ci16_t);
    // The symbol's samples start at the cyclic prefix, and the transform reads from there minus the
    // DFT window offset - exactly the slice fill_dft_input() converts (see its subspan).
    params.time_window_start = cp.get_length(symbol_index, scs).to_samples(sampling_rate_Hz) - nof_samples_window_offset;
    params.time_gain          = 1.0F / ocuduvec::scaling_factor_ci16_to_cf;
  }
  return grid_write->submit_grid_write(slot, params);
}

unsigned ofdm_symbol_demodulator_impl::get_cp_offset(unsigned symbol_index, unsigned slot_index) const
{
  // Calculate number of symbols per slot.
  unsigned nsymb = get_nsymb_per_slot(cp);

  // Calculate the offset in samples to the start of the symbol CP within the current slot
  unsigned cp_offset = 0;
  for (unsigned symb_idx = 0; symb_idx != symbol_index; ++symb_idx) {
    cp_offset += cp.get_length(nsymb * slot_index + symb_idx, scs).to_samples(sampling_rate_Hz) + dft_size;
  }

  return cp_offset;
}

void ofdm_symbol_demodulator_impl::refresh_phase_compensation()
{
  // Recalculate phase compensation if the center frequency has changed.
  double center_freq_Hz = next_center_freq_Hz.load(std::memory_order::memory_order_relaxed);
  if (center_freq_Hz != current_center_freq_Hz) {
    phase_compensation_table = phase_compensation_lut(scs, cp, dft_size, center_freq_Hz, false);
    current_center_freq_Hz   = center_freq_Hz;
  }
}

void ofdm_symbol_demodulator_impl::fill_dft_input(span<cf_t>         dft_input,
                                                  span<const ci16_t>  input,
                                                  unsigned            symbol_index)
{
  refresh_phase_compensation();

  // Calculate cyclic prefix length.
  unsigned cp_len = cp.get_length(symbol_index, scs).to_samples(sampling_rate_Hz);

  // Make sure output buffer matches the symbol size.
  ocudu_assert(input.size() == (cp_len + dft_size),
               "The input buffer size ({}) does not match the symbol index {} size ({}+{}={}). SCS={}kHz.",
               input.size(),
               symbol_index,
               cp_len,
               cp_len + dft_size,
               scs_to_khz(scs));

  // Get phase correction (TS138.211, Section 5.4).
  cf_t phase_compensation = phase_compensation_table.get_coefficient(symbol_index);

  // Prepare the DFT inputs, while skipping the cyclic prefix. Include the conversion from ci16 to cf.
  ocuduvec::sc_prod(dft_input,
                    input.subspan(cp_len - nof_samples_window_offset, dft_size),
                    phase_compensation * scale / ocuduvec::scaling_factor_ci16_to_cf);
}

void ofdm_symbol_demodulator_impl::process_dft_output(resource_grid_writer& grid,
                                                      span<const cf_t>      dft_output,
                                                      unsigned              port_index,
                                                      unsigned              symbol_index)
{
  // Calculate number of symbols per slot.
  unsigned nsymb = get_nsymb_per_slot(cp);

  // Extract view of the destination frequency domain for the corresponding OFDM symbol.
  span<cbf16_t> symbol_view = grid.get_view(port_index, symbol_index % nsymb);

  // Compensate DFT window offset phase shift.
  if (!window_phase_compensation.empty()) {
    span<const cf_t> phase_shift(window_phase_compensation);

    // DFT window shift compensation and map the upper bound frequency domain data.
    ocuduvec::prod(symbol_view.first(half_rg_size), phase_shift.last(half_rg_size), dft_output.last(half_rg_size));

    // DFT window shift compensation and map the lower bound frequency domain data.
    ocuduvec::prod(symbol_view.last(half_rg_size), phase_shift.first(half_rg_size), dft_output.first(half_rg_size));
  } else {
    // Map the upper bound frequency domain data.
    ocuduvec::convert(symbol_view.first(half_rg_size), dft_output.last(half_rg_size));

    // Map the lower bound frequency domain data.
    ocuduvec::convert(symbol_view.last(half_rg_size), dft_output.first(half_rg_size));
  }
}

void ofdm_symbol_demodulator_impl::demodulate(resource_grid_writer& grid,
                                              span<const ci16_t>    input,
                                              unsigned              port_index,
                                              unsigned              symbol_index)
{
  // Fill the DFT input, execute one transform and post-process its output.
  fill_dft_input(dft->get_input().first(dft_size), input, symbol_index);
  span<const cf_t> dft_output = dft->run();
  process_dft_output(grid, dft_output, port_index, symbol_index);
}

// NOTE: the gNB RX path (puxch_processor_impl) calls demodulate() once per OFDM symbol because the
// radio paces the processing symbol by symbol, so the batched path below is dormant there. It pays
// off only for *independent* transform streams: the Rx ports of one symbol (available together, so
// the batch adds no latency) or the same symbol across carriers/sectors. Multiple PUSCH
// allocations or UEs of one cell share one per-symbol transform and need no batching.
// TODO(multi-port/multi-carrier): batch the ports of one symbol first (no added latency), then the
// same symbol of several carriers once multiple cells are driven from one place.
unsigned ofdm_symbol_demodulator_impl::get_pipeline_depth() const
{
  // Debug probe (documented in the plan): OCUDU_DFT_PIPELINE_DEPTH overrides the depth so a
  // suspicious RX regression can be bisected between the pipelined path (depth > 1) and the
  // original synchronous per-symbol path (depth = 1) without rebuilding.
  static const unsigned debug_depth = []() {
    const char* env = std::getenv("OCUDU_DFT_PIPELINE_DEPTH");
    return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : max_pipeline_depth;
  }();

  // Only as deep as the DFT ring and the configured bound; 1 keeps the synchronous per-symbol path.
  return std::min({dft->get_max_batch(), max_pipeline_depth, std::max(1U, debug_depth)});
}

void ofdm_symbol_demodulator_impl::submit_symbol(resource_grid_writer& grid,
                                                 span<const ci16_t>    input,
                                                 unsigned              port_index,
                                                 unsigned              symbol_index,
                                                 unsigned              slot)
{
  ocudu_assert(slot < max_pipeline_depth, "Invalid pipeline slot {}.", slot);

  // The transform input, and the grid write that rides the same dispatch: the engine reads the
  // radio's int16 samples straight out of the buffer the upper layers filled (see
  // dft_grid_write_params::time_samples), so NOTHING on the host touches the samples - the cyclic
  // prefix is an offset and the int16 -> float scaling is one multiply in the kernel. When the
  // engine refuses (the samples are not in a page-aligned allocation, or the symbol does not fit),
  // the input is staged on the host as before, once per run: refresh_phase_compensation() keeps the
  // per-symbol coefficient below correct whether or not fill_dft_input() ran.
  refresh_phase_compensation();
  bool device_write = false;
  if (!time_input_failed && device_grid_write && (grid_write != nullptr)) {
    device_write = submit_grid_write(grid, port_index, symbol_index, slot, input);
    if (!device_write) {
      time_input_failed = true;
      ocudulog::fetch_basic_logger("PHY").warning(
          "OFDM demodulator: the transform input cannot be read from the radio buffer; the samples are staged on the "
          "host for this run");
    }
  }

  // The device grid write (when available) has to be encoded together with the transform, so it happens here and not in
  // finish_symbol(): the transform output never has to reach the host.
  if (!device_write) {
    fill_dft_input(dft->get_input().subspan(static_cast<size_t>(slot) * dft_size, dft_size), input, symbol_index);
    device_write = submit_grid_write(grid, port_index, symbol_index, slot);
  }
  if (!device_write) {
    dft->run_async(slot);
  }
  pipeline_slots[slot] = {
      .port_index = port_index, .symbol_index = symbol_index, .valid = true, .device_write = device_write};
}

void ofdm_symbol_demodulator_impl::finish_symbol(resource_grid_writer& grid, unsigned slot)
{
  ocudu_assert(slot < max_pipeline_depth, "Invalid pipeline slot {}.", slot);
  ocudu_assert(pipeline_slots[slot].valid, "Pipeline slot {} holds no symbol.", slot);

  // The upper PHY reads the grid as soon as the symbol is reported, and with the device write that
  // reads memory the GPU produced - but it reads it on the DEVICE here: the estimator extracts the
  // pilots with resource_grid_reader::get_device_view() and the equalizer gathers the received symbols
  // with set_device_grid(). With the front-end fence on, that ordering is established by the shared
  // event instead, so the host does not have to wait for each
  // symbol - which is exactly the per-symbol synchronization the fusion removes (design document,
  // 48.189).
  //
  // The wait is KEPT whenever the grid can also be read on the host: device_grid_write false means the
  // grid is not device-resident at all, and host_grid_readers_enabled() lists the routes whose readers
  // touch it on the host. Keeping it costs the optimization and is always correct; skipping it where a
  // host reader exists would read memory the GPU has not written yet.
  // ... and only when the deployment DECLARED that the grid's consumers read it on the device: writing it
  // there (device_grid_write) says nothing about who reads it, and a host reader - the demodulator's own
  // test verifies the grid on the host - would read memory the GPU has not written yet.
  const bool last_symbol_of_slot =
      ((pipeline_slots[slot].symbol_index % nof_symbols_per_slot) == (nof_symbols_per_slot - 1));
  // Whether the grid can be read before the slot is complete. Nobody in the receiving chain does - the
  // upper PHY processes a slot after its last symbol has been reported - but a configuration has to SAY
  // so: that is exactly what grid_consumed_on_device declares (the same declaration the front-end fence
  // needs), and the demodulator's own test, which verifies the grid symbol by symbol on the host, does
  // not declare it and therefore keeps the historical wait. With the declaration, the wait belongs to
  // the slot's LAST symbol: fourteen host waits for one consumer become one. Every earlier symbol is
  // complete by then anyway - one queue completes its command buffers in submission order.
  // The slot's transforms were encoded into one command buffer (see set_lane_slot()): its last symbol is
  // where the block ends, so the command buffer is closed here - before the wait below, which would
  // otherwise find nothing to wait for.
  // ★★ AND ONLY WHEN THE HAND-OVER IS THERE TO ORDER THOSE READERS (2026-09-28, 6.151⑨).
  // The declaration above is about the DEPLOYMENT (config.device_resource_grid), but the wait this line
  // controls is what an ORDERING needs: "one queue completes its command buffers in submission order" only
  // covers a symbol whose transforms went into the slot's single command buffer (the block), and a host
  // reader that runs right after this call is ordered by nothing else. Measured with the counters the
  // engine already reports: the zero-copy route DOES put all fourteen symbols of a slot into one dispatch
  // (`batched=37308/522312` and `46341/648774` on p100/p109 - 14 transforms each), but the staged route
  // cannot (each transform carries its own fresh buffer, so it gets a command buffer of its own:
  // `batched=0/0` on p105/p111, "0 joined an open block") - and with the hand-over refused, which is what
  // `cpu_gpu` and every host-reader arm do, the per-slot wait then leaves thirteen symbols' grid writes
  // uncovered while the CE (on the host in that arm) and the PUCCH (a host reader by construction) read
  // that grid. `handover_allowed()` is exactly "a reader is ordered without this wait": either the adopted
  // block carries the ordering, or the block's own completion is waited for at the slot's last symbol.
  // Requiring it is therefore the conservative form - it never skips a wait a reader might need - and it
  // costs the optimization only in the split/debug mode, never in the delivery lane.
  // \note Residual, recorded so it is not rediscovered the hard way: mode=gpu with
  //       OCUDU_DFT_STAGE_INPUT=1 would combine an allowed hand-over with unbatched transforms. That arm is
  //       not flown (the staging arm belongs to the split mode, where this predicate now waits per symbol);
  //       if it ever is, its per-slot wait will not cover the staged symbols.
  const bool wait_per_slot = pipeline_slots[slot].device_write && grid_consumed_on_device &&
                             (handover_allowed() || per_slot_wait_requested());
  // D1 step 2: instead of committing the slot's block, HAND IT OVER to the hop that will read this grid.
  // The upper PHY's first back-end stage adopts it (same command buffer, its own encoder), so the slot's
  // transforms, the channel estimation, the equalization and the demapping are ONE submission - which is
  // what takes the hop from 2.83 CPU submissions to 1.00 (design document 5.9.4), and what makes the
  // grid's producer the buffer's own first dispatch: no front-end fence is needed for it, and the host
  // wait below is not either.
  //
  // ARMED ONLY WHERE A CONSUMER IS GUARANTEED. The handover makes the slot's block the HOP's command
  // buffer: the grid's producer is inside it and nothing commits it before the hop's own end. Three ways
  // that can go wrong, all SILENT, and all therefore refused here instead of assumed away:
  //
  //  * a stage that GATHERS the grid on the HOST would read memory nobody has written yet - the block is
  //    committed later and a host read is not ordered against it at all. Those are the estimator's own
  //    host-route arms (the counterpart of hold_extraction_for_weights()), and they are listed below;
  //  * a hop the DEVICE cannot serve would be covered by the host - which is the same read. In mode=gpu
  //    that hop fails the grant instead of being covered (phy_pipeline_strict_enabled()), so requiring the
  //    claim is what keeps a device refusal loud rather than a wrong grid. Without the claim (cpu_gpu, a
  //    replay that publishes no mode) the handover stays off, and OCUDU_GPU_STRICT=1 is the override the
  //    offline harnesses use;
  //  * **the SAMPLES the transforms read would be recycled before they run** - the one that actually broke
  //    the uplink when this call site first landed (crc=KO 942 / OK 46 at sinr=37.6 dB, 5.9.7). `finish_symbol()`
  //    returning is what tells the radio a symbol's buffer may be reused. That is no longer the caller's
  //    risk to remember: while the release path is armed the demodulator reports
  //    defers_transform_execution(), and the receiving chain hands every transform's samples over
  //    (puxch_processor_impl::retain_symbol_input()) so the block keeps them alive until it completes.
  //
  // The answer is taken, not assumed: a processor whose release path is off (or that has no block open)
  // answers false and the block is committed exactly as before - one call site, one decision.
  bool released = false;
  if (block_open && last_symbol_of_slot) {
    released = wait_per_slot && handover_allowed() && dft->release_block(grid.get_device_view().base);
    if (!released) {
      (void)dft->end_block();
    }
    block_open = false;
  }

  if (released) {
    // The transforms of this slot are not this engine's submission any more: the adopter commits them, and
    // waiting here would name a buffer this engine does not own (wait_slot() says so and refuses). The
    // ordering the wait used to provide is inside the adopted buffer now.
    static bool reported_release = false;
    if (!reported_release) {
      reported_release = true;
      ocudulog::fetch_basic_logger("PHY").info(
          "OFDM demodulator: the slot's transforms are handed over to the fused lane instead of being "
          "committed and waited for (D1 step 2) - the hop commits them once, with its own stages");
    }
  } else if (!wait_per_slot || last_symbol_of_slot) {
    dft->wait_slot(slot);
  } else {
    // The host wait for this symbol is gone: the back end is ordered by the fence event instead. Said
    // once, because it is the property the whole configuration now depends on - a host consumer that
    // reads the grid without being listed in host_grid_readers_enabled() would read memory the GPU has
    // not written yet, and the leg would only see it as wrong LLRs. The counters that prove it are the
    // ones the contract prints (ce device estimates, equalizer ch_re): both must report host=0.
    static bool reported = false;
    if (!reported) {
      reported = true;
      ocudulog::fetch_basic_logger("PHY").info(
          "OFDM demodulator: the resource grid is waited for once per slot (last symbol) - the earlier "
          "symbols are not waited for, since nothing reads the grid before the slot is complete");
    }
  }

  if (!pipeline_slots[slot].device_write) {
    span<const cf_t> dft_output = dft->get_output_batch().subspan(static_cast<size_t>(slot) * dft_size, dft_size);
    process_dft_output(grid, dft_output, pipeline_slots[slot].port_index, pipeline_slots[slot].symbol_index);
  }
  // ★ OCUDU_DFT_GRID_AUDIT (diagnostic arm, see grid_audit()): the wait above just returned, so the grid is
  // supposed to be final - snapshot it, drain every committed submission, and compare. A difference is the
  // direct evidence that the wait did not cover the slot, with the per-symbol counts as the fingerprint.
  if (pipeline_slots[slot].device_write && last_symbol_of_slot && grid_audit_due()) {
    const resource_grid_device_view audit_view = grid.get_device_view();
    if (audit_view.is_valid()) {
      grid_audit_take(audit_view, grid_audit().early);
      dft->wait();
      grid_audit_compare(audit_view);
    }
  }

  pipeline_slots[slot].valid = false;
}

void ofdm_symbol_demodulator_impl::demodulate_batch(resource_grid_writer& grid,
                                                    span<const ci16_t>    input,
                                                    unsigned              port_index,
                                                    unsigned              first_symbol_index,
                                                    unsigned              nof_symbols)
{
  // Batched execution requires a DFT processor able to run several transforms in one dispatch.
  if ((nof_symbols <= 1) || (dft->get_max_batch() < nof_symbols)) {
    ofdm_symbol_demodulator::demodulate_batch(grid, input, port_index, first_symbol_index, nof_symbols);
    return;
  }

  // 1) Fill the input of every transform of the batch.
  span<cf_t>         batch_input = dft->get_input().first(static_cast<size_t>(nof_symbols) * dft_size);
  span<const ci16_t> remaining   = input;
  for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
    unsigned symbol_index = first_symbol_index + i_symbol;
    unsigned symbol_size  = get_symbol_size(symbol_index);
    fill_dft_input(batch_input.subspan(static_cast<size_t>(i_symbol) * dft_size, dft_size),
                   remaining.first(symbol_size),
                   symbol_index);
    remaining = remaining.last(remaining.size() - symbol_size);
  }

  // 2) Execute all transforms with a single dispatch.
  span<const cf_t> batch_output = dft->run_batch(nof_symbols);

  // 3) Post-process each transform output.
  for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
    process_dft_output(grid,
                       batch_output.subspan(static_cast<size_t>(i_symbol) * dft_size, dft_size),
                       port_index,
                       first_symbol_index + i_symbol);
  }
}

unsigned ofdm_slot_demodulator_impl::get_slot_size(unsigned slot_index) const
{
  unsigned nsymb = get_nsymb_per_slot(cp);
  unsigned count = 0;

  // Iterate all symbols of the slot and accumulate
  for (unsigned symbol_idx = 0; symbol_idx != nsymb; ++symbol_idx) {
    count += symbol_demodulator->get_symbol_size(nsymb * slot_index + symbol_idx);
  }

  return count;
}

void ofdm_slot_demodulator_impl::demodulate(resource_grid_writer& grid,
                                            span<const ci16_t>    input,
                                            unsigned              port_index,
                                            unsigned              slot_index)
{
  unsigned nsymb = get_nsymb_per_slot(cp);

  // Demodulate the whole slot: implementations whose DFT supports batching execute all the
  // symbol transforms in a single dispatch.
  symbol_demodulator->demodulate_batch(grid, input, port_index, nsymb * slot_index, nsymb);
}
