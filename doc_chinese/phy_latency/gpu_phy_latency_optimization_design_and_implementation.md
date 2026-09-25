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
| **V3** | 电台 | RF 失败 **≤ 10**（cpu 量级 0–1）、`gaps == 0` | 同上 |
| **V4** | **不许用提交数换时延** | `cbs/lane ≤ 2.00 (max=2)`、`dropped == 0` | 同上 |
| **V5** | 不回归 | 契约 8/8、`crossings 0.00+0.00`/跳、CRC KO% 不劣化 | 同上 |

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
| `[ul_pipeline]` | `ul_pipeline_probe`（`include/ocudu/support/executors/ul_pipeline_probe.h`）| 一跳的端到端跨度（**与模式无关**）| 与 `[ul_gpu_pipeline]` **是两个口径**，不要混着比；`stale=` 数的是 >8000 µs 的样本 |
| `[ul_gpu_pipeline]` | 同上 | **车道模式下的同一跨度** | 本工作流的 **V1 就用它**（重载基线中位 2675 µs）|
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
| `[ul_rx_wait]` | 收包侧（`ul_pipeline_probe`）| 接收线程**一次 `receive()`** 的阻塞时长 | 三段覆盖不到时的去处；**它是 `t2f` 的一部分**（§2.3）|
| `[metal_stats] burst dispatches` | `ocudu_metal_burst.mm` | 一跳里各模块的 dispatch **次数** | 次数 ≠ 时间；不要用它推断时延 |
| `[metal_stats] gpu busy (front_end/back_end)` | `ocudu_metal_queue.mm` | **每队列**命令缓冲的 GPU 时间 | 只到"队列"这一层（`commits/busy/mean/window`），**不是 kernel 级** |
| `[metal_stats] dft handover …` | `ocudu_dft_metal_engine.mm` | 交棒：`handed/taken/superseded/evicted/…`、`keepalives=released/attached (max in flight)`、`(armed=…)`、**P2-E 的 `tokens_early=signals:N,by_event:M,by_complete:K`** | `keepalives` 的差额=**在飞**（不是泄漏），判泄漏看 `max in flight`（§5.9.120）；**P2-E：`by_event == 0` 表示"开关开了但释放没搬到前端"**，此时池数字应读作**未变**（§6.4）|
| **`[ul_lane_exec]`** | `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp`（**推导值 + 输入**）与 `lib/du/du_low/du_low_executor_mapper.cpp`（**执行器形态**）| 车道的并发度：`max_pusch_and_srs_concurrency` 的生效值、`bw/layers/ul_ratio/cpus`、以及它是**串行 strand** 还是 **N 路 fork limiter** | **每次启动打两行，在电台打开之前**（短跑一次 `gnb -c <配置>` 就能离线读到，不必飞腿）。**实测**：n78 `bw=20MHz layers=1 ul_ratio=0.30` → **1**；n1 `bw=5MHz ul_ratio=1.00` → **1** ⇒ **都是串行 strand**；池上限定死这个值的上限（超过池子直接**报配置错误**，不夹取）|

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

### 5.2 五条纪律（每条都是血换来的）

1. **判据有三个绑定维度**：**工况**（默认 vs 加压，`--regime=` 让腿自报）、**流量**（零流量腿无效）、
   **几何**（A1-2 的 C4 是 n78 PRAT 几何特定的；n1 上记"未判"）。
2. **不许为过关改阈值**；**"读不出"按 RED 算**；**单次红不是证据**（先复跑一次，且 detail 必须保留首次读数）。
3. **任何代码改动都会让"腿的提交证据"失效**（门会报 `the commit it ran, vs HEAD`）⇒ 代码阶段之后要重新飞腿。
4. **不要在用户飞腿时跑任何碰 GPU 的东西**（门/单测/replay/短跑 gnb 都算）；先 `pgrep -x gnb`。
   **进程检查永远按名字**：`pgrep -f` 曾匹配到自己的 shell，挡了用户一条腿（§6.5）。
5. 腿要**自己声明工况**：`run_leg.sh` 会把 `[leg] regime=` 写进腿的 stderr（provenance 的 knob 行已修好换行，
   早前粘在上一行导致行首 grep 读不到）。

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

## 7. 杠杆与候选改动（技术账）

### 7.1 归属式预算（优化对象的量化锚点，腿 `s82`，中位 µs）

