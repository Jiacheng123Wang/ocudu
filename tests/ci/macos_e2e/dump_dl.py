#!/usr/bin/env python3
"""Dump the segment/ACK timeline of a few DL replies (gnb->UE on port 2000)."""
import subprocess, sys, re

LINE = re.compile(
    r'^(\d+\.\d{6}) IP (\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+): '
    r'Flags \[([^\]]*)\], seq (\d+)(?::(\d+))?, ack (\d+), win (\d+).*?, length (\d+)$')

def load(pcap):
    out = subprocess.run(['tcpdump', '-tt', '-nn', '-S', '-r', pcap],
                         capture_output=True, text=True, errors='replace').stdout
    pkts = []
    for line in out.splitlines():
        m = LINE.match(line)
        if m:
            pkts.append((float(m.group(1)), m.group(2), m.group(4), m.group(7), m.group(8), m.group(11)))
    return pkts

def main():
    pcap, label, skip = sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0
    pkts = load(pcap)
    # DL replies: data from gnb (not 153) to port 2000; requests from 153 (len 5).
    reqs = [p for p in pkts if p[2] == '192.168.100.153' and p[5] == '5']
    shown = 0
    for i in range(1, len(reqs) - 1):
        if shown >= skip:
            pass
        t0, t1 = reqs[i][0], reqs[i + 1][0]
        segs = [p for p in pkts if t0 <= p[0] < t1 and p[2] == '192.168.100.153' and p[5] != '5' and p[5] != '0']
        acks = [p for p in pkts if t0 <= p[0] < t1 and p[1] == '192.168.100.153' and p[2] != '192.168.100.153' and p[5] == '0']
        if not segs:
            continue
        shown += 1
        if shown <= skip:
            continue
        print(f'--- {label} DL reply #{i} at t0={t0:.3f} ({len(segs)} data segs, {len(acks)} acks)')
        ev = [(p[0], 'DATA', p[5]) for p in segs] + [(p[0], 'ACK', '') for p in acks]
        ev.sort()
        for t, kind, l in ev[:40]:
            print(f'    {t - t0:8.3f}ms {kind} len={l}')
        if shown >= 2:
            break

if __name__ == '__main__':
    main()
