#!/usr/bin/env bash
# The HOST SCHEDULING view of one on-air leg: what the macOS scheduler and the USB controller think, while
# the PHY probes only ever see the symptom.
#
# WHY (dev doc 6.157 (12), user's reading 2026-09-29). The receive tail `[ul_rx_timing] recv over 1ms / calls`
# appears in EVERY mode - cpu legs read 0.0004-0.0030%, cpu_gpu 0.0003-0.118%, gpu 0.0001-0.171% - so the GPU
# path does not create a new kind of failure, it raises the PROBABILITY of one. That shape (a hazard that is
# always present and whose rate follows how many high-QoS threads and wakeups the process has) points at host
# scheduling rather than at any block of the PHY, and macOS exposes exactly that layer for inspection:
#
#   * `taskpolicy -l/-t <0..5> -p <pid>` changes a RUNNING process's LATENCY and THROUGHPUT QoS tiers
#     (mach/task_policy.h: LATENCY_QOS_TIER_0..5, launch default TIER_3). This is the zero-code A/B: if the
#     transport class follows the tier, the mechanism is the process's scheduling tier, and the fix is a
#     policy, not a data path;
#   * `taskinfo <pid>` (root) READS those tiers back, so the A/B is verifiable rather than assumed;
#   * `powermetrics --samplers tasks,interrupts,sfi --show-process-qos-tiers --show-process-wait-times
#     --show-process-amp` gives the scheduler's own view per process: QoS tiers, scheduler wait time, and
#     P-core/E-core placement (this machine is an M4 Pro, 14 cores = 10P + 4E);
#   * `sample <pid>` shows WHERE each thread is, UHD's and libusb's included - those threads are not ours to
#     schedule, which is why the fused lane's extra wakeups (Metal completion handlers, lane fences,
#     grid-ready, token events) are the prime suspect and the CPU path is not.
#
# What this script already RULED OUT, so nobody repeats it: the B200 holds a DEDICATED XHCI controller
# (`ioreg -p IOUSB`: USRP B200 behind its own USB3 Gen2 hub; phone, keyboard, mouse and dock on three other
# controllers), it has NO foreign user client, and the controller's own statistics show no fault at all -
# `kControllerStatSpuriousInterruptCount=0`, `kPortStatEOF2ViolationCount=0`, `AddressFailureCount=0`,
# `EnumerationFailureCount=0`, `link-error-count=0`. The unified log carries no USB/GPU driver message in a
# sick window either (compared against a healthy one). So the failure is "not serviced in time", not "the
# controller saw an error" - which is exactly what the instruments below are for.
#
# usage (MUST be root - powermetrics, taskinfo and spindump all require it):
#   sudo bash doc_chinese/phy_pipeline_gpu/wip/host_sched_watch.sh 900            # watch for 900 s
#   sudo bash doc_chinese/phy_pipeline_gpu/wip/host_sched_watch.sh 900 /tmp/cap   # artifacts into /tmp/cap
#
# Start it FIRST, then fly the leg in another terminal, then read the leg with leg_triage.sh. The script
# prints the `taskpolicy` command with the leg's own pid filled in when it sees the gNB come up.
set -u

SECS=${1:-600}
OUT=${2:-/tmp/host_sched_$(date +%m%d_%H%M)}
[[ $SECS =~ ^[0-9]+$ ]] || { echo "usage: sudo bash host_sched_watch.sh <seconds> [outdir]" >&2; exit 2; }

if [[ $(id -u) -ne 0 ]]; then
  echo "this needs root (powermetrics / taskinfo / spindump):  sudo bash $0 $SECS $OUT" >&2
  exit 2
fi

mkdir -p "$OUT"
echo "artifacts -> $OUT"

# ---- 1. the B200's controller, before ---------------------------------------------------------------
xhci_stats() { # $1 = file
  ioreg -l -w0 -r -c AppleT8132USBXHCI 2>/dev/null | grep -E 'kControllerStatIOCount|SpuriousInterruptCount' > "$1" || true
  ioreg -l -w0 -p IOUSB 2>/dev/null | grep -B2 -A2 'USRP' >> "$1" || true
}
xhci_stats "$OUT/xhci_before.txt"

