#!/usr/bin/env python3
"""Which chain's published SINR is closest to a PRESCRIBED one?

O5' of `model_network_design` section 46. The arm's publication was calibrated against the
CLASSICAL arm's publication, and the arm's own error against truth is +0.26..+1.22 dB over
12-30 dB (`heads_vs_prescribed.py`). If the classical chain sits well above truth, then the
"the arm reads 1.2-3.4 dB low" reading was a statement about the REFERENCE, and the calibration
moved the arm away from truth rather than towards it. This probe measures that directly.

Method: for every unit and every prescribed SNR, build a synthetic grid from the unit's measured
channel with `build_synth_dataset.build_and_replay` (which stages the grid and prescribes
`sigma2 = sig / 10^(snr/10)`), then replay THE SAME staged grid through each receiver and read the
`sinr=` it publishes on its PUSCH line. Both chains are compared against the same prescribed
value, which is the comparison the arm's ladder already made for one of them.

Usage:
  classic_vs_prescribed.py --corpus <dir> --tb <bits-dir> [--snrs 0,6,12,18,24,30]
                           [--max-units N] [--model <mlmodelc>] [--out <csv>]
"""

import argparse
import os
import re
import subprocess
import sys

import numpy as np

AI_TRAIN = ("/Users/jiachengwang/dev/ocudu/lib/phy/upper/"
            "signal_processors/channel_estimator/metal/ai_train")
sys.path.insert(0, AI_TRAIN)
import build_synth_dataset as B   # noqa: E402
import depth3 as D3               # noqa: E402
import synth as SN                # noqa: E402

SINR = re.compile(r"sinr=(-?[\d.]+) dB")


def read_sinr(stdout):
    """The SINR a run published, one value per reception it logged."""
    return [float(v) for v in SINR.findall(stdout)]


def replay(staged, out, receiver, model=None):
    env = dict(os.environ, OCUDU_REPLAY_RECEIVER=receiver)
    if model:
        env["OCUDU_REPLAY_MODEL"] = model
    result = subprocess.run([B.REPLAY, staged, "--metal", "--out", out],
                            env=env, capture_output=True, text=True, check=False)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--tb", required=True)
    parser.add_argument("--snrs", default="0,6,12,18,24,30")
    parser.add_argument("--max-units", type=int, default=3)
    parser.add_argument("--min-prb", type=int, default=43,
                        help="same selection rule as the training corpus: narrower units do not "
                             "fill the network's window")
    parser.add_argument("--model", default="", help="also measure the AI arm with this bundle")
    parser.add_argument("--staged", default="/tmp/classic_vs_prescribed")
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    snrs = [float(x) for x in args.snrs.split(",")]
    os.makedirs(args.staged, exist_ok=True)

    units = []
    for name in sorted(os.listdir(args.corpus)):
        if not name.startswith("C_") or not name.endswith(".bin") or name.endswith("_h.bin"):
            continue
        stem = name[:-4]
        prefix = os.path.join(args.corpus, stem)
        if not (os.path.exists(prefix + "_h.bin") and os.path.exists(prefix + ".txt")):
            continue
        if not os.path.exists(os.path.join(args.tb, stem + ".bits")):
            continue
        cfg = SN.read_config(prefix)
        alloc = [int(x) for x in cfg["alloc_prb"].split(",")]
        if min(alloc) == 0 or int(cfg["slot"]) < 40 or len(alloc) < args.min_prb:
            continue
        units.append((stem, prefix, cfg, alloc))
    units = units[:args.max_units]
    if not units:
        raise SystemExit("no unit matched the selection rule")

    rows = []
    print(f"{len(units)} unit(s), prescribed SNR {args.snrs} dB"
          f"{', plus the AI arm' if args.model else ''}")
    header = f"{'unit':>22} {'prescr':>7} {'classical':>10} {'err':>7}"
    print(header + (f"{'ai':>9} {'err':>7}" if args.model else ""))
    for stem, prefix, cfg, alloc in units:
        bwp = int(cfg["bwp_size_rb"])
        H = np.fromfile(prefix + "_h.bin", dtype=np.complex64).reshape(D3.NOF_SYMBOLS, bwp * 12)
        tb_bits = np.unpackbits(np.fromfile(os.path.join(args.tb, stem + ".bits"),
                                            dtype=np.uint8))
        staged = os.path.join(args.staged, stem)
        with open(staged + ".src", "w") as handle:
            handle.write(open(prefix + ".txt").read())
        sub = np.concatenate([np.arange(r * 12, (r + 1) * 12) for r in alloc])
        keep = sub[:-max(1, int(len(sub) * 0.03))]
        for snr in snrs:
            # Staging is the only thing `build_and_replay` is used for here; its returned
            # estimate is the metal estimator's, which is not what this probe asks about.
            B.build_and_replay(cfg, tb_bits, H, alloc, snr, 20261017, staged,
                               truth_channel=H)
            classic = read_sinr(replay(staged, staged + "_classic", "classic"))
            ai = read_sinr(replay(staged, staged + "_ai", "ai", args.model)) if args.model else []
            c = float(np.median(classic)) if classic else float("nan")
            a = float(np.median(ai)) if ai else float("nan")
            rows.append((stem, snr, c, a))
            line = f"{stem:>22} {snr:7.1f} {c:10.2f} {c - snr:+7.2f}"
            if args.model:
                line += f" {a:8.2f} {a - snr:+7.2f}"
            print(line)

    if rows:
        err = [r[2] - r[1] for r in rows if np.isfinite(r[2])]
        if err:
            print(f"\nclassical publication minus prescribed: mean {np.mean(err):+.2f} dB, "
                  f"min {np.min(err):+.2f}, max {np.max(err):+.2f} over {len(err)} point(s)")
        if args.model:
            aerr = [r[3] - r[1] for r in rows if np.isfinite(r[3])]
            if aerr:
                print(f"AI publication minus prescribed:        mean {np.mean(aerr):+.2f} dB, "
                      f"min {np.min(aerr):+.2f}, max {np.max(aerr):+.2f}")
    if args.out:
        with open(args.out, "w") as handle:
            handle.write("unit,prescribed_db,classical_db,ai_db\n")
            for stem, snr, c, a in rows:
                handle.write(f"{stem},{snr:g},{c:.3f},{a:.3f}\n")
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
