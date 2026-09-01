// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \brief Linux backend of the SCTP gateway family: the native kernel SCTP stack.
///
/// This is the upstream implementation (byte-for-byte): socket creation via
/// ::socket(IPPROTO_SCTP), options through ::setsockopt/::getsockopt, and the
/// lksctp API (sctp_bindx / sctp_sendmsg / ...) linked via ${SCTP_LIBRARIES}.

#include "sctp_socket_backend.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/io/sockets.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/socket.h>

using namespace ocudu;
using namespace ocudu::sctp_backend;

// Human-readable names for the SCTP notification fields in the gateway logs (kernel enum types; the usrsctp
// backend logs the numeric values instead, as before).
template <>
struct fmt::formatter<sctp_sac_state> : fmt::formatter<std::string_view> {
  auto format(sctp_sac_state v, fmt::format_context& ctx) const
  {
    std::string_view name = "UNKNOWN";
    switch (v) {
      case SCTP_COMM_UP:
        name = "SCTP_COMM_UP";
        break;
      case SCTP_COMM_LOST:
        name = "SCTP_COMM_LOST";
        break;
      case SCTP_RESTART:
        name = "SCTP_RESTART";
        break;
      case SCTP_SHUTDOWN_COMP:
        name = "SCTP_SHUTDOWN_COMP";
        break;
      case SCTP_CANT_STR_ASSOC:
        name = "SCTP_CANT_STR_ASSOC";
        break;
    }
    return fmt::formatter<std::string_view>::format(name, ctx);
  }
};

template <>
struct fmt::formatter<sctp_sn_error> : fmt::formatter<std::string_view> {
  auto format(sctp_sn_error v, fmt::format_context& ctx) const
  {
    std::string_view name = "UNKNOWN";
    switch (v) {
      case SCTP_FAILED_THRESHOLD:
        name = "SCTP_FAILED_THRESHOLD";
        break;
      case SCTP_RECEIVED_SACK:
        name = "SCTP_RECEIVED_SACK";
        break;
      case SCTP_HEARTBEAT_SUCCESS:
        name = "SCTP_HEARTBEAT_SUCCESS";
        break;
      case SCTP_RESPONSE_TO_USER_REQ:
        name = "SCTP_RESPONSE_TO_USER_REQ";
        break;
      case SCTP_INTERNAL_ERROR:
        name = "SCTP_INTERNAL_ERROR";
        break;
      case SCTP_SHUTDOWN_GUARD_EXPIRES:
        name = "SCTP_SHUTDOWN_GUARD_EXPIRES";
        break;
      case SCTP_PEER_FAULTY:
        name = "SCTP_PEER_FAULTY";
        break;
    }
    return fmt::formatter<std::string_view>::format(name, ctx);
  }
};

template <>
struct fmt::formatter<sctp_sn_type> : fmt::formatter<std::string_view> {
  auto format(sctp_sn_type v, fmt::format_context& ctx) const
  {
    std::string_view name = "UNKNOWN";
    switch (v) {
      case SCTP_DATA_IO_EVENT:
        name = "SCTP_DATA_IO_EVENT";
        break;
      case SCTP_ASSOC_CHANGE:
        name = "SCTP_ASSOC_CHANGE";
        break;
      case SCTP_PEER_ADDR_CHANGE:
        name = "SCTP_PEER_ADDR_CHANGE";
        break;
      case SCTP_SEND_FAILED:
        name = "SCTP_SEND_FAILED";
        break;
      case SCTP_REMOTE_ERROR:
        name = "SCTP_REMOTE_ERROR";
        break;
      case SCTP_SHUTDOWN_EVENT:
        name = "SCTP_SHUTDOWN_EVENT";
        break;
      case SCTP_PARTIAL_DELIVERY_EVENT:
        name = "SCTP_PARTIAL_DELIVERY_EVENT";
        break;
      case SCTP_ADAPTATION_INDICATION:
        name = "SCTP_ADAPTATION_INDICATION";
        break;
      case SCTP_AUTHENTICATION_EVENT:
        name = "SCTP_AUTHENTICATION_EVENT";
        break;
      case SCTP_SENDER_DRY_EVENT:
        name = "SCTP_SENDER_DRY_EVENT";
        break;
      case SCTP_STREAM_RESET_EVENT:
        name = "SCTP_STREAM_RESET_EVENT";
        break;
      case SCTP_ASSOC_RESET_EVENT:
        name = "SCTP_ASSOC_RESET_EVENT";
        break;
      case SCTP_STREAM_CHANGE_EVENT:
        name = "SCTP_STREAM_CHANGE_EVENT";
        break;
      case SCTP_SEND_FAILED_EVENT:
        name = "SCTP_SEND_FAILED_EVENT";
        break;
    }
    return fmt::formatter<std::string_view>::format(name, ctx);
  }
};

