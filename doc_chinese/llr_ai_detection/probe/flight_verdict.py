#!/usr/bin/env python3
"""Turn a leg directory into the verdict the acceptance criteria are written in.

Why a script. The last three flights were read by hand, and one of those readings produced a mechanism that a later
measurement refuted (design section 19.172: `net` does not change the SINR the scheduler sees, so the "-2.5 dB"
explanation of a 12.6% throughput loss could not hold). The numbers that decide the verdict are few and each has a
canonical source, so they are extracted mechanically and printed together with the checks the criteria require:

    comparability  the classical estimator's SNR median per leg (independent of the reporting source, which is what
                   makes it a valid basis for "same channel conditions"): adjacent legs within 1.5 dB;
    utility        the throughput per leg, from the operator's iperf3 (`iperf3 -R -t 120`, no `-b`);
    what the scheduler saw
                   `[depth3-sinr]` when the arm prints it (the PUBLISHED value, raw against calibrated), and otherwise
                   the `[depth3-measure]` plane estimate -- which is NOT the published value and is labelled so: that
                   line compares the arm's estimate against the ESTIMATOR's SNR, two different quantities.

Usage:
    flight_verdict.py --legs <leg dir> --iperf "r8a1=10.9,r8b1=9.58,r8b2=10.0,r8a2=11.5" [--batch r8]

The letter in a leg's name is its arm: a -> A, b -> B, c -> C. The criteria are then applied by letter:
    A >= ...  B    the calibration (or whatever B changes) must not lose throughput
    B >= C    the AI arm must not lose throughput against the classical chain
"""
import argparse
import collections
import glob
import os
import re
import statistics
import sys

MEASURE = re.compile(r"\[depth3-measure\] reception \d+ \(Qm=(\d+).*?snr net=(-?[\d.]+) classic=(-?[\d.]+) dB")
PUBLISHED = re.compile(r"\[depth3-sinr\] (\d+) reception\(s\): PUBLISHED SINR raw p10 (-?[\d.]+) p50 (-?[\d.]+) "
                       r"p90 (-?[\d.]+) dB \| calibrated p10 (-?[\d.]+) p50 (-?[\d.]+) p90 (-?[\d.]+) dB \| "
                       r"calibration (\w+)")
TIMING = re.compile(r"\[depth3-timing\] (\d+) forward\(s\).*?plane p50 (\d+).*?forward p50 (\d+).*?deliver p50 (\d+)")
LEG = re.compile(r"([a-z0-9]*?)([a-c])(\d+)$")


def parse_iperf(text):
    out = {}
    for pair in text.split(","):
        if "=" in pair:
            tag, value = pair.split("=", 1)
            out[tag.strip()] = float(value)
    return out


