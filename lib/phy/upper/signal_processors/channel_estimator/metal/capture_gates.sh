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
#   capture_gates.sh combos                     flag-combination matrix (SINR/CRC, must PASS)
#
# The corpus is a set of <name>_ce.txt baselines (the capture-info sidecars; the replay tool derives
# everything else from the name). Each capture is replayed with ul_chain_replay and the published
# outputs are compared. NOTE the tool's --out is a filename PREFIX, not a directory: it writes
# <out>_<slot>_<rnti>{,.bin,_ce.txt,_llr.bin,_h.bin}.
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
# ---- combos: the flag matrix ------------------------------------------------------------------
# OCUDU_CE_GPU_INVERT x OCUDU_CE_CORR_DEV x OCUDU_CE_SPLIT_TAIL over the three reference captures.
# This pins the semantics of the escape hatches, which changed in S-7f-4f and are easy to get wrong:
# OCUDU_CE_GPU_INVERT used to mean "on for ANY value, including 0" and now means "on unless 0".
# The non-split combinations must reproduce the known SINR/CRC; the SPLIT_TAIL ones are printed but
# NOT gated, because that form is a KNOWN-BAD opt-in (S-7f-4h: its tail batch is wrong on real
# captures, identically before and after S-7f-4f - the merged form is the default and is correct).
set -u

MODE=${1:-k0d}
JOBS=${2:-6}
GLOB=${3:-/tmp/iq1_*_ce.txt /tmp/iq2_*_ce.txt}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../.." && pwd)
BIN=$REPO/build/lib/phy/upper/channel_processors/metal/ul_chain_replay

case "$MODE" in k0d|k1|combos) ;; *) echo "usage: $0 <k0d|k1|combos> [jobs] [corpus_glob]"; exit 2;; esac
[ -x "$BIN" ] || { echo "no replay tool at $BIN - build the ul_chain_replay target first"; exit 2; }

mapfile -t CAPS < <(ls $GLOB 2>/dev/null | sed -E 's/_ce\.txt$//' | sort -u)
[ "${#CAPS[@]}" -gt 0 ] || { echo "no captures matched: $GLOB"; exit 2; }

WORK=$(mktemp -d /tmp/capgates.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

if [ "$MODE" = combos ]; then
  # name|env|expected "sinr crc" per capture (empty = not gated)
  REF="iq2_1009_17923 iq1_10049_17921 iq2_10044_17923"
  # The device inversion rounds differently, so the tolerance covers its <=0.08 dB.
  TOL=0.2
  # Defined here as well: this block runs before the helpers further down are declared.
  numeric() { awk -v v="$1" 'BEGIN{exit !(v ~ /^-?[0-9.]+$/)}'; }
  fails=0
  printf '%-34s | %-14s | %-14s | %-14s\n' "combination" "iq2_1009" "iq1_10049" "iq2_10044"
  printf '%-34s | %-14s | %-14s | %-14s\n' "expected (host K1)" "23.97 KO" "6.24 OK" "31.78 KO"
  run_combo() {  # label env gate(yes/no)
    local label=$1 envs=$2 gate=$3 line="" bad=0 sinr crc out
    for c in $REF; do
      out=$(env $envs "$BIN" "/tmp/$c" --metal --out "$WORK/cb" 2>/dev/null | grep -o "crc=[A-Z]* .*sinr=[-0-9.a-z]* dB")
      crc=$(sed -E 's/.*crc=([A-Z]+).*/\1/' <<<"$out")
      sinr=$(sed -E 's/.*sinr=([-0-9.a-z]+) dB.*/\1/' <<<"$out")
      [ -z "$sinr" ] && { sinr="CRASH"; crc="-"; }
      line="$line | $(printf '%-7s %-6s' "$sinr" "$crc")"
      if [ "$gate" = yes ]; then
        case "$c" in
          iq2_1009_17923) exp_s=23.97; exp_c=KO;;
          iq1_10049_17921) exp_s=6.24; exp_c=OK;;
          iq2_10044_17923) exp_s=31.78; exp_c=KO;;
        esac
        if [ "$crc" != "$exp_c" ] || ! numeric "$sinr" ||
           ! awk -v a="$sinr" -v b="$exp_s" -v t="$TOL" 'BEGIN{exit !((a-b<=t)&&(b-a<=t))}'; then
          bad=1
        fi
      fi
      rm -f "$WORK"/cb_* 2>/dev/null
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
  run_combo "6  SPLIT_TAIL=1" "OCUDU_CE_SPLIT_TAIL=1" no
  run_combo "7  SPLIT_TAIL=1 GPU_INVERT=0" "OCUDU_CE_SPLIT_TAIL=1 OCUDU_CE_GPU_INVERT=0" no
  run_combo "8  SPLIT_TAIL=1 CORR_DEV=0" "OCUDU_CE_SPLIT_TAIL=1 OCUDU_CE_CORR_DEV=0" no
  run_combo "9  SPLIT_TAIL=1 GPU_INVERT=0 CORR_DEV=0" "OCUDU_CE_SPLIT_TAIL=1 OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0" no
  run_combo "10 GPU_INVERT=0 CPU_INVERT=1" "OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CPU_INVERT=1" yes
  echo
  if [ $fails -ne 0 ]; then echo "combos: $fails gated combination(s) FAILED"; exit 1; fi
  echo "combos: PASS"
  exit 0
