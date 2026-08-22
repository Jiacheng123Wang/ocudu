# macOS test port - final notes (hand-written)

Status (macOS, `make test` / `scan.sh 1`): **100% tests passed (7556), 34 tests disabled (not applicable on
macOS), out of 7590 total**. Ubuntu runs all 7590: `100% tests passed, 0 tests failed out of 7590`. The macOS
summary line is printed by `tests/ci/macos_triage/postrun_summary.py` after every scan; the disabled count is
computed dynamically (currently 34 = 10 ctest-disabled + 24 runtime-skipped) and will follow the test list.

## 1. How the macOS test port differs from Ubuntu

Porting policy: **Linux compiles and runs the upstream logic byte-for-byte**. Every fix that had to touch shared
code is confined to macOS with `#if defined(__APPLE__)` (audited on 2026-08-22, verified 538/538 on the Ubuntu
reference machine with the original paths):

| shared file | macOS-only change | Linux keeps |
|---|---|---|
| `lib/gateways/sctp_socket.cpp` | the whole usrsctp shim (UDP encapsulation, PID-seeded encapsulation port, `sa_len`, SCTP_RECVRCVINFO, SO_RCVTIMEO emulation, graceful shutdown, RTO defaults) | kernel-SCTP implementation |
| `lib/gateways/udp_network_gateway_impl.cpp` | family-sized `msg_namelen` (macOS EINVAL on `sizeof(sockaddr_storage)`) plus the recvmmsg/sendmmsg emulations | Linux syscalls, `sizeof(sockaddr_storage)` |
| `lib/rrc/metrics/rrc_du_metrics_aggregator.h` | `rbegin()->second` (libc++ reads 0 when dereferencing `end()`) | original `end()->second` expression |
| `include/ocudu/adt/mutexed_mpmc_queue.h` | working timed `pop_blocking` wrapper | original latent wrapper (nothing instantiates it on Linux) |
| `tests/.../ethernet_frame_pool_test.cpp` | order-independent frame matching (libc++ destroys `std::vector` elements in reverse order, so the read burst comes back reversed) | positional comparison |
| `tests/.../text_formatter_test.cpp` | timestamp-shape validation (macOS `high_resolution_clock` is `steady_clock`, epoch = boot) | original full golden line |
| `tests/.../rlc_tx_{tm,am,um}_test.cpp` | inclusive `hol_toa` window bounds via `RLC_TEST_HOL_TOA_GE/LE` (macOS `steady_clock` ticks at ~41 ns; fast write paths can land on the same tick as the captured start time) | original `EXPECT_GT`/`EXPECT_LT` |
| `tests/.../sctp_network_link_test.cpp` | 127.0.0.1 client bind, connect/send pacing, bounded association/data waits (burst chunk drops on the shared usrsctp UDP socket) | unbound/unpaced/unbounded flow |
| `tests/.../sctp_network_{server,client}_test.cpp` | deadline-driven broker draining instead of fixed wake-up counts | one wake-up per notification |
| `tests/.../unique_thread_test.cpp` | names the main thread (macOS `pthread_getname_np` returns an empty name until the thread names itself) | process-name behaviour |

Other production-level macOS fixes: the lower-PHY teardown deadlock (`dl_task_executor` drain) and the E2E 27 s
ping-burst stall (the recvmmsg emulation held the receive callback for one inter-packet gap per datagram; it now
blocks on the first datagram and drains with MSG_DONTWAIT).

## 2. Tests that do not run on macOS (34; they run on Ubuntu only)

### 2a. ctest-disabled on macOS (10) - Ubuntu/Linux-only targets

