#!/bin/bash
# Monitor three depth3 dataset builds; when each finishes, move/replace its npz files
# from datasets_bin to data/datasets_full_w2 with size verification.
# Poll every 5 minutes, hard stop at 70 minutes from DSH_START.
# Never kills, never deletes: destination conflicts are quarantined, not removed.
set -u
HOST=jwang@192.168.31.211
SSH="ssh -o BatchMode=yes $HOST"
RH='/home/jwang/work/ai_phy_train'
LEGS="1028de 1028f 1028g"
START=${DSH_START:-$(date +%s)}
DEADLINE=$(( START + 70*60 ))
ST_1028de=running; ST_1028f=running; ST_1028g=running
MOVED_1028de="-"; MOVED_1028f="-"; MOVED_1028g="-"
ANYFIN=0
say(){ echo "[$(date '+%F %T')] $*"; }
getst(){ eval "echo \"\$ST_$1\""; }
setst(){ eval "ST_$1=\"\$2\""; }
getmv(){ eval "echo \"\$MOVED_$1\""; }
setmv(){ eval "MOVED_$1=\"\$2\""; }
readable(){ date -r "$1" '+%F %T' 2>/dev/null || echo "epoch=$1"; }

say "MONITOR START epoch=$START deadline_epoch=$DEADLINE"
BF=$($SSH "ls -1 $RH/data/datasets_full_w2/*.npz 2>/dev/null | wc -l" | tr -d ' ')
BB=$($SSH "ls -1 $RH/datasets_bin/*.npz 2>/dev/null | wc -l" | tr -d ' ')
say "BASELINE dest_npz_before=$BF bin_npz_before=$BB"

iter=0
while :; do
  now=$(date +%s); left=$(( DEADLINE - now ))
  if [ "$left" -le 0 ]; then say "DEADLINE reached before next poll"; break; fi
  wait_s=$left; [ "$wait_s" -gt 300 ] && wait_s=300
  sleep "$wait_s"
  iter=$((iter+1))
  say "--- poll #$iter $(date '+%F %T') ---"
  RAW=$($SSH 'ps ax -o command= | grep "[b]uild_depth3_dataset" || true')
  for leg in $LEGS; do
    [ "$(getst "$leg")" = running ] || continue
    n=$(printf '%s\n' "$RAW" | grep -c -- "$leg")
    if [ "$n" -gt 0 ]; then
      say "leg $leg: RUNNING ($n process)"
      continue
    fi
    FL=$($SSH "cd $RH && stat -c '%Y|%s|%n' datasets_bin/w2_harq_${leg}_*.npz 2>/dev/null || true")
    if [ -z "$FL" ]; then
      say "leg $leg: process gone but NO matching npz in datasets_bin -> FAILED, not moving"
      setst "$leg" failed; ANYFIN=1; continue
    fi
    printf '%s\n' "$FL" | while IFS='|' read -r mt sz nm; do
      [ -n "${nm:-}" ] || continue
      say "leg $leg: candidate $nm size=$sz mtime=$(readable "$mt")"
    done
    bad=$(printf '%s\n' "$FL" | awk -F'|' -v s="$START" '$1+0 <= s {print $3}')
    if [ -n "$bad" ]; then
      say "leg $leg: process gone but output is NOT fresh (predates monitor start: $bad) -> NOT MOVING"
      setst "$leg" stale; ANYFIN=1; continue
    fi
    say "leg $leg: FINISHED -> moving"
    OUT=$($SSH "bash /tmp/dsh_move_leg.sh $leg")
    printf '%s\n' "$OUT" | while IFS= read -r l; do [ -n "$l" ] && say "  MOVE| $l"; done
    ERR=$(printf '%s\n' "$OUT" | grep -E "SIZE_MISMATCH|REFUSED|FRESH_INVALID|MOVE_FAIL|QUARANTINE|SRC_REMAINS" || true)
    if [ -n "$ERR" ]; then
      say "leg $leg: ERROR during move -> stopping further action for this leg"
      setst "$leg" error; ANYFIN=1
    else
      setst "$leg" done; ANYFIN=1
    fi
    setmv "$leg" "$(printf '%s' "$OUT" | tr '\n' ';')"
  done
  anyrun=0
  for leg in $LEGS; do [ "$(getst "$leg")" = running ] && anyrun=1; done
  if [ "$anyrun" -eq 0 ]; then say "ALL LEGS SETTLED"; break; fi
done

say "=== FINAL STATUS ==="
allok=1
for leg in $LEGS; do
  say "leg $leg status=$(getst "$leg") move_result=$(getmv "$leg")"
  [ "$(getst "$leg")" = done ] || allok=0
done
AF=$($SSH "ls -1 $RH/data/datasets_full_w2/*.npz 2>/dev/null | wc -l" | tr -d ' ')
AB=$($SSH "ls -1 $RH/datasets_bin/*.npz 2>/dev/null | wc -l" | tr -d ' ')
say "COUNTS dest_npz_before=$BF dest_npz_after=$AF bin_npz_before=$BB bin_npz_after=$AB"
QN=$($SSH "ls -1 $RH/data/datasets_full_w2_replaced_fragments/*.npz 2>/dev/null | wc -l" | tr -d ' ')
if [ "$QN" != "0" ]; then
  say "QUARANTINED fragments: $QN"
  $SSH "ls -l --time-style=+%F_%H:%M $RH/data/datasets_full_w2_replaced_fragments/ 2>/dev/null" | while IFS= read -r l; do say "  Q| $l"; done
fi
if [ "$allok" -eq 1 ]; then
  say "=== FINAL DESTINATION FILES FOR THESE LEGS ==="
  $SSH "ls -l --time-style=+%F_%H:%M $RH/data/datasets_full_w2/w2_harq_mcs02_ping_1028de_*.npz $RH/data/datasets_full_w2/w2_harq_mcs02_ping_1028f_*.npz $RH/data/datasets_full_w2/w2_harq_mcs02_ping_1028g_*.npz 2>/dev/null" | while IFS= read -r l; do say "  FILE| $l"; done
  say "=== POST-MOVE NPZ LOAD CHECK (files touched) ==="
  for leg in $LEGS; do
    F=$($SSH "ls -1 $RH/data/datasets_full_w2/w2_harq_${leg}_*.npz 2>/dev/null" | tr '\n' ' ')
    if [ -z "$F" ]; then say "leg $leg: CHECK_SKIP no destination file"; continue; fi
    $SSH "$RH/.venv/bin/python /tmp/dsh_check_npz.py $F" | while IFS= read -r l; do say "  CHECK| $l"; done
  done
  say "=== AUDIT (tail -5) ==="
  $SSH 'set -o pipefail; cd ~/work/ai_phy_train && ./.venv/bin/python ~/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/check_depth3_dataset.py --dir data/datasets_full_w2 2>&1 | tail -5; echo "AUDIT_RC=${PIPESTATUS[0]}"' | while IFS= read -r l; do say "AUDIT| $l"; done
else
  say "AUDIT SKIPPED: not all three legs settled successfully (see statuses above)"
fi
say "MONITOR END"
