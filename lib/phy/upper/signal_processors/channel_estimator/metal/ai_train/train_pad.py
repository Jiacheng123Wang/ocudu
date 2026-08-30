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

REPO_AI = os.path.join(os.path.expanduser('~'),
    'Library/CloudStorage/OneDrive-个人/newWork/work/ocudu',
    'lib/phy/upper/signal_processors/channel_estimator/metal/ai_train')
sys.path.insert(0, REPO_AI)

def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

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
    n = X.shape[0]
    idx = np.random.RandomState(42).permutation(n)
    nva = max(int(n * 0.1), 200)
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
