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
for d in mediaanalysisd photoanalysisd mds mds_stores mdworker mdworker_shared backupd cloudd bird UTM QEMULauncher qemu-system-aarch64; do
  cpu=$(printf '%s\n' "$busy" | awk -v n="$d" '$1 == n {print $2}' | sort -rn | head -1)
  if [ -n "$cpu" ] && awk -v c="$cpu" 'BEGIN { exit (c + 0 >= 5.0) ? 0 : 1 }'; then
    hot="$hot $d=${cpu}%"
  fi
done
if [ -n "$hot" ]; then verdict 1 "no heavy background daemon" "busy:$hot"; else verdict 0 "no heavy background daemon" "-"; fi
top5=$(printf '%s\n' "$busy" | awk 'NF == 2 && $2 + 0 > 0 {printf "%s=%.1f%% ", $1, $2}' | cut -c1-90)
say "(top CPU now)" "${top5:-none}"

# 3b. ACTIVITY OVER TIME, not cumulative CPU. The first version of this check compared each daemon's
# CUMULATIVE CPU time against 60 s and told the operator to wait for it to clear - which can never happen: a
# daemon up for two hours that did legitimate work earlier keeps that total forever, so the check was
# unsatisfiable (the operator's own output showed mds at 30:00 and mediaanalysisd at 3:42 while both were
# idle). "Recently" can only be answered by SAMPLING TWICE and looking at the DELTA.
#
# Counting note, because it cost the operator a round trip: `ps aux | grep -c mediaanalysis` answers 3 forever
# - mediaanalysisd, mediaanalysisd-access (an on-demand XPC service) AND THE GREP ITSELF. `pgrep -x` is the
# command that means what a person means.
daemon_cpu_seconds() {
  local total=0 t v
  for d in mediaanalysisd photoanalysisd mds mds_stores mdworker mdworker_shared backupd cloudd bird; do
    for pid in $(pgrep -x "$d" 2>/dev/null); do
      t=$(ps -o time= -p "$pid" 2>/dev/null) || continue
      # macOS prints a FRACTIONAL last field (`3:42.43`); truncate to an integer or the arithmetic silently
      # produces floats and every comparison against it fails without saying so.
      v=$(printf '%s' "$t" | awk -F'[-:]' '{ if (NF == 4) x = $1*86400 + $2*3600 + $3*60 + $4;
                                            else if (NF == 3) x = $1*3600 + $2*60 + $3;
                                            else x = $1*60 + $2;
                                            printf "%d", x }')
      total=$((total + v))
    done
  done
  echo "$total"
}
d0=$(daemon_cpu_seconds)
sleep 3
d1=$(daemon_cpu_seconds)
delta=$((d1 - d0))
busy_pct=$((delta * 100 / 3))
# 25% of one core sustained by this family is the line: the quiet reference legs showed them idle, and the void
# legs' underflow bursts sat next to exactly this family working.
if [ "$busy_pct" -ge 25 ]; then
  verdict 1 "daemon family idle over 3 s" "${busy_pct}% of a core just now (delta ${delta}s/3s)"
else
  verdict 0 "daemon family idle over 3 s" "${busy_pct}% of a core (cumulative totals are informational only)"
fi

# 3c. SPOTLIGHT, checked by its own switch rather than inferred. On 2026-10-02 the operator believed indexing
# was off; `mdutil -as` said "Indexing enabled" for / and /System/Volumes/Data, and `mds` had burned 30 minutes of
# CPU with `mds_stores` another 25 - i.e. a heavy, bursty, IO-generating neighbour running throughout. That is the
# shape of the ~10-minute underflow bursts. `mdutil -s` needs no root to READ; changing it does.
idx=$(mdutil -as 2>/dev/null | grep -c 'Indexing enabled')
if [ "${idx:-0}" -gt 0 ]; then
  verdict 1 "Spotlight indexing off" "$idx volume(s) still 'Indexing enabled' -> sudo mdutil -a -i off"
else
  verdict 0 "Spotlight indexing off" "-"
fi

# 4. The radio. MEASURED 2026-10-02: `system_profiler SPUSBDataType` does NOT list a connected USRP B200 on this
# machine - the first version of this check grepped it, found nothing, and reported NO-GO while the radio was
# plugged in and active. `ioreg -p IOUSB` does list it ("USRP B200@... <class IOUSBHostDevice ... active>"), and
# the B200 also exposes a /dev/cu.usbmodem* node. Both are accepted; the operator is told how to overrule.
if ioreg -p IOUSB -w0 2>/dev/null | grep -qiE 'USRP|Ettus|B200'; then
  verdict 0 "USRP visible" "$(ioreg -p IOUSB -w0 2>/dev/null | grep -oiE 'USRP B200|Ettus[^<]*' | head -1)"
elif ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbserial* >/dev/null 2>&1; then
  verdict 0 "USRP visible" "USB serial node present (ioreg did not name it)"
else
  verdict 1 "USRP visible" "neither ioreg nor /dev shows it - plug it in, or overrule with 'uhd_find_devices'"
fi

echo
if [ "$no_go" -eq 0 ]; then
  echo "GO: nothing above was present on the void legs. Fly, and check gaps=0 + 'contract MET' the moment it ends."
else
  echo "NO-GO: fix the items marked above first. The void legs all failed at least one of them."
  echo
  echo "  Reading the daemon lines, because they measure different windows of time:"
  echo "    'no heavy background daemon'      = THIS INSTANT (from a 1 s top sample)"
  echo "    'daemon family idle over 3 s'     = the last 3 seconds (the one that predicts a leg)"
  echo "    the cumulative totals, if printed = history. They NEVER go down, so do not wait for them."
  echo "  These are launchd services: they stay resident and wake on demand, so 'pgrep -x mediaanalysisd'"
  echo "  answering 1 is normal and is NOT a problem. Judge them by the 3-second delta."
  echo
  echo "  Spotlight is the usual offender and it lies quietly - 'mdutil -as' is the truth, not the intention:"
  echo "    sudo mdutil -a -i off          # then re-run this script; 'Indexing disabled' is the goal"
  echo "    sudo mdutil -a -i on           # to restore it afterwards"
  echo "  To stop one for the run instead:"
  echo "    sudo launchctl list | grep -i <name>;  sudo launchctl bootout system/<label>"
  echo "  Then re-run this script. Do NOT spend 30 minutes hoping it clears mid-leg."
fi
[ "$VERBOSE" -eq 1 ] && { echo; echo "--- evidence ---"; uptime; ps aux | grep -E 'mediaanalysis|photoanalysis|cloudd|bird|mds_stores|backupd' | grep -v grep | head; }
exit "$no_go"
