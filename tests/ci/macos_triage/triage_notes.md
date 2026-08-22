# macOS `make test` hang triage log

Method: `make test ARGS="-I <n>,"` under `build/macos_triage/scan.sh`, which watches the
ctest output stream. When no progress appears for 120 s (or a single test exceeds 600 s) the
whole process tree is SIGKILLed, the currently running case (from the last `Start <n>:` line)
is recorded, the case is skipped in the sources, and the scan resumes at the next index.

Skips keep the ctest index stable (`GTEST_SKIP` at fixture SetUp, or `DISABLED TRUE`), so
resume indices stay valid across iterations.

| # | ctest index | test case | symptom | action taken | needs macOS debug |
|---|---|---|---|---|---|
| 1 | 2524 | `e1_gateway_test` (gtest cases `e1_gateway_link_tests/e1_gateway_link_test.*/0`, i.e. `use_sctp=true`) | SCTP association is established, fd registered in io_broker, then no PDU is ever delivered; test blocks in `pop_cu_rx_pdu()` / `assoc_close_signaled.wait()`. Killed after 120 s of no output, process was alive at ~3 % CPU. | `SetUp()` in `e1_gateway_link_test` calls `GTEST_SKIP()` on macOS when `GetParam()==true`; the non-SCTP (`/1`) variants still run and pass. | yes - macOS has no kernel SCTP; the `usrsctp` userspace shim returns handles that are not real fds, so the io_broker (kqueue/epoll shim) never reports them readable |
| 2 | 2708 | `f1c_gateway_test` (gtest cases `f1c_gateway_link_tests/f1c_gateway_link_test.*/0`, `use_sctp=true`) | identical to #2524: association established, then blocks forever waiting for the peer PDU. Killed after 120 s of no output. | `SetUp()` in `f1c_gateway_link_test` calls `GTEST_SKIP()` on macOS when `GetParam()==true`; the non-SCTP variants still run and pass (`#2708 Passed 3.22 s`). | yes - same usrsctp/io_broker root cause |
| 3 | 2983 | `sctp_network_server_test.when_client_connects_then_server_request_new_sctp_association_handler_creation` | blocks in `connect_client()` waiting for the server-side association. Killed after 120 s. Siblings classified individually with `ctest --timeout 30`: #2983-#2989 time out, #2990/#2991 pass, #2992-#3019 fail fast, #3020-#3027 time out. | `OCUDU_SKIP_SCTP_CLIENT_LINK_ON_MACOS()` (GTEST_SKIP under `__APPLE__`) added to the 7 hanging `sctp_network_server_test` bodies. The 8 `sctp_network_link_test` cases hang inside the fixture *constructor* (before SetUp), so they use the `MAYBE_`/`DISABLED_` rename: CMake's gtest discovery maps it to the ctest DISABLED property, keeping the ctest name and index. | yes - same usrsctp/io_broker root cause |
| 4 | 5407 | `lower_phy_test` (gtest case `LowerPhy/LowerPhyFixture.BasebandDownlinkFlow/*`, flaky per random numerology params) | `sample(1)` on the killed process showed the main thread inside `LowerPhyFixture::TearDown()` and the stop thread inside `lower_phy_baseband_processor::stop()`. On macOS `stop()` flushes the DL/UL processing executors with sentinel tasks (commit 565a85c88b guards that behavior with `__APPLE__`), but the fixture's flush loop only drained `rx_`/`tx_`/`ul_task_executor`, never `dl_task_executor`, so the downlink sentinel was never executed. | **fixed, not skipped**: the TearDown flush loop now drains every manual executor (including `dl_task_executor`) until `stop()` returns, under `#if defined(__APPLE__)`; Linux keeps the original loop. 432/432 gtest cases pass, verified over 7 consecutive runs. | no - resolved |
| 5 | 7404 | `network_test` (gtest case `io_broker_epoll.af_inet_socket_tcp_trx_test`) | `sample(1)` showed the main thread waiting on the rx condition variable in `run_tx_rx_test()` while the `io_broker_kqueue` thread sat in `kevent()`. The fixture creates one TCP socket, binds it to loopback:0 and `connect()`s it to its own address (Linux self-connect), then expects the bytes back. On macOS `connect()`/`send()` succeed but nothing loops back. The unix-socket and UDP variants of the same fixture pass. | `GTEST_SKIP()` under `__APPLE__` at the top of that test body; `#7404 network_test` now passes with 16 cases run and 1 skipped. | yes - the macOS variant should be rewritten with a real `listen()`/`accept()` socket pair |

