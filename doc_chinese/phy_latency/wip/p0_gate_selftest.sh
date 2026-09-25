#!/usr/bin/env bash
# Self-test for the p0_gate.sh readings that a leg can only produce on air (dev doc 6.19): D11 (Q9-F commit
# order) and D12 (Q9-F2/F3 queue occupancy + front-end blocks).
#
# WHY IT EXISTS. A gate parser that silently matches NOTHING prints "cannot read", and "cannot read" is RED -
# but a parser that silently matches the WRONG token prints a number that looks like a reading (the kind of
# defect 4.3 (3) and 5.9.125 (6) already cost this workflow twice: the kind field came out as `?` and the hole
# line was never found, both caught by running the gate on a synthetic leg before believing it).
#
# It takes a REAL leg's stderr (default: the newest leg in the logs directory) and APPENDS the three lines the
# air leg will produce, then asserts the gate reads them back. Nothing touches the GPU: this is a text test.
#
# usage: bash p0_gate_selftest.sh [leg-label-or-stderr-path]
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
LOGDIR="$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs"
GATE="$HERE/p0_gate.sh"

ARG=${1:-}
if [ -z "$ARG" ]; then
  # The fixture base must be a leg flown BEFORE the instruments whose readings this self-test appends (6.19's
  # D11/D12, 6.24's D14, 6.26's D15). From leg p15 on every leg carries them, so "the newest log" stopped
  # being a valid base on 2026-09-25 and turned three checks red for a property of the LOG directory rather
  # than of the gate (recorded in dev doc 6.30). The newest leg WITHOUT the commit-order line is used instead;
  # if every leg has it, the self-test says so and skips rather than reporting a false failure.
  ARG=$(ls -t "$LOGDIR"/*.log.stderr 2>/dev/null | while read -r f; do
    grep -q 'commit order (Q9-F)' "$f" 2>/dev/null || {
      printf '%s' "$f"
      break
    }
  done)
fi
if [ -z "$ARG" ] || [ ! -f "$ARG" ]; then
  echo "SKIP: no pre-6.19 leg stderr to build the fixture from (looked in $LOGDIR; pass one as the 1st argument)"
  exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FIXTURE="$TMP/selftest.stderr"
cp "$ARG" "$FIXTURE"
cat >> "$FIXTURE" <<'EOF'
[metal_stats] commit order (Q9-F): commits=48123 waits=27957 waiter-committed-first=8 (same-queue=8 cross-queue=0) max=5001.2ms worst kind=grid slot=9612; per kind: stage 12/27922, corr 0/0, grid 8/35 (inversions/waits)
[metal_stats] queue occupancy (Q9-F3): commits=100437 busy(union)=38123456.7us window=100123456.0us holes>100us=3 largest=5002.4ms
[metal_stats] queue occupancy (Q9-F3) slowest commits, by commit -> GPU start (label = what the buffer carries):
[metal_stats]   label=merged_hop  slot=9612 commit->start=5002977.3us start->end=1244.1us
[metal_stats]   hole 5002.4ms -> next label=merged_hop slot=9612 (nothing was executing on any probed queue for that long)
[ul_gpu_lane] dft carried blocks (Q9-F2): resolved=48123 of 48123 (never committed=0, committed but unfinished at exit=0, no GPU timestamps=0, dropped over the bound=0)
[ul_gpu_lane] dft carried deposit ->GPU start samples=48123 mean=2211.0us median=1105.0us min=498.1us max=5004688.5us p95=3562.0us p99=8021.0us
[metal_stats] dft handover handed=48123 taken=27887 superseded=0 evicted=47867 evicted_unproduced=0 over_bound=0 unproduced=0 fallback=18331 late=1905 late_time=12 not_found=1891 timeouts=0 keepalives=673722/673722 (max in flight 112) (armed=1) tokens_early=signals:0,by_event:0,by_complete:48123 handshake=waits:7,timeouts:0,max:412us
[metal_stats] dft commits=48123 transforms=673722 waits=143878 slots_in_flight=14 radio_inputs=673722 wrap_copies=0 released=48123 released_waits=0 batched=4812/67368 batch_max=14 batch_src=auto slot_symbols=14
[dl_tx_slack] transmissions=144657 mean=1850.4us median=1900.0us p1=310.0us p5=420.0us p25=1500.0us min=208us (due_ts=99887766); below 2ms=52000, below 1ms=1200, below 500us=40, AT/BELOW 0=0
EOF

OUT=$(bash "$GATE" "$FIXTURE" 2>&1)
FAILED=0
expect() {
  local what=$1 token=$2
  if printf '%s' "$OUT" | grep -qF -- "$token"; then
    echo "PASS: $what  [$token]"
  else
    echo "FAIL: $what - the gate did not read '$token'"
    printf '%s\n' "$OUT" | grep -A3 "D11 \|D12 " | head -12
    FAILED=1
  fi
}
expect "D11 reads the inversion count"        "waiter-committed-first=8"
expect "D11 reads the same-queue subset"      "same-queue=8 = the shape that cannot resolve itself"
expect "D11 reads the longest inversion"      "longest=5001.2ms worst kind=grid slot=9612"
expect "D11 states the branch verdict"        "the Q9-G ordering IS on this leg"
expect "D12 reads the occupancy line"         "queue occupancy (Q9-F3): commits=100437"
expect "D12 reads the largest hole"           "hole 5002.4ms -> next label=merged_hop slot=9612"
expect "D12 reads the front-end account"      "dft carried blocks (Q9-F2): resolved=48123 of 48123"
expect "D12 reads the front-end queue wait"   "dft carried deposit ->GPU start samples=48123"
expect "D13 reads the handshake waits"        "handshake waits=7 timeouts=0 max=412us"
expect "D14 reads the absence of a stall dump" "the pool never parked the receive thread for 20 ms"
expect "D15 reads the drop counter when absent" "a leg flown before 6.26 cannot say"
expect "D13 states what a wait would have cost" "the Q9-G window IS reached on air"
expect "D16 reads the batched pair"          "batched=4812/67368 batch_max=14 batch_src=auto slot_symbols=14"
expect "D16 states the branch verdict"       "the front end DID defer"
expect "D17 reads the transmit margin"     "AT/BELOW 0=0"

# The reverse direction: a leg WITHOUT the new lines must say so instead of printing a number (rule 4.3 (3)).
# (The gate NAMES the line it looked for in that message, so the check is on the verdict, not on the token.)
# The fixture is the one above, which is by construction a leg without those lines (see the header).
OLD_LEG=$ARG
OUT_OLD=$(bash "$GATE" "$OLD_LEG" 2>&1)
D11_OLD=$(printf '%s\n' "$OUT_OLD" | grep -A3 "D11 (Q9-F)" | grep "read    :")
D12_OLD=$(printf '%s\n' "$OUT_OLD" | grep -A3 "D12 " | grep "read    :")
if printf '%s' "$D11_OLD" | grep -qF "cannot say" && ! printf '%s' "$D11_OLD" | grep -qF "waiter-committed-first=" &&
   printf '%s' "$D12_OLD" | grep -qF "cannot say" &&
   printf '%s\n' "$OUT_OLD" | grep -A3 "D13 " | grep -qF "cannot say" &&
   printf '%s\n' "$OUT_OLD" | grep -A3 "D14 " | grep -qF "no 'p0 dump' line" &&
   printf '%s\n' "$OUT_OLD" | grep -A3 "D15 " | grep -qF "cannot say" &&
   printf '%s\n' "$OUT_OLD" | grep -A3 "D16 " | grep -qF "a leg flown before 6.30 cannot say" &&
   printf '%s\n' "$OUT_OLD" | grep -A3 "D17 " | grep -qF "a leg flown before 6.41 cannot say"; then
  echo "PASS: a leg without the 6.19 lines reads as 'cannot say' rather than as a number ($(basename "$OLD_LEG"))"
else
  echo "FAIL: a leg without the 6.19 lines did not read as 'cannot say' ($(basename "$OLD_LEG")):"
  printf '%s\n%s\n' "$D11_OLD" "$D12_OLD"
  FAILED=1
fi

[ "$FAILED" = "0" ] && echo "self-test: PASS (all of the above)" || echo "self-test: FAILED"
exit "$FAILED"
