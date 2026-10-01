#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# WHERE THE CRITERIA FOR "THREAD RUNNING STABILITY" COME FROM: the distribution over the last N legs of the same
# FAMILY, not a number someone liked (dev doc macos_thread_priority/..., P0 deliverable 3; high level 5.2).
#
# WHY A SCRIPT AND NOT A TABLE. A threshold that cannot be re-derived is a threshold nobody can re-check, and
# this line will fly legs for weeks: every new leg must be able to move the bar, visibly, with the legs that
# produced it named. So the rules are fixed here and the numbers are whatever the logs say today.
#
# ★ THE RULES CHANGED ON THEIR FIRST RUN, AND THE DATA IS WHY (2026-10-01, recorded in the dev doc). The
# pre-registered rule was "threshold = 1.25 x the worst per-leg max". Over the 20 newest n78/gpu legs - ten
# different commits, no scheduling change among them - the per-leg `ul_channel_estimation` max read 201 us on one
# leg and 4792 us on another (a factor of 24) while its per-leg p99 stayed inside 146..173 us, and
# `ul_time_frequency` p99 inside 605..675 us. A bar built on max would therefore have to be so wide that a real
# regression disappears under it, and it would move every time the platform hiccupped once. The tail IS the
# subject of this line, so max is not dropped - it is moved to the job it can actually do:
#
#   * C1  p99      <= ceil_nice(1.25 x median of the per-leg p99)    the POPULATION did not move  -- binding
#   * C2  median   <= ceil_nice(1.25 x median of the per-leg median) the WORK did not get slower  -- binding
#   * max           reported, never binding: one draw from a heavy tail, attributed by the worst-K event list
#                   (OCUDU_UL_TIMING_EVENTS) which says WHEN and ON WHICH THREAD it happened.
#
# Both C1 and C2 use one bar per (series, regime) so a leg is judged by a number that existed before it flew.
# The whole point of the pair is that they fail differently: a uniformly slow leg passes C1 and fails C2, a leg
# with a new tail fails C1 and passes C2 - and only the second one is what "thread running stability" means.
#
# WHAT IS EXCLUDED, AND WHY IT MUST BE SAID OUT LOUD.
#  1. Legs observed with the heavy set (taskinfo/powermetrics/sample) carry the observation cost in their tails -
#     measured at two orders of magnitude (dev doc 10.3) - so they are named in OBSERVED_LEGS rather than matched
#     by a pattern: "which legs were perturbed" is a fact about what the operator did, not about the file name.
#  2. Legs of another FAMILY. A leg is only comparable with a leg of the same cell config and pipeline mode, and
#     the first run proved it rather than assuming it: over the newest 12 legs per regime taken unfiltered, the
#     [ul_pipeline] "median per-leg max" came out 4947 us, because the window reached back into 5 MHz n1 legs and
#     cpu-mode legs (--any-family prints that mixture, to SEE the confound).
#  3. Legs of another COMMIT, optionally (--same-commit). Every leg in this project tends to test a different
#     code state - measured: the 20 newest legs carry 20 distinct commits - so this filter usually leaves one or
#     two legs and NO distribution. It is offered because it is the right population for run-to-run variance, and
#     its emptiness is itself the finding: a run-to-run distribution requires legs that repeat a commit.
#
# usage:
#   python3 wip/threshold_candidates.py                 # last 12 legs per regime of the newest family
#   python3 wip/threshold_candidates.py --legs 20 --regime stress
#   python3 wip/threshold_candidates.py --same-commit   # one code state only (usually 1-2 legs)
#   python3 wip/threshold_candidates.py --any-family    # the unfiltered mixture, to see the confound

import argparse
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# One parser for both tools: a second copy of the series regexes would drift from spike_census.py silently
# (and the two would then disagree about the same leg).
from spike_census import LEG_DIRS, SERIES, leg_label, read_leg  # noqa: E402

# Legs whose tails carry the OBSERVATION, not the link (dev doc 10.3). A named list on purpose.
OBSERVED_LEGS = ["p182-n78-stress_1001_1726"]

# The series that carry a criterion, with the thread each one is attributed to (high level 3). A series that is
# not here is still measured and still censused - it just does not get a threshold until someone registers one.
CARRIERS = {
    "ul_rx_wait": "lower_phy_rx#0 (UHD receive)",
    "ul_rx_wait_hop": "lower_phy_rx#0 (hop-scoped)",
    "ul_time_frequency": "lower_phy_ul#0 (host front end)",
    "ul_channel_estimation": "main_pool#N (Metal CE dispatch/commit)",
    "ul_equalization_demod": "main_pool#N",
    "ul_ldpc_decode": "main_pool#N (CPU LDPC)",
    "ul_gpu_pipeline": "whole hop (fused lane)",
    "ul_pipeline": "whole hop",
    "dl_tx_call": "lower_phy_tx#0 (UHD send)",
}

