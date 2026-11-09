"""Check every dumped leg against the manual's three counts, and summarise the corpus.

The dump is judged by one identity (`corpus_to_training_to_measurement_db_manual.md` section 6):

    index.csv rows  ==  units with a .bin  ==  units.txt lines

Anything else is a dump that has to be redone. This walks the output tree, prints one line per leg with the three
numbers plus the CRC-OK rate, and exits non-zero if a leg disagrees, so a long unattended run has a single command
that answers "is it done, and is it good".

    python verify_dumps.py [out-root] [--corpus DIR]
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=os.path.expanduser("~/ai_depth3/dump_s2_mcs"))
    ap.add_argument("--corpus", default=os.path.expanduser("~/ai_depth3/corpus"))
    args = ap.parse_args()

    legs = sorted(d for d in os.listdir(args.out)
                  if d.startswith("tb_") and os.path.isdir(os.path.join(args.out, d)))
    bad = []
    tot_units = tot_ok = 0
    print(f"{'leg':38} {'units':>8} {'rows':>8} {'crc_ok':>8} {'ok%':>6}  state")
    for name in legs:
        d = os.path.join(args.out, name)
        units_file = os.path.join(d, "units.txt")
        index_file = os.path.join(d, "index.csv")
        if not (os.path.exists(units_file) and os.path.exists(index_file)):
            print(f"{name[3:]:38} {'-':>8} {'-':>8} {'-':>8} {'-':>6}  incomplete (no index or units)")
            bad.append(name)
            continue
        # `units.txt` HOLDS ABSOLUTE PATHS TO THE .txt SIDECARS, NOT BARE UNIT NAMES (dump_tb.sh writes the paths it
        # scanned). Treating them as names made every leg compare against `<leg>/<abs path>.bin` and fail with
        # with_bin=0 -- a bug in the checker, not in the dump. Reduce each line to the unit name it denotes.
        units = []
        for line in open(units_file):
            line = line.strip()
            if not line:
                continue
            name = os.path.basename(line)
            if name.endswith(".txt"):
                name = name[:-4]
            units.append(name)
        rows = sum(1 for _ in open(index_file)) - 1
        ok = sum(1 for line in open(index_file) if ",OK," in line)
        tot_units += len(units)
        tot_ok += ok
        # A LEG WHOSE CAPTURES WERE ARCHIVED CANNOT BE CHECKED AGAINST .bin ANY MORE, and that is not a fault: the
        # manual deletes the captures only after the dump and the archive are verified. The identity that still
        # applies is rows == units; the .bin count is added when the corpus directory is still there.
        leg_dir = os.path.join(args.corpus, name[3:])
        if os.path.isdir(leg_dir):
            with_bin = sum(1 for u in units if os.path.exists(os.path.join(leg_dir, u + ".bin")))
            state = "ok" if rows == len(units) == with_bin else f"MISMATCH with_bin={with_bin}"
        else:
            state = "ok (captures archived)" if rows == len(units) else f"MISMATCH rows={rows}"
        if rows == 0:
            state = "IN PROGRESS (no rows yet)"
        if not state.startswith("ok"):
            bad.append(name)
        rate = 100.0 * ok / len(units) if units else 0.0
        print(f"{name[3:]:38} {len(units):8} {rows:8} {ok:8} {rate:5.1f}%  {state}")
    print(f"\nlegs: {len(legs)}   units: {tot_units:,}   crc_ok: {tot_ok:,} "
          f"({100.0 * tot_ok / max(tot_units, 1):.1f}%)")
    if bad:
        print(f"FAIL: {len(bad)} leg(s) need attention: {', '.join(b[3:] for b in bad[:8])}"
              + (" ..." if len(bad) > 8 else ""))
        return 1
    print("PASS: every leg reconciles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
