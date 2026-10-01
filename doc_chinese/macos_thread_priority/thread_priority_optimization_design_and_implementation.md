# macOS PHY 线程运行稳定性 —— **设计与实施**（追加式）

> **性质**：追加式开发记录。**不改历史**；更正写在新的小节里并注明更正了哪一条。
> **上层**：`thread_running_high_level_status_and_plan.md`（高层，活文档）。**分工**见 `README.md`。
> **来源**：本线从 `../phy_latency/` 的 §6.248（ms 级停顿收口为欠账）与 §6.249（macOS 测量规划）延伸出来。

---

## 0. 范围与不变量（**任何改动都必须满足**）

**范围**：让 macOS 上 PHY 流水线关键线程的**运行稳定性可测、可归因、可设置**；必要时调整 macOS 侧的调度设置。

**不变量（违反即回退）**
1. **Linux/Ubuntu 逐字不变**：`#if defined(__APPLE__)` 之外零行为改动；`#else` 分支只写 no-op；Linux 的调度参数（`radio_worker_realtime_priority()` 的返回值、池优先级数值、亲和性、`SCHED_FIFO` 用法）**一个字节不动**。
2. **两把钥匙**：所有新仪器 = **编译期开关 + env**，默认关；**关着时报告逐字节不变**（沿用 `../phy_latency` 的探针契约，§6.145③）。
3. **不改交付判据**：本线不新增/不修改验收判据（V1–V4 与空口门保持原样）；新读数一律先作**读数**，要成为判据必须**先登记再飞**（`../phy_latency` §5.2 第 2 条）。
4. **不重开 Q27**：ms 级停顿仍按 `../phy_latency` §6.248 结案存档；本线是"若要重开，从这里开始"。

---

## 1. 代码地图（要碰的文件与插入点）

| 文件 | 现状（2026-10-01）| 本线要用它做什么 |
|---|---|---|
| `apps/services/worker_manager/worker_manager.cpp` | 线程优先级实参全在这里：main pool `max()−2`（`create_main_worker_pool`）、`radio`（`create_lower_phy_executors`，用 `compat::radio_worker_realtime_priority()`）、triple profile 的 `lower_phy_tx#0`=`max()` / `lower_phy_rx#0`=`max()−1` / `lower_phy_ul#0`=`max()−1`、`ru_timing`=`max()−0` | **只读参照**：优先级数值不动（P3/P4 若动，也只动 macOS 分支）|
| `lib/support/executors/unique_thread.cpp` | `unique_thread`：起线程 → `compat::apply_worker_thread_scheduling(prio, cpu_mask, name)` → 再尝试 `pthread_setschedparam` | P1 的 `[sched]` 自读挂在这里（线程内、设置之后）|
| `utils/macos_compat/macos_compat.cpp` | `apply_worker_thread_scheduling()`（QoS + affinity tag）、`bind_thread_to_performance_core()`（QoS + **Mach time constraint**，**仅单测调用**）、`posix_realtime_priority_is_enforceable()`（macOS 返回 true）、`radio_worker_realtime_priority()` | **P1 自读**、**P3 attr-QoS**、**P4 时间约束**的主战场（macOS 专属实现都在这层）|
| `lib/support/scheduling/darwin_thread_scheduling.{h,cpp}` | `darwin_qos_class_for_prio()`、`set_this_thread_qos_class()`、`set_pthread_attr_qos_class()`（**已实现、生产未用**）、`set_this_thread_time_constraint()`、`set_this_thread_affinity_tag()`（Intel only）| P1 读回实际档位；P3 接 attr；P4 调约束参数 |
| `include/ocudu/support/executors/ul_pipeline_probe.h` | `OCUDU_UL_TIMING_EVENTS` 的最慢 K 条：`timing_event{value_us, begin/end_ns, wall_ms, air_us, due_ts, load1_x100, cpu_ns, ivcsw, nvcsw, base_age_us, win_us}`、`late_baselines` 计数器、`print_timing_events()` | P1：加**线程 id/名字**与**线程级 CPU**；`ivcsw` 归一化；把 `ce`/`ldpc`/`t2f` 的尾部接进同一机制 |
| `lib/phy/lower/lower_phy_baseband_processor.cpp` | 收包循环 `rx_executor.defer(ul_process)`；`uplink_executor.defer(... uplink_processor.process)`；`record_rx_wait()` 的调用点（CPU 基线与窗口两端） | P1：在**已知线程身份**的位置采集线程级读数（UL 线程上的 t2f 事件、RX 线程上的 wait 事件）|
| `lib/phy/upper/uplink_processor_impl.cpp` | `task_executors.pusch_executor.defer(...)`（PUSCH 处理派到池上）| P1：`ce`/`ldpc` 事件的线程身份 |
| `lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp` | `record_ce_end()` | 同上（`ce` 的归属）|
| `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm` | `metal::shared_burst::commit()`（**提交 GPU 的那一句**）| P1：若要给"提交"打时间戳，这里是唯一入口 |
| `tests/unittests/support/macos_compat_test.cpp` | `apply_worker_thread_scheduling` 的 smoke 用例、`bind_thread_to_performance_core()` 用例 | P1/P3/P4 的**双向单测**放这里（Linux 上断言 no-op）|

