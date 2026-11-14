"""Measure the depth3 training step rate on whatever GPU this host has, in samples/s.

WHY IT IS HERE AND NOT IN /tmp. `/tmp` gets cleared (it happened), and this is the script that tells us whether a new
host is worth training on at all -- memo D.5/D.8 in particular. It is synthetic on purpose: the point is the compute
rate, not convergence, so it runs in a minute and needs no dataset.

Usage:
    python bench_depth3_gpu.py --code <path to depth3/ai_train> --dtype float16 --batch 16 32 64 [--jit]

The step is the trainer's own: the built model, the LLR head, the masked BCE, gradients and an Adam update.
"""
import argparse
import sys
import time

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--code", required=True, help="directory holding train_depth3.py and depth3_loss.py")
    ap.add_argument("--dtype", default="float16", choices=("float16", "float32"))
    ap.add_argument("--blocks", type=int, default=18)
    ap.add_argument("--c-hid", type=int, default=32)
    ap.add_argument("--dense", action="store_true")
    ap.add_argument("--kernel-size", type=int, default=3)
    ap.add_argument("--groups", type=int, default=1)
    ap.add_argument("--batch", type=int, nargs="+", default=[16])
    ap.add_argument("--jit", action="store_true", help="compile the step with XLA")
    ap.add_argument("--reps", type=int, default=10)
    args = ap.parse_args()

    import tensorflow as tf
    sys.path.insert(0, args.code)
    import depth3_loss as L
    import train_depth3 as T

    print(f"TF {tf.__version__}  GPUs {[d.name for d in tf.config.list_physical_devices('GPU')]}")
    model = T.build(blocks=args.blocks, c_hid=args.c_hid, qm=8, aux_heads=True, reporting_heads=False,
                    dtype=args.dtype, layout="spatial", in_channels=16, cond=True, dense=args.dense,
                    scale_init=0.03, ksize=args.kernel_size, groups=args.groups)
    print(f"parameters {int(sum(np.prod(w.shape) for w in model.weights)):,}")
    opt = tf.keras.optimizers.Adam(1e-3)
    for batch in args.batch:
        rng = np.random.default_rng(0)
        x0 = tf.constant(rng.random((batch, 612, 14, 16), dtype=np.float32))
        lab0 = tf.constant((rng.random((batch, 612, 112)) < 0.5).astype(np.float32))
        mask0 = tf.ones((batch, 612, 112), dtype=tf.float32)

        @tf.function(jit_compile=args.jit)
        def step(x, lab, mask):
            with tf.GradientTape() as tape:
                out = model(x, training=True)
                llr = tf.reshape(tf.cast(out["llr"], tf.float32), (batch, 612, 112)) * 20.0
                loss = L.tf_bce_loss(llr, lab, mask)
            g = tape.gradient(loss, model.trainable_variables)
            opt.apply_gradients(zip(g, model.trainable_variables))

        try:
            for _ in range(3):
                step(x0, lab0, mask0)
            t0 = time.time()
            for _ in range(args.reps):
                step(x0, lab0, mask0)
            dt = (time.time() - t0) / args.reps
            print(f"  batch {batch:3d}: {dt*1000:8.1f} ms/step => {batch/dt:7.1f} samples/s"
                  f"  (dtype {args.dtype}, jit={args.jit})")
        except Exception as exc:                                    # noqa: BLE001 - report and continue
            print(f"  batch {batch:3d}: FAILED {type(exc).__name__}: {str(exc)[:110]}")


if __name__ == "__main__":
    main()
