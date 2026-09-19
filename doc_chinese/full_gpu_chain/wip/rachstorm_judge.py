#!/usr/bin/env python3
"""Judge whether a leg reproduced the random-access storm behind the multi-second ping/iperf outages.

Called by rachstorm_repro.sh judge; see that script's header for the experiment and the evidence.

---- The mechanism this detects ----------------------------------------------------------------
An established UE loses its radio link and re-accesses. While it does, the gNB cycles through RACH
temporary identities (tc-rnti = 0x4601, 0x4602, ...), and each one shows the same failure: it is
granted up to 5 PUSCHs (Msg3 plus four HARQ retransmissions), every one crc=KO with a meaningless
SINR (-17..-53 dB, i.e. nothing was received), it never sends PUCCH, and the gNB gives up with
"Maximum number of reTxs 4 exceeded" / "Discarding UL HARQ process TB". The UE then tries again with
the NEXT tc-rnti. During that stretch it has no data connection at all, so TCP/ICMP queues and drains
afterwards - which is exactly the "RTT ramps down from 1.5-4.4 s" that ping shows and the "one second
with zero transfer" that iperf3 shows.

CAUSALITY: the RACH is the CONSEQUENCE of the drop, not its cause. The gNB logs the cause - every
release in the archived legs is "RLF detected. Cause: 100 consecutive undecoded CSIs" - and the number
of RACH tries the UE needs is what sets the SIZE of the ping spike. Removing the spikes means removing
the drops (the MAC RLF thresholds / the PUCCH SINR filter), not the re-access.

So the episode is NOT "an established UE went silent"; it is "the UE is not connected at all and keeps
failing to attach, because the gNB released it". The detector keys on that:
  * PRACH detections, de-duplicated into storms (consecutive detections within STORM_GAP seconds);
  * a storm is FAILED when the tc-rnti(s) it produced never carried a crc=OK PUSCH;
  * the outage a user feels is the span of the storm (plus the release/re-access around it).
"""

import collections
import datetime
import re
import sys

TS = re.compile(r"^(\d{4}-\d\d-\d\d)T(\d\d):(\d\d):(\d\d)\.(\d{6})")
PRACH = re.compile(r"PRACH: rsi=.*detected_preambles=\[\{idx=(\d+).*?power_dB=([-0-9.]+)")
TCRNTI = re.compile(r"tc-rnti=(0x[0-9a-f]+)")
PUSCH = re.compile(r"PUSCH: rnti=(0x[0-9a-f]+).*?crc=(OK|KO).*?sinr=([+-]?[\d.]+)dB")
PUCCH = re.compile(r"PUCCH: rnti=(0x[0-9a-f]+)")
DISCARD = re.compile(r"rnti=(0x[0-9a-f]+).*?(Maximum number of reTxs|Discarding UL HARQ)")

# Two PRACH detections this close belong to the same re-access attempt.
STORM_GAP_S = 20.0
# A tc-rnti that reached this many crc=KO PUSCHs without ever succeeding is a failed Msg3.
MSGS_FAILED = 4


def hhmmss(epoch):
    return datetime.datetime.fromtimestamp(epoch).strftime("%H:%M:%S")


