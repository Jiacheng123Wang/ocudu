#!/usr/bin/env bash
# Reproduce the multi-second ping/iperf spikes, and judge whether a leg shows their mechanism.
#
# ---- The mechanism (established from the logs, see the design document) ------------------------
# CAUSALITY, in the order it happens (the RACH is the CONSEQUENCE, not the cause):
#   1. the gNB detects Radio Link Failure and starts a release timer (4 s)
#        "ue=0: RLF detected. Cause: 100 consecutive undecoded CSIs"  <- 16 of these in one leg,
#        and 16 re-accesses: every release in the leg has this cause and no other;
#   2. the UE loses its connection and re-accesses: PRACH -> tc-rnti -> Msg3;
#   3. Msg3 often fails (5 grants, all crc=KO, no PUCCH, gNB gives up) and the UE tries again with
#      the next tc-rnti. HOW MANY TRIES IT TAKES IS THE SIZE OF THE PING SPIKE;
#   4. once the access succeeds, the queued TCP/ICMP drains - the linear RTT ramp the operator sees.
# So RACH failures are the SYMPTOM and the spike's duration; the thing to remove is step 1 (the
# drops). The relevant gNB rule is the MAC RLF detector's consecutive-KO thresholds, and the PUCCH
# SINR filter that decides which indications count towards them.
#
# The spikes are NOT a GPU-pipeline or host problem:
#   * the UE loses its link and re-accesses; the gNB answers each PRACH with a fresh tc-rnti and
#     grants it up to 5 PUSCHs (Msg3 + 4 HARQ retransmissions);
#   * on the failed attempts every grant is crc=KO with a meaningless SINR (-17..-53 dB: nothing was
#     received), the identity never sends PUCCH, and the gNB gives up ("Maximum number of reTxs 4
#     exceeded" -> "Discarding UL HARQ process TB");
#   * during those attempts the UE has NO data connection, so TCP/ICMP queues and drains afterwards -
#     which is the multi-second stall and the linear RTT ramp the ping shows.
# Measured across all 19 archived legs: 160 failed vs 85 successful accesses, and the failed ones all
# sat at PRACH power -70..-65 dB against -52..-8 dB for the successful ones. It is present in the
# EARLIEST available leg, so it is not a recent regression.
#
# ---- Use --------------------------------------------------------------------------------------
#   Terminal 1:  sudo -E bash doc_chinese/full_gpu_chain/wip/run_air_leg.sh rachstorm
#   Terminal 2 (or the phone): keep ping/iperf3 running for >= 2 minutes so at least one re-access
#                              happens. No manual outage is needed - the failures are spontaneous (a
#                              forced outage is optional: see FORCE below).
#
#   Then:        bash doc_chinese/full_gpu_chain/wip/rachstorm_repro.sh judge [log]
#
# FORCE (optional): to make an outage deliberate, put the phone in airplane mode for >= 30 s while the
# leg runs and note the wall-clock time - then read the attempt table around it.
#
# \note The judge needs nothing but the log: it finds the attempts by their signature rather than by a
# window. That is deliberate - a window-based reading has to guess which evidence lies inside it, and
# with attempts every few seconds that guess is wrong.
set -u
MODE=${1:-}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
LOGDIR=$HERE/../work_tmp/logs

case "$MODE" in
  judge)
    LOG=${2:-$(ls -t "$LOGDIR"/gnb_rachstorm_*.log 2>/dev/null | head -1)}
    [ -n "${LOG:-}" ] && [ -f "$LOG" ] || {
      echo "no log found: pass one explicitly, or run the leg as 'run_air_leg.sh rachstorm'" >&2
      exit 2; }
    exec "${PYTHON:-python3}" "$HERE/rachstorm_judge.py" "$LOG"
    ;;
  *)
    sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
