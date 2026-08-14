// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/gateways/sctp_socket.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/io/sockets.h"
#include "ocudu/support/ocudu_assert.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <unordered_map>
#include <mutex>
#if defined(__APPLE__)
#include <usrsctp.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <netinet/sctp.h>
#endif
#include <sys/socket.h>

using namespace ocudu;

#if defined(__APPLE__)
// usrsctp is a user-space SCTP stack: its sockets are "struct socket*" pointers rather than kernel file
// descriptors. This shim bridges them to the fd-based code (and to the kqueue io_broker) as follows:
//   - a socketpair is created for every SCTP socket. Its read end is the fd handed out to the rest of the code
//     and registered with the io_broker;
//   - the write end is passed as user data to usrsctp_set_upcall(): whenever the stack has data or events pending
//     on the socket, the upcall writes one byte, which wakes the broker (EVFILT_READ on the read end);
//   - sctp_recvmsg() consumes one byte after every usrsctp_recvv() call.
static std::unordered_map<int, struct socket*> g_sctp_map;
static std::unordered_map<int, int>            g_write_fd_map;
static std::mutex                              g_sctp_mutex;

/// Initializes the usrsctp library once per process (creates its internal timer and worker threads).
static void usrsctp_once_init()
{
  static std::once_flag init_flag;
  std::call_once(init_flag, []() { usrsctp_init(0, nullptr, nullptr); });
}

/// Returns the usrsctp socket associated to the bridge fd, or nullptr if the fd is unknown.
static struct socket* get_usr_socket(const unique_fd& fd)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(fd.value());
  return (it != g_sctp_map.end()) ? it->second : nullptr;
}

/// get/setsockopt shims: SCTP options must be applied to the usrsctp socket, never to the AF_UNIX bridge
/// socketpair.
static int sctp_getsockopt_shim(const unique_fd& fd, int level, int option_name, void* optval, socklen_t* optlen)
{
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_getsockopt(so, level, option_name, optval, optlen);
}

static int
sctp_setsockopt_shim(const unique_fd& fd, int level, int option_name, const void* optval, socklen_t optlen)
{
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_setsockopt(so, level, option_name, optval, optlen);
}

static socklen_t sockaddr_actual_size(const sockaddr* addr)
{
  return (addr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

/// Called by the usrsctp stack when data, events or errors are pending on the socket. Wakes up the io_broker by
/// writing a byte to the bridge socketpair.
static void my_upcall_func(struct socket* sock, void* addr, int flags)
{
  (void)sock;
  (void)flags;
  int write_fd = static_cast<int>(reinterpret_cast<intptr_t>(addr));
  if (write_fd <= 0) {
    return;
  }
  // The write end is non-blocking: if the pipe is already full, a wake-up is pending in it anyway.
  uint8_t dummy = 1;
  (void)::write(write_fd, &dummy, sizeof(dummy));
}

int sctp_bindx(int s, struct sockaddr* addrs, int addrcnt, int flags)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  // One-to-one mapping: usrsctp_bindx() iterates over the packed addresses based on their address family,
  // mirroring the kernel's sctp_bindx().
  return usrsctp_bindx(it->second, addrs, addrcnt, flags);
}

int sctp_connectx(int s, struct sockaddr* addrs, int addrcnt, uint32_t* id)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  // usrsctp has no connectx(): connect to the first address (connecting to multiple peer addresses is not
  // supported by usrsctp) and report the association id via usrsctp_getassocid().
  (void)addrcnt;
  int ret = usrsctp_connect(it->second, addrs, sockaddr_actual_size(addrs));
  if (ret < 0) {
    return -1;
  }
  if (id != nullptr) {
    *id = usrsctp_getassocid(it->second, addrs);
  }
  return 0;
}

int sctp_sendmsg(int s, const void* msg, size_t len, struct sockaddr* to, socklen_t tolen,
                 uint32_t ppid, uint32_t flags, uint16_t stream_no, uint32_t timetolive, uint32_t context)
{
  (void)tolen;
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  // Carry the control information (PPID, stream, flags, context) in a sctp_sndinfo structure, mirroring the
  // kernel's sctp_sendmsg(). The PPID is passed in network byte order by the callers (matching the Linux API).
  // Note: timetolive has no sctp_sndinfo field; the callers always pass 0 for it.
  (void)timetolive;
  struct sctp_sndinfo sinfo = {};
  sinfo.sinfo_ppid           = ppid;
  sinfo.sinfo_flags          = flags;
  sinfo.sinfo_stream         = stream_no;
  sinfo.sinfo_context        = context;
  return usrsctp_sendv(
      it->second, msg, len, to, (to != nullptr) ? 1 : 0, &sinfo, sizeof(sinfo), SCTP_SENDV_SNDINFO, 0);
}

