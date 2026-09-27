# Session handoff — 2026-09-27 #3

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份** —— 现在就是本文件）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，日期取会话开始那天，序号是那天第几份。
> **上一份 = `session_handoff_2026-09-27-2.md`**（它写于本轮之前，覆盖到 §6.137）。
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`：
> **上一份 memo 之后新增的是 §6.138–§6.139**。`high_level_status_and_plan.md` 是高层现状（§0 已同步）。

---

## 0. 一句话现状

**主线判据全绿**（V1 1392.5–1400.9 / V2 池绿 / V3 按重述判据收口 / V4 `cbs/lane=2.00` / V5 契约 9/9、`gaps=0`）。
本会话把上一份 memo 的**唯一开放项**（一跳里那 **~450–470 µs**）推到了**只剩一次手机测试**的位置，并且**已经把那条腿飞了一条**：

> `p83-n78-ablate8` 的**读数**：label 修好了（`merged_hop_ablated` **n=17583** = 1/8），但那 ~450 µs **没动**（466.5 vs 对照 470.5）——
> **可是这条腿不作数**：读码发现 **`OCUDU_LANE_ABLATE` 够不着前端 DFT 的网格写**（前端引擎自己开 encoder，从不经过 `shared_burst::encoder()`），
> 而按 `p75` 的分解**那才是 ~445 µs 的那部分**（p83 真正消去的是 eq+demap 的 ~24 µs 档）⇒ 阳性对照（`dft_front_end` 的 min/p5）**没塌**，按预登记**作废**。
> **本会话已把仪器补全**（默认关，交付不变）并**离线自证**：`dft_release_adopt_metal_test`（D1 交棒 harness）在 `EVERY=1` 下 **`FAIL: the ordinary block path left 300 of 300 grid elements unwritten`**。
> ⇒ **唯一动作 = 飞 `p84-n78-ablate8`**（与 p83 逐字相同）：这一次那条 cb 里**已经没有真活了**。

**当前状态**：代码 + 文档已提交（`git log -1` 见下）；**已按提交重新打戳并重链 `gnb`** ——⚠ **飞之前再核一次**（§3.2）；**工作区干净**、**无腿在跑、无 gNB 在跑**。
**交付配置**：收环 512 / 发环 64 / 池 32 / `otw_format: sc12` / `srate 23.04`（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。

| 判据 | 现状 |
|---|---|
| V1 `[ul_gpu_pipeline]` 中位 ≤2150 | ✅ **1400.9 µs**（`p83`；`p81`/`p82` 1392.5/1399.8）|
| V2 池 | ✅ `starved_events=0`、`dropped=0`（`p83`：`held_max=9`、`free_min=23`）|
| V3 电台（**判据已重述**：`gaps==0` + 每 DL 递交失败率带宽）| ✅ gpu 0.123–0.202%、cpu_gpu 0.1467%、cpu 0.0012–0.0068%；⚠ `p83` 有 **1 gap / 1 rx overflow**（契约 8/9），属 V3 家族偶发 |
| V4（代理量）`cbs/lane ≤2.00` | ✅ 2.00（`p83` `max=5`）|
| V5 契约 / `gaps` / crossings | ✅ 9 of 9 / 0 / 0.00+0.00 每跳（`p83` 因那 1 次电台 gap 读 8/9）|
| **那 ~450–470 µs** | ⏳ **p83 已飞但因仪器漏洞作废；仪器已补全 + 离线自证，只差 `p84`** |

---

## 1. 本会话做了什么（§6.138–§6.140）

| 动作 | 结论 |
|---|---|
| ★ **修 label 前序**（`ocudu_metal_burst.mm` 的 `commit()`）| 不"挪一行"，而是**把次序变成值**：`ablated ? "merged_hop_ablated" : (merged ? "merged_hop" : "lane_burst")` ⇒ **没有语句能再"后写覆盖先写"**。**p83 证明修好了**：`merged_hop_ablated n=17583`（正好 1/8）|
| ★ **per-label 表加 `min`/`p5`**（`ocudu_metal_queue.mm`）| wait/exec 各四个读数；排序只做一次。**p83 就是靠它读出低尾**（也正是它证明阳性对照没塌）|
| ★ **飞 `p83-n78-ablate8`** | 对照干净（`merged_hop` 470.5、`ch_wt` 41.7、`cbs/lane=2.00`、池绿、`stale=0`）；消去总体 **466.5**（−4.0）；**CRC 87.5% → 49.4%** ⇒ 消去确实咬到了 eq/demap |
| ★ **发现并补全仪器漏洞** | `OCUDU_LANE_ABLATE` 的绑定点在 `shared_burst::encoder()`，**前端 DFT 引擎自己开 encoder** ⇒ **前端网格写从未被消去**（那才是 ~445 µs 的部分）⇒ p83 的"没动"**不可解读** |
| ★ **仪器补全（默认关）** | `shared_burst::ablate_cb/ablation_noop/forget_ablation`（全进程、以 cb 为键、单一决策）；前端**建块时问一次**；**网格写 + plain 路**两处按答案换 kernel；三条自带 label 的提交路径**归还决策**；决策表**有界**（65536 + ticket 防地址复用误删）|
| **离线自证（四臂）** | `dft_release_adopt_metal_test`：control PASS / `EVERY=1` **`FAIL: … left 300 of 300 grid elements unwritten`**；`dft_processor_metal_unit_test`：ALL OK / 8 条数值 FAIL；延迟链：5 PASSED / 2 FAILED；`ctest -L phy` 串行 **193/193** |

---

## 2. 增量结论（必读）

1. ★★ **消去法的覆盖范围必须逐个 dispatch 问清楚**："旋钮开着"≠"我关心的那段被消去了"。p83 的教训：**先写下"这条 cb 里有哪些 dispatch、各自由谁编码、谁提交、label 是谁"**，再飞。
   （本会话的通用问法现在写进了代码注释：凡不是通过 `shared_burst::encoder()` 编码的 dispatch，都必须**主动来问** `ablate_cb()`。）
2. ★ **同一族缺陷的第四次**：`p80`（决策点在"新建 cb"分支）、`p81`（`thread_local`）、`p82`（label 被覆盖行盖掉）、本会话（**"谁会赢"写成按次序赋值**）。
   ⇒ **纪律**：凡"谁会赢/谁说了算"的逻辑，写成**表达式或单一归属点**，不要写"按次序赋值"。
3. ★ **阳性对照要选"旋钮必然覆盖到的总体"**：我在 §6.138④ 用 `dft_front_end` 当阳性对照，**机制是错的**（它同样不过 `shared_burst::encoder()`）⇒ 它的"没塌"被我误读成风险。
   §6.140③ 补全之后，`dft_front_end` 才是**正确**的阳性对照（现在它的 1/8 真的会被消去）。
4. ★ **p83 的行为证据不受仪器漏洞影响**：**CRC-OK 87.5% → 49.4%** ⇒ 空解映射器确实在跑。**行为证据与窗口证据要分开记账**。
5. **p83 的其它数字**（将来引用）：`dft_front_end n=361321 exec 46.6`、`ce_weights n=140664 exec 40.4`、`late_handed n=2571 exec 50.1`、busy split `ch_wt 41.7 + merged_hop 472.2`、`lanes=140659`、`released=143215`、`fence wait` 45975 次 median 34.1 µs（**不是**那 ~450 的来源）。
6. **已知口径**：`dft_front_end`（plain 路）不是融合车道的前端；一跳墙钟 **1935 µs**（换量纲必须用它）；`merged_hop` 的 `commit→start` 在 (B) 修复后是 ~215 µs。

---

## 3. 下一步（**只有一件事**）

### 3.1 ★ 飞 `p84-n78-ablate8`（命令与 `p83` **逐字相同**，只换腿名；需要 sudo + 手机）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p84-n78-ablate8 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8
```

