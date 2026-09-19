#!/usr/bin/env bash
# Replay the real 1-2 PRB captures through every arm of the narrow-hop question - and ASSERT the
# identity of each arm from its own log.
#
# ---- Why the guards exist (S13-P2c errata, 2026-09-20) ----
# The first four-arm run had an arm named `cpu/` that was never the CPU chain. `--cpu` is a plain flag
# (ul_chain_replay.cpp:246: it clears use_metal_ce/use_metal_demod/use_metal_decoder), the tool prints
# the backends it actually used in a footer - `replay <cap> -> <out>  (cpu CE, cpu equalizer/demapper,
# cpu LDPC)` - and NOBODY compared that footer against the arm's name. The arm turned out to be a
# duplicate of the host-built-A arm (same `device_corr_builds`/`refusals` fingerprints, and 19 of the
# 20 captures byte-identical), and the note "the CPU reference also fails on every narrow hop" in the
# design document was that artifact. The real CPU chain decodes them (see the file's summary table).
#
# So every arm is now identified by TWO fingerprints taken from its own log, and a mismatch FAILS the
# run instead of becoming a footnote:
#   * the backends the tool reports (the footer) - and NOTE that the tool's DEFAULT is the CPU chain
#     (ul_chain_replay.cpp:205-207 initialise use_metal_* to false), so a metal arm that forgets
#     `--metal` silently replays on the CPU and agrees with the `cpu` arm byte for byte. That is the
#     second way this table can lie, and the footer guard catches it too, and
#   * the route the estimator reports (`device_corr_builds` and the `refusals=` reason), which differs
#     between "the fix is in this binary" and "the host still builds A for a hop narrower than a block";
# plus the ARTIFACT identity: the `dev` arm's binary must be the current build (md5), and the `host`
# arm's must not be.
#
# usage: bash narrow_arms.sh [outdir] [capture-glob]
#
# Arms:
#   cpu     current build, `--cpu`                       -> the actual CPU reference chain
#   dev     current build, default                       -> the device builds the narrow hop's A
#   host    pre-fix binary, default                      -> the host builds it (the S13-P2c symptom)
#   hostsc  pre-fix binary, OCUDU_CE_HOST_SCALARS=1      -> host build WITH the device's scalars (the
#                                                           mechanism proof: must equal `dev`)
#   exactab current build, CORR_DEV=0 + HOST_SCALARS=1   -> the host builds A with the device's scalars.
#                                                           This is the EXACT A/B of the fix: its three
#                                                           DATA dumps must be byte-identical to `dev`'s
#                                                           (asserted below), which is what makes
#                                                           "the device route == the host route with the
#                                                           right diagonal loading" a measurement rather
#                                                           than an argument. Only `_ce.txt` may differ,
#                                                           and only in its `cfo_hz` field (`na` on the
#                                                           device route, a number when the host builds).
#\warning EVERY ARM RUNS SERIALLY, one process at a time, and it must stay that way: the tool's own
# header (ul_chain_replay.cpp:32-38) records that under heavy parallelism it produces WRONG results -
# measured, 39 of 980 captures differed between two runs of the SAME configuration. A serial run costs
# 0.12 s, so there is nothing to gain. For the record: the archived `narrow_cmp/` table took 0.57 s per
# replay (37 s for 65 dumps), i.e. it was NOT serial, and exactly one of its dumps (cap_3322's host arm,
# md5 63a9fdd5…, -8.26 dB) cannot be reproduced by either pre-fix binary today - the failure mode this
# warning describes.
#   ARMS="cpu dev host hostsc exactab"   which arms to run (default: all five)
#   CMP_TREE=<binary>            the current build      (default: build/.../ul_chain_replay)
#   BIN_HOSTA=<binary>           the pre-fix reference  (default: doc_chinese/work_tmp/ref/replay_narrow_hostA)
#
# Arms:
#   cpu     current build, `--cpu`                       -> the actual CPU reference chain
#   dev     current build, default                       -> the device builds the narrow hop's A
#   host    pre-fix binary, default                      -> the host builds it (the S13-P2c symptom)
#   hostsc  pre-fix binary, OCUDU_CE_HOST_SCALARS=1      -> host build WITH the device's scalars (the
#                                                           mechanism proof: must equal `dev`)
# Exit: 0 all arms ran and every fingerprint matched; 3 a fingerprint or an artifact check failed.
set -u

