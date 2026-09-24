#!/usr/bin/env bash
# Evaluates the criteria PRE-REGISTERED in design document 5.9.124 for the "real-time wall" A/B:
#   A-1  gpu, the same load recipe as s70                 -> is the wall reproducible?
#   A-2  the same, with --expert_phy.phy_pipeline cpu     -> which side is it on?
#
# The criteria were written down BEFORE the legs were run (5.9.124 3), so this script only applies them -
# it does not choose them. It exists because the interesting question here is a SHAPE question (when do
# the RF failures happen, relative to the offered load), and shapes read by eye drift towards whatever
# the reader expects. "Cannot read" is RED, never absent (5.9.97).
#
# usage:
#   bash wall_ab.sh <A-1 leg> [A-2 leg]        # labels or paths; A-2 optional (then only R1-R4 apply)
#   bash wall_ab.sh --reference                # print the s70 reference arm's readings only
#   bash wall_ab.sh --self-test                # the script's own arms: can it call a wall and a difference?
set -u

LEG1=""; LEG2=""; REFERENCE=0; SELF_TEST=0
for a in "$@"; do
  case "$a" in
    --reference) REFERENCE=1 ;;
    --self-test) SELF_TEST=1 ;;
    *) if [ -z "$LEG1" ]; then LEG1=$a; elif [ -z "$LEG2" ]; then LEG2=$a; fi ;;
  esac
done

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs

resolve() {
  local hit
  if [ -f "$1" ]; then printf '%s' "$1"; return; fi
  # A path given without its .log suffix is still a path (the self-test's synthetic legs are stored that
  # way, and a reader copying a name out of this document's tables will do the same).
  if [ -f "$1.log" ]; then printf '%s' "$1.log"; return; fi
  hit=$(ls -1t "$LOGDIR"/gnb_*"$1"*.log 2>/dev/null | grep -v '\.stderr$\|\.stdout$' | head -1)
  [[ -n $hit ]] || { echo "no leg matched '$1' in $LOGDIR" >&2; exit 2; }
  printf '%s' "$hit"
}

if [ "$REFERENCE" = 1 ]; then LEG1=s70-heavy-n78; fi
# The usage guard must not fire for --self-test, which runs before any leg is named (the first version of
# this script had the guard above the self-test block, so --self-test printed the usage and exited).
if [ -z "$LEG1" ] && [ "$SELF_TEST" = 0 ]; then
  echo "usage: bash wall_ab.sh <A-1 leg> [A-2 leg] | --reference | --self-test" >&2
  exit 2
fi

