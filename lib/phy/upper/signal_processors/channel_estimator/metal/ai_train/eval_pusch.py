#!/usr/bin/env python
"""Phase B eval: per-SNR NMSE on the OCUDU PUSCH synthetic test set.

Rows: input (linear-interp LS) / HELENA fine-tuned. The metal_mmse head-to-head
runs through the C++ harness on the same .npz (see the unit test).
Usage: eval_pusch.py <dataset.npz> <savedmodel> [--keras path]
"""
import os, sys
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import numpy as np, tf_keras

def nmse(a, b):
    num = np.sum(np.abs(a - b)**2, axis=(1, 2, 3))
    den = np.sum(np.abs(b)**2, axis=(1, 2, 3))
    return 10 * np.log10(num / den)

def main():
    npz_path, sm = sys.argv[1], sys.argv[2]
    d = np.load(npz_path)
    Xte, Yte, snr = d['X_test'], d['Y_test'], d['snr_test']
    model = tf_keras.models.load_model(sm)
    y = model.predict(Xte, batch_size=32, verbose=0)
    print(f'test n={Yte.shape[0]}, snr [{snr.min():.0f},{snr.max():.0f}] dB')
    for name, p in [('Input (lin-interp LS)', Xte), ('HELENA fine-tuned', y)]:
        nm = nmse(p, Yte)
        print(f'{name:22s} overall {np.mean(nm):8.2f} dB')
        for lo, hi in [(-5, 0), (0, 5), (5, 10), (10, 15), (15, 20), (20, 25)]:
            m = (snr >= lo) & (snr < hi)
            if m.sum():
                print(f'   SNR [{lo:3d},{hi:3d}): {np.mean(nm[m]):8.2f} dB (n={m.sum()})')

if __name__ == '__main__':
    main()
