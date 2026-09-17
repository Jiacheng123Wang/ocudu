// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "extension_header/pdcp_pdu_number_packing.h"
#include "gtpu_tunnel_base_rx.h"
#include "ocudu/gtpu/gtpu_config.h"
#include "ocudu/gtpu/gtpu_tunnel_pdcp_rx.h"
#include "ocudu/ran/cu_up_types.h"
#include "ocudu/support/sdu_window.h"
#include "ocudu/support/timers.h"

namespace ocudu {

/// GTP-U PDCP RX state variables
struct gtpu_pdcp_rx_state {
  /// RX_NEXT indicates the SN value of the next GTP-U SDU expected to be received.
  uint16_t rx_next;
  /// RX_DELIV indicates the SN value of the first GTP-U SDU not delivered to the lower layers, but still
  /// waited for.
  uint16_t rx_deliv;
  /// RX_REORD indicates the SN value following the SN value associated with the GTP-U PDU which
  /// triggered t-Reordering.
  uint16_t rx_reord;

  /// \brief gtpu_pdcp_rx_state Creates a GTP-U RX state initialized to a given value.
  /// \param init_sn Initial sequence number expected to be seen first.
  gtpu_pdcp_rx_state(uint16_t init_sn = 0) : rx_next(init_sn), rx_deliv(init_sn), rx_reord(init_sn) {}
};

struct gtpu_pdcp_rx_tpdu_info {
  /// GTP-U T-PDU encapsuling a PDCP SDU.
  byte_buffer tpdu = {};
  /// PDCP PDU number. Conveys a 12-bit PDCP SN or a 18-bit PDCP SN.
  uint32_t pdcp_pdu_number = {};
  /// GTP-U sequence number.
  std::optional<uint16_t> gtpu_sn = {};
};

/// Class used for receiving GTP-U PDCP tunnels, e.g. on Xn-U interface.
class gtpu_tunnel_pdcp_rx_impl : public gtpu_tunnel_base_rx
{
public:
  gtpu_tunnel_pdcp_rx_impl(cu_up_ue_index_t                                    ue_index,
                           gtpu_tunnel_pdcp_config::gtpu_tunnel_pdcp_rx_config cfg,
                           gtpu_tunnel_pdcp_rx_lower_layer_notifier&           rx_lower_,
                           timer_factory                                       ue_ctrl_timer_factory_) :
    gtpu_tunnel_base_rx(gtpu_tunnel_log_prefix{cfg.lif, ue_index, cfg.local_teid, "DL"}),
    pdcp_pdu_number_packer(logger.get_basic_logger()),
    lower_dn(rx_lower_),
    config(cfg),
    rx_window(logger, GTPU_RX_WINDOW_SIZE),
    ue_ctrl_timer_factory(ue_ctrl_timer_factory_)
  {
    if (config.t_reordering.count() != 0) {
      reordering_timer = ue_ctrl_timer_factory.create_timer();
      reordering_timer.set(config.t_reordering, reordering_callback{this});
    }
    logger.log_info("GTP-U PDCP RX configured. {}", config);
    ocudu_assert(
        cfg.lif == gtpu_logical_interface::xnu, "GTP-U PDCP RX node not correctly initialized. lif={}", cfg.lif);
  }
  ~gtpu_tunnel_pdcp_rx_impl() override = default;

  void stop()
  {
    if (not stopped) {
      reordering_timer.stop();
      stopped = true;
    }
  }

