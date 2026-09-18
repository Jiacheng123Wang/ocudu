#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# The two capture-corpus gates of the Metal channel estimator, run over every staged capture.
# They replace the session-local /tmp scripts the earlier rounds used, so the evidence survives
# /tmp being cleaned.
#
#   capture_gates.sh k0d [jobs] [corpus_glob]   K0-d equivalence      (byte-level, must PASS)
#   capture_gates.sh k1  [jobs] [corpus_glob]   K1 functional equivalence (decisions, must PASS)
#   capture_gates.sh ydev [jobs] [corpus_glob]  glue #2: device-written y vs host-staged y
#   capture_gates.sh k0dm [jobs] [corpus_glob]  K0-d merged: device-built vs host-built A/R_hp
#   capture_gates.sh combos                     flag-combination matrix (SINR/CRC, must PASS)
#
# The corpus is a set of <name>_ce.txt baselines (the capture-info sidecars; the replay tool derives
# everything else from the name). Each capture is replayed with ul_chain_replay and the published
# outputs are compared.
#
# WARNING the replay tool is NOT reliable under heavy parallelism: at 10 shards it intermittently
# produces WRONG (not merely missing) results - measured, 39 of 980 captures came out different
# between two runs of the SAME configuration, and every one of them was clean when re-run serially.
# A mismatch is therefore ALWAYS re-checked serially before it is reported (see RETRY below), and a
# mismatch that survives the re-check is a real finding. Do not read a bare mismatch out of a
# parallel run without that re-check. NOTE the tool's --out is a filename PREFIX, not a directory: it writes
# <out>_<slot>_<rnti>{,.bin,_ce.txt,_llr.bin,_h.bin}.
#
# A corpus is a set of capture PREFIXES, discovered through their <prefix>_ce.txt (the one the gNB
# writes with OCUDU_UL_DUMP; the replay tool only needs <prefix>.txt and <prefix>.bin). When the
# recorded corpus is gone - /tmp does not survive a reboot, and it did not - make_synthetic_capture.py
# in this directory writes a valid one. Its random grid is enough for every MODE here, because each of
# them compares the device against the host ON THE SAME INPUT: it is the geometry and the plumbing that
# are under test, not the channel. The one exception is combos, which pins the known SINR/CRC of three
# RECORDED receptions; that mode needs the real corpus.
#
# ---- k0d: is the DEVICE-BUILT correlation matrix a drop-in for the host's? -------------------
# Both routes invert on the HOST (OCUDU_CE_GPU_INVERT=0), so the only difference under test is WHO
# BUILT A and R_hp:
#     route A  OCUDU_CE_GPU_INVERT=0                     device build (K0-d) + host inversion
#     route B  OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0 host build         + host inversion
# Every published file must come out BYTE-IDENTICAL: the correlation kernels reproduce the host's
# float expressions bit for bit, which is what makes the device build consumable at all. A 1-ulp
# difference is amplified by cond_2(A) ~ 2e4 into ~1% of W and h, so a byte mismatch here means the
# kernel arithmetic drifted or the staging write is wrong - not "rounding".
#
# WARNING this gate is only meaningful while BOTH routes consume what they built. It passed
# vacuously for a whole round (S-7f-4c): the host staging used to overwrite the device build on
# every hop, so it compared host against host and never saw that R_hp was written a ninth full.
#
# ---- k1: does the DEVICE inversion change any DECODING DECISION? -----------------------------
#     route A  default (device inversion, K1)
#     route B  OCUDU_CE_CPU_INVERT=1 (host Gauss-Jordan)
# The LLR BYTES are expected to differ - the two inverses round differently and that is accepted
# (it is the accuracy the device float32 kernel can reach) - so the gate is the DECODING OUTCOME
# (crc, tbs, iterations class) plus the SINR delta distribution. A CRC flip is a failure; the
# byte-identical LLR count is reported as information, not as a criterion.
#
# ---- ydev: who writes the engine's pilot vectors y (glue #2, S-7f-5u)? ------------------------
#     route A  default          the DEVICE scatters K0-a's pilots into the y slots, inside the
#                               weights' own command buffer
#     route B  OCUDU_CE_DEV_Y=0 the HOST stages them (device -> pilots_lse_view -> y), i.e. the
#                               behaviour before glue #2
# Every published file must come out BYTE-IDENTICAL. The kernel only re-indexes the pilots K0-a
# already produced and applies the same inv_beta product the host applies, so a byte difference
# means the re-indexing is wrong (block/pilot mapping, the system offset, the pad rows, or the
# merged tail group's zeroed blocks) - not "rounding". This is the decisive gate for the step.
#
# It is a VACUITY-CHECKED gate: route A's [metal_stats] line must report device_y_writes > 0 and
# route B's must report 0. Without that, a gate like this compares the host against the host and
# passes - which is exactly how the k0d gate once passed for a whole round (S-7f-4c).
#
# ---- k0dm: the MERGED batch's standard group, built on the device (S-7f-5v) -------------------
#     route A  default              the DEVICE builds the standard group's A/R_hp as a prefix of the
#                                   engine's own command buffer (the edge group stays host-built)
#     route B  OCUDU_CE_CORR_DEV=0  the HOST builds them, as before
# Both routes invert on the DEVICE, so the only difference under test is WHO BUILT those matrices,
# and every published file must be BYTE-IDENTICAL for the same reason k0d demands it: the correlation
# kernels reproduce the host's float expressions exactly, and a 1-ulp difference is amplified by
# cond_2(A) ~ 2e4 into ~1% of W and h.
#
# This is the gate the earlier, failed attempt at a merged device build did not have. It is
# VACUITY-CHECKED on both sides: route A must report device_corr_builds > 0 and route B must report 0.
# Without that, a capture whose hops all skipped the prefix would compare host against host - which is
# exactly how k0d passed for a whole round (S-7f-4c) while the host staging overwrote the device build.
#
# ---- sig2: does the DEVICE-computed noise variance reproduce the host's (S-7f-5w)? -------------
#     route A  default                  the hop's noise variance is computed in K0-a's command buffer
#     route B  OCUDU_CE_DEV_SIGMA2=0    the host keeps estimate_sigma2(), as before
# The moved quantity is one float per hop, and it reaches the output twice: channel_statistics turns
# it into the SINR (and the RSRP/EPRE the _ce.txt records), and the equalizer adds it to the diagonal
# of A as a ridge. So this gate reads BOTH:
#   - the staged _ce.txt scalars, noise_variance per port, with a RELATIVE allowance of 1e-4. The
#     device sums the same expression in a different order, so this is a TOLERANCE, not the byte
#     equality k0d/k0dm demand: measured 1.2e-07 on the L1 harness, i.e. the allowance sits two
#     orders above the measurement and four below anything that could move a decision (§48.110 - the
#     amplification from sigma2 to the output is ~1, so a tolerance is the honest criterion here);
#   - the DECISION (tbs + crc), which must be identical, with |dSINR| reported as information only.
# VACUITY-CHECKED on both sides like k0dm: route A must report device_sigma2 > 0 and route B 0, or
# this would be the host compared against itself (the k0d lesson, S-7f-4c).
#
# ---- combos: the flag matrix ------------------------------------------------------------------
# OCUDU_CE_GPU_INVERT x OCUDU_CE_CORR_DEV x OCUDU_CE_SPLIT_TAIL over the three reference captures.
# This pins the semantics of the escape hatches, which changed in S-7f-4f and are easy to get wrong:
# OCUDU_CE_GPU_INVERT used to mean "on for ANY value, including 0" and now means "on unless 0".
# The non-split combinations must reproduce the known SINR/CRC. The SPLIT_TAIL ones are gated too:
# the tail batch's addressing defect they were excluded for (S-7f-4h) was FIXED in 3417703ba3, and
# S-7f-5u re-checked that split and merged publish byte-identical output on a real capture. If a
# split combination ever fails here again, do not reach for this comment - look at the tail batch.
set -u

