#!/usr/bin/env bash
# THE NEXT LEG LABEL — the workstream's monotonic sequence number, computed instead of remembered.
#
# WHY (user, 2026-10-06): legs flown weeks apart are found by their LABEL, and this workstream's first seven
# labels (`mkf-m0-base`, `mkf-m0-abl-eq`, ...) say which phase and which arm but not WHICH ONE CAME FIRST.
# The user asked for a monotonic prefix - `mkf001-m0c-metal` - so a label is also a position in the record.
#
# It counts the leg logs themselves (any workstream root), so the number cannot drift from what was flown and it
# never has to be edited when a workstream directory is added. The first seven legs already flown carry the old
# un-numbered form; they are COUNTED, not renamed (a flown label is evidence and does not change), which is why
# the next label is 008 and not 001.
#
# usage:  bash next_leg_label.sh                 # just print the next number, e.g. 008
#         bash next_leg_label.sh m0c-metal       # print the full next label, e.g. mkf008-m0c-metal
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

# The caller's argument is saved FIRST: the counting below re-uses the positional parameters (`set -- $labels`),
# which silently ate the suffix it was given - caught by running the helper rather than reading it.
suffix="${1:-}"

labels=""
maxnum=0
for _d in "$ROOT"/doc_chinese/*/wip/logs; do
  [ -d "$_d" ] || continue
  for _f in "$_d"/gnb_*_mkf*.log.stderr; do
    [ -e "$_f" ] || continue
    # The label is everything between the mode prefix (gnb_<mode>_) and the _MMDD_HHMM date stamp.
    _l=$(basename "$_f" | sed -E 's/^gnb_[a-z_]+_(mkf[^_]*(-[a-z0-9]+)*)_[0-9]{4}_[0-9]{4}\.log\.stderr$/\1/')
    case "$_l" in mkf*) ;; *) continue ;; esac
    case " $labels " in *" $_l "*) ;; *) labels="$labels $_l" ;; esac
    _n=$(printf '%s' "$_l" | sed -nE 's/^mkf([0-9]{3})-.*/\1/p')
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
  printf 'mkf%03d-%s\n' "$next" "$suffix"
else
  printf '%03d  (of %d legs already flown)\n' "$next" "$count"
fi