> **先读 §2.3.1**：三段的**名字**与**窗口内容**不是一回事。下表是**按工作量归属**重写后的版本——它才是优化要用的一张表。

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
| **P1-4** | 池容量（**诊断用**，见 §7.4）| 时延不变而 `starved_events` 归零 ⇒ 因果链"持有→池干→阻塞→underflow"坐实；**不作为交付** | 待跑 |
| **P1-5** | `OCUDU_CE_CORR_*` / `OCUDU_CE_INV_BARRIERS`（相关矩阵的 barrier/分片）| 针对 D 项里已知的贵项（K1 串行 pivot、atan2 归约、reformat 链；见主文档 §5.8 的账单）逐项定量 | 待跑 |
| **P1-6** | `OCUDU_UL_SLOT_TRACE=N`（配对 `rxwait`/`t2f`/`ce` 的**逐槽**时间线）| 把 B 项（48 µs）拆开：executor 跳 / 14 次 encode / 交棒记账 / notify / 探针各占多少 | 待跑 |
| **P1-7** | 符号级收包（S-7g-13；`OCUDU_UL_RX_SYMBOLS`）**在加压腿上** | A 项（473 µs）的唯一候选杠杆。⚠ §5.8.29 只量过该段 −1.2%（未加压、且**丢了融合 1 次提交**）⇒ 本臂必须**同时读跨度与 `cbs/lane`**：若跨度降而提交数升 ⇒ 触 V4，交用户裁决 | 待跑 |
| **P1-8** | **车道并发度**（`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=N`）| ✅ 前置条件已满足（§6.1）：生效值 = **1**，可提到 **2..5**。读数看"跨度 / `ce` / `cbs/lane` / `dropped` / RF"。**用户已裁决**：V4 只约束**交付**，本臂**作为纯测量臂**放行 | ✅ **已跑并两次复现**（见 §7.5）|

### 7.3 P2 —— 结构/实现改动（按 P1 的结论选，不预设）

| 候选 | 形态 | 预期 | 风险 / 状态 |
|---|---|---|---|
| **P2-B** | **缩短 D 项（本跳设备执行 1125 µs）**：按 P0-1 的 kernel 拆分，针对最贵 kernel 改线程组/占用率/代数 | 直接砍 D；D 是 busy 97% 的实打实算力 | kernel 改动需逐字节/容差判据（`value_net` + `-L phy`）；⚠ 本机**不支持逐 dispatch 计数器** ⇒ P0-1 必须先做（✅ 已做） |
| **P2-F** | **车道并发度**（`max_pusch_and_srs_concurrency` 1→2）：让两条跳的设备执行**重叠**，把 C 项（901）吃掉一部分 | 若 GPU 余量（41%）够 ⇒ C 项可大幅下降（§7.5 实测：−55%）| **直接碰 V4**（提交数）⇒ **必须先出测量臂（P1-8）并把结论交用户裁决**（§7.4）。**未开工** |
| **P2-E** | **输入缓冲寿命解耦**（keepalive token 从"整跳完成"挪到"最后一个读输入的 dispatch"）| **不缩短时延**（V1 不由它达成），只把持有期从跨度压到前端那一段 ⇒ 治 V2（池饥饿）| ✅ 已实现（默认关）；⛔ **被平台证伪**（§6.4），**待用户裁决** |
| **P2-A** | **把估计器的"提取/权重"提前**：让它在网格 DMRS 符号就绪时就开跑，而不是等到跳尾 | 缩短 C（排队）与 D 的串行部分 | 与交棒次序耦合，需保证顺序正确（有 fence 机制）。**未开工** |
| **P2-D** | **池按流水线深度定尺**（`size = ceil(hold_p99 / slot) + margin`，而不是固定 8）| 用**测量**而不是拍脑袋定容量；它是"按设计定尺"，不是加大池 | **不缩短时延**；必须在 V1 达成后再按新时延重算（否则又是治标）。**未开工**；P0-5（§6.3）的配对分布正好是它的输入 |
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

## 8. 未决问题（技术层面；高层版见 `high_level_status_and_plan.md`）

