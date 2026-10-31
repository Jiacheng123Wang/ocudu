import sys
import numpy as np

need = {"X", "H", "bits", "tb_bits", "qm", "snr_nominal"}
rc = 0
for p in sys.argv[1:]:
    try:
        d = np.load(p)
        miss = sorted(need - set(d.files))
        if miss:
            print("LOAD_PARTIAL", p, "missing", miss)
            rc = 1
        else:
            print("LOAD_OK", p, "keys=%d" % len(d.files))
    except Exception as e:  # noqa: BLE001 - report, do not fail the shell silently
        print("LOAD_FAIL", p, type(e).__name__, e)
        rc = 1
sys.exit(rc)