| ctest case | why it does not run on macOS |
|---|---|
| `rlc_um6_eia2_eea2_stress_test`, `rlc_um12_eia2_eea2_stress_test`, `rlc_am12_eia2_eea2_stress_test`, `rlc_am18_eia2_eea2_stress_test`, `rlc_um12_eia1_eea1_stress_test`, `rlc_um12_eia3_eea3_stress_test`, `rlc_um12_eia2_eea0_stress_test` (7) | `pthread_barrier_*` is not implemented on macOS |
| `ofh_integration_test` | drives a real Ethernet controller through AF_PACKET (`linux/if_packet.h`) |
| `du_high_benchmark` | pins threads with the Linux CPU-affinity APIs (`pthread_setaffinity_np`, `CPU_SET`, ...) |
| `dft_processor_ci16_test` | the ci16 DFT processor is an x86_64 implementation; not built on Apple Silicon |

All ten are still registered (ctest `DISABLED` property) so they stay visible in the 7590 list instead of being
dropped silently. (`ocudulog_frontend_latency` is a build target only - no ctest case on either host.)

### 2b. Runtime-skipped on macOS with an explicit reason (24)

| cases | ctest case(s) | reason |
|---|---|---|
| 6 | `sctp_network_server_peer_test.*` | each peer binds a different loopback address (127.0.0.1 / .2 / .3); the usrsctp shim cannot associate distinct local addresses |
| 4 | `sctp_network_client_test.when_client_binds_address_...`, `...when_server_has_multihomed_ipv4_...`, `...when_server_has_multihomed_mixed_...`, `...ipv4_bind_and_ipv4_and_ipv6_connect_...` | client bound to another local address, or a multihomed server; same shim limitation |
| 1 | `sctp_socket_test.bindx_with_mixed_ipv4_and_ipv6_addresses` | usrsctp cannot bind an IPv4 and an IPv6 address on the same socket (fails as root too) |
| 3 | `sctp_socket_test.connectx_with_multiple_ipv4_addresses`, `...connectx_with_mixed_ipv4_and_ipv6_addresses`, `...connectx_with_different_address_counts` | usrsctp has no `sctp_connectx()`; the shim can only connect to the first peer address |
| 1 | `e2ap_network_adapter_test.when_e2_setup_response_received_then_ric_connected` | the E2 agent binds 127.0.0.101 while the RIC listens on 127.0.0.1; same shim limitation |
| 8 | `nia2/fxt_nia2.integrity_engine_nia2_cmac/*` | the Homebrew mbedtls@2 bottle is built with `MBEDTLS_CMAC_C` disabled |
| 1 | `rrc_du_ref_time_r16_test.subsecond_component_is_preserved` | macOS `system_clock` has microsecond resolution; sub-microsecond precision is not preserved |

The structural reason behind the 15 SCTP cases: unprivileged usrsctp tunnels SCTP over UDP through a single
wildcard-bound socket, so every packet leaves with the default source address and an association bound to another
local address never receives the answers; as root it emits native SCTP packets, which macOS does not loop back to a
local raw socket. Each case skips with `GTEST_SKIP()` and its reason, so the suite fails loudly instead of hanging.

### 2c. gtest-level skips inside passing ctest entries (2; not part of the 34-count above)

| parent ctest case | skipped gtest case | reason |
|---|---|---|
| `network_test` | `io_broker_epoll.af_inet_socket_tcp_trx_test` | macOS does not loop a TCP socket connected to itself back, so the bytes never arrive; the case should be rewritten with a real `listen()`/`accept()` pair |
| `f1ap_ref_time_provider_adapter_test` | `subsecond_component_encodes_losslessly` | macOS `system_clock` has microsecond resolution; sub-microsecond precision is not preserved |

The SCTP-variant gtest cases of `e1_gateway_test`/`f1c_gateway_test` were restored by the usrsctp shim fixes and
run now.

## 3. Suggestions for further macOS improvements

In rough priority order:

1. **Vendor usrsctp and fix its send path (low priority, agreed).** usrsctp 0.9.5.0 sends every datagram of every
   socket through one shared UDP socket with `sendmsg(MSG_DONTWAIT)` and drops a chunk whose send fails with EAGAIN
   (errno 35) without retrying; the SCTP timers recover it, which is why a burst of connects occasionally costs a
   few seconds. Building usrsctp from source would allow either a retry-on-EAGAIN patch in
   `sctp_userspace_ip_output` or switching the shim to the AF_CONN/`conn_output` transport. The tests mitigate with
   single-homing, pacing and bounded waits, so this is test-duration only, not correctness.