MODE=${1:-k0d}
JOBS=${2:-6}
# Corpus: either the dumped <capture>_ce.txt files of an earlier round, or the captures themselves
# (<name>.bin, written by make_synthetic_capture.py) - a reborn corpus after /tmp was lost has the
# latter. Both name a capture PREFIX, which is what the tool takes.
GLOB=${3:-/tmp/iq1_*_ce.txt /tmp/iq2_*_ce.txt}
case "$GLOB" in
  *.bin*) STRIP='s/\.bin$//' ;;
  *)      STRIP='s/_ce\.txt$//' ;;
esac
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../.." && pwd)
BIN=$REPO/build/lib/phy/upper/channel_processors/metal/ul_chain_replay

case "$MODE" in k0d|k1|ydev|k0dm|sig2|ratdev|combos) ;; *) echo "usage: $0 <k0d|k1|ydev|k0dm|sig2|ratdev|combos> [jobs] [corpus_glob]"; exit 2;; esac
[ -x "$BIN" ] || { echo "no replay tool at $BIN - build the ul_chain_replay target first"; exit 2; }

mapfile -t CAPS < <(ls $GLOB 2>/dev/null | sed -E "$STRIP" | sort -u)
[ "${#CAPS[@]}" -gt 0 ] || { echo "no captures matched: $GLOB"; exit 2; }