**接手机之前**核三行：① `knob : OCUDU_LANE_ABLATE=1` **与** `…_EVERY=8` 都在；② `[metal_ablate] ABLATION ON …`；③ 戳一致（`run_leg.sh` 自己会拒）。

**判据（对照 = 同腿 `merged_hop`；p83 的 470.5 可作历史对照）**：

| 读数 | 若 | 则 |
|---|---|---|
| **主**：`merged_hop_ablated`（预期 n ≈ 1.7 万）exec p50 | ★ **塌到 ~20–80 µs** | **那 ~450 µs 就是这些 kernel 的执行** ⇒ 下一轮用同一旋钮二分 **前端网格写 vs eq/demap**（现在两者都能单独消去）|
| 同上 | ★ **仍 ~450–470 µs** | **这条 cb 的账与"活"无关**（同网格、同绑定、同屏障、同提交，里面什么都没干）⇒ 与 §6.131 的四族否证合起来，这一线**收口**为"已定价、不可动" |
| ★ **阳性对照**：`dft_front_end` 的 `min`/`p5` | **应**塌到个位数（这次它的 1/8 真会被消去）| 若**不塌** ⇒ 旋钮没进二进制/没建 pipeline ⇒ **腿作废**（p83 就是栽在这里）|
| 红线 | `ch_wt`（41.7）/ `cbs/lane=2.00` / 池 / `stale=0` 不许变；`gaps` 看电台侧偶发（p83 有 1 次）| —— |

