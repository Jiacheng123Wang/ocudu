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

### 10.4 2026-10-01 —— P0 的**阈值规则被它自己的第一次运行推翻**（更正 §2.3）

**预登记的规则**（§2.3，先写后跑）："阈值 = 最近 N 条腿里最坏的 per-leg `max` × 1.25"。
**第一次运行的读数**（`wip/threshold_candidates.py`，n78/gpu 家族，最近 20 条腿、**10 个不同 commit**、其中无任何调度改动）：

| 序列 | per-leg `p99` 的范围 | per-leg `max` 的范围 | max 的离散度 |
|---|---|---|---|
| `ul_channel_estimation` | **146.2 … 173.5 µs** | **201 … 4792 µs** | **×24** |
| `ul_time_frequency` | **605.5 … 674.6 µs** | 605 … 21235 µs | ×35（含被观察腿）|
| `ul_ldpc_decode` | 67 … 210 µs | 86 … 713 µs | ×8 |

**结论**：`max` 是**重尾里的一次抽样**，`p99` 是**总体的性质**。用 max 立阈值只有两个选择，两个都错：
把它立宽到能容下 ×24 的抖动（则真正的回归躲在它下面），或者立窄（则每次平台打嗝都要重判）。
⇒ **规则改为两条，且都要过**（高层 §5.2 已按此重写）：

* **C1（总体没动）**：`p99 ≤ ceil_nice(1.25 × 最近 N 条腿 per-leg p99 的中位)`；
* **C2（活儿没变慢）**：`median ≤ ceil_nice(1.25 × 最近 N 条腿 per-leg median 的中位)`；
* **`max` 降级为读数**，不再承载判据 —— 它的归属由 `OCUDU_UL_TIMING_EVENTS` 的最慢 K 条（P1 起带线程名与线程 CPU）回答。

两条判据**失败的方式不同**，这正是要两条的原因：整体变慢的腿过 C1 而挂 C2；只多了新尾巴的腿挂 C1 而过 C2 ——
而"线程运行稳定性"说的正是后者。

**第三条更正（种群）**：家族键必须是 **`pipeline mode` + `cell config` + 接收策略**，且**只取同家族的腿**。
第一次运行的实现**只在显式给了 `--family` 时才过滤**，于是默认运行把 5 MHz n1 腿、cpu 模式腿、whole-slot 接收腿
混在一张表里，还顶着 `family: gpu/…/rx1` 的标题 —— "标题说一件事、表说另一件事"。
修好后（`rx1` = 每符号一块，`rxslot` = 整槽一块，旧腿从 `[ul_rx_pool]` 的措辞里读出来）：
`default` 家族只剩 **1 条**腿（p181），`stress` 家族 **7 条**。**p99 的稳定性正是靠这 7 条跨 7 个 commit 的腿成立的。**

### 10.5 ★★★ 2026-10-01 —— **我们请求的 QoS 类从来没有生效过：把它抹掉的正是我们自己的 POSIX 调用**（回答 §3.1 的悬案，更正 §1/§3.1 的插入点与 `utils/macos_compat` 的旧注释）

**这是本线开线的那个问题**（高层 §4：`taskinfo` 显示 UI/IN 计费 0 s、天花板 `THREAD_QOS_LEGACY`，
"请求的档到底生效没有"没有任何读数）。P1 的 `[sched]` 自读（`OCUDU_SCHED_VERBOSE=1`）**第一次运行就给出了答案**。

**读数 1：真实进程**（本机 loopback 台架，两条臂同一个二进制，只差一个环境变量）

| 臂 | 代表性输出 |
|---|---|
| 默认（历史行为） | `[sched] thread=main_pool#0 id=… rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED run=running posix=FIFO/44` |
| `OCUDU_SCHED_SKIP_POSIX_RT=1` | `[sched] thread=main_pool#0 id=… rt_intent=1 req=USER_INTERACTIVE eff=USER_INTERACTIVE run=running posix=OTHER/31` |

