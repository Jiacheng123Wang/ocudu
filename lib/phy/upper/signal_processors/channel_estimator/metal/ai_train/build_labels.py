#!/usr/bin/env python3
"""G-5 end-to-end DD label builder with candidate content verification.

For each TB (dd_meta row), pair_capture.py emits up to K candidate rx-grid/CE
triples (nearest timestamps first). Timestamps alone are ambiguous when two
grants land in adjacent slots (the decode finishes ~0.2-3 slots after its
grant, so a TB can sit closer in time to the NEXT grant's dumps). This builder
re-encodes the TB against EVERY candidate and keeps the one whose label best
matches the CE input (the label-vs-input power ratio is ~-20 dB for the true
grant and ~0 dB for a foreign grant). The per-pair winner is then quality
gated (default -10 dB).

Per chosen candidate:
  TB -> CRC + segmentation -> LDPC encode (nr_ldpc) -> PUSCH rate match (rv)
     -> scramble -> modulate -> X_hat at the data REs -> H_dd = Y / X_hat
     -> FD+TD smoothing -> per-symbol CFO alignment against the CE input
     -> zero-pad to the --bucket grid width.

Output: labels.npz {X, Y, width, snr_train, rank} (grids padded to
--bucket*12 subcarriers; snr_train from meta.csv of the chosen CE dump).

Usage: build_labels.py <capture_dir> [--max N] [--out labels.npz]
                        [--bucket 52|106] [--gate DB] [--keep-raw]
"""
import os, sys, csv
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dd_label import segment, rate_match, bit_interleave, scramble, modulate, smooth_dense
import nr_ldpc


def build_label(tb_bits, rx_grid, n_prb, n_syms, n_ports, mod, dmrs_mask, n_id,
                n_scid, scr_id, rnti, n_layers, rv):
    """Re-encode the TB against this grant and return H [n_sc, n_syms] complex."""
    n_sc = n_prb * 12
    # Data RE layout (measured on the captured grids): all 12 REs/PRB on the
    # non-DMRS symbols; the DMRS symbols carry ONLY the pilots (both combs are
    # DMRS/reserved with 2 CDM groups without data - the odd REs measure ~0).
    dmrs_syms = [s for s in range(14) if (dmrs_mask >> s) & 1]
    data_pos = []  # (sym, sc) in the transmission order (symbol-major, sc ascending)
    for s in range(n_syms):
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
    # The CRC overheads are NOT in select_bg_and_zc's model: bump the lifting size
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
    # Map to the data REs; rx grid file order: [sym][port][sc] (single-port
    # captures: identical to symbol-major [sym][sc]).
    rx = rx_grid.reshape(n_syms, n_ports, n_sc)[:, 0]
    H = np.zeros((n_syms, n_sc), dtype=np.complex64)
    for i, (s, sc) in enumerate(data_pos):
        H[s, sc] = rx[s, sc] / syms[i]
    Hd = smooth_dense(H.T, n_sc, n_syms, [s in dmrs_syms for s in range(n_syms)], n_prb)
    return Hd  # [n_sc, n_syms]


def metric_db(Xc, Y):
    """Label-vs-input power ratio on the active region: strongly negative = good."""
    err = np.abs(Xc - Y) ** 2
    sig = np.abs(Y) ** 2
    return 10 * np.log10(err.sum() / max(sig.sum(), 1e-30))


