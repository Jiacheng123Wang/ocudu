// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/gateways/sctp_socket.h"
#include "sctp_socket_backend.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/io/sockets.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

using namespace ocudu;
using namespace ocudu::sctp_backend;

/// Subscribes to a single SCTP event type.
static bool sctp_subscribe_to_event(const unique_fd& fd, uint16_t event_type)
{
  struct sctp_event event = {};
  event.se_assoc_id       = SCTP_FUTURE_ASSOC;
  event.se_type           = event_type;
  event.se_on             = 1;

  return setsockopt_shim(fd, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) == 0;
}

/// Subscribes to various SCTP events to handle association and shutdown gracefully.
static bool sctp_subscribe_to_events(const unique_fd& fd)
{
  ocudu_sanity_check(fd.is_open(), "Invalid FD");

  // Subscribe to the per-message info delivery (platform-specific mechanism, see the backend) and to each event
  // individually using the SCTP_EVENT socket option.
  if (not enable_per_message_info(fd)) {
    return false;
  }
  if (!sctp_subscribe_to_event(fd, SCTP_SHUTDOWN_EVENT)) {
    return false;
  }
  if (!sctp_subscribe_to_event(fd, SCTP_ASSOC_CHANGE)) {
    return false;
  }
  return true;
}

/// \brief Modify SCTP default parameters for quicker detection of broken links.
/// Changes to the maximum re-transmission timeout (rto_max).
static bool sctp_set_rto_opts(const unique_fd&                         fd,
                              std::optional<std::chrono::milliseconds> rto_initial,
                              std::optional<std::chrono::milliseconds> rto_min,
                              std::optional<std::chrono::milliseconds> rto_max,
                              const std::string&                       if_name,
                              ocudulog::basic_logger&                  logger)
{
  ocudu_sanity_check(fd.is_open(), "Invalid FD");

  if (not rto_initial.has_value() && not rto_min.has_value() && not rto_max.has_value()) {
    // no need to set RTO
    return true;
  }

  // Set RTO_MAX to quickly detect broken links.
  sctp_rtoinfo rto_opts  = {};
  socklen_t    rto_sz    = sizeof(sctp_rtoinfo);
  rto_opts.srto_assoc_id = SCTP_FUTURE_ASSOC;
  if (getsockopt_shim(fd, SOL_SCTP, SCTP_RTOINFO, &rto_opts, &rto_sz) < 0) {
    logger.error("{}: Error getting RTO_INFO sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }

  if (rto_initial.has_value()) {
    rto_opts.srto_initial = rto_initial.value().count();
  }
  if (rto_min.has_value()) {
    rto_opts.srto_min = rto_min.value().count();
  }
  if (rto_max.has_value()) {
    rto_opts.srto_max = rto_max.value().count();
  }

  // Platform validation of the RTO triple (usrsctp requires rto_min <= rto_initial <= rto_max; the kernel accepts
  // any combination). No-op on Linux.
  adjust_rto_opts(rto_opts, if_name, logger);

  logger.debug(
      "{}: Setting RTO_INFO options on SCTP socket. Association {}, Initial RTO {}, Minimum RTO {}, Maximum RTO {}",
      if_name,
      rto_opts.srto_assoc_id,
      rto_opts.srto_initial,
      rto_opts.srto_min,
      rto_opts.srto_max);

  if (setsockopt_shim(fd, SOL_SCTP, SCTP_RTOINFO, &rto_opts, rto_sz) < 0) {
    logger.error("{}: Error setting RTO_INFO sockopts. errno={}", if_name, ::strerror(errno));
    return false;
  }
  return true;
}

/// \brief Modify SCTP default parameters for quicker detection of broken links.
/// Changes to the SCTP_INITMSG parameters (to control the timeout of the connect() syscall)
static bool sctp_set_init_msg_opts(const unique_fd&                         fd,
                                   std::optional<int>                       init_max_attempts,
                                   std::optional<std::chrono::milliseconds> max_init_timeo,
                                   const std::string&                       if_name,
                                   ocudulog::basic_logger&                  logger)
{
  ocudu_sanity_check(fd.is_open(), "Invalid FD");

  if (not init_max_attempts.has_value() && not max_init_timeo.has_value()) {
    // No value set for init max attempts or max init_timeo,
    // no need to call set_sockopts()
    return true;
  }

  // Set SCTP INITMSG options to reduce blocking timeout of connect()
  sctp_initmsg init_opts = {};
  socklen_t    init_sz   = sizeof(sctp_initmsg);
  if (not get_init_msg_opts(fd, init_opts, init_sz, if_name, logger)) {
    return false; // Responsibility of closing the socket is on the caller
  }

  if (init_max_attempts.has_value()) {
    init_opts.sinit_max_attempts = init_max_attempts.value();
  }
  if (max_init_timeo.has_value()) {
    init_opts.sinit_max_init_timeo = max_init_timeo.value().count();
  }

  logger.debug("{}: Setting SCTP_INITMSG options on SCTP socket. Max attempts {}, Max init attempts timeout {}",
               if_name,
               init_opts.sinit_max_attempts,
               init_opts.sinit_max_init_timeo);
  if (setsockopt_shim(fd, SOL_SCTP, SCTP_INITMSG, &init_opts, init_sz) < 0) {
    logger.error("{}: Error setting SCTP_INITMSG sockopts. errno={}\n", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }
  return true;
}

/// \brief Modify SCTP default Peer Address parameters for quicker detection of broken links.
/// Changes to the heartbeat interval.
static bool sctp_set_paddr_opts(const unique_fd&                         fd,
                                std::optional<std::chrono::milliseconds> hb_interval,
                                const std::string&                       if_name,
                                ocudulog::basic_logger&                  logger)
{
  ocudu_sanity_check(fd.is_open(), "Invalid FD");

  if (not hb_interval.has_value()) {
    // no need to set heartbeat interval
    return true;
  }

  // Set SCTP_PEER_ADDR_PARAMS to quickly detect broken links.
  sctp_paddrparams paddr_opts = {};
  socklen_t        paddr_sz   = sizeof(sctp_paddrparams);
  paddr_opts.spp_assoc_id     = SCTP_FUTURE_ASSOC;
  if (getsockopt_shim(fd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS, &paddr_opts, &paddr_sz) < 0) {
    logger.error("{}: Error getting SCTP_PEER_ADDR_PARAMS sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }

  if (hb_interval.has_value()) {
    paddr_opts.spp_hbinterval = hb_interval.value().count();
  }

  logger.debug("{}: Setting SCTP_PEER_ADDR_PARAMS options on SCTP socket. Heartbeat Interval={}",
               if_name,
               (unsigned)paddr_opts.spp_hbinterval);

  if (setsockopt_shim(fd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS, &paddr_opts, paddr_sz) < 0) {
    logger.error("{}: Error setting SCTP_PEER_ADDR_PARAMS sockopts. errno={}", if_name, ::strerror(errno));
    return false;
  }
  return true;
}

/// \brief Modify SCTP default Assocination parameters for quicker detection of broken links.
/// Changes to the maximum number of re-transmission sent before a address is considered unreachable.
static bool sctp_set_assoc_opts(const unique_fd&        fd,
                                std::optional<int>      assoc_max_rxt,
                                const std::string&      if_name,
                                ocudulog::basic_logger& logger)
{
  ocudu_sanity_check(fd.is_open(), "Invalid FD");

  if (not assoc_max_rxt.has_value()) {
    // no need to set assoc max rxt
    return true;
  }

  // Set SCTP_ASSOCINFO to quickly detect broken links.
  sctp_assocparams assoc_opts = {};
  socklen_t        assoc_sz   = sizeof(sctp_assocparams);
  assoc_opts.sasoc_assoc_id   = SCTP_FUTURE_ASSOC;
  if (getsockopt_shim(fd, SOL_SCTP, SCTP_ASSOCINFO, &assoc_opts, &assoc_sz) < 0) {
    logger.error("{}: Error getting SCTP_ASSOCINFO sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }

  if (assoc_max_rxt.has_value()) {
    assoc_opts.sasoc_asocmaxrxt = assoc_max_rxt.value();
  }

  logger.debug("{}: Setting SCTP_ASSOCINFO options on SCTP socket. Maximum Retransmissions {}",
               if_name,
               assoc_opts.sasoc_asocmaxrxt);

  if (setsockopt_shim(fd, SOL_SCTP, SCTP_ASSOCINFO, &assoc_opts, assoc_sz) < 0) {
    logger.error("{}: Error setting SCTP_ASSOCINFO sockopts. errno={}", if_name, ::strerror(errno));
    return false;
  }
  return true;
}

/// Set or unset SCTP_NODELAY. With NODELAY enabled, SCTP messages are sent as soon as possible with no unnecessary
/// delay, at the cost of transmitting more packets over the network. Otherwise their transmission might be delayed and
/// concatenated with subsequent messages in order to transmit them in one big PDU.
///
/// Note: If the local interface supports jumbo frames (MTU size > 1500) but not the receiver, then the receiver might
/// discard big PDUs and the stream might get stuck.
static bool sctp_set_nodelay(const unique_fd& fd, std::optional<bool> nodelay)
{
  if (not nodelay.has_value()) {
    // no need to change anything
    return true;
  }

  int optval = nodelay.value() ? 1 : 0;
  return setsockopt_shim(fd, IPPROTO_SCTP, SCTP_NODELAY, &optval, sizeof(optval)) == 0;
}

/// \brief Pack addresses into contiguous buffer with correct sizes for each address family.
/// sctp_bindx/sctp_connectx expect addresses to be packed based on their actual size (sockaddr_in or sockaddr_in6),
/// not sockaddr_storage which is 128 bytes and causes offset issues when reading multiple addresses.
static bool pack_addresses(const std::vector<sockaddr_storage>& addrs, std::vector<uint8_t>& packed_addrs)
{
  size_t total_size = 0;
  for (const auto& addr : addrs) {
    sa_family_t family = reinterpret_cast<const sockaddr*>(&addr)->sa_family;
    if (family == AF_INET) {
      total_size += sizeof(sockaddr_in);
    } else if (family == AF_INET6) {
      total_size += sizeof(sockaddr_in6);
    } else {
      return false;
    }
  }

  packed_addrs.resize(total_size);
  size_t offset = 0;
  for (const auto& addr : addrs) {
    sa_family_t family    = reinterpret_cast<const sockaddr*>(&addr)->sa_family;
    size_t      addr_size = (family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    std::memcpy(packed_addrs.data() + offset, &addr, addr_size);
    offset += addr_size;
  }

  return true;
}

// sctp_socket class.

sctp_socket::sctp_socket() : logger(ocudulog::fetch_basic_logger("SCTP-GW")) {}

expected<sctp_socket> sctp_socket::create(const sctp_socket_params& params)
{
  sctp_socket socket;
  if (params.if_name.empty()) {
    socket.logger.error("Failed to create SCTP socket. Cause: No interface name was provided");
    return make_unexpected(default_error_t{});
  }
  socket.if_name = params.if_name;

  // Platform mapping lives in the backend: a kernel SCTP socket on Linux, a usrsctp socket bridged to an fd on
  // macOS (see sctp_socket_linux.cpp / sctp_socket_usrsctp.cpp).
  auto created = sctp_backend::create_socket(params, socket.if_name, socket.logger);
  if (not created.has_value()) {
    return make_unexpected(default_error_t{});
  }
  socket.sock_fd = std::move(created.value());

  socket.logger.debug("{}: SCTP socket created with fd={}", socket.if_name, socket.sock_fd.value());

  if (not socket.set_sockopts(params)) {
    socket.close();
    return make_unexpected(default_error_t{});
  }

  // Save non-blocking mode to apply after bind/connect. We do not yet support async bind/connect.
  socket.non_blocking_mode = params.non_blocking_mode;

  return socket;
}

sctp_socket& sctp_socket::operator=(sctp_socket&& other) noexcept
{
  sock_fd           = std::move(other.sock_fd);
  if_name           = std::move(other.if_name);
  non_blocking_mode = other.non_blocking_mode;
  return *this;
}

bool sctp_socket::close()
{
  if (not sock_fd.is_open()) {
    return true;
  }

  // Platform teardown (macOS: graceful association shutdown + shim bookkeeping) before closing the fd.
  sctp_backend::prepare_close(sock_fd, if_name, logger);

  if (not sock_fd.close()) {
    logger.error("{}: Error closing SCTP socket: {}", if_name, ::strerror(errno));
    return false;
  }
  logger.info("{}: SCTP socket closed", if_name);
  if_name.clear();
  return true;
}

bool sctp_socket::bindx(const std::vector<sockaddr_storage>& addrs, const std::string& bind_interface)
{
  if (addrs.empty()) {
    logger.info("{}: Failed to bind {} address(es). Cause: Empty list was provided", if_name, addrs.size());
    return false;
  }

  if (not is_open()) {
    logger.error("{}: Failed to bind {} address(es). Cause: Socket is closed", if_name, addrs.size());
    return false;
  }

  // SO_BINDTODEVICE does not make sense in case of multihoming with SCTP.
  // Only bind to interface if a single or no address is provided.
  if (addrs.size() <= 1) {
    if (not bind_to_interface(sock_fd, bind_interface, logger)) {
      return false;
    }
  } else if (!bind_interface.empty() && bind_interface != "auto") {
    logger.error(
        "{}: bind_interface is not supported with multihoming ({} bind addresses configured) and will be ignored. "
        "Please remove bind_interface or use a single address",
        if_name,
        addrs.size());
  }

  logger.debug("{}: Binding {} address(es) using sctp_bindx()...", if_name, addrs.size());

  std::vector<uint8_t> packed_addrs;
  if (!pack_addresses(addrs, packed_addrs)) {
    logger.error("{}: Unknown address family in sctp_bindx address list", if_name);
    return false;
  }

  int result =
      sctp_bindx(sock_fd.value(), reinterpret_cast<sockaddr*>(packed_addrs.data()), addrs.size(), SCTP_BINDX_ADD_ADDR);

  if (result < 0) {
    logger.error("{}: Failed to bind {} address(es). Cause: {}", if_name, addrs.size(), ::strerror(errno));
    return false;
  }

  logger.info("{}: Bind to {} address(es) was successful", if_name, addrs.size());
  // Set socket to non-blocking after bind is successful.
  if (non_blocking_mode) {
    if (not set_non_blocking()) {
      return false;
    }
  }

  return true;
}

bool sctp_socket::connectx(const std::vector<sockaddr_storage>& addrs, sctp_assoc_t& assoc_id)
{
  if (addrs.empty()) {
    logger.error("{}: Failed to connect with sctp_connectx(). Cause: can't connect with empty address list", if_name);
    return false;
  }

  if (not is_open()) {
    logger.error(
        "{}: Failed to connect to {} address(es) with sctp_connectx(). Cause: socket is closed", if_name, addrs.size());
    return false;
  }

  logger.debug("{}: Connecting to {} address(es) using sctp_connectx()...", if_name, addrs.size());

  std::vector<uint8_t> packed_addrs;
  if (!pack_addresses(addrs, packed_addrs)) {
    logger.error("{}: Unknown address family in sctp_connectx address list", if_name);
    return false;
  }

  int result =
      sctp_connectx(sock_fd.value(), reinterpret_cast<sockaddr*>(packed_addrs.data()), addrs.size(), &assoc_id);

  if (result < 0) {
    logger.debug("{}: Failed to connect to {} address(es) with sctp_connectx(). Cause: {}",
                 if_name,
                 addrs.size(),
                 ::strerror(errno));
    return false;
  }

  logger.info("{}: Successfully connected to {} address(es) using sctp_connectx()", if_name, addrs.size());

  // Set socket to non-blocking after sctp_connectx() finishes.
  // TODO: for more than one client association per socket we will need to do sctp_connectx() in a non-blocking way:
  // - handle EINPROGRESS,
  // - wait for SCTP_ASSOC_CHANGE/SCTP_COMM_UP event.
  if (non_blocking_mode) {
    if (not set_non_blocking()) {
      return false;
    }
  }

  return true;
}

bool sctp_socket::listen()
{
  if (not is_open()) {
    logger.error("{}: Failed to listen for new SCTP connections. Cause: socket is closed", if_name);
    return false;
  }

  auto port = get_bound_port();
  if (not port.has_value() || port.value() == 0) {
    logger.error("{}: Failed to listen for new SCTP connections. Cause: server needs to be bound to a port", if_name);
    return false;
  }

  // Listen for connections (platform mapping in the backend: kernel listen() vs usrsctp_listen()).
  int ret = sctp_backend::listen_socket(sock_fd);
  if (ret != 0) {
    logger.error("{}: Error in SCTP socket listen: {}", if_name, ::strerror(errno));
    return false;
  }
  if (logger.info.enabled()) {
    // Note: avoid computing the listen_port if log channel is disabled.
    logger.info("{}: Listening for new SCTP connections on port {}...", if_name, port.value());
  }
  return true;
}

bool sctp_socket::set_non_blocking()
{
  return sctp_backend::set_non_blocking(sock_fd, logger);
}

bool sctp_socket::set_sockopts(const sctp_socket_params& params)
{
  logger.debug("Setting socket options. params=[{}]", params);
  if (not sctp_subscribe_to_events(sock_fd)) {
    logger.error(
        "{}: SCTP failed to be created. Cause: Subscribing to SCTP events failed: {}", if_name, ::strerror(errno));
    return false;
  }

  // Disable IPV6_V6ONLY for IPv6 sockets to support both IPv4 and IPv6.
  if (not sctp_backend::set_ipv6_v6only(sock_fd, params.ai_family, if_name, logger)) {
    return false;
  }

  // Receive timeout (kernel SO_RCVTIMEO on Linux; emulated by the shim's sctp_recvmsg() on macOS).
  if (not sctp_backend::apply_receive_timeout(sock_fd, params, if_name, logger)) {
    return false;
  }

  // Set SRTO_MAX
  if (not sctp_set_rto_opts(sock_fd, params.rto_initial, params.rto_min, params.rto_max, if_name, logger)) {
    return false;
  }

  // Set SCTP init options
  if (not sctp_set_init_msg_opts(sock_fd, params.init_max_attempts, params.max_init_timeo, if_name, logger)) {
    return false;
  }

  // Set SCTP association options
  if (not sctp_set_assoc_opts(sock_fd, params.assoc_max_rxt, if_name, logger)) {
    return false;
  }

  // Set SCTP peer address options
  if (not sctp_set_paddr_opts(sock_fd, params.hb_interval, if_name, logger)) {
    return false;
  }

  // Set SCTP NODELAY option
  if (not sctp_set_nodelay(sock_fd, params.nodelay)) {
    logger.error("{}: Could not set SCTP_NODELAY. optval={} error={}",
                 if_name,
                 params.nodelay.value() ? 1 : 0,
                 ::strerror(errno));
    return false;
  }

  // SO_REUSEADDR (kernel option; skipped on macOS, where usrsctp manages port binding in user space).
  if (not sctp_backend::apply_reuse_addr(sock_fd, params, logger)) {
    return false;
  }

  return true;
}

std::optional<uint16_t> sctp_socket::get_bound_port() const
{
  if (not sock_fd.is_open()) {
    logger.error("Socket of SCTP network gateway not created.");
    return {};
  }

  return sctp_backend::get_bound_port(sock_fd, if_name, logger);
}

std::optional<int> sctp_socket::get_address_family() const
{
  if (not sock_fd.is_open()) {
    return {};
  }

  return sctp_backend::get_address_family(sock_fd, if_name, logger);
}
