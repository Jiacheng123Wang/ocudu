#!/usr/bin/env bash
# Watch the w2 corpus root ON 211 for finished dumps and build one mother set at a time.
#
# This is the w2 deployment of watch_dumps_build.sh. The two differ only where the two lines differ:
# the leg prefix is w2_, the capture and dump roots are the same directory, and a finished dump whose
# captures are gone is recorded as `no-captures` instead of being re-extracted over ssh (there is no
# remote: the host IS the archive).
#
# The w2 line keeps both halves of a leg under one directory: /mnt/corpus/w2_tb_tarballs/<leg>/ holds the
# captures and /mnt/corpus/w2_tb_tarballs/tb_<leg>/ the dump, so CORPUS_ROOT and DUMP_ROOT are the same
# place. The capturing/dumping workflow (run_w2_batch.sh) fills it leg by leg and keeps the captures; this
# watcher consumes each finished dump into /mnt/corpus/datasets_full_w2 and frees the captures afterwards
# (the tarballs in w2_corpus_tarballs remain the durable copy, so a rebuild is a two-minute extraction).
#
# WHY ONE AT A TIME. The dump batch is running on the same machine and the same drive; three concurrent builds
# stretched a leg from 38 minutes to 78-103 and starved both pipelines. A single build leaves the dump batch
# most of the machine, and the queue drains at the rate legs finish anyway.
#
# WHEN IS A DUMP FINISHED. `dump_tb.sh` truncates dump_meta.txt when it starts and appends `finished=` only after
# both passes (per-unit and HARQ chains), the index assembly and the population split, so the marker cannot survive
# a half run. The counts that follow it are re-derived from the products, and this script checks them rather than
# trusting the marker alone: index.csv rows must equal units.txt lines, and tb/ must hold exactly self+harq blocks
# (every labelled sample has exactly one block). index_ko.csv must exist, which it only does after the labelling
# step. A directory that fails any of these is left alone and retried on the next poll, so a run that is still in
# progress is never mistaken for a finished one.
#
# WHAT IT BUILDS. Every `w2_*` leg with a finished dump that is not already built. Legs already `ok` in the
# progress table are skipped; a leg that failed twice is left for a human.
# After a successful build the leg's captures are freed (RECLAIM=1): the dump batch waits when the drive is low,
# and a built leg's captures are the only thing that can be freed without losing anything.
#
# Usage: watch_dumps_build.sh [--dry-run]
#   POLL=120 RECLAIM=1 watch_dumps_build.sh &
set -uo pipefail

REPO=${REPO:-/home/jwang/ocudu}
CORPUS=${CORPUS:-/mnt/corpus/w2_tb_tarballs}
OUT=${OUT:-/mnt/corpus/datasets_full_w2}
PY=${PY:-/usr/bin/python3}
TOOL=$REPO/lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh
LOGD=${LOGD:-$HOME/logs_w2_motherset}
PROGRESS=$OUT/_progress.csv
WATCHLOG=$LOGD/watch.log
POLL=${POLL:-120}
RECLAIM=${RECLAIM:-1}
EXCLUDE=""
PREFIX="w2_"
DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

mkdir -p "$LOGD" "$OUT"

# ONE WATCHER PER LIBRARY. Two instances would both pick the same next leg and build it into the
# same filenames. Restarting by hand is exactly when that happens: `pgrep -f watch_dumps_build.sh`
# also matches the `bash -c ... nohup bash watch_dumps_build.sh` wrapper, so killing "the first
# match" can leave the real watcher running while a second one starts. A directory holding the pid
# is the guard; a stale one (its pid gone) is taken over.
LOCK=$OUT/_watcher.lock
if ! mkdir "$LOCK" 2>/dev/null; then
  other=$(cat "$LOCK/pid" 2>/dev/null)
  if [ -n "${other:-}" ] && kill -0 "$other" 2>/dev/null; then
    echo "another watcher already runs (pid $other, lock $LOCK); exiting" >&2
    exit 0
  fi
  echo "taking over a stale lock ($LOCK, pid ${other:-unknown} is gone)" >&2
  rm -rf "$LOCK"
  mkdir "$LOCK" 2>/dev/null || { echo "cannot take $LOCK; exiting" >&2; exit 1; }