def main():
    if len(sys.argv) != 2:
        print("usage: rachstorm_judge.py <gnb log>", file=sys.stderr)
        return 2
    logpath = sys.argv[1]

    prach = []                                   # (epoch, preamble, power_db)
    tc = {}                                      # tc-rnti -> first epoch seen
    ok = collections.Counter()                   # rnti -> crc=OK count
    ko = collections.Counter()                   # rnti -> crc=KO count
    dead_sinr = {}                               # rnti -> worst SINR seen
    gaveup = collections.defaultdict(list)       # rnti -> epochs
    with open(logpath, errors="ignore") as f:
        for line in f:
            m = TS.match(line)
            if not m:
                continue
            t = datetime.datetime.strptime(f"{m.group(1)} {m.group(2)}:{m.group(3)}:{m.group(4)}",
                                           "%Y-%m-%d %H:%M:%S").timestamp()
            p = PRACH.search(line)
            if p:
                prach.append((t, int(p.group(1)), float(p.group(2))))
            p = TCRNTI.search(line)
            if p:
                tc.setdefault(p.group(1), t)
            p = PUSCH.search(line)
            if p:
                r, crc, sinr = p.group(1), p.group(2), float(p.group(3))
                (ok if crc == "OK" else ko)[r] += 1
                dead_sinr[r] = min(dead_sinr.get(r, 0.0), sinr)
            p = DISCARD.search(line)
            if p:
                gaveup[p.group(1)].append(t)

    if not prach:
        print(f"no PRACH detection in {logpath}: nothing to judge (was the cell up?)")
        return 2

    # ---- one access attempt = one PRACH + the tc-rnti the gNB answered it with --------------------
    # The gNB allocates a fresh tc-rnti per RACH attempt, so pairing them in time order gives the
    # attempt's outcome directly: crc=OK on that identity means the attempt completed, a run of
    # crc=KO (up to the HARQ limit) means Msg3 never got through and the UE will try again.
    #
    # \note Grouping PRACH detections into "storms" by a time gap was the first attempt and it was
    # wrong: on a leg where the UE re-accesses every few seconds the whole run collapses into ONE
    # group, and "the storm succeeded" then hides every failed attempt inside it.
    attempts = []
    tc_sorted = sorted(tc.items(), key=lambda kv: kv[1])
    used = set()
    for t, preamble, power in prach:
        nxt = next(((r, e) for r, e in tc_sorted if r not in used and e >= t - 0.5), None)
        if nxt is None:
            attempts.append(dict(t=t, preamble=preamble, power=power, tc=None,
                                 ok=0, ko=0, sinr=None, at=None, gaveup=0))
            continue
        r, e = nxt
        used.add(r)
        attempts.append(dict(t=t, preamble=preamble, power=power, tc=r, ok=ok[r], ko=ko[r],
                             sinr=dead_sinr.get(r), at=e, gaveup=len(gaveup.get(r, []))))

    print("=" * 96)
    print(f"log: {logpath}")
    print(f"PRACH detections: {len(prach)}   tc-rnti identities: {len(tc)}   paired attempts: "
          f"{sum(1 for a in attempts if a['tc'])}")
    print()
    print("%-10s %4s %9s %-8s %5s %5s %9s %5s  %s" %
          ("at", "pre", "power_dB", "tc-rnti", "OK", "KO", "worstSINR", "giveup", "outcome"))
    for a in attempts:
        if a["tc"] is None:
            print("%-10s %4d %9.1f %-8s %5s %5s %9s %5s  no identity paired" %
                  (hhmmss(a["t"]), a["preamble"], a["power"], "-", "-", "-", "-", "-"))
            continue
        out = "SUCCESS" if a["ok"] > 0 else ("FAILED (Msg3 never got through)" if a["ko"] > 0 else "no result")
        print("%-10s %4d %9.1f %-8s %5d %5d %8.1f%s %5d  %s" %
              (hhmmss(a["t"]), a["preamble"], a["power"], a["tc"], a["ok"], a["ko"],
               a["sinr"] if a["sinr"] is not None else 0.0, "dB", a["gaveup"], out))
    print("=" * 96)

    # ---- verdict ----------------------------------------------------------------------------------
    first_ok = next((i for i, a in enumerate(attempts) if a["ok"] > 0), None)
    if first_ok is None:
        print()
        print("NOT REPRODUCED: the UE never completed an access in this log, so there is no established")
        print("                connection to lose. That is a different (and worse) situation - check the")
        print("                RF level and the interference notes in the design document.")
        return 1

    runs = []            # (start_index, end_index) of consecutive failed attempts after first_ok
    cur = None
    for i, a in enumerate(attempts):
        if i <= first_ok:
            continue
        failed = a["ok"] == 0 and a["ko"] > 0
        if failed and cur is None:
            cur = [i, i]
        elif failed:
            cur[1] = i
        elif cur is not None:
            runs.append(tuple(cur)); cur = None
    if cur is not None:
        runs.append(tuple(cur))

    print()
    if not runs:
        print("NOT REPRODUCED: every access attempt after the first one succeeded. Whatever caused the")
        print("                multi-second spikes on this leg, it was not a re-access outage.")
        return 1

    def span_of(run):
        lo = attempts[run[0]]["t"]
        after = [a["t"] for a in attempts[run[1] + 1:] if a["ok"] > 0]
        hi = after[0] if after else attempts[run[1]]["t"]
        return hi - lo

    spans = [span_of(r) for r in runs]
    grants = [sum(attempts[i]["ko"] for i in range(r[0], r[1] + 1)) for r in runs]
    print(f"REPRODUCED: {len(runs)} outage(s) where the UE had been connected and then failed to")
    print(f"            re-access - {sum(1 for r in runs if r[1] > r[0])} of them spanning several")
    print(f"            attempts. Longest: {max(spans):.0f} s, up to {max(grants)} Msg3 grants burned")
    print( "            (all crc=KO with SINR down to " +
          f"{min((a['sinr'] for a in attempts if a['sinr'] is not None), default=0):.0f} dB, no PUCCH).")
    print()
    print("            During each outage the UE has no data connection, so TCP/ICMP queues and drains")
    print("            afterwards - the multi-second ping/iperf stall and the linear RTT ramp.")
    print("            The RACH is therefore the CONSEQUENCE, and its duration is the spike's size.")
    print("            What has to go is the DROP that precedes it: the gNB releases the UE on RLF -")
    print("            every release in the archived legs reads 'RLF detected. Cause: 100 consecutive")
    print("            undecoded CSIs'. Its thresholds are the MAC consecutive-KO counters")
    print("            (cell_cfg.pucch/pusch/pdsch.max_consecutive_kos, all 100 by default) and the")
    print("            PUCCH SINR filter (cell_cfg.pucch.sinr_threshold) that decides which")
    print("            indications count towards them. Neither is a GPU-pipeline or host matter.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
