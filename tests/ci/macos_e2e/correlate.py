#!/usr/bin/env python3
"""Correlate gnb probe log (tx channel) with UE-side pcap DL requests, per round.

For each DL request:
  - UE sends request (pcap, UE clock)
  - gnb logs request-received (gnb log, gnb clock)
  - gnb sends reply (log time of buffer-empty-wait event OR request time + empty wait; also plain replies)
  - first/last DL data segment at UE (pcap)
Decompose the UE inter-request gap.
"""
import subprocess, sys, re, datetime, statistics

LOG_TS = re.compile(r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)\s.*\[zmq-probe\] tx=\S+ (.*)$')
PCAP_TS = re.compile(r'^(\d+\.\d{6}) IP ([\d.]+)\.(\d+) > ([\d.]+)\.(\d+): .*length (\d+)$')

def log_epoch_us(s):
    dt = datetime.datetime.fromisoformat(s).replace(tzinfo=datetime.timezone.utc)
    return int(dt.timestamp() * 1e6)

def load_log(path):
    reqs, sents = [], []  # (epoch_us, extra)
    with open(path, errors='replace') as f:
        for line in f:
            m = LOG_TS.match(line)
            if not m:
                continue
            t = log_epoch_us(m.group(1))
            body = m.group(2)
            if 'request-received' in body:
                reqs.append(t)
            elif 'buffer-empty-wait' in body:
                w = int(re.search(r'buffer-empty-wait=(\d+)us', body).group(1))
                sents.append((t, w))
            elif 'zmq-send-block' in body:
                pass
    return reqs, sents

def load_pcap(path):
    out = subprocess.run(['tcpdump', '-tt', '-nn', '-S', '-r', path],
                         capture_output=True, text=True, errors='replace').stdout
    reqs, datas = [], []  # (epoch_us)
    for line in out.splitlines():
        m = PCAP_TS.match(line)
        if not m:
            continue
        t = int(float(m.group(1)) * 1e6)
        src, sport, dst, dport, l = m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6))
        if src == '192.168.100.153' and dport == 2000 and l == 5:
            reqs.append(t)
        elif src != '192.168.100.153' and sport == 2000 and l > 0:
            datas.append(t)
    return reqs, datas

def align(log_reqs, pcap_reqs):
    """Find shift k (pcap index = log index + k) and clock offset minimizing median diff."""
    best = None
    for k in range(0, 60):
        offs = [log_reqs[i] - pcap_reqs[i + k] for i in range(min(300, len(log_reqs) - k, len(pcap_reqs) - k))]
        if not offs:
            continue
        med = statistics.median(offs)
        spread = statistics.median(abs(o - med) for o in offs)
        if best is None or spread < best[0]:
            best = (spread, k, med)
    return best[1], best[2]

def main():
    log_path, pcap_path, label = sys.argv[1], sys.argv[2], sys.argv[3]
    lreqs, lsents = load_log(log_path)
    preqs, pdatas = load_pcap(pcap_path)
    k, off = align(lreqs, preqs)
    print(f'{label}: gnb log reqs={len(lreqs)}, pcap reqs={len(preqs)}, shift={k}, '
          f'gnb-clock - ue-clock = {off / 1e3:.1f} ms')
    n = min(len(lreqs) - k, len(preqs) - k, 5000)
    lreqs = lreqs[k:]
    sent_by_req = {}
    si = 0
    for i in range(len(lreqs)):
        while si < len(lsents) and lsents[si][0] < lreqs[i]:
            si += 1
        if si < len(lsents):
            sent_by_req[i] = lsents[si]
    req_gap, gnb_wait, wire, ue_hold = [], [], [], []
    for i in range(1, n):
        rt_log = lreqs[i]           # gnb receives request
        rt_ue = preqs[i + k]            # UE sends request
        # reply send time at gnb (approx): request time + empty wait (if logged); else request time
        send_t = rt_log
        if i in sent_by_req:
            send_t, w = sent_by_req[i]
        # first data seg arrival at UE (pcap)
        first = next((t for t in pdatas if rt_ue <= t <= preqs[i + k + 1]), None)
        last = max((t for t in pdatas if rt_ue <= t <= preqs[i + k + 1]), default=None)
        if first is None or last is None:
            continue
        req_gap.append(preqs[i + k + 1] - preqs[i + k - 1])
        gnb_wait.append(send_t - rt_log)
        wire.append(last - rt_ue)
        ue_hold.append(preqs[i + k + 1] - last)
    def s(v):
        v = sorted(v)
        if not v:
            return 'n/a'
        return f'n={len(v)} med={v[len(v)//2]/1e3:.1f}ms mean={sum(v)/len(v)/1e3:.1f}ms p90={v[int(len(v)*.9)]/1e3:.1f}ms max={v[-1]/1e3:.1f}ms'
    print(f'  UE req period (pcap):      {s(req_gap)}')
    print(f'  gnb empty-wait (log):      {s(gnb_wait)}')
    print(f'  req->last seg at UE (wire+gnb): {s(wire)}')
    print(f'  last seg -> next req (UE): {s(ue_hold)}')

if __name__ == '__main__':
    main()