def read_leg(path):
    """Everything one leg's stderr says, reduced to the numbers the verdict needs."""
    raw = open(path, "r", errors="replace").read()
    classic, net, published, calibration = [], [], None, None
    for qm, net_db, classic_db in MEASURE.findall(raw):
        classic.append(float(classic_db))
        net.append(float(net_db))
    published_hits = PUBLISHED.findall(raw)
    if published_hits:
        # The LAST line is the whole leg's ring by then; the calibration state is taken from it as well.
        _, raw10, raw50, raw90, cal10, cal50, cal90, calibration = published_hits[-1]
        published = (float(raw50), float(cal50))
    timing = TIMING.findall(raw)
    last_timing = timing[-1] if timing else None
    return {
        "classic_snr": statistics.median(classic) if classic else None,
        "plane_estimate": statistics.median(net) if net else None,
        "published": published,
        "calibration": calibration,
        "degradations": raw.count("DEGRADED to the classical chain"),
        "receptions": int(last_timing[0]) if last_timing else None,
        "plane": int(last_timing[1]) if last_timing else None,
        "forward": int(last_timing[2]) if last_timing else None,
        "deliver": int(last_timing[3]) if last_timing else None,
        "samples": len(classic),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--legs", required=True)
    parser.add_argument("--iperf", required=True, help="throughput per leg, Mbit/s, as tag=value,...")
    parser.add_argument("--batch", default="", help="only legs whose tag starts with this")
    parser.add_argument("--tolerance-db", type=float, default=1.5,
                        help="maximum adjacent classical-SNR difference for the legs to count as comparable")
    args = parser.parse_args()

    throughput = parse_iperf(args.iperf)
    rows = []
    # BY TIME, not by name: the ABBA order is the criterion's whole point, and an alphabetical sort puts a1, a2, b1,
    # b2 -- i.e. two legs of the same arm adjacent and the two A legs compared against each other. The leg's stamp is
    # the trailing MMSS/HHMM pair in `gnb_gpu_<tag>_<MMDD>_<HHMM>.log.stderr`.
    def stamp(path):
        parts = os.path.basename(path).replace(".log.stderr", "").split("_")
        return "_".join(parts[-2:])

    for path in sorted(glob.glob(os.path.join(args.legs, "*.log.stderr")), key=stamp):
        name = os.path.basename(path).replace(".log.stderr", "")
        # gnb_gpu_r8a1_1017_0951 -> r8a1
        parts = [part for part in name.split("_") if LEG.match(part)]
        if not parts:
            continue
        tag = parts[-1]
        if args.batch and not tag.startswith(args.batch):
            continue
        arm = LEG.match(tag).group(2).upper()
        row = read_leg(path)
        row.update({"tag": tag, "arm": arm, "mbit": throughput.get(tag)})
        rows.append(row)

    if not rows:
        print("no leg log matched; check --legs and --batch", file=sys.stderr)
        return 1

    arm_order = sorted({row["arm"] for row in rows})
    print(f"{'leg':>8} {'arm':>4} {'Mbit/s':>8} {'classic SNR':>12} {'plane est':>10} {'published':>18} "
          f"{'degr':>5} {'recv':>7} {'plane us':>9} {'fwd us':>7} {'deliv us':>9}")
    for row in rows:
        published = "-"
        if row["published"] is not None:
            raw, cal = row["published"]
            published = f"{raw:.2f} -> {cal:.2f} ({row['calibration']})"
        print(f"{row['tag']:>8} {row['arm']:>4} {row['mbit'] if row['mbit'] is not None else float('nan'):>8.2f} "
              f"{row['classic_snr'] if row['classic_snr'] is not None else float('nan'):>12.2f} "
              f"{row['plane_estimate'] if row['plane_estimate'] is not None else float('nan'):>10.2f} "
              f"{published:>18} {row['degradations']:>5} "
              f"{row['receptions'] if row['receptions'] else 0:>7} {row['plane'] if row['plane'] else 0:>9} "
              f"{row['forward'] if row['forward'] else 0:>7} {row['deliver'] if row['deliver'] else 0:>9}")

    print()
    # COMPARABILITY: the classical estimator's SNR, which neither reporting source changes.
    classic_ok = True
    for before, after in zip(rows, rows[1:]):
        if before["classic_snr"] is None or after["classic_snr"] is None:
            continue
        delta = abs(before["classic_snr"] - after["classic_snr"])
        flag = "ok" if delta <= args.tolerance_db else "NOT COMPARABLE"
        if delta > args.tolerance_db:
            classic_ok = False
        print(f"  comparability {before['tag']:>6} vs {after['tag']:<6} classic SNR differs {delta:5.2f} dB  {flag}")
    by_arm = collections.defaultdict(list)
    for row in rows:
        if row["mbit"] is not None:
            by_arm[row["arm"]].append(row["mbit"])
    print()
    for arm in arm_order:
        values = by_arm.get(arm, [])
        if values:
            print(f"  arm {arm}: {len(values)} leg(s), mean {statistics.mean(values):.2f} Mbit/s  "
                  f"({', '.join(f'{value:.2f}' for value in values)})")
        else:
            print(f"  arm {arm}: no throughput given")

    print()
    print("  VERDICTS (pre-registered: A is the reference, B must not be worse, C is the classical chain)")
    for candidate in ("B", "C"):
        if candidate not in by_arm or "A" not in by_arm:
            continue
        reference = statistics.mean(by_arm["A"])
        value = statistics.mean(by_arm[candidate])
        change = 100.0 * (value - reference) / reference
        verdict = "PASS" if value >= reference else "FAIL"
        comparability = "conditions matched" if classic_ok else "CONDITIONS NOT MATCHED, the verdict does not count"
        print(f"  {candidate} vs A: {value:.2f} vs {reference:.2f} Mbit/s = {change:+.1f}%  -> {verdict} "
              f"({comparability})")
    if not args.iperf:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
