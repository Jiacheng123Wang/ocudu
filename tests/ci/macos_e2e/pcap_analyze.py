#!/usr/bin/env python3
"""Analyze ZMQ lockstep streams from a tcpdump pcap.

Usage: pcap_analyze.py <pcap> <label> [--peer <other_pcap>]

Reconstructs, per connection direction:
  - request timing (small segments: DL request = 1B on port 2000; UL request = 5B on port 2001)
  - reply messages: total payload size, segment count, first/last segment time, duration
  - inter-request gaps, ACK round trips
Outputs stats and (optionally) per-reply CSV for correlation.
"""
import subprocess, sys, re, statistics, collections

LINE = re.compile(
    r'^(\d+\.\d{6}) IP (\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+): '
    r'Flags \[([^\]]*)\], seq (\d+)(?::(\d+))?, ack (\d+), win (\d+)(?:.*?), length (\d+)$')

def load(pcap):
    out = subprocess.run(['tcpdump', '-tt', '-nn', '-S', '-r', pcap],
                         capture_output=True, text=True, errors='replace').stdout
    pkts = []
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        t = float(m.group(1))
        src, sport = m.group(2), int(m.group(3))
        dst, dport = m.group(4), int(m.group(5))
        flags = m.group(6)
        seq = int(m.group(7))
        end = int(m.group(8)) if m.group(8) else seq
        ack = int(m.group(9))
        win = int(m.group(10))
        length = int(m.group(11))
        pkts.append(dict(t=t, src=src, sport=sport, dst=dst, dport=dport,
                         flags=flags, seq=seq, end=end, ack=ack, win=win, length=length))
    return pkts

def analyze(pkts):
    # stream: port 2000 = DL (UE requests from an ephemeral port, gnb replies);
    #          port 2001 = UL (gnb requests from an ephemeral port, UE replies).
    UE = '192.168.100.153'
    streams = {'2000': {'req': [], 'data': [], 'acks': []},
               '2001': {'req': [], 'data': [], 'acks': []}}
    for p in pkts:
        if p['dport'] in (2000, 2001):
            key = str(p['dport'])
            to_ue = (p['dst'] == UE)
        elif p['sport'] in (2000, 2001):
            key = str(p['sport'])
            to_ue = (p['src'] == UE)
        else:
            continue
        s = streams[key]
        if p['length'] == 5 and ((key == '2000' and not to_ue) or (key == '2001' and to_ue)):
            s['req'].append(p)   # ZMQ request (5 bytes on both ports)
        elif p['length'] > 0:
            s['data'].append(p)
        else:
            s['acks'].append(p)  # pure ACK / control; win = peer's receive window
    return streams

def reply_stats(reqs, data):
    """Given request list (from peer) and data list (from host), group data by request order."""
    # requests and data are both time-ordered per direction. Each reply message
    # = data segments between consecutive requests (data starts after req_i, ends before req_{i+1}).
    if not reqs:
        return []
    data_by_req = []
    i = 0
    for j, r in enumerate(reqs):
        segs = []
        nxt = reqs[j + 1]['t'] if j + 1 < len(reqs) else float('inf')
        while i < len(data) and data[i]['t'] < r['t']:
            i += 1  # stale data before first request (attach phase)
        while i < len(data) and data[i]['t'] <= nxt + 1e-6:
            segs.append(data[i])
            i += 1
        if segs:
            size = sum(s['length'] for s in segs)
            data_by_req.append({
                'req_t': r['t'], 'size': size, 'nsegs': len(segs),
                'first_t': segs[0]['t'], 'last_t': segs[-1]['t'],
                'dur': segs[-1]['t'] - r['t'],
            })
    return data_by_req

def dist(vals):
    vals = sorted(vals)
    n = len(vals)
    if n == 0:
        return None
    return (n, vals[0], vals[n // 2], sum(vals) / n, vals[int(n * .9)], vals[int(n * .99)], vals[-1])

def report(name, rows):
    if not rows:
        print(f'  {name}: no replies')
        return None
    if len(rows) > 1:
        gaps = [rows[i + 1]['req_t'] - rows[i]['req_t'] for i in range(len(rows) - 1)]
        s = dist([g * 1000 for g in gaps])
        print(f'  {name} inter-request gap(ms): n={s[0]} min={s[1]:.1f} med={s[2]:.1f} mean={s[3]:.1f} p90={s[4]:.1f} p99={s[5]:.1f} max={s[6]:.1f}')
    for label, key in [('size', 'size'), ('duration (req->last seg, ms)', 'dur'), ('nsegs', 'nsegs')]:
        vals = [r[key] for r in rows]
        if key == 'dur':
            vals = [v * 1000 for v in vals]
        s = dist(vals)
        print(f'  {name} {label}: n={s[0]} min={s[1]:.0f} med={s[2]:.0f} mean={s[3]:.1f} p90={s[4]:.0f} p99={s[5]:.0f} max={s[6]:.0f}')
    return rows

def main():
    pcap = sys.argv[1]
    label = sys.argv[2]
    pkts = load(pcap)
    print(f'== {label}: {len(pkts)} packets from {pcap}')
    streams = analyze(pkts)
    for key in ['2000', '2001']:
        s = streams[key]
        reqs = sorted(s['req'], key=lambda p: p['t'])
        data = sorted(s['data'], key=lambda p: p['t'])
        print(f'-- port {key}: {len(reqs)} requests, {len(data)} data segments')
        rows = reply_stats(reqs, data)
        report(f'  reply', rows)
        # receiver's advertised window: on 2000 the UE receives (gnb sends); on 2001 the gnb receives.
        recv_ip = '192.168.100.153' if key == '2000' else None
        wins = [p['win'] for p in pkts if p['src'] == recv_ip and p['length'] == 0
                and (p['sport'] == int(key) if recv_ip else p['dport'] == int(key))]
        if wins:
            w = dist(wins)
            print(f'  receiver advertised win: n={w[0]} min={w[1]} med={w[2]} mean={w[3]:.0f} p90={w[4]} max={w[6]}')
    # retransmissions: seq seen twice
    seqs = collections.Counter()
    for p in pkts:
        if p['length'] > 0 and (p['dport'] in (2000, 2001) or p['sport'] in (2000, 2001)):
            seqs[(p['src'], p['sport'], p['dst'], p['dport'], p['seq'])] += 1
    dup = [k for k, v in seqs.items() if v > 1]
    print(f'  retransmitted/duplicated segments: {len(dup)}')
    if dup:
        for k in dup[:10]:
            print('    ', k)

if __name__ == '__main__':
    main()