同一份 stderr 里还有一条**自洽性证据**：`io_timer_tick`（`rt_intent=0`，**从不调用** `pthread_setschedparam`）
回读 `req=USER_INITIATED eff=USER_INITIATED` —— 即"调用过的那条路丢了档，没调用的那条路留着档"。
⇒ 今天的真实状态是：**数据面线程没有任何 QoS 档**（`UNSPECIFIED`），**比它们本该压制的 `io_timer`/`io_broker` 还低一档**。

**读数 2：机制**（微实验，`/tmp/qos_probe*.c`，本机 SDK）

```
set_qos_class_self_np(USER_INTERACTIVE)          -> 0，回读 USER_INTERACTIVE
pthread_setschedparam(SCHED_FIFO, 46)            -> 0，回读 UNSPECIFIED   ← 档被抹掉
set_qos_class_self_np(USER_INTERACTIVE) 再来一次 -> 1 (EPERM)，档仍是 UNSPECIFIED
setschedparam(SCHED_OTHER, 31) 之后再来一次      -> 1 (EPERM)：**不可恢复**
```

* **属性上声明的档同样保不住**：`pthread_attr_set_qos_class_np(USER_INTERACTIVE)` → 线程起来时回读
  `USER_INTERACTIVE`，**同一个 `pthread_setschedparam` 之后回读 `UNSPECIFIED`** ⇒ **P3 单独做没有意义**，
  必须与"跳过 POSIX 调用"一起做。
* **Mach 时间约束（P4）同样与 QoS 互斥**：`thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)` **成功**（rc=0），
  但它**也把 QoS 档清成 0**；而且此后 `pthread_setschedparam(SCHED_FIFO,46)` **返回 22 (EINVAL)**。
* 规则一句话：**Darwin 上线程要么由 QoS 管、要么是显式调度（POSIX 参数 / Mach 时间约束），不能两者兼有；
  显式调度那一侧会把 QoS 档静默清掉且不可恢复。**

**这条同时追认了两件旧事**
1. `taskinfo` 的 `UI 0.000 / IN 0.000 / ceiling THREAD_QOS_LEGACY` 不再需要猜测：**根本没有档**；
   P 核 99.55% 来自**进程级 boost/donation**（§2.2 的 `req other=boosted`、`imp_donor=CURRENTLY`），不是我们的 per-thread QoS。
2. 2026-09-01 那次"自动施用时间约束 + OAI-UE 随机接入回归"（§6/§4 层 4）：时间约束**不只是**加了一个策略，
   它**顺带把每个实时 worker 的 QoS 档清掉了** —— 当时被记为"改变了运行语义"，现在知道改的是什么。

**由谁来修（未决）**：跳过 POSIX 调用是一个**行为改变**（尽管 SCHED_FIFO 在本机只是被"记录"），
所以实现成 **A/B 开关**而不是新默认：`OCUDU_SCHED_SKIP_POSIX_RT=1`。是否把哪一臂立为默认，**需要用户裁决**（高层 §10）。

### 10.6 2026-10-01 —— P1 的实施记录（仪器、字段、判据、反向臂）

**新增/改动的文件**