## Result

The final uninterrupted `make test` (index 1 -> 7590) completed with no hang:
**7503 Passed, 52 Failed, 1 Crashed, 24 Skipped, 10 Disabled** (7590 total). See `SUMMARY.md`.

History of the three passes:

| pass | Passed | Failed | Crashed | Skipped | Disabled |
|---|---|---|---|---|---|
| 1. hang scan (7569 cases registered) | 7446 | 107 | 1 | 7 | 8 |
| 2. test-count parity with Ubuntu (7590) | 7448 | 107 | 1 | 16 | 18 |
| 3. usrsctp shim fixes | **7503** | 52 | 1 | 24 | 10 |

Note: the ctest indices in the hang table above are the ones observed during the first pass, when the list still had
7569 entries. After the parity work `lower_phy_test` is #5408 and `network_test` is #7414; the SCTP indices (#2524,
#2708, #2983-#2989, #3020-#3027) are unchanged.

## The SCTP pass (third pass)

The 15 cases that pass 1 had to skip/disable were not test bugs: the usrsctp shim never established an association,
because usrsctp sends native SCTP packets through a raw socket and macOS only allows that to root. Eight distinct
defects were found and fixed in `lib/gateways/sctp_socket.cpp` (see the `2e` table in `SUMMARY.md`): SCTP-over-UDP
encapsulation fallback, `sa_len` filling, `SCTP_RECVRCVINFO`, SO_RCVTIMEO emulation with `MSG_DONTWAIT`, graceful
shutdown through per-association `SCTP_EOF`, `SO_NOSIGPIPE` on the bridge socketpair, RTO clamping plus lower usrsctp
RTO defaults, and INITMSG/NODELAY routing (with public `sctp_get/setsockopt()` so tests can read SCTP options).

`ctest -L sctp` (78 cases): 20 passing / 43 failing / 15 hanging  ->  **63 passing, 15 skipped, 0 failing, 0 hanging**.
The same sources were copied to the Ubuntu machine and give **78/78 passing** there (3 consecutive runs), so Linux did
not regress.

### Known follow-up on macOS

