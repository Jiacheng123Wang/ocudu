# macOS 上 PHY 关键线程的**运行稳定性** —— 高层现状与计划

> **本文件只放高层**：一句话现状、问题形状、线程地图、目标与判据、三层测量、阶段计划、待裁决。
> 细节（读数与证据、机制与归属、每个改动的实施记录、坑与教训）一律在
> **`thread_priority_optimization_design_and_implementation.md`**（下称"开发文档"）；
> 会话快照在 `session_handoff_*.md`。三类文档的分工沿用 `../phy_latency/README.md` 的约定。
>
> **本目录于 2026-10-01 按用户要求建立**：此后与"macOS 上线程运行稳定性 / 线程优先级 / QoS"有关的工作都放这里。
> 上游（历史）记录仍是 `../phy_latency/`（**追加式，不改历史**）；本线的**入口**是那里的
> 开发文档 **§6.248**（ms 级停顿收口为欠账）与 **§6.249**（macOS 测量规划）。

---

## 0. 一句话现状（2026-10-01）

> 交付侧是**绿的**（全量审计 **33 PASS / 0 FAIL / 0 RED**，两条当前 HEAD 的腿 `leg_gate` 都 **10 of 10**）；
> 但**尾部尖峰在每一条插桩序列上都出现**（`ce` max ≈ **20×** 中位、`ldpc` ≈ 8–15×、V1 max ≈ 4×、`[ul_rx_wait]` ~2 ms），
> 而 macOS **不提供硬实时**：我们标的 "RT" 实际只落实为 **QoS 类（P 核偏向）**，且 `taskinfo` 显示 **UI/IN 计费为 0、有效 QoS 天花板 = `THREAD_QOS_LEGACY`** ——
> **"我们请求的 QoS 到底有没有生效"目前没有读数**。本目录的工作 = 把这件事变成**可测、可归因、可设置**，同时**保证 Linux/Ubuntu 行为逐字不变**。

---

## 1. 为什么有这条线（问题从哪来）

1. 用户从系统"活动监视器"观察到 gnb 运行时常驻 **~200% CPU**，并注意到我们给 `lower_phy_rx#0`/`lower_phy_ul#0` 标的 "(RT)"。
2. 追问链：**是多少个线程？**（实测 loopback 18 / 真电台 24）→ **谁提交 GPU？**（池线程）→ **优先级是什么？macOS 怎么保持 RT？**（四层机制只有一层生效）→
   **`taskinfo` 显示 QoS 计费全在 UN、天花板 LEGACY**（悬案）→ 用户判断：**"max 的尖峰不仅仅出现在 `[ul_rx_wait]`，其他地方也出现过"**（已量化证实，见 §2）。
3. 上一段工作（`../phy_latency/`）已把 ms 级停顿**收口为欠账**（§6.248），并给出"重开条件 + 重开第一步"；本目录是那个"第一步"的**正式工作流**。

---

## 2. 问题形状（**可复核的读数**，2026-10-01）

### 2.1 尖峰跨序列、跨线程（用户观察成立）

| 序列 | 中位 | max（p163 / p180 / p182）| max/中位 | 归属线程 |
|---|---|---|---|---|
| `[ul_rx_wait]` | 0 µs | 760 / **2049** / **2159** µs | — | `lower_phy_rx#0`（UHD USB 收包）|
| `[ul_time_frequency]`(t2f) | ~490 µs | 1658 / 686 / 721 | 1.4–3.4× | `lower_phy_ul#0`（host 前端变换）|
| **`[ul_channel_estimation]`(ce)** | ~60 µs | **1256 / 1218 / 1228** | **≈20×** | `main_pool#N`（Metal CE 派发/提交）|
| `[ul_ldpc_decode]` | 27–60 µs | 425 / 455 / 444 | ≈8–15× | `main_pool#N`（CPU LDPC）|
| `[ul_gpu_pipeline]`(V1) | 1246–1257 µs | 4374 / 5033 / 4916 | ≈4× | 整跳（含以上全部）|
| `[dl_tx_call]` | 38 µs | **84498**（p163 = call #1 启动项）/ 232 / 221 | — | `lower_phy_tx#0`（UHD USB 发包）|
| `[ul_rx_timing] recv max` | — | 100.3–100.6 **ms**（每条腿同一常量）| — | 启动伪读数（`../phy_latency` §6.146）|

