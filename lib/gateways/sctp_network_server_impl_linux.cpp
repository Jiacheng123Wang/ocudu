// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \brief Linux-only parts of the SCTP server: per-association socket handling.
///
/// On Linux each association gets a peeled-off fd subscribed to the broker, so
/// the receive path is per-association. On macOS the single one-to-many socket
/// delivers every association through the parent-socket receive() path, so
/// these members are declared but never referenced there.

#include "sctp_network_server_impl.h"
#include "ocudu/gateways/sctp_socket.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <array>
#include <netinet/sctp.h>

using namespace ocudu;

void sctp_network_server_impl::sctp_associaton_context::receive()
{
  struct sctp_sndrcvinfo                            sri       = {};
  int                                               msg_flags = 0;
  std::array<uint8_t, network_gateway_sctp_max_len> temp_recv_buffer;

  // fromlen is an in/out variable in sctp_recvmsg.
  sockaddr_storage msg_src_addr;
  socklen_t        msg_src_addrlen = sizeof(msg_src_addr);

  int rx_bytes = ::sctp_recvmsg(fd,
                                temp_recv_buffer.data(),
                                temp_recv_buffer.size(),
                                (struct sockaddr*)&msg_src_addr,
                                &msg_src_addrlen,
                                &sri,
                                &msg_flags);

  if (rx_bytes == -1) {
    if (errno != EAGAIN) {
      parent.logger.error("Error reading from SCTP socket: {}", ::strerror(errno));
      while (not parent.app_exec.defer([this, keepalive = parent.keepalive_token]() {
        if (*keepalive) {
          parent.handle_sctp_comm_lost(assoc_id);
        }
      })) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } else {
      if (!parent.node_cfg.non_blocking_mode) {
        parent.logger.debug("Socket timeout reached");
      }
    }
    return;
  }

  /// We pass the actual data and association handling back to the parent, to avoid code duplication.
  auto payload = std::vector<uint8_t>(temp_recv_buffer.begin(), temp_recv_buffer.begin() + rx_bytes);
  parent.receive_impl(std::move(payload), sri, msg_flags, msg_src_addr, msg_src_addrlen);
}

bool sctp_network_server_impl::subscribe_association_to_broker(unique_fd assoc_fd, sctp_associaton_context& assoc_ctxt)
{
  assoc_ctxt.io_sub = broker.register_fd(
      std::move(assoc_fd),
      io_rx_executor,
      [&assoc_ctxt]() { assoc_ctxt.receive(); },
      [this](io_broker::error_code code) {
        logger.info("Connection loss due to IO error code={}.", (int)code);
        defer_socket_shutdown(nullptr);
      });
  return assoc_ctxt.io_sub.registered();
}
