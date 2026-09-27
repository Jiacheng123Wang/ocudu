# Session handoff — 2026-09-27 #2

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份** —— 现在就是本文件）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，日期取会话开始那天，序号是那天的第几份。
> **上一份 = `session_handoff_2026-09-27-1.md`**（它写于本轮之前，覆盖到 §6.123）。
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`：
> **上一份 memo 之后新增的是 §6.124–§6.136**（本会话的全部读数与更正都在里面）。`high_level_status_and_plan.md` 是高层现状。

---

## 0. 一句话现状

**主线判据全绿**（V1 1392.5 / V2 池绿 / V3 按重述判据收口 / V4 `cbs/lane=2.00` / V5 契约 9/9、`gaps=0`；IQ→LLR 收口审计 29/0/0）。
本会话**只做一件事**：查用户裁定的"那 **~450–470 µs** 未归属窗口"。结论已经**收窄到一条 cb、一句话**，并且**只剩最后一个动作**就能定案：

> **一跳里恰好一条 cb 吃掉 ~450–470 µs 的设备执行时间**（总量 ~505–517 µs 与结构无关，"哪一条 cb"随结构搬家）；那 ~450 µs **不是**等待、不是足迹、不是队列排空、不是绑定大小、不是线程组串行、不是同伴车道、不是派发价；
> 它**是否就是那几个 kernel 的执行**，由**消去法**回答 —— 消去旋钮已经做好、离线自证过，**只差一个 label 覆盖次序的小修**（见 §3.1）。

**当前状态**：HEAD = `4559ce9f62`（本 memo 之前最后一次代码提交）；`build/hashes.h` = `build/apps/gnb/gnb` 内嵌戳 = HEAD；**工作区干净**（除本 memo）；**无腿在跑、无 gNB 在跑**。
**交付配置**：收环 512 / 发环 64 / 池 32 / `otw_format: sc12` / `srate 23.04`（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。

| 判据 | 现状 |
|---|---|
| V1 `[ul_gpu_pipeline]` 中位 ≤2150 | ✅ **1392.5 µs**（`p81`/`p82` 同量级）|
| V2 池 | ✅ `starved_events=0`、`dropped=0` |
| V3 电台（**判据已重述**：`gaps==0` + 每 DL 递交失败率带宽）| ✅ gpu 0.123–0.202%、cpu_gpu 0.1467%、cpu 0.0012–0.0068% |
| V4（代理量）`cbs/lane ≤2.00` | ✅ 2.00（`p77`–`p82` 多条腿 `max=5`，均值仍 2.00）|
| V5 契约 / `gaps` / crossings | ✅ 9 of 9 / 0 / 0.00+0.00 每跳 |
| **那 ~450–470 µs** | ⏳ **已定价、机制已二分到"一条 cb"，最后一个动作见 §3.1** |

---

## 1. 本会话做了什么（腿 `p72`–`p82` + 开发文档 §6.124–§6.136）

| 腿 | 变量 | 一句话结论 |
|---|---|---|
| `p72`/`p73`/`p74` | `OCUDU_DFT_BATCH_SYMBOLS=14/7/2` | 拆包只会更差（窗口 468.7→505.8→703.4，V1 放大 **1.77–1.99×**）⇒ **每条派发 ~37–39 µs（与离线同价）**，打包仍最优 |
| `p75` | `OCUDU_LANE_DIAG_SPLIT=1` | 那 ~445 µs **就在承载前端块的那条 cb 里**（86% of busy）；总量 515.7 ≈ 融合臂 513.9 ⇒ **总量与结构无关** |
| `p76` | 并发 1 | 窗口**不动**（464.2 vs 468.7）⇒ **不是"等同伴车道"** |
| `p77` | `OCUDU_DFT_RELEASE_BLOCK=0`（关 D1）| ★ 前端块（**引擎提交、前端队列**、仍是一条 14 组派发）exec **46.9 µs** ⇒ **打包派发是便宜的**；而 lane 提交的 burst（4 条派发）**516.7 µs** ⇒ **同一批派发：一条 cb 517 / 两条 cb 70** |
| `p78` | （空臂：命令行沿用了 `p77` 的 `RELEASE_BLOCK=0`，**没带消去旋钮**）| 链路正常（102k CRC-OK）⇒ 身份 = **`p77` 的第二次复现**（457.4 vs 459.1、47.0 vs 46.9）|
| `p79` | 全消去（`OCUDU_LANE_ABLATE=1`）| **手机接不进来**：上行解不出来就**无法接入**（接入本身要解 Msg3/RRC）⇒ 改成 1-in-N |
| `p80` | 1-in-8 消去 | 只命中 **1 次**（决策点在"新建 cb"分支，合并路是 **adopt** 的）⇒ 那 1 个样本 **18.1 µs**（vs 对照 468.6）|
| `p81` | 1-in-8（决策移到 `encoder()`）| `merged_hop_ablated` **没出现**（label 在编码线程写、提交线程读）|
| `p82` | 1-in-8（决策改成 **cb 键**的进程级表）| 仍**没出现**（见 §3.1：label 被后面的 `merged_hop` 覆盖行盖掉）|

**离线（`wip/dft_dispatch_cost.mm`，本会话新增约 10 条臂，全部为反例）**：空口混合（plain 路/同 kernel/占满每核 32 KiB 线程组内存）、537 MB 足迹、设备侧 event 等待、队列排空、绑定缓冲 64 MB、编码后等 0.5/2 ms 再提交、cb 内部时间线（宿主轮询 + 设备 marker 两种，均被 event 的 cb 粒度可见性否掉）。

---

## 2. 增量结论（必读）

1. ★ **窗口是真执行**：设备侧 event 等待**不在** `GPUStart→GPUEnd` 里（等待 11.8 µs = 对照 11.8 µs，宿主实测 752 µs）；队列排空不算（后面挂 2673 µs 的缓冲，窗口仍 11.9 µs）；足迹无关（537 MB）；绑定大小无关（4×64 MB）。
2. ★ **"线程组串行"作废**：`p77` 里同一条 14 组派发（引擎提交、前端队列）= **46.9 µs** ⇒ 空口的派发价与离线一致（37–43 µs/条），**组数几乎免费**。
3. ★ **结构无关的总量 + 会搬家的"贵 cb"**：一跳设备时间合计 **~505–517 µs**；其中**恰好一条 cb 吃 ~450–520**，其余几十 µs：
   融合 = `merged_hop` 473 + `ch_wt` 41；拆分 = `dft` 445.8 + 45.5 + 24.4；关 D1 = 前端 cb ~50 + `ch_wt` 41 + burst **516.7**。
4. ★ **最强的一对对照**：**同一批 4 条 burst 派发**（eq + demap + 2×CE）在 `p77` 合在一条 cb = **516.7 µs**，在 `p75` 拆成两条 cb = **69.9 µs**（回到离线价）。
5. 候选收窄（尚未有反例）：最贵的那条 cb 都满足"**lane 提交 × 触及资源网格**"（`p77` 引擎提交的写网格 cb 只有 46.9）。
6. 已知口径：`dft_front_end`（plain 路，46.6 µs）不是融合车道的前端；一跳墙钟 **1935 µs**（换量纲必须用它）；`merged_hop` 的 `commit→start` 在 (B) 修复后才变 ~215 µs（此前 37 µs 不可比）。

---

## 3. 下一步（按顺序，第一步就能定案）

### 3.1 ★ 第一个动作：把消去跳的 label 放到 `merged_hop` 覆盖**之后**（一行）

`ocudu_metal_burst.mm` 的 `commit()` 里，label 次序是：
```cpp
const char* burst_label = "lane_burst";
if (ablation_for_cb(cb, false)) { burst_label = "merged_hop_ablated"; }   // ← 现在在这里
#if defined(OCUDU_METAL_STATS)
  if (s.commit_label == gpu_lane_probe::stage::merged_hop) { burst_label = "merged_hop"; }  // ← 后面把它盖掉了