| 文件 | 改动 |
|---|---|
| `include/ocudu/support/scheduling/thread_sched_snapshot.h`（**新**）| `thread_sched_snapshot{cpu_ns, nvcsw, ivcsw, wall_ns, run_state, qos_class, posix_policy, posix_prio, thread_id}`、`this_thread_sched_snapshot()`、`qos_class_name()`、`thread_run_state_name()`、`log_this_thread_scheduling()` |
| `lib/support/scheduling/thread_sched_snapshot.cpp`（**新**，进 `ocudu_support`）| macOS：`THREAD_BASIC_INFO`（user+system `time_value_t`）+ `pthread_get_qos_class_np` + `pthread_getschedparam` + `pthread_threadid_np`；Linux：`RUSAGE_THREAD` + `getpid`/`gettid` + `pthread_getschedparam` |
| `lib/support/executors/unique_thread.cpp` | 在**所有**调度调用之后调用 `log_this_thread_scheduling(prio, name)`；新增 `configure_worker_thread_attributes_qos(attr, prio)` 调用（P3）|
| `include/ocudu/support/macos_compat.h` / `utils/macos_compat/macos_compat.cpp` | `configure_worker_thread_attributes_qos()`（`OCUDU_SCHED_ATTR_QOS`，默认关，Linux 不读环境变量）；`posix_realtime_priority_is_enforceable()` 在 macOS 上受 `OCUDU_SCHED_SKIP_POSIX_RT` 影响（默认关 = 历史行为）|
| `include/ocudu/support/executors/ul_pipeline_probe.h` | `timing_event` 新增 `kind`/`thread_id`/`thread_name`/`tcpu_ns`；`cpu_snapshot` 新增线程半边与 `leg_base`；新增 4 条相位序列的 worst-K（`record_phase_timing_event`，带每序列 floor）；新增 `rate_str()` 与 `to_ns()`；报告新增 `thread=`/`tcpu=`/`ivcsw_rate=` 与 `leg :` 基线行 |
| `tests/unittests/support/{macos_compat_test.cpp,executors/ul_pipeline_probe_test.cpp}` | 新增 4 个用例（见下）|
| `doc_chinese/phy_pipeline_gpu/wip/{leg_gate.sh,milestone_audit.sh}` | 白名单加入 `OCUDU_SCHED_VERBOSE`（**只打印**）；两个**改调度**的开关**故意不在**白名单（fail-closed）|
| `doc_chinese/phy_latency/wip/gen_knob_inventory.py` | 扫描根加入 `utils/`；新增一条"复合守卫"分类规则（放在链条**最后**，见下）|

**`[sched]` 自读的两把钥匙**：`ENABLE_FLOW_PROBES`（编译）+ `OCUDU_SCHED_VERBOSE`（运行，默认关）。
两者缺一即**一个字都不打印**（两平台都是）。插入点**改在 `unique_thread` 里、所有调度调用之后**，
而不是预登记的 `apply_worker_thread_scheduling()` 内部：那个位置在 `pthread_setschedparam` **之前**，
读到的 `posix=` 是线程还没到达的状态 —— 而"请求 vs 实际"正是这台仪器要分的那件事（10.5 的读数就来自这一点）。

**`tcpu=` 的三条规则**（每条都有反向臂）
1. 只有**基线与本事件由同一条线程**记录时才给值（累计计数器只能与同线程的读数相减）；
   跨线程时打 `-`，**绝不打 0**。反向臂：`timing_events_name_the_thread_and_carry_its_own_cpu` 在**另一条线程**上
   记录事件，断言 `tcpu=-` 而 `cpu=` 仍是有效值。
2. `cpu=`（进程，`RUSAGE_SELF`）与 `tcpu=`（线程）**并列打印**，判读写在报告头：
   `cpu=12.00ms tcpu=0.00ms` = "这条线程丢了核、兄弟线程在跑"（调度问题）；`cpu=0.00ms` = "整个进程没跑"；
   两者都 ≈ `win` = "这条线程一直在跑，是活儿/IO 本身慢"。
3. `ivcsw_rate=`（每条事件的自愿/非自愿切换 ÷ 窗口 ms）+ 报告头一条 `leg :` 行给出**本腿基线速率**
   （例：loopback 20.1 s 内 7 729 356 次非自愿切换 = **384.05/ms**）—— 没有基线速率的归一化数字读不出异常。

**相位尾部（`ce`/`ldpc`/`t2f`/`eqdem`）**：这四条序列此前**只有聚合值**，而 `ce` max ≈ 20× 中位正是本线的起点现象。
现在它们进同一套 worst-K，每序列一个 floor（**先写后跑**，取自 P0 的 p99 分布：t2f 2 ms / ce 1 ms / eqdem 3 ms / ldpc 500 µs，
每个 floor 都在该序列 p99 的 ~6 倍以上，健康腿因此一条都不留）。事件带 `thread=`（**完成**该段的那条线程）
与窗口两端 `begin_ns`/`end_ns`（`phases_entry` 新增三个瞬时；`to_ns()` 把 `high_resolution_clock` 的瞬时
**重基到 steady 轴**——`high_resolution_clock` 在 libc++ 上是 `steady_clock`、在 libstdc++ 上是 `system_clock`，
不重基就会让两个平台的相位事件落在不同时间轴上）。

