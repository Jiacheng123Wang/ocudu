#!/usr/bin/env bash
# GO / NO-GO before a utility flight, as a script rather than a memory.
#
# Every check here exists because one of them has already gone wrong, or would have:
#
#   * a stale binary           -- the leg runner stamps the build, but a binary older than the source silently flies the
#                                 previous receiver;
#   * the wrong cell           -- the runner's DEFAULT is N1 / 5 MHz / FDD, which this model has never seen, and the
#                                 collect configuration pins the UL MCS, which makes the published SINR unreachable;
#   * a wrong bundle           -- a bundle that declares no geometry binds and then refuses every reception, and one
#                                 without `depth3_sinr_cal` cannot show the calibration at all;
#   * a gNB already running    -- two gNBs on one radio is an unreadable batch;
#   * a phone that never came back after a re-attach -- the leg runs and measures nothing.
#
# It exits non-zero on the first failure and prints what to fix. Run it immediately before `fly_both.sh`.
#
# Usage: preflight_check.sh [collect|delivery] [UE_IP]
set -u
ROOT=/Users/jiachengwang/dev/ocudu
W=$ROOT/doc_chinese/llr_ai_detection/work_tmp
HERE=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-both}
UE=${2:-}
DEPLOYED=$W/data/models/d3_modcond_ft.mlmodelc
CALIBRATED=$W/data/models/d3_modcond_cal.mlmodelc
COLLECT=$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml
DELIVERY=$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml
GNB=$ROOT/build/apps/gnb/gnb
fail=0
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; fail=$((fail + 1)); }
warn() { printf '  warn  %s\n' "$*"; }

echo "== tree and binary"
[ "$(cd "$ROOT" && git status --porcelain | wc -l | tr -d ' ')" = 0 ] \
  && ok "working tree clean (the leg runner refuses a dirty tree unless LEG_ALLOW_DIRTY=1)" \
  || bad "working tree is dirty: commit, or the runner will refuse every leg"
newest=$(cd "$ROOT" && find lib apps include -name '*.cpp' -o -name '*.h' -o -name '*.mm' 2>/dev/null | xargs ls -t 2>/dev/null | head -1)
[ -n "$newest" ] || { bad "cannot list source files"; newest=; }
if [ -x "$GNB" ] && { [ -z "$newest" ] || [ "$GNB" -nt "$ROOT/$newest" ]; }; then
  ok "the gNB binary is newer than the newest source"
else
  bad "the gNB binary is stale or missing: cmake --build build --target gnb -j 6"
fi

echo "== the two bundles"
for spec in "deployed:$DEPLOYED" "calibrated:$CALIBRATED"; do
  name=${spec%%:*}; path=${spec##*:}
  if [ -d "$path" ]; then ok "$name bundle present: $(basename "$path")"; else bad "$name bundle missing at $path"; fi
done
if [ -d "$CALIBRATED" ]; then
  # The declaration is what makes the difference between the two bundles; a calibrated bundle without it would make arm
  # b silently identical to arm a, i.e. a batch that cannot answer its own question.
  if strings "$CALIBRATED/coremldata.bin" 2>/dev/null | grep -q 'depth3_sinr_cal'; then
    ok "the calibrated bundle DECLARES depth3_sinr_cal (arm b differs from arm a)"
  else
    bad "the calibrated bundle does not declare depth3_sinr_cal: arm b would equal arm a"
  fi
  if strings -n 3 "$CALIBRATED/coremldata.bin" 2>/dev/null | grep -qx '612'; then
    ok "the calibrated bundle declares the 612-subcarrier geometry"
  else
    warn "no standalone 612 in the metadata string table (check by hand)"
  fi
fi

echo "== the two cells"
for spec in "collect:$COLLECT" "delivery:$DELIVERY"; do
  name=${spec%%:*}; path=${spec##*:}
  if [ ! -f "$path" ]; then bad "$name cell config missing at $path"; continue; fi
  grep -q 'band: 78' "$path" && grep -q 'channel_bandwidth_MHz: 20' "$path" \
    && ok "$name cell is n78 / 20 MHz" || bad "$name cell is not n78 / 20 MHz"
  if grep -qE '^\s*(min_ue_mcs|max_ue_mcs)\s*:' "$path"; then
    echo "        $name PINS the UL MCS ($(grep -oE '^\s*(min|max)_ue_mcs: [0-9]+' "$path" | tr -s ' ' | sort -u | tr '\n' ' '))"
    [ "$name" = collect ] && ok "  (that is correct for the receiver contrast and wrong for the calibration)" \
                           || bad "  the calibration phase needs a cell without min/max_ue_mcs"
  else
    [ "$name" = delivery ] && ok "  $name leaves the MCS free (calibration phase)" \
                            || warn "  $name does not pin the MCS, so a receiver contrast there is confounded by link adaptation"
  fi
done
case "$MODE" in
  collect)  echo "== phase check: receiver contrast wants a PINNED MCS (collect)" ;;
  delivery) echo "== phase check: calibration wants a FREE MCS (delivery)" ;;
  *)        echo "== phase check: both phases (r9r on collect, r9c on delivery)" ;;
