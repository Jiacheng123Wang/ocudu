#!/usr/bin/env bash
# Archive every capture leg as one tar.gz on the storage host, but only after its mother set is built AND clean.
#
# THE ORDER IS THE POINT. The raw captures are the only copy of the corpus, so a leg is not archived until the thing
# derived from it (the mother set) has passed its audit: `<build-out>/.done_<leg>` exists, which `build_mother_set.sh`
# writes only when every pair built and `check_depth3_dataset.py` printed `findings: none`. A leg that is missing,
# empty or reported findings is skipped and listed at the end -- never archived as if it were fine.
#
# WHY THE DIRECT LINK. The Mac and the storage host are joined by an Ethernet cable (198.19.0.1 <-> 198.19.0.2,
# 0.4 ms, ~80 MB/s measured) while the Wi-Fi path is ~110 ms; the archive goes over the cable.
#
# VERIFICATION. A tarball is accepted when its entry count equals the leg's file count on the source side and gzip
# reports the stream intact. COPYFILE_DISABLE=1 keeps macOS from injecting `._*` AppleDouble entries, which would
# double the entry count and confuse the comparison.
#
# Usage: archive_legs.sh <corpus-root> <build-out> <host> <dest-dir> [--legs N] [--min-free-gb G]
set -u
CORPUS=${1:?usage: archive_legs.sh <corpus-root> <build-out> <host> <dest-dir> [--legs N]}
BUILD_OUT=${2:?}
HOST=${3:?}
DEST=${4:?}
shift 4
LEGS=8
MIN_FREE=200
WAIT=0
while [ $# -gt 0 ]; do
  case "$1" in
    --legs) LEGS=$2; shift 2 ;;
    --min-free-gb) MIN_FREE=$2; shift 2 ;;
    --wait) WAIT=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
LOG=${LOG:-$HOME/ai_depth3/runs/archive_legs.log}
mkdir -p "$(dirname "$LOG")"
say() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
say "=== archive start: corpus=$CORPUS build=$BUILD_OUT host=$HOST dest=$DEST legs=$LEGS wait=$WAIT"

# ARCHIVING STARTS ONLY WHEN THE BUILD HAS STOPPED. With --wait the driver sits until no builder process is alive and
# every capture leg has its marker; without it a leg whose build is still running is skipped, which would archive a
# corpus whose mother set was never checked -- the one thing this order exists to prevent.
if [ "$WAIT" -eq 1 ]; then
  while :; do
    live=$(pgrep -f "build_[m]other_set.sh" | wc -l | tr -d ' ')
    legs_with_captures=$(ls -d "$CORPUS"/*/ 2>/dev/null | wc -l | tr -d ' ')
    have=$(ls "$BUILD_OUT"/.done_* 2>/dev/null | wc -l | tr -d ' ')
    if [ "${live:-1}" -eq 0 ] && [ "${have:-0}" -ge "${legs_with_captures:-1}" ]; then break; fi
    say "--- waiting for the build: builders=$live markers=$have/$legs_with_captures"
    sleep 300
  done
  say "=== build finished ($have markers); starting the archive"
fi

leg_ok() {
  leg=$1
  [ -e "$BUILD_OUT/.done_$leg" ] || { echo "no build marker"; return 1; }
  log=$BUILD_OUT/logs/$leg.log
  [ -f "$log" ] || { echo "no build log"; return 1; }
  # THE AUDIT BANNER HAS NO COLON: `check_depth3_dataset.py` prints `--- findings ---` and the verdict on the next
  # line. Looking for "findings:" matched nothing in any of the 73 logs and the first archiver run therefore skipped
  # every leg -- a checker bug, not a build problem.
  grep -q -- "--- findings ---" "$log" || { echo "no audit line"; return 1; }
  grep -A2 -- "--- findings ---" "$log" | grep -q "none" || { echo "audit reported findings"; return 1; }
  n=$(ls "$BUILD_OUT/${leg}_"*.npz 2>/dev/null | wc -l | tr -d ' ')
  [ "${n:-0}" -ge 1 ] || { echo "no npz"; return 1; }
  echo "ok"
}

running=0
for cap in "$CORPUS"/*/; do
  leg=$(basename "$cap")
  if ssh -o BatchMode=yes "$HOST" "test -s '$DEST/$leg.tar.gz'" 2>/dev/null; then
    say "skip $leg (tarball already there)"
    continue
  fi
  verdict=$(leg_ok "$leg")
  if [ "$verdict" != "ok" ]; then
    say "SKIP $leg: $verdict"
    continue
  fi
  while [ "$running" -ge "$LEGS" ]; do running=$(jobs -rp | wc -l | tr -d ' '); sleep 10; done
  free_gb=$(ssh -o BatchMode=yes "$HOST" "df -BG --output=avail '$DEST' | tail -1 | tr -dc '0-9'" 2>/dev/null)
  while [ "${free_gb:-0}" -lt "$MIN_FREE" ]; do
    say "--- waiting: $HOST free ${free_gb} GB < ${MIN_FREE} GB"
    sleep 300
    free_gb=$(ssh -o BatchMode=yes "$HOST" "df -BG --output=avail '$DEST' | tail -1 | tr -dc '0-9'" 2>/dev/null)
  done
  (
    n_local=$(find "$CORPUS/$leg" -type f | wc -l | tr -d ' ')
    sz_local=$(du -sk "$CORPUS/$leg" | awk '{printf "%.1f", $1/1048576}')
    # gzip -1, NOT -6. Measured on this corpus: level 1 compresses 5.3x faster (81 vs 15 MB/s) for 6% more bytes
    # (78.4 vs 74.1 MB per 205 MB of captures). With eight legs in flight the cable (~80 MB/s) becomes the limit
    # instead of one core per stream, which is the whole point of running several at once.
    if set -o pipefail; COPYFILE_DISABLE=1 tar cf - -C "$CORPUS" "$leg" | gzip -1 | \
        ssh -o BatchMode=yes "$HOST" "cat > '$DEST/$leg.tar.gz.part' && mv '$DEST/$leg.tar.gz.part' '$DEST/$leg.tar.gz'"; then
      n_tar=$(ssh -o BatchMode=yes "$HOST" "tar tzf '$DEST/$leg.tar.gz' | grep -vc '/\$'" 2>/dev/null | tr -d ' ')
      if ssh -o BatchMode=yes "$HOST" "gzip -t '$DEST/$leg.tar.gz'" 2>/dev/null; then
        if [ "${n_tar:-0}" = "$n_local" ]; then
          sz_tar=$(ssh -o BatchMode=yes "$HOST" "stat -c %s '$DEST/$leg.tar.gz'" 2>/dev/null)
          echo "[$(date '+%F %T')] [$leg] OK files=$n_local entries=$n_tar raw=${sz_local} GB tar=$((sz_tar/1048576)) MB" >> "$LOG"
        else
          echo "[$(date '+%F %T')] [$leg] MISMATCH files=$n_local entries=${n_tar:-?}" >> "$LOG"
        fi
      else
        echo "[$(date '+%F %T')] [$leg] GZIP FAILED" >> "$LOG"
      fi
    else
      echo "[$(date '+%F %T')] [$leg] TRANSFER FAILED" >> "$LOG"
      ssh -o BatchMode=yes "$HOST" "rm -f '$DEST/$leg.tar.gz.part'" 2>/dev/null
    fi
  ) &
  running=$(jobs -rp | wc -l | tr -d ' ')
  say "--- started $leg ($running in flight)"
done
wait
say "=== archive done"
