#!/bin/bash
# Give a training run its canonical name while it is still running, without splitting it in two.
#
# WHY A SYMLINK AND NOT JUST mv. The trainer holds the output path as a string and writes every snapshot into it, so moving
# the directory away from under a live run makes it recreate the old path and the epochs then land in two places. Renaming
# and leaving a symlink of the old name pointing at the new directory keeps those writes flowing into the canonical dir.
#
# usage: rename_run.sh <runs-root> <old-dir-name> <new-name>
#   writes <new-name>/model.json from the run's own `argv:` line, per model_naming_convention.md section 3.
set -u
ROOT=${1:?runs root}
OLD=${2:?old dir name}
NEW=${3:?canonical name}
[ -d "$ROOT/$OLD" ] || { echo "no such run: $ROOT/$OLD" >&2; exit 1; }
if [ ! -L "$ROOT/$OLD" ]; then
  mv "$ROOT/$OLD" "$ROOT/$NEW"
  ln -s "$NEW" "$ROOT/$OLD"
fi
LOG=$HOME/work/ai_phy_train/logs/$OLD.log
[ -f "$LOG" ] || LOG=$HOME/work/ai_phy_train/logs/$NEW.log
python3 - "$ROOT/$NEW" "$NEW" "$OLD" "$LOG" <<'PY'
import json, os, re, sys
out, name, old, log = sys.argv[1:5]
argv = ""
if os.path.exists(log):
    # THE LAST argv, NOT THE FIRST. A log file accumulates every attempt that wrote to it -- a start that failed on a missing
    # path, a restart with a corrected dataset -- so the first argv describes whatever was tried first, not what is running.
    for line in open(log, errors="replace"):
        if line.startswith("argv:"):
            argv = line[5:].strip()
def arg(flag, default=None):
    m = re.search(rf"--{flag}\s+(\S+)", argv)
    return m.group(1) if m else default
doc = {
    "name": name,
    "aliases": [old],
    "dataset": {"train": os.path.basename(arg("train", "").rstrip("/")),
                "val": os.path.basename(arg("val", "").rstrip("/"))},
    "arch": {"c_hid": int(arg("c-hid", 0)), "blocks": int(arg("blocks", 0)),
             "layout": arg("layout", "spatial"),
             "dense": "--dense-blocks" in argv,
             "dense_bottleneck": int(arg("dense-bottleneck", 0))},
    "train": {"batch": int(arg("batch", 0)), "lr": float(arg("lr", 1e-3)),
              "epochs_planned": int(arg("epochs", 0)), "seed": int(arg("seed", 0)),
              "init_from": arg("init")},
    "measured": {},
}
path = os.path.join(out, "model.json")
with open(path, "w") as f:
    json.dump(doc, f, indent=2)
    f.write("\n")
print(f"wrote {path}")
PY
echo "renamed: $OLD -> $NEW (symlink left in place for the live run)"