2. **zmq radio slow motion (the residual E2E burst).** The cell slot counter advances at only ~2.5 slots/s
   (should be 1000/s) - measured again in the 2026-08-22 E2E run (`/tmp/gnb.log`): slot rate 2.51 slots/s over a
   144 s window. This quantizes every over-the-air opportunity to a ~400 ms slot period and is the cause of the
   residual ping bursts (500 pings, 0 % loss, rtt min/avg/max = 248/779/2459 ms, mdev 408 ms; Ubuntu gnb: uniform
   300-400 ms). Per-packet analysis of `/tmp/gnb.log`:
   - UL requests wait a median 333 ms (PUSCH -> GTPU egress), ~2.8 requests per PUSCH (PUSCH RX at ~3.5/s), so the
     requests reach the UPF in mini-batches;
   - the replies come back in the same batch pattern (7-8 datagrams per GTP-U ingress batch, ~1.3-1.9 s apart);
   - DL replies wait a median 386 ms in the MAC queue (RLC TX SDU -> PDSCH TX);
   - the UE-side sawtooth (RTT descending ~100 ms per packet) is the flush signature of these batches: replies
     accumulated at the 10 pps request rate are flushed together, so each reply's RTT drops by the request
     interval per step. Between sawtooths the RTT approaches ~250-300 ms, at or below the Ubuntu baseline - the
     gnb's own processing is fast; all the excess latency is the slot quantization. The usrsctp TODO is NOT
     involved: the user plane never touches SCTP.
   Suspects for the slow slot clock: clock drift between the two hosts and the zmq `tx_time` pacing; needs its own
   investigation with zmq timestamps on both sides.
3. **usrsctp multihoming / `connectx()` support in the shim** (unblocks the 10 multihomed/bindx/connectx cases and
   the E2 agent case in section 2b). Requires the from-source usrsctp work of item 1 (custom per-association UDP
   sockets), or an alternative userspace transport.
4. **mbedtls CMAC rebuild** (unblocks the 8 `nia2` cases): build/install a Homebrew mbedtls@2 with `MBEDTLS_CMAC_C`
   enabled.
5. **TCP self-connect test rewrite** (`network_test.io_broker_epoll.af_inet_socket_tcp_trx_test`): macOS does not
   loop a self-connected TCP socket back; rewrite the case with a real `listen()`/`accept()` pair.
6. **`pthread_barrier_*` shim** (7 RLC stress tests): a small macOS compatibility header could unblock them.
7. **`du_high_benchmark` on macOS**: map the Linux CPU-affinity calls onto the Mach thread_policy APIs.
8. **`ofh_integration_test`**: needs a macOS raw-Ethernet equivalent of AF_PACKET (BPF), out of scope for now.
9. **`dft_processor_ci16_test`**: needs an arm64 implementation of the ci16 DFT path.
10. **Sub-microsecond wall clock** (`rrc_du_ref_time_r16_test`): cosmetic; skip until the test matters for macOS.
11. **gnb UDP-encapsulation fixed port for cross-machine SCTP** (E2E against Linux kernel SCTP peers with
    `sysctl net.sctp.udp_port`): needs an env override for the encapsulation port once item 1 lands.

## 4. Run instructions (macOS test host)

```sh
sudo ifconfig lo0 alias 127.0.0.2 up      # plus 127.0.0.3, 127.0.1.1, 127.0.0.101 - once per boot
cd build && ../tests/ci/macos_triage/scan.sh 1          # full make test with the hang watchdog
sudo OCUDU_USRSCTP_MODE=udp ctest -L sctp               # the SCTP label alone (UDP encapsulation)
```

`SUMMARY.md` is regenerated from the scan logs with `tests/ci/macos_triage/aggregate.py` +
`tests/ci/macos_triage/make_summary.py`; this file is hand-written - update it after every new finding.