⇒ **(a)** 尾巴不是接收线程独有；**(b)** 每条序列的 max 由**调度/IO 事件**支配，不是模块算力；**(c)** **中位数是稳的**（这正是交付判据一直绿的原因）。
⇒ 因此判据必须**按线程 + 按序列**成立，**不能**用进程平均或单一序列代表。

### 2.2 进程画像（用户提供的 `taskinfo`，真电台腿，pid 85078）

| 读数 | 值 | 含义 |
|---|---|---|
| `CPU time / run time` | 686.0 s / 357 s | **平均 1.92 核**（= 活动监视器的 ~200%）|
| `P-time / E-time` | **99.55% / 0.45%** | 我们的线程**几乎全在 P-core** ✓ |
| `P/E switches` | 2.44 M（4%）| 迁移不频繁 |
| **`QoS time`** | **UI 0.000 / IN 0.000** / DF 94.78 / **UN 591.23** | ★ **没有 CPU 被记在 USER_INTERACTIVE/USER_INITIATED 上** |
| **`eff qos ceiling`** | **`THREAD_QOS_LEGACY`** | ★ 请求的档位**可能被钳** |
| `runningboard managed` / `req managed` | NO / NO | 进程**不是** RunningBoard 托管 ⇒ 每线程 QoS 的落地条件不明 |
| `req other` / `imp_donor` / `eff other` | boosted / CURRENTLY / live-donor | P 核 99.55% 更可能来自**进程级 boost/donation**，而非我们的 per-thread QoS |
| `csw` | **69.36 M / 357 s = 194 k/s** | 与 `[ul_timing_events]` 量到的 `ivcsw` 150–320/ms **同量级** |
| `interrupt wakeups` / `msgsent` | 54.66 M（153 k/s）/ 149.98 M（**420 k/s**）| 与内核交互极其频繁（GPU 驱动 + UHD/USB + 调度）|

### 2.4 ★★ 观察者会**扰动**被观察者（2026-10-01 实测，直接影响 P0 的配方）

| 腿 | 是否被观察 | `[ul_rx_wait] max` | `ce max` | `[dl_tx_slack] AT/BELOW 0` | `gaps` / `rx_overflows` |
|---|---|---|---|---|---|
| `p180` / `p182`（1319）| **否** | 2049 / 2159 µs | 1218 / 1228 µs | 1 / 1 | **0 / 0** |
| **`p182_1001_1726`**（23 min，腿跑 6 分钟后在其上跑了 `taskinfo`+`powermetrics`+`sample`）| **是** | **153 135 µs（153 ms）** | **13 267 µs** | **709**（`min = −167.7 ms`）| **1 / 1** |

* 该腿 V1 **中位**仍是 1262.4（**中位不动、尾部爆炸**）⇒ 与 §2.1 的形状一致，但量级大两个数量级；
* `sample` 会**挂起目标进程**逐个线程走栈；`powermetrics` 读调度器 ⇒ 二者都不是"无侵入"；
* ⇒ **规则**：`taskinfo`/`powermetrics`/`sample` **只能在专门的诊断腿上跑**（并明确标注"该腿的尾部含观察代价"）；真实腿只允许 `ps -M`（零侵入）。
  已落进 `wip/observe_threads.sh`（默认 light，`--heavy` 显式开启并打印警告）。
* ⇒ 这条同时是"**该平台对干扰没有余量**"的直接证据：几秒钟的观察就换来 153 ms 的停顿与一次丢样。

### 2.3 已立的读数（真实栈，`sample` 3 s）

| 线程 | 栈里的热点帧 | 结论 |
|---|---|---|
| `lower_phy_rx#0` | `ul_process()`、**`uhd::...::recv_packet_streamer::recv(...)`**、`clock_gettime_nsec_np` | 收包线程**停在 UHD 的 USB 收包里** |
| `lower_phy_ul#0` | `puxch_processor_impl::process_symbol`、`uplink_processor_impl::process_complete_symbol/process/process_symbol_boundary` | **前端（host 变换）在这条线程** |
| `main_pool#0..#4` | **`metal::mmse_engine::build_pilots_lse`**、`pusch_demodulator_impl::demodulate`、`ldpc_decoder_impl::decode`、**`metal::shared_queue::wrap_no_copy`**、`pdxch_baseband_modulator::handle_request` | **CE/EQ/demod 的派发+提交 + LDPC + DL 都在池线程** |
| `radio` | `radio_uhd_tx_stream::run_recv_async_msg`、`send_packet_handler::recv_async_msg` | UHD 异步消息/TX 控制 |
| `lower_phy_tx#0` | `dl_process`、**`uhd::...::send_packet_streamer::send(...)`** | DL 线程**停在 UHD 的 USB 发包里** |

