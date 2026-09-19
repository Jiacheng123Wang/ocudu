#!/usr/bin/env bash
# Compare the PUBLISHED DUMPS of TWO ul_chain_replay BINARIES over the whole capture corpus.
#
# ---- Why this exists (and why ab_dumps.sh is not it) ----
# ab_dumps.sh compares two ENVIRONMENTS of ONE binary. That answers "does knob X change anything",
# which is the right question for an A/B of two routes that both exist in the shipped build. It is the
# WRONG question for a change that replaces an implementation, because both arms would then run the
# new code and agree with each other while both differing from what was shipped. Batch 5g (the symbol
# start epochs move from a host array to a device computation) is exactly that case: the criterion is
# "the new binary reproduces the OLD BINARY's bytes", so the OLD BINARY has to be kept.
#
# The HEAD binary is stashed before the edit:
#     cp build/lib/phy/upper/channel_processors/metal/ul_chain_replay /tmp/replay_head
#
# ---- The guards (each one is a measurement that was wrong once, see ab_dumps.sh) ----
#   * every run gets its own directory, so a dump left by a previous capture cannot be picked up - and
#     the directory is CREATED first: `--out` names a file prefix inside it, and a missing one makes
#     ul_capture::capture_h() return before it asks the estimator for the estimates, which removes the
#     host-grid read from the crossing line and makes a real run look like a smaller one;
#   * the KERNELS are paired explicitly (AB_METALLIB_A / AB_METALLIB_B): the engine loads the
#     .metallib from the absolute source-tree path baked in at configure time, so the reference binary
#     and the new one share that one file unless the script swaps it around each side. The pairing is
#     then ASSERTED from the logs (the reference build must not print [epoch_impl]);
#   * a MISSING dump on either side is counted and the script FAILS: "no data" must never read as
#     "equal";
#   * the four dumps are compared byte for byte per capture, so a pass cannot come from the average;
#   * the crossing line and the write-site table of BOTH sides are printed, because that is the other
#     half of the batch-5g criterion (the write side must reach 0 and the site table must be empty).
#
# usage: bash ab_replay_bins.sh <binA> <binB> [env] [mode-ARGS] [corpus-glob]
#   AB_MARKER=<name>   the self-announcement only side B prints (default: epoch_impl, batch 5g's).
#   e.g. AB_METALLIB_A=/tmp/mmse_head.metallib AB_METALLIB_B=/tmp/mmse_new.metallib \
#          bash ab_replay_bins.sh /tmp/replay_head build/.../ul_chain_replay
# Exit: 0 when every capture produced dumps on both sides AND the four dumps are byte-identical;
#       1 when they differ; 2 when a dump is missing, a binary is absent, or the pairing is wrong.
set -u
BIN_A=${1:?usage: ab_replay_bins.sh <binA> <binB> [env] [mode] [glob]}
BIN_B=${2:?usage: ab_replay_bins.sh <binA> <binB> [env] [mode] [glob]}
ENV=${3:-}
MODE=${4:---metal}
ROOT=/Users/jiachengwang/dev/ocudu
GLOB=${5:-$ROOT/doc_chinese/work_tmp/corpus/*.bin}
# The pairing marker: a line "[<marker>] ..." the reference build must NOT print and the new one must.
MARKER=${AB_MARKER:-epoch_impl}
# The KERNELS are a separate artifact and the engine loads them from the absolute source-tree path
# baked in at configure time (OCUDU_MMSE_METALLIB_PATH; there is no environment override yet), so a
# binary and its .metallib can be paired wrongly - which is the trap this port has hit more than once.
# A and B therefore name their own metallib, and the script swaps them in AROUND each side's runs and
# puts the original back on exit. Set both or neither.
ML_A=${AB_METALLIB_A:-}
ML_B=${AB_METALLIB_B:-}
MMSE_ML=$ROOT/lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib
for b in "$BIN_A" "$BIN_B"; do
  [ -x "$b" ] || { echo "missing $b (build it first)" >&2; exit 2; }
done
if [ -n "$ML_A" ] || [ -n "$ML_B" ]; then
  for m in "$ML_A" "$ML_B" "$MMSE_ML"; do
    [ -f "$m" ] || { echo "missing metallib $m" >&2; exit 2; }
  done
fi
CAPTURES=$(ls $GLOB 2>/dev/null | sed -E 's/\.bin$//')
if [ -z "$CAPTURES" ]; then echo "no captures match $GLOB" >&2; exit 2; fi

WORK=$(mktemp -d)
# AB_KEEP=<dir>: copy each side's dumps there, per capture, before WORK is removed. The per-byte diff
# above says HOW MUCH differs; the dumps are what says WHICH FIELD does (e.g. a reporting value whose
# reduction moved to the device, S13-P2's epre: the LLR/h/bin dumps are identical and _ce.txt differs
# in that one field by a few ulps).
KEEP=${AB_KEEP:-}
restore() {
  # Put the metallib the tree had back, whatever happened (a missing one would leave the NEXT run
  # loading nothing at all - the failure mode is silent, so it must not depend on the script's exit).
  if [ -n "${ML_SAVED:-}" ]; then cp "$ML_SAVED" "$MMSE_ML"; fi
  rm -rf "$WORK"
}
trap restore EXIT
if [ -n "$ML_A" ] || [ -n "$ML_B" ]; then
  ML_SAVED=$WORK/metallib.orig
  cp "$MMSE_ML" "$ML_SAVED"
fi

n=0; miss=0; badcaps=0; pairing=0
d_llr=0; d_h=0; d_bin=0; d_ce=0
for c in $CAPTURES; do
  n=$((n+1))
  name=$(basename "$c")
  a="$WORK/A$n"; b="$WORK/B$n"
  mkdir -p "$a" "$b"
  # The dump directory MUST exist: --out names a file PREFIX inside it, and ul_capture::capture_h()
  # returns before it asks the estimator for anything when its fopen fails - which silently removes
  # the host-grid read the crossing line reports (measured: a run into a missing directory showed
  # 0.00 read(s) per hop and looked like a second crossing had gone). mkdir -p above is the guard.
  if [ -n "$ML_A" ]; then cp "$ML_A" "$MMSE_ML"; fi
  # shellcheck disable=SC2086
  bash /tmp/limited_run.sh 120 "$a/log" env $ENV "$BIN_A" "$c" $MODE --out "$a/dump"
  if [ -n "$ML_B" ]; then cp "$ML_B" "$MMSE_ML"; fi
  # shellcheck disable=SC2086
  bash /tmp/limited_run.sh 120 "$b/log" env $ENV "$BIN_B" "$c" $MODE --out "$b/dump"
  # The pairing, asserted rather than assumed: the change under test adds a self-announcement that
  # side A (the reference build) does NOT print and side B does. A wrong pairing agrees with itself and
  # proves nothing. The marker is a parameter because each batch has its own: batch 5g's was
  # [epoch_impl], S13-P2's is [ce_inputs] (the route that extracts the received pilots).
  ga=$(grep -c "^\[$MARKER\]" "$a/log" | tr -d ' ')
  gb=$(grep -c "^\[$MARKER\]" "$b/log" | tr -d ' ')
  if [ "$ga" != "0" ] || [ "$gb" = "0" ]; then
    echo "  PAIRING WRONG: $name  A printed [$MARKER] $ga time(s), B $gb time(s)"
    pairing=$((pairing + 1))
  fi
  fa=$(ls "$a"/dump*_ce.txt 2>/dev/null | head -1)
  fb=$(ls "$b"/dump*_ce.txt 2>/dev/null | head -1)
  if [ -z "$fa" ] || [ -z "$fb" ]; then
    echo "  MISSING DUMP: $name  A=$([ -n "$fa" ] && echo ok || echo NONE)  B=$([ -n "$fb" ] && echo ok || echo NONE)"
    miss=$((miss+1))
    continue
  fi
  cap=0
  for suf in _llr.bin _h.bin .bin _ce.txt; do
    k=$(cmp -l "${fa%_ce.txt}$suf" "${fb%_ce.txt}$suf" 2>/dev/null | wc -l | tr -d ' ')
    case "$suf" in
      _llr.bin) d_llr=$((d_llr + k)) ;;
      _h.bin)   d_h=$((d_h + k)) ;;
      .bin)     d_bin=$((d_bin + k)) ;;
      _ce.txt)  d_ce=$((d_ce + k)) ;;
    esac
    [ "$k" != "0" ] && { echo "  DUMP DIFFERS: $name$suf ($k bytes)"; cap=1; }
  done
  [ "$cap" = "1" ] && badcaps=$((badcaps + 1))
  if [ -n "$KEEP" ]; then
    mkdir -p "$KEEP/$name/A" "$KEEP/$name/B"
    cp "$a"/dump* "$KEEP/$name/A/" 2>/dev/null
    cp "$b"/dump* "$KEEP/$name/B/" 2>/dev/null
  fi
  # The instrument half: the crossing line and the write-site table of each side.
  ca=$(grep -oE "= [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" "$a/log" | tail -1)
  cb=$(grep -oE "= [0-9.]+ read\(s\) \+ [0-9.]+ write\(s\) per hop" "$b/log" | tail -1)
  sa=$(grep -cE "^    [a-z].*call\(s\)" "$a/log" | tr -d ' ')
  sb=$(grep -cE "^    [a-z].*call\(s\)" "$b/log" | tr -d ' ')
  printf "  %-12s A%s sites=%s | B%s sites=%s\n" "$name" "${ca:- ?}" "$sa" "${cb:- ?}" "$sb"
done

total=$((d_llr + d_h + d_bin + d_ce))
echo "A = $BIN_A   kernels=${ML_A:-<tree $MMSE_ML>}   env=${ENV:-<default>}  mode=$MODE   pairing-marker=[$MARKER]"
echo "B = $BIN_B   kernels=${ML_B:-<tree $MMSE_ML>}"
echo "captures=$n  missing-dumps=$miss  captures-with-differences=$badcaps  total-differing-bytes=$total  pairing-wrong=$pairing"
printf "  %-10s %s\n" _llr.bin "$d_llr" _h.bin "$d_h" .bin "$d_bin" _ce.txt "$d_ce"

if [ "$miss" != "0" ]; then
  echo "FAILED: $miss capture(s) produced no dump on one side - this comparison is NOT evidence"
  exit 2
fi
if [ "$pairing" != "0" ]; then
  echo "FAILED: $pairing capture(s) ran the two sides with the same kernel implementation"
  exit 2
fi
if [ "$total" != "0" ]; then
  echo "FAILED: the two binaries do not publish the same bytes"
  exit 1
fi
echo "PASSED: the two binaries publish byte-identical dumps on every capture"