  /*
   * Testing Helpers
   */
  void                              set_state(std::optional<gtpu_pdcp_rx_state> rx_state_) { rx_state = rx_state_; }
  std::optional<gtpu_pdcp_rx_state> get_state() { return rx_state; }
  bool                              is_reordering_timer_running() { return reordering_timer.is_running(); }

protected:
  // domain-specific PDU handler
  void handle_pdu(gtpu_dissected_pdu&& pdu, const sockaddr_storage& src_addr) final
  {
    if (stopped) {
      return;
    }

    size_t      pdu_len              = pdu.buf.length();
    gtpu_teid_t teid                 = pdu.hdr.teid;
    uint32_t    pdcp_pdu_number      = 0;
    bool        have_pdcp_pdu_number = false;
    for (auto ext_hdr : pdu.hdr.ext_list) {
      switch (ext_hdr.extension_header_type) {
        case gtpu_extension_header_type::pdcp_pdu_number:
          if (!have_pdcp_pdu_number) {
            have_pdcp_pdu_number = pdcp_pdu_number_packer.unpack(pdcp_pdu_number, ext_hdr.container);
            if (!have_pdcp_pdu_number) {
              logger.log_error("Failed to unpack PDCP PDU number. pdu_len={}", pdu_len);
            }
          } else {
            logger.log_warning("Ignoring multiple PDCP PDU numbers. pdu_len={}", pdu_len);
          }
          break;
        default:
          logger.log_warning("Ignoring unexpected extension header at Xn-U interface. type={} pdu_len={}",
                             ext_hdr.extension_header_type,
                             pdu_len);
      }
    }
    if (!have_pdcp_pdu_number) {
      logger.log_warning(
          "Incomplete PDU at Xn-U interface: missing PDCP PDU number. pdu_len={} teid={}", pdu_len, teid);
      // TS 38.300 Sec. 9.2.3.2.3: The SN of forwarded PDCP SDUs is carried in the "PDCP PDU number"
      // field of the GTP-U extension header.
      return;
    }

    logger.log_debug(pdu.buf.begin(), pdu.buf.end(), "RX PDU. pdu_len={} rx_state=[{}]", pdu_len, rx_state);

    if (!pdu.hdr.flags.seq_number || config.t_reordering.count() == 0) {
      // Forward this SDU straight away.
      byte_buffer            rx_sdu      = gtpu_extract_msg(std::move(pdu)); // header is invalidated after extraction.
      gtpu_pdcp_rx_tpdu_info rx_sdu_info = {std::move(rx_sdu), pdcp_pdu_number, std::nullopt};
      deliver_sdu(rx_sdu_info);
      return;
    }

    uint16_t    gtpu_sn = pdu.hdr.seq_number;
    byte_buffer rx_sdu  = gtpu_extract_msg(std::move(pdu)); // header is invalidated after extraction.

    // Initialize rx_state if this is the first SN we received.
    if (!rx_state.has_value()) {
      if (gtpu_sn != 0) {
        if (!config.warn_on_drop) {
          logger.log_info("Initialized rx_state to non-zero value. gtpu_sn={}", gtpu_sn);
        } else {
          logger.log_warning("Initialized rx_state to non-zero value. gtpu_sn={}", gtpu_sn);
        }
      }
      rx_state = gtpu_pdcp_rx_state(gtpu_sn);
    }
    auto& st = *rx_state;

    // Check out-of-window
    if (!inside_rx_window(gtpu_sn, st)) {
      if (nof_log_sn_out_of_window++ < max_nof_log_sn_out_of_window) {
        logger.log_warning("GTP-U SN falls out of Rx window. gtpu_sn={} pdu_len={} {} reordering_timer_running={}",
                           gtpu_sn,
                           pdu_len,
                           st,
                           reordering_timer.is_running());
        if (nof_log_sn_out_of_window == max_nof_log_sn_out_of_window) {
          logger.log_warning("Throttling previous log message after {} contiguous repetitions",
                             nof_log_sn_out_of_window);
        }
      }
      gtpu_pdcp_rx_tpdu_info rx_sdu_info = {std::move(rx_sdu), pdcp_pdu_number, gtpu_sn};
      deliver_sdu(rx_sdu_info);
      return;
    }

    // Check late SN
    if (rx_mod_base(gtpu_sn, st) < rx_mod_base(st.rx_deliv, st)) {
      logger.log_debug("Out-of-order after timeout or duplicate. gtpu_sn={} pdu_len={} {}", gtpu_sn, pdu_len, st);
      gtpu_pdcp_rx_tpdu_info rx_sdu_info = {std::move(rx_sdu), pdcp_pdu_number, gtpu_sn};
      deliver_sdu(rx_sdu_info);
      return;
    }

    // Check if PDU has been received
    if (rx_window.has_sn(gtpu_sn)) {
      logger.log_warning("Duplicate PDU dropped. gtpu_sn={} pdu_len={}", gtpu_sn, pdu_len);
      return;
    }

    gtpu_pdcp_rx_tpdu_info& rx_sdu_info = rx_window.add_sn(gtpu_sn);
    rx_sdu_info.tpdu                    = std::move(rx_sdu);
    rx_sdu_info.pdcp_pdu_number         = pdcp_pdu_number;
    rx_sdu_info.gtpu_sn                 = gtpu_sn;

    // Update RX_NEXT
    if (rx_mod_base(gtpu_sn, st) >= rx_mod_base(st.rx_next, st)) {
      st.rx_next = gtpu_sn + 1;
    }

    if (rx_mod_base(gtpu_sn, st) == rx_mod_base(st.rx_deliv, st)) {
      // Deliver all consecutive SDUs in ascending order of associated SN
      deliver_all_consecutive_sdus();
    }

    // Stop re-ordering timer if we advanced the window past RX_REORD
    if (reordering_timer.is_running() and (not inside_rx_window(st.rx_reord, st))) {
      reordering_timer.stop();
      logger.log_debug("Stopped t-Reordering. {}", st);
    }

    if (config.t_reordering.count() == 0) {
      st.rx_reord = st.rx_next;
      handle_t_reordering_expire();
    } else if (!reordering_timer.is_running() and rx_mod_base(st.rx_deliv, st) < rx_mod_base(st.rx_next, st)) {
      st.rx_reord = st.rx_next;
      reordering_timer.run();
      logger.log_debug("Started t-Reordering. {}", st);
    }

    // Reset throttled logs
    nof_log_sn_out_of_window = 0;
  }

