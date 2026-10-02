#!/usr/bin/env bash
# ADAPTIVE CPU disturbance for the P4 experiment (dev doc macos_thread_priority 10.44).
#
# WHAT WENT WRONG WITH v1. It used `yes > /dev/null`, which calls write() once per line. Ten of those put
# 82.55% of the machine's CPU in SYSTEM time, starved the kernel/USB path the radio needs, and broke the
# link: 942 "Real-time failure in RF" lines in the last 2 MB, CPU idle at 3.56%, and iperf3's uplink at
# zero - a UE that cannot decode its grants does not transmit, so the leg measures nothing at all. A Mach
# time constraint defends a thread against CPU competition; it cannot defend the radio against a saturated
# kernel, so that intervention destroys the thing being measured.
#
# WHAT THIS DOES INSTEAD
#   * burns CPU in USER space only (the `burn` helper makes no syscall in its loop), so the interference is
#     runnable-slot competition - what the constraint is supposed to be able to defend against;
#   * WATCHES THE LEG and backs off. It counts RF real-time failures in the leg's log every 5 s; when they
#     appear it kills one burner and says so, because "as much competition as the link survives" is the
#     regime worth testing, while "so much that the link dies" tests nothing. The number of burners still
#     alive at the end is what the experiment must record as the ACTUAL disturbance level.
#
# usage: bash disturbance2.sh start <n_burners> <seconds> [--watch <leg_ocudulog>] [--duty <percent>]
#        bash disturbance2.sh check
#
# Ctrl-C kills everything it started.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BURN="$HERE/burn"

CMD="${1:-}"
case "$CMD" in
  start)
    N="${2:-6}"
    SECS="${3:-1200}"
    shift 3 2>/dev/null || true
    WATCH=""
    DUTY=100
    while [ $# -gt 0 ]; do
      case "$1" in
        --watch) WATCH="${2:-}"; shift 2 ;;
        --duty)  DUTY="${2:-100}"; shift 2 ;;
        *) shift ;;
      esac
    done

    if [ ! -x "$BURN" ]; then
      echo "compiling the user-space burner (no leg should be flying yet)..."
      clang -O2 -o "$BURN" "$HERE/burn.c" || { echo "FAILED to build $BURN"; exit 1; }
    fi

    pids=()
    for _ in $(seq 1 "$N"); do
      "$BURN" "$SECS" "$DUTY" &
      pids+=("$!")
    done
    alive=$N
    echo "disturbance2: $N user-space burner(s) (duty=${DUTY}%), duration ${SECS}s"
    [ -n "$WATCH" ] && echo "  watching $WATCH for RF failures (back off when they appear)"

    stop_all() {
      for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
      echo
      echo "disturbance2: stopped (burners alive at the end: $alive)"
      exit 0
    }
    trap stop_all INT TERM

    elapsed=0
    while [ "$elapsed" -lt "$SECS" ]; do
      sleep 5
      elapsed=$((elapsed + 5))
      # `--watch`: RF failures in the last ~1 MB of the leg's log. A handful means the DL path is starting
      # to starve; back off one burner per interval until they stop.
      recent=0
      if [ -n "$WATCH" ] && [ -f "$WATCH" ]; then
        recent=$(tail -c 1000000 "$WATCH" 2>/dev/null | grep -ac 'Real-time failure' || true)
      fi
      if [ "$recent" -gt 3 ] && [ "$alive" -gt 1 ]; then
        # kill the most recently started burner
        last="${pids[$((alive - 1))]}"
        kill "$last" 2>/dev/null
        alive=$((alive - 1))
        echo "disturbance2: ${recent} RF failure(s) in the last 1MB -> backed off to ${alive} burner(s) at t=${elapsed}s"
      elif [ $((elapsed % 30)) -eq 0 ]; then
        idle=$(top -l 1 -n 0 2>/dev/null | awk '/^CPU usage:/{print $7}')
        echo "disturbance2: t=${elapsed}s burners=${alive} cpu_idle=${idle:-?} load1=$(uptime | sed 's/.*load averages*: *//' | awk '{print $1}') rf_recent=${recent}"
      fi
    done
    stop_all
    ;;
  check)
    echo "burn processes: $(pgrep -x burn | wc -l | tr -d ' ')"
    top -l 1 -n 0 2>/dev/null | grep '^CPU usage'
    ;;
  *)
    sed -n '2,26p' "$0"
    exit 2
    ;;
esac
