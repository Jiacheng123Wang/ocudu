"""Split a leg's captures into one folder per modulation, moving each capture's live dump with it.

WHY. Two runs of these legs landed in one folder -- the first at 64QAM, the second at 256QAM -- because the corpus path was not changed between them. And
a scatter of captures in both runs belongs to neither: several hundred distinct RNTIs with one or two samples each, which is other devices' uplink traffic
the gNB received incidentally. Splitting by the modulation the PDU reports puts every sample where its own label says it belongs, and keeps the incidental
traffic out of the legs that will be trained on.

The live dump is keyed by the index in `dd_meta.csv`, and each capture's index is looked up in that table, so the reception grids and transport blocks
follow their capture.

Usage: split_leg_by_modulation.py <leg-dir> <output-root> [--dry]
   Output directories are <output-root>/<leg-name-without-modulation>_<MODULATION>, and the modulation is removed from the original name if present.
"""
import csv
import json
import os
import re
import sys
import collections

MODS = ("QPSK", "16QAM", "64QAM", "256QAM")
leg_dir = sys.argv[1].rstrip("/")
out_root = sys.argv[2]
dry = "--dry" in sys.argv[3:]

leg_name = os.path.basename(leg_dir)
# "w2_harq_64QAM_ping_1030a" -> base "w2_harq_ping_1030a": the modulation in the name is what we are about to replace with the truth.
base = leg_name
for m in MODS:
    base = base.replace(f"_{m}_", "_")
    if base.startswith(f"{m}_"):
        base = base[len(m) + 1:]

# 1. Classify every file by the modulation in its capture's PDU. A capture is four files sharing a stem --
#    <stem>.txt (the PDU), <stem>.bin (the grid), <stem>_h.bin (the channel), <stem>_ce.txt (the estimator sidecar) -- and they must travel together.
def stem_of(name):
    for suffix in ("_ce.txt", "_h.bin", "_llr.bin", "_bits.bin", ".bin", ".txt"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return None

by_mod = collections.defaultdict(list)
for name in os.listdir(leg_dir):
    path = os.path.join(leg_dir, name)
    if not os.path.isfile(path) or not name.startswith("cap_"):
        continue
    stem = stem_of(name)
    if stem is None:
        by_mod["UNKNOWN"].append(name)
        continue
    mod = None
    pdu = os.path.join(leg_dir, stem + ".txt")
    try:
        for line in open(pdu, errors="ignore"):
            k, sep, v = line.strip().partition("=")
            if sep and k == "modulation":
                mod = v
                break
    except OSError:
        pass
    by_mod[mod if mod in MODS else "UNKNOWN"].append(name)

print(f"{leg_name}: " + "  ".join(f"{m}={len(v)} files" for m, v in sorted(by_mod.items())))
if dry:
    sys.exit(0)

# 2. The live index, so that reception files travel with their capture.
meta_rows = {}
meta_path = os.path.join(leg_dir, "tb_live", "dd_meta.csv")
if os.path.isfile(meta_path):
    with open(meta_path, newline="") as handle:
        rows = list(csv.reader(handle))
    for r in rows[1:]:
        if len(r) >= 6:
            meta_rows[int(r[0])] = r

for mod, files in by_mod.items():
    if mod == "UNKNOWN":
        continue
    # Keep the corpus convention, which is <prefix>_<MODULATION>_<traffic>_<date>: the modulation goes where it belongs in the name.
    if base.startswith("w2_harq_"):
        dest_name = f"w2_harq_{mod}_{base[len('w2_harq_'):]}"
    else:
        dest_name = f"{base}_{mod}"
    dest = os.path.join(out_root, dest_name)
    # A LEG NAMED FOR ITS DOMINANT MODULATION ALREADY IS THAT FOLDER, so its own captures stay where they are; only the minority modulations move out.
    if os.path.abspath(dest) == os.path.abspath(leg_dir):
        for name in files:
            continue
        print(f"  {mod}: {len(files)} files stay (this folder is already {dest_name})")
        remaining = True
    else:
        os.makedirs(dest, exist_ok=True)
        for name in files:
            os.rename(os.path.join(leg_dir, name), os.path.join(dest, name))
        remaining = False

    # The live dump: match captures to index rows by (frame, slot), then move the files that index names.
    takes = set()
    slots_by_frame = collections.defaultdict(set)
    for name in files:
        m = re.match(r"^cap_(\d+)_(\d+)_", name)
        if m:
            slots_by_frame[int(m.group(1))].add(int(m.group(2)))
    for idx, r in meta_rows.items():
        try:
            frame, slot = int(r[4]), int(r[5])
        except ValueError:
            continue
        if slot in slots_by_frame.get(frame, ()):
            takes.add(idx)
    if remaining:
        continue
    if not takes:
        print(f"  {mod}: {len(files)} files moved, no live rows matched")
        continue
    os.makedirs(os.path.join(dest, "tb_live"), exist_ok=True)
    keep, take = [], []
    for idx, r in meta_rows.items():
        (take if idx in takes else keep).append(r)
    with open(os.path.join(dest, "tb_live", "dd_meta.csv"), "w", newline="") as h:
        w = csv.writer(h); w.writerow(["idx", "ts_us", "tb_bits", "nof_cb", "frame", "slot"]); w.writerows(sorted(take))
    src_tb = os.path.join(leg_dir, "tb_live")
    moved = 0
    for name in os.listdir(src_tb):
        parts = name.split("_")
        if len(parts) < 2 or parts[0] not in ("rx", "tb"):
            continue
        try:
            idx = int(parts[1])
        except ValueError:
            continue
        if idx in takes:
            os.rename(os.path.join(src_tb, name), os.path.join(dest, "tb_live", name))
            moved += 1
    print(f"  {mod}: {len(files)} files, {len(take)} live rows, {moved} live files")

print("done")
