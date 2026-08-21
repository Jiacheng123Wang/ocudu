// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/gateways/sctp_socket.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/io/sockets.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <thread>
#include <vector>
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
/// Receive timeout requested through SO_RCVTIMEO, per bridge fd (0 = block until a message arrives).
static std::unordered_map<int, std::chrono::milliseconds> g_rx_timeout_map;
static std::mutex                              g_sctp_mutex;

/// SCTP-over-UDP encapsulation port in use, or 0 when SCTP packets are sent natively over IP (raw sockets).
static uint16_t g_udp_encaps_port = 0;

/// Default SCTP-over-UDP tunneling port (IANA-assigned for SCTP encapsulation, RFC 6951).
static constexpr uint16_t default_udp_tunneling_port = 9899;

/// \brief Returns true if this process may open a raw SCTP socket.
///
/// usrsctp sends SCTP packets natively over IP through a raw socket, which macOS only allows to root. Without it the
/// INIT chunks never leave the process, every association attempt ends in SCTP_CANT_STR_ASSOC and any code waiting
/// for an association blocks until the INIT retransmissions give up.
static bool raw_sctp_socket_available()
{
  int fd = ::socket(AF_INET, SOCK_RAW, IPPROTO_SCTP);
  if (fd < 0) {
    return false;
  }
  ::close(fd);
  return true;
}

/// Returns a free UDP port for SCTP-over-UDP encapsulation, starting at the IANA-assigned one.
static uint16_t pick_udp_tunneling_port()
{
  for (uint16_t port = default_udp_tunneling_port; port < default_udp_tunneling_port + 16; ++port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      return default_udp_tunneling_port;
    }
    sockaddr_in addr = {};
    addr.sin_len     = sizeof(addr);
    addr.sin_family  = AF_INET;
    addr.sin_port    = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bool free_port       = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    if (free_port) {
      return port;
    }
  }
  return default_udp_tunneling_port;
}

/// Initializes the usrsctp library once per process (creates its internal timer and worker threads).
static void usrsctp_once_init()
{
  static std::once_flag init_flag;
  std::call_once(init_flag, []() {
    ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("SCTP-GW");

    // usrsctp inherits the FreeBSD defaults (initial RTO 3 s, min 1 s), which are far too slow for the local links
    // this stack is used on: a single lost chunk stalls an association setup or shutdown for seconds. Use defaults in
    // the same range as the ocudu SCTP configuration (rto_initial=120 ms, rto_max=500 ms in the shipped configs); an
    // explicit per-socket configuration still overrides them through SCTP_RTOINFO.
    usrsctp_sysctl_set_sctp_rto_initial_default(500);
    usrsctp_sysctl_set_sctp_rto_min_default(100);
    usrsctp_sysctl_set_sctp_rto_max_default(6000);
    usrsctp_sysctl_set_sctp_init_rto_max_default(6000);

    if (raw_sctp_socket_available()) {
      // Root: keep the wire format of a kernel SCTP stack (plain SCTP over IP).
      usrsctp_init(0, nullptr, nullptr);
      logger.info("usrsctp initialized with native SCTP packets (raw sockets available)");
    } else {
      // Unprivileged process: raw sockets are not permitted, so tunnel SCTP over UDP (RFC 6951). This is what makes
      // loopback associations - and therefore the SCTP unit tests - work without root. A remote peer must use the
      // same encapsulation port (Linux: sysctl net.sctp.udp_port, or SCTP_REMOTE_UDP_ENCAPS_PORT).
      g_udp_encaps_port = pick_udp_tunneling_port();
      usrsctp_init(g_udp_encaps_port, nullptr, nullptr);
      logger.info("usrsctp initialized with SCTP-over-UDP encapsulation on port {} (no permission to open a raw "
                  "socket; run as root for native SCTP packets)",
                  g_udp_encaps_port);
    }
  });
}