fi
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT
[ -f "$PROGRESS" ] || echo "leg,status,seconds,files,samples,finished_at" > "$PROGRESS"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$WATCHLOG"; }
mark() { echo "$1,$2,$3,$4,$5,$(date '+%Y-%m-%dT%H:%M:%S')" >> "$PROGRESS"; }
done_leg() { awk -F, -v l="$1" '$1==l && $2=="ok"{f=1} END{exit !f}' "$PROGRESS"; }
failed_twice() { [ "$(awk -F, -v l="$1" '$1==l && $2=="failed"{n++} END{print n+0}' "$PROGRESS")" -ge 2 ]; }
findings_ok() { awk '/--- findings ---/{getline; if ($0 ~ /none/) ok=1} END{exit !ok}' "$1"; }
# `df -g` is BSD syntax; GNU's df rejects it and the log line came out empty on 211.
free_gib() {
  df -BG --output=avail "$1" 2>/dev/null | tr -dc '0-9' || df -g "$1" 2>/dev/null | awk 'NR==2{print $4}'
}
# EXCLUDE is a list; comparing one leg against the whole string would match nothing.
is_excluded() { local x; for x in $EXCLUDE; do [ "$1" = "$x" ] && return 0; done; return 1; }

# A dump is finished when the marker is there AND the products agree with the counts next to it.
dump_finished() {
  local d=$1 units rows tb recorded
  grep -q '^finished=' "$d/dump_meta.txt" 2>/dev/null || return 1
  [ -f "$d/index.csv" ] && [ -f "$d/units.txt" ] && [ -f "$d/index_ko.csv" ] || return 1
  units=$(wc -l < "$d/units.txt" | tr -d ' ')
  rows=$(( $(wc -l < "$d/index.csv") - 1 ))
  [ "$units" -gt 0 ] && [ "$units" -eq "$rows" ] || return 1
  # tb/ holds one block per TRAINABLE sample: self + every labelled harq (first transmissions AND the
  # intermediate chain members), which is `tb_blocks=` in the meta. `self=` and `harq=` next to it count
  # only the first transmissions, so their sum is smaller than tb/ by the chain members (714 on 1103i).
  recorded=$(awk -F= '/^tb_blocks=/{print $2}' "$d/dump_meta.txt")
  [ -n "$recorded" ] || return 1
  tb=$(ls "$d/tb" 2>/dev/null | wc -l | tr -d ' ')
  # AT LEAST the recorded count, not exactly. A dump re-run into the same directory truncates the
  # indexes but not tb/, so stale blocks from the earlier run stay behind: 1029c carries 230 of them
  # (and four more than its own meta recorded, written after the snapshot). The build reads only the
  # units the indexes name, so extra blocks are harmless -- while FEWER than recorded means the dump
  # really is short and must not be built.
  [ "$tb" -ge "$recorded" ] || return 1
  if [ "$tb" -gt "$recorded" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] NOTE: $leg has $tb block(s) in tb/, meta recorded $recorded" >> "$WATCHLOG"
  fi
  return 0
}

# A FINISHED DUMP IS NOT ENOUGH: the captures must be there too, and whole. The other workflow
# copies both halves into the same directory, so a dump can appear while its captures are still
# arriving, and building then would read missing .bin files. The unit count is the same check the
# producer used to decide the leg was complete (and a re-extraction can leave MORE than the dump's
# list, which is fine).
captures_complete() {
  local d=$1 leg=$2 cap="$CORPUS/$2" want have
  [ -d "$cap" ] || return 1
  want=$(wc -l < "$d/units.txt" | tr -d ' ')
  have=$(find "$cap" -maxdepth 1 -name '*.txt' ! -name '*_ce.txt' ! -name '._*' 2>/dev/null | wc -l | tr -d ' ')
  [ "${have:-0}" -ge "${want:-1}" ]
}

