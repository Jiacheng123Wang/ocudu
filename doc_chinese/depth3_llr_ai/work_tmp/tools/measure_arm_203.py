import os, re, subprocess, sys
B = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
C = os.path.expanduser("~/replay_bits_1102/corpus")
units_file, bundle, out_csv = sys.argv[1], sys.argv[2], sys.argv[3]
model = os.path.expanduser(f"~/ai_depth3/gate_ai_models/bundles/{bundle}")
units = [l.strip() for l in open(os.path.expanduser(units_file)) if l.strip()]
env = dict(os.environ); env["OCUDU_DEPTH3_FALLBACK"] = "none"; env["OCUDU_REPLAY_MODEL"] = model
rows = []
for u in units:
    p = subprocess.run([B, os.path.join(C, u), "--out", "/tmp/_ma203",
                        "--phy_pipeline", "npu", "--pusch_receiver_backend", "ai",
                        "--pusch_ldpc_decoder_type", "generic"], env=env, capture_output=True, text=True)
    ok = "crc=OK" in p.stdout
    m = re.search(r"tbs=(\d+)", p.stdout)
    rows.append((u, "OK" if ok else "no", (int(m.group(1)) * 8 if (ok and m) else 0)))
with open(out_csv, "w") as f:
    f.write("unit,status,bits\n")
    for r in rows: f.write(f"{r[0]},{r[1]},{r[2]}\n")
print(f"{os.path.basename(out_csv)}: n={len(rows)} OK={sum(1 for r in rows if r[1]=='OK')} bits={sum(r[2] for r in rows):,}")