---

## 3. 线程地图（代码 + 实测一致）

| 线程 | 角色 | 优先级意图 `os_thread_realtime_priority` | 本机数值 | macOS 上实际得到 |
|---|---|---|---|---|
| `lower_phy_tx#0` | DL 链（USB 发包）| `max()` | 46 | QoS 请求 `USER_INTERACTIVE`（生效性待测）|
| `lower_phy_rx#0` | 收包（USB 收包 + `[ul_rx_wait]`）| `max() − 1` | 45 | 同上 |
| `lower_phy_ul#0` | 上行前端（**host 变换** + 交棒）| `max() − 1` | 45 | 同上 |
| `radio` | RU/电台（UHD 异步消息）| `radio_worker_realtime_priority()` = macOS `max()−1` / Linux `no_realtime()` | 45 | 同上 |
| **`main_pool#0..#4`** | **PHY 各执行器；CE/EQ/demap 的编码+`commit()`；LDPC；DL 处理** | `max() − 2` | 44 | 同上 |
| `ru_timing` | RU 时基 | `max() − 0` | 46 | 同上 |
| `phy_worker`/`phy_exec`、`io_broker_epoll`、`io_timer_tick`、SCTP×5、main | 支持面 | `no_realtime()` | — | `USER_INITIATED` |

* `max() = sched_get_priority_max(SCHED_FIFO) − 1`（注释：**减 1 是为了不抢 OS 关键任务的最高档**）；本机 `sched_get_priority_max(SCHED_FIFO) = 47` ⇒ `max()=46`。
* 池大小由 `get_default_nof_workers()` 决定：`cells×(dl+ul) + cells + 2 = 5`（14 核，`avail_cpus − 3` 上限），腿里打印为 `medium pool max_concurrency=5`。
* 线程数：**loopback 18**（无 UHD 线程）/ **真电台 24**（多出 UHD 与 libusb 热插拔线程）。

---

## 4. macOS 的 "RT" 到底是什么（四层机制，**只有一层可能生效**）

| 层 | 机制 | 现状 |
|---|---|---|
| 1 | **QoS 类**（`pthread_set_qos_class_self_np`）：RT 意图 → `QOS_CLASS_USER_INTERACTIVE`，否则 `USER_INITIATED` | **唯一可能生效的一层**；但 `taskinfo` 的 UI/IN 计费为 0 且 ceiling = `THREAD_QOS_LEGACY` ⇒ **悬案** |
| 2 | Mach affinity tag（`THREAD_AFFINITY_POLICY`）| **Apple Silicon 不支持**（`KERN_NOT_SUPPORTED`）⇒ 本机**空操作**（代码注释已写明）|
| 3 | POSIX `pthread_setschedparam(SCHED_FIFO, prio)` | 实测**无特权即成功并读回**（`policy=4`=SCHED_FIFO、`priority=46`），但按代码注释内核**只记录**，调度仍由 QoS 主导 |
| 4 | **Mach 时间约束**（`THREAD_TIME_CONSTRAINT_POLICY`）| **实现存在但默认不施用**（生产路径无人调用；`bind_thread_to_performance_core()` 只在单测里）。2026-09-01 曾自动施用，**与一次 OAI-UE 随机接入回归同时出现** ⇒ 回退为显式 opt-in |

⇒ **结论**：macOS 上我们**没有硬实时**；"RT" 的实际含义是"QoS 提升 + 尽量给 P-core"。这与观测到的 `ivcsw` 150–320/ms、ms 级停顿**不矛盾**。

---

## 5. 目标与判据（**先写死**）

