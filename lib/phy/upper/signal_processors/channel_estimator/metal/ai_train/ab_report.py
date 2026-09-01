#!/usr/bin/env python3
"""G-5 A/B report: per-leg CRC + SINR comparability from gnb logs.

Usage: ab_report.py <logA> <logB> [<logC> ...]
Prints per leg: tb_status counts, CRC-OK rate, and the SINR distribution of the
CRC indications (the comparability check: legs must have similar SINR medians
for the CRC comparison to be valid - see the real3 A/B: 12.5 vs 21.4 dB).

Per-UE (rnti) breakdown: a UE that re-attaches after RLF gets a NEW C-RNTI, so
raw rnti rows fragment one UE into several. The breakdown merges rnti groups
by union-find: two rntis are the same UE when their time ranges do NOT overlap,
the gap between them is small (<= GAP_S), and their SINR medians match
(<= SINR_TOL dB) - concurrent rntis are always different UEs. Msg3-attempt
rntis (a handful of KO indications) are filtered out.
"""
import re, sys
from datetime import datetime
import numpy as np

GAP_S = 120.0      # max idle gap (seconds) between two rntis of the same UE
SINR_TOL = 4.0     # max SINR median difference (dB) for the same UE
OVERLAP_TOL = 30.0 # RLF re-attach handover: small time overlap between the old
                   # and new rnti is normal; genuine concurrency overlaps more
MIN_GRANTS = 20    # drop rntis below this many CRC indications (Msg3 noise)

def _ts(line):
    m = re.match(r'(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})\.(\d+)', line)
    if not m:
        return None
    return datetime.strptime(m.group(1), '%Y-%m-%dT%H:%M:%S').timestamp() + int(m.group(2)) / 1e6

def parse_rntis(path):
    """Return {rnti: [ok, ko, sinr_list, t_first, t_last]}."""
    rntis = {}
    with open(path, errors='replace') as f:
        for line in f:
            if 'CRC.indication' not in line:
                continue
            m = re.search(r'rnti=0x([0-9a-f]+)', line)
            if not m:
                continue
            r = m.group(1)
            d = rntis.setdefault(r, [0, 0, [], None, None])
            if 'tb_status=OK' in line:
                d[0] += 1
            elif 'tb_status=KO' in line:
                d[1] += 1
            ms = re.search(r'sinr=([-0-9.]+)dB', line)
            if ms:
                d[2].append(float(ms.group(1)))
            t = _ts(line)
            if t is not None:
                d[3] = t if d[3] is None else min(d[3], t)
                d[4] = t if d[4] is None else max(d[4], t)
    return rntis

def merge_groups(rntis):
    """Union-find over rntis: same UE = non-overlapping time ranges, small gap,
    matching SINR median. Returns {root: {rntis:[...], ok, ko, sinr}}."""
    items = [(r, d) for r, d in rntis.items() if d[0] + d[1] >= MIN_GRANTS]
    parent = {r: r for r, _ in items}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    rs = list(parent.keys())
    for i in range(len(rs)):
        for j in range(i + 1, len(rs)):
            r1, d1 = rs[i], rntis[rs[i]]
            r2, d2 = rs[j], rntis[rs[j]]
            if d1[3] is None or d2[3] is None:
                continue
            # Genuine concurrency (overlap > OVERLAP_TOL) = different UEs;
            # a small overlap is the RLF re-attach handover, allow merging.
            overlap = min(d1[4], d2[4]) - max(d1[3], d2[3])
            if overlap > OVERLAP_TOL:
                continue
            gap = max(d2[3] - d1[4], d1[3] - d2[4])
            if gap > GAP_S:
                continue
            s1 = np.median(d1[2]) if d1[2] else np.nan
            s2 = np.median(d2[2]) if d2[2] else np.nan
            if abs(s1 - s2) > SINR_TOL:
                continue
            union(r1, r2)
    groups = {}
    for r in rs:
        groups.setdefault(find(r), []).append(r)
    return groups

def main():
    paths = sys.argv[1:]
    if len(paths) < 2:
        print('usage: ab_report.py <logA> <logB> [...]')
        sys.exit(1)
    for p in paths:
        ok = ko = 0
        sinr = []
        with open(p, errors='replace') as f:
            for line in f:
                if 'CRC.indication' in line:
                    if 'tb_status=OK' in line:
                        ok += 1
                    elif 'tb_status=KO' in line:
                        ko += 1
                    m = re.search(r'sinr=([-0-9.]+)dB', line)
                    if m:
                        sinr.append(float(m.group(1)))
        s = np.array(sinr) if sinr else np.array([np.nan])
        print(f'{p}: OK={ok} KO={ko} crc_ok={100*ok/(ok+ko) if ok+ko else float("nan"):.1f}% | '
              f'sinr min={np.min(s):.1f} p25={np.percentile(s,25):.1f} '
              f'p50={np.percentile(s,50):.1f} p75={np.percentile(s,75):.1f} '
              f'max={np.max(s):.1f} dB (n={len(s)})')
        rntis = parse_rntis(p)
        groups = merge_groups(rntis)
        if len(groups) > 1:
            for root in sorted(groups, key=lambda r: min(rntis[x][3] or 0 for x in groups[r])):
                o = sum(rntis[x][0] for x in groups[root])
                k = sum(rntis[x][1] for x in groups[root])
                ss = [v for x in groups[root] for v in rntis[x][2]]
                ss = np.array(ss) if ss else np.array([np.nan])
                t0 = min(rntis[x][3] for x in groups[root] if rntis[x][3] is not None)
                t1 = max(rntis[x][4] for x in groups[root] if rntis[x][4] is not None)
                span = f'{t1-t0:.0f}s' if t0 is not None and t1 is not None else '?'
                rnti_s = ','.join(sorted(groups[root]))
                print(f'  UE[{rnti_s}]: OK={o} KO={k} crc_ok={100*o/(o+k) if o+k else float("nan"):.1f}% '
                      f'sinr_p50={np.percentile(ss,50):.1f} span={span} (n={o+k})')
    meds = []
    for p in paths:
        ss = []
        with open(p, errors='replace') as f:
            for line in f:
                if 'CRC.indication' in line:
                    m = re.search(r'sinr=([-0-9.]+)dB', line)
                    if m:
                        ss.append(float(m.group(1)))
        meds.append(np.percentile(ss, 50) if ss else np.nan)
    print(f'comparable SINR medians: {"YES" if max(meds) - min(meds) < 3 else "NO (be careful with the CRC comparison)"}')

if __name__ == '__main__':
    main()
