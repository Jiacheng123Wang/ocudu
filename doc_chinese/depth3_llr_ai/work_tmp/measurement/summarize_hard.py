"""Summarise a leg's hard test set: CPU baseline against one model's self pass and optional HARQ pass.

WHY A SCRIPT. Three things have to be exercised together and each has bitten us once: the denominator is the CPU bits
INSIDE the test set (not the full leg), the HARQ pass overlaps the self pass on the units the model decoded on its own
(first transmission), and the KO units have no CPU bits at all so their model bits are pure gain. Doing that in a shell
one-liner lost a whole afternoon to a double count, so it lives here with the overlap printed every time.

Usage: summarize_hard.py <testsets_hard dir> <leg> <self.csv> [<harq.csv>]
"""
import collections
import csv
import os
import sys

tsdir, leg = sys.argv[1], sys.argv[2]
self_csv = sys.argv[3]
harq_csv = sys.argv[4] if len(sys.argv) > 4 else None

labels = {r["unit"]: r for r in csv.DictReader(open(os.path.join(tsdir, leg, "labels.csv")))}
self_ok = {r["unit"]: int(r["bits"]) for r in csv.DictReader(open(self_csv)) if r["status"] == "OK"}
harq_ok = ({r["unit"]: int(r["bits"]) for r in csv.DictReader(open(harq_csv)) if r["status"] == "OK"}
           if harq_csv and os.path.exists(harq_csv) else {})

agg = collections.defaultdict(lambda: [0, 0, 0, 0])   # units, cpu_ok, cpu_bits, model_bits
for u, r in labels.items():
    a = agg[r["category"]]
    a[0] += 1
    if r["crc"] == "OK":
        a[1] += 1
        a[2] += int(r["tbs_bytes"]) * 8
    if u in self_ok:
        a[3] += self_ok[u]

# the HARQ pass only matters for units the self pass did NOT already decode: counting both would double count, and the
# number of such units is exactly what the earlier error hid.
overlap = set(self_ok) & set(harq_ok)
# A HARQ index can hold units the test set dropped (a retransmission the chain only needed one or two iterations for),
# so anything outside the labels is skipped rather than assumed to belong to a category.
extra = set(harq_ok) - set(labels)
for u in (set(harq_ok) - set(self_ok) - extra):
    agg[labels[u]["category"]][3] += harq_ok[u]

print(f"=== {leg} ===")
tot_cpu = tot_model = 0
for cat in ("KO", "self_hard", "harq_hard"):
    n, cok, cb, mb = agg[cat]
    tot_cpu += cb
    tot_model += mb
    delta = f"{100*(mb-cb)/cb:+.2f}%" if cb else "n/a (CPU 0)"
    print(f"  {cat:10} units {n:6d} | CPU {cok:6d} u {cb:>12,} b | model {mb:>12,} b | {delta}")
print(f"  {'TOTAL':10} units {sum(v[0] for v in agg.values()):6d} | CPU {tot_cpu:>14,} b | model {tot_model:>14,} b "
      f"| {100*(tot_model-tot_cpu)/tot_cpu:+.2f}%")
if harq_csv:
    print(f"  HARQ overlap: {len(overlap)} units / {sum(self_ok[u] for u in overlap):,} bits "
          f"(kept once, from the self pass); units outside the test set: {len(extra)}")
