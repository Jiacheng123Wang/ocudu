// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

/// \brief Platform-compat type header for the SCTP gateway family.
///
/// This header is the single place where the SCTP layer maps the platform
/// types: the Linux kernel SCTP API (<netinet/sctp.h>) or the usrsctp
/// user-space stack (macOS, which has no native SCTP support). The gateway
/// and socket code only includes this header and never sees a platform
/// conditional.
///
/// On macOS the usrsctp structures are re-exposed under Linux-compatible,
/// layout-identical definitions (sctp_rcvinfo / sctp_sndinfo / sctp_sndrcvinfo)
/// so the shared code can use one set of type names on both platforms.

#include <cstdint>
#include <netinet/in.h>

#if defined(__APPLE__)
// usrsctp names the fields of sctp_rcvinfo/sctp_sndinfo rcv_*/snd_* instead of the Linux kernel header names
// (sinfo_*) that the gateway code uses. Rename the usrsctp structures during the include and re-expose
// Linux-compatible, layout-identical definitions.
#define sctp_rcvinfo usrsctp_sctp_rcvinfo
#define sctp_sndinfo usrsctp_sctp_sndinfo
#include <usrsctp.h>
#undef sctp_rcvinfo
#undef sctp_sndinfo

#include <cstddef>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/// Linux-compatible sctp_rcvinfo (same layout as usrsctp's structure with rcv_* field names).
struct sctp_rcvinfo {
  uint16_t     sinfo_stream;
  uint16_t     sinfo_ssn;
  uint16_t     sinfo_flags;
  uint32_t     sinfo_ppid;
  uint32_t     sinfo_tsn;
  uint32_t     sinfo_cumtsn;
  uint32_t     sinfo_context;
  sctp_assoc_t sinfo_assoc_id;
};

/// Linux-compatible sctp_sndinfo (same layout as usrsctp's structure with snd_* field names).
struct sctp_sndinfo {
  uint16_t     sinfo_stream;
  uint16_t     sinfo_flags;
  uint32_t     sinfo_ppid;
  uint32_t     sinfo_context;
  sctp_assoc_t sinfo_assoc_id;
};

/// Linux-compatible sctp_sndrcvinfo (same layout as sctp_rcvinfo above, which is the part the usrsctp shim
/// fills in sctp_recvmsg(); the gateway only reads the stream/ssn/flags/ppid/assoc-id fields, whose offsets
/// match the Linux kernel structure).
struct sctp_sndrcvinfo {
  uint16_t     sinfo_stream;
  uint16_t     sinfo_ssn;
  uint16_t     sinfo_flags;
  uint32_t     sinfo_ppid;
  uint32_t     sinfo_tsn;
  uint32_t     sinfo_cumtsn;
  uint32_t     sinfo_context;
  sctp_assoc_t sinfo_assoc_id;
};

static_assert(sizeof(sctp_rcvinfo) == sizeof(usrsctp_sctp_rcvinfo));
static_assert(offsetof(sctp_rcvinfo, sinfo_assoc_id) == offsetof(usrsctp_sctp_rcvinfo, rcv_assoc_id));
static_assert(sizeof(sctp_sndinfo) == sizeof(usrsctp_sctp_sndinfo));
static_assert(offsetof(sctp_sndinfo, sinfo_ppid) == offsetof(usrsctp_sctp_sndinfo, snd_ppid));
static_assert(sizeof(sctp_sndrcvinfo) == sizeof(sctp_rcvinfo));
static_assert(offsetof(sctp_sndrcvinfo, sinfo_assoc_id) == offsetof(sctp_rcvinfo, sinfo_assoc_id));

#ifndef IPPROTO_SCTP
#define IPPROTO_SCTP 132
#endif
#ifndef SOL_SCTP
#define SOL_SCTP IPPROTO_SCTP
#endif

// The usrsctp shim exports the Linux kernel SCTP API surface (sctp_bindx, sctp_connectx, sctp_sendmsg, ...) so the
// shared gateway code can call the same functions as on Linux. The definitions live in the macOS-only backend
// (lib/gateways/sctp_socket_usrsctp.cpp).
extern "C" int  sctp_bindx(int s, struct sockaddr* addrs, int addrcnt, int flags);
extern "C" int  sctp_connectx(int s, struct sockaddr* addrs, int addrcnt, uint32_t* id);
extern "C" int  sctp_sendmsg(int s, const void* msg, size_t len, struct sockaddr* to, socklen_t tolen, uint32_t ppid,
                             uint32_t flags, uint16_t stream_no, uint32_t timetolive, uint32_t context);
extern "C" int  sctp_recvmsg(int s, void* msg, size_t len, struct sockaddr* from, socklen_t* fromlen, void* sinfo,
                             socklen_t* sinfo_len, int* msg_flags);
/// Non-waiting receive: performs a single non-blocking read and returns EAGAIN when nothing is queued, without the
/// emulated SO_RCVTIMEO wait of sctp_recvmsg(). Used by the gateway receive callbacks to drain every message queued
/// behind one broker wake-up without stalling the io thread on an empty queue.
extern "C" int  sctp_recvmsg_nowait(int s, void* msg, size_t len, struct sockaddr* from, socklen_t* fromlen,
                                    void* sinfo, socklen_t* sinfo_len, int* msg_flags);
extern "C" int  sctp_getpaddrs(int s, uint32_t assoc_id, struct sockaddr** addrs);
extern "C" void sctp_freepaddrs(struct sockaddr* addrs);
extern "C" int  sctp_getladdrs(int s, uint32_t assoc_id, struct sockaddr** addrs);
extern "C" void sctp_freeladdrs(struct sockaddr* addrs);
/// SCTP-level get/setsockopt for the usrsctp-backed sockets. The fd handed out by sctp_socket is only a bridge
/// socketpair used to wake the io_broker, so SCTP options must be routed to the usrsctp socket instead of being
/// passed to ::getsockopt()/::setsockopt() (which would silently act on the AF_UNIX socketpair).
extern "C" int  sctp_getsockopt(int s, int level, int optname, void* optval, socklen_t* optlen);
extern "C" int  sctp_setsockopt(int s, int level, int optname, const void* optval, socklen_t optlen);
/// Returns the SCTP-over-UDP encapsulation port in use (0 when packets are sent as plain SCTP).
extern "C" uint16_t sctp_udp_encapsulation_port(void);
#else
#include <netinet/sctp.h>
#endif

#include <sys/socket.h>