if [ "$SELF_TEST" = 1 ]; then
  # Two synthetic legs written into a scratch dir and evaluated by the SAME python below: one that repeats
  # s70's shape and one that does not, plus a cpu arm with a fifth of the failures. A gate that has never
  # been shown to go red is not a gate.
  T=$(mktemp -d)
  mk() { # $1=file $2=rf_underflow $3=rf_late $4=stale $5=total_seconds $6=mode
    local f=$1 u=$2 l=$3 st=$4 tot=$5 mode=$6 i=0
    : > "$f.log"
    # Chronological, like a real log: one PUSCH grant per second across the run, and every RF failure
    # inside the LAST 20% of it - so R3 (>=80% in the last quarter) has something true to read.
    while [ "$i" -lt "$tot" ]; do
      printf '2026-09-24T10:00:%02d.000000 [PHY     ] [I] [ %5d.0] PUSCH: rnti=0x4601 tbs=704\n' "$i" "$i" >> "$f.log"
      if [ "$i" = "$((tot - 2))" ]; then
        local k=0
        while [ "$k" -lt "$((u + l))" ]; do
          printf '2026-09-24T10:00:%02d.500000 [RF      ] [W] Real-time failure in RF: %s\n' "$i" \
                 "$([ "$k" -lt "$u" ] && echo underflow || echo late)" >> "$f.log"
          k=$((k + 1))
        done
      fi
      i=$((i + 1))
    done
    { printf '[phy_pipeline] contract (mode=%s):\n' "$mode"
      printf '[phy_pipeline]   radio sample continuity: 0 gaps over 100 blocks (0 samples missing or repeated), 0 timestamp-0 blocks -> OK\n'
      if [ "$mode" = gpu ]; then
        printf '[phy_pipeline]   host device data crossings: 0 host read(s) (0 bytes) and 0 host write(s) (0 bytes) of device data over 10 device hop(s) = 0.00 read(s) + 0.00 write(s) per hop\n'
      fi
      printf '[phy_pipeline] contract MET (%s of %s checks applicable)\n' "$([ "$mode" = gpu ] && echo 8 || echo 5)" "$([ "$mode" = gpu ] && echo 8 || echo 7)"
      [ "$mode" = gpu ] && printf '[ul_gpu_lane] lanes=10 cbs/lane=2.00 (max=2) dropped=0 carried=0 period_dropped=0\n'
      printf '[ul_pipeline] stale=%s (span > 8000 us, the uplink HARQ round trip) mean=8397.4us max_stale=8767.0us\n' "$st"
    } > "$f.log.stderr"
  }
  mk "$T/repeat" 235 116 5 60 gpu     # R2 >=100, R3 >=80% late, R4 stale>0, R1 ok -> "reproduced"
  mk "$T/norep"  4   2   0 60 gpu     # R2 <=10, R4 =0                         -> "not reproduced"
  mk "$T/cpu"    40  20  0 60 cpu     # A-2 with ~1/6 of A-1's failures, mode=cpu -> "lane side"
  echo "self-test: repeat arm (expect R2 reproduced)"
  bash "$0" "$T/repeat" | grep -E "^\s+\[|verdict" | sed 's/^/    /'
  echo "self-test: norep arm (expect R2 not reproduced)"
  bash "$0" "$T/norep" | grep -E "^\s+\[|verdict" | sed 's/^/    /'
  echo "self-test: cpu arm vs repeat (expect 'lane/GPU side')"
  bash "$0" "$T/repeat" "$T/cpu" | grep -E "R5|verdict" | sed 's/^/    /'
  rm -rf "$T"; exit 0
fi

LEG1_LOG=$(resolve "$LEG1")
LEG2_LOG=""; [ -n "$LEG2" ] && LEG2_LOG=$(resolve "$LEG2")

python3 - "$LEG1_LOG" "$LEG2_LOG" "$LEG1" "${LEG2:-<none>}" <<'PY'
import os, re, sys

l1, l2, n1, n2 = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

def read(path):
    err = path + ".stderr" if os.path.exists(path + ".stderr") else path
    return (open(path, errors="replace").read() if os.path.exists(path) else "",
            open(err, errors="replace").read() if os.path.exists(err) else "")