{
  echo "started      : $(date '+%F %T')"
  echo "cpu          : $(sysctl -n machdep.cpu.brand_string) ($(sysctl -n machdep.cpu.core_count) cores)"
  echo "sched profile: $(sysctl -n kern.sched 2>/dev/null)"
  echo "loadavg      : $(sysctl -n vm.loadavg)"
} | tee "$OUT/env.txt"

# ---- 2. the scheduler's own view, 1 Hz for the whole leg --------------------------------------------
powermetrics --samplers tasks,interrupts,sfi \
             --show-process-qos-tiers --show-process-wait-times --show-process-amp \
             -i 1000 -n "$SECS" -o "$OUT/powermetrics.txt" &
PM_PID=$!
echo "powermetrics : pid $PM_PID, 1 Hz x ${SECS}s -> $OUT/powermetrics.txt"

# ---- 3. when the gNB appears: tiers, threads, one user stack sample, and the A/B command -------------
deadline=$(( $(date +%s) + SECS ))
gnb=""
while [[ $(date +%s) -lt $deadline ]]; do
  gnb=$(pgrep -x gnb | head -1 || true)
  [[ -n $gnb ]] && break
  sleep 2
done

if [[ -z $gnb ]]; then
  echo "no gnb appeared within ${SECS}s - nothing else to read" | tee -a "$OUT/env.txt"
else
  echo "gnb pid      : $gnb  ($(date '+%T'))" | tee -a "$OUT/env.txt"
  taskinfo "$gnb" > "$OUT/taskinfo_before.txt" 2>&1 || true
  grep -iE 'policy|tier|qos' "$OUT/taskinfo_before.txt" | head -10 | sed 's/^/  /'
  ps -M "$gnb" -o pid,tid,pri,state,time,comm > "$OUT/threads.txt" 2>/dev/null || ps -M "$gnb" > "$OUT/threads.txt"
  echo "threads      : $(($(wc -l < "$OUT/threads.txt") - 1))  (-> $OUT/threads.txt)" | tee -a "$OUT/env.txt"
  cat <<EOF | tee -a "$OUT/env.txt"

  == the zero-code A/B (paste in THIS terminal while the leg runs), read back with taskinfo:
     sudo taskpolicy -l 5 -t 5 -p $gnb     # most latency/throughput-responsive  (tier 0 = least)
     sudo taskinfo $gnb | grep -i tier     # confirm the change landed
  == take a user-stack sample of the same pid now, from a THIRD terminal:
     sudo sample $gnb 5 -file $OUT/sample_mid.txt
EOF
  sleep 5
  sample "$gnb" 5 -file "$OUT/sample_mid.txt" >/dev/null 2>&1 || true
  echo "sample       : $OUT/sample_mid.txt (grep for Thread_ and for uhd/libusb frames)"
fi

wait $PM_PID 2>/dev/null || true

# ---- 4. the controller, after: did anything at all move? --------------------------------------------
xhci_stats "$OUT/xhci_after.txt"
echo
echo "== controller statistics, before -> after (a fault would show up here; so far nothing ever has)"
diff <(grep -o 'kControllerStatIOCount"=[0-9]*' "$OUT/xhci_before.txt") \
     <(grep -o 'kControllerStatIOCount"=[0-9]*' "$OUT/xhci_after.txt") || true
grep -o 'kPortStat[A-Za-z]*=[0-9]*' "$OUT/xhci_after.txt" 2>/dev/null | sort -u | head -8 | sed 's/^/  /'
echo
echo "== now classify the leg itself:  bash $(dirname "$0")/leg_triage.sh <leg-label>"
echo "== and read the scheduler's view: grep -A6 -iE 'gnb' $OUT/powermetrics.txt | head -60"
