#!/usr/bin/env python3
"""Judge a mirrored multi-arm flight: per-arm throughput, the interleaved comparison, and each
leg's DM-RS pattern.

WHY IT EXISTS (design sections 19.211, 46.20, 46.21). The receiver's outcome on this cell is decided
by the DM-RS pattern the session happens to use -- the arm beats the classical chain on 2,11 and
collapses on 2,7,11 -- so a flight whose legs ran different patterns is not a comparison of arms.
The pattern cannot be read from the leg's log; it is in the PDU of an uplink capture, so each leg
records a few captures (OCUDU_UL_DUMP_COUNT=60, ~8 MB) and this tool reports what each leg actually
ran. That number is printed next to the throughput, because an arm comparison across different
patterns must be refused, not averaged.

The order is MIRRORED (a b c c b a): the channel drifts, so each arm appears once in each half and
the drift cancels. The comparison reported is therefore mean-of-two-halves per arm, plus the
conservative reading: the WORST leg of each arm, so a single good half cannot carry a verdict.

Usage:
  arms_summary.py --batch work_tmp/legs/c32dmrs \\
      --arms a:new b:ft c:classic --mirror a b c c b a \\
      --rate a=112.4 --rate b=96.1 --rate c=104.7 ...      (Mbit/s, one per leg, in flight order)
  arms_summary.py ... --rates-file rates.txt               (same, one "arm,rate" per line)
"""

import argparse
import collections
import glob
import os
import re
import sys

ARM_MODEL_HINT = {
    "a": "the candidate bundle",
    "b": "the deployed bundle",
    "c": "the classical chain",
}


def pattern_of_leg(batch, label, short=""):
    """The DM-RS pattern a leg ran, from the PDU of its uplink captures (None if none recorded).

    Both layouts are looked for: the captures sit under a directory named after the leg label in some
    flights, and under `pat/<short>/` in the one this tool was written for (batch c32dmrs), where the
    capture prefix is per-leg but the directory is `pat/a1` and so on.
    """
    roots = glob.glob(os.path.join(batch, "**", f"{label}*"), recursive=True) + \
            glob.glob(os.path.join(batch, f"{label}*"))
    if short:
        roots += glob.glob(os.path.join(batch, "pat", short)) + \
                 glob.glob(os.path.join(batch, "**", "pat", short), recursive=True)
    seen = collections.Counter()
    for root in roots:
        if not os.path.isdir(root):
            continue
        for meta in glob.glob(os.path.join(root, "C_*.txt")):
            if meta.endswith("_ce.txt"):
                continue
            try:
                for line in open(meta):
                    if line.startswith("dmrs_symbols="):
                        seen[line.strip().split("=", 1)[1]] += 1
                        break
            except OSError:
                continue
    if not seen:
        return None
    return seen.most_common(1)[0][0]