uint16_t sctp_udp_encapsulation_port(void)
{
  return g_udp_encaps_port;
}

/// \brief Fills the BSD sockaddr length field of every address of a packed address array.
///
/// macOS sockaddr structures carry an sa_len field and usrsctp (built with HAVE_SA_LEN) validates it: usrsctp_bindx()
/// fails with EINVAL when it is left at 0, which is how the shared (Linux-oriented) code fills its addresses.
static void fill_sockaddr_lengths(struct sockaddr* addrs, int addrcnt)
{
  auto* ptr = reinterpret_cast<uint8_t*>(addrs);
  for (int i = 0; i != addrcnt; ++i) {
    auto*     sa   = reinterpret_cast<struct sockaddr*>(ptr);
    socklen_t size = (sa->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    sa->sa_len     = size;
    ptr += size;
  }
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

/// \brief Shuts every association of a usrsctp socket down gracefully.
///
/// Closing a kernel SCTP socket makes the peer observe SCTP_SHUTDOWN_EVENT followed by SCTP_SHUTDOWN_COMP, whereas
/// usrsctp_close() alone sends an ABORT and the peer observes SCTP_COMM_LOST. Emulate the kernel behaviour by sending
/// a zero-length message with the SCTP_EOF flag on each association and waiting (briefly) for the SHUTDOWN handshake
/// to complete, because usrsctp drops the association state as soon as the socket is closed.
static void
shutdown_associations_gracefully(struct socket* so, const std::string& if_name, ocudulog::basic_logger& logger)
{
  uint32_t  nof_assocs = 0;
  socklen_t opt_len    = sizeof(nof_assocs);
  if (usrsctp_getsockopt(so, IPPROTO_SCTP, SCTP_GET_ASSOC_NUMBER, &nof_assocs, &opt_len) != 0 or nof_assocs == 0) {
    return;
  }

  std::vector<uint8_t> id_buffer(sizeof(struct sctp_assoc_ids) + nof_assocs * sizeof(sctp_assoc_t));
  auto*                assoc_ids = reinterpret_cast<struct sctp_assoc_ids*>(id_buffer.data());
  socklen_t            ids_len   = id_buffer.size();
  if (usrsctp_getsockopt(so, IPPROTO_SCTP, SCTP_GET_ASSOC_ID_LIST, assoc_ids, &ids_len) != 0) {
    logger.debug(
        "{}: Could not list the SCTP associations to shut them down gracefully: {}", if_name, ::strerror(errno));
    return;
  }

  for (uint32_t i = 0; i != assoc_ids->gaids_number_of_ids; ++i) {
    struct sctp_sndinfo sndinfo = {};
    sndinfo.sinfo_assoc_id      = assoc_ids->gaids_assoc_id[i];
    sndinfo.sinfo_flags         = SCTP_EOF;
    // usrsctp rejects a null data pointer, so send a zero-length message from a dummy buffer.
    const uint8_t eof_payload = 0;
    if (usrsctp_sendv(so, &eof_payload, 0, nullptr, 0, &sndinfo, sizeof(sndinfo), SCTP_SENDV_SNDINFO, 0) < 0) {
      logger.debug("{}: Failed to send SCTP EOF for assoc={}: {}",
                   if_name,
                   assoc_ids->gaids_assoc_id[i],
                   ::strerror(errno));
    }
  }

  // Wait for the associations to disappear, i.e. for the SHUTDOWN handshake to complete. Over a local link this takes
  // well under a millisecond; the cap only protects against an unresponsive peer.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < deadline) {
    uint32_t  remaining = 0;
    socklen_t len       = sizeof(remaining);
    if (usrsctp_getsockopt(so, IPPROTO_SCTP, SCTP_GET_ASSOC_NUMBER, &remaining, &len) != 0 or remaining == 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  logger.debug("{}: Timed out waiting for the SCTP associations to shut down gracefully", if_name);
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
  // mirroring the kernel's sctp_bindx(). It requires the BSD sa_len field, which the shared code does not set.
  fill_sockaddr_lengths(addrs, addrcnt);
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
  fill_sockaddr_lengths(addrs, 1);
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
  // usrsctp cannot send from a null data pointer (it fails with EFAULT): use a dummy payload for zero-length
  // messages (e.g., the SCTP_EOF messages sent during association shutdown).
  static const uint8_t dummy_payload = 0;
  if (msg == nullptr && len == 0) {
    msg = &dummy_payload;
  }
  struct sctp_sndinfo sinfo = {};
  sinfo.sinfo_ppid           = ppid;
  sinfo.sinfo_flags          = flags;
  sinfo.sinfo_stream         = stream_no;
  sinfo.sinfo_context        = context;
  if (to != nullptr) {
    fill_sockaddr_lengths(to, 1);
  }
  return usrsctp_sendv(
      it->second, msg, len, to, (to != nullptr) ? 1 : 0, &sinfo, sizeof(sinfo), SCTP_SENDV_SNDINFO, 0);
}

int sctp_recvmsg(int s, void* msg, size_t len, struct sockaddr* from, socklen_t* fromlen, void* sinfo,
                 socklen_t* sinfo_len, int* msg_flags)
{
  // usrsctp ignores SO_RCVTIMEO (its internal sbwait() is a plain condition wait), so a blocking usrsctp_recvv()
  // would never return when the peer sends nothing. Emulate the Linux SO_RCVTIMEO semantics: poll the socket with
  // MSG_DONTWAIT - which usrsctp does honour even on a blocking socket - and wait on the bridge socketpair (written
  // by the upcall) in between. A configured rx_timeout that expires reports EAGAIN, exactly like a kernel SCTP
  // socket with SO_RCVTIMEO, and a socket set to non-blocking mode never waits at all.
  std::chrono::milliseconds timeout{0};
  {
    std::lock_guard<std::mutex> lock(g_sctp_mutex);
    auto                        it_to = g_rx_timeout_map.find(s);
    if (it_to != g_rx_timeout_map.end()) {
      timeout = it_to->second;
    }
  }
  const auto deadline    = std::chrono::steady_clock::now() + timeout;
  const bool has_timeout = timeout.count() > 0;

  for (;;) {
    bool non_blocking = false;
    {
      std::lock_guard<std::mutex> lock(g_sctp_mutex);
      auto                        it = g_sctp_map.find(s);
      if (it == g_sctp_map.end()) {
        errno = EBADF;
        return -1;
      }
      non_blocking = usrsctp_get_non_blocking(it->second) != 0;

      struct sctp_rcvinfo rsinfo     = {};
      socklen_t           rsinfo_len = sizeof(rsinfo);
      unsigned int        infotype   = 0;
      // Note: infotype must not be null: usrsctp_recvv() writes it on the data path. MSG_DONTWAIT guarantees that
      // the call returns immediately, so the shim lock is never held while waiting.
      int flags  = MSG_DONTWAIT;
      int result = usrsctp_recvv(it->second, msg, len, from, fromlen, &rsinfo, &rsinfo_len, &infotype, &flags);
      if (result >= 0) {
        if (msg_flags != nullptr) {
          *msg_flags = flags & ~MSG_DONTWAIT;
        }
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
      if (errno != EAGAIN and errno != EWOULDBLOCK and errno != EINTR) {
        return -1;
      }
    }

    if (non_blocking) {
      errno = EAGAIN;
      return -1;
    }

    // Nothing to read yet: wait on the bridge socketpair, which the usrsctp upcall writes to. The wait is capped so
    // that a lost wake-up byte (the write end is non-blocking) only delays the next read attempt.
    int wait_ms = 50;
    if (has_timeout) {
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        errno = EAGAIN;
        return -1;
      }
      wait_ms = static_cast<int>(std::min<int64_t>(remaining.count(), wait_ms));
    }
    struct pollfd pfd = {};
    pfd.fd            = s;
    pfd.events        = POLLIN;
    int poll_ret      = ::poll(&pfd, 1, wait_ms);
    if (poll_ret < 0 and errno != EINTR) {
      return -1;
    }
    if (poll_ret > 0 and (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
      errno = EBADF;
      return -1;
    }
  }
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

int sctp_getladdrs(int s, uint32_t assoc_id, struct sockaddr** addrs)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_getladdrs(it->second, assoc_id, addrs);
}

void sctp_freeladdrs(struct sockaddr* addrs)
{
  usrsctp_freeladdrs(addrs);
}

int sctp_getsockopt(int s, int level, int optname, void* optval, socklen_t* optlen)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_getsockopt(it->second, level, optname, optval, optlen);
}

int sctp_setsockopt(int s, int level, int optname, const void* optval, socklen_t optlen)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_setsockopt(it->second, level, optname, optval, optlen);
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
#if defined(__APPLE__)
  // SCTP_DATA_IO_EVENT is Linux-specific (it makes the kernel deliver a sctp_sndrcvinfo with every message). The
  // usrsctp equivalent is the SCTP_RECVRCVINFO socket option: without it usrsctp_recvv() reports SCTP_RECVV_NOINFO
  // and leaves the sctp_rcvinfo - and therefore rcv_assoc_id - untouched, so the receiver cannot tell which
  // association a message belongs to ("Received data on unknown SCTP association").
  int recvrcvinfo_on = 1;
  if (sctp_setsockopt_shim(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &recvrcvinfo_on, sizeof(recvrcvinfo_on)) != 0) {
    return false;
  }
#else
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

#if defined(__APPLE__)
  // usrsctp inherits the FreeBSD validation of SCTP_RTOINFO and rejects the whole option (EINVAL) unless
  // srto_min <= srto_initial <= srto_max, while the Linux kernel accepts any combination. Clamp the initial RTO into
  // the requested window so that a Linux-tuned configuration still applies on macOS.
  if (rto_opts.srto_min > rto_opts.srto_max) {
    logger.warning("{}: SCTP rto_min={} is larger than rto_max={}; swapping them for usrsctp",
                   if_name,
                   rto_opts.srto_min,
                   rto_opts.srto_max);
    std::swap(rto_opts.srto_min, rto_opts.srto_max);
  }
  uint32_t clamped_initial = std::min(std::max(rto_opts.srto_initial, rto_opts.srto_min), rto_opts.srto_max);
  if (clamped_initial != rto_opts.srto_initial) {
    logger.warning("{}: SCTP rto_initial={} is outside [rto_min={}, rto_max={}]; using {} (usrsctp requires "
                   "rto_min <= rto_initial <= rto_max)",
                   if_name,
                   rto_opts.srto_initial,
                   rto_opts.srto_min,
                   rto_opts.srto_max,
                   clamped_initial);
    rto_opts.srto_initial = clamped_initial;
  }
#endif

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
  // usrsctp implements SCTP_INITMSG, but the option must be routed to the usrsctp socket instead of the bridge fd.
  sctp_initmsg init_opts = {};
  socklen_t    init_sz   = sizeof(sctp_initmsg);
  if (sctp_getsockopt_shim(fd, IPPROTO_SCTP, SCTP_INITMSG, &init_opts, &init_sz) < 0) {
    logger.error("{}: Error getting SCTP_INITMSG sockopts. errno={}", if_name, ::strerror(errno));
    return false;
  }
#else
  // Set SCTP INITMSG options to reduce blocking timeout of connect()
  sctp_initmsg init_opts = {};
  socklen_t    init_sz   = sizeof(sctp_initmsg);
  if (sctp_getsockopt_shim(fd, SOL_SCTP, SCTP_INITMSG, &init_opts, &init_sz) < 0) {
    logger.error("{}: Error getting sockopts. errno={}", if_name, ::strerror(errno));
    return false; // Responsibility of closing the socket is on the caller
  }
#endif

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
  return sctp_setsockopt_shim(fd, IPPROTO_SCTP, SCTP_NODELAY, &optval, sizeof(optval)) == 0;
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
  // that the upcall never blocks the usrsctp stack. SO_NOSIGPIPE is also needed: when the read end is closed while
  // the stack still has events pending, the upcall's write() would otherwise raise SIGPIPE and kill the process
  // (macOS has no MSG_NOSIGNAL).
  for (int fd : {sv[0], sv[1]}) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 or ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      socket.logger.error("{}: Failed to set bridge socketpair non-blocking: {}", socket.if_name, ::strerror(errno));
      ::close(sv[0]);
      ::close(sv[1]);
      usrsctp_close(usr_sock);
      return make_unexpected(default_error_t{});
    }
    int nosigpipe = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) < 0) {
      socket.logger.error("{}: Failed to set SO_NOSIGPIPE on the bridge socketpair: {}",
                          socket.if_name,
                          ::strerror(errno));
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
    // usrsctp does not honour SO_RCVTIMEO, so sctp_recvmsg() emulates it (see there).
    g_rx_timeout_map[sv[0]] = std::chrono::duration_cast<std::chrono::milliseconds>(params.rx_timeout);
  }

  if (g_udp_encaps_port != 0) {
    // Tunnel the outgoing SCTP packets of every future association over UDP. Incoming associations do not need this:
    // the stack learns the peer encapsulation port from the received packets.
    struct sctp_udpencaps encaps = {};
    encaps.sue_assoc_id          = SCTP_FUTURE_ASSOC;
    encaps.sue_port              = htons(g_udp_encaps_port);
    if (usrsctp_setsockopt(usr_sock, IPPROTO_SCTP, SCTP_REMOTE_UDP_ENCAPS_PORT, &encaps, sizeof(encaps)) < 0) {
      socket.logger.warning("{}: Failed to enable SCTP-over-UDP encapsulation on port {}: {}",
                            socket.if_name,
                            g_udp_encaps_port,
                            ::strerror(errno));
    }
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
  int            fd       = sock_fd.value();
  struct socket* usr_sock = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_sctp_mutex);
    auto                        it = g_sctp_map.find(fd);
    if (it != g_sctp_map.end()) {
      usr_sock = it->second;
    }
  }
  if (usr_sock != nullptr) {
    // Mirror the kernel: closing the socket shuts the associations down gracefully. Done outside the shim lock, as
    // it waits for the SHUTDOWN handshake.
    shutdown_associations_gracefully(usr_sock, if_name, logger);

    std::lock_guard<std::mutex> lock(g_sctp_mutex);
    // Remove the entries first so concurrent shim calls fail fast, then close the usrsctp socket. Any upcall firing
    // after this point only writes to a stale pipe (the write end is closed afterwards).
    g_sctp_map.erase(fd);
    g_rx_timeout_map.erase(fd);
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

#if defined(__APPLE__)
  // The receive timeout is emulated by sctp_recvmsg() (usrsctp ignores SO_RCVTIMEO) and was registered for this fd
  // when the socket was created, so there is nothing to set here.
  if (params.rx_timeout.count() > 0) {
    logger.debug(
        "{}: SCTP receive timeout of {} s is emulated by the usrsctp shim", if_name, params.rx_timeout.count());
  }
#else
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
    if (errno == ENOTCONN or errno == EINVAL) {
      // Socket is open but not bound yet. The kernel SCTP stack answers getsockname() with port 0 in this case, so
      // report the same to the shared code instead of an error.
      logger.debug("{}: SCTP socket with sock_fd={} is not bound yet, reporting port 0", if_name, sock_fd.value());
      return 0;
    }
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
