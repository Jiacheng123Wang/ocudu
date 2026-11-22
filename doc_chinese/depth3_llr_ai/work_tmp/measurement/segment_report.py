"""The standard report for a model: delivered bits split by what the classical chain did with the block.

WHY FOUR SEGMENTS. A single total hides the only thing that matters: where the difference comes from. The chain either
decoded the block on its own (and then no receiver can deliver MORE than those bits -- a model can only tie), decoded
it only after >= 3 LDPC iterations (the cliff), failed on its own transmission but a later one recovered it (the chain
needs HARQ for those), or never decoded it at all. Those four behave completely differently:

    easy       the chain needed 1-2 iterations: the model can at best tie  -> contributes ~0 to any difference
    self_hard  the chain needed >= 3 iterations: a fair head-to-head on the cliff
    harq       the chain's first transmission failed: the model's first-transmission decodes are pure gain, but once
               the chain is allowed to combine, the chain usually wins those back
    KO         the chain never decoded it: every bit the model delivers is net gain

Measuring the model therefore needs two passes (its own transmission, and the HARQ units with their donors), and the
report has to say which of the two produced each segment's bits.

Usage:
    segment_report.py <label> <dump dir> <self csv> [<harq csv>] [--units <units.txt>]

`--units` restricts the report to the units that were actually measured. Without it the report covers every unit in the
dump, which is right for a full-leg pass and wrong for a 5000-unit ruler pass: the chain's bits would be summed over all
50,000 units while the model only had 5,000 of them.

The dump directory supplies the chain's verdict (index.csv) and the HARQ list (index_harq_only.csv); the CSVs are
measure_fulllegen.py / measure_harq.py output (unit,status,bits).
"""
import collections
import csv
import os
import sys


def load_ok(path):
    if not path or not os.path.exists(path):
        return {}
    return {r["unit"]: int(r["bits"]) for r in csv.DictReader(open(path)) if r["status"] == "OK"}


def main():
    argv = list(sys.argv[1:])
    units_file = None
    if "--units" in argv:
        k = argv.index("--units")
        units_file = argv[k + 1]
        del argv[k:k + 2]
    label, dump, self_csv = argv[0], argv[1], argv[2]
    harq_csv = argv[3] if len(argv) > 3 else None

    index = {r["unit"]: r for r in csv.DictReader(open(os.path.join(dump, "index.csv")))}
    if units_file:
        keep = {l.strip() for l in open(units_file) if l.strip()}
        index = {u: r for u, r in index.items() if u in keep}
    harq_path = os.path.join(dump, "index_harq_only.csv")
    harq_rows = {r["unit"]: r for r in csv.DictReader(open(harq_path))} if os.path.exists(harq_path) else {}
    self_ok = load_ok(self_csv)
    harq_ok = load_ok(harq_csv)

    seg_of = {}
    for u, r in index.items():
        if r["crc"] == "OK":
            it = int(float(r["iterations"] or 0))
            seg_of[u] = "easy" if it <= 2 else "self_hard"
        else:
            seg_of[u] = "harq" if u in harq_rows else "KO"

    # a HARQ unit the model decoded on its own transmission is still the same block: count its bits once, from
    # whichever pass produced them (the self pass, since it is the cheaper one).
    stats = collections.defaultdict(lambda: collections.Counter())
    for u, seg in seg_of.items():
        s = stats[seg]
        s["units"] += 1
        if seg in ("easy", "self_hard"):
            s["cpu_self"] += int(float(index[u]["tbs_bytes"])) * 8
        elif seg == "harq":
            r = harq_rows.get(u)
            if r and r.get("crc") == "OK":
                s["cpu_harq"] += int(float(r["tbs_bytes"])) * 8
        if u in self_ok:
            s["model_self"] += self_ok[u]
        elif u in harq_ok:
            s["model_harq"] += harq_ok[u]

    print(f"=== {label} ===")
    head = f"{'segment':10} {'units':>7} {'CPU self':>14} {'model self':>14} {'d(self)':>10} {'CPU+HARQ':>14} {'model+HARQ':>14} {'d(+H)':>10}"
    print(head)
    tot = collections.Counter()
    for seg in ("easy", "self_hard", "harq", "KO"):
        s = stats[seg]
        cpu_self = s["cpu_self"]
        cpu_harq = s["cpu_self"] + s["cpu_harq"]
        model_self = s["model_self"]
        model_harq = s["model_self"] + s["model_harq"]
        d_self = f"{100*(model_self-cpu_self)/cpu_self:+9.2f}%" if cpu_self else "        -"
        d_harq = f"{100*(model_harq-cpu_harq)/cpu_harq:+9.2f}%" if cpu_harq else "        -"
        print(f"{seg:10} {s['units']:7d} {cpu_self:14,} {model_self:14,} {d_self:>10} {cpu_harq:14,} {model_harq:14,} {d_harq:>10}")
        for k in ("units", "cpu_self", "cpu_harq", "model_self", "model_harq"):
            tot[k] += s[k]
    cpu_self, cpu_harq = tot["cpu_self"], tot["cpu_self"] + tot["cpu_harq"]
    model_self, model_harq = tot["model_self"], tot["model_self"] + tot["model_harq"]
    print(f"{'TOTAL':10} {tot['units']:7d} {cpu_self:14,} {model_self:14,} "
          f"{100*(model_self-cpu_self)/cpu_self:+9.2f}% {cpu_harq:14,} {model_harq:14,} "
          f"{100*(model_harq-cpu_harq)/cpu_harq:+9.2f}%")
    # the overlap is the number that has burned us once: units the model decoded on their own transmission that the
    # HARQ pass also decoded. They are counted once, from the self pass, and the count is printed so it stays visible.
    overlap = set(self_ok) & set(harq_ok)
    if harq_csv:
        print(f"  (HARQ pass: {len(harq_ok):,} units OK; overlap with the self pass {len(overlap):,} units / "
              f"{sum(self_ok[u] for u in overlap):,} bits, counted once)")


if __name__ == "__main__":
    main()
