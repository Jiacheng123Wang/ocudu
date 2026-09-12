#!/usr/bin/env python3
"""Summarize a gNB log (log.all_level: debug) into a link-health report.

Single pass over the log, one line per 10 s of run time, plus per-modulation and
per-direction summaries.  It exists to answer the questions that the aggregate
`[ul_*]` metrics cannot: which modulation order fails, whether the uplink CRC
failures reach the MAC, how often the downlink HARQ-ACK is not even detected
(DTX), and how much MCS the outer loops are asking for while doing so.

Usage:  scripts/gnb_log_stats.py /tmp/gnb.log [--bucket 10] [--rnti 0x460c]
"""

import argparse
import collections
import re
import statistics
import sys

TS = re.compile(r"^(\d{4}-\d\d-\d\dT(\d\d):(\d\d):(\d\d\.\d+))")
PUSCH = re.compile(
    r"PUSCH: rnti=(\S+) harq_id=(\d+) prb=\[(\d+), (\d+)\) symb=\[(\d+), (\d+)\) "
    r"mod=(\S+) rv=(\d+) tbs=(\d+) crc=(\w+) iter=([\d.]+) sinr=(-?[\d.]+)dB "
    r"epre=(-?[\d.]+)dB rsrp=(-?[\d.]+)dB t_align=(-?[\d.]+)us t=([\d.]+)us")
PUCCH = re.compile(
    r"PUCCH: rnti=(\S+) format=(\d+) .*?ack=(\d+)?.*? sinr=(-?[\d.]+)dB")
UCI = re.compile(r"UCI\.indication .*?rnti=(\S+).*?harq_ack=(\w+)")
CRC_IND = re.compile(r"\- CRC: ue=(\d+) rnti=(\S+) rx_slot=(\S+) h_id=(\d+) crc=(\w+) sinr=(-?[\d.]+)dB")
DL_GRANT = re.compile(r"PDSCH rnti=(\S+) .*?CW: mod=(\S+) mcs_index=(\d+) mcs_table=(\d+) rv_idx=(\d+) tbs=(\d+)")
UL_GRANT = re.compile(
    r"PUSCH rnti=(\S+) .*?target_code_rate=(\d+) modulation=(\S+) mcs_index=(\d+) "
    r".*?CW: rv_idx=(\d+) harq_id=(\d+) new_data=(\w+) tbs=(\d+)")
PRACH = re.compile(r"PRACH: rsi=(\d+) rssi=(-?[\d.]+)dB")
RAR = re.compile(r"RAR PDSCH: ra-rnti=(\S+).*?grants \(\d+\): \[tc-rnti=(\S+):")
MSG3_GRANT_FAIL = re.compile(r"Failed to allocate PUSCH Msg3")
GRANT = re.compile(r"^\t")


def to_sec(hh, mm, ss):
    return int(hh) * 3600 + int(mm) * 60 + float(ss)


def pct(values, p):
    if not values:
        return float("nan")
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * p))]


def median(values):
    return statistics.median(values) if values else float("nan")


class Bucket:
    def __init__(self):
        self.pusch = []
        self.pucch_ack = collections.Counter()
        self.ul_crc = collections.Counter()
        self.dl_grant = []
        self.ul_grant = []
        self.ul_harq_discard = 0
        self.dl_harq_discard = 0
        self.prach = 0
        self.prach_rssi = []
        self.rar = 0
        self.msg3_grant_fail = 0
        self.dl_pdu = 0


def mod_ko(rows):
    per_mod = collections.defaultdict(lambda: [0, 0])
    for row in rows:
        entry = per_mod[row["mod"]]
        entry[0] += 1
        entry[1] += row["crc"] == "KO"
    return per_mod