OUT=${1:-doc_chinese/work_tmp/narrow_cmp2}
GLOB=${2:-doc_chinese/work_tmp/narrow_cap/cap_*.bin}
ARMS=${ARMS:-"cpu dev host hostsc exactab"}
CMP_TREE=${CMP_TREE:-build/lib/phy/upper/channel_processors/metal/ul_chain_replay}
BIN_HOSTA=${BIN_HOSTA:-doc_chinese/work_tmp/ref/replay_narrow_hostA}

for b in "$CMP_TREE" "$BIN_HOSTA"; do
  [ -x "$b" ] || { echo "missing binary: $b"; exit 2; }
done

MD5_TREE=$(md5 -q "$CMP_TREE")
MD5_HOSTA=$(md5 -q "$BIN_HOSTA")
echo "current build : $CMP_TREE  md5=$MD5_TREE"
echo "pre-fix binary: $BIN_HOSTA  md5=$MD5_HOSTA"
[ "$MD5_TREE" != "$MD5_HOSTA" ] || { echo "FATAL: the two arms are the same artifact"; exit 2; }
echo

mkdir -p "$OUT"
AB_N=0
AB_BAD=0
CE_N=0
SUMMARY="$OUT/summary.tsv"
printf "capture\tnrb\tarm\tcrc\titers\tsinr\tfingerprint\tguard\n" > "$SUMMARY"
FAILED=0

