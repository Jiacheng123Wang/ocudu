#!/usr/bin/env python3
"""Stream-merge contiguous chunk builds of ONE (modulation, source) dataset into one npz.

WHY THIS IS NOT merge_depth3_datasets.py. That tool loads every input and then concatenates each
key, so the parts and the merged copy exist together: at 100k samples that is ~28 GB and it would
reintroduce the OOM that chunking exists to avoid. This tool writes the npz member by member and
copies each part's rows straight into the output member. Peak memory is one block plus the small
per-sample columns, not the dataset.

WHAT IT PRESERVES. The parts must be contiguous slices, in order, of the same build (build_mother_set
creates them that way), so concatenating them reproduces the unchunked row order exactly. Keys,
dtypes and per-sample shapes must agree; `bits`/`tb_bits` are padded to the widest part, exactly as
the single build pads them to the widest sample. A truncated or mismatched part is an error.

Usage: merge_depth3_parts.py --out NAME.npz NAME_part0.npz NAME_part1.npz ...
"""
import argparse
import io
import os
import sys
import zipfile

import numpy as np

BLOCK = 8 << 20


def read_header(stream):
    version = np.lib.format.read_magic(stream)
    if version == (1, 0):
        shape, fortran, dtype = np.lib.format.read_array_header_1_0(stream)
    elif version == (2, 0):
        shape, fortran, dtype = np.lib.format.read_array_header_2_0(stream)
    else:
        raise SystemExit(f"unsupported .npy header version {version}")
    return shape, fortran, dtype


def inspect(path):
    with zipfile.ZipFile(path) as archive:
        keys = [name[:-4] for name in archive.namelist() if name.endswith(".npy")]
        info = {}
        for key in keys:
            with archive.open(key + ".npy") as stream:
                shape, fortran, dtype = read_header(stream)
            if fortran:
                raise SystemExit(f"{path}: {key} is Fortran-ordered; this tool only writes C order")
            info[key] = (shape, dtype)
    if "X" not in info:
        raise SystemExit(f"{path}: no X array")
    return keys, info


def copy_numeric(out, path, key, dtype, part_width, total_width):
    """Copy one numeric member, padding each row to total_width when part_width is not None."""
    with zipfile.ZipFile(path) as archive, archive.open(key + ".npy") as source:
        read_header(source)
        if part_width is None:
            while True:
                chunk = source.read(BLOCK)
                if not chunk:
                    break
                out.write(chunk)
            return
        row_bytes = part_width * dtype.itemsize
        rows_per = max(1, BLOCK // row_bytes)
        while True:
            chunk = source.read(rows_per * row_bytes)
            if not chunk:
                break
            rows = np.frombuffer(chunk, dtype=dtype)
            if rows.size % part_width:
                raise SystemExit(f"{path}: {key} stream is not a whole number of rows")
            rows = rows.reshape(-1, part_width)
            padded = np.zeros((rows.shape[0], total_width), dtype=dtype)
            padded[:, :part_width] = rows
            out.write(padded.tobytes())


def write_object(out, parts, key):
    arrays = [np.load(path, allow_pickle=True)[key] for path in parts]
    merged = np.concatenate(arrays)
    buffer = io.BytesIO()
    np.lib.format.write_array(buffer, merged, allow_pickle=True)
    out.write(buffer.getvalue())


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    parser.add_argument("parts", nargs="+")
    args = parser.parse_args()
    if len(args.parts) < 2:
        raise SystemExit("need at least two parts")

    first_keys, reference = inspect(args.parts[0])
    rows = 0
    infos = []
    for path in args.parts:
        keys, info = inspect(path)
        if keys != first_keys:
            raise SystemExit(f"REFUSING: {path} carries {keys}, {args.parts[0]} carries {first_keys}")
        for key in keys:
            got, want = info[key], reference[key]
            if got[1].str != want[1].str:
                raise SystemExit(f"REFUSING: {path} {key} dtype {got[1]}, expected {want[1]}")
            same_tail = len(got[0]) == len(want[0]) and all(
                a == b for a, b in zip(got[0][1:], want[0][1:]))
            if not same_tail and key not in ("bits", "tb_bits"):
                raise SystemExit(f"REFUSING: {path} {key} shape {got[0]}, expected {want[0]}")
            if len(got[0]) != len(want[0]):
                raise SystemExit(f"REFUSING: {path} {key} rank differs")
        n = info["X"][0][0]
        if n <= 0:
            raise SystemExit(f"REFUSING: {path} has no samples")
        rows += n
        infos.append((path, n, info))

    widths = {}
    for key in first_keys:
        shape = reference[key][0]
        widths[key] = max(info[key][0][1] for _, _, info in infos) if len(shape) > 1 else None

    tmp = args.out + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
        for key in first_keys:
            shape, dtype = reference[key]
            tail = (widths[key],) if key in ("bits", "tb_bits") else tuple(shape[1:])
            final_shape = (rows,) + tail
            with archive.open(key + ".npy", "w", force_zip64=True) as out:
                if dtype == object:
                    write_object(out, args.parts, key)
                    continue
                np.lib.format.write_array_header_1_0(
                    out, {"descr": np.lib.format.dtype_to_descr(dtype),
                          "fortran_order": False, "shape": final_shape})
                for path, n, info in infos:
                    if key in ("bits", "tb_bits"):
                        copy_numeric(out, path, key, dtype,
                                     info[key][0][1], widths[key])
                    else:
                        copy_numeric(out, path, key, dtype, None, None)
    os.replace(tmp, args.out)
    print(f"merged {len(args.parts)} part(s) -> {args.out}: {rows} samples, keys={len(first_keys)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
