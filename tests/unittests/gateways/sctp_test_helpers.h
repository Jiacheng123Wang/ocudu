// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/gateways/sctp_socket.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/io/io_broker.h"
#include <arpa/inet.h>
#include <map>
#include <chrono>
#include <gtest/gtest.h>
#include <netdb.h>
#include <thread>

namespace ocudu {

/// \brief Skips the test when the SCTP stack cannot associate two distinct local addresses.
///
/// macOS has no in-kernel SCTP, so the gateway runs on usrsctp. An unprivileged process cannot open the raw socket
/// usrsctp needs for native SCTP packets, so the shim tunnels SCTP over UDP (RFC 6951) through a single
/// wildcard-bound UDP socket: outgoing packets always leave with the default source address, therefore an
/// association bound to another local address - or a multihomed one - never receives the peer's answers. Running as
/// root does not help: usrsctp then emits native SCTP packets, which macOS does not loop back to a local raw socket.
#if defined(__APPLE__)
#define OCUDU_SKIP_IF_NO_SCTP_MULTI_LOCAL_ADDRESS()                                                                    \
  GTEST_SKIP() << "usrsctp on macOS cannot associate distinct local addresses (SCTP-over-UDP encapsulation uses one "  \
                  "wildcard socket)"
#else
#define OCUDU_SKIP_IF_NO_SCTP_MULTI_LOCAL_ADDRESS()                                                                    \
  do {                                                                                                                 \
  } while (0)
#endif

/// \brief Skips the test when the SCTP stack has no sctp_connectx() with more than one peer address.
///
/// usrsctp only provides usrsctp_connect(): the shim connects to the first address of the list, so tests that check
/// the multi-address behaviour of sctp_connectx() cannot pass on macOS.
#if defined(__APPLE__)
#define OCUDU_SKIP_IF_NO_SCTP_CONNECTX()                                                                               \
  GTEST_SKIP() << "usrsctp has no sctp_connectx(): only the first peer address is used"
#else
#define OCUDU_SKIP_IF_NO_SCTP_CONNECTX()                                                                               \
  do {                                                                                                                 \
  } while (0)
#endif


/// Dummy IO broker where the registered callbacks have to be called manually.
class dummy_io_broker : public io_broker
{
public:
  struct dummy_subscription {
    unique_fd        registered_fd;
    recv_callback_t  handle_receive;
    error_callback_t handle_error;
  };

  [[nodiscard]] subscriber register_fd(
      unique_fd        fd,
      task_executor&   executor,
      recv_callback_t  handler_,
      error_callback_t err_handler_ = [](error_code) {}) override
  {
    if (not accept_next_fd) {
      return {};
    }
    last_registered_fd          = fd.value();
    sub_map[last_registered_fd] = dummy_subscription{std::move(fd), handler_, err_handler_};
    registration_count++;
    return subscriber{*this, last_registered_fd};
  }

  [[nodiscard]] bool unregister_fd(int fd, std::promise<bool>* complete_notifier) override
  {
    sub_map.erase(fd);
    deregistration_count++;
    if (complete_notifier) {
      complete_notifier->set_value(true);
    }
    return true;
  }

  int  nof_registered_sockets() const { return sub_map.size(); }
  int  get_nof_registrations() const { return registration_count; }
  int  get_nof_deregistrations() const { return deregistration_count; }
  bool is_socket_registered(int fd) const { return sub_map.find(fd) != sub_map.end(); }
  int  get_last_registered_fd() const { return last_registered_fd; }
  void handle_receive(int fd)
  {
    if (not is_socket_registered(fd)) {
      return;
    }
    sub_map[fd].handle_receive();
  }
  bool accept_next_fd = true;

private:
  std::map<int, dummy_subscription> sub_map;
  uint32_t                          registration_count   = 0;
  uint32_t                          deregistration_count = 0;
  int                               last_registered_fd   = -1;
};

struct test_recv_data {
#if defined(__APPLE__)
  struct sctp_rcvinfo sri       = {};
  socklen_t           sri_len   = sizeof(sri);
#else
  struct sctp_sndrcvinfo sri       = {};
#endif
  int                    msg_flags = 0;
  sockaddr_storage       msg_src_addr;
  // fromlen is an in/out variable in sctp_recvmsg.
  socklen_t            msg_src_addrlen = sizeof(msg_src_addr);
  std::vector<uint8_t> data;