esac

echo "== disk and a clean environment"
# The logs are the big artefact: 170 MB per leg at info, so twelve legs are about two gigabytes. The volume here is at
# 94% used, which is the kind of thing a batch discovers halfway through.
avail_gb=$(df -g / | awk 'NR==2 {print $4}')
if [ -n "$avail_gb" ] && [ "$avail_gb" -ge 10 ]; then
  ok "${avail_gb} GB free (twelve legs are about 2 GB)"
else
  bad "${avail_gb:-?} GB free: twelve legs plus the pre-flight build may not fit"
fi
# `sudo -E` preserves the environment, so a capture knob left over from a corpus session would record EVERY reception
# (the corpus beside this work is 74 GB). The flight script strips these itself; this says so before the fact.
leaked=""
for variable in OCUDU_UL_DUMP OCUDU_UL_PHASE_SEGMENTS OCUDU_METAL_GPU_TIME; do
  value="${!variable:-}"
  [ -n "$value" ] && leaked="$leaked $variable=$value"
done
[ -n "$leaked" ] && warn "these are set in this shell and will be STRIPPED by the flight script:$leaked" \
                 || ok "no capture / heavy-probe variables in this shell"

echo "== the batch directory"
# The runner stamps every log with a new timestamp, so re-flying into a directory that already holds a batch leaves both
# copies and the verdict would average over four legs of an arm the operator believes has two. It refuses now, but the
# cheapest place to catch it is here.
#
# WHICH directory is checked is the one about to be flown into, not a fixed list: the first version hardcoded the two
# batches of that day (`r9r`/`r9c`), so every later flight reported NO-GO for the crime of having finished them --
# measured 2026-10-18, when a fresh batch was refused because the PREVIOUS ones still existed. `BATCH` (or the runner's
# own `LEG_LOGDIR`) names the batch; with neither, the check is informational and says so.
BATCH=${BATCH:-}
TARGET=${LEG_LOGDIR:-${BATCH:+$W/legs/$BATCH}}
if [ -n "$TARGET" ]; then
  # A BATCH IS A DIRECTORY AND A LEG IS A LABEL, so "the directory is not empty" only matters when the LEDGER for
  # this very leg already exists. The first version demanded an empty directory, which refused the second leg of a
  # batch that was being built leg by leg (measured 2026-10-18, flying o2b after o2a). With LEG_LABEL set the check
  # is a duplicate-label test; without it the empty-directory rule still applies, because a new batch must start
  # clean.
  if [ -n "${LEG_LABEL:-}" ]; then
    dup=$(ls -1 "$TARGET" 2>/dev/null | grep -c "_${LEG_LABEL}_" || true)
    if [ "${dup:-0}" != 0 ]; then
      bad "$TARGET already holds $dup log(s) for leg $LEG_LABEL: pick a new label"
    else
      ok "no leg named $LEG_LABEL in $TARGET ($(ls -1 "$TARGET"/*.log.stderr 2>/dev/null | wc -l | tr -d ' ') other leg(s) there, which is how a batch is built)"
    fi
  elif [ -d "$TARGET" ] && [ -n "$(ls -A "$TARGET" 2>/dev/null)" ]; then
    bad "$TARGET already holds a batch ($(ls -1 "$TARGET"/*.log.stderr 2>/dev/null | wc -l | tr -d ' ') leg log(s)): fly into a NEW name, or set LEG_LABEL to add a leg to this batch"
  else
    ok "$TARGET is empty (a fresh batch)"
  fi
else
  ok "no batch named (set BATCH=<name> or LEG_LOGDIR=<dir> to have this check the target)"
fi
# The finished batches are listed rather than failed: they are the record, and their presence says nothing about the
# batch about to fly.
done_batches=$(ls -d "$W"/legs/*/ 2>/dev/null | wc -l | tr -d ' ')
[ "$done_batches" != 0 ] && ok "$done_batches earlier batch director(ies) on disk, left as the record"

echo "== the radio and the peer"
[ "$(pgrep -x gnb | wc -l | tr -d ' ')" = 0 ] && ok "no gNB running" || bad "a gNB is already running: stop it first"
sudo -n true 2>/dev/null && ok "sudo is passwordless (no prompt per batch)" \
                          || warn "sudo will ask for a password once per batch (the script authenticates once, then reuses it)"
if [ -n "$UE" ]; then
  ping -c 1 -W 2000 "$UE" >/dev/null 2>&1 && ok "the UE at $UE answers ping" \
    || bad "the UE at $UE does not answer: attach it (each phase restarts the gNB, so the address CHANGES)"
else
  warn "no UE_IP given, so the peer was not checked (pass it as the second argument)"
fi

echo
if [ "$fail" = 0 ]; then
  echo "GO: $fail failing check(s). Fly it with the manual runbook (one leg at a time, the gNB first):"
  echo "    $HERE/FLIGHT_RUNBOOK_MANUAL.md"
else
  echo "NO-GO: $fail failing check(s) above."
fi
exit "$fail"