---

## 2. 阶段 P0 —— 旁观配方 + 尖峰清单 + 阈值候选（**零代码**）

### 2.1 交付物 1：`wip/observe_threads.sh`
一个脚本，腿跑着的时候在**开始 / 中段 / 结束**各调用一次，产出三个文件到 `work_tmp/obs_<label>_<phase>.*`：

```
usage: bash wip/observe_threads.sh <leg-label> <phase>      # phase ∈ start|mid|end
产物：
  work_tmp/obs_<label>_<phase>.taskinfo.txt      # sudo taskinfo <pid>
  work_tmp/obs_<label>_<phase>.powermetrics.txt  # sudo powermetrics --samplers tasks --show-process-qos-tiers \
                                                 #      --show-process-wait-times --show-process-amp -n 3
  work_tmp/obs_<label>_<phase>.sample.txt        # sample <pid> 3 -file …
  work_tmp/obs_<label>_<phase>.psM.txt           # ps -M <pid>（每线程 CPU 时间）
```
约定：`pid=$(pgrep -f 'build/apps/gnb/gnb' | head -1)`；缺 `sudo` 时打印一行 `SKIPPED (needs root)` 而**不是**静默跳过（"没测"必须可见）。

### 2.2 交付物 2：`wip/spike_census.py`（尖峰清单）
从 `wip/logs/*.log.stderr`（新腿）+ `../phy_pipeline_gpu/wip/logs/*.log.stderr`（历史）提取**每条序列**的 `samples/mean/median/min/max/p95/p99`，输出一张表：**序列 × 腿**，并标出 `max/median` 比值最大的前 N 条。
目的：**证明/否证"尖峰是全局形状"**，并给 §2.3 的阈值候选一个**分布依据**。

### 2.3 交付物 3：阈值候选（写回高层 §5.2）
规则（**先写死规则，再看数**）：对每条序列取最近 **N ≥ 10** 条腿的 `max` 与 `p99`，阈值候选 = **最近 N 条腿的 p99.9**（或"max ≤ 2× 该序列历史中位 max"），并把**出处（哪条腿、哪个文件）**记进开发文档。
⚠ 目的不是"设一个能过的阈值"，而是**把尾部从"未知"变成"有档位对照"**；阈值只在**新腿**上生效（不追溯）。

---

## 3. 阶段 P1 —— 线程级读数 + `[sched]` 自读（**动代码，一次改动合并**）

### 3.1 `[sched]` 启动自读（回答悬案）
在 `apply_worker_thread_scheduling()` 里 `set_this_thread_qos_class()` 之后加（**env 门控，默认关**）：