  void deliver_sdu(gtpu_pdcp_rx_tpdu_info& sdu_info)
  {
    logger.log_info(sdu_info.tpdu.begin(),
                    sdu_info.tpdu.end(),
                    "RX SDU. sdu_len={} pdcp_pdu_num={} gtpu_sn={}",
                    sdu_info.tpdu.length(),
                    sdu_info.pdcp_pdu_number,
                    sdu_info.gtpu_sn);
    lower_dn.on_new_sdu(std::move(sdu_info.tpdu), sdu_info.pdcp_pdu_number);
  }

  void deliver_all_consecutive_sdus()
  {
    if (!rx_state.has_value()) {
      logger.log_error("Invalid state to deliver consecutive SDUs. rx_state=[{}]", rx_state);
      return;
    }
    auto& st = *rx_state;

    while (st.rx_deliv != st.rx_next && rx_window.has_sn(st.rx_deliv)) {
      gtpu_pdcp_rx_tpdu_info& sdu_info = rx_window[st.rx_deliv];
      deliver_sdu(sdu_info);
      rx_window.remove_sn(st.rx_deliv);

      // Update RX_DELIV
      st.rx_deliv = st.rx_deliv + 1;
    }
  }

  void handle_t_reordering_expire()
  {
    if (!rx_state.has_value()) {
      logger.log_error("Invalid state to handle expired t_reordering. rx_state=[{}]", rx_state);
      return;
    }
    auto& st = *rx_state;

    // Check if timer has been restarted by the PDU handling routine between expiration and execution of this handler.
    if (reordering_timer.is_running()) {
      logger.log_info("reordering timer has been already restarted. Skipping outdated event. {}", st);
      return;
    }
    if (not inside_rx_window(st.rx_reord, st)) {
      logger.log_info("rx_reord is outside RX window. Skipping outdated event. {}", st);
      return;
    }

    while (st.rx_deliv != st.rx_reord) {
      if (rx_window.has_sn(st.rx_deliv)) {
        gtpu_pdcp_rx_tpdu_info& sdu_info = rx_window[st.rx_deliv];
        deliver_sdu(sdu_info);
        rx_window.remove_sn(st.rx_deliv);
      }

      // Update RX_DELIV
      st.rx_deliv = st.rx_deliv + 1;
    }

    deliver_all_consecutive_sdus();

    if (rx_mod_base(st.rx_deliv, st) < rx_mod_base(st.rx_next, st)) {
      if (config.t_reordering.count() == 0) {
        logger.log_error("reordering timer expired after 0ms and rx_deliv < rx_next. {}", st);
        return;
      }
      logger.log_debug("updating rx_reord to rx_next. {}", st);
      st.rx_reord = st.rx_next;
      reordering_timer.run();
    }
  }

private:
  pdcp_pdu_number_packing                   pdcp_pdu_number_packer;
  gtpu_tunnel_pdcp_rx_lower_layer_notifier& lower_dn;
  bool                                      stopped = false;