struct addrinfo sctp_backend::make_sctp_addrinfo_hints()
{
  struct addrinfo hints = {};
  // Support ipv4, ipv6 and hostnames.
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_SEQPACKET;
  hints.ai_protocol = IPPROTO_SCTP;
  return hints;
}

expected<unique_fd> sctp_backend::create_socket(const sctp_socket_params& params,
                                               const std::string&        if_name,
                                               ocudulog::basic_logger&   logger)
{
  unique_fd sock_fd{::socket(params.ai_family, params.ai_socktype, IPPROTO_SCTP)};
  if (not sock_fd.is_open()) {
    int ret = errno;
    if (ret == ESOCKTNOSUPPORT) {
      logger.error(
          "{}: Failed to create SCTP socket: {}. Hint: Please ensure 'sctp' kernel module is available on the system.",
          if_name,
          ::strerror(ret));
      report_error("{}: Failed to create SCTP socket: {}. Hint: Please ensure 'sctp' kernel module is available on the "
                   "system.\n",
                   if_name,
                   ::strerror(ret));
    }
    return make_unexpected(default_error_t{});
  }
  return sock_fd;
}

void sctp_backend::prepare_close(const unique_fd& /*sock_fd*/,
                                 const std::string& /*if_name*/,
                                 ocudulog::basic_logger& /*logger*/)
{
  // Nothing to prepare: closing a kernel SCTP socket shuts the associations down gracefully.
}

int sctp_backend::getsockopt_shim(const unique_fd& fd, int level, int option_name, void* optval, socklen_t* optlen)
{
  return ::getsockopt(fd.value(), level, option_name, optval, optlen);
}

int sctp_backend::setsockopt_shim(const unique_fd& fd, int level, int option_name, const void* optval, socklen_t optlen)
{
  return ::setsockopt(fd.value(), level, option_name, optval, optlen);
}

bool sctp_backend::enable_per_message_info(const unique_fd& fd)
{
  // SCTP_DATA_IO_EVENT makes the kernel deliver a sctp_sndrcvinfo with every message.
  struct sctp_event event = {};
  event.se_assoc_id       = SCTP_FUTURE_ASSOC;
  event.se_type           = SCTP_DATA_IO_EVENT;
  event.se_on             = 1;
  return ::setsockopt(fd.value(), IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) == 0;
}

void sctp_backend::adjust_rto_opts(sctp_rtoinfo& /*rto_opts*/,
                                   const std::string& /*if_name*/,
                                   ocudulog::basic_logger& /*logger*/)
{
  // The Linux kernel accepts any rto_min/initial/max combination.
}

