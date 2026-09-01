// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \brief macOS backend of the SCTP gateway family: the usrsctp user-space stack.
///
/// macOS has no native SCTP support. usrsctp sockets are "struct socket*"
/// pointers rather than kernel file descriptors, so this shim bridges them to
/// the fd-based code (and to the kqueue io_broker) as follows:
///   - a socketpair is created for every SCTP socket. Its read end is the fd
///     handed out to the rest of the code and registered with the io_broker;
///   - the write end is passed as user data to usrsctp_set_upcall(): whenever
///     the stack has data or events pending on the socket, the upcall writes
///     one byte, which wakes the broker (EVFILT_READ on the read end);
///   - sctp_recvmsg() consumes one byte after every usrsctp_recvv() call.

#include "sctp_socket_backend.h"
#include "sctp_network_server_impl.h" // subscribe_association_to_broker (macOS stub, never called)
#include "ocudu/gateways/sctp_types.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <usrsctp.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::sctp_backend;

namespace {

std::unordered_map<int, struct socket*> g_sctp_map;
std::unordered_map<int, int>            g_write_fd_map;
/// Receive timeout requested through SO_RCVTIMEO, per bridge fd (0 = block until a message arrives).
std::unordered_map<int, std::chrono::milliseconds> g_rx_timeout_map;
std::mutex                                         g_sctp_mutex;

/// SCTP-over-UDP encapsulation port in use, or 0 when SCTP packets are sent natively over IP (raw sockets).
uint16_t g_udp_encaps_port = 0;

/// Default SCTP-over-UDP tunneling port (IANA-assigned for SCTP encapsulation, RFC 6951).
constexpr uint16_t default_udp_tunneling_port = 9899;

/// \brief Returns true if this process may open a raw SCTP socket.
///
/// usrsctp sends SCTP packets natively over IP through a raw socket, which macOS only allows to root. Without it the
/// INIT chunks never leave the process, every association attempt ends in SCTP_CANT_STR_ASSOC and any code waiting
/// for an association blocks until the INIT retransmissions give up.
bool raw_sctp_socket_available()
{
  int fd = ::socket(AF_INET, SOCK_RAW, IPPROTO_SCTP);
  if (fd < 0) {
    return false;
  }
  ::close(fd);
  return true;
}

/// Returns a free UDP port for SCTP-over-UDP encapsulation, starting at the IANA-assigned one.
///
/// The first candidate is offset by the PID: concurrently starting processes (e.g. a parallel ctest run) each probe
/// 9899 as free in the gap between the probe close and the usrsctp bind, and the shared UDP socket has no
/// SO_REUSEADDR, so one of them ends up with a failed bind and all its associations stall. Every association of a
/// process runs over that process's single shared socket, so peers never require a specific port (the fixed IANA
/// port only matters for cross-machine interop with kernel-SCTP peers, which is not wired up yet).
uint16_t pick_udp_tunneling_port()
{
  const uint16_t start = default_udp_tunneling_port + static_cast<uint16_t>(::getpid() % 1000);
  for (uint16_t port = start; port < start + 32; ++port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      return start;
    }
    sockaddr_in addr        = {};
    addr.sin_len            = sizeof(addr);
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(port);
    addr.sin_addr.s_addr    = htonl(INADDR_ANY);
    bool free_port          = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    if (free_port) {
      return port;
    }
  }
  return start;
}

