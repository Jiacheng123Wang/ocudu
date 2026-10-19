#!/usr/bin/env python3
"""Add samples to an existing MIXED depth-3 dataset, and print the coverage it buys.

WHY IT EXISTS (design section 19.206). The training set is 98.8% 25+ PRB allocations: of
12 000 samples, 8 sit at 9-24 PRB, 18 at 5-8 PRB and 123 at <=4 PRB -- and every single one
of those narrow samples is Qm=8. The field's attach runs 83% of its grants at 5-8 PRB, which
is why the arm fails there while the classical chain does not. The fix is data, so the set
has to be extended along the ALLOCATION-WIDTH dimension, and the extension has to be visible
in the file that training reads.

`merge_depth3_datasets.py` cannot do this: it concatenates PER-MODULATION files and refuses
any input whose `qm` column holds more than one value, which is exactly what the mixed
(modulation-conditioned) training set is. This tool takes that mixed set as the BASE and adds
pure per-modulation files on top, with an explicit repetition factor per file -- because the
narrow corpora are small and the point of the exercise is that they stop being a rounding
error.

WHAT IT CHECKS, for the same reason the merge tool checks:
  * the same key set, and identical per-sample shapes for every key that is NOT a function
    of Qm (`bits` and `tb_bits` are, and are padded to the widest -- every reader slices by
    `nof_bits`/`nof_tb_bits`);
  * each ADDED file holds exactly one modulation (a pure file is what "add this cell" means);
  * the base still covers all four modulations after the addition.

WHAT IT PRINTS: the qm x width-band table before and after, so the coverage change is a
number in the log and not an intention.

Usage:
  augment_depth3_dataset.py --base d3_modcond_train.npz --out d3_modcond_w_train.npz \
      --add d3_narrow_64qam.npz --repeat 12 --add d3_narrow_qpsk.npz --repeat 8
"""

# The shared DSP utilities (dmrs, synth, labels, LDPC) still live in the AI-CE training tree; this is
# the single place that knows where, so the import below is not a hidden path dependency.
import depth3_paths
depth3_paths.add_ce_shared()
import argparse
import collections
import sys

import numpy as np

# Keys whose per-sample size is a function of the modulation (same list, same reason, as
# merge_depth3_datasets.py): a 64QAM coded-bit sequence is three times a QPSK one.
SCALES_WITH_QM = ("bits", "tb_bits")


def width_band(nof_prb):
    n = int(nof_prb)
    return "<=4" if n <= 4 else ("5-8" if n <= 8 else ("9-24" if n <= 24 else "25+"))


def rate_band(rate):
    r = float(rate)
    return "<0.70" if r < 0.70 else ("0.70-0.85" if r < 0.85 else (">=0.92" if r >= 0.92 else "0.85-0.92"))


def coverage(data):
    """The qm x width x code-rate table: what a sample of this set actually teaches."""
    qm = np.asarray(data["qm"]).tolist()
    prb = np.asarray(data["alloc_nof_prb"]).tolist()
    rate = np.asarray(data["r"]).tolist()
    table = collections.Counter()
    for q, n, rr in zip(qm, prb, rate):
        table[(int(q), width_band(n))] += 1
    return table, collections.Counter(int(q) for q in qm)


def print_coverage(title, data):
    table, per_qm = coverage(data)
    print(f"  {title}: {sum(per_qm.values())} samples, per Qm {dict(sorted(per_qm.items()))}")
    print(f"    {'qm':>3} {'<=4':>7} {'5-8':>7} {'9-24':>7} {'25+':>7}")
    for q in (2, 4, 6, 8):
        row = [table[(q, b)] for b in ("<=4", "5-8", "9-24", "25+")]
        print(f"    {q:>3} {row[0]:>7} {row[1]:>7} {row[2]:>7} {row[3]:>7}")
    narrow = sum(v for (_, b), v in table.items() if b != "25+")
    print(f"    narrow (<25 PRB) share: {100.0 * narrow / max(1, sum(table.values())):.1f}%")


