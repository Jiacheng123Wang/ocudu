#!/usr/bin/env bash
# IPERF UPLINK LADDER: offer a series of rates and let the grant width follow.
#
# WHY A LADDER RATHER THAN ONE RATE. Measured 2026-10-29 on leg w2_harq_mcs06_ping_1029: the median grant goes 43, 46, 50, 51, 51 PRBs as
# the offered uplink rate goes 1, 3, 5, 8, unlimited Mbit/s, and at 1 Mbit/s the grants span 2 to 51 PRBs while at 8 Mbit/s every grant is
# the full band. So rate is a width knob, and a leg that wants narrow grants has to be flown slow.
#
# -R IS NOT OPTIONAL. The iperf3 server runs on the PHONE and the client runs here, on the core-network side. Without -R the client is the
# sender, which is downlink; with -R the server sends, which is the uplink this corpus wants.
#
# THE PHONE'S ADDRESS CHANGES EVERY ATTACH. Pass it, or pass nothing and let the UPF log say what it is -- the last session it created
# names the address it handed out.
#
# Usage: run_iperf_ladder.sh [phone-ip] [rate ...]
#        run_iperf_ladder.sh                     # address from the UPF log, default ladder
#        run_iperf_ladder.sh 10.45.0.9           # explicit address, default ladder
#        run_iperf_ladder.sh 10.45.0.9 0.5M 1M   # explicit address, explicit rates
#
# Env: DURATION (per phase, seconds, default 30), PORT (default 5201), OMIT (default 3), UPF_LOG.
set -u

UPF_LOG=${UPF_LOG:-$HOME/work/open5gs/install/var/log/open5gs/upf.log}
PORT=${PORT:-5201}
DURATION=${DURATION:-30}
OMIT=${OMIT:-3}

# THE ADDRESS CANNOT BE TRUSTED FROM THE LOG ALONE. Every attach creates a session and the log is append-only, so the last line is
# usually right -- but it was wrong within two minutes of use on 2026-10-29, when the log's newest session said 10.45.0.2 and the phone
# actually answered on 10.45.0.3. Rather than guess which line is current, take the most recent distinct addresses and ask the network:
# the one that answers a ping is the phone. sed, not tr -d, for the same day's other lesson -- tr deletes characters, so tr -d 'IPv[]'
# left the 4 of "IPv4" glued to the address.
phone_candidates() {
  [ -r "$UPF_LOG" ] || return 1
  grep -o 'IPv4\[[0-9][0-9.]*\]' "$UPF_LOG" 2>/dev/null |
    sed 's/^IPv4\[//; s/\]$//' | tac | awk '!seen[$0]++' | head -6
}

# PING IS NOT ENOUGH TO IDENTIFY THE PHONE. A session that has ended can leave an address that the UPF still answers for, so a candidate
# chosen on ping alone produced a phase that failed with "Connection reset by peer" while the phone was in fact on a newer address
# (measured 2026-10-29). Require the iperf port to answer too: that is the service this ladder is about to use.
find_phone() {
  local ip
  for ip in $(phone_candidates); do
    ping -c 1 -W 1 "$ip" >/dev/null 2>&1 || continue
    if (exec 3<>"/dev/tcp/$ip/$PORT") 2>/dev/null; then
      echo "$ip"
      return 0
    fi
  done
  return 1
}

UE=${1:-}
if [ -n "$UE" ]; then
  shift
else
  UE=$(find_phone) || true
  if [ -z "${UE:-}" ]; then
    echo "usage: $0 <phone-ip> [rate ...]" >&2
    echo "  (no address given and none found in $UPF_LOG)" >&2
    exit 2
  fi
  echo "phone address found by probing the UPF log's recent sessions: $UE"
fi

RATES=${*:-"1M 3M 5M 8M 0"}

# PREFLIGHT, because a ladder that fails on the first rung wastes the leg it was flown for: the phone must answer, and the server must be
# listening. Both are cheap to check and neither produces a useful error later.
if ! ping -c 2 -W 2 "$UE" >/dev/null 2>&1; then
  echo "REFUSING: $UE does not answer ping (is the phone attached? is the gNB running?)" >&2
  exit 2
fi
if ! (exec 3<>"/dev/tcp/$UE/$PORT") 2>/dev/null; then
  echo "REFUSING: nothing is listening on $UE:$PORT -- start 'iperf3 -s -p $PORT' on the phone." >&2
  exit 2
fi
command -v iperf3 >/dev/null || { echo "REFUSING: iperf3 is not installed here." >&2; exit 2; }

# A PHASE MUST NOT BE ABLE TO HANG, AND ITS OUTPUT MUST BE VISIBLE AS IT ARRIVES.
# Two complaints from the first flight, both fair: piping iperf through grep left the console silent for the whole ladder, so a phone that
# dropped mid-phase looked like a script that had hung; and nothing bounded the phase, so when the link died iperf waited forever and the
# script sat there. So: print iperf's own per-second line (--forceflush makes it arrive per line, not per buffer), and give each phase a wall
# clock a little longer than its own duration, after which it is interrupted and the ladder stops.
TIMEOUT_BIN=""
command -v timeout >/dev/null 2>&1 && TIMEOUT_BIN="timeout --signal=INT --kill-after=5"

echo "phone $UE, port $PORT, ${DURATION}s per phase, rates: $RATES"
for b in $RATES; do
  # The markers carry EPOCH SECONDS so the captures can be binned per phase afterwards (the corpus side does exactly that).
  echo "===PHASE b=$b START $(date +%s) $(date +%T)"
  # shellcheck disable=SC2086
  $TIMEOUT_BIN $((OMIT + DURATION + 15)) \
    iperf3 -c "$UE" -p "$PORT" -R -u -b "$b" -t "$DURATION" --omit "$OMIT" --forceflush
  rc=$?
  echo "===PHASE b=$b END $(date +%s) $(date +%T) rc=$rc"
  if [ "$rc" -ne 0 ]; then
    echo "!! phase b=$b failed (rc=$rc). If this is a connection error the phone re-attached with a new address:" >&2
    echo "   re-run the ladder without arguments and it will find the new one." >&2
    exit 3
  fi
  # AND ASK BETWEEN PHASES, rather than trusting the next iperf to fail quickly: a dropped attach produces a phase that hangs until the
  # timeout, and five of those is most of a leg spent on a phone that is not there.
  if ! ping -c 2 -W 2 "$UE" >/dev/null 2>&1; then
    echo "!! $UE stopped answering after b=$b -- the phone dropped. Stopping the ladder." >&2
    exit 3
  fi
  sleep 3
done
echo "ladder done (rc=0). Bin the captures by the PHASE markers, or by each cap file's mtime."
