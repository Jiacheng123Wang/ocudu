"""Harvest hard samples in the shape the mother set already has: rv0 captures, split by whether they needed a donor.

WHY THE FLOW IS TWO STAGES. An rv0 reception either decodes on its own or it does not. If it does -- at the raised cap --
the sample is a `self` sample and its retransmissions are irrelevant: the user's own observation is that once rv0 succeeds
the donor need not be decoded at all. If it does not, the reception can still become a `harq` sample, but only through the
soft combined buffer: the rv0 capture plus one retransmission of the same block, replayed in one process.

THE RETRANSMISSIONS ARE NEVER SAMPLES. They carry no label of their own; they exist so their rv0 can decode. That is how
the mother set was built, so this harvest does not invent a third population.

WHY THE CANDIDATES ARE rv0 AND KO. A retransmission replayed alone cannot even reserve its HARQ soft buffer (the pool has
no entry for an identifier it never created, and `rx_buffer_pool_impl::reserve` returns an invalid buffer for a
non-new-data reservation without a match), so replaying retransmissions as candidates is pure waste.

Usage: harvest_hard.py --leg <name> --corpus <dir> --index <index.csv> --out <dir> --bin <replay> [--cap 20] [--jobs 8]
"""
import argparse
import collections
import csv
import glob
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

LINE = re.compile(r"\[0\].*?crc=(\w+).*?iterations=(\d+).*?iterations_max=(\d+)")


def pdu_fields(path):
    want = ("rnti", "harq_id", "rv", "new_data", "slot", "scs_khz")
    out = {}
    try:
        with open(path, errors="ignore") as fh:
            for line in fh:
                key, sep, value = line.strip().partition("=")
                if sep and key in want:
                    out[key] = value
    except OSError:
        return None
    return out


def ordinate(name):
    """A monotonic coordinate: frames are monotonic, slot numbers wrap, and the capture slot trails the grant slot."""
    try:
        _, frame, slot, _ = name.split("_")
        return int(frame) * 20 + (int(slot) % 20)
    except (ValueError, IndexError):
        return None


def replay(binary, base, work_dir, cap, also=None):
    """One reception (optionally soft combined with a donor). Returns (crc, iterations, iterations_max, tb_path)."""
    os.makedirs(work_dir, exist_ok=True)
    env = dict(os.environ, OCUDU_REPLAY_DEC_ITERATIONS=str(cap), OMP_NUM_THREADS="1",
               OCUDU_HELENA_DUMP_DIR=work_dir)
    cmd = [binary, base, "--phy_pipeline", "cpu", "--pusch_receiver_backend", "classic",
           "--out", os.path.join(work_dir, "stage")]
    if also:
        cmd += ["--also", also, "--harq-chain"]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    crc, it, itmax = None, -1, -1
    for line in proc.stdout.splitlines():
        m = LINE.search(line)
        if m:
            crc, it, itmax = m.group(1), int(m.group(2)), int(m.group(3))
            break
    tb = next(iter(glob.glob(os.path.join(work_dir, "tb_*.bits"))), "")
    return crc, it, itmax, tb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--index", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--cap", type=int, default=20)
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.index)))
    units = {}
    for r in rows:
        pdu = pdu_fields(os.path.join(args.corpus, r["unit"] + ".txt"))
        if pdu is None:
            continue
        units[r["unit"]] = dict(pdu, unit=r["unit"], crc=r["crc"], ord=ordinate(r["unit"]))

    # THE CHAINS: rv=0 opens a block and a following transmission with rv != 0 belongs to it. A retransmission whose
    # first transmission was not captured opens nothing and can supply no label.
    chains = collections.defaultdict(list)
    for u in units.values():
        chains[(u.get("rnti"), u.get("harq_id"))].append(u)
    donor_of = {}
    for members in chains.values():
        members = sorted([m for m in members if m["ord"] is not None], key=lambda m: m["ord"])
        head = None
        for m in members:
            if m.get("rv") == "0":
                head = m
            elif head is not None and m["unit"] not in donor_of:
                donor_of[head["unit"]] = m["unit"]

    candidates = [u for u in units.values() if u["crc"] != "OK" and u.get("new_data") == "1"]
    print(f"{args.leg}: {len(rows)} units, {sum(1 for r in rows if r['crc'] != 'OK')} without a verdict, "
          f"{len(candidates)} rv0 candidates, {len(donor_of)} chains with a donor", flush=True)

    out_tb = os.path.join(args.out, "tb")
    os.makedirs(out_tb, exist_ok=True)
    stage = os.path.join(args.out, "stage")
    os.makedirs(stage, exist_ok=True)

    def work(u):
        base = os.path.join(args.corpus, u["unit"])
        work_dir = os.path.join(stage, u["unit"])
        crc, it, itmax, tb = replay(args.bin, base, work_dir, args.cap)
        population = "self" if crc == "OK" else ""
        if crc != "OK" and u["unit"] in donor_of:
            crc2, it2, itmax2, tb2 = replay(args.bin, base, work_dir + "_chain", args.cap,
                                            also=os.path.join(args.corpus, donor_of[u["unit"]]))
            if crc2 == "OK":
                population, crc, it, itmax, tb = "harq", crc2, it2, itmax2, tb2
            shutil.rmtree(work_dir + "_chain", ignore_errors=True)
        if population and tb:
            shutil.copyfile(tb, os.path.join(out_tb, u["unit"] + ".bits"))
        shutil.rmtree(work_dir, ignore_errors=True)
        return u["unit"], population, crc or "", it, itmax, u.get("rv", ""), u.get("harq_id", ""), u["unit"] in donor_of

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for i, r in enumerate(pool.map(work, candidates), 1):
            results.append(r)
            if i % 200 == 0:
                print(f"  {i}/{len(candidates)}", flush=True)

    with open(os.path.join(args.out, "index_hard.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["unit", "population", "crc", "iterations", "iterations_max", "rv", "harq_id", "has_donor"])
        w.writerows(sorted(results))
    counts = collections.Counter(r[1] or "none" for r in results)
    print(f"  populations: {dict(counts)}  bits: {len(os.listdir(out_tb))}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