/// Initializes the usrsctp library once per process (creates its internal timer and worker threads).
void usrsctp_once_init()
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

    // OCUDU_USRSCTP_MODE overrides the automatic transport selection:
    //   auto (default): native SCTP over IP when the process may open a raw socket (root), otherwise
    //                   SCTP-over-UDP encapsulation (RFC 6951), which works unprivileged;
    //   udp: force SCTP-over-UDP encapsulation (e.g. for `sudo ctest -L sctp`, because macOS does not
    //        loop native SCTP packets back to a local raw socket);
    //   raw: force native SCTP over IP (requires root; associations fail without it).
    const char* mode_override = ::getenv("OCUDU_USRSCTP_MODE");
    const std::string mode    = (mode_override == nullptr) ? "auto" : std::string(mode_override);
    const bool        raw_permitted = raw_sctp_socket_available();
    bool              use_raw       = raw_permitted;
    if (mode == "udp") {
      use_raw = false;
    } else if (mode == "raw") {
      use_raw = true;
    } else if (mode != "auto") {
      logger.warning("OCUDU_USRSCTP_MODE='{}' is unknown (expected 'auto', 'udp' or 'raw'); using automatic "
                     "selection",
                     mode);
      use_raw = raw_permitted;
    }

    if (use_raw) {
      if (not raw_permitted) {
        logger.warning("OCUDU_USRSCTP_MODE=raw was requested but this process may not open a raw socket; "
                       "association attempts will fail. Run as root, or use mode 'auto'/'udp'.");
      }
      // Root: keep the wire format of a kernel SCTP stack (plain SCTP over IP).
      usrsctp_init(0, nullptr, nullptr);
      logger.info("usrsctp initialized with native SCTP packets (raw sockets; mode='{}')", mode);
    } else {
      // Unprivileged process: raw sockets are not permitted, so tunnel SCTP over UDP (RFC 6951). This is what makes
      // loopback associations - and therefore the SCTP unit tests - work without root. A remote peer must use the
      // same encapsulation port (Linux: sysctl net.sctp.udp_port, or SCTP_REMOTE_UDP_ENCAPS_PORT).
      g_udp_encaps_port = pick_udp_tunneling_port();
      usrsctp_init(g_udp_encaps_port, nullptr, nullptr);
      logger.info("usrsctp initialized with SCTP-over-UDP encapsulation on port {} (mode='{}'; no root needed)",
                  g_udp_encaps_port, mode);
    }
  });
}

/// \brief Fills the BSD sockaddr length field of every address of a packed address array.
///
/// macOS sockaddr structures carry an sa_len field and usrsctp (built with HAVE_SA_LEN) validates it: usrsctp_bindx()
/// fails with EINVAL when it is left at 0, which is how the shared (Linux-oriented) code fills its addresses.
void fill_sockaddr_lengths(struct sockaddr* addrs, int addrcnt)
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
struct socket* get_usr_socket(const unique_fd& fd)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(fd.value());
  return (it != g_sctp_map.end()) ? it->second : nullptr;
}

/// get/setsockopt shims: SCTP options must be applied to the usrsctp socket, never to the AF_UNIX bridge
/// socketpair.
int sctp_getsockopt_shim_impl(const unique_fd& fd, int level, int option_name, void* optval, socklen_t* optlen)
{
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_getsockopt(so, level, option_name, optval, optlen);
}

int sctp_setsockopt_shim_impl(const unique_fd& fd, int level, int option_name, const void* optval, socklen_t optlen)
{
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_setsockopt(so, level, option_name, optval, optlen);
}

