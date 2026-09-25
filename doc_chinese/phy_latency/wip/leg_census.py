#!/usr/bin/env python3
"""Leg census: where the UL chain stopped, from a leg's ocudulog file.

This is the LATENCY phase's workbench tool (doc_chinese/phy_latency/wip/, see its README): it PRINTS, it
does not judge, and no criterion depends on it. It was written during the Q9 investigation (2026-09-25)
and produced the silence tables in the dev doc 6.6/6.8 - the tables that showed the UL data plane going
completely quiet for 5.002 s / 28.55 s while the DL and the scheduler kept running.

*** READ THIS BEFORE BELIEVING A SILENCE (dev doc 6.56). ***
A leg is not busy from end to end: `iperf3 -t 240` runs inside a ~280 s leg, so the log has an IDLE HEAD
(attach, ramp-up) and an IDLE TAIL (test over, then Ctrl-C).  A "silence" measured over the whole log is
therefore mostly NOT AN EVENT - measured on p31/p35: every one of their ten longest silences (1.1-6.5 s)
lay outside the traffic window, and the UE was transmitting PUCCH at ~100/s throughout them.  That is why
this tool now DERIVES THE TRAFFIC WINDOW from the log (a second is busy when it carries >= busy-floor
PUSCH results) and reports silences INSIDE it, with the PUCCH traffic during each one: PUCCH > 0 during a
silence means the UE was transmitting and the gNB was receiving, i.e. no traffic, not a dead radio.
Use --all to get the old whole-log behaviour (and the idle head/tail back).

usage: python3 doc_chinese/phy_latency/wip/leg_census.py <leg>.log [--min-silence 0.3] [--all]

Prints: the derived traffic window, PUSCH-result silences > threshold (biggest first) with the PUCCH
traffic inside each, the RF underflow/overflow and pool-EMPTY events, the PUxCH-late count, and the first
few of each event with timestamps. Read it TOGETHER with the leg's stderr report ([metal_stats] input
hold / block lifecycle, [ul_rx_pool] pop_blocking wait): this script says WHERE the chain stopped in
wall-clock terms, those lines say WHY.
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
    whole_log = '--all' in sys.argv
    # A second counts as busy when it carries this many PUSCH results. A busy leg reads ~600/s (one result
    # per slot at 2 slots/ms... 500/s) and an idle one 0, so the floor only has to separate the two.
    busy_floor = 50

    per_second = collections.Counter()   # second (as a string) -> PUSCH results
    second_time = {}                     # second -> its timestamp
    pucch_per_second = collections.Counter()
    prev, sil, n, first, last = None, [], 0, None, None
    events = []
    with open(path, errors='ignore') as fh:
        for line in fh:
            t = ts_of(line)
            if t is None:
                continue
            sec = line[:19]
            if 'PUSCH:' in line:
                n += 1
                first = first or t
                per_second[sec] += 1
                second_time[sec] = t
                if prev is not None:
                    d = (t - prev).total_seconds()
                    if d > threshold:
                        sil.append((round(d, 3), line[:23], prev, t))
                prev, last = t, t
            if 'PUCCH' in line:
                pucch_per_second[sec] += 1
            for tag in ('receive pool is EMPTY', 'Real-time failure in RF: overflow',
                        'Real-time failure in RF: underflow', 'PUxCH request late'):
                if tag in line:
                    events.append((line[:23], tag))

    busy = sorted(s for s, c in per_second.items() if c >= busy_floor)
    if whole_log or not busy:
        inside, outside = sil, []
        window = 'WHOLE LOG (--all, or no busy second found)'
    else:
        lo, hi = busy[0], busy[-1]
        span = [s for s in per_second if lo <= s <= hi]
        inside = [x for x in sil if lo <= x[1][:19] <= hi]
        outside = [x for x in sil if not (lo <= x[1][:19] <= hi)]
        window = (f'{lo} .. {hi}  ({len(busy)} busy second(s) of {len(span)} in the span, '
                  f'PUSCH >= {busy_floor}/s defines busy)')

    print(f'traffic window: {window}')
    print(f'PUSCH results={n} over {(last - first).total_seconds():.1f}s'
          f'  ({(n / max(1e-9, (last - first).total_seconds())):.1f}/s)')
    print(f'silences > {threshold}s INSIDE the traffic window: {len(inside)}')
    for d, ts, t0, t1 in sorted(inside, reverse=True)[:10]:
        # The PUCCH traffic inside the silence is what tells "no data" from "no radio": the UE keeps sending
        # its control channel while it has nothing to put on the PUSCH.
        pucch = sum(c for s, c in pucch_per_second.items() if t0.strftime('%Y-%m-%dT%H:%M:%S') <= s <= t1.strftime('%Y-%m-%dT%H:%M:%S'))
        rf = sum(1 for ts_e, tag in events
                 if tag.startswith('Real-time') and t0.strftime('%Y-%m-%dT%H:%M:%S') <= ts_e[:19] <= t1.strftime('%Y-%m-%dT%H:%M:%S'))
        verdict = 'the UE WAS transmitting (PUCCH) - no traffic, not a dead radio' if pucch else 'NO PUCCH either: the radio itself was quiet'
        print(f'   {d:9.3f}s ending {ts}  PUCCH lines inside={pucch}  RF failures inside={rf}  <-- {verdict}')
    if outside:
        worst = max(d for d, _, _, _ in outside)
        print(f'silences > {threshold}s OUTSIDE the window (idle head/tail, NOT events): {len(outside)}'
              f'  (longest {worst}s)')
    print('events:', dict(collections.Counter(t for _, t in events)))
    shown = collections.Counter()
    for ts, tag in events:
        if shown[tag] < 3:
            print(f'   {ts}  {tag}')
            shown[tag] += 1

if __name__ == '__main__':
    main()
