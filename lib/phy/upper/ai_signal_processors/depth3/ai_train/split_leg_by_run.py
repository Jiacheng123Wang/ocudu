"""Split a leg written by two runs into two legs, using the captures' frames as the boundary.

The first run of these legs is 64QAM and the second is 256QAM, but both landed in one folder because the corpus path was not changed between them. The
captures carry their frame in the name and the live index carries it in a column, so the boundary is found once in the captures and then applied to the
index and to the reception and transport-block files it points at, by index.

Everything is os.rename: same filesystem, no copying, no extra space.
"""
import csv
import json
import os
import re
import shutil
import sys
import datetime

leg_dir, new_dir, boundary = sys.argv[1], sys.argv[2], sys.argv[3]
dry = len(sys.argv) > 4 and sys.argv[4] == "--dry"
boundary_ts = datetime.datetime.strptime(boundary, "%Y-%m-%d %H:%M:%S").timestamp()
cap_re = re.compile(r"^cap_(\d+)_(\d+)_(\d+)(_.*)?$")

moved, first_frame = [], None
for name in os.listdir(leg_dir):
    path = os.path.join(leg_dir, name)
    if not os.path.isfile(path) or not name.startswith("cap_"):
        continue
    if os.path.getmtime(path) < boundary_ts:
        continue
    m = cap_re.match(name)
    if m:
        f = int(m.group(1))
        first_frame = f if first_frame is None else min(first_frame, f)
    moved.append(name)
if first_frame is None:
    print("nothing after the boundary"); sys.exit(1)
print(f"{len(moved)} capture files after the boundary; first frame of run 2 = {first_frame}")
if dry:
    sys.exit(0)

os.makedirs(new_dir, exist_ok=True)
for name in moved:
    os.rename(os.path.join(leg_dir, name), os.path.join(new_dir, name))

old_logs = os.path.join(leg_dir, "logs")
if os.path.isdir(old_logs):
    os.makedirs(os.path.join(new_dir, "logs"), exist_ok=True)
    for name in os.listdir(old_logs):
        p = os.path.join(old_logs, name)
        if os.path.isfile(p) and os.path.getmtime(p) >= boundary_ts:
            os.rename(p, os.path.join(new_dir, "logs", name))

old_tb = os.path.join(leg_dir, "tb_live")
if os.path.isdir(old_tb):
    os.makedirs(os.path.join(new_dir, "tb_live"), exist_ok=True)
    meta = os.path.join(old_tb, "dd_meta.csv")
    keep, take_idx = [], set()
    with open(meta, newline="") as h:
        rows = list(csv.reader(h))
    header, body = rows[0], [r for r in rows[1:] if len(r) >= 6]
    for r in body:
        if int(r[4]) >= first_frame:
            take_idx.add(int(r[0]))
        else:
            keep.append(r)
    with open(meta, "w", newline="") as h:
        w = csv.writer(h); w.writerow(header); w.writerows(keep)
    take = [r for r in body if int(r[0]) in take_idx]
    with open(os.path.join(new_dir, "tb_live", "dd_meta.csv"), "w", newline="") as h:
        w = csv.writer(h); w.writerow(header); w.writerows(take)
    print(f"dd_meta: kept {len(keep)}, moved {len(take)}")

    # The reception grids and the transport blocks are named by index.
    names = os.listdir(old_tb)
    n_moved = 0
    for name in names:
        parts = name.split("_")
        if len(parts) < 2 or parts[0] not in ("rx", "tb"):
            continue
        try:
            idx = int(parts[1])
        except ValueError:
            continue
        if idx in take_idx:
            os.rename(os.path.join(old_tb, name), os.path.join(new_dir, "tb_live", name))
            n_moved += 1
    print(f"tb_live files moved: {n_moved}")

    rec = os.path.join(old_tb, "record.jsonl")
    if os.path.isfile(rec):
        keep_lines, take_lines = [], []
        for line in open(rec, errors="ignore"):
            try:
                raw = json.loads(line)
                (take_lines if int(raw.get("idx", -1)) in take_idx else keep_lines).append(line)
            except Exception:
                keep_lines.append(line)
        open(rec, "w").writelines(keep_lines)
        open(os.path.join(new_dir, "tb_live", "record.jsonl"), "w").writelines(take_lines)
        print(f"record.jsonl: kept {len(keep_lines)}, moved {len(take_lines)}")

print("done")
