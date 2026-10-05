#!/usr/bin/env bash
# UPLINK HEALTH of a leg, from its own PHY log - the gate that was missing.
#
# WHY (dev doc 11.59). Three pairs in a row were declared "not comparable" by pair_check on a PAYLOAD criterion
# (B/hop differing by 26-28%), and the payload was never the problem: the transport-block MEDIAN was 4612 B in
# every working leg. What differed was the UPLINK ERROR RATE - the fraction of PUSCH transmissions whose CRC
# failed - which swings between legs with the same configuration:
#
#     p272-triple 99.4% OK (571 retx)   p273-dual 77.4% OK (14751 retx)
#     p271-dual   98.6% OK (1626 retx)  p270-triple 84.6% OK (18155 retx)
#
# One leg of every pair was running a degraded uplink, in both arms, so it is the CHANNEL (these are OTA legs
# with a phone), not the code under test. But it contaminates every reading that depends on decoding: a failed
# transmission runs the decoder to its maximum iteration count, is retransmitted, and changes the MIX of work
# per delivered transport block. That is what pair_check was seeing, and it attributed it to the payload.
#
# So this is the check to run BEFORE quoting any stage ratio: both legs of a pair must be clean.
# usage:  bash ul_health.sh p272 p273        (label prefixes, as pair_check takes them)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ROOT="$(cd "$HERE/../../.." && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
DIRS="${LEG_LOG_DIRS:-$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs $ROOT/doc_chinese/macos_thread_priority/wip/logs}"
[ $# -ge 1 ] || { echo "usage: bash ul_health.sh <leg-label> [leg-label...]" >&2; exit 2; }

# The scan is a single pass in python3 (the first version sorted in awk, which is O(n^2) and timed out on a
# 70 MB log - a tool that cannot finish is worse than no tool).
python3 - "$@" <<'PYEOF'
import re, sys, glob, os, statistics as st
pat = re.compile(r'PUSCH: rnti=0x[0-9a-f]+ harq_id=(\d+) prb=\[(\d+), (\d+)\) symb=\[(\d+), (\d+)\) mod=(\w+) rv=(\d+) tbs=(\d+) crc=(\w+)')
root = os.environ.get('ROOT', '.')
dirs = os.environ.get('LEG_LOG_DIRS', '').split() or [
    os.path.join(root, 'doc_chinese/phy_pipeline_gpu/wip/logs'),
    os.path.join(root, 'doc_chinese/macos_thread_priority/wip/logs')]
print('%-14s %9s %7s %8s %9s %9s %9s  %s' % ('leg','new-tx','retx','CRC all','CRC steady','TBS p50','TBS mean','verdict'))
for leg in sys.argv[1:]:
    # A leg LABEL is searched for in the two log directories; a PATH is used as given (that is how a
    # verification run such as /tmp/mcs13_test.log gets checked before any leg is flown).
    if os.path.isfile(leg):
        f = leg
        leg = os.path.basename(leg)
    else:
        cands = []
        for d in dirs:
            cands += [x for x in glob.glob(os.path.join(d, 'gnb_*%s*.log' % leg)) if not x.endswith(('.stderr','.stdout'))]
        if not cands:
            print('%-14s no log found' % leg); continue
        f = max(cands, key=os.path.getmtime)
    tbs = []; n = retx = ok = ko = 0
    for line in open(f, errors='ignore'):
        if 'PUSCH:' not in line: continue
        m = pat.search(line)
        if not m: continue
        rv, t, crc = m.group(7), int(m.group(8)), m.group(9)
        if rv != '0': retx += 1; continue
        n += 1; tbs.append(t); ok += (crc == 'OK'); ko += (crc != 'OK')
    if n == 0:
        print('%-14s no PUSCH lines' % leg); continue
    # STEADY STATE decides. A leg starts with the traffic's ramp (HARQ/CFO convergence, the first slots after
    # attach) and ends with its drain, and both are lower than the middle for reasons that have nothing to do
    # with the arm: the MCS-13 verification run read 97.8% / 98.3% in its edge windows against 99.4-99.8% in the
    # middle. The gate is therefore on the middle 80% of the transmissions, and the overall figure is printed
    # beside it so a reader can see how much of the leg was ramp.
    pct = 100.0 * ok / max(1, ok + ko)
    lo, hi = int(0.1 * (ok + ko)), int(0.9 * (ok + ko))
    s_ok = s_ko = 0
    seen = 0
    for line in open(f, errors='ignore'):
        if 'PUSCH:' not in line:
            continue
        m = pat.search(line)
        if not m or m.group(7) != '0':
            continue
        if lo <= seen < hi:
            if m.group(9) == 'OK':
                s_ok += 1
            else:
                s_ko += 1
        seen += 1
    s_pct = 100.0 * s_ok / max(1, s_ok + s_ko)
    verdict = 'CLEAN' if s_pct >= 99.0 else ('marginal' if s_pct >= 95.0 else 'DEGRADED - do not quote stage ratios')
    tbs.sort()
    print('%-14s %9d %7d %7.1f%% %8.1f%% %9d %9.0f  %s' % (leg, n, retx, pct, s_pct, tbs[len(tbs)//2], st.mean(tbs), verdict))
PYEOF
echo
echo "A pair is quotable only when BOTH legs are CLEAN (>=99% CRC OK) AND pair_check passes."
