#!/usr/bin/env bash
# Pass 5 of the audit (5.9.102): the air leg's PRE-REGISTERED criteria, evaluated mechanically.
#
# The criteria were registered in 5.9.96 / 5.9.101 before the leg was run, so this script only applies
# them - it does not choose them. It exists because "read the numbers afterwards" is how a leg gets
# judged by the person who wants it to pass: every line here is either a threshold registered in the
# record or the presence of an instrument, and "cannot read" counts as RED rather than as absent
# (5.9.97 caught a hard gate printing "None" that looked exactly like a pass).
#
# 2026-09-27 REPAIR (this script had gone stale enough to be unusable - measured: 5 of 9 on a healthy
# delivery leg, p72-n78-batch14). Three kinds of repair, and the distinction matters:
#
#   * OUT-OF-DATE LITERALS, corrected: the contract grew to NINE names (lane host participation, 6.97)
#     while this file still demanded "MET (8 of 8"; and `cbs/lane` is judged on its MEAN (2.00), which is
#     what the leg prints and what V4 registered - the "max <= 2" literal came from the early legs whose
#     worst hop happened to be 2, and the current ones read max=5 with the same mean. The worst hop is
#     now REPORTED, not judged (no threshold was ever registered for it).
#   * BINDINGS, made explicit - THREE of them, each found by a healthy leg that failed it: `stale=0` was
#     registered for the DEFAULT regime (under load, 5.9.127 R4 licenses the opposite); ">= 50% of slots
#     carry a UL grant" for the n1/FDD geometry (a 20 MHz TDD cell with ul_ratio 0.30 cannot reach 50% by
#     construction); "UL >= 2.0 Mbit/s (5x the baseline)" for the STRESS recipe (it exists so a loaded leg
#     proves it was loaded, and the default regime is defined as "no load generator"); and the CRC floor
#     needs TRAFFIC (660 hops on an idle phone read 60.0% while the same binary read 86.0% under load).
#     A check whose binding does not match the leg now reads NOT JUDGED with the reason, instead of FAILing
#     a healthy leg - the rule the milestone audit already follows for A1-2 ("judged wherever it can be
#     judged, never softened").
#   * ARM DETECTION, added (the reason this file could certify a leg whose link was deliberately dead):
#     the same two checks the milestone audit gained - the leg's `knob` lines must carry nothing that
#     changes behaviour, and CRC-OK/lanes must be >= 60% (arms read 43.7-58.8%, delivery 79.9-95.6%).
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