WORK=$(mktemp -d /tmp/capgates.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

if [ "$MODE" = combos ]; then
  # The invariant this mode gates needs no specific recording: on the SAME capture, EVERY combination
  # of the flag matrix must reproduce the DEFAULT combination's decision (crc identical, |dSINR| within
  # TOL - the device inversion rounds differently, <=0.08 dB). So the reference is taken from the corpus
  # under test: the first three captures, baselined by combination 1 below. This is what makes the mode
  # usable on a re-recorded corpus, which is the normal case after a reboot (/tmp does not survive one).
  #
  # The three receptions every earlier round was gated against are kept as an OPTIONAL absolute anchor
  # (they pin the host's cross-session behaviour, which a fresh corpus cannot). When they are not in the
  # corpus the mode SAYS SO - an unchecked anchor must not read as a checked one.
  PINNED="iq2_1009_17923 iq1_10049_17921 iq2_10044_17923"
  PINNED_SINR="23.97 6.24 31.78"
  PINNED_CRC="KO OK KO"
  TOL=0.2
  # Defined here as well: this block runs before the helpers further down are declared.
  numeric() { awk -v v="$1" 'BEGIN{exit !(v ~ /^-?[0-9.]+$/)}'; }
  fails=0
  NREF=$(( ${#CAPS[@]} < 3 ? ${#CAPS[@]} : 3 ))
  REFS=("${CAPS[@]:0:$NREF}")
  hdr=""; for c in "${REFS[@]}"; do hdr="$hdr | $(printf '%-14s' "$(basename "$c" | cut -c1-14)")"; done
  printf '%-34s%s\n' "combination" "$hdr"
  run_one() {  # envs capture -> "sinr crc"
    local out
    out=$(env $1 "$BIN" "$2" --metal --out "$WORK/cb" 2>/dev/null | grep -o "crc=[A-Z]* .*sinr=[-0-9.a-z]* dB")
    rm -f "$WORK"/cb_* 2>/dev/null
    if [ -z "$out" ]; then echo "CRASH -"; return; fi
    echo "$(sed -E 's/.*sinr=([-0-9.a-z]+) dB.*/\1/' <<<"$out") $(sed -E 's/.*crc=([A-Z]+).*/\1/' <<<"$out")"
  }
  # Baseline: the default combination, on this corpus.
  base=""
  for c in "${REFS[@]}"; do base="$base $(run_one "" "$c")"; done
  set -- $base
  printf '%-34s' "baseline (default, this corpus)"
  for (( i = 0; i < NREF; i++ )); do printf ' | %-14s' "$(printf '%-7s %-6s' "${1:-?}" "${2:-?}")"; shift 2; done
  echo
  run_combo() {  # label env gate(yes/no)
    local label=$1 envs=$2 gate=$3 line="" bad=0 got sinr crc exp_s exp_c k=0
    for c in "${REFS[@]}"; do
      got=$(run_one "$envs" "$c"); sinr=${got% *}; crc=${got#* }
      line="$line | $(printf '%-7s %-6s' "$sinr" "$crc")"
      if [ "$gate" = yes ]; then
        # The baseline of THIS capture (same order as $base above).
        set -- $base
        exp_s=$(eval echo \${$((2 * k + 1))}); exp_c=$(eval echo \${$((2 * k + 2))})
        if [ "$crc" != "$exp_c" ] || ! numeric "$sinr" ||
           ! awk -v a="$sinr" -v b="$exp_s" -v t="$TOL" 'BEGIN{exit !((a-b<=t)&&(b-a<=t))}'; then
          bad=1
        fi
      fi
      k=$(( k + 1 ))
    done
    printf '%-34s%s' "$label" "$line"
    if [ "$gate" = yes ]; then
      if [ $bad -eq 1 ]; then echo "  <-- FAIL"; fails=$(( fails + 1 )); else echo "  ok"; fi
    else
      echo "  (not gated: known-bad opt-in, see S-7f-4h)"
    fi
  }
  run_combo "1  default (device K1 + dev corr)" "" yes
  run_combo "2  GPU_INVERT=0" "OCUDU_CE_GPU_INVERT=0" yes
  run_combo "3  CPU_INVERT=1" "OCUDU_CE_CPU_INVERT=1" yes
  run_combo "4  GPU_INVERT=0 CORR_DEV=0" "OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0" yes
  run_combo "5  CORR_DEV=0" "OCUDU_CE_CORR_DEV=0" yes
  run_combo "6  SPLIT_TAIL=1" "OCUDU_CE_SPLIT_TAIL=1" yes
  run_combo "7  SPLIT_TAIL=1 GPU_INVERT=0" "OCUDU_CE_SPLIT_TAIL=1 OCUDU_CE_GPU_INVERT=0" yes
  run_combo "8  SPLIT_TAIL=1 CORR_DEV=0" "OCUDU_CE_SPLIT_TAIL=1 OCUDU_CE_CORR_DEV=0" yes
  run_combo "9  SPLIT_TAIL=1 GPU_INVERT=0 CORR_DEV=0" "OCUDU_CE_SPLIT_TAIL=1 OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0" yes
  run_combo "10 GPU_INVERT=0 CPU_INVERT=1" "OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CPU_INVERT=1" yes
  echo
  # The optional absolute anchor: the three recorded receptions every earlier round was gated against.
  # It pins the HOST's behaviour across sessions, which the self-baseline above cannot (it would happily
  # accept a corpus-wide regression as long as every combination agreed with itself). Checked when the
  # recordings are there, and REPORTED AS UNCHECKED when they are not - never silently skipped.
  set -- $PINNED_SINR; pin_s=("$@"); set -- $PINNED_CRC; pin_c=("$@")
  n_pin=0; n_absent=0; i=0; pin_bad=0
  for c in $PINNED; do
    if [ -r "/tmp/$c.txt" ]; then
      got=$(run_one "" "/tmp/$c"); sinr=${got% *}; crc=${got#* }
      [ "$crc" = "${pin_c[$i]}" ] && numeric "$sinr" &&
        awk -v a="$sinr" -v b="${pin_s[$i]}" -v t="$TOL" 'BEGIN{exit !((a-b<=t)&&(b-a<=t))}' ||
        pin_bad=$(( pin_bad + 1 ))
      echo "pinned anchor $c: $sinr ${crc} (expected ${pin_s[$i]} ${pin_c[$i]})"
      n_pin=$(( n_pin + 1 ))
    else
      n_absent=$(( n_absent + 1 ))
    fi
    i=$(( i + 1 ))
  done
  if [ $n_absent -ne 0 ]; then
    echo "pinned anchors absent from this corpus: $n_absent/3 - the cross-session absolute anchor is NOT checked"
  fi
  if [ $pin_bad -ne 0 ]; then echo "combos: $pin_bad pinned anchor(s) FAILED"; fails=$(( fails + pin_bad )); fi
  if [ $fails -ne 0 ]; then echo "combos: $fails check(s) FAILED (combination and/or pinned anchor)"; exit 1; fi
  echo "combos: PASS ($n_pin/3 pinned anchors checked; self-baselined on ${#REFS[@]} capture(s))"
  exit 0
fi


# The decoding decision of one replay run: tbs + crc + SINR, from the result line on stdout. The
# degenerate captures (tbs=88, no usable grant) report `sinr=inf`, which is a valid outcome - the
# SINR field is matched loosely and only used for the delta when it is actually a number.
decision() { sed -nE 's/.*tbs=([0-9]+) slot=[0-9]+ rnti=[0-9]+ [A-Z0-9]+: crc=([A-Z]+).*sinr=([-0-9.a-z]+) dB.*/\1 \2 \3/p' <<<"$1" | head -1; }
numeric() { awk -v v="$1" 'BEGIN{exit !(v ~ /^-?[0-9.]+$/)}'; }
# Relative difference between the noise_variance scalars of two staged captures (both routes captured
# the same reception, port by port). Reads the file, not stdout: a parallel-run corruption keeps the
# CRC but reports a wild SINR, and this number must not inherit that.
sigma2_rel() {
  local fa fb
  fa=$(ls "$1"_*_ce.txt 2>/dev/null | head -1)
  fb=$(ls "$2"_*_ce.txt 2>/dev/null | head -1)
  if [ -z "$fa" ] || [ -z "$fb" ]; then echo "nan"; return; fi
  awk 'NR==FNR { split($2, x, "="); a[FNR] = x[2] + 0; next }
       { split($2, y, "="); v = a[FNR]; w = y[2] + 0; d = w - v; if (d < 0) d = -d;
         r = (v != 0) ? d / ((v < 0) ? -v : v) : d; if (r > m) m = r }
       END { printf "%.3e", m + 0 }' "$fa" "$fb"
}
# The allowance of the sig2 mode (see its section in the header).
SIG2_TOL=1e-04

# The two env sets of the BYTE-COMPARISON modes (k0d, ydev, k0dm): route A is the one under test,
# route B the reference it has to reproduce exactly. See the header for what each pair isolates.
# VACUOUS_COUNTER names the [metal_stats] counter that proves route A took the device path at all
# (and that route B's knob turned it off); without it the comparison can be host against host.
case "$MODE" in
  k0d)  ENV_A="OCUDU_CE_GPU_INVERT=0"; ENV_B="OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0"
        # No hard assertion here: route A builds on the device only for hops WITHOUT a remainder, so a
        # capture whose hops are all merged is legitimately host-against-host. The coverage is
        # REPORTED instead (see the summary), so a vacuous corpus cannot hide.
        VACUOUS_COUNTER=""; REPORT_COUNTER="device_corr_builds";;
  ydev) ENV_A=""; ENV_B="OCUDU_CE_DEV_Y=0"
        VACUOUS_COUNTER="device_y_writes"; REPORT_COUNTER="";;
  k0dm) ENV_A=""; ENV_B="OCUDU_CE_CORR_DEV=0"
        VACUOUS_COUNTER="device_corr_builds"; REPORT_COUNTER="";;
  sig2) ENV_A="OCUDU_CE_SIGMA2_CHECK=1"; ENV_B="OCUDU_CE_DEV_SIGMA2=0 OCUDU_CE_SIGMA2_CHECK=1"
        VACUOUS_COUNTER="device_sigma2"; REPORT_COUNTER="";;
  # S-7g-20: route A loads A's diagonal from the ratio the extraction's own command buffer computed;
  # route B computes the same ratio on the host. The two must publish byte-identical dumps - that IS
  # the property, and it is not self-evident: the quotient is bit-exact only while
  # ocudu_mmse_pilots_power.metal is compiled with -fno-fast-math (without it, 28.4% of 2^20 pairs
  # round differently and LLR decisions flip on every capture), and the kernel must read the buffer
  # element the host means (reading past it loaded A with no noise at all). This mode turns both from
  # "argued" into "gated", so it must keep running with the strict flag in place.
  ratdev) ENV_A="OCUDU_CE_K0A_RATIO_DEV=1 OCUDU_CE_K0A_RATIO_CHECK=1"
          ENV_B="OCUDU_CE_K0A_RATIO_DEV=0"
          VACUOUS_COUNTER=""; REPORT_COUNTER="k0a_ratio";;
  *)    ENV_A=""; ENV_B=""; VACUOUS_COUNTER=""; REPORT_COUNTER="";;