# The leg's banner prints the three things that make one leg comparable with another: the cell config (geometry,
# hence the slot period every span is relative to), the pipeline mode (which lane does the work) and the RECEIVE
# POLICY - a whole-slot receive blocks ~1052 us per call and a symbol-grained one ~34 us, so mixing them mixes
# two [ul_rx_wait] populations and the mixture shows up in C2 as a 496 us bar against a current median of 34 us.
# Legs that predate the `[ul_rx_policy]` line get the policy from the buffer pool's own wording instead
# ("(slot=11520, whole-slot buffers, the gpu pipeline mode)"); a leg with neither reads `rx?`, which is a family
# of its own - it is not silently folded into the newest family just because its cell config happens to match.
BANNER_MODE = re.compile(r"^pipeline mode\s*:\s*(\S+)", re.M)
BANNER_CELL = re.compile(r"^cell config\s*:\s*(\S+)", re.M)
BANNER_RX = re.compile(r"^\[ul_rx_policy\] blocks of (\d+) OFDM symbols?", re.M)
BANNER_RX_SLOT = re.compile(r"^\[ul_rx_pool\][^\n]*whole-slot buffers", re.M)
# The commit is in the sibling `.log` (ocudulog), not in the `.log.stderr` the probes write to.
BUILT_COMMIT = re.compile(r"using commit ([0-9a-f]{7,40})")


def regime_of(label):
    """default / stress / other, from the leg label (the regime is part of the label by construction)."""
    if "stress" in label:
        return "stress"
    if "default" in label:
        return "default"
    return "other"


def banner_of(path):
    """'gpu/gnb_rf_b200_tdd_n78_20mhz.yml/rx1' - the key a leg is comparable under."""
    try:
        with open(path, errors="replace") as fh:
            txt = fh.read(200000)
    except OSError:
        return ""
    mode = BANNER_MODE.search(txt)
    cell = BANNER_CELL.search(txt)
    if mode is None or cell is None:
        return ""
    rx = BANNER_RX.search(txt)
    if rx is not None:
        policy = f"rx{rx.group(1)}"
    elif BANNER_RX_SLOT.search(txt):
        policy = "rxslot"
    else:
        policy = "rx?"
    return f"{mode.group(1)}/{os.path.basename(cell.group(1))}/{policy}"


def commit_of(path):
    """The commit the binary was built at, from the leg's own log line - '' for very old legs."""
    log = path[: -len(".log.stderr")] if path.endswith(".log.stderr") else path
    try:
        with open(log, errors="replace") as fh:
            txt = fh.read(200000)
    except OSError:
        return ""
    m = BUILT_COMMIT.search(txt)
    return m.group(1) if m else ""


def collect(files, nof_legs, family, any_family, same_commit):
    """({regime: [(label, stats)]}, family, commit) - the newest `nof_legs` eligible legs of each regime.

    TWO PASSES, and the first one is not optional: the family filter DEFAULTS to the newest leg's family, so the
    newest leg has to be known before anything can be filtered. The first version applied the filter only when
    --family was given explicitly, so the default run silently published an unfiltered mixture under a
    `family: gpu/…/rx1` heading - the heading said one thing and the table another.
    """
    legs = []
    for f in files:
        label = leg_label(os.path.basename(f))
        if label in OBSERVED_LEGS:
            continue
        stats = read_leg(f)
        if not stats:
            continue
        legs.append((os.path.getmtime(f), label, banner_of(f), commit_of(f), stats))
    legs.sort(key=lambda t: t[0], reverse=True)  # newest first
    if not legs:
        return {}, None, None
    newest_family = legs[0][2]
    newest_commit = legs[0][3]
    if not any_family:
        target = family if family is not None else newest_family
        legs = [leg for leg in legs if target in leg[2]]
    if same_commit:
        legs = [leg for leg in legs if leg[3] == newest_commit]

    per_regime = {}
    for _mtime, label, _fam, _commit, stats in legs:
        bucket = per_regime.setdefault(regime_of(label), [])
        if len(bucket) < nof_legs:
            bucket.append((label, stats))
    return per_regime, newest_family, newest_commit


