#!/usr/bin/env python
"""Synthesize OCUDU-style PUSCH grids for HELENA training (width-parameterized).

Format: INPUT = linear-interpolated LS grid [n, NFFT, 14, 2] (dense, allocation-
local subcarrier indexing), LABEL = true channel grid. NFFT here is the GRID
WIDTH in subcarriers (12 x PRB); e.g. 624 for 52 PRB, 1272 for 106 PRB.

--pad-aware --min-prb P --max-prb Q: every sample gets a random allocation
width in [P, Q] PRB; the allocation sits at subcarrier offset 0 of the full
grid and the remainder is zero-padded (the gNB bucket semantics). X is
interpolated from the allocation's OWN pilots only (matching the gNB classical
pre-stage, which only sees the grant). The width in PRB is stored per sample
(width_train/width_test) for loss masking / per-band evaluation.

Channels: 3GPP TDL-A..E (38.901 delays/powers, normalized), Rayleigh taps,
quasi-static per slot. DMRS: PUSCH type-1 (6 RE/PRB at subcarrier offsets
0,2,4,6,8,10) at symbols {2,7,11} (the E2E cell pattern). SNR -5..25 dB,
pilot power 1.

Output: .npz {X_train, Y_train, snr_train, width_train, R_train,
              X_test, Y_test, snr_test, width_test, R_test}.
"""
import os
import numpy as np
import sys

NSYM = 14
DMRS_SC = [2, 7, 11]
PILOT_SC = np.arange(0, 12, 2)          # type-1: 6 RE/PRB

TDL = {
    'A': dict(delays_ns=[0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120],
              powers_db=[-13.4, 0, -2.2, -4.0, -6.0, -8.2, -9.9, -10.5, -7.5, -7.2, -6.6, -12.5, -13.8]),
    'B': dict(delays_ns=[0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110],
              powers_db=[0, -2.2, -3.4, -8.2, -12.4, -13.1, -12.3, -10.7, -12.7, -10.4, -12.6, -14.3]),
    'C': dict(delays_ns=[0, 65, 70, 190, 195, 200, 240, 305, 410, 470, 530, 600, 640, 690],
              powers_db=[-4.4, -1.2, -3.5, -5.2, -2.5, 0, -2.2, -3.8, -8.0, -9.9, -10.1, -11.5, -11.1, -11.7]),
    'D': dict(delays_ns=[0, 20, 45, 120, 160, 195, 235, 305, 460, 540, 640, 720, 800, 900, 950],
              powers_db=[-1.4, -7.1, -1.6, -5.3, -2.8, -4.3, -5.8, -6.7, -8.1, -8.9, -8.8, -10.5, -8.6, -10.8, -10.5]),
    'E': dict(delays_ns=[0, 15, 30, 45, 60, 90, 105, 130, 150, 175, 200, 230, 250, 280, 300, 340, 375],
              powers_db=[-2.1, 0, -1.0, -2.5, -4.7, -2.5, -4.9, -5.1, -7.1, -8.4, -7.1, -6.8, -7.1, -6.9, -7.5, -8.0, -10.1]),
}
PROFILES = list(TDL.keys())

def gen_true_channels(n, nfft, rng):
    """True channel grids [n, nfft, 14, 2], quasi-static per slot, unit-average power."""
    H = np.zeros((n, nfft, NSYM, 2), dtype=np.float32)
    k = np.arange(nfft)
    profs = rng.integers(len(PROFILES), size=n)
    for pi, prof in enumerate(PROFILES):
        idx = np.where(profs == pi)[0]
        if idx.size == 0:
            continue
        d = np.array(TDL[prof]['delays_ns']) * 1e-9
        plin = 10.0 ** (np.array(TDL[prof]['powers_db']) / 10.0)
        plin /= plin.sum()
        taps = (rng.standard_normal((idx.size, len(plin))) + 1j * rng.standard_normal((idx.size, len(plin)))) / np.sqrt(2)
        taps *= np.sqrt(plin)
        h = (taps[:, :, None] * np.exp(-2j * np.pi * 15e3 * d[None, :, None] * k[None, None, :])).sum(1)
        H[idx, :, :, 0] = h.real[:, :, None]
        H[idx, :, :, 1] = h.imag[:, :, None]
    return H