socklen_t sockaddr_actual_size(const sockaddr* addr)
{
  return (addr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

/// Called by the usrsctp stack when data, events or errors are pending on the socket. Wakes up the io_broker by
/// writing a byte to the bridge socketpair.
void my_upcall_func(struct socket* sock, void* addr, int flags)
{
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
void shutdown_associations_gracefully(struct socket* so, const std::string& if_name, ocudulog::basic_logger& logger)
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
  // a few milliseconds; poll with a short interval so that the wait returns as soon as the last association is gone
  // instead of always burning the whole budget (important when many sockets are closed back to back, e.g. the
  // 32-client multi-client tests). The cap only protects against an unresponsive peer.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (std::chrono::steady_clock::now() < deadline) {
    uint32_t  remaining = 0;
    socklen_t len       = sizeof(remaining);
    if (usrsctp_getsockopt(so, IPPROTO_SCTP, SCTP_GET_ASSOC_NUMBER, &remaining, &len) != 0 or remaining == 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  logger.debug("{}: Timed out waiting for the SCTP associations to shut down gracefully", if_name);
}

} // namespace

// The Linux kernel SCTP API surface (see sctp_types.h) exported by the shim so the shared gateway code calls the
// same functions on both platforms.

extern "C" int sctp_bindx(int s, struct sockaddr* addrs, int addrcnt, int flags)
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

extern "C" int sctp_connectx(int s, struct sockaddr* addrs, int addrcnt, uint32_t* id)
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

extern "C" int sctp_sendmsg(int s, const void* msg, size_t len, struct sockaddr* to, socklen_t tolen,
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
  // usrsctp rejects a null data pointer (it fails with EFAULT), but the length must stay 0: sending a 1-byte dummy
  // payload would put data on the wire, and in the SCTP_EOF case the peer would deliver a bogus 1-byte SDU instead of
  // a clean SHUTDOWN_EVENT + SHUTDOWN_COMP sequence (this happened with the shim's previous behaviour). A dummy
  // pointer with length 0 produces a true zero-length message.
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

extern "C" int sctp_recvmsg(int s, void* msg, size_t len, struct sockaddr* from, socklen_t* fromlen, void* sinfo,
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

extern "C" int sctp_recvmsg_nowait(int s, void* msg, size_t len, struct sockaddr* from, socklen_t* fromlen,
                                   void* sinfo, socklen_t* sinfo_len, int* msg_flags)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  struct sctp_rcvinfo rsinfo     = {};
  socklen_t           rsinfo_len = sizeof(rsinfo);
  unsigned int        infotype   = 0;
  int                 flags      = MSG_DONTWAIT;
  int result = usrsctp_recvv(it->second, msg, len, from, fromlen, &rsinfo, &rsinfo_len, &infotype, &flags);
  if (result < 0) {
    return -1;
  }
  if (msg_flags != nullptr) {
    *msg_flags = flags & ~MSG_DONTWAIT;
  }
  if (sinfo != nullptr && sinfo_len != nullptr && *sinfo_len >= sizeof(struct sctp_rcvinfo)) {
    ::memcpy(sinfo, &rsinfo, sizeof(struct sctp_rcvinfo));
  }
  uint8_t dummy;
  while (::read(s, &dummy, sizeof(dummy)) < 0 and errno == EINTR) {
  }
  return result;
}

extern "C" int sctp_getpaddrs(int s, uint32_t assoc_id, struct sockaddr** addrs)
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

extern "C" void sctp_freepaddrs(struct sockaddr* addrs)
{
  // usrsctp_freepaddrs() unwinds the hidden sctp_getaddresses header.
  usrsctp_freepaddrs(addrs);
}

extern "C" int sctp_getladdrs(int s, uint32_t assoc_id, struct sockaddr** addrs)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_getladdrs(it->second, assoc_id, addrs);
}

extern "C" void sctp_freeladdrs(struct sockaddr* addrs)
{
  usrsctp_freeladdrs(addrs);
}

extern "C" int sctp_getsockopt(int s, int level, int optname, void* optval, socklen_t* optlen)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_getsockopt(it->second, level, optname, optval, optlen);
}

extern "C" int sctp_setsockopt(int s, int level, int optname, const void* optval, socklen_t optlen)
{
  std::lock_guard<std::mutex> lock(g_sctp_mutex);
  auto                        it = g_sctp_map.find(s);
  if (it == g_sctp_map.end()) {
    errno = EBADF;
    return -1;
  }
  return usrsctp_setsockopt(it->second, level, optname, optval, optlen);
}

extern "C" uint16_t sctp_udp_encapsulation_port(void)
{
  return g_udp_encaps_port;
}

// Backend implementation.

struct addrinfo sctp_backend::make_sctp_addrinfo_hints()
{
  // macOS has no native SCTP support: getaddrinfo() rejects the IPPROTO_SCTP protocol hint (EAI_BADFLAGS).
  // Resolve the address without transport protocol constraints; the resulting sockaddr is protocol-agnostic and
  // is only used to configure the usrsctp stack.
  struct addrinfo hints = {};
  // Support ipv4, ipv6 and hostnames.
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;
  return hints;
}

expected<unique_fd> sctp_backend::create_socket(const sctp_socket_params& params,
                                               const std::string&        if_name,
                                               ocudulog::basic_logger&   logger)
{
  usrsctp_once_init();

  // One-to-many style (SOCK_SEQPACKET), like the Linux kernel SCTP sockets: associations are established and
  // tracked via notifications on this single socket, so no accept() is required. Data is delivered through the
  // recv path (the receive_cb is left null) and the upcall wakes the io_broker.
  struct socket* usr_sock =
      usrsctp_socket(params.ai_family, SOCK_SEQPACKET, IPPROTO_SCTP, nullptr, nullptr, 0, nullptr);
  if (usr_sock == nullptr) {
    logger.error("{}: usrsctp_socket failed", if_name);
    return make_unexpected(default_error_t{});
  }

  // Bridge to the fd-based code and the kqueue broker: the read end is handed out as the "fd" and registered with
  // the io_broker, the write end is woken by the upcall whenever the stack has data or events for the socket.
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    logger.error("{}: socketpair failed: {}", if_name, ::strerror(errno));
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
      logger.error("{}: Failed to set bridge socketpair non-blocking: {}", if_name, ::strerror(errno));
      ::close(sv[0]);
      ::close(sv[1]);
      usrsctp_close(usr_sock);
      return make_unexpected(default_error_t{});
    }
    int nosigpipe = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) < 0) {
      logger.error("{}: Failed to set SO_NOSIGPIPE on the bridge socketpair: {}",
                   if_name,
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
    g_sctp_map[sv[0]]              = usr_sock;
    g_write_fd_map[sv[0]]          = sv[1];
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
      logger.warning("{}: Failed to enable SCTP-over-UDP encapsulation on port {}: {}",
                     if_name,
                     g_udp_encaps_port,
                     ::strerror(errno));
    }
  }

  return unique_fd{sv[0]};
}