def facts(path, label):
    txt, err = read(path)
    f = {}
    rf = re.findall(r"^(\S+) \[RF\s*\] \[W\] Real-time failure in RF: (\w+)", txt, re.M)
    f["rf_total"] = len(rf)
    f["rf_underflow"] = sum(1 for _, k in rf if k == "underflow")
    f["rf_late"] = sum(1 for _, k in rf if k == "late")
    # the run's own time span, from the log's first and last timestamps
    ts = re.findall(r"^(\S+) \[", txt, re.M)
    def secs(t):
        m = re.match(r"(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)", t)
        if not m: return None
        h, mi, s = int(m.group(4)), int(m.group(5)), int(m.group(6))
        return h * 3600 + mi * 60 + s
    f["t0"], f["t1"] = (secs(ts[0]), secs(ts[-1])) if ts else (None, None)
    f["dur"] = (f["t1"] - f["t0"]) if (f["t0"] is not None and f["t1"] is not None) else None
    # R3: share of failures inside the LAST 25% of the run
    if rf and f["dur"]:
        cut = f["t1"] - 0.25 * f["dur"]
        f["rf_last_quarter"] = sum(1 for t, _ in rf if (secs(t) or 0) >= cut)
    else:
        f["rf_last_quarter"] = None
    # offered load: grants per second in 10 s windows, PER DIRECTION, and the PEAK window is the
    # "offered rate". Both directions are measured because the two regimes are different phenomena
    # (5.9.127): the uplink-load legs (s70..s75) and the downlink-saturated ones (s76..s79). Judging a
    # downlink leg by its PUSCH peak - which is what this script did at first - compares the wrong number
    # and would have called a 6x-lighter downlink "comparable".
    # findall with ONE group returns plain strings, not 1-tuples: unpacking them was this script's first
    # bug, and it took the whole run down (caught by running it on the reference arm, as intended).
    g  = [secs(t) for t in re.findall(r"^(\S+) \[[A-Z-]+\s*\] \[I\].*PUSCH:", txt, re.M)]
    gd = [secs(t) for t in re.findall(r"^(\S+) \[[A-Z-]+\s*\] \[I\].*PDSCH:", txt, re.M)]
    def peak_rate(seq):
        if not seq:
            return None
        lo = min(seq)
        bucket = {}
        for x in seq:
            bucket[(x - lo) // 10] = bucket.get((x - lo) // 10, 0) + 1
        return max(bucket.values()) / 10.0
    f["grants"] = len(g)
    f["grants_dl"] = len(gd)
    f["peak_rate"] = peak_rate(g)        # uplink
    f["peak_rate_dl"] = peak_rate(gd)    # downlink
    # the DOMINANT direction is the load this leg is actually about
    ul, dl = f["peak_rate"] or 0.0, f["peak_rate_dl"] or 0.0
    f["direction"] = "downlink" if dl > ul else "uplink"
    f["peak_dominant"] = max(ul, dl)
    # stale, contract, gaps, crossings, cbs/lane -- from the LAST occurrence (the shutdown prints twice)
    def last(rx, where=err):
        m = re.findall(rx, where)
        return m[-1] if m else None
    st = last(r"\[ul_pipeline\] stale=(\d+)")
    f["stale"] = int(st) if st is not None else None
    # BOTH printed forms: "MET (8 of 8 checks applicable)" and
    # "NOT MET: 1 of 8 applicable checks failed (mode=gpu)" - the first version of this reader only knew
    # the first, so the two legs that actually had something to report read as "cannot read".
    f["contract"] = last(r"contract ((?:MET|NOT MET)[^\n]*)")
    f["mode"] = last(r"contract \(mode=([a-z_]+)\)")
    f["gaps"] = last(r"radio sample continuity: (\d+) gaps")
    f["cross"] = last(r"= ([0-9.]+) read\(s\) \+ ([0-9.]+) write\(s\) per hop")
    f["cbs"] = last(r"cbs/lane=([0-9.]+) \(max=(\d+)\) dropped=(\d+)")
    return f

F1, F2 = facts(l1, n1), (facts(l2, n2) if l2 else None)
rows = []
def check(name, ok, detail):
    rows.append(("PASS" if ok is True else ("RED (cannot read)" if ok is None else "FAIL"), name, detail))

def hard(f, label, expect_gpu):
    """R1 is MODE-AWARE on purpose. A cpu leg cannot print `MET (8 of 8)`: the checks are registered by the
    code path, and cpu mode has no lane (so no crossings denominator and no cbs/lane line at all). Judging
    both arms with the gpu arm's expectations would fail A-2 for a reason that is not the wall - which is
    the same class of mistake this whole audit keeps finding."""
    if expect_gpu:
        ok_contract = (f["contract"] is not None and f["contract"].startswith("MET (8 of 8") and f["mode"] == "gpu")
        ok_cross    = f["cross"] == ("0.00", "0.00")
        ok_cbs      = (f["cbs"] is not None and float(f["cbs"][0]) <= 2.001 and f["cbs"][1] == "2" and f["cbs"][2] == "0")
        what        = "mode=gpu + MET (8 of 8) + crossings 0.00+0.00/hop + cbs/lane<=2 max=2 dropped=0 + gaps=0"
    else:
        ok_contract = (f["contract"] is not None and f["contract"].startswith("MET (") and f["mode"] == "cpu")
        ok_cross    = (f["cross"] is None) or (f["cross"] == ("0.00", "0.00"))
        ok_cbs      = True   # no lane in cpu mode: the line is absent BY CONSTRUCTION, not by failure
        what        = "mode=cpu + contract MET (N of N) + gaps=0 (no lane, so no crossings/cbs expectations)"
    ok = ok_contract and f["gaps"] == "0" and ok_cross and ok_cbs
    check(f"R1 {label}: hard gates ({what})", ok,
          f"contract={f['contract']} mode={f['mode']} gaps={f['gaps']} crossings={f['cross']} cbs/lane={f['cbs']}")

hard(F1, n1, F1["mode"] != "cpu")
t = F1["rf_total"]
check(f"R2 {n1}: the wall REPRODUCED (>=100 RF failures)", True if t >= 100 else (False if t <= 10 else None),
      f"{t} RF failure(s) = {F1['rf_underflow']} underflow + {F1['rf_late']} late"
      + ("" if t >= 100 or t <= 10 else "  <- GRAY ZONE (11..99): a third leg is needed"))
q = F1["rf_last_quarter"]
check(f"R3 {n1}: the SHAPE reproduced (>=80% of them in the last 25% of the run)",
      None if q is None else (q >= 0.8 * t if t else False),
      "no RF failures, so no shape" if q is None or not t else f"{q} of {t} in the last quarter of a {F1['dur']}s run")
check(f"R4 {n1}: stale >= 1 (a hop crossed the 8 ms round trip)", None if F1["stale"] is None else F1["stale"] >= 1,
      f"stale={F1['stale']}")

if F2 is not None:
    hard(F2, n2, F2["mode"] != "cpu")
    p1, p2 = F1["peak_dominant"], F2["peak_dominant"]
    ok = (p1 and p2 and max(p1, p2) <= 2.0 * min(p1, p2))
    check(f"P0 load comparability: peak offered rate within 2x, dominant direction ({n1} vs {n2})", ok,
          f"{F1['direction']} {p1} vs {F2['direction']} {p2} grants/s (ul {F1['peak_rate']}/{F2['peak_rate']}, "
          f"dl {F1['peak_rate_dl']}/{F2['peak_rate_dl']})"
          + ("" if ok else "  <- THE COMPARISON IS VOID: the loads are not comparable"))
    c1, c2 = F1["rf_total"], F2["rf_total"]
    # R5 is registered for a MODE difference (gpu vs cpu) and for nothing else: applying it to two legs of
    # the same mode would "conclude" a lane-vs-machine split out of run-to-run variance (met 2026-09-24 on
    # the cold/hot pair, both gpu: 166 vs 18, which says nothing about which side the wall is on).
    if F1["mode"] == F2["mode"]:
        check(f"R5 {n2} vs {n1}: which side is the wall on?", None,
              f"not applicable: both legs are mode={F1['mode']}; R5 needs a gpu leg and a cpu leg "
              f"(their {c1} vs {c2} difference is run-to-run variance, not a side)")
    elif ok:
        if c2 <= c1 / 5.0:
            verdict = "LANE/GPU SIDE: cpu mode at the same load is >=5x cleaner"
        elif c2 >= c1 / 2.0:
            verdict = "MACHINE/RF SIDE: cpu mode hits the same wall"
        else:
            verdict = "GRAY ZONE (between 1/5 and 1/2): a third leg is needed"
        check(f"R5 {n2} vs {n1}: wall on the LANE/GPU side? (PASS = yes, FAIL = machine/RF side)",
              c2 <= c1 / 5.0, f"{c2} vs {c1} RF failures -> {verdict}")
    else:
        check(f"R5 {n2} vs {n1}: which side is the wall on?", None, "not judged: P0 failed")

print(f"wall A/B (5.9.124):  {os.path.basename(l1)}" + (f"   vs   {os.path.basename(l2)}" if l2 else ""))
for f, n in ((F1, n1), (F2, n2)):
    if f is None: continue
    print(f"  {n}: {f['dur']}s, {f['grants']} ul / {f['grants_dl']} dl grants, peak {f['peak_rate']} ul / "
          f"{f['peak_rate_dl']} dl grants/s ({f['direction']}-loaded), "
          f"RF {f['rf_total']} ({f['rf_underflow']}u/{f['rf_late']}l), stale={f['stale']}")
print()
for v, name, detail in rows:
    print(f"  [{v:<16}] {name}")
    print(f"                     {detail}")
red = sum(1 for v, _, _ in rows if v != "PASS")
print()
print(f"  {len(rows) - red} of {len(rows)} criteria pass"
      + ("" if red == 0 else "  --  see 5.9.124 (4) for what each shape means"))
PY
