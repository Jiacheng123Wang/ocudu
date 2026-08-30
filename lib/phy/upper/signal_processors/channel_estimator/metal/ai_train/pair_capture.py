#!/usr/bin/env python3
"""Pair the G-5 capture triples (CE input / rx grid / CRC-OK TB) by timestamp.

Usage: pair_capture.py <capture_dir> [--out <pairs.npz>]
Produces pairs.npz with the paired arrays + a report of the pairing quality.
The re-encode stage (CRC24A -> LDPC -> rate match -> scramble -> modulate ->
H = Y / X_hat -> smooth) consumes this output.
"""
import os, sys, csv
import numpy as np

def load_meta(path):
    return list(csv.reader(open(path)))

def main():
    d = sys.argv[1]
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(d, 'pairs.npz')
    ce = [(int(r[0]), int(r[1]), int(r[5])) for r in load_meta(os.path.join(d, 'meta.csv'))]        # idx, prb, t_us
    rx = [(int(r[0]), int(r[1]), int(r[2]), tuple(int(x) for x in r[3:]) if len(r) > 3 else ()) for r in load_meta(os.path.join(d, 'rx_meta.csv'))]
    dd = [(int(r[0]), int(r[1]), int(r[2])) for r in load_meta(os.path.join(d, 'dd_meta.csv'))]      # idx, t_us, tb_bits
    ce_t = np.array([x[2] for x in ce]); ce_p = np.array([x[1] for x in ce]); ce_i = np.array([x[0] for x in ce])
    rx_t = np.array([x[1] for x in rx]); rx_p = np.array([x[2] for x in rx]); rx_i = np.array([x[0] for x in rx])
    dd_t = np.array([x[1] for x in dd]); dd_i = np.array([x[0] for x in dd])

    print(f'capture {d}: ce={len(ce)} rx={len(rx)} tb={len(dd)}')
    rows = []
    for k, t in enumerate(dd_t):
        j = int(np.argmin(np.abs(rx_t - t)))
        l = int(np.argmin(np.abs(ce_t - t)))
        dt_rx = abs(rx_t[j] - t); dt_ce = abs(ce_t[l] - t)
        # Keep only tightly-paired, width-consistent triples.
        if dt_rx <= 5000 and dt_ce <= 5000 and rx_p[j] == ce_p[l]:
            rows.append((dd_i[k], rx_i[j], ce_i[l], rx_p[j], dt_rx, dt_ce))
    print(f'tight pairs (|dt|<=5ms, width match): {len(rows)} / {len(dd)}')
    if rows:
        a = np.array(rows)
        print(f'  dt_rx us: p50={np.median(a[:,4]):.0f} p95={np.percentile(a[:,4],95):.0f} max={a[:,4].max():.0f}')
        print(f'  dt_ce us: p50={np.median(a[:,5]):.0f} p95={np.percentile(a[:,5],95):.0f} max={a[:,5].max():.0f}')
        np.savez(out, tb_idx=a[:,0], rx_idx=a[:,1], ce_idx=a[:,2], n_prb=a[:,3])
        print(f'saved -> {out}')
    else:
        print('NO PAIRS')

if __name__ == '__main__':
    main()