void sctp_backend::prepare_close(const unique_fd& sock_fd, const std::string& if_name, ocudulog::basic_logger& logger)
{
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
}

int sctp_backend::getsockopt_shim(const unique_fd& fd, int level, int option_name, void* optval, socklen_t* optlen)
{
  return sctp_getsockopt_shim_impl(fd, level, option_name, optval, optlen);
}

int sctp_backend::setsockopt_shim(const unique_fd& fd, int level, int option_name, const void* optval, socklen_t optlen)
{
  return sctp_setsockopt_shim_impl(fd, level, option_name, optval, optlen);
}

bool sctp_backend::enable_per_message_info(const unique_fd& fd)
{
  // usrsctp equivalent of the Linux SCTP_DATA_IO_EVENT subscription: without it usrsctp_recvv() reports
  // SCTP_RECVV_NOINFO and leaves the sctp_rcvinfo - and therefore rcv_assoc_id - untouched, so the receiver cannot
  // tell which association a message belongs to ("Received data on unknown SCTP association").
  int recvrcvinfo_on = 1;
  return sctp_setsockopt_shim_impl(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &recvrcvinfo_on, sizeof(recvrcvinfo_on)) == 0;
}

void sctp_backend::adjust_rto_opts(sctp_rtoinfo& rto_opts, const std::string& if_name, ocudulog::basic_logger& logger)
{
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
}

