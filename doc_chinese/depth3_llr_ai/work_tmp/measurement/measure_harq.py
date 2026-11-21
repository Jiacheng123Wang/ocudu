"""Replay the HARQ-only units of a leg through an arm, combining each retransmission with its donor.

WHY. The PK's "vs CPU+HARQ" column compares the classical chain WITH HARQ combining against the AI arm WITHOUT it, which
is not like-for-like: the CPU baseline adds 10,716,400 bits that only exist because a retransmission was combined with
the soft bits of the earlier one (3,496 units of the 1102a leg). The replay binary supports exactly that situation for
any backend ("--also <base> --harq-chain replays several transmissions of ONE HARQ process through ONE processor"), so
the missing measurement is mechanical rather than architectural -- it was simply never run for the AI arm.

USAGE
    python measure_harq.py <index_harq_only.csv> <bundle.mlmodelc> <out.csv> <corpus_dir> [limit]

The output has the same (unit, status, bits) shape as the full-leg CSV, so it can be summed with it: AI+HARQ total =
AI self total + the OK bits written here.
"""
import csv
import os
import subprocess
import sys

BIN = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
index, model, out_csv, corpus = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
limit = int(sys.argv[5]) if len(sys.argv) > 5 else 0

rows = list(csv.DictReader(open(os.path.expanduser(index))))
if limit:
    rows = rows[:limit]

written = 0
with open(out_csv, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["unit", "status", "bits"])
    for r in rows:
        unit, donor = r["unit"], r["donor"]
        env = dict(os.environ, OCUDU_DEPTH3_FALLBACK="none", OCUDU_REPLAY_MODEL=model)
        cmd = [BIN, os.path.join(corpus, unit), "--out", "/tmp/_harq",
               "--phy_pipeline", "npu", "--pusch_receiver_backend", "ai",
               "--pusch_ldpc_decoder_type", "generic",
               "--also", os.path.join(corpus, donor), "--harq-chain"]
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
        ok = "crc=OK" in proc.stdout
        bits = 0
        if ok:
            for line in proc.stdout.splitlines():
                if "crc=OK" in line and "tbs=" in line:
                    try:
                        bits = int(line.split("tbs=")[1].split()[0]) * 8
                    except (IndexError, ValueError):
                        bits = 0
                    break
        w.writerow([unit, "OK" if ok else "KO", bits])
        written += 1
        if written % 100 == 0:
            print(f"{written}/{len(rows)}", flush=True)
print(f"{out_csv}: n={len(rows)}")
