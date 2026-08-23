# macOS vs Ubuntu gnb — E2E comparison analysis (2026-08-23)

Data: Round A = mac gnb (192.168.100.125) + srsUE (192.168.100.153), two captures
(v1 07:30–07:38 CST, v2 08:05–08:21 CST). Round B = Ubuntu gnb (192.168.100.131)
+ same srsUE, 07:42–07:50 CST. gnb commit f42e5b981b (probe instrumentation) on both hosts.
Ping: from the UE (153) to UPF 10.45.0.1, 3×120 pings per round, `-D` timestamps.

## 1. Ping (E2E RTT, UE -> UPF 10.45.0.1)

| round | group | min / avg / max (ms) |
|---|---|---|
| mac v1 | 1 | 421 / 972 / 2105 |
| mac v1 | 2 | 405 / 1351 / 10062 |
| mac v1 | 3 | 396 / 1140 / 2752 |
| mac v2 | 1 | 396 / 841 / 2011 |
| mac v2 | 2 | 308 / 978 / 7343 |
| mac v2 | 3 | 378 / 1006 / 2422 |
| Ubuntu | 1 | 254 / 524 / 1145 |
| Ubuntu | 2 | 155 / 553 / 2076 |
| Ubuntu | 3 | 191 / 528 / 2228 |

Ubuntu is ~2x faster on average; mac min RTT ~400ms vs Ubuntu ~155–254ms.

## 2. gnb probe logs ([zmq-probe], f42e5b981b)

| metric | mac (v1) | Ubuntu |
|---|---|---|
| tx reply-rate (lockstep rounds/s) | p50 13.0 | p50 51.9 |
| rx reply-rate | p50 19.0 | p50 55.6 |
| tx buffer-empty-wait | p50 1.5ms, p99 110ms | p50 1.5ms, p99 2.7ms |
| rx reply-age (UL leg) | p50 46.7ms, p99 188ms | p50 17.0ms, p99 33.6ms |
| zmq-send-block events (>2ms) | 0 | 0 |
| buffer-full-block events | 0 | 0 |
| dl/ul lower-PHY probes | absent (lib_level defaults to warning; fixed in configs after the runs) | absent |

- The producer keeps up on BOTH hosts (empty-wait p50 ≈ 1.5ms): the old hypothesis
  "Ubuntu produces DL slots faster" is REFUTED.
- The 8MB socket-buffer fix works: zero zmq_send blocking on mac.
- The lockstep rate differs 4x: 13–19/s (mac) vs 52–56/s (Ubuntu).

## 3. pcap analysis

DL stream (port 2000, gnb->UE replies):

| metric | mac v2 (gnb side) | Ubuntu (gnb side) |
|---|---|---|
| reply size | med 184KB (2 slots), max 385KB | med 92KB (1 slot), p99 184KB |
| inter-request gap | med 77.9ms | med 18.3ms |
| wire transfer (req -> last seg) | med 55ms | med 6ms |
| segments per reply | med 128 (MSS 1448) | med 3 (TSO super-segments) |
| retransmissions | 1520 | ~0 |

UL stream (port 2001, UE->gnb replies): size med 92KB both; duration med 37ms (mac,
from gnb side) vs 16ms (Ubuntu).

UE-side checks:
- UE internal arrival->ACK delay: med **0.0ms** on BOTH rounds — the UE TCP stack
  ACKs instantly; the UE is exonerated.
- UE PHY pulls 11520 samples (1 slot) every 39.4ms (mac) / 17.4ms (Ubuntu) — a
  FOLLOWER of the DL delivery rate, not the pacemaker.

## 4. Root cause: the mac's Wi-Fi TX path

All three hosts are on Wi-Fi (mac en1, 131 wlo1, 153 wlo1).

One-way measurements (matched segment seqs, both-end pcaps):

- mac -> UE DL data: median +50ms (after removing the ~21ms clock skew: **~70ms one-way**)
- UE -> mac UL data: ~4ms
- 131 -> UE / UE -> 131: both small (skew-dominated, single-digit ms)

Per-reply timeline (mac v2, DL, one 184KB reply): mac kernel queues a 64KB burst
(captured at t+2.9ms) -> nothing on the wire for ~70ms -> burst arrives at the UE
at t+73ms -> UE ACKs in 0.0ms -> ACK back at mac at t+73.1ms -> mac queues the next
64KB burst -> ... 184KB = 3 window-fulls x ~70ms = 210ms.

Idle check right now: `ping 192.168.100.153` from the mac gives
min/avg/max = 9.9 / 51.3 / 93.8 ms — a LAN that bad only happens on a degraded Wi-Fi
link. `system_profiler SPAirPortDataType`: 5GHz ch149, RSSI **-67 dBm**; awdl0 is
UP (AWDL causes periodic TX stalls on macOS).