#endif
```
⇒ **把消去那段挪到 `#if defined(OCUDU_METAL_STATS)` 块之后**（消去跳优先），重建 `gnb`，然后飞 `p83-n78-ablate8`（命令见下，只换腿名）：
```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p83-n78-ablate8 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8
```
接手机**前**核两行：`knob : OCUDU_LANE_ABLATE=1` + `…_EVERY=8`；`[metal_ablate] ABLATION ON …`。
**判据（同腿两总体）**：per-label 表里必须有 **`merged_hop_ablated`（n ≈ 1.8 万）**：
* exec p50 **~20–80 µs** ⇒ ★ **那 ~450 µs 就是这些 kernel 的执行** ⇒ 下一轮用同样手法二分 **eq / CE / demap**（`OCUDU_LANE_ABLATE_STAGE=…`，需要时再加），杠杆回到"换 kernel 形状 / 让一个线程组做多个变换"；
* 仍 **~450 µs** ⇒ 与"活"无关 ⇒ 平台给这条 cb 的账，**本平台无杠杆** ⇒ 这一线以"已定价、不可动"**收口**（把 §6.124–§6.136 的全部否证整理成一页）。
* 备用（即使 label 再出问题也能读）：给 per-label 表加 **`min`/`p5`**，从 `merged_hop` 的**低尾**直接看消去样本（廉价、对以后所有腿都有用）。

### 3.2 之后的岔路（等 3.1 的答案再选）

