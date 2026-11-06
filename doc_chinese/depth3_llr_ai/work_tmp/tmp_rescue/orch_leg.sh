#!/bin/bash
# One leg, end to end: capture -> corpus, dump -> tb_<leg>, build, ship npz to 211, verify, clean local.
# Never touches queue_build_datasets.sh. Exits non-zero on any failure and leaves evidence in place.
# RESUMABLE: a corpus directory that already holds captures is reused; the dump is always re-copied.
set -u
LEG=${1:?usage: orch_leg.sh <leg>}
TRAIN=/Users/jiachengwang/ai_depth3
CORPUS=/Users/jiachengwang/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
REMOTE=jwang@192.168.31.211
DATA=$TRAIN/datasets_full_w2
LOG=/tmp/orch_$LEG.log
exec >>"$LOG" 2>&1
say() { echo "[$(date '+%F %T')] $*"; }
freeg() { df -g /System/Volumes/Data | tail -1 | awk '{print $4}'; }
fail() { say "ORCH_FAIL $LEG: $*"; exit 1; }

say "=== START $LEG free=$(freeg)GiB ==="

# 0. preconditions
[ -x /tmp/worker2.sh ] || fail "worker2.sh missing"
f=$(freeg)
[ "$f" -ge 30 ] || fail "insufficient free space before extract: ${f}GiB (<30)"

