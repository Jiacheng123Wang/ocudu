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

## 0. 一句话现状（2026-10-01 晚）

> **悬案结了，而且答案比"被钳"更硬**：`[sched]` 自读（P1）第一次运行就显示 —— 我们请求的 `USER_INTERACTIVE`
> **从来没有生效过**，因为**紧跟着的 `pthread_setschedparam(SCHED_FIFO)` 把它静默抹掉且不可恢复**（实测：再设返回 EPERM）。
> 今天 macOS 上**数据面线程处于"无 QoS 档"**，比它们本该压制的 `io_timer`/`io_broker` 还低一档；
> `taskinfo` 的 `UI/IN 计费 0 s` 因此不再需要猜测（开发文档 **10.5**）。
> **P0 完成**（脚本 + 阈值候选，规则被自己的第一次运行推翻 → 改成 C1/C2，见 §5.2 与开发文档 10.4）；
> **P1 的代码已实现并本地验证**（线程级读数、事件带线程名/线程 CPU、`ivcsw` 归一化、相位尾部、A/B 开关），
> `ctest -L phy` **207/207**、loopback 两条臂都跑过；**P2 配方与预登记已写好**（开发文档 10.7）。
> **下一步要用户裁决两件事**（§10）：是否把"跳过 POSIX 调用"立为新的 macOS 默认，以及飞哪几条腿。
> **交付侧 GREEN**：**`p187-n78-default`（20:44 那次）+ `p188-n78-stress`**（HEAD `76860a9afb`）——
> 全量审计 **33 PASS / 0 FAIL / 0 RED**（连跑两次一致），`leg_gate` 两条各 **10/10**；
> 三项预登记（新默认 `eff=USER_INTERACTIVE`、相位事件带 `cpu=`、10/10）**全部命中**（开发文档 10.17）。
> ⚠ 同一标签的 **20:40 那次作废**：它在 `12:41:07` 遭遇一次 ~15 ms 的**整进程冻结**（21 次迟到交接 + 2 次 RF underflow
> 全在同一秒；该窗口 `ivcsw_rate` 只有本腿基线的 1/30，而机器是闲的 2.68/14 核）—— 与 p182_1726 同类，不带进任何结论。

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

## 4. macOS 的 "RT" 到底是什么（四层机制，**实测只有一层曾经生效，而它被我们自己关掉了**）

| 层 | 机制 | 现状（2026-10-01 实测，开发文档 10.5）|
|---|---|---|
| 1 | **QoS 类**（`pthread_set_qos_class_self_np`）| **能设、能生效、也能读回** —— 但**紧跟着的 `pthread_setschedparam` 会把它清成 `UNSPECIFIED`，且此后 `set_qos` 返回 EPERM（不可恢复）**。⇒ 今天**没有一条实时线程带着档**；只有不调用 POSIX 的线程（`io_timer`、`io_broker`…）留着 `USER_INITIATED` |
| 2 | Mach affinity tag（`THREAD_AFFINITY_POLICY`）| **Apple Silicon 不支持**（`KERN_NOT_SUPPORTED`）⇒ 本机**空操作** |
| 3 | POSIX `pthread_setschedparam(SCHED_FIFO, prio)` | 无特权**成功并读回**（`policy=4`、`prio=46`），内核**只记录**；★ 它的**副作用**是把第 1 层抹掉 —— 这就是 2026-09-01 那次"改了运行语义"的真正内容 |
| 4 | **Mach 时间约束**（`THREAD_TIME_CONSTRAINT_POLICY`）| 实现存在、**生产未用**；实测与 QoS **互斥**：`thread_policy_set` 成功（rc=0）但**同样把 QoS 档清 0**，且此后 `pthread_setschedparam(SCHED_FIFO)` 返回 **EINVAL** |

**一句话规则**：Darwin 上线程**要么由 QoS 管、要么是显式调度**（POSIX 参数 / Mach 时间约束），**不能两者兼有**；
显式调度那一侧会把 QoS 档**静默清掉**。我们过去**两条路都走**，先设档再抹档 ⇒ 净效果 = **无档**。
⇒ `taskinfo` 的 `UI 0.000 / IN 0.000 / ceiling THREAD_QOS_LEGACY` 由此解释完毕；
观察到的 P 核 99.55% 来自**进程级 boost/donation**（§2.2 的 `req other=boosted`），**不是**我们的 per-thread QoS。