def main():
    d = sys.argv[1]
    max_n = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 1000000
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(d, 'labels.npz')
    bucket = int(sys.argv[sys.argv.index('--bucket') + 1]) if '--bucket' in sys.argv else 52
    gate = float(sys.argv[sys.argv.index('--gate') + 1]) if '--gate' in sys.argv else -10.0
    nsc_pad = bucket * 12

    pairs = np.load(os.path.join(d, 'pairs.npz'))
    def to2d(a):
        a = np.asarray(a)
        return a.reshape(-1, 1) if a.ndim == 1 else a
    tb_idx = to2d(pairs['tb_idx'])[:max_n]   # (N,K) — backward-compat with 1-D files
    rx_idx = to2d(pairs['rx_idx'])[:max_n]
    ce_idx = to2d(pairs['ce_idx'])[:max_n]
    n_prb = to2d(pairs['n_prb'])[:max_n]
    N, K = tb_idx.shape

    dd = {}
    for r in csv.reader(open(os.path.join(d, 'dd_meta.csv'))):
        dd[int(r[0])] = int(r[2])
    rx = {}
    for r in csv.reader(open(os.path.join(d, 'rx_meta.csv'))):
        rx[int(r[0])] = tuple(int(x) for x in r)
    ce_snr = {}
    for r in csv.reader(open(os.path.join(d, 'meta.csv'))):
        if len(r) >= 6:
            ce_snr[int(r[0])] = float(r[2])

    Xs, Ys, Ws, Ss, Rs = [], [], [], [], []
    stats = {'none': 0, 'no_candidate_pass': 0, 'build_fail': 0, 'won_rank': {}, 'gate': 0}
    for i in range(N):
        t = tb_idx[i, 0]
        tb_file = os.path.join(d, f'tb_{t:08d}_tbs{dd[t]}.bits')
        if not os.path.exists(tb_file):
            stats['none'] += 1
            continue
        tb_bits = [int(x) for x in np.unpackbits(np.fromfile(tb_file, dtype=np.uint8))]
        best = None  # (metric, X_padded, Y_padded, width, snr, rank)
        for k in range(K):
            r, c, p = rx_idx[i, k], ce_idx[i, k], n_prb[i, k]
            if r < 0 or c < 0:
                break
            try:
                row = rx[r]
                row = row + (0, 1)[len(row) - 13:]  # tolerate 13-col metas (no rv/nd): rv=0, nd=1
                (_, _, _, nsym, nports, k0, mod, dmask, nid, nscid, scrid, rnti, nlay, rv, nd) = row[:15]
                g = np.fromfile(os.path.join(d, f'rx_{r:08d}_prb{p}.f32'), dtype=np.float32)
                if g.size < p * 12 * nsym * nports * 2:
                    continue
                rx_grid = g[0::2] + 1j * g[1::2]
                Y = build_label(tb_bits, rx_grid, p, nsym, nports, mod, dmask, nid,
                                nscid, scrid, rnti, nlay, rv)
                Xf = os.path.join(d, f'dump_{c:08d}_prb{p}.f32')
                X = np.fromfile(Xf, dtype=np.float32).reshape(p * 12, 14, 2)
                Xc = X[..., 0] + 1j * X[..., 1]
                # Align the label frame to the input frame: the raw rx grid carries
                # the residual CFO phase ramp (the classical input is CFO-compensated)
                # and the SCH-to-DMRS power ratio. Fit one complex gain per symbol
                # and de-rotate the label.
                for s in range(14):
                    gg = np.vdot(Y[:, s], Xc[:, s]) / max(np.vdot(Xc[:, s], Xc[:, s]).real, 1e-12)
                    if abs(gg) > 1e-6:
                        Y[:, s] = Y[:, s] / gg
                m = metric_db(Xc, Y)
                if best is None or m < best[0]:
                    Xp = np.zeros((nsc_pad, 14, 2), np.float32); Xp[:p * 12] = X
                    Yp = np.zeros((nsc_pad, 14, 2), np.float32); Yp[:p * 12] = np.stack([Y.real, Y.imag], -1)
                    best = (m, Xp, Yp, p, ce_snr.get(c, np.nan), k)
            except Exception:
                stats['build_fail'] += 1
                continue
        if best is None:
            stats['no_candidate_pass'] += 1
            continue
        if best[0] > gate:
            stats['gate'] += 1
            continue
        Xs.append(best[1]); Ys.append(best[2]); Ws.append(best[3]); Ss.append(best[4]); Rs.append(best[5])
        stats['won_rank'][best[5]] = stats['won_rank'].get(best[5], 0) + 1
    done = len(Xs)
    print(f'pairs={N} done={done} skipped: none={stats["none"]} '
          f'no_candidate_pass={stats["no_candidate_pass"]} build_fail={stats["build_fail"]} '
          f'gate({gate:.0f}dB)={stats["gate"]}')
    print(f'chosen candidate rank histogram: {dict(sorted(stats["won_rank"].items()))}')
    if done:
        Xa = np.stack(Xs); Ya = np.stack(Ys); Wa = np.array(Ws)
        np.savez(out, X=Xa, Y=Ya, width=Wa, snr_train=np.array(Ss), rank=np.array(Rs))
        err = np.abs(Xa - Ya) ** 2
        act = np.broadcast_to((np.arange(nsc_pad)[None, :, None, None] < Wa[:, None, None, None] * 12), Xa.shape)
        print(f'saved -> {out}')
        print(f'label vs input (active region): {10*np.log10(err[act].sum()/max((np.abs(Ya)[act]**2).sum(),1e-30)):.1f} dB '
              f'(expect strongly negative)')
    else:
        print('NOTHING BUILT')

if __name__ == '__main__':
    main()