def describe(path):
    data = np.load(path, allow_pickle=True)
    keys = sorted(data.keys())
    shapes = {key: (np.asarray(data[key]).shape[1:], str(data[key].dtype)) for key in keys}
    return data, keys, shapes


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True, help="the mixed dataset to extend")
    parser.add_argument("--out", required=True)
    parser.add_argument("--add", action="append", default=[], help="a PURE per-modulation dataset")
    parser.add_argument("--repeat", action="append", type=int, default=[],
                        help="repetition factor for the matching --add (same order)")
    parser.add_argument("--seed", type=int, default=20261018)
    args = parser.parse_args()

    if len(args.add) != len(args.repeat):
        print("REFUSING: every --add needs one --repeat (they are paired by position)", file=sys.stderr)
        return 2

    base, base_keys, base_shapes = describe(args.base)
    for key in ("qm", "alloc_nof_prb", "r"):
        if key not in base_keys:
            print(f"REFUSING: {args.base} has no '{key}' column, so the coverage cannot be described "
                  f"(let alone extended)", file=sys.stderr)
            return 2
    print(f"base: {args.base}")
    print_coverage("before", base)

    pieces = []
    for path, repeat in zip(args.add, args.repeat):
        data, keys, shapes = describe(path)
        if keys != base_keys:
            print(f"REFUSING: {path} carries keys {keys}, the base carries {base_keys}", file=sys.stderr)
            return 2
        for key in keys:
            got, want = shapes[key], base_shapes[key]
            if got[1] != want[1]:
                print(f"REFUSING: {path} has {key} {got[1]}, the base has {want[1]}", file=sys.stderr)
                return 2
            if key not in SCALES_WITH_QM and got[0] != want[0]:
                print(f"REFUSING: {path} has {key} {got[0]}, the base has {want[0]} -- this key is not a "
                      f"function of Qm, so the two files do not describe the same tensor", file=sys.stderr)
                return 2
        qms = sorted(set(int(v) for v in np.asarray(data["qm"]).tolist()))
        if len(qms) != 1:
            print(f"REFUSING: {path} mixes {qms}; an addition names ONE cell, so it must be pure",
                  file=sys.stderr)
            return 2
        if repeat < 1:
            print(f"REFUSING: --repeat {repeat} for {path}", file=sys.stderr)
            return 2
        order = np.tile(np.arange(len(data["qm"])), repeat)
        pieces.append((path, data, keys, order, qms[0], repeat))

    widths = {}
    for _, data, keys, _, _, _ in [(None, base, base_keys, None, None, None)] + pieces:
        for key in keys:
            shape = np.asarray(data[key]).shape
            widths[key] = max(widths.get(key, 0), shape[1] if len(shape) > 1 else 0)

    merged = collections.OrderedDict()
    sequence = [(args.base, base, base_keys, np.arange(len(base["qm"])), None, 1)] + pieces
    for path, data, keys, order, _, _ in sequence:
        for key in keys:
            piece = np.asarray(data[key][order])
            if len(piece.shape) > 1 and piece.shape[1] != widths[key]:
                padded = np.zeros((piece.shape[0], widths[key]) + piece.shape[2:], dtype=piece.dtype)
                padded[:, :piece.shape[1]] = piece
                piece = padded
            merged[key] = piece if key not in merged else np.concatenate([merged[key], piece], axis=0)

    counts = collections.Counter(int(v) for v in merged["qm"].tolist())
    missing = [q for q in (2, 4, 6, 8) if q not in counts]
    if missing:
        print(f"REFUSING: the result has no Qm={missing} sample; a conditioned set must cover all four",
              file=sys.stderr)
        return 2

    np.savez_compressed(args.out, **merged)
    print(f"\n{args.out}: {sum(counts.values())} samples")
    for path, _, _, order, qm_, repeat in sequence[1:]:
        print(f"  Qm={qm_} from {path}: {order.size // repeat} x {repeat} = {order.size}")
    print_coverage("after", merged)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
