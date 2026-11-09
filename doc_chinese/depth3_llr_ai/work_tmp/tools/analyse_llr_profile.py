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
# ALLOCATION WIDTH IS THE OTHER AXIS THAT MATTERS. The units the CPU wins are 2 PRB at 24-32 dB and a code rate
# near one, so the profile has to say whether the error rate at 24-28 dB is the same in a narrow allocation as in a
# wide one: if it is, the model is not worse there and the block losses come only from the missing redundancy.
PRB_EDGES = [1, 4, 9, 25, 10 ** 6]
PRB_LABEL = ["<=3", "4-8", "9-24", ">24"]


def analyse(ckpt, limit, data_dir=None, modulation="256QAM", prb_min=0, prb_max=10 ** 6, edges=None):
    global EDGES
    if edges:
        EDGES = edges
    val = T.Batches(data_dir or VAL, "QPSK", 0, seed=20261105, cond=True, layout="spatial")
    model = tf.keras.models.load_model(ckpt, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    matched = [i for i in range(val.n)
               if val.modulation_of(i) == modulation
               and prb_min <= int(val.data["alloc_nof_prb"][i]) <= prb_max]
    # TAKE A SPREAD, NOT THE HEAD. The datasets are ordered by bin, so `matched[:limit]` is one bin (typically the
    # first SNR band) and says nothing about the others; every k-th sample covers the whole set at the same cost.
    if limit and len(matched) > limit:
        stride = len(matched) // limit
        matched = matched[::stride][:limit]
    idx = matched
    par = {0: [0, 0], 1: [0, 0]}
    mod4 = {m: [0, 0] for m in range(4)}
    dist = {}
    band = {k: [0, 0] for k in range(len(EDGES) - 1)}
    prb_band = {(w, k): [0, 0] for w in range(len(PRB_LABEL)) for k in range(len(EDGES) - 1)}
    llr_abs = {(w, k): 0.0 for w in range(len(PRB_LABEL)) for k in range(len(EDGES) - 1)}
    # DISTANCE TO THE ALLOCATION EDGE, in subcarriers. A 2-PRB reception occupies 24 of the 612 subcarriers the model
    # sees, so every one of its REs is close to the zero region outside the allocation while a wide allocation has an
    # interior. If the error concentrates near the edge, the input representation (zeros reading as deep fades) is the
    # suspect; if it is flat inside, the suspect is the channel/noise estimate over few REs.
    edge = {}
    # BER CONDITIONED ON THE MODEL'S OWN CONFIDENCE. If the errors sit where |LLR| is small, the model is unsure and
    # the information is missing from the input (channel or noise estimate); if they sit where |LLR| is large, the
    # model is confidently wrong, which points at a representation or label defect instead.
    LL = [(0, 1), (1, 2), (2, 4), (4, 8), (8, 16), (16, 1e9)]
    ll = {j: [0, 0] for j in range(len(LL))}
    llr_n = {(w, k): 0 for w in range(len(PRB_LABEL)) for k in range(len(EDGES) - 1)}
    per_unit = []
    skipped = 0
    for i in idx:
        sample = val.sample(i)
        out = model(tf.constant(sample["x"][None]), training=False)
        channels = sample["llr_shape"].shape[1]
        llr = tf.reshape(tf.cast(out["llr"], tf.float32),
                         (-1, D3.NOF_SUBCARRIERS, channels)).numpy()[0] * float(sample["scale"])
        label, weights = sample["llr_shape"], sample["llr_weights"]
        dmrs = val.dmrs_symbols_of(i)
        snr = float(sample["snr"])
        # A SAMPLE OUTSIDE THE TABLE'S RANGE IS SKIPPED, NEVER CLAMPED. Clamping put every reception below the first
        # edge into the LAST band, which made a 26-99 dB band look catastrophically bad whenever the edges did not
        # start at zero -- a pure analysis artefact that read like a 60x regression.
        k = next((j for j in range(len(EDGES) - 1) if EDGES[j] <= snr < EDGES[j + 1]), None)
        if k is None:
            skipped += 1
            continue
        prb = int(val.data["alloc_nof_prb"][i])
        w = next(j for j in range(len(PRB_LABEL)) if PRB_EDGES[j] <= prb < PRB_EDGES[j + 1])
        cols = [sc for sc in range(llr.shape[0]) if weights[sc].any()]
        lo, hi = (min(cols), max(cols)) if cols else (0, 0)
        unit_err = unit_n = 0
        for c in range(llr.shape[1]):
            symbol = c // D3.COND_STRIDE
            d = min(abs(symbol - s) for s in dmrs)
            for sc in range(llr.shape[0]):
                if weights[sc, c] <= 0:
                    continue
                err = int((llr[sc, c] > 0) != (label[sc, c] > 0.5))
                d_edge = min(sc - lo, hi - sc)
                j = next(jj for jj in range(len(LL)) if LL[jj][0] <= abs(float(llr[sc, c])) < LL[jj][1])
                ll[j][0] += err
                ll[j][1] += 1
                for slot in (par[sc % 2], mod4[sc % 4], band[k], prb_band[(w, k)],
                             edge.setdefault(min(d_edge, 8), [0, 0]),
                             dist.setdefault(min(d, 6), [0, 0])):
                    slot[0] += err
                    slot[1] += 1
                llr_abs[(w, k)] += abs(float(llr[sc, c]))
                llr_n[(w, k)] += 1
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
    print("  BER by width x SNR band (n in brackets):")
    print("      width  " + "".join(f"{f'{EDGES[k]}-{EDGES[k+1]}':>16}" for k in range(len(EDGES) - 1)))
    for w, label in enumerate(PRB_LABEL):
        row = f"      {label:6} "
        for k in range(len(EDGES) - 1):
            e, n = prb_band[(w, k)]
            row += (f"{e/n:10.4f} ({n:4d})" if n else f"{'-':>16}")
        print(row)
    if skipped:
        print(f"  NOTE: {skipped} reception(s) fell outside the SNR table and were skipped (use --edges covering the range)")
    print("  BER by distance to the allocation edge (subcarriers, capped at 8):")
    print("      " + "  ".join(f"d{k}:{edge[k][0]/edge[k][1]:.4f}({edge[k][1]})" for k in sorted(edge)))
    print("  BER conditioned on |LLR| (the model's own confidence):")
    print("      " + "  ".join(f"{LL[j][0]:g}-{LL[j][1]:g}:{ll[j][0]/ll[j][1]:.4f}({ll[j][1]})"
                              for j in range(len(LL)) if ll[j][1]))
    print("  mean |LLR| by width x SNR band:")
    for w, label in enumerate(PRB_LABEL):
        row = f"      {label:6} "
        for k in range(len(EDGES) - 1):
            n = llr_n[(w, k)]
            row += (f"{llr_abs[(w, k)]/n:16.2f}" if n else f"{'-':>16}")
        print(row)
    u = np.asarray(per_unit)
    print(f"  per unit   : median {np.median(u):.4f}  p90 {np.percentile(u, 90):.4f}  max {u.max():.4f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoints", nargs="+")
    ap.add_argument("--limit", type=int, default=150)
    ap.add_argument("--data", default=None, help="dataset directory (default: the monitor set)")
    ap.add_argument("--modulation", default="256QAM")
    ap.add_argument("--prb-min", type=int, default=0)
    ap.add_argument("--prb-max", type=int, default=10 ** 6)
    ap.add_argument("--edges", default=None,
                    help="comma separated SNR band edges, e.g. 0,4,8,12,16,18,20,22,24,26,99")
    args = ap.parse_args()
    for ckpt in args.checkpoints:
        if os.path.exists(ckpt):
            edges = [float(x) for x in args.edges.split(",")] if args.edges else None
            analyse(ckpt, args.limit, args.data, args.modulation, args.prb_min, args.prb_max, edges)
        else:
            print(f"missing {ckpt}")


if __name__ == "__main__":
    main()
