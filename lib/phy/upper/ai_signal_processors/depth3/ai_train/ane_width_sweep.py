"""Latency vs trunk width on the ANE, measured on UNTRAINED weights on purpose.

WHY THIS BELONGS IN THE TREE. It is the measurement that changed the latency conclusion: on 2026-11-06 it showed the same
architecture costing 420 us at c_hid 32, 1024 at 64, 1312 at 96 and 2824 at 128, with the widest step between 96 and 128 --
so the cost is not arithmetic (the square law would predict 4x, 9x and 16x) but the volume each operator streams. A dense
3x3 control multiplies the FLOPs by 5.7 and leaves the latency unchanged, which is the same finding from the other side.

Weights do not enter the measurement -- latency depends on the graph and the tensor shapes, not on what the weights hold --
so a random-weight build answers the question without waiting for a training run. That is the whole reason this script is
cheap enough to be run whenever a width is being chosen.

The trained bundles answer this only for the widths we happened to train (32 and 128). Latency does not depend on what the
weights are, so a random-weight graph of the same architecture measures the same engine behaviour -- and that is the
question: does the cost grow linearly with width, or does it step at some tile boundary?

usage: ane_width_sweep.py 32 64 96 128      (prints one .mlpackage per width)
"""
import sys, numpy as np
import tensorflow as tf
import train_depth3 as T

for c in [int(x) for x in sys.argv[1:]]:
    model = T.build(blocks=18, c_hid=c, cond=True, layout="spatial", reporting_heads=False, in_channels=16)
    path = f"/tmp/w{c}.mlpackage"
    T.export_deployment(model, qm=2, path=path, cond=True, snr_range=(-0.18, 34.70))
    print(f"exported c{c} -> {path}  (compile it and point coreml_latency_probe at the .mlmodelc)", flush=True)
