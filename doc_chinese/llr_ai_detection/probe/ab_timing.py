#!/usr/bin/env python3
"""Interleaved A/B of two replay binaries -- or of one binary under two settings -- on the same capture.

WHY INTERLEAVED, and why the mode column exists
    This machine is bimodal on the HOST-side segments of the depth-3 arm: the same binary and the same
    capture measured `deliver` p50 102-134 us on some runs and 507-809 us on others, with the device
    segment only 2.2x apart while the host loops were 8x (2026-10-17, design section 19.168). The slow
    state persists for several consecutive runs, so a "run A, rebuild, run B" comparison can put the two
    legs on opposite sides of the flip and attribute a scheduling state to the code. Every run therefore
    records `wall` and the arm's own `plane` p50 -- both host-side, both mode markers -- and the A/B runs
    the two variants ALTERNATELY (ABBA) with the same capture in one session, so drift cancels and any
    slow-mode sample is visible instead of being averaged in.

    `--drop-slow` discards runs whose plane p50 exceeds the fast-mode ceiling; it is OFF by default so the
    raw table always shows what was thrown away.

Usage:
    ab_timing.py --a <bin> --b <bin> [--label-a A --label-b B] [--rounds 5] [--repeat 700] \
        --model <bundle> --capture <base> [--drop-slow 100] [--env-a K=V,K2=V2] [--env-b K=V]

    The two variants can also be one binary under two environments (--env-a / --env-b), which is how the
    compute-unit tiers are compared: the same bundle, the same capture, the only difference being the tier the
    engine asked Core ML for.

The number that matters is the paired difference of `deliver` p50 (and its p99) within fast-mode rounds.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

TIMING = re.compile(
    r"\[depth3-timing\] (\d+) forward\(s\), microseconds: "
    r"plane p50 (\d+) p99 (\d+) max (\d+) \| "
    r"forward p50 (\d+) p99 (\d+) max (\d+) \| "
    r"deliver p50 (\d+) p99 (\d+) max (\d+) \|")


def parse_env(text):
    """Turns "K=V,K2=V2" into a dict; empty pairs are ignored."""
    out = {}
    for pair in (text or "").split(","):
        if "=" in pair:
            key, value = pair.split("=", 1)
            out[key.strip()] = value.strip()
    return out


def one_run(tool, capture, model, repeat, out_prefix, extra, extra_env):
    """Runs one process and returns the arm's LAST timing line plus the chain's verdict counts."""
    env = dict(os.environ)
    env["OCUDU_REPLAY_RECEIVER"] = "ai"
    env["OCUDU_REPLAY_MODEL"] = model
    env.update(extra_env)
    started = time.monotonic()
    result = subprocess.run(
        [tool, capture, "--metal", "--out", out_prefix, "--harq-chain", "--repeat", str(repeat)] + extra,
        capture_output=True, text=True, env=env)
    wall = time.monotonic() - started
    output = result.stdout + result.stderr
    lines = TIMING.findall(output)
    row = {"exit": result.returncode, "wall": wall, "ok": output.count("crc=OK"),
           "receptions": len(re.findall(r"^  \[0\] tbs=", output, re.M))}
    if not lines:
        return row
    # The LAST line is the warmest: the ring reports on forward 1 and every 500th, and each line carries the
    # count it was taken at, so a run shorter than one report interval has only the cold sample and must not
    # be compared at all.
    count, plane, plane99, plane_max, forward, forward99, forward_max, deliver, deliver99, deliver_max = (
        int(value) for value in lines[-1])
    row.update({"count": count, "plane": plane, "plane99": plane99,
                "forward": forward, "forward99": forward99, "forward_max": forward_max,
                "deliver": deliver, "deliver99": deliver99, "deliver_max": deliver_max})
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True)
    parser.add_argument("--b", required=True)
    parser.add_argument("--label-a", default="A")
    parser.add_argument("--label-b", default="B")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--repeat", type=int, default=700)
    parser.add_argument("--model", required=True)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--work", default="/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/tmp/soak")
    parser.add_argument("--drop-slow", type=int, default=0,
                        help="discard runs whose plane p50 exceeds this many microseconds (fast-mode ceiling)")
    parser.add_argument("--env-a", default="", help="extra environment for variant A, as K=V,K2=V2")
    parser.add_argument("--env-b", default="", help="extra environment for variant B, as K=V,K2=V2")
    parser.add_argument("rest", nargs="*", help="extra replay arguments")
    args = parser.parse_args()

    os.makedirs(args.work, exist_ok=True)
    rows = []
    print(f"capture {os.path.basename(args.capture)}  repeat {args.repeat}  model {os.path.basename(args.model)}"
          f"  env_a [{args.env_a}]  env_b [{args.env_b}]")
    print(f"{'round':>5} {'variant':>8} {'wall':>5} {'n':>6} {'plane':>6} {'forward':>8} {'deliv':>6} {'deliv99':>8} {'crcOK':>6}")
    for round_index in range(args.rounds):
        # ABBA: the order flips every round, so a monotone drift (thermal, cache, background load) lands on both
        # variants equally instead of on whichever one always ran second.
        env_a, env_b = parse_env(args.env_a), parse_env(args.env_b)
        order = [(args.label_a, args.a, env_a), (args.label_b, args.b, env_b)]
        if round_index % 2 == 1:
            order.reverse()
        for label, tool, extra_env in order:
            prefix = os.path.join(args.work, f"ab_{label}_r{round_index}")
            row = one_run(tool, args.capture, args.model, args.repeat, prefix, args.rest, extra_env)
            row.update({"round": round_index, "variant": label})
            rows.append(row)
            print(f"{round_index:5d} {label:>8} {row['wall']:5.1f} {row.get('count', 0):6d} "
                  f"{row.get('plane', 0):6d} {row.get('forward', 0):8d} {row.get('deliver', 0):6d} "
                  f"{row.get('deliver99', 0):8d} {row['ok']:6d}")

    def summarise(title, selected):
        if not selected:
            print(f"\n{title}: no run to summarise")
            return
        print(f"\n{title} ({len(selected)} run(s))")
        for label in (args.label_a, args.label_b):
            subset = [row for row in selected if row["variant"] == label]
            if not subset:
                continue
            deliver = [row["deliver"] for row in subset]
            deliver99 = [row["deliver99"] for row in subset]
            forward = [row["forward"] for row in subset]
            print(f"  {label:>8}: deliver p50 {statistics.median(deliver):6.0f} us "
                  f"(range {min(deliver)}-{max(deliver)})  p99 {statistics.median(deliver99):6.0f} us  "
                  f"forward p50 {statistics.median(forward):6.0f} us   n={len(subset)}")

    summarise("ALL RUNS", rows)
    if args.drop_slow:
        fast = [row for row in rows if row.get("plane", 0) <= args.drop_slow]
        summarise(f"FAST MODE ONLY (plane p50 <= {args.drop_slow} us)", fast)
        # Paired by round: only rounds where BOTH legs are in the same mode say anything about the code.
        pairs = []
        for round_index in range(args.rounds):
            a_row = [r for r in fast if r["round"] == round_index and r["variant"] == args.label_a]
            b_row = [r for r in fast if r["round"] == round_index and r["variant"] == args.label_b]
            if a_row and b_row:
                pairs.append((a_row[0]["deliver"], b_row[0]["deliver"], a_row[0]["deliver99"], b_row[0]["deliver99"]))
        if pairs:
            print(f"\n  paired rounds: {len(pairs)}")
            for a_value, b_value, a99, b99 in pairs:
                print(f"    {args.label_a} {a_value:5d} us  {args.label_b} {b_value:5d} us   "
                      f"delta {b_value - a_value:+5d} us   (p99 {a99}/{b99})")
            deltas = [b - a for a, b, _, _ in pairs]
            print(f"    median paired delta ({args.label_b} - {args.label_a}) = "
                  f"{statistics.median(deltas):+.0f} us over {len(deltas)} round(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
