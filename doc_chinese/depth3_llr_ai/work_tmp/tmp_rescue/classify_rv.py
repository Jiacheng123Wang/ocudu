import glob, os, collections

L = "/Volumes/T7_Corpus/corpus/s2_QPSK_ping_1031b"
# 1) 成功解码的 slot 集合（来自 dd_meta 的第 5 列 system_slot）
ok_slots = set()
with open(os.path.join(L, "tb_live/dd_meta.csv")) as f:
    for line in f:
        p = line.strip().split(",")
        if len(p) >= 5:
            ok_slots.add(int(p[4]))
print("成功解码 TB 数:", len(ok_slots))

# 2) 每个捕获的 (slot, harq_id, rv, new_data)
rows = []
for p in glob.glob(os.path.join(L, "cap_*.txt")):
    b = os.path.basename(p)
    if b.startswith("._") or b.endswith("_ce.txt"):
        continue
    slot = harq = rv = nd = None
    with open(p, "r", errors="ignore") as f:
        for line in f:
            if line.startswith("slot="): slot = int(line.split("=")[1])
            elif line.startswith("harq_id="): harq = int(line.split("=")[1])
            elif line.startswith("rv="): rv = int(line.split("=")[1])
            elif line.startswith("new_data="): nd = int(line.split("=")[1])
            if None not in (slot, harq, rv, nd): break
    if None in (slot, harq, rv, nd):
        continue
    rows.append((slot, harq, rv, nd))

rows.sort()
print("捕获总数:", len(rows))
print("rv 分布:", dict(collections.Counter(r[2] for r in rows)))
print("new_data 分布:", dict(collections.Counter(r[3] for r in rows)))

# 3) 按 harq_id 串成链：new_data=1 起新链
chains = collections.defaultdict(list)
cur = {}
for slot, harq, rv, nd in rows:
    if nd == 1:
        cur[harq] = []
    if harq in cur:
        cur[harq].append((slot, rv))
    else:
        cur[harq] = [(slot, rv)]
for h, lst in cur.items():
    chains[tuple(lst)].append(h)

# 4) 分类
self_ok = recov = lost = other = 0
chain_stats = collections.Counter()
for lst, _ in ((c, None) for c in chains):
    first_slot, first_rv = lst[0]
    later_ok = any(s in ok_slots for s, _ in lst[1:])
    first_ok = first_slot in ok_slots
    if first_ok:
        self_ok += 1; chain_stats["self"] += 1
    elif later_ok:
        recov += 1; chain_stats["recovered"] += 1
    else:
        lost += 1; chain_stats["lost"] += 1
print()
print("=== HARQ 链分类（每条链 = 一个 TB 的完整传输历史）===")
print(f"  链总数:              {sum(chain_stats.values())}")
print(f"  ★ 首传(rv=0)即成功:  {self_ok}  ({self_ok*100/max(1,sum(chain_stats.values())):.1f}%)")
print(f"  ★ 首传失败、重传捞回: {recov}  ({recov*100/max(1,sum(chain_stats.values())):.1f}%)")
print(f"  始终失败（丢失）:     {lost}  ({lost*100/max(1,sum(chain_stats.values())):.1f}%)")
