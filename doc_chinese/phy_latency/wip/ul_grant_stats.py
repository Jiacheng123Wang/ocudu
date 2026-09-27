#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# WHAT AN UPLINK LEG ACTUALLY DELIVERED - read it from the gNB's own log, per modulation.
#
# WHY IT EXISTS (2026-09-27). Two iperf3 runs, same phone, same recipe, only the gNB config different:
# n78 20 MHz TDD gave 7.24 Mbit/s and n1 5 MHz FDD gave 16.7 - the SMALLER cell 2.3x faster. Bandwidth was
# not the answer, and the log already contained the answer: 44.8% of the PUSCH transmissions failed CRC and
# 80.4% of the 256QAM ones did, while the scheduler kept choosing 256QAM at a 19 dB median SINR. This script
# turns that from an afternoon of one-off greps into a repeatable reading, because the fix candidates
# (wip/mk_arm_cfg.sh's rv / mcs19 / qam64 / p0up arms) have to be judged by the SAME ruler.
#
# WHAT IT READS (the two line formats it depends on; both are at all_level: info):
#   SCHED: Slot decisions pci=1 ... UL: ue=.. rnti=.. h_id=.. ss_id=.. rb=[a..b) newtx=true rv=0 tbs=N
#   PHY:   PUSCH: rnti=.. harq_id=.. prb=[a, b) symb=[..) mod=256QAM rv=0 tbs=N crc=OK|KO iter=6.0 sinr=..dB
#
# WHAT IT PRINTS, per log: the window and rates two ways, and per modulation the hops, BLER, bytes
# successfully decoded per grant and the EFFECTIVE bits/RE - success bytes divided by the REs of EVERY grant
# of that modulation (so a modulation that is fast when it works and fails 80% of the time reads as the slow
# one it is).
#
# ★ WHY THERE ARE TWO WINDOWS (learned the hard way, 2026-09-27). The first version measured from the first
# to the last UL grant in the log. That window is NOT the test: a run whose phone kept a little background
# uplink before and after iperf3 read "granted 6.24 Mbit/s" against another run's 16.42, which looks like a
# 2.6x regression and is really a 476 s window holding a 240 s test. So both are printed now: the FULL
# window (with its length, so dilution is visible) and the BUSIEST <window> s - the densest span of that
# length, which is the test itself. Compare arms on the busiest window; use the ratio readings (retx share,
# BLER, effective bit/RE) from either, since they do not depend on the window.
#
# usage: python3 ul_grant_stats.py /tmp/gnb_n78_ul_ab.log [/tmp/gnb_n1_ul_ab.log ...]
#        python3 ul_grant_stats.py --window=120 <logs...>
#        The runs must be kept apart by --log.filename; the default /tmp/gnb.log overwrites.

import collections
import datetime
import os
import re
import statistics
import sys

BUSY_WINDOW_S = 240.0   # the iperf3 test length used by every arm; override with --window=<s>
TCP_MBPS = None         # iperf3's receiver Mbit/s, passed with --tcp=<n>: prints the MAC -> TCP survival
                        # (2026-09-27: that ratio is what separated a +10.6% arm from a +34% one - the MAC
                        # side cannot see TBs that are dropped below TCP, so it needs the TCP number)

SCHED_RX = re.compile(
    r"^(\S+) .*?Slot decisions pci=\d+ .*?UL: ue=\S+ rnti=\S+ h_id=(\d+) ss_id=(\d+) "
    r"rb=\[(\d+)\.\.(\d+)\) (newtx=\w+) rv=(\d+) tbs=(\d+)")
PHY_RX = re.compile(
    r"PUSCH: rnti=\S+ harq_id=(\d+) prb=\[(\d+), (\d+)\) symb=\[(\d+), (\d+)\) mod=(\w+) rv=(\d+) "
    r"tbs=(\d+) crc=(\w+) iter=([0-9.]+) sinr=([-0-9.a-zA-Z]+)dB")

