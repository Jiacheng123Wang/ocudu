import csv, random, os, json, sys
from collections import Counter

DUMP='/Users/jiachengwang/ai_depth3/dump_s2/tb_s2_amc_ping_1102a'
CORP='/Users/jiachengwang/ai_depth3/corpus/s2_amc_ping_1102a'
OUT='/Users/jiachengwang/ai_depth3/exp_bits_1102'
SEED=20261102
N=5000

rows=list(csv.DictReader(open(os.path.join(DUMP,'index.csv'))))
print('index rows', len(rows))
print('OK', sum(1 for r in rows if r['crc']=='OK'), 'notOK', sum(1 for r in rows if r['crc']!='OK'))
rng=random.Random(SEED)
idxs=rng.sample(range(len(rows)), N)
idxs.sort()
sample=[rows[i] for i in idxs]

missing=[]; parsed=[]
mods=Counter(); bands=Counter()
for r in sample:
    u=r['unit']; base=os.path.join(CORP,u)
    for suf in ('.bin','_h.bin','.txt','_ce.txt'):
        if not os.path.exists(base+suf): missing.append(u+suf)
    meta={}
    try:
        for line in open(base+'.txt'):
            line=line.strip()
            if '=' in line:
                k,v=line.split('=',1); meta[k]=v
    except Exception as e:
        missing.append(u+'.txt:'+str(e))
    mod=meta.get('modulation','?')
    try: rb=int(meta.get('alloc_nof_rb','-1'))
    except: rb=-1
    if rb<=3: band='<=3'
    elif rb<=8: band='<=8'
    elif rb<=24: band='<=24'
    else: band='>24'
    mods[mod]+=1; bands[band]+=1
    parsed.append(dict(unit=u, idx=r.get('unit'), dump_crc=r['crc'], dump_tb_bits=int(r['tb_bits'] or 0),
                       dump_tbs_bytes=(int(r['tbs_bytes']) if r['tbs_bytes'] else None), mod=mod, alloc_nof_rb=rb, band=band,
                       sinr_db=r.get('sinr_db',''), target_code_rate=meta.get('target_code_rate','')))
print('sample', len(sample), 'OK', sum(1 for r in sample if r['crc']=='OK'), 'notOK', sum(1 for r in sample if r['crc']!='OK'))
print('missing files', len(missing), missing[:5])
print('mods', dict(mods)); print('bands', dict(bands))

with open(os.path.join(OUT,'sample.csv'),'w',newline='') as f:
    w=csv.DictWriter(f, fieldnames=list(parsed[0].keys())); w.writeheader(); w.writerows(parsed)
with open(os.path.join(OUT,'units.txt'),'w') as f:
    for p in parsed: f.write(p['unit']+'\n')
with open(os.path.join(OUT,'files.txt'),'w') as f:
    for p in parsed:
        for suf in ('.bin','_h.bin','.txt','_ce.txt'):
            f.write(p['unit']+suf+'\n')
# also a small probe list, first 5
with open(os.path.join(OUT,'probe_units.txt'),'w') as f:
    for p in parsed[:5]: f.write(p['unit']+'\n')
json.dump(dict(seed=SEED,n=N,total=len(rows),ok=sum(1 for r in rows if r['crc']=='OK'),sample_ok=sum(1 for r in sample if r['crc']=='OK')), open(os.path.join(OUT,'sample_meta.json'),'w'), indent=1)
