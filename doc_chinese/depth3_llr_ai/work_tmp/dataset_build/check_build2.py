"""Audit the built depth3 datasets, correctly this time: unpacked bit balance, CRC, and per-file SNR."""
import glob
import os
import collections
import numpy as np

MODS = ["QPSK", "16QAM", "64QAM", "256QAM"]
POP = {"self": "crc=OK on its own", "harq": "recovered from a later rv"}
files = sorted(glob.glob("datasets_bin/*.npz"))

def bit_fraction(packed):
    """Fraction of 1-bits. The stored array is PACKED BYTES, so the mean byte value is not a balance."""
    return float(np.unpackbits(packed.astype(np.uint8)).mean())

rows, flags = [], []
for f in files:
    name = os.path.basename(f)
    d = np.load(f, allow_pickle=True)
    n = len(d["X"])
    bal = bit_fraction(np.asarray(d["bits"]))
    snr = np.asarray(d["snr_nominal"], dtype=float)
    prb = np.asarray(d["alloc_nof_prb"])
    qm = sorted(set(np.asarray(d["qm"]).tolist()))
    mod = next((m for m in MODS if m in name), "?")
    pop = "harq" if name.endswith("_harq.npz") else "self"
    label_from = collections.Counter(np.asarray(d["label_from"]).tolist()) if "label_from" in d.files else {}
    rows.append(dict(name=name, mod=mod, pop=pop, n=n, bal=bal,
                     snr_med=float(np.median(snr)), snr_lo=float(np.percentile(snr, 5)), snr_hi=float(np.percentile(snr, 95)),
                     lo=int(prb.min()), hi=int(prb.max()), qm=qm, labels=dict(label_from)))
    if bal < 0.40 or bal > 0.60:
        flags.append((name, "bit fraction %.3f" % bal))
    if len(qm) != 1:
        flags.append((name, "mixed qm %s" % qm))
    if mod != "?" and qm and not (qm[0] == MODS.index(mod) + 1 or qm[0] == MODS.index(mod)):
        pass          # qm encoding varies; the name already states the modulation

print("%d files, %d samples" % (len(rows), sum(r["n"] for r in rows)))
print("\n%-44s %-6s %-5s %6s %7s %13s %10s" % ("file", "mod", "pop", "n", "bit1", "snr p5-p95", "prb"))
for r in rows[:18]:
    print("%-44s %-6s %-5s %6d %7.3f %6.1f-%5.1f %5d-%d" %
          (r["name"][:44], r["mod"], r["pop"], r["n"], r["bal"], r["snr_lo"], r["snr_hi"], r["lo"], r["hi"]))

print("\n--- per modulation ---")
for m in MODS:
    sel = [r for r in rows if r["mod"] == m]
    if sel:
        print("  %-6s files %3d  samples %6d  bit1 %.3f  snr %.1f..%.1f dB" %
              (m, len(sel), sum(r["n"] for r in sel),
               np.mean([r["bal"] for r in sel]), min(r["snr_med"] for r in sel), max(r["snr_med"] for r in sel)))

print("\n--- label provenance (self files should say 'self') ---")
prov = collections.Counter()
for r in rows:
    for k, v in r["labels"].items():
        prov[(r["pop"], str(k))] += v
for k, v in list(prov.items())[:8]:
    print("  %-6s label_from=%-8s %6d" % (k[0], k[1], v))

print("\n--- findings ---")
for name, why in flags[:10]:
    print("  %-44s %s" % (name[:44], why))
if not flags:
    print("   none")
