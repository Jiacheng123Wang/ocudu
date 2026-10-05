
# PING RTT AS THE gNB ITSELF SEES IT: pair each DL ICMP request (GTPU RX SDU) with the nearest following UL PDU
# (GTPU TX PDU) and report the distribution.  This is an INDEPENDENT path to the same question the [ul_pipeline]
# series answers: one sample per ping, taken from the GTPU trace rather than from the PHY's own instrumentation,
# so it cannot be flattered by a boundary or a buffer that the PHY report happens to sit behind.
#
# usage:  python3 rtt_split.py p291 p292 p293        # labels (searches LEG_LOG_DIRS)
#         python3 rtt_split.py path/to/gnb_cpu_x.log  # an explicit .log (stdout, mode-agnostic)
#
# CAVEAT, and it is the reason this is a CROSS-CHECK and not the verdict: the DL request timestamp is when the
# SDU arrived at the gNB and the UL timestamp is when the PDU left it, so this EXCLUDES the core-side and the
# phone-side legs of the round trip.  It is the RTT as the radio sees it, not the RTT ping prints.
import re,sys,glob,os
from collections import Counter

HERE=os.path.dirname(os.path.abspath(__file__))
# where legs are written: this workstream first, then the older one (legs flown before 2026-10-05 live there)
DIRS=[os.environ.get('LEG_LOGDIR',os.path.join(HERE,'logs')),
      os.path.join(HERE,'..','..','phy_pipeline_gpu','wip','logs')]

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

def resolve(a):
    # an explicit .log, or 'foo.log.stderr', or a label
    for cand in (a, a[:-len('.stderr')] if a.endswith('.stderr') else None):
        if cand and os.path.exists(cand) and cand.endswith('.log'): return cand
    hits=[]
    for d in DIRS:
        # MODE-AGNOSTIC: the runner names legs gnb_<mode>_<label>_<date>_<time>.log, and mode is not the label
        hits += glob.glob(os.path.join(d,f'gnb_*_{a}*.log.stderr')) + glob.glob(os.path.join(d,f'gnb_*_{a}*.log'))
    hits=[h for h in hits if h.endswith('.log')]
    return sorted(set(hits),key=os.path.getmtime)[-1] if hits else None

for a in sys.argv[1:]:
    lg=resolve(a)
    if not lg: print(f"{a}: no leg found"); continue
    ev=parse(lg)
    if not ev: print(f"{a}: no GTPU events"); continue
    dl=Counter(s for t,k,s in ev if k=='D')
    # SELF-CALIBRATING: the ping stream is the DL size class with a few hundred to a few thousand instances
    cand=[s for s,c in dl.items() if 300<=c<=100000]
    if not cand: print(f"{a}: no ping found"); continue
    dsz=max(cand,key=lambda s:dl[s])
    req=[t for t,k,s in ev if k=='D' and s==dsz]
    rep=[t for t,k,s in ev if k=='U']
    # NEAREST-NEXT pairing: each request takes the first reply that follows it
    j=0; d=[]; lost=0
    for r in req:
        while j<len(rep) and rep[j]<=r: j+=1
        if j<len(rep):
            dt=(rep[j]-r)/1000.0
            if dt<1000: d.append(dt)
            else: lost+=1
            j+=1
        else: lost+=1
    d.sort()
    if len(d)<100: print(f"{a}: DLsize={dsz} pairs={len(d)} (too few)"); continue
    mean=sum(d)/len(d)
    print(f"{a:12} n={len(d):5} dl={dsz:5}B lost={lost:4}  gNB-visible RTT ms: "
          f"min={d[0]:6.2f} p50={pct(d,.5):6.2f} p90={pct(d,.9):6.2f} p95={pct(d,.95):6.2f} "
          f"p99={pct(d,.99):6.2f} max={d[-1]:7.2f} mean={mean:6.2f}")
