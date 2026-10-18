#!/usr/bin/env python3
"""Bit agreement with the classical chain, on receptions the classical chain DECODED.

WHY IT EXISTS (design section 19.206). The arm's failure is not a confidence problem: on the
receptions it loses, 16-21% of its soft-bit SIGNS are the opposite of the classical chain's, and
the classical chain's signs are the transmitted bits because its CRC passed. A number that
measures exactly that is therefore the cheapest capability signal this workstream has -- it
needs no ground truth, no synthesis and no radio, only a capture and the two arms.

What it is: for each selected reception, run the classical arm and the arm under test over the
same grid, each writing its soft bits, and count the bits whose signs differ. Reported per
reception and pooled, with the median |LLR| of each arm so a difference caused by saturation is
visible next to one caused by wrong decisions.

Selection (`--ab`, the CSV attach_ab.py writes): first transmissions only (a retransmission
replayed one-shot meets an empty soft buffer, so its verdict says nothing), and by default only
the receptions the CLASSICAL chain decoded -- those are the ones whose signs are known to be the
transmitted bits.

Usage:
  bit_agreement.py --ab work_tmp/cmp/ab_ai22_ft.csv --model .../d3_modcond_ft.mlmodelc \
      [--cell 64QAM:5-8] [--limit 20] [--min-agreement 99.5]
"""

import argparse
import csv
import glob
import os
import shutil
import subprocess
import sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
REPLAY = os.path.join(ROOT, "build/lib/phy/upper/channel_processors/metal/ul_chain_replay")


def width_band(nof_prb):
    n = int(nof_prb)
    return "<=4" if n <= 4 else ("5-8" if n <= 8 else ("9-24" if n <= 24 else "25+"))


def run(capture, receiver, model, out, staged):
    env = dict(os.environ, OCUDU_REPLAY_RECEIVER=receiver)
    if model:
        env["OCUDU_REPLAY_MODEL"] = model
    for leftover in glob.glob(os.path.join(staged, "*")):
        if os.path.isfile(leftover):
            os.remove(leftover)
    proc = subprocess.run([REPLAY, capture, "--metal", "--out", out], env=env,
                          capture_output=True, text=True, check=False)
    text = proc.stdout + proc.stderr
    found = glob.glob(os.path.join(staged, "*_llr.bin"))
    if not found:
        return None, "no soft bits written"
    # A DEGRADED reception's soft bits are the CLASSICAL chain's, so comparing them with the
    # classical chain's would report a perfect 100% agreement for a forward pass that never ran
    # (measured 2026-10-18 19:35: the Core ML forward failed while a training job held the machine,
    # and six receptions came back "100% agreement" with identical, saturated LLRs). It is an
    # error, not a data point.
    if "DEGRADED to the classical chain" in text:
        reason = text.split("DEGRADED to the classical chain:")[1].split("(")[0].strip()
        return None, f"arm did not compute: {reason}"
    return np.fromfile(found[0], dtype=np.int16).astype(np.int64), ""


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ab", required=True, help="attach_ab.py CSV naming the captures")
    parser.add_argument("--captures", default="", help="capture directory (default: derived from the CSV)")
    parser.add_argument("--model", required=True)
    parser.add_argument("--cell", default="", help="restrict to one cell, e.g. 64QAM:5-8 or 64QAM:5-8:>=0.92")
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--include-arm-ok", action="store_true",
                        help="also include receptions the arm decoded (default: only its losses)")
    parser.add_argument("--staged", default="/tmp/bit_agreement")
    parser.add_argument("--min-agreement", type=float, default=99.5)
    args = parser.parse_args()

    captures = args.captures
    if not captures:
        guess = os.path.join(os.path.dirname(os.path.abspath(args.ab)), "..", "corpus", "attach",
                             os.path.basename(args.ab).replace("ab_", "", 1).split("_")[0])
        captures = os.path.abspath(guess)
    if not os.path.isdir(captures):
        raise SystemExit(f"--captures {captures} is not a directory (pass it explicitly)")

    rows = []
    for row in csv.DictReader(open(args.ab)):
        if not row.get("crc_ai") or row.get("new_data") != "1":
            continue
        if row["crc_classic"] != "OK":
            continue
        if not args.include_arm_ok and row["crc_ai"] == "OK":
            continue
        if args.cell:
            parts = args.cell.split(":")
            if row["mod"] != parts[0] or width_band(row["nof_prb"]) != parts[1]:
                continue
            if len(parts) > 2:
                rate = float(row["code_rate"]) / 1024.0
                lo, _, hi = parts[2].partition("--")
                if parts[2].startswith(">=") and rate < float(parts[2][2:]):
                    continue
                if parts[2].startswith("<") and rate >= float(parts[2][1:].lstrip("=")):
                    continue
                if lo and hi and not (float(lo) <= rate < float(hi)):
                    continue
        rows.append(row)
    rows = rows[:args.limit]
    if not rows:
        raise SystemExit("no reception matched the selection")

    staged = os.path.abspath(args.staged)
    os.makedirs(staged, exist_ok=True)
    print(f"{len(rows)} reception(s) the classical chain decoded, from {args.ab}")
    print(f"  {'capture':>16} {'mod':>6} {'w':>4} {'rate':>5} {'disagree':>9} {'|LLR| p50 c/a':>18}")
    agree_all = []
    skipped = 0
    for row in rows:
        capture = os.path.join(captures, row["stem"])
        classic, why_c = run(capture, "classic", "", os.path.join(staged, "c"), staged)
        arm, why_a = run(capture, "ai", args.model, os.path.join(staged, "a"), staged)
        if classic is None or arm is None:
            print(f"  {row['stem']:>16}: SKIPPED - {why_c or why_a}", file=sys.stderr)
            skipped += 1
            continue
        n = min(classic.size, arm.size)
        classic, arm = classic[:n], arm[:n]
        nonzero = (classic != 0) & (arm != 0)
        disagree = 100.0 * float(np.mean(np.sign(classic[nonzero]) != np.sign(arm[nonzero])))
        pool = np.median(np.abs(classic[nonzero])) if nonzero.any() else float("nan")
        arm_pool = np.median(np.abs(arm[nonzero])) if nonzero.any() else float("nan")
        agree_all.append(100.0 - disagree)
        print(f"  {row['stem']:>16} {row['mod']:>6} {width_band(row['nof_prb']):>4} "
              f"{int(row['code_rate']) / 1024.0:>5.2f} {disagree:>8.1f}% {pool:>8.0f}/{arm_pool:<8.0f}")
    shutil.rmtree(staged, ignore_errors=True)
    if skipped:
        print(f"  {skipped} reception(s) skipped: the arm did not compute them (see stderr). A run in which the "
              f"arm degrades for most receptions measures NOTHING -- do not run training or other GPU work "
              f"alongside this tool.", file=sys.stderr)
    if not agree_all:
        print("REFUSING: no reception had an AI result to compare", file=sys.stderr)
        return 2
    pooled = float(np.mean(agree_all))
    print(f"\n  mean bit agreement {pooled:.2f}%  (worst {min(agree_all):.2f}%, "
          f"n={len(agree_all)}); target >= {args.min_agreement}%")
    return 0 if pooled >= args.min_agreement else 1


if __name__ == "__main__":
    sys.exit(main())
