#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# The SPIKE CENSUS: every instrumented series' max and p99 next to its median, per leg, so the question
# "is the tail a property of one module or a property of the platform?" is answered by a table instead of by
# memory (dev doc macos_thread_priority/... , P0; the finding that started this workflow: on p163/p180/p182 the
# CE max is ~20x its median, the LDPC max ~8-15x, V1 ~4x, and even the receive wait spikes to ~2 ms).
#
# WHY IT MATTERS: the per-series MEDIAN is stable and that is why every delivery criterion is green; the tail is
# what moves, and it moves in EVERY series - including ones that run on different threads (rx thread, ul thread,
# the pool threads). A criterion for "thread timing stability" therefore has to be per-series and per-thread.
#
# usage:
#   python3 wip/spike_census.py                       # all legs in wip/logs (this workflow) + the history dir
#   python3 wip/spike_census.py <file.log.stderr> …   # explicit files
#   python3 wip/spike_census.py --top 12              # show the 12 worst max/median ratios (default 10)
#
# Output: one row per (leg, series) with samples/median/max/ratio, then a short "worst ratios" section.

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
# This workflow's own legs first, then the two history directories, so a fresh checkout still gets a table.
LEG_DIRS = [
    os.path.join(HERE, "logs"),
    os.path.join(ROOT, "doc_chinese", "phy_pipeline_gpu", "wip", "logs"),
]

# The series this census knows about. The NAMES are deliberately explicit - a generic parser would silently pick
# up unrelated "samples=… median=…" lines and inflate the table - but they all carry the same six numbers
# (samples/mean/median/min/max/p95/p99), so one template reads them.
#
# WHY p95/p99 ARE READ AT ALL (added 2026-10-01, same session as the first threshold derivation). The census
# originally printed median and max only, and the first attempt to derive a criterion from max failed for a
# reason the data then showed plainly: over the 20 newest n78/gpu legs - ten different commits - the per-leg
# `ul_channel_estimation` max swung from 201 us to 4792 us while its p99 stayed in 146..173 us and its
# `ul_time_frequency` p99 in 605..675 us. max is ONE DRAW from a heavy tail; p99 is a property of the
# population. A criterion therefore needs both, for different jobs (see threshold_candidates.py).
_STATS = (r"samples=(\d+) mean=([\d.]+)us median=([\d.]+)us min=([\d.]+)us max=([\d.]+)us "
          r"p95=([\d.]+)us p99=([\d.]+)us")
SERIES = [
    (name, r"^\[" + name + r"\] " + _STATS)
    for name in ("ul_rx_wait", "ul_rx_wait_hop", "ul_time_frequency", "ul_channel_estimation",
                 "ul_equalization_demod", "ul_ldpc_decode", "ul_gpu_pipeline", "ul_pipeline")
]
SERIES.append(
    # NOTE: dl_tx_call's max carries "(at call #N)" and call #1 is the TX stream spin-up (84-85 ms on EVERY leg,
    # dev doc phy_latency 6.219): a census that printed its ratio without saying so would report a 2000x "spike"
    # that is a startup constant. The pattern captures the call index for exactly that reason. This series has no
    # `samples=` field and no `min=`; its fields are mapped onto the same record so the table stays uniform.
    ("dl_tx_call",
     r"^\[dl_tx_call\] calls=(\d+) median=([\d.]+)us p95=([\d.]+)us p99=([\d.]+)us max=([\d.]+)us"
     r"(?: \(at call #(\d+)\))?")
)

# Field order of one record: the numbers a criterion can be read from, and the sample count that says how much
# of the run the reading covers. `mean` and `min` are read but NOT kept: no criterion in this line uses them
# (a mean is dominated by the median, a min by the fastest slot), and carrying them would invite one.
FIELDS = ("samples", "median", "p95", "p99", "max")


def leg_label(name):
    """"gnb_gpu_p182-n78-stress_1001_1319.log.stderr" -> "p182-n78-stress_1001_1319"."""
    n = name
    for pre in ("gnb_gpu_", "gnb_cpu_gpu_", "gnb_cpu_"):
        if n.startswith(pre):
            n = n[len(pre):]
            break
    return n.replace(".log.stderr", "")


