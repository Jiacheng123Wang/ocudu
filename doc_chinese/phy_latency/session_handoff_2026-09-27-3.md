# Session handoff — 2026-09-27 #3

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份** —— 现在就是本文件）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，日期取会话开始那天，序号是那天第几份。
> **上一份 = `session_handoff_2026-09-27-2.md`**（它写于本轮之前，覆盖到 §6.137）。
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`：
> **上一份 memo 之后新增的是 §6.138–§6.139**。`high_level_status_and_plan.md` 是高层现状（§0 已同步）。

---

## 0. 一句话现状

**主线判据全绿**（V1 1392.5 / V2 池绿 / V3 按重述判据收口 / V4 `cbs/lane=2.00` / V5 契约 9/9、`gaps=0`）。
本会话把上一份 memo 的**唯一开放项**（一跳里那 **~450–470 µs**）推到了**只剩一次手机测试**的位置：

> 修好了"消去跳的 label 被覆盖"这个**第四次**栽在同一族（作用域/次序）上的缺陷 —— 而且**没有照抄"挪一行"**，改成了**单一表达式的前序**（`ablated > merged_hop > lane_burst`），
> 并给 per-label 表加了 **`min`/`p5`** ⇒ 这条腿**有两个独立读数**（label 表 + 低尾），**不再"label 对了才有答案"**。
> **离线三臂全过**（不设旋钮 5 PASSED 且只有 `lane_burst`；`EVERY=1` ⇒ `ABLATION ON` + 2 FAILED 且出现 **`merged_hop_ablated` exec p50 20.5 µs**），
> `ctest -L phy` **193/193** ⇒ **交付路径不变**。**只差飞 `p83-n78-ablate8`**（需要 sudo + 手机，见 §3.1）。

**当前状态**：代码 + 文档已提交（`git log -1` 见下）；**已按提交重新打戳并重链 `gnb`**（`build/hashes.h` = `gnb` 内嵌戳 = HEAD）——⚠ **飞之前再核一次**（§3.2）；**工作区干净**、**无腿在跑、无 gNB 在跑**。
**交付配置**：收环 512 / 发环 64 / 池 32 / `otw_format: sc12` / `srate 23.04`（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。

| 判据 | 现状 |
|---|---|
| V1 `[ul_gpu_pipeline]` 中位 ≤2150 | ✅ **1392.5 µs**（`p81`/`p82` 同量级）|
| V2 池 | ✅ `starved_events=0`、`dropped=0` |
| V3 电台（**判据已重述**：`gaps==0` + 每 DL 递交失败率带宽）| ✅ gpu 0.123–0.202%、cpu_gpu 0.1467%、cpu 0.0012–0.0068% |
| V4（代理量）`cbs/lane ≤2.00` | ✅ 2.00（`p77`–`p82` 多条腿 `max=5` 或 `2`，均值仍 2.00）|
| V5 契约 / `gaps` / crossings | ✅ 9 of 9 / 0 / 0.00+0.00 每跳 |
| **那 ~450–470 µs** | ⏳ **已定价、机制已二分到"一条 cb"；消去法的仪器已修好并离线自证，只差飞 `p83`** |

---

## 1. 本会话做了什么（§6.138–§6.139；**没有飞腿**）

| 动作 | 结论 |
|---|---|
| ★ **修 label 前序**（`ocudu_metal_burst.mm` 的 `commit()`）| 不"挪一行"，而是**把次序变成值**：`const char* burst_label = ablated ? "merged_hop_ablated" : (merged ? "merged_hop" : "lane_burst");` ⇒ **没有任何一条语句能再"后写覆盖先写"** |
| ★ **per-label 表加 `min`/`p5`**（`ocudu_metal_queue.mm`）| wait 与 exec 各自 `p50/p95/min/p5`；排序只做一次（`pct_sorted` + 调用点显式 `std::sort`）⇒ **即使 label 再失败，也能从 `merged_hop` 的低尾读出消去样本** |
| **离线三臂自证**（`pusch_demodulator_deferred_chain_test`，重建后）| 不设旋钮：**5 PASSED**、表里只有 `lane_burst`（exec p50 23.5）；`EVERY=1`：**`ABLATION ON` + 2 FAILED**、表里出现 **`merged_hop_ablated` n=69 exec p50 20.5（min 16.1 / p5 19.9）** ⇒ 与 `p80` 那 1 个空口样本（**18.1**）同量级 |
| **`ctest -L phy`（串行）** | **193/193 passed**（1 条 `Disabled`）⇒ **交付路径不变** |
| **重建 + 打戳** | 两文件与 `gnb` 零警告重建；**所有提交做完之后**才 `touch build/hashes.h && cmake --build build --target gnb`（次序错了戳会落在旧提交上，§6.44 (5) 记过）|

---

## 2. 增量结论（必读）

1. ★★ **这一族的第四次**：`p80`（决策点在"新建 cb"分支，adopt 路不经过）、`p81`（`burst_state` 是 `thread_local`，编码线程写/提交线程读）、`p82`（label 被后面的 `merged_hop` 覆盖行盖掉）、
   以及本会话修掉的**根因形状**：**"多个赋值按次序写，最后写的人赢"**。⇒ **纪律升级**：凡"谁会赢"的逻辑，**写成表达式/单一归属点**，不要写成"按次序赋值"。
2. ★ **离线自证这次覆盖到哪、没覆盖到哪**（别过度引用）：它证明了"**消去 label 不再被丢**"（`encoder()` 建、`commit()` 读得到、真的进了 Q9-F3 报告）；
   它**没有**覆盖"与 `merged_hop` 那一行的次序"（离线臂里 `commit_label` 是默认的 `equalizer_demapper`）——
   那一条靠 §6.138① 的表达式形状，且 §6.138② 让**即使它再失败也能读出答案**。
3. ★ **消去法的形态**（§6.131–§6.133）：`OCUDU_LANE_ABLATE=1` = 每个阶段的绑定换成空 kernel（**记账/屏障/网格/提交结构全不变，只换活**）；
   **全消去手机接不进来**（上行全垃圾 ⇒ Msg3/RRC 解不出）⇒ 必须 `…_EVERY=8`（1-in-N，HARQ 兜）⇒ **同腿两总体**：`merged_hop` vs `merged_hop_ablated`。
4. **`p82` 给出的"预期变差"基线**（判据里不作数，但别被吓到）：`81892 CRC-OK / 145943 跳 = 56%`、V1 **1399.8**、契约 9/9、`gaps=0`、池全绿。
5. **已知口径**：`dft_front_end`（plain 路）不是融合车道的前端；一跳墙钟 **1935 µs**（换量纲必须用它）；`merged_hop` 的 `commit→start` 在 (B) 修复后是 ~215 µs。

---

## 3. 下一步（**只有一件事**）

### 3.1 ★ 飞 `p83-n78-ablate8`（需要 sudo + 手机；**这是唯一动作**）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p83-n78-ablate8 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8
```

