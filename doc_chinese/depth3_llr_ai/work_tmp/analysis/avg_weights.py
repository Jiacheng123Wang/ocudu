"""Average the weights of several checkpoints -- but only after proving the layouts match.

WHY A COMPATIBILITY CHECK. Weight averaging is only meaningful between graphs that have the SAME layers with the SAME
shapes. The arms differ: L6 feeds the noise head into the demapper, so its `llr_1x1/kernel` is [1,1,33,8] against
[1,1,32,8] everywhere else; the constellation head has no `llr_1x1` at all; the post-filter has four extra layers. A
silent `load_weights` would either fail or (worse) align mismatched tensors by position, so this refuses unless every
name and shape agrees, and `--partial` averages the common subset while reporting exactly what it skipped.

Usage:
    python avg_weights.py out.keras a.keras b.keras [c.keras ...] [--partial]
"""
import argparse
import sys

import numpy as np
import tensorflow as tf

CODE = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train"
sys.path.insert(0, CODE)
import train_depth3 as T   # noqa: E402

CUSTOM = T.CUSTOM_OBJECTS  # ONE dict: a layer the trainer registers must load in every probe/aggregation tool


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("models", nargs="+")
    ap.add_argument("--partial", action="store_true",
                    help="average only the weights every model shares; copy the rest from the first model")
    args = ap.parse_args()

    models = [tf.keras.models.load_model(p, compile=False, custom_objects=CUSTOM) for p in args.models]
    ref = {w.path: w for w in models[0].weights}
    ref_shapes = {k: tuple(w.shape) for k, w in ref.items()}
    for tag, m in zip(args.models[1:], models[1:]):
        other = {w.path: tuple(w.shape) for w in m.weights}
        diff = {k for k in set(ref_shapes) & set(other) if ref_shapes[k] != other[k]}
        missing = set(ref_shapes) - set(other)
        extra = set(other) - set(ref_shapes)
        if diff or missing or extra:
            print(f"{tag}: shapes differ {sorted(diff)} | missing {sorted(missing)} | extra {sorted(extra)}")
            if not args.partial:
                raise SystemExit("refusing to average mismatched layouts (pass --partial to average the common part)")

    acc, n = {}, {}
    for m in models:
        for w in m.weights:
            if w.path not in ref_shapes:
                continue
            v = np.asarray(w)
            if w.path not in ref_shapes or tuple(v.shape) != ref_shapes[w.path]:
                continue        # only identical shapes are averaged
            acc[w.path] = acc.get(w.path, 0.0) + v.astype(np.float64)
            n[w.path] = n.get(w.path, 0) + 1
    # write the averaged values back into the first model (which keeps every layer, including ones it will drop later)
    for w in models[0].weights:
        if w.path in acc and n[w.path] > 1:
            w.assign(acc[w.path] / n[w.path])
    models[0].save(args.out)
    print(f"averaged {sum(1 for k in n if n[k] > 1)} weight tensors over {len(models)} models -> {args.out}")
    skipped = [k for k in ref_shapes if n.get(k, 0) <= 1]
    if skipped:
        print(f"kept from the first model (not averaged): {len(skipped)} tensors, e.g. {sorted(skipped)[:4]}")


if __name__ == "__main__":
    main()
