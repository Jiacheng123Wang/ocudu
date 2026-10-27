#!/usr/bin/env python3
"""Choose which units to build, so that a bin that is already full stops growing while rare bins are still scanned.

WHY NOT A PER-LEG CAP. A cap counts samples, not coverage: a hundred-thousand-capture leg is mostly the same cell, and a
cap spends its budget on that cell while the rare ones stay empty. This selects by BIN instead, against a state file that
is shared across legs, so the first legs fill the common bins and later legs spend their whole scan on whatever is still
short -- which is the only way a rare cell accumulates anything when it appears once per thousand receptions.

BIN = modulation x width band x SNR band, from the PDU (modulation, PRB count) and the dump (SINR). The bands match
split_depth3_stratified.py, so what is filled here is what the split can then measure.

Usage:
  select_by_bin.py --legs <file with one leg name per line> --corpus DIR --tb-root DIR --state FILE
                   [--quota-self 500] [--quota-harq 250] [--seed 20261027]
Writes <tb-root>/<leg>/index_sel_self.csv and index_sel_harq.csv, and updates the state file.
"""
import argparse
import collections
import csv
import json
import os
import random

MODS = {2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM"}
WIDTH_EDGES = [3, 8, 24]
SNR_EDGES = [0.0, 6.0, 10.0, 14.0, 20.0, 30.0]


def width_band(prb):
    previous = None
    for edge in WIDTH_EDGES:
        if prb <= edge:
            return f"<={edge}"
        previous = edge
    return f">{WIDTH_EDGES[-1]}"


def snr_band(snr):
    previous = None
    for edge in SNR_EDGES:
        if snr < edge:
            return f"<{edge:g}" if previous is None else f"{previous:g}-{edge:g}"
        previous = edge
    return f">={SNR_EDGES[-1]:g}"


def pdu_modulation_and_prb(path):
    mod, prb = None, None
    try:
        with open(path, errors="ignore") as handle:
            for line in handle:
                key, sep, value = line.strip().partition("=")
                if not sep:
                    continue
                if key == "modulation":
                    mod = value
                elif key == "alloc_nof_rb":
                    try:
                        prb = int(value)
                    except ValueError:
                        prb = None
    except OSError:
        return None, None
    return mod, prb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--legs", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--tb-root", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--quota-self", type=int, default=500)
    ap.add_argument("--quota-harq", type=int, default=250)
    ap.add_argument("--seed", type=int, default=20261027)
    args = ap.parse_args()

    state = json.load(open(args.state)) if os.path.exists(args.state) else {}
    rng = random.Random(args.seed)
    legs = [l.strip() for l in open(args.legs) if l.strip()]

    for leg in legs:
        tb = os.path.join(args.tb_root, leg)
        corpus = os.path.join(args.corpus, leg)
        if not os.path.isdir(tb):
            print(f"{leg}: no dump, skipped")
            continue
        # Candidates per population, then per bin, shuffled so the choice is not the first N in time order.
        picked = {"self": collections.defaultdict(list), "harq": collections.defaultdict(list)}
        for pop, fname in (("self", "index_self.csv"), ("harq", "index_harq_only.csv")):
            path = os.path.join(tb, fname)
            if not os.path.exists(path):
                continue
            for row in csv.DictReader(open(path)):
                mod, prb = pdu_modulation_and_prb(os.path.join(corpus, row["unit"] + ".txt"))
                if mod is None or prb is None:
                    continue
                try:
                    snr = float(row.get("sinr_db") or "nan")
                except ValueError:
                    continue
                if snr != snr:      # NaN: the dump could not estimate one
                    continue
                key = f"{mod}|{width_band(prb)}|{snr_band(snr)}"
                picked[pop][key].append(row)
        for pop in ("self", "harq"):
            quota = args.quota_self if pop == "self" else args.quota_harq
            rows = []
            for key, members in picked[pop].items():
                have = state.get(f"{pop}:{key}", 0)
                room = max(0, quota - have)
                if room == 0:
                    continue
                rng.shuffle(members)
                take = members[:room]
                state[f"{pop}:{key}"] = have + len(take)
                rows.extend(take)
            if not rows:
                continue
            out = os.path.join(tb, f"index_sel_{pop}.csv")
            with open(out, "w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
                writer.writeheader()
                writer.writerows(rows)
            print(f"{leg} {pop}: selected {len(rows)} of {sum(len(v) for v in picked[pop].values())}")
        json.dump(state, open(args.state, "w"), indent=1)
    print("state bins:", len(state))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
