#!/usr/bin/env bash
# Every cross-binary gate of a leg, in one command.
#
# Since S-7g-3 the estimator reduces the pilots' mean power on the device, so the DEVICE route's
# dumps cannot be byte-identical any more (different summation order; see the design doc, 48.164).
# The protocol splits accordingly - one strict net, one bounded net, one CPU-side net:
#
#   1. STRICT  (host LS route): --metal + OCUDU_CE_CPU_LS=1, byte-identical expected (wip/ab_strict.sh).
#      This is the sensitive net for the whole upper-PHY chain, and the route the device-side pilots
#      reduction does not touch.
#   2. BOUNDED (device route): --metal, bounded difference + LLR decision identity (wip/ab_tol.sh).
#   3. CPU-SIDE: the pure CPU chain (wip/ab_cpu.sh). The cpu_gpu mixes belong to net 2: they run the
#      device estimator.
#
# usage: bash run_ab_all.sh <reference-binary> [nof-captures]
set -u
REF=${1:?reference binary, e.g. /tmp/ul_chain_replay_s7f6d_ref}
N=${2:-30}
W=$(dirname "$0")
rc=0
echo "== 1/3 strict (host LS route, byte-identical) =="
bash "$W/ab_strict.sh" "$REF" "$N" OCUDU_CE_CPU_LS=1 --metal || rc=1
echo "== 2/3 bounded (device route) =="
bash "$W/ab_tol.sh" "$REF" "$N" "--metal --metal-cpu-ldpc --metal-cpu-demod" || rc=1
echo "== 3/3 CPU side =="
bash "$W/ab_cpu.sh" "$REF" "$N" || rc=1
echo "AB_ALL_RC=$rc"
exit $rc
