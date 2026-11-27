#!/usr/bin/env python3
"""Replay the units of an existing dump that produced no verdict, and fold the answers back into it.

WHY. A dump is one process per unit, and under load a process occasionally dies before printing its verdict: the unit's
index row then holds no SINR and, if the unit is a first transmission, no label either. The reception itself is fine; running
it again decodes it (measured on the w2 legs 2026-11-27: four of ten thousand units, every one of them decoding on the
retry). A dump made before dump_tb.sh retried these holes by itself therefore carries a handful of lost samples.

WHAT IT TOUCHES. Only the rows it replayed: the index line is rewritten in place, in the same order, and the recovered
transport block is written to <out>/tb/<unit>.bits. The three population indexes are left stale on purpose, because they
are the labeller's output; run harq_backfill.py afterwards to rebuild them.

A unit is retried when its row has no SINR and its sidecar says new_data=1. A retransmission has no soft buffer to reserve
and fails the same way every time, so it is skipped.

Usage: retry_holes.py --leg <capture dir> --out <dump dir> [--binary PATH] [--jobs N] [--limit N]
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROW = re.compile(r"^\s*\[0\] tbs=(\d+) slot=(\d+) rnti=(\d+) (\S+): crc=(\w+) iterations=(\d+) iterations_max=(\d+) sinr=([-\d.]+) dB")


def new_data_of(unit_txt):
    with open(unit_txt, errors="ignore") as handle:
        for line in handle:
            if line.startswith("new_data="):
                return line.strip().split("=", 1)[1]
    return "?"


def replay(binary, unit, workdir):
    """One unit, one process; returns the parsed verdict line, the TB file and its byte count."""
    bits_dir = os.path.join(workdir, "bits")
    os.makedirs(bits_dir, exist_ok=True)
    env = dict(os.environ)
    env["OCUDU_HELENA_DUMP_DIR"] = bits_dir
    env["OMP_NUM_THREADS"] = "1"
    proc = subprocess.run([binary, unit, "--phy_pipeline", "cpu", "--pusch_receiver_backend", "classic",
                           "--out", os.path.join(workdir, "stage")],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=600)
    for line in proc.stdout.splitlines():
        match = ROW.search(line)
        if match:
            tb = [os.path.join(bits_dir, f) for f in os.listdir(bits_dir) if f.startswith("tb_") and f.endswith(".bits")]
            path = tb[0] if tb else ""
            return match, path
    return None, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True, help="capture directory")
    ap.add_argument("--out", required=True, help="dump directory to repair")
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                     "..", "..", "..", "..", "..", "..",
                                                     "build", "lib", "phy", "upper", "channel_processors",
                                                     "ul_chain_replay_cpu"))
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--limit", type=int, default=0, help="only the first N holes (smoke)")
    args = ap.parse_args()

    binary = os.path.normpath(args.binary)
    index_path = os.path.join(args.out, "index.csv")
    lines = open(index_path).read().splitlines()
    header, body = lines[0], lines[1:]
    cols = header.split(",")
    at = {name: i for i, name in enumerate(cols)}

    holes = []
    for line in body:
        fields = line.split(",")
        if fields[at["sinr_db"]] != "":
            continue
        unit = fields[at["unit"]]
        sidecar = os.path.join(args.leg, unit + ".txt")
        if os.path.exists(sidecar) and new_data_of(sidecar) == "1":
            holes.append((line, unit))
    if args.limit:
        holes = holes[:args.limit]
    print(f"{len(body)} row(s), {len(holes)} hole(s) to replay", file=sys.stderr)

    fixed = 0
    def one(hole):
        line, unit = hole
        workdir = tempfile.mkdtemp(prefix="retry_")
        try:
            match, bits = replay(binary, os.path.join(args.leg, unit), workdir)
            if match is None:
                return None
            tbs, slot, _rnti, _mod, crc, iters, iters_max, sinr = match.groups()
            fields = line.split(",")
            bits_size = os.path.getsize(bits) if bits and os.path.exists(bits) else 0
            if bits_size:
                shutil.copyfile(bits, os.path.join(args.out, "tb", unit + ".bits"))
            fields[at["tb_bits"]] = str(bits_size * 8)
            fields[at["crc"]] = crc
            fields[at["sinr_db"]] = sinr
            fields[at["iterations"]] = iters
            if "iterations_max" in at:
                fields[at["iterations_max"]] = iters_max
            fields[at["slot"]] = slot
            fields[at["tbs_bytes"]] = tbs
            return ",".join(fields)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    replacement = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for (line, unit), answer in zip(holes, pool.map(one, holes)):
            if answer is None:
                continue
            replacement[unit] = answer
            fixed += 1

    if replacement:
        patched = [replacement.get(line.split(",")[at["unit"]], line) for line in body]
        with open(index_path + ".tmp", "w") as handle:
            handle.write("\n".join([header] + patched) + "\n")
        os.replace(index_path + ".tmp", index_path)
    print(f"replayed {fixed} hole(s); index rewritten in place", file=sys.stderr)
    print("now re-run harq_backfill.py to rebuild index_self / index_harq / index_ko", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