`sctp_network_client_test.when_client_sender_is_destroyed_then_client_sends_eof` (#3011) still flakes roughly once
per three full-suite runs: occasionally the client's own `SCTP_SHUTDOWN_COMP` is not delivered, and
`sctp_network_client_impl::~sctp_network_client_impl()` then waits on an unbounded condition variable, so the case
times out instead of failing. Bounding that wait naively is not an option (it races with the receive path and
segfaults); the teardown has to deregister the io_broker subscription before waiting. Three sibling cases that had
the same symptom were made event driven (drive the broker until the expected state) and are stable now.

Tests whose assertions assumed one notification per broker wake-up were made event driven under `__APPLE__`
(`when_multiple_clients_connect_...`, `when_client_sends_sctp_message_...`, `when_server_sends_eof_...`,
`when_client_sends_eof_before_...`): a user-space stack delivers asynchronously and may interleave the
notifications of two associations, which the kernel stack does not.


Test-host prerequisite (macOS only, does not survive a reboot):

```
sudo ifconfig lo0 alias 127.0.0.2 up
sudo ifconfig lo0 alias 127.0.0.3 up
sudo ifconfig lo0 alias 127.0.1.1 up
sudo ifconfig lo0 alias 127.0.0.101 up
```

Linux routes the whole 127.0.0.0/8 to loopback, macOS only 127.0.0.1. With these aliases the f1u split-connector
(10 of 12), cu_up data-flow (3) and udp gateway cases pass as well.


## Test-count parity with Ubuntu (second pass)

Ubuntu (`jwang@192.168.100.131:/home/jwang/work/ocudu`, CMake 3.28, x86_64) registers 7590 ctest cases; macOS
registered only 7569. All 21 missing cases were attributed and restored, so both hosts now register 7590:

| cases | ctest case(s) | cause | port-introduced | restoration |
|---|---|---|---|---|
| 7 | `rlc_*_stress_test` | `if (NOT APPLE)` also dropped the `add_test()` calls (no `pthread_barrier_*`) | yes | `else()` branch registers them with `DISABLED TRUE` |
| 1 | `du_high_benchmark` | `if (NOT APPLE)` (Linux CPU affinity) | yes | same |
| 1 | `ofh_integration_test` | `if (NOT APPLE)` (AF_PACKET) | yes | same |
| 1 | `rrc_du_ref_time_r16_test.subsecond_component_is_preserved` | compiled out with `#if !defined(__APPLE__)` | yes | compiled in + runtime `GTEST_SKIP()` (+ `duration_cast` so it builds on libc++) |
| 8 | `nia2/fxt_nia2.integrity_engine_nia2_cmac/*` | upstream `#ifdef MBEDTLS_CMAC_C`; Homebrew mbedtls@2 has CMAC disabled | no | case always registered + runtime `GTEST_SKIP()` |
| 1 | `dft_processor_ci16_test` | upstream x86_64-only gate; host is arm64 | no | `elseif (APPLE)` registers it with `DISABLED TRUE` |
| 2 | `du_high_many_ues_test/du_high_many_ues_tester.*` | CMake 4.4 test discovery is `#`-delimited and the parameter label contained `#ues=` | no (toolchain) | label renamed to `nof_ues=`; both cases now run and pass |
| - | `f1ap_ref_time_provider_adapter_test.subsecond_component_encodes_losslessly` | compiled out with `#if !defined(__APPLE__)` (count-neutral: single-binary ctest entry) | yes | compiled in + runtime `GTEST_SKIP()` |
| - | `segmented_circular_map_test*` (60) | typed-test naming differs between CMake 3.28 and 4.4; count identical | no | none needed |

All eight changed files were copied to the Ubuntu machine and verified there: `cmake` + build succeed, the list is
still 7590 entries, the previously Linux-only cases are *not* disabled, and the 14 affected cases all pass.

## Stability pass (fourth pass)

The remaining SCTP flakiness was caused by a wake-up coalescing defect in the usrsctp shim: the kqueue io_broker
delivers one callback per bridge wake-up byte, but the stack can queue several notifications behind a single byte,
so the gateway receive callback must drain the socket (read until EAGAIN). Added:

- `sctp_recvmsg_nowait()` in the shim (single MSG_DONTWAIT read without the emulated rx timeout wait);
- drain loops in `sctp_network_server_impl::receive()` and `sctp_network_client_impl::receive()` (macOS-only);
- event-driven assertions in the four tests whose expectations assumed one notification per broker wake-up;
- bounded graceful-close (100 ms cap, 500 us polling) and a bounded client destructor wait with a keepalive token;
- ctest TIMEOUT 300 for the SCTP gateway tests (the 32-client cases take 90-300 s on the user-space stack).

Result: the SCTP suite (78 cases) passes 100% repeatedly on macOS and on the Ubuntu reference machine.

## SCTP transport mode override + E2E ping stall localization (2026-08-22)

`usrsctp_once_init()` now honours `OCUDU_USRSCTP_MODE=auto|udp|raw` (default auto). `sudo ctest -L sctp` without an
override switches the suite to raw mode, which hangs on macOS loopback (the kernel does not loop native SCTP
packets back to a local raw socket), so the documented invocation is `sudo OCUDU_USRSCTP_MODE=udp ctest -L sctp`.
`postrun_summary.py` prints the mode of a scan plus the raw-mode validation caveat (raw mode is validated by the
gnb end-to-end against Linux kernel-SCTP peers, not on macOS loopback).

E2E analysis of `/tmp/gnb.log` (gnb on macOS, srsUE + Open5GS on the Ubuntu host at 192.168.100.153, zmq radio;
the ping run happened at 00:08:33.9-00:09:26):

* The UL ping requests cross the gnb at a continuous 10 pps (PUSCH -> RLC -> GTPU egress, 00:08:34-00:09:24); the
  UL path does not stall.
* The DL replies enter the gnb GTPU ingress in exactly two bursts, 256 at 00:08:59.27 and 240 at 00:09:25.27
  (26 s apart) - the same two bursts the ping output shows (RTT ramps 27.5s->1.0s and 26.7s->2.1s are the
  single-flush artifact of those two bursts). The RLC DL TX SDUs mirror the GTPU ingress 1:1, and each burst goes
  over the air in ~1-2 s, so the stall sits strictly between the gnb N3 egress (continuous) and the gnb N3 ingress
  (bursty): either the Open5GS UPF stalls ~26 s per cycle or the gnb NGU receive path (kqueue io_broker wake-up)
  stalls. 256 is exactly the NGU demux `batch_size=256`. The SCTP fixes are unrelated to this stall (the user
  plane never touches SCTP); tcpdump on both N3 ends is the planned discriminator.
* Separate defect: the zmq radio runs in slow motion - the cell slot counter advances at only ~0.2-2 slots/s
  (should be 1000/s) and degrades over the 54-minute run (at the end the zmq RX receives 12 samples per ~58 s).
  10 pps still works because each slot packs many packets; this caps E2E throughput and needs its own
  investigation (suspects: clock drift between the two hosts, the zmq tx_time pacing).

## E2E 27 s ping stall: root cause found and fixed (2026-08-22, capture correlation)

Four-way captures (UE tun, N3 on the CN host, N3 on the macOS NIC, plus `/tmp/gnb.log`) of a fresh 500-ping run
were aligned per packet. With the Ubuntu gnb (192.168.100.131) against the same UE/CN the ping is uniform
300-400 ms, so the stall is macOS-gnb-side. The captures show:

* the requests cross the gnb continuously (10 pps at every hop);
* the UPF answers immediately and its replies arrive at the macOS NIC **continuously** (~10-18 pps);
* but the gnb GTP-U ingress logs them in bursts of 256/118/69/53 (27.4 s, 12.2 s, 8.8 s, 4.1 s apart), and the UE
  receives exactly those same bursts - each reply waits in the gnb kernel socket buffer until its batch is
  released.

Root cause: the macOS `recvmmsg()` emulation in `lib/gateways/udp_network_gateway_impl.cpp` (macOS has no
recvmmsg; `MSG_WAITFORONE` was #defined to 0). The emulation looped `recvmsg` up to `rx_max_mmsg` (=256) times
on the blocking N3 socket, so after the first datagram every further `recvmsg` blocked for one inter-packet gap
(~100 ms at 10 pps). Collecting a full batch therefore held the receive callback - and the whole io_rx_executor -
for ~25 s, and every datagram's delivery waited until its batch completed (the ~1 s SO_RCVTIMEO lull ended the
last partial batches). That is exactly the ping's two decreasing RTT ramps: a batch flush artifact.

Fix: the emulation now waits for the first datagram (MSG_WAITFORONE semantics) and then drains with MSG_DONTWAIT
until EAGAIN. The io_broker level-triggered rearm then drives successive short callbacks, like Linux. Bonus:
the same defect caused the two `f1u_*_split_connector_test.destroy_bearer_disconnects_and_stops_rx` failures
(destroy blocked behind a 256-deep batch), which now pass - the failed count drops from 52 to 50.

Still open: the slow-motion zmq radio (~2.6 slots/s, smooth - not bursty; the zmq code uses ZMQ_DONTWAIT, so it is
a different mechanism, likely the tx_time pacing between the two hosts), and the udp v6 dual-stack case (#3033).

## Load-induced flakes found by parallel stress runs (2026-08-22)

`ctest -j 8` (and even `-j 4` over the SCTP label) surfaced flakes that the sequential `make test` never shows.
Three families were fixed; one limitation is documented:

1. **Fixed 10-iteration broker-drive loops** (`sctp_network_server_test`, `sctp_network_client_test`): when the
   user-space stack delivers slowly under CPU load (a lost chunk is recovered by the retransmission timers, taking
   seconds), 10 wake-ups were not enough and the cases failed. All drive loops now poll until the expected state or
   a generous deadline (2-5 s) expires.

2. **Unbounded waits in the link-test fixture**: waiting for all server associations and the multi-client data pops
   now use bounded waits (20 s cvar deadline, 10 s timed `pop_blocking`). This also fixed the timed
   `pop_blocking(elem, wait_time)` wrapper in `mutexed_mpmc_queue.h`, which passed `&success` as the element
   reference (it never compiled before, so nothing used it).

3. **`rlc_tx_tm_test.test_tx` hol_toa bounds**: the test asserted `hol_toa` strictly greater than the wall time
   captured immediately before `handle_sdu()`. macOS `steady_clock` ticks at ~41 ns and the TM write path can land
   on the same tick, so the recorded time equals `t_start` (reproduced once in ~150 runs; Linux clocks are
   ns-granular and never show it). The bounds are now inclusive, which is the correct statement anyway (arrival
   within `[t_start, t_end]`). The same pattern exists in `rlc_um_test`/`rlc_tx_am_test` but has not been observed
   to collide there (longer call paths).

4. **Remaining limitation - parallel ctest runs over the SCTP label**: each ctest case is its own process and
   `pick_udp_tunneling_port()` probes from the fixed encapsulation port 9899. Two processes starting
   simultaneously can both probe 9899 as free and then both bind it (usrsctp sets SO_REUSEADDR), so macOS delivers
   each datagram to only one of them and the other's associations stall. Sequential runs (the authoritative
   `make test`) are unaffected; `ctest -j N` over SCTP cases can still flake for this reason. Deliberately not
   changed: the gnb E2E needs the deterministic 9899 default to interop with the Linux kernel SCTP peer
   (`sysctl net.sctp.udp_port`).

## The 50 failures + 1 crash: all fixed (2026-08-22)

The remaining 50 failures and the crash were grouped by root cause and fixed one group at a time:

1. **`unique_thread_test` crash (fixed).** `pthread_getname_np` reports the process name for the main thread on Linux
   but an empty string on macOS until the thread names itself, so `this_thread_name() != t.get_name()` aborted. The
   test names the main thread at the start of `main()` (macOS-only), matching the Linux behaviour.

2. **`EthFramePoolTestSuite.read_after_write_should_return_correct_data` (32 cases, fixed in the test).** The frame
   pool's pending order follows the release order of the scoped buffers, which for a destroyed `std::vector` is
   implementation-defined: libc++ (macOS) destroys the elements in reverse order, libstdc++ in forward order, so the
   read burst came back reversed on macOS and the positional size/data comparison failed for every multi-frame case
   (MTU 5000/1500; MTU 9000 always writes a single frame and was unaffected). The test now matches every written
   frame against the set of read frames instead of comparing positionally. The pool itself makes no ordering
   guarantee, so production is unaffected (frames are independent).

3. **`cu_cp_rrc_inactive_test` (16 cases, production bug fixed).** `rrc_du_metrics_aggregator::get_mean_nof_rrc_connections()`
   dereferenced the map's `end()` iterator in its single-measurement branch - undefined behaviour that happens to
   read the last value under libstdc++ but reads 0 under libc++ (macOS), so `mean_nof_inactive_rrc_connections` was
   0 instead of 1. Fixed with `rbegin()->second`. All 404 cu_cp-labelled cases pass.

4. **`udp_network_gateway_tester.when_v6_config_valid_then_trx_succeeds` (production bug fixed).** `sendmsg()` with
   `msg_namelen = sizeof(sockaddr_storage)` (128) fails with EINVAL on macOS for IPv6 destinations (verified with a
   standalone probe: only the exact `sizeof(sockaddr_in6)` is accepted), so the ::1 dual-stack test never delivered
   a datagram. New `sockaddr_length()` helper derives `msg_namelen` from the address family; Linux accepts both.

5. **`text_formatter_test` (fixed in the test).** The formatter maps the entry time point to wall clock via
   `tp - high_resolution_clock::now() + system_clock::now()`. On Linux the `high_resolution_clock` epoch coincides
   with the system clock epoch, so the fixed 50000 us test time point prints as `1970-01-01T00:00:00.050000`; on
   macOS `high_resolution_clock` is `steady_clock` (epoch = system boot), so the printed time depends on the
   machine's uptime. The test now validates the timestamp shape and compares the rest of the golden line verbatim.

## SCTP multi-client link tests: the N x 2 s teardown and the burst-setup retransmissions (2026-08-22)

The `sctp_network_link_test` multi-client cases used to take 2.02 s / 11.4 s / 20.4 s / 75.5 s on macOS versus
0.04 s on Ubuntu. Two independent causes:

1. **Client destructor dead wait (fixed).** `sctp_network_client_impl::~` reset the keepalive token and dropped the
   io_broker subscription *before* waiting for SCTP_SHUTDOWN_COMP, so the receive callback could never deliver the
   notification and every connected teardown burned the full 2 s cap. Fixed by waiting first (the handshake
   normally completes in milliseconds) and only then cancelling the token and deregistering (the deregistration
   completes once an in-flight callback finished, so members stay safe). The link-test fixture now also destroys
   the clients while the server is still running. Result: 1/4-client cases 2.02 s / 11.4 s -> 0.02 s, and the
   whole client test suite drops to ~10 ms per case. Ubuntu unchanged (78/78, 4.45 s total).

2. **Burst-setup chunk drops: root cause found (usrsctp 0.9.5.0 send path), mitigated in the test.** With 32
   clients connecting at once, a few associations' INIT/INIT_ACK/COOKIE_ACK (and in the send case, DATA) chunks
   were lost on the usrsctp UDP-encapsulation loopback and recovered through the SCTP retransmission timers
   (T1-INIT 3 s / T1-cookie 1 s), which produced the ~1.0/3.4/6.6 s COMM_UP batches and the 13-50 s setup times.
   The kernel SCTP stack on Linux does not lose anything on loopback (0.04 s).

   Root cause (proven with usrsctp's own SCTP_DEBUG chunk trace enabled through a temporary debug_printf hook):
   usrsctp's built-in UDP transport funnels every datagram of every socket of the process through ONE shared UDP
   socket, and sends it with `sendmsg(MSG_DONTWAIT)` (user_socket.c `sctp_userspace_ip_output`). On macOS
   loopback-to-self a send during the connect burst transiently fails with EAGAIN (errno 35, "IP output returns
   35"), and usrsctp DROPS the chunk ("Gak send error 35") without a send-layer retry - the packet accounting in
   the trace shows e.g. 32 INITs sent / 31 received. Two amplifiers: (a) unbound client sockets advertise every
   local address of the host in the INIT (all the lo0 aliases + the LAN address), so every association becomes
   multi-homed and each COOKIE_ECHO processing emits one HEARTBEAT per peer address, multiplying the burst; (b) the
   32 connects run in a tight loop. Mitigation in the link-test fixture (macOS test-duration only, correctness is
   unaffected): bind each client to 127.0.0.1 (single-homed) and pace the connects by 1 ms. Result: the 32-client
   cases take ~60 ms typically and ~1.3 s at worst (one lost chunk recovered by the 1 s T1-cookie timer), down from
   13-50 s. A complete fix would require patching usrsctp's send path (retry on EAGAIN or blocking sends) or
   switching the shim to the AF_CONN/conn_output transport, both of which mean building usrsctp from source - left
   as a follow-up.
