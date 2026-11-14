import glob, os, re, subprocess, collections, sys
B = os.path.expanduser("~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay")
C = os.path.expanduser("~/dev/ocudu/lib/phy/upper/ai_signal_processors/test/ruler_s2_amc_5000_corpus")
BD = os.path.expanduser("~/ai_depth3/gate_ai_models/bundles")
models = sys.argv[1:]
units = []
for f in glob.glob(os.path.join(C, "*.txt")):
    d = dict(l.strip().partition("=")[::2] for l in open(f) if "=" in l)
    if int(d.get("alloc_nof_rb", 0)) <= 3 and d.get("new_data") == "1":
        units.append((os.path.basename(f)[:-4], int(d["alloc_nof_rb"])))
env = dict(os.environ); env["OCUDU_DEPTH3_FALLBACK"] = "none"
def run(unit, model):
    args = [B, os.path.join(C, unit), "--out", "/tmp/_nb", "--phy_pipeline", "npu",
            "--pusch_receiver_backend", "ai", "--pusch_ldpc_decoder_type", "generic"]
    e = dict(env); e["OCUDU_REPLAY_MODEL"] = model
    p = subprocess.run(args, env=e, capture_output=True, text=True)
    m = re.search(r"tbs=(\d+)", p.stdout)
    ok = "crc=OK" in p.stdout
    return (int(m.group(1)) * 8 if (ok and m) else 0), ok
for name in models:
    model = os.path.join(BD, name, name + ".mlmodelc")
    if not os.path.isdir(model):
        print(f"{name}: 模型不存在 {model}"); continue
    st = collections.defaultdict(lambda: {"n":0,"ok":0,"bits":0})
    for u, n in units:
        bits, ok = run(u, model)
        for k in (n, "all"):
            s = st[k]; s["n"]+=1; s["ok"]+=ok; s["bits"]+=bits
    a = st["all"]
    print(f"\n=== {name}  (n={a['n']}) ===")
    for k in sorted([x for x in st if isinstance(x,int)]):
        s = st[k]
        print(f"  {k} PRB: n={s['n']:>3}  OK={s['ok']:>3}  bits={s['bits']:>9,}")
    print(f"  合计 OK={a['ok']}  bits={a['bits']:,}")
