#!/bin/bash
# One leg, end to end, in the shape the mother set was built: harvest at a raised cap, then label self/harq the way
# harq_backfill does.
#
# WHAT IS DIFFERENT FROM THE ORIGINAL BUILD. The original replayed the whole leg at a cap of six. This replays only the
# units the archived dump did NOT already decode -- their labels exist and replaying them would be dead work -- and it
# raises the cap to twenty, which is the whole point: a reception the decoder abandons at six may converge at twenty, and
# then it has a label.
#
# THE SIDECARS COVER THE WHOLE LEG. harq_backfill decides which rv0 unit belongs to which retransmission by looking at the
# PDU sequence of the leg; if the already-decoded units' sidecars are missing, chains break at those points and harq
# samples are silently lost. Captures are extracted only for the candidates and their donors.
#
# Usage: harvest_build_leg.sh <leg> <kind: amc|mcs|w2> <work dir> [--jobs 12] [--cap 20]
set -u
LEG=$1; KIND=$2; WORK=$3; shift 3
JOBS=12; CAP=20
while [ $# -gt 0 ]; do case "$1" in --jobs) JOBS=$2; shift 2;; --cap) CAP=$2; shift 2;; *) exit 2;; esac; done
SRC=192.168.31.211
PY=$HOME/work/ai_phy_train/.venv/bin/python
case "$KIND" in amc) K=s2_amc;; mcs) K=s2_mcs;; w2) K=w2;; *) echo "bad kind"; exit 2;; esac
BIN=${BIN:-$HOME/work/ai_phy_train/bin/ul_chain_replay_cpu}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$WORK"; cd "$WORK" || exit 1

TB="tb_${LEG}.tar.gz"; CORP="${LEG}.tar.gz"
[ -s "$TB" ] || scp -q jwang@$SRC:/mnt/corpus/${K}_tb_tarballs/tb_${LEG}.tar.gz "$TB" || exit 1
[ -s "$CORP" ] || scp -q jwang@$SRC:/mnt/corpus/${K}_corpus_tarballs/${LEG}.tar.gz "$CORP" || exit 1

mkdir -p idx && tar xzf "$TB" -C idx --wildcards "*/index.csv" 2>/dev/null
INDEX=$(find idx -name index.csv | head -1); [ -n "$INDEX" ] || { echo "ABORT: no index"; exit 1; }

# 1. EVERY SIDECAR FIRST, because only the sidecars say which units are retransmissions (and so which can donate).
awk -F, 'NR > 1 { print $1 }' "$INDEX" > all_units.txt
nice -n 10 $PY "$HERE/extract_units.py" "$CORP" /dev/null . "$LEG" --sidecar=all_units.txt >/dev/null || exit 1

# 2. CANDIDATES ARE THE rv0 UNITS THE DUMP DID NOT DECODE. Donors are the rv!=0 units of the same chain.
awk -F, 'NR > 1 && $3 != "OK" { print $1 }' "$INDEX" > ko_units.txt
: > cand.txt; : > donor.txt
while read -r u; do
  f="$WORK/$LEG/$u.txt"; [ -f "$f" ] || continue
  nd=$(grep -m1 '^new_data=' "$f" | cut -d= -f2)
  [ "$nd" = "1" ] && echo "$u" >> cand.txt
  [ "$nd" = "0" ] && echo "$u" >> donor.txt
done < ko_units.txt

echo "  candidates $(wc -l < cand.txt), retransmissions in the leg $(wc -l < donor.txt), sidecars $(wc -l < all_units.txt)"

