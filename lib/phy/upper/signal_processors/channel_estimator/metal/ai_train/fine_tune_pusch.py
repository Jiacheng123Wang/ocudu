#!/usr/bin/env python
"""Phase B fine-tune: HELENA on the OCUDU PUSCH synthetic set (.npz from
gen_pusch_dataset.py). Starts from the G2-A weights (or the original .keras).

Usage: fine_tune_pusch.py <dataset.npz> <init_savedmodel|.keras> <out_savedmodel>
       [--epochs N]
"""
import os, sys, time
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import numpy as np, tf_keras

def main():
    npz_path, init_path, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    epochs = int(sys.argv[sys.argv.index('--epochs') + 1]) if '--epochs' in sys.argv else 10
    d = np.load(npz_path)
    Xtr, Ytr, snr_tr = d['X_train'], d['Y_train'], d['snr_train']
    Xte, Yte, snr_te = d['X_test'], d['Y_test'], d['snr_test']
    print(f'data: train {Xtr.shape} test {Xte.shape}; snr [{snr_tr.min():.0f},{snr_tr.max():.0f}] dB')

    if init_path.endswith('.keras'):
        model = tf_keras.models.load_model(init_path)
    else:
        model = tf_keras.models.load_model(init_path)
    model.compile(optimizer=tf_keras.optimizers.Adam(learning_rate=3.2768e-4), loss='mse', metrics=['mae'])

    t0 = time.time()
    model.fit(Xtr, Ytr, batch_size=32, epochs=epochs,
              validation_data=(Xte, Yte), verbose=2)
    print(f'trained {epochs} epochs in {time.time()-t0:.0f}s')
    model.save(out_dir, save_format='tf')
    print('saved', out_dir)

if __name__ == '__main__':
    main()