⇒ **可选修法（需用户裁决，§10）**：`OCUDU_SCHED_SKIP_POSIX_RT=1` 跳过 POSIX 调用，让档活下来；
同一条腿里已验证两臂读数（`eff=UNSPECIFIED` vs `eff=USER_INTERACTIVE`）。P3 的 attr-QoS **单独做没有意义**（同样被抹）。

---

## 5. 目标与判据（**先写死**）

### 5.1 目标（四条）
1. **可测**：每条关键线程都能给出**它自己**的 CPU/调度读数（不是进程级）；
2. **可归因**：任一 max 尖峰能落到"**哪条线程 + 哪一类原因**（CPU 被抢 / 等 IO / 等同步 / 等 GPU）"；
3. **可设置**：能对关键线程施加并**回读**优先级/QoS/时间约束，且**有档位对照**；
4. **Linux/Ubuntu 逐字不变**：见 §8。

> ⚠ macOS 上**不承诺**"达到 Linux 的确定性"；本线的判据写成"**尾部率/最坏值 ≤ 登记阈值**"，并**同时记录 Linux 的同读数**作对照。

### 5.2 判据（阈值**从分布导出、飞行前登记**，不在这里拍）

★ **规则在 2026-10-01 被它自己的第一次运行推翻并已重写**（开发文档 10.4）：原来打算用 `max ≤ k×中位`，
但实测同家族 20 条腿（10 个 commit、无调度改动）里 `ce` 的 per-leg `max` 摆动 **×24**，而 per-leg `p99` 只在
**146…173 µs** 之间。⇒ `max` 是重尾的一次抽样，**不能承载判据**；`p99` 是总体的性质，**可以**。

**登记的判据（每条腿，按 regime 分别判；出处 = `wip/threshold_candidates.py`，先登记再飞，不追溯）**

| 判据 | 形式 | 说明 |
|---|---|---|
| **C1 总体没动** | `p99 ≤ 登记值` | 每个序列一条（下表）；`p99` 由探针直接打印 |
| **C2 活儿没变慢** | `median ≤ 登记值` | 每条载体一条；C1 与 C2 失败方式不同，必须都过 |
| `max` | **读数，不判** | 由 `OCUDU_UL_TIMING_EVENTS` 的最慢 K 条回答"何时/哪条线程/线程自己有没有 CPU" |
| 反例（Linux） | 同读数在 Ubuntu 上**不得变化**；台架编译 + `ctest` 必须绿 | §8 |

**C1/C2 登记值**（`stress` 家族 = 最近 **7** 条 n78/gpu/rx1 腿：p169/p176/p177/p178/p179/p180/p182；
`default` 家族当前只有 **1** 条（p181），所以它的登记值是**单腿候选**、等 ≥5 条再重导）

| 序列 | 归属线程 | C1：`p99 ≤` | C2：`median ≤` | 7 条腿 p99 的中位 / 最坏 |
|---|---|---|---|---|
| `ul_rx_wait` | `lower_phy_rx#0` | **220 µs** | **43 µs** | 174.0 / 175.0 |
| `ul_rx_wait_hop` | `lower_phy_rx#0` | **180 µs** | **42 µs** | 137.0 / 140.0 |
| `ul_time_frequency` | `lower_phy_ul#0` | **780 µs** | **640 µs** | 619.9 / 622.5 |
| **`ul_channel_estimation`** | `main_pool#N` | **210 µs** | **84 µs** | 162.9 / 166.1 |
| `ul_equalization_demod` | `main_pool#N` | **1130 µs** | **850 µs** | 898.3 / 913.6 |
| `ul_ldpc_decode` | `main_pool#N` | **150 µs** | **58 µs** | 115.0 / 148.0 |
| `ul_gpu_pipeline` | 整跳（融合车道）| **2000 µs** | **1600 µs** | 1527.4 / 1565.9 |
| `ul_pipeline` | 整跳 | **2000 µs** | **1700 µs** | 1599.0 / 1618.0 |
| `dl_tx_call`（排除 call #1 启动项）| `lower_phy_tx#0` | 走 `[dl_tx_slack]` 的 `AT/BELOW 0` 与 floor（**不给 max 立判据**）| — | — |

* **登记值怎么来的**：`ceil_nice(1.25 × 最近 N 条腿 per-leg p99 的中位)`（`ceil_nice` = 向上取整到可读刻度：
  <100 µs 取 1、<1 ms 取 10、否则取 100）。**出处**：`python3 doc_chinese/macos_thread_priority/wip/threshold_candidates.py --legs 7`。
