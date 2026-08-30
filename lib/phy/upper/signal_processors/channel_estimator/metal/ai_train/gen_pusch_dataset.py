#!/usr/bin/env python
"""Phase B: synthesize OCUDU-style PUSCH grids for HELENA fine-tuning.

Format matches the authors' trainData EXACTLY as discovered (see
AI_CE_training_memo.md): the INPUT is the LINEAR-INTERPOLATED LS grid
[n, 612, 14, 2] (dense), the LABEL is the true channel grid.

Channels: 3GPP TDL-A..E (38.901 delays/powers, normalized), Rayleigh taps,
quasi-static per slot. DMRS: PUSCH type-1 (6 RE/PRB at subcarrier offsets
0,2,4,6,8,10) at symbols {2,7,11} (the E2E cell pattern). SNR -5..25 dB,
pilot power 1.

Output: .npz {X_train, Y_train, snr_train, X_test, Y_test, snr_test}.
"""
import os
import numpy as np
import sys

PRB, NSYM, NFFT = 51, 14, 612
DMRS_SC = [2, 7, 11]
# The 52-PRB (624-subcarrier) E2E cell variant is selected via NFFT/PRB override:
#   gen_pusch_dataset.py 40000 4000 out.npz --nfft 624
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

def gen_true_channels(n, rng):
    """True channel grids [n, 612, 14, 2], quasi-static per slot, unit-average power."""
    H = np.zeros((n, NFFT, NSYM, 2), dtype=np.float32)
    k = np.arange(NFFT)
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

def ls_and_interp(H, noise_std, rng):
    """LS at the DMRS REs (pilot = 1), then FD + TD linear interpolation."""
    n = H.shape[0]
    ls = np.zeros((n, NFFT, len(DMRS_SC), 2), dtype=np.float32)
    xs = np.concatenate([12 * prb + PILOT_SC for prb in range(PRB)])
    xs_all = np.arange(NFFT)
    for si, s in enumerate(DMRS_SC):
        h = H[:, :, s, 0] + 1j * H[:, :, s, 1]
        rx = h + (rng.standard_normal((n, NFFT)) + 1j * rng.standard_normal((n, NFFT))) * noise_std / np.sqrt(2)
        ls_h = rx[:, xs]  # pilots only
        for i in range(n):
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

def main(n_train, n_test, out, nfft_override=0):
    global NFFT, PRB
    if nfft_override:
        NFFT = nfft_override
        PRB = NFFT // 12
    rng = np.random.default_rng(0)
    def make(count, rng):
        H = gen_true_channels(count, rng)
        snr_db = rng.uniform(-5, 25, count)
        ns = np.sqrt(10.0 ** (-snr_db / 10.0))
        # X: interpolated-LS input grid; R: raw received grids at the DMRS symbols
        # (the C++ metal_mmse head-to-head harness consumes R + unit pilots).
        X = np.zeros((count, NFFT, NSYM, 2), dtype=np.float32)
        R = np.zeros((count, len(DMRS_SC), NFFT, 2), dtype=np.float32)
        for i in range(count):
            h = H[i]
            X[i] = ls_and_interp(h[None], ns[i], rng)[0]
            for si, s in enumerate(DMRS_SC):
                hc = h[:, s, 0] + 1j * h[:, s, 1]
                rxc = hc + (rng.standard_normal(NFFT) + 1j * rng.standard_normal(NFFT)) * ns[i] / np.sqrt(2)
                R[i, si, :, 0] = rxc.real
                R[i, si, :, 1] = rxc.imag
        return X, R, H, snr_db
    Xtr, Rtr, Ytr, str_ = make(n_train, rng)
    Xte, Rte, Yte, ste = make(n_test, rng)
    np.savez_compressed(out, X_train=Xtr, Y_train=Ytr, snr_train=str_, R_train=Rtr,
                        X_test=Xte, Y_test=Yte, snr_test=ste, R_test=Rte)
    print(f'saved {out}: train {Xtr.shape} test {Xte.shape}; '
          f'X range [{Xtr.min():.2f},{Xtr.max():.2f}] mean|X|={np.abs(Xtr).mean():.3f}')

if __name__ == '__main__':
    n_train = int(sys.argv[1]) if len(sys.argv) > 1 else 40000
    n_test  = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    out     = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.expanduser('~/ai_ce_work'), 'work', 'pusch_ce_dataset.npz')
    nfft    = 0
    if '--nfft' in sys.argv:
        nfft = int(sys.argv[sys.argv.index('--nfft') + 1])
    main(n_train, n_test, out, nfft)
