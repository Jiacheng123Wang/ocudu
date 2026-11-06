import os, re, subprocess, sys
B = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
units_file, corpus, model_dir, out_csv = sys.argv[1:5]
model = os.path.expanduser(model_dir)
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
print(f"DONE {os.path.basename(out_csv)}: n={len(units)} OK={okn} bits={bt:,}")
