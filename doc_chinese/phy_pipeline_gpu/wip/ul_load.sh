#!/usr/bin/env bash
# The UPLINK LOAD a leg actually carried, and what the lane paid for it.
#
# Why this exists: the lane's headroom was quoted as ~45% on a leg whose uplink was nearly idle
# (0.39 Mbit/s, a UL grant in 16% of slots, TBS median 96 B - i.e. TCP acknowledgements, not a flow).
# A headroom number is only meaningful next to the load that produced it, so this prints the LOAD
# first and the lane's price second, and refuses to present the second as headroom when the first did
# not move.
#
# "余量" is the record's definition (5.9.92 / 5.9.94): 1 - residency median / slot period. The period
# is 1000us on the 5 MHz FDD n1 leg (15 kHz SCS) and 500us on the 20 MHz TDD n78 one (30 kHz), hence
# --slot-us. The lane's own measured `period` line is printed too, as a cross-check on that constant.
#
# usage:
#   bash ul_load.sh s62-default-fenced                  # a leg label (looked up in wip/logs)
#   bash ul_load.sh logs/gnb_gpu_s62-*.log              # explicit logs
#   bash ul_load.sh --slot-us=500 <n78 leg>             # a 30 kHz leg
#   bash ul_load.sh s62-default-fenced s63-heavy-ul     # two legs side by side
set -u

SLOT_US=1000
ARGS=()
for a in "$@"; do
  case "$a" in
    --slot-us=*) SLOT_US=${a#*=} ;;
    *)           ARGS+=("$a") ;;
  esac
