#!/usr/bin/env python3
"""Aggregate ctest outcomes from all `make test` scan logs in build/macos_triage/logs.

Writes one row per ctest index to results.tsv/results.json: index, name, outcome, seconds, log.
Later runs override earlier ones for the same index.
"""
import os
import glob
import json
import sys
from collections import Counter

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
# Run artefacts live in <build>/macos_triage (generated directory); the tools themselves are versioned in the
# source tree. Override with TRIAGE_DIR when needed.
TRIAGE = os.environ.get("TRIAGE_DIR") or os.path.normpath(os.path.join(TOOLS_DIR, "..", "..", "..", "build", "macos_triage"))
os.makedirs(TRIAGE, exist_ok=True)
from parse_ctest import parse_log  # noqa: E402

LOGS = sorted(glob.glob(os.path.join(TRIAGE, "logs", "run_from_*.log")), key=os.path.getmtime)

results, started = {}, {}
for log in LOGS:
    r, s = parse_log(log)
    for idx, entry in r.items():
        entry["log"] = os.path.basename(log)
    results.update(r)
    started.update(s)

with open(os.path.join(TRIAGE, "results.tsv"), "w") as fh:
    for idx in sorted(results):
        r = results[idx]
        fh.write(f"{idx}\t{r['name']}\t{r['outcome']}\t{r['sec']}\t{r['log']}\n")
with open(os.path.join(TRIAGE, "results.json"), "w") as fh:
    json.dump({str(k): v for k, v in results.items()}, fh, indent=1)

counts = Counter(r["outcome"] for r in results.values())
print(f"logs parsed: {len(LOGS)}")
print(f"tests with a recorded outcome: {len(results)}")
for k, v in sorted(counts.items()):
    print(f"  {k}: {v}")

killed = sorted(set(started) - set(results))
if killed:
    print(f"started but never completed (killed by the hang scan): {len(killed)}")
    for idx in killed:
        print(f"  #{idx} {started[idx]}")

if results:
    covered = set(results) | set(killed)
    total = max(covered)
    missing = [i for i in range(1, total + 1) if i not in covered]
    print(f"max index seen: {total}; indices below it never attempted: {len(missing)}")
    if missing:
        ranges, s, p = [], missing[0], missing[0]
        for i in missing[1:]:
            if i != p + 1:
                ranges.append((s, p))
                s = i
            p = i
        ranges.append((s, p))
        print("  " + ", ".join(f"{a}" if a == b else f"{a}-{b}" for a, b in ranges[:40]))
