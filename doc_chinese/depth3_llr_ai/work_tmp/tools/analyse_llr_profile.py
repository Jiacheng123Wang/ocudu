"""Profile a checkpoint's LLR errors by the grid's PHYSICAL structure, before spending a training run.

WHY THIS EXISTS. The architecture discussion kept producing "plausible" mechanisms (the DMRS comb parity, the
time interpolation from two pilot symbols, the receptive field). Each of them implies a *shape* in the error
profile: a parity hypothesis says one subcarrier class is worse, a pilot-distance hypothesis says symbols far
from DMRS are worse, a "needs more context" hypothesis says the edges of the allocation are worse. Those are
cheap to test and expensive to be wrong about -- a 20-epoch run on the GPU box is seven hours, this is one
minute. The tool therefore prints:

  * BER by subcarrier parity and by subcarrier mod 4 (a dilation-4 alias would show up here),
  * BER by distance to the nearest DMRS symbol (time interpolation),
  * BER by nominal-SNR band, and the per-unit BER spread (is the failure per position or per reception?),
  * and, for reference, the same numbers for the operating point 24-26 dB where the 256QAM bits are decided.

WHAT IT MEASURED (2026-11-08, monitor set, 138 x 256QAM receptions):

  parity 0 vs 1: 0.0667 vs 0.0667 (RF 127) and 0.0674 vs 0.0673 (RF 379)  -> flat, the parity hypothesis is dead,
      even though 13 of 18 blocks run an even dilation and cannot mix the two combs within a layer. Depth mixes
      them anyway.
  distance to DMRS: 0.0658 / 0.0670 / 0.0672 / 0.0673  -> flat, so interpolating two pilot symbols across 14 is
      not the bottleneck (the trunk's time receptive field is 1 + 2*18 = 37 symbols, i.e. the whole slot).
  SNR bands (dw e52): 0.1824 / 0.0467 / 0.0244 / 0.0036 / 0.0018  -> all the errors are at low SNR; at the
      operating point the BER is 0.36%, and the arms that deliver more bits have a lower number there
      (weighted 0.0033, dense 0.0033) while the arm that delivers fewer has a higher one (RF 379: 0.0041).
  per-unit spread: median 0.0287, p90 0.2847  -> the failures are concentrated in a minority of receptions,
      not spread over the grid.

USE IT AS A SCREEN: a candidate whose 24-26 dB BER is not below the incumbent's will not deliver more bits, so
do not pay for a probe or a ruler until this number moves.

    python analyse_llr_profile.py <checkpoint.keras> [<checkpoint.keras> ...] [--limit N]
"""
import argparse
import os
import sys

import numpy as np
import tensorflow as tf

LIB = os.path.expanduser("~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train")
sys.path.insert(0, LIB)
import train_depth3 as T  # noqa: E402  (the trainer owns the dataset reader and the custom layer)
import depth3 as D3  # noqa: E402

VAL = os.path.expanduser("~/ai_depth3/w2v1_data/datasets_monitor_w2")
EDGES = [0, 20, 22, 24, 26, 99]


def analyse(ckpt, limit):
    val = T.Batches(VAL, "QPSK", 0, seed=20261105, cond=True, layout="spatial")
    model = tf.keras.models.load_model(ckpt, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    idx = [i for i in range(val.n) if val.modulation_of(i) == "256QAM"][:limit]
    par = {0: [0, 0], 1: [0, 0]}
    mod4 = {m: [0, 0] for m in range(4)}
    dist = {}
    band = {k: [0, 0] for k in range(len(EDGES) - 1)}
    per_unit = []
    for i in idx:
        sample = val.sample(i)
        out = model(tf.constant(sample["x"][None]), training=False)
        channels = sample["llr_shape"].shape[1]
        llr = tf.reshape(tf.cast(out["llr"], tf.float32),
                         (-1, D3.NOF_SUBCARRIERS, channels)).numpy()[0] * float(sample["scale"])
        label, weights = sample["llr_shape"], sample["llr_weights"]
        dmrs = val.dmrs_symbols_of(i)
        snr = float(sample["snr"])
        k = next(j for j in range(len(EDGES) - 1) if EDGES[j] <= snr < EDGES[j + 1])
        unit_err = unit_n = 0
        for c in range(llr.shape[1]):
            symbol = c // D3.COND_STRIDE
            d = min(abs(symbol - s) for s in dmrs)
            for sc in range(llr.shape[0]):
                if weights[sc, c] <= 0:
                    continue
                err = int((llr[sc, c] > 0) != (label[sc, c] > 0.5))
                for slot in (par[sc % 2], mod4[sc % 4], band[k], dist.setdefault(min(d, 6), [0, 0])):
                    slot[0] += err
                    slot[1] += 1
                unit_err += err
                unit_n += 1
        per_unit.append(unit_err / max(unit_n, 1))
    head = os.path.basename(ckpt)
    print(f"\n=== {head}  (256QAM receptions: {len(idx)})")
    print(f"  parity 0/1 : {par[0][0]/par[0][1]:.4f} / {par[1][0]/par[1][1]:.4f}")
    print("  mod 4      : " + "  ".join(f"{m}:{mod4[m][0]/mod4[m][1]:.4f}" for m in range(4)))
    print("  to DMRS    : " + "  ".join(f"d{k}:{dist[k][0]/dist[k][1]:.4f}" for k in sorted(dist)))
    print("  SNR bands  : " + "  ".join(
        f"{EDGES[b]}-{EDGES[b+1]}:{band[b][0]/band[b][1]:.4f}" for b in range(len(EDGES) - 1) if band[b][1]))
    u = np.asarray(per_unit)
    print(f"  per unit   : median {np.median(u):.4f}  p90 {np.percentile(u, 90):.4f}  max {u.max():.4f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoints", nargs="+")
    ap.add_argument("--limit", type=int, default=150)
    args = ap.parse_args()
    for ckpt in args.checkpoints:
        if os.path.exists(ckpt):
            analyse(ckpt, args.limit)
        else:
            print(f"missing {ckpt}")


if __name__ == "__main__":
    main()