done
[[ ${#ARGS[@]} -gt 0 ]] || { echo "usage: bash ul_load.sh [--slot-us=N] <log | leg-label> [...]" >&2; exit 2; }

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs

RESOLVED=()
for a in "${ARGS[@]}"; do
  if [[ -f $a ]]; then
    RESOLVED+=("$a")
    continue
  fi
  # a bare label: newest leg whose name carries it. A leg is three files; the .log is the one whose
  # siblings hold the contract and the counters, and the python below finds them by suffix.
  hit=$(ls -1t "$LOGDIR"/gnb_*"$a"*.log 2>/dev/null | grep -v '\.stderr$\|\.stdout$' | head -1)
  [[ -n $hit ]] || { echo "no leg matched '$a' in $LOGDIR" >&2; exit 2; }
  RESOLVED+=("$hit")
done

python3 - "$SLOT_US" "${RESOLVED[@]}" <<'PY'
import glob, os, re, sys

slot_us = float(sys.argv[1])
paths   = sys.argv[2:]

def num(txt, rx, group=1):
    m = re.findall(rx, txt)
    return m[-1] if m else None

def fnum(txt, rx):
    v = num(txt, rx)
    return float(v) if v is not None else None

rows = []
for p in paths:
    log  = p
    err  = p + ".stderr" if os.path.exists(p + ".stderr") else p
    txt  = open(log, errors="replace").read()
    etxt = open(err, errors="replace").read()
    lab  = os.path.basename(p).replace("gnb_gpu_", "").replace(".log", "")

    slots = num(etxt, r"\[ul_rx\] blocks=(\d+)")
    slots = int(slots) if slots else None
    dur   = (slots or 0) / (1e6 / slot_us)          # seconds, from the slot count
    ul    = [int(x) for x in re.findall(r"PUSCH:.*?tbs=(\d+)", txt)]
    dl    = [int(x) for x in re.findall(r"PDSCH:.*?tbs=(\d+)", txt)]
    r = {"lab": lab, "slots": slots, "dur": dur, "ul": ul, "dl": dl}

    r["ul_grants"]  = len(ul)
    r["ul_mbps"]    = (sum(ul) * 8 / dur / 1e6) if dur else None
    r["ul_duty"]    = (100.0 * len(ul) / slots) if slots else None
    r["ul_med"]     = sorted(ul)[len(ul) // 2] if ul else None
    r["ul_max"]     = max(ul) if ul else None
    r["ul_big"]     = (100.0 * sum(1 for x in ul if x >= 1000) / len(ul)) if ul else None
    r["dl_mbps"]    = (sum(dl) * 8 / dur / 1e6) if dur else None
    r["dl_grants"]  = len(dl)

    r["lanes"]      = num(etxt, r"\[ul_gpu_lane\] lanes=(\d+)")
    r["cbs"]        = fnum(etxt, r"\[ul_gpu_lane\] lanes=\d+ cbs/lane=([0-9.]+)")
    r["cbs_max"]    = num(etxt, r"cbs/lane=[0-9.]+ \(max=(\d+)\)")
    r["dropped"]    = num(etxt, r"dropped=(\d+)")
    r["ch_wt"]      = fnum(etxt, r"ch_wt=([0-9.]+)us/lane")
    r["merged_hop"] = fnum(etxt, r"merged_hop=([0-9.]+)us/lane")
    r["res_med"]    = fnum(etxt, r"\[ul_gpu_lane\] residency .*?median=([0-9.]+)us")
    r["res_p95"]    = fnum(etxt, r"\[ul_gpu_lane\] residency .*?p95=([0-9.]+)us")
    r["res_p99"]    = fnum(etxt, r"\[ul_gpu_lane\] residency .*?p99=([0-9.]+)us")
    r["lane_per"]   = fnum(etxt, r"\[ul_gpu_lane\] period .*?median=([0-9.]+)us")
    r["headroom"]   = (100.0 * (1 - r["res_med"] / slot_us)) if r["res_med"] else None

    r["gpu_med"]    = fnum(etxt, r"\[ul_gpu_pipeline\] samples=\d+ mean=[0-9.]+us median=([0-9.]+)us")
    r["gpu_p95"]    = fnum(etxt, r"\[ul_gpu_pipeline\] samples=\d+ mean=[0-9.]+us median=[0-9.]+us min=[0-9.]+us max=[0-9.]+us p95=([0-9.]+)us")
    r["ul_pipe_med"] = fnum(etxt, r"\[ul_pipeline\] samples=\d+ mean=[0-9.]+us median=([0-9.]+)us")
    r["stale"]      = num(etxt, r"\[ul_pipeline\] stale=(\d+)")
    r["defer_med"]  = fnum(etxt, r"defer_wait distribution:.*?median=([0-9.]+)us")
    r["ce_total"]   = fnum(etxt, r"\[mmse_time_sum\] calls=.*?mean total=([0-9.]+)us")
    r["pool"]       = num(etxt, r"(\[ul_rx_pool\][^\n]*)")
    r["rtf"]        = len(re.findall(r"Real-time failure in RF", txt))
    r["crc_ok"]     = len(re.findall(r"crc=OK", txt))
    r["crc_ko"]     = len(re.findall(r"crc=KO", txt))
    r["contract"]   = num(etxt, r"contract ((?:MET|NOT MET) \(\d+ of \d+\))")
    r["mode"]       = num(etxt, r"contract \(mode=([a-z_]+)\)")
    rows.append(r)

def show(v, fmt="{:.2f}", unit="", dash="  -  "):
    return dash if v is None else (fmt.format(v) + unit)

for r in rows:
    print(f"================ {r['lab']}   (slot period assumed {slot_us:.0f} us)")
    print("-- LOAD (read this FIRST: headroom without load says nothing)")
    print(f"  slots                : {r['slots']}  ({show(r['dur'])} s)")
    print(f"  UL grants / duty     : {r['ul_grants']}  ({show(r['ul_duty'], '{:.1f}', '%')} of slots)")
    print(f"  UL payload           : {show(r['ul_mbps'], '{:.2f}', ' Mbit/s')}   "
          f"TBS median {r['ul_med']} B, max {r['ul_max']} B, >=1000 B: {show(r['ul_big'], '{:.1f}', '%')}")
    print(f"  DL grants / payload  : {r['dl_grants']}  ({show(r['dl_mbps'], '{:.2f}', ' Mbit/s')})")
    print("-- LANE (what the load cost)")
    print(f"  lanes / cbs per lane : {r['lanes']}  ({show(r['cbs'], '{:.2f}')} max={r['cbs_max']}, dropped={r['dropped']})")
    print(f"  busy split           : ch_wt={show(r['ch_wt'], '{:.1f}', 'us')}/lane   "
          f"merged_hop={show(r['merged_hop'], '{:.1f}', 'us')}/lane")
    print(f"  residency med/p95/p99: {show(r['res_med'], '{:.1f}', 'us')} / "
          f"{show(r['res_p95'], '{:.1f}', 'us')} / {show(r['res_p99'], '{:.1f}', 'us')}")
    print(f"  lane period median   : {show(r['lane_per'], '{:.1f}', 'us')}   "
          f"(assumed slot {slot_us:.0f} us)")
    print(f"  ==> 余量 = 1 - residency_median/slot : {show(r['headroom'], '{:.1f}', '%')}")
    print("-- LATENCY / COST")
    print(f"  [ul_gpu_pipeline]    : median {show(r['gpu_med'], '{:.1f}', 'us')}   p95 {show(r['gpu_p95'], '{:.1f}', 'us')}")
    print(f"  [ul_pipeline]        : median {show(r['ul_pipe_med'], '{:.1f}', 'us')}   stale={r['stale']}")
    print(f"  defer_wait median    : {show(r['defer_med'], '{:.1f}', 'us')}")
    print(f"  CE mean total        : {show(r['ce_total'], '{:.1f}', 'us')}")
    print("-- VALIDITY / HARD GATES")
    print(f"  contract             : {r['contract']}  (mode={r['mode']})")
    print(f"  crc OK/KO, RT fails  : {r['crc_ok']} / {r['crc_ko']},  {r['rtf']}")
    print(f"  rx pool              : {r['pool']}")
    print()

if len(rows) == 2:
    a, b = rows
    print("================ side by side (baseline -> heavy)")
    def pair(key, fmt="{:.2f}", ratio=True):
        x, y = a.get(key), b.get(key)
        s = f"  {key:<20}: {show(x, fmt)}  ->  {show(y, fmt)}"
        if ratio and x not in (None, 0) and y is not None:
            s += f"   (x{y / x:.2f})"
        return s
    for k, f in (("ul_mbps", "{:.2f}"), ("ul_duty", "{:.1f}"), ("ul_grants", "{:.0f}"),
                 ("ul_med", "{:.0f}"), ("res_med", "{:.1f}"), ("headroom", "{:.1f}"),
                 ("ch_wt", "{:.1f}"), ("merged_hop", "{:.1f}"), ("gpu_med", "{:.1f}"),
                 ("defer_med", "{:.1f}"), ("ce_total", "{:.1f}")):
        print(pair(k, f))
    print("  (the pre-registered rules for this pair are in the record, section 5.9.96)")
PY