esac

run_shard() {
  local id=$1 total=0 same=0 bytesame=0 maxdelta=0 bad="" delta nbytes=0
  local i c base out_d out_h stdout_d stdout_h ok f rel retried=0
  for (( i = id; i < ${#CAPS[@]}; i += JOBS )); do
    c=${CAPS[$i]}; base=$(basename "$c")
    out_d=$WORK/${id}_${base}_a; out_h=$WORK/${id}_${base}_b
    total=$(( total + 1 ))
    if [ "$MODE" = k0d ] || [ "$MODE" = ydev ] || [ "$MODE" = k0dm ] || [ "$MODE" = ratdev ]; then
      # stderr of both routes carries the [metal_stats] line (printed at exit): the vacuity check
      # reads the counter that says whether the device path engaged.
      env $ENV_A "$BIN" "$c" --metal --out "$out_d" >/dev/null 2>"$WORK/${id}_${base}.a.err" ||
        { bad="$bad $base(A)"; continue; }
      env $ENV_B "$BIN" "$c" --metal --out "$out_h" >/dev/null 2>"$WORK/${id}_${base}.b.err" ||
        { bad="$bad $base(B)"; continue; }
      if [ "$REPORT_COUNTER" = "k0a_ratio" ]; then
        # The ratio probe is a per-hop diagnostic LINE, not a [metal_stats] counter: route A must have
        # printed at least one, or it never took the device ratio and this would be host against host -
        # a gate that passes while proving nothing.
        if ! grep -q "\[k0a_ratio\]" "$WORK/${id}_${base}.a.err"; then
          echo "$base" >> "$WORK/vac_$id"
          continue
        fi
      fi
      if [ -n "$VACUOUS_COUNTER" ]; then
        local va vb
        va=$(grep -o "$VACUOUS_COUNTER=[0-9]*" "$WORK/${id}_${base}.a.err" | head -1 | cut -d= -f2)
        vb=$(grep -o "$VACUOUS_COUNTER=[0-9]*" "$WORK/${id}_${base}.b.err" | head -1 | cut -d= -f2)
        if [ -z "$va" ] || [ "$va" -eq 0 ]; then
          # Route A never took the device path on this capture, so the comparison below would be host
          # against host. That is a property of the capture's geometry (a hop with no block the device
          # builds), not a defect - it is counted and reported, and the summary refuses to PASS if
          # NOTHING engaged (a device path that silently went dead must not read as a green gate).
          echo "$base" >> "$WORK/vac_$id"
          continue
        fi
        if [ -n "$vb" ] && [ "$vb" -ne 0 ]; then
          # Route B's knob did not turn the device path off: the two routes are the same one.
          bad="$bad $base(knob-ineffective:$VACUOUS_COUNTER=$vb)"; continue
        fi
      fi
      if [ -n "$REPORT_COUNTER" ]; then
        # How many captures route A actually ran the device path on: reported, not gated (see the
        # mode table above).
        local rv
        rv=$(grep -o "$REPORT_COUNTER=[0-9]*" "$WORK/${id}_${base}.a.err" | head -1 | cut -d= -f2)
        [ -n "$rv" ] && [ "$rv" -gt 0 ] && echo 1 >> "$WORK/rep_$id"
      fi
      ok=1
      for f in "$out_d"_*; do
        rel=${f#"$out_d"}
        cmp -s "$f" "$out_h$rel" || ok=0
      done
      if [ $ok -eq 0 ]; then
        # Re-checked AFTER the parallel phase, with nothing else running (see the second pass below).
        # Re-running it here as well - which is what the earlier revision did - does not help: the
        # other shards are still busy, so the tool's parallel-run flakiness is still in play. It was
        # measured producing false mismatches on 5 captures across three gate runs, every one of them
        # byte-identical when re-run alone.
        retried=$(( retried + 1 ))
        nbytes=$(( nbytes + 1 ))
        echo "$c" >> "$WORK/bad_$id"
      fi
      # A byte mismatch is NOT counted here: the serial second pass below decides whether it is real,
      # and adding it to $bad as well would report every parallel-phase flag as a finding (which is
      # what an earlier revision of this rework did - it printed 46 mismatches that the second pass
      # had just cleared).
      if [ $ok -eq 1 ]; then same=$(( same + 1 )); fi
    elif [ "$MODE" = sig2 ]; then
      # Both routes write the same staged capture: what differs is WHO computed the hop's noise
      # variance (the engine's command buffer in A, estimate_sigma2() in B).
      env $ENV_A "$BIN" "$c" --metal --out "$out_d" >"$WORK/${id}_${base}.a.out" 2>"$WORK/${id}_${base}.a.err" ||
        { bad="$bad $base(A)"; continue; }
      env $ENV_B "$BIN" "$c" --metal --out "$out_h" >"$WORK/${id}_${base}.b.out" 2>"$WORK/${id}_${base}.b.err" ||
        { bad="$bad $base(B)"; continue; }
      local va vb
      va=$(grep -o "device_sigma2=[0-9]*" "$WORK/${id}_${base}.a.err" | head -1 | cut -d= -f2)
      vb=$(grep -o "device_sigma2=[0-9]*" "$WORK/${id}_${base}.b.err" | head -1 | cut -d= -f2)
      if [ -z "$va" ] || [ "$va" -eq 0 ]; then
        # This capture's hops never took the device path: comparing would be host against host.
        echo "$base" >> "$WORK/vac_$id"
        continue
      fi
      if [ -n "$vb" ] && [ "$vb" -ne 0 ]; then
        bad="$bad $base(knob-ineffective:device_sigma2=$vb)"; continue
      fi
      local da dh rel raw dec_bad
      da=$(decision "$(grep -m1 'crc=' "$WORK/${id}_${base}.a.out")")
      dh=$(decision "$(grep -m1 'crc=' "$WORK/${id}_${base}.b.out")")
      # The GATED quantity is the stage's own output: [sigma2_check] reports the device scalar against
      # the reference implementation INSIDE the same process, per hop, with nothing downstream of it.
      # The noise_variance in the staged _ce.txt is DERIVED from it by channel_statistics and amplifies
      # the difference (measured on air: raw 2.8e-07 -> derived 9.86e-05, ~350x, i.e. 1.4% below the old
      # allowance). Gating the derived value would fail the gate for an amplification that has nothing to
      # do with this stage, so it is reported instead.
      raw=$(grep -o 'rel=[-0-9.e+]*' "$WORK/${id}_${base}.a.err" | sed 's/rel=//' |
            awk 'BEGIN{m=0} {v=$1+0; if(v<0)v=-v; if(v>m)m=v} END{printf "%.3e", m}')
      rel=$(sigma2_rel "$out_d" "$out_h")
      if [ -z "$da" ] || [ -z "$dh" ] || [ "$rel" = nan ]; then bad="$bad $base(no-result)"; continue; fi
      echo "$raw" >> "$WORK/sig2raw_$id"
      echo "$rel" >> "$WORK/sig2rel_$id"
      dec_bad=0
      [ "${da% *}" = "${dh% *}" ] || dec_bad=1
      # Both kinds of candidate - a scalar beyond the allowance and a decision that moved - are only
      # CANDIDATES here: the replay tool produces wrong results under parallelism (it flipped the CRC of
      # two of these 237 captures while six instances were running, and both were clean re-run alone).
      # The post-parallel pass below decides.
      if [ "$dec_bad" -eq 1 ] || awk -v r="$raw" -v t="$SIG2_TOL" 'BEGIN{exit !(r > t)}'; then
        echo "$c" >> "$WORK/recheck_$id"
      fi
      if [ "$dec_bad" -eq 0 ]; then same=$(( same + 1 )); fi
      if numeric "${da##* }" && numeric "${dh##* }"; then
        delta=$(awk -v a="${da##* }" -v b="${dh##* }" 'BEGIN{d=a-b; if(d<0)d=-d; printf "%.2f", d}')
        maxdelta=$(awk -v m="$maxdelta" -v d="$delta" 'BEGIN{print (d>m)?d:m}')
      fi
    else
      stdout_d=$("$BIN" "$c" --metal --out "$out_d" 2>/dev/null | grep -m1 "crc=") || true
      stdout_h=$(OCUDU_CE_CPU_INVERT=1 "$BIN" "$c" --metal --out "$out_h" 2>/dev/null | grep -m1 "crc=") || true
      # The replay tool has a known intermittent failure (rx_buffer_impl::get_codeblock_data_bits,
      # ~1% of runs, both routes alike, unrelated to this work): retry once before calling it a
      # mismatch, so the gate reports real differences only.
      if [ -z "$stdout_d" ] || [ -z "$stdout_h" ]; then
        retried=$(( retried + 1 ))
        stdout_d=$("$BIN" "$c" --metal --out "$out_d" 2>/dev/null | grep -m1 "crc=") || true
        stdout_h=$(OCUDU_CE_CPU_INVERT=1 "$BIN" "$c" --metal --out "$out_h" 2>/dev/null | grep -m1 "crc=") || true
      fi
      local da dh
      da=$(decision "$stdout_d"); dh=$(decision "$stdout_h")
      if [ -z "$da" ] || [ -z "$dh" ]; then bad="$bad $base(no-result)"; continue; fi
      if [ "${da% *}" != "${dh% *}" ]; then
        # Serial re-check before reporting (see the parallelism warning above).
        retried=$(( retried + 1 ))
        rm -f "$out_d"_* "$out_h"_*
        stdout_d=$("$BIN" "$c" --metal --out "$out_d" 2>/dev/null | grep -m1 "crc=") || true
        stdout_h=$(OCUDU_CE_CPU_INVERT=1 "$BIN" "$c" --metal --out "$out_h" 2>/dev/null | grep -m1 "crc=") || true
        da=$(decision "$stdout_d"); dh=$(decision "$stdout_h")
      fi
      if [ -z "$da" ] || [ -z "$dh" ]; then bad="$bad $base(no-result)"; continue; fi
      if [ "${da% *}" = "${dh% *}" ]; then same=$(( same + 1 )); else bad="$bad $base"; fi
      # Informational: how often the two inversions round to the same LLR bytes.
      for f in "$out_d"_*_llr.bin; do
        [ -e "$f" ] || continue
        rel=${f#"$out_d"}
        cmp -s "$f" "$out_h$rel" && bytesame=$(( bytesame + 1 ))
      done
      if numeric "${da##* }" && numeric "${dh##* }"; then
        delta=$(awk -v a="${da##* }" -v b="${dh##* }" 'BEGIN{d=a-b; if(d<0)d=-d; printf "%.2f", d}')
        maxdelta=$(awk -v m="$maxdelta" -v d="$delta" 'BEGIN{print (d>m)?d:m}')
      fi
    fi
    rm -f "$out_d"_* "$out_h"_*
  done
  # nbytes counts the BYTE mismatches of the comparison modes: their capture paths are in bad_$id and
  # the serial second pass below re-runs exactly those. Everything else (a route that crashed, a
  # missing result, a knob that did not engage) is already final and travels in the bad-name file.
  # The names go through a FILE, not through a field of res_$id: read(1) eats the leading whitespace of
  # its last field, so a shard whose findings started after a clean one lost the separator and its names
  # ran into the previous ones. An empty list must also stay empty - a gate that answers "MISMATCH" with
  # nothing after it is worse than no gate at all.
  printf '%s\n' $bad > "$WORK/badnames_$id"
  echo "$total $same $bytesame $maxdelta $retried $nbytes" > "$WORK/res_$id"
}

for (( j = 0; j < JOBS; j++ )); do run_shard "$j" & done
wait

TOTAL=0; SAME=0; BYTESAME=0; MAXDELTA=0; RETRIED=0; BYTEMIS=0; BAD=""
for (( j = 0; j < JOBS; j++ )); do
  read -r t s b m r nb < "$WORK/res_$j"
  TOTAL=$(( TOTAL + t )); SAME=$(( SAME + s )); BYTESAME=$(( BYTESAME + b )); RETRIED=$(( RETRIED + r ));
  BYTEMIS=$(( BYTEMIS + nb ))
  MAXDELTA=$(awk -v x="$MAXDELTA" -v y="$m" 'BEGIN{print (y>x)?y:x}')
done
BAD=$(grep -h . "$WORK"/badnames_* 2>/dev/null | tr '\n' ' ')

# ---- Serial second pass over the byte mismatches ---------------------------------------------
# Nothing else is running now, so this is the "re-check it serially before believing it" the header
# promises: the replay tool produces WRONG results under heavy parallelism (see the top of this
# file), and only what still differs here is a finding.
VACUOUS=0
for (( j = 0; j < JOBS; j++ )); do
  [ -f "$WORK/vac_$j" ] && VACUOUS=$(( VACUOUS + $(wc -l < "$WORK/vac_$j") ))
done

SURVIVORS=""; RECHECKED=0
if [ "$BYTEMIS" -ne 0 ]; then
  cat "$WORK"/bad_* 2>/dev/null | sort -u > "$WORK/badlist"
  while read -r bc; do
    [ -n "$bc" ] || continue
    RECHECKED=$(( RECHECKED + 1 ))
    rm -f "$WORK/sa"_* "$WORK/sb"_*
    if ! env $ENV_A "$BIN" "$bc" --metal --out "$WORK/sa" >/dev/null 2>&1; then
      SURVIVORS="$SURVIVORS $(basename "$bc")(rerun-A)"; continue
    fi
    if ! env $ENV_B "$BIN" "$bc" --metal --out "$WORK/sb" >/dev/null 2>&1; then
      SURVIVORS="$SURVIVORS $(basename "$bc")(rerun-B)"; continue
    fi
    ok=1
    for f in "$WORK/sa"_*; do rel=${f#"$WORK/sa"}; cmp -s "$f" "$WORK/sb$rel" || ok=0; done
    if [ $ok -eq 1 ]; then
      # The parallel phase counted this one as differing; with an idle GPU it does not. Put it back
      # into the tally, or the summary reads "byte-identical=26 ... MISMATCH:" - which is exactly the
      # self-contradictory line this pass exists to avoid (the sigma2 pass below has done this from
      # the start; the byte-comparison modes did not, and a reader cannot tell a cleared candidate
      # from a live finding).
      SAME=$(( SAME + 1 ))
      BYTEMIS=$(( BYTEMIS - 1 ))
    else
      SURVIVORS="$SURVIVORS $(basename "$bc")"
    fi
    rm -f "$WORK/sa"_* "$WORK/sb"_*
  done < "$WORK/badlist"
  BAD="$BAD$SURVIVORS"
fi

# ---- Serial (nothing else running) re-check of the sigma2 candidates ---------------------------
# Same discipline as the byte mismatches above: neither a moving decision nor a scalar beyond the
# allowance is believed until it survives a run with an idle GPU. On the 237-capture corpus this pass
# cleared every one of the candidates it was given.
SIG2RECHECKED=0; SIG2SURV=0; SIG2RAWMAX=0; SIG2CLEARED=0
if [ "$MODE" = sig2 ]; then
  cat "$WORK"/recheck_* 2>/dev/null | sort -u > "$WORK/rechecklist"
  SIG2RECHECKED=$(wc -l < "$WORK/rechecklist" | tr -d ' ')
  while read -r bc; do
    [ -n "$bc" ] || continue
    rm -f "$WORK/ra"_* "$WORK/rb"_*
    if ! env $ENV_A "$BIN" "$bc" --metal --out "$WORK/ra" >"$WORK/ra.out" 2>"$WORK/ra.err"; then
      BAD="$BAD $(basename "$bc")(rerun-A)"; continue
    fi
    if ! env $ENV_B "$BIN" "$bc" --metal --out "$WORK/rb" >"$WORK/rb.out" 2>"$WORK/rb.err"; then
      BAD="$BAD $(basename "$bc")(rerun-B)"; continue
    fi
    rda=$(decision "$(grep -m1 'crc=' "$WORK/ra.out")")
    rdb=$(decision "$(grep -m1 'crc=' "$WORK/rb.out")")
    rraw=$(grep -o 'rel=[-0-9.e+]*' "$WORK/ra.err" | sed 's/rel=//' |
           awk 'BEGIN{m=0} {v=$1+0; if(v<0)v=-v; if(v>m)m=v} END{printf "%.3e", m}')
    SIG2RAWMAX=$(awk -v m="$SIG2RAWMAX" -v v="$rraw" 'BEGIN{print (v>m)?v:m}')
    if [ -z "$rda" ] || [ -z "$rdb" ]; then
      BAD="$BAD $(basename "$bc")(rerun-no-result)"; continue
    fi
    if [ "${rda% *}" != "${rdb% *}" ]; then
      BAD="$BAD $(basename "$bc")(serially-reproduced-decision:$rda|$rdb)"; SIG2SURV=$(( SIG2SURV + 1 ))
    else
      # The parallel phase had counted this capture as decision-different; it is not. Put it back, or
      # the summary would read "decision-identical=236 ... survivors=0" and look self-contradictory.
      SIG2CLEARED=$(( SIG2CLEARED + 1 ))
    fi
    if awk -v r="$rraw" -v t="$SIG2_TOL" 'BEGIN{exit !(r > t)}'; then
      BAD="$BAD $(basename "$bc")(serially-reproduced-sigma2-rel=$rraw)"; SIG2SURV=$(( SIG2SURV + 1 ))
    fi
    rm -f "$WORK/ra"_* "$WORK/rb"_*
  done < "$WORK/rechecklist"
fi

if [ "$MODE" = k0d ]; then
  DB=0
  for (( j = 0; j < JOBS; j++ )); do
    [ -f "$WORK/rep_$j" ] && DB=$(( DB + $(wc -l < "$WORK/rep_$j") ))
  done
  echo "mode=k0d captures=$TOTAL byte-identical=$SAME retried=$RETRIED device-built-captures=$DB"
elif [ "$MODE" = ydev ]; then
  echo "mode=ydev captures=$TOTAL byte-identical=$SAME vacuous=$VACUOUS rechecked=$RECHECKED retried=$RETRIED"
elif [ "$MODE" = k0dm ]; then
  echo "mode=k0dm captures=$TOTAL byte-identical=$SAME vacuous=$VACUOUS rechecked=$RECHECKED retried=$RETRIED"
elif [ "$MODE" = ratdev ]; then
  echo "mode=ratdev captures=$TOTAL byte-identical=$SAME vacuous=$VACUOUS rechecked=$RECHECKED retried=$RETRIED"
  # The device quotient must equal the host's BIT FOR BIT - the mode's whole point - so any byte
  # difference is a finding, not a tolerance.
  [ "$SAME" -eq "$TOTAL" ] || BAD="$BAD (device and host ratios disagree)"
elif [ "$MODE" = sig2 ]; then
  SIG2REL=0; SIG2RAW=0
  for (( j = 0; j < JOBS; j++ )); do
    [ -f "$WORK/sig2rel_$j" ] && SIG2REL=$(awk -v m="$SIG2REL" '{ if ($1 + 0 > m) m = $1 + 0 } END { printf "%.3e", m }' "$WORK/sig2rel_$j")
    [ -f "$WORK/sig2raw_$j" ] && SIG2RAW=$(awk -v m="$SIG2RAW" '{ if ($1 + 0 > m) m = $1 + 0 } END { printf "%.3e", m }' "$WORK/sig2raw_$j")
  done
  SAME=$(( SAME + SIG2CLEARED ))
  echo "mode=sig2 captures=$TOTAL decision-identical=$SAME vacuous=$VACUOUS retried=$RETRIED serially-rechecked=$SIG2RECHECKED survivors=$SIG2SURV"
  # The GATED number is the scalar the stage itself produces, against the reference implementation in
  # the same process ([sigma2_check]), and it is the one the serial pass re-measured: clean runs gave
  # 2.3e-06 on air, 1.3e-07 on the L1 harness, against an allowance of $SIG2_TOL.
  echo "mode=sig2 max raw sigma2 rel (GATED, serial runs only)=$SIG2RAWMAX"
  echo "mode=sig2 max raw sigma2 rel (all runs, contaminated by parallel-run flakiness)=$SIG2RAW"
  echo "mode=sig2 max noise_variance rel=$SIG2REL (INFORMATIONAL: derived downstream, amplifies ~350x)"
  echo "mode=sig2 max|dSINR|=${MAXDELTA}dB (INFORMATIONAL, contaminated by parallel-run flakiness)"
else
  echo "mode=k1 captures=$TOTAL decision-identical=$SAME llr-byte-identical=$BYTESAME retried=$RETRIED"
  # max|dSINR| is INFORMATIONAL ONLY: a corrupted parallel run keeps its CRC but reports a wild
  # SINR, so this statistic is contaminated and must not be gated (it moved 7.3 -> 48.9 dB across
  # runs of an unchanged configuration). The gate is decision-identical.
  echo "mode=k1 max|dSINR|=${MAXDELTA}dB (informational, contaminated by parallel-run flakiness)"
fi
if [ -n "$BAD" ]; then
  echo "MISMATCH:$BAD"
  exit 1
fi
# A comparison mode that never engaged the device path anywhere compared host against host on every
# capture: that is not a PASS, it is a gate that tested nothing (S-7f-4c).
if [ "$MODE" != k1 ] && [ "$MODE" != k0d ] && [ "$SAME" -eq 0 ]; then
  echo "VACUOUS: $MODE never engaged the device path on any of the $TOTAL captures"
  exit 1
fi
echo "PASS"
