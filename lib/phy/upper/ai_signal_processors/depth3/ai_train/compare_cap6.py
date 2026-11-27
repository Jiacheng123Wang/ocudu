#!/usr/bin/env python3
"""Compare a fresh dump of one leg against the local cap-6 dump of the same leg.

WHY. The dump now runs the LDPC decoder at twenty iterations by default, so its `crc_ok` is a superset of the air
configuration's verdicts (cap 6). The comparison answers three questions at once for every leg: did the data change at
all where it must not (SINR, slot, transport-block size), did anything that decoded at six regress at twenty (nothing
should, because of early termination), and do the transport blocks that both dumps carry agree byte for byte.

WHAT IT WRITES. One CSV row on stdout (the driver appends it to a report) and a readable block on stderr. Exit status is
0 when the comparison passes, 3 when it finds a difference worth a human's look, and 2 when there is no reference dump.

Usage: compare_cap6.py --leg <leg> [--corpus DIR] [--ref DIR] [--seconds N] [--status S]
"""
import argparse
import concurrent.futures
import csv
import hashlib
import os
import sys

SHARED = ("sinr_db", "slot", "tbs_bytes")
FLIP = ("tb_bits", "crc")


def md5(path):
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_index(path):
    """Index rows, minus the AppleDouble stubs.

    THE OLD DUMPS CARRIED `._` JUNK UNITS. A corpus packed on a Mac has a resource-fork stub beside every file, and the
    dump of the time selected `*.txt` without excluding them, so a w2 reference index holds twice the real unit count and
    every one of those stubs is "missing" from a dump made after the exclusion was added. They carry no verdict, so they
    are dropped rather than reported as a difference.
    """
    return {row["unit"]: row for row in csv.DictReader(open(path)) if not row["unit"].startswith("._")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True)
    ap.add_argument("--corpus", default="/Volumes/T7_Corpus/corpus")
    ap.add_argument("--ref", default=os.path.expanduser("~/ai_depth3/dump_s2_amc"))
    ap.add_argument("--seconds", default="")
    ap.add_argument("--status", default="")
    args = ap.parse_args()

    new = os.path.join(args.corpus, f"tb_{args.leg}")
    ref = os.path.join(args.ref, f"tb_{args.leg}")
    row = dict(leg=args.leg, status=args.status, ref="yes", units="", crc_ok="", self="", harq="", ko="", tb="",
               flips="", regressions="", shared_diffs="", bit_common="", bit_mismatch="", bits_new="",
               seconds=args.seconds)

    # The fresh dump is the subject: its counts are reported whether or not there is a reference.
    if not os.path.isdir(new) or not os.path.exists(os.path.join(new, "index.csv")):
        print(f"{args.leg}: no fresh dump at {new}", file=sys.stderr)
        print(",".join(str(row[k]) for k in row), end="\n")
        return 3
    with open(os.path.join(new, "index.csv")) as handle:
        new_rows = list(csv.DictReader(handle))
    row["units"] = len(new_rows)
    row["crc_ok"] = sum(1 for r in new_rows if r["crc"] == "OK")
    row["tb"] = len([f for f in os.listdir(os.path.join(new, "tb")) if f.endswith(".bits")])
    for key, name in (("self", "index_self.csv"), ("harq", "index_harq_only.csv"), ("ko", "index_ko.csv")):
        path = os.path.join(new, name)
        row[key] = len(list(csv.DictReader(open(path)))) if os.path.exists(path) else ""

    if not os.path.isdir(ref) or not os.path.exists(os.path.join(ref, "index.csv")):
        row["ref"] = "no"
        print(f"{args.leg}: no cap-6 reference at {ref}", file=sys.stderr)
        print(",".join(str(row[k]) for k in row))
        return 2

    old_rows = load_index(os.path.join(ref, "index.csv"))
    new_idx = {r["unit"]: r for r in new_rows}
    common = set(old_rows) & set(new_idx)
    missing = set(old_rows) - set(new_idx)
    extra = set(new_idx) - set(old_rows)

    shared_diffs = {c: sum(1 for u in common if old_rows[u][c] != new_idx[u][c]) for c in SHARED}
    flips = {c: sum(1 for u in common if old_rows[u][c] != new_idx[u][c]) for c in FLIP}
    up = sum(1 for u in common if old_rows[u]["crc"] == "no" and new_idx[u]["crc"] == "OK")
    down = sum(1 for u in common if old_rows[u]["crc"] == "OK" and new_idx[u]["crc"] == "no")
    row["flips"] = up
    row["regressions"] = down
    row["shared_diffs"] = sum(shared_diffs.values())

    # THE TRANSPORT BLOCKS ARE THE PAYLOAD. A shared file that differs, or a decode that regressed, is the only kind of
    # difference this comparison treats as a finding; everything else is the expected label harvest.
    old_tb = {f[:-5] for f in os.listdir(os.path.join(ref, "tb"))
              if f.endswith(".bits") and not f.startswith("._")}
    new_tb = {f[:-5] for f in os.listdir(os.path.join(new, "tb")) if f.endswith(".bits")}
    bit_common = sorted(old_tb & new_tb)

    # PARALLEL, BECAUSE THE COST IS THE OPEN, NOT THE HASH. Twenty-two thousand one-kilobyte files took eighteen minutes
    # one at a time on the replay host, with almost no CPU time: the wall clock went to per-file latency on a busy disk.
    def differs(unit):
        return md5(os.path.join(ref, "tb", unit + ".bits")) != md5(os.path.join(new, "tb", unit + ".bits"))

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        mismatch = sum(pool.map(differs, bit_common))
    row["bit_common"] = len(bit_common)
    row["bit_mismatch"] = mismatch
    row["bits_new"] = len(new_tb - old_tb)

    bad = (row["shared_diffs"] != 0 or down != 0 or mismatch != 0 or missing or extra)
    if not args.status:
        row["status"] = "CHECK" if bad else "ok"

    print(f"{args.leg}: units={row['units']} crc_ok={row['crc_ok']} (cap6 {sum(1 for r in old_rows.values() if r['crc'] == 'OK')}) "
          f"self={row['self']} harq={row['harq']} ko={row['ko']} tb={row['tb']}", file=sys.stderr)
    print(f"  +{up} 个 KO→OK, -{down} 个 OK→KO | shared diffs {shared_diffs} | "
          f"bits: common {len(bit_common)} mismatch {mismatch}, only-new {len(new_tb - old_tb)}, "
          f"only-old {len(old_tb - new_tb)} | unit 差异: 缺 {len(missing)} 多 {len(extra)}", file=sys.stderr)
    print(f"  verdict: {'PASS' if not bad else 'CHECK'}", file=sys.stderr)
    print(",".join(str(row[k]) for k in row))
    return 0 if not bad else 3


if __name__ == "__main__":
    raise SystemExit(main())