| # | 问题 | 状态 / 影响 |
|---|---|---|
| **Q1** | `merged_hop` 的 **1030 µs** 里，各 kernel 各占多少（K1/抽取/重排/权重/均衡/解映射/DFT）？ | **仍然开放，且是头号仪表缺口**。⚠ 本机（M4 Pro）**不支持逐 dispatch 计数器**（只有 `AtStageBoundary`，§5.8.15 ①）⇒ 只能靠"诊断用命令缓冲分片"（P0-1，✅ 已做，给出 `dft` vs 其余）或 `MTLCounterSampleBuffer` |
| **Q2** | `ce` 的 **p95 尾巴 1782 µs** 从哪来？ | **基本收口**（代码判读）：单车道 strand 排队 + 上一跳缓冲回收。**残余未知**是这两者各占多少——`OCUDU_UL_SLOT_TRACE`（P1-6）可补 |
| **Q3** | `t2f` 的 521 µs 是**14 次 DFT 的 GPU 执行**还是**宿主记账/提交**？ | **已收口（两个都不是）**：≈473 µs 是**收样点等待**，≈48 µs 是宿主。⇒ **`OCUDU_DFT_*` 系列旋钮在这段上无效**（`PIPELINE_DEPTH=1`、`OPEN_BLOCK=0`、`RELEASE_BLOCK=0` 全部**变差**，代码路径无歧义）|
| **Q4** | 三段**在时间上是否重叠**？ | **已收口**：同一跳内三段是**墙钟划分**（互不重叠，构造决定）；**相邻跳之间**才重叠（跳 N 的 `ce` 覆盖跳 N−1 的 `eq_demap`）。⇒ "把两段重叠"在一个跳内**不存在**这个杠杆；杠杆是**跨跳并发** |
| **Q5** | residency 里的 busy 是**必要工作**还是**低效执行**（占用率/线程组配置）？ | **开放**，依赖 Q1；另有已知的贵项（K1 197 µs/跳、抽取 117、重排 117）被"逐字节不变"钉住。⚠ **"~95% busy" 已作废**：n1 默认腿配对后 **0.643**、n78 加压腿 0.93，比值逐腿读（§6.3 ⑥）|
| **Q6** | 把 `max_pusch_and_srs_concurrency` 改变能否把 `ce` 的排队项吃掉？代价是什么？ | ✅ **已回答（P1-8，§7.5）**：能（−58~64×），代价是那次 **5 秒收包停顿**（两次复现）⇒ 交付前必须查清 |
| **Q7** | 符号级收包（S-7g-13）在**负载下**对**跨度**的效果？ | **开放**：§5.8.29 只量过**该段** −1.2%（当时未加压、且当时丢了融合 1 次提交）⇒ 必须在加压腿 + 融合路径上重量一次 |
| **Q8** | n78/n1 腿上 `max_pusch_and_srs_concurrency` 的生效值？车道是串行 strand 还是 fork limiter？ | ✅ **已收口（§6.1）**：两者**都是 1**、**都是串行 strand**；上限 = 中等池 `max_concurrency = 5`。⚠ 更正手算：n78 的 `ul_ratio` 是 **0.30**（不是 1.0）|
| **Q9** | 并发 2 下 UL 断流 / 收包停顿的成因？ | ✅ **已结案（§6.10）**：**根因是代码缺陷**——`ocudu_metal_burst.mm` 的 sweep 用**模 10240 的 `slot_point::count()`** 做 `entry.slot + 2 < slot` 比较，跨 hyperframe（10.24 s）环绕即失效 ⇒ 落在环绕末尾的无人认领块要等**一整个 10.24 s 周期**（实测等待是 10.24 s 的整数倍：3.000×/2.000×/1.001×）才被 sweep，其间咬着 14 个输入 token ⇒ 池空 ⇒ 接收线程 park （实测 2 次 ≈4.998 s）⇒ 电台溢出 9.97 s ⇒ UL 归零。**修复已落地（2026-09-25，§6.11，提交 `3d00eafe97`）：sweep 改成「slot 窗口（环绕安全守护）+ 10 ms 单调时钟期限」两个触发，并在每一个注册表入口（deposit / take / 宿主读）都查一遍。确认腿 `p08-conc2`（§6.12）证明它把注册表那一侧治好了（`starved_events` 383→6、unclaimed age 77.9→5.95 s、无 10.24 s 整数倍等待、token 零泄漏），但 **UL 断流没消失**：剩下的持有者是**命令缓冲的完成**（认领 ~3 ms，`deposit->completion ≈ 5.00 s`，而 token 是在完成处理器里回池）⇒ 见 §6.12 ③/④ 的 A/B/C** |
| **Q10** | 那条 **n1 2.85 倍退化**是否还有 `ce` 之外的成分？ | 已由单变量腿定位（§7.5：`ce` 是主因）；`p05-pair` **配对后**：`ce` 中位 **3278 µs** = 跨度 5248 的 **62%**，而一跳的设备执行只有 **517 µs**（Q14）⇒ `ce` 的排队项就是这条退化的主体。**残余**是 `t2f`（1095 vs 历史 521 量级）——与 Q7 的收样点策略、以及 n1 的 rx_wait（1052 µs）有关，**未单独开臂** |
| **Q11** | `value_net` 的归档基线陈旧、`ab_dumps` arm1 改前就红 | **待用户裁决**：重建基线（= 承认过期）还是把该网标为"HEAD 不可用"；arm1 需要查清"是否曾经绿过"（§6.5⑤）|
| **Q12** | `s84b-p0` 的第一次尝试**没有留下任何日志**（本仓与 `ocudu_premerge` 都没有）| **未验证**：最可能是被"戳 ≠ HEAD"拒绝（那种情况**不产生日志**）。要它当证据就得重飞一条；否则按"无效腿"处理（登记，低优先）|
| **Q13** | 配对账里 **199 行（0.2%）** 既没匹配也没被淘汰 ⇒ 被同 slot 的后一条车道的插入**覆盖** | ✅ **仪器已补（§6.7）**：配对行新增 `overwritten by a later lane for the same slot=`，并有 `paired/phase account` 自解释行；**成因仍待下一条腿**（多跳槽只解释得了 15 行）|
| **Q14** | 一跳的设备执行在 n1 上到底是多少？ | ✅ **已收口（P0-5 配对）**：**`busy` = 517 µs（中位）**，不是 residency 836，也不是 n78 加压腿的 1125 ⇒ 引用 D 项必须写清"哪条腿的 busy"（§6.3 ⑥）|
| **Q15** | 并发 2 的收益来自"两个跳重叠"还是在飞缓冲变多？ | ⚠ **初读（§6.8 ②，未单独开臂）**：`busy max = 1.48 ms` 而 `residency max = 5.004 s`、`carried=986` ⇒ residency 的尾巴是**两条缓冲之间的间隔**，不是设备执行；`cbs/lane=2.00` 不变、`merged_hop` busy **888.8** µs/lane（并发 1 是 540）⇒ **收益来自重叠，代价来自等待链变长** |

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

