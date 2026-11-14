"""Correct bit-balance check: the stored bits are PACKED and PADDED, so count only the first nof_bits."""
import glob
import os
import numpy as np

files = sorted(glob.glob("datasets_bin/*.npz"))
print("keys of one file:", sorted(np.load(files[0], allow_pickle=True).files))

print("\n%-44s %6s %8s %8s %8s" % ("file", "n", "bit1", "crc_ok", "padded"))
for f in files[::12][:10]:                       # a spread across the 121
    d = np.load(f, allow_pickle=True)
    bits = np.asarray(d["bits"])
    nof = np.asarray(d.get("nof_bits", []), dtype=np.int32)
    ones = total = 0
    for i in range(len(bits)):
        nb = int(nof[i]) if nof.size else bits.shape[1] * 8
        packed = bits[i].reshape(-1)
        full, rem = divmod(nb, 8)
        u = np.unpackbits(packed[:full])
        if rem:
            u = np.concatenate([u, np.unpackbits(packed[full:full + 1])[:rem]])
        ones += int(u.sum()); total += u.size
    crc = np.asarray(d["crc_ok"]).mean() if "crc_ok" in d.files else float("nan")
    mnb = int(nof.min()) if nof.size else -1
    mxb = int(nof.max()) if nof.size else -1
    print("%-44s %6d %8.3f %8.3f %5d-%d bits" %
          (os.path.basename(f)[:44], len(bits), ones / max(1, total), crc, mnb, mxb))
