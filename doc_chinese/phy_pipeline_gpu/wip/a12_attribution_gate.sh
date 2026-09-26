#!/usr/bin/env bash
# Evaluates the criteria PRE-REGISTERED in design document 5.9.118 for the A1-2 attribution leg:
# the three "Plain route by why:" counters, read against the two hypotheses that section names
# (the puxch path with no block open, vs the PRACH demodulator's SECOND engine instance).
#
# Why a separate gate instead of a sentence in the log: 5.9.111 (3) had already registered the two
# DEFECT branches, and this leg's most likely reading ("unblocked is the majority") is exactly the
# first of them - so without a gate that also carries the third branch, the reading would be filed
# as "nobody told the engine the slot, go fix the caller" and someone would go and "fix" a path that
# is correct by construction (the PRACH demodulator never gets a lane slot, by design).
#
# The criteria are the ones in 5.9.118 (4) - C1..C5 - and they are evaluated, not chosen, here:
#   C1  plain_with_block + plain_without_block == the plain route total   (the instrument's identity)
#   C2  plain_without_lane_slot <= plain_without_block                    (it is a subset of it)
#   C3  plain_block == 0 AND plain_no_slot/plain_none >= 0.999            (the third branch's prediction)
#   C4  plain == 12*round(T/10ms) + 1, +-1 occasion                       (PRACH B4, index 159: 12/10ms)
#   C5  the leg is valid at all: contract MET mode=gpu, stale=0, crossings 0.00+0.00
#       (the count is dynamic - it was 8 before the lane-host-participation check landed on 2026-09-26)
# "Cannot read" is RED, never absent - the lesson of 5.9.97 (a hard gate printed "None" and looked
# exactly like a pass).
#
# TWO things this gate is bound to, and it now says so instead of judging something else:
#   * REGIME. C5's pre-registration is the DEFAULT regime's validity ("contract 8/8, stale=0, crossings
#     0.00+0.00, gaps=0" - 5.9.118 (3)). Under a load generator `stale > 0` is the pre-registered
#     EXPECTATION (wall_ab's R4, 5.9.127), so --regime=stress drops that one condition from C5.
#   * GEOMETRY. C4's identity ("plain route == 12*round(T/10ms) + 1") is an empirical law of the n78
#     PRACH geometry this file was written for: format B4 at one occasion per radio frame, 12 IDFTs
#     per occasion. It is NOT a law about "PRACH in general" - and on the n1 5 MHz FDD config the
#     premise is absent outright (measured 2026-09-24: the leg detected preambles, but the Metal DFT
#     engine's own counter reads `dft commits=1`, i.e. only its warm-up: PRACH's IDFTs fell back to
#     the CPU implementation, which the Metal factory's comment names as a supported fallback). On
#     such a leg C4 is NOT JUDGED, and it must not be reported as a failure of A1-2.
#
# usage:
#   bash a12_attribution_gate.sh s69-a12-n78            # newest leg carrying that label
#   bash a12_attribution_gate.sh --slot-ms=0.5 s69-a12-n78
#   bash a12_attribution_gate.sh --regime=stress s82-phases-heavy-n78
#   bash a12_attribution_gate.sh --self-test            # the gate's own arms: can it go red AND green?
set -u

LEG=""
SLOT_MS=0.5          # a 30 kHz cell: this gate is written for the n78 PRACH geometry (12 per 10 ms)
SYMS_PER_SLOT=14
SELF_TEST=0
REGIME=default       # C5 keeps `stale=0` here; --regime=stress drops it (5.9.127 R4)
for a in "$@"; do
  case "$a" in
    --slot-ms=*)        SLOT_MS=${a#*=} ;;
    --symbols-per-slot=*) SYMS_PER_SLOT=${a#*=} ;;
    --regime=*)         REGIME=${a#*=} ;;
    --self-test)        SELF_TEST=1 ;;
    *)                  LEG=$a ;;
  esac
done
case "$REGIME" in default|stress) ;; *) echo "regime must be default or stress" >&2; exit 2 ;; esac

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs

if [ "$SELF_TEST" = 1 ]; then
  # Two synthetic legs, evaluated by the SAME code path below: one where the third branch holds and
  # one where it does not. A gate that only ever sees the real log has not been shown to be able to
  # go red (and this one is meant to be able to contradict its own section).
  T=$(mktemp -d)
  mk() { # $1 = file, $2 = plain_none, $3 = plain_no_slot, $4 = plain_block
    { printf 'Plain route by why: %s got their own command buffer (%s of those before any slot was told to the engine), %s joined an open block\n' "$2" "$3" "$4"
      printf '   dft radio inputs: 391909 transform(s) went through the two submit routes: the hand-over route carried 280560 (71.6%%), the plain route %s; 0 buffer wrap(s) had to stage a host copy\n' "$(( $2 + $4 ))"
      printf '   host device data crossings: 0 host read(s) (0 bytes) and 0 host write(s) (0 bytes) of device data over 9505 device hop(s) = 0.00 read(s) + 0.00 write(s) per hop\n'
      printf '   host sample assembly: 2598400 of 2598400 symbols read where the radio put them, 0 copied into a symbol buffer (mode=gpu) -> OK\n'
      printf '   radio sample continuity: 0 gaps over 185612 blocks (0 samples missing or repeated), 0 timestamp-0 blocks -> OK\n'
      printf '   dft radio inputs: OK\ncontract (mode=gpu):\ncontract MET (9 of 9 checks applicable)\n'
      printf '[ul_pipeline] stale=0\n'
      # The GEOMETRY PREMISE C4 needs (see the header): the registered n78 config, and PRACH actually on
      # the engine (commits > 1, i.e. more than the warm-up). Without these the gate must NOT judge C4 -
      # and a self-test whose legs lack them would silently stop exercising C4 altogether.
      printf 'cell config   : configs/gnb_rf_b200_tdd_n78_20mhz.yml\n'
      printf '[metal_stats] dft commits=111362 transforms=%s waits=1 slots_in_flight=0 radio_inputs=1\n' "$(( $2 + $4 ))"
    } > "$1.stderr"
    : > "$1"
  }
  mk "$T/branch3" 111361 111360 0            # C3 must PASS (PRACH instance), C4 must PASS
  mk "$T/defect"  111361 7      111354       # C3 must FAIL (a slotted instance did not join a block)
  # A pre-5.9.113 leg: the counters do not exist. "Cannot read" has to come out RED, not absent.
  grep -v "Plain route by why" "$T/branch3.stderr" > "$T/oldfmt.stderr"
  : > "$T/oldfmt"
  echo "self-test: branch3 (expect C1-C5 all PASS)"
  bash "$0" --slot-ms=0.5 "$T/branch3" | grep -E "^\s+\[|criteria pass" | sed 's/^/    /'
  echo "self-test: defect (expect C3 FAIL, C1 still PASS)"
  bash "$0" --slot-ms=0.5 "$T/defect" | grep -E "^\s+\[|criteria pass" | sed 's/^/    /'
  echo "self-test: old format (expect C1-C3 RED: cannot read)"
  bash "$0" --slot-ms=0.5 "$T/oldfmt" | grep -E "^\s+\[|criteria pass" | sed 's/^/    /'
  rm -rf "$T"
  exit 0
fi

resolve() {
  local hit
  if [ -f "$1" ]; then printf '%s' "$1"; return; fi
  hit=$(ls -1t "$LOGDIR"/gnb_*"$1"*.log 2>/dev/null | grep -v '\.stderr$\|\.stdout$' | head -1)
  [[ -n $hit ]] || { echo "no leg matched '$1' in $LOGDIR" >&2; exit 2; }
  printf '%s' "$hit"
}

[ -n "$LEG" ] || { echo "usage: bash a12_attribution_gate.sh [--slot-ms=N] <leg-label|path>" >&2; exit 2; }
LEG_LOG=$(resolve "$LEG")