```cpp
// utils/macos_compat/macos_compat.cpp（macOS 分支内）
if (const char* v = std::getenv("OCUDU_SCHED_VERBOSE"); v != nullptr && v[0] != '0') {
  qos_class_t got = QOS_CLASS_UNSPECIFIED; int rel = 0;
  ::pthread_get_qos_class_np(::pthread_self(), &got, &rel);
  int pol = -1; sched_param sp{};
  ::pthread_getschedparam(::pthread_self(), &pol, &sp);
  std::fprintf(stderr,
               "[sched] thread=%s requested=%s effective=%s rel=%d posix_policy=%d posix_prio=%d rt_intent=%d\n",
               std::string(thread_name).c_str(), qos_name(darwin_qos_class_for_prio(prio)), qos_name(got), rel, pol,
               sp.sched_priority, int(prio != os_thread_realtime_priority::no_realtime()));
}
```
* Linux 侧该函数是 no-op ⇒ **不打印**（或在 `#else` 里打印 `[sched] linux-unchanged` 之类**仅在 env 开时**；默认关，二者都可）；
* 判据：**loopback 6 秒**就能读出"请求档 vs 实际档"；若 `effective != requested` ⇒ **§4 的悬案成立（被钳）**，P3 才有依据。

### 3.2 线程级调度快照（新接口，两平台同形）
```cpp
// lib/support/scheduling/thread_sched_snapshot.h（新文件；两平台同接口）
struct thread_sched_snapshot {
  int64_t  cpu_ns    = -1;   // 本线程累计 CPU
  uint64_t ivcsw     = 0;    // 本线程非自愿切换
  uint64_t nvcsw     = 0;    // 本线程自愿切换
  uint32_t run_state = 0;    // macOS: THREAD_BASIC_INFO.run_state（Linux: 0）
  int32_t  qos_class = -1;   // macOS: pthread_get_qos_class_np（Linux: -1）
  int32_t  posix_prio = -1;
};
thread_sched_snapshot this_thread_sched_snapshot();   // macOS: thread_info(THREAD_BASIC_INFO) + pthread_get_qos_class_np
                                                     // Linux : RUSAGE_THREAD + pthread_getschedparam
```
* **只在事件被保留时调用**（与现有 `cpu_base` 同策略）⇒ 热路径成本为零；
* 单测：`tests/unittests/support/…`（Linux 上断言字段可读但值为 no-op 语义）。

### 3.3 `OCUDU_UL_TIMING_EVENTS` 的字段扩展
| 字段 | 含义 | 采集点 |
|---|---|---|
| `thread=` | 线程名（`pthread_getname_np`）+ 线程 id（`pthread_threadid_np`）| 事件被保留时 |
| `tcpu=` | **本线程**在该窗口的 CPU 增量（§3.2 的快照差分）| 同上（与现有进程级 `cpu=` 并列）|
| `ivcsw_rate=` | **归一化**：`ivcsw / win_ms`，并给本腿中位 | 打印时（中位数由探针维护）|
| `ce/ldpc/t2f` 尾部 | 把这三条序列的**最慢 K 条**纳入同一机制（它们现在只有聚合值）| 各自 `record_*` 的调用点（线程身份已知）|

### 3.4 判据与反向臂（**每个新字段都要能被反向臂打红**）
沿用 `../phy_latency` 的做法（§6.241/§6.245）：
* 开着 ⇒ 字段有真实值（`thread=` 非空、`tcpu=` 为正且 ≤ 窗口+slack、`ivcsw_rate=` 与 `ivcsw/win` 自洽）；
* 关着 ⇒ **报告逐字节不变**（`git diff` 一份基线与一份开着/关着的 stderr 对比）；
* 反向臂（至少 3 条）：①基线不采集 ⇒ 字段为 `-`；②线程差分拿错线程 ⇒ 断言红；③`ivcsw_rate` 不归一化 ⇒ 断言红。

---

## 4. 阶段 P2 —— `taskpolicy` 档位零代码 A/B

**配方**（同一二进制，只改运行中的 task 档）：
```bash
# A 臂（默认档）
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 \
  OCUDU_UL_TIMING_EVENTS=16 sudo -E bash ../phy_pipeline_gpu/wip/run_leg.sh gpu <label-A> --regime=stress
# B 臂（腿跑起来后，在另一个终端改档；taskpolicy 可对运行中的进程生效）
sudo taskpolicy -l <tier> -t <tier> -p $(pgrep -f 'build/apps/gnb/gnb')
sudo taskinfo $(pgrep -f 'build/apps/gnb/gnb') | grep -E 'qos|latency|thruput'   # 回读
```
* **档位先扫一遍**（`LATENCY_QOS_TIER_0..5` 与 throughput 档），确认哪些档能被接受（回读为准）；
* **判据**：把**尾部率**（`max`、`p99.9`、`over 1ms` 计数，见高层 §5.2）与档位对照；**跟档位走 ⇒ 机制在宿主调度层**；不跟 ⇒ 指向电台/USB；
* **不追溯**：A/B 的结论只用于**下一次**预登记。

