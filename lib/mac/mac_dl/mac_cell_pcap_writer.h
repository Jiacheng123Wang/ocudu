// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/adt/span.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/scheduler/config/si_scheduling_config.h"
#include <array>
#include <cstdint>

namespace ocudu {

class mac_pcap;
struct mac_dl_data_result;
struct sched_result;

/// Writer of the MAC PDUs transmitted in a cell to a PCAP file.
class mac_cell_pcap_writer
{
public:
  mac_cell_pcap_writer(mac_pcap& pcap, bool is_tdd, span<const si_message_scheduling_config> si_messages);

  /// Writes the DL MAC PDUs of a given slot.
  void write_slot_result(slot_point sl_tx, const sched_result& sl_res, const mac_dl_data_result& dl_res);

private:
  void write_si_pdus(slot_point sl_tx, const sched_result& sl_res, const mac_dl_data_result& dl_res);
  void write_rar_pdus(slot_point sl_tx, const sched_result& sl_res, const mac_dl_data_result& dl_res);
  void write_paging_pdus(slot_point sl_tx, const sched_result& sl_res, const mac_dl_data_result& dl_res);
  void write_ue_pdus(slot_point sl_tx, const sched_result& sl_res, const mac_dl_data_result& dl_res);

  mac_pcap& pcap;

  const uint8_t radio_type;

  // SI-message indexes whose version-based dedup must be bypassed. PWS (ETWS/CMAS) SI-messages cycle through multiple
  // content segments and repetitions without bumping "version" (which only tracks SI scheduling info updates), so
  // version-based dedup would wrongly suppress genuinely different payloads.
  bounded_bitset<MAX_SI_MESSAGES> pws_si_messages;

  // Last SIB1 and SI-message versions written to PCAP, used to dedup repeated broadcasts.
  unsigned                              sib1_dumped_version;
  std::array<unsigned, MAX_SI_MESSAGES> si_dumped_version;
};

} // namespace ocudu
