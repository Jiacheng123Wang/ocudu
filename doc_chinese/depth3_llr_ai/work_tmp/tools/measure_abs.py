"""One arm over a unit list against a corpus and an ABSOLUTE bundle path.

WHY THE LOCK IS HERE AND NOT IN run_arm_ext.py's guard. The replay binary refuses to start when another one is
alive, but this script spawns ONE PROCESS PER UNIT, so between two units the field is clear: two measurements
started together each see an idle machine and then abort on each other, leaving an all-zero csv behind. The lock
directory is the same one `probe_snapshot.sh` uses, so a probe and a ruler can never overlap either. `mkdir` is
atomic on every POSIX system, which is why it is the lock primitive here (macOS ships no `flock`).
"""
import os, re, subprocess, sys, time

B = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
units_file, corpus, model_dir, out_csv = sys.argv[1:5]
model = os.path.expanduser(model_dir)

LOCK = os.path.expanduser("~/ai_depth3/measure.lock.d")
waited = 0
while True:
    try:
        os.mkdir(LOCK)
        break
    except FileExistsError:
        if waited >= 7200:
            raise SystemExit(f"REFUSING: {LOCK} has been held for two hours by another measurement")
        time.sleep(10)
        waited += 10

try:
    units = [l.strip() for l in open(os.path.expanduser(units_file)) if l.strip()]
    env = dict(os.environ); env["OCUDU_DEPTH3_FALLBACK"] = "none"; env["OCUDU_REPLAY_MODEL"] = model
    with open(out_csv, "w") as f:
        f.write("unit,status,bits\n")
        okn = bt = 0
        for u in units:
            p = subprocess.run([B, os.path.join(os.path.expanduser(corpus), u), "--out", "/tmp/_mabs",
                                "--phy_pipeline", "npu", "--pusch_receiver_backend", "ai",
                                "--pusch_ldpc_decoder_type", "generic"], env=env, capture_output=True, text=True)
            ok = "crc=OK" in p.stdout
            m = re.search(r"tbs=(\d+)", p.stdout)
            bits = (int(m.group(1)) * 8 if (ok and m) else 0)
            f.write(f"{u},{'OK' if ok else 'no'},{bits}\n")
            okn += ok; bt += bits
finally:
    # A CRASH MUST NOT LEAVE THE LOCK BEHIND: the next measurement would sit in the wait loop for two hours.
    try:
        os.rmdir(LOCK)
    except OSError:
        pass

# ZERO DECODED UNITS IS A BROKEN RUN, NOT A WEAK MODEL. Seen when a probe collided with a ruler: every unit
# aborted, the csv still had 5 000 `no` rows, and the summary read `OK=0 bits=0` as if the chain had failed.
if okn == 0:
    raise SystemExit(f"FAILED {os.path.basename(out_csv)}: n={len(units)} OK=0 -- the replay produced nothing")
print(f"DONE {os.path.basename(out_csv)}: n={len(units)} OK={okn} bits={bt:,}")