RE_PER_PRB = 12 * 14   # one slot's resource elements per PRB


def analyse(path):
    grants = []            # (ts, prb_width, is_newtx, rv, tbs)
    phy = collections.defaultdict(lambda: dict(n=0, ok=0, okbytes=0, re=0, sinr=[], ko_sinr=[]))
    first = last = None
    with open(path, errors="replace") as fh:
        for line in fh:
            if "Slot decisions" in line and "UL: ue=" in line:
                m = SCHED_RX.match(line)
                if m:
                    ts, _hid, _ss, rb0, rb1, newtx, rv, tbs = m.groups()
                    when = datetime.datetime.fromisoformat(ts)   # datetime, so the busy-window scan can subtract
                    grants.append((when, int(rb1) - int(rb0), newtx == "newtx=true", int(rv), int(tbs)))
                    first = first or when
                    last = when
            elif "PUSCH: rnti=" in line:
                m = PHY_RX.search(line)
                if m:
                    _hid, p0, p1, _s0, _s1, mod, rv, tbs, crc, it, snr = m.groups()
                    d = phy[mod]
                    prb = int(p1) - int(p0)
                    d["n"] += 1
                    d["re"] += prb * RE_PER_PRB
                    if crc == "OK":
                        d["ok"] += 1
                        d["okbytes"] += int(tbs)
                    try:
                        v = float(snr)
                    except ValueError:
                        v = None
                    if v is not None:
                        (d["sinr"] if crc == "OK" else d["ko_sinr"]).append(v)
    return grants, phy, first, last


def busiest_window(grants, length_s):
    """The densest span of `length_s` seconds: two pointers over the (time-ordered) grant list. Returns
    (i, j, span_s) or None. This is what makes arms comparable when a log's own window is diluted by
    background uplink before/after the test - see the header."""
    if not grants:
        return None
    best = None
    j = 0
    for i in range(len(grants)):
        while j < len(grants) and (grants[j][0] - grants[i][0]).total_seconds() <= length_s:
            j += 1
        if best is None or (j - i) > best[0]:
            best = (j - i, i, j)
    n, i, j = best
    span = (grants[j - 1][0] - grants[i][0]).total_seconds()
    return (i, j, span) if span > 0 else None


def window_rates(grants, i, j, span):
    seg = grants[i:j]
    tot = sum(g[4] for g in seg)
    newb = sum(g[4] for g in seg if g[2])
    ntx = sum(1 for g in seg if g[2])
    return dict(rate=len(seg) / span, granted_mbps=tot * 8 / span / 1e6,
                newdata_mbps=newb * 8 / span / 1e6, retx_pct=100 * (len(seg) - ntx) / len(seg),
                avg_tb=sum(g[4] for g in seg) / len(seg), span=span)


def summarise(path):
    grants, phy, first, last = analyse(path)
    out = {"file": os.path.basename(path), "grants": len(grants), "phy": phy}
    if not grants or first is None:
        out["error"] = "no UL grant line found (is this the right log? does it carry SCHED at info level?)"
        return out
    dur = (last - first).total_seconds()
    tot = sum(g[4] for g in grants)
    newb = sum(g[4] for g in grants if g[2])
    newtx = sum(1 for g in grants if g[2])
    out.update(dur=dur, rate=len(grants) / dur, granted_mbps=tot * 8 / dur / 1e6,
               newdata_mbps=newb * 8 / dur / 1e6, retx_pct=100 * (len(grants) - newtx) / len(grants))
    bw = busiest_window(grants, BUSY_WINDOW_S)
    if bw is not None:
        out["busy"] = window_rates(grants, *bw)
    all_re = sum(d["re"] for d in phy.values())
    ok_b = sum(d["okbytes"] for d in phy.values())
    out["eff_bit_re"] = (ok_b * 8 / all_re) if all_re else float("nan")
    out["bler_pct"] = 100 * (sum(d["n"] for d in phy.values()) - sum(d["ok"] for d in phy.values())) / \
        max(1, sum(d["n"] for d in phy.values()))
    return out


