#!/usr/bin/env bash
# PRE-FLIGHT: is this machine fit to fly a leg right now?
#
# WHY (dev doc macos_thread_priority 10.34). Three legs in a row lost radio samples - p195_0703 gaps=1,
# p198 gaps=4 (6 972 254 samples missing or repeated), p199 gaps=1 (3 784 146) - and every one of them
# also failed the transport criterion (AT/BELOW 0 rates 0.0185% / 0.0121% against a 0.0025% bound) and
# the contract check. The reference quiet legs (p193/p194) had gaps=0. The RF underflows arrive in BURSTS
# roughly ten minutes apart, and the system log around those bursts is dominated by `mediaanalysisd` plus
# the process-launch machinery (`runningboardd`, `launchd`, `sandboxd`, `tccd`) - the same family of
# activity that produced the earlier whole-process freezes.
#
# A 30-minute leg that ends in gaps=4 is 30 minutes wasted, and the failure is visible BEFORE it starts.
# This script checks the few things that were measurably present on the bad legs and prints GO or NO-GO.
#
# It is deliberately read-only: it starts nothing, kills nothing, and takes no sudo.
#
# usage: bash preflight_quiet.sh          # GO/NO-GO
#        bash preflight_quiet.sh -v       # also print the evidence lines
set -u

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1
no_go=0

say() { printf '  %-34s %s\n' "$1" "$2"; }
verdict() { # <ok:0|1> <label> <detail>
  if [ "$1" -eq 0 ]; then say "$2" "OK   $3"; else say "$2" "NO-GO  $3"; no_go=1; fi
}

# 1. A leg must never overlap another gnb.
if pgrep -x gnb >/dev/null; then verdict 1 "no leg already flying" "a gnb process is running"; else verdict 0 "no leg already flying" "-"; fi

# 2. Load average. The quiet reference legs ran at load1 ~3.9; both void long legs reached 7.2 and 10.7.
load1=$(uptime | sed 's/.*load averages*: *//' | awk '{print $1}')
high=$(awk -v l="$load1" 'BEGIN { print (l + 0 > 4.5) ? 1 : 0 }')
verdict "$high" "load1 below 4.5" "load1=$load1 (quiet reference legs read ~3.9)"

# 3. ACTIVITY, not mere presence. cloudd / bird / mds_stores / backupd are launchd daemons that are ALWAYS
#    resident on macOS and idle almost all the time, so "is it running" is a useless test - the first version of
#    this script was NO-GO on every machine because of it. Sample twice, one second apart, and judge what is
#    actually BURNING CPU: on the void legs the underflow bursts sat next to mediaanalysisd plus launch churn.
snap=$(top -l 2 -n 40 -s 1 -stats command,cpu 2>/dev/null)
idle=$(printf '%s\n' "$snap" | awk '/^CPU usage:/{v=$7} END{print v}' | tr -d '%')
idle_ok=$(awk -v i="${idle:-0}" 'BEGIN { print (i + 0 >= 70) ? 0 : 1 }')
verdict "$idle_ok" "CPU idle >= 70%" "idle=${idle:-?}%"
# The second sample is what matters; the first is a warm-up delta.
busy=$(printf '%s\n' "$snap" | awk '/^COMMAND/{buf=""; seen=1; next} seen{buf=buf $0 "\n"} END{printf "%s", buf}')
hot=""
# `utm`/`qemu` are not macOS daemons but a virtual machine's host process - a VM is a large, bursty, IO-generating
# neighbour that can also claim USB devices, and one was running when this script was first used (it surfaced in the
# top-CPU line, not in a check).
for d in mediaanalysisd photoanalysisd mds_stores backupd cloudd bird UTM QEMULauncher qemu-system-aarch64; do
  cpu=$(printf '%s\n' "$busy" | awk -v n="$d" '$1 == n {print $2}' | sort -rn | head -1)
  if [ -n "$cpu" ] && awk -v c="$cpu" 'BEGIN { exit (c + 0 >= 5.0) ? 0 : 1 }'; then
    hot="$hot $d=${cpu}%"
  fi
done
if [ -n "$hot" ]; then verdict 1 "no heavy background daemon" "busy:$hot"; else verdict 0 "no heavy background daemon" "-"; fi
top5=$(printf '%s\n' "$busy" | awk 'NF == 2 && $2 + 0 > 0 {printf "%s=%.1f%% ", $1, $2}' | cut -c1-90)
say "(top CPU now)" "${top5:-none}"

# 4. The radio must be enumerable. `system_profiler` is slow, so this is the cheap check.
if system_profiler SPUSBDataType 2>/dev/null | grep -qi 'ettus\|ni \|b200\|usrp'; then
  verdict 0 "USRP visible on USB" "-"
else
  verdict 1 "USRP visible on USB" "not found in SPUSBDataType"
fi

echo
if [ "$no_go" -eq 0 ]; then
  echo "GO: nothing above was present on the void legs. Fly, and check gaps=0 + 'contract MET' the moment it ends."
else
  echo "NO-GO: fix the items marked above first. The void legs all failed at least one of them."
  echo "  Waiting out mediaanalysisd is usually enough: it finishes on its own; 'ps aux | grep -c mediaanalysis'"
  echo "  until it drops to zero, then re-run this script. Do NOT spend 30 minutes hoping it clears mid-leg."
fi
[ "$VERBOSE" -eq 1 ] && { echo; echo "--- evidence ---"; uptime; ps aux | grep -E 'mediaanalysis|photoanalysis|cloudd|bird|mds_stores|backupd' | grep -v grep | head; }
exit "$no_go"
