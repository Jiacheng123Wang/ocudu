#!/usr/bin/env python3
"""Split build_labels.py output into a train/val npz for train_pad.py.

Usage: split_labels.py <labels_all.npz> <realtrain.npz>
                       [--gate DB] [--snr-min DB] [--snr-max DB] [--val-frac R] [--seed N]

The --gate re-applies the label-vs-input quality gate POST-HOC (the metric is
recomputed from X/Y/width, so no rebuild is needed to try a stricter gate than
build_labels.py's -10 dB default). The optional --snr filters select the
regime (labels carry snr_train from the capture meta).
"""
import sys
import numpy as np

def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def main():
    src, dst = sys.argv[1], sys.argv[2]
    gate = float(opt('--gate', '-1e9'))
    snr_min = float(opt('--snr-min', '-1e9'))
    snr_max = float(opt('--snr-max', '1e9'))
    val_frac = float(opt('--val-frac', '0.1'))
    seed = int(opt('--seed', '42'))

    d = np.load(src)
    X, Y, W = d['X'], d['Y'], d['width']
    S = d['snr_train'] if 'snr_train' in d else np.full(len(W), np.nan)
    n = len(W)
    m = np.zeros(n)
    for i in range(n):
        w = int(W[i]) * 12
        m[i] = 10 * np.log10((np.abs(X[i, :w] - Y[i, :w]) ** 2).sum() /
                             max((np.abs(Y[i, :w]) ** 2).sum(), 1e-30))
    keep = (m <= gate) & (S >= snr_min) & (S <= snr_max)
    print(f'{src}: n={n} gate<= {gate}dB -> {keep.sum()} ({100*keep.mean():.1f}%)'
          + (f' snr [{snr_min},{snr_max}]' if snr_min > -1e8 or snr_max < 1e8 else ''))
    idx = np.where(keep)[0]
    rng = np.random.default_rng(seed)
    perm = rng.permutation(len(idx))
    nva = max(int(len(idx) * val_frac), 1)
    te, tr = idx[perm[:nva]], idx[perm[nva:]]
    np.savez(dst, X_train=X[tr], Y_train=Y[tr], width_train=W[tr],
             snr_train=S[tr] if 'snr_train' in d else np.array([]),
             X_test=X[te], Y_test=Y[te], width_test=W[te],
             snr_test=S[te] if 'snr_train' in d else np.array([]))
    print(f'train={len(tr)} test={len(te)} saved -> {dst}')

if __name__ == '__main__':
    main()