* **等价性**：同一族 `default`（p181 单腿）的 C1 与上表相差 ≤ 10 µs（ce 220、t2f 780、ldpc 270），
  因为两族的 p99 本来就落在同一带里 —— 但**在 default 家族攒够 5 条腿之前，stress 表是唯一的登记值**。
### 5.3 ★ 运行稳定性 = **同一次运行内**统计量的可重复性（用户 2026-10-01 定义，**已按此更正**）

> 用户定义（两次给全）：**"PHY 处理线程在运行相同的任务时，所需时间应该变化不大"**；
> ★ 补充更正：**不同的运行之间本来就可能变**（无线环境、业务流量类型不同），
> **所以判据是"同一次运行的统计中，mean/median/min/max/p95/p99 变化不大"**。

**⇒ 上一版把这条判据登记成"跨腿离散"是错的层次**（跨腿差异是环境造成的，不是不稳定）。已更正为：

**判据 C3（同一次运行内，前瞻、不追溯）**：一条腿的报告里，**每条序列被切成 K 个等样本窗口**（按**记录顺序**），
每个窗口的 `median`/`p95` 与**整腿值**的偏离，不得超过**登记值**（下表；K 由 `OCUDU_UL_STABILITY_WINDOWS=K` 指定）。

**工具（零新腿成本、热路径零成本）**：`OCUDU_UL_STABILITY_WINDOWS=K` —— 探针把**已经保存的样本向量**
（本就按时间顺序追加）在**报告期**重新切窗，逐序列打印每个窗口的 median/p95 与**最坏偏离**：

```
[ul_stability] OCUDU_UL_STABILITY_WINDOWS=8: … a stable run repeats its own statistics
  ul_gpu_pipeline        n=29533    median[ 1258.1 1261.0 1262.4 … ] p95[ … ]   worst window vs whole run: 0.9%
```
* **关着（不设或 `=1`）一个字都不打印** ⇒ 既有报告逐字节不变；
* 只有**新腿**带它才有这张表（老腿的报告里没有这行，见"不追溯"）；
* **反向臂已实测**：把实现改成"先排序再切片"，单测里**交替快慢**的输入立刻变红 —— 因为排序会把"每条窗口都像整腿"
  变成"前四窗快、后四窗慢"，即**给一条没有漂移的运行报出 100% 漂移**（开发文档 §10.20）。

**登记值（★ 2026-10-01 重新登记；K=8；样本不足 8×8 的序列不切并明说）**

★★ **更正**：下面这组数在**第一次登记时是"猜"的** —— 那时 `OCUDU_UL_STABILITY_WINDOWS` 还不存在，
我拿**跨腿离散**类比出来的（gpu/ul_pipeline ≤2%、t2f/eqd ≤5%、ce ≤15%、rx_wait ≤5%）。
**首对腿（p189/p190）把这个猜测证伪了一条**：`ul_pipeline` 读到 **2.5%**（登记 ≤2%）。
现按**首对腿的同一次运行实测值**重新登记（前瞻，只对之后飞的腿生效），首次读数与出处一并写死：

| 序列 | 首对腿实测（p189 / p190）| **登记带** |
|---|---|---|
| `ul_time_frequency` | 0.5% / 0.5% | **≤5%** |
| `ul_gpu_pipeline` | 0.9% / 1.3% | **≤5%** |
| `ul_pipeline` | **2.5%** / 1.4% | **≤5%**（原登记 ≤2% 即被这对腿证伪）|
| `ul_equalization_demod` | 1.5% / 1.2% | **≤5%** |
| `ul_channel_estimation` | 5.0% / **11.9%** | **≤15%** |
| `ul_rx_wait` | 0.0% / 0.0%（p95 恒 158–159）| **≤5%** |
| `dl_tx_call` | 未测（老腿无此视图）| 暂不判，等 ≥2 对腿 |
| `ul_ldpc_decode` | **41.2%** / 20.0% | **不判**（随载荷尺寸变化，不是稳定性载体）|

**为什么带宽取 5%/15% 而不是卡在实测值上**：稳定性判据要抓的是**运行内的漂移**
（热降频、某条线程逐渐丢预算），那会是几十 %；而**同一次运行内载荷本身也会变**
（p189 的 `ldpc` 窗口 median 58→69→36 就是业务量变了），带宽必须容得下工作量的正常起伏。
当前的实测值离登记带还有 2–4× 余量，同时仍能一眼抓住 ldpc 那种 20–41% 的漂移。

