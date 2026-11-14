#!/usr/bin/env bash
# Archive every capture leg as one tar.gz on the storage host, verified before it is promoted, and only for legs whose
# mother set passed its audit.
#
# WHAT WENT WRONG IN THE FIRST VERSION, AND WHY THIS ONE IS DIFFERENT.
#   1. `tar czf` (gzip -6) ran at ~15 MB/s on this data. gzip -1 compresses the same stream at 81 MB/s for 6% more
#      bytes (measured on 205 MB of captures: 13.4 s -> 2.5 s), so the cable becomes the limit instead of one core.
#   2. The remote side was `cat > part && mv part final`. When the sender was killed the pipe closed, cat saw EOF and
#      `mv` promoted a TRUNCATED file to the final name -- three legs were archived as if complete and had to be
#      rebuilt. Promotion is now conditional on `tar tzf` succeeding and the entry count matching the source, and the
#      verification is that same single pass (tar reading the stream to the end is what checks the gzip integrity, so
#      a separate `gzip -t` would only reread the file).
#   3. A killed transfer must leave nothing behind: the remote side removes its own `.part` on a mismatch.
#
# Usage: archive_legs2.sh <corpus-root> <build-out> <host> <dest-dir> [--legs N] [--min-free-gb G]
set -u
CORPUS=${1:?usage: archive_legs2.sh <corpus-root> <build-out> <host> <dest-dir> [--legs N]}
BUILD_OUT=${2:?}
HOST=${3:?}
DEST=${4:?}
shift 4
LEGS=8
MIN_FREE=200
while [ $# -gt 0 ]; do
  case "$1" in
    --legs) LEGS=$2; shift 2 ;;
    --min-free-gb) MIN_FREE=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
LOG=${LOG:-$HOME/ai_depth3/runs/archive_legs2.log}
REMOTE_HELPER=${REMOTE_HELPER:-/tmp/archive_one_remote.sh}
mkdir -p "$(dirname "$LOG")"
say() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
say "=== archive start: corpus=$CORPUS host=$HOST dest=$DEST legs=$LEGS"

leg_ok() {
  leg=$1
  [ -e "$BUILD_OUT/.done_$leg" ] || { echo "no build marker"; return 1; }
  log=$BUILD_OUT/logs/$leg.log
  [ -f "$log" ] || { echo "no build log"; return 1; }
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
  [ "$verdict" = "ok" ] || { say "SKIP $leg: $verdict"; continue; }
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
    out=$(set -o pipefail; COPYFILE_DISABLE=1 tar cf - -C "$CORPUS" "$leg" | gzip -1 | \
          ssh -o BatchMode=yes "$HOST" "$REMOTE_HELPER '$DEST/$leg.tar.gz.part' '$DEST/$leg.tar.gz' '$n_local'" 2>&1 | tail -1)
    status=$?
    if [ $status -ne 0 ]; then
      echo "[$(date '+%F %T')] [$leg] FAILED transfer (status $status): ${out:-no output}" >> "$LOG"
    elif [ "${out%% *}" = "OK" ]; then
      sz_tar=$(ssh -o BatchMode=yes "$HOST" "stat -c %s '$DEST/$leg.tar.gz'" 2>/dev/null)
      echo "[$(date '+%F %T')] [$leg] ARCHIVED files=$n_local entries=${out##* } raw=${sz_local} GB tar=$((sz_tar/1048576)) MB" >> "$LOG"
    else
      echo "[$(date '+%F %T')] [$leg] REJECTED: ${out:-no output}" >> "$LOG"
    fi
  ) &
  running=$(jobs -rp | wc -l | tr -d ' ')
  say "--- started $leg ($running in flight)"
done
wait
say "=== archive done"
