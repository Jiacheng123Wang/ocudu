#!/usr/bin/env python3
"""Merge per-modulation depth-3 datasets into ONE mixed set, for the modulation-conditioned model.

WHY IT EXISTS (design section 19.102). The conditioned model serves every modulation from one bundle: the Qm is
an input (four one-hot channels appended at sample time by `train_depth3.py --modcond`) and the loss masks each
sample to its own channels. What it needs is a dataset whose samples carry DIFFERENT modulations -- the `qm`
column has always been per sample, so this is a concatenation and not a rebuild.

WHAT IT CHECKS, LOUDLY, because a merge that misaligns silently poisons training and looks like a model that
"does not learn":
  * every input file carries the SAME key set and the same per-sample shapes -- a plane packed by a different
    dataset builder would concatenate without complaining and train on two conventions at once;
  * every file's `qm` column holds exactly one modulation, and each of the four appears at least once after the
    merge: a "mixed" set that lost a modulation is a per-modulation model wearing the conditioned contract's
    name, which is exactly the confusion this workstream keeps paying for;
  * the balance is printed, and `--per-modulation` caps each file so the merged set is balanced rather than
    dominated by whichever corpus was collected last.

Usage:
  merge_depth3_datasets.py --out <mixed_train.npz> [--per-modulation N] <a.npz> <b.npz> ...
"""
# The shared DSP utilities (dmrs, synth, labels, LDPC) still live in the AI-CE training tree; this is
# the single place that knows where, so the import below is not a hidden path dependency.
import depth3_paths
depth3_paths.add_ce_shared()
import argparse
import collections
import sys

import numpy as np


def describe(path):
    data = np.load(path, allow_pickle=True)
    keys = sorted(data.keys())
    qm = data["qm"] if "qm" in keys else None
    shapes = {}
    for key in keys:
        arr = data[key]
        shapes[key] = (arr.shape[1:], str(arr.dtype))
    return data, keys, qm, shapes


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    parser.add_argument("--per-modulation", type=int, default=0,
                        help="cap each input file at this many samples, so the merged set is balanced")
    parser.add_argument("--seed", type=int, default=20261016)
    parser.add_argument("inputs", nargs="+")
    args = parser.parse_args()

    rng = np.random.default_rng(args.seed)
    reference = None
    pieces = []
    for path in args.inputs:
        data, keys, qm, shapes = describe(path)
        if reference is None:
            reference = (path, keys, shapes)
        else:
            if keys != reference[1]:
                print(f"REFUSING: {path} carries keys {keys}, {reference[0]} carries {reference[1]}", file=sys.stderr)
                return 2
            for key in keys:
                got, want = shapes[key], reference[2][key]
                # ONLY the keys whose per-sample size is a function of Qm may differ: the coded bit sequence of a
                # 16QAM reception is twice a QPSK one. They are named here rather than inferred, because a check
                # that accepts "any shape that differs" also accepts two different PLANE PACKINGS -- and
                # concatenating a 12-channel plane with a 64-channel one is silent (measured 2026-10-16: this
                # guard's first version compared rank and dtype only, and a 12-vs-16 channel mismatch would have
                # passed it). Everything else must match exactly.
                scales_with_qm = key in ("bits", "tb_bits")
                if got[1] != want[1]:
                    print(f"REFUSING: {path} has {key} {got[1]}, {reference[0]} has {want[1]}", file=sys.stderr)
                    return 2
                if not scales_with_qm and got[0] != want[0]:
                    print(f"REFUSING: {path} has {key} {got[0]}, {reference[0]} has {want[0]} -- this key is not "
                          f"a function of Qm, so the two files do not describe the same tensor", file=sys.stderr)
                    return 2
                if scales_with_qm and len(got[0]) != len(want[0]):
                    print(f"REFUSING: {path} has {key} rank {len(got[0])}, {reference[0]} has {len(want[0])}",
                          file=sys.stderr)
                    return 2
        if qm is None:
            print(f"REFUSING: {path} has no 'qm' column, so the samples cannot be masked per modulation",
                  file=sys.stderr)
            return 2
        modulations = sorted(set(int(v) for v in qm.tolist()))
        if len(modulations) != 1:
            print(f"REFUSING: {path} mixes {modulations} in one file; merge the pure ones instead",
                  file=sys.stderr)
            return 2

        order = np.arange(len(qm))
        if args.per_modulation:
            if args.per_modulation > order.size:
                print(f"REFUSING: --per-modulation {args.per_modulation} exceeds {path}'s {order.size} samples",
                      file=sys.stderr)
                return 2
            order = np.sort(rng.choice(order, size=args.per_modulation, replace=False))
        elif len(args.inputs) > 1:
            # Balance by default: cap every file at the SMALLEST one, so no modulation dominates merely because
            # its corpus was collected last.
            pass
        pieces.append((path, data, keys, order, modulations[0]))

    if len(pieces) > 1 and not args.per_modulation:
        smallest = min(order.size for _, _, _, order, _ in pieces)
        pieces = [(p, d, k, order[:smallest], qm_) for p, d, k, order, qm_ in pieces]

    # The widest per-sample size of every key, so a key that scales with Qm can be padded instead of concatenated
    # into a ragged array. Padding is safe because the datasets carry the length (`nof_bits`, `nof_tb_bits`) and
    # every reader slices by it; leaving it out would either fail the concatenation or, worse, silently misalign
    # the samples after the first short one.
    widths = {}
    for _, data, keys, _, _ in pieces:
        for key in keys:
            # Scalars (a per-sample number with no sample axis) have rank 1 here and need no width.
            shape = np.asarray(data[key]).shape
            widths[key] = max(widths.get(key, 0), shape[1] if len(shape) > 1 else 0)

    merged = collections.OrderedDict()
    n = sum(order.size for _, _, _, order, _ in pieces)
    for path, data, keys, order, qm_ in pieces:
        for key in keys:
            piece = np.asarray(data[key][order])
            if len(piece.shape) > 1 and piece.shape[1] != widths[key]:
                padded = np.zeros((piece.shape[0], widths[key]) + piece.shape[2:], dtype=piece.dtype)
                padded[:, :piece.shape[1]] = piece
                piece = padded
            merged[key] = piece if key not in merged else np.concatenate([merged[key], piece], axis=0)

    counts = collections.Counter(int(v) for v in merged["qm"].tolist())
    missing = [qm_ for qm_ in (2, 4, 6, 8) if qm_ not in counts]
    if missing:
        print(f"REFUSING: the merged set has no Qm={missing} sample: a conditioned set must cover all four",
              file=sys.stderr)
        return 2

    np.savez_compressed(args.out, **merged)
    print(f"{args.out}: {n} samples")
    for path, _, _, order, qm_ in pieces:
        print(f"  Qm={qm_} from {path}: {order.size}")
    print(f"  merged distribution: {dict(sorted(counts.items()))}")
    print(f"  keys: {len(merged)} (identical across the inputs, shapes checked per sample)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
