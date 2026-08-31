#!/usr/bin/env python3
"""Pad-aware fine-tune of HELENA at a bucket width (52 or 106 PRB).

Usage: train_pad.py <npz> <out_sm> --prb P
                    [--epochs N] [--batch N] [--lr R]
                    [--transfer <savedmodel> | --init <savedmodel>]
Loss: mse with an element-wise mask (active allocation region only), so the
zero-padded region neither trains nor counts. The model output in the padded
region is driven to zero by the residual connection (input there is 0).
"""
import os, sys, time
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import numpy as np
import tf_keras

# Self-contained: helena_arch.py lives next to this script (no external paths).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

def opt_float(name, default):
    return float(opt(name, default))

def main():
    npz_path, out_sm = sys.argv[1], sys.argv[2]
    prb    = int(opt('--prb', '106'))
    epochs = int(opt('--epochs', '8'))
    batch  = int(opt('--batch', '32'))
    lr     = float(opt('--lr', '1e-4'))
    nsc    = prb * 12

    d = np.load(npz_path)
    X, Y, W = d['X_train'], d['Y_train'], d['width_train']
    assert X.shape[1] == nsc, f'dataset grid {X.shape[1]} != model grid {nsc}'
    # Optional high-SNR focus pass: keep only the samples at/above the SNR floor so
    # the optimizer is forced to fit the (near-)identity mapping on clean inputs
    # (the high-SNR absolute errors are otherwise too small to influence the loss).
    snr_floor = opt_float('--filter-snr-min', -1e9)
    if '--filter-snr-min' in sys.argv and 'snr_train' in d:
        keep = d['snr_train'] >= snr_floor
        X, Y, W = X[keep], Y[keep], W[keep]
        print(f'snr filter >= {snr_floor}: kept {X.shape[0]} samples')
    n = X.shape[0]
    idx = np.random.RandomState(42).permutation(n)
    nva = min(max(int(n * 0.1), 200 if n >= 1000 else 1), n - 1)
    va, tr = idx[:nva], idx[nva:]
    print(f'{npz_path}: train {tr.size} val {va.size}, widths {W.min()}..{W.max()} PRB')

    if '--transfer' in sys.argv:
        from helena_arch import transfer_from_51
        from tf_keras.models import load_model
        src = load_model(opt('--transfer', ''))
        model = transfer_from_51(src, prb)
        print(f'init: transfer from {opt("--transfer", "")}')
    else:
        from tf_keras.models import load_model
        model = load_model(opt('--init', ''))
        print(f'init: {opt("--init", "")}')

    # Element-wise mask: active where subcarrier < allocation width. The trailing
    # dim is 1 (not 2): tf.keras squeezes sample_weight's trailing size-1 dims and
    # broadcasts against the (n, nsc, 14, 2) labels.
    mask = np.broadcast_to(
        (np.arange(nsc)[None, :, None, None] < W[:, None, None, None]).astype(np.float32),
        (n, nsc, 14, 1))

    # Optional SNR balancing (--snr-weight): weight ~ 1/error_power = 10^(snr/10),
    # normalized at 25 dB and clipped. Without it the high-SNR samples' tiny absolute
    # errors never influence the loss and the model underfits the clean regime
    # (the 45 dB corruption bug); a pure high-SNR focus pass instead causes
    # catastrophic forgetting of the low-SNR regime.
    if '--snr-weight' in sys.argv and 'snr_train' in d:
        snr_db = d['snr_train'][keep] if '--filter-snr-min' in sys.argv else d['snr_train']
        snr_db = snr_db[: n]
        wsnr = np.clip(np.power(10.0, (snr_db - 25.0) / 10.0), 0.02, 300.0)
        mask = mask * wsnr[:, None, None, None]
        print(f'snr weight: range [{wsnr.min():.2f}, {wsnr.max():.0f}]')

    model.compile(optimizer=tf_keras.optimizers.legacy.Adam(learning_rate=lr), loss='mse')
    t0 = time.time()
    model.fit(X[tr], Y[tr], sample_weight=mask[tr], batch_size=batch, epochs=epochs,
              validation_data=(X[va], Y[va], mask[va]), verbose=2)
    model.save(out_sm, save_format='tf')

    yp = model.predict(X[va], batch_size=batch, verbose=0)
    err, sig = np.abs(yp - Y[va]) ** 2, np.abs(Y[va]) ** 2
    act = np.broadcast_to(mask[va].astype(bool), Y[va].shape)
    nmse = 10 * np.log10(err[act].sum() / sig[act].sum())
    print(f'val pooled NMSE (active region): {nmse:.2f} dB')
    print(f'saved -> {out_sm} in {time.time()-t0:.0f}s')

if __name__ == '__main__':
    main()