# 1. capture tarball -> corpus (skipped when already extracted)
ncaps=0
if [ -d "$CORPUS/$LEG" ]; then
  ncaps=$(find "$CORPUS/$LEG" -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' | wc -l | tr -d ' ')
fi
if [ "$ncaps" -gt 0 ]; then
  say "---- step1 have capture dir ($ncaps captures), skipping extract"
else
  say "---- step1 extract capture"
  ssh -o BatchMode=yes "$REMOTE" "cat /mnt/corpus/tarballs/$LEG.tar" | tar -xf - -C "$CORPUS"
  ps=("${PIPESTATUS[@]}"); src=${ps[0]:-1}; trc=${ps[1]:-1}
  say "extract ssh_rc=$src tar_rc=$trc"
  [ "$src" -eq 0 ] || fail "ssh cat tarball failed rc=$src"
  [ "$trc" -eq 0 ] || fail "tar extract failed rc=$trc"
  ncaps=$(find "$CORPUS/$LEG" -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' | wc -l | tr -d ' ')
  say "captures=$ncaps free=$(freeg)GiB"
  [ "$ncaps" -gt 0 ] || fail "no cap_*.txt under $CORPUS/$LEG"
fi

# 2. dump directory (exclude AppleDouble ._* stubs: macOS tar treats them as metadata and fails)
say "---- step2 copy dump"
rm -rf "$TRAIN/tb_$LEG"
ssh -o BatchMode=yes "$REMOTE" "cd ~/work/ai_phy_train && tar -cf - --exclude='._*' --exclude='*/._*' tb_$LEG" | tar -xf - -C "$TRAIN"
ps=("${PIPESTATUS[@]}"); src=${ps[0]:-1}; trc=${ps[1]:-1}
say "dump ssh_rc=$src tar_rc=$trc"
[ "$src" -eq 0 ] || fail "ssh tar dump failed rc=$src"
[ "$trc" -eq 0 ] || fail "untar dump failed rc=$trc"
[ -s "$TRAIN/tb_$LEG/index.csv" ] || fail "dump index.csv missing"
local_real=$(find "$TRAIN/tb_$LEG" -type f ! -name '._*' | wc -l | tr -d ' ')
remote_real=$(ssh -o BatchMode=yes "$REMOTE" "find ~/work/ai_phy_train/tb_$LEG -type f ! -name '._*' | wc -l" | tr -d ' ')
local_ad=$(find "$TRAIN/tb_$LEG" -type f -name '._*' | wc -l | tr -d ' ')
say "dump files local=$local_real remote=$remote_real appledouble=$local_ad"
[ "$local_real" -eq "$remote_real" ] || fail "dump file count mismatch local=$local_real remote=$remote_real"
[ "$local_ad" -eq 0 ] || fail "dump still contains $local_ad AppleDouble files"
if [ -f "$TRAIN/tb_$LEG/units.txt" ]; then
  want=$(wc -l < "$TRAIN/tb_$LEG/units.txt" | tr -d ' ')
  got=$(( $(wc -l < "$TRAIN/tb_$LEG/index.csv") - 1 ))
  say "dump rows=$got units=$want"
fi

# 3. build; worker stamps .done_<leg> only after the queue exits 0
say "---- step3 build"
[ -d "$DATA" ] || fail "output dir $DATA missing"
stamp="$DATA/.done_$LEG"
rm -f "$stamp"
startline=$(wc -l < "$TRAIN/build_hq.log" 2>/dev/null | tr -d ' ')
startline=${startline:-0}
nohup /tmp/worker2.sh "$LEG" >/dev/null 2>&1 &
wpid=$!
say "worker pid=$wpid log_offset=$startline"
built=0
for i in $(seq 1 60); do
  sleep 180
  nn=$(find "$DATA" -maxdepth 1 -name "${LEG}_*.npz" | wc -l | tr -d ' ')
  newlog=$(tail -n +$((startline+1)) "$TRAIN/build_hq.log" 2>/dev/null)
  if [ -f "$stamp" ]; then built=1; break; fi
  if echo "$newlog" | grep -q "FAILED $LEG"; then fail "queue exited non-zero (log says FAILED $LEG)"; fi
  if ! kill -0 "$wpid" 2>/dev/null; then fail "worker exited without stamp (npz=$nn)"; fi
  say "poll $i: npz=$nn free=$(freeg)GiB"
done
[ "$built" -eq 1 ] || fail "no stamp after 3h"
for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$wpid" 2>/dev/null || break; sleep 3; done
nn=$(find "$DATA" -maxdepth 1 -name "${LEG}_*.npz" | wc -l | tr -d ' ')
say "stamp present npz=$nn free=$(freeg)GiB"
[ "$nn" -gt 0 ] || fail "stamp present but 0 npz"
bytes=$(find "$DATA" -maxdepth 1 -name "${LEG}_*.npz" -exec stat -f %z {} \; | awk '{s+=$1} END {print s+0}')
say "npz_bytes=$bytes"

# 4. ship to 211, verify names and sizes
say "---- step4 ship"
( cd "$TRAIN" && tar -cf - datasets_full_w2/${LEG}_*.npz ) | ssh -o BatchMode=yes "$REMOTE" 'tar -xf - -C ~/work/ai_phy_train/data/'
ps=("${PIPESTATUS[@]}"); src=${ps[0]:-1}; trc=${ps[1]:-1}
say "ship ssh_rc=$src tar_rc=$trc"
[ "$src" -eq 0 ] || fail "ship ssh failed rc=$src"
[ "$trc" -eq 0 ] || fail "ship tar failed rc=$trc"

local_list=$(cd "$DATA" && for x in ${LEG}_*.npz; do printf '%s %s\n' "$x" "$(stat -f %z "$x")"; done | sort)
remote_list=$(ssh -o BatchMode=yes "$REMOTE" "cd ~/work/ai_phy_train/data/datasets_full_w2 && for x in ${LEG}_*.npz; do printf '%s %s\\n' \"\$x\" \"\$(stat -c %s \"\$x\")\"; done | sort")
lcount=$(echo "$local_list" | sed '/^$/d' | wc -l | tr -d ' ')
rcount=$(echo "$remote_list" | sed '/^$/d' | wc -l | tr -d ' ')
say "verify local=$lcount remote=$rcount"
if [ "$lcount" != "$nn" ] || [ "$rcount" != "$nn" ]; then
  say "LOCAL_LIST:"; echo "$local_list"; say "REMOTE_LIST:"; echo "$remote_list"
  fail "remote count mismatch (local npz=$nn listed=$lcount remote=$rcount)"
fi
if [ "$local_list" != "$remote_list" ]; then
  say "LOCAL_LIST:"; echo "$local_list"; say "REMOTE_LIST:"; echo "$remote_list"
  fail "remote name/size mismatch"
fi
say "verify OK ($rcount files, sizes match)"

# 5. free local space
say "---- step5 cleanup"
rm -f "$DATA"/${LEG}_*.npz
rm -rf "$TRAIN/tb_$LEG"
rm -rf "$CORPUS/$LEG"
left_npz=$(find "$DATA" -maxdepth 1 -name "${LEG}_*.npz" | wc -l | tr -d ' ')
[ "$left_npz" -eq 0 ] || fail "cleanup left $left_npz npz"
[ ! -e "$TRAIN/tb_$LEG" ] || fail "cleanup left tb_$LEG"
[ ! -e "$CORPUS/$LEG" ] || fail "cleanup left corpus/$LEG"
say "ORCH_DONE $LEG npz=$nn bytes=$bytes free=$(freeg)GiB"
exit 0
