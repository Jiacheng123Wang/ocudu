"""Replay the HARQ-only units of a leg through an arm, combining each retransmission with its donor.

WHY. The PK's "vs CPU+HARQ" column compares the classical chain WITH HARQ combining against the AI arm WITHOUT it, which
is not like-for-like: the CPU baseline adds bits that only exist because a retransmission was combined with the soft bits
of the earlier one. The replay binary supports exactly that situation for any backend ("--also <base> --harq-chain
replays several transmissions of ONE HARQ process through ONE processor"), so the missing measurement is mechanical
rather than architectural.

FAILURES ARE LOUD NOW. Two runs came back with every unit KO and the next run of the same command was fine, which means
the tool was hiding a real error: the subprocess output was captured and dropped, so "the model would not load" and "the
block did not decode" looked identical. Every run's exit status and its first stderr line are therefore collected, and a
non-zero exit is reported with the count -- a silent all-KO result is exactly the failure mode this protects against.

USAGE
    python measure_harq.py <index_harq_only.csv> <bundle.mlmodelc> <out.csv> <corpus_dir> [limit]
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
nonzero = 0
combined = 0
first_error = None
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
        if proc.returncode != 0:
            nonzero += 1
            if first_error is None:
                lines = [l for l in proc.stderr.splitlines() if l.strip()]
                first_error = (unit, proc.returncode, lines[-1] if lines else "(no stderr)")
        # HOW MANY RECEPTIONS WERE ACTUALLY REPLAYED. The binary says so, and a run that replays only the first
        # transmission is a run with no combining at all -- which is indistinguishable from "the block did not decode"
        # in the result column. Counting them is what turns that silent failure into a number.
        if "replayed 2 reception" in proc.stdout:
            combined += 1
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

print(f"{out_csv}: n={len(rows)}, combined {combined}")
if rows and combined < len(rows):
    print(f"!! {len(rows) - combined}/{len(rows)} runs replayed only ONE reception: the donor was not found or not "
          f"read, so those results carry no combining", flush=True)
if nonzero:
    unit, code, message = first_error
    print(f"!! {nonzero}/{len(rows)} runs exited non-zero; first: {unit} exit={code}: {message}", flush=True)