**配套读数（保留，但降级为"背景"，不承载判据）**：跨腿的统计量离散（`spike_census.py --stability`）——
它回答的是"换个环境/换种流量，这套处理还在原来的地方吗"，**不是**本线的稳定性判据。

**已登记的 VALIDITY 绑定（另有）**，2026-10-01 用户裁决后**重新登记（**前瞻，不追溯**）：
`AT/BELOW 0 <= 10`（**不变**）**且** `slip over 1ms <= 22`、`recv over 1ms <= 19`（原为 10/10）。
出处 = 当前家族 23 条腿中**最近 9 条**的最坏值（slip 22 / recv 19，被观察腿除外）；推导表在开发文档 §10.16(1)，
判定行与注释在 `leg_gate.sh`。旧界限 10 恰好压在平台自己的众数上（健康簇 10..12）。

* **同族定义**（写进工具里，防止再次混样）：`pipeline mode` + `cell config` + **接收策略**（`rx1` = 每符号一块，
  `rxslot` = 整槽一块，旧腿从 `[ul_rx_pool]` 的措辞读出）。**不同族不可比**：混样的第一次运行曾把 5 MHz n1 腿
  和 whole-slot 腿算进来，得出的 `ul_pipeline` "中位 max" 是 4947 µs，而当前腿自己的中位才 ~1300 µs。

---

## 6. 三层测量（由内到外）—— **三层都已实现**

1. **线程级（新，已实现）**：`ocudu::this_thread_sched_snapshot()` —— 关键线程读**自己**的 CPU / 运行态 / 请求档 vs 实际档。
   macOS：`THREAD_BASIC_INFO` + `pthread_get_qos_class_np()`；Linux：`RUSAGE_THREAD`。**平台上给不出的字段是 `-1`，不是 0**
   （macOS 上**没有** per-thread 切换计数 —— SDK 已核；Linux 上**没有** QoS 概念）。
2. **事件级（已扩展 `OCUDU_UL_TIMING_EVENTS`）**：最慢 K 条现在带 **`thread=<名字>#<id>`**、
   **`tcpu=<本线程窗口内 CPU>`**（与进程级 `cpu=` 并列）、**`ivcsw_rate=`**（切换/ms）+ 报告头一条 `leg :` 基线速率；
   **`ce`/`ldpc`/`t2f`/`eqdem` 四条相位序列的尾部接进同一机制**（每序列一个 floor）。判读写在报告头：
   `cpu=12.00ms tcpu=0.00ms` = 这条线程丢核；`cpu=0.00ms` = 整个进程没跑；两者 ≈ `win` = 线程一直在跑（活儿/IO 慢）。
3. **平台旁观（零代码，已固化成腿配方）**：`wip/observe_threads.sh`（**默认 light**；`--heavy` 才 taskinfo/powermetrics/sample，
   只能在 `-diag` 腿上跑 —— §2.4）+ `wip/taskpolicy_ab.sh`（P2 档位 A/B）。另有 `wip/spike_census.py`（尖峰普查，
   现已读 p99）与 `wip/threshold_candidates.py`（阈值导出，按家族过滤）。

---

## 7. 阶段计划（P0–P4）—— 进度已更新

| 阶段 | 内容 | 状态（2026-10-01 晚）|
|---|---|---|
| **P0** | 旁观配方 + 尖峰清单 + 阈值候选 | ✅ **完成**：三个脚本（`observe_threads.sh`/`spike_census.py`/`threshold_candidates.py`）；阈值规则被第一次运行推翻 → 改成 C1/C2 并登记进 §5.2（开发文档 10.4）。**未跑腿**（脚本不需要腿）|
| **P1** | 线程级读数 + 事件加线程名/线程 CPU + `ivcsw` 归一化 + 相位尾部 + **`[sched]` 自读** | ✅ **完成**：代码 + 单测（含反向臂）+ **两对真腿**（p183 发现并修掉一个热路径 bug，p185/p186 是修复后的验收对）。★ 悬案已答（§4，开发文档 10.5）；真腿上 `[sched]` 逐字复现。**已知缺口**：相位事件的 `cpu=`/`tcpu=` 因基线时序多为 `-`（开发文档 10.15(3)），修法已设计，与下一次改动合并重飞 |
| **P2** | **零代码 A/B**：`taskpolicy -l/-t` 档位 | 🟡 **配方与预登记已写好**（`wip/taskpolicy_ab.sh`，开发文档 10.7），**等腿**（臂，不是验收证据）|
| **P3** | attr-QoS 接到线程创建 | 🟡 **已接线但默认关**（`OCUDU_SCHED_ATTR_QOS=1`）。★ 实测：**单独它没有用**（POSIX 调用同样会把它抹掉）⇒ 必须与 P1 的 `OCUDU_SCHED_SKIP_POSIX_RT=1` 一起做（开发文档 10.5）|
| **P4** | Mach time constraint | ⛔ **建议暂不施用**：① 参数标定缺乏每线程计算量分布（开发文档 10.8）；② 实测它**与 QoS 互斥**且会把 `pthread_setschedparam` 变成 EINVAL ⇒ 现在施用等于**再次关掉 QoS**。列为最后选项，需用户裁决 |