def main(argv):
    global BUSY_WINDOW_S, TCP_MBPS
    argv = list(argv)
    args = [a for a in argv[1:] if not a.startswith("--")]
    for a in argv[1:]:
        if a.startswith("--window="):
            BUSY_WINDOW_S = float(a.split("=", 1)[1])
        elif a.startswith("--tcp="):
            TCP_MBPS = float(a.split("=", 1)[1])
    argv = [argv[0]] + args
    if len(argv) < 2:
        print(__doc__ or "usage: ul_grant_stats.py <gnb log> [more logs...]", file=sys.stderr)
        return 2
    results = [summarise(p) for p in argv[1:]]
    for r in results:
        print(f"=== {r['file']}")
        if "error" in r:
            print(f"    {r['error']}")
            continue
        print(f"    full window {r['dur']:.1f} s, {r['grants']} UL grants = {r['rate']:.1f} grants/s")
        print(f"      granted {r['granted_mbps']:.2f} Mbit/s | new data {r['newdata_mbps']:.2f} Mbit/s | "
              f"retransmissions {r['retx_pct']:.1f}%  (diluted if the window is longer than the test)")
        if TCP_MBPS is not None and "busy" in r and r["busy"]["newdata_mbps"]:
            surv = 100.0 * TCP_MBPS / r["busy"]["newdata_mbps"]
            print(f"    ★ MAC new data -> TCP survival: {surv:.0f}%  ({TCP_MBPS} Mbit/s received / "
                  f"{r['busy']['newdata_mbps']:.2f} Mbit/s of MAC new data; the rest was dropped under TCP)")
        if "busy" in r:
            b = r["busy"]
            print(f"    ★ busiest {b['span']:.0f} s (the test): {b['rate']:.1f} grants/s | "
                  f"granted {b['granted_mbps']:.2f} Mbit/s | new data {b['newdata_mbps']:.2f} Mbit/s | "
                  f"retx {b['retx_pct']:.1f}% | avg TB {b['avg_tb']:.0f} B/grant")
        print(f"    CRC BLER {r['bler_pct']:.1f}% | effective {r['eff_bit_re']:.2f} bit/RE")
        print(f"    {'mod':>7} {'hops':>7} {'BLER%':>6} {'ok bytes/grant':>15} {'eff bit/RE':>11} {'SINR p50':>9}")
        for mod, d in sorted(r["phy"].items(), key=lambda kv: -kv[1]["n"]):
            if d["n"] == 0:
                continue
            eff = d["okbytes"] * 8 / d["re"] if d["re"] else float("nan")
            p50 = statistics.median(d["sinr"]) if d["sinr"] else float("nan")
            print(f"    {mod:>7} {d['n']:>7} {100*(d['n']-d['ok'])/d['n']:>6.1f} "
                  f"{d['okbytes']/d['n']:>15.0f} {eff:>11.2f} {p50:>9.1f}")
    if len(results) > 1:
        print("\n=== comparison (the whole point: same ruler for every arm)")
        print(f"    (busiest {BUSY_WINDOW_S:.0f} s = the test; ratios are window-independent)")
        print(f"    {'log':>34} {'grants/s':>9} {'granted':>8} {'newdata':>8} {'TB B':>6} {'retx%':>6} "
              f"{'BLER%':>6} {'bit/RE':>7}")
        for r in results:
            if "error" in r:
                continue
            b = r.get("busy", r)
            print(f"    {r['file']:>34} {b['rate']:>9.1f} {b['granted_mbps']:>8.2f} {b['newdata_mbps']:>8.2f} "
                  f"{b['avg_tb']:>6.0f} {r['retx_pct']:>6.1f} {r['bler_pct']:>6.1f} {r['eff_bit_re']:>7.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