**接手机之前**核三行：① `knob : OCUDU_LANE_ABLATE=1` **与** `…_EVERY=8` 都在；② `[metal_ablate] ABLATION ON …`；③ 戳一致（`run_leg.sh` 自己会拒，不用人看）。

**判据（同腿两总体，对照 = 同腿的 `merged_hop`，历史 467.5–470.0）**：

| 读数 | 若 | 则 |
|---|---|---|
| **主**：`merged_hop_ablated`（预期 **n ≈ 1.8 万**）exec p50 | **~20–80 µs** | ★ 那 ~450 µs **就是这些 kernel 的执行** ⇒ 下一轮用 `OCUDU_LANE_ABLATE_STAGE=…` 二分 eq / CE / demap |
| 同上 | **仍 ~450 µs** | ★ 与"活"无关 ⇒ 平台给这条 cb 记的账，**本平台无杠杆** ⇒ 这一线以"已定价、不可动"收口 |
| **备用**（label 若再失败）：`merged_hop` 的 `min`/`p5` | 从 470 掉到 **~20** | 与主读数同义；**两行一起读，交叉验证** |
| 红线 | `ch_wt`（41.1）/ 契约 / `gaps=0` / `rx_overflows=0` / 池 `starved_events=0` 不许变；`cbs/lane` 预期 **2.00** | 变了 ⇒ 先读原因再解读窗口 |

### 3.2 飞之前的身份核对（三条，缺一不可）

```bash
git -C /Users/jiachengwang/dev/ocudu log --oneline -1
grep build_hash /Users/jiachengwang/dev/ocudu/build/hashes.h
grep -ac "$(git -C /Users/jiachengwang/dev/ocudu rev-parse --short=10 HEAD)" /Users/jiachengwang/dev/ocudu/build/apps/gnb/gnb
```
三者必须一致。**若之后又提交了任何东西**（例如本 memo 的后续修订）⇒ 必须 `touch build/hashes.h && cmake --build build --target gnb` 重打戳重链，否则 `run_leg.sh` 会拒。

### 3.3 之后的岔路（等 3.1 的答案再选）