**测试（4 个新用例，两平台都跑）**
* `macos_compat_sched_test.thread_sched_snapshot_is_a_per_thread_reading`：CPU 单调、**上界 = 流逝的墙钟**、
  换线程换计数器；macOS 断言 `nvcsw/ivcsw == -1`（**本机没有任何 Mach thread-info flavor 带切换计数**，SDK 已核）
  且 `qos_class` 可读；Linux 断言 `qos_class == -1`、切换计数可读。
* `macos_compat_sched_test.sched_self_read_is_env_gated_and_reports_requested_vs_effective`：
  环境变量不设 ⇒ **空输出**；设了 ⇒ 一行且 `req=`/`eff=` 都在。
* `macos_compat_sched_test.attr_qos_is_opt_in_and_platform_gated`：不设变量 ⇒ 属性**不变**；
  设了 ⇒ 属性上就是 `USER_INTERACTIVE`；Linux 两臂都断言不变。
* `ul_pipeline_probe_test.timing_events_name_the_thread_and_carry_its_own_cpu` +
  `ul_pipeline_probe_test.phase_segment_tails_are_ranked_and_attributed`：见上面的反向臂；相位用例还断言
  "低于 floor 的不进列表"、"空序列要打印 candidate 计数"（"关着"与"开着但安静"不能长得一样）。

**回归**：`ctest -L phy -j 1` = **207/207 通过**（208 个用例，1 个禁用）；loopback 台上两条臂都跑过（10.5 的表）。

**一处实现修正**：`record_ldpc_start()` 在**持锁**状态下组装三个相位时长，所以相位记录分成
`record_phase_timing_event()`（自己加锁）与 `..._locked()`（调用者已持锁）两份 —— 本探针的 mutex 是普通
`std::mutex`，重入即死锁。

### 10.7 2026-10-01 —— P2 的腿配方与**预登记**（判据先写死，再飞）

**脚本**：`wip/taskpolicy_ab.sh`（`scan` / `set --latency=N --throughput=N` / `clear`）。
★ **正式 A/B 的那一次改档请加 `--no-readback`**：改档是**干预**、不是观察，而 `taskinfo` 恰好会落在"前后两半"
的交界处（10.3 的 153 ms 教训）；加上它以后腿上只留一行时间戳，档位本身的效果必须从探针报告里看出来。
**本机实测的档位**：`taskpolicy -l` 接受 **1..5**，**拒绝 6 及以上**（`Could not parse '6' as a qos tier`）；
数字**越大越偏延迟**（= XNU 的 `LATENCY_QOS_TIER_1`），与常量名字相反，所以 `scan` 会把回读打在旁边。
`-p` 改**别的**进程需要 root，回读用 `taskinfo`（**重观察**，见 10.3：只能出现在诊断腿上）。

**预登记（飞之前就写在这里）**

```
### 预登记 2026-10-01 / P2 档位 A/B
* 目的：判据是「尾部率是否跟档位走」——跟 ⇒ 机制在宿主调度层；不跟 ⇒ 指向电台/USB。
* 自变量：同一条腿内 `taskpolicy -l <tier>` 的前后半段（同一二进制、同一电台、同一手机、同一热状态）；
  另加一条 `-t <thr>` 的独立腿，避免两个档位混淆。
* 载荷：stress（真加载，先过 ul_load.sh 资格）；时长 ≥ 15 min，改档点在第 6~8 分钟。
* 判据（读数载体，与高层 §5.2 一致）：
  C1 `p99`（rx_wait / t2f / ce / eqdam / ldpc / gpu_pipeline）在**改档前后**是否变化；
  `[ul_timing_events]` 的 `leg :` 基线切换速率、每条事件 `ivcsw_rate=` 与 `tcpu=` 的分布是否变化。
* 反例判据（必须同时满足，否则判"档位无效"而不是"档位有效"）：
  ① 改档前后的 `median`（C2）**不得**移动（移动说明是热/载荷漂移，不是档位）；
  ② 同一条腿上改档点两侧的**载荷资格**（CRC-OK 率、TBS 分布）不得变化；
  ③ `gaps`/`rx_overflows` 必须仍为 0。
* 回退条件：出现 `gaps>0`、`rx_overflows>0`、契约红、或手机掉线 ⇒ 立刻 `clear`，该腿作废。
* 读数位置：`work_tmp/taskpolicy_<label>_*.txt`（含改档瞬时）、腿的 stderr（`[ul_timing_events]`）、
  `.log` 的 `[RF]` 行。
* ⚠ 这条腿是**臂**：`taskpolicy` 改的是调度，`leg_gate.sh` 的白名单里**没有**它，所以它**不得**进任何验收结论。
```

