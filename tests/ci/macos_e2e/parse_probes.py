#!/usr/bin/env python3
"""Parse [zmq-probe] lines from mac and Ubuntu gnb logs; compare distributions."""
import re, sys, statistics

TS_RE = re.compile(r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)\s')

def parse_log(path):
    events = []  # (epoch_us, kind, dict)
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if '[zmq-probe]' not in line:
                continue
            m = TS_RE.match(line)
            if not m:
                continue
            t = m.group(1)
            # epoch from UTC string
            import datetime
            dt = datetime.datetime.fromisoformat(t)
            epoch_us = int(dt.timestamp() * 1_000_000)
            rest = line.split('[zmq-probe]', 1)[1].strip()
            if rest.startswith('tx='):
                chan, _, fields = rest.partition(' ')
                if 'reply-rate' in fields:
                    mm = re.search(r'64 replies in (\d+)ms = ([\d.]+) replies/s, mean zmq-send=([\d.]+)us', fields)
                    if mm:
                        events.append((epoch_us, 'tx_reply_rate', {
                            'elapsed_ms': int(mm.group(1)), 'rate': float(mm.group(2)),
                            'mean_send_us': float(mm.group(3))}))
                elif 'zmq-send-block' in fields:
                    mm = re.search(r'zmq-send-block=(\d+)us bytes=(\d+)', fields)
                    if mm:
                        events.append((epoch_us, 'tx_send_block', {'us': int(mm.group(1)), 'bytes': int(mm.group(2))}))
                elif 'buffer-empty-wait' in fields:
                    mm = re.search(r'buffer-empty-wait=(\d+)us reply=(\d+)samples', fields)
                    if mm:
                        events.append((epoch_us, 'tx_empty_wait', {'us': int(mm.group(1)), 'samples': int(mm.group(2))}))
                elif 'buffer-full-block' in fields:
                    mm = re.search(r'buffer-full-block=(\d+)us pushed=(\d+)samples', fields)
                    if mm:
                        events.append((epoch_us, 'tx_full_block', {'us': int(mm.group(1)), 'samples': int(mm.group(2))}))
                elif 'request-received' in fields:
                    events.append((epoch_us, 'tx_request', {}))
            elif rest.startswith('rx='):
                chan, _, fields = rest.partition(' ')
                if 'reply-rate' in fields:
                    mm = re.search(r'64 replies in (\d+)ms = ([\d.]+) replies/s, mean reply-age=([\d.]+)us', fields)
                    if mm:
                        events.append((epoch_us, 'rx_reply_rate', {
                            'elapsed_ms': int(mm.group(1)), 'rate': float(mm.group(2)),
                            'mean_age_us': float(mm.group(3))}))
                elif 'reply-age' in fields:
                    mm = re.search(r'reply-age=(\d+)us samples=(\d+)', fields)
                    if mm:
                        events.append((epoch_us, 'rx_reply_age', {'us': int(mm.group(1)), 'samples': int(mm.group(2))}))
            elif rest.startswith('dl slot='):
                mm = re.search(r'dl slot=(\d+) rx-wait=(\d+)us process=(\d+)us transmit=(\d+)us', rest)
                if mm:
                    events.append((epoch_us, 'dl_anomaly', {
                        'slot': int(mm.group(1)), 'rx_wait': int(mm.group(2)),
                        'process': int(mm.group(3)), 'transmit': int(mm.group(4))}))
            elif rest.startswith('dl rate'):
                mm = re.search(r'64 slots in (\d+)ms = ([\d.]+) slots/s', rest)
                if mm:
                    events.append((epoch_us, 'dl_rate', {'elapsed_ms': int(mm.group(1)), 'rate': float(mm.group(2))}))
            elif rest.startswith('ul recv-wait'):
                mm = re.search(r'ul recv-wait=(\d+)us', rest)
                if mm:
                    events.append((epoch_us, 'ul_recv_wait', {'us': int(mm.group(1))}))
    return events

def stats(vals):
    vals = sorted(vals)
    n = len(vals)
    if n == 0:
        return None
    def pct(p):
        return vals[min(n - 1, int(n * p / 100))]
    return {
        'n': n, 'min': vals[0], 'p50': pct(50), 'mean': sum(vals) / n,
        'p90': pct(90), 'p99': pct(99), 'max': vals[-1],
    }

def summarize(events, kind, key):
    vals = [e[2][key] for e in events if e[1] == kind]
    return stats(vals)

for path in sys.argv[1:]:
    ev = parse_log(path)
    print(f'==== {path}  (total probe events: {len(ev)})')
    kinds = {}
    for _, k, _ in ev:
        kinds[k] = kinds.get(k, 0) + 1
    print('event counts:', kinds)
    for kind, key, unit in [
        ('tx_reply_rate', 'rate', 'replies/s'),
        ('tx_send_block', 'us', 'us'),
        ('tx_empty_wait', 'us', 'us'),
        ('tx_full_block', 'us', 'us'),
        ('rx_reply_rate', 'rate', 'replies/s'),
        ('rx_reply_age', 'us', 'us'),
        ('dl_rate', 'rate', 'slots/s'),
        ('dl_anomaly', 'rx_wait', 'us'),
        ('dl_anomaly', 'process', 'us'),
        ('dl_anomaly', 'transmit', 'us'),
        ('ul_recv_wait', 'us', 'us'),
    ]:
        s = summarize(ev, kind, key)
        if s:
            print(f'  {kind}.{key}: n={s["n"]} min={s["min"]} p50={s["p50"]} mean={s["mean"]:.1f} '
                  f'p90={s["p90"]} p99={s["p99"]} max={s["max"]} ({unit})')
    # time window
    ts = [e[0] for e in ev]
    print(f'  time window: {ts[0]} .. {ts[-1]} ({ (ts[-1]-ts[0])/1e6:.1f} s)')
    print()
