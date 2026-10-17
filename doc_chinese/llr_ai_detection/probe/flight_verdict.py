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
                       r"calibration (not declared|APPLIED)")
TIMING = re.compile(r"\[depth3-timing\] (\d+) forward\(s\).*?plane p50 (\d+).*?forward p50 (\d+).*?deliver p50 (\d+)")
LEG = re.compile(r"([a-z0-9]*?)([a-d])(\d+)$")
# The PHY logs every reception at INFO with the value the chain published, so a CLASSICAL leg -- which has no
# `[depth3-measure]` line at all, because the arm is not running -- still yields a comparability basis. `tbs` filters
# the MSG3 receptions out: they are QPSK, tiny, and their SINR is not the traffic's.
PUSCH = re.compile(r"PUSCH: .*?tbs=(\d+).*?sinr=(-?(?:inf|[\d.]+))dB")


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
        # The leg runs ONE ARM PER POOL THREAD, and each arm keeps its own ring: the counters interleave (measured on
        # the r8 logs: 1 1 500 1000 500 1500 ... -- two series), so the last line is whichever arm printed last and a
        # single line would describe one thread. The median over ALL lines is the leg.
        raw50s = [float(hit[2]) for hit in published_hits]
        cal50s = [float(hit[5]) for hit in published_hits]
        calibration = published_hits[-1][7]
        published = (statistics.median(raw50s), statistics.median(cal50s))
    # The published value per reception, from the PHY log beside the stderr one (see PUSCH above).
    chain = []
    log_path = path.replace(".log.stderr", ".log")
    if os.path.exists(log_path):
        for tbs, sinr in PUSCH.findall(open(log_path, "r", errors="replace").read()):
            if int(tbs) < 100 or "inf" in sinr:
                continue
            chain.append(float(sinr))
    timing = TIMING.findall(raw)
    # Same reason as the published SINR: several arms, several series. Each line is a ring's p50, so the median over
    # the lines is the leg's value; the reception count is the LARGEST counter, which is the busiest arm's.
    if timing:
        def median_at(position):
            return int(statistics.median(int(line[position]) for line in timing))
        last_timing = (max(int(line[0]) for line in timing), median_at(1), median_at(2), median_at(3))
    else:
        last_timing = None
    return {
        # The classical ESTIMATOR's SNR: sampled in an AI leg (`[depth3-measure] classic=`), and equal to what the
        # chain publishes in a classical leg, where the PHY line above carries it at full rate.
        "classic_snr": statistics.median(classic) if classic else (statistics.median(chain) if chain else None),
        "classic_source": "depth3-measure" if classic else ("pusch line" if chain else "none"),
        "published_chain": statistics.median(chain) if chain else None,
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
    parser.add_argument("--ping", default="",
                        help="mean ping RTT per leg, ms, as tag=value,... . It is the ONLY comparability basis that "
                             "does not come from the receiver under test, and for a classical leg it can be the only "
                             "one available at all: the PHY's per-reception `PUSCH: ... sinr=` line is not in every "
                             "build, and a classical leg has no `[depth3-measure]` line.")
    parser.add_argument("--batch", default="", help="only legs whose tag starts with this")
    parser.add_argument("--equivalence-pct", type=float, default=5.0,
                        help="a difference smaller than this is NOT read as a result. Pre-registered from the "
                             "leg-to-leg spread inside one arm in the r8 batch (A 10.9 vs 11.5 = 5.5%%, "
                             "B 9.58 vs 10.0 = 4.4%%): below that, two legs of the SAME arm already differ, so a "
                             "difference of that size between arms is not measurable with this setup.")
    parser.add_argument("--tolerance-rtt-pct", type=float, default=10.0,
                        help="maximum relative ping-RTT difference between adjacent legs for the comparability check, "
                             "used when the SNR bases cannot be compared")
    parser.add_argument("--tolerance-db", type=float, default=1.5,
                        help="maximum adjacent classical-SNR difference for the legs to count as comparable")
    args = parser.parse_args()

    throughput = parse_iperf(args.iperf)
    rtt = parse_iperf(args.ping) if args.ping else {}
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
            # LOUD, not silent. The first version of this pattern ended at `c`, so an eight-leg batch's `d` legs were
            # dropped without a word and the verdict reported three arms out of four -- a result that looks fine and
            # answers a different question.
            print(f"  IGNORED: {name} -- no leg tag of the form <batch><a-d><n> in it, so this leg is not in the verdict")
            continue
        tag = parts[-1]
        if args.batch and not tag.startswith(args.batch):
            continue
        arm = LEG.match(tag).group(2).upper()
        row = read_leg(path)
        row.update({"tag": tag, "arm": arm, "mbit": throughput.get(tag), "rtt": rtt.get(tag)})
        rows.append(row)

    if not rows:
        print("no leg log matched; check --legs and --batch", file=sys.stderr)
        return 1

    arm_order = sorted({row["arm"] for row in rows})
    print(f"{'leg':>8} {'arm':>4} {'Mbit/s':>8} {'rtt ms':>7} {'comparability':>13} {'basis':>14} {'plane est':>10} "
          f"{'published':>18} {'degr':>5} {'recv':>7} {'plane us':>9} {'fwd us':>7} {'deliv us':>9}")
    print("    (timings and the published SINR are medians over EVERY [depth3-*] line in the leg: the pools run one arm"
          " per thread, so a leg's log holds several series)")
    for row in rows:
        published = "-"
        if row["published"] is not None:
            raw, cal = row["published"]
            published = f"{raw:.2f} -> {cal:.2f} ({row['calibration']})"
        print(f"{row['tag']:>8} {row['arm']:>4} {row['mbit'] if row['mbit'] is not None else float('nan'):>8.2f} "
              f"{row['rtt'] if row.get('rtt') is not None else float('nan'):>7.1f} "
              f"{row['classic_snr'] if row['classic_snr'] is not None else float('nan'):>13.2f} "
              f"{row['classic_source']:>14} "
              f"{row['plane_estimate'] if row['plane_estimate'] is not None else float('nan'):>10.2f} "
              f"{published:>18} {row['degradations']:>5} "
              f"{row['receptions'] if row['receptions'] else 0:>7} {row['plane'] if row['plane'] else 0:>9} "
              f"{row['forward'] if row['forward'] else 0:>7} {row['deliver'] if row['deliver'] else 0:>9}")

    print()
    # COMPARABILITY, and which metric it uses is decided by what the legs actually carry:
    #   * every leg has an estimator SNR of the SAME kind  -> use it (dB, the tolerance is in dB);
    #   * otherwise every leg has a ping RTT               -> use it (ms, the tolerance is a ratio);
    #   * otherwise                                        -> say the batch cannot be certified.
    # A classical leg can fall in the second case on its own: it has no `[depth3-measure]` line, and the PHY's
    # per-reception `PUSCH: ... sinr=` line is not present in every build.
    sources = {row["classic_source"] for row in rows}
    have_rtt = all(row["rtt"] is not None for row in rows)
    use_metric = None
    if len(sources) == 1 and "none" not in sources:
        use_metric = "snr"
    elif have_rtt:
        use_metric = "rtt"
    print(f"  comparability metric: {use_metric or 'NONE AVAILABLE'}  "
          f"(leg sources: {', '.join(sorted(sources))}; ping RTT for every leg: {'yes' if have_rtt else 'no'})")
    classic_ok = use_metric is not None
    for before, after in zip(rows, rows[1:]):
        if use_metric == "snr":
            first, second = before["classic_snr"], after["classic_snr"]
            delta = abs(first - second)
            limit, unit = args.tolerance_db, "dB"
        elif use_metric == "rtt":
            first, second = before["rtt"], after["rtt"]
            delta = abs(first - second) / max(min(first, second), 1e-9) * 100.0
            limit, unit = args.tolerance_rtt_pct, "%"
        else:
            print(f"  comparability {before['tag']:>6} vs {after['tag']:<6}: no metric available")
            continue
        flags = []
        if delta > limit:
            flags.append("NOT COMPARABLE")
            classic_ok = False
        if use_metric == "snr" and before["classic_source"] != after["classic_source"]:
            flags.append("different basis: " + before["classic_source"] + " vs " + after["classic_source"])
        print(f"  comparability {before['tag']:>6} vs {after['tag']:<6} differs {delta:5.2f} {unit}  "
              f"{'ok' if not flags else '; '.join(flags)}")
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
    print("  VERDICTS (pre-registered: A is the reference, no arm may be worse than it)")
    meanings = {"B": "the published-SINR calibration", "C": "the classical chain",
                "D": "R6's delivery form (calibrated + net)"}
    for candidate in [arm for arm in arm_order if arm != "A"]:
        if "A" not in by_arm:
            continue
        reference = statistics.mean(by_arm["A"])
        value = statistics.mean(by_arm[candidate])
        change = 100.0 * (value - reference) / reference
        comparability = "conditions matched" if classic_ok else "CONDITIONS NOT MATCHED, the verdict does not count"
        if abs(change) < args.equivalence_pct:
            print(f"  {candidate} vs A: {value:.2f} vs {reference:.2f} Mbit/s = {change:+.1f}%  -> "
                  f"NO MEASURABLE DIFFERENCE (inside the +-{args.equivalence_pct:g}% band the same arm's own legs span) "
                  f"({comparability})   [{meanings.get(candidate, 'no pre-registered meaning')}]")
            continue
        if candidate == "C":
            # The classical chain is not a candidate to adopt, it is the REFERENCE POINT for "is the AI receiver
            # worth it". A classical leg BELOW the AI arm is the good outcome, so a PASS/FAIL word would read
            # backwards; what the reader needs is which side is better and by how much.
            reading = ("AI arm is BETTER by " if change < 0 else "AI arm is NOT better (classical is above by ") + \
                      f"{abs(change):.1f}%" + ("" if change < 0 else ")")
            print(f"  {candidate} vs A: {value:.2f} vs {reference:.2f} Mbit/s = {change:+.1f}%  -> {reading} "
                  f"({comparability})")
        else:
            verdict = "PASS" if value >= reference else "FAIL"
            print(f"  {candidate} vs A: {value:.2f} vs {reference:.2f} Mbit/s = {change:+.1f}%  -> {verdict} "
                  f"({comparability})   [{meanings.get(candidate, 'no pre-registered meaning')}]")
    if not args.iperf:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
