#!/usr/bin/env python3
"""What the scheduler DOES with the published SINR: map each reception through the gNB's own MCS table.

Why this exists (design section 19.175). The arm's published SINR reads 1.2-3.4 dB below the classical chain's, and
the bundle now declares a calibration that removes it (section 19.172). Whether that matters for THROUGHPUT is a
question about the consumer, not about the number: the gNB turns the reported SINR into an MCS through
`map_snr_to_mcs_ul`, whose thresholds are a table in `lib/scheduler/support/mcs_calculator.cpp`. This script reads
that table FROM THE SOURCE -- transcribing 57 thresholds by hand is how a "prediction" becomes a rumour -- and reports
the MCS distribution the classical chain, the raw arm and the calibrated arm would each select.

THE HONEST CAVEAT, printed with the result: this ignores OLLA. The scheduler's outer loop adds an offset
(`olla_ul_snr_inc` 0.001 dB per ACK, cap 5 dB by default) that absorbs a CONSTANT reporting bias over time, so the
steady-state MCS difference caused by a constant offset tends to zero. What OLLA cannot absorb is a bias that VARIES
with SNR -- and the arm's error does (section 19.172(b): QPSK -1.3 dB at 20-23 rising to -3.1 at 26-30). So the MCS
table below is an UPPER BOUND on the instantaneous difference and says nothing about the steady state; it is the right
number for "what does the scheduler see on a reception before its outer loop has adapted".

Usage:
    sinr_to_mcs.py --csv "name=path.csv,..." --modulation 256QAM [--operating-point 16,24]
"""
import argparse
import csv
import re
import statistics
import sys

SOURCE = "/Users/jiachengwang/dev/ocudu/lib/scheduler/support/mcs_calculator.cpp"


def load_thresholds():
    """The two UL SNR tables, parsed out of the scheduler's own source."""
    text = open(SOURCE).read()

    def array(name):
        block = re.search(name + r"\s*=\s*\{(.*?)\};", text, re.S).group(1)
        block = block.split("// clang-format on")[0]
        return [float(value) for value in re.findall(r"-?\d+\.?\d*", re.sub(r"//[^\n]*", "", block))]

    return {"qam256": array("ul_snr_256qam_mcs_table"), "qam64": array("ul_snr_mcs_table")}


def mcs_for(snr, thresholds):
    """The largest MCS whose threshold is at or below `snr` (what `map_snr_to_mcs_ul` does with upper_bound)."""
    selected = 0
    for index, minimum in enumerate(thresholds):
        if snr >= minimum:
            selected = index
    return selected


def load(path, column):
    """{capture: published dB} from one run's report, taking the column that holds THAT arm's publication."""
    out = {}
    for row in csv.DictReader(open(path)):
        try:
            value = float(row[column])
        except (TypeError, ValueError, KeyError):
            continue
        if value < -90:
            continue
        out[row["capture"]] = value
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True,
                        help="name=path[#column] items, comma separated; column defaults to sinr_ai")
    parser.add_argument("--modulation", default="256QAM", choices=("QPSK", "16QAM", "64QAM", "256QAM"))
    parser.add_argument("--table", default="", help="qam256 or qam64; defaults to qam256 for 256QAM, else qam64")
    parser.add_argument("--operating-point", default="",
                        help="lo,hi dB: keep only captures whose CLASSICAL publication is in this range. The table "
                             "saturates at its top entry, so the corpus's own operating point can hide the effect "
                             "completely -- evaluate where the MCS has headroom.")
    args = parser.parse_args()
    table = args.table or ("qam256" if args.modulation == "256QAM" else "qam64")
    thresholds = load_thresholds()[table]

    # name=path#column: `sinr_ai` is the AI arm's publication and `sinr_classic` the classical chain's, both from the
    # SAME report (one process per arm per capture), so the comparison is capture by capture.
    runs = {}
    for pair in args.csv.split(","):
        name, spec = pair.split("=", 1)
        path, _, column = spec.partition("#")
        runs[name] = load(path, column or "sinr_ai")
    if not runs:
        print("no csv given", file=sys.stderr)
        return 1

    shared = set.intersection(*(set(run) for run in runs.values()))
    if args.operating_point:
        low, high = (float(value) for value in args.operating_point.split(","))
        reference_column = runs.get("classical")
        if reference_column is None:
            print("--operating-point needs a 'classical' source to filter on", file=sys.stderr)
            return 1
        shared = {capture for capture in shared if low <= reference_column[capture] < high}
        print(f"(restricted to the classical publication in [{low:g}, {high:g}) dB: {len(shared)} capture(s))")
        if not shared:
            return 1
    print(f"{args.modulation} over {len(shared)} capture(s), UL MCS table '{table}' "
          f"({len(thresholds)} entries, thresholds {thresholds[0]:.2f} to {thresholds[-1]:.2f} dB)")
    print(f"{'arm':>22} {'SINR p50':>9} {'MCS p50':>8} {'MCS p10':>8} {'MCS p90':>8} {'MCS=0':>7}")
    mcs_by_run = {}
    for name, run in runs.items():
        values = [run[capture] for capture in shared]
        mcs = [mcs_for(value, thresholds) for value in values]
        mcs_by_run[name] = (values, mcs)
        print(f"{name:>22} {statistics.median(values):>9.2f} {statistics.median(mcs):>8.1f} "
              f"{sorted(mcs)[len(mcs)//10]:>8d} {sorted(mcs)[9*len(mcs)//10]:>8d} "
              f"{sum(1 for value in mcs if value == 0):>7d}")
    # The classical chain's publication is the reference the scheduler has always used, so it is the baseline the
    # other arms are compared against -- not the "classic" column of the arm, which is the ESTIMATOR's SNR.
    names = list(runs)
    if "classical" in names:
        base = mcs_by_run["classical"][1]
        print()
        for name in names:
            if name == "classical":
                continue
            deltas = [value - reference for value, reference in zip(mcs_by_run[name][1], base)]
            print(f"  MCS steps {name:>20} - classical: median {statistics.median(deltas):+.1f}  "
                  f"p10 {sorted(deltas)[len(deltas)//10]:+.1f}  p90 {sorted(deltas)[9*len(deltas)//10]:+.1f}  "
                  f"({sum(1 for value in deltas if value > 0)} higher, {sum(1 for value in deltas if value < 0)} lower)")
    print()
    print("  NOTE: OLLA is NOT modelled. It adds an offset (0.001 dB/ACK, 5 dB cap by default) that absorbs a")
    print("  CONSTANT bias over time, so this table is an upper bound on the INSTANTANEOUS difference and says")
    print("  nothing about steady state. A bias that varies with SNR is what OLLA cannot absorb.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