* 若"就是执行" ⇒ 二分到具体 kernel 后，考虑 **kernel 形状**（§6.80 的老清单：一个线程组做多个变换、降 tgmem/寄存器 footprint），并**先在离线 harness 里量形状**；
* 若"与活无关" ⇒ 把这一线封存，回到 G1/G2 指向的**链条延长**（LDPC→Metal，用户裁定**单独规划**）或 S-E（最后选项）。

### 3.3 明确不要做的

* 宿主 staging / 拷贝（破 G1）；`OCUDU_DFT_RELEASE_BLOCK=0`、`OCUDU_CE_LANE_ORDER=event` 作为**交付结构**（破 G2，只可作测量臂）；
* V3 传输参数（四个机制已收口）；**全消去**的腿（手机接不进来，`p79` 已证）。

---

## 4. 纪律（本会话又花过代价的）

1. ★★ **"每 burst / 每线程"的状态要问一句：读到它的线程是不是写它的线程**（`burst_state` 是 `thread_local`）。本会话**连续三次**栽在这里：`p80`（决策点只在"新建 cb"分支，合并路是 adopt）、`p81`（编码线程写、提交线程读）、以及 §3.1 的 label 覆盖次序。**新增状态一律用进程级 + 以 cb 为键**。
2. ★ **飞之前先读腿自己的 `knob` 登记行**（stderr 顶部）：`p78` 因为沿用了上一条命令的 `OCUDU_DFT_RELEASE_BLOCK=0` 而变成空臂。
3. ★ **先离线自证再飞腿**：本会话因此避免了 4 条无效腿（cb 内部时间线三种做法全部在离线被否；等待/足迹/队列排空/绑定大小四族在离线被否）。
4. **腿停了等进程退出**（`[metal_stats]` 是 atexit 打的）；`ctest -L phy` 串行；腿运行时**不碰 GPU/电台**；一条臂一个变量。
5. **判据重述要留原文**（V3 的做法）；**更正写在开发文档并引用**，不回头改旧 memo。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| 开发文档 `…gpu_phy_latency_optimization_design_and_implementation.md` | ★ **§6.124–§6.136 = 本轮窗口调查的全部读数与更正**（§6.129 的结构律与 §6.131–§6.136 的消去法最要紧）|
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状（§0 已同步本轮结论）|
| **`lib/phy/metal/ocudu_metal_burst.mm`** | ★ **§3.1 的一行修在这里**（`commit()` 的 label 次序）；`ablation_for_cb()`/`ablate_next_burst()`/`ablation_pipeline_lazy()` 也在这里 |
| `lib/phy/upper/channel_modulation/metal/ocudu_demod.metal` | `lane_ablate_noop` kernel |
| `doc_chinese/phy_latency/wip/dft_dispatch_cost.mm` | ★ 离线派发/窗口 harness（~40 条臂，全部反例；含 cb 内部时间线的两种失败尝试）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` + `logs/` | 起腿（`--option=value`，`OCUDU_*=…` 走环境变量）/ 日志（本会话 `p72`–`p82`）|
| `lib/phy/metal/ocudu_metal_queue.mm` | Q9-F3 per-label 表（wait/exec p50/p95）—— 读窗口的主尺子 |

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-27-2.md`，然后读开发文档 **§6.129（结构律）** 与 **§6.131–§6.136（消去法）**。
> **主线判据全绿**，唯一开放项是用户裁定的"**一跳里那 ~450–470 µs**"：它已经收窄成"**一跳里恰好一条 cb 吃掉这笔钱、哪一条随结构搬家**"，而且**不是**等待/足迹/队列排空/绑定大小/线程组串行/同伴车道/派发价。
> **第一个动作（一行）**：把 `lib/phy/metal/ocudu_metal_burst.mm` 的 `commit()` 里 `burst_label = "merged_hop_ablated"` 那段挪到 `#if defined(OCUDU_METAL_STATS)` 的 `merged_hop` 覆盖**之后**（现在被它盖掉，所以 `p80`–`p82` 都读不到消去跳），重建 `gnb` 后飞 **`p83-n78-ablate8`**（命令见 §3.1）。
> **判据**：`merged_hop_ablated`（n≈1.8 万）的 exec p50 落在 **20–80 µs** ⇒ 那 ~450 µs **就是这些 kernel 的执行**（下一轮二分 eq/CE/demap）；仍在 **~450 µs** ⇒ 与"活"无关 ⇒ **本平台无杠杆**，这一线以"已定价、不可动"收口。
> 开工先对齐三条：`git log --oneline -1` = `build/hashes.h` = `gnb` 内嵌戳；判 `ctest -L phy` 用**串行**；腿停了**等进程退出**再读 `[metal_stats]`。
> 纪律：**新增"每 burst/每线程"状态一律改成进程级 + 以 cb 为键**（本会话三次栽在这上面）、**飞前先读 `knob` 登记行**、**先离线自证再飞腿**、**一条臂一个变量**。