### 5.1 目标（四条）
1. **可测**：每条关键线程都能给出**它自己**的 CPU/调度读数（不是进程级）；
2. **可归因**：任一 max 尖峰能落到"**哪条线程 + 哪一类原因**（CPU 被抢 / 等 IO / 等同步 / 等 GPU）"；
3. **可设置**：能对关键线程施加并**回读**优先级/QoS/时间约束，且**有档位对照**；
4. **Linux/Ubuntu 逐字不变**：见 §8。

> ⚠ macOS 上**不承诺**"达到 Linux 的确定性"；本线的判据写成"**尾部率/最坏值 ≤ 登记阈值**"，并**同时记录 Linux 的同读数**作对照。

### 5.2 判据（阈值**从分布导出、飞行前登记**，不在这里拍）
* 主判据载体（每条腿）：`[ul_rx_wait] max`、`[ul_time_frequency] max`、**`[ul_channel_estimation] max`**、`[ul_ldpc_decode] max`、`[ul_gpu_pipeline] max`、`[dl_tx_call] max`（**排除 call #1 的启动项**）、`over 1ms` 计数、`[RF]` 实时失败率；
* 候选阈值规则：**max ≤ k×中位**（k 由最近 N 条腿的 p99.9 导出）或"**p99.9 ≤ 登记值**"；
* **反例判据（必须同时登记）**：任何改动都**不得**让 Linux 上同一读数变化；Ubuntu 侧编译 + `ctest` 必须绿。

---

## 6. 三层测量（由内到外）

1. **线程级（新）**：关键线程记录**自己**的 `cpu_usage` / 调度状态 / 被抢次数 / 请求档 vs 实际档 ——
   macOS：`thread_info(mach_thread_self(), THREAD_BASIC_INFO)` + `pthread_get_qos_class_np()`；Linux：`RUSAGE_THREAD`（同接口 `#else`）。
2. **事件级（扩展现有的 `OCUDU_UL_TIMING_EVENTS`）**：每条"最慢 K 条"加 **线程 id/名字 + 该线程的 CPU 增量**；`ivcsw` **归一化**（/ms、对本腿中位）；把 `ce`/`ldpc`/`t2f` 的**尾部**也接进同一机制（它们现在只有聚合值）。
3. **平台旁观（零代码，固化成腿配方）**：
   `sudo taskinfo <pid>`、`sudo powermetrics --samplers tasks --show-process-qos-tiers --show-process-wait-times --show-process-amp -n 3`、`sample <pid> 3 -file …`
   —— **腿的开始 / 中段 / 结束各跑一次并存档**（不再"出问题才手跑"）。

---

## 7. 阶段计划（P0–P4）

| 阶段 | 内容 | 判据 | 动代码 |
|---|---|---|---|
| **P0**（脚本已建，待跑腿）| 旁观命令固化成腿配方（`wip/observe_threads.sh`，**默认 light**）；尖峰清单（`wip/spike_census.py`，已能跑）；从分布**导出阈值候选** | 配方可复现；阈值有出处；**重观察只在诊断腿上**（§2.4）| 否 |
| **P1** | 线程级读数（§6.1）+ 事件加线程名/线程 CPU（§6.2）+ `ivcsw` 归一化 + **`[sched]` 启动自读**（每线程：请求 vs 实际 QoS、POSIX prio、是否 RT）| 单测双向（开着有值、关着**逐字节不变**）；**loopback 6 秒即可读出真实档位** ⇒ 回答 §4 的悬案 | **是** |
| **P2** | **零代码 A/B**：`taskpolicy -l/-t` 各档 + `powermetrics` 对照（同一二进制）| 尾部率是否**跟着档位走** | 否 |
| **P3** | 若 P1 显示"请求被钳"或 P2 显示档位有效但不足：把**已存在但未使用的 attr-QoS**（`set_pthread_attr_qos_class`）接到线程创建 | 关键线程**从第一条指令**就在 P 核；尾部不劣 | **是** |
| **P4** | 最硬：**Mach time constraint**（`bind_thread_to_performance_core()`），参数先用 `[ul_rx_wait]`/`t2f` 分布**离线标定** | 单腿 + ★ **预登记回退条件**（`gaps>0`/`rx_overflows>0`/契约红/OAI-UE 随机接入异常 ⇒ 立即回退）| **是** |