def parse(path, bucket_seconds, rnti_filter):
    buckets = collections.defaultdict(Bucket)
    per_mod = collections.defaultdict(lambda: [0, 0, []])
    pusch_rows = []
    msg3_rntis = set()
    first = last = None
    commit = ""
    cur = None
    pusch_total = 0

    with open(path, errors="ignore") as handle:
        for line in handle:
            match = TS.match(line)
            if match:
                cur = to_sec(match.group(2), match.group(3), match.group(4))
                if first is None:
                    first = cur
                last = cur
            if cur is None:
                continue
            bucket = buckets[(cur - first) // bucket_seconds]
            if "Built in Release mode using commit" in line:
                commit = line.split("using commit")[1].split("on branch")[0].strip()
            elif "PUSCH: rnti=" in line:
                found = PUSCH.search(line)
                if found:
                    g = found.groups()
                    row = {
                        "t": cur, "rnti": g[0], "rv": int(g[7]), "mod": g[6],
                        "tbs": int(g[8]), "crc": g[9], "iter": float(g[10]),
                        "sinr": float(g[11]), "rsrp": float(g[13]), "lat": float(g[15]),
                    }
                    pusch_rows.append(row)
                    bucket.pusch.append(row)
                    pusch_total += 1
            elif "- CRC: ue=" in line:
                found = CRC_IND.search(line)
                if found:
                    bucket.ul_crc[found.group(5)] += 1
            elif "harq_ack=" in line:
                found = UCI.search(line)
                if found:
                    bucket.pucch_ack[found.group(2)] += 1
            elif "Discarding UL HARQ process" in line:
                bucket.ul_harq_discard += 1
            elif "Discarding DL HARQ process" in line:
                bucket.dl_harq_discard += 1
            elif "PRACH: rsi=" in line:
                found = PRACH.search(line)
                bucket.prach += 1
                if found:
                    bucket.prach_rssi.append(float(found.group(2)))
            elif "RAR PDSCH:" in line:
                found = RAR.search(line)
                if found:
                    bucket.rar += 1
                    msg3_rntis.add(found.group(2))
            elif MSG3_GRANT_FAIL.search(line):
                bucket.msg3_grant_fail += 1
            elif "DL PDU:" in line:
                bucket.dl_pdu += 1
            elif GRANT.match(line):
                found = DL_GRANT.search(line)
                if found:
                    g = found.groups()
                    bucket.dl_grant.append((g[0], g[1], int(g[2]), int(g[4]), int(g[5])))
                    continue
                found = UL_GRANT.search(line)
                if found:
                    g = found.groups()
                    bucket.ul_grant.append((g[0], g[2], int(g[3]), int(g[4]), g[6] == "true", int(g[7])))

    for row in pusch_rows:
        if rnti_filter and row["rnti"] != rnti_filter:
            continue
        entry = per_mod[row["mod"]]
        entry[0] += 1
        entry[1] += row["crc"] == "KO"
        entry[2].append(row["sinr"])
    return buckets, per_mod, pusch_rows, first, last, commit, pusch_total, msg3_rntis


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", default="/tmp/gnb.log")
    parser.add_argument("--bucket", type=int, default=10, help="bucket length in seconds")
    parser.add_argument("--rnti", default=None, help="focus the per-modulation table on one UE")
    args = parser.parse_args()

    buckets, per_mod, pusch_rows, first, last, commit, pusch_total, msg3_rntis = parse(
        args.log, args.bucket, args.rnti)
    if first is None:
        print("no timestamped lines found in %s" % args.log)
        return 1

    print("log      : %s" % args.log)
    print("commit   : %s" % (commit or "?"))
    print("duration : %.0f s   PUSCH results: %d" % (last - first, pusch_total))
    if args.rnti:
        print("focus    : rnti=%s (%d results, %d of them other RNTIs)"
              % (args.rnti, sum(e[0] for e in per_mod.values()),
                 pusch_total - sum(e[0] for e in per_mod.values())))
    print()

    # ---- per-bucket timeline -------------------------------------------------
    print("== timeline (%d s buckets)" % args.bucket)
    print("  t(s)  PUSCH  crcKO   ULCrc ok/ko  PUCCH ack/nack/dtx  PRACH  HARQd UL/DL  DLgrant mcs  ULgrant mcs(mod)  DLPdu")
    for key in sorted(buckets):
        b = buckets[key]
        ko = sum(1 for r in b.pusch if r["crc"] == "KO")
        pusch = "%4d %5.0f%%" % (len(b.pusch), 100 * ko / len(b.pusch)) if b.pusch else "   -     -"
        ulcrc = "%4d/%-4d" % (b.ul_crc["ok"], b.ul_crc["ko"])
        ack = "%4d/%4d/%4d" % (b.pucch_ack["ack"], b.pucch_ack["nack"], b.pucch_ack["dtx"])
        dl_mcs = "%4.0f" % median([g[2] for g in b.dl_grant]) if b.dl_grant else "   -"
        ul_mcs = "%4.0f" % median([g[2] for g in b.ul_grant]) if b.ul_grant else "   -"
        mods = collections.Counter(g[1] for g in b.ul_grant).most_common(1)
        print("%6d  %s  %s  %s  %5d  %5d/%1d  %s  %s %-10s %4d"
              % (key * args.bucket, pusch, ulcrc, ack, b.prach, b.ul_harq_discard,
                 b.dl_harq_discard, dl_mcs, ul_mcs,
                 mods[0][0] if mods else "-", b.dl_pdu))
    print()

    # ---- uplink CRC by modulation -------------------------------------------
    print("== uplink PUSCH results by modulation%s"
          % (" (rnti %s)" % args.rnti if args.rnti else ""))
    print("  %-8s %7s %7s %8s %9s %9s" % ("mod", "n", "KO%", "iter_med", "sinr_med", "sinr_p90"))
    for mod, (count, ko, sinrs) in sorted(per_mod.items(), key=lambda kv: -kv[1][0]):
        print("  %-8s %7d %6.1f%% %8.1f %9.1f %9.1f"
              % (mod, count, 100 * ko / count, median([r["iter"] for r in pusch_rows
                                                       if r["mod"] == mod
                                                       and (not args.rnti or r["rnti"] == args.rnti)]),
                 median(sinrs), pct(sinrs, 0.9)))
    print()

    # ---- random access ------------------------------------------------------
    nof_prach = sum(b.prach for b in buckets.values())
    nof_rar = sum(b.rar for b in buckets.values())
    msg3 = [r for r in pusch_rows if r["rnti"] in msg3_rntis]
    print("== random access")
    print("  PRACH detections=%d  RAR grants=%d  Msg3 grant allocation failures=%d"
          % (nof_prach, nof_rar, sum(b.msg3_grant_fail for b in buckets.values())))
    if msg3:
        ok = sum(1 for r in msg3 if r["crc"] == "OK")
        print("  Msg3 attempts=%d  crc OK=%d (%.1f%%)  sinr median %.1f p10 %.1f p90 %.1f dB  epre median %.1f"
              % (len(msg3), ok, 100 * ok / len(msg3), median([r["sinr"] for r in msg3]),
                 pct([r["sinr"] for r in msg3], 0.1), pct([r["sinr"] for r in msg3], 0.9),
                 median([r["rsrp"] for r in msg3])))
        print("  Msg3 cycles=%d (tc-rntis: %s)"
              % (len(msg3_rntis), " ".join(sorted(msg3_rntis)[:6]) + (" ..." if len(msg3_rntis) > 6 else "")))
    if nof_prach:
        rssi = [v for b in buckets.values() for v in b.prach_rssi]
        print("  PRACH rssi: median %.1f dB  min %.1f  max %.1f"
              % (median(rssi), min(rssi), max(rssi)))
    print()

    # ---- aggregate counters -------------------------------------------------
    ul_ok = sum(b.ul_crc["ok"] for b in buckets.values())
    ul_ko = sum(b.ul_crc["ko"] for b in buckets.values())
    ack = collections.Counter()
    for b in buckets.values():
        ack.update(b.pucch_ack)
    total_ack = sum(ack.values())
    lat = [r["lat"] for r in pusch_rows if not args.rnti or r["rnti"] == args.rnti]
    print("== summary")
    print("  UL CRC indications      : ok=%d ko=%d  (ko %.1f%%)"
          % (ul_ok, ul_ko, 100 * ul_ko / max(1, ul_ok + ul_ko)))
    print("  DL HARQ-ACK on PUCCH    : ack=%d nack=%d dtx=%d  (ack %.1f%%, dtx %.1f%%)"
          % (ack["ack"], ack["nack"], ack["dtx"],
             100 * ack["ack"] / max(1, total_ack), 100 * ack["dtx"] / max(1, total_ack)))
    print("  HARQ TB discards        : UL=%d DL=%d"
          % (sum(b.ul_harq_discard for b in buckets.values()),
             sum(b.dl_harq_discard for b in buckets.values())))
    if lat:
        print("  PUSCH latency t= [us]   : median %.0f p90 %.0f p99 %.0f max %.0f"
              % (median(lat), pct(lat, 0.9), pct(lat, 0.99), max(lat)))
    dl_all = [g for b in buckets.values() for g in b.dl_grant if not args.rnti or g[0] == args.rnti]
    ul_all = [g for b in buckets.values() for g in b.ul_grant if not args.rnti or g[0] == args.rnti]
    if dl_all:
        print("  DL grants (%s): n=%d  mcs median %.0f max %d  rv0 %.0f%%  worst-mod share %.0f%%"
              % (args.rnti or "all", len(dl_all), median([g[2] for g in dl_all]),
                 max(g[2] for g in dl_all),
                 100 * sum(1 for g in dl_all if g[3] == 0) / len(dl_all),
                 100 * sum(1 for g in dl_all if g[1] == "256QAM") / len(dl_all)))
    if ul_all:
        new_data = sum(1 for g in ul_all if g[4])
        print("  UL grants (%s): n=%d  mcs median %.0f max %d  new_data %.0f%%  256QAM share %.0f%%"
              % (args.rnti or "all", len(ul_all), median([g[2] for g in ul_all]),
                 max(g[2] for g in ul_all), 100 * new_data / len(ul_all),
                 100 * sum(1 for g in ul_all if g[1] == "256QAM") / len(ul_all)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