def read_leg(path):
    """{series: {samples, median, p95, p99, max}} - {} when the file cannot be read.

    A series the leg does not report is ABSENT (not zero): a leg whose phase segments were off must not look
    like a leg whose phase segments were 0 us.
    """
    try:
        with open(path, errors="replace") as fh:
            txt = fh.read()
    except OSError:
        return {}
    out = {}
    for name, rx in SERIES:
        m = re.search(rx, txt, re.M)
        if m is None:
            continue
        if name == "dl_tx_call":
            call = int(m.group(6)) if m.group(6) else 0
            # call #1 == the startup spin-up: reported, but marked so nobody reads it as a stall.
            key = name + ("@call#1(startup)" if call == 1 else "")
            # [calls, median, p95, p99, max] - the call count stands in for `samples`.
            out[key] = dict(zip(FIELDS, [int(m.group(1))] + [float(m.group(i)) for i in (2, 3, 4, 5)]))
        else:
            out[name] = dict(zip(FIELDS, [int(m.group(1))] + [float(m.group(i)) for i in (2, 6, 7, 5)]))
    return out


def main():
    args = sys.argv[1:]
    top = 10
    recent = 12
    all_legs = "--all" in args
    if "--all" in args:
        args.remove("--all")
    if "--recent" in args:
        i = args.index("--recent")
        recent = int(args[i + 1])
        del args[i:i + 2]
    if "--legs" in args:
        i = args.index("--legs")
        want = args[i + 1]
        del args[i:i + 2]
        args.append(want)  # reused below as a substring filter
        want_legs = want
    else:
        want_legs = None
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i:i + 2]

    files = []
    if args:
        files = [a for a in args if os.path.isfile(a)]
    else:
        for d in LEG_DIRS:
            if not os.path.isdir(d):
                continue
            files += [os.path.join(d, n) for n in sorted(os.listdir(d)) if n.endswith(".log.stderr")]
        if not all_legs:
            # newest first by mtime, then the N most recent (a 100-leg table is unreadable and hides the point)
            files.sort(key=lambda f: os.path.getmtime(f), reverse=True)
            files = files[:recent]
        files.sort()
    if want_legs is not None:
        files = [f for f in files if want_legs in f]
    if not files:
        print("no legs found; pass .log.stderr paths or fly one into wip/logs/", file=sys.stderr)
        return 2
    print(f"# {len(files)} leg(s); newest {recent} by default, --all for every leg, --legs <substr> to filter")

    rows = []
    for f in files:
        for series, st in read_leg(f).items():
            ratio = (st["max"] / st["median"]) if st["median"] > 0 else float("inf")
            rows.append((leg_label(os.path.basename(f)), series, st, ratio))

    # p99 sits between the two columns it explains: it is the highest number in the report that a CRITERION can
    # be read from, because it describes the population; `max` is one draw from the tail and moves by 30x between
    # legs of the same family (see the SERIES comment).
    print(f"{'leg':34s} {'series':24s} {'samples':>9s} {'median':>9s} {'p99':>9s} {'max':>10s} {'max/med':>8s}")
    for leg, series, st, ratio in rows:
        r = "   inf" if ratio == float("inf") else f"{ratio:8.1f}"
        print(f"{leg:34s} {series:24s} {st['samples']:9d} {st['median']:9.1f} {st['p99']:9.1f} {st['max']:10.1f} {r}")

    print()
    print(f"== worst max/median ratios (top {top}) ==")
    # 0-median series (a symbol-grained receive reads median 0) would sort as inf; keep them but label them, since
    # "median 0 with a 2 ms max" IS the shape this census exists for.
    # Sort by ratio, and BY THE MAX on a tie: every median-0 series is "inf", so ordering on the ratio alone
    # leaves the biggest spike wherever the table happened to put it (measured: the 153 ms row was hidden behind
    # smaller inf rows until this secondary key was added).
    for leg, series, st, ratio in sorted(rows, key=lambda r: (-r[3], -r[2]["max"]))[:top]:
        r = "inf (median 0)" if ratio == float("inf") else f"x{ratio:.1f}"
        print(f"  {r:16s} {series:24s} median={st['median']:8.1f}us max={st['max']:9.1f}us  [{leg}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