⚠ **成本（必须记住）**：**任何 `lib/` 代码改动都会让现有腿按审计规则过期**（`p181`/`p182` 是当前唯一全量 GREEN 的证据）
⇒ P1 与 P3/P4 应**合并成一次改动**、或接受"每次改动重飞一对腿"（default + stress）。**P0/P2 不动代码，可先做。**

---

## 8. Linux / Ubuntu 行为不变（三条保证 + 验证）

**保证**
1. 只动 macOS 层：`utils/macos_compat/`、`lib/support/scheduling/darwin_thread_scheduling.*`；头文件只暴露**平台中立**接口（`os_thread_realtime_priority` / `compat::` 包装），跨平台 `#else` 只写 **no-op**；
2. **两把钥匙**：所有新读数 `env` 门控（默认关）+ 已有编译期开关 ⇒ **关着时两个平台的报告都逐字节不变**；
3. **不改 Linux 的调度参数**：`radio_worker_realtime_priority()` 在 Linux 上仍是 `no_realtime()`；池/线程优先级数值、亲和性、`SCHED_FIFO` 用法一个字不动。

**验证（Ubuntu 台架已具备）**
* 台架：**`jwang@192.168.0.106:~/work/ocudu`**（用户 2026-10-01 提供，可用于编译/运行/测试）；
* 流程：同一 commit 在 Ubuntu 上 **编译 + `ctest -L phy`（或至少 `-R` 相关单测）**，并把结果记进开发文档的"Linux 复核"小节；
* 本机（macOS）侧：① `git diff` 逐行审"`#if defined(__APPLE__)` 之外是否有改动"；② 新单测在两平台都跑（Linux 断言 no-op）。

---

## 9. 与已收口线条的关系

* `../phy_latency/` 的 **Q27（ms 级停顿 / `[RF]` 实时失败）仍结案存档**（§6.248），**本线不重开它**；
* 本线是"**若要重开，从这里开始**"的可执行版本（§6.248⑥ 的两条第一步 = 本目录的 P1/P2）；
* 本线的产物（线程级读数、平台档位回读、旁观配方）对**任何**未来的尾部问题都通用，不只为 Q27。

---

## 10. 待用户裁决 / 待办

| # | 事项 | 建议 |
|---|---|---|
| 1 | **P1 是否现在做**（要动 `lib/` ⇒ `p181`/`p182` 过期，需重飞一对腿）| 建议：**P0 + P2 先做**（零代码），P1 与 P3/P4 合并成一次改动 |
| 2 | **QoS 悬案的解法**：加 `[sched]` 启动自读（env 门控）—— 这是 P1 的一部分 | 建议做；否则"我们有没有 RT"永远只是推断 |
| 3 | `taskpolicy` 档位 A/B 的**档位选择**（`-l`/`-t` 各档）| 建议先扫一遍档位，再挑 2 档做正式 A/B |
| 4 | Ubuntu 台架的使用方式（我 ssh 跑，还是你跑后贴结果）| 需要你定；`ssh` 可用的话最省事 |
| 5 | P4（Mach time constraint）是否列入计划 | 建议**列为最后选项**，且必须先离线标定参数 + 预登记回退条件 |

---

## 11. 文档索引（本目录）

| 类 | 文件 | 说明 |
|---|---|---|
| **高层现状与计划** | **`thread_running_high_level_status_and_plan.md`** | 本文件（活文档，可重写）|
| **开发文档** | `thread_priority_optimization_design_and_implementation.md` | **追加式**：详细设计、实施记录、读数与证据、坑与教训 |
| 会话快照 | `session_handoff_2026-10-01-1.md` | 最新快照（新会话先读它）|
| 目录约定 | `README.md` | 三类文档分工与本目录的 wip/logs 约定 |
| 工具/腿脚本 | `wip/*.sh`（**跟踪**）| 本线的腿配方与旁观脚本（`observe_threads.sh` 等）|
| 腿日志 | `wip/logs/`（**git ignore**）| 本线新飞的腿（老的 `../phy_pipeline_gpu/wip/logs/` 作为历史）|
| 临时产物 | `work_tmp/`（**git ignore**）| 采样文件、dump、scratch |

**上游（历史）**：`../phy_latency/`（Q27 收口 §6.248、macOS 规划 §6.249）、`../phy_pipeline_gpu/`（更早的阶段）。
