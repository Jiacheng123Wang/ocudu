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

# The series this census knows about: the name and the regex that reads its line. Deliberately explicit - a
# generic parser would silently pick up unrelated "samples=… median=…" lines and inflate the table.
SERIES = [
    ("ul_rx_wait", r"^\[ul_rx_wait\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_rx_wait_hop", r"^\[ul_rx_wait_hop\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_time_frequency", r"^\[ul_time_frequency\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_channel_estimation", r"^\[ul_channel_estimation\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_equalization_demod", r"^\[ul_equalization_demod\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_ldpc_decode", r"^\[ul_ldpc_decode\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_gpu_pipeline", r"^\[ul_gpu_pipeline\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    ("ul_pipeline", r"^\[ul_pipeline\] samples=(\d+) mean=[\d.]+us median=([\d.]+)us min=[\d.]+us max=([\d.]+)us"),
    # NOTE: dl_tx_call's max carries "(at call #N)" and call #1 is the TX stream spin-up (84-85 ms on EVERY leg,
    # dev doc phy_latency 6.219): a census that printed its ratio without saying so would report a 2000x "spike"
    # that is a startup constant. The pattern captures the call index for exactly that reason.
    ("dl_tx_call", r"^\[dl_tx_call\] calls=\d+ median=([\d.]+)us p95=[\d.]+us p99=[\d.]+us max=([\d.]+)us(?: \(at call #(\d+)\))?"),
]


def leg_label(name):
    """"gnb_gpu_p182-n78-stress_1001_1319.log.stderr" -> "p182-n78-stress_1001_1319"."""
    n = name
    for pre in ("gnb_gpu_", "gnb_cpu_gpu_", "gnb_cpu_"):
        if n.startswith(pre):
            n = n[len(pre):]
            break
    return n.replace(".log.stderr", "")


def read_leg(path):
    """[(series, samples, median, max)] - `dl_tx_call` has no samples field, reported as 0."""
    try:
        txt = open(path, errors="replace").read()
    except OSError:
        return []
    out = []
    for name, rx in SERIES:
        m = re.search(rx, txt, re.M)
        if m is None:
            continue
        if name == "dl_tx_call":
            median, mx = float(m.group(1)), float(m.group(2))
            call = int(m.group(3)) if (m.lastindex or 0) >= 3 and m.group(3) else 0
            # call #1 == the startup spin-up: reported, but marked so nobody reads it as a stall.
            out.append((name + ("@call#1(startup)" if call == 1 else ""), 0, median, mx))
        else:
            out.append((name, int(m.group(1)), float(m.group(2)), float(m.group(3))))
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
        for series, samples, median, mx in read_leg(f):
            ratio = (mx / median) if median > 0 else float("inf")
            rows.append((leg_label(os.path.basename(f)), series, samples, median, mx, ratio))

    print(f"{'leg':34s} {'series':24s} {'samples':>9s} {'median':>9s} {'max':>10s} {'max/med':>8s}")
    for leg, series, samples, median, mx, ratio in rows:
        r = "   inf" if ratio == float("inf") else f"{ratio:8.1f}"
        print(f"{leg:34s} {series:24s} {samples:9d} {median:9.1f} {mx:10.1f} {r}")

    print()
    print(f"== worst max/median ratios (top {top}) ==")
    # 0-median series (a symbol-grained receive reads median 0) would sort as inf; keep them but label them, since
    # "median 0 with a 2 ms max" IS the shape this census exists for.
    # Sort by ratio, and BY THE MAX on a tie: every median-0 series is "inf", so ordering on the ratio alone
    # leaves the biggest spike wherever the table happened to put it (measured: the 153 ms row was hidden behind
    # smaller inf rows until this secondary key was added).
    for leg, series, samples, median, mx, ratio in sorted(rows, key=lambda r: (-r[5], -r[4]))[:top]:
        r = "inf (median 0)" if ratio == float("inf") else f"x{ratio:.1f}"
        print(f"  {r:16s} {series:24s} median={median:8.1f}us max={mx:9.1f}us  [{leg}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
