#!/usr/bin/env python3
"""G-5 capture QA: one-glance health report of a capture directory.

Usage: capture_qa.py <capture_dir>
Checks: file counts, prb/mod/snr/engine distributions, CE input digital scale
(|X| RMS - the model's trained regime is ~0.1-0.25), CRC-OK rate per width band.
"""
import os, sys, csv
import numpy as np

def main():
    d = sys.argv[1]
    meta = list(csv.reader(open(os.path.join(d, 'meta.csv'))))
    rx   = list(csv.reader(open(os.path.join(d, 'rx_meta.csv'))))
    dd   = list(csv.reader(open(os.path.join(d, 'dd_meta.csv'))))
    print(f'{d}: ce={len(meta)} rx={len(rx)} tb={len(dd)} '
          f'rx_cols={len(rx[0]) if rx else 0} dd_cols={len(dd[0]) if dd else 0}')
    prb  = np.array([int(r[2]) for r in rx])
    mod  = np.array([int(r[6]) for r in rx])
    snr  = np.array([float(r[2]) for r in meta])
    eng  = np.array([int(r[4]) for r in meta])
    print('prb dist:', {int(k): int(v) for k, v in zip(*np.unique(prb, return_counts=True))})
    print('mod dist:', {int(k): int(v) for k, v in zip(*np.unique(mod, return_counts=True))})
    print('engine_nsc dist:', {int(k): int(v) for k, v in zip(*np.unique(eng, return_counts=True))})
    m = np.isfinite(snr)
    if m.any():
        print(f'snr: p5={np.percentile(snr[m],5):.1f} p50={np.percentile(snr[m],50):.1f} '
              f'p95={np.percentile(snr[m],95):.1f} dB (n={m.sum()})')
    # CE input digital scale on a few NN-active dumps (the model regime: |X|~0.1-0.25).
    rms = []
    for r in meta[:200]:
        if int(r[4]) == 0:
            continue
        p = os.path.join(d, f'dump_{int(r[0]):08d}_prb{r[1]}.f32')
        if not os.path.exists(p):
            continue
        x = np.fromfile(p, dtype=np.float32)
        rms.append(float(np.sqrt((x ** 2).mean())))
        if len(rms) >= 40:
            break
    if rms:
        rms = np.array(rms)
        print(f'CE input |X| rms: p50={np.median(rms):.4f} p5={np.percentile(rms,5):.4f} '
              f'p95={np.percentile(rms,95):.4f} (model trained ~0.1-0.25)')
    # CRC-OK rate per width band (the label yield proxy). slot matching must be
    # time-windowed: system_slot() wraps every hyperframe (~5.12 s at 30 kHz).
    # Hash on (slot, coarse time bucket) to keep large captures linear.
    tb_keys = {}
    for r in dd:
        tb_keys.setdefault((int(r[4]), int(r[1]) // 10000), True)
    bands = [(1, 5), (6, 12), (13, 25), (26, 52), (53, 106)]
    for lo, hi in bands:
        sel = [r for r in rx if lo <= int(r[2]) <= hi]
        ok = 0
        for r in sel:
            b = int(r[1]) // 10000
            if (int(r[15]), b) in tb_keys or (int(r[15]), b - 1) in tb_keys or (int(r[15]), b + 1) in tb_keys:
                ok += 1
        if sel:
            print(f'prb {lo:3d}-{hi:3d}: grants={len(sel)} crc_ok_grants={ok}')

if __name__ == '__main__':
    main()
