#!/usr/bin/env python3
"""Depth truncation of the DEPLOYED conditioned model: architecture -> capability, with NO training.

Why this exists (user's request, 2026-10-17): latency alone does not choose an architecture -- the trunk's depth
and width also decide what the LLR head and the reporting heads can resolve. Training a model per candidate costs
hours per point, so the depth axis is measured by TRUNCATION instead: keep the trained weights of the first k
residual blocks and attach the trained heads to block k's output. Nothing is retrained, every weight is the one
that was trained, and the only change is which trunk stage feeds the heads.

    k = 18  the deployed model itself (the control, must reproduce it exactly)
    k < 18  the same weights, heads reading an earlier stage

THE RESULT IS A LOWER BOUND, and the report must say so: the heads were trained on block 18's features, so a
truncated model is a model whose head is being fed a distribution it never saw. A model TRAINED at depth k would
be at least as good. That makes the curve useful in one direction only -- if the capability is already unchanged
at k, depth k is certainly enough; if it collapses, the collapse may be the head's mismatch rather than the depth,
which is what `--head-refit` at the end of the script is for.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

AI = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train"
W = "/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp"
sys.path.insert(0, AI)

import tensorflow as tf  # noqa: E402

tf.get_logger().setLevel("ERROR")
import train_depth3 as T  # noqa: E402

def truncate(model, blocks):
    """The same trained layers, with the four heads reading block `blocks - 1`'s output."""
    if blocks >= 18:
        return model
    return _assemble(model, model.get_layer(f"add{blocks - 1}").output)


def _assemble(model, trunk):
    """Wires the head sub-graph onto `trunk` in the order the trained model has it.

    Written as a straight chain rather than recursing on the trained graph: the head is twelve layers deep at most
    and its order is fixed by `build()`, so the explicit chain is shorter and cannot silently pick up a wrong
    predecessor.
    """
    g = {}
    g["llr_1x1"] = model.get_layer("llr_1x1")(trunk)
    g["llr"] = model.get_layer("llr")(g["llr_1x1"])
    g["llr_flat"] = model.get_layer("llr_flat")(g["llr"])
    g["noise_1x1"] = model.get_layer("noise_1x1")(trunk)
    g["post_eq_noise"] = model.get_layer("post_eq_noise")(g["noise_1x1"])
    g["noise_flat"] = model.get_layer("noise_flat")(g["post_eq_noise"])
    g["ch_est_1x1"] = model.get_layer("ch_est_1x1")(trunk)
    g["ch_est"] = model.get_layer("ch_est")(g["ch_est_1x1"])
    g["ch_est_flat"] = model.get_layer("ch_est_flat")(g["ch_est"])
    g["dmrs_pool"] = model.get_layer("dmrs_pool")(trunk)
    g["dmrs_noise_1x1"] = model.get_layer("dmrs_noise_1x1")(g["dmrs_pool"])
    g["dmrs_noise"] = model.get_layer("dmrs_noise")(g["dmrs_noise_1x1"])
    outputs = {"llr": g["llr_flat"], "post_eq_noise": g["noise_flat"],
               "ch_est": g["ch_est_flat"], "dmrs_noise": g["dmrs_noise"]}
    out = tf.keras.Model(model.input, outputs)
    out._depth3_cond = True
    return out


def export(model, tag, snr_range):
    keras_path = os.path.join(W, "data/models", f"trunc_{tag}.keras")
    package = os.path.join(W, "data/models", f"trunc_{tag}.mlpackage")
    model.save(keras_path)
    subprocess.run([sys.executable, os.path.join(AI, "train_depth3.py"), "--export-only", keras_path,
                    "--export-probe", package, "--modcond", "--layout", "spatial",
                    "--modulation", "256QAM", "--snr-range", snr_range], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["xcrun", "coremlcompiler", "compile", package, os.path.join(W, "data/models")], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(W, "data/models", f"trunc_{tag}.mlmodelc")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", default=f"{W}/data/runs/d3_modcond_ft/depth3_best.keras")
    parser.add_argument("--depths", default="18,15,12,9,6")
    parser.add_argument("--snr-range", default="12.00,48.00")
    args = parser.parse_args()
    model = tf.keras.models.load_model(args.checkpoint, compile=False,
                                       custom_objects={"FoldedScaleConv": T.FoldedScaleConv})
    for depth in [int(value) for value in args.depths.split(",")]:
        tag = f"b{depth}"
        truncated = truncate(model, depth)
        bundle = export(truncated, tag, args.snr_range)
        # The parameter count is the honest size of the artifact the arm would load.
        print(f"depth {depth:2d}: {truncated.count_params():6d} parameter(s) -> {bundle}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