**备选臂**（若想同时钉死"结构"这一维）：`p85` = p84 + `OCUDU_DFT_RELEASE_BLOCK=0`（D1 关，**只作测量臂**）：那时最贵的 cb 是 lane 的 burst（`p77` 4 条派发合一条 = 516.7），而它的 4 条派发**全部**经 `shared_burst::encoder()` ⇒ 同样被覆盖。

### 3.2 飞之前的身份核对（三条，缺一不可）

```bash
git -C /Users/jiachengwang/dev/ocudu log --oneline -1
grep build_hash /Users/jiachengwang/dev/ocudu/build/hashes.h
grep -ac "$(git -C /Users/jiachengwang/dev/ocudu rev-parse --short=10 HEAD)" /Users/jiachengwang/dev/ocudu/build/apps/gnb/gnb
```
三者必须一致。**若之后又提交了任何东西**（例如本 memo 的后续修订）⇒ 必须 `touch build/hashes.h && cmake --build build --target gnb` 重打戳重链，否则 `run_leg.sh` 会拒。

### 3.3 之后的岔路（等 3.1 的答案再选）

* 若"就是执行" ⇒ 二分到具体 dispatch 后考虑 **kernel 形状**（§6.80 的老清单：一个线程组做多个变换、降 tgmem/寄存器 footprint），**先在离线 harness 里量形状**；
* 若"与活无关" ⇒ 把这一线封存，回到 G1/G2 指向的**链条延长**（LDPC→Metal，用户裁定**单独规划**）或 S-E（最后选项）。

### 3.4 明确不要做的

* 宿主 staging / 拷贝（破 G1）；`OCUDU_DFT_RELEASE_BLOCK=0`、`OCUDU_CE_LANE_ORDER=event` 作为**交付结构**（破 G2，只可作测量臂）；
* V3 传输参数（四个机制已收口）；**全消去**的腿（手机接不进来，`p79` 已证）。

---

## 4. 纪律（本会话又花过代价的）