**离线载体**：`dft_release_adopt_metal_test`（P0-1/P2-E 的 Metal 臂；**arm 11 = Q9 修复的回归臂**，§6.11 ③）、`ul_pipeline_probe_test`（P0-5 的 hook 契约）、
`tests/unittests/du_low/du_low_executor_mapper_test.cpp`（P0-6 的规则）、`ul_chain_replay`（逐字节/容差网；**与 pristine HEAD 二进制的逐字节比对见 §6.11 ④**）。
**⏳ 待飞的腿**：**Q9 确认腿**（§6.11 ⑤ 的配方与 F1–F8 判据；飞完在 §9 里补一行，并把 V1–V5 一起读出来）。
**诊断开关**（都不进判据）：`OCUDU_CE_WAIT_TRACE=1`（held cb 的 publish/close 指针轨迹，§6.13②）、`OCUDU_METAL_GPU_TIME=1`（每队列 GPU 时间）、`OCUDU_D1_HANDED_BOUND`（注册表上界的诊断覆盖）、`OCUDU_DFT_RELEASE_TOKENS_EARLY`（P2-E 仪器，默认关）。
**门与工具**：本阶段的 `phy_latency/wip/`（`p0_gate.sh`：A1/A2/B1/B2/C1/**C2/C2b** + **D1–D5（Q9，§6.11⑤）**；`leg_census.py`：腿普查）与上一阶段的 `phy_pipeline_gpu/wip/`（`leg_gate.sh`（只用于加压腿）、`milestone_audit.sh`（互斥锁）、
`ab_dumps.sh`、`value_net.py`、`l1_handover_arms.sh`、`l1_hop_arms.sh`、`edge_block_arms.sh`、`run_leg.sh`。