⚠ **成本（已支付一次）**：本次改动动了 `lib/`+`utils/`+`include/` ⇒ **`p181`/`p182` 按审计规则过期**，
必须重飞一对腿（default + stress）才能恢复"当前 HEAD 的 GREEN 证据"。

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

## 10. 待用户裁决 / 待办（**2026-10-01 晚重写：前两条已经做完，这里是新的两条**）

| # | 事项 | 建议 |
|---|---|---|
| 1 | `OCUDU_SCHED_SKIP_POSIX_RT`（让 QoS 档活下来）| ✅ **已裁决（2026-10-01）：立为新默认**。开关反转为 `OCUDU_SCHED_POSIX_RT`（`=1` = 恢复历史行为，作为对照臂保留在同一二进制上）。两条臂都在 loopback 上复验（开发文档 §10.16(2)）|
| 2 | ★ **重飞哪些腿**（动了 `lib/`+`utils/`+`include/` ⇒ `p181`/`p182` 已过期）| 建议顺序：① 一对**验收腿**（`default` + `stress`，当前 HEAD，**不设**新开关）恢复 GREEN；② 一对**读档腿**（同上但带 `OCUDU_SCHED_VERBOSE=1`，读每线程 `eff=`）；③ **P2 档位 A/B 腿**（stress + `taskpolicy`，开发文档 10.7 的预登记）；④ 可选：B 臂（`OCUDU_SCHED_SKIP_POSIX_RT=1`）一对 |
| 3 | Ubuntu 台架 | ✅ 已按你给的 `ssh jwang@192.168.0.106` 免密使用（见开发文档 10.10/10.16 的 Linux 复核）|
| 7 | ~~`ul_ldpc_decode` 按尺寸分层~~ | ❌ **用户 2026-10-01 否决：不属于本线**。该序列的尾部**跟着载荷走** ⇒ 它**不是稳定性的载体**；判据标定属于管判据的那条线。本线只保留读数与结论"该序列承载载荷、不承载稳定性"（开发文档 §10.18）|
| 8 | ★ **对照臂一对腿**（`OCUDU_SCHED_POSIX_RT=1`，同配方同二进制）| **建议做**：新默认让**非自愿切换率从 194.4/ms 降到 179.3/ms（−7.8%，组内一致、且载荷更大）**，但 n=2/臂 ⇒ 只有对照臂能把它从**假设**变成**结论**（开发文档 §10.18）|
| 6 | `slip/recv over 1ms <= 10` 这条绑定 | ✅ **已裁决（2026-10-01）：前瞻性重登记为 22/19**，只对之后飞的腿生效；`AT/BELOW 0 <= 10` 不变。落地在 `leg_gate.sh` + §5.2（开发文档 §10.16(1)）|
| 4 | P4（Mach time constraint）| 维持**最后选项**；实测它与 QoS 互斥（开发文档 10.5/10.8）⇒ 现在施用等于再次关掉 QoS |
| 5 | 旧的悬案条目（QoS 自读、P1 是否动手、档位怎么选）| ✅ 已闭环：自读做了且给了答案；P1 代码已落地；档位扫描做成 `wip/taskpolicy_ab.sh scan` |

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

**树级文档**：`../ocudu_env_knobs_inventory_and_leg_whitelist.md`（生成物：全部 `OCUDU_*` 旋钮的默认值/读取点/腿 + 验收腿白名单）—— 本线新增的任何旋钮都要在那里出现。