---

## 5. 阶段 P3 —— attr-QoS 接线（若 P1 表明被钳 / P2 表明档位有效但不足）

* 现状：`set_pthread_attr_qos_class()` **已实现但生产未用**（代码注释自己写了它的价值：*"declare the class on the attributes so the thread starts on the performance cores from its first instruction"*）。
* 做法：在 `unique_thread` 的线程创建路径上（`pthread_attr_t` 已存在）调用它；**仅 macOS**（`#if defined(__APPLE__)`），Linux 不动。
* 判据：`[sched]` 自读显示线程**从一开始**就是目标档；尾部不劣（同 §5.2 载体）。
* 风险：只影响"起跑瞬间"的核选择，**不改变**任何 Linux 行为；A/B 仍按 P2 的方式做。

---

## 6. 阶段 P4 —— Mach time constraint（**最后选项**）

* 现状：`set_this_thread_time_constraint()` 已实现；`bind_thread_to_performance_core()` 只在单测里被调用；2026-09-01 曾自动施用，**与一次 OAI-UE 随机接入回归同时出现**（`macos_compat.cpp:285-305` 的注释原文）。
* **参数标定（离线）**：用现有分布给候选值 —— `period` = 一跳/一槽的期望周期（如 n78 SLOT 500 µs）、`computation` = 该线程 p99.9（如 `lower_phy_ul#0` 的 t2f p99.9）、`constraint` = computation + 余量（如 +50%）、`preemptible` = true；**标定报告**写进开发文档 §9。
* **判据与回退（先登记）**：单腿 A/B；出现以下任一 ⇒ **立即回退**：`gaps>0`、`rx_overflows>0`、契约红、`[RF]` 率进入病态带、手机随机接入异常。
* **不做**：不把它设为默认（保持 opt-in），不改 Linux。

---

## 7. 测试与验证

| 层 | 内容 |
|---|---|
| 单测（两平台）| `macos_compat_test` 增：`[sched]` 自读（env 开/关）、线程级快照字段自洽、attr-QoS 生效性（macOS 断言档位；Linux 断言 no-op）|
| 反向臂 | 每个新字段 ≥1 条（见 §3.4）；每条都要能**打红**（沿用 `../phy_latency` 的纪律 79）|
| 逐字节不变 | 开着/关着各跑一次 loopback（6 s），`diff` 报告（除新增行外**零差异**）|
| 回归 | `ctest -L phy -j 1` 全绿（含 `ul_pipeline_probe_test`）|
| **Linux 复核** | 同一 commit 在 **`jwang@192.168.0.106:~/work/ocudu`** 上：`cmake --build build --target ul_pipeline_probe_test macos_compat_test`（或等价目标）+ `ctest`；结果记进 §10 的"Linux 复核"小节 |

---

## 8. 判据登记模板（**飞腿前**填）

```
### 预登记 <日期> / <腿标签>
* 目的：
* 改动（commit）：
* 对照臂 / 变量：
* 判据（阈值 + 出处）：
* 反例判据（Linux 不得变化；不劣于哪条腿）：
* 回退条件：
* 读数位置（stderr/log/taskinfo/…）：
```

---

## 9. 参数标定记录（P4 用；先空着，标定时填）

| 线程 | 期望周期 | p99.9（出处腿）| 候选 computation | 候选 constraint | 备注 |
|---|---|---|---|---|---|
| `lower_phy_ul#0` | 500 µs（n78 槽）| 待填（`[ul_time_frequency]` p99.9）| | | |
| `lower_phy_rx#0` | 35.7 µs（符号）| 待填（`[ul_rx_wait]` p99.9）| | | |
| `main_pool#N` | 500 µs | 待填（`ce`/`ldpc` p99.9）| | | |