bool sctp_backend::get_init_msg_opts(const unique_fd&        fd,
                                     sctp_initmsg&           init_opts,
                                     socklen_t&              init_sz,
                                     const std::string&      if_name,
                                     ocudulog::basic_logger& logger)
{
  if (::getsockopt(fd.value(), SOL_SCTP, SCTP_INITMSG, &init_opts, &init_sz) < 0) {
    logger.error("{}: Error getting sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }
  return true;
}

bool sctp_backend::set_ipv6_v6only(const unique_fd& fd, int ai_family, const std::string& if_name, ocudulog::basic_logger& logger)
{
  if (ai_family != AF_INET6) {
    // Only applicable to IPv6 sockets
    return true;
  }

  int optval = 0; // 0 = allow both IPv4 and IPv6
  if (::setsockopt(fd.value(), IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
    logger.error("{}: Failed to set IPV6_V6ONLY=0. errno={}", if_name, ::strerror(errno));
    return false;
  }

  logger.debug("{}: IPV6_V6ONLY disabled, socket supports both IPv4 and IPv6", if_name);
  return true;
}

bool sctp_backend::apply_receive_timeout(const unique_fd&         fd,
                                         const sctp_socket_params& params,
                                         const std::string&       if_name,
                                         ocudulog::basic_logger&  logger)
{
  if (params.rx_timeout.count() > 0) {
    if (not set_receive_timeout(fd, params.rx_timeout, logger)) {
      logger.error("{}: Error setting SO_RCVTIMEO", if_name);
      return false;
    }
  }
  return true;
}

bool sctp_backend::apply_reuse_addr(const unique_fd&         fd,
                                    const sctp_socket_params& params,
                                    ocudulog::basic_logger&  logger)
{
  if (params.reuse_addr) {
    if (not set_reuse_addr(fd, logger)) {
      return false;
    }
  }
  return true;
}

int sctp_backend::listen_socket(const unique_fd& fd)
{
  return ::listen(fd.value(), SOMAXCONN);
}

bool sctp_backend::set_non_blocking(const unique_fd& fd, ocudulog::basic_logger& logger)
{
  return ocudu::set_non_blocking(fd, logger);
}

std::optional<uint16_t> sctp_backend::get_bound_port(const unique_fd&      fd,
                                                     const std::string&    if_name,
                                                     ocudulog::basic_logger& logger)
{
  sockaddr_storage gw_addr_storage;
  sockaddr*        gw_addr     = (sockaddr*)&gw_addr_storage;
  socklen_t        gw_addr_len = sizeof(gw_addr_storage);

  int ret = ::getsockname(fd.value(), gw_addr, &gw_addr_len);
  if (ret != 0) {
    logger.error("{}: Failed `getsockname` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 fd.value(),
                 ::strerror(errno));
    return {};
  }

  uint16_t gw_bound_port;
  if (gw_addr->sa_family == AF_INET) {
    gw_bound_port = ntohs(((sockaddr_in*)gw_addr)->sin_port);
  } else if (gw_addr->sa_family == AF_INET6) {
    gw_bound_port = ntohs(((sockaddr_in6*)gw_addr)->sin6_port);
  } else {
    logger.error("{}: Unhandled address family in SCTP network gateway with sock_fd={} family={}",
                 if_name,
                 fd.value(),
                 gw_addr->sa_family);
    return {};
  }

  return gw_bound_port;
}

std::optional<int> sctp_backend::get_address_family(const unique_fd&      fd,
                                                    const std::string&    if_name,
                                                    ocudulog::basic_logger& logger)
{
  sockaddr_storage gw_addr_storage;
  sockaddr*        gw_addr     = (sockaddr*)&gw_addr_storage;
  socklen_t        gw_addr_len = sizeof(gw_addr_storage);

  int ret = ::getsockname(fd.value(), gw_addr, &gw_addr_len);
  if (ret != 0) {
    logger.error("{}: Failed `getsockname` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 fd.value(),
                 ::strerror(errno));
    return {};
  }

  if (gw_addr->sa_family == AF_INET || gw_addr->sa_family == AF_INET6) {
    return gw_addr->sa_family;
  }

  return {};
}

int sctp_backend::receive_available(int fd, message_sink sink, void* user)
{
  // One message per broker wake-up: the kernel delivers one event per peeled-off association fd.
  std::array<uint8_t, sctp_backend::max_sctp_msg_len> temp_recv_buffer;
  struct sctp_sndrcvinfo sri       = {};
  int                    msg_flags = 0;

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
    return -1;
  }

  std::vector<uint8_t> payload(temp_recv_buffer.begin(), temp_recv_buffer.begin() + rx_bytes);
  sink(user, std::move(payload), sri, msg_flags, msg_src_addr, msg_src_addrlen);
  return 1;
}

int sctp_backend::recv_message_nowait(int                      fd,
                                      void*                   msg,
                                      size_t                  len,
                                      struct sockaddr*        from,
                                      socklen_t*              fromlen,
                                      struct sctp_sndrcvinfo* sri,
                                      socklen_t*              /*sri_len*/,
                                      int*                    msg_flags)
{
  // The drain loops behind one broker wake-up are only needed on the single-bridge-fd model (macOS); on Linux the
  // kernel delivers one event per peeled-off fd, so receive_available() never calls this. Kept for API symmetry:
  // implement it with a temporary O_NONBLOCK toggle, since lksctp's sctp_recvmsg() cannot pass MSG_DONTWAIT through.
  const int orig_flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK);
  int ret            = ::sctp_recvmsg(fd, msg, len, from, fromlen, sri, msg_flags);
  int saved_errno    = errno;
  ::fcntl(fd, F_SETFL, orig_flags);
  errno = saved_errno;
  return ret;
}

int sctp_backend::send_eof(int                    fd,
                           const struct sockaddr* dest_addr,
                           socklen_t              dest_addrlen,
                           uint32_t               ppid /* network byte order */,
                           unsigned               stream_no)
{
  struct sctp_sndinfo sndinfo{};
  sndinfo.snd_sid   = stream_no;
  sndinfo.snd_ppid  = ppid;
  sndinfo.snd_flags = SCTP_EOF;

  char control[CMSG_SPACE(sizeof(sndinfo))];

  struct iovec iov{};
  iov.iov_base = nullptr;
  iov.iov_len  = 0;

  struct msghdr msg{};
  msg.msg_name    = const_cast<struct sockaddr*>(dest_addr);
  msg.msg_namelen = dest_addrlen;
  msg.msg_iov     = &iov;
  msg.msg_iovlen  = 1;

  msg.msg_control    = control;
  msg.msg_controllen = sizeof(control);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level     = IPPROTO_SCTP;
  cmsg->cmsg_type      = SCTP_SNDINFO;
  cmsg->cmsg_len       = CMSG_LEN(sizeof(sndinfo));

  memcpy(CMSG_DATA(cmsg), &sndinfo, sizeof(sndinfo));

  msg.msg_controllen = cmsg->cmsg_len;

  return sendmsg(fd, &msg, MSG_NOSIGNAL);
}

peel_off_result sctp_backend::peel_off_socket(int fd, int assoc_id)
{
  peel_off_result result;
  result.supported = true;
  result.fd        = unique_fd(::sctp_peeloff(fd, assoc_id));
  return result;
}
