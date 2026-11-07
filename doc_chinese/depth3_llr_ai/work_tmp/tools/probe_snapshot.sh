#!/bin/bash
# Measure ONE training snapshot on a slice of the ruler -- export, compile, calibrate, replay and score all on
# THIS Mac, in one command.
#
# WHY IT NO LONGER HOPS MACHINES. The first version exported here and replayed on 203, which meant two macOS
# machines had to be awake for one measurement. Everything the chain needs is local: coremltools and the
# trainer are in this venv, coremlcompiler ships with the CLT, the replay binary is built here, and the
# ruler's 10,000 capture files sit in the repository. Offloading to 203 buys nothing for a few hundred units.
#
# THE BINARY IS NOT THE SAME ONE 203 RUNS. sha256 here is dbe8f06d..., on 203 it is d785f039...; delivered
# bits from this probe are therefore comparable WITHIN this probe family and to each other, and not directly
# to the 5,000-unit runs recorded on 203. The script prints BIN_SHA so every result carries its provenance;
# full-benchmark numbers still come from 203 for exactly this reason.
set -u
SRC=${1:?usage: probe_snapshot.sh <snapshot.keras | user@host:/path/snapshot.keras> [units]}
UNITS=${2:-500}
# TWO CHECKPOINTS CAN SHARE A BASENAME, AND THAT USED TO OVERWRITE A BUNDLE. Every run names its snapshots
# `epoch_0010_periodic.keras`, so the tag -- and therefore the bundle directory -- collided: probing a second
# "e10" silently replaced the first one's compiled bundle, and the measurement that followed scored a model
# other than the one it was named after. The tag therefore carries a short hash of the SOURCE: the file itself
# when it is local, the `host:path` string when it is remote (the fetch has not happened yet at this point).
TAG_BASE=$(basename "$SRC" .keras); TAG_BASE=${TAG_BASE##*:}
if [[ "$SRC" == *:* ]]; then TAG_HASH=$(printf '%s' "$SRC" | shasum -a 256 | cut -c1-6)
else TAG_HASH=$(shasum -a 256 "$SRC" | cut -c1-6); fi
TAG=${TAG_BASE}_${TAG_HASH}
MAC=$HOME/ai_depth3
WORK=$MAC/probe/$TAG
# THE HOST-SPECIFIC PATHS ARE OVERRIDABLE, SO ONE COPY SERVES BOTH MACHINES. On the Mac everything lives
# under this home; on 203 the interpreter is ~/venv_tf312, the ruler corpus is the replay copy, and the
# scoring reference is a single csv. The per-host file sets these without a second copy of this script.
PY=${PY:-$HOME/ai_ce_work/venv/bin/python}
LIB=${LIB:-$HOME/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train}
CORPUS=${CORPUS:-$HOME/dev/ocudu/lib/phy/upper/ai_signal_processors/test/ruler_s2_amc_5000_corpus}
BIN=${BIN:-$HOME/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay}
RUN_ARM=${RUN_ARM:-$HOME/ai_depth3/exp_bits_1102/run_arm_ext.py}
# THE REFERENCE IS THE CLASSICAL CPU CHAIN, AND IT IS A NUMBER, NOT A CSV. Until 2026-11-07 the score
# block loaded a per-unit `clean_cpu.csv` and printed the delta against it; that csv is the RULER's
# reference for the Metal front end (the old gpu pipeline vocabulary, 3 465 of 5 000 units), so slicing
# it to the probe's 500 units printed 930 016 -- a reference 19% weaker than the chain the AI arm is
# actually replacing (the dump's CPU chain, 1 149 352 on the same units). Two probe results were read
# against the wrong baseline before that was noticed. The csv has been deleted (it was byte-identical to
# the ruler archive, so nothing is lost) and the reference is now this constant, with its provenance.
CPU_REF=${CPU_REF:-1149352}
CPU_REF_NOTE="dump tb_s2_amc_ping_1102a (b68ef309), npu/generic decoder, PURE, 409/500 OK"
[ -f "$HOME/ai_depth3/bin/probe_env.sh" ] && . "$HOME/ai_depth3/bin/probe_env.sh"
mkdir -p "$WORK"

echo "=== $TAG  units $UNITS  $(date +%T)"
if [[ "$SRC" == *:* ]]; then scp -q "$SRC" "$WORK/$TAG.keras" || { echo "FETCH FAILED"; exit 2; }
else cp "$SRC" "$WORK/$TAG.keras"; fi
$PY -u "$LIB/train_depth3.py" --export-only "$WORK/$TAG.keras" --export-probe "$WORK/$TAG.mlpackage" \
  --modcond --layout spatial --modulation QPSK ${C32:---c-hid 32 --blocks 18} --snr-range=-0.18,34.70 \
  2>&1 | grep -a "wrote the fp16" | tail -1
xcrun coremlcompiler compile "$WORK/$TAG.mlpackage" "$WORK/" 2>&1 | tail -1
$PY "$LIB/calibrate_depth3.py" --model "$WORK/$TAG.keras" --data "$MAC/gate_ai_models/calib/datasets_monitor_w2" \
  --modulation QPSK --cond --layout spatial --samples 60 --write-scales "$WORK/" 2>&1 | grep -a "chosen" | tail -1

# A PREFIX OF THE RULER IS NOT A SAMPLE OF IT. `head -500` of the sorted unit list drew 72.8% 256QAM
# against the ruler's 39.2%, and 2 425 bits per unit against 1 787 -- a thirty-six percent richer slice.
# The probe therefore reads a fixed, seeded random subset (seed 20261105) whose composition matches the
# whole within a percentage point; every probe uses the same file, so probes compare with each other.
UNITS_FILE=${UNITS_FILE:-$MAC/exp_bits_1102/units_probe_500.txt}
head -n "$UNITS" "$UNITS_FILE" > "$WORK/units_$UNITS.txt"
# THE OLD CSV MUST GO FIRST. run_arm_ext resumes from the csv it finds and skips units already in it, so
# re-running a probe with a DIFFERENT unit set silently scored the previous set -- which is exactly how
# the first attempt at the seeded subset reproduced the prefix result to the bit.
rm -f "$WORK/$TAG.csv"
OCUDU_DEPTH3_FALLBACK=none python3 "$RUN_ARM" --arm ai \
  --bundle "$WORK/$TAG.mlmodelc" --units "$WORK/units_$UNITS.txt" --corpus "$CORPUS" \
  --out "$WORK/$TAG.csv" --logs "$WORK/logs" --tmp "$WORK/tmp" --warmup 0 --guard 1 \
  > "$WORK/run.log" 2>&1
UNITS_SHA=$(head -n "$UNITS" "$UNITS_FILE" | shasum -a 256 | cut -c1-24)
$PY - "$WORK/$TAG.csv" "$UNITS" "$UNITS_SHA" "$CPU_REF" "$CPU_REF_NOTE" "$(shasum -a 256 "$BIN" | cut -c1-24)" <<'PY'
import csv, sys
arm_csv, n, units_sha, ref, ref_note, bin_sha = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), sys.argv[5], sys.argv[6]
rows = list(csv.DictReader(open(arm_csv)))[:n]
bits = [int(r['tbs_bits']) if r['status'] == 'OK' else 0 for r in rows]
ok = sum(1 for r in rows if r['status'] == 'OK')
total = sum(bits)
# THE PROBE'S OWN SAMPLING INTERVAL, so nobody reads a five-hundred-unit number as if it were the ruler's.
# A random five hundred is unbiased for the five-thousand result -- its median lands on it -- but its 95
# percent spread is about six points wide, and one fixed subset can therefore sit five points off the ruler
# without anything being wrong. Bootstrap the ARM side only: the reference is a fixed whole-set number, so
# there is no paired per-unit CPU sample to resample any more (that is what the deleted csv provided).
import random
rng = random.Random(20261102)
gains = []
for _ in range(2000):
    s = sum(bits[rng.randrange(len(bits))] for _ in range(len(bits)))
    gains.append((s - ref) / ref * 100)
gains.sort()
lo, hi = gains[50], gains[-51]
print(f"units {len(bits)}  OK {ok}  arm {total:,}  cpu_ref {ref:,}  delta {total-ref:+,} "
      f"({(total-ref)/ref*100:+.2f}%)  arm 95% 区间 [{lo:+.2f}%, {hi:+.2f}%]  "
      f"UNITS_SHA {units_sha}  BIN_SHA {bin_sha}")
print(f"  reference: {ref_note}")
PY
echo "=== done $(date +%T)"