* 若"就是执行" ⇒ 二分到具体 kernel 后，考虑 **kernel 形状**（§6.80 的老清单：一个线程组做多个变换、降 tgmem/寄存器 footprint），**先在离线 harness 里量形状**；
* 若"与活无关" ⇒ 把这一线封存，回到 G1/G2 指向的**链条延长**（LDPC→Metal，用户裁定**单独规划**）或 S-E（最后选项）。

### 3.4 明确不要做的

* 宿主 staging / 拷贝（破 G1）；`OCUDU_DFT_RELEASE_BLOCK=0`、`OCUDU_CE_LANE_ORDER=event` 作为**交付结构**（破 G2，只可作测量臂）；
* V3 传输参数（四个机制已收口）；**全消去**的腿（手机接不进来，`p79` 已证）。

---

## 4. 纪律（本会话又花过代价的）

1. ★★ **"谁会赢"的逻辑一律写成表达式/单一归属点**，不要写"按次序赋值"——这一族已经四次（§2.1）。
2. ★ **离线自证要写清"覆盖到哪、没覆盖到哪"**（本会话 §6.138③ 的保留句就是模板）：不要拿一个证明"label 不丢"的离线臂去论证"次序也对"。
3. ★ **打戳必须在所有提交之后**（先重建后提交 = 戳落在旧提交上）；腿的身份三核对见 §3.2。
4. **腿停了等进程退出**（`[metal_stats]` 是 atexit 打的）；`ctest -L phy` 串行；腿运行时**不碰 GPU/电台**；一条臂一个变量。
5. **判据重述要留原文**（V3 的做法）；**更正写在开发文档并引用**，不回头改旧 memo。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| 开发文档 `…gpu_phy_latency_optimization_design_and_implementation.md` | ★ **§6.138 = label 前序的表达式化 + per-label 表加 min/p5 + 离线三臂 + `p83` 预登记**；§6.139 = 重建与 `ctest -L phy` 193/193；上游背景 §6.124–§6.137 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状（§0 已同步 §6.131–§6.138）|
| **`lib/phy/metal/ocudu_metal_burst.mm`** | ★ `commit()` 的 label 前序（本会话改的那一处）；`ablation_for_cb()`/`ablate_next_burst()`/`ablation_pipeline_lazy()` 也在这里 |
| **`lib/phy/metal/ocudu_metal_queue.mm`** | ★ Q9-F3 per-label 表（本会话加了 `min`/`p5` 与 `pct_sorted`）—— 读窗口的主尺子 |
| `lib/phy/upper/channel_modulation/metal/ocudu_demod.metal` | `lane_ablate_noop` kernel |
| `doc_chinese/phy_latency/wip/dft_dispatch_cost.mm` | 离线派发/窗口 harness（~40 条臂，全部反例）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` + `logs/` | 起腿（`--option=value`，`OCUDU_*=…` 走环境变量）/ 日志（`p72`–`p82`）|

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-27-3.md`，然后读开发文档 **§6.138**（label 前序表达式化 + per-label 表 min/p5 + `p83` 预登记）与 **§6.131–§6.137**（消去法与本族四次缺陷）。
> **主线判据全绿**，唯一开放项是用户裁定的"**一跳里那 ~450–470 µs**"：已收窄成"**一跳里恰好一条 cb 吃掉这笔钱、哪一条随结构搬家**"，且**不是**等待/足迹/队列排空/绑定大小/线程组串行/同伴车道/派发价。
> **本会话已把消去法的仪器修好并离线自证**（`merged_hop_ablated` exec p50 **20.5 µs**，对照 18.1 的空口单样本），**`ctest -L phy` 193/193**。
> **唯一动作**：飞 **`p83-n78-ablate8`**（命令见 §3.1；需要 sudo + 手机）。判据：`merged_hop_ablated` 的 exec p50 **20–80 µs** ⇒ 那 ~450 µs **就是这些 kernel 的执行**（下一轮二分 eq/CE/demap）；仍 **~450 µs** ⇒ 与"活"无关 ⇒ **本平台无杠杆**，这一线以"已定价、不可动"收口。**备用读数**：`merged_hop` 的 `min`/`p5`（低尾）。
> 开工先对齐三条：`git log --oneline -1` = `build/hashes.h` = `gnb` 内嵌戳（§3.2）；判 `ctest -L phy` 用**串行**；腿停了**等进程退出**再读 `[metal_stats]`。
> 纪律：**"谁会赢"的逻辑写成表达式/单一归属点**（本族已四栽）、**飞前先读腿自己的 `knob` 登记行**、**先离线自证再飞腿并写清覆盖范围**、**一条臂一个变量**。
