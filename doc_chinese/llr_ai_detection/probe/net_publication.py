#!/usr/bin/env python3
"""What `net` actually publishes, against what the classical chain publishes on the SAME reception.

Why this exists. R6's acceptance has two halves, and only one of them is throughput. The other is accuracy: under
`--pusch_reporting_source=net` the chain's RSRP, EPRE and SINR come from the arm's heads, and the question is whether
they agree with the classical chain, which is the set of numbers the scheduler has always acted on. The corpus can
answer that offline, reception by reception, because the replay harness prints all three fields of the PUBLISHED
result -- the same fields the scheduler reads.

Note what the SINR column can and cannot say: the arm publishes its own post-equalisation noise in EITHER reporting
mode, so the SINR here is the arm's publication under `net`, not a difference between the two modes (design section
19.172(a)). With the calibrated bundle it should agree with the classical chain's; with the deployed one it should
read low by the amount section 19.172 measured.

Usage:
    net_publication.py --tool <replay> --corpus <dir> --list <file> --model <bundle> [--work <dir>] [--limit N]
"""
import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import tempfile

RESULT = re.compile(r"crc=(\w+).*?sinr=(-?[\d.]+|-?inf)\s*dB\s+epre=(-?[\d.]+|-?inf)\s*dB\s+rsrp=(-?[\d.]+|-?inf)\s*dB")


def run(tool, capture, prefix, model, reporting):
    env = dict(os.environ)
    if model:
        env["OCUDU_REPLAY_RECEIVER"] = "ai"
        env["OCUDU_REPLAY_MODEL"] = model
        env["OCUDU_REPLAY_REPORTING"] = reporting
    else:
        env["OCUDU_REPLAY_RECEIVER"] = "classic"
    for stale in (f"{prefix}_llr.bin",):
        if os.path.exists(stale):
            os.remove(stale)
    out = subprocess.run([tool, capture, "--metal", "--out", prefix], capture_output=True, text=True,
                         env=env).stdout
    found = RESULT.search(out)
    if not found:
        return None
    def number(text):
        return float(text) if "inf" not in text else None
    return {"crc": found.group(1), "sinr": number(found.group(2)), "epre": number(found.group(3)),
            "rsrp": number(found.group(4))}


def summarise(rows, field, label):
    deltas = [row[f"{field}_ai"] - row[f"{field}_classic"] for row in rows
              if row.get(f"{field}_ai") is not None and row.get(f"{field}_classic") is not None]
    if not deltas:
        print(f"  {label:>6}: no comparable reception")
        return
    deltas.sort()
    absolute = sorted(abs(value) for value in deltas)
    print(f"  {label:>6}: n={len(deltas):3d}  median {statistics.median(deltas):+6.2f} dB  "
          f"p10 {deltas[len(deltas)//10]:+6.2f}  p90 {deltas[9*len(deltas)//10]:+6.2f}  "
          f"|delta| p90 {absolute[9*len(absolute)//10]:5.2f} dB")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--list", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--reporting", default="net")
    parser.add_argument("--work", default="")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--csv", default="", help="also write the per-reception table, so a tail can be attributed "
                                                 "instead of merely reported")
    args = parser.parse_args()

    work = args.work or tempfile.mkdtemp(prefix="net_pub_")
    os.makedirs(work, exist_ok=True)
    captures = [line.strip() for line in open(args.list) if line.strip()]
    if args.limit:
        captures = captures[:args.limit]

    rows = []
    for index, name in enumerate(captures):
        capture = os.path.join(args.corpus, name)
        prefix = os.path.join(work, f"c{index}")
        classic = run(args.tool, capture, prefix + "_k", None, None)
        reported = run(args.tool, capture, prefix + "_n", args.model, args.reporting)
        if classic is None or reported is None:
            continue
        row = {"capture": name, "crc": reported["crc"]}
        for field in ("sinr", "epre", "rsrp"):
            row[f"{field}_classic"] = classic[field]
            row[f"{field}_ai"] = reported[field]
        rows.append(row)
        if (index + 1) % 25 == 0:
            print(f"  ... {index + 1}/{len(captures)}", flush=True)

    print(f"{os.path.basename(args.corpus)}, {len(rows)} reception(s), reporting '{args.reporting}', "
          f"model {os.path.basename(args.model)}")
    print("  delta = published(net) - published(classical), on the same reception")
    for field, label in (("rsrp", "RSRP"), ("epre", "EPRE"), ("sinr", "SINR")):
        summarise(rows, field, label)
    if args.csv:
        with open(args.csv, "w") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        print(f"  wrote {args.csv}")
    decoded = sum(1 for row in rows if row["crc"] == "OK")
    print(f"  {decoded}/{len(rows)} reception(s) decoded with the AI arm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
