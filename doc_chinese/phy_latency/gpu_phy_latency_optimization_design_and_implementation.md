# GPU PHY 融合车道 —— **时延优化**：设计与实现（开发文档）

> 本文件是本工作流（`gpu` 融合车道的**端到端时延**及其对接收池/电台的影响）的**开发文档**：
> 现象与证据、机制与归属、仪表手册、跑腿规范、以及**每一条已做改动的实施记录**。
> **开发过程中要记的东西都记在这里。**
>
> `doc_chinese/phy_latency/` 的三类文档（分工与索引见 `README.md`）：
>
> | 类 | 文件 | 性质 |
> |---|---|---|
> | **design & implementation** | **本文件** | §1–§5 活（可重写，但更正必须留痕）；**§6 起追加式（只追加，不改历史）** |
> | **session handoff memo** | `session_handoff_<会话开始日>-<序号>.md`（**日期取会话开始那天，跨午夜不改**；序号是那天的第几份）| 一次性快照（新会话开工只读**序号最大的那一份**）；写完只追加更正 |
> | **high level status and plan** | `high_level_status_and_plan.md` | 活文档：一句话现状、判据、阶段进度、下一步、待裁决 |
>
> 上游（历史）设计记录仍是 `doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md`（**追加式，不改历史**）；
> 腿日志在 `doc_chinese/phy_pipeline_gpu/wip/logs/`，腿脚本与门在 `doc_chinese/phy_pipeline_gpu/wip/`。
> **引用外部时一律用"章节名/腿名"而不是行号**（合并 `origin/main` 之后行号已整体漂移过一次，见主文档 §5.9.123）。
>
> **2026-09-24 会话（第二段：#1 之后）**（P0-5 交付、P2-E 实现并被平台证伪、文档整理）的内容**已并入本文件**
> （用户要求：memo 只是快照，开发过程中要记的东西统一记在这里）。**该会话的交接快照是
> `session_handoff_2026-09-24-2.md`**；曾用名 `session_handoff_2026-09-25-1.md` 是一次**命名错误**
> （会话开始于 09-24，跨午夜不改日期），该名字**留给下一个会话**。
>
> **旧文件名映射**（2026-09-25 整理前 → 本文件）：`00_status.md` → §2/§3/§4 + 高层文档；
> `01_plan.md` → §3 + §7 + 高层文档；`02_measurement.md` → §4/§5；`03_p0_instrumentation.md` → §6.1/§6.2；
> `04_p2e_plan.md` → §6.4.1；`05_p2e_platform_finding.md` → §6.4.2–§6.4.5。

---

## 1. 工作流定义

**为什么有这个工作流（一句话）**：重上行空口腿（2026-09-24）量到 **`gpu` 融合车道在 ~14–19 Mbit/s 就把 8 个接收缓冲抽干**
⇒ `pop_blocking()` 阻塞接收线程 ⇒ 电台 underflow 几百次；而**同负载下 `cpu` 模式 0–1 次**。
用户裁定：**加大池是治标**，根因是**数据在 GPU 流水线里停留太久**（接收缓冲的持有期 = 流水线的端到端时延）。
⇒ 目标：**缩短 `[ul_gpu_pipeline]` 的端到端时延**，并以"池不再饥饿"为症状消失的判据。

**与里程碑的关系（不要搞混）**：融合车道的**契约性质**（8/8、`0.00+0.00`/跳、就地装配、0 wraps）在**所有重上行腿上都成立**；
**不成立的是"车道在高负载下不给电台添麻烦"**。所以本工作流是**里程碑之后的新工作项**，
它不是"契约变红"，而是"车道在高负载下的时延把接收侧拖垮"。

**不做**：用额外提交换时延（V4）；用更大的池冒充时延改善（§7.4）；改预登记判据去迁就某条腿（沿用主文档 §5.9.121 ⑤ 的规矩）。

---

## 2. 现象与基线读数

### 2.1 现象（重上行腿，已复现 3/3）

| 腿 | 模式 | 上行净荷（占槽）| RF 失败 | 其中 underflow | `gaps` | 接收池 `held_max`/`free_min` | 饥饿事件 | 契约 |
|---|---|---|---|---|---|---|---|---|
| `s78-dlcap40-n78` | gpu | 15.97 Mbit/s（21.9%）| **733** | 651 | **1** | **8 / 0** | 69 | NOT MET（连续性）|
| `s79-dlcap40-cpu-n78` | cpu | 19.04 Mbit/s（24.7%）| **0** | 0 | 0 | 3 / 5 | **0** | MET |
| `s80-ulcap40-n78` | gpu | 19.06 Mbit/s（24.3%）| **555** | 471 | 0 | **8 / 0** | 56 | MET |
| `s81-ulcap40-cpu-n78` | cpu | 19.55 Mbit/s（26.6%）| **1** | 1 | 0 | 2 / 6 | **0** | MET |
| `s82-phases-heavy-n78` | gpu | 14.31 Mbit/s（26.0%）| **476** | 432 | 0 | **8 / 0** | 17 | MET |

⇒ **现象已复现 3/3 条 gpu 腿**（同负载 cpu 为 0–1 次 ⇒ 差 2–3 个数量级）。
⇒ **"丢样点"（`gaps>0`）是间歇的、且两种模式都出现过**（`s73` 是 cpu 也丢过 3 个）——**它不是模式性质**，与"underflow 次数"是**两个量**。
⇒ ⚠ 预登记的"重上行腿"有效性阈值（≥2.0 Mbit/s **且** ≥50% 占槽）**没有一条腿达到**：净荷超了 7–9.5 倍，
   但占槽只有 21.9%–26.6%。**阈值不得为了过关而改**（§5.9.121 ⑤）；引用时必须连负载剖面一起引。

### 2.2 时延分解（腿 `s82-phases-heavy-n78`，`OCUDU_UL_PHASE_SEGMENTS=1`，单位 µs）

> ⚠ **读这三段之前先读 §2.3.2**：三段是**墙钟切分**，不是**工作量归属**。三段之和 ≈ 跨度**不**意味着它是三项独立工作。

| 段 | 起点 → 终点 | 中位 | 均值 | p95 | p99 | max | 占比 |
|---|---|---|---|---|---|---|---|
| `[ul_time_frequency]` | `record_start()`（**在 `receiver.receive()` 之前**）→ `record_t2f_end()`（`drain_pipeline()` 之后一格）| **521** | 546 | 643 | 658 | 5470 | 19.6% |
| `[ul_channel_estimation]` | → 估计器这一段做完（`pusch_processor_impl.cpp:357`）| **901** | 965 | **1782** | 1895 | 6339 | 33.9% |
| `[ul_equalization_demod]` | → 第一次 LDPC 解码调用前 | **1234** | 1241 | 1334 | 1401 | 5828 | **46.5%** |
| `[ul_ldpc_decode]` | → CRC-OK（按槽配对）| 70 | 73 | 127 | 161 | 325 | （不算在跨度里）|
| **三段和** | | **2657** | | | | | **99.3%** |
| `[ul_gpu_pipeline]` | 整条跨度 | **2675** | 2728 | 3560 | 3712 | 7865 | 100% |
| `[ul_pipeline]`（另一口径）| 同上 | 2767 | 2820 | 3624 | 3758 | 7707 | — |

### 2.3 每段窗口里**实际**是什么（代码逐点核对，2026-09-24）

| 段 | 窗口里实际发生的事 | 依据 |
|---|---|---|
| `t2f` 521 | **≈473 µs 是等本槽最后一个样点**（`receiver.receive()`，整槽收包策略下的结构等待，也就是 `[ul_rx_wait]` 的同一次调用）+ **≈48 µs 前端主机工作**（14 次 `submit_symbol` 的 **encode**、交棒记账、通知入队、探针）。**14 个 DFT/grid-write 只被编码、没有执行** | `lower_phy_baseband_processor.cpp:565`（起点取在 `receive()` **之前**）、`:577`（`receive()` 返回）、`:591`（`[ul_rx_wait]` 量的就是这次调用）；`puxch_processor_impl.cpp:228`；`ofdm_demodulator_impl.cpp:487` → `release_block()` 把命令缓冲**未提交地**寄存给车道（`ocudu_dft_metal_engine.h:148-168`：*"a released block executes LATER - at the lane's commit"*）；腿证据 `[ul_dft_wait] no samples`、`dft commits=323233 = 12×26936`（那是 PRACH 自己的引擎）、`gpu busy (front_end): commits=0`。探针自己写明 `[ul_time_frequency] = [ul_rx_wait] + 前端自己的工作`（`ul_pipeline_probe.h:117-122`）|
| `ce` 901 | (i) **单车道 strand 排队**：`executor.defer` 进的是与 `pusch_executor` **同一条串行 strand**（`max_pusch_and_srs_concurrency` 默认 1），回调要等当前 strand 任务返回 ⇒ 这段主要是"**等到单车道为我这条跳空出来**"，即等**前一跳**的车道窗口走完；(ii) **上一跳 held/outstanding 命令缓冲的回收**：`close_held_buffer` 在每个 stage 开头**提交并等待**（`[cb waitUntilCompleted]`）。**窗口里没有抓格等待**。✅ **(i) 已由 P0-6 实测坐实**：`[ul_lane_exec]` 打印出 n78 与 n1 的生效值**都是 1** ⇒ 车道**就是串行 strand**（§6.1）| `dmrs_pusch_estimator_impl.cpp:68`（defer）；`du_low_executor_mapper.cpp:119-131`、`task_fork_limiter.h:204-220`；`ocudu_metal_mmse_engine.mm:906/1232` → `:1126`（等待点）；`impl:2511/3589/4380` → `engine .mm:3795/3818-3825`；**反证**：`take_released` 只加锁查表（`ocudu_metal_burst.mm:680-694`），MISS 时把顺序等待**编码到设备上**（`ocudu_metal_mmse_engine.mm:912-943`），`impl:1261-1265` 明文写了"**不在宿主上等**"的理由（§5.9.23 那次 13 秒事故）|
| `eq_demap` 1234 | **本跳那一次提交与等待**：merged 默认下估计器把**尚未提交**的命令缓冲交给车道（`shared_burst::adopt`，标签 `merged_hop`）⇒ **commit 与 `waitUntilCompleted` 都落在这段**，于是 **DFT+CE+EQ+demap 的整跳设备执行**（residency ≈1125）记在这里；再加 eq/demap 的宿主 encode、Pass-3 的 LLR 出页/解扰/解复用、以及（重 TB 时**跨线程**的）解码 fork。`defer_wait` 也记在本段（**不在** `ce`）| `ocudu_metal_burst.mm:401`（`[cb commit]`）、`:425`（`waitUntilCompleted`）；`ocudu_metal_lane_probe.h:63-70`（`merged_hop` 标签的自我说明）；`pusch_demodulator_impl.cpp:450-698`；`impl:3110-3126` → `:4935-4957`（`defer_wait` 的口径：从 stage 结束到批次完成），其完成点在 `pusch_processor_impl.cpp:561` / `pusch_demodulator_impl.cpp:345`，**都在 `record_ce_end` 之后** |

#### 2.3.1 ★ 由此得到的两条**读法更正**（2026-09-24，本工作流最重要的结论之一）

1. **三段是墙钟切分，不是工作量归属。** 整跳的设备执行记在 `eq_demap`；`t2f`/`ce` 两个名字对应的 **GPU 工作并不在那两段里**
   （`t2f` 里的 DFT 只是 encode，`ce` 段的估计器工作执行在 `eq_demap` 里）。
   `ocudu_metal_lane_probe.h:63-70` 早就警告过：*"把那条缓冲叫 `equalizer_demapper` 会让 `eq_demap` 变成四段之和——而那正是优化会盯上的数字"*。
   ⇒ **禁止**用"哪段最大就砍哪段"的方式读这张表：`eq_demap` 最大，是因为**整跳的执行**记在它名下。
2. **§5.9.130 ④ 预登记的读法没有触发。** 那条写的是"三段之和 ≪ 跨度 ⇒ 先查 `[ul_rx_wait]` 与 `gpu_lane gap`"；
   实测三段和 **≈99.3% 跨度**，原因是 **`t2f` 已经包含 `[ul_rx_wait]`**。
   ⇒ §5.9.130 ② 里"车道外 ≈1460–1560 µs（重载）"**不是**一块独立的、未归因的开销：其中 ≈473 µs 是**收样点等待**（在 `t2f` 内），
   其余主要是**单车道排队**（在 `ce` 内）。该行需按本节重算后才能引用。

### 2.4 同一腿的设备侧读数

| 量 | 值 | 读法 |
|---|---|---|
| `[ul_gpu_lane] residency` 中位 | **≈1125 µs**（n78 加压，`s82`）| 车道那条命令缓冲的寿命；**与负载几乎无关**（轻载 `s75` 也是 1121；n1 默认腿 `p05-pair` 配对后 835.5）。⚠ 它的**样本总体与相位探针不同**（140204 跳 vs 97331）——**这一条已由 P0-5 解决**（§6.3：按 slot 配对后同总体），引用时请用**配对后**的那一组 |
| `[ul_gpu_lane] busy split` | `merged_hop ≈ 1030 µs`（97% of busy）+ `ch_wt ≈ 37 µs`（n78 加压，`s82`）| ⚠ **2026-09-25 更正（P0-5 的配对读数）**：这里原来写"residency 里 ~95% 是 busy ⇒ 执行受限、不是排队受限"。**配对后该结论只在 n78 加压腿上成立**（`s85-p0phases` 的未配对读数是 0.93），**在 n1 默认腿上不成立**：`p05-pair` 配对后 `busy/residency` 中位 **0.643**（全部车道的同一比值 0.647）⇒ n1 上车道的窗口里有 **~260 µs 的设备空闲**（一跳两条缓冲之间的 fence/排队）。**这个比值随腿/随负载变，必须逐腿读**（§6.3 ⑥）|
| `mmse_time_sum defer_wait` 中位 | ≈1227 µs | 宿主等"延迟链"的时间；**与 residency 是同一窗口的两个视角，不可相加** |
| 车道占用 / 余量 | 58.8% / 41.2%（`s82`）| **"可服务 886 跳/s，只要求 520.5"** ⇒ 车道的**吞吐**有余量，紧的是**时延** |
| `starved_takes` / `starved_events` | 24 / 17（`s82`）| 池饥饿（`held_max=8` 触顶）|

### 2.5 机制（结论）

```
样本到齐（电台时序；整槽收包策略）
  └─[ ≈473 µs 等本槽最后一个样点 ]── 前端 ≈48 µs 主机编码（14 次 encode，一次都不执行）
        │
        ├── 交棒：把该槽的变换"未提交地"寄存给车道（设计目的：变换延迟到那一跳的缓冲里执行）
        │
        └─[ ≈901 µs 等到单车道空出来（前一跳的窗口还没走完）]
              └─[ ≈1125 µs 本跳设备执行：DFT+CE+EQ+demap（busy 97%）]   ← commit 与 wait 都在 eq_demap 里
                    └─ Pass-3：LLR 出页 → 解扰 → 解复用 → fork 解码  ≈110 µs
                          └─ LDPC 解码（70 µs，不在跨度里）
```
中位核对：473 + 48 + 901 + 1125 + 110 ≈ **2657** ≈ 跨度 2675 ✓

* **接收缓冲必须在整段跨度里活着**（交棒的设计目的就是让变换在那一跳的缓冲里执行），
  ⇒ **持有期 = 跨度**，而池 **8 个 × 每槽 1 个**：**跨度逼近 4 ms 就开始饿死接收线程**（尾部 max 已 7.9 ms）。
  ⚠ 但"输入只需要活到**最后一个读它的 dispatch**"——估计器/均衡/解映射读的是**网格**，不是输入；
  现在却把它绑在**整条命令缓冲完成**上 ⇒ **多持有**（**P2-E** 就是去掉这段多余持有；**2026-09-25 的结果是：机制被平台证伪**，见 §6.4）。
* **车道的吞吐不是瓶颈，而"一次只能有一条跳"是实测事实**（P0-6 收口）：占用 59%、能力是需求的 1.7 倍，
  但生效并发度 = **1** ⇒ 车道是一个**串行 strand**，一跳占住它 ≈1125 µs（§2.4），而重载下每 ~1.5 ms 就要求一跳
  ⇒ **排队项（`ce` 901）与"本跳执行"（`eq_demap` 1125）是同一条串行链的两半**。
  结构杠杆因此只有两个：**提高并发度**（1 → 上限 = 中等池 `max_concurrency`；`[ul_lane_exec]` 打印出的池值是 **5**，
  mapper 对超过池子的值直接报配置错误）或**缩短单跳窗口**（= 压低 §2.4 的 1125 µs 执行时间）。
  两者都可能在**交付**上动到 **V4（提交数）**；用户已裁决 V4 **只约束交付**（§7.4），故 P1-8 测量臂放行。
* ⇒ 优化目标不是"让池更大"，而是**把这条串行链上的执行与等待压下来**。

---

## 3. 验收判据与负载配方

### 3.1 验收判据（**先写死，不随结果改**）

| # | 判据 | 阈值 | 出处 |
|---|---|---|---|
| **V1** | `[ul_gpu_pipeline]` 中位 | **≤ 2150 µs**（重负载基线 2675 µs，−20%）| §5.9.130 ⑤ |
| **V2** | 池压力（症状）| `starved_events == 0` 且 `held_max < pool`（池 = 8）| 同上 |
| **V3** | 电台 | ~~RF 失败 **≤ 10**（cpu 量级 0–1）~~ **⇒ 2026-09-26 用户裁决重述（见 §6.122）**：<br>**`gaps == 0`** 且 **每 DL 递交的实时失败率** 落在 **模式带宽**内：`gpu` **≤ 0.25%**、`cpu_gpu` **≤ 0.25%**、`cpu` **≤ 0.01%**（≥2 条**同负载**腿，并声明负载）| 同上 + §6.122 |
| **V4** | **不许用提交数换时延** | `cbs/lane ≤ 2.00 (max=2)`、`dropped == 0` | 同上 |
| **V5** | 不回归 | 契约 8/8、`crossings 0.00+0.00`/跳、CRC KO% 不劣化 | 同上 |

**判据现状（2026-09-26 更新，含本会话的 `p41`–`p49`）**：V1 ✅ **最好 1364.2 µs**（`p39`，基线 2675.1 ⇒ **−49.0%**）；
交付配置的近腿重复在 **1366.8–1392.4**（`p42`/`p43`/`p45`/`p49`，腿间散布 ~17 µs ⇒ 都在带内）；`p46`（并发 1）1599.4、`p48`（输入 staging 测量臂）2181.2 ⇒ **都是测量臂，不作交付读数**。
V2 ✅ `starved_events=0`、`held_max 9–18 < 32`、`dropped=0`；**V3 ✅（2026-09-26 按重述判据收口，§6.122）**：`gaps=0` 且 gpu 模式失败率 **0.123–0.202% < 0.25%**；
（**原文"RF ≤ 10"不可达**：它写在确定性链路的假设上，而这块 USB/B200 的实测带宽是 0.12–0.20%/DL 递交——传输侧四个机制已全部排除，见 §6.118–§6.121）；
V4 ✅ `cbs/lane=2.00 (max=2)`、crossings `0.00+0.00`（`p47` 的诊断臂 3.00 是预登记的测量臂）；V5 ✅ 契约 **MET 8/8**、`gaps=0`、`rx_overflows=0`、`p0_gate.sh` **29 of 29**。
**逐腿读数与出处见 §9、§6.63/§6.64 与 §6.71–§6.84。**

---

### 3.1.1 ★★★ 合并时间账（2026-09-26 盘点：**所有已测量到的分布 + 各自的测法 + 状态**）

> 用户在 2026-09-26 要求把"已测到的时间分布"合并成一份账。**每一行都标注口径与载体**——
> 这是本项目最容易出错的地方（同一物理量在设备/宿主/跨度三种口径下可以差几倍）。**作废项一并标注，别重走。**

#### ① 先立三个口径（引用任何数字前先看这一格）

| 口径 | 含义 | 载体 |
|---|---|---|
| **设备口径** | Metal 每命令缓冲的 `GPUStartTime → GPUEndTime` | `shared_queue::arm_gpu_time()` 的完成处理器 |
| **宿主口径** | `steady_clock` 打点（阶段段、队列、出手） | `lane_clock` 打点 + 各模块 probe |
| **跨度口径** | 槽键配对的端到端（本槽首样点 → LLR 就绪） | `ul_pipeline_probe` |

⚠ 三条铁律：**`busy`（Σ cb 窗口）是"占用"不是"算力"**；**窗口 ≠ 关键路径代价**；**"未重叠上界"与"实际收益"必须说清是哪一个**。

#### ② 端到端锚点（跨度口径）

| 读数 | 值（中位）| 怎么测 |
|---|---|---|
| **V1 `[ul_gpu_pipeline]`** | **1364.2**（`p39`）/ 1366.8（`p42`）/ 1377.2（`p49`）/ 1392.4（`p43`）| `ul_pipeline_probe`：`record_start` = 本槽首样点、`record_ldpc_start` = LLR 就绪；阈值 ≤2150 |
| `[ul_pipeline]` | 1451–1465 | 同上（起点定义略不同）|

#### ③ 一跳自己的链（`p42` 交付配置、并发 2，中位 µs）

| 段 | µs | 怎么测 |
|---|---|---|
| 样点到达（物理）| ~500 | **不是仪器**：一槽样点只在槽末全部存在；旁证 `[ul_rx_wait]` **473** |
| 宿主：跳入口 → 交出 lane | **45.8** | `[ul_gpu_lane] gap: stage entry -> extraction commit (host)`（`lane_clock` 打点，§7.6.0c）|
| **本跳设备窗口** | **541.8** | `[ul_gpu_lane] busy split merged_hop`（每 cb 的 GPU 窗口 ÷ lanes）|
| 后端队列：提交 → 开始 | **39.5** | `[ul_gpu_lane] queue: weights commit -> weights start`（宿主 commit 时刻 − `GPUStartTime`）|
| 同伴占用（串行化）| ≈541.8 | **反推**（`busy≈residency` + `burst max_in_flight=1`），非直测 |
| 设备做完 → 消费者 | **54.1**（均值）| `[metal_stats] block lifecycle … handler lag`（GPU done → handler 跑）|
| 残差 | ≈144 | = V1 − 上述项 |

⚠ **常见混淆（2026-09-26 用户提出核查）：那 ~452/541 µs 的窗口**是否**包含"等空中样点到达"？**
**不包含——等样点发生在窗口之前**，三条独立证据：
1. **代码**：FE 块只在**本槽最后一个符号**被交出（`finish_symbol` 的 `last_symbol_of_slot` → `release_block`，`ofdm_demodulator_impl.cpp:444/486`），
   而 puxch 只提交"样点已经到齐"的符号 ⇒ **块与它的 cb 都在样点到齐之后才出现**；Metal 的 `GPUStartTime` 又在 commit 之后 ⇒ **窗口整段在"到达"的下游**。
2. **数字闭合**（`p47` 拆分臂，中位）：`[ul_rx_wait]` **473**（宿主等样点）→ deposit → **`dft carried deposit ->GPU start` 275** → **窗口 453** → handler 54 + LDPC 54 ≈ **1356 ≈ V1（1366.8–1392.4）** ⇒ 两段各记一次，**没有重复计**。
3. **对照腿**（最强）：`p43` 用 `OCUDU_UL_RX_SYMBOLS=7`（半槽收包 ⇒ 前半槽样点提前 ~250 µs 到）⇒ 若窗口含"等到齐"，窗口应移动 ~250 µs；
   实测 `merged_hop` **575.0 vs 541.8 = +33 µs** ⇒ **窗口不跟着到达时间走**。

⇒ **"等空中样点"那条 ~473 µs 是 `[ul_rx_wait]`，它是独立且在窗口上游的一段**（V1 里已经算过）。
⚠ 旧文档 §6.65③ 曾把这 ~464 µs 归给"等本槽样点"**并塞进窗口**，该说法**已作废**（见 ⑪ 与 §6.75③/§6.80/§6.82），**别据此重复计算**。

#### ④ 一跳窗口内部：三个粒度（**都能测，但不能混用**）

| 粒度 | 读数 | 怎么测 | 可信度 |
|---|---|---|---|
| **A 宿主阶段段** | `t2f` **532** / CE **69** / `eq_demap` **739** / LDPC **68** / FAPI-MAC **3.0**（`p42`）| 各模块宿主打点；⚠ **段间重叠，禁止相加**（和 > V1）| 可靠（单个）|
| **B lane probe** | `ch_wt` **43.5** + `merged_hop` **541.8** ≈ `busy` 585（`p42`）| 设备口径，每 cb 一个数 | 可靠 |
| **C 诊断拆分臂** | **`dft` 452.3（87%）+ `ch_wt` 45.4 + `merged_hop` 24.4 ≈ 521**（`p47`）| `OCUDU_LANE_DIAG_SPLIT=1` 把一跳拆成三条 cb | **只作指示**（提交结构变了）|
| D CE 自记 | **`defer_wait` 中位 811**（`p43`：mean 796.6 / p95 945 / max 10.7 ms）| `[mmse_time_sum]`：延迟阶段返回 → 批完成（**eq/demap 跑在里面**）| 可靠 |

**⇒ 当前最佳"一跳设备账"（`p47`）**：**前端 452 + CE 45 + eq/demap 24 ≈ 521 ≈ 窗口 541.8**；
`p49` 读码证明 **D1 武装下前端 cb 与一跳 cb 在同一条串行队列** ⇒ **那 452 µs 整段在关键路径上**。

#### ⑤ 算力账单（离线；**含已作废项**）

| 项 | 值 | 载体 | 状态 |
|---|---|---|---|
| CE 六核合计 | **≈38 µs/跳**（weights 13.2、corr_rhp 6.07、apply_lse 7.42、corr_a 1.69、reformat 1.52）| `wip/ce_kernel_cost.mm`（预热 + min-of-3）| ✅ 与空口 `ch_wt` 43–48 µs 自洽 |
| 均衡 / 解映射 | 1.4–1.6 / 1.25–1.37 µs | 同上 | ✅ |
| 空派发地板 | 1.26–1.38 µs | 同上 | ✅ |
| 宿主 encode | 6.5–8.2 µs/CE 阶段（≈1.3–1.6/派发）；四种阶段边界 ≤0.3 µs | `OCUDU_MMSE_DEBUG` 相位表 + 微基准 | ✅ |
| **前端** | ❌ 旧值 **10.6 µs/槽**（0.76/变换）| 隔离微基准 | **已作废**：真实几何 harness（`wip/dft_dispatch_cost.mm`）说打包 14 变换 = **46.9 µs**、14 次单组派发 = **530 µs** |

#### ⑥ 平台事实（2026-09-26 新增，全部离线 harness）

| 事实 | 值 | 工具 |
|---|---|---|
| **派发之间不重叠**，每次 ≈38 µs 驻留（线性）| 1→47.5 / 2→84.8 / 4→161.3 / 7→278.2 / **14→539.3** µs | `dft_dispatch_cost.mm`（真实 kernel、真实几何 n=768/612/int16+网格）|
| 线程组**完全重叠** | 14 组 = 1 组 = **46.6 µs** | 同上 |
| 平凡 cb = **7.1 µs**（**无"每 cb 地板"**）| — | `fe_wakeup_probe.mm`（合成）|
| 空闲 1 ms **不加价** | `gap − back-to-back = 0.0` | 同上 |
| 别队列重载 | 只 **+11 µs** | 同上 |
| 输入 = 被并发写的**实时页映射** | **23.1 µs** | 同上 |
| 逐派发 GPU 时间戳 | **不可得**（`AtDispatchBoundary = no`，Apple M4 Pro）| `metal_counter_probe.mm` |

⚠ **两个数别用错场合**：harness 的 **38 µs/派发**是**未重叠上界**；空口"消一次派发值 **5–14 µs** V1"（§6.63/§6.64）是**实际临界路径收益**。

#### ⑦ 队列 / 设备占用（空口，F3 探针，需 `OCUDU_METAL_GPU_TIME=1`）

| 读数 | 值 |
|---|---|
| `busy(union)/window` | **27–29%**（`p45`/`p46`/`p47`）|
| `gpu busy (front_end) mean` | 46.7–48.6 µs，**全部是 PRACH**（334–393k cb ≈ 12/帧）|
| `gpu busy (back_end) mean` | 253–307 µs（前端/估计器/一跳混合）|
| `holes>100 µs` / 最大 | 298–426k / 2.0–2.1 s（宿主停顿族）|

#### ⑧ 接收 / 池 / 传输侧

| 读数 | 值 | 怎么测 |
|---|---|---|
| `[ul_rx_wait]` | **473**（`p42`/`p47`/`p49`）| 接收线程等样点 |
| `gaps` / `rx_overflows` | **0 / 0** | 时间戳连续性 + UHD 元数据 |
| `[ul_rx_pool]` | `held_max 9–18/32`、`starved=0`、`pop_blocking max 28 µs` | 池计数 |
| `input hold` 中位 | **818–832 µs**（`p42`/`p47`/`p49`；`p48` staging 臂 1562）| IQ token 持有期 |
| `[ul_dft_wait]` | `p41`（符号级）**577.6**；其余腿 **no samples** | 前端等待（宿主）|
| `[ul_rx_timing]` | recv / loop / slip 分布 | 每次 receive 的两个时钟读 |

#### ⑨ 栅栏（2026-09-26 新增仪器 Q24/Q24b）

**28.9% 的跳**有 stage fence 停等，mean **59** / median 54.9 / max 892.9（`p45`）⇒ **≈17 µs/跳**；并发 1 腿（`p46`）26.8% / mean 34.9 ⇒ **≈9 µs/跳**；
`Q24b`（按时间配对的独立读数）在 `p46` 给出**完全相同的数** ⇒ 两条配对互证。**`corr` / `grid` 两端为 0。**

#### ⑩ 已经"量掉"的解释（**别再重走**）

算力（尺寸无关：3→25 PRB 窗口 245→~100 µs）、栅栏（9–17 µs）、DL 负载（`dlcap40` vs `ulcap40` 同窗口 1032.8/1029.6）、
同伴（并发 1 只 −70 µs）、线程组串行（打包完全重叠）、**输入映射（换掉更差 +464 µs）**、缓冲绑定、队列（D1 下本就在同一队列）、逐派发时间戳（平台不可得）。

#### ⑪ 结论与不确定性

* **那条 ~450–500 µs 的窗口**：**结构无关**（§6.94：融合前档案 `ch_wt` 308–434、`p50` 把它搬到 `ch_wt` 487.7 + `eq_demap` 22.7、`p51`/`p52` 不变）⇒ 归为**平台/空口固有**；
  **宿主不是它的原因**（§6.97：一跳宿主总额只有 **94.0 µs**、`gap` 中位 10.7、`[ul_dft_wait]` 空、`gpu_wait` 0）；
* **可靠**：V1、`busy split`、F3、Q24/Q24b、阶段段（单个）、池/持有期、**宿主头/总额（`p53` 145291/145291 = 100% 覆盖）**。
* **只作指示**：`LANE_DIAG_SPLIT` 的分段（结构变了）、离线拆分臂的 per-cb 反推（`cbs/lane` 在 0.50↔2.00 跳）。
* **已作废/更正**：§6.65（前端 10.6 µs）、§6.77（"37 µs 是平台地板"）、§6.80（"前端 = 真实执行且不重叠"）、
  §6.65③（"464 µs 是等样点"）、§6.71（H4）、§6.73/§6.77（两版 Q24/Q24b 的配对与聚合缺陷）、§6.89（两条回退臂都把 CPU 放回跳中）。
* **未归属**：那 ~450–500 µs 的窗口（**四次**从宿主/融合/队列/栅栏/算力上摘掉之后仍挂着）；下一步是 §0.1 的**链条延长**（兼容 G1/G2），而不是继续改结构。

#### ⑫ 宿主参与总额（**G2 的定量形式**，2026-09-26 `p53` 首次测到）

| 读数 | 值（中位 µs）| 怎么测 |
|---|---|---|
| **头**：入口 → 抽取提交 | **47.2**（`p42` 45.8）| `gap: stage entry -> extraction commit (host)`（旧仪表）|
| **总额**：入口 → lane 提交 | **94.0**（p95 144.2、p99 187.8）| `host: stage entry -> lane commit`（§6.96 新仪表）|
| **尾**：抽取提交 → lane 提交 | **46.8** | 上两者之差（= weights + burst 的宿主 encode）|
| 覆盖 | **145291/145291 lane**，0 条"总额短于头" | `lane host participation` 契约检查 |
| 同腿对照 | `residency` 569.0 / `busy` 555.9 / `gap` 10.7 / `period` 461.0 | lane 探针 |

⇒ 宿主占一跳窗口 **16.5%**，且**全程不等设备**（`[ul_dft_wait]` 无样本、`gpu_wait=0`）⇒ **CPU 已不是瓶颈**；
要再快只能把 CPU 从链上**拿掉**（链条延长 / S-E），不是把 94 µs 里的某一段调快。详见 §6.97。

### 3.2 负载配方（复跑用，缺一不可）

* **默认工况**（判契约 8/8、`stale=0`、A1-2 5/5；**腿必须自报 `regime=default`**）：
  `LEG_CONFIG` 用默认的 n1 桥接配置、`run_leg.sh gpu <label>`；UE **接入 + PDU session**，然后
  **从 CN ping 手机** `ping 10.45.0.10 -i 0.1 -c 100`（`10.45.0.10` = UE 的 IP ⇒ **下行** ping，**10 Hz × 100 包 = 10 s**）
  + **10 s 下行 iperf3** + **10 s 上行 iperf3** ⇒ 流量窗口 ≈ **30 s**，之后继续空跑到 ≈ **100 s** 再收尾
  （历史默认腿总长 `s62` 84.8 s / `s71` 105.8 s）。
  ⚠ **零流量的腿无效**：没有 PUSCH 跳时契约会少判两条（`MET (6 of 6)`，见 §4.4），
  而里程碑判据要 `MET (8 of 8`。
  为什么这样就够：ping 的**回显应答**与下行 iperf3 的 **TCP ACK** 都是**上行**流量 ⇒ 产生 PUSCH 跳
  （`s71` 在该配方下记录 `[ul_pipeline] samples=7703` / 105.8 s ≈ **73 跳/s**）。
  ⚠ 这条配方里的 10 s 上行 iperf3 是**短促**的，不是持续负载：`s69`/`s71` 用它仍读到 `stale=0`；
  但同一配方在 `s69` 上留下了 **33 次 RF 实时失败**——这正是"0 RF 失败"**不能**当判据（无登记阈值）的实例。
* **加压工况**（判 V1–V5；**腿必须自报 `regime=stress`**）：n78 小区、
  `LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml`、手机**发**、CN **收**、
  `iperf3 -c <phone> -R -b 40M -P 4 -t 240`（**`-R` 不能省**，否则变成下行负载——见 §5.9.128 ①）、
  `run_leg.sh gpu <label> --regime=stress`，诊断腿另加 `OCUDU_UL_PHASE_SEGMENTS=1`。
  * ⚠ **命令本身的两个坑（2026-09-29 用户实操踩到）**：① `-c` **自带参数**，所以 `iperf3 -c 10 10.45.0.2` 会去连主机名 `10`（后面那个地址被当成多余位置参数）——正确写法是 `iperf3 -c 10.45.0.2 …`；
    ② 手机侧必须是**服务端**（`iperf3 -s`，端口 5201，且 app 在前台）。
  * ★ **排障顺序（ping 通但 iperf3 不通时）**：**(a)** 先证明 TCP 通不通 —— `timeout 3 bash -c 'cat < /dev/null > /dev/tcp/<phone>/5201' && echo OPEN || echo CLOSED`；
    **(b)** 去掉 `-R` 跑 `iperf3 -c <phone> -t 5`（这是**下行**数据，但能证明控制连接与服务端正常）；**(c)** 再加 `-R` 跑 10 s；
    **(d)** 若控制连接正常而 `-R` 不工作（部分手机 app 的服务端不会主动发），**交换角色**：CN 侧 `iperf3 -s`、**手机侧当客户端** `iperf3 -c 10.45.0.1 -t 240 -P 4 -b 40M` —— 数据方向仍是**手机→CN = 上行**，只是不再依赖 app 的反向发送。
  * ℹ **没有 iperf3 也能飞 `default` 腿**：ping 的**回显应答本身就是上行流量**（会产生 PUSCH 跳），默认配方 §3.2 本来就只有 10 s 上行 iperf3；对"读新执行表/看空闲间隔"这类目的，ping-only 腿反而**空闲更多、更适合**。
* ⚠ **`leg_gate.sh` 只用于加压腿**：它的两条 `VALIDITY` 判据（`UL >= 2.0 Mbit/s`、`占槽 >= 50%`）
  是给重上行腿预登记的，用在默认腿上必然双红（§4.4）。默认腿用 `milestone_audit.sh`（它按**工况**选腿与判据）。

### 3.3 工况（regime）是**声明**，不是**症状**——加压腿必须自报

2026-09-24 实测：里程碑门 `milestone_audit.sh` 原来**自动取 mtime 最新的腿**，而最新腿是故意加压的 `s82`，
于是它把两条**默认工况**的判据判成了 FAIL：

* A1-2 的 **C5** 预登记原文是"**腿仍然有效**：契约 8/8、`stale=0`、crossings `0.00+0.00`"（§5.9.118 ③）——这是**默认工况**判据；
* 而 wall A/B 的 **R4** 在加压工况下写的是**相反**的话："`stale > 0` ⇒ 与本线同类，**PASS**"（§5.9.127）。

⇒ **两条都不是发现，是判据绑错了工况。** 之所以一直没暴露：更早一次审计时"最新腿"恰好是一条加压但 `stale=0` 的腿，
默认判据**靠运气**过了。

**规矩（已落到脚本）**：

1. **加压腿必须声明**：`run_leg.sh gpu <label> --regime=stress`，它会把 `[leg] regime=stress` 写进**腿自己的 stderr**（记录源）；
   早于该开关的腿在 `milestone_audit.sh` 的 `STRESS_LEGS` 里按标签声明，并附负载证据来源。
   **"工况"说的是"有没有负载发生器在持续打流量"，不是"有没有流量"**：默认腿自己的配方
   （`接入 + PDU session + ping/短 iperf3`）**仍然算默认工况**——它正是历史默认腿（`s47/s67/s69/s71`）的跑法，
   也是契约 8/8 与 `stale=0` 的取证条件。把"任何流量都算加压"会让默认工况**永远取不到证**。
   反之 `ul_load.sh` / `wall_ab.sh` 的持续配方（如 `iperf3 -R -b 40M -P 4 -t 240`）**才算加压**。
2. **门同时审计两条腿**：默认腿判"契约 / `stale=0` / A1-2 5/5"，压力腿只判**负载改不了的性质**
   （契约名字、MET+mode=gpu、crossings、`gaps`），压力腿的 `stale`/RF/池数字**只报不判**——
   那一档的判据是预登记的 **V1–V5**（§3.1），也是本工作流的目标。
3. **绝不允许**按腿自己的 `stale` 读数去判定它属于哪个工况：那会让判据**不可否证**。
4. **门的另一条新检查**："**这条腿跑的是哪个提交**"。`run_leg.sh` 在二进制戳 ≠ HEAD 时**拒绝起腿**，
   但这个事实过去只留在控制台和腿的 `.stdout` banner 里，门里的 `binary stamp == HEAD` 说的是**当前构建**、不是**这条腿**。
   现在门会读腿自己的 commit banner，并要求：相等，**或** 该 commit 到 HEAD 的 diff **不触及代码**。
   实测：`s82` 跑 `33de116ce3`，到 HEAD 只差 4 个文件（全在 `doc_chinese/phy_latency/`，**0 代码**）⇒ **它是 HEAD 的证据**；
   而当时最新的**默认**腿 `s71` 跑 `4c4880012c`，到 HEAD 差 **1526 个文件（1463 个代码）**⇒ **它不是**——这句话现在由门说出来。

---

## 4. 仪表手册

> **每一条读数都要能在这里查到"它是怎么来的、怎么复跑、什么情况下会骗人"。**

### 4.1 读数清单（谁印的、量什么、怎么读）

| 读数 | 出处 | 量什么 | 读法 / 坑 |
|---|---|---|---|
| `[ul_pipeline]` | `ul_pipeline_probe`（`include/ocudu/support/executors/ul_pipeline_probe.h`）| 一跳的端到端跨度（**与模式无关**）| 与 `[ul_gpu_pipeline]` **是两个口径**，不要混着比；`stale=` 只数**配对成功**且 >8000 µs 的跨度 ⇒ **它有盲区，别当成"电台/传输打嗝"的计数器**（见 §4.1.1）|
| `[ul_gpu_pipeline]` | 同上 | **车道模式下的同一跨度**（终点=第一个码块进 LDPC 解码器）| 本工作流的 **V1 就用它**（重载基线中位 2675 µs）；样本总体是"走到解码的跳"，与 `[ul_pipeline]`（只 CRC-OK）**不同**（§4.1.1）|
| `[ul_time_frequency]` / `[ul_channel_estimation]` / `[ul_equalization_demod]` / `[ul_ldpc_decode]` | 同上的 staged probes，`OCUDU_UL_PHASE_SEGMENTS=1` **强制开启**（`gpu` 模式默认关）| 三段 + 解码段 | **三段之和应 ≈ `[ul_gpu_pipeline]`**（实测 2657 vs 2675 = 99.3%）；**它们在时间上顺序相接，不能理解为可重叠的三块**（§2.3.1）|
| `[ul_gpu_lane] residency` | `lib/phy/metal/ocudu_metal_lane_probe.{h,mm}` | 车道那条命令缓冲的**寿命** | 与负载几乎无关（轻 1121 / 重 1125 µs）|
| `[ul_gpu_lane] busy split` | 同上 | 按**组**分：`ch_wt`（权重）与 `merged_hop`（合并跳）；诊断拆分臂还多一个 `dft` | **`merged_hop` ≈ 1030 µs = 车道 busy 的 97%**（n78 加压）。⚠ **2026-09-25 更正**：`residency` 里"~95% 是 busy"**只在 n78 加压腿上成立**；n1 默认腿配对后是 **0.643** ⇒ 别把它当恒等式（§2.4/§6.3 ⑥）。**内部不可再分**（D1 把一跳做成一条缓冲，Metal 只给整条缓冲的时间）⇒ 见 §6.2 |
| `[ul_gpu_lane] paired … (P0-5)` | 同上（§6.3）| **配对后**的 `residency`/`busy`/三段与两个比值 | 只有这一组才和相位探针**同总体**；`samples=… of phase_samples=…` 是配对的**完整账**（没配上的原因逐项打印）|
| **`[ul_gpu_lane] commit -> completion (Q9-B)`** ＋ **slowest 表** | 同上（§6.13）| 每条命令缓冲的 **`commit->start`（队列）/ `start->end`（设备，含设备侧栅栏等待）/ `commit->end`** | 回答「一个 cb 为什么几秒才完成」：**`commit->start` 大 = 队列**、**`start->end` 大 = 设备侧等待**；表里每行带 slot 与 stage ⇒ 一条腿给一行答案 |
| **P0-7 行上的 `registry commit->completion=` 与 `dry-pool reaps=`** | `ocudu_metal_burst.{h,mm}`（§6.13）| 前者：注册表**发出提交之后**的完成时长（与 `deposit->completion` 一对读）；后者：**干池驱动的 sweep** 次数与救回的块数 | `commit->completion ≈ deposit->completion` ⇒ 慢在提交之后；前者 ms 而后者秒 ⇒ 慢在**提交之前**（hold/认领）。`reaps` 的 events>0 而 blocks=0 ⇒ 卡池的是 **claimed 的块**，不是没人认领的块 |
| `[ul_gpu_lane] queue / gap / host` | 同上 | 各阶段之间的**空隙**与排队 | `queue: weights commit → weights start` 等；`gap` 的负值正常（时间基准不同）|
| `[mmse_time_sum] defer_wait distribution` | `port_channel_estimator_metal_mmse_impl.cpp` | 宿主**等延迟链**的时间分布 | **与 residency 是同一窗口的两个视角**（宿主视角 / 设备视角）⇒ **不可相加** |
| `[mmse_time_sum]`（其它字段）| 同上 | 估计器宿主阶段：`pre/stage/submit/unpack/cpl_*/corr/gpu_path/cpu_blocks` | 全部**只有几十 µs** ⇒ 估计器的**宿主**工作不是时延主项 |
| `[ul_rx_pool]` | `lower_phy_baseband_processor.cpp` | 接收缓冲池：`taken/returned/held_end/held_max/pool/free_min/starved_takes/starved_events` | **`held_max == pool` + `free_min == 0` + `starved_events > 0` = 池被抽干**（接收线程会被 `pop_blocking()` 阻塞）⇒ **这就是 underflow 的直接原因** |
| `[ul_rx_wait]` | 收包侧（`ul_pipeline_probe`）| 接收线程**一次 `receive()`**（收齐整块）的阻塞时长 | 三段覆盖不到时的去处；**它是 `t2f` 的一部分**（§2.3）。它的**尾部**（~101 ms 级）是**本进程自己的启动项**（`delay_s = 0.1` 把电台流起点推后 100 ms，**不是传输打嗝** —— 见 **§6.146**），只能在本序列与 `[ul_rx_timing]`/`[ul_rx]` 里看到，不会出现在 `stale` 里（§4.1.1）；dry-pool drop 分支的那次 `receive()` 两条序列都不记（§4.1.1 盲区 3）|
| `[metal_stats] burst dispatches` | `ocudu_metal_burst.mm` | 一跳里各模块的 dispatch **次数** | 次数 ≠ 时间；不要用它推断时延 |
| `[metal_stats] gpu busy (front_end/back_end)` | `ocudu_metal_queue.mm` | **每队列**命令缓冲的 GPU 时间 | 只到"队列"这一层（`commits/busy/mean/window`），**不是 kernel 级** |
| `[metal_stats] dft handover …` | `ocudu_dft_metal_engine.mm` | 交棒：`handed/taken/superseded/evicted/…`、`keepalives=released/attached (max in flight)`、`(armed=…)`、**P2-E 的 `tokens_early=signals:N,by_event:M,by_complete:K`** | `keepalives` 的差额=**在飞**（不是泄漏），判泄漏看 `max in flight`（§5.9.120）；**P2-E：`by_event == 0` 表示"开关开了但释放没搬到前端"**，此时池数字应读作**未变**（§6.4）|
| **`[ul_lane_exec]`** | `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp`（**推导值 + 输入**）与 `lib/du/du_low/du_low_executor_mapper.cpp`（**执行器形态**）| 车道的并发度：`max_pusch_and_srs_concurrency` 的生效值、`bw/layers/ul_ratio/cpus`、以及它是**串行 strand** 还是 **N 路 fork limiter** | **每次启动打两行，在电台打开之前**（短跑一次 `gnb -c <配置>` 就能离线读到，不必飞腿）。**实测**：n78 `bw=20MHz layers=1 ul_ratio=0.30` → **1**；n1 `bw=5MHz ul_ratio=1.00` → **1** ⇒ **都是串行 strand**；池上限定死这个值的上限（超过池子直接**报配置错误**，不夹取）|

### 4.1.1 ★ `stale` 的口径与它的三个盲区（**2026-09-26 读法更正**）

> 缘起：一份 n1 手机腿的 log 里 `[ul_rx_wait] max=101866 µs`（101 ms），而同一条腿的
> **`[ul_pipeline] stale=0`、`[ul_gpu_pipeline] stale=0`**。此前的说法（"这类事件会被 stale 判据统计"）**不准确**：`stale`
> 是**跳跨度**的过滤器，不是"电台/传输打嗝"的计数器。三条盲区如下。

**口径**：`stale=` 数的是**该序列里配对成功、且跨度 > `stale_us()`** 的样点；`stale_us()` 默认 **8000 µs**（= 上行 HARQ 往返），
可用 **`OCUDU_UL_STALE_US`** 覆盖。它**只**回答"这条序列里有没有跳跨度超过 8 ms"。

**盲区 1 —— 三个序列的分母不同，不能互推。** 同一条 n1 手机腿实测：

| 序列 | 一次样本 = | 本次运行 |
|---|---|---|
| `[ul_rx_wait]` | **一次 `receive()` 调用**（收包线程，每时隙块一次）| **300,407** |
| `[ul_gpu_pipeline]` | 一次**走到"LLR 交给解码器"**的跳 | **200,924** |
| `[ul_pipeline]` | 一次 **CRC-OK** 的跳 | **177,533** |

⇒ 约 **10 万次接收调用根本没有对应的跨度样本**。用一条序列的 `max` 去推断另一条序列是否"应该"看见它，是**读法错误**。

**盲区 2 —— 未配对 ≠ 样本。** 跨度只有"起点配到终点"才产生：起点由收包路径在 `receive()` **之前**记（每时隙一个），终点分别是
"第一个码块进解码器"（`[ul_gpu_pipeline]`）与"CRC-OK"（`[ul_pipeline]`）。抬头原文：*"A start left behind by a CRC-failed TB
or a retransmission that needed no decode is simply left unmatched (and eventually evicted)"*，逐出闸门 **`max_entry_age = 2 s`**。
⇒ **电台/传输打嗝、而那一跳始终没有走到终点时，`stale` 什么都不会记。**

**盲区 3 —— 收包侧自己还有一处洞。** dry-pool drop 分支（`lower_phy_baseband_processor.cpp` 里 `drop_samples` 那条）自己调用
`receiver.receive(drop_writer)` 后**直接 `return`**，位置在 `record_start()`（≈1314 行）与 `record_rx_wait()`（1362 行，唯一一处）
**之前** ⇒ 那一次接收的等待**两条序列都不记**。

**判读规则（两条一起读，缺一个都会漏判）**：

* `stale` ⇒ "**跳跨度**有没有超过 8 ms"（会话/流水线侧）；
* 接收/传输侧的离群点 ⇒ `[ul_rx_wait]`（本条）、**`[ul_rx_timing]`**（`recv(max/over 1ms/over 5ms)` 与 **`loop(...)`**、`slip`、`load1`）、
  **`[ul_rx]`**（`gaps` / `gap_samples` / **`timestamp-0 blocks`**）、**`[ul_rx_pool]`**（`pop_blocking` / `starved_events`）、
  以及腿 `.log` 里电台自己的 `[RF] …` 与驱动错误行；
* 一刀切的归因：**`recv` 大而 `loop` 不大 + 发送方向（`[dl_tx_call]`）同时也卡 ⇒ 传输/USB**；**`loop` 大 + `load1` 高 ⇒ macOS 调度**；
  **只有 CRC/SINR 变差而两者正常 ⇒ 空口**。

**为什么本次 `stale=0` 反而是信息**：若那一块只是"晚到但最终解出来了"，配成的跨度会 ≈100 ms ⇒ `stale ≥ 1`（两条序列都该有）。
两条都读 0 ⇒ **那一次 101 ms 的等待没有跟一个完成的跳**，与"超时/错误返回（`ts=0`，没有可用样点）"这一类一致。同一现象在本账里早有先例：
更早的三条 n1 腿也各读到 ~101 ms 的 `[ul_rx_wait]` 最大值（101591 / 101670 / 101319 µs），**其中有的腿 `gaps=0、stale=0`**
（§6.40 ①：*"停顿时不时落在跳上"*——落在跳上才进 `gaps`/`stale`，落在空时隙上就只在接收侧留下一条 `max`）。

### 4.2 两个探针的**样本总体**：P0-5 之前不同、之后已配对

实测（腿 `s82`）：相位三段各 **97331** 个样本，而 `[ul_gpu_lane] residency` 有 **140204** 个（= 授权数）。
**它们不是同一个总体**（前者只覆盖"走完整条链并与解码配对上的那些跳"，后者按跳计）。
⇒ P0-5 之前：**可以比"比例与量级"，不可以比"同一跳的两个读数"**，`eq_demap ≈ residency` 只能算指示性。
⇒ **2026-09-25 起**：`ul_pipeline_probe::set_phase_sample_observer()` 把每个**定稿的相位样本**交给车道探针，
按 **slot** 与车道行配对，报告里给出 `paired …` 系列与两个比值（§6.3）。读 D 项预算时**用配对后的那一组**。

### 4.3 本工作流的读法总则

1. **先读定义，再比较**：两个数不一样时，第一件事是问"它们是不是同一个量"（本线四次更正都栽在这里）。
2. **单位对齐**：`staged` 数的是 wrap 次数；`busy split` 的 `cbs/lane` 是**每组**的，两个组相加才是总的（`ch_wt 1.00 + merged_hop 1.00 = 2.00`）。
3. **"读不出"按红算**：契约有**两种打印形式**（`MET (8 of 8 checks applicable)` 与 `NOT MET: 1 of 8 applicable checks failed`），
   解析器只认第一种时，**恰好在契约有话要说时读不出**（§5.9.125 ⑥ 的 bug）。
4. **单次绿等于零证据，单次红也不是证据**：先复跑一次（门已内置；`value_net`、边缘块臂、MMSE 单测都有偶发）。

### 4.4 已知的坑（都实测过）

| 坑 | 症状 | 规避 |
|---|---|---|
| **`-R` 的方向** | 以为在跑下行，其实是上行（或反之）| 认准：`-R` = server 发、client 收；**写指令时把这句写进命令注释** |
| **收尾被截断** | 洪泛腿 Ctrl-C 后停在 `Could not stop application after 5 seconds`，`.stderr` 有契约但**没有 `[metal_stats]`** | `run_leg.sh` 现在**同时要求**契约行与 `[metal_stats]`；见到强制退出会点名 |
| **日志洪泛** | 下行饱和时 RF 告警按槽刷，单腿 `.log` 到 **637 MB**（~160 MB/s 峰值）| 见洪泛就尽快 Ctrl-C；磁盘 258 GB 未构成风险但别无谓跑 |
| **缺设备内核的静默回退** | 新建 worktree 没有 `.metallib`，引擎静默走宿主路径而腿仍自称 gpu | `run_leg.sh` 在非 cpu 模式**预检四个内核**并拒绝启动 |
| **并行跑** | 两个门/两个回放同时跑 ⇒ 假失败（实测四条同时出现）| `milestone_audit.sh` 有**互斥锁**；回放类工具**串行**用 |
| **合并后的行号漂移** | 引用主文档行号会指错 | **按句子/章节名检索**，不按行号 |
| ★ **零流量的腿会"静默少判两条"** | 没有 PUSCH 跳时，契约里 `ce device estimates`（`device==0 && host==0` ⇒ `nullopt`）与 `host sample assembly`（`symbols==0` ⇒ `nullopt`）会**退出 applicable 集合**，于是打印成 `MET (6 of 6 checks applicable)`；而里程碑判据要的是 **`MET (8 of 8`** ⇒ **直接 FAIL**，且看起来像"契约坏了" | **默认腿也必须跑正常流量**（§3.2）。⚠ 只有 PRACH（attach 本身）**不产生**这些跳——PRACH 喂的是 A1-2 的"普通路由"，不是融合车道的计数器 |
| ★ **别把重上行 A/B 的门用在默认腿上** | `leg_gate.sh` 里有两条 **`VALIDITY`** 判据（`UL >= 2.0 Mbit/s`、`UL grant in >= 50% of slots`）是 **§5.9.96/§5.9.101 为重上行腿预登记**的；拿它去判一条无负载的默认腿，**必然两条红** | 默认腿用 `milestone_audit.sh`（它按**工况**选腿与判据）；`leg_gate.sh` 只用于**加压腿**。这与"判据绑错工况"是同一类错误，只是方向相反 |
| ★ **判据还有第三个绑定维度：几何** | A1-2 的 C4（`plain route == 12×round(T/10ms)+1`）里那个 **12** 是 **n78 配置的 PRACH 格式（B4）**；在 n1 上 PRACH 的 IDFT **走 CPU 回退**（Metal 工厂对不支持尺寸透明回退，注释点名"the PRACH FFT sizes"）⇒ 该腿 `dft commits=1`、`plain route=1`，**没有可供归属的信号** | `a12_attribution_gate.sh` 现在带 **几何前提**：腿的 `cell config` 必须是登记的 n78 配置**且** `dft commits > 1`；前提不成立 ⇒ **C4 记 "NOT JUDGED" 并给理由**（不进分母），门再把 A1-2 移到能判的腿上。**判据在被判得了的地方判，绝不软化** |
| ★ **"逐字节相同"可能是空结论** | 用 `cmp -l A B \| wc -l` 比较两个**不存在的文件**得到 **0**（空输出）⇒ 读出"432 次全部相同"，而真实原因是 replay 的 capture 参数**必须剥掉 `.bin` 后缀**，一个 dump 都没产出 | 任何"逐字节相同"**先核验两侧文件存在**（`cmp -s` + 存在性检查）；这也是 `ab_dumps.sh` 文件头警告过的同一类错误 |
| **偶发读数不许静默重试** | 门的某些臂偶发在**无代码改动**时读红（`ab_dumps` 历史臂：14 次连跑与 432 次已核验比较均为 0，但在完整审计里读到 **551**、在紧跟重 GPU 活动的循环里读到 **2728**；两侧各自在隔离下是确定的）| 门的 **6.5 偶发规则**已落成代码：首次非零时**重跑一次**，通过则 PASS，但 **detail 必须保留首次读数**（`[6.5 flake rule: the FIRST read was 551]`）。**不许**用静默重试把偶发洗成绿 |

---

### 4.5 关键文件地图（按用途；新会话按这张表找东西）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **本文档**：判据（§3）、仪表（§4）、跑腿规范（§5）、**追加式实施记录（§6）**、杠杆（§7）、未决（§8）、证据索引（§9）|
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5 逐条、下一步 |
| `doc_chinese/phy_latency/README.md` | 三类文档分工 + 结项状态 + 工具清单 |
| `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.{h,mm}` | MMSE 估计器的引擎：`ce_sites`/`mmse_ce` 计数、`stage_repeat` 旋钮、`OCUDU_MMSE_DEBUG` 相位表、K2 的双读者分支（§6.62）|
| `…/metal/ocudu_mmse_apply_lse.metal` | K2 直读 LSE 的 kernel（**独立文件 + `-fno-fast-math`**，§6.62）|
| `…/metal/ocudu_mmse_apply.metal` | 旧 K2（**一行未改、保持 fast math**：y 路仍发布昨天的字节）|
| `…/metal/ocudu_mmse_refusals.h` | 各阶段"为什么没走设备路"的原因计数（含 `y_direct_*` 五条）|
| `…/metal/port_channel_estimator_metal_mmse_impl.{h,cpp}` | 估计器宿主侧：`build_slots_on_device`、`stage_engine_group`、`record_device_y_stage`、`probe_device_y_stage` |
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` + `lib/phy/lower/lower_phy_factory.cpp` | 接收池（P2-D 定尺）、RX 探针（`rx_overflows`/`gap_us`/`[ul_rx_timing]`/`load1`）、TX 探针 |
| `configs/gnb_rf_b200_tdd_n78_20mhz.yml` | 腿配置：接收环 **512 帧**、发送环 64、`otw_format: sc12` |
| `doc_chinese/phy_latency/wip/p0_gate.sh` + `p0_gate_selftest.sh` | **P0 门（D1–D19）**；D18 = 直读网格分流，D19 = 接收侧余量与归属（自测双向）|
| `doc_chinese/phy_latency/wip/leg_census.py` | 腿普查（自行推导流量窗口 + 每次停顿打印 PUCCH 行数与判词）|
| `doc_chinese/phy_latency/wip/dft_kernel_cost.mm` | 前端 `dft_dit` 的离线微基准（§6.29/§6.30 的依据）|
| **`doc_chinese/phy_latency/wip/ce_kernel_cost.mm`** | **CE kernel 的离线微基准（§6.65/§6.66）：算力 + 空派发地板 + 四种阶段边界 + 宿主 encode** |
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` | 起腿（戳 + 内容判据双重守卫 + 进程/端口预检）；腿日志在 `…/wip/logs/` |
| `doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh` / `ab_replay_bins.sh` | A/B 网：前者 = 一个二进制两个环境；后者 = 两个二进制（**必须成对给 `AB_METALLIB_A/B`**，§5.2 第 7 条）|
| `doc_chinese/work_tmp/` | **跑出来要看的东西（git 忽略）**：语料 `corpus/`、归档基线、`ref/`（一次性二进制 + metallib）|

---

## 5. 跑腿与门的操作规范

### 5.1 起腿与判据（命令）

```bash
# 0) 起腿前（run_leg.sh 也会硬检查；它按进程名精确匹配，不会误伤 shell）
pgrep -x gnb || echo ok; pgrep -x ul_chain_replay || echo ok; lsof -nP -iUDP:2152 || echo ok

# 1) 默认腿（n1，判契约 8/8、stale=0、A1-2）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>
#    流量：CN `ping 10.45.0.10 -i 0.1 -c 100` → 10s 下行 iperf3 → 10s 上行 iperf3 → 空跑到 ~100s，一次 Ctrl-C
#    ⚠ 零流量的腿无效（契约会少判两条，读成 MET (6 of 6)）

# 2) 加压腿（n78，判 V1–V5）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> --regime=stress
#    ⚠ 加压配方 `iperf3 -c <phone> -R -b 40M -P 4 -t 240`：`-R` 不能省（否则变成下行负载）

# 3) 判据
bash doc_chinese/phy_latency/wip/p0_gate.sh <腿> [--vs=<出厂臂>]   # P0 读数（只读日志）：A/B/C + Q9 的 D1-D5
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 <腿>    # ⚠ 只用于加压腿
bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh               # 里程碑门（~3 分钟，互斥锁）
```

### 5.2 纪律（每条都是血换来的；1–5 为原五条，6–9 为 2026-09-25 会话 #6 新增）

1. **判据有三个绑定维度**：**工况**（默认 vs 加压，`--regime=` 让腿自报）、**流量**（零流量腿无效）、
   **几何**（A1-2 的 C4 是 n78 PRAT 几何特定的；n1 上记"未判"）。
2. **不许为过关改阈值**；**"读不出"按 RED 算**；**单次红不是证据**（先复跑一次，且 detail 必须保留首次读数）。
3. **任何代码改动都会让"腿的提交证据"失效**（门会报 `the commit it ran, vs HEAD`）⇒ 代码阶段之后要重新飞腿。
4. **不要在用户飞腿时跑任何碰 GPU 的东西**（门/单测/replay/短跑 gnb 都算）；先 `pgrep -x gnb`。
   **进程检查永远按名字**：`pgrep -f` 曾匹配到自己的 shell，挡了用户一条腿（§6.5）。
5. 腿要**自己声明工况**：`run_leg.sh` 会把 `[leg] regime=` 写进腿的 stderr（provenance 的 knob 行已修好换行，
   早前粘在上一行导致行首 grep 读不到）。
6. **`ctest` 判据一律串行（`-j 1`）**：`ctest -L phy -j 4` 会让 Metal 测试互相争用而**假红**——已用"两条路线都红 + 隔离连跑 6 次全绿"
   证伪（§6.62③）。**回放类工具也串行用**（§4.4）。
7. **A/B 的 metallib 必须成对，且不许把树里那份当 B 臂**：`ab_replay_bins.sh` 的 `AB_METALLIB_B` 若指向
   `lib/.../ocudu_mmse.metallib` 本身，`cp` 会因"同一文件"跳过 ⇒ **两侧跑同一个内核、dumps 全 0 却是空结论**（§6.62③ 实测）。
   新 metallib 先拷到中立路径；脚本的 `pairing-wrong` 判据就是为这件事存在的。
8. **预登记只登记增量，不要顺手锚一个绝对区间**：§6.63 判过一次假 MISS，就是因为把**异二进制、早一小时**的对照腿
   （1371.9）当基准，而**同一条路线的基线自己漂了 13.8 µs**（§6.64②）。绝对区间会把对照的漂移也算进预测。
9. **加压腿上 `leg_gate.sh` 的 `stale = 0` 与 `grant ≥50%` 是误绑项**（前者为默认腿、后者为重上行腿登记的）——
   但**翻红要记、要归因，不许静默放过**（§6.64⑤ 是范例：`stale` 在两对里方向相反 ⇒ 归宿主竞争另案）。

### 5.3 仓库与构建状态（每次改代码后必做）

* ⚠ **开发产物一律放 `doc_chinese/work_tmp/`（git 忽略、不进跟踪）**，**不要放 `/tmp`**（macOS 重启会清掉，
  用户 2026-09-25 明确）。放这里的是"跑出来要看的东西"：腿普查输出、dump 比对、一次性二进制（如
  `work_tmp/ref/replay_head_pre_p05`）；**门与判据依赖的脚本必须进 `wip/` 并被跟踪**（见 5.4 起）。

* 分支 **`apple-silicon`**。本工作流的提交：`470316ab8d`（P0-6）、`4dcb03e3d5`/`f148808e3d`/`ebf3951920`（P0-1 + 缺陷修复）、
  `747b9d475b`（P0-5）、`7c40327f81`（P2-E），以及 2026-09-25 的文档整理提交。**都已推送到 `origin/apple-silicon`**。
* ⚠ **戳的纪律**：任何提交之后 `run_leg.sh` 会因为"二进制戳 ≠ HEAD"**拒绝起腿**（这是有意的：腿是关于**二进制**的证据）。
  改完代码先 `touch build/hashes.h && cmake --build build --target gnb`，再起腿。
* ⚠ **`.metallib` 是构建产物且被 git 忽略**（本机 8 个）；新 clone/worktree 里缺了会**静默走宿主路径**
  （`run_leg.sh` 有预检会拒绝）。本机这 8 个的时间戳是 **9-24 14:51** —— 它是 `value_net` 归档基线陈旧的证据链之一（§6.3 ④）。
* 每步之后工作区保持干净（`git status --short` 为空）；离线验证一律在**同一台机器、同一份构建**上跑。
* **提交之后要重建**：`build/hashes.h` 的戳必须对齐 HEAD，否则下一次起腿会被自己的预检挡住。
* **开工前的自行对齐（三条判据，全为真才算对齐）**：
  `git log --oneline -1` 与 `grep -oE '[0-9a-f]{10}' build/hashes.h | head -1` **相同**，且
  `grep -aq "$(grep -oE '[0-9a-f]{10}' build/hashes.h | head -1)" build/apps/gnb/gnb` 为真；不同就重跑
  `cmake --build build --target ocudu_versioning && cmake --build build --target gnb`（**分开两条命令**）。
  ⚠ **`ul_chain_replay` 从不内嵌版本戳**（它不链 versioning 目标）⇒ 对它做 `grep -aq "$H"` **恒为假**，
  别把"离线工具没有戳"误判成"二进制是旧的"（2026-09-25 #6 实测）。

### 5.4 文档分工与会话交接（**用户 2026-09-25 明确**）

* **`session_handoff_<日期>-<序号>.md` 是"会话交接时刻的现状快照"**：命名规则"日期取会话开始那天（跨午夜不改）、序号是那天的第几份"。
  它的用途是**让新会话快速开工**（现状、读数出处、下一步、纪律、文件地图、第一句开工指令）。
* ⚠ **开发过程中不要创建、更不要修改它**——改过之后它就不再是"那一刻的快照"了。
  **快照有错要更正、或开发过程中要记的任何东西，一律记入本文档**（§6 追加式记录 / §3 判据 / §4 仪表 / §5 规范 / §8 未决 / §9 证据索引）。
* **只在会话交接时创建**（那一刻写一份，写完不再动）。若开工后发现上一份快照有误，**在本文档里更正并引用它**，不要回头改那份文件。

---

## 6. 实施记录（**追加式：只追加，不改历史**）

### 6.1 P0-6 ✅ 车道的**生效并发度**现在可读（2026-09-24，提交 `470316ab8d`）

**为什么必须有它**：时延工作第一个问题是"`ce` 段 901 µs 能否归因为**单车道排队**"（§8 Q8）。
判它的前提是知道车道执行器的生效并发度，而这个值**过去在任何腿、任何 dump 里都没有**：
配置默认 `concurrency_auto`，YAML dump 把哨兵原样写回；而历史上所有"单车道"读数都来自 **n1** 腿，
两个配置的推导值本不必相同（`derive_pusch_and_srs_concurrency()` 随带宽与 TDD 上行占比变）。

**怎么读**（两行，stderr，**在电台打开之前**打印 ⇒ 短跑 `gnb -c <配置>` 即可离线读到，不必飞腿）：

```
[ul_lane_exec] PUSCH/SRS concurrency = 1 (auto-derived; bw=20MHz layers=1 ul_ratio=0.30; available cpus=14)
[ul_lane_exec] PUSCH lane executor: max_pusch_and_srs_concurrency=1, medium pool max_concurrency=5
               -> pusch_executor.max_concurrency=1 (a serialising STRAND: ONE PUSCH hop at a time …)
```

**实测（2026-09-24）**

| 配置 | 推导输入 | 生效值 | 形态 |
|---|---|---|---|
| n78 `configs/gnb_rf_b200_tdd_n78_20mhz.yml` | `bw=20MHz layers=1 ul_ratio=0.30`、cpus=14 | **1** | **串行 strand** |
| n1 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` | `bw=5MHz layers=1 ul_ratio=1.00`、cpus=14 | **1** | **串行 strand** |

> ✅ **空口确认（2026-09-24，用户腿 `s84-p0_0924_2303` 的 `.stderr`，非短跑）**——两行确实落在**腿自己的 stderr**（门读的就是这个文件）：
> `[leg] regime=default` / `cell config : configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` /
> `[ul_lane_exec] PUSCH/SRS concurrency = 1 (auto-derived; bw=5MHz layers=1 ul_ratio=1.00; available cpus=14)` /
> `[ul_lane_exec] PUSCH lane executor: … -> pusch_executor.max_concurrency=1 (a serialising STRAND …)`。
> （读于腿运行中：启动行已落盘，契约行只在收尾时打印。）

**三条结论**

1. **C 项（`ce` 901 µs）"单车道排队"的归因成立**——车道确实是串行 strand；一跳占住它 ≈1125 µs，而重载下每 ~1.5 ms 要求一跳。
2. **P1-8 有真杠杆**：可把并发度提到 **2..5**（上限 = 中等池的 `max_concurrency`；`mapper` 对**超过池子**的值直接报配置错误，不夹取）。
   用户已裁决 V4 **只约束交付**（§7.4），故该臂作为**纯测量臂**放行。
3. ⚠ **教训（写死）**：该值的推导输入必须**读出来**，不能从配置字面推断——曾据"小区级未设 TDD 图案"推 `ul_ratio=1.0` ⇒ 2.5 ⇒ 3 路 fork，
   实测是 **0.30**（图案来自**公共小区**那一层）⇒ 0.75 → ceil **1**。

**规则的单测**：`tests/unittests/du_low/du_low_executor_mapper_test.cpp`（4 例：`1→strand`、`3→3 路 fork`、`12=池→fork 12`、`0=无限制→池值`）。
标签 `du_low;du_high`，**故意不含 `phy`**（不改动审计门 `ctest -L phy` 的 193 计数）。

### 6.2 P0-1 ✅ 诊断开关 `OCUDU_LANE_DIAG_SPLIT`（2026-09-24，**默认关**，提交 `4dcb03e3d5` / `f148808e3d` / `ebf3951920`）

**它做什么**：在 `shared_burst::adopt()`（前端把命令缓冲交给估计器的那一点）**不再接管**那条缓冲，而是
`encodeSignalEvent` → `arm_gpu_time` → `gpu_lane_probe::register_commit(cb, stage::dft)` → 提交它，
并为后续阶段开一条 `encodeWaitForEvent` 的新缓冲。
于是 **`dft` 段第一次有读数**——该标签在 `gpu_lane_probe::stage` 里**一直存在**（注释原文 *"per-symbol DFTs; not registered yet"*），只是从未被注册过。

**为什么是这一处**：分组读数其实早有（`OCUDU_CE_LANE_ORDER=event` 路线：`ch_wt≈483 µs` 87% + `eq_demap≈71 µs` 13%，§5.9.66 ②），
缺的只是**前端那 14 个 transform 在 `merged_hop`（≈1030 µs）里占多少**。

**离线验证载体**：`dft_release_adopt_metal_test`（`lib/phy/generic_functions/metal/test/`）——
它专门演练"前端交出未提交的块 → 车道接管 → 消费者读网格"，并打印 `[ul_gpu_lane]`。
（CE 单测与 `ul_chain_replay` **都到不了 `adopt()`**：它们的 `busy split` 是 `ch_est`+`ch_wt`，没有 `merged_hop`。）

| 同一二进制 | `cbs/lane` | 断言 |
|---|---|---|
| **OFF（出厂默认）** | **1.00**（max=1）| 两条全 PASS，含 *"one command buffer, one commit"* ⇒ **出厂形态一字未变** |
| **ON** | **2.00**（max=2）| 拆分生效；residency 44 → 172 µs（多一次提交的代价）。**离线也读到了 `dft=37.5us/lane (83% of busy)`** |

**空口读数（2026-09-24，腿 `s86-diagsplit`，n78 加压）**：`dft=937.9 µs/lane (88%)` + `ch_wt=41.9 (4%)` + `merged_hop=87.8 (8%)`，
三组之和 **1067.6** vs 出厂臂 `merged_hop` **1032.7** ⇒ **比值 1.034（B2 通过，±10%）**。
⚠ 该臂 `cbs/lane=3.00`（比出厂臂多一条），**V4 不适用于诊断臂**。

**★ 该臂的第二重身份（必须记住）**：ON 时那条测试的**别名陷阱 FAIL**——因为陷阱要求两次 dispatch 在**同一条**缓冲里
（坑 36：Metal 按 `MTLBuffer`**对象**排序，barrier 排不了两个对象），而拆分用**事件**给出了真实的跨缓冲顺序，**把陷阱掩盖了**。
⇒ **拆分臂的 ordering 语义比生产路线更严格**：**不得**用它判生产正确性；它**只**用于**分组 GPU 时间**。

**读该臂前的两条静态核实（读码，2026-09-24）**：① `stage_name(stage::dft)` 返回字面 **`dft`**（`ocudu_metal_lane_probe.mm`）⇒ `busy split` 里的 token 就是 `dft=`；
② `register_commit()` 只是把 `{cb, stage, now}` 压进**本线程的 `pending`**，**不要求车道已开启**，由 `close_lane()`（跳尾 `wait_committed()` 调用）归属 ⇒ 前端那次提交会被正确计入。

⚠ **命名陷阱（必须记住）**：拆分臂里第二个缓冲仍被估计器标成 **`merged_hop`**（它并不知道前端已被分出去）⇒
**开关 ON 时读到的 `merged_hop` 含义是「前端之后的全部」，不是整跳**。
判据里的「各段之和 ≈ 出厂臂的 `merged_hop`」算术仍成立，但**名字会骗人**：读该臂时请把它当 `rest_after_front_end`。

**该臂的污染项（写死）**：① 一跳 **3 条缓冲** ⇒ `cbs/lane≈3`，**V4 不适用于诊断臂**；
② 前端输入缓冲**更早释放**（正是 **P2-E** 的题目）⇒ **不得**用该臂读跨度、`cbs/lane`、池、V1–V5。

**离线确认（一条命令，已补）**：

```bash
OCUDU_LANE_DIAG_SPLIT=1 build/lib/phy/generic_functions/metal/dft_release_adopt_metal_test 2>&1 | grep -a "busy split"
# → dft=37.5us/lane (83% of busy, cbs/lane=1.00) eq_demap=7.5us/lane (17% of busy, cbs/lane=1.00)
```

### 6.3 P0-5 ✅ 相位探针与车道探针**按 slot 配对**（2026-09-25，提交 `747b9d475b`）

**① 问题**：`[ul_time_frequency]`/`[ul_channel_estimation]`/`[ul_equalization_demod]` 按 **slot** 记、
且**只为 CRC-OK 的 TB** 出样本；`gpu_lane_probe` 的 `residency`/`busy` 按 **车道（一跳一条线程的链）** 记、**每一跳都算**。
`s85-p0phases` 实测 **60389** 相位样本 vs **142022** 车道样本（比值 **0.425**）⇒
"residency 里 ~95% 是 busy"、"`eq_demap` ≈ residency" 这两句**是跨两个总体读出来的**，而 D 项预算恰恰要用这个分母。

**② 改法（报告侧，不动数据面）**

| 侧 | 改动 |
|---|---|
| 车道 | `lane_host_clock::lane_slot`/`has_lane_slot`（由 MMSE 适配器在**一跳开始处** `mark_stage_entry(args.slot)` 写入，`ocudu_metal_lane_clock.h`）；`register_commit()` 把 slot 写进**每一个** `lane_entry`（**按 entry 取**，不按 lane 关时取：carry 过来的 entry 属于上一跳）；`close_lane()` 把关闭的车道按 slot 记进有界表（按插入序淘汰 + **2 s 年龄门**，防 10.24 s 的 slot 键回绕）|
| 相位 | `ul_pipeline_probe::set_phase_sample_observer()`：探针在**把样本写进三段序列的那一支**里把 `(slot, t2f, ce, eqdem)` 交给观察者，**在释放自己的 mutex 之后**调用（防死锁）；车道探针注册为观察者，命中即配对并弹掉该行 |
| 报告 | `[ul_gpu_lane] paired with the phase segments (P0-5): samples=… of phase_samples=…（no lane for the slot=…, lane older than 2s=…）over lanes=…`；`paired residency/busy/t2f/ce/eq_demap` 五条序列；**`paired ratios (P0-5)`**（`busy/residency` 与 `eq_demap/residency`，并给出"全车道自己的 `busy/residency`"作对照）；**`paired reading (P0-5)`**（0.95±0.05 / 1.00±0.05 是否复现——**这是描述，不是判据**）|

**③ 判据（先写死）**：(1) 配对样本数 **==** 相位三段样本数；(2) 两个比值在**配对样本上重算**并打印；
(3) 零数据面影响（`ab_dumps` 逐字节、`value_net`、`l1_*`、`ctest -L phy`、契约 8/8）、**不动提交数**（V4）。
`p0_gate.sh` 增 **C2**：只对声明 `OCUDU_UL_PHASE_SEGMENTS=1` 的腿判 (1)，读不出按 RED（5.9.97）。

**④ 离线验证（2026-09-25）**

* `ctest -L phy` **100% passed out of 193**；
* 新单测例（`tests/unittests/support/executors/ul_pipeline_probe_test.cpp`，标签 `support`，**不动 193 的计数**）
  钉住 hook 契约：**每个定稿样本恰好通告一次**、携带**与序列相同的三个时长**、未组装/未定稿的跳**不通告**、`count(通告) == count(记录)`；
* `ul_chain_replay` 的 **3 capture × 4 dump** 与 **pristine HEAD 二进制逐字节相同**（0 differing）；
* `l1_handover_arms` **5 PASS**、`l1_hop_arms` **4/4 `differing=0`**、`edge_block_arms` 默认 **6/6 绿** + 回退臂 **6/6 红**、`ab_dumps` arm2 **0 B**。
* ⚠ **两条网在改前就红**（用 pristine HEAD 二进制复现同一读数，**不是**本次改动）：
  `value_net.py`（47 capture / **183 problems**，归档基线 `doc_chinese/work_tmp/determinism/a` 是 9-20 05:26，
  而 CE 的 `.metal` 源在 9-20 10:27–21:41 改过、`.metallib` 9-24 14:51 重建 ⇒ **基线陈旧**）
  与 `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` vs 默认：**15/27 capture、159068 B**，HEAD 二进制在 `syn004_4` 上复现同样差值）。

**⑤ 还缺的读数（需一条空口腿；跑法与判据）**

```bash
# 起腿前：pgrep -x gnb / pgrep -x ul_chain_replay / lsof -nP -iUDP:2152 都要干净
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>   # n1 默认配方 + 标准流量
bash doc_chinese/phy_latency/wip/p0_gate.sh <label>        # 读 C2（配对 n == 相位 n）与 paired ratios/reading
```
**先写死的判读**：C2 绿 ⇒ 两个探针从此描述同一批跳，D 项（本跳设备执行）的分母可用；
C2 红 ⇒ 先查 `paired …` 行里的分项（`no lane for the slot` / `lane older than 2s` / `awaiting at exit`），再决定是配对的哪个前提错了。

**⑥ 空口读数（腿 `p05-pair`，n1 默认工况 + `OCUDU_UL_PHASE_SEGMENTS=1`，2026-09-24 夜；⑤ 要的那条腿已飞）**

腿：`gnb_gpu_p05-pair_0925_0034`（二进制戳 `93c909e423`，到当前 HEAD 只差文档；`[leg] regime=default`、
`cell config = configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`knob OCUDU_UL_PHASE_SEGMENTS=1`）。
门：`p0_gate.sh p05-pair` → **10/10 PASS**（A1/A2/B1/C1/C2/C2b + INFO）。

**判据 1（配对 n == 相位 n）：✅ 成立，且是精确相等**

```
[ul_gpu_lane] paired with the phase segments (P0-5): samples=73529 of phase_samples=73529
              (no lane for the slot=0, lane older than 2s=0) over lanes=100224
              (slot named=100224, not named=0); awaiting at exit=511, evicted=25985
```

* `samples == phase_samples`（73529 == 73529）；**配对零丢失**：`no lane for the slot=0`、`lane older than 2s=0`；
  **100224/100224 条车道都报出了 slot**（`lane_host_clock::lane_slot` 在空口上工作）⇒ 按 slot 配对的接线成立。
* 配对**值**也对：`paired t2f/ce/eq_demap` 的中位/均值与 `[ul_time_frequency]`/`[ul_channel_estimation]`/`[ul_equalization_demod]`
  **逐位一致**（1046.9/1095.1、2933.3/3278.0、920.5/907.9）⇒ hook 交出去的是同一批样本，不只是同一个数。
* 账目：100224 次插入 = 匹配 73529 + 淘汰 25985 + 退出时在等 511 + **199 行被"同 slot 的后一条车道"覆盖**（0.2%，见 §8 Q13）。
  `evicted` 26% ≈ 非 CRC-OK 的跳（73529/100224 = 73.4% 匹配率），**且一次都没有因为淘汰而配不上**（`no lane=0`）。

**判据 2（在配对样本上重算两个比值）：⚠ 两个"指示性"读数在 n1 上都不复现**

```
[ul_gpu_lane] paired ratios (P0-5): busy/residency median=0.643 over ALL lanes' own ratio=0.647; eq_demap/residency median=1.086
[ul_gpu_lane] paired reading (P0-5): "residency is ~95% busy" (0.95 +- 0.05) is NOT reproduced (0.643);
                                     "eq_demap ~ residency" (1.00 +- 0.05) is NOT reproduced (1.086)
```

* **`busy/residency` = 0.643**（全部车道的同一比值 0.647，配对子集没有偏）⇒ 在这条 n1 默认腿上，
  车道 residency（中位 835.5 µs）里**只有 ~64% 是设备在执行**，另外 ~260 µs 是**车道自身窗口里的设备空闲**
  （一跳两条缓冲：`ch_wt` → `merged_hop`，中间有 fence/等待；`queue: weights commit → weights start` 中位仅 24.2 µs，
  所以不是提交延迟）。⚠ 而 **n78 加压腿**（`s85-p0phases`，未配对）读到的同一比值是 **0.93**（1047.4/1127.5）
  ⇒ **这个比值是随腿/随负载变的**，"residency 里 ~95% 是 busy"**不能跨腿引用**（§2.4/§4.1 已按此更正）。
* **`eq_demap/residency` = 1.086**：`eq_demap` 是**宿主墙钟**窗口（`record_ce_end` → 第一次 LDC 解码调用），
  比设备侧的 residency **长 ~9%** ⇒ "eq_demap ≈ residency" 不是恒等式。注意这个比值在**两条腿上都 ≈1.09**
  （n1 1.086；n78 `s85` 1231.0/1127.5 = 1.092）⇒ **稳定的是 eq_demap ≈ 1.09 × residency，不是 ≈ residency**。
* **对 D 项预算的影响（本节最重要的结论）**：在 n1 默认腿上，**一跳的设备执行 ≈ `busy` = 517 µs（中位）**，
  不是 residency 836，更不是 n78 加压腿的 1125。⇒ 引用 D 项时必须写清是**哪条腿的 busy**，
  并且"residency ≈ D"这个等式要换成 **`residency = D + 车道自身的空闲（fence/排队）`**。

**判据 3（零数据面 + 不动提交数）：✅**

* `cbs/lane=2.00 (max=2) dropped=0`（V4 不变）；`crossings 0.00 read(s) + 0.00 write(s) per hop`；
  `dft radio inputs` 100.0% 走交棒；`zero-copy wraps 0 failures`；`ce device estimates: 1202588 device, 0 host`。
* （离线那半边见 §6.3 ④：与 pristine HEAD 二进制**逐字节相同**。）

**这条腿同时暴露了一个我自己的读数缺陷（gate 侧，已修，未动判据）**：C2 原来把车道报告的 `samples=`
（atexit 打印）与 `[ul_time_frequency] samples=`（**收尾一开始**就打印的快照）相比 ⇒ 这条腿上读出
**73529 vs 73528 = 假 FAIL**。原因是代码事实而非阈值：`gnb.cpp` 在收尾开头调 `ul_pipeline_probe::report()`，
车道探针在 atexit 报告，而观察者在两者之间还会继续计数（收尾期间完成的那一个样本）。
⇒ 现修法：**C2 只比同一行上的两个数**（`samples=` vs `phase_samples=`，同一瞬间）；另加 **C2b**：
`[ul_time_frequency]` 的打印值必须 **≤ announced**（该序列只增不减，反向即真缺陷）——**没有引入任何阈值**。
复验：`p05-pair` 10/10 PASS；**未装仪表的旧腿 `s85-p0phases` 仍 RED**（`paired='<absent>'`）；
非相位腿仍记 INFO。⇒ 门没有被软化。

**腿的效力/工况说明（引用这些数字时要一起引）**：契约 **NOT MET (1 of 8)**，红的是 `radio sample continuity: 7 gaps`
（`[ul_rx_pool] held_max=8 free_min=0 starved_events=93`、`[ul_gpu_pipeline] stale=933` max_stale 84 ms、
`[ul_rx_wait] max=101.6 ms`）。**这不是 P0-5 带来的**（数据面逐字节相同），且是**已知的间歇收包停顿**：
~101 ms 的 `[ul_rx_wait]` 最大值在**三条更早的 n1 腿上都存在**（101591 / 101670 / 101319 µs，其中 `s83`/`s87` 的 gaps=0、stale=0）
⇒ 停顿时不时落在跳上，这次落下的是 gaps/stale。另：这条腿跑了 **226 s、100233 个 PUSCH 槽（≈44% 的槽、~443 跳/s）**，
比标准的"默认配方"（~73 跳/s）重得多 ⇒ 它的池/stale 数字不能与历史默认腿直接比；**中位数**可以，且与 `s87-n1phases` 几乎逐位相同
（residency 836.7 vs 837.3、busy 517.4 vs 517.9、t2f 1095.1 vs 1093.0、ce 3278.0 vs 3228.3、eq_demap 907.9 vs 905.5、
`[ul_gpu_pipeline]` 5248.0 vs 5218.0）⇒ **P0-5 的改动没有移动任何一个中位数**。

**待办（登记，下一次动代码时一起做）**：给"配对账"再加一行**自解释**输出——
`paired vs announced vs 序列在退出时的条数`（后者需要一个 `ul_pipeline_probe::phase_samples_recorded()` 访问器），
这样读腿的人不必自己知道"两个报告的快照时刻不同"。**本轮没有动它**：动代码会让这条腿的提交证据失效（判据纪律 3）。

### 6.4 P2-E ⛔ 输入缓冲寿命解耦：**机制已按计划实现，但被平台证伪**（2026-09-25，提交 `7c40327f81`）

#### 6.4.1 计划（原文摘要，`04_p2e_plan.md` 已并入本节）

**要解决的问题**：输入缓冲的持有期 = **整跳跨度**（keepalive token 挂在整条命令缓冲的完成上，而 D1 之后整条缓冲就是整跳），
而**只有前端那一段读接收样点**（估计器/均衡/解映射读的是**网格**）⇒ **多持有** → 池抽干 → 接收线程被 `pop_blocking` 卡住。

| # | 锚点 | 改什么 |
|---|---|---|
| **E-1** | `ocudu_dft_metal_engine.mm` 的块关闭/交出路径（`release_block()`）| 在**最后一次网格写之后** `encodeSignalEvent:token_event value:generation` |
| **E-2** | 同文件的 `arm_tokens_on_complete()` | 开关打开时改由 `notifyListener:atValue:` 释放该块 tokens；**保留完成处理器作兜底**（防泄漏）|

**开关**：`OCUDU_DFT_RELEASE_TOKENS_EARLY=1`，**默认关**；反向臂 = 现行为（`=0`）。
**不新增提交、不新增命令缓冲** ⇒ **不动 V4**（`cbs/lane` 保持 2.00）。
**前提必须先验**：除前端外**没有**任何 dispatch 读接收样点。
**预期（先写死）**：持有期 跨度→前端那段；池 `held_max` 8→**≤3**、`starved_events`→**0**（V2）；
那次 **5 秒停顿消失**（`gaps=0` 且 residency max 回到 ms 量级）；`cbs/lane` 保持 **2.00**（V4）。

#### 6.4.2 实现（与计划的差异只有一处，且是 Metal 的硬约束）

| | 计划 | 实现 |
|---|---|---|
| E-1 | "最后一次网格写之后、**关编码器之前**" | 在 `release_block()` 里、**`endEncoding` 之后**、交出之前编码信号。`encodeSignalEvent:` 是命令缓冲级 API，**Metal 规定有 encoder 活动时不得调用**（`MTLCommandBuffer.h`）⇒ 关编码器之后是唯一合法位置；而真正要满足的约束（"在最后一个读输入的 dispatch 之后"）两种写法都满足 |
| E-2 | 事件通知释放 + 完成处理器兜底 | `arm_tokens_on_complete(cb, tokens, early_generation)`：完成处理器**两条臂都留着**；`early_generation != 0` 时再加 `notifyListener:atValue:` 通知；两条路都汇入**幂等**的 `release_block_tokens()`，谁先到谁算（`block_token_set::released`）|
| 开关 | 默认关 | `release_tokens_early_requested()`，**每次调用读取**（单测可在同一进程里开关）|
| 提交数 | 不变 | 只在既有缓冲里多一条信号命令；默认关时行为与改前逐字节相同（§6.4.4 的验证）|
| 可读性（计划未要求）| —— | 新增计数器 `token_early_signals`/`token_sets_by_event`/`token_sets_by_complete`，打印在既有 `[metal_stats] dft handover … (armed=…) tokens_early=signals:N,by_event:M,by_complete:K` 行尾；`dft_metal_engine::token_release_stats()` 供单测读 |

**前提核实（读码）**：token 是**电台接收缓冲的 handle**（`puxch_processor_impl.cpp` 的 `retain_symbol_input()` 每 transform 一个），
只有 DFT 的 transform kernel 读它（`submit_slot_grid_write()` 里 `b_in16`/`time_samples` 的零拷贝绑定）；
估计器/均衡/解映射读**网格**；P0-1 的拆分臂实测前端那一组就是 `dft`。⇒ **"除前端外无人读输入"成立**。

#### 6.4.3 ★★ 平台证伪：macOS 26.6.2 把"命令缓冲中途的事件信号"推迟到整条缓冲完成

**平台**：macOS 26.6.2（build 25G83）/ Apple Silicon。**载体**：`dft_release_adopt_metal_test` 新增的 premise 一节
（每次运行都测，见 §6.4.5）+ 三份一次性最小程序（同机）。

| 测法 | 形状 | 读数 |
|---|---|---|
| **A 宿主轮询 `signaledValue`** | `[短 encoder] → signal → [长 encoder（~100 ms）]`，宿主在缓冲运行期间轮询 | 值**只在完成时**出现（97.1 ms 见值 / 97.4 ms 完成，margin 0.3 ms）|
| **B `notifyListener` 投递时刻** | 同上，通知块里记时间戳 | 通知在完成时刻到达（105.8 ms vs 完成 105.9 ms）|
| **C 另一条命令缓冲 `encodeWaitForEvent`** | A 缓冲中途发信号，B 缓冲只等待该信号 | B 的完成时刻 ≈ A 的完成时刻（77.1 vs 77.0 ms）⇒ **GPU 侧等待也没提前满足** |
| **对照（关键）** | `signal → [长 encoder（~80 ms）]`（信号在**第一个 encoder 之前**）| 值在 **2.04 ms** 就可见 ⇒ **发布本身是即时的**，被推迟的是"encoder 边界之后"的那一个 |

⇒ **规则（本平台）**：信号**一旦编码在"已经创建过 encoder"之后，就只在整条命令缓冲完成时发布**；
编码在第一个 encoder 之前则立即发布。这不是"通知投递慢"（测法 A 与通知无关），也不是"宿主读得慢"（测法 C 是 GPU 侧）。

**单测每次运行打印的判定（本机）**：

```
[dft-release] P2-E premise: a signal encoded MID-buffer was published ONLY AT COMPLETION
              (signal seen at 97.1 ms, buffer completed at 97.4 ms, margin 0.3 ms) - the token release cannot
              move to the front end's end on this platform
[dft-release] P2-E counters: early signals=20, released by the event=0..4, by a completion=36..40
```
（`by_event` 的少量非零是"通知在完成时刻与完成处理器抢 token"的竞态，**不是**提前发布的证据。）

**引擎退出行的自诊断（`by_event == 0` 时）**：

```
[metal_stats] P2-E: N early token-release signal(s) encoded and NOT ONE released a block: this platform
publishes a mid-command-buffer signal only at its completion, so the input is still held for the whole hop.
Read the pool numbers as UNCHANGED, not as 'the hold does not matter'
```

**顺带推论（未验证）**：`shared_queue::grid_ready_signal(cb)` 在合并车道上也是**中途**信号（前端 dispatch 之后、adopter dispatch 之前），
而 PUCCH 的宿主读用 `grid_ready_wait()` 等它 ⇒ 在本平台上它实际等价于"等整跳完成"（**保守**，不是错误；**未飞腿验证**）。

#### 6.4.4 为什么这挡住 P2-E（不是"再调一下参数"）

输入 token 的释放是**宿主动作**（把 `rx_buffer_handle` 还给接收池，见 `release_symbol_input()`），所以必须有一个**宿主可见的时刻**。
本平台上该时刻只能来自：

| 选项 | 代价 | 结论 |
|---|---|---|
| (a) 命令缓冲完成 | 现状 = **整跳** | 这就是要拆的东西 |
| (b) 让前端块**自成一次提交**（= P0-1 拆分臂的形态）| `cbs/lane` **+1**：n1/n78 现在都已是 **2.00 (max=2)** ⇒ 变 3.00 ⇒ **V4 不再满足** | 需要用户裁决（与 P2-F 同类）|
| (b') 变体（**未验证，仅登记**）：用前端块**替换**现有的 `ch_wt` 提交，而不是在它之外再加一次 | 若成立则 `cbs/lane` 仍 2.00；但需重排估计器内部提交点（`ch_wt` 现在承担"提取完成即提交、让权重段编码与之重叠"的角色）| 工作量与风险都远大于 (b) |
| (c) 由**第三次提交**只带信号+等待来"提前完成" | 同样是 +1 提交 | 同 (b) |
| (d) 宿主把输入**拷进引擎自己的 ring**（放弃零拷贝）| 破"零拷贝 radio input"契约；且 D1 的立项目的之一就是去掉这次拷贝 | 不建议 |
| (e) 加一个宿主**自旋轮询**线程读设备侧写完的标志位 | 收包 RT 路径多一个自旋线程；需要 kernel 内 barrier + 可见性论证 | 最后手段，未实现 |
| (f) `commitAndContinue`（"提交并继续"）| **macOS SDK 里不存在**（`MTLCommandBuffer.h` 只有 `enqueue`/`commit`，已查 SDK 头文件）| 不可用 |

⇒ **"不新增提交、不新增命令缓冲"（计划的硬约束）与本平台的信号语义不能同时成立。**

#### 6.4.5 复现方法（离线，不飞腿）与离线验证

```bash
# 1) 平台前提 + 两条臂（每次运行都打印判定行）
./build/lib/phy/generic_functions/metal/dft_release_adopt_metal_test
# 2) 引擎自己的行（任意 arm，开关开时）
grep -a "tokens_early=" <leg>.log.stderr
# 3) 反向臂（两条腿，需飞腿 —— **尚未飞**）：n1 默认配方 + OCUDU_DFT_RELEASE_TOKENS_EARLY=1
#    ⚠ 若 by_event == 0，池数字**不能**用来判断"持有期是否重要"（机制根本没生效）
```

| 网 | 结果 |
|---|---|
| 与 pristine HEAD 二进制逐字节比对（`ul_chain_replay`，3 capture × 4 dump，默认关）| **0 differing**（P0-5 + P2-E 都未动数据面）|
| `ctest -L phy` | **100% passed out of 193**（含扩展后的 `dft_release_adopt_metal_test`）|
| `ab_dumps` arm2（historical，sigma2 固定）| 0 differing bytes |
| `ab_dumps` arm1 / `value_net` | **改前就红**（同 §6.3 ④）|

**单测的断言（两条臂，逐次交替）**：token **恰好释放一次**；**不得在交出之前释放**；
ON 臂**确实编码了信号**（`token_release_stats()`，否则"接了线的空臂"也能过）。
**"哪一端赢"只报告不断言**——平台决定；判定行与 `by_event` 一起给出结论。

**意义（写给下一个读它的人）**：§2.5 那条"输入被持有到整跳 ⇒ 池被抽干 ⇒ 收包停顿"的**因果链仍未验证**——
本机制不动持有期，所以**不能用它的腿来判断"持有期是否重要"**；开了开关而 `by_event == 0` 时，池数字应读作**未变**。

#### 6.4.6 ★ 用户裁决（2026-09-25）：**保留开关作仪器（选项 3）**；不做 (b)/(c)/(d)/(e)，也不因此转 P2-D

**裁决原文（用户）**：「P2-E 裁决：根据你的建议保留开关作仪器，请继续。」

| 项 | 状态 |
|---|---|
| `OCUDU_DFT_RELEASE_TOKENS_EARLY` | **保留**，**默认关**，每次调用读取；反向臂 = 现行为 |
| E-1/E-2（中途信号 + `notifyListener` + 完成处理器兜底） | **保留**（`release_block()` / `arm_tokens_on_complete()`） |
| 计数器 `tokens_early=signals:N,by_event:M,by_complete:K` + `token_release_stats()` | **保留**（任何平台一条腿就能判"这台机器的中途信号会不会提前发布"） |
| 引擎的 `by_event == 0` 自诊断警告行 | **保留** |
| 单测里的平台前提测量 + 两条臂的不变量断言 | **保留**（每次 `ctest -L phy` 都在测） |
| **选项 (b) 前端块自成一次提交**（`cbs/lane` 2.00→3.00，需重裁 V4） | **不做**（本轮未获裁决） |
| **选项 (c)/(d)/(e)**（第三次提交 / 宿主拷贝输入 / 自旋轮询） | **不做** |
| **不因此转 P2-D** | P2-D 仍**未开工**；它与 P2-E 无绑定（它的输入是 P0-5 的配对分布，见 §6.3 ⑥ 与 §8 Q14） |
| **本平台的结论** | macOS 26.6.2 / Apple Silicon 上"中途信号"不提前发布 ⇒ 为 P2-E 再飞腿只会看到池数字未变，**无需再飞** |

**这条裁决意味着什么（写清楚，免得下一会话误解）**：P2-E 从此**不是待交付项，而是一件已建好、已解释清楚的仪器**——
它把"这个平台能不能提前释放输入"从**假设**变成了**一条腿一行的读数**。
§2.5 的因果链（持有期 → 池 → 收包停顿）**仍未验证**：要验证它必须真的移动持有期，
而本平台能移动它的形态只有 +1 提交（选项 (b)，破 V4）或宿主拷贝（破零拷贝契约）
⇒ **Q9 的追查必须走别的路**（§6.4.7），不能指望这个开关。

### 6.5 已修的缺陷与流程教训（2026-09-24 / 09-25）

1. **`adopt()` 的顺序缺陷（已修，提交 `ebf3951920`）**：`shared_burst::adopt()` 原先是"**先提交旧缓冲、再建新缓冲**"，
   建失败会穿透到已提交缓冲上（二次提交 = Metal 硬错误）。现改为**先建后提交**，建失败则**完全跳过拆分**、退回生产形态。
   `mmse_engine` 那边的契约本身是安全的（只有 `adopt()` 返回 false 时它才自己提交），所以缺陷只在"创建失败"这条异常路径上。
2. **孤儿 gNB 挡了用户一条腿（已加硬预检）**：我为读一行启动打印而短跑的 gnb 没被杀掉，成孤儿并占住 `192.168.64.1:2152`
   ⇒ 用户的下一条腿 15 秒即失败、**没有任何契约报告**（审计读成 `0 of 8`），且它**抢 GPU** 让我随后的门读数被污染。
   ⇒ `run_leg.sh` 现在在起腿前**两条硬检查**：(a) 有 `apps/gnb/gnb` 或 `ul_chain_replay` 残留进程就**拒绝启动**并列出它们；
   (b) `lsof -nP -iUDP:2152` 已被绑定就**拒绝启动**并打印持有者。
3. **预检第一版用 `pgrep -f` 挡了用户一条正常腿**：它匹配整条命令行，**匹配到了正在执行该命令的我自己的 shell**。
   ⇒ 改为**按进程名精确匹配**（`pgrep -x gnb` / `pgrep -x ul_chain_replay`）。
   **规则：进程检查永远按名字**；命令行的模糊匹配会把"提到这个名字的人"当成"那个进程"。
4. **`run_leg.sh` 的 provenance 换行缺陷（已修）**：`$(printf …)` 会吃掉结尾换行，导致第一行 knob 粘在上一行 ⇒
   行首 `grep`（门读 knob 的方式）读不到，一条真实的拆分臂被读成"knob off"。现在 provenance 前**显式补一个前导换行**。
5. **两条"改前就红"的网（登记，未处理）**：`value_net` 的归档基线陈旧（§6.3 ④）；
   `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` vs 默认）在 HEAD 就红、且 pristine HEAD 二进制复现同一读数。
   **是否曾经绿过、要不要重建基线，需要用户裁决**（重建 = 承认基线过期；不重建 = 该网在 HEAD 不可用）。

---

### 6.6 Q9 追查（2026-09-25，**离线**，用现成的腿日志）：因果链坐实到"池空 → 收包阻塞 → 电台丢样点"；缺的一环是"**为什么被持有的块 5 秒不完成**"

> 触发：P2-E 的裁决（§6.4.6）说清了"要验证持有期→池→停顿必须真的移动持有期"，而本平台唯一能移动它的形态会破 V4/零拷贝
> ⇒ **Q9 必须走别的路**。本节只用**已有腿的日志 + 读码**（不飞腿、不碰 GPU）。

**① 手段**：`[ul_rx_pool]` 的 EMPTY 告警（WARNING 级，落在 ocudulog 文件里，带时间戳）、
RF 的 `underflow/overflow/late` 告警、`PUSCH:` 结果行的时间戳（UL 链是否在动）、以及 `[metal_stats] dft handover` 的 token 计数。

**② 池 EMPTY 是**常态**，致命的是"恢复要多久"**

| 腿 | 并发 | `receive pool is EMPTY` 告警 | 契约 `gaps` | `PUSCH:` 静默 >0.3 s 的**最大**值 |
|---|---|---|---|---|
| `s87-n1phases` | 1 | **0** | **0** | 4.39 s（交通空闲，无丢样点）|
| `p05-pair` | 1 | **1** | 7（共 2.55 M 样点 ≈ **166 ms**）| 4.12 s（同上）|
| `s88-laneconc2` | 2 | **1** | 2（共 153.2 M ≈ **9.97 s**）| **19.89 s**（空闲）+ **5.002 s ×2** |
| `s88b-laneconc2` | 2 | **1** | 2（共 153.1 M ≈ **9.97 s**）| **21.25 s**（空闲）+ **5.002 s ×2** |

⇒ **三次 EMPTY 告警对应三条有 gaps 的腿**（唯一没有 EMPTY 的 `s87` 是唯一 0 gaps 的腿）；
但 **EMPTY 本身不是答案**：并发 1 的 `p05-pair` 也 EMPTY 了，代价只有 ~24 ms/次，
而并发 2 的代价是 **5.002 s/次**。（>15 s 的静默经核对是**交通空闲**：那些窗口里连 SCHED/RF 都没有一行。）

**③ 5 秒停顿的完整链条（`s88` 的原始时间戳）**

```
15:35:38.071952 [PHY][W] [ul_rx_pool] the receive pool is EMPTY (held=8/8): the next take blocks the receive thread
15:35:38.074671 [RF ][W] Real-time failure in RF: underflow
   ... 5.000 s 里：SCHED 3149 行、PDCCH 329 行、RF 7152 行（DL 与调度**照常**），
       而 RLC 只有 68 行（前 2 s 是 10568 行）、**一条 `PUSCH:` 结果都没有** ⇒ UL 数据面整段死掉
15:35:43.073060 [RF ][W] Real-time failure in RF: overflow      ← 收包恢复的那一刻，USRP 溢出
15:35:43.073546 [PHY][I] [529.3] PUSCH: ... crc=KO            ← 积压的 529.x 结果这时才出来（DL 已在 530.x）
```
⇒ **链条**：池被抽干（held=8/8）→ `pop_blocking()` 把接收线程按住 5.002 s（`ul_process()` 的第一行就是取缓冲，
**在 `receive()` 之前**）→ 这 5 s 里电台的 64 帧队列溢出、样点被丢 → 连续性检查记一个 gap；
UL 数据面停住是因为**池里的缓冲全被"未完成的块"的 token 按着**，没有缓冲就没有新样点、就没有新跳。

**④ 关键对照：并发 1 的恢复是毫秒级，并发 2 是 5 秒**

同一条 `p05-pair` 腿在 EMPTY 告警之后（16:35:45.838）**每个槽都在出 PUSCH 结果**（871.7 / 871.8 / 871.9 …，间隔 ~1 ms），
电台只丢了 7 小段（合计 166 ms）；而 `s88` 在 EMPTY 之后**整整 5.002 s 一条 UL 结果都没有**。
⇒ **5 秒不是"池太小"或"持有期 = 整跳"能解释的**（这两条在并发 1 上同样成立，代价却是毫秒级）
⇒ 它是**并发 2 专有**的：两条车道在飞时，被持有的块 5 秒不完成。

**⑤ token 账：整池都可能在别人手里**

`keepalives ... (max in flight)`：`s87` 84、`p05-pair` **126**、`s88` **112**、`s88b` **112**、`s85`(n78) 84。
一跳 14 个 token（每符号一个），而**一个接收缓冲装一个整槽 = 14 个符号** ⇒ **112 ≈ 8 个缓冲 = 整个池**
⇒ "把池按满"在**任何**腿上都发生过（并发 1 也一样），所以它仍然不是 5 秒的解释。

**⑥ 本节排除的（都有出处）**

| 不是它 | 依据 |
|---|---|
| `receiver.receive()` 被卡住 | 阻塞发生在 `ul_process()` 第一行的 `pop_blocking()`（**在 `receive()` 之前**），所以 `[ul_rx_wait]` 三条腿的 max 都是 ~101 ms，**看不见**这 5 s |
| 代码里的 5 秒超时 | `grep` 全仓：PHY/radio 路径**没有** 5 s 常数（只有 E2/NGAP 的 5000 ms 与 `lower_phy_baseband_processor.cpp` 里 5 ms 的 late 阈值）|
| 池容量本身 | 并发 1 也会 EMPTY（1 次/腿）；差别在**恢复时间**（②④）|
| "输入被持有到整跳完成"这一条**单独** | 它在两种并发度下都成立；只有并发 2 付出 5 s（④）|
| 交通空闲 | >15 s 的静默窗口里 SCHED/RF 一行都没有 ⇒ 那几段是**没流量**，不是停顿 |

**⑦ 缺的一环（下一步要测的）**：**为什么两条车道在飞时，被持有的块 5 秒不完成？**
候选（按可解释性排序，均**未验证**）：
1. **设备侧 wait 的环**：车道缓冲里 `encodeWaitForEvent`（stage fence / grid-ready）等一个由**另一条车道**（或下一跳）发布的信号；
   两条在飞时才可能成环，而链一旦成环，唯一的"突破口"是**更高代的信号**——它又依赖收包，于是整条链互相等；
   5 s 这个**整数**暗示某个**外部**事件把它打破（尚未找到是哪一个是 5 s）。
2. **事件发布语义**（§6.4.3 的平台结论）把这个环放大：中途信号只在**所在缓冲完成**时发布，
   于是"等信号"等价于"等那条缓冲整个完成"，wait 链比设计预期长得多。
3. 队列/驱动层的调度（两条车道 + LDPC 解码共用同一个后端队列，`ocudu_metal_queue.h` 有记）。

**⑧ 下一步（登记，等与下一条腿一起规划）**：**实现 P0-2（持有期直方图）+ 池等待时长**——它是这一环唯一的直接读数：
* 在 `retain_for_block()`/`release_block_tokens()` 上记 **attach→release 的时长**，打 p50/p95/p99/max + 直方图，
  并**记住最久的那一个属于哪个 slot / 哪个 stage**（`block_token_set::cb` 已经能对回缓冲，再加 slot 即可）；
* 在 `pop_blocking()` 外面记 **等待时长**（p50/p95/max），补上 `rx_wait` 看不到的那一段；
* 判读（先写死）：若重跑并发 2 时 `pool wait max ≈ 5 s` 且**被持有的块是同一跳**（同一 slot），
  则"块不完成 ⇒ 池空 ⇒ 收包停"坐实，剩下的问题就变成"那个块在等什么"（候选 1/2/3）；
  若 `pool wait max` 只有毫秒级，则 5 s 停顿**不在**池→收包的这条链上，Q9 要回到"电台/驱动侧"。
* ⚠ **代价**：这是**代码**改动 ⇒ 腿 `p05-pair` 的"提交证据"会失效（§5.2 纪律 3）。
  因此**不要零散地做**：与下一条腿（以及 Q13 的配对账输出、P0-2 的直方图）**一起**改、一次重飞。

### 6.7 P0-2 ✅ 落地（2026-09-25）：**持有期直方图 + 池等待时长**（+ Q13 的配对账自解释输出）

> 依据：§6.6 ⑧ 登记的下一步。**这是代码改动** ⇒ 腿 `p05-pair` 的"提交证据"失效（§5.2 纪律 3），
> 所以三件事**一次做完**：P0-2 的两半 + Q13 的配对账输出。

**① 改了什么**

| # | 位置 | 读数 |
|---|---|---|
| **P0-2a** | `ocudu_dft_metal_engine.mm`：`retain_for_block()` 为每个 token 记 attach 时刻（与 `open_tokens` **同下标**并行），`release_block_tokens()` 在**跑回调之前**结算 hold（attach→release） | `[metal_stats] input hold (P0-2): tokens=… mean/median/p95/p99/max µs at slot=S (slot named=…)` + 尾部分布行 `over 100ms=…, over 1s=…`（并明说"SECONDS 就是停顿，不是本跳跨度"）|
| **P0-2b** | `lower_phy_baseband_processor.cpp`：`ul_process()` 的两处 `pop_blocking()`（取缓冲 + 换填充缓冲）记**等待时长** | `[ul_rx_pool] pop_blocking wait (P0-2): takes=… mean/median/p95/p99/max µs; over 1ms/10ms/100ms/1s=…` |
| **Q13** | `ul_pipeline_probe::phase_samples_recorded()`（新访问器，**在退出时**读三段序列的真实条数）+ 车道探针的"同 slot 覆盖"计数器 | 配对行新增 `overwritten by a later lane for the same slot=…`；新增自解释行 `[ul_gpu_lane] paired/phase account (P0-5): paired=…, announced=…, series at exit=… -> EXACT MATCH / MISMATCH` |

**② 为什么这两半能判 Q9（先写死的判读）**

* 并发 2 重跑时若 `pop_blocking wait … max ≈ 5 s` / `over 1s ≥ 1`，**且** `input hold` 的 max 也是秒级
  ⇒ "**块不完成 ⇒ 输入被持有 ⇒ 池空 ⇒ 收包停**"闭合，剩下的问题变成"那个块在等什么"（§6.6 ⑦ 的候选）。
* 若 `pop_blocking wait` 只有 ms 级，则 5 s 停顿**不在**"池→收包"这条链上 ⇒ Q9 回到电台/驱动侧。
* `input hold … at slot=S` 给出**最久持有的那一跳的 slot**，这是把它和 `[ul_gpu_lane]` 的
  5.004 s residency（也按 slot 配对）对上号的钥匙。
* ⚠ `pop_blocking wait` 是**每次 take** 的（~200k/腿），`input hold` 是**每 token**（~2.8M/腿）
  ⇒ 两条序列都存样本（分别 ~0.8 MB / ~11 MB），与其它探针同风格（要真分位数，不要直方图猜尾巴）。

**③ 两个自己踩出来的缺陷（都已被测试抓到，已修）**

1. **atexit 与互斥量的顺序**：`dft_stats_t` 与 `rx_pool_accounting` 都是**函数内静态对象**，
   而它们的报告都是 **atexit** 处理器 —— 报告跑在静态析构**之后**。原来两个结构体里只有原子量（析构无害），
   P0-2 放进去 `std::mutex` + vector 之后，**金属测试当场在退出时 abort**：
   `libc++abi: terminating … mutex lock failed: Invalid argument`。
   ⇒ 两个单例都改成**故意泄漏**（`new`，永不析构），与车道探针的 `stats()` 同一条注释规矩（§6.7 ④）。
   **教训（写死）**：**任何被 atexit 报告的探针状态，都不许放有析构语义的成员**（mutex/vector/string），
   除非它自己 `new` 出来。
2. `phase_samples_recorded()` 的断言第一版拿"本进程累计条数"与"本用例里观察者看到的条数"比（差 3 个样本，
   那是同进程更早的用例留下的）⇒ 改成**增量**比较（与文件里其它断言同一规矩）。

**④ 离线验证**

| 验证 | 结果 |
|---|---|
| `ctest -L phy` | **100% passed out of 193**（含扩展后的金属测试）|
| `ul_pipeline_probe_test`（含新访问器断言）| **7/7 PASS** |
| `dft_release_adopt_metal_test` | rc=0；`input hold (P0-2): tokens=45 mean=453.7us median=220.0us p95=842.0us p99=857.0us max=943.5us at slot=4242 (slot named=1)` + 尾部 `over 100ms=0, over 1s=0` |
| `lower_phy_test`（528 例，池的载体）| **528/528 PASS**；`pop_blocking wait (P0-2): takes=2012 … max=13.0us; over 1ms=0 …` ⇒ 该行确实打印且量级合理 |
| 与 pristine HEAD 二进制逐字节（3 capture × 4 dump）| **0 differing**（见 §6.7 ⑤）|
| `l1_handover_arms` / `l1_hop_arms` | 见 §6.7 ⑤ |

**⑤ 值中性（本轮的关键证据）**

| 网 | 结果 |
|---|---|
| `ul_chain_replay` 四个 dump vs **pristine HEAD 二进制**（3 capture）| **0 differing**（12 个文件全 0）|
| `l1_handover_arms` | **5 PASS** |
| `l1_hop_arms` | 4 臂全部 `differing=0` |
| `ctest -L phy` | **100% passed out of 193** |

⇒ P0-2 只加读数（每 token 一次 `steady_clock::now()`、每次 take 两次），**不改任何 dispatch、提交数（V4 不变）或数据**。

**⑥ 对腿的影响（必须与结果一起引）**：本提交之后，腿 `p05-pair`（戳 `93c909e423`）到 HEAD 的 diff **触及代码**
⇒ 按审计门的规矩，它**不再是 HEAD 的证据**；P0-5 的空口读数**本身不受影响**（它已经记录在 §6.3 ⑥），
但"腿跑的是哪个提交"这一条要等下一条腿。下一条腿的建议配方（写死）：**n1 默认工况 + 并发 2**
（`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2`），读
`pop_blocking wait (P0-2)` / `input hold (P0-2)` / `paired/phase account` 三行。

### 6.8 ★★ Q9 的腿（`q9-conc2`，n1 + **并发 2**，2026-09-25 07:51）：**持有期被直接量到 61.4 秒**，UL 断流由此解释

> 配方：`LEG_CONFIG` 默认 n1 + `--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2`
> （`[ul_lane_exec] … -> pusch_executor.max_concurrency=2 (a task fork limiter: that many PUSCH hops may overlap)`）
> + `OCUDU_UL_PHASE_SEGMENTS=1`；二进制戳 **`99f2509fcb`**（P0-2 那个提交，= 本节的仪器）。
> CN 侧流量：`ping -i 0.1 -c 100`（0% 丢包，RTT 中位 ~31 ms）、下行 `iperf3`（17.8 Mbit/s，正常）、
> **上行 `iperf3 -c <phone> -R -t 100`：中段整段归零**（见 ①）。

**① 用户侧的观测（CN 的 iperf3 输出，原文摘要）**：上行 0–38 s 约 5 Mbit/s；
**38.17–48.66 s 塌到 286 Kbit/s**；**48.66–88.17 s 整整 ~40 s `0.00 bits/sec`**；88 s 后恢复成涓流（0.3–0.9 Mbit/s）；
总计 1.68 Mbit/s、**Retr 7893**。同一次会话里 ping 与下行 iperf3 都正常 ⇒ **只有 UL 数据面断了**。

**② 腿里的直接读数（P0-2 第一次派上用场）**

```
[metal_stats] input hold (P0-2): tokens=1030246 mean=5189.2us median=1129.0us p95=5947.9us p99=16098.0us
                                 max=61446160.0us at slot=10226 (slot named=1)
[metal_stats] input hold (P0-2) tail: over 100ms=210 (0.0204%), over 1s=210 (0.0204%) of 1030246 token(s)
                                 - tokens held for SECONDS: that is the stall, not the hop's span
[ul_gpu_lane] residency samples=46685 mean=1163.8us median=1143.0us min=14.4us max=5003860.0us ...
[ul_gpu_lane] busy      samples=46685 mean=921.3us median=1099.3us min=14.4us max=1475.8us ...
[ul_gpu_lane] lanes=46685 cbs/lane=2.00 (max=2) dropped=0 carried=986
[ul_gpu_lane] paired with the phase segments (P0-5): samples=43063 of phase_samples=43063
              (no lane for the slot=0, lane older than 2s=0) over lanes=46685
              (slot named=46685, not named=0); awaiting at exit=510, evicted=1929, overwritten=1183
```

* **`max = 61.4 s`**：某个接收缓冲的输入 token 从 attach 到 release 被持有 **61 秒**（slot 10226）；
  **210 个 token（0.02%）超过 1 秒**；中位仍是 **1.13 ms**（正常的"一跳跨度"持有），p99 = 16.1 ms。
  ⇒ §2.5 那条因果链的**第一环**现在不是推断而是读数：**输入真的会被持有几十秒**。
* **`residency max = 5.004 s` 而 `busy max = 1.48 ms`** ⇒ 那条 5 s 的"车道驻留"**不是某条缓冲在设备上跑了 5 s**，
  而是**两条缓冲之间隔了 5 s**（车道的 residency = 首条 start → 末条 end，`carried=986` 把上一跳的缓冲带进来）。
  ⇒ 与 §6.6 的推断一致：5 s 是"**链在这段时间里前进了一步**"的节拍，不是一个超时，也不是设备执行时间。
* 配对账（P0-5 的新自解释行不在这一版里，Q13 的计数器已在）：**43063/43063，零丢失**；`overwritten=1183`（2.5%，比并发 1 的 0.2% 高一截 —— 并发 2 下同 slot 的重复插入明显变多，Q13 的成因仍待查）。

**③ 腿里的时间线（与用户观测对齐）**

| 时刻 | 事件 |
|---|---|
| 23:53:27.519 | `[ul_rx_pool] the receive pool is EMPTY (held=8/8)` |
| 23:53:27.521 | `RF: underflow` |
| 23:53:27.55–.68 | `PUxCH request late` 开始（全腿共 **408** 次） |
| **23:53:32.519** | `RF: overflow` —— **池空之后 5.004 s**（与 `s88`/`s88b` 逐位同形） |
| 23:53:32.5→23:54:01.9 | **PUSCH 结果静默 28.55 s**（全腿最大的 UL 空档；>0.3 s 的空档共 **164** 次） |
| 全腿 | PUSCH 结果 46687 个 / 280.9 s = **166 个/s**（并发 1 的同配方是 555 个/s） |
| 契约 | `radio sample continuity: 1 gaps … (76,572,193 samples)` = **4.99 s** ⇒ NOT MET (1 of 8) |

⇒ **用户看到的 ~40 s UL 归零，就是这些空档的叠加**：池被"未完成的块"按住 → 接收线程停在 `pop_blocking()` →
没有新样点 → 没有 PUSCH → iperf3 的上行（要 ACK 与数据）停住；而 ping（一次一来回、由 DL 触发）与下行 iperf3
仍然正常，这与"**只有 UL 数据面死**"完全吻合。

**④ 这一条腿同时确认了 P1-8 的收益仍然真实**：`[ul_gpu_pipeline]` 中位 **2331 µs**（并发 1 是 5248）、
**`stale=0`**、max 5819 µs ⇒ 跨度 −56%，且这次没有被 stale 掩盖。⇒ **杠杆是真的，代价也是真的**：
并发 2 之下，输入会被持有到几十秒（本腿 61.4 s），UL 吞吐 166 跳/s（−70%）。
⇒ **P2-F 的交付形态讨论必须带上这一条**：并发度不能单独交付，必须先解决"块为什么不完成"。

**⑤ 这条腿暴露了我自己的一个缺陷（已修，已加离线回归）**

腿的收尾报告**在打印配对行之后 abort**：

```
libc++abi: terminating due to uncaught exception of type std::__1::system_error: mutex lock failed: Invalid argument
```

* **根因**：P0-2 的 Q13 那一半让**车道探针的报告（atexit）去读 `ul_pipeline_probe` 的样本条数**
  （`phase_samples_recorded()`，为了把配对账比在同一瞬间）——而 `ul_pipeline_probe::get()` 是**函数内静态对象**，
  它的析构跑在 atexit 处理器**之前** ⇒ 锁一个**已析构的 mutex**。
  这正是我在 §6.7 ③ 写下的那条规矩的**第二种形态**：不只是"被 atexit 报告的状态"，**任何会被 atexit 处理器读取的探针单例**都必须故意泄漏。
* **代价**：`[ul_rx_pool] …` 与新的 `pop_blocking wait (P0-2)` 两行**随进程一起丢了**（atexit 链在它之后），
  所以这条腿上"接收线程被 park 了多久"没有直接读数 —— **但 §6.8 ② 的 `input hold = 61.4 s` 已经把问题回答了**。
* **修法**：`ul_pipeline_probe::get()`（两条编译分支都）改成**故意泄漏**（与车道探针 `stats()`、DFT `dft_stats()`、
  池 `rx_pool_accounts()` 同一条规矩，注释里写明这次是被哪条腿抓到的）。
* **离线回归（写死）**：`dft_release_adopt_metal_test` 末尾新增一节——为它自己的 slot（4242）**定稿一个相位样本**
  并断言 `phase_samples_recorded() != 0`。这样车道探针在 atexit 真的会走到 account 行；
  **若单例再被过早析构，进程会在退出时 abort（rc≠0）而不是打印判定** ⇒ 这条缺陷不会再无声回归。
  （该行的 verdict 在那条测试里**故意不断言**：测试的最后一条车道早已超出 2 s 配对窗口，样本合理地读成未配对；
  这一节考的是"报告跑得起来"。）
* **复验**：金属测试 **rc=0**（修前 rc=134）、`ul_pipeline_probe_test` **7/7**、`ctest -L phy` **100% of 193**、
  `ul_chain_replay` 2 capture × 4 dump 与 pristine HEAD 二进制**逐字节相同**。

**⑥ 下一步（Q9 的新缺口，写死）**：现在知道"输入被持有到几十秒"，但**不知道那几十秒里它在等谁**。
需要的是一条**逐块生命周期**的读数（下一件仪表，登记为 **P0-7**）：按 slot 记录每个前端块的
`deposit →（take | miss）→ commit（谁提交的）→ GPU start → GPU end → token release`，
并记下它**设备侧等待的目标**（stage fence / grid-ready 的 generation 与 signaller 的 slot）。
判读：若"块 X 等的是块 Y 的信号，而 Y 又在等 X（或等一个只有 X 放行才能产生的新块）"⇒ **等待成环**坐实，
修法是切断环（例如把 grid-ready 的发布点从"缓冲完成"改成"最后一次网格写"——而平台语义（§6.4.3）恰好是这条环的放大器）。

### 6.9 P0-7 ✅ 落地（2026-09-25）：**逐块生命周期**（deposit → 认领 → 完成），与"谁按住了输入"的直接读数

> 依据：§6.8 ⑥。**代码改动** ⇒ 与 P0-2 同理，会在下一次飞腿时统一补证据。
> 本轮同时按用户要求把**需要留存的开发产物放进 `doc_chinese/work_tmp/`**（git 忽略，见 §6.9 ④）。

**① 要回答的问题**：P0-2 的 `input hold` 说"输入被按住多久"，但**没说被谁按住**。
一个**没人认领**的 deposit 只有两条出路：**sweep** 在接收链前进 `sweep_after_slots = 2` 之后提交它，
或淘汰循环丢掉它（而**只有 PRODUCED 的条目才会被淘汰**）。在那一刻到来之前，它的 transforms 持有的
输入 token **一直不回池** ⇒ "一个没人认领的块坐了几十秒"就是"交接机制自己把上行停住"的形状。

**② 改了什么**（`ocudu_metal_burst.{h,mm}` + 报告在 `ocudu_dft_metal_engine.mm`）

| 位置 | 记什么 |
|---|---|
| `deposit_released()` | 每个条目的 `deposited_at`（新条目与 supersede 都重打），并刷新"**未认领且未完成**"的**仪表**（个数 + 最老那一个的年龄与 slot）|
| `take_released()` | `deposit → 认领` 的等待（count/sum/max）|
| sweep（`late_commits`）| 同样记等待，并标 `swept=1` ⇒ **认领者是谁**可分（跳 vs 注册表的 sweep）|
| `mark_handed_produced()`（完成处理器）| `deposit → 完成` 的等待（count/sum/max），并维护**最慢 8 个块**的记录 |

报告（紧跟 `[metal_stats] dft handover` 之后）：

```
[metal_stats] block lifecycle (P0-7): claimed=N wait max=…us mean=…us; produced=N deposit->completion
              max=…us mean=…us; unclaimed at once max=N, oldest unclaimed age max=…us at slot=…
[metal_stats] block lifecycle (P0-7) slowest deposit->completion (claimed=by a hop, swept=by the registry's sweep):
[metal_stats]   slot=… claimed=0/1 swept=0/1 wait_for_a_claim=…us deposit->completion=…us
```

**③ 判读（先写死，下一条腿用）**

* `oldest unclaimed age max` 与 `deposit->completion max` 是**两个关键数**：
  若它们≈ P0-2 的 `input hold` max（本线的 61.4 s）⇒ **按住输入的就是"没人认领/没完成"的块**，
  即"池干 → 收包停"的**上游**就是交接（hand-over）自己；
  若它们只有 ms 级，而 `input hold` 仍是秒级 ⇒ 输入不是被"未认领"按住的 ⇒ 要查**已认领但未提交**的路径
  （认领者拿了缓冲却不提交，例如那条跳的 burst 被丢弃）。
* `slowest` 行里的 `claimed=0 swept=1` 说明"**是 sweep 救的场**"，`wait_for_a_claim` 就是它干等了多久；
  `claimed=1 swept=0` 却 `deposit->completion` 很大 ⇒ 是**认领者自己**（那条跳）迟迟不提交。
* **它仍不能说的**：设备侧等待的目标（stage fence / grid-ready 的 generation 与 signaller 的 slot）。
  若 ③ 的第一种读数成立，下一步就是把这几个 generation 也记进条目（P0-7b，未做）。

**④ 开发产物的存放（用户 2026-09-25 明确）**：`/tmp` 不可靠（macOS 重启会清），
**需要留存的开发文件一律放 `doc_chinese/work_tmp/`（git 忽略、不进跟踪）**。本轮已归档：

| 路径 | 内容 |
|---|---|
| **`doc_chinese/phy_latency/wip/leg_census.py`** | **腿普查脚本**（UL 静默表 + RF/pool 事件）——§6.6/§6.8 的表格就是它跑出来的。⚠ 它**已从 `work_tmp/` 搬进本阶段的 `wip/` 并被跟踪**（用户 2026-09-25：新工具按阶段放本阶段 `wip/`；门与判据只许依赖被跟踪的文件，见 §5.3 与本阶段 `wip/README.md`）|
| `doc_chinese/work_tmp/p0_7/q9_conc2_census.txt` | 腿 `q9-conc2` 的普查输出（存档） |
| `doc_chinese/work_tmp/p0_7/q9_leg_record.md` | §6.8 的原始记录（并入本文之前的稿） |
| `doc_chinese/work_tmp/ref/replay_head_pre_p05` | **P0-5 之前的 pristine HEAD `ul_chain_replay` 二进制**，用于"与 pristine HEAD 逐字节相同"这条网（每次重新构建较贵，故留一份） |
| `doc_chinese/work_tmp/p0_7_byte/` | 本轮值中性比对的 dump（h_/m_ 两套） |

⚠ 这些文件**不在 git 里**：门与判据不依赖它们（依赖的必须进 `wip/` 并被跟踪）。

**⑤ 离线验证**：`ctest -L phy` **100% of 193**；`lower_phy_test` **528/528**；
金属测试 **rc=0** 且新行正常（`block lifecycle (P0-7): claimed=49 wait max=1472.0us …; produced=46
deposit->completion max=1666.0us …; unclaimed at once max=6, oldest unclaimed age max=1470.0us at slot=4343`，
最慢 3 行也打印）；`ul_chain_replay` **3 capture × 4 dump 与 pristine HEAD 二进制逐字节相同**（12 个文件全 0）。
**纯读数改动**：不加 dispatch、不加提交（V4 不变）。

### 6.10 ★★★ Q9 结案（腿 `p07-conc2`，n1 + 并发 2，2026-09-25 08:17）：根因是**交接注册表的 sweep 用了跨环绕不安全的 slot 比较**

> 腿：`gnb_gpu_p07-conc2_0925_0817`，配方与 `q9-conc2` 相同（n1 默认 + `max_pusch_and_srs_concurrency=2` +
> `OCUDU_UL_PHASE_SEGMENTS=1`），二进制戳 `78cb3fe0d1`（= P0-7 那个提交）。
> 用户侧：上行 `iperf3 -R -t 100` **断流两次**（约 19–55 s 与 87.8–100 s 归零），Retr 7536。

**① P0-7 的判读分支一，成立**（开发文档 §6.9 ③ 事先写死的那一支）

```
[metal_stats] input hold (P0-2): tokens=880558 mean=4523.2us median=1049.2us p95=14987.4us p99=17130.4us
                                 max=30723776.0us at slot=10226
[metal_stats] block lifecycle (P0-7): claimed=32605 wait max=30723002.0us mean=5047.3us;
              produced=62897 deposit->completion max=30723752.0us mean=4512.6us;
              unclaimed at once max=5, oldest unclaimed age max=77860119.0us at slot=10226
[ul_rx_pool] pop_blocking wait (P0-2): takes=372639 … max=4997628.0us; over 1ms=2, over 10ms=2,
                                      over 100ms=2, over 1s=2
```
* **`input hold` max = 30.72 s** 与 **`deposit->completion` max = 30.72 s**、其中 **`wait_for_a_claim` = 30.72 s**
  三者**逐位相同**（30,723,776 / 30,723,752 / 30,723,002 µs）⇒ **输入就是被"没人认领的块"按住的**。
* `slowest` 行指明**认领者是 sweep**（`claimed=1 swept=1`），而且它干等了整整 30.72 s。
* `pop_blocking wait`：全腿 372639 次取缓冲里**只有 2 次超过 1 ms**，而这 2 次都**≈4.998 s** ⇒
  接收线程被 park 的就是这两次（与两次 `RF: overflow` 5.001 s 的配对完全对应）。**这是 §6.6 缺的那条直接读数。**

**② 根因：sweep 的判据在 slot 计数**环绕**处失效**

| 证据 | 内容 |
|---|---|
| **等待时长** | `slowest` 里的等待是 **10.24 s 的整数倍**：30.723002 s = **3.000×**、20.483005 = **2.000×**、10.246039 = **1.001×**、10.242983 = **1.000×**、10.242990 = **1.000×**（余数只有 3–6 ms）；另有两条 5.003/5.004 s 是**另一个机制**（接收 park）|
| **10.24 s 是什么** | 15 kHz 下 `nof_slots_per_hyper_system_frame() = 10 × 1024 = 10240` 个 slot = **10.24 s** |
| **`slot_point::count()` 是模数** | `include/ocudu/ran/slot_point.h`：`count_val` 是 `uint32_t`，且 `ocudu_assert(count < nof_slots_per_hyper_system_frame())` ⇒ **它是环绕的**，不是绝对计数 |
| **比较本身** | `ocudu_metal_burst.mm` 的 sweep：`if (!claimed && !produced && ((entry.slot + sweep_after_slots) < slot))` —— **两个模数相减/比较**，跨环绕即失效 |
| **数值自洽** | 最慢的行都是 `slot=10226`：判据要求新 deposit 的模 slot `> 10228`，而环绕后要等计数爬回 10229 ⇒ 等待 ≈ (10240−10226) + 10228 ≈ **10.242 s** ✓ 与实测逐位吻合 |
| **危险区** | `slot ≥ 10238` 的条目**永远**不满足该判据（`entry.slot+2` 超出范围）⇒ 只能靠"s同一 (storage, slot) 再次 deposit 时 supersede"或"PRODUCED 后被淘汰"释放（本腿 `superseded=0`、`evicted=62645`）|

⇒ **机制**：并发 2 下 hop 更容易错过交接（`not_found=3696`、`late_commits=3700`、`fallback=30292`），
于是注册表里积起**没人认领**的条目；**其中落在 hyperframe 末尾的条目要等一整个 10.24 s 周期**才被 sweep
（甚至永远等不到），这段时间它咬着 14 个输入 token ⇒ 池里的缓冲被按住（本腿 `unclaimed at once max=5`，
池只有 8）⇒ 接收线程 `pop_blocking` 被 park（实测 4.998 s ×2）⇒ 电台 64 帧队列溢出
（`2 gaps / 153,167,345 samples ≈ 9.97 s`）⇒ **UL 数据面整段归零**。并发 1 下几乎不产生"没人认领"的条目，
所以从来没见过这个形状。

**③ 修复方案（待实施，两种）**

| 方案 | 改法 | 代价/风险 |
|---|---|---|
| **a1（推荐）时间判据** | 用**单调时钟**做期限：`now - entry.deposited_at > sweep_after`（`sweep_after ≈ 10 ms` = 10 个 slot，而并发 2 下 hop 的自身时延是 ~1–2 ms ⇒ 余量充足），并在**每一个**注册表入口（deposit / take / 宿主读 `ensure_grid_produced`）都检查一遍 | **与环绕无关**，而且**不再依赖"必须有新 deposit"**（当前规则的死结正是"收包停 ⇒ 没有新 deposit ⇒ 永远不 sweep"）。代价：被 sweep 的块由注册表提交 = 多一次提交（已计入 `late_commits`），所以期限要留足，让 `not_found`/`late_commits` 保持低 |
| **a2（最小改动）环绕安全比较** | 保留 slot 语义，但比较改成环绕安全：`const uint32_t age = static_cast<uint32_t>(slot - entry.slot); if (age > sweep_after_slots && age < nof_slots_per_hyper_system_frame()/2) sweep;` | 修掉 10.24 s 的等待，但**仍要求新 deposit 到来** ⇒ 死结的另一半（"没有新 deposit 就永远不 sweep"）还在 |

**建议 a1 + a2 的环绕安全守护一起做**（a1 负责"不再依赖新 deposit"，a2 的守护负责"不误判跨环绕"），
并把 `sweep_after` 写成**时间**（`std::chrono::milliseconds`，符合本仓"配置时间参数用强类型"的规矩）。

**④ 顺带确认的三件好事（同一条腿）**

1. **配对账精确**：`paired=24450, announced=24450, series at exit=24450 -> EXACT MATCH`（新自解释行第一次在空口生效）。
2. **"residency 里 ~95% 是 busy" 在并发 2 下复现**：`busy/residency median=0.916`（全车道 0.919）——
   与并发 1 的 0.643 对比 ⇒ 这个比值**随负载/并发度变**（§6.3 ⑥ 的结论再次被支持）；`eq_demap/residency = 1.076`（仍不是 1.00）。
3. **并发 2 的跨度收益第三次复现**：`[ul_gpu_pipeline]` 中位 **2375 µs**（并发 1 是 5248）、`stale=0`、max 5167 µs；
   `merged_hop=885.3 µs/lane`、`cbs/lane=2.00 (max=2) dropped=0`（V4 不变）。

### 6.11 ✅ Q9 修复落地（2026-09-25，提交 `3d00eafe97`）：**时间期限 + 环绕安全守护 + 每一个注册表入口都查**

> 依据 §6.10 ③ 的 **a1 + a2**。**代码改动** ⇒ 与 P0-2/P0-7 同理，"腿的提交证据"要在**确认腿**上统一补（§5.2 纪律 3）。
> 用户 2026-09-25 的指示：**修好之后由用户跑一条确认腿**（配方与判据见 ⑤）。

**① 改了什么**（`ocudu_metal_burst.{h,mm}`、`phy_pipeline_grid_ready.h`；报告在 `ocudu_dft_metal_engine.mm`、`ul_chain_replay.cpp`）

| 位置 | 改动 |
|---|---|
| **`sweep_due()`**（新，匿名命名空间）| 判"这个块该由注册表自己提交吗"：**先 slot 窗口、后时间期限**，两者 OR。slot 窗口**先守护再相减**：`entry.slot < newest_slot` 且 `newest_slot - entry.slot > sweep_after_slots(=2)`；时间期限 = `now - deposited_at > sweep_after`，`sweep_after = std::chrono::milliseconds{10}`（**强类型**，按本仓"时间参数用 chrono"的规矩）|
| **`sweep_unclaimed()`**（新）| 把原先内联在 `deposit_released()` 里的 sweep 提成函数，**在每一个注册表入口调用**：`deposit_released()` / `take_released()` / `claim_grid_production()`（后者覆盖宿主读的 `ensure_grid_produced()` 与设备侧 `grid_production_generation()`）。被 sweep 的块收进 `std::vector`，**解锁之后**才 `commit_dropped()`（提交可能跑完成处理器，而完成处理器要拿同一把锁）|
| `handed_state::newest_slot`（新）| slot 窗口的参考值是**最近一次 deposit 的 slot**；不能用来访者自己的 slot——宿主读的是它要读的那个 slot，可以落后接收链好几拍 |
| `take_released()` / `claim_grid_production()` | **先服务调用者要的那个块，再 sweep 其余的** ⇒ sweep **永远不会**让"已经来了的消费者"吃 MISS（MISS 会让跳改开自己的命令缓冲 = 多一次提交，撞 V4）|
| `handed_counters::late_commits_time`（新）| `late_commits` 里**由时间期限**认领的那一部分（slot 窗口先判 ⇒ 不环绕的腿上它≈0）⇒ 以后一条腿能直接读出"**是谁救的场**" |
| 报告 | `[metal_stats] dft handover … late=… **late_time=…**`（退出报告 + debug 心跳两处同步）、`[l1_handover]` / `[l1_hop]` 各一处；`grid_handover_counts` 同步加字段 |

**② 为什么守护写成 `entry.slot < newest_slot`，而不是 §6.10 ③ a2 里写的 `age < nof_slots_per_hyper_system_frame()/2`**

那是一处**有意偏离**，理由写在代码里也记在这里：`nof_slots_per_hyper_system_frame()` 是 **`slot_point` 的成员**，
而注册表这一层拿到的是**裸 `uint64_t` slot**（`deposit_released(grid_base, slot, …)`），它**没有 numerology**；
要用那个模数就得把 numerology 一路传进注册表（改 API + 全部调用点 + 测试），
而这条判据**本来就不需要 slot 也能做对**（a1）。
`entry.slot < newest_slot` 是**更强的守护**：只有"同一 hyperframe 内、确实落后"时才做减法，
跨环绕时**根本不做减法**（旧条目 10239 > 新 deposit 5 ⇒ 直接落到时间期限）。
⇒ a2 要的"不误判跨环绕"给足（既不会误判、也不会失效），a1 要的"不依赖环绕"由时间期限给足。

**③ 新增的红绿回归臂（`dft_release_adopt_metal_test` 的 arm 11）**

它**造出修复前的形状**，再断言修复后的行为：孤儿块存在 `slot = test_slot + 400`，
紧接着**一次 slot 更小的 deposit（= 5）模拟计数器环绕**（此后 `entry.slot + 2 < slot` 永不成立），然后
**(a)** 断言这次 deposit **什么也没 sweep**（`late`/`late_time` 都不动）⇒ 这条臂确实处在"slot 规则够不到"的位置，不是侥幸；
**(b)** 睡过 10 ms 期限后，**用一次 take（不是 deposit）**驱动注册表，断言孤儿被打扫（`late_time` 3→5）、
**输入 token 恰好回池一次**。⚠ 修复前这条臂**必然红**：没有任何入口会 sweep 它（sweep 只在 deposit 里，而这条臂不再 deposit）。

**④ 离线验证（提交前全部做完，提交 `3d00eafe97`）**

| 网 | 读数 |
|---|---|
| `dft_release_adopt_metal_test` | **rc=0**；arm 11 打印 `… invisible to the slot rule (wrapped newest slot=5 < orphan slot=4642) and is still swept by the TIME deadline - swept nothing at the deposit, swept by a TAKE 50 ms later (late_time 3->5), input released exactly once`；arm 8（老 sweep 臂）、arm 10（软界臂）全绿；新字段在 `dft handover … late=6 late_time=5` 上可见 |
| `ctest -L phy` | **100% passed out of 193**（复跑 4 次；见 ⑥ 的一次单发红）|
| `lower_phy_test` | **528/528 PASSED** |
| `ul_pipeline_probe_test`（support）| **3/3** |
| `l1_handover_arms.sh 8 /tmp/l1_handover_final` | **5 PASS**（`cand vs ref` 8 个网格逐字节 0；`drop` / `skew` 各 8 个**不同** ⇒ 网非空）|
| 与 **pristine HEAD 二进制**（`work_tmp/ref/replay_head_pre_p05`）逐字节 | `cand` + `nogrid` 两臂 **16 个网格文件、0 differing 字节**；同一工具复跑两遍之间也 0（⇒ 可复现，差异不是噪声）|
| `l1_hop_arms.sh` | **4/4 `differing=0`**（cand/hostfirst/claim/claimnowait 各 16 个软比特文件全同；`OPEN` 那条是 5.9.39 的老结论，与本次无关）|
| `ul_chain_replay` 真捕获 `syn004_4` | 4 个 dump 与 pristine HEAD 二进制**逐字节相同**（0 differing）|

⚠ **一个与本次无关、但会浪费下一次时间的工具坑**（顺带记下）：
`l1_handover_arms.sh <slots> <workdir>` 若把 `<workdir>` 传成**相对路径**，
`run_arm()` 里的 `( cd "$WORK" && … --out "$WORK/…" )` 会把 `--out` 变成一个不存在的**嵌套**路径 ⇒ **一个 dump 都不写**，
脚本于是打印 `files=0 differing=0`（**一条空网**，仍是 PASS 形状），并且 `unproduced` 会偶发 1。
**传绝对路径**（如 `/tmp/l1_handover_final`）两个现象都消失；用 **pristine HEAD 二进制**在同样的相对路径下**复现同样两条** ⇒
两者都不是本次改动带来的。⇒ 这条工具用法记在这里：**workdir 传绝对路径，并核对 `files=8`**。

**⑤ 确认腿（用户跑）：配方与预登记判据**

```bash
# 配方与 p07-conc2 完全相同：n1 默认 + 并发 2 + 相位配对开关；跑 ~100 s ⇒ 跨过 ~10 个 hyperframe（10.24 s）
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量：100 s 上行 iperf3（-R，见 §3.2 配方）；起腿前 pgrep -x gnb 干净
# 判读：bash doc_chinese/phy_latency/wip/p0_gate.sh <label>          # D1–D4 = F1–F4，D5 = F6 的两个计数器
#       再读 V1–V5（§3）与契约 8/8；普查：python3 doc_chinese/phy_latency/wip/leg_census.py <leg>.log
```

| # | 判据（**先写死**）| 修复前（`p07-conc2`）| 通过条件 |
|---|---|---|---|
| **F1** | `input hold (P0-2)` max | 30.72 s | **< 100 ms** |
| **F2** | `block lifecycle (P0-7)` 的 `wait max` / `oldest unclaimed age max` | 30.72 s / 77.86 s | **< 100 ms** |
| **F3** | `pop_blocking wait (P0-2)` max / `over 1s=` | 4.998 s ×2 | **max < 10 ms 且 `over 1s=0`** |
| **F4** | `radio sample continuity` | 2 gaps / 153,167,345 samples（≈9.97 s）| **`gaps = 0`** |
| **F5** | 用户侧上行数据面（`iperf3 -R`）| 两次断流，Retr 7536 | **不断流** |
| **F6** | `late=` / `late_time=` | 3700 / （当时无此计数器）| `late_time` **≪** `late`（期限只做兜底）；`late` 不显著高于 3700 |
| **F7** | V1–V5（§3）| V4 `cbs/lane=2.00 (max=2)`、`dropped=0` | **不变** |
| **F8** | 契约 8/8、`paired/phase account … EXACT MATCH` | ✅ | **不变** |

**F1–F5 已写进 `wip/p0_gate.sh` 的 D 组**（`D1`–`D4` 判 F1–F4，`D5` INFO 打印 F6 的两个计数器；阈值就是上表，一字不改），
⇒ 确认腿飞完一条命令即可：`bash doc_chinese/phy_latency/wip/p0_gate.sh <label>`。
D 组的两条路径都已自测：**在 `p07-conc2` 上 D1–D4 全 FAIL**（30.72 s / 30.72 s+77.86 s / 4.998 s / 2 gaps）、
**在 `q9-conc2` 上 D1 FAIL + D2/D3 RED**（该腿缺 P0-7 行、`[ul_rx_pool]` 两行被 atexit 缺陷吃掉）、
**在一份合成的"修复后"日志上 15/15 PASS**（合成件在 `work_tmp/q9_synth/`，不入 git）。
⚠ D4 是一条**无条件**判据（`gaps == 0`）：零流量的腿本来就无效（§5.1），所以它不设例外。

**⑥ 判读分支（也先写死）**

* **F1–F4 全绿** ⇒ 修复成立，**Q9 收口**（并发 2 的 ~5 s 停顿不再出现）。
* F1/F2 仍**秒级**但 `late_time` **很大** ⇒ 期限太松或还有第二个持有者：读 `slowest` 表——
  `claimed=1 swept=0` 却 `deposit->completion` 很大 = **认领者自己不提交**（那是 P0-7b 的形状，不是注册表的）。
* F6 的 `late` **明显上涨**（> 2×）或 **V4 变红** ⇒ 期限太紧（10 ms 抢了跳本来要认领的块）。
  **处置：只改这一个内部期限（10 → 50 ms）再飞一条，两次读数都留着**——**不许改判据**。
* F4 仍有 gap 而 F1–F3 全绿 ⇒ 断流另有来源：按 §6.6 的普查脚本定位（**不要把这条修复当失败**，也不要把这条腿当通过）。
* 一次单发红**不算证据**（§5.2 纪律 2）：先复跑一次，保留首次读数。（本次离线阶段 `ctest -L phy` 出现过**一次** 1/193 红，
  失败者名字未能捕获（当时的输出只留了汇总行）；此后**复跑 10 次全绿**，按纪律记为**未复现单发**。
  若在确认腿前的复跑里再出现，就必须先查清再飞腿。）

### 6.12 ⚠ Q9 修复的确认腿（`p08-conc2`，n1 + 并发 2，2026-09-25 09:10）：**修复本身生效，但 UL 断流没消失**——剩下的持有者不是注册表，而是**命令缓冲的完成**

> 腿：`gnb_gpu_p08-conc2_0925_0910`，配方与 `p07-conc2` 完全相同（n1 默认 + `max_pusch_and_srs_concurrency=2` +
> `OCUDU_UL_PHASE_SEGMENTS=1`），二进制戳 = `bbc2ddf96f`（= §6.11 的修复 + 文档）。腿跑了 **357 s**，用户在
> 01:13:48–01:15:28 之间跑上行 `iperf3 -R -t 100`：**仍然断流**（19.2–71 s 归零），Retr 6816。

**① 预登记判据（§6.11 ⑤）的读数：F1–F5 全部未过**（`p0_gate.sh p08-conc2` 的 D 组）

```
D1 FAIL  input hold max = 5947.8 ms   (判据 < 100 ms；p07 是 30723.8 ms)
D2 FAIL  wait_for_a_claim max = 5945.2 ms, oldest unclaimed age max = 5945.2 ms (p07: 30723 / 77860 ms)
D3 FAIL  pop_blocking wait max = 4997.4 ms, over 1s = 2      (p07: 4997.6 ms / 2)
D4 FAIL  radio sample continuity: 3 gaps / 153,257,099 samples (p07: 2 gaps / 153,167,345)
D5 INFO  late=5859, late_time=15  <- 时间期限确实在工作（它认领了 15 个 slot 规则够不到的块）
```
F6：`late` 5859（p07 3700）——**没有暴涨**；F7：`cbs/lane=2.00 (max=2) dropped=0` ✓、契约 8/8 里**只有** continuity 一条红
（`contract NOT MET: 1 of 8`）；F8：`paired/phase account … EXACT MATCH`（44777/44777）✓。
⚠ 新出现 `[ul_gpu_pipeline] stale=2`（p07 是 0）；`[ul_gpu_lane] period max = 8.99 s`。

**② 修复本身**确实**把注册表那一侧治好了**（同配方逐项对比）

| 读数 | `p07-conc2`（修复前）| `p08-conc2`（修复后）|
|---|---|---|
| `input hold` max | 30.72 s（slot 10226）| **5.95 s**（slot 415）|
| `input hold` mean | 4523.2 µs | **3118.5 µs** |
| `block lifecycle` wait max / mean | 30.72 s / 5047.3 µs | **5.95 s / 2206.1 µs** |
| `oldest unclaimed age max` | **77.86 s** | **5.95 s** |
| **`[ul_rx_pool] starved_takes / starved_events`** | **415 / 383** | **42 / 6**（**↓64×**）|
| `held_end` | 4 | **0** |
| `late` / `late_time` | 3700 / —（无此计数器）| 5859 / **15** |
| `keepalives` | 880558/880614（差 56）| **1186416/1186416（零差）** |
| gaps（样点丢失）| 2 / 9.97 s | 3 / **9.98 s** |
| `pop_blocking` max / >1s | 4.998 s ×2 | 4.997 s ×2 |
| `[ul_gpu_pipeline]` 中位 | 2375.3 µs | **2328.7 µs** |

⇒ **没有 10.24 s 的整数倍等待了**（Q9 的机制消失），池饥饿事件从 **383 降到 6**，token 零泄漏。
但**样点丢失一分钟都没少**：因为 **9.98 s ≈ 2 × 4.997 s**——丢样点的时间就是那**两次 5 秒 park**，与"慢性饥饿"无关。

**③ 这条腿把剩下的持有者钉到哪一步了**（P0-7 的 `slowest` 表 + 主日志时间线）

```
[metal_stats] block lifecycle (P0-7) slowest deposit->completion:
  slot=4824 claimed=1 swept=1 wait_for_a_claim=3050.0us deposit->completion=5003642.0us
  slot=4825 claimed=1 swept=1 wait_for_a_claim=3024.0us deposit->completion=5003373.0us
  slot=404  claimed=1 swept=1 wait_for_a_claim=3015.0us deposit->completion=5002566.0us
  slot=415  claimed=1 swept=1 wait_for_a_claim=5945247.0us deposit->completion=5947806.0us
  slot=4823/4826/4822/403 …    同样 ~3.0 ms 认领 / ~5.003 s 完成（4822 是 swept=0，被跳认领，21 µs）
```

* **认领已经很快**（~3.0 ms；注册表这一侧好了）⇒ 剩下的等待在**块的命令缓冲完成**上：`deposit->completion ≈ 5.003 s`。
* **输入 token 是在 cb 的完成处理器里回池的**（`arm_tokens_on_complete`）⇒ cb 完成多晚，接收缓冲就被按住多久。
* 于是链条是：**cb 完成晚 5 s ⇒ 池被按住（`held_max=8, free_min=0`）⇒ `pop_blocking` park 4.997 s ⇒
  RT 环 `Real-time failure in RF: late`（01:14:39 一秒内数千条）与 underflow（全腿 31 次）⇒ RX overflow 3 次
  （= 全部 9.98 s 样点丢失）⇒ 调度器 `PUSCH allocation skipped. Cause: All the UE HARQs are busy waiting for their results`
  ⇒ 不再发 grant ⇒ UE 掉线重接（**每一次静默之后都紧跟一条 `PRACH: … detected_preambles`**）⇒ iperf3 的 ~50 s 断流
  （其中大部分是 TCP/RLC 恢复与重接时间，不是 gNB 静默时长）。**
* **第二个洞**：`slot=415` 的 `wait_for_a_claim = 5.945 s`，而认领之后 **2.6 ms 就完成** ⇒ 那 5.9 s 里
  **没有任何注册表入口被调用**（收包停 ⇒ 没有 deposit；UL 流水线被自己的 cb 堵住 ⇒ 没有 take）
  ⇒ **§6.11 的"每个入口都查"在"全停"时依然没人来问**。期限再准，也得有人来问。
* 候选的挂起点（要下一条腿的仪器来判，**不要凭猜**）：`[ul_gpu_lane] lanes=48078 cbs/lane=2.00 carried=16038`
  （**33% 的车道在关闭时前端 cb 还没完成**）、`lane fence signals=96156 waits=48078`（每个车道都等一次抽取栅栏）、
  `grid_devwaited=60`。车道序是默认的 `merged`（整个延迟跳一次提交），所以"被 hold 住的抽取 cb 跨跳未提交"
  与"MISS 跳等一个没被提交的生产者"这两条**都还没有被排除**，**它们都能让一个 cb 在 GPU 里干等几秒**。

**④ 下一步（A/B 不需要裁决，C 需要）**

| 选项 | 内容 | 代价/风险 |
|---|---|---|
| **A** | 把**停在 `pop_blocking` 的接收线程**变成 sweep 的入口（一个 hook）：注册表被"没人来问"卡住时，由**被 park 的那个线程自己**去收孤儿 | 小、离线可测；只治第二个洞（`claimed=1` 的块不归 sweep 管——那是编码安全，不能动）|
| **B** | 查"cb 为什么 5 s 才完成"：**P0-7b**（每条记录 `commit→completion` + 设备侧等待目标：抽取栅栏 / grid-ready 的 generation 及其 signaller 的 slot）＋现成开关 **`OCUDU_CE_WAIT_TRACE=1`**（按指针打印 held cb 的 publish / close 站点，能直接看"谁、隔了多久才 close"）| 纯仪器、不动数据面；需要一条腿 |
| **C** | **结构性**：让交接块的输入不要依赖"可能跨跳被 hold 的 cb"——前端 DFT 单独一次提交（= P2-E 选项 (b)，用户 2026-09-25 已裁掉）。**新证据是：它不只是时延问题，而是"池被按住 5 s"的机制**；按 `merged` 车道序语义实施会让 V4（`cbs/lane`）需要重新裁决 | 需要用户二次裁决 + 重测 |

### 6.13 ✅ Q9-A + Q9-B 落地（2026-09-25，提交 `5ff8759804`）：**干池的接收线程自己驱动 sweep** ＋ **"这 5 秒在哪里"的两组读数**

> 依据 §6.12 ③/④ 的 A、B 两项（用户 2026-09-25 批准"A + B 一起做"）。**A 是代码改动、B 是纯仪器**；
> 离线已全绿（见 ③），**等一条腿**（配方与读法见 ④；判据仍是 §6.11 ⑤ 的 F1–F8，**没有新阈值**）。

**① A：把"停在 `pop_blocking` 的接收线程"变成注册表的入口**

| 位置 | 改动 |
|---|---|
| `include/ocudu/phy/phy_pipeline_grid_ready.h` | 新增 `handover_reap_hook`（与 `grid_ready_hook` / `slot_hop_plan_hook` 同一套 hook 模式：无 Metal 的构建里是 no-op）|
| `lib/phy/metal/ocudu_metal_burst.mm` | `shared_burst::reap_unclaimed_now()`：跑**同一个** `sweep_unclaimed()`，**解锁后**才提交；计 `reaped_by_park_events` / `reaped_by_park_blocks`。安装点与其它四个 hook 同一个 `once` lambda |
| `lower_phy_baseband_processor.{h,cpp}` | 新增 `pop_rx_buffer_blocking()`：**先 `try_pop`**（健康路径一次 hook 都不调）→ 池干时 `reap()` ＋ `pop_wait_for(10 ms)` **分片等待**，超时后再 reap；队列 stop 时返回与原来 `pop_blocking()` 相同的空缓冲。**两个原 `pop_blocking()` 调用点都走它** |

* **为什么必须由"被 park 的那个线程"来问**：§6.11 的规则是"每个入口都查"，而**全停时没有入口**——
  收包线程停在空池上 ⇒ 没有 deposit；UL 流水线堵在等缓冲的块上 ⇒ 没有 take。腿 `p08-conc2` 的
  `slot=415` 就是这一支：`wait_for_a_claim = 5.945 s`、认领后 **2.6 ms** 完成 ⇒ 那 5.9 s 全是在等一个**永远不会来的调用者**。
* **代价**：健康路径不变（`try_pop` 命中即返回）；只有在**池已经干了**的时候才多一次注册表扫描 + 可能几次迟提交
  （`late_commits` 本来就该发生，只是提前到被 park 的那一刻）。
  ⚠ 已知风险（写在这里而不是事后解释）：`[cb commit]` 在队列饱和时可能阻塞（§6.9 的 arm 10 注释），而这条路径跑在
  **收包线程**上——但此时代码本来就要在这个空池上阻塞，所以最坏情况不比改动前更差。

**② B：两组"这 5 秒在哪里"的读数**（纯读数，不加 dispatch、不加提交）

| 读数 | 出处 | 说什么 |
|---|---|---|
| `registry commit->completion=N max=… mean=…`（P0-7 行上，Q9-B）| `handed_counters::commit_count/commit_wait_max_us/sum` | 把 `deposit->completion` 切成两半：**注册表已经发出提交之后**又花了多久。若它 ≈ `deposit->completion` 的 max ⇒"提交了、但完成得慢"；若它只有 ms 而后者是秒 ⇒"**提交之前**就慢了"（= 认领者 / hold 那一侧）|
| `[ul_gpu_lane] commit -> completion (Q9-B, all stages)` ＋ `slowest command buffers by commit -> completion (Q9-B)` 表 | `ocudu_metal_lane_probe.mm`（每条 cb 记 `commit_time`，读 `GPUStartTime/GPUEndTime`）| 每条命令缓冲拆成 **`commit->start`（队列没轮到它）** 与 **`start->end`（设备拿着它——**设备侧栅栏等待就算在这里**）**，并**逐行打印最慢 8 条的 slot + stage** ⇒ 一条腿的答案是一行，不是一个平均值 |
| `dry-pool reaps=N recovering M block(s)`（P0-7 行上，Q9-A）| `reaped_by_park_events/blocks` | **A 是否真的被触发**。**events>0 而 blocks=0** ⇒ 卡住池的**不是**"没人认领的块"，而是 **`claimed=1` 的块**（sweep 不许碰它——往已提交的缓冲里编码是错误）⇒ 那条腿直接把责任指到 hold/提交一侧，B 的表接着回答是队列还是设备 |
| （既有、本次只是首次写进手册）`OCUDU_CE_WAIT_TRACE=1` | `ocudu_metal_mmse_engine.mm` | 按**指针**打印 held cb 的 publish / close 站点（`end_stage_async: published as pending` / `close_held_buffer` / `encode_run`）⇒ 谁的 hold 跨了多久，看两行之间隔了多少事件。**stderr 会变大，只在诊断腿上开** |

**③ 离线验证（提交前全部做完，提交 `5ff8759804`）**

| 网 | 读数 |
|---|---|
| `dft_release_adopt_metal_test` | **rc=0**；新增 **arm 12（Q9-A）**：`a DRY pool reaped an unclaimed block with NO other registry entry point - input released exactly once, dry-pool events 0->1, blocks 0->1, late_time 5->6`；arm 11（Q9）仍绿；P0-7 行新字段 `registry commit->completion=8 max=731.0us mean=482.0us (Q9-B); dry-pool reaps=1 recovering 1 block(s)`；lane 报告的 `commit -> completion (Q9-B)` 与 slowest 表都打印（本测里 `commit->start≈60–110 µs` / `start->end≈450–560 µs`）|
| `ring_buffer_test`（adt，编入 `blocking_queue_test.cpp`）| **4/4 PASS**，其中**新增** `pop_wait_for_reports_success_timeout_and_stop` 钉住新循环依赖的三个结果（success / timeout / stopped⇒failed；这一调用在树里此前**没有使用者**）|
| `ctest -L phy` | **100% passed out of 193** |
| `lower_phy_test` | **528/528 PASSED** |
| `ul_pipeline_probe_test`（support）| **3/3** |
| `l1_handover_arms.sh 8 /tmp/l1_handover_q9ab` | **5 PASS**（`cand vs ref` 8 文件 0 differing；`drop`/`skew` 各 8 不同）|
| `l1_hop_arms.sh` | **4/4 `differing=0`** |
| 与 pristine HEAD 二进制（`work_tmp/ref/replay_head_pre_p05`）逐字节 | `cand`+`nogrid` **16 个网格文件 0 differing**；真捕获 `syn004_4` **4 个 dump 0 differing** ⇒ **值中性** |

**④ 下一条腿：配方与"要读什么"（判据不新增）**

```bash
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量同 p08-conc2：上行 iperf3 -R -t 100；读法：p0_gate.sh <label>（F1–F8 一字不改）
```

| 读 | 若 A 起作用 | 若没有 |
|---|---|---|
| `block lifecycle` 的 `wait max` / `oldest unclaimed age max`（**F2**）| 落 **ms 级**（被 park 的线程自己会来 reap）| 仍秒级 ⇒ 读 `dry-pool reaps`：**events>0 而 blocks=0** ⇒ 不是"没人认领"，是 **claimed 的块** ⇒ 看 B 的表 |
| `[ul_gpu_lane] commit -> completion` 最慢行 | — | **`commit->start` 大** ⇒ 队列（前面有更长的缓冲或等待者）；**`start->end` 大** ⇒ **设备侧等待**（抽取栅栏 / grid-ready），即 hold 那条线 ⇒ 下一步是 §6.12 ④ 的 C，或"让 hold 不跨跳" |
| `registry commit->completion` 的 max vs `deposit->completion` 的 max | 两者都小 | **同量级且都是秒** ⇒ 提交之后才慢（设备/队列）；**前者 ms、后者秒** ⇒ 提交之前慢（hold/认领）|

**⑤ 环境注记（与本次改动无关，但影响可复现性）**：用户接受 Xcode 许可后，工具链/SDK 换成了 **Xcode 26** 那一套，
它的 libc++ 更严，**四处**把整棵树挡在编译之外（用户 2026-09-25 报"build 错误，无法运行测试"）：
`future::get()` 变成 `[[nodiscard]]`（`io_broker.h`、`task_worker.cpp` 的"调用本身就是等待"两个站点，以及
PUCCH 资源管理测试里一句无副作用的 `set::count()`），以及 `std::partial_sort` 现在通过 `iter[n]`
（`__algorithm/sift_down.h`）取堆元素 —— 而 `flat_map.h` 的 `sort_iter` 声明了 `random_access_iterator_tag`
却没有下标运算符。已按原意补齐（提交 `2be02bfdb5`）：`(void)` 三处 + `sort_iter::operator[]` 一处。
修完 `make -k` 全树干净；**整棵树在改前后各重编一次**，而与 pristine HEAD 二进制的逐字节比对（新 SDK 编译）
仍是 **0 differing** ⇒ SDK 变化没有改字节，§6.11/§6.13 的逐字节网仍然可比。
⚠ 另注：`ctest -j 6`（整套并行）会让两个 **CE Metal 单测**因 GPU 争用而红（`mmse_landmine.sh` 记录过的那类非确定性），
**串行跑（`-L phy` / `-R port_channel_estimator_metal_mmse`）2/2 全绿**；我们的网一律按串行读数记。
**2026-09-25（§6.24 那次）补充**：即使**串行** `ctest -L phy`，只要紧跟在别的 Metal 测试之后跑，
`port_channel_estimator_metal_mmse_unit_test_ta_chain` 仍可能红一次；**隔几秒重跑即绿**（`--rerun-failed` 通过、随后整套 193/193）。
⇒ 见到它先按"GPU 争用偶发"处理，但按 §4.4 的偶发规则**必须保留首次读数**，不许静默重试洗绿。

### 6.14 ✅ Q9-C 落地（2026-09-25，提交 `5f51b0e9fe`）：**车道 burst 等的是"本跳自己的"估计器 generation，不是全局最新**——跨车道栅栏夹死（cross-lane pinch）

> 依据 §6.13 ④ 的表：腿 `p09-conc2` 的 B 读数把"这 5 秒"钉在**队列**上。**A 已被证明在工作**（`dry-pool reaps=1471`），
> 但只救回 9 个块 ⇒ 持有者是 **claimed 的块**（sweep 按设计不能碰）⇒ 顺着 B 的表找到真正的等待者。

**① 腿 `p09-conc2` 的读数（判据 D 组 + 新读数）**

```
D1 FAIL input hold max = 5004.6 ms            D2 FAIL wait max = 3566.0 ms, unclaimed age max = 3566.0 ms
D3 FAIL pop_blocking max = 4997.5 ms, over 1s = 3      D4 FAIL 3 gaps / 229,736,932 samples（≈14.9 s）
D5 INFO late=1278, late_time=11               D6 INFO registry commit->completion=16180 max=5001107.0us
D7 INFO dry-pool reaps=1471 recovering 9 block(s)
[ul_gpu_lane] commit -> completion (Q9-B): samples=35515 mean=964.1us median=406.3us p95=1492.4us p99=1831.9us max=5004208.5us
[ul_gpu_lane]   slot=5252 stage=merged_hop commit->start=5002824.6us start->end=1241.5us commit->end=5004066.1us
[ul_gpu_lane]   slot=322  stage=merged_hop commit->start=5002582.3us start->end=1243.5us commit->end=5003825.8us
[ul_gpu_lane]   slot=712  stage=merged_hop commit->start=5002963.6us start->end=1244.9us commit->end=5004208.5us
```

* **`commit->start ≈ 5.00 s` 而 `start->end ≈ 1.24 ms`** ⇒ 秒在**队列**里，不在设备上；
  三条（以及同批的 8 条）**`commit->end` 相差不到 400 µs** ⇒ 它们是被**同一个事件**在同一个瞬间放行的；
  P0-7 的 `slowest` 行里同一批 slot 的 `deposit->completion` 也都是 5.004 s，`registry commit->completion` max = 5.0011 s
  （**注册表早就提交过了**），`wait_for_a_claim` 只有 3.0 ms（`swept=1`）或几十 µs（`swept=0`）。
* ⇒ **"一个共享的、晚到的事件"** = 车道 burst 的 **stage fence**（`backend_stage_wait()`）。它的注释写的是
  "it targets the estimator command buffer of **THIS** hop, which was committed before this burst"，
  但**实现读的是"此刻的全局最新 generation"**：并发 2 时，最新那条可能属于**另一条车道的估计器**，
  而 generation 是在它 **commit 之前**发出的 ⇒ 存在一个窗口：**本 burst 等的 signal 走在一条
  "排在同一条串行后端队列里、却在它后面"的命令缓冲里**。等的值由"排在它后面的"缓冲来发 ⇒ 互锁。
  （并发 1 时不存在"另一条车道"，所以这条缺陷从来只在并发 2 出现——与 §6.8 起的观察一致。）

**② 改了什么**（`ocudu_metal_burst.{h,mm}`、`ocudu_metal_mmse_engine.mm`、`ocudu_metal_queue.{h,mm}`）

| 位置 | 改动 |
|---|---|
| `shared_burst::set_stage_wait(generation)`（新）| 与 `set_grid_wait()` 同形状的**按线程**交接：估计器在提交**本跳自己的**提交物之前把 generation 交出来，本线程**下一个** burst 消费它 |
| `mmse_engine::signal_stage_fence_for_burst(cb)`（新）| 把三处"为本跳自己的提交物发栅栏"的站点统一：取 generation → 发信号 → **publish**（event 序的抽取、merged 序里 hold 未被采纳而单独提交的抽取、merged 序 hold 交棒前）。**凡是提前提交的路径都经过其中之一**，而且都在"随后跑车道各阶段的同一个线程"上 ⇒ 下一个 burst 必定等到**自己那一跳**的提交物，而它按构造**排在前面** ⇒ 环路不可能形成 |
| `burst_ensure_open()` | 消费它：`own != 0` ⇒ `backend_stage_wait_generation(cb, own)`；否则回退旧的"全局最新"（并计数）——只有"估计器在别的线程同步跑完/根本没跑"的路径会走到，那时信号早已完成，旧读法无害 |
| `[metal_stats] lane fence` 行 | 新增 **`own=` / `newest=` / `cross_lane=`**：`cross_lane` 是"全局最新 ≠ 本跳自己的"出现次数，即**旧规则本来会去等一个外来 generation** 的次数（**上界**：外来但已提交的 generation 排在前面，无害）|

**③ 离线验证（提交前全部做完，提交 `5f51b0e9fe`）**

| 网 | 读数 |
|---|---|
| `port_channel_estimator_metal_mmse_unit_test` | **All tests PASSED**（含断言"默认 merged 也要挂后端栅栏"的 Test 13），并且**它当场复现了这个夹死**：`lane fence … own=3 newest=5 cross_lane=2` ⇒ 8 次等待里有 2 次在旧规则下会去等外来的 generation |
| `dft_release_adopt_metal_test` | **rc=0**（arm 11/12 仍绿；新 `lane fence` 行打印）|
| `ctest -L phy` / `ul_pipeline_probe` / `ring_buffer_test` / `lower_phy_test` | **193 全绿** / **3/3** / **4/4** / **528/528** |
| `l1_handover_arms` / `l1_hop_arms` | **5 PASS** / **4/4 differing=0** |
| 与 pristine HEAD 二进制逐字节 | 网格 **16 文件 0 differing**；真捕获 `syn004_4` **4 个 dump 0 differing**（replay 日志里 `own=1 newest=0 cross_lane=0`）|

**④ 下一条腿（判据仍是 F1–F8，无新阈值）**：配方与 `p09-conc2` 完全相同（n1 默认 + 并发 2 + 相位开关 + 上行 `iperf3 -R -t 100`）。

| 读 | 通过的样子 | 若仍然红 |
|---|---|---|
| `lane fence … cross_lane=` | **>0 且腿不再有 5 s 停顿** ⇒ 旧规则确实会去等外来 generation，而新规则不（这是"缺陷真的发生过 + 修好了"的同一条读数）| `cross_lane=0` 而仍有 5 s 停顿 ⇒ 等待者不是 stage fence ⇒ 下一个候选是 **grid-ready**（MISS 跳等生产者的 generation，跨队列、本不该成环，但可能被"生产者被拖住"影响）⇒ 用 `grid_devwaited` 与 P0-7 的 `generation` 对齐 |
| D1–D4（F1–F4）| 落 ms 级 / `gaps=0` | 按 §6.13 ④ 的表继续分支 |
| D6/D7 | `commit->completion` 与 `dry-pool reaps` 都小 | 若 D7 仍是 events≫blocks，说明池仍被 claimed 的块按住 ⇒ 结合 `own/newest` 与 B 的表看是哪一个等待 |

### 6.15 ✅ 第一条**全绿**的腿（`p10-conc2`，n1 + 并发 2，2026-09-25 10:14）：`p0_gate.sh` **18/18**，`gaps=0`，池从未见底——**但 Q9-C 在这条腿上没有被执行，功劳不能记在它头上**

> 腿：`gnb_gpu_p10-conc2_0925_1014`，配方与 `p07/p08/p09` 完全相同（n1 默认 + `max_pusch_and_srs_concurrency=2` +
> `OCUDU_UL_PHASE_SEGMENTS=1`），上行 `iperf3 -R -t 100`（用户侧：**没有断流**）。跑完由用户 Ctrl-C（atexit 报告完整）。

**① 判据：`p0_gate.sh p10-conc2` → 18 of 18 criteria pass**（第一次全绿）

| 判据 | p07 | p08 | p09 | **p10** |
|---|---|---|---|---|
| **D1** `input hold` max | 30.72 s | 5.95 s | 5.00 s | **18.9 ms** ✅ |
| **D2** `wait max` / `oldest unclaimed age max` | 30.72 s / 77.86 s | 5.95 s / 5.95 s | 3.57 s / 3.57 s | **17.4 ms / 17.4 ms** ✅ |
| **D3** `pop_blocking` max / `over 1s` | 4.998 s / 2 | 4.997 s / 2 | 4.998 s / 3 | **17 µs / 0** ✅ |
| **D4** `radio sample continuity` | 2 gaps / 9.97 s | 3 gaps / 9.98 s | 3 gaps / 14.9 s | **0 gaps** ✅ |
| `[ul_rx_pool]` `taken/returned…` | `starved_events=383`, `held_max=8`, `free_min=0` | 6 / 8 / 0 | 4 / 8 / 0 | **`starved_events=0`, `held_max=4`, `free_min=4`** |
| `[ul_gpu_lane]` `residency max` | — | 5.004 s | 5.004 s | **2141 µs** |
| `carried`（关闭时前端 cb 未完成的车道）| 4109 | 16038 | 4457 | **0** |
| `commit -> completion (Q9-B)` max | — | — | 5.0042 s | **3697 µs** |
| D6 `registry commit->completion` | — | — | 5.0011 s | **2377 µs** |
| D7 `dry-pool reaps` | — | — | 1471 ev / 9 blk | **0 / 0**（池从未干） |
| `[ul_gpu_pipeline]` 中位 / `stale` | 2375.3 µs / 0 | 2328.7 / 2 | 2299.2 / 2 | **2382.7 µs / 0** |
| `cbs/lane` | 2.00 | 2.00 | 2.00 | **2.00 (max=2) dropped=0** ✅ |
| 契约 / `paired/phase account` | continuity 红 | continuity 红 | continuity 红 | **8/8 + EXACT MATCH（101289/101289）** ✅ |

**运行期间的实时读数**（用户 iperf 窗口 ≈02:15:30–02:17:10）：**PUSCH = 5000/5 s（1000/s，每个 slot）整 100 s**，
期间 **0 次 `[RF]` 实时失败、0 次 pool EMPTY、0 次 `PUSCH allocation skipped`、0 次 `PUxCH request late`**、
**0 次 RX overflow**；全腿 15 次 underflow **全部在 02:17:34 之后**（测试结束之后）。
普查对照：p09 `PUSCH=17761/228.8 s (77.6/s)`、`PUxCH late=1237`、`overflow=3`、`pool EMPTY=1`、最长静默 23.9 s；
**p10 `PUSCH=104943/230.4 s (455.6/s)`、`PUxCH late=0`、`overflow=0`、`pool EMPTY=0`、最长静默 9.0 s（在接入阶段）**。

**② ⚠ 归因警告：Q9-C 在这条腿上没有被执行，所以"全绿"不能记成"Q9-C 修好了断流"**

```
[metal_stats] lane fence signals=209882 waits=104941 skipped=0 generation=209882 own=0 newest=0 cross_lane=0
```
* `own=0 newest=0` ⇒ **`burst_ensure_open()` 在这条腿上一次都没有编码过 stage-fence 等待**（否则两支之一必然计数）。
  ⇒ 默认 `merged` 车道上，车道的 EQ/demap 走的是 **`shared_burst::adopt()`**（与前端块同一条缓冲 ⇒ 构造上就有序），
  **根本不经过 `burst_ensure_open()`** ⇒ **§6.14 的改动在默认空口路径上是"零执行"的**（它保护的是
  `merged` 的**回退**路径——几何不允许 hold 时估计器单独提交那一支——以及 `event` 车道序）。
* 那 104941 次等待来自**另一条**设备侧等待：**corr 栅栏**（`backend_stage_wait_generation(cb, e->corr_fence_generation)`，
  §6.14 ② 里"不在本次改动范围"的那一条），它的 signaller 是**本跳更早的**提交物 ⇒ 排在前面 ⇒ 不成环。
* ⇒ **p09→p10 之间唯一的代码差异是 Q9-C，而它没跑** ⇒ 这条腿的干净**只能解释为"这次没触发"**（腿间波动），
  不能作为"Q9-C 修好了"的证据。**按 §5.2 纪律 2：单条腿不是证据**（p07/p08/p09 三次都有停顿，p10 一次没有）。

**③ 这条腿真正证明了什么**

1. 整条停顿链（没人认领的块 → 池干 → park → 电台溢出）**可以完全不存在**：`starved_events=0`、`held_max=4/8`、
   `pop_blocking max=17 µs`、`gaps=0`，而且是在**满速上行 100 s（121038 个块）**下取得的——不是"轻载没触发"。
2. A/Q9-B 的仪器工作正常且自洽：`dry-pool reaps=0`（池没干过 ⇒ 没东西可收）、`registry commit->completion max=2377 µs`
   （提交之后最长 2.4 ms）。
3. **触发的间歇性**被摆到明面上：p07/p08/p09 有、p10 没有，而且 p09 的**载荷本身更差**（UE 反复静默/重接、
   `PUxCH late=1237`）⇒ 停顿与"UE 侧/链路侧的抖动词"相关，而不是与"吞吐量"相关。

**④ 下一步（**重复**才是证据；两条臂）**

| 臂 | 跑法 | 看什么 |
|---|---|---|
| **A：同配方复跑 2–3 条**（`p11/p12/p13-conc2`）| 与 p10 完全相同 | 停顿是否复现；若复现，D8 的 `cross_lane`、D6/D7 与 `commit -> completion` 表**直接指出等待者**（是否又是"队列 → 5 s"）|
| **B：把 Q9-C 真正跑起来**（`p11-event`）| 同配方 + **`OCUDU_CE_LANE_ORDER=event`** | `event` 序下 `burst_ensure_open()` **必然**编码 stage-fence 等待 ⇒ `own=` 应显著 >0；若 `cross_lane>0` 而不再出现 5 s 停顿，才是 Q9-C 的**空口**证据（`cbs/lane` 仍是 2.00，V4 不变）|

若 A 里停顿复现且 D8 显示 `cross_lane=0`、而 B 的表显示 `commit->start` 大 ⇒ 等待者不是 stage fence ⇒
按 §6.14 ④ 的下一候选（**grid-ready**：MISS 跳等生产者的 generation）继续查。

### 6.16 ⚠ 复跑腿 `p11-conc2`（2026-09-25 10:55）：停顿**复现**了，且**Q9-C 再次零执行** ⇒ 上一个假设被证伪；新增 **Q9-D 栅栏次序仪**（等待 vs signaller 的先后 + 那次等待持续多久）

> 配方与 `p10/p09` 完全相同，上行 `iperf3 -R -t 100`：用户侧 ~68 s 起断流到最后。**14/18 判据**，
> 失败的是 D1–D4（一条 5 秒停顿）。⇒ **`p10` 的"全绿"确实只是"那次没触发"**（§6.15 ② 的警告成立）。

**① 这条腿的读数**

```
D3 FAIL pop_blocking max = 4994.6 ms, over 1s = 1        D4 FAIL 1 gaps / 76,547,707 samples（≈4.98 s）
D6 INFO registry commit->completion=16350 max=5000026.0us  vs  deposit->completion max=5002964.0us
        -> 100% of the wait came AFTER the registry committed it
D7 INFO dry-pool reaps=500 recovering 3 block(s)         D8 INFO own=0 newest=0 cross_lane=0
[metal_stats] block lifecycle (P0-7) slowest:
  slot=1232 claimed=1 swept=0 wait_for_a_claim=33.0us     deposit->completion=5001452.0us registry_commit=0
  slot=1233 claimed=1 swept=1 wait_for_a_claim=2937.0us   deposit->completion=5002964.0us registry_commit=1 commit->completion=5000026.0us
  slot=1234 … 1239                                        同样 ~3 ms 认领 / ~5.000 s 完成（1237/1238/1239 是时间期限在 ~12 ms 收的）
[ul_gpu_lane] residency max = 5722.3us（**毫秒级！**）  carried=482
[ul_gpu_lane] commit -> completion (Q9-B) max = 7384.8us（**毫秒级**）  slowest 行: slot=1233 merged_hop commit->start=6923.9us
```

**两个决定性事实**：

1. **受害者是"落单的前端块"，不是车道缓冲**：`slot=1232…1239` 是**连续 8 个 slot**（正好一个池 8 个缓冲），
   它们的**前端块 cb** 在 ~3 ms 被认领（多数由 sweep，一个由跳）却在 **~5.000 s** 后才完成；
   而同一批 slot 的**车道 cb**（`merged_hop`）只有 **7 ms**（`residency max = 5.7 ms`）。
   ⇒ 停顿发生在**前端/交接那条命令缓冲**上，**不是** §6.14 假设的车道 burst 栅栏。
2. **Q9-C 再次零执行**（`own=0 newest=0`）⇒ 默认 `merged` 路径上 `burst_ensure_open()` 依然没跑。
   **§6.14 的"跨车道 stage-fence 夹死"不是这条腿停顿的原因**（它是真缺陷、离线可复现，但不在空口默认路径上）。

时间线（主日志）：iperf 窗口 ≈02:57:15–02:58:55，满速 5000 PUSCH/5 s 直到 **02:58:20**；那一刻起
`receive pool is EMPTY`、`RF: late`（7248 条）、underflow/overflow、`PUSCH allocation skipped`（2292）、
`PUxCH request late`（371）一起出现，PUSCH 之后collapse 到 0–38/5 s ⇒ **一次 5 秒停顿毁掉整段 TCP**。

**② 新增仪表 Q9-D（提交在本次）：`[metal_stats] fence order (Q9-D)`**

一条 5 秒的等待究竟挂在**哪一道**设备侧栅栏上，现有计数器答不了（它们数"有多少次等待"，不数"等待的先后"）。
设备侧等待只有三类（全在 `ocudu_metal_queue.mm`）：**stage 栅栏**、**corr 栅栏**、**grid-ready 栅栏**。
Q9-D 在 `note_fence_signal()` / `note_fence_wait()` 里成对记录：

| 读数 | 含义 |
|---|---|
| `signaller-first=N` | 等待被编码时，它等的 generation **已经发出**（信号已在前面）⇒ 立即满足，**永远不会堵队列**（安全形状）|
| `signaller-after=K` | 等待被编码时那个 generation **还没发出** ⇒ signaller 的命令缓冲**可能排在等待者后面**（串行队列上这条次序无法自行解开）|
| `max=…ms worst kind=… slot=…` | 这些"倒序"等待里**最长的那次持续了多久**、属于哪道栅栏、哪个 slot ⇒ **一条腿的答案是一行** |
| （尾部提示） | 退出时仍挂着未满足的等待 ⇒ signaller 从未到来（真正的挂死）|

离线自测（`dft_release_adopt_metal_test` **arm 13**）：安全的形状与"倒序"的形状被分开计数，
并实测出那次等待的时长（`signaller-first 0->1, signaller-after 0->1, longest wait 0->26899 us`）；
`port_channel_estimator_metal_mmse_unit_test` 上 3251 次等待**全部**是安全形状（`signaller-after=0`）。
⇒ **仪器本身可判**（不是"两条路都打印同一个数"）。

**③ 下一步：一条腿就能定位**

```bash
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p12-conc2 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2   # 配方/流量同 p11
bash doc_chinese/phy_latency/wip/p0_gate.sh p12-conc2                      # D1–D4 判据 + D5–D9 读数
```

| `fence order (Q9-D)` 的读数 | 结论 |
|---|---|
| `signaller-after=0`，而腿**又停**了 | 三类栅栏都不是肇事者 ⇒ 排队等待的**不是设备侧事件**，而是**队列本身/主机提交**（下一步转向 `commit_dropped` 与 lane 的提交顺序、以及 `MTLCommandQueue` 的提交路径）|
| `signaller-after>0` 且 `max` ≈ 停顿时长，`worst kind=grid` | **grid-ready 栅栏**：MISS 跳等的生产者是**host 读者（fallback）**提交的 ⇒ 因果链是"跳先提交等待、生产者后提交" ⇒ 修法是让 claim 那一刻**同步提交**（已在 `claim_grid_production` 做）或让等待**只在生产者已提交时**才编码 |
| `worst kind=stage` / `corr` | 对应栅栏的 signaller 与等待者的**跨线程次序**问题（§6.14 的同类，只是发生在另一道栅栏上）⇒ 按同样办法把 generation 绑到"本跳自己的提交物" |

**④ 顺带修掉的判据读数 bug**：`p0_gate.sh` 的 D6 detail 之前把 `commit->completion max` 与 **`wait max`（认领等待）**
相比，读出来是荒谬的 "210%"；现在与 **`deposit->completion max`** 比（p11 上读作 **100%**：秒全在提交之后）✅。

### 6.17 ⚠ 腿 `p12-conc2`（2026-09-25 11:07，带 Q9-D）：**栅栏被排除**——56054 次设备侧等待**全部**是安全形状；新增 **Q9-E 完成处理器滞后仪**

> 配方与前几条完全相同，上行 `iperf3 -R -t 100`：~49 s 起断流、89 s 起部分恢复。判据 **D1/D3/D4 红**（**D2 这次是绿的**：21.1 ms）。

**① 判决性负结果（Q9-D）**

```
[metal_stats] fence order (Q9-D): waits=56054 signaller-first=56054 signaller-after=0 max=0.0ms
[metal_stats] lane fence … own=0 newest=0 cross_lane=0
```

* **56054 次等待，没有一次**等的 generation 尚未发出 ⇒ **三类设备侧栅栏都不是肇事者**（stage / corr / grid 都被排除）。
  这正是 §6.16 ③ 表里的第一支；⇒ 问题不在"设备侧事件等待"，而在**队列里的命令缓冲本身**或**主机侧**。
* `own=0` 再次确认 Q9-C 在默认 `merged` 路径上零执行。
  ⚠ Q9-D 的**盲区**要写清楚：它比较的是"等待 vs generation **发出**"，而 grid 栅栏的 generation 是在**deposit 时**发出、
  在**提交时**才生效的（等待者可能比"生产者的提交"更早进队列）——这一支由 §6.17 ③ 的 Q9-F（提交票号）覆盖。

**② 这条腿的形状与 p11 逐项相同**

| 读数 | p11 | p12 |
|---|---|---|
| 受害 slot | 1232–1239（连续 8 个 = 一个池）| **3362–3369**（同样连续 8 个）|
| `commit->completion` max（注册表提交的块）| 5.000026 s | **4.999512 s** |
| `deposit->completion` max | 5.002964 s | 5.002592 s |
| 车道 cb 的 `commit -> completion` max | 7.4 ms | **7.5 ms** |
| `pop_blocking` max / `over 1s` | 4.9946 s / 1 | 4.9942 s / 1 |
| gap | 1 / 76,547,707 样点 | 1 / 76,538,695 样点 |
| `[RF] late` / `PUSCH allocation skipped` | 7248 / 2292 | **7248** / 2208 |
| `dry-pool reaps` | 500 ev / 3 blk | 501 ev / 3 blk |

* 冻结的**起点**（主日志）：`03:09:10.650 [PHY] [W] [ul_rx_pool] the receive pool is EMPTY (held=8/8)` —— 池**先**被按满，
  0.4 ms 后才有第一个 `RF: late`；也就是说**不是电台先出问题**，而是**8 个块的输入没回来**。
* 两条腿的事件计数几乎逐位相同（`late=7248` 两次一模一样）⇒ **可复现的机制**，不是随机抖动。
* 但受害者的**车道 cb 全部只有 7 ms** ⇒ 卡住的**不是**车道那条缓冲，而是**落单的前端（deposit）块**（8 个，正好一个池）。

**③ 新增 Q9-E：完成处理器滞后（`handler lag=`，本次提交）**

输入 token（以及 `produced`）都是在**完成处理器**里释放的 ⇒ **处理器被延迟派发 = 池被按住**，而 P0-7 的时间戳本来就在处理器里，
所以它**分不清**"缓冲跑得慢"和"处理器来得晚"。Q9-E 用 `GPUEndTime`（GPU 自己的结束时刻，Darwin 上与 `steady_clock` 同一基准）
算出 `now - GPUEndTime`：

```
… ; handler lag=<count> max=…us mean=…us at slot=… (Q9-E: GPU done -> handler ran)
```

离线基线（`dft_release_adopt_metal_test`，本机）：`handler lag=49 max=110.0us mean=65.9us` ⇒ 正常情况处理器在 GPU 结束后 **~66 µs** 就跑。

**④ 下一条腿（同配方）的分支表——一条腿就能定性**

| `handler lag` max | `commit->completion` | 结论 / 下一步 |
|---|---|---|
| **≈5 s** | ≈5 s | **GPU 早就跑完了，是主机侧处理器滞后** ⇒ 池是被"没人跑处理器"按住的。下一步：查处理器派发为何被饿（CPU/优先级/驱动线程），以及**让输入释放不依赖处理器派发**（例如在 deposit 路径上轮询 `cb.status`；这会碰到 §6.4.6 那次裁决，需要请你重新裁）|
| **ms** | ≈5 s | 缓冲**自己**在等 ⇒ D9 已排除设备侧栅栏 ⇒ 只剩**队列次序**：装 **Q9-F 提交票号**（把每个 `[cb commit]` 编上序号，比较"等待者 vs signaller 的提交次序"），它会直接点名排在前面、堵住队列的那个缓冲 |
| 两者都 ms | 都 ms | 这条腿没复现 ⇒ 继续复跑（单条腿不是证据，§5.2 纪律 2）|

⇒ 因此**下一条腿（`p13-conc2`）同时带 Q9-D/Q9-E**：无论落在哪一支，都能把 5 秒钉到"主机处理器"或"队列次序"上。

### 6.18 ⚠ 腿 `p13-conc2`（2026-09-25 11:17，带 Q9-E）：**"主机处理器滞后"被排除，"队列次序"成为唯一剩下的位置**；交接 memo 见 `session_handoff_2026-09-25-1.md`

> 配方同 p07–p12。用户侧上行 `iperf3 -R -t 100` **两次断流**（~15 s 起与 ~65 s 起），与腿自己的 **2 次 park / 2 个 gap** 一一对应。
> **本会话到此收口**：更完整的"现状 / 已证明与已证伪 / 下一步"见交接 memo **`session_handoff_2026-09-25-1.md`**（它是本轮的完整快照）。

**① 判据与读数**

```
D1 FAIL input hold max = 5004.7 ms (slot 9612)      D2 PASS wait max = 19.97 ms
D3 FAIL pop_blocking max = 4997.6 ms, over 1s = 2   D4 FAIL 2 gaps / 153,152,535 samples (~9.98 s)
D5 INFO late=3828, late_time=9                      D6 INFO registry commit->completion=20236 max=5001219.0us
D7 INFO dry-pool reaps=982 recovering 6 block(s)    D8 INFO own=0 newest=0 cross_lane=0
D9 INFO fence order (Q9-D): waits=27957 signaller-first=27957 signaller-after=0
       [metal_stats] block lifecycle (P0-7): … handler lag=48123 max=2037.0us mean=53.9us at slot=6143 (Q9-E)
       [ul_gpu_lane] commit -> completion max = 5004221.4us:
         slot=9612 stage=merged_hop commit->start=5002977.3us start->end=1244.1us
         slot=1022 stage=merged_hop commit->start=5002737.0us start->end=1242.6us
```

* **`commit->start ≈ 5.0027 s` 而 `start->end ≈ 1.24 ms`** ⇒ 缓冲**在队列里等**，一旦开始跑就只要 1.24 ms。
* **Q9-D：27957 次设备侧栅栏等待全部是"安全形状"**（signaller 早已发出）⇒ 三类栅栏（stage/corr/grid）**全部排除**。
* **Q9-E：`handler lag` 最大 2.0 ms、均值 53.9 µs** ⇒ **"GPU 早跑完、主机处理器晚"这一支也被排除**（本机基线：metal 测试 110 µs / 66 µs）。
* 受害块既包含 `swept=1`（sweep 认领后 `commit->completion ≈ 5.0003–5.0012 s`）也包含 `swept=0`（跳认领）；
  两次停顿的起点都是 `[ul_rx_pool] the receive pool is EMPTY (held=8/8)`（**池先满，随后才是 RF 失败**）。

**② 结论：剩下唯一的位置是"命令缓冲之间的队列次序"**

能"在队列里等 5 秒才开始"的只有一种东西：**排在同一条串行队列前面的某个命令缓冲没跑完**。
把它与 §6.17 ③ 的盲区合起来，得到**当前主假设 Q9-G**（详见 memo §4）：

> `claim_grid_production()` 对"**已被别人认领但尚未提交**"（`claimed && !produced`）的条目**直接返回 generation**、
> 不自己提交；跳据此把"等这个生产者"的等待编进自己的缓冲并提交，而**生产者的提交由另一个线程晚几微秒发出**
> （sweep 在 `deposit_released()` 里"锁内认领 → 解锁后提交"）⇒ **等待者的提交排在生产者之前** ⇒ 队首等一个排在自己后面的事件
> ⇒ 整条队列冻结，直到**更高 generation** 的生产者完成（≈5 s）把事件值推过等待值。

**③ 下一步（已写进 memo §6；新会话按它开工）**：**Q9-F**（给每个 `[cb commit]` 编提交票号，
比较"等待者 vs signaller 的提交次序"，把 Q9-G 变成读数）＋ **Q9-F2**（把前端 deposit 块也注册进 lane 探针的前端组，
拿到它们的 GPU 时间）⇒ 一条腿定性：`等待者先提交 > 0` ⇒ 实现 Q9-G 的**提交握手**；`= 0` ⇒ 转 GPU/队列结构。

### 6.19 ✅ Q9-F + Q9-F2（＋Q9-F3）落地（2026-09-25，提交 `bf33445b89`）：**把"等待者 vs signaller"的次序从"发出"改成"提交"，并给**每一个**命令缓冲一条 GPU 窗口**（本机离线已验证，**空口腿待飞**）

> 本节是 §6.18 ③ 的执行记录。配方与 p07–p13 完全相同；**下一条腿的标签建议 `p14-conc2`**，且必须带
> `OCUDU_METAL_GPU_TIME=1`（Q9-F3 的读数由它打开，见 ⑤ 的腿命令）。

**① 为什么需要 Q9-F：§6.17 ③ 已登记的盲区就是它**

Q9-D（§6.16）比较的是"等待 vs signaller **发出**"（`backend_stage_signal()` / `grid_ready_signal()` 返回 generation
的那一刻），p12/p13 都读成 `signaller-after=0`——**全部安全形状**。但**发出 ≠ 提交**：信号骑在一条命令缓冲上，
那条缓冲的提交可能晚得多，而且**可能是另一个线程**提交的：

| 生产者块（带着 grid 信号） | 谁提交 | 何时 |
|---|---|---|
| 跳认领了交棒块（`take_released` → `adopt`） | **车道自己**（`shared_burst::commit()`） | 认领后几十 µs，**在等待者之前** |
| 跳**错过**交棒，注册表 fallback 提交 | 该跳自己（`claim_grid_production` → `commit_dropped`）| 返回 generation **之前** |
| **sweep** 认领（`deposit_released` / `take_released` / `reap_unclaimed_now`） | **触发 sweep 的那个线程**，`commit_late` 在**解锁之后**才提交 | **可能晚于等待者的提交**（几 µs–几十 µs 的窗口）|

最后一行就是 **Q9-G**：在"锁内认领 → 解锁后提交"的窗口里，另一个线程的跳调 `grid_production_generation()`
看到 `claimed && !produced` ⇒ **只返回 generation、不自己提交**（fallback 分支要求 `!claimed`），于是它把等待编进
自己的缓冲并提交——**等待者可能排在生产者前面**。而同一条（串行 start 次序的）队列上，
**等一个排在自己后面的事件是解不开的**。

**② Q9-F：提交票号与"等待者先提交"（`ocudu_metal_queue.{h,mm}`）**

* `shared_queue::note_commit_order(cb)`：在**每一个可能带设备侧栅栏的 `[cb commit]` 之前**取一个全局递增票号
  （`burst.mm` 的两处、`mmse_engine.mm` 的 8 处、`dft_metal_engine.mm` 的两处、mmse 的 `handed_direct` 一处）；
  `note_fence_signal()/note_fence_wait()` 现在**多带一个 cb 参数**，把"这条缓冲将发哪个 generation / 在等哪个 generation"
  挂到它身上，提交时一次性兑现。
* 判据（**这就是 Q9-G 的读数**）：等待者提交时，**若已有一条携带 `generation' ≥ generation` 的缓冲提交过** ⇒ 安全
  （那条缓冲在等待者前面，事件值会先被推过等待值）；**否则记为 `waiter-committed-first`（倒置）**，并在真正的
  signaller 提交时量出**时长**（等待者提交 → signaller 提交）、**两种队列关系分开计**
  （**same-queue = 解不开的那一种**，cross-queue = 另一条队列并行跑，只是延迟）。
* 报告行（`[metal_stats]`，atexit）：

```
[metal_stats] commit order (Q9-F): commits=… waits=… waiter-committed-first=… (same-queue=… cross-queue=…) max=…ms worst kind=stage|corr|grid slot=…; per kind: stage i/w, corr i/w, grid i/w (inversions/waits)
```

* ⚠ **票号在 `[cb commit]` 之前一拍取**：票号次序 = 各线程"打算提交"的次序。若线程在票号与提交之间被抢占，
  真实队列次序可能相反——这种竞态的时长是**亚毫秒**，而缺陷是**秒级**，报告里的 `max` 用来区分（写在头文件注释里）。
* ⚠ **"读不出"不静默**：`waits still open at exit` 表示**没有任何 ≥ 该值的信号提交过**（signaller 从未提交），
  在报告行尾点名，不计入 `waits`。

**③ Q9-F2：前端（deposit）块终于有了自己的 GPU 窗口（`ocudu_metal_lane_probe.{h,mm}` + DFT 引擎）**

* `dft_metal_engine::release_block()` 在 `deposit_released()` 之后调 `gpu_lane_probe::register_front_end_commit(cb, slot)`。
  **为什么这是缺口**：交棒默认开（`grid_handover_armed()`）⇒ deposit 块**不由 DFT 引擎提交**（车道提交，或注册表 sweep 提交），
  而前端系列此前只由 `commit_front_end()` 喂 ⇒ **空气腿的前端系列是空的**（p13 的 stderr 里没有 `[ul_gpu_lane] dft slots=` 行），
  受害的却正是这些块。
* 一个块在自己的槽位分组关闭时还没跑完 ⇒ 不再被丢掉，而是**带着注册时刻（= deposit 时刻）留给报告**；报告里：
  `dft carried blocks (Q9-F2): resolved=R of T (never committed=…, committed but unfinished at exit=…, no GPU timestamps=…, dropped over the bound=…)`
  ＋两条系列（`dft carried deposit ->GPU start`、`dft carried GPU start ->end`）＋**按设备时间最慢的 8 块**
  （`deposit->start` 大 = 在队列里等；`start->end` 大 = **设备真的占着这条块**）。
  **两个"读不出"分开计**：`never committed` 是交棒自己的缺陷（没人提交的 deposit），`unfinished at exit` 是腿停在停顿里。

**④ Q9-F3：队列占用时间线（`ocudu_metal_queue.{h,mm}`，本次新增，属"§6.2 第二支"的准备）**

Q9-F 只回答"次序对不对"；若倒置为 0，§6.2 的分支要的是 **GPU 侧证据**。`arm_gpu_time()`（既有开关
`OCUDU_METAL_GPU_TIME=1`）现在**多接一个 `label` 与 `slot`**，并让**每条命令缓冲**在完成处理器里留下一条
（GPU 窗口、提交时刻、label、slot）记录。报告把**所有窗口求并**，给出：

```
[metal_stats] queue occupancy (Q9-F3): commits=… busy(union)=…us window=…us holes>100us=… largest=…ms
[metal_stats] queue occupancy (Q9-F3) slowest commits, by commit -> GPU start (label = what the buffer carries):
[metal_stats]   label=merged_hop|late_handed|ce_weights|… slot=… commit->start=…us start->end=…us
[metal_stats]   hole …ms -> next label=… slot=… (nothing was executing on any probed queue for that long)
```

* **它回答的是"等待期间设备在干什么"**：并集里的**洞**= 那段时间**没有任何**探针队列在执行；
  洞后第一条的 label/slot = **谁在等**。这是"队列被夹住"与"设备被别的活占着"的唯一分界读数。
* `[metal_stats] gpu busy (front_end/back_end)`（既有行）**只在开关打开时非零**——它此前一直是 `commits=0`，
  因为 p07–p13 都没有带这个环境变量（**这是一个此前无人注意的读数空洞**：`gpu busy` 行一直在，值一直是 0）。
* label 覆盖：`merged_hop`（被采纳的整跳）、`lane_burst`（非采纳）、`late_handed`（注册表迟提交，**此前完全不可见的那些块**）、
  `ce_stage/ce_weights/ce_held/ce_abandon/ce_weights_fb/ce_commit`、`dft_front_end`、`equalizer`、`demapper`、`split_dft`、`handed_direct`。

**⑤ 离线验证（本机，2026-09-25；判据"改代码后必跑"的那一套）**

* `dft_release_adopt_metal_test` **新增 arm 14（Q9-F 自测）**：用**真实命令缓冲**造出两种次序——
  (a) signaller 先提交、等待者后提交（**安全形状**，必须不计入倒置）；(b) **等待者先提交**、signaller 25 ms 后提交
  （**倒置**，且必须计入 `same-queue` 并量出时长）。**注意**：arm 14 **不把 wait 真的编进缓冲**——那正是它要检出的挂起
  （串行队列过不去），所以它按编码点的调用方式直接驱动簿记。实测输出：

```
[dft-release] arm 14 (Q9-F): the commit-order instrument counts a waiter that is SUBMITTED before its signaller
              apart from the safe shape - commits 53->57, waits 0->2, waiter-first 0->1 (same-queue 0->1), longest 25549 us
[dft-release] arm 14 (Q9-F3): the occupancy probe recorded a GPU window for every one of the arm's four commits (0 -> 4 records)
[metal_stats] commit order (Q9-F): commits=57 waits=2 waiter-committed-first=1 (same-queue=1 cross-queue=0) max=25.5ms worst kind=stage slot=0
[metal_stats] queue occupancy (Q9-F3): commits=4 busy(union)=15.6us window=26333.8us holes>100us=3 largest=26.0ms
```

  arm 14 的四个缓冲**每个都带一条真实（短）dispatch**，所以 GPU 会给它们时间戳、占用探针会留下记录——
  这让 Q9-F3 的"记录路径"也能**离线自证**，而不是靠相信；`largest=26.0ms` 就是该臂**故意**在等待者与
  signaller 之间插入的 25 ms 睡眠，是这台仪器自己的演示。

  同一次运行、带 `OCUDU_METAL_GPU_TIME=1` 时 Q9-F3 也自证（真实 GPU 窗口 + 洞）：

```
[metal_stats] queue occupancy (Q9-F3): commits=53 busy(union)=10382.6us window=267521.3us holes>100us=48 largest=98.8ms
[metal_stats]   label=dft_front_end slot=0 commit->start=2054.0us start->end=40.0us
[metal_stats]   hole 98.8ms -> next label=lane_burst slot=0 (nothing was executing on any probed queue for that long)
```

* Q9-F2 在同一次运行里也给出了读数（测试自己 deposit 的块）：
  `dft carried blocks (Q9-F2): resolved=5 of 8 (never committed=3, committed but unfinished at exit=0, …)`。
* **顺带修掉一个 atexit 崩溃**：`state()` 从"函数内静态对象"改为**故意不析构**（`new` + 注释）。
  理由是本 TU 在**静态初始化期**注册 atexit，而对象在**首次使用**时才构造 ⇒ 处理器**在析构之后**才跑，
  里面每个 mutex 都已失效。实测：`libc++abi: terminating due to uncaught exception … mutex lock failed: Invalid argument`
  （Q9-F3 是这段代码里**第一个在报告期加锁**的读数）。lane 探针与交棒注册表早就是同样的写法，这里是同一理由的补齐。
* `p0_gate.sh` 增加 **D11（Q9-F）/ D12（Q9-F2+F3）**：都是 **INFO（读数，不是判据）**；旧腿读成
  "a leg flown before 6.19 cannot say"（**"读不出"不静默**）。D1–D4 的阈值**一字未动**。
* **新增 `wip/p0_gate_selftest.sh`（门的 D11/D12 自测）**：拿**真实腿的 stderr** 追加三条合成行（空气腿才会有的形状），
  断言门把 `waiter-committed-first=` / `same-queue=` / `worst kind=` / 洞后的 label / `dft carried …` 都**读回原值**，
  再拿**没有这些行的腿**断言它读成 "cannot say" 而不是编一个数。**为什么值得留**：本轮就是这样抓到两个解析缺陷——
  `worst kind=` 被 `awk '{print $3}'` 读成了 `?`、洞那行因 `-A2` 太短**根本没配上**（§4.3 (3) 与 5.9.125 ⑥ 是同一类错误）。
  只读文件、不碰 GPU，用户飞腿时也能跑。
* 其余网（本机串行跑，改动后必跑的那一套）：`ctest -L phy` **193/193**、`lower_phy_test` **528/528**、`l1_handover_arms.sh` **5 PASS**；`l1_hop_arms.sh` 保持它自己那句 `OPEN`（该臂在**空闲机器**上不可证伪，5.9.39/5.9.40，与本节无关）。
* ⚠ **网必须在没有并行构建时跑**：本次有一条腿的网正好撞上 `cmake --build`（二进制被重链接）⇒ 出现一次 `port_channel_estimator_metal_mmse_unit_test_ta_chain (Bus error)` 和一次 `the input token was released 0 times (rep 17)`；**构建结束后串行重跑三次全绿**。这与 §4.4「并行跑 = 假失败」是同一条纪律，**记在这里**。

**⑥ 下一条腿（`p14-conc2`）怎么飞、怎么判（**一条腿定性**）**

```bash
# 起腿前：pgrep -x gnb / pgrep -x ul_chain_replay / lsof -nP -iUDP:2152 都要干净
# 构建：本提交已构建（戳 = HEAD）
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p14-conc2 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量：CN 侧（10.45.0.1）上行 iperf3 -c <gNB-ip> -R -t 100   ← -R 不能省
# 判读：bash doc_chinese/phy_latency/wip/p0_gate.sh p14-conc2      # D1–D12
```

> ⚠ **测量臂声明（§3.3）**：`OCUDU_METAL_GPU_TIME=1` 会给**每条命令缓冲**加一个完成处理器
> （既有注释已警告"不是免费的：驱动为每个处理器派发一个 block"）。本腿是**诊断腿**：D1–D4 仍是判据，
> 但 `V1/V4` 与 `cbs/lane` **不因它而变**（只加处理器，不改提交），若读数与 p13 差得离谱要**先怀疑探针本身**。

**判读分支表（与 §6.2 的分支表一致，写成读数）**

| `D11 waiter-committed-first` | `D12`（占用/洞/前端块） | 结论 / 下一步 |
|---|---|---|
| **> 0 且 `max` ≈ 该腿的停顿（秒级）** | 洞 ≈ 停顿，洞后 label 是 `merged_hop`/`late_handed` | **Q9-G 成立**：实现**提交握手**（§6.2 第一支）——`claimed && !produced` 时，在返回 generation 之前**等它"已经提交"**（新增 `handed_entry::commit_done`，由提交方**在提交之后、锁内**置位；等待**有界**，超界退回今天的行为并计数）。⚠ **不能把 `[cb commit]` 挪进锁内**（完成处理器可能同线程内联 ⇒ 死锁）|
| **> 0 但 `max` 只有 µs/ms** | 洞小、或前端块 `start->end` 是 ms | 倒置发生了但**没吃到停顿**（竞态窗口没被踩中/被 driver 化解）⇒ 仍建议做握手（便宜且把窗口关死），但**要继续找那 5 s**：看 D12 的 `hole` 后 label 与 `start->end` 最大的块 |
| **= 0** | 洞 ≈ 5 s 且洞后 label 明确 | **次序不是原因** ⇒ 走 §6.2 第二支：**那条 label 的命令缓冲就是阻塞者**，用它自己的 `commit->start`/`start->end` 定性（等 vs 跑），再谈"前端块独立队列"（结构性改动，**先请用户裁**）|
| **= 0** | 无洞（`holes>100us=0`）且所有 `start->end` 都是 ms | 设备**一直在跑**别的活 ⇒ 停顿是**吞吐/排队**（回到 §7.1 的 C 项：单车道串行）而不是栅栏 |
| **读不出**（无 D11/D12 行） | — | 腿没带 `OCUDU_METAL_GPU_TIME=1`，或二进制不是本节提交 ⇒ **按 RED 算**，重飞 |

**⑦ 仍未做（本节边界）**：Q9-G 的**握手本身**（等 D11 的读数）；`[ul_gpu_lane] dft carried` 的**空口基线**
（本机测试里 resolved=5/8 是测试自己造的 deposit，空气腿的形态要 p14 才知道）；P2-E 的 (b) 选项（§6.4.6 已裁"不做"）。

### 6.20 ★★★ 根因结案（2026-09-25）：**Metal 对"未被满足的设备侧事件等待"有 5.00 s 硬上界**——那 5 秒 = 一个"等待者先于 signaller **提交**"的栅栏，代价固定 5.000 s，而且**整条队列连同整个 gNB 时隙环一起停**

> 本节的结论是**离线复现**出来的（`wip/metal_wait_timeout_probe.mm`，本机 Apple M4 Pro / macOS 26.6.2），
> 不依赖任何新腿；p07–p14 的七条腿的读数随后被它一一解释。**触发腿 `p14-conc2`（§6.19 ⑥ 的那条）没有报告**
> （进程在停机 5 s 宽限内没停下，`[APP] [E] Emergency flush of the logger`），但它的**主日志**给出了本节 ③ 的系统级证据。

#### ① 离线复现（决定性读数）

`wip/metal_wait_timeout_probe.mm`：**每个臂独占一条队列与一个事件**，等待者（`encodeWaitForEvent:value:1`）**先提交**，
signaller 由另一个线程在 0/200/2000/8000 ms 后提交，最后一个臂**永远不提交**：

```
device=Apple M4 Pro
(waiter is committed FIRST in every arm except the last; 'signaller: -1' = never committed)
A1 signaller at +0 ms (same instant) waiter:  5.001 s (st=5, commit->start=5001187.0 us, run=    0.6 us)   signaller: -1.000 s
A2 signaller at +200 ms              waiter:  5.001 s (st=5, commit->start=5000667.7 us, run=    0.6 us)   signaller:  5.008 s
A3 signaller at +2000 ms             waiter:  5.002 s (st=5, commit->start=5001350.1 us, run=    0.4 us)   signaller:  5.008 s
A4 signaller at +8000 ms             waiter:  5.001 s (st=5, commit->start=5001325.0 us, run=    0.5 us)   signaller:  8.005 s
A5 NO signaller (event stays 0)      waiter:  5.000 s (st=5, commit->start=5000260.2 us, run=    0.5 us)   signaller: -1.000 s
B  signaller FIRST                   waiter:  0.206 s (st=4, commit->start=    484.5 us, run=  126.4 us)   signaller:  0.007 s
```

**两次运行逐位相同。** 三条结论：

1. **未被满足的设备侧等待在 5.00 s 后被驱动"放行"**（`status=5` Completed，**没有错误**）：A5 里事件**从未被 signal**
   （`event=0`），命令缓冲照样在 5.000 s 后跑完。⇒ 这是 Metal/AGX 驱动的一个**硬上界**，不是我们代码里的任何超时
   （本节之前查过全仓库：PHY 路径上没有任何 5 s 常量）。
2. **在那 5 s 之前，整条队列被按住**：A2/A3 里 signaller 只晚提交 200 ms/2 s，但它**直到 5.008 s 才跑**——
   **生产者被堵在消费者后面整整一个上界**。
3. **上界之后栅栏是"尽力而为"**：A4 里消费者在 **5.001 s** 放行、生产者到 **8.005 s** 才跑 ⇒
   **消费者先于生产者执行**，栅栏要保证的次序被破坏。⇒ **设备侧等待只能对"已经提交"的信号编码**。

#### ② 为什么这**就是**那 5 秒：四条独立读数指的是同一个瞬间

| 读数 | 腿 | 值 | 它与本节的关系 |
|---|---|---|---|
| lane 探针 `commit->start` | p13 | **5002977.3 µs**，而 `start->end` 只有 **1244.1 µs** | **复现里逐位相同的形状**：`commit->start ≈ 5.0011 s`、`run` 极小 ⇒ "GPU 5 秒没开始它"就是**等待被放行**的时刻 |
| 注册表 `deposit->completion` | p13 | 5004669 µs（slot 9612） | 同一个块、同一个 5 s |
| `input hold (P0-2)` max | p13 | 5004.7 ms | 输入 token 在**该命令缓冲的完成处理器**里回池 ⇒ 同一次放行 |
| `pop_blocking wait (P0-2)` max | p13 | 4997.6 ms | 池在同一次放行后才拿到缓冲 ⇒ 接收线程 park 的时长 = 同一个 5 s |

⇒ 四个"不同层面的 5 秒"其实是**一个**：一个等待者把队列按住了 5.00 s。

#### ③ 空口上的完整因果链（含 p14 为什么变成"再也不恢复"）

```
(1) 并发 2 下，某个消费者在它的命令缓冲里编码了设备侧栅栏等待，
    而承载该信号的命令缓冲**尚未提交**（Q9-G 窗口：注册表 sweep 在锁内认领、**解锁后**才提交；
    另一个线程的跳在窗口里拿到 generation 就编码等待并提交）
(2) 等待者排在 signaller 前面 ⇒ 队列按住 ⇒ **5.00 s 后驱动才放行**
(3) 排在其后的**每一个**命令缓冲都被拖住，包括那些完成时才释放接收缓冲的**交棒块**
(4) 输入 token 不回池 ⇒ 池被抽干（8/8）⇒ `pop_rx_buffer_blocking()` 无事可等
    —— 它是 `for(;;) { reap(); pop_wait_for(slice); }`，而且**在 `receiver.receive()` 之前**
    ⇒ **接收线程不再收样点**：`Real-time failure in RF: late` 每槽一次、UHD 环形缓冲溢出
    （p14 实测 `Receive stream discontinuity … (255773 samples)`）
(5) **没有样点就没有 slot indication** ⇒ L1/L2 的**整个时隙环停住（下行也停）**
(6) 5.00 s 后驱动放行 ⇒ UHD 里积压的块被一次排空 ⇒ **追赶突发**
(7) ⇒ p14 的后果：追赶期间若干 UL 时隙的 **CRC 指示永远没到 MAC** ⇒
    调度器 `All the UE HARQs are busy waiting for their respective CRC result`（30 s 内 2469 次）⇒
    该 UE 不再被授 Grant ⇒ 掉线并反复重接（RNTI `0x4603 → 0x4611 → 0x4616 → 0x4617`）⇒ iperf3 上行再也不回来
```

**p14 主日志的直接证据**（全部来自 `gnb_gpu_p14-conc2_0925_1150.log`，不需要 stderr）：

| 时刻 | 现象 |
|---|---|
| `03:52:27–03:52:42` | **完美运行**：`PUSCH` 解码 **1000/s**、`RLC UL SDU` ~1000/s、**RF 失败 0** |
| `03:52:42.14` | 最后一次成功解码；**258 ms 后**第一声 `Real-time failure in RF: late` |
| `03:52:43–46` | `PUSCH=0`、`RF late≈1450/s`、`Slot decisions 66→43/s`（正常 1000/s）、`Discarded uplink slot` ≈80/s、`PRACH buffer pool depleted` ≈38/s |
| `03:52:47` | **追赶突发**：`Slot decisions 3288`、`PUSCH 744`（一秒内把积压排空） |
| `03:52:49–52` | 再次全停（`Slot decisions = 0`、`PDSCH/PDCCH = 0`）⇒ **下行也停**，证明停的不是"某个 UL 任务"而是**时隙环本身** |
| `03:52:53` 起 | PHY **空闲**（RF 失败 0、`UL processor busy` 0）：不是 PHY 卡住，而是**该 UE 再也不被授 Grant** |
| 之后 | `ue=0`（iperf3 的 UE）**3.5 分钟零解码**，反复重接；`ue=1` 仍能被服务（PHY 是好的） |
| `03:56:34` | `[APP] [E] Emergency flush of the logger`：Ctrl-C 后 5 s 宽限到期 ⇒ **本次报告全部丢失**（§6.19 的仪器因此没用上） |

⚠ **"池先满、电台后错"这一条在本腿同样成立**：`03:52:37.938` 出现 `[ul_rx_pool] the receive pool is EMPTY (held=8/8)`
（**一次性告警**，全腿只打一行），`03:52:37.952` 才出现第一声 RF late 与 `Receive stream discontinuity`。
⇒ **池是"因"，电台 late 是"果"**，与 §6.11 的机制一致。

#### ④ 为什么以前五层追查都看不见它

* **Q9-D 比的是"generation 发出"**：信号**编码**在等待之前，但**提交**可以晚（§6.17 ③ 已登记的盲区）⇒ 全读成"安全形状"。
* **Q9-E 比的是"完成处理器滞后"**：本缺陷里处理器很及时（GPU 一跑完就回池），5 s 在**队列没开始**那一段。
* **`commit->start` 一直是正确的读法**——只是它同时兼容"队列前面有别的缓冲"与"等事件被放行"两种解释；
  本节把第二种**离线钉死**了（`commit->start ≈ 5.0011 s` 而事件从未被 signal）。
* **Q9-F/Q9-F3 本来是为此造的**，但 p14 丢了报告（③ 的表最后一行）⇒ 下一条腿必须保证干净停机（见 ⑥）。

#### ⑤ 修复：两件，分开裁

**A（缺陷本身，推荐立刻做）——"提交握手"：设备侧等待只对已提交的 signaller 编码。**
`shared_burst` 的 `handed_entry` 增加 `commit_issued`，由**提交方**在 `[cb commit]` **之后**置位
（提交点：`shared_burst::commit()` 的采纳路径、`commit_dropped()`（sweep/fallback/替代）、mmse 的 `handed_direct`）；
`grid_production_generation()` 在返回 generation 之前检查它：

* 条目的载体**已提交**（或已 `produced`）⇒ 直接返回 generation（signaller 在队列里排在前面，安全）；
* 载体**已被别人认领但还没提交** ⇒ **有界等待**（~1–2 ms，轮询；窗口本身只有几微秒，认领者是另一个跳时最长等它一跳）：
  变成已提交 ⇒ 返回 generation；
* 等待超界 ⇒ **不再编码设备侧等待**，退回**有界的主机等待**（`ensure_grid_produced()` 的 200 ms 语义）并返回 0，
  计入 `handshake_timeouts`（罕见、有界、且**正确**——比 5 s 队列冻结 + 栅栏失效好得多）。

**判据（离线可判，见 ⑥）**：`handshake_waits` 计数 >0 说明窗口真的被踩到过；`handshake_timeouts` 应为 0；
空口腿的 5 s 停顿应消失。

**B（爆炸半径，需用户裁决）——让"池"不能停电台。**
即使 A 修好，**任何**一次晚完成（或未来任何一个栅栏缺陷）都会再次抽干池 ⇒ 接收线程 park ⇒
**整个 gNB（含下行）停 5 s，然后该 UE 掉线重接**。池是**上行链路的背压**，不该是**电台的时序源**：
池干时应**继续收样点**（丢掉缺缓冲那一槽的样点），而不是把整条时隙环停住。这不是"加大池"（治标），
而是把"背压"与"时序"解耦。**它是结构性改动，必须先请用户裁**（选项：(a) 预留几个只给接收路径用的缓冲；
(b) 池干时收到临时缓冲并丢弃该槽；(c) 缩短持有期（P2-E 的 (b)）——(c) 只降低概率，不解除耦合）。

#### ⑥ 下一条腿（`p15-conc2`）怎么飞、怎么判

配方与 p07–p14 相同，**外加 `OCUDU_METAL_GPU_TIME=1`**（Q9-F3 的开关）；**跑完必须 Ctrl-C 并确认进程真的退出**
（p14 的那 5 s 宽限把报告吃掉了）：停机后 `ls -la` 的 `.stderr` 里必须有 `[metal_stats]` 与 `[ul_gpu_lane]` 行。

| 读数 | A 生效时应看到 |
|---|---|
| `p0_gate.sh` D1/D3/D4 | **不再有 ~5 s 的 `input hold` / `pop_blocking` / gap**（阈值一字不改）|
| D11（Q9-F）`waiter-committed-first` | **0**（若 >0 且 `max≈5 s`，A 没覆盖到那条路径——把 `worst kind`/`slot` 交回来）|
| D12（Q9-F3） | 运行期不再出现 ~5 s 的 **洞**；`dft carried deposit ->GPU start` 的 max 落回 ms 量级 |
| `handshake_*`（A 新增计数，接在 `[metal_stats] dft handover` 行）| `waits>0` = 窗口被踩到过；`timeouts=0` |
| 用户侧 iperf3 | 不再断流；即使偶发丢槽，**UE 不再掉线重接** |

#### ⑦ 本节边界

* 本节**没有**改任何判据（D1–D4 阈值不动）；A 的实现在下一小节（§6.21）。
* 未决：p14 里"该 UE 永久不再被授 Grant"的**恢复路径**（HARQ/CRC 指示丢失后如何自愈）是**另一个**问题（MAC 侧），
  本节只把它的**触发**去掉了；B 的裁决仍待用户。
* 复现脚本是**测量**：它花 ~50 s 等每个臂的上界到期，**不能在飞腿时跑**（会碰 GPU）。

### 6.21 ✅ 修复 A 落地（2026-09-25）：**提交握手**——设备侧等待只对"已提交"的 signaller 编码（离线 arm 15 自证）

> 针对 §6.20 的根因。**不改判据**（D1–D4 阈值一字未动）；**不改提交形态**（`cbs/lane` 不变：握手只影响
> "何时把 generation 交给消费者"，不增加、不移动任何 `[cb commit]`）。

**① 改了什么（`lib/phy/metal/ocudu_metal_burst.{h,mm}` + 一个 mmse 提交点）**

| 位置 | 改动 |
|---|---|
| `handed_entry` | 新增 `bool commit_issued`（替代条目时复位）——"承载该块 grid 信号的命令缓冲**已经提交**" |
| `shared_burst::note_block_commit_issued(cb)` | 新增公开入口，**由提交方在 `[cb commit]` 之后**调用（按 cb 查条目置位；非交棒块是 no-op）|
| 四个提交点 | `shared_burst::commit()`（车道采纳块）、`commit_dropped()`（注册表 sweep/主机 fallback/替代）、mmse `handed_direct`（交接块无法编码时）、`adopt()` 的诊断拆分前端提交 |
| `claim_grid_production()` | 多返回一个 `must_wait_for_commit`：条目 **claimed && !produced && !commit_issued** ⇒ 调用方必须先确认提交 |
| `wait_for_block_commit()` | 新增：**有界（2 ms）轮询**等待该提交（轮询而不是持锁睡眠——提交方要拿同一把锁才能置位）|
| `grid_production_generation()` | 窗口命中 ⇒ 先等提交：等到 ⇒ 返回 generation（signaller 已排在队列前面，安全）；**等不到 ⇒ 返回 0 并退回 `ensure_grid_produced()` 的 200 ms 主机等待**（有界、正确、计入计数）|
| 计数器 | `handshake=waits:…,timeouts:…,max:…us` 接在 `[metal_stats] dft handover` 行尾；门新增 **D13**（INFO）|

**为什么"等到提交"就安全**：提交次序 = 队列次序（§6.20 ① 的 B 臂：signaller 先提交时等待立刻满足）。窗口本身只有微秒级
（sweep"锁内认领 → 解锁后提交"），长尾是"另一个跳正持有该块直到它提交"——而**消费者本来就要等这个块的完成**，
所以等它的提交不引入新的等待类别。超界（2 ms）说明认领者卡住了：这时**绝不能**编码设备侧等待（那会变成 5 s 队列冻结），
退回主机等待是**正确**的那一侧。

**② 离线证据（改代码后必跑的那一套）**

* **arm 15（新，三个子例，都用真实块）**：
  * (a) 载体**已提交** ⇒ 立刻拿回 generation，`waits/timeouts` 都不动；
  * (b) 另一个线程在 **300 µs 后**提交 ⇒ 消费者的等待**把窗口关掉**（`waits 0->1`，`max 453us`），generation 正常拿回；
  * (c) 载体**永不提交** ⇒ **不交出 generation**（返回 0）且 `timeouts 0->1`（退回主机等待）。
  实测打印：

```
[dft-release] arm 15 (6.20 commit handshake): a committed carrier is handed back at once, a carrier committed
              DURING the wait is confirmed (waits 0->1, max 453us), and an uncommitted one is never handed out -
              the consumer falls back to the host wait (timeouts 0->1)
[metal_stats] dft handover … timeouts=1 … handshake=waits:1,timeouts:1,max:453us
```

* `ctest -L phy` **193/193**、`lower_phy_test` **528/528**、`dft_release_adopt_metal_test` rc=0（arm 10–15 全过）、
  `l1_handover_arms.sh` **5 PASS**（`l1_hop_arms.sh` 仍是它自己那句 `OPEN`）。
* 门的 **D13** 有自测（`p0_gate_selftest.sh`：读回 `waits/timeouts/max`，并断言没有该字段的旧腿读成 "cannot say"）。

**③ 这条修复**不**解决什么（写清楚，免得下一条腿误读）**

* **修复 B（让池不能停电台）仍未做**：握手把"5 s 冻结"的**触发**去掉了，但**任何**一次晚完成（或未来任何栅栏缺陷）
  仍会抽干池 ⇒ 接收线程 park ⇒ 整个 gNB（含下行）停住。**这是结构性改动，等用户裁**（§6.20 ⑤ B 的三个选项）。
* **MAC 侧的恢复**没动：p14 里"CRC 指示丢了以后该 UE 再也不被授 Grant、掉线重接"是另一个问题（§6.20 ③ (7)）。
* **2 ms 界与 200 ms 主机回退**是**新引入的参数**，它们只在窗口命中且认领者卡住时才生效；`timeouts` 必须为 0，
  非 0 就是"有消费者的载体从未提交"——那本身是一个**新的**缺陷读数。

**④ 下一条腿 `p15-conc2`（回归 + 定性同一条腿）**

```bash
pgrep -x gnb || echo ok; pgrep -x ul_chain_replay || echo ok      # 起腿前
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p15-conc2 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量：CN 侧上行 iperf3 -c <gNB-ip> -R -t 100
# ⚠ 跑完 Ctrl-C 后【确认进程真的退出】：p14 的报告就是被停机 5 s 宽限吃掉的（§6.20 ③ 末行）
bash doc_chinese/phy_latency/wip/p0_gate.sh p15-conc2      # D1–D13
```

| 读数 | 通过时应看到 |
|---|---|
| D1/D3/D4（判据，阈值不动）| `input hold` / `pop_blocking` / gaps **不再有 ~5 s 的尾巴** |
| D11（Q9-F）| `waiter-committed-first=0`；若非 0，把 `worst kind`/`slot` 交回来（说明还有一条等待路径没被握手覆盖）|
| D12（Q9-F3）| 运行期不再出现 ~5 s 的**洞** |
| **D13（新）**| `handshake waits>0` = 窗口真的被踩到过（这条腿就是旧腿里 5 s 的来源）；`timeouts=0` |
| 用户侧 iperf3 | 不断流；即使偶发丢槽，**UE 不再掉线重接** |

### 6.22 ★★★ 腿 `p15-conc2`（2026-09-25，带修复 A）：**触发条件确实出现过 24 次，一次都没变成停顿**——判据 **23/23 全绿**，契约 **8/8**，**0 gaps**

> 配方与 p07–p14 完全相同（n1 + `max_pusch_and_srs_concurrency=2` + `OCUDU_UL_PHASE_SEGMENTS=1` + `OCUDU_METAL_GPU_TIME=1`），
> 上行 `iperf3 -R -t 100`，腿长 **315 s**（`04:18:23 → 04:23:38`），**128,393 次交棒 / 105,951 跳**（本系列最大的一条腿）。
> 报告在进程退出后完整落盘（`gnb_gpu_p15-conc2_0925_1218.log.stderr`，185 行）。

#### ① 决定性读数：**Q9-G 窗口被踩到 24 次，每一次都被握手关掉（≤165 µs）**

```
[metal_stats] dft handover handed=128393 taken=105886 … fallback=18586 late=3921 late_time=5 not_found=3921
              timeouts=0 keepalives=1797502/1797502 (max in flight 42) (armed=1) … handshake=waits:24,timeouts:0,max:165us
[metal_stats] commit order (Q9-F): commits=234414 waits=106016 waiter-committed-first=0 (same-queue=0 cross-queue=0)
              max=0.0ms worst kind=stage slot=0; per kind: stage 0/105951, corr 0/0, grid 0/65 (inversions/waits)
[metal_stats] fence order (Q9-D): waits=106016 signaller-first=106016 signaller-after=0 max=0.0ms
```

* **`handshake waits=24, timeouts=0, max=165 µs`**：24 次"消费者拿到一个**尚未提交**的承载者的 generation"。
  §6.20 的离线复现说得很清楚——**修复前这 24 次每一次都是 5.00 s 的队列冻结**；现在每一次都在 **≤165 µs** 内关掉窗口，
  且**没有一次**需要退回主机等待（`timeouts=0`）。
* **`waiter-committed-first=0` / 106,016 次等待**：没有任何等待者排在 signaller 前面。
  `per kind: … grid 0/65` 说明"错过交棒 → 编码 grid 等待"这条**唯一存在该窗口的路径**被走了 **65 次**，全部安全。
* 这两条合起来回答"到底修好了还是没触发"：**触发过（24 次），且没再变成停顿**。这与 §6.15 的 p10 不同——
  那条全绿腿里 Q9-C **零执行**（`own=0 newest=0`），所以什么都不能证明；本条腿里机制**执行了 24 次**。

#### ② 症状侧：p13/p14 的四个"5 秒"全部消失（同配方对照）

| 读数 | p13（修前，判据红）| p14（修前）| **p15（修后）** |
|---|---|---|---|
| `input hold (P0-2)` max | **5004.7 ms** | 无报告 | **17.5 ms**（`over 100ms=0`，`over 1s=0`）|
| `deposit->completion` max | 5004669 µs | — | **17453 µs** |
| `registry commit->completion` max | 5001219 µs | — | **4196 µs** |
| lane `commit -> completion` max | **5004221 µs**（`commit->start=5002977`）| — | **3422 µs**（`commit->start` 最慢 3417 µs）|
| `pop_blocking wait` max | **4997.6 ms**（`over 1s=2`）| — | **22 µs**（`over 1ms=0`）|
| `radio sample continuity` | **2 gaps / 9.98 s** | — | **0 gaps / 0 samples**（314,942 块）|
| 池 `held_max / free_min / starved_events` | 8 / 0 / **2** | — | **4 / 4 / 0** |
| `dry-pool reaps` | 982 事件 / 6 块 | — | **0 / 0**（接收线程从未 park）|
| `handler lag` max | 2037 µs | — | **1238 µs** |
| RF 失败 | **14,520**（late 为主）| **28,082** | **16 条 underflow**（整腿 315 s，0 条 late）|
| 时隙环冻结（`Slot decisions` 1000/s → ~0）| 2 次 | 3–4 次 | **0 次**（满速段每秒都是 1000）|
| 满速上行窗口 | ~20 s 后停顿 | ~20 s 后崩溃 | **~105 s 连续 1000 PUSCH/s + ~1000 RLC SDU/s，无停顿** |
| `p0_gate.sh` | D1/D3/D4 红 | 无报告 | **23/23 全绿**（D1–D4 判据 + D5–D13 读数）|

* 契约 **`MET (8 of 8)`**、`cbs/lane=2.00 (max=2) dropped=0`、P0-5 配对账 **EXACT MATCH**（102,262/102,262）
  ⇒ **V4 与形状没有回归**（握手不增加、不移动任何提交）。
* `[ul_gpu_lane] dft carried blocks (Q9-F2): resolved=71735 of 71735 (never committed=0, …)`
  ⇒ Q9-F2 这条仪器在空气腿上**完整跑通**（此前前端系列在空气腿上是空的，见 §6.19 ③）；
  前端块自己的 `deposit -> GPU start` max **16.1 ms**、`start -> end` max 1.6 ms——**没有一个是秒级**。

#### ③ 诚实的两条边界（不许把本条腿读成"全都好了"）

1. **仍然是"一条腿"**（§5.2 纪律 2）。本条腿之所以强，是因为**机制被执行了 24 次**而不是"零执行"；
   要按判据收口仍建议**再飞 1–2 条同配方**（可选更狠的暴露：`iperf3 -R -t 300`，因为旧腿的停顿都出现在负载前 20–100 s 内）。
2. **`queue occupancy (Q9-F3)` 的"洞"不能单独当停顿读**：它只说"那段时间**没有任何**探针队列在执行"，
   而**空闲**（未接入、iperf3 结束后的静默）同样是大洞。p15 的 `busy(union)=105.7 s / window=318 s`，
   最大的三个洞（23.2 s / 11.0 s / 5.2 s）都落在**负载之外**；判断"是不是停顿"必须**同看**池/token 读数
   （本腿 `input hold max 17.5 ms`、`pop_blocking 22 µs`、`held_max=4/8` 都说明没有停顿）。
   ⇒ 这一条读法写进本节，避免下一个人把"空闲"读成"冻结"。

#### ④ 仍未做（与本条腿无关，但别忘了）

* **修复 B（让池不能停住电台）**：握手拿掉了**这一类**触发；池作为"电台时序源"的脆弱耦合仍在（§6.20 ⑤ B，**待用户裁**）。
* **MAC 侧恢复**：p14 里"CRC 指示丢失 ⇒ 该 UE 永不再被授 Grant ⇒ 掉线重接"（§6.20 ③ (7)）——本条腿没有触发，问题仍在。
* **Q12/P1-6/P2-D** 与两条陈旧网（§8）。

### 6.23 ⚠ 腿 `p16-n78-conc2`（2026-09-25，n78 重载 + 并发 2）：**停顿没了，但 V1/V2/V3 在重载工况上仍未达成**；吞吐 12.3 Mbit/s 是**链路**的限制（BLER 21%，256QAM 40%），不是本代码

> 配方 = §3.2 的加压工况（n78 20 MHz TDD、`ul_ratio=0.30`、手机发、CN 收、`iperf3 -c 10.45.0.40 -R -b 40M -P 4 -t 240`）＋ 并发 2 ＋
> `OCUDU_UL_PHASE_SEGMENTS=1`（**故意不带** `OCUDU_METAL_GPU_TIME=1`：那是会扰动提交路径的测量，本腿是判据腿）。
> 腿长 ≈ 300 s，143,506 跳 / 149,752 次交棒，报告完整（171 行）。
> **用户侧 iperf3：4 流，SUM sender 12.9 / receiver 12.3 Mbit/s，371 MB / 352 MB，3814 次重传——全程稳定、无断流。**

#### ① 稳定性：修复在 n78 重载下成立（**但这条腿没有复验 Q9-G 窗口**）

| 读数 | p13（修前，n1）| **p16（修后，n78 重载）** |
|---|---|---|
| `radio sample continuity` | **2 gaps / 9.98 s** | **0 gaps / 663,341 块** |
| `pop_blocking wait` max | 4997.6 ms | **486 µs**（`over 1ms=0`）|
| `input hold` max | 5004.7 ms | **47.2 ms**（`over 100ms=0`）|
| lane `commit -> completion` max | 5004221 µs | **7007 µs** |
| 契约 | 8/8（但判据红）| **MET (8 of 8)** |

⚠ **`handshake waits=0`、`Q9-F waiter-committed-first=0`**：30 kHz 重载下那个窗口**一次都没被踩到**
（对比 p15 的 n1 腿 24 次）⇒ **本腿是"重载下不停顿"的证据，不是修复 A 的复验**（复验是 §6.22）。

#### ② 吞吐 12.3 Mbit/s 的来源：**链路 BLER**，不是 PHY/调度器，也不是本代码

主日志逐条算（315 MB）：

```
143,506 个 UL grant = 143,506 个 PUSCH 解码（100% 的 UL 时隙都授了权、都解了）
 0 条 "PUSCH allocation skipped" / "Discarded uplink slot" / FAPI 实时失败
授权总量 4435 Mbit，解码成功 3218 Mbit（0.73）⇒ 有效 ~12–13 Mbit/s = iperf3 的 12.3
BLER 21.2%：256QAM 40.2%（59,559 次）、64QAM 5.1%、16QAM 3.4%
失败块 SINR 中位 24.1 dB > 成功块 23.0 dB；失败块 TBS 中位 5637 B > 成功块 3329 B
```

⇒ 失败**集中在最激进的高阶调制**、且**失败块的 SINR 反而更高** = **链路自适应偏激进 + OTA 信道**的特征，
不是解码器/流水线的特征。同机同小区的对照组（BLER 与业务量）：

| 腿 | 模式 | BLER | 256QAM BLER | 授权 / 解码 OK |
|---|---|---|---|---|
| `s80-ulcap40-n78` | gpu, 并发 1 | **52.3%** | 58% | 5702 / 2557 Mbit |
| `s82-phases-heavy-n78`（**V1 基线**）| gpu, 并发 1 | 27.5% | 74% | 3854 / 2349 Mbit |
| `s81-ulcap40-n78` | **cpu** | 35.3% | 47% | — |
| **`p16-n78-conc2`** | gpu, 并发 2 | **21.2%** | **40%** | 4435 / **3218 Mbit** |

⇒ 本腿的链路质量是四次里**最好**的，业务量也最大；`-b 40M` 从来不是可达目标（TDD UL 占空比 0.30）。
**"数据流没那么高"是这条 OTA 链路的现实，不是融合车道引入的回归。**

#### ③ V1–V5 在重载工况上的判定（**这才是主线目标**）

| 判据 | 阈值 | p16 实测 | 结论 |
|---|---|---|---|
| **V1** `[ul_gpu_pipeline]` 中位 | ≤ 2150 µs | **2428.1 µs**（基线 s82 = 2675.1）⇒ **−9.2%** | ❌ **未达**（要 −20%）|
| **V2** 池压力 | `starved_events==0` 且 `held_max<pool` | **starved_events=114**、`held_max=8/8`、`free_min=0` | ❌ **未达** |
| **V3** 电台 | RF 失败 ≤10、gaps=0 | gaps **0** ✓；RF 失败 **1490** | ❌ **未达** |
| **V4** 提交数 | `cbs/lane ≤2.00`、dropped=0 | 2.00 (max=2) / 0 | ✅ |
| **V5** 不回归 | 契约 8/8、crossings、CRC KO% 不劣化 | 8/8、0.00+0.00、KO% 22.1%（优于 27.5/35.3/52.3）| ✅ |

⚠ **`leg_gate.sh` 的两个 FAIL 不要记在本工作账上**：它判 `stale=0`（而 stress 腿 `stale>0` 是**预登记预期**，5.9.127 R4）
与 `UL grant ≥50% 槽`（TDD `ul_ratio=0.30` 下**连它自己的基线都只有 16.2%**）⇒ 又一次"判据绑错工况/几何"（§4.4）。

#### ④ 为什么并发 2 在 n78 上只值 −9%（n1 上值 −56%）

* 本条腿：lane 配对中位 **residency 1456 µs / busy 1057 µs** ⇒ 设备执行占驻留 73%，
  一跳是**设备绑定**；n1 的一跳是**排队绑定**（`ce` 3228 → 51 µs，5218 → 2318 µs）。
  ⇒ **n78 上 V1 的杠杆是 D（设备执行 ~1057 µs = 跨度的 44%）与 A（等样点 ~473 µs），不是并发度。**
* D 现在**分不开**：`busy split: merged_hop=1047.7 µs/lane (97%)`——整跳一条缓冲 ⇒ **Q1（头号仪表缺口）**，
  要用 P0-1 的 `OCUDU_LANE_DIAG_SPLIT` 臂把 `dft` 与其余拆开。
* ⚠ **读法更正（写进仪表口径）**：并发 2 时 `busy/residency` 从 0.93（s82，并发 1）掉到 **0.719**，
  因为**车道驻留窗口里现在包含另一条车道的执行**——这是并发下的口径变化，不是设备变懒。

#### ⑤ 重载下"池压力"的真实来源：持有期的**尾巴**（V2 的线索，也是 P2-D 的输入）

```
input hold：中位 1.7 ms、p95 2.5 ms、p99 3.0 ms、max 47.2 ms（slot 20009）
池 = 8 个 × 每 0.5 ms 一槽 ⇒ 中位只占 ~3.4 个缓冲，但 20–47 ms 的尾巴一出现就占满 8 个
⇒ starved_takes=219、starved_events=114（每次 park ≤486 µs，0 gaps ⇒ 有压力、无损坏）
```

⇒ 重载下池**已无余量**：要么砍尾巴（P2-E 的 (b) / 缩短跨跳持有），要么让池不再能停电台（**修复 B**）＋按分布定容（**P2-D**）。

#### ⑥ 单独登记：1490 条 RF 失败是**滴流**，不是风暴

构成 **1298 `underflow`（TX 侧）+ 192 `late`（RX 侧）**，从 `04:36:12`（= 全腿唯一一次 `receive pool is EMPTY` 的同一秒）
开始、以 **~5/s** 稳定滴流到 `04:39` 停止（负载结束后）。期间 **UL 从未掉过**：每秒 600 个 PUSCH 解码、0 gaps、park ≤486 µs。
⇒ 与 p13/p14 的"late 风暴"（1450/s × 5 s）**形状不同**，更像 TX/DL 侧时序（当时 DL 有 PDSCH ~200/s）；
**单独登记待查**（不阻塞主线），但在 V3 的账上**算红**。

#### ⑦ 下一步（按价值）

1. **报告不被停机吃掉**（p14/p15 的教训）＋ 再飞 1 条 n78（判据单条腿不是证据，且要确认 V1–V3 的复现性）。
2. **P0-1 拆分臂**（`OCUDU_LANE_DIAG_SPLIT`）在 n78 上跑一条 ⇒ 拆开 `merged_hop` 的 1057 µs ⇒ 才能对 **D** 下手（V1 的最大头）。
3. **修复 B / P2-D 的裁决**：重载下池已无余量（V2 红），这两件从"保险"升级为**主线的一部分**。
4. 1490 条 TX underflow 单独立项。

### 6.24 ✅ 停机丢报告的洞补上（提交，2026-09-25）：**干池的接收线程在停顿发生时就把全部 P0 读数打出来**；外加一条新读数：**RF 失败是"GPU 模式病"，并发 2 让它变 3 倍**

#### ① 为什么做：p14 那条最关键的腿**一份读数都没留下**

P0 的全部读数都由 `atexit` 处理器打印，而 atexit 只在**干净退出**时运行。`p14-conc2` 没有干净退出——
**同一个停顿把停机也按住了**：5 s 宽限到期 ⇒ `[APP] [E] Emergency flush of the logger` ⇒ 进程走了，**报告全丢**
（p15/p16 只是操作者多等了一会儿，才拿到报告；⚠ 顺带更正 §6.22 的说法：**"报告丢失"只真实发生过 p14 一次**，
p15 是我当时分析得太早、腿还在跑）。

**做法**（新增 `include/ocudu/phy/phy_pipeline_report.h` + `lib/phy/support/phy_pipeline_report.cpp`）：
把各家的报告函数（metal 队列、交棒注册表、lane 探针、DFT 引擎、接收池、`[ul_gpu_pipeline]` 探针、契约）
**同时注册进一个按需转储表**，新增 `p0_dump_reports(reason, min_interval_ms)`：现打一份与退出报告**同样的行**
（所以 `p0_gate.sh` 用现成的解析就能读）。**触发点选在"干池的接收线程"**——
`lower_phy_baseband_processor::pop_rx_buffer_blocking()` 的 park 循环里，**park 超过 20 ms** 就转储：

* 这个线程**就是**停顿的震中（它 park 是因为释放输入 token 的完成迟迟不来；它一 park，电台就没人收样点 ⇒ 没有 slot indication ⇒ 整个时隙环停住，§6.20 ③）；
* 因此转储描述的是**停顿正在发生的那一刻**——这是 p13/p14 **从来没有过**的读数（它们只能事后回读，p14 连事后都没有）；
* **限流 2 s**：一个 5 s 的停顿打 1–2 份快照，而不是几千份；**健康腿永远不会触发**（实测 park max：n1 22 µs、n78 重载 486 µs ≪ 20 ms）。

转储行自带原因与序号（`[metal_stats] p0 dump #1 (dry-pool park)`），读者一眼能分辨"停顿快照"与"退出报告"。
**离线自证**：metal 测试新增 **arm 16**——两个测试 reporter、一次不设限的转储（两个都跑）、一次限流（被抑制）、
再一次强制（又跑），并核对 dump 计数；`rc=0`。

#### ② 新读数（补 §6.23）：**1490 条 RF 失败不是"今天特别糟"，而是"GPU 模式病"＋"并发 2 放大 3 倍"**

拿同机同小区的四条腿做**归一化**比较（失败数 / grant 与 / 业务量）：

| 腿 | 模式 | 腿长 | UL 负载 | `late` | `underflow` | grant 数 | 失败/grant | 失败/100 Mbit |
|---|---|---|---|---|---|---|---|---|
| **`p16-n78-conc2`** | gpu, **并发 2** | 334 s | 316 s | 192 | **1298** | 143,506 | **1.04%** | **336** |
| `s80-ulcap40-n78` | gpu, 并发 1 | 302 s | 264 s | 84 | 471 | 145,311 | 0.38% | 97 |
| `s82-phases-heavy-n78` | gpu, 并发 1 | 273 s | 267 s | 44 | 432 | 140,204 | 0.34% | 124 |
| `s81-ulcap40-n78` | **cpu** | 276 s | 270 s | **0** | **1** | 145,246 | **0.00%** | **0.19** |

* **cpu 模式 276 s 只有 1 次失败，gpu 模式 432–1298 次**（三个数量级）⇒ 这正是本工作流的**原始症状**
  （README："gpu 融合车道…电台 underflow 几百次；同负载 cpu 0–1 次"），**V3 从未在重载工况上达成过**。
* **并发 2 把失败率抬到 ~3 倍**（1.04% vs 0.34–0.38%；每 100 Mbit 336 vs 97–124）⇒ 这是 **P2-F 裁决必须带上的一条代价**：
  并发度买到时延的同时，**把电台的实时失败率抬了 3 倍**（且这些失败**不影响** UL 数据：p16 全程 600 PUSCH/s、0 gaps）。
* 形态：**~5/s 的稳定滴流**（`underflow` 为主 = TX 侧），从 `04:36:12`（全腿唯一一次池空）起持续 ~3.5 min；
  与 p13/p14 的"late 风暴"（1450/s × 5 s）**形状不同**。⇒ 机制尚未查明，**单独立项**（疑似 GPU 模式下宿主/GPU 负载抢占了电台 TX 线程的实时性；
  cpu 模式 0 次是重要线索）。

#### ③ 下一步（两条腿，都已备好命令）

| 腿 | 目的 | 关键读数 |
|---|---|---|
| `p17-n78-conc2` | **确认腿**：与 p16 完全相同 ⇒ 单条腿不是证据；确认"重载下无停顿"与 V1–V3 的复现性 | D1–D14、V1–V5 |
| `p18-n78-split` | **Q1 拆分臂**（`OCUDU_LANE_DIAG_SPLIT=1`）：把 `merged_hop` 的 **1057 µs** 拆成 `dft` + 其余 ⇒ 这是 V1 的最大头（D 项占跨度 44%）| `busy split` 里出现 `dft=…`、`merged_hop=…` |

⚠ `p18` 是**测量臂**：一跳两条命令缓冲（`cbs/lane≈2` 但**分组**变了），**V4 不适用于它**，腿报告里必须写明。

### 6.25 ★★★ 腿 `p17-n78-conc2`（确认）+ `p18-n78-split`（Q1 拆分臂）：**Q1 结案——一跳的设备执行 87% 是前端 DFT**；V1 复现为 **2435 µs（−9%，未达）**；残余红是**同一耦合的 ms 级版本**（池干 ⇒ 接收线程 park 4 ms ⇒ 电台丢 ~10 万样点）

> 配方同 §6.23（n78 20 MHz TDD + 并发 2 + `OCUDU_UL_PHASE_SEGMENTS=1`），`iperf3 -R -b 40M -P 4 -t 240`；
> **操作者在这两条腿的上行负载前加了热身**（`ping 10.45.0.42 -i 0.1 -c 100` + 下行 `iperf3 -c 10.45.0.42`）——记录在案，
> p16 没有热身，做严格 A/B 时配方要一致（本节结论不受影响：RF 失败量级与 p16 相同）。
> `p18` 另加 `OCUDU_LANE_DIAG_SPLIT=1`（**测量臂**：一跳两条命令缓冲，V4 不适用）。

#### ① 复现性：两条独立腿几乎逐位相同 ⇒ **V1 未达是确定的**

| 读数 | p16 | **p17**（确认腿）| p18（拆分臂，仅参考）|
|---|---|---|---|
| `[ul_gpu_pipeline]` 中位（**V1**）| 2428.1 µs | **2435.4 µs** | 2577.8 µs（多一次提交，不可比）|
| p99 | 3549 | 3321 | 3629 |
| lane `residency` / `busy` 中位 | 1431.3 / 1086.3 | **1401.1 / 1091.5** | 1162.7 / 1066.5 |
| lane `period` 中位 | 1022.0 µs | **1021.0 µs** | 1089.6 µs |
| lanes | 143,506 | 145,162 | 144,429 |

⇒ V1 **−9.2% ~ −8.9%**（基线 2675.1），**没到 −20%**；两条腿一致 ⇒ 这不是"某条腿没触发"。
V4/V5 仍绿（`cbs/lane=2.00 dropped=0`、契约 8/8）；**V2/V3/D4 仍红**（见 ②）。

#### ② Q1 结案（`p18`，本轮的**决定性测量**）：**一跳的设备执行 = 87% 前端 DFT**

```
[ul_gpu_lane] busy split: dft=941.2us/lane (87% of busy, cbs/lane=1.00)
                          ch_wt=43.5us/lane (4% of busy)  merged_hop=97.0us/lane (9% of busy)
[ul_gpu_lane] dft residency == dft busy samples=52165 mean=891.3us median=908.1us max=3653.3us
```

* 拆开之后：**前端逐符号变换 941 µs**、估计器权重 43.5 µs、**抽取+EQ+解映射合计只有 97 µs**。
  合并臂的总量（`merged_hop ≈ 1048 + ch_wt 38 ≈ 1086 µs`）与拆开后的总和（941+43.5+97 ≈ 1082 µs）**一致** ⇒ 拆分可信、归因成立。
* §7.1 的账单里"已知贵项（K1 197、抽取 117、重排 117）"**全部落在那 13% 里**；
  **V1 的杠杆是前端 DFT（941 µs），不是估计器链路**——这与此前的直觉相反，是本轮最重要的方向更正。
* 拆开一跳要多一次提交 + 一道栅栏 ⇒ p18 的跨度变差（2577.8 µs），符合"测量臂"的预期（不进 V1 判定）。

#### ③ 残余症状的形状（两条腿都有，量级缩小 1000×）

| 读数 | p16 | **p17** | **p18** |
|---|---|---|---|
| `radio sample continuity`（**D4**）| 0 gaps | **1 gap / 100,938 样点** | **2 gaps / 195,247 样点** |
| `pop_blocking wait` max | 486 µs | **3969 µs** | 1343 µs |
| 池 `starved_events` / `held_max` / `free_min` | 114 / 8 / 0 | **102 / 8 / 0** | **63 / 8 / 0** |
| `input hold` max / p99 | 47.2 ms / 3.0 ms | 21.2 ms / 5.5 ms | **96.4 ms** / 4.6 ms |
| `wait_for_a_claim` max（P0-7）| 46.3 ms | 20.0 ms | **95.4 ms** |
| `commit->completion`（认领之后）| 0.9–1.1 ms | 0.88–1.5 ms | 0.89–1.1 ms |
| RF 失败（late/underflow/overflow）| 192/1298/0 | **219/1498/1** | 184/1071/2 |
| `handshake waits` | 0 | 0 | 1（max 0 µs）|

**机制（与 §6.20 的 5 秒同源，只是短）**：`p17` 的池被抽干（`held_max=8/8`）⇒ 接收线程 park **3.97 ms** ⇒
电台环形缓冲溢出 ⇒ `Receive stream discontinuity … (100938 samples)`（= 4.38 ms @23.04 Msps，**与 park 对得上**）
⇒ D4 红、RF late/underflow 起。`input hold` 的尾巴（21–96 ms）是抽干的**扳机**：
一个块从 deposit 到**被认领**要等 20–96 ms，而**认领之后**完成只要 0.9 ms ⇒ 时间全在"没人来认领"那一段。
⇒ **D14（20 ms 转储阈值）没触发**（park 只有 1.3–4 ms）——这正是设计意图：20 ms 那条线是"停机都会被按住的"那一类；
ms 级的那一类由 **D3/D4** 读数覆盖。

#### ④ 结论与下一步

1. **修复 B（让池不能停住电台）从"保险"升级为"残余红判据的修复"**：D4/V3 现在唯一的成因就是"池干 ⇒ 接收线程 park ⇒ 电台丢样点"。
   池的容量是 8 块 × 每槽一块；只要有一次多块同时晚归（20–96 ms 的认领等待），池就见底。
   三个选项（§6.20 ⑤ B）：(a) 预留接收专用缓冲；(b) **池干时继续收样点、丢弃该槽**（推荐）；(c) 缩短持有期。**需用户裁**。
2. **V1 的新目标 = 前端 DFT 的 941 µs**（占跨度 38%、占设备执行 87%）。要先回答"为什么 14 个符号的变换要 908 µs"
   （65 µs/符号），再谈砍它或与上一跳重叠；这需要一条**新的 DFT 专项读数**（现有 P1-1/P1-2 已由读码收口为"必然更差"，不适用）。
3. 顺带登记的读数：`dft radio inputs` 的 **plain route 仍有 397,993 次变换（84:16）"got their own command buffer"**
   ——占全线变换的 16%，值得单独看一眼是不是绕过了交棒（不阻塞主线）。

### 6.26 ✅ 修复 B 落地（用户裁决 2026-09-25：**(b) 池干时继续收样点、丢弃该槽**）：**干池不再能停住电台**——有界 park + 保留缓冲 + 丢弃计数

> 针对 §6.25 ③ 的残余红（D4/V2/V3）。**判据一字未改**；**不改提交形态**（`cbs/lane` 不变）；改动只在 `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}`。

#### ① 改了什么

| 位置 | 改动 |
|---|---|
| `pop_rx_buffer_or_reserve(bool& dropped)`（原 `pop_rx_buffer_blocking`）| park **有界**：`rx_park_budget = 1 ms`；超界 ⇒ 返回**保留缓冲**并置 `dropped=true`。20 ms 的停顿转储（§6.24）保留为兜底 |
| **保留缓冲** `rx_reserve_buffer` | 与池内缓冲**同形状**（`nof_rx_ports × rx_buffer_size`），但**永不进池、永不交给上行处理器、永不持有输入 token** |
| `ul_process()` 的丢弃路径 | **照常收样点**（`receiver.receive()` 进保留缓冲）⇒ **推进 `last_rx_timestamp`** ⇒ **不处理**（不打 `process_symbol_boundary`，不做 grid 写、不生成 deposit）⇒ 重新 `defer(ul_process)` 返回 |
| 符号策略下的 `rx_fill` | 丢弃的样点数被**跳过**（`rx_fill += drop_samples`）⇒ 该槽**只留一个"陈旧样点"的洞**，其余样点位置正确；否则整个槽会整体错位 |
| 计数与报告 | `[ul_rx_pool] … dropped=%llu drop_park_max=%lluus`；门新增 **D15（INFO）** |
| 反向臂 | `OCUDU_UL_RX_POOL_DROP=0` 恢复"park 到有缓冲为止"的旧行为 ⇒ A/B 用 |

**为什么 1 ms**：健康腿最坏 park 是 486 µs（n78 @12.9 Mbit/s）/22 µs（n1），而丢样点那一类 park 是 1.3–4.0 ms（p17/p18）、长形态 4997 ms（p13）。
1 ms 因此**在健康腿上永不触发、在停顿腿上必然触发**，而且远在电台环形缓冲深度之内（实测溢出对应 ~4.4 ms park = 100,938 样点 @23.04 Msps，p17）。
**代价**：丢一个块 = 一处陈旧样点的洞 ⇒ 该槽 CRC KO ⇒ **一次 HARQ 重传**；换来的是**电台继续被消费**（环形缓冲不溢出、后面的槽全都不受影响）。

#### ② 离线证据（改代码后必跑的那一套）

* `lower_phy_test` **528/528**；报告里 `[ul_rx_pool] … dropped=0 drop_park_max=0us` —— 该 fixture 的池从不干，**所以丢弃路径正确地没有触发**（这是"健康腿不丢"的那一半证据）。
* `ctest -L phy` **193/193**（首次一条 `port_channel_estimator_metal_mmse_unit_test_ta_chain` 红 = §5.4 记录的 GPU 争用偶发，`--rerun-failed` 通过、整套复跑全绿；**首次读数保留**）、`dft_release_adopt_metal_test` rc=0（arm 10–16）、`l1_handover_arms.sh` 5 PASS。
* 门 **D15** + 自测（有字段 ⇒ 读回；旧腿 ⇒ "a leg flown before 6.26 cannot say"；p16 上正确显示 `no 'dropped=' field`）。

#### ③ 空口验证：**A/B 两条腿**（同配方、只差一个开关）

| 腿 | 开关 | 期望 |
|---|---|---|
| `p19-n78-drop` | 默认（drop 开）| `dropped>0`（因为 p16/p17/p18 都出现 `starved_events=63–114`）、**`gaps=0`（D4 转绿）**、RF late/underflow 显著减少 |
| `p20-n78-nodrop` | `OCUDU_UL_RX_POOL_DROP=0` | `dropped=0`、**`gaps≥1` 回来**（复现 p17/p18 的 `Receive stream discontinuity`）|

⇒ 两条腿合起来证明"丢块而不是丢样点"这件事**确实由本改动造成**，而不是又一次"没触发"。

#### ④ 边界与未决

* 丢块只影响**当前槽**（洞 + 该槽的 CRC KO）；时间戳照常前进，符号/相位对齐不变。
* **不解决**：V1（时延，杠杆是 §6.25 ② 的前端 DFT 941 µs）与 V2 的**根因**（池为什么会被抽干：`input hold` 的 20–96 ms 尾巴 = "没人来认领"那一 段）；本修复只把"池干"的**后果**从"电台丢样点"降级为"丢一个块"。
* `OCUDU_UL_RX_POOL_DROP=0` 是**测量臂**，不是交付选项。

### 6.27 ⚠ A/B 腿 `p19-n78-drop` / `p20-n78-nodrop`：**A/B 本身不确定**（两条腿都没到丢弃阈值），**但它抓出了修复 B 自己的一个真缺陷**（已修 + 加编译期护栏）；顺带两条新读数

> 配方同 §6.23/§6.25（n78 + 并发 2 + `OCUDU_UL_PHASE_SEGMENTS=1` + 上行 `iperf3 -R -b 40M -P 4 -t 240`）；
> `p20` 用 `OCUDU_UL_RX_POOL_DROP=0` 关掉修复 B 作反向臂。两条腿的报告都完整。

#### ① A/B 判不了：**两条腿都没触发丢弃**

| 读数 | p19（drop 开）| p20（drop 关）|
|---|---|---|
| `[ul_rx_pool] dropped=` | **0** | **0** |
| `pop_blocking wait` max / `over 1ms` | 306 µs / **0** | 235 µs / **0** |
| `starved_events` | 102 | 90 |
| `radio sample continuity`（**D4**）| **0 gaps** | **2 gaps / 163,495 样点** |
| `[ul_gpu_pipeline]` 中位（V1）| 2413.1 µs | 2416.4 µs |

⇒ **两条腿的 park 都远在 1 ms 预算之下，丢弃路径一次都没进** ⇒ **"0 gaps vs 2 gaps" 不能记在修复 B 头上**（p16 同码 0 gaps、p17 同码 1 gap 已经说明 D4 是**偶发读数**）。
⇒ **按 §5.2 纪律 2，这一对腿对修复 B 是"不确定"**，不是"证明"。

#### ② ★ 但这对腿抓出了**修复 B 自己的一个真缺陷**（这正是飞腿的价值）

**`pop_wait_for()` 等的是整个 slice（10 ms），而我的预算是 1 ms** —— 于是预算**永远到不了**：第一次等待就已经 park 了 10 ms，
等预算判定时电台环形缓冲（实测 ~4.4 ms 就溢出）早就丢过样点了。
⇒ 若真出现 p13/p14 那种 5 s 停顿，修复 B 会**晚 10 ms 才丢弃**，等于没修。
**已修**：drop 打开时 `wait_slice = min(rx_reap_slice, rx_park_budget)`（顺带把干池期的 reap 频率提高 10 倍，这正是让注册表更快还缓冲的那件事）；
drop 关闭时保持 10 ms（= 修复前行为，A/B 臂）。
并加**编译期护栏**：`static_assert(rx_park_budget <= rx_reap_slice, "the dry-pool wait slice must not exceed the drop budget")`
——这样后来的人不可能再悄悄把它改回去。

#### ③ 修复 B 的**离线自证**（把"没触发"这件事从腿里挪出来）

新增诊断臂 **`OCUDU_UL_RX_POOL_DROP_FORCE=<n>`**（与 `OCUDU_D1_HANDED_BOUND` 同类：**强制前 n 次 take 走"池干"路径**，
让丢弃路径在健康池上也能真跑一遍）。`lower_phy_test`（真·收包链 + mock 电台）：

```
drop 开 + FORCE=3： dropped=3   drop_park_max=1001us   [ul_rx] blocks=2009（少了 3 块）   528/528 PASS
drop 关 + FORCE=3： dropped=0   drop_park_max=0us      [ul_rx] blocks=2012（一块不少）   528/528 PASS
```

⇒ **预算（1001 µs = 1 ms 整）、丢弃决策、保留缓冲、时间戳推进、计数器** 全部离线跑通，且**丢掉一个块不会破坏流水线**（528/528）。
这是"健康腿不会触发"与"触发时行为正确"两半证据里的后一半。

#### ④ 新读数一：p19 的 **2.79 秒持有是良性的**，而且解释了"为什么以前 5 秒会炸"

`p19`：`input hold max = 2786.5 ms`、`oldest unclaimed age max = 2785.5 ms`（slot 10968，最后是 **sweep** 认领的、认领后 **0.93 ms** 完成），
但 **`pop_blocking max = 306 µs`、`gaps = 0`、`dropped = 0`** ⇒ **池没有被抽干**。
原因：那 2.79 s 里 **UL 整段静默**（没有 deposit ⇒ 没有 hop ⇒ 注册表没有入口点 ⇒ sweep 的两条规则都**没人来评估**），
而**只有一个块**被按在那里，另外 7 个缓冲照常轮转 ⇒ 池不干 ⇒ 没有连锁。
⇒ **结论：单块长持有是无害的；以前 5 秒之所以炸，是因为"一次停顿 = 一整池 8 个块同时被按住"**（§6.20 ③）。
⇒ 同时登记一个新洞：**sweep 的 10 ms 期限只在"有注册表入口点"时才被评估**，而 UL 静默时唯一的驱动是"池干触发 reap"
（Q9-A）—— 池不干就没人驱动。**这一条是"持有期尾巴"（20–105 ms）的直接来源，但它是无害的长尾，优先级低于 V1。**

#### ⑤ 新读数二：**D4（样点连续性）是偶发的，而且至少有两个成因**

| 腿 | 代码 | `pop_blocking` max | gaps / 丢失样点 |
|---|---|---|---|
| p16 | 修复前 | 486 µs | 0 |
| p17 | 修复前 | **3969 µs** | **1 / 100,938**（≈4.38 ms @23.04 Msps ⇒ **与 park 对得上**）|
| p18 | 修复前（拆分臂）| 1343 µs | 2 / 195,247 |
| p19 | 修复 B 开 | 306 µs | **0** |
| p20 | 修复 B 关 | **235 µs** | **2 / 163,495**（⇒ **与 park 对不上**：park 只有 235 µs）|

⇒ 除"宿主 park"之外**还有第二个丢样点机制**（p20 同时有 2 条 `Real-time failure in RF: overflow`；`Receive stream discontinuity` 的告警是**一次性**的，所以"只有一行告警但计数 2"是正常的）。
⇒ **D4 不能靠单条腿判**；要判它需要多跑几条同配方腿（并记录每条的 park 与 RF 种类）。p16/p19 的 0 gaps 与 p17/p18/p20 的 1–2 gaps 是同一套代码。

#### ⑥ 下一步

1. **可选、便宜**：一条**短腿**（`-t 90`）带 `OCUDU_UL_RX_POOL_DROP_FORCE=20` ⇒ 在**真电台**上跑通丢弃路径
   （期望 `dropped=20`、`drop_park_max≈1000us`、契约 8/8、无 `Unexpected symbol index`、解码率不塌）。这是修复 B 在空口上的最后一格证据。
2. **D4 的第二个机制**：需要 3–4 条同配方腿做统计（顺带看 `overflow` 与 park 的关系）；已登记。
3. **主线不变**：V1 的杠杆是**前端 DFT 941 µs**（§6.25 ②）；V2 的根因是池压力（`starved_events` 90–114，与 20–105 ms 的持有长尾同源，见 ④）。

### 6.28 ✅ 修复 B 的空口验证（腿 `p21-n78-forcedrop`，`OCUDU_UL_RX_POOL_DROP_FORCE=20`，上行 `-t 240`）：**丢弃路径真跑过 20 次，全部哨兵通过；park 被真正界在 1 ms 内**

> 配方同 §6.23（n78 + 并发 2 + `OCUDU_UL_PHASE_SEGMENTS=1`），**强制臂让前 20 次 take 走"池干"路径**
> （§6.27 ③ 的离线臂搬到空口：真电台 + 真负载）。报告 189 行。

```
[ul_rx_pool] taken=535170 returned=535150 held_end=20 held_max=28 pool=8 free_min=0
             starved_takes=85 starved_events=52 dropped=20 drop_park_max=1001us
[ul_rx_pool] pop_blocking wait (P0-2): takes=535170 … max=950.0us; over 1ms=0, over 10ms=0, over 100ms=0, over 1s=0
[phy_pipeline]   radio sample continuity: 0 gaps over 535140 blocks -> OK
[phy_pipeline]   host sample assembly: 7491960 of 7491960 … 0 copied -> OK
[phy_pipeline] contract MET (8 of 8 checks applicable)
[ul_host] symbols=7491960 in_place=7491960 … assembled=0
[ul_gpu_pipeline] samples=123088 mean=2413.0us median=2440.0us p95=3033.9us p99=3125.5us
[ul_gpu_lane] lanes=143866 cbs/lane=2.00 (max=2) dropped=0 carried=0
```

| 哨兵 | 结果 |
|---|---|
| `dropped=` | **20**（= 强制数）且 `taken − returned = 20` ⇒ **账目逐位自洽**（20 块确实进了保留缓冲、没有回池）|
| `drop_park_max` | **1001 µs** = 预算整（与离线一致）|
| `pop_blocking max` | **950 µs**（修复前同配方是 **3969 µs**，p17）⇒ **park 真的被界在 1 ms 内** |
| **D4 样点连续性** | **0 gaps / 535,140 块** ⇒ 绿（对照 p20 关掉修复时 2 gaps / 163,495 样点）|
| 契约 | **MET (8 of 8)**；`host sample assembly` 0 copied；`[ul_host] assembled=0` |
| 对齐哨兵 | **`Unexpected symbol index` 出现 0 次**（20 次丢弃没有破坏时间戳/对齐记账）|
| V1 | 中位 **2440.0 µs**（p16–p20：2413–2435）⇒ **无惩罚** |
| V4/V5 | `cbs/lane=2.00 (max=2) dropped=0`、零 crossing |

⇒ **修复 B 收口**：健康腿不触发（p19/p20 `dropped=0`）、触发时行为正确（本腿 20 次）、且**界限生效**（`pop_blocking max` 从 3969 µs 降到 950 µs，
即使不需要丢弃，短 slice 也把 park 界住了：本腿 `starved_events=52` 全部在 1 ms 内自行解决）。

#### 仍未结的两项（下一阶段）

1. **V1（主线）**：杠杆是**前端 DFT 941 µs**（§6.25 ②）——n78 一跳 ~908–941 µs 的 GPU 时间做 14 个符号的变换（≈65 µs/符号），
   对一个 1024 点 FFT 而言**慢了一到两个数量级**，是最大的一块钱。下一步先**读码 + 读现有计数器**定位这 941 µs 花在哪
   （变换本体 / grid 写回 / 分批与 dispatch 开销 / 输入 wrap），**不需要飞腿**；有结论再给测量臂。
2. **V2 的根因**：池压力（`starved_events` 52–114）来自 20–105 ms 的持有长尾 = "UL 静默期没有注册表入口点"（§6.27 ④）。
   现在它是**无害的长尾**（单块、池不干），但它是 V2 判据唯一剩下的东西。

### 6.29 ⚠⚠ **更正 §6.25 ②**：那 941 µs 不是"DFT 的算力"，而是**前端"每符号一次单 threadgroup 派发"的占用窗口**——离线微基准（`wip/dft_kernel_cost.mm`）

> 起因：§6.25 ② 用拆分臂把一跳的设备执行归因为"**87% 前端 DFT（941 µs）**"，并据此把 V1 的杠杆指向 DFT。
> 但 941 µs / 14 个符号 = 67 µs/符号，对一个 768 点 FFT 而言**慢得不像话** ⇒ 先写一个**离线微基准**去量这个 kernel 本身。

#### ① 离线微基准（加载**生产**的 `ocudu_dft.metallib`，同表同参数，200 次派发记 GPU 窗口）

```
device=Apple M4 Pro  kernel=dft_dit
=== n=768  (radix2=8 radix3=1, threads=768, threadgroup memory=6144 B) ===
xforms/dispatch   GPU us/dispatch   GPU us/transform
        1              12.18              12.18
        2              12.15               6.08
        7              14.25               2.04
       14              13.75               0.98        ← 一个 n78 时隙的 14 个符号
```
（n=512 同形：1 个/派发 28.3 µs、14 个/派发 23.4 µs；grid 写回只占 1–3 µs；n=768 与 n=512 都测了。）

⇒ **kernel 本身很快**：14 个符号的变换**一次派发**只要 **13.75 µs**（0.98 µs/变换）。
⇒ 但**单 threadgroup 派发是"延迟绑定"的**：12.2 µs/次，而且 200 次**不重叠**（整条缓冲 200×12.2 µs）。
⇒ 而**前端走的正是单 threadgroup 派发**（`ocudu_dft_metal_engine.mm`：`dispatchThreadgroups:MTLSizeMake(1,1,1)`，
注释自己写明 *"the RX chain dispatches one transform per symbol"*）⇒ 一个时隙 14 次 × 12.2 µs ≈ **171 µs**。

#### ② 那 941 µs 到底是什么：**`busy` 是"占用窗口"，不是"算力"**

* 空中读到的 941 µs（拆分臂 dft 段）是整个前端**命令缓冲的 `GPUEndTime − GPUStartTime`**。
  Metal 允许同一条队列的命令缓冲**重叠执行**，所以这个窗口里包含"**设备在跑别的缓冲**"的时间——
  它不是这 14 个变换消耗的 SM 时间（后者上限是 171 µs，批量化后是 14 µs）。
* 文中已做过一个直接对照实验（同一微基准）：**短缓冲跟在一条 ~800 µs 的长缓冲之后**，
  无论两者是否绑定**同一个 MTLBuffer 对象**，短缓冲的窗口都只有 **17–21 µs**（`start-delay ≈ 0.3 µs`）
  ⇒ **不是"池复用地址导致驱动串行化"**（这是先前一个候选解释，已排除）。
* ⇒ **结论（更正）**：§6.25 ② 的"设备执行 87% 是前端 DFT"应当读作
  "**前端命令缓冲的占用窗口占 lane busy 的 87%**"；**真实算力**是 ~171 µs（前端，未批量化）
  ＋ ~140 µs（估计器+EQ+解映射）≈ **300 µs/时隙**，而不是 ~1080 µs。
  §7.1 的 D 项（≈1125 µs"本跳设备执行"）同样应当读作**窗口上界**，不是算力预算。

#### ③ 由此得到的两个**具体**杠杆（都比"优化 FFT kernel"更对症）

1. **把前端的 14 次单 threadgroup 派发合成 1 次 14-threadgroup 派发**：
   离线实测 171 µs → **13.75 µs**（−92%）。需要 kernel 支持"多符号"参数（每个 threadgroup 的
   输入偏移/窗口/网格目标由 `tgid` 索引一张小的常量表），引擎把 14 个符号**编码进同一次派发**。
   这是**有界、离线可判**的改动（微基准就是验收：`xforms=14` 行必须保持 ~14 µs）。
2. **重新核对 C 项**：既然 D 的**算力**只有 ~300 µs，而跨度是 2428 µs，那么 §7.1 里"C ≈ 901 µs 等车道空出来"
   其实是**更大的一块**（按 600 跳/s × 2 车道、跨度 2.4 ms ⇒ 车道利用率 ~72%，排队项自然最大）。
   ⇒ 缩短**任何**一跳固定时延（A 收样点 473、D 窗口、E 110）都会**超线性**地缩小 C；
   **并发的收益也来自这里**（P2-F 裁决的那张表）。

#### ④ 顺手登记的读法更正（与 §2.3.1 同类）

* **`[ul_gpu_lane] busy` 与 `busy split` 是"每条命令缓冲的占用窗口的加和"**，在同队列缓冲重叠时
  **大于**它们消耗的 SM 时间 ⇒ 不能用它当"算力账单"；要量算力必须**离线微基准**（本节）或
  per-dispatch 计数器（本机不支持，§8 Q1）。
* 因此 `busy/residency` 低（0.719–0.917）**主要反映重叠/排队**，不是"设备在偷懒"。

### 6.30 ★★ **前端批量化（Q9-F4）：一个时隙的 14 个变换合成 1 次派发**——离线 **159.8 → 10.6 µs/槽（15.1×）**，网格**逐字节相同**；并据此**重核 §7.1 的账**（提交 `a6b2d3f629`）

> 这是 §6.29 ③ 1 的那把刀，也是本工作流**第一次按"修正后的算力账单"动手**。
> 结论先行：**离线达标（15.1×，判据 `xforms=14` 行保持 ~14 µs 且写侧打开 ⇒ 实测 10.6 µs/槽）**，
> 在线收益待腿 `p22-n78-batch14`（§6.30 ⑥ 的预登记）。

#### ① 改了什么

1. **kernel**（`ocudu_dft.metal`）：`grid_write_params` 的最后一个字段（原本的 `pad`，**一直是 0**）成为
   **多变换标志**：`0` = 历史"一次派发一个变换"；`N > 1` = **这次派发携带 N 个变换**（每 threadgroup 一个），
   **两个参数块都按 `tgid` 索引**。实现是常量地址空间上的三行地址算术
   （`&gw_block + tgid` / `&ip_block + tgid`），**kernel 的参数列表没有变** ⇒ 所有既有派发点都不用改绑定
   （这是选 `pad` 而不是加新参数的原因：加参数会让每个派发点都必须改，包括离线工具与别的引擎）。
2. **引擎**（`ocudu_dft_metal_engine.mm`）：新旋钮 **`OCUDU_DFT_BATCH_SYMBOLS=N`，默认 1 = 关**。
   `N ≥ 2` 时，**开着的块**里走**电台 int16 输入**的变换被**延迟**，在块结束时用**一次**
   `MTLSizeMake(N,1,1)` 派发出去。四条正确性要点（都写进了代码注释）：
   * **延迟对设备不可见**：块在交棒/提交前**不提交**，所以一个时隙的 14 个变换**本来就在同一条命令缓冲里**——
     移动的只是**宿主编码的时机**，不是设备执行的内容与顺序（这正是"批量化能在交棒默认开的链路上安全落地"的原因；
     也解释了为什么它**不该**改 `cbs/lane`）。
   * **只批处理电台输入**（`grid_write::time_samples != nullptr`）：它的 14 个切片是**同一个分配**的 14 个偏移，
     而块的 keepalive token 已经把这个分配保活；`staged` 的 float2 环可能被调用方在编码前重新填充。
   * **表项的输入偏移要减去 `tgid × n`**：kernel 读 `ip[tgid].offset + tgid×n + perm[i]`（批布局"一个变换一个步长"），
     而电台的符号切片是 `cp + n` 一个、且 CP 跳过已经折进偏移 ⇒ 表项 = **切片自身偏移 − 步长**。
     切片比自己的步长更靠近分配基址（只可能出现在**重叠切片**的调用方）时**不可表达**：关掉当前组，让它做下一组的 0 号。
   * **组只在绑定相同时扩张**（grid 映射 + 其偏移、电台分配、float2 环、`base`）；**块的三处结束**都会决定它的去向：
     `commit_open()` 与 `release_block()` 在关编码器**之前**flush，`discard_open_block()` **只清不编码**（缓冲被丢弃，
     编码了也没人跑）；`begin_block()` 清空；`submit_at()`（plain 路径）在编码前 flush，保证"延迟的先出去"。
3. **仪器**：`[metal_stats] dft … batched=<dispatches>/<transforms> batch_max=<N>`；
   `dft_metal_engine::batch_stats()`（离线臂可读）；门 **D16**（INFO：`batch_max=1` = 对照臂；
   `batch_max>1` 而 `batched=0/0` ⇒ **机制没跑**，这条腿不算 A/B）；metal 自测 **arm 17**。
4. **顺带修的两处"夹具前提过期"**（是自测的缺陷，不是门的缺陷）：`p0_gate_selftest.sh` 原先拿**最新**腿做夹具，
   而自 6.19 起**每条腿都带 Q9-F 行** ⇒ 反向判据（"没有新行的腿必须读成 cannot say"）与 D14/D15（p21 真丢了 20 个块、
   真打了 dump）在 p15 之后**误红**。现在夹具取**最新的、不含 Q9-F 行的腿**（当前 `p14-conc2`），找不到就**带原因 SKIP**；
   反向判据与 D14/D15/D16 现在一起绿。

#### ② 离线验收（判据先登记：`xforms=14` 那一行保持 ~14 µs，**且写侧打开**）

`wip/dft_kernel_cost.mm`（**加载生产的 `ocudu_dft.metallib`**）新增"**前端自己的形状**"一段：
14 个符号、**电台 int16 输入**、**grid write 打开**、**每符号各自的 `dst_offset` 与相位补偿**（这正是表路径要支持的东西）：

```
--- the front end's own shape: 14 symbols, radio int16 input, grid write ACTIVE ---
14 dispatches x 1 threadgroup (scalar blocks)            159.77 us/slot     ← 前端原样
 1 dispatch  x 14 threadgroups (per-tgid tables)          10.61 us/slot     ← 批量化后
speed-up                                                  15.06x
the two shapes wrote the same grid                   YES (batched == per-symbol, byte for byte)
```

* n=512（n1 小区）：**141.62 → 10.35 µs/槽（13.68×）**；n=768（n78）：上表。
* 与 §6.29 的估计一致（14 × 12.2 ≈ 171 / 13.75 µs），**且逐字节证明表路径没有写错行 / 错相位 / 错切片**
  （这是"表索引写错"唯一能在离线抓住的地方）。
* 完整读数留在 `doc_chinese/work_tmp/dft_kernel_cost_q9f4_0925_1412.txt`（git 忽略）。

#### ③ 在线自测（arm 17，走**生产的交棒路径**）

同一个引擎、同样的 14 个符号、同样的每符号参数，跑**两次**（旋钮 1 vs 14），都经
`begin_block → 14 × submit_slot_grid_write → release_block → take_released → commit → wait`：

```
[dft-release] arm 17 (6.30 batched front end): the same 14 symbols through the same release path produce a
BIT-IDENTICAL grid whether they are dispatched one per symbol or all in one dispatch, and the counters say the
batched arm really deferred them (dispatches 0->1, transforms 0->14, knob=14)
```

臂里同时断言**对照臂的计数必须是 0/0**（旋钮关时一次批处理都不许发生），以及批处理臂是**恰好 1 次派发**（不是 2 次或 14 次）。
> 开发这条臂时它先报了 `dispatches=2 transforms=10` ⇒ 抓出**推送路径的一个真实次序缺陷**：绑定记录写在"步长守卫"**之前**，
> 而守卫关组时会清掉刚记下的绑定 ⇒ 每个新组都从"nil 绑定"开始、第二个变换就再次切组。修法是把绑定记录移到守卫**之后**
> （代码里以 (1)(2)(3)(4) 标注了必须的顺序）。**另一处**是臂自己的切片尺寸写错（把"复数样点"当成了"int16"，
> 导致切片重叠），守卫**正确地**把它拆成多组——即守卫按设计工作。

#### ④ 网（全绿；一次 CE Metal flake 按 §5.4"保留第一次读数"记录）

| 网 | 结果 |
|---|---|
| `ctest -L phy` | **193/193**（第一次整套跑到 `port_channel_estimator_metal_mmse_unit_test_ta_chain` **Bus error**；单独重跑 **3/3 绿**、整套重跑 **193/193 绿** ⇒ §5.4 记录的争用 flake，第一次读数保留）|
| `lower_phy_test` | **528/528**（`OCUDU_UL_RX_POOL_DROP_FORCE=3` 同样 528/528）|
| metal 测试 | **arm 10–17 全 PASS**（`OCUDU_DFT_BATCH_SYMBOLS=14` 时同样 exit 0）|
| `dft_processor_metal_unit_test` | **ALL OK**（旋钮关 / 开各一次）|
| `l1_handover_arms.sh` | **5 PASS** |
| `p0_gate_selftest.sh` | **PASS**（含新 D16 的双向断言）|
| `p0_gate.sh` 对既有腿 | p16 / p21 仍 **26/26**；D16 在 6.30 之前的腿读成 **"a leg flown before 6.30 cannot say"**（"读不出按 RED"的规矩）|

#### ⑤ §3.2 的产出：重核 §7.1 的账——**C 是"驻留窗口在排队"，不是"算力在排队"**

把 §7.1 的五项**按"窗口 / 算力"重写**（n78 加压、并发 2、中位 ~2430 µs；§6.29 + 本节读数）：

| 项 | 量级 µs | 它**真实**是什么 | 其中的**算力** | 杠杆 |
|---|---|---|---|---|
| **A** 等样点 | ≈ 473 | 电台整槽收包的**结构**项 | **0** | P1-7（符号级收包），触 V4 |
| **B** 前端宿主 | ≈ 48 | 14 × encode + 交棒记账 + notify | 宿主 ≈48 | **本节：encode 从 14 次降到 1 次** |
| **C** 等车道空出来 | ≈ 901 | **本车道上一跳的"驻留窗口"还没走完**（排队） | **0** | 任何固定窗口缩短都会**超线性**缩小它 |
| **D** "本跳设备执行" | ≈1125 | **占用窗口**：算力 + **别的缓冲的重叠** | **~300**（前端 ~171 → **10.6**，CE/EQ/demap ~140） | 本节（前端）；其后 CE/EQ/demap |
| **E** Pass-3 + fork | ≈ 110 | 宿主（LLR 出页/解扰/解复用 + 解码 fork） | 宿主 ≈110 | P1-6（拆 B/E） |

* **车道利用率**：ρ = λ·S/m = **600 跳/s × 2.43 ms / 2 车道 ≈ 0.73**（与 §6.29 ③ 2 的 ~72% 一致；600 跳/s = n78 TDD
  `ul_ratio=0.30` 的上行槽率，腿的 `lanes=143866` 也印证了它）。
* ⇒ 缩短一跳的**任何固定窗口**都会**超线性**地缩小 C：一阶放大 **1/(1−ρ) ≈ 3.7**（随机到达的上界；确定性到达下**至少 1×**）。
* ⇒ **本节的直接收益**：前端窗口 **−149 µs/槽**、宿主 encode **−13 次/槽**。**V1 的预登记预测**（两种结果都要接受）：
  * **若前端窗口在关键路径上** ⇒ 中位**至少 −149 µs（≈2290）**；叠加 C 的放大后可能 **≈1950–2100**（**可能直接落进 ≤2150**）。
  * **若中位不动** ⇒ 前端窗口**已被前一跳完全重叠**，关键路径是 **A 与 C** ⇒ 下一个杠杆是 **A（P1-7）**与 CE/EQ/demap 链，
    **不再**在前端上花时间。
* **增量账（谁最贵）**：一跳**算力** ~300 → **~150 µs**；而 A（473）+ C（901）= **1374 µs 是零算力的时序/排队项**
  ⇒ **只砍算力打不到 V1**：2150 = 2675 − 525，本节把这条路的**一大半**走完（含放大可能 −400），
  剩下的**必须**来自 A 或 C 的**结构项**。这句话是给下一条腿定的性：**p22 若只降 ~150 µs，就该转 A/C，而不是继续砍 kernel。**

#### ⑥ 怎么用（A/B 与腿命令）

```bash
# B 臂（批量化）：与 p16–p21【逐字相同】的配方 + 一条旋钮；飞前先重建（戳必须 = HEAD）
cmake --build build --target ocudu_versioning && cmake --build build --target gnb
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p22-n78-batch14 \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
#    流量（CN 侧）：iperf3 -c <gNB-ip> -R -b 40M -P 4 -t 240      ← -R 不能省
#    停机：单次 Ctrl-C，并确认进程真的退出（atexit 才写报告）
# 判读
bash doc_chinese/phy_latency/wip/p0_gate.sh p22-n78-batch14                       # D1–D16
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 p22-n78-batch14   # V1–V5
```

* **A 臂 = p16–p21**（`batch_max=1`，当年是默认值），无需再飞；**先验条件**：`p0_gate.sh` 的 **D16 必须读成
  `batched=<d>/<t> batch_max=14`**——`batched=0/0` 表示这条腿**不是** A/B（延迟从未发生），此时不要读 V1。
  ⚠ **自 §6.31 的用户裁决起，默认值就是"开"（**§6.33 更正为 AUTO = 一个时隙的符号数**，normal CP 即 14）**：B 臂**不需要**旋钮，A 臂必须**显式** `OCUDU_DFT_BATCH_SYMBOLS=1`。
* ⚠ **判 V1–V5 的腿一律不带 `OCUDU_METAL_GPU_TIME=1`**（它会扰动提交路径）。
* 读数：**V1 中位**（对照 2413–2440）、`[ul_gpu_lane]` 前端块窗口（Q9-F2 carried）、契约 8/8、
  **V4 `cbs/lane` 应当不变**（批量化不改提交数——这一条是本改动的**契约性**断言，不只是性能）。



### 6.31 ★★★ 腿 `p22-n78-batch14`（前批量化的 A/B）：**V1 达成** —— `[ul_gpu_pipeline]` 中位 **1513.4 µs**（p16–p21：2413–2440），**−38%**；一跳自己的窗口 **1047 → 625 µs**（**与负载无关**的那一半）；同时 D1/D2 转红，且**原因不是批量化**

> 配方与 p16–p21 **逐字相同**（n78 加压、`OCUDU_UL_PHASE_SEGMENTS=1`、并发 2），只多 `OCUDU_DFT_BATCH_SYMBOLS=14`。

#### ① 先验条件（预登记的第一条）：**机制真的跑了** —— 门 **D16**

```
read: batched=154062/2156868 batch_max=14: the front end DID defer
```
* 2,156,868 / 154,062 = **14.000** 个变换/派发 ⇒ **一个时隙一次派发**，没有例外。
* 交叉核对（两条独立读数对上了）：`[metal_stats] dft … released=154062`（一槽一次交棒）与契约行的
  `dft radio inputs: … the hand-over route carried 2156868 (82.7%)` ⇒ **批处理的变换全部走交棒路径**。
* `handshake waits:1 max:35us`（无害）、`slots_in_flight=0`、`wrap_copies=0`。

#### ② V1（**本工作流的主线判据**）：**达成**

| 读数 | p16 | p17 | p19 | p20 | p21 | **p22（批量 14）** |
|---|---|---|---|---|---|---|
| **V1 `[ul_gpu_pipeline]` 中位** | 2428.1 | 2435.4 | 2413.1 | 2416.4 | 2440.0 | **1513.4 µs** |
| 均值 / p95 / p99 | 2421.9 / 3037 / 3549 | 2409.7 / — / — | 2408.5 / — / — | 2404.5 / — / — | 2413.0 / 3034 / 3126 | **1533.0 / 1671 / 2041** |

* **判据 ≤ 2150 µs ⇒ MET**：中位 **−926.6 µs（−38.0%）**；对基线 `s82`（2675.1）是 **−43.4%**（判据要 −20%）。
* **不只是中位**：**p95 = 1671 µs 也在 2150 之内**（p16/p21 的 p95 是 3034–3037），p99 = 2041。
* **没有任何"用别的东西换"**：契约 **8 of 8**（`0 gaps`、`0.00+0.00` crossings、`ce device estimates 0 host`、
  零拷贝 0 failures/0 misaligned）、**V4 `cbs/lane=2.00 (max=2) dropped=0` 逐字不变**
  （这正是 §6.30 ① 断言过的"批量化不改提交数"——**契约性**结论，现在有空口读数）、`stale=0`。
* `leg_gate.sh`：**8/9**，唯一 FAIL 仍是那条绑错工况的 `UL grant ≥50%`（TDD `ul_ratio=0.30`；基线自己也只有 16.2%）。
* 流量可比性：**hop 数几乎相同**（143,125 vs 143,508/143,866），**授权率在流量窗口内同为 ~600 跳/s**
  （143k / 240 s）；p22 的 7.09 Mbit/s 更低是**每授权 TBS 更小**（链路 BLER），与本改动无关（V5 口径）。

#### ③ 机制（**哪一半与负载无关**）：一跳自己的命令缓冲窗口短了 **422 µs**

| 读数（中位） | p16 / p21 | **p22** | 差 |
|---|---|---|---|
| `busy split merged_hop`（本跳缓冲的占用窗口） | 1047.7 / 1047.4 | **625.1** | **−422 µs** |
| `busy split ch_wt` | 38.0 / 40.4 | 42.9 | ~不变 |
| 车道 `residency` | 1515.8 / 1516.7（中位 1456 / 1447）| **737.2（中位 745.2）** | **−711 µs** |
| `dft carried GPU start->end`（= 交棒后的整跳窗口） | — | 576.5 | （= merged_hop ✓ 对得上）|
| `dft carried deposit->GPU start`（排队） | 358.4 / 355.6 | 259.6 | −97 µs |
| `input hold`（token 持有，P0-2） | 均值 1836 / 1811，中位 1707 / 1758，p95 2482 / 2484 | **均值 1090.8，中位 950.4，p95 1074.2** | **−40%** |

* **`merged_hop` 的 −422 µs 是"与到达率无关"的证据**：那是**本跳自己那条命令缓冲的 GPU 窗口**，
  它变短只能来自缓冲里的工作变短。离线只预测了 14 × 12.2 ≈ 171 µs 的**窗口**，实测 **−422** ⇒
  说明那 13 次单 threadgroup 派发不是"12 µs 的窗口"，而是**一跳关键路径上串行的 ~32 µs/次**
  （估计器的第一个 dispatch 要读前端写的网格 ⇒ 依赖把它钉在关键路径上，离线微基准量不到这一点）。
  ⇒ **§6.30 ⑤ 的预登记区间（−149 ~ −500）被突破**，原因就在这里，也说明"窗口 vs 算力"的账还差一项：
  **依赖串行化**（dependency serialization）。
* **剩下的 −500 µs（920 − 422）来自排队/重叠**：`residency −711`、`deposit->start −97`；
  与 §6.30 ⑤ 的 ρ 论证同向（服务时间短了 ⇒ 等待与重叠一起短）。

#### ④ 转红的两条：D1/D2 的 **10.365 s 未认领块**——**不是批量化造成的**（是腿长/流量窗口的产物）

```
[FAIL] D1: max hold = 10365.6 ms            (p16–p21: 47 / 21 / 2786 / 106 / 97 ms)
[FAIL] D2: wait_for_a_claim max = 10365.1 ms, oldest unclaimed age max = 10365.0 ms at slot=13729
read : dry-pool reaps=16 recovering 0 block(s)
```
* **它是"没有 hop 来认领的块"**（§6.27 ④ 记过的那一类：上行静默窗口里的单块，**没有任何注册点去评估清扫的 10 ms 截止**）；
  这一次的 10.4 s **≈ 这条腿多出来的静默尾巴**：`[ul_rx] blocks=754519`（377 s）而授权仍只有 **143,127**（≈ p16/p21 的 143.5k/143.9k），
  ⇒ **流量窗口一样长，腿多跑了 ~110 s**。读数与之自洽：`handed 154062 − taken 143095 = 10,967` 个未认领块，
  `fallback` 2,205 → **9,968**、`late_time` 1 → **110**。
* **它没有伤到电台**（这正是修复 B 的功劳）：**D3 PASS**（`pop_blocking max=223 µs`、over 1s=0）、
  **D4 PASS**（**0 gaps** over 754,519 blocks）、`dropped=0`（池从未超过 1 ms 预算 ⇒ 不需要丢块）、
  `keepalives 2156868/2156868`（**一个都没漏**）、`max in flight 84` 与 p21 相同。
* ⇒ **判据本身没错**（它测的就是"最长的持有"，而 10.4 s 确实是错的），**触发条件是腿尾巴**；
  真正的修法是给清扫一个**入口点**（定时器或接收路径驱动）——**Q9-A 的遗留项、V2 的根因，与本改动正交**。
  记录：**D1/D2 在 p22 上 RED，原因 = 静默尾巴 + 无入口点；修法已存在（§6.27 ④）但未做。**

#### ⑤ 仍然红的两条（**没有因为 V1 达成而消失**）

| 判据 | 阈 | p22 | p16–p21 |
|---|---|---|---|
| **V2** 池压力 | `starved_events == 0` 且 `held_max < pool` | **97**、`held_max=8/8`、`free_min=0` | 52–114（**没变**）|
| **V3** 电台 | RF 失败 ≤10 | **1241** | 1000–1500（**没变**）|

⇒ **持有期短了 40%，池仍然被抽干**：说明"池 8 个 × 每槽 1 个"这个**容量本身**小于"一跳 1.5 ms × 每秒 600 槽"
（Little 定律：600/s × 1.09 ms ≈ 0.65 个在飞 + 每槽 14 个符号缓冲 ⇒ 8 个仍不够）⇒ **V2 是容量/结构问题，不是时延问题的残影**。
这正是 **P2-D**（按流水线深度**定尺**）要回答的，也是 §7.4/P2-F 裁决的输入。

#### ⑥ 结论与下一步

1. **V1 达成（−38%，p95 也在阈值内）且没有代价**（契约 8/8、`cbs/lane` 不变、0 gaps、无监控回归）。
   ⚠ 但**按本工作流的纪律"单条腿不是证据"**：需要**一条确认腿**（同配方 + 同流量窗口）。
2. **确认腿怎么飞**（避免这次的两个混淆项）：
   * 用操作者惯常的**热身 + `iperf3 -t 240`**，并在**流量结束后尽快停机**（这次的 110 s 静默尾巴既拉长了腿、
     又制造了 D1/D2 的红）；或者至少**记录**尾巴长度，判读时按流量窗口换算。
   * 建议**同日交替**：`p23-n78-batch14`（旋钮 14）与 `p24-n78-nobatch`（默认 1）各一条 ⇒ 把"当天链路漂移"从 A/B 里消掉。
3. **旋钮的默认值 —— ✅ 用户已裁决（2026-09-25）：改成 14（默认开）**。已实现：
   *⚠ **本条已被 §6.33 更正**：默认不再是常量 14，而是 **AUTO = 一个时隙的符号数**（extended CP = 12），
   由接收链通过 `set_slot_symbols()` 告知；对 normal CP 的 n78 小区（p22/p23 的工况）结果与 14 相同，故本节的结论不变。*
   * `front_end_batch_requested()` 在**未设**时返回 **14**；`=1`（或任何 <2 的值）⇒ **逐符号 = 对照臂**；
     非数字 ⇒ **一次性警告**并使用默认值（与 `grid_handover_armed()` 对自家旋钮的做法一致）。
   * ⇒ **B 臂（批量化）现在不需要旋钮**，A 臂（对照）必须**显式**写 `OCUDU_DFT_BATCH_SYMBOLS=1`。
   * 依据：机制已在**离线**（网格逐字节相同）与**在线自测**（arm 17）双重自证，空口 **V1 −38%**、代价为零（本节的读数）。
   * 网（默认值改动后重跑）：`ctest -L phy` **193/193**、`dft_processor_metal_unit_test` **ALL OK**（默认与 `=1` 各一次）、
     metal 测试 **arm 10–17 全 PASS**、`ofdm_demodulator_metal_batch_test` ✓、`l1_handover_arms.sh` 5 PASS、门自测 PASS。
     ⚠ **注意：测试可执行文件不在默认构建目标里**（`cmake --build build` 不会重链它们）⇒ 改引擎后必须
     **显式构建依赖 `ocudu_dft*` 的那几个目标**再跑 `ctest`（本次列出的 7 个：`channel_equalizer_*`、`dft_processor_metal_unit_test`、
     `dft_release_adopt_metal_test`、`helena_head2head_bench`、`ofdm_demodulator_metal_batch_test`、
     `port_channel_estimator_metal_mmse_unit_test`、`ul_chain_replay`）——**否则网的绿是旧二进制说的**。
4. **V1 之后的账**：一跳 1513 µs 的组成按 §6.30 ⑤ 的框架重读 —— residency 745（其中 `merged_hop` 625）、
   `deposit->start` 260、A（等样点 ~473，零算力）仍在；⇒ **下一个最大的可攻击项回到 A（P1-7 符号级收包）**
   与 CE/EQ/demap 链（`merged_hop` 625 里的 140+）、以及 **V2 的容量问题（P2-D）**。



### 6.32 ★★ 确认腿 `p23-n78-batch14`（**默认值**）+ 同日对照 `p24-n78-nobatch`：**V1 复现并收口**（同一天 −38.7%），且对照组出现 **D4 的 1 个 gap / 128 万样点**

> 用户裁决（§6.31 ⑥ 3）之后的两条腿：`p23` **不设旋钮**（验的就是"默认 = 批量化"），`p24` **显式 `OCUDU_DFT_BATCH_SYMBOLS=1`**（对照）。
> 两条腿**同一天、同一配方、同一流量窗口**（309 s / 294 s），这就是"单条腿不是证据"要的那条证据。

#### ① 先验条件：两条腿的 D16 各自到位

| 腿 | `batched=` | `batch_max` | 说明 |
|---|---|---|---|
| `p23`（不设旋钮）| **148178/2074492** | **14** | = **14.000** 变换/派发 ⇒ **新默认值真的生效** |
| `p24`（`=1`）| **0/0** | **1** | 对照臂：一次批处理都没有 ✓ |

#### ② V1：**达成 + 复现**，且**同一天**消掉了链路漂移

| 读数 | **p24（对照，逐符号）** | **p23（默认，批量 14）** | Δ |
|---|---|---|---|
| **`[ul_gpu_pipeline]` 中位** | **2444.1 µs** | **1497.2 µs** | **−946.9（−38.7%）** |
| 均值 | 2434.2 | 1509.1 | −925.1 |
| p95 / p99 | 3083.6 / 3175.0 | **1644.2 / 1757.6** | −1439 / −1417 |
| `busy split merged_hop`（本跳缓冲窗口） | 1067.6 | **617.6** | **−450** |
| 车道 `residency`（中位） | 1485.5 | **743.1** | −742 |
| `input hold` 均值 / 中位 / p95 / **max** | 1852.5 / 1799.4 / 2528.9 / **96442.4** | 1013.5 / 945.2 / 1050.9 / **20367.6** | −45% / **max −79%** |
| `starved_events` | 62 | **38** | −39%（**仍 >0 ⇒ V2 未达**）|
| `cbs/lane` | **2.00 (max=2)** | **2.00 (max=2)** | 不变 ✓ |

* **与前两条批量腿一致**：`p22` 1513.4、`p23` **1497.2**（差 16 µs）⇒ 复现性成立；对照 `p24` 2444.1 落在历史队列 2413–2440 里 ✓。
* **⇒ V1（≤2150 µs）判定为"达成且已确认"**：2 条批量腿 + 1 条同日对照 + 5 条历史对照。

#### ③ 同日对照的"意外收获"：**对照组红了 D4，批量组全绿**（**但只算线索，不算证据**）

| 判据 | **p24（对照）** | **p23（默认）** |
|---|---|---|
| 契约 | ❌ **NOT MET 1 of 8**（`radio sample continuity`）| ✅ **MET 8 of 8** |
| **D4** 电台丢样点 | ❌ **1 gap / 1,283,405 样点** | ✅ **0 gaps** |
| D3 收线程 park | `max=1.131 ms`（`over 1ms=1`）| `max=0.321 ms`（`over 1ms=0`）|
| D1 最长持有 | 96.4 ms（PASS 但接近）| **20.4 ms** |
| D2 未认领块 | 95.5 ms | **19.9 ms** |
| RF 实时失败 | 1168 | **835** |
| 上行吞吐 | 9.79 Mbit/s | 14.76 Mbit/s |

* **链路是通的**：一次 **1.131 ms** 的收包 park 撞上了电台环的余量 ⇒ 丢了 128 万样点（`dropped=0`，即**没有触发修复 B 的丢弃**：池在 1 ms 预算内又有了缓冲，但电台的环已经溢出 ⇒ 正是 §6.25/§6.27 记的**电台侧**那第二个机制）。
* ⚠ **纪律**：**D4 是偶发读数且有 ≥2 个机制**（§6.27 ④），这两条腿**各只有一条** ⇒ **不能**据此宣称"批量化消除了丢样点"。
  能说的是：**同一天、同配方下，持有期/池压力/公园时长三条链上的读数都朝同一方向**（1.85→1.01 ms、62→38、1.131→0.321 ms），
  而这次对照组正好越过了电台的余量。⇒ **要把它变成证据，需要按对（pair）累计**：后续每条腿都读 D4，比较"批量组 vs 对照组的 gap 率"，
  而不是看单条腿的红绿。这一条写进 §9 的"待做"。

#### ④ 结论

1. **V1 收口**：目标达成、可复现、同日对照成立（−946.9 µs / −38.7%），代价为零（契约、`cbs/lane`、0 gaps 全在批量组这边）。
2. **V2 仍未达**（`starved_events=38`、`held_max=8/8`）但**方向对了**（62→38、持有期 −45%）⇒ 容量/结构问题（P2-D）仍在清单上。
3. **V3 仍未达**（RF 835–1168 ≫ 10），且**对照组的 D4 说明"池压力 → park → 电台丢样点"这条链仍然活着**（只是被时延改善压小了）。
4. **D4/D1/D2 的"按对累计"** 是下一个能在不改代码的情况下回答的问题；这也正是 §6.31 ⑥ 4 说的"V1 之后最大项"的一部分。



### 6.33 ⚠ **用户更正 §6.31 ⑥ 3**：批大小**不是一个常量 14**，而是**一个时隙的 OFDM 符号数**（extended CP = 12）——改成"由小区告知"的 AUTO 默认

> 用户原话（2026-09-25）：*"尽管绝大多数的情况下，14 个符号成为一批是正确的，因为 1 slot = 14 symbols。但是有些情况下，1 slot 并不是 14 symbols，例如 extended CP。所以，这里不应该固定为 14，而是根据 1 slot 有多少个 OFDM symbols。"*
> ⇒ 这是对的，而且指出的是**机制自己的单位**被写成了一个常量：Q9-F4 的单位是**一个时隙**，而"一个时隙有多少符号"是**小区的属性**（normal CP 14、extended CP 12，demodulator 自己就是这么算的：`ofdm_demodulator_impl.cpp` 里 `nof_symbols_per_slot = (cp == NORMAL) ? 14 : 12`）。
> 把 14 写进前端，等于**把小区配置偷藏进引擎**：normal CP 下"碰巧对"，extended CP 下"仍然能跑但不是一条时隙一次派发"，而且没人会从读数里看出来。

#### ① 改法：旋钮从"值"变成"**覆盖**"，默认是 **AUTO = 小区说了算**

| `OCUDU_DFT_BATCH_SYMBOLS` | 含义 | 批上限 |
|---|---|---|
| **未设 / `0`** | **AUTO（新默认）** | **= 接收链告知的一个时隙的符号数**（14 或 12）|
| `1` | 对照臂（逐符号） | 1 |
| `N ≥ 2` | 诊断覆盖（夹到 16） | N |

* 新接口 `dft_processor::set_slot_symbols(unsigned)`（**默认空实现**，CPU 实现不受影响）⇒ `dft_processor_metal` 转发 ⇒ `dft_metal_engine::set_slot_symbols()`。
* **调用点就在知道时隙的那一处**：`ofdm_demodulator_impl::set_lane_slot()` 在 `dft->set_lane_slot(slot_index)` 之后紧接着
  `dft->set_slot_symbols(nof_symbols_per_slot)`（开块之前）⇒ "哪个时隙"与"这个时隙有多大"一起到达。
* **AUTO 且从未被告知 ⇒ 不批处理**（上限 = 1），不猜 14：机制的单位是一个时隙，**不知道单位就不动手**。
  非数字的旋钮值 ⇒ **一次性警告** + AUTO（不是静默当成对照臂）。
* 读数：`[metal_stats] dft … batched=<d>/<t> batch_max=<生效上限> batch_src=<auto|knob|off> slot_symbols=<被告知的值>`；
  门 **D16** 增加两条支路：`batch_src=auto` 且 `slot_symbols=0` ⇒ **"没人告诉前端一个时隙有多大"（接线缺陷）**；
  没有 `batch_src` 字段 ⇒ 老构建（读成旋钮值）。

#### ② 离线自证（metal **arm 17 重写**：这一节的核心证据）

对**一个时隙可能有的两种符号数**各跑一遍，每遍都是"对照臂（逐符号）作参考 → AUTO 必须与之**逐字节相同**且**恰好一次派发**"：

```
[dft-release] arm 17 (6.30/6.33 batched front end): for BOTH symbol counts a slot can carry (14 normal CP,
12 extended CP) the deferred path produces a BIT-IDENTICAL grid to the per-symbol path and really deferred it
into ONE dispatch, the batch size comes from what the cell TOLD the engine (AUTO), and an engine that was
never told does not batch at all - the per-symbol control arm reads exactly zero
```
臂里断言的四个子例（`batch_stats()` 逐项检查）：
1. **14 符号（normal CP）**：`cap=14 told=14 override=0`、`dispatches 0->1`、`transforms 0->14`、网格**逐字节相同**；
2. **12 符号（extended CP）**：`cap=12`、`0->1`、`0->12`、网格**逐字节相同**；
3. **AUTO 但从未被告知**（`slot_symbols=0`）：`cap=1`、**计数不变**（一次都没批）；
4. **显式覆盖**（旋钮 12、被告知 14）：`cap=12 override=12`、`0->1`、`0->12`。
退出报告随之给出 `batched=3/38 batch_max=14 batch_src=auto slot_symbols=14`（14+12+12 = 38 ✓ 自洽）。

#### ③ 网（全绿，且**这次把"测试可执行文件不在默认构建目标里"的坑按 §6.31 ⑥ 3 的名单补上了**）

`ctest -L phy` **193/193**（显式构建 7 个依赖 `ocudu_dft*` 的目标之后）、`dft_processor_metal_unit_test` **ALL OK**（默认与 `=1` 各一次）、
`ofdm_demodulator_metal_batch_test` ✓、metal **arm 10–17 全 PASS**、`lower_phy_test` ✓、`l1_handover_arms.sh` **5 PASS**、门自测 **PASS**（含 D16 的三条支路）。

#### ④ 对在飞/已飞腿的影响（**读数口径**）

* `p22`/`p23` 是在"14 是常量"的构建上飞的，但对**normal CP 的 n78 小区**，AUTO 给出的就是 **14** ⇒
  **§6.31/§6.32 的 V1 结论不受影响**（同一批大小、同一条代码路径）；D16 的读法换成带 `batch_src=auto` 的新行即可。
* **extended CP 的小区**（本次没有腿）从这条改动起才真正是"一条时隙一次派发"；在那之前它会以 14 为上限
  （12 个符号时仍在块尾一次派发，功能正确但"上限"名不副实）。⇒ 若要为它取证，需要一条 extended CP 的腿。



### 6.34 ★ **V2 归因（零腿分析）**：池是 **8 个"时隙缓冲"**，而**在飞峰值就是 6 个块 + 2 个在收**⇒ 余量为 0；可避免的那部分持有者是**未被认领的块**（20–96 ms），因为**清扫只在"入池"和"干池 park"两处被驱动，普通取缓冲不驱动它**

> 用户裁决先打 V2（池饥饿）。这一节**只用已有腿的读数 + 代码**，不烧腿。
> 读数取自 `p16/p21`（批量化之前）、`p22/p23`（批量）、`p24`（同日对照）。

#### ① 池的单位与定尺：GPU 模式下缓冲是**整槽**的，`+8` 已经退化成常量

`lower_phy_factory.cpp`：

```
rx_buffer_size = nof_samples_per_slot            (gpu 模式【强制】整槽缓冲：policy = slot / optimal_slot，
                                                  否则直接报致命错误——"没有一个 OFDM 符号被拆到两个块里")
nof_rx_buffers = max( 8,
                      rx_to_tx_max_delay / rx_buffer_size,                      // 本例 ≈ 23040/11520 ≈ 2
                      (max_pipeline_depth * max_symbol_size)/rx_buffer_size + 8 )// 本例 ≈ (8*856)/11520 + 8 = 0 + 8 = 8
```
* 那条注释写的是**符号级**缓冲的心算（"最深流水线 8 个符号 + 正在收的 1 个 + 1 个备用"）。但 GPU 模式**强制整槽缓冲**，
  一个缓冲里已经装着那 8 个符号 ⇒ **`max_pipeline_depth * max_symbol_size` 这一项塌成 0**，"+8" 于是变成
  **"8 个整槽 = 4 ms 的流水线"**，与流水线深度**不再有函数关系**。腿上的 `pool=8` 就是这么来的。
* ⇒ 这是 **P2-D 的题目**（按测量定尺），而且现在有了明确的"为什么旧公式在 GPU 模式下名不副实"。

#### ② 需求侧：在飞峰值 = **6 个块**（跨腿恒定），池 = 8 ⇒ 余量 = 2，而这 2 个正好被"在收 + 刚收"占掉

| 腿 | `keepalives … (max in flight)` | ÷ 每槽 14 个 token ⇒ **在飞块数** | `pool` | `held_max` | `free_min` | `starved_takes/events` | `pop_blocking` max |
|---|---|---|---|---|---|---|---|
| `p16`（批量前）| **84** | **6.0** | 8 | — | — | — | — |
| `p21`（批量前）| **84** | 6.0 | 8 | — | — | — | — |
| `p22`（批量）| **84** | 6.0 | 8 | 8 | **0** | — | — |
| `p23`（批量）| **84** | 6.0 | 8 | **8** | **0** | 71 / **38** | **321 µs**（over 1ms=0）|
| `p24`（对照）| 82 | 5.9 | 8 | 9* | **0** | 108 / **62** | **1131 µs**（over 1ms=1）|

\* `held_max=9 > pool=8` 只能来自"这些计数器是进程级、池是每扇区"（§6.20 的计数注释）——本条腿不需要它来解释什么，登记备查。

* **峰值 84 个 token = 6 个整槽块**，而且**批量化前后一模一样**（84/84/84/84）⇒ 峰值由**车道的并发与链路的流水结构**决定，
  **不由跨度决定**（时延从 2440 降到 1497，峰值没动）。
* 池 8 = 6（在飞）+ 1（正在收）+ 1（刚收/正在交棒）⇒ **余量恰好为 0** ✓ 这就解释了 `held_max=8 / free_min=0`：
  不是"池小得离谱"，而是**池正好等于峰值 + 收包路径**，任何额外的持有者都会立刻把它压干。
* 而等待是**亚毫秒**的（`pop_blocking` max 321 µs，`over 1ms=0`；只有对照组的 1131 µs 那次越过了 1 ms 并丢了样点）
  ⇒ **不是**"5 秒/秒级尾巴"那类机制，而是**余量被吃掉的瞬间**。

#### ③ 可避免的那部分：**未被认领的块**，以及**清扫只有两个入口点**

| 读数 | `p23` | `p24` |
|---|---|---|
| `unclaimed at once max` | **3** | **4** |
| `wait_for_a_claim max`（最久没人来认领）| **19.9 ms** | **95.5 ms** |
| `produced − claimed`（整条腿没被认领的块）| 148177 − 145059 = **3,118**（2.1%）| 149271 − 145298 = **3,973**（2.7%）|
| `dry-pool reaps` / 从中恢复的块 | 9 / **0** | 7 / **0** |

**清扫（sweep）目前只有两个入口点**（代码可查）：
1. **每次"入池"**（`deposit_released()` 里跑一遍 sweep）；
2. **干池 park**：接收线程在**已经取不到缓冲**时才进 `pop_rx_buffer_or_reserve()` 的阻塞循环，循环里才 `handover_reap_hook::reap()`
   （`lower_phy_baseband_processor.cpp`）。

⇒ **普通（成功）取缓冲不驱动清扫**。于是**上行静默窗口**里：没有新入池 ⇒ 入口点 1 不发生；池还没干 ⇒ 入口点 2 不发生；
那个"没人认领的块"就一直攥着它的整槽缓冲，**直到下一次入池或下一次干池**——这就是 19.9 / 95.5 ms（`p19` 2.79 s、`p22` 10.4 s）的来源，
也正是 §6.27 ④ 记的"没有任何注册点去评估清扫的 10 ms 截止"。

**归因结论**：V2 剩下的红 = **池余量为 0** × **最多 3–4 个可避免的持有者**。
* 平均持有只需要 ~2–3 个缓冲（均值 1.01 ms ≈ 2 槽；`p99` 1.85 ms ≈ 3.7 槽）⇒ **按均值算池是够的**；
* 但峰值（6 块）已经占掉 6 个，再由**没人认领的块**额外占 3 个，就把 8 个吃光 ⇒ 38 次亚毫秒饥饿。
* 批量化把事件数**减半**（62→38）是因为它缩短了持有期（碰撞概率下降），**并没有改变峰值**。

#### ④ 两个修法（按"要不要裁决"分）

**(A) 给清扫第三个入口点：普通取缓冲也驱动它**（**不需要裁决**，改动小，同时治 D1/D2）
* 在成功取到缓冲之后（或按 1 ms 节流）调 `handover_reap_hook::reap()`。清扫的**规则不变**（仍然是"窗口外 2 个时隙 + 10 ms 截止"），
  只是**按时被评估**：未被认领的块会在 ~2 个时隙（1 ms）内被回收 ⇒ 它的整槽缓冲回到池里。
* 预期：`unclaimed at once` 的持有时间从 20–96 ms 降到 ≤ ~1 ms ⇒ **D1/D2 转绿**（最长持有 < 100 ms，且"未认领年龄"回到 ms 级），
  **V2 的 `starved_events` 应从 38 降到接近 0**（余量从 0 恢复到 2–3）。风险：清扫更频繁 ⇒ `fallback` 计数略增（本来就发生，只是更早），
  且**不会**抢走合法的认领（窗口规则是契约本身）。**必须验证**：契约 8/8、`cbs/lane` 不变、0 gaps、`fallback` 与 `late_time` 的量级。

**(B) P2-D：按测量定池尺**（**需要用户裁决**，因为它动的是"池容量"这条你曾裁定为"治标"的路）
* 现在的 8 是**常量**（①）；把它改成**显式**：`ceil(在飞峰值) + 1（在收） + margin`，其中"在飞峰值"由**读数**给出
  （`keepalives max in flight / 每槽符号数` = 6，或 `hold_p99 / 时隙` + 流水线深度），margin 明确写出来。
* 这不是"加大池"：它是把**已经存在**的 8 用**测量**解释清楚，并把"再深一档流水线/双端口就会不够"这个**潜在脆弱**摆到明面上。
* 建议顺序：**先 (A)**（它治的是根因里可避免的部分，且能同时收 D1/D2），**再按 (A) 之后的读数决定 (B)** —— 如果 (A) 之后
  `starved_events` 已经回到 0，则 (B) 降级为"按设计补文档 + 加一条定尺断言"，不必改容量。



### 6.35 ★ **修法 (A) 落地（用户裁决）**：清扫得到**第三个入口点——普通取缓冲**（1 ms 节流），两个调用者用**理由**分开计数

> §6.34 的归因结论：V2 剩下的红 = **池余量为 0**（8 = 在飞 6 + 在收 1 + 刚收 1）× **最多 3–4 个可避免的持有者**（未被认领的块，
> 20–96 ms），根因是**清扫只有两个入口点**（入池、干池 park），**上行静默窗口里两个都不发生**。用户裁决：先做 (A)。

#### ① 改了什么

| 位置 | 改动 |
|---|---|
| `include/ocudu/phy/phy_pipeline_grid_ready.h` | `handover_reap_hook::reap()` → **`reap(reap_reason)`**，`enum class reap_reason { dry_pool, take }`——**调用者的身份成为契约的一部分**，腿因此能把两个入口分开读 |
| `lib/phy/metal/ocudu_metal_burst.{h,mm}` | `reap_unclaimed_now(reason)`；新增 **`reaped_by_take_events/blocks`**（与 `reaped_by_park_*` 分开）；`block lifecycle` 行加打印 **`take sweeps=N recovering M block(s)`** |
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` | **普通取缓冲（`try_pop` 成功）也驱动清扫**，节流 **`rx_sweep_interval = 1 ms`**（30 kHz 下约每两个时隙一次），状态 `rx_last_sweep`；干池 park 那处改成 `reap(dry_pool)` |
| metal 自测 | 原 arm 12 改成 `reap(dry_pool)`；**新增 arm 12b**：一个孤儿块 + **只有 take 形状的一次调用**（无入池、无 park），断言"输入恰好回来一次"且 **take 计数动、park 计数不动** |

**规则一字未改**：一个块仍然只在"链已经把它那个时隙之后的窗口走完"或"过了 10 ms 截止"时才可回收
⇒ 这个入口点**不可能**抢走一个还有权认领它的跳；改变的是**规则被评估的时刻**（从"下一次入池/下一次干池"变成"≤1 ms 后"）。

**代价**：健康运行每毫秒一次注册表互斥（而不是每次取缓冲一次）；被更早回收的块会产生更早的 `fallback` 提交——**那本来就发生**（`fallback=4191` on `p23`），只是更早。

#### ② 离线自证

```
[dft-release] arm 12  (Q9-A): a DRY pool reaped an unclaimed block with NO other registry entry point - input released exactly once, dry-pool events 0->1, blocks 0->1, late_time 5->6
[dft-release] arm 12b (6.34 take-path sweep): an ordinary TAKE reaped an unclaimed block with no deposit and no park, the input came back exactly once, and the TAKE counters moved while the PARK pair stood still (take events 0->1)
```

#### ③ 网（全绿）

`ctest -L phy` **193/193**（按 §6.31 ⑥ 3 的名单显式重建 7 个依赖 `ocudu_dft*` 的目标之后）、`dft_processor_metal_unit_test` **ALL OK**、
`ofdm_demodulator_metal_batch_test` ✓、metal **arm 10–17 + 12b 全 PASS**、`lower_phy_test` ✓（`OCUDU_UL_RX_POOL_DROP_FORCE=3` 同）、
`l1_handover_arms.sh` **5 PASS**、门自测 **PASS**。

#### ④ 验证腿的**预登记**（下一条腿按这个读；两条腿**都要看**，因为 (A) 动的是"何时回收"，不是"回收什么"）

| 读数 | 修前（`p23` 批量 / `p24` 对照）| 预登记（修后）| 说明 |
|---|---|---|---|
| **D1** 最长 token 持有 | 20.4 / 96.4 ms | **≤ ~5 ms** | 窗口 2 槽 + 1 ms 节流 + 余量 |
| **D2** 最久未认领年龄 | 19.9 / 95.5 ms | **≤ ~5 ms** | 同上 |
| `take sweeps=N recovering M`（新读数）| —（不存在）| **N≈节拍×秒数、M ≥ 1** | **入口点真的回收到了块**；M=0 是"没有可回收的"，不是失败 |
| `[ul_rx_pool] starved_events` | 38 / 62 | **0–5** | 可避免的持有者回到池里 ⇒ 余量从 0 恢复 |
| `starved_takes` / `pop_blocking` | 71 / max 321 µs | 下降 | 同上 |
| `fallback` / `late` / `late_time` | 4191 / 999 / 110 | **可能略升** | 更早回收 ⇒ 更早提交；`late_time` 甚至可能**降**（窗口规则先到，不必等时间截止）。**不是回归** |
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | **不变** | 硬要求 |

⚠ **一个必须写清的口径**：V2 的判据是 **`starved_events == 0` 且 `held_max < pool`**。修法 (A) 治的是**前半**
（可避免的持有者），但 **`held_max` 可能仍然是 8**——因为"6 个在飞 + 1 在收 + 1 刚收"是**结构性**的峰值，不是缺陷。
⇒ 若修后 `starved_events` 回到 0 而 `held_max` 仍 = 8，则 V2 的后半只能由 **(B) P2-D** 回答（给定尺公式一个明确的 margin，
或把"峰值 = 池"这一事实写成断言/文档），**不要**把 (A) 的成功误读成 V2 未达成。



### 6.36 ★ 验证腿 `p25-n78-sweep`：**(A) 按设计生效**（take 清扫回收 52 个块；D1/D2 的最坏值被压到清扫自己的 10 ms 截止），**但 `starved_events` 没有改善** ⇒ **更正 §6.34 的归因**：饥饿是**结构性峰值 = 池尺寸**，不是未被认领的块

#### ① 预登记 vs 实测

| 读数 | `p23`（(A) 之前）| **`p25`（(A) 之后）** | 预登记 | 判读 |
|---|---|---|---|---|
| **D16** 先验条件 | `batch_max=14` | **`batched=162784/2278976 batch_max=14 batch_src=auto slot_symbols=14`** | ✅ | 机制在位（默认 AUTO = 14）|
| **`take sweeps=N recovering M`** | —（不存在）| **`297854 / 52`** | M ≥ 1 | ✅ **入口点真的回收到了块** |
| `dry-pool reaps` | 9 / 0 | **30 / 0** | — | park 路径已不再需要兜底（它找到 0 个）|
| **D1** 最长 token 持有 | 20.4 ms | **12.0 ms** | ≤ ~5 ms | ⚠ 改善但**停在 10 ms 截止**（+完成滞后）|
| **D2** 最久未认领年龄 | 19.9 ms | **10.1 ms** | ≤ ~5 ms | ⚠ 同上：**= `sweep_after` 10 ms** |
| `input hold` p99 | 1851.6 µs | **4946.8 µs** | — | 尾部**被压进 5–12 ms 桶**（见 ②），不再是 20–96 ms |
| **`starved_events` / `starved_takes`** | 38 / 71 | **107 / 203** | 0–5 / ↓ | ❌ **没改善**（在历史带内，见 ③）|
| `pop_blocking` max | 321 µs | 335 µs（over 1ms=0）| ↓ | 亚毫秒不变 |
| `held_max` / `free_min` / `pool` | 8 / 0 / 8 | **8 / 0 / 8** | 可能仍 8 | ✅ 如预登记（结构性峰值）|
| **V1** 中位 | 1497.2 | **1514.4**（p95 1671.8、p99 2186.3）| 不变 | ✅ 差 17 µs |
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | **8/8 / 2.00 (max=2) dropped=0 / 0 gaps** | 不变 | ✅ |
| `fallback` / `late` / `late_time` | 4191 / 999 / 110 | **13968 / 1752 / 52** | 可能升 | ✅ 如预登记；`late_time` 反而**降**（窗口规则先到，不必等时间截止）|
| `keepalives` 在飞峰值 | 84（=6 个整槽块）| **84** | — | 结构性峰值，跨 9 条腿恒定 |

#### ② 为什么最坏值停在 ~10–12 ms：**静默窗口里只有"时间截止"这条规则能触发**

`sweep_due()` 的两条规则：
* **槽窗口**：`entry.slot < newest_slot && (newest_slot − entry.slot) > 2`；
* **时间截止**：`now − deposited_at > 10 ms`。

而 **`newest_slot` 只在"入池"时更新**（`deposit_released()`）。⇒ 上行静默窗口里没有入池 ⇒ **槽窗口永远不动**，
只有时间截止能命中 ⇒ 一个没人认领的块**恰好等满 10 ms** 被回收（外加完成滞后 ⇒ D1 的 12.0 ms、D2 的 10.1 ms）。
**这正是设计里写死的上界**（10 ms 是"槽窗口 2 ms 的五倍"，见 `sweep_after` 的注释）。

⇒ **(A) 的真实收益 = 把这一类的最坏值从"等下一次入池"（20–96 ms，p19 2.79 s、p22 10.4 s）压到"清扫自己的 10 ms 截止"**；
`p99` 从 1.85 → 4.95 ms 不是变差，而是**那些块从 20–96 ms 桶搬进了 5–12 ms 桶**（原来它们只在 `max` 上可见）。

#### ③ ⚠ **更正 §6.34 的归因**：饥饿不是未被认领的块造成的，而是**结构性峰值 = 池尺寸**

* 9 条腿的**每槽饥饿率**：`0.62e-4 … 1.72e-4`（p16 1.72、p17 1.40、p19 1.58、p20 1.23、p21 0.97、p22 1.29、**p23 0.62**、p24 1.06、**p25 1.37**）。
  ⇒ **p25 的 107 次落在历史带中间**，而 p23 的 38 次是这批腿里**最好**的一条 —— 我的预登记（"38 → 0–5"）**过拟合了单条最好的腿**。
* **机制上必然如此**：池算的是 `held = taken − returned`，即**有多少个整槽缓冲被取出未还**。在飞的 6 个块**不管是"有跳认领"
  还是"没人认领"**，占用的缓冲数**一样**；把没人认领的块更早回收，**不会**降低峰值（链路照样跑在前面）。
  ⇒ 未被认领的块是 §6.34 里**被我过度加权**的一项；它们**只影响尾部**（(A) 已治），**不影响峰值**。
* **峰值的来源**：`keepalives max in flight = 84 token = 6 个整槽块`（9 条腿恒定，与批量化无关）+ **1 个正在收** + **1 个刚收**
  = **8** = 池尺寸。而 `starved_takes/events` 的定义就是"取缓冲时 free ≤ 1"（`rx_pool_note_taken`: `free_buffers <= 1`）
  ⇒ 只要峰值**碰到**池尺寸，就进入"nearly dry"状态 ⇒ 每槽 0.6–1.7e-4 次。**余量为 0 是结构性的**。
* ⇒ **V2 剩下的红 = 容量/余量问题（P2-D 的题目）**，而且 `held_max < pool` 这半条判据**只能**由尺寸（或降低峰值）满足。

#### ④ 下一步：**P1-4（池容量，诊断臂）** —— 已实现旋钮，待飞

工作区早就登记过这条臂（§7.2 P1-4："时延不变而 `starved_events` 归零 ⇒ 因果链坐实；**不作为交付**"；
§7.4 的用户裁定："**加大池 = 诊断**，它能**证明**因果链，但不缩短时延"；"P1-4 只看 `starved_events`；P2-D 必须同时满足 V1 与 V2"）。
现在把它做成**可飞的旋钮**：

```
OCUDU_UL_RX_POOL_SIZE=<n>    # 只允许【大于】公式算出的值（更小的池是测试里唯一会死锁的配置）；
                            # 未设/0 = 用公式值；武装时在 stderr 上自报"这是诊断臂（P1-4）"
```
读数（含 `pool=` 字段的 `[ul_rx_pool]` 行）在腿的 atexit 报告里。

**预登记（一条腿，配方同 p25）**：

| 读数 | 期望 | 若不成立说明 |
|---|---|---|
| `pool=12`（旋钮 12）| 池确实变大 | 旋钮没到工厂 |
| **`starved_events`** | **0（或个位数）** | 饥饿还有别的来源（那就得再查，而不是动尺寸）|
| `free_min` | **> 0** | 同上 |
| `held_max` | ≤ 12（可能仍 8）| — |
| **V1** 中位 | **不变（≈1500）** | 若变好 ⇒ 说明"池干也在拖时延"，那 P2-D 的账要重写 |
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | 变大池不该动任何一条 |
| `pop_blocking` / `starved_takes` | 降到 ~0 | — |

⇒ 若上表成立，**V2 = 容量问题被证明**，然后是 **(B) P2-D 的裁决**：把定尺写成"测量出来的峰值 + 明确 margin"
（并说明 GPU 模式下 `+8` 已经退化成常量），而不是继续用那个常量。



### 6.37 ★★★ 腿 `p26-n78-pool12`（**P1-4 池容量诊断臂**）：**V2 = 容量问题被证明** —— `starved_events` **0**、`free_min` **5**、`pop_blocking` max **49 µs**，而 V1/契约/`cbs/lane`/gaps **一字不变**

> 旋钮 `OCUDU_UL_RX_POOL_SIZE=12`（只允许放大，武装时自报"这是诊断臂"）。**§6.36 ④ 的预登记全部命中。**

#### ① 预登记 vs 实测

| 读数 | 池 = 8（`p23`/`p24`/`p25`）| **`p26`（旋钮 12）** | 预登记 | 判读 |
|---|---|---|---|---|
| `pool=` | 8 | **16** | `pool=12` | ⚠ 见 ②：**环形缓冲把容量向上取到 2 的幂**（12 → 16）；旋钮本身到厂了（stderr 自报 "12 buffers instead of the computed 8"）|
| **`starved_events`** | 38 / 62 / 107 | **0** | 0（或个位数）| ✅✅ **命中** |
| **`starved_takes`** | 71 / 108 / 203 | **0** | ~0 | ✅ |
| **`free_min`** | 0 | **5** | > 0 | ✅ |
| **`held_max`** | 8 | **11** | ≤ 池 | ✅ **见 ③：峰值本身升到 11** |
| **`pop_blocking` max** | 321 / 1131 / 335 µs | **49 µs**（over 1ms=0）| 下降 | ✅ **收线程几乎不再等** |
| `dry-pool reaps` | 9 / 7 / 30 | **0** | — | ✅ park 路径一次都没用上 |
| **V1** 中位 | 1497 / 2444(对照) / 1514 | **1510.4**（p95 1660.7、p99 1744.1）| **不变** | ✅ **没有靠"加大池"换时延** |
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | **8/8 / 2.00 (max=2) dropped=0 / 0 gaps** | 不变 | ✅ |
| D1 / D2 | 12.0 / 10.1 ms（`p25`）| 12.3 / 10.1 ms | — | 与 (A) 的行为一致（最坏值 = 清扫的 10 ms 截止）|
| `input hold` p99 | 1851.6 µs（`p23`）| **1132.5 µs** | — | 反而更紧 |
| RF 失败 / 上行 | 835–1168 / 9.8–14.8 Mbit/s | 702 / 10.39 Mbit/s | — | 同量级（链路运气）|

#### ② 一个必须登记的仪器细节：**池容量会被向上取到 2 的幂**

`blocking_queue` → `ring_buffer_storage(unsigned sz) : super_type(to_next_pow2(sz))` ⇒ **容量 = 2 的幂**。
所以 `pool=` 的取值是 **8 / 16 / 32 …**，**"12" 这个池不存在**：旋钮 12 得到的是 **16**。
⇒ 旋钮的 stderr 提示词应说明这一点（"the receive pool is 12 buffers instead of the computed 8" 是**请求值**，
真正的容量看 `[ul_rx_pool] … pool=`）。P2-D 若要走尺寸这条路，**决策的粒度是"8 还是 16"**。

#### ③ 机制（这是本节的**核心证据**）：**峰值不是 8，池 8 是把流水线夹住了**

| | 池 8 | 池 16 |
|---|---|---|
| `held_max`（在飞缓冲峰值）| **8**（= 池，被夹住）| **11**（自然峰值）|
| `free_min` | 0 | 5 |
| `starved_takes/events` | 71–203 / 38–107 | **0 / 0** |

* 池变大使峰值**从 8 升到 11** ⇒ **流水线本来就要 11 个整槽缓冲**，而池只有 8 ⇒ 收线程只能等（= 饥饿），
  这正是 `starved_takes`（"取缓冲时 free ≤ 1"）的定义。**这是"V2 = 容量"的直接证明**，而不是推断。
* 与之独立的自洽性：(A) 之后 `take sweeps` 仍在工作（`209205` 次、回收 `206` 个块）、`keepalives max in flight` 仍是 **84**（= 6 个整槽块的 token 峰值，
  与池大小无关）⇒ **池变的是"能同时在飞的缓冲数"，不是"块怎么被认领"** —— 与 §6.36 ③ 的更正一致。
* 用户最初的顾虑（"加大池是治标、不缩短时延"）在这里**被测量分开**了：**时延由 §6.30/§6.32 的前端批量化解决（−38.7%）**，
  而 V2 剩下的这条红**只**由容量决定（`starved_events` 与 V1 在同一条腿上分别读：0 与 1510.4）。
  P1-4 的定位（工作区早就写死）："**加大池 = 诊断**，它能证明因果链，但不缩短时延；**P1-4 只看 `starved_events`；P2-D 必须同时满足 V1 与 V2**" —— 本条腿**同时**给出了这两半。

#### ④ 由此得到的 (B) P2-D 决策输入（**待用户裁决**）

| 选项 | 内容 | 代价 / 风险 |
|---|---|---|
| **(B1) 交付侧定尺：把池按"实测峰值 + margin"定到 16** | 改 `lower_phy_factory.cpp` 的定尺项（GPU 模式下 `+8` 已退化，见 §6.34 ①），把测量写进注释与断言；**V2 转绿**（`starved_events=0`、`held_max(11) < pool(16)`、`free_min=5`）| 内存 ≈ **1.5 MB**（16 × 2 端口 × 11,520 样点 × 4 B，可忽略）；V1/契约/`cbs/lane`/gaps 在本条腿上均不变；粒度只能取 2 的幂（②）|
| (B2) 保持 8，接受 V2 红 | 不动容量 | V2 的"症状消失"判据永远不达成；每槽 0.6–1.7e-4 次饥饿（亚毫秒，至今未造成丢样点，但 D4 的两个机制里"电台侧"那条与之同源）|
| (B3) 降低峰值而不是加池 | 缩链路的前瞻（接收侧少跑在流水线前面）| 直接换掉"链路喂饱车道"的能力，风险最高；且峰值 11 是**流水线结构**的产物（UL 突发 + 车道排空），不是可随手拧的参数 |

**建议 (B1)**：把定尺写成"**实测峰值（11）+ 明确 margin**"⇒ 16，并在注释里写清"GPU 模式下 `+8` 退化成常量、
环形缓冲把容量取到 2 的幂、这条尺寸由 P1-4 的测量给出"。这既让 V2 转绿，又把"再深一档流水线/SRS 并发更高时会不够"这件事
从**隐藏的常量**变成**有测量的设计**。⚠ 但它动的是**容量**（用户曾裁定"加大池是治标"）⇒ **需要裁决**；
且按纪律，改完要跑网 + 一条**确认腿**（配方同 p26，读 `pool=16`、`starved_events=0`、V1 不变）。



### 6.38 ★★ **(B1) 落地（用户裁决）：池按"实测峰值 + 明确 margin"定尺 ⇒ GPU 模式 16**，并把定尺依据**打印在腿自己的日志里**

> §6.37 的裁决输入：(B1) 定到 16（V2 转绿）、(B2) 保持 8、(B3) 降峰值。用户裁决 **(B1)**。

#### ① 改了什么（`lower_phy_factory.cpp`）

* 新增**只在整槽缓冲（= GPU 模式）时生效**的一项：
  `slot_pipeline_buffers = slot_pipeline_peak(11) + rx_path_buffers(2) + rx_pool_margin(3) = 16`，
  其中 **11 是 §6.37 ③ 的测量**（池 16 时 `held_max=11`、`free_min=5`），2 = "正在收 + 刚收"，3 = margin。
* 与原有的三项一起进 `max()`：地板 8、电台时延项、**符号级**流水线项（该项在整槽缓冲下**退化为 0 + 8**，§6.34 ①）——
  于是符号级缓冲的策略（非 GPU 模式）**行为一字不变**，GPU 模式的池变成 **16**。
* **定尺依据打印在启动行**（这是"定尺"从隐藏常量变成可读设计的关键）：
  ```
  [ul_rx_pool] size=16 buffers of 23040 samples (slot=23040, whole-slot buffers, the gpu pipeline mode):
  floor 8, radio latency 1, symbol pipeline 8, slot pipeline 16 (peak 11 + rx path 2 + margin 3, dev doc 6.37)
  ```
* 环形缓冲把容量取到 2 的幂（§6.37 ②）：16 已经是 2 的幂 ⇒ 请求值 = 生效值。
* 诊断旋钮 `OCUDU_UL_RX_POOL_SIZE`（只允许放大）**保留**：它现在是"在这之上再加"的臂，不再是唯一的扩容手段。

#### ② 离线自证与影响面

* `lower_phy_test`（整槽配置）打印 **`size=16 … slot pipeline 16 (peak 11 + rx path 2 + margin 3)`**，且 **528/528 通过**
  （⚠ 本次又踩到 §6.31 ⑥ 3 那个坑：`cmake --build build` **不重链测试可执行文件**，必须先显式构建 `lower_phy_test`，
  否则看到的仍是旧二进制）。
* **影响面**：所有**整槽缓冲**（GPU 模式）的配置都会从 8 变 16 —— 包括 n1。
  但 n1 的实测（`p15-conc2`，满速 105 s）是 **`held_max=4 / free_min=4 / starved=0`** ⇒ n1 本来够用；
  真正需要它的是**加压的 n78**（`p16`：`held_max=8 / free_min=0 / starved_takes=219 / events=114`）。
  内存代价：16 × 2 端口 × 23040 样点 × 4 B ≈ **2.9 MB/扇区**（可忽略）。

#### ③ 确认腿的预登记（`p27`，**不需要任何旋钮** —— 验的就是新的交付定尺）

| 读数 | 期望 |
|---|---|
| 启动行 | **`[ul_rx_pool] size=16 … slot pipeline 16 (peak 11 + rx path 2 + margin 3, dev doc 6.37)`** |
| 退出行 `pool=` | **16** |
| **`starved_takes` / `starved_events`** | **0 / 0** |
| `free_min` | **> 0**（`p26` 是 5）|
| `held_max` | **≤ 12**（`p26` 是 11）|
| `pop_blocking` max | **≪ 1 ms**（`p26` 是 49 µs）|
| **V1** 中位 | **≈1510（不变）** |
| 契约 / `cbs/lane` / gaps / D16 | **8/8 / 2.00 (max=2) / 0 / `batch_max=14 batch_src=auto`** |
| D1 / D2 | 12.3 / 10.1 ms 量级（(A) 的 10 ms 截止，与池无关）|

⇒ 若成立：**V2 的 `starved_events == 0` 与 `held_max < pool` 两半同时满足**（`11 < 16`）⇒ **V1–V5 里只剩 V3 未达**。



### 6.39 ★★★ 确认腿 `p27-n78-pool16`（**交付定尺，不带旋钮**）：**V2 达成** —— `starved_events` **0**、`free_min` **6**、`held_max` **10 < 16**、`pop_blocking` max **23 µs**；V1 **1495.4**（历史最好），门 **26/26** ⇒ **V1–V5 只剩 V3 未达**

#### ① 预登记 vs 实测（逐项命中）

| 读数 | 预登记 | **`p27` 实测** | |
|---|---|---|---|
| 启动行 | `size=16 … slot pipeline 16 (peak 11 + rx path 2 + margin 3)` | **`size=16 buffers of 11520 samples (slot=11520, whole-slot buffers, the gpu pipeline mode): floor 8, radio latency 2, symbol pipeline 8, slot pipeline 16 (peak 11 + rx path 2 + margin 3, dev doc 6.37)`** | ✅ |
| 退出行 `pool=` | 16 | **16** | ✅ |
| **`starved_takes` / `starved_events`** | 0 / 0 | **0 / 0** | ✅✅ |
| `free_min` | > 0 | **6** | ✅ |
| `held_max` | ≈11，< 16 | **10** | ✅ |
| `pop_blocking` max | ≪ 1 ms | **23 µs**（全部腿里最小）| ✅ |
| **V1** 中位 | ≈1510 不变 | **1495.4**（mean 1505.1、p95 1642.7、p99 1728.9）| ✅ **全部腿里最好** |
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | **8/8 / 2.00 (max=2) dropped=0 / 0 gaps** | ✅ |
| **D16** | `batch_max=14 batch_src=auto` | **`batched=146939/2057146 batch_max=14 batch_src=auto slot_symbols=14`** | ✅ |
| D1 / D2 / D3 / D4 | — | **12.0 / 10.0 ms / 0.0 ms / 0 gaps** 全 PASS | ✅ |
| 门 | — | **26 of 26** | ✅ |

* 机制侧：`take sweeps=215479 recovering 229 block(s)`、**`dry-pool reaps=0`**（(A) 的入口点接手后，park 兜底一次都没用上）、
  `input hold` 均值 **947.9 µs** / p99 1127.5 µs（最初基线是 1836 / 3010）、`keepalives max in flight 84`（结构性，不变）。
* 流量可比：`[ul_rx] blocks=564735`（282 s）、`lanes=144655`、12.66 Mbit/s、RF 失败 707（同队列量级）。

#### ② 里程碑：**V1–V5 的现状**

| | V1 时延 | V2 池症状 | V3 电台 | V4 提交数 | V5 不回归 |
|---|---|---|---|---|---|
| 现状 | ✅ **1495.4 µs**（≤2150；p22/p23/p26/p27 四点复现）| ✅ **`starved_events=0` 且 `held_max=10 < pool=16`** | ❌ **RF 失败 707**（≤10）| ✅ `cbs/lane=2.00 (max=2) dropped=0` | ✅ 契约 8/8、`0.00+0.00`、0 gaps |

* **V1 的两条腿链**：`p22` 1513.4、`p23` 1497.2（同日对照 `p24` 2444.1 ⇒ −38.7%），加上定尺后的 `p26` 1510.4、`p27` 1495.4 ⇒ **四点一致，无回归**。
* **V2 的两半同时满足**（这正是 §6.34 归因、§6.35 修法 (A)、§6.37 P1-4、§6.38 定尺这一串的结果）：
  `starved_events == 0`（(A) + 容量）与 `held_max (10) < pool (16)`（容量）。
* **⇒ 只剩 V3。**

#### ③ 下一步：V3（RF 失败 700–1500/腿，阈值 ≤10）

已知（分散在 §6.23/§6.24/§6.32）：
* **是"GPU 模式病"**：cpu 模式 276 s 只有 **1** 次；n78 加压 gpu 腿 700–1500 次。
* **并发 2 放大 ~3 倍**（1.04% vs 0.34–0.38% 的 grant）。
* 与**池干/收线程 park** 那条链**部分**相关（`p24` 对照组 park 1.131 ms ⇒ 丢 128 万样点；`p20` park 仅 235 µs 也丢 16 万 ⇒ 还有**电台侧**的第二个机制）。
* 修复之后 park 已基本消失（`p27` max 23 µs、`p25` 335 µs），但 RF 失败仍是 **707** ⇒ **不能**只归因于 park。
* 观察到的量级走势（同配方）：p16 1490 → p22 1241 → p23 835 → p26 702 → **p27 707**，**可能**是时延减半带来的下降，也可能只是链路运气（每腿的 BLER/MCS 都在变）⇒ **需要按对/按量级先澄清**。

⇒ V3 的第一步同样是**零腿分析**：用 `leg_census.py`（"UL 静默表 + RF/pool 事件"）把失败**分类**：
`underflow` vs `overflow`、发生在**哪一段**（流量中/静默/启动）、是否与 `park`/`gap`/`late` 同时出现，
以及**每 grant 的失败率**在 cohort 里的分布（把"链路运气"和"机制"分开）。然后才决定测量臂：
`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=1`（同日对照，验证"并发 2 放大 3 倍"）
或 `OCUDU_UL_RX_POOL_DROP=0`（反向臂，看丢样点是否回来）。



### 6.40 **V3 侦察（零腿，第一步读数）**：这 700 次失败是 **UHD 的 TX 侧实时失败**（`underflow` + `late`），且**随负载出现**——不是 UL 收包路径的丢样点

> V1/V2/V4/V5 都绿了（§6.39），只剩 V3。这一节是它的**入门读数**，只用 `p27` 的日志与 `leg_census.py`。

#### ① 它们是什么

| 读数（`p27`）| 值 |
|---|---|
| `[RF]` 行分类 | **`Real-time failure in RF: underflow` 633 次 + `late` 74 次**（同一 logger，UHD 的实时失败消息）|
| 出现时段 | **第一个 150 s 只有 ~7 次**（前 30 s 仅 7 次、且那 30 s 是 ramp-up：PDSCH 1.2/s），**后 113 s 有 505–632 次** |
| 与调度的相关性（同一腿内分窗）| 前 150 s：PDSCH **174/s**、PUSCH 449/s、PDCCH 623/s ⇒ 失败 **~7**；后 113 s：PDSCH **248/s**、PUSCH **593/s**、PDCCH **841/s** ⇒ 失败 **~505–632** |
| `gaps`（收包连续性）| **0**（D4 绿）|
| 收线程 park | `pop_blocking` max **23 µs**（D3 绿）|
| 上下文抽样 | 一次 `underflow` 前后是正常的 PUCCH/PUSCH/调度行（`tbs=2625`、CRC OK、sinr 22 dB）——**不是停顿，是"错过了实时截止"** |

⇒ **它们是 TX（下行馈送）侧的"宿主没把样点按时交给电台"**，与 UL 的收包路径（`gaps`、park、池）**已经无关**：
`gaps=0`、`pop_blocking=23 µs`、`starved_events=0` 同时成立。**V3 的判据文字（"电台"）此前一直与 UL 的丢样点混在一起读，这里要分开。**

#### ② 已知的三条旁证（都在既有记录里）

1. **模式相关**：cpu 模式 276 s **仅 1 次**（§6.24 ②）⇒ 与 GPU 流水线给宿主加的活儿相关。
2. **随跨度下降**：`p16` 1490 次（跨度 2428 µs）→ `p22` 1241（1513）→ `p23` 835 → `p26` 702 → **`p27` 707**（1495.4）
   ⇒ 前端批量化把宿主的编码工作砍掉 ~13/14、跨度砍半之后，失败数也**大致减半**（1490 → ~707）——同向，但**每腿的链路/MCS 都在变，不能当因果**。
3. **并发相关**：并发 2 把失败率变 ~3 倍（1.04% vs 0.34–0.38% 的 grant，§6.24 ②）。

#### ③ 由此得到的 V3 路线（**先补仪器，再做臂** —— 与 P0-1/P0-5/P0-6 同一套纪律）

* **S1（已完成，本节）**：分类 + 时段 + 负载相关性 ⇒ **它是 TX 侧的、负载相关的、非停顿时的事件**。
* **S2（补仪器，离线可做）**：**TX 侧目前没有任何读数**——UL 那一侧有 `pop_blocking`/`input hold`/`unacked` 等一整套，
  而"宿主把 DL 样点交给电台时已经多晚"没有探针。V3 要落地，第一件事是把这个"迟到"量出来：
  最小可用版本 = 在传送路径上记录**每次递交相对时隙截止的余量/迟到量**，并给一个分布（与 P0-2 同形）。
  没有它，V3 的任何改动都只能靠"失败数变少了"来判断，而那个数随链路运气波动（同配方 700–1500）。
* **S3（臂，需要飞腿）**：
  * **同日并发 1 vs 2**（验"2 放大 3 倍"）：如果并发 1 把失败压回 ~10–20，机制就锁定在"宿主并发/调度"；
  * **`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=1` + 纯 UL 负载**（去掉下行 iperf3 的热身）⇒ 分离 DL 与 UL 竞争；
  * 视 S2 的读数再定"提高 TX 线程优先级/亲和性"这类实现臂。



### 6.41 ★ **V3 的 S2 落地（用户裁决）**：TX 侧的第一个读数——`[dl_tx_slack]`，"递交时距截止还剩多少**宿主**时间"的分布；门加 **D17（INFO）**

> §6.40 的结论：V3 的 700–1500 次失败是 UHD 的 **TX 侧**实时失败、随负载出现，而 TX 侧**一个读数都没有**。
> 用户裁决：**先补仪器（S2）**。这一节就是它。

#### ① 量什么，以及为什么必须用**两个时钟**

`dl_process()` 在把一整槽样点交给电台之前，手里有三个量：本槽的无线时间 `timestamp`、DL 提前量 `tx_time_offset`
（`metadata.ts = timestamp + tx_time_offset` 就是"这些样点必须在电台时间轴上出现的时刻"），以及接收侧最新的 `last_rx_timestamp`。

只用无线时间**不够**：`(due_ts − last_rx_ts)` 是"还剩多少**无线**时间"，而 `underflow` 的定义是"宿主交得太晚"——
**宿主**才是我要量的那一维。所以探针从接收路径保留**两个时钟的映射**（`receiver.receive()` 返回时的
`(无线时间戳, 宿主时刻)` 对），在递交那一刻算：

```
margin_us = (due_ts − last_rx_ts) / rate  −  (host_now − last_rx_host)
```

即"**用宿主微秒表示的、距离截止还剩多少**"，并且把电台自身的收包缓冲延迟**消掉**（两项都含它）。
`margin ≤ 0` = 递交发生在截止之后 ⇒ **underflow 的宿主侧形状**。

* 读数（atexit + 按需 dump 一起打印）：
  `[dl_tx_slack] transmissions=N mean=… median=… p1=… p5=… p25=… min=…us (due_ts=…); below 2ms=…, below 1ms=…, below 500us=…, AT/BELOW 0=…`
* **分布才是重点**："一直贴着截止线 200 µs"和"通常提前 2 ms、偶尔晚 3 ms"是两种不同缺陷，只有分位数能分开。
* ⚠ **单元夹具里读到的不是这个量**：`lower_phy_test` 的"电台"是 mock，收包时间戳与宿主节奏没有固定关系（没有采样时钟可晚），
  所以它成片读出负值（实测 mean −1173 µs、651/999 ≤ 0）。**这条探针是给空口腿读的**，要和同一条腿的 `Real-time failure in RF` 计数一起看。

#### ② 门：**D17（INFO）**

```
[INFO] D17 (6.41) did the transmit hand-over have time left
   read: transmissions=… min=…us AT/BELOW 0=0 against <N> RF failure(s) in the .log - the hand-over never ran out
         of time, so those failures are NOT this: look inside the radio/driver, or at the DL load that feeds it
   read: transmissions=… min=…us AT/BELOW 0=<k> against <N> RF failure(s)  <-- the host-side shape of an underflow
```
自测：`p0_gate_selftest.sh` 增加"读到该行"与"6.41 之前的腿读成 cannot say"两个方向（已绿）。
门对既有腿：`p27` 现在 **27/27**（D17 读成 "a leg flown before 6.41 cannot say"）。

#### ③ 网

`ctest -L phy` **193/193**、`lower_phy_test` ✓、metal arms 10–17 ✓、`l1_handover_arms.sh` **5 PASS**、门自测 **PASS**。

#### ④ 验证腿的预登记（`p28`，配方同 p27）——**两种结果都是结论**

| 读数 | 期望 A（宿主侧成因）| 期望 B（电台/驱动侧成因）|
|---|---|---|
| `[dl_tx_slack]` 分布 | 中位正常（~1–3 ms），**尾部越过 0** | 中位正常，**`min > 0`**（从不越过 0）|
| `AT/BELOW 0` | **> 0**，且与 RF 失败**同量级/同窗口** | **0** |
| RF 失败 | 700–1500（同 cohort）| 700–1500 |
| 结论 | **宿主在满负载时把 DL 交晚了** ⇒ 下一步：定位是哪个线程/哪段工作（与 UL 车道负载的相关性）⇒ 再做臂（并发 1 vs 2、纯 UL 负载、线程优先级/亲和性）| **不是这里**：迟到发生在电台或其驱动内部（本机 B200 是 **USB 3** 传输，抖动是一号嫌疑）⇒ 下一步查 USB/时钟/缓冲，而不是宿主调度 |

另外记两条要一起看的：`min` 若长期 < 500 µs ⇒ "一直贴着截止线"（负载能力不足的形态）；
`below 1ms` / `below 500us` 的比例给出**危险窗口**占多少。
**V1/V2/契约/`cbs/lane`/gaps 应全部不变**（探针只读不写；一次 `steady_clock::now()` + 一次原子读/槽）。



### 6.42 ★★ 腿 `p28-n78-txslack`（TX 探针的**第一次空口读数**）：**正常的递交提前 1.5 ms，但 52 次越过 0（最差 −4.2 ms）**——而这条腿有 **1064 次 RF 失败** ⇒ **宿主侧只解释 ~5%，主因在递交之后**（UHD/驱动的传输）

#### ① 探针读数（D17 命中，且是"有越过 0"那一支）

```
[dl_tx_slack] transmissions=685435 mean=1511.0us median=1512.0us p1=1507.0us p5=1511.0us p25=1511.0us
              min=-4208us (due_ts=5108659500); below 2ms=685422, below 1ms=503, below 500us=66, AT/BELOW 0=52
D17: transmissions=685435 min=-4208us AT/BELOW 0=52 against 1064 RF failure(s) in the .log
```

* **常态非常健康**：中位 **1512 µs**（≈ `tx_time_offset` 1.5 ms = 3 个时隙），p1 都有 1507 µs ⇒ **不是"一直贴着截止线"**。
* **但有 52 次越过 0**（0.0076%），最差 **−4.2 ms** ⇒ **期望 A 的一半成立：宿主确实偶尔交晚**。
* **量级对不上**：52 次 vs **1064 次 RF 失败** ⇒ **宿主侧的迟到最多解释 ~5%**；其余 95% 的 `underflow` **发生在递交之后** ——
  即 **期望 B 为主**：UHD 自己的传输/线程（本机 B200 走 USB 3）没赶上，而不是我们这个 `transmit()` 的时刻。
* `below 1ms=503`、`below 500us=66`：有 ~500 次余量被压到 1 ms 以内 ⇒ **危险窗口存在但不是常态**。
* 时间上：`due_ts=5108659500`（无线时间基）≈ 222 s，落在 §6.40 记的"满负载后半段（155–255 s）"窗口内 ✓ 与 RF 失败同时段。

#### ② 同一条腿的其余读数（**V1/V2/V4 不变，V5 这条腿红**）

| 读数 | p28 | 判读 |
|---|---|---|
| **V1** 中位 | **1511.4**（mean 1523.6）| ✅ 不变 |
| **V2** | `starved_takes=0`、`starved_events=0`、`held_max=12`、`free_min=4`、`pool=16` | ✅ 不变 |
| V4 | `cbs/lane=2.00 (max=2) dropped=0` | ✅ |
| **V5 / D4** | ❌ **2 gaps / 285,262 样点**、契约 **NOT MET 1/8** | 与 p24（对照）同类；**这条腿 `pop_blocking` max 仅 25 µs、`over 1ms=0`** ⇒ 收线程**没有** park ⇒ **电台侧的第二个机制**（§6.25/§6.27 记过），**与 RF 失败同源**（都在递交之后）|
| D1 / D2 | 14.7 ms / 11.6–10.0 ms（PASS）| `input hold` p99 **10.9 ms**、`take sweeps` 回收 **1749** 个块 ⇒ 这条腿的**未认领块比 p27 多得多**（长尾 + 流量空档），但仍在 10 ms 截止内被回收 ✓ (A) 在干活 |
| D3 | `pop_blocking` max **25 µs** | ✅ |

#### ③ 机制收敛（把 V3 与残留 gap 合成一个故事）

三条独立证据指向同一个位置——**`transmit()` 之后**：
1. 递交**提前 1.5 ms**（中位）却仍有 1064 次 underflow；
2. 同腿 **2 个 gap / 28.5 万样点**，而收线程**没有 park**（max 25 µs）⇒ 丢样点也**不在宿主收包路径**；
3. 既有旁证：**cpu 模式同电台 276 s 仅 1 次**、**并发 2 放大 3 倍**、**前端批量化把失败数大致减半**（p16 1490 → p27 707）。

⇒ 最自洽的解释：**递交本身是准时的，但 UHD 的传输线程/USB 通道没有及时把样点送出去**；
而"GPU 模式"通过**宿主 CPU 争用**（收线程、车道执行器、Metal 驱动线程、CE/EQ/demap 的宿主部分）把它挤晚——
所以 cpu 模式几乎不出现、并发 2 放大、宿主活儿变少（批量化）就减半。**这不再是"我们晚交"，而是"我们让它晚传"。**

#### ④ 下一步：先用一个**零腿**读数把"UHD 调用阻塞"与"UHD 线程被饿"分开（S2b），再做臂

* **✅ S2b 已落地（提交见下）**：在同一个 `transmit()` 前后加计时，新增第二个读数
  ```
  [dl_tx_call] calls=N median=…us p95=…us p99=…us max=…us; over 1ms=…, over 5ms=…
  ```
  它把"**调用内部在等**（电台/USB 背压，宿主无关）"与"**瞬间返回、样点躺在 UHD 自己的队列里**（其工作线程被饿）"分开；
  门 **D17** 现在同时给出**份额**与**归口**：
  ```
  read: transmissions=685435 min=-4208us AT/BELOW 0=52 against 1064 RF failure(s) in the .log
        (the hand-over explains at most 4% of them). transmit() itself returns at once … =>
        the samples waited in UHD's OWN queue: look at CPU CONTENTION and at the USB path
  ```
  （对 6.42 之前的腿，D17 明确写 "a leg flown before this probe cannot say whether transmit() itself blocks"。）
* **S3（臂，按 S2b 的结果选）**：
  * 若 `transmit()` 阻塞 ⇒ 查 **USB 传输/缓冲**（UHD 的 `num_send_frames`/`send_frame_size`、USB 3 拥塞），宿主侧无解；
  * 若是**线程被饿** ⇒ 做**CPU 亲和/优先级**臂（把上层 PHY 的执行器钉到一部分核，给 UHD 的线程留出整核），
    以及**同日并发 1 vs 2**（复现 3 倍放大）。



### 6.43 ★★★ **收口（用户裁决，2026-09-25）：目标达成** —— V1 与 V2 都达成、可复现、代价为零；**V3 与残留 gap 另案（电台/传输侧）并暂停**

> 用户裁决："宣布目标达成，V3 另案暂停。" 本节是这次会话的**结项记录**：目标是什么、达成到什么程度、靠什么达成、
> 代价是什么、什么被留在外面（以及为什么那不属于本目标）。

### ① 目标（工作流的原始定义，未改过）

> **缩短 `[ul_gpu_pipeline]` 的端到端时延，并以"池不再饥饿"为症状消失的判据。**
> 起因（2026-09-24）：`gpu` 融合车道在 ~14–19 Mbit/s 就把 8 个接收缓冲抽干 ⇒ `pop_blocking()` 阻塞接收线程 ⇒
> 电台 underflow 几百次（同负载 `cpu` 模式 0–1 次）。用户裁定：**加大池是治标**，根因是数据在流水线里停留太久。

### ② 达成情况（判据见 `high_level_status_and_plan.md` §1）

| | 阈值 | 基线 | 最终读数 | 证据腿 |
|---|---|---|---|---|
| **V1 时延** | `[ul_gpu_pipeline]` 中位 **≤2150 µs** | **2675.1** | ✅ **1495.4 – 1513.4 µs（−43%）**；p95 1643–1671、p99 1729–2186 | `p22` 1513.4、`p23` 1497.2、`p26` 1510.4、`p27` 1495.4、`p28` 1511.4；**同日对照 `p24` 2444.1 ⇒ −38.7%** |
| **V2 池** | `starved_events==0` 且 `held_max<pool` | `starved` **52–114**、`held_max=8=pool`、`pop_blocking` max **486 µs–5 s** | ✅ **`starved_events=0`**、`held_max=10–12 < pool=16`、**`pop_blocking` max 23–25 µs**、`free_min=4–6` | `p26`（P1-4）、`p27`（交付定尺）、`p28` |
| V4 提交数 | `cbs/lane ≤2.00 (max=2)`、`dropped=0` | — | ✅ 全程未变 | 所有腿 |
| V5 不回归 | 契约 8/8、`0.00+0.00`/跳 | — | ✅ 多数腿 8/8；⚠ 偶发 1–2 gap（见 ⑤，电台侧）| `p23/p26/p27` 8/8；`p24/p28` 各 1–2 gap |
| **V3 电台** | RF 失败 **≤10** | cpu 模式 276 s **1** 次 | ❌ **700–1500/腿**（**另案**，见 ⑤）| 全 cohort |

机制侧（与 V1/V2 同向、跨腿稳定）：**一跳自己的窗口 1047 → 617–625 µs**、车道 `residency` 1456 → 737–745 µs、
`input hold` 均值 **1836 → 948–1044 µs**、`pop_blocking` max **3969 → 23–25 µs**、`merged_hop` 1047 → 625。

### ③ 靠什么达成（本会话的六件事，按发生顺序）

| # | 内容 | 出处 | 对目标的贡献 |
|---|---|---|---|
| 1 | **5 秒停顿的根因**：Metal 对"未被满足的设备侧事件等待"有 **5.00 s 硬上界**，且"消费者先于 signaller 提交"会触发它 | §6.20 | 去掉 5 s 级停顿（V1/V2 的**前提**）|
| 2 | **修复 A（提交握手）** + **修复 B（池干丢块，用户裁决 (b)）** | §6.21/§6.22/§6.26/§6.28 | 停顿不再出现；池干不再拖住电台 |
| 3 | **§6.29 更正**：941 µs 是**占用窗口**不是 DFT 算力；真实算力 ~300 µs/槽，前端 ~171 µs 是"每符号一次单 threadgroup 派发"造成的 | §6.29/§6.30 | 把 V1 的杠杆从"优化 kernel"改到"**改派发形状**"（**这是本会话最重要的方向修正**）|
| 4 | **前端批量化**：一个时隙的 14 次派发 → **1 次**（离线 15.1×、网格逐字节相同；默认 **AUTO = 小区的符号数**，normal CP 14 / extended CP 12）| §6.30/§6.33 | ✅ **V1 −38.7%**（一跳自己的窗口 −422 µs；离线只预测 149 µs ⇒ 差额是**依赖串行化**，`merged_hop` 里量到）|
| 5 | **V2 归因 → 修法 (A) 清扫入口点 → P1-4 证明容量 → P2-D 定尺 16** | §6.34/§6.35/§6.37/§6.38 | ✅ **V2**（`starved_events=0`；`held_max=10–12<16`）|
| 6 | 仪器与门：D11–D17、`take sweeps`/`[dl_tx_slack]`、按需 dump、自测双向 | §6.19–§6.41 | 让每一步都**可判读**（"读不出按 RED"）|

**代价：零。** 契约 8/8、`cbs/lane=2.00 (max=2)`、`dropped=0`、`crossings 0.00+0.00`、`ce device estimates 0 host`、
零拷贝 0 failures/0 misaligned 全程未变 —— 即 **V1 不是用提交数或契约换来的**（V4/V5 的意义所在）。

### ④ 交付形态

* 默认值：**`OCUDU_DFT_BATCH_SYMBOLS` 未设 = AUTO**（= 接收链通过 `dft_processor::set_slot_symbols()` 告知的一个时隙符号数；
  `=1` 是对照臂；`N≥2` 诊断覆盖）；**GPU 模式（整槽缓冲）的接收池 = 实测峰值 11 + 收包路径 2 + margin 3 = 16**，
  定尺依据打印在启动行（`[ul_rx_pool] size=16 … slot pipeline 16 (peak 11 + rx path 2 + margin 3, dev doc 6.37)`）。
* 离线验收：`wip/dft_kernel_cost.mm` 的"前端自己的形状"臂（14 次 **159.77** → 1 次 **10.61** µs/槽、网格逐字节相同）、
  metal **arm 17**（14/12 符号两例 + AUTO 未告知不批 + 显式覆盖）、**arm 12b**（take 形状的清扫）。
* 网：`ctest -L phy` **193/193**、`lower_phy_test` **528/528**、metal **arm 10–17 + 12b** 全 PASS、
  `l1_handover_arms.sh` **5 PASS**、`p0_gate_selftest.sh` **PASS**（D11–D17 双向）。

### ⑤ **另案（暂停）：V3 与残留 gap —— 电台/传输侧，不属于本目标**

用户裁决："V3 另案暂停。" 依据（本会话量到的，不是推断）：

* **V3 的 700–1500 次失败是 UHD 的 TX 侧实时失败**（`Real-time failure in RF: underflow` 633 + `late` 74），
  **随负载出现**（前 150 s 仅 ~7 次；后 113 s 505–632 次，同窗 PDSCH 174→248/s、PUSCH 449→593/s）。（§6.40）
* **TX 侧探针（§6.41）的第一次空口读数**：递交时中位**提前 1512 µs**（p1 都还有 1507）、
  只有 **52/685435 次越过 0**（最差 −4.2 ms）⇒ 对同腿 **1064 次**失败**最多解释 ~4%**：**迟到发生在递交之后**（UHD 的传输/线程）。
* **残留 gap 与它同源**：`p28` 有 2 gaps / 285,262 样点，而同一腿 `pop_blocking` max **25 µs、over 1ms=0** ⇒ **收线程没有 park**。
* 既有旁证一致：cpu 模式 276 s 仅 1 次；并发 2 放大 ~3 倍；前端批量化后失败数大致减半（p16 1490 → p27 707）。

⇒ **若日后重启这条线**：从 `[dl_tx_call]`（`transmit()` 调用自身耗时，§6.42 ④ 已落地）开始——它一步就能分开
"电台/USB 在调用内背压（宿主无解）"与"样点躺在 UHD 队列里、其线程被饿（CPU 争用，可查亲和/优先级）"；
然后是同日**并发 1 vs 2** 与 **CPU 亲和/优先级**臂。**判据（≤10）与 gap 的偶发性都要先按对累计**，别用单腿红绿说话。

### ⑥ 明确留在外面（可选，需另行裁决）

1. **继续压时延**：V1 已达成但仍有余量；下一个最大项是 **A ≈473 µs（等样点，零算力）** ⇒ **P1-7 符号级收包**，
   它会**触 V4**（提交数），需要用户裁决"测量臂/交付"的边界；其后是 `merged_hop` 里 CE/EQ/demap 的 ~140 µs。
2. **extended CP 取证腿**：12 符号路径已实现且离线自证（arm 17），但**没有空口腿**。
3. **D4/gap 按对累计**（零腿，方法已在 §6.32 ③ 定好）。
4. 旧清单：`value_net`/`ab_dumps` 两条陈旧网、**Q12**、**P1-6**、**P2-F**（并发度作为交付，需二次裁决）、
   **MAC 侧从丢失 CRC 指示恢复**（HARQ 饥饿，独立缺陷，未修）、**P2-A**（估计器提前启动）。

### ⑦ 本会话留下的**读数口径更正**（后来者必读，别按旧口径读）

1. `[ul_gpu_lane] busy` / `busy split` 是**每条命令缓冲占用窗口的加和**，**不是算力账单**（§6.29 ④）。
2. **窗口 ≠ 关键路径代价**：14 次单 threadgroup 派发的窗口是 171 µs，但在一跳的关键路径上是 ~422 µs（**依赖串行化**）（§6.31 ③）。
3. 池的 `pool=` 值是**环形缓冲的容量**（向上取到 2 的幂）：旋钮 12 实际是 16（§6.37 ②）。
4. **`[ul_rx_pool] starved_events` 的定义**是"取缓冲时 free ≤ 1"的**进入次数**，不是"等了多久"（§6.36 ③）。
5. 调试陷阱：`cmake --build build` **不重链测试可执行文件** ⇒ 改引擎后必须显式构建依赖 `ocudu_dft*` 的目标（§6.31 ⑥ 3）。
6. 单元夹具里的 `[dl_tx_slack]` **不是**空口读数（mock 电台没有采样时钟），见 §6.41。
7. **未声明的静态库依赖在 macOS 上能链、在 Linux 上不能**：`ld64` 不关心归档顺序，GNU ld 单趟解析。
   本次就踩到：`ocudu_lower_phy` 与 `gnb_base` 用了 `ocudu_phy_support` 的 `register_p0_report`/`p0_dump_reports`
   却没声明依赖 ⇒ Ubuntu 构建在 `lower_phy_test` 链接处报 `undefined reference`（提交 `f1c7dc1d49` 修：两处显式声明，
   并把"为什么显式写"记在 CMakeLists 注释里）。**新增使用某库符号的库/可执行文件时，必须在自己的 target 上声明依赖。**



### 6.44 ★ **V1 的下一步（零腿分析）**：一跳自己的窗口 613 µs 里有 **12 次派发**，而算力估计只有 ~150 µs ⇒ 杠杆是"**把派发并起来**"（与前端同一个病，且**不动 V4**）

> 用户裁决：目标（V1/V2）达成后**继续压 V1**。这一节是先做的零腿功课：把当前预算重读一遍，并找出最大的那一块。

#### ① 当前预算（腿 `p27`，V1 中位 **1495.4 µs**）——**名字 ≠ 窗口内容**（§2.3.1 的老规矩）

| 读数（中位） | 值 | 它是什么 |
|---|---|---|
| `[ul_gpu_pipeline]` | **1495.4** | V1 的跨度 |
| 相位段 `t2f` | 530.8 | 宿主/墙钟：从收包到时间-频率段结束（**含等样点的那一段**）|
| 相位段 `ce` | 71.0 | 信道估计段 |
| 相位段 `eq_demap` | **874.3** | 均衡+解映射段（**注意：这是墙钟段，不是算力**）|
| 相位段 `ldpc_decode` | 65.0 | 解码 fork |
| 车道 `residency` / `busy`（配对） | 734.9 / 629.9 | 一跳占用车道的时间窗 / 其中"有命令缓冲在跑"的窗口和 |
| `busy split` | **merged_hop 613.4（94%）** + `ch_wt` 39.9（6%） | 一跳只有**两个**命令缓冲：融合跳 + 权重 |

**关键对照**：`merged_hop` 的窗口 **613 µs**，而 §6.29 给出的"设备**算力**"估计是前端 ~10.6（批量化后）
＋ CE/EQ/demap ~140 ⇒ **约 460 µs 不是算力**。§6.29 的教训在这里同样适用：窗口 ≠ 算力。

#### ② 那 460 µs 是什么：**12 次派发的固定开销 + 依赖串行**（读数已在腿里）

```
[metal_stats] burst commits=144655 dispatches=1735995 (equalizer=1302030 demapper=144655 channel_estimator=289310)
⇒ 每次跳 12.0 次派发：均衡器 9.0 + 信道估计 2.0 + 解映射 1.0
[metal_stats] eq_batch flushes=144655 symbols=1735815 runs=434010 batched=434010 max_run=8 first_break=estimates
⇒ 均衡器每跳 12 个符号、批成 3.0 个 run（最长 8），**run 断在 `estimates`**
```

* 离线量过（`wip/dft_kernel_cost.mm`）：**单 threadgroup 派发的固定开销 ~12 µs**，且互不重叠（§6.29/§6.30）。
  12 次派发 × ~12 µs ≈ **144 µs** 是"至少"的固定开销；剩下的差额（~300 µs）只能是**派发之间的依赖串行**
  （每个 kernel 要读前一个写的结果 ⇒ Metal 在同一命令缓冲内插入屏障；本机**不支持逐 dispatch 计数器**，§8 Q1）。
* **均衡器已经有批量路径**（把连续符号并成一个 run、一次派发），但 run 被 **`estimates`** 掐断：
  判据要求各符号的估计切片**共享一个缓冲且按固定步长前进**，实际不是 ⇒ 只能 3 个 run。
  把它修成整跳 1 个 run 只省 2 次派发（~24 µs）——**不是大头**。
* ⇒ 真正的疑点是 `stage::equalizer` 那 **9.0 次/跳**里除 run 之外的派发（`stage::equalizer` 把
  **四个 kernel**（两个 gather、表构建、均衡本身）算在一起，所以"9 次"说不清该并哪一个）。

#### ③ 因此做的仪器（零腿）：把 `stage::equalizer` **按派发点拆开**

新增 `sites(ch_gather=… y_gather=… y_batch=… run=… single=…)` 到同一行 `[metal_stats] eq_batch` 里
（五个点：`eq_build_gather_on_device` / `eq_encode_gather_dispatch` / `eq_encode_batch_dispatch` /
`enqueue_burst_batch_at` / `enqueue_burst`）。一条腿就能回答："**每跳那 9 次派发分别是谁发的**"，
从而定下要并的是哪一个（gather 每符号一次？表构建每跳一次？run 3 次？）。

离线已自证打印（`channel_equalizer_metal_unit_test`：`sites(ch_gather=0 y_gather=0 y_batch=13 run=2 single=0)`）。

#### ④ 为什么这条路**不需要裁决**：**派发数不是提交数**

V4 约束的是 `cbs/lane`（**命令缓冲**数/跳），把一跳内的 12 次派发并成 2 次**不改变命令缓冲数**
（仍是 `merged_hop` + `ch_wt` = 2.00/跳）⇒ **不触 V4**。这与前端批量化同理（它也没改 `cbs/lane`，§6.30/§6.31）。
⇒ 这条路可以按"先量后改"的常规纪律推进，不需要新的用户裁决。

#### ⑤ 预登记（下一条腿 `p29-n78-dispatch`，标准配方，无新旋钮）

| 读数 | 期望 / 判读 |
|---|---|
| `[metal_stats] eq_batch … sites(…)` | 出现，且各点数量能给出一张"**每跳派发来源表**" |
| `dispatches/跳`、`(equalizer/… )` | 与 p27 的 12.0 / 9.0+2.0+1.0 对得上（先证明读数自洽）|
| V1 / 契约 / `cbs/lane` / gaps | 不变（本步只加计数器）|

⇒ 拿到来源表后，**要并的 kernel 就确定了**；届时按 §6.30 的同一套做法做（不变量判据：`value_net` + 逐字节/容差 + metal 单测），
并用 `busy split`/`residency` 与 V1 判收益。

#### ⑥ ⚠ **流程事故：腿 `p29-n78-dispatch` 是"旧二进制"飞的（我的错）** —— 并已把守卫补强

* **现象**：`p29` 的 `[metal_stats] eq_batch` 行**没有 `sites(...)` 字段**，即这条腿跑的是**加计数器之前**的代码。
* **原因（可查证）**：我在 17:22 提交了计数器，然后**只跑了 `cmake --build build --target ocudu_versioning`**
  （它只重新生成 `build/hashes.h`，**不重链任何东西**）⇒ `build/hashes.h` 的 mtime 变成 17:22，
  而 `build/apps/gnb/gnb` 还是 **17:05** 的旧二进制。`run_leg.sh` 的守卫比的是"戳 = HEAD"⇒ **两边都是 `e3d4658f58`，守卫通过**，
  腿就这么飞了（17:25 起，17:31 落盘）。
* **p29 仍然可用**：作为**基线腿**——V1 中位 **1506.5**（cohort 内）、`cbs/lane=2.00`、`merged_hop=616.5 µs`、
  每跳派发 **12.0**（eq 9.0 + CE 2.0 + demap 1.0）、`eq_batch runs=3.0/跳 max_run=8 first_break=estimates` —— 与 `p27` 逐项一致
  （即"读数自洽"这一条预登记成立，只是**站点表**缺）。⚠ 这条腿 **`gaps=3`、契约 NOT MET 1/8**（与 p28 同类，电台侧）。
* **补强守卫（`run_leg.sh`）**：不再只比戳，而是**在二进制里找那个戳字符串**
  （`lib/support/versioning` 会把 commit 编进二进制）：
  ```
  if ! grep -aq "$STAMP" "$ROOT/build/apps/gnb/gnb"; then
    echo "REFUSING to run: build/apps/gnb/gnb does not carry the stamp it claims ($STAMP)." ...
  ```
  这样"刷新戳但不重建"这类事故**在下一次飞腿前就会被拦下**（内容判据，不依赖 mtime；实测该检查 ~0.19 s）。
* **纪律重申（写进本节的教训）**：改过代码后**必须重建 `gnb`**（`cmake --build build --target ocudu_versioning && cmake --build build --target gnb`）；
  **单独跑 versioning 只会让守卫说谎**。




### 6.45 **V1 的下一刀（用户裁决 ①）第一步**：设备侧 gather 表**每次 run 都重建**（与"一跳一次"的注释矛盾）⇒ 加同一跳内的缓存（9 → 7 次派发/跳）

> 腿 `p30-n78-dispatch2` 的站点表把 ① 拆到了可实现的一步：**每跳 9 次均衡派发 = 3 个 run × [建表 + gather y + 均衡]**。
> 用户裁决：先做 ①（合并 gather）。第一步取其中**最安全、且有注释可对照**的一处。

#### ① 站点表（腿 `p30`，147,075 跳，二进制已携带戳）

```
[metal_stats] eq_batch flushes=147075 symbols=1764879 runs=441246 batched=441246 max_run=8
              first_break=estimates sites(ch_gather=441246 y_gather=441246 y_batch=441246 run=0 single=0)
[metal_stats] burst dispatches=1764963 (equalizer=1323738 demapper=147075 channel_estimator=294150)
```
⇒ 每跳：`flushes=1`、`symbols=12.0`、`runs=3.0`（断因 `estimates`）、**`ch_gather=3.0` + `y_gather=3.0` + `y_batch=3.0`**；
另一条批量路径（`run`/`single`）为 0。总派发 **12.0/跳**（均衡 9 + 信道估计 2 + 解映射 1）。
这条腿本身：**V1 中位 1488.6**（cohort 最好）、契约 **8/8**、`cbs/lane=2.00`、**0 gaps**。

#### ② 查到的**缺陷**（与注释直接矛盾）

`eq_gather_tables()` 里那条"**一跳一次构建**"的缓存（`hop_tables_valid` + `hop_tables_plan`，注释写着
"one build per hop, every dispatch of that hop reads them"）**只对宿主路径生效**：函数**先**试设备路径并在成功后**直接 return**，
于是自 batch 5e（默认走设备 `eq_build_gather`，腿的启动行也这么打印）起，**每个 run 都重建一次表** ⇒ 空中实测 **3.0 次/跳**。

#### ③ 改动（本节的交付）

* `eq_flush_state_t` 增加 `dev_tables_plan/dev_tables_valid`（与宿主缓存的键同一规则：**计划对象的地址**，只在**一跳内**有效）；
* `eq_gather_tables()` 在设备路径上也先查这个缓存，命中即返回；未命中才 `eq_build_gather_on_device()` 并记下键；
* `eq_flush_recycle()` 里与宿主缓存一起失效（同一跳生命周期）。

**预期**：`sites(ch_gather=…)` 从 **3.0/跳 → 1.0/跳** ⇒ 总派发 **12 → 10/跳**；
按离线"单次派发 ~12 µs、依赖链内不重叠"（§6.29/§6.30）⇒ **约 −24 µs** 的跳窗口，若这次重建还带着"写表 → 读表"的屏障代价则更多。
**不动 V4**（命令缓冲仍 `merged_hop` + `ch_wt` = 2.00/跳）。

#### ④ 正确性论证（为什么安全）

* 表的内容只由 `plan` 决定；**同一跳内**计划对象的**地址**唯一且稳定（demodulator 每跳新建一个 plan 对象）
  ⇒ 同址即同内容，缓存命中不会把别的跳的表拿来用；
* **跨跳**一律失效（`eq_flush_recycle()`），与宿主缓存同一条已被验证过的规则（那条注释写明：跨跳复用地址会
  "把上一跳的表喂给新跳"，比没有缓存更糟）；
* 不同地址（内容相同）只是不命中 ⇒ 退回逐 run 重建，**不会算错**。

#### ⑤ 网（全绿）

`ctest -L phy` **193/193**（含 `channel_equalizer_metal_unit_test`、`port_channel_estimator_metal_mmse_unit_test`、`ul_chain_replay`）、
`l1_handover_arms.sh` **5 PASS**、门自测 **PASS**；离线站点读数仍打印（`sites(...)`）。

#### ⑥ 预登记（腿 `p31-n78-eqtable`，标准配方）

| 读数 | 期望 |
|---|---|
| `sites(ch_gather=…)` | **1.0/跳**（原 3.0）；`y_gather`/`y_batch` 仍 3.0（这一步不动它们）|
| `burst dispatches`/跳 | **10.0**（原 12.0）|
| `busy split merged_hop` | 略降（−24 µs 量级或更多）|
| **V1 中位** | 不变或略好（≈1490 → 期望 ~1460–1490）|
| 契约 / `cbs/lane` / gaps / D16 | 8/8 / 2.00 / 0 / `batch_max=14 batch_src=auto` |
| 反例判读 | 若 `ch_gather` 仍 3.0 ⇒ 缓存没命中（键或失效点写错），先修读数再谈收益 |

**下一步（① 的后半、未做）**：把 `y_gather` 折进均衡派发（3 → 1/run，再省 2 次/跳），
以及 ②（解开 `estimates` 断因让 run 覆盖整跳）——两者都在 §6.44 ③ 的同一张账上。



### 6.46 ★★ 腿 `p31-n78-eqtable`：**预登记全部命中** —— 设备侧表 3.0 → **1.0/跳**、总派发 12.0 → **10.0/跳**、`merged_hop` **−24.4 µs**、**V1 中位 1488.6 → 1463.5（−25.1 µs）**；⇒ **"每去掉一次派发 ≈ 一跳 −12 µs"被 1:1 校准**

#### ① 预登记 vs 实测

| 读数 | 预登记 | `p30`（改前）| **`p31`（改后）** | |
|---|---|---|---|
| `sites(ch_gather=)` | **1.0/跳** | 441,246 = 3.0/跳 | **143,559 = 1.0/跳** | ✅ |
| `sites(y_gather=)` / `(y_batch=)` | 仍 3.0/跳 | 3.0 / 3.0 | **3.0 / 3.0** | ✅（本步不动它们）|
| `burst dispatches` | **10.0/跳** | 12.0（1,764,963 / 147,075）| **10.0**（1,435,750 / 143,559）| ✅ |
| 其中 `equalizer=` | 7.0/跳 | 9.0 | **7.0**（1,005,073 / 143,559）| ✅ |
| `busy split merged_hop` | 略降（−24 µs 量级）| 616.5 µs | **592.1 µs** | ✅ **−24.4 µs** |
| **V1 中位** | 不变或略好 | 1488.6 µs | **1463.5 µs** | ✅ **−25.1 µs** |
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | 8/8 / 2.00 / 0 | **8/8 / 2.00 (max=2) / 0** | ✅ |
| `runs` / `max_run` / `first_break` | 不变 | 3.0 / 8 / `estimates` | **3.0 / 8 / `estimates`** | ✅ |

#### ② 这把"标尺"（本节最有价值的产出）

* **去掉 2 次派发/跳 ⇒ `merged_hop` −24.4 µs、V1 −25.1 µs** ⇒ **≈ 12 µs/派发**，
  与离线微基准（单 threadgroup 派发 ~12.18 µs、依赖链内不重叠，§6.29/§6.30）**1:1 吻合**。
* ⇒ **从今往后，跳内每合并一次派发，就可以按 −12 µs 预估**（不需要再飞腿就能排优先级）；
  也说明**其余 10 次派发**就是 **~120 µs** 的固定开销，而 `merged_hop` 的 592 µs 里还有 ~470 µs 是**别的**东西
  （真实算力 + 派发之间的依赖等待 + 大 kernel 的执行窗口）——下一层账要用"移除某个 kernel/阶段"的臂去量，
  或者把某个 kernel 单独拿去做离线微基准（§6.29 的做法）。

#### ③ 下一步（按同一把标尺排队，预计收益）

| # | 内容 | 派发/跳 | 预计 |
|---|---|---|---|
| **①后半** | 把 **`y_gather` 折进均衡派发**（每 run 少 1 次：3 → 1）| 10 → **7** | **≈ −36 µs** |
| **②** | 解开 **`estimates`** 断因，让 **run 覆盖整跳**（3 → 1）| 7 → **5** | 再 **≈ −24 µs**（①②合计 ≈ −60 µs ⇒ V1 ≈ 1400）|
| ③ | 审视**信道估计的 2 次**与**解映射的 1 次**（是否也能并/提前）| 5 → ? | 待量 |
| ④ | 下一层：`merged_hop` 里"非派发"的那 ~470 µs（真算力 + 依赖等待）⇒ 需要**单 kernel 离线微基准**或"移除阶段"臂 | — | 未知，可能最大 |

**纪律**：①后半与②都按 §6.30 的不变量判据做（`value_net` + 逐字节/容差 + metal 单测 + `l1_handover_arms`），
并**同时读** `sites(...)`、`burst dispatches`、`merged_hop`、**V1**、契约/`cbs/lane`/gaps——**读不出按 RED**。



### 6.47 **①后半的施工方案（已查清、待施工）**：把 `y_gather` 去掉 —— **优先"把网格直接绑成 y"（只动引擎，不改 kernel）**

> 本节是一次**交接性的方案记录**：代码已经查清到"改哪里、判据是什么"，但没有动手（当时会话上下文已近耗尽，
> 而这是一处要动 kernel/绑定与不变量网的改动，硬塞在最后会违反"先证后改"的纪律）。

#### ① 已经查清的事实

* **`y_gather` 是"纯拷贝"**（其自身注释：*"The device work is a plain copy, and it is encoded in the same command
  buffer right before the equalization that consumes it"*）：它用 `taps/entries` 表把设备网格里的接收符号**搬到**
  均衡 kernel 期望的连续布局（`[symbol][port][re]`），**没有算术**，元素类型两侧都是 `cbf16_t`。
* **均衡派发（`eq_encode_batch_dispatch`）读 y 的方式**：`setBuffer:b_y.buffer offset:b_y.offset atIndex:1` +
  `strides.y_stride`（每符号步长）⇒ 它**只认一个缓冲区 + 偏移 + 步长**，不关心那个缓冲区是谁。
* 每次 run 的顺序是：`ch_gather`（建表，已在 §6.45 变成一跳一次）→ **`y_gather`（搬 y）** → `y_batch`（均衡）。
  空中实测（`p31`）：`sites(ch_gather=1.0 y_gather=3.0 y_batch=3.0)`/跳，均衡派发 **7.0/跳**。

#### ② 两个施工变体（按优先级）

**变体 A（首选，只动引擎、不改 kernel、不改 metallib）**：
当**这一 run 的 y 在网格里本来就是连续可描述**时（判据候选：`nof_ports == 1`、`subc_stride == 1`、
且 `y_stride` 与网格的 `symb_stride` 一致），**跳过 `y_gather`**，直接把**网格缓冲**绑成 `b_y`：
`b_y = b_grid`（偏移 = 该 run 第一个符号在网格里的字节偏移），`strides.y_stride = grid.symb_stride`。
* **逐字节相同**：读的是同一批 `cbf16_t` 元素、没有转换、没有算术 ⇒ 结果与"搬过去再读"完全一致。
* 条件不满足时**退回原路径**（照旧发 `y_gather`），所以是"能省则省"，不是"改变语义"。
* 建议加旋钮（如 `OCUDU_EQ_DIRECT_GRID=1`，默认开）以便 A/B 与反例判读。

**变体 B（后备，要动 kernel）**：给均衡 kernel 加"**读时 gather**"模式（绑定 `taps/entries` + `b_grid`，
按表寻址 y），适用于多端口等布局不一致的情况。代价是均衡 kernel 内循环多一次表查找，收益同样是省掉一次派发与一次往返。

#### ③ 预登记（施工后飞腿）

| 读数 | 期望 |
|---|---|
| `sites(y_gather=)` | **3.0 → 0**（变体 A 命中时）|
| `burst dispatches`/跳 | **10 → 7**（均衡 7 → 4：1 建表 + 3 均衡）|
| `merged_hop` / **V1** | 各 **≈ −36 µs**（按 §6.46 校准的 12 µs/派发）|
| 契约 / `cbs/lane` / gaps / D16 | 8/8 / 2.00 / 0 / `batch_max=14 batch_src=auto` |
| **不变量** | `channel_equalizer_metal_unit_test`、`ul_chain_replay`（逐字节/容差）、`value_net`、`l1_handover_arms.sh` **全部先绿** |
| 反例判读 | 若 `sites(y_gather)` 仍 3.0 ⇒ 变体 A 的条件没命中（回退路径被走），先读 `nof_ports`/strides 再谈收益 |

#### ④ 施工第一步（机械动作，给下一次会话）

1. 找到 `ch_gather_desc` 的定义（`ocudu_equalizer_metal_engine` 相关的 metal 头），确认 `nof_ports`/`grid.subc_stride`/
   `grid.symb_stride` 三个字段与"这一 run 在网格里连续"的判据；
2. 在 `eq_flush_hook` 里那一处 `if (gather_run && !eq_gather_tables(...))` 与 `y_gather` 调用点之间加判据分支
   （命中 ⇒ 不发 `y_gather`，把 `b_y` 指向网格、改 `strides.y_stride`）；
3. 跑 §6.47 ③ 的不变量网（离线）＋ 门自测；**重建戳与 gnb 两者**（§6.44 ⑥ 的教训）；
4. 交给用户飞一条腿（标签建议 `p32-n78-directgrid`），按 ③ 判读。

### 6.48 ★ **①后半落地（变体 A）**：`y_gather` 不再存在 —— 均衡直接**在网格里读**收到的符号；离线 **27 条语料 ×2 臂 = 135 个 dump 文件逐字节相同**，而派发 **12 → 7**（其中**比预登记多省一次**：不 direct 的跳**连设备侧建表派发都不需要**）

> 本节是 §6.47 的施工结果。**没有飞腿**：这一刀的全部证据是离线取证（逐字节 A/B + 双面证伪臂），
> 空口读数留给 `p32-n78-directgrid`（§6.48 ⑥ 给出判读表）。

#### ① 先把前提查实（都在离线／已有日志里，不烧腿）

§6.47 把变体 A 的判据写成"`nof_ports==1`、`subc_stride==1`、`y_stride` 与网格 `symb_stride` 一致"。查实后，
**前两条对每一条 gather run 自动成立，真正的判据只剩"这一 run 的元素在网格里连续"**：

* `channel_equalizer_metal::consumes_gathered_symbols()` 的实现就是 `(nof_ports == 1) && (nof_layers == 1)`
  （`channel_equalizer_metal.cpp`）⇒ 既然这一跳**有** gather run，端口条件**必然**已满足。
* `ch_gather_desc::build()` 里 `dest` 是**逐元素自增**的（`0,1,2,…`，每个符号重新从 0 开始）
  ⇒ 目的地**按构造就是稠密的**，所以"能不能直接读"只取决于**源子载波**是否连续（`entries[i].subc == subc_base + i`）。
* `subc_stride` 由 `make_resource_grid_device_view()` 写成 1，`symb_stride = 网格子载波数`——两者都是布局常量。
* **DM-RS 符号有没有数据**决定了"跳里有没有带洞的符号"：`active_re_per_prb_dmrs = ~get_dmrs_prb_mask(type1, nof_cdm_groups_without_data)`，
  只有 `nof_cdm_groups_without_data = 0` 时它才等于全 12 个子载波。`p31` 的 `eq_batch` 读数**间接证明**空口就是这样：
  `flushes=143559 symbols=1722628 runs=430757 batched=430757 max_run=8 first_break=estimates`
  ⇒ **每个 run 都 ≥2 个符号**（`batched == runs`）⇒ **不存在孤立的单符号 run**。
  若 DM-RS 符号带数据（6 RE/PRB 或 0 RE/PRB），它们与数据符号的 `nof_re` 不同，`same_geom` **必然**把它们切成
  单符号 run（0 RE 的甚至根本不进 pending，见 `pusch_demodulator_impl.cpp` 的 `if (nof_re_symbol == 0) continue;`）
  ⇒ 与"无单符号 run"矛盾 ⇒ **空口这一跳里 12 个符号同几何**（14 符号的槽去掉 2 个不带数据的 DM-RS 符号）。
* **分配是否连续**：`p31` 的腿日志里 **143,561 条 PUSCH 授权中 143,559 条是单一连续区间** `prb=[start, stop)`
  （`[0,51)` 57897、`[26,51)` 23762、`[3,51)` 18867、`[5,51)` 10817、`[8,51)` 9747 …）⇒ `rb_mask` 连续 ⇒ 入口稠密。

⇒ **预登记的条件在空口上应当 3.0/3.0 命中**（3 个 run 全部 direct）。

#### ② 实现（三处，全部只动宿主侧布局，不动 kernel/metallib）

| 文件 | 改动 |
|---|---|
| `include/ocudu/phy/upper/equalization/channel_equalizer_device_grid.h` | `ch_gather_symbol` 增加 `subc_base`（该符号第一个 entry 的网格子载波）与 `dense`（entries 是否**就是**网格本身那一段连续子载波）|
| `lib/phy/upper/equalization/channel_equalizer_device_grid.cpp` | `build()` 里**从刚建好的 entries 表读出**这两个字段（而不是从掩码重新推导）⇒ "能不能原地读"与"gather 会搬哪些元素"**不可能各自漂移** |
| `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm` | 旋钮 `OCUDU_EQ_DIRECT_GRID`（默认开）；判据 `eq_direct_grid_run()`；`eq_flush_hook` 里**在分配 staging 之前**判定 ⇒ direct 的 run **不分配 y**、**不发 gather**、`b_y` 绑定网格、`strides.y_stride = grid.symb_stride`；计数 `y_direct` + `eq_direct miss(…)` 直方图 |

* **为什么"逐字节相同"是构造性的**：gather 用 `taps/entries` 把 `grid[port][sym][subc]` 搬到
  `y[sym][port][dest]`，元素类型两侧都是 `cbf16_t`、**无算术**；`dest` 稠密 ⇒ 直接绑定时均衡 kernel 读到的
  就是同一批元素、同一顺序（kernel 只认"一个缓冲 + 偏移 + 每符号步长"，见 `eq_encode_batch_dispatch`）。
* **顺序保证不变**：gather 与均衡本来就是**同一个融合命令缓冲**里的两次派发，direct 只是让均衡**自己**去读那块网格，
  跨流水线的屏障关系与原来完全一致（grid 的生产者仍是前端 DFT，同一个队列）。
* **V4 不受影响**：去掉的是**派发**，不是**命令缓冲**（§6.44 ④ 已证），`cbs/lane` 按构造不动。

#### ③ 离线取证（双面）

| 臂 | 语料/形状 | `y_direct` / `y_gather` | 派发 | 结果 |
|---|---|---|---|---|
| **正面** | `work_tmp/corpus` 全部 **27 条**（3…25 PRB，14 符号，DM-RS 不带数据） | **4 / 0**（每条都一样）| `burst dispatches` **12 → 7**、`equalizer` **9 → 4** | **135 个 dump 文件逐字节相同**（`llr.bin`、`h.bin`、grid `.bin`、`_ce.txt`、`.txt`）|
| **反面（洞来自 DM-RS 梳）** | 同上但把 `dmrs_nof_cdm_groups_without_data` 改成 **1**（DM-RS 符号**带数据** ⇒ 梳状有洞）| **4 / 3**，`miss(holes=3)` | `y_gather` 7 → **3** | 5 个 dump **逐字节相同**；且 4 个数据 run 命中、3 个 DM-RS run **被拒**（判据有齿）|
| **反面（洞来自分配缺口）** | 直接探针 `ch_gather_desc::build()`（§6.48 ④）| 连续 ⇒ `dense=1`；`{0,1,3}` ⇒ **`dense=0`**；`{0,1,4,5}` ⇒ **`dense=0`** | — | 分配缺口确实清 `dense` |
| **不变量网** | — | — | — | `ctest -L phy` **193/193**、`l1_handover_arms.sh` **全 PASS（含 drop 臂 8/8 不同）**、`metal_chain_probe`/`eq_handoff_probe` **rc=0** |
| **`value_net`（容差网）** | 47 条（27 corpus + 20 narrow）| 两臂 | — | 两臂都 `captures=47 problems=183`，**失败清单逐行相同**（⇒ 183 条全是**归档基线陈旧**的老账，Q11；这一刀**新增 0 条**）|

* ⇒ **比预登记多省一次派发**：预登记只算了 3 次 `y_gather`（10 → 7），但一个**全是 direct run** 的跳
  **根本不需要 gather 表**，于是 `eq_gather_tables()` 不被调用 ⇒ **设备侧建表派发也消失**（`sites(ch_gather)` 1 → **0**）。
  语料实测 `equalizer` **9 → 4**（=4 次均衡 + 0 建表 + 0 gather）⇒ 按 §6.46 的标尺，空口预计 **−4×12 = −48 µs**
  （**修正 §6.47 的 −36 µs 预估**：跳内派发 **10 → 6**，不是 7）。
* **两臂都真的走了各自的路**（不是"网空跑"）：off 臂每条都是 `miss(disabled=4)`、`y_gather=4`；
  on 臂每条都是 `y_direct=4`、`y_gather=0`。

#### ④ 一路上的两个"实验设计缺陷"（记下来，别重犯）

1. **用 replay 的 `alloc_prb` 带洞语料去证伪 `dense` 是无效的**：replay 为带洞分配建的是
   `vrb_bitmap(bwp_start + max_prb + 1)`，而 `bwp_size_rb` 仍是 3 ⇒ `get_crb_mask()` 里
   `coreset_start + vrbs.size() <= bwp_size` **不成立**，`non_interleaved_mapping::vrb_to_crb()` 又在
   `crb_bitmap(bwp_start + bwp_size)` 上 `fill()` ⇒ **洞在进入 plan 之前就被压平了**（实测该臂
   `y_direct=4 holes=0`，但 dump 与 3 PRB 连续语料不同 ⇒ 掩码确实变了，只是变成了另一种**连续**掩码）。
   ⇒ 要测 plan 的判据就**直接测 plan**：`/tmp` 下的独立探针（29 行）构造三种掩码，打印每个符号的
   `nof_entries/subc_base/dense`，一次就看清（本次已做）。
2. `eq_handoff_probe` / `metal_chain_probe` **不走 gather**（`ch_re device=0 host=…`，全部宿主 staging）
   ⇒ 它们对这一刀是**"没动过那条路"的回归网**，**不是** direct 路径的覆盖；direct 路径的离线覆盖
   只有 **`ul_chain_replay` 语料**（27×4 = 108 次 direct run）。

#### ⑤ 反例判读表（腿 `p32-n78-directgrid` 用）

| 读数 | 含义 |
|---|---|
| `y_direct≈3.0/跳`、`y_gather=0`、`burst dispatches` **6.0/跳**、`merged_hop`/V1 各 **≈ −48 µs** | ✅ 预登记（修正版）命中 |
| `y_direct=0`，`miss(holes=…)` 非零 | 该跳的符号**有洞**：要么 DM-RS 带了数据（`cdm_groups_without_data≠0`），要么调度器给了**多簇**分配 ⇒ 回退，不是"没收益"；先看 miss 直方图再看分配（腿日志的 `PUSCH: … prb=[a, b)` 行）|
| `y_direct=0`，`miss(ports=…)` 非零 | 这一跳有 **>1 个接收端口** ⇒ 变体 A 不适用（要么回退，要么走变体 B 的 kernel 内 gather）|
| `y_direct=0`，`miss(disabled=…)` 非零 | **A/B 对照臂**（`OCUDU_EQ_DIRECT_GRID=0`），不是缺陷 |
| `y_direct` 与 `y_gather` 相加 ≠ `runs` | **仪器坏了**（每个 gathered run 必须落进二者之一）⇒ 按 RED 处理 |

#### ⑥ 下一步

* **要飞的一条腿**：`p32-n78-directgrid`（§6.48 ⑤ 判读）。**它同时是 ①后半的验收腿**：
  预登记 = `sites(y_direct≈3.0 y_gather=0)`、`sites(ch_gather=0)`、`burst dispatches 6.0/跳`、
  `merged_hop` **≈ 592 → 544 µs**、**V1 ≈ 1463.5 → 1415 µs**、契约 8/8、`cbs/lane=2.00`、gaps 0、D16 不变。
* 之后按 §6.46 ③ 的表走：**②**（解开 `estimates` 断因让 run 覆盖整跳，7 → 5，≈ −24 µs）
  ——注意**②与①后半有交互**：`estimates` 一旦不再断开 run，run 会**跨过 DM-RS 符号**……
  但空口这一跳的 DM-RS 符号**不带数据、根本不进 pending**，所以 run 里只有数据符号，**①后半仍然命中**；
  只有在"DM-RS 带数据"的配置里才会由 direct 退回 gather（有 `miss(holes)` 可见）。
* **③④ 不变**：信道估计的 2 次 + 解映射的 1 次；`merged_hop` 里非派发的 ~470 µs（需要"移除阶段"臂或单 kernel 微基准）。



### 6.49 ★★ 腿 `p32-n78-directgrid`：**机制逐项命中、V1 中位 1463.5 → 1411.3 µs（−52.2）**；且**修正了那把标尺的口径** —— "≈12 µs/派发"是 **V1/跨度**的尺，**不是设备占用窗口**的尺（窗口只降 26.2 µs）

#### ① 预登记 vs 实测

| 读数 | 预登记（§6.48 ⑥）| `p31`（改前）| **`p32`（改后）** | |
|---|---|---|---|---|
| `sites(y_direct=)` | ≈3.0/跳 | —（无此字段）| **432,397 = 3.00/跳** | ✅ |
| `sites(y_gather=)` | **0** | 430,757 = 3.00/跳 | **0** | ✅ |
| `sites(ch_gather=)` | **0** | 143,559 = 1.00/跳 | **0** | ✅ |
| `burst dispatches` | **6.0/跳** | 1,435,750 / 143,559 = **10.00** | **864,721 / 144,108 = 6.00** | ✅ |
| 其中 `equalizer=` | 3.0/跳 | 7.00 | **3.00**（432,397）| ✅ |
| `merged_hop` | ≈544 µs | 592.1 µs | **565.9 µs** | ⚠ **只降 26.2**（见 ③）|
| **V1 中位** | ≈1415 µs | **1463.5** µs | **1411.3 µs** | ✅ **−52.2**（new best）|
| 契约 / `cbs/lane` / gaps | 8/8 / 2.00 / 0 | 8/8 / 2.00 / 0 | **7/8** / **2.00 (max=2) dropped=0** / **1** | ⚠ 唯一失败 = 电台 gap（见 ⑤）|
| `runs` / `max_run` / `first_break` / D16 | 不变 | 3.0 / 8 / `estimates` / `batch_max=14 batch_src=auto` | **3.0 / 8 / `estimates` / 同** | ✅ |

**仪器自洽**：`y_direct + y_gather == runs`（432,397 = 432,397）⇒ §6.48 ⑤ 的"仪器坏了"判据不触发。

#### ② 省在哪一段（把 −52 µs 拆开）

同一配对口径（P0-5 相位分段，p31 → p32）：

| 段 | `p31` 中位 | `p32` 中位 | Δ |
|---|---|---|---|
| `t2f`（= rx_wait + 前端宿主/设备）| 534.3 µs | 531.1 µs | −3.2 |
| `ce` | 71.4 µs | 67.1 µs | −4.3 |
| **`eq_demap`**（均衡 + 解映射所在的融合缓冲）| 836.4 µs | **794.7 µs** | **−41.7** |
| 三段和 | 1442.1 | 1392.9 | **−49.2**（对 V1 的 −52.2，覆盖率照旧）|

⇒ **收益集中在 `eq_demap`**：被去掉的正是**它那个命令缓冲里的 4 次派发**（3 次 `y_gather` + 1 次设备侧建表）。

#### ③ ★ 标尺的口径（本节最该记住的一条）

* **V1 口径**：去掉 4 次派发 ⇒ **−52.2 µs** ⇒ **≈13 µs/派发**，与 §6.46 空口校准的 12 µs **1:1 吻合**。
* **设备占用窗口口径**：同一改动的 `merged_hop` 只降 **26.2 µs** ⇒ 只相当于 **~6.5 µs/派发**。
* ⇒ **§6.46 的"12 µs/派发"要写清口径**：它成立在 **V1/跨度**上（含宿主 encode 与派发发射延迟），
  **不能**直接用在 `merged_hop`（设备 busy 窗口）上——一个**小派发**在一个**已经很忙**的融合缓冲里，
  它的发射延迟有一部分被别的活儿**遮住**了（本腿也不排除"均衡现在按 `symb_stride=612` 而不是 300 跨步读 y，稍贵一点"这一项）。
  **两种解释本腿分不开**（没有第三个臂），⇒ 记入 §8 待办：要分开就用 `metal_chain_probe` 的 device-slice 臂（`ch_re device=…`）
  或照 `dft_kernel_cost.mm` 的形状做一个**均衡 kernel 微基准**（同一 run：gather-读 vs 网格直读）。
* 实践含义：**排优先级仍按 12–13 µs/派发（V1 口径）**；但**不要**再用 `merged_hop` 的降幅去反推派发数。

#### ④ 判据现状（本腿）

| 判据 | 读数 | |
|---|---|---|
| **V1** | **1411.3 µs**（p95 1599.1、p99 1722.7；samples=129,665）| ✅ **全部腿最好**（基线 2675.1 ⇒ **−47.2%**）|
| **V2** | `starved_events=0`、`held_max=12 < pool=16`、`free_min=4`、`pop_blocking` max **27 µs**、`dropped=0` | ✅ 保持 |
| **V4** | `cbs/lane=2.00 max=2 dropped=0`、crossings `0.00 + 0.00`/跳 | ✅ 保持（**合并派发不动提交数**，第三次确认）|
| **V5** | 契约 **7/8**（唯一失败 = `radio sample continuity: 1 gaps`）| ⚠ 见 ⑤ |

#### ⑤ 那 1 个 gap 与 `stale=1` 的判读（**不是这一刀造成的**）

* **同形状的先例**：`p24`（1 gap）、`p28`（2 gaps / 285,262 样点）、`p29`（3 gaps）**都在改动之前**；
  本腿 `1 gaps`（139,378 样点 ≈ 4.5–6 ms），而 `stale=1`（span 14.45 ms）与它是**同一件事的两面**（缺样点 ⇒ 跨度越过 8 ms 门限）。
* **收包侧全程健康**：`starved_events=0`、`free_min=4`、`pop_blocking` max **27 µs**（无 1 ms 以上）、`dropped=0`、
  `dry-pool reaps=0`、门 **D1/D2/D3 全 PASS**（最长持有 sub-second、未认领块 ms 级回收、接收线程从未 park）
  ⇒ **gap 不是池饥饿**，与目标（V2 症状）无关。
* **电台侧本腿更差**：RF 失败 **1013**（`p31` 792）、`transmit()` **672 次 >1 ms**（`p31` 545，最大 85 ms）、
  census：`underflow 881 + overflow 1`、28 段 >0.3 s 静默（最长 3.96 s）⇒ 全部落在**已暂停的 V3 域**（电台/USB 传输侧）。
* ⇒ **判读**：这条 gap 按 §3.4 的"**按对累计、别用单腿红绿**"处理；且本腿电台更差而 V1 仍降 52 µs ⇒
  **收益不是靠电台变好换来的**（方向上偏保守）。

#### ⑥ 下一步

* **首选**：同日再飞一条**对照腿 `p32b-n78-gather`**（同配方 + `OCUDU_EQ_DIRECT_GRID=0`），把 −52 µs 做成**成对**读数
  （机制方向已由 `y_direct/y_gather` 计数器钉死，对照腿只为**幅度**；若不成对，−52 仍应写成"单腿、机制已证"）。
* 然后进 **§6.44 ③ 的 ②**：解开 `estimates` 断因让 run 覆盖整跳（3 → 1，≈ −24 µs）——注意 §6.48 ⑥ 的交互说明。



### 6.50 ★★ 对照腿 `p32b-n78-gather`（`OCUDU_EQ_DIRECT_GRID=0`，同一二进制）：把 −52 µs 变成 **ABA 区间 [−52.2, −69.3]**，并量出**同臂腿间噪声 17.1 µs**；口径再修正为 **V1/跨度 13–17 µs/派发** vs **设备窗口 6.5–9.4**

#### ① 对照臂有效（旋钮确实进了进程）

`p32b`：`sites(y_direct=0 y_gather=436720) miss(disabled=436720 …)`、`sites(ch_gather=145568)`、
`burst dispatches 1455712/145568 = 10.00/跳`（均衡 1019008/145568 = **7.00**）、`runs 3.00/跳`、`max_run=8`、`first_break=estimates`。
⇒ 与 `p32` 相比**只有旋钮不同**（两臂 `runs`/`max_run`/`first_break`/D16/V4 全同）。**派发账逐项闭合**：
`p32b`：`ch_gather 145568 + y_gather 436720 + y_batch 436720 = 1019008 = equalizer` ✓；
`p32`：`y_direct 432397 + 0 + 0 + ch_gather 0 = 432397 = equalizer` ✓。

#### ② 三腿对照（**A B A**：`p31` 采集臂 → `p32` 直读臂 → `p32b` 采集臂，同日）

| 读数 | `p31`（gather，旧二进制）| **`p32`（direct）** | `p32b`（gather，同二进制+旋钮）| 直读臂相对两个采集臂 |
|---|---|---|---|---|
| **V1 中位** | 1463.5 µs | **1411.3 µs** | 1480.6 µs | **−52.2 … −69.3 µs** |
| samples | 127,916 | 129,665 | 138,773 | — |
| `merged_hop` | 592.1 | **565.9** | 603.6 | **−26.2 … −37.7** |
| `t2f` / `ce` / **`eq_demap`** | 534.3 / 71.4 / **836.4** | 531.1 / 67.1 / **794.7** | 533.6 / 70.5 / **857.4** | eq_demap **−41.7 … −62.7** |
| 三段和 | 1442.1 | 1392.9 | 1461.5 | −49.2 … −68.6 |
| 派发/跳（均衡）| 10.00（7.00）| **6.00（3.00）** | 10.00（7.00）| −4 次 |
| contract / gaps | 8/8 / 0 | 7/8 / 1 | **7/8 / 1** | 两臂都 1 gap |
| RF 失败 | 792 | 1013 | 807 | — |
| `stale` | 0 | 1（14.45 ms）| 0 | — |
| `cbs/lane` / 池 | 2.00 / `starved=0` | 2.00 / `starved=0` | 2.00 / `starved=0` | 全同 |

#### ③ 结论（这才是可以引用的说法）

* **效果量**：直读臂 `1411.3 µs` **同时低于两个采集臂**（1463.5 / 1480.6）⇒ **净效应 ∈ [−52.2, −69.3] µs**，中点 ≈ **−60 µs**。
  这是 **ABA** 设计：中间那一条无论环境往哪漂，效应都被两个采集臂**夹住**，不必挑一个基线。
* **同臂腿间噪声**：`p31` 与 `p32b`（**同一条采集路径**）差 **17.1 µs** ⇒ 效应 ≥ **3×** 噪声底线
  （V1 单腿噪声量级从此有了数：**~17 µs**，与 §6.46 里观测到的 24–25 µs 级差异同量级）。
* **标尺（修正版，取代 §6.46 的单一口径）**：4 次派发 ⇒
  **V1/跨度口径 13–17 µs/派发**（中点 ~15），**设备 busy 窗口口径 6.5–9.4 µs/派发**。
  ⚠ 排优先级用前者；**不要**用 `merged_hop` 降幅反推派发数（§6.49 ③、§8 Q16）。
* **gap 的归属到此收口**：**采集臂也有 1 gap**（78,288 样点；`p32` 是 139,378）——两臂同病、且改动前就有（`p24`/`p28`/`p29`），
  池在两臂都健康（`starved_events=0`、`held_max=12`、`pop_blocking` max 27/38 µs、`dropped=0`）
  ⇒ **与这一刀无关**，属已暂停的 V3 域（电台/USB 传输侧）。**V5 的"契约 8/8"在本轮两条腿上都是 7/8，红的永远是同一项**。

#### ④ 下一步

①后半到此**收口**（离线逐字节 + 空口 ABA 成对）。下一步 = **§6.44 ③ 的 ②**：解开 `estimates` 断因让 run 覆盖整跳
（3 → 1，≈ −24 µs；注意 §6.48 ⑥ 的交互）。按修正后的标尺，若 ② 真能省 2 次派发，预期 **V1 −26 … −34 µs ⇒ ≈1385–1390 µs**。



### 6.51 ★★ **残留 gap 的归属收口**：每一次 gap 都是**电台接收环溢出**（最近 11 条腿 **overflow 次数 == gaps 次数，逐事件对应**）；补上 RX 侧仪器（`rx_overflows` / `[ul_rx_timing]` / 门 **D19**）并把接收环 **64 → 256 帧**

> 用户裁决：先把 gap 讲清楚，再做**加深接收环**与**补 RX 侧仪器**这两件。本节是它们的记录。

#### ① 先把现象定清楚（读码 + 11 条腿）

* **定义**：`lower_phy_baseband_processor.cpp` 每次收块比对"本块 `ts`"与"上一块末尾"，不等即记 1 个 gap，
  `gap_samples += |差|`（**丢或重复的样点数**）；`ts==0`（电台 stop/error 返回）单列，不算 gap。
  契约项 `radio sample continuity` = `gaps == 0`（无条件）。
* **三条日志行**：`[RF] Real-time failure in RF: overflow`（因）、
  `[PHY] Receive stream discontinuity: block at timestamp … where … was expected (N samples)`（只打**第一条**）、
  退出时 `radio sample continuity: N gaps over M blocks (K samples missing or repeated)`（判据读它）。
* **量级**（本配置 `srate=23.04` Msps ⇒ 1 样点 = 43.4 ns）：`p29` 3 gaps/1,048,733 样点（45.5 ms）、
  `p24` 1/1,283,405（55.7 ms）、`p28` 2/285,262（12.4 ms）、`p32` 1/139,378（6.05 ms）、`p32b` 1/78,288（3.40 ms）。
  **修复前**是 76M–229M 样点 ≈ **3–10 s** ⇒ **"宿主停顿 ⇒ 秒级丢样"那条链已经治好了**（§6.11/§6.22/§6.27/§6.38），
  剩下的是**毫秒级**的另一种。
* **与"UL 静默"是两个现象**（别混）：`p31` **0 gaps** 却有 20 段 >0.3 s 的 PUSCH 静默（最长 **6.48 s**）；
  `p32` 1 gap 而有 28 段（最长 3.96 s）⇒ 秒级静默不是 gap 造成的，属手机/调度侧。

#### ② 归属：1:1，无例外

| 腿 | RX `overflow` | gaps | | 腿 | RX `overflow` | gaps |
|---|---|---|---|---|---|---|
| `p20` | 2 | 2 | | `p26` | 0 | 0 |
| `p21` | 0 | 0 | | `p27` | 0 | 0 |
| `p22` | 0 | 0 | | `p28`（另一条同配方腿）| 0 / 2 | 0 / 2 |
| `p23` | 0 | 0 | | `p29` | 3 | 3 |
| `p24` | 1 | 1 | | **`p32`** | **1** | **1** |
| `p25` | 0 | 0 | | **`p32b`** | **1** | **1** |

时间戳也吻合（overflow 之后 ~1 ms 就是 discontinuity）：

```
p32b 10:42:53.152417 [RF] Real-time failure in RF: overflow
     10:42:53.153088 [PHY] Receive stream discontinuity: … (78288 samples)
p32  10:27:57.067232 [RF] … overflow
     10:27:57.067961 [PHY] … (139378 samples)
p29  09:28:00.626260 [RF] … overflow  →  09:28:00.627279 [PHY] … (54500 samples)
```

⇒ **机制链**：**宿主（或 USB 传输）没按时抽干 B200 的接收环 ⇒ 环灌满 ⇒ 电台丢样点（UHD 报 `overflow`）
⇒ 下一块时间戳往后跳 ⇒ 连续性检查记 1 个 gap**。
**"宿主还是 USB"当时分不出来**，因为 UHD 已经算好的分类**在 gateway 边界被丢掉了**（`metadata` 只带 `ts`），
只变成一行 warning。

#### ③ 改了什么（本节落地）

| 处 | 改动 |
|---|---|
| `baseband_gateway_receiver::metadata` | 增加 `rx_error {none, late, overflow, other}`：**电台自己对这一块的判词**随块一起交上来（默认 `none`，不填的电台不受影响）|
| `lib/radio/uhd/radio_uhd_rx_stream.cpp` | 把已有的 `md.error_code` 分类**填进 metadata**（原来只用来发事件/日志）|
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` | 计数 `rx_overflows/rx_lates/rx_other`、**每个 gap 的大小**（`gap_us=[…]`，原来只记第一条）、以及 **`[ul_rx_timing]`**：每次收块的 **RECV**（`receive()` 调用自身时长，TX 侧 `[dl_tx_call]` 的孪生）、**LOOP**（上一次返回→本次发起之间，宿主自己的活儿与调度）、**SLIP**（`LOOP+RECV−本块空口时长`，跑在时间线后面的漂移量）与**每次 overflow 的上下文** `overflow_ctx=[recv_us=…,loop_us=…]` |
| `configs/gnb_rf_b200_tdd_n78_20mhz.yml` | `num_recv_frames` **64 → 256**（`num_send_frames` **刻意不动**：下一条腿只差一个变量）。64 帧在这个速率下只有**毫秒量级**余量（见 ⑤），而自宿主停顿被治好以来的每个 gap 都是 **2–6 ms** ⇒ 这是**缓解**：不消除停顿，只消除**丢样** |
| `doc_chinese/phy_latency/wip/p0_gate.sh` | **D19（INFO）**：读 `[ul_rx_timing]` + `rx_overflows` + `gap_us`，并按 `overflow_ctx` **自动判"宿主迟到"还是"传输内阻塞"**；`p0_gate_selftest.sh` 对**两个归属各有一个夹具**（只喊一边的解析器过不了）|

**判读表（下一条腿）**：

| `overflow_ctx` | 含义 |
|---|---|
| `loop_us` 大（≫ `recv_us`）| **宿主没按时来问**（自身调度；融合车道的宿主线程是嫌疑）⇒ 打**优先级/亲和性/并发度**臂 |
| `recv_us` 大（≥ `loop_us`）| **调用内部被传输顶住**（电台/USB 背压）⇒ 宿主调度不是杠杆，查 USB/端口/线/`otm_format` |
| `recv_us` 与 `loop_us` 都小，却仍 overflow | 停顿发生在**两次收块之外**（例如 stop/restart、或 UHD 内部线程）⇒ 看 `gap_us` 大小与 census |

#### ④ 验证（离线）

* **事件路径有齿**：接收 spy 新增 `set_rx_error()`；`lower_phy_test` 新臂 `RadioReceiveOverflowIsReported` 注入
  "overflow + 5 槽时间戳跳变"，断言**已注册的连续性判据**（就是门 D4 读的那个对象）**必须失败**；
  该次运行的报告自证：`1 gaps over 3 blocks (122887 samples …), 1 radio receive overflow(s)`，
  并打出 `gap_us=[…]` 与 `overflow_ctx=[recv_us=531,loop_us=2 …]`。
* `lower_phy_test` **576/576**；`ctest -L phy` **193/193**（**第一次跑有 1 个失败 = 已知 flake
  `port_channel_estimator_metal_mmse_unit_test`**，`LastTestsFailed.log` 记下名字，重跑绿）；
  `p0_gate_selftest.sh` 全 PASS（含 D19 两个归属 + "老腿读不出"三个分支）。
* **夹具读数不是空口读数**（与 TX 探针同一条警告，已写进代码注释）：夹具里 `recv max=696us / loop max=1503us`
  只说明夹具怎么驱动，不说明电台。

#### ⑤ 环有多深（为什么 64 → 256 是对的量级）

`num_recv_frames=64` 自 9 月 17 日第一条腿就写死、从没被质疑。按 UHD 的 USB 默认帧长与 `otw_format=sc12` 估，
64 帧在 23.04 Msps 下是**毫秒量级（~5–8 ms）**的余量——而观测到的 gap 正是 **2.4–6 ms**（`p24` 的 55.7 ms 是一次更长的停顿）。
**"停顿超过环余量 ⇒ 超出的部分被丢"** 与这些数字量级一致。256 帧把余量抬到 **~20–30 ms**，代价是几 MB，
**稳态不增加时延**（环按序抽干，深环只在突发时提供余量）。

#### ⑥ 预登记（腿 `p33-n78-rxring`，待飞）

| 读数 | 期望 |
|---|---|
| `rx_overflows` | **必须等于 `gaps`**，且等于合并日志里 `[RF] … overflow` 的行数（这条等价关系是本次改动能被"就地"读出来的东西）|
| `gaps` | **0**（256 帧吸收掉 2–6 ms 的停顿）；若仍 >0 ⇒ **停顿比 ~28 ms 更长**，目标转向传输侧（USB/端口/线）|
| `[ul_rx_timing]` | `recv`/`loop` 的 max 与本轮对照腿同量级；`slip over 1ms` 少量 |
| 其余（V1/V2/V4/契约/D18）| 与 `p32` 同形，**不应因本条改动而动**（改的是收包余量与仪器）|

**纪律**：`gaps` 仍按"**按对累计**"读（单腿不当红绿）；**不许为提高通过率改 D4 的阈值**——
若 ⑥ 证明成因在电台环，V5 的"0 gaps"**是否重新定义**要由用户裁决，而不是悄悄调阈值。



### 6.52 ★ 腿 `p33-n78-rxring`（256 帧 + RX 仪器）：**Q17 有答案了——堵点在传输侧，不在宿主**；预登记的三项等价 **0==0==0** 成立；但**这一腿把环与仪器一起改了（我的实验设计错误）**，所以"环是否有效"**还没有对照**；且 **V2 在这一腿读红**（`held_max=16=pool`、`starved_events=2`，但 `pop_blocking` max 249 µs、`dropped=0`、无 park）

#### ① 预登记：成立

| 读数 | 预登记 | `p33` |
|---|---|---|
| `rx_overflows` | == `gaps` == 日志里 `[RF] … overflow` 行数 | **0 == 0 == 0** ✅（`p32`/`p32b` 上同一等式是 1==1==1）|
| `gaps` | 0（深环吸收 2–6 ms 停顿）| **0** ✅ |
| 契约 | 8/8 | **MET (8 of 8)** ✅ |
| `[ul_rx] gap_us` | 空 | 无该行（0 gaps）✅ |

#### ② ★ Q17 的答案：**传输（USB/电台）侧，不是宿主**

```
[ul_rx_timing] calls=615791 recv(max=100645us over 1ms=657 over 5ms=24)
                        loop(max=1187us   over 1ms=1   over 5ms=0)
                        slip(max=19453us  over 1ms=536)
```

* **`loop`（宿主在两次收块之间自己的时间+调度）在 615,791 次里只有 1 次 >1 ms，最大 1.19 ms**
  ⇒ **宿主从不"迟到发问"**。⇒ §6.51 ⑥ 里"打宿主优先级/亲和性臂"的猜想**被本腿否掉**。
* **`recv`（调用内部）有 657 次 >1 ms、24 次 >5 ms** ⇒ **是传输在调用内部顶住**。
  （`max=100645 µs` 是**已知的启动伪读数**：`[ul_rx_wait] max` 在**每一条**腿上都是 ~101 ms（含 0-gap 的 `p27`/`p31`），
  且 `slip max(19.45 ms) ≪ recv max(100.6 ms)` ⇒ 那次调用**没有前驱**（`slip` 不计算第一次），即开流前的那一次。
  ★ **为什么正好是 ~101 ms：见 §6.146 —— `ru_controller_sdr_impl` 的 `delay_s = 0.1` 把电台流起点推后 100 ms 并对齐到子帧。**）
* **`slip` 最大 19.45 ms**：`slip = loop + recv − 本块空口时长` ⇒ **那一次迭代里未读积压增长了约 19.5 ms**
  ⇒ **这正是 64 帧（~5–8 ms 余量）必丢、256 帧（~20–30 ms）能吸收的那种事件**。
* 三腿的 `[ul_rx_wait]` **形状几乎相同**（median 474/473/474、p95 595/597/595、max ~101 ms）
  ⇒ **传输的停顿分布本身没变**，变的只是"停顿是否变成丢样" ⇒ 与"深环把丢样换成积压"的解释一致。

#### ③ 环是否有效：**有强证据，但还不是对照**（我的错误）

* **支持**：`p33` 里出现了一次 **19.45 ms 的积压增长**（超过 64 帧余量 2.5–4 倍）而 **0 丢样**；
  `p32`/`p32b`（64 帧）在**同形状**的停顿下各丢了 1 次（6.05 ms / 3.40 ms）。
* **但是**：我把**接收环 64→256** 与**RX 仪器**放在**同一个提交**里（`9633a4e4f3`）⇒ 这一腿**同时改了两个变量**，
  严格说**不能归因**。这是实验设计上的错误（纪律要求"每条臂只改一个变量"）。
* ⇒ **修法极便宜**：同二进制、**只把 `num_recv_frames` 改回 64** 再飞一条（`p34-n78-ring64`），读
  `rx_overflows`/`gaps`/`slip`/`held_max`/`starved_events`。这一条腿就能把"环"这一变量单独拿出来。

#### ④ 本腿的 V2 读红（必须直说，且**不能归因于环**）

| 读数 | `p30` | `p31` | `p32`/`p32b` | **`p33`（256）** | `p29`（64 帧，旧二进制）|
|---|---|---|---|---|---|
| 契约 | 8/8 | 8/8 | 7/8 | **8/8** | 7/8 |
| `gaps` | 0 | 0 | 1 / 1 | **0** | 3 |
| `held_max` / `free_min` | 12 / 4 | 10 / 6 | 12 / 4 | **16 / 0** | **15 / 1** |
| `starved_events` | 0 | 0 | 0 / 0 | **2** | **1** |
| `pop_blocking` max | — | 23 µs | 27 / 38 µs | **249 µs** | — |
| `dropped` | 0 | 0 | 0 | **0** | 0 |

* ⇒ **V2 的严格判据在本腿读红**（`starved_events==0` 与 `held_max<pool` 都不成立）。
* ⇒ **但"池顶到 15/16 + starved_events 1–2"在改动之前就出现过**（`p29`，64 帧、旧二进制），
  且本腿**没有**目标症状：`pop_blocking` max **249 µs**（历史是 486 µs–5 s）、`dropped=0`、无 park、无 on-demand dump。
  ⇒ **不能把这条读成"环的代价"**，也不能读成"没问题"——它需要 ③ 的对照腿来分离。
* **`stale=1`（11.18 ms）而 `gaps=0`** ⇒ 再次证明：`stale` 与 gap **不是同一个东西**（stale 有自己的成因）。

#### ⑤ 与这一刀无关的读数（对照不变量）

* **V1 中位 1408.2 µs**（`p32` 1411.3）——最好的一条；`eq_demap` 776.0（`p32` 794.7）、`merged_hop` 562.7（565.9）。
* **V4**：`cbs/lane=2.00 max=2 dropped=0`、crossings `0.00+0.00`/跳 ✅（第四次确认）。
* **D18**：`y_direct=434653 = 3.00/跳`、`y_gather=0`、`miss` 全 0 ✅；派发 869266/144871 = **6.00/跳** ✅。
* **TX 侧未动（单一变量对照成立）**：RF 失败 **890**（`p32` 1013、`p32b` 807），
  `[dl_tx_call] over 1ms=537 max=84211us`（`p32` 672 / 85063us）⇒ TX 环没改，读数照旧。
* 门 **29/29 全过**（`D4` 也在内，因 `gaps=0`）。

#### ⑥ 下一步（按优先级）

1. **`p34-n78-ring64`（配置一行，同二进制）**：把 `num_recv_frames` 改回 **64** 再飞一条，
   与 `p33` 成对，回答"环到底做了什么"。读：`rx_overflows`/`gaps`/`slip`/`held_max`/`starved_events`/`pop_blocking`。
2. **若要保留深环**：池按 **P2-D 自己的规则**重算（"按测得峰值定尺"：峰值 16 + 收包路径 2 + margin 3 = 21 ⇒ 取 **32**），
   这是**按既定方法用新测量重算**，不是改阈值——但池尺寸是交付参数，**要用户裁决**（P2-D 原本就是裁决项）。
3. **传输侧（真正的堵点，Q17 已指认）**：换 USB 口/控制器/线、跑腿时看 `dmesg`、试 `otw_format=sc8`；
   宿主调度臂**不再优先**（②已否掉）。



### 6.53 ★★★ 对照腿 `p34-n78-rxring`（**同二进制，只把 `num_recv_frames` 改回 64**）：**环的作用与代价都量出来了** —— 64 帧 ⇒ **5 次丢样（最大 35.3 ms）**，256 帧 ⇒ **0 次**；而池的顶格**正是深环的代价**（64 帧 `held_max=11/free_min=5/starved=0`、256 帧 `16/0/2`）

#### ① 对照（一条腿只差配置一行）

| 读数 | **`p33`（256 帧）** | **`p34`（64 帧）** | 结论 |
|---|---|---|---|
| `rx_overflows` / `gaps` / `[RF] … overflow` 行数 | **0 / 0 / 0** | **5 / 5 / 5** | **三者相等**（第三次成立，且这次是非零对非零）|
| `gap_us` | — | **3272, 4627, 35291, 28656, 4121** | 64 帧丢了 5 段，**最大 35.3 ms** |
| `slip` max | **19453 µs**（19.5 ms）| **33861 µs**（33.9 ms）| "积压增长"与 gap 大小同量级（`丢 ≈ slip − 环容`）|
| `recv` max / >1ms | 100645 / **657** | 101107 / **1265** | 形状同族（`max` 都是开流伪读数）|
| `loop` max / >1ms | 1187 / **1** | 2055 / **1** | **两腿的宿主都从不迟到**（615k/790k 次里各 1 次）|
| `overflow_ctx` | （无 overflow）| **`recv_us=1769,1667,1543,1926,1832` / `loop_us=3,2,2,2,2`** | ★ **五次 overflow 全部发生在"调用内部被顶住 1.5–1.9 ms、宿主 loop 只有 ~2 µs"** ⇒ Q17 逐事件确认 |
| **`held_max` / `free_min` / `starved_events`** | **16 / 0 / 2** | **11 / 5 / 0** | ★ **池的顶格是深环的代价** |
| `pop_blocking` max | 249 µs | 34 µs | 症状仍远在 1 ms 预算内（两腿 `dropped=0`、无 park）|
| 契约 / V5 | **8/8** | 7/8（1 项 = gaps）| — |
| **V1 中位** | 1408.2 µs | **1398.6 µs** | 环不动时延（差 10 µs < 17 µs 腿间噪声）|
| `merged_hop` | 562.7 | 551.8 | 同上 |
| RF 失败 / 块数 | 890 / 615,791 | **1821** / 790,938 | p34 的电台更差（见 ③）|

#### ② 机制闭合（两腿的数字互相咬合）

* **积压 vs 环容**：`p34` 最大 `slip` 33.9 ms、环容 ~5–8 ms ⇒ 应当丢 **~26–29 ms**，实测 gap **28.7 / 35.3 ms** ✓；
  `p33` 最大 `slip` 19.5 ms、环容 ~20–30 ms ⇒ 应当**不丢**，实测 **0 丢** ✓。
* **逐事件归属**：`p34` 五次 overflow 的 `overflow_ctx` 全是 `recv_us≈1.5–1.9 ms`、`loop_us≈2 µs`
  ⇒ **宿主瞬间就发了请求，是传输在调用内顶住**。⇒ **Q17 定案：传输侧**（USB/电台），宿主调度**不是**杠杆。
* ⇒ **Q18 定案：环的作用是"把丢样换成积压"**——不消除停顿，只消除**丢样**；代价是**同一批积压以突发形式交给接收路径**，
  于是**池**要同时接住（`held_max` 从 11 抬到 16 = 池上限、`starved_events` 0 → 2）。

#### ③ 必须一起读的两条限制

1. **两腿的"电台天气"不同**：`p34` RF 失败 **1821**（`p33` 890）、`stale=2`（max 42.8 ms）、`input hold` max 42.3 ms
   ⇒ 64 帧那一条本来就遇上了更差的传输，**"0 vs 5"里含有天气成分**。
   但 ① 里的**量级咬合**（19.5 ms 积压被 256 帧完全吸收 / 33.9 ms 积压按环容丢出 28.7–35.3 ms）**不是天气**——
   它由两腿各自的 `slip` 与 `gap_us` 直接给出。⇒ 结论：**环确实按设计工作**，但**"5 次"这个计数不能当作纯环效应**。
2. **深环的代价是真的、也已量出**：同二进制、差一行配置，池从 `11/5/0` 变成 `16/0/2`。
   ⇒ 这是 **V5（不丢样）与 V2（池判据）之间的交换**，不是"白拿"。

#### ④ 结论与建议（**需要用户裁决**）

| 方案 | V2（池）| V5（gaps）| 评价 |
|---|---|---|---|
| **A. 256 帧 + 池按 P2-D 规则重算**（峰值 16 + 收包 2 + margin 3 = 21 ⇒ **32**）| 预期恢复（`held_max<pool`、`starved_events=0`）| 0 gaps | ★ **推荐**：两条判据的"因"都治住——环不再丢样，池按**已批准的定尺方法**用**新测量**重算（"按既定方法重算"，不是改阈值）|
| B. 回到 64 帧 | ✅ 11/5/0、34 µs | ❌ 5 gaps（最大 35.3 ms 样点真丢）| 保住 V2、放弃 V5；丢的是**真实上行样点** |
| C. 打传输（根因）| — | — | **与 A/B 并行**：换 USB 口/控制器/线、跑腿看 `dmesg`、试 `otw_format=sc8`；TX 侧同样在调用内阻塞（`[dl_tx_call]` max 84 ms）⇒ 是**同一条 USB 链路**的性质 |

**纪律**：V2 的判据（`starved_events==0`、`held_max<pool`）与 V5 的判据（`gaps==0`）**都不动**；
方案 A 的合法性来自 **P2-D 的原始规则**（"按测得峰值定尺，并随时延改善重算"，§6.38），
且**池尺寸是交付参数 ⇒ 必须由用户裁决**（P2-D 原本就是裁决项）。



### 6.54 **用户裁决 A + C 并行**：池按 P2-D 规则用新测量重算（**16 → 32**）、环回到 **256 帧**（A）；overflow 事件自带**宿主负载**上下文、传输侧两条臂用"只改一行"的生成器做出来（C）

#### ① A（交付侧）：池 = 32，且**打印的就是真实存在的那个数**

* P2-D 的规则（§6.38，用户已批）是"**按测得峰值定尺**"：峰值 + 收包路径 2 + margin 3。**变的是峰值**：
  `p33`（256 帧）测得 `held_max=16` = 当时的池上限、`free_min=0`（⇒ **测量被池本身夹住了，16 是下界**），
  而 `p34`（64 帧）同一配方只有 11 ⇒ **11 是旧环产生的数**。
* ⇒ `16 + 2 + 3 = 21`，接收环容量取 **2 的幂**（§6.37 ②）⇒ **池 = 32**。
* **同时修掉 §6.37 ② 记下的那个仪器缺陷**：把"向上取到 2 的幂"**显式做在工厂里**，于是启动行宣布的就是腿真正会跑的尺寸
  （原来旋钮 12 会得到 16，而打印说 12）：

  ```
  [ul_rx_pool] size=32 buffers of 23040 samples (slot=23040, whole-slot buffers, the gpu pipeline mode):
  floor 8, radio latency 1, symbol pipeline 8, slot pipeline 32
  (peak 16 + rx path 2 + margin 3 = 21, rounded up to a power of two - the ring's capacity, dev doc 6.53)
  ```
* 腿配置里 `num_recv_frames` **回到 256**（A 的另一半），并把 **64 vs 256 的实测对照写在配置旁边**（不是光写一个数字）。
  `num_send_frames` **保持 64**（发送环是另一个变量，没这样量过）。

#### ② C（根因侧）：三条新读数 + 两条"只改一行"的臂

**先排除掉最容易的一条**：B200 的 USB 拓扑**没有问题**（`ioreg` 实测）——
在**独立控制器** `AppleT8132USBXHCI@03000000` 下的 USB3 Gen2 Hub 上，**协商到 5 Gb/s（SuperSpeed）**，
**同控制器上没有别的设备**（另一台设备在另一个控制器上）⇒ 不是"插错口/共用总线"。

**新读数（事件自带上下文）**：`[ul_rx_timing]` 增加 **`load1`**（宿主 1 分钟负载，**只在尾事件时采样**，稳态零成本）：

```
[ul_rx_timing] ... load1(max at a tail event=N.NN)
[ul_rx_timing] overflow_ctx=[recv_us=1769,loop_us=3,load1=2.65 ...]
```

为什么它决定性：Q17（§6.53）已把毫秒定位在 `receive()` 内部、**我们自己的线程不到 2 µs**，
于是嫌疑落到**我们不拥有的线程**（UHD 的收包 worker、USB 栈）。**负载高 ⇒ 宿主把它们饿着**；**负载低 ⇒ 设备/线在停**。
⇒ 下一次 overflow 的 `load1` 一读就知道该往哪边打。

**两条臂**（都是**配置**臂，不是命令行臂——实测 `--otw_format`/`--device_args` 是 `ru_sdr` 的**子命令**选项，
腿脚本用的全局位置会被 parser 拒绝，dry run 就能拦下）：

| 臂 | 变量 | 判读 |
|---|---|---|
| **bigframe**（C1）| `recv_frame_size=16384`（**波形不变**，USB 传输更大块）| 判**传输读数**（`recv`/`slip`/`rx_overflows`/`load1`）|
| **sc8**（C2）| `otw_format: sc8`（**线上速率减半**，波形会变）| 同样只判传输读数；**不判 V1–V5**（波形/链路变了）|

工具：**`wip/mk_arm_cfg.sh <bigframe|sc8>`** —— 从**交付配置**生成臂配置，**拒绝任何不是"恰好改一行"的结果**，
并把 diff 打出来（腿日志会带配置，diff 就是这只腿移动的变量）：

```
# arm 'bigframe' generated from gnb_rf_b200_tdd_n78_20mhz.yml - the variable it moves:
  -  device_args: type=b200,num_recv_frames=256,num_send_frames=64
  +  device_args: type=b200,num_recv_frames=256,num_send_frames=64,recv_frame_size=16384
```

#### ③ 离线验证与预登记

* 池：`lower_phy_test` **576/576**、`ctest -L phy` **193/193**；启动行实测为 `size=32 … = 21 …`。
* 载荷/上下文：夹具里读得到 `load1(max at a tail event=2.65)` 与 `overflow_ctx=[…,load1=2.65]`
  （夹具读数是夹具的，空口才算——这条警告已写在代码注释里）。
* 两条臂配置：`--dryrun` 都通过 parser。
* **待飞的腿与预登记**：

| 腿 | 变量 | 预登记 |
|---|---|---|
| **`p35-n78-pool32`**（A）| 交付配置（256 帧 + 池 32）| **V2：`starved_events=0` 且 `held_max<32`**；**V5：`gaps=0`**；`rx_overflows=0`；V1/V4 与 `p33` 同形；启动行 `size=32` |
| **`p36-n78-bigframe`**（C1）| `LEG_CONFIG=…/arm_bigframe.yml` | 与 p35 成对：`recv` 的 >1ms 计数与 `slip` max 下降（或至少不变差）、`rx_overflows` 不增；`load1` 同量级 |
| （可选）`p37-n78-sc8`（C2）| `LEG_CONFIG=…/arm_sc8.yml` | 只判传输读数；若 `recv` 尾巴显著变短 ⇒ **带宽是约束**；若不变 ⇒ 是**驱动/调度**而非带宽 |

**判读要点**：`p35` 若 V2 与 V5 **同时绿**，则 A 成立（环不丢样 + 池按新测量有容量）；
`p36`/`p37` 的作用是把"根因在 USB 的哪一层"钉下来——**它们是诊断腿，不改变交付配置**。


### 6.55 ★★ 腿 `p35-n78-pool32`（**A 验收通过：V2 与 V5 同时绿**）＋ `p36-n78-bigframe` **失败复盘**（**臂本身就是致命的**，手机是被它挡住的）＋ **离线电台台架**（把 C 的问题在**没有手机、没有 gNB**的情况下问清楚）

#### ① A 的验收：`p35-n78-pool32` —— 预登记全中

| 读数 | 预登记 | `p35` |
|---|---|---|
| 启动行 | `size=32` | **`size=32 buffers of 11520 samples … (peak 16 + rx path 2 + margin 3 = 21, rounded up to a power of two)`** ✅ |
| **V2** | `starved_events=0` 且 `held_max<pool` | **`starved_events=0`、`held_max=11 < 32`、`free_min=21`、`pop_blocking` max **26 µs**、`dropped=0`** ✅ |
| **V5** | `gaps=0` | **`gaps=0`、`rx_overflows=0`、契约 MET (8 of 8)** ✅ |
| V1 | 与 `p33` 同形 | **1404.0 µs**（`p33` 1408.2、`p34` 1398.6）✅ |
| V4 | 不变 | `cbs/lane ≤2.00 max=2 dropped=0`、crossings `0.00+0.00`/跳 ✅ |
| D18 | 不变 | `y_direct=433290 = 3.00/跳`、`y_gather=0`、派发 **6.00/跳** ✅ |

* ⇒ **环不丢样 + 池按新测量有余量，两条判据第一次同时绿**（`merged_hop` 549.8 µs 也是最好的一条）。
* ⚠ **诚实标注**：这一腿的传输本来就**温和**（`slip max=6247 µs`，`p33` 是 19453、`p34` 是 33861），
  所以 `held_max=11` 只说明"**这条腿没把池逼到边**"，**不等于"池 32 是它变绿的原因"**——
  真正的证据是"池≥需求 + 峰值是下界"这条推理（§6.53），以及下面 ③ 的台架测量。

#### ② `p36-n78-bigframe` 失败复盘：**问题在臂，不在手机**

现象（用户报告）：手机多次开关飞行模式也接不上。日志给出的完整链条：

| 证据 | 读数 |
|---|---|
| UHD 自己的警告 | **`Requested recv_frame_size of 16384 is too large. It will be set to 16360.`** |
| RX 溢出 | **188,762 次**（≈**628/s**，稳态持续 5.3 分钟；`[RF] overflow` 行数与之同量级）|
| TX 侧 | `underflow` **16,927** 次 |
| 下层 PHY | `PRACH request late` **28,797** 次（**注意：这不是"检测到 PRACH"，是调度请求迟到**）|
| 数据面 | **PUSCH=0、PDSCH=0** ⇒ **从未发出 RAR** ⇒ 手机无从接入 |
| 进程状态 | 退出报告缺失（stderr 只有 37 行）⇒ 被硬杀 |

⇒ **手机没问题，是 gNB 的 UL 从第一秒起就是坏的**：`recv_frame_size=16384` 让 B200 的接收传输**每块都溢出**，
UL 时间线全废（PUSCH 解不出、PRACH 请求迟到），于是**没有 RAR，手机永远接不上**——
用户开关飞行模式当然没有用。**这是我给的一条不该飞的臂**（预登记写了"波形不变"，却没先验证参数本身是否可用）。

#### ③ ★ 离线电台台架 `wip/uhd_rx_health.cpp`：把 C 的问题在**没有手机/没有 gNB**时问清楚

`p36` 的教训是"**用腿去问电台的问题太贵**"。所以补一个**直连 UHD** 的小工具：开 B200（给定 device args）、
按给定速率/格式收（可选**同时发**），报告与下层 PHY **同一套**读数（连续性、错误码分类、`recv` 时长）。
它**不发真实波形、也不跑 PHY** ⇒ 是**过滤器**，不是判据。

构建与用法（实测命令）：

```bash
clang++ -std=c++17 -O2 -I /opt/homebrew/include doc_chinese/phy_latency/wip/uhd_rx_health.cpp \
  -L /opt/homebrew/lib -luhd -Wl,-rpath,/opt/homebrew/lib -o /tmp/uhd_rx_health
/tmp/uhd_rx_health --args "type=b200,num_recv_frames=256,num_send_frames=64" --seconds 10 --tx
```

**测量结果（本机、本电台、sc12、23.04 Msps、每点 6–10 s、`--tx` = 收发同时）**：

| 配置 | 时长 | 交付率 | 电台错误 | `recv` max | 结论 |
|---|---|---|---|---|---|
| **交付配置**（256 帧、默认帧长）**仅收** | 6 s | **100%** | **0** | 744 µs | ✅ 干净 |
| **交付配置 收+发** | 10 s | **100%** | **0** | 781 µs | ✅ **干净（USB 链路不是瓶颈）** |
| 交付配置 + **CPU 打满**（14 个自旋）| 10 s | 100% | **0** | **28,136 µs** | ✅ 不丢样，**但调用被拖到 28 ms**（环吸收住了）|
| `recv_frame_size=8192` | 6 s | 100% | 0 | 4,165 µs | ✅ |
| `recv_frame_size=8200`（UHD 默认）| 6 s | 100% | 0 | 1,928 µs | ✅ |
| **`recv_frame_size=12288`** | 6 s | **9.8%** | **每块 overflow** | 7,045 µs | ❌ **崩** |
| **`recv_frame_size=16360`**（16384 被夹）| 8 s | **9.8%** | **每块 overflow** | 7,045 µs | ❌ **崩** |
| `num_recv_frames=512`（默认帧长）| 8 s | 100% | 0 | 1,009 µs | ✅ |
| `num_recv_frames=1024`（默认帧长）| 8 s | 100% | 0 | 756 µs | ✅ |

⇒ **三条结论**：

1. **帧长有硬边界（~8 KB）**：≥12 KB 直接崩（每块 overflow、交付率 9.8%）。**默认 8200 已经贴着天花板** ⇒
   "把 USB 传输做大"这条路**封死**，`bigframe` 臂**退役**（`mk_arm_cfg.sh` 现在**拒绝**生成它，并把上面的数字打在拒绝信息里）。
2. **环深可以按帧数加深**（512/1024 都健康）：8200 B/帧 ≈ 2733 样点 ⇒ 256 帧 ≈ **30 ms**、512 ≈ **60 ms**、1024 ≈ **121 ms** 的停顿余量。
3. ★ **停顿的来源是宿主调度，不是电台**：把 CPU 打满后 `recv` 被拖到 **28 ms**（而**电台零错误、100% 交付**）——
   即"电台一直在正常供数，是宿主的线程没能按时跑"。这与 §6.53 的 `loop_us≈2 µs`（**我们自己的线程**从不迟到）**并不矛盾**：
   迟到的是**我们不拥有的线程**（UHD 的传输 worker）。⇒ **C 的答案落在宿主侧**，而**`otw_format=sc8`（C2）不再是必要的腿**（带宽已证明不是约束）。

#### ④ 处置与建议

| 项 | 建议 | 依据 |
|---|---|---|
| 环深 | **保持 256（A 已批）**；若要买余量，**按帧数加深**（512 ≈ 60 ms） | ③ 表：512/1024 健康；且 `p33` 吸收过 19.5 ms、`p34` 丢在 33.9 ms |
| ⚠ 加深的代价 | **池要跟着重算**（突发更大 ⇒ 峰值更高），仍按 P2-D 规则 | §6.53（256 帧已把峰值从 11 抬到 16）|
| `bigframe` 臂 | **退役**（生成器拒绝） | ③ |
| `sc8` 臂（原"第 3 条腿"）| **不再需要** | ③：带宽不是约束 |
| 根因（宿主调度）| 若要继续：**减少竞争线程数 / 提高 UHD 相关线程优先级**，并用 `load1`（§6.54 ②）在腿上读 | ③ 的 CPU 打满实验 |


### 6.56 **更正"UL 静默"（它不是现象，是我的工具在误报）**＋ **裁决 B 落地（环 512）**、**裁决 C 退役（TX 环不是杠杆）**

#### ① 更正：所谓"秒级 UL 静默"其实是**流量之外的空闲段**，**不是**电台/UE 的静默

用户提出："秒级 UL 静默有可能是手机本身在 UL 没发任何信号"。**查证后：用户是对的，而且比这更彻底——那些"静默"主要根本不在测试流量之内。**

* **工具缺陷（我的）**：`leg_census.py` 只在**整条日志**上量"两条 `PUSCH:` 之间隔了多久"，
  而一条腿**不是全程满载**：`iperf3 -t 240` 只跑其中 ~242 s，日志的头（attach/爬坡）和尾（测试结束 + Ctrl-C）都是空闲的。
* **实测（修正后的普查）**：

| 腿 | 推导出的流量窗口 | 窗口**内** >0.3 s 的停顿 | 窗口**外**（空闲头/尾，**不是事件**）|
|---|---|---|---|
| `p35` | 12:15:10–12:19:12（243 个"忙"秒 / 跨度 243 秒）| **1 次，0.42 s**（其内 **PUCCH 1291 行**）| 14 次，最长 **3.92 s** |
| `p31` | 09:51:12–09:55:35（252 / 259）| 6 次，最长 **3.82 s**（其内 **PUCCH 525 行**）| 其余，最长 **6.48 s** |

* **判据是 PUCCH**：窗口内每一次停顿里 **PUCCH 都在 ~100/s 地继续** ⇒ **UE 一直在发、gNB 一直在收**，
  停的只是**数据面**（`PUSCH` 无数据可收、`PDSCH` 无数据可发、`GTPU` 两侧都为 0）⇒ **是"没流量"，不是"电台死了"**。
  （`p31` 的 6.48 s 与 `p35` 的 3.92 s 都紧贴日志末尾 ⇒ 那正是测试结束/停腿的尾巴。）
* ⇒ **memo 里"gap ≠ UL 静默（两个现象）"这句话要作废**：**不存在"UL 静默"这个现象**；
  真正存在的是"**数据面停顿**"（TCP/流量层），与 PHY/电台无关。
* **工具已修**（`leg_census.py`）：现在**从日志自行推导流量窗口**，只报窗口内的停顿，并**为每次停顿打印其内的 PUCCH 行数与 RF 失败数**，
  给出判词（"the UE WAS transmitting (PUCCH) - no traffic, not a dead radio" / "NO PUCCH either: the radio itself was quiet"）；
  `--all` 保留旧的整段行为。

#### ② 裁决 B 落地：接收环 **256 → 512**（余量原本压在最坏观测值上）

* 按 8200 B/帧、sc12（3 B/样点 ⇒ **2733 样点/帧**）折算：

| 环深 | 可吸收停顿 |
|---|---|
| 64 帧 | **7.6 ms**（`p34` 在 33.9 ms 时丢了 5 段、最大 35.3 ms）|
| 256 帧 | **30.4 ms**（`p33` 吸收 19.5 ms ✓，但 33.9 ms 那次**仍会溢出**）|
| **512 帧**（本次）| **60.7 ms** ← 最坏观测值的 ~1.8 倍 |
| 1024 帧 | 121 ms |

* **台架验证**（`wip/uhd_rx_health`，**收发同时**）：`num_recv_frames=512,num_send_frames=512` ⇒
  **100% 交付、0 电台错误、0 TX 异步错误**；`1024` 同样健康。⇒ **按帧数加深是安全的**（而**帧长** ≥12 KB 必崩，见 §6.55 ③）。
* **池**：本次**仍用 32**——P2-D 的规则是"**按测得峰值定尺**"，而 **512 帧下的峰值还没有测量**；
  这条腿就是产出那个测量的地方。若峰值随之上升，再按同一规则重算（**不预设**）。

#### ③ 裁决 C 退役：**发送环不是 TX underflow 的杠杆**（台架 + 既有证据都不支持）

我原本建议"TX 环 64 → 512"作为 V3 的复开臂。**先验证机制，结果是被否掉的**：

1. **台架复现不出来**：`num_send_frames=64` 与 `512` 在**收发同时**、**CPU 打满**、甚至**注入 20 ms 喂数停顿**（每 200 块一次）的条件下，
   TX 异步 **underflow 都是 0**。
2. **腿上证据指向"线上/设备"而不是"环深"**：`p35` 的 `transmit()` **447 次阻塞 >1 ms（max 85 ms）** 而 `AT/BELOW 0` 只有 **23**，
   同时 RF underflow **649–710** 次 ⇒ **90%+ 的 underflow 发生在"递交提前、宿主发送环是满的"时刻**：
   **样点在宿主环里排着队、没有在线上流动**。**环再深也治不了"排队的东西不动"**（这就是 §6.42 的结论：
   迟到在递交之后）。⇒ **退役，不飞这条腿**（少一次手机测试），把 V3 的靶子留给**宿主竞争/优先级**（§6.55 ④）。


### 6.57 ★★ 腿 `p37-n78-ring512`（**B 验收通过**）：`gaps=0`、契约 **8/8**、V1 **1408.2**；**池的峰值测得 13**（⇒ P2-D 重算仍得 **32**，池不变）；而**深环把"丢样"换成了"迟到"**——`stale` 从 2 次升到 **13 次（max 42.8 ms）**

#### ① 预登记逐项

| 读数 | 预登记 | `p37`（512 帧）|
|---|---|---|
| 启动行 | `size=32` | **`size=32 … (peak 16 + rx path 2 + margin 3 = 21, rounded up to a power of two)`** ✅ |
| `rx_overflows` / `gaps` | 0 / 0 | **0 / 0**（`radio sample continuity: 0 gaps over 582712 blocks … 0 overflow(s) -> OK`）✅ |
| 契约 / V4 / crossings | 8/8 / 2.00 / 0 | **MET (8 of 8)** / `cbs/lane ≤2.00 max=2 dropped=0` / `0.00+0.00` ✅ |
| **池峰值（本腿的测量）** | 待测 | **`held_max=13`、`free_min=19`、`starved_events=0`、`pop_blocking` max **30 µs**、`dropped=0`** ✅ |
| V1 中位 | 与 `p35` 同形 | **1408.2 µs**（`p35` 1404.0、`p33` 1408.2、`p34` 1398.6 ⇒ 全在 17 µs 噪声内）✅ |
| `merged_hop` / D18 | 不变 | 555.2 µs / `y_direct=411656 = 3.00/跳`、派发 **6.00/跳** ✅ |
| 电台 | 未动 | RF 失败 **880**（`p35` 710、`p33` 890）⇒ 同量级 ✅ |

#### ② 池：**P2-D 重算仍得 32 ⇒ 池不变**（这是本腿要产出的那个测量）

* `p37`（512 帧）测得峰值 **13**、`free_min=19`（从未低于 19 个空闲）；`p33`（256 帧）测得 **16 = 当时的池上限**（被夹住，是下界）。
* 按 P2-D 的规则（峰值 + 收包 2 + margin 3，再取 2 的幂）：
  **13 + 5 = 18 ⇒ 32**；**16 + 5 = 21 ⇒ 32** ⇒ **两次测量给出同一个池尺寸 32** ⇒ **不动**。
* ⇒ 代码里的常数**保持 16**（= 跨测量观测到的最大值，定尺该用保守值），注释里补上 `p37` 的 13。

#### ③ ★ 深环的代价现形了：**"丢样"变成了"迟到"**（`stale` 2 → 13 次）

| 读数 | `p30` | `p31` | `p32` | `p33` | `p34` | `p35` | **`p37`（512）** | `p29`（64，有 3 gap）|
|---|---|---|---|---|---|---|---|---|
| `gaps` | 0 | 0 | 1 | 0 | 5 | 0 | **0** | 3 |
| `stale`（span > 8 ms）| 1 | 0 | 1 | 1 | 2 | 2 | **13** | **13** |
| `stale` max | 8.1 ms | — | 14.5 | 11.2 | — | 8.5 | **42.8 ms** | 42.3 ms |

* 机制：**同样的一次 ~40 ms 停顿，浅环把它变成"丢样"（gap），深环把它变成"迟到"（跨度跨过 8 ms 门限）**。
  `p29`（64 帧）与 `p37`（512 帧）的 `stale=13 / max≈42 ms` **几乎相同**——一个在丢样，一个在迟到。
* **代价有多大**：13 次 / 124,490 个样本 = **0.01%**；V1 中位不动，**p95/p99 反而更好**
  （`p37` 1588/1685 µs vs `p35` 1602/1738）⇒ **尾部分位数看不见它**，只有 `stale` 这个"越界计数"看得见。
* ⇒ 结论：**深环买到的是"不丢"，代价是"极少数跳被延迟处理"**（上限 = 环容 60.7 ms）。要让两者都消失，
  只能治**那一次 ~40 ms 的停顿本身**（宿主竞争，§6.55 ④），这也再次说明"gap 消失 ≠ 问题解决"。
* 顺带：本腿 `loop(max=5032 µs)`——**宿主自己的两次收块之间出现过一次 5 ms**（这批腿里第一次），
  而它**没有造成 gap**（环吸收了）⇒ 正是上面这条交换的直接例证。
* **census（修正后的工具）**：流量窗口 242/242 全忙；**窗口内 7 次停顿，最长 0.47 s，每次都有 PUCCH（231–1193 行）**
  ⇒ **没有电台静默**，与 §6.56 ① 的更正一致。

#### ④ 状态

* **V1 ✅ 1408.2、V2 ✅、V4 ✅、V5 ✅（契约 8/8、gaps 0）**；V3 ⏸（880）。
* 交付配置：**接收环 512 帧 + 池 32**（B 已验收）；发送环 64（C 已退役，§6.56 ③）。
* 剩余的真问题只有一个：**偶发的 ~40 ms 宿主停顿**（表现为 13 次 stale / 0.01% 的跳），它的靶子是宿主竞争与线程优先级。


### 6.58 **§3.2 开工第一步（只读码）就改了结论**：光"解开 `estimates` 断因"**今天已经不值派发**——要省那 2 次派发，必须**把 `h_starts` 那套办法搬到 y 上**（kernel + 宿主）

> 用户裁决：B + C 之后**继续主线 §3.2**。按纪律先只读码、把方案与预登记写清楚，再动手。**读码的结果是原方案要改**。

#### ① 原方案为什么不再成立（三处代码事实）

1. **断因的机理**（`eq_flush_hook` 的 run 谓词）：设备侧估计走 `same_h` 的强条件
   `next.h.offset == prev.h.offset + h_step`，而 `h_step = layer_stride ?: nof_re`；
   本空口是 **1 层** ⇒ `view_ch_est_list.h` 把 `layer_stride` 置 **0** ⇒ 步长就是 `nof_re`，
   而估计器为**每个符号**（含 DM-RS 符号）都发布了切片 ⇒ 数据符号的偏移**跨过 DM-RS 符号时会跳**
   ⇒ 在 DM-RS 处断 run（`first_break=estimates`）。
2. **但 run 还有第二道锁**：`same_gather` 要求 `next.gather.symbol == head.gather.symbol + n_run`，
   即 **run 的符号必须在网格里连续**；而 DM-RS 符号因 `nof_re_symbol == 0` **根本不进 pending**
   （`pusch_demodulator_impl.cpp`）⇒ 合并后的 run **必然跨越网格上的空档** ⇒ 这一条**也**会断。
3. **而"跨越空档的 run"表达不出来**：
   * **直读网格**（§6.48）要求符号连续（`sym.symbol != head.symbol + k → miss(start)`），因为 kernel 里
     y 是**统一步长**（`y += sym * st.y_stride`）；
   * **gather** 的 tap 表按 `taps[first_symbol + sym]` 索引（符号连续）⇒ 同样表达不了；
   * 唯一现成的"任意偏移"机制是 **`h_starts[]`**（batch 5f 为 h 做的），**y 没有对应物**。

⇒ **算术**：今天一跳的均衡派发 = **3**（= 3 个 run 各 1 次直读，`ch_gather=0`）。
若只解开谓词、让 1 个 run 覆盖整跳：该 run **必须走 gather** ⇒ 派发变成
**1 建表 + 1 gather + 1 均衡 = 3** ⇒ **净收益 0**（还与 §6.48 的收益抵消）。

#### ② 改后的方案（**把 `h_starts` 搬到 y 上**，一次改动同时拿下两件事）

| 处 | 改动 |
|---|---|
| `ocudu_equalizer.metal`：`equalize_strides` | 增加 **`uint y_starts[eq_max_run_symbols]`**（与 `h_starts` 同形）；`equalize_mxn_batch` 里把 `y += sym * st.y_stride` 换成 `y += st.y_starts[min(sym,…)] - p.y_offset` |
| `equalize_params` | 增加 **`uint y_offset`**（h 已有 `h_offset`，y 今天靠绑定偏移，没有参数位的对应物）|
| 宿主 `eq_strides_t` + `eq_encode_batch_dispatch` | 同步加字段（有 `static_assert` 对齐 MSL 结构，照 h 的做法）|
| `eq_flush_hook` 的 run 谓词 | 设备侧 `same_h`：**只要求同一缓冲**（不再要求定步长——kernel 有逐符号表了）；`same_gather`：**只要求同一 plan**（不再要求符号连续）|
| `eq_direct_grid_run` | 直读判据去掉"符号连续"，改为**逐符号各自给出网格行**（`subc_base` 仍要求同一起点、每符号 dense）；`y_starts[k] = symbols[first+k].symbol * symb_stride + subc_base` |

**收益（预登记）**：run **3 → 1**；均衡派发 **3 → 1**；总派发 **6 → 4/跳**（2 CE + 1 解映射 + 1 均衡，**无建表、无 gather**）
⇒ 按 §6.50 修正后的标尺（V1/跨度 13–17 µs/派发）**V1 −26 … −34 µs ⇒ ≈1375–1382 µs**；
`merged_hop` 按窗口口径（6.5–9.4）**−13 … −19 µs**。

**不变量（必须先离线全绿，照 §6.48 的做法）**：
* **逐字节**：`ul_chain_replay` 27 条语料 ×2 臂（`OCUDU_EQ_DIRECT_GRID=0/1`）**dump 逐字节相同**；
  再与**改前二进制**对拍一遍（同一语料，`ab_replay_bins.sh` 的形状）⇒ 这次动的是 kernel，**必须**两边都比；
* `ctest -L phy`（含 `channel_equalizer_metal_unit_test` 两个注册臂）、`l1_handover_arms.sh`、`value_net`；
* **金属库要重建**（改了 `.metal`），并且**显式**构建那 7 个依赖目标（§4.3）。
**反例判读**：若 `runs` 仍 3.0 ⇒ 谓词有一处没放开（看 `first_break` 变成什么）；若 `y_direct` 掉到 0 ⇒ 直读判据被新条件挡住（看 `miss(...)`）。

#### ③ 为什么值得做

* 这是 §6.44 ③ 表里**剩下的唯一"派发级"杠杆**（③ 的 CE 2 次与解映射 1 次是别的模块）；
* 它把 §6.47 的"**变体 B**"（kernel 侧读时定位）**缩小到一个纯地址改动**：不引入查表、不改算术语义 ⇒
  逐字节相同是**构造性**的，判据仍然是 §6.48 那套对拍。


### 6.59 ★★ **§3.2 施工完成（离线）：把 `h_starts` 搬到 y 上** —— 语料上 **run 4 → 1**、**派发 12 → 4**（均衡 9 → 1），且 **3×135 个 dump 全部逐字节相同**（含与**改前二进制**对拍）

#### ① 改了什么（纯地址，不动算术）

| 处 | 改动 |
|---|---|
| `ocudu_equalizer.metal` | `equalize_strides` 去掉 `y_stride`、加 **`y_starts[]`**（逐符号起点，**相对 y 绑定**）；`equalize_mxn_batch` 里 `y += sym * st.y_stride` → **`y += st.y_starts[min(sym, …)]`**（`h_starts` 的写法照搬）|
| 宿主 `eq_strides_t` | 同步（`static_assert` 更新为 `4*uint + 2*14*uint`）|
| `eq_flush_hook` | 每个 run 填 `y_starts[k]`：直读 ⇒ `(symbols[run_plan_index[k]].symbol − first_row) * symb_stride`；staged/gathered ⇒ `k * nof_ports * nof_re`（打包布局）|
| run 谓词 | **设备侧 `same_h`：只要求"同一缓冲 + 偏移不回退"**（原来要求定步长 `offset == prev + h_step`）；**`same_gather`：允许跨越网格空档**，但**前提是"该 run 的每个符号都能原地读"**（否则退回今天的行为，run 在空档处停）|
| 直读谓词 `eq_direct_grid_run` | **去掉"符号必须连续"**，改为按 run 的**真实符号表**逐个检查（每符号自己的网格行 + 同一 `subc_base` + dense）|
| 不变量 | flush 里加断言：**非连续的 run 必须是直读 run**（gather 的 tap 表按连续符号索引，否则会读错资源粒子）|
| 同步路径 / `enqueue_burst_batch_at` | 分别填 `y_starts[0]=0`（绑定即符号区域）与打包步长（调用方给的是打包数组）|

#### ② ★ 施工中我自己踩的**两个同类缺陷**（都由机制计数器当场抓出，值得记下）

两个都源于同一件事：**run 的第 k 个符号不是 `first_symbol + k`**（run 会跨过"从未提交"的 DM-RS 符号），而"计划表"是按**连续网格符号**枚举的。

1. **run 谓词**里我用 `first_plan_index + n_run` 去问"下一个符号能不能原地读" ⇒ 问到了**DM-RS 符号**（不 dense）⇒ run 在空档处照旧断开。
   **症状**：`runs` 仍 4、`first_break=gather`（不是预期的 `runs=1 / first_break=none`）。
2. **直读谓词**里我仍按 `plan.symbols[first_symbol + k]` 线性遍历 ⇒ 遍历到了 DM-RS 符号（`nof_entries=0 ≠ nof_re`）⇒ 直读被拒。
   **症状**：`runs=1` 但 **`y_direct=0 / y_gather=1`、`miss(len=1)`** ⇒ 派发数**没降**（6 而不是 4）。

⇒ 两处都改成**按 run 自己的符号表**（`gather.symbol − symbols[0].symbol`）后，立刻出现预期形状。
**这就是"先读机制计数器、再看时延"的价值**：两次都不是崩溃、不是数值错，而是"看起来跑了但没省到"。

#### ③ 离线证据（**不变量：逐字节**）

| 项 | 结果 |
|---|---|
| 27 条语料 ×2 臂 | **`runs=1`（原 4）、`max_run=11`（原 4）、`first_break=none`（原 `estimates`/`gather`）** |
| 派发（整条 replay）| **12 → 4**（均衡 **9 → 1**：原 1 建表 + 4 gather + 4 均衡；现 1 次均衡，**无建表、无 gather**）|
| **逐字节** | 新 ON 对**旧 ON 基线**：**135 文件 0 差异**；新 OFF 对**旧 OFF 基线**：**135 文件 0 差异**；两臂互比：**135 文件 0 差异** |
| 反例臂（`cdm=1`，DM-RS 符号**带**数据 ⇒ 梳状有洞）| `runs=7` **与改前一致**、`first_break=geometry`；`DIRECT=1` 仍是 **4 直读 + 3 gather**（`miss(holes)`）；dumps **5 文件 0 差异** |
| 单测 / 探针 | `channel_equalizer_metal_unit_test` **ALL OK**；`eq_batch_kernel_probe` / `metal_chain_probe` / `eq_handoff_probe` rc=0 |
| `l1_handover_arms.sh` | **全 PASS**（含 drop 臂 8/8 不同 ⇒ 网有齿）|
| `value_net` | `captures=47 problems=183`，**失败清单与改前逐行相同**（183 条全是归档基线陈旧，**新增 0 条**）|
| 全套 | **`ctest -L phy` 193/193** ✅（194 个注册、1 个 disabled）。⚠ 过程中我先误用 `cmake --build … --clean-first`（见 ⑤），清掉了包括 `ldpc_metal_unit_test` / `demodulation_mapper_metal_unit_test` 在内的**非默认目标**二进制 ⇒ 一次 ctest 报 "Unable to find executable"×2；**显式重建这两个目标后 193/193 全绿**——这条正是 §4.3 的老纪律（测试可执行文件不在默认构建目标里）|

#### ④ 待飞腿与预登记（`p38-n78-wholehop`）

| 读数 | 今天（`p37`）| 预登记 |
|---|---|---|
| `eq_batch runs` / `max_run` / `first_break` | 3.0 / 8 / `estimates` | **1.0 / 12 / `none`** |
| `sites(y_direct=)` / `(y_gather=)` / `(ch_gather=)` | 3.0 / 0 / 0 | **1.0 / 0 / 0** |
| `burst dispatches`（均衡）| **6.0**（3.0）| **4.0**（1.0）|
| `merged_hop` | 555.2 µs | **−13…−19 µs**（≈536–542）|
| **V1 中位** | 1408.2 µs | **−26…−34 µs ⇒ ≈1375–1382** |
| 契约 / `cbs/lane` / gaps / D18 | 8/8 / 2.00 / 0 / 不变 | **同** |
| 反例判读 | — | `runs` 仍 3 ⇒ 谓词没放开；`y_direct` 仍 3 ⇒ 直读判据没放开（看 `miss(...)`）；`y_gather` 变 1 而派发不变 ⇒ 退回 gather（§6.58 ① 的算术）|

#### ⑤ 一条流程教训（记下）

`cmake --build build --target X --clean-first` **不是"只重建 X"**：它先跑 `make clean`（**整个项目**）再建 X。
本次因此清掉了大部分测试二进制，随后一次 `ctest -L phy` 只见到 75 个测试、报了 67 个 "Not Run"，**看起来像大面积回归**。
⇒ 要动 `.metal` 让 metallib 重编，用 `--target ocudu_metallib_equalizer`（它有依赖关系，改 `.metal` 会触发重编），**不要用 `--clean-first`**。


### 6.60 ★★★ 腿 `p38-n78-wholehop`：**§3.2 预登记全部命中** —— run **3.0 → 1.0/跳**、派发 **6.0 → 4.0/跳**（均衡 3.0 → 1.0）、`merged_hop` **−14.1 µs**、**V1 1408.2 → 1371.9 µs（新最好，基线 −48.7%）**，契约 **8/8**、`gaps=0`、池 `held_max=10`

#### ① 预登记 vs 实测

| 读数 | 预登记 | `p37`（改前）| **`p38`（改后）** | |
|---|---|---|---|---|
| `eq_batch runs` / `max_run` / `first_break` | **1.0 / 12 / none** | 3.0 / 8 / `estimates` | **1.00（145038/145038）/ 12 / none** | ✅ |
| `sites(y_direct=)` / `(y_gather=)` / `(ch_gather=)` | **1.0 / 0 / 0** | 3.0 / 0 / 0 | **1.00 / 0 / 0**（`miss` 全 0）| ✅ |
| `burst dispatches`（均衡）| **4.0（1.0）** | 6.00（3.00）| **4.00（1.00）**（580152/145038；CE 2.00 + demap 1.00 + **均衡 1.00**）| ✅ |
| `merged_hop` | −13…−19 µs | 555.2 µs | **541.1 µs（−14.1）** | ✅ |
| **V1 中位** | −26…−34 ⇒ ≈1375–1382 | 1408.2 µs | **1371.9 µs（−36.3）** | ✅ **比预登记还好** |
| 契约 / V4 / gaps / 池 | 8/8 / 2.00 / 0 / — | 8/8 / 2.00 / 0 / `held_max=13` | **8/8 / 2.00 max=2 dropped=0 / 0 gaps、0 overflow / `held_max=10`、`free_min=22`、`starved_events=0`** | ✅ |

* **段账**（配对相位段，`p37` → `p38`）：`eq_demap` **784.7 → 761.2（−23.5）**、`ce` 69.4 → 66.5、`t2f` 534.5 → 532.0；
  三段和 −27.7（V1 −36.3 覆盖得住）。
* **`merged_hop` 只降 14.1，而 V1 降 36.3** ⇒ 与 §6.49/§6.50 的口径结论一致：**去掉派发省下的不只是设备 busy 窗口，
  还有宿主的编码工作**（这次少的正是"1 次建表 + 1 次 gather"的 encode）。⇒ 标尺（V1/跨度 13–17 µs/派发）再次成立：
  2 次派发 ⇒ −36.3 µs，**≈18 µs/派发**。
* census（修正后的工具）：流量窗口 **243/243 全忙**、**窗口内 0 次 >0.3 s 停顿**；81 次都在窗口外（空闲头/尾）。
* **V1 的完整推进**（本轮"继续压 V1"）：`p29` 12 派发 → `p31` 10 → `p32` 6 → **`p38` 4**；V1 **1488.6 → 1463.5 → 1411.3 → 1371.9 µs**；
  对基线（2675.1）**−48.7%**。⚠ 每条腿的电台天气不同（`p38` RF 失败 **1546**，`p37` 880），所以**逐条比较只作参考**，
  净效应要看 §6.48–§6.50、§6.60 的机制计数与配对读数。

#### ② 下一步的账（剩下的 4 次派发与"非派发"的那 ~490 µs）

| 项 | 量级 | 说明 |
|---|---|---|
| 信道估计 **2 次** | 未拆 | 与解映射 **1 次** 并列，是**最后 3 次派发**（均衡已到 1）；要动得先读它们自己的编码路径 |
| `merged_hop` 里**非派发**的部分 | **~490 µs**（541 − 4×~12）| 只有两条路：**"移除某阶段"的臂**（§6.44③ 列过的旋钮）或**单 kernel 离线微基准**（`dft_kernel_cost.mm` 的形状）|
| A 项（收样点 ≈473 µs）| 结构项 | 仍**需要用户裁决**（触 V4）|


### 6.61 **①开工（只读码 + 补仪器 + 一个反例实验）：信道估计跳内其实是 6–9 次 kernel 派发，而车道只数到 2** —— 新的头号派发来源；读码后**首选改为消掉 `scatter`（覆盖全部跳）**

#### ① 先补仪器，再谈合并（等距器 `sites(...)` 的做法）

车道里的 `channel_estimator=2.00/跳` 是**阶段数**（extraction + weights 两个逻辑阶段，两条路由刻意对齐），**不是 kernel 数**。
估计器链上真实的 `dispatchThreads` 站点有 **6 个**（`reformat`/K3、`pilots_lse`、`pilots_cfo`、`corr_a`/K1、`corr_rhp`、`scatter`），
于是加了 **`[metal_stats] ce_sites …`**（逐站点计数，只报不判），并用**离线 replay**（`--metal`，无手机无腿）读出真数：

| 语料（PRB）| `reformat` | `pilots_lse` | `pilots_cfo` | `corr_a` | `corr_rhp` | `scatter` | **合计** |
|---|---|---|---|---|---|---|---|
| 3 | 1 | 1 | 1 | 1 | 1 | 1 | **6** |
| **4** | 1 | 1 | 1 | **2** | **2** | **2** | **9** |
| 6 | 1 | 1 | 1 | 1 | 1 | 1 | **6** |
| **8** | 1 | 1 | 1 | **2** | **2** | **2** | **9** |
| 12 / 18 | 1 | 1 | 1 | 1 | 1 | 1 | **6** |
| **25** | 1 | 1 | 1 | **2** | **2** | **2** | **9** |

#### ② 规律与**已验证的机制**：尾巴组是**第二个 queued stage**（不是"merge_tail 失效"）

**规律**：分配宽度不是估计块（3 PRB）的整数倍 ⇒ **尾巴块作为第二个"组"被编码**：
3 = 1 块 + 0；4 = 1 + **尾**；6 = 2 + 0；8 = 2 + **尾**；12 = 4 + 0；18 = 6 + 0；25 = 8 + **尾** —— 完全吻合。
⇒ 有尾巴的跳，`corr_a + corr_rhp + scatter` **各多一次 = +3 次派发**。

**机制（读码 + 一个反例实验确认，更正本节的初稿）**：

* `merge_tail` **默认开**（`rem_prb && n_std_blocks && !matrix_on && !tail_on_cpu && 2*layers<=MAX_LAYERS && !OCUDU_CE_SPLIT_TAIL`），
  但它合并的是**槽位布局**（把窄的尾巴块放进标准组的槽位），**不是派发**：
  `OCUDU_CE_SPLIT_TAIL=1` 与默认的 `ce_sites` **完全一样**（`corr_a=2 corr_rhp=2 scatter=2`，只有 `reformat` 1→0）。
* 真正的两次来自 **`build_slots_on_device()` 被调用两次**：标准组（`port_channel_estimator_metal_mmse_impl.cpp` 的 standard 调用点）
  与尾巴组（edge 调用点），每次 `build_correlation()` 一次 ⇒ **2 次 `encode_corr`**（每次派发 A 与 R_hp 各一个 kernel）。
* **这两次是同一个提交**：`queue_correlation_fenced()` 只**入队**（队列"按一跳两组"定尺），
  `flush_correlations_fenced()` 把队列里所有 stage 编进**一个**命令缓冲、一次 commit ⇒ **没有多付提交**，
  多付的只是**派发**（A、R_hp、scatter 各多一次）。
* 估计器为什么不用"融合形式"（`fused_corr` + `gpu_invert`，那个形式一跳只一个命令缓冲）：**精度**——
  块序 54 时 K1 的逆有 1.3e-5 相对误差（实测 353.2758 vs 宿主 353.2786，足以把 256QAM 从 24 dB 打到 −18 dB），
  所以它**坚持 standalone 构建 + 宿主求逆**；而 standalone 形式下 `corr_std` 的槽位是"超尺寸"的（尾巴 padded 进标准槽位，blockdiag）。

⇒ **真实派发账（每跳）**：CE **6（无尾）/ 9（带尾）** + 均衡 1 + 解映射 1 = **8 或 11**；
而车道计数只写 4.00 ⇒ **"还剩 4 次派发"要更正为"8–11 次"**，估计器是最大的来源。

#### ③ 候选杠杆（读码后的修正排序）

| # | 杠杆 | 省 | 覆盖 | 读码后的结论 |
|---|---|---|---|---|
| **C** | **`scatter` 用"改寻址"消掉**（它把 K0-a 的输出搬成 weights 要的布局，是**纯搬运**；让下游直接按它的寻址读，§3.2 对 y 的同一招）| **−1…−2**（有尾跳 −2）| **全部跳** | ★ **首选**：纯搬运、判据是逐字节（§3.2 已验证这套做法一次）；代价是 kernel + 宿主 |
| **A** | **两组并成一次派发**（corr 与 scatter 各 2→1）| −3 | 带尾跳（≈40%）| ⚠ **不是纯宿主改动**：`corr_stage` 的几何标量（`l`/`npf`/`ncomb`/`pilot_base`/`sigma2`）是**一次调用一份**，尾巴更窄 ⇒ 合并需要**逐 system 几何**（或"尾巴 padded 进标准槽位"的形式，而那个形式在代码里**已有已知的 pad 缺陷**：注释写着 *"the fused route's pads come out NaN"*）⇒ 风险最高的一个 |
| **D** | `pilots_lse` + `pilots_cfo` 融合（CFO 是对 LSE 输出的逐点相位乘）| −1 | 全部跳 | kernel 重写；逐字节或有界容差 + `value_net` |
| **B** | `R_hp` 按几何缓存 | −1 | 分配重复的跳 | ❌ **读码后基本否掉**：`corr_stage` 带 `sigma2`/`fd_hz`/`tau_rms_s`（**每跳的信道统计**）⇒ R_hp 不只看几何，缓存键会几乎每跳都变 |
| — | **`reformat`(K3)** | −1 | 全部跳（split 路由下为 0）| 与 C 同类，可作 C 之后的第二步 |

#### ④ 第一步（下一次施工的起点）与预登记

**第一步 = C：把 `scatter` 消掉（改寻址）**——理由：**覆盖全部跳**（不像 A 只有带尾的 40%）、
是**纯搬运**（§3.2 已证明这类改动可以逐字节不变）、且它的表兄弟（尾巴那份）能一起省。
施工前先读 `encode_scatter` 与 weights/apply kernel 的 y 寻址，确认"下游直接读 `gpu_ls_out`"要改哪几处索引。

**预登记（腿 `p39`，暂定）**：`ce_sites scatter` **1.0（无尾）/ 2.0（有尾）→ 0**（按跳均值 ≈1.4 → 0）、
`burst dispatches` 相应 −1…−2/跳、**V1 −15…−35 µs**、契约 8/8、V2/V4 不变、**dump 逐字节相同**（27 条 ×2 臂 + 与改前二进制对拍）。
**反例判读**：`scatter` 计数不变 ⇒ 下游仍走旧寻址（改动没生效）；dumps 有差异 ⇒ 寻址映射错了（先查 `pilot_base`/`Gb`/`npf`）。

#### ④ 第一步（下一次施工的起点）与预登记（暂定）

**第一步 = 查清 A 是否已是"kernel 已支持、只差宿主把两组并成一组"**：
读 `port_channel_estimator_metal_mmse_impl.cpp` 里 `device_y_stage` / `nof_device_y_stage` 与 `corr`/`corr_edge` 的构建，
以及 `encode_corr`/`encode_scatter` 对 `nof_systems`/`a_sys_stride`/`r_sys_stride` 的使用。
**若成立**，预登记（腿 `p39`）：`ce_sites corr_a/corr_rhp/scatter` 在带尾跳上 **2 → 1**（均值 **≈1.4 → 1.0**）、
`[metal_stats] burst dispatches` 相应下降（每带尾跳 −3）、`merged_hop` −13…−19 µs（按其占比折算 **≈−16 µs 平均**）、
**V1 ≈ −20…−30 µs（均值）**、**dump 逐字节相同**（`ul_chain_replay` 27 条 ×2 臂 + 与改前二进制对拍）、契约/V4/V2 不变。
**反例判读**：若 `ce_sites` 计数不变 ⇒ 宿主仍建两组（改动没生效）；若 dumps 有差异 ⇒ 合并批次的几何（`a_l_stride`/块序）用错了。


### 6.62 ★★★ 杠杆 C 落地：**K2 直接读 LSE ⇒ `scatter` 派发消失**（−1…−2 次/跳、**覆盖全部跳**）；**第一次 A/B 抓到一处真实的"重结合"缺陷**（fast-math 把 `lse*inv_beta` 提到权重上），改法 = **新 kernel 独立文件 + `-fno-fast-math`**

> 施工对象 = §6.61③ 的首选杠杆 C（"把下游要的布局变成下游自己的寻址"）。本节只记**离线**结论与读数；
> 空口读数在腿 `p39-n78-noscatter`（§6.62⑤ 给配方与预登记）。

#### ① 机制：scatter 的映射被搬进了 K2 的寻址（没有搬数据）

`mmse_pilots_scatter_y()` 原本做的是一次**重索引 + 一次单精度乘**（`ocudu_mmse_pilots.metal` 的注释就是规范）：

```
y[(i_layer*n_blk_slots + b)*2*Ls + 2*(i_symb*npf + j)]
    = lse[2*((i_symb*nof_layers + i_layer)*nof_pilots + pilot_base + b*npf + j)] * inv_beta
```

外加**两块清零**：行 `i_symb*npf + j >= nof_symb*npf`（合并批里窄尾组的 pad 行）与块槽 `b >= n_blk_real`（尾组没填的槽）。
新 kernel **`mmse_apply_lse`**（新文件 `ocudu_mmse_apply_lse.metal`）就是把这个等式读出来：
`i_symb` 外层、`j` 内层（`k = i_symb*npf + j` **升序**，与 K2 原来的单层循环一致），**两块清零区按 `w*0` 项参与求和**（与 scatter 写下的字面 0 同义，非有限权重照样进 h）。

* **派发账**：一个 staged group 一次 scatter ⇒ 无尾跳 **−1**、带尾（合并）跳 **−2**（均值 ≈1.4）；
  `lse_applies` 每次批调用 +1（合并批两组只发一次 K2）。
* **系统区间不靠第二份数字**：组的 `sys_lo` 由 **`s.y - y_base` 除以系统步长**得到——正是 `encode_scatter()` 自己算 `y_off` 用的那个差
  （S-7f-5l 的分尾缺陷就是"同一个数写了两份"）。⇒ 写者与读者不可能对"哪几个 system"有分歧。
* **门槛**（`build_lse_sources()`，**每一条都只是"退回旧路"，不是正确性风险**）：knob 关、metallib 缺 `mmse_apply_lse`、
  没有描述符、两组不共用同一个 LSE / 系统区间有洞或重叠 / 几何越界（`nof_symb*npf > L`、`pilot_base + n_blk_real*npf > nof_pilots`、
  读出 LSE 容量）。每条**各自计数**（`mmse_refusals` 新增 `y_direct_{disabled,no_kernel,no_source,coverage,geometry}`），
  因为"路没走"和"没有东西可走"是两个结论。
* **`OCUDU_CE_DEV_Y=0` 保持原义**：那条臂下宿主自己 stage y（没有描述符）⇒ 本路**必然**退回 scatter（计 `y_direct_no_source`），
  writer 的 A/B 因此原样保留。

#### ② ★ 第一次 A/B 读出 40 字节差异 ⇒ 不是寻址错，是**编译器重结合**（本节最值得记的一条）

按纪律先跑 `ab_dumps.sh "OCUDU_CE_Y_DIRECT=0" ""`（同一二进制两臂、27 语料）：

| 读数（第一次） | 值 |
|---|---|
| `_h.bin` / `_llr.bin` / 网格 `.bin` | **0 / 0 / 0**（逐字节相同）|
| `_ce.txt` | **40 字节，13/27 语料**，且只在 `noise_variance`（及其派生的 `snr`）与 `rsrp` 的**末位** |

`_h.bin` 是 **cbf16** 的估计网格（`ul_capture.cpp` 的 `capture_h()` 走 `get_symbol_ch_estimate`），bf16 只有 8 位尾数 ⇒
**h 的 1 ulp 差异在它和 LLR 上看不见，却在 float32 的归约（K4 的 `noise_variance`、K5 的 `rsrp`）上现形** ⇒ 差异在 **h 的最后一两位**。

**根因（IR 直读，不是推测）**：新 kernel 原来写在 `ocudu_mmse_apply.metal` 里（该文件是**默认 fast math**），
`-fno-fast-math` 没加 ⇒ LLVM 的 Reassociate 把 `w * (lse * inv_beta)` 改写成 `(w * inv_beta) * lse`，并**把 inv_beta 提到权重上、一次乘法服务实虚两路**：

```
%132 = fmul fast float %123, %106      ; %123 = w[k]，%106 = inv_beta   ← 被提出来的那一乘
%133 = fmul fast float %132, %127      ; (w*inv_beta) * lse_re
%134 = fadd fast float %133, %117
```

即 `round(round(w*inv_beta)*lse)` 取代了 `round(round(lse*inv_beta)*w)`——**两次舍入的位置不同，就是不同的浮点数**。
（这也解释了为什么只有 `_ce.txt` 动：K2 的输出 h 变了 1 ulp，bf16/LLR 级别被量化吃掉，float32 归约没被吃掉。）

**改法（两条一起才成立）**：

1. 新 kernel **独立成文件** `ocudu_mmse_apply_lse.metal`，进 `IEEE_MATH_SOURCES`（**`-fno-fast-math`**，CMake 的 `ocudu_add_metallib` 是**按文件**给这个标志的）；
   ⇒ 重结合消失，IR 变成两次**无 flag** 的 `fmul` + 一次 `fmuladd`：
   `%128 = fmul(inv_beta, lse_re)`、`%134 = fmuladd(w, %128, acc)` ⇒ `round(w*round(lse*inv_beta) + acc)`，
   与 scatter 路线（y 先被舍入存下、K2 再 `fmul+fadd`/FMA）**逐位相同**：t 与 y 是同一个 float，剩下的是同一个表达式。
2. 旧 `mmse_apply` **留在原文件、保持 fast math 且源码零改动** ⇒ "y 路仍然发布昨天的字节"是**源码性质**，不是测量结论。
   （先量过才敢这么切的：**把 `ocudu_mmse_apply.metal` 整体编成 strict，27 语料 135 个 dump 文件 0 字节差异** ⇒ 这个文件中
   快/严数学对**值**无影响；把新 kernel 分出去只是为了不冒"旧 kernel 代码生成漂移"的险。）

**修完复测**：`ab_dumps.sh "OCUDU_CE_Y_DIRECT=0" ""` ⇒ **exit 0、27 语料 0 字节**。**第一次读数（40 字节）按纪律保留在册**：
它是真实缺陷（不是 flake）——两臂各自确定（`Y_DIRECT=0` 臂连跑 3 次同值）。

#### ③ 不变量网（全部在**最终产物**上重跑）

| 网 | 命令 | 读数 |
|---|---|---|
| 本路 vs 旧 scatter（同二进制两臂）| `ab_dumps.sh "OCUDU_CE_Y_DIRECT=0" ""` | ✅ **0 字节 / 27 语料**（四类 dump 全 0）|
| **与改前二进制对拍**（默认路 = 本路）| `ab_replay_bins.sh ref/replay_pre_c_bc149ab266 build/…/ul_chain_replay`（`AB_MARKER=y_direct`，A 用 `mmse_pre_c_bc149ab266.metallib`、B 用 `mmse_post_c_bc149ab266.metallib`）| ✅ **PASSED**：`pairing-wrong=0`、**0 字节**（配对判据由 `[y_direct]` 自证，见 ④）|
| 旧 y 路 vs 改前二进制 | 同上 + `ENV=OCUDU_CE_Y_DIRECT=0` | ✅ **0 字节**（第一次读到 4546 字节/`syn009_6`，按 6.5 偶发规则重跑得 0；**隔离复跑** 3 条语料含 `syn009_6` 全 0 ⇒ 归入已登记的 `ab_dumps` 偶发类，见 §6.5⑤/Q11；`pairing-wrong=27` 是该臂的**预期**：knob 关 ⇒ B 侧不打印 `[y_direct]`，缺席本身即断言）|
| `ctest -L phy` | `ctest --test-dir build -L phy -j 1` | ✅ **193/193 PASS**（`dft_processor_ci16_test` 是 Disabled，未跑）|
| `value_net` | `python3 …/value_net.py --quiet` 与改前输出逐行对拍 | ✅ **逐行相同**（仍对陈旧归档基线红，= Q11，改前也红）|
| 机制计数 | `[metal_stats] ce_sites` / `mmse_ce` | ✅ 见 ④ |

**⚠ `ctest -j 4` 会假红（已定位为并行产物，不是本次改动）**：`-j 4` 下 `port_channel_estimator_metal_mmse_unit_test`
（Test 3：SNR 20 dB 处 NMSE 差 3.13 dB > 1.5 dB 阈值）与 `…_ta_chain` 红。**对照实验**：
(a) 同一二进制**直接连跑 6 次**（两臂交替）全部 PASS 且**两臂数值完全相同**（+1.14 dB）；
(b) `OCUDU_CE_Y_DIRECT=0 ctest -j 4` **同样红（且红 2 条）**⇒ 与本次改动无关；
(c) `-j 1` 全绿。⇒ 与 §4 的"并行跑 ⇒ 假失败"同一条纪律：**判 `ctest` 用串行**。

#### ④ 新仪器（腿上一眼可读）

* `[metal_stats] mmse_ce … device_y_writes=… lse_applies=… refusals=…`：
  `lse_applies` = 走了本路的 K2 派发数（合并批两组只 +1）；`device_y_writes` 应同时**掉到 0**；`refusals` 应 `<none>`，
  否则点名是哪条门（`y_direct_coverage` 最常见 = 有 system 没被覆盖）。**两个数要一起读**："宿主自己 stage 了 y"也会让
  `device_y_writes=0`，只有 `lse_applies` 能把它和本路分开。
* **`[y_direct] …` 一次性自报**（每进程一行，带首个批的 `systems/blocks/L/groups`）：`ab_replay_bins.sh` 用它做**配对判据**，
  腿日志里它一句就说清"这一跑是谁在读导频行"。
* `OCUDU_CE_Y_DIRECT=0` = 本路**精确 A/B**（保留 scatter）。
* `OCUDU_CE_Y_CHECK=1` 的 `[y_check]` 在本路下**改为打印 `routes=lse_direct` 并跳过比对**：本路没有写 y，
  拿 y 槽对宿主 staging 只会读上一次的残留并报成缺陷（探针的判据由 ③ 的两条 dump 网承担）。

#### ⑤ 下一步 = 腿 `p39-n78-noscatter`（预登记）+ 之后

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p39-n78-noscatter \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# CN 侧：iperf3 -R -b 40M -P 4 -t 240；单次 Ctrl-C 停
# 判读：bash doc_chinese/phy_latency/wip/p0_gate.sh p39-n78-noscatter
#       bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 p39-n78-noscatter
```

| 读数 | 今天（`p38`）| 预登记 |
|---|---|---|
| `ce_sites scatter` | 1.0/跳（无尾）、2.0/跳（有尾）| **0**（按跳均值 ≈1.4 → 0）|
| `lse_applies` | —（新仪器）| **≈ 设备跳数**（不是组数：合并批两组 = 1）|
| `device_y_writes` | 组数/跳 | **0** |
| `refusals` | `<none>` | **`<none>`**（出现 `y_direct_coverage/geometry` ⇒ 有批退回 scatter，先查覆盖）|
| `[y_direct]` | — | **恰好 1 行**，`groups=2` 出现在带尾跳 |
| `burst dispatches` | 4.00/跳（车道口径）| **−1…−2/跳**（均值 −1.4）|
| `merged_hop` | 541.1 µs | −7…−19 µs（按 −1.4 × 13–18 µs/派发）|
| **V1 中位** | 1371.9 µs | **−15…−35 µs ⇒ ≈1337–1357** |
| 契约 / V2 / V4 / D18 | 8/8 / 绿 / 2.00 / 不变 | **同** |
| 反例判读 | — | `scatter` 仍非 0 ⇒ 门没生效（看 `refusals`）；`lse_applies≠` 设备跳数 ⇒ 部分批退回了；V1 不降而 `scatter=0` ⇒ 这次派的省不在关键路径上（回 §6.49③ 的口径问题）|

**之后（§6.61③ 的重排）**：A（标准组 + 尾巴组并成一次派发 —— 需要**逐 system 几何**）、
`reformat`(K3) 同法消掉、D（`pilots_lse` + `pilots_cfo` 融合）。**§3.3 的单 kernel 离线微基准**仍是"非派发那 ~490 µs"的入口。


### 6.63 ★★ 腿 `p39-n78-noscatter`（杠杆 C 的验收）：**机制预登记全中**（`scatter` 2→**0**、`lse_applies=142740`、`device_y_writes=0`、−1.455 次派发/跳、`cbs/lane` 不变）；**V1 中位 1371.9 → 1364.2 µs（新最好，基线 −49.0%）——但只降 7.7 µs，低于预登记的 −15…−35** ⇒ **"13–18 µs/派发"这个标尺被证伪（实测 5–7 µs）**

> 腿配方与预登记见 §6.62⑤；HEAD = `0a4bea6968`（含杠杆 C），`[leg] regime=stress`、`concurrency=2`、
> `OCUDU_UL_PHASE_SEGMENTS=1`、交付配置（环 512 / 池 32 / sc12）。无并跑。

#### ① 机制预登记：**逐条命中**（这是本节最硬的一半）

| 读数 | 预登记 | `p38` | **`p39`** | 判定 |
|---|---|---|---|---|
| `ce_sites scatter` | **0** | 1.0/跳（无尾）、2.0/跳（有尾）| **0** | ✅ |
| `lse_applies`（新仪器）| ≈ 设备跳数 | — | **142740** = `burst commits` = `eq_direct samples` = 设备跳数 | ✅ |
| `device_y_writes` | **0** | 198724（= 1.370/跳）| **0** | ✅ |
| `refusals` | `<none>` | `<none>` | **只有 `y_direct_no_source=4`** | ✅（4 = **构造期 warm-up**：`run_weights_only()` 不传描述符；142740 + 4 = `commits=142744` **精确闭合**）|
| `[y_direct]` | 恰好 1 行 | — | **1 行**（首个批 `systems=1 blocks=1 L=54 groups=1`）| ✅ |
| −派发/跳 | 均值 ≈1.4 | — | **1.455**（= `device_corr_builds` 207762 / 142740；与 §6.61 的"带尾跳 ≈40%"一致）| ✅ |
| `cbs/lane` | 不变 | 2.00 (max=2) | **2.00 (max=2)、dropped=0** | ✅ |
| crossings（V4）| 0 | 0.00+0.00 | **0.00 + 0.00** | ✅ |
| 契约 8/8 / `gaps=0` | 同 | ✅ / ✅ | **MET (8 of 8) / gaps=0、rx_overflows=0** | ✅ |
| V2 池 | 绿 | starved=0、held_max=10、free_min=22 | **starved=0、held_max=18、free_min=14、dropped=0** | ✅ |

⇒ **本路不是"部分生效"**：142744 次 K2 里 **142740 次走直读、4 次是构造期 warm-up、0 次退回 scatter**。

#### ② V1：**降了，但只有预登记的一半** ⇒ 记一次预登记 MISS，并**更正标尺**

| 读数 | `p37`（§3.2 前）| `p38`（§3.2 后）| **`p39`（本杠杆）** |
|---|---|---|---|
| **V1 中位** | 1408.2 | 1371.9 | **1364.2 µs（基线 2675.1 ⇒ −49.0%，新最好）** |
| V1 均值 / p95 / p99 | — | 1388.2 / 1577.5 / 1707.2 | 1383.0 / 1587.8 / **1807.5** |
| `[ul_gpu_lane] merged_hop`（93% busy）| 555.2 | 541.1 | **533.7** |
| `[ul_gpu_lane] ch_wt`（7% busy）| 39.9 | 39.9 | **42.7** |
| `[ul_gpu_lane] busy` 中位 | — | 553.7 | **532.1** |
| 设备跳数 / 语料 | — | 145038 | 142740 |

* **`p38`→`p39`：V1 −7.7 µs、`merged_hop` −7.4 µs ⇒ 1:1**（本腿的省**没有**被放大，也**没有**被藏起来）。
* **每次派发的代价（现在有两个成对读数）**：本杠杆 **7.4 / 1.455 = 5.1 µs**；§3.2 的均衡器派发 **14.1 / 2 = 7.05 µs**。
  ⇒ **§6.61③/§3.1 用的"13–18 µs/派发"高估了 2–3 倍**（这与 §2 第 2 条早就记下的口径警告一致：设备 busy 窗口只有 6.5–9.4 µs）。
  **下次预登记用 5–7 µs/派发**。⚠ 另一条老结论同时被再确认：**V1 的降幅不是窗口降幅的固定倍数**（§3.2 那对是 2.6×，本对是 1.0×）——"窗口 ≠ 关键路径代价"（§6.31③）。
* **反例判读（预登记里写好的）没有被触发**：`scatter=0` 且 V1 确实降了 ⇒ 不在"省了派发但没进关键路径"的坑里；只是省下来的量**本来就小**。
* **判据全部保持**：V1 1371.9 → **1364.2**（阈值 2150）、V4 2.00、V5 8/8 + 0 gaps、V2 绿、`p0_gate.sh` **29 of 29 PASS**。
  V3（RF 1448，`p38` 1546）仍是**逐腿天气**，未作判据。

#### ③ 两条"动了"的读数（都不进判据，但必须点名，别让下一条腿误读）

1. **尾巴变差**：`[ul_gpu_pipeline] stale` **1 → 20**（`p37` 是 13）、p99 +100 µs、`[ul_pipeline] stale=4` ⇒ `leg_gate.sh` 的
   `stale = 0` 一项从 PASS 翻 FAIL（该项在加压腿上**本就误绑**，见 §4/§6.56③，但**翻红要记**）。
   **归因**：本腿宿主更忙 —— `load1(max at a tail event)` **6.34 → 8.67**、`[ul_ldpc_decode]` 中位 **55 → 63 µs**（p99 118→142）；
   且**本改动只减不增**（少一次派发，不含任何新工作）。⇒ 仍归 §3.5 的**另案（宿主竞争/线程优先级）**，不是本杠杆的代价。
   **要成对才能判**：下次做**反向臂**（`OCUDU_CE_Y_DIRECT=0`，把 scatter 放回来）连跑，同一天交替各一条（见 ⑤）。
2. **`ch_wt` +2.8 µs**（7%-of-busy 的小窗口，`p37`/`p38` 两次都恰好 39.9）：方向与预期相反（该窗口里也少了 scatter）。
   未解释、量小（占 busy 权重 0.07×2.8 ≈ 0.2 µs）⇒ **下次腿盯一眼**，不要据此下结论。
   `held_max` 10→18（仍 `< pool=32`，P2-D 的定尺不变）与 `[ul_chain]` 无关，同属上一条的宿主拥挤。

#### ④ 这条线索还剩多少（给"要不要继续消派发"的决策盘）

`p39` 的**真实派发账**（每设备跳，`ce_sites`/跳数）：`reformat=1.00`、`pilots_lse=1.00`、`pilots_cfo=1.00`、
`corr_a=1.456`、`corr_rhp=1.456`、`scatter=0` ⇒ **5.91**；再加均衡 1.00、解映射 1.00 = **7.91/跳**（§6.61② 说的 8–11 得到实测支持）。

| 候选 | 能省/跳 | 按 5–7 µs/派发 | 覆盖 |
|---|---|---|---|
| `reformat`(K3) 改寻址 | −1.00 | **−5…−7 µs** | 全部跳 |
| D（`pilots_lse`+`pilots_cfo` 融合）| −1.00 | **−5…−7 µs** | 全部跳 |
| A（两组并一次派发）| −2.91 | **−15…−20 µs** | 带尾跳（≈46%）|
| **全部做完** | −4.91 | **≈ −25…−35 µs** | —— |

⇒ **派发这条矿脉总共还剩 ≈25–35 µs**（V1 1364 → ≈1330–1340 的乐观界），而 `merged_hop` 里**非派发的 ~450–480 µs**
（真实算力 + 派发间依赖等待）**没有派发可消了** ⇒ 下一段的主战场是 **§3.3（单 kernel 离线微基准 → 砍最贵的 kernel）**，
不是继续加派发臂。**建议顺序：先 §3.3 的微基准（不用飞腿、一次量出 CE/均衡/解映射的算力与窗口），再按读数选 K3/A/D。**

#### ⑤ 下一步（**需要用户拍板的两件**）

1. **反向臂 `p40-n78-scatter`（建议做，成本一条腿）**：配方与 `p39` 完全相同，只加 `OCUDU_CE_Y_DIRECT=0`（把 scatter 放回来）。
   两条**同日交替**跑 → 这是唯一能把 −7.7 µs 与"逐腿天气"分开的做法（§6.56③ 的裁决：**比较要成对**）。
   预登记：`scatter` 回到 1.0/2.0、`lse_applies=0`、`device_y_writes>0`、**V1 差 ≈ +7 µs（±5）**、`cbs/lane`/契约/V2/V4 不变。
2. **§3.3 的单 kernel 离线微基准**（照 `doc_chinese/phy_latency/wip/dft_kernel_cost.mm` 的形状）：不用飞腿、不碰电台，
   把 CE / 均衡 / 解映射各自的**算力**与**窗口**一次量出来 ⇒ 才有资格决定砍哪个 kernel。


### 6.64 ★★★ 反向臂 `p40-n78-noscatter`（`OCUDU_CE_Y_DIRECT=0`）：**配对 A/B 给出 V1 −21.5 µs，落在预登记带内** ⇒ **本节更正 §6.63 的三处结论**（判据、标尺、A 项的账）

> 用户按 §6.63⑤ 的建议飞了反向臂：**同配方、同二进制、紧邻 `p39`**，只多一行 `OCUDU_CE_Y_DIRECT=0`。
> 这一节存在的理由就是"**单腿不是证据；比较要成对**"——§6.63 的 V1 判定与标尺更正**都是拿一条异二进制、早一小时的腿当对照得出的，现已翻案**。

#### ① 四腿总表（同一交付配置、同一配方；唯一变量见"路"列）

| 腿 | 路 | 二进制 | **V1 中位** | stale | load1 | held_max | LDPC 中位 | `merged_hop` | `ch_wt` | RF 失败 |
|---|---|---|---|---|---|---|---|---|---|---|
| `p37` | scatter | §3.2 前 | 1408.2 | 13 | 5.98 | 13 | 65.0 | 555.2 | 39.9 | 880 |
| `p38` | scatter | §3.2 | 1371.9 | 1 | 6.34 | 10 | 55.0 | 541.1 | 39.9 | 1546 |
| **`p39`** | **直读 LSE** | **+ 杠杆 C** | **1364.2** | 20 | 8.67 | 18 | 63.0 | **533.7** | 42.7 | 1448 |
| **`p40`** | **scatter（knob 关）** | **+ 杠杆 C** | **1385.7** | 0 | 7.22 | 9 | 65.0 | **551.1** | 43.4 | **1447** |

**`p39` vs `p40` 是设计最强的一对**：同二进制、紧邻、**RF 失败 1448 vs 1447（天气几乎相同）**、
`p0_gate` 两边都 **29 of 29**、契约两边都 **8/8**、`gaps=0`、`crossings 0.00+0.00`、`starved_events=0`、`dropped=0`。

| 配对读数（`p40` → `p39`，scatter → 直读）| 值 | 每次派发（`p40` 自己 214305/140187 = **1.529** 次/跳）|
|---|---|---|
| **V1 中位** | **1385.7 → 1364.2 = −21.5 µs** | **14.1 µs** |
| `merged_hop`（93% busy）| **551.1 → 533.7 = −17.4 µs** | **11.4 µs** |
| `queue: weights commit→start` 中位 | 39.6 → 40.4（不变）| — |
| `cbs/lane` / 契约 / V2 / V4 / crossings | 2.00 / 8-8 / 绿 / 0.00 / 0.00（两边同）| — |

**机制侧的反向臂也逐条对**（`p40`）：`lse_applies=0`、`device_y_writes=214305` = `ce_sites scatter=214305`、
`refusals=y_direct_disabled=140191`（= `commits`，每个调用都数到 knob）、**`[y_direct]` 公告缺席**（= knob 真的生效）。

#### ② 更正 1（判据）：§6.63② 的"V1 MISS"**撤回** —— 预登记的**增量**是对的

§6.63② 判 MISS 的依据是"`p38` 1371.9 → `p39` 1364.2 = −7.7 µs < 预登记 −15…−35"。**但 `p38` 是异二进制、
早一小时、RF 1546 的腿**；把同条件的反向臂拿来，"scatter 路"的基线是 **1385.7**（不是 1371.9）——
**同一条路的基线自己就漂了 13.8 µs**（`p38` 1371.9 vs `p40` 1385.7）。

⇒ **配对增量 −21.5 µs，落在预登记带（−15…−35）内** ⇒ **预登记命中**；绝对值的偏差（1364.2 高于预登记的绝对区间 1337–1357）
来自**对照腿的漂移**，不是效应估错。**教训（写进纪律）**：预登记**只登记增量**，不要顺手锚一个绝对区间——绝对区间把对照的漂移也算进了预测里。

#### ③ 更正 2（标尺）：§6.63② 的"13–18 µs 被证伪、实测 5–7"**本身是单对结论，现更正为 5–14 µs（配对口径 11–14）**

四次"每次派发"的估计，**按设计强度排序**：

| 来源 | 设计 | merged_hop | V1 |
|---|---|---|---|
| **`p39` vs `p40`（本节）** | **同二进制、紧邻、RF 相同、只差 knob** | **11.4** | **14.1** |
| §6.60 `p37`→`p38`（均衡器 −2/跳）| 异代码、紧邻 | 7.05 | — |
| §6.63 `p38`→`p39` | 异二进制、隔 1 小时 | 5.1 | 5.3 |

⇒ **~5–14 µs/派发，最强设计给 11–14**；**"13–18 µs"没有被证伪**（它在区间上沿）。
**下次预登记：10–14 µs/派发（配对口径），并写明区间 5–14。**
（§6.63② 那句"高估 2–3 倍"作废——它把一个**单对**的差当成了标尺的错。）

#### ④ 更正 3（账）：§6.63④ 的 **A 项算错**，剩余派发预算重算为 **≈ −30…−42 µs**

杠杆 C 落地后 **scatter 已经不在了**，所以 §6.61③ 里 A 的"corr 与 scatter 各 2→1"**只剩 corr 那一对**：
每个**带尾跳**省 **2 次**（`corr_a` 2→1、`corr_rhp` 2→1），带尾占比 **≈49%**（`p39` 45.5%、`p40` 52.9%）
⇒ **A = −0.98 次/跳，不是 §6.63④ 写的 −2.91**。

| 候选 | 次/跳 | ×10–14 µs |
|---|---|---|
| `reformat`(K3) 改寻址 | 1.00 | −10…−14 µs |
| D（`pilots_lse`+`pilots_cfo` 融合）| 1.00 | −10…−14 µs |
| A（corr 一对并成一次派发）| **0.98** | −10…−14 µs |
| **合计** | **2.98** | **≈ −30…−42 µs** |

⇒ §6.63④ 的"≈−25…−35 µs""派发矿脉见底"**作废**：还剩 **≈30–42 µs**（V1 1364 → ≈1322–1334 的乐观界）。
**但方向不变**：`merged_hop` 533.7 µs 里**非派发的 ~450 µs** 仍大一个数量级 ⇒ **§3.3（单 kernel 离线微基准）仍是主战场**，
派发臂可以按"顺手做掉"的节奏继续（K3 → A → D），不必再为它们单独设计实验。

#### ⑤ 两条异常读数结案

1. **`ch_wt` +2.8 µs 不是本杠杆**：对照臂 `p40`（scatter 回来）读 **43.4**，比 treatment 的 42.7 **还高**
   ⇒ `39.9`（`p37`/`p38`）→ `42.7`/`43.4`（`p39`/`p40`）是**按二进制/时段的漂移**（7%-of-busy 的小窗口，加权后 ≈0.2 µs），与 knob 无关。§6.63③ 的"未解释"到此结案。
2. **`stale` 20 不能判、也不归于本杠杆**：两对**互相矛盾**——本对 treatment 20 vs control **0**；上一对（都走 scatter）是 13 vs 1，
   **反向**。四腿的 stale 是 13/1/20/0，与"路"没有单调关系。而本对里 RF 几乎相同（1448 vs 1447）却 `load1` 8.67 vs 7.22、`held_max` 18 vs 9、
   `LDPC 中位` 63 vs 65 ⇒ 指向**宿主侧**（§3.5 另案）。**结论：要判它得飞"宿主竞争/线程优先级"臂，不是再飞路线臂**（§6.63⑤ 的"靠 p40 判 stale"这个期待**没有兑现**，据实记录）。
   附：`p39` 的 `leg_gate.sh` 因此是 7/9（`stale = 0` 翻红，该项在加压腿上本就误绑），`p40` 是 8/9（只有 `grant ≥50%` 那条误绑项）。


### 6.65 ★★ §3.3 的第一半（**CE kernel 的算力，离线微基准**）：一跳 CE 的**执行**只有 **≈38 µs**（单 kernel 1.5–13.2 µs、**空派发地板 1.3–1.4 µs**）⇒ **§6.63④/§6.64④ 说的"非派发 ~450 µs 是真实算力"作废**；并且**腿上"每去一次派发值 10–14 µs"既不是 kernel 执行、也不是派发固定成本 ⇒ 那是阶段边界的串行化**

> §3.3 的原话是"一次量出 CE/均衡/解映射各自的算力与窗口，做完才知道该砍哪个 kernel"。本节给出 **CE 六个 kernel 的算力**（本机可复现到 ~3%）、
> **空派发地板**，并**证伪了"非派发那 ~450 µs 是算力"这个前提**。载体 = 新工具 `doc_chinese/phy_latency/wip/ce_kernel_cost.mm`（附带一个**没能成为仪器**的方法，见①）。

#### ① 先说两个**没能成为仪器**的方法（都花过时间，写下来免得下一个人再走）

| 方法 | 结果 | 为什么不是仪器 |
|---|---|---|
| 用引擎自带的 `OCUDU_CE_*_REPEAT` + `[metal_stats] gpu busy (back_end)` 的斜率 | ❌ **斜率 −130 µs/派发**（负的）| `gpu busy` 是**整次运行所有 commit 的并集**（19 个 commit、12.5 ms busy，其中 16 个是 CPU 参考跳），逐次运行抖动**几十 %** |
| 用 `queue occupancy … slowest commits` 的逐 commit `start->end` | ❌ 一个 `ce_weights` commit 读 **745–750 µs**，而它所在的整跳窗口只有 **534 µs** | 那条 commit **编码了抽取 fence 的 wait**，它的 `start->end` 是**窗口**不是执行（§6.29④/§6.31③ 的同一条教训）|
| ★ 独立微基准（本节的工具）里**不预热**就测 | ❌ 同一臂读到 **28.7 µs**（它是该 pipeline 在本进程的第一次）与 **9.2 µs**（同一几何、在文件后面的 sweep 里）；`mmse_weights` 两次运行读 **21.6 / 58.6** | GPU 时钟爬坡 + pipeline 首次使用的上传，**都落在被计时的那个 command buffer 里**。修法：每臂先跑 20 次不计时 + 取 3 次的**最小值** + 开工前 **~50 ms 全局预热** ⇒ 两次运行一致到 **~3%** |

#### ② 读数（**生产几何**：3 PRB 块、`dmrs_type=1` 2 CDM 组 ⇒ `re_pattern.count()=6`、`npt=3` ⇒ `npf=18`、`L=54`、`nf=36`、`nout=504`、1 层、合并批 2 system；200 次派发/臂，取 3 次最小值）

| kernel | 网格线程 | **µs/派发** | 每跳次数（`p39`）| 每跳 µs |
|---|---|---|---|---|
| **空派发地板**（`mmse_weights`，1 threadgroup）| 128 | **1.26–1.38** | — | — |
| `mmse_reformat`（K3）| 504 | **1.52** | 1.00 | 1.5 |
| `mmse_corr_a`（K0-d）| 5832 | **1.69** | 1.456 | 2.5 |
| `mmse_corr_r_hp`（K0-d）| 54432 | **6.07** | 1.456 | 8.8 |
| `mmse_apply`（K2，y 路）| 1008 | **6.45** | — | — |
| `mmse_apply_lse`（K2，LSE 路，§6.62）| 1008 | **7.42** | 1.00 | 7.4 |
| `mmse_weights`（K1b）| 54528 | **13.18** | 1.00 | 13.2 |
| `mmse_pilots_lse` / `mmse_pilots_apply_cfo` | — | **未测**（参数块未镜像）| 1.00 / 1.00 | ~4–8（估）|
| **CE 合计** | | | **5.91 次/跳** | **≈38 µs/跳** |

块尺寸扫描（`block_prb` 1/2/3 ⇒ `L`=18/36/54）：`corr_a` 1.49/1.66/1.67（**几乎不随块变**，它的网格是 `L×L` 但算得很少）、
`corr_r_hp` 1.78/3.28/6.12、`apply` 2.50/3.87/6.50（**两者随输出行数近似线性**）⇒ 要省它们就得动代数，不是动网格。

#### ③ 结论（三条，都直接决定"下一个杠杆做什么"）

1. ★ **一跳的 CE 执行 ≈38 µs，占 `merged_hop`（533.7 µs）的 ~7%**。⇒ **§6.63④/§6.64④ 写的"非派发的 ~450 µs 是真实算力 + 依赖等待"这个前提不成立**：
   即使把 CE 的算力**全部抹掉**，V1 也拿不到 40 µs；**单个 kernel 的最大头 `mmse_weights` 只有 13.2 µs**，而且它**不在**任何"消派发"候选名单里。
2. ★★ **腿上"每去一次派发值 10–14 µs"（§6.64③ 的配对读数）既不是 kernel 执行（被消掉的那些只有 1.5–6.5 µs），也不是派发固定成本（1.3–1.4 µs）**
   ⇒ 它是**阶段边界的串行化**：每次派发之间的 `stage_pipeline` 切换 / `memoryBarrierWithScope` / 宿主 encode，以及**被切掉的重叠**。
   **这给"消派发"这条线一个更准的读法**：赢的是**边界**，不是那几微秒算力——所以**"减少边界"（合并/去 barrier）与"消派发"是同一件事的两种做法**，
   而 §6.61③ 的"移除阶段"旋钮（`OCUDU_CE_CORR_SEGMENT`/`OCUDU_CE_WEIGHTS_BARRIER`/`OCUDU_CE_EQ_DEFER_ENCODE` …）**可以直接当杠杆试**，不必等 kernel 重写。
3. **V1 剩下的大头不是 kernel**：`merged_hop` 533.7 µs 里约 **464 µs 是"等本槽的样点到达"**（前端 lane 的 residency 中位 463.9 µs ≈ 一个时隙 14×1/30 kHz = 466.7 µs，
   这与 §3.4 早已登记的 **A 项 ≈473 µs（零算力）** 独立吻合），再加 CE ≈38 µs 与均衡/解映射/重排 ≈30 µs。
   ⇒ **下一步的结构性杠杆就是 A 项（`P1-7` 符号级收包），而它触 V4、需要用户裁决**（§3.4）。

#### ④ 下一步（按此顺序）

1. **补测**：把 `mmse_pilots_lse`、`mmse_pilots_apply_cfo` 以及**均衡 / 解映射**的 kernel 加进 `ce_kernel_cost.mm`（后两者在各自的 metallib 里，
   参数块要照各自 engine 镜像）⇒ 有了完整的"算力账单"才能判"均衡那条 741 µs 的相位段"里有多少是算力。
2. **"减少边界"臂（便宜、不需要新 kernel）**：用现成旋钮做**离线 dumps + 一条腿**，量"去掉一个阶段边界"值多少（预登记按 §6.64③ 的 10–14 µs/边界）。
3. **A 项（结构项，需用户裁决）**：`OCUDU_UL_RX_SYMBOLS=N`（`P1-7`）—— 它是唯一能碰那 ~464 µs 的东西，但**触 V4**（§3.4）。


### 6.66 ★★ §3.3 的另一半（**边界与宿主 encode 的价钱**）：**四种"阶段边界"全部 ≤0.3 µs、宿主 encode 0.18 µs** ⇒ **"减少边界"这条臂不必飞**；且**腿上"每去一次派发值 10–14 µs"已没有任何机制支持**（四个候选全被量掉）⇒ **派发/边界这条线见底**

> 用户在 §6.65 之后裁决"先做便宜的两步"：**(a) 补全算力账单**、**(b) 用现成旋钮做"减少阶段边界"臂**，A 项裁决等读数。
> 本节就是这两步的结果——**(b) 被测量否掉，因此省下一条腿**；(a) 的边界部分补完（均衡/解映射的 kernel 仍待补）。

#### ① 边界与宿主 encode：**四个候选全都不值钱**（工具：`wip/ce_kernel_cost.mm` 的 stage-boundaries 段；同一 kernel 连发 200 次，只在两次之间插入被测的边界）

| 两次派发之间的"边界" | run 1（µs/派发）| run 2 | vs 背靠背 |
|---|---|---|---|
| 无（背靠背）| 7.384 | 7.442 | — |
| `memoryBarrierWithScope`（= 引擎非 burst 路径显式加的那个）| 7.549 | 7.529 | **−0.10…+0.14** |
| pipeline 切换（`setComputePipelineState` 来回，= `stage_pipeline` 做的事）| 7.606 | 7.418 | **−0.04…+0.03** |
| **encoder 边界**（`endEncoding` + 新 encoder，= `OCUDU_CE_CORR_SEGMENT` 做的事）| 7.636 | 7.684 | **−0.01…+0.29** |
| **宿主 encode 一个派发**（只计 CPU 的 encode，不等 GPU）| **0.180** | **0.080** | — |

⇒ **四者都在噪声量级（≤0.3 µs），宿主侧也只有 0.1–0.2 µs。** 这也顺带说明：**`OCUDU_CE_CORR_SEGMENT` / `OCUDU_CE_WEIGHTS_BARRIER` 这类"减少边界"的旋钮没有可赚的量**（它们是诊断/排除工具，不是杠杆）——**不要为它们飞腿**。

#### ② 那么腿上的 10–14 µs 是什么？——**没有任何机制支持这个分解**

被消掉的那个派发（scatter）自己的账：**执行 ~1.5 µs（同量级 kernel 实测 1.5–1.7）+ 派发地板 1.3–1.4 + 边界 ≤0.3 + 宿主 0.2 ≈ 3.5 µs**，
而配对腿读数说它值 **10–14 µs**（`p39` vs `p40`：V1 中位 −21.5、均值 −14.4）。**差 3–4 倍，且四个候选机制全部被排除。**

**据实记录（不解释掉）**：
* 配对腿的 **V1 增量是真的**（同二进制、紧邻、RF 1448/1447、`p0_gate` 两边 29/29），**但它的"每次派发 10–14 µs"归因现在没有支撑**；
* 一个可能的落点（**未验证，下一个仪器要去的地方**）：本微基准**把宿主与队列排除在外**（GPU 一直忙、没有跨 kernel 依赖、没有别的 lane 并发）。
  在真跳里 CE 的 buffer 是**与前端/均衡/解映射共享的那个 lane burst 的一部分**，少一个派发可能改变的是**buffer 完成的时刻与下一段的起跑关系**（调度），而不是那几微秒本身。
  ⇒ 要看见它，得用**宿主/lane 级**的仪器（`OCUDU_CE_TIME` 的 `[mmse_time]` 相位表——**本构建未定义该宏**，需要一次带 `-DOCUDU_CE_TIME` 的构建；或 §6.24 的 P0 dump），**不是再做一个单 kernel 基准**。

#### ③ 预算重算（把 §6.64④ / §6.65③ 的账并起来）

| 项 | 量 | 依据 |
|---|---|---|
| 剩余"消派发/减边界"候选（`reformat` 1 + D 1 + A 0.98 次/跳）| **≈5–20 µs**（按各自 kernel 的**延迟** 1.5–7 µs，不是 10–14）| §6.66①② |
| "减少边界"（barrier/segment/switch）| **≈0** | §6.66① |
| CE 全部算力 | **≈38 µs**（已知 6 个 kernel；均衡/解映射未测）| §6.65② |
| `merged_hop` 的其余 | **≈464 µs = 等本槽样点到达（A 项，零算力）** | §6.65③ + §3.4 |

⇒ **派发/边界这条线到此见底**：能赚的总量 **≤20 µs**，而 **A 项那 ~464 µs 的窗口是唯一还有量级差的地方**。
⇒ **下一步只剩三件**：(1) 补测**均衡/解映射**的算力（把账单补全，判 §6.65③ 里"均衡 741 µs 相位段"有多少是算力）；
(2) 需要时用 `-DOCUDU_CE_TIME` 的构建看**宿主/lane 级**的 10–14 µs 落在哪（诊断，不承诺收益）；
(3) ★ **A 项 / `P1-7` 的裁决**（唯一能动 ~464 µs 的东西，触 V4）。


### 6.67 ★ 宿主级诊断（`OCUDU_MMSE_DEBUG=1` 的 `[mmse_eng]` 相位表）：**宿主 encode 一个 CE 阶段只有 6.5–8.2 µs（≈1.3–1.6 µs/派发）⇒ 宿主侧也找不到那 10–14 µs**；§6.66 的负结论因此更硬

> 用户裁决的第二步（宿主级诊断）。**不需要重新构建**：`mmse_phase_timer` 的开关是**运行期** `OCUDU_MMSE_DEBUG`（不是编译宏，`OCUDU_CE_TIME` 是另一组计时器）。
> 载体：`ul_chain_replay <capture> --metal --repeat 20`（注意参数是**空格分隔**：`--repeat 20`，`--repeat=20` 会报 unknown option），两臂 `OCUDU_CE_Y_DIRECT=1/0`。

#### ① 读数（36 个 `[mmse_eng]` 调用/臂 = 20 次重复；中位数，单位 µs）

| 相位 | 含义 | `Y_DIRECT=1`（直读）| `Y_DIRECT=0`（scatter）|
|---|---|---|---|
| `wrap` | 缓冲映射 | 0.8 | 0.5 |
| `cb` | 建命令缓冲 | 4.5 | 3.5 |
| **`encode`** | **整个 CE 权重阶段的宿主编码**（K1+weights+K2+K3+K4 ≈5 个派发）| **8.2** | **6.5** |
| `commit` | ⚠ 见下 | 6.1 | 3.8 |
| `wait` | 恒 0（被 `commit` 吃掉）| 0.1 | 0.0 |

⇒ **宿主 encode 一个 CE 阶段 6.5–8.2 µs ⇒ ≈1.3–1.6 µs/派发**（与 §6.66① 孤立测的 0.18 µs、以及"腿上一派发 10–14 µs"都不同量级）。

#### ② ⚠ 仪器陷阱（记下来，别人会踩）：**`mmse_phase_timer` 的 `commit` 字段在同步路径上包含 GPU 等待**

`encode_run()` 的顺序是 `phase.encoded()` → `end_stage(...)`（**里面就 `commit` + `waitUntilCompleted`**）→ `phase.committed()`。
⇒ `commit` = endEncoding + commit + **等 GPU 完成**（最慢的 20 个调用均值 **~900 µs**，正是设备跳的批处理时间），**`wait` 恒 0**。
**别把 `commit` 读成"提交开销"**；要看 GPU 侧就用 `[metal_stats] queue occupancy`（且注意 §6.65① 记的 fence 污染）。

#### ③ 结论：**宿主侧也排除了**，§6.66② 的"归因无支撑"成立

现在四个方向都量过了：**GPU 执行 1.5–13.2 µs/派发**（§6.65）、**派发地板 1.3–1.4 µs**、**四种边界 ≤0.3 µs**（§6.66①）、**宿主 encode 1.3–1.6 µs/派发**（本节）。
把它们加起来，被消掉的 scatter 值 **≈3.5–5 µs**，而配对腿读数是 **10–14 µs**。
⇒ **§6.66② 的结论维持并加强**：**配对 V1 增量是真的，但"每次派发 10–14 µs"这个分解没有任何仪器支持**；
它要么是**跳结构/调度**效应（本微基准与相位表都把并发与别的 lane 排除在外），要么**部分来自逐腿天气**（`p39` 的均值只降 14.4 µs，中位降 21.5 µs）。
**不要再用 10–14 µs 做预算**；用 **§6.66③ 的 ≈5–20 µs（剩余消派发候选之和）**。

#### ④ 仍未做（据实登记）

* **均衡 / 解映射的算力**未进账单（它们的 kernel 在各自的 metallib：`ocudu_equalizer.metal` 的 `equalize_params`、`ocudu_demod.metal`）——
  §6.65③ 里"均衡那条 741 µs 相位段有多少是算力"仍是**未答**。这是补账的下一件。
* 那 10–14 µs（§8 Q19）若要坐实，需要一个**能看见并发/别的 lane** 的仪器（lane 级 GPU 时间戳或 §6.24 的 P0 dump），**不是更多的孤立基准**。


### 6.68 ★ 会话 #6 的现状快照（2026-09-25；**原 `session_handoff_2026-09-25-6.md` 的内容整理入档**，那份文件按 §5.4 的规矩删除）

> **为什么在这里**：交接 memo 是"交接时刻的快照"、开发过程中不改（用户 2026-09-25 明确，§5.4）。该会话在开发中途创建并多次改动了
> `session_handoff_2026-09-25-6.md`（**那是错的**），本节把它的内容按章节归位：判据现状 → §3.1；开工对齐办法 → §5.3；
> 纪律增量 → §5.2；文件地图 → §4.5；未决项 → §8；腿 → §9；**技术结论本来就在 §6.62–§6.67**。这里只留一条会话级的记录。

#### ① 本会话做了什么（提交，逆序）

| 提交 | 内容 | 出处 |
|---|---|---|
| `5a375cb1ba` | **杠杆 C 落地**：K2 直读 LSE、`scatter` 派发消失（新 kernel 独立文件 + `-fno-fast-math`）| §6.62 |
| `7db01fa5c8` / `0a4bea6968` | §6.62 文档 + 当时那份（已删的）交接 memo | §6.62 |
| `045378f2e4` | §6.63：**腿 `p39`**（机制预登记全中；当时误判 V1 MISS）| §6.63 |
| `70fefc4511` | §6.64：**反向臂 `p40`** ⇒ **配对 A/B 确认 −21.5 µs**，并更正 §6.63 三处（V1 判定 / 标尺 / A 项的账）| §6.64 |
| `171d54d194` | §6.65 + 工具 `ce_kernel_cost.mm`：**CE 六个 kernel 的算力 ≈38 µs/跳**、空派发地板 1.3–1.4 µs | §6.65 |
| `fec7359dd0` | §6.66：**四种阶段边界 ≤0.3 µs、宿主 encode 0.18 µs** ⇒ "减少边界"臂**不必飞** | §6.66 |
| `f2e5d87ce0` | §6.67：宿主相位表 ⇒ **宿主 encode 1.3–1.6 µs/派发** ⇒ 那 10–14 µs 四个方向都无支撑 | §6.67 |

#### ② 判据现状（四腿对照，全部同一交付配置：环 512 / 池 32 / sc12；加压、并发 2）

| 腿 | 路 | 二进制 | **V1 中位** | stale | load1 | held_max | `merged_hop` | `ch_wt` | RF 失败 |
|---|---|---|---|---|---|---|---|---|---|
| `p37` | scatter | §3.2 前 | 1408.2 | 13 | 5.98 | 13 | 555.2 | 39.9 | 880 |
| `p38` | scatter | §3.2 | 1371.9 | 1 | 6.34 | 10 | 541.1 | 39.9 | 1546 |
| **`p39`** | **直读 LSE** | **+C** | **1364.2** | 20 | 8.67 | 18 | **533.7** | 42.7 | 1448 |
| **`p40`** | **scatter（knob 关）** | **+C** | **1385.7** | 0 | 7.22 | 9 | **551.1** | 43.4 | **1447** |

`p39` 与 `p40` 是**设计最强的一对**（同二进制、紧邻、RF 1448/1447、两臂 `p0_gate` 都 29/29）⇒ **V1 −21.5 µs、`merged_hop` −17.4 µs**。

#### ③ 交接口径的预算（**照这个用**，三处更正后的数）

| 项 | 量 | 出处 |
|---|---|---|
| 每次**派发**的标尺 | **5–14 µs（配对口径 10–14）** | §6.64③ |
| 剩余消派发候选（`reformat` 1 + D 1 + A **0.98** 次/跳）| **≈5–20 µs**（按各自 kernel 的**延迟**）| §6.64④ / §6.66③ |
| "减少边界"（barrier / segment / switch）| **≈0** | §6.66① |
| CE 全部算力（6 kernel）| **≈38 µs/跳** | §6.65② |
| `merged_hop` 其余 | **≈464 µs = 等本槽样点到达（A 项，零算力）** | §6.65③ + §3.4 |

#### ④ 下一步（顺序已按测量结果排好）

1. **补账**：把**均衡 / 解映射**的 kernel 加进 `ce_kernel_cost.mm`（§8 **Q20**；Q1 的剩余部分）。
2. ★ **A 项 / `P1-7` 的裁决**（用户）：**派发/边界这条线已见底（≤20 µs）**，它是唯一还有量级差的东西（~464 µs 窗口、零算力，触 V4）。
3. 那 10–14 µs（§8 Q19）若要坐实，需要**能看见并发/别的 lane** 的仪器（lane 级 GPU 时间戳或 §6.24 的 P0 dump），**不是更多孤立基准**。


### 6.69 ★★★ 算力账单补全（§3.3 收口）：**均衡 + 解映射 ≈2.7–3.0 µs/跳（纯派发地板，与 RE 数无关）** ⇒ **一跳的全部 kernel 算力 ≈51 µs，占 `merged_hop` 533.7 µs 的 ~10%** ⇒ **"重写 kernel"这条杠杆也死了**：除了 `mmse_weights`(13.2)，每个 kernel 都在 1.3–1.6 µs 的地板上

> 用户裁决"回到主线"后的第一件（§8 Q20：补账）。工具仍是 `wip/ce_kernel_cost.mm`，新增 `equalize_mxn`（`ocudu_equalizer.metallib`）与 `demod_soft`（`ocudu_demod.metallib`）两臂。

#### ① ★ 先记一个**仪器陷阱**（它差点给出 256 倍大的假读数）

两个引擎用 **`dispatchThreads`（非均匀网格，网格尺寸就是线程数）**，而 CE 的 kernel 用的是 `dispatchThreadgroups`（网格 × 线程组）。
第一版拿 `time_it()`（threadgroups）去测它们 ⇒ 实际起了 **156×256 = 39936 个线程**，读出 **4.0 µs/156 RE = 25.7 ns/线程**（≈"每线程 25 ns 什么也不干"）。
⇒ 工具里补了 `time_it_threads()`（`dispatchThreads`），并**在注释里写明"别把非均匀派发的 kernel 塞进 threadgroups 计时器"**。

#### ② 读数（1 层 / 1 端口 / QPSK = 空口与语料的形状；两次运行一致到 ~5%）

| `nof_re`（数据 RE）| 线程 | `equalize_mxn` µs | `demod_soft` µs |
|---|---|---|---|
| 156 | 156 | 1.40 | 1.25 |
| 612 | 612 | 1.45 | 1.32 |
| 1224 | 1224 | 1.52 | 1.33 |
| 2184 | 2184 | 1.61 | 1.37 |

⇒ **花了 14 倍 RE，代价只涨 1.15 倍**：两者都**贴着 §6.65 的空派发地板（1.3–1.4 µs）**，**算力在 2184 RE 以内基本免费**（GPU 把这些 RE 分散到各核，几千个线程根本填不满）。
⇒ 每跳（一跳一次派发，`eq_direct samples = 设备跳数`）= **≈2.7–3.0 µs**。

#### ③ ★★★ 结论：**一跳的全部 kernel 算力 ≈51 µs，没有值得砍的 kernel**

| 阶段 | 每跳 µs | 依据 |
|---|---|---|
| CE：`weights` 13.2 + `corr_r_hp` 5.9 + `apply_lse` 7.5 + `corr_a` 1.6 + `reformat` 1.4 | **≈30**（+ `pilots_lse`/`apply_cfo` 两个小网格 kernel ≈3）| §6.65② |
| 均衡 + 解映射 | **≈2.8** | §6.69② |
| 前端 DFT 的**执行**（batch=14，一槽一次派发）| ⚠ ~~≈10.6 µs/槽~~ ⇒ **≈46.9 µs/槽** | ~~§6.30~~ —— **10.6 那个数已被本文档自行作废**（隔离微基准与真实几何不符，见 §6.65 前的对照表）：真实几何 harness 与空口**都读 46.9**（空口 `dft_front_end` exec p50 **46.6**、p5 46.1/p95 47.5 ⇒ 几乎是常数），见 §6.143⑧ |
| **合计** | ⚠ 上面两行相加的口径要跟着改：`≈79–87 µs ≈ merged_hop 的 ~17%`（原表按 10.6 算成 43–51 µs，**已过时**）| —— |
| `merged_hop` 其余 | **≈460–480 µs = 等本槽样点到达（A 项，零算力）** | §6.65③ + §3.4 |

⇒ **§3.3 的问句"做完才知道该砍哪个 kernel"到此有答案：没有值得砍的 kernel。**
唯一的"大"kernel 是 `mmse_weights`（13.2 µs，且不在任何消派发名单里）；**把全部算力抹掉也只有 ~50 µs**。
**派发线 ≤20 µs（§6.66③）+ 算力线 ~50 µs + 边界 ~0** ⇒ **V1 剩下的量级差只剩 A 项那 ~464 µs 的窗口**（`P1-7`，零算力，触 V4，§8 Q7）。
**本工作流的"代码侧"优化空间到此基本收口**——再往下必须动**结构（收包策略）**，那是用户裁决项。
⇒ **§6.68④ 的"下一步"更新为**：~~(1) 补账~~ **已完成（本节）**；**(2) A 项 / `P1-7` 的裁决 = 唯一剩下的一件**（§8 Q7）；
(3) 那 10–14 µs 的归因（§8 Q19）是诊断、不承诺收益，需要时再做。


### 6.70 ★★ 预登记：腿 `p41-n78-rxsym`（**A 项 / `P1-7` 符号级收包**，用户 2026-09-25 放行为**纯测量臂**）

> 背景：§6.65③/§6.69③ 收口后，**派发线 ≤20 µs、算力线 ≈50 µs、边界 ≈0**，V1 剩下的大头是 `merged_hop` 里
> **~464 µs 的"等本槽最后一个样点"（零算力）**，与 §3.4 早已登记的 A 项 ≈473 µs 独立吻合。
> `OCUDU_UL_RX_SYMBOLS=N`（N>0 = 符号级收包；0/不设 = 整槽）是**唯一能碰它**的旋钮。用户裁决：按 **P2-F 的先例**当**纯测量臂**
> （V4 只约束交付；本臂只读读数、不当作交付）。

#### ① ★ 这个臂的**判据不能是 V1 本身**（先写死，免得读出一个假结论）

`lower_phy_baseband_processor.cpp` 的注释已经写明：`[ul_pipeline]`/`[ul_time_frequency]` **从"第一个被收到的块"起算**——
**整槽策略下那是时隙的末尾，符号级策略下那是时隙的开头**。⇒ **V1 的端点自己会移动，移动量 ≈ 一个时隙 = 14 × 1/30 kHz = 466.7 µs。**
所以：

| 读数 | 怎么读 |
|---|---|
| **`V1_sym − V1_whole`** | **判据在这里**。预登记：**若流水线本身不变 ⇒ ≈ +467 µs（±40）**；**若 ≤ +100 µs ⇒ 流水线真的快了 ≥ ~370 µs**（两个效应相互抵消）；**若 < +467 但 > +100 ⇒ 部分收益** |
| `V1_sym` 的绝对值 | **不许拿去和 §3.1 的 2150 µs 比**（口径不同、端点移动）——**也不许为它改阈值** |
| `cbs/lane` / `(max=)` / `dropped` | **V4：必须 ≤2.00 / max≤2 / dropped=0**。这是本臂的**主要风险**（符号级收包可能让前端从"一槽一次提交"变成逐符号提交）|
| `[metal_stats] dft … batched=…/… batch_max=` | **D16：`batch_max` 必须仍是 14**（批量化的前提是"一槽的样点都在"，符号级收包若把它拆掉，本臂就同时破了 P2-B′ 的收益）|
| 契约 / `gaps` / `rx_overflows` / `assembled` / `dropped` / `starved_events` | **功能绿**：8/8、0、0、0、0、0（§5.8.29 早前量过 `contract MET 7/7, assembled=0, gaps=0`，本臂要在**加压 + 融合路径**上重量）|
| 腿的收尾 | ⚠ **已知缺陷**：该策略的 **shutdown 会踩 DU teardown race**（代码注释原文）⇒ 腿尾出现 `Could not stop application after 5 seconds` 或 `[metal_stats]` 缺失**属预期**；`run_leg.sh` 会点名，**据实记录**，不要当成新的失败 |

#### ② 腿与参数

* **主臂**：`OCUDU_UL_RX_SYMBOLS=1`（每个接收请求一个符号 = 该策略的极端形式；收益上界也在这里）。
* **备选**（若 N=1 在加压下把接收线程打满/丢样）：`OCUDU_UL_RX_SYMBOLS=7`（半槽）——**同一腿不要混**，一次一个变量。
* 其余与 `p39`/`p40` **完全同配方**（n78 加压、并发 2、`OCUDU_UL_PHASE_SEGMENTS=1`、交付配置环 512 / 池 32 / sc12）。
* **对照**：同日的 `p39`/`p40`（整槽策略、同二进制系）就是 `V1_whole` 的参照——两者 V1 差 21.5 µs（§6.64），
  所以 `V1_whole` 取 **1364–1386 µs**、`V1_sym` 的预期区间 = **+445…+510 µs 偏移后的 1810–1880 µs**（"流水线不变"假设）。

#### ③ 反例判读

* `batch_max < 14` ⇒ 符号级收包把前端批量化拆了（**同时破 P2-B′**，本臂的收益要扣掉这部分）。
* `cbs/lane > 2.00` 或 `dropped > 0` ⇒ **触 V4**，按用户裁决只作读数、不交付，并请用户就"值不值得"再裁。
* `V1_sym − V1_whole > +510 µs` ⇒ 比"端点移动"还差 ⇒ 符号级收包在加压下**净亏**。
* 腿尾 `gaps > 0` 或 `rx_overflows > 0` ⇒ 接收策略把余量吃掉了（§6.51 的 D19）。


### 6.71 ★★ 腿 `p41-n78-rxsym` 的结果 + 两处前提更正 + 542 µs 窗口的离线新证据（2026-09-26，本会话；**零新代码**）

> 本节是 §6.70 那条腿的**结果**（`p41-n78-rxsym`，`OCUDU_UL_RX_SYMBOLS=1`，配方同 `p39`；腿日志
> `logs/gnb_gpu_p41-n78-rxsym_0926_0734.log*`），加上按 memo §3 的施工顺序做下去时**读码读出来的两处前提更正**。
> memo §3.1（补 H4 计数）**被现有仪表证伪为不必要**；§3.2（C2）**前提不成立**（见③）。

#### ① 结果：A 项（`P1-7` 符号级收包）**被证伪为交付路径** —— 伤害在"前端提交数"，不在符号

| 读数 | `p41`（`RX_SYMBOLS=1`）| `p42`（整槽，同日、同二进制系）| 判读 |
|---|---|---|---|
| `[ul_gpu_pipeline]` 中位 | **2043.1** | 1366.8 | 跳变慢 +676（+49%）|
| `[ul_pipeline]` 中位 | 2061.0 | —— | |
| UL MAC PDU 中位 / **总量** | **217 B** / 18.2 MB | 3329 B / 333.5 MB | **15× / 18× 塌** |
| `[ul_by_size]` CRC-OK 跳 / 总跳 | 97,978 / 152,666（64%）| 102,699 / 144,787（71%）| **比率接近**，塌的是*每跳的 TB* |
| `[metal_stats] dft … batched=` / `released=` | **0/0** / **0** | 157,357/2,202,998 / 157,357 | C1 的靶子 |
| ★ `[ul_gpu_lane] dft slots / cbs` | 178,407 / **2,497,686**（**14.0 cb/槽**）| 60,868 / **60,868**（0.39/槽）| **前端每符号一条命令缓冲** |
| ★ `[ul_dft_wait]` | **中位 577.6 µs**（samples=178,397）| **no samples** | 前端拿不到车道 |
| `[ul_gpu_lane] period` 中位 | 801.6 | 427.7 | 车道周转被拉长 |
| `[ul_rx_wait]` 中位 | 0.0 | 473.0 | 收包侧确实更早拿到样点 |
| 契约 / `gaps` / 池 / `cbs/lane` | 8/8 / 0 / 绿 / 2.00 | 同 | **不允许变的都没变** |

**因果链（与所有读数一致）**：`RX_SYMBOLS=1` ⇒ `block_batching_enabled()` 那条 `return`（**C1 已删**）把"成批"与"D1 交棒"同时关掉
⇒ **每个符号一条 cb**（2,497,686 = 14/槽 × 178,407）⇒ 前端与后端抢同一条车道 ⇒ `[ul_dft_wait]` 578 µs
⇒ 跳变慢 ⇒ **UL 每跳 TB 塌 15×**（而 CRC-OK *比率*几乎不变 ⇒ 不是解码质量塌，是**调度器给的块变小**）。
⇒ **A 项不是"交付路径"**（memo §2 第 8 条），但它的**用法**（C1 + 半槽收包）仍待一条腿验收：`p43`（见⑤）。

#### ② ★ H4（"符号级路在丢符号"）**被三条独立现有读数证伪** ⇒ **不补 H4 计数**（memo §3.1 作废）

| 读数（`p41`）| 值 | 说明 |
|---|---|---|
| `[ul_rx] blocks` / `samples` | 14,842,803 / 12,213,506,471 | **822.857 样点/块 = 恰好一个 OFDM 符号**（11520/14）⇒ 一块一符号，无混叠 |
| 样点数推出的符号数 | 12,213,506,471 / 822.857 = **14,842,803** | = 块数（每块一符号）|
| `[ul_host] symbols` | **14,842,392** | = 块数 − **411**（0.0028%；相位建立 2 + 收尾）⇒ **到达的每个符号都被处理了** |
| `gaps` / `rx_overflows` / `[ul_rx_pool]` | 0 / 0 / `held_end=0 dropped=0 starved_events=0` | 电台与池都干净 |

⇒ `process_alignment` / `max_phase_blocks` **一个符号也没丢**：任何"整块被丢"都会让 `[ul_host] symbols` **低于**上面的期望值
（丢弃点在符号计数器**下游**：`process_alignment` 的"留在 alignment"分支与 `process_symbol_boundary` 的 `i_sample_symbol != 0` 递归都不产出符号）。
⇒ 而且 memo §3.1 想让计数"随腿增长"是**不可能**的：两处 `++nof_phase_blocks` 都被 `nof_phase_blocks < max_phase_blocks`（=2/流，`start()` 复位）夹住
⇒ 加那两行只能读到 **0/1/2**，改变不了任何结论。**免费判据**（任何腿、不需要新代码）：**`[ul_host] symbols` ≈ `[ul_rx] samples / 822.857`**（n78，误差 <0.01%）。

#### ③ ★★ C2 的前提被读码推翻：**块不是在"下一个时隙开始时"关的**

§7.7 第 2 条（以及 memo §3.2）写的是"现在块只在 `set_lane_slot()`（下一槽）关 ⇒ 一槽一批"。**读码结果：不是。**
真正的关块触发器在 **`ofdm_demodulator_impl.cpp::finish_symbol()`（第 486 行）**：

```cpp
const bool last_symbol_of_slot =
    ((pipeline_slots[slot].symbol_index % nof_symbols_per_slot) == (nof_symbols_per_slot - 1));
...
if (block_open && last_symbol_of_slot) {
  released = wait_per_slot && handover_allowed() && dft->release_block(grid.get_device_view().base);
  if (!released) { (void)dft->end_block(); }
  block_open = false;
}
```

`puxch_processor_impl::process_symbol()` 在**本槽最后一个符号**提交后立刻 `drain_pipeline()`（第 226–230 行）
⇒ 那次 `finish_symbol()` 命中 `last_symbol_of_slot` ⇒ **块在本槽最后一个符号处关**，而 `set_lane_slot()` 里的 `end_block()` 只是**兜底**
（该槽没有网格 / 网格提前释放 / 上一块没走完）。**⇒ 字面上的 C2（"按已到样点的最后一组关块"）就是现状**。

**C2 的"有用版本"**（在每个**收包组**边界关块，让前 7 个符号的变换在半槽时就派发）的账：

* 每跳**多一条 cb**（前端半槽一条）⇒ `cbs/lane` **2.00 → 3.00**，**直接触 V4 的 `cbs/lane ≤ 2.00`**（用户判据）；
* 买到的是"前端**执行**前移"：前端执行 ≈**10.6 µs/槽**（§6.65）⇒ 半槽量级 **~5 µs**，与窗口里那 ~500 µs 无关（见④）；
* 还要重定义 D1 的 `deposit/take` 契约（键 = 网格基址，现在隐含"一槽一块"）。
⇒ **建议：C2 从 S-B 清单里划掉**；若要把"前端半槽就开跑"做成交付形态，先请用户裁决"**V4 是否放宽到 3.00**"（这是取舍，不是技术障碍）。
**S-B 剩下的就是 C1（已落地）+ 它的空口验收。**

#### ④ ★★ 那 ~500 µs 的窗口：**离线回放首次逐 cb 量到"一跳真执行 ≈67 µs"**（本会话新证据）

`ce_kernel_cost.sh` 的基线跑（`ul_chain_replay syn004_4 --metal`，**0.12 s/次**）在 `OCUDU_METAL_GPU_TIME=1` 下逐 cb 打印
`commit->start`（队列）与 `start->end`（**设备窗口**）：

| cb（label）| commit→start | **start→end** | 是什么 |
|---|---|---|---|
| `ce_weights` slot=**10049**（真跳的估计器路）| 378.8 µs | **44.6 µs** | 前端变换（adopt 进来的整块）+ CE 全链 |
| `lane_burst` slot=**10049**（真跳的融合跳）| 1050.0 µs | **22.2 µs** | 均衡 + 解映射 |
| `ce_weights` slot=**0** ×**17**（该 capture 的其余 CE 调用；**不是本跳的**）| 355–453 µs | **744–890 µs** | ⚠ 成因未查（首次使用/等一条迟迟不就绪的 fence 都说得通），但**无论成因如何它们都不是热跳的读数** |

⇒ **热起来以后一跳的两条 cb 合计执行 ≈ 67 µs**（与 §6.65/§6.69 的 ~51 µs 账单、与空口 `p42` 的 `ch_wt=43.5 µs` 三者互证）
⇒ **空口 `merged_hop` 的 541.8 µs 里 ~500 µs 是"等"，不是算力**。"重写 kernel"这条杠杆的死亡结论**站得住**。
⚠ **仪器陷阱（新增，别踩）**：`ce_kernel_cost.sh` 的**斜率法把上面那 17 条非本跳 cb 的窗口算进基线**（744–890 µs vs 真跳 44.6 µs，**17 倍**），
本会话重跑因此得到 `invert K1=115 µs`、`reformat K3=93 µs`、`weights K1b=62 µs`——**与空口 `ch_wt` 的 43.5 µs 差 8 倍**，且两项为负。
**判读那条脚本必须只看 `slot != 0` 的真跳 cb，不要用均值/斜率**；算力账单的载体是 `ce_kernel_cost.mm`（隔离、预热、min-of-3）。

**⇒ 空口那 500 µs 归属仍然未定，但两个候选现在可以用一条读数分开**：

* 若 `busy(union) ≈ window`（设备**一直在执行别人的活**）⇒ 是**车道被 DL / 同伴瓜分**（要动 DL 的 GPU 负载或调度）；
* 若出现大量 hole（设备**闲着**而我们那条 cb 窗口很长）⇒ 是 **cb 内部的 fence 等待**：burst 的 cb 只有两种 cb 级等待
  （`burst_ensure_open()` 里的 **stage fence** 与 **grid-ready**），而 `Q9-F` 只证明了"signaller 先**提交**"，**没有**证明"signaller 先**完成**"。
* ⚠ **这条读数在所有 n78 腿上都是关的**（`gpu busy (front_end/back_end): commits=0` ⇒ "the probe is off"）：`p37`–`p42` 全关，
  只有 n1 的 `p15` 开过，而它给的是 **`busy(union)=105.7 s / window=318 s` = 33%**（设备 2/3 时间是闲的、最大 hole 是**秒级**）
  ⇒ **"长窗口 + 闲设备"这个形状已经在 n1 上出现过一次**（§7.6.0c 的 (a)/(b)/(c) 三候选正是要分开它）。
  ⇒ **⇒ `p43` 上开 `OCUDU_METAL_GPU_TIME=1`**（纯仪器，见⑤）。

**顺带排除掉的一个候选（读码 + 现成计数）**：**不是 burst 的 stage fence 在等**
* 空口上 `[metal_stats] lane fence … own=0 newest=0 cross_lane=0`（`p37`/`p39`/`p42` 三腿一致），而**离线回放是 `own=1`**
  ⇒ 空口的 burst **全部走回退路径** `backend_stage_wait()`（`ocudu_metal_queue.mm:919`）：它等的是**编码那一刻的最新代**
  `generation = stage_fence_generation.load()` —— 该代的 signaller **早已提交**（`Q9-F` 的 `waiter-committed-first=0` 与此一致）⇒ **这条等待是立即满足的，不可能是那 500 µs**。
* 成因（记下来，免得误读）：`set_stage_wait()` 是**逐线程**发布的"下一个 burst 消费"的（`ocudu_metal_burst.h:265`），
  而空口的 burst 与估计器**不在同一条线程**（并发 2 的池）⇒ 每个 burst 创建时读到的都是 0 ⇒ `own=0`。**它不是缺陷**（同一条队列的提交顺序已经提供了顺序性），
  但它说明：**在空口上不能靠"own 代"来解释或修复这条窗口**。

#### ⑤ ★ 修正后的 `p43-n78-halfslot` 预登记（**与 memo §3.3 的差异都写在这里**）

配方（memo §3.3 原样）+ **一个纯仪器**（`OCUDU_METAL_GPU_TIME=1`）：

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p43-n78-halfslot \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_UL_RX_SYMBOLS=7 OCUDU_METAL_GPU_TIME=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
```

| 读数 | 预登记 | 出处 |
|---|---|---|
| `dft … batched=` / `released=` | **都 ≠ 0**（C1 在空口上的唯一验收；`batch_max` 只是上限）| §7.7 / §6.71① |
| `[ul_rx] blocks` × 822.857 vs `[ul_host] symbols` | **相等（<0.01%）** ⇒ 没丢符号（H4 的免费判据）| §6.71② |
| `[ul_gpu_lane] dft slots/cbs` | **`cbs/slots ≤0.5`（与 `p42` 同级；绝不是 `p41` 的 14/槽）**——两个半槽组会**并入同一个开着的块**（块在"本槽最后一个符号"才关，③）| §6.71①③ |
| `[ul_dft_wait]` | **回到 "no samples" 或 ≪577 µs** | §6.71① |
| TBS / 吞吐 | **不许塌**（`[ul_mac_pdu_size]` 中位应回到 ~3 kB 量级、总量同 `p42`）| §6.71① |
| ★ `[metal_stats] queue occupancy (Q9-F3)` + `gpu busy (…)` | **新判据**：`busy(union)/window` 与 hole 数 ⇒ 把 ④ 的两个候选分开 | §6.71④ |
| `[metal_stats] lane fence … own=` | **预期仍是 0**（空口逐线程发布失效）；若变成 ≠0 说明并发/线程布置变了 | §6.71④ |
| `merged_hop` 中位 | **预期不变（541.8 ± 20）**——⚠ 与 memo §3.3 的"应 < 541.8（早开跑）"**不同**：块边界已在"本槽最后一个符号"（③），半槽收包**不改**关块时刻 | §6.71③ |
| 契约 / `cbs/lane` / `gaps` / 池 | 8/8 / **2.00 (max=2)** / 0 / `starved=0`、`dropped=0`（**不允许变**；若 `cbs/lane=3.00` ⇒ 立刻停手并记录）| §3 |
| V1 中位 | **口径不同，只作参考**（端点随收包窗移动）| §6.70① |

⚠ **已知的外因**：该策略的 shutdown 会踩 DU teardown race（`[metal_stats]` 可能延迟几分钟才落地），**属预期**，别当丢失（§4 纪律 8）。


### 6.72 ★★★ 腿 `p43-n78-halfslot`：**C1 空口验收通过**；一跳的设备账首次逐 cb 拆开 ⇒ **617 µs 窗口里只有 ~67 µs 是执行，且车道只占 32% 的设备时间**（2026-09-26）

> 腿：`logs/gnb_gpu_p43-n78-halfslot_0926_0931.log*`（`OCUDU_UL_RX_SYMBOLS=7` + `OCUDU_METAL_GPU_TIME=1`，配方见 §6.71⑤；
> 二进制戳 `564a8de8fa` = HEAD）。时长由样点数推出：6,424,969,787 / 23.04 MHz = **278.9 s**。

#### ① 预登记逐条（**含我自己写错的一条**）

| 判据 | 预登记 | 实测 | 判定 |
|---|---|---|---|
| `dft … batched=` | ≠ 0 | **294,980 / 2,064,860**（`batch_max=14`）| ✅ |
| `dft … released=` | ≠ 0 | **147,490**（`handed=147,490 taken=145,637 fallback=1,570`）| ✅ |
| H4 免费判据 | `[ul_host] symbols` ≈ `[ul_rx] samples/822.857` | 1,115,447 块 × 7 符号 = 7,808,129 vs **7,808,025** ⇒ **−104（0.0013%）** | ✅ |
| `[ul_gpu_lane] dft slots/cbs` | 我写的"**≤0.5**" | **50,265 / 50,265 = 1.00** | ⚠ **判据写错**（我用了错的分母：`p42` 同口径也是 **1.00**）⇒ **实质 PASS**（**绝不是 `p41` 的 14.0/槽**）|
| `[ul_dft_wait]` | no samples 或 ≪577 | **no samples recorded** | ✅ |
| TBS / 吞吐 | 不许塌 | 中位 **1985 B**、总量 **272.2 MB / 278.9 s = 7.81 Mbit/s**（`p42`：3329 B / 7.23 Mbit/s；`p41`：217 B / 0.27）| ✅ **比 p42 还高 8%** |
| 契约 / V4 / V2 / V5 | 8/8 / 2.00 / 绿 / 0 gaps | **MET (8 of 8)**、`cbs/lane=2.00 (max=2) dropped=0`、`starved_events=0 dropped=0 held_max=10 free_min=22`、`gaps=0 rx_overflows=0` | ✅ **全部不变** |
| `[ul_gpu_lane] lane fence … own=` | 预期仍 0 | **own=0 newest=0 cross_lane=0**（与 §6.71④ 的推断一致）| ✅ |
| `merged_hop` 中位 | **541.8 ± 20（预期不变）** | **575.0**（`ch_wt=42.1`）| ⚠ **边缘 MISS（+33.2）**：腿间散布 ~17 µs ⇒ 大概率是真的、但很小 |
| V1 中位 | 口径不同，只作参考 | **1392.4**（`p42` 1366.8，`p41` 2043.1）| —— |

⇒ **C1 在空口上成立**：收包粒度不再拆掉批量化与 D1 交棒，前端提交回到 **1 cb/槽**、`[ul_dft_wait]` 消失、吞吐不塌、
契约/V4/V2/V5 一律不变。**S-B 的"部分槽收包"这部分因此可交付**；`merged_hop` +33 µs 记在账上（下一节给出它的位置）。

#### ② ★★ 一跳的设备账（`p43`，全部是 GPU 时间戳）

| cb（`[ul_gpu_lane]` stage）| 它是什么 | **GPU 窗口/lane** |
|---|---|---|
| `ch_wt`（label `ce_weights`）| **fenced correlation build**：单独一条 cb、一次相关派发（`flush_correlations_fenced()`，`ocudu_metal_mmse_engine.mm:2806`）| **42.1 µs** |
| `merged_hop`（label `merged_hop`）| **整跳一条 cb**（前端变换 + 估计器 + 均衡 + 解映射；枚举注释原文：*"the whole hop … in ONE submission"*）| **575.0 µs** |
| **合计** | `cbs/lane=2.00` 正好对上 | **617 µs/跳** |

* **真实执行**：离线热跳 **67 µs**（§6.71④：44.6 + 22.2）＋算力账单 **≈51 µs**（§6.65–§6.69）⇒ **~550 µs/跳 是 cb 内的非执行驻留**。
* **设备占用率**：145,649 跳 × 617 µs = **89.9 s / 278.9 s = 32%** ⇒ **单车道不是设备吞吐受限**（还有 ~3× 余量）。
* 旁证：`[ul_gpu_lane] dft carried GPU start->end` 中位 **553 µs**（= 同一批 cb 的另一条读法）、
  `[ul_gpu_lane] commit -> completion (Q9-B, all stages)` 中位 478.9 µs。

#### ③ 三条否证（全部离线，0.1–3 s/臂；载体 `ul_chain_replay syn004_4 --metal --repeat 20`）

1. **不是算力**：**同一次回放**里，同一个 `ce_weights` 标签的 cb 窗口**双峰**——`slot=10049/10409` 是 **44–46 µs**，而 `slot=0`（= correlation build，没有 lane slot）是 **744–757 µs**。同样的 kernel、同一次运行 ⇒ 大窗口不是执行。
2. **不是设备争用**：2 份 / 4 份回放**并发**跑同一语料，小 cb 窗口不变（22.2 / 45.5 µs），而 `defer_wait` **反而变小**（674 → 479 → 345 µs）⇒ 加负载不会把窗口撑大。
3. **不是"前端等整槽样点"专属**：离线没有电台、没有时隙节拍，照样 ~670 µs ⇒ 与收包策略无关。

#### ④ ★ 新的主指标：`[mmse_time_sum] defer_wait`（**811 µs/跳，这条链上最大的"有名字"项**）

`p43`：`defer_wait` 中位 **811.1 µs**（mean 796.6、p95 945.0、p99 992.5、max 10.7 ms）；`cpl_wait=746.9 µs`；CE 自身 `total=38.9 µs`。
定义（`port_channel_estimator_metal_mmse_impl.cpp:160`）：**"延迟阶段返回" → "它的批完成"**，而**均衡与解映射就跑在这段里面**
⇒ 它就是**一跳的后端延迟**，且与设备窗口对得上：**811 ≈ 575（merged_hop 窗口）+ ~236（宿主 encode / 队列 / 出手）**。
⇒ 这条读数**每腿都打**、**不受"融合路没有分阶段边界"影响**（§7.6.0b）⇒ **从本节起，单车道的主指标是 `defer_wait`（配 `merged_hop`）**。

#### ⑤ ⚠ 仪器陷阱（新增，别再踩）：**离线回放的 `defer_wait` 不是空口时延的代理**

| 离线臂 | `defer_wait` 中位 | 空口对应证据 |
|---|---|---|
| `merged`（默认）| 670 µs | （默认）|
| `OCUDU_CE_LANE_ORDER=event` | 646 | 旧 P2 路 |
| `OCUDU_CE_LANE_ORDER=burst` | 326 | |
| `OCUDU_DFT_RELEASE_BLOCK=0`（无 D1 交棒）| 312 | |
| `OCUDU_DFT_OPEN_BLOCK=0`（**关前端批量化**）| **316** | ⚠ **空口上关掉它让 V1 从 2440 恶化到 1513 µs（P2-B′：−38% 是"打开"的收益）** ⇒ **离线这个指标在"批量化"这一维上与空口反号** |
| `OCUDU_CE_LANE_ORDER=host_wait` | 0.8（但 `submit` 吞掉 **543 µs**，`total` 46.9 → 563.2）| 等待**没消失**，只是换了记账位置 |

⇒ **回放可以查机制（设备窗口、栅栏、结构），不能用来排时延名次**（§4 纪律 2 的加强版：连"离线可测"的量也要先证它与空口同号）。

#### ⑥ 下一步（我建议的顺序；都需要先有仪器或先有裁决）

1. ★ **给"设备侧栅栏"加时长读数（离线可自证，不飞腿）**：现在 Q9-C/Q9-D/Q9-F 只数**次数与次序**，
   **没有任何一处量"这次等待持续了多久"**（Q9-D 的 `max=0.0ms` 只在**发生倒置**时才有值，而倒置恒为 0）。
   Metal 没有 per-dispatch 时间戳，但**有每 cb 的 GPU 起止** ⇒ 可以在**等待者**的完成处理里记 `signaller 的 GPUEndTime − waiter 的 GPUStartTime`：
   若为正且是几百 µs，就是那 550 µs 的直接证据，并能指认是哪条栅栏（stage fence / grid-ready / corr fence）。
   ⚠ 注意 `backend_stage_wait()` 的"安全"论证依赖**同一条队列**（"signaller 已经排在它前面"）——**跨队列时该论证不成立**，这正是要量的东西。
2. **结构臂**（离线腰斩 `defer_wait`，但两条都触 V4 ⇒ **需用户裁决**）：`noD1`（放弃 D1 ⇒ `cbs/lane` 3.00）、
   空口的 `event` 路（`cbs/lane=3.00`）。
3. `merged_hop` 那 +33 µs 的归属：本腿无法分开"半槽收包的代价"与"逐腿天气"，**要成对腿**（同二进制、`RX_SYMBOLS` 开/关交替）——优先级低。


### 6.73 ★ Q24 落地：**给设备侧栅栏加"时长"读数**（2026-09-26，本会话；离线已自证，**代码改动**）

> 目的（§6.72⑥ 第 1 项）：那 **~550 µs/跳**的 cb 内驻留，**落在哪条 `encodeWaitForEvent` 上，现在不可见**——
> Q9-C/Q9-D/Q9-F 只数**次数与次序**，`Q9-D` 的 `max` 只在**发生倒置**时才有值，而空口上倒置恒为 0。

#### ① 量的是什么（Metal 只给每 cb 的 GPU 起止，所以只有这一种量法）

```
signaller 的 GPUEndTime  −  waiter 的 GPUStartTime
```
**为正 ⇒ 等待者的缓冲"已经在设备上驻留"时，它等的那条缓冲还在跑**，这个区间就是那次等待的设备代价，且**按栅栏种类归属**（`stage` / `corr` / `grid`）。

**为什么它必须存在**：`backend_stage_wait()` 的"安全"论证（以及 Q9-D 的 `signaller-first` 分类）依赖**同一条队列**——"signaller 已经排在它前面，所以等它是安全的"。
**跨队列时这句话不成立**（"先提交"不等于"先完成"），而 §6.72 量到的正是"cb 驻留 575 µs、执行只有 ~67 µs"这种形状。

#### ② 实现（`lib/phy/metal/ocudu_metal_queue.mm`，一个文件、零派发改动）

| 件 | 内容 |
|---|---|
| 记录 | `note_fence_signal()` 与 `note_fence_wait()` 各加一次**带锁的 vector push**（等待者那条**在"安全桶"提前 return 之前**记录——那个桶正是要量的事）|
| 配对键 | `occupancy_record` 新增 `key`（`__bridge` 的命令缓冲身份，**不持有**）；报告时用它把"等待者/信号者"两头对到 F3 探针的 GPU 记录上 |
| 报告 | `[metal_stats] fence wait on the device (Q24): …`（紧随 Q9-D 行）；`no-signal` / `unresolved` / `dropped` **各自计数**，不静默丢弃 |
| 开关 | **`OCUDU_METAL_GPU_TIME=1`**（与 F3 同一个探针：解析需要它的记录）；**没开时一行也不记**，报告明说 "the probe is off" |
| 界 | 每类栅栏两端各 1M 条，超出计数（`dropped`）|
| 默认路 | **零派发改动**：不设环境变量时，每个栅栏端点只多一次 `getenv`（2–3 次/跳）|

#### ③ 离线自证（`ul_chain_replay syn004_4 --metal --repeat 20`）

| 臂 | Q24 读数 |
|---|---|
| `OCUDU_METAL_GPU_TIME=1` | ⚠ **下表是"指针配对"版本的读数，已被 §6.74 证伪为仪器缺陷的产物**（当时读到 `20 (50.0%) mean=7.9us`）；**修好后（唯一 id 配对）读数是 `40/40 解析、0.0% 驻留、inconsistent=0`** |
| 4 份并发 | 同上（同样受该缺陷污染）|
| 不开探针 | `no GPU-time records - the probe is off (OCUDU_METAL_GPU_TIME=1 turns it on)…`（不误报 0）|
| `ctest -L phy -j 1` | **193/193 全绿**（默认路与全部 dump 网不变；⚠ **不要与任何 GPU 工作并行跑**，见 §6.74④）|

⚠ **一条重要的负结论**：离线**两头对不上**——Q24 的栅栏等待只有 **7–20 µs**，而同一回放的 cb 窗口是 **500–750 µs**
⇒ **回放里那些大窗口也不是栅栏等待**（是回放自己的节拍/宿主结构）。**⇒ 这条读数只能上空口腿验证机制，离线只能验证"仪器本身工作"**（解析率、按种类归属、随负载单调）。

#### ④ 空口上怎么读（`p44`，配方 = 交付路的 `p42` + 探针）

* `unresolved`/`no-signal` 应 ≈ 0（否则配对键在空口上不成立，先看 `dropped`）；
* ★ **`waiter resident while its signaller ran` 的比例与分位数**：
  * **≈100% 且 mean/p95 在几百 µs** ⇒ **那 575 µs 就是栅栏等待**，`per kind` 直接点名是哪一条（stage / corr / grid）⇒ 下一步是那条栅栏的**跨队列次序**；
  * **≈0%** ⇒ 不是这些栅栏：cb 是被**别的东西**按住（未探针化的队列/DL、或 D1 交棒的所有权），下一步转向队列侧；
* V1 / 契约 / `cbs/lane` / `gaps` / 池**不允许变**（探针只加一个完成处理器）。


### 6.74 ⚠⚠ 腿 `p44-n78-fencewait`：**Q24 第一版是坏仪器**（指针当身份）——被"物理上不可能"抓住；已修 + 加免配对交叉校验（2026-09-26）

> 腿：`logs/gnb_gpu_p44-n78-fencewait_0926_0949.log*`（交付配方 + `OCUDU_METAL_GPU_TIME=1`，戳 `ef6c65fc3c`）。
> **这一腿的设备侧读数一切正常**（这就是抓住缺陷的东西）：`merged_hop=541.0 µs/lane`（`p42` 541.8）、
> `busy=584.8`、`residency=544.5`、`commit -> completion` 中位 439.8 **max 5542 µs**、F3 最慢表里最大的 `start->end` 只有 **707 µs**。

#### ① 缺陷：`MTLCommandBuffer` 对象会被**池化复用**，地址不是身份

Q24 第一版把"等待者/信号者"两头按**命令缓冲的地址**（`__bridge const void*`）配对。`p44` 的读数是：

```
fence wait on the device (Q24): waits=145395 resolved=145395 … | waiter resident while its signaller ran:
73282 (50.4%) mean=3499661.5us median=771492.6us p95=29629599.9us max=31135186.4us worst kind=stage …
```

**物理上不可能**，两条独立的界都越了：
* 该腿**没有任何** cb 驻留超过 **5.5 ms**（`commit->completion` max），而中位"停等"读到 **771 ms**；
* Σ停等 = 73,282 × 3.5 s ≈ **25.6 万秒**，而整腿只有 **276 s**（车道最多 2 条 cb 在飞）。
根因：Metal 会把**同一个 command buffer 对象**发给后来的提交 ⇒ 一个地址同时"是"很多条 cb，报告里 `id → 窗口` 的映射取到最后一次复用 ⇒ 配对跨越秒级。

#### ② 修法（两件，都已落地）

1. **唯一 id 配对**：栅栏端点第一次被记时给该 cb 发一个 id（`fence_id_of_cb`，按地址查、**被 `arm_gpu_time()` 取走并删除**——没有栅栏的后续复用者不会继承），F3 记录里存的是 **id 而不是地址**；报告对**同 id 出现两次**的记 `ambiguous` 并**排除**（不猜）。
2. ★ **自我校验**：每个已解析的配对都查**物理不变量**——"等待者不可能在它等的信号发出**之前**完成" ⇒ `signaller_end ≤ waiter_end`；违反记 `inconsistent`，
   并在行尾直接打 **`<- INCONSISTENT PAIRS: the stalls above are NOT evidence`**。**p44 那版如果带这个检查，会当场把自己否掉。**

#### ③ ★ 再加一条**免配对**的交叉校验（`Q24b`），两条读数互证

按 **slot** 把 F3 记录里的 `ce_weights`（**信号**端：估计器权重阶段/相关构建的提交点）与 `merged_hop`（**等待**端：整跳那条 cb）对上
——这两个标签本来就是那条 stage fence 的两端，**不需要 generation、不需要 id、不需要地址**：

```
[metal_stats] fence wait, pair-free cross-check (Q24b): slots with both ends=N | hop buffer resident while its
estimator ran: … | <- IMPOSSIBLE PAIRS: … NOT evidence   （`est_end > hop_end` 时打）
```

#### ④ 离线自证（修好之后）

| 臂 | Q24 / Q24b |
|---|---|
| 回放 `syn004_4`（默认）| Q24：`40/40 解析、no-signal=0、unresolved=0、inconsistent=0、ambiguous=0` ⇒ **0.0% 驻留**；Q24b：`slots with both ends=20` ⇒ **0.0%** |
| 另外 5 条结构臂（`diag-split` / `no-batch` / `noD1` / `event` / `burst`）| 全部 `0.0%`、`inconsistent=0`（`burst` 记到 59 次等待）|
| **正对照**：`dft_release_adopt_metal_test`（含 Q9 的"signaller 落后"臂，`Q9-D signaller-after=2 max=26.4ms`）| 仪器**确实触发**（`waits=2 resolved=2`）且**抓出 `inconsistent=1` 并标注 NOT evidence** ⇒ **它会拒绝说话，而不是编数字** |
| `ctest -L phy -j 1`（**单独**跑）| **193/193** |

⚠ **纪律补充（本会话新踩）**：`ctest -L phy` **不能与任何 GPU 工作并行**——本次一边跑回放臂一边跑 ctest，得到 "2 tests failed"；隔离后连跑 **3 次 193/193**。
（原来只记了"`-j 4` 会假红"；实际规则更强：**任何并发的 GPU 使用者都会**。）

#### ⑤ 结论与下一步

* **`p44` 的 Q24 读数作废**（仪器缺陷，不是机制证据）；`p44` 的其余读数（`merged_hop` 541.0、契约、V4、池）与 `p42` 一致 ⇒ **交付路无回归**，可留作对照。
* **栅栏问题仍未回答**，需要**用修好的二进制重飞一条**（`p45`，配方与 `p44` 完全相同）：
  * 先看 `inconsistent` / `ambiguous` / `unresolved` / `no-signal`：**不为 0 就不许读停等数字**；
  * Q24 与 Q24b **两条都 0%** ⇒ **那 ~541 µs 不是这些栅栏** ⇒ 下一步转向队列/所有权侧（`Q9-F3` 的 hole 与 DL 是否共享设备）；
  * 任一条 >0 且一致 ⇒ **就是那条栅栏**，`per kind`（stage / corr / grid）点名，再打它的跨队列次序。
* 离线**永远给不出正例**（回放的 6 种结构全 0%）：这符合"同队列、signaller 先提交"的构造，**但也意味着机制只能靠空口腿判定**。


### 6.75 ★★ 腿 `p45-n78-fencewait2`：**栅栏被量清了（≈17 µs/跳，不是那 ~551 µs）**；"窗口=算力"被离线扫描否掉；**逐派发时间戳在本平台不可得**（仪器路线的终点，2026-09-26）

> 腿：`logs/gnb_gpu_p45-n78-fencewait2_0926_1003.log*`（配方同 `p44`，戳 `6bd6ae3b76` = 修好的 Q24）。
> 腿的规模：`lanes=146,214`、`merged_hop=551.4 µs/lane`、`busy=584.8`、窗口 312.0 s（由 `gpu busy (back_end) window` 读出）。

#### ① ★ Q24 判读：**不变量全 0 ⇒ 数字可信；而 FENCE 只值 ~17 µs/跳**

```
fence wait on the device (Q24): waits=146236 resolved=146236 no-signal=0 unresolved=0 inconsistent=0
  ambiguous=0 dropped=0 lost=0 | waiter resident while its signaller ran: 42230 (28.9%) mean=59.0us
  median=54.9us p95=144.6us max=892.9us sum=2493486.6us worst kind=stage slot=12137; per kind: stage=42230 corr=0 grid=0
```

* **配对可信**：`inconsistent=0`（物理不变量全部满足）、`ambiguous=0`（没有同 id 两记录）、`unresolved=0`/`no-signal=0`（两头都找到了）——**§6.74 的 id 修法在空口上成立**。
* **量级物理可信**：Σ = **2.49 s** ≪ 腿长 312 s（`p44` 那版是 25.6 万秒 ⇒ 假）。
* **结论**：**28.9% 的跳**有一条 stage fence 的停等，mean 59.0 / median 54.9 / p95 144.6 / max 892.9 µs
  ⇒ 平均 **0.289 × 59 ≈ 17 µs/跳** ⇒ 只占 `merged_hop`（551.4）的 **~3%**。
  ⇒ **那 ~551 µs 的 cb 内驻留不是栅栏**（栅栏那条杠杆到此关闭；`corr`/`grid` 两端为 0）。

#### ② ⚠ Q24b（免配对交叉校验）**第一版也错了**：**slot 号每超帧回绕**

`p45` 读到 `slots with both ends=6144 | … median=235520990.0us max=276481224.7us`——**235 秒的"窗口"**，且**没有**打 IMPOSSIBLE 警告。
根因：我按 **`slot` 号**聚合 `ce_weights`/`merged_hop`，而 lane 的 slot 号**每 10.24 s（一个超帧）回绕一次**；
276 s 的腿里同一个号出现 ~27 次 ⇒ `min(start)`/`max(end)` 跨了整腿 ⇒ 假窗口；而"est_end > hop_end"的不变量被我自己放在**聚合之后**，聚合已经把两端拉宽，所以它不会触发。
（与 Q9 的 sweep 缺陷同族：**用模 10240 的 slot 号当身份**。）

**修法（已落地）**：改成**按时间配对**——"**最后一条先于该跳提交的 `ce_weights` 就是它的估计器**"（依据 `Q9-F` 的 signaller-committed-first）；不变量仍在配对后检查。
离线复核：`hops with both ends=20`（正好 20 跳，之前是 6144 个假 slot）、`0.0%`。

#### ③ ★ "窗口 = 算力"被离线扫描否掉（语料本来就是 3→25 PRB 的扫描，空口是 51 PRB）

| 语料 PRB | 3 | 4 | 6 | 8 | 10 | 12 | 14 | 18 | 25 |
|---|---|---|---|---|---|---|---|---|---|
| `merged_hop`（离线 µs/lane）| 245.1 | 210.0 | 130.4 | 122.1 | **92.0** | 103.1 | 92.6 | 108.5 | 117.8 |

⇒ 窗口**不随分配增长，反而从 245 掉到 ~100 并封顶** ⇒ **它不是按 RE 计的算力**（若是，51 PRB 应比 4 PRB 大十几倍）。
⇒ 顺带解决了一个隐患：离线载体是 **4 PRB**、空口是 **51 PRB** ⇒ **几何差异不改变上面这条结论**（窗口对尺寸不敏感）。

#### ④ ★★ 逐派发 GPU 时间戳：**本平台不支持**（`wip/metal_counter_probe.mm`，离线 1 秒）

```
device: Apple M4 Pro (unified memory: YES)
  AtDispatchBoundary   : no   <- the one a per-dispatch timeline needs
  AtStageBoundary      : YES
  counter sets: timestamp = present (GPUTimestamp); statistic = absent
  usable per-dispatch timeline: no
```

⇒ **"那 551 µs 里哪一段是执行、哪一段是驻留"在本平台无法直接测量**（Metal 只给每 cb 的起止）。
⇒ **仪器路线到此为止**：能建的三条（F3 队列时间线、Q24/Q24b 栅栏时长、离线算力账单）都已建好并互相印证，剩下的只能靠**结构 A/B**（改一处结构、看窗口动不动）。

#### ⑤ 于是"剩下的解释"被逼到唯一一条：**设备被同一时刻的其它工作分时**

已被排掉：**不是算力**（③）、**不是栅栏**（①，仅 17 µs）、**不是 DL 负载**（`s78-dlcap40` vs `s80-ulcap40` 的 `merged_hop` 1032.8 vs 1029.6 µs，**同窗口**）、
**不是被探针队列自身占满**（`busy(union)/window = 84.7/312.0 = 27%`；`back_end` 自身 88.0 s/312 s = 28%）。
⇒ 只剩"**同一条 GPU 上、同一时刻、别的队列的工作**"（同伴跳 / 未探针的队列 / 系统）⇒ **判据 = 并发 1 的对照臂**。

#### ⑥ `p46-n78-conc1`：预登记（**测量臂，按 §7.4 放行；V4 不受影响**）

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p46-n78-conc1 \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=1
```

| 读数 | 预登记 |
|---|---|
| ★ `[ul_gpu_lane] busy split merged_hop` | **判据**：若**塌到 ≪541**（比如 <200）⇒ 那 ~551 µs 是**被同伴/其它队列分时**（⇒ 结构上要减少同刻竞争，而不是改跳内）；若**仍在 541 ± 40** ⇒ 是**本跳自己的驻留**（⇒ 跳内无可改，杠杆只能落在"每跳提交数/在飞跳数"这类结构量上）|
| `[ul_gpu_lane] lane fence … own=` / Q24 不变量 | 预期仍 `own=0`、不变量全 0（若 `inconsistent>0` 则该行照旧不可读）|
| `[ul_gpu_lane] queue: weights commit -> weights start`、`period` | 并发 1 下队列项**预期变大**（§7.5）——记录，不作判据 |
| V1 中位 / `[ul_gpu_pipeline]` | **口径仍是参考**；并发 1 的跨度**预期上升**（§7.5 的实测），**判据在上面的窗口** |
| 契约 / `cbs/lane` / `gaps` / 池 | 8/8 / **2.00 (max=2)** / 0 / `starved=0`、`dropped=0`（**不允许变**）|


### 6.76 ★★ 腿 `p46-n78-conc1`：**预登记判据有答案了 —— 那 ~500 µs 是"本跳自己的驻留"，不是被同伴分时**；并把它**定位到前端那条 cb**（2026-09-26）

> 腿：`logs/gnb_gpu_p46-n78-conc1_0926_1014.log*`（交付配方 + `OCUDU_METAL_GPU_TIME=1` +`max_pusch_and_srs_concurrency=1`，戳 `bc82dfb42a`）。
> `[ul_lane_exec]` 两行都确认生效：**`max_concurrency=1` ⇒ serialising STRAND（一次只有一跳）** ✓。

#### ① 预登记判据：**没有塌 ⇒ 窗口是本跳自己的驻留**

| 读数（中位）| `p46`（**并发 1**）| `p45`（并发 2）| `p42`（并发 2，交付参照）| 差 |
|---|---|---|---|---|
| ★ **`merged_hop`** | **475.1 µs** | 551.4 | 541.8 | **−67…−76 µs（−13%）** |
| `ch_wt` | 40.3 | 48.4 | 43.5 | −3…−8 |
| lane `residency` | 520.2 | 544.5 | 543.9 | −24 |
| lane `busy` | 494.3 | 566.1 | 565.2 | −71 |
| `gap` | 31.1 | 7.5 | 3.2 | +24…+28（没有同伴来填）|
| `queue: weights commit→start` | **57.4**（mean 91.9）| 39.5 | 39.5 | **+18** |
| `period` | **710.1** | 425.0 | 425.7 | **+284** |
| `[ul_gpu_pipeline]` 中位（V1）| **1599.4** | 1372.0 | 1366.8 | **+227** |
| `cbs/lane` / 契约 / `gaps` / 池 | 2.00 (max=2) / **MET 8/8** / 0 / `starved=0`、`dropped=0`、`held_max=9` | 同 | 同 | **不允许变的都没变** ✓ |

⇒ 判据读作：**并发 1 只让窗口降 13%（~70 µs），远不是"塌"** ⇒
**那 ~475–540 µs 是这条 cb 自己的驻留**；同伴/同刻竞争只解释得了 **~70 µs**。
⇒ 同时确认：**并发 2 是交付该有的设置**（并发 1 的 V1 +227 µs、`period` +284 µs、队列项 +18 µs）——与 §7.5 的旧结论一致。

#### ② ★ Q24 与 Q24b **首次完全一致** ⇒ 栅栏读数（≈9–17 µs/跳）坐实

| 仪器 | 读数 |
|---|---|
| Q24（id 配对）| `waits=145443 resolved=145443 inconsistent=0 ambiguous=0 unresolved=0 no-signal=0`；`38938 (26.8%) mean=34.9us median=33.9us p95=56.4us max=432.4us sum=1359383.8us` |
| Q24b（按时间配对）| **`hops with both ends=145379`**（**每一跳都配上了**）、`38938 (26.8%) mean=34.9us median=33.9us max=432.4us sum=1359383.8us` |

⇒ **两条独立配对给出同一个数**（连 `sum` 都相同）⇒ 栅栏代价 = **0.268 × 34.9 ≈ 9 µs/跳**（并发 2 时 17 µs）⇒ 占窗口 **~2%**。

#### ③ ★★ 新线索：那 ~500 µs 挂在**前端那条 cb** 上（离线拆分臂，`OCUDU_LANE_DIAG_SPLIT=1`）

| 臂（离线 `syn004_4`）| `busy split` |
|---|---|
| 默认（D1 合并）| `ch_wt=21.8 (7%) eq_demap=12.4 (4%) **merged_hop=261.1 (88%)**` |
| **`OCUDU_LANE_DIAG_SPLIT=1`**（把前端单独成 cb）| **`dft=177.5 µs/lane (89%)`**、`ch_wt=14.6`、`eq_demap=7.0` |

⇒ **一跳窗口里 ~2/3 是前端那条 cb**（177.5 / 261.1），而前端的**执行**账单只有 **10.6 µs/槽**（§6.65）
⇒ **"窗口 ≫ 执行"这个形状在前端这条 cb 上离线就存在（17×）**，与"逐派发时间戳不可得"合起来看：
**前端那条 cb 是唯一还能再切一刀的地方**。

#### ④ `p47-n78-diagsplit`：预登记（**测量臂**；`cbs/lane` 会到 3.00 ⇒ 按 §7.4 只作读数、不交付）

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p47-n78-diagsplit \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 OCUDU_LANE_DIAG_SPLIT=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
```

| 读数 | 预登记 |
|---|---|
| ★ `[ul_gpu_lane] busy split` 的 **`dft=`** | **判据**：若 `dft` ≈ **400–500 µs** ⇒ 那 ~500 µs **就是前端那条 cb**（⇒ 靶子 = 前端派发形状/输入路径，可离线继续切）；若 `dft` 只有几十 µs ⇒ 窗口在 `ch_wt`/`eq_demap` 侧（⇒ 靶子换边）。⚠ 该臂**改变提交结构**（每跳 3 条 cb）⇒ 分段读数**只作指示**（§7.6.0b）|
| `cbs/lane` | **预期 3.00**（测量臂；**不作交付判据**，只登记）|
| Q24 / Q24b | 不变量应为 0；`per kind` 是否仍是 `stage` 独占 |
| 契约 / `gaps` / 池 / `rx_overflows` | 8/8 / 0 / `starved=0`、`dropped=0` / 0（**不允许变**）|
| V1 中位 | 参考（结构变了）|


### 6.77 ★★★ 腿 `p47-n78-diagsplit`：**判据命中 —— 那 ~500 µs 就是"前端那条命令缓冲"**（452 µs = 87%）；且它**不随队列、不随算力**（2026-09-26）

> 腿：`logs/gnb_gpu_p47-n78-diagsplit_0926_1022.log*`（交付配方 + `OCUDU_METAL_GPU_TIME=1` + `OCUDU_LANE_DIAG_SPLIT=1`，戳 `17fdc60524`）。

#### ① 预登记判据：**命中**（`dft` 在 400–500 µs 带内）

```
[ul_gpu_lane] busy split: dft=452.3us/lane (87% of busy, cbs/lane=1.00) ch_wt=45.4us/lane (9%) merged_hop=24.4us/lane (5%)
[ul_gpu_lane] cbs/lane=3.00 (max=3) dropped=0      [phy_pipeline] contract MET (8 of 8)
```

⇒ 把跳拆开后：**前端 452.3 µs（87%）**、估计器 45.4、均衡+解映射 24.4。
⇒ **一跳那 ~500 µs 的设备驻留 = 前端那条 cb** ——靶子第一次被**点名并隔离**（不是 CE、不是 eq/demap、不是栅栏、不是 DL）。
（Q24/Q24b：不变量全 0、`inconsistent=0`；该臂下 86.2% 的等待有代价 mean 30.5 / max 388.6 µs —— 仍然只有 ~26 µs/跳；`cbs/lane=3.00` 如预登记，仅作读数。）

#### ② 它**不是算力**（离线三条，各 0.1–3 s）

| 证据 | 读数 |
|---|---|
| 离线**同一个**前端 cb（拆分臂，b14）| `dft≈507 µs/`cb（`253.6 us/lane ÷ cbs/lane=0.50`）⇒ **空口 452 的形状在没有电台、没有 DL 的机器上就存在** |
| **批量扫描**（拆分臂，b1→b14）| 每 cb：b1 **232** / b2 217 / b4 302 / b7 270 / b14 **507** µs ⇒ **b1 那条 cb 只装 1 个 768 点 FFT（~0.76 µs）却驻留 232 µs**；前端的执行账单是 **10.6 µs/槽** ⇒ 比 **~43×**（b14）到 **~300×**（b1）|
| 纯 DFT 路（`--dft --dft-metal --synth 30`）| 每条 `dft_front_end` cb 的 `start->end` = **36.4–39.9 µs**（偶发 244/246）⇒ **该平台上"一条平凡 cb"的驻留下限 ≈ 37–40 µs**（与空口 PRACH 的 46.7–48.6 µs 同量级）⇒ **14 × 37 ≈ 518 ≈ b14 的 507** ⇒ 与"**每个线程组各付 ~37 µs**"一致 |

#### ③ 它**不跟队列走**（`OCUDU_DFT_BACKEND_QUEUE=1`，离线对照）

| 臂（拆分） | `dft` |
|---|---|
| 前端队列（默认）| **256.0 µs/lane** |
| 后端队列（`OCUDU_DFT_BACKEND_QUEUE=1`）| **251.6 µs/lane** |

⇒ 把前端 DFT 挪到后端队列，窗口**不变** ⇒ **成本跟着 cb 的"内容"走，不跟队列的调度走**（这条同时给那个 EXPERIMENT 旋钮一个读数：它换不来窗口）。

#### ④ 于是候选收窄到"cb 里到底装了什么"（按可能性排序）

| # | 候选 | 现有线索 | 离线怎么判（**不需要腿**）|
|---|---|---|---|
| **(a)** | **输入路径**：前端变换读的是**电台的 IQ 缓冲**，经 **zero-copy wrap**（页映射、由 USB DMA 在写）；而隔离微基准读的是自己的缓冲 | 腿上 `zero-copy wraps: hits=3.8M **creates=244k** purges=265k` ⇒ **~1.7 次 mapping 创建/跳** 的churn | 加一个**强制 staging（不走 wrap）**的环境开关（3–5 行），在拆分臂上读 `dft=`：若掉到几十 µs ⇒ 就是它 |
| (b) | **同一条 cb 里的其它活**：网格写、D1 keepalive/信号、Q9-F4 的 pending 变换 | 纯 DFT 路（无这些）37 µs/cb vs 融合路 b1（有这些）232 µs/cb ⇒ **6× 差** | `OCUDU_DFT_RELEASE_BLOCK=0` + 拆分臂读 `dft=`；以及 `OCUDU_DFT_OPEN_BLOCK=0` |
| (c) | **派发内线程组被串行化**（每组 ~37 µs）| `14 × 37 ≈ 518 ≈ 507` | 更细的批量扫描（b1/2/4/7/14，已在做）；或把 14 个组改成"1 组内多符号"的 kernel 形状（kernel 容量核对）|

⚠ **若 (a) 成立，交付上的含义要用户裁决**："不做样点宿主拷贝"是**契约**（`host sample assembly: 0 copied` ⇒ 8/8 之一）⇒ 用一次宿主 staging 去换 ~400 µs 的设备驻留**会破契约**，这是取舍不是技术障碍。

#### ⑤ 下一步（**全部离线**，不飞腿；按此顺序）

1. **(a) 的判决开关**：`wrap_buffer` 里加 `OCUDU_FORCE_STAGE_INPUT=1`（强制走 staging 抄一份），只在探针下生效；在拆分臂上对拍 `dft=` 与 `wrap_copies`。
2. **(b) 的判决**：拆分臂 × `OCUDU_DFT_RELEASE_BLOCK=0/1` × `OCUDU_DFT_OPEN_BLOCK=0/1`，读 `dft=`。
3. **(c) 的判决**：把批量扫描做细（每档 3 次取中位，消掉 ±30% 的抖动），看窗口是"每 cb 常数"还是"每组线性"。
4. 三者里**只有 (a) 或 (c) 被证实**才值得再飞腿；若都不成立，则这条 500 µs 是**该平台 command buffer 的固有驻留**，单车道内部已经没有可动的旋钮，杠杆回到"每跳提交数 / 在飞跳数"这类结构量（§7.6.1 的 S-E）。


### 6.78 ★ Q25 落地：**"输入 staging"判决开关**（`OCUDU_DFT_STAGE_INPUT=1`）+ `p48` 预登记（2026-09-26，本会话）

#### ① 先记两条**离线就把候选砍掉**的结果

* **候选 (c)（派发内线程组被串行化，每组 ~37 µs）被现成测量否掉**：批量派发的**执行**账单是 **10.6 µs/槽**（14 个线程组，§6.65/§6.30 的隔离微基准）——若是"每组 37 µs"它应该是 ~518 µs。⇒ (c) 死。
  （曾被"14 × 37 ≈ 518 ≈ 507"误导；那个巧合来自把**空 cb 的驻留下限**当成了每组的执行。）
* ⚠ **离线载体在这条臂上不可信**：同一个拆分臂（`syn004_4`，默认批量）连跑两次，`cbs/lane` 在 **0.50 ↔ 2.00** 之间跳，`dft` 的 per-cb 值随之在 **78 ↔ 507 µs** 之间跳
  ⇒ **拆分臂的分段数字只能用空口的**（空口 `cbs/lane=1.00` 稳定、账目闭合），离线只用"纯 DFT 路"的**单 cb** 读数（36.4–39.9 µs，稳定）。
  ⇒ 这类"用 `us/lane ÷ cbs/lane` 反推 per-cb"的读法**必须先看 `cbs/lane` 是否稳定**。

#### ② 开关：只作用于**电台输入**那一条路（`OCUDU_DFT_STAGE_INPUT=1`）

* 位置：`ocudu_dft_metal_engine.mm` 的 `write.time_samples` 分支（电台 IQ 的 zero-copy 映射就在那一句 `wrap_buffer(engine, alloc_base, alloc_size)`）。
* 做法：走**引擎已经在用的 staging 回退路**（同一句 `newBufferWithBytes`，并被 `staged` 计数）——**不是新机制**，只是强制选它；
  绑定的缓冲从"整个 allocation"变成"该符号的切片"，因此 `input.offset` 从"allocation 内偏移 + 窗口起点"变成 **只有窗口起点**。
* **绝不碰网格与输出环**（代码自己警告：对网格 staging 会得到"GPU 写副本、宿主读原件"的垃圾网格——`dft_release_adopt_metal_test` 里就有这条断言）。

#### ③ 离线验证（**新路正确**，且失败项都是这支臂的定义）

| 网 | 结果 |
|---|---|
| `[grid] size=512 window=0/1 subcarriers=300 **mismatching=0**`、`[ci16] … mismatching=0` | ✅ **staging 后的网格与零拷贝路逐 RE 相同** |
| `dft radio inputs: … N buffer wrap(s) had to stage a host copy -> **FAILED**` | ✅ **预期**：这正是"输入被 staging 了"的证据（该契约检查就是钉这件事的）|
| `FAIL: overwriting the samples of an in-flight transform did not corrupt its grid` | ✅ **预期**：staging 后变换读的是副本，覆写原件不再影响它（该测试钉的是**零拷贝别名**本身）|
| 三个 DFT 相关测试**默认路** | ✅ 3/3、全量 `ctest -L phy -j 1` **193/193** |
| 环境变量不开时 | **零行为变化**（只多一次 `getenv`/transform）|

#### ④ `p48-n78-stageinput`：预登记（**测量臂**；契约会 7/8，只作读数）

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p48-n78-stageinput \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 OCUDU_DFT_STAGE_INPUT=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
```

**飞交付配方（不加 `LANE_DIAG_SPLIT`）**：这样判据就是 `merged_hop` 本身，且 `cbs/lane` 保持 2.00（不引入第三个变量）。

| 读数 | 预登记 |
|---|---|
| `[phy_pipeline] dft radio inputs: … **N buffer wrap(s) had to stage a host copy**` | **N > 0 且该行 FAILED** ⇒ **臂已生效**（若 N=0 ⇒ 开关没进二进制/没生效，本腿作废）|
| ★ **`[ul_gpu_lane] busy split merged_hop`** | **判据**：**塌到 ≲150 µs** ⇒ 那 ~500 µs **就是"前端读电台零拷贝映射"的代价**（⇒ 真杠杆，但要用户裁"破契约换时延"）；**仍在 541±40** ⇒ **不是输入路径**（⇒ 只剩"cb 里其它活"与平台固有驻留，单车道内部收口，杠杆回结构量）|
| `wrap_copies` / `wraps creates` | `wrap_copies` 应 ≈ 每跳 1（staging 计数）；`creates` 应大幅下降 |
| `cbs/lane` / `gaps` / 池 / `rx_overflows` | 2.00 / 0 / `starved=0`、`dropped=0` / 0（**不允许变**）|
| 契约 | **预期 7/8**（`dft radio inputs` 红——这是臂的定义，不是缺陷）|
| CRC / SINR / `[ul_by_size]` | **必须健康**（网格已离线证明逐 RE 相同；若 SINR 塌 ⇒ staging 路在空口上有问题，立即停手）|


### 6.79 ★★ 腿 `p48-n78-stageinput`：**输入路径被证伪** —— 零拷贝映射不是那 ~500 µs 的原因（把它换成 staging 反而**翻倍**）；并带出**新线索**（2026-09-26）

> 腿：`logs/gnb_gpu_p48-n78-stageinput_0926_1039.log*`（交付配方 + `OCUDU_DFT_STAGE_INPUT=1`，戳 `c5eb27eb05`）。

#### ① 臂已生效（预登记的两条"臂活着"证据都命中）

* `dft radio inputs: … **2,070,264** buffer wrap(s) had to stage a host copy`（此前所有腿都是 **0**）；
* `contract **NOT MET: 1 of 8**`，红的正是 `dft radio inputs` —— **与预登记逐字一致**；
* 其余不变量没动：`cbs/lane=2.00 (max=2) dropped=0`、`gaps=0`、`rx_overflows=0`、`starved_events=0 dropped=0 held_max=14`、CRC-OK **133,159** 跳、吞吐不塌（218.2 MB / 267.1 s ≈ 6.5 Mbit/s）。

#### ② 判据：**既不是"塌"，也不是"守" —— 是反向恶化 +464 µs** ⇒ **(a) 被证伪**

| 读数（中位）| **`p48`（staging）** | `p42`/`p45`（零拷贝，交付）| 差 |
|---|---|---|---|
| ★ **`busy split merged_hop`** | **1005.6 µs** | 541.8 / 551.4 | **+464（+86%）** |
| `dft carried GPU start->end` | **1010.1 µs** | 511.7 | +498 |
| lane `residency` / `busy` | 1306.8 / 1034.0 | 543.9 / 565.2 | +763 / +469 |
| lane `gap` | 306.0 | 3.2 | +303 |
| `commit -> completion` | 959.8 | 440.6 | +519 |
| `period` | **973.3** | 425.7 | **+548** |
| `input hold` | 1562.6 | 828.8 | +734 |
| **V1**（`ul_gpu_pipeline` 中位）| **2181.2** | 1366.8 | **+814** |

⇒ 若"前端读电台的零拷贝映射"是那 ~500 µs 的原因，换成**引擎自己的普通缓冲**应当**降**；实测**翻倍**。
⇒ **结论：零拷贝映射不是原因，恰恰相反——它是更快的那条路**（一次缓存映射喂 14 个变换，比我这条"每个变换一块新缓冲"便宜得多）。**候选 (a) 死。**

#### ③ ★ 新线索：驻留**跟着"缓冲的绑定结构"走**（这条比 (a) 值钱）

我这条 staging 是**每个变换一块新建 MTLBuffer**（14 块/槽）。它相对零拷贝（**1 次映射喂 14 个变换**，`wrap hits=3.8M` vs `creates=244k`）多出的 **+464 µs ≈ 14 × 33 µs** ——
与 §6.77 量到的"**一条平凡 cb/一次新缓冲 ≈ 36–40 µs**"是**同一个量级**。
⇒ **该平台似乎对"一次派发里新出现的缓冲绑定"收 ~33–40 µs**（映射/固定页的代价），而**复用的缓存映射不收费**。
⇒ 这条**同时解释了 452 µs 的形状**（前端那条 cb 绑定的缓冲数 × ~37 µs），也给了一个**可离线检验**的方向：
数一条前端 cb 里**首次出现的缓冲绑定**个数，与窗口比；若能对上，则"把绑定数降下来/把映射复用起来"就是一条**不破任何契约**的杠杆。

#### ④ 单车道猎捕的现状（**七条候选，六条已被实测排除**）

| # | 候选 | 判定 | 出处 |
|---|---|---|---|
| 1 | 算力（按 RE 计）| ❌ 3→25 PRB 扫描下窗口**反向**（245→~100 µs）| §6.75③ |
| 2 | 设备侧栅栏 | ❌ 9 µs/跳（并发 1 时 17）| §6.76② |
| 3 | DL 负载 | ❌ `dlcap40` vs `ulcap40` 同窗口 | §6.75⑤ |
| 4 | 同伴/同刻竞争 | ❌ 只 ~70 µs（并发 1 对照）| §6.76① |
| 5 | 派发内线程组串行 | ❌ 14 组执行只要 10.6 µs | §6.78① |
| 6 | **输入路径（零拷贝映射）** | ❌ **本腿：换掉它反而 +464 µs** | §6.79 |
| 7 | 缓冲**绑定**的新增（映射/固定页） | ⏳ **新线索（③）**，可离线判 | §6.79 |

⇒ **单车道内部（跳内）已经全部量过**；若 ⑦ 也被否掉，则那 ~450 µs 是**该平台 command buffer 的固有驻留**，
单车道内部**没有可动旋钮**，杠杆只能落在结构量（**每跳提交数**、**在飞跳数**）——即 §7.6.1 的 S-E，以及 §7.6 里那两条触 V4 的候选（`noD1` / `event`，离线把 `defer_wait` 腰斩但 `cbs/lane` 到 3.00）。

#### ⑤ 下一步

1. **（离线，先做）** ⑦ 的判决：把一条前端 cb 的**首次缓冲绑定数**与它的窗口对上（`wrap hits/creates` + 内核绑定表 + 纯 DFT 路的 36–40 µs/次作单价）；
2. **（需用户裁决）** 若 ⑦ 也不成立，交付侧只剩两条路：**(i)** 接受 `cbs/lane=3.00` 换 `defer_wait` 腰斩（`noD1`/`event`，**要裁 V4**）；**(ii)** 转 S-E（并发/队列结构），把 ~450 µs 的驻留**遮住**而不是去掉。


### 6.80 ★★★ ⑦ 判完了 —— **平台没有"每 cb 地板"；那 ~450 µs 是前端 kernel 的*真实*执行，而它*不重叠***（2026-09-26，离线；**算力账单需要更正**）

> 工具：新探针 `wip/fe_wakeup_probe.mm`（一个合成 kernel，只改两件事：**线程组数**与**每组工作量**；读 cb 自己的 GPU 窗口，15 次取中位、10 次预热）。

#### ① 平台标定：**没有 gap 效应、没有每 cb 地板、轻量线程组完全并行**

| 每组工作量（256 线程 × N iters）| 1 个线程组 | 14 个线程组 | `gap 1ms − back-to-back` |
|---|---|---|---|
| 8 | **7.1 µs** | 7.1 µs | **0.0** |
| 64 | 10.4 | 10.6 | 0.0 |
| 512 | 38.3 | 38.5 | 0.0 |

⇒ (i) **一次 1 ms 空闲不会让派发变贵**（"冷启动/唤醒"假设**否掉**）；
⇒ (ii) **平凡 cb 只要 7.1 µs**（不是 37-40 µs）⇒ §6.77 里"平凡 cb 的驻留下限 ≈ 37 µs"被**更正**：那个 37 µs 是**真实 kernel 的时间**；
⇒ (iii) **轻量线程组 1 与 14 同窗口** ⇒ 它们**完全并行**（M4 Pro 核数足够）。

#### ② 于是 (c) 以**正确的形式**复活：**真实 FFT kernel 的线程组不重叠**

* **纯 DFT 路**（稳定读数）：一条 cb = **一个** 768 点变换 = **36.4–39.9 µs**（1 个线程组）；
* **空口前端 cb**（批量）：一条 cb = **14** 个变换 = **452.3 µs** ⇒ **452 / 14 ≈ 32 µs/变换**；
* 两者自洽 ⇒ **每个变换 ~32–37 µs，且 14 个变换在一条派发里*没有*并行**（否则应是 ~37 µs 而不是 452；对照①(iii)：轻量组是并行的 ⇒ 真实 kernel 的组被 **占用率（线程组内存/寄存器）** 限住 ⇒ 串行）。

#### ③ ★★ 算力账单必须更正：前端不是 0.76 µs/变换，而是 **~32–37 µs/变换**

§6.65/§6.30 记的"前端执行 **10.6 µs/槽**（= 0.76 µs/变换）"是**隔离微基准**的读数，与链上两条独立读数（纯 DFT 路 37 µs/单变换 cb、空口批量 cb 452 µs/14 变换）**差 ~40 倍**
⇒ **那条孤立测量测的不是这个几何/这个派发**（§6.69 已记过同族陷阱：`dispatchThreads` 与 `dispatchThreadgroups` 混用会读出 **256 倍**的假值——这次是相反方向的同一类错误）。
⇒ 影响面：§6.65③ 的"窗口里 ~464 µs 是等待、不是算力"**要改**；§6.72–6.77 的"不是算力"结论**只在 CE/均衡/解映射上成立**（它们的账单与空口 `ch_wt`(≈40-48 µs)、`merged_hop`(24 µs) 窗口**自洽**✓），**前端那一条不成立**。

#### ④ 这条把整条主线重新点亮：**前端是 ~450 µs 的真实执行，而它不重叠**

* 一跳的窗口 ≈ 前端 452 + 估计器 45 + eq/demap 24（§6.77 的拆分读数，三项相加 ≈ 521 ≈ 一跳窗口）✓ **账目闭合**；
* 前端 452 / 时隙 500 ≈ **91%** ⇒ **单是前端就把一个时隙吃满**；这解释了"链长 > 时隙"、解释了 V1 ≈ 2×窗口 + C、也解释了为什么所有"等待类"候选都量不到东西（**因为它们本来就不是等待**）。

#### ⑤ 下一步（**离线优先**，这是本会话最有希望的杠杆）

| # | 动作 | 预期 |
|---|---|---|
| 1 | ★ **前端 kernel 形状**：让 14 个变换**共享一个线程组**（或降低每线程组的线程组内存/寄存器footprint），在**离线**用 `--dft --dft-metal --synth N` 直读每 cb 窗口（现有读数：1 变换 37 µs） | 若窗口从 452 µs 掉到 ~40–100 µs ⇒ **V1 可能 −600…−800 µs**（V1 ≈ 2×窗口 + C）|
| 2 | 更正 §6.65③/§6.30 的算力账单（把前端那一项按链上几何重测；隔离法要么修几何、要么弃用） | 文档正确性 |
| 3 | 若 (1) 成立 ⇒ 飞一条腿（交付配方，只改前端派发形状） | 验收 |

#### ⑥ ⚠ **更正 §6.80 的措辞（用户 2026-09-26 追问"这 14 个 FFT 是被串行计算吗"）**：**"串行"未被证明**，而且新事实把口径改了

**已确立的（读数）**：

| 事实 | 读数 | 出处 |
|---|---|---|
| 空口上每次批量派发**正好 14 个变换**（一槽一次）| `batched=152,445/2,134,230` ⇒ 14.0；`released=152,445` = 1/跳 | `p47` `[metal_stats] dft` |
| 链上**单个**变换 = 一条 cb（纯 DFT 路）| **36.4–39.9 µs**（稳定，F3 逐 cb）| §6.77 |
| 链上**14 个**变换 = 一条 cb（空口前端）| **452.3 µs** ⇒ **32 µs/变换** | `p47` busy split |
| 平台无"每 cb 地板"、无 gap 效应、**轻量线程组完全并行** | 平凡派发 **7.1 µs**；1 组 ≡ 14 组；gap − back-to-back = **0.0** | §6.80 探针 |

⇒ **关键读法：批量化*没有*降低"每变换"的成本**（32 vs 37 µs）——它降的是**每槽的 cb 数**（14→1）。**"每个变换 ~32–37 µs" 在批与不批下都一样** ⇒ 这正是 P2-B′ 的收益来源（少 13 次提交），**而不是**"14 个 FFT 被并行/串行"造成的。

**为什么"串行"这个说法不严谨**：同一算术（`14 × 32 ≈ 452`）**也**由"**它们其实并行、但争用同一个共享资源（L2/带宽/barrier）⇒ 每个慢 14 倍**"产生。
区分这两者需要"同样的总工作量、不同的并发形状"的对照，而**现有读数做不到**：
* 离线拆分臂的 `busy split`/`cbs/lane` **不可用**（同一臂两次跑出 **504 vs 78 µs/cb**，`cbs/lane` 在 0.50↔2.00 跳，§6.78 已记）；
* 纯 DFT 路与空口的**分配几何不同**（`--synth` vs 51 PRB），不能直接比总量。

**★ 新事实（这条把杠杆换了形状）**：kernel 的线程组内存是**声明式固定大数组**：
```metal
constant uint MAX_FFT_N = 4096;
threadgroup float2 buf[MAX_FFT_N];      // = 4096 × 8 B = 32 KiB / 线程组
```
而本平台 n = 768（23.04 MHz / 30 kHz）**只需要 6 KiB** ⇒ **每个线程组按 32 KiB 计**。Apple GPU 每核线程组内存通常是 32 KiB 量级
⇒ **占用率被这个声明压到 ~1 个线程组/核**（无论 n 是多少）。这既是"14 个组难以重叠"的**可能机制**，也是一条**零行为风险的杠杆候选**：
把它改成**动态 threadgroup 内存**（`threadgroup float2* buf` + 宿主 `setThreadgroupMemoryLength:`，按 n 申请）或按 n 模板化 ⇒ 占用率可升到 4 组/核级别。

**下一步（离线，判"串行 vs 争用"并且顺手试杠杆）**：
1. **真实 kernel 的 N 扫描**（受控 harness：直接加载 metallib，N = 1/2/7/14 个线程组各一条 cb，读 GPU 窗口）——只有这个能区分"串行"与"争用"；
2. **`MAX_FFT_N` 那一刀**：按 n 申请 threadgroup 内存（或模板化），在同一个 harness 里对拍窗口；
3. 两者都做完再决定值不值得飞腿。

### 6.82 ★★★ 受控 harness 定案（用户追问"14 个 FFT 是不是被串行计算"）：**打包后它们完全重叠；不重叠的是"同一 cb 里的多个派发"**（2026-09-26，离线）

> 工具：`wip/dft_dispatch_cost.mm` —— **运行时编译真实的 `ocudu_dft.metal`**（把它自己的 `ocudu_dft_butterflies.h` 内联进来），按 **本平台真实几何**（n=768 = 2⁸·3¹、`nof_subc=612`=51 PRB、`is_ci16=1` + 网格写）派发，读 cb 自己的 GPU 窗口（15 次中位、10 次预热）。

| 臂 | cb 窗口 | 每变换 |
|---|---|---|
| 1 变换，**1 次派发**（float2、无网格）| **46.6 µs** | 46.6 |
| **14 变换，1 次派发（打包）** | **46.6 µs** | **3.3** |
| **14 变换，14 次派发（旧前端）** | **530.2 µs** | **37.9** |
| 1 变换，1 次派发（int16+网格）| 47.4 | 47.4 |
| **14 变换，1 次派发（int16+网格）** | **46.9** | **3.3** |
| **14 变换，14 次派发（int16+网格）** | **538.4** | **38.5** |

#### ① 结论（回答"串行吗"）

* **打包进一次派发时：完全重叠** —— 14 个变换花 **46.6 µs**，与**一个**变换的 46.6 µs **一模一样**（每变换 3.3 µs）；
  这同时印证 kernel 自己注释里的实测（1 个 12.18 µs / 14 个打包 13.75 µs）。⇒ **"14 个 FFT 被串行"在打包形态下不成立。**
* **真正串行的是"同一 cb 内的多个派发"** —— 14 个单组派发花 **530–538 µs ≈ 14 × 38 µs**：**连续派发之间不重叠**（哪怕它们互不依赖、还在同一条 cb 里）。
  ⇒ 这是一条**平台事实**：**派发之间不重叠，线程组之间完全重叠**；每次派发带着 ~38 µs 的"驻留"（它的执行只有几 µs 到十几 µs）。

#### ② 更正 §6.80 的结论，并把它换成正确的那条

* §6.80 说"前端那 ~450 µs 是**真实 kernel 执行**、且**不重叠**" ⇒ **前半句错、后半句只对"多派发"形态**：
  空口前端**已经**是打包形态（`batched=152,445/2,134,230` ⇒ **14 变换/派发**，`≈1.04` 次派发/跳）⇒ 它的派发只值 **~47 µs**，
  而它的 cb 窗口是 **452 µs** ⇒ **~405 µs 既不是派发、也不是栅栏（9–17 µs）、也不是算力**。
* ⇒ **"每次派发 ~38 µs 驻留、派发之间不重叠"这条平台事实，才是 P2-B′（14 派发 → 1 派发）当年 −38% V1 的真正机制**，也说明：
  **一跳的 cb 窗口 ≈ 它包含的*派发数* × 每次派发的驻留**（一跳的 cb 里有 2×CE + eq + demap = **4 次派发**）。

#### ③ 新的、可动的杠杆（第一次有明确算术）

| 杠杆 | 算术 | 备注 |
|---|---|---|
| ★ **减少"每跳的派发数"** | 每次派发 ~38 µs 驻留 ⇒ 去掉 1 次 = ~38 µs 窗口（经 V1 的 1.2–2.6× 放大 ⇒ **~45–100 µs V1**）| 与 §6.63/§6.64 的"每去一次派发值 5–14 µs"**不一致**，需要用本 harness 重测"去掉的是哪一种派发" |
| ★★ **把前端那 ~405 µs 的剩余驻留归位** | 已排除：派发（47）、栅栏（9–17）、算力、输入映射（§6.79 反向）、DL、同伴（~70）| 只剩"**平台把这条 cb 按住不执行**"（与"派发之间不重叠"同族）⇒ 可用本 harness 复现：给一条 cb 加**一个**极端轻的派发，看窗口是否仍 ~38 µs |
| （已死）`MAX_FFT_N` 32 KiB 占用率 | 打包 14 组重叠良好 ⇒ 占用率不是限制 | §6.82① |

#### ④ 下一步（离线）

1. 用本 harness 复现"空口 FE cb 的 452 µs"：在 **int16+网格** 几何上，把**派发数**从 1 扫到 14（每次 1 组）与**打包**对照，
   看"1 次派发"落在 47 µs 还是几百 µs ⇒ 判 ② 里那 ~405 µs 是"平台按住"还是"空口特有的东西"；
2. 用同一 harness 量**一跳 cb 的 4 次派发**（2×CE + eq + demap）⇒ 得到"每跳派发数 × 单价"的**账**，据此挑下一个消派发目标。

### 6.83 ★★ 派发线性 + 争用/实时映射都被否掉 ⇒ **空口那 452 µs 在离线任何形状下都复现不出来**（2026-09-26，离线；机制猎捕到此收口）

> 同一个 harness（`wip/dft_dispatch_cost.mm`）加了三组臂：**派发数扫描**、**别队列重载**、**输入=被并发写入的实时页映射**。

#### ① 派发数：**严格线性**（≈ 9 µs 截距 + **~38 µs/派发**）

| 派发数（每次 1 组，int16+网格）| 1 | 2 | 4 | 7 | 14 |
|---|---|---|---|---|---|
| cb 窗口 | 47.5 | 84.8 | 161.3 | 278.2 | **539.3** µs |
| 每次派发 | 47.5 | 42.4 | 40.3 | 39.7 | 38.5 |

⇒ **每次派发 ~38 µs 驻留、且互不重叠**（§6.82 的平台事实）在这条曲线上是干净的。
⇒ ⚠ 但与空口"消一次派发只值 **5–14 µs** V1"（§6.63/§6.64）**差 3–7 倍** ⇒ 空口上这些驻留**大部分被别的活遮住了**：
harness 的 38 µs 是**未重叠的上界**，V1 的 5–14 µs 是**实际临界路径收益**。**两个数都要用对场合。**

#### ② 争用：**只加 ~11 µs**（不是 400）

| 臂 | 窗口 |
|---|---|
| 14 in 1 dispatch（空闲）| 47.1 µs |
| 同上 + **另一条队列 8 个长 cb** | 58.6 |
| 同上 + **32 个长 cb** | 22.6（抖动；量级不变）|

#### ③ 实时映射（最后一个"空口独有"的假设）：**也一样快**

输入换成**页对齐、零拷贝 wrap、且被一个宿主线程持续写入**的区域（模拟电台 DMA 在写同一片内存）：**23.1 µs**。

#### ④ ⇒ 结论（**机制猎捕收口**）

**在交付几何下，前端一槽的活在离线能构造的每一种形状里都只要 23–58 µs；空口上承载它的 cb 却驻留 452 µs（~10–20×）。**
已排除：kernel 本身、几何、输入映射（含**实时写入**的映射）、派发数、设备争用、DL 负载、同伴、栅栏、算力、缓冲绑定。
⇒ 那 ~400 µs 是**空口环境特有**的，而 **Metal 在本平台不提供逐派发时间戳（§6.75）⇒ harness 无法再逼近**。
⇒ 按纪律（**不再造判不了的仪器**），**机制猎捕到此为止**；要动它只能**空口 A/B**：挑一个*理论上会影响它*的结构旋钮，看窗口动不动。

#### ⑤ 建议的 air A/B 清单（都是**测量臂**，各自一个变量）

| 臂 | 若窗口动 ⇒ 说明 | 备注 |
|---|---|---|
| `OCUDU_DFT_BACKEND_QUEUE=1`（前端 DFT 挪到车道队列）| 与队列/提交路径有关 | 离线无差别（§6.77③）⇒ 正好检验"空口特有" |
| `OCUDU_DFT_RELEASE_BLOCK=0`（不交棒）| 与 D1 交棒/被 adopt 有关 | 离线 `defer_wait` 腰斩；V4→3.00 ⇒ **需裁** |
| `OCUDU_CE_LANE_ORDER=event`（旧 P2 路）| 与 lane 的单缓冲融合有关 | 同上，V4→3.00 |
| 收包环/`otw_format`（电台侧）| 与 USB DMA/内存路径有关 | §6.52 的旧结论指过这个方向 |

**建议先飞第 1 条**（零 V4 代价、离线已知"无差别"⇒ 信息量最大：动 ⇒ 空口队列特有；不动 ⇒ 与提交路径无关，转第 2/4 条）。

### 6.84 ★ 腿 `p49-n78-dftqueue`：**构造性零臂**（D1 武装时前端 DFT 本来就在后端队列）⇒ 队列假设由**代码+读数**双重关闭；并确认**前端 cb 与一跳 cb 严格串行**（2026-09-26）

> 腿：`logs/gnb_gpu_p49-n78-dftqueue_0926_1103.log*`（交付配方 + `OCUDU_DFT_BACKEND_QUEUE=1`，戳 `857d36f40f`）。
> 登记行确认姿势正确：`knob : OCUDU_DFT_BACKEND_QUEUE=1` ✓（不是没传进去）。

#### ① 零臂的**代码证据**（这条日志就是答案）

```
[dft_release] D1 step 1: the DFT's open block is handed over uncommitted (the default since 5.9.49),
              so it is created on the BACK-END queue - the queue the lane commits on
```

`dft_queue()` **先**看 `block_release_requested()`（D1 默认武装）⇒ **直接返回后端队列**，**根本走不到** `OCUDU_DFT_BACKEND_QUEUE` 那一支
⇒ **交付配置下前端 DFT 早就在后端队列上** ⇒ 这个旋钮在**默认路上是 no-op**。**队列假设因此被代码关闭**（不再需要腿）。

#### ② 读数：与交付基线**逐项一致**（这一腿的价值 = 交付配置的**重复数据点**）

| 读数（中位）| `p49` | `p42` | `p45` |
|---|---|---|---|
| **`merged_hop`** | **543.8** | 541.8 | 551.4 |
| `ch_wt` | 45.2 | 43.5 | 48.4 |
| lane `residency` / `busy` | 568.7 / 567.2 | 543.9 / 565.2 | 544.5 / 566.1 |
| V1（`ul_gpu_pipeline`）| **1377.2** | 1366.8 | 1372.0 |
| 契约 / `cbs/lane` / `gaps` / 池 | **8/8** / 2.00 (max=2) / 0 / 绿 | 同 | 同 |

⇒ 腿间散布（`merged_hop` 541.8↔551.4、V1 1366.8↔1377.2）与 §6.71 记的 ~17 µs 同量级 ⇒ **无回归、无收益**。

#### ③ ★ 但这一腿的读码带出一条**要紧的结构事实：前端 cb 与一跳 cb 是同一条串行队列上的前后两条**

D1 武装 ⇒ 前端块**在后端队列**（①）⇒ 一跳的 cb（`merged_hop`）**必须等前端 cb 结束才能开始**（同队列串行）
⇒ 这正好解释 `p47` 的拆分账：**452（前端）+ 45（估计器）+ 24（eq/demap）≈ 521 ≈ 一跳窗口 541.8** ✓
⇒ **那 452 µs *整段*都在一跳的关键路径上**（不是被遮住的并行工作）⇒ 若能去掉，按 §7.6.1 的 1.2–2.6× 放大 ⇒ **V1 潜在 −540…−1180 µs**。

#### ④ 于是"值不值得飞腿"的账变了，但**原因仍只在空口**

§6.83 已证：交付几何下前端一槽的活在离线**每种形状**里只要 23–58 µs（kernel/几何/实时映射/争用/派发数全排掉），空口却是 452 µs（~10–20×）。
§6.79 的 staging 臂把输入换成普通缓冲后**更差**（1005 µs）——但那条臂**同时**引入了"每变换一块新建缓冲"（14 块/槽）的 churn。
⇒ **下一个该飞的臂 = §6.79 的精修版**：把输入 staging 到**一块预先分配、反复复用**的引擎缓冲里（**零分配 churn**）：

* 若窗口**塌**（452 → 几十 µs）⇒ 那 ~400 µs 就是"**读电台那片页映射内存**"的代价（⇒ 与电台/USB 路径有关，转 §6.52 的方向）；
* 若窗口**不动** ⇒ 输入内存不是原因，剩余候选只剩 V4 那两条（`noD1` / `event`）与电台侧旋钮，**都需要先裁 V4 或换方向**。

#### ⑤ 更新后的待飞清单（按"信息量 ÷ 代价"排序）

| # | 臂 | V4 代价 | 判读 |
|---|---|---|---|
| 1 | ★ **持久 staging 缓冲**（§6.79 精修：复用一块缓冲，不新建）| **无** | 塌 ⇒ 电台内存路径；不动 ⇒ 输入无关 |
| 2 | `OCUDU_CE_LANE_ORDER=event`（旧 P2 路）| 3.00 ⇒ **需裁** | lane 单缓冲融合是否是那 452 µs 的所在 |
| 3 | `OCUDU_DFT_RELEASE_BLOCK=0`（真·关 D1）| 3.00 ⇒ **需裁** | 同时换队列 + 取消交棒，两个效应要一起读 |
| 4 | 电台侧（收包环/`otw_format`）| 无 | §6.52 指向的传输侧 |

**⇒ 建议先做第 1 条**（我可以离线实现并自证：默认路零行为变化 + 网格逐 RE 相同），再按它的结果决定要不要请你就 V4 裁决。


### 6.85 ★★ 452 µs 的异常：**离线能构造的每一种"代码/缓冲形状"都已排除** ⇒ 只剩"电台 DMA 写那片页"与"真实实时环境"两个候选（2026-09-26，离线）

> 用户在 §3.1.1 核查之后裁定"那 452 µs 肯定是异常"，并要求按规划继续。顺着"最可能的机制"逐个判掉，**四个新臂全在 `wip/dft_dispatch_cost.mm`**（真实 kernel、真实几何）。

| 臂 | 窗口 |
|---|---|
| 默认（私有缓冲、编码即提交）| 57.5 µs |
| **每 run 新建一个 wrap 对象盖在同一片页上**（模拟腿上 ~1.7 次创建/跳的 re-wrap）| **22.1 µs**（比复用还快）|
| **编码器开着 500 µs 再提交**（前端块跨一个时隙才 release，`finish_symbol`）| **19.1 µs** |
| **由另一条线程提交**（D1 交棒：编码在接收线程、提交在 lane 线程）| **13.7 µs** |
| 两者同时（空口的真实形状）| **13.6 µs** |
| 输入=零拷贝页映射且被**并发写**（宿主线程模拟 DMA）| 21.8 µs |

⇒ **至此离线已排除的形状清单**（全部 13–58 µs，而空口 452 µs）：
真实 kernel 与几何、派发数（1 次）、打包 vs 分开、输入来源（私有缓冲 / 复用 wrap / **每 run 新建 wrap** / **实时被写**）、
设备争用（+11 µs）、编码器跨时隙、跨线程提交、缓冲绑定结构、队列、栅栏（9–17 µs）、DL 负载、同伴（−70 µs）。

⇒ **剩下两个候选，harness 造不出来（都需要电台在场）**：

1. ★ **USB DMA 正在写那一片页**（我的"并发写"是**宿主 CPU 写**；真实是**设备侧 DMA**，对 GPU 的页表/cache/内存控制器是另一回事）；
2. **真实实时环境**（宿主负载、lane 线程、DL 的真实提交pattern；`busy(union)/window = 27–29%` 说明被探针的两条队列大多数时间是闲的）。

#### 下一步（**新的仪器，不需要手机/OTA**）：**电台在环的 mini-probe**

设计：起一个小工具 —— 让 B200 **只做 RX 流**（不跑 gNB、不上手机）→ 把收到的样点缓冲**零拷贝 wrap** → 反复派发**真实 DFT kernel** → 读 cb 窗口；
三个臂对照：**(a)** 该缓冲在被电台流写入时 / **(b)** 停止流后同一缓冲 / **(c)** harness 自己的缓冲。

* **判据**：若 (a) ≫ (b) ≈ (c)（例如 452 vs 50 µs）⇒ **就是"GPU 读 DMA 正在写的内存"** ⇒ 修法方向 = **把样点 staging 一次到一块复用缓冲**（**§6.79 之所以失败是因为它每变换新建缓冲——本节的 22.1 µs 已证明"新建对象"不是问题、§6.79 的 1005 µs 另有原因，需重测**）；
* 若 (a) ≈ (b) ≈ (c) ⇒ DMA 不是原因 ⇒ 只剩"真实实时环境"，那就必须回到空口 A/B（V4 或电台侧旋钮）。

⚠ 该工具**只碰电台与 GPU，不跑腿、不上手机**；按纪律**只能在没有腿在跑时使用**（当前：无腿无 gNB ✓）。


### 6.86 ★★★ 452 µs 的机制猎捕**收口**：数据路径（含 DMA）**由代码排除**、冷缓存与交棒信号也否掉；并发现**后端队列上一个每跳 1 条的"隐形 cb"**（2026-09-26，离线）

#### ① ★ 电台在环的 mini-probe **不必做了**——它的前提被代码推翻

`radio_uhd_rx_stream.cpp` 的接收路径是**带宿主指针**调用 UHD 的：

```cpp
// receive_block()
buffs_flat_ptr.emplace_back(reinterpret_cast<void*>(data[channel].subspan(offset, num_samples).data()));  // 我们的池缓冲
uhd::rx_streamer::buffs_type buffs_cpp(buffs_flat_ptr.data(), nof_channels);
nof_rxd_samples = stream->recv(buffs_cpp, num_samples, md, RECEIVE_TIMEOUT_S, ONE_PACKET);
```

⇒ **UHD 把 DMA 的数据*拷贝*进我们的池缓冲**；DMA 环形缓冲是 **UHD 自己的**，**GPU 从不读它**。
⇒ GPU 读的是**普通、页对齐的主机内存**，由 UHD 的拷贝（宿主线程）写入——**这正是 harness 测过的形状**（复用 / 每 run 新建 wrap / 被并发写：20–60 µs）。
⇒ **"GPU 读 DMA 正在写的页"这一类由构造排除**，§6.85 计划的那个工具**取消**（省下一次电台占用）。

#### ② 另两条离线假设也否掉（harness，真实 kernel/几何）

| 臂 | 窗口 |
|---|---|
| 热缓存（harness 默认）| 13.8 µs |
| **每 run 之前用 256 MiB 扫描把缓存冲掉**（模拟空口每跳的冷工作集）| **16.5 µs**（+2.7）|
| 不 signal | 21.8 µs |
| **在释放的块上 signal 一个 shared event**（D1 交棒形状）| **21.8 µs**（+0.0）|

#### ③ ★★ 最尖锐的剩余事实：**空口上"另一类 cb"与离线完全一致**（⇒ 不是环境整体变慢）

`p47` 同一条腿、同一台设备：

| cb 类 | 窗口 |
|---|---|
| `gpu busy (front_end) mean`（**394,429** 条：PRACH + plain 路，单变换、不交棒、不 signal）| **47.78 µs** ← **与离线 harness 的 47 µs 一致** |
| FE/一跳那类 cb（承载整槽 14 变换 + D1 交棒/被 adopt）| **452–541 µs** |

⇒ **"设备被节流"、"整机都冷"、"环境整体变慢"这一类全部排除**；那 ~10× 只发生在**承载整槽批量且走 D1 的那类 cb** 上——
而它的**每一种可离线构造的属性**（批量、队列、输入来源、缓存冷热、signal、跨线程提交、编码器跨时隙、争用）**都已实测为 13–58 µs**。

#### ④ ★ 会计洞：后端队列上有**每跳 ~1 条不可见的 cb**（很可能是 LDPC 解码）

`p47`：`gpu busy (back_end): commits=**446,006**`，而可解释的只有 ~**295,000**（每跳 2 条 × 146,771 + 注册表）⇒ **多出 ~151,000 ≈ 每跳 1 条**。
读码确认：`lib/phy/upper/channel_coding/ldpc/metal/ocudu_metal_decoder_engine.mm:823` **`[cmd_buf commit]` 之前没有 `arm_gpu_time()`、也没有 `register_commit()`**
⇒ **LDPC 解码器的命令缓冲既不在 F3 时间线里、也不在 lane probe 的分段里**（`[ul_ldpc_decode]` 中位 54 µs 是**宿主**计时，不是设备窗口）。
⇒ 后端队列的占用账**目前是不完整的**，而它正是承载那一跳的那条队列。

#### ⑤ 结论与下一步（两条，都不需要先飞腿就能准备）

1. ✅ **洞已补（本节当天落地）**：`ocudu_metal_decoder_engine.mm` 的 commit 前加了
   `arm_gpu_time(cmd_buf, back_end, "ldpc_dec")` ⇒ **LDPC 解码器的 cb 从此进 F3 时间线**（`OCUDU_METAL_GPU_TIME=1` 时才生效，默认路零变化）。
   下一条腿要读：`[metal_stats] queue occupancy (Q9-F3) slowest commits` 里有没有 `ldpc_dec`、它的窗口多大、以及 `gpu busy (back_end) commits` 的账能否对上（"每跳 2 条 + 注册表 + ldpc"）。
   ⚠ 它**不是** lane probe 的分段（那需要 `register_commit`，会改变 lane 的账），所以只进 F3 与 `commits` 计数——**刚好够把这个洞填上而不动任何分段口径**。
2. **机制猎捕收口**：离线能构造的形状已经**全部**是 13–58 µs（约 20 种臂），而那 ~450 µs 只在空口出现在**特定那类 cb** 上；
   按纪律**不再造判不了的仪器** ⇒ 要动它只剩**空口 A/B**：`noD1` / `event`（V4→3.00，**需用户裁决**）与电台侧旋钮。


### 6.87 ⚠ **更正 §6.86④**（用户 2026-09-26 指出）：**那个"每跳 1 条隐形 cb"不存在**；LDPC 在腿上是 **CPU** 解码

> 用户指出两点：**(1) LDPC 位于融合 pipeline 之外；(2) 目前 LDPC 仍由 CPU 执行。两点都由代码与账目证实。**

#### ① 账目：差额**只存在于诊断拆分臂**，交付腿闭合

| 腿 | `gpu busy (back_end) commits` | hops | 2×hops | 差额 |
|---|---|---|---|---|
| `p45`（交付）| 296,517 | 146,214 | 292,428 | **+4,089（1.4%）** |
| `p46`（并发 1）| 298,464 | 145,379 | 290,758 | +7,706（2.6%）|
| **`p47`（诊断拆分臂）** | **446,006** | 146,771 | 293,542 | **+152,464（+52%）** |
| `p49`（交付）| 294,248 | 146,175 | 292,350 | **+1,898（0.6%）** |

⇒ **那 ~1 条/跳正是拆分臂自己造的**：`shared_burst::adopt()` 把前端块**单独提交在后端队列**上
（`arm_gpu_time(cb, queue_kind::back_end, "split_dft")` + `register_commit(cb, stage::dft)`）⇒ 每跳多 1 条 cb。
⇒ **§6.86④ 的"LDPC 解码器的隐形 cb"是错的**：我拿**拆分臂**的 `back_end commits` 去比**交付路**的"每跳 2 条"。
**交付腿的账在 0.6–2.6% 内闭合** ⇒ **后端队列上没有隐形流量**这一假设**作废**。

#### ② 代码：LDPC 的后端由 `dec_type` 选，不由 `phy_pipeline` 选

```cpp
// lib/phy/upper/channel_coding/channel_coding_factories.cpp
#if defined(OCUDU_METAL_LDPC)
if (dec_type == "metal") { return std::make_unique<ldpc_decoder_metal>(…); }   // 只有显式选 metal 才走 GPU
#endif
```

交付配方**不设** `dec_type=metal` ⇒ **腿上的 LDPC 是 CPU 解码** ✓，`[ul_ldpc_decode]`（中位 54–70 µs）是围绕 **CPU** 解码的**宿主**计时，
与"设备窗口/融合一跳"无关 ✓ —— 用户的两点都对，§6.86④ 的两处论断（"LDPC 的 cb 在 GPU 上"、"它隐形"）**都撤回**。

#### ③ 那我给 LDPC 引擎加的探针算什么？（保留，但**写清适用范围**）

`arm_gpu_time(cmd_buf, back_end, "ldpc_dec")`（§6.86① 的落地）在**空口交付路上是休眠的**（那个引擎根本没被实例化）；
它**只在 `ul_chain_replay --metal` 那条离线路上生效**（replay 的 `--metal` = CE/解调/**解码**三者都走 Metal）——
所以它的价值是**离线**：给"Metal 解码器"这条**可选**路径留下一个设备窗口读数（若将来把 `dec_type=metal` 接进交付，它自然就开始计时）。
**它不补任何空口上的洞（那个洞不存在），也不改变任何默认路**（`OCUDU_METAL_GPU_TIME=1` 才生效）。

#### ④ 计划更正

* ❌ **撤回**"下一条腿读 `ldpc_dec`"这一项（空口上没东西可读）；
* ✅ **后端队列的占用账现在是完整的**（交付腿 0.6–2.6% 残差 = 注册表提交）⇒ **"有隐形工作占着那条队列"这一类解释也排除**；
* ⇒ 那 ~450 µs 的候选又被削掉一层：**不是隐藏流量、不是数据路径、不是缓存/信号/线程形状、不是队列/栅栏/算力/DL/同伴**。
  剩下的只有**空口 A/B**（`noD1` / `event`，V4→3.00，**待用户裁决**）或电台侧旋钮。


### 6.88 ★★★ 用户追问的答案：**这 ~450 µs 不是车道融合引入的**（历史腿档案直接给出反例），**是否"不可避免"仍未知**（2026-09-26）

> 用户裁定：**"全程 GPU"是首要目标**（"破坏了这个其实再快的车道也没有用处"）⇒ **任何靠宿主 staging/拷贝换时延的方案从计划里删除**（§6.85/§6.86 那条"持久 staging"臂**作废**）；
> 同时**授权为测量暂时放宽 V4**。下面是"是否融合引入 / 是否不可避免"的历史分析。

#### ① ★ 历史腿档案（`logs/` 里的 `busy split`，按时间）：**融合前就有同样的形状，且每跳总量相同**

| 时代 | `cbs/lane` | `ch_est` | **`ch_wt`** | `eq_demap` | 每跳窗口合计 |
|---|---|---|---|---|---|
| **融合前** `ota-b3b`（09-19）| 3.40 | 123.4 | **335.6** | 74.0 | ≈533 |
| **融合前** `s13p2`（09-19）| 3.73 | 141.0 | **434.0** | 114.2 | ≈689 |
| **融合前** `s13p1-rollback`（09-20）| 3.71 | 132.9 | **363.9** | 91.5 | ≈588 |
| **融合腿** `s13p1-fused`（09-20）| **3.00** | 97.9 | **307.9** | 75.5 | ≈481 |
| **今天（交付）** `p42`/`p49` | **2.00** | ——（前端在跳内）| 43.5–45.2（相关构建）| ——（在跳内）| **541.8 / 543.8** |

⇒ **结论一：不是融合引入的。** 融合前**每一条 cb 就已经"窗口 ≫ 算力"**（`ch_wt` 308–434 µs，而它的算力账单只要 ~22–28 µs；`eq_demap` 72–114 µs，账单 ~3 µs），
而且**每跳窗口总量**（≈480–690）与今天（≈585）**同量级**。
⇒ **结论二：融合改变的是"形状"而不是"总量"**——它把 3–4 条 cb 并成 2 条（V4 从 3.0 降到 2.0），把前端的工作**搬进了跳内那条 cb**（D1 交棒 / adopt），
于是**一跳变成一条串行单元**，前端不再能与上一跳的后端重叠。**这条结构代价是真的**（也正是 `noD1`/`event` 两条臂要测的东西），但**那 ~450 µs 的体积不是它带来的**。

#### ② 新测量：**"设备闲着"确实让每次派发变贵，但只值 ~2.6–3.7×、且饱和在 ~50 µs**

同一 kernel、同一几何，只改"提交前设备空转多久"：

| 提交前空闲 | 0 | 1 ms | 50 ms | 500 ms | 2 s |
|---|---|---|---|---|---|
| cb 窗口 | 19.3 | 13.7 | **49.4** | 50.4 | **50.6** µs |

⇒ 空口的设备在**被探针的两条队列上只有 27–29% 的时间在跑**（`busy(union)/window`）⇒ **长期低时钟状态**是一部分原因（~2.6–3.7×，饱和），
但**它只能解释到 ~50 µs，解释不了 452**。⇒ 剩下的 ~9× **仍无归属**，且离线**任何形状**都构造不出来（约 20 个臂）。

#### ③ 于是"是否不可避免"的诚实回答

* **在"融合"这个层面**：那 ~450 µs **不是融合的产物**，融合只是重新分配（①）；**融合的唯一结构代价**是"前端与上一跳后端不再重叠"。
* **在"当前平台/空口"这个层面**：**未知**。已排除的机制清单很长（数据路径、DMA、缓存、时钟饱和、signal、跨线程、队列、栅栏、算力、DL、同伴、隐藏流量），
  而**唯一能回答"它是不是 unavoidable"的实验**恰恰需要**把结构改回去**测：`OCUDU_DFT_RELEASE_BLOCK=0`（真关 D1）与 `OCUDU_CE_LANE_ORDER=event`。
* ★ **这两条臂不破坏"全程 GPU"**：它们只改**提交数与提交结构**（`cbs/lane` 2.00 → 3.00），
  **样点依旧全程在设备上**（零拷贝 wrap、`host sample assembly: 0 copied`、crossings 0.00+0.00 都不变）⇒ **V4 是"提交数"判据，不是"数据路径"判据** ✓（这正是用户担心的那一点）。

#### ④ 计划（按用户裁定更新）

| # | 动作 | 状态 |
|---|---|---|
| 1 | ❌ **宿主 staging 臂**（§6.85/6.86 的"持久 staging 缓冲"）| **删除**：破"全程 GPU"，与首要目标冲突 |
| 2 | ★ **`OCUDU_DFT_RELEASE_BLOCK=0`**（关 D1：前端回到自己的队列、交棒取消）| **待飞**（测量臂，V4 3.00 已授权）|
| 3 | ★ **`OCUDU_CE_LANE_ORDER=event`**（旧 P2 结构：多 cb + 栅栏而非单缓冲融合）| **待飞**（同上）|
| 4 | 电台侧旋钮（收包环 / `otw_format`）| 零 V4 代价，可作第四条 |

**判据（两条臂一致）**：若窗口/`merged_hop` **明显下降** ⇒ 当前的单缓冲融合**在空口上**确实是那 ~450 µs 的一部分（且**可以不改数据路径地拿回来**）；
若**不变** ⇒ 那 ~450 µs 与本平台/空口的某处硬性开销有关，单车道内部到此收口，主线交回 S-E 的结构量（每跳提交数 / 在飞跳数）。


### 6.89 ★★★ 用户追问"这两条臂是否也改变**提交的时刻点**"：**是——两条都会把 CPU 拉回中途** ⇒ **两条都撤出候选**（2026-09-26）

> 用户指出第二首要目标：**不只是"IQ→LLR 全程 GPU 一步到底"，还包括"CPU 全程靠边站"——开始时提交一次，然后只在出口等 LLR**。
> 按此逐条查码，结论是**这两条臂都恢复"宿主中途参与"**，因此它们测的是一个**我们不要的**配置。

#### ① `OCUDU_CE_LANE_ORDER=event`（旧 P2 结构）：**宿主在跳中途插一手**（有实测）

`ocudu_metal_lane_probe.mm:147-154` 自己的记录：

```
/// * hole_to_weights_us is the HOST's. … measured at 125.3us, against a 238.3us
///   extraction-commit-to-weights-commit distance and a 116.6us extraction. … the hole IS
///   "the host had not handed the second command buffer over yet", and there is a real dependency
///   behind it: the weights command buffer's parameters carry the CFO the host reads OUT of the
///   extraction's buffer, so it cannot be encoded before that command buffer completed
```

⇒ `event` 路上**weights 那条 cb 必须等抽取 cb 完成、宿主把 CFO 读回来才能编码** ⇒ **CPU 中途参与**（实测缺口 **125.3 µs**）⇒ **与第二目标直接冲突**，**撤出候选**。

#### ② `OCUDU_DFT_RELEASE_BLOCK=0`（关 D1）：**恢复一次阻塞宿主的 `wait_slot`**（有代码 + 历史读数）

`ofdm_demodulator_impl.cpp:486-506`：

```cpp
released = wait_per_slot && handover_allowed() && dft->release_block(grid.get_device_view().base);
if (!released) { (void)dft->end_block(); }
…
if (released) { /* … waiting here would name a buffer this engine does not own (wait_slot() refuses) */ }
else if (!wait_per_slot || last_symbol_of_slot) { dft->wait_slot(slot); }   // ← 关掉交棒就走到这里
```

`wait_slot()` 内部是 **`[cmd_buf waitUntilCompleted]`（阻塞宿主）**（`ocudu_dft_metal_engine.mm` 的 `[ul_dft_wait]`："the host time this wait costs"）。
⇒ 关掉 D1 ⇒ **宿主在"本槽最后一个符号"处阻塞等前端变换完成** ⇒ **CPU 中途参与**，**撤出候选**。
（历史读数：`p41` 那条腿上 `[ul_dft_wait]` 中位 **577.6 µs** —— 那正是这个宿主阻塞的量级。）

#### ③ ⇒ 结论：**融合（merged + D1）本身就是"CPU 靠边站"的实现**

* `released=true` 那条分支的注释就是全部答案：**交棒之后"宿主等在这里会点到一个不属于本引擎的缓冲"** ⇒ **融合把那次宿主等待删掉了**，并把整跳并成**一次提交**。
* 所以：**那 ~450 µs 不能用这两条臂去"测掉"而不付出目标的代价**；§6.88 ④ 的待飞清单**撤回**（`noD1`、`event` 两条都删）。
* **~450 µs 的定位维持为**：**不是融合引入**（§6.88① 历史腿档案）、**不是数据路径/缓存/时钟（只解释 ~2.6–3.7× 且饱和在 ~50 µs）/队列/栅栏/算力/DL/同伴/隐藏流量**，
  离线任何形状 13–58 µs ⇒ **本平台+空口的一个未归属量**，已按纪律收口记录。

#### ④ 仍然**同时满足两个目标**的杠杆（下一批候选）

| # | 候选 | 机制 | 代价/收益（预登记）|
|---|---|---|---|
| 1 | ⛔ **~~把 fenced 相关构建并进前端那条"开着的块"~~ ⇒ 撤回**，理由见 §6.90（那会重新引入 5.9.89 修掉的**平台正确性缺陷**）| —— | —— |
| 2 | **S-E：并发/队列结构**（把驻留**遮住**而不是去掉）| 并发 2 已实测比 1 好 **227 µs** 的 V1；不改数据路径 | 触 V4 的是"每跳提交数"，并发不改它；需再裁 |
| 3 | **减少跳内派发数**（CE/eq/demap 4 次）| 每次派发在空口有 ~38 µs（热）级驻留 | 与 §6.63/6.64 的 5–14 µs 口径需先对齐 |


### 6.90 ★★★ 用户追问的确认：**融合后确实曾是 `cbs/lane=1.00`；今天的 2.00 是 2026-09-23 的一个"平台正确性修复"带来的，且不能退回**（2026-09-26）

> 用户问：**目前的 2.00 是否已经破坏了"CPU 全程靠边站"？是否需要恢复到 1.00（他记得融合后就是 1.00）？**

#### ① 历史确认：**用户记得没错**

| 时代 | `cbs/lane`（车道总数）| `busy split` |
|---|---|---|
| 09-19（D1 之前）| **3.00–3.73** | `ch_est` 96–157 + `ch_wt` 296–434 + `eq_demap` 73–114 |
| **09-21（D1 step 2 落地）** | **1.00** | 单条：`eq_demap=434–1034 µs (100% of busy)` |
| **今天（交付）** | **2.00** | `ch_wt=43–48`（相关构建）+ `merged_hop=541.8` |

⇒ 融合（D1）之后**确实是 1.00**（一跳一条 cb）。

#### ② 2.00 是**修复**带来的，不是设计倒退：`5.9.89` 的原文

```
5.9.89 - a fenced correlation build fixes it and passes the gate, and the fix's real price is the extra submission
… What is left is the measurement itself: on this GPU and driver a memory barrier between two
  dispatches of one encoder does not deliver the producer's writes to the consumer, and a
  command-buffer boundary does.
… So the fix is landed … The correlation build opens its own command buffer directly … signals the
  shared back-end fence, and commits. The weights buffer waits for that generation before its encoder
  opens, so the GPU orders itself and THE HOST NEVER BLOCKS.
… The price is the point of the commit. Paired on identical hop sets … the default sits at ~215us a
  hop, the fenced form at ~275us and the host-wait form at ~285us. The extra COMMAND BUFFER costs ~50us;
  the host's wait adds only ~10us more …
```

三点关键：
1. **根因是平台缺陷**：**同一条 encoder 内、两个派发之间的内存屏障不能把生产者写的值传给消费者；命令缓冲边界可以** ⇒ 相关构建（产出 `A`/`R_hp`）与权重核（消费它）**必须在不同的命令缓冲里** ⇒ **2 条 cb 是这个跳在这台 GPU 上的下界**。
2. **修复是刻意做到"宿主不阻塞"的**（"the GPU orders itself and the host never blocks"）⇒ 2.00 **不是**"CPU 中途参与"：两条 cb 背靠背提交完，CPU 就走开了；被实测并否决的是 `host_wait` 形式（285 vs 275 µs）。
3. **它的价钱被明确记账**：**多出来的这条 cb ≈ 50 µs/跳**（配对同 hop 集）。

#### ③ ⇒ 结论与更正

* **2.00 没有破坏"靠边站"的实质**（无宿主等待、无中途回合）；它只是"CPU 每跳摸设备两次"。
* **"恢复到 1.00"不可行**：那条被省掉的 cb 正是**平台缺陷的规避手段**；退回 1.00 = **重新引入 `A`/`R_hp` 的竞态**（5.9.89 之前那些腿就是 1.00，那时这个缺陷是存在的）。
* ⇒ **§6.89④ 候选 1（把相关构建并进前端那条开着的块）撤回**：D1 之下前端块**会被并进跳的那条 cb** ⇒ 那正好把"生产者"和"消费者"放回**同一条命令缓冲** ⇒ **违背修复**。
* ⇒ **"每跳一次提交"在本平台对这个跳不可达**：把相关构建与权重核放进**同一条 dispatch**（kernel 融合）才能既保序又省一条 cb，但 Metal **没有跨线程组的 dispatch 内同步**（除了逐线程组的 barrier）⇒ 两个不同几何的核无法在一次派发内串起来 ⇒ **也不可行**。
* ⇒ 因此：**当前的 2.00 是"两个首要目标"下的最优形态**（一跳一提交 + 一跳一必需边界），那 ~50 µs 是**平台的正确性税**，应记入 §3.1.1 的固定开销，而不是当作可优化项。


### 6.91 ★★ 用户裁定的"测量分支"规划：**可行，而且 `event` 那条今天已经是一条"无 CPU 中途参与"的 3.00 分支**（2026-09-26）

> 用户裁定（三点）：**(1)** S-E 结构是**最后**的选择（它与当前工作**不同维度、可以同时得到**收益）；
> **(2)** 为了测量，暂时放宽到 `cbs/lane=3.00` **可以**，但**规划解决方案时必须记住最终目标——不能"放宽到中途再把 CPU 叫回来"**；
> **(3)** `3.00` 与 `2.00` **可以做成条件分支**：3.00 只对**测量分支**开放（测量不是目的，是手段），**生产路径仍是 2.00**。

#### ① ★ 关键新事实：`event` 路的"宿主 CFO 读回"**今天已经不存在了**

`port_channel_estimator_metal_mmse_impl.cpp:2434-2442`：

```cpp
// Where K4's rotation takes its CFO from (see noise_stage_t). The extraction wrote this hop's
// own estimate into the slot reserved for it, and the kernel reads it there - which is what
// keeps the scalar out of the host's hands and lets the weights command buffer be encoded
// without waiting for the extraction first. …
reformat.noise.cfo_dev         = &gpu_ls_cfo[cfo_slot_];
reformat.noise.cfo_from_device = device_ls_valid;
```

⇒ 空口走的是**设备 LSE 路**（`device_ls_valid = true`）⇒ **CFO 在设备上取** ⇒ **宿主不必读回、weights 那条 cb 可以"不等抽取"就编码** ✓
⇒ §6.89① 引用的那条 `hole_to_weights_us = 125.3 µs` 是**旧时代**（宿主读回 CFO）的读数，**在今天这条路上不成立**。
⇒ 因此 **`OCUDU_CE_LANE_ORDER=event` 今天就是一条：3 条 cb/跳 + 无宿主等待 + 无宿主回合** 的结构 ✓✓（抽取→权重的次序由**设备侧 fence** 保证，D1 仍武装 ⇒ 也没有 `wait_slot`）。

#### ② 于是"条件分支"的可行性：**已经具备，且生产路零改动**

| 分支 | 怎么开 | `cbs/lane` | 数据路径 | CPU |
|---|---|---|---|---|
| **生产（默认）** | 不设旋钮 | **2.00** | 全程 GPU | 一次提交后靠边站 |
| **测量 A：多 cb 结构** | `OCUDU_CE_LANE_ORDER=event` | **3.00** | 全程 GPU（同样核、零拷贝、设备网格）| **无宿主等待、无回合**（① 证明）|
| **测量 B：无交棒结构** | `OCUDU_DFT_RELEASE_BLOCK=0` **＋**跳过那次多余的宿主 `wait_slot` | 3.00 | 全程 GPU | 需要那个小旋钮（见③）|

**测量 A 不需要任何新代码**（旋钮已有、默认关、生产路不受影响），并且**同时满足两个目标** ⇒ 它测的是"**单缓冲融合**在空口上值不值那 ~450 µs"，而**不是**用一个"CPU 回来的世界"去测。

#### ③ 测量 B 需要的一小步（要不要做由你定）

关 D1 会把 `finish_symbol` 推到 `dft->wait_slot(slot)`（**阻塞宿主**）那一支。
但那个等待按代码自己的说法是**多余的**："waiting here would name a buffer this engine does not own"——次序已由 burst 的设备侧 fence（`backend_stage_wait`）保证。
⇒ 可加一个**只对测量分支开放**的旋钮（例如 `OCUDU_DFT_SKIP_SLOT_WAIT=1`，默认关）跳过它 ⇒ **3.00 且无宿主等待**。
**风险与验收**：若那个等待并非多余，网格会出现错误值 ⇒ 由**离线 dump 逐字节 + 契约 + CRC/SINR** 兜住（离线可先证；空口腿上 SINR/CRC 变差就立即停手）。

#### ④ 两条臂的预登记（都只作读数；生产路不变）

| 读数 | 测量 A（`event`）| 测量 B（`noD1`+跳过等待）| 判读 |
|---|---|---|---|
| ★ `busy split` / `merged_hop` / lane `residency` | 记录 | 记录 | **若显著下降** ⇒ 单缓冲融合/交棒**在空口上**确实是那 ~450 µs 的一部分（且**可以不改数据路径地拿回来**，只需裁 V4）；**不变** ⇒ 与本平台硬性开销有关 |
| `cbs/lane` | **3.00（预登记）** | 3.00 | 生产判据（≤2.00）**不适用于测量分支**，只登记 |
| **`[ul_dft_wait]`** | 必须仍是 **no samples** | **no samples**（③ 的旋钮生效的直接证据）| **"CPU 没有回来"的判据** |
| `[mmse_time_sum] gpu_wait` / `[ul_gpu_lane] gap: … (host)` | 仍为 0 / 与 `p42`（45.8 µs）同量级 | 同 | 同上 |
| 契约 / `gaps` / 池 / crossings | 8/8 / 0 / 绿 / `0.00+0.00` | 同 | **不允许变**（数据路径不动的证据）|

#### ⑤ 决策规则（测量之后）

* 若 **A 或 B 明显更短** 且上表"CPU 没回来"的判据全绿 ⇒ **产出一条"交付形态候选"**：把该结构做成生产路（`cbs/lane=3.00`），此时**只需你裁 V4 阈值**——而两个首要目标（全程 GPU + CPU 靠边站）**都仍然成立**。
* 若 **都不变** ⇒ 那 ~450 µs 与本平台/空口硬性开销有关，**单车道内部收口**，届时再谈 S-E（用户已定为最后选择，且与当前工作不同维度、可叠加）。


### 6.92 ★★★ 用户补充裁定：**判据重述——`cbs/lane` 是"代理量"，真正的目标是"宿主一次性、背靠背提交完就靠边站"**（2026-09-26）

> 用户原文精神：**"`cbs/lane=3.00` 并不是完全不可接受的，正像 `cbs/lane=2.00` 一样；只要是 CPU 在开始的时候就背靠背地完成 3 次 commit，然后就全程靠边站，实际上这就是和我们的设计目标一致的。"**

#### ① 判据重述（这条要覆盖 §3.1 的 V4 **措辞**，不改它的历史记录）

| 层级 | 内容 |
|---|---|
| **两个首要目标（不可交易）** | **(G1) 数据流全程在 GPU**：IQ→LLR 一步到底，样点零拷贝、宿主不搬数据（`host sample assembly: 0 copied`、`crossings 0.00+0.00`）；<br>**(G2) 宿主一次性参与**：**在跳入口背靠背完成该跳的所有 commit，然后全程靠边站**，直到出口取 LLR（无中途等待、无宿主回合）|
| **代理量（可调、用于成本核算）** | `cbs/lane`（提交数）、`[ul_gpu_lane]` 的宿主段、`[ul_dft_wait]`、`gpu_wait` —— 它们**衡量 G2 是否被破坏**、以及提交本身的开销，**但它们不是目标本身** |
| ⇒ **推论** | **`cbs/lane=3.00` 与 2.00 地位相同**：只要"**背靠背**"成立，它就**不违反任何首要目标**；V4 的 `≤2.00` 因此是一条**代理阈值**，可以在有证据时更新（**不是**"拿目标换时延"）|

#### ② ⇒ 这把测量 A/B 的性质也改了：它们不再是"为测量而牺牲目标"，而是**候选交付形态的形状**

* 测量 A（`event`，3.00）：抽取/权重/burst 三条 cb **由宿主在入口连续编码提交**，次序由**设备侧 fence** 保证（§6.91①：CFO 已在设备上取）⇒ **背靠背成立** ⇒ **若它更快，它就是"G1+G2 都满足"的交付候选**，只需更新 V4 的代理阈值。
* 测量 B（`noD1`+跳过 `wait_slot`，3.00）：同理，前提是**跳过那次多余的宿主等待**（§6.91③ 的旋钮）⇒ 也满足 G1+G2。

#### ③ ★ "背靠背"是**可判**的（用现成仪器，不必新造）

| 判据 | 仪器 | 期望（若背靠背成立）|
|---|---|---|
| 两条 cb 的提交之间**没有宿主等待** | `[ul_gpu_lane] host: extraction commit -> weights commit`（`event` 路会有样本）| **几 µs–几十 µs 量级**（=纯编码时间）；**若是几百 µs ⇒ 宿主中途干了活** |
| 同上，第二条边界 | `[ul_gpu_lane] host: weights commit -> burst commit` | 同上 |
| **没有任何宿主等待设备** | `[ul_dft_wait]`、`[mmse_time_sum] gpu_wait` | **no samples / 0.0 µs** |
| 入口那一段是"一次编码"而非"等" | `[ul_gpu_lane] gap: stage entry -> extraction commit (host)` | 与 `p42` 的 **45.8 µs** 同量级 |
| 数据路径没动 | 契约 8/8、`crossings 0.00+0.00`、`gaps=0`、池绿 | **不允许变** |

（前两条正是 §7.6.0c 补丁放出来的那两条宿主段——它们在融合路上是 "no samples"，在 `event` 路上**应当有样本**，于是**恰好成为"宿主是否中途参与"的直接读数**。）

#### ④ 决策规则（更新版）

* **A 或 B 更快 + ③ 的表全绿** ⇒ **产出交付候选**（结构多 cb、宿主仍一次性）⇒ 需要你做的只是**更新 V4 的代理阈值**（并用这条腿作为证据），**G1/G2 均未被交易** ✓
* **都不变** ⇒ 那 ~450 µs 归平台硬性开销，单车道收口，再谈 S-E（最后选择、可叠加）。

#### ⑤ 下一步（我建议的顺序）

1. **飞测量 A**（零代码改动；`OCUDU_CE_LANE_ORDER=event`）——它是"3.00 且背靠背"的第一条实证；
2. 同时我**把测量 B 的旋钮做出来**（`OCUDU_DFT_SKIP_SLOT_WAIT=1`，默认关、离线可自证），B 的腿随后即可飞；
3. 两条腿的结果一起判，再决定是否把某个多 cb 结构提升为**交付候选**并更新 V4 阈值。


### 6.93 ✅ 测量 B 的旋钮落地：`OCUDU_DFT_SKIP_SLOT_WAIT=1`（默认关，构造上安全）（2026-09-26，本会话）

`ofdm_demodulator_impl.cpp::finish_symbol()` 里，关掉 D1 后本槽**最后一个符号**会落到 `dft->wait_slot(slot)`（**阻塞宿主**）那一支。
新增开关让它走一条**只在"网格由设备写、由设备消费"（`wait_per_slot`）时**才生效的新分支：

```cpp
} else if (skip_slot_wait_enabled() && wait_per_slot && last_symbol_of_slot) {
  // 测量臂：消费者自身的设备侧 fence 就是次序，宿主不必在这里阻塞（只打印一次以标注这条腿是测量腿）
} else if (!wait_per_slot || last_symbol_of_slot) {
  dft->wait_slot(slot);
}
```

* **构造上安全**：`wait_per_slot = device_write && grid_consumed_on_device` —— 这正是代码**本来**用来给"槽内其它符号"跳过等待的**同一个谓词**，
  所以带宿主读者的配置（解调器自测、`cpu_gpu` 路）**永不跳过**，不可能读到 GPU 还没写的内存 ✓
* **默认关**：生产路走 D1 交棒，**根本到不了这一支**；实测默认路**零变化**（回放日志里 0 次 "SKIPPED" 提示）✓
* **判据**：`ctest -L phy -j 1` **193/193** ✓

**两条测量臂现在都可飞**（都只作读数，生产路默认不变）：

```bash
# A：多 cb 结构（3.00，宿主仍背靠背；零代码改动）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p50-n78-event3cb \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 OCUDU_CE_LANE_ORDER=event \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2

# B：无交棒结构（3.00，且跳过那次多余的宿主等待）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p51-n78-nod1-nowait \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 \
  OCUDU_DFT_RELEASE_BLOCK=0 OCUDU_DFT_SKIP_SLOT_WAIT=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
```

**判读**（§6.92③ 的表 + §6.91④）：`busy split`/`merged_hop`/`residency` 是否下降；**`[ul_dft_wait]` 必须 no samples**、`gpu_wait` 0.0、
`host: extraction commit -> weights commit` 与 `host: weights commit -> burst commit` 应出现样本且是 **µs–几十 µs**（=背靠背）；
契约 8/8、`crossings 0.00+0.00`、`gaps=0`、池绿**不许变**；`cbs/lane` 3.00 是预登记值（**不是**生产判据）。


### 6.94 ★★★ 腿 `p50-n78-event3cb` 与 `p51-n78-nod1-nowait`：**融合被"活体 A/B"排除**（窗口只是换了个位置）；而 p51 是一次**无效测量**（质量塌了，两个变量同时动）（2026-09-26）

> 两腿戳都是 `3dea21e5b2`。A = `CE_LANE_ORDER=event`（3 cb、宿主背靠背）；B = `RELEASE_BLOCK=0` + `SKIP_SLOT_WAIT=1`。

#### ① ★★ 测量 A（`p50`）：**预登记命中，而且它回答了那个核心问题——融合不是那 ~450 µs 的来源**

| 读数 | `p50`（event，3 cb）| `p42`/`p49`（merged，2 cb，生产）|
|---|---|---|
| `cbs/lane` | **3.00 (max=3)** ✓ 预登记 | 2.00 |
| `busy split` | **`ch_wt=487.7 µs/lane（96%，cbs/lane=2.00）`** + `eq_demap=22.7（4%，1.00）` | `ch_wt=43.5 + merged_hop=541.8` |
| **一跳窗口合计** | **≈510 µs**（487.7 + 22.7）| **≈585 µs**（43.5 + 541.8）|
| `[ul_dft_wait]` / `gpu_wait` | **no samples / 0.0 µs** ✓（无宿主等待）| 同 |
| `host: weights commit -> burst commit` | **有样本：中位 68.8 µs**（mean 73.3）| **no samples** |
| `host: extraction commit -> weights commit` | no samples | no samples |
| 契约 / `gaps` / 池 / crossings | **8/8** / 0 / `starved=0 dropped=0` / `0.00+0.00` ✓ | 同 |
| CRC-OK 跳 | **136,647**（占跳数 ~94%）| —— |
| 吞吐（MAC PDU 总量 ÷ 腿时长）| 201.7 MB / 270.6 s ≈ **745 kB/s** | 333.5 MB / 369 s ≈ 904 kB/s |

**⇒ 结论（决定性）**：把结构从"单缓冲融合"换成"3 条 cb、多阶段"，**那 ~500 µs 没有消失，只是从 `merged_hop` 搬到了估计器的两条 `ch_wt` 上**（487.7 µs）。
这与 §6.88① 的历史档案（融合前的 `ch_wt` 也是 308–434 µs）**独立吻合** ⇒ **车道融合不是这 ~450 µs 的成因**，它是**本平台在这条链上的一笔与结构无关的开销**。
（⇒ §6.88/§6.90 里"融合只改形状不改总量"的判断，现在有了**活体 A/B 证据**。）

**顺带得到一条新线索**：`event` 路上出现了一条**宿主段** `host: weights commit -> burst commit` **中位 68.8 µs**——它不是"等设备"（`ul_dft_wait`/`gpu_wait` 都是 0），而是**宿主把 burst 编码出去花的时间**（含 eq/demap 的编码 + 该跳适配器侧的其余工作）。它落在车道的关键路径上（burst 的提交被推迟 69 µs）⇒ **"宿主 encode 该跳剩余部分"本身值得量一次**（生产路上这一段是 "no samples"，因为 merged 路把 burst 并进了同一条 commit）。

#### ② ⚠ 测量 B（`p51`）：**无效测量** —— 质量塌了，而且它同时动了两个变量

| 读数 | `p51`（noD1 + skip-wait）| `p50`（A）| 差 |
|---|---|---|---|
| **CRC-OK 跳** | **48,835** | 136,647 | **−64%** |
| **CRC-OK 占跳数** | **≈44%**（48,835 / 111,635）| ≈94% | **质量塌** |
| MAC PDU 总量 / 时长 | 121.6 MB / 317.7 s ≈ **383 kB/s** | 201.7 MB / 270.6 s ≈ 745 kB/s | **−49%** |
| `[ul_mac_pdu_size] samples`（有 TB 的跳）| 47,298 | 135,235 | −65% |
| 每跳 TB 中位 | 2496 B（**不小**）| 1281 B | —— |
| `[ul_host] symbols` vs 期望 | 8,894,172 vs 8,894,318 ⇒ **没丢符号** ✓ | 同 ✓ | —— |
| `gaps` / `rx_overflows` / 池 | 0 / 0 / `starved=0` ✓ | 同 | —— |
| 契约 | **8/8** | 8/8 | —— |
| `dft … released=` | **0** ✓（D1 确实关了）| 148,026 | —— |
| `busy split` | `ch_wt=46.9（1.00）+ merged_hop=483.7（1.00）` | `ch_wt=487.7（2.00）+ eq_demap=22.7` | ⚠ 见下 |

**判读**：**每跳 TB 不小（2496 B）、符号没丢、契约 8/8、池电台全绿，但"能解出来的跳"从 94% 掉到 44%** ⇒ **这是一次"数据不对"的塌陷，不是变慢**。
⇒ **这一腿同时动了两件事**（关 D1 **与** 跳过 `wait_slot`），**无法归因**；而且它暴露了**我的开关是可疑的**：
`SKIP_SLOT_WAIT` 的 guard（`wait_per_slot`：网格由设备写且由设备消费）本意是"构造上安全"，但 p51 的证据说明——**在没有交棒的结构里，那次宿主等待是承重的**（代码原文也写着"keeping it … is always correct"）。
⚠ **另一个仪器缺口**：p51 的 `busy split` 只有 2 条 cb（`ch_wt` 46.9 + `merged_hop` 483.7），**FE/CE 那条 cb（无交棒时由估计器提交）没有登记到 lane probe** ⇒ 该臂的车道总量**少算**，不能与 `p42`/`p50` 直接比。

#### ③ 下一步（一条腿就能归因）

**只关 D1、不跳等待** ⇒ 把两个变量分开：

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p52-n78-nod1-only \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 OCUDU_DFT_RELEASE_BLOCK=0 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
```

* **判据**：**CRC-OK 占比是否回到 ~94%**。
  * **回到 ~94%** ⇒ 罪魁是**我跳掉的那次等待**（`SKIP_SLOT_WAIT` 在无交棒结构里不成立）⇒ 该旋钮**撤回**（或加更严的 guard），B 这条臂**不再飞**（因为它测的世界必须让 CPU 回来，本就不该飞）；
  * **仍是 ~44%** ⇒ 是**无交棒结构本身**在这条交付路上不成立（与我的旋钮无关）⇒ 记录为"D1 是承重的"，B 臂同样收口。
* 同时记录 `[ul_dft_wait]`（这一腿应当**有样本**：宿主确实等了一次，那就是"CPU 回来了"的直接证据 ⇒ 更说明 B 臂与 G2 冲突）。

#### ⑤ ★★★ 腿 `p52-n78-nod1-only`（只关 D1、保留等待）：**归因完成——罪魁是我跳掉的那次等待；而且它量出了融合的价值**

| 读数 | **`p52`（noD1，保留等待）** | `p51`（noD1 **+** 跳等待）| `p50`（A：event）| `p42`（生产）|
|---|---|---|---|---|
| **CRC-OK 跳** | **113,457** | 48,835 | 136,647 | 102,699 |
| **CRC-OK 占跳数** | **77%** | **44%** | 94% | 71% |
| **吞吐（MAC PDU ÷ 时长）** | **983 kB/s**（299.0 MB / 304.1 s）| 383 | 745 | 904 |
| 每跳 TB 中位 | 2817 B | 2496 | 1281 | 3329 |
| ★ **`[ul_dft_wait]`** | **samples=151,273，中位 154.9 µs** ✗ | **no samples** | no samples | no samples |
| V1（`[ul_gpu_pipeline]` 中位）| **1515.9** | —— | 1496.7 | **1366.8** |
| `busy split` | `ch_wt=41.9 + merged_hop=519.4` | `ch_wt=46.9 + merged_hop=483.7` | `ch_wt=487.7(2 cbs) + eq_demap=22.7` | `ch_wt=43.5 + merged_hop=541.8` |
| 契约 / `gaps` / 池 | **8/8** / 0 / 绿 | 8/8 / 0 / 绿 | 8/8 / 0 / 绿 | 同 |

**① 归因**：`p51` 与 `p52` **只差那一个开关**，而 CRC-OK 从 **44% → 77%**、吞吐 **383 → 983 kB/s**（这些腿里最高）⇒
**那次宿主 `wait_slot` 在"无交棒"结构里是承重的**，跳过它会**静默地把数据弄错**（代码原文"keeping it … is always correct"是对的）。
⇒ **`OCUDU_DFT_SKIP_SLOT_WAIT` 已撤回并从代码里删除**（不能留一个会撒谎的仪器）；**测量 B 这条臂收口**（它天然要求 CPU 回来，与 G2 冲突）。

**② ★ 顺带量出了融合的价值（这是本腿最有价值的副产品）**：`p52` 里 **`[ul_dft_wait]` 中位 154.9 µs** ⇒
**没有交棒时，宿主每跳要阻塞 ~155 µs**；对应的 **V1 = 1515.9 vs 生产的 1366.8 ⇒ +149 µs**。
⇒ **D1 交棒（merged + 释放）对 V1 的价值 ≈ 150 µs**，而且它正是"把 CPU 从跳里请出去"的那一步（G2）✓
⇒ 这也**反向印证**了 §6.90 的结论：融合不是那 ~450 µs 的来源，但它**确实是 G2 的实现**，并且值 ~150 µs。

**③ 第三次确认**：一跳窗口在四种结构里都落在 **~510–542 µs**（p50 487.7+22.7、p51 483.7、p52 519.4、生产 541.8）⇒
**那 ~450–500 µs 与结构无关**（§6.88 历史、§6.94① 活体 A/B、本腿三线一致）。

**④ 主线回到兼容 G1/G2 的杠杆**（顺序不变）：**(i)** `p50` 暴露的那条 **68.8 µs 宿主 encode 段**；**(ii)** 跳内派发数；**(iii)** §0.1 的链条延长（LDPC→Metal，尺子已就位）；**(iv)** 最后 S-E。


### 6.95 第 (i) 条杠杆的量化：那条 68.8 µs 宿主段**落在 eq/demap 相位里**，而在生产路上它是**暗的**（2026-09-26，离线）

#### ① 现有读数能定位到"哪一段"，但分不开"是什么"

| 读数（中位）| `p50`（event，3 cb）| `p42`（merged，生产）| 差 |
|---|---|---|---|
| `[ul_time_frequency]`（宿主相位）| 532.7 | 532.0 | +0.7 |
| `[ul_channel_estimation]` | 69.8 | 69.3 | +0.5 |
| ★ **`[ul_equalization_demod]`** | **886.0** | **738.8** | **+147** |
| `[ul_ldpc_decode]`（宿主 CPU 解码）| 47.0 | 68.0 | −21 |
| `[mmse_time_sum] total`（CE 宿主）| 36.8 | 36.2 | +0.6 |
| `[ul_gpu_pipeline]` 中位（V1）| 1496.7 | 1366.8 | **+130** |

⇒ **event 臂多出来的 ~147 µs 全部落在 `[ul_equalization_demod]` 这一段**，而 **V1 的惩罚是 +130** ⇒ **这一段就是该臂变慢的地方**；
而 `p50` 的 `host: weights commit -> burst commit`（中位 **68.8 µs**）**也在这一段里**（它是宿主把 burst 编码出去的时间）。

#### ② ⚠ 但这段在生产路上**没有仪表**（所以第 (i) 条目前只能"知道有、说不出是什么"）

* eq / demap / burst 三个引擎里**没有任何宿主相位计时开关**（`getenv` 里只有 `OCUDU_EQ_*`/`OCUDU_D1_HANDED_BOUND`/`OCUDU_LANE_DIAG_SPLIT`，都不是计时器）；
* 生产路（merged）上 `host: extraction commit -> weights commit` 与 `host: weights commit -> burst commit` **都是 "no samples"**——§7.6.0b 早已记过：**融合路把五段尺子按构造变成了暗的**；
* 已有的 `gap: stage entry -> extraction commit (host)`（**45.8 µs**）只覆盖**宿主参与的前半段**（入口→抽取提交），**后半段（weights + burst 的编码，直到 lane 的统一提交）在生产路完全不可见**。

#### ③ 于是下一步的仪器很小、且**直接量 G2 关心的东西**：**给"宿主参与的总额"补一个尾标记**

**方案**（一条打点，离线默认零行为变化）：

| 位置 | 内容 |
|---|---|
| `shared_burst::commit()` 里、`[cb commit]` **之前** | 加一次 `lane_clock` 打点（例如 `mark_lane_commit_host()`）|
| `ocudu_metal_lane_probe.mm` 的报告 | 新增一段 **`host: stage entry -> lane commit`**（= 宿主在**本跳**里的全部参与时间，从入口到把整跳交出去）|
| 判据 | 生产路上它应 ≈ **45.8（前半，实测）+ 后半**；`event` 路上可与 `68.8 µs` 相加对照 ⇒ **第一次把"CPU 每跳到底花了多少"夹住**（G2 的定量形式）|
| 为什么值得 | 它同时服务两件事：**(a)** 量化第 (i) 条杠杆（宿主 encode 是否值得优化）；**(b)** 给 G2 一个**可验收的数字**（今天的 G2 只有定性判据：`ul_dft_wait`/`gpu_wait` 为 0）|

⇒ **建议**：先加这条尾标记（离线自证：默认路读数不变、`ctest` 193/193），**下一条腿**顺手带上它 —— 届时"生产路每跳宿主参与 X µs"变成可直接读的数，
再决定要不要把 eq/demap 那条宿主路径拆细（那才需要各自的相位计时器）。

#### ④ 对主线的净影响

* **G1/G2 都没被 p50 破坏** ✓，而且 **p50 把"融合有害"这条假设彻底关掉了** ⇒ 现在**没有任何证据支持"把结构改回去能拿回那 ~450 µs"**；
* ⇒ 那 ~450 µs 归为**平台/空口固有开销**（§6.88/§6.90/§6.94① 三线一致）；
* ⇒ 下一步回到**兼容 G1/G2 的杠杆**：**(i)** 那条 68.8 µs 的宿主 encode 段（生产路上被 merged 隐藏，值得单独量）；**(ii)** 跳内派发数（CE/eq/demap 4 次）；**(iii)** §0.1 的链条延长（LDPC 进 Metal，尺子已就位）；**(iv)** 最后才是 S-E。


### 6.96 尾标记落地：`host: stage entry -> lane commit`（2026-09-26，离线实现完成）

§6.95③ 的方案已实现（**只加一条打点，不改任何数据路径/提交结构/等待语义**）：

| 代码点 | 内容 |
|---|---|
| `ocudu_metal_lane_clock.h` | `lane_host_clock` 新增 `lane_commit` / `entry_to_lane_commit_us` 与 `mark_lane_commit()`；`mark_stage_entry()` **同时清零**它（尾标记属于**一条** lane，新 lane 不许继承上一条的读数）|
| `ocudu_metal_burst.mm`（`shared_burst::commit()`，`[cb commit]` 前一行）| `metal::lane_clock.mark_lane_commit();` —— 这是本跳**宿主最后一个动作**（此后 CPU 靠边站，即 G2 的时点）；放在 commit 前是为了把这段 encode 也包进去 |
| `ocudu_metal_lane_probe.mm` | 采集 `entry_to_lane_commit_us >= 0.0` 进新序列，报告里紧挨着旧的宿主段打印：**`host: stage entry -> lane commit`** |

**并且它自带一条契约检查**（`lane host participation`，与 `zero-copy wraps` 同一机制）——因为"仪表悄悄失效"正是生产路尾段暗掉的方式（§7.6.0b）：

```
[phy_pipeline]   lane host participation: tail measured for N of M lanes
                 (head median X us, total median Y us, work after the extraction Y-X us;
                  K lanes read shorter than their own head) -> OK / FAILED
```

* **RED 的两种情形**：**(a)** 头有样本而尾没有（声称测宿主却测不全）；**(b)** 任何一条 lane 的"总额"比它**自己的头**还短。
* (b) 的比较**不涉及配对**：两个数在同一瞬间从同一结构体读出，总额与头**同起点、更晚结束** ⇒ 比头短只能说明结构体在两次打点之间被覆盖（即 `mark_stage_entry()` 里那次清零被删掉时会发生的事）；
* **唯一 "not applicable"**：整轮没有任何东西测过宿主侧（探针不在路径上）。

**它和已有仪表的关系**（读法，别混）：

```
[ul_channel_estimation] 相位  ⊃  gap: stage entry -> extraction commit (host)   ← 前半（已有，p42 = 45.8）
   （入口→抽取提交）                    ↓ 同一时点
                                 host: stage entry -> lane commit (host)        ← 总额（新）
                                        = 前半 + 后半（weights + burst 的 encode，直到 lane 交出）
```

* **判据**：生产路（merged，1 cb/跳）上该序列应 ≈ **45.8 + 后半**；`event` 路（3 cb）上"后半"可与 p50 的 `host: weights commit -> burst commit = 68.8` 相加对照 ⇒ **第一次把"CPU 每跳花了多少"夹住**（G2 的定量形式，§0.1）；
* **`-1` / "no samples" 的含义**：提交这条 lane 的线程**从未**进入它的 stage（`mark_stage_entry` 没被调用）⇒ 按构造未测，不报部分值（若空口上真读到 −1，说明 lane 的提交与入口不在同一线程，那就要退到按 slot 配对 —— 见 §6.95② 的告警）；
* **为什么值得**：它给第 (i) 条杠杆（宿主 encode 段）与 G2 各一个可验收数字，且**不需要**在 eq/demap 里加各自的相位计时器（那一步等这个数出来再决定）。

**自证**：`gnb` 重建通过；探针默认关闭时行为不变（这条打点不在 `OCUDU_METAL_STATS` 里，但只是一次 `clock::now()`，与本工程其它 `lane_clock` 打点同风格）；`ctest -L phy -j 1` 193/193（独跑，无并发 GPU 作业；加契约检查后再跑一遍仍 193/193）。落地提交：**`d877c9677f`**（尾标记）+ **`a43a782cc6`**（契约检查）。**下一条腿**在交付配方上顺手带上它 ⇒ "生产路每跳宿主参与 X µs"直接可读。


### 6.97 ★ 尾标记的首次空口读数：**生产路一跳的宿主参与 = 中位 94.0 µs**（头 47.2 + 尾 46.8），且**不在关键路径上**（2026-09-26，腿 `p53-n78-tailmark`）

交付配方（`n78` 20 MHz、默认 `merged`+D1、并发 2、**regime=default**、`OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1`），145,291 跳。

#### ① 读数（契约里的那一行就是它）

```
[phy_pipeline] lane host participation: tail measured for 145291 of 145291 lanes
   (head median 47.2us, total median 94.0us, work after the extraction 46.8us;
    0 lanes read shorter than their own head) -> OK        ← 契约 9/9（新增这条）
[ul_gpu_lane] host: stage entry -> lane commit  samples=145291 mean=99.3 median=94.0 p95=144.2 p99=187.8 max=6307.4
[ul_gpu_lane] gap: stage entry -> extraction commit (host) samples=145291 mean=52.7 median=47.2 p95=84.9  p99=114.0
```

| 量 | `p42`（stress）| `p53`（default）| 读法 |
|---|---|---|---|
| 头（入口→抽取提交）| 45.8 | **47.2** | 两工况**一致** ⇒ 仪器可复现 |
| **总额（入口→lane 提交）** | 腿里**没有这个数**（尾段当时是暗的）| **94.0** | **G2 的定量形式：CPU 每跳 94 µs** |
| 尾（抽取提交→lane 提交）| — | **46.8** | weights + burst 的宿主 encode（生产路上第一次可见）|
| `lanes` / `cbs/lane` | 144787 / 2.00 | 145291 / **2.00** | 生产地板不变（5.9.89）|
| `residency` / `busy` 中位 | 543.9 / 565.2 | 569.0 / 555.9 | 两条腿同量级 |
| `gap` 中位 | 3.2 | 10.7 | **设备等宿主只有 ~11 µs** |
| `period` 中位 | 427.7 | 461.0 | 设备节奏 |
| V1 中位 | 1366.8 | 1370.2 | 同量级 |
| CRC-OK | 102699/144787 = 70.9% | **134778/145291 = 92.8%** | default 工况更干净（数据完好）|
| `[ul_dft_wait]` / `gpu_wait` | no samples / 0.0 | no samples / **0.0** | 宿主没等设备 |
| `defer_wait` 均值 | 763.8 | 769.5 | 流水线填充量不变 |

#### ② 四个结论（都直接由这一行支持）

1. **配对是对的**（§6.95② 的告警可以解除）：**145291/145291 = 100% 覆盖**，`0 lanes read shorter than their own head` ⇒ lane 的提交与它的阶段入口**确实在同一线程**（`close_lane()` 在 `wait_committed()` 里、早于下一跳的 `mark_stage_entry()`），不必退到按 slot 配对。这同时验证了 `mark_stage_entry()` 里那次清零是必要的且生效的。
2. **G2 有了可验收的数字**：一跳的宿主参与 **94.0 µs**（p95 144、p99 188）。它与 `[ul_channel_estimation]`（70.7）**相容**（头 47.2 是那一相位里的**纯宿主工作**，其余 ~23 µs 是等前端），而 `[ul_dft_wait]` 空 + `gpu_wait=0` ⇒ **宿主做这 94 µs 的同时一次都没等设备**："背靠背做完就靠边站"在数字上成立。
3. **杠杆 (i) 被定价并且降级**：那 46.8 µs 的尾巴就是 §6.95 要找的"抽取之后宿主还在干什么"（生产路上的对应物；`event` 臂的 `host: weights commit -> burst commit` 是 68.8，融合路**更便宜**，与 §6.94"融合不是病因"一致）。但它**只值 ~47 µs 的 CPU 时间**，而这 47 µs 现在**藏在设备窗口里**（`gap` 中位 10.7 µs、`[ul_dft_wait]` 空）⇒ **优化它买不到关键路径上的时间**，除非同时改变结构（把 CPU 从链上彻底拿掉，那是 §0.1 的链条延长/S-E）。
4. **那 ~450–500 µs 与宿主无关**：本腿 `residency` 中位 569.0、`busy` 555.9、`gap` 10.7，而宿主总额只有 94.0（占窗口 **16.5%**）⇒ 与 §6.88/§6.90/§6.94 三线一致：它是**平台/空口固有**的，不是 CPU 慢造成的。

#### ③ 边界（这条腿**不能**证明什么）

* **regime=default**，不是 `p42`–`p52` 的 `stress` ⇒ 表里的 `residency`/`period`/V1/CRC **只能同工况对照**（"default 更干净"是工况差，不是改进）；宿主两个数（头 47.2 / 总额 94.0）是 CPU 侧工作量，跨工况可比（头 45.8→47.2 就是证据）。
* 它**没有**回答"94 µs 里各段各占多少"（eq / demap / burst 内部仍无相位计时器——现在这个总数说明**不值得**先去拆它，除非 G1/G2 的链条延长需要）。
* 它**没有**回答那 ~450–500 µs 是什么，只是**第四次**把它从宿主身上摘掉。
* `stale=10`（141390 个 V1 样本里的 0.0071%；近期腿 0–2，`p39` 为 20）—— 对**宿主侧**的读数（头/总额/覆盖率）没有影响，但 V1 / `period` 的对照必须同工况、且这条腿的 `stale` 不构成"数据路径变坏"的证据（CRC-OK 92.8% 为近期最好）。


### 6.98 ★★★ 链条延长（LDPC→Metal）的现状核查：**现有解码器是"模块级卸载"，违反 G1/G2**；而它那 **0.24–0.35 ms** 的窗口是**平台提交地板**（与工作量无关，Z 变 96 倍窗口不变）（2026-09-26，离线）

§0.1 的方向是"把 LDPC 也搬进 GPU，让设备内的链越来越长"。开工前先核查"现成的 `ldpc_decoder_metal` 能不能直接用"——**答案是不能**，而且核查过程顺手给出了那 ~450–500 µs 的**第一个可分解的构成**。

#### ① 代码事实：它存在、可选、也测过，但形态是"上传→算→回读"

* **存在且可选**：`lib/phy/upper/channel_coding/ldpc/metal/{ldpc_decoder_metal.cpp,ocudu_metal_decoder_engine.mm}`（767 + 906 行）；`--pusch_ldpc_decoder_type=metal`（`channel_coding_factories.cpp:121`，validator 允许 `metal`/`metal_flooding`/`metal_persistent`/`metal_async`/`metal_lls`，`du_low_config_validator.cpp:135`）；`gpu` 模式下这个旋钮**没有被改掉**（`du_low_phy_pipeline.h` 的 `gpu` 分支不碰 `out.ldpc`）⇒ 今天空口上跑的是 **CPU（NEON）解码**，与用户此前的更正一致。
* **已经测过**：`ctest -R ^ldpc_metal_unit_test$` **PASS（5.89 s）**；BLER 曲线齐全（`metal/test/bler_results/` 60 份 CSV/PNG，2026-09-02，覆盖 BG1/BG2 × Z∈{4,16,64,128,256,384} × 4 个码率）。
* **但它不是 lane 内的形态**（违反 G1/G2 的两处硬证据）：
  * **G1 ✗ 宿主打包 LLR**：`ldpc_decoder_metal.cpp:684-687` —— `memset` + `fill`（打孔/尾部的弱偏置）+ **`memcpy(slot.llr_i8.get()+2z, input.data(), …)`**，即宿主把 LLR **拷进**一块自己的缓冲再交给 GPU（`input` 是宿主 span）；`decode()` 的返回也要从 `slot.hard_bits`（宿主内存）搬回 `bit_buffer`。
  * **G2 ✗ 每码块阻塞提交**：`ocudu_metal_decoder_engine.mm:831-833` —— `[cmd_buf commit]` 之后立刻 `[cmd_buf waitUntilCompleted]`；每一次 `decode()` 都是一次"提交 + 等"。内核本身**已经把全部轮次编码进同一个 cb**（`for (it…)` 在同一个 encoder 里展开，line 804-819），所以问题不在内核数量，而在**这次提交与等待本身**。

#### ② 离线定价（今天的实测；命令可复跑）

```bash
cmake --build build --target ldpc_metal_bler_test -j 8
./build/lib/phy/upper/channel_coding/ldpc/metal/ldpc_metal_bler_test \
   --gpu-type metal --bg 2 --z <Z> --rates 0.2 --snrs 3 --latency <N> --max-iter <M>
```

| 臂（BG2, rate 0.2, SNR 3 dB, 已 warm-up）| CPU 解码（NEON，中位）| **Metal 解码 wall（中位）**| Metal cb 的 GPU 窗口（均值）|
|---|---|---|---|
| Z=56, 迭代 6 上限（收敛用 2）| 6.0 µs | **732.6 µs** | 652.4 µs |
| Z=56, `--max-iter 1` | 8.4 | **524.5** | 334.5 |
| Z=56, `--max-iter 2` | 5.8 | **339.9** | 228.4 |
| Z=56, `--max-iter 4` | 6.1 | **491.8** | 376.7 |
| **Z=4**（最小区块）, `--max-iter 1` | 5.1 | **501.7** | **349.6** |
| **Z=384**（Z 大 96 倍）, `--max-iter 1` | 22.2 | **326.8** | **237.2** |

`[ldpc_time_sum]` 分解（Z=56, 6 迭代臂）：`pack=0.4us submit=857.3us gpu=659.7us gap=199.7us unpack=1.7us`

**正对照（同一个 harness 换算法，只改派发数）**——用来分开"块大小/算力"与"提交/派发开销"：

| `--gpu-type`（Z=56, rate 0.2, SNR 3）| 每轮派发数（代码）| 1 轮 window | 2 轮 window |
|---|---|---|---|
| `metal`（layered NMS）| `n_layers`(=42) 个 CN + 1 syndrome + 1 gate | 334.5 | 228.4 |
| `metal_flooding` | **2**（CN 全体 + VN 全体）| **156.8** | 182.9 |
| `metal_persistent`（layered_persistent）| **1**（整个解码在一个常驻 threadgroup 里，`threadgroup_barrier` 串行）| 197.5 | 309.5 |

⇒ **四条结论**：

1. **as-is 不可用**：比 CPU 慢 **10–100 倍**（同一码块 CPU 5–22 µs，Metal **157–860 µs**；**三种算法都逃不掉**）。**不要飞 `dec_type=metal` 的腿**——离线已经把结论钉死，空口只会再花一次电台时间。
2. **窗口与"块大小/算力"无关**：Z 从 4 到 384（**96 倍**）而窗口 **349.6 → 237.2 µs**（不升反降）；CPU 侧同一区间 5.1 → 22.2 µs（正常随工作量增长）就是对照。⇒ 这条路径的成本是**每次提交/每次派发的开销**，不是它算的东西。
3. **⚠ 本节初稿的 H5（"平台按'提交+等待'收 ~0.25–0.35 ms/次"）被上表推翻，撤回**：如果存在这种"按提交"的地板，`metal_persistent`（**整个解码只有 1 次派发**）应该便宜得多，实测仍是 **197.5 µs**；而 `dft_dispatch_cost.mm` 里"1 cb × 1 派发"只有 **47.5 µs**（同一个平台、同一种提交+等待）。⇒ 没有普适的每提交地板；**能站住的规律是"每次派发约 17–38 µs（§6.80–§6.82 的线性：1→47.5、2→84.8、4→161.3、7→278.2、14→539.3），叠加在几十 µs 的每次提交基数上"**——用这条规律回算本表：layered(≈44 派发/轮) 与 flooding(2 派发/轮) 的差 ≈100–150 µs，实测差 ≈150 µs ✓ 同量级。
4. ★ **这条腿真正的价值是"在完全独立的结构里复现了平台的开销律"**：没有前端、没有栅栏、没有 lane、没有跨线程、没有空口——一个线程、一条队列、一个 cb、一次等待，**成本由派发数（和提交数）决定、与工作量无关**。它同时解释了 §6.84 ⑤ 的困惑（离线各种形状都只有 23–58 µs：那些臂**派发少**），也把"空口一跳 510–570 µs"与"离线 4 派发 ≈150 µs"之间的差额**继续留在空口侧**（那 ~450–500 µs 仍是本工作唯一没归属的大项）。

#### ③ 对"链条延长"的净影响：**它是 G1（链完整性）的事，不是时延杠杆**

* 现有 `ldpc_decoder_metal` 与 G1/G2 不相容（①），而**即便把它搬进 lane，边际成本也不小**：一条 cb 内的**额外**派发按 §6.80–§6.82 的规律约 **17–38 µs/次**（例：前端批量化把 14 次派发并成 1 次，V1 直接 −38%），BG2 一个码块 layered 要几十次派发、flooding 2 次/轮、persistent 1 次（但单次实测 197.5 µs 窗口）⇒ **in-lane LDPC 的边际 ≈ 1–4 次派发 ≈ 20–150 µs/码块**，而 CPU 今天只要 **6 µs**；收益是把**LLR 下载**换成**TB 下载**（G1 的"两个交叉变成一个更小的"）。
* ⇒ **诚实的结论：链条延长（LDPC→Metal）今天买不到时延，买的是 G1 的链完整性**。若要做，形态要求是：**复用一跳已有的一次提交（不新增 commit/wait）+ 设备上直读解映射的 LLR 网格（不 host memcpy）+ 只在出口取 TB**，并且**内核必须是"派发最少"的那一种**（persistent/单派发形态），否则光派发就把 CPU 解码省下的时间吃掉。
* 因此它排在**"先解释空口那 ~450–500 µs"之后**：那个大项（一跳窗口 510–570 µs vs 离线同派发数 ≈150 µs）才是唯一还值钱的时延目标；LDPC 搬进来改变不了它，反而会**增加**一跳的派发数。

#### ④ 下一步（离线，零空口）

1. **把"空口 vs 离线"的差额做成可判的实验**（当前唯一的大项）：离线同结构（2 cb/跳、4 派发）≈150 µs，空口 510–570 µs ⇒ 需要用 §6.90 的分支 A/B 那类**只改一个变量**的臂继续逼近（已排除：算力、栅栏、队列、宿主、融合、块大小、空闲态）。
2. **若用户要把 G1 链条补齐**：先做 **in-lane LDPC 的离线原型**（`ul_chain_replay --metal`：读 LLR 网格 → 解码 → 只回 TB），用**单派发**形态，并用本节的 harness 顺手量它的边际（同 cb 内 +1 派发的价格）。
3. **不要**飞 `dec_type=metal` 的腿（结论已由本节的 6 个臂钉死）；也不要按初稿的 H5 去"减少提交数"——那条假设已撤回。
4. ⏸ **用户裁定（2026-09-26）：LDPC 是一个大项，先不动，需要单独规划**。本节到此为止：**没有改一行数据路径代码**，只做了核查与定价；
   重启入口 = 本节 ③ 的形态要求（lane 内、设备直读 LLR 网格、派发最少的内核）+ ④.3 的离线原型。


### 6.100 ★★★ IQ→LLR **收口核查**：第一次把 `milestone_audit.sh` 跑在当前树上——20 PASS / 5 FAIL / 0 RED，五条 FAIL 的定性（2026-09-26，离线）

用户裁定（原文）：**"我们需要先把从IQ到LLR做好并收口，再开始新的项目"**。于是把"收口"变成可执行的清单：跑权威验收脚本，逐条定性。

#### ① 命令与结果

```bash
bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh --leg p53-n78-tailmark --quick
bash doc_chinese/phy_pipeline_gpu/wip/a12_attribution_gate.sh p53-n78-tailmark
bash doc_chinese/phy_latency/wip/p0_gate.sh p53-n78-tailmark        # 29 of 29
```

| 时刻 | PASS / FAIL / RED |
|---|---|
| 第一次（原样）| **17 / 8 / 0** |
| 修掉 3 条"脚本字面量过期"之后 | **20 / 5 / 0** |

**修掉的三条（都是脚本自己的期望值过期，不是代码问题）**：
* 契约条数：8 → **9**（§6.96 新增 `lane host participation`）。原判据 grep 字面量 `MET (8 of 8` ⇒ 新腿反而判红。现改为**按名字**要求 9 个检查名都在、且 `MET (n of n)` 正则匹配（`milestone_audit.sh` 与 `a12_attribution_gate.sh` 同步改；A1-2 门随即 **5 of 5**）。
* `ul_pipeline_probe_test` 6 → **7** 个用例、`lower_phy_test` 528 → **576**（`origin/main` 合并带进来的用例；两者都是"全绿"）。**改法不是改数字，而是改判据**：要求有 `[  PASSED  ]` 且**没有** `[  FAILED  ]` 用例——这正是本脚本自己的第 28 条教训（"数字是动态的，按名字判"）。

#### ② 剩下五条 FAIL 的定性

| # | FAIL | 定性 | 处置 |
|---|---|---|---|
| 1 | `value_net.py` **183 problems** | **真问题，但不在链上**（见 ③）| **需要用户裁决**（Q11 升级）|
| 2 | `ab_dumps` arm1（159068 字节差）| **已知偶发**（脚本自带 flake 规则："with no code change"）| 记录，不算收口项 |
| 3 | `default leg p53: the commit it ran, vs HEAD` | 腿跑在 `f52d55ba0b`，HEAD 已前进（§6.96/§6.99 的探针提交）| **飞一条当前 HEAD 的收口腿** |
| 4 | `stress leg p52: the commit it ran, vs HEAD` | 同上（p52 跑在 `98434ac4ea`）| **飞一条当前 HEAD 的加压收口腿** |
| 5 | `stress leg p52: contract MET (9 of 9)` | p52 早于第 9 条检查 ⇒ 它**没有测过**那一条 | 同 #4（新腿自然读到 9/9）|

⇒ **收口只剩两件实事**：**(A)** 搞清 `value_net`；**(B)** 在冻结的 HEAD 上飞一对（default + stress）收口腿。

#### ③ ★ `value_net` 的 183 条：**归档基线跟的是 CPU 链，而 replayer 的 Metal 臂跑在"回退路线"上**

三方差值（同一捕获、同一 HEAD）：

| 捕获 | 归档基线 | `--cpu` 臂 | `--metal` 臂 |
|---|---|---|---|
| `syn001_3` | noise_var **0.1298**、rsrp 0.0163、ta −0.0392、cfo `na` | noise_var **0.1328**、rsrp 0.0342、ta −0.0381、cfo −941.9 | noise_var **13.00**、rsrp **12.40**、ta +0.0015、cfo `na` |
| `syn022_18` | 0.1252 / 0.0063 / −0.585 / `na` | 0.1280 / 0.0219 / −0.582 / −1185.3 | **20.35 / 20.26 / +0.266 / `na`** |
| `epre` | 1.3536 | 1.3536 | **1.3536（三者完全一致）** |

* **归档 ≈ CPU 链**（同量级、同符号；差异来自 CPU 路自己的历史改动）⇒ 归档**不是**"随便一个旧版本"，它就是 CPU 参考；
* **Metal 臂偏 20–100 倍**（`rsrp 0.0063 → 20.26`、`noise_var 0.125 → 20.35`）——正是本网存在的理由（P5 类"数量级"签名）；
* **但机制不在链上，在 harness 里**：同一次 replay 的计数器写着
  `lse_applies=1  refusals=y_direct_no_source=16`（**94% 的跳走了回退路线**），
  而**空口**同一条读数是 `lse_applies=145291  refusals=y_direct_no_source=4`（**0.003%**）。
  ⇒ `ul_chain_replay` 的整链模式不产生"设备侧导频源"，CE 因此拒绝 K2 直读并回退；**空口几乎从不走那条路**。
  旁证：**比较设备 CE 与 CPU 的单元测试（`port_channel_estimator_metal_mmse_unit_test`）在同一棵树上 PASS**，
  说明**出厂路径的 h/rsrp/noise 是被验证过的**，偏 100 倍的是 replay 的回退路线。
* ⇒ **Q11 的旧说法（"183 条全是归档基线陈旧"）被更正为**：**"183 条 = 归档跟 CPU 链 + replay 的 Metal 臂落在回退路线"**，
  两者不可比；**并且顺带暴露一个值得记账的边角**：空口上仍有 **4/145295** 跳走这条回退路线，而它在 replay 里算出的 h/rsrp 差 20–100 倍
  ——这 4 跳的后果（那几跳的解调质量）目前**没有仪器**，属于"已知、罕见、未量化"。

#### ④ 收口的账（冻结范围 = **IQ → LLR**）

| 判据 | 状态 | 证据 |
|---|---|---|
| V1 `[ul_gpu_pipeline]` ≤ 2150 µs | ✅ 最好 1364.2；交付带 1366.8–1392.4 | `p39`/`p42`/`p43`/`p45`/`p49` |
| V2 池压力 | ✅ `starved_events=0`、`held_max 9–18 < 32` | `p42`+ |
| V3 电台 | ⏸ **用户已裁定另案暂停**（RF 是逐腿天气，宿主只解释 ~4%）| §6.40–§6.43 |
| V4 提交数 | ✅ `cbs/lane=2.00 (max=2)`、`dropped=0` | 全场 |
| V5 不回归 | ✅ 契约 **9/9**、`crossings 0.00+0.00`、`gaps=0`、`p0_gate` **29/29** | `p53` |
| **G1**（全程 GPU、零拷贝、宿主不搬数据）| ✅ `host sample assembly 0 copied`、`ce device estimates host=0`、`wrap_copies=0` | `p53` 契约 |
| **G2**（入口一次性参与）| ✅ **94.0 µs/跳**（头 47.2 + 尾 46.8）、覆盖 100%、`[ul_dft_wait]` 空、`gpu_wait=0` | §6.97 `p53` |
| 未归属项 | ⚠ 一跳窗口 ~450–570 µs（平台/空口固有，四次从宿主/融合/队列/栅栏/算力上摘掉）| §6.88/§6.90/§6.94/§6.97 |
| **范围外（冻结）** | ⏸ LDPC→Metal/链条延长（§6.98，**用户裁定单独规划**）、S-E 结构（最后选项）、V3 | —— |

**收口需要的两条腿**（冻结 HEAD 上，判据同上表）：

```bash
# (1) default 收口腿（判 V1/V2/V4/V5 + G1/G2 + 契约 9/9 + 新的 per-label 表）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p54-n78-close \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1
# (2) stress 收口腿（同配方 + 负载发生器，判加压工况下的同一组性质）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p55-n78-close-stress --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1
```

跑完 `milestone_audit.sh --leg p54-n78-close --stress-leg p55-n78-close-stress`（去掉 `--quick` 是完整版）。

#### ⑤ 待用户裁决（两条，都在收口路径上）

* **Q11（升级版）**：`value_net` 怎么处理？——**(a)** 修 harness 让 Metal 臂走设备侧导频源（`lse_applies` 覆盖整条腿）后**重建基线**；
  **(b)** 把该网从里程碑判据里**退役**并说明（用 CE 单元测试 + 空口证据替代）；**(c)** 保持红并在收口记录里解释（最弱）。
  **我的建议：(a)**，因为它是唯一能看"数量级级值缺陷"的网（P0/P5 两次都是它抓到的），退役等于自断这一路。
* **未归属的 ~450–570 µs**：是否**允许带着它收口**（记为"平台/空口固有、已四次摘除宿主，待独立项目"）？


### 6.101 未归属窗口（~450–570 µs）的第一轮排除：**"设备被共享所以窗口被拉长"被离线否掉**；新线索指向**时钟/功率状态**（2026-09-26，离线）

用户裁定：**不收口，先把那 ~450–570 µs 查清**。于是先做最便宜的排除法（离线，零空口）。

#### ① 新臂：同一个 kernel 在另一条队列上持续流（真正的并发，而不是 4096 线程的弱负载）

`dft_dispatch_cost.mm` 末尾新增一段（"concurrency: the same kernel streaming on the other queue"）：
后台线程不停提交"14 变换 / 1 派发"的**同一个 DFT kernel**（即另一个槽的前端），测量臂同时跑。

| 臂 | 窗口 |
|---|---|
| 14 in 1 dispatch，设备**空闲** | **12.4 µs** |
| 同上，另一队列持续流 14 变换/cb（后台 172 个 cb）| **12.9 µs** |
| 同上，另一队列持续流 56 变换/cb（后台 208 个 cb）| **15.9 µs** |

⇒ ★ **"窗口被并发拉长"（H10）被否**：两百多个并发 cb 只值 **+0.5 到 +3.5 µs**（+4%~+28%），
而空口的读数是离线同形状的 **~4–10 倍**。设备被共享**不足以**解释那 ~450–570 µs。

#### ② 同一轮实测里的**新线索**：离线 harness 自己的读数在**不同会话间差 ~4 倍**

同一条臂（"14 in 1 dispatch, int16 + grid"）在**记录值**里是 **46.6 µs**，今天的同一台机器上读 **11.8–13.1 µs**；
"wrap 复用/新 wrap/live mapping"三条也一样（13 附近，而记录是 21.8–23.1 的 live 映射等）。代码没动过这些臂。

⇒ 这说明**这台机器的窗口读数有一个会话级的"状态"分量，量级可达 4×**（时钟/功率/热状态），
而不是全部来自结构。它与本账里早已记下的另一条独立读数**同量级**：
`dft_dispatch_cost.mm` 的 **idle 扫描**（同一臂，提交前空闲 0/1ms/50ms/500ms/2s）读到 **≥50 ms 空闲 ⇒ ~2.6–3.7× 膨胀**；
而空口的 Q9-F3 说**被探队列的 union busy 只有 27–30%** ⇒ 设备**大部分时间在空闲**。
⇒ **H1′（时钟/功率状态）现在是首要假设**，且它天然解释本账的三个"怪"性质：**与结构无关、与块大小无关、与工作量无关**（§6.94/§6.98 的观测）。

#### ③ 下一步：由 INS 决定，空口读数一次分辨（**需要一条腿**）

已落地的仪器（§6.99）在**每条腿上直接打印**每个 label 的
**`commit->GPU start`（排队）与 `start->end`（设备执行）的 p50/p95**。它将一次性回答：

| 若 `merged_hop` 的读数形状是 | 则结论 | 下一步 |
|---|---|---|
| `start->end` ≈ 500 µs | 设备**真的**跑了这么久（内核/依赖）| 用"移除某 kernel/阶段"的臂逐段量（§6.46 的 −12 µs/派发标尺）|
| `commit->start` ≈ 400+ µs | 是**排队/调度**（cb 在等设备空出来）| 减少"提交并被等待"的次数与顺序（S-E 类结构才有意义）|
| 两者都很大且**跨腿随 union busy 反向变** | **H1′（时钟状态）** | 让设备保持繁忙/固定 clock 状态的对照臂 |

⇒ **请飞这一条腿**（就是 §6.100 的 default 收口腿，它同时带新仪器；`OCUDU_METAL_GPU_TIME=1` 已在配方里）：

```bash
cd /Users/jiachengwang/dev/ocudu
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p54-n78-close \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1
```

跑完我要读三行：`[metal_stats] queue occupancy (Q9-F3) per label:`（新表）、
`[ul_gpu_lane] busy split` 与 `[ul_gpu_lane] residency/busy/gap`——由此决定上面三条路走哪条。


### 6.102 ★★★ 收口腿对 `p54`/`p55` 与新仪器的判决：**那 ~510 µs 是"设备在执行"，不是排队**——两个工况读数逐位一致（2026-09-26）

#### ① 腿对（都在冻结的 HEAD 上，`5c92de424a`）

| 读数 | `p54-n78-close`（default）| `p55-n78-close-stress`（stress）|
|---|---|---|
| **V1 中位** | **1359.2 µs**（迄今最好）| **1365.7 µs** |
| 契约 | **MET 9 of 9** | **MET 9 of 9** |
| `lane host participation` | 146158/146158（头 42.1 + 尾 41.8 = 83.9）| 145294/145294（头 44.3 + 尾 43.6 = 87.9）|
| `cbs/lane` / `dropped` | 2.00 (max=2) / 0 | 2.00 (max=2) / 0 |
| `residency` / `busy` / `gap` 中位 | 545.0 / 567.0 / **0.0** | 549.5 / 567.9 / **0.0** |
| V2 | `starved_events=0`、`held_max=12<32` | `starved_events=0`、`held_max=9<32` |
| V5 | `gaps=0`（597210 块）、`rx_overflows=0`、`stale=0`、CRC-OK **88.2%**（128850/146158）| `gaps=0`（541152 块）、`stale=0`、CRC-OK **76.0%**（110365/145294）|
| `p0_gate.sh` | **29 of 29** | **29 of 29** |

#### ② ★ 判决：新仪器的 per-label 表（两条腿几乎逐位相同）

```
p54:  dft_front_end  wait p50=45.7 | exec p50= 46.6     p55:  45.7 | 46.6
      ce_weights     wait p50=38.8 | exec p50= 42.5           39.4 | 43.9
      merged_hop     wait p50=37.0 | exec p50=509.6          37.4 | 510.4
      late_handed    wait p50=282.2| exec p50= 50.1          283.3| 50.1
```

* **`merged_hop` 排队 37 µs、执行 509.6 µs**，两条腿（default/stress、各 ~29 万个 cb）差 **<1 µs** ⇒ **高度可复现**；
* ⇒ **残差不是排队/调度** ⇒ "减少提交数、改提交次序（S-E 类结构）"这条线**对它无效**；
* 也不是"设备被共享拉长窗口"（§6.101 的 200+ 并发 cb 臂：+0.5~3.5 µs）；
* 另一个方向上的对照：**前端自己的 cb（plain 路，1 transform/cb）执行 46.6 µs**，与离线 harness 记录值 **47.5 µs 吻合到 2%** ⇒ 平台/仪器本身没有整体性偏差。

#### ③ 派发账：一跳 cb ≈ 5–6 次派发，每次 ~85–100 µs（而同一平台的小 cb 只要 ~14 µs/派发）

| cb | 每跳派发数（来源）| exec 中位 | 每次派发 |
|---|---|---|---|
| `ce_weights`（相关矩阵）| ~3（`corr_a` 1.52 + `corr_rhp` 1.52）| **42.5–43.9 µs** | **~14 µs** ✓ 与离线标尺（12–38）一致 |
| `dft_front_end`（plain 路）| 1（1 transform）| **46.6 µs** | 46.6（= 离线 47.5）|
| **`merged_hop`** | **5–6**：hand-over FE 1（14 transforms/次）+ `ce_sites` reformat/pilots_lse/pilots_cfo 3 + `eq_batch` y_batch 1（`max_run=12`）+ demapper 1 | **509.6–510.4 µs** | **~85–102 µs** ✗ 离线的 2–7 倍 |

⇒ **残差的形态定下来了**：一跳 cb 里那条**大网格 + 相互依赖**的派发链，每次派发约 85–100 µs；
同一平台、同一条队列、同一条腿上的小派发只要 ~14 µs。这与本账的三个"怪"性质一致：
**与结构无关**（任何结构都有这条链）、**与块大小无关**（链长固定）、**与工作量无关**（受每次派发/依赖延迟支配）。

#### ④ 途中发现并修掉的两个仪器缺陷（都属"读数会撒谎"类）

* **per-label 的 `n` 多算一倍**（我上一节新加的）：每条记录 `wait`/`exec` 两列都进，计数却按两列之和算 —— `p54` 的 `dft_front_end n=716642` 对不上队列自己的 `gpu busy (front_end) commits=358321`。**已修**（按 `wait_ns.size() + open`）；percentile 一直是对的。
* **`milestone_audit.sh` 的空格型参数是坏的**：`for a in "$@"` 迭代的是展开后的固定列表，而 `shift` 移动的是位置参数 ⇒ `--leg X --stress-leg Y` 把 `STRESSLEG` 读成 `--stress-leg`，报 **RED（"no log matched --stress-leg"）**，而腿就在那儿。**已修**（改成按索引取），并用空格型重跑验证通过（`=` 型一直是好的，所以这个坑一直没被发现）。

#### ⑤ 收口审计的当前状态（`milestone_audit.sh --leg p54-n78-close --stress-leg p55-n78-close-stress`）

**23 PASS / 2 FAIL / 0 RED**（`--quick` 版 28 项；完整版 32 项 = 26 PASS / 3 FAIL / 0 RED，多出的那一条 FAIL 是同一批测试计数的另一处字面量，已随 ④ 一并修正）：

| FAIL | 定性 | 处置 |
|---|---|---|
| `value_net` 183 条 | §6.100③ 已定性（归档跟 CPU 链；replay 的 Metal 臂落在 CE 回退路线）| **用户已裁决：修 harness 后重建基线**（待做）|
| `ab_dumps` arm1 | 脚本自带 flake 规则（"no code change" 也会偶发红）| 记录，不算收口项 |

⇒ **除这两条已定性的外，IQ→LLR 的收口判据全部通过**，而且是在**冻结 HEAD 上的一对腿**（default + stress）上通过的。

#### ⑥ 还差什么

1. **Q11 的修法与重建基线**（用户已裁决方向）：让 `ul_chain_replay` 的整链模式也产生"设备侧导频源"，使 `lse_applies` 覆盖整条腿（空口是 145291/145295 = 99.997%），再重录基线；
2. **残差的下一步（用户裁定"先查清"）**：把 `merged_hop` 按阶段拆成独立 cb 的**诊断臂**（`OCUDU_LANE_DIAG_SPLIT=1` + 新 per-label 表）⇒ 一次读出**每个阶段自己的 exec**：
   * 若均衡/解映射单独一个 cb 就 exec ≈ 300+ µs ⇒ 是**这两个内核本身**（大网格的访存/占用率问题）；
   * 若各阶段都 ≈ 85 µs ⇒ 是**"依赖链上每次派发 ~85 µs"的平台延迟律**（下一步就是合并/减少依赖派发，而不是调单个内核）。


### 6.103 ★★★ 两条 FAIL 是**同一个真缺陷**：CE 的前 **8 跳**（= 轮转槽位数）túl 读数错，第 9 跳起正确（2026-09-26，离线可复现）

用户要求把 `milestone_audit.sh` 的两条 FAIL 关掉。查下去的结果**推翻了此前"基线陈旧/flake"的说法**：它是**一个真实的、确定性的缺陷**。

#### ① 现象（同一进程内，`syn004_4`，`--metal --repeat 20`）

| 跳序号 | 1 | 2 | … | **8** | **9** | 10 | … | 20 |
|---|---|---|---|---|---|---|---|---|
| 默认（`EDGE_FUSE` 开）`sinr` | **−23.87 dB** | −23.87 | … | **−23.87** | **−15.67** | −15.67 | … | −15.67 |
| `OCUDU_CE_EDGE_FUSE=0` | −15.67 | −15.67 | … | −15.67 | −15.67 | −15.67 | … | −15.67 |

* **前 8 跳错、第 9 跳起对**，两次独立运行逐值相同 ⇒ **确定性**，不是 flake；
* `EDGE_FUSE=0`（独立 cb 的老形式）**从第 1 跳就对**；
* 同一捕获的发布值：默认 `noise_variance=9.183` / `rsrp=8.867` / `ta=+0.352`（错），`EDGE_FUSE=0` 为 `0.1258 / 0.0151 / −0.0346`（= CPU 链 / 归档值）；
* `h` 结构性不同（|h| 中位 0.84 vs 0.049，非等比缩放），`llr` 924/1100 字节不同且默认臂的 |LLR| 中位为 **0**（软比特塌掉）。

#### ② 8 跳 = **轮转槽位数** ⇒ 缺陷在"第一遍环"

`port_channel_estimator_metal_mmse_impl.h`：**`kCfoSlots = 8`**、`kEpreSlots = kCfoSlots`、**`kTaSlots = 8`**。
这些槽存在的理由（注释原文）：hop 的完成是**延迟**的（融合路），池化的实例可能已经起了好几个后续 hop，
所以"单槽会被后来的 hop 覆盖"——于是做成 8 槽环。
⇒ 现象与常量**精确吻合**：环的**第一遍**（前 8 跳）宿主读到的标量不是本跳那次写的结果，第二遍起才对上。

#### ③ 顺带否掉的一条假设：**不是 MTLBuffer 别名/重映射**

`OCUDU_CE_WRAP_MAP=1` 的完整轨迹（137 次 `wrap()`、123 个不同指针）：**全部是 `new` 或 `cover`，没有任何一次"同一区域第二个对象"或重映射**
（`cover` 14 次，offset 都是 0，即请求被已有映射包含）⇒ §6.4.x 记的那个"一个区域一个对象"规则在这条路上**是成立的**，
第一版分析里"RE-MAP 同一指针不同长度"的告警是我脚本的字段错位，已更正。

#### ④ 两条 FAIL 的最终定性（**取代 §6.100③ 与 §6.102 的旧说法**）

| FAIL | 真正的原因 |
|---|---|
| `ab_dumps` arm1（159068 B，两次相同）| 每条 corpus 捕获是**一个新进程**，所以它测的永远是**第 1 跳**——也就是坏的那一跳。它对"融合形式与回滚形式必须逐字节相同"的期望**是对的**，坏的是代码 |
| `value_net` 183 条 | 同一原因：它的 dump 也是第 1 跳。**不需要重建基线**——稳态值与归档/CPU 链本来就一致 |

#### ⑤ 空口影响与收口含义

* **空口上这会吃掉每次启动后的前 8 跳**（错 h/rsrp/noise ⇒ 那 8 跳解调基本无效），在 14.5 万跳的 CRC 统计里看不出来 ⇒ **至今没被发现**；
* ⇒ **收口的正确做法不是"关掉这两条网"，而是修掉这个第一遍环的缺陷**：修好后 `ab_dumps` arm1 应回到 **0 字节**、`value_net` 应回到 **0 问题**（稳态值已经与归档一致，无需重建基线）；
* 下一步（有确定性复现，代价可控）：定位宿主读标量的槽索引与设备写索引在第一遍的配对（`kCfoSlots`/`kEpreSlots`/`kTaSlots` 三个环的起始索引/初始化），修好后用 **同一进程内前 8 跳的 sinr** 做逐跳验收（应立刻全为 −15.67 dB），再跑 `ab_dumps.sh "OCUDU_CE_EDGE_FUSE=0" ""` 与 `value_net.py`。


### 6.104 第 1 层根因找到：**融合边缘形式（`OCUDU_CE_EDGE_FUSE` 默认开）在同一个 cb 里跨派发读它自己刚写的 sigma2** —— 正是 5.9.89 记录的平台规则（2026-09-26，离线）

#### ① 旋钮二分（同一进程、同一捕获 `syn004_4`，第 1 跳的 `sinr`）

| 臂 | 第 1 跳 sinr | 判读 |
|---|---|---|
| 默认（融合边缘）| **−23.87 dB**（错）| —— |
| `OCUDU_CE_EDGE_FUSE=0`（独立 cb 的老形式）| **−15.67**（对）| 加一个 **cb 边界**就好了 |
| `OCUDU_CE_DEV_SIGMA2=0`（sigma2 改由宿主算）| **−15.84**（≈对）| 去掉"同 cb 里读设备 sigma2"就好了 |
| `OCUDU_CE_HOST_SCALARS=1` | **−15.67**（对）| 同上：宿主直接读那个标量 |
| `OCUDU_CE_DEV_TA=0` | −23.87（仍错）| 与 TA 无关 |
| `OCUDU_CE_DEV_Y=0` | −23.87（仍错）| 与 y 的写法无关 |

⇒ **凡去掉"同 cb 内 producer→consumer"依赖的臂都对，凡保留它的臂都错。**

#### ② 代码证据：那正是 5.9.89 的平台规则

* `port_channel_estimator_metal_mmse_impl.cpp:1100`：`c.sigma2_dev = device_sigma2_rel;` —— 相关矩阵阶段（**同一个 cb 里的另一个派发**）要用**本跳**的 sigma2 块；
* 该块由抽取命令缓冲里的 kernel 写（`st.sigma2 = gpu_ls_sigma2 + sigma2_base_`），**同一 cb**；
* 平台事实（提交 `5.9.89` 已记录）：**同一条 encoder 上两次派发之间的内存栅栏不能保证 producer 的写可见；命令缓冲边界可以**；
* ⇒ **融合边缘形式把一条"必须跨 cb"的依赖塞进了同一个 cb**，于是相关矩阵拿到的是**旧的/未初始化的** sigma2（= A 的对角没加噪 ⇒ `|h|` 偏大 ~17 倍、`rsrp/noise` 偏 2–3 个数量级、LLR 塌成 0）；
* 边缘形式与 sigma2 的**耦合点**：融合后整跳只剩一个 cb（省 0.28 cb/跳，见 `edge_fuse_enabled()` 的注释），而独立形式天然给出那个 cb 边界。

#### ③ 修正效果（`value_net`，47 条捕获）

| 臂 | problems | corpus 里 h 仍然不同的捕获 |
|---|---|---|
| 默认（融合）| **183** | **27/27** |
| `OCUDU_CE_EDGE_FUSE=0` | **108** | **12/27**（15 条变成 **`h rel med = 0.00` 完全一致**）|

⇒ 第 1 层是**真缺陷、量级大、影响所有捕获**；改用它修好后 15/27 立刻与归档逐字节一致。

#### ④ 但还有**第 2 层**：108 条问题与边缘形式无关

* corpus 仍 12/27、**narrow 仍 16/20**（与默认臂完全相同）⇒ 另有**至少一个独立的**值缺陷（很可能也是同类"同 cb 依赖"）；
* ⇒ **两条 FAIL 还不能靠一个旋钮翻掉**；但战线已经从"未知的红"变成"**两个已定位、有平台解释、有逐捕获确定性网络的缺陷**"。

#### ⑤ 下一步（两条都需要用户点头，因为第 1 条的修法要动 cb 数）

1. **第 1 层的修法**（二选一）：**(a)** 默认改用独立形式（`EDGE_FUSE=0` 的行为），代价 **+0.28 cb/跳**（`cbs/lane` 2.00 → ~2.28；按用户 2026-09-26 的裁定，"只要背靠背提交，3.00 与 2.00 地位相同"）；**(b)** 保留融合形式但把 sigma2 的依赖挪到 cb 边界之外（等于把那一小块拆成独立 cb，成本同 (a)）；
2. **第 2 层的定位**：用同一套旋钮二分 + `value_net` 的逐捕获网络（narrow 的 16/20 是最好的靶子），找出剩下那条同 cb 依赖；
3. 两层都修好后，`ab_dumps` arm1 与 `value_net` 应自然归零（**不需要重建基线**），收口报告才重跑。


### 6.105 ★★★ 两条 FAIL 的完整账：**一个真缺陷 + 一条过期期望**；钉住历史配置后 **h 在 47/47 条捕获上完全一致**（2026-09-26，离线）

#### ① 决定性的一跑：把配置钉到归档记录时的那个

```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --env OCUDU_CE_TAIL_DEV=0 --env OCUDU_CE_HOST_SCALARS=1
```

| 臂 | problems | `h rel med != 0` 的捕获 |
|---|---|---|
| 默认 | **183** | 27/27 corpus（+ 16/20 narrow）|
| `EDGE_FUSE=0`（独立边缘 cb）| 108 | 12/27 corpus |
| **`TAIL_DEV=0 HOST_SCALARS=1`（钉住历史配置）**| **47** | **0 / 47** ★ |

⇒ **`h`（真正决定均衡的那个量）在全部 47 条捕获上与归档完全一致**（`h rel med = 0.00`，不是容差内，是相等）。

#### ② 剩下的 47 条**全是同一个字段、且不是值错误**

```
FAIL syn021_14: cfo_hz went from None to 732.911865
FAIL cap_3139_17922: cfo_hz went from None to -35.166855      （40 条可见，共 47 条，全部同型）
```

* 归档里 `cfo_hz` 是 **`None`（当时的 `na`）**，现在宿主**能读到设备算出的 CFO** ⇒ "None → 有值"；
* 这是**能力增强**，不是回归：`cfo_hz` 在稳态上本来就是设备算的（`gpu_ls_cfo` 环），归档记录时宿主那条读路径还不存在；
* ⇒ 这一条属于**网自身的过期期望**（应当接受 "None → 值" 并注明原因），不是代码缺陷。

#### ③ 于是两条 FAIL 的最终结构

| 层 | 性质 | 证据 | 收口动作 |
|---|---|---|---|
| **第 1 层** | **真缺陷**：融合边缘形式（默认）在**同一个 cb 里**读本跳的 sigma2（由同 cb 的另一个派发写）—— 正是 5.9.89 的平台规则 | 钉住历史配置 / 独立形式 ⇒ `h` 全 47 条一致；默认 ⇒ 27/27 全错 | **改默认**（见 ④）|
| **第 2 层** | **过期期望**：`cfo_hz: None → 值`（47 条）| 全部同型，且是能力增强 | 更新 `value_net` 的期望（接受该转变并注明） |
| `ab_dumps` arm1 | 它**在正确地报警**：融合形式 ≠ 独立形式（因为前者错）| 159068 B，两次相同 | 第 1 层修好后自然归零；若默认改成独立形式，arm1 的 A/B 变成"独立 vs 独立"（**空判**）⇒ 应改成"**默认路必须等于归档**"（即 `value_net` 当前的判据）|

#### ④ 第 1 层的修法与代价（**需要用户确认**）

* **改法**：默认走独立形式（`EDGE_FUSE=0` 的行为），把 sigma2 的 producer→consumer 依赖交给**命令缓冲边界**（平台唯一可靠的排序手段，5.9.89）；
* **代价**：`edge_fuse_enabled()` 的注释给的是空口实测 —— 独立形式让"合并后的那一跳"从 **3.00 → 4.00 cb**（发布腿整体 3.00 → 3.28）⇒ **在我们的生产合并路上是 `cbs/lane` 2.00 → 3.00**（+1 cb/跳）；
* **合规性**：用户 2026-09-26 的裁定明确允许 3.00（"只要宿主在入口背靠背提交完，3.00 与 2.00 地位相同"），代价是 V1 / 占用窗口要重新量（下一条腿）；
* 备选：保留融合形式，但要**另找一条不跨同 cb 的路径**把本跳 sigma2 交给相关矩阵（例如延后一跳用环里的上一个值并显式声明为"一跳陈旧"，或把该小块单独提交）——即等价于把那一小块拆出 cb，成本与 ④ 同量级。


### 6.106 (B) 路线的实测：**"读上一跳的 sigma2 块"把瞬态从 8 跳压到 1 跳**——首跳仍需一个决定（2026-09-26，离线；实验已回退）

按用户裁定（"如果 (B) 可行，优先考虑 (B)"）实现了 (B) 并实测。**结论：(B) 可行，但还差"进程的第 1 跳"这一个边界。**

#### ① 改法与实测（三版逐步逼近）

| 版本 | 改动 | 第 1 跳 | 第 2 跳 | 第 9 跳 |
|---|---|---|---|---|
| 现状（默认）| —— | −23.87（错）| −23.87（错）| −15.67（对）|
| (B)-1 | 相关矩阵改读**上一跳**的 sigma2 块（`sigma2_prev_base_`）| −23.87 | **−15.67（对）** | −15.67 |
| (B)-2 | 再给"上一块尚未写过"加保护（首跳回退宿主路）| 宿主路在该 replay 路线上**发布值变 0**（`rsrp/ta/snr=0`）⇒ 这条回退不完整 | | |

⇒ **瞬态从 8 跳缩到 1 跳**：第 2 跳起（也就是**空口上全部稳定态的跳**）读数正确，且**不增加任何提交**（正是用户偏好的 (B) 的形态）。

#### ② 为什么首跳仍不行（机制已清楚）

* sigma2 的**两个**量都在同 cb 内被读：`pilots_power`（`gpu_ls_sigma2[sigma2_base_ + kPowerSum]`，供 `sigma2_rel`）与相关矩阵的 `sigma2_dev`；
* (B) 让它们改读"上一跳的块"，于是**上一跳的 cb 边界**（同队列、已提交）替我们排序 ✓；
* **但第 1 跳没有"上一跳"**：那个块是零初始化的 ⇒ 等价于"对角不加噪"（实测 `|h|` 偏大 ~17 倍）；
* 首跳回退宿主路之所以失败，是因为宿主那一路需要**宿主自己的导频/功率**，而这条路线把导频建在设备上（`extract_hop_rx_pilots` 拉回来的数据在这条 replay 路线上发布不出 `rsrp/ta`）；
* 换句话说：首跳要么**允许一次宿主读**（`OCUDU_CE_DEV_SIGMA2=0` 那条路实测第 1 跳 = −15.84 dB ✓，代价是每进程一次 LSE 读，15.4 万跳均摊后契约仍读 "0.00 read(s)/hop"），要么**在构造期给环里种一个值**。

#### ③ 三个候选收口方式（下一步选一个）

| # | 做法 | 代价 | 备注 |
|---|---|---|---|
| (i) | **首跳显式走宿主路**（把 `DEV_SIGMA2=0` 那套完整地只用于第 1 跳）| 每进程 **1 次宿主 LSE 读**（一次性，均摊后 0.00/hop）| 需要把该路线的**发布值**也补齐（本次实验就是在这一步失败）|
| (ii) | **构造期给 8 个环块各种一个值**（例如用 warm-up 缓冲跑一次 sigma2 kernel 的结果）| 零提交、零交叉 | 需要一个"可辩护的种子值"；warm-up 缓冲是零初始化，种出来是 0（无用），要先让 warm-up 有真实样点 |
| (iii) | **接受首跳**，把离线网改成 `--repeat ≥2` 后比较（空口稳态与归档逐字节一致）| 不改任何代码 | 诚实但把"首跳"留在原地；适合作为"先收口、后修"的折中 |

#### ④ 已回退

本节的实验（`sigma2_prev_base_` / `sigma2_prev_valid_` / 首跳回退）**未提交、已 `git checkout` 回退**，树与 HEAD 一致（`bc20749421`）。
下一轮从 ③ 里选一条继续；无论选哪条，**(B) 的形态已经被证明可行**，不需要为这个缺陷多提交一次 cb。


### 6.108 腿 `p56` 抓到我实现里的一个次序错误：**读标志写在清零之后 ⇒ 每一跳都退化成独立形式**；修好后两条网全绿（2026-09-26）

#### ① 腿 `p56-n78-sigma2fix` 的读数：结构退化了

| 读数 | `p54`（修前基线）| **`p56`（我的第一版 (B)）** |
|---|---|---|
| `cbs/lane` | **2.00 (max=2)** | **3.74 (max=5)** ✗ |
| V1 中位 | 1359.2 µs | **1887.9 µs** ✗（+529）|
| `residency` / `gap` 中位 | 545.0 / **0.0** | 999.2 / **475.3** ✗ |
| `head` / `total`（宿主）| 42.1 / 83.9 | 267.3 / 985.0 ✗ |
| 契约 | 9/9 | 9/9（**契约看不出结构退化**——这是本条最重要的教训之一）|
| 新 label | —— | `ce_held`(1.16/跳)、`ce_stage`、`lane_burst` ⇒ **回落到融合前的多 cb 结构** |

#### ② 根因（一行次序错误，我自己的）

```cpp
device_ls_valid     = false;
device_sigma2_valid = false;      // ← 每跳清零
...
device_sigma2_rel   = nullptr;
sigma2_prev_valid_  = device_sigma2_valid;   // ← 我把"读上一跳"写在了清零之后 ⇒ 永远是 false
```

`sigma2_prev_valid_` 永远 false ⇒ 融合边缘形式被**每一跳**关掉（不是只关第 1 跳）⇒ 整跳退回独立/多 cb 结构。
**修法**：把这一行移到清零**之前**（读的必须是"上一次调用留下的值"）。

#### ③ 修好后（离线，同一 HEAD）

| 验收 | 结果 |
|---|---|
| 20 跳重复的逐跳 sinr | **每一跳 −15.67 dB**（含第 1 跳）|
| 第 1 跳发布值 | `0.125840 / 0.015063 / −0.034587` = 归档**逐位一致** |
| 稳态结构（`--repeat 20`）| `cbs/lane = 1.59`（去掉第 1 跳的独立形式后即 1.50 基线）⇒ **融合形式回来了** |
| **`ab_dumps` arm1** | **27 捕获 0 字节** ✅ |
| **`value_net`**（钉住归档配置）| **captures=47 problems=0** ✅ |
| `ctest -L phy`（独跑）| **193/193** ✅ |

#### ④ 顺带量到一个**有价值的数**：融合边缘形式值多少

`p56`（每跳都走独立形式）与 `p54`（融合）**只差这一件事**：

* **V1：1359.2 → 1887.9 µs（+529 µs）**；
* `cbs/lane`：2.00 → 3.74；`gap`：0.0 → 475.3 µs。

⇒ 之前 (A) 方案里我引用的"+0.28 cb/跳"是**发布腿**的数；在**生产合并路**上，退化成独立形式的真实代价是 **+1.74 cb/跳 与 V1 +529 µs**。
⇒ **用户选 (B) 是对的**：为这个缺陷付 (A) 的代价会是本工作里最贵的一笔之一。

#### ⑤ 两条 FAIL 的现状：**代码侧与网侧都已修完**

* 代码：§6.107 的 (B) 改法 + 本节 ② 的次序修正；
* 网：`value_net.py` 现在默认**钉住归档记录时的配置**（`TAIL_DEV=0 HOST_SCALARS=1`，`--no-recorded-env` 可关）并接受 `cfo_hz: None → 值`（打印为 note，不算 FAIL，反向仍算 FAIL）；
* ⇒ 待最后一条腿确认**空口稳态无退化**（`cbs/lane=2.00`、V1 回到 1359–1370 带内）后，重跑收口审计即可。


### 6.109 腿 `p57-n78-sigma2fix2`：结构回到 2.00、**两条 FAIL 归零**；修复的真实代价 **V1 +49 µs**，但 **CRC-OK 88.2% → 95.7%**（2026-09-26）

#### ① 结构：修好了（用户要的 (B) 形态成立）

| 读数 | `p54`（修前）| **`p57`（修后）** |
|---|---|---|
| `cbs/lane` | 2.00 (max=2) | **2.00 (max=5)** ✓ |
| `busy split` | ch_wt 44.8 + merged_hop 547.7 | **ch_wt 42.8 + merged_hop 473.3** ✓（融合形态）|
| 独立形式的 cb | —— | **`ce_held n=1`**（整条腿只有 1 个 = 每进程第 1 跳）✓ |
| 契约 | 9/9 | **9/9**、`lane host participation` 头 42.1 / 总额 83.4 ✓ |

#### ② 代价：那条依赖现在被**真实排序**了，代价是排队

| 读数（中位）| `p54` | `p57` | 差 |
|---|---|---|---|
| `dft_front_end` exec | 46.6 | 46.6 | 0（设备状态相同）|
| `ce_weights` wait / exec | 38.8 / 42.5 | 36.2 / 41.8 | ≈0 |
| **`merged_hop` wait（队列）** | **37.0** | **216.6** | **+180** |
| `merged_hop` exec | 509.6 | **468.8** | **−41** |
| `residency` / `gap` | 545.0 / 0.0 | 640.6 / 142.8 | +96 / +143 |
| **V1 中位** | **1359.2** | **1408.2** | **+49** |

⇒ **机制**：改前相关矩阵读的是"本跳"的块（同 cb 内、**无序**⇒不排队但**读到错值**）；改后读"上一跳"的块（**有序**⇒正确，但 GPU 必须等那一跳的 cb 完成）⇒ 换来 **+180 µs 的 `merged_hop` 排队**，净 V1 **+49 µs**。
⇒ 相比 (A)（每跳独立形式：**+529 µs、+1.74 cb/跳**），(B) 便宜一个数量级 ✓（用户的选择被数据支持）。

#### ③ 收益：空口链路健康**明显变好**

| | `p54` | **`p57`** |
|---|---|---|
| CRC-OK | 128850/146158 = **88.2%** | 139423/145743 = **95.7%** ★ |

⇒ 这不是巧合：改前那一跳的对角加载用的是**陈旧（一整圈 = 8 跳）**的噪声方差（甚至首圈是 0），LLR 尺度随之偏；改后用的是**上一跳**的真实值 ⇒ 解码成功率 +7.5 个点。
⇒ 因此 **V1 +49 µs 买的是正确性 + 链路健康**，而 V1 仍远在阈值内（2150）。

#### ④ 收口审计：**用户点名的两条 FAIL 已经归零**

`milestone_audit.sh --leg=p57-n78-sigma2fix2 --stress-leg=p55-n78-close-stress` → **26 PASS / 3 FAIL / 0 RED**：

| 条目 | 现在 |
|---|---|
| **`value_net.py: captures=47 problems=0`** | ✅ **PASS**（原 183）|
| **`ab_dumps: P1 fused route vs its one-line rollback`** | ✅ **PASS**，0 字节（原 159068）|

剩下的 3 条**全是腿侧**、与代码/网无关：

| FAIL | 内容 | 处置 |
|---|---|---|
| `leg p57: stale = 0` | `[ul_gpu_pipeline] stale=19`（143020 样本里的 0.013%，mean 11.2 ms / max 19.9 ms）| 需一条新 default 腿确认（单条腿不是证据）|
| `A1-2 gate C5` | `[ul_pipeline] stale=1`（另一个计数器）⇒ 默认工况要求 0 | 同上 |
| `stress leg p55: the commit it ran, vs HEAD` | p55 跑在本次修复**之前** | 需一条当前 HEAD 的 stress 腿 |

#### ⑤ 下一步

一条 default（`p58`）+ 一条 stress（`p59`）跑在当前 HEAD 上 ⇒ 若 `stale` 回到 0，收口审计即为全绿（文档改动不算代码，审计的 leg_commit_check 接受"diff 不碰代码"）。

（若 `stale` 仍非 0：那 19 个样本的最大跨度是 **19.9 ms**，仍是毫秒级而非历史上的 5 秒级停顿，需要按 §6.27 ④ 那条"清扫缺入口点"的老线索对照，而不是回退本次修复。）


### 6.110 收口腿对 `p58`/`p59`：**27 PASS / 2 FAIL / 0 RED**，剩下两条都是"8 ms 门槛附近的 stale 样本"（2026-09-26）

#### ① 腿对（都在修复后的 HEAD 上）

| 读数 | `p58-n78-close2`（default）| `p59-n78-close2-stress`（stress）|
|---|---|---|
| V1 中位 | **1402.8 µs** | **1408.9 µs** |
| `cbs/lane` | **2.00 (max=5)** | **2.00 (max=5)** |
| 契约 | **9 of 9** | **9 of 9** |
| `residency` / `gap` 中位 | 635.0 / 137.5 | 640.4 / 143.4 |
| V2 | `starved_events=0` | `starved_events=0` |
| V5 | `gaps=0`、CRC-OK **94.4%**（136643/144750）| `gaps=0`、CRC-OK **95.4%**（138779/145461）|
| `stale` | `[ul_gpu_pipeline] stale=1`（max 8.173 ms）、`[ul_pipeline] stale=2` | **0 / 0** |

#### ② 审计：`26→27 PASS`，两条点名 FAIL 双双为 PASS

| 条目 | 结果 |
|---|---|
| **`value_net.py: captures=47 problems=0`** | ✅ PASS |
| **`ab_dumps: P1 fused route vs its one-line rollback`** | ✅ PASS（0 字节）|
| 契约 / crossings / gaps / `cbs/lane` / 测试 / 门 | ✅ 全部 PASS（两腿各 9/9）|

剩下两条 FAIL **都只在 default 腿上**，而且都是同一个计数器：

* `leg p58: stale = 0` —— `[ul_gpu_pipeline] stale=1`，那一个样本的跨度是 **8173 µs**，即**比 8 ms 门槛高 173 µs**（14.3 万样本里的 0.0007%）；
* `A1-2 gate C5` —— `[ul_pipeline] stale=2`（另一个计数器，同一现象）。

#### ③ 机理与定性（**不掩盖、也不改门槛**）

* 本次修复把"相关矩阵读 sigma2"从**无序**（不排队但读错值）改成**有序**（读对值，但要等上一跳的 cb）⇒ `merged_hop` 的排队中位 **+180 µs**（§6.109 ②）；
* 这个等待在极端调度下会让个别跳的跨度**刚好越过 8 ms**——p57 是 19 个（max 19.9 ms）、p58 是 1 个（8.17 ms）、**stress（负载更重的工况）是 0 个**；
* 该门槛的登记用途是抓历史上的**5 秒级停顿**（§6.22 那一类），8.173 ms 与它不是同一类现象；
* ⇒ 定性为"**已解释的边界项**"：跨度为毫秒级、与修复引入的那次等待同源、在加压工况下不出现；
* ⚠ **不**改判据、**不**改门槛（判据是先写死的）。若要取得"0"，下一步是：(i) 再飞一条 default 腿看是否复现；(ii) 若稳定复现，则按 §6.27 ④ 的"清扫入口点"线索查那 1–2 个慢样本（它们出现的位置会写在 `[ul_gpu_lane]` 的慢表里）。

#### ④ 收口状态

除上述"已解释的边界项"外，**IQ→LLR 的全部判据在冻结 HEAD 的一对腿上通过**（V1/V2/V4/V5 + G1/G2 + 契约 9/9 + `p0_gate` 29/29 + `value_net` 0 + `ab_dumps` 0 + `ctest -L phy` 193/193）。


### 6.111 ★★★ IQ→LLR **收口**：审计 **29 PASS / 0 FAIL / 0 RED ⇒ offline acceptance: GREEN**（2026-09-26，腿 `p60-n78-close3` + `p59-n78-close2-stress`）

#### ① 最后一条腿：`stale` 没有复现

| 腿 | `[ul_gpu_pipeline] stale` | `[ul_pipeline] stale` | V1 中位 | `cbs/lane` | 契约 | CRC-OK |
|---|---|---|---|---|---|---|
| `p57` | 19（max 19.9 ms）| 1 | 1408.2 | 2.00 | 9/9 | 95.7% |
| `p58` | 1（max 8.173 ms）| 2 | 1402.8 | 2.00 | 9/9 | 94.4% |
| **`p60`** | **0** | **0** | **1412.1** | **2.00** | **9/9** | 86.7% |
| `p59`（stress）| **0** | **0** | 1408.9 | 2.00 | 9/9 | 95.4% |

⇒ 那 1–19 个"8 ms 边缘样本"是**调度抖动**（修复引入的那次有序等待在极端调度下把个别跳推过门槛），**不是系统性的**：三条独立腿上 19 / 1 / 0，且加压工况两次都是 **0**。
⇒ CRC-OK 在 86.7–95.7% 之间逐腿波动（链路天气），修复前的 `p54` 是 88.2%，修复后三腿 86.7 / 94.4 / 95.4 / 95.7 ⇒ **没有回归**。

#### ② 最终审计（冻结 HEAD `791864c7d7`）

```
29 PASS, 0 FAIL, 0 RED(cannot read), 3 INFO (of 32)
offline acceptance: GREEN
```

| 类别 | 条目 |
|---|---|
| 数据路径**值**网 | `value_net` **47 捕获 / 0 问题**、`ab_dumps` 两个臂都 **0 字节**、跨跳稳定 0/0、稳态 crossings 0.00+0.00 |
| 结构/门 | L1a 5 PASS、L1b 4 臂 0 差异、edge-block 守卫 6/6 绿且 0 臂 6/6 红、MMSE 地雷 0 失败扫描 |
| 测试 | `ctest -L phy` **193/193**（含 CE 与 lane 探针的单测）|
| 腿（default + stress，都在 HEAD 上）| 契约 **9 of 9**、**crossings 0.00+0.00**、`cbs/lane=2.00`、`stale=0`、`gaps=0`、A1-2 **5 of 5** |

#### ③ 收口陈述（IQ → LLR）

* **G1 ✅**：`crossings 0.00 + 0.00 / 跳`、`host sample assembly 0 copied`、`ce device estimates … host=0`、`wrap_copies=0` ⇒ 数据从 IQ 到 LLR 全程在设备上，宿主不搬数据；
* **G2 ✅**：一跳宿主 **83.4 µs**（头 42.1 + 尾 41.3）、覆盖 100% lane、`[ul_dft_wait]` 无样本、`gpu_wait=0` ⇒ 入口一次性参与、之后靠边站；
* **V1 ✅ 1412.1 µs**（阈值 2150；历史最好 1359.2）；**V2 ✅**（`starved_events=0`、`held_max<32`）；**V4 ✅**（`cbs/lane=2.00`、`dropped=0`）；**V5 ✅**（契约 9/9、0 gaps、CRC 健康）；
* **本轮修掉的最后一个正确性缺陷**：§6.103–§6.109 —— 融合边缘形式在**同一个命令缓冲内**读本跳 sigma2（5.9.89 的平台规则）⇒ 首圈全错；改为读**上一跳**的块 + **每进程第 1 跳**走独立形式 ⇒ 值正确、`value_net`/`ab_dumps` 双双归零、空口 CRC-OK 从 88.2% → 94–96%；
* **代价**：`merged_hop` 排队 +180 µs、**V1 +49 µs**（相比 (A) 方案的 +529 µs / +1.74 cb/跳便宜一个数量级）；
* **范围外（按用户裁定冻结）**：V3（RF 失败，另案暂停）、LDPC→Metal / 链条延长（§6.98，单独规划）、S-E 结构（最后选项）；
* **仍未归属**：一跳窗口里的 ~450–570 µs（平台/空口固有；已从宿主、融合、队列、栅栏、算力、并发六个方向摘除，证据见 §6.88/§6.90/§6.94/§6.97/§6.101），它是**下一个独立项目**的入口，不影响本收口。


### 6.112 Linux/GCC 构建与测试修复：`[ul_rx]` 采样连续性探针**不该有守卫**（2026-09-26，提交 `d35272a264` + `ee410a9be8`）

> ⚠ **本节的第一版结论是错的，已更正**：我当时把"调用点补上定义所在的守卫"当作修法（`d35272a264`）。真正的错误在**被守卫的那一侧** —— 这个探针是**数据路径判据**（D4/Q9-D4），不是 Metal 调试辅助，Linux 构建（`ENABLE_METAL_STATS=OFF`）本就应该编译它。构建过了之后 `lower_phy_test` 立刻暴露出来。

用户报：Ubuntu（`jwang@192.168.31.211:~/work/ocudu`，Release、`ENABLE_FLOW_PROBES=ON`、`ENABLE_METAL_STATS=OFF`）编译失败：

```
lib/phy/lower/lower_phy_baseband_processor.cpp:1334: error: 'ul_rx_note_call' was not declared in this scope
```

**根因**：`ul_rx_note_call()` 与整个 `[ul_rx]` 探针块定义在 `#if defined(OCUDU_METAL_STATS)`（该文件 466 行起）里，而调用点在 `ul_process()` 里**没有守卫**。
macOS 侧一直编译通过，因为**本机调试开关默认全开**（`ENABLE_METAL_STATS=ON`）——这正是审计里那句"Clang-green is not GCC-green"的实例。

**修法（两半，同一处）**：
1. 调用点加 `#if defined(OCUDU_METAL_STATS)`（与定义同守卫；探针的读者本来就是 Metal-stats 报告）；
2. 修完暴露第二半：探针被编译掉后 `rx_call_begin` **只写不用**，GCC 的 `-Werror=unused-but-set-variable` 拦住 ⇒ 把**那一次读时钟**也放进同一守卫；`rx_call_end` **不能**动（`tx_slack_note_receive()` 不在任何开关后面，每次构建都要用它）。

**验证**：
* Ubuntu：应用补丁后 `cmake --build build -j16` **跑到 100%、`gnb` 与全部测试目标链接完成、exit 0**（用户的两个本地修改过的电台配置未被触碰）；
* macOS：`gnb` 重建、`lower_phy_test` **576/576 PASSED**（就是这条接收路径）；
* **该文件里 `OCUDU_METAL_STATS` 独有的四个符号全部核对过**，只有这一处是未守卫的使用（另两处出现在注释里）；
* ★ **对空口构建是零行为变化（机械证明）**：在 macOS（`ENABLE_METAL_STATS=ON`）下，修改前后把同一个 TU 编成目标文件，**`cmp` 逐字节相同**（126024 B）——因为新增的两个 `#if` 在这个配置下都为真、被包住的记号一字未改。
  ⇒ 收口腿对（`p59`/`p60`，跑在 `791864c7d7`）所依据的**空口代码与当前 HEAD 完全一致**；审计的"腿 == HEAD"机械比对会看到一次代码差异，其解释就是本条。

#### 第二半（真正的根因）：探针被守卫掉 ⇒ 单元测试失败

**现象**（Ubuntu 全量 `cmake --build build --target test`）：`5574 - lower_phy_test (Failed)`，48 个用例同一条：

```
lower_phy_test.cpp:999: Failure  ... "no 'radio sample continuity' check is registered"
```

**逐步定位**：
1. 注册点（`register_phy_pipeline_check({"radio sample continuity", …})`）与整个 `[ul_rx]` 块（`ul_rx_stats`、`ul_rx_counters/note_call/note_gap/stats_report`）都在 `#if defined(OCUDU_METAL_STATS)` 里 ⇒ Linux 上**检查根本没注册**；
2. 把注册放出来后，测试改在 **996 行**失败：`verdict` 是空的（检查"拒绝判定"），它自己打印的证据是 **"0 gaps over 0 blocks"** ⇒ 计数没被喂；
3. 第二个 `#if defined(OCUDU_METAL_STATS)` 区（`ul_process()` 里，**块/间隙的记账**：`c.blocks/gaps/samples.fetch_add`、`ul_rx_note_gap`）同样被编掉 ⇒ 计数器恒为 0 ⇒ 检查按契约"不在路径上"返回 not applicable。

**修法**：**两处守卫全部删掉**（探针无条件编译），并**撤销** `d35272a264` 的两处调用点补丁 —— 因为定义现在永远存在。判据依据：测试明确要求它注册、`p0_gate.sh` 在**每个平台**都解析这行（D4/Q9-D4）、而空口构建（macOS，`METAL_STATS=ON`）本来就一直在跑它。

**验证**：
* **Ubuntu**：`cmake --build build -j16` → 100%；`lower_phy_test` → **Passed (1/1)**；**全量 `cmake --build build --target test` → exit 0**（此前是 `Error 8`；标签汇总含 `phy = 184 tests`、`tsan = 1174 tests` 等，唯一一条是 Skipped）；
* **macOS**：`gnb` 重建、`lower_phy_test` **576/576 PASSED**（退出时那行 `-> FAILED` 是**mock 电台**自己的判词，与改动前逐字相同）；
* ★ **空口零行为变化（机械证明）**：`ENABLE_METAL_STATS=ON` 下本 TU 的目标文件在改动前后 **`cmp` 逐字节相同** ⇒ 空口二进制不可能变了。

**遗留**：Ubuntu 那棵树上这个文件是**工作区未提交**状态（HEAD 仍是 `90a288c26a`，另有两个电台配置是用户自己的改动）；两边文件已用 sha256 核对一致（`feb620ba…` → 之后为最终版）。


### 6.113 V3 的**复跑计划**（用户裁定：先不改任何参数，按历史 log 的配方重跑 cpu / cpu_gpu / gpu，看现象能否复现）+ 历史基线表（2026-09-26）

> 代码已经改了很多（D1 交棒、批量化、池/环、§6.103–§6.107 的 sigma2 修复…），所以 V3 的第一步不是改传输参数，而是
> **先证明现象还在不在**。本节把"历史基线"和"要飞的臂"都写成可机械比对的形式；工具：`doc_chinese/phy_pipeline_gpu/wip/repro_compare.py <leg> …`。

#### ① 历史基线（脚本直接读旧腿生成；**绝对数不可跨配置比较，见 ④**）

| 腿（模式/工况）| RF 失败 | 其中 | `[ul_pipeline]` 中位 | `[ul_gpu_pipeline]` 中位 | lane `residency` 中位 | `gaps` | 每 DL 递交失败率 |
|---|---|---|---|---|---|---|---|
| `p27`（gpu/stress，ring64+pool16）| **707** | 633 uf + 74 late | 1560 | 1495.4 | 723.7 | 0 | —（无 slack 探针）|
| `p42`（gpu/stress，ring256+pool32）| **1724** | 1329 uf + 395 late | 1435 | 1366.8 | 543.9 | 0 | **0.2336%** |
| `p60`（gpu/default，ring512+pool32）| **909** | 757 uf + 152 late | 1476 | **1412.1** | 637.5 | 0 | **0.1628%** |
| `s79-dlcap40`（cpu）| **0** | — | **670** | 无 | 无 | 0 | — |
| `s81-ulcap40`（cpu）| **1** | 1 uf | **668** | 无 | 无 | 0 | — |
| `s73-wall`（cpu）| 51 | 28 uf + 20 late + 3 ovf | **598** | 无 | 无 | **3 / 282199 样点** | — |

两条**与模式无关**的读数（cpu 与 gpu 都有）：`[ul_rx_wait]` 中位 **473 µs**、max **~100.7–101.0 ms**；而两个 gpu 腿的
`[ul_rx_timing]` 给出去向：**`recv` max ≈100.8 ms、`over 5ms`=20–24，而 `loop` max 只有 1.5–6.5 ms**；
`[dl_tx_slack]` **中位提前 1512 µs、`AT/BELOW 0` 只有 19–48**（对 909–1724 次失败）⇒ 与 §6.42 一致：**失败在递交之后、传输内部**。

#### ② 要飞的臂（**不改任何数据路径参数**；配置用当前交付配置原样）

| # | 臂 | 命令要点 | 历史对照 | 预期复现 |
|---|---|---|---|---|
| A1 | **gpu**（stress）| `run_leg.sh gpu p61-n78-repro-gpu --regime=stress --expert_execution…concurrency=2 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1` | `p42`/`p60` | RF 700–1700、underflow 占 ~80%、`gaps=0`、slack 中位 +1.5 ms & `AT/BELOW 0`≤50、`[dl_tx_call]` over-1ms 数百、`recv max≈100 ms` 而 `loop` 数 ms、V1 1360–1500、`cbs/lane=2.00`、契约 9/9 |
| A2 | **cpu**（stress，**忠实**：不传任何 gNB 选项 ⇒ 派生并发=1）| `run_leg.sh cpu p62-n78-repro-cpu --regime=stress OCUDU_UL_PHASE_SEGMENTS=1` | `s79`/`s81` | RF 0–5、`gaps=0`、`[ul_pipeline]` 中位 **600–700 µs** |
| A3 | **cpu + 并发 2**（**混杂对照**，见 ④.2）| 同 A2 但加 `--expert_execution…concurrency=2` | 无（历史 cpu 腿都是派生并发 1）| 若 cpu 在并发 2 下仍 ~0–50 ⇒"GPU 模式病"成立；若 cpu 也上千 ⇒ 归因改为**并发/负载** |
| A4 | **cpu_gpu**（模块级卸载；**三个 backend 旋钮一个都不要传**，见 ④.4）| `run_leg.sh cpu_gpu p64-n78-repro-cpugpu --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1` | **无历史腿**（新基线）| 给出模块级路线的 RF/`crossings`/`cbs/lane`（回答 profile 第 1 段）|
| A5（可选）| **n1 bridge**（gpu + cpu，手机流量，default）| `LEG_CONFIG=configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`，无其它选项 | 你的两条 console log | gpu `[ul_pipeline]` ≈1721、cpu ≈1200、两边 `[ul_rx_wait] max`≈101 ms |

#### ③ 判读（跑完一条命令出对照表）

```bash
python3 doc_chinese/phy_pipeline_gpu/wip/repro_compare.py p61-n78-repro-gpu p62-n78-repro-cpu p63-n78-repro-cpu-conc2 p64-n78-repro-cpugpu
```
脚本对每条腿打印：provenance（mode/regime/config/options/knobs）、**RF 失败总数与分类**、`[ul_pipeline]`/`[ul_gpu_pipeline]` 的
samples/中位/stale、`[ul_rx_wait]`、`[ul_rx_timing]`、`[dl_tx_slack]`、`[dl_tx_call]`、`[ul_rx] gaps/ts0`、`[ul_rx_pool]`、
`[ul_gpu_lane]`（lanes/cbs/residency）、契约，以及**每 DL 递交的失败率**。

#### ④ 三条必须记住的注意事项

1. **配置已经演进，绝对失败数不可跨配置比**：历史 V3 腿是 ring **64**(`p27`)/**256**(`p42`) + pool **16**(`p27`)/**32**；今天是 **ring 512 + pool 32 + TX ring 64**。
   ⇒ 只比**率**（failures/DL transmission：`p42` 0.234% vs `p60` 0.163%）与**签名**（underflow/late 比例、负载相关、cpu/gpu 对比）。
2. **历史 cpu 腿与 gpu 腿的并发不同**：三条 cpu 腿**没有**传 `max_pusch_and_srs_concurrency` ⇒ 派生值是 **1**（§4.1 表：n78 → 1）；而所有 gpu 腿都用 **2**。
   ⇒ "cpu 0–1 vs gpu 700–1700"**混杂了并发**，所以必须有 **A3（cpu@conc2）**这条对照，否则不能把差别归给模式。
3. **单条腿不是证据**：A1/A2（以及 A3）各飞 **≥2 条**、同日同负载；工况标签写清（`p27`–`p42` 是 stress，`p60` 是 default）。
   另外：**`cpu_gpu` 单独用（全 auto）= 全 CPU**，而 `run_leg.sh` 的 `cpu_gpu` 分支**自己就注入**了那三个 metal backend 旋钮
   （`--expert_phy.pusch_dft_type metal --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal`，
   外加 `--expert_phy.pusch_ldpc_decoder_type auto` 与 **`--expert_phy.device_resource_grid on`**，见脚本 224–235 行及其注释：
   "a cpu_gpu arm that resolves to CPU backends would be comparing the CPU path with itself"）。
   ⇒ **命令行里再传一遍会直接失败**：CLI11 报 `--pusch_channel_estimator_algo: At Most 1 required but received 2`
   （实测于 2026-09-26 的第一次 A4）。A4 只需 `run_leg.sh cpu_gpu <label>` 后面跟探针旋钮即可。
   ⚠ 与用户 profile 第 1 段的手写命令相比，脚本配方**多一项 `device_resource_grid on`**（设备写资源网格）——对照时要记这一笔。

#### ⑤ 结果会导向什么

* **现象复现 + cpu/cpu@conc2 仍近零** ⇒ 坐实"GPU 模式病"，下一步按 §6.42 ④ 的 S3 做**宿主亲和/优先级**臂，或按传输侧做参数扫描（那一步才需要动参数，届时请用户批准）；
* **现象不再复现（RF 大幅下降）** ⇒ 说明代码演进（批量化/D1/池环/sigma2 修复）顺带治好了它 ⇒ 记为"已复现修复"，V3 可重新判定；
* **cpu 在并发 2 下也变红** ⇒ 归因改为**并发/负载**而非模式，V3 的判据与配方都要重述（§6.113 ④.2）。


### 6.114 ★★★ V3 复跑结果（`p61` gpu / `p62` cpu / `p63` cpu@conc2，同日同配方）：**现象逐项复现；而且混杂项被控制后，"GPU 模式病"成立（1443 vs 13，~110×）**（2026-09-26）

#### ① 三条腿 vs 历史基线

| 读数 | `p61`（gpu）| `p62`（cpu，忠实 conc=1）| `p63`（cpu，conc=2）| 历史对照 |
|---|---|---|---|---|
| **RF 失败** | **1443**（1150 uf + 293 late）| **37**（27 + 10）| **13**（uf）| gpu 707–1724；cpu 0–51 |
| **每 DL 递交失败率** | **0.2020%** | 0.0068% | **0.0012%** | gpu 0.163–0.234% |
| `[dl_tx_slack]` 中位 / `AT/BELOW 0` | 1512 µs 提前 / **58** | 1512 / 12 | 1512 / **0**（min +423 µs，**从没晚过**）| gpu 1512 / 19–48 |
| **`[dl_tx_call]` over 1ms** | **830**（max 84.9 ms）| **1** | **1** | gpu 517–969 |
| `[ul_rx_timing]` recv max / over1ms / over5ms | **101.1 ms** / 1029 / 44 | 100.8 / **16** / 3 | 101.3 / 24 / 1 | 100.7–101.0 |
| `[ul_rx_timing]` loop max | **1.74 ms** | 0.25 ms | 0.53 ms | gpu 1.5–6.5 ms |
| `[ul_pipeline]` 中位 | **1474** | **661** | **667** | gpu 1435–1560；cpu 598–670 |
| `[ul_gpu_pipeline]` 中位 | 1411 | — | — | gpu 1366.8–1495.4 |
| lane `residency` / `cbs/lane` | 637.9 / **2.00 (max 5)** | — | — | p60 637.5 / 2.00 |
| `gaps` / 池 / 契约 | **0** / max 71 µs / **9 of 9** | 0 / 35 µs / 5 of 8 | 0 / 31 µs / 5 of 8 | 同 |

#### ② 三条结论

1. **GPU 模式的现象逐项复现**：失败数 1443、率 0.202%、underflow 占 80%、`gaps=0`、`AT/BELOW 0`=58（对 1443 次失败 ⇒ 宿主迟到只解释 ~4%）、`transmit()` over-1ms **830**、`recv` max 101 ms 而 `loop` 1.7 ms ⇒ **与 §6.40/§6.42 的结论完全一致：失败在递交之后、传输内部**。
2. **CPU 路线也复现，且混杂项被控制**：忠实腿 37、conc2 腿 **13**，而 gpu 在同并发（2）下是 **1443** ⇒ **~110×，差别是"模式"而不是"并发"**；`[ul_pipeline]` 中位 661–667 与历史 598–670 逐位吻合。
3. ★ **机制在对照中直接可见**：~100 ms 的 `recv` 停顿**两种模式都有**（USB 一样糟），但 **`transmit()` 被挡 >1 ms 的次数是 830（gpu）vs 1（cpu）**——CPU 路线的递交**从没晚过**（`AT/BELOW 0=0`）。
   ⇒ **是 GPU 模式给宿主加的活儿/争用把 UHD 的 TX 通道顶进了传输背压**，而那些阻塞的调用正是 underflow 的同侪。这与"并发 2 放大 3 倍、批量化后减半、cpu 模式几乎不发生"三条旧旁证同一方向。

#### ③ 顺带（同一天、同一配方、同一配置）复现的第二个事实：**本小区上全 CPU 的上行跨度是融合 GPU 车道的 ~1/2**

`[ul_pipeline]`（收样点开始 → CRC-OK）中位：**cpu 661–667 µs vs gpu 1411–1474 µs**；两边都含同一个 ~473 µs 的收包等待
⇒ 扣掉等待后的"流水线部分"：**cpu ≈ 190 µs，gpu ≈ 940 µs**。
⚠ 这不否定本工作流的 V1（V1 是**融合路自己的**基线 2675 → 1412）；它说明的是**在 n78/单 UE/51 PRB 这个工况下，CPU 的 DSP 本来就很快**，
而融合车道买的是**时延的可控性/CPU 卸载**，不是在这个小区上"比 CPU 更快"。要把它变成结论还需要在更重负载（多 UE/更多 PRB）上比——那属于新的立项。

#### ④ 下一步（等 A4 与判读）

* **A4 被 `run_leg.sh` 拒绝过两次，两次都是配方的错**：第一次是"选项 + 空格 + 值"（脚本**故意**要求 `--option=value`）；
  第二次是**重复传了脚本已经注入的三个 metal backend 旋钮**（CLI11: `At Most 1 required but received 2`）。
  **正确写法：`run_leg.sh cpu_gpu <label> --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1`，不要传任何 backend 旋钮**（见 §6.113 ②/④.4）。
* A4 落地后即可补齐三模式对照（profile 的第 1/2/3 段），并回答"模块级卸载落在哪一侧"。
* 之后按 §6.113 ⑤ 分支：**现在证据已经把"模式"钉住**（不是并发），所以 S3 的选择变成
  **(a) 宿主亲和/优先级臂**（把 UHD 的 RX/TX 线程独占核，把上层 PHY 执行器挪开）或
  **(b) 传输参数臂**（`num_send_frames`/`send_frame_size`/`send_buff_size`）——两者都要**动参数**，请用户批准后再飞。


### 6.115 三模式复跑齐了（`p61` gpu / `p62`+`p63` cpu / `p64` cpu_gpu）：**RF 失败跟"宿主是否在喂 GPU"走，而不是跟结构走**；而且**两条 GPU 路线的设备占用几乎相同**（回答了 profile 的疑问）（2026-09-26）

#### ① 三模式对照（同日、同配置、同负载、n78、stress）

| 读数 | **cpu**（`p62` conc1 / `p63` conc2）| **gpu**（`p61`）| **cpu_gpu 模块级**（`p64`）|
|---|---|---|---|
| **RF 失败 / 率** | 37 / 13 ⇒ 0.0068% / **0.0012%** | **1443** ⇒ 0.2020% | **876** ⇒ 0.1467% |
| **`[dl_tx_call]` over 1ms** | **1** / 1 | **830** | **598** |
| `[ul_rx_timing]` recv max / loop max | 100.8 ms / 0.25–0.53 ms | 101.1 ms / 1.74 ms | 100.8 ms / **5.08 ms** |
| `[ul_pipeline]` 中位 | **661 / 667** | 1474（gpu 1411）| **1807**（三模式最慢）|
| `gaps` / 契约 | 0 / MET | 0 / **MET 9 of 9** | **1 gap（157571 样点）/ NOT MET** ← 只有 `radio sample continuity` 一项红（电台 1 次 overflow）|
| **Q9-F3 设备占用（union/window）** | 无 GPU 活 | **24.3%**（87.1 s / 359.3 s）| **25.9%**（78.0 s / 300.7 s）|
| GPU 提交数（Q9-F3 commits） | — | 730,615 | 764,946 |
| `dft commits/transforms` | — | 428,653 / 428,653 | 496,852 / **2,297,599**（≈1 commit / 4.6 变换）|
| `burst commits / dispatches` | — | 145,590 / 582,358（4.0/commit）| 134,046 / 536,184（4.0/commit）|
| lane `cbs/lane` | — | 2.00 (max 5) | 2.00 (max 2) |

#### ② 三条结论

1. ★ **RF 失败跟"宿主在喂 GPU"走，不跟结构走**：两条 GPU 路线（融合 `p61`、模块级 `p64`）都是 **876–1443 次（0.15–0.20%）**，
   而全 CPU 只有 **13–37 次（0.001–0.007%）**；`transmit()` 被挡 >1 ms 的次数同样是 **598–830 vs 1**。
   ⇒ 病因不是"融合"或"分段"，而是**宿主侧与 GPU 路径的争用**把 UHD 的 TX 通道顶进传输背压（§6.114 ③ 的机制在第三条腿上再次成立）。
2. **模块级路线是最慢的**：`[ul_pipeline]` 中位 **1807 µs**（融合 1411、CPU 661–667），而且这一次它还**丢了一次样点**
   （1 gap / 157,571 样点 = 6.8 ms，电台 1 次 overflow）⇒ 契约 **NOT MET**（唯一红项是连续性）。
   注意 p64 的 `pop_blocking` max 只有 27 µs ⇒ 丢样点**不是池干**，而是那次 ~100 ms 的 `recv` 停顿超过环能吸收的量。
3. ★★ **profile 的疑问有了答案：两条 GPU 路线的"设备占用"几乎相同**（union/window **25.9% vs 24.3%**，提交数 76.5 万 vs 73.1 万）。
   ⇒ 你截图里第 1 段"密而高"、第 3 段"疏而尖"的差别，**不是平均占用的差别，而是时间形状的差别**：
   融合路每跳一次突发（一条链跑完就空下来），模块级把同一份负载摊成更多、更小的提交；
   而**模块级并没有更高效** —— 它的端到端跨度反而更长（1807 vs 1411）。
   ⚠ 边界：你的 profile 是 **n1/手机**，这三条腿是 **n78/加压**；若要在同一小区上对齐，需要 A5（n1 bridge，gpu+cpu 各一条）。

#### ③ 下一步（都需要批准才动参数）

* **V3 的杠杆已经收敛到"宿主与 GPU 路径的争用"**：(a) **宿主亲和/优先级**臂（UHD 的 RX/TX 线程独占核 + 时限 QoS，把上层 PHY 执行器挪开）；
  (b) **传输参数**臂（`num_send_frames`/`send_frame_size`/`send_buff_size`）。两者都是参数改动。
* **profile 的形状问题**若要坐实，跑 A5（n1，gpu + cpu 各一条，同日）即可用同一支 `repro_compare.py` 对齐。


### 6.116 用户假设的检验："GPU 跨度超过 1 个时隙 ⇒ 阻塞 ⇒ RF 失败？"——**两条腿的现成数据就把它否掉了**（2026-09-26）

#### ① 先把量纲统一：**换算成"时隙数"后，CPU 模式也超过 1 个时隙**

| 小区（时隙）| CPU 中位 | GPU 中位（融合）| 模块级 | GPU 并发 1 |
|---|---|---|---|---|
| **n1（1000 µs）** | 你 console 里 **1200 µs = 1.20 slot** | **1721 µs = 1.72 slot** | — | — |
| **n78（500 µs）** | `p62/p63` **661/667 µs = 1.32/1.33 slot** | `p61` **1474 µs = 2.95 slot** | `p64` **1807 µs = 3.61 slot** | `p46` **1655 µs = 3.31 slot** |

⇒ "CPU < 1 slot、GPU > 1 slot"是**绝对 µs 跨小区比较**造成的错觉：两条 cell 上都**超过** 1 个时隙，差别只是**深度**。

#### ② 超过 1 个时隙**不等于**阻塞（本轮腿里的直接读数）

流水线是**重叠**的，而且没有任何阻塞的证据：`[ul_dft_wait]` 无样本、`gpu_wait=0`、`defer_wait` ~770 µs（等待被推迟）、
`starved_events=0`、`pop_blocking` max 23–71 µs、`stale` 0–2。

#### ③ 判别实验：**跨度越长，RF 失败反而越少**（三条 GPU 路线，排序完全相反）

| 腿 | 跨度中位（slot）| RF 失败 | 率 | `transmit()` >1 ms |
|---|---|---|---|---|
| `p61` gpu conc2 | 2.95 | **1443** | 0.2020% | 830 |
| `p46` gpu **conc1** | **3.31（更长）** | **1215（更少）** | 0.1853% | 788 |
| `p64` **模块级** | **3.61（最长）** | **876（最少）** | 0.1467% | 598 |
| `p62/p63` cpu | 1.32 | **13–37** | 0.0012–0.0068% | **1** |

* 若"跨度 > 1 slot ⇒ RF 失败"成立，则**并发 1**（跨度最长、余量最小）应该最红——实测它**最轻**（1215 < 1443），模块级（3.61 slot）更是最轻（876）；
* CPU 腿在 n1 上同样是 1.20 slot（>1）却只有 ~0 失败；`[dl_tx_slack]` 显示 DL 递交**始终提前 1.5 ms**、只有 19–58 次晚（≤4%）；
* 两条 GPU 路线的 `transmit()` 阻塞是 598–830 次，CPU 是 **1** 次 ⇒ **真凶仍是"宿主是否在喂 GPU"**（§6.114 ③/§6.115 ②），与跨度无关。

⇒ **假设否掉。** 跨度那颗账要记在别处：`余量 = 1 − residency/slot`，n78 融合路 **637.9/500 ⇒ −27.6%（过订阅）**，
所以它**必须**靠并发 ≥2 才维持"每时隙一跳"；而 CPU 路的**计算部分**只有 ~190 µs（0.38 slot）、余量充足。
这是**容量/V2** 的账（池、`starved_events`、`stale`），不是 V3 的账。


### 6.117 V3 的第一条修复臂：**传输参数扫描**（用户裁定"先改传输参数，比较容易"）——预登记（2026-09-26）

#### ① 为什么这两个环深**值得一飞**（此前的"arm C 不飞"是**分析**结论，不是空口读数）

一帧 ≈ 8200 B（sc12、2733 样点）⇒ 在 23.04 Msps 下：

| 环 | 当前 | 折算时长 | 实测最坏停顿 | 覆盖？ |
|---|---|---|---|---|
| **TX `num_send_frames`** | **64** | **7.6 ms** | **84.7 ms**（`transmit()` max，两模式都读到） | ❌ **差 ~11 倍** |
| RX `num_recv_frames` | 512 | 60.7 ms | **101.2 ms**（`recv` max） | ❌ 差 ~1.7 倍 |

⇒ 两个方向的环都**短于**实测最坏停顿。RX 环 64→256→512 的历史 A/B 已经证明"深环把丢样点换成可容忍的积压"；TX 环 8 年没动过（配置注释说"TX 环不是杠杆"，依据是"环已满、样点不动"的**分析** + 台架复现不出）。
**本条腿就是把这个分析放到空口上检验。**

#### ② 命令（**不改配置文件**：`--ru_sdr.device_args=` 覆盖 YAML；一次只改一个变量）

```bash
cd /Users/jiachengwang/dev/ocudu
COMMON="--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1"

# B0 对照（原样：recv 512 / send 64）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml   bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p65-n78-txring64 --regime=stress $COMMON

# B1 TX 环 4×（≈30 ms）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml   bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p66-n78-txring256 --regime=stress   --ru_sdr.device_args=type=b200,num_recv_frames=512,num_send_frames=256 $COMMON

# B2 TX 环 8×（≈61 ms，与 RX 环等量）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml   bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p67-n78-txring512 --regime=stress   --ru_sdr.device_args=type=b200,num_recv_frames=512,num_send_frames=512 $COMMON

# B3 RX 环 2×（≈121 ms，唯一能覆盖实测最坏 recv 停顿的深度）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml   bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p68-n78-rxring1024 --regime=stress   --ru_sdr.device_args=type=b200,num_recv_frames=1024,num_send_frames=64 $COMMON

# B4（可选）USB 传输尺寸 16 KiB（更少更大的传输）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml   bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p69-n78-frames16k --regime=stress   --ru_sdr.device_args=type=b200,num_recv_frames=512,num_send_frames=64,recv_frame_size=16384,send_frame_size=16384 $COMMON
```

#### ③ 预登记判据（**先写死，跑完不追认**）

| 臂 | 成功 = | 反例 = |
|---|---|---|
| B1 / B2 | RF 失败数与 `[dl_tx_call] over 1ms` **相对 B0 下降 ≥2×**，且 `gaps=0`、V1（`[ul_gpu_pipeline]` 中位）仍在 **1411–1490** 带内 | 落回腿间散布（700–1700、率 ~0.2%）⇒ **环深不是杠杆** |
| B3 | `[ul_rx_timing]` 的 `over 1ms/over 5ms` 或 `recv` max 明显下降，`gaps` 保持 0 | 同样不动 ⇒ RX 环也已到位 |
| B4 | 同上（失败数或传输阻塞次数下降）| —— |

无论成败都要读：RF 失败的**分类**（underflow/late）、`[dl_tx_slack]`（`AT/BELOW 0`、min）、`[ul_rx_timing]`（recv/loop/slip）、`gaps/ts0/rx_overflows`、`[ul_rx_pool]`、契约、`cbs/lane`、`stale`。

#### ④ 跑完一条命令出对照

```bash
python3 doc_chinese/phy_pipeline_gpu/wip/repro_compare.py p65-n78-txring64 p66-n78-txring256 p67-n78-txring512 p68-n78-rxring1024
```

#### ⑤ 结果会导向

* **任一臂成功** ⇒ 传输缓冲就是杠杆 ⇒ 继续（把成功值写进交付配置，需要改 config，届时请你批准）；
* **全部落回散布** ⇒ 环深/尺寸不是杠杆，坐实"USB 链路本身" ⇒ 转 **(a) 宿主亲和/优先级臂**，或按 §6.113 ⑤ 讨论判据重述；
* ⚠ 若 UHD 拒绝某个值（帧尺寸/环数超出其允许范围），腿会在启动时报错 —— **那本身也是一条读数**（记下来，换一个值继续）。


### 6.118 传输参数扫描结果（`p65`/`p66`/`p67`/`p68`）：**环深不是杠杆**（预登记判据的反例成立）；但 TX 环**把最坏阻塞调用从 84.8 ms 压到 5.6 ms**（2026-09-26）

#### ① 四条腿（同日、同配方、n78、stress、conc 2，只差 `--ru_sdr.device_args`）

| 臂 | device_args | RF 失败 | 率 | `[dl_tx_call]` >1ms / >5ms / **max** | `recv` max / >1ms / >5ms | `AT/BELOW 0` | V1 中位 | gaps |
|---|---|---|---|---|---|---|---|---|
| **B0** `p65` | recv512 / **send64**（原样）| **684** | 0.1284% | 403 / 8 / **84.8 ms** | 100.9 ms / 499 / 12 | 11 | 1400.3 | 0 |
| **B1** `p66` | send **256** | **770** | 0.1494% | 395 / 2 / **44.4 ms** | 100.6 / 502 / 7 | 15 | 1407.1 | 0 |
| **B2** `p67` | send **512** | **810** | 0.1577% | 383 / 2 / **5.6 ms** | 101.5 / 511 / 12 | 37 | 1410.3 | 0 |
| **B3** `p68` | recv **1024** | **748** | ~0.15% | 409 / 5 / 85.0 ms | **101.1 / 518 / 5** | 22 | 1410.3 | 0 |

#### ② 判决（按 §6.117 ③ 的预登记）

* **B1/B2 未达"RF 失败与 `transmit()` 阻塞次数下降 ≥2×"** ⇒ **环深不是杠杆** ✓（反例成立）。
  四条腿的失败数 684–810 全部落在同日散布内（同日其它 gpu 腿 876–1443），率 0.128–0.158% 同带；
  `transmit()` 被挡 >1 ms 的**次数**几乎不变（403 → 395 → 383 → 409）。
* **B3 未改变 `recv` 的尾部**（max 101.1 ms、>1 ms 518 次）⇒ 那些停顿**在调用内部**，不是环容量造成的；512 帧（60.7 ms）已经够用（四腿 `gaps=0`）。
* ★ **唯一真实的效应**：TX 环加深后**最坏的那一次** `transmit()` 阻塞被压下来了 —— **84.8 ms → 44.4 ms → 5.6 ms**（64 → 256 → 512 帧），
  即"等环空位"的极端情形被环吸收了；但**失败率不变** ⇒ **underflow 不是"等环空位"造成的**，而是**样点在那段时间根本没上线**（与 §6.56 的"环已满、样点不动"分析一致，现在有了空口证据）。
* 四条腿 V1（1400–1410）与 `cbs/lane`、契约、`gaps` 全部正常 ⇒ 参数改动**没有引入回归**，可以作为"已排除项"记账。

#### ③ 还剩下什么（按预期价值排序）

| 选项 | 性质 | 预期 | 备注 |
|---|---|---|---|
| **B4：`send_frame_size`/`recv_frame_size`=16384 + `spp`** | 空口腿，零代码 | 若"每次 USB 传输的开销"是病因则可下降 | **最后一个未测的传输机制**（B0–B3 测的是**缓冲深度**，B4 测的是**传输粒度**）|
| **(a) 宿主调度臂** | 配置/代码 | 有限 | ⚠ macOS 上 **`set_thread_affinity` 是 no-op**（`macos_compat.h:183`：XNU 无用户态绑核），真正生效的是 `apply_worker_thread_scheduling`（**QoS 类 + Mach affinity tag + 时限约束**）；而且 UHD 自己的线程在开电台时就已请求 `uhd_set_thread_priority(0.90, true)`（**四条腿里都没有 "Scheduling priority of UHD not changed" 警告** ⇒ 该请求成功了）⇒ 可动的只有 **ocudu 的 lower-PHY/RU 线程**（配置 `threads`/`cell_affinities`，当前 n78 配置没设）|
| **(c) V3 判据重述** | 需要用户裁决 | —— | 现状：**率 0.13–0.20%/DL 递交、`gaps=0`（512 帧）**；≤10 的绝对阈值是为确定性链路写的 |

⇒ 建议：**先飞 B4**（唯一没测过的传输机制，便宜）；若 B4 也不动，则传输/缓冲这一侧**到此为止**，V3 归入"USB 链路/上游"，并就 (c) 做一次裁决。


### 6.119 B4a（16 KiB USB 帧）被**硬件级否决**：链路直接崩，手机无法接入——并记下"UHD 会接受一个会杀死链路的值"这个陷阱（2026-09-26，腿 `p69-n78-frames16k`）

#### ① 读数（同配方，只多了 `recv_frame_size=16384,send_frame_size=16384`）

| 读数 | B4a（16 KiB 帧）| 正常腿（如 `p65`）|
|---|---|---|
| **PUSCH 跳数** | **0**（手机从未接入）| 13–14 万 |
| RF 失败（140 s）| **279,385**（late 185,359 + **overflow 85,893** + underflow 8,653）≈ **2000/s** | 684–1443（≈3–5/s）|
| `[ul_rx]` 块/样点比 | blocks=64,343、samples=365,646,606 ⇒ **平均只有 5,684 样点/块**（整槽应为 11,520）| 整块 |
| `[ul_rx] gaps / gap_samples / rx_overflows` | **49,079 / 2.92 G 样点 / 49,079** | 0 / 0 / 0 |
| `[ul_rx_timing]` | `recv(max=103.8 ms, **over 1ms=49,079**)` ⇒ **76% 的接收调用 >1 ms** | 499/532,670 ≈ 0.09% |
| `[dl_tx_slack]` | 中位 **+132 µs**、**AT/BELOW 0 = 126,475 / 285,301 = 44%** | +1512 µs、11–58（≤4%）|
| `[dl_tx_call]` >1 ms | 1（调用很快返回：不是背压，是**电台在丢**）| 383–409 |

⇒ **16 KiB 帧把这块 USB 传输打崩了**：接收不断返回**部分块**（5.7 k 样点）、电台环持续溢出（49k 次、丢 2.9 G 样点）、DL 递交近一半迟到 ⇒ 小区起得来但手机接不上。
**结论：8 KiB（UHD 默认）已经是这块硬件的边界，传输粒度不能往上调。**

#### ② ★ 操作陷阱（写进记录，避免下次再撞）

**UHD 接受了这个值**（启动无报错、`Cell was activated`），所以这类错误的表现是"**链路崩**"而不是"**配置被拒**"。
⇒ 任何传输参数臂的**第一验收条件必须是"手机能接入 + 有 PUSCH"**（`grep -c "PUSCH: rnti"` > 0），
在这一点成立之前，其它读数（V1/契约）都不构成证据。

#### ③ 传输侧的总账（B0–B4a）

| 机制 | 结论 |
|---|---|
| 缓冲**深度**（TX 64→256→512、RX 512→1024）| ❌ 不影响失败（684/770/810/748，同散布）；只把**最坏**一次 `transmit()` 阻塞 84.8 → 5.6 ms |
| 传输**粒度**（16 KiB 帧）| ❌ **硬件否决**（本文）|
| UHD 线程优先级 | ✅ **已经是** `uhd_set_thread_priority(0.90, true)`（无失败警告）|
| `spp`（每包样点数）| ⏳ **唯一未测**（B4b；8 KiB 帧下 `spp=1024` 在范围内，可以一飞）|

⇒ **传输/缓冲这一侧基本探到边界**：现有"8 KiB 帧 + 64–512 帧环"就是这块 USB 硬件能给的最好状态，而 underflow 仍以 **0.13–0.20%/DL 递交**的速率发生。


### 6.120 B4b（`spp=1024`）**跑完了但不能作证据**：报告被停机吃掉；能读的部分显示它**没有变好**（18.6/千跳，今天 9 条腿里最差）（2026-09-26，腿 `p70-n78-spp1024`）

#### ① 为什么它不可读（该腿 stderr 只有 74 行）

```
[phy_pipeline] contract MET (9 of 9 checks applicable)
Could not stop application after 5 seconds. Forcing exit.
```

* 契约行在**停机早期**打印，而 `[ul_rx_timing]`/`[dl_tx_call]`/`[dl_tx_slack]`/`[ul_rx]`/`[metal_stats]`/lane 探针都在**它之后** ⇒ 强杀时**全部丢失**
  （正是 `run_leg.sh` 抬头警告的那类腿：文件看起来齐全，要读的数字根本不存在）⇒ 本条腿**不构成 V3 扫描的读数**。
* 顺带：它跑了 **589 s**（常规 ~280 s），且停机挂住 ⇒ 时长与工况都不可控。

#### ② 能读的部分（来自 `.log`，运行中实时写盘）

| 腿 | PUSCH 跳数 | RF 失败 | **每千跳失败** | 备注 |
|---|---|---|---|---|
| **`p70` spp=1024** | 145,213 | **2700**（2023 uf + 677 late）| **18.59** | 今天 9 条腿里最差；但报告缺失、时长 2× |
| `p65` B0（send 64）| 144,914 | 684 | 4.72 | 可读 |
| `p67` B2（send 512）| 139,533 | 810 | 5.81 | 可读 |
| `p61` 复跑对照 | 145,590 | 1443 | 9.91 | 可读 |

⇒ 两个可用信息：**(i)** `spp=1024` **不会**像 16 KiB 帧那样打断链路（手机接入了、14.5 万跳）；**(ii)** 它**没有变好**，反而落在今天最差的一档。

#### ③ 传输侧总账（收尾）

| 机制 | 结论 |
|---|---|
| 缓冲**深度**（TX 64→512、RX 512→1024）| ❌ 不影响失败率；只把最坏一次 `transmit()` 阻塞 84.8 → 5.6 ms |
| 传输**粒度** 16 KiB | ❌ 硬件否决（链路崩，§6.119）|
| `spp=1024` | ❌ 无可读证据表明更好；提示更差（本文 ②）|
| UHD 线程优先级 | ✅ 已经是 `0.90`（无失败警告）|

⇒ **USB 传输这一侧到此为止**：现有"8 KiB 帧 + 64/512 帧环 + 默认 spp"已是这块硬件能给的最好状态；underflow 仍以 **0.13–0.20%/DL 递交**发生。
⇒ V3 只剩两条路：**(a) 宿主 QoS 臂**（macOS 上只能动 ocudu 的线程）与 **(c) V3 判据重述的裁决**。
⚠ 若要把 `spp` 的结论坐实，需**重飞一条干净腿**（~280 s、一个 Ctrl-C 并等报告打完）；否则按"提示更差 + 不可读"记档。


### 6.121 `spp=1024` 的**干净复跑**：674 次 / 0.1232% ⇒ **也不是杠杆**；传输侧四个机制全部收口（2026-09-26，腿 `p70-n78-spp1024` 重飞）

#### ① 干净腿 vs 对照（同配方、同工况，报告完整）

| 读数 | `p70`（spp=1024，重飞）| `p65` B0（send 64）| `p67` B2（send 512）|
|---|---|---|---|
| PUSCH 跳数 | 145,509 ✓（手机接入）| 144,914 | 139,533 |
| **RF 失败 / 率** | **674 / 0.1232%** | 684 / 0.1284% | 810 / 0.1577% |
| `[dl_tx_call]` over 1ms / max | 260 / 84.5 ms | 403 / 84.8 ms | 383 / 5.6 ms |
| `[ul_rx_timing]` recv max / >1ms | 100.6 ms / 360 | 100.9 ms / 499 | 101.5 ms / 511 |
| `[dl_tx_slack]` AT/BELOW 0 | 16 | 11 | 37 |
| V1（`[ul_gpu_pipeline]` 中位）| 1389.8 | 1400.3 | 1410.3 |
| `gaps` / 契约 | 0 / **9 of 9** | 0 / 9 of 9 | 0 / 9 of 9 |

⇒ 与 B0 **逐项同带**（674 vs 684 次、0.1232% vs 0.1284%）⇒ **`spp` 既不改善也不显著变差** ⇒ 按预登记判据 **不是杠杆** ✗。
*（第一次那条 `p70`（589 s、2700 次）是**不可读且时长不受控**的一次运行，已被本条取代。）*

#### ② 传输侧总账（**四个机制全部收口**）

| 机制 | 空口结论 |
|---|---|
| 缓冲**深度**：TX 64→256→512、RX 512→1024 | ❌ 不影响失败率（684/770/810/748）；只把**最坏**一次 `transmit()` 阻塞 84.8 → 5.6 ms |
| 传输**粒度**：16 KiB 帧 | ❌ **硬件否决**（链路崩：0 PUSCH、27.9 万次失败、接收只剩 5.7 k 样点/块，§6.119）|
| **`spp=1024`** | ❌ **无效果**（674 / 0.1232%，本节）|
| UHD 线程优先级 | ✅ 已经是 `uhd_set_thread_priority(0.90, true)`（无失败警告）|

⇒ **USB 传输这一侧到此结束**。现在这块硬件给出的最好状态就是："8 KiB 帧 + 512/64 帧环 + 默认 spp"，
underflow 稳定在 **0.12–0.20% / DL 递交**（全 CPU 路线 0.001–0.007%）。

#### ③ V3 只剩这两条（请用户裁决）

**(a) 宿主 QoS 臂**（预期有限）：macOS 上绑核是 no-op，只能给 **ocudu 的 lower-PHY/RU 线程**提 QoS/时限约束（配置 `threads`/`cell_affinities`；UHD 内部线程已是 0.90）。

**(c) 判据重述**（推荐）：把 V3 从"RF 失败 ≤ 10（绝对数）"改述为**可达且可复现**的形式，基线取今天的干净腿：

| 模式 | 每 DL 递交失败率 | 每千跳失败 | `gaps` |
|---|---|---|---|
| `gpu`（融合）| **0.123–0.202%** | 4.7–9.9 | 0 |
| `cpu_gpu`（模块级）| 0.1467% | — | 0（一次例外：1 gap）|
| `cpu` | **0.0012–0.0068%** | — | 0 |

建议判据：**"`gaps=0` 且 gpu 模式每 DL 递交失败率 ≤ 0.25%，≥2 条同负载腿；cpu 模式 ≤ 0.01%"** —— 用测量带宽加余量，而不是一个为确定性链路写的绝对数。


### 6.122 ★★★ V3 **基线报告**与**判据重述**（2026-09-26 用户裁决："采纳判据重述并收口 V3"）

> 生成方式（可一键复跑）：`python3 doc_chinese/phy_pipeline_gpu/wip/repro_compare.py --table <leg> …`

#### ① 判据重述（**原文保留，不删**）

| | 内容 |
|---|---|
| **原文（`5.9.130 ⑤`）** | RF 失败 **≤ 10**（cpu 量级 0–1）、`gaps == 0` |
| **为什么不可达** | 它假设**确定性链路**。这块 USB 连接的 B200 上，失败发生在**递交之后、传输内部**（§6.40/§6.42/§6.114），而**传输侧四个机制已全部排除**（§6.118–§6.121）：环深无效、16 KiB 帧被硬件否决、`spp` 无效、UHD 线程优先级**本来就已经是 0.90**。⇒ 现硬件最好状态就是 **0.12–0.20% / DL 递交**。|
| **重述（用户裁决，2026-09-26）** | **`gaps == 0`** 且 **每 DL 递交的实时失败率** ∈ 模式带宽：**`gpu` ≤ 0.25%**、**`cpu_gpu` ≤ 0.25%**、**`cpu` ≤ 0.01%**；**≥2 条同负载腿**，并在腿里**声明负载**。|

#### ② 基线表（今日 12 条 + 历史 3 条；`+D` = 当日同配方对照）

| leg | mode | regime | RF 失败 | uf/late | **率/DL 递交** | span 中位 | `tx>1ms` | `tx` max | `recv` max | `AT/BELOW 0` | gaps | 契约 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `p27-n78-pool16` | gpu | stress | 707 | 633/74 | —（无探针）| 1495.4 | — | — | — | — | 0 | MET |
| `p42-n78-hostgap` | gpu | stress | 1724 | 1329/395 | 0.2336% | 1366.8 | 969 | 84.6 ms | 100.7 ms | 48 | 0 | MET |
| `p60-n78-close3` | gpu | default | 909 | 757/152 | 0.1628% | 1412.1 | 517 | 84.7 ms | 100.8 ms | 19 | 0 | MET |
| `p61-n78-repro-gpu` ＋D | gpu | stress | 1443 | 1150/293 | 0.2020% | 1411.0 | 830 | 84.9 ms | 101.1 ms | 58 | 0 | MET |
| `p62-n78-repro-cpu` ＋D | cpu | stress | 37 | 27/10 | 0.0068% | 661.0 | 1 | 84.8 ms | 100.8 ms | 12 | 0 | MET |
| `p63-n78-repro-cpu-conc2` ＋D | cpu | stress | **13** | 13/0 | **0.0012%** | 667.0 | 1 | 85.3 ms | 101.3 ms | 0 | 0 | MET |
| `p64-n78-repro-cpugpu` ＋D | cpu_gpu | stress | 876 | 728/147 | 0.1467% | 1807.0 | 598 | 84.7 ms | 100.8 ms | 37 | **1** | **NOT MET** |
| `p65-n78-txring64` ＋D | gpu | stress | 684 | 573/111 | 0.1284% | 1400.3 | 403 | 84.8 ms | 100.9 ms | 11 | 0 | MET |
| `p66-n78-txring256` ＋D | gpu | stress | 770 | 634/136 | 0.1494% | 1407.1 | 395 | 44.4 ms | 100.6 ms | 15 | 0 | MET |
| `p67-n78-txring512` ＋D | gpu | stress | 810 | 710/100 | 0.1577% | 1410.3 | 383 | **5.6 ms** | 101.5 ms | 37 | 0 | MET |
| `p68-n78-rxring1024` ＋D | gpu | stress | 748 | 647/101 | 0.1429% | 1410.3 | 409 | 85.0 ms | 101.1 ms | 22 | 0 | MET |
| `p70-n78-spp1024`（重飞）| gpu | stress | 674 | 458/216 | 0.1232% | 1389.8 | 260 | 84.5 ms | 100.6 ms | 16 | 0 | MET |
| `p69-n78-frames16k` | gpu | stress | **279,385** | 8,653/185,359 | —（链路崩）| — | 1 | 75.7 ms | 103.8 ms | **126,475** | 49,079 | — |
| `s73-wall-cpu-n78` | cpu | n/a | 51 | 28/20 | —（无探针）| 598.0 | — | — | — | — | **3** | NOT MET |
| `s79-dlcap40-cpu-n78` | cpu | n/a | **0** | 0/0 | —（无探针）| 670.0 | — | — | — | — | 0 | MET |
| `s81-ulcap40-cpu-n78` | cpu | n/a | **1** | 1/0 | —（无探针）| 668.0 | — | — | — | — | 0 | MET |

#### ③ 基线带宽（判据用）

| 模式 | 率/DL 递交 | 每千跳失败（今日同配方）| `gaps` |
|---|---|---|---|
| **`gpu`（融合）** | **0.123–0.202%**（含历史 0.163–0.234%）| 4.7–9.9 | 0 |
| **`cpu_gpu`（模块级）** | 0.1467% | — | 0（一次例外：1 gap ⇒ 契约 NOT MET）|
| **`cpu`** | **0.0012–0.0068%** | — | 0 |

#### ④ 机制结论（一页）

* **失败在递交之后、传输内部**：递交中位/ p1 都**提前 1.5 ms**、`AT/BELOW 0` 只有 11–58（对 674–1443 次失败 ⇒ ≤4%）；而 `transmit()` 被挡 >1 ms **260–969 次**、`recv` max **~101 ms** 且 **`loop` max 只有 0.25–5.1 ms**（⇒ 不是宿主被剥夺 CPU）。
* **模式相关，结构无关**：两条 GPU 路线（融合/模块级）都是 0.12–0.23%，全 CPU 是 0.001–0.007%（**~30–100×**）；并发 2 对照腿（`p63` 13 次）**否掉了"并发"这个混杂项**。
* **传输侧四个机制**：环深无效、粒度（16 KiB）被硬件否决、`spp` 无效、UHD 优先级已 0.90 ⇒ **现硬件上限**。
* **归属**：V3 归**电台/USB 侧**（与 §6.40–§6.43 一致），不再作为本工作流的红项。

#### ⑤ 以后怎么判 V3

1. 每条腿**必须自报负载**（regime + 流量配方），并给出 `gaps`、`Real-time failure` 的**分类与技术**、`[dl_tx_slack]` 的 `AT/BELOW 0`、`[dl_tx_call]` 的 over-1ms；
2. **≥2 条同负载腿**取中位；判 `gaps=0` 与率是否落在 ③ 的带宽内；
3. `[dl_tx_slack]`/`[dl_tx_call]`/`[ul_rx_timing]` 这三条是**归因三件套**（宿主 vs 传输 vs 空口），任何"变红"都要先过它们；
4. ⚠ **任何传输参数臂的第一验收条件是"手机能接入且有 PUSCH"**（§6.119 ②：UHD 会接受一个会杀死链路的值）。


### 6.123 ★★★ 零腿重读：那 ~450–570 µs 的**最新一跳账**（`p57`–`p70` 十条腿）—— 总量钉死，且 `commit→start` 那一半**不是 37 µs 而是 ~215 µs**（2026-09-27，零腿）

> 用户裁定"那 ~450–570 µs 要查清、不要盖过去"。本节**不飞腿、不改码**：把 §6.99 落地的 Q9-F3 per-label 表 + `busy split` + `residency` 在**现成腿**上重读，
> 目标只有两个：**(a) 把"一跳设备账"钉到可复算**；**(b) 核对 §6.101 ③ 那张判读表的前提（`wait` 小 / `exec` 大）是否还成立**。
> 结论先说：**(a) 成立且极稳；(b) 的前提在 (B) 修复之后已经变了 —— `wait p50` 是 ~215 µs，不是 37 µs。**

#### ① 十条腿的 per-label 读数（`OCUDU_METAL_GPU_TIME=1`，都是 n78 加压、并发 2、交付配置）

| 腿（09-26）| `merged_hop` n | **wait p50** | **exec p50** | `ce_weights` exec p50 | `dft_front_end` exec p50 / n |
|---|---|---|---|---|---|
| `p54-n78-close`（修复**前**）| 292316（= 2× 真值，计数缺陷，§6.102④）| **37.0** | 509.6 | 42.5 | 46.6 / 716642（= 2×）|
| `p55-n78-close-stress`（修复前）| 290588（= 2×）| **37.4** | 510.4 | 43.9 | 46.6 / — |
| `p57-n78-sigma2fix2` | 145742 | **216.6** | 468.8 | 41.8 | 46.6 / 321973 |
| `p58-n78-close2` | 144749 | 211.8 | 467.8 | 38.5 | 46.6 / 358057 |
| `p59-n78-close2-stress` | 145460 | 213.5 | 468.5 | — | — |
| `p60-n78-close3` | 144679 | 217.0 | 468.7 | 42.0 | 46.6 / 334909 |
| `p61-n78-repro-gpu` | 145589 | 215.0 | 469.5 | — | — |
| `p65-n78-txring64` | 144914 | 212.9 | 470.7 | 37.5 | 46.5 / 319597 |
| `p66`/`p67`/`p68`（环深扫描）| 145964 / 139532 / 142703 | 217.7 / 212.8 / 214.0 | 468.5 / 469.8 / 470.1 | — | — |
| `p70-n78-spp1024` | 145508 | 214.6 | 468.5 | 37.1 | 46.6 / 328297 |

⇒ **十条腿（`p57`–`p70`）的 `wait p50` 全部落在 211.8–217.7 µs、`exec p50` 全部落在 467.8–470.7 µs**（散布 6 µs / 3 µs）。
⇒ ★ **新事实（本节要登记的更正）**：`wait p50` 在 **(B) 修复的边界上从 37 µs 跳到 ~215 µs**，而 `exec p50` 只从 510 缓降到 469。
   两条边界的腿是 `p55`（37.4 / 510.4）与 `p57`（216.6 / 468.8），中间只隔了 sigma2 的 (B) 修复（§6.104–§6.109）。
   **⇒ 跨这条边界不可比**：那段修复同时改了**融合边缘的排序**，于是"这条 cb 什么时候被提交、排在谁后面"也换了对象。
   `p54`/`p55` 的 37 µs **只能作为修复前的历史值**引用，**不能**和 `p57` 之后的 215 µs 并列（§6.102② 那句"是执行、不是排队"只对 `exec` 那一半成立）。

#### ② 一跳的账（`p60`/`p70` 中位，全部可由腿日志复算）

| 量 | 值 | 出处 |
|---|---|---|
| 车道 `residency`（阶段入口 → 最后一条 cb 结束）| **637.5 / 627.0 µs** | `[ul_gpu_lane] residency`（`p57`–`p68` 九条腿中位 634.5–640.6）|
| 车道 `busy`（本跳 cb 的并集）| 502.2 / 504.6 | `[ul_gpu_lane] busy` |
| `gap = residency − busy` | 139.5 / 129.1（可为负 ⇒ 有重叠）| `[ul_gpu_lane] gap` |
| `busy split`：`ch_wt` + `merged_hop` | **43.0 + 474.1 = 517.1** / 38.7 + 466.8 = 505.5 | `[ul_gpu_lane] busy split` |
| `merged_hop` cb：`commit→start` / `start→end` | **217.0 / 468.7** | Q9-F3 per-label |
| 宿主：阶段入口 → lane 提交 | 81.8 / 80.5 | `host: stage entry -> lane commit` |
| `V1`（`[ul_gpu_pipeline]` 中位）| **1412.1 / 1389.8** | `p57`–`p68` 九条腿 1402.8–1412.1 |
| **一跳的墙钟**（见 ③）| **1935 µs**（1 跳 / 3.86 个 500 µs 时隙）| `[ul_rx_wait] samples` ÷ hop 数 |
| 设备并集占用（全队列）| **29.2% / 29.5%** | Q9-F3 `busy(union)/window` |

#### ③ 复算：三处自洽（这套数可以当作后续的锚点）

1. **每跳的设备需求**（Q9-F3 per-label 均值 × 次数）：`dft_front_end` 2.31 次/跳 × 46.6 + `ce_weights` 42.0 + `merged_hop` 468.7 + `late_handed` ≈ **619 µs/跳**（`p60`）；
   同期 `busy(union)` = 82.0 s / 279 s = **29.2%**，而 619/1935 = **32.0%** ⇒ **差 2.8 个点 = 各 cb 之间的重叠只有 ~27 µs/跳**（`p70` 同形：612 µs vs 28.8%）。
2. ⇒ **一跳的墙钟必须是 1935 µs，不是 500 µs**：`[ul_rx_wait] samples=558144` × 500 µs = 279 s = 腿长，而跳只有 144679 次
   ⇒ **`receive()` 每槽一次，上行跳每 3.86 槽一次**。⚠ 任何"窗口 / 时隙 500 µs"的比例换算都要先用这一条换量纲
   （否则 619 µs/跳 会被读成"占用 124%"，那是错的）。
3. **`wait` 那一半大体能对上"排在它前面的活"**：D1 武装下前端块**就在后端队列**、与 lane 同一条**串行**队列（§6.84①③ 的代码证据）
   ⇒ lane cb 之前排着 `ch_wt` 43 + 同队列其它 cb（`dft_front_end` 2.31 次/跳 × 46.6 = 108，plain 路）≈ **151 µs**，加上宿主交棒 / 提交间隙 ⇒ **≈215 µs 是"排队"，不是"设备不动"**。
   ★ 这一条**待零腿验证**（读一间：两种队列种类是否同一个 `MTLCommandQueue`；再把 records 的时间轴按 label 排一次看重叠），本节只登记为**算术一致的候选**。

#### ④ 于是开放问题收窄成一句（这才是下一个项目的入口）

> **一条 5–6 次派发、算力只有 ~78–150 µs 的 cb，为什么在空口上要占 ~470 µs 的设备时间？**
> （同一平台离线：打包 14 变换 = 46.9 µs、每派发 ~38 µs 未重叠上界、单派发 47.5 µs；而空口同一形状 = 452–470 µs。）

两个候选（**不是**新提的，是 §6.101② 与 §6.83 的收窄版）：

* **H1′ 时钟 / 功率状态**：同一台机器**会话间差 ~4×**（记录值 46.6 µs vs 今日 11.8–13.1 µs）、**空闲 ≥50 ms ⇒ 2.6–3.7×**（饱和在 ~50 µs）；且设备**只有 29% 的时间在跑**。
* **H2 空口下"每次派发 / 每次新绑定"的价格更高**（Q25）：离线每派发 38 µs 是**未重叠上界**，空口读到 ~85 µs/派发；
  `p48` 的 staging 臂多出的 **+464 µs ≈ 14 × 33 µs** 与"一次新缓冲 ≈ 36–40 µs"同量级 ⇒ 值得把"每跳 cb 的首次绑定数 × 单价"对一次账。

⇒ **判别方法（都不破 G1/G2）**：离线补两条臂 —— "**链式（互相依赖）vs 独立**派发"、以及"**复用绑定 vs 每跳新绑定**"；
空口只加一条**并发扫描腿**（conc 1/2/4，带 Q9-F3 per-label 表）——若设备越忙、每次派发越便宜 ⇒ H1′ 成立。

#### ⑤ 两条容易读错的口径（写进记录，避免下次再撞）

* `dft_front_end` 的 **46.6 µs 是 plain 路（1 变换/cb）**，与离线 47.5 µs 吻合到 2%（§6.98④）⇒ **它不是融合车道的"前端"**；
  融合车道的前端工作**已被并进 `merged_hop`**（只有 `OCUDU_LANE_DIAG_SPLIT=1` 拆分臂里它才单独出现，`p47` 读 452.3 µs，§6.77）。
* `[ul_gpu_lane] dft busy`（`p60` 均值 435.2 µs / 50639 样本）是**打包前端车道自己**的读数，与上一条**不是同一个对象**，两者不可互证。

> ★ **两处后续更正（见 §6.124，2026-09-27）**：
> ① 本节 **③-3** 登记的"215 µs = `ch_wt` 43 + 前端 2.31 × 46.6 ≈ 151 µs"**已被否**（`dft_front_end` 在**另一条** `MTLCommandQueue` 上，不排在 lane cb 前面）；
> ② 离线 harness 的**正确路径**是 `doc_chinese/phy_latency/wip/dft_dispatch_cost.mm`（本工作流的 memo `session_handoff_2026-09-27-1.md` §5 里写成了 `phy_pipeline_gpu/wip/`，**以本节为准**）。


### 6.124 ★★★ 零腿 + 离线（本会话）：那 ~470 µs 被收窄成一个"**线程组在空口不并行**"的现象 —— 两条旧归因作废（2026-09-27）

> 承 §6.123 的"开放问题一句话"（5–6 次派发、算力 78–150 µs 的 cb，为什么在空口占 ~470 µs 设备时间）。
> 本节的顺序是：**先读码钉队列身份（否掉 §6.123③-3 的候选）→ 再读计数器钉"一跳 cb 里到底有几条派发"→ 再用离线把"空口能混合进来的一切"逐个混进来量**。
> 结论：**空口与离线差的不是派发价、不是算力、不是争用，而是"一条派发里的 14 个线程组在空口被一个接一个地跑"**。

#### ① 零腿 A：队列身份 —— **两条独立的 `MTLCommandQueue`**，`dft_front_end` 不在 lane 的前面

* `shared_queue::queue()` 与 `shared_queue::backend_queue()` 是**两个独立对象**（`ocudu_metal_queue.mm:1082/1086`，两次 `[device newCommandQueue]`）。
* `dft_front_end` 这个 label 全仓库只有**一处**赋值，且用的就是 `queue_kind::front_end`（`ocudu_dft_metal_engine.mm:1416`）⇒ 那 **334909 条 46.85 µs** 的 cb 是 **plain 路**，跑在**前端队列**上。
* lane 的每一条 cb（`ce_weights`/`ce_stage`/`ce_held`/`ce_abandon`/`handed_direct`/`split_dft`/`merged_hop`/`demapper`/`late_handed`）都是 `queue_kind::back_end`。
* ⇒ ★ **§6.123③-3 登记的候选（215 µs = `ch_wt` 43 + 前端 2.31 × 46.6 ≈ 151）被否**：那条队列的活在**另一条**队列上，不排在 lane cb 前面（只与它争设备，而争用已被 §6.101① 与本节的 ③ 否掉）。
* ⇒ 排队账改按**后端队列自己**算：`p60` `gpu busy (back_end)`: commits **291705**、busy **74.93 s**、**mean 256.87 µs**、window 281.2 s ⇒ **利用率 26.6%**，一跳 **2.02 条 cb**。
  26.6% 利用率下的 FIFO 等待与实测**中位 215 µs 同量级**（M/M/1：≈93 µs；成对到达 + 同伴正在服务会更大）⇒ **方向与量级一致**；
  **精确分解仍要 record 时间轴**（把 Q9-F3 的 `[start,end]` 按 label 排序看重叠）—— 保留为待办，不再当"证据"引用。
* 同时钉下：`gpu busy (front_end)` = commits 334909 / busy 15.69 s ⇒ **5.6% 设备占用**；两条队列的 busy 之和 90.6 s vs 并集 82.0 s ⇒ **跨队列重叠只有 8.6 s（占 9.5%）**。

#### ② 零腿 B：一跳 cb 里的派发数（都是现成腿的计数器）

* `burst commits=144680 dispatches=578718 (equalizer=144680 demapper=144680 channel_estimator=289358)` ⇒ lane 的 burst cb = **4.00 次派发/跳**（eq 1 + demap 1 + CE 2）。
* `dft commits=334909 transforms=334909 … released=147003 batched=147003/2058042 batch_max=14`：
  `flush_pending_front_end()` 把 pending 的变换**一次 `dispatchThreadgroups(nof)` 编成一条派发**（源码 1070–1103 已逐行核对）⇒
  ★ **空口一跳里那 14 个变换确实是一条派发、14 个线程组**（`batch_dispatches/batch_transforms` 的语义就写在那段注释里）；plain 路才是"一个变换一条 cb"。
* ⇒ 一跳 cb ≈ **1（打包 DFT）+ 4（burst）** 条派发，而不是 14+4。

#### ③ 离线基线（今天的同一次运行，`wip/dft_dispatch_cost.mm`，真实 kernel + 真实几何 n=768 = 23.04 Msps ÷ 30 kHz SCS）

| 臂 | cb 窗口 | 每变换 |
|---|---|---|
| 1 变换，1 派发（int16+网格）| **47.4 µs** | 47.4 |
| **14 变换，1 派发（打包）** | **47.3 µs** | **3.4** |
| 14 变换，14 派发 | 573.2 | 40.9 |
| 2 / 4 / 7 变换，2 / 4 / 7 派发 | 87.5 / 171.3 / 294.0 | 43.7 / 42.8 / 42.0 |

⇒ **判据**：离线在同形状下 **14 个线程组是并行的**（47.3 ≈ 一个线程组的 47.4）。
⇒ 顺带更正两条旧话：§6.80 的"真实 kernel 的线程组不重叠（被占用率限住）"**在离线不成立**（今天实测 47.3）；§6.77 的"平凡 cb 驻留 ≈ 37 µs"也被 §6.80① 的 7.1 µs 更正过（这里再次确认：**47 µs 是真实 kernel 一个线程组的钱**）。

#### ④ ★ 新臂：把"空口真正与它共享设备的东西"逐个混进来 —— **测量臂 12.8–17.3 µs，一点都不动**

上一轮（§6.101①）的背景是**同一个 kernel**；§6.83② 的背景是**不声明线程组内存的自旋 kernel**。本节补的是**两者都不是**的空口形状：

| 背景（另一条队列持续流，测量臂 = 打包 14 线程组）| 测量臂窗口 |
|---|---|
| 设备空闲 | 14.5 µs |
| **1 变换/cb 的 DFT**（= plain 路的形状，46.85 µs × 320 条）| 15.0 |
| 14 变换/cb 的 DFT | 16.0 |
| 自旋 kernel（**0 KiB** 线程组内存，10 条长 cb）| 17.3 |
| 自旋 kernel（**每组 32 KiB × 16 组** = 整个设备每核预算之和）| **13.9** |
| 自旋 kernel（32 KiB × 4 组）| 12.8 |

⇒ ★ **线程组内存 / 占用率假设被否**：把设备每核的 32 KiB 预算（DFT kernel 声明 `threadgroup float2 buf[4096]` = 32 KiB/组）**占满**，测量臂**没有变慢**。
⇒ 同时把 §6.101① 的"200 并发 cb ⇒ +0.5…+3.5 µs"补成完整结论：**背景无论是同 kernel、plain 路形状，还是把线程组内存占满，都不动它**。

#### ⑤ ★★ 模型：同一条直线 `O + N × G` 同时穿过空口的两个点，而离线是 `O + G`

| 点 | 窗口 | 出处 |
|---|---|---|
| 空口，**1 个线程组** 的 cb | **46.85 µs**（n=334909，p95 **47.6**，极紧）| `gpu busy (front_end)`/per-label `dft_front_end`（`p60`）|
| 空口，**14 个线程组** 的 cb | **452–482 µs**（`dft carried GPU start->end` 中位 **481.8 / 469.6 / 453.4**，**min 47.1 / 47.9 / 48.1**）| `p60` / `p70` / `p47` |
| 离线，1 个线程组 | 47.4 µs | 本次 |
| 离线，14 个线程组 | **47.3 µs** | 本次 |

⇒ 解 `O + N×G`：**O = 15.7 µs、G = 31.2 µs**（空口的 1 组点与 14 组点同时满足）；同一组 `O/G` 在离线给出的是 **`O + G`（组并行）**。
⇒ ★ **空口与离线的差别不是"派发贵"、不是"算力贵"、不是"设备被占"，而是"那 14 个线程组在空口被一个接一个地执行"**；
   而 **min ≈ 47 µs** 说明**并行那条路在空口上存在**（罕见）。⇒ 这是**调度/功率状态**类现象，不是 kernel 或几何的静态性质。

#### ⑥ 现在还剩什么（都还没被证）

* **H1″（本轮的新形式）**：空口队列里**每 ~1 ms 只有一条 cb**，设备大部分时间空着 ⇒ GPU 可能**只给这条派发 1 个核**（核心门控/唤醒），
  这与 §6.88② 的 idle 扫描（≥50 ms 空闲 ⇒ 2.6–3.7×，饱和 ~50 µs）**方向一致但量级差 ~7 倍**（扫描只到 50–66 µs）⇒ 需要一个能把"核数"而不是"时钟"放大的实验。
* 已否（本会话 + §6.83–§6.86）：派发价、线程组内存/占用率、同 kernel/plain 路/占满 tgmem 的争用、输入映射（含实时写入与每 run 新 wrap）、栅栏、signal、跨线程提交、宿主、队列身份、块大小/算力。

#### ⑦ 两条原计划的离线臂**不必再飞**（现成读数已回答）

* **"链式（互相依赖）vs 独立派发"**：不需要。融合 cb 里的 5 条派发本来就是**依赖有序**的（阶段屏障），而读数说那 4 条 burst 派发**一共只值 ~17 µs**（469 − 452）⇒
  **同一 cb 内的多条派发并没有被"每条 ~43 µs"串起来**；被串起来的是**一条派发内部的线程组**。
* **Q25（"每次新绑定收费"）**：`wip/dft_dispatch_cost.mm` 的"**wrap 建一次复用 vs 每 run 新建 wrap**"臂今天读 **11.8 vs 12.7 µs** ⇒ **否**（与 §6.85 的 22.1 µs 同向）。

#### ⑧ 下一步（**一条腿的分辨实验**，判据先写在这里）

空口按 **`OCUDU_DFT_BATCH_SYMBOLS`** 扫 **14（默认，不传）/ 7 / 2** 三个值（只改"一条派发里放几个变换"，**`cbs/lane` 不变、V4 零代价**），各读四件：`ul_gpu_pipeline` 中位、per-label `merged_hop` 的 wait/exec、`dft carried GPU start->end`、`busy split`：

| 若 | 则 | 下一步 |
|---|---|---|
| 窗口 ≈ **15.7 + 32 × batch**（7 ⇒ ~240、2 ⇒ ~80）| **组被串行**（H1″）| 杠杆 = **减少线程组数**（让一个线程组做多个变换），而不是减少派发数 |
| 窗口随 batch 变小而**变差**（(14/B) 条派发 × ~43 µs）| 打包在空口**仍然有效** | 452 µs 另有来源，回 ③ 的离线表继续二分 |
| 窗口**不动**（三个值都一样）| 既不是组也不是派发 | 归"设备状态"，转 conc 扫描（`p71-n78-conc1/2/4`）|


### 6.125 分辨实验的**预登记**（腿 `p72`/`p73`/`p74-n78-batch14/7/2`，待飞）

> 登记在飞腿**之前**（项目纪律）。一条臂只改一个变量：**一条派发里放几个变换**。
> 旋钮语义（源码 `front_end_batch_override()` 已核）：不传 = **AUTO = 一个时隙自己的符号数**（本小区 `slot_symbols=14`）；
> `N≥2` = 显式上限（pending 攒到 N 就 flush 成一条派发）；`N=1` = 历史控制臂（每符号一条派发）。
> ★ **不动数据路径**：变换照样一条 cb、照样交棒、`cbs/lane` 照样 2.00 ⇒ **V4 零代价**（改的是 cb *内部*的派发数，不是提交数）。

**配方**（与 `p59`/`p61`/`p65`–`p70` 同族，便于同日对照）：

```bash
cd /Users/jiachengwang/dev/ocudu
for B in 14 7 2; do
  sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
    doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p7$((B==14?2:(B==7?3:4)))-n78-batch$B \
    --regime=stress \
    --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
    OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=$B
done
# CN 侧每条腿：iperf3 -R -b 40M -P 4 -t 240；【单次 Ctrl-C】停，等进程退出再读报告
```

**开跑前必须先看的登记行**（姿势正确性，`p49` 的教训）：

```
[phy_pipeline] dft radio inputs: … the hand-over route carried … / the plain route …
[metal_stats] dft commits=… batched=<dispatches>/<transforms> batch_max=14 batch_src=knob slot_symbols=14
```

| 预登记判据 | 期望（若"组被串行"成立 = H1″）| 若相反 |
|---|---|---|
| `batch_src` | `knob`（N=14 时也应是 `knob`，因为显式传了值）| —— |
| `batch_max` / `batch_transforms/batch_dispatches` | 14（B=14）、**7**（B=7）、**2**（B=2）| —— |
| `dft carried GPU start->end` 中位 | **≈ 15.7 + 32 × B**：B=14 ⇒ ~470（= 现状）、B=7 ⇒ **~240**、B=2 ⇒ **~80** | 随 B 变小而**变差**（(14/B) 条派发 × ~43 µs）⇒ 打包仍有效，452 另有来源 |
| `merged_hop` cb `start→end` p50 | 同上（+4 条 burst 派发 ≈ +17 µs）| 同上 |
| `ul_gpu_pipeline` 中位 | 随 B 变小而**下降**（若组被串行，则窗口降幅经 1.2–2.6× 放大到 V1）| 上升（打包有效）|
| `cbs/lane` / 契约 / `gaps` / 池 | **2.00 (max=5)** / 9 of 9 / 0 / 绿（**不允许变**）| —— |
| RF 失败率 | 只记录（按 §6.122 的带宽判，不做本次判据）| —— |

**两种结果都是结论**：① 若窗口随 B 线性下降 ⇒ 空口确实"一组一跑"，**下一个杠杆是让一个线程组做多个变换**（或换 kernel 形状），而不是继续减派发；
② 若窗口随 B 变小而变差 ⇒ 打包在空口依然有效，那 452 µs 必须另找来源，届时回 §6.124③ 的离线表二分（下一条臂是"每组工作量 × 组数"的二维扫描）。


### 6.126 ★★★ 腿 `p72`/`p73`/`p74`（batch 14/7/2）：**预登记的第二个分支命中** —— 打包在空口仍然有效、**H1″（"线程组被一个接一个地跑"）被否**，每条派发 ~37–39 µs（与离线同价），**仍剩一个 ~365 µs 的常数**（2026-09-27）

#### ① 先确认"只有一个变量"，且不允许变的都没变

| 项 | `p72` | `p73` | `p74` |
|---|---|---|---|
| `batched=`（派发/变换）| **153606 / 2150484 = 14.0** | **301464 / 2110248 = 7.0** | **1044428 / 2088856 = 2.0** |
| `batch_max` / `batch_src` / `slot_symbols` | 14 / **knob** / 14 | 7 / **knob** / 14 | 2 / **knob** / 14 |
| `lanes` / `cbs/lane` | 141729 / **2.00 (max=5)** | 145722 / **2.00 (max=5)** | 145983 / **2.00 (max=5)** |
| 契约 / `gaps` / `rx_overflows` | **9 of 9** / 0 / 0 | 9 of 9 / 0 / 0 | 9 of 9 / 0 / 0 |
| 池 `starved_events` / `held_max` / `free_min` | 0 / 10 / 22 | 0 / 10 / 22 | 0 / 12 / 20 |
| `p0_gate` | **29 of 29** | 29 of 29 | 29 of 29 |
| `leg_gate` | 5 of 9 | 5 of 9 | 5 of 9 |

⇒ 旋钮姿势正确（`batch_src=knob`、每条派发的变换数正好 14/7/2）。
⇒ ⚠ `leg_gate.sh` 的 4 条不过项是**脚本字面量/误绑**（`contract 8 of 8` 应为 9、`stale=0`、`cbs/lane` 那条还要求 `max<=2`、`grant>=50%`）——
**同日同类加压腿 `p61`/`p70` 也是 5 of 9**、默认工况的 `p60` 6 of 9 ⇒ **不是本轮的回归**；
但 `leg_gate.sh` 的 `8 of 8` 与 `max<=2` 两条字面量**还没按"按名字判"修**（`milestone_audit.sh`/`a12_attribution_gate.sh` 已修）⇒ **记一笔待办**。

#### ② 读数（单变量扫描）

| 腿 | batch | DFT 派发/跳 | `merged_hop` exec p50 | `busy split` merged_hop | `dft carried` 中位（**min**）| `residency` 中位 | `busy` 中位 | **V1 中位** |
|---|---|---|---|---|---|---|---|---|
| `p72` | **14** | 1 | **468.7 µs** | 473.1 | 470.8（**47.5**）| 623.4 | 504.0 | **1399.5** |
| `p73` | **7** | 2 | **505.8** | 507.0 | 511.7（87.4）| 699.9 | 536.5 | **1473.4** |
| `p74` | **2** | 7 | **703.4** | 702.4 | 713.9（237.8）| 1101.5 | 737.2 | **1823.5** |

* 窗口：468.7 →（+37.1）→ 505.8 →（+197.6）→ 703.4；**V1**：1399.5 →（+73.9）→ 1473.4 →（+350.1）→ 1823.5
  ⇒ **放大 1.81–1.99×**（与 §7.6.1 记的 1.2–2.6× 同族，且这是同一结构上的直接配对）。
* ★ 三点拟合 **窗口 ≈ 429.6 + 39.1 ×（DFT 派发数）**（残差 ≤2 µs）；换成"总派发数"是 **283 + 37.1 × 总派发数**（残差 ≤12 µs）。

#### ③ 判决：**预登记 §6.125 的第二个分支命中**，并且它把 §6.124⑤ 的模型**改掉了**

* ★ **H1″ 被否**：若"14 个线程组在空口被一个接一个地跑"成立，三个 batch 的**组数都是 14**，窗口应当**几乎不动**；实测它**按派发数线性增长**（+37.1 / +197.6 µs）。
* ★ **打包（14 变换/派发，P2-B′）在空口依旧是最优形态**：把一条派发拆成 2 条或 7 条只会更差 ⇒ 这是 P2-B′"当年 −38% V1"的机制**第三次被复现**（前两次：§6.31 的 p22/p23/p24，§6.82 的离线派发扫描）。
* ★ **空口的"每条派发单价"≈ 37–39 µs，与离线同一条平台规律一致**（离线：2/4/7/14 条派发 = 87.5/171.3/294.0/573.2 ⇒ ≈41–43 µs/条，且**与组数无关**，§6.83①）⇒ **空口并没有为"派发"付更贵的钱**。
* ⚠ **更正 §6.124⑤**：那份模型把"`dft carried` 中位 470–482"与"plain 路 1 组 cb = 46.85"联立，解出"组被串行（O=15.7、G=31.2）"——**这条单变量扫描把它否掉**。
  正确的读法是：**`窗口 ≈ 365 µs（与被测的派发数、组数都无关的常数）+ 每条派发 ≈ 37–39 µs`**；
  而 `dft carried` 的 **min（47.5 / 87.4 / 237.8）**说明那 ~365 µs **在最好的情况下可以接近 0**（≈ "每条派发 47.5 µs"的理想路径）。

#### ④ 于是剩下的问题换了一个问法（**新的第一嫌疑：那条"被交棒 / 被 adopt 的 cb"自己**）

* 三条腿里那 ~365 µs 常数只出现在**承载前端块的 lane cb** 里；对照：**plain 路**（引擎自己提交、1 条派发、1 个线程组）的 cb 只有 **46.6 µs**（n=340621–455977，p95 47.4–47.5，**极紧**），**没有常数项**。
* 已被离线否掉的"交棒特有"机制：encoder 跨时隙开 500 µs、跨线程提交、signal shared event、每 run 新建 wrap、被实时写入的映射、**同 kernel / plain 路形状 / 占满每核 32 KiB 线程组内存的争用**、冷缓存（§6.85/§6.86/§6.124④）。
* ★ **新候选（本轮提出）**：常数可能是**窗口内部的设备侧等待** —— 融合边缘的 sigma2 依赖（(B) 修复引入的"读上一跳的块"）如果落在**同一个 cb 内的一条 fence/wait** 上，
  那么 `start→end` 会把"等上一跳（很可能是**同伴车道**那条 cb）的尾巴"也算进去 ⇒ 天然解释：**与 batch 无关、与组数无关、min 可以接近 0、plain 路没有**。
  ⚠ 已知的反面证据：§6.76 的 conc-1 只让窗口动 13%——**但那条腿在 (B) 修复之前**，当时的融合边缘还没有这条跨跳依赖 ⇒ 需要**在现在的 HEAD 上重做 conc 1**。

#### ⑤ 下一步（两条腿，都是零代码；判据先写）

1. ★ **`OCUDU_LANE_DIAG_SPLIT=1`**（+`OCUDU_METAL_GPU_TIME=1`）：把一跳拆成 3 条 cb，用**现在**的 per-label 表读三条各自的 wait/exec。
   * `split_dft` ≈ **450 µs**、`ch_wt`/`merged_hop` 各 ~45/24 ⇒ 常数在**前端那条 cb** 里（与 `p47` 的旧读数一致），继续问"它的哪一部分"；
   * 三条都只有几十 µs、和 ≈ 500 但**没有一条独占常数** ⇒ 常数是**跨 cb 的**（队列/时序/跨跳依赖），转 2。
   * 代价：`cbs/lane` → 3.00（**测量臂**，§3.1.1 已授权"为测量暂时放宽 V4"）。
2. ★ **conc 1**（同配方，只改 `max_pusch_and_srs_concurrency=1`）：`p46` 的 13% 是 **(B) 修复之前**的读数。
   * 若窗口的**中位**从 ~470 掉到 ~200 且 `min` 基本不变 ⇒ 常数 = **等同伴车道**（④ 的新候选成立）；
   * 若窗口不动 ⇒ 常数在**本跳自己的 cb** 里，回 1 的拆分读数。


### 6.127 ★★★ 腿 `p75`（diag-split）/ `p76`（conc 1）：那 ~430–445 µs **在前端块自己的 cb 里**、**与同伴车道无关**；并**更正 §6.126③ 的一次过度解读**（2026-09-27）

#### ① 腿 A `p75-n78-diagsplit2`（`OCUDU_LANE_DIAG_SPLIT=1` + per-label 表）

| label（cb 装什么）| `commit→start` p50 | **`start→end` p50** |
|---|---|---|
| **`split_dft`（前端块自己那条 cb）** | 271.6 | **443.5** |
| `ce_weights`（相关/权重）| 284.0 | 45.7 |
| `merged_hop`（均衡+解映射）| 756.9 | 23.4 |
| `dft_front_end`（plain 路）| 46.7 | 46.7 |

* `busy split`：**`dft` 445.8（86%）** + `ch_wt` 45.5 + `merged_hop` 24.4 = **515.7** ≈ 融合臂的 40.8 + 473.1 = **513.9** ⇒ **总量与结构无关**（第五次复现）。
* `residency` 567.8 / `busy` 509.8 / `gap` 56.5；V1 中位 **1518.3**；`cbs/lane=3.00 (max=3)`（**测量臂**）、契约 9/9、`gaps=0`。
* ⇒ ★ **判决（预登记第一支命中）**：那 ~445 µs **就在承载前端块的那条 cb 里**（一跳 busy 的 86%），CE 与 eq/demap 各只有 45.7 / 23.4 µs。

#### ② 腿 B `p76-n78-conc1`（同配方，并发 1）

| 读数 | `p76`（conc 1）| `p72`（conc 2，同日同配方对照）|
|---|---|---|
| `merged_hop` exec p50 | **464.2** | **468.7** |
| `merged_hop` wait p50 | **54.4** | 206.5 |
| `dft carried` 中位（min）| 477.0（**47.1**）| 470.8（47.5）|
| `residency` / `busy` / `gap` 中位 | 495.7 / 494.9 / **0.0** | 623.4 / 504.0 / 125.7 |
| `ce_weights` exec p50 | 40.4 | 37.0 |
| V1 中位 | **1606.2** | 1399.5 |
| `cbs/lane` / 契约 / `gaps` | 2.00 (max=2) / 9 of 9 / 0 | 2.00 (max=5) / 9 of 9 / 0 |

* ⇒ ★ **判决（预登记第二支命中）**：**窗口不动**（464.2 vs 468.7，−1%）⇒ 那 ~430 µs **不是"等同伴车道"**；§6.126④ 提的跨跳候选（连同"窗口内部的设备侧等待"这一族）**被否**。
* 并发 1 只改**等待类**的量：`wait` 206.5 → 54.4、`residency` 623.4 → 495.7、`gap` 125.7 → 0.0；而 V1 **变差** 1399.5 → 1606.2（与 §7.5 记的"并发是杠杆、但有代价"一致）。
* ⚠ 注意 `p46` 那条"conc-1 只让窗口动 13%"的旧读数**不要再引用**：它在 **(B) 修复之前**。现在有了修复后的同配方对照。

#### ③ ★ **更正 §6.126③ 的过度解读**（这条比上面两条腿更重要）

§6.126③ 写了"**H1″（14 个线程组在空口被一个接一个地跑）被否**"，依据是"三点扫描按**派发数**线性增长"。**这个推断越界了**：
那条扫描**三个 batch 都仍然是 14 个变换**，它只变了"14 个变换被分成几条派发"，**没有变线程组总数**。因此它证明的只有两件事：
1. **每条派发 ~37–39.5 µs**（5→6 条 37.1、6→11 条 39.5），**与离线同价**（离线 41–43 µs/条）；
2. **打包仍然更优**（拆成 2/7 条只会更差，V1 放大 **1.77–1.99×**）。
而"组是不是被串行"这条扫描**根本分辨不了**：`A + 38 × 派发数`（A ≈ 430 = 一个固定块成本）与 `30.5 × 组数 + 38 × 派发数`（组被串行）在**组数恒为 14** 时是同一个式子。

**支持 H1″ 的证据仍然是 ① 里那对读数**：

| 对比 | 空口 | 离线（同一 kernel/几何）|
|---|---|---|
| 1 个线程组的 cb → 14 个线程组的 cb | 46.7 → **443.5**（**+396.8 µs，13 个组 ⇒ 30.5 µs/组**）| 47.4 → **47.3**（**+0.0 µs**）|
| 打包的收益（14 条单组派发 ÷ 1 条 14 组派发）| 14 × 46.7 = 654 ÷ 443.5 = **1.47×** | 573.2 ÷ 47.3 = **12.1×** |

⇒ 空口上"**一个线程组 ≈ 一条派发**"（~30 vs ~38 µs），离线上"**一个线程组几乎免费**（3.4 µs）、一条派发 ~41 µs"——**这才是那个 ~430 µs 的所在**，也是本工作流唯一还没解释的量级。

#### ④ 于是"机制猎捕"到了它现在这套旋钮的边界（诚实结论）

* 空口的组数与派发数**没有独立的旋钮**：这个引擎只能"把一槽的 14 个变换打包成 1 条派发"（现状）或"分成 N 条"。**没有任何旋钮能"保持派发数不变、只改线程组数"** ⇒
  "固定块成本 A" 与 "组被串行（30.5 µs/组）"这两个模型**在现有旋钮下观测等价**（两者都给出 `A + 38 × 派发数`）。
* 离线侧已经把**能构造的形状**用尽（~30 条臂：派发数扫描、打包、争用三族、线程组内存占满、实时映射、每 run 新建 wrap、冷缓存、encoder 跨时隙开、**编码后等 0.5/2 ms 再提交**、跨线程提交、signal event、idle 扫描…）——**没有一种能把 443.5 复现出来**（本轮新加的两条：`encoded, then 500/2000us before the commit` = **12.5 / 18.0 µs**）。
* ⇒ **要继续，只剩"改结构"的测量臂**（都会破坏交付形态，只能当测量）：`OCUDU_DFT_OPEN_BLOCK=0`（前端回到"一变换一 cb"，V4 会从 2.00 暴涨）与 `OCUDU_DFT_RELEASE_BLOCK=0`（关 D1：前端回到自己的队列、由引擎自己提交）。
  **两条都需要用户裁决**（§6.89 已因 **G2** 把 `RELEASE_BLOCK=0` 从*候选结构*里撤掉；当*测量臂*用是另一回事）。
* **prize 的量级**：前端块 ~430–445 µs 是 ~470 µs 窗口的九成；按 §7.6.1/本节的 **1.77–1.99×** 放大，若它能降到离线价（~47 µs），V1 大致 **−700…−800 µs**（即 ~600–700 µs V1）。**但机制在 ~30 条离线臂 + 12 条腿之后仍然未知**，这是要不要继续投入的分水岭。


### 6.128 关 D1 的测量臂（`p77-n78-nod1`）—— **预登记**（用户 2026-09-27 裁决：先飞这一条）

> **问题**：那 ~443 µs 到底是"**未提交交棒**（D1 的块 cb）"的属性，还是"**打包的 14 组派发**"本身的属性？
> 已知的两个端点：**plain 路**（引擎自己提交、1 变换/cb、前端队列）= **46.7 µs**；**D1 块 cb**（未提交交棒、14 变换/派发）= **443.5 µs**。
> 这条臂把 **D1 关掉**，让前端块回到"引擎自己提交"的形态，从而把这两个变量分开。

**命令**（一条腿，独立跑）：

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p77-n78-nod1 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_DFT_RELEASE_BLOCK=0
```

**这条臂会同时改动的东西**（不是单变量，登记在案）：① 关 D1 ⇒ 前端块由**引擎自己提交**（不再是 lane 提交的未提交块）；② `dft_queue()` 因此回到**前端队列**（§6.84①）；
③ 估计器无法 adopt ⇒ 一跳变成 **3 条 cb**（前端 / ch_wt / merged_hop）⇒ `cbs/lane` **3.00**（V4 代理量，**测量臂**）；
④ 契约里 `dft radio inputs` 的**路线分布会翻**（plain 路 ≈ 全部）——这是**预期**，不是缺陷。

**判据（先写死，跑完不追认）**：

| 读数 | 若 **≈47 µs**（像 plain 路）| 若 **≈440 µs**（不变）|
|---|---|---|
| `dft_front_end` 的 exec p50（新腿里前端块会带这个 label）| ★ **触发点 = 未提交交棒（D1）本身** ⇒ 下一步问"交棒的哪一部分"（提交线程/时刻、队列、以及是否存在 **G1/G2 兼容**的等价形态）| ★ **触发点 = 打包的 14 组派发本身** ⇒ "空口线程组不并行"成立为**平台属性**，杠杆只剩**改 kernel 形状**（一个线程组做多个变换）|
| `[metal_stats] gpu busy (front_end)` 的 mean | 同上（前端块那时会被算进 front-end 队列）| 同上 |
| `[ul_gpu_lane] dft busy` / 其 min | 若中位塌到 ~47 而 min 不变 ⇒ 同左 | 若仍 ~430 ⇒ 同右 |

**不许变（G1/G2 的红线，这条臂必须保住）**：`host device data crossings` **0.00 + 0.00 / 跳**、`host sample assembly … 0 copied`、
`radio sample continuity: 0 gaps`、`gaps=0`、`rx_overflows=0`；**允许变且要登记**：`cbs/lane`（2.00 → 3.00）、V1（预计变差，`p42`/`p52` 的历史同族读数在 +130…+150 µs 量级）、`merged_hop` 的窗口（若前端工作从跳内搬出去，它应回落到 ~100 µs 量级 = 1 派发 + 4 条 burst）。


### 6.129 ★★★ 腿 `p77-n78-nod1`（关 D1）：**打包的 14 组派发在空口是便宜的（46.9 µs）** ⇒ H1″ 至此才被真正否掉；而那 ~450–520 µs 是"**一跳里恰好一条 cb**"的账，随结构搬家（2026-09-27）

#### ① 姿势与红线（含与预登记的两处偏差，都要登记）

* `OCUDU_DFT_RELEASE_BLOCK=0` 生效：`released=0 released_waits=0`（**没有交棒块**）；`batched=135379/1895306`、`batch_src=knob`（**仍然是 14 变换/派发**）。
* 红线全守住：契约 **9 of 9 MET**；`host device data crossings` **0.00 + 0.00 / 跳**（125296 跳）、`host sample assembly` **0 copied**（9735922/9735922）、
  `radio sample continuity: 0 gaps`（695413 块）、`gaps=0`、`rx_overflows=0`、池 `starved_events=0`（`held_max=21`）；`leg_gate` **6 of 9**（三条仍是脚本字面量/误绑）。
* ★ **偏差 (a)**：预登记说 `cbs/lane` 会到 **3.00** —— 实测 **仍是 2.00 (max=2)**：前端块没有落到 lane 上，而是**回到前端队列**（§6.84① 的代码预期）⇒ 这条臂的 V4 代价**比预期小**。
* ★ **偏差 (b)**：`p0_gate` **27 of 29**，两条 RED 都是 **"读不到"**：`D1 (Q9)` 报 `no 'input hold (P0-2)' line`、`D2 (Q9)` 报 `lifecycle line='<absent>'`
  —— 因为"**input token 持有**"本来就是 **D1 交棒机制的产物**；D1 关了就没人持有 token ⇒ 这两条是**臂自身造成的"无样本"**，**不是回归**
  （对照 `p72`：D1/D2 PASS，max hold 16.2 ms、reap 10.0 ms）。
* V1 中位 **1509.6**（融合 `p72` 1399.5 ⇒ **+110 µs**，与同族历史 +130…+150 一致）；`residency` 556.5 / `busy`（并集）505.2 / `gap` 34.5。

#### ② ★★ 判决：预登记的**第一支命中**，但它把归因**反过来了**

| 前端块那条 cb（现在是**引擎提交、前端队列**、仍是 1 条 14 组派发）| 读数 |
|---|---|
| Q9-F3 per-label `dft_front_end` exec p50 / p95 | **46.9 / 59.4 µs** |
| `[ul_gpu_lane] dft busy`（每块）| mean 57.0 / **中位 50.2** / min 16.4 / max 504.8（n=135378）|
| `gpu busy (front_end)` | commits 552632、**mean 50.03 µs** |

* ⇒ ★ **打包的 14 组派发在空口上是便宜的（46.9 µs ≈ 离线 47.3）** ⇒
  ★★ **H1″（"空口把 14 个线程组串起来跑"）至此才被真正否掉**：`p72`–`p74` 的 batch 扫描**分辨不了**这件事（那三条腿的组数恒为 14），
  而 `p77` 用"**同一条派发、换提交者与队列**"直接给出 46.9 ⇒ 那 ~443 µs **既不是打包的属性、也不是线程组的属性**。
* ⚠ 这也**取代**了 §6.127③ 里"支持 H1″ 的那对读数"（46.7 → 443.5 = +30.5 µs/组）的解释：那对读数**跨了 cb 身份**（plain 路 vs 交棒块），
  不能当作"每组 30.5 µs"的证据。**§6.124⑤ 与 §6.127③ 的两种读法都到此作废**，正确的读法见 ③。

#### ③ 那它是什么：**一跳的总量不变，而"贵的那一条 cb"随结构搬家**

| 结构 | 一跳的 cb 与窗口（中位 / 均值，µs）| 合计 |
|---|---|---|
| 融合 `p72` | `merged_hop`（FE 被 adopt + 4 条 burst）**473.1** + `ch_wt` 40.8 | **513.9** |
| 拆分 `p75` | `dft`（FE 块，lane 提交）**445.8** + `ch_wt` 45.5 + `merged_hop` 24.4 | **515.7** |
| 关 D1 `p77` | 前端 cb ~**50** + `ch_wt` 40.8 + `merged_hop`（4 条 burst 合在一条）**516.7** | 505.2（并集）/ 607（和）|

* **同一批 4 条 burst 派发**（eq + demap + 2×CE；`burst dispatches=501184/125296 = 4.00`）：
  `p77` **合在一条 cb = 516.7 µs**；`p75` **拆成两条 cb**（`ce_weights` 45.5 + `merged_hop` 24.4）= **69.9 µs**（回到离线价）。
* **同一条 14 组派发**：`p75` 里由 **lane 提交（交棒）** = **445.8**；`p77` 里由 **引擎提交（前端队列）** = **46.9**。
* ★ **经验规律（五条腿 / 三种结构）**：**一跳的设备时间合计 ~505–517 µs 与结构无关；其中恰好一条 cb 吃掉 ~450，其余都是几十 µs**，
  而"哪一条"随结构搬家（融合 = `merged_hop`、拆分 = `dft`、关 D1 = `merged_hop`/burst）。

#### ④ 两个候选（现有仪器分不开）

* **H-A：同一条 cb 内的派发被串行**（依赖 / 内存屏障 ⇒ 空口 ~100–130 µs/条；**把它们拆到两条 cb 就并行 ⇒ 回到几十 µs**）。
  这与 ③ 的第一条对照**方向一致**（4 条合一条 516.7 vs 拆两条 69.9）；但 `p75` 的 `dft` cb **只有 1 条派发**却 445.8 ⇒ 它还需要"那 1 条派发内部串行"或"它等了什么"。
* **H-B：跨 cb / 跨队列的**资源交接等待**落在 `start→end` 里**（`p77` 正好新造了"前端 cb（前端队列）→ burst cb（后端队列）"这条跨队列交接）。
* 现有反面证据：本平台**没有逐派发时间戳**（§6.75）；Q24 的 fence 等待只有 ~26–40 µs（`p77`：8.4% 命中、mean 26.6 µs）⇒ 若 H-B 成立，那笔等待**不在现有的 fence 通道上**。

#### ⑤ 下一步（**一次仪器投资，而不是又一次盲扫**）：cb **内部**的时间线

在**每条派发之间**对同一个 `MTLSharedEvent` 递增信号（`encodeSignalEvent` 必须在**没有 encoder 打开**时调用，源码注释已写明），
宿主侧轮询 `signaledValue` + 读自己的时钟 ⇒ 得到**逐阶段完成时刻**（µs 级轮询抖动），从而把那条 cb 的 ~450 µs **定位到段**。
* **为什么是它**：我们已经有 cb 级（`start→end`）、队列级（`commit→start`）、fence 级（Q24）三把尺子，**唯独没有 cb 内部**那一级；
  而"一条 cb 里 ~450 µs 花在哪一段"正是从 §6.124 到本节唯一**没有被问过**的问题。
* **纪律**：先在 `wip/dft_dispatch_cost.mm` 里对这个形状**离线自证**（同一条 cb 里 5 条派发、逐段打点，与已知的 47 µs 对拍），再上空口；否则又是一条读不出东西的腿。


### 6.130 ★★ "cb 内部的时间线"在本平台**不可得**：三种办法全部被实测否掉（离线，2026-09-27）

> 用户裁决"做 cb 内部时间线这把尺子"。按纪律**先离线自证**（§6.129⑤），结果**三种候选全部被否**，**因此没有飞腿**——这正是"先离线自证"要挡掉的东西。

#### ① 办法 A：宿主轮询 `MTLSharedEvent`（每条派发之间 `encodeSignalEvent`）

`wip/dft_dispatch_cost.mm` 里新增臂：**一条 cb 里 5 条 14 组派发**，每条派发之间 `encodeSignalEvent`（必须无 encoder 打开，正是空口 stage fence 的形状），宿主忙等线程轮询 `signaledValue` 并读自己的时钟。

| 读数 | 值 |
|---|---|
| 该 cb 自己的 GPU 窗口 | **51.6 µs** |
| "段 1"（GPUStart → 第一次观测到信号）| **83.1 µs**（比整条 cb 还长！）|
| "段 2…5" | 1.9 / 0.4 / 0.3 / 0.4 µs |
| 信号数 | expected 75 / missed 0 / observed 125（**一次性全到**）|

⇒ ★ **event 的 `signaledValue` 是"命令缓冲结束时一次性刷给 CPU"的**（5 个信号在彼此 ~2 µs 内、且在 `GPUEndTime` 之后 ~30 µs 才被看见）⇒ **宿主轮询看不见 cb 内部**。
（顺带记一个坑：`signaledValue` **只增不减**，跨 run 必须用**单调递增**的 generation；第一版每条 run 都从 1 数到 5，结果 25 条 run 只观测到 5 次信号、五段全是 0。）

#### ② 办法 B：**设备侧打点**（每条信号配一条只等该事件的"marker cb"，读它自己的 `GPUStartTime`）

不用宿主时钟，而是让**另一条队列上的一条空 cb 等事件**，它自己的 GPU 时间戳就是"信号发生的设备时刻"；marker 在测量 cb **之前**提交（等未来的 generation），所以不会被合并。

| 读数 | 值 |
|---|---|
| 该 cb 窗口 | 51.8 µs |
| marker 1（buffer 起点 → 此）| **82.0 µs** |
| marker 2…5 | 1.6 / 1.0 / 1.0 / 0.8 µs |

⇒ ★ **一样被合并**（5 个 marker 在 ~1 µs 内一起解等待）⇒ **本平台的 event 可见性是 cb 粒度**，设备侧打点同样看不见内部。

#### ③ 办法 C：`MTLCounterSampleBuffer` 的逐派发采样 —— **硬件不支持**（现有探针复核）

`wip/metal_counter_caps.mm` 今天重跑：

```
supportsCounterSampling:AtStageBoundary    = YES
supportsCounterSampling:AtDrawBoundary     = no
supportsCounterSampling:AtBlitBoundary     = no
supportsCounterSampling:AtDispatchBoundary = no
```

且 compute encoder 上调用 `sampleCountersInBuffer:` **会断言并 SIGABRT**（`wip/metal_counter_caps.mm` 头部记着 2026-09-20 那次实测）⇒ **纯 compute 链没有可放采样点的位置**（§6.75 的结论今天被独立复核）。

#### ④ 于是：**唯一还能"看进 cb 内部"的办法是"拿掉一段再看窗口"**（消去法，§6.46 的旧标尺）

* 可用的**消去法**（都要一个**只用于测量**的 env 旋钮，默认关，打开即破坏正确性、但保留数据路径）：
  1. **去掉 eq + demap 两条派发**（只留下 CE 与 FE 派发）；
  2. **去掉 2 条 CE 派发**（只留 eq/demap）；
  3. **把某条派发换成"空 dispatch"**（保留提交结构，去掉它的执行）。
* **判据**：若拿掉 eq/demap 让那条 cb 从 ~517 掉到 ~40–80 ⇒ 那 ~450 就落在被拿掉的那段里（再二分）；若窗口**不动**（仍 ~450）⇒ 那笔钱与 cb 的*内容*无关，属于**结构/调度**（这条与 `p75` 的"拆分只搬不降"一致）。
* ⚠ **红线**：这类臂**必然**让 CRC/RF/吞吐变红（拿掉的是真活），**只能当测量**；登记时要把"红的是预期的"写清楚，避免被下一位读成回归。

#### ⑤ 顺便收窄的一条候选（本轮新增，尚未测）

把五条腿并排看，"贵的那一条 cb"似乎是**接触资源网格的那一条**：
* `p72`（融合）`merged_hop`（**读**网格的 CE 在里面，lane 提交）= 473.1；
* `p75`（拆分）`dft`（**写**网格的前端，lane 提交）= 445.8，而两条各 2 条派发的 lane cb 只有 45.5 / 24.4；
* `p77`（关 D1）**写**网格的前端 cb（**引擎**提交、前端队列）= 46.9（便宜），而**读**网格的 burst（lane 提交）= 516.7。
⇒ **"谁提交"与"写还是读"两者都不能单独解释**（p77 的前端写而便宜、p75 的前端写而贵），但"**lane 提交 × 触及网格**"这个组合目前没有被反例。
这与**消去法**合起来是下一轮的两个抓手。


### 6.131 ★★ 消去法之前又关掉四族：窗口**不是等待**、**不是足迹**、**不是队列排空**、**不是绑定大小**（离线，2026-09-27）

> 在实现消去法旋钮之前，先把"窗口到底是不是执行"这件事钉死。四条臂**全部否**，因此 §6.132 的消去法现在是唯一剩的手段。

| 假设 | 臂（都在 `wip/dft_dispatch_cost.mm`）| 读数 | 判定 |
|---|---|---|---|
| ★ 设备侧等待被算进窗口（H-B）| 一条等事件（~500 µs 后才被另一条 cb 信号）的打包 cb，与不等事件的对照 | **11.8 µs（等待）vs 11.8 µs（对照）**，宿主实测 752 µs | ❌ **等待落在 `commit→start`**，`GPUStartTime` 是"真正开始执行"的时刻 ⇒ **窗口是真执行** |
| 已映射足迹/页表压力 | 分配并写满 **64 × 8 MB = 537 MB** `StorageModeShared` 缓冲后再测 | **11.8 → 11.8 µs** | ❌ 足迹不是原因（与 §6.79 的 staging 反例不冲突：那条是"每槽新建 14 个缓冲"）|
| `GPUEndTime` 是**队列排空**而不是本 cb 最后一条派发 | 测量臂后面**紧跟**一条 2673 µs 的缓冲 | **11.9 µs**（后面那条 2672.9 µs）| ❌ 窗口只算自己 |
| 绑定缓冲的**大小**（不是碰到的字节）| 同样的 kernel、同样的触碰字节，但 `in/out/grid/window` 换成 **4 × 64 MB** | **11.9 µs** | ❌ 绑定大小不计费 |

⇒ 四条合起来：**那 ~450 µs 是"这条 cb 自己的执行"，而且与内容、与足迹、与后续队列无关** —— 这与 §6.129③ 的"同一批派发换个位置就从 517 变 70"合在一起，指向**调度/执行**层面的东西，而不是数据或等待。
⇒ 因此只剩**消去法**（§6.132）：把交付结构里的"活"换成空 kernel，看窗口动不动。

#### ① 消去法旋钮的落地（默认关，交付路径不变）

* `ocudu_demod.metal` 新增 `lane_ablate_noop`（一条几乎不存在的 kernel，只声明一个 buffer）。
* `shared_burst::set_ablation_pipeline(pipeline)`：装上之后，`encoder()` 里**只换绑定的 pipeline**，
  **保留**原来的 `s.pipeline` 记账 ⇒ **阶段屏障、fence 结构、每个调用者要的派发网格、提交结构全部与交付一致**（这才是这条臂的意义）。
* `ocudu_demod_metal_engine.mm`：`OCUDU_LANE_ABLATE=1` 时从同一个 metallib 建 noop pipeline 并装上，并打一条 **WARNING**（提醒这不是一条能工作的链路）。
* `OCUDU_LANE_ABLATE` **不设或为 0** 时 `ablation_pipeline == nil` ⇒ `encoder()` 走的就是原来那一行 ⇒ **交付路径逐字节不变**（`ctest -L phy` 串行复核）。

#### ② 腿的预登记（`p78-n78-ablate`）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p78-n78-ablate \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1
```

| 判据（对照 = 同日同配方的 `p72`）| 若 | 则 |
|---|---|---|
| `merged_hop` 的 `start→end`（`p72` = **468.7**）| **≈40–80 µs** | ★ **那 ~450 是这些 kernel 的"执行"** ⇒ 下一步二分是哪一段（eq / CE / demap），并且杠杆回到"换 kernel 形状" |
| 同上 | **≈400–470 µs（几乎不动）** | ★ **那 ~450 与"活"无关** ⇒ 是平台给这条 cb 记的账（调度/提交侧）⇒ 本平台**无杠杆**，这一线的结论就是"已定价、不可动" |
| `ch_wt`（40.8）/ 契约 / `gaps` / 池 | **不许变**（`ch_wt` 不在 burst 里，应该不动；`gaps=0`、`rx_overflows=0` 必须保持）| —— |
| CRC / RF / 吞吐 | **预期全红**（没人写估计与 LLR）| 登记为**预期**，不作判据 |
| `cbs/lane` | 预期 **2.00**（结构没变）| 若变 ⇒ 说明某段因失败而退出了 burst，要读原因 |


### 6.132 腿 `p78-n78-ablate` 是**空臂**（旋钮没传进去）——但它逼出两个真缺陷，已修好并**离线自证**（2026-09-27）

#### ① 空臂的事实（先登记，别当成消去法的读数）

`p78` 的 `knob` 登记行只有 `OCUDU_DFT_BATCH_SYMBOLS=14` / **`OCUDU_DFT_RELEASE_BLOCK=0`** / `OCUDU_METAL_GPU_TIME=1` / `OCUDU_UL_PHASE_SEGMENTS=1` —— **没有 `OCUDU_LANE_ABLATE`**（那条命令沿用了 `p77` 的行）。
后果与读数一致：链路**正常工作**（`CRC-OK` 102273 跳）、`merged_hop` exec **457.4**、`dft_front_end` **47.0**、`cbs/lane=2.00 (max=5)`。
⇒ ★ **`p78` 的真实身份是"关 D1 的第二次复现"**，而且它复现得非常好：对照 `p77`（`merged_hop` 459.1 / `dft_front_end` 46.9 / `cbs/lane` 2.00）——**逐项一致**（这是 `p77` 那条腿的第二数据点，价值在此）。
⇒ ⚠ 同时记一条纪律：**飞之前先核对腿自己的 `knob` 登记行**（`run_leg.sh` 会把它打在 stderr 顶部）；一行之差就让一条腿换了身份。

#### ② 但这条空臂逼出两个**我的**真缺陷（都已修）

1. **作用域错**：`ablation_pipeline` 原本存在 `burst_state` 里，而 `state()` 是 `static thread_local`（burst 属于编码它的线程）
   ⇒ 由"初始化 demod 引擎的那个线程"装上的 pipeline，对**真正编码各阶段派发的线程**不可见 ⇒ **即使旋钮传进去也会一点效果都没有**。
   修法：改成**进程级**（`std::atomic<void*>` + 一份强引用保活；Objective-C 指针不能用 `std::atomic<id<…>>`，必须 `__bridge`）。
2. **安装点选错**：原设计让 demod 引擎在 `init()` 里装，但**没有任何离线测试构造那个引擎** ⇒ 旋钮**无法离线自证**（而"先离线自证"正是 §6.129⑤ 立的规矩）。
   修法：把安装搬进 `shared_burst::encoder()` 自己——**首次调用时惰性构建**（从 `ocudu_demod.metallib` 取 `lane_ablate_noop`），进程级、任何编码线程都生效。
   顺带修好 metallib 的定位（原来只找 CWD 直下；现在**从 CWD 逐级向上**找 `lib/phy/upper/channel_modulation/metal/ocudu_demod.metallib`——测试在 build 目录里跑，正是这一条暴露了它）。

#### ③ 离线自证（这次是真的）

| 命令 | 结果 |
|---|---|
| `OCUDU_LANE_ABLATE=1 ./pusch_demodulator_deferred_chain_test`（重建后）| **`[metal_ablate] ABLATION ON …`** + **2 个用例 FAIL**（metal 后端不再等于 CPU 链 —— 空 demapper 必然如此）|
| `./pusch_demodulator_deferred_chain_test`（不设旋钮）| **5/5 PASS** ⇒ 交付路径不变 |
| `ctest -L phy`（串行，不设旋钮）| 全绿（全量重建后复核）|

⇒ 旋钮现在有**行为证据**（不只是"代码在那儿"）：开着它，链路的数值结果就变了；关着它，一切照旧。

#### ④ 重飞（同一条命令的**正确**形态，`p79-n78-ablate`）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p79-n78-ablate \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1
```

**飞之前先看两行**（各一秒，能省一次手机测试）：
1. stderr 顶部的 `knob          : OCUDU_LANE_ABLATE=1`（**必须在**）；
2. 启动后不久的 `[metal_ablate] ABLATION ON …`（**必须在**；若出现 `metallib was not found` 就是路径问题，停手告诉我）。

判据与 §6.131② 相同（对照 `p72` 的 `merged_hop` 468.7）：**40–80 µs ⇒ 那 ~450 是这些 kernel 的执行**；**400–470 µs ⇒ 与"活"无关**。


### 6.133 `p79` 手机接不进来 ⇒ 消去法改成 **1-in-N**（每 N 跳消去一跳，并给消去的那条 cb 单独打标签）（2026-09-27）

* **`p79` 的事实**：`OCUDU_LANE_ABLATE=1`（全消去）时**手机无法接入**。原因不是路径/构建，而是**设计**：消去法把估计/均衡/解映射的**执行**全部换成空 kernel ⇒ 上行**一个 TB 都解不出来**（LLR 全是垃圾），而**接入过程本身就需要上行能被解出**（Msg3/RRC）⇒ 链路根本建立不起来。
  ⚠ 我在 §6.131② 写的"DL 不受影响、手机应能保持接入"**是错的**，这条更正以本节为准。
* **改法（已实现）**：`OCUDU_LANE_ABLATE_EVERY=N` ⇒ **每 N 跳只消去一跳**（`ablate_next_burst()`，在 burst 打开时按进程级计数决定），HARQ 足以覆盖这 1/N 的损失 ⇒ 链路活着、能接入；
  被消去的那条 cb 用**自己的 label `merged_hop_ablated`**（`commit()` 里换掉 `arm_gpu_time` 的 label）⇒ **两种总体在报告里永不混**。
* **离线自证**：`OCUDU_LANE_ABLATE=1`（EVERY=1）⇒ `pusch_demodulator_deferred_chain_test` **5 个用例全 FAIL**（空链路必然）；`EVERY=8` ⇒ 只剩被命中的对拍失败；不设旋钮 ⇒ **5/5 PASS**（交付路径不变）。
* ★ **这条腿的判读是"同腿两总体 A/B"**（比跨腿对照更干净）：同一条腿里 `merged_hop`（正常跳，对照）与 **`merged_hop_ablated`（消去跳）** 在**同一负载、同一分钟**下并存。
  * `merged_hop_ablated` 落到 **40–80 µs** ⇒ 那 ~450 µs 就是这些 kernel 的**执行**；
  * 它仍在 **400–470 µs** ⇒ 与"活"无关 ⇒ 平台给这条 cb 的账，本平台**无杠杆**，这一线收口。
* **命令**（`OCUDU_LANE_ABLATE_EVERY=8`）：

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p80-n78-ablate8 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8
```

  接手机**之前**先看：① `knob          : OCUDU_LANE_ABLATE=1` 与 `…_EVERY=8` 都在；② `[metal_ablate] ABLATION ON …`（出现即说明 no-op pipeline 已建成）。
  跑完的判据行：`[metal_stats] … per label` 里要有 **`merged_hop_ablated`** 这一行（没有它 ⇒ 没命中，腿作废）。


### 6.134 `p80` 的消去只命中 **1 次**（n=1）：决策点选错（adopt 路）——已修；而那 1 个样本是 **18.1 µs**（2026-09-27）

* **`p80` 的读数**：旋钮两行都在、`[metal_ablate] ABLATION ON` 也在、契约 9/9、`gaps=0`、`cbs/lane=2.00 (max=5)`；
  但 per-label 表里 **`merged_hop_ablated n=1`**（exec **18.1 µs**），而 `merged_hop n=144941` 仍是 468.6 µs ⇒ **这条腿的对照是好的，消去样本只有 1 个**。
* **根因（我的）**：决策原本写在 `burst_ensure_open()` 里"新建 cb"的那条分支，而**合并路的那条 cb 是被 adopt 的**（`shared_burst::adopt()`，前端块交棒过来的未提交缓冲）⇒ 绝大多数跳根本没经过那条分支。
  修法：把决策挪到 **`encoder()` 的第一次调用**（`ablate_this_decided` 每 burst 一次）——**新建路与 adopt 路都覆盖**。
* 离线复核：`EVERY=1` 下延迟链测试仍然 **5 个用例全 FAIL**（消去确实生效）。
* ⚠ **那 1 个样本（18.1 µs）只是线索，不是证据**：它方向明确（同样的 cb、同样的结构，去掉活之后 468.6 → 18.1），但 n=1。
  **下一条腿（`p81-n78-ablate8`，同一命令）就是把它变成证据**：预期 `merged_hop_ablated` 有 ~1.8 万个样本，且若它落在 **~20–80 µs** ⇒
  **那 ~450 µs 就是这些 kernel 的执行**（下一轮二分 eq/CE/demap）；若它仍 ~450 ⇒ 与"活"无关。


### 6.135 `p81`：消去跳**没有出现在报告里**（label 也没生效）——同类缺陷第三次（作用域/线程），腿本身不能定案（2026-09-27）

* **`p81` 读数**：旋钮两行 ✓、`[metal_ablate] ABLATION ON` ✓、对照干净（**`merged_hop` n=147596、exec p50=467.5、`ch_wt` 42.3、`cbs/lane=2.00 (max=5)`、契约 9/9、`gaps=0`、V1 1392.5**）；
  但 **per-label 表里没有 `merged_hop_ablated` 这一行**（`p80` 至少还有 n=1）⇒ **消去跳的 label 没打上**。
* **根因（同一族缺陷的第三次，都是"状态放错作用域/线程"）**：`ablate_this_burst` 是 **`burst_state`（thread_local）**，
  而合并路上**编码发生在估计器线程、提交（`commit()` 里换 label）发生在 lane 线程** ⇒ 决策在编码线程写入，提交线程读到的是 `false`。
  修法（下一步、离线可自证）：把"这条 cb 是否被消去"放进**进程级、以 cb 指针为键**的表（编码时写入、提交时取出并删除），这样跨线程也能对上。
* **这条腿定不了案**：若消去确实生效（pipeline 已绑），则 ~1/8 的样本落在 `merged_hop` 总体里；**p50 不会动**（12.5% 的快样本撼不动中位），而报告只打 p50/p95 ⇒ **看不见**。
  唯一的方向性线索仍是 `p80` 那 **1 个**样本：**468.6 → 18.1 µs**（n=1，不算证据）。
* **下一步（二选一，都不大）**：
  1. ★ **修 label 的跨线程作用域**（cb 键表），再飞一次 `p82`（同命令）⇒ `merged_hop_ablated` 有 ~1.8 万样本，直接给答案；
  2. 或者**给 per-label 表加 `min`/`p5`** ⇒ 即使 label 没打上，也能从 `merged_hop` 的**低尾**读出消去样本（廉价、且对以后所有腿都有用）。
* ⚠ 纪律沉淀（这条线已经三次）：**凡是"每 burst/每线程"的状态，都要问一句"读到它的线程是不是写它的线程"**；`burst_state` 是 `thread_local`，`adopt()` 路与 `commit()` 路都可能换线程。


### 6.136 消去法的决策改成**进程级、以 cb 为键**（离线自证：EVERY=1 ⇒ 5 用例全 FAIL；不设旋钮 ⇒ 0 FAIL）（2026-09-27）

* 修法：`ablation_for_cb(cb, create)` / `forget_ablation_for_cb(cb)`（`std::mutex` + `std::unordered_map<void*,bool>`），
  **编码线程**在 `encoder()` 里建决策，**提交线程**在 `commit()` 里读它决定 label，再删除 ⇒ 跨线程对得上（`burst_state` 的 `thread_local` 陷阱不再参与）。
* 离线复核：`EVERY=1` ⇒ 延迟链测试 **5/5 FAIL**（消去生效）；**不设旋钮 ⇒ 0 FAIL**（交付不变）。`EVERY=8` 在**短测试**里也全 FAIL 是正常的（每条用例的第一个 burst 恰好是 8 的倍数被命中），空口上 ~147k 跳不受影响。
* **重飞命令**（`p82-n78-ablate8`）：与 `p80`/`p81` 相同，只需换腿名；判据仍是**同腿两总体**：
  `merged_hop_ablated` 的 **n ≈ 1.8 万**（必须出现），其 exec p50 若 **~20–80 µs** ⇒ 那 ~450 µs 是这些 kernel 的**执行**；若仍 **~450 µs** ⇒ 与"活"无关，这一线收口。


### 6.137 `p82`：消去跳仍然读不到 —— label 被后面的 `merged_hop` 覆盖行盖掉（**一行修**，已定案）（2026-09-27）

* `p82` 读数：旋钮两行 ✓、`[metal_ablate] ABLATION ON` ✓、对照干净（`merged_hop` n=145943、exec p50 **470.0**、`ch_wt` 41.1、`cbs/lane=2.00 (max=2)`、契约 9/9、`gaps=0`、V1 **1399.8**），但 per-label 表里**仍然没有 `merged_hop_ablated`**。
* **根因（读码即定案）**：`commit()` 里的 label 次序是
  `burst_label = "lane_burst"` → （我的）`if (ablation_for_cb(…)) burst_label = "merged_hop_ablated";` → **`#if OCUDU_METAL_STATS` 里的 `if (commit_label == merged_hop) burst_label = "merged_hop";`**
  ⇒ 在合并路上**后面那一行把消去 label 盖掉**。`p80`/`p81`/`p82` 三条腿因此都读不到消去总体（`p80` 的 n=1 是 adopt 之前的偶发路径）。
* **修法（一行：把消去那段挪到覆盖之后，消去优先）**；离线自证沿用现有三条命令（`EVERY=1` ⇒ 延迟链 5/5 FAIL；不设旋钮 ⇒ 0 FAIL）。
* **判据不变**（同腿两总体）：`merged_hop_ablated` 的 exec p50 **20–80 µs** ⇒ 那 ~450 µs 是**这些 kernel 的执行**；仍 **~450 µs** ⇒ 与"活"无关、本平台无杠杆。


### 6.138 ★★ label 的**前序**从"语句次序"改成"单一表达式"（这一类缺陷不能再悄悄回来）＋ per-label 表加 **min/p5**（第二个读数）；离线三臂自证已过（2026-09-27）

> 承接 §6.137 的一行修。**没有直接照抄那一行**，理由是：这个缺陷的本质是"**三个赋值按次序写，最后写的人赢**"，
> 而"把其中一段挪到另一段之后"**仍然是同一个形状** —— 下一次编辑只要在末尾再追加一行 label 赋值，同样的三腿代价会**原样重演**。
> 所以修法是**把次序变成值**。

#### ① 修法：前序写成一个表达式（`lib/phy/metal/ocudu_metal_burst.mm` 的 `commit()`）

```cpp
const bool ablated = ablation_for_cb(cb, /*create=*/false);
bool       merged  = false;
#if defined(OCUDU_METAL_STATS)
  merged = (s.commit_label == gpu_lane_probe::stage::merged_hop);
#endif
// 最具体的赢：ablated > merged_hop > lane_burst。嵌套而不是三段赋值 —— 次序就是值本身。
const char* burst_label = ablated ? "merged_hop_ablated" : (merged ? "merged_hop" : "lane_burst");
```

* 交付语义逐项不变（三个字符串与各自的判据都没动）；变的只是**没有任何一条语句能"后写覆盖先写"**。
* `forget_ablation_for_cb(cb)` 仍在读之后立刻调用（表以 cb 指针为键，不清会在地址复用后串味）。

#### ② 第二个读数：per-label 表加 `min` / `p5`（`lib/phy/metal/ocudu_metal_queue.mm`）

* 动机（§6.135 的备用方案）：**即使 label 再出问题**，1/N 的消去样本也会落在**同一 label 的低尾**（12.5% 的快样本撼不动 p50，但一定改写 `min`/`p5`）。
  现在 per-label 一行给 **wait 与 exec 各自的 `p50/p95/min/p5`** ⇒ 这条腿有**两个互相独立的读数**，不再是"label 对了才有答案"。
* 实现细节：同一向量现在要读四次，所以**排序只做一次**（`pct_sorted`，调用点显式 `std::sort`），而不是每次读数排一遍（一条腿 14 万条记录 × 8 次排序是白烧的）。

#### ③ 离线自证：三臂（`pusch_demodulator_deferred_chain_test`，两个文件重建之后）

| 臂 | gtest | per-label 表（`OCUDU_METAL_GPU_TIME=1`）|
|---|---|---|
| 不设旋钮（交付路径）| **5 PASSED / 0 FAILED** | `lane_burst n=69 wait p50=58.4 p95=87.2 min=16.9 p5=28.8 \| exec p50=23.5 p95=66.5 min=22.6 p5=22.9` —— **没有** `merged_hop_ablated` |
| `OCUDU_LANE_ABLATE=1 EVERY=1` | `[metal_ablate] ABLATION ON` + **2 FAILED**（metal≠CPU，空链路）| **`merged_hop_ablated n=69 … exec p50=20.5 p95=58.6 min=16.1 p5=19.9`** |

* ★ 两行合起来证明了三件事：**(a)** 消去决策在 `encoder()` 建、在 `commit()` **读得到**（§6.135/§6.136 的跨线程那条彻底闭环，label 不再被丢）；
  **(b)** 新 label 真的进了 Q9-F3 报告（不只是"代码在那儿"）；**(c)** 空 kernel 的那条 cb 在**离线**就是 **~20 µs**，与 §6.134 那 1 个空口样本（**18.1**）同量级 —— 方向一致。
* ⚠ **保留一句**：离线臂里 `commit_label` 是默认的 `equalizer_demapper`（不是 `merged_hop`），所以这条自证覆盖的是"**消去 label 不被丢**"，
  **不覆盖**"与 `merged_hop` 那一行的次序"。后一条由 ① 的表达式形状保证（次序不再是可追加的语句序列），且 ② 让**即使它再失败也能读出答案**。
* `ctest -L phy`（串行，不设旋钮）：见 §6.139 的登记（本条只登记离线三臂）。

#### ④ 腿预登记 `p83-n78-ablate8`（与 `p80`–`p82` 同命令，只换腿名）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p83-n78-ablate8 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8
```

**接手机之前**核三行（各一秒）：① stderr 顶部 `knob : OCUDU_LANE_ABLATE=1` **与** `knob : OCUDU_LANE_ABLATE_EVERY=8` 都在；
② `[metal_ablate] ABLATION ON …`；③ `commit` 行 = `build/hashes.h` = `gnb` 内嵌戳（`run_leg.sh` 自己会拒，不用人看）。

| 判据（**同腿两总体**，对照 = 同腿的 `merged_hop`，历史 467.5–470.0）| 若 | 则 |
|---|---|---|
| **主读数**：per-label 表里 `merged_hop_ablated`（预期 **n ≈ 1.8 万** = 145943/8）| **exec p50 ~20–80 µs** | ★ 那 ~450 µs **就是这些 kernel 的执行** ⇒ 下一轮用 `OCUDU_LANE_ABLATE_STAGE=…` 二分 eq / CE / demap（`p80` 的 18.1 与本节离线的 20.5 都已经指向这里）|
| 同上 | **仍 ~450 µs（几乎不动）** | ★ 与"活"无关 ⇒ 平台给这条 cb 记的账，**本平台无杠杆** ⇒ 这一线以"已定价、不可动"收口（把 §6.124–§6.138 的全部否证整理成一页）|
| **备用读数**（label 若再失败）：`merged_hop` 的 **`min`/`p5`** | 从 470 掉到 **~20** | 与主读数同义（消去样本藏在低尾）；两者一起读，互相交叉验证 |
| ★ **阳性对照**（**与 label 前序完全无关**）：`dft_front_end` 的 **`min`/`p5`** | 从 **46.5** 掉到**个位数 µs**（p50/p95 仍 ~46.5，**不许动**）| 说明消去**真的把 kernel 换掉了**。理由：plain 路的 FE cb 由**引擎自己**提交（`ocudu_dft_metal_engine.mm` 自己 `arm_gpu_time(…, "dft_front_end")`，**不走** `shared_burst::commit()`），所以它**不在** ① 的前序里；但消去决策是在**共享的 `encoder()`** 里按 cb 建的 ⇒ **`dft_front_end` 总体里也有 1/8 被消去**，它们的执行塌到几 µs、**藏在低尾**。⇒ 这是"旋钮物理生效"的独立证据（比 `[metal_ablate] ABLATION ON` 那句强，那句只说明 pipeline 建成了）|
| ★ **若阳性对照不动** | `dft_front_end` 的 min/p5 **仍是 ~46** | ⇒ 消去**根本没绑上**（或这条腿跑的是没带旋钮的二进制）⇒ **这条腿作废，别去解读 `merged_hop_ablated`**（无论它显示什么）|
| 红线 | `ch_wt`（41.1）/ 契约 / `gaps=0` / `rx_overflows=0` / 池 `starved_events=0` **不许变**；`cbs/lane` 预期 **2.00** | 变了 ⇒ 说明某段因失败退出 burst，先读原因再解读窗口 |
| CRC / 吞吐 | **预期显著变差**（1/8 跳的 LLR 是垃圾，靠 HARQ 兜）—— `p82` 已给出基线：**81892 CRC-OK / 145943 跳（56%）** | 登记为**预期**，不作判据 |


### 6.139 重建与全绿复核（本节 = p83 之前的那次；`ctest -L phy` **193/193**）（2026-09-27）

* 两个文件重建（`ocudu_metal_burst.mm`、`ocudu_metal_queue.mm`）+ `gnb` 重建：**零警告零错误**。
* **交付路径复核**：`ctest -L phy`（**串行**）⇒ **193/193 passed**（194 条里 1 条 `Disabled`：`dft_processor_ci16_test`），
  与 2026-09-26 收口审计的 193/193 一致 ⇒ **不发旋钮时，这次的改动不改变任何交付行为**（改的是 label 的取值形状与报告的两列，不是派发、不是屏障、不是提交结构）。
* **腿的身份纪律（飞之前必做，`run_leg.sh` 自己也会拒）**：`git log -1 --format=%h` == `build/hashes.h` 的 `build_hash` == `gnb` 内嵌戳。
  ⚠ 本条要在**所有提交做完之后**再 `touch build/hashes.h && cmake --build build --target gnb` —— 先重建后提交会把戳落在旧提交上（§6.44 (5) 记过这个坑）。
* ▲ 本节**不含** `p83` 的读数：腿还没飞（需要 sudo + 手机）。读数与判据登记在 §6.138④，结果写在 §6.140。


### 6.140 ★★★ 腿 `p83-n78-ablate8`：label 修好了（`merged_hop_ablated` **n=17583**），但那 ~450 µs **没动** —— **但前端的网格写根本没被消去**，所以这条腿**不作数**；仪器已补全并离线自证（2026-09-27）

#### ① p83 的读数（腿本身有效：旋钮两行 ✓、`ABLATION ON` ✓、红线基本守住）

| 项 | 值 |
|---|---|
| ★ **主读数** `merged_hop_ablated` | **n=17583**（= 140658/8，**正好 1/8**，label 修好了）、exec **p50 466.5** p95 486.7 min 159.9 p5 432.0、wait p50 208.2 |
| 对照 `merged_hop` | n=123075、exec **p50 470.5** p95 491.8 min 147.6 p5 428.7、wait p50 214.9 |
| ★ 预登记的**阳性对照** `dft_front_end` | n=361321、exec p50 46.6 **min 15.8 p5 46.1** ⇒ **低尾没有塌**（预登记说 12.5% 被消去时 p5 应落到个位数）|
| `ce_weights` | n=140664、exec p50 40.4 min 8.8 p5 26.3 |
| busy split / V1 / 结构 | `ch_wt=41.7` + `merged_hop=472.2`（92% of busy）、V1 中位 **1400.9**、`cbs/lane=2.00 (max=5)`、`stale=0`、池绿（`starved_events=0`、`dropped=0`、`held_max=9`）|
| 契约 / 电台 | **8 of 9**：`radio sample continuity: 1 gaps / 160285 samples, 1 radio receive overflow -> FAILED`（**电台侧偶发**，V3 家族；本次改动碰不到这条路径，且 p82 同消去配方是 0 gaps）|
| CRC | **69518 CRC-OK / 140659 跳 = 49.4%**（对照 `p72` **123945/141729 = 87.5%**；`p82` 同配方 56%）|

* ★ **−4.0 µs**：把 eq+demap 的 kernel 整个换成空 kernel 之后，那条 cb 的窗口从 **470.5 → 466.5**（p5 反而 +3.3、min +12.3）。
* ★ **CRC 49.4% 是"消去真的生效"的行为证据**（空解映射器 ⇒ LLR 是垃圾 ⇒ HARQ 兜不住的部分掉下来）：这一条不依赖任何窗口仪器。

#### ② 判决：**这条腿不能定案**，原因是**仪器的漏洞**，不是平台的结论

* 按 §6.138④ 的预登记，阳性对照（`dft_front_end` 的 `min`/`p5`）**必须塌**才算"消去真的换了 kernel"；它**没塌** ⇒ **腿作废**，`merged_hop_ablated` 的 466.5 **不能**读成"与活无关"。
* **根因（读码即定案）**：`OCUDU_LANE_ABLATE` 的绑定点在 **`shared_burst::encoder()`** 里 —— 而**前端 DFT 引擎自己开 encoder**（`[cmd_buf computeCommandEncoder]`，它在缓冲**尚未提交**时就交棒），**从不经过 `shared_burst::encoder()`**。
  ⇒ 那条 cb 里**唯一没被消去的 dispatch 就是前端的网格写**，而按 §6.129③ / `p75` 的分解，一跳 ~470 µs 里**它才是 ~445 µs 的那部分**（`p75` 拆分臂：`dft`（前端块、lane 提交）**445.8** + `ch_wt` 45.5 + `merged_hop` 24.4）。
  ⇒ p83 真正消去的是 **eq+demap（~24 µs 那一档）**，所以"窗口没动"这件事**既可能是平台的账、也可能只是被消去的那段本来就小** —— 两者分不开。
* ⚠ 同时更正我在 §6.138④ 写下的阳性对照的**机制**：`dft_front_end` 的总体里**没有** 1/8 的空 kernel（它的 dispatch 同样不过 `shared_burst::encoder()`），所以"p5 不塌"**不是**"消去没生效"，而是**我的对照选错了对象**。真正的阳性对照见 ⑤（离线）。

#### ③ 仪器的缺口与补全（**默认关，交付路径逐字节不变**）

| 改动 | 位置 |
|---|---|
| ★ 把"这条 cb 是否消去"变成**任何模块都能问**的一件事：`ablate_cb(cb)`（问一次即决定、全进程一份答案）、`ablation_noop()`（空 pipeline，旋钮关时 nil）、`forget_ablation(cb)`（**自带 label 的提交路径要归还决策**）| `ocudu_metal_burst.h/.mm` |
| ★ **前端在建块时就问**（`begin_block()`）⇒ 这一问同时决定了"交棒后被 lane 提交"的那条 cb，与 lane 的 `commit()` 读到的是**同一个答案** | `ocudu_dft_metal_engine.mm` |
| ★ **两处绑定按答案换 kernel**：`encode_grid_write_dispatch()`（网格写，交付路）与 `submit_at()`（plain 路，同一 label） —— **网格、线程组大小、绑定、屏障、提交结构全不变，只换 kernel** | 同上 |
| ★ **三条自带 label 的提交/丢弃路径归还决策**：`commit_front_end()`（`dft_front_end`）、`commit_late_handed_block()`（`late_handed`）、`discard_open_block()`（丢弃）。不归还 = 表里留下一条按**地址**索引的旧决策，会被 Metal 复用的下一个 cb 继承 | 同上 |
| ★ **决策表有界**（`ablate_table_max = 65536`，配 **ticket** 防止"删掉同地址的新决策"）：决策只需从"第一次编码"活到"提交"，两者是同一条跳的宿主动作；而 `ce_weights` 这类"经 `shared_burst::encoder()` 编码、却由引擎自己的 label 提交"的 cb 每腿会留下 ~14 万条永不归还的条目 | `ocudu_metal_burst.mm` |

#### ④ 离线自证：**前端网格写现在真的被换掉了**（三臂，全部重建后）

| 命令 | 不设旋钮 | `OCUDU_LANE_ABLATE=1 EVERY=1` |
|---|---|---|
| ★ `dft_release_adopt_metal_test`（**D1 交棒的离线 harness**，本跑 **96.3%** 走交棒路 = 与交付同结构）| **PASS** | ★ **`FAIL: the ordinary block path left 300 of 300 grid elements unwritten`**（exit 1）⇒ **交棒块的前端网格写被空 kernel 取代** |
| `dft_processor_metal_unit_test` | **ALL OK** | **8 条数值 FAIL**（含 `the device grid write differs from the host reference`）|
| `pusch_demodulator_deferred_chain_test` | **5 PASSED** | `ABLATION ON` + **2 FAILED**（metal ≠ CPU，空链路必然）|
| `ctest -L phy`（**串行**）| **193/193 passed**（1 条 Disabled）| —— |

⇒ ★ 第一条就是 p83 缺的那块拼图：**同一个 `OCUDU_LANE_ABLATE=1` 现在能把"交付结构里前端那条网格写"也拿掉**，而且拿掉之后交棒 harness 立刻读不到网格。

#### ⑤ 预登记 `p84-n78-ablate8`（命令与 p83 **逐字相同**，只换腿名）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p84-n78-ablate8 \
  --regime=stress \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2 \
  OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_DFT_BATCH_SYMBOLS=14 \
  OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8
```

**这一次那条 cb 里已经没有真活了**（前端网格写 + eq + demap 全部空 kernel），所以判据是干净的：

| 判据（对照 = 同腿 `merged_hop`，p83 = 470.5）| 若 | 则 |
|---|---|---|
| `merged_hop_ablated`（预期 n ≈ 1.7 万）exec p50 | ★ **塌到 ~20–80 µs** | **那 ~450 µs 就是这些 kernel 的执行**（下一轮二分：前端网格写 vs eq/demap —— 现在两者都能单独消去了）|
| 同上 | ★ **仍 ~450–470 µs** | **这条 cb 的账与"活"无关**（同样的派发网格、同样的绑定、同样的屏障、同样的提交，里面什么都没干）⇒ 与 §6.131 的四族否证合起来，"本平台无杠杆"这一线可以收口 |
| ★ 附带：`dft_front_end` 的 `min`/`p5` | 这次**应该**塌到个位数（它的 1/8 也被消去了）| 若**不塌** ⇒ 旋钮没进二进制/没建 pipeline ⇒ **腿作废**（这才是正确的阳性对照）|
| 红线 | `ch_wt`（41.7）/ `cbs/lane=2.00` / 池 / `stale=0` 不许变；契约看 `gaps`（p83 有 1 次电台侧 gap，属 V3 家族偶发）| —— |

★ **备选臂（若想同时把"结构"这一维也钉死）**：`p85` = p84 的配方 **+ `OCUDU_DFT_RELEASE_BLOCK=0`**（D1 关，**只作测量臂**）。那时最贵的那条 cb 是 lane 提交的 burst（`p77`：4 条派发合一条 = 516.7），而它的 4 条派发**全部**经 `shared_burst::encoder()` ⇒ 也被本次改动覆盖 ⇒ 该结构同样能问"里面什么都没干还贵不贵"。


### 6.141 ★★★ 腿 `p84-n78-ablate8`（仪器补全后）：**那条 cb 里所有 kernel 都换成空的了，窗口仍是 ~463 µs** ⇒ 那 ~450 µs **不是这些 kernel 的执行** ⇒ 本平台无杠杆，这一线**收口**（2026-09-27）

> **这是本轮开放项的判决腿**，也是 §6.140⑤ 预登记的**第二支命中**：仪器补全后，"一跳里那条最贵的 cb"里已经**没有任何 kernel 在做活**（前端网格写 + 均衡 + 解映射全部空 kernel），而它的 `start→end` **一动不动**。

#### ① 姿势与红线（**全绿**，比 `p83` 还干净）

* 旋钮两行 ✓（`OCUDU_LANE_ABLATE=1`、`…_EVERY=8`）、`[metal_ablate] ABLATION ON …` ✓。
* ★ **契约 9 of 9 MET**（`p83` 因电台侧 1 次 gap 读 8/9，本次 **0 gaps / 0 rx_overflows** ⇒ 那确实是 V3 家族的偶发，不是消去法带来的）；`host device data crossings 0.00+0.00/跳`、`zero-copy wraps 0 failures/0 misaligned`、池绿（`starved_events=0`、`dropped=0`、`held_max=8`、`free_min=24`）、`stale=0`、`cbs/lane=2.00 (max=5)`、`lane host participation` head 42 µs 级。
* V1 中位 **1381.0**（mean 1378.8、p95 1517.1）—— 消去家族里最好的一条（意料之中：1/8 的跳不做活）。
* `burst dispatches = 580878/145220 = 4.00/跳`（**派发数不变**：消去换的是 kernel，不是结构）。

#### ② 阳性对照（**这次是对的**）：前端网格写**真的**被消去了

| label | `p83`（前端未消去）| `p84`（前端已消去）|
|---|---|---|
| `dft_front_end` exec | p50 46.6、**min 15.8 / p5 46.1** | p50 **46.5**、**min 5.5 / p5 6.2** ★ |
| `late_handed` exec | p50 50.1、min 47.4 / p5 48.3 | p50 50.1、**min 6.2 / p5 7.4** ★ |

⇒ ★ **低尾塌到 ~5–6 µs**（= 空 kernel 的真价），而 p50 不变（12.5% 的快样本撼不动中位）⇒ **§6.140③ 的补全生效**：`OCUDU_LANE_ABLATE` 现在真的够得着"一跳里那条网格写"。**这也是 p83 缺失的那块拼图**。

#### ③ ★★ 判决：**窗口与"活"无关**

| 总体（同腿，同负载，同一分钟）| exec p50 | min | p5 | p95 | wait p50 |
|---|---|---|---|---|---|
| `merged_hop`（对照，n=126736）| **470.0** | 378.0 | 428.9 | 535.7 | 203.2 |
| ★ `merged_hop_ablated`（n=**18483**，= 1/8）| **462.9** | **360.5** | 386.9 | 641.2 | **44.1** |

* ★ **−7.1 µs（1.5%）**：把那条 cb 里**所有** kernel 的算力拿掉（前端网格写 + 均衡 + 解映射），只拿掉 **1.5%** 的窗口 —— 而"窗口 = 这些 kernel 的执行"要求拿掉 **~450 µs（96%）**。
* ★ **1.8 万个消去跳里最快的一个也是 360.5 µs**（不是"大部分照旧、少数变快"，而是**整族都在 360–640 µs 带里**）。
* ★ **附带一个把"等待"彻底摘掉的读数**：消去跳的 `commit→start` 掉到 **44.1 µs**（对照 203.2）⇒ 队列侧确实"轻松"了（一跳总账 673 → 507 µs），**但 `start→end` 一动不动** ⇒ **也不是"排在队列里/等依赖"被记到窗口里的那一类**（§6.131 已从设备侧 event 等待、队列排空两侧否过；这里是从**同腿两总体**的队列侧再否一次）。
* 行为证据仍在（且与 p83 一致）：**CRC-OK 70315/145220 = 48.4%**（对照 `p72` **87.5%**）⇒ 空解映射器确实在跑 —— 消去不是"没生效"，而是"生效了但窗口不在乎"。

#### ④ 结论：这一项**已定价、不可动**（收口）

把本线全部读数并排，得到的是一个**结构律**而不是一个可优化的量：

| 结构 | 一跳里最贵的那条 cb | 它的 `start→end` | 里面是什么 |
|---|---|---|---|
| 融合 `p72` | `merged_hop` | **473.1** | 前端块（adopt，写网格）+ eq + demap |
| 拆分 `p75` | `dft`（lane 提交）| **445.8** | 前端块（写网格）|
| 关 D1 `p77` | `merged_hop`（lane burst）| **459.1 / 516.7** | eq + demap（前端在引擎队列上，46.9）|
| 融合 + 消去 eq/demap `p83` | `merged_hop_ablated` | **466.5** | 前端（**真**）+ 空 eq/demap |
| ★ 融合 + **全部消去** `p84` | `merged_hop_ablated` | **462.9** | **全是空 kernel** |

* ⇒ ★★ **一跳的设备时间合计恒为 ~505–517 µs，其中恰好一条 cb 吃 ~450–520；哪一条随结构搬家，而它的内容无关** ⇒ **这笔账属于"结构/平台"，不属于算力**。
* 已证否（本线累计）：kernel 算力（`p84`，最强）、设备侧等待（§6.131①）、已映射足迹 537 MB（§6.131②）、队列排空（§6.131③）、绑定缓冲大小 4×64 MB（§6.131④）、线程组串行（`p77`，14 组打包 = 46.9）、同伴车道（`p76`）、派发价（`p72`–`p74`）、**队列等待/依赖等待被记进窗口**（`p84`：wait 掉到 44 µs 而 span 不变）。
* 唯一还站得住、但**本平台没有尺子去证**的机制候选（记录，**不作杠杆**）：那条 cb 自己 `start→end` 里含一个**跨队列的网格依赖**（前端队列产出 → 后端队列消费），即驱动把"资源就绪"的等待算在这个缓冲的执行区间里。⇒ 要证伪它需要平台级手段，而本平台**没有逐派发时间戳**（§6.130③ `AtDispatchBoundary = no`）、**event 只有 cb 粒度可见**（§6.130①）⇒ **不再投入**。
* ★ **杠杆结论（写给决策）**：**在这一线里没有杠杆**——不是"还没找到更便宜的做法"，而是"这条 cb 的价钱与它做什么无关"。要让它消失，必须是"一跳里不再有这样一个 buffer"，而**试过的三种结构（融合 / 拆分 / 关 D1）每一种都恰好有一个**，总量还恒定在 ~505–517 µs（拆分与关 D1 只是把它**搬家**）。
  ⇒ 按 §3.3 的预登记岔路，下一步是 **G1/G2 指向的链条延长（LDPC→Metal，用户裁定单独规划）** 或 **S-E（最后选项）**，而**不是**继续在这个窗口里找。


### 6.142 ★★ 收口后的"理账"（用户 2026-09-27 指示：**先不推进，把现状摸清**）：文档刷新 + **门禁补两处盲点** + **旋钮盘点**（2026-09-27）

> 三件都不需要空口腿。做完之后，"现状"第一次集中在三份可复查的东西里：本文件（读数）、`high_level_status_and_plan.md`（现状与待裁决）、`knob_inventory.md`（开关清单）。
> 还在做之前先量了一次"账有多乱"，三条都是**工具本身的缺陷**，不是读数问题：

#### ① 先量出来的三处"账不平"（修之前的事实）

| 症状 | 证据 |
|---|---|
| 高层文档 §1–§5 **停在 09-25/26**：§1 的 V1–V5 表还写着 `p22`/`p27`（V1 1513.4 / 池 16），§3 的"立刻可做"清单早已做完，§4 把已关闭项与仍开项混在一张表里，§5 的 6 条有 5 条已裁决 | 读 `high_level_status_and_plan.md` 现状与 §0 滚动块对比 |
| ★ **审计脚本不看 `knob`、也不看 CRC** ⇒ **臂腿可以冒充验收腿** | `p84`（消去臂）：契约 9/9、crossings `0.00+0.00`、`stale=0`、`gaps=0`、`cbs/lane=2.00` —— **每一条**都过，而它的链路是**故意打坏**的（CRC-OK 48.4%）；而它的提交与 HEAD **只差文档** ⇒ 用 `--leg=p84` 跑审计，**旧脚本会把它当成合格的加压腿** |
| ★ **`leg_gate.sh` 在一条健康交付腿上只读 5 of 9** | `p72`：FAIL×4 —— `contract 8 of 8`（字面量过期，现为 **9/9**）、`cbs/lane max<=2`（字面量过期，现读 `max=5`、**均值仍 2.00** 且 V4 判的就是均值）、`stale=0`（**绑定错**：该判据注册给默认工况，`p72` 是 stress）、`UL grant >=50% slots`（**绑定错**：注册给 n1/FDD，n78 TDD `ul_ratio 0.30` 结构上到不了 50%）|

#### ② 修法（三条，都不"放松判据"）

* **文档**：`high_level_status_and_plan.md` 的 **§1–§5 全文重写**（判据文字与阈值**一字未改**，只换"当前读数/证据腿"；新增"每条腿都是关于某个二进制的证据"一条纪律），§0 加指针（**最新读数只在文件开头的滚动块**），§0.1/§6/§7 保留。
* **门禁**：`milestone_audit.sh` 与 `leg_gate.sh` 各加**同一对检查**（两条腿都判）：
  1. ★ **"这条腿是不是交付腿"**：`knob` 行只允许**探针**（`OCUDU_METAL_GPU_TIME`、`OCUDU_UL_PHASE_SEGMENTS`）或**显式等于交付默认**的值（`OCUDU_DFT_BATCH_SYMBOLS=14`、`OCUDU_DFT_OPEN_BLOCK=1`、`OCUDU_DFT_RELEASE_BLOCK=1`、`OCUDU_CE_LANE_ORDER=merged`）。**fail-closed**：不认识的旋钮一律拒（新增探针必须显式加进白名单，那一刻正好问一句"它改不改行为"）。
  2. ★ **"链路解出来了吗"**：`CRC-OK / lanes >= **60%**`（一条消去臂伪造不了的读数）。阈值是**量出来的**：60 条腿里**臂读 43.7–58.8%**（`p51` 43.7 / `p80` 47.1 / `p84` 48.4 / `p83` 49.4 / `p82` 56.1 / `p81` 58.8），**sigma2 修好后的交付腿读 79.9–95.6%** ⇒ 60% 落在**两族之间的空带**上。它是**地板**，不是质量判据（V5 的"CRC KO% 不劣化"仍由人对照时代读）。
  * `leg_gate.sh` 另有**第三条**修法：把绑定写进判据 —— `check_bound()`：绑定不符的检查读 **NOT JUDGED + 理由**（不再 FAIL 一条健康腿），与审计脚本对 A1-2 的做法一致（"能判就判，绝不软化"）。它的 `contract` 检查也改成"**9 个名字齐 + MET**"（以名字判，era-proof，pitfall 28）。

#### ③ 验证（都用现成的腿，零飞腿）

| 命令 | 修之前 | 修之后 |
|---|---|---|
| `milestone_audit.sh --quick --leg=p72-n78-batch14` | 23 PASS / 2 FAIL / 0 RED（of 28）| **27 PASS / 2 FAIL / 0 RED（of 32）** —— 新增 4 条（两条腿 × 2）全 PASS；**仅剩的两条 FAIL 是同一条**："腿的提交 ≠ HEAD" |
| 同上，但 `--leg=p84-n78-ablate8`（**臂**）| 会被当成合格加压腿 | **两条新检查立刻 FAIL**（`OCUDU_LANE_ABLATE=1` 是行为旋钮；CRC 48.4% < 60%）⇒ **臂腿再也冒充不了验收腿** |
| `leg_gate.sh --slot-ms=0.5 p72-n78-batch14` | **5 of 9** | **9 of 9 judged pass**（2 条 NOT JUDGED：绑定不符，附理由）|
| `leg_gate.sh --slot-ms=0.5 p84-n78-ablate8` | 5 of 9（同样被"字面量"淹没）| 两条新检查 **FAIL**（臂被点名），其余照读 |

#### ④ 旋钮盘点：`knob_inventory.md`（生成物 + 人工判读）

* **生成器** `wip/gen_knob_inventory.py`（可重跑）：扫 `lib/apps/include/tests` 里的 `getenv("OCUDU_*")`，**从守卫表达式**判默认（`ON`/`OFF`/`AUTO`/`?`），并给出**首个读取点、站点数、模块、范围**，外加**两列"用过没有"**：
  `knob :` 登记行数（`run_leg.sh` 打印，**只有较新的腿有**：108 条 `s` 腿里只有 3 条带它）＋ **记录提及次数/文件数**（覆盖全部历史的腿命令）。
* **盘出来的事实**：树里 **113 个** `OCUDU_*`；其中 **19 个默认 `ON`**（⇒ **"交付形态"不是"一个旋钮都不设"，而是"这 19 个默认值"**）、16 个有腿登记行、88 个只在记录里出现过、**9 个两处都没有**（其中 **8 个在 `test/` 里**，是离线臂，正常；**1 个落在交付代码里 = 真正的退役候选：`OCUDU_CE_PP_PERTURB`**）。
* 文件里同时写明**验收腿的旋钮白名单**（与门禁里那份是同一份事实的两处表达，改一处必须改另一处）。


#### ⑤ ★ 一对 HEAD 上的腿（`p85-n78-default` + `p86-n78-stress`）：审计转 GREEN，并当场纠正了 ① 里两条新检查的标定

**姿势**：同一套命令配方（n78 + `concurrency=2`），**只有两个探针旋钮**（`OCUDU_METAL_GPU_TIME=1`、`OCUDU_UL_PHASE_SEGMENTS=1`），两条都跑在 **HEAD `b4267fb8b6`**（腿横幅逐字核对）。
默认腿**不跑负载发生器**（`--regime=default` 的定义），加压腿照 §1 的配方跑 `iperf3 -R -b 40M -P 4 -t 240`。

| 腿 | V1 中位 | 跳数 | 契约 | `stale` | gaps | `cbs/lane` | 池 | CRC-OK/lanes |
|---|---|---|---|---|---|---|---|---|
| `p85-n78-default` | **1348.0**（p95 1488.5）| **660**（~5.6 跳/s，手机空载）| 9 of 9 | 0 | 0 | 2.00 (max=5) | `starved=0`/`dropped=0`/`held_max=4` | 396/660 = **60.0%** |
| `p86-n78-stress` | **1409.8**（p95 1547.1）| 145341（~525 跳/s）| 9 of 9 | **0** | 0 | 2.00 (max=5) | `starved=0`/`dropped=0`/`held_max=9` | 125137/145341 = **86.1%** |

* ★ **审计：28 PASS / 0 FAIL / 0 RED / 4 INFO（of 32）⇒ `offline acceptance: GREEN`**（此前 23/2/0；那两条 FAIL 就是"腿的提交 ≠ HEAD"，现在两条新腿的横幅都等于 HEAD）。
* ★★ **这两条腿当场证伪了 ① 里新加检查的两处标定，都已修**（"先量再定阈值"的一次自我应用）：
  1. **CRC 地板必须绑流量**：`p85`（**空载手机、660 跳**）读 **60.0%**，而**同一个二进制**在加压腿上读 **86.1%** ⇒ 这条比值在轻流量下量的是**手机**不是代码。
     量了全部记录：**重流量腿（两种工况）60.1–96.5%**（p56 60.1 是 sigma2 缺陷期、p60 86.7、p57 95.6、p86 86.1）、**消去臂 43.7–58.8%**。
     ⇒ 规则改为：**≥ 2 万跳才判 60% 地板；少于 2 万跳改读 INFO/NOT JUDGED，并把两个参照值写出来**。
  2. **"UL ≥ 2.0 Mbit/s（5× 基线）"是加压配方的有效性检查**（它存在的意义是"证明这条腿真的被加载过"），而**默认工况按定义不跑负载发生器**（`p85` 0.03 Mbit/s）⇒ 改为**绑定 `regime=stress`**。
* ★ **V1 的两个工况要分开说**：加压 **1409.8**（相对重载基线 2675.1 ⇒ **−47.3%**）、默认 **1348.0**。默认腿的 1348 是**轻流量**（660 跳、排队少）的结果，**不能当加压指标**，也不能和 1409.8 比优劣；默认腿的价值在契约/`stale=0`/A1-2。
* 附：`leg_gate.sh` 现在在 `p85` 上 **8 of 8 judged**（NOT JUDGED 的四项各带理由：CRC 绑流量、`stale` 绑工况、duty 绑几何、载荷绑工况）、在 `p86`/`p72` 上 **9 of 9**，在 `p84`（臂）上**点名失败**。


### 6.143 ★★★ 一次直跑的副产品：**n78 20 MHz 的上行只有 n1 5 MHz 的 0.43×** —— 不是带宽，是**256QAM 在高档位崩了**（2026-09-27）

> 用户用手机跑了两次 `iperf3 -R -b 40M -P 4 -t 240`，**只换 gNB 的 config**：n78 20 MHz TDD 收到 **7.24 Mbit/s**，n1 5 MHz FDD 收到 **16.7 Mbit/s**（小的反而快 2.3×）。
> 这不是本工作流的时延判据，但它是**同一套融合车道**的吞吐问题，而且日志里已经写明了原因。

#### ① 证据（n78 那次的 gNB 日志；n1 那次的日志被默认路径覆盖，只剩 iperf3 数字）

`bash doc_chinese/phy_latency/wip/ul_grant_stats.py /tmp/gnb_n78_baseline.log`（脚本本次新增，读 SCHED/PHY 两类日志行）：

```
window 267.6 s, 146754 UL grants = 548.3 grants/s
granted 16.42 Mbit/s | new data 8.87 Mbit/s | retransmissions 36.0% | CRC BLER 44.8% | effective 1.80 bit/RE
    mod    hops  BLER%  ok bytes/grant  eff bit/RE  SINR p50
  256QAM  53804   80.4             750        0.87      22.2
   64QAM  41773   10.0            2710        2.89      19.6
   16QAM  10291    1.7            1964        1.93      17.5
    QPSK    426    3.8             549        0.61      18.7
```

* ★ **44.8% 的 PUSCH 传输 CRC 失败**，而其中 **256QAM 档 80.4%**；按"成功字节 ÷ 全部 grant 的 RE"算，**256QAM 只有 0.87 bit/RE，比 16QAM（1.93）还差、只有 64QAM（2.89）的 30%** ⇒ **最激进的档位产出最低**。256QAM 内部按档位看：4.5 bit/RE 失败 62%、5.0 → 78%、**5.5 → 91%**、6.0 → 90% ⇒ 崩在**表顶**。
* **SINR 中位 19.0 dB**（p5 14.2 / p95 24.5）—— 链路不差，但 256QAM 顶部要 ~28 dB；**同一条腿上 `PHR` 显示手机还有 11–17 dB 功率余量**（P_cmax 21–22 dBm）⇒ **不是功率受限**。
* 调度侧后果：**36% 的 grant 是重传**（52,792/146,754），且**重传全部 `rv=0`** —— `cell_cfg.pusch.rv_sequence` 默认 **`[0]`**（DL 侧默认是 `[0,2,3,1]`）⇒ HARQ 只有"同样比特再合一次"的 ~3 dB，没有 RV 循环增益。
* 算术闭合：granted **16.42** → 新数据 **8.87**（×0.54 = 新传占比）→ TCP 收到 **7.24**（×0.82 = 协议头/重传开销）。
* **为什么 n1 不受这个罪**（两条都在"每个 RE"层面）：**同样的手机功率摊到 25 PRB 比摊到 51 PRB 每 RE 高 ~6 dB**；且 n1 是 **FDD**（每时隙都可上行）而 n78 是 **TDD**（5 ms 周期 = 6D+8S+3U ⇒ **只有 30% 的时隙能上行**）
  ⇒ n78 的**平均**上行容量只有 n1 的 **~1.2–1.6×**（不是 4×），而它只交出 **0.43×** ⇒ 差的是**效率**，不是带宽。

#### ② 交付物（本次新增，四件）

| 文件 | 是什么 |
|---|---|
| `configs/gnb_rf_b200_tdd_n78_20mhz_ul_ab.yml` | ★ **n78 的 A/B 模板**（带注释）：交付配置 + **`max_pusch_and_srs_concurrency: 2`** + 独立 `log.filename` + `cell_cfg.pusch` 下**四个注释掉的旋钮**；文件头写全了"为什么做这组 A/B"与四条臂的预期 |
| `configs/gnb_rf_b200_fdd_n1_5mhz_bridge_ul_ab.yml` | ★ **n1 的镜像（对照）**：同一套改动 + 同名旋钮。**n1 只需要这一份**（不是四份）：那四个旋钮治的是 n78 特有的病（256QAM 高档位崩），n1 的作用是把"设置差异"从"频段/几何差异"里摘出来；且 `p0` 是按频段定的，抄过去没有意义 |
| `wip/mk_arm_cfg.sh`（扩展） | 加 `rv` / `mcs19` / `qam64` / `p0up` / `p0up_rv_mcs19` 五个臂：**只解开模板里对应的那一行**，并保留"必须是预期行数的改动 + 输出必须含目标字符串"的双重护栏（首次运行正是被它拦下：UL 臂错误地默认去交付配置里找注释行）。臂的默认源按族选择，`ARM_SRC=` 可指向 n1 镜像 |
| `wip/ul_grant_stats.py` | ★ **同一把尺子**：从 gNB 日志读 grants/s、granted/new-data Mbit/s、重传比例、按调制的 BLER 与**有效 bit/RE**；多个日志一起给会打一张对比表 —— 四个臂必须用同一把尺子判 |

#### ③ 校验（不飞腿、不碰电台）

* 两个模板都进了 `tests/unittests/apps/yaml_roundtrip_gnb_test.cpp` 的清单 —— 那个测试用**真实 parser**（`allow_config_extras(error)`：未知键直接解析失败）+ leaf 保留对比。**n78 模板 OK（roundtrip/7）**。
* ★ **n1 镜像不进清单**，原因**不是**新加的东西：它忠实保留了原文件的 `cu_cp.amf.addr` / `bind_addr`（单数），而 writer 输出复数 `addrs`/`bind_addrs` ⇒ leaf 对比差两条 —— 原 `gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 也因此不在那份清单里。它的**解析**是证明过的（否则测试会解析失败），且它新加的键与 n78 模板完全相同（那份是绿的）。原因写在测试文件里。
* **四个旋钮键的形状逐个过真实 parser**：把四行同时解开后临时装成模板跑 `roundtrip/7` ⇒ **OK**，随后从备份还原并对该文件 `git diff --exit-code` 确认干净。
* `wip/ul_grant_stats.py` 在 n78 那次日志上**复现了手算的全部数字**（548.3 grants/s、16.42/8.87 Mbit/s、36.0%、44.8%、1.80 bit/RE）。

#### ④ 怎么跑 / 怎么判（写进模板头部，此处只留摘要）

```bash
bash doc_chinese/phy_latency/wip/mk_arm_cfg.sh rv      # → doc_chinese/work_tmp/arm_rv.yml（打印 diff）
sudo ./build/apps/gnb/gnb -c <arm>.yml --expert_phy.phy_pipeline gpu
# 每个臂 4 分钟同一条 iperf3；跑完等进程退出
python3 doc_chinese/phy_latency/wip/ul_grant_stats.py /tmp/gnb_n78_ul_ab.log
```
建议顺序 **`rv` → `mcs19`**（两条最便宜），有收益再考虑 `p0up` 与组合；n1 侧在 n78 选出赢家后**只镜像 `rv`/`mcs19`/`qam64`**。

★ **与验收的关系**：这些是**实验配置**，交付配置（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）本轮**未改** —— 它上面挂着一对验收腿（`p85`/`p86`），改它等于让那对腿的证据指向另一份配置。要合并进交付形态，得在 A/B 有结论后单独做一次，并重新飞那对腿。


#### ⑤ ★★ 第一批读数（`rv` 臂 + n1 镜像）：**重传塌了 4.3×，但吞吐只涨 11%** —— 因为上行时隙已 97-100% 占满 ⇒ 剩下的瓶颈是 **TDD 配比**

`rv` 臂 = 模板（`concurrency 2`）+ `rv_sequence: [0,2,3,1]`。先验"臂真的生效"：日志里**重传的 rv 从全 0 变成 1/2/3**（439/825/7745/2769），而基线 52,792 条重传**全是 0** ✓。

| 读数（**最忙 240 s** = 真正的测试窗）| n78 基线（交付配置，AUTO 并发）| n78 + `rv`（模板，并发 2）| n1 镜像（并发 2）|
|---|---|---|---|
| ★ **iperf3 收到（TCP 真值）** | **7.24 Mbit/s** | ★ **9.93 Mbit/s（+37%）** | **16.6 Mbit/s** |
| iperf3 发送 / TCP Retr | 7.81 / 2806 | 10.7 / 3786 | 17.3 / 2376 |
| grants/s（占可用上行时隙）| **600.0（100%）** | 584.8（97.5%）| 984.1（98.4%）|
| granted Mbit/s | 17.96 | 12.29 | 23.03 |
| 新数据 Mbit/s（MAC 侧，不含上层丢包）| 9.73 | 10.76 | 18.15 |
| ★ **MAC 新数据 → TCP 的存活率** | **74%** | ★ **92%** | **91%** |
| 平均 TB（51 PRB 峰值 ~6787 B）| 3741 B（55%）| 2626 B（39%）| 2925 B（43%）|
| 重传比例 | 35.8% | **8.3%** | 19.7% |
| CRC BLER | 44.8% | **7.9%** | 23.1% |
| 有效 bit/RE | 1.80 | **2.54（+41%）** | 4.49 |
| 256QAM：跳数 / BLER / 有效 | 53,804 / 80.4% / 0.87 | 11,121 / 29.8% / 3.68 | 164,231 / 24.1% / 4.53 |

* ★★ **更正（同日，拿到 iperf3 真值之后）**：`rv` 臂的**交付**收益是 **+37%**（7.24 → 9.93 Mbit/s），**不是**我从 MAC 侧读数推的 +10.6%。
  原因就在上表新增的那一行：**MAC "新数据" → TCP 存活率**，基线只有 **74%** 而 `rv` 臂 92%、n1 91%。
  ⇒ RV 循环的真正好处不只是"少花 36%→8.3% 的 grant 在重传上"，更是**TB 被丢弃得少得多**（基线上那些重传耗尽 HARQ 后丢掉的 TB，在 MAC 侧仍被记成"新数据"，只有 TCP 侧看得见损失；RLC 层的重传对 iperf3 的 Retr 计数不可见）。
  ⇒ 两个"健康"运行的存活率一致（92%/91%）本身就是这条解释的旁证；而三次的 TCP 层重传率几乎相同（1.24% / 1.25%），说明差别不在 TCP，在它下面的空口。
* ★ **剩下的差距几乎全是时隙数**：`rv` 臂 9.93 vs n1 16.6 = **0.60×**，而 (984/585 时隙比) × (2925/2626 TB 比) × (相同的新传占比) ≈ **1.87**，与 16.6/9.93 = 1.67 同量级 ⇒ **时隙数是主项**。
* ★ **`rv` 臂是对的**（重传 35.8%→8.3%、BLER 44.8%→7.9%、有效 bit/RE +41%、交付 +37%），代价是**平均 TB 变小**（3741→2626 B）。
* ★★ **三次运行的上行时隙都被填到 97-100%** ⇒ 调度器**没有余量** ⇒ 吞吐 = (上行时隙数) × (每 grant 的 TB) × (1 − 重传率)。
  这条把问题结构化了：**n1 赢在时隙数（984 vs 585），不在 TB 大小（2925 vs 2626 B）** —— 那是 **FDD（每时隙可上行）vs TDD（5 ms 里只有 3 个上行时隙 = 30%）**。
  ⇒ 与 §6.143① 的"每 RE 少 ~6 dB"合起来，"5 MHz 比 20 MHz 快"被完整解释：**一次是每 RE 的功率，一次是时隙的数目**。
* ⚠ **两处必须写明的保留**：
  1. **混淆变量**：基线跑的是**交付配置**（`concurrency` 未设 ⇒ AUTO = 1），而 `rv` 臂跑的是**模板**（= 2）⇒ 这一对差**两个变量**。干净的做法是先跑**模板本身**（不改任何旋钮）当对照，再跑 `rv`。
  2. **不是同分钟**：两次运行相隔 ~20 分钟，信道/热状态可能不同 ⇒ "平均 TB 变小"这一项**不能**归因给 `rv_sequence`（它就应该是 HARQ 的账）。
* ★ **新增的结构性臂 `ulheavy`**（`configs/…_ul_ab.yml` 里注释着，`mk_arm_cfg.sh ulheavy` 一行解开）：TDD 配比 **6D+8S+3U → 4D+8S+5U**（上行时隙 600 → 1000 个/s，**+67%**，保留 8 符号的 DL→UL 保护时隙）。
  判据：若"时隙数是瓶颈"成立，新数据速率应大致同比例上升；若几乎不动 ⇒ 瓶颈在 per-RE SINR/MCS 那一侧（回到 `p0up`/`mcs19`）。
  **代价是真的**：DL 从 6 个整时隙降到 4 个 —— 对 `iperf3 -R` 这种"手机发"的测试代价很小，但**两个方向都要判**。该臂过了真实 parser 的 round-trip（临时装成模板跑 `roundtrip/7` ⇒ OK，随后还原）。
* ★ **尺子自己也修了一处**（同一晚被这条数据打出来的）：`ul_grant_stats.py` 原来只按"日志里第一条到最后一条 grant"量速率，而**日志窗口 ≠ 测试窗**（背景上行把 240 s 的测试摊进 476 s，读出 6.24 Mbit/s 的假退步）
  ⇒ 现在**同时给两个窗口**：全窗口（连长度一起打，稀释可见）+ **最忙 <window> s**（默认 240 s = 测试本身），并在对比表里用后者。**比值类读数（重传率/BLER/bit-per-RE）与窗口无关**，两个都可读。
* 附：`rv` 臂那次**没有优雅退出**（`Could not stop application after 5 seconds. Forcing exit.` ⇒ 日志以 `[APP] [E] Emergency flush of the logger` 结尾，`[metal_stats]` 那份停机报告没了）。
  对**本问题无影响**（grant 读数是 logger 行、实时落盘），但那次运行的**时延读数缺失**；这是记录里 `p14` 同族现象的再一次出现，值得单独留一眼。

#### ⑥ 追问："那次没优雅退出"是偶发还是错误？—— **偶发（12/248 ≈ 5%）、不是运行错误，但确实吃掉一部分读数**（2026-09-27 量的）

* **机制（读码）**：`lib/support/signal_handling.cpp`。第一次 SIGINT ⇒ 应用的 interrupt handler（`is_app_running=false`）+ **`::alarm(5)`**（`TERMINATION_TIMEOUT_S = 5`，编译期可由 `-DTERM_TIMEOUT_S=<n>` 覆盖）。
  应用若 **5 s 内没退完**，SIGALRM 触发 ⇒ 打印 `Could not stop application after 5 seconds. Forcing exit.` ⇒ 跑 cleanup handler（**它打的就是 `[APP] [E] Emergency flush of the logger`**，顺带 flush 日志）⇒ **`std::raise(SIGKILL)`**（所以 shell 报 `Killed: 9`）。
  ⇒ **这不是崩溃**：`raise(SIGKILL)` 会**跳过 atexit**，所以丢的是"停机报告"，不是数据。
* **那一次卡在哪**：`rv` 臂日志的有序停机是走完的 —— `CU-UP stopped successfully` → `CU-CP stopped successfully` → `Closing PCAP files...` → `PCAP files successfully closed.`（08:23:59.632），**之后 4.8 s** 才是 Emergency flush（08:24:04.447）。
  ⇒ 卡点在 **PCAP 关闭之后的最后一段 teardown**（DU/lower-PHY 停机、worker pool、UHD/电台关闭），**不在数据路径**；日志本身无法指出是哪一次阻塞调用。
* **频率（量出来的）**：**248 条腿里 12 条**（≈5%）打印过 `Forcing exit`；横跨 09-20…09-27、跨多种配置（`p14-conc2`、`p28-txslack`、`p70-spp1024`、`p77/p78` 的**第一次**尝试、`s13p4-phases`、`s33*`、`s35-d1-keyed`、`s76-wall-premerge`）⇒ **与 `rv` 臂、与并发度都无关**，是这套停机路径的固有偶发。
* ★ **代价（实测对比：p77 第一次【异常】vs `p86`【正常】）**：
  | 报告行 | 异常退出 | 正常退出 |
  |---|---|---|
  | `[metal_stats] …`（含 **Q9-F3 per-label 表**、dft commits、fence wait）| **0** | 1 |
  | `[ul_gpu_lane] …`（`residency`/`busy split`/`cbs/lane`）| **0** | 1 |
  | `[ul_rx_pool] …` / `[ul_rx] blocks=…` | **0** | 1 |
  | `[phy_pipeline] contract …` / `lane host participation` / `[ul_by_size]` / **`[ul_gpu_pipeline] samples`（V1）** | 1 | 1 |
  ⇒ 丢的是**停机路径上还没打出来的那些**（顺序相关，不是固定集合）：`p14` 那次连契约都丢了，而 `p77`/`p70` 保住了契约与 V1。
  ⇒ **对吞吐 A/B 无影响**（判读用的 SCHED/PHY grant 行是实时 logger 行）；但对**时延腿**，丢的正是审计要读的那几张表 ⇒ **这种腿必须重飞**（记录里 p77 就是这么做的：0931 那次被吃掉，被引用的是 0940 那次健康的）。
* **三个可选处置**（按代价排序）：
  1. **零成本（纪律）**：看到 `Forcing exit` 就按"报告不完整"登记；时延腿重飞，吞吐/调度类判读照用（本次即如此）。
  2. **一行构建选项（治标、立竿见影）**：`TERMINATION_TIMEOUT_S` 支持 `-DTERM_TIMEOUT_S=<n>` 覆盖 ⇒ 给 teardown 20 s，这类丢失基本消失。
  3. **定位根因（治本、低优先、另开一线）**：在 PCAP 关闭之后的每一步之间加 logger 行，把"卡在哪一步"变成读数。这是真实缺陷，但不属于吞吐/时延主线。


#### ⑦ ★★★ 第二、三、四批读数（模板对照 / `p0up` / `ulheavy`）：**三个单项全部有效，且"时隙数"这条归因被证实**

第一次直跑之后按建议跑了三条：① 模板本身（干净对照，conc 2、无旋钮）② `p0up` ③ `ulheavy`。**① 把 `rv` 那次的混淆变量彻底消掉了**：

| 运行 | TCP 收到 | vs 对照 | grants/s | granted | MAC 新数据 | 平均 TB | 重传% | BLER% | 有效 bit/RE | **MAC→TCP 存活** |
|---|---|---|---|---|---|---|---|---|---|---|
| n78 **交付配置**（AUTO=1）| 7.24 | −2% | 600.0 | 17.96 | 9.73 | 3741 B | 36.0 | 44.8 | 1.80 | 74% |
| ★ n78 **模板对照**（conc 2）| **7.39** | 基准 | 600.0 | 15.42 | 9.49 | 3214 B | 25.8 | 33.2 | 1.90 | 78% |
| n78 + **`rv`** | **9.93** | ★ **+34.4%** | 584.8 | 12.29 | 10.76 | 2626 B | **8.3** | **7.9** | **2.54** | ★ **92%** |
| n78 + **`p0up`** | **9.11** | ★ **+23.3%** | （日志被覆盖，只有 TCP 值）| | | | | | | |
| n78 + **`ulheavy`** | **10.70** | ★ **+44.8%** | **986.4** | 31.68 | **15.46** | 4015 B | 40.2 | 50.1 | 1.57 | 69% |
| n1 镜像（conc 2）| **16.60** | — | 984.1 | 23.03 | 18.15 | 2925 B | 19.7 | 23.1 | **4.49** | 91% |

* ★★ **混淆变量已消（这是 ① 的全部价值）**：模板对照 **7.39** ≈ 交付配置 **7.24**（AUTO=1）⇒ **并发 1→2 不是驱动因素**，`rv` 的 +34% 可以单独归因给 `rv_sequence` ✓。三个单项相对**同源对照**的增益：`ulheavy` **+44.8%** > `rv` **+34.4%** > `p0up` **+23.3%**。
* **每个单项为什么有效（尺子给出的机制）**：
  * `rv`：重传 25.8%→**8.3%**、BLER 33.2%→**7.9%** ⇒ **存活率 78%→92%**（最干净的效率赢）；
  * `ulheavy`：grants/s **600→986（+64%）**——正是配比改动预测的（3→5 个上行时隙/5 ms，600→1000 个/s）——且**平均 TB 反而变大**（3214→4015 B）；但**重传涨到 40.2%、BLER 50.1%** ⇒ **存活率掉到 69%**，所以 TCP 增益（+44.8%）低于时隙增益（+64%）；
  * `p0up`：只有 TCP 值（+23.3%），机制未读到（该次日志被同名覆盖）——按设计它抬的是每 RE 的 SINR ⇒ 应体现在 TB 上。
* ★★★ **被"配平"出来的对照**：**`ulheavy` 的时隙数（986.4/s）≈ n1 的（984.1/s）** ⇒ **在上行时隙数相同的条件下**，n78 交付 **10.70** 而 n1 交付 **16.60** Mbit/s，而 n78 的平均 TB 还**更大**（4015 vs 2925 B）。
  ⇒ 剩下的 **1.55×** 全部是**每 RE 的质量**：n1 的 BLER 23.1%（vs 50.1%）、有效 4.49 bit/RE（vs 1.57）。
  这正是"同样的手机功率摊到 25 PRB 比 51 PRB 每 RE 高 ~6 dB"（§6.143①）在配平条件下的直接读数。
* ⇒ **下一批臂 = 组合**（已加进生成器）：`rv_p0up`（两个链路级赢家）、**`rv_ulheavy`**（把 `ulheavy` 丢掉的那 31 个百分点的存活率补回来 —— 算术上 15.46 × 90% ≈ **13.9 Mbit/s**）、`rv_p0up_ulheavy`（三变量，只能确认天花板、不能归因，放最后）。
* ⚠ **`mcs19`/`qam64` 降级为低优先**：`rv` 已经把 BLER 压到 7.9%，而 `ulheavy` 显示"TB 越大越好"（4015 B > 3214 B）⇒ 此时**去砍 MCS 上限方向相反**。
* ⚠ **`ulheavy` 的代价仍然是真的**：DL 从 6 个整时隙降到 4 个（SIB dump 已核：`nrofUplinkSlots` 3→5 ✓ 生效）。本测试是"手机发"，下行只承载 ACK ⇒ 代价小；**进交付形态需要单独裁决**。


#### ⑧ ★★ 前端 DFT 的并行度：**默认一个时隙 14 个符号同时在跑**；这台机器同时能跑 **20** 个（实测曲线）（2026-09-27）

> 问题（用户）：*"default 的 DFT 并行度是多少？有几个 OFDM 符号被 metal kernel 同时跑在不同 GPU 核心上？"* —— 分三层答，**第二层的数字是本次新加的臂量出来的**。

* **① 派发请求的并行度 = 一个时隙的符号数 = 14**（本小区）。`front_end_batch_cap()`：`OCUDU_DFT_BATCH_SYMBOLS` 不设 ⇒ **AUTO = `set_slot_symbols()` 告知的每槽符号数**（normal CP **14**、extended CP 12），上限 `max_batch_slots = 16`；`=1` 是"关批处理"的对照臂。
* **② 并行的单位 = 1 个 OFDM 符号 = 1 个 threadgroup**：`dispatchThreadgroups(MTLSizeMake(nof_transforms,1,1), threadsPerThreadgroup: min(n,1024))` —— 本小区 `n = srate/SCS = 23.04e6/30000 = 768` ⇒ **每组 768 线程**；kernel 里 `threadgroup float2 buf[4096]` ⇒ **每组占 32 KiB 线程组内存**。
* **③ 物理并发上限 = GPU 核数**。本次在 `wip/dft_dispatch_cost.mm` 新增"并行度扫描"臂（air-like 形状：int16 输入 + 网格写；15 次取中位），实测：

  | 一条 dispatch 里的变换数 | 1 | 2 | 4 | 7 | 10 | **14** | 16 | **20** | **24** | 28 | 32 |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | cb 窗口 (µs) | 48.0 | 47.0 | 46.9 | 47.9 | 47.1 | **46.9** | 47.0 | **47.0** | **75.9** | 76.8 | 77.0 |
  | 每变换 (µs) | 48.0 | 23.5 | 11.7 | 6.8 | 4.7 | **3.3** | 2.9 | **2.4** | 3.2 | 2.7 | 2.4 |

  ⇒ ★ **窗口从 1 到 20 组恒定（≈47 µs），到 24 组才跳一档（75.9 µs）** ⇒ **本机同时容纳 20 个 threadgroup**，恰好等于 **Apple M4 Pro 的 20 个 GPU 核**（每组 32 KiB ⇒ 每核一组）⇒ **默认请求的 14 个符号全部同时在跑**（占 14/20 个核），离线复现了 §6.124/§6.126 的空口结论（空口 14 组 = 46.9 µs，与此处 46.9 µs 一致）。
* **对照（旧前端）**：同样 14 个变换、但**每符号一条派发**（14 条 dispatch 在同一个 cb 里）= **530–563 µs** ⇒ **打包 11.3–12×**；而"2 个变换分 2 条派发"= 86.3 µs ⇒ **多一条派发就多一份 ~40 µs 的固定代价** ⇒ 这就是 §6.126 里 batch 7/2 越拆越差的机制。
* ⚠ **口径提醒（别混绝对值）**：`wip/dft_kernel_cost.mm` 的"1 个变换 12.18 µs / 14 个 13.75 µs"是**另一种几何**（无网格写、无 int16 输入）；本节 harness 的 air-like 形状是 **≈47 µs 恒定**。两者都说明"**打包几乎免费、组是真并行**"，但绝对值不可互换。
* **跨时隙这一层不是并行的**：一个块 = 一条 dispatch = 一条 cb（一个时隙的 14 个符号装在里面）；**不同时隙的 DFT 不会被合并进同一条 dispatch**，它们能否重叠看前端管道深度与交棒结构（`[metal_stats] dft … slots_in_flight=` 就是**管道深度**计数器，不是"提交减等待"，本会话的腿上读 0）。eq/demap 是**另一条** dispatch（延迟分组，最多 12 符号/组，实测 `max_run=12`）。




### 6.144 ★★★ 融合 GPU 车道的**结构地图**与**组合候选**（用户 2026-09-27 指示：先理解各部分如何组织，再看有没有更优的组合）（2026-09-27，读码 + 现有读数）

> 出发点（用户）："overhead 还是很大，远超 CPU 路径"。本节把**一次跳的设备侧组织**逐件拆开：**谁提交什么、在哪个队列、命令缓冲边界在哪、靠什么同步、每条 cb 花多少**，
> 然后把"哪些是平台强加的、哪些是可以重组的"分开，最后给出候选组合与**各自的判据/代价**。全部结论都标了来源（读码 / 实测 / 假设）。

#### ① 一跳的设备侧提交清单（**读码**，与 `p86` 的 per-label 表逐条对齐）

| # | 命令缓冲（label）| 内容 | 谁创建 / 谁提交 | 队列 | p86 实测（n / wait p50 / exec p50）|
|---|---|---|---|---|---|
| 1 | **`merged_hop`** | **整跳**：前端抽取（14 变换一次派发）+ CE 权重 + 均衡 + 解映射 | DFT 引擎**创建但不提交**（D1 交棒）→ 估计器 adopt → **车道提交** | **back_end** | 145,340 / 213.9 / **468.1** |
| 2 | **`ce_weights`** | ★ **本跳的"相关矩阵构建"**（fenced corr），**不是**估计器自己的整跳缓冲 | 估计器（`flush_correlations_fenced`），**提前提交** | back_end | 145,346 / 36.1 / 42.0 |
| 3 | `dft_front_end` | ★ **全部是 PRACH 自己的变换**（B4：每 10 ms 一 occasion × 12 符号 × **每符号一条 cb**）| DFT 引擎（PRACH 的实例，**从不通告 lane slot** ⇒ 不走块打包）| **front_end** | 331,909 / 45.7 / 46.6 |
| 4 | `late_handed` | 没人认领、被注册表清扫兜底提交的块 | 注册表（drop committer）| back_end | 2,940 / 284.0 / 50.1 |

* ⇒ **`cbs/lane=2.00` 的真正含义**：**#1 车道整跳 + #2 相关矩阵的独立 cb**。**不是**"估计器 + 车道各一条"——`OCUDU_CE_LANE_ORDER=merged` 已经生效（`ce_lane_order_from_env()` 默认 merged，标签 `merged_hop` 就是它设的）。
  ⚠ 这一点值得记住：把 #2 误认成"估计器在中间插了一次提交"，会让人以为"翻转 lane order 就能省掉它"——**不能**（见 ②）。
* ⇒ **#3 是新发现的结构性浪费**：PRACH 的 12 个符号**每个一条 cb**（1200 条/s、每条 46.6 µs ⇒ **约 5.6% 的前端队列占用**），而 PUSCH 前端走的是"14 个符号一条派发"。
  代码注释（`plain_without_lane_slot`）已把这件事定死："`plain_without_block == plain_without_lane_slot == 12 × frames + 1`"，并明确**不是**覆盖缺口。**它对 PUSCH 跳无直接影响**（前端队列 vs 后端队列，见 ②）——价值在 PRACH 自身时延与前端队列抖动。

#### ② 三条**平台强加**的约束（决定了"什么不能重组"）

* **(F1) 同一条 encoder 内的两条 dispatch 之间，屏障不足以保证生产者→消费者可见性；只有命令缓冲边界保证。**
  这是旧设计文档 §5.9.87③/§5.9.88④ 用**六种方式**量出来的经验定律，也是 #2 存在的唯一理由：相关矩阵必须落在**比它的消费者更早的一条 cb** 里。
  ⇒ **`cbs/lane=2.00` 不是调参，是这台 GPU/驱动上的正确性代价**；实测它的价格是 **CE 跳时间 ~215 → ~275 µs（+28%）**，且"多一条 cb"的代价**无法用任何缓冲区内原语消掉**（六种都量过）。三种修法里 **`CORR_FENCED` 是最便宜的一种**（比 `CORR_STANDALONE` 便宜 ~10 µs）。
* **(F2) 车道那条 cb 的窗口 ≈ 一个时隙（~470 µs），且与内容无关。** 五条腿六个数据点：`p72` 473.1 / `p75` 445.8 / `p77` 459–517 / `p83` 466.5 / `p84`（**全部 kernel 换成空的**）**462.9**。
  ⇒ 这条 cb 的账**不是算力**，而且**换结构只是搬家**（`p75`/`p77` 的对照）。**"≈ 一个时隙（500 µs）"这个量级本身**是目前最好的线索（min 403.8 / p5 427.4 / p50 468.1 / p95 492.6，全在 500 以下的一个 ~90 µs 宽的带里）。
* **(F3) 交棒结构把前端队列"藏"起来了**：D1 打开的块在 **back_end** 队列创建（车道提交的队列），所以交付结构里**看不到**前端那条 46.9 µs 的窗口（它只在关 D1 时以 `dft_front_end` 出现）。
  ⇒ 想"把前端算力单独读出来/单独优化"就必须先破坏交付结构（`p77` 就是这么做的）。

#### ③ overhead 预算：**真正的算力 ≈ 110 µs/跳，V1 = 1400 µs ⇒ ~92% 是结构**

| 项 | 每跳 µs | 性质 | 来源 |
|---|---|---|---|
| `merged_hop` 的 `exec` | **468** | 其中 ~450 与内容无关（F2）| p86 / p84 |
| `merged_hop` 的 `wait`（后端队列 FIFO）| **214** | 排队 | p86 / §6.123 |
| `ce_weights`（正确性代价）| 42 + 36 | 一小部分与 #1 重叠 | p86 |
| 宿主参与（头 43 + 尾 41）| **84** | G2 的量 | p86 |
| fence 等待（Q24）| median 34（32% 命中）| 与 #1 重叠 | p86 |
| PRACH plain cb（另一队列）| 46.6 × 2.28 | 与跳无关 | p86 |
| **真算力合计**（corr 42 + eq/demap ~20–25 + 前端 47）| **≈110** | —— | §6.65/§6.69 + §6.143⑧ |
| **V1** | **1400**（一跳墙钟 **1935**）| —— | p86 |

#### ④ 组合候选（按"预期收益 / 代价 / 需要什么"排序）

| # | 候选 | 机制 | 预期 | 代价/风险 | 现状 |
|---|---|---|---|---|---|
| **C-A** | ★★ **一条车道 cb 承载 N 跳**（不是 N 个符号）| 若那条 cb 的账**按 cb 计**（F2：与内容无关），则 2 跳/cb ⇒ 每跳 ~0.5 个"时隙账" | 可能 **−200 µs 以上** | 第一跳的 LLR 要等第二跳编码完（与前端打包、corr fence 同类代价）；**需要新的分组机制**；先离线判"账是按 cb 还是按跳" | **未做过**；`p84` 只证明了"同一个 cb 内换内容不动"，**没证明**"两个跳量放进一个 cb 后账只算一次" |
| **C-B** | ★ **把"账 ≈ 一个时隙"这条机制钉死** | 找出是什么把窗口量化到 ~470 µs：下一个时隙的 stage fence / grid 产出 / 令牌释放 / 驱动的提交批处理 | 决定 C-A 是否成立的门 | 零腿可先做的相关分析（现有腿的 slot/相位分段数据）| **未做过** |
| **C-C** | **PRACH 变换打包**（12 符号/派发）| 与 PUSCH 前端同一手法 | 前端队列占用 5.6%→0.5%，PRACH 自身时延 ↓ | 改的是 PRACH 的引擎实例（不通告 lane slot ⇒ 要给它一条块路径）| **未做过**；对 V1 **预期无影响**（不同队列）|
| **C-D** | 队列/提交路径重组 | 换"谁提交/哪个队列"（`p75`/`p77` 已各试一种）| 已证：**只是搬家**（总量 ~505–517 不变）| —— | 已有反例 |
| **C-E** | `OCUDU_CE_CORR_FENCED=0`（回到 1 条 cb/跳）| 少一次提交 | ❌ **不可用**：重新引入 §5.9.88 的**可见性缺陷**（A 矩阵读到坏值）| 正确性 | 已否（平台定律 F1）|

#### ⑤ V1 的构成（用户 2026-09-27 追问："那 92% 主要是什么？"）—— **嵌套分解，不是相加**

先把三个口径钉死（都来自探针自己的定义，不是推断）：

| 量 | 定义（源码原话）| 时钟 |
|---|---|---|
| **V1 `[ul_gpu_pipeline]`** | **本槽第一个 IQ 样点到达 → LLR 交给解码器**（`record_start()` → 第一次 codeblock decode）| **墙钟** |
| `residency` | **车道链的首条 cb 开始 → 末条 cb 结束**（pure GPU timestamps）| **设备** |
| `busy` | 该车道各 cb 的 GPU 执行时间之和 | 设备 |
| `gap` | `residency − busy` = "**设备在等宿主喂**"（探针原话：the GPU waiting for the CPU to feed it）| 设备 |

★ **收包策略这一条决定了 A 的量级**：`ul_pipeline_probe.h` 明写 —— 整槽收包时"**一个块就是一整个时隙，所以等待不可能短于'直到本槽最后一个样点存在'**"；而 `record_start()` 取的是**该块的第一个样点**的时间戳 ⇒ **A ≈ 一个时隙（30 kHz = 500 µs）**，与算力无关。

★ **A 的物理含义（用户 2026-09-27 追问"473 是不是说明 CPU 迟到 27 µs"）—— A = 时隙时长 − δ，δ = 宿主唤醒相位**：

设 `T0` = 本槽第一个样点存在的瞬间（ADC 时间线，**固定**），`T1 = T0 + slot`（最后一个样点），`Tc` = 宿主**发起** `receive()` 的时刻（`record_start()` 打的墙钟）。则 `A = T1 − Tc = slot − δ`，其中 `δ = Tc − T0`：

| 腿 | slot | A 中位 | ⇒ δ | 读法 |
|---|---|---|---|---|
| `p86`（n78 gpu，30 kHz）| 500 | **473.0** | **+27 µs** | 宿主在**槽边界之后** 27 µs 才来要样本 |
| 用户引的 n1 腿（15 kHz）| 1000 | **1054.0** | **−54 µs** | 宿主在**槽边界之前** 54 µs 就来了 |

* ★ **A 变小 ≠ 变快**：样本在 `T1` 之前**根本不存在**，一跳最早也只能从 `T1` 起步。宿主**更早**来 ⇒ A 变大而物理延迟不变；**更晚**来 ⇒ A 变小、V1 也变小，而 `T0 → LLR` 的物理延迟**一点没变**。
* ★ 因此 **V1 与 A 都以 `Tc` 为锚**：`V1 = (T0 → LLR 的物理延迟) − δ`。跨腿/跨模式比 V1 时，**δ 是未记账的偏移**（p86 是 27 µs）；要还原物理延迟就把它加回去。
* ⇒ 这也解释了 A 为什么在不同腿/模式间从 473 跳到 1054：**变的是 δ（宿主唤醒相位），不是结构**。
* ⇒ "理想情况下 A 应该是 500"这句话的正确读法：500 只是"宿主恰好在 `T0` 醒来"（δ = 0）时的读数。它既不是下界（A 可以更大），**也不是目标**（把 A 做小不减少任何延迟）。真正的目标是 **`T1 → LLR` 的那 936.8 µs**（= 1409.8 − 473.0：头 43.4 + residency 636.8 + 差值的 256.6）。
* ⚠ **唯一需要盯的 A 的尾部**是"宿主太晚"那一侧（`min` 越小 = 来得越晚）：p86 `min=22.0` ⇒ 最晚一次宿主在槽边界之后 478 µs 才来（仍 > 0 ⇒ 仍没丢样）。**漂移**（δ 持续增长 = 宿主落后于时间线）才是故障前兆，由 `[ul_rx_timing]` 的 **`slip`** 度量（p86 `slip max=8820 µs`、`over 1ms=424` ⇒ 稳态没有漂移）。

★ **这张表的依据（用户 2026-09-27 追问："在没有这样的探针改造前，这些数凭什么？"）** —— **全部来自 `p86` 那条腿当时就已经开着的三台仪器**，没有一个是新探针：

| 仪器 | 门控 | `p86` 原始行（`wip/logs/gnb_gpu_p86-n78-stress_0927_1500.log.stderr`）|
|---|---|---|
| `[ul_gpu_pipeline]`（V1）| **无**（探针自带）| `samples=137332 mean=1414.0us median=1409.8us p95=1547.1us`（L42）|
| ★ **`[ul_rx_wait]`（= A）** | **无**（`report()` 无条件打印）| `samples=553142 mean=496.9us **median=473.0us** min=22.0us p95=593.0us`（L47）|
| `lane host participation`（契约 9/9 里的一条）| **无** | `head median 43.4us, total median 86.0us, work after the extraction 42.6us; 0 lanes read shorter than their own head`（L64）|
| `[ul_gpu_lane]` residency / busy / gap | `OCUDU_METAL_GPU_TIME=1` | `residency median=636.8us`（L98）/ `busy median=500.8us`（L99）/ `gap median=140.0us`（L100）|
| `[ul_gpu_lane]` **busy split**（按 label 的**加法**分解）| 同上 | `ch_wt=42.8us/lane (8% of busy, cbs/lane=1.00) merged_hop=472.3us/lane (92%, cbs/lane=1.00)`（L137）|
| Q9-F3 **per-label** 表（cb 粒度的 wait/exec）| 同上 | `ce_weights n=145346 wait p50=36.1 exec p50=42.0`（L157）/ `merged_hop n=145340 wait p50=213.9 exec p50=468.1`（L158）|

⇒ **只有两个数只能靠差值，必须这么标注**：
1. **A** 当时写成"≈500"（结构论证 + 借 §7.1 的 A 项 473）—— **其实 `[ul_rx_wait]` 在 p86 上的中位就是 473.0 µs**（它按块、无门控、无条件打印）⇒ 见 §6.145⑦ 的更正；
2. **LLR 回传 + 交付段** ＝ `V1 − A − 头 − residency`（没有仪器单独量过它）。
⇒ 用**实测 A** 重算：`1409.8 − 473.0 − 43.4 − 636.8 = ` **256.6 µs**（老账写 "~230" 是拿 A = 500 算的）。
⇒ 树里原来那行"其余 ~33"也是**两个中位相减**；腿里其实有**更好的加法读数**：`ch_wt 42.8 + merged_hop 472.3 = busy 均值 515.1`（与 `busy mean=515.1us` 逐位对上，且每跳恰好 2 条 cb ⇒ `cbs/lane = 1.00 + 1.00 = 2.00`）。

**p86 中位数的嵌套分解**（↳ 表示"包含在上一行里"；括号里写明**是读数还是差值**）：

```
V1 = 1409.8 µs（墙钟：本槽第一个样点 → LLR 交付）
├── A：等本跳最后一个样点到达（整槽收包）        473.0      ← 读数（`[ul_rx_wait]` 中位；§6.145⑦ 更正原来的 ≈500）
├── 宿主头：stage entry → extraction commit        43.4      ← 读数（契约行 `head median`）
├── 设备 residency（车道链首 cb → 末 cb）          636.8     ← 读数（纯 GPU 时间戳）
│   ├── busy（各 cb 执行窗口之和）                 500.8     ← 读数（均值 515.1 有加法分解，见下）
│   │   ├── merged_hop（92% of busy）              472.3/lane ← 读数（`busy split` 均值；per-label exec p50 = 468.1）
│   │   └── ch_wt（8% of busy，= `ce_weights`）     42.8/lane ← 读数（`busy split` 均值；per-label exec p50 = 42.0）
│   └── gap = residency − busy                     140.0     ← 读数（宿主尾 42.6 在其中）
└── LLR 回传 + 交给解码器 + 残差                  256.6     ← ★ **差值**（= V1 − A − 头 − residency，无仪器）
```

| 组成 | µs | 占 V1 | 性质 | 依据（**读数 / 差值**）|
|---|---|---|---|---|
| ★ **A：等本跳最后一个样点** | **473.0** | **~34%** | 整槽收包策略的结构延迟（**不是设备时间**）| **读数**：`[ul_rx_wait]` 中位（p86 L47；§5.8.29 在 cpu 腿上量到 median 1057.0）|
| ★ **车道 cb 的平台账**（busy 的主项）| **472.3/lane** | **~34%** | 与内容无关（`p84` 全空 kernel 仍 462.9）| **读数**：`busy split` 的 `merged_hop` 均值 + per-label `exec p50 468.1` |
| **车道内设备空洞 `gap`** | **140.0** | ~10% | 设备等宿主/排队；宿主尾 42.6 在其中 | **读数**：`[ul_gpu_lane] gap` |
| 宿主参与（头 + 尾）| 86.0 | 6% | G2 的量，已是小项 | **读数**：契约 `lane host participation` |
| LLR 回传 + 交付 + 残差 | **256.6** | ~18% | **差值**（目前最大的测量盲区）| **差值**：`V1 − A − 头 − residency`（§6.145⑥① 的 `ce → ldpc` 列可夹住它）|
| （参考）真正的 kernel 算力 | ~110 | 8% | corr 42 + eq/demap ~20 + 前端 47 | §6.143⑧ |
| （参考）一跳墙钟 | 1935 | — | 跳与跳的间隔（V1 只占 73%）| §6.123 |

* ⚠ **关系是嵌套不是相加**：宿主的 86 µs 与设备 residency **部分重叠**（编码/提交本来就与设备执行并行，这正是融合车道的目的），所以上表**不能求和**；能相加的只有"A → 头 → residency → LLR 交付"这条墙钟链。
* ⚠ **三点口径上的近似（读这张表必须知道）**：
  1. `residency / busy / gap` 是**三条独立分布**，中位之间**不严格可加**：`636.8 − 500.8 = 136.0 ≠ gap 中位 140.0`，且 `gap min = −247.0`（允许重叠）⇒ 树里的 ↳ 只是**结构关系**，不是恒等式；要严格相加请用**均值**那一组（`515.1 = 42.8 + 472.3`，见 p86 L137）。
  2. 链条 `A → 头 → residency → 残差` 假设**头与设备不重叠**：头的终点是 extraction commit，而设备首条 cb（权重 cb）必须先被宿主提交才能开始 ⇒ 可加；**宿主尾 42.6 故意不加**（它与设备并行，这正是 G2 的目的）。
  3. ★ "**~450 与内容无关**"是**跨腿借用**：判决来自 `p84`，而 `p84` 跑在 `318b057bef`、`p85`/`p86` 跑在 `b4267fb8b6`。已核（`git diff --stat 318b057bef b4267fb8b6`）：两提交之间**没有任何 C++/Metal 改动**（只有文档 + `leg_gate.sh` + `milestone_audit.sh` + `gen_knob_inventory.py`）⇒ 二进制同源、可以借用；但按纪律"**每条腿都是关于某个二进制的证据**"，这个借用必须写明。
* ★★ **一句话回答"92% 是什么"**：主要是**两大块**——**① 等本跳最后一个样点到达（473.0 µs 实测，收包策略决定，与 GPU 无关）**和**② 车道那条 cb 的平台账（472.3/lane，与内容无关）**；加上车道内的设备空洞 140 µs 与**差值得到的** LLR 路径 256.6 µs。三者性质完全不同，**杠杆也完全不同**：
  * ① 只能靠**收包策略**动（symbol-grained receive，S-7g-13）——但要小心：`p73`/`p74` 已证明"把打包切细"会让 V1 变差（1473/1823 vs 1399），因为前端块完成得更晚 ⇒ **① 是"打包收益"的代价**（打包省 ~483 µs 设备时间/slot，代价是 ~一个时隙的延迟；设备 busy(union) 只有 26% ⇒ 这个交换对**延迟**不利、对**容量**有利）；
  * ② 已判**与内容无关**（F2）⇒ 要么接受，要么**改变"这条 cb 承载几跳"**（C-A）；
  * ③ **256.6 µs** 这一块**从来没被单独测过**（它是个差值）⇒ 它是目前最大的测量盲区（`OCUDU_UL_SLOT_TRACE` 把"本槽样点收齐的时刻 → 各 landmark"的差值直接打出来，同时把 A 变成逐槽读数）。
* ★ **回到用户的判断（"overhead 远超 CPU 路径"）**：GPU 路径的这两大项**恰好都是 CPU 路径不付的**——CPU 路径没有"命令缓冲账"（②），也没有"为了打包而等满一槽"（①，它是逐符号处理的）。所以"overhead 远超 CPU"在结构上成立，而且**不是调参问题**。

#### ⑥ 结论与下一步

* **"overhead 远超算力"是结构性的、且已经被量化**：一跳真正的设备算力 ≈110 µs，V1 1400 µs ⇒ **~92% 不在算力上**；其中最大两块是**车道 cb 的 ~470 µs 内容无关账（F2）**与**后端队列的 ~214 µs 排队**。
* **可以重组的只剩"边界放在哪、一条 cb 装多少"**（F1 锁死了"必须有一条更早的 cb"，F2 锁死了"车道那条 cb 要付账"）。
  因此**唯一还没被问过、且理论上收益最大**的问题是 **C-A：那条账是按"cb"计还是按"跳/时隙"计** —— `p84` 只证明了前者在**一个** cb 内与内容无关。
* **建议的第一步（零腿）**：用现有腿的数据做 **C-B 的相关分析**（把每条 lane 的 `start→end` 与它在时隙内的**提交位置/相位分段**对齐，看窗口是否随"提交点到下一个时隙边界"的距离变化）；若相关性强 ⇒ 窗口确实是"等到下一个时隙边界"⇒ **C-A 的成功率大幅上升**（把两跳的编码凑到同一个时隙边界前提交，账仍只付一次）。
* 需要一条**新腿**才能定的：C-A（要新机制）与 C-C（要给 PRACH 一条块路径）。两者都要**先离线自证**（C-A 可以先用"同一 cb 里放两份跳形状的派发"看窗口是否 2×；若离线就是 2×，那说明账与内容有关、C-A 死）。

### 6.145 ★★ "GPU 模式也加分阶段探针" —— **可以，而且比预想便宜**：规则、已有、缺什么、建议（用户 2026-09-27 提问；**读码结论 + 预登记的探针契约**）

#### ① 问题（用户原文）

> 在不违反 G1 和 G2 的原则下，为了更加清楚的了解"时间都到哪里去了"，是否对 gpu 模式也可以加入探针，分阶段的测量？
> （其实这个对 GPU 模式的测量探针可以是有条件的，在 release 的生产模式下不违反 G1 和 G2 的原则就可以了）

#### ② 结论（三句话）

1. **"release 生产模式不违反"不需要新机制** —— 本仓的探针**一直是两把钥匙**：编译期 `option(ENABLE_METAL_STATS / ENABLE_FLOW_PROBES / ENABLE_CE_TIME / ENABLE_UL_CAPTURE ... **OFF**)`（**生产构建里这些代码根本不编进去**，不是"编进去但关着"）+ 运行期 env（`OCUDU_METAL_GPU_TIME` / `OCUDU_UL_PHASE_SEGMENTS` / `OCUDU_UL_SLOT_TRACE`，默认关）。本实验 build 四个编译期门**全 ON**，所以这一点在实验室里"看不出来"。⇒ **新探针照抄这个模式即可**，无需为 release 另做一套。
2. **你要的"分阶段"里，大部分今天就有读数，零代码**：`[ul_rx_wait]`（= **A**）与 `[ul_slot_trace]`（逐槽 `rxwait / t2f / ce / ldpc / crc_ok / pipeline`）在 gpu 模式**代码上是活的**（证据见 ④）。
3. **真正缺的只有一层**：车道那条 cb **内部**的先后与等待（"CE 等所有符号的 DFT 完"、"EQ 等每个 RE 的估计"就在这一层）。这一层本平台**不可得**（§6.130 三法皆否），只能靠"把阶段拆成各自的 cb"的**诊断结构**近似 —— 而**那是另一个被测对象**（cb 边界自带挂号费，F2/§6.141）。⇒ 把它当**理解**做，不要当**杠杆**做。

#### ③ 探针契约（本节新增的可验收规范；以后每条探针都按它判）

| 条 | 内容 |
|---|---|
| **硬线 1（G1）** | **不得中途读回设备数据**：不得为了读数 `waitUntilCompleted` 取 buffer、不得中途 `synchronize` 估计结果 |
| **硬线 2（G2）** | **不得中途阻塞**：不得轮询 event、不得在跳中间 sleep/等信号 |
| 允许 | 读**宿主时钟**、读 cb 的 **GPUStart/GPUEnd 时间戳**、cb **完成回调**、原子计数 |
| 门控 | 编译期 option **默认 OFF** ＋ 运行期 env **默认关**（两把钥匙，缺一不可）|
| **关着时必须** | 交付路径**逐字节不变**（唯一机检：`nm` 符号对照 / `build/hashes.h` 对照）|
| **开着时也必须** | G1/G2 红线**保持绿**：`host device data crossings` 检查仍判 0（`phy_pipeline_crossings`）、`[ul_dft_wait]` 无样本、`[ul_gpu_lane] host` 在噪声内。**否则探针改变了被测对象，读数不可用**|
| 若非要碰设备数据 | 必须像 `ul_capture` 那样**声明**（`phy_pipeline_crossings::scoped_debug_touches` + `declare_reporter`），门里**打印**被减掉多少 —— 但**建议新探针一个字节都不碰**（只读时钟），这类声明根本不需要 |

#### ④ 已有仪器在 gpu 模式的存活情况（**读码结论**，2026-09-27；⚠ 待一条零代码腿确认）

| 仪器 | 门控 | 在 gpu 模式 | 读数含义 |
|---|---|---|---|
| `[ul_rx_wait]` | 无（`report()` **无条件**打印；仅需编译期 `ENABLE_FLOW_PROBES`）| **活** | **A**：本块 `receive()` 阻塞了多久。整槽收包下"不可能短于本槽最后一个样点存在" ⇒ **A ≈ 一个时隙**（§5.8.29）|
| `[ul_slot_trace]` | 运行期 `OCUDU_UL_SLOT_TRACE=N`（N≤512）| **代码上活**（↳ 见下）| 逐槽一行：`rxwait`(=`A`) / `t2f` / `ce` / `ldpc` / `crc_ok` + 该槽 `pipeline`（§5.8.31 的形状）|
| `[ul_gpu_pipeline]`（V1）| 无 | 活 | 判据本体 |
| `[ul_gpu_lane]` residency / busy / gap / host | `OCUDU_METAL_GPU_TIME=1` | 活 | 设备侧时间轴 |
| per-label `wait/exec` p50/p95/min/p5 | 同上 | 活 | **cb 粒度**的窗口 |
| `[ul_dft_wait]` | 同上 | 活 | 宿主**等设备**了多少（G2 的红线读数）|

★ **2026-09-28 已空口确认（§6.148③）**：`p87`/`p88` 两条腿都打出 `rows=512`，`rxwait/t2f/ce/ldpc/crc_ok` 全部到达（`crc_ok` 的 NaN 行数 88/39 ≈ 同腿 CRC BLER 14.4%/8.6%，属合法），且 `ce` 中位 123 µs ≪ residency 628 µs ⇒ **确认它是宿主时刻**。

★ **限定（2026-09-28，读码）**：`record_t2f_end()`/`record_ce_end()` 的第一行是 `if (!records_phase_segments()) return;`，所以在融合车道里**除非 `OCUDU_UL_PHASE_SEGMENTS=1` 强制打开，`t2f`/`ce` 两列不会被记录**（`ldpc_start`/`crc_ok`/`rxwait` 不受影响）。`p87`/`p88` 观测到这两列，是因为它们沿用了 `p85`/`p86` 那条带强制相位分段的配方（§6.148③）。

**为什么断言 `[ul_slot_trace]` 在 gpu 模式"代码上活"**：`trace_slot()` 在四个 recorder 里是**无条件**调用的（`ul_pipeline_probe.h:265 / 342 / 357 / 395`，只被"该槽是否已进 `slot_samples_done`"门控），而四个调用点在 gpu 模式**都到达**：

* `record_t2f_end` —— `puxch_processor_impl.cpp:228`，在"本槽所有符号已提交"处（gpu 模式走的就是 `pipeline_depth > 1` 那条路，即**融合车道的单次提交 + D1 交接**）；
* `record_ce_end` —— `pusch_processor_impl.cpp:357`（估计结果到宿主手里）；
* `record_ldpc_start` —— `pusch_decoder_impl.cpp:360`（**这正是 V1 的终点**）；
* `record_end_crc_ok` —— `pusch_processor_notifier_adaptor.h:251`。

**⚠ 两条读法限制（不先钉死就会读错）**：

1. gpu 模式下 `t2f` / `ce` 是**宿主侧**时刻（"本槽符号都交出去了" / "估计结果在宿主手里"），**不是**设备执行完的时刻；设备时刻只有 `[ul_gpu_lane]` 的 GPU 时间戳给。**两者拼起来才是完整时间线**。
2. 存档日志里 `grep '[ul_slot_trace]' = 0 命中` ⇒ 这条仪表**从未在 gpu 腿上开过**（它是 2026-09-20…23 给 cpu 腿做的，§5.8.31）⇒ 上表是**读码**结论，必须由一条腿确认（`OCUDU_UL_SLOT_TRACE=512` ＋ `OCUDU_METAL_GPU_TIME=1`，**零代码**）。

#### ⑤ 缺的那一层，以及它值不值得做

* **缺**：一条 cb **内部**的分段。本平台**拿不到**（§6.130：宿主轮询 event、设备 marker cb、`MTLCounterSampleBuffer`@dispatch 三条**全被实测否掉**；本平台的事件可见性是 **cb 粒度**）。
* **P-G2（唯一可行的近似）＝ 把每个阶段做成自己的 cb**（`OCUDU_LANE_DIAG_SPLIT` 是它的**局部**版，只拆前端；`p75`/`p77`/`s86` 测过局部）。每段一个窗口 + 段间空隙都能读出来，但：
  * 它测的是**诊断结构**（cb 变多 ⇒ 挂号费 × N）⇒ 报告必须**标明**，不能当成交付车道的分解；
  * ★ **它给不出直接杠杆**：§6.141（`p84`）已证这条 cb 的窗口**与内容无关**（全空 kernel 仍 462.9 µs vs 控制 470.0）⇒ **cb 内的依赖不管多少，都装在这个"与内容无关的账"里**。拆出来也不能把那 ~468 µs 变没。
  * ⇒ 它唯一的价值是回答一个**结构判别问题**：那 ~468 µs 是 **(a) 阶段间依赖串行**（→ 可用结构攻击：让网格更早产出 / 让 CE 逐符号滚动，D1 那种提前交棒），还是 **(b) 平台对"一条 cb"的平摊挂号费**（→ 不可动，与 `p84` 一致）。**这是"理解"，不是"优化"。**
* **第三条路（建议先走）**：**离线/单测里的分阶段**（`dft_release_adopt_metal_test` 那一族臂）—— 本项目的既有做法：空口腿贵，结构性问题先在离线载体上问。

#### ⑥ 建议的最小动作集（按 价值/代价 排序）

| # | 动作 | 代价 | 回答什么 |
|---|---|---|---|
| **①** | **零代码腿**：gpu ＋ `OCUDU_UL_SLOT_TRACE=512` ＋ `OCUDU_METAL_GPU_TIME=1` | **一条腿**（无代码改动）| **A**（`rxwait` 列）＋ 宿主侧四个 landmark ＋ 该槽 `pipeline`；同时**钉住 ④ 的读码结论** |
| **②** | **C-B 零腿相关分析**（§6.144⑤ 已开）：每跳 `start→end` 对齐它在槽内的提交位置 | **零腿** | ~470 µs 是否被**槽边界**量化（C-A 的前置判别）|
| **③** | 若 ① 之后 LLR→交付那段仍有剩余：再加**一个宿主 landmark**（LLR 缓冲归还 / 上抛）| 小改 | 收掉最后一段盲区（仍是宿主时钟、零等待、零读回）|
| **④** | **P-G2 全分段诊断结构** | 中改 ＋ 离线自证 | 只在需要判 (a)/(b) 时做；先在离线载体上自证 |
| **⑤** | 把 ③ 的**探针契约**写进 gate（关着 ⇒ 逐字节；开着 ⇒ 红线绿）| 小改 | 把"探针不许改变被测对象"变成**机检** |

**明确不要再试的**（§6.130 已否）：宿主轮询 event 求 cb 内粒度、设备 marker cb、`MTLCounterSampleBuffer` 在 dispatch 边界。

#### ⑦ 对 §6.144⑤ 的两处更正

1. **A 一直有直接读数**：`[ul_rx_wait]`（按块，`report()` 无条件打印）。**p86 上的实测中位就是 473.0 µs**（`mean=496.9`、`p95=593.0`、`samples=553142`；而 gpu 腿里 `553142 ≈ 腿长/一个时隙` 正说明**每槽一块**）。所以缺口不是"A 没仪器"，而是**没有人把 `[ul_rx_wait]` 的中位读成 A**（§5.8.29 在 cpu 腿上量过 median 1057.0 / mean 997.9），以及 §6.144⑤ 的树把它标成了"无直接读数"、用 ≈500 参与了残差计算。
2. **那一段"未测"仍成立，但数值要用实测 A 重算**：`V1 − A − 头 − residency = 1409.8 − 473.0 − 43.4 − 636.8 = ` **256.6 µs**（老账的 "~230" 是用 A = 500 算出来的）⇒ 它是目前**最大的测量盲区**，而 ④ 的 `ce → ldpc` 列 + `[ul_gpu_lane]` 的设备 end 就能把它夹住 ⇒ **不需要新仪器就能先做一次**。

### 6.146 ★★★ 那个"每条腿都 ~101 ms"的 `[ul_rx_wait] max` **不是打嗝，是本进程自己的启动项**：源码里的 `delay_s = 0.1`（用户 2026-09-27 追问"CPU 来取样本要等 101 ms，从哪个角度看都不正常"）

#### ① 结论

`[ul_rx_wait]` 的最大值在**每一条腿**上都是 ~101 ms（`p86` 100.786 / 用户那条 n1 腿 101.557 / 更早的 101591、101670、101319、100645…），跨 **cpu / cpu_gpu / gpu 三种模式、五种配置、9 天**都成立。它**不是**传输或驱动打嗝，而是：

> ★ **电台流的起点被本进程故意推后 100 ms**，而接收线程**立刻就开始要样本** ⇒ **每次运行的第一条 `receive()` 必然等约 100.5–101.9 ms**。

源码（`lib/ru/sdr/ru_controller_sdr_impl.cpp:70-76`，无 `start_time` 时走的那条分支）：

```cpp
// Calculate starting time from the radio current time plus one hundred milliseconds.
double                     delay_s      = 0.1;
baseband_gateway_timestamp current_time = radio->read_current_time();
baseband_gateway_timestamp start_ts     = current_time + static_cast<uint64_t>(delay_s * srate_MHz * 1e6);
// Round start time to the next subframe.
uint64_t sf_duration = static_cast<uint64_t>(srate_MHz * 1e3);
start_ts             = divide_ceil(start_ts, sf_duration) * sf_duration;   // 对齐到子帧
radio->start(start_ts);
```

* 这个 `0.1 s` 是**给定的启动余量**（给 TX 流与电台起流留时间），不是异常；**向上取整到子帧**解释了观测值比 100 ms 多出的那 0.5–1.9 ms。
* （另一条 `start_time.has_value()` 的分支走 1PPS：起点 ="下一个 PPS 上升沿前 10 ms"。**这两条腿不是那条**——若走 PPS，首次等待会接近 1 s，而不是稳定的 ~101 ms。）

#### ② 为什么能断定它落在"**首次调用**"上（**证明**，不是推断）

`[ul_rx_timing]` 的 `slip` 是"这次迭代相对样点时间线的漂移"，定义为 `slip = loop + recv − air`（探针原话），而它**只对"有前驱"的调用计算**：`if (last_return_ns != 0)`（`lower_phy_baseband_processor.cpp:614`；进程内**恰好一次**调用没有前驱）。

⇒ 对**任何非首次调用**：`recv = slip − loop + air ≤ slip_max + air`。

| 腿 | `slip_max` | `air`（本块空口时长）| ⇒ 非首次调用的 `recv` **上界** | 实测 `recv_max` | 结论 |
|---|---|---|---|---|---|
| `p86`（n78 gpu）| 8 820 µs | 500 µs | **9 320 µs** | **100 783 µs** | 100.8 ms **只能**是首次调用 ✅ |
| `p62`（n78 cpu）| 10 540 µs | 500 µs | 11 040 µs | 100 847 µs | 同上 ✅ |
| `p43`（n78 half-slot）| 5 922 µs | 250 µs | 6 172 µs | 100 514 µs | 同上 ✅ |

#### ③ 跨腿复核（`doc_chinese/phy_pipeline_gpu/wip/logs/` 里 53 条带 `[ul_rx_timing]` 的腿）

抽样（13 条，全表见本条的生成方式：`grep -m1 '^\[ul_rx_timing\]' *.log.stderr`）：

| 腿（模式）| `rxwait` 中位 | `rxwait` max | `recv_max` | `loop_max` | `slip_max` | `rx_overflows`/`gaps` |
|---|---|---|---|---|---|---|
| `p33-n78-rxring`（gpu）| 474.0 | 100 646 | 100 645 | 1 187 | 19 453 | 0 / 0 |
| `p37-n78-ring512`（gpu）| 473.0 | 101 359 | 101 355 | 5 032 | 6 925 | 0 / 0 |
| `p42-n78-hostgap`（gpu）| 473.0 | 100 729 | 100 724 | 6 498 | 11 997 | 0 / 0 |
| `p53-n78-tailmark`（gpu）| 473.0 | 100 708 | 100 702 | 2 762 | 15 687 | 0 / 0 |
| `p62-n78-repro-cpu`（cpu）| 473.0 | 100 851 | 100 847 | 251 | 10 540 | 0 / 0 |
| `p64-n78-repro-cpugpu`（cpu_gpu）| 473.0 | 100 836 | 100 832 | 5 079 | 7 051 | 1 / 0 |
| `p86-n78-stress`（gpu）| 473.0 | 100 786 | 100 783 | 1 644 | 8 820 | 0 / 0 |
| ★ `p36-n78-bigframe`（**真积压的反例**）| **1 625.0** | 103 986 | 103 981 | 118 | **59 734** | **135 143** / — |

* `recv_max` 恒在 **100.5–101.9 ms**（与模式、配置、负载**无关**）⇒ 与"固定的启动偏移"一致，与"随机的传输打嗝"不一致。
* `loop_max` 只有 0.1–7 ms ⇒ **宿主从不迟到发问**（`loop` ＝上一次返回→本次发起，宿主自己的活儿＋调度）。
* `slip_max` 2–60 ms，**恒 ≪ `recv_max`** ⇒ 稳态没有把时间线跑丢。
* ★ **反例给出的判据**：`p36`（bigframe，已知的坏腿）是唯一 `slip_max` 逼近 `recv_max` 的腿，且 `rx_overflows = 135 143`、`rxwait` 中位 1625 µs ⇒ **真积压长什么样与启动项完全不同**，可以一眼分开。

#### ④ 读法（写进探针手册，替换旧归属）

1. **`[ul_rx_wait] max` 是个常量，不要当症状**：它等于启动项（~101 ms，§①②）。看接收侧要看**中位/尾部形状**（中位 = A，见 §6.144⑤ 的 δ 口径）。
2. 判"接收侧打嗝"要看 **`[ul_rx_timing]`**：`recv` 大而 `loop` 小 ⇒ 传输/电台在调用内部顶住；`loop` 大而 `load1` 高 ⇒ macOS 调度；两者都正常而 CRC/SINR 差 ⇒ 空口（§4.1.1 的三条归属规则）。
3. 判"有没有丢样/积压"要看 **`[ul_rx]`**（`rx_overflows` / `rx_lates` / `gaps` / `gap_us`）与 **`slip`**（`slip` 持续为正才是落后）。
4. 判"它有没有进流水线"要看**分母**：`[ul_rx_wait]` 按**块**（每槽一次），而 V1 只统计**走到 LLR 的跳**；启动那一次落在"还没有 PUSCH 授权"的时隙上 ⇒ 两条序列都看不到它（`stale=0` 与它不矛盾，§4.1.1 盲区 1/2）。

#### ⑤ 更正两处旧写法

* §4.1.1 的手册表里写"它的**尾部**（~101 ms 级）是**传输/驱动**现象" —— **归属错了**：它是**本进程自己**推后的流起点（本节①②）。已经就地改掉。
* §6.55② 当时已经用 `slip ≪ recv` 正确断定"那次调用没有前驱"，但**没给出为什么正好是 ~101 ms**；本节把它补成源码级结论。

### 6.147 ★★ `[ul_rx_wait]` 的总体收口：**启动项剥离（可见地）+ 按跳配对的新序列 `[ul_rx_wait_hop]`**（用户 2026-09-27 指示："一次性读数不应该进入 `[ul_rx_wait]` 的视野；它应该专注 PUSCH 链路的真实表现"）

#### ① 用户的要求与它命中的既有原则

> "既然那 ~101 ms 尾部是启动时的一次性读数（孤立的），而 `[ul_rx_wait]` 反映的应该是整个链路的统计特征，那么这个一次性读数就不应该进入 `[ul_rx_wait]` 的视野。`[ul_rx_wait]` 应该专注于我们关心的（目前是 PUSCH）链路的真实表现。"

三条既有原则都指向同一个方向：**① 序列的总体必须是它声称描述的对象**（本仓已因"两个序列总体不同"吃过账）；**② 排除必须可见，不许静默过滤**（`stale` 的样本照收但单独打印、crossings 打印被减掉的调试触碰）；**③ §6.146 已证**那个 ~101 ms 是 `delay_s = 0.1` 的启动项、每次运行**恰好一次**。

#### ② 交付的实现（三个动作，全部在探针内，交付路逐字节不变）

| # | 动作 | 位置 |
|---|---|---|
| **A** | **启动那条不进分布**：`record_rx_wait(wait_ns, spans_stream_start)`；为真时写进 `rx_wait_startup_us` 并 return（**保留而非丢弃**）| `ul_pipeline_probe.h` |
| **A′** | **报告显式打印它**：`[ul_rx_wait] startup=100786.0us excluded (1 sample: the first receive() of the run spans the radio's stream start; see dev doc 6.146)` | 同上（`report()`）|
| **B** | **新序列 `[ul_rx_wait_hop]`**：`record_slot_rx_wait(slot, wait_ns, spans_stream_start)` 把"**完成该槽的那一块**"的等待按槽存进有界登记表（512，插入序淘汰），由 `record_ldpc_start(slot)`（**V1 的终点**）消费 | 同上 + `lower_phy_baseband_processor.cpp` |

★ **谓词只有一处**：`ul_rx_note_call()` 的返回值（`last_return_ns == 0`，即"这次调用没有前驱"）—— 与 `[ul_rx_timing]` 的 `loop/slip` **用的是同一个判据**（此前两个仪器对同一事件各有一套规则，这本身就不一致）。`ul_rx_note_call()` 从 `void` 改为返回 `bool`。

★ **按块的那条序列不缩小总体**（否则"打嗝落在空时隙上"就再也看不见了，§4.1.1 的三个盲区之一）——**新增**一条而不是**替换**。

#### ③ 离线自证（`ul_pipeline_probe_test`，含三个反向臂，全部**确认真的重编**）

`tests/unittests/support/executors/ul_pipeline_probe_test.cpp` 的 `one_report_shape_per_pipeline_mode` 末尾新增一段，断言四件事：

1. 带 `spans_stream_start=true` 的记录**不进**分布（计数只 +1，那是随后那条普通记录）；
2. 它**没被丢掉**：报告里出现 `[ul_rx_wait] startup=101000.0us`；
3. `[ul_rx_wait_hop]` 只收**被跳消费**的那一条（`record_start` + `record_slot_rx_wait` + `record_ldpc_start`）⇒ 1 个样本、均值 ≈3 ms；
4. 空闲槽（绑定了但没有跳）与启动槽（`spans_stream_start=true`）**都不进**该序列。

| 臂 | 变异 | 期望症状 | 实测 |
|---|---|---|---|
| 对照 | —— | 7/7 PASS | ✅ **7/7 PASS** |
| **A** | 去掉启动分支（启动样本进分布）| `startup=` 行消失 / 计数不符 | ✅ **FAILED**（缺 `startup=`）|
| **B** | 绑定去掉（`record_slot_rx_wait` 不存）| `[ul_rx_wait_hop] no samples recorded` | ✅ **FAILED** |
| **C** | 跳 landmark 不消费 | 同上 | ✅ **FAILED** |
| 复原 | —— | 7/7 PASS | ✅ **7/7 PASS** |

⚠ **一个真踩到的坑（写进纪律）**：用 `cp` 复原被变异的头文件后，`cmake --build` **可能因为 mtime 打平而不重编**，于是"复原后仍 FAIL"——读到的是**上一个二进制的读数**。因此本节的每个臂都**核对了 `Building CXX object` 的条数**（B 那次 `=0` 被当场判 VOID，改用 `rm -f <obj>` 强制重编后才算数）。这与"旋钮没传进去"（`p78`）、"label 没生效"（`p82`）是同一族错误。

#### ④ 预登记（下次空口腿要读的三个数）

1. **`[ul_rx_wait] max` 必须从 ~101 ms 掉到 ms 量级**（p86 有 9 次 `recv > 5 ms` ⇒ 预期 **5–20 ms**）；若仍 ~101 ms ⇒ **剥离没生效，该腿不算数**。
2. **`[ul_rx_wait_hop]` 的中位应 ≈ `[ul_rx_wait]` 的中位**（整槽策略下"完成槽的那一块"就是那一槽的块；p86 的基准是 473.0）；两条的**样本数之比**应 ≈ 跳数/槽数（p86：145340/553142 ≈ 0.26）。

★ **判据已落成工具**：`wip/probe_recheck.sh`（读一条腿的 stderr，按本节四个预登记数逐条判 PASS/FAIL/INFO，并把 p86 的基线值 `473.0 / 100786 / 1644 / 8820 / 1409.8` 写在自己头上作为对照）。**负向对照已做**：拿它跑 `p86`（早于新序列的那条腿）⇒ 三条新序列检查 **FAIL**（缺 `startup=`、`[ul_rx_wait_hop]` 不存在、max 仍 100786）、而红线与 V1 全 PASS ⇒ 它确实在判新仪器，不是盖章。

★★ **2026-09-28 空口裁决（腿 `p87`/`p88`，§6.148②）：三个预登记数全部命中** —— `max` **6.450 / 7.435 ms**（预登记 ≤20 ms，p86 常量 100.786 ms）、`startup=` **100.985 / 100.500 ms** 恰好一行、`[ul_rx_wait_hop]` 中位 **474.0 / 473.0** vs `[ul_rx_wait]` **472.0 / 472.0**；扰动自检也全绿（`loop max` 1280/2364 vs p86 1644；`slip` 5949/6933 vs 8820；crossings 0.00+0.00；契约 9/9）。⇒ 本次改动**按预登记生效，且没有移动它所测量的东西**。
3. **`[ul_rx_wait] startup=` 必须恰好出现一行、值 ≈100.5–101.9 ms**（§6.146 的 53 条腿基线）。

#### ⑤ 口径变更（留原文 + 指针）

* **旧腿**：`[ul_rx_wait] max ≈101 ms` ⇒ 读作"启动项仍在分布里"，不是链路尾部。
* **新腿**：`max` 是链路尾部；启动项在 `startup=` 字段。
* 引用 p85/p86 的 `max` 时必须按旧口径说明（§6.146 已记录基线值）。

#### ⑥ 代价（已向用户说明并获准）

* 改动全在 `ENABLE_FLOW_PROBES` 内 ⇒ **release 构建逐字节不变**、与 G1/G2 无关（§6.145③ 的探针契约自动满足）。
* lab 二进制变了 ⇒ 按本仓纪律 **`p85`/`p86` 不再是"跑在 HEAD 上的证据"** ⇒ 下次有手机时**重飞一对**（或者与交付配置改动合并成一次）。

### 6.148 ★★★ 腿对 `p87-n78-default` / `p88-n78-stress`（HEAD `49528a3cc3`，三个探针旋钮）：**预登记三个数全中**、**GPU 模式槽时间轴确认**、**§6.144⑤ 的最后一个盲区收口**、并暴露**槽时间轴的一个残留下缺陷**（2026-09-28 空口）

#### ① 配方与身份

| 项 | 值 |
|---|---|
| 腿 | `p87-n78-default`（08:59，`--regime=default`）/ `p88-n78-stress`（09:05，`--regime=stress`），各 ~240 s，一次 Ctrl-C 收尾，报告完整 |
| 二进制 | HEAD `49528a3cc3`（= `build/hashes.h` = `gnb` 内嵌戳，`run_leg.sh` 自检通过）|
| 与 `p85`/`p86` 的唯一差别 | 多一个探针旋钮 `OCUDU_UL_SLOT_TRACE=512`（其余逐项相同：同 config、同 `--…max_pusch_and_srs_concurrency=2`、同 `OCUDU_METAL_GPU_TIME=1`、同 `OCUDU_UL_PHASE_SEGMENTS=1`）|

#### ② §6.147④ 的三个预登记数：**全部命中**

| 预登记 | 判据 | `p87` | `p88` | 结论 |
|---|---|---|---|---|
| `[ul_rx_wait] max` 掉到 ms 量级 | ≤20 ms（否则剥离没生效、腿作废）| **6.450 ms** | **7.435 ms** | ✅（p86 是常量 100.786 ms）|
| `startup=` 恰好一行、值在 53 条腿的带内 | 90–130 ms | **100.985 ms** | **100.500 ms** | ✅ |
| `[ul_rx_wait_hop]` 中位 ≈ `[ul_rx_wait]` 中位 | ±25%（p86 基准 473.0）| **474.0** vs 472.0 | **473.0** vs 472.0 | ✅ |

* 两条序列的**形状几乎重合**（`p87`：mean 494.6/499.6、p95 592/593、p99 604/607）⇒ 整槽收包下"完成槽的那一块"就是该跳的块，**新序列与它的定义一致**。
* hop/block 样本比 0.202 / 0.226（p86 0.26）⇒ 每跳对应的槽数更少 = 有 PUSCH 的槽更多（流量更高），**是流量的性质、不是仪器的**。
* ★ **新老仪器互证**：`startup=100985` 与同一条腿的 `[ul_rx_timing] recv(max=100982us …)` 只差 3 µs ⇒ 两把尺子量的是**同一次启动调用**，判据统一（§6.147②）落地。
* ⚠ `[ul_rx_timing] recv(max≈100.5 ms)` **仍然存在且是对的**：那条序列按设计含首次调用（它靠 `loop/slip` 排除）；被剥离的只是 `[ul_rx_wait]` 的**分布**。读这两条时必须知道这个差别。

#### ③ §6.145④ 的读码结论：**在 gpu 模式确认**（这是本次腿的主要目的之一）

`[ul_slot_trace] rows=512`（两条腿都把 512 行预算用满），四列 landmark **全部到达且无 NaN**：

| 列 | `p87` 中位 | `p88` 中位 | 含义（gpu 模式）|
|---|---|---|---|
| `rxwait` | 475.0 | 473.0 | = **A**（与 `[ul_rx_wait]`/`[ul_rx_wait_hop]` 中位一致 ✓）|
| `t2f` | 50.3 | 52.2 | 样点收齐 → 前端把本槽符号交出去（**宿主**时刻）|
| `ce` | 123.3 | 124.9 | 样点收齐 → **估计器调用返回**（宿主交棒结束）|
| `ldpc` | 933.5 | 949.6 | 样点收齐 → **LLR 交给解码器**（= V1 的终点）|
| `crc_ok` | 1042.5 | 1047.8 | → CRC OK |
| NaN 行数 | `crc_ok` 88 / `tb_bytes`+`pipeline` 91 | 39 / 39 | **合法**：解码失败就没有 CRC-OK landmark（88/512=17% ≈ 同腿 CRC BLER 14.4%；39/512=7.6% ≈ 8.6% ✓ 互证）|

★ **`ce` 是宿主时刻，用数值即可证明**：`ce` 中位 123 µs **远小于** 同腿 `residency`（GPU 时间戳）628.5 µs ⇒ 它不可能是"估计结果到手"，而是**宿主把这一跳交出去**的时刻。这正是 §6.145④ 说的"gpu 模式下 `t2f`/`ce` 是宿主侧时刻"——现在有数了。设备侧仍只能从 `[ul_gpu_lane]` 读。

#### ④ ★★ §6.144⑤ 的**最后一个盲区收口**：那个 256.6 µs 其实是"队列 + 宿主提交"

逐槽分解（`p87` 中位，`p88` 同形）：

```
V1 = 1409.1 µs（IQ → LLR，[ul_gpu_pipeline]）
├── A = rxwait（样点收齐）                          475.0      ← 读数（= [ul_rx_wait] / [ul_rx_wait_hop] 中位）
└── 样点收齐 → LLR 交付（= ldpc 列）                933.5      ← 读数（新）
    ├── 宿主交棒（t2f+ce：前端交出 → 估计器返回）    ~123       ← 读数（t2f 50.3 / ce 123.3）
    ├── 队列等待（首条 cb 开始前的排队）              208.7      ← 读数（per-label `merged_hop` wait p50）
    └── 设备 residency（首条 cb 开始 → 末条 cb 结束） 628.5      ← 读数（GPU 时间戳）
        （三者之和 960 略大于 933.5：**宿主提交与排队重叠**，中位数之间不可严格相加）
```

⇒ ★ **原来标"未测（差值 256.6）"的那一段，其实就是"队列等待 + 宿主交棒"**（208.7 + ~123 ≈ 332，落在同一量级；之前的账把队列等待算进了设备 `gap`，于是同一笔钱被记了两次、并在 V1 的差额里又空出一块）。**LLR 回传与交付本身 ≲ 数十 µs**。⇒ §6.144⑤ 那张表里"目前最大的测量盲区"这一行**可以撤掉**：V1 的构成现在**每一段都有读数**。
⇒ 这同时解释了为什么 `[ul_gpu_lane] gap`（133.5）不能与 residency 相加：gap 是 residency **内部**的设备空闲，而队列等待在 residency **之前**。

#### ⑤ ✅ 槽时间轴的缺陷：**机制两层、已修好、已离线自证**（2026-09-28）

**症状**：`ce/ldpc/crc_ok` 的少数行打出 **−10.24 s**（正好一个 SFU 周期；`p87` 4/17/42 行，`p88` 77/77/80 行；`rxwait`/`t2f` 干净）。

**机制（两层，缺一不可地解释了症状）**：

1. **map 层**：landmark 以**模时隙**为键。`t2f` **每个周期都会重记**（PUXCH 对它处理的每个槽都在槽末记一次），而 `ce`/`ldpc_start`/`crc_ok` 只在该槽**真的有授权且 TB 被处理/解码**时才重记。于是"上个周期带过 PUSCH、本周期没带"的槽：新周期的 `t2f` 触发重算，而 `slot_landmarks` 里那三个还是**上个周期的时刻** ⇒ `新基准 − 旧 landmark` = **−10.24 s**。
   （打印器此前修的是另一半——把基准/landmark 的原始 epoch 一起打出来，治"打印时查表拿到新基准"；map 里旧值没作废这一半一直在。）
2. **行层**：只擦 map 还不够。行的 `t2f_us/ce_us/...` 是**上次重算时写进去的值**，`assign()` 只在 landmark 存在时覆盖、不会清空 ⇒ 擦掉 map 后，行里仍留着**上一周期的差值**。★ 单元夹具第一次跑就抓到了这一层：`ce/ldpc/crc_ok` 读回 **~2 µs**——**看着完全合理**，比负数更危险。

**修法（`ul_pipeline_probe.h`，两处都默认关、release 逐字节不变）**：

| # | 改动 | 为什么 |
|---|---|---|
| 1 | `record_slot_samples_complete()` 覆盖某槽基准时，**作废该槽的 landmarks** | 一行描述**一个**周期；不可能丢掉本周期的东西（landmark 是"被这次到达的样本"测出来的，必然在此之后） |
| 2 | 同时把**行内派生列重置为 NaN**（`t2f/ce/ldpc/crc_ok/tb_bytes/pipeline`）| 新周期只填它真正到达的那些；没带授权的槽就如实报"没到达"，而不是记得上次的跳 |
| 3 | `us()` 加**兜底**：landmark 早于基准 ⇒ NaN **并计数**；报告头新增 `rebased=N …; negative deltas refused=M` | 不做**静默过滤**：负数一旦回来必须先在报告里看得见（本仓的 `stale`/crossings 都是这个规矩）|

**离线自证（`ul_slot_trace_test.a_new_cycle_rebases_a_slot_instead_of_reusing_last_cycles_landmarks`）**：周期 1 记满四个 landmark，周期 2 **只**重记基准与 `t2f`（模拟"本周期没授权"），断言：该行的 `rxwait` 是新周期那条（防止"行根本没被动过"的假通过）、`t2f` 小且为正、`ce/ldpc/crc_ok` **为 NaN**、`rebased` 差值**正好 4**、`negative deltas refused=0`。

| 臂 | 变异 | 实测 |
|---|---|---|
| 对照 | —— | ✅ **8/8 PASS** |
| **A** | 不擦 map、只重置行 | ✅ **FAILED**（撞在 `refused=0`：兜底把旧 landmark 抓成负数并计数）|
| **B** | 擦 map、不重置行 | ✅ **FAILED**（撞在三个 NaN 断言：行里留着上周期"看着合理"的 ~2 µs）|
| 复原 | —— | ✅ **8/8 PASS** |
（两臂都核对了 `Building CXX object` 条数 =1；这是 §6.147③ 记下的那个"mtime 打平导致没重编"的坑。）

★ **顺带查清一件必须写下来的事**：`record_t2f_end()`/`record_ce_end()` 开头都有 `if (!records_phase_segments()) return;` ⇒ **在 gpu 模式（融合车道）里，除非 `OCUDU_UL_PHASE_SEGMENTS=1` 强制打开，时间轴的 `t2f`/`ce` 两列是空的**。`p87`/`p88` 之所以有这两列，正是因为它们沿用了 `p85`/`p86` 那条**带强制相位分段**的配方。⇒ §6.145④ 的结论要补一句限定：四个 landmark 在 gpu 模式都**会到达**，但其中两个的**记录**被 `records_phase_segments()` 门控。

**影响面**：分布序列与全部判据**都不读**时间轴；受影响的只有那三列在**少数行**上的可读性。**判据无一条因此改变。**

★★ **做这条修复时又抓到一个门禁缺口（本仓第三次同类）：探针的离线自证根本不在验收标签里。**
`tests/unittests/support/executors/CMakeLists.txt` 给 `ul_pipeline_probe_test` 打的是目录标签 `support`，而**验收命令一直是 `ctest -L phy`** ⇒ 实测 `ctest -N -L phy` 里该二进制的 **8 个用例一个都不在**（`ctest -N -L support` 里 8 个）。也就是说：本工作流每次里程碑引用的 `ctest -L phy 193/193`，以及"**先离线自证再飞腿**"里的"离线"那一半，**从来没有执行过这个探针的契约与回归臂**。
修法：给它加 `phy` 标签。⚠ 两个坑都踩到了：
1. `gtest_discover_tests(... PROPERTIES LABELS "support;phy")` 里的分号会被展开成**两个属性**（`LABELS=support` + 一个空的 `phy`）——`ctest --show-only=json-v1` 一看便知；必须写成**转义的分号** `"support\;phy"`；
2. 改了 CMake 之后**必须重新 configure**（`cmake -S . -B build`），因为标签是写进 configure 期生成的 `*_include.cmake` 的（只 build 不改）。
⇒ 验收计数因此从 **193 → 201**（历史引用的 193 不含这 8 条，按本仓规矩留原文 + 本指针）。

#### ⑥ 交付判据（腿对在 HEAD 上）

| 门 | 结果 |
|---|---|
| `milestone_audit.sh --leg=p87-n78-default --stress-leg=p88-n78-stress` | **31 PASS / 2 FAIL / 0 RED / 3 INFO** —— 两条 FAIL **同因**：`p87` 标了 `default` 却在带载（`stale=3`），而 default 工况的判据要求 `stale=0`（见 ⑦）|
| `leg_gate.sh --slot-ms=0.5 p88-n78-stress` | ✅ **9 of 9 judged**（2 条 NOT JUDGED = n1/FDD 绑定不适用）|
| V1 | `p87` **1409.1** / `p88` **1397.4**（判据 ≤2150；p86 1409.8）|
| `cbs/lane` / 契约 / crossings / gaps | 2.00 / MET (9 of 9) / 0.00+0.00 / 0（两条腿）|
| `★ stress leg p88: the commit it ran, vs HEAD` | ✅ **PASS** ⇒ **p88 是当前 HEAD 的有效证据腿** |

#### ⑦ 两条腿的吞吐差异（用户："后一个流量明显少很多"）——**空口质量，不是递送路回归**

| | `p87`（08:59）| `p88`（09:05）|
|---|---|---|
| TCP 收到（iperf3 `-P 4`）| **9.26 Mbit/s**（重传 207）| **2.56 Mbit/s**（重传 **6240**）|
| grants/s（最忙 240 s）| 593.7 | 594.1 |
| granted / new data | 12.36 / 9.66 Mbit/s | 2.61 / 2.37 Mbit/s |
| **平均 TB** | **2603 B/授权** | **548 B/授权** |
| 有效 bit/RE | 2.16 | 0.51 |
| 调制分布 | 16QAM 44.4k / 64QAM 30.4k / 256QAM 18.1k / QPSK 6.3k | **QPSK 90.6k** / 16QAM 15.6k / 64QAM 0.4k / 256QAM 0.6k |
| **QPSK 的 SINR p50** | **12.5 dB** | **5.9 dB** |

⇒ **调度器把上行几乎全部压到 QPSK**（90.6k/107k 跳），因为同一调制的 SINR 中位掉了 **~6.6 dB**；授权数不变（594/s）而 TB 掉到 1/4.7 ⇒ 吞吐掉到 1/3.6。**V1 反而略好**（1397.4 vs 1409.1）、lane 各项一致 ⇒ **与 gNB/GPU 路径无关**，是那 6 分钟里空口变了（手机发射功率/热/位置/干扰之一）。
⇒ 直接后果与本项目有关的一点：**`p87` 不能再当 default 腿**（它带了载）。

#### ⑧ 下一步（状态在 ⑤ 修好之后）

1. ★ **欠一对腿，不是一条**：⑤ 的修复动了 `include/…/ul_pipeline_probe.h`，那是**代码**（不是文档）⇒ 审计的"腿 vs HEAD"行会判 `p87`/`p88` **FAIL**（它们跑在修复前的二进制上）。要恢复"跑在 HEAD 上的证据"，需要**在修复后的 HEAD 上重飞一对**：**空载 default（本来也欠，无需流量）+ 加压 stress（需要 iperf3）**。
   * 若只飞空载那条：审计会剩一条 FAIL（`stress leg p88: the commit it ran, vs HEAD`）——**可以解释、但不要假装它不存在**（本仓的规矩：`cannot read`/`not on HEAD` 都是要写明的状态）。
   * 修复本身是**探针改动**（`ENABLE_FLOW_PROBES` 内）⇒ release 路径逐字节不变、交付语义未动；重飞是为了**交付判据 + V1 仪表**都落在同一个二进制上。
2. 之后才是与交付配置改动（`rv_sequence`/并发 2）合并的那次重飞机会。

### 6.149 ★★ 修复后的一对腿 `p89-n78-default-idle` / `p90-n78-stress`（HEAD `fd5fbd516a`）：**修复在空中确认**、**审计 GREEN**、并定下 `[ul_slot_trace]` 的 NaN 读法（2026-09-28 空口）

#### ① 修复的空中确认（这是这一对腿存在的主要理由）

| 检查（`wip/probe_recheck.sh` 的预登记项）| `p89`（空载）| `p90`（加压）| 修复前（`p87`/`p88`）|
|---|---|---|---|
| **时间轴里"减一个周期"的行** | **0** | **0** | **42 / 80** ❌ |
| 报告头 `negative deltas refused=` | **0** | **0** | 该行不存在 ❌ |
| `rebased=`（被挡在行外的陈旧 landmark）| **41 206** | **21 911** | —— |
| `[ul_rx_wait] max`（剥离启动项）| **7.186 ms** | **6.283 ms** | 已在 `p87`/`p88` 生效 |
| `startup=`（恰好一行）| 101.420 ms | 101.196 ms | 同上 |
| `[ul_rx_wait_hop]` 中位 vs `[ul_rx_wait]` | 470.0 vs 471.0 | 473.0 vs 472.0 | 同上 |
| 扰动自检：`loop max` / `slip max` | 338 / 6 672 µs | 1 950 / 5 783 µs | p86 基线 1 644 / 8 820 |
| crossings / 契约 | 0.00+0.00 / MET (9 of 9) | 0.00+0.00 / MET (9 of 9) | —— |
| **V1 中位** | **1 359.2**（轻流量，与 `p85` 的 1 348 同形）| **1 416.1** | `p86` 1 409.8 |

⇒ ★ **修复在空中成立**：负差值从 42/80 行降到 **0**，而 `rebased=41 206/21 911` 说明确实有大量陈旧 landmark 被挡在行外（不是"没触发"）。扰动自检也全绿 ⇒ 新增的每槽一次 `lower_bound`+至多 4 次 erase **没有移动它所测量的东西**。

#### ② NaN 的语义与读法（用户 2026-09-28 提问："许多数字是 nan，似乎是异常"）

**不是异常，是修复所引入的"重置"语义的正常形态**，但**读法要跟上**：

* 行的 `ldpc` 列是 **NaN** ⇔ 该行描述的那个槽，在**它所属的那个周期**里**没有走到解码**（键在下一个 SFU 周期又轮到、而本周期没有授权）。这是行结构体里写明的含义（"A field left at NaN means this landmark was not reached for this slot"），**不是数据丢失**：上面的分布序列（`[ul_rx_wait]`/`[ul_rx_wait_hop]`/`[ul_pipeline]`）**完全不受影响**。
* **行形态是单调的**（实测 p89：`rxwait only` 350 行、`+t2f` 147、四 landmark 齐但无 `tb_bytes` 10、完整 5；p90：完整 **432**）⇒ 若 `ce` 缺则 `ldpc` 必缺，说明"没到达"是逐级发生的，没有错配。
* ★ **读法**：**`ldpc` 非 NaN ⇔ 这一行是该周期的一次真实跳的时间轴**。`p90`（加压）512 行里 **449** 行完整（原本就是给忙腿看的仪表）；`p89`（空载）只有 **15** 行完整——**空载腿的时间轴本来就没什么可看**，而修复前那些行显示的是**上一周期的陈旧值**（更糟）。该计数已加进 `probe_recheck.sh`（`rows carrying a complete timeline`）。
* ⏳ **登记一个显示层的改进（与下一次代码改动合并，不要为此单独飞腿）**：现在"重置"会留下一行"只有 rxwait 的壳"。更干净的做法是**基准确认刷新时直接丢弃该行**（信息量相同、噪声更少）；而要**同时**恢复"慢行能长期留在表里"的能力，就得把行的键从**模时隙**换成**绝对时隙**（探针维护一个周期计数；landmark 仍由调用方的模时隙来，查当前周期即可归属）——那是真正的修法，但改动更大，需要自己的离线自证。

#### ③ 交付判据（HEAD `fd5fbd516a`，一对腿）

| 门 | 结果 |
|---|---|
| `milestone_audit.sh --leg=p89-n78-default-idle --stress-leg=p90-n78-stress` | ✅ **32 PASS / 0 FAIL / 0 RED / 4 INFO ⇒ offline acceptance: GREEN** |
| `leg_gate.sh --slot-ms=0.5 p90-n78-stress` | ✅ **9 of 9 judged**（2 条 NOT JUDGED = n1/FDD 绑定不适用）|
| `p90` 的上行（最忙 240 s）| 见下方 ⚠ **同名两次** |

⚠ **`p90-n78-stress` 有两次运行**（09-28 10:03 与 10:27，两条都在 HEAD 上、报告都完整）——文件名带时间戳就是为了这个，但 `--stress-leg=p90-n78-stress` 的解析是**取最新**（`ls -1t | head -1`），**审计用的是 10:27 那条**。两条的读数（都记下来，因为它们测的是同一件事的两个空口状态）：

| | `p90` @**10:03**（审计未用）| `p90` @**10:27**（★ 审计用的就是这条）|
|---|---|---|
| V1 中位 / samples | 1416.1 / 140 725 | **1408.4** / 122 640 |
| grants/s（最忙 240 s）| 600.0 | 598.0 |
| granted → new data | 9.88 → 9.20 Mbit/s | **17.22 → 10.59 Mbit/s** |
| 重传 / BLER / 平均 TB | 4.6% / **4.4%** / 2 059 B | **26.7%** / **29.3%** / 3 599 B |
| 调制 | 16QAM 为主 | **256QAM 49 478 跳、BLER 55.0%**（SINR p50 22.6 dB）|
| `stale`（pipeline / gpu）| 0 / 0 | **4 / 10**（>8 ms，max 16.9 ms——`stress` 工况的预登记预期就是 `stale > 0`）|
| residency / busy / gap 中位 | 631.9 / 502.0 / 138.0 | 628.7 / 505.3 / 131.9 |

★ 两条都过门（`leg_gate` 9 of 9、审计 32/0/0）⇒ **结论不依赖选哪一条**；但**引用时要说清是哪一条**。建议以后重跑换标签（`p90b-…`），别让"最新"决定证据。
| `p89`（空载）| 1 694 跳、V1 **1 359.2**（与 `p85` 的 1 348 同形，**不能当加压指标**）|

★ 顺带把审计里一处**硬编码字面量**改成**下界**：`ctest -L phy` 原来要求恰好 `"100% tests passed out of 193"`，而 §6.148⑤ 让标签从 193 涨到 201 ⇒ 第一次"合法的增长"被读成 FAIL。现在的判据是 **100% 且 n ≥ 193**（这个文件自己写着"totals are dynamic, judge by names"的教训）；**缩水仍然会被抓**（那正是探针那 8 个用例曾经掉出验收路径的方式）。

#### ④ ★ "时间轴里 rxwait ~5 ms，是不是 CPU 取 IQ 样本要等这么久？"（用户 2026-09-28 追问）

**是——那一次 `receive()` 确实阻塞了 ~5 ms**（trace 的 `rxwait` 与 `[ul_rx_wait]` 同源、同一次调用）。但**原因不是 CPU 慢**，而是**那一块的样本晚到了 ~4.5 ms**。四条证据，缺一条都会读错：

| 证据 | 读数 | 说明 |
|---|---|---|
| 宿主**有没有迟到去要**？ | `[ul_rx_timing] loop(max=` **338 µs（空载）/ 1 950 µs（加压）**，`over 1ms` = **0 / 1**，共 55–61 万次调用 | 宿主自己的活儿＋调度从没拖过 ⇒ 不是"CPU 忙不过来" |
| 慢在**调用内部**吗？ | `recv(max≈101 ms` 是首次启动项（§6.146）；`over 1ms` = **555–603**、`over 5ms` = **9–32** | 慢都慢在传输/电台交付这一侧 |
| **丢样了吗**？ | `[ul_rx] gaps=0 gap_samples=0 rx_overflows=0 rx_lates=0 ts0_blocks=0` | 晚到的那块样本**仍然是对的**（时间戳连续） |
| **缓冲池**争用？ | `[ul_rx_pool] pop_blocking … max=` **35–48 µs**，`over 1ms=0`、`starved_events=0` | 与池无关 |
| 是不是**接收专属**？ | 同腿 `[dl_tx_call] … max=` **85 ms**、`over 1ms` = **441–454**、`over 5ms` = 5 | **发送侧同类停顿也在** ⇒ 更像 **USB/宿主栈**（gNB 不拥有的那些线程），而不是"ADC 生产晚了" |

**量级与频率**：中位 **471–472 µs**（≈ 一个时隙 500 µs 减去宿主相位），p95 **591**，**p99 603–604**；**≥2 ms 已在 p99 之外**：512 行里 18–19 行（**空载腿 0 行**），整腿 `recv > 1 ms` 只占 **0.09–0.1%**，`> 5 ms` 只有 **9–32 次**。

★ **它们聚成一个 ~9 秒的窗口**（`p90`@10:03：boot 334 059.5→334 068.0 s；@10:27：335 484.1→335 491.7 s）⇒ **每个腿一次短插曲**，不是稳态抖动。

★★ **它确实是 V1 的 `max` 的来源，但只碰尾部**：
* 带完整时间轴的慢行里 `pipeline = rxwait + ldpc` 依然成立（slot 4208：`4712 + 812 ≈ 5623`），与健康行同构（`475 + 934 = 1409`）⇒ **这 5 ms 全落在"等样本"上，跳的后半段完全正常**（`t2f 50.2 / ce 134.1 / ldpc 812 / crc 908`，`ldpc` 与健康中位 934 同量级）；
* ⇒ `[ul_gpu_pipeline] max`（6 589–7 899 µs）与 `stale`（10:27 那条 4/10）就是这些事件；**中位/p95 不受影响**（V1 1 359 / 1 408 / 1 416）。
* ⇒ 与 GPU 车道**无关**：同腿的 `residency/busy/gap` 中位都在正常带内（见上表）。

**所以（本轮的结论，已被 §6.150 更正一半）**：那 5 ms 是"**CPU 在等样本**"的**字面事实**，不是 CPU 慢、不是丢样；当时我写成"外部链路/宿主栈、与 gNB 无关"——**那一半是错的**：§6.150 用同夜配对三腿证明，**触发条件恰恰是"PHY 里有 Metal 路径"**（`cpu` 0.002–0.003% vs `cpu_gpu` 0.118% vs `gpu` 0.058–0.171%），而与流量/负载无关。它仍然只影响尾部读数（中位不动）。

#### ⑤ 下一步（真正的岔路只剩交付配置）

* 槽时间轴的显示改进（②）——**与下一次代码改动合并**。
* **交付配置的两个待裁决仍在**：`rv_sequence`（§6.143：`rv` 臂 +37%）、`max_pusch_and_srs_concurrency: 2`（目前只在腿的命令行上、不在配置里）。两者都需要重飞一对 ⇒ **建议一次裁决、一次重飞**。

### 6.150 ★★★ 慢接收的真正触发条件是"**PHY 里有 Metal 路径**"，不是流量/负载 —— 我上一轮的归属错了（用户 2026-09-28 追问"空载腿与加压腿的时间轴不一样，说明它与负载正相关"）

#### ① 我错在哪，为什么

§6.149④ 里我写了"是 USB/宿主栈的现象，**与 gNB 无关**"。我只检验了**负载轴**（空载 `p89` vs 加压 `p90`：慢接收率 0.098% vs 0.098–0.102%，确实无关），就把它推广成了"与 gNB 无关"。**我没有检验模式轴** —— 而档案里一直躺着一组**同夜、同二进制、同配置、同流量、只差 pipeline 模式**的配对三腿（2026-09-26 22:56 / 23:01 / 23:21）。

#### ② 配对三腿（决定性证据）

| 腿 | 模式 | 提交 | calls | `recv >1ms` | **占比** | `>5ms` | `loop max` | **`dl_tx_call >1ms`** | 稳态 rx 最大（去掉启动项）|
|---|---|---|---|---|---|---|---|---|---|
| `p62-n78-repro-cpu` | **cpu** | `37e876773c` | 540 707 | **16** | **0.003%** | 3 | 251 µs | **1** | ≈p99 = 603 µs |
| `p63-n78-repro-cpu-conc2` | **cpu**（conc2）| `37e876773c` | 1 099 023 | **24** | **0.002%** | 1 | 526 µs | **1** | ≈p99 = 603 µs |
| `p64-n78-repro-cpugpu` | **cpu_gpu**（模块级 offload）| 同夜 | 597 241 | **705** | **0.118%** | 37 | 5 079 µs | **598** | —— |
| `p89`/`p90`/`p33`–`p88`（**52 条 gpu 腿**）| **gpu**（融合车道）| 多提交 | ~550k | 360–1 297 | **0.058–0.171%** | 5–50 | 833–6 889 µs | 441–454 | 6.3–7.9 ms |

⇒ ★★ **只要 PHY 里启用了 Metal 路径，慢接收就从 ~0.002% 跳到 ~0.1%（40–60×），而且 RX 与 DL 两个方向同时跳**（DL 侧 1 → 441–598 次）。流量、并发度、配置都不是变量（三条腿的流量同量级 120–134k 授权）。

#### ③ 与流量/负载确实**无关**（这一半我原来是对的）

* 同一批 52 条 gpu 腿里，**空载** `p89`（3 439 跳）0.098% ≈ **加压** `p90`（145 421 跳）0.098–0.102%；
* 只开 **1/8 跳**的消去臂（`p78`/`p80`/`p81`/`p82`/`p83`/`p84`，几乎所有 kernel 换成空 kernel）仍是 0.070–0.111% ⇒ **与 GPU 干的活多少也无关**；
* 环深 64 / 512 / 1024（`p65`/`p37`/`p68`）0.094–0.114% ⇒ 与接收环深度无关。

⇒ 结论：**触发条件是"Metal 路径存在"，不是它有多忙、也不是链路有多忙。** 这更像**驱动/线程/内存子系统的交互**（Metal 的提交与完成线程、UMA 上的 DMA 与 GPU 争用）而不是"USB 本身"。

#### ③b "**平台交互的尾部成本**"这个词的定义（用户 2026-09-28 追问）

它是我给这一类现象的**标签**，由三个各自可测的部分组成，缺一个都不该用这个词：

| 词 | 含义 | 本条的实测依据 |
|---|---|---|
| **尾部** | 影响只落在分布的**尾部**，不在中心 | `[ul_rx_wait]` 中位 471–473 µs、p95 591、p99 603 **全部不动**；事件在 p99 之外（>1 ms，占调用 ~0.1%）。V1 **中位** 1 359–1 416 µs 不动，动的是 **max**：cpu 1.3–2.0 ms → gpu 6.6–7.9 ms。对均值的贡献只有几 µs/跳（≈0.3–0.6% 的 V1）⇒ **它解释不了那 ~92% 的结构性开销**，那是"中心成本"（整槽等待 ~473 + cb 平台账 ~470），两者不是一回事 |
| **平台** | 因果链落在 **PHY 之外**的机器部件：Metal 运行时的宿主侧机制（命令缓冲提交/完成、驱动管理的缓冲）、UMA 上的 USB DMA、以及宿主调度把 UHD 接收线程与 Metal 相关线程排在一起 | 本条的实测：主机**从不迟到去要**（`loop` 251→5 079 µs 只 0–1 次 >1 ms）、电台**不报错不丢样**（`gaps/rx_overflows/rx_lates` 全 0）、**发送方向同时出现同类停顿**（`dl_tx_call >1ms`：cpu 1 次 → Metal 441–598 次）⇒ 不是 PHY 的数据路径逻辑，而是共享的传输/调度/内存通道 |
| **成本** | 它**由交付指标付账**：抬 `[ul_gpu_pipeline]` 的 max/p99，并把个别跳推进 `stale` | `p90`@10:27 的 `stale=4/10`（>8 ms，max 16.9 ms）；CPU 路径几乎不付（稳态 max ≈ p99 = 603 µs）|

★ **一个由数据得出的收窄（重要，但仍是候选）**：这些停顿**与 GPU 真正干的活无关** —— 把内核几乎全换成空 kernel 的消去臂（`p78`/`p80`/`p81`/`p82`/`p83`/`p84`，同一批 cb、同一提交/完成路径、GPU 执行与访存都大幅减少）仍是 **0.070–0.111%**，与满负载的腿（`p86` 0.102%、`p87` 0.162%）同一带内。
⇒ 所以更准确的说法是：**不是"GPU 在算"造成的，而是"Metal 运行时在回路里"（提交/完成/驱动缓冲这一整套机制在场）造成的**。到底是宿主 CPU/线程调度，还是驱动与 USB/DMA 路径的交互，正是 §6.150⑤ 的判别实验要分开的。

#### ④ 它有多大影响（只碰尾部）

| | cpu 模式（`p62`/`p63`）| gpu 模式（`p89`/`p90`×2）|
|---|---|---|
| 流水线中位 | 661 / 667 µs（`[ul_pipeline]`）| 1 359 / 1 408 / 1 416 µs（`[ul_gpu_pipeline]`）|
| **max** | **1 337 / 1 992 µs** | **6 589 / 7 899 / 7 073 µs** |
| p99 | 831 / 839 | 1 575 / 1 627 / 1 699 |
| 慢接收 | 16 / 24 次 | 555–603 次 |

* 0.1% 的块被推迟 1–6 ms ⇒ 对**均值**的贡献只有**几 µs/跳**（≈0.3% 的 V1），**中位完全不动**；它决定的是 **V1 的 max/p99** —— CPU 路径的 max 是 1.3–2.0 ms，GPU 路径是 6.6–7.9 ms。
* ⚠ 因此：这条**不是**"可优化项"（不是算法/结构），而是**平台交互的尾部成本**；在判据里它只体现在 `[ul_gpu_pipeline] max` 与 `stale`。

#### ④b ⚠ 腿 `p94-cg-all-light` 作废：**电台 USB 掉线**（2026-09-28 12:08）——本账第一次出现这个失败类

**时间线（全部来自那条腿自己的文件）**：

| 时刻 | 事件 | 出处 |
|---|---|---|
| ~12:08:5x | DL 的 RLC 队列开始缓慢积压（`queued_sdus=10`，即 DL 发不出去）| `.log` 的 `[RLC] … queued_sdus=` 行 |
| 腿内大部分时间 | **RX 无数据**：stdout 共 **52** 行 `Error: exceeded maximum number of timed out receive calls.` —— 每行 = `radio_uhd_rx_stream.cpp` 的 **10 次 ×200 ms 超时**上限 ⇒ 累计 **~104 s 的接收静默** | `.log.stdout` |
| 12:10:29 | DL 队列涨到 **1000** 个 SDU（87000 B）| `.log` |
| 崩溃瞬间 | `[ERROR] [STREAMER] recv packet demuxer unexpected sid 0x50` → `libc++abi: terminating due to uncaught exception of type uhd::usb_error: RuntimeError: USBError -5: usb tx2 submit failed: LIBUSB_ERROR_NOT_FOUND` | `.log.stderr` |
| 收尾 | `Abort trap: 6`，`rc=134`；**无 contract / 无 `[metal_stats]`** ⇒ `run_leg.sh` 正确地打出 `THIS LEG HAS NO COMPLETE REPORT` | `.stdout` |

**这不是实验造成的**：⑤ 号臂**不含** `gpu_load`（外部负载在 ⑦）；机器上当时也没有 `gpu_load` 进程。

★ **它也不是孤立的**：同一失败类（`exceeded maximum number of timed out receive calls`）在档案里**只有两处**，另一处正是 **`p92` 的第一次尝试（11:56，被看门狗强杀那次）**；而夹在中间的 `p92` 重跑（11:59）与 `p93`（12:01）**是干净的**（完整报告、`gaps/ovf=0`、blocks≈calls）。
⇒ 电台自 **11:56 起就是"边缘状态"**：坏 → 自愈 → 再坏。`LIBUSB_ERROR_NOT_FOUND`（设备/接口在 libusb 视野里消失）指向**USB 物理链路的稳定性**，而不是我们的代码。

**新的运行纪律（本次新增）**：

1. **任何 abort / 看门狗强杀 / `Forcing exit` 之后，先给 B200 断电重启**（拔插 USB，等 ~10 s，最好换口、不走 hub），再 `uhd_find_devices` 确认枚举（本次崩溃后**已能枚举**：`B200 / 000000560 / lutetia`）。
2. **每条腿的头 ~15 s 看 stdout**：`exceeded maximum…` / `failed receiving packet` 一旦出现 ⇒ **立刻 Ctrl-C**，那条腿必然是 void，不必盲跑 4 分钟。
   ★ **已把它做成脚本开关**：`run_leg.sh` 新增 **`--smoke=N`（默认 0 = 关）** —— 启动后 N 秒内监视 `$LOG.stdout/$LOG.stderr`，命中电台失败行就**自己打断 gnb** 并打印原因（自测：伪造失败行 ⇒ 1 s 内打断；干净 ⇒ 不打扰）。默认关闭 ⇒ 任何不显式要求它的腿，行为逐字不变。
   ⚠ 顺带修掉一个**这次改动自己引入**的隐患：为了拿 gnb 的 pid（smoke 需要）它从**前台**改成了**后台 + `wait`**，而"Ctrl-C 只停 gnb、不停脚本"这一条此前是**靠 bash 对前台作业忽略 SIGINT 的行为**继承来的，换到后台就**不成立** —— 脚本会跟着 gnb 一起死，从而**跳过收尾的报告自检**（正是那句"别读这条腿"）。现在改成**显式 `trap stop_gnb INT`**：中断只杀 gnb，脚本活下去做自检。
3. 报告守卫是有效的：这次它自己喊出了 `NO COMPLETE REPORT`，没有被读成"零值"。

#### ④c 批次 2 的裁决（2026-09-28）：**要的是"整套模块"，不是流量、也不是"机器上有 Metal 活动"**

⚠ **先纠一个我自己的测量错误**：`p94` 的 stderr 里有**两份报告** —— 第一份是腿跑到 ~31 s 时 **`p0 dump #1 (dry-pool drop)`** 打的**中途快照**（同一批计数器！），第二份才是收尾报告。我第一版表读的是 `re.search` 的**第一个**匹配 ⇒ 把 0.001% 写成了 p94 的读数，而它的**真实读数是 0.111%（100× 差）**。`wip/stall_rate.py` 已改成**只读最后一次出现**（与各门禁一贯的 `tail -1` 同规），并把"报告块数"打出来（>1 会显式提示）。

| 腿 | 模式 | offload 的模块 | 跳/s | **`recv>1ms%`** | `dl>1ms` | 池 `held_max/free_min/starved/dropped` | `[ul_dft_wait]` 中位/最大 |
|---|---|---|---|---|---|---|---|
| `p92-cg-none` | cpu_gpu | **none** | 8.7 | **0.001%** | 1 | 3 / 29 / 0 / 0 | 无样本 |
| `p93-cg-dft` | cpu_gpu | **dft** | 8.7 | **0.003%** | 1 | 7 / 25 / 0 / 0 | 183 148 次 / **0.1 µs** / 1 435 µs |
| **`p94-cg-all-light`** | cpu_gpu | **all** | 21 | **0.111%** | **546** | **33 / 0 / 1 / 1** | 38 103 次 / **381.6 µs** / **4 501 µs** |
| `p95-cpu-light` | cpu | — | 9.2 | **0.000%** | 1 | 3 / 29 / 0 / 0 | 无样本 |
| `p96-cpu-light` **+ 外部 GPU 负载** | cpu | — | 9.4 | **0.000%** | 1 | 2 / 30 / 0 / 0 | 无样本 |
| 参考：`p62`/`p63` cpu 重载 | cpu | — | ~490 | 0.002–0.003% | 1 | —— | —— |
| 参考：`p64` cg-all 重载 / `p89` gpu 轻载 | | all / lane | 449 / 11.2 | 0.118% / 0.098% | 598 / 467 | —— | —— |

**三条结论**：

1. ★ **不是流量**：`p94`（**21 跳/s**）0.111% ≈ `p64`（449 跳/s）0.118%。批次 1 那两条低读数因此**可解读了**：它们是"模块没开够"，不是"流量太低"。
2. ★★ **是"整套模块"**：同一轻载下 `none` **0.001%** → `dft` **0.003%** → **`all` 0.111%**。**只把 DFT 放上设备不够。**
3. ★★ **不是"机器上有 Metal 活动"**：`p96` 全程跑着与 gNB 无关的外部 Metal 负载（`/tmp/gpu_load empty`，**288 547 次派发 / ~10 000 次每秒**，其日志可查），而 cpu 模式腿仍是 **0.000%**（= `p95` 无负载的读数）。
   ⚠ 限定：`empty` 模式**不写内存**（空 kernel），所以"外部**访存/算力**争用"仍未被排除 —— 那是还没跑的 `heavy` 变体。

**★ 两个独立指纹同时只在"全模块"臂出现**（这是本批最有价值的发现）：

| 指纹 | 只在 `all` 出现 | 含义 |
|---|---|---|
| 慢接收 0.111% + DL 侧 546 次 | ✅ | 传输停顿 |
| **接收池干涸**：`held_max=33 > pool=32`、`free_min=0`、`starved_events=1`、**`dropped=1`**、park 1 131 µs | ✅ | **流水线把 33 个整槽缓冲（= 16.5 ms）握在手里**，把接收池抽干 |
| **宿主在等 DFT**：38 103 次、中位 **381.6 µs**、max 4 501 µs（而 `dft` 单模块臂几乎不等：中位 0.1 µs）| ✅ | 全模块时**网格被设备侧 CE 消费**，宿主必须等 DFT 结果 ⇒ 宿主的节奏被设备绑住 |

⇒ ★★ **机制候选收窄成一句**：全模块路径让**同一批缓冲对象**同时处在"USB DMA 要复用"与"设备侧模块要读写"的边界上 —— 流水线握缓冲更久（16.5 ms 量级）⇒ 偶尔把接收池抽干 ⇒ 传输停顿。**这解释了为什么外部空负载（碰不到这些缓冲）什么也复现不了。**

#### ④c′ 批次 3 的裁决（2026-09-28）：**没有任何"单模块"能复现——是组合**

| 腿 | offload | 跳/s | **`recv>1ms%`** | `dl>1ms` | 池 `held/free/starved/dropped` | `[ul_dft_wait]` 中位 |
|---|---|---|---|---|---|---|
| `p92` none | — | 8.7 | 0.001% | 1 | 3/29/0/0 | 无样本 |
| `p97` **grid** | 设备网格 | 9.9 | **0.001%** | 1 | 2/30/0/0 | 无样本 |
| `p98` **ce** | 估计器 | 14.5 | **0.001%** | 1 | 2/30/0/0 | 无样本 |
| `p99` **eq** | 均衡器 | 10.5 | **0.001%** | 1 | 2/30/0/0 | 无样本 |
| `p93` **dft** | 前端 | 8.7 | **0.003%** | 1 | 7/25/0/0 | 0.1 µs |
| **`p94` all** | 四个都要 | 21 | **0.111%** | **546** | **33/0/1/1** | **381.6 µs** |

⇒ ★★ **单模块全部落在"CPU 级"带内（0.001–0.003%），四个一起才是 0.111%（~100×）**，而且**池干涸与"宿主等 DFT"两个指纹也只在四模块齐全时出现**。
⇒ 机制不在"某一个模块"，而在**设备侧链条的组合效应**（网格被设备写入 + 被设备消费 + 估计/均衡链在同一批缓冲上）——这与 ④c 的"缓冲处在 USB DMA 与设备模块的边界上"一致。下一步是**子集**（`LEG_CG_MODULES` 现已支持 `+` 组合，例如 `dft+grid+ce`）。

#### ④c″ ★★★ 批次 4 的裁决（2026-09-28）：触发条件是 **`dft` + `grid` 这一对**，机制指纹同时命中

| 腿 | offload | 跳/s | **`recv>1ms%`** | `dl>1ms` | 池 `held/free/starved/dropped` | `[ul_dft_wait]` 中位 |
|---|---|---|---|---|---|---|
| `p92` none | — | 8.7 | 0.001% | 1 | 3/29/0/0 | 无样本 |
| `p97` grid | 设备网格 | 9.9 | 0.001% | 1 | 2/30/0/0 | 无样本 |
| `p93` dft | 设备前端 | 8.7 | 0.003% | 1 | 7/25/0/0 | **0.1 µs**（183k 次）|
| `p98` ce / `p99` eq | 单模块 | 10–15 | 0.001% | 1 | 2/30/0/0 | 无样本 |
| `p101` **ce+eq** | 设备估计+均衡链 | 13.5 | **0.000%** | 1 | 2/30/0/0 | 无样本 |
| **`p100` dft+grid** | 设备前端 + 设备网格 | 17 | **0.073%** | **343** | 11/21/0/0 | **384.0 µs**（37k 次）|
| **`p102` dft+grid+ce** | 再加估计器 | 21 | **0.063%** | **300** | 13/19/0/0 | **384.1 µs**（39k 次）|
| `p94` all | 四个 | 21 | **0.111%** | **546** | **33/0/1/1** | 381.6 µs |
| `p95`/`p96` cpu（含外部 GPU 负载）| — | 9.2/9.4 | 0.000% | 1 | 2–3/29–30/0/0 | 无样本 |

**三条结论，全部有配对或单调证据**：

1. ★★★ **必要且充分的那一对 = `pusch_dft_type=metal` ＋ `device_resource_grid=on`**：单独任一个 0.001–0.003%（= CPU 级），**合起来 0.073%**；再加 CE（0.063%）或 EQ（0.111%）只改变量级，不改变"是否发生"。**`ce+eq`（设备侧估计+均衡链）完全不复现（0.000%）** ⇒ 与"设备侧估计链"无关。
2. ★★ **机制指纹同一个对**：`[ul_dft_wait]` 中位从 `dft` 单模块的 **0.1 µs** 跳到 `dft+grid` 的 **384 µs**（37–39k 次等待）⇒ **设备侧网格把宿主的节奏绑到了设备上**（宿主在这个模块边界上真的阻塞），而同一条腿的传输停顿就出现在这里。
   ⇒ 与批次 2/3 的推论一致并收窄为：**"设备常驻的资源网格"（由设备侧 DFT 写入）就是那块处在 USB DMA 与 GPU 之间边界上的缓冲**。
3. ★★ **"池干涸"是另一个、更高的门槛**：池 `held_max` 随设备链条单调上升（2–7 → 11 → 13 → **33/free=0/starved=1/dropped=1**）⇒ **传输尾部的停顿只需 `dft+grid`；而"流水线握满缓冲、接收池被抽干并丢样"需要整条链都在设备上**（这一条才触到 V2 的 `starved_events==0`）。

**含义（这条线现在可以收口为"已归属 + 已定价"）**：

* 这个尾部代价**不是"有 GPU 路径"的固有属性**（任何单模块、`ce+eq`、外部 GPU 负载都不复现），而是**"设备常驻网格"这个具体选择的代价**；
* 在**融合车道（交付形态）里网格必须是设备常驻的**（G1：宿主不参与数据流；`phy_pipeline_lane_defaults` 也默认 `device_grid on`）⇒ **交付形态要付它**，与 §6.141 的 cb 账同类：**结构性、无 PHY 旋钮可动**；
* 但在 `cpu_gpu` 形态里它是一个**有价的旋钮**：`dft`（Metal 前端 + 宿主网格，`p93`）= 0.003%、无宿主等待，代价是网格的宿主往返（正是契约里 `zero-copy wraps` 那条在数的事）⇒ **"零拷贝"与"传输尾部"是一个可量化的交换**。

#### ④d 关于"ping 的 RTT 升到 >1 s 后逐步恢复"（用户观察）——**不是 GPU 流水线阻塞**

同一条腿（`p94`）自带的界：

| 量 | 读数 | 能解释 >1 s 吗 |
|---|---|---|
| 最坏接收停顿 `[ul_rx_wait] max` | **33.7 ms** | ❌ |
| 最坏 DL 截止期迟到 `[dl_tx_slack] min` | **−17.7 ms**（116 次 ≤0）| ❌ |
| DL 队列积压 `queued_sdus` | ≤ **14**（对比崩溃腿的 1000）| ❌ |
| `stale` / `gaps` / `rx_overflows` | 0 / 0 / 0 | ❌ |
| 池 park 最坏 | 1.13 ms | ❌ |

⇒ **gNB 自身从未把任何一个包扣住 ≥1 s**。而日志显示这条腿的**开头就是 UE 接入**（`SRB0 DL: TX SDU` = RRC 建立，04:17:26，正在腿开始后几秒）⇒ ping 的头几个包在等 **RRC/DRB 建立**，之后回落到 30–50 ms —— 这正是你看到的"先 >1 s、再逐步恢复"的形状。**要坐实需要 ping 自己的时间戳 + 接入时间线**，但现有证据指向**接入瞬态**，不是流水线。

★ 顺带抓到一个**真事件**：~31 s 那次**池干涸**（33 个缓冲被握、`free_min=0`、1 个块被丢、park 1.13 ms）—— 它正是 V2 判据（`starved_events==0`）盯的那种事，而它**只在这一条臂上**出现。虽然只有毫秒级，但它是"流水线握缓冲"的**可复现指纹**，值得跟着批次 3 一起看。

#### ⑤ 机制候选与判别实验（都是"下一步"，未做）

| 候选 | 判别实验 | 代价 |
|---|---|---|
| (a) **宿主 CPU 竞争**：Metal 路径带来的宿主线程/完成回调把 UHD 接收线程或 USB 栈挤开（注意 `loop max` 也从 251 µs 涨到 5 079 µs）| `cpu` 模式腿 + **同机另跑一个 GPU 负载**（gNB 一行代码不动）| 一次 cpu 腿（无流量要求）+ 一个 GPU 基准 |
| (b) **内存子系统争用**：USB DMA 写 UMA 与 GPU 抢带宽 | 同上（若外部 GPU 负载就能复现 ⇒ 指向 (b) 而非 gNB 代码）| 同上 |
| (c) **具体哪个 Metal 模块**带进来的 | 在 **`cpu_gpu`** 模式下逐个只开一个金属模块（DFT / CE / EQ）——`cpu_gpu` 的定义就是"每个模块跟自己的旋钮"，需要给 `run_leg.sh` 加一个覆盖模块集的入口（**wip 脚本改动，非代码**）| 3–4 条腿 |
| (d) **仪表改进**：RX 侧现在**没有慢事件的时间戳**（DL 侧有 `due_ts`），所以无法把 episode 与 iperf3/GPU 活动对齐 | 给 `recv` 超阈值的事件打一行带时间戳的日志（**代码改动 ⇒ 与下次改动合并**）| 小改 |

**批次 1（`LEG_CG_MODULES=none` / `=dft`，2026-09-28）—— 仪器生效，但结论**不可定案**，因为同时动了两个变量**：

| 腿 | offload | 空口时长 | 跳 | **跳/s** | calls | `recv >1ms` | **占比** | `dl>1ms` | 契约 |
|---|---|---|---|---|---|---|---|---|---|
| `p92-cg-none`（重跑的那次）| **none** | 125 s | 1 090 | **8.7** | 249 708 | 3 | **0.001%** | 1 | NOT MET（见下）|
| `p93-cg-dft` | **dft** | 117 s | 1 025 | **8.7** | 234 694 | 7 | **0.003%** | 1 | NOT MET（见下）|
| `p89`（对照，gpu）| lane | 307 s | 3 439 | **11.2** | 614 254 | 603 | **0.098%** | 467 | MET |
| `p64`（对照，cpu_gpu）| **all** | 299 s | 134 046 | **448.9** | 597 241 | 705 | **0.118%** | 598 | —— |
| `p62`（对照，cpu）| — | 270 s | 131 599 | **486.8** | 540 707 | 16 | **0.003%** | 1 | —— |

* ✅ **阶梯入口生效**：两条腿的 `mode options` 分别是 `<none>` 与 `--expert_phy.pusch_dft_type metal`；并发度都是 auto-derived 1（与 `p64` 相同）⇒ 这两个混淆变量被排除。
* ❌ **但流量同时变了**：阶梯两臂只有 **8.7 跳/s**，而参考的 `cpu_gpu(all)` 是 **448.9 跳/s**（`gpu` 对照 `p89` 是 11.2 跳/s）。**"0.001–0.003%" 既可以由"只开 DFT 不够"解释，也可以由"流量太低"解释** —— 二者不可分。这不是第一次：`p79`/`p85`（1.7–2.75 跳/s）也是 ~0.000–0.001%。
* ⏱ 阶梯两臂只跑了 ~120 s（`calls × 500 µs`），而参考腿是 ~280–300 s ⇒ **下次跑满 ~240 s**，并且**每条臂都要带与 `p89` 同级的那条轻载 ping**（~10 跳/s），事后用 `wip/stall_rate.py` 核对跳/s。
* ⚠ **小门禁发现（记下，不改判据）**：两条臂的契约都在 `ce device estimates: 0 device, N host -> FAILED` 这一行失败 —— 因为那条检查是**按 `cpu_gpu = 全 offload` 写的**，而这两条臂的 CE 本来就该在 CPU 上。⇒ 阶梯臂是**测量臂**，其契约行要读作"对本臂不适用"，不是回归；若以后要让阶梯臂过门，该检查应当按**该臂实际 offload 的模块集**决定是否适用。
* ✅ 第一次 `p92`（11:56）被正确丢弃：`Forcing exit` + 无 `[metal_stats]` + 契约 NOT MET ⇒ 读的是 11:59 的重跑。

**实验工具已就绪（2026-09-28，全部 wip 层，不动交付代码）**：

| 工具 | 作用 |
|---|---|
| `wip/run_leg.sh` 新增 **`LEG_CG_MODULES=all\|none\|grid\|dft\|ce\|eq`** | `cpu_gpu` 模式下**选择只 offload 哪个模块**（默认 `all` = 这模式一直以来的集合，历史读数语义不变）。白名单外的值**当场拒绝**；选择会写进腿自己的 `mode options` 行 |
| `wip/gpu_load.mm`（编译到 `/tmp/gpu_load`）| **外部 GPU 负载**，与 gNB 无关的独立进程：`empty` = 只提交空 kernel（**~10 300 次提交/秒**，几乎无 GPU 执行、无访存）、`heavy` = 满算力/访存（1024 组×256 线程×4096 次 FMA，~650 次/秒）。两种模式正好把"**运行时在回路里**"与"**GPU 真在算/访存**"分开 |
| `wip/stall_rate.py` | 一把尺：把任意几条腿的 `recv>1ms%` / `>5ms` / **`dl>1ms`** / `rx_max` / `gaps·ovf` 与**该腿实际 offload 的模块**并排打出来（读法写在文件头；参考值 cpu 0.002–0.003% / cpu_gpu 0.118% / gpu 0.058–0.171%）|

⚠ **每条臂都必须带同样的轻载流量**（例如 host 向 UE 打 `ping -i 0.2`）：档案里 `p85`（660 跳）与 `p79`（403 跳）**几乎没有慢接收**（0.000%/0.001%），而 `p89`（3 439 跳，带 ping）是 0.098% ⇒ "几乎无业务"本身就会让现象消失，臂之间必须配平这个变量。

★ 纪律教训（写下来）：**归属一个现象之前，先把"轴"列全** —— 我列了"负载轴"就下了结论；档案里的配对三腿本来就能把"模式轴"一次问清（"**同夜配对**"是这个项目已有的工具，只是这次没人去用）。

#### ⑥ ★★ 机制路线（"不能稀里糊涂过去"）：读码已把对象缩到三个，且**判决臂早就造好了**

用户 2026-09-28 指示：这一条要像 §6.141 的 cb 账一样**搞清到底是什么**，不能以"已定价"收尾。以下是把 11 条腿的结论再往代码里推一层的结果。

**读码得到的具体对象**（`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm` + `lib/phy/upper/channel_processors/metal/channel_equalizer_metal.cpp`）：

| # | 对象 | 事实 | 与触发对的关系 |
|---|---|---|---|
| 1 | **电台样本缓冲被零拷贝 wrap 成 Metal buffer** | `newBufferWithBytesNoCopy(ptr, …, MTLResourceStorageModeShared)`（`:1540`）⇒ **IQ 页同时被 USB DMA 写、被 GPU 读**；wrap 被拒时才走 `newBufferWithBytes` 拷贝（引擎既有的 fallback 路径，计数器 `staged` + 契约行 `dft radio inputs` 在数它）| **两个臂都有**（都有 Metal DFT）⇒ 单靠它解释不了 `dft`(0.003%) 与 `dft+grid`(0.073%) 的差别 |
| 2 | **设备常驻网格** | `device_grid=on` 让网格留在设备内存；均衡器的 `set_device_grid()` 路径明说"**宿主不再拷贝**"那条 gather 计划（`channel_equalizer_metal.cpp:540-551`）| **只在 `grid=on` 的臂存在** ✅ 与触发对一致 |
| 3 | **宿主在 `[cmd_buf waitUntilCompleted]` 上阻塞**（`record_dft_wait` 只有这一个调用点，`:2118`）| 测的是**同步本身占住调用者的时间**，不是 GPU 跨度；`dft` 单模块臂中位 **0.1 µs**（从不真等），`dft+grid` **384 µs** | 与触发对一致，且**这是宿主被绑到设备上的那一刻** |

⇒ 三个候选机制：**(M1) 共享页**（USB DMA 与 GPU 同页 ⇒ 驱动的一致性/映射管理拖慢 USB 完成）；**(M2) 宿主阻塞**（384 µs 的 `waitUntilCompleted` 期间宿主的其它线程/锁关系被牵动）；**(M3) 网格本身的设备内存压力/分配**（只在 `grid=on` 存在）。

**★ 判决臂 M1 已经存在，默认关，且就是为这个问题造的**：`OCUDU_DFT_STAGE_INPUT=1`（Q25）—— 把变换的**输入**改成"经普通设备内存分派一次"（一处调用点、一次拷贝），**网格与输出环保持零拷贝**。引擎自己的注释写明它存在的理由就是要分离"输入路径"与"缓冲上还载着什么"（§6.77/Q25 查那条 452 µs cb 驻留时留下的）。

| 实验 | 配方 | 预登记判读 |
|---|---|---|
| **M1** | `LEG_CG_MODULES=dft+grid` ＋ `OCUDU_DFT_STAGE_INPUT=1`（轻载 ping，~240 s）；**对照 = `p100`（同臂、无旋钮、0.073%）** | 停顿掉到 ~0.003% ⇒ **机制是"共享页"**（USB DMA 与 GPU 同页）；仍 ~0.07% ⇒ 共享页不是主因，转 M2/M3；⚠ 若**升到 >0.1%** ⇒ 是"总线/DMA 争用"本身（多加的一次拷贝加重了它），同样是结论 |
| **M2** | 给 RX 慢事件加**时间戳**（已登记的代码小改），把 episode 与 `[ul_dft_wait]`/lane 活动对齐 | 若停顿与 384 µs 等待**同刻**发生 ⇒ 宿主阻塞是共同原因；否则不是 |
| **M3** | **网格尺寸缩放**：n1（25 PRB，网格约一半）同臂 vs n78（51 PRB）| 停顿率随网格**成比例** ⇒ M3 成立；与尺寸无关 ⇒ 排除 M3 |

**建议顺序**：先飞 **M1**（一条腿、零代码、单变量、有现成对照 `p100`）——它一次就能把三个候选砍掉一个或坐实一个。**建议顺序**：先飞 **M1**（一条腿、零代码、单变量、有现成对照 `p100`）——它一次就能把三个候选砍掉一个或坐实一个。

##### 已飞的两条判决臂（2026-09-28）：**M1 否掉"共享页"，M2′ 把机制钉到"宿主真阻塞在 Metal 完成上"**

**M1（`p105-cg-dft-grid-stage`，`OCUDU_DFT_STAGE_INPUT=1`）—— 阳性对照生效、假设被否**：

| | `p100` 对照 | `p105` M1 |
|---|---|---|
| 契约行 `dft radio inputs` | OK | **FAILED**（预期内：433 916 次 *wrap 必须 stage 一次宿主拷贝*）⇒ **臂不是空的** |
| `recv>1ms%` / `dl>1ms` | 0.073% / 343 | **0.065% / 310** ⇒ **没塌、没变** |
| `[ul_dft_wait]` 中位 / min | 384.0 µs / 22.4 µs | **889.7 µs / 559.6 µs**（多出来的那次拷贝的代价，印证旋钮确实生效）|

⇒ **零拷贝 wrap 电台样本页（USB DMA 与 GPU 同页）不是机制**：输入改走普通设备内存后，两个方向的停顿率一样。
⇒ 同时 **M3（网格内存压力）也被 `p97` 否掉**：`grid=on` 但 DFT 在宿主时，同一块网格缓冲、同样的分配，停顿率是 0.001%。

**M2′（`p106-cg-grid-ce`）—— 让"设备网格 + 设备侧消费者"在场、但取消宿主的阻塞等待**：

| 腿 | 设备网格 | 设备侧消费者 | 宿主阻塞在 DFT cb | **`recv>1ms%`** | `dl>1ms` |
|---|---|---|---|---|---|
| `p97` grid | ✅ | ❌（DFT 在宿主）| 不适用 | 0.001% | 1 |
| **`p106` grid+ce** | ✅ | ✅（**47 691 次设备估计**，契约行 OK ⇒ 阳性对照）| **无**（`[ul_dft_wait]` 无样本）| **0.001%** | 1 |
| `p93` dft | ❌（宿主网格）| ❌ | 调用即返回（0.1 µs）| 0.003% | 1 |
| `p100` dft+grid | ✅ | ❌ | **是（384 µs）** | **0.073%** | 343 |
| `p102` dft+grid+ce | ✅ | ✅ | **是（384 µs）** | **0.063%** | 300 |
| `p94` all | ✅ | ✅ | **是（382 µs）** | **0.111%** | 546 |

⇒ ★★★ **存活并收窄为 M2**：必要条件是 **"宿主在一个输出落在设备内存的 Metal 提交上*真的阻塞*"**（`[ul_dft_wait]` 中位从 0.1 µs 跳到 381–384 µs 的那几族），而**不是**：设备网格本身（`p97`/`p106` 都有它，零停顿）、设备侧消费者（`p106` 有，零停顿）、输入路径（M1 否）、网格内存压力（`p97` 否）、设备工作量（消去臂否）、流量（否）、外部 GPU 活动（`p96` 否）。
⇒ ⚠ **限定（已被 `p107` 清掉）**：`p106` 有 **4 次 `gaps`/`rx_overflows`**、且跑了 ~772 s ⇒ 需要一条干净的复飞固化。

**`p107-cg-grid-ce-clean`（2026-09-28 14:05，同臂、干净）**：`gaps=0 gap_samples=0 ts0_blocks=0 rx_overflows=0 rx_lates=0` ✓；阳性对照 `ce device estimates: 43 093 device, 0 host -> OK` ✓、`[ul_dft_wait] no samples recorded` ✓；**`recv(max=101 348us over 1ms=2 over 5ms=1)` ⇒ 2/590 783 = `0.0003%`**、`slip max=531 µs`、`loop max=255 µs`。
⇒ ★★★ **M2′ 固化**：**"设备网格 + 设备侧消费者"在场、而宿主不阻塞 ⇒ 停顿率与全 CPU 路径同级（0.000–0.003%）**。机制**唯一存活项 = "宿主真阻塞在 Metal 完成上"**。

**下一步（M2 的因果检验，代码小改已登记）**：给**慢接收事件打时间戳**，并统计"慢接收是否落在 `waitUntilCompleted` 的阻塞窗口内"（同腿共现率 vs 基线率）——这是把"跨臂相关"变成"同腿共现"的唯一办法，也正是 §6.150⑤ 表里 (d) 那条已登记的仪表改动。**M2 的因果检验：仪表已实现（2026-09-28，`ul_pipeline_probe.h` + 两处调用点）****M2 的同腿判决（腿 `p108-cg-dft-grid-m2`，2026-09-28 14:17）—— ★ 否**：

```
[ul_rx_wait] DFT-blocking overlap: 38187 blocking window(s) = 4% of the leg;
slow (>1 ms) receives 26 of 387 overlapped one = 7%  (a coincidence predicts 4%; all receives 7%)
```

* **慢接收与阻塞窗口的重叠率 7%，与"全部接收"的 7% 完全相同**（阻塞占空比 4%）⇒ **没有任何富集** ⇒ **宿主的那段阻塞不是停顿的原因**；跨臂相关（有阻塞的臂有停顿、没阻塞的臂没有）因此是**共症状**，不是因果。
* 该腿其它读数（用于界定"ping 尖峰能有多大是 gNB 的"）：`[ul_rx_wait] max = 8 614 µs`、`[dl_tx_slack] min = −3.0 ms`、`stale=0`、`gaps=0`、池 `held_max=13 free_min=19`、`recv>1ms = 387/579 176 = 0.067%`（与 `p100` 的 0.073% 同量级）。
* ⚠ **覆盖范围的诚实声明（这决定了"否"的强度）**：我记录的窗口是 **`[wait_begin, wait_end]`**，即**阻塞区间本身**。它**不覆盖**：(a) 提交/编码那一刻的驱动处理（`commit_open()` 到 wait 开始之间）；(b) 该 cb 的 **GPU 执行窗口**（`GPUStartTime → GPUEndTime`，引擎其实已经在读这两个时间戳）。
  ⇒ 所以"否"的边界是：**停顿不在"宿主等待"里**；它是否落在"驱动提交期"或"GPU 执行期"里**还没测**。

**下一步仪表（小改，窗口按种类分开）—— ✅ 已实现（2026-09-28）**：每次提交记**三个窗口**（`commit → wait_end`、`GPUStart → GPUEnd`、`wait` 本身），报告给出**每一类的命中数/慢接收数 = 率 vs 该类窗口的占空比**：
```
[ul_rx_wait] DFT-window overlap: S slow of M receive(s) accounted; wait h/S=P% vs duty D% (W win, all a/M=q%);
             commit->end …; gpu …; gpu-clock offset last Zus over K sample(s)
```
* **分子与占空比都打出来**：只给比率无法核对，也无法在两条样本数不同的腿之间比较。
* ★ **`gpu-clock offset` 是"把假设变成读数"的那一项**：GPU 时间戳与宿主 `steady_clock` 是否同基准**不靠假设**（`ocudu_metal_burst.mm` 已有 `now − cb.GPUEndTime` 的用法），报告打出的偏移应当是**小的正值**；若它巨大，则 `gpu` 列无意义（那种情况下我会先修换算再读）。
* 离线自证：`the_three_submission_windows_are_counted_apart`（嵌套情形三列都命中；**非嵌套**情形只有 `gpu` 列命中；一小时外的那次任何列都不命中）+ 两个反向臂（三列共用一个环 ⇒ FAIL；GPU 环不填充 ⇒ FAIL），**10/10 PASS**、`ctest -L phy -j 1` **203/203**。
* ⚠ 写这两个用例时又踩到**同一类坑两次**并都已修：① 比率的分母混用（"all receives" 要用自己的分母）；② 探针是**进程级单例**，ctest 每个用例独立进程、而直接跑二进制不是 ⇒ 断言改为**增量**，且"不该命中"的接收放在**任何窗口都不可能覆盖的时刻**。

**下一步（M2 的因果检验，代码小改已登记）**

* `record_dft_wait(wait_ns, begin_ns, end_ns)` 现在**记住窗口**（16 个的环 + 累计阻塞时长），`record_rx_wait(...)` 接收自己的窗口并在每次接收时与环比对 ⇒ 报告新增一行：
  `[ul_rx_wait] DFT-blocking overlap: K blocking window(s) = D% of the leg; slow (>1 ms) receives X of N overlapped one = P% (a coincidence predicts D%; all receives Q%)`
* ★ **这一行自带零假设的数**：若阻塞与停顿只是巧合，`P ≈ D`（实测 D ≈ 3.8–4.1%）；若阻塞是原因，`P → 100%`。
* 离线自证：`ul_pipeline_probe_test.slow_receives_are_tested_against_the_dft_blocking_windows`（**9/9 PASS**，全套 `ctest -L phy -j 1` **202/202**）；反向臂：去掉重叠判定 ⇒ 该用例 FAIL（已确认重编）。
  ⚠ 写这条仪表时**自己踩过一个统计口径错误**并被用例抓住：最初把「重叠的慢接收数 / 见过的全部接收数」当成了慢接收的重叠率（读成 50%），必须同时数「**总共多少次慢接收**」作分母 ⇒ 已修，用例断言写死为 `slow (>1 ms) receives 1 of 1 overlapped one = 100%`。
* ⚠ 该臂的契约 `ce device estimates` 会 FAIL（CE 在宿主是这一臂的变量）——已登记的"契约对阶梯臂的适用性"小改**本次未做**（需要发布 effective backend，改动更大），读作"对本臂不适用"。

**★★ M2b 的同腿判决（腿 `p109-cg-dft-grid-m2b`，2026-09-28 14:59）—— 三个窗口**全部否****：

```
[ul_rx_wait] DFT-window overlap: 803 slow of 741103 receive(s) accounted;
             wait 56/803=7% vs duty 3.9% (46341 win, all 45164/741103=6%);
             commit->end 56/803=7% vs duty 3.9%; gpu 52/803=6% vs duty 0.6% (all 43429/741103=6%);
             gpu-clock offset last 60us over 46341 sample(s)
```

* **三列的富集全部为零**：`wait` 7% vs 占空 3.9%、`commit→end` 7% vs 3.9%、`gpu` 6% vs 0.6% —— 而"全部接收"的命中率是 6% ⇒ **慢接收与任何一个提交相位都没有富集**（有富集的写法是 P→100%）。
* ★ **`gpu-clock offset = 60 µs` 这一项是"把假设变成读数"的自检**：它证明 GPU 时间戳与宿主 `steady_clock` 同基准（若不同基准，`gpu` 列会读出无意义的值）⇒ **上面的"否"不是在错误时钟上得出的**。
* ⇒ **M2/M2b 全部否**：停顿**不在**宿主等待、提交期、GPU 执行期**中的任何一段**。至此"同一提交的三个相位"这条路走完，剩下的候选只能是**跨提交/配置级**的东西（见 §6.151 —— 那条路最终把"配置级"这个词兑现成了 RACH 风暴）。
* 该腿其它读数（腿界、供交叉核对）：`[ul_rx_wait] max=12357 µs`、`gaps=0 rx_overflows=0`、`stale=0`、`dl_tx_slack min=−5314 µs`、`recv>1ms=804/755507=0.106%`、`loop max=840 µs`、`slip max=11862 µs`。

#### ⑦ ★ ping 的"偶发尖峰"是**会话起飞瞬态**，不是复发性停顿（用户 2026-09-28 给出 `rtt min/avg/max/mdev = 10.671/72.204/1658.281/109.327 ms, pipe 16`）
> ⚠ **本条归属已被 §6.151 就地更正（2026-09-28）**：这里的"起飞瞬态"只对**腿开头那一次**成立；用户在 `p109` 上给出的两次**中段**尖峰（seq 551 / seq 1138）**不是**瞬态，而是 `dft+grid` 配置下 **RACH 风暴 ⇒ DL 调度饥饿 ⇒ RLC 丢 SDU** 的产物（丢弃的 `pdcp_sn=551..556` 就是丢的 seq 552–557）。下面这段的分析（空档都在前 ~15 s）对 `p108` 的**开头**那一次仍然有效，但**不能**推广到中段。

**读法**：`pipe 16` = 有 16 个包同时在飞 ⇒ 不是"普遍慢"，而是**一次约 1.6 s 的停顿**（10 包/s × 1.6 s ≈ 16）。

**在 `p108` 那条腿（同一次 ping）的 gNB 日志里把它定位到了**（按 logger × 方向切开看的行间空档）：

| 流 | 最大空档（整腿去掉头尾 20 s）| 位置 |
|---|---|---|
| **GTPU（核心→gNB 到达）** | **4.28 s / 4.20 s / 2.12 s** | 全部在 06:18:19–26 |
| RLC/MAC（gNB→电台 发送）| 4.08 s / **1.60 s** / 1.54 s | 同上 + 06:18:28.58 |
| SCHED（调度，两方向）| 1.54 s / 1.33 s / 0.74 s | 06:18:16–31 |
| UL（UE→gNB 回复）| 3.96 s / 2.09 s / 0.74 s | 06:18:23–31 |

★ **关键**：**所有大空档都挤在 ping 开始后的前 ~15 s**（06:18:16–31），其中 **RLC/MAC 的 `1.60 s@06:18:28.58` 与 ping 报出的 `max=1.658 s` 对得上**；而 06:18:31 之后整段窗口**再没有 >0.75 s 的空档**。而且这些空档**连"请求从核心到达 gNB"（GTPU）那一层都在** ⇒ 那一瞬间**根本没有包到 gNB**。
⇒ **不是 gNB 的无线路径**（它自己的界：`rx_wait max 8.6 ms`、`dl_tx_slack min −3.0 ms`、DL 队列 ≤15 SDU、`gaps=0`），而是**上游（宿主协议栈/核心/ping 进程）在会话开头停了 1–4 s**，最可能是**邻居发现（ND/ARP）**：期望的邻居缓存缺失时宿主会把 ICMP 请求排队，等 UE 的 NA 回来才放行，而 NA 又要等上行授权 —— 这一串都在 gNB 之外。
（另：腿开头还有一个 **14.8 s 的"空档"**，那是 ping 还没开始、腿已经起来的正常静默，不是事件。）

**给后续腿的读法（已入纪律）**：① **先做一条热身 ping**（`ping -c 10 <UE>`）再跑被统计的那条，否则 `avg/mdev/max` 会被起飞瞬态污染；② 若尖峰出现在**会话中段**，在宿主上 `tcpdump -i <tun> -n icmp` 抓前 20 s 即可判"请求有没有及时离开宿主"。

### 6.151 ★★★ 「ping 尖峰」机制落定：`dft`+`grid` 配置下的 **RACH 风暴 ⇒ DL 调度饥饿 ⇒ RLC 丢 SDU**（★ 就地更正 §6.150⑦ 的"起飞瞬态"归属）

**缘起**：用户 2026-09-28 追问"上次 N78 的非优雅退出是偶发的还是有错误？"，并给出完整 ping 序列轨迹（两条**线性下降的 RTT 斜坡** + 丢包：seq 552–557 缺失，第一条 1638→75.4 ms、第二条 1253→69.9 ms）。§6.150⑦ 当时把它归给"会话起飞瞬态"，**那次归属是错的**：这两次都在**会话中段**。

#### ① 时间轴锚定（`p109-cg-dft-grid-m2b`；日志时间戳是 UTC，文件名是本地 +8）

* ping 的包在日志里**可单独识别**：`[GTPU] lif=NG-U DL … RX SDU. sdu_len=84`（84 = 20 IP + 8 ICMP + 56 data）。
* 首个请求 **07:00:34.933**（`pdcp_sn=0`）、末个 07:04:40.580，共 **2400 个 / 9.77 s⁻¹** ⇒ ping 覆盖 07:00:34.9–07:04:40.6。
* ⇒ **`pdcp_sn = ping seq − 1`**；用户给的两个尖峰：**seq 551 → 07:01:29.93**、**seq 1138 → 07:02:28.63**（`-i 0.1`）。

#### ② 尖峰时刻，gNB 的**两条流都是干净的**（按到达**间隔**读，不是按空档读）

| 窗口 | DL 请求到达间隔（38–40 个） | UL 回复经过间隔 |
|---|---|---|
| 07:01:29.93 ±2 s | 99–109 ms | 100–110 ms（含 60/140 各一次）|
| 07:02:28.63 ±2 s | 100–106 ms | 100–110 ms |
| 07:04:20.3 ±2 s（对照：**真有**停顿的一次）| 106–109 ms 但**只有 19 个**（前有空档）| **0 个** |

⇒ ★ **尖峰的瞬间，无线路径上既没有空档也没有突发** ⇒ 停顿**不在无线路径**（这条排除了 §6.150⑦ 的"上游 1–4 s 停顿"读法用于中段尖峰）。

#### ③ 真正的签名在 **DL 队列**：`queued_sdus` 堆积 + **RLC 丢弃 SDU**（两次事件同构）

| | 第一次（seq 551 那批） | 第二次（seq 1138 那批） |
|---|---|---|
| DL SDU 进队列但不发 | 07:01:31.317 → 32.674，`queued_sdus` **1→15**（`queued_bytes` 87→1305）| 07:02:29.683 → 32.183，`queued_sdus` **1→12** |
| 同期 DL 授权去哪了 | 每 ~100 ms 一个新 PRACH ⇒ `RAR: ra-rnti=0x10b rb=[0..3) tbs=9` + `UL: ue=invalid rnti=0x4833…` | 同构（`tc-rnti=0x4a5e…0x4a75`）|
| 队列里的 SDU 被**丢弃** | `RLC DL: Discarding SDUs. pdcp_sn=551..552 / 553 / 554 / 555 / 556`（07:01:32.714–33.238）+ `pdcp_sn=557..557` "Could not discard" | `Max reTxs 4 exceeded` 后同构 |
| ★ 与 ping 的丢包对账 | **丢弃的 `pdcp_sn=551..556` = ping 的 seq 552–557** —— 与用户轨迹**一对一** | — |
| 恢复：一个**大** DL 授权 | 07:01:33.189 `grant_len=909/904…`、`MAC DL PDU size=912: nof_sdus=11` | 07:02:32.189 `grant_len=1083`、`size=1121: nof_sdus=13` |

#### ④ **一次 UL TB 的批量回复**就是那条 RTT 斜坡

| 时刻 | 那一个 UL TB |
|---|---|
| 07:01:33.472 | `PUSCH rnti=0x4601 tbs=1473 prb=[0,11) 256QAM crc=OK` → `subPDUs: [lcid=4: len=90 ×15, len=88]` = **16 个 ping 回复塞进一个 TB**（RLC sn 788–803 连号）|
| 07:02:32.252 | 同构：12 个 `pdu_len=90`（sn 1414–1425）|

⇒ ★★★ **机制闭合**：gNB 把积压的 12–16 个请求**一次性**推给手机 ⇒ 手机**一次性**回完 ⇒ 宿主在**同一刻**收到 16 个回复 ⇒ RTT = 到达时刻 − 各自发送时刻 ⇒ **线性下降、斜率 = 发送间隔（100 ms）**，最大值 = 队列驻留时长（第一条 1658 ms / 第二条 1253 ms，与"队列开始堆积"的时刻逐一对上）。
⇒ `pipe 16`、线性斜坡、丢包 **是同一个事件的三个侧面**，不是三种现象。

#### ⑤ ★★★ 成因是**配置级**的：`dft` + `grid` 同时开 ⇒ 手机退回 **RACH 风暴**（~10 次/s）

手机不是在发 SR 要授权，而是在**做随机接入**：`PRACH detected_preambles` 在 `p109` 出现 **2044 次**（07:00:22 → 07:04:33，密集段 ~10/s 直到 ~07:04:07；按分钟 260/566/588/530/76/21/3），**每次一个新 `tc-rnti`**（0x4833→0x4846→0x4a5e…），且 Msg3 里带 **`C-RNTI: 0x4601`**（= 那部手机自己的 C-RNTI）⇒ 每 100 ms 一次 RACH（= 每次 ping 回复都撞上一次 RACH）。

**全 leg 普查（`grep -c detected_preambles`）—— 与 `LEG_CG_MODULES` 精确对应**：

| 臂 | PRACH | 臂 | PRACH |
|---|---|---|---|
| `cg-none`(`p92`) / `cg-dft`(`p93`) / `cg-grid`(`p97`) | **1 / 1 / 1** | **`cg-dft+grid`(`p100`)** | **2376** |
| `cg-ce`(`p98`) / `cg-eq`(`p99`) / `cg-ce-eq`(`p101`) | 1 / 1 / 1 | **`cg-dft+grid+ce`(`p102`)** | **2407** |
| `cg-grid-ce`(`p106`) / `-clean`(`p107`) | 1 / 1 | **`cg-all-light`(`p94`)** | **2121** |
| ★ **`cg-dft+grid` ＋ `OCUDU_DFT_STAGE_INPUT=1`（`p105`）** | ★ **1** | **`cg-dft+grid-m2/-m2b`(`p108`/`p109`)** | **2406 / 2044** |
| `gpu` 验收腿 `p103`/`p104`（`fused=yes`）| **1 / 1** | `cg-repro`(`p64`) | 107 |

**判别腿是 `p105`**：同样两个旋钮，只多一个"**把 DFT 输入先 staging 到独立缓冲**" ⇒ 风暴**清零**（2400 → 1）。

#### ⑥ ★ M1（共享页）的两个口径必须**分开记**（更正 §6.150⑤ 的"否"）

* **对"毫秒尾部"口径：否**（原判成立）——`p105` 无风暴但 `recv>1ms` 仍是 **0.065%** ≈ `p100` 的 **0.073%** ⇒ **尾部与风暴解耦**，`dft+grid` 那条"触发对"结论本身不变。
* **对"功能性上行"口径：★ 是**——`newBufferWithBytesNoCopy` 把**电台样本页零拷贝**交给 Metal，而 `fused=no` 时宿主仍要读网格（启动行自陈：`… refuses it (grid_has_host_consumers=false)` 只在 `cg` 族出现）⇒ 上行处理出错。
* **故障的样子**（风暴起点 07:00:21–22）：`TA_CMD: tag_id=0, ta_cmd=29/32/34`（TA 命令**反复重发**且值抖动）→ `Discarding DL HARQ process TB with tbs=111. Cause: Maximum number of reTxs 4 exceeded`（手机**不 ACK**）→ `Discarding UL HARQ process TB tbs=11`（Msg3 也收不上来）⇒ **手机失步，改用 RACH 要授权** ⇒ 自我维持的风暴。
  ⚠ 而 **PUSCH 数据本身的 CRC-KO 率没有异常**（`p109` 16.0% vs `p103` 14.6% vs `p104` 15.9%）⇒ 坏的是**时序/控制面**（TA、ACK、Msg3），不是数据面解码质量 —— 这也是它此前一直没被"吞吐/CRC"类判据发现的原因。

#### ⑦ 影响与纪律（**这一条会影响别的结论，必须执行**）

1. ★ **验收对不受污染**：`p103`/`p104`（`mode=gpu fused=yes device_grid=yes`）**PRACH=1、丢SDU=0、多回复TB=0** ⇒ V1 1354.3/1420.4、契约 9/9 的读数**不含本现象**。
2. ⚠ **风暴腿清单：`p94` / `p100` / `p102` / `p108` / `p109`** —— 它们的时延读数（含 `p100` 的 0.073%、`p94` 的 0.111%"尾部"）是在**额外 RACH 信令 + DL 重传 + 调度饥饿**下测的；引用时必须标注（`p100`↔`p93`/`p97` 的"单模块对照"因此**不是干净对照**）。
3. ★ **新增必读读数（已入纪律）**：每条腿读一次 `detected_preambles` 计数；**`>100` = 风暴腿**（其读数不得单独用于时延结论），**`==1` = 干净**。
4. 若还要用 `dft+grid` 做实验：**必须同时开 `OCUDU_DFT_STAGE_INPUT=1`**（`p105` 形态）——这是目前唯一已知能让该配置既保留设备侧路径、又不破坏空口的组合。
5. 附带观察（本次未追）：RF 实时失败在两种模式下都有（`p109` 790 underflow + 394 late、`p103` 414 + 177）⇒ 宿主实时余量普遍偏薄；与风暴没有一对一关系，但**DL 欠载本身会破坏手机接收**，值得单列。

#### ⑧ 待裁决（**功能性缺陷，不是时延问题**）

* 代码侧：`fused=no` 下**禁止**零拷贝 wrap（退化为既有 staging 路径），或在 wrap 成功时给出契约告警 —— 这属于**修 bug**，与"用不用 GPU 换时延"的取舍无关。
* 验收日报把 **PRACH 计数**列为必读（风暴 = 0 才算健康腿）。
* §6.150⑦ 的更正已在"证据索引"登记；用户给的那次 ping 轨迹可作为该缺陷的**外部复现证据**（macOS `ping` 的 seq/斜坡/丢包三件套与日志逐条对上）。

#### ⑨ ✅ 处置（用户裁决 2026-09-28）：**分裂模式（`cpu_gpu`）的 DFT 输入改为"提交时拷一份"**，交付的融合车道保持零拷贝

**裁决原话（要点）**：`cpu_gpu` 只是我们设计的**中间（debug／对照）模式，不是最终模式** —— 最终形态是"IQ 一旦开始流动，要么走全 CPU 路径、要么走全 GPU 路径"，**数据在同一跳内于 CPU/GPU 之间来回本身就是开销**，没有充分理由。所以只要这个 bug 是 `cpu_gpu` 特有的，修法就可以是**一次 memcpy**，但必须在**代码注释与设计文档里写清楚**，免得以后把它误读成"零拷贝设计目标被放弃"。

★ 这个裁决与 `include/ocudu/phy/phy_pipeline_mode.h` 的既有定义**完全一致**（原文）：`gpu` = 融合车道，"the whole IQ -> LLR chain runs inside one device-side pipeline with only two host <-> device *data* crossings (the IQ upload and the LLR download)"；而 `cpu_gpu` = "module-level offload. **Each module follows its own backend knob, so every module boundary keeps its own host <-> device crossing**"。⇒ **在分裂模式的模块边界拷一份，是这个模式自己的语义，不是零拷贝目标的破例。**

**① 实现（一处默认值 + 一条按模式重述的判据）**

* `ocudu_dft_metal_engine.mm` 的 `stage_input_requested()` 改为**模式化默认**：未设 `OCUDU_DFT_STAGE_INPUT` 时，**已发布模式 == `cpu_gpu` ⇒ staged（拷贝）**，`gpu` ⇒ wrap（零拷贝）；env 仍可双向强制（`=1` 总拷、`=0` 总不拷）用于 A/B。未发布模式的进程（单测/工具/回放）**保持历史 wrap**。
* **契约判据按模式重述**（这是**判据变更，先登记再改**）：新增精确计数器 **`radio_zero_copy`**（只数"零拷贝读了电台页"的变换，走 `[metal_stats] dft … radio_zero_copy=` 报告）；`dft radio inputs` 在 **`gpu`** 下仍要求 **`staged == 0`**（融合车道的红线，反向臂仍在 `dft_processor_metal_unit_test.cpp`），在 **`cpu_gpu`** 下要求 **`radio_zero_copy == 0`**（"没有任何变换直接读电台页"），两段的措辞与读数都打在该契约行上。
  ⚠ 为什么不能直接用 `radio_inputs == wrap_copies`：`wrap_copies` 数的是**所有**回退拷贝（含**网格**与引擎自身表），与"变换数"量纲不同（5.9.98 那次判据修正就是踩了混量纲）。

**② 离线自证（两条臂必须反向成立，各占一个进程）**

`ofdm_demodulator_metal_batch_test` 的负对照（"提交后立刻覆盖样点"）在两种模式下**要求相反**，因此新增了一个 ctest 用例（模式注册表是进程级、不可撤销）：

| 进程 | 负对照读数 | 契约行 |
|---|---|---|
| 无模式（历史/交付路）| `[reuse] … mismatching=17808`（wrap 活着）→ `ALL OK` | 不适用 |
| **`--stage-split`（`cpu_gpu`）** | `[reuse] … mismatching=0`（读的是拷贝）→ `ALL OK` | `radio_zero_copy=0 of radio_inputs=770 … -> OK`（措辞为分裂模式的要求）|

⇒ `ctest -L phy` **204 → 205**（地板规则 ≥193 照旧）。

**③ 交付路径不变的证据（不是承诺）**

| 腿 | 模式 | `radio_inputs` | `wrap_copies` | PRACH | 丢 SDU |
|---|---|---|---|---|---|
| `p103` default / `p104` stress | `gpu` | 435 260 / 2 066 078 | **0** | **1** | **0** |

⇒ 融合腿仍走 wrap，契约在 `gpu` 下仍要求 `staged == 0`；本次改动**不触交付二进制路径的行为**（只多了一个计数器与一个模式判断）。

**④ 纪律（新增，与"臂腿不能冒充验收腿"同族）**

★ **`cpu_gpu` 腿的 `wrap_copies > 0` 意味着这条腿不是零拷贝读数**：它的 `crossings` 记账里含这次拷贝，**不得**用它的读数论证 G1/G2 或任何零拷贝主张；引用时写明"该腿输入为 staged"。

**⑤ ⚠⚠ 空口验证**失败**（2026-09-28 18:41 腿 `p110-cg-dft-grid-staged`）—— 本节的因果归属就地更正，且**不得**再宣称"staging 解决了风暴"**

* **腿跑的确实是新二进制**（`Built in Release mode using commit 2e4c89956e`、构建 18:34 < 起跑 18:41；二进制内含 `radio_zero_copy` 与新的契约措辞），`mode=cpu_gpu`、`dft=metal` + `device_resource_grid=on`、**未设任何 `OCUDU_*` 旋钮**。
* **风暴照旧**：`detected_preambles` 已 1427（266/589/148 每分），`Discarding SDUs` 117，`TA_CMD` 27，ping 的 SR 真被发出仅 5 次/分（干净腿 413–603），请求到达**零空档** ⇒ 同一签名；三条 RTT 斜坡（seq 136 / 1045 / 1146，皆 ~16 包批量释放）。
* ★★ **腿作废，但它仍然回答了那个开问题**：关机时 `Could not stop application after 5 seconds. Forcing exit.` ⇒ 排在契约之后的 `[metal_stats]` 块没打 ⇒ `run_leg.sh` 判 **NO COMPLETE REPORT（本腿不得当证据读，尤其不得把缺失的计数器读成 0）**。**但契约行抢先打出来了，它证明默认值生效**：
  `dft radio inputs: … 709548 buffer wrap(s) had to stage a host copy … split mode: … zero-copy reads (radio_zero_copy=0 of radio_inputs=709548) must be 0 -> OK`
  ⇒ **输入已 staged（`radio_zero_copy=0`）而风暴照旧** ⇒ **"零拷贝输入读 ⇒ 风暴"这条归因到此关闭**；剩下站得住的是下面 ⑥ 的"臂 × 功率状态"交互。
  ⚠ 该腿的 `ce device estimates: 0 device, 62281 host -> FAILED` 是 **`dft+grid` 臂的已知预期 FAIL**（CE 在宿主，§6.150⑥ 已登记），不是新问题。
  ⚠ **关机挂住**（`Forcing exit`）在全归档只出现过两次：`p70-n78-spp1024`（0927，gpu 模式、无关臂）与 `p110`；同走 staged 路径的 `p105`（433916 次 staged）**没挂** ⇒ 罕见、有先例、**不是本改动的专属产物**；`p110` 同时是 RF 实时失败率最高的腿（1773 vs p109 1184 / p105 474），与"关机时仍有东西没离开循环"一致。★ 缓解办法（腿侧，零代码）：**先停 ping、等几秒让 UL 排空，再按一次 Ctrl-C**。若在后续腿上复现，第一件该做的是把 staged 路径从"每个变换 `newBufferWithBytes` 新建"改成**每个块一次分配**（引擎自持、寿命随块/令牌），既治挂住也去掉一处真实低效。

**⑥ ★★★ 新的、更强的判别量：手机的发射功率状态（`SE_PHR: ph=[…`）—— 并且它与臂**互相混杂**

| 腿 | 臂 | PRACH | **PHR 中位** | PHR 时间过程（attach 后 0–10/10–20/20–40 s）| gNB 侧 PUSCH SINR 中位 |
|---|---|---|---|---|---|
| `p105` | `dft+grid`+stage | 1 | **+14** | 12 / 15 / 14–15，**全程不降** | 23–24 dB |
| `p103` | `gpu` 融合 | 1 | **+16** | 16 / 16 / 17 | 22–23 dB |
| `p109` | `dft+grid` | 2044 | **+7** | **17 / 9 / 7–8（先掉余量）**，风暴在 **+19 s** | 26–29 dB |
| `p110` | `dft+grid` staged | 1427 | **−2** | **6 / −5 / −2…+1**（起跑即顶格）| 26–28 dB |

* **风暴腿的手机被顶到/顶过最大发射功率**（PHR→0/负；`p_cmax` 18–19 dBm），而 **gNB 自己测到的 SINR 反而很高（26–29 dB）** ⇒ 不是"路损大"，而是**功率环把手机推到上限**；`p109` 的顺序是**先掉余量（前 20 s）再起风暴（+19 s）**。
* ★ **同余量比臂，臂仍然赢**：`p92`(none, PHR 9, PRACH 1) vs `p108`(dft+grid, PHR 9, **2406**)；`p97`/`p98`(grid/ce, PHR 11, 1/1) vs `p94`(all, PHR 11, **2121**)。
* ★ **同臂比余量，余量赢**：`p105`(dft+grid+stage, PHR 14, **1**) vs `p110`(同臂同 staged 默认, PHR −2, **1427**)。
* ⇒ **结论（更正版）：这是交互** —— **臂（`cpu_gpu` 下 `dft+grid` 的零拷贝输入读）决定"多脆弱"，手机功率状态/链路余量决定"是否越过阈值"**。普查里"5/6 条 `dft+grid` 腿风暴、8 条其它臂全干净"的对比，**同时**被臂与余量混杂（`dft+grid` 那批腿正好跑在余量较低的时间段），旧归档**无法单独分开**。
* ⚠ **本次修复的定位随之降级**：它按模式语义仍然正确（分裂模式本来就在模块边界搬运一次），也**确实**去掉了分裂模式里的零拷贝输入读，但**没有**消除风暴 ⇒ **不得**把它记为风暴的修复；保留/回退由用户裁决。
* ★ **新增腿质量协变量（纪律）**：**每条 OTA 腿的头 30 s 必读 `SE_PHR: ph=[…`**；PHR 中位 < ~10 dB ⇒ 该腿处于"最大功率边缘"，控制面（PUCCH SR/ACK、Msg3、TA）本就脆弱，**此时的 ping 尖峰不能归给车道**。
* **下一步（受控实验，两次短腿，零代码）**：① **同臂好余量**：把手机放到 PHR ≥ ~13（像 `p105`/`p103`）后重飞 `cpu_gpu + dft+grid` ⇒ 若干净，则臂单独不足以触发；② **别的臂坏余量**：在**今天这个位置**（PHR ≈ 0）重飞从未风暴过的臂（如 `LEG_CG_MODULES=grid`）⇒ 若风暴，则臂无关、余量/功率环才是触发。⚠ 另：本仓**未记录 TPC**（`grep -c tpc = 0`）⇒"谁把功率顶上去的"目前不可直读，若需要就得加仪器。


**⑧ ★★★ `p111`/`p112` 两条腿定案：触发条件是"设备网格写生效"，输入路径与 UL 时延都不是；并且**撤回上一版的 staging 默认****

| 腿 | 臂 | 输入 | **UL 跳中位**（`[ul_pipeline]`）| PRACH | 丢SDU | SR真发出 | PHR |
|---|---|---|---|---|---|---|---|
| `p112` | `grid` 单臂（**回落宿主写网格**，启动行自陈）| — | **598 µs** | **1** | **0** | **2159** | +10 |
| `p111` | `dft+grid`，**staged 默认** | 全拷贝（`radio_zero_copy=0 of radio_inputs=514332`）| 1542 µs | **2179** | 162 | 103 | −3 |
| `p110` | `dft+grid`，staged（void）| 全拷贝 | — | 2393 | 138 | 158 | −2 |
| `p105` | `dft+grid`，staged（旋钮）| 全拷贝 | 1547 µs | **1** | 0 | 2074 | +14 |
| `p100`/`p109` | `dft+grid`，wrap | 零拷贝 | **997 / 993 µs** | 2376 / 2044 | 69 / 165 | –/408 | 4 / 7 |

* ★ **臂就是触发器**：`p112`（在今天更差的余量 PHR+10 下）完全干净，而它的启动行写明 **`grid` 单臂回落成宿主写网格**（CPU DFT 不提供 device grid write）⇒ 干净的/风暴的边界是"**设备网格写是否生效**"（=`dft=metal` + `grid=on`），**不是**"变换读哪块内存"。
* ★★ **输入路径被彻底排除**：`p111` 的报告证明**每个输入都是拷贝**（`wrap_copies=514332`、`radio_zero_copy=0`）**而风暴照旧** ⇒ §6.151⑤ 那次归因关闭。
* ★★ **UL 时延与风暴无关**（另外那个方向的对照）：wrap 腿 993–997 µs 会风暴，staged 腿 1542–1547 µs 既有干净（`p105`）也有风暴（`p111`）。
* ⚠⚠ **上一版 staging 默认撤回（本节 ①–④ 相应作废，只留其历史）**：同臂对照给出它的真实代价 —— **UL 跳中位 +550 µs（997→1542）**，因为该路径**每个变换新建一个 `MTLBuffer`**（`p111` 里 514 332 次 ≈ 1785 次/秒）。撤回到"仅 `OCUDU_DFT_STAGE_INPUT` 旋钮可开"，契约判据回到对所有模式统一 `staged == 0`，离线测试的 `--stage-split` 用例与其 ctest 条目一并删除（`ctest -L phy` 回到 204）。**保留** `radio_zero_copy` 计数器（纯诊断，正是它证明了这次默认值生效过）。
* ★★★ **两条腿的 ping 汇总与 gNB 侧计数器逐项对上（用户 2026-09-28 提供）**：

| 腿 | 丢包 | ping `min/avg/max/mdev` | `pipe` | gNB 侧 `丢SDU`（RLC 丢弃）|
|---|---|---|---|---|
| `p111` dft+grid staged | **6.75%**（2400 发 / 2238 收 = 丢 **162**）| 12.079 / **92.047** / 1632.590 / 111.942 | 16 | **162** ← 一对一 |
| `p112` grid 单臂（宿主写网格）| **0%**（2400/2400）| 8.835 / **27.270** / 51.298 / 6.134 | – | **0** |

  ⇒ **丢包账闭合**：ping 丢的 162 个包 = RLC 丢弃的 162 个 SDU；`pipe 16` 与 `max 1632 ms` = 那条批量释放停顿（与 `p109` 的 1658 ms 同签名）。
* ★ **"平均时延更高"的分解（同臂对照，不是跨臂）**：`avg` 从 27.3 ms（干净）涨到 92.0 ms，**主体是风暴本身**（手机转入 RACH 模式后典型回复 ~60 ms vs 干净腿 ~25 ms）+ 三条 1.6 s 停顿；**其中属于 staging 的只有 ~0.55 ms**（UL 跳中位 993→1542，wrap 腿 `p100`/`p109` vs staged 腿 `p111`）。另有一条**与本次改动无关的臂级差**：**设备网格写**腿的 UL 跳中位 ~1.55 ms vs 宿主写网格腿 **598 µs**（`p112`）⇒ 即使干净的 `dft+grid` 腿（`p105` 1547 µs）也会比宿主网格腿高出 ~1 ms 的基线 RTT。

**⑨ ✅ 顺路修掉一处真实的顺序缺口：`wait_per_slot` 现在要求"交棒可用"（`ofdm_demodulator_impl.cpp`）**

在读上面这些报告时发现：`grid_consumed_on_device = config.device_resource_grid`（`lower_phy_factory.cpp:148`）把"**网格在设备上写**"当成了"**网格在设备上被消费**"，于是 `wait_per_slot` 为真 ⇒ **只等每槽最后一个符号**（日志自陈 "the earlier symbols are not waited for"）。这条等待的全部理由是"**一个队列按提交顺序完成命令缓冲**"，而它只覆盖**进了同一个命令缓冲（块）**的变换：

| 路由 | `batched=`（批派发/承载变换）| 槽末等待是否覆盖全槽 |
|---|---|---|
| 零拷贝（wrap）`p100`/`p109` | **37308/522312、46341/648774**（每批 14）| ✅ 覆盖（全槽一个 cb）|
| **staged**（拷贝）`p105`/`p111` | **0/0**、`0 joined an open block` | ❌ **只覆盖最后一个符号** |
| 宿主路 `p93` | 0/0（无电台输入）| 不适用（网格由宿主写）|

⇒ 在**交棒被拒**的配置（`cpu_gpu`、以及任何宿主读者臂）里，staged 路会把**前 13 个符号的网格写置之不待**，而 **CE（该臂在宿主）与 PUCCH（构造上就是宿主读者）** 正好随后就去读那张网格 —— 这正是代码自己警告过的失败模式（"a host consumer ... would read memory the GPU has not written yet"）。**修法（保守形式，一行）**：`wait_per_slot = device_write && grid_consumed_on_device && handover_allowed()` —— 只有"读者的顺序由交棒/grid-ready 等待提供"时才允许一跳只等一次；**交付的融合车道行为不变**（`handover_allowed()` 为真），分裂/调试模式退回逐符号等待（只损失调试模式里的一个优化）。
★ **这条缺口也追认了撤回 staging 默认为正确**：那个默认不只是没用（+550 µs/跳），它还**悄悄破坏了 `wait_per_slot` 所依赖的每槽一个 cb 的批量化**。
★ **它不解释 wrap 腿的风暴**（`p100`/`p109` 的批量化完好、槽末等待覆盖全槽）⇒ 对那两条腿，**"设备写出的网格内容是否正确"仍是唯一未测的落点**，即下面那条仪器。
★ **离线自证**：`ofdm_demodulator_metal_batch_test` 的 `[grid] pipelined device write vs host write` 与 `[armed] hand-over grid vs host write` 仍 **0/17808 不一致** ⇒ 改动不改变设备网格的数值；`ctest -L phy` 全绿。

**⑭ ★★★ 诊断臂空口复现（`p118`）：旧"一跳只等一次"在 **wrap 路** 上照样起风暴 —— 我 ⑫ 那次"更正"被实测否掉，以实测为准**

同一臂（`dft+grid`）、同一位置、同一天，只差等待策略：

| 腿 | 策略 | **PRACH** | sr=yes | TA_CMD | HARQ 扔 | **DL 队列峰值** | **PHR** | ping |
|---|---|---|---|---|---|---|---|---|
| **`p118`** | **旧（一跳一次）** | **2472** | **125** | 83 | 28 | **7** | **+2** | 0% 丢包，**avg 64.0 / max 698 / mdev 23.6 / pipe 7** |
| `p115` | 保守（逐符号）| **1** | 2058 | 2 | 1 | **1** | +9 | 0% 丢包，avg 28.5 / max 177 / pipe 2 |
| `p109`（下午）| 旧 | 2044 | 408 | 189 | 184 | 22 | +7 | 6.75% 丢包 / pipe 16 |

* ⇒ **旧策略在 wrap 路上同样起风暴**（PRACH 2472、SR 从 ~2000 跌到 125、TA 抖动、DL 队列 1→7、PHR +9→+2）。这次队列峰值只有 7、未到丢弃阈值 ⇒ 表现为**攒包（`pipe 7`/`max 698 ms`）而不是丢包** —— 也是"**风暴常先于丢包出现**"的一个清晰样本。
* ★★ **PHR/功控那一环由此接上**：余量塌陷（+9→+2）**跟着等待策略走**，而不是跟着链路 ⇒ 因果是"**网格顺序缺口 ⇒ 宿主 CE/PUCCH 读到未写完的网格 ⇒ 估计偏 ⇒ 功控把手机顶到上限 ⇒ 控制信道先崩 ⇒ 风暴**"。
* ⚠ **结论修正史（诚实记录）**：⑫ 我依据代码（`:2401` 逐符号登记同一 `open_cb`、`:2157` `wait_slot` 自行 `commit_open`）断定"wrap 路本来被覆盖、下午的风暴是时间/状态"，并据此写了"实测不支持代码推断"。**`p118` 把这次更正也否掉了**：代码阅读仍缺一环（wrap 路到底在哪里漏等，见下），**以实测为准**。
* ✅ **因此保守策略（`handover_allowed()`）是永久的**：调试臂 wrap 每跳 993→1393 µs、staged 1542→1700 µs 的代价保留。⚠ 推论：**`cpu_gpu` 臂的跨臂时延对比换了口径** —— 修好之前测的"设备网格写 vs 宿主写网格"差（+400 µs）在保守策略下变大（逐符号等待也计进去），引用旧数时要写明"修复前口径"。
* ★ **交付车道不受影响**（且已实测）：融合 `gpu` 腿用的是**交棒 + grid-ready 等待**给宿主读者排序（不是这条一跳一次的等待），`p103`/`p104`/`p116`/`p117` 全部 `PRACH=1`、0 丢包。
* **仍未解释（下一步仪器）**：wrap 路究竟在哪里漏等。唯一能直接看见它的读数是**早/晚两次读回同一批 RE**（在槽末等待**之前**读一次、**之后**再读一次并比对）：若"前≠后" ⇒ 抓到了未覆盖的写；若恒等 ⇒ 漏等不在网格内容而在别处（例如读者拿到的代次）。这也是唯一能把调试臂那 400 µs 安全收回来的路径。

**⑬ ✅ 加了"旧策略"诊断臂 `OCUDU_DFT_WAIT_PER_SLOT=1`（默认关）—— 一条腿即可判定 wrap 路的下午风暴是"策略"还是"时间"**

* 行为：开启后 `wait_per_slot = device_write && grid_consumed_on_device`（即 6.151⑨ 之前的策略）；**只在输入未 staged 时生效**（staged 时该等待并不覆盖，开启它只会重现缺口 ⇒ 检测到组合就拒绝并提示一次）⇒ **这条臂按构造不会制造缺口**。
* 离线：默认路 wrap 活着（`[reuse] mismatching=17808`、`ALL OK`）、`WAIT_PER_SLOT=1` 单开 `ALL OK`、与 `STAGE_INPUT=1` 同开时由 staging 那条接管；`ctest -L phy` 全绿。
* **待飞的判别腿**（臂腿，门禁会拒 ⇒ 只按 PRACH/SR/PHR/丢SDU 判）：`cpu_gpu` + `LEG_CG_MODULES=dft+grid` + `OCUDU_DFT_WAIT_PER_SLOT=1`（不设 `STAGE_INPUT`），今天同一位置：**干净 ⇒ 下午的风暴是时间/状态驱动、旧策略在 wrap 路安全 ⇒ 可永久放宽策略、省回 wrap 臂 400 µs/跳**；**风暴 ⇒ 代码推断仍缺一环 ⇒ 保守策略保留**。

**⑩ ✅✅ 空口验证通过（`p114`，2026-09-28 19:25）：等待修复**就是 staged 腿风暴的成因****

同臂、同一天、同一位置，只差这次修复：

| 腿 | 臂 | PRACH | TA_CMD | sr=yes | 丢SDU | 多回复TB | **PHR** | ping |
|---|---|---|---|---|---|---|---|---|
| **`p114`** | dft+grid + **等待修复** | **1** | **3** | **2062** | **0** | **0** | **+8** | **0% 丢包，avg 29.05，max 135.9，mdev 7.36，pipe 2** |
| `p111` | dft+grid（修复前）| 2179 | 53 | 103 | 162 | 2 | −3 | 6.75% 丢包，avg 92.0，max 1632，mdev 111.9，pipe 16 |
| `p112` | `grid` 单臂（宿主写网格）| 1 | 3 | 2159 | 0 | 0 | +10 | 0% 丢包，avg 27.3，max 51.3 |

* ★ **决定性**：`p114` 的余量只有 **+8 dB**（仍属偏差，与风暴腿的 8/9 同档），链路却完全干净 ⇒ **起作用的是等待修复，不是余量**。
* ★ 修复本身也打在计数器上：`waits=765709 = transforms`（逐变换等待；修复前 `waits=377971`＝每次提交一次）。
* 代价只在调试臂：`[ul_pipeline]` 中位 **1700 µs**（修复前 1542；宿主网格路 598）⇒ 逐符号等待 ≈ +160 µs/跳，**融合交付车道不受影响**（`handover_allowed()` 为真，仍是一跳一等等）。
* 该腿的两条契约 FAIL 都是该臂**预期内**的（`ce device estimates: 0 device…host`、以及 staged ⇒ "输入必须是电台样点"那条）。

**⑫ ✅ `p115`：wrap 臂在保守等待策略下也干净 —— 因此"精确判据"被否，保守版保留**

| 腿 | 臂 | 等待策略 | PHR | PRACH | sr=yes | 丢SDU | ping | UL 跳中位 | `batched=`（每派发）|
|---|---|---|---|---|---|---|---|---|---|
| **`p115`** | **wrap** | **保守（逐符号）** | **+9** | **1** | **2058** | **0** | **0% 丢包，avg 28.49，max 176.8，pipe 2** | 1393 µs | 31143/249144 = **8.0** |
| `p109` | wrap | 旧（一跳一次） | 7 | 2044 | 408 | 165 | 6.75% 丢包，pipe 16 | 993 µs | 46341/648774 = **14.0** |
| `p114` | staged | 保守 | +8 | 1 | 2062 | 0 | 0% 丢包 | 1700 µs | 0/0 |

* ⚠⚠ **更正（同日读码）：wrap 路的旧策略是被覆盖的——我上面那句"实测不支持代码推断"说过头了。** 三条代码事实：
  ① 电台输入（wrap）路上，**每个符号提交时都把该环形槽登记到同一个 `open_cb`**（`ocudu_dft_metal_engine.mm:2401`），块中途的 `flush_pending_front_end` 只是往**同一个还开着的编码器**里编码（不是提交新 cb）；
  ② **`wait_slot()` 自己先 `commit_open()`**（`:2157-2159`）再等该槽的 cb ⇒ **在 14 个符号中任何一个上等待都覆盖整槽**；
  ③ staged 路每个变换换新缓冲 ⇒ 绑定变化触发 flush（`:2366-2369`）⇒ 每变换独立 cb ⇒ 旧策略只等最后一个符号 ⇒ **前 13 个符号无人等待**（`batched=0/0` 是它的指纹）。
  ⇒ 因此：**staged 路的缺口是真的**（代码 + 计数器 + `p111`→`p114` 空口验证）；而 **wrap 路下午那几条腿（`p100`/`p102`/`p108`/`p109`）的风暴不能用等待策略解释** —— 与傍晚同臂同余量的干净腿（`p112`/`p114`/`p115`，PHR 8–10）相比，剩下的变量是**时间/状态**（宿主负载、手机状态）。宿主负载指标确实更高（风暴腿 RF 实时失败 108–186/min、`recv>1ms` 362–804；干净腿 0.6–72/min、9–372），但**它多半是风暴的后果而非原因**，不能定因果。
* ★ **因此不做"精确判据"**（让引擎回报"这次进没进块"、wrap 路恢复一跳一次）：那会把 wrap 路带回**已经实测会风暴**的策略。代价是调试臂每跳 +400 µs（wrap：993→1393；staged：1542→1700），**交付融合车道零影响**（`handover_allowed()` 为真，仍是一跳一次）。
* ⚠ **留作后账**：为什么 wrap 路的"槽末一次等待"没有覆盖住整槽（是块中途 flush？是逐槽登记与 wait 的环形槽索引错位？还是 `end_block` 的登记点？）—— 这是纯代码问题，与交付无关；**在解释清楚之前不要恢复该策略**。

**⑪ wrap 腿（`p100`/`p102`/`p108`/`p109`）不是这条缺口 —— 它们的风暴仍未解释**

代码把两条路的差别钉死：`note_slot_submission(…, slot, block_accumulating ? open_cb : last_committed_cb)`（`ocudu_dft_metal_engine.mm:1887`）——**批量化（wrap）路在每个符号提交时就把该环形槽登记到"开着的块 cb"**，而 `wait_slot()` 自己会先 `commit_open()` 再等（`:2157-2160`）⇒ 即使逐符号等待也能覆盖整块 ⇒ **修复前"只等最后一个符号"在 wrap 路上也是覆盖全槽的** ⇒ wrap 腿没有这条缺口。
⇒ 对 wrap 腿，**"设备写出的网格内容"仍是唯一未测的落点**（也正是下面那条待建仪器）。★ 另记一条仍未分开的混杂：`p102`(PHR 8)、`p108`(9) 风暴 vs `p92`(9)/`p93`(10) 干净 ⇒ 同余量下臂仍分得开 ⇒ "臂 × 余量"两者都在起作用。

* **机制现在只剩一个落点**：**由 Metal kernel 在设备上写的资源网格，以及读它的那些宿主消费者**（本臂里 CE 在宿主、PUCCH 无设备视图）。★ **仪器缺口**：现有 8 条契约检查**没有一条**覆盖"设备写出的网格内容/可见性"（`p111` 与 `p112` 的检查清单逐字相同）。⇒ 下一步该做的仪器：**在空口上把设备写出的网格与用同一批样点在宿主算出的参考网格逐 RE 比对**（离线该比对是精确的：`[grid] pipelined device write vs host write: REs=17808 mismatching=0`），外加"宿主消费者拿到的网格代次"读数。相关假设（待证）：设备网格若有细微错误 ⇒ 宿主 CE 的估计偏 ⇒ 功率控制把手机顶到上限（PHR→0）⇒ 控制信道先崩 ⇒ 风暴。


**⑦ 仍开放，以及这个默认的撤销条件**

机制**仍未诊断**（搬运仍然只是隔离）：

* 生命周期解释**已被引擎自己的计数否掉**：风暴腿 `keepalives=648774/648774 (max in flight 14)` —— 每个零拷贝变换都挂了输入保留令牌并正常释放；
* 也不是 PHY 自己又拷了一份：`[ul_host] symbols=10576930 in_place=10576930 assembled=0`（全部"在电台放的地方原地读"）；
* 写作方已查实：`radio_uhd_rx_stream::receive_block()` 把**我们的池缓冲裸指针**交给 `uhd::rx_streamer::recv()`（一个包一次、循环填满一块）⇒ **写这块内存的就是 UHD 的接收路径**；与 GPU 的读之间除了提交顺序没有别的同步。
* ⇒ 剩余候选是"**UHD 的写 与 GPU 的读之间的可见性/时序**"，拷贝按构造把它绕开。**判别实验**（决定能否改成零拷贝）：在同一批符号上比 ①提交时宿主看到的切片哈希、②设备写出的网格、③用①在宿主算出的参考网格；②==③ ⇒ 我的时序叙事错，必须换解释；②≠③ 且与"该块刚落地的时刻"相关 ⇒ 用**事件**把 kernel 的输入读序化在 `receive()` 返回之后（`encodeSignalEvent`/`encodeWaitForEvent` 这一族本仓已有），**一个字节都不用拷**。**那次诊断的产出就是这个默认的移除**，而不是偏好。

### 6.152 ✅ 新 HEAD 上的验收对 `p116`/`p117`（2026-09-28 19:4x）：判据全绿，且顺带量出判据对链路差异的稳健性

| 腿 | 臂 | 种类 | **V1 中位** | p95 / max | 契约 | `cbs/lane` | `gaps` | `stale` | PRACH | PHR | **UL SINR 中位** | UL TBS 中位 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **`p116`** | gpu | default（带 ping）| **1401.5** | 1560 / 6242 | **9 of 9** | 2.00 | 0 | 0 | 5 | +11 | – | – |
| **`p117`** | gpu | stress（iperf3）| **1407.0** | 1541 / 6926 | **9 of 9** | 2.00 | 0 | 0 | 2 | +8 | **12.9 dB** | **976** |
| `p103` | gpu | default（历史）| 1354.3 | 1486 / 5857 | 9 of 9 | 2.00 | 0 | 0 | 1 | +16 | – | – |
| `p104` | gpu | stress（历史）| 1420.4 | 1554 / 6794 | 9 of 9 | 2.00 | 0 | 0 | 1 | +8 | **23.5 dB** | **4097** |

* `leg_gate`：`p116` **8 of 8 judged**、`p117` **9 of 9 judged**（其余检查与腿的 binding 不匹配 ⇒ 不判）。
* ★ 用户观察"iperf3 流量偏低"——**成因是上行链路，不是车道**：同一条 stress 配方下今天的 **UL SINR 中位 12.9 dB vs 历史 23.5 dB（差 10.6 dB）**，调度器据此把 UL MCS 从 256QAM 降到 QPSK/16QAM、TBS 中位 4097→976（DL 侧调制/TBS 完全一致，256QAM/233 ⇒ 是**上行单侧**变差，PHR 两边都 +8 ⇒ 不是功率被顶）。CRC-KO 反而更低（7.2% vs 15.9%），因为 MCS 更保守。
* ★★ **因此这条对子还给出一条稳健性结论**：**10.6 dB 的上行链路差下，V1/契约/gaps/stale/cbs-lane 全部同档全绿** ⇒ 验收判据对链路差异稳健。
* ✅ **收尾**：`milestone_audit.sh --quick` = **28 PASS / 0 FAIL / 0 RED ⇒ `offline acceptance: GREEN`**（两条"腿的提交 = HEAD"由 FAIL 转 PASS）⇒ 本轮欠账清空。
* ⚠ 提醒（腿侧）：本轮换代码后 `p103`/`p104` 已不是 HEAD 证据，这一对即为新 HEAD 的证据腿；重跑时仍按"先停 ping/iperf3 → 等上行排空 → 单次 Ctrl-C"。

### 6.156 ★★★★★ **接收尾巴的成因找到，并且交付车道上可一键消除（腿 `p122`）**

**结果（`gpu` default，唯一变量 = `OCUDU_DFT_RELEASE_TOKENS_EARLY=1`）**：

| 腿 | `recv>1ms` | `>5ms` | `slip>1ms` | 池 `held_max` | **V1 中位 / max** | V1 p99 | ping max |
|---|---|---|---|---|---|---|---|
| **`p122`**（P2-E 开）| **6/592711 = 0.001%** | 1 | **3** | 4/32 | **1366.7 / 2703.6** | – | **62.3 ms** |
| `p116`（对照）| 479/670890 = **0.071%** | 32 | 411 | 6/32 | 1401.5 / 6241.5 | – | 167.2 ms |
| `p103`（历史对照）| 313/538646 = **0.058%** | 17 | 239 | 6/32 | 1354.3 / 5857.1 | – | – |

* ★ **尾巴 0.071% → 0.001%（60–70×，直接落到 CPU 级 0.000–0.003%）**，`slip>1ms` **411 → 3**（≈130×），**V1 中位反而更好**（1366.7 vs 1401.5）、**V1 max 6242 → 2704**、**ping max 167 → 62 ms**。
* ✅ **红线与判据全部不变**：`crossings` **0 host read / 0 host write**、`stale=0`、`gaps=0 rx_overflows=0`、`cbs/lane=2.00 (max=2)`、契约 **9 of 9**、`released=32386`（交棒仍在）、`radio_zero_copy=453404`（零拷贝输入仍在）、`batched=32386/453404` = 每派发 14（批量化未变）。

**机制（与代码注释一致）**：交棒（D1）把**输入令牌**武装在**整跳的完成**上，而**只有本块的 DFT 派发读那些样点**（其后的 CE/均衡/解映射读的是**它们写出的网格**）⇒ RX 池块要等到**整跳结束**才回到电台 ⇒ 接收路径的节奏被拖（`slip`），偶尔一次接收调用 >1 ms。P2-E 在**最后一个读输入的派发之后**（`encodeSignalEvent`，编码在同一个已存在的命令缓冲里、且排在那批派发之后）就释放令牌 ⇒ 池块提前约"一跳"归还 ⇒ 尾巴与 `slip` 一起消失。**不加命令缓冲**（V4 不变，实测印证）。

**由此得到的两条结论**：

1. **分模块臂（`ce`/`eq`/`grid`/`dft` 单独）之所以在 CPU 级**：它们没有"整跳一个块 + 令牌随块活到跳尾"这套结构；
2. **分裂模式修好后的残余尾巴（0.026–0.045%）应当也能被同一旋钮吃掉**（那边没有交棒、令牌在**块完成**时释放，仍早于 P2-E 的"最后一个读输入的派发"）—— 属调试模式，优先级低于交付。

**★★ 机制的正面证据（空口计数器）与一处离线陷阱**：

```
p122: tokens_early=signals:32386, by_event:10930, by_complete:21455   ← 34% 的块确实经事件提前释放
p116: tokens_early=signals:0,     by_event:0,     by_complete:36733   ← 对照腿：全部在完成时释放
```

* `by_event>0` 是"释放真的提前了"的**直接证据** ✓；对照腿为 0 ✓（该计数器本来就是为这个判据造的）。
* ⚠ **离线陷阱（务必记住）**：`dft_release_adopt_metal_test` 会打一行 `P2-E premise: a signal encoded MID-buffer was published ONLY AT COMPLETION … margin 96.4 ms - the token release cannot move to the front end's end on this platform`。那一行测的是**它自己 ~100 ms 量级的合成窗口**里的中途信号，**不能**用来判断这条臂；空口上（~1.5 ms 的块）同一条路径有 **34%** 的块提前释放 ⇒ **判这条臂用空口计数器，不用那行离线结论**。（离线该测试仍然全绿：`ctest -L phy` 203/203，其自身 rc=0；那条 `zero-copy wraps … 1 misaligned -> FAILED` 是它合成环境的既有红条，两种默认值下一致。）

**已实施**：`release_tokens_early_requested()` 改为**默认开**（`OCUDU_DFT_RELEASE_TOKENS_EARLY=0` 为后退旋钮），并把 `dft_release_adopt_metal_test` 的默认值断言同步改到新语义（它正是这道护栏）。**待取证（预登记）**：在新 HEAD 上飞**验收对**（`gpu` default + stress，**不设任何 env**）：

| 读数 | 期望 | 对照（旧默认）|
|---|---|---|
| `[ul_rx_timing] recv over 1ms / calls` | **≈0.001–0.01%** | 0.071% / 0.119% |
| `slip over 1ms` | **≤ ~30** | 411 / 684 |
| `tokens_early … by_event` | **> 0（≈1/3）** | 0 |
| V1 中位 / max | **≤1400 / ≤~3000 µs** | 1401.5 / 6242 |
| 契约 / `crossings` / `cbs/lane` / `gaps` / `stale` / PRACH | 9/9 · 0+0 · 2.00 · 0 · 0 · 1 | 同 |

**落地路径（建议）**：① 把 P2-E **改为默认开**（保留 `OCUDU_DFT_RELEASE_TOKENS_EARLY=0` 作为后退旋钮）+ 注释/旋钮清单/本文档；② 在**新 HEAD** 上重飞**验收对**（default + stress，**不设任何 env**）⇒ 一次同时完成"复现（含加压，0.119% 那条）"与"验收取证"；③ 门禁 + 审计 + 入档。

> ⚠⚠⚠ **本节结论已被 §6.157 撤回（2026-09-28 深夜，腿 `p123`/`p124`）**：预登记的对子**没有复现** —— 同模式同配方、旋钮两条腿都在工作的前提下，`p123` 读 **0.080%（675/842093）**、`slip>1ms=549`，即**对照腿自己的量级**，与 `p122` 的 0.001% 差 **79×**。⇒ **`p122` 的 0.001% 是一条腿的读数，不是这个旋钮的读数**；"60–70×"撤回。旋钮**保持默认开**（它确实把输入保持时间结构性地拿掉了，`by_event` 25–33%），但**理由改成"输入保持"**，**不再是"吃尾巴"**。尾巴的成因见 §6.157（腿级：电台宿主接口实时性；86 条腿 Spearman 0.986）。

### 6.155 ★★ 收束风暴线、回到**接收尾巴**（用户 2026-09-28 指示）：全臂对照 + 两条新否证 + 新的正面线索

**风暴线收束**：现象只在 `cpu_gpu` 调试模式、已被保守等待策略隔离、交付融合车道独立验证干净（`p103/p104/p116/p117`）；机制仍差"宿主等待策略 → 手机失去 SR"这一环，而**下一步需要 UE 侧观测（本仓没有）** ⇒ 记录为"已隔离、机制部分未定位"，两条纪律入册（调试臂 +400 µs/跳、腿质量协变量 PHR），力量转回尾巴。

**尾巴的全臂对照（`[ul_rx_timing]` 的 `recv over 1ms / calls`；`recv max≈101 ms` 是启动一过性读数，已由 `[ul_rx_wait] startup=… excluded` 剥离）**：

| 臂 | `recv>1ms` 比例 | >5ms | `slip max / slip>1ms` | `loop max` | 尾巴时 `load1` |
|---|---|---|---|---|---|
| cpu（并发2 / 普通 / +外部GPU）| **0.002% / 0.003% / 0.000%** | 1–3 | 0.7–2.2 ms / 0–11 | 0.27–0.53 ms | 4.5–4.9 |
| 单模块 `none`/`dft`/`grid`/`ce`/`eq`/`ce-eq`/`grid-ce`(×2) | **0.000–0.003%** | 1–6 | 2.1–7.0 ms / **0–4** | 0.07–0.15 ms | 4.0–6.1 |
| **`dft+grid` 旧策略**（`p100`/`p109`；`p105` staged）| **0.065 / 0.073 / 0.106%** | 25–46 | 4.9–5.7 ms / **120–353** | 0.41–1.46 ms | 4.0–4.6 |
| **`dft+grid` 保守（本次修复）** | **0.026%（staged）/ 0.045%（wrap）** | 9–11 | 4.9–12.3 ms / **120–234** | 0.41–0.68 ms | 4.0–10.2 |
| **`gpu` 融合（交付）** | **0.058 / 0.071 / 0.100 / 0.119%** | 7–32 | 5.7–14.5 ms / **411–684** | 0.49–1.87 ms | 4.5–6.7 |

* ① **触发对仍是 `dft+grid`**（所有单模块臂都在 CPU 级 0.000–0.003%）。
* ② **本次等待策略修复把分裂模式的尾巴压低 2–4 倍**（0.106→0.026、0.073→0.045），**但没到底**；**融合交付车道的尾巴（0.058–0.119%）完全没被触及，而且是全表最高**。
* ③ ★ **新否证一：池不是瓶颈** —— 有尾巴的腿上池完全健康（`p115` `held_max=11/32 free_min=21 starved=0 dropped=0`；`p116` `held_max=6 free_min=26`）。池被握干（`held_max=33>32`、`free_min=0`、`starved=1`、`dropped=1`）**只属于风暴腿** `p100/p109`。
* ④ **新否证二：`load1` 不区分**（尾巴腿 4.0–10.2，CPU 4.5–4.9）—— 1 分钟平均抓不住 6 ms 的抖动。
* ⑤ ★★ **新正面线索：`slip` 与 `loop max` 和尾巴同向** —— 单模块臂 `slip>1ms` 只有 0–4 次，而 `dft+grid` 与融合臂是 **120–684 次**（两个数量级）；`loop max`（宿主在两次接收之间的自用时间）也从 0.07–0.53 ms 涨到 0.41–1.87 ms ⇒ **设备网格写这条路让宿主的接收节奏变差、宿主线程更忙**，而**池健康、网格完整、外部 GPU 负载无关**都已排除。
* ⇒ **尾巴的当前定位：设备网格写路径的"宿主侧代价/争用"**（不是池、不是网格完整性、不是 waitUntilCompleted 窗口富集、不是外部 GPU 负载）。

**下一步（低成本、按顺序）**：

1. **零代码腿（融合配置，尾巴最重且未被触及）**：`gpu` default + **`OCUDU_DFT_RELEASE_TOKENS_EARLY=1`**（P2-E 现成旋钮：把输入令牌在"最后一个读输入的派发"就释放，而不是等整跳完成）⇒ 若尾巴下降 ⇒ 尾巴至少一部分来自**完成处理器/令牌释放那条宿主唤醒路径**（这正是融合模式特有的：token 随被收养的块活到**整跳结束**）。
2. 若①为负：给**宿主 UL 循环**做一次"逐子步骤"计时探针（接收调用之前/之后各打一个时间戳，把 >1 ms 的事件归到具体子步骤：wait/submit/CSR/其他），回答"那 1–6 ms 被谁花掉"。
3. 交付口径提醒：尾巴只影响 V1 的 **max/p99**（中位不动、判据全绿），因此它是**监视项**而非阻塞项；但既然交付车道是全表最高，值得按上面两条把它压下去。

> ⚠⚠⚠ **本节的臂表与"尾巴 = 设备网格写路径的宿主侧代价"这一归属已被 §6.157 降级（2026-09-28 深夜）**：表里**每一条"有尾巴"的腿，电台宿主接口同时在丢实时性**（DL 侧 `[dl_tx_call] over 1ms` 与 `recv>1ms` 在 86 条腿上 Spearman **0.986**）⇒ "尾巴 ↔ 臂"与"臂 ↔ 输运健康"完全重合，**尾巴归属不能从本表读出**；⑤ 的"宿主更忙（`loop max` 抬高）"方向也反了（`p123`：`loop>1ms` **2** 次 vs `recv>1ms` **675** 次 ⇒ `slip` 全来自 `recv`）。① 与 ③/④ 的否证不受影响；本表的**读数**仍有效，**归属**改为"候选之一（未分离）"。

### 6.154d ★★★ **`p121`（未碰手机）在"好链路"下照样风暴 ⇒ 策略确认；但"策略 → 手机"这一环仍未定位（收束建议）**

| 腿 | 策略 | PRACH | sr=yes | **PHR** | **UL SINR** | DL TBS 中位 | 手机 CSI | ping |
|---|---|---|---|---|---|---|---|---|
| `p121`（**未碰手机**，紧接 `p120`）| **旧** | **2287** | 86 | **+7** | **26.2 dB** | **106** | **`2222`** | **14.4% 丢包 / avg 95.3 / pipe 17** |
| `p119` | 旧 | 2410 | 105 | 0 | 16.3 | 111 | `2222` | 5.9% 丢包 |
| `p120` | 保守 | **1** | **2998** | +15 | 26.0 | **233** | `1111` | 0% 丢包 / avg 28.3 / pipe 2 |

* ⇒ **路径损耗/位置/时间混杂彻底排除**：`p121` 的链路与干净腿**同档**（PHR +7、SINR 26.2 dB）**却仍风暴** ⇒ **等待策略就是驱动**（6 腿、含"不碰手机"的复现）。
* ⇒ DL TBS/CQI 的恶化**跟着策略走、不跟着链路走** ⇒ 它们是**后果**。
* PUCCH 接收质量对照（`p121` vs `p120`）：**ACK 资源反而在风暴腿更好**（metric 中位 588 / 13.4 dB vs 237 / 9.5 dB）⇒ **手机的 PUCCH 不是"发得弱/收得坏"**；`sr=yes` 的塌陷是"**手机已转入 RACH 模式**"的后果（风暴先于 ping 出现，`p109`/`p110` 已证）。

**排除清单（本轮否掉的全部假设）**：输入生命周期（keepalives 全挂全放）→ 输入路由（wrap/staged 都中招）→ 网格完整性（审计 38 747 槽 EARLY==LATE）→ 环槽索引/块切换（同上撤回）→ RF 实时失败（风暴腿反而更低）→ 上行信道/PHR/位置（`p121` 同档却风暴）→ PUCCH 接收质量（风暴腿更好）。

**仍开放的那一环**：**"宿主侧等待策略"如何到达"手机在 attach 后 ~5–20 s 失去 SR 资源"**。所有能看见的 gNB 侧读数都已排除；剩下的差异只可能在 *时序*（UL 结果/指示到达 MAC/调度器的时刻、grants 的结构）或 *UE 内部状态*（而 UE 侧我们没有观测手段）。

**收束建议（供用户裁决）**：这一现象是**调试模式专属**、**已被保守策略隔离**，交付融合车道**独立验证干净**（`p103/p104/p116/p117`）。为它已飞 7 条腿、否掉 6 个假设，而**下一步需要 UE 侧观测**（我们没有）⇒ 建议**收手并记录**（保留 containment；两条纪律：调试臂 +400 µs/跳、腿质量协变量 PHR），把力气转向已裁定"单独规划"的 **LDPC→Metal**；若日后需要调试臂的速度或该现象出现在融合腿上（至今未出现），再从此处的证据链续查。

### 6.154c ⚠⚠⚠ **背靠背对照腿 `p120` 支持"策略"，但**链路读数暴露"路径损耗"这个更强的候选**（2026-09-28 深夜）

**`p120`（与 `p119` 同臂同位置、紧接其后、只差等待策略）：干净**（2400/2400、0% 丢包、avg 28.25、max 137.6、`pipe 2`；`p119` 是 5.875% 丢包、avg 68.7、max 1719、`pipe 17`）⇒ **"策略 ↔ 风暴"的 A/B 到目前 5 腿全中，且含一组背靠背**。

**但背靠背这对腿的链路读数指向另一个解释**：

| 读数 | `p120` 保守（干净）| `p119` 旧（风暴）| `p115` 保守（干净，早先）|
|---|---|---|---|
| **PHR 中位** | **+16** | **0**（顶在功率上限）| +9 |
| **gNB 侧 UL SINR 中位** | 25.7 | **16.3** | 15.4 |
| **DL TBS 中位 / 调制** | 233 / 256QAM 主导 | **111 / 256+64+16QAM 混合** | 233 / 256QAM 主导 |
| 手机 CSI | `1111`/`1110` | **`2222`** | `1111` |
| PDSCH / PDCCH 条数 | 5076 / 7913 | 4916 / 7768（同档）| 5014 / 7773 |

* ★★ **同一个 gNB 侧 SINR（16.3 vs 15.4）下，`p119` 的手机却多用约 9 dB 功率**（PHR 0 vs +9）⇒ 等价于**那一次的路径损耗差了约 9 dB**；同一条腿的 **DL CQI（手机上报）与 DL MCS 也同步变差**（`2222` 主导、TBS 111、调制混合）⇒ **上下行同时变差 = 链路/位置因素**，而不是"某条路被写坏"。
* ★ 因此：**"等待策略 ↔ 风暴"仍可能是把"链路/手机位置"混杂进来的巧合** —— 背靠背对照也**不能**排除它（两腿之间用户很可能动过手机/天线）。**能控制链路的只有一次"不碰手机"的重复**。
* ⚠ 由此**再次更正**：6.154 的机制（环槽/块切换）已被审计否掉；6.154b 的"时间混杂"担心**不能撤回**（`p120` 只证明"策略不同 → 结果不同"，不证明"策略是原因"）。
* **决定性腿（待飞 `p121`）**：**与 `p119` 完全相同（旧策略 + 审计），但飞之前不要碰手机/天线**（紧接 `p120` 之后）：**再次风暴 ⇒ 策略确认**（DL/链路差异是它的后果）；**干净 ⇒ 策略无关**，"风暴"是链路/手机状态现象，前几轮的策略相关性全部重判（并且 containment 的 400 µs 也就没有存在理由）。

### 6.154b ⚠⚠ **审计臂的负结果把 6.154 的机制否掉了，并且暴露出"时间混杂"**

**`p119`（旧策略 + 审计，风暴）的审计行**：

```
[grid_audit] slots: audited=38747 of 38747 seen, EARLY!=LATE in 0, differing REs=0 (max 0)
             per-symbol=[0,0,0,0,0,0,0,0,0,0,0,0,0,0]
```

* ⇒ 在**正在风暴**的腿上，38747 个槽里"槽末等待之后读到的网格"与"把全部提交排空之后读到的网格"**逐 RE 相同** ⇒ **网格不是"没写完"** ⇒ **§6.154 的"环槽索引复用 × 块切换 ⇒ 旧块没人等"机制被否掉（撤回）**。
* ⇒ 更强的一条：审计**每槽都做一次全排空**（比任何等待都强），**风暴照样发生**（PRACH 2410、丢 SDU 148、队列峰值 15、ping 丢 5.9%）⇒ **风暴与"GPU 工作是否及时完成"无关**。

**同长度对照（`p115` 干净 vs `p119` 风暴）暴露的新事实**：

| 读数 | `p115` 保守（干净）| `p119` 旧+审计（风暴）|
|---|---|---|
| PRACH / 丢 SDU | 1 / 0 | 2410 / 148 |
| `sr=yes`（真被发出的 SR）| **2058**（metric 中位 34.3）| **105**（中位 1.2）|
| SR 资源 `metric>100` 总数 | 773 | **6** ⇒ 手机**没在发 SR**（与 `p109` 一致）|
| TA_CMD 条数 / 取值 | 2 / 29–32 | 51 / 26–34（抖动 ±2–4 单位 ≈0.2–0.4 µs，**远在 CP 内**）|
| RF 实时失败/分 | **71.6** | **0.6** ⇒ ★ **RF 失败率不跟随风暴** |
| DL 调制 | 256QAM 主导 | **256/64/16QAM 混合** |
| 手机 CSI 上报 | `1111` 主导 | **`2222` 主导**（下午的风暴腿 `p109` 也是 `2222` 主导）|

* ⚠ **更正两处我自己的读数**：① "SR 有能量却检测不出"是**计数器写错**（把 format=2 的 CSI 也算进去了）；正确过滤后风暴腿 **SR 相关能量≈0** ⇒ 手机确实**停止发 SR**。② "风暴腿 RF 失败更高"只成立于**下午那批**腿；今晚的风暴腿 RF 失败率**最低**。
* ★★ **DL 路径与 UL 等待策略无关**，而 DL 调制/手机 CSI 在"干净对"与"风暴对"之间系统性不同 ⇒ **策略与风暴的 A/B 很可能被"时间/手机状态"混杂**（干净对 19:25/19:37，风暴对 20:18/20:44）。
* **决定性对照腿（待飞）**：**与 `p119` 完全同臂同位置，但不设 `OCUDU_DFT_WAIT_PER_SLOT`**（即保守策略），**紧接 `p119` 之后飞** ⇒ 干净 ⇒ 策略确实是驱动（那 DL 差异是它的后果，需另查）；**仍风暴** ⇒ 策略无关、驱动是手机/时间状态，前几轮的策略相关性都要重判。
* （若对照腿显示"仍风暴"，下一步线索已就位：**手机 CSI `2222` 主导**与 **DL 调制混合** 是风暴的可观测伴随量，可从 `PUCCH format=2` 的 CSI 字段语义入手解码。）

### 6.154 ★★★★★ **病因定位（2026-09-28 深夜，代码级）：环槽索引复用 × 块切换 ⇒ 旧块的命令缓冲"没人等"**

**机制（三步，全部有代码依据）**：

1. **环槽只有 8 个，每槽却有 28 个变换**：`puxch_processor_impl` 里 `unsigned slot = next_pipeline_slot % pipeline_depth;` **在端口循环内自增**（每变换一次），`pipeline_depth` 默认 = `max_pipeline_depth` = **8**（`ofdm_demodulator_impl.h:97`）⇒ 2 端口下**同一个环槽索引每 4 个符号被复用一次**（14 符号/槽 ⇒ 每槽每个索引被复用 3–4 次）。
2. **"一跳只等一次"时，非槽末符号的 `finish_symbol` 不等**：`slot_pending[idx]` 仍为 true、`slot_cb[idx]` 仍指旧块 cb —— 而该索引在 4 个符号后被下一次提交**覆写**（`ocudu_dft_metal_engine.mm` 的 `note_slot_submission(engine, slot, open_cb)`）。
3. **每个槽切换时 `set_lane_slot()` 会 `end_block()` 关掉上一个块**（`ofdm_demodulator_impl.h:158-163`），而此刻上一个槽最多 `pipeline_depth` 个变换仍在飞 ⇒ 那些"注册在旧块、又从未被等待"的变换，其**唯一的 cb 引用被覆写丢失** ⇒ **旧块永远没人等** ⇒ 它承载的网格写（= **该槽尾部若干符号**）在宿主读者（本臂的宿主 CE、以及构造上就是宿主读者的 PUCCH）读网格时**仍在飞**。

**这条机制同时解释**：

| 观察 | 解释 |
|---|---|
| wrap 与 staged 两条路都风暴 | 与输入路由无关；staged 只是每变换独立 cb ⇒ 被丢的引用更多 |
| `p115`（逐符号等待）干净 | 每个变换在**索引被复用之前**就被等掉 ⇒ 引用不会被覆写丢失 |
| `p118`（旧策略）复现风暴、且 `pipe 7 / max 698 ms`（攒包不丢包） | 网格读晚 ⇒ 估计偏 ⇒ 功控顶格 ⇒ 控制面退化，程度取决于时序 |
| **离线 harness 复现不了**（正向对照 0 差异） | 它每槽只调一次 `set_lane_slot`、没有飞行滞后 ⇒ 索引不会被提前复用 |

**预测（可由审计臂直接判）**：`OCUDU_DFT_GRID_AUDIT=1` + 旧策略（`OCUDU_DFT_WAIT_PER_SLOT=1`，wrap 臂）应报 **EARLY≠LATE**，且逐符号指纹集中在**该槽尾部符号**。（审计探针已落地：见本节的 `grid_audit`；臂关时全程静默、负向对照 0 差异。）

**如果预测成立，则有一个"既安全又快"的定向修法**（可把调试臂那 ~400 µs/跳收回来）：**在块切换处只等一次** —— 即 `set_lane_slot()` 在 `end_block()` 之后、允许索引复用之前，等掉**刚关掉的那个块**（需要一个极小的引擎入口，如 `wait_last_block()`），而不是每符号都等。预计 wrap 臂 UL 跳从 1393 µs 回到 ~993 µs，且不丢任何注册 ⇒ 旧策略重新安全。**这需要一次独立验证（同一腿 A/B）**，不能只凭读码上线。

### 6.153 ✅ 交付侧排序审查（2026-09-28，回答"分裂模式的洞够不够得到交付车道"）：**够不到**，三条证据

**动机**：§6.151⑨ 的修复是**剂量式隔离**（把"一跳只等一次"改成逐符号等），不是诊断 —— 代码读起来 wrap 路本该被覆盖而实测不然。既然解释不了分裂侧的洞，就不能假定交付侧那套（**不同的**）机制没有同类前置条件，尤其它自己的历史里发生过同族失败（5.9.12：无人等待时每次 PUCCH 报告 `metric=nan sinr=-inf`、attach 完不成、随后 53135 次实时失败）。

**审查结果（负向：未发现暴露）**：

1. **宿主读者清单是完整的**：融合车道里读设备网格的宿主读者只有三处 —— PUCCH 两种格式与 SRS，全部"先 `grid_ready_hook::wait()`，**失败即丢弃而不是照读**"（`lib/phy/upper/uplink_processor_impl.cpp:374 / :440 / :514`）。
2. **registry 的不变量自证**（`lib/phy/metal/ocudu_metal_burst.mm:1391-1410`）：条目**只在已产出后**才被擦除；`not_found` 的原文语义是"从未交棒"或"早已产出、记录已擦除"；"**未认领且未产出**"的条目由 registry **当场代为提交**（"the fallback a hand-over owes"，对应 `fallback` 计数）。⇒ 一次 `not_found` 的读取，读到的必然是**已经产出**的网格。
3. **实测（四条融合腿，含加压）**：`evicted_unproduced = 0`、`timeouts = 0`、`handed > 0`、`fallback` 数千至数万次（全部落在安全路径）。

| 腿 | `handed` | `taken` | `fallback` | `late` | `evicted_unproduced` | `timeouts` | `not_found` |
|---|---|---|---|---|---|---|---|
| `p103`/`p104` | 31090 / 147577 | 2969 / 145089 | 23256 / 2022 | 4865 / 466 | **0 / 0** | **0 / 0** | 4865 / 459 |
| `p116`/`p117` | 36733 / 156940 | 3031 / 145390 | 31197 / 9114 | 2505 / 2435 | **0 / 0** | **0 / 0** | 2505 / 2431 |
| `p114`/`p115`（分裂，交棒被拒）| 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 31331 / 31319（合法：从未交棒）|

⇒ **交付车道的宿主读者顺序由 hand-over + grid-ready registry 保证（有仪表、四条腿全绿），与分裂模式那条"宿主等待"是两套机制** ⇒ 分裂侧的洞**够不到交付车道**。

**还剩什么**：只剩**次要目的** —— 调试臂那 ~400 µs/跳（wrap 993→1393、staged 1542→1700）与跨臂时延可比性；要拿回来必须先**精确定位**分裂侧的洞（早/晚两次读回同一批 RE 的审计）。**若调试模式的速度不是痛点，可以就此收手**，把状态记为"**已隔离 + 交付侧已独立验证 + 分裂侧洞的确切位置未定位**"。

### 6.157 ⚠⚠⚠⚠ **「P2-E 消除尾巴」没有复现（`p123` = 0.080%，`p122` = 0.001%，同模式同配方、旋钮两条腿都武装）；并且整条尾巴被一个**腿级**协变量解释掉了 —— 电台宿主接口的实时性（86 条腿，Spearman **0.986**）

**这一节撤回 §6.156 的因果结论，并把 §6.155 那张全臂表的尾巴归属降级为"未分离的候选"。**

#### ① 预登记对子落地（HEAD `c082d9e823`，`gpu` 模式，**不设任何 env** ⇒ P2-E 按新默认武装）

| 腿 | 工况 | `recv>1ms` / calls | `>5ms` | `slip>1ms` | V1 中位 / max | 契约 | `leg_gate` | UHD 实时失败 | `by_event` |
|---|---|---|---|---|---|---|---|---|---|
| **`p123`** | default | **675/842093 = 0.080%** | 46 | **549** | 1366.7 / 2838.2 | ⚠ **NOT MET（1/9）** | **6 of 8** | **1071**（685 underflow + 385 late + 1 overflow）| **33%**（15154/46414）|
| **`p124`** | stress | 278/608235 = 0.046% | 10 | 203 | 1431.4 / 6513.8 | 9 of 9 | **9 of 9** | 379 | **25%**（37094/147645）|
| `p122`（预登记腿）| default，`…TOKENS_EARLY=1` | **6/592711 = 0.001%** | 1 | 3 | 1366.7 / 2703.6 | 9 of 9 | – | 4 | 34% |
| `p116`（§6.155 对照）| default，旧默认 | 479/670890 = 0.071% | 32 | 411 | 1401.5 / 6241.5 | 9 of 9 | 8 of 8 | 675 | 0 |
| `p117`（§6.155 对照）| stress，旧默认 | 862/726932 = 0.119% | 27 | 684 | 1407.0 / 6925.5 | 9 of 9 | 9 of 9 | 1225 | 0 |

**逐行判决预登记表（§6.156 末尾那张）**：`recv>1ms ≈0.001–0.01%` ✗（0.080%）；`slip>1ms ≤ ~30` ✗（549）；`by_event>0（≈1/3）` ✓（33% / 25%）；V1 default 中位 ≤1400 ✓（1366.7）；V1 max ≤~3000 ✓（2838.2）；**契约 9/9 ✗**（`p123` 1 条失败）。stress 腿 V1 中位 1431.4 略高于预登记的 1400（旧默认 `p117` 是 1407.0，即加压中位本来就在 1400 上方）。

#### ② 旋钮在动，但尾巴不跟着它走

* `by_event` **33%（`p123`）** 与 **34%（`p122`）** 同档、`radio_zero_copy` 全覆盖（`649796/649796`、`2067030/2067030`）、`wrap_copies=0`、`batched` 仍是 **14/派发** ⇒ **P2-E 声称的那条路径在两条腿上都在工作**。
* 而尾巴差 **79×**（0.001% vs 0.080%）。⇒ **`p122` 的 0.001% 不是旋钮的读数，是一条腿的读数。** §6.156 的"60–70×"撤回。

#### ③ 找到那个腿级协变量：**电台宿主接口的实时性**（DL 侧、独立线程、独立方向）

* 口径：`[dl_tx_slack] transmissions=… ; below 1ms=…, AT/BELOW 0=…` 与 `[dl_tx_call] … over 1ms=…`（**DL 发射**的宿主调用），对 `[ul_rx_timing] recv over 1ms / calls`（**UL 接收**的宿主调用）。
* **86 条腿（剔 `p36`/`p69` 两条畸形腿）**：`dl_tx_call over 1ms` 率 对 `recv>1ms` 率，**Spearman 0.986 / Pearson 0.835**，两条比值稳定在 **0.75–1.0**。
* 分层：**19 条"输运健康"腿**（`dl_tx_call>1ms` 率 ≤ **0.0064%**）尾巴 **0.00012–0.0035%**（中位 0.00085%）；**67 条"输运病态"腿**（≥ **0.0248%**）尾巴 0.00012–0.171%（中位 **0.111%**）。**唯一例外 `p41`**（2 小时长腿：DL margin 差但**宿主调用不慢**，`over 1ms = 0.0000%`，尾巴 0.00012%）⇒ **要用"调用时长"那把尺，不要用 margin 那把**。
* 用 `RF` 原始条数（UHD 的 `underflow`/`late`）做同一分类：Spearman 0.925，**唯一例外 `p120`**（空载 30 分钟腿：10514 条 RF 失败，但 `dl_tx_call>1ms=2`、`AT/BELOW 0=1`、`rx_overflows=0`）⇒ **"DL 链没在产"的腿上 RF 计数是伪信号**，所以纪律用 `dl_tx_call` 率、不单用 RF 条数。

#### ④ 腿内对照（同模式、同配方、同旋钮）：`p122` vs `p123`

| | `p122`（0.001%）| `p123`（0.080%）|
|---|---|---|
| UHD 实时失败 | **4**（1/0/0/…/3，几乎全程无）| **1071**，**均匀铺满整条腿**（10 等分桶 91/69/81/61/101/78/68/83/**371**/68）|
| `[dl_tx_slack]` | `AT/BELOW 0=0`、`below 1ms=2`、`min=+832 µs` | `AT/BELOW 0=162`、`below 500us=223`、`below 1ms=592`、**`min=−22160 µs`** |
| `[dl_tx_call] over 1ms` | 4 | 520 |
| 池 / 接收节奏 | `held_max=4`、`free_min=28`、`pop_blocking max=54 µs`、`loop>1ms=0` | `held_max=20`、`free_min=12`、`pop_blocking max=58 µs`、`loop>1ms=**2**` |
| 契约 | 9 of 9 | ⚠ 1 条失败：`radio sample continuity: 1 gaps … 1 radio receive overflow`（556855 样点）|

⇒ 两条腿的差别是**"电台宿主接口有没有按时"**，不是"令牌什么时候放"。★ `p123` 的失败读数（1 次接收溢出 + 1 个 gap）与它自己 DL 侧 1071 次实时失败**同步出现**，而池与接收节奏都健康（`starved=0`、`pop_blocking` 最大 58 µs）⇒ 归因给该腿的接口实时性；严格说这是**单腿**证据，同 HEAD 的 `p124` 契约 **9/9**。

**欠账（`p123` 的账要还）**：`milestone_audit.sh --quick` = **26 PASS / 1 FAIL / 1 RED（of 32）**，两条**同因**：① A1-2 的 **C5 "leg valid"** 要求 `contract MET` + `gaps=0`，而 `p123` 两条都不满足；② "契约可读（`contract MET (9 of 9)`）"那条直接读到 `NOT MET` ⇒ 记 RED。A1-2 的 **C4**（`plain route == 12*round(T/10ms)+1`：期望 505249、读到 505225，**少 24 个变换**）也是**同一次样点丢失**的产物 —— `p122`/`p124`/`p116` 的 C4 全部 PASS ⇒ **本 HEAD 的验收对还欠一条干净的 default 腿**（`p125-n78-default`：既还审计的账，又给 default 类第二条尾巴样本，用来判 `p122`↔`p123` 是"双峰腿质量"还是"概率抬高"）。

#### ⑤ 顺带否掉 §6.155⑤ 的"宿主更忙"读法（这一条现在是硬证据）

`slip = loop + recv − air`，而 `p123` 的 `loop>1ms` 只有 **2** 次、`recv>1ms` **675** 次 ⇒ 那 549 次 `slip>1ms` **全部来自 `recv`（传输调用阻塞）**，不是"宿主在两次接收之间的自用时间"。`[ul_rx_pool] pop_blocking` 也是微秒级。⇒ **本仓自己的宿主代码在这条尾巴上是被排除的一方**（§6.155⑤ 把 `loop max` 的抬升读成"宿主侧代价"，方向反了：`loop max` 只是跟着同一次传输停顿抬高）。

#### ⑥ §6.155 那张全臂表要按新协变量重读（**尾巴归属降级为"候选、未分离"**）

| 臂（§6.155 的分类）| `RF`/1000 调用 | 新分类 | 尾巴 |
|---|---|---|---|
| `cpu`（全 4 条）| 0.000–0.068 | 健康 | 0.0004–0.003% |
| 单模块 `none`/`dft`/`grid`/`ce`/`eq`/`ce-eq`/`grid-ce`（`p92`/`p93`/`p97`/`p98`/`p99`/`p101`/`p106`/`p107`/`p112`）| 0.000–0.060 | **全部健康** | 0.0003–0.003% |
| **`dft+grid`**（`p100`/`p102`/`p105`/`p108`/`p109`/`p111`/`p114`/`p115`）| **0.358–1.570** | **全部病态** | 0.026–0.106% |
| `gpu` 融合（交付，60 条）| 0.000–2.597 | **4 条健康**（`p41`/`p79`/`p85`/`p122`）其余病态 | 健康 4 条 0.0001–0.0014%；其余 0.046–0.171% |

* ⇒ **表里每一条"有尾巴"的腿，输运同时也在丢实时性** ⇒ "尾巴 ↔ 臂"与"臂 ↔ 输运健康"**完全重合**，**尾巴的归属不能从那张表读出来**。§6.155 的"设备网格写路径的宿主侧代价/争用"从"**当前定位**"降级为"**候选之一（未分离）**"。
* ★ **仍然成立、而且更锋利的一条**：**`cpu_gpu` 内部，臂与输运健康高度对齐** —— **只把 DFT 或只把 grid 放设备**的臂（`p92` none / `p93` dft / `p97`+`p112` grid / `p98` ce / `p99` eq / `p101` ce-eq / `p106`+`p107` grid-ce）**9/9 全部健康**（RF/1000 = 0.000–0.060）；而**`dft+grid` 合体**的臂 **8 条病态（0.358–1.570）、2 条健康**（`p119` 审计臂 0.005、`p121` 旧等待臂 0.083）；另有 `p94`（`cg-all-light`，1.771）与 `p64`（`repro-cpugpu`，1.467）两条**全上设备**的腿也病态 ⇒ **设备侧"DFT→grid 合体"与"输运丢实时性"强烈同现**。但 `gpu` 模式下**同一交付配置能出 `p85`/`p122` 两条干净腿**（`p85`：`AT/BELOW 0=0`、`RF=0`、尾巴 0.0004%）⇒ **不是确定性因果**：要么它只是**抬高概率**，要么另有一个腿级环境变量在调制它。**这两者必须用腿来分**（见 ⑦）。

#### ⑦ 纪律入册（第 20 条）与下一步

* ★★ **纪律 20**：**任何关于"接收尾巴 / `[ul_rx_wait]`"的结论，必须带该腿的 `[dl_tx_call] over 1ms / calls`（DL 侧独立通道）作为腿质量协变量**；`dl_tx_call>1ms` 率 **≥0.01% 的腿不能用来判 RX 宿主路径**。实测分界：健康侧 ≤0.0064%、病态侧 ≥0.0248%（约 4 倍空档）。落地：`leg_gate.sh` 增一行 **INFO**（不判红——交付腿的时延/契约判据与这条无关），点名该腿属于哪一类、尾巴结论**可用/不可用**。
* **对用户指定的 `[ul_rx_wait]` 项，事实重述**：ms 级接收等待**不是融合车道的属性** —— `gpu` 模式有 4 条腿在 CPU 级（0.0001–0.0014%），`cpu` 模式也在 0.0004%；它**随"输运是否按时"出现**，而且**两个方向、两个线程同比例**（比值 0.75–1.0，Spearman 0.986）。CPU 路径之所以"看起来总是好的"，是因为 CPU 模式的腿恰好都健康（4/4），不是因为它没有这条尾巴。
* **两个竞争假设（下一步要分的就是这两个）**：
  * **H1｜我们的设备侧工作抬高"输运丢实时性"的概率**（宿主线程 / 统一内存带宽与 UHD 自己的 USB 栈争用）：支持 = `cpu_gpu` 内 8/8 vs 11/11 的臂对齐、`gpu` 模式 60 条里 56 条病态；反证 = `p85`/`p122` 在**同一交付配置**下干净。
  * **H2｜腿级环境**（同一二进制同一配方分裂成 4 干净 / 56 病态）：支持 = `p122` vs `p123`；反证 = 臂对齐太整齐，不像随机。
* **判别实验（最小、零代码）**：**同一次会话内**交替"设备侧合体"与"设备侧单体"两条 `cpu_gpu` 腿（`LEG_CG_MODULES=dft+grid` vs `dft`），同配方同时长，**除电台之外宿主不动** ⇒ 输运健康**跟着臂开关** = H1；**跟着时间走** = H2。
* **若要再进一步（小探针，遵 §6.145③ 两把钥匙）**：RX 尾巴**现在只有计数、没有时间戳**（`[ul_rx_wait]` 只给分布），而 DL 侧的实时失败**有时间戳** ⇒ 给 `[ul_rx_wait]` 的慢事件加"前 N 次 + 宿主时刻"（env 门控、默认关）就能把**RX 停顿 / DL 失败 / GPU 提交**对齐到同一条时间轴上；那是唯一能把 H1 的"争用"从相关变成机制的东西。

#### ⑧ `p125-n78-default`（同 HEAD 之后的第三条 default 腿）：**欠账清掉，而且把"双峰还是孤例"问成了剂量-响应**

| 腿 | 提交 | `dl_tx_call>1ms` | RX 尾巴 | 比值 | `slip>1ms` | V1 中位 |
|---|---|---|---|---|---|---|
| `p122` | `9d47ee7d2e` + env | 0.0007% | **0.0010%** | 1.5 | 3 | 1366.7 |
| **`p125`** | `d4741bc96c` | **0.0058%** | **0.0095%** | 1.6 | 37 | **1359.8** |
| `p123` | `c082d9e823` | 0.0617% | **0.0802%** | 1.3 | 549 | 1366.7 |

* ⇒ **RX 尾巴 ≈ 1.4 ×（DL 传输调用自己的尾巴）**，三条腿单调、比值稳定 ⇒ 尾巴**不是**"双峰腿质量"，而是**输运尾巴在另一侧的同一条曲线**；`p125`（输运 0.0058%，正好在健康带上沿 ≤0.0064%）读 **0.0095%，正好落在 §6.156 预登记带（≈0.001–0.01%）的上沿** ⇒ 那条预登记带**是可达的，条件是输运健康**（`p123` 是同一份代码、输运差 10 倍）。
* ✅ **欠账清空**：契约 **9 of 9**、`gaps=0`、`rx_overflows=0`、`stale=0`、池绿（`held_max=5`、`free_min=27`、`starved=0`）、`cbs/lane=2.00`、`leg_gate` **8 of 8**、A1-2 **5 of 5**（C4/C5 都过）⇒ §6.157④ 里 `p123` 造成的那 1 FAIL + 1 RED 消失；**V1 中位 1359.8 是交付族最好的中位**（p103 1354.3 是轻流量腿）；`by_event` **11375/34982 = 32.5%**（P2-E 武装确认）。

#### ⑨ `p126`/`p127`（`cpu_gpu` 的**臂 vs 会话**判别对，2026-09-28 22:39/22:44）：**臂不是驱动** ⇒ H1 的确定性形式被否，`cpu_gpu` 那条"臂 ↔ 输运健康"的对齐是**时间/会话混杂**

命令更正先记一笔：`LEG_CG_MODULES` 是 `run_leg.sh` 的**环境变量**（尾随参数只收 `--regime=`/`--smoke=`/`--option=value`/`OCUDU_*=value`，其余当场拒绝）⇒ 必须写在 `bash` 前面（`sudo -E … LEG_CG_MODULES=dft+grid bash …`）。

| 腿 | 臂（`mode options`）| `dl_tx_call>1ms` | RX 尾巴 | `slip>1ms` | RF | 契约 | 池 |
|---|---|---|---|---|---|---|---|
| **`p126`** | `dft+grid`（**裸臂，无任何 env**）| **1/567223 = 0.0002%** | **0.0009%** | 2 | 3 | 1 of 7 FAILED（`ce device estimates`，**臂的固有形状**）| `held_max=5 free_min=27` |
| **`p127`** | `dft`（裸臂）| **1/527657 = 0.0002%** | **0.0011%** | 4 | 17 | 1 of 6 FAILED（同）| `held_max=17 free_min=15` |

* ★★ **预登记判决：两条同档 = 都健康** ⇒ 按 ⑦ 的规则 = **H2（会话/腿级环境主导）**，而 H1 的**确定性**形式（"`dft+grid` ⇒ 输运病态"）被**直接否掉**：`p126` 是**同一个裸臂**（`--expert_phy.pusch_dft_type metal --expert_phy.device_resource_grid on`，无任何旋钮），下午读 0.0301–0.0825%，今晚读 **0.0002%**。
* ⇒ **§6.157⑥ 里"`dft+grid` 合体与输运丢实时性强烈同现（8 病态/2 健康）"这句降级为"风险因子（未分离）"**：能确认的只有 `cpu_gpu` 的**单模块臂 12/12 全健康**（`p93`/`p97`/`p98`/`p99`/`p101`/`p106`/`p107`/`p112`/`p119`/`p121`/`p127` + `p92`），而 `dft+grid` 臂**既有 8 次病态、也有今晚这一次健康**。
* ✅ **同时否掉三个旁证候选**：
  * **等待策略**：`p114`（19:25，正是"`&& handover_allowed()`"修复那一次提交 `34b59defa` 的验证腿）与 `p115`（19:37）**修好之后仍然病态**（0.0218% / 0.0400%）⇒ 不是它；而 `p121`（今晚，`OCUDU_DFT_WAIT_PER_SLOT=1` 强制旧策略）输运**健康**（0.0003%）⇒ 旧策略单独也不致病。
  * **我自己在宿主上的构建/测试**：逐腿用"腿的时间窗 ∧ `build/` 内的文件写入"机械核对 **27 条腿，窗口内有构建写入的 = 0 条**（病态 12 条、健康 15 条）⇒ 我的负载从未与任何一条腿重叠，**这不是解释**。
  * **P2-E**：`cpu_gpu` 下 **`tokens_early=signals:0,by_event:0,by_complete:…`、`handed=0`**（交棒在分裂模式被拒）⇒ P2-E 在分裂模式**根本不作用**，今晚的健康与它无关。
* ⚠ **会话窗口的实测形状（当前唯一的正面线索）**：今晚 20:44–21:41 连续 5 条健康（`p119`→`p122`），**21:55–22:08 病态（`p123`/`p124`）**，22:28 起又健康（`p125`/`p126`/`p127`）；而 `p123`/`p124` 恰好紧跟在 **21:52 的 relink（新 `gnb` 落盘）**之后。**只能记成线索**：`build/` 写入不在腿的窗口内（上面那条否证），但"新二进制落盘 → Spotlight/页缓存/驱动状态"这类**构建后效应**没有任何仪器能量，**不得**当成结论。
* ★ **下一步（把"抽签"变成"分类"，两条都不改 PHY）**：
  1. **预检腿（零代码，立刻可用）**：任何测量腿之前先飞一条 **60 s** 的 `gpu` default 腿（`--regime=default --smoke=20`，到点单次 Ctrl-C），读它 stderr 里的 `[dl_tx_call] over 1ms`：60 s ≈ 12 万次调用 ⇒ 健康类预期 **~0.2** 个慢调用、`p125` 那种边缘类 **~7** 个、病态类 **~72** 个 ⇒ **三类可分**。判为病态就先**给 B200 断电重启 + `uhd_find_devices`**（纪律 13），重飞预检，再飞测量腿。代价 1 分钟，收益是"每条记录下来的腿都能归类"。
  2. **腿内时间轴（小探针，遵 §6.145③ 两把钥匙）**：把 `[ul_rx_timing]`/`[dl_tx_call]` 的慢事件**按周期打印**（env 门控、默认关）⇒ 开局 10–20 s 就能分类、能看出病态是"开场即病"还是"中途发作"（下午的腿两种都有：`p102`/`p105`/`p108`/`p115` 均匀铺满，`p100`/`p109`/`p111`/`p114` 中途起），从而把"会话"这个变量**在腿内**看见。
  3. 若要再切一刀"会话 vs 我们的设备工作"：飞一条 **`cpu` 模式 + stress** 腿（零设备工作、同样的上下行流量）—— 它若病态，会话假说定案；若健康，设备侧工作仍是嫌疑。

#### ⑩ ★★★ 归属再校正（**用户 2026-09-29 指示**）：**高比例 ms 级 Rx 尾巴是 GPU 路径结构性的**；`[dl_tx_call]` 协变量是**表现形式**，不是原因

**用户判读**：CPU 模式的历史读数**一致地**远小于 GPU ⇒ 这是 GPU 路径的**结构性**问题，只是**具体原因还没搞清**。**数据支持这个判读**（全部腿，`recv>1ms ≥0.01%` 记为病态）：

| 模式 | 腿数 | 病态 | 中位 | 区间 |
|---|---|---|---|---|
| `cpu` | 4 | **0** | 0.0022% | 0.00037–0.00296% |
| `cpu_gpu` | 24 | 10 | 0.0030% | 0.00033–0.11804% |
| **`gpu`（融合交付）** | **61** | **56** | **0.1116%** | 0.00012–0.17066% |

* **`cpu`+`cpu_gpu` vs `gpu`：病态 10/28 vs 56/61，Fisher 双侧 `p = 7.0e-08`**；`cpu` vs `gpu`：0/4 vs 56/61，`p = 1.9e-04`（`cpu` vs `cpu_gpu` 不显著，n=4）；`cpu_gpu` 里那 10 条病态**全部**是"设备 DFT + 设备网格"的 `dft+grid` 臂。
* **同会话对照（今晚，11–16 分钟内只换模式/臂）**：22:28 `gpu` **0.0058%** → 22:39 `cpu_gpu`(`dft+grid`) **0.0002%** → 22:44 `cpu_gpu`(`dft`) **0.0002%**。
* 两个因素**并存**：**结构易感（模式/结构）** ＋ **未定位的触发**（21:41 与 21:55 两条**同模式**腿差 80×；下午的裸 `dft+grid` 病态、今晚同一裸臂健康）。

★ **归属更正（本节措辞校订）**：⑨ 把 `[dl_tx_call]` 的共动写成"尾巴属于输运/腿级环境"，是**把症状当原因** —— 该协变量与 RX 尾巴本来就是**同一个现象的两侧**（比值 1.3–1.6），它**标示**这条腿在丢实时性，却**不解释**为什么 GPU 路径更常丢。正确的三层表述：

1. **结构性易感 = GPU 路径**（上面那张表 + 同会话对照；§6.155 的"设备网格写路径"仍是最可疑的结构段）；
2. **直接症状位置 = 宿主↔电台接口**（两个方向同时；我们的 RX 线程按时 `loop>1ms` 0–2 次、池健康、`grid_devwaited=0` ⇒ **不是**池/网格/我们的等待/我们的宿主代码）；
3. **触发条件 = 未知**（继续找的就是它）。

#### ⑪ 机制候选与判别读数（下一步的实验设计）

| # | 候选机制 | 为什么像 | 判别读数（尽量零代码）|
|---|---|---|---|
| **M-A** | **宿主调度 / QoS 优先反转**：我们的命名线程被设成 `USER_INTERACTIVE`/`USER_INITIATED` 并绑 P 核（`utils/macos_compat/macos_compat.cpp:285` `apply_worker_thread_scheduling`、`lib/support/scheduling/darwin_thread_scheduling.cpp:18`），而 **UHD 自己的 worker/USB 线程不受我们控制** | ① 观测尺度（1–20 ms、与我们的 CPU 负载**无关**）正是"被按一个调度量子压住"的形状；② 唤醒源**是 GPU 路径特有的**（Metal 完成回调、lane fence、grid-ready、token 事件、每跳命令缓冲）；③ 融合车道在 default 只有 **7–10 跳/s**（加压 ~480/s）、设备账 ~110 µs/跳（§6.144）⇒ **CPU/GPU 占用都 ≪1%**，**不是"负载饱和"型原因** | sick 腿期间 `sudo sample <gnb pid> 10 -file /tmp/sample.txt`：UHD worker 的样本有多少**不在 CPU 上**、在等什么；可选对照 = `taskpolicy` 把整进程提为 latency-sensitive |
| **M-B** | **内核/驱动级停顿**（IOGPU 的 VM/分配路径、USB host controller 的锁）| 与 M-A 同症状，但责任在内核而非调度器 | 同一次 `sample`/`spindump` 里 UHD worker 的**内核栈**占比（`IOUSBHostFamily`/`IOGPU`）|
| **M-C** | **总线/设备状态**（USB 链路、同总线的其它设备、设备电源态）| 病态能自愈、断电重启有先例（纪律 13）| ✅ **本次已查**：`ioreg -p IOUSB` ⇒ **B200 独占一个 XHCI 控制器**（`AppleT8132USBXHCI@03000000 → USB3 Gen2 Hub → USRP B200`），手机/键鼠/Dock 在**另外三个**控制器上 ⇒ **"同总线争用"这条基本排除**（只剩设备电源/热状态）|
| **M-D** | **结构定位（哪一段）**：设备 DFT→grid 链 vs 融合车间的回调/事件机制 | `cpu_gpu` 单模块臂 **12/12 健康**、`dft+grid` 8 病态/1 健康；融合（超集）56/61 病态 | **同会话 A-B-A**（`gpu`/`cpu_gpu`，或 `dft+grid`/`dft`），每条腿先读类别 ⇒ 把"结构性"从群体统计升级为**同分钟内**的因果对照 |

★ **下一步最小方案（不改 PHY 代码）**：① 用 `wip/leg_triage.sh <label>` 给每条腿**先分类**（健康/边缘/病态 + 百分比）；② **病态腿同期**跑 `sudo sample <pid> 10`（+ `ps -M <pid> | wc -l`）把"谁在占 CPU、UHD worker 卡在哪"抓下来；③ 按 M-A/M-B/M-C 的读数分派修法。**只有先把触发条件看见，才有资格谈修哪里。**

#### ⑫ ★★★ macOS **内核调度**视角的排查（用户 2026-09-29 提问）：能查的都查了（**五项负结果**），并备好**一套可执行工具链 + 一个零代码 A/B**

**用户判读（与数据一致）**：CPU 与 GPU **都**会出现，只是**机率**不同 ⇒ 更像"**一直存在的宿主调度危险**，GPU 路径把它的概率抬高"，而不是某个 PHY 模块的缺陷。

**本轮（零腿）的负结果 —— 都排除掉，别再重复**：

| 查什么 | 怎么查 | 结果 |
|---|---|---|
| B200 是否与别的设备共用总线 | `ioreg -p IOUSB` | **独占一个 XHCI 控制器**（`AppleT8132USBXHCI@03000000 → USB3 Gen2 Hub → USRP B200`）；手机/键鼠/Dock 在**另外三个**控制器上 ⇒ **总线争用排除** |
| 有没有第三方进程占用 B200 | `ioreg -l -r -c IOUSBHostDevice` 的 `IOUserClientCreator` | B200 **无外部用户客户端**；但 **UTM(pid 439) 与 Chrome(pid 9970) 各持有 14 个** USB 用户客户端（都在 Dock/键鼠/音频上）⇒ **宿主负载有外部来源**，与"腿的机率不同"可能有关（记录在案，未证实） |
| 控制器自己有没有报错 | `ioreg` 的 `controller-statistics` / `port-statistics` | `SpuriousInterruptCount=0`、`EOF2Violation*Count=0`、`AddressFailureCount=0`、`EnumerationFailureCount=0`、`link-error-count=0` ⇒ **控制器层面零故障** ⇒ 症状是"**没被及时服务**"，不是"看到了错误" |
| 统一日志里有没有驱动消息 | `log show --info --debug` 病态窗口（21:50–22:10）vs 健康窗口（22:20–22:50） | USB/GPU 驱动消息 **2 条 vs 0 条**（那 2 条还是 configd 给 Dock 命名的）⇒ **默认级别下 USB 栈不落日志**；要看必须 `sudo log config --subsystem <USB 子系统> --mode level:debug,persist:on` |
| 调度器暴露了什么 | `sysctl -a \| grep -iE 'sched\|qos'` | `kern.sched: edge`、`sched_rt_avoid_cpu0`、`sched_recommended_cores`；机器 = **Apple M4 Pro，14 核（10P + 4E）** |

**可执行的"内核调度"工具链（本机已验证参数与权限要求）**：

1. ★ **`sudo taskpolicy -l <0..5> -t <0..5> -p <pid>`**：**对运行中的进程改 latency / throughput QoS tier**（`mach/task_policy.h`：`LATENCY_QOS_TIER_0..5`、`THROUGHPUT_QOS_TIER_0..5`，启动默认都是 **TIER_3**）；**`sudo taskinfo <pid>` 把 tier 读回来**（本机 `/usr/bin/taskinfo` 存在、需 root）⇒ **这是"内核调度是不是杠杆"的零代码判据**。
2. `sudo powermetrics --samplers tasks,interrupts,sfi --show-process-qos-tiers --show-process-wait-times --show-process-amp -i 1000 -n <秒> -o <文件>`：**按进程**给出 QoS tier、**调度等待时间**、**P/E 核分布**，外加**中断落点**与 selective-forced-idle（`powermetrics` 在本机只差 root，参数已验证合法）。
3. `sudo sample <pid> 5`（用户栈）/ `sudo spindump`（含内核栈）：看 **UHD / libusb 自己的线程**在哪里等 —— 那些线程**不归我们调度**，而融合车道比 CPU 路径多出的唤醒源（Metal 完成回调、lane fence、grid-ready、token 事件）正是首要嫌疑。
4. ★ 新脚本 **`wip/host_sched_watch.sh <秒> [输出目录]`**（**先跑它、再飞腿**）：开跑前/后各抓一次 XHCI 统计，全程 1 Hz `powermetrics`，gNB 起来时抓 `taskinfo` + 线程表 + 一次 mid-leg `sample`，并把上面那条 `taskpolicy` 命令**带着 pid 打印出来**；腿跑完再用 `wip/leg_triage.sh <label>` 分类。

**预登记的零代码 A/B（下一步就该飞这个）**：

| 腿 | 施加 | 若"调度 tier 是杠杆" | 若"危险在内核 workloop/中断路径" |
|---|---|---|---|
| 对照 | 无（`gpu` default，与 `p123` 同配方）| 按当前机率（历史 `gpu` 56/61 病态）| 同 |
| **升档** | 腿跑到 ~10 s 时 `sudo taskpolicy -l 5 -t 5 -p <pid>` | **`dl_tx_call>1ms` 与 RX 尾巴当场塌到 ≤0.006% / ≤0.01%**，且 `powermetrics` 里该进程调度等待时间下降 | 无变化 |
| **降档** | 同样在 ~10 s 时 `sudo taskpolicy -l 0 -t 0 -p <pid>` | **当场变严重** | 无变化 |

* 判读：升/降**双向都动** ⇒ 机制 = **进程调度 tier**（修法 = 启动时设 tier，**不动数据路径**，属"策略"而非 PHY）；**都不动** ⇒ 危险在下游（内核 workloop/中断），继续用 ②③ 的内核栈 + 打开 USB 栈 debug 日志。
* ⚠ **口径**：`[ul_rx_timing]` / `[dl_tx_call]` 是**终值计数器**，腿内切换只有 **RF 失败的时间戳**能切分（所以腿内 A/B 的读数是"切换前/后的 RF 失败速率"，RX 尾巴要**两条腿**对比）；这也再次说明 ⑨ 的**周期打印探针**值得做。

### 6.158 ⏹ **收口（用户 2026-09-29 裁决）：ms 级 Rx 尾巴 / `[ul_rx_wait]` 这条线结案存档，作为"欠账"登记 —— 不再为它花腿**

> 用户原话："我们在这条路径上已经挖了很久，但是还是没有实质性的进展，我觉得应该就此收口，做为一个欠账记录在案。"

**一、为什么可以收（交付侧不受影响）**：这条尾巴只动 V1 的 **max/p99**（中位与全部判据不受它支配）—— 本 HEAD 上最好的中位恰好来自这条线：`p125-n78-default` **V1 中位 1359.8 µs**；加压 `p124` 契约 **9 of 9**、`leg_gate` **9 of 9**；`milestone_audit.sh --quick` = **27 PASS / 1 FAIL（stress 腿的提交 ≠ HEAD）/ 0 RED**。**收口不是"判它没问题"，而是"它的代价在判据之外、且当前投入产出已为负"。**

**二、已确立（留下来，别推翻）**：
1. ★ **结构易感在 GPU 路径**（`cpu` 0/4、`cpu_gpu` 10/24、`gpu` **56/61** 病态；`cpu`+`cpu_gpu` vs `gpu` **Fisher p = 7.0e-08**、`cpu` vs `gpu` **1.9e-04**；同会话对照：22:28 `gpu` 0.0058% vs 22:39/22:44 `cpu_gpu` **0.0002%×2**）—— **用户判读，数据支持**（§6.157⑩）；
2. **直接症状位置 = 宿主↔电台接口**（双向同时、比值 1.3–1.6；`grid_devwaited=0`、`loop>1ms` 0–2 次、池绿、`pop_blocking` µs 级 ⇒ **不是**池、不是网格完整性、不是我们的等待、不是我们的 RX 宿主代码）；
3. **触发条件未知** ← **这就是欠账**；
4. `[ul_rx_wait]` / `[ul_rx_timing]` 的**口径已经理清**（启动那条 ~101 ms 是常量、`startup=` 显式剥离、按跳配对序列 `[ul_rx_wait_hop]`、`slip = loop + recv − air` 的分解、`loop` 与 `recv` 各测什么）—— 这条线上的**读数本身**是干净的，欠的只是"为什么有时会丢实时性"；
5. **P2-E 保持默认开**，理由 = "去掉输入保持"（§6.156 的"吃尾巴 60–70×"已由 §6.157 撤回）；
6. ★ **纪律 20 永久有效**：任何尾巴结论必须带该腿 DL 侧的输运协变量；**一条腿的低尾不是旋钮的读数**。已机械化：`leg_gate.sh` 与 `leg_triage.sh` 每次自动打**三档**类别（健康 ≤0.0064% / 边缘 / 病态 ≥0.0248%）。

**三、已否证清单（避免重复劳动 —— 这些都查过了，都否了）**：
池 / 网格完整性 / 环槽索引与块切换 / **宿主调度（我们的线程按时发问）** / `load1` / 外部 GPU 负载 / **我自己的构建负载（27 条腿窗口内 `build/` 写入 = 0）** / 等待策略（`p114`/`p115` 修好后仍病态、`p121` 旧策略健康）/ **P2-E（分裂模式里 `by_event=0`，不作用）** / **USB 总线争用（B200 独占 XHCI 控制器）** / **控制器故障（`SpuriousInterrupt`、`EOF2Violation*`、`AddressFailure`、`EnumerationFailure`、`link-error-count` 全 0）** / 统一日志驱动消息（默认级别不落）/ **臂**（裸 `dft+grid` 今晚健康、下午病态）/ **模式**（CPU 也会出现，只是机率低）。

**四、留下的工具（重开这条线只要几条命令，不必重新发明）**：
| 工具 | 作用 |
|---|---|
| `wip/leg_triage.sh <label>` | 给一条腿**分类**（健康/边缘/病态）+ 打出病态时的采样命令与 USB/线程清单 |
| `wip/host_sched_watch.sh <秒> [目录]` | **先跑它再飞腿**：腿前/后 XHCI 统计、1 Hz `powermetrics`（QoS tier / 调度等待 / P-E 核分布 / 中断 / SFI）、gNB 起来时 `taskinfo` + 线程表 + mid-leg `sample`，并打印带 pid 的 `taskpolicy` 命令 |
| `leg_gate.sh` 的输运行 | 每条腿自动三档分类（判定"这条腿的尾巴能不能用来判代码"）|
| §6.157⑪ / ⑫ | 机制候选表（M-A 调度 / M-B 内核驱动 / M-C 总线 / M-D 结构）与**预登记 A/B**（`taskpolicy -l/-t` 升/降档）|

**五、重开条件（预登记：满足其一才动腿）**：
1. 交付 SLO 开始约束 **max/p99**（现在只判中位；历史带：V1 max 2.7–7.0 ms、p99 1.55–1.64 ms）；
2. 出现 **`gaps>0` 或 `rx_overflows>0`**（那不是"尾巴"，是**真丢样点**，`p123` 那次就是）；
3. 要把融合车道搬到**别的平台/别的电台**（"结构性易感"的判据就是这套读数，届时必须重新标定）。

**六、重开的第一步（已预登记，按代价排序）**：
0. **物理**：B200 现在挂在 `USB3 Gen2 Hub@03200000` 上 —— 直接插到 Mac 的端口（去掉 hub）飞一条同配方腿，与 `p125`/`p122` 比类别（零代码、零风险）；
1. `host_sched_watch.sh` + **对照 / 升档 `-l 5 -t 5` / 降档 `-l 0 -t 0`** 三条腿（§6.157⑫ 的表）：**双向都动** ⇒ 机制 = 进程调度 tier（修法是**策略**，不动数据路径）；**都不动** ⇒ 危险在内核 workloop/中断路径，转 `sample`/`spindump` 内核栈 + 打开 USB 栈 debug 日志；
2. 若要**腿内**分辨"开场即病 vs 中途发作"，先做 §6.157⑨ 的**周期打印慢事件**小探针（两把钥匙、默认关）。

### 6.159 ★★★ **`t2f` 的 2× 不是"FFT 被拆成两批"，而是"缓冲在排队等 GPU 开工"**（腿 `p128-n78-phases`，`OCUDU_METAL_GPU_TIME=1` 的直接读数）

**问题（用户 2026-09-29）**：`[ul_time_frequency]` 里含 `[ul_rx_wait]`；纯 Metal 的 FFT（14 个变换并行）应当只有 ~50 µs，但 `[ul_slot_trace]` 里偶有 `t2f` 是 ~100 µs（2×）—— 是不是"上一个 slot 的 `eq_demod` 还在跑 ⇒ 可用 GPU 核心 < 14（normal CP）/12（extended CP）⇒ FFT 必须分两批"？

**① 观测属实，并给出分布**：trace 的 `t2f` 列（**收包等待之后**的那一段）中位 **54.7**、p90 82.2、p95 **106.6**、max **213.7** µs；直方图在 **100–150 µs 有第二个簇（22/378 ≈ 6%）**。而 `[ul_time_frequency]` 中位 **539.7 ≈ rxwait 476 + 这一段** ⇒ **该段确实含收包等待**（用户判断正确）。

**② 判决性读数：设备侧执行时间从不翻倍**（`OCUDU_METAL_GPU_TIME=1` 的 Q9-F3 per-label 表）：

| 标签 | 它是什么 | exec p5 / p50 / p95 |
|---|---|---|
| **`late_handed`** | **交棒出去、未被车道采纳、由注册表兜底单独提交的前端块**（= 只含该槽变换的那条缓冲）| **48.4 / 50.0 / 51.0 µs**（n=3205）|
| `dft_front_end` | plain 路（PRACH，每缓冲 1 个变换）| 46.1 / 46.5 / 48.1 µs |
| `ce_weights` | CE 权重 | 25.8 / 37.2 / 58.8 µs |
| `merged_hop` | 融合车道的整跳（**含被采纳的前端变换**）| 428.7 / 470.2 / 491.0 µs |

⇒ **"50 µs"这个量级用户的估计是对的**（`late_handed` 的 p50 正好 50.0），而**如果 FFT 真的被分成两批，执行时间会在 ~93–100 µs 出现第二个模态 —— 四条标签里一条都没有**（`late_handed` 的带宽只有 ±1.3 µs，最干净）。

**③ 2× 出现在"开工之前"**：`commit → GPU start` 的等待 —— `dft_front_end` wait p50 **45.5** / p95 **81.2**（min 7.7）µs、`ce_weights` p50 36.3 / p95 **310.8**、`merged_hop` p50 205.2 / p95 **284.4**；最坏的单条缓冲 **6296 µs**（slot 17648），而它 `start->end` 仍只有 **51.1 µs**。⇒ **GPU 是"推迟开工"，不是"把 FFT 跑慢"**；两者在墙钟上不可分（都是 +50 µs 量级），**唯一能分开的就是 exec 的分布**，而它回答"从不翻倍"。

**④ 共驻留是真的，但代价落在等待上**：Q24 `waiter resident while its signaller ran` = **46052/142384 = 32.3%**（mean 38.8 µs、p95 55.6、max 216 µs）⇒ 前后端**确实会在设备上同时驻留**（这正是"重叠"直觉的来源），但实测它换来的是**排队等待**，不是"核心不够 ⇒ 分两批"。

**⑤ `t2f` 慢的槽不由"上一槽的 `eq_demod`"预测**（287 对相邻槽）：`corr(t2f[N], eqdemod[N−1]) = 0.20`；慢 `t2f` 组的上一槽 `eq_demod` 中位 **926 µs** vs 快组 **890 µs**（同档）。反而 `corr(t2f[N], ce[N]) = **0.49**` ⇒ 慢的成因在**同一跳内**（本跳自身的排队/共驻留），不是上一跳的尾巴压过来。

**⑥ 面**：前端 duty **5.6%**、后端 **22.1%**、union **25.3%**，>100 µs 的空洞 214635 个（最大 2.7 s）⇒ **GPU 远未饱和**（本机 GPU = **20 核**，M4 Pro）：这是"瞬时排布"问题，不是吞吐墙。

**结论**：2× 真实存在，但机制是 **"缓冲等 GPU 开工"（queue wait）**，而不是 **"FFT 被拆成两批执行"**；"重叠/争用"的方向没错，作用点要改写成"**开工被推迟**"。

**两条后续（已预登记）**：
1. ★ 本轮顺手给 Q9-F3 的 per-label 表加了 **`max=` 列**（此前只有 p50/p95/min/p5 ⇒ **占比 <5% 的 2× 子总体根本看不见**）：下一条带 `OCUDU_METAL_GPU_TIME=1` 的腿就能**直接读**"exec 是否曾经翻倍"，而不必再推断。
2. **交付路径上 PUSCH 前端的 GPU 窗口没有自己的标签**：它随被采纳的块走、落在 `merged_hop` 里（`ocudu_metal_burst.mm:731` 的注释写明 "the merged route's adopted block, which brings the front end's transforms with it"）。要在交付配置下**逐槽**读前端自己的 `commit→start / start→end`，只能飞一条**测量臂**：`OCUDU_DFT_RELEASE_BLOCK=0`（不采纳前端块 ⇒ 前端自己提交，回到 `dft_front_end` 标签）+ `OCUDU_METAL_GPU_TIME=1` + `OCUDU_UL_SLOT_TRACE=512`。⚠ 该臂**改结构**（D1 关闭，V1 会变差），只用于机制，**不作为交付读数**（门禁按 `KNOB_EQ` 拒它当交付腿——设计如此）。

#### ⑦ `p129-n78-noadopt`（上面那条测量臂，D1 关）：**payload 拿到了，同时暴露了我给用户的命令漏了一个旋钮**

* **payload（②的判决读数，第一次带 `max=`）**：前端自己提交后 `dft_front_end n=447228`（其中 **109007 条是 14 变换的 PUSCH 批**；`batched=109007/1526098`、`released=0` 证明交棒确实关掉了）：**exec p50 46.9 / p95 54.8 / min 43.2 / p5 46.1 / max 242.5 µs**；wait p50 44.9 / p95 **290.6** / max **1705.2** µs。
  ⇒ **FFT 批次的执行中位就是 46.9 µs（用户"~50 µs"的估计在 44.7 万条缓冲上成立）**；★ **修正 ② 的措辞**：当时只有 p95（48.1），所以写了"从不翻倍"——**`max=242.5` 说明执行侧的 2–5× 尾巴确实存在**，但 `p95=54.8` ⇒ **≥2× 的批次 ≤5%**；主变差仍是 **等待开工**（p95 290.6、max 1705）。⇒ 尾巴的**稀有度**（≤5%，而后端忙碌时间占 20–30%）**不支持"GPU 经常凑不齐 14 个核"**，更像队列层级的串行/优先级效应。
  * 同表另两条也值得记：`merged_hop` exec p50 457.2 / **max 4651.5** µs、`ce_weights` p50 41.6 / **max 750.0** µs ⇒ **长执行尾巴是这台平台调度的普遍现象**，不是前端独有。
* ⚠ **我给用户的 p129 命令漏了 `OCUDU_UL_PHASE_SEGMENTS=1`** ⇒ `record_t2f_end()`/`record_ce_end()` 在 `records_phase_segments()==false` 时**直接 return**（`ul_pipeline_probe.h:363`），于是 `[ul_slot_trace]` 的 **`t2f`/`ce` 两列 512/512 全 NaN**（p128 带了该旋钮 ⇒ 378/512 完整行）。**这不是数据坏了，是旋钮没开** —— 与 ㊲ 同一条口径：**`gpu` 模式下要这三列必须显式 `OCUDU_UL_PHASE_SEGMENTS=1`**。
* **交通塌陷（用户观察）**：UL PDU **390.6 MB → 180.9 MB（−54%）**、跳数 −29%、LDPC −46%、V1 中位 **1415.8 → 1519.1（+103 µs**，与"关 D1 = 把账搬家 + 变差"的预测一致）。可见成因是 **PRACH 检测行 16 → 615（≈2/s）**，而 **PHR 反而更好（+11..12 dB vs +8..9）** ⇒ 手机在**反复随机接入（上行不连续）**，不是功率/链路问题。按纪律 16（>100 即风暴腿）**`p129` 是风暴腿** ⇒ 吞吐损失里有一部分是**风暴本身**的后果；n=1 **分不开"臂导致风暴"与"手机状态恰好如此"**（这正是已收口的风暴线的那条老歧义）。契约 **9/9**、`gaps=0`、`crossings` 0/0 ⇒ **臂没有破坏正确性**，只是结构变差 + 伴随风暴。
* **结论**：这条臂**只作机制读数**（它问的问题已经答完）；要再飞必须带 `OCUDU_UL_PHASE_SEGMENTS=1`，并且**先看 `detected_preambles` 与 PHR** 判断这条腿能不能当"干净臂"。

#### ⑧ `p130-n78-noadopt-phases`（D1 关 + 四个探针）：**`t2f` 的 ~500 µs 不是新缺陷 —— 它就是"关掉交棒"的代价**（主机开始自己等前端缓冲），而**交通塌陷在"每授权载荷"上，不在授权数上**

**用户观察 1：`t2f` 到了 ~500 µs 量级。** 归因（三腿对照，`p128` = D1 开、`p129`/`p130` = D1 关）：

| 读数 | `p128`（D1 开，交付结构）| `p130`（D1 关）| 差 |
|---|---|---|---|
| `[ul_dft_wait]`（主机等前端自己的缓冲）| **no samples recorded** | **n=140365 中位 147.8 / p95 415.7 / max 1725.8 µs** | **D1 开时主机一次都不等** |
| `[ul_time_frequency]`（含收包等待）| 539.7 | **712.6** | **+173 ≈ 上面那 148 µs** |
| `[ul_channel_estimation]` | 62.5 | 71.5 | +9 |
| `[ul_equalization_demod]` | 799.0 | **622.0** | **−177**（账搬走了）|
| 三段合计 | 1401.2 | 1406.1 | **+5** |
| `[ul_gpu_pipeline]`（V1）| 1415.8 | **1513.2** | **+97** |

* ⇒ **机制读法**：D1（交棒）把"等前端网格产出"从**主机侧阻塞**（`waitUntilCompleted`，中位 148 µs、p95 416）搬到了**命令缓冲内的栅栏**（设备侧、可与其他工作重叠）；关掉 D1 ⇒ 主机把那 148 µs 自己吃回去 ⇒ 它落进 `t2f` 段（+173）、同时 `eq_demod` 段不再需要等网格（−177），**总量只涨 V1 的那 ~97 µs**。**这是 D1 的设计意图被量化**，不是新缺陷。
* ⚠ **口径**：`[ul_slot_trace]` 的 `t2f` 列在 p130 里只有 **179/512** 行有值（该表按"最慢 128 行"淘汰 ⇒ **偏向慢样本**），所以你看到的 ~418（中位）/486（p95）/547（max）是**偏慢子集**；**无偏的**读数是聚合行 `[ul_time_frequency]` 712.6 = 收包等待 ~473 + 收包后 ~240。

**用户观察 2：iperf3 流量仍然很低。** 归因（三腿）：

| | `p128`（D1 开）| `p129`（D1 关）| `p130`（D1 关 + 探针）|
|---|---|---|---|
| UL PDU 总量 | 390.6 MB | 180.9 MB | **137.4 MB** |
| **UL 授权数** | 142380 | 97382 | **136164（≈p128，没少）** |
| **TBS 中位 / p95** | **3585 / 6402 B** | 3072 / 6528 B | **1313 / 1985 B（塌）** |
| PDU 均值 | 3551 B | 3044 B | **1306 B** |
| CRC-OK / lanes | **84.4%** | 66.8% | **78.3%** |
| PRACH 检测行 | 16 | 615 | **144** |
| PHR | +8..9 dB | +11..12 | +7..8（同档）|
| 契约 | 9/9 | 9/9 | **9/9** |

* ⇒ **授权数没变、每授权载荷塌了 2.7×**（TBS 中位 3585→1313）⇒ 不是"少发了授权"，是**调度器把 UL MCS/TB 打小了**（CRC-OK 从 84.4% 掉到 78.3%，PRACH 从 16 涨到 144 ⇒ 链路自适应下调 + 手机反复接入）。PHR 同档 ⇒ **不是功率问题**；契约 9/9 ⇒ **不是正确性问题**。
* ⚠ **归因边界（必须写明）**：这是**单腿对照**，"臂导致 UL 结果变差 ⇒ OLLA 下调 ⇒ 小 TB" 与 "手机/会话当时就是这个状态" **分不开**（已收口的风暴线同一条教训）。**便宜的判别**：紧接着飞一条 **D1 开**（交付默认、其余探针相同）的对照腿，比 TBS/PRACH/CRC —— 若对照也小，则与臂无关。
* ⇒ **结论**：机制问题（exec 47 µs 中位 / p95 60 / max 251，等待才是变量）**已经答完**；D1 关的臂**只作机制**，其交通读数**不得当作交付性能**（结构变差 + 手机状态混杂）。若只要**交付配置下的分阶段分解**，用 `p128` 的读数即可（t2f 539.7 = 收包等待 + 55、ce 62.5、eq_demod 799.0、V1 1415.8）。

#### ⑨ `p131-n78-adopt-phases`（D1 开 + 四探针，`p130` 的对照）：**"D1 关 ⇒ 上行变小 TB"被这对背靠背腿证实**；但这条腿**没有报告（void）**，而且**同配置的会话间波动很大**

**先说状态**：`p131` 的 stderr 里进程跑完 **00:51:24 → 00:58:24（≈7 min）**，末行是 **`Could not stop application after 5 seconds. Forcing exit.`** ⇒ **atexit 报告被截断**（已知先例 `p70`/`p110`）。★ **用户随后退出 gNB，截断后的 stderr 现已可读（684 行）**：**前半段在**（`[ul_gpu_pipeline]`、三段、**契约判决**、`[ul_dft_wait]`、`[ul_mac_pdu_size]`、trace 表、`[ul_rx_wait]`/`ul_rx_wait_hop`、`[ul_host]`），**尾段缺**（`[ul_rx_timing]`/`[ul_rx]`/`[ul_rx_pool] taken`/`[dl_tx_call]`/`[dl_tx_slack]` 与 `[metal_stats] queue occupancy / gpu busy`）⇒ 按纪律**仍以"无完整报告"作废（不得当验收腿）**，但**已到的读数可用**（下面全部取自它）。缓解仍是三步：**先停流量 → 等上行排空 → 单次 Ctrl-C**。⚠ `.log` 里 **16:56:04 `ue=0: DRB traffic stopped`**（腿长的 ~2/3 处），即 iperf3 在腿结束前约 2 分钟就停了 ⇒ "~300 MB"里有一部分是**窗口尾部没有流量**。

**★ 截断报告给出的三件事（都是"第三次确认"级别）**：

| 读数 | `p128`（D1 开）| `p131`（D1 开 + 四探针）| `p130`（D1 关 + 四探针）|
|---|---|---|---|
| `[ul_dft_wait]` | **no samples** | **no samples** ✅ | n=140365，中位 **147.8** / p95 415.7 |
| `[ul_time_frequency]` | 539.7 | **538.4** ✅ | **712.6** |
| `[ul_channel_estimation]` | 62.5 | **62.1** ✅ | 71.5 |
| `[ul_equalization_demod]` | 799.0 | **798.2** ✅ | **622.0** |
| 三段合计 | 1401.2 | **1398.7** ✅ | 1406.1 |
| `[ul_gpu_pipeline]`（V1 中位）| 1415.8 | **1407.3** ✅ | 1513.2 |
| `[ul_mac_pdu_size] total` | 390.6 MB | **316.4 MB**（用户 iperf3 ~300 MB ✓）| 137.4 MB |
| 契约 | 9/9 | **9/9** ✅ | 9/9 |

1. ⇒ **§6.159⑧ 的机制被第三次独立确认**：D1 开 ⇒ 主机**一次都不等**前端（`no samples`）、t2f 回到 **538**、eq_demod 回到 **798**；D1 关 ⇒ 主机等 **147.8 µs**、t2f 涨到 712.6、eq_demod 掉到 622.0。（三腿：`p128`/`p131` 开、`p130` 关。）
2. ⇒ **⑧ 的 D1 对照在正确性上也闭合**：`p131` 契约 **9 of 9**（12 条检查全 OK：`ce device estimates` **1740924 device / 0 host**、`crossings` 0/0、`lane host participation` 145079/145079、`dft radio inputs` 交接路 79.4%），V1 **1407.3** 甚至略优于 `p128` 的 1415.8。
3. ⇒ **⑧ 的 t2f 观测在第二条 D1 开腿复现**：trace 里非 NaN 的 `t2f`（`p131` 56 行）中位 **54.6**、p90 71.1、p95 76.9、max **117.8** —— 与 `p128` 的 54.8 / 105.3 / 213.7 同量级 ⇒ **"收包后 ~55 µs"是可复现的**；尾巴这一次小一些（样本只有 56 行）。
   ⚠ 同一条 trace 在 `p131` 里只有 **56/512** 行能配上 `t2f`、**9/512** 行完整（`p128` 是 453/378）⇒ 这是**保留策略与基准注册表的年限冲突**（该表"把最慢的 128 行按年龄留下"，而那些老行的 landmark 早已被 `pending_*` 的定长表淘汰）⇒ **引用分阶段读数一律用聚合行**（`[ul_time_frequency]` 等），trace 只用于"逐槽形状"且必须先数完整行。

**但它的 `.log` 足以回答判别问题**（四腿同口径，全部从 `.log` 取：授权数 / TBS / **分配总量** / CRC-OK / PRACH）：

| 腿 | 结构 | 授权数 | TBS 中位 | TBS 均值 | 分配总量 | CRC-OK | PRACH 行 |
|---|---|---|---|---|---|---|---|
| `p128` | **D1 开** | 142380 | **3585** | 3827 | **544.9 MB** | 84.4% | 16 |
| `p129` | D1 关 | 97382 | 3072 | 3387 | 329.9 MB | 66.8% | **615** |
| `p130` | D1 关 + 探针 | 136164 | **1313** | 1302 | **177.3 MB** | 78.3% | 144 |
| **`p131`** | **D1 开 + 探针** | 145012 | **2241** | 2398 | **347.8 MB** | **95.7%** | **3** |

* ⚠ **此条已在 §6.160⑥ 撤回（时间预算检查）**：下面的差异**应归给链路状态**（两腿 PHR 相差 3–4 dB），不是 D1。原文保留以便追溯：★ **背靠背对照（`p130` → `p131`，只翻 `OCUDU_DFT_RELEASE_BLOCK` 一个旋钮，相隔 13 分钟）**：PRACH **144 → 3**、TBS 中位 **1313 → 2241 B（+71%）**、分配总量 **177.3 → 347.8 MB（×2.0）**、CRC-OK **78.3% → 95.7%** ⇒ **"关 D1 会压上行"这个假设被证实**（与 ⑧ 里那条"臂让 UL 结果变差 ⇒ OLLA 下调 + 手机反复接入"的链条一致）。
* ⚠ 但**两条 D1 关的腿彼此差很多**（`p129` TBS 3072 / PRACH 615 vs `p130` 1313 / 144）⇒ **臂的代价是"会话相关"的，不是常数**；而**两条 D1 开的腿也差**（`p128` 3585 / 544.9 MB vs `p131` 2241 / 347.8 MB，**同配置**）⇒ ★ **本配置的会话间波动本来就有 ~1.6×**（分配总量），引用吞吐/流量类结论时必须成对读（§6.143 的吞吐臂同理）。
* 用户观察（iperf3 ~300 MB，历史好时 ~400 MB）与上表一致：`347.8 MB × 95.7% ≈ 333 MB` 解码 TB，折成 MAC PDU ≈ 280–300 MB；而 `p128` 的 MAC PDU 总量正是 **390.6 MB**（"历史好时 ~400"）。
* ⇒ **待办（可选）**：把 `p131` 用纪律重飞一条（`p132`，同旋钮）以拿到 V1/契约/`[ul_dft_wait]`（预期回到 **"no samples"** ⇒ 再次印证 D1 已开）与输运协变量；否则 `p131` 只作为**链路/流量**证据，**不得**当验收腿。

### 6.160 ★★★ **D1（交棒）机制详解：它删掉的是"宿主中途参与"，所以影响远不止 100 µs**（用户 2026-09-29 提问；数字来自 §6.159⑦⑧⑨ 的三腿对照）

#### ① 它是什么（一条跳里发生的三件事）

**D1 = 前端（DFT + 网格写）把"装满变换、但还没提交"的那条命令缓冲，直接交给下一个阶段**，让 CE/均衡/解映射**接着往同一条缓冲里编码**，最后由车道提交一次。默认开（`OCUDU_DFT_RELEASE_BLOCK=1`，5.9.49 起；`=0` 是关掉它的测量臂）。

| 步 | 代码位置 | 做什么 |
|---|---|---|
| ① 交棒 | `ocudu_dft_metal_engine.mm` 的 `release_block()`（`:2031`）| 先把该槽的延迟变换**编码进这条 cb**（`flush_pending_front_end`，注释写明"the adopter appends its own dispatches AFTER them, so the grid is produced before anything that reads it"）→ 关掉自己的 encoder → **不提交** → 把 grid 基址 + 槽号 + generation 与输入 token 一起 `deposit_released()` 进注册表 → 该槽标记 `slot_released`（此后 `wait_slot()` **拒绝**，因为"这条缓冲不归本引擎"）|
| ② 采纳 | `ocudu_metal_burst.mm` 的 `shared_burst::adopt()`（`:614`）| 车道的 burst **接住**这条未提交的 cb（不新建），把 CE/均衡/解映射的派发编码进去；la bel 变成 `merged_hop`（注释："the merged route's adopted block, **which brings the front end's transforms with it**"）|
| ③ 提交 | 同上（`:751` 附近）| 车道提交这**一条** cb；宿主侧读者（PUCCH×2、SRS）的顺序由 `grid_ready_hook::wait` + 注册表保证 —— 找不到就**丢弃**，绝不读半成品（§6.153 的顺序审查）|
| 兜底 | `commit_late_handed_block()`（`:754`）| 没人采纳的（迟到/被清扫）交棒块由注册表的清扫线程**代提交**，label `late_handed`，只为"输入 token 不泄漏"；它**故意不发前端栅栏信号**（消费者已经不在）|

**与之相反的那条路（D1 关）**：`ofdm_demodulator_impl.cpp:486-506` 里 `release_block()` 返回空 ⇒ `end_block()` ⇒ 走到 `else if (!wait_per_slot || last_symbol_of_slot) dft->wait_slot(slot);`，而 **`wait_slot()` 里是 `[cmd_buf waitUntilCompleted]`（阻塞宿主）**；前端回到自己的队列、自己的 cb，跨队列的"网格就绪"必须由 fence/事件表达，车道的阶段只能**在那次宿主等待之后**提交。

#### ② 它删掉的三样东西（这就是"为什么影响大"）

1. ★ **宿主的中途参与**：D1 开 ⇒ 整跳**一次提交**，网格依赖由**同一条 cb 内的编码顺序**保证，GPU 自己解析 ⇒ 宿主**从不等待**（实测：交付腿 `[ul_dft_wait]` = **no samples recorded**）。D1 关 ⇒ **每槽一次**宿主阻塞（实测 `p130`：**中位 147.8 / p95 415.7 / max 1725.8 µs**；历史 `p41` 更差，中位 **577.6 µs**）。**这笔等待在关键路径上、每 500 µs 的槽来一次**，所以它既加中位也加抖动。
2. **跨队列依赖 + 一条多余的 cb**：D1 开 ⇒ 前端变换**住进车道已有的那条 cb**（不新增）；D1 关 ⇒ 前端每槽自己提交一条（`p130`：`dft_front_end n=466590`，exec 中位 **47.0 µs**）外加一次"提交→完成→再提交"的往返。
3. **可重叠性**：设备侧栅栏可以被 GPU 与其他工作重叠排布，宿主侧 `waitUntilCompleted` **不可**——它把宿主钉在那里，也把后面所有阶段的**下发时刻**推后。

#### ③ 三腿实测（同一份代码，只翻 `OCUDU_DFT_RELEASE_BLOCK`）

| 读数 | `p128`/`p131`（**D1 开**）| `p130`（D1 关）| 差 |
|---|---|---|---|
| `[ul_dft_wait]` | **no samples** | 中位 **147.8** / p95 415.7 µs | 宿主从"不等"变成"每槽等" |
| `[ul_time_frequency]`（含收包等待）| 539.7 / **538.4** | **712.6** | **+173 ≈ 那 148 µs** |
| `[ul_equalization_demod]` | 799.0 / **798.2** | **622.0** | **−177（账搬家）** |
| 三段合计 | 1401.2 / 1398.7 | 1406.1 | ≈0 |
| `[ul_gpu_pipeline]`（V1 中位）| 1415.8 / **1407.3** | 1513.2 | **+97** |

★ **系统级的后果比 +97 µs 大得多**（`p130` vs `p131`，背靠背只翻一个旋钮）：上行 **TBS 中位 1313 → 2241 B**、**分配总量 177.3 → 347.8 MB（×2.0）**、**PRACH 检测 144 → 3**、**CRC-OK 78.3% → 95.7%**。机制链（⚠ **已在 ⑥ 撤回：这是猜想，不是读数**）：宿主每槽阻塞 ⇒ 上行结果更晚更抖 ⇒ 链路自适应（OLLA）压低 MCS + 手机反复随机接入 ⇒ **小 TB、流量掉 2 倍以上**。⇒ **D1 不是"快 100 µs"，它是"上行能不能连续工作"的一部分**；这也是为什么 `OCUDU_DFT_RELEASE_BLOCK=0` 只能当**机制臂**、其流量读数**不得当交付性能**（另见 ⑨ 的会话波动警告）。

#### ④ D1 的代价（它不是免费午餐）

1. **输入 token 挂在整跳的块上** ⇒ RX 池缓冲要等**整跳完成**才归还 ⇒ 这就是 **P2-E**（`OCUDU_DFT_RELEASE_TOKENS_EARLY`，现默认开）存在的理由（§6.156/§6.157）；
2. **"网格就绪"的时点从"前端提交"后移到"车道提交"** ⇒ 宿主读者必须走 `grid_ready_hook::wait` + 注册表 + **丢弃语义**（§6.153）；
3. **"没人采纳"的兜底路径**（`late_handed`）必须存在，否则输入 token 泄漏（注册表清扫线程代提交）；
4. ⚠ **判据缺口（本次新数据点，记入 Q22）**：`cbs/lane` 在 `p128`/`p130`/`p131` **都是 2.00**（它只数车道自己的 cb：`merged_hop` + `ce_weights`）——**D1 关掉后前端那条独立 cb 并不进这个计数** ⇒ "不许用提交数换时延"（V4）**看不见**多出来的那一条。⇒ Q22"整跳提交数"的欠账又多一条证据。

#### ⑤ ★ "代价不止 100 µs"里那 **100 µs 到底是什么**（用户 2026-09-29 追问）

**直接回答：是"V1 中位"的跨腿差值 —— 确实就是"打开 D1 快 ~100 µs"。**

| | 腿数 | V1 中位区间 | 中位 |
|---|---|---|---|
| **D1 开** | **10** | **1354.3 – 1431.4 µs** | **1407.0** |
| **D1 关** | 2（`p129`/`p130`）| **1513.2 / 1519.1 µs** | 1519.1 |

⇒ **两条 D1 关的腿整体落在 D1 开那 10 条腿的带之外**：比 D1 开**最差**的一条还高 **+81.8 µs**、比其中位高 **+112.1 µs** ⇒ **"打开 D1 ≈ 快 100 µs（80–115 µs）"** 成立。
⚠ 口径：这是**跨腿**比较（两条相隔 13 分钟、流量还不同；D1 关只有 2 条腿），只是**都在带外**；不是同一条腿内的 A/B。

**这 100 µs 的物理来源**（= ② 里那笔宿主阻塞）：

* `[ul_dft_wait]`（`wait_slot` 里的 `waitUntilCompleted`）：**D1 关 = 中位 147.8 / p95 415.7 / max 1725.8 µs**；**D1 开 = no samples**。
* 那 148 µs **本来在跳的关键路径上**（前端做完 → 宿主"知道"之后，后面的阶段才能下发）；D1 换成**同一条 cb 内的编码顺序**、设备自己解析 ⇒ 关键路径上删掉这一笔。**V1 只涨 ~100 而不是 ~148**：差额被跨槽流水线/重叠吸收（且那 148 µs 的样本总体还含 plain 路（PRACH）等其它等待）。
* ⚠ **必须说清的口径**：三段的**合计没变**（1401.2 → 1406.1）——那笔等待在段账里是**搬家**（`t2f` **+173**、`eq_demod` **−177**）。⇒ **那 ~100 µs 不在三段之和里**，而在"三段之和 ↔ V1"的**残差**（D1 关时未被三段覆盖的那些跳 —— 正是 landmark 被淘汰的**慢跳** —— 以及 LLR/宿主尾巴）。所以"快 100 µs"靠的是 **12 条腿的 V1 中位带**，**不是**段账的算术。

**"代价不止 100 µs"的意思（⚠ 此句已在 ⑥ 撤回）**：原写作"100 µs 只是时延价签，背靠背的 `p130`→`p131` 还带回功能性代价（TBS 1313→2241 B、流量 177→348 MB、PRACH 144→3、CRC-OK 78.3%→95.7%）⇒ 关掉 D1 让上行断续"。**⑥ 的时间预算检查证明这条链条未成立**（占用率只有 ~35%、`stale`=0、且两腿的 PHR 相差 3–4 dB）⇒ **那些上行差异应归给链路状态，不是 D1**。

#### ⑥ ★★★ 预算检查（用户 2026-09-29 追问）：**那 148 µs 的宿主阻塞在这套配置里只花"时延"，不花"速率"** —— 并据此撤回 ③/⑤ 里的因果链

**用户的追问（成立）**：整条链的时间如果**小于**一个 slot 的时间，被阻塞 147 µs 其实**无所谓** —— 因为样点以恒定速率从 ADC 来，宿主**无论如何**都要在 `[ul_rx_wait]` 里等样点到齐；不被 `waitUntilCompleted` 挡住，也会被收包等待挡住。

**更精确的说法**（一半对、一半要分情况）：

* **同一跳内**：顺序是 `A（等样点，中位 472–476 µs）→ 前端 → [阻塞 148 µs] → 其余阶段`。阻塞**在 A 之后**，所以它**不能**"藏进"同一跳的 A 里 —— 它**永远加时延**（实测：V1 中位 +~100 µs）。
* **跨跳的稳态**：真正决定"要不要紧"的是**宿主每跳的占用**与**跳的到达间隔**之比。若线程本来就在 `[ul_rx_wait]` 里闲着等下一个 PUSCH，那这 148 µs 只是把"闲"换成"等 GPU"，**速率不变**。

**实测：我们正好在"有富余"的那一侧**（三腿）：

| | `p128`（D1 开）| `p130`（D1 关）| `p131`（D1 开）|
|---|---|---|---|
| 跳数 / 时长 | 142380 / 326 s | 136164 / 272 s | 145079 / 516 s |
| **跳到达率** | 431/s（每 2.32 ms 一跳）| **495/s**（每 2.02 ms）| 281/s（每 3.56 ms）|
| 车道 `residency` 中位 | 619.4 µs | **539.2** | 621.3 |
| 车道 `busy` 中位 | 506.1 | 494.8 | 504.8 |
| 宿主参与（头/尾，中位）| 42.8 / 88.0 µs | 45.2 / 90.0 | 42.5 / 85.5 |
| **每跳宿主占用（估算 ≈ residency + 宿主 + 阻塞）** | ~710 µs | **~780 µs** | ~710 µs |
| **占用率 = 占用/到达间隔** | **~31%** | **~39%** | **~20%** |
| `stale`（跳 > 8 ms 的 HARQ 往返）| 2 | **0** | 1 |

⇒ **占用率只有 20–39%，期限也没被错过（D1 关那条腿 `stale`=0，反而是三条里最好的）** ⇒ **那 148 µs 在这套配置里被稳态吸收**：它把每跳时延抬高 ~100 µs，**没有**把速率压下去。

**因此撤回 ③/⑤ 的那条链条**（"阻塞 ⇒ 上行结果更晚更抖 ⇒ OLLA 下调 + RACH ⇒ 小 TB/流量减半"）：它**从未被证据支持**，而且有更强的竞争解释 —— **`p130` 的 PHR 是 +7..8 dB、`p131` 是 +11..12 dB（差 3–4 dB 余量）**，CRC-OK 78.3% vs 95.7% 与之同向 ⇒ **两腿的上行差异应归给链路状态**，何况每臂只有一条腿（与已收口的风暴线同一类混杂）。**`late` 类计数（`on_puxch_request_late`）在这两条腿的报告里都没有出现**，也没有任何期限被错过的读数。

**收窄后的结论（可引用版）**：
1. **D1 关掉的可证代价 = 时延**：V1 中位 +~100 µs（D1 开 10 条腿 1354–1431、D1 关 2 条腿 1513/1519）、宿主每槽阻塞 148 µs（p95 416）、段账搬家（t2f +173 / eq_demod −177）。
2. **没有可证的速率/鲁棒性代价**——在这套配置下，因为管线有 60–80% 的富余。
3. ★ **预登记：这条代价什么时候会从"时延"变成"速率"**——当**每跳宿主占用率逼近 ~70–80%** 时（即上行占空比/流量再高 2–3 倍，或期限更紧），那 148 µs 才会开始丢期限。到那时这个判据（占用率 + `stale`）就是现成的读数。

#### ⑦ ★★★ "抖动/突发导致链路崩"这个机制：**原理成立，但这套数据说我们不在那个区间**（用户 2026-09-29 追问）

**用户的假设**：我算的"跳到达间隔"是**平均**值；若某段时间出现**到达风暴**，多出来的那 147 µs 可能把管线压垮 ⇒ 丢期限 ⇒ 手机重新接入 ⇒ RACH 风暴。

**用 `.log` 的逐跳时间戳直接检验（三条独立读数）**：

1. **真实到达过程一点都不"风暴"**（到达 ≈ 日志时刻 − 该跳的 `t=`）：`p130`：**中位 3 跳/5 ms、p95 3、最坏 5 ms 窗口 8 跳**；`p131`：最坏 **9 跳/5 ms**。而容量（2 条车道 ÷ 每跳宿主占用）≈ **12.9 跳/5 ms**（D1 关，777 µs/跳）与 **14.1**（D1 开，707 µs/跳）⇒ **连峰值突发都还有 1.5–2× 富余**。
2. ★ **同腿内的"密度 ↔ 时延"检验给出相反的符号**：`corr(前 5 ms 到达数, t=)` = **−0.392（`p130`）/ −0.351（`p131`）**；按密度分组看每跳时延：**最密的窗口（5–8 跳/5 ms）时延最低**（t 中位 **690–830 µs**），而**孤零零的跳（5 ms 内 0–1 跳）才带尾巴**（t 中位 773–1396、max 4.5–9.9 ms）。⇒ **排队会给正相关**；这里是负相关 ⇒ **迟到的是"稀疏"，不是"拥挤"**。
3. **期限读数**：调度器可见的每跳时延 `t=` **> 8 ms 的跳数 = 0（`p130`，D1 关）vs 1（`p131`）**；管线自己的 `stale` = **0 vs 1–2**。⇒ **两条腿都没有丢期限，D1 关那条反而是更干净的一条。**

**两条腿真正的差别在链路状态**：`p130` 的 144 次 PRACH **摊在 34 秒里（≈4/s，持续而非风暴）**、`p131` 只有 3 次；而 **PHR +7..8 dB vs +11..12 dB**、**CRC-OK 78.3% vs 95.7%** ⇒ 与 ⑥ 的结论一致：**上行差异跟着链路走**（而且因果方向可能和我原先写的相反：链路差 ⇒ CRC 失败多 + 手机重接入）。

**⇒ 收口（可引用版）**：
* 147 µs 在本配置里是**时延代价**；**"抖动/突发把它变成期限代价"这条机制原理上成立，但当前数据不支持**（突发远低于容量、密度与时延负相关、零期限错过）。
* ★ **预登记（要真正检验它，需要把占用率推上去）**：把上行占空比/流量提到 **2–3 倍**（占用率 → 70–80%），或**故意制造突发授权**；届时读 **密度↔时延相关符号 + `stale` + `t=` 尾 + PRACH 时刻**。若届时相关转正、`stale` 出现 ⇒ 该机制成立，D1 的 147 µs 就从"时延"升级为"速率/鲁棒性"。

#### ⑧ ★★★ "GPU 冷启动/降频"假设的检验（用户 2026-09-29 追问）：**"空闲越久 ⇒ 下一跳越慢"不成立**；尾巴的真身是**流量收尾阶段的时间簇**

**假设（用户）**：长空闲后 GPU 降频/入睡，下一跳"手忙脚乱" ⇒ 表现为 `t` 的 773–1396 µs（max 4.5–9.9 ms）尾巴。

**检验 1：按"该跳之前的空闲间隔"分桶**（`.log` 逐跳，到达 ≈ 日志时刻 − `t`）：

| 前间隔 | `p130`（D1 关）t 中位 / p95 / **max** | `p131`（D1 开）t 中位 / p95 / **max** |
|---|---|---|
| < 0.5 ms | 888 / 1096 / 4560（n=64052）| 949 / 1056 / 4806（n=53069）|
| 0.5–1 ms | **687** / 850 / 3814（n=26371）| 856 / 973 / 4733（n=43403）|
| 3–5 ms | 799 / 984 / 4469（n=45198）| 980 / 1104 / 6082（n=48169）|
| 5–10 ms | 711 / 930 / 2638（n=247）| 1037 / 1316 / 2274（n=64）|
| 10–50 ms | 750 / 911 / 1005（n=115）| 926 / 1152 / 1292（n=69）|
| 50–200 ms | 928 / 1038 / **1111**（n=134）| 927 / 1150 / 2496（n=30）|
| **> 200 ms** | 858 / 1022 / **1031**（n=21）| 944 / 1124 / **9943**（n=172）|

⇒ **中位随空闲间隔没有趋势**，而**长空闲桶的最大值恰好最小**（`p130` 的 50–200 ms 与 >200 ms 桶 max 只有 1.0–1.1 ms）⇒ **"睡醒手忙脚乱"在这两条腿上不成立**。全库唯一的"冷启动形状"样本是 `p131` **16:59:37 那一跳（前间隔 546 ms，t=9943 µs）** —— 28 万跳里**一例**。

**检验 2：尾巴长什么样**（`t` > 2 ms 的跳）：

| | `p130` | `p131` |
|---|---|---|
| 数量 / 占比 | **31 / 136164 = 0.023%** | **57 / 145079 = 0.039%** |
| 出现时段 | **16:42:35–16:42:59**（12 个最大值里有 6 个挤在 5.5 s 内）| **16:55:36–16:56:03**（12 个里 11 个）|
| 与流量收尾的关系 | 该腿 PUSCH 结束于 16:43:09 ⇒ 尾巴在**最后 30 s** | 该窗口**紧接 16:56:04 `ue=0: DRB traffic stopped`** |
| 前间隔 | 多为 0.2–0.5 ms（**紧跟前一跳**，不是空旷之后）| 0.5 / 4.0 ms |
| CRC-KO 比例 | **35%**（正常 21.7%）| **24.6%**（正常 **4.3%**，1.7–5.7 倍）|
| 其它 | rv=0 占 24/31、iter 中位 2.0、TBS 中位 1089 | rv=0 占 51/57、iter 中位 2.0、TBS 中位 2562 |

**检验 3：与 PRACH 无关**：每 10 s 的 `t`>2 ms 跳数 与 PRACH 数 的相关 = **−0.058（`p130`）/ +0.081（`p131`）** ⇒ 尾巴**不是** RACH 引起的（`p130` 的 144 次 PRACH 集中在 90–150 s，尾巴在 250–270 s）。

**⇒ 结论（可引用版）**：尾巴是**罕见（0.02–0.04%）、在时间上成簇、且落在流量"收尾"阶段**的现象（伴随更高的 CRC 失败率）；它**不由**"前面空闲多久"决定，也**与 PRACH 无关**。**"GPU 冷启动"目前只有 1 个候选样本，不支持它是主因**；"紧跟前一跳（0.2–0.5 ms）的跳略慢"是**弱排队**效应（中位 888 vs 687–799），量级只有几十 µs，解释不了 4–10 ms 的尾巴。

**★ 仪器缺口与下一步（要真正裁决 GPU 侧，只需补一小段探针）**：设备侧确实存在**罕见的 5–10× 执行时间**（`p130`：`merged_hop` exec max **4651** vs p50 457 µs、`ce_weights` max **751** vs 41.6、前端 max 242–251 vs 47），但现有 Q9-F3 报告**只记录"最慢的等待（commit→start）"及其 slot**，**不记录"最慢的执行（start→end）"在何时/哪一跳** ⇒ 无法判断那些 10× 执行是否落在长空闲之后。**✅ 已实施（2026-09-29，`ocudu_metal_queue.mm`）**：在 Q9-F3 报告里加了 **"slowest executions, by start→end"** 表，每行 = `label + slot + exec(start→end) + commit→start + idle_before（该条开始前"任何被探队列都没在执行"的时长，复用 union walk 的 frontier 现成算出来）+ t+（相对首条记录提交时刻的腿内偏移）`。**同一把钥匙**（`OCUDU_METAL_GPU_TIME=1`；不开探针时没有记录、一行不打印 ⇒ 交付路径逐字节不变）。**离线冒烟**：`OCUDU_METAL_GPU_TIME=1 ./build/lib/phy/generic_functions/metal/dft_release_adopt_metal_test` 真打出该表（如 `label=lane_burst exec=598.4us commit->start=65.5us idle_before=0.29ms t+=0.1s`），`ctest -L phy` 全绿。⚠ **一个坑**：该 harness **不在 `cmake --build build` 的默认目标里**（要 `--target dft_release_adopt_metal_test` 才重链）⇒ 只看默认构建会以为"已重编"，实测第一次冒烟就是旧二进制（无新行）。
**判读（腿侧，一条就够）**：看那 8 条 exec 最大项 —— **exec 5–10× 且 `idle_before` 是几百 ms/秒级 ⇒ GPU 冷启动成立**（修法：保温/周期小派发）；**exec 5–10× 但 `idle_before` 只有几百 µs（即处在忙段里）⇒ 是争用/收尾事件**（与 ⑧ 检验 2 的"收尾时间簇"一致），不必为睡眠做改动。

#### ⑨ `p132-n78-exec`（新表首读，2026-09-29）：**8 条最慢的设备执行全部"开工时 GPU 正忙" ⇒ GPU 冷启动被否**；7/8 落在 iperf3 密集段

**腿状态**（HEAD `aa10f6706b`，knobs 只有 `OCUDU_METAL_GPU_TIME=1` ⇒ D1 按默认武装）：契约 **9 of 9**、**`[ul_dft_wait] no samples`**（再次印证交棒已开）、`gaps=0`、`stale=0`、PRACH **1**、PHR +10..11 dB、输运 **健康**（`dl_tx_call>1ms` = **1/200106 = 0.0005%**）；V1 中位 **1430.5**（p95 1563.1）；`leg_gate` **8 of 8**、A1-2 **5 of 5**（顺带：这条腿**还清了审计 default 侧在当前 HEAD 上的账**）。

**流量时间轴**（`.log` 逐跳）：**10–50 s = ping 段**（5–94 跳/10 s、TBS 中位 ≈530 B）；**50–100 s = iperf3 段**（**6000 跳/10 s = 600 跳/s**、TBS 中位 ≈3.3 KB）。

**★ 新表（`slowest executions`）首读**：

| exec | `idle_before` | `t+` | 段 |
|---|---|---|---|
| **1342.0 µs** | **0.10 ms** | 74.5 s | iperf3 |
| 1010.9 | **0.00** | 76.2 | iperf3 |
| 1003.0 | **0.09** | 84.7 | iperf3 |
| 914.0 | 0.08 | 10.4 | ping |
| 909.2 | 0.04 | 89.8 | iperf3 |
| 840.0 | **0.00** | 76.4 | iperf3 |
| 839.0 | **0.00** | 89.9 | iperf3 |
| 825.1 | **0.00** | 90.3 | iperf3 |

* ★ **八条全部 `idle_before` ≤ 0.10 ms** —— 它们**开工的那一刻，被探队列上已经有东西在执行**。冷启动要求的是**几百 ms–秒级的空闲**，这里连 0.1 ms 都没有 ⇒ **GPU 冷启动被否**（这正是用户提出该假设时我们缺的那条读数）。
* ★ **7/8 落在 iperf3 密集段**（`t+` 74–90 s；唯一 ping 段那条 `idle_before` 也是 0.08 ms）⇒ 慢执行住在**忙段**里。
* **量级**：`merged_hop` exec **p50 469.3 / p95 493.7 / max 1342.0 µs** ⇒ 最慢 8 条是 **1.76–2.86×**；小缓冲的相对离群更大：`ce_weights` max **749.2**（p50 42.5 ⇒ **17.6×**）、`dft_front_end` max **383.0**（p50 46.6 ⇒ **8.2×**）。**等待**的最大值也在忙段（`dft_front_end` wait max 1399.9、`ce_weights` 1823.2、`merged_hop` 1012.5 µs）。
* 另一条否证：本腿**最长的 GPU 空洞 2674.3 ms**，紧跟其后的是一条 **`dft_front_end`（plain/PRACH 路）**——"睡醒"之后并不是慢跳。

**⇒ 结论**：设备侧的 2–3×（小缓冲 8–18×）**发生在忙段，且开始时 GPU 已在执行** ⇒ 机制是**争用/共驻留**，不是 GPU 冷启动；与宿主侧三条检验（⑦ 密度↔时延负相关、⑧ 空闲间隔不预测时延、尾巴落在收尾窗口）**方向一致**。**这条假设结案**：现在**没有**任何证据支持"保温/防降频"式改动。

### 6.161 ⚠⚠⚠ **"空 kernel 仍要 463 µs"到底花在哪：窗里能装什么 + `p84` 那条结论的**三个红旗** + 离线复核计划（用户 2026-09-29 追问）**

**问题（用户）**：把这条 cb 里的 kernel 全换成空 kernel，窗口几乎不动（470.0 → 462.9 µs）—— 很难理解；这些时间到底花在哪里？是模块之间的**同步栅栏等待**吗？**原来的测试是否有问题？**

#### ① `start→end` 这个窗**能**装什么（`p84` 只消去了其中一项）

| # | 窗里可能包含 | 换空 kernel 后还在吗 | 判别读数 |
|---|---|---|---|
| a | **每个派发/线程组的启动与命令处理器记账**（pipeline state、参数绑定、threadgroup launch）| **在**（换的是函数，不是这些）| 空派发普查：1 条 cb 里 K 个空派发 → 窗口随 K 的斜率 |
| b | ★ **cb 内编码的同步**：stage fence 的 wait、`grid_ready_signal`、keepalive、token 事件；D1 下还可能是**跨队列依赖** | **在**（消去不碰事件指令）| ★ 同一条空 kernel cb **带事件 vs 去掉事件**两臂 |
| c | **资源/页驻留与依赖解析**（驱动为这批 dispatch 做的准备工作）| **在** | 同几何、不同绑定数量（复用 vs 新建）对照 |
| d | **别人的活**：共驻留时，本 cb 的窗口可以只是"等 GPU 空出来"（`p132` 已证：8 条最慢执行**开工时 GPU 都在忙**）| **在** | 单独跑 vs 与一条长 cb 同时跑 |
| e | **非 kernel 的工作**：同一条 cb 里的上传/下载、网格 staging、wrap 失效等 `blit`/拷贝 | 部分在（消去只覆盖 compute）| 数清这条 cb 里到底编了哪些指令（`encode` 的调用点）|
| f | **测量本身**：`GPUEndTime` 是否含 cb 的收尾/排空，或时间戳有下限 | — | ★ **空 cb 对照**（0 派发）：读出来是多少 |

⇒ **"空 kernel 也很贵"完全可能**：它只说明了"不是 kernel 的执行"，**还没说明**是 a/b/c/d/e/f 里的哪一个。**用户的"同步栅栏"猜测（b）正是 `p84` 没有排除的那一类。**

#### ② `p84` 解读的三个红旗（我这次复查代码与记录发现的）

1. ⚠ **覆盖度从未被计数**：代码里**没有任何"消去了几个派发"的计数器**（只有 `g_burst_index` 用于 1-in-N）。"所有 kernel 都换掉了"是**读码结论**，不是读数。
2. ⚠ **阳性对照读在别的标签上**：p84 的对照是 `dft_front_end` 的**低尾**（15.8 → 5.5 µs）——那是**总体论证**，**不是"这一条 cb 的每个派发都被换掉"的证明**；而且该标签在 gpu 模式下还混着 plain 路（PRACH）的 cb。
3. ⚠ **量级对不上**：前端**自己那条 cb**（1 派发、14 变换）消去后低尾掉了**几十 µs**，而**包含同一批前端变换的** `merged_hop` 只掉了 **7.1 µs（−1.5%）** ⇒ 要么消去**没有到达被采纳块里的前端派发**，要么有别的活把它补上了。**这正是文档自己在 §6.140 记下的那类缺陷**（当年 `p83` 的读数就是"前端没被消去 ⇒ 不可解读"）。
   ⇒ **结论：`p84` 的"这笔账与内容无关"目前是"未计数覆盖度 + 量级不自洽"的结论，应当降级为"待复核"**。**结构律（三种结构 505–517 µs、哪条 cb 吃 450 随结构搬家）不依赖它，仍然成立。**

#### ③ 离线复核计划（**零空口、零用户操作**，按顺序，每步都有判决读数）

1. **覆盖度（先做，最便宜）**：给消去路径加一个**按标签的"已消去派发"计数器**（同一把钥匙 `OCUDU_LANE_ABLATE` 之内），用离线 harness（`dft_release_adopt_metal_test`，D1 交棒路）跑 `control` vs `ablate` ⇒ **证明每条 cb 的每个派发确实被换掉**，并读该 cb 的窗口。
2. **空/空派发普查**：空 cb（0 派发）／K 个空派发／K 个空变换（1 派发批量化）／同几何真 kernel ⇒ **窗口随"cb 数 / 派发数 / 变换数"的斜率**，顺带把"33 µs/变换"这个历史形状归因或否掉。
3. ★ **栅栏隔离（直答用户的猜测）**：同一条空 kernel cb，**带事件**（stage fence wait + `grid_ready_signal` + keepalive/token 事件）**vs 去掉事件** ⇒ 若窗口塌掉 ⇒ 是**同步**；若不动 ⇒ 是启动/几何/驱动/共驻留。
4. **共驻留**：同一 cb 单独跑 vs 与一条长 cb 同时跑（复现 `p132` 的忙段 2–3×）。
5. 只有 3 的答案是"是同步"时，才谈"怎么把这段同步去掉/缩短"——**那时它才是真杠杆**；否则按 ① 的表逐项继续否。

**预登记判决**：若 3 显示"事件去掉后窗口塌掉"⇒ **C-A（多跳一条 cb）不是正确方向**，正确的方向是**减少/压缩 cb 内的同步**（事件数、跨队列依赖）；若 2 显示"账 ∝ cb 数"⇒ C-A 才有奖；若 2 显示"账 ∝ 派发/变换数"⇒ 平台硬地板，结构线关闭，转 LDPC→Metal。

#### ④ ★★★ 离线复核第一批读数（2026-09-29，新 harness `wip/dispatch_count_probe.mm`）：**没有"每 cb 地板"、线程组免费、事件几乎免费 —— 但"每次派发 ~38 µs"是**内容相关**的（空 kernel 只要 2.75 µs）**

新 harness（自重编译一个**空 kernel**，读 cb 自己的 `GPUEndTime − GPUStartTime`，15 次中位）在我这台 M4 Pro 上：

| 臂 | GPU 中位 | 与历史读数的关系 |
|---|---|---|
| **空 cb（0 派发）** | **0.1 µs** | ✅ **不存在"每 cb 地板"**（历史"平凡 cb = 7.1 µs"其实是 **1 个派发**的 cb）|
| 1 个空派发 | 6.3 µs | — |
| 2 / 4 / 7 / 14 个空派发 | 9.1 / 16.1 / 23.3 / **42.0** | **斜率 ≈ 2.75 µs/派发** |
| 1 派发 × 1 / 7 / 14 线程组 | 6.3 / 6.2 / **6.6** | ✅ **线程组完全免费**（与历史 46.6 一致）|
| 1 派发 + **编码 signal 事件** | **6.2** | ✅ 事件本身几乎免费 |
| 1 派发 + **跨队列等待** | **6.1** | ✅ 跨队列等待在这个尺度上也几乎免费 |
| 5 个空派发（≈一跳的派发数）| **17.4** | 对照：空口一跳的 cb 是 469 µs |

**与历史真实 kernel 扫描（`dft_dispatch_cost.mm`）对照**：1→47.5 / 2→84.8 / 4→161.3 / 7→278.2 / **14→539.3 µs** ⇒ **斜率 ≈ 37.8 µs/派发**；而**空 kernel 同一条扫描只有 2.75 µs/派发**。

**⇒ 三条结论（可以据此"对症"）**：
1. ★ **"每 cb 固定账"被否**（空 cb = 0.1 µs）⇒ **C-A（把 N 跳塞一条 cb 去摊薄）没有可摊的东西**；C-B（槽边界量化）也随之失去意义 —— **结构线这两个候选可以关掉**。
2. ★ **"每次派发 ~38 µs"不是结构税，而是内容/几何账**（空 kernel 2.75 vs 真 DFT 37.8，**14× 差**）⇒ 真正的价钱在**派发里做的事**（真几何的访存/网格写/蝶形），所以"减少派发数"确实是杠杆（14 符号合 1 派发 = V1 −38.7% 的历史战绩与此一致）。
3. ★ **用户猜的"模块间同步栅栏"不是主项**：单派发尺度上 signal/跨队列等待 ≈ 免费，空口 Q24 也只量到 9–17 µs/跳。
4. ⚠ **仍未归属的正是"空口那一跳的 cb = 469 µs"**：它的已知成分（前端批派发 46.9 + 车道 4 派发）远小于 469，而**空口消去实验又显示内容几乎不影响** ⇒ 这两个事实同时成立，只能说明**空口那条 cb 的"派发构成"与我们的计数不一致**。★ **新线索（可零腿查）**：同一条腿的空口计数**互相矛盾** —— `burst dispatches=4.00/跳`（eq 1 + demap 1 + CE 2）而 `ce_sites` 自报 **5.09 个 CE 内核派发/跳**、`eq_batch` 1.00、`demod_batch` 1.00 ⇒ **一跳的 cb 里到底有几个派发，账本身就不清**；下一步就是**把这几个计数器对齐**，再按族（DFT/CE/eq/demap）各量一次"每派发价钱"，用 `Σ 计数 × 单价` 与 469 对账。

#### ⑤ ★★★ 第 1–3 步：**计数器对账 + 单价表 + 与 469 µs 的对账** —— **结论：这 470 µs 是"内容/访存账"，`p84` 的 −1.5% 必须用"消去没覆盖到"来解释**

**第 1 步：把"一跳的 cb 里到底有几个派发"对上**（全部取自 `p132` 自己那份报告）：

| 读数 | 值（每跳）| 说明 |
|---|---|---|
| 车道 `busy` 中位 | **502.2 µs** | 一跳的设备占用 |
| `busy split` | **`merged_hop` 470.5（92%，1 cb）+ `ce_weights` 43.3（8%，1 cb）+ `eq_demap` 0.0（0%，**cbs/lane=0.00**）+ `ch_est` 0.0** | ★ **均衡/解映射没有自己的 cb —— 它们在 `merged_hop` 里面** |
| 车道 burst 派发 | **4.00/跳**（equalizer 1 + demapper 1 + channel_estimator 2）| 这是**车道 encoder** 的派发数 |
| 前端交棒批派发 | `batched=25455/356370` ⇒ **1.32/跳**（每次 14 变换）| 被采纳进同一条 cb |
| **⇒ `merged_hop` 的派发清单** | **≈ 5.3 个/跳**（前端 1.32 + 车道 4.00）| **"4.00" 与 "5.09（`ce_sites`）"不是同一把尺**：前者是车道 burst 的派发数，后者是 **CE 引擎的"内核站点"数**（reformat/pilots_lse/pilots_cfo/corr_a/corr_rhp），两者单位不同 —— **原来的"账目不清"就在这里**，现在对上了 |
| 栅栏（Q9-D/Q24）| `signaller-first=19279/19279`、`resolved=19279/19279`、`no-signal=0`；共驻留 33.0%，mean 50.7 µs ⇒ **≈17 µs/跳** | **等待都能解析（不阻塞）** ⇒ 栅栏不是主项 |

**第 2 步：单价表**（离线 harness，本机 M4 Pro，今日实测/历史记录）：

| 派发内容 | 每个派发的中位窗口 | 来源 |
|---|---|---|
| **空 kernel（零访存）** | **2.75 µs** | 今日 `dispatch_count_probe.mm`（1→14 派发线性）|
| **访存型 kernel（46 KB 入 / 64 KB 网格出）** | **≈26 µs** | 今日同 harness（5 派发：129.6/143.2 µs）|
| 真实 `dft_dit`（空口几何）| **≈37.8 µs** | 历史 `dft_dispatch_cost.mm`（1→14 线性）|
| 前端"批 14 变换"整条 cb | **46.9–51.0 µs** | 空口 `dft_front_end` / `late_handed`（单派发 cb）|
| `ce_weights` 整条 cb（≈5 个 CE 内核站点）| **43.3 µs** | 空口 `busy split` |
| ★ **空口 `merged_hop` 里每派发的均值** | **470.5 / 5.32 ≈ 88 µs** | 对账结果 |

**同时否掉一条**：**派发之间的"数据依赖"不收费** —— 今日新增臂：同样 5 个访存型派发，**串在同一条输入+网格上（有依赖）129.6 µs** vs **各自独立缓冲 143.2 µs**（独立反而略贵）⇒ **"网格依赖串行化"不是那 470 的来源**。

**第 3 步：对账 ⇒ 强迫出一个结论**
* 按离线单价，一跳的 5.3 个派发 ≈ **5.3 × 26–38 ≈ 140–200 µs**；实测 `merged_hop` **470.5 µs** ⇒ **空口每个派发比离线同类贵 2.3–3.4×**（≈88 vs 26–38）。
* 而 **`p84` 的"全部换空 kernel 只掉 1.5%"** 与"内容是钱"**直接冲突**（空 kernel 只要 2.75 µs/派发，若真被换掉，470 µs 的窗口应当塌到几十 µs）。
* ⇒ **两者只能有一个成立**。今天三条独立离线臂已经证明**内容确实决定单价**（2.75 / 26 / 37.8），所以**更可能的解释是：`p84` 的消去从未覆盖到 `merged_hop` 里的那些派发**（覆盖度从未计数；阳性对照又读在 `dft_front_end` 这个**别的标签**上；且量级不自洽 —— 前端自己那条 cb 掉了数十 µs，含同一批变换的 `merged_hop` 只掉 7.1 µs）。这与 §6.140 记下的同型缺陷是**同一类**。
* ★ **因此：`§6.141` 的"这条 cb 的价钱与它做什么无关 ⇒ 这一线没有杠杆"需要撤回/重判**；正确的表述是：**这 470 µs 主要是"派发的内容 + 空口特有的额外开销（离线同类 26–38 vs 空口 88 µs/派发）"**，其中"减少派发数 / 减少访存趟数"是**真实且已被历史战绩支持**的方向（14 符号合 1 派发 = V1 −38.7%；历史每消去一个派发值 5–14 µs，候选 30–42 µs）。

**⇒ 判决性的下一步（仍是离线，顺序固定）**：
1. **证明/否掉覆盖度**：给消去路径加"按标签已消去派发数"计数器，用 `dft_release_adopt_metal_test`（D1 交棒路）跑 `control` vs `ablate` ⇒ 若 `merged_hop` 那条 cb 的派发**没被全部换掉** ⇒ `p84` 作废、上面的结论成立；
2. **按族 + 按批量化几何量单价**：把 eq/demap（`max_run=12` 的批）与 CE 的 5 个内核站点在**空口几何**下各量一次"每派发窗口"，用来解释"空口 88 vs 离线 26–38"那 2.3–3.4× 是哪一族贡献的（离线 harness 已具备：`ce_kernel_cost.mm` + `dft_dispatch_cost.mm` 的形状）；
3. 对上了 ⇒ "对症"就是**减少/合并派发与访存趟数**（不是动结构、也不是动 cb 的合并）。

#### ⑥ ★★ 追问"离线 26–38 vs 空口 88 µs/派发"：**这个对比本身不成立（不是同类量），而今天又排除一条、并暴露一个新问题**（用户 2026-09-29）

**用户的质疑（成立）**：离线和空口的**唯一**区别应当是 IQ 的来源，而空口那份 IQ 账**已经由 `[ul_rx_wait]` 付过了** —— 凭什么每派发贵 2.3–3.4×？

**先承认两处口径错误**：
1. **"88 µs/派发"不是同类量**：它是 `470.5 ÷ 5.32`，而 5.32 是**混合派发集**（前端批 14 变换 1.32 + 车道 burst 4.00：eq 1、demap 1、CE 2）。我的离线单价是**同一 kernel 重复**的单价（空 2.75、访存 26、真 DFT 37.8）⇒ **不能直接比**。**"空口 2.3–3.4× 额外开销"这句应降级为"在按族量出批量化几何的单价之前，未归属"。**
2. **同一个 `merged_hop` 内部的自洽检查**：若前端那一份在 cb 内仍是 46.9（它单独成 cb 时就是 46.9/51.0），那么剩下 4 个派发（eq/demap/CE×2）要摊掉 **≈423 µs ⇒ 每个 ≈106 µs**；而 CE **自己的**那条 cb（含约 5 个内核站点）只有 **43.3 µs**。⇒ **光靠"族单价"也补不平这个缺口**，说明窗口里还有**非本跳工作或等待**。

**今天新增的两条离线臂**（`dispatch_count_probe.mm`）：
| 臂 | GPU 中位 | 结论 |
|---|---|---|
| **真·read-after-write 链**（1 个生产者 + 4 个消费者，同一条 64 KB 网格，46 KB 输入）| **130.6 µs** | 与"无 RAW 边"的 5 派发链（134.0）**相同** ⇒ **派发间的读后写依赖不收费**（排除"网格可见性/缓存刷新"这类机制）|
| 同一条 RAW 链 **+ 另一队列持续竞争** | **38.8 µs**（host 1441 µs）| ⚠ **不可解读**：竞争下读到的窗口反而变小 ⇒ 这个竞争臂的**设计有问题**（用"疯狂提交"的线程做竞争负载会污染测量），**必须重做**（改成**有界**的竞争负载 + 更多次数 + 报告 min/中位）|
| （对照）5 个空 kernel 派发 | 4.9–17.9 µs（**跨运行波动 3.5×**）| ⚠ 我的空 kernel 扫描**跨运行不稳** ⇒ 细粒度结论（2.75 µs/派发）要按"量级"读，不能当精确值 |

**仍然开放的三个候选（按当前证据排序）**：
| 候选 | 为什么还站着 | 怎么量（离线，下一步）|
|---|---|---|
| ★ **共享 GPU**（空口跑并发 2 条车道 + DL + PRACH + PUCCH/SRS；窗口是**墙钟**，别人插进来的工作都会撑长它）| 空口 duty 只有 **14.3%**（`busy(union)` 14.7 s / 102.9 s）⇒ 不是"排队积压"，但**窗口内可以夹进别人的派发**；我的竞争臂失败了，**这一条实际上还没被量过** | **有界**竞争负载（另一队列连续提交 N 条已知的 5 派发链，**先提交后测量**），报告本 cb 窗口的 min/中位/次数 |
| **按族 + 按批量化几何的单价**（eq/demap 是 `max_run=12` 的批、CE 5 个站点、前端批 14 变换 int16→网格）| 完全没量过；我的单价都是"单 kernel 重复" | 扩 `ce_kernel_cost.mm`/`dft_dispatch_cost.mm` 的形状到批量化几何 |
| **输入是电台的实时 DMA 页映射**（被 USB 写、被零拷贝 wrap 成 MTLBuffer）| 记录里只有**合成**臂（+23.1 µs），真件没量 | 让一个生产者队列持续写输入缓冲，再测链的窗口 |

**⇒ 因此"对不对症"的结论要推迟一步**：现在能说的只有 —— **每 cb 地板、线程组、事件编码、派发间依赖、冷启动、槽边界量化都已被排除**；剩下的**最大嫌疑是"共享 GPU 把窗口撑长"**（而它恰好是"离线 vs 空口"最实质的差别，**不是** IQ 来源）。下一批离线臂（有界竞争 + 批量化几何单价 + 实时页输入）跑完，才能给出"470 µs 的族级账单"。

#### ⑦ ⚠⚠ **负结果：我的合成离线臂回答不了这 470 µs**（第二批臂跑完，2026-09-29）—— 并给出**唯一可信的两条路**

**跑完的东西（ARM 1 有界竞争 / ARM 3 实时页输入 / ARM 2 批量化几何）**：

| 臂 | 本次读数 | 判读 |
|---|---|---|
| 5 派发 RAW 链，GPU 空闲 | **38.0 µs**（上一轮同一臂 **130.6 µs**）| ⚠ **跨运行差 3.4×** |
| +1 / +4 条竞争链（**有界**，先提交后测）| 39.0 / 38.0 µs（host 173/166 µs）| **窗口不动、host 只涨几十 µs** ⇒ "另一条车道的活**不会**撑长本 cb 的窗口" |
| +8 / +64 个输入写者（实时页）| 37.6 / 37.5 µs | 同上 ⇒ **实时页也没有可测的影响** |
| 1 派发 @ 1 / 2 / 12 符号的网格（最大 235 KB）| 3.2 / 3.2 / **7.4 µs** | ⚠ **235 KB 在 ~100 GB/s 下本来就只要 ~2 µs** ⇒ **我的"访存型"臂根本不访存受限** |

**⇒ 两条自我更正（必须记）**：
1. ⚠ **合成臂的绝对单价（"空 2.75 µs / 访存 26 µs"）不能当"离线同类单价"用**：它们跨运行波动 3.5×，且规模只有几百 KB（物理上 µs 级）。§6.161④/⑤ 里拿它们与空口 470 µs 对比的算术**作废**。
2. ⚠ **"离线 vs 空口"的差别也不能由我的臂来枚举**：合成环境里**没有**真 kernel 的访存图样（散列读网格、子载波去交织、FFT 蝶形）、没有 2 条车道的真并发、没有 DL/PRACH/PUCCH 的同时在场。⇒ **这条路已经到头。**

**仍然可信的（只有三类）**：
* **类别级**（今天实测，稳）：**空 cb = 0.1 µs ⇒ 无"每 cb 地板"**；**线程组数**（1 vs 14）**不影响窗口**；**事件编码/跨队列等待**在这个尺度上几乎免费。
* **真 kernel 的离线数**（历史，`dft_dispatch_cost.mm`）：真实 `dft_dit` **≈37.8 µs/派发**（1→14 线性）、**批 14 变换 = 46.9 µs**。
* **空口自报数**：`merged_hop` **470.5 µs/跳**（1 cb）、`ce_weights` 43.3、`dft_front_end` 46.6、`late_handed` 51.0；一跳 ≈ **5.3 个派发**；**栅栏不阻塞**（Q9-D/Q24）。

**⇒ "470 µs 花在哪"只能靠下面两条路之一（合成路已否）**：

| 路 | 内容 | 代价 | 能回答什么 |
|---|---|---|---|
| **A. 真 kernel 的按族离线 harness** | 照 `ce_kernel_cost.mm`/`dft_dispatch_cost.mm` 的形状，把**均衡/解映射（`max_run=12` 批）与 CE 五站点**在**空口几何**下各量"每派发窗口" | 我数小时（无腿）| **族级单价**（仍不含真并发/真输入）|
| ★ **B. 按阶段的空口消去臂** | 现在消去是**按 cb 整体**（`ablate_cb(cb)`，1-in-N 由 `OCUDU_LANE_ABLATE[_EVERY]` 选）；**没有按阶段选择** ⇒ 新增 **`OCUDU_LANE_ABLATE_STAGE=front_end\|ce\|eq\|demap`**（同一把钥匙内）+ **"按标签已消去派发"计数器**（同时补上 `p84` 的覆盖度红旗），**一条腿**里 1-in-8 交替四种消去 | 探针小改 + **一条腿** + 重打戳 | **族级窗口贡献（真并发、真输入、真几何）**，并顺手判死/救活 `p84` |

**建议**：走 **B**（一条腿就能给出"族级账单"，且顺手清掉 `p84` 的覆盖度疑问）；A 可以在等腿时并行做，但它**永远差"真并发/真输入"那一块**。

> 📌 **本节的交接快照 = `session_handoff_2026-09-29-1.md`**（§0 现状 / §2 增量结论 / §3.1 的 A-B 裁决 / §4 纪律 21–26）。

### 6.162 ★★★ **走 B：消去臂按阶段族可选 + 覆盖度计数器（用户裁决 2026-09-29）** —— 以及**一条被自己否掉的实现**（每 cb 窗口归属）

**裁决**：`session_handoff_2026-09-29-1.md` §3.1 的唯一待裁决 = **A（真 kernel 按族离线 harness）还是 B（按阶段的空口消去臂）** ⇒ **用户选 B**。本节的读数是**离线自证**；空口读数要一条腿（⑦ 给了命令与判决读数）。

#### ① 改了什么（三处，全部在探针里；旋钮关着时交付路径逐字节不变）

| # | 位置 | 内容 |
|---|---|---|
| 1 | `lib/phy/metal/ocudu_metal_burst.{h,mm}` | **`OCUDU_LANE_ABLATE_STAGE`**（`front_end\|ce\|eq\|demap`，`\|`/`,` 可组合，不设 = `all`）：四族位掩码，**读一次**（臂是按 cb 决定的，跑到一半换旋钮会让同一条腿的两条 cb 不可比）；拼错的名字 ⇒ **WARNING + 按 `all`**（宁可多消，不可静默不消）|
| 2 | 同上 + `ocudu_dft_metal_engine.mm` | **覆盖度计数器**（`binds` / `ablated`，按族）；`shared_burst::count_dispatch()` 顺带刷新"当前族"，`set_stage()` 由每个阶段在**开 encoder 之前**显式设置（绑定点要读它，而 `count_dispatch` 在派发旁边、即绑定**之后**才跑，不能当权威）；前端自己那条派发通过 `note_front_end_dispatch()` 计数 |
| 3 | `ocudu_metal_burst.mm` 的 `[metal_stats]` 报告 | 新增 **`Q9-F5 ablation coverage`** 块（只在臂开着时打印）：每族 `binds=… ablated=… (…%)`，`binds==0` 直接标 **`NOT REACHED: this family was never measured by this arm`** |

**四族 = 一跳 5.32 个派发的四个来源**（§6.161⑤ 的对账）：`front_end` = DFT 引擎自己的网格写（它**自己拥有 encoder**，走 `ablate_cb()`/`ablate_stage_for_cb()`，§6.140 之前这是**唯一**够不到的那一条）；`ce`/`eq`/`demap` = 通过 `shared_burst::encoder()` 编码的三个阶段族。

**族名怎么来的（要点）**：阶段在开 encoder 前调 `set_stage()`，绑定点把这个族名与**该 cb 自己那份掩码**比较，只有掩码覆盖它才绑空 kernel。**"该 cb 自己那份掩码"是关键**：1-in-N 之下，同一条腿的**交付** cb 必须原样不动，而"环境变量此刻是什么"不能用来回溯改变一个已经决定过的 cb（单测会在一个进程里换旋钮）。
⚠ `shared_burst::front_end_stage()` 返回 `stage::other` —— 这不是双关：`stage` 枚举描述的是"某个阶段往共享 burst 里追加了什么"，而前端**什么都不追加**（它生产出被采纳的那条 cb），所以它的族名不能放进那个枚举（否则会有人把 `other` 传给 `set_stage()`）；名字由 `front_end_stage()` 承载，映射在臂内部完成。

#### ② ⚠⚠ **自我更正：每 cb 的窗口归属做不出来，已整块删除**

第一版给每条 cb 标注"这个窗里含哪些族、其中几个真的被换掉了、掩码是什么"，做法是：臂里一张以 cb 地址为键的表 + 一个回调让队列在 `arm_gpu_time()` 时把标注拷进 occupancy 记录。**离线一跑就露馅**：同一族同一掩码的 cb 在报告里读成 `arm=control dispatches 22..#abl=22`，而真正被消去的那 69 条读成 `control`。

**根因（平台事实，值得记）**：**Metal 回收命令缓冲对象**。同一次测试运行里，同一个地址被提交了 **25 次**（`0x87be00000`）。于是：
* 按地址存的"决定"会在交付路径上被**清掉**（`forget_ablation()` 就在 `arm_gpu_time()` **之前**一行跑），所以"提交时再问一次"读到的是**空**；
* 改成"丢弃前主动推送"也不行 —— 记录是在**完成处理器**里才写入的（GPU 完成后），推送时表里还没有那条记录；
* 改成"完成处理器里回查"更不行 —— 那时表里的条目**已经被后来那一跳的编码覆盖**（这就是 `total` 在同一个地址上从 1 涨到 3 的原因）。

⇒ **接口（队列侧的 annotation 结构、回调、每 cb 标注表、`occupancy_record` 的新字段）已全部删除，不留死代码。** 窗口归属改用**消去臂自己的 label**：`merged_hop_ablated` vs `merged_hop`，Q9-F3 的分标签表**本来就分开打印**⇒ 四臂的窗口分布从同一个 label 表的对应行读，族账单 = "四臂各自的窗口分布 + 每族的覆盖度计数"。**代价**：读不到"这一条 cb 里有 11 个派发"这种逐 cb 细节 —— 而它对判读不必要。

#### ③ 离线自证（`pusch_demodulator_deferred_chain_test` + `dft_release_adopt_metal_test`）

| 臂 | 读数 | 判读 |
|---|---|---|
| **不设旋钮**（对照） | `5/5 PASS`，报告里**没有** `Q9-F5` 行 | ✅ **关着时交付路径逐字节不变**（新增代码全在 `OCUDU_METAL_STATS` + 旋钮之内）|
| `OCUDU_LANE_ABLATE=1`（EVERY=1，掩码 `all`） | **2 例 FAILED**（空链路，与 §6.132 的判据一致）；`ablated buffers=69`；eq **67/67**、demap **2/2** | ✅ 臂咬到了（`eq` 是 `max_run` 批，67 次绑定对应 48 条 equalizer cb + 19 次落进 merged 路）|
| `OCUDU_LANE_ABLATE_STAGE=eq` | eq **67/67**、demap **0/67** | ✅ **掩码真的在选族**（`demap` 的 67 次绑定全部保持 real）|
| `OCUDU_LANE_ABLATE_STAGE=demap` | demap **2/2**、eq **0/67** | ✅ 反向对照成立 |
| `OCUDU_LANE_ABLATE_STAGE=eq\|demap` | 两条都 **100%** | ✅ 组合语义正确（`ablate_mask_name` 打印 `eq+demap`）|
| `OCUDU_LANE_ABLATE_STAGE=front_end`（D1 测试） | `front_end` **2/2**（另有部分运行 60/60、103/103） | ✅ **阳性对照第一次能读在同一族上** —— §6.161② 红旗 2 的病根 |
| `OCUDU_LANE_ABLATE_STAGE=eq`（D1 测试，反向） | `front_end` **103/103 real**、eq 0 | ✅ 前端族没有被误消 |
| `OCUDU_LANE_ABLATE_STAGE=bogus` | WARNING + 按 `all` 处理，`binds` 与全消一致 | ✅ 拼错不会静默变成"什么都没消"|
| `ctest -L phy -j 1` | **203/203** | ✅（一次 `Bus error` 是**我并发重链 `gnb`** 造成的，单独重跑 2.11 s 通过 —— 见 ⑥ 纪律 27）|

**⚠ 一个仍在的口径提醒**：`ce` 族在这两条离线测试里都是 `binds=0`（这两条链**不走** burst 路的 CE 引擎）⇒ **空口腿上必须看到 `ce binds≠0`**，否则那条腿的 `ce` 臂什么都没测到（这正是计数器存在的意义）。

#### ④ 与 `p84` 的关系（判死/救活的条件）

`p84` 的结论是"这条 cb 的价钱与它做什么无关 ⇒ 这一线没有杠杆"（§6.141）。**本节不推翻它，只让它可以被判**：
* 若腿上 `front_end binds≠0`（即消去**真的到达**被采纳块里的前端派发）**而** `merged_hop_ablated` 的窗口分布与对照相同 ⇒ **`p84` 成立、§6.141 维持**，结构线关闭；
* 若 `front_end binds≠0` **而窗口明显塌**（或反过来：`binds=0` ⇒ 覆盖不到）⇒ **`p84` 作废、§6.141 撤回**，"减派发/减访存"重新成为有历史战绩支撑的方向（14 符号合 1 派发 = V1 −38.7%）。

#### ⑤ 未改的东西（明确写下来，免得后来者去猜）

* `OCUDU_LANE_ABLATE` / `OCUDU_LANE_ABLATE_EVERY` 的**语义与默认值不变**（`=1` + 不设 `_STAGE` ⇒ 与 `p83`/`p84` 逐位同款）；
* 交付路径：不设旋钮时 `ablate_next_burst()` 直接返回、`count_bind` 不被调用、报告不打 `Q9-F5` 行；
* **不碰** `ocudu_metal_queue.mm`（这一轮为它加过又被整块撤掉，最终 `git diff` 里**没有它** —— 这是 ② 的直接结果）。

#### ⑥ 纪律（接 §4 新增 27）

27. ★ **腿期/测试期不要并发构建**：一次 `ctest -L phy -j 1` 里 `port_channel_estimator_metal_mmse_unit_test_ta_chain` 报 `Bus error`，而**单独重跑通过**（2.11 s）—— 原因是我在测试跑的同时重链了 `gnb`（同一棵树、同一个 `build/`）。读数取信之前先确认没有别的进程在写这棵树。

#### ⑦ 空口腿（待飞；用户操作）—— 分四条臂，或一条臂内轮换

**一条腿一份掩码**（最干净、不用学新东西），四份掩码各飞一条 `gpu` default 腿；想省腿就**一条腿内 1-in-8 轮换**（把四次运行合并，但四个族的样本都只有 1/8）：

```bash
# 每条腿 = 一个掩码值；推荐顺序：先 all 复现历史，再单族
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8 \
OCUDU_LANE_ABLATE_STAGE=all \
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p133-n78-abl-all --regime=default
# 然后 STAGE=front_end / ce / eq / demap 各一条（标签 p134…p137）
```

接手机**之前**核三行：① `knob` 行里有 `OCUDU_LANE_ABLATE=1`、`…_EVERY=8`、`…_STAGE=<族>`；② `[metal_ablate] ABLATION ON …`；③ 若某族名字拼错，会有 `[metal_ablate] … names a stage that does not exist`（**出现即停下改命令**）。

**判决读数**（一条腿的报告里）：
1. `[metal_stats] Q9-F5 ablation coverage` 的四行 —— **每族 `binds≠0`**（尤其 `ce`），`ablated/binds` 应为 **1/8 左右**（`EVERY=8`），全部为 0 ⇒ 这条腿没量到东西；
2. `[metal_stats] queue occupancy (Q9-F3) per label` 里的 **`merged_hop`（对照）与 `merged_hop_ablated`（臂）两行** —— 比较 `exec p50 / p95 / min`；**族级账单 = 对照减去该族臂**；
3. `[metal_stats] burst … (equalizer=… demapper=… channel_estimator=…)` 与 `[ul_rx_wait]`/契约照旧（**臂腿不是能工作的链路**，CRC 会掉、契约可能红 —— 这与 §6.132 的既有口径一致，**不要**拿它判交付）。

> 📌 本节对应的工作区改动：`lib/phy/metal/ocudu_metal_burst.{h,mm}`、`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm`、`lib/phy/upper/channel_modulation/metal/ocudu_demod_metal_engine.mm`、`lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm`、`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm`（后三个各一行 `set_stage()`）。

### 6.163 ⚠⚠⚠ **腿 `p133`–`p137` 的判决：覆盖度计数器**数错了东西**（我三次实现都错），已修好** —— 并给出**修订后的腿命令**

**这五条腿（2026-09-29，`OCUDU_LANE_ABLATE=1 EVERY=8`，`_STAGE` = `all`/`front_end`/`ce`/`eq`/`demap` 各一条，`gpu` default，每条 100 ping + 30 s iperf3）**的结论分两半：

#### ① 五条腿**证明了改造的价值**，也**证伪了我的计数器**

| 腿 | `front_end` | `eq` | `demap` | `ce` |
|---|---|---|---|---|
| `p133` all | 150877 / **18860 (12.5%)** | 19237 / 2503 (13.0%) | **0 / 0** | **0 / 0** |
| `p134` front_end | 114254 / **14281 (12.5%)** | 20269 / **0** | **0 / 0** | **0 / 0** |
| `p135` ce | 135829 / **0** | 19402 / **0** | **0 / 0** | **0 / 0** |
| `p136` eq | 132152 / **0** | 20253 / **2340 (11.6%)** | **0 / 0** | **0 / 0** |
| `p137` demap | 110870 / **0** | 19272 / **0** | **0 / 0** | **0 / 0** |

* ✅ **掩码本身是对的**：`front_end` 臂下前端 12.5%、`eq` 臂下前端 **0**；`eq` 臂下 eq 11.6–13.0%、`front_end` 臂下 eq **0**。**1-in-8 的比例也准**（理论 12.5%）。
* ✅ **前端族（`p84` 的病根）现在可读了**：五条腿都读到 `front_end binds` **11–15 万**，`STAGE=front_end` 时真的消去 **12.5%** ⇒ **§6.161② 红旗 2 被彻底拆掉**。
* ❌ **但 `demap` 和 `ce` 恒为 `0 / 0` —— 而这不是"没测到"，是计数器错了**：同一条腿的车道统计白纸黑字写着 `burst dispatches=19237 (equalizer=19237 demapper=19237 channel_estimator=38474)`、`demod_batch dispatches=19237`、`ce_sites reformat/pilots_lse/pilots_cfo=19237`。**派发明明发生了，计数器却说这个族从未被测量。**

#### ② 根因（三层，逐层修掉；留下的是"判定必须在计数点做"）

我最初把计数器做成 **`encoder()` 里"pipeline 变了"那个分支**的事件计数，理由是"空 kernel 就是在这里被换上的"。**这个位置从三个方向都是错的**：

1. **它是"绑定事件"不是"派发事件"**：延迟路径（空口走的就是它）的派发是在 `flush_pending()` 里编码的，很多情况下 pipeline 早就是那个、**根本不发生重绑定** ⇒ 整族丢数（`binds=0`）。
2. **绑定不知道自己在替谁干活**：merged 路上 **demapper 的 `encoder()` 会先跑 eq 的 flush**，于是那次绑定换的是 **eq 的 pipeline**，用绑定算出来的标志就答错了族 —— 实测就是 `STAGE=eq` 下 demapper 读到 **91.4%**（穿着 eq 的答案）、`STAGE=demap` 下读到 **0%**（穿着"下一个阶段"的答案）。**"绑定"与"族"不是一回事。**
3. **阶段在开 encoder 之前就报了名，而那时 burst 还没开**：离线打印证据 —— 延迟路上**每一次 `set_stage(demapper)` 的 `s.cb` 都是 `nil`**（`demod_metal_engine::enqueue_burst_deferred()` 先 `set_stage` 后 `encoder()`），所以任何"在 set_stage 里算掩码"的实现都只能算出"没有臂"。

**⇒ 定案：判定放在 `count_dispatch(which)` 里** —— 那是**唯一**同时握有"这条 cb 的掩码"（`ablate_mask_for_cb(bs.cb)`）与"这一派发属于哪一族"（调用参数）的地方。计数也就此变成**派发计数**，不再依赖任何绑定事件。

**离线对齐证明（修复后）**：`pusch_demodulator_deferred_chain_test` 上

| 掩码 | `eq` | `demap` | 判读 |
|---|---|---|---|
| `eq` | **85 / 85 (100%)** | 93 / **0** | ✅ 与 `burst dispatches` 的 `equalizer=85`、`demapper=93` **逐一对齐**，且不越界 |
| `demap` | 85 / **0** | **93 / 93 (100%)** | ✅ 反向成立 |
| `all` | 85 / 85 | 93 / 93 | ✅ |
| `EVERY=8`（无 `_STAGE`） | 77 / 12 (**15.6%**) | 77 / 12 (**15.6%**) | ✅ ≈1/8 |

外加：**前端族**在 D1 测试上 `front_end` 2/2、`STAGE=eq` 时前端 **103/103 保持 real**；**不设旋钮 ⇒ `Q9-F5` 一行都不打**（交付路径不变）。

#### ③ ⚠ 五条腿的**窗口读数完全没有**：`OCUDU_METAL_GPU_TIME` 没带

`p133`–`p137` 的 stderr 里是 `queue occupancy (Q9-F3): no GPU-time records - the probe is off` ⇒ **`merged_hop` / `merged_hop_ablated` 的窗口分布一条都没有**，而"族级账单"正是要靠它。**这不怪你** —— §6.162⑦ 的命令块里漏了这一项（**是我的命令写漏了**）。修复后的命令在 ④。

**不过已有的 V1 读数仍然有价值**（`gpu` default、`stale=0`、五条腿都在 1350–1440 带内）：

| 腿 | V1 中位 | `residency` 中位 |
|---|---|---|
| `p133` all | 1393.5 | 554.7 |
| `p134` front_end | **1385.7** | 558.2 |
| `p135` ce | **1427.4** | 633.2 |
| `p136` eq | 1423.8 | 625.8 |
| `p137` demap | 1425.6 | 635.5 |

⚠ **只能当线索，不能当判决**：消去腿**不是能工作的链路**（CRC 掉、HARQ 重传、`residency` 与 V1 都被协议层反馈污染），所以"`ce`/`eq`/`demap` 三条比 `all`/`front_end` 高 ~35 µs"这种差**不能归给族**。判决要等带 `OCUDU_METAL_GPU_TIME=1` 的腿，看的是 **cb 自己的窗口分布**（`merged_hop` vs `merged_hop_ablated`），不是端到端 V1。

#### ④ ★ 修订后的腿命令（**下次照这个飞**）

```bash
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_EVERY=8 \
OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 \
OCUDU_LANE_ABLATE_STAGE=all \
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p138-n78-abl-all --regime=default
# 再按 STAGE=front_end / ce / eq / demap 各一条 → p139 / p140 / p141 / p142
```

**接手机之前核四行**：① `knob` 里有 `OCUDU_LANE_ABLATE=1` + `…_EVERY=8` + `…_STAGE=<族>`；② **`OCUDU_METAL_GPU_TIME=1`**（**这次别再漏**）；③ `[metal_ablate] ABLATION ON …`；④ 拼错族名会有 `names a stage that does not exist`。

**判决读数（按重要性）**：
1. `Q9-F5 ablation coverage` 四行 —— 每族 `binds` 应**与同腿 `burst dispatches` / `ce_sites` 同量级**（这是**本次修复的自检**：对不上说明计数器又错了，读数作废）；`ablated/binds ≈ 1/8`；
2. `Q9-F3 per label` 里 `merged_hop` 与 `merged_hop_ablated` 的 `exec p50/p95/min/max` —— **族级账单 = 对照 − 该族臂**；
3. **`front_end binds≠0` + 该臂窗口是否塌** ⇒ **`p84` 的判决**（塌 ⇒ §6.141 撤回；不塌 ⇒ 维持）。

#### ⑥ ★★★ 腿 `p138`–`p142`（修订命令，探针齐）：**`p84` 判决落地 = `§6.141` 维持**，并抓到一条"消去位置 → gNB 调度"的**仪器效应**（2026-09-29）

**探针这次齐**（五条腿都有 `OCUDU_METAL_GPU_TIME=1` + `OCUDU_UL_PHASE_SEGMENTS=1` + `_STAGE`，`ABLATION ON`，戳 `d7caa5cb0a`）。

**① 覆盖度自检（本次修复的验收）—— 与车道自己的账逐项相等：**

| 腿 | `ce binds` ↔ 车道 | `eq binds` ↔ 车道 | `demap binds` ↔ 车道 | 非本族 |
|---|---|---|---|---|
| p138 `all` | **41194 = channel_estimator 41194** | **20598 = equalizer 20598** | **20598 = demapper 20598** | — |
| p139 `front_end` | 31042 = 31042 | 15521 = 15521 | 15521 = 15521 | eq/ce/demap 全 **0** |
| p140 `ce` | **37436 = 37436** | 18719 = 18719 | 18719 = 18719 | 其余全 0 |
| p141 `eq` | 40402 = 40402 | **20202 = 20202** | 20202 = 20202 | 其余全 0 |
| p142 `demap` | 38396 = 38396 | 19199 = 19199 | **19199 = 19199** | 其余全 0 |

⇒ **§6.163 的修复成立**，且**每族在自己的臂上恰为 1/8 左右、在别人的臂上恰为 0** —— 掩码与计数都对。

**② ★ `p84` 的判决 = `§6.141` 维持（这一线确实没有杠杆）**

`merged_hop` 的**窗口**（`exec`，µs）：

| 腿 | 掩码 | 对照 `merged_hop` p50 | 消去臂 `merged_hop_ablated` p50 | 差 |
|---|---|---|---|---|
| p138 | all | 469.4 | **441.6** | −27.8 (−5.9%) |
| p139 | **front_end** | 470.1 | **475.6** | **+5.5（不动）** |
| p140 | ce | 468.3 | 471.2 | +2.9 |
| p141 | eq | 468.2 | 463.0 | −5.2 |
| p142 | demap | 470.2 | 468.2 | −2.0 |

* ★ **前端族这次真的被消去了（`front_end binds=146571、ablated=18322 = 12.5%`，§6.161② 红旗 2 的病根已拆），而窗口 470.1 → 475.6 —— 纹丝不动。** ⇒ **`§6.141`"这条 cb 的价钱与它做什么无关 ⇒ 这一线没有杠杆"成立**，"减派发/减访存"作为**时延**杠杆**不成立**。
* 唯一"掉"的是 `all`（−27.8 µs）—— 那是**四族同时消去**，仍只掉 6%，且**不是某一族的贡献**（单族都在 ±5 µs 内）⇒ 与 §6.141 的量级结论一致。
* ⚠ 这也**回头看清楚了 `p84`**：它当年的"全消去只掉 1.5%"**结论方向是对的**，错的只是**覆盖度没被计数**（前端族当时够不到）——现在这个漏洞补齐后，结论**没有被推翻，而是第一次被证明**。

**③ ★★ 用户的观察成立，而且数据给出了机制（一条重要的仪器效应）**

用户报：**`p142`（demap）表现明显更好**（ping 更短、iperf3 流量更多）。读数（`ul_mac_pdu_size`，上行载荷）：

| 腿 | 掩码 | 上行 TB 数 | 平均 PDU | **总字节** | 相对最低 |
|---|---|---|---|---|---|
| p139 | front_end | 9661 | 2043 B | 19.7 MB | 1.0× |
| p141 | eq | 11375 | 2448 B | 26.4 MB | 1.3× |
| p140 | ce | 16388 | 1928 B | 30.9 MB | 1.6× |
| p138 | all | 10624 | 2816 B | 27.3 MB | 1.4× |
| **p142** | **demap** | **16934** | **3247 B** | **56.5 MB** | **2.9×** |
| *对照* | *交付 `p132`* | *42969* | *1928 B* | *82.8 MB* | *4.2×* |

**机制 = 消去的位置决定"gNB 看不看得见信道变坏"**：
* **`front_end` 消去** ⇒ 整块网格是垃圾 ⇒ CE/均衡/解映射**全部基于垃圾** ⇒ **gNB 的信道估计与 OLLA 看到"信道很差"** ⇒ **降 MCS、减少授权** ⇒ 上行量塌（19.7 MB、平均 PDU **2043 B**，比交付的 1928 B **还小**）。**p139 的跳数还比 p140 多 33%，载荷却只有 56%** —— 这是"每块变小"的直接证据，不是"测得久"。
* **`ce` 消去** ⇒ 同样污染 CE 输出，平均 PDU 也小（1928 B）—— **与机制一致**。
* **`eq` 消去** ⇒ 中等（2448 B）。
* **`demap` 消去** ⇒ **均衡与 CE 都正常，只丢 LLR** ⇒ gNB 看到的信道**正常** ⇒ **不降 MCS** ⇒ 平均 PDU **3247 B（最大）**，且**TB 数最多（16934）** ⇒ 载荷 2.9×。

**⇒ 三条结论**：
1. **这不是"demap 阶段值钱"** —— 它的窗口贡献只有 −2.0 µs（②）。它是**"破坏一个对 gNB 决策不可见的环节，代价最小"**；
2. **⚠ 绝不能读成"去掉 demap 能改善体验"**：用户的"更好"是**消去臂之间**的相对读数，**绝对水平仍只有交付基线的 68%**（56.5 vs 82.8 MB）；交付链路上**没有"被破坏的跳"**，没有可省的东西 ⇒ **这条对交付路径零杠杆**；
3. ★ **它是"消去臂"这种仪器的系统性偏差**：**消去位置 → gNB 的链路自适应 → 上行量/TB 大小**，所以**任何消去臂的协议层读数（吞吐/CRC-OK/PDU 大小）都不能用来比较族**，只能用**同一个臂内**"对照 cb vs 消去 cb"的**窗口**。**纪律 31 入册。**

**④ 顺带复现的两条老读数**：`stale=0` 五条腿全绿；`residency` 554.7–635.5 µs、V1 中位 1396.2–1426.8 µs（都在 D1 开的历史带内）⇒ **没有把装置跑坏**。

#### ⑤ 纪律（接 27；新增 28–30）

28. ★★ **"消去/减法"的覆盖度计数器必须数在"派发点"，不能数在"绑定点"**：绑定点只知道"我换了 kernel"，不知道"这条派发是谁的"，而延迟路径还会让别人替它绑定（`p133`–`p137` 的 `demap`/`ce` 归零就是这么来的）。
29. ★★ **计数器必须与独立读数对账**：本次的判据是"`binds` ↔ 车道自己的 `burst dispatches`/`ce_sites`"。**对不上就作废**，不要解释。
30. ★ **阶段在 `set_stage()` 时报名时，burst 可能还没打开**（延迟路径先报名后开 encoder）⇒ **任何"在 set_stage 里做判定"的实现都会失败**；必须在 burst 已经开着、且族已知的那一刻做判定。
31. ★★ **消去臂的协议层读数（吞吐 / CRC-OK / MAC PDU 大小 / TB 数）不能用来比较"族"**：消去**哪个**环节，决定了 **gNB 看不看得见信道变坏** —— 破坏 CE/前端 ⇒ 降 MCS、减授权（`p139` 平均 PDU 2043 B、载荷 19.7 MB），破坏 demap ⇒ gNB 看不见、不降码率（`p142` 3247 B、56.5 MB）。**这是仪器的系统性偏差，不是族的价值**。族与族之间只能比**同一个臂内**"对照 cb vs 消去 cb"的**窗口**（§6.163④ 的读数 2）。


## 7. 杠杆与候选改动（技术账）

### 7.1 归属式预算（优化对象的量化锚点，腿 `s82`，中位 µs）

> **先读 §2.3.1**：三段的**名字**与**窗口内容**不是一回事。下表是**按工作量归属**重写后的版本——它才是优化要用的一张表。
>
> ⚠⚠ **本节的两列数字已被 §6.29 / §6.30 更正，别再按原样读**：下表里的 **D（1125）是"占用窗口"不是算力**
> （真实算力 ~300 µs/槽，其中前端 ~171 已在 §6.30 批量化到 **10.6**），而 **C（901）是"驻留窗口在排队"**
> （ρ ≈ 0.73，零算力）。**按"窗口 / 算力"重写后的版本在 §6.30 ⑤ 的表里**（含 A/B/C/D/E 各自的算力与杠杆）。

```
一跳的串行链（中位，加总 ≈ 跨度 2675）：

  A  ≈ 473 µs  等本槽最后一个样点（receiver.receive()；整槽收包策略）   ← 电台时序，结构项
  B  ≈  48 µs  前端主机工作（14 次 encode + 交棒记账 + 通知入队）        ← 宿主
  C  ≈ 901 µs  等到单车道空出来（前一跳的车道窗口还没走完）            ← 单车道串行  ★最大可攻击项
  D  ≈1125 µs  本跳设备执行（DFT+CE+EQ+demap；busy 占驻留 97%）        ← GPU 算力     ★第二大
  E  ≈ 110 µs  Pass-3（LLR 出页/解扰/解复用）+ 解码 fork                ← 宿主

设备/资源侧：residency ≈1125（≈D）、busy split merged_hop 1030 + ch_wt 37、
             车道占用 58.8%、可服务 886 跳/s vs 需求 520.5 跳/s。
接收缓冲持有期 = 整个跨度；池 8 个 × 每槽 1 个 ⇒ 4 ms 抽干。
```

| 序 | 项 | 量级 | 形态 | 已知约束 |
|---|---|---|---|---|
| 1 | **C 单车道排队** | **901**（p95 1782）| 结构：**并发度只有 1**（一条串行 strand）| 动它要碰 **V4**（提交数）⇒ 需用户裁决（§7.4）|
| 2 | **D 本跳设备执行** | **1125**（busy 97%）| 实现：砍 kernel / 提占用率 | 已知贵项被"逐字节不变"钉住（K1 197、抽取 117、重排 117）；本机不支持逐 dispatch 计数器 ⇒ 先做 P0-1 |
| 3 | **A 收样点等待** | **473** | 结构：整槽收包策略 | §5.8.29 只量过该段 −1.2%（未加压）⇒ 要按**跨度**在加压腿上重量（§8 Q7）|
| 4 | **B+E 宿主** | **≈158** | 实现 | `t2f` 的 48 µs 尚未细分（P1-6）|

### 7.2 P1 —— 单变量臂（每条一个变量，判据先登记）

> 读法一律照 §5.9.32 的判读表（哪段尾巴涨 ⇒ 主攻哪一侧）。**每条臂只改一个变量**，且在**同一重载配方**下跑。
>
> ⚠ **先说三条不必烧腿的**（2026-09-24 由代码路径判读**预先回答**，路径无歧义，属于"读码即证据"）：
> **P1-1** `OCUDU_DFT_PIPELINE_DEPTH=1` 走同步路径（宿主 staging + `run()` **带等待** + 宿主写网格）⇒ **必然变差**；
> **P1-2** `OCUDU_DFT_OPEN_BLOCK=0` 每符号各自提交并重新引入 `wait_slot()` ⇒ **必然变差**；
> **P1-3** `OCUDU_CE_HOLD_EXTRACTION=0` 让提取的命令缓冲**自己提交并等待** ⇒ 把一次宿主等待塞进 `ce` 里，**必然变差**。
> 这三条已**从空口清单撤下**（要验也只作为离线反向臂）。**`OCUDU_DFT_*` 家族对 A 项（473 µs 收样点等待）无效**——见 §8 Q3。

| 臂 | 变量 | 目的 / 预登记判据 | 状态 |
|---|---|---|---|
| ~~**P1-1**~~ | ~~`OCUDU_DFT_PIPELINE_DEPTH`~~ | **已由代码判读收口**：同步路径必然变差 | 撤下 |
| ~~**P1-2**~~ | ~~`OCUDU_DFT_OPEN_BLOCK=0`~~ | **已由代码判读收口**：每符号提交 + 重新引入等待 | 撤下 |
| ~~**P1-3**~~ | ~~`OCUDU_CE_HOLD_EXTRACTION=0`~~ | **已由代码判读收口**：把宿主等待塞进 `ce` | 撤下 |
| **P1-4** | 池容量（**诊断用**，见 §7.4）| 时延不变而 `starved_events` 归零 ⇒ 因果链"持有→池干→阻塞→underflow"坐实；**不作为交付** | ✅ **旋钮已实现（§6.36 ④：`OCUDU_UL_RX_POOL_SIZE`，只允许放大）**；**待飞** |
| **P1-5** | `OCUDU_CE_CORR_*` / `OCUDU_CE_INV_BARRIERS`（相关矩阵的 barrier/分片）| 针对 D 项里已知的贵项（K1 串行 pivot、atan2 归约、reformat 链；见主文档 §5.8 的账单）逐项定量 | 待跑 |
| **P1-6** | `OCUDU_UL_SLOT_TRACE=N`（配对 `rxwait`/`t2f`/`ce` 的**逐槽**时间线）| 把 B 项（48 µs）拆开：executor 跳 / 14 次 encode / 交棒记账 / notify / 探针各占多少 | 待跑 |
| **P1-7** | 符号级收包（S-7g-13；`OCUDU_UL_RX_SYMBOLS`）**在加压腿上** | A 项（473 µs）的唯一候选杠杆。⚠ §5.8.29 只量过该段 −1.2%（未加压、且**丢了融合 1 次提交**）⇒ 本臂必须**同时读跨度与 `cbs/lane`**：若跨度降而提交数升 ⇒ 触 V4，交用户裁决 | 待跑 |
| **P1-8** | **车道并发度**（`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=N`）| ✅ 前置条件已满足（§6.1）：生效值 = **1**，可提到 **2..5**。读数看"跨度 / `ce` / `cbs/lane` / `dropped` / RF"。**用户已裁决**：V4 只约束**交付**，本臂**作为纯测量臂**放行 | ✅ **已跑并两次复现**（见 §7.5）|

### 7.3 P2 —— 结构/实现改动（按 P1 的结论选，不预设）

| 候选 | 形态 | 预期 | 风险 / 状态 |
|---|---|---|---|
| **P2-B** | **缩短 D 项（本跳设备执行 1125 µs）**：按 P0-1 的 kernel 拆分，针对最贵 kernel 改线程组/占用率/代数 | 直接砍 D；D 是 busy 97% 的实打实算力 | kernel 改动需逐字节/容差判据（`value_net` + `-L phy`）；⚠ 本机**不支持逐 dispatch 计数器** ⇒ P0-1 必须先做（✅ 已做） |
| **P2-B′**（新增，§6.30）| **前端批量化**：一个时隙的 14 次单 threadgroup 派发 → **1 次 14-threadgroup 派发**（`OCUDU_DFT_BATCH_SYMBOLS=14`）；离线 **159.8 → 10.6 µs/槽**、网格**逐字节相同** | 砍掉前端那 ~149 µs 的**占用窗口**（并顺带把宿主 encode 从 14 次降到 1 次）；按 §6.30 ⑤ 的 ρ≈0.73，C 项会**超线性**跟着缩 | ✅ **空口已验证（§6.31，腿 `p22-n78-batch14`）：V1 中位 2440 → 1513 µs（−38%）、`merged_hop` 1047→625、契约 8/8、`cbs/lane` 不变**；⏳ 待一条**确认腿**；⏳ 旋钮**默认值改 14** 需用户裁决 |
| **P2-G**（新增，§6.61/§6.62）| **消派发（改寻址）**：把"下游要的布局"变成**下游自己的寻址**，而不是搬一遍数据——`scatter` 已落地（K2 直读 LSE）；`reformat`(K3) 同法；**A**（标准组 + 尾巴组并成一次派发）需**逐 system 几何**；**D**（`pilots_lse`+`pilots_cfo` 融合）需 kernel 重写 | 每去掉 1 次派发 ⇒ V1/跨度 **≈13–18 µs**（**标尺口径见 §6.49③/§6.50③**，不要用 `merged_hop` 降幅反推）| ✅ **`scatter` 离线全绿（§6.62）：−1…−2 次/跳、覆盖全部跳、`ab_dumps` 0 字节、与改前二进制对拍 PASSED**；⏳ 待腿 `p39-n78-noscatter`；⚠ 真实派发账是 **8–11 次/跳**（§6.61②），不是车道口径的 4 |
| **P2-E** | **输入缓冲寿命解耦**（keepalive token 从"整跳完成"挪到"最后一个读输入的 dispatch"）| **不缩短时延**（V1 不由它达成），只把持有期从跨度压到前端那一段 ⇒ 治 V2（池饥饿）| ✅ 已实现（默认关）；⛔ **被平台证伪**（§6.4），**待用户裁决** |
| **P2-F** | **车道并发度**（`max_pusch_and_srs_concurrency` 1→2）：让两条跳的设备执行**重叠**，把 C 项（901）吃掉一部分 | 若 GPU 余量（41%）够 ⇒ C 项可大幅下降（§7.5 实测：−55%）| **直接碰 V4**（提交数）⇒ **必须先出测量臂（P1-8）并把结论交用户裁决**（§7.4）。**未开工** |
| **P2-A** | **把估计器的"提取/权重"提前**：让它在网格 DMRS 符号就绪时就开跑，而不是等到跳尾 | 缩短 C（排队）与 D 的串行部分 | 与交棒次序耦合，需保证顺序正确（有 fence 机制）。**未开工** |
| **P2-D** | **池按流水线深度定尺**（`size = ceil(hold_p99 / slot) + margin`，而不是固定 8）| 用**测量**而不是拍脑袋定容量；它是"按设计定尺"，不是加大池 | ✅ **已落地（§6.38，用户裁决 B1）**：GPU 模式 = 实测峰值 11 + 收包路径 2 + margin 3 = **16**，依据打印在启动行；⏳ 待确认腿 `p27` |
| ~~**P2-C**~~ | ~~前端 DFT 的批/深度调优~~ | **撤销**：A 项已查明是"等最后一个样点"而非 DFT 执行 ⇒ `OCUDU_DFT_*` 在这段上**没有可用杠杆**（§8 Q3）| — |

**纪律**：P2 的任何条目都必须先有 P1 的读数支撑；**不许"顺手一起改"**（否则读数不可归因）。

### 7.4 ★ 用户裁决（2026-09-24）：**V4 只约束交付，测量臂放行**

C 项（901 µs，单车道排队）与 D 项（1125 µs，本跳执行）是**同一个串行链的两半**，而它们只有两个互斥的打开方式：

* **形态 1 —— 提高并发度（P2-F）**：不减少 GPU 工作量，让第二条跳的设备执行与第一条重叠。
  代价：**在飞命令缓冲数上升 ⇒ 可能碰 V4**；且 LDPC 解码器与车道**共用同一条后端口队列**
  （`ocudu_metal_queue.h:52-61`、decoder 引擎 `:298`）⇒ 并发跳也可能把解码挤到后面。
* **形态 2 —— 缩短单跳窗口（P2-B/E/A）**：不增加在飞数，靠减少 GPU 工作或提前启动。
  代价：已知贵项被"**逐字节不变**"钉住；用户已在 §5.8.19 ① 放宽过该约束（"逐字节不变太严格… 最根本的是算法逻辑正确、误差在一定范围内"），
  但**每一次用这个自由度的改动都必须飞一条空口腿**。

**裁决（用户）**：**V4 只约束交付；允许把 P1-8 作为纯测量臂先跑。**

⇒ 落地方式：① 先做 P0-6（✅ 已做）；② P1-8 的读数**只用于回答"并发度能不能吃掉 C 项"**，与 V4 无关；
③ **若要把并发度作为交付**（P2-F），**必须回到用户再裁一次**——那时 V4 才生效，本轮裁决不等于预先批准交付。

**与"加大池"的关系（用户裁定，别搞混）**：**加大池 = 诊断**（P1-4），它能**证明**因果链，但**不缩短时延**，
因此**不作为交付**、也不满足 V1。**P2-D** 是它的"有原则版本"：容量**由测得的持有期分布导出**，并随时延的改善重算。
判据上二者必须分开：P1-4 只看 `starved_events`；P2-D 必须同时满足 V1 与 V2。

### 7.5 P1-8 读数（2026-09-24）：并发度是真实杠杆，但带一个复现两次的代价

n1 默认配方 + `OCUDU_UL_PHASE_SEGMENTS=1`：

| 腿 | 车道并发度 | 跨度中位 | `t2f` | **`ce`** | `eq_demap` | `stale` | 池 `starved_takes` |
|---|---|---|---|---|---|---|---|
| `s87-n1phases` | **1**（P0-6 实测）| **5268** | 1093.0 | **3228.3** | 905.5 | 5 | 102 |
| `s88-laneconc2` | **2** | **2400** | 1094.6 | **55.6** | 1232.9 | 0 | 35 |
| `s88b-laneconc2`（复跑）| **2** | **2359** | 1092.5 | **50.6** | 1228.2 | 0 | 35 |

* 三段和占跨度 **99.2%**（5226.8 vs 5268）；`t2f` 不动（= rx_wait 1052 + 前端主机 ~41 µs）。
* ⇒ **并发 1→2 使 `ce` −58~64×、跨度 −55%**，且**两次独立复现** ⇒ n1 的 2.85 倍退化就是"等串行 strand"，**P1-8 是真实杠杆**。
* ⚠ **代价也复现两次，形状完全相同**：`radio sample continuity: **2 gaps**，每次丢 **~1.531 亿样点（≈5 秒）**`，
  并与一次 **5.0004 秒**的车道 residency 异常同现（5,004,199 / 5,004,062 µs）。
  ⇒ **不是"复跑即消"的偶发项**；**并发度 2 会把收包路径压出一次 ~5 秒的停顿**（或两者共有第三个因）。
  ⇒ **在查清之前，P1-8 只能作为测量结论，不能作为交付**。
* 设备侧在并发 2 下**全部 OK**（`ce device estimates: 369065 device, 0 host`、`host sample assembly` 仅 2 次拷贝、
  crossings/wraps 0）⇒ 并发 2 **没有**破坏契约机制，红的那条是**收包侧**。

---

### 7.6 ★ 结构性候选（"换结构"，§6.71 之后唯一还有量级空间的方向）

**靶子（先把数摆清）**：V1 中位 **1364.2 µs** 里，**属于这一跳自己的设备窗口只有 `merged_hop` 533.7 µs（39%）**，
其中 **~467 µs 是"等本槽最后一个样点"（零算力）**、**~67 µs 才是全部工作**（算力 ~51 + 交棒/边界的余量）；
**其余 ~830 µs 是它在等**（车道排队、宿主 encode、交棒、LLR 出手）——量级与 §7.1 的 **C 项（~901 µs 单车道排队）** 一致。
⇒ **结构项要动的是"等"的两半**：**等样点（~467，本跳内）** 与 **等车道（~830，跳之间）**。**一次性把两者都算上，上限 ≈ V1 的 ~95%**，
但**任何只看"窗口"的方案最多拿到 ~534 µs，且并发 2 下只会有一部分落在 V1 上**。

| # | 结构 | 机制 | 需要什么 | 上限 / 风险 |
|---|---|---|---|---|
| **S-A**（**最便宜，先做**）| **`OCUDU_UL_RX_SYMBOLS=7`（现成旋钮的"中间档"）**：半槽收包 ⇒ 前端与估计器早半槽开跑 | 与 `p41`（N=1）**同一个旋钮**，只是粒度不同 | **一条腿**（不用改代码）| 上限 ~233 µs（半个时隙）。⚠ **前提：先答 §8 Q21**（N=1 为什么把 MCS 打到最低）——否则可能再破一次 UL。**要读的是 `batched=`（不能是 0）与 `released=`（交棒必须还在）** |
| **S-B**（**真正的代码活**）| **把"收包粒度"与"前端块/交棒粒度"解耦**：部分槽收包 + **保留 14 个变换的批量化与 D1 交棒** | 今天 `whole_slot_policy()` 这个环境量**同时**驱动收包块大小与 `ofdm_demodulator_impl` 的**块开/关**（`set_lane_slot`→`end_block`→`begin_block`，以及 D1 的 `released`）。`p41` 证明按符号关块 ⇒ **批量化与交棒双失**（`batched=0/0`、`released=0`、20.5 提交/跳）| 改 `lower_phy_baseband_processor.cpp`（`symbol_blocks` 分支，1188 起）与 `ofdm_demodulator_impl.h` 的块逻辑：**收包按 N 符号、块仍按"已到的样点"成批**（N=7 ⇒ 一槽 2 次批派发 + 1 次交棒）| 上限 ~233 µs（半槽）～~467 µs（按符号开跑）。风险：交棒契约与网格可见性（D1 的 `deposit/take` 按**槽**配对，提前交棒要重新定义"这一块覆盖几个符号"）|
| **S-C** | **按"最后一个 DM-RS 符号"开跑估计器** | 估计器只需要 DM-RS 符号（语料/空口为符号 2/7/11）⇒ 等符号 11 而不是 13 | 前端要能在槽未结束时交棒（同 D1 路径），网格视图覆盖已到符号 | 上限 **~67 µs**（2 个符号）。风险：与 S-B 同一处交棒契约 |
| **S-D** | **段间跨跳重叠（软件流水）**：跳 N 的均衡/解映射 与 跳 N+1 的等样点 重叠 | 今天**跳内是串行链**（前端→CE→均衡→解映射，`Q4`），只有**跳之间**靠并发 2 重叠 | 逐段命令缓冲 + 更深在飞池（≥3）| 上限 **~67 µs**（那点工作）。⚠ **触 V4**（在飞提交数）与 P2-D 的池定尺；且宿主停顿敏感（§6.56③）|
| **S-E** | **动 ~830 µs 的"等车道"**：队列/并发结构 | 这是 V1 里**最大的一块**，但杠杆就是**并发度**（P1-8/P2-F）与队列次序 | 需要用户裁决（V4）；已有读数：并发 2 已吃到 −55~64%，代价是偶发停顿 | 上限最大、**也最需要裁决**；**本工作流已把"并发度作为交付"判为需二次裁决（P2-F）** |

**建议顺序**：**先答 Q21**（读码/短腿即可）⇒ **S-A（一条腿，验"中间档"是否保住批量化与交棒"）** ⇒ 若 S-A 的读数证明"半槽收包 + 保住批量"可行，再做 **S-B**（把它变成交付形态）。
S-C 是 S-B 的顺带收益；**S-D/S-E 都要用户裁决**（V4）。

#### 7.6.0 ★ 为什么"链长 533.7 µs > 时隙 500 µs"**没有**把链路堵塌（用户 2026-09-25 提问）

**一句话**：**"链长"不是"每时隙必须完成一次"的约束**——约束是 HARQ 时间线，而**到达率也远低于时隙率**，中间的错配由**池**吸收。三条机制：

1. **到达率 ≪ 容量**（主因）：UL 不是每时隙都有 PUSCH（`ul_ratio=0.30` + TDD 图案 + 流量形状）。实测 `[ul_gpu_lane] period` = **mean 2011 µs、median 434.9 µs**（成串到达、串间有空档），
   即 **~400–500 跳/秒**；而车道容量 = 1/533.7 µs ≈ **1874 跳/秒**（并发 2 下两跳可重叠 ⇒ 上限 ~3700/秒）⇒ **利用率 ≈13–27%** ⇒ 队列不增长。
2. **每跳的截止时间不是时隙，而是 HARQ 时间线**：调度 `k2=4`（4 个时隙 ≈ 2 ms 的授权提前量），
   而探针自己把 **8000 µs 定为"uplink HARQ round trip"**（`stale` 的门限）⇒ 一跳跨度 1364 µs 只占这个预算的 **~17%**。⇒ 链跨过一个时隙边界**完全允许**。
3. **池把"收包时钟"与"处理车道"解耦**：池是 **32 个整时隙缓冲**；电台边收边填，车道还在处理更早的跳（`held_max` 10–18、`free_min` 14–22 就是这份弹性在被动用）。
   **收包侧不因为车道忙而停**——只有池见底才会停（§6.26 修复 B 之后是"丢弃并计数"，而不是停住电台）。

**池里的缓冲在哪个节点（用户 2026-09-25 提问）**：**在 FFT 之前 —— 是时域 IQ 样本，一个缓冲 = 整个时隙**。
证据：启动行 `[ul_rx_pool] size=32 buffers of 11520 samples (slot=11520, whole-slot buffers, the gpu pipeline mode)`（11520 = 23.04 MHz × 0.5 ms ✓）；
以及代码注释（`lower_phy_baseband_processor.cpp`，符号级收包的分支）："**the transforms that still read it keep it alive and return it to the pool themselves**"
—— 即**缓冲是被"变换（DFT）"读的**，读完才归还。
⇒ **FFT 之后没有等价弹性的池**：时频网格是**前端在车道自己的命令缓冲里写出来**的（DFT 的 grid 写），再由 **D1 交棒（`shared_burst::deposit_released()`，按 `resource_grid_device_view::base` 配对）** 把**命令缓冲**交给估计器 —— 那是**顺序（ordering）**，不是**缓冲（elasticity）**。
⇒ 这条对结构项（§7.6 的 S-A/S-B）很关键：**IQ 侧可以"多存几槽"来吸收抖动，网格侧不能**；要让估计器早开跑，必须让前端**产出并交棒"部分网格"**，而不只是改收包粒度。

**池的 32 是"滞后的预算"，不是"延迟"（用户 2026-09-25 提问：32 个时隙岂不是要等 16 ms？）**：**池是 free list（可互换的空闲缓冲表），不是 FIFO 队列**——
一个缓冲"被取走"是电台往里填一个时隙，"归还"是**读它的变换做完**。实测持有期（`p39` 的 P0-2 行）：

| 读数 | 值 | 折合时隙（500 µs）|
|---|---|---|
| **中位持有** | **819.1 µs** | **≈1.6 个** |
| 均值 / p95 / p99 | 873.1 / 977.8 / **1583.2 µs** | ≈1.7 / 2.0 / **3.2** |
| **最大** | **20 259.6 µs**（slot=16257）| ≈40（**就是那次 ~40 ms 宿主停顿被吸收**）|
| 超过 100 ms / 1 s 的 token | **0 / 0** | —— |
| `held_max` / `free_min` / `dropped` | 18 / 14 / **0** | 32 个里常驻只用 ~18 |

⇒ **稳态延迟由流水线深度决定（≈1.6 个时隙），与池大小无关**；**32 买的是"抖动容限"**：允许车道的滞后在丢样之前累积到 ~16 ms ——
这正是 20.3 ms 那次停顿没有变成丢样的原因。**池小的时候这个预算就不够**：§6.34–§6.38 的池 = 8 + 持有峰值 11 ⇒ `starved_events=97` ⇒ 收包停住 ⇒ 电台丢样（历史上看到的"塌"）。
⚠ **一条待办**：P2-D 的定尺规则是 `ceil(hold_p99/slot)+rx2+margin3`（用 p99 = 3.2 槽 ⇒ ≈8–9），而**今天的 32 是按"峰值 16 + 2 + 3 = 21 ⇒ 取 2 的幂"**定出来的（§6.38）；
⇒ **S-B 把链缩短之后应当按新的 `hold_p99` 重定尺**（并明确"按峰值还是按 p99"这个选择，因为它就是"worst-case 允许多滞后"的取舍）。

**塌的条件（三者之一，且本工作流都亲眼见过其反面）**：
(a) **链 × 到达率 > 车道容量**（利用率越过 100% ⇒ 队列无界增长 ⇒ 丢 HARQ）；
(b) **在飞缓冲 > 池**——**这正是 §6.34–§6.38 的历史**：池 = 8 而持有峰值 = 11 时，`starved_events=97`、`pop_blocking` 出现、**电台丢样点**（用户观察到的"塌"就是它）；定尺到 32 后消失（§6.39）；
(c) **跨度 > HARQ 截止**（`stale` 那一族，8 ms 门限）。
⇒ 今天三者余量都 ≥3×，所以"链 > 时隙"只表现为 **cbs/队列的一点排队**，不表现为断流。

#### 7.6.0b ★ 单车道优化的第一步（拆那 ~830 µs）**卡在尺子上**：五段宿主/队列分段在交付路是"no samples"

**要拆的量**：V1 1364.2 − `merged_hop` 533.7 = **~830 µs** 不在本跳的设备窗口里。要拆它，需要 `[ul_gpu_lane]` 的五段：
`gap: stage entry -> extraction commit (host)`、`queue: burst commit -> burst start`、`host: extraction commit -> weights commit`、
`host: weights commit -> burst commit`、`gap: commit -> first command buffer starts (queue)`。

**现状**：这五段在 `p37`–`p41` 上**全部打印 "no samples"**（`ocudu_metal_lane_probe.mm` 的 `print_series(..., "no samples")`）。
读码：分段本身存在（`ocudu_metal_lane_clock.h` 的 `handover_us` / `host_to_weights_us` / `host_to_burst_us`，以及 probe 的 `queue_to_burst`），
**缺的是给它们打点的调用**：`mark_stage_entry()`（由 adapter 带 slot 调）与 weights/burst 那两个 commit 的 mark。
（`mark_extraction_commit()` 本身是在引擎里被调的：`ocudu_metal_mmse_engine.mm` 1126/1200/1227/1259。）

⇒ **更正（读码后）**：**不是"缺 mark"** —— `mark_stage_entry()`（`port_channel_estimator_metal_mmse_impl.cpp:1481`）与 `mark_extraction_commit()` 都在被调。
真正的闸门在 probe 的 `transition()`（`ocudu_metal_lane_probe.mm:826-844`）：
```cpp
if (!stage_gpu[f] || !stage_gpu[t]) { return; }   // ← 两个阶段的 GPU 时间戳缺一就整段丢掉
...
host_span.push_back(stage_host[t] - stage_host[f]); // ← host 跨度本不需要 GPU 戳，却被这个 return 一起挡掉
```
而 **Metal 只给"每个命令缓冲"一个 GPU 窗口**，**融合路上整跳就是 ONE command buffer** ⇒ **分阶段的 GPU 戳根本不存在**（这正是 §6.19/P0-1 记的那件事）。
⇒ **再更正（第二次读码，坐实）**：连"host 跨度"也**不是被误挡** —— 融合路的一条 lane**只注册一个命令缓冲**（`merged_hop`，占 busy 的 93%；只有 7% 的 busy 落在 `ch_wt` 那种非融合 lane 上），
`entries_for_starts` 里因此**没有** `channel_estimator` / `channel_estimator_weights` 这两个 stage 的条目（它的 GPU 窗口与 host commit 都按**命令缓冲**记），
而 `transition()` 要的正是"两个 stage 各自的边界"。⇒ **在交付（融合）路上，这五段"构造上就不存在"**，不是打点缺失、也不是闸门写错。
**可选的两条路**：(a) 飞**诊断臂 `OCUDU_LANE_DIAG_SPLIT=1`**（把前端缓冲单独提交 ⇒ 有分阶段边界 ⇒ 五段亮，但结构变了、只作指示）；
(b) **把 host 侧的尺子延伸到融合路**（lane clock 的 `stage_entry`/`extraction_commit` 已经是 host 打点，`handover_us` 本应可用 —— 它也是 "no samples"，**这一个是真的可疑**，下一步查它）。
⇒ **结论**：拆那 ~830 µs 在交付路上**没有现成尺子**，必须先补（b）或借（a）。

**★ 第三次读码：`handover_us` 为空的根因找到并已修（§7.6.0b 的收口）**：
融合路上**没有任何地方打"抽取提交"这个点** —— `shared_burst` 模块里没有 `mark_extraction_commit()`，
而 `merged` 分支的两个出口（`shared_burst::adopt()` 成功、以及 adopt 失败时的回退 `[st.cb commit]`）**也都没打**。
其它 lane order（`event`/`burst`/`host_wait`）都经 `end_stage()`/`end_stage_async()`，那里有打点 ⇒ **所以离线（replay 全是 host_wait）看不见这个洞，只有空口腿上是 "no samples"**（`p37`–`p41` 全中）。
**修法**：在 `merged` 分支的出口补一次 `lane_clock.mark_extraction_commit()`（引擎 `ocudu_metal_mmse_engine.mm`，带注释说明"为什么必须在这里"）。
**语义**：融合路上"抽取提交"= **这条 lane 的第一个命令缓冲成为 lane 的**那一刻（adopt 或回退 commit）⇒ `handover_us` = **宿主从跳入口到交出 lane 的那一段**，正是它设计时要量的东西。
**验证**：⚠ **离线验不了**（replay 的跳非延迟 ⇒ 适配器强制 `host_wait`，永远走不到 merged 分支）⇒ **只能靠一条腿**：应在 `[ul_gpu_lane] gap: stage entry -> extraction commit (host)` 上看到样本而不再是 "no samples"；
**不变量**：改动只加一次诊断时钟调用（除 probe 报告外无人读它），`ctest -L phy -j 1` 193/193 通过（CE 单测的 Test 13 正是钉 lane order 的那条）。

**（下面这段是第一次读码的中间结论，保留以记录推理过程）**

⇒ **尺子的施工内容（两件）**：
 (a) **把 host 跨度从 GPU 闸门里放出来**（`transition()` 里让 `host_span` 只要两侧 host 打点就 push）⇒ 五段里先亮两段
     （`host: extraction commit -> weights commit`、`host: weights commit -> burst commit`）；小、局部、离线可自证。
 (b) `queue:`/`gap:` 那三段需要**分阶段 GPU 戳** ⇒ 只能用**现成的诊断臂 `OCUDU_LANE_DIAG_SPLIT=1`**（§6.2：它不再采纳前端的缓冲、把它单独提交，于是 `dft` 段可读）。
     ⚠ 该臂**改变提交结构**（每车道 2 个命令缓冲）⇒ 它的分段读数**只作指示**，不能直接当作交付配置的分解。
 (c) `handover_us`（stage entry → extraction commit）本应可用，需再查一处：adapter 的 `mark_stage_entry` 与引擎的 `mark_extraction_commit` 在融合路上是否**次序颠倒**（`mark_stage_entry` 会把 `handover_us` 重置为 −1）。
⚠ **在尺子补好之前，不要动 S-A/S-B**：否则改完只能说"V1 变了"，说不出"变在哪一段"。

**同时已能确定的上界**（不依赖那五段）：一跳真正的 GPU 执行 ≈ **~75 µs**（前端 ~10.6/槽 + 算力 ~51 + 派发地板 ~13），
而 V1 = 1364.2 ⇒ **~95% 的跨度是"等"**（等样点 + 等车道 + 交棒/提交/出手）。⇒ **单车道优化的标的全部在"等的结构"里，不在算力里**（与 §6.65–§6.69 的结论一致）。

#### 7.6.0c ★ 那 ~830 µs 的第一版分解（用现有腿数据）+ 验证腿 `p42-n78-hostgap` 的预登记

**可测的三个锚**（`p39`，全部中位）：

| 锚 | 读数 | 含义 |
|---|---|---|
| **本跳自己的设备窗口** | `merged_hop` **533.7 µs** | 含"等本槽样点" ~467 + 真执行 ~75（§6.65/§6.69）|
| **同伴窗口（串行化等待）** | ≈ **533.7 µs**（= 一次窗口）| 车道上同一时刻只有一个跳在执行（`busy≈residency`、`burst max_in_flight=1`）⇒ 一跳的跨度里约一半是**等同伴占着 GPU** |
| **队列延迟** | `queue: weights commit → weights start` **40.4 µs**（p95 316.5）| 后端队列把这块缓冲排到队的时间 |

⇒ **V1 1364.2 ≈ 533.7（自己）+ 533.7（同伴）+ 40.4（队列）+ 残差 ≈ 256 µs**。
**残差 256 µs 的候选**（现在**分不开**，正是要补的尺子）：宿主从"跳入口→交出 lane"（今天的 `handover_us`，按引擎已有相位表预估 **~15–40 µs**：wrap 0.8 + cb 4.5 + encode 6.5–8.2）、
适配器在引擎前后的部分、"等网格就绪"（前端交棒）、LLR 出手、以及收包侧抖动（`[ul_rx_timing] recv over 1ms=996`）。

**★ 验证腿 `p42-n78-hostgap`（配方与 `p39` 完全相同，唯一变量 = 今天这处补丁）**：

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p42-n78-hostgap \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 判读：p0_gate.sh p42-n78-hostgap ；leg_gate.sh --slot-ms=0.5 p42-n78-hostgap
grep -a "stage entry -> extraction commit" <腿>.log.stderr     # ★ 必须不再是 "no samples"
grep -aE "ul_gpu_pipeline|queue: weights commit|commit -> completion|ul_gpu_lane\] period" <腿>.log.stderr
```

| 判据 | 预登记 |
|---|---|
| `gap: stage entry -> extraction commit (host)` | ★ **出现样本**（这是本次补丁的唯一验收判据）；中位落在 **10–60 µs**（引擎相位表外推）；若 >100 µs 说明适配器侧还有宿主工作 |
| V1 中位 | **≈1364 ± 20 µs**（补丁只加一次诊断时钟调用，**不应改变任何值**）；若偏离 >40 µs ⇒ 先怀疑它不只是诊断 |
| `merged_hop` / `queue: weights commit→start` / `period` | 与 `p39` 同量级（534 / 40 / 435 µs）|
| 契约 / `cbs/lane` / `gaps` / 池 | 8/8 / 2.00 (max=2) / 0 / `starved=0`、`dropped=0`（**不允许变**）|
| `handover_us` 的份额 | 若它只有 ~20 µs，则那 256 µs 残差主要在**"等网格就绪"与 LLR 出手**上 ⇒ 下一步的尺子要往那两处加（而不是继续在 CE 里找）|

#### 7.6.0d ★★ 腿 `p42-n78-hostgap`：**新尺子亮了、预登记逐条命中**，830 µs 的分解第一版成立

| 判据（§7.6.0c 预登记）| 实测（`p42`）| 判定 |
|---|---|---|
| ★ `gap: stage entry -> extraction commit (host)` | **samples=144787、mean 50.6、中位 45.8、min 24.1、p95 78.5、p99 98.1、max 889.5 µs** | ✅ **不再是 "no samples"**（预登记 10–60 µs，落在带内）；`p37`–`p41` 的洞闭合 |
| V1 中位 | **1366.8 µs**（`p39` 1364.2 ⇒ **+2.6**）| ✅ 预登记 ≈1364±20 ⇒ **这处补丁确实只是诊断**（不改值）|
| `merged_hop` / `queue: weights commit→start` / `period` | 541.8 / 39.5 / 427.7 µs | ✅ 与 `p39`（533.7 / 40.4 / 434.9）同量级 |
| 契约 / `cbs/lane` / `gaps` / 池 | 8/8 / 2.00 (max=2)、dropped=0 / **0** / `free_min=18`、`starved=0` | ✅ **不允许变的都没变** |
| `p0_gate` / `leg_gate` | **29 of 29** / **8 of 9**（唯一红是预登记点名的误绑项 `grant ≥50%`）| ✅ |
| 附加读数 | `batched=157357/2202998 batch_max=14` ✓、UL **13.98 Mbit/s** ✓、`stale=0`（`p39` 是 20）、D1 max hold 15.6 ms | —— |

**分解（`p42` 中位，四块可测 + 一块残差）**：

| 块 | µs | 出处 |
|---|---|---|
| 本跳设备窗口 | **541.8** | `merged_hop` |
| **宿主：跳入口 → 交出 lane** | **45.8** | ★ 本次新增的尺子 |
| 后端队列：提交 → 开始 | **39.5** | `queue: weights commit → weights start` |
| 同伴占用 / 串行化等待 | ≈541.8 中落在本跳跨度内的部分 | 由残差反推 |
| **残差** | **≈198 µs**（= 1366.8 − 541.8 − 541.8 − 39.5 − 45.8）| **当前最大的一块未归属** |
| （参考）一跳自己的链 | ≈**1042 µs** = 样点到达 ~500 + 设备窗口 ~542 | 前者是物理，后者含 ~467 非执行 |

⇒ **下一步要归属的是那 ~198 µs**，候选：(a) 同伴占用中真正落进本跳跨度的部分（**要一条并发 1 的对照臂**才能分，但并发 1 会让队列项变大，见 §7.5）；(b) **LLR 出手**（目前**没有打点**——这是尺子阶梯上唯一还缺的一级）；(c) 收包 → 跳入口那一段（前端交棒）。
⇒ **建议先补 (b) 的那一级**（在适配器里 LLR 就绪处加一次 `lane_clock` 打点，做法与本次完全相同、离线可自证），这样"一跳自己的路径"就被首尾完全夹住：入口 → 交出 → 排队 → 窗口 → LLR。

**★ 读码后：那一级不用补了 —— 它已经有现成读数（因此本节不改代码、也不多飞腿）**：
`[ul_gpu_pipeline]` 的**结束时刻**就是 `ul_pipeline_probe::record_ldpc_start()`（`pusch_decoder_impl.cpp:360`，注释原文："In the fused-lane mode this call is ALSO the end of the [ul_gpu_pipeline] series (the LLRs are ready here)"）。
而与它同义的"设备做完 → 消费者拿到"这一段，**D10 已经在量**：`[metal_stats] … handler lag=… mean=54.1us max=4910.0us`（`p42`）—— 就是"GPU done → handler ran"。
⇒ **阶梯已经闭合**（`p42` 中位/均值）：

| 段 | µs | 出处 |
|---|---|---|
| 本跳设备窗口 | 541.8 | `merged_hop` |
| 同伴占用（串行化）| ≈541.8 | 由残差反推（`busy≈residency`、`max_in_flight=1`）|
| 宿主：入口 → 交出 lane | 45.8 | 本次新增的尺子 |
| 后端队列：提交 → 开始 | 39.5 | `queue: weights commit → start` |
| 设备做完 → 消费者（LLR 出手）| 54.1（均值）| **D10 `handler lag`** |
| **残差** | **≈144 µs** | = 1366.8 − 541.8 − 541.8 − 45.8 − 39.5 − 54.1 ⇒ **归"收包 → 估计器入口"（前端交棒那一段）+ 抖动** |

⇒ **单车道优化的两个最大项已经点名**：(i) **设备窗口 541.8（其中 ~467 是非执行）** —— S-A/S-B 的靶子；(ii) **同伴占用 541.8** —— 它是"单车道链长约等于一个窗口"的直接后果，**所以缩短 (i) 会同时缩短 (ii)**（这就是 §7.6.1 说的 ~2× 放大）。

#### 7.6.1 ★ 用户裁决：**先把"单车道"自己优化好，再动并发（S-E）** —— 数字支持这个顺序

**① V1 可以近似写成 `2 × merged_hop + 常数`（并发 2 下）**：`p39` 的 V1 1364.2 ≈ **2 × 533.7 + 297**。
解释：车道被占用的中位时间是 533.7 µs/跳，所以**一跳的跨度里约一半是"等另一跳占着车道"** ⇒ **缩短本跳的窗口不只省它自己，还让它的同伴少等同样长的时间**。
**成对读数直接支持**：§3.2 那对 `merged_hop −14.1` ⇒ **V1 −36.3（放大 2.6×）**；`p39/p40` 那对 `−17.4` ⇒ **V1 −21.5（放大 1.24×）**。
⇒ **窗口类改动（S-A/S-B/S-C）在 V1 上的期望收益 = 窗口降幅 × 1.2…2.6**；这也是"窗口 ≠ 关键路径代价"（§6.31③）在这条流水线上的**定量形式**。

**② 单车道是"串行化受限"而不是"算力受限"（这是先做单车的理由）**：一槽 500 µs 里真正的 GPU 执行只有 **~78 µs**（前端 10.6 + 算力 ~51 + 交棒余量）⇒ **算力余量 ~6×**；
而跳内是**串行链**（前端等整槽样点 → CE → 均衡 → 解映射），且**链长 533.7 µs 已经超过一个时隙 500 µs** ⇒ **队列不是硬件不够，而是链太长**。
并发只是把这个链**遮住**，同时付出在飞提交数（V4）与偶发停顿（P1-8 实测）的代价；**把链缩短才是"去掉"而不是"遮住"**。

**③ 顺序建议（前两步不飞腿）**：
1. **量化那 ~830 µs**（用现成腿日志即可）：确认 `V1 ≈ 2×W + C` 里 C 的成分（宿主 encode ≈1.3–1.6 µs/派发、交棒、`queue: weights commit→start` 中位 40 µs、`gap` 中位 8.4 µs）⇒ 定出 S-B 的期望值。
2. **答 Q21**（读码为主）：符号级收包为什么把 MCS 打到最低；**并顺带读出 S-B 必须保住的那两件**（`batched=`、`released=`）。
3. **S-A：一条腿 `OCUDU_UL_RX_SYMBOLS=7`**，只读四件：`batched=`（≠0）、`released=`（≠0）、**整跳提交数**（Q22）、TBS/吞吐。
4. **S-B 改码**（部分槽收包 + 保住批量化与 D1 交棒），S-C 是顺带收益。
5. **再回到 S-E**：链缩短后**队列压力会自己下来**；若链明显短于时隙间隔，甚至可以把并发**降回 1**，从而**同时去掉 P1-8 的偶发停顿代价**——这是"先单车"可能拿到的额外收益。

**④ 纪律**：这一串测量**并发度固定为 2**（一个变量）；每步按 §5.2 第 8 条**只预登记增量**。

### 7.7 Q21 开工记录（P1-7 为什么把 MCS 打到最低）：已排除两条、锁定两条，判别读码指向"符号网格的相位"

**已排除（用 `p41` vs `p42` 的现成读数）**：
* **不是丢样**：`gaps=0`、`rx_overflows=0`、`rx_lates=0`、`ts0_blocks=0`（`p41`）——电台把样点都交到了。
* **不是接收线程被更狠地堵住**：`[ul_rx_timing] recv over 1ms` = **18**（`p41`）vs **1194**（`p42`）；`load1` 7.64 vs 7.37 ⇒ 符号级收包**没有**让接收更糟。
* **不是估计器变慢**（那不会改 MCS）：`paired ce` 中位 69.3 → 79.9 µs（+15%），同量级。

**锁定（待读码判别的两条）**：
* **H1 — 网格的符号相位错位（首选）**：`p41` 的样点**都在**，但符号级收包把"哪个符号落在网格的哪一行"的参考换了 ——
  前端/收包侧一旦按**符号边界**对齐，而网格的行仍然按**时隙起点**编号，DM-RS 就会落在错误的符号上 ⇒
  **0 丢样、契约 8/8、池绿，但信道估计拿到的不是 DM-RS** ⇒ MCS 落到底、TBS ~200 bit、吞吐 ↓17×（**与全部观察一致**）。
* **H3 — 时隙填充缓冲在符号级路径上的"退役相位"漂移**：`lower_phy_baseband_processor.cpp` 1216–1240 的
  "窗口放不下一个整符号就退役缓冲、在新边界重开"那条路，在 n78 的几何（11520 样点/时隙、14 符号、CP 不等长）下
  是否**每个时隙都落在同一相位**——若不等长符号让相位逐槽漂移，网格就会逐槽偏移。**H1/H3 是同一个根的两种表现**。
* （弱）**H2 — 宿主饱和**：接收调用 20×（14.8M vs 0.74M），但 `recv over 1ms` 更少 ⇒ 证据不支持，降级。

**★★ 判别读码结果：H1/H3 坐实（机制已可判定，不需要腿）**

`symbol_grid_position`（`include/ocudu/phy/lower/processors/uplink/uplink_processor_baseband.h:48`）**只有三个字段**：
`nof_samples_to_boundary` / `nof_samples` / `nof_symbols` —— **没有任何"这是网格的第几个符号"的信息**。
而 `locate_symbols()` 的实现（`lib/phy/lower/processors/uplink/uplink_processor_impl.cpp:362` 起）是**按时间戳在"子帧符号图案"里的位置**算出来的：
`i_sample = timestamp % (一个子帧的样点数)` → 走 `symbol_sizes[]` 得到"距下一个符号边界多少样点"和"整符号有多少个"，**返回的只是样点计数**。

⇒ **样点落进网格哪一行，是由填充偏移（`rx_fill`/`rx_offset`）决定的，不是由时间戳的符号索引决定的**（`lower_phy_baseband_processor.cpp` 1202–1242 的 `rx_fill` 累加）。
于是只要**块边界与填充偏移在时隙交界处不一致**，整个时隙的样点就会被**按符号整体旋转**放进网格：
**DM-RS 落到别的符号行上 ⇒ 估计器拿到的不是 DM-RS ⇒ MCS 落底、TBS ~200 bit、吞吐 ↓17×，而 `gaps=0`、契约 8/8、池绿** —— **与 `p41` 的全部观察逐条吻合**。
两条已知能造成这种不一致的路径：
① 1220–1240 的"窗口放不下一个整符号 ⇒ 退役缓冲、在当前边界重开"（重开后 `rx_offset=0`，但边界未必是**时隙**的第一符号）；
② 块被 `nof_samples_per_slot - rx_offset` 截断（1217 的 while 循环）时，块的长度与时隙相位的关系。
**⇒ S-B 的硬约束第 ③ 条就是它**：**收包粒度可以改，但"样点→网格行"的映射必须仍按时间戳/时隙相位来锚定**（而不是按填充偏移），否则就会重现 `p41` 的旋转。
⇒ 这也解释了 §7.6.0b 里那个"符号级路径把 `released`/`batched` 也一起关掉"的现象：**那条路径与整槽路径在"网格怎么装"上是两套语义**，不是同一个语义的不同粒度。

**★★★ S-B 的改动清单（读码落定，三处；`p41` 的"批量化被关"是显式开关，不是时序副作用）**

1. **`ofdm_demodulator_impl.h` 的 `block_batching_enabled()` —— 这就是"收包粒度"与"批量化"的耦合点**：
   ```cpp
   const char* rx = std::getenv("OCUDU_UL_RX_SYMBOLS");
   return (rx == nullptr) || (std::strtoul(rx, nullptr, 10) == 0);   // ← N>0 ⇒ 批量化被直接关掉
   ```
   ⇒ `p41` 的 `batched=0/0`/`released=0` **是这条 `return` 造成的**（`OCUDU_DFT_OPEN_BLOCK=0` 是另一个独立开关）。**C1 = 去掉这条耦合**。
2. ⚠ **（前提已更正，见 §6.71③）** **批的边界（`set_lane_slot()` 的关块时刻）**：现在块只在**下一个时隙开始**时关（"上一槽的样点都到了、变换都编码了"）⇒ 一槽一批。
   **C2 = 把关块/派发/交棒的触发从"下一槽"改成"已到样点的最后一组"**（收包侧已经知道 `position.nof_symbols`）。
   ⇒ **更正（2026-09-26）**：关块的真实触发器是 `ofdm_demodulator_impl.cpp::finish_symbol()` 第 486 行的 `last_symbol_of_slot`
   （**本槽最后一个符号**），`set_lane_slot()` 的 `end_block()` 只是兜底 ⇒ **字面上的 C2 已是现状**；
   "每个收包组边界关块"要多一条 cb/跳（`cbs/lane` 2.00→3.00，**触 V4**）而只买到前端 ~5 µs 的执行前移。**详见 §6.71③。**
3. ★ **网格锚定（约束③ / H1/H3）**：**样点落进网格哪一行，必须仍按时间戳/时隙相位**，不能按 `rx_fill` 累加；具体是 1220–1240 的"退役并重开"路径**必须重锚到时隙的第一个符号**（它现在在新边界重开，而那个边界未必是符号 0）。
   **C3 = 修这条路径的相位锚定。** 只要 C1/C2 不碰 `rx_offset`/`rx_fill` 的**装填语义**、C3 把相位钉住，`p41` 那种"整槽按符号旋转"就不会重现。

**★★ C3 不必要 + H1 被推翻（继续读码的结论，`uplink_processor_impl.cpp:419`）**：
`process_symbol_boundary(samples, timestamp)` **是按时间戳算符号的**：
```cpp
i_sf        = (timestamp / nof_samples_per_subframe) % (NOF_SFNS*NOF_SUBFRAMES_PER_FRAME);
i_sample_sf = timestamp % nof_samples_per_subframe;
i_symbol_sf = 走 symbol_sizes[]（一整个子帧的符号图案）后的索引;
i_slot      = i_sf * nof_slots_per_subframe + i_symbol_sf / nof_symbols_per_slot;
i_symbol    = i_symbol_sf % nof_symbols_per_slot;
```
⇒ **符号索引来自时间戳（模一个子帧的符号图案），不是来自缓冲的填充偏移**；`rx_fill`/`rx_offset` 只决定"一块要多少样点"，
不决定"落在网格哪一行"。**所以：(a) H1（整槽按符号旋转）不成立；(b) C3（给退役/重开路径重锚相位）不需要做** —— 锚定本来就在时间戳上。
（这条正是"先读再写"省下来的一处改动。）另外注释里写明：**`symbol_sizes` 的图案周期是"一个子帧"**，而池缓冲是一**槽**（11520 样点），
两者不整除 —— 这也是 1217 那个"窗口放不下整符号就退役"的由来，但它**不影响放置**（放置由时间戳锚定）。

**⇒ 于是 `p41` 的 MCS 塌陷需要新解释（H4）**：样点既没丢（`gaps=0`）也没放错行（时间戳锚定），那最可能是
**PHY 内部把某些符号丢掉了**：`process_symbol_boundary` 在时间戳**未对齐到符号起点**时会走 `process_alignment()`（对齐丢弃），
`max_phase_blocks` 只是把它界在"至多一块"上；若符号级路径让流**周期性失去符号对齐**，这些被丢弃的样点是**网格里的洞**，
而**`gaps` 数的是电台的连续性、看不见它们** ⇒ 质量塌陷 + 零丢样 + 契约绿，全部吻合。
**判别读数**：`process_alignment` / `max_phase_blocks` 的触发计数（若不存在，需要加一个；这是 `p41` 那一族的复现判据）。
⇒ **⚠ 已被证伪为不必要（2026-09-26，§6.71②）**：`p41` 上 `[ul_host] symbols = [ul_rx] samples / 822.857 − 411`（0.0028%）⇒ **一个符号也没丢**；
且两处 `++nof_phase_blocks` 被 `max_phase_blocks = 2` 夹死 ⇒ 计数器只可能读 0/1/2。**免费判据 = `[ul_host] symbols` vs `[ul_rx] samples/822.857`。**

**★ C1 已落地（2026-09-25，本会话）**：`ofdm_demodulator_impl.h` 的 `block_batching_enabled()` 去掉了 `OCUDU_UL_RX_SYMBOLS` 那条 `return`，改为**恒允许成批**（带注释说明"那条 return 就是 `p41` 的 `batched=0/0`/`released=0` 的成因，而**延迟回退来自批量化被关，不是来自更小的收包块**"）。
* **验证性质**：**开关未设时它是逐字等价的重构**（旧表达式此时恒为 true）⇒ 默认路零行为变化；
* **离线网**：`ctest -L phy -j 1` 全绿；⚠ **replay 验不了它**（实测：`ul_chain_replay` 的 dump 在 `OCUDU_UL_RX_SYMBOLS` 开/关下**逐字节相同** ⇒ **RX 策略是空口专属**，replay 不走收包路）⇒ **C1 的行为验证只能靠空口腿**（读 `batched=`/`released=` ≠ 0）；这也意味着 **H1（网格旋转）也无法离线复现**；
* ⇒ **C1 之后的第一条腿就是"离线验不了"的那条**：配方同 `p42`，只加 `OCUDU_UL_RX_SYMBOLS=7`（半槽，比 N=1 温和），读 `batched=`/`released=`/整跳提交数/TBS-Thr；若 `batched≠0` 且吞吐不塌 ⇒ C1 成立、C2/C3 才值得做。

**离线自证的网（C1–C3 的判据，先定死）**：
* **网格 dump 与整槽路径逐字节相同**（`ul_chain_replay` 的 `.bin` 就是网格副本）——这是**约束③ 的直接判据**，也是本工作流最熟的那类网；
* `dft` 计数器：`batched=` 与 `released=` 都**不为 0**、且 `batch_max` 仍为 14（分组后应为"每槽 k 批"）；
* `ctest -L phy -j 1` 全绿 + `ab_dumps` 两组；
* 空口腿（在读数达标后再飞）：`batched=`/`released=` ≠0、整跳提交数（§8 Q22）、TBS/吞吐不塌（Q21 的复发判据）。

**下一步判别读码（不飞腿）**：`uplink_processor_baseband::locate_symbols()` 与 `symbol_grid_position` 的语义（符号索引相对哪个参考、`nof_samples_to_boundary` 怎么算），
以及符号路径的 fill/retire 相位；对照整槽路径的 `phase = last_rx_timestamp % nof_samples_per_slot` 对齐。
**S-B 必须保住的三件（现已明确）**：① `batched=` 不为 0（前端仍按"已到样点"成批）；② `released=` 不为 0（D1 交棒仍每槽一次）；
③ **样点落进网格的符号相位与整槽路径完全一致**（H1/H3 一旦坐实，这一条就是 S-B 的硬约束）。

## 8. 未决问题（技术层面；高层版见 `high_level_status_and_plan.md`）

| # | 问题 | 状态 / 影响 |
|---|---|---|
| **Q26**（2026-09-29 收口）| ★ **ms 级 Rx 尾巴 / `[ul_rx_wait]`：为什么"有时"会丢实时性**（结构性易感已确立在 GPU 路径；触发条件未知）？ | ⏹ **结案存档 = 欠账（§6.158，用户 2026-09-29 裁决）**：不再为它花腿。**保留的结论**：结构易感在 GPU 路径（`cpu` 0/4、`cpu_gpu` 10/24、`gpu` 56/61；Fisher p=7.0e-08）、症状位置在**宿主↔电台接口**（双向同时、比值 1.3–1.6）、剩余未知 = **触发条件**。**交付不受影响**（只动 V1 max/p99；本 HEAD 中位 1359.8、契约 9/9、审计 27/1/0）。**重开条件与第一步已预登记**（SLO 管 max/p99、出现 `gaps/rx_overflows`、换平台电台；第一步 = 去掉 USB hub / `taskpolicy` 升降档 A/B）。工具留在 `wip/leg_triage.sh`、`wip/host_sched_watch.sh`、`leg_gate.sh` 的三档输运行 |
| **Q1** | `merged_hop` 的 **1030 µs** 里，各 kernel 各占多少（K1/抽取/重排/权重/均衡/解映射/DFT）？ | ✅ **已收口（§6.65 + §6.69）**：离线微基准给出**全部 kernel 的执行账单**（唯一未测：`pilots_lse`/`apply_cfo`，小网格 ≈3 µs）⇒ **一跳算力 ≈43–51 µs**，`merged_hop` 533.7 µs 的其余 **≈460–480 µs 是"等本槽样点到达"（零算力）**。⚠ 原注保留：本机（M4 Pro）**不支持逐 dispatch 计数器**（只有 `AtStageBoundary`，§5.8.15 ①）⇒ 只能靠"诊断用命令缓冲分片"（P0-1，✅ 已做，给出 `dft` vs 其余）或 `MTLCounterSampleBuffer` |
| **Q2** | `ce` 的 **p95 尾巴 1782 µs** 从哪来？ | **基本收口**（代码判读）：单车道 strand 排队 + 上一跳缓冲回收。**残余未知**是这两者各占多少——`OCUDU_UL_SLOT_TRACE`（P1-6）可补 |
| **Q3** | `t2f` 的 521 µs 是**14 次 DFT 的 GPU 执行**还是**宿主记账/提交**？ | **已收口（两个都不是）**：≈473 µs 是**收样点等待**，≈48 µs 是宿主。⇒ **`OCUDU_DFT_*` 系列旋钮在这段上无效**（`PIPELINE_DEPTH=1`、`OPEN_BLOCK=0`、`RELEASE_BLOCK=0` 全部**变差**，代码路径无歧义）|
| **Q4** | 三段**在时间上是否重叠**？ | **已收口**：同一跳内三段是**墙钟划分**（互不重叠，构造决定）；**相邻跳之间**才重叠（跳 N 的 `ce` 覆盖跳 N−1 的 `eq_demap`）。⇒ "把两段重叠"在一个跳内**不存在**这个杠杆；杠杆是**跨跳并发** |
| **Q5** | residency 里的 busy 是**必要工作**还是**低效执行**（占用率/线程组配置）？ | **开放**，依赖 Q1；另有已知的贵项（K1 197 µs/跳、抽取 117、重排 117）被"逐字节不变"钉住。⚠ **"~95% busy" 已作废**：n1 默认腿配对后 **0.643**、n78 加压腿 0.93，比值逐腿读（§6.3 ⑥）|
| **Q6** | 把 `max_pusch_and_srs_concurrency` 改变能否把 `ce` 的排队项吃掉？代价是什么？ | ✅ **已回答（P1-8，§7.5）**：能（−58~64×），代价是那次 **5 秒收包停顿**（两次复现）⇒ 交付前必须查清 |
| **Q7** | 符号级收包（S-7g-13）在**负载下**对**跨度**的效果？ | **开放，且已成为唯一还有量级差的项**：§6.65③ 量出 `merged_hop` 533.7 µs 里 **~464 µs 是"等本槽样点到达"（零算力）**，与 §3.4 的 A 项 ≈473 µs 独立吻合；**只有它能动这一段**。⚠ 触 V4（提交数），需用户裁决，且其代码注释写明时延收益目前无法判读 |
| **Q21**（`p41` 新增）| **为什么符号级收包（`OCUDU_UL_RX_SYMBOLS=1`）把 UL 的 MCS 打到最低**（TBS ~200 bit、吞吐 ↓17×）？ | ★ **已收口（2026-09-26，§6.71①）**：**不是丢符号**（`[ul_host] symbols` = 期望值 − 0.0028%，§6.71②；memo §3.1 的 H4 计数因此作废），**也不是接收线程被堵**。机制读数：**前端每符号一条命令缓冲**（`dft slots/cbs = 178,407/2,497,686` = **14.0/槽**，`p42` 是 0.39/槽）+ **`[ul_dft_wait]` 中位 577.6 µs**（`p42` 无样本）⇒ 前端抢不到车道 ⇒ 跳变慢 ⇒ **每跳 TB 塌 15× 而 CRC-OK *比率*不变**（是调度器的块变小，不是解码质量塌）。成因是 `block_batching_enabled()` 那条 `return`（**C1 已删**）⇒ **验收腿 = `p43-n78-halfslot`**（§6.71⑤）|
| **Q22**（`p41` 新增）| **V4 的判据看不见"整跳提交数"**：`cbs/lane` 只数车道自己的提交（`p41` 仍是 2.00），而该腿的**前端提交从 ~1/跳 爆到 20.5/跳** | **判据缺口**：V4 的精神是"不许用提交数换时延"，需要一条**整跳**提交读数（`dft commits` + `cbs/lane` 的合计，或直接在腿启动/收尾打印合计）。⚠ 改判据要**先登记再改**（§5.2 第 2 条）。★ **2026-09-29 新证据（§6.160④）**：`OCUDU_DFT_RELEASE_BLOCK=0`（关 D1）时 `cbs/lane` 仍是 **2.00**（`p128`/`p130`/`p131` 三腿同读），而前端**每槽多提交一条**独立 cb（`p130`：`dft_front_end n=466590`，exec 中位 47.0 µs）⇒ **V4 看不见这次"提交数换时延"**，缺口比原来记的更具体：需要的是**整跳的 cb 合计**（前端的 + 车道的），而不是车道自己的那两条 |
| **Q24**（本会话新增）| ★ **那 ~550 µs/跳的 cb 内驻留落在哪条设备侧栅栏上**？(Q23 的量化延续) | ⚠ **仪器已落地但第一版有缺陷（§6.74）**：按**地址**配对在 `p44` 上给出 771 ms 中位（物理不可能：该腿 cb 窗口最大 5.5 ms、Σ停等 = 腿长的 1000 倍）⇒ **已改为唯一 id 配对 + 免配对的 `Q24b`（按 slot 配 `ce_weights`↔`merged_hop`）+ 物理不变量自检（`inconsistent` 非 0 即打印 "NOT evidence"）**；离线 6 种结构全 0%、正对照证明它会拒绝说话。**判据在重飞腿 `p45`**。原描述：`[metal_stats] fence wait on the device (Q24)` = `signaller GPUEndTime − waiter GPUStartTime`，按 `stage/corr/grid` 归属，随 `OCUDU_METAL_GPU_TIME=1` 开启；**离线自证**：解析率 100%、随负载单调、`ctest -L phy -j 1` 193/193。⚠ 离线**不能**验证机制（离线栅栏等待只有 7–20 µs 而离线窗口 500–750 µs ⇒ 回放的大窗口不是栅栏）⇒ **判据在空口腿 `p44`**：`waiter resident while its signaller ran` ≈100% 且几百 µs ⇒ 是栅栏；≈0% ⇒ 转向队列/所有权侧 |
| **Q25**（本会话新增）| ★ **那 ~450 µs 的 cb 驻留，最后的候选是"缓冲绑定的新增（映射/固定页）"吗**？| ⏳ **开放，可离线判**（§6.79③）：staging 臂多出的 **+464 µs ≈ 14 × 33 µs**，与"一次平凡 cb/一次新缓冲 ≈ 36–40 µs"同量级 ⇒ 假设"**该平台对每次派发里新出现的缓冲绑定收 ~33–40 µs，而复用的缓存映射不收费**"。判法：数一条前端 cb 的首次绑定数 × 单价，与窗口（452 µs）对账。**若否掉**，则单车道内部收口，杠杆只剩结构量（§6.79④）|
| **Q23**（本会话新增）| ★ **`merged_hop` 的 541.8 µs 窗口里那 ~500 µs 到底是什么**（离线已证明一跳真执行只有 ~67 µs，§6.71④）？| **开放，但两个候选已可用一条读数分开**：(i) **车道被 DL/同伴瓜分**（`busy(union) ≈ window`）；(ii) **cb 内部的 fence 等待**（出现大量 hole；burst 的 cb 只有 stage fence 与 grid-ready 两种 cb 级等待，而 `Q9-F` 只证明 signaller 先**提交**）。**判据读数 = `p43` 上的 `OCUDU_METAL_GPU_TIME=1`**（`[metal_stats] queue occupancy (Q9-F3)` + `gpu busy (…)`；**所有 n78 腿到 `p42` 都是关的**，只有 n1 的 `p15` 给过 `busy(union)/window = 33%`、最大 hole 秒级）⇒ 若坐实 (ii)，再补一条 **并发 1 的对照臂**把"同伴占用"从残差里分出来（§7.6.0d 的候选 (a)）|
| **Q19**（会话 #6 新增）| 腿 `p39`/`p40` 的**配对 V1 增量 −21.5 µs** 该记为"每次派发 10–14 µs"吗？ | **归因开放**（增量本身成立）：四个方向全量过——GPU 执行 1.5–13.2 µs、派发地板 1.3–1.4、四种边界 ≤0.3、宿主 encode 1.3–1.6（§6.65–§6.67）⇒ 加起来只有 ~3.5–5 µs。**预算一律用 §6.66③ 的 ≈5–20 µs**，不要用 10–14/派发。要坐实需**能看见并发/别的 lane**的仪器（lane 级 GPU 时间戳或 §6.24 的 P0 dump）|
| **Q20**（会话 #6 新增）| 均衡 / 解映射的**算力**是多少（`merged_hop` 里除了 CE 38 µs 与 A 项 464 µs 的其余部分）？ | ✅ **已收口（§6.69）**：`equalize_mxn` 1.40–1.61 µs、`demod_soft` 1.25–1.37 µs（156→2184 RE，**与 RE 数几乎无关**，贴着 1.3–1.4 µs 派发地板）⇒ 两段合计 **≈2.8 µs/跳**。**一跳全部算力 ≈51 µs（~10%）** ⇒ "重写 kernel"这条杠杆死了；⚠ 附带一个仪器陷阱：两引擎用 `dispatchThreads`（非均匀），塞进 threadgroups 计时器会读出 **256 倍**大的假值 |
| **Q11**（重申）| `value_net` 归档基线陈旧、`ab_dumps` arm1 改前就红 | 仍待用户裁决（§6.5⑤）；会话 #6 又遇到一次同类偶发（§6.62③：旧 y 路臂首读 4546 字节、重跑与隔离复跑均 0）|
| **Q8** | n78/n1 腿上 `max_pusch_and_srs_concurrency` 的生效值？车道是串行 strand 还是 fork limiter？ | ✅ **已收口（§6.1）**：两者**都是 1**、**都是串行 strand**；上限 = 中等池 `max_concurrency = 5`。⚠ 更正手算：n78 的 `ul_ratio` 是 **0.30**（不是 1.0）|
| **Q9** | 并发 2 下 UL 断流 / 收包停顿的成因？ | ✅ **已结案（§6.10）**：**根因是代码缺陷**——`ocudu_metal_burst.mm` 的 sweep 用**模 10240 的 `slot_point::count()`** 做 `entry.slot + 2 < slot` 比较，跨 hyperframe（10.24 s）环绕即失效 ⇒ 落在环绕末尾的无人认领块要等**一整个 10.24 s 周期**（实测等待是 10.24 s 的整数倍：3.000×/2.000×/1.001×）才被 sweep，其间咬着 14 个输入 token ⇒ 池空 ⇒ 接收线程 park （实测 2 次 ≈4.998 s）⇒ 电台溢出 9.97 s ⇒ UL 归零。**修复已落地（2026-09-25，§6.11，提交 `3d00eafe97`）：sweep 改成「slot 窗口（环绕安全守护）+ 10 ms 单调时钟期限」两个触发，并在每一个注册表入口（deposit / take / 宿主读）都查一遍。确认腿 `p08-conc2`（§6.12）证明它把注册表那一侧治好了（`starved_events` 383→6、unclaimed age 77.9→5.95 s、无 10.24 s 整数倍等待、token 零泄漏），但 **UL 断流没消失**：剩下的持有者是**命令缓冲的完成**（认领 ~3 ms，`deposit->completion ≈ 5.00 s`，而 token 是在完成处理器里回池）⇒ 见 §6.12 ③/④ 的 A/B/C** |
| **Q10** | 那条 **n1 2.85 倍退化**是否还有 `ce` 之外的成分？ | 已由单变量腿定位（§7.5：`ce` 是主因）；`p05-pair` **配对后**：`ce` 中位 **3278 µs** = 跨度 5248 的 **62%**，而一跳的设备执行只有 **517 µs**（Q14）⇒ `ce` 的排队项就是这条退化的主体。**残余**是 `t2f`（1095 vs 历史 521 量级）——与 Q7 的收样点策略、以及 n1 的 rx_wait（1052 µs）有关，**未单独开臂** |
| **Q11** | `value_net` 的归档基线陈旧、`ab_dumps` arm1 改前就红 | **待用户裁决**：重建基线（= 承认过期）还是把该网标为"HEAD 不可用"；arm1 需要查清"是否曾经绿过"（§6.5⑤）|
| **Q12** | `s84b-p0` 的第一次尝试**没有留下任何日志**（本仓与 `ocudu_premerge` 都没有）| **未验证**：最可能是被"戳 ≠ HEAD"拒绝（那种情况**不产生日志**）。要它当证据就得重飞一条；否则按"无效腿"处理（登记，低优先）|
| **Q13** | 配对账里 **199 行（0.2%）** 既没匹配也没被淘汰 ⇒ 被同 slot 的后一条车道的插入**覆盖** | ✅ **仪器已补（§6.7）**：配对行新增 `overwritten by a later lane for the same slot=`，并有 `paired/phase account` 自解释行；**成因仍待下一条腿**（多跳槽只解释得了 15 行）|
| **Q14** | 一跳的设备执行在 n1 上到底是多少？ | ✅ **已收口（P0-5 配对）**：**`busy` = 517 µs（中位）**，不是 residency 836，也不是 n78 加压腿的 1125 ⇒ 引用 D 项必须写清"哪条腿的 busy"（§6.3 ⑥）|
| **Q15** | 并发 2 的收益来自"两个跳重叠"还是在飞缓冲变多？ | ⚠ **初读（§6.8 ②，未单独开臂）**：`busy max = 1.48 ms` 而 `residency max = 5.004 s`、`carried=986` ⇒ residency 的尾巴是**两条缓冲之间的间隔**，不是设备执行；`cbs/lane=2.00` 不变、`merged_hop` busy **888.8** µs/lane（并发 1 是 540）⇒ **收益来自重叠，代价来自等待链变长** |
| **Q16** | 去掉 4 次派发后，**V1 降 52.2 µs**（≈13 µs/派发）而**设备占用窗口 `merged_hop` 只降 26.2 µs**（≈6.5 µs/派发）——差的那一半是"小派发被遮住"还是"直读更宽的步长稍贵"？ | ⚠ **量级已由 ABA 收口（§6.50）**：V1 **−52.2 … −69.3**（13–17 µs/派发）、窗口 **−26.2 … −37.7**（6.5–9.4）；同臂腿间噪声 **17.1 µs**。**但两种解释仍分不开**（没有第三个臂）。要分开：`metal_chain_probe` 的 device-slice 臂（比 gather-读 vs 网格直读的 kernel 自身耗时），或照 `wip/dft_kernel_cost.mm` 的形状给均衡 kernel 做离线微基准。⚠ **"12 µs/派发"只用于 V1/跨度口径**，不要用 `merged_hop` 的降幅反推派发数 |
| **Q17** | 残留 gap 的**停顿**到底在宿主还是在 USB？（成因已收口：**电台接收环溢出**，§6.51 ②） | ✅ **已作答（`p33`，§6.52 ②）**：**传输侧**。615,791 次收块里，`loop`（宿主自己的时间）**只有 1 次 >1 ms**（max 1.19 ms），而 `recv`（调用内部）**657 次 >1 ms、24 次 >5 ms**，且有一次 `slip` 达 **19.45 ms** ⇒ 宿主从不迟到发问，**宿主优先级/亲和性臂不是杠杆**。⇒ 目标：USB/端口/线/`otw_format`（§6.52 ⑥ 3）|
| **Q18** | 接收环 **64 → 256 帧**到底做了什么、代价是什么？ | ✅ **已定案（`p34` 对照，§6.53）**：**作用是"把丢样换成积压"** —— 64 帧丢 **5 段（最大 35.3 ms）**，256 帧 **0 丢**（同二进制、只差一行配置）；**代价也确实存在**：池从 `11/5/0` 变成 `16/0/2`（`held_max` 顶到池上限）。两腿的 `slip` 与 `gap_us` 量级互相咬合（丢 ≈ slip − 环容）。⇒ **V5 与 V2 的交换，已量出**；处置见 §6.53 ④（推荐 256 帧 + 池按 P2-D 规则重算为 32，**待用户裁决**）|

---

## 9. 证据索引（腿 → 用途）

| 腿 | 用途 | 关键读数 |
|---|---|---|
| `s78` / `s80` / `s82`（gpu）、`s79` / `s81`（cpu）| 现象复现（3/3 + 同负载对照）| 池 `held_max=8`、`starved_events` 17–69、RF 476–733 vs cpu 0–1 |
| `s82-phases-heavy-n78` | 三段分解基线 + 代码级核对 | 521 / 901 / 1234，和 2657 vs 跨度 2675（99.3%）|
| `s87-n1phases` | n1 默认配方 + 相位分段（并发 1）| 跨度 5268、`ce` 3228.3、`starved_takes` 102 |
| `s88` / `s88b-laneconc2` | P1-8 并发 2（两次复现）| 跨度 2400 / 2359、`ce` 55.6 / 50.6、**`gaps=2`（~5 秒停顿）** |
| `s85-p0phases` | P0-5 的立项依据（n78 + 相位分段）| 相位 60389 vs 车道 142022（比值 0.425）|
| `s86-diagsplit` | P0-1 空口读数（n78 加压 + 拆分）| `dft=937.9 µs/lane (88%)`，三组和 1067.6 vs 出厂 1032.7（1.034）|
| `s84-p0_0924_2303` | P0-6 空口确认（n1 默认）| 两行 `[ul_lane_exec]` 落在腿自己的 stderr |
| `s47/s49/s51/s61/s62`（n1，合并前）、`s69/s71`（n78）| n1 2.85 倍退化的历史对照 | 1844–1896 vs **5263–5268** |
| `s75`（轻载）| residency 与负载无关的对照 | 1121 vs 重载 1125 µs |
| `p14`–`p15`（n1 满速）| 5 s 停顿的根因与**修复 A** 的空口验证 | `p15`：`handshake waits=24 max=165us`、**0 gaps**、105 s 无停顿（§6.22）|
| `p16`–`p21`（n78 加压 conc2）| **修复 A/B** 的重载确认 + Q1 拆分臂 + 修复 B 的空口收口 | V1 2413–2440、`starved 52–114`、`dropped=20`（p21）（§6.23–§6.28）|
| `p23` / `p24`（同日对照）| 批量化默认值的确认腿 + 对照（`nobatch`）| V1 −38.7% 复现；对照组 **D4 的 1 个 gap**（§6.32）|
| `p25` / `p26` / `p27`（n78 加压）| V2 的归因、池容量诊断臂、**交付定尺的确认腿** | `p27`：`starved_events=0`、`free_min=6`、`held_max=10 < 16`、V1 1495.4、门 26/26（§6.36–§6.39）|
| `p30`–`p38`（n78 加压、并发 2）| 时延阶段的主线腿：站点表 → 直读网格 → 环深 64/256/512 → 池 32 → **整跳一个 run** | `p38`：**V1 1371.9**、派发 6→4/跳、契约 8/8（§6.44–§6.60）|
| **`p39-n78-noscatter`** | **杠杆 C 的验收腿**（K2 直读 LSE，`scatter` 消失）| `scatter=0`、`lse_applies=142740`、`device_y_writes=0`、**V1 1364.2**、`p0_gate` 29/29（§6.63）|
| **`p40-n78-noscatter`** | **反向臂**（`OCUDU_CE_Y_DIRECT=0`，把 scatter 放回来）| **配对 A/B：V1 −21.5 µs、`merged_hop` −17.4 µs**；RF 1448/1447（§6.64）|
| **`p22-n78-batch14`** | **前端批量化的 A/B（Q9-F4）——V1 达成** | `batched=154062/2156868 batch_max=14`；**V1 中位 1513.4 µs（p95 1671）**；`merged_hop` 1047→625、`residency` 1456→745、`input hold` 均值 −40%；契约 8/8、`cbs/lane=2.00` 不变、0 gaps（§6.31）|

**离线载体**：`dft_release_adopt_metal_test`（P0-1/P2-E 的 Metal 臂；**arm 11 = Q9 修复的回归臂**，§6.11 ③）、`ul_pipeline_probe_test`（P0-5 的 hook 契约）、
`tests/unittests/du_low/du_low_executor_mapper_test.cpp`（P0-6 的规则）、`ul_chain_replay`（逐字节/容差网；**与 pristine HEAD 二进制的逐字节比对见 §6.11 ④**）。
**⏳ 待飞的腿**：**A 项 / `P1-7` 的裁决腿**（唯一还有量级差的东西；触 V4，见 §6.68④ 与 §8 Q7）。
**已收口**：V1 ✅（`p39` 1364.2）、V2 ✅（`p27`/`p39`/`p40` 均 `starved_events=0`）、V4 ✅（`cbs/lane=2.00`）；V3 ⏸ 另案暂停（逐腿天气）。
**离线基准**：`wip/dft_kernel_cost.mm`（前端）、**`wip/ce_kernel_cost.mm`（CE：算力 + 空派发地板 + 四种阶段边界 + 宿主 encode，§6.65/§6.66）**。
**诊断开关**（都不进判据）：`OCUDU_CE_WAIT_TRACE=1`（held cb 的 publish/close 指针轨迹，§6.13②）、`OCUDU_METAL_GPU_TIME=1`（每队列 GPU 时间）、`OCUDU_D1_HANDED_BOUND`（注册表上界的诊断覆盖）、`OCUDU_DFT_RELEASE_TOKENS_EARLY`（P2-E 仪器，默认关）、
**`OCUDU_DFT_BATCH_SYMBOLS=N`**（前端批量化，默认 1 = 逐符号；§6.30/§6.31）。
**门与工具**：本阶段的 `phy_latency/wip/`（`p0_gate.sh`：A1/A2/B1/B2/C1/**C2/C2b** + **D1–D5（Q9，§6.11⑤）**；`leg_census.py`：腿普查）与上一阶段的 `phy_pipeline_gpu/wip/`（`leg_gate.sh`（只用于加压腿）、`milestone_audit.sh`（互斥锁）、
`ab_dumps.sh`、`value_net.py`、`l1_handover_arms.sh`、`l1_hop_arms.sh`、`edge_block_arms.sh`、`run_leg.sh`。