bool sctp_backend::get_init_msg_opts(const unique_fd&        fd,
                                     sctp_initmsg&           init_opts,
                                     socklen_t&              init_sz,
                                     const std::string&      if_name,
                                     ocudulog::basic_logger& logger)
{
  // usrsctp implements SCTP_INITMSG, but the option must be routed to the usrsctp socket instead of the bridge fd.
  if (sctp_getsockopt_shim_impl(fd, IPPROTO_SCTP, SCTP_INITMSG, &init_opts, &init_sz) < 0) {
    logger.error("{}: Error getting SCTP_INITMSG sockopts. errno={}", if_name, ::strerror(errno));
    return false;
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
  // The bridge socketpair must not be touched; apply the option to the usrsctp socket on a best-effort basis.
  if (sctp_setsockopt_shim_impl(fd, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
    logger.debug(
        "{}: Failed to set IPV6_V6ONLY=0 on the usrsctp socket (ignored). errno={}", if_name, ::strerror(errno));
  }
  return true;
}

bool sctp_backend::apply_receive_timeout(const unique_fd&         /*fd*/,
                                         const sctp_socket_params& params,
                                         const std::string&       if_name,
                                         ocudulog::basic_logger&  logger)
{
  // The receive timeout is emulated by sctp_recvmsg() (usrsctp ignores SO_RCVTIMEO) and was registered for this fd
  // when the socket was created, so there is nothing to set here.
  if (params.rx_timeout.count() > 0) {
    logger.debug(
        "{}: SCTP receive timeout of {} s is emulated by the usrsctp shim", if_name, params.rx_timeout.count());
  }
  return true;
}

bool sctp_backend::apply_reuse_addr(const unique_fd& /*fd*/,
                                    const sctp_socket_params& /*params*/,
                                    ocudulog::basic_logger& /*logger*/)
{
  // SO_REUSEADDR is a kernel socket option; usrsctp manages port binding in user space. Skip on macOS.
  return true;
}

int sctp_backend::listen_socket(const unique_fd& fd)
{
  // With one-to-many SOCK_SEQPACKET sockets, usrsctp_listen() enables incoming association requests. The
  // associations are then announced through SCTP_ASSOC_CHANGE notifications, like with the kernel stack.
  auto* so = get_usr_socket(fd);
  int   ret = (so != nullptr) ? usrsctp_listen(so, SOMAXCONN) : -1;
  if (so == nullptr) {
    errno = EBADF;
  }
  return ret;
}

bool sctp_backend::set_non_blocking(const unique_fd& fd, ocudulog::basic_logger& /*logger*/)
{
  // The bridge fd is already non-blocking; make the usrsctp stack non-blocking as well.
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    return false;
  }
  usrsctp_set_non_blocking(so, 1);
  return true;
}

std::optional<uint16_t> sctp_backend::get_bound_port(const unique_fd&      fd,
                                                     const std::string&    if_name,
                                                     ocudulog::basic_logger& logger)
{
  // The bridge fd is an AF_UNIX socketpair: get the bound port from the usrsctp socket instead.
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    logger.error("{}: Failed to get the bound port: SCTP socket not found for sock_fd={}", if_name, fd.value());
    return {};
  }
  struct sockaddr* laddrs = nullptr;
  int              cnt    = usrsctp_getladdrs(so, 0, &laddrs);
  if (cnt <= 0 or laddrs == nullptr) {
    if (errno == ENOTCONN or errno == EINVAL) {
      // Socket is open but not bound yet. The kernel SCTP stack answers getsockname() with port 0 in this case, so
      // report the same to the shared code instead of an error.
      logger.debug("{}: SCTP socket with sock_fd={} is not bound yet, reporting port 0", if_name, fd.value());
      return 0;
    }
    logger.error("{}: Failed `usrsctp_getladdrs` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 fd.value(),
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
                 fd.value(),
                 gw_addr->sa_family);
    return {};
  }
  usrsctp_freeladdrs(laddrs);
  return gw_bound_port;
}

std::optional<int> sctp_backend::get_address_family(const unique_fd&      fd,
                                                    const std::string&    if_name,
                                                    ocudulog::basic_logger& logger)
{
  auto* so = get_usr_socket(fd);
  if (so == nullptr) {
    logger.error("{}: Failed to get the address family: SCTP socket not found for sock_fd={}",
                 if_name,
                 fd.value());
    return {};
  }
  struct sockaddr* laddrs = nullptr;
  int              cnt    = usrsctp_getladdrs(so, 0, &laddrs);
  if (cnt <= 0 or laddrs == nullptr) {
    logger.error("{}: Failed `usrsctp_getladdrs` in SCTP network gateway with sock_fd={}: {}",
                 if_name,
                 fd.value(),
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
                 fd.value(),
                 gw_addr->sa_family);
    return {};
  }
  return family;
}

