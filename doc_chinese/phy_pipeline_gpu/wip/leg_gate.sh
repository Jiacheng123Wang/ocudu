#!/usr/bin/env bash
# Pass 5 of the audit (5.9.102): the air leg's PRE-REGISTERED criteria, evaluated mechanically.
#
# The criteria were registered in 5.9.96 / 5.9.101 before the leg was run, so this script only applies
# them - it does not choose them. It exists because "read the numbers afterwards" is how a leg gets
# judged by the person who wants it to pass: every line here is either a threshold registered in the
# record or the presence of an instrument, and "cannot read" counts as RED rather than as absent
# (5.9.97 caught a hard gate printing "None" that looked exactly like a pass).
#
# usage:
#   bash leg_gate.sh s65-heavy-ul                      # the leg, against the s62 baseline
#   bash leg_gate.sh s65-heavy-ul s62-default-fenced    # explicit baseline
#   bash leg_gate.sh --slot-ms=0.5 s64-heavywide        # a 30 kHz cell: the Mbit/s time base follows the slot
set -u

LEG=""
BASE="s62-default-fenced"
SLOT_MS=1.0
for a in "$@"; do
  case "$a" in
    --slot-ms=*) SLOT_MS=${a#*=} ;;
    *) if [[ -z $LEG ]]; then LEG=$a; else BASE=$a; fi ;;
  esac
done
[[ -n $LEG ]] || { echo "usage: bash leg_gate.sh [--slot-ms=N] <leg-label> [baseline-label]" >&2; exit 2; }

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs

resolve() { # newest .log whose name carries the label (never the .stderr / .stdout siblings)
  local hit
  hit=$(ls -1t "$LOGDIR"/gnb_*"$1"*.log 2>/dev/null | grep -v '\.stderr$\|\.stdout$' | head -1)
  [[ -n $hit ]] || { echo "no leg matched '$1' in $LOGDIR" >&2; exit 2; }
  printf '%s' "$hit"
}

LEG_LOG=$(resolve "$LEG")
BASE_LOG=$(resolve "$BASE")

python3 - "$LEG_LOG" "$BASE_LOG" "$LEG" "$BASE" "$SLOT_MS" <<'PY'
import os, re, sys

leg_log, base_log, leg_lab, base_lab = sys.argv[1:5]
slot_ms = float(sys.argv[5])   # 1.0 on the 15 kHz n1 leg, 0.5 on a 30 kHz one: it is the time base of every Mbit/s below

def read(path):
    err = path + ".stderr" if os.path.exists(path + ".stderr") else path
    return open(path, errors="replace").read(), open(err, errors="replace").read()

def f(txt, rx):
    m = re.findall(rx, txt)
    return m[-1] if m else None

leg_txt, leg_err = read(leg_log)
base_txt, base_err = read(base_log)

def ul_mbps(txt, err):
    slots = f(err, r"\[ul_rx\] blocks=(\d+)")
    if not slots:
        return None
    dur = int(slots) * slot_ms / 1000.0            # slots x slot length
    tbs = [int(x) for x in re.findall(r"PUSCH:.*?tbs=(\d+)", txt)]
    return (sum(tbs) * 8 / dur / 1e6) if dur else None, len(tbs), int(slots)

def ul_duty(txt, err):
    slots = f(err, r"\[ul_rx\] blocks=(\d+)")
    grants = len(re.findall(r"PUSCH:", txt))
    return (100.0 * grants / int(slots)) if slots else None

b_mbps, b_grants, b_slots = (ul_mbps(base_txt, base_err) or (None, None, None))
l_mbps, l_grants, l_slots = (ul_mbps(leg_txt, leg_err) or (None, None, None))
b_duty, l_duty = ul_duty(base_txt, base_err), ul_duty(leg_txt, leg_err)

rows = []
def check(name, ok, detail):
    # ok is True / False / None (None = cannot read, which is RED - see the header).
    verdict = "PASS" if ok is True else ("RED (cannot read)" if ok is None else "FAIL")
    rows.append((verdict, name, detail))

# BOTH printed forms (2026-09-24): the reader used to know only "MET (8 of 8 checks applicable)", so a
# contract that FAILED - "NOT MET: 1 of 8 applicable checks failed (mode=gpu)" - printed as None, i.e. as
# "cannot read" instead of as the failure it was.
contract = f(leg_err, r"contract ((?:MET|NOT MET)[^\n]*)")
mode     = f(leg_err, r"contract \(mode=([a-z_]+)\)")
check("contract 8 of 8, mode=gpu", (contract is not None) and contract.startswith("MET (8 of 8") and mode == "gpu",
      f"{contract} mode={mode}")