def ls_and_interp(H, noise_std, rng, nfft, prb):
    """LS at the DMRS REs (pilot = 1), then FD + TD linear interpolation."""
    m = H.shape[0]
    ls = np.zeros((m, nfft, len(DMRS_SC), 2), dtype=np.float32)
    xs = np.concatenate([12 * prb + PILOT_SC for prb in range(prb)])
    xs_all = np.arange(nfft)
    for si, s in enumerate(DMRS_SC):
        h = H[:, :, s, 0] + 1j * H[:, :, s, 1]
        rx = h + (rng.standard_normal((m, nfft)) + 1j * rng.standard_normal((m, nfft))) * noise_std[:, None] / np.sqrt(2)
        ls_h = rx[:, xs]  # pilots only
        for i in range(m):
            ls[i, :, si, 0] = np.interp(xs_all, xs, ls_h[i].real)
            ls[i, :, si, 1] = np.interp(xs_all, xs, ls_h[i].imag)
    # TD linear interpolation across DMRS symbols, edge symbols = nearest DMRS.
    X = np.zeros_like(H)
    for sym in range(NSYM):
        los = [s for s in DMRS_SC if s <= sym]
        his = [s for s in DMRS_SC if s >= sym]
        lo = max(los) if los else min(DMRS_SC)
        hi = min(his) if his else max(DMRS_SC)
        if lo == hi:
            X[:, :, sym] = ls[:, :, DMRS_SC.index(lo)]
        else:
            w = (sym - lo) / (hi - lo)
            X[:, :, sym] = (1 - w) * ls[:, :, DMRS_SC.index(lo)] + w * ls[:, :, DMRS_SC.index(hi)]
    return X

def make(count, rng, grid_prb, pad_aware, min_prb, max_prb):
    nfft = grid_prb * 12
    H = np.zeros((count, nfft, NSYM, 2), dtype=np.float32)
    X = np.zeros_like(H)
    R = np.zeros((count, len(DMRS_SC), nfft, 2), dtype=np.float32)
    width = np.full(count, grid_prb, dtype=np.int32)
    snr_db = rng.uniform(-5, 25, count)
    ns = np.sqrt(10.0 ** (-snr_db / 10.0))
    if pad_aware:
        width = rng.integers(min_prb, max_prb + 1, count)
    for w in np.unique(width):
        idx = np.where(width == w)[0]
        wsc = int(w) * 12
        # Channel + interpolated-LS grid computed INSIDE the allocation only,
        # then scattered into the zero-padded full grid at offset 0.
        Hw = gen_true_channels(idx.size, wsc, rng)
        Xw = ls_and_interp(Hw, ns[idx], rng, wsc, int(w))
        for j, i in enumerate(idx):
            H[i, :wsc] = Hw[j]
            X[i, :wsc] = Xw[j]
            for si, s in enumerate(DMRS_SC):
                hc = Hw[j, :, s, 0] + 1j * Hw[j, :, s, 1]
                rxc = hc + (rng.standard_normal(wsc) + 1j * rng.standard_normal(wsc)) * ns[i] / np.sqrt(2)
                R[i, si, :wsc, 0] = rxc.real
                R[i, si, :wsc, 1] = rxc.imag
    return X, R, H, snr_db, width

def main(n_train, n_test, out, nfft_override=0, pad_aware=False, min_prb=0, max_prb=0):
    grid_prb = (nfft_override or 612) // 12
    rng = np.random.default_rng(0)
    Xtr, Rtr, Ytr, str_, wtr = make(n_train, rng, grid_prb, pad_aware, min_prb, max_prb)
    Xte, Rte, Yte, ste, wte = make(n_test, rng, grid_prb, pad_aware, min_prb, max_prb)
    np.savez_compressed(out, X_train=Xtr, Y_train=Ytr, snr_train=str_, width_train=wtr, R_train=Rtr,
                        X_test=Xte, Y_test=Yte, snr_test=ste, width_test=wte, R_test=Rte)
    wmsg = f'width range [{wtr.min()},{wtr.max()}] PRB; ' if wtr.size else ''
    xmsg = f'X range [{Xtr.min():.2f},{Xtr.max():.2f}] mean|X|={np.abs(Xtr).mean():.3f}' if Xtr.size else ''
    print(f'saved {out}: train {Xtr.shape} test {Xte.shape}; ' + wmsg + xmsg)

if __name__ == '__main__':
    n_train = int(sys.argv[1]) if len(sys.argv) > 1 else 40000
    n_test  = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    out     = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.expanduser('~/ai_ce_work'), 'work', 'pusch_ce_dataset.npz')
    nfft    = int(sys.argv[sys.argv.index('--nfft') + 1]) if '--nfft' in sys.argv else 0
    pad     = '--pad-aware' in sys.argv
    min_prb = int(sys.argv[sys.argv.index('--min-prb') + 1]) if '--min-prb' in sys.argv else 0
    max_prb = int(sys.argv[sys.argv.index('--max-prb') + 1]) if '--max-prb' in sys.argv else 0
    main(n_train, n_test, out, nfft, pad, min_prb, max_prb)
