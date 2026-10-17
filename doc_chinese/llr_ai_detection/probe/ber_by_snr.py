#!/usr/bin/env python3
"""Uncoded BER of the LLR head as a function of SNR, per architecture depth.

Why the overall BER is not the answer: the validation mix spans the whole declared domain, and most of it is where
any receiver is near chance, so an overall BER is dominated by samples no one can decode. The chain only ever
operates at its own SNR, so the question "how much LLR capability does a shallower trunk cost" has to be asked INSIDE
an SNR bin. This is also the honest way to compare a depth ablation with the deployed model: same samples, same bin.

Usage:
    ber_by_snr.py --models "18=data/models/refit_b18.keras,12=data/models/refit_b12.keras" \
        --data data/datasets/d3_modcond_val.npz [--modulation 256QAM] [--per-bin 40]
"""
import argparse
import collections
import os
import sys

import numpy as np

AI = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train"
sys.path.insert(0, AI)

import tensorflow as tf  # noqa: E402

tf.get_logger().setLevel("ERROR")
import depth3 as D3  # noqa: E402
import train_depth3 as T  # noqa: E402

BINS = [(12, 16), (16, 20), (20, 24), (24, 28), (28, 32), (32, 40)]


def ber_for(model, data, modulation, per_bin):
    """{bin: [bits, wrong]} for the samples of ONE modulation, gathered exactly as the arm gathers them."""
    out = collections.defaultdict(lambda: [0, 0])
    counts = collections.Counter()
    want = D3.BITS_PER_SYMBOL[modulation]
    for index in range(data.n):
        if D3.BITS_PER_SYMBOL[data.modulation_of(index)] != want:
            continue
        snr = float(data.data["snr_nominal"][index])
        slot = next((bin_ for bin_ in BINS if bin_[0] <= snr < bin_[1]), None)
        if slot is None or counts[slot] >= per_bin:
            continue
        sample = data.sample(index)
        llr = model(np.asarray(sample["x"])[None, ...], training=False)["llr"].numpy()[0]
        gathered = D3.llr_gather_cond(llr, data.allocation_of(index), data.dmrs_symbols_of(index),
                                      data.modulation_of(index))
        sent = np.unpackbits(data.data["bits"][index])[:int(data.data["nof_bits"][index])]
        if gathered.size != sent.size:
            continue
        counts[slot] += 1
        # The receiver decides bit 0 on a POSITIVE soft bit, so the hard decision is `llr < 0`.
        out[slot][0] += sent.size
        out[slot][1] += int(np.sum((gathered < 0).astype(np.uint8) != sent))
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--models", required=True, help="depth=path pairs, comma separated")
    parser.add_argument("--data", default="/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/"
                                          "work_tmp/data/datasets/d3_modcond_val.npz")
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--per-bin", type=int, default=40)
    args = parser.parse_args()

    models = []
    for pair in args.models.split(","):
        depth, path = pair.split("=", 1)
        model = tf.keras.models.load_model(path, compile=False,
                                           custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
        models.append((depth, model))

    data = T.Batches(args.data, args.modulation, 0, seed=2, layout="spatial", cond=True)
    header = f"{args.modulation:>9}" + "".join(f"{low}-{high} dB".rjust(12) for low, high in BINS)
    print("uncoded BER by SNR bin (bits in brackets); the same validation samples for every depth")
    print(f"{'depth':>6} {header}")
    for depth, model in models:
        stats = ber_for(model, data, args.modulation, args.per_bin)
        row = f"{depth:>6} "
        for slot in BINS:
            bits, wrong = stats.get(slot, [0, 0])
            row += (f"{wrong/bits:12.5f}" if bits else f"{'-':>12}") + f" ({bits:6d})"
        print(row)
    return 0


if __name__ == "__main__":
    sys.exit(main())
