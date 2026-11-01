#!/usr/bin/env bash
# Build one leg's FULL mother set: every decodable transport block, split by where it came from.
#
# WHY THIS IS NOT queue_build_datasets.sh. That script is the sampling pipeline: it runs select_by_bin.py to
# take a quota per (modulation x width x SNR) bin, 2500 first-transmission and 250 recovered, because a small
# experiment wants a balanced library rather than a faithful one. A mother set wants everything, so this script
# calls build_depth3_dataset.py directly and never touches the sampler.
#
# That distinction is not cosmetic. The queue's own header records that an "unlimited" build once silently
# reused the sampled build's ledger, because the two environment variables were written as plain assignments
# that ran before their default expansion -- and it cost hours, because neither produced an error. The safe
# way to build everything is to not invoke the quota machinery at all.
#
# WHAT IT PRODUCES. One npz per (modulation, source), named <leg>_<MOD>_<self|harq>.npz, matching the w2 set:
#   self  the reception whose first redundancy version decoded on its own
#   harq  one whose first version failed and a later version of the same process recovered
#
# THE MODULATIONS ARE READ FROM THE DATA, not from the leg's name. A leg's grant fixes one modulation, but the
# same slot can carry other traffic at a lower rate -- a w2 leg granted 256-QAM produced captures at 64-QAM,
# 16-QAM and QPSK as well. The census below is what decides how many datasets there are.
#
# Usage: build_mother_set.sh <leg> [<leg> ...]
#   TRAIN (default ~/work/ai_phy_train) holds tb_<leg>/ (the dump), <leg>/ (the captures) and the output.
#   GAP_SLOTS (default 200) is passed to the backfill tool; see the note there before lowering it.
set -u

TRAIN=${TRAIN:-$HOME/work/ai_phy_train}
OUT=${OUT:-$TRAIN/datasets_full_s2}
# WHERE THE INPUTS ACTUALLY LIVE. The dumps and the captures need not sit under $TRAIN, and on a machine that
# only builds from a corpus drive they do not: the dump goes wherever dump_tb.sh put it and the captures stay
# on the drive. Both default to the $TRAIN layout so the common case needs no arguments.
DUMP_ROOT=${DUMP_ROOT:-$TRAIN}
CORPUS_ROOT=${CORPUS_ROOT:-$TRAIN}
# The backfill tool orders receptions by frame*20 + slot, but `slot` is a system-slot counter wrapping at
# 20480, so a threshold measured in "slots" is not measured in slots and short gaps cut real chains. The
# documented rule has no gap guard at all, so this is made wide enough to be inert rather than tuned.
GAP_SLOTS=${GAP_SLOTS:-200}
MODS=${MODS:-"QPSK 16QAM 64QAM 256QAM"}
TOOLS=$(cd "$(dirname "$0")" && pwd)
PY=${PY:-$TRAIN/.venv/bin/python}

# build_depth3_dataset.py refuses to import unless it can find a repository root, which it identifies by a
# directory holding both build/ and lib/. A training tree has lib/ only, so give it the empty build/ it looks
# for. Nothing reads it.
[ -d "$TRAIN/build" ] || mkdir -p "$TRAIN/build"
[ -d "$OUT" ] || mkdir -p "$OUT"

status=0

for LEG in "$@"; do
  DUMP="$DUMP_ROOT/tb_$LEG"
  CAP="$CORPUS_ROOT/$LEG"
  echo "=== $LEG  $(date +%H:%M:%S)"

  if [ ! -f "$DUMP/index.csv" ]; then
    echo "  SKIP: no $DUMP/index.csv (the leg has not been dumped)" >&2
    status=1
    continue
  fi
  if [ ! -d "$CAP" ]; then
    echo "  SKIP: no capture directory $CAP" >&2
    status=1
    continue
  fi

  # 1. Mark every unit with where its transport block came from. Writes index_self.csv,
  #    index_harq_only.csv (whose rows carry crc=OK already, which is what the builder wants) and a combined
  #    index_harq.csv carrying the label as a column.
  echo "  marking sources (gap guard inert at $GAP_SLOTS)"
  # --leg is a PATH, not a name: the tool reads each PDU sidecar as os.path.join(args.leg, name + ".txt"),
  # so a bare leg name resolves against the working directory and silently labels nothing when that is not
  # the corpus root. Pass the capture directory itself.
  "$PY" "$TOOLS/harq_backfill.py" --leg "$CAP" --index "$DUMP/index.csv" \
      --live-tb "$CAP/tb_live" --out "$DUMP" --max-gap-slots "$GAP_SLOTS" 2>&1 | sed 's/^/    /'
  for f in index_self.csv index_harq_only.csv; do
    [ -f "$DUMP/$f" ] || { echo "  SKIP: backfill did not write $DUMP/$f" >&2; status=1; continue 2; }
  done

  # 2. Which modulations are actually present. Read from every capture header, because the leg name says what
  #    the grant carries and not what the slot contains.
  # One pass over the headers rather than one pass per candidate modulation: a leg runs to six figures of
  # capture files, and globbing them onto a command line overflows it.
  census=$(grep -rh --include='cap_*.txt' --exclude='*_ce.txt' '^modulation=' "$CAP" 2>/dev/null | sort | uniq -c)
  echo "  modulations present:"
  printf '%s\n' "$census" | sed 's/^/    /'
  present=""
  for M in $MODS; do
    # shellcheck disable=SC2086
    n=$(printf '%s\n' "$census" | awk -v m="modulation=$M" '$2 == m {print $1}')
    [ "${n:-0}" -gt 0 ] && present="$present $M"
  done
  if [ -z "$present" ]; then
    echo "  SKIP: no capture reported a known modulation" >&2
    status=1
    continue
  fi

  # 3. One build per (modulation, source). No selection step: this is the mother set.
  for M in $present; do
    for POP in self harq; do
      UNITS="$DUMP/index_self.csv"
      [ "$POP" = "harq" ] && UNITS="$DUMP/index_harq_only.csv"
      NAME="${LEG}_${M}_${POP}"
      echo "  building $NAME"
      "$PY" "$TOOLS/build_depth3_dataset.py" --units "$UNITS" --tb "$DUMP/tb" --corpus "$CAP" \
          --out "$OUT" --name "$NAME" --source real --modulation "$M" --min-prb 1 --max-units 0 2>&1 \
          | sed 's/^/    /'
    done
  done
done

# 4. Audit whatever landed. The checker reports sample counts, shapes, bit balance and SNR, and "findings:
#    none" is what a good build looks like.
echo "=== audit"
"$PY" "$TOOLS/check_depth3_dataset.py" --dir "$OUT" 2>&1 | tail -20 | sed 's/^/  /'

exit $status