python3 - "$LEG_LOG" "$SLOT_MS" "$SYMS_PER_SLOT" "$REGIME" <<'PY'
import os, re, sys

leg_log, slot_ms, syms_per_slot, regime = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
err_txt = open(leg_log + ".stderr", errors="replace").read() if os.path.exists(leg_log + ".stderr") else ""
txt     = open(leg_log, errors="replace").read() if os.path.exists(leg_log) else ""

def f(where, rx):
    m = re.findall(rx, where)
    return m[-1] if m else None

rows = []
judged = 0
not_judged = []
def check(name, ok, detail):
    global judged
    verdict = "PASS" if ok is True else ("RED (cannot read)" if ok is None else "FAIL")
    judged += 1
    rows.append((verdict, name, detail))
def skip(name, reason):
    if not not_judged:
        rows.append(("n/a (not judged)", name, reason))

# ---- the three counters (5.9.113) and the plain-route total they must add up to -------------------
why = f(err_txt, r"Plain route by why: (\d+) got their own command buffer \((\d+) of those before any slot "
                 r"was told to the engine\), (\d+) joined an open block")
plain = f(err_txt, r"the plain route (\d+)")
plain_src = "contract line"
if plain is None:
    # Fallback for a leg older than 5.9.99, whose contract line read "N of M transforms" (two different
    # populations). The population is the same one and it is defined just as precisely: `transforms` is
    # incremented by commit_front_end() (ocudu_dft_metal_engine.mm:112 via :674), i.e. it counts exactly
    # the transforms that went out through the plain route, and never the handed-over ones (radio_inputs).
    plain = f(err_txt, r"\[metal_stats\] dft commits=\d+ transforms=(\d+)")
    plain_src = "[metal_stats] dft transforms (pre-5.9.99 line)"
if why is None:
    plain_none = plain_no_slot = plain_block = None
else:
    plain_none, plain_no_slot, plain_block = (int(x) for x in why)
plain_total = int(plain) if plain else None

check("C1 plain_without_block + plain_with_block == plain route",
      None if (why is None or plain_total is None) else (plain_none + plain_block == plain_total),
      "instrument absent (predates 5.9.113)" if why is None else
      f"{plain_none} + {plain_block} = {plain_none + plain_block} vs plain route {plain_total}")

check("C2 plain_without_lane_slot <= plain_without_block",
      None if why is None else (plain_no_slot <= plain_none),
      "n/a" if why is None else f"{plain_no_slot} <= {plain_none}")

ratio = None if (why is None or plain_none == 0) else plain_no_slot / plain_none
check("C3 plain_with_block == 0 AND (no-slot / no-block) >= 0.999  <- 5.9.118 (3) prediction",
      None if why is None else (plain_block == 0 and ratio is not None and ratio >= 0.999),
      "n/a" if why is None else
      f"block={plain_block}, no-slot/no-block={ratio if ratio is None else round(ratio, 4)}"
      + ("" if (ratio is not None and ratio >= 0.999) else "  <- a slotted instance did NOT join a block: 5.9.111 (3) branch 2"))

# ---- the geometry premise C4 is a law of ---------------------------------------------------------
leg_cfg  = f(err_txt, r"cell config\s*:\s*(\S+)")
commits  = f(err_txt, r"\[metal_stats\] dft commits=(\d+)")
prach_on_engine = None if commits is None else (int(commits) > 1)
n78_geom = (leg_cfg is not None) and ("tdd_n78" in leg_cfg)
premise_reason = None
if leg_cfg is None or commits is None:
    premise_reason = ("cannot read the premise: " + ("no `cell config` provenance in the leg" if leg_cfg is None else "")
                      + (" and " if leg_cfg is None and commits is None else "")
                      + ("no `[metal_stats] dft commits=` line" if commits is None else ""))
elif not prach_on_engine:
    premise_reason = (f"PRACH does not run on the Metal DFT engine here (dft commits={commits}: only its warm-up, "
                      f"so the IDFTs took the CPU fallback the Metal factory documents) - there is no plain-route "
                      f"signal to attribute")
