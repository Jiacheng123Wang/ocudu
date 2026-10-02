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
    target=""
    last_size=0
    seen=0   # has this target been looked at once already? (see the note below)
    while [ "$elapsed" -lt "$SECS" ]; do
      sleep 5
      elapsed=$((elapsed + 5))
      # `--watch`: RF failures in the last ~1 MB of the leg's log. A handful means the DL path is starting
      # to starve; back off one burner per interval until they stop.
      recent=0
      # `--watch` may name a DIRECTORY, which is the useful form: the disturbance must start BEFORE the leg
      # (the leg is what needs the competition), and the leg's log file is named after its label and start
      # time - so at start-up there is nothing to point at. Watching the newest *.log in the directory
      # resolves that, and re-resolves it every cycle so a restarted leg is picked up too.
      new_target="$WATCH"
      if [ -n "$WATCH" ] && [ -d "$WATCH" ]; then
        new_target=$(ls -t "$WATCH"/*.log 2>/dev/null | head -1)
      fi
      # ★ NEW FAILURES ONLY, not the count inside a sliding window. The first version counted "failures in the
      # last 1 MB", and since the newest log in that directory was the VOID p210 leg - which already contains
      # 1371 of them - it read a constant 1371 every cycle and backed off to one burner for no reason.
      # Counting only the bytes appended since the previous check is what "are failures happening NOW" means,
      # and it is also cheap: the append between two 5 s checks is small.
      # The FIRST look at a file records its size and counts NOTHING: counting from byte 1 on the first cycle
      # is what made the second version report 1900 failures and back off - the file it was looking at was the
      # void p210 leg, whose whole history is failures. Only bytes appended between two checks are "now", and
      # for a freshly created leg log that is exactly right (it starts empty).
      if [ -n "${new_target:-}" ] && [ -f "$new_target" ]; then
        size=$(stat -f %z "$new_target" 2>/dev/null || echo 0)
        # ★ A SEPARATE FLAG, not `last_size -eq 0`: a leg log starts EMPTY, so its size is 0 on the first
        # look, and using that as the "first look" test made every cycle a first look - the counter never ran
        # and the backoff could never fire. The reverse-arm test (append ten failures to a fresh log and see
        # whether it backs off) is what caught it; without that test the controller would have sat at full
        # disturbance through a real link failure while reporting nothing.
        if [ "${new_target}" != "${target:-}" ] || [ "$seen" -eq 0 ]; then
          target="$new_target"
          last_size=$size
          seen=1
        elif [ "$size" -gt "$last_size" ]; then
          recent=$(tail -c +$((last_size + 1)) "$new_target" 2>/dev/null | grep -ac 'Real-time failure' || true)
          target="$new_target"
          last_size=$size
        elif [ "$size" -lt "$last_size" ]; then
          target="$new_target"
          last_size=$size
        fi
      fi
      if [ "$recent" -gt 3 ] && [ "$alive" -gt 1 ]; then
        # kill the most recently started burner
        last="${pids[$((alive - 1))]}"
        kill "$last" 2>/dev/null
        alive=$((alive - 1))
        echo "disturbance2: ${recent} NEW RF failure(s) since the last check -> backed off to ${alive} burner(s) at t=${elapsed}s"
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