---

## 10. 变更记录（**追加式**）

### 10.1 2026-10-01 —— 规划落地（本文件建立）
* 建立本目录与三份文档（用户指示）；高层、设计、交接各一份；`wip/`（脚本，跟踪）+ `wip/logs/`（腿，忽略）+ `work_tmp/`（忽略）。
* 完成 **P0 的定义**（旁观配方、尖峰清单、阈值候选规则）与 **P1–P4 的设计**（接口、插入点、判据、反向臂、回退条件）。
* 记录三条关键事实（来源见高层 §2）：尖峰**跨序列跨线程**（`ce` max ≈ 20× 中位）；进程 **1.92 核 / P 核 99.55%**；**`taskinfo` 的 UI/IN 计费为 0、天花板 `THREAD_QOS_LEGACY`** ⇒ "QoS 是否生效"**尚无读数**（P1 要回答的第一个问题）。
* **Linux 复核**：（未做）等待 Ubuntu 台架 `jwang@192.168.0.106:~/work/ocudu` 的第一次编译/测试。

### 10.3 2026-10-01 —— ★★ **观察者会扰动被观察者**（P0 的配方因此改写）

**读数**（leg `gnb_gpu_p182-n78-stress_1001_1726.log.stderr`，戳 `c1b59d0aa5`，腿 09:26:25→09:49:36 UTC = 23 min，
用户在该腿运行 6 分钟后（17:32）在其上跑了 `sudo taskinfo` + `sudo powermetrics … -n 3` + `sample … 3`）：

| 读数 | 被观察腿 | 未被观察腿（p180 / p182-1319）|
|---|---|---|
| `[ul_rx_wait] max` | **153 135 µs** | 2049 / 2159 µs |
| `[ul_time_frequency] max` | **21 235 µs** | 686 / 721 µs |
| `[ul_channel_estimation] max` | **13 267 µs** | 1218 / 1228 µs |
| `[ul_equalization_demod] max` | **20 977 µs** | （未摘）|
| `[ul_ldpc_decode] max` | 961 µs | 455 / 444 µs |
| `[ul_gpu_pipeline] max` | 7398 µs（中位 1262.4 **不变**）| 5033 / 4916 |
| `[ul_rx_timing]` | `recv max 153 134`、`over 1ms=118`、`over 5ms=41`、`loop max 19 397` | `over 1ms` 2 / 5 |
| `[dl_tx_slack]` | **`AT/BELOW 0 = 709`**、`min = −167 679 µs` | 1 / 1、`min ≈ −250 µs` |
| `[ul_rx]` | **`1 gaps`、`1 radio receive overflow`、3 903 898 samples** | 0 / 0 |
| `load1(max at a tail event)` | **5.55** | 4.11 / 4.68 |

**结论（含边界）**：相关性很强但**不是受控实验**（只有一条腿），所以表述为"**该腿的尾部含观察代价**"；
机制上是可解释的：`sample` **挂起目标进程**走栈，`powermetrics` 周期性读取调度器状态，二者都不是无侵入工具。
另外一个独立价值：**中位数完全不动**（1262.4），**尾部大两个数量级** —— 正是本线要量化的那种"中位稳、尾部炸"的形状。

**设计后果（已落进 P0）**
1. `wip/observe_threads.sh` **默认 light**（只 `ps -M`，零侵入）；`--heavy` 才跑 `taskinfo`/`powermetrics`/`sample`，
   并**打印警告**：该腿是**诊断腿**，不得当验收证据（它可能带着观察造成的 `gaps`/`rx_overflows`）。
2. **腿配方**更新：真实腿（default/stress）只允许 light；要跑 heavy 就**另飞一条诊断腿**，并给它单独的标签（如 `<label>-diag`）。
3. 审计/门的口径不变：一条带 `gaps>0` 的腿本来就不合格（`[ul_rx]` 是判据）——这条腿**不得**进入任何验收结论。

### 10.4 待补
（每次飞腿/改动后追加：做了什么、读数、判据是否满足、更正了哪一条。）
