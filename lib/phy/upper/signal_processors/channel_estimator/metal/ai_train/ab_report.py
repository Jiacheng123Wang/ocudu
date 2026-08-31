#!/usr/bin/env python3
"""G-5 A/B report: per-leg CRC + SINR comparability from gnb logs.

Usage: ab_report.py <logA> <logB> [<logC> ...]
Prints per leg: tb_status counts, CRC-OK rate, and the SINR distribution of the
CRC indications (the comparability check: legs must have similar SINR medians
for the CRC comparison to be valid - see the real3 A/B: 12.5 vs 21.4 dB).
"""
import re, sys
import numpy as np

def leg(path):
    ok = ko = 0
    sinr = []
    with open(path, errors='replace') as f:
        for line in f:
            if 'CRC.indication' in line:
                if 'tb_status=OK' in line:
                    ok += 1
                elif 'tb_status=KO' in line:
                    ko += 1
                m = re.search(r'sinr=([-0-9.]+)dB', line)
                if m:
                    sinr.append(float(m.group(1)))
    sinr = np.array(sinr) if sinr else np.array([np.nan])
    rate = 100.0 * ok / (ok + ko) if ok + ko else float('nan')
    return ok, ko, rate, sinr

def main():
    paths = sys.argv[1:]
    if len(paths) < 2:
        print('usage: ab_report.py <logA> <logB> [...]')
        sys.exit(1)
    for p in paths:
        ok, ko, rate, s = leg(p)
        print(f'{p}: OK={ok} KO={ko} crc_ok={rate:.1f}% | '
              f'sinr min={np.min(s):.1f} p25={np.percentile(s,25):.1f} '
              f'p50={np.percentile(s,50):.1f} p75={np.percentile(s,75):.1f} '
              f'max={np.max(s):.1f} dB (n={len(s)})')
        # per-rnti breakdown (dual-UE runs: 0x4601=OAI UE, 0x4602=phone)
        rntis = {}
        with open(p, errors='replace') as f:
            for line in f:
                if 'CRC.indication' in line:
                    m = re.search(r'rnti=0x([0-9a-f]+)', line)
                    if m:
                        r = m.group(1)
                        d = rntis.setdefault(r, [0, 0, []])
                        if 'tb_status=OK' in line:
                            d[0] += 1
                        elif 'tb_status=KO' in line:
                            d[1] += 1
                        ms = re.search(r'sinr=([-0-9.]+)dB', line)
                        if ms:
                            d[2].append(float(ms.group(1)))
        if len(rntis) > 1:
            for r, (o, k, ss) in sorted(rntis.items()):
                ss = np.array(ss) if ss else np.array([np.nan])
                print(f'  rnti=0x{r}: OK={o} KO={k} crc_ok={100*o/(o+k) if o+k else float("nan"):.1f}% '
                      f'sinr_p50={np.percentile(ss,50):.1f} (n={o+k})')
    meds = [np.percentile(leg(p)[3], 50) for p in paths]
    print(f'comparable SINR medians: {"YES" if max(meds) - min(meds) < 3 else "NO (be careful with the CRC comparison)"}')

if __name__ == '__main__':
    main()
