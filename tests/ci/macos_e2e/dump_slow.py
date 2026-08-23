#!/usr/bin/env python3
"""Find slow DL replies (gnb->UE, port 2000) and dump the full bidirectional timeline."""
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
            pkts.append(dict(t=float(m.group(1)), src=m.group(2), dst=m.group(4),
                             seq=int(m.group(7)), length=int(m.group(11)), win=int(m.group(10))))
    return pkts

def main():
    pcap, label, want = sys.argv[1], sys.argv[2], int(sys.argv[3])
    pkts = load(pcap)
    reqs = [p for p in pkts if p['src'] == '192.168.100.153' and p['length'] == 5]
    print(f'{label}: {len(reqs)} requests')
    for i in range(1, len(reqs) - 1):
        t0, t1 = reqs[i]['t'], reqs[i + 1]['t']
        data = [p for p in pkts if t0 <= p['t'] < t1 and p['dst'] == '192.168.100.153' and p['length'] > 0]
        if not data:
            continue
        last = max(p['t'] for p in data)
        dur = (last - t0) * 1000
        if dur < want:
            continue
        size = sum(p['length'] for p in data)
        print(f'--- slow reply #{i}: dur={dur:.1f}ms size={size} start={t0:.3f}')
        ev = []
        for p in pkts:
            if t0 - 0.01 <= p['t'] <= t1:
                d = 'gnb->UE' if p['dst'] == '192.168.100.153' else 'UE->gnb'
                ev.append((p['t'], d, p['length'], p['win']))
        ev.sort()
        for t, d, l, w in ev:
            print(f'    {(t - t0) * 1000:9.3f}ms {d:7s} len={l:6d} win={w}')
        return

if __name__ == '__main__':
    main()
