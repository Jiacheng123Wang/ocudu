"""EMA over a run's checkpoints: the exponential-moving-average alternative to a tail SWA.

WHY. The tail average (SWA) is a flat average of the last few checkpoints and gave +0.75 to +1.77 pp. An EMA weights
the recent checkpoints more and the older ones less, so it uses the whole trajectory instead of a window and does not
depend on picking the window. It must be built in ORDER, from the first checkpoint of the run (or of the segment).

Usage:
    python ema_weights.py out.keras decay ckpt_33.keras ckpt_34.keras ...
"""
import sys

import numpy as np
import tensorflow as tf

CODE = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train"
sys.path.insert(0, CODE)
import train_depth3 as T   # noqa: E402

CUSTOM = T.CUSTOM_OBJECTS  # ONE dict: a layer the trainer registers must load in every probe/aggregation tool


def main():
    out, decay = sys.argv[1], float(sys.argv[2])
    paths = sys.argv[3:]
    model = tf.keras.models.load_model(paths[0], compile=False, custom_objects=CUSTOM)
    ema = {w.path: np.asarray(w).astype(np.float64).copy() for w in model.weights}
    for p in paths[1:]:
        m = tf.keras.models.load_model(p, compile=False, custom_objects=CUSTOM)
        for w, src in zip(model.weights, m.weights):
            ema[w.path] = decay * ema[w.path] + (1.0 - decay) * np.asarray(src).astype(np.float64)
    for w in model.weights:
        w.assign(ema[w.path].astype(np.float32))
    model.save(out)
    print(f"ema(decay={decay}) over {len(paths)} checkpoints -> {out}")


if __name__ == "__main__":
    main()
