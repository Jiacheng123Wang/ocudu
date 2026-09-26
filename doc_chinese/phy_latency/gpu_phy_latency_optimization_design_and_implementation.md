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

**判据现状（2026-09-25 会话 #6 结束时的腿）**：V1 ✅ **1364.2 µs**（`p39`，基线 2675.1 ⇒ **−49.0%**；配对 A/B 见 §6.64）；
V2 ✅ `starved_events=0`、`held_max=18 < 32`、`dropped=0`；V3 ⏸ **另案暂停**（RF 1447–1448 是逐腿天气）；V4 ✅ `cbs/lane=2.00 (max=2)`、crossings `0.00+0.00`；
V5 ✅ 契约 **MET 8/8**、`gaps=0`、`rx_overflows=0`、`p0_gate.sh` **29 of 29**（两臂）。**逐腿读数与出处见 §9 与 §6.63/§6.64。**

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
  且 `slip max(19.45 ms) ≪ recv max(100.6 ms)` ⇒ 那次调用**没有前驱**（`slip` 不计算第一次），即开流前的那一次。）
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
| 前端 DFT 的**执行**（batch=14，一槽一次派发）| **≈10.6 µs/槽** | §6.30 |
| **合计** | **≈43–51 µs ≈ `merged_hop` 的 ~10%** | —— |
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

## 8. 未决问题（技术层面；高层版见 `high_level_status_and_plan.md`）

| # | 问题 | 状态 / 影响 |
|---|---|---|
| **Q1** | `merged_hop` 的 **1030 µs** 里，各 kernel 各占多少（K1/抽取/重排/权重/均衡/解映射/DFT）？ | ✅ **已收口（§6.65 + §6.69）**：离线微基准给出**全部 kernel 的执行账单**（唯一未测：`pilots_lse`/`apply_cfo`，小网格 ≈3 µs）⇒ **一跳算力 ≈43–51 µs**，`merged_hop` 533.7 µs 的其余 **≈460–480 µs 是"等本槽样点到达"（零算力）**。⚠ 原注保留：本机（M4 Pro）**不支持逐 dispatch 计数器**（只有 `AtStageBoundary`，§5.8.15 ①）⇒ 只能靠"诊断用命令缓冲分片"（P0-1，✅ 已做，给出 `dft` vs 其余）或 `MTLCounterSampleBuffer` |
| **Q2** | `ce` 的 **p95 尾巴 1782 µs** 从哪来？ | **基本收口**（代码判读）：单车道 strand 排队 + 上一跳缓冲回收。**残余未知**是这两者各占多少——`OCUDU_UL_SLOT_TRACE`（P1-6）可补 |
| **Q3** | `t2f` 的 521 µs 是**14 次 DFT 的 GPU 执行**还是**宿主记账/提交**？ | **已收口（两个都不是）**：≈473 µs 是**收样点等待**，≈48 µs 是宿主。⇒ **`OCUDU_DFT_*` 系列旋钮在这段上无效**（`PIPELINE_DEPTH=1`、`OPEN_BLOCK=0`、`RELEASE_BLOCK=0` 全部**变差**，代码路径无歧义）|
| **Q4** | 三段**在时间上是否重叠**？ | **已收口**：同一跳内三段是**墙钟划分**（互不重叠，构造决定）；**相邻跳之间**才重叠（跳 N 的 `ce` 覆盖跳 N−1 的 `eq_demap`）。⇒ "把两段重叠"在一个跳内**不存在**这个杠杆；杠杆是**跨跳并发** |
| **Q5** | residency 里的 busy 是**必要工作**还是**低效执行**（占用率/线程组配置）？ | **开放**，依赖 Q1；另有已知的贵项（K1 197 µs/跳、抽取 117、重排 117）被"逐字节不变"钉住。⚠ **"~95% busy" 已作废**：n1 默认腿配对后 **0.643**、n78 加压腿 0.93，比值逐腿读（§6.3 ⑥）|
| **Q6** | 把 `max_pusch_and_srs_concurrency` 改变能否把 `ce` 的排队项吃掉？代价是什么？ | ✅ **已回答（P1-8，§7.5）**：能（−58~64×），代价是那次 **5 秒收包停顿**（两次复现）⇒ 交付前必须查清 |
| **Q7** | 符号级收包（S-7g-13）在**负载下**对**跨度**的效果？ | **开放，且已成为唯一还有量级差的项**：§6.65③ 量出 `merged_hop` 533.7 µs 里 **~464 µs 是"等本槽样点到达"（零算力）**，与 §3.4 的 A 项 ≈473 µs 独立吻合；**只有它能动这一段**。⚠ 触 V4（提交数），需用户裁决，且其代码注释写明时延收益目前无法判读 |
| **Q21**（`p41` 新增）| **为什么符号级收包（`OCUDU_UL_RX_SYMBOLS=1`）把 UL 的 MCS 打到最低**（TBS ~200 bit、吞吐 ↓17×）？ | **开放**：电台干净（`gaps=0`/`rx_overflows=0`/`rx_lates=0`）、契约 8/8、池绿 ⇒ **不是丢样**。候选：宿主被 20× 的接收调用打满（`[ul_rx] calls` 743k→14.8M、`recv over 1ms=18`、`load1=7.64`）、逐符号的 UL 时序/网格装配参考、或接收策略与 PUSCH 窗口的对齐。**它是"结构项能不能做"的前提**（§6.71）|
| **Q22**（`p41` 新增）| **V4 的判据看不见"整跳提交数"**：`cbs/lane` 只数车道自己的提交（`p41` 仍是 2.00），而该腿的**前端提交从 ~1/跳 爆到 20.5/跳** | **判据缺口**：V4 的精神是"不许用提交数换时延"，需要一条**整跳**提交读数（`dft commits` + `cbs/lane` 的合计，或直接在腿启动/收尾打印合计）。⚠ 改判据要**先登记再改**（§5.2 第 2 条）|
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
