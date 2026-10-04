
import re,sys,glob,os
from collections import Counter
def parse(f):
    pat=re.compile(r'^\S+T(\d{2}):(\d{2}):(\d{2})\.(\d+) \[GTPU\s*\] \[I\] lif=NG-U ue=0 (DL teid=0x[0-9a-f]+: RX SDU\. sdu_len=(\d+)|UL teid=0x[0-9a-f]+: TX PDU\. pdu_len=(\d+))')
    ev=[]
    with open(f,errors='ignore') as fh:
        for line in fh:
            m=pat.match(line)
            if not m: continue
            us=((int(m.group(1))*3600+int(m.group(2))*60+int(m.group(3)))*1_000_000)+int(m.group(4).ljust(6,'0'))
            ev.append((us,'D' if m.group(6) else 'U',int(m.group(6) or m.group(7))))
    return ev
def pct(s,p): return s[min(len(s)-1,int(p*len(s)))]
for p in sys.argv[1:]:
    fs=sorted(glob.glob(f'gnb_gpu_{p}-*.log.stderr'),key=os.path.getmtime)
    if not fs: continue
    lg=fs[-1][:-len('.stderr')]
    if not os.path.exists(lg): continue
    ev=parse(lg)
    dl=Counter(s for t,k,s in ev if k=='D')
    cand=[s for s,c in dl.items() if 800<=c<=1200]
    if not cand: print(f"{p}: no ping found"); continue
    dsz=max(cand,key=lambda s:dl[s])
    req=[t for t,k,s in ev if k=='D' and s==dsz]
    rep=[t for t,k,s in ev if k=='U']
    # NEAREST-NEXT pairing: each request takes the first reply that follows it
    j=0; d=[]
    for r in req:
        while j<len(rep) and rep[j]<=r: j+=1
        if j<len(rep):
            dt=(rep[j]-r)/1000.0
            if dt<1000: d.append(dt)
            j+=1
    d.sort()
    if len(d)<100: print(f"{p}: DL={dsz} pairs={len(d)} (too few)"); continue
    print(f"{p:6} n={len(d):4} gNB-visible RTT ms: min={d[0]:6.2f} p50={pct(d,.5):6.2f} p90={pct(d,.9):6.2f} p99={pct(d,.99):6.2f} max={d[-1]:7.2f}")

