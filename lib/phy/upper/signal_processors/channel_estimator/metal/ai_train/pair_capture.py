#!/usr/bin/env python3
"""Pair the G-5 capture triples (CE input / rx grid / CRC-OK TB).

The gNB dump pipeline is asynchronous: the TB dump is written when its LDPC
decode COMPLETES, which lands 0.2-3 slots after the grant's rx/CE dumps. When
two PUSCH grants land in adjacent slots, nearest-timestamp pairing can grab the
NEXT grant's grid instead of the one that carried the TB (the "529 us"
mispairing observed on retransmission pairs).

Newer captures carry a slot column in rx_meta.csv (col 16) and dd_meta.csv
(col 5); single-UE captures pair exactly on it (hashed, O(n)). Older captures
(and any slot without a match) fall back to up to K timestamp candidates
(nearest first); build_labels.py then picks the candidate whose re-encoded
label matches the input (content verification) instead of trusting the
timestamp.

Usage: pair_capture.py <capture_dir> [--out <pairs.npz>] [--k K] [--win US]
Output pairs.npz: tb_idx[N], rx_idx[N,K], ce_idx[N,K], n_prb[N,K]
                  (-1 sentinels pad the candidate axis)
"""
import os, sys, csv
import numpy as np

def load_meta(path):
    return list(csv.reader(open(path)))

def main():
    d = sys.argv[1]
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(d, 'pairs.npz')
    K   = int(sys.argv[sys.argv.index('--k') + 1]) if '--k' in sys.argv else 4
    WIN = int(sys.argv[sys.argv.index('--win') + 1]) if '--win' in sys.argv else 4000

    ce = [(int(r[0]), int(r[1]), int(r[5])) for r in load_meta(os.path.join(d, 'meta.csv'))]   # idx, prb, t_us
    rx = [tuple(int(x) for x in r) for r in load_meta(os.path.join(d, 'rx_meta.csv'))]          # idx, t_us, prb, ... [, slot]
    dd = [tuple(int(x) for x in r) for r in load_meta(os.path.join(d, 'dd_meta.csv'))]          # idx, t_us, tb_bits, ... [, slot]
    ce_t = np.array([x[2] for x in ce]); ce_p = np.array([x[1] for x in ce])
    rx_t = np.array([x[1] for x in rx]); rx_p = np.array([x[2] for x in rx])
    dd_t = np.array([x[1] for x in dd])
    slot_mode = all(len(r) >= 16 for r in rx) and all(len(r) >= 5 for r in dd)
    print(f'capture {d}: ce={len(ce)} rx={len(rx)} tb={len(dd)} slot_mode={slot_mode}')

    # Slot hash (the exact-pairing path) + sorted time index (the fallback path).
    slot_idx = {}
    if slot_mode:
        for j, r in enumerate(rx):
            slot_idx.setdefault(r[15], []).append(j)
    order_t = np.argsort(rx_t, kind='stable')

    # CE dumps sorted by time for the same-slot association.
    ce_order = np.argsort(ce_t, kind='stable')
    ce_t_s = ce_t[ce_order]

    def assoc_ce(j):
        """CE dump written in the SAME slot as rx grid j: the CE pre-stage runs just
        before the grid dump (same pipeline task), so the matching dump is
        microseconds away with an equal width. A half-slot window keeps the
        association from jumping to the adjacent slot. Returns (ce_idx, prb)."""
        pos = np.searchsorted(ce_t_s, rx_t[j])
        best = None
        for k in (pos - 1, pos, pos + 1):
            if 0 <= k < len(ce_t_s):
                l = ce_order[k]
                if abs(ce_t[l] - rx_t[j]) <= 500 and ce_p[l] == rx_p[j]:
                    return ce[l][0], rx_p[j]
        return best

    tb_rows, rx_rows, ce_rows, prb_rows = [], [], [], []
    exact = 0
    for di, t in enumerate(dd_t):
        cand = []
        if slot_mode:
            # Exact: the rx rows of the same slot (single-UE capture).
            for j in slot_idx.get(dd[di][4], ()):
                if abs(rx_t[j] - t) <= WIN:
                    a = assoc_ce(j)
                    if a is not None:
                        cand.append((rx[j][0], a[0], a[1]))
                        exact += 1
        # Timestamp candidates (backup / legacy captures), nearest first, via
        # the sorted time index: window [t-WIN, t+WIN) then sort by |dt|.
        if len(cand) < K:
            rx_t_s = rx_t[order_t]
            left  = int(np.searchsorted(rx_t_s, t - WIN, side='left'))
            right = int(np.searchsorted(rx_t_s, t + WIN, side='right'))
            near = sorted(order_t[left:right], key=lambda j: abs(rx_t[j] - t))
            for j in near:
                if abs(rx_t[j] - t) > WIN or len(cand) >= K:
                    break
                if any(c[0] == rx[j][0] for c in cand):
                    continue
                a = assoc_ce(j)
                if a is not None:
                    cand.append((rx[j][0], a[0], a[1]))
        for k in range(K):
            tb_rows.append(dd[di][0])
            if k < len(cand):
                rx_rows.append(cand[k][0]); ce_rows.append(cand[k][1]); prb_rows.append(cand[k][2])
            else:
                rx_rows.append(-1); ce_rows.append(-1); prb_rows.append(-1)
    tb_idx = np.array(tb_rows, dtype=np.int64).reshape(-1, K)
    rx_idx = np.array(rx_rows, dtype=np.int64).reshape(-1, K)
    ce_idx = np.array(ce_rows, dtype=np.int64).reshape(-1, K)
    n_prb  = np.array(prb_rows, dtype=np.int64).reshape(-1, K)
    n_cand = (rx_idx >= 0).sum(1)
    n0, n1, n2p = (n_cand == 0).sum(), (n_cand == 1).sum(), (n_cand >= 2).sum()
    print(f'tb rows {len(n_cand)}: 0 candidate={n0} 1={n1} >=2={n2p}')
    if slot_mode:
        print(f'slot-exact primary candidates: {exact}')
    np.savez(out, tb_idx=tb_idx, rx_idx=rx_idx, ce_idx=ce_idx, n_prb=n_prb)
    print(f'saved -> {out}')

if __name__ == '__main__':
    main()
