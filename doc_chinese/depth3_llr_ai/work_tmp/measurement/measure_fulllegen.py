"""Run one arm over the FULL leg (all units of the 1102a dump), not the 5 000-unit ruler subset.

WHY A SEPARATE SCRIPT. `measure_arm.py` hardcodes the ruler corpus (5 000 captures) because that is what the PK's
shared number is defined on. The full leg needs the whole corpus, which lives on the replay box only (8.6 GB), so the
corpus and the unit list are arguments here instead of constants.

Usage:
    python measure_fulllegen.py <units.txt> <bundle.mlmodelc> <out.csv> <corpus_dir>
"""
import csv
import os
import subprocess
import sys

BIN = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
units_file, model, out_csv, corpus = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
units = [l.strip() for l in open(os.path.expanduser(units_file)) if l.strip()]
with open(out_csv, "w", newline="") as f:
    w = csv.writer(f); w.writerow(["unit", "status", "bits"])
    for i, u in enumerate(units, 1):
        env = dict(os.environ, OCUDU_DEPTH3_FALLBACK="none", OCUDU_REPLAY_MODEL=model)
        r = subprocess.run([BIN, os.path.join(corpus, u), "--out", "/tmp/_full", "--phy_pipeline", "npu",
                            "--pusch_receiver_backend", "ai", "--pusch_ldpc_decoder_type", "generic"],
                           capture_output=True, text=True, env=env)
        ok = "crc=OK" in r.stdout
        bits = 0
        if ok:
            for line in r.stdout.splitlines():
                if "tbs=" in line and "bits" not in line:
                    pass
            # the replay prints tbs in bytes on the result line
            for line in r.stdout.splitlines():
                if "crc=OK" in line and "tbs=" in line:
                    try:
                        bits = int(line.split("tbs=")[1].split()[0]) * 8
                    except (IndexError, ValueError):
                        bits = 0
                    break
        w.writerow([u, "OK" if ok else "KO", bits])
        if i % 1000 == 0:
            print(f"{i}/{len(units)}", flush=True)
print(f"{out_csv}: n={len(units)}")