int sctp_backend::receive_available(int fd, message_sink sink, void* user)
{
  std::array<uint8_t, sctp_backend::max_sctp_msg_len> temp_recv_buffer;
  struct sctp_sndrcvinfo sri       = {};
  socklen_t              sri_len   = sizeof(sri);
  int                    msg_flags = 0;

  // fromlen is an in/out variable in sctp_recvmsg.
  sockaddr_storage msg_src_addr;
  socklen_t        msg_src_addrlen = sizeof(msg_src_addr);

  int nof_msgs = 0;

  int rx_bytes = ::sctp_recvmsg(fd,
                                temp_recv_buffer.data(),
                                temp_recv_buffer.size(),
                                reinterpret_cast<sockaddr*>(&msg_src_addr),
                                &msg_src_addrlen,
                                &sri,
                                &sri_len,
                                &msg_flags);
  if (rx_bytes == -1) {
    return -1;
  }

  ++nof_msgs;
  {
    std::vector<uint8_t> payload(temp_recv_buffer.begin(), temp_recv_buffer.begin() + rx_bytes);
    sink(user, std::move(payload), sri, msg_flags, msg_src_addr, msg_src_addrlen);
  }

  // Drain the socket: the usrsctp shim may queue several notifications/messages behind a single broker wake-up
  // byte, and reading only one message per callback would leave the rest waiting indefinitely for another event.
  while ((rx_bytes = ::sctp_recvmsg_nowait(fd,
                                           temp_recv_buffer.data(),
                                           temp_recv_buffer.size(),
                                           reinterpret_cast<sockaddr*>(&msg_src_addr),
                                           &msg_src_addrlen,
                                           &sri,
                                           &sri_len,
                                           &msg_flags)) != -1) {
    ++nof_msgs;
    std::vector<uint8_t> payload(temp_recv_buffer.begin(), temp_recv_buffer.begin() + rx_bytes);
    sink(user, std::move(payload), sri, msg_flags, msg_src_addr, msg_src_addrlen);
  }

  // A fatal error while draining aborts the whole wake-up (EAGAIN is the normal end of the drain).
  if (errno != EAGAIN) {
    return -1;
  }

  return nof_msgs;
}

int sctp_backend::recv_message_nowait(int                      fd,
                                      void*                   msg,
                                      size_t                  len,
                                      struct sockaddr*        from,
                                      socklen_t*              fromlen,
                                      struct sctp_sndrcvinfo* sri,
                                      socklen_t*              sri_len,
                                      int*                    msg_flags)
{
  return ::sctp_recvmsg_nowait(fd, msg, len, from, fromlen, sri, sri_len, msg_flags);
}

int sctp_backend::send_eof(int                    fd,
                           const struct sockaddr* dest_addr,
                           socklen_t              dest_addrlen,
                           uint32_t               ppid /* network byte order */,
                           unsigned               stream_no)
{
  // The shim's sctp_sendmsg() carries the SCTP_EOF flag in the sctp_sndinfo, mirroring the kernel's sendmsg +
  // SCTP_SNDINFO control-message path on Linux.
  return ::sctp_sendmsg(fd,
                        nullptr,
                        0,
                        const_cast<struct sockaddr*>(dest_addr),
                        dest_addrlen,
                        ppid,
                        SCTP_EOF,
                        stream_no,
                        0,
                        0);
}

peel_off_result sctp_backend::peel_off_socket(int /*fd*/, int /*assoc_id*/)
{
  // The single one-to-many socket delivers every association: there is nothing to peel off.
  peel_off_result result;
  result.supported = false;
  return result;
}

// The per-association broker subscription is part of the uniform server interface but is never invoked on macOS:
// no per-association fd is ever peeled off (see peel_off_socket()), so the receive path always goes through the
// parent-socket receive() callback.
bool sctp_network_server_impl::subscribe_association_to_broker(unique_fd /*assoc_fd*/,
                                                               sctp_associaton_context& /*assoc_ctxt*/)
{
  return false;
}
