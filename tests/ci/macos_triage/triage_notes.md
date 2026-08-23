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
2. **zmq radio E2E bursts - root cause found and fixed (2026-08-22).** The gnb/UE zmq link is a REQ/REP lockstep
   whose pace is set by srsUE's processing (good equilibrium ~26 rounds/s, ping rtt avg ~418 ms, close to Ubuntu's
   300-400 ms). The bad equilibrium (ping avg 2-6 s, max 10 s, lockstep decaying 9->1 rounds/s) is a positive
   feedback loop proven with tcpdumps on both ends + temporary [zmq-probe] logs:
   - the UE's receive pace slows -> the gnb's blocking `zmq_send` trickles each 92 KB block through the UE's TCP
     window (median 85 ms, max 3.2 s) -> the REP channel loop freezes (request handling p50 1.16 s DL / 1.8 s UL);
   - meanwhile `dl_process` keeps filling the 614400-sample TX queue and `send_response()` drained it ALL into one
     reply, so a stalled period produced a 385 KB (4+ slot) mega-message whose trickle was proportionally longer -
     the feedback that degraded the lockstep and piled up multi-second latencies.
   - Fix attempt 2 (reverted): cap each DL reply at one slot (macOS) to bound the per-reply trickle. Validation: the
     lockstep recovered to a stable 15.2 rounds/s and the ping bursts shrank (avg 0.9-1.4 s vs 2-6 s), BUT the UE
     attach became unreliable: during attach the UE pulls DL blocks much slower (cell-search/SIB processing), and
     with the one-slot cap the DL delivery is exactly slaved to that slow pull, so the NAS/auth exchange crawls
     (~0.4 slots/s, 10-20 s per round trip) and the AMF's ~6 s retransmission/registration timers give up -> RRC
     Release, no IP. Without the cap the DL production runs ahead of the UE during attach and the NAS messages get
     through. The cap was reverted: attach reliability wins over the steady-state burst. A fix that bounds the
     reply without slaving the delivery (e.g. a cap that only applies once the UE is in connected state, or a
     per-association backlog limit enforced in dl_process instead of the channel) remains open.
   - Constraint learned from a reverted fix attempt (zero-filled reply for empty buffers): the wire sample stream
     must stay exactly 1:1 with the gnb's slot production - extra zero blocks advance the UE's sample clock and
     break the RACH timing ("tx time ... in the past"). The usrsctp TODO is NOT involved (the user plane never
     touches SCTP).
   - Current fix (gnb-only, per the project decision that the UE side stays upstream): enlarge the zmq socket
     buffers on the gnb (`ZMQ_SNDBUF`/`ZMQ_RCVBUF` = 8 MB, macOS-only, in the TX/RX channels). With the send
     buffer larger than a baseband block, the blocking `zmq_send` hands the whole block to the kernel and returns
     immediately even while the UE's small TCP window makes the wire delivery trickle - the REP channel loop no
     longer freezes and the request-latency compounding disappears. The matching srsUE-side change
     (`ue_zmq_sockbuf.patch` in this directory) was applied twice during the analysis (to prove the TCP-window
     bottleneck, and for the final A/B comparison) and reverted on the 153 machine both times - the UE stays
     upstream (DO NOT APPLY).
   - Final E2E state (gnb-only fix + upstream srsUE, 7 ping rounds): avg 710-1372 ms (mean ~918 ms), max
     1.5-3.5 s, min ~300-500 ms, 0-1% loss - stable across rounds (previously: bursts to 10 s, avg 2-6 s,
     good/bad alternation). The A/B comparison with the UE-side socket buffers applied as well: avg 740-897 ms
     (mean ~817 ms), max 1.6-2.8 s - an ~11% improvement, negligible compared with the Ubuntu gnb reference
     (300-400 ms), so the UE patch was reverted. min RTT ~400 ms equals the Ubuntu baseline: the gnb's own data
     path is fine; the remaining avg-vs-min gap comes from the lockstep quantization (the round rate is set by
     srsUE's processing, 15-26 rounds/s) plus the gnb scheduler's UL grant cycle (~250-400 ms wall time).
   - Ubuntu gnb difference (same UE): plausible causes, ranked - (a) the Ubuntu gnb's per-slot DL production is
     faster (x86, no macOS thread-QoS overhead), so its DL stream stays ahead of the UE and the UE's PHY pipeline
     is never starved, keeping the lockstep at the fast equilibrium; the macOS gnb's slower production makes the
     lockstep settle lower and every UE hiccup is amplified by the feedback loops above. (b) The macOS gnb's
     scheduling quanta (~30-70 ms/slot) add directly to the RTT while the Ubuntu gnb's are ~ms. (c) Host-level
     differences (Apple Silicon thread scheduling, QoS classes) rather than code logic. To discriminate: run the
     same dual-end tcpdump + [zmq-probe] methodology against the Ubuntu gnb (131) for a reference rounds/s and
     per-stage latency profile, and compare dl_process per-slot cost on both hosts.
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

## 5. macOS-vs-Ubuntu gnb E2E comparison (2026-08-23, probe commit f42e5b981b)

Same UE (upstream srsUE, 192.168.100.153), same core, same probe-instrumented gnb commit on both hosts;
ping from the UE to UPF 10.45.0.1, dual-end tcpdumps + `[zmq-probe]` logs. Full numbers in the local
analysis workspace `../e2e_compare/REPORT.md` (pcaps, logs, and the parse scripts).

Result: Ubuntu gnb ping avg ~530ms (min ~155-254ms) vs mac gnb avg ~840-1350ms (min ~400ms). Lockstep
rounds/s: Ubuntu ~52-56/s, mac 13-19/s. Root cause found - see below.

Evidence chain (all measured):

1. gnb probe logs: `tx buffer-empty-wait` p50 ~1.5ms on BOTH hosts -> the DL producer keeps up with the
   UE pulls on the mac too. The earlier hypothesis "Ubuntu produces DL slots faster per slot" is REFUTED.
   `zmq-send-block` = 0 events on both (the macOS 8MB socket-buffer fix works; Ubuntu never blocks).
2. pcap (DL stream): Ubuntu replies = 1 slot (92KB) delivered in 3 TSO super-segments, wire time med 6ms;
   mac replies = 2 slots (184KB) in 128 MSS segments, wire time med 55ms, 1520 retransmissions.
3. UE-side pcaps: UE arrival->ACK delay med 0.0ms on BOTH rounds -> the UE TCP stack is exonerated
   (its 64KB receive window only shapes the burst count, it never stalls the stream).
4. Both-end one-way matching: mac->UE DL data one-way ~70ms per burst vs UE->mac ~4ms; 131<->153 both
   directions are single-digit ms. Per-reply timeline: the mac's kernel queues a 64KB burst, the burst
   reaches the UE ~70ms later, the UE ACKs instantly, and only then does the next burst go out; a 184KB
   reply costs 3 x ~70ms = 210ms and collapses the lockstep to 13-19 rounds/s, which multiplies into
   ~1s ping RTT (UE waits for UL requests ~47ms + DL cycles + gnb scheduling quanta).
5. All three hosts are on Wi-Fi (mac en1, 131 wlo1, 153 wlo1). Idle mac->153 ping right now:
   9.9/51.3/93.8 ms; RSSI -67dBm (5GHz ch149); awdl0 UP (AWDL causes periodic macOS TX stalls).

Conclusion: the E2E gap is NOT in the gnb code, the UE, or the ZMQ/TCP logic. It is the mac's Wi-Fi
transmit path adding ~70ms per 64KB TX burst (weak signal + AWDL); Ubuntu's good Wi-Fi link + TSO bursts
deliver each 92KB reply in ~6ms and keep the lockstep at ~55 rounds/s.

Next steps:
1. Re-run Round A with the mac on Ethernet (definitive confirmation; expect ~Ubuntu-level ping).
2. If stuck on Wi-Fi: `sudo ifconfig awdl0 down` before the run, improve RSSI/channel.
3. Optional re-run with `lib_level: info` (already added to both `configs/gnb_zmq.yaml`) to capture the
   dl production-rate probes (they were dropped before because lib_level defaults to warning).
4. Keep the 8MB socket-buffer fix (keeps the channel loop healthy on slow links); revert only the probe
   commit f42e5b981b when the comparison is done. Tag `macos_e2e_stable` stays at 383dc4eae8.

### 5b. Wired re-run confirms the Wi-Fi diagnosis (2026-08-23)

ZMQ moved to a direct Ethernet cable (mac=198.19.0.1, UE=198.19.0.2, Ubuntu gnb=198.19.0.3),
same gnb commit f42e5b981b and same UE. Results (full data in `../e2e_compare/REPORT.md`):

| metric | mac Wi-Fi | Ubuntu Wi-Fi | mac wired | Ubuntu wired |
|---|---|---|---|---|
| ping avg (UE->UPF) | ~840-1350ms | ~530ms | **~100ms** | ~137ms |
| ping min | ~400ms | ~155-254ms | **~27-36ms** | ~45-52ms |
| lockstep rounds/s | 13-19 | 52-56 | **711** | 444 |
| dl production slots/s | (not logged) | (not logged) | **719** | 444 |
| rx reply-age | 46.7ms | 17.0ms | 1.32ms | 1.19ms |
| DL wire transfer (92KB) | 55-65ms | 6ms | 0.2ms | 1.4ms |

- The Wi-Fi TX path was the whole story: wired, the mac gnb is 10x faster than its Wi-Fi
  self and now BEATS the Ubuntu gnb (711 vs 444 rounds/s; the Apple Silicon slot production
  outruns the NUC, which only shows once the link stops dominating).
- One-way link latency on the wire: ~0.2-0.4ms both directions.
- The remaining ~100ms ping floor is the gnb scheduling path + GTP-U round trip to the UPF
  on 153 (still Wi-Fi), not the ZMQ lockstep (~2-3ms per round now). Optional follow-up:
  capture the N3/GTP-U interface during a ping run to decompose that floor.
- Close-out: keep the 8MB socket-buffer fix and tag `macos_e2e_stable`; revert the probe
  commit f42e5b981b when no further rounds are planned.

### 5c. Probe instrumentation is now permanent, gated by ENABLE_FLOW_PROBES

The `[zmq-probe]` instrumentation (tx request-received / buffer-empty-wait / buffer-full-block /
zmq-send-block + per-64 reply-rate, rx reply-age + reply-rate, lower-PHY dl rx-wait/process/transmit +
dl 64-slot rate, ul recv-wait) is kept in the tree for future E2E analysis (ping and iperf3), compiled
only when the existing CMake option `ENABLE_FLOW_PROBES` is ON (`-DOCUDU_FLOW_PROBES`, off by default):

```sh
cmake -S . -B build_probes -DENABLE_FLOW_PROBES=ON   # analysis build
make -C build_probes gnb
```

With the flag OFF no probe code is compiled (zero impact, the gnb binary contains no `[zmq-probe]`
strings). The zmq/PHY logs go to the normal log file; remember to set `log: lib_level: info` in
`configs/gnb_zmq.yaml` so the "ALL"-logger probes (dl/ul lower-PHY) are not dropped. The wired ZMQ
setup (gnb=198.19.0.1, UE=198.19.0.2, Ubuntu gnb=198.19.0.3, direct cable) is the reference transport
for future E2E runs. The reusable analysis scripts, the capture protocol and the full comparison
report live in `tests/ci/macos_e2e/` (REPORT.md, README.md and the pcap/probe parsers).