elif not n78_geom:
    premise_reason = (f"leg ran {leg_cfg}, not the registered n78 geometry this identity was measured on "
                      f"(format B4, 12 IDFTs per occasion, one occasion per radio frame)")

# ---- C4: the PRACH rate. n78 index 159 = format B4 (12 symbols) once per 10 ms radio frame --------
# T comes from the leg's own symbol count, NOT from the log file's timestamps: the span includes the
# startup and the shutdown block, which is ~4 s of a ~95 s leg - enough to misjudge a 10 ms period.
sym = f(err_txt, r"host sample assembly: \d+ of (\d+) symbols")
T = None if sym is None else int(sym) / syms_per_slot * slot_ms / 1000.0
occ = None if T is None else int(round(T / 0.010))
exp = None if occ is None else 12 * occ + 1
if premise_reason is None:
    check("C4 plain route == 12*round(T/10ms) + 1  (PRACH B4 at one occasion per radio frame)",
          None if (exp is None or plain_total is None) else abs(plain_total - exp) <= 12,
          "no symbol count" if exp is None else
          f"T={T:.1f}s -> {occ} occasions -> expected {exp}, read {plain_total}"
          + (f" (implied period {1000.0 * T / occ:.2f} ms)" if occ else ""))
else:
    skip("C4 plain route == 12*round(T/10ms) + 1  (PRACH B4 at one occasion per radio frame)",
         "NOT JUDGED: " + premise_reason)

# ---- C5: is the leg valid at all ------------------------------------------------------------------
contract = f(err_txt, r"contract ((?:MET|NOT MET) \(\d+ of \d+ checks[^)]*\))")
mode     = f(err_txt, r"contract \(mode=([a-z_]+)\)")
stale    = f(err_txt, r"\[ul_pipeline\] stale=(\d+)")
cross    = f(err_txt, r"= ([0-9.]+) read\(s\) \+ ([0-9.]+) write\(s\) per hop")
gaps     = f(err_txt, r"radio sample continuity: (\d+) gaps")
# In the STRESS regime the stale condition is dropped, not relaxed: 5.9.127's R4 pre-registers
# `stale > 0` as the expectation under load, so requiring 0 here would contradict the registration.
stale_ok = (stale == "0") if regime == "default" else (stale is not None)
check("C5 leg valid: contract MET mode=gpu, crossings 0.00+0.00, 0 gaps"
      + (", stale=0" if regime == "default" else f"  [stress regime: stale={stale} is expected, 5.9.127 R4]"),
      (contract is not None and re.match(r"MET \([0-9]+ of [0-9]+", contract) is not None and mode == "gpu"
       and stale_ok and cross == ("0.00", "0.00") and gaps == "0"),
      f"{contract} mode={mode} stale={stale} crossings={cross} gaps={gaps}")

print(f"A1-2 attribution gate (5.9.118): {os.path.basename(leg_log)}   slot {slot_ms} ms")
if T is not None:
    # plain_total is None on a leg older than 5.9.99 (the line then read "N of M transforms", comparing
    # two different populations) - say so instead of dividing by it.
    print(f"  plain route {plain_total if plain_total is not None else '<unreadable>'}"
          + (f" over T={T:.1f}s = {plain_total / T:.0f}/s" if (T and plain_total is not None) else f" over T={T:.1f}s")
          + (f";  PRACH occasions {occ} -> {occ * 12}/s expected" if occ else "")
          + f"   [total from: {plain_src}]")
print()
for verdict, name, detail in rows:
    print(f"  [{verdict:<16}] {name}")
    print(f"                     {detail}")
red = sum(1 for v, _, _ in rows if v not in ("PASS", "n/a (not judged)"))
print()
print(f"  {judged - red} of {judged} criteria pass"
      + ("" if red == 0 else f"  --  {red} to explain (see 5.9.118 (6) for what each shape means)"))
for _, name, why in not_judged:
    print(f"  NOT JUDGED: {name.split('  (')[0]} -- {why}")
PY
