#!/bin/bash
# Move finished per-modality npz files for one leg from datasets_bin to datasets_full_w2.
# If the destination already has the same name (known fragment/partial leftovers), the old
# file is quarantined (moved aside, never deleted) and the fresh file takes its place.
# Refuses to replace when the fresh file is smaller than the existing one, or when the fresh
# file is not a loadable npz with the expected keys.
leg="$1"
RH="${DSH_RH:-$HOME/work/ai_phy_train}"
PY_BIN="${DSH_PY:-$HOME/work/ai_phy_train/.venv/bin/python}"
cd "$RH" || { echo "CD_FAIL $RH"; exit 2; }
dst=data/datasets_full_w2
qdir=data/datasets_full_w2_replaced_fragments
shopt -s nullglob
files=(datasets_bin/w2_harq_"$leg"_*.npz)
if [ ${#files[@]} -eq 0 ]; then
  echo "NO_FILES $leg"
  exit 0
fi
for f in "${files[@]}"; do
  base=$(basename "$f")
  sz=$(stat -c %s "$f")
  check=$("$PY_BIN" - "$f" <<'PY'
import sys
import numpy as np
need = {"X", "H", "bits", "tb_bits", "qm", "snr_nominal"}
try:
    d = np.load(sys.argv[1])
    miss = sorted(need - set(d.files))
    print("NPZ_OK" if not miss else "NPZ_MISSING:" + ",".join(miss))
except Exception as e:  # noqa: BLE001 - report any failure, do not move
    print("NPZ_ERR:" + type(e).__name__)
PY
)
  if [ "$check" != "NPZ_OK" ]; then
    echo "FRESH_INVALID $base $check size=$sz (left in place)"
    continue
  fi
  if [ -e "$dst/$base" ]; then
    dsz=$(stat -c %s "$dst/$base")
    if [ "$sz" -lt "$dsz" ]; then
      echo "REFUSED_FRESH_SMALLER $base fresh=$sz existing=$dsz (nothing changed)"
      continue
    fi
    mkdir -p "$qdir" || { echo "QUARANTINE_MKDIR_FAIL $base"; continue; }
    q="$qdir/$base"
    [ -e "$q" ] && q="$qdir/$base.$(date +%s)"
    if ! mv -- "$dst/$base" "$q"; then
      echo "QUARANTINE_MOVE_FAIL $base"
      continue
    fi
    if [ -e "$dst/$base" ]; then
      echo "QUARANTINE_VERIFY_FAIL $base"
      continue
    fi
    if mv -- "$f" "$dst/$base"; then
      nsz=$(stat -c %s "$dst/$base" 2>/dev/null)
      if [ "$nsz" = "$sz" ]; then
        echo "REPLACED $base $sz (old fragment $dsz quarantined)"
      else
        echo "SIZE_MISMATCH $base recorded=$sz actual=${nsz:-MISSING}"
      fi
    else
      mv -- "$q" "$dst/$base" && echo "FRESH_MOVE_FAIL_RESTORED $base"
      continue
    fi
  else
    if mv -- "$f" "$dst/$base"; then
      nsz=$(stat -c %s "$dst/$base" 2>/dev/null)
      if [ "$nsz" = "$sz" ]; then
        echo "MOVED $base $sz"
      else
        echo "SIZE_MISMATCH $base recorded=$sz actual=${nsz:-MISSING}"
      fi
    else
      echo "MOVE_FAIL $base"
      continue
    fi
  fi
  if [ -e "$f" ]; then echo "SRC_REMAINS $base"; fi
done
exit 0
