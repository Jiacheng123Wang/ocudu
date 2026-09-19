#!/usr/bin/env bash
# The fused lane's own A/B: the SAME binary, over the three lane ORDERS of the estimator's deferred hop
# (OCUDU_CE_LANE_ORDER, see ce_lane_order in ocudu_metal_mmse_engine.h):
#
#   event      (default) the estimator commits its OWN command buffer as soon as its dispatches are
#                        encoded; the lane burst waits for it through the back-end stage fence
#   wait                 the same own command buffer, waited for by the host (the pre-S-7g-16 route)
#   burst                the estimator's dispatches ride the lane's shared command buffer (S-7g-16 b)
#
# Why this gate exists and why it is not one of the three capture nets: those compare the tree against an
# ARCHIVED reference binary, which cannot have the knob - so they can prove the default route is unchanged,
# but they cannot say anything about the other two orders. The three orders publish the very same numbers
# or one of them is wrong (they differ only in WHOSE command buffer carries the work and WHO waits for it),
# so the gate is byte identity between the orders of one binary, over the whole corpus and over the ROUTES
# the fusion has to survive:
#
#   --metal --device-grid            the air route: the estimator's device stages are live (K0-a included)
#   --metal --device-grid OCUDU_CE_GPU_INVERT=0   host inversion + the standalone correlation build
#   --metal OCUDU_CE_CPU_CE=1        the estimates are read from HOST memory: the estimator completes
#                                    BEFORE the receiving chain submits the equalization (the order that
#                                    made the burst-order completion commit the burst itself - 48.188)
#   --metal OCUDU_CE_CPU_LS=1        the host LS pre-stage (no K0-a): the strict net's route
#
# usage: bash ab_fused_lane.sh [nof-captures] [corpus-dir]
set -u
N=${1:-27}
CORPUS=${2:-$( [ -d /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus ] && echo /Users/jiachengwang/dev/ocudu/doc_chinese/work_tmp/corpus || echo /tmp/corpus )}
NEW=/Users/jiachengwang/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CAPTURES=$(ls "$CORPUS"/*.bin 2>/dev/null | sed -E 's/\.bin$//' | head -"$N")
if [ -z "$CAPTURES" ]; then echo "no captures in $CORPUS: nothing was compared" >&2; exit 2; fi

rc_all=0
for route in \
  "--metal --device-grid" \
  "--metal --device-grid OCUDU_CE_GPU_INVERT=0" \
  "--metal OCUDU_CE_CPU_CE=1" \
  "--metal OCUDU_CE_CPU_LS=1" ; do
  set -- $route
  envs=""; mode=""
  for a in "$@"; do
    case "$a" in
      *=*) envs="$envs $a" ;;
      *)   mode="$mode $a" ;;
    esac
  done
  # The reference of the comparison is the DEFAULT order, run with nothing set: that is also the pin that
  # the default really is the event order (a default that silently drifted would differ on every capture).
  bad=""; n=0
  for c in $CAPTURES; do
    n=$((n+1)); rm -f /tmp/f0* /tmp/f1*
    env                     $envs "$NEW" "$c" $mode --out /tmp/f0 >/dev/null 2>&1
    ref=$(ls /tmp/f0*_ce.txt 2>/dev/null | head -1)
    if [ -z "$ref" ]; then bad="$bad $(basename "$c")(nodump)"; continue; fi
    for order in event wait burst; do
      env OCUDU_CE_LANE_ORDER=$order $envs "$NEW" "$c" $mode --out /tmp/f1 >/dev/null 2>&1
      b=$(ls /tmp/f1*_ce.txt 2>/dev/null | head -1)
      if [ -z "$b" ]; then bad="$bad $(basename "$c")/$order(nodump)"; continue; fi
      tot=0
      for suf in _llr.bin _h.bin .bin _ce.txt; do
        d=$(cmp -l "${ref%_ce.txt}$suf" "${b%_ce.txt}$suf" 2>/dev/null | wc -l | tr -d ' ')
        tot=$((tot + d))
      done
      [ "$tot" = "0" ] || bad="$bad $(basename "$c")/$order($tot)"
      rm -f /tmp/f1*
    done
    rm -f /tmp/f0*
  done
  echo "lane-order A/B [$mode $envs]: $n capture(s) x 3 orders (event/wait/burst; the reference is the default)"
  [ -n "$bad" ] && echo "   differing:$bad"
  [ -n "$bad" ] && rc_all=1
done
echo "FUSED_AB_RC=$rc_all"
exit $rc_all
