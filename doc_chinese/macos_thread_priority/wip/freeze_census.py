#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# WHO OWNED THE TAIL: the timing events of a leg, sorted by how badly they went, with the one number that says
# whether the PROCESS was even running - and the wall clock of each one, ready to be lined up against the
# system's own log.
#
# WHY THIS EXISTS (dev doc macos_thread_priority 10.24/10.25). The goal of this line is to push `max` down by
# adjusting the runtime priority and scheduling policy of the PHY threads. The events of leg `p192-n78-tier5`
# showed that the biggest tails are NOT threads losing their turn: their `ivcsw_rate` sits at a TENTH of the
# leg's own baseline while the process burns a fraction of a core - i.e. the kernel is not running the process
# at all. No thread priority can fix that, and the reading that separates the two cases already exists in the
# report (the `leg :` line prints the baseline); what was missing was a tool that puts every event next to it.
#
# THE VERDICTS (and they are deliberately crude - this is a triage, not a criterion):
#   STALLED  ivcsw_rate < baseline/5   the process was not being scheduled: look for something OUTSIDE the gNB
#                                      (another process, a system daemon, a driver, the observer)
#   NO-CPU   cpu < 30% of the window   the process was scheduled but got almost no CPU over this window
#   BUSY     otherwise                 the process had its cores; the span is work, queueing or a fence
#
# usage:
#   python3 wip/freeze_census.py <leg-label | path-to-.log.stderr> [...]
# then, for any instant worth chasing:
#   log show --style compact --start "YYYY-MM-DD HH:MM:SS" --end "YYYY-MM-DD HH:MM:SS" | tail -n +2 \
#     | awk '{print $4}' | sort | uniq -c | sort -rn | head
# (that last command is what found `deleted`/`biomesyncd`/`mobileassetd` behind p192's worst CE segments.)

import os
import re
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from spike_census import LEG_DIRS, leg_label  # noqa: E402

# `[ul_timing_events]` prints the leg's own baseline; every event's `ivcsw_rate=` is read against it.
BASELINE = re.compile(r"leg : over [0-9.]+s .* = ([0-9.]+)/ms")
# One event line, whichever series it belongs to. The fields that matter here are the window and the CPU the
# process and the recording thread consumed inside it.
EVENT = re.compile(
    r"^  (?P<kind>rx|dl|t2f|ce|eqd|ldpc)\s*#(?P<rank>\d+)\s+"
    r"(?:(?P<value_name>wait|margin|took)=(?P<value>-?\d+)us\s+)?"
    r".*?wall=(?P<wall>[0-9T:.+-]+)\s+epoch_ms=(?P<epoch>\d+)\s+.*?"
    r"thread=(?P<thread>\S+)\s+"
    r"(?:load1=(?P<load1>-?[\d.]+)\s+)?"
    r"cpu=(?P<cpu>[-\d.]+)ms\s+tcpu=(?P<tcpu>-|[-\d.]+)ms\s+"
    r"ivcsw=(?P<ivcsw>-|\+\d+)\s+ivcsw_rate=(?P<rate>-|[\d.]+)/ms",
    re.M,
)


def resolve(arg):
    """A path, or a leg label looked up in this line's logs then the historical ones (newest match wins)."""
    if os.path.isfile(arg):
        return arg
    for d in reversed(LEG_DIRS):  # this line's directory first
        if not os.path.isdir(d):
            continue
        hits = [os.path.join(d, n) for n in os.listdir(d) if n.endswith(".log.stderr") and arg in n]
        if hits:
            return max(hits, key=os.path.getmtime)
    return None


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__.strip().split("usage:")[1].strip(), file=sys.stderr)
        return 2
    for arg in args:
        path = resolve(arg)
        if path is None:
            print(f"no leg matched {arg!r}", file=sys.stderr)
            continue
        with open(path, errors="replace") as fh:
            txt = fh.read()
        m = BASELINE.search(txt)
        baseline = float(m.group(1)) if m else None
        print(f"== {leg_label(os.path.basename(path))}")
        print(f"   leg baseline: {baseline if baseline is not None else '?'} involuntary switch(es)/ms")
        rows = []
        for ev in EVENT.finditer(txt):
            wall = ev.group("wall")
            # The report prints UTC; the system log is read in LOCAL time, so the conversion is done here
            # rather than in the reader's head (a one-hour mistake in a timestamp hunt is expensive).
            try:
                local = datetime.strptime(wall, "%Y-%m-%dT%H:%M:%S.%f").replace(tzinfo=timezone.utc).astimezone()
                local_s = local.strftime("%H:%M:%S.") + f"{local.microsecond // 1000:03d}"
            except ValueError:
                local_s = "?"
            rate = None if ev.group("rate") == "-" else float(ev.group("rate"))
            cpu = float(ev.group("cpu"))
            win = float(ev.group("value")) if ev.group("value") else 0.0
            ratio = (rate / baseline) if (rate is not None and baseline) else None
            if ratio is not None and ratio < 0.2:
                verdict = "STALLED"
            elif win > 0 and cpu < 0.3 * win / 1000.0:
                verdict = "NO-CPU"
            else:
                verdict = "BUSY"
            # The VERDICT RIDES IN THE TUPLE. The first version computed it here and printed a stale `verdict`
            # from the end of THIS loop, so every row showed the last event's classification - caught by the
            # self-contradiction of a row reading `ratio 1.03 STALLED` (dev doc 10.25).
            rows.append((win, ev.group("kind"), ev.group("rank"), cpu, ev.group("tcpu"), rate, ratio,
                         ev.group("load1"), local_s, verdict))
        if not rows:
            print("   no timing events kept (the knob was off, or nothing crossed a floor)")
            continue
        print(f"   {'series':6s} {'window':>9s} {'cpu':>8s} {'tcpu':>8s} {'ivcsw/ms':>9s} {'/baseline':>9s} "
              f"{'load1':>6s}  {'local time':>13s}  verdict")
        for win, kind, rank, cpu, tcpu, rate, ratio, load1, local_s, verdict in sorted(rows, reverse=True):
            print(f"   {kind + '#' + rank:6s} {win:8.0f}us {cpu:7.2f}ms {tcpu:>8s} "
                  f"{(f'{rate:.2f}' if rate is not None else '-'):>9s} "
                  f"{(f'{ratio:.2f}' if ratio is not None else '-'):>9s} "
                  f"{(load1 if load1 else '-'):>6s}  {local_s:>13s}  {verdict}")
        print("   (STALLED = the process was not being run: the cause is outside the gNB - chase it with `log show`)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
