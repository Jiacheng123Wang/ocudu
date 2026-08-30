#!/usr/bin/env python
"""Phase B: synthesize OCUDU-style PUSCH DMRS grids for HELENA fine-tuning.

Format matches the authors' trainData: [n, 612, 14, 2] complex grids (real/imag
interleaved) over 51 PRB x 14 symbols. The INPUT is the LS estimate at the DMRS
REs with zeros elsewhere (the sparse grid); the LABEL is the true channel grid.

Channels: 3GPP TDL-A..E (per-tap delays/powers from 38.901, normalized), random
Rayleigh taps, no Doppler (quasi-static per slot, matching the OCUDU synth test).

To be finalized against the authors' data statistics once the dataset is loaded
(their normalization vs ours).
"""
import numpy as np

PRB, NSYM, NFFT = 51, 14, 612          # 51 PRB x 12 SC, 14 symbols
DMRS_SC = [2, 7, 11]                    # E2E cell pattern {2,7,11}
PILOT_SC = np.arange(0, 12, 2)          # type-1: every other subcarrier

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

def gen_channels(n, rng, profiles=('A', 'B', 'C', 'D', 'E')):
    """True channel grid [n, 612, 14, 2], quasi-static per slot, unit-average power."""
    H = np.zeros((n, NFFT, NSYM, 2), dtype=np.float32)
    k = np.arange(NFFT)
    for i in range(n):
        prof = TDL[profiles[rng.integers(len(profiles))]]
        delays = np.array(prof['delays_ns']) * 1e-9
        plin = 10.0 ** (np.array(prof['powers_db']) / 10.0)
        plin /= plin.sum()
        taps = (rng.standard_normal(len(plin)) + 1j * rng.standard_normal(len(plin))) / np.sqrt(2)
        taps *= np.sqrt(plin)
        h = (taps[:, None] * np.exp(-2j * np.pi * 15e3 * delays[:, None] * k[None, :])).sum(0)
        H[i, :, :, 0] = np.repeat(h.real[:, None], NSYM, 1)
        H[i, :, :, 1] = np.repeat(h.imag[:, None], NSYM, 1)
    return H

def ls_sparse_grid(H, noise_std):
    """LS at the DMRS REs (pilot = 1+1j/sqrt(2) constant), zeros elsewhere."""
    X = np.zeros_like(H)
    for s in DMRS_SC:
        for prb in range(PRB):
            for pos in PILOT_SC:
                sc = prb * 12 + pos
                X[:, sc, s, 0] = (H[:, sc, s, 0] + noise_std * np.random.randn(H.shape[0])) / np.sqrt(2) \
                                 + (H[:, sc, s, 1] + noise_std * np.random.randn(H.shape[0])) / np.sqrt(2)
                X[:, sc, s, 1] = (H[:, sc, s, 1] + noise_std * np.random.randn(H.shape[0])) / np.sqrt(2) \
                                 - (H[:, sc, s, 0] + noise_std * np.random.randn(H.shape[0])) / np.sqrt(2)
    return X

if __name__ == '__main__':
    rng = np.random.default_rng(0)
    H = gen_channels(4, rng)
    X = ls_sparse_grid(H, 0.1)
    print('H', H.shape, 'X', X.shape, 'sparse ratio', np.mean(np.abs(X).reshape(4, -1) < 1e-9))