def leg_slots(err):
    """[ul_rx] blocks as SLOTS - and since the per-symbol receive policy became the default (dev doc 6.215
    (1)) ONE BLOCK IS ONE OFDM SYMBOL, so blocks/14 is the slot count. Reading blocks as slots inflates the
    wall clock by 14x and deflates every rate by the same factor: measured on p169-n78-stress this made the
    stress-validity check read 0.24 Mbit/s against 0.77 (FAIL) for a leg that carried 34.34 MB in 103.1 s
    (2.67 Mbit/s, 2.7x the baseline). Same two independent facts as wip/ul_load.sh: the [ul_rx_policy] line
    says it in words, samples/blocks says it numerically on every leg, old or new (822.86 = 11520/14 on a
    symbol leg, exactly 11520 on a whole-slot one)."""
    blocks = f(err, r"\[ul_rx\] blocks=(\d+)")
    if not blocks:
        return None
    samps = f(err, r"\[ul_rx\] blocks=\d+ samples=(\d+)")
    spb   = (float(samps) / int(blocks)) if samps else None
    nsym  = f(err, r"(\d+) of \d+ slot symbols")
    nsym  = int(nsym) if nsym else 14
    sym   = ("blocks of 1 OFDM symbol" in err) or (spb is not None and spb < 2000.0)
    return (int(blocks) // nsym) if sym else int(blocks)

def ul_mbps(txt, err):
    slots = leg_slots(err)
    if not slots:
        return None
    dur = slots * slot_ms / 1000.0                 # slots x slot length
    tbs = [int(x) for x in re.findall(r"PUSCH:.*?tbs=(\d+)", txt)]
    return (sum(tbs) * 8 / dur / 1e6) if dur else None, len(tbs), slots

def ul_duty(txt, err):
    slots = leg_slots(err)
    grants = len(re.findall(r"PUSCH:", txt))
    return (100.0 * grants / slots) if slots else None

b_mbps, b_grants, b_slots = (ul_mbps(base_txt, base_err) or (None, None, None))
l_mbps, l_grants, l_slots = (ul_mbps(leg_txt, leg_err) or (None, None, None))
b_duty, l_duty = ul_duty(base_txt, base_err), ul_duty(leg_txt, leg_err)

rows = []
def check(name, ok, detail):
    # ok is True / False / None (None = cannot read, which is RED - see the header).
    verdict = "PASS" if ok is True else ("RED (cannot read)" if ok is None else "FAIL")
    rows.append((verdict, name, detail))

def check_bound(name, ok, detail, bound_here, reason):
    """A check whose BINDING does not match this leg reads NOT JUDGED with the reason, never FAIL (2026-09-27).
    `ok` is only consulted when bound_here is True; `ok=None` there still means RED (cannot read)."""
    if not bound_here:
        rows.append(("NOT JUDGED", name, "binding does not match this leg: " + reason))
        return
    check(name, ok, detail)

# ---- the leg's own identity: regime and geometry, read from the report header -----------------------
# Both are needed below, and both are printed by run_leg.sh into the leg's .stderr:
#   [leg] regime=stress
#   cell config   : configs/gnb_rf_b200_tdd_n78_20mhz.yml
leg_regime = f(leg_err, r"\[leg\] regime=(\w+)") or "?"
leg_cfg    = f(leg_err, r"cell config\s*:\s*(\S+)") or "?"
is_default = (leg_regime == "default")
is_n1_fdd  = ("fdd_n1" in leg_cfg)

# ---- ARM DETECTION (2026-09-27): a measurement arm must not certify acceptance ---------------------
# p84 (ablation arm) satisfied EVERY other criterion in this file: contract 9 of 9, crossings 0.00+0.00,
# stale=0, gaps=0, cbs/lane=2.00 - with a link that was deliberately dead (CRC-OK 48.4%). Two mechanical
# facts separate an arm from a delivery leg; both are now judged, and the knob list is FAIL-CLOSED: an
# unknown knob is refused rather than assumed harmless, so adding a probe is a deliberate act.
# Probes: report-only, but they do perturb (see milestone_audit.sh: the category rule and why
# OCUDU_UL_SLOT_TRACE is one of them).
# Probes: report-only, and they belong here by CATEGORY, not by name (the same rule milestone_audit.sh
# states). OCUDU_UL_TIMING_EVENTS joined 2026-10-01 with dev doc 6.241: it PRINTS the worst receive waits and
# hand-over margins with their host wall clocks and changes no delivery decision; when unset it reads no clock
# and prints nothing, and when set it stores at most 64 events (only above a 1 ms / 500 us floor).
# OCUDU_SCHED_VERBOSE joined 2026-10-01 with doc_chinese/macos_thread_priority/ dev doc 10.5: it reads back each
# worker thread's granted QoS class / POSIX policy ONCE at thread creation and PRINTS one line; it changes no
# scheduling parameter, reads no clock, and prints nothing when unset. The two knobs that DO change macOS
# scheduling (OCUDU_SCHED_SKIP_POSIX_RT, OCUDU_SCHED_ATTR_QOS) are deliberately NOT here: they are arms, and an
# arm can satisfy every other criterion in this file.
KNOB_ANY = ("OCUDU_METAL_GPU_TIME", "OCUDU_UL_PHASE_SEGMENTS", "OCUDU_UL_SLOT_TRACE", "OCUDU_UL_TIMING_EVENTS",
            "OCUDU_SCHED_VERBOSE")
KNOB_EQ  = ("OCUDU_DFT_BATCH_SYMBOLS=14", "OCUDU_DFT_OPEN_BLOCK=1", "OCUDU_DFT_RELEASE_BLOCK=1",
            "OCUDU_CE_LANE_ORDER=merged",
            # OCUDU_DFT_BACKEND=vdsp joined 2026-10-01: on Apple that IS the value an unset leg resolves to
            # (dev doc 6.231-6.233), so spelling it out changes nothing. `=generic` is the A/B arm and stays
            # refused on purpose - an arm satisfies every other criterion here, which is what this check is for.
            "OCUDU_DFT_BACKEND=vdsp")
CRC_FLOOR_PCT = 60.0

knobs = re.findall(r"^knob\s*:\s*(\S+)", leg_err, re.M)
bad_knobs = [k for k in knobs if (k.split("=")[0] not in KNOB_ANY) and (k not in KNOB_EQ)]
check("DELIVERY leg: probe knobs only, or a knob at its delivery default",
      len(bad_knobs) == 0,
      ("behaviour-changing knob(s): " + " ".join(bad_knobs) +
       "  <- an arm can satisfy every other criterion here") if bad_knobs
      else (f"{len(knobs)} knob line(s): " + (" ".join(knobs) if knobs else "<none, which is the strongest case>")))

crc_n  = f(leg_err, r"(\d+) CRC-OK hop")
lane_n = f(leg_err, r"\[ul_gpu_lane\] lanes=(\d+)")
# BOUND TO TRAFFIC (2026-09-27, measured after the first HEAD pair): the ratio is decoded hops over
# scheduled hops, so with a nearly idle phone it measures the PHONE and not the code - p85 (default
# regime, no load generator, 660 hops) read 60.0% while the SAME BINARY under load (p86, 145341 hops)
# read 86.0%. Every heavy leg on record reads 60.1-96.5%, the ablation arms 43.7-58.8%.
CRC_MIN_HOPS = 20000
if crc_n is None or lane_n is None or int(lane_n) == 0:
    check("the link decoded: CRC-OK / lanes >= 60%", None, f"crc={crc_n} lanes={lane_n}")
else:
    crc_pct = 100.0 * int(crc_n) / int(lane_n)
    check_bound("the link decoded: CRC-OK / lanes >= 60%", crc_pct >= CRC_FLOOR_PCT,
                f"{crc_n}/{lane_n} = {crc_pct:.1f}%  (arms read 43.7-58.8%, every heavy leg 60.1-96.5%)",
                bound_here=(int(lane_n) >= CRC_MIN_HOPS),
                reason=f"only {lane_n} hops (< {CRC_MIN_HOPS}): the ratio would measure the phone's traffic, "
                       f"not the code (p85 read 60.0% on 660 hops while the same binary read 86.0% under load)")

# BOTH printed forms (2026-09-24): the reader used to know only "MET (8 of 8 checks applicable)", so a
# contract that FAILED - "NOT MET: 1 of 8 applicable checks failed (mode=gpu)" - printed as None, i.e. as
# "cannot read" instead of as the failure it was.
# 2026-09-27: the contract has NINE names since 6.97 (lane host participation), so the literal "8 of 8" is
# gone; what is judged is "all nine NAMES are present AND MET", which is era-proof (pitfall 28: names).
#
# 2026-10-01: TWO READINGS since the host-written grid became the DEFAULT (dev doc 6.215 (4)). With the DFT
# on the HOST the "dft radio inputs" check has no population left, so the contract reports "MET (8 of 9)"
# and this gate used to FAIL a perfectly good delivery leg - measured on p163-n78-delivered, the first
# delivery leg, while milestone_audit.sh (which already carries the rule) PASSED the same line. Accepting
# "8 of 9" is not a loosening: it is bound to the instrument that lost its population, exactly as the
# milestone audit does, and "NOT MET" / a missing name / another count still fail closed.
NAMES = ["radio sample continuity", "dft radio inputs", "zero-copy wraps", "lane host participation",
         "ce device estimates", "host device data crossings", "cfo compensation", "baseband metrics",
         "host sample assembly"]
names_found = sum(1 for n in NAMES if f"]   {n}:" in leg_err)
contract = f(leg_err, r"contract ((?:MET|NOT MET)[^\n]*)")
mode     = f(leg_err, r"contract \(mode=([a-z_]+)\)")
dft0     = f(leg_err, r"dft radio inputs: (\d+) transform")
contract_ok = (contract is not None) and mode == "gpu" and (
    contract.startswith("MET (9 of 9") or (contract.startswith("MET (8 of 9") and dft0 == "0"))
check("contract: the 9 names present and MET, mode=gpu",
      (names_found == 9) and contract_ok,
      f"names {names_found}/9; {contract} mode={mode}"
      + ("" if contract_ok else "  <- 'MET (8 of 9)' is accepted ONLY with 'dft radio inputs: 0 transform(s)'"))

# 2026-09-27: registered for the DEFAULT regime. Under load, 5.9.127's R4 licenses the opposite (the
# stressed legs on record read stale=1..2), so a stress leg is not judged on it - reported instead.
stale = f(leg_err, r"\[ul_pipeline\] stale=(\d+)")
check_bound("stale = 0", stale == "0", f"stale={stale} (regime={leg_regime})",
            bound_here=is_default, reason=f"registered for the default regime; this leg is regime={leg_regime}")

cross = f(leg_err, r"= ([0-9.]+) read\(s\) \+ ([0-9.]+) write\(s\) per hop")
check("crossings 0.00 + 0.00 per hop", cross == ("0.00", "0.00"), f"{cross}")

cbs  = f(leg_err, r"\[ul_gpu_lane\] lanes=\d+ cbs/lane=([0-9.]+)")
cbsm = f(leg_err, r"cbs/lane=[0-9.]+ \(max=(\d+)\)")
drop = f(leg_err, r"cbs/lane=[0-9.]+ \(max=\d+\) dropped=(\d+)")
# 2026-09-27: the MEAN is the criterion (it is what V4 registered and what the leg prints); the worst hop
# is reported. The old "max <= 2" literal came from the early legs, and holding it would fail every
# current leg (p72..p84 all read max=5 with cbs/lane=2.00).
check("cbs/lane <= 2.00 (mean) and dropped = 0",
      (cbs is not None) and (float(cbs) <= 2.001) and (drop == "0"),
      f"cbs/lane={cbs} dropped={drop}  [reported, not judged: worst hop max={cbsm}]")

gaps = f(leg_err, r"radio sample continuity: (\d+) gaps")
check("radio sample continuity: 0 gaps", gaps == "0", f"gaps={gaps}")

# 2026-09-27: this is the STRESS recipe's validity check - it exists so that a "loaded" leg proves it was
# loaded. The default regime is defined as "no load generator" (run_leg.sh: "default = no load generator
# (stale=0 is a criterion)"), so on a default leg it measures the operator's decision, not the code:
# p85-n78-default carried 0.03 Mbit/s and 660 hops BECAUSE no iperf3 was run, while the same binary under
# load (p86) carried 9+ Mbit/s. Bound to the regime, reported otherwise.
check_bound("VALIDITY: UL >= 2.0 Mbit/s (5x the baseline)",
            (l_mbps is not None) and (l_mbps >= 2.0),
            f"{l_mbps if l_mbps is None else round(l_mbps, 2)} Mbit/s against {b_mbps if b_mbps is None else round(b_mbps, 2)}"
            f" (regime={leg_regime})",
            bound_here=not is_default,
            reason=f"it is the stress recipe's validity check (was the leg really loaded?); this leg is "
                   f"regime=default, which run_leg.sh defines as 'no load generator'")
# 2026-09-27: registered on the n1/FDD geometry, where every slot carries uplink. A 20 MHz TDD cell with
# ul_ratio 0.30 cannot put a grant in 50% of slots by construction (the current stress legs read ~19%
# while carrying 9.3 Mbit/s, 12x the baseline), so the check is bound to the geometry it was written for.
check_bound("VALIDITY: UL grant in >= 50% of slots", (l_duty is not None) and (l_duty >= 50.0),
            f"{l_duty if l_duty is None else round(l_duty, 1)}% against {b_duty if b_duty is None else round(b_duty, 1)}% "
            f"(config={os.path.basename(leg_cfg)}, regime={leg_regime})",
            bound_here=is_n1_fdd,
            reason=f"registered on the n1/FDD geometry; this leg is {os.path.basename(leg_cfg)} (ul_ratio 0.30)")

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

# ---- §6.198④'s DL-timeliness VALIDITY gate, RESTATED (dev doc 6.230; user ruling 2026-10-01) --------
# OLD TEXT: "dl_tx_slack ... AT/BELOW 0 == 0 ... non-zero => this leg's link conclusions do not count".
# It was written for the p153 transport incident (16/46/36 there, against 0 on p151/p152) and it was
# CALIBRATED ON THE BROKEN INSTRUMENT: pairing the block's FIRST sample with the call's return inflated
# every margin by one block (500us whole-slot, 35.7us symbol), so p151's "+416us" was really about
# -84us - the healthy calibration legs had late hand-overs too, hidden by the inflation. With the clock
# map fixed (6.213) AND the teardown window excluded (6.219 (8)) the IN-STREAM reading on six
# transport-clean legs is 1..6 (p164 2 and 6, p165 2, p167 1, p168 1, p169 2), so the literal 0 is
# unreachable, and the teardown tail that used
# to supply most of the count is no longer in the population at all.
# What replaces it is the CONJUNCTION THAT STILL CATCHES THE INCIDENT, and it keeps the incident's
# shape: p153 was "late AND a slip/recv storm" (slip 21/9/467, recv 35/12/570), while p164/p165/p166
# (the phone's data toggle off) were "2 late, NO storm" - this gate says "transport healthy, look
# elsewhere" for the latter, which is the direction that would have saved two of those three legs.
# NOTE the placement: this is a JUDGED row, so it must be appended BEFORE the rows are printed below.
tx_windowed = f(leg_err, r"\[dl_tx_slack\][^\n]*excluded \d+ hand-over")
tx_pop      = f(leg_err, r"\[dl_tx_slack\][^\n]*; every number above is over the (\d+) hand-over")
late_in     = f(leg_err, r"\[dl_tx_slack\][^\n]*AT/BELOW 0=(\d+)")
slips       = f(leg_err, r"\[ul_rx_timing\][^\n]*slip\(max=\d+us over 1ms=(\d+)")
recvs       = f(leg_err, r"\[ul_rx_timing\][^\n]*recv\(max=\d+us over 1ms=(\d+)")
gaps_txt    = f(leg_err, r"radio sample continuity: (\d+) gaps")
tx_name = ("VALIDITY (6.198 (4) restated): AT/BELOW 0 <= 10 inside the stream AND no transport storm "
           "(slip/recv over 1ms <= 10, gaps = 0)")
if tx_windowed is None:
    check_bound(tx_name, None, "", bound_here=False,
                reason="the window-scoped reading needs the 2026-10-01 instrument ([dl_tx_slack] carrying "
                       "'excluded N hand-over(s)'); this leg predates it, so its AT/BELOW 0 counts the "
                       "teardown tail too")
elif None in (late_in, slips, recvs, gaps_txt):
    check(tx_name, None, f"cannot read every term: AT/BELOW 0={late_in} slip={slips} recv={recvs} gaps={gaps_txt}")
else:
    ok = (int(late_in) <= 10) and (int(slips) <= 10) and (int(recvs) <= 10) and (gaps_txt == "0")
    check(tx_name, ok,
          f"in-stream AT/BELOW 0={late_in} (population {tx_pop}), slip={slips}, recv={recvs}, gaps={gaps_txt}"
          + ("" if ok else "  <- the p153 shape was 'late AND a slip/recv storm'; a small count with a clean "
                           "transport is NOT a transport fault (6.230)"))

print(f"pass 5 gate: {leg_lab}   (baseline {base_lab}, slot {slot_ms} ms)")
print(f"  leg log: {os.path.basename(leg_log)}")
print(f"  leg identity: regime={leg_regime}  config={os.path.basename(leg_cfg)}")
print()
for verdict, name, detail in rows:
    print(f"  [{verdict:<16}] {name}")
    print(f"                     {detail}")
red = sum(1 for v, _, _ in rows if v in ("FAIL", "RED (cannot read)"))
na  = sum(1 for v, _, _ in rows if v == "NOT JUDGED")
judged = len(rows) - na
tail = "" if red == 0 else f"  --  {red} to explain"
if na:
    tail += f"  ({na} NOT JUDGED: binding does not match this leg)"
print()
print(f"  {judged - red} of {judged} judged checks pass" + tail)

# ---- INFORMATION, deliberately NOT a tenth row ---------------------------------------------------
# The milestone table (5.9.54 1) carries the row "dropped slots / RF real-time failures | 0 / 0", and the
# audit of 5.9.120 found that half of it is a ONE-LEG claim: s47 and s67 read 0, but s62=2, s63=4,
# s64b=21, s65=8, s66=40 and s69=33 (all `[RF] [W] Real-time failure in RF: underflow|late`). It is
# printed here rather than judged because NO THRESHOLD IS REGISTERED, and inventing one inside a gate
# is how a criterion becomes whatever the last person wanted. What it must not do is stay invisible:
# a leg with 33 of them currently scores 7 of 9 and looks like the one with 0.
#
# 2026-09-27: the RF count also moves with the LINK, not only with the code - the current stress legs
# (p65..p84) read 1974-3500 of them with `gaps=0` and the pool green. V3 was re-ruled on 2026-09-26 for
# exactly that reason (dev doc 6.122): "RF <= 10" became "gaps == 0 AND the per-DL-transmission real-time
# failure rate inside the mode's band". This line stays INFO, and that rate is what V3 is judged by.
rtf = len(re.findall(r"Real-time failure in RF", leg_txt))
print()
print(f"  [INFO           ] RF real-time failures in this leg's .log: {rtf}")
print(f"                     not a criterion (no threshold registered; V3 re-ruled 2026-09-26: gaps==0 + the per-DL rate band)")

# ---- TRANSPORT HEALTH (INFO, and the covariate every receive-tail claim has to carry) --------------
# Dev doc 6.157 (legs p122/p123/p124, 2026-09-28): the receive tail measured by `[ul_rx_timing] recv over
# 1ms / calls` is NOT a property of the RX host path. Across 86 legs it tracks the DL TRANSMIT call's own
# `over 1ms` share - another thread, another direction, a channel this code does not drive - at Spearman
# 0.986, with the two shares at a stable ratio of 0.75-1.0. Two legs of the SAME mode, recipe and knob state
# (p122 / p123) differed 79x in the tail and 130x in this covariate, so a leg whose transport is missing its
# deadlines cannot be used as evidence about the RX path (and, symmetrically, is the only kind of leg on
# which a receive-tail reading means anything at all).
#
# The band is MEASURED, not chosen: of the 86 legs the healthy side tops out at 0.0064% and the sick side
# starts at 0.0248%, a four-fold gap. It is INFO rather than a tenth judged row because a leg can be sick
# here and still be a perfectly good latency/contract leg (p116/p117/p123 all carry valid V1 and contract
# readings); failing one would fail legs nothing about them is wrong, which is how a gate stops being read.
slack = f(leg_err, r"\[dl_tx_slack\][^\n]*?below 1ms=(\d+),[^\n]*?AT/BELOW 0=(\d+)")
call  = f(leg_err, r"\[dl_tx_call\] calls=(\d+)[^\n]*?over 1ms=(\d+)")
rx    = f(leg_err, r"\[ul_rx_timing\] calls=(\d+) recv\(max=\d+us over 1ms=(\d+)")
print()
if slack and call and rx:
    rate   = 100.0 * int(call[1]) / int(call[0])
    rxrate = 100.0 * int(rx[1]) / int(rx[0])
    # Three bands, MEASURED: over the legs on record the healthy side tops out at 0.0064% and the sick side
    # starts at 0.0248%. The middle band is reported as such rather than folded into either side - p114 sat
    # there (0.0218%) while p125 sat ON the healthy edge (0.0058%, with a 0.0095% receive tail), and calling
    # that middle band "healthy" would license a tail reading from a leg whose transport was not clean.
    # Same bands as wip/leg_triage.sh.
    verdict = "HEALTHY" if rate < 0.0064 else ("MARGINAL" if rate < 0.0248 else "SICK")
    print(f"  [INFO           ] transport health (DL side, dev doc 6.157): dl_tx_call over 1ms = "
          f"{call[1]}/{call[0]} = {rate:.4f}%  ->  {verdict}")
    print(f"                     this leg's RX tail = {rx[1]}/{rx[0]} = {rxrate:.4f}%; "
          f"dl_tx_slack below 1ms={slack[0]}, AT/BELOW 0={slack[1]}")
    print("                     " + {"HEALTHY": "an RX-tail reading from this leg IS usable",
                                   "MARGINAL": "cite this leg's RX tail WITH its transport (it is not clean)",
                                   "SICK": "do NOT attribute this leg's RX tail to code: at this rate the tail follows the transport"}[verdict]
          + "  (bands over the legs on record: healthy <=0.0064%, marginal 0.0064-0.0248%, sick >=0.0248%)")
else:
    print(f"  [INFO           ] transport health (DL side, dev doc 6.157): cannot read "
          f"([dl_tx_slack]/[dl_tx_call]/[ul_rx_timing] missing from this leg's report)")
    print(f"                     'cannot read' is not 'healthy': do not use this leg for a receive-tail claim")

PY
