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
# HOW MANY (MODULATION, SOURCE) BUILDS RUN AT ONCE. The pairs are independent -- each writes its own
# npz -- and every child is one BLAS thread, so this is process-level parallelism: it changes wall
# time, not content, and a leg's total sample memory is the same as when the pairs run serially.
# Default 1 keeps the historical behaviour. A child's exit status is folded into `status`, so a
# pair that dies in the background cannot make the leg look successful.
JOBS=${JOBS:-1}
case "$JOBS" in ''|*[!0-9]*|0) JOBS=1 ;; esac
# CHUNK (default 0 = off) caps how many samples one build process assembles. A pair with more
# candidates than this is built as contiguous slices and then stream-merged, because a 100k-sample
# leg peaks at ~25 GB in one process. Contiguous slices concatenated in order reproduce the
# unchunked sample order exactly; the merge streams members, so it does not need the parts and the
# result in memory together. A failed part fails the leg: the merge is skipped, never three quarters.
CHUNK=${CHUNK:-0}
case "$CHUNK" in ''|*[!0-9]*) CHUNK=0 ;; esac
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

  # 3. One build per (modulation, source). No selection step: this is the mother set. Up to
  #    $JOBS pairs run at once; each child keeps one BLAS thread and writes a different npz, so
  #    this is wall-clock parallelism between processes, not inside one. A child's exit code is
  #    recorded and folded into $status below.
  job_dir=$(mktemp -d "${TMPDIR:-/tmp}/motherset.XXXXXX") || exit 1
  job_n=0
  pair_names=()
  pair_k=()
  for M in $present; do
    for POP in self harq; do
      UNITS="$DUMP/index_self.csv"
      [ "$POP" = "harq" ] && UNITS="$DUMP/index_harq_only.csv"
      NAME="${LEG}_${M}_${POP}"
      k=1
      if [ "$CHUNK" -gt 0 ]; then
        count=$("$PY" "$TOOLS/build_depth3_dataset.py" --units "$UNITS" --tb "$DUMP/tb" \
                --corpus "$CAP" --out "$OUT" --name "$NAME" --source real --modulation "$M" \
                --min-prb 1 --max-units 0 --count-only 2>/dev/null | tail -1)
        case "$count" in
          ''|*[!0-9]*)
            echo "  FAILED: could not count candidates for $NAME" >&2
            status=1
            continue
            ;;
        esac
        if [ "$count" -gt "$CHUNK" ]; then
          k=$(( (count + CHUNK - 1) / CHUNK ))
        fi
      fi
      pair_idx=${#pair_names[@]}
      pair_names[$pair_idx]="$NAME"
      pair_k[$pair_idx]="$k"
      for ci in $(seq 0 $((k - 1))); do
        if [ "$k" -gt 1 ]; then PARTNAME="${NAME}_part${ci}"; else PARTNAME="$NAME"; fi
        job_n=$((job_n + 1))
        echo "  building $PARTNAME"
        printf '%s\n' "$POP" > "$job_dir/$job_n.pop"
        printf '%s\n' "$PARTNAME" > "$job_dir/$job_n.name"
        printf '%s\n' "$pair_idx" > "$job_dir/$job_n.pair"
        (
          set -o pipefail
          "$PY" "$TOOLS/build_depth3_dataset.py" --units "$UNITS" --tb "$DUMP/tb" --corpus "$CAP" \
              --out "$OUT" --name "$PARTNAME" --source real --modulation "$M" --min-prb 1 --max-units 0 \
              --chunk-index "$ci" --chunk-count "$k" 2>&1 \
              | tee "$job_dir/$job_n.log" | sed 's/^/    /'
          echo $? > "$job_dir/$job_n.rc"
        ) &
        # Poll the job table instead of `wait -n`: bash 3.2 (macOS) has no `wait -n`, and this
        # form is correct there too. The children are reaped by the `wait` below.
        while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
          sleep 1
        done
      done
    done
  done
  wait
  for rc_file in "$job_dir"/*.rc; do
    [ -e "$rc_file" ] || continue
    n=${rc_file##*/}; n=${n%.rc}
    rc=$(cat "$rc_file")
    pair=$(cat "$job_dir/$n.pair")
    if [ "$rc" -eq 0 ]; then continue; fi
    # An empty harq population is not a failure: when a modulation has no recoveries there is no
    # retransmission to label, and build_depth3_dataset exits non-zero with this exact message.
    # A non-empty harq pair, or any self pair, that exits non-zero is a real failure.
    if [ "$(cat "$job_dir/$n.pop")" = "harq" ] && grep -q 'no samples produced' "$job_dir/$n.log"; then
      continue
    fi
    echo "  FAILED: $(cat "$job_dir/$n.name") exited $rc" >&2
    status=1
    : > "$job_dir/pair_$pair.failed"
  done
  # Merge only pairs whose every part succeeded; a partial pair is deleted, not merged.
  for pair_idx in "${!pair_names[@]}"; do
    k=${pair_k[$pair_idx]}
    [ "$k" -gt 1 ] || continue
    NAME=${pair_names[$pair_idx]}
    parts=()
    for ci in $(seq 0 $((k - 1))); do parts+=("$OUT/${NAME}_part${ci}.npz"); done
    if [ -e "$job_dir/pair_$pair_idx.failed" ]; then
      echo "  FAILED: $NAME has a failed chunk; not merging" >&2
      rm -f "${parts[@]}"
      continue
    fi
    for part in "${parts[@]}"; do
      if [ ! -f "$part" ]; then
        echo "  FAILED: $NAME is missing $part; not merging" >&2
        status=1
        rm -f "${parts[@]}"
        continue 2
      fi
    done
    if "$PY" "$TOOLS/merge_depth3_parts.py" --out "$OUT/$NAME.npz" "${parts[@]}"; then
      rm -f "${parts[@]}"
    else
      echo "  FAILED: merging $NAME failed" >&2
      status=1
    fi
  done
  rm -rf -- "$job_dir"
done

# 4. Audit this leg's datasets. The checker reports sample counts, shapes, bit balance and SNR, and
#    "findings: none" is what a good build looks like. Only THIS leg's files are audited: auditing
#    the whole output directory after every leg re-reads every earlier dataset, and the cost grows
#    with the square of the corpus (measured 2026-11-02: 19 GB of output already made each leg's
#    audit minutes long, and it only gets worse). Every dataset is still audited exactly once, by
#    the leg that produced it, and the whole directory is audited again at the end.
echo "=== audit"
audit_dir=$(mktemp -d "${TMPDIR:-/tmp}/motherset-audit.XXXXXX")
ln -s "$OUT"/${LEG}_*.npz "$audit_dir"/ 2>/dev/null || true
"$PY" "$TOOLS/check_depth3_dataset.py" --dir "$audit_dir" 2>&1 | tail -20 | sed 's/^/  /'
rm -rf -- "$audit_dir"

exit $status
