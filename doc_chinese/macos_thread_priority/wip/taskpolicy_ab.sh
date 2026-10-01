#!/usr/bin/env bash
# The P2 lever: change the RUNNING gNB's QoS TIER with `taskpolicy`, without touching the binary (zero code).
#
# WHY THIS IS SEPARATE FROM EVERYTHING ELSE IN THIS DIRECTORY. P1's readback showed that the per-thread QoS class
# this project requests has NEVER been in effect on macOS: the POSIX call that follows it silently erases it, and
# the erasure is irreversible for the life of the thread (dev doc 10.5). What is left as a host-side lever is the
# PROCESS-wide QoS tier, which `taskpolicy` can set on a live process - and that is exactly what makes it useful
# here: two arms can be compared INSIDE ONE LEG (before / after, same binary, same radio, same phone, same
# thermal state), which no rebuild-based A/B can do.
#
# ⚠⚠ AN ARM, NOT AN ACCEPTANCE LEG. Applying a tier changes the process's scheduling, so the leg carrying it is a
# measurement arm: it can satisfy every other criterion in leg_gate.sh and still not be delivery evidence (that is
# the rule the gate's fail-closed knob list exists for, and OCUDU_SCHED_* is deliberately NOT in the whitelist).
# Label such a leg `<label>-tier` and never mix it into an audit.
#
# usage:
#   sudo -E bash wip/taskpolicy_ab.sh <leg-label> scan                 # try tiers 1..5, read each one back
#   sudo -E bash wip/taskpolicy_ab.sh <leg-label> set --latency=5      # set one tier (mid-leg) and read it back
#   sudo -E bash wip/taskpolicy_ab.sh <leg-label> clear                # put the process back on its default tier
#
# products: work_tmp/taskpolicy_<label>_<action>.txt  (the tool's own output plus a readback when taskinfo is
#           available), and one line on stdout saying which tier is in force NOW - so a leg's timeline can be cut
#           at that instant without guessing.
#
# WHAT THE TIERS ARE (measured on this machine, 2026-10-01): `taskpolicy -l` accepts 1..5 and refuses 6 and above
# ("Could not parse '6' as a qos tier"), which maps to XNU's LATENCY_QOS_TIER_5 (1) down to LATENCY_QOS_TIER_1 (5)
# - i.e. the NUMBER GOES UP as the tier gets more latency-sensitive, the opposite of the constant's name. The
# scan arm prints the readback next to the number for exactly that reason.
set -u

LABEL="${1:-}"
ACTION="${2:-}"
LATENCY=""
THROUGHPUT=""
for a in "$@"; do
  case "$a" in
    --latency=*)    LATENCY="${a#*=}" ;;
    --throughput=*) THROUGHPUT="${a#*=}" ;;
  esac
done
if [[ -z $LABEL || -z $ACTION ]]; then
  echo "usage: sudo -E bash wip/taskpolicy_ab.sh <leg-label> {scan|set|clear} [--latency=N] [--throughput=N]" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "$HERE/.." && pwd)/work_tmp"
mkdir -p "$OUT"
BASE="$OUT/taskpolicy_${LABEL}_${ACTION}_$(date +%H%M%S)"

PID="$(pgrep -f 'build/apps/gnb/gnb' | head -1)"
if [[ -z $PID ]]; then
  echo "[taskpolicy] no gnb process found (pgrep -f 'build/apps/gnb/gnb')" >&2
  exit 3
fi
if [[ $EUID -ne 0 ]]; then
  echo "[taskpolicy] ⚠ not root: changing ANOTHER process's tier needs root, and so does the taskinfo readback." >&2
  echo "[taskpolicy]   (taskpolicy on a process you own works unprivileged - measured - but the gNB runs under sudo.)" >&2
fi

# The readback. taskinfo needs root and IS NOT FREE - it is one of the tools that produced the 153 ms stall of leg
# p182_1726 (dev doc 10.3) - so it is taken ONCE per action, at the instant the tier is changed, and its cost is
# inside the arm's own leg by definition.
readback() {  # <suffix>
  {
    echo "== $(date -u +%Y-%m-%dT%H:%M:%SZ) pid=$PID action=$ACTION latency=${LATENCY:-<unchanged>} throughput=${THROUGHPUT:-<unchanged>}"
    echo "-- taskpolicy output --"
    cat "$1" 2>/dev/null || true
    echo "-- taskinfo (QoS / tier lines) --"
    taskinfo "$PID" 2>&1 | grep -iE "qos|tier|latency|throughput|clamp|managed|boost" || echo "SKIPPED (needs root)"
  } >>"$BASE.txt"
}

case "$ACTION" in
  scan)
    # The scan exists to answer "which tiers does THIS machine accept, and what do they read back as" - a tier
    # that is silently ignored would otherwise look exactly like a tier that does not help.
    : >"$BASE.txt"
    for t in 1 2 3 4 5; do
      TMP="$(mktemp)"
      taskpolicy -l "$t" -p "$PID" >"$TMP" 2>&1
      echo "[taskpolicy] -l $t (rc=$?)" | tee -a "$BASE.txt"
      readback "$TMP"
      rm -f "$TMP"
      sleep 2   # let the scheduler settle: the readback is a booking, not an instant effect
    done
    echo "[taskpolicy] scan written to $BASE.txt (the tier is now whatever the LAST loop iteration set: run"
    echo "[taskpolicy]   the 'clear' action before flying anything you intend to read)."
    ;;
  set)
    if [[ -z $LATENCY && -z $THROUGHPUT ]]; then
      echo "[taskpolicy] 'set' needs --latency=N and/or --throughput=N" >&2
      exit 2
    fi
    ARGS=()
    [[ -n $LATENCY ]]    && ARGS+=(-l "$LATENCY")
    [[ -n $THROUGHPUT ]] && ARGS+=(-t "$THROUGHPUT")
    TMP="$(mktemp)"
    taskpolicy "${ARGS[@]}" -p "$PID" >"$TMP" 2>&1
    RC=$?
    echo "[taskpolicy] taskpolicy ${ARGS[*]} -p $PID -> rc=$RC"
    readback "$TMP"
    rm -f "$TMP"
    echo "[taskpolicy] ★ the tier changed at $(date -u +%Y-%m-%dT%H:%M:%SZ) (epoch_ms=$(($(date +%s) * 1000)))"
    echo "[taskpolicy]   cut the leg's timeline here: `[ul_timing_events]` prints epoch_ms= on every event."
    echo "[taskpolicy]   this leg is an ARM (the tier is a scheduling change) - not acceptance evidence."
    ;;
  clear)
    TMP="$(mktemp)"
    taskpolicy -l 0 -p "$PID" >"$TMP" 2>&1 || true   # 0 = unspecified: back to the process's own tier
    readback "$TMP"
    rm -f "$TMP"
    echo "[taskpolicy] asked for the unspecified tier; the readback above says what the process is on now."
    ;;
  *)
    echo "refusing action '$ACTION': use scan, set or clear" >&2
    exit 2
    ;;
esac
