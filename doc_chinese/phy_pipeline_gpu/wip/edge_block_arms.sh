#!/usr/bin/env bash
# Test 15 (edge-block guard) arms.
#
# The guard sweeps n_prb = 48, 49, 50, 51 (rem_prb 0, 1, 2, 0 with block_prb = 3) x three levels x 40
# realizations and carries four criteria:
#   (a) SELF-CHECK - the rem_prb = 0 control pair must agree, or the case says it is measuring its own
#       noise floor instead of reporting a defect;
#   (b) the four allocations' means must agree within 1.10 (the registered limit);
#   (c) the per-realization band within one allocation must stay under 8.0 (clean 1.750, a polluted hop
#       14.7 to 160.1);
#   (d) STRUCTURAL - every hop must encode the wait for its own correlation build's command buffer.
#
# This script runs both arms and reports which criterion carried each run. (d) is the reverse arm's
# teeth: the prefix form (OCUDU_CE_CORR_FENCED=0) encodes no such wait, so (d) is red in every run that
# reaches it, while (a) to (c) need the race to be lost (~1 hop in 19000, so ~2.5% of runs at 480 hops).
# A rollback run that is red at Test 3 or Test 9 never reaches Test 15 at all - that is also a red
# binary, and the summary counts the two separately.
#
# Usage:
#   bash doc_chinese/phy_pipeline_gpu/wip/edge_block_arms.sh [runs-per-arm]
set -u

RUNS=${1:-5}

cd "$(dirname "$0")/../../.." || exit 1
BIN=build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
[[ -x $BIN ]] || { echo "missing $BIN (lib/ is EXCLUDE_FROM_ALL: build the target by name)" >&2; exit 1; }

OUT=$(mktemp -d)
report() { # report <arm> <index> <file> <rc> <detail-file>
  python3 - "$1" "$2" "$3" "$4" >>"$5" <<'PY'
import re, sys
arm, i, path, rc = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
txt = open(path, errors="replace").read()

def one(rx, flags=0):
    m = re.search(rx, txt, flags)
    return m

verdict, detail = "?", ""
if one(r"Test 15 PASS"):
    verdict = "PASS"
    detail = "; ".join(
        f"L{lvl} spread {sp} control {ct}" for lvl, sp, ct in
        re.findall(r"level ([0-9.e+-]+) allocations agree within ([0-9.]+) \(control pair ([0-9.]+)", txt))
elif (m := one(r"Test 15 SELF-CHECK FAILED")):
    verdict = "T15-(a)-SELFCHECK"
    detail = one(r"control pair[^.]*\.").group(0)[:210] if one(r"control pair[^.]*\.") else ""
elif (m := one(r"Test 15 FAIL: the edge block changes the noise estimate")):
    verdict = "T15-(b)-ALLOCATIONS"
    detail = one(r"Test 15 FAIL: the edge block changes[^.]*\.").group(0)[:210]
elif (m := one(r"Test 15 FAIL: one realization is out of band[^.]*\.")):
    verdict = "T15-(c)-BAND"
    detail = m.group(0)[:210]
elif (m := one(r"Test 15 FAIL: (\d+) of (\d+) hops encoded the wait[^.]*\.")):
    verdict = "T15-(d)-FENCE"
    detail = f"{m.group(1)} of {m.group(2)} hops waited"
else:
    m = one(r"Test (\d+) FAIL: ([^\n]{0,120})")
    verdict = f"stopped-at-Test{m.group(1)}" if m else "no-verdict"
    detail = m.group(2) if m else txt.strip().splitlines()[-1][:120] if txt.strip() else ""
print(f"  {arm:<9} run {i:>2}  rc={rc:<4} {verdict:<20} {detail}")
PY
}

echo "Test 15 edge-block guard: $RUNS runs per arm"
: >"$OUT/default.txt"
: >"$OUT/rollback.txt"
for ((i = 1; i <= RUNS; ++i)); do
  "$BIN" >"$OUT/d$i.txt" 2>&1
  report default "$i" "$OUT/d$i.txt" "$?" "$OUT/default.txt"
  OCUDU_CE_CORR_FENCED=0 "$BIN" >"$OUT/r$i.txt" 2>&1
  report rollback "$i" "$OUT/r$i.txt" "$?" "$OUT/rollback.txt"
done

cat "$OUT/default.txt"
echo
cat "$OUT/rollback.txt"
echo
python3 - "$OUT/default.txt" "$OUT/rollback.txt" <<'PY'
import sys, collections
for name, path, want in (("default", sys.argv[1], "green"), ("rollback", sys.argv[2], "red")):
    kinds = collections.Counter(l.split()[4] for l in open(path) if l.strip())
    rcs = collections.Counter(l.split()[3] for l in open(path) if l.strip())
    print(f"  {name:<9} ({want}): " + ", ".join(f"{k} x{v}" for k, v in kinds.most_common())
          + "   [rc: " + ", ".join(f"{k} x{v}" for k, v in rcs.most_common()) + "]")
PY
rm -rf "$OUT"
