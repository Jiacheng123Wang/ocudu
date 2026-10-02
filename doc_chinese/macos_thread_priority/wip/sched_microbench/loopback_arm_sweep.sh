#!/usr/bin/env bash
# Base-rate sweep for the P4 arms on the LOOPBACK bench (no radio, no sudo, no UE).
#
# WHY: the first loopback run of the blanket arm (OCUDU_SCHED_TIME_CONSTRAINT='*=...', i.e. 2026-09-01's
# shape) died with
#     OCUDU FATAL ERROR: Attempting to write samples [...] that would overwrite unread sample [...]
# while the arm with the knob OFF ran to "contract MET".  One run each proves nothing, so this runs each arm
# N times and reports the counts.  It is the bench-level version of the question the 2026-09-01 radio
# regression left open: is the harm in the CONSTRAINT or in giving every worker the SAME parameters?
#
# usage: bash loopback_arm_sweep.sh [runs-per-arm] [seconds-per-run]
set -u

RUNS="${1:-3}"
SECS="${2:-20}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CFG="doc_chinese/phy_latency/wip/gnb_loopback_n78.yml"
OUT="${OUT:-/tmp/loopback_sweep}"
mkdir -p "$OUT"

if pgrep -x gnb >/dev/null; then
  echo "REFUSING: a gnb is already running (a leg may be flying)"
  exit 1
fi

# arm label -> OCUDU_SCHED_TIME_CONSTRAINT value ('' = the knob is unset, i.e. the shipped default)
#
# The first four arms answered the question this sweep was written for (3 runs each, 2026-10-01):
#   off 3/3 clean | blanket 0/3 clean (3 CRASHES) | single 3/3 clean | single_b 3/3 clean
# so the harm is in the SCOPE, not in the constraint or in its parameter values - which is what
# 2026-09-01 could never tell apart. The remaining arms bisect WHICH members of the blanket set are the
# dangerous ones, because the set the pipeline actually needs protected is the five pool threads.
all_arms=("off" "blanket" "single" "single_b" "pool5" "io2" "lower3" "lower3_min" "lower3_hist" "lower3_loose" "pool5_cal" "pool5_cal2")
all_values=("" "*=500/200/400" "main_pool#0=500/200/400" "main_pool#0=500/200/400;radio=500/100/200" \
            "main_pool#0=500/200/400;main_pool#1=500/200/400;main_pool#2=500/200/400;main_pool#3=500/200/400;main_pool#4=500/200/400" \
            "io_timer_tick=500/200/400;io_broker_epoll=500/200/400" \
            "lower_phy_tx#0=500/200/400;lower_phy_rx#0=500/200/400;lower_phy_ul#0=500/200/400" \
            "lower_phy_tx#0=1000/100/200;lower_phy_rx#0=1000/100/200;lower_phy_ul#0=1000/100/200" \
            "lower_phy_tx#0=500/500/500;lower_phy_rx#0=500/500/500;lower_phy_ul#0=500/500/500" \
            "lower_phy_tx#0=2000/50/100;lower_phy_rx#0=2000/50/100;lower_phy_ul#0=2000/50/100" \
            "main_pool#0=5000/1000/2000;main_pool#1=5000/1000/2000;main_pool#2=5000/1000/2000;main_pool#3=5000/1000/2000;main_pool#4=5000/1000/2000" \
            "main_pool#0=5000/2500/3000;main_pool#1=5000/2500/3000;main_pool#2=5000/2500/3000;main_pool#3=5000/2500/3000;main_pool#4=5000/2500/3000")
arms=("${all_arms[@]}")
values=("${all_values[@]}")
if [ -n "${ARMS:-}" ]; then
  arms=()
  values=()
  for want in $ARMS; do
    for idx in "${!all_arms[@]}"; do
      if [ "${all_arms[$idx]}" = "$want" ]; then
        arms+=("$want")
        values+=("${all_values[$idx]}")
      fi
    done
  done
fi

declare -A crash clean
for a in "${arms[@]}"; do crash[$a]=0; clean[$a]=0; done

for i in $(seq 1 "$RUNS"); do
  for idx in "${!arms[@]}"; do
    arm="${arms[$idx]}"
    spec="${values[$idx]}"
    err="$OUT/${arm}_run${i}.err"
    log="$OUT/${arm}_run${i}.log"
    if [ -z "$spec" ]; then
      env OCUDU_SCHED_VERBOSE=1 ./build/apps/gnb/gnb -c "$CFG" --expert_phy.phy_pipeline gpu \
        --log.filename "$log" >/dev/null 2>"$err" &
    else
      env OCUDU_SCHED_VERBOSE=1 OCUDU_SCHED_TIME_CONSTRAINT="$spec" ./build/apps/gnb/gnb -c "$CFG" \
        --expert_phy.phy_pipeline gpu --log.filename "$log" >/dev/null 2>"$err" &
    fi
    pid=$!
    sleep "$SECS"
    kill -INT "$pid" 2>/dev/null
    for _ in $(seq 1 40); do kill -0 "$pid" 2>/dev/null || break; sleep 0.25; done
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null

    fatal="$(grep -ac 'OCUDU FATAL ERROR' "$err" || true)"
    met="$(grep -ac 'contract MET' "$err" || true)"
    # A run counts as a CRASH when the fatal error appeared; it counts as CLEAN when the shutdown report says
    # the contract was met (the report is printed by the shutdown path, so an early death cannot fake it).
    if [ "$fatal" -gt 0 ]; then
      crash[$arm]=$((crash[$arm] + 1))
      verdict="CRASH"
    elif [ "$met" -gt 0 ]; then
      clean[$arm]=$((clean[$arm] + 1))
      verdict="clean"
    else
      verdict="NO-REPORT (neither: see $err)"
    fi
    printf '  %-9s run %d/%d: %-12s %s\n' "$arm" "$i" "$RUNS" "$verdict" \
      "$(grep -a 'OCUDU FATAL ERROR' "$err" | head -1 | cut -c1-110)"
  done
done

echo
echo "arm        clean  crash   constraint spec"
for idx in "${!arms[@]}"; do
  arm="${arms[$idx]}"
  printf '%-10s %5d %6d   %s\n' "$arm" "${clean[$arm]}" "${crash[$arm]}" "${values[$idx]:-<unset>}"
done
echo "logs: $OUT"