  bool has_notification() const { return msg_flags & MSG_NOTIFICATION; }
  bool has_data() const { return not has_notification(); }
  int  sctp_notification() const
  {
    ocudu_assert(has_notification(), "bad access");
    const auto* notif = reinterpret_cast<const union sctp_notification*>(data.data());
    return notif->sn_header.sn_type;
  }
  const struct sctp_assoc_change& sctp_assoc_change() const
  {
    ocudu_assert(has_notification() and sctp_notification() == SCTP_ASSOC_CHANGE, "bad access");
    const auto* notif = reinterpret_cast<const union sctp_notification*>(data.data());
    return notif->sn_assoc_change;
  }
};

class dummy_sctp_node
{
public:
  dummy_sctp_node(const std::string& name_) : name(name_), logger(ocudulog::fetch_basic_logger(name))
  {
    logger.set_level(ocudulog::basic_levels::debug);
  }
  ~dummy_sctp_node() { close(); }

  bool close()
  {
    if (socket.is_open()) {
      socket.close();
      logger.info("{} shut down", name);
      return true;
    }
    return false;
  }

  /// \brief Receives one SCTP message or notification.
  ///
  /// The socket is non-blocking, so poll it until the peer message shows up (or the deadline expires). A single
  /// attempt is not enough with a user-space SCTP stack (macOS/usrsctp), where the peer's message is delivered
  /// asynchronously by the stack's receive thread instead of being queued by the kernel before send() returns.
  std::optional<test_recv_data> receive(std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      std::optional<test_recv_data> result = try_receive();
      if (result.has_value() or last_recv_errno != EAGAIN or std::chrono::steady_clock::now() >= deadline) {
        return result;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::optional<test_recv_data> try_receive()
  {
    static constexpr uint32_t network_gateway_sctp_max_len = 9100;

    test_recv_data data;

    std::array<uint8_t, network_gateway_sctp_max_len> temp_buf;
    int                                               rx_bytes = ::sctp_recvmsg(socket.fd().value(),
                                  temp_buf.data(),
                                  temp_buf.size(),
                                  (struct sockaddr*)&data.msg_src_addr,
                                  &data.msg_src_addrlen,
                                  &data.sri,
#if defined(__APPLE__)
                                  &data.sri_len,
#endif
                                  &data.msg_flags);
    if (rx_bytes < 0) {
      last_recv_errno = errno;
      if (errno != EAGAIN) {
        logger.error("Recv error: {}", ::strerror(errno));
      }
      return std::nullopt;
    }
    last_recv_errno = 0;

    data.data.assign(temp_buf.begin(), temp_buf.begin() + rx_bytes);
    return data;
  }

  bool send_data(const std::vector<uint8_t>& bytes, int ppid, const sockaddr& dest_addr, socklen_t dest_addrlen)
  {
    int bytes_sent = ::sctp_sendmsg(socket.fd().value(),
                                    bytes.data(),
                                    bytes.size(),
                                    (struct sockaddr*)&dest_addr,
                                    dest_addrlen,
                                    htonl(ppid),
                                    0,
                                    0,
                                    0,
                                    0);
    return bytes_sent == (int)bytes.size();
  }

  bool send_data(const std::vector<uint8_t>& bytes, int ppid, std::string dest_addr, int dest_port)
  {
    sockaddr_in addr     = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(dest_port);
    addr.sin_addr.s_addr = ::inet_addr(dest_addr.c_str());
    return send_data(bytes, ppid, (struct sockaddr&)addr, sizeof(addr));
  }

  bool send_eof(int ppid, const sockaddr& dest_addr, socklen_t dest_addrlen)
  {
    // Send EOF to SCTP server.
    int bytes_sent = ::sctp_sendmsg(socket.fd().value(),
                                    nullptr,
                                    0,
                                    const_cast<struct sockaddr*>(&dest_addr),
                                    dest_addrlen,
                                    htonl(ppid),
                                    SCTP_EOF,
                                    0,
                                    0,
                                    0);
    return bytes_sent != -1;
  }

  sctp_socket             socket;
  std::string             name;
  ocudulog::basic_logger& logger;
  /// errno of the last try_receive() call, used by receive() to decide whether to retry.
  int last_recv_errno = 0;
};

} // namespace ocudu
