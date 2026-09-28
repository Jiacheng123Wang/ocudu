#!/usr/bin/env python3
"""The transport-stall readings of a set of legs, side by side - the ruler for the 6.150 experiments.

WHY IT EXISTS. The slow-receive tail is invisible in every series the acceptance criteria read: it lives beyond
p99 of [ul_rx_wait], it moves V1's max and not its median, and the per-call counters that DO show it are three
different lines ([ul_rx_timing]'s recv, [dl_tx_call]'s over-1ms, and the pulse of [ul_rx] which must stay 0). The
experiments of dev doc 6.150 (5) compare a ladder of legs that differ ONLY in which modules are offloaded, so the
table has to make those three lines comparable at a glance - and it has to print the mode/module set the leg
actually ran with, because that is the variable.

READ IT LIKE THIS:
  * `recv>1ms%`  - share of receive calls that blocked more than a millisecond. Reference readings: an all-CPU PHY
                   reads 0.002-0.003%, any Metal path in the PHY reads 0.058-0.171%; the rate is flat across
                   traffic (0.098% at 3439 hops vs 0.098-0.102% at 145421) and across the 1-in-8 ablation arms.
  * `dl>1ms`     - the SAME stall class on the transmit side. In cpu mode it is 1 (the start-up call); with a
                   Metal path it is 441-598. A leg where one direction moves and the other does not is a
                   different finding from one where both do.
  * `rx_max`     - the largest steady-state wait (the start-up sample is excluded from the distribution since
                   6.147, so this is a link quantity).
  * `gap/ovf`    - MUST be 0. If they are not, the leg lost samples and the stall readings above are not a
                   transport-latency story any more.

usage: python3 stall_rate.py <leg .stderr | leg label> [more legs ...]
"""
import glob
import os
import re
import sys

LOGDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "phy_pipeline_gpu", "wip", "logs")


def resolve(arg):
    if os.path.isfile(arg):
        return arg
    hits = sorted(glob.glob(os.path.join(LOGDIR, "gnb_*%s*.log.stderr" % arg)), key=os.path.getmtime, reverse=True)
    return hits[0] if hits else None


def field(text, pattern, group=1, cast=float, default=None):
    """The LAST occurrence, never the first.

    A leg's stderr can carry MORE THAN ONE report block: a p0 dump takes a mid-leg snapshot of the same counters
    ("p0 dump #1 (dry-pool drop): the readings below are a SNAPSHOT taken while the leg was running"), so reading
    the first match reads a moment in the middle of the leg. Measured 2026-09-28 on p94-cg-all-light: the dump at
    ~31 s printed `[ul_rx_timing] calls=62253 ... over 1ms=3`, the shutdown report printed `calls=582715 ...
    over 1ms=649` - 0.001% against the leg's real 0.111%, a 100x error in the very column these experiments are
    about. `tail -1` is the convention the gates already use; this is the same rule in Python.

    re.M: the provenance block is one line per field inside a multi-line banner, so "^pipeline mode" only ever
    matches with MULTILINE on.
    """
    hits = list(re.finditer(pattern, text, re.M))
    return cast(hits[-1].group(group)) if hits else default


def dumps(text):
    """How many times the counters were printed: 1 = the shutdown report, >1 = a mid-leg p0 dump happened too."""
    return len(re.findall(r"p0 dump #\d+", text)) + 1


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    hdr = ("leg", "mode", "modules offloaded", "hops", "calls", "recv>1ms", "recv>1ms%", ">5ms", "dl>1ms",
           "rx_max us", "gap/ovf", "blocks")
    print("%-30s %-8s %-34s %8s %9s %8s %8s %6s %7s %9s %9s %7s" % hdr)
    n_missing = 0
    for arg in argv:
        path = resolve(arg)
        if path is None:
            print("%-30s <no leg matches>" % arg[:30])
            n_missing += 1
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        mode = field(text, r"^pipeline mode : (\S+)", 1, str, "?")
        mods = field(text, r"^mode options  : (.*)$", 1, str, "")
        mods = "<none: the mode resolves them>" if mods.strip() in ("", "<none>") else mods.strip()
        # The module set is the variable of the ladder: shorten it to the modules that are actually offloaded.
        off = [m for m in ("dft_type", "estimator_algo", "equalizer_backend", "resource_grid") if m in mods]
        mods = "+".join(o.replace("_type", "").replace("_algo", "").replace("_backend", "") for o in off) or "none"
        hops = field(text, r"\[ul_gpu_lane\] lanes=(\d+)", 1, int, None)
        if hops is None:
            hops = field(text, r"\[ul_pipeline\] samples=(\d+)", 1, int, 0)
        calls = field(text, r"\[ul_rx_timing\] calls=(\d+)", 1, int, 0)
        r1 = field(text, r"\[ul_rx_timing\] calls=\d+ recv\(max=\d+us over 1ms=(\d+)", 1, int, None)
        r5 = field(text, r"recv\(max=\d+us over 1ms=\d+ over 5ms=(\d+)\)", 1, int, None)
        dl = field(text, r"\[dl_tx_call\].*over 1ms=(\d+)", 1, int, None)
        rxmax = field(text, r"\[ul_rx_wait\] samples=\d+ mean=[\d.]+us median=[\d.]+us min=[\d.]+us max=([\d.]+)us", 1, float, 0.0)
        gaps = field(text, r"\[ul_rx\] blocks=\d+ samples=\d+ gaps=(\d+)", 1, int, None)
        ovf = field(text, r"gaps=\d+ gap_samples=\d+ ts0_blocks=\d+ rx_overflows=(\d+)", 1, int, None)
        leg = os.path.basename(path).replace("gnb_", "").replace(".log.stderr", "")[:30]
        pct = ("%.3f" % (100.0 * r1 / calls)) if (r1 is not None and calls) else "?"
        print("%-30s %-8s %-34s %8s %9s %8s %8s %6s %7s %9.0f %9s %7d" %
              (leg, mode, mods, hops, calls,
               "?" if r1 is None else r1, pct, "?" if r5 is None else r5,
               "?" if dl is None else dl, rxmax,
               "%s/%s" % ("?" if gaps is None else gaps, "?" if ovf is None else ovf),
               dumps(text)))
        if dumps(text) > 1:
            print("%-30s   note: %d report blocks - the columns above are the LAST one (a mid-leg p0 dump came"
                  " first)" % ("", dumps(text)))
        if (gaps not in (None, 0)) or (ovf not in (None, 0)):
            print("%-30s   !! gaps/overflows are not 0: this leg lost samples, read the stall columns with care"
                  % "")
    print()
    print("reference: cpu 0.002-0.003% | cpu_gpu(all) 0.118% | gpu 0.058-0.171% | flat across traffic and ablation")
    return 1 if n_missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
