// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

/// \brief Private platform backend of the SCTP gateway family.
///
/// One implementation per platform is compiled (see lib/gateways/CMakeLists.txt):
/// - sctp_socket_linux.cpp: the Linux kernel SCTP stack (the upstream implementation);
/// - sctp_socket_usrsctp.cpp: the usrsctp user-space stack on macOS.
///
/// The shared gateway/socket code only calls these entry points, so the
/// platform selection never appears in the business logic.

#include "ocudu/adt/expected.h"
#include "ocudu/gateways/sctp_socket.h"
#include "ocudu/gateways/sctp_types.h"
#include "ocudu/support/io/unique_fd.h"
#include <netdb.h>
#include <sys/socket.h>
#include <vector>

namespace ocudu {

namespace sctp_backend {

/// Maximum SCTP message length of the gateway receive buffers (mirrors the protected
/// sctp_network_gateway_common_impl::network_gateway_sctp_max_len constant).
constexpr uint32_t max_sctp_msg_len = 9100;

// Socket lifecycle.

/// Creates the platform SCTP socket and returns the fd the shared code works with.
/// On macOS this is the read end of the bridge socketpair that wakes the io_broker.
expected<unique_fd> create_socket(const sctp_socket_params& params,
                                  const std::string&        if_name,
                                  ocudulog::basic_logger&   logger);

/// Prepares the fd for closing (macOS: graceful association shutdown + shim bookkeeping cleanup).
/// The shared close() closes the fd itself afterwards.
void prepare_close(const unique_fd& sock_fd, const std::string& if_name, ocudulog::basic_logger& logger);

// Socket options.

/// SCTP get/setsockopt routed to the platform stack (kernel fd on Linux, usrsctp socket on macOS).
int getsockopt_shim(const unique_fd& fd, int level, int option_name, void* optval, socklen_t* optlen);
int setsockopt_shim(const unique_fd& fd, int level, int option_name, const void* optval, socklen_t optlen);

/// Enables delivery of per-message info (association id, stream, ...) with every received message.
/// Linux: subscribes to SCTP_DATA_IO_EVENT. macOS: enables SCTP_RECVRCVINFO on the usrsctp socket.
bool enable_per_message_info(const unique_fd& fd);

/// Applies platform validation to the RTO options before setting them.
/// macOS: usrsctp requires rto_min <= rto_initial <= rto_max, so the triple is clamped (with warnings).
/// Linux: no-op.
void adjust_rto_opts(sctp_rtoinfo& rto_opts, const std::string& if_name, ocudulog::basic_logger& logger);

/// Reads the current SCTP_INITMSG options. macOS routes the get through the usrsctp socket with the
/// IPPROTO_SCTP level; Linux reads the kernel socket with SOL_SCTP (upstream behaviour).
bool get_init_msg_opts(const unique_fd&        fd,
                       sctp_initmsg&           init_opts,
                       socklen_t&              init_sz,
                       const std::string&      if_name,
                       ocudulog::basic_logger& logger);

/// Disables IPV6_V6ONLY so IPv6 sockets accept IPv4-mapped addresses.
/// macOS: applies to the usrsctp socket on a best-effort basis (the bridge socketpair must not be touched).
/// Linux: applies to the kernel fd and fails hard on error (upstream behaviour).
bool set_ipv6_v6only(const unique_fd& fd, int ai_family, const std::string& if_name, ocudulog::basic_logger& logger);

/// Applies the configured receive timeout. Linux: SO_RCVTIMEO on the kernel fd. macOS: logs that the timeout is
/// emulated by the shim's sctp_recvmsg() (usrsctp ignores SO_RCVTIMEO).
bool apply_receive_timeout(const unique_fd&        fd,
                           const sctp_socket_params& params,
                           const std::string&      if_name,
                           ocudulog::basic_logger& logger);

/// Applies SO_REUSEADDR. Linux: on the kernel fd. macOS: skipped (usrsctp manages port binding in user space).
bool apply_reuse_addr(const unique_fd& fd, const sctp_socket_params& params, ocudulog::basic_logger& logger);

// Socket state transitions.

int  listen_socket(const unique_fd& fd);
bool set_non_blocking(const unique_fd& fd, ocudulog::basic_logger& logger);
std::optional<uint16_t> get_bound_port(const unique_fd& fd, const std::string& if_name, ocudulog::basic_logger& logger);
std::optional<int>      get_address_family(const unique_fd& fd, const std::string& if_name, ocudulog::basic_logger& logger);

// Gateway receive path.

/// \brief Returns the getaddrinfo() hints used to resolve SCTP peer/bind addresses.
///
/// Linux: SOCK_SEQPACKET + IPPROTO_SCTP (kernel stack). macOS: the protocol hint is left unset - macOS has no
/// native SCTP support and getaddrinfo() rejects IPPROTO_SCTP with EAI_BADFLAGS; the resolved sockaddr is only
/// used to configure the usrsctp stack.
struct addrinfo make_sctp_addrinfo_hints();

/// Sink invoked by receive_available() for every message read from the socket.
using message_sink = void (*)(void*                              user,
                              std::vector<uint8_t>               payload,
                              const struct sctp_sndrcvinfo&      sri,
                              int                                msg_flags,
                              const sockaddr_storage&            src_addr,
                              socklen_t                          src_addrlen);

/// Reads the messages pending on the socket and invokes \p sink for each of them.
///
/// Linux: reads exactly one message per wake-up (the peeled-off association fds deliver one event per
/// notification, upstream behaviour). macOS: reads one message, then drains every additional message the
/// usrsctp stack queued behind the single bridge wake-up byte (read until EAGAIN).
///
/// \return Number of messages delivered (>= 0), or -1 with errno set (EAGAIN = timeout, anything else = fatal).
int receive_available(int fd, message_sink sink, void* user);

/// Single non-blocking read used by the drain loops inside receive_available().
int recv_message_nowait(int                    fd,
                        void*                 msg,
                        size_t                len,
                        struct sockaddr*      from,
                        socklen_t*            fromlen,
                        struct sctp_sndrcvinfo* sri,
                        socklen_t*            sri_len,
                        int*                  msg_flags);

/// Sends a zero-length message with the SCTP_EOF flag to \p dest_addr.
/// Linux: sendmsg with an SCTP_SNDINFO control message. macOS: sctp_sendmsg with SCTP_EOF through the shim.
/// \return Bytes sent (-1 on error, errno set).
int send_eof(int                       fd,
             const struct sockaddr*    dest_addr,
             socklen_t                 dest_addrlen,
             uint32_t                  ppid /* network byte order */,
             unsigned                  stream_no);

// Association handling (server).

/// Result of peeling off a per-association socket.
struct peel_off_result {
  /// Whether the platform implements per-association socket peeling.
  bool      supported;
  /// The peeled-off fd (open only when \c supported and the peel-off succeeded).
  unique_fd fd;
};

/// Peels off a per-association socket (Linux; needed for DTLS). macOS: not supported - the single one-to-many
/// socket delivers every association, so nothing is peeled off.
peel_off_result peel_off_socket(int fd, int assoc_id);

} // namespace sctp_backend

} // namespace ocudu
