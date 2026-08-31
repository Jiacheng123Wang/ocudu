#!/usr/bin/env python3
"""Per-width-band pooled NMSE of a HELENA SavedModel on a pad-aware test set.

Usage: eval_pad.py <savedmodel> <npz> [--bands "6-12,13-25,26-52"] [--prb 52]
                 [--norm <rms>]   # scale each sample's active-region RMS to <rms>
                                  # (mirror the runtime's input normalization)
Bands are PRB ranges; NMSE is pooled (10log10(sum err / sum sig)) over the
ACTIVE allocation region only.
"""
import os, sys
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import numpy as np
from tf_keras.models import load_model

def main():
    sm_path, npz_path = sys.argv[1], sys.argv[2]
    def opt(name, default):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
    prb = int(opt('--prb', '106'))
    nsc = prb * 12
    bands = opt('--bands', '53-79,80-106' if prb == 106 else '6-12,13-25,26-52')
    bands = [(int(a), int(b)) for a, b in (b.split('-') for b in bands.split(','))]

    model = load_model(sm_path)
    d = np.load(npz_path)
    X, Y, W = d['X_test'], d['Y_test'], d['width_test']
    assert X.shape[1] == nsc, f'dataset grid {X.shape[1]} != model grid {nsc}'
    if '--norm' in sys.argv:
        tgt = float(opt('--norm', '0.15'))
        X = X.astype(np.float64); Y = Y.astype(np.float64)
        for i in range(len(W)):
            w = int(W[i]) * 12
            rms = float(np.sqrt((X[i, :w] ** 2).sum() / (w * 14 * 2)))
            if rms > 1e-9:
                f = tgt / rms
                X[i], Y[i] = X[i] * f, Y[i] * f
        X = X.astype(np.float32); Y = Y.astype(np.float32)
        print(f'--norm: samples scaled to rms {tgt} (runtime mirror)')
    yp = model.predict(X, batch_size=32, verbose=0)
    err, sig = np.abs(yp - Y) ** 2, np.abs(Y) ** 2
    act = np.broadcast_to((np.arange(nsc)[None, :, None, None] < W[:, None, None, None]).astype(bool), Y.shape)

    def pooled(m):
        return 10 * np.log10(err[m].sum() / sig[m].sum())
    print(f'overall pooled NMSE (active): {pooled(act):.2f} dB')
    for a, b in bands:
        m = act & (W[:, None, None, None] >= a) & (W[:, None, None, None] <= b)
        print(f'  PRB {a:3d}-{b:3d}: {pooled(m):.2f} dB (n={m.any(axis=(1,2,3)).sum()})')

if __name__ == '__main__':
    main()