### 10.8 P4 参数标定（用 P0 的 p99 分布填 §9；**仍未施用**）

| 线程 | 期望周期 | p99（最近 7 条 n78/gpu/rx1 腿）| 候选 computation | 候选 constraint | 备注 |
|---|---|---|---|---|---|
| `lower_phy_ul#0` | 500 µs（一槽）| t2f **619.9 µs**（含接收等待）| 不应期定：t2f 的固定部分 ≈ 619.9 − 174 = **446 µs** 已接近一槽 | 若能做，只能给"固定部分" | ★ 值已接近周期，**没有余量** |
| `lower_phy_rx#0` | 35.7 µs（一符号）| rx_wait p99 **174 µs**（等待，不是计算）| 未测（本机没有"收包本身耗时"的序列）| — | 缺 computation 的读数 |
| `main_pool#N` | 500 µs | ce 162.9 / eqdem 898.3 / ldpc 115 µs | ce+eqdem+ldpc 的 p99 之和 ≈ **1.18 ms** > 一槽 | — | 池线程每槽要跑**多个** hop，周期不是 500 µs |

**结论（写给用户裁决）**：P4 的两个前提现在都**不成立**：
① 参数标定需要的"每线程计算量的 p99"只对 ce/ldpc 有（eqdem 是 898 µs，池线程一槽要跑好几跳）；
② 更硬的一条 —— 10.5 实测**时间约束与 QoS 互斥**，施用它会**再次**把 QoS 档清掉并把 `pthread_setschedparam` 变成 EINVAL。
⇒ 建议 P4 **维持"最后选项"并且暂不施用**；真要试，必须**一次只改一个线程**（现在 `bind_thread_to_performance_core()` 没有线程过滤）。

### 10.10 Linux 复核（Ubuntu 台架 `jwang@192.168.0.106:~/work/ocudu`，2026-10-01）

**怎么把 commit 送过去**：本机 `git bundle create <file> apple-silicon` → `scp` → 台架 `git fetch <bundle> apple-silicon`
→ `git merge --ff-only FETCH_HEAD`（台架的 `032948b560` 是本 commit 的祖先，所以是快进；**不动共享远端**）。

| 检查 | 结果 |
|---|---|
| `cmake --build build --target macos_compat_test ul_pipeline_probe_test` | ★ **抓到第 1 个真实错误**：`attr_qos_is_opt_in_and_platform_gated` 里 `qos_class_t qos` 声明在 `#if defined(__APPLE__)` **外面**（该类型只有 Darwin 有）。已修（commit `268f3b1760`，把声明移进守卫内）。**这就是跑台架的理由** |
| `ctest -R "macos_compat\|ul_pipeline_probe"` | **22/22 通过**（1 skip = `compiled_out_without_flow_probes`，因为台架的 build 是 `ENABLE_FLOW_PROBES=OFF`）|
| **Linux + `-DOCUDU_FLOW_PROBES` 定点编译** | ✅ 通过。台架的 build 是 probes **OFF**，所以 P1 那些分支在 Linux 上永远不会被编译到；用一个只 include 探针头并**实例化**新接口的 TU（`-Werror -Wall -Wextra-semi -Wshadow`，用台架自己的 flags）补上这一格 |
| 全量 `cmake --build build -j 8` | ★ **抓到第 2 个真实错误，而且是"默认配置根本编不过"**（见下），已修（commit `94e93cc482`）⇒ 修后 **`BUILD_RC=0`、`warning:` 计数 = 0** |
| 全量 `ctest -L phy -j 4` | ✅ **184/184 通过，0 失败**（1 skip = `lower_phy_uplink_processor_assembly_arm`，登记的禁用臂）。本机 macOS 同一标签是 208 个用例（平台差异：memcheck/仅 Apple 的用例），**两边的通过率都是 100%** |