stale = f(leg_err, r"\[ul_pipeline\] stale=(\d+)")
check("stale = 0", stale == "0", f"stale={stale}")

cross = f(leg_err, r"= ([0-9.]+) read\(s\) \+ ([0-9.]+) write\(s\) per hop")
check("crossings 0.00 + 0.00 per hop", cross == ("0.00", "0.00"), f"{cross}")

cbs  = f(leg_err, r"\[ul_gpu_lane\] lanes=\d+ cbs/lane=([0-9.]+)")
cbsm = f(leg_err, r"cbs/lane=[0-9.]+ \(max=(\d+)\)")
drop = f(leg_err, r"cbs/lane=[0-9.]+ \(max=\d+\) dropped=(\d+)")
check("cbs/lane <= 2.00, max <= 2, dropped = 0",
      (cbs is not None) and (float(cbs) <= 2.001) and (cbsm == "2") and (drop == "0"),
      f"cbs/lane={cbs} max={cbsm} dropped={drop}")

gaps = f(leg_err, r"radio sample continuity: (\d+) gaps")
check("radio sample continuity: 0 gaps", gaps == "0", f"gaps={gaps}")

check("VALIDITY: UL >= 2.0 Mbit/s (5x the baseline)",
      (l_mbps is not None) and (l_mbps >= 2.0),
      f"{l_mbps if l_mbps is None else round(l_mbps, 2)} Mbit/s against {b_mbps if b_mbps is None else round(b_mbps, 2)}")
check("VALIDITY: UL grant in >= 50% of slots",
      (l_duty is not None) and (l_duty >= 50.0),
      f"{l_duty if l_duty is None else round(l_duty, 1)}% against {b_duty if b_duty is None else round(b_duty, 1)}%")

pool_line = f(leg_err, r"(\[ul_rx_pool\][^\n]*)")
free_min  = f(leg_err, r"\[ul_rx_pool\].*?free_min=(-?\d+)")
empty_warn = len(re.findall(r"the receive pool is EMPTY", leg_err)) + len(re.findall(r"the receive pool is EMPTY", leg_txt))
if free_min is None:
    check("RX pool: readable", None, "no [ul_rx_pool] line")
elif int(free_min) == 0:
    check("RX pool EMPTY => the one-shot WARN appears exactly once", empty_warn == 1,
          f"free_min=0, WARN count={empty_warn}")
else:
    check("RX pool: no EMPTY (free_min >= 1)", True, f"free_min={free_min}; WARN count={empty_warn}")

new_fmt = f(leg_err, r"(dft radio inputs: [^\n]*submit routes[^\n]*)")
check("dft radio inputs in the NEW format (5.9.99)", new_fmt is not None, (new_fmt or "old format or missing")[:110])

print(f"pass 5 gate: {leg_lab}   (baseline {base_lab}, slot {slot_ms} ms)")
print(f"  leg log: {os.path.basename(leg_log)}")
print()
for verdict, name, detail in rows:
    print(f"  [{verdict:<16}] {name}")
    print(f"                     {detail}")
red = sum(1 for v, _, _ in rows if v != "PASS")
print()
print(f"  {len(rows) - red} of {len(rows)} checks pass" + ("" if red == 0 else f"  --  {red} to explain"))

# ---- INFORMATION, deliberately NOT a tenth row ---------------------------------------------------
# The milestone table (5.9.54 1) carries the row "dropped slots / RF real-time failures | 0 / 0", and
# the audit of 5.9.120 found that half of it is a ONE-LEG claim: s47 and s67 read 0, but s62=2, s63=4,
# s64b=21, s65=8, s66=40 and s69=33 (all `[RF] [W] Real-time failure in RF: underflow|late`). It is
# printed here rather than judged because NO THRESHOLD IS REGISTERED, and inventing one inside a gate
# is how a criterion becomes whatever the last person wanted. What it must not do is stay invisible:
# a leg with 33 of them currently scores 7 of 9 and looks like the one with 0.
rtf = len(re.findall(r"Real-time failure in RF", leg_txt))
print()
print(f"  [INFO           ] RF real-time failures in this leg's .log: {rtf}")
print(f"                     not a criterion (no threshold registered); 5.9.54's '0 RF failures' row holds only on s47/s67")
PY
