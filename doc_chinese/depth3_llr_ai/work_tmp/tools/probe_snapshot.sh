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
TAG=$(basename "$SRC" .keras)
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
CLEAN_CPU=${CLEAN_CPU:-$HOME/ai_depth3/exp_bits_1102/clean_cpu.csv}
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
$PY - "$WORK/$TAG.csv" "$CLEAN_CPU" "$UNITS" "$(shasum -a 256 "$BIN" | cut -c1-24)" <<'PY'
import csv, sys
arm_csv, cpu_csv, n, bin_sha = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
def load(p): return {r['unit']: r for r in csv.DictReader(open(p))}
a, c = load(arm_csv), load(cpu_csv)
units = [u for u in a if u in c][:n]
def bits(d, u):
    r = d.get(u)
    return int(r['tbs_bits']) if r and r['status'] == 'OK' else 0
ab = sum(bits(a, u) for u in units); cb = sum(bits(c, u) for u in units)
# THE PROBE'S OWN SAMPLING INTERVAL, so nobody reads a five-hundred-unit number as if it were the ruler's.
# A random five hundred is unbiased for the five-thousand result -- its median lands on it -- but its 95
# percent spread is about six points wide, and one fixed subset can therefore sit five points off the ruler
# without anything being wrong. Bootstrap the subset itself to show that width.
import random, statistics
rng = random.Random(20261102)
gains = []
for _ in range(2000):
    s = [units[rng.randrange(len(units))] for _ in range(len(units))]
    a_ = sum(bits(a, x) for x in s); c_ = sum(bits(c, x) for x in s)
    if c_: gains.append((a_ - c_) / c_ * 100)
gains.sort()
lo, hi = gains[50], gains[-51]
print(f"units {len(units)}  arm {ab:,}  cpu {cb:,}  delta {ab-cb:+,} ({(ab-cb)/cb*100:+.2f}%)  "
      f"自 95% 区间 [{lo:+.2f}%, {hi:+.2f}%]  BIN_SHA {bin_sha}")
PY
echo "=== done $(date +%T)"