def leg_stderr_facts(batch, label):
    """What the leg's own stderr says the arm was: the two lines that cannot be inferred from the flag."""
    facts = {"arm_active": None, "calibrated": None, "degraded": None}
    for path in glob.glob(os.path.join(batch, "**", f"*{label}*.log.stderr"), recursive=True):
        try:
            text = open(path, errors="ignore").read()
        except OSError:
            continue
        m = re.search(r"AI ARM ACTIVE over (\d+) model", text)
        facts["arm_active"] = int(m.group(1)) if m else ("classical" if "receiver=classic" in text else None)
        m = re.search(r"slope ([0-9.]+) offset ([-+0-9.]+)", text)
        facts["calibrated"] = f"slope {m.group(1)} offset {m.group(2)}" if m else "not declared"
        m = re.search(r"reception DEGRADED to the classical chain", text)
        facts["degraded"] = bool(m)
    return facts


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--batch", required=True, help="the leg directory of the batch")
    parser.add_argument("--arms", nargs="+", default=["a:new", "b:ft", "c:classic"],
                        help="arm letter : description")
    parser.add_argument("--mirror", nargs="+", default=["a", "b", "c", "c", "b", "a"],
                        help="the order the legs were flown in")
    parser.add_argument("--rate", action="append", default=[],
                        help="iperf3 receiver Mbit/s, one per leg, IN FLIGHT ORDER (arm=value)")
    parser.add_argument("--slack", type=float, default=3.0,
                        help="percent a challenger must exceed the reference by to count as better")
    args = parser.parse_args()

    arms = dict(spec.split(":", 1) if ":" in spec else (spec, ARM_MODEL_HINT.get(spec, spec))
                for spec in args.arms)
    if len(args.rate) != len(args.mirror):
        raise SystemExit(f"--rate needs one value per leg ({len(args.mirror)}), got {len(args.rate)}")

    print(f"batch {args.batch}")
    print(f"  {'#':>2} {'arm':>4} {'rate Mbit/s':>12} {'pattern':>10} {'arm active':>11} {'calibration':>22}")
    per_arm = collections.defaultdict(list)
    patterns = collections.defaultdict(set)
    # The label's ordinal counts PER ARM (a1 b1 c1 c2 b2 a2), not the flight position: the legs are
    # named after the arm they are the first/second instance of, and matching on the position would
    # look for `c4` and find nothing.
    ordinal = collections.Counter()
    for index, (spec, letter) in enumerate(zip(args.rate, args.mirror), start=1):
        ordinal[letter] += 1
        arm, value = spec.split("=", 1)
        if arm != letter:
            print(f"  leg {index}: --rate names arm '{arm}' but the mirror says '{letter}'", file=sys.stderr)
            return 2
        label = f"{os.path.basename(args.batch.rstrip('/'))}-{letter}{ordinal[letter]}"
        facts = leg_stderr_facts(args.batch, label)
        pattern = pattern_of_leg(args.batch, label, f"{letter}{ordinal[letter]}")
        rate = float(value)
        per_arm[letter].append(rate)
        if pattern:
            patterns[letter].add(pattern)
        print(f"  {index:>2} {letter:>4} {rate:>12.1f} {str(pattern or '?'):>10} "
              f"{str(facts['arm_active']):>11} {str(facts['calibrated']):>22}")

    print()
    print(f"  {'arm':>4} {'meaning':<26} {'legs':>5} {'mean':>8} {'min':>8} {'max':>8}")
    for letter in sorted(per_arm):
        values = per_arm[letter]
        print(f"  {letter:>4} {arms.get(letter, '')[:26]:<26} {len(values):>5} "
              f"{sum(values) / len(values):>8.1f} {min(values):>8.1f} {max(values):>8.1f}")

    # A comparison across different DM-RS patterns measures the pattern, not the arm.
    mixed = {letter: pats for letter, pats in patterns.items() if len(pats) > 1}
    all_patterns = set().union(*patterns.values()) if patterns else set()
    print()
    if mixed:
        print("REFUSING a verdict: legs of the same arm ran DIFFERENT DM-RS patterns "
              f"({ {k: sorted(v) for k, v in mixed.items()} }). The arm's decode on this cell is decided "
              "by the pattern, so this batch compares patterns, not arms. Re-fly, or compare only the "
              "legs that share a pattern.")
        return 1
    if len(all_patterns) > 1:
        print(f"WARNING: the arms ran different patterns ({sorted(all_patterns)}); the comparison is only "
              "meaningful if every arm is represented in every pattern, which this batch is not.")
    elif all_patterns:
        print(f"all legs ran DM-RS {sorted(all_patterns)[0]} -- the comparison is like-for-like")

    def compare(challenger, reference):
        if not per_arm.get(challenger) or not per_arm.get(reference):
            return
        a = per_arm[challenger]
        b = per_arm[reference]
        change = 100.0 * (sum(a) / len(a) - sum(b) / len(b)) / (sum(b) / len(b))
        worst = 100.0 * (min(a) - max(b)) / max(b)
        verdict = ("BETTER" if change >= args.slack and worst > 0 else
                   "WORSE" if change <= -args.slack else "NO MEASURABLE DIFFERENCE")
        print(f"  {challenger} vs {reference}: mean {change:+.1f}%  conservative (worst {challenger} "
              f"against best {reference}) {worst:+.1f}%  -> {verdict}")
    print()
    compare("a", "c")
    compare("b", "c")
    compare("a", "b")
    return 0


if __name__ == "__main__":
    sys.exit(main())