**第 2 个错误（与本线无关，但只有台架能看见）**：`ENABLE_FLOW_PROBES=OFF` —— **项目的默认值**，也是台架用的配置 ——
在 `lib/phy/lower/lower_phy_baseband_processor.cpp` 上被 GCC 以 `-Werror=unused-variable` 拒收：
`spans_stream_start` 只被 `#if defined(OCUDU_FLOW_PROBES)` 那一臂消费，而 `ul_rx_note_call()` **本身不是可选的**
（它的副作用就是 `[ul_rx_timing]` 那条**每平台都判**的交付序列），所以调用必须留着、只有返回的标志是条件性的。
本机是 **clang + probes ON**，两个条件都不满足 ⇒ 这条线从 `328d273e0f` 起就一直是断的，没人看见。
修法 = `[[maybe_unused]]`，**任何平台、任何配置下行为不变**。

**"关着时逐字节不变"的两条实证**
1. **结构**：`git diff` 里**没有任何既有的打印语句被修改** —— 新字段/新块全部落在已经由 `OCUDU_UL_TIMING_EVENTS` 门控的
   `print_timing_events()` 内部，`[sched]` 是一个**新行**且由 `OCUDU_FLOW_PROBES` + `OCUDU_SCHED_VERBOSE` 双门控。
2. **实测**（本机 loopback，全部旋钮不设）：完整一条腿（`contract MET`），stderr 里
   `tcpu=` / `ivcsw_rate=` / `thread=…#` / `[sched]` / `none above the … floor` 的出现次数 **全部为 0**。

### 10.11 ★ 第一对腿的**预登记**（仪器本身怎么算"装好了"）

飞之前登记，免得事后挑解释。**读数腿** = 与验收腿同配方 + `OCUDU_SCHED_VERBOSE=1`；**验收腿**不设任何新开关。

| 检查 | 期望（默认臂）| 反向（说明仪器或修复坏了）|
|---|---|---|
| `[sched]` 行数 | = 该腿的 `unique_thread` 线程数（loopback 11、真电台 ≥ 24）| 0 行 ⇒ 两把钥匙没同时开 |
| RT 线程（`rt_intent=1`）| `req=USER_INTERACTIVE eff=UNSPECIFIED posix=FIFO/44..46` —— **这正是今天的真实现状**，不是故障 | `eff=USER_INTERACTIVE` ⇒ **10.5 的机制被推翻了**，必须重开那一节 |
| 非 RT 线程（`rt_intent=0`）| `req=USER_INITIATED eff=USER_INITIATED posix=OTHER/…` | `eff=UNSPECIFIED` ⇒ 两条路的差别消失，机制另说 |
| B 臂（`OCUDU_SCHED_SKIP_POSIX_RT=1`）| RT 线程 `eff=USER_INTERACTIVE posix=OTHER/31` | 仍是 `UNSPECIFIED` ⇒ 跳过没生效或档另有来路 |
| `[ul_timing_events]` 头 | 有 `leg : over …s … = …/ms` 一行（非零速率）| 缺行 ⇒ 接收路径没跑起来 |
| 每条事件 | 都有 `thread=<名>#<id>`；rx 事件有 `tcpu=`（`-` 也算**合法**读数，见规则 1）；`ivcsw_rate=` 与 `ivcsw/win` 自洽 | 事件行缺 `thread=` ⇒ 归因链断了 |
| **相位块**（加载腿）| `ce`/`t2f`/`eqd`/`ldpc` 四块**应各有一到数条**（历史 max：ce 1.2 ms > 1 ms floor、t2f 0.7–13 ms > 2 ms、eqd 3–13 ms > 3 ms、ldpc 441–961 µs > 500 µs）| 全 "none above the floor" 而**载荷资格成立** ⇒ 相位 floor 定得不对，回 P0 重导 |
| 判据 | C1/C2（高层 §5.2）在**当前 HEAD** 上成立 | 挂 C2 而 C1 过 ⇒ "整体变慢"，先查是不是本次改动带进来的 |

