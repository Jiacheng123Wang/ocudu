#!/usr/bin/env python
"""G2: train HELENA on the authors' 5G-NR TDL dataset (tf-keras, M4 Pro CPU).

Pipeline: load HDF5 (trainData -> trainLabels) -> train/val/test split ->
load the pretrained HELENA .keras as init -> fine-tune (Adam, mse) ->
save SavedModel + .h5 for the coremltools conversion.
"""
import os, sys, time
# Self-contained: data/model paths default to this script's directory; AI_CE_WORK
# can point them at a historical checkout (e.g. ~/ai_ce_work).
WORK = os.environ.get('AI_CE_WORK', os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import numpy as np, h5py, tf_keras

DATA = os.path.join(WORK, 'dataset', '290525_dataset_ce.mat')
MODEL = os.path.join(WORK, 'repo', 'helena_repo', 'models', '010625_HELENA_CE_model.keras')
OUT_SM = os.path.join(WORK, 'work', 'helena_g2_savedmodel')
EPOCHS = int(os.environ.get('HELENA_EPOCHS', '5'))
BATCH = 32

def load_dataset(path):
    with h5py.File(path, 'r') as f:
        keys = list(f.keys())
        refs = f[keys[1]]
        parts = []
        for i, ref in enumerate(refs):
            obj = f[ref[0]]
            if i < 5:
                parts.append(np.transpose(np.array(obj, dtype=obj.dtype), (3, 2, 1, 0)))
            else:
                other = np.transpose(np.array(obj, dtype=obj.dtype), (1, 0))
        return parts[0], parts[1], other  # trainData, trainLabels, otherLabels(snr,profile,delay,doppler)

t0 = time.time()
trainData, trainLabels, other = load_dataset(DATA)
print(f'loaded {trainData.shape} -> {trainLabels.shape} in {time.time()-t0:.0f}s; '
      f'other={other.shape}; input range [{trainData.min():.3f},{trainData.max():.3f}] '
      f'label range [{trainLabels.min():.3f},{trainLabels.max():.3f}] snr range [{other[3].min():.1f},{other[3].max():.1f}]')
print(f'sparse ratio of input (|x|<1e-6): {np.mean(np.abs(trainData[:512].reshape(512,-1)) < 1e-6):.3f}')

n = trainData.shape[0]
idx = np.random.RandomState(42).permutation(n)
tr, va, te = idx[:int(n*0.7)], idx[int(n*0.7):int(n*0.85)], idx[int(n*0.85):]

model = tf_keras.models.load_model(MODEL)
model.compile(optimizer=tf_keras.optimizers.Adam(learning_rate=3.2768e-4), loss='mse', metrics=['mae'])
model.fit(trainData[tr], trainLabels[tr], batch_size=BATCH, epochs=EPOCHS,
          validation_data=(trainData[va], trainLabels[va]), verbose=2)
model.save(OUT_SM, save_format='tf')
print('saved', OUT_SM)
