"""Compute several EMA variants in ONE pass over the checkpoints.

WHY ONE PASS. Each checkpoint load costs a second or two and 187k parameters; loading the same 40 checkpoints once per
beta is 6x the work for no reason, and on a machine that is also collecting corpus that matters.
"""
import glob
import sys

import numpy as np
import tensorflow as tf

CODE = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train"
sys.path.insert(0, CODE)
import train_depth3 as T   # noqa: E402

CUSTOM = {"FoldedScaleConv": T.FoldedScaleConv, "MaskedSmooth": T.MaskedSmooth}
betas = [float(b) for b in sys.argv[1].split(",")]
outdir = sys.argv[2]
paths = sorted(glob.glob(sys.argv[3]))

ref = tf.keras.models.load_model(paths[0], compile=False, custom_objects=CUSTOM)
names = [w.path for w in ref.weights]
ema = {b: [np.asarray(w).astype(np.float64).copy() for w in ref.weights] for b in betas}
for p in paths[1:]:
    m = tf.keras.models.load_model(p, compile=False, custom_objects=CUSTOM)
    for b in betas:
        acc = ema[b]
        for i, src in enumerate(m.weights):
            acc[i] *= b
            acc[i] += (1.0 - b) * np.asarray(src).astype(np.float64)
for b in betas:
    m = tf.keras.models.load_model(paths[0], compile=False, custom_objects=CUSTOM)
    for w, v in zip(m.weights, ema[b]):
        w.assign(v.astype(np.float32))
    out = f"{outdir}/ema_b{b:g}.keras"
    m.save(out)
    print("wrote", out)
