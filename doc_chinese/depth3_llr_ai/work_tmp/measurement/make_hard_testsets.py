"""Split each full leg into the units that are still worth testing, and label them.

WHY. Most transport blocks are decoded by the classical chain in one or two LDPC iterations, and those blocks carry no
signal about which receiver is better: the network and the CPU both get them, so counting them only dilutes whatever
difference exists. The blocks that discriminate are the other three kinds:

    KO         the chain never decoded the block (CRC failed), so no receiver in this comparison got it on its own
    self_hard  it decoded on its own transmission, but only after >= 3 LDPC iterations
    harq_hard  the first transmission failed and a later one recovered it, again after >= 3 iterations

The threshold is on the dump's own `iterations` column, which is the classical chain's LDPC iteration count -- the one
quantity that says how close to the cliff a block is. `index.csv` holds the self decodes and `index_harq_only.csv` the
harq-recovered ones, both written by dump_tb.sh and harq_backfill.py.

OUTPUT, per leg, under <out>/<leg>/:
    units.txt    the hard units, in the corpus order (this is what measure_fulllegen.py takes)
    labels.csv   unit,category,crc,iterations,tbs_bytes,donor
    stats.txt    what was kept, per category, with the bit totals

Usage: make_hard_testsets.py <full_leg_measurement dir> <out dir> [--min-iterations 3]
"""
import argparse
import collections
import csv
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="directory holding corpus_<leg>/ dump dirs (and corpus/ for the captures)")
    ap.add_argument("out", help="where the four test sets are written")
    ap.add_argument("--min-iterations", type=int, default=3,
                    help="a CRC-OK unit is kept only when the chain needed at least this many LDPC iterations")
    args = ap.parse_args()

    dumps = os.path.join(args.root, "dumps")
    legs = sorted(d for d in os.listdir(dumps) if d.startswith("corpus_"))
    if not legs:
        sys.exit(f"no corpus_* dump directories under {dumps}")

    for leg in legs:
        d = os.path.join(dumps, leg)
        # units.txt holds absolute capture paths (that is what dump_tb.sh writes); the test sets carry bare unit names,
        # because measure_fulllegen.py joins them with a corpus directory that may live anywhere.
        units = [os.path.basename(l.strip())[:-4] for l in open(os.path.join(d, "units.txt")) if l.strip()]
        self_rows = {r["unit"]: r for r in csv.DictReader(open(os.path.join(d, "index.csv")))}
        harq_path = os.path.join(d, "index_harq_only.csv")
        harq_rows = {r["unit"]: r for r in csv.DictReader(open(harq_path))} if os.path.exists(harq_path) else {}

        labels = {}
        for u in units:
            s = self_rows.get(u)
            h = harq_rows.get(u)
            crc_ok = (s is not None and s.get("crc") == "OK")
            harq_ok = (h is not None and h.get("crc") == "OK")
            if crc_ok:
                it = int(float(s.get("iterations") or 0))
                if it >= args.min_iterations:
                    labels[u] = ("self_hard", "OK", it, int(float(s.get("tbs_bytes") or 0)), "")
            elif harq_ok:
                it = int(float(h.get("iterations") or 0))
                if it >= args.min_iterations:
                    labels[u] = ("harq_hard", "OK", it, int(float(h.get("tbs_bytes") or 0)), h.get("donor", ""))
            else:
                labels[u] = ("KO", "no", 0, 0, "")

        outdir = os.path.join(args.out, leg)
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, "units.txt"), "w") as fh:
            fh.write("\n".join(u for u in units if u in labels) + "\n")
        with open(os.path.join(outdir, "labels.csv"), "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["unit", "category", "crc", "iterations", "tbs_bytes", "donor"])
            for u in units:
                if u in labels:
                    cat, crc, it, tb, donor = labels[u]
                    w.writerow([u, cat, crc, it, tb, donor])
        counts = collections.Counter(v[0] for v in labels.values())
        bits = sum(v[3] * 8 for v in labels.values() if v[1] == "OK")
        with open(os.path.join(outdir, "stats.txt"), "w") as fh:
            fh.write(f"leg {leg}\n")
            fh.write(f"units total            {len(units)}\n")
            fh.write(f"units kept (hard)      {len(labels)}  ({100*len(labels)/max(len(units),1):.1f}%)\n")
            for cat in ("KO", "self_hard", "harq_hard"):
                fh.write(f"  {cat:10} {counts.get(cat, 0)}\n")
            fh.write(f"CPU deliverable bits   {bits}\n")
        print(f"{leg}: kept {len(labels):5d}/{len(units)}  KO {counts.get('KO',0):5d}  "
              f"self_hard {counts.get('self_hard',0):4d}  harq_hard {counts.get('harq_hard',0):4d}  bits {bits:,}")


if __name__ == "__main__":
    main()
