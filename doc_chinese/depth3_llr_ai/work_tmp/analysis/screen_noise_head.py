"""Does the model's own `post_eq_noise` head carry per-RE information about where it is wrong?

WHY THIS SCREEN EXISTS. Plan L6 would feed that head back into the LLR as a per-RE calibration, which costs ~50 lines
and one 40-epoch run (~15 h on the 107). Before spending that, ask the cheap question: does the head's output predict
the per-RE error at all? If the error rate is flat across noise-head quantiles, L6 has nothing to calibrate and the
lever should be dropped.

WHAT IT REPORTS
  * Spearman correlation between the head's output and |LLR| (a calibrated head should be anti-correlated: more
    predicted noise, less confident the model)
  * error rate and mean |LLR| per quintile of the head's output -- the shape that matters: does error rise with it?

Usage:
    python screen_noise_head.py <checkpoint.keras> [--limit 150] [--data DIR] [--modulation 256QAM]
"""
import argparse
import os
import sys

import numpy as np

D3 = os.path.expanduser("~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train")
# The monitor set every other analysis uses; same default as analyse_llr_profile.py.
VAL = os.path.expanduser("~/ai_depth3/w2v1_data/datasets_monitor_w2")


def spearman(a, b):
    """Rank correlation without scipy: correlate the ranks, ties averaged."""
    def rank(v):
        order = v.argsort()
        r = np.empty_like(order, dtype=np.float64)
        r[order] = np.arange(len(v))
        # average ties
        _, inv, counts = np.unique(v, return_inverse=True, return_counts=True)
        sums = np.bincount(inv, weights=r)
        return (sums / counts)[inv]
    ra, rb = rank(a), rank(b)
    ra -= ra.mean(); rb -= rb.mean()
    den = np.sqrt((ra ** 2).sum() * (rb ** 2).sum())
    return float((ra * rb).sum() / den) if den else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint")
    ap.add_argument("--limit", type=int, default=150)
    ap.add_argument("--data", default=None)
    ap.add_argument("--modulation", default="256QAM")
    ap.add_argument("--seed", type=int, default=20261105)
    args = ap.parse_args()

    sys.path.insert(0, D3)
    import tensorflow as tf
    import train_depth3 as T

    val = T.Batches(args.data or VAL, "QPSK", 0, seed=args.seed, cond=True, layout="spatial")
    model = tf.keras.models.load_model(args.checkpoint, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    idx = [i for i in range(val.n) if val.modulation_of(i) == args.modulation]
    if args.limit and len(idx) > args.limit:
        stride = len(idx) // args.limit
        idx = idx[::stride][:args.limit]
    print(f"receptions {len(idx)}  modulation {args.modulation}")

    noise_all, llr_all, err_all = [], [], []
    shapes_printed = False
    for i in idx:
        s = val.sample(i)
        out = model(tf.constant(s["x"][None]), training=False)
        llr = tf.reshape(tf.cast(out["llr"], tf.float32), (-1, T.D3.NOF_SUBCARRIERS, s["llr_shape"].shape[1])) \
            .numpy()[0] * float(s["scale"])
        # ONE value per RE, the LLR head one per (RE, bit): the noise grid is [subcarrier, symbol] and the LLR
        # columns are symbol-major with COND_STRIDE bits each, so repeat each symbol to bit resolution.
        n_re = np.asarray(out["post_eq_noise"]).reshape(-1, llr.shape[0], 14)[0]
        noise = np.repeat(n_re, llr.shape[1] // 14, axis=1)
        if not shapes_printed:
            print(f"llr {llr.shape}  noise {noise.shape}")
            shapes_printed = True
        label, weights = s["llr_shape"], s["llr_weights"]
        m = weights > 0
        noise_all.append(noise[m])
        llr_all.append(np.abs(llr)[m])
        err_all.append(((llr > 0) != (label > 0.5))[m].astype(np.float32))

    noise = np.concatenate(noise_all); llr = np.concatenate(llr_all); err = np.concatenate(err_all)
    print(f"REs with a label: {len(err):,}   overall BER {err.mean():.4f}")
    print(f"noise head: min {noise.min():.4f} median {np.median(noise):.4f} max {noise.max():.4f}")
    print(f"Spearman(noise, |LLR|) = {spearman(noise, llr):+.3f}   "
          f"Spearman(noise, err) = {spearman(noise, err):+.3f}")

    q = np.quantile(noise, [0.0, 0.2, 0.4, 0.6, 0.8, 1.0])
    print("\nquintile of the noise head  ->  error rate / mean |LLR|")
    for j in range(5):
        sel = (noise >= q[j]) & (noise <= q[j + 1] if j == 4 else noise < q[j + 1])
        if not sel.any():
            continue
        print(f"  Q{j + 1} [{q[j]:8.4f}, {q[j + 1]:8.4f}]  err {err[sel].mean():.4f}   "
              f"|LLR| {llr[sel].mean():7.2f}   n={sel.sum():,}")
    # ★ THE DECIDING TABLE: does the head still separate errors WITHIN a confidence band? The model's own |LLR|
    # already tracks the head (Spearman -0.7), so the head only earns its place if the error rate still moves with
    # it at fixed |LLR| -- i.e. if it says something the LLR magnitude does not already say.
    print("\n★ error rate inside each |LLR| quartile, across noise quintiles (does the head add anything?)")
    lq = np.quantile(llr, [0.0, 0.25, 0.5, 0.75, 1.0])
    print("      |LLR| band        " + "".join(f"  N{j+1:<9}" for j in range(5)))
    for a in range(4):
        sel_l = (llr >= lq[a]) & (llr <= lq[a + 1] if a == 3 else llr < lq[a + 1])
        row = f"  [{lq[a]:6.2f},{lq[a+1]:6.2f}) "
        for j in range(5):
            sel_n = (noise >= q[j]) & (noise <= q[j + 1] if j == 4 else noise < q[j + 1])
            sel = sel_l & sel_n
            row += f"{err[sel].mean():10.4f}" if sel.any() else f"{'-':>10}"
        print(row)

    ratio = err[(noise >= q[4])].mean() / max(err[(noise <= q[1])].mean(), 1e-9)
    print(f"\nVERDICT: top quintile / bottom quintile error ratio = {ratio:.2f}x "
          f"({'signal: L6 worth a pilot' if ratio >= 1.5 else 'flat: drop L6 or find another signal'})")


if __name__ == "__main__":
    main()