1. ★★ **"谁会赢"的逻辑一律写成表达式/单一归属点**，不要写"按次序赋值"——这一族已经四次（§2.2）。
2. ★★ **消去法飞之前，先列出"这条 cb 里每个 dispatch 由谁编码、谁提交、label 是谁"**，逐条确认旋钮覆盖得到；**凡不过 `shared_burst::encoder()` 的 dispatch，它必须主动来问 `ablate_cb()`**。
3. ★ **阳性对照必须选"旋钮必然覆盖到的总体"**（否则它的"没塌"会被误读成"消去没生效"）；**行为证据（CRC）与窗口证据分开记**。
4. ★ **打戳必须在所有提交之后**（先重建后提交 = 戳落在旧提交上）；腿的身份三核对见 §3.2。
5. **腿停了等进程退出**（`[metal_stats]` 是 atexit 打的）；`ctest -L phy` 串行；腿运行时**不碰 GPU/电台**；一条臂一个变量。
6. **判据重述要留原文**（V3 的做法）；**更正写在开发文档并引用**，不回头改旧 memo。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| 开发文档 `…gpu_phy_latency_optimization_design_and_implementation.md` | ★ **§6.140 = p83 读数 + 仪器漏洞 + 仪器补全 + 离线自证 + `p84` 预登记**（最要紧）；§6.138–§6.139 = label 前序表达式化 / min-p5 / p83 之前的重建复核；上游背景 §6.124–§6.137 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状（§0 已同步 §6.131–§6.140）|
| **`lib/phy/metal/ocudu_metal_burst.mm/.h`** | ★ `commit()` 的 label 前序；`ablate_cb()`/`ablation_noop()`/`forget_ablation()`（**任何模块都能问消去决策**）；有界决策表（65536 + ticket）|
| **`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm`** | ★ 前端**建块时问一次**（`begin_block`）、**网格写**与 **plain 路**两处按答案换 kernel、`commit_front_end`/`commit_late_handed_block`/`discard_open_block` 归还决策 |
| **`lib/phy/metal/ocudu_metal_queue.mm`** | Q9-F3 per-label 表（`p50/p95/min/p5`）—— 读窗口的主尺子 |
| `lib/phy/upper/channel_modulation/metal/ocudu_demod.metal` | `lane_ablate_noop` kernel |
| `lib/phy/generic_functions/metal/test/dft_release_adopt_metal_test.mm` | ★ **D1 交棒离线 harness**（消去是否真的换掉前端网格写的**阳性对照**）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` + `logs/` | 起腿 / 日志（新会话找 `p83`、将来的 `p84`）|

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-27-3.md`，然后读开发文档 **§6.140**（p83 读数、仪器漏洞、仪器补全、离线自证、`p84` 预登记）与 **§6.131–§6.138**（消去法与本族四次缺陷）。
> **主线判据全绿**，唯一开放项是用户裁定的"**一跳里那 ~450–470 µs**"。
> **`p83` 已飞但作废**：label 修好了（`merged_hop_ablated` n=17583、窗口 466.5 vs 470.5），**但前端网格写从未被消去**（前端引擎自己开 encoder）⇒ 按预登记（阳性对照没塌）不作数；
> **仪器已补全并离线自证**（`dft_release_adopt_metal_test` 在 `EVERY=1` 下 `FAIL: … left 300 of 300 grid elements unwritten`），`ctest -L phy` 193/193。
> **唯一动作**：飞 **`p84-n78-ablate8`**（与 p83 逐字相同，命令见 §3.1；需要 sudo + 手机）。这一次那条 cb 里**已经没有真活了**：塌（~20–80 µs）⇒ 那 ~450 就是这些 kernel 的执行；仍 ~450 ⇒ 与"活"无关 ⇒ **本平台无杠杆**，这一线收口。
> 开工先对齐三条：`git log --oneline -1` = `build/hashes.h` = `gnb` 内嵌戳（§3.2）；判 `ctest -L phy` 用**串行**；腿停了**等进程退出**再读 `[metal_stats]`。
> 纪律：**"谁会赢"的逻辑写成表达式/单一归属点**、**消去法飞前逐条列出"这个 cb 里每个 dispatch 由谁编码/谁提交/label 是谁"并确认旋钮覆盖得到**、**阳性对照要选旋钮必然覆盖的总体**、**飞前先读腿自己的 `knob` 登记行**、**一条臂一个变量**。

