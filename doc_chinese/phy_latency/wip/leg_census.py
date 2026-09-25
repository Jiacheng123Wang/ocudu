#!/usr/bin/env python3
"""Leg census: where the UL chain stopped, from a leg's ocudulog file.

This is the LATENCY phase's workbench tool (doc_chinese/phy_latency/wip/, see its README): it PRINTS, it
does not judge, and no criterion depends on it. It was written during the Q9 investigation (2026-09-25)
and produced the silence tables in the dev doc 6.6/6.8 - the tables that showed the UL data plane going
completely quiet for 5.002 s / 28.55 s while the DL and the scheduler kept running.

usage: python3 doc_chinese/phy_latency/wip/leg_census.py <leg>.log [--min-silence 0.3]

Prints: PUSCH-result silences > threshold (biggest first), the RF underflow/overflow and pool-EMPTY
events, the PUxCH-late count, and the first few of each event with timestamps. Read it TOGETHER with the
leg's stderr report ([metal_stats] input hold / block lifecycle, [ul_rx_pool] pop_blocking wait): this
script says WHERE the chain stopped in wall-clock terms, those lines say WHY.
"""
import collections, datetime, sys

def ts_of(line):
    if len(line) < 23 or line[0] != '2':
        return None
    try:
        return datetime.datetime.strptime(line[:23], '%Y-%m-%dT%H:%M:%S.%f')
    except ValueError:
        return None

def main():
    path = sys.argv[1]
    threshold = 0.3
    if '--min-silence' in sys.argv:
        threshold = float(sys.argv[sys.argv.index('--min-silence') + 1])
    prev, sil, n, first, last = None, [], 0, None, None
    events = []
    with open(path, errors='ignore') as fh:
        for line in fh:
            t = ts_of(line)
            if t is None:
                continue
            if 'PUSCH:' in line:
                n += 1
                first = first or t
                if prev is not None:
                    d = (t - prev).total_seconds()
                    if d > threshold:
                        sil.append((round(d, 3), line[:23]))
                prev, last = t, t
            for tag in ('receive pool is EMPTY', 'Real-time failure in RF: overflow',
                        'Real-time failure in RF: underflow', 'PUxCH request late'):
                if tag in line:
                    events.append((line[:23], tag))
    print(f'PUSCH results={n} over {(last - first).total_seconds():.1f}s'
          f'  ({(n / max(1e-9, (last - first).total_seconds())):.1f}/s)')
    print(f'silences > {threshold}s: {len(sil)}')
    for d, ts in sorted(sil, reverse=True)[:10]:
        print(f'   {d:9.3f}s ending {ts}')
    print('events:', dict(collections.Counter(t for _, t in events)))
    shown = collections.Counter()
    for ts, tag in events:
        if shown[tag] < 3:
            print(f'   {ts}  {tag}')
            shown[tag] += 1

if __name__ == '__main__':
    main()
