import glob, os, collections
L = "/Volumes/T7_Corpus/corpus/s2_QPSK_ping_1031b"

# 成功解码的 (sfn, slot)：dd_meta = idx,ts,tb_bits,nof_codeblocks,sfn,slot
ok = set()
with open(os.path.join(L, "tb_live/dd_meta.csv")) as f:
    for line in f:
        p = line.strip().split(",")
        if len(p) >= 6:
            ok.add((int(p[4]), int(p[5])))
print("dd_meta 行数:", len(ok))

# 每个捕获：文件名给 (sfn, slot)，txt 给 rv/harq/new_data
rows = []
for p in glob.glob(os.path.join(L, "cap_*.txt")):
    b = os.path.basename(p)
    if b.startswith("._") or b.endswith("_ce.txt"):
        continue
    parts = b[:-4].split("_")
    sfn, slot = int(parts[1]), int(parts[2])
    rv = harq = nd = None
    with open(p, "r", errors="ignore") as f:
        for line in f:
            if line.startswith("rv="): rv = int(line.split("=")[1])
            elif line.startswith("harq_id="): harq = int(line.split("=")[1])
            elif line.startswith("new_data="): nd = int(line.split("=")[1])
            if None not in (rv, harq, nd): break
    if None in (rv, harq, nd):
        continue
    rows.append((sfn, slot, harq, rv, nd))
rows.sort()
print("捕获总数:", len(rows))
print("rv 分布:", dict(sorted(collections.Counter(r[3] for r in rows).items())))

# 按 harq_id 串链：new_data=1 起新链，遇到下一个 new_data=1 或流末收尾
chains = []
cur = {}
for sfn, slot, harq, rv, nd in rows:
    if nd == 1:
        if harq in cur: chains.append(cur[harq])
        cur[harq] = [(sfn, slot, rv)]
    else:
        if harq in cur: cur[harq].append((sfn, slot, rv))
for h in cur: chains.append(cur[h])
print("HARQ 链总数:", len(chains))

self_ok = recov = lost = 0
for ch in chains:
    sfn, slot, rv = ch[0]                 # 首传
    first_ok = (sfn, slot) in ok
    later_ok = any((s, sl) in ok for s, sl, _ in ch[1:])
    if first_ok: self_ok += 1
    elif later_ok: recov += 1
    else: lost += 1
tot = len(chains)
print()
print("=== 按【一个 TB 的完整传输历史】分类 ===")
print(f"  ★ 首传即 CRC OK（self）:      {self_ok:6d}  ({self_ok*100/max(1,tot):5.1f}%)")
print(f"  ★ 首传失败、重传捞回（harq）: {recov:6d}  ({recov*100/max(1,tot):5.1f}%)")
print(f"  始终失败（丢失，无样本）:      {lost:6d}  ({lost*100/max(1,tot):5.1f}%)")
print(f"  合计:                          {tot:6d}")
