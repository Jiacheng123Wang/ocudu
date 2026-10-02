#!/usr/bin/env bash
# CONTROLLED DISTURBANCE for the P4 arm experiment (dev doc macos_thread_priority 10.40).
#
# WHY: the arm's tail advantage showed 2-6x in one pair, 2-2.6x in the next, and REVERSED in the third
# (ul_pipeline 1.64, i.e. worse). The explanation offered is that a Mach time constraint protects against
# CPU competition, so its value scales with how much competition there is - and the control legs have been
# getting monotonically quieter (ul_pipeline over-2000us rate 0.516% -> 0.096% -> 0.051%). That explanation
# is testable only if the competition is set by US rather than by whatever the machine happens to be doing,
# which is what this script is for.
#
# It is deliberately dumb: N busy processes at the default QoS, which is exactly the competition a time
# constraint is supposed to be able to defend against. It starts nothing else, changes no scheduling, and
# kills only the processes it started (by pid, so it cannot take out anything of yours).
#
# usage: bash disturbance.sh start <n_spinners> <duration_s>   # foreground; Ctrl-C kills the spinners
#        bash disturbance.sh check <n_spinners>                # is the load actually up?
#
# The spinners are `yes`, which burns CPU and issues write() syscalls - closer to a busy daemon than a pure
# arithmetic loop, and available everywhere. Their output goes to /dev/null.
set -u

CMD="${1:-}"

case "$CMD" in
  start)
    N="${2:-4}"
    SECS="${3:-900}"
    pids=()
    for _ in $(seq 1 "$N"); do
      yes >/dev/null 2>&1 &
      pids+=("$!")
    done
    echo "disturbance: $N spinner(s) started for ${SECS}s (pids: ${pids[*]})"
    echo "disturbance: load1 now $(uptime | sed 's/.*load averages*: *//' | awk '{print $1}')"
    trap 'for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; echo; echo "disturbance: spinners stopped"; exit 0' INT TERM
    sleep "$SECS" &
    wait $!
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    echo "disturbance: ${SECS}s elapsed, spinners stopped"
    ;;
  check)
    N="${2:-4}"
    running=$(pgrep -x yes | wc -l | tr -d ' ')
    load1=$(uptime | sed 's/.*load averages*: *//' | awk '{print $1}')
    echo "spinners running: $running (asked for $N), load1=$load1"
    if [ "$running" -lt "$N" ]; then
      echo "WEAK: fewer spinners than asked - the disturbance is not what the experiment assumes"
      exit 1
    fi
    ;;
  *)
    sed -n '2,20p' "$0"
    exit 2
    ;;
esac