### 10.13 2026-10-01 —— 第一条真腿 `p183-n78-default`：**发现我自己的一个热路径 bug**，以及腿本身的读数

#### (1) ★ bug：相位事件记录的"关着"臂**没有真的关**

**怎么发现的**：读 p183 的腿文件时按 §10.11 的预登记逐条核对（这条腿**没有**带 `OCUDU_UL_TIMING_EVENTS`），
于是问了一句"旋钮关着时，相位那一臂到底做了什么"，读代码发现：

* `record_phase_timing_event()`（公开入口）**有**旋钮检查；
* 但**探针内部的两个调用者**（`record_ldpc_start()`、`record_end_crc_ok()`）持有锁、直接调
  `record_phase_timing_event_locked()` —— **那个函数体里没有检查**。

⇒ 旋钮关着时，**每个 PUSCH hop** 仍然：构造事件 → `attach_cpu_delta()` →
`this_thread_sched_snapshot()`（`thread_info` + `pthread_threadid_np` + `pthread_getschedparam` 三次系统调用）
+ `snprintf` 线程名，**在池线程上、每 hop 四次**（t2f/ce/eqdem/ldpc）。
另外 `record_ldpc_start()` 里给 `phases_entry` 填三个瞬时用的 `to_ns()` **每次两次读钟**，×4 = 每 hop 六次读钟，
也是无条件执行的。

**这违反本项目自己的规则**（"旋钮不设 ⇒ 不读钟、不打印、零成本"），而且落在**数据面热路径**上。
量级：约 **8–16 µs CPU / hop**（hop ≈ 1.25 ms），约 1%。

**修法（两个调用点都要）**
1. 把检查**移进 `record_phase_timing_event_locked()` 函数体** —— 一个检查，所有调用者都经过它；
2. 昂贵的**实参**要在调用点先判：`if (timing_event_wanted_phase(...)) { … to_ns(…) … }`（C++ 先算实参再进函数）；
3. `record_ldpc_start()` 里的三个瞬时改成 `want_phase_events ? to_ns(…) : 0`（0 = "没有采集"，因为**没有别人读它们**：
   `get_phase_durations()` 只用三个**时长**）。

**反向臂**（纪律 79，已实测能打红）：单测直接调 **`_locked`** 入口（这正是 bug 的入口），断言
`phase_event_candidates[]` **不增加**；把函数体里的检查删掉重编译，用例变红（实测过），加回去变绿。
*candidate 计数器就是"函数体跑过了"的观测量 —— 不需要数系统调用。*

#### (2) 腿 p183 的读数（**关于修复前的二进制**，所以按审计规则它随旧二进制过期）

**`[sched]` 在真电台腿上复现了 10.5**（12 行；`unique_thread` 覆盖的线程）：

```
[sched] thread=main_pool#0    rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED run=running posix=FIFO/44
[sched] thread=lower_phy_rx#0 rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED run=running posix=FIFO/44
[sched] thread=lower_phy_ul#0 rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED run=running posix=FIFO/45
[sched] thread=lower_phy_tx#0 rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED run=running posix=FIFO/46
[sched] thread=io_timer_tick  rt_intent=0 req=USER_INITIATED   eff=USER_INITIATED   run=running posix=OTHER/31
```
⇒ **10 条实时线程全部无档，2 条非实时线程有档** —— 与 loopback 上完全一致，机制在真机上成立。
（覆盖范围：`unique_thread` 创建的 12 条；UHD/libusb 与 SCTP 那几条不走这条路径，所以不打印 —— 这是**已知边界**。）

**C1/C2（§5.2 登记值）对 p181（同配方、同配置的参考腿）**：

