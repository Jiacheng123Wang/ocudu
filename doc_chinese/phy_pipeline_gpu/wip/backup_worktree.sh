#!/usr/bin/env bash
# Save the WORKING TREE before any operation that can destroy it (reset --hard, checkout --, clean).
#
# ---- Why this exists ----
# A `git reset --hard` during a baseline switch destroyed the bench's UNCOMMITTED changes to
# configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml (gains, clock source, pusch.max_ue_mcs,
# pucch.max_consecutive_kos). Those changes were in no commit and in no backup, so they could not be
# recovered - the bench had to be reconfigured by hand. Printing a diff on screen is NOT a backup.
#
# Tracked modifications are cheap to preserve (`git stash` does it), but the ones that actually get
# lost are the ones nobody thought about: the same file edited by someone else, configs the leg
# script reads, and untracked helper scripts. So this saves BOTH:
#   * a stash-like patch of every tracked modification, and
#   * a copy of every untracked file that is not build output.
#
# usage: bash wip/backup_worktree.sh [label]     (default label: timestamp)
# Prints the backup directory. Nothing is modified in the repository.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LABEL=${1:-$(date +%m%d_%H%M%S)}
DEST=$ROOT/doc_chinese/phy_pipeline_gpu/wip/worktree_backups/$LABEL

mkdir -p "$DEST"

cd "$ROOT"

# 1) Tracked modifications and staged changes, as a patch that `git apply` can replay.
git diff HEAD > "$DEST/tracked.patch" 2>/dev/null || true
git status --porcelain=v1 > "$DEST/status.txt" 2>/dev/null || true
git rev-parse HEAD > "$DEST/HEAD.txt" 2>/dev/null || true

# 2) Untracked files, EXCLUDING build output and the doc tree itself (which is gitignored and huge).
#    A file that is ignored but hand-maintained (a local config) is exactly the kind that gets lost,
#    so it is copied too when it is small enough to be a config rather than an artifact.
git ls-files --others --exclude-standard -z 2>/dev/null |
  while IFS= read -r -d '' f; do
    case "$f" in
      build/*|Testing/*|*.o|*.a|*.csv) continue ;;
    esac
    mkdir -p "$DEST/untracked/$(dirname "$f")"
    cp -p "$f" "$DEST/untracked/$f" 2>/dev/null || true
  done

# 3) The configs the leg script actually reads, unconditionally: they are small, and a lost one has
#    already cost a bench reconfiguration once.
mkdir -p "$DEST/configs"
for c in configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
         configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17 \
         configs/gnb_zmq.yaml; do
  [ -f "$c" ] && cp -p "$c" "$DEST/configs/$(basename "$c")"
done

echo "worktree backed up to: $DEST"
echo "  HEAD            : $(cat "$DEST/HEAD.txt" 2>/dev/null)"
echo "  tracked patch   : $(wc -l < "$DEST/tracked.patch" | tr -d ' ') lines"
echo "  untracked copies: $(find "$DEST/untracked" -type f 2>/dev/null | wc -l | tr -d ' ')"
echo "  configs copies  : $(find "$DEST/configs" -type f 2>/dev/null | wc -l | tr -d ' ')"
echo
echo "restore a tracked change with:  git apply $DEST/tracked.patch"
echo "restore a config with        :  cp $DEST/configs/<name> configs/"
