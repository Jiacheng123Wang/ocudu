#!/usr/bin/env python3
"""Measure the Layer-3 two-graph early-exit cascade on the same replay binary as the ruler.

WHAT IT ANSWERS. The Fast graph (K=2) and the Deep graph (K=6) are the SAME trained weights unrolled a different
number of times (train_depth3.py --export-fast-loops). The deployed cascade is:

    run Fast -> if its LDPC decode passes CRC, that is the answer
             -> otherwise run Deep and use its LLRs

so the fraction of receptions that NEVER need the deep graph, and the delivered bits of the union, are measurable
without touching the receiver: both arms are the project's normal `ul_chain_replay`, and CRC is the judge. This is the
"check the first LDPC decode" form of the gate. The mean-|LLR| threshold is a refinement that can skip the decoder on
easy receptions; it needs the LLR buffer and is measured separately.

WHY THE UNION IS THE RIGHT NUMBER. The cascade cannot lose bits relative to Deep: a reception Deep would have decoded
is decoded by Deep whenever Fast failed. Its gain over Deep is the Fast-only successes (Fast OK, Deep KO), which is
exactly the part of the trajectory that the K=2 graph gets right and K=6 does not. The cost is the deep pass on the
Fast-KO fraction; `fast_fraction` is what the 65% target is about.

Usage:
  early_exit_measure.py --units <units.txt> --fast <fast.mlmodelc> --deep <deep.mlmodelc> \
      --corpus <corpus_dir> [--out results.csv] [--limit N] [--ldpc generic]
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import time

BIN = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")


def replay(model, capture, out, ldpc):
    env = dict(os.environ)
    env["OCUDU_DEPTH3_FALLBACK"] = "none"      # PURE: a refusal must be visible, not silently replaced
    env["OCUDU_REPLAY_MODEL"] = model
    proc = subprocess.run([BIN, capture, "--out", out, "--phy_pipeline", "npu",
                           "--pusch_receiver_backend", "ai", "--pusch_ldpc_decoder_type", ldpc],
                          capture_output=True, text=True, env=env)
    ok = "crc=OK" in proc.stdout
    tbs = re.search(r"tbs=(\d+)", proc.stdout)
    its = re.search(r"iterations=(\d+)", proc.stdout)
    return {"ok": ok, "bits": (int(tbs.group(1)) * 8 if (ok and tbs) else 0),
            "iterations": (int(its.group(1)) if its else 0),
            "stdout": proc.stdout, "stderr": proc.stderr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--units", required=True)
    ap.add_argument("--fast", required=True)
    ap.add_argument("--deep", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--ldpc", default="generic")
    args = ap.parse_args()

    units = [line.strip() for line in open(args.units) if line.strip()]
    if args.limit:
        units = units[:args.limit]
    rows = []
    started = time.time()
    for index, unit in enumerate(units, 1):
        capture = os.path.join(args.corpus, unit)
        fast = replay(args.fast, capture, "/tmp/_ee_fast", args.ldpc)
        deep = None
        if not fast["ok"]:
            deep = replay(args.deep, capture, "/tmp/_ee_deep", args.ldpc)
        rows.append({"unit": unit, "fast_ok": int(fast["ok"]), "fast_bits": fast["bits"],
                     "fast_iter": fast["iterations"],
                     "deep_ok": ("" if deep is None else int(deep["ok"])),
                     "deep_bits": ("" if deep is None else deep["bits"]),
                     "deep_iter": ("" if deep is None else deep["iterations"])})
        if index % 50 == 0:
            print(f"{index}/{len(units)} units, {time.time() - started:.0f}s", flush=True)

    fast_ok = sum(r["fast_ok"] for r in rows)
    fast_bits = sum(r["fast_bits"] for r in rows)
    deep_ran = [r for r in rows if r["deep_ok"] != ""]
    deep_bits = sum(r["deep_bits"] for r in deep_ran)
    cascade_bits = fast_bits + deep_bits
    # Deep-only: for units that stayed on Fast, the deep result is unknown exactly (not run). The honest lower bound
    # of the cascade's gain over Deep is the Fast-only successes; the upper bound assumes those units were Deep-KO.
    fast_only_bits = fast_bits
    print(f"units {len(rows)}  fast OK {fast_ok} ({100.0 * fast_ok / max(1, len(rows)):.1f}%)  "
          f"deep runs {len(deep_ran)}  fast bits {fast_bits}  deep rescue bits {deep_bits}  "
          f"cascade bits {cascade_bits}  (fast-only ceiling {fast_only_bits})")
    if args.out:
        with open(args.out, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else ["unit"])
            writer.writeheader()
            writer.writerows(rows)
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
