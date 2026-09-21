#!/usr/bin/env bash
# How often does the MISS path change the data? (design document 5.9.38 finding 2)
#
# One 8-slot `hostfirst` run differed from `ref` in slot 2's estimator scalars and 47/792 soft bits, and has
# not reappeared since. This script measures the rate instead of arguing about it: it runs the pair N times
# and reports, per run, which slots' soft bits and estimator scalars differ. No difference in N runs is a
# RATE (an upper bound), not a proof - the observation itself is on record either way.
#
# Usage: doc_chinese/phy_pipeline_gpu/wip/l1_hop_rate.sh [pairs] [slots] [workdir]
set -u

PAIRS="${1:-50}"
SLOTS="${2:-8}"
WORK="${3:-/tmp/l1_rate}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TOOL="$REPO/build/lib/phy/upper/channel_processors/metal/ul_chain_replay"
PDU="${L1_PDU:-doc_chinese/work_tmp/corpus/syn001_3}"

[[ -x "$TOOL" ]] || { echo "missing $TOOL" >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$REPO"

# Arm A: the hand-over OFF (the hop reads a grid the front end committed itself).
# Arm B: the hand-over ON with the host consumer first, i.e. the hop MISSES and takes the device-side wait.
for i in $(seq 1 "$PAIRS"); do
  "$TOOL" "$PDU" --out "$WORK/ref$i" --metal --device-grid --hop-td "$SLOTS" >"$WORK/ref$i.log" 2>&1
  OCUDU_DFT_RELEASE_BLOCK=1 OCUDU_GPU_STRICT=1 OCUDU_L1_HOST_FIRST=1 \
    "$TOOL" "$PDU" --out "$WORK/hf$i" --metal --device-grid --hop-td "$SLOTS" >"$WORK/hf$i.log" 2>&1
done

python3 - "$WORK" "$PAIRS" <<'PY'
import glob, os, struct, sys

work, pairs = sys.argv[1], int(sys.argv[2])

def llr(path):
    out = []
    with open(path, "rb") as handle:
        while True:
            head = handle.read(4)
            if len(head) < 4:
                break
            (size,) = struct.unpack("<I", head)
            data = handle.read(size)
            if len(data) < size:
                break
            out.extend(struct.unpack("<%db" % size, data))
    return out

def blob(path):
    with open(path, "rb") as handle:
        return handle.read()

bad_llr = []
bad_ce = []
for i in range(1, pairs + 1):
    slots = sorted(int(os.path.basename(p).split("_")[1]) for p in glob.glob(f"{work}/ref{i}_*_llr.bin"))
    if not slots:
        print(f"run {i}: NO CAPTURES (see {work}/ref{i}.log)")
        bad_llr.append(i)
        continue
    llr_slots = [s for s in slots if llr(f"{work}/ref{i}_{s}_17921_llr.bin") != llr(f"{work}/hf{i}_{s}_17921_llr.bin")]
    ce_slots = [s for s in slots
                if blob(f"{work}/ref{i}_{s}_17921_ce.txt") != blob(f"{work}/hf{i}_{s}_17921_ce.txt")]
    if llr_slots or ce_slots:
        print(f"run {i}: slots={len(slots)} LLR-differing={llr_slots} CE-differing={ce_slots}")
        bad_llr.append(i)
    if ce_slots:
        bad_ce.append(i)

print()
print(f"MISS arm vs reference: {len(bad_llr)}/{pairs} runs differ in the soft bits, "
      f"{len(bad_ce)}/{pairs} in the estimator scalars")
if bad_llr:
    print("differing runs kept in %s: %s" % (work, " ".join(str(i) for i in bad_llr)))
    sys.exit(1)
print("no difference observed; the single 5.9.38 observation was not reproduced at this rate")
PY