# 4. REPLAY. Standalone first; a candidate that fails alone gets one chained attempt against its donor.
mkdir -p out/tb out/logs
one() {
  local u=$1; local base="$WORK/$LEG/$u"; local w="$WORK/out/stage/$u"
  mkdir -p "$w"
  local line
  line=$(OCUDU_REPLAY_DEC_ITERATIONS="$CAP" OCUDU_HELENA_DUMP_DIR="$w" OMP_NUM_THREADS=1 \
           "$BIN" "$base" --phy_pipeline cpu --pusch_receiver_backend classic --out "$w/s" 2>/dev/null \
         | grep -E '^\s*\[0\]' | head -1)
  local crc=none pop=""
  case "$line" in *"crc=OK"*) crc=OK; pop=self ;; esac
  if [ "$crc" != "OK" ]; then
    # -F, IS NOT OPTIONAL: without it $1 is the whole "head,donor" line, no candidate ever matches, and every chained
    # attempt is skipped -- which is exactly how the harq half stayed empty while the chain file was correct.
    local d; d=$(awk -F, -v u="$u" '$1 == u { print $2 }' "$WORK/out/chains.txt" 2>/dev/null | head -1)
    if [ -n "$d" ] && [ -f "$WORK/$LEG/$d.bin" ]; then
      local w2="$w/chain"
      # THE COMBINED VERDICT IS THE LAST LINE, NOT THE FIRST. A chained run replays the first transmission and then the
      # donor through the same soft buffer, so it prints one result line per reception: the first is the candidate still
      # failing on its own (that is why it is a candidate), and the last is the block decoded with the donor's LLRs
      # added. Taking the first line -- as this did -- made every chained attempt look like a failure and left the harq
      # half of the harvest empty while the combined decodes were succeeding all along.
      line=$(OCUDU_REPLAY_DEC_ITERATIONS="$CAP" OCUDU_HELENA_DUMP_DIR="$w2" OMP_NUM_THREADS=1 \
               "$BIN" "$base" --also "$WORK/$LEG/$d" --harq-chain \
               --phy_pipeline cpu --pusch_receiver_backend classic --out "$w2/s" 2>/dev/null \
             | grep -E '^\s*\[0\]' | tail -1)
      case "$line" in *"crc=OK"*) crc=OK; pop=harq ;; esac
    fi
  fi
  if [ "$pop" != "" ]; then
    local tb; tb=$(ls "$w"/tb_*.bits "$w"/chain/tb_*.bits 2>/dev/null | head -1)
    [ -n "$tb" ] && cp "$tb" "$WORK/out/tb/$u.bits"
  fi
  echo "$u,$pop,$crc" > "$WORK/out/logs/$u.row"
  rm -rf "$w"
}
export -f one; export WORK LEG BIN CAP
# The donor map must exist before the pool starts; it is one column per candidate.

python3 - "$WORK" "$LEG" <<'PY'
import collections, os, sys
work, leg = sys.argv[1], sys.argv[2]
units = {}
for name in os.listdir(os.path.join(work, leg)):
    if not name.endswith(".txt") or name.endswith("_ce.txt"):
        continue
    u = name[:-4]; f = {}
    for line in open(os.path.join(work, leg, name), errors="ignore"):
        k, s, v = line.strip().partition("=")
        if s and k in ("rnti", "harq_id", "rv", "new_data", "scs_khz"):
            f[k] = v
    try:
        _, frame, slot, _ = u.split("_")
        f["ord"] = int(frame) * (20 if f.get("scs_khz", "30") == "30" else 10) + (int(slot) % (20 if f.get("scs_khz", "30") == "30" else 10))
    except ValueError:
        continue
    units[u] = f
chains = collections.defaultdict(list)
for u, f in units.items():
    chains[(f.get("rnti"), f.get("harq_id"))].append((f["ord"], u, f))
donor = {}
for mem in chains.values():
    mem.sort()
    head = None
    for _, u, f in mem:
        if f.get("rv") == "0":
            head = u
        elif head and head not in donor and f.get("new_data") == "0":
            donor[head] = u
with open(os.path.join(work, "out", "chains.txt"), "w") as fh:
    for k, v in donor.items():
        fh.write(f"{k},{v}\n")
print(f"  chains: {len(donor)} rv0 units have a donor")
PY

# 3. CAPTURES FOR EXACTLY WHAT WILL BE REPLAYED: the candidates, plus the donors the chain map actually paired them
# with. Extracting "every retransmission" instead was wrong for a concrete reason: the donor of a candidate is chosen by
# the chain walk over the whole leg, and it can be a unit the previous dump already decoded -- whose capture was never
# extracted, so the chained attempt silently found no donor file and the harq half came out empty.
cp cand.txt needed.txt
awk -F, '{ print $2 }' out/chains.txt >> needed.txt
sort -u needed.txt -o needed.txt
nice -n 10 $PY "$HERE/extract_units.py" "$CORP" needed.txt . "$LEG" >/dev/null || exit 1
echo "  captures for $(wc -l < needed.txt) units"


tr '\n' '\0' < cand.txt | xargs -0 -n 1 -P "$JOBS" bash -c 'one "$1"' _ 2>/dev/null

echo "unit,population,crc" > out/index_cap20.csv
while read -r u; do [ -f "out/logs/$u.row" ] && cat "out/logs/$u.row" >> out/index_cap20.csv; done < cand.txt
awk -F, 'NR > 1 { c[$2]++ } END { for (k in c) printf "  %s=%d", k, c[k]; print "" }' out/index_cap20.csv
echo "  bits: $(ls out/tb | wc -l)"