Conclusion: the E2E gap between the mac gnb and the Ubuntu gnb is NOT in the gnb
code, the probe data, the UE, or the TCP/ZMQ stack logic. It is the mac's Wi-Fi
transmit path adding ~70ms per 64KB TX burst. Ubuntu (good Wi-Fi link, TSO bursts)
delivers 92KB replies in 6ms and runs the lockstep at ~55 rounds/s; the mac's weak
Wi-Fi + AWDL turns every window-full into a ~70ms round and collapses the lockstep
to 13-19 rounds/s, which multiplies into ~1s ping RTT.

## 5. Recommendations

1. Put the mac on Ethernet (USB-C adapter) and re-run Round A; expect ping to drop
   to Ubuntu's ~530ms or below. This is the definitive confirmation.
2. If Ethernet is impossible: `sudo ifconfig awdl0 down` before the run, move the
   mac closer to the AP, verify the AP channel/band (5GHz -67dBm is marginal).
3. Optional re-run with `lib_level: info` (already added to configs/gnb_zmq.yaml on
   both hosts) to capture the dl production-rate probes for completeness.
4. Keep the 8MB socket-buffer fix (it is what keeps the gnb channel loop healthy on
   a slow link); revert only the probe instrumentation commit f42e5b981b when done.

## Files

Raw captures and probe logs (hundreds of MB to GB) were kept in a local analysis
workspace during the analysis and discarded afterwards; the reusable scripts and
this report live in `tests/ci/macos_e2e/` in the repository.

mac_A/ (v1), mac_A2/ (v2: zmq_mac_gnb.pcap 155MB, gnb.log.v2), ubuntu_B/,
ue/ (zmq_ue_A_v1.pcap, zmq_ue_A_v2.pcap, zmq_ue_B.pcap, ue logs, ping logs).
Scripts: parse_probes.py, pcap_analyze.py, tcp_ack.py, correlate.py, dump_slow.py.

## 6. Wired re-run (direct cable, 198.19.0.0/24) — Wi-Fi hypothesis CONFIRMED

Same gnb code (f42e5b981b), same UE, ZMQ now over a direct Ethernet cable:
Round A wired = mac gnb (198.19.0.1) <-> UE (198.19.0.2); Round B wired = Ubuntu gnb
(198.19.0.3) <-> UE. Wi-Fi still up for management/GTP. Captures:
wired_A/zmq_mac_gnb.pcap (4.6GB), wired_ue/zmq_ue_A.pcap (715MB),
wired_B/zmq_ubuntu_gnb.pcap (496MB), wired_ue/zmq_ue_B.pcap (481MB), probe logs in
wired_A/gnb_wired.log and wired_B/gnb_wired.log (lib_level: info -> dl probes present).

Ping (UE -> UPF 10.45.0.1):

| round | group | min / avg / max (ms) |
|---|---|---|
| mac wired | 1 | 29 / 103 / 242 |
| mac wired | 2 | 36 / 108 / 247 |
| mac wired | 3 | 27 / 100 / 438 |
| Ubuntu wired | 1 | 45 / 133 / 583 |
| Ubuntu wired | 2 | 46 / 146 / 454 |
| Ubuntu wired | 3 | 52 / 132 / 449 |

- mac: 1000ms -> 100ms (10x), Ubuntu: 530ms -> 137ms (4x). **mac now BEATS Ubuntu.**
- Lockstep rounds/s (probes): mac p50 711 (13/s on Wi-Fi = 55x), Ubuntu p50 444 (55/s = 8x).
- dl production rate: mac 719 slots/s, Ubuntu 444 slots/s (consumption-paced on both).
- rx reply-age: mac 1.32ms, Ubuntu 1.19ms (was 46.7 / 17.0ms on Wi-Fi).
- tx buffer-empty-wait events: mac 77 vs Ubuntu 20907 -> the mac's producer stays ahead;
  the Apple Silicon slot production outruns the NUC's, and with a fast link that now
  determines the lockstep rate (711 vs 444/s).
- pcap DL replies: 1 slot (92KB) both; wire transfer med 0.2ms (mac, 64 MSS segs) /
  1.4ms (Ubuntu, 3 TSO segs); inter-request gap med 1.36ms (mac) / 2.27ms (Ubuntu).
- One-way link latency: ~0.2-0.4ms both directions (the +/-16ms / +/-135ms medians in the
  cross-pcap matching are host clock skews, symmetric in both directions).

Remaining ~100ms ping floor: the zmq lockstep now costs only ~2-3ms per round; the rest
is the gnb scheduling path + GTP-U round trip to the UPF on 153 (still Wi-Fi). Optional
follow-up: capture the N3/GTP-U interface during a ping run to decompose that floor.

## 7. Final verdict

The macOS-vs-Ubuntu gnb E2E difference was the mac's Wi-Fi TX path (weak signal -67dBm
+ AWDL) adding ~70ms per 64KB TX burst. With ZMQ moved to a direct wire, the mac gnb
delivers ping avg ~100ms and a 711 rounds/s lockstep, beating the Ubuntu gnb's ~137ms /
444 rounds/s. gnb code, UE, and the ZMQ/TCP logic are all exonerated; the 8MB socket
buffer fix and the tag macos_e2e_stable stand.
