"""Retransmission (rv) share per SNR band and width band: the cheap proxy for a failed first attempt."""
import glob, os, sys, math, collections
leg = sys.argv[1]
W = lambda p: "<=3" if p <= 3 else ("<=8" if p <= 8 else ("<=24" if p <= 24 else ">24"))
S = lambda d: "<0" if d < 0 else ("0-6" if d < 6 else ("6-10" if d < 10 else ("10-14" if d < 14 else ("14-20" if d < 20 else ("20-30" if d < 30 else ">=30")))))
tot = collections.Counter(); retx = collections.Counter()
n = 0
for f in glob.glob(leg + "/cap_*.txt"):
    if f.endswith("_ce.txt"):
        continue
    rv = prb = None
    try:
        for line in open(f, errors="ignore"):
            k, sep, v = line.strip().partition("=")
            if not sep:
                continue
            if k == "rv":
                rv = int(v)
            elif k == "alloc_nof_rb":
                prb = int(v)
            if rv is not None and prb is not None:
                break
    except OSError:
        continue
    if rv is None or prb is None:
        continue
    snr = None
    try:
        for line in open(f[:-4] + "_ce.txt", errors="ignore"):
            if line.startswith("port=0"):
                for tok in line.split():
                    if tok.startswith("snr="):
                        try:
                            s = float(tok[4:])
                            if s > 0:
                                snr = 10 * math.log10(s)
                        except ValueError:
                            snr = None
                break
    except OSError:
        pass
    if snr is None:
        continue
    key = (W(prb), S(snr))
    tot[key] += 1
    if rv > 0:
        retx[key] += 1
    n += 1
print(f"{n} captures with rv, width and SNR")
print("%-6s %-7s %8s %8s %7s" % ("width", "snr", "n", "rv>0", "retx%"))
for w in ["<=3", "<=8", "<=24", ">24"]:
    for s in ["<0", "0-6", "6-10", "10-14", "14-20", "20-30", ">=30"]:
        t = tot.get((w, s), 0)
        if not t:
            continue
        r = retx.get((w, s), 0)
        print("%-6s %-7s %8d %8d %6.1f%%" % (w, s, t, r, 100 * r / t))
print("\noverall rv>0: %d of %d = %.1f%%" % (sum(retx.values()), sum(tot.values()), 100 * sum(retx.values()) / max(1, sum(tot.values()))))
