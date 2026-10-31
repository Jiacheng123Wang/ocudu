#!/usr/bin/env python3
"""Predict, per bin, how many samples a uniformly random draw will contain.

A fully random draw of a thousand samples out of two and a half million gives each bin a share proportional to how much
of the mother set that bin holds. The common bins get tens of samples; a bin holding twelve rows has a one-in-two-
thousand chance per draw and will almost always be empty. That is not a defect of the draw -- a random monitor set is
meant to mirror the traffic mixture -- but it has to be stated, because "the monitor set covers every bin" is false.

The tool prints, per bin, the mother-set count, the expected draw count and the probability of seeing at least one
sample, then summarises how many bins are expected to come up empty. Use it before deciding the draw size: to see a bin
holding n rows with even odds you need about 0.7 x mother / n samples.

Run it against an inventory CSV of the form produced by inventory_datasets.py: modulation,width,snr,self,harq.
"""
import csv, math, subprocess

# 从 211 取上一轮清点出来的逐箱统计（self+harq）
out = subprocess.run(["ssh", "-o", "BatchMode=yes", "jwang@192.168.31.211",
                      "cat /tmp/inv60.csv"], capture_output=True, text=True).stdout
rows = list(csv.DictReader(out.splitlines()))
N = 2526019
K = 1000
scale = K / N

data = []
for r in rows:
    n = int(r["self"]) + int(r["harq"])
    exp = n * scale
    p_at_least_one = 1.0 - math.exp(-exp) if exp > 0 else 0.0
    data.append((r["modulation"], r["width"], r["snr"], n, exp, p_at_least_one))

data.sort(key=lambda x: x[3], reverse=True)
print("母集总样本 {}，抽 {} 个，抽样比例 1/{:.0f}".format(N, K, N / K))
print()
print("| 箱 | 母集样本 | 期望 | 至少抽到 1 个的概率 |")
print("|---|---|---|---|")
for mod, w, snr, n, exp, p in data[:8]:
    print("| {} {} {} | {:,} | {:.2f} | {:.0%} |".format(mod, w, snr, n, exp, p))
print("| … | | | |")
for mod, w, snr, n, exp, p in data[-6:]:
    print("| {} {} {} | {:,} | {:.2f} | {:.1%} |".format(mod, w, snr, n, exp, p))

populated = [d for d in data if d[3] > 0]
exp_empty = sum(1 for d in populated if d[5] < 0.5)
print()
print("母集有样本的箱: {}".format(len(populated)))
print("预计抽不到（概率 < 50%）的箱: {}".format(exp_empty))
print("预计能抽到至少 1 个的箱: {}".format(len(populated) - exp_empty))
print()
big = sum(1 for d in populated if d[4] >= 10)
print("期望样本数 >= 10 的箱: {} 个".format(big))
print("这 {} 个箱合计承载母集 {:.1%} 的样本".format(
    big, sum(d[3] for d in populated if d[4] >= 10) / sum(d[3] for d in populated)))
