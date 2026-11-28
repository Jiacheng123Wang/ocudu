#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Measure what a narrow-band bucket does to the full-band model's output.

WHY THIS EXISTS. A bucket is a second Core ML graph built at a narrower input width, and the receiver feeds it a CROP of
the plane it assembles (`pusch_depth3_receiver_impl.cpp`: `crop_lo`, `plane_nsc`). Whether that is the same function or
merely a similar one is a property of the ARCHITECTURE, not of the export:

  * a fully-local trunk (conv/depthwise, ReLU, Add) is translation-equivariant, so a crop reproduces the interior
    outputs to fp16 rounding -- the four-PRB margin exists only to keep the boundary out of the compared region;
  * a trunk with SE is NOT: the squeeze averages the whole input window, so a 108-wide crop hands every block a
    different gate from the 612-wide one -- everywhere, not just at the boundary, and no margin can fix a global
    reduction.

So the number this tool prints is the field difference between "the model that was measured on the ruler" and "the model
that would fly", and the only honest way to report the latter is with this number beside it.

Usage:
  bucket_equiv.py <full.mlmodelc|.mlpackage> <bucket.mlmodelc|.mlpackage> [--lo N] [--margin 48] [--seed 1]
                  [--check] [--max-rms F]

  --lo        first subcarrier of the crop in the full plane (default: centred)
  --margin    subcarriers excluded at each end of the crop (default 48 = four PRB of context)
  --check     exit non-zero when the LLR RMS exceeds --max-rms (in quantiser LSB at s_llr = 20)

