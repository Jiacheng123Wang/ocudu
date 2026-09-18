// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "extension_header/long_pdcp_pdu_number_packing.h"
#include "extension_header/pdcp_pdu_number_packing.h"
#include "gtpu_pdu.h"
#include "gtpu_tunnel_base_tx.h"
#include "ocudu/adt/byte_buffer.h"
#include "ocudu/gtpu/gtpu_config.h"
#include "ocudu/gtpu/gtpu_tunnel_pdcp_tx.h"
#include "ocudu/ran/cu_up_types.h"
#include <arpa/inet.h>
#include <netinet/in.h>

namespace ocudu {

/// Class used for transmitting GTP-U PDCP tunnels, e.g. on Xn-U interface.
class gtpu_tunnel_pdcp_tx_impl final : public gtpu_tunnel_base_tx, public gtpu_tunnel_pdcp_tx_lower_layer_interface
{
public:
  gtpu_tunnel_pdcp_tx_impl(cu_up_ue_index_t                                           ue_index,
                           const gtpu_tunnel_pdcp_config::gtpu_tunnel_pdcp_tx_config& cfg_,
                           dlt_pcap&                                                  gtpu_pcap_,
                           gtpu_tunnel_common_tx_upper_layer_notifier&                upper_dn_) :
    gtpu_tunnel_base_tx(gtpu_tunnel_log_prefix{cfg_.lif, ue_index, cfg_.peer_teid, "UL"}, gtpu_pcap_, upper_dn_),
    pdcp_pdu_number_packer(logger.get_basic_logger()),
    long_pdcp_pdu_number_packer(logger.get_basic_logger()),
    cfg(cfg_),
    cfg_pdcp_pdu_number_extension_type(cfg.pdcp_sn_len == pdcp_sn_size::size12bits
                                           ? gtpu_extension_header_type::pdcp_pdu_number
                                           : gtpu_extension_header_type::long_pdcp_pdu_number),
    current_peer_teid(cfg_.peer_teid)
  {
    to_sockaddr(peer_sockaddr, cfg.peer_addr.c_str(), cfg.peer_port);
    logger.log_info("GTPU PDCP TX configured. {}", cfg);
    ocudu_assert(
        cfg.lif == gtpu_logical_interface::xnu, "GTP-U PDCP TX node not correctly initialized. lif={}", cfg.lif);
    ocudu_assert(cfg.pdcp_sn_len == pdcp_sn_size::size12bits || cfg.pdcp_sn_len == pdcp_sn_size::size18bits,
                 "GTP-U PDCP TX node not correctly initialized. pdcp_sn_len={}",
                 cfg.pdcp_sn_len);
  }

  void stop() { stopped = true; }

  /*
   * SDU/PDU handlers
   */

  void handle_sdu(byte_buffer buf, uint32_t pdcp_pdu_number) override
  {
    if (stopped) {
      return;
    }

    gtpu_header hdr         = {};
    hdr.flags.version       = GTPU_FLAGS_VERSION_V1;
    hdr.flags.protocol_type = GTPU_FLAGS_GTP_PROTOCOL;
    hdr.flags.ext_hdr       = true;
    hdr.message_type        = GTPU_MSG_DATA_PDU;
    hdr.length              = buf.length() + 4 + 4;
    hdr.teid                = current_peer_teid;
    hdr.next_ext_hdr_type   = cfg_pdcp_pdu_number_extension_type;

    // Put PDCP PDU number.
    byte_buffer ext_buf;
    if (cfg_pdcp_pdu_number_extension_type == gtpu_extension_header_type::pdcp_pdu_number) {
      if (!pdcp_pdu_number_packer.pack(ext_buf, pdcp_pdu_number)) {
        logger.log_error("Dropped T-PDU, error writing PDCP PDU number to GTP-U extension header. teid={} ext_len={}",
                         hdr.teid,
                         ext_buf.length());
        return;
      }
    } else {
      if (!long_pdcp_pdu_number_packer.pack(ext_buf, pdcp_pdu_number)) {
        logger.log_error(
            "Dropped T-PDU, error writing long PDCP PDU number to GTP-U extension header. teid={} ext_len={}",
            hdr.teid,
            ext_buf.length());
        return;
      }
    }

    gtpu_extension_header ext;
    ext.extension_header_type = cfg_pdcp_pdu_number_extension_type;
    ext.container             = ext_buf;

    hdr.ext_list.push_back(ext);

    bool write_ok = gtpu_write_header(buf, hdr, logger);

    if (!write_ok) {
      logger.log_error("Dropped T-PDU, error writing GTP-U header. teid={}", hdr.teid);
      return;
    }
    logger.log_info(buf.begin(),
                    buf.end(),
                    "TX G-PDU. gpdu_len={} teid={} pdcp_pdu_num={}",
                    buf.length(),
                    hdr.teid,
                    pdcp_pdu_number);
    send_pdu(std::move(buf), peer_sockaddr);
  }

private:
  gtpu::pdcp_pdu_number_packing      pdcp_pdu_number_packer;
  gtpu::long_pdcp_pdu_number_packing long_pdcp_pdu_number_packer;

  const gtpu_tunnel_pdcp_config::gtpu_tunnel_pdcp_tx_config cfg;
  /// configured extension type used for PDCP PDU number, either pdcp_pdu_number or long_pdcp_pdu_number.
  const gtpu_extension_header_type cfg_pdcp_pdu_number_extension_type;

  gtpu_teid_t      current_peer_teid = {};
  sockaddr_storage peer_sockaddr     = {};
  bool             stopped           = false;
};
} // namespace ocudu
