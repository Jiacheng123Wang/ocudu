#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""
Side-by-side of the readings a V3 (radio RF failure) reproduction leg must match.

WHY THIS EXISTS. V3 (RF failures <= 10; the all-CPU route reads 0-1) is the one criterion the
fused-lane work left unmet, and it is parked as a radio/transport item (dev doc 6.40-6.43).
Before touching any parameter we re-fly the RECORDED recipes (cpu / cpu_gpu / gpu) on today's
code and ask whether the phenomenon still reproduces. "Reproduces" has to be a comparison
against the historical legs' own numbers, one line per reading, not a judgement by eye -
the RF count alone moves with the link (700-1500 on the same recipe), so the SHAPE and the
probe signatures are what a verdict may rest on.

The script also makes the two questions that need answering explicit in its output:
  * the mode contrast (cpu vs gpu on the same day, same load) - the "GPU-mode disease";
  * the transport question: is the lateness INSIDE the transmit()/receive() call (transport)
    or is the calling thread starved (host scheduling)? [dl_tx_call] and [ul_rx_timing]
    separate them by construction (dev doc 6.42 4).

usage: python3 repro_compare.py <leg|path> [<leg|path> ...]
       python3 repro_compare.py p42 p60 s79-dlcap40-cpu-n78
A leg is looked up in wip/logs as gnb_{cpu,cpu_gpu,gpu}_<leg>*.{log,log.stderr}.
Exit: 0 both files found for every leg; 2 one is missing (NOT evidence).
"""

import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(HERE, "logs")

# (label, regex on the .stderr, which group is the value) - one row of the output table.
PROBES = [
    ("ul_pipeline", r"\[ul_pipeline\] samples=(\d+) mean=([\d.]+)us median=([\d.]+)us", "s/mean/med"),
    ("ul_pipeline stale", r"\[ul_pipeline\] stale=(\d+)", "count"),
    ("ul_gpu_pipeline", r"\[ul_gpu_pipeline\] samples=(\d+) mean=([\d.]+)us median=([\d.]+)us", "s/mean/med"),
    ("ul_gpu_pipeline stale", r"\[ul_gpu_pipeline\] stale=(\d+)", "count"),
    ("ul_rx_wait", r"\[ul_rx_wait\] samples=(\d+) mean=([\d.]+)us median=([\d.]+)us min=([\d.]+)us max=([\d.]+)us", "s/mean/med/max"),
    ("ul_rx_timing", r"(\[ul_rx_timing\][^\n]*)", "text"),
    ("dl_tx_slack", r"(\[dl_tx_slack\] transmissions=\d+[^\n]*)", "text"),
    ("dl_tx_call", r"(\[dl_tx_call\] calls=\d+[^\n]*)", "text"),
    ("ul_rx gaps", r"(\[ul_rx\] blocks=\d+ samples=\d+ gaps=\d+[^\n]*)", "text"),
    ("ul_rx_pool", r"(\[ul_rx_pool\] pop_blocking wait[^\n]*)", "text"),
    ("ul_gpu_lane", r"(\[ul_gpu_lane\] lanes=\d+ cbs/lane=[\d.]+ \(max=\d+\)[^\n]*)", "text"),
    ("lane residency", r"\[ul_gpu_lane\] residency samples=(\d+) mean=([\d.]+)us median=([\d.]+)us", "s/mean/med"),
    ("contract", r"(\[phy_pipeline\] contract MET[^\n]*)", "text"),
]


def find(leg):
    """Returns (log, stderr) paths for a leg label or an explicit path."""
    if os.path.exists(leg) or os.path.exists(leg + ".log"):
        base = leg[:-4] if leg.endswith(".log") else leg
        return base + ".log", base + ".log.stderr"
    hits = []
    for mode in ("cpu", "cpu_gpu", "gpu"):
        hits += glob.glob(os.path.join(LOGDIR, "gnb_%s_%s*.log" % (mode, leg)))
    hits = [h for h in hits if not h.endswith(".stderr") and not h.endswith(".stdout")]
    if not hits:
        return None, None
    newest = max(hits, key=os.path.getmtime)
    return newest, newest + ".stderr"


def read(path):
    try:
        with open(path, errors="ignore") as f:
            return f.read()
    except OSError:
        return ""


def rf_failures(log_text):
    """The radio's own real-time failure lines, by kind (UHD's message)."""
    kinds = {}
    for m in re.finditer(r"Real-time failure in RF: (\w+)", log_text):
        kinds[m.group(1)] = kinds.get(m.group(1), 0) + 1
    return kinds


def provenance(stderr_text):
    keep = ("regime", "pipeline mode", "cell config", "gNB options", "knob")
    out = []
    for line in stderr_text.splitlines()[:12]:
        s = line.strip()
        if any(s.startswith(k) or s.startswith("[leg] " + k) for k in keep):
            out.append(s)
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    legs = []
    missing = 0
    for arg in argv[1:]:
        log, err = find(arg)
        if log is None:
            print("no leg matched '%s' in %s" % (arg, LOGDIR), file=sys.stderr)
            missing += 1
            continue
        legs.append((arg, log, err))

    for name, log, err in legs:
        stderr_text = read(err)
        kinds = rf_failures(read(log))
        total = sum(kinds.values())
        print("=" * 100)
        print("%s   [%s]" % (name, os.path.basename(log)))
        for p in provenance(stderr_text):
            print("   %s" % p)
        print("   RF failures: %d  %s" % (total, dict(sorted(kinds.items())) if kinds else ""))
        for label, pattern, how in PROBES:
            m = re.search(pattern, stderr_text)
            if not m:
                print("   %-22s <absent>" % label)
                continue
            if how == "text":
                print("   %-22s %s" % (label, m.group(1).strip()))
            else:
                parts = m.groups()
                if how == "count":
                    print("   %-22s %s" % (label, parts[0]))
                else:
                    print("   %-22s samples=%s mean=%s median=%s%s" % (
                        label, parts[0], parts[1], parts[2],
                        (" max=%s" % parts[4]) if how.endswith("max") and len(parts) > 4 else ""))
        # The one derived number a verdict needs: failures per DL transmission.
        slack = re.search(r"\[dl_tx_slack\] transmissions=(\d+)", stderr_text)
        if slack and total:
            t = int(slack.group(1))
            print("   => RF failures per DL transmission: %.4f%%  (%d / %d)" % (100.0 * total / t, total, t))
    return 2 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
