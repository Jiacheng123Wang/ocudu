#!/usr/bin/env python3
"""Per-DL-reply TCP analysis from the gnb-side pcap: send timeline, ACK latency, stalls."""
import subprocess, sys, re, statistics

LINE = re.compile(
    r'^(\d+\.\d{6}) IP (\d+\.\d+\.\d+\.\d+)\.(\d+) > (\d+\.\d+\.\d+\.\d+)\.(\d+): '
    r'Flags \[([^\]]*)\], (?:seq (\d+)(?::(\d+))?, )?ack (\d+), win (\d+).*?, length (\d+)$')

def load(pcap):
    out = subprocess.run(['tcpdump', '-tt', '-nn', '-S', '-r', pcap],
                         capture_output=True, text=True, errors='replace').stdout
    snd, acks, reqs = [], [], []
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        t = float(m.group(1))
        src, sport = m.group(2), int(m.group(3))
        dst, dport = m.group(4), int(m.group(5))
        seq = int(m.group(7)) if m.group(7) else None
        end = (int(m.group(8)) if m.group(8) else seq)
        ack = int(m.group(9))
        length = int(m.group(11))
        if src == '192.168.100.153' and dport == 2000:
            if length == 5:
                reqs.append((t, ack))
            else:
                acks.append((t, ack, length))
        elif sport == 2000 and dst == '192.168.100.153' and length > 0:
            snd.append((t, seq, end))
    return snd, acks, reqs

def main():
    pcap, label = sys.argv[1], sys.argv[2]
    snd, acks, reqs = load(pcap)
    print(f'== {label}: {len(snd)} data segs, {len(acks)} UE acks, {len(reqs)} requests')
    # For each reply (between req i and req i+1): gather send events and ACK events.
    ack_lat = []       # (ack_t - send_t) per acknowledged segment
    stall = []         # gaps between consecutive ACK advances
    acks_per_reply = []
    retrans = 0
    seen = {}
    for p in snd:
        key = (p[1], p[2])
        if key in seen:
            retrans += 1
        seen[key] = 1
    print(f'  retransmitted segments: {retrans}')
    for i in range(len(reqs) - 1):
        t0, t1 = reqs[i][0], reqs[i + 1][0]
        segs = [p for p in snd if t0 <= p[0] <= t1]
        ac = [(t, a) for t, a, _ in acks if t0 <= t <= t1]
        acks_per_reply.append(len(ac))
        if not segs:
            continue
        # track ACK latency: when ack field advances past a segment's end, record latency
        base = min(p[1] for p in segs)
        prev_ack = None
        for t, a in ac:
            if prev_ack is None:
                prev_ack = a
                continue
            if a > prev_ack:
                # this ACK acknowledges up to 'a'; find last sent segment <= a
                covered = [p for p in segs if p[1] < a]
                if covered:
                    last_send = max(p[0] for p in covered)
                    ack_lat.append((t - last_send) * 1000)
                stall.append((t - prev_t) * 1000 if 'prev_t' in locals() else 0)
                prev_ack = a
            prev_t = t
    def s(v):
        v = sorted(v)
        if not v:
            return 'n/a'
        return (f'n={len(v)} med={v[len(v)//2]:.1f}ms p90={v[int(len(v)*.9)]:.1f}ms '
                f'p99={v[int(len(v)*.99)]:.1f}ms max={v[-1]:.1f}ms')
    print(f'  ACK latency (send->ack):     {s(ack_lat)}')
    print(f'  ACK inter-arrival gaps:      {s(stall)}')
    v = sorted(acks_per_reply)
    print(f'  pure ACKs per reply: n={len(v)} med={v[len(v)//2]} p90={v[int(len(v)*.9)]} max={v[-1]}')

if __name__ == '__main__':
    main()