| 序列 | p181 p99 / median | **p183 p99 / median** | C1 | C2 |
|---|---|---|---|---|
| `ul_pipeline` | 1644.0 / 1329.0 | **1605.0 / 1302.0** | ≤2000 ✔ | ≤1700 ✔ |
| `ul_gpu_pipeline` | 1535.8 / 1262.1 | **1535.5 / 1253.2** | ≤2000 ✔ | ≤1600 ✔ |
| `ul_time_frequency` | 622.2 / 490.2 | **620.8 / 489.5** | ≤780 ✔ | ≤640 ✔ |
| `ul_channel_estimation` | 173.2 / 64.0 | **165.9 / 63.2** | ≤210 ✔ | ≤84 ✔ |
| `ul_equalization_demod` | 905.5 / 700.8 | **898.6 / 691.7** | ≤1130 ✔ | ≤850 ✔ |
| `ul_ldpc_decode` | 210.0 / 59.0 | **128.0 / 43.0** | ≤150 ✔ | ≤58 ✔ |
| `ul_rx_wait` | 174.0 / 0.0 | **174.0 / 0.0** | ≤220 ✔ | — |

⇒ **C1/C2 全部达标，且多数比参考腿略好**（t2f/ce/eqdem/ldpc 的 p99 与中位都更低）。

**门的结果：9/10**，唯一的 FAIL 是：
```
[FAIL] VALIDITY (6.198 (4)): AT/BELOW 0 <= 10 AND no transport storm (slip/recv over 1ms <= 10, gaps = 0)
       in-stream AT/BELOW 0=5 (population 377989), slip=12, recv=10, gaps=0
```
`slip over 1ms = 12` 超了登记的 `≤10`（p181 是 10，正好压线）。**历史分布**：同类 n78 腿的
`slip over 1ms` 读到过 7 / 10 / 10 / 12 / 22 —— 12 落在**这个平台的常态**里，而这条判据的登记值偏紧。
**诚实的表述**：这条 FAIL 与"本次改动"的关系**无法从一条腿分开**（而我的热路径 bug 恰好会给池线程多加
每 hop 几 µs、可能正好推高 slip）；bug 已修，重飞的腿会把它变成可判的问题。`gaps=0`、`rx_overflows=0`、
契约 MET、`AT/BELOW 0 = 5`（≤10）都是干净的。

**没测到的**：这条 default 腿**没有**带 `OCUDU_UL_TIMING_EVENTS`（历史配方如此），所以没有 `[ul_timing_events]`
块，**相位尾部与 `thread=`/`tcpu=` 尚未在真腿上出现过** —— 留给下一对腿（§10.11 的预登记就是为此写的）。

### 10.14 2026-10-01 —— 腿日志的目录：**改工具让它支持文档说的事**（用户发现）

**现象**：`p183` 落在 `doc_chinese/phy_pipeline_gpu/wip/logs/`，而本目录 README 写的是"本线新腿放本目录 `wip/logs/`"。
**原因**：`run_leg.sh` 把 `LOGDIR` 写死成历史目录，**这条约定从规划期起就没有被实现过**（本目录的 `wip/logs/` 一直是空的）。

**决定（已实现）**：**让工具支持这条约定，而不是把 README 改小**：
1. `run_leg.sh`：`LOGDIR=${LEG_LOGDIR:-<历史目录>}` —— 默认**逐字不变**（其它线不受影响），本线在命令里带
   `LEG_LOGDIR=doc_chinese/macos_thread_priority/wip/logs`；
2. 三个门（`leg_gate.sh`、`ul_load.sh`、`milestone_audit.sh`）改成**两个目录一起找**（`ls -1t` 跨参数按 mtime 排，
   所以"最新的那条同名腿"语义不变）—— 老腿照旧能找到，新腿也能（两个方向都实测过）；
3. `spike_census.py`/`threshold_candidates.py` **早就**读两个目录，无需改动。
**没有做**：没有移动任何既有腿文件（历史不改），也没有改任何一处判据。

### 10.15 待补
（每次飞腿/改动后追加：做了什么、读数、判据是否满足、更正了哪一条。）