int sctp_recvmsg(int s, void* msg, size_t len, struct sockaddr* from, socklen_t* fromlen, void* sinfo,
                 socklen_t* sinfo_len, int* msg_flags)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  struct sctp_rcvinfo rsinfo     = {};
  socklen_t           rsinfo_len = sizeof(rsinfo);
  int                 result = usrsctp_recvv(it->second, msg, len, from, fromlen, &rsinfo, &rsinfo_len, nullptr, msg_flags);

  // Provide the per-message info (association id, PPID, stream, ...) to the caller.
  if (sinfo != nullptr && sinfo_len != nullptr && *sinfo_len >= sizeof(struct sctp_rcvinfo)) {
    ::memcpy(sinfo, &rsinfo, sizeof(struct sctp_rcvinfo));
  }

  // Consume one wake-up byte. The read end is non-blocking: if no byte is pending (e.g., several messages were
  // delivered in one broker wake-up), the read simply returns EAGAIN.
  uint8_t dummy;
  while (::read(s, &dummy, sizeof(dummy)) < 0 and errno == EINTR) {
  }
  return result;
}

int sctp_getpaddrs(int s, uint32_t assoc_id, struct sockaddr** addrs)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  // On macOS (HAVE_SA_LEN) usrsctp packs the returned addresses with their actual sizes, matching the Linux
  // kernel layout, so the shared code walks the array identically on both platforms.
  return usrsctp_getpaddrs(it->second, assoc_id, addrs);
}

void sctp_freepaddrs(struct sockaddr* addrs)
{
  // usrsctp_freepaddrs() unwinds the hidden sctp_getaddresses header.
  usrsctp_freepaddrs(addrs);
}
#else
// On Linux, SCTP options go directly to the kernel socket.
static int sctp_getsockopt_shim(const unique_fd& fd, int level, int option_name, void* optval, socklen_t* optlen)
{
  return ::getsockopt(fd.value(), level, option_name, optval, optlen);
}

static int
sctp_setsockopt_shim(const unique_fd& fd, int level, int option_name, const void* optval, socklen_t optlen)
{
  return ::setsockopt(fd.value(), level, option_name, optval, optlen);
}
#endif

/// Subscribes to a single SCTP event type.
static bool sctp_subscribe_to_event(const unique_fd& fd, uint16_t event_type)
{
  struct sctp_event event = {};
  event.se_assoc_id       = SCTP_FUTURE_ASSOC;
  event.se_type           = event_type;
  event.se_on             = 1;

#if defined(__APPLE__)
  return sctp_setsockopt_shim(fd, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) == 0;
#else
  return ::setsockopt(fd.value(), IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) == 0;
#endif
}

