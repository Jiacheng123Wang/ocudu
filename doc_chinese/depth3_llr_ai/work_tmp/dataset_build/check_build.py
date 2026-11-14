"""Audit the built depth3 datasets: shape, bit balance, SNR, grant width, modulation consistency."""
import glob
import os
import collections
import numpy as np

files = sorted(glob.glob("datasets_bin/*.npz"))
rows, flags = [], []
MODS = ["QPSK", "16QAM", "64QAM", "256QAM"]

for f in files:
    name = os.path.basename(f)
    try:
        d = np.load(f, allow_pickle=True)
    except Exception as exc:                      # a file that will not open is a finding, not a crash
        flags.append((name, "will not open: %s" % exc))
        continue
    n = len(d["X"])
    X = d["X"]
    bits = np.asarray(d["bits"])
    snr = np.asarray(d.get("snr_nominal", []), dtype=float)
    prb = np.asarray(d.get("alloc_nof_prb", []))
    qm = sorted(set(np.asarray(d.get("qm", [])).tolist()))
    mod = next((m for m in MODS if m in name), "?")
    bal = float(bits.astype(np.uint8).mean()) if bits.size else float("nan")
    finite = X.astype(np.float32)
    nonfinite = int((~np.isfinite(finite)).sum())
    rows.append(dict(name=name, mod=mod, n=n, dtype=str(X.dtype), bal=bal,
                     snr=float(np.median(snr)) if snr.size else float("nan"),
                     lo=int(prb.min()) if prb.size else -1, hi=int(prb.max()) if prb.size else -1,
                     qm=qm, nonfinite=nonfinite,
                     shape=tuple(X.shape[1:])))
    if nonfinite:
        flags.append((name, "%d non-finite value(s) in X" % nonfinite))
    if bal != bal or bal < 0.35 or bal > 0.65:
        flags.append((name, "bit balance %.3f" % bal))
    if mod != "?" and qm and len(qm) != 1:
        flags.append((name, "mixed qm %s" % qm))

print("%d npz files" % len(files))
hdr = "%-46s %-6s %7s %-8s %7s %8s %10s %6s"
print(hdr % ("file", "mod", "n", "dtype", "bitmean", "snr_med", "prb", "qm"))
for r in rows[:24]:
    print(hdr % (r["name"][:46], r["mod"], r["n"], r["dtype"], "%.3f" % r["bal"],
                 "%.1f" % r["snr"], "%d-%d" % (r["lo"], r["hi"]), str(r["qm"])))

print("\n--- totals ---")
print("samples:", sum(r["n"] for r in rows), " over %d files" % len(rows))
print("X shapes:", collections.Counter(r["shape"] for r in rows))
print("X dtypes:", collections.Counter(r["dtype"] for r in rows))
print("bit mean range: %.3f .. %.3f" % (min(r["bal"] for r in rows), max(r["bal"] for r in rows)))
print("median SNR range: %.1f .. %.1f dB" % (min(r["snr"] for r in rows), max(r["snr"] for r in rows)))
print("grant width seen: %d .. %d PRB" % (min(r["lo"] for r in rows), max(r["hi"] for r in rows)))
print("samples per modulation:", collections.Counter({m: sum(r["n"] for r in rows if r["mod"] == m) for m in MODS}))
print("\n--- findings ---")
for name, why in flags[:15]:
    print("  %-46s %s" % (name[:46], why))
if not flags:
    print("   none")