fi


# The decoding decision of one replay run: tbs + crc + SINR, from the result line on stdout. The
# degenerate captures (tbs=88, no usable grant) report `sinr=inf`, which is a valid outcome - the
# SINR field is matched loosely and only used for the delta when it is actually a number.
decision() { sed -nE 's/.*tbs=([0-9]+) slot=[0-9]+ rnti=[0-9]+ [A-Z0-9]+: crc=([A-Z]+).*sinr=([-0-9.a-z]+) dB.*/\1 \2 \3/p' <<<"$1" | head -1; }
numeric() { awk -v v="$1" 'BEGIN{exit !(v ~ /^-?[0-9.]+$/)}'; }

run_shard() {
  local id=$1 total=0 same=0 bytesame=0 maxdelta=0 bad="" delta
  local i c base out_d out_h stdout_d stdout_h ok f rel retried=0
  for (( i = id; i < ${#CAPS[@]}; i += JOBS )); do
    c=${CAPS[$i]}; base=$(basename "$c")
    out_d=$WORK/${id}_${base}_a; out_h=$WORK/${id}_${base}_b
    total=$(( total + 1 ))
    if [ "$MODE" = k0d ]; then
      OCUDU_CE_GPU_INVERT=0 "$BIN" "$c" --metal --out "$out_d" >/dev/null 2>&1 || { bad="$bad $base(A)"; continue; }
      OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0 "$BIN" "$c" --metal --out "$out_h" >/dev/null 2>&1 ||
        { bad="$bad $base(B)"; continue; }
      ok=1
      for f in "$out_d"_*; do
        rel=${f#"$out_d"}
        cmp -s "$f" "$out_h$rel" || ok=0
      done
      if [ $ok -eq 1 ]; then same=$(( same + 1 )); else bad="$bad $base"; fi
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
  echo "$total $same $bytesame $maxdelta $retried$bad" > "$WORK/res_$id"
}

for (( j = 0; j < JOBS; j++ )); do run_shard "$j" & done
wait

TOTAL=0; SAME=0; BYTESAME=0; MAXDELTA=0; RETRIED=0; BAD=""
for (( j = 0; j < JOBS; j++ )); do
  read -r t s b m r rest < "$WORK/res_$j"
  TOTAL=$(( TOTAL + t )); SAME=$(( SAME + s )); BYTESAME=$(( BYTESAME + b )); RETRIED=$(( RETRIED + r )); BAD="$BAD$rest"
  MAXDELTA=$(awk -v x="$MAXDELTA" -v y="$m" 'BEGIN{print (y>x)?y:x}')
done

if [ "$MODE" = k0d ]; then
  echo "mode=k0d captures=$TOTAL byte-identical=$SAME"
else
  echo "mode=k1 captures=$TOTAL decision-identical=$SAME llr-byte-identical=$BYTESAME max|dSINR|=${MAXDELTA}dB retried=$RETRIED"
fi
if [ -n "$BAD" ]; then
  echo "MISMATCH:$BAD"
  exit 1
fi
echo "PASS"