The two models must hold the same weights: this measures the crop, and a bucket that was not copied from the source
would make the number look like a crop effect when it is a different network (which is exactly the failure of
2026-11-28 -- see train_depth3.trunk_flags_from_source).
"""
import argparse
import os
import sys

import numpy as np

S_LLR = 20.0          # the deployed sidecar's main-head scale (depth3_scales.txt)
NOF_SYMBOLS = 14
COND_CHANNELS = 4


def syn_plane(width, channels, seed=1):
    """A deterministic, plausible plane: noise-like grid, a DM-RS comb, an allocation window, LS pilots.

    The SE question does not depend on the signal being realistic, but a plane of i.i.d. noise makes the squeeze's
    average unusually well behaved; the pilot and allocation structure is what a real reception carries, so it is
    reproduced here rather than left out.
    """
    rng = np.random.default_rng(seed)
    plane = np.zeros((1, width, NOF_SYMBOLS, channels), dtype=np.float32)
    plane[0, :, :, 0] = rng.standard_normal((width, NOF_SYMBOLS)).astype(np.float32)
    plane[0, :, :, 1] = rng.standard_normal((width, NOF_SYMBOLS)).astype(np.float32)
    dmrs_symbols = (2, 7, 11)
    for symbol in dmrs_symbols:
        plane[0, ::2, symbol, 2] = 1.0                          # DM-RS type 1: every second subcarrier
        plane[0, ::2, symbol, 5] = plane[0, ::2, symbol, 0]     # LS re  = grid re on the pilots
        plane[0, ::2, symbol, 6] = plane[0, ::2, symbol, 1]     # LS im  = grid im on the pilots
    first = width // 8
    last = width - width // 8
    plane[0, first:last, :, 3] = 1.0                            # allocation
    plane[0, :, :, 7] = 1.0                                     # one TX layer
    plane[0, :, :, 8] = 1.0                                     # one CDM group
    plane[0, :, :, 9] = 1.0                                     # DM-RS type 1
    plane[0, :, :, 10] = float(len(dmrs_symbols))
    plane[0, :, :, 11] = 1.0                                    # scaling
    if channels >= 12 + COND_CHANNELS:
        plane[0, :, :, 12 + 3] = 1.0                            # Qm = 8 one-hot
    return plane


AI_TRAIN = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "..", "..", "..", "lib", "phy", "upper", "ai_signal_processors", "depth3", "ai_train")
AI_TRAIN = os.path.normpath(AI_TRAIN)


def real_planes(dataset, indices):
    """The planes the trainer itself would have fed this model, taken from a monitor dataset.

    WHY NOT SYNTHETIC ONLY. The squeeze averages the window, so the size of the crop effect depends on how stationary
    the plane's statistics are -- and a synthetic i.i.d. plane is the most stationary thing there is. A real reception
    carries a narrow allocation, a pilot comb and a faded grid, which is exactly where a global average over 108
    subcarriers can differ from the average over 612. This loader reproduces `Batches.sample`'s layout conversion
    (`respaciate` plus the four conditioning channels) rather than re-deriving a plane, so the input is the model's.
    """
    sys.path.insert(0, AI_TRAIN)
    import depth3_paths                                   # noqa: E402 - the shared DSP tree, resolved once
    depth3_paths.add_ce_shared()
    import depth3 as D3                                   # noqa: E402 - needs the path above
    first_prb = np.load(os.path.join(dataset, "alloc_first_prb.npy"), mmap_mode="r")
    nof_prb = np.load(os.path.join(dataset, "alloc_nof_prb.npy"), mmap_mode="r")
    planes = []
    for index in indices:
        folded = np.asarray(np.load(os.path.join(dataset, "X.npy"), mmap_mode="r")[index], dtype=np.float32)
        spatial = D3.respaciate(folded)
        qm = int(np.load(os.path.join(dataset, "qm.npy"), mmap_mode="r")[index])
        one_hot = D3.qm_one_hot(qm)
        spatial = np.concatenate([spatial, np.broadcast_to(one_hot, spatial.shape[:-1] + (one_hot.size,))], axis=-1)
        planes.append({"tag": f"#{index}", "qm": qm, "plane": spatial[None, ...],
                       "first_prb": int(first_prb[index]), "nof_prb": int(nof_prb[index])})
    return planes


def load(path):
    """Either artifact form: a `.mlpackage` (as exported) or a compiled `.mlmodelc` (as a bundle carries it).

    `MLModel` only reads a compiled directory that Xcode produced (it looks for `Manifest.json`); the `coremlcompiler`
    output the bundle holds has `coremldata.bin` instead, and `CompiledMLModel` is the class that takes it.
    """
    import coremltools as ct
    if path.rstrip("/").endswith(".mlmodelc"):
        return ct.models.CompiledMLModel(path, compute_units=ct.ComputeUnit.CPU_ONLY)
    return ct.models.MLModel(path, compute_units=ct.ComputeUnit.CPU_ONLY)


def shape_of(model, path):
    """The declared input shape, from the spec (`.mlpackage`) or from the compiled bundle's `metadata.json`.

    `CompiledMLModel` exposes no spec at all, and `coremldata.bin` is not the Model protobuf, so the compiled form's
    own declaration is read from `metadata.json` -- which `coremlcompiler` writes next to it and the bundle already
    carries.
    """
    if hasattr(model, "get_spec"):
        return [int(d) for d in model.get_spec().description.input[0].type.multiArrayType.shape]
    import json
    import os
    with open(os.path.join(path, "metadata.json"), encoding="utf-8") as handle:
        metadata = json.load(handle)
    entry = {item["name"]: item for item in metadata[0]["inputSchema"]}
    return [int(d) for d in json.loads(entry["x"]["shape"])]


def compare(full, out_full, bucket_out, lo, lo_i, hi_i):
    """Per-head statistics over the compared interior of one reception."""
    stats = {}
    for name in ("llr", "post_eq_noise"):
        if (name not in out_full) or (name not in bucket_out):
            continue
        a = np.asarray(out_full[name], dtype=np.float32)[0, lo + lo_i:lo + hi_i]
        b = np.asarray(bucket_out[name], dtype=np.float32)[0, lo_i:hi_i]
        if a.shape != b.shape:
            raise SystemExit(f"{name}: shapes disagree after the crop ({a.shape} against {b.shape})")
        diff = np.abs(a - b)
        rms = float(np.sqrt(np.mean(diff ** 2)))
        stats[name] = {
            "max": float(diff.max()),
            "rms": rms,
            # THE QUANTISER IS `tanh(...) * s_llr` THEN A ROUND, so a difference in the head's units becomes `s_llr`
            # times as many LSB. Dividing (the first version) understated it by s_llr squared.
            "lsb": rms * S_LLR if name == "llr" else rms,
            "flips": float(np.mean(np.signbit(a) != np.signbit(b))) if name == "llr" else 0.0,
            "n": int(diff.size),
        }
    return stats


def run(model, plane):
    """One prediction; Core ML wants fp16 for an fp16 graph and the plane's layout as (1, W, T, C)."""
    return model.predict({"x": np.ascontiguousarray(plane, dtype=np.float16)})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("full")
    parser.add_argument("bucket")
    parser.add_argument("--lo", type=int, default=None, help="first subcarrier of the crop (default: around the allocation)")
    parser.add_argument("--margin", type=int, default=48, help="subcarriers excluded at each end (default 48 = 4 PRB)")
    parser.add_argument("--seed", type=int, default=1, help="seed of the synthetic plane when no --dataset is given")
    parser.add_argument("--dataset", default="", help="a monitor dataset directory (X.npy, qm.npy, allocations)")
    parser.add_argument("--index", default="", help="comma-separated sample indices (default: the first --count)")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--check", action="store_true", help="exit non-zero when the worst LLR RMS exceeds --max-rms")
    parser.add_argument("--max-rms", type=float, default=1.0, help="worst acceptable LLR RMS in quantiser LSB")
    args = parser.parse_args()

    full = load(args.full)
    bucket = load(args.bucket)
    full_shape = shape_of(full, args.full)
    bucket_shape = shape_of(bucket, args.bucket)
    width_full, width_bucket, channels = full_shape[1], bucket_shape[1], full_shape[3]
    if bucket_shape[3] != channels:
        raise SystemExit(f"the two graphs take different channel counts ({channels} against {bucket_shape[3]})")
    lo_i, hi_i = args.margin, width_bucket - args.margin
    if hi_i <= lo_i:
        raise SystemExit(f"margin {args.margin} leaves nothing of a {width_bucket}-wide bucket")

    if args.dataset:
        indices = ([int(v) for v in args.index.split(",") if v.strip()] if args.index
                   else list(range(args.count)))
        samples = real_planes(args.dataset, indices)
    else:
        samples = [{"tag": "synthetic", "qm": 0, "plane": syn_plane(width_full, channels, seed=args.seed),
                    "first_prb": None, "nof_prb": None}]

    print(f"full x=[1,{width_full},{NOF_SYMBOLS},{channels}]  bucket x=[1,{width_bucket},{NOF_SYMBOLS},{channels}]  "
          f"interior [{lo_i},{hi_i})  scale s_llr={S_LLR:g}")
    print(f"{'sample':10s} {'qm':>2s} {'crop':>11s} {'llr max|d|':>11s} {'llr LSB rms':>12s} {'sign-flip':>10s} "
          f"{'noise max|d|':>13s}")
    worst = 0.0
    for sample in samples:
        plane = sample["plane"]
        # THE CROP IS THE RUNTIME'S, not an arbitrary one: `pusch_depth3_receiver_impl.cpp` places the window around
        # the allocation with four PRB of context on each side, clipped to the plane. Using the same rule is what makes
        # this number comparable to a reception, instead of to a centred window no reception would produce.
        if args.lo is not None:
            lo = args.lo
        elif sample["first_prb"] is not None:
            lo = max(0, sample["first_prb"] * 12 - 48)
            lo = min(lo, width_full - width_bucket)
            need_last = min(width_full, (sample["first_prb"] + sample["nof_prb"]) * 12 + 48)
            lo = max(lo, need_last - width_bucket)
            lo = max(0, min(lo, width_full - width_bucket))
        else:
            lo = max(0, (width_full - width_bucket) // 2)
        if (lo + width_bucket) > width_full:
            lo = width_full - width_bucket

        out_full = run(full, plane)
        out_bucket = run(bucket, plane[:, lo:lo + width_bucket, :, :])
        stats = compare(full, out_full, out_bucket, lo, lo_i, hi_i)
        llr = stats.get("llr", {})
        noise = stats.get("post_eq_noise", {})
        worst = max(worst, llr.get("lsb", 0.0))
        print(f"{sample['tag']:10s} {sample['qm']:2d} [{lo:4d},{lo + width_bucket:4d}) {llr.get('max', 0.0):11.4f} "
              f"{llr.get('lsb', 0.0):12.3f} {llr.get('flips', 0.0) * 100:9.2f}% {noise.get('max', 0.0):13.4f}")

    print(f"worst LLR RMS: {worst:.3f} LSB (threshold {args.max_rms:g}); "
          f"1 LSB is one quantiser step of the 8-bit LLR")
    if args.check and (worst > args.max_rms):
        print("FAIL: the bucket is not the full-band model's function on the compared interior", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