def percentile(values, q):
    """Nearest-rank percentile: with a handful of legs an interpolated p90 would print a number no leg had."""
    if not values:
        return float("nan")
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, int(math.ceil(q * len(ordered))) - 1))
    return ordered[idx]


def ceil_nice(x, step):
    """Round up to a readable step, so the threshold reads like a threshold and not like a measurement."""
    return math.ceil(x / step) * step


def step_for(x):
    """1 us below 100 us, 10 us below 1 ms, 100 us above: a bar reading 1247.3 us invites arguing about the
    decimal instead of about the tail."""
    return 1 if x < 100 else (10 if x < 1000 else 100)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--legs", type=int, default=12, help="how many legs per regime (default 12)")
    ap.add_argument("--regime", default=None, help="only this regime (default: all)")
    ap.add_argument("--family", default=None, help="cell-config substring; default: the newest leg's family")
    ap.add_argument("--any-family", action="store_true", help="do not filter by family (shows the confound)")
    ap.add_argument("--same-commit", action="store_true", help="only legs of the newest leg's commit")
    args = ap.parse_args()

    files = []
    for d in LEG_DIRS:
        if os.path.isdir(d):
            files += [os.path.join(d, n) for n in os.listdir(d) if n.endswith(".log.stderr")]
    if not files:
        print("no legs found", file=sys.stderr)
        return 2

    per_regime, newest_family, newest_commit = collect(
        files, args.legs, args.family, args.any_family, args.same_commit)
    if not per_regime:
        print(f"no leg matches family {args.family!r}" + (" (and --same-commit)" if args.same_commit else ""),
              file=sys.stderr)
        return 2
    print(f"# family: {newest_family if not args.any_family else '<unfiltered: every config and mode>'}"
          f"{' / commit ' + newest_commit if args.same_commit else ''}")
    regimes = [r for r in ("stress", "default", "other") if r in per_regime]
    if args.regime is not None:
        regimes = [r for r in regimes if r == args.regime]

    for regime in regimes:
        legs = per_regime[regime]
        print(f"\n## regime `{regime}` - {len(legs)} leg(s), newest first")
        print("legs: " + ", ".join(label for label, _s in legs) + "\n")
        print("| series | carrier | legs | per-leg p99 med/worst | **C1: p99 <=** | per-leg max med/worst | max spread |")
        print("|---|---|---|---|---|---|---|")
        for series, _rx in SERIES:
            if series.startswith("dl_tx_call"):
                # This series goes wrong DOWNWARDS (the margin), and call #1 is the transmit stream's spin-up
                # (dev doc phy_latency 6.219), so a bar on its max would be a bar on the startup constant. Its
                # criterion is "no hand-over below", registered against [dl_tx_slack], not against this table.
                continue
            stats = [s[series] for _label, s in legs if series in s]
            if not stats:
                continue
            p99s = [st["p99"] for st in stats]
            maxes = [st["max"] for st in stats]
            med_p99, worst_p99 = percentile(p99s, 0.5), max(p99s)
            med_max, worst_max = percentile(maxes, 0.5), max(maxes)
            c1 = ceil_nice(1.25 * med_p99, step_for(med_p99))
            spread = (worst_max / med_max) if med_max > 0 else float("inf")
            carrier = CARRIERS.get(series, "")
            print(f"| `{series}` | {carrier} | {len(stats)} | {med_p99:.1f} / {worst_p99:.1f} us | {c1} | "
                  f"{med_max:.1f} / {worst_max:.1f} us | x{spread:.1f} |")
        # C2 is derived the same way and printed apart, because it answers the other half of the question.
        print("\n| series | per-leg median med/worst | **C2: median <=** |")
        print("|---|---|---|")
        for series, _rx in SERIES:
            if series.startswith("dl_tx_call"):
                continue
            stats = [s[series] for _label, s in legs if series in s]
            medians = [st["median"] for st in stats]
            if not medians:
                continue
            med_med, worst_med = percentile(medians, 0.5), max(medians)
            c2 = ceil_nice(1.25 * med_med, step_for(med_med))
            print(f"| `{series}` | {med_med:.1f} / {worst_med:.1f} us | {c2} |")
    print("\n(C1/C2 are CANDIDATES: a threshold only binds a leg flown after it is registered, and registering is "
          "a user decision - dev doc `phy_latency` 5.2 rule 2. A `max spread` far above 1 is the evidence that "
          "max cannot carry a criterion; the worst-K event list is what attributes those maxima.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