build_leg() {
  local leg=$1 start=$SECONDS rc n samples cap="$CORPUS/$1" dump="$CORPUS/tb_$1" log="$LOGD/$1.log"

  if [ ! -d "$cap" ]; then
    # Defensive: the loop only calls this once captures_complete said yes, so a disappear in between
    # means someone moved the tree. Nothing to build from.
    log "$leg: capture directory $cap vanished before the build; skipped"
    return 0
  fi

  # A previous failed attempt may have left npz behind; never mix two builds of one leg.
  if ls "$OUT/${leg}"_*.npz >/dev/null 2>&1; then
    local trash="$OUT/_trash_${leg}_$(date '+%Y%m%d%H%M%S')"
    mkdir -p "$trash" && mv "$OUT/${leg}"_*.npz "$trash"/ && log "$leg: moved stale npz to $trash"
  fi

  log "$leg: building (single, others wait)"
  nice -n 10 env TRAIN="$REPO" DUMP_ROOT="$CORPUS" CORPUS_ROOT="$CORPUS" OUT="$OUT" PY="$PY" \
      JOBS=1 CHUNK=8000 REQUIRE_ITERATIONS=1 bash "$TOOL" "$leg" > "$log" 2>&1
  rc=$?
  n=$(ls "$OUT/${leg}"_*.npz 2>/dev/null | wc -l | tr -d ' ')
  # The build writes _manifest_<leg>.csv; prefer it: counting samples from the npz decompresses every
  # X member (1.3 GB for QPSK self) for a number that is already on disk.
  if [ -f "$OUT/_manifest_${leg}.csv" ]; then
    samples=$(awk -F, 'NR > 1 { s += $4 } END { print s + 0 }' "$OUT/_manifest_${leg}.csv")
  else
    samples=$("$PY" - "$OUT" "$leg" <<'PY'
import sys, glob, os, numpy as np
print(sum(len(np.load(p, allow_pickle=True)["X"]) for p in glob.glob(os.path.join(sys.argv[1], sys.argv[2] + "_*.npz"))))
PY
)
  fi
  if [ "$rc" -eq 0 ] && findings_ok "$log"; then
    mark "$leg" ok $((SECONDS - start)) "$n" "$samples"
    log "$leg: OK files=$n samples=$samples ($(( (SECONDS - start) / 60 ))m)"
    if [ "$RECLAIM" = "1" ]; then
      rm -rf "$cap"
      log "$leg: captures freed ($(free_gib "$CORPUS") GiB free)"
    fi
  else
    mark "$leg" failed $((SECONDS - start)) "$n" "$samples"
    log "$leg: FAILED rc=$rc files=$n (log $log)"
    # A failed attempt may also have left a partial npz; do not let the next attempt see it.
    if [ "$n" -gt 0 ]; then
      local trash="$OUT/_trash_${leg}_$(date '+%Y%m%d%H%M%S')"
      mkdir -p "$trash" && mv "$OUT/${leg}"_*.npz "$trash"/ 2>/dev/null
    fi
  fi
}

log "watcher start: poll=${POLL}s reclaim=${RECLAIM} dry_run=${DRY}"
[ "$DRY" = "1" ] && log "DRY RUN: only listing what would be built"

last_heartbeat=0
while :; do
  built_this_round=0
  for d in "$CORPUS"/tb_*; do
    [ -d "$d" ] || continue
    leg=${d##*/tb_}
    case "$leg" in "$PREFIX"*) ;; *) continue ;; esac
    is_excluded "$leg" && continue
    # THE CHEAP CHECKS COME FIRST. `dump_finished` stats every block of a 42k-file tb/ directory
    # (~6 s a leg), and the library only grows: with the expensive check first, each cycle would
    # re-verify every leg already built before reaching the next candidate, and the gap between
    # builds would grow from seconds to minutes. The progress table and the two-failure rule are
    # one awk each, so they decide almost every directory without touching the drive.
    if done_leg "$leg"; then continue; fi
    if failed_twice "$leg"; then
      [ "$DRY" = "1" ] && log "[dry] $leg: would skip (failed twice)"
      continue
    fi
    dump_finished "$d" || continue
    captures_complete "$d" "$leg" || continue
    if [ "$DRY" = "1" ]; then
      log "[dry] $leg: would build now"
      built_this_round=1
      continue
    fi
    build_leg "$leg"
    built_this_round=1
  done

  if [ "$DRY" = "1" ]; then
    log "[dry] done listing"; exit 0
  fi

  now=$(date +%s)
  if [ "$built_this_round" = "0" ] && [ $(( now - last_heartbeat )) -ge 1800 ]; then
    complete=$(for d in "$CORPUS"/tb_*; do
      leg=${d##*/tb_}; case "$leg" in "$PREFIX"*) ;; *) continue ;; esac
      dump_finished "$d" && echo x
    done | wc -l | tr -d ' ')
    built=$(awk -F, '$2=="ok"{n++} END{print n+0}' "$PROGRESS")
    log "waiting: $complete finished dump(s), $built built, $(free_gib "$CORPUS") GiB free on corpus"
    last_heartbeat=$now
  fi
  [ "$built_this_round" = "0" ] && sleep "$POLL"
done
