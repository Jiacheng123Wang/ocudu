#!/usr/bin/env bash
# Both utility flights, in the order that makes them cheap to read: the receiver contrast first, then the calibration.
#
# WHY A WRAPPER AND NOT TWO COMMANDS. The only flight failure that has actually cost phone time twice is a WRONG
# CONFIGURATION -- once a `-b` cap that saturated every leg, once the MCS-pinned collect cell used for a question it
# cannot answer. Two hand-typed commands are two chances to pick the wrong cell; this one names each cell for its own
# phase and refuses the combinations that would waste a leg (inside fly_utility_abba.sh).
#
# WHY THERE IS A PAUSE IN THE MIDDLE. Each phase starts its own gNB, so the UE re-attaches between them and takes a NEW
# IP address (the flights have handed out 10.45.0.27 through .39 over a day). The second phase cannot use the first
# phase's address, and a leg that measures a closed loop with no peer is a leg that measured nothing.
#
# Usage:
#   UE_IP=<phone ip> bash fly_both.sh [batch-prefix]      e.g. UE_IP=10.45.0.31 bash fly_both.sh r9
# The prefix defaults to `r9`; the two batches become <prefix>r (receiver contrast) and <prefix>c (calibration/R6).
set -u
PREFIX=${1:-r9}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=/Users/jiachengwang/dev/ocudu
COLLECT=$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml
DELIVERY=$ROOT/configs/gnb_rf_b200_tdd_n78_20mhz.yml

die() { echo "STOPPING: $*"; exit 2; }
[ -n "${UE_IP:-}" ] || die "pass the phone's address, e.g. UE_IP=10.45.0.31 bash fly_both.sh $PREFIX"

echo "==================================================================="
echo " phase 1 of 2: RECEIVER CONTRAST   batch ${PREFIX}r"
echo "   cell: $COLLECT"
echo "   arms: a (AI receiver) vs c (classical chain), mirrored: a c c a"
echo "   question: at the same code rate, does the AI receiver carry more traffic?"
echo "   reading: 'AI arm is BETTER by X%' / 'NO MEASURABLE DIFFERENCE' / 'AI arm is NOT better'"
echo "==================================================================="
echo "   traffic per leg, on the CN machine:"
echo "     ping -i 0.2 -c 200 \$UE_IP"
echo "     iperf3 -c \$UE_IP -R -t 120        # no -b"
echo
PUB1=${UE_IP}
LEG_CONFIG=$COLLECT ARMS="a c c a" UE_IP=$PUB1 bash "$HERE/fly_utility_abba.sh" "${PREFIX}r" || die "phase 1 failed"
read -r -p "ground: the phone must re-attach for phase 2 (the gNB restarts, so its IP changes). New IP: " PUB2
[ -n "$PUB2" ] || PUB2=$PUB1
echo "   using $PUB2 for phase 2"

echo
echo "==================================================================="
echo " phase 2 of 2: CALIBRATION and R6  batch ${PREFIX}c"
echo "   cell: $DELIVERY   (MCS free, OLLA in the loop)"
echo "   arms: a (raw publication) b (calibrated) c (classical) d (calibrated + net), mirrored"
echo "   question: does the calibrated publication cost anything, and is R6's delivery form neutral?"
echo "   reading: B/D vs A must not be worse; inside +-5% the verdict says NO MEASURABLE DIFFERENCE"
echo "==================================================================="
echo "   traffic per leg: same two commands, with \$UE_IP = $PUB2"
echo
ARMS="a b c d d c b a" UE_IP=$PUB2 bash "$HERE/fly_utility_abba.sh" "${PREFIX}c" || die "phase 2 failed"

echo
echo "=== BOTH PHASES DONE. The two verdicts:"
echo "    $HERE/flight_verdict.py --legs $ROOT/doc_chinese/llr_ai_detection/work_tmp/legs/${PREFIX}r \\"
echo "        --iperf \"${PREFIX}ra1=?,${PREFIX}rc1=?,${PREFIX}rc2=?,${PREFIX}ra2=?\" \\"
echo "        --ping  \"${PREFIX}ra1=?,${PREFIX}rc1=?,${PREFIX}rc2=?,${PREFIX}ra2=?\" --batch ${PREFIX}r"
echo "    $HERE/flight_verdict.py --legs $ROOT/doc_chinese/llr_ai_detection/work_tmp/legs/${PREFIX}c \\"
echo "        --iperf \"${PREFIX}ca1=?,${PREFIX}cb1=?,${PREFIX}cc1=?,${PREFIX}cd1=?,${PREFIX}cd2=?,${PREFIX}cc2=?,${PREFIX}cb2=?,${PREFIX}ca2=?\" --batch ${PREFIX}c"
