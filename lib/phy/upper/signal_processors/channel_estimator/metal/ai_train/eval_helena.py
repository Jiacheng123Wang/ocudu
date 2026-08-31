#!/usr/bin/env python
"""G2 eval: per-SNR NMSE on the authors' test split.

Rows: LS / linear-interpolation / practical-MMSE (authors' baselines) / HELENA.
"""
import os, time
# Self-contained: paths default to this script's directory; AI_CE_WORK can point
# them at a historical checkout (e.g. ~/ai_ce_work).
WORK = os.environ.get('AI_CE_WORK', os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import numpy as np, h5py, tf_keras

DATA = os.path.join(WORK, 'dataset', '290525_dataset_ce.mat')
MODEL = os.path.join(WORK, 'work', 'helena_g2_savedmodel')

def load_dataset(path):
    with h5py.File(path, 'r') as f:
        refs = f[list(f.keys())[1]]
        parts = []
        for i, ref in enumerate(refs):
            obj = f[ref[0]]
            if i < 5:
                parts.append(np.transpose(np.array(obj, dtype=obj.dtype), (3, 2, 1, 0)))
            else:
                other = np.transpose(np.array(obj, dtype=obj.dtype), (1, 0))
        return parts[0], parts[1], parts[2], parts[3], parts[4], other

t0 = time.time()
X, Y, Yprac, Ylin, Yls, other = load_dataset(DATA)
print(f'loaded in {time.time()-t0:.0f}s; snr shape {other.shape}')

n = X.shape[0]
idx = np.random.RandomState(42).permutation(n)
te = idx[int(n*0.85):]
snr = other[te, 3]

model = tf_keras.models.load_model(MODEL)
y = model.predict(X[te], batch_size=32, verbose=0)

def nmse(a, b):
    num = np.sum(np.abs(a - b)**2, axis=(1, 2, 3))
    den = np.sum(np.abs(b)**2, axis=(1, 2, 3))
    return 10 * np.log10(num / den)

for name, p in [('LS', Yls[te]), ('LinearInterp', Ylin[te]), ('PracticalMMSE', Yprac[te]), ('HELENA', y)]:
    nm = nmse(p, Y[te])
    print(f'{name:15s} overall NMSE {np.mean(nm):8.2f} dB')
    for lo, hi in [(-5, 0), (0, 5), (5, 10), (10, 15), (15, 25)]:
        m = (snr >= lo) & (snr < hi)
        if m.sum() > 0:
            print(f'   SNR [{lo:2d},{hi:2d}): {np.mean(nm[m]):8.2f} dB (n={m.sum()})')
