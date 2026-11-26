"""Pilot for the hard-sample harvest: replay KO units at two LDPC iteration caps and count what the higher cap cracks.

WHY THIS IS THE WHOLE EXPERIMENT. The corpus's `iterations` column stops at six because the air leg ran with a cap of
six; a reception that would decode on its seventh iteration is `crc=KO` there and therefore has no label. Raising the
replay's cap does not invent labels: the decoder either decodes the block (and then its bits ARE the ground truth) or it
does not. So the fraction that flips KO -> OK at cap 20 is exactly the harvest rate, and those units arrive with an
`iterations` value between 7 and 20 -- a difficulty label the corpus does not currently contain.

Two caps on the SAME units is what makes it a measurement rather than a re-dump: any unit that is OK at six must also
be OK at twenty, so the only interesting cells are the ones that change.

Usage: pilot_harvest.py <dump index.csv> <corpus dir> [--units 1000] [--jobs 6] [--caps 6,20]
"""
import argparse
import collections
import csv
import os
import random
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

BIN = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
OUT = "/tmp/_pilot_harvest"

LINE = re.compile(r"\[0\].*?crc=(\w+).*?iterations=(\d+)")


def one(job):
    unit, cap, corpus = job
    env = dict(os.environ, OCUDU_REPLAY_DEC_ITERATIONS=str(cap), OMP_NUM_THREADS="1")
    work = os.path.join(OUT, f"w_{cap}_{unit}")
    os.makedirs(work, exist_ok=True)
    cmd = [BIN, os.path.join(corpus, unit),
           "--phy_pipeline", "cpu", "--pusch_receiver_backend", "classic",
           "--out", os.path.join(work, "stage")]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=dict(env, OCUDU_HELENA_DUMP_DIR=work))
    crc, iters = "no", ""
    for line in proc.stdout.splitlines():
        m = LINE.search(line)
        if m:
            crc, iters = m.group(1), m.group(2)
            break
    subprocess.run(["rm", "-rf", work])
    return unit, cap, crc, iters, proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("index")
    ap.add_argument("corpus")
    ap.add_argument("--units", type=int, default=1000)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--caps", default="6,20")
    ap.add_argument("--seed", type=int, default=20261126)
    args = ap.parse_args()

    caps = [int(c) for c in args.caps.split(",")]
    rows = [r for r in csv.DictReader(open(args.index)) if r.get("crc") != "OK"]
    random.seed(args.seed)
    sample = random.sample(rows, min(args.units, len(rows)))
    units = [r["unit"] for r in sample]
    print(f"{len(rows)} KO units in the index; replaying {len(units)} at caps {caps}", flush=True)

    results = collections.defaultdict(dict)
    jobs = [(u, c, args.corpus) for u in units for c in caps]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        done = 0
        for unit, cap, crc, iters, rc in pool.map(one, jobs):
            results[unit][cap] = (crc, iters, rc)
            done += 1
            if done % 200 == 0:
                print(f"  {done}/{len(jobs)}", flush=True)

    low, high = caps[0], caps[-1]
    flipped, stayed, regressed = [], [], []
    for u in units:
        a = results[u].get(low, ("no", "", 0))[0]
        b = results[u].get(high, ("no", "", 0))[0]
        if a != "OK" and b == "OK":
            flipped.append((u, results[u][high][1]))
        elif a != "OK" and b != "OK":
            stayed.append(u)
        elif a == "OK" and b == "no":
            regressed.append(u)

    print(f"\ncap {low}: OK {sum(1 for u in units if results[u][low][0] == 'OK')}/{len(units)}")
    print(f"cap {high}: OK {sum(1 for u in units if results[u][high][0] == 'OK')}/{len(units)}")
    print(f"★ flipped KO->OK: {len(flipped)} ({100*len(flipped)/max(len(units),1):.2f}%)")
    print(f"  stayed KO: {len(stayed)}   regressed OK->KO: {len(regressed)}")
    if flipped:
        dist = collections.Counter(it for _, it in flipped)
        print(f"  iterations of the flipped units: {dict(sorted(dist.items(), key=lambda kv: int(kv[0])))}")
    pairs = collections.Counter((results[u][low][0], results[u][high][0]) for u in units)
    print(f"  pairs (cap{low}, cap{high}): {dict(pairs)}")
    with open("/tmp/pilot_harvest_pairs.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["unit", f"crc{low}", f"it{low}", f"crc{high}", f"it{high}"])
        for u in units:
            w.writerow([u, results[u][low][0], results[u][low][1], results[u][high][0], results[u][high][1]])
    with open("/tmp/pilot_harvest_flipped.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["unit", "iterations_at_high_cap"])
        w.writerows(flipped)


if __name__ == "__main__":
    sys.exit(main())
