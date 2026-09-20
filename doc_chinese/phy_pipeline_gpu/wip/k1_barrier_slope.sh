#!/usr/bin/env bash
# K1's barrier slope, measured ON AIR: two legs, one with OCUDU_CE_INV_BARRIERS=8 and one without.
#
# Why on air: the offline machine shares its GPU with the host's own GUI (design document 5.8.20), so the
# same configuration has been read at 45.7us and 206.4us in different blocks of wall clock and the
# offline geometry sweep contradicts the hop-level one. On air the gNB has the device to itself.
#
# Why the probe is legal in a leg: inserting a threadgroup barrier changes no published value (it orders
# nothing that is not already ordered), so the probe leg runs the SHIPPED configuration and the legs stay
# comparable. What is being measured is the marginal cost of a barrier inside K1, and from it the share
# of K1's cost that barriers are:
#
#   slope      = d(ch_wt) / (8 extra barriers x 54 pivots)   us per barrier
#   barrier    = 122 x slope                                  the barriers K1 already pays for
#   K1         = the whole dispatch (needs INV_REPEAT=3 to measure, not in these legs)
#
# usage: bash wip/k1_barrier_slope.sh <control-leg.log> <probe-leg.log>
set -u
A=${1:?control leg log (OCUDU_CE_INV_BARRIERS unset)}
B=${2:?probe leg log (OCUDU_CE_INV_BARRIERS=8)}

grab()
{
  local log=$1 what=$2
  case "$what" in
    ch_wt)   grep -oE "ch_wt=[0-9.]+" "$log" | head -1 | cut -d= -f2 ;;
    eq)      grep -oE "eq_demap=[0-9.]+" "$log" | head -1 | cut -d= -f2 ;;
    cbs)     grep -oE "cbs/lane=[0-9.]+" "$log" | head -1 | cut -d= -f2 ;;
    lane)    grep -oE "\[ul_gpu_pipeline\] samples=[0-9]+ mean=[0-9.]+us median=[0-9.]+" "$log" | head -1 ;;
    knobs)   grep -E "^knob" "$log.stderr" 2>/dev/null | tr '\n' ' ' ;;
    rtf)     grep -oE "Real-time failures   : [0-9]+" <(bash "$(dirname "$0")/leg_report.sh" "$log" 2>/dev/null) | grep -oE "[0-9]+$" ;;
  esac
}

aw=$(grab "$A" ch_wt); bw=$(grab "$B" ch_wt)
ae=$(grab "$A" eq);    be=$(grab "$B" eq)

echo "control : $(basename "$A")"
echo "  ch_wt=$aw  eq_demap=$ae  cbs/lane=$(grab "$A" cbs)  $(grab "$A" lane)"
echo "probe   : $(basename "$B")"
echo "  ch_wt=$bw  eq_demap=$be  cbs/lane=$(grab "$B" cbs)  $(grab "$B" lane)"
echo "  knobs: $(grab "$B" knobs)"
echo
if [ -z "$aw" ] || [ -z "$bw" ]; then
  echo "FAILED: one of the legs has no busy split (did the fused lane run?)"
  exit 2
fi
python3 - "$aw" "$bw" <<'PY'
import sys
a, b = float(sys.argv[1]), float(sys.argv[2])
extra = 8 * 54           # the probe's extra barriers per K1 dispatch, at the air order n = 54
slope = (b - a) / extra
print(f"  d(ch_wt)            = {b - a:+.1f} us  ({a:.1f} -> {b:.1f})")
print(f"  slope               = {slope:.3f} us per threadgroup barrier")
print(f"  K1's 122 barriers   = {122 * slope:.1f} us per hop")
print(f"  as a share of ch_wt = {100 * 122 * slope / a:.0f}% of the control leg")
print()
print("  reading it:")
print("    slope ~0.0-0.3  -> barriers are a MINORITY of K1; the per-element work is the target")
print("    slope ~1-3      -> barriers dominate; fewer-barrier or multi-threadgroup forms win")
print("    d(ch_wt) <= 0   -> the probe did not reach the device (check the knob reached the process)")
PY
