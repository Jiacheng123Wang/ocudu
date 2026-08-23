#!/usr/bin/env python3
"""Streaming pcap analyzer for the wired-run captures (large files)."""
import subprocess, sys, re, statistics

LINE = re.compile(
    r'^(\d+\.\d{6}) IP (\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+): '
    r'Flags \[([^\]]*)\], (?:seq (\d+)(?::(\d+))?, )?ack (\d+), win (\d+).*?, length (\d+)$')

def stream(pcap, filt):
    proc = subprocess.Popen(['tcpdump', '-tt', '-nn', '-S', '-r', pcap, filt],
                            stdout=subprocess.PIPE, text=True, errors='replace')
    for line in proc.stdout:
        m = LINE.match(line)
        if not m:
            continue
        t = float(m.group(1))
        src, sport = m.group(2), int(m.group(3))
        dst, dport = m.group(4), int(m.group(5))
        seq = int(m.group(7)) if m.group(7) else 0
        end = int(m.group(8)) if m.group(8) else seq
        ack = int(m.group(9))
        win = int(m.group(10))
        l = int(m.group(11))
        yield t, src, sport, dst, dport, seq, end, ack, win, l
    proc.wait()

def main():
    pcap, label = sys.argv[1], sys.argv[2]
    filt = sys.argv[3] if len(sys.argv) > 3 else 'port 2000 or port 2001'
    # per-port: requests (len 5 from the requester) and data (from the responder)
    reqs = {'2000': [], '2001': []}
    data = {'2000': [], '2001': []}
    acks = {'2000': [], '2001': []}
    n = 0
    for t, src, sport, dst, dport, seq, end, ack, win, l in stream(pcap, filt):
        n += 1
        key = str(dport if dport in (2000, 2001) else sport)
        if key == '2000':
            # DL: UE requests (len 5 -> gnb:2000), gnb replies
            if dst == 2000 and l == 5 and src != '' :
                pass
            if l == 5 and dport == 2000:
                reqs[key].append((t, ack))
            elif l > 0 and sport == 2000:
                data[key].append((t, end, l))
            else:
                acks[key].append((t, ack, win))
        else:  # 2001: gnb requests (len 5 -> UE:2001), UE replies
            if l == 5 and dport == 2001:
                reqs[key].append((t, ack))
            elif l > 0 and sport == 2001:
                data[key].append((t, end, l))
            else:
                acks[key].append((t, ack, win))
    print(f'== {label}: {n} packets parsed')
    for key in ['2000', '2001']:
        rs = reqs[key]; ds = data[key]
        print(f'-- port {key}: {len(rs)} requests, {len(ds)} data segs')
        # group data by request windows
        rows = []
        di = 0
        for j, (rt, _) in enumerate(rs):
            nxt = rs[j + 1][0] if j + 1 < len(rs) else float('inf')
            sz = 0; first = None; last = None; nsegs = 0
            while di < len(ds) and ds[di][0] < rt:
                di += 1
            while di < len(ds) and ds[di][0] <= nxt:
                if first is None:
                    first = ds[di][0]
                last = ds[di][0]; sz += ds[di][2]; nsegs += 1; di += 1
            if sz:
                rows.append((rt, sz, nsegs, (last - rt) * 1000))
        if len(rows) > 1:
            gaps = sorted([(rows[i + 1][0] - rows[i][0]) * 1000 for i in range(len(rows) - 1)])
            print(f'  inter-req gap ms: n={len(gaps)} med={gaps[len(gaps)//2]:.2f} p90={gaps[int(len(gaps)*.9)]:.2f} max={gaps[-1]:.2f}')
        for name, idx in [('size', 1), ('nsegs', 2), ('dur ms', 3)]:
            v = sorted(r[idx] for r in rows)
            if v:
                print(f'  {name}: n={len(v)} min={v[0]} med={v[len(v)//2]:.1f} mean={sum(v)/len(v):.1f} p90={v[int(len(v)*.9)]:.1f} p99={v[int(len(v)*.99)]:.1f} max={v[-1]:.1f}')
        wins = [w for t, a, w in acks[key] if w > 0]
        if wins:
            w = sorted(wins)
            print(f'  ack win: n={len(w)} med={w[len(w)//2]} p10={w[int(len(w)*.1)]} p90={w[int(len(w)*.9)]} max={w[-1]}')

if __name__ == '__main__':
    main()
