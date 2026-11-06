import os, re, subprocess, sys, time
B = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
units_file, corpus, bundle, out_csv = sys.argv[1:5]
decoder = sys.argv[5] if len(sys.argv) > 5 else "generic"
model = os.path.expanduser(f"~/ai_depth3/gate_ai_models/bundles/{bundle}")
units = [l.strip() for l in open(os.path.expanduser(units_file)) if l.strip()]
env = dict(os.environ); env["OCUDU_DEPTH3_FALLBACK"] = "none"; env["OCUDU_REPLAY_MODEL"] = model
rows = []
t0 = time.monotonic()
with open(out_csv, "w") as f:
    f.write("unit,status,bits\n")
    for i, u in enumerate(units, 1):
        p = subprocess.run([B, os.path.join(os.path.expanduser(corpus), u), "--out", "/tmp/_mf",
                            "--phy_pipeline", "npu", "--pusch_receiver_backend", "ai",
                            "--pusch_ldpc_decoder_type", decoder], env=env, capture_output=True, text=True)
        ok = "crc=OK" in p.stdout
        m = re.search(r"tbs=(\d+)", p.stdout)
        bits = (int(m.group(1)) * 8 if (ok and m) else 0)
        f.write(f"{u},{'OK' if ok else 'no'},{bits}\n")
        rows.append((ok, bits))
        if i % 2000 == 0:
            okn = sum(1 for r in rows if r[0]); bt = sum(r[1] for r in rows)
            el = time.monotonic() - t0
            print(f"  {i}/{len(units)}  OK={okn}  bits={bt:,}  {el/60:.1f} min  (ETA {el/i*(len(units)-i)/60:.0f} min)", flush=True)
okn = sum(1 for r in rows if r[0]); bt = sum(r[1] for r in rows)
print(f"DONE {os.path.basename(out_csv)}: n={len(rows)} OK={okn} bits={bt:,}")