  /// Rx config
  gtpu_tunnel_pdcp_config::gtpu_tunnel_pdcp_rx_config config;

  /// Rx state
  ///
  /// The state is optional and is initialized upon first receptions of a sequence number
  std::optional<gtpu_pdcp_rx_state> rx_state;

  /// Rx window
  sdu_window<gtpu_pdcp_rx_tpdu_info, gtpu_tunnel_logger> rx_window;

  /// Rx reordering timer
  unique_timer reordering_timer;

  /// Timer factory
  timer_factory ue_ctrl_timer_factory;

  /// Reordering callback (t-Reordering)
  class reordering_callback
  {
  public:
    explicit reordering_callback(gtpu_tunnel_pdcp_rx_impl* parent_) : parent(parent_) {}
    void operator()()
    {
      if (not parent->config.warn_on_drop) {
        parent->logger.log_info("reordering timer expired after {}ms. rx_state=[{}]",
                                parent->config.t_reordering.count(),
                                parent->rx_state);
      } else {
        parent->logger.log_warning("reordering timer expired after {}ms. rx_state=[{}]",
                                   parent->config.t_reordering.count(),
                                   parent->rx_state);
      }
      parent->handle_t_reordering_expire();
    }

  private:
    gtpu_tunnel_pdcp_rx_impl* parent;
  };

  /// \brief Helper function for arithmetic comparisons of state variables or SN values.
  ///
  /// When performing arithmetic comparisons of state variables or SN values, a modulus base shall be used.
  /// This is adapted from RLC AM, TS 38.322 Sec. 7.1.
  ///
  /// \param sn The sequence number to be rebased from RX_Deliv, as this is the lower-edge of the window.
  /// \param st The state of the RX entity.
  /// \return The rebased value of sn.
  constexpr uint16_t rx_mod_base(uint16_t sn, const gtpu_pdcp_rx_state& st) const
  {
    return (sn - st.rx_deliv) % GTPU_SN_MOD;
  }

  /// Checks whether a sequence number is inside the current Rx window.
  ///
  /// \param sn The sequence number to be checked.
  /// \param st The state of the RX entity.
  /// \return True if sn is inside the Rx window, false otherwise.
  constexpr bool inside_rx_window(uint16_t sn, const gtpu_pdcp_rx_state& st) const
  {
    // RX_Deliv <= SN < RX_Deliv + Window_Size
    return rx_mod_base(sn, st) < GTPU_RX_WINDOW_SIZE;
  }

  // Log helper for throttling
  static constexpr unsigned max_nof_log_sn_out_of_window = 5;
  unsigned                  nof_log_sn_out_of_window     = 0;
};

} // namespace ocudu

namespace fmt {
template <>
struct formatter<ocudu::gtpu_pdcp_rx_state> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx)
  {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const ocudu::gtpu_pdcp_rx_state& st, FormatContext& ctx) const
  {
    return format_to(ctx.out(), "rx_deliv={} rx_reord={} rx_next={} ", st.rx_deliv, st.rx_reord, st.rx_next);
  }
};

} // namespace fmt
