#!/usr/bin/env bash
# THE NEXT LEG LABEL for the llr_ai_detection workstream - the monotonic sequence number, computed instead of
# remembered.
#
# WHY: legs flown weeks apart are found by their LABEL, and an un-numbered label says which phase and which arm
# but not WHICH ONE CAME FIRST. A monotonic prefix - `aillr001-baseline` - makes a label also a position in the
# record.
#
# COPIED from doc_chinese/metal_kernel_fusion/wip/next_leg_label.sh (the previous workstream): same logic,
# different prefix. `mkf` = metal kernel fusion; `aillr` = AI LLR detection. The two sequences are INDEPENDENT
# - aillr001 has nothing to do with mkf001. See doc_chinese/llr_ai_detection/wip/README.md section 2.
#
# It counts the leg logs themselves, filtered to THIS workstream's prefix, so the number cannot drift from what
# was flown. A flown label is NEVER renamed - a leg label is evidence.
#
# usage:  bash next_leg_label.sh                 # just print the next number, e.g. 001
#         bash next_leg_label.sh baseline        # print the full next label, e.g. aillr001-baseline
set -u

PREFIX="aillr"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

# The caller's argument is saved FIRST: the counting below re-uses the positional parameters (`set -- $labels`),
# which would otherwise silently eat the suffix it was given.
suffix="${1:-}"

labels=""
maxnum=0
for _d in "$ROOT"/doc_chinese/*/wip/logs; do
  [ -d "$_d" ] || continue
  for _f in "$_d"/gnb_*_"$PREFIX"*.log.stderr; do
    [ -e "$_f" ] || continue
    # The label is everything between the mode prefix (gnb_<mode>_) and the _MMDD_HHMM date stamp.
    _l=$(basename "$_f" | sed -E "s/^gnb_[a-z_]+_(${PREFIX}[^_]*(-[a-z0-9]+)*)_[0-9]{4}_[0-9]{4}\.log\.stderr\$/\1/")
    case "$_l" in "$PREFIX"*) ;; *) continue ;; esac
    case " $labels " in *" $_l "*) ;; *) labels="$labels $_l" ;; esac
    _n=$(printf '%s' "$_l" | sed -nE "s/^${PREFIX}([0-9]{3})-.*/\1/p")
    if [ -n "$_n" ]; then
      # 10# forces base 10: "008" is not an octal number.
      _n=$((10#$_n))
      [ "$_n" -gt "$maxnum" ] && maxnum=$_n
    fi
  done
done

# set -- turns the label list into positional parameters; $# is the number of DISTINCT legs flown.
# shellcheck disable=SC2086
set -- $labels
count=$#
next=$((count + 1))
[ "$maxnum" -ge "$next" ] && next=$((maxnum + 1))

if [ -n "$suffix" ]; then
  printf '%s%03d-%s\n' "$PREFIX" "$next" "$suffix"
else
  printf '%03d  (of %d %s legs already flown)\n' "$next" "$count" "$PREFIX"
fi