/// Subscribes to various SCTP events to handle association and shutdown gracefully.
static bool sctp_subscribe_to_events(const unique_fd& fd)
{
  ocudu_sanity_check(fd.is_open(), "Invalid FD");

  // Subscribe to each event individually using SCTP_EVENT socket option.
#if !defined(__APPLE__)
  // SCTP_DATA_IO_EVENT is Linux-specific (delivers sctp_sndrcvinfo on receive); the usrsctp shim always provides
  // the sctp_rcvinfo, so this subscription is skipped on macOS.
  if (!sctp_subscribe_to_event(fd, SCTP_DATA_IO_EVENT)) {
    return false;
  }
#endif
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
  if (sctp_getsockopt_shim(fd, SOL_SCTP, SCTP_RTOINFO, &rto_opts, &rto_sz) < 0) {
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

  logger.debug(
      "{}: Setting RTO_INFO options on SCTP socket. Association {}, Initial RTO {}, Minimum RTO {}, Maximum RTO {}",
      if_name,
      rto_opts.srto_assoc_id,
      rto_opts.srto_initial,
      rto_opts.srto_min,
      rto_opts.srto_max);

  if (sctp_setsockopt_shim(fd, SOL_SCTP, SCTP_RTOINFO, &rto_opts, rto_sz) < 0) {
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

#if defined(__APPLE__)
  // usrsctp does not implement SCTP_INITMSG and its connect() path is non-blocking, so the init timeout options
  // are not applicable.
  logger.debug("{}: SCTP_INITMSG is not supported by usrsctp, skipping", if_name);
  return true;
#else
  // Set SCTP INITMSG options to reduce blocking timeout of connect()
  sctp_initmsg init_opts = {};
  socklen_t    init_sz   = sizeof(sctp_initmsg);
  if (sctp_getsockopt_shim(fd, SOL_SCTP, SCTP_INITMSG, &init_opts, &init_sz) < 0) {
    logger.error("{}: Error getting sockopts. errno={}", if_name, ::strerror(errno));
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
  if (sctp_setsockopt_shim(fd, SOL_SCTP, SCTP_INITMSG, &init_opts, init_sz) < 0) {
    logger.error("{}: Error setting SCTP_INITMSG sockopts. errno={}\n", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }
  return true;
#endif
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
  if (sctp_getsockopt_shim(fd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS, &paddr_opts, &paddr_sz) < 0) {
    logger.error("{}: Error getting SCTP_PEER_ADDR_PARAMS sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }

  if (hb_interval.has_value()) {
    paddr_opts.spp_hbinterval = hb_interval.value().count();
  }

  logger.debug("{}: Setting SCTP_PEER_ADDR_PARAMS options on SCTP socket. Heartbeat Interval={}",
               if_name,
               (unsigned)paddr_opts.spp_hbinterval);

  if (sctp_setsockopt_shim(fd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS, &paddr_opts, paddr_sz) < 0) {
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
  if (sctp_getsockopt_shim(fd, SOL_SCTP, SCTP_ASSOCINFO, &assoc_opts, &assoc_sz) < 0) {
    logger.error("{}: Error getting SCTP_ASSOCINFO sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }

  if (assoc_max_rxt.has_value()) {
    assoc_opts.sasoc_asocmaxrxt = assoc_max_rxt.value();
  }

  logger.debug("{}: Setting SCTP_ASSOCINFO options on SCTP socket. Maximum Retransmissions {}",
               if_name,
               assoc_opts.sasoc_asocmaxrxt);

  if (sctp_setsockopt_shim(fd, SOL_SCTP, SCTP_ASSOCINFO, &assoc_opts, assoc_sz) < 0) {
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
#if defined(__APPLE__)
  // usrsctp does not implement SCTP_NODELAY; messages are always sent without Nagle-style delays.
  (void)optval;
  return true;
#else
  return sctp_setsockopt_shim(fd, IPPROTO_SCTP, SCTP_NODELAY, &optval, sizeof(optval)) == 0;
#endif
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

/// Disable IPV6_V6ONLY for IPv6 sockets to allow both IPv4 and IPv6 addresses in case of multihoming.
/// This allows IPv4-mapped IPv6 addresses to work on IPv6 sockets.
static bool
set_ipv6_v6only(const unique_fd& fd, int ai_family, const std::string& if_name, ocudulog::basic_logger& logger)
{
  if (ai_family != AF_INET6) {
    // Only applicable to IPv6 sockets
    return true;
  }

  int optval = 0; // 0 = allow both IPv4 and IPv6
#if defined(__APPLE__)
  // The bridge socketpair must not be touched; apply the option to the usrsctp socket on a best-effort basis.
  if (sctp_setsockopt_shim(fd, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
    logger.debug(
        "{}: Failed to set IPV6_V6ONLY=0 on the usrsctp socket (ignored). errno={}", if_name, ::strerror(errno));
  }
  return true;
#else
  if (::setsockopt(fd.value(), IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
    logger.error("{}: Failed to set IPV6_V6ONLY=0. errno={}", if_name, ::strerror(errno));
    return false;
  }

  logger.debug("{}: IPV6_V6ONLY disabled, socket supports both IPv4 and IPv6", if_name);
  return true;
#endif
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

#if defined(__APPLE__)
  usrsctp_once_init();

  // One-to-many style (SOCK_SEQPACKET), like the Linux kernel SCTP sockets: associations are established and
  // tracked via notifications on this single socket, so no accept() is required. Data is delivered through the
  // recv path (the receive_cb is left null) and the upcall wakes the io_broker.
  struct socket* usr_sock =
      usrsctp_socket(params.ai_family, SOCK_SEQPACKET, IPPROTO_SCTP, nullptr, nullptr, 0, nullptr);
  if (usr_sock == nullptr) {
    socket.logger.error("{}: usrsctp_socket failed", socket.if_name);
    return make_unexpected(default_error_t{});
  }

  // Bridge to the fd-based code and the kqueue broker: the read end is handed out as the "fd" and registered with
  // the io_broker, the write end is woken by the upcall whenever the stack has data or events for the socket.
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    socket.logger.error("{}: socketpair failed: {}", socket.if_name, ::strerror(errno));
    usrsctp_close(usr_sock);
    return make_unexpected(default_error_t{});
  }
  // Both ends non-blocking: the read end so that sctp_recvmsg() never blocks on the wake-up byte, the write end so
  // that the upcall never blocks the usrsctp stack.
  for (int fd : {sv[0], sv[1]}) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 or ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      socket.logger.error("{}: Failed to set bridge socketpair non-blocking: {}", socket.if_name, ::strerror(errno));
      ::close(sv[0]);
      ::close(sv[1]);
      usrsctp_close(usr_sock);
      return make_unexpected(default_error_t{});
    }
  }

  usrsctp_set_upcall(usr_sock, my_upcall_func, reinterpret_cast<void*>(static_cast<intptr_t>(sv[1])));

  {
    std::lock_guard<std::mutex> lock(g_sctp_mutex);
    g_sctp_map[sv[0]]    = usr_sock;
    g_write_fd_map[sv[0]] = sv[1];
  }

  socket.sock_fd = unique_fd{sv[0]};
#else
  socket.sock_fd = unique_fd{::socket(params.ai_family, params.ai_socktype, IPPROTO_SCTP)};
  if (not socket.sock_fd.is_open()) {
    int ret = errno;
    if (ret == ESOCKTNOSUPPORT) {
      socket.logger.error(
          "{}: Failed to create SCTP socket: {}. Hint: Please ensure 'sctp' kernel module is available on the system.",
          socket.if_name,
          ::strerror(ret));
      report_error("{}: Failed to create SCTP socket: {}. Hint: Please ensure 'sctp' kernel module is available on the "
                   "system.\n",
                   socket.if_name,
                   ::strerror(ret));
    }
    return make_unexpected(default_error_t{});
  }
#endif

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

#if defined(__APPLE__)
  int fd = sock_fd.value();
  {
    std::lock_guard<std::mutex> lock(g_sctp_mutex);
    auto it = g_sctp_map.find(fd);
    if (it != g_sctp_map.end()) {
      struct socket* usr_sock = it->second;
      // Remove the entry first so concurrent shim calls fail fast, then close the usrsctp socket. Any upcall
      // firing after this point only writes to a stale pipe (the write end is closed afterwards).
      g_sctp_map.erase(it);
      auto it_w = g_write_fd_map.find(fd);
      int  wfd  = (it_w != g_write_fd_map.end()) ? it_w->second : -1;
      if (it_w != g_write_fd_map.end()) {
        g_write_fd_map.erase(it_w);
      }
      usrsctp_close(usr_sock);
      if (wfd > 0) {
        ::close(wfd);
      }
    }
  }
#endif

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

  // Listen for connections
#if defined(__APPLE__)
  // With one-to-many SOCK_SEQPACKET sockets, usrsctp_listen() enables incoming association requests. The
  // associations are then announced through SCTP_ASSOC_CHANGE notifications, like with the kernel stack.
  auto* so = get_usr_socket(sock_fd);
  int   ret = (so != nullptr) ? usrsctp_listen(so, SOMAXCONN) : -1;
  if (so == nullptr) {
    errno = EBADF;
  }
#else
  int ret = ::listen(sock_fd.value(), SOMAXCONN);
#endif
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
#if defined(__APPLE__)
  // The bridge fd is already non-blocking; make the usrsctp stack non-blocking as well.
  auto* so = get_usr_socket(sock_fd);
  if (so == nullptr) {
    return false;
  }
  usrsctp_set_non_blocking(so, 1);
  return true;
#else
  return ::set_non_blocking(sock_fd, logger);
#endif
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
  if (not set_ipv6_v6only(sock_fd, params.ai_family, if_name, logger)) {
    return false;
  }

#if !defined(__APPLE__)
  // SO_RCVTIMEO does not apply to the user-space stack (its recv path is non-blocking); skip on macOS.
  if (params.rx_timeout.count() > 0) {
    if (not set_receive_timeout(sock_fd, params.rx_timeout, logger)) {
      return false;
    }
  }
#endif

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

#if !defined(__APPLE__)
  // SO_REUSEADDR is a kernel socket option; usrsctp manages port binding in user space. Skip on macOS.
  if (params.reuse_addr) {
    if (not set_reuse_addr(sock_fd, logger)) {
      return false;
    }
  }
#endif

  return true;
}

std::optional<uint16_t> sctp_socket::get_bound_port() const
{
  if (not sock_fd.is_open()) {
    logger.error("Socket of SCTP network gateway not created.");
    return {};
  }

#if defined(__APPLE__)
  // The bridge fd is an AF_UNIX socketpair: get the bound port from the usrsctp socket instead.
  auto* so = get_usr_socket(sock_fd);
  if (so == nullptr) {
    logger.error("{}: Failed to get the bound port: SCTP socket not found for sock_fd={}", if_name, sock_fd.value());
    return {};
  }
  struct sockaddr* laddrs = nullptr;
  int              cnt    = usrsctp_getladdrs(so, 0, &laddrs);
  if (cnt <= 0 or laddrs == nullptr) {
    logger.error("{}: Failed `usrsctp_getladdrs` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 sock_fd.value(),
                 ::strerror(errno));
    return {};
  }
  // On macOS usrsctp packs the returned addresses with their actual sizes.
  const sockaddr* gw_addr = laddrs;
  uint16_t        gw_bound_port;
  if (gw_addr->sa_family == AF_INET) {
    gw_bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(gw_addr)->sin_port);
  } else if (gw_addr->sa_family == AF_INET6) {
    gw_bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(gw_addr)->sin6_port);
  } else {
    usrsctp_freeladdrs(laddrs);
    logger.error("{}: Unhandled address family in SCTP network gateway with sock_fd={} family={}",
                 if_name,
                 sock_fd.value(),
                 gw_addr->sa_family);
    return {};
  }
  usrsctp_freeladdrs(laddrs);
  return gw_bound_port;
#else
  sockaddr_storage gw_addr_storage;
  sockaddr*        gw_addr     = (sockaddr*)&gw_addr_storage;
  socklen_t        gw_addr_len = sizeof(gw_addr_storage);

  int ret = ::getsockname(sock_fd.value(), gw_addr, &gw_addr_len);
  if (ret != 0) {
    logger.error("{}: Failed `getsockname` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 sock_fd.value(),
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
                 sock_fd.value(),
                 gw_addr->sa_family);
    return {};
  }

  return gw_bound_port;
#endif
}

std::optional<int> sctp_socket::get_address_family() const
{
  if (not sock_fd.is_open()) {
    return {};
  }

#if defined(__APPLE__)
  auto* so = get_usr_socket(sock_fd);
  if (so == nullptr) {
    logger.error("{}: Failed to get the address family: SCTP socket not found for sock_fd={}",
                 if_name,
                 sock_fd.value());
    return {};
  }
  struct sockaddr* laddrs = nullptr;
  int              cnt    = usrsctp_getladdrs(so, 0, &laddrs);
  if (cnt <= 0 or laddrs == nullptr) {
    logger.error("{}: Failed `usrsctp_getladdrs` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 sock_fd.value(),
                 ::strerror(errno));
    return {};
  }
  const sockaddr* gw_addr = laddrs;
  int             family  = (gw_addr->sa_family == AF_INET or gw_addr->sa_family == AF_INET6)
                                ? gw_addr->sa_family
                                : 0;
  usrsctp_freeladdrs(laddrs);
  if (family == 0) {
    logger.error("{}: Unhandled address family in SCTP network gateway with sock_fd={} family={}",
                 if_name,
                 sock_fd.value(),
                 gw_addr->sa_family);
    return {};
  }
  return family;
#else
  sockaddr_storage gw_addr_storage;
  sockaddr*        gw_addr     = (sockaddr*)&gw_addr_storage;
  socklen_t        gw_addr_len = sizeof(gw_addr_storage);

  int ret = ::getsockname(sock_fd.value(), gw_addr, &gw_addr_len);
  if (ret != 0) {
    logger.error("{}: Failed `getsockname` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 sock_fd.value(),
                 ::strerror(errno));
    return {};
  }

  if (gw_addr->sa_family == AF_INET || gw_addr->sa_family == AF_INET6) {
    return gw_addr->sa_family;
  }

  return {};
#endif
}
