#!/usr/bin/env python3
"""G-5 end-to-end DD label builder.

Per tight pair (tb bits + rx grid + CE input dump, from pair_capture.py):
  TB -> CRC + segmentation -> LDPC encode (nr_ldpc) -> PUSCH rate match (rv)
     -> scramble -> modulate -> X_hat at the data REs -> H_dd = Y / X_hat
     -> FD+TD smoothing -> label grid [n_sc, 14, 2].

Output: labels.npz {X, Y, width} for the fine-tune (X = the CE input dump,
Y = the smoothed DD label, width in PRB).

Usage: build_labels.py <capture_dir> [--max N] [--out labels.npz]
"""
import os, sys, csv
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dd_label import segment, rate_match, bit_interleave, scramble, modulate, smooth_dense
import nr_ldpc


def build_one(tb_bits, rx_grid, n_prb, n_syms, n_ports, mod, dmrs_mask, n_id,
              n_scid, scr_id, rnti, n_layers, rv):
    n_sc = n_prb * 12
    # Data RE layout (measured on the captured grids): all 12 REs/PRB on the
    # non-DMRS symbols; the DMRS symbols carry ONLY the pilots (both combs are
    # DMRS/reserved with 2 CDM groups without data - the odd REs measure ~0).
    dmrs_syms = [s for s in range(14) if (dmrs_mask >> s) & 1]
    data_pos = []  # (sym, sc) in the transmission order (symbol-major, sc ascending)
    for s in range(14):
        if s in dmrs_syms:
            continue
        for sc in range(n_sc):
            data_pos.append((s, sc))
    n_data_re = len(data_pos)
    E_total = n_data_re * mod  # mod = the srsRAN enum = bits per symbol

    tbs = len(tb_bits)
    cbs = segment(tb_bits)
    n_cb = len(cbs)
    bg, Zc, n_cb2 = nr_ldpc.select_bg_and_zc(tbs, E_total)
    assert n_cb2 == n_cb, f'cb count mismatch {n_cb2} vs {n_cb}'
    # The CRC overheads are NOT in the subagent's selection: bump the lifting size
    # so K_b fits the actual (CRC-attached) code block lengths.
    max_cb = max(len(b) for b in cbs)
    zc_list = [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 26,
               28, 30, 32, 36, 40, 44, 48, 52, 56, 60, 64, 72, 80, 88, 96, 104, 112,
               120, 128, 144, 160, 176, 192, 208, 224, 240, 256, 288, 320, 352, 384]
    kb_info = 22 if bg == 1 else nr_ldpc._info_columns(bg, max_cb)
    for z in zc_list:
        if kb_info * z >= max_cb:
            Zc = z
            break
    K_b = kb_info * Zc
    # Encode + rate match + bit interleave per block; the fillers sit at the
    # end of the info part and are excluded from the transmitted systematic
    # bits.  The modulation-order bit interleaver (38.212 5.4.2.2) is applied
    # PER CODE BLOCK (on each E_r-bit rate-matched sequence), and only then are
    # the code blocks concatenated (38.212 5.5), before the scrambling.
    # E_r (38.212 5.4.2.1) is distributed over the code blocks at SYMBOL
    # granularity (so every E_r is a multiple of the modulation order): the first
    # (C - mod(G/Qm, C)) blocks take floor(G/Qm/C)*Qm bits, the rest ceil(...).
    E_sym = E_total // mod
    nof_short = n_cb - (E_sym % n_cb)
    coded = []
    for c in range(n_cb):
        if K_b < len(cbs[c]):
            raise ValueError(f'K_b {K_b} < cb {len(cbs[c])}')
        u = list(cbs[c]) + [0] * (K_b - len(cbs[c]))  # filler padding
        cw = nr_ldpc.ldpc_encode(bg, Zc, u)
        E_c = (E_sym // n_cb) * mod if c < nof_short else ((E_sym + n_cb - 1) // n_cb) * mod
        coded.extend(bit_interleave(rate_match(cw, len(cbs[c]), K_b, Zc, E_c, rv, bg), mod))
    assert len(coded) == E_total, f'E mismatch: {len(coded)} vs {E_total}'
    # Scramble (the PUSCH data c_init has no slot term).
    sc = scramble(coded, (rnti << 15) + n_id)
    syms = np.array(modulate(sc, mod), dtype=np.complex64)
    assert len(syms) == n_data_re
    # Map to the data REs; rx grid order: [port][sym][sc].
    rx = rx_grid.reshape(n_ports, n_syms, n_sc)[0]
    H = np.zeros((n_syms, n_sc), dtype=np.complex64)
    for i, (s, sc) in enumerate(data_pos):
        H[s, sc] = rx[s, sc] / syms[i]
    Hd = smooth_dense(H.T, n_sc, n_syms, [s in dmrs_syms for s in range(n_syms)], n_prb)
    return Hd  # [n_sc, n_syms]


def main():
    d = sys.argv[1]
    max_n = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 100000
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(d, 'labels.npz')
    pairs = np.load(os.path.join(d, 'pairs.npz'))
    tb_idx = pairs['tb_idx'][:max_n]; rx_idx = pairs['rx_idx'][:max_n]
    ce_idx = pairs['ce_idx'][:max_n]; n_prb = pairs['n_prb'][:max_n]
    dd = {}
    for r in csv.reader(open(os.path.join(d, 'dd_meta.csv'))):
        dd[int(r[0])] = int(r[2])
    rx = {}
    for r in csv.reader(open(os.path.join(d, 'rx_meta.csv'))):
        rx[int(r[0])] = tuple(int(x) for x in r)
    Xs, Ys, Ws = [], [], []
    done = skipped = 0
    for i in range(len(tb_idx)):
        t, r, c = tb_idx[i], rx_idx[i], ce_idx[i]
        (_, t_us, p, nsym, nports, k0, mod, dmask, nid, nscid, scrid, rnti, nlay, rv, nd) = rx[r]
        tb_file = os.path.join(d, f'tb_{t:08d}_tbs{dd[t]}.bits')
        if not os.path.exists(tb_file):
            skipped += 1
            continue
        tb_bits = [int(x) for x in np.unpackbits(np.fromfile(tb_file, dtype=np.uint8))]
        g = np.fromfile(os.path.join(d, f'rx_{r:08d}_prb{p}.f32'), dtype=np.float32)
        rx_grid = g[0::2] + 1j * g[1::2]
        try:
            Y = build_one(tb_bits, rx_grid, p, nsym, nports, mod, dmask, nid,
                          nscid, scrid, rnti, nlay, rv)
        except Exception:
            skipped += 1
            continue
        X = np.fromfile(os.path.join(d, f'dump_{c:08d}_prb{p}.f32'), dtype=np.float32)
        X = X.reshape(p * 12, 14, 2)
        # Align the label frame to the input frame: the raw rx grid carries the
        # residual CFO phase ramp (the classical input is CFO-compensated) and the
        # SCH-to-DMRS power ratio. Fit one complex gain per symbol and de-rotate.
        Xc = X[..., 0] + 1j * X[..., 1]
        for s in range(14):
            g = np.vdot(Y[:, s], Xc[:, s]) / max(np.vdot(Xc[:, s], Xc[:, s]).real, 1e-12)
            if abs(g) > 1e-6:
                Y[:, s] = Y[:, s] / g
        Yr = np.stack([Y.real, Y.imag], -1).astype(np.float32)
        # Zero-pad to the 52-PRB bucket width (the train_pad fixed-grid format).
        Xp = np.zeros((624, 14, 2), np.float32); Xp[:p * 12] = X
        Yp = np.zeros((624, 14, 2), np.float32); Yp[:p * 12] = Yr
        Xs.append(Xp); Ys.append(Yp); Ws.append(p)
        done += 1
    if done:
        Xa = np.stack(Xs); Ya = np.stack(Ys); Wa = np.array(Ws)
        np.savez(out, X=Xa, Y=Ya, width=Wa)
        err = np.abs(Xa - Ya)**2; sig = np.abs(Ya)**2
        print(f'done={done} skipped={skipped} saved -> {out}')
        print(f'label vs input: |X-Y|^2/|Y|^2 = {10*np.log10(err.sum()/sig.sum()):.1f} dB (expect strongly negative)')
    else:
        print(f'NOTHING BUILT (skipped {skipped})')


if __name__ == '__main__':
    main()