for f in $GLOB; do
  base=${f%.bin}
  # the captures directory also holds this tool's own outputs (`*_h.bin`, `*_llr.bin`): skip them
  case "$base" in *_h | *_llr) continue ;; esac
  cap=$(basename "$base")
  nrb=$(grep -m1 '^alloc_nof_rb=' "$base.txt" 2>/dev/null | cut -d= -f2)
  nrb=${nrb:-?}
  mkdir -p "$OUT/$cap"

  for arm in $ARMS; do
    case "$arm" in
      cpu)    ENV=""                        BIN="$CMP_TREE" ARGS="--cpu"   ; WANT="(cpu CE, cpu equalizer/demapper, cpu LDPC)"  ;;
      dev)    ENV=""                        BIN="$CMP_TREE" ARGS="--metal" ; WANT="(metal CE, metal equalizer/demapper, metal LDPC)" ;;
      host)   ENV=""                        BIN="$BIN_HOSTA" ARGS="--metal"; WANT="(metal CE, metal equalizer/demapper, metal LDPC)" ;;
      hostsc) ENV="OCUDU_CE_HOST_SCALARS=1" BIN="$BIN_HOSTA" ARGS="--metal"; WANT="(metal CE, metal equalizer/demapper, metal LDPC)" ;;
      exactab) ENV="OCUDU_CE_CORR_DEV=0 OCUDU_CE_HOST_SCALARS=1" BIN="$CMP_TREE" ARGS="--metal"; WANT="(metal CE, metal equalizer/demapper, metal LDPC)" ;;
      *) echo "unknown arm $arm"; exit 2 ;;
    esac

    log="$OUT/$cap/$arm.log"
    env $ENV "$BIN" "$base" $ARGS --out "$OUT/$cap/$arm" > "$log" 2>&1
    rc=$?

    # --- guard 1: the backends the tool actually used (this is the check that was missing) ---
    footer=$(grep -a -m1 '^replay ' "$log")
    guard="ok"
    case "$footer" in
      *"$WANT"*) ;;
      *) guard="FOOTER-MISMATCH" ;;
    esac

    # --- guard 2: the route the estimator reports, per arm ---
    refusals=$(grep -a -m1 '\[metal_stats\] mmse_ce' "$log" | grep -o 'refusals=[^ ]*')
    corr_builds=$(grep -a -m1 '\[metal_stats\] mmse_ce' "$log" | grep -o 'device_corr_builds=[0-9]*' | cut -d= -f2)
    # the CPU chain's own route: the demodulator must report that the HOST produced every estimate
    est_dev=$(grep -a -m1 'ce device estimates' "$log" | grep -o '[0-9]* device' | grep -o '[0-9]*')
    est_host=$(grep -a -m1 'ce device estimates' "$log" | grep -o '[0-9]* host' | grep -o '[0-9]*')
    fp="-"
    case "$arm" in
      cpu)    fp="cpu-chain estimates=${est_dev:-?}/${est_host:-?}"
              { [ "$guard" = "ok" ] && [ "${est_dev:-1}" -eq 0 ] && [ "${est_host:-0}" -ge 1 ]; } || guard="ROUTE-MISMATCH" ;;
      dev)    fp="builds=$corr_builds ${refusals:-refusals=n/a}"
              { [ "${corr_builds:-0}" -ge 1 ] && [ "${refusals:-}" = "refusals=<none>" ]; } || guard="ROUTE-MISMATCH" ;;
      host)   fp="builds=$corr_builds ${refusals:-refusals=n/a}"
              { [ "${corr_builds:-0}" -eq 0 ] && [ "${refusals:-}" != "refusals=<none>" ]; } || guard="ROUTE-MISMATCH" ;;
      hostsc) fp="builds=$corr_builds ${refusals:-refusals=n/a}"
              { [ "${corr_builds:-0}" -eq 0 ] && [ "${refusals:-}" != "refusals=<none>" ]; } || guard="ROUTE-MISMATCH" ;;
      exactab) fp="builds=$corr_builds ${refusals:-refusals=n/a}"
              { [ "${corr_builds:-0}" -eq 0 ] && [ "${refusals:-}" = "refusals=corr_disabled=1" ]; } || guard="ROUTE-MISMATCH" ;;
    esac

    # --- guard 3: a missing dump must never read as a result ---
    if ! ls "$OUT/$cap/${arm}_"*_h.bin >/dev/null 2>&1; then
      guard="NO-DUMP"
    fi
    [ "$rc" -eq 0 ] || guard="EXIT-$rc"

    line=$(grep -a -m1 'crc=' "$log")
    crc=$(echo "$line"  | grep -o 'crc=[A-Za-z]*'     | cut -d= -f2)
    iters=$(echo "$line" | grep -o 'iterations=[0-9]*' | cut -d= -f2)
    sinr=$(echo "$line"  | grep -o 'sinr=[-0-9.]*'     | cut -d= -f2)
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "$cap" "$nrb" "$arm" "${crc:--}" "${iters:--}" "${sinr:--}" "$fp" "$guard" >> "$SUMMARY"
    [ "$guard" = "ok" ] || { FAILED=1; echo "GUARD FAILED: $cap $arm -> $guard"; }
  done

  # --- guard 4: the EXACT A/B. `exactab` (host builds A with the device's scalars) must reproduce
  #     `dev`'s three DATA dumps byte for byte. Only the scalar text dump may differ, and only in the
  #     `cfo_hz` field (`na` on the device route, a number when the host builds) - report it, since a
  #     second differing field would be a finding. ---
  dev_h=$(ls "$OUT/$cap/"dev_*_h.bin 2>/dev/null | head -1)
  ab_h=$(ls "$OUT/$cap/"exactab_*_h.bin 2>/dev/null | head -1)
  if [ -n "$dev_h" ] && [ -n "$ab_h" ]; then
    AB_N=$((AB_N + 1))
    for suf in _h.bin _llr.bin .bin; do
      a=$(ls "$OUT/$cap/"dev_*"$suf" 2>/dev/null | head -1)
      b=$(ls "$OUT/$cap/"exactab_*"$suf" 2>/dev/null | head -1)
      cmp -s "$a" "$b" || { echo "AB-MISMATCH: $cap $suf"; AB_BAD=$((AB_BAD + 1)); FAILED=1; }
    done
    a=$(ls "$OUT/$cap/"dev_*_ce.txt 2>/dev/null | head -1)
    b=$(ls "$OUT/$cap/"exactab_*_ce.txt 2>/dev/null | head -1)
    if ! cmp -s "$a" "$b"; then
      fields=$(diff <(tr ' ' '\n' < "$a") <(tr ' ' '\n' < "$b") | grep -c '^[<>]')
      where=$(diff <(tr ' ' '\n' < "$a") <(tr ' ' '\n' < "$b") | grep '^[<>]' | tr '\n' ' ')
      CE_N=$((CE_N + 1))
      echo "note: $cap _ce.txt differs in $fields field token(s): $where"
    fi
  fi
done

echo
column -t -s $'\t' "$SUMMARY"
echo
if [ "$AB_N" -ne 0 ]; then
  echo "exact A/B (CORR_DEV=0 + HOST_SCALARS=1 vs dev): $AB_N capture(s) compared, $AB_BAD data dump(s) different, $CE_N _ce.txt different"
fi
if [ "$FAILED" -ne 0 ]; then
  echo "some arm(s) failed their fingerprint - the table above is NOT usable as evidence"
  exit 3
fi
echo "every arm matched its fingerprint; table: $SUMMARY"
