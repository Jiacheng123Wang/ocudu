# macOS E2E analysis toolkit

Tools and report for the gnb E2E latency analysis (macOS vs Ubuntu gnb, ZMQ transport, srsUE +
Open5GS core). This directory is the reusable part of the analysis; the multi-GB raw captures were kept in a
local workspace during the analysis and discarded once the report was committed.

## Reference transport (wired ZMQ)

Wi-Fi adds ~70ms per 64KB TCP burst to the ZMQ stream on a weak link and dominates the E2E
latency (see REPORT.md). For any E2E measurement, run the ZMQ stream over the direct cable:

| host | wired IP | role |
|---|---|---|
| mac (this machine) | 198.19.0.1 | gnb (analysis target) |
| 192.168.100.153 | 198.19.0.2 | UE host (srsRAN_4G + Open5GS) |
| 192.168.100.131 | 198.19.0.3 | Ubuntu gnb (reference) |

One cable, switched between the mac and 131 as needed. The UE configs on 153 point
`rx_port` at the wired address of the gnb under test (`ue_ocudu_zmq_macOS.conf` ->
`tcp://198.19.0.1:2000`, `ue_ocudu_zmq_Ubuntu.conf` -> `tcp://198.19.0.3:2000`).

## Capturing a round

gnb (both hosts, from the repo checkout, `make test` green):

```sh
# analysis build with the [zmq-probe] instrumentation
cmake -S . -B build_probes -DENABLE_FLOW_PROBES=ON -DCMAKE_BUILD_TYPE=Release
make -C build_probes gnb
# configs/gnb_zmq.yaml: log: lib_level: info  (otherwise the ALL-logger dl/ul probes are dropped)
./build_probes/apps/gnb/gnb -c configs/gnb_zmq.yaml > /tmp/gnb_mac.log 2>&1   # gnb.log -> /tmp/gnb.log
```

tcpdump on both ends (epoch timestamps, headers only):

```sh
sudo tcpdump -i <iface> -tt -s 96 -w /tmp/zmq_<side>.pcap 'tcp port 2000 or tcp port 2001'
```

ping from the UE (153) to the UPF, 3x120 pings per round:

```sh
ping -D -i 1 -c 120 10.45.0.1 | tee /tmp/ping_<round>_<n>.log
```

Clock sanity on all hosts (pcaps self-align via matching seqs; this is a cross-check):

```sh
date -u; ssh jwang@192.168.100.153 'date -u'; ssh jwang@192.168.100.131 'date -u'
```

## Scripts

- `parse_probes.py <gnb.log> ...` - parse the `[zmq-probe]` lines from gnb logs: lockstep
  reply-rate, reply-age, buffer-empty-wait/full-block, zmq-send-block, dl slot rate and
  per-stage anomalies; prints distributions per metric.
- `pcap_analyze.py <pcap> <label>` - reconstruct the ZMQ lockstep from a pcap: per-reply
  size, segment count, transfer duration, inter-request gaps, retransmissions. Requests are
  the 5-byte segments on ports 2000 (DL) / 2001 (UL).
- `pcap_stream.py <pcap> <label>` - streaming variant of pcap_analyze for multi-GB captures.
- `tcp_ack.py <pcap> <label>` - per-reply TCP behavior from the gnb side: ACK latency,
  ACK inter-arrival gaps, pure-ACK counts, retransmissions.
- `correlate.py <gnb.log> <ue.pcap> <label>` - aligns the gnb log's request-received events
  with the UE pcap's request sends (handles clock skew) and decomposes the cycle into
  gnb empty-wait / wire / UE hold.
- `dump_slow.py <pcap> <label> <threshold_ms>` - dumps the full bidirectional packet
  timeline of a slow DL reply (for eyeballing stalls).
- `dump_dl.py <pcap> <label>` - dumps a couple of DL reply timelines.

Notes: pcap request/reply grouping assumes the srsRAN ZMQ framing (5-byte requests, port 2000
= DL gnb REP, port 2001 = UL gnb REQ). The scripts parse `tcpdump -tt -nn -S` output; they
work on macOS and Linux tcpdump. PCAP IPs are hardcoded to the lab hosts (192.168.100.x /
198.19.0.x) - adjust if the topology changes.
