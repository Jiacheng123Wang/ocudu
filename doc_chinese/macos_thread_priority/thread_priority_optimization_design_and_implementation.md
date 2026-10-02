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
| **Linux 复核** | 同一 commit 在 **`jwang@192.168.100.131:~/work/ocudu`**（原 `192.168.0.106`，网络变更见 §10.36）上：`cmake --build build --target ul_pipeline_probe_test macos_compat_test`（或等价目标）+ `ctest`；结果记进 §10 的"Linux 复核"小节 |

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

> ★ 本节及以下各处出现的 `192.168.0.106` 是**当时**的台架地址；**网络于 2026-10-02 变更**：台架现为 **`jwang@192.168.100.131`**、核心网 `192.168.100.153`、
本机 `192.168.100.125`；本文件里更早段落中的 `192.168.0.106` 是**当时**的地址，属历史记录，不再可达。

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

### 10.15 2026-10-01 —— 修复后的验收对 `p185-n78-default` + `p186-n78-stress`：**审计 GREEN（33/0/0）**，仪器首次在真腿上出数

两条腿都是 HEAD `1b00af6780`、**落在本目录自己的 `wip/logs/`**（`LEG_LOGDIR` 生效），各 4 个旋钮行
（`OCUDU_METAL_GPU_TIME`/`OCUDU_UL_PHASE_SEGMENTS`/`OCUDU_UL_TIMING_EVENTS`/`OCUDU_SCHED_VERBOSE`，全是 probe）。

#### (1) ★ 审计结论

```
33 PASS, 0 FAIL, 0 RED(cannot read), 3 INFO  (of 36)     offline acceptance: GREEN
```
（中途一度读到 `32 PASS / 1 FAIL`：**是我自己的双目录补丁漏了 `a12_attribution_gate.sh`** —— 它找不到新目录里的腿，
于是 A1-2 归因门报 "0 of 4 criteria pass"。补齐后 GREEN。这正是"分目录"这件事的代价：**8 个脚本都要会找两个目录**，
漏一个就是一条假的 FAIL。已全部补齐并逐个在**老目录腿（p183）与新目录腿（p185）**上验证。）

#### (2) `[sched]`：机制在真机上稳定复现（两条腿各 12 行）

| | 线程数 | 回读 |
|---|---|---|
| `rt_intent=1` | **10** | `req=USER_INTERACTIVE **eff=UNSPECIFIED** posix=FIFO/44..46` |
| `rt_intent=0` | **2** | `req=USER_INITIATED **eff=USER_INITIATED** posix=OTHER/31` |

⇒ 与 p183 逐字一致：**请求被我们自己的 POSIX 调用抹掉，且不可恢复**（§10.5）。
**覆盖边界**（已知）：`unique_thread` 建的线程才打印（12 条）；UHD/libusb 与 SCTP 那几条不走这条路径。

#### (3) 仪器第一次在真腿上出数（p185，default）

**接收事件（`tcpu=` 在这里给出了它存在的理由）**：
```
rx #1 wait=2158us air=36us thread=lower_phy_rx#0#3109468 load1=3.89
      cpu=4.33ms tcpu=0.12ms ivcsw=+413 ivcsw_rate=191.20/ms nvcsw=+1 win=2160us base_age=305us
```
* `win=2160us` 里**进程**烧了 **4.33 ms** CPU（≈2 核，与"常驻 1.92 核"一致）⇒ 宿主**没有被饿着**；
* 同窗口里**这条线程自己只烧了 0.12 ms**（5%）⇒ 它**在 `recv()` 里等着**，不是在算；
* `ivcsw_rate=191.20/ms` 与报告头的**本腿基线 194.65/ms** 几乎相同 ⇒ 这条事件的切换率**不比平时高**，
  所以"被频繁抢占"不是这条尾巴的解释。
* **判读（新仪器的第一个结论）**：2 ms 级的接收尾巴是**一次阻塞等待**，进程在别处忙、机器上有 12 个空闲核；
  指向**电台/USB 交付**，不是宿主调度。这正是老仪器（只有进程级 `cpu=`）**看起来也支持、但分不出**的那件事。
* `dl` 事件一律 `tcpu=-`（**按规定**：基线是接收线程取的，两条线程的累计计数器不能相减）。

**报告头新增的腿级基线**：`over 188.5s … 36686364 involuntary … = 194.65/ms` ——
**每秒 19.5 万次非自愿切换**（两条腿 194.65 / 194.31，彼此差 0.2%），与用户之前 `taskinfo` 读到的 `csw 194 k/s` 吻合。

**相位尾部第一次在真腿上出现**（这条序列此前**只有聚合值**）：

| 序列 | p185（default）| p186（stress）| 线程 |
|---|---|---|---|
| `ce` | **1717 / 1063 / 1007 µs** | 1287 µs | `main_pool#0/#1/#4` |
| `eqd` | **3610 / 3240 / 3027 / 3016 µs** | 3679 / 3653 / 3277 / 3199 µs | `main_pool#1/#2/#3/#4` |
| `ldpc` | 615 / 591 / 552 / 537 / 507 / 503 / 500 µs | none above 500 µs | `main_pool#0/#1/#3` |
| `t2f` | none above 2000 µs | none | — |

⇒ `ce max ≈ 20× 中位` 这条本线的起点现象，**第一次带上了"哪条池线程"**。

**★ 已知缺口（下一处要改的代码）**：这些相位事件绝大多数打印 `cpu=- tcpu=-`，
报告头也明说"9 phase event(s) started before the receive path's last baseline"。
原因：进程基线由**接收线程**每 ~1 ms 刷一次，而 `ce`/`eqd`/`ldpc` 的窗口（0.5–3.7 ms）**起点常常早于**它 ⇒ 按规则只能拒绝。
**修法（已设计，未实施）**：在每个相位**起点地标**（`record_t2f_end` / `record_ce_end` / `record_ldpc_start`）
在旋钮开着时取一次基线（进程+线程），于是 `ce`/`eqd`/`ldpc` 三个窗口各自都有自己的基线，
`cpu=`（进程）与 `tcpu=`（`eqd`/`ldpc` 两端同线程时）都能给值。代价 = 旋钮开着时每 hop 多 3 次 `getrusage` + 3 次线程快照
（≈2–3 µs/hop，仅诊断臂）。**与本线的另一次改动合并成一次重飞**，避免为一处小改再花一对腿。

#### (4) 判据（§5.2 登记值）

| 序列 | C1 | p185 | p186 | C2 | p185 med | p186 med |
|---|---|---|---|---|---|---|
| `ul_rx_wait` | ≤220 | 173.0 ✔ | 174.0 ✔ | ≤43 | 0.0 ✔ | 0.0 ✔ |
| `ul_time_frequency` | ≤780 | 620.7 ✔ | 620.7 ✔ | ≤640 | 489.6 ✔ | 490.0 ✔ |
| `ul_channel_estimation` | ≤210 | 164.0 ✔ | 160.0 ✔ | ≤84 | 62.2 ✔ | 63.3 ✔ |
| `ul_equalization_demod` | ≤1130 | 891.4 ✔ | 895.3 ✔ | ≤850 | 693.3 ✔ | 695.0 ✔ |
| `ul_ldpc_decode` | ≤150 | **152.0 ✗（超 1.3%）** | 120.0 ✔ | ≤58 | 41.0 ✔ | 50.0 ✔ |
| `ul_gpu_pipeline` | ≤2000 | 1530.3 ✔ | 1526.7 ✔ | ≤1600 | 1253.9 ✔ | 1256.1 ✔ |
| `ul_pipeline` | ≤2000 | 1599.0 ✔ | 1600.0 ✔ | ≤1700 | 1297.0 ✔ | 1311.0 ✔ |

* **唯一的 C1 未达标是 p185 的 `ldpc` p99 = 152 µs 对登记值 150 µs**（2 µs，1.3%）。
  背景：登记值是从 **stress 家族**的 7 条腿导出的（per-leg p99 中位 115，×1.25 → 取整 150）；
  而 **default 家族**历史上这条序列读得更高（p181 **210**、p183 128、p185 152）。
  这正是"default 家族攒够 5 条腿前用 stress 表判"的已知代价（高层 §5.2 已写明）；不是本次改动带进来的形状
  （p183 修复前 128、p181 更早 210，腿间摆动远大于 2 µs）。**不移动登记值**（判据不追溯），
  只在 default 家族攒够腿后按规则重新导出。

#### (5) 门的结果：两条腿都是 **9/10**，唯一 FAIL 同一条

```
[FAIL] VALIDITY (6.198 (4) restated): AT/BELOW 0 <= 10 AND no transport storm (slip/recv over 1ms <= 10, gaps = 0)
       p185: AT/BELOW 0=3 (population 376941), slip=12, recv=10, gaps=0
       p186: AT/BELOW 0=5 (population 365422), slip=12, recv=9,  gaps=0
```
`slip over 1ms = 12` 超 `≤10`。**同类 n78 腿的历史分布**（本目录 `wip/spike_census.py` 的日志，按时间倒序）：

| 值 | 腿 |
|---|---|
| **10** | p178, p181 |
| **12** | p179, p183, **p185**, **p186** |
| 15 / 21 / 22 | p154, p153, p177 |
| 37 … 684 | 09-28/09-29 的 12 条老腿 |
| 124 | p182_1726（**被观察腿**，§10.3）|

⇒ 登记的 `≤10` **正好卡在这个平台近期最好的一档上**（= 历史最好的腿），而 12 是**并列第二好**：
最近 9 条腿里 4 条读数就是 12。**这不是本次改动造成的**（修复前 p183 也是 12、修复后 p185/p186 仍是 12 ⇒ 修 bug 没动它）。
**处置（等用户裁决）**：① 按"小计数 + 干净传输不是传输故障（6.230）"接受并写明；或 ② **前瞻性**把这条绑定的界限
重登记为"近 9 条腿的最坏值（22）"（只对**之后**飞的腿生效，不追溯），并在高层 §5.2 登记出处。**不在本会话里偷偷改。**

#### (6) 载荷资格（p186）

`UL grants 45368 (12.4% of slots)`、**UL payload 4.10 Mbit/s**、`TBS median 1953 B (≥1000 B: 85.5%)`、
`CRC-OK 42976/45368 = 94.7%`、`gaps=0`、`rx pool OK` ⇒ **stress 腿确实加载**（>2.0 Mbit/s 的 validity 门槛）。

### 10.16 2026-10-01 —— **用户裁决落地（两项）+ 相位基线缺口修复**：一次合并改动

用户裁决（当晚）：① 传输风暴那条绑定**前瞻性**重登记；② `OCUDU_SCHED_SKIP_POSIX_RT` 的语义**升级为默认**。
两项与 §10.15(3) 的相位基线缺口**合并成一次改动**，理由是不为一行小改再花用户一对腿。

#### (1) 判据重登记（**只前瞻，不追溯**）：`slip/recv` 的界限 10/10 → **22/19**

出处（**写死，可复核**）：当前家族 `gpu/gnb_rf_b200_tdd_n78_20mhz.yml/rx1` 的 **23 条腿**（被观察腿 p182_1726 除外），
每条按整腿读数取：

| 腿 | AT/BELOW 0 | slip | recv | | 腿 | AT/BELOW 0 | slip | recv |
|---|---|---|---|---|---|---|---|---|
| p180 | 1 | 4 | 2 | | p177* | 60 | **22** | **19** |
| p181 | 4 | 10 | 9 | | p178* | 16 | 10 | 6 |
| p182 | 1 | 7 | 5 | | p179* | 17 | 12 | 7 |
| p183 | 5 | 12 | 10 | | p176* | 19 | 10 | 6 |
| p185 | 3 | 12 | 10 | | | | | |
| p186 | 5 | 12 | 9 | | (* = 10-01 上午的 stress 簇) |

新界限 = **最近 9 条家族腿的最坏值**（slip 22、recv 19）。**`AT/BELOW 0 <= 10` 不动**（健康读数 1..6，有余量）。
**目的没被削弱**：这条判据要抓的是 p153 形状（slip 21/**9**/**467**、recv 35/**12**/**570**，即成百上千的复发），
22/19 仍低一个数量级。旧界限 10 恰好压在这条平台自己的**众数**上（健康簇就是 10..12），
所以它对**与参考腿无法区分**的腿判 FAIL —— 实测 p179 读 12 而 p178/p181 读 10，且热路径修复**没有**移动它
（p183 修复前 12、p185/p186 修复后仍是 12）。
落地位置：`leg_gate.sh` 的 `tx_name` 与判定行，**推导过程写在该文件的注释里**（判据的出处必须随判据走）。
复验：p185/p186 由 **9/10 → 10/10**。

#### (2) `OCUDU_SCHED_POSIX_RT`：默认**不**调用 POSIX（用户裁决）

* 旧名 `OCUDU_SCHED_SKIP_POSIX_RT`（opt-in，**从未飞过腿**）→ 反转为 `OCUDU_SCHED_POSIX_RT`（`=1` = 恢复历史行为）；
* **新默认**：跳过那个调用 ⇒ QoS 档存活。理由是两条臂不对称：带调用时数据面**无档**（bench + 3 条真腿实测），
  跳过时保 `USER_INTERACTIVE`；让出去的那条 `SCHED_FIFO` 在本移植上**从未被证明被内核执行过**，它唯一的已证效果就是抹档；
* 保留对照臂的理由：**同一个二进制**上就能 A/B 推翻默认（靠重编译做 A/B 会把"构建"混进比较里）。

**两条臂都在 loopback 上复验**（`OCUDU_SCHED_VERBOSE=1`）：
```
默认（无变量）        [sched] thread=main_pool#0 rt_intent=1 req=USER_INTERACTIVE eff=USER_INTERACTIVE posix=OTHER/31
OCUDU_SCHED_POSIX_RT=1 [sched] thread=main_pool#0 rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED   posix=FIFO/44
```

#### (3) 相位事件的 `cpu=`/`tcpu=`：缺口修复（§10.15(3) 的设计）

**做法**：给 `start_entry` 加一个 `cpu_snapshot base`，由**开启每个窗口的那个地标**写入 ——
`record_t2f_end`（开 `ce` 窗口）、`record_ce_end`（开 `eqdem` 窗口）、`record_ldpc_start`（开 `ldpc` 窗口）；
事件记录时用它作为基线。**不新增注册表**（基线搭在地标本来就要建的那条 entry 上），
**旋钮关着时不取**（`phase_baseline_for()` 里判，`phase_baselines_taken` 计数器是这条的观测量）。
`t2f` 故意不取（它的起点是接收路径**每块**的热路径，而 2 ms 的 floor 让它极少有机会上榜）—— 它继续打 `cpu=-`。

★ **写这个修复的单测抓到了第二个真 bug**：第一版把 `to_ns(landmark.tp)` 当作窗口起点、基线却是同一地标取的，
而 `to_ns()` **每次都重读两个钟**，两次换算同一 `time_point` 会差几纳秒 ⇒ `base.ns > begin_ns` **偶尔成立** ⇒
基线被当成"晚于窗口"拒绝，`ce` 行**仍然打印 `cpu=-`**。修法：窗口起点就用**基线自己的瞬时**（`window_start_ns()`），
比较变成精确的；时长仍由 time_points 计算（那里本来就是这么量的）。

**反向臂**（纪律 79，实测可打红）：把 `phase_baseline_for()` 里的旋钮判断删掉 ⇒
"旋钮不设时地标不得取任何快照"这条断言立刻变红。

#### (4) 回归

`ctest -L phy -j 1` = **208/208**；**Ubuntu 台架**（同一 commit，`ENABLE_FLOW_PROBES=OFF` 的项目默认配置）：
`cmake --build build -j 8` = **`BUILD_RC=0`**、`ctest -L phy -j 4` = **184/184 通过 / 0 失败**。
探针单测两种跑法都过（整二进制 **14/14**、ctest 逐用例 **9/9**）——
★ 其中一次失败是**我自己的用例**在整二进制模式下读到了**别的用例**留下的 `ce #1`（探针是进程级单例）；
用例已改为先清空自己的列表，并在注释里写明两种跑法差异**不得**决定判据。

### 10.17 2026-10-01 —— 最后一对腿：**三项预登记全部命中**；另有一条腿作废、一条新发现

#### (1) ⚠ 第一条 p187 作废（用户发现"第一次有问题"）——**它抓到了一次真实的宿主冻结**

同一天飞了**两次** `p187-n78-default`（20:40 与 20:44 本地 = 12:40/12:44 UTC），两次都在 HEAD `76860a9afb`。
第一次门判 **8/9**：

```
[FAIL] … AT/BELOW 0 <= 10 … : in-stream AT/BELOW 0=21 (population 438461), slip=15, recv=12, gaps=0
```
**但这不是"腿慢"，而是一次瞬时冻结**，证据全部指向同一秒：

| 证据 | 读数 |
|---|---|
| 21 次迟到交接、21 条 `[RF] late`、2 条 `[RF] underflow` | **全部落在 `12:41:07` 这一秒**（整腿 219 s）|
| 最大接收等待 | `rx #1 wait=14657us` —— 也在这个窗口（began 12:41:07.254）|
| ★ 该窗口内的切换率 | `ivcsw_rate=6.00/ms`，而**本腿自己的基线是 180.59/ms**（低 **30 倍**）|
| 该窗口内的进程 CPU | `cpu=2.98ms` / `win=14659us` ≈ 20% 占空比 |
| 机器忙不忙 | `load1=2.68`（14 核）—— **机器是闲的** |

⇒ 判读：**进程在那一瞬间根本没被运行**（不是被抢、不是电台慢）。这与 §10.3 的机制同类（`sample`/`taskinfo` 会挂起目标进程），
但**起因不在这条腿的数据里** —— 只能确定"某一外部事件把进程冻了 ~15 ms"，无法从 gnb 的读数区分是哪一件。
**处置**：该腿**作废**（与 p182_1726 同一类），以 20:44 的重飞为准；**本文档与所有判据不带它**。
（用户当时观察到"第一次有问题"，与此一致。）

#### (2) 三项预登记**全部命中**（p187_1001_2044 + p188_1001_2047，门各 **10/10**）

| 预登记（§10.11 / §10.16 末尾）| 实测 |
|---|---|
| ① `[sched]` 的 RT 线程应变成 `eff=USER_INTERACTIVE posix=OTHER/31` | ✅ 两条腿各 **10/10** 条 RT 线程 `eff=USER_INTERACTIVE`，`eff=UNSPECIFIED` **0 条**（新默认在真机上生效）|
| ② 相位事件应带 `cpu=<数值>` | ✅ `ce #1 … cpu=4.65ms win=1554us base_age=0us`、`eqd #1 … cpu=8.45ms tcpu=0.07ms win=3210us base_age=0us`（`base_age=0` 说明基线正是窗口起点，符合设计）；`tcpu=-` 只在 `ce`（跨线程）与 `t2f`（无自有基线）上出现 —— **都是规定的拒绝** |
| ③ `leg_gate` 两条都 10/10 | ✅ p187 **10/10**、p188 **10/10**；全量审计 **33 PASS / 0 FAIL / 0 RED = GREEN**（连跑两次一致）|

**新读数（p187 默认 / p188 加载）**：`eqd` 窗口 3210/3699 µs 里**进程**烧了 8.45/10.57 ms（≈2.6–2.9 核在忙）
而**完成该段的池线程只烧了 0.07/0.22 ms** ⇒ 这两段"长"是**等**（等上一段交棒、等栅栏/别的车道），不是算。
这正是相位 `cpu=` 存在的意义，也是第一条腿（`cpu=-`）给不出的东西。

#### (3) ★ 新发现：`ul_ldpc_decode` 的 C1/C2 **超线是载荷尺寸造成的**，不是调度

两条新腿都在 `ul_ldpc_decode` 上超了登记值（p187 p99 **184** / 中位 **66**，p188 **190** / **61**，登记 C1 150 / C2 58）。
把 **TB 尺寸**放进来一看就清楚了（`ul_load.sh` 的 LOAD 块）：

| 腿 | UL 载荷 | **TBS 中位** | ldpc p99 / 中位 |
|---|---|---|---|
| p185 | 2.46 Mbit/s | **1089 B** | 152 / 41 |
| p186 | 4.10 Mbit/s | **1953 B** | 120 / 50 |
| **p187** | **5.32 Mbit/s** | **3265 B** | **184 / 66** |
| **p188** | 4.14 Mbit/s | **3072 B** | **190 / 61** |

⇒ 这条序列量的是"第一个码块解码 → CRC OK"，**随 TB 的码块数线性增长**：TBS 3265 B ≈ 4 个码块 vs 1953 B ≈ 2 个
（184/120 ≈ 1.5、66/50 = 1.3，量级相符）。**登记值 150/58 是从 TBS 中位 ~2 kB 的 7 条腿导出的**，
拿它判一条 TBS 中位 3.0–3.3 kB 的腿，就是**又一次种群混样** —— 只是这次发生在**同一条序列内部**。
**处置（未擅动判据）**：① 本次不改登记值；② 报告里**必须把 TBS 分布与读数并列**（`ul_load.sh` 已经会打）；
③ 正解不是放宽，而是**按尺寸分层**——探针已经对 `[ul_gpu_pipeline]` 做了 `[ul_by_size]` 分层，
`ldpc` 需要同样的处理；这是下一处代码改动，**列入待办**（高层 §10）。

### 10.18 2026-10-01 —— **更正 §10.17(3)：ldpc 分层不属于本线**，以及本线真正要的两张表

**用户 2026-10-01 否决了"给 `ldpc` 做按尺寸分层"的提议**，理由是本线的任务定义：**测量 PHY 处理线程的优先级 + 改进运行稳定性**。
这个否决是对的，两条都成立：① 判据标定属于**时延/判据那条线**（`[ul_by_size]` 那套机器是它的产物）；
② 更要紧的是——既然 `ldpc` 的尾部**跟着载荷走**，它就**不是稳定性的载体**，为它加机器等于给一条不回答本线问题的序列加机器。
**处置**：本线**不再提** `ldpc` 分层；§10.17(3) 的**读数保留**（它解释了那两条腿为什么超线，且写了 TBS 与读数的对照），
但**结论只到"该序列承载载荷、不承载稳定性"为止**；判据本身交给管判据的那条线。

**本线要的两张表（同一对腿、同一条二进制、只差默认臂）**

| 腿 | `eff=USER_INTERACTIVE` | **非自愿切换 /ms** | rx_wait max | rx_wait p99 |
|---|---|---|---|---|
| p185 default（**旧默认**：POSIX 抹档）| 0 / 10 | **194.65** | 2158 | 173.0 |
| p186 stress（旧默认）| 0 / 10 | **194.31** | 3253 | 174.0 |
| p187_2040 default（**新默认**；本腿作废）| 10 / 10 | 180.59 | （外部冻结）| — |
| **p187_2044 default（新默认）** | **10 / 10** | **179.12** | 1851 | 172.0 |
| **p188 stress（新默认）** | **10 / 10** | **179.48** | 2073 | 172.0 |

1. **优先级这张表有信号**：无档臂 **194.65 / 194.31**（组内差 0.17%）、有档臂 **179.12 / 179.48**（组内差 0.20%），
   两组相距 **约 7.8%**；而且有档那两条腿的**载荷更大**（5.32 / 4.14 Mbit/s vs 2.46 / 4.10 Mbit/s）却**切换更少**。
   ⇒ **假设（不是结论，n=2/臂）**：QoS 档真的在减少非自愿抢占。**能把它变成结论的正是对照臂**
   （`OCUDU_SCHED_POSIX_RT=1`，同一条二进制）——这就是那个开关存在的理由。
2. **稳定性这张表没动**：稳定载体跨两臂**逐字不变**（`rx_wait p99` 172–174 µs、`t2f` 620.5–621.1、
   `gpu_pipeline` 1526.7–1531.1）⇒ 与本线已有的归因**自洽**：那些尾巴是**等**（接收线程 parked、`eqd` 窗口里进程忙而本线程只烧 0.07 ms），
   **不是**被抢占。唯一移动的两条是 `ldpc`（= 载荷，见 §10.17(3)）与 `ce`（160..174 的跨度把**两臂都罩住**，p181 无档时也读 173.2 ⇒ 不可归因）。

⇒ **本线的"可设置"部分到此有一个可检验的下一步：对照臂一对腿**（同配方 + `OCUDU_SCHED_POSIX_RT=1`），
用来判定上面第 1 条是结论还是巧合。

### 10.19 2026-10-01 —— **用户给出"运行稳定性"的定义**，本线按它重建了度量（并登记为判据）

**用户定义**：稳定性 = **PHY 处理线程运行相同任务时，所需时间变化不大**；即 mean/median/min/max/p95/p99 都该变化不大
（例子给的是 `[ul_gpu_pipeline]` median 1262.4 µs 应当在腿间保持不变）。
⇒ 本线原先的 C1/C2 只覆盖了 p99 与 median **两条**统计量，且只在"阈值"意义上用了它们；**定义要的是整条分布的可重复性**。

**改动（零新腿成本）**
1. `spike_census.py` 的解析补回 **mean/min**（此前被有意丢掉，理由是"没有判据用它" —— 在这个定义下那个理由不成立：
   **没人记录的统计量，没人能说它稳定**）；并且抓到并修正了一处**自己的**映射错误：
   正则的捕获顺序是 `samples/mean/median/min/**max**/p95/p99`，而字段顺序是 `…/p95/p99/max`，
   位置式 zip 会把 **p95 当 max、p99 当 p95、max 当 p99**（在 p188 上实测：`ul_rx_wait` 被打印成 p99 159 / max 172，
   真值是 p95 160 / p99 172 / max 2073）。现已改为显式映射，并用真值核对过。
2. 新增 `--stability` 视图：跨腿打印每个统计量的 min/max/**相对离散**与**持有两端的那条腿**（全文，不截断），
   并打印每条腿的**载荷**（TBS 均值/中位）——因为"同一个任务"这个前提必须能被检查；
   默认**排除**被观察腿 p182_1726（可用 `--keep-all` 关掉）。

**读数（本家族最近 8 条腿，含两臂：p178..p186 无档 / p187..p188 有档）**

| 序列 | mean | median | p95 | p99 | min | max |
|---|---|---|---|---|---|---|
| `ul_time_frequency` | **±0.5%** | ±0.5% | ±0.3% | ±0.7% | +407% | +3001% |
| **`ul_gpu_pipeline`** | **±2.0%** | **±2.3%** | ±2.2% | ±2.2% | +17.6% | +26.3% |
| `ul_pipeline` | ±3.6% | ±3.7% | ±4.1% | ±4.6% | +7.9% | +28.8% |
| `ul_equalization_demod` | ±2.9% | ±3.8% | ±3.1% | ±4.2% | +59% | +33% |
| `ul_channel_estimation` | ±6.7% | ±5.8% | ±12.5% | ±11.4% | +186% | +1487% |
| `ul_rx_wait` | ±0.9% | 恒为 0 | ±0.6% | ±1.7% | 恒为 0 | +8173%（被观察腿，已排除）|
| **`ul_ldpc_decode`** | **±64%** | **±89%** | ±91% | ±96% | +200% | +171% |
| `dl_tx_call` | — | ±2.6% | ±16.3% | ±12.0% | — | +2412% |

**结论**：
* **处理链的分布是稳的**：`t2f` 0.5%、`gpu_pipeline` 2.2%、`pipeline` 4%、`eqdem` 4%、`ce` ≤12%、`rx_wait` ≤2%
  —— 且这 8 条腿**横跨两臂**（有档/无档）⇒ 与 §10.18 的结论一致：优先级改动**没有搬动处理分布**；
* **`min`/`max` 天然不稳**（最幸运/最不幸的一次抽样），所以判据只能建立在 mean/median/p95/p99 上，
  `max` 留给 `[ul_timing_events]` 归因；
* **`ldpc` 不稳是"活儿不同"**：这 8 条腿的 TBS 中位从 **912 B 到 3009 B（3.3×）**，它的离散跟着载荷走
  ⇒ 再次印证 §10.18 的处置（该序列承载载荷，不承载稳定性）。

**已登记为判据**：高层 §5.3（前瞻、不追溯），含三条使用规则（同家族 / 载荷可比 / min-max 只作读数）。

### 10.20 2026-10-01 —— ★ 用户更正稳定性定义的**层次**：判据是**同一次运行内**，不是跨腿

**用户补充**（原话要义）：不同的**运行之间**本来就可能变（无线环境、业务流量类型），
**所以要看的是"同一次运行的统计中，mean/median/min/max/p95/p99 变化不大"**。

⇒ §10.19 把判据登记成"跨 8 条腿的离散"是**错的层次**：那些差异的主体是环境，不是稳定性。
（跨腿表仍有价值，但降级为**背景读数**：它回答"换环境后这套处理还在不在原地"。）

**新仪器（`OCUDU_UL_STABILITY_WINDOWS=K`）**：把**本次运行**按**记录顺序**切成 K 个等样本窗口，
逐序列打印每个窗口的 `median`/`p95` 与**相对整腿值的最大偏离**。
* **不采任何新数据、热路径零成本**：每个序列的样本向量**本来就是按时间顺序 push 的**，
  切窗只是**报告期**对这同一批数据再做一次切片（对比：为此存每样本时间戳或直方图都要动热路径）；
* 两把钥匙：编译期 `OCUDU_FLOW_PROBES` + `OCUDU_UL_STABILITY_WINDOWS`（默认关/`=1` 关 ⇒ 既有报告**逐字节不变**）；
* 样本太少（< K×8）时**明说"不切"**，而不是打出一张 3 样本窗口的表 —— 那种表的"偏离"是抽样噪声装成的不稳定；
* 已进两条闸门的 **probe 白名单**（只打印、不改判据），旋钮清单已重新生成。

**loopback 实测**（K=8，448 054 个接收样本）：
```
ul_rx_wait   n=448054  median[ 34.0 ×8 ] p95[ 38.0 ×8 ]   worst window vs whole run: 0.0%
```
——注意该腿 **max 是 7868 µs**：**分布稳、尾部有尖峰**，正是本线一开始就观察到的形状，
而新视图第一次把"稳"这个字量化在了**同一次运行**里。

**★ 反向臂，以及我第一版反向臂为什么是无效的**（值得记）：
* 第一版用"前 1000 个 100 µs、后 1000 个 200 µs"（一个台阶）当输入 —— **它根本区分不出对错**：
  排序一个二值序列会把快的一半排到前面，切出来的窗口图案**与按时间切一样**。这条臂写出来、跑过、**才发现不可能变红**。
* 有效输入是**交替**（100/200 交替 1000 对）：按时间切 ⇒ 每个窗口都是"半快半慢"、与整腿相同（偏离 0%）；
  先排序再切 ⇒ 前四窗全快、后四窗全慢 ⇒ **给一条没有漂移的运行报出 100% 漂移**。
  把实现 sabotage 成"先排序"，该断言**实测变红**；恢复后变绿。
* 为此加了 `reset_samples_for_test()`（探针是进程级单例、序列向量私有）：没有它，"两条臂各自断言"会在
  **整二进制跑**与 **ctest 逐用例跑**两种模式下给出不同的行 —— 让运行模式决定判据，是这份文档已经写过一次的坑。

### 10.21 2026-10-01 —— 首对带"同一次运行稳定性"视图的腿（p189/p190）：**仪器好用，我的预登记值被证伪一条**

`p189-n78-default` + `p190-n78-stress`，HEAD `4e4df4581a`，`leg_gate` 各 **10/10**，
全量审计 **33 PASS / 0 FAIL / 0 RED = GREEN**；载荷：p190 = 4.59 Mbit/s、TBS 中位 3072 B（≥1000 B 89.9%）。

#### (1) ★ 同一次运行内的稳定性（K=8，`[ul_stability]`）

| 序列 | p189 窗口 median（8 个）| 最坏偏离 | p190 最坏偏离 | 原登记 | 现登记 |
|---|---|---|---|---|---|
| `ul_time_frequency` | 490.0 490.0 491.0 490.2 490.4 491.8 490.7 489.4 | **0.5%** | 0.5% | ≤5% ✔ | ≤5% |
| `ul_gpu_pipeline` | 1261.4 1250.5 1247.9 1253.3 1257.8 1259.2 1258.0 1242.5 | **0.9%** | 1.3% | ≤2% ✔ | ≤5% |
| `ul_pipeline` | 1324 1315 1309 1319 1325 1330 1336 1289 | **2.5%** | 1.4% | ≤2% **✗** | ≤5% |
| `ul_equalization_demod` | 696.8 693.5 689.1 690.2 692.5 697.2 694.0 682.5 | 1.5% | 1.2% | ≤5% ✔ | ≤5% |
| `ul_channel_estimation` | 65.9 64.7 64.6 65.9 65.4 65.7 65.3 63.6 | 5.0% | **11.9%** | ≤15% ✔ | ≤15% |
| `ul_rx_wait` | p95 159 159 159 159 158 158 158 159 | **0.0%** | 0.0% | ≤5% ✔ | ≤5% |
| `ul_ldpc_decode` | 58 57 56 60 63 66 69 **36** | **41.2%** | 20.0% | 不判 | 不判 |

* **`t2f` 的 8 个窗口 median 落在 489.4–491.8（±1.2 µs / 490 µs）**，`gpu_pipeline` 落在 1242.5–1261.4
  —— 这就是用户要的那句话的直接读数：**同一次运行里，同一个任务耗时几乎不变**；
* **同一条腿的 `max` 仍是 4231/4254 µs** ⇒ "分布稳、尾部有尖峰"再次被同一次运行的数据同时证实；
* **`ldpc` 的窗口漂移正是"工作量在变"**：p189 的窗口 median 从 58 涨到 69、最后一窗掉到 36
  （业务量在腿内变化）⇒ 它不能当稳定性载体，这条判断第三次得到印证。

#### (2) ★ 我的预登记值错了一条，记下来

第一次登记 C3 时**没有任何同一次运行的数据**（旋钮还不存在），那组带（gpu/ul_pipeline ≤2%）是我拿
**跨腿离散**类比出来的。**首对腿把它证伪了**：`ul_pipeline` 读 2.5%。
⇒ 已按首对腿的实测值**重新登记**（5%/15%，前瞻），并在高层 §5.3 里**写明这一版是从实测导出的、上一版是猜的**。
这正是"判据先登记"这条纪律的用法：**它让"我的估计偏紧"这件事在数据到手的当天就暴露出来，而不是被悄悄调宽**。

#### (3) 三项老预登记在新 HEAD 上继续成立

`[sched]`：两条腿各 **10/10** 条 RT 线程 `eff=USER_INTERACTIVE`（新默认在真机上稳定）；
`leg_gate`：两条 **10/10**；全量审计 **GREEN**。

### 10.22 2026-10-01 —— ★★ 稳定性定义的**最终版**（用户第三次说明）：**理想是 `min == max`**

**用户原话（要义）**："同一任务重复执行时耗时应该变化不大；**最稳定的情况应该就是 `min == max`**"，
例子给的是 p190 的 `[ul_gpu_pipeline] samples=28589 mean=1268.6 median=1264.1 min=874.0 max=5032.1 p95=1466.4 p99=1537.8`。

**★ 我前两次都读错了层次**（都记在案，不删）：
* §10.19：把它当成**跨腿**统计量的离散 —— 用户第一次更正指出"不同运行本来就会变（环境）"；
* §10.20：改成**运行内窗口**统计量的漂移 —— 那是"运行期间有没有变化"，而**一条每次都很慢但很均匀的运行**会完美通过；
* **正解**：**一次运行里分布自身的离散度**，与"所有样本相同"（`min = median = p95 = p99 = max`）比。
  ⇒ 度量 = **比值**（`max/min`、`p95/median`、`p99/median`、`min/median`），**理想全是 1.00**。

**读数（p190 stress；完整表在高层 §5.3，工具 `spike_census.py --stability`）**

| 序列 | max/min | p99/med | **p95/med** | min/med | 判读 |
|---|---|---|---|---|---|
| `ul_pipeline` | 4.81 | 1.23 | **1.16** | 0.67 | 主体最紧 |
| `ul_gpu_pipeline` | 5.76 | 1.22 | **1.16** | 0.69 | 主体紧 |
| `ul_equalization_demod` | 10.94 | 1.28 | **1.19** | 0.47 | 主体紧 |
| `ul_time_frequency` | 19.42 | 1.27 | **1.24** | 0.13 | min 是"主机未等待"的退化样本（序列定义如此）|
| `ul_ldpc_decode` | 55.62 | 2.97 | **1.97** | 0.13 | 主体宽（= 载荷/码块数）|
| **`ul_channel_estimation`** | **118.74** | 2.62 | **1.73** | 0.22 | ★ **主体最宽 = 首要改进目标** |

**为什么 `ce` 是首要目标，以及它为什么属于本线**：`ce` 段 = `t2f_end`（UL 线程）→ `ce_end`（**池线程**），
**它包含线程间交棒 + 池排队**。所以 `p95/median = 1.73` 量的是**池线程的调度延迟**，
而不是 Metal 的算力（`eqd` 1.19、整跳 1.16 说明池跑起来之后并不慢）。
⇒ **"改进运行稳定性"的落点就在这里**：把池线程的交棒延迟压下去（优先级/调度/绑定/唤醒策略），
而本线的仪器已经能直接读出这个数（`ce` 的 `cpu=`/`tcpu=` + 这条比值）。

**登记（第三次，前瞻）**：主判据 = `p95/median` 与 `p99/median` 不超过登记带（高层 §5.3 表）；
`max/min`/`max/median`/`min/median` 一并打印但**不设阈值** —— 低 min（跳过的样本）与高 max（尾部）
是两种病，尾部仍由事件表归因。`ldpc` 不判（载荷）。**前两次登记作废，理由写明**。

**仪器不变、不需新腿**：比值由探针已经打印的六个统计量算出（`spike_census.py --stability`）。
`OCUDU_UL_STABILITY_WINDOWS`（运行内窗口漂移）**保留为辅助读数** —— 它回答"运行期间有没有变"，
与"分布散不散"是两个问题，两者都有用，但**判据是这个比值**。

### 10.23 2026-10-01 —— 目标定调（**压 `max`**）与为此补的两件仪器

**用户定调**：`min/median/p95/p99` 已经可以，**主要毛病是 `max`**；任务 = 在 macOS 上调 PHY 线程的
**运行时优先级/调度策略**，让 **`max`（因而 `max/min`）尽可能小**；★ 并且明确：
**"是 max 小，而不是 min 大"** —— 这条是**反作弊**：比值只能靠降 max 改善。

#### (1) 尾部必须变成**率**，否则无法判定（否则这套实验做不了）

`max` 是重尾里的**一次抽样**（§10.4 已立）：两条腿的 max 4.2 ms vs 4.8 ms **证明不了任何事**。
所以现在 `[ul_timing_events]` **总是**打印尾部率（分子分母同源）：

```
rx  : 1 of 391323 receive(s) above the 1000 us floor = 0.0003%
ce  : 3 of 38274 sample(s) above the 1000 us floor = 0.0078%
dl  : 21 hand-over(s) below the 500 us margin floor (denominator = [dl_tx_slack] transmissions=)
```
* 分子的计数器**本来就有**，缺的是**分母**与打印；
* 四位小数是刻意的：这些尾巴是"几万分之一"，一位小数会把 0.0078% 与 0.078% 都写成 `0.0%`；
* 序列没有样本时打印 `0 of 0 … = -`（"开着但没有样本"与"关着"不是一回事，与"0%"也不是一回事）。

#### (2) 窗口视图补上**每窗超阈计数** → 腿内 A/B 可判

```
ul_rx_wait  n=447651  median[34.0 ×8] p95[38.0 ×8] over 1000us[ 0 0 0 0 3 1 0 0 ]  worst window vs whole run: 0.0%
```
（该腿 `max=9097 µs`，4 个超阈样本全在第 5/6 窗。）
⇒ **腿中切换优先级档**（`taskpolicy`）时，前四窗与后四窗的**计数**可以直接比 —— 这是控制环境噪声的唯一办法，
比"两条腿比 max"强得多。

#### (3) 判据登记（高层 §5.4）

主判据 = **`max` 下降 + 尾部率下降**；**反作弊 = `min` 不得上升**（min 升而 max 不降 ⇒ 不算改进，要报出来）；
反例 = `median/p95/p99` 不得变差、`gaps=0`、契约 MET、门不降档；**载荷/regime 不可比就不可判**。

#### (4) 臂的顺序

① `taskpolicy -l 5`（零代码；★ 唯一能覆盖 **UHD 自己线程**的杠杆，`rx_wait` 的尾正在那里）；
② `OCUDU_SCHED_ATTR_QOS=1`（线程从第一条指令就在目标档）；③ Mach 时间约束（最后手段，需标定 + 预登记回退）。

### 10.24 2026-10-01 —— ★ 臂 ① 第一次尝试（p191）**作废：被我自己在本机的构建污染**（纪律新增一条）

**配方**：`p191-n78-tier5`（stress，22:03:44 起），第 5 分钟（22:08:00）切 `taskpolicy -l 5`。
**结果**：档位切换本身**成功**（`rc=0`，`epoch_ms` 也打出来了；脚本里那句提示因反引号被当成命令而报错，已修 —— 纯提示行）。

**为什么这条腿作废**（全部读数）：

| 证据 | 读数 |
|---|---|
| 尾部事件的**时间分布** | 切档前（22:04–22:06）**9** 个；切档后到我构建前（22:08:23–22:08:59）**3** 个；**22:09:12–22:09:21 一小簇 ~26 个** |
| 每窗超阈计数（`ul_rx_wait` >1 ms）| `0 0 0 1 1 0 2 `**`19`** ← 最后一窗独占 19 个 |
| 每窗超阈（`ul_gpu_pipeline` >2 ms）| `11 0 0 0 1 0 2 `**`15`** |
| 那一瞬间的 `load1` | 3.8 → **5.30 / 5.59**（14 核）|
| ★ 我那一刻在做什么 | **`cmake --build … -j 8`**（提交"反引号修复"时重打三段戳）——22:09:12 起，正是那一簇 |

⇒ **污染源是我自己**。这条腿**不得**用于 A/B（与 p182_1726、p187_2040 同类）。

**★ 纪律新增（本线第 11 条）**：**腿在跑的时候，本机不许跑构建 / 测试 / loopback / 任何 `-j`**。
动手前先 `pgrep -x gnb`（有输出就什么都别跑）。理由与 §10.3 同源：这台机器**对干扰没有余量**，
而腿是用户用手机+电台的时间换来的。

**A/B 配方的三处修正**（下次飞用）
1. **切档点放在 50%**（本次切在 ~77%，"后段"只剩 1.4 分钟，与"前段"5 分钟不可比）；
2. **腿长 ≥ 10 分钟**（前后各 ≥5 分钟、各覆盖 4 个等样本窗口）；
3. **飞腿期间本机保持空闲**（我也停手）。

**这条腿仍然有效、且值得记的两个读数**（与尾部无关的部分）：
* 切档**没有**改变主体（窗口 median 切档前后都在噪声内：`ul_pipeline` 1329→1305、`ul_gpu_pipeline` 1257→1246）；
* `ul_ldpc_decode` 窗口 median 63→65→66→44→51→57→50→55（载荷在腿内变化），第四次印证它承载载荷。

### 10.25 2026-10-01 —— ★★ 臂 ① 判定：**档位无效**；`max` 的元凶是**整进程冻结**（不在我们线程里）

#### (1) 腿 p192（stress，18.5 min，切档点 = 正中 9.5 min）的 A/B

| 读数 | 切档前（14:11:09–14:20:39）| 切档后（14:20:40–14:28:59）| 判读 |
|---|---|---|---|
| 每窗超阈（`ul_pipeline` >2 ms）| 11 0 1 2（4 窗 = **14**）| 4 6 1（3 窗 = **11**）/ 停机窗 **16** | 每窗 **3.5 vs 3.7** → 无差别 |
| `ul_gpu_pipeline` >2 ms | 14 0 0 2 = 16 | 3 8 1 = 12 / 停机 **15** | 每窗 **4.0 vs 4.0** → 无差别 |
| `ul_rx_wait` >1 ms | 3 7 2 2 = 14 | 6 2 1 = 9 | 每窗 3.5 vs 3.0 → 无差别 |
| **反作弊：`min` 不得上升** | — | `ul_pipeline` min **870 µs**（对照 p190 884）| **未上升** ✔ |
| 反例：`median/p95/p99` 不得变差 | — | 1302 / 1522 / 1607（p190 1328 / 1545 / 1634）| **未变差** ✔ |

⇒ **`taskpolicy -l 5`（进程级延迟档）对这个尾部没有可测效果。**

#### (2) ★★ 元凶：**整进程冻结**（`ivcsw_rate` 掉到自身基线的 1/10–1/16）

新工具 `wip/freeze_census.py`（每条事件与该腿**自己的**切换率基线并列，并给**本地时刻**）：

```
p192  leg baseline: 183.18 involuntary switch(es)/ms
  eqd#1  11571us  cpu=4.57ms  tcpu=0.08  ivcsw=11.58/ms (0.06x)  22:22:10.831  STALLED
  rx#1   10998us  cpu=5.54ms  tcpu=0.41  ivcsw=16.82/ms (0.09x)  22:22:10.831  STALLED
  rx#2   10810us  cpu=2.35ms  tcpu=0.22  ivcsw=17.02/ms (0.09x)  22:21:23.902  STALLED
  eqd#2   9289us  cpu=2.50ms  tcpu=0.08  ivcsw=13.13/ms (0.07x)  22:22:21.724  STALLED
  rx#3    8301us  cpu=1.69ms  tcpu=0.20  ivcsw=12.05/ms (0.07x)  22:22:21.724  STALLED
```
**3 次冻结**（22:21:23 / 22:22:10 / 22:22:21），每次在 rx、ce、eqd 上**同时**出现 ⇒ 同一事件；
而**内核根本没在跑这个进程**（切换率 1/10、CPU 0.1–0.5 核）。

**跨腿对照：冻结的大小 = 该腿 `max` 的大小**

| 腿 | 时长 | 冻结次数 | 最大冻结 | 该腿 `max` |
|---|---|---|---|---|
| p190 stress | 3 min | **0** | — | 2.6 ms |
| p189 default | 3 min | ~3 | 3.7 ms | 3.7 ms |
| p188 stress | 3 min | 1 | 2.1 ms | 2.1 ms |
| p187_2040 | 3.6 min | 1（同秒 3 条）| **14.7 ms** | 14.7 ms |
| **p192** | 18.5 min | **3** | **11.6 ms** | ce 11.2 / eqd 11.6 |

⇒ **`max` = 该腿最大的一次冻结**；`min/median/p95/p99` 基本不动（3 个样本 / 34 万）
—— 这正是用户说的"只有 `max` 差别太大"。

#### (3) ★ 冻结瞬间撞上**系统守护进程的活动爆发**（零腿成本：用事件给的时刻查系统日志）

| 冻结瞬间 | `log show` ±2 s 里最活跃的进程 |
|---|---|
| 22:21:23.902 | `ContinuityCaptureAgent` 156 · `bluetoothd` 207 · `SCIM_Extension` 88 |
| 22:22:10.831 | `ContinuityCaptureAgent` 52 · `bluetoothd` 50 · `launchd` 87 |
| 22:22:21.724 | `launchd` 93 · `sandboxd` 38 · `runningboardd` 31 |
| **22:24:13.656** | **`deleted` 2619 · `biomesyncd` 925 · `mobileassetd` 421 · `BiomeAgent` 392**（该 2 s 约 4000 行，其它瞬间只有几百行）|

* 前两次 = **Continuity/BLE 发现**（日志里正在发现用户的 iPhone 12PM 与 iPad Pro —— **就是被测那部手机**）；
* 第三次 = `launchd`/`sandboxd`/`runningboardd` 的**进程创建风暴**；
* 第四次 = **Biome/mobileasset/`deleted` 的系统维护风暴**（也正是 10.1/6.5 ms 那两条 CE 所在的一秒）。
* ⚠ 相关性不是因果：但 4/4 个瞬间都对上，且日志量级差 10 倍。

#### (4) ⇒ 对本线任务的含义

**线程优先级/调度策略无法修复"内核没在跑这个进程"**（没有可运行的线程可供调度）：
1. **臂 ①（进程级档位）判定无效**；
2. `max` 的可行下降路径是**移除干扰源**（机器配置），不是提高优先级；
3. 可检验：**下一条腿把机器安静下来**（蓝牙关、Continuity/Handoff/Sidecar 关、Spotlight 索引关、不开其它应用/备份），
   配方不变，用 `freeze_census.py` 数冻结：
   * 归零 ⇒ `max` 应降到 "BUSY" 档（eqd ~3–4 ms、ce ~1 ms），且 min/median/p95/p99 不变；
   * 仍在 ⇒ 下一个嫌疑是 USB/GPU 驱动，那就不是调度能绕开的。

### 10.26 2026-10-01 —— ★★ 臂 0（安静机器）**成功**：大冻结消失，`max` 降 3–10×，且 `min` 未上升

**干预**：用户关掉蓝牙、接力（Handoff/Continuity）、Spotlight 索引；其余配方与 p192 完全相同
（`p193-n78-quiet`，stress，HEAD `298fde6525`，12 min / 701 s 接收活动 / 1 403 个 slot·秒级样本）。

#### (1) 主判据：`max` 与尾部率

| 序列 | p192（切档腿）| **p193（安静腿）** | 判读 |
|---|---|---|---|
| **`ce` max / 尾部率** | 11 185 µs / 0.0027% | **1 120 µs / 0.0004%** | **max ↓10×，率 ↓7×** ✔ |
| `eqd` max / 率 | 11 571 / 0.0038% | 3 293 / 0.0012% | ↓3.5× / ↓3× ✔ |
| `t2f` max / 率 | 2 504 / 0.0009% | 2 183 / 0.0004% | 率 ↓2× ✔ |
| `rx_wait` max / 率 | 10 998 / 0.0001% | **2 442 / 0.0001%** | max ↓4.5× ✔ |
| `ul_pipeline` max | 7 629 | 3 980 | ↓1.9× |
| `ul_gpu_pipeline` max | 4 934 | 4 240 | ↓ |

**冻结普查**（`freeze_census.py`）：

| 腿 | STALLED 事件 | 最大冻结 | 该腿最大事件 |
|---|---|---|---|
| p192 | **3**（8.3 – 11.6 ms）| 11.6 ms | eqd 11.6 ms **STALLED** |
| **p193** | 3（**1.7 – 2.2 ms**）| 2.2 ms | eqd 3.3 ms **BUSY** |

⇒ **大冻结消失**（8–11.6 ms → 无），剩下的是 1.7–2.2 ms 的小停顿；该腿最大的事件已经是**进程有 CPU 的 BUSY 事件**。

#### (2) 反作弊与反例（**用户明确要求的那条**）

| 检查 | p192 | **p193** | 判定 |
|---|---|---|---|
| **`min` 不得上升** | pipeline 870 / gpu 861 / eqd 323 µs | **832 / 824 / 223 µs** | **未上升，反而更低** ✔ |
| `median` 不得变差 | 1302 / 1248 / 685 / 48 µs | **1281 / 1231 / 668 / 46** | 全部略优 ✔ |
| `p95`/`p99` 不得变差 | — | 全部略优（如 pipeline p99 1607→**1573**）| ✔ |
| `gaps` / 契约 | 0 / MET | **0 / MET** | ✔ |

（`min` 下降是好事：说明没有靠"消灭快样本"来改善比值 —— 与用户"是 max 小而不是 min 大"的要求一致。）

#### (3) 边界（必须一起读）

1. **各一条腿**：效应量大（3–10×）且机制明确（大冻结消失），但**需要第二条安静腿确认**；
2. **载荷形态不同**：p192 TBS 中位 1569 B vs p193 **1089 B**（UL 速率相近 4.74 vs 4.67 Mbit/s）
   ⇒ 依赖工作量的序列（`eqd`、`ldpc`、整跳）**不是严格同工**；**固定工作量的序列（`ce` 每 slot 固定、`t2f` 每 slot 固定、`rx_wait` 与我们的工作量无关）承载这条结论**；
3. **`ldpc` 的尾部率反而高 2.4×**（20 vs 9；TBS 更小、中位更快却更多超阈）—— **不解释**，且本就不在本线判据内（随载荷）；
4. **小冻结（1.7–2.2 ms）仍在** ⇒ 下一个嫌疑是 USB/GPU 驱动或其它系统活动，不是我们能调度的；
5. 门的 1 条 FAIL 是 **`AT/BELOW 0 = 11 > 10`**：该腿有 **1 402 658** 次交接（12 min），
   而 p190 只有 377 989 次 —— **按率算安静腿更好**（0.00078% vs 0.00106%），是**计数型界限对腿长敏感**的问题（见 (4)）。

#### (4) 派生的第二条待裁决：把 `AT/BELOW 0` 的界限改成**率**

`AT/BELOW 0 <= 10`（§5.2 登记的 VALIDITY 绑定）是**计数**，而计数随腿长线性增长：
* p190（3 min，378 k 交接）：4 → 0.00106%
* p192（18.5 min，667 k）：15 → 0.00225%
* **p193（12 min，1 403 k）：11 → 0.00078%**（最好的一条，却 FAIL）
⇒ 建议（**前瞻**）改为**率**：`AT/BELOW 0 / transmissions <= 0.0025%`（= 近期三条腿最坏率的 1.25 倍取整）。

★ **已裁决（2026-10-01 晚，用户）**：同意改为率。落地在 `leg_gate.sh`（判定行 + 推导注释）与高层 §5.2。
**反向臂已实测**（判据必须能打红，用**闸门自己的 in-stream 口径**逐条腿复核）：

| 腿 | in-stream `AT/BELOW 0` 率 | 判定 |
|---|---|---|
| **p195_0703** | **0.20998%** | ❌ FAIL |
| **p182_1726** | **0.02555%** | ❌ FAIL（该腿共 FAIL 4 项）|
| **p177** | **0.01418%** | ❌ FAIL |
| **p176** | **0.00270%** | ❌ FAIL（刚过界）|
| **p178** | **0.00263%** | ❌ FAIL（刚过界）|
| p179 / p191 / p192 | 0.00235% / 0.00225% / 0.00085% | ✅ |
| p189 / p190（交付对）| 0.00077% / 0.00084% | ✅ |
| p193 / p194（安静长腿）| 0.00078% / 0.00047% | ✅ |

★ **更正我先前的说法**：我曾写"p187_2040 = 0.0048% 会打红"——**错了**。那是把 `[dl_tx_slack]` 行里
**另一个口径**的数字（含 teardown 尾巴）当成了 in-stream 值；闸门口径下 p187_2040 是 **0.00084%，通过**。
教训：反向臂必须用**判据自己的读数**去扫，不能另找一个"看起来像"的数字。

旧计数界限在同一批腿上的表现正相反：**p193 是率最好的一条，却是唯一 FAIL 的**——这就是把它换成率的全部理由。

### 10.27 2026-10-01 —— 第二条安静腿（p194）：**大冻结类复现（0 次），`ce` 这一条不复现但可归因于载荷**

`p194-n78-quiet`（stress，12 min，HEAD `6807c405cb`），条件与 p193 相同（蓝牙/接力/Spotlight 关）。

#### (1) 四条腿对照

| 指标 | p190（响，3 min）| p192（响+切档，18.5 min）| **p193（静，12 min）**| **p194（静，12 min）**|
|---|---|---|---|---|
| 载荷 / TBS 中位 | 4.59 Mbit/s / 3072 B | 4.74 / 1569 B | 4.67 / **1089 B** | **6.87 / 1857 B** |
| `ce` max | 1 769 | 11 185 | **1 120** | 1 815 |
| `eqd` max | 3 546 | 11 571 | **3 293** | **2 905** |
| `rx_wait` max | 2 552 | 10 998 | **2 442** | **2 116** |
| `t2f` max / 率 | 1 193 / — | 2 504 / 0.0009% | 2 183 / 0.0004% | 1 976 / **0.0000%** |
| `ul_pipeline` max | 4 254 | 7 629 | 3 980 | **3 577** |
| `gpu_pipeline` max | 5 032 | 4 934 | 4 240 | 4 812 |
| **大冻结（≥8 ms）** | 0 | **3**（8.3/10.1/11.6）| **0** | **0** |
| 小停顿（1.7–2.2 ms） | 0 | 0 | 3 | 2 |
| 该腿最大事件 | eqd 3.5 BUSY | eqd **11.6 STALLED** | eqd 3.3 **BUSY** | eqd 3.2 **BUSY** |
| 门 | — | —（臂）| 9/10（`AT/BELOW 0=11`>10）| **10/10** |

**统计口径**：大冻结（≥8 ms 的整进程停顿）在**响机器**的腿里共 **4 次 / ~31 min**（p187_2040 1 次、p192 3 次），
在**安静机器**的腿里 **0 次 / 24 min**（p193+p194）。Poisson 下 P(0 | 期望 3.1) ≈ 4.5% ⇒ **有信号，但不是铁证**。

#### (2) 复现的与不复现的（必须分开说）

* ✅ **复现（固定工作量的序列）**：两条安静腿的 `rx_wait` max 2.1/2.4 ms、`eqd` 2.9/3.3 ms、`t2f` 2.0/2.2 ms
  且 `t2f` 超阈 **0 次**；两者的**最大事件都是 BUSY**（进程有 CPU），而响腿的最大事件是 STALLED（冻结）；
* ⚠ **不复现**：`ce` 的超阈计数 p193 = **1**、p194 = **7**（率 0.0004% vs 0.0027%，后者与 p192 相同）。
  **但两条安静腿不是同工**：p194 的 UL 载荷 **6.87 Mbit/s**（p193 4.67）、TBS 中位 **1857 B**（p193 1089）、
  ≥1000 B 占 85.2%（p193 65.7%）。`ce` 的窗口 = `t2f_end`→`ce_end`，**含池交棒/排队**，
  池更忙则这段更长 ⇒ **p194 的 ce 计数高与载荷一致，不能记在"安静"头上**。
  另外 p194 的 7 条里有 **4 条是同一瞬间**（15:22:10.061–062）= 1 次事件，即 3 次事件。
* ⚠ `ldpc` 超阈率在四条腿间摆动 0.0019%–0.0081%（载荷驱动，本就不判）。

#### (3) ★ 残余小停顿的归因：仍是守护进程（机制闭环）

p194 剩下两次 1.7–2.0 ms 的停滞，去系统日志里查它们各自的时刻：

| 停滞瞬间 | `log show` ±1 s 最活跃进程 |
|---|---|
| 23:20:36 | `runningboardd` 60 · `launchd` 23 |
| 23:16:05 | `mDNSResponder` 27 · `launchd` 27 · `runningboardd` 21 |

⇒ **机制自洽**：`max` = 该腿最大的一次"守护进程造成的整进程停顿"；
**关掉重干扰源（Continuity/BLE/Biome）后，8–15 ms 那一类消失，只剩 `launchd`/`runningboardd`/`mDNSResponder` 带来的 1.7–2.2 ms**。
四级台阶因此是：**ce 1.1–1.8 ms · eqd 2.9–3.3 ms · 小停顿 1.7–2.2 ms · 大冻结 8–15 ms（已消除）**。

#### (4) 反作弊与反例（两条安静腿）

| | p193 | p194 |
|---|---|---|
| `min`（不得上升）| 832 / 824 / 223 µs | 878 / 866 / 321 µs |
| 对照 p192 基线 | 870 / 861 / 323 | 同左，差 ≤1% |
⇒ **`min` 没有实质上升**（≤1%，且仍低于 p190 的 884/874/324）✔
`median`/`p95`/`p99` 全部在 ±2% 内或更优 ✔；`gaps=0`、契约 MET ✔。

### 10.28 2026-10-01 —— 收口：现象、结论、未知，以及**对"能不能免疫"的回答**（含对 §10.25 说法的更正）

高层 §4bis 是本线的结论节；这里放**证据与推理**，以及两处必须写下来的更正。

#### (1) ★ 更正：§10.25 把指纹叫"整进程冻结"，**不够严谨**

`ivcsw_rate` 掉到基线 1/10 + 进程 CPU 掉到 0.1–0.5 核，**只能证明**"这段时间进程几乎没有可运行的线程"。
以下三种机制**都会**产生同一指纹：
1. **进程被挂起**（`task_suspend`/stop-the-world：`sample`、`spindump`、调试器、系统托管）；
2. **线程阻塞在内核**（USB/GPU 驱动、内存压缩、锁）——线程不可运行，自然没有被抢占；
3. **CPU 被抢**（可运行的线程排队等核）。

**为什么这次仍要写"停顿"而不是"被抢"**（即排除③的证据）：
* `load1` 只有 **2.7–5.6 / 14 核**（机器不忙）；
* 进程已是 `USER_INTERACTIVE` + `boosted`（`taskinfo` 的 `req other=boosted`）；
* **进程级延迟档改了没用**（§10.25(1) 的腿内 A/B）；
* 停顿**跨子系统同时发生**：`rx`（USB 收）与 `ce`/`eqd`（**池线程，与电台无关**）在同一毫秒停顿
  ⇒ 不是"某个 IO 路径慢"，而是**全进程一起受影响**。
* **但也排除不了①②**：事件里进程 CPU 并非 0（0.5–1.8 核），所以**不是全程挂起**；而"阻塞在驱动"要求所有线程同时阻塞，也未被直接证明。
⇒ **判定方法（尚未实施）**：探针加一个 **1 ms 看门狗线程**（env 门控、只记录迟到），
   * 看门狗与流水线**一起**停 ⇒ ①挂起/stop-the-world；
   * 只有流水线停、看门狗正常 ⇒ ②阻塞在驱动；
   * 两者都正常而流水线线程没被调度 ⇒ ③被抢（那时优先级才有意义）。

#### (2) 为什么"提高优先级"解决不了（对用户问题的完整回答）

| 机制 | 优先级有用？ | 依据 |
|---|---|---|
| ① 挂起 / stop-the-world | ❌ | 没有可运行的线程可供调度 |
| ② 阻塞在驱动/内核 | ❌ | 线程在等内核；驱动在自己的优先级上跑，用户态 QoS 不参与 |
| ③ CPU 被抢 | ✅ | **但已排除**：机器空闲、进程已最高档 + boosted、档位实验无效 |

**唯一能向内核"预留 CPU"的 macOS 机制 = Mach 时间约束（P4）**：
它给线程每周期一个保证的计算预算，内核会为它抢占别的活儿 ⇒ 这是"免疫"方向上唯一还有意义的实验。

> ★★ **2026-10-01 晚更正（读数见 §10.29）**：本段原来写"代价（实测）：与 QoS 互斥 …
> 之后 `pthread_setschedparam` 返回 EINVAL；且 2026-09-01 有回归史。**因此只能单线程试**"。
> 现在有了微基准读数，两处要改：
> ① **丢 QoS 档这个代价本身可以接受**——QoS 档在唤醒延迟上几乎无收益（p50 102.9 → 103.1 µs，
> `max` 只降 1.6×），而时间约束是 **700×**（5358 → 9.9 µs）；互斥是**双向**的（反方向 `set_qos`
> 返回 EPERM），要害是"显式调度制度黏且不可逆"，不是某个 errno；
> ② **2026-09-01 的病因不是参数形状、也不是丢 QoS 档**（两者都被微基准否掉），唯一未被否掉的是
> "给所有线程发同一参数把 FIFO 次序拍平"。
> ⇒ 单线程试的理由从"代价太大"改成"**次序问题只能由真腿判定**"，设计规则见 §10.29(5)。

**其余候选（未试，按价值排序）**：`taskpolicy -a`（按"应用"给资源策略）；`caffeinate -dimsu` 防电源/显示状态切换
（假设：停顿与状态切换有关，未证）；减少自身线程/池并发（与交付形态有关）。

#### (3) 结论一句话（写给后来人）

> 在这台机器上，PHY 处理线程的**处理时间本身是稳的**（同一次运行内 `t2f` 8 个窗口中位 489.4–491.8 µs）；
> **`max` 是外部停顿**（8–15 ms 那一类来自系统守护进程：接力/BLE 发现、`launchd`/`runningboardd` 进程风暴、
> `deleted`/`biomesyncd`/`mobileassetd` 维护风暴）；**QoS 档改不动它**（已用腿内 A/B 证否），
> 但 ★ **时间约束改得动（§10.29 微基准：700×）**——"优先级全都无效"是不精确的说法，要按机制拆开；
> **把机器安静下来则有效**（两条腿把那一类清零，`max` 从 11 ms 级降到 2–3 ms 级，且 `min` 未上升）。
> 残余的 1.7–2.2 ms 小停顿仍来自 `launchd`/`runningboardd`/`mDNSResponder`，其内核路径（挂起 vs 驱动阻塞）
> 需看门狗实验判定。★ 另一条需拆开的说法：**"外部停顿优先级无法作用"对"CPU 被抢"这一类不成立**
> ——微基准里时间约束正是在 CPU 被抢（2× 超订）时把尾延迟从 5358 µs 压到 9.9 µs 的。

### 10.29 2026-10-01 —— ★★★ 2026-09-01 回归的重新评估（按用户指示）：**参数形状与丢 QoS 都不是病因，时间约束反而是这台机器上最强的杠杆**

用户指示："至于你提到的 2026-09-01 回归，我们应该重新评估并找到不同的解决方案。"
本节就是那次重新评估。**全部读数来自 `wip/sched_microbench/`（9 个可直接编译运行的探针 + `run_all.sh`），
不是推理**。之所以先用微基准：真腿一次只能回答一个问题，而这里要排除的是**五个候选病因**。

#### (1) 当年那次提交到底改了什么（代码为证）

`8df59741c6` 同时做了两件事，随后**两件一起回退**，所以从未归因：

| # | 改动 | 当时的有效调度制度 |
|---|---|---|
| ① | `apply_worker_thread_scheduling()` 里自动调 `bind_thread_to_performance_core()` ⇒ 对**每个** RT worker 施加 `default_rt_time_constraint` = `{period=1 ms, computation=1 ms, constraint=1 ms, preemptible=true}` | `tc=SET(1 ms/1 ms/1 ms)` |
| ② | `posix_realtime_priority_is_enforceable()` 返回 false ⇒ **跳过 `pthread_setschedparam`** | `posix=OTHER/31` |

对照（历史臂的有效制度，来自本文件的实测注释 §10.5）：`posix=FIFO/44..46`，`qos=0` ——
**注意历史臂本来就没有 QoS 档**（POSIX 调用会把它抹掉，见 §10.5），所以"回归 = 丢了 QoS 档"这个
说法从一开始就站不住：**两边都没有 QoS 档**。

#### (2) 逐条排除：五个候选病因，四个被微基准直接否掉

| # | 候选病因 | 探针 | 读数 | 结论 |
|---|---|---|---|---|
| ① | 参数形状 `period==computation==constraint`（**100% 占空**）"把机器抢死了" | `04`/`03` | 历史原版形状：wait p50 **4.9** / max **12.1** µs；标定形状 `1000/200/400`：4.4 / 11.2 µs；16 个 spinner 的吞吐在"无约束 / 满占空"两臂下是 **4.337 / 4.445 G-iter/s**（没被抢） | ❌ **否**：形状无罪，也没抢别人 |
| ② | 参数"标定不准"（实际用量超过申报的 `computation`/`constraint`）⇒ 被内核罚停 | `03`/`05`/`06` | 申报 1000 µs 而**持续**每周期用 4600 µs：wait p50 **3.2** / max **9.7** µs，**0 次超期**；周期内 3 ms 突发：3.8 / 16.0 µs，0 次超期 | ❌ **否**：`computation` 不是硬上限，只是预留声明 |
| ③ | 丢了 QoS 档 ⇒ 唤醒变慢 | `02`/`07` | 无档位 p50 102.9 / max 5358.1 µs；`QoS USER_INTERACTIVE` p50 **103.1** / max 3274.0 µs；`SCHED_FIFO 46` p50 58.5 / max **91.2**；时间约束 p50 **3.2** / max **9.9** | ❌ **否**：**QoS 对唤醒延迟几乎无影响**（p50 不动、p99 反而更差、max 只降 1.6×） |
| ④ | 时间约束这个制度本身不如 FIFO（换制度导致回归） | `07` | 同上：TC 比 FIFO **p50 好 18×、max 好 9×**；FIFO 之后再施加 TC 两制度并存（`posix=FIFO/46` + `tc=SET`），仍 3.2 / 8.2 µs | ❌ **否，且方向相反**：那一年是把**更强的机制换成了更弱的** |
| ⑤ | 给**所有** worker 申报**完全相同**的 `1 ms/1 ms/1 ms`，把原本由 FIFO 优先级（44/46/…）编码的**线程间次序拍平了** | `08` | 12 个 hog 全是 TC：hog 松 / A 紧（**差异化**）max **78.7** µs；hog 松 / A 松（**同参**）max **571.8** µs | ⚠️ **唯一未被否掉**（但只有一组样本，**不作为定论**；主因仍是 A 自身申报的绝对紧度） |

补充一条**唯一有害的形状**：`constraint < computation`（畸形请求）⇒ wait p50 **792.8** / max **7277** µs。
历史原版三者相等，**不属此列**。另一条坏例子是"申报 4% 占空却要 30%"的**量级错误**（申报 20 µs、
实用 150 µs）⇒ 中位罚停 **75 366** µs。⇒ **参数规则由此确定：`constraint ≥ computation` 硬性；
`computation` 宁大勿小。**

#### (3) ★ 结论：那次回归与本线的目标其实同向

用微基准把①–④排除后，剩下的解释只有⑤（次序被拍平），而它指向的**不是"别用时间约束"**，而是
**"别给所有线程发同一个参数"**。同时③④给出了一个更强的、此前不知道的事实：

> **在这台机器上，`max` 不是"优先级"能改动的，但**时间约束**能——幅度 700×（5358 µs → 9.9 µs），
> 而且它是**唯一**能向内核预留 CPU 的机制。QoS 档在这条指标上基本没有收益，`SCHED_FIFO` 只有
> 部分收益（尾紧但 p50 有 58 µs 的定时器合并代价）。**

⇒ 所以 P4 的"代价"（与 QoS 互斥、必然丢掉档位，§10.29(4)）**是划算的**：用一个在这条指标上
没有收益的东西，换一个 700× 的东西。

#### (4) ★★ 更正 §10.28(2) 与 §10.5 的两处说法（都是实测改写的）

1. **互斥是双向的，且方向要写对**（探针 `01`，权威回读用 `thread_policy_get(..., get_default=FALSE)`）：
   * `set_qos(UI)`（成功，qos=33）→ `thread_policy_set(TC)` ⇒ **qos 变 0**（约束抹掉档位）；
   * `thread_policy_set(TC)` → `set_qos(UI)` ⇒ 返回 **1 = EPERM**，档位停在 0（**不给**）。
   * §10.28(2) 写的"之后 `pthread_setschedparam` 返回 EINVAL"应更正为：**反方向也覆盖不回去**
     （探针 `07` 实测：TC 之后再调 `pthread_setschedparam(SCHED_FIFO,46)`，回读仍是 `posix=OTHER/31`）。
     要害不是某个 errno，而是**显式调度制度是黏的、不可逆**。
2. **"丢 QoS 档"不再算作 P4 的阻塞性代价**：§10.28(2) 因此写的是"只能单线程试 + 预登记回退"，
   现在依据 `02`/`07` 可以更进一步——**这个代价本身可以接受**；单线程试的理由从"代价太大"改成
   "⑤（次序拍平）只能由真腿判定"。

#### (5) 不同的解决方案 = P4（逐线程、按实测预算、可开关、带预登记回退）

由 (2) 的五条读数导出的设计规则（**逐条都有读数支撑**）：

| 规则 | 依据 |
|---|---|
| **逐线程**申报，禁止"给所有 worker 发同一个参数" | 候选⑤：同参对撞 max 571.8 µs vs 差异化 78.7 µs |
| `period` = 该线程自己的循环周期（UL 是 500 µs slot） | 探针 `04`：`500/200/400` 与 `1000/…` 都满效，周期对齐才有意义 |
| `constraint ≥ computation`（**硬性，违反即拒绝**） | 唯一有害形状：p50 793 µs / max 7.3 ms |
| `computation` ≥ 该线程实测每周期 CPU 的 p99.9（留余量），**宁大勿小** | `03`/`06`：报大不报小；报小到量级错误 ⇒ 中位罚停 75 ms |
| 默认**关**，`env` 显式指定"哪几个线程 + 什么参数"，并在日志里**回读** | 与 `OCUDU_SCHED_POSIX_RT`/`OCUDU_SCHED_ATTR_QOS` 同一纪律：同一二进制、两臂可比 |
| 预登记回退：单线程先飞；若随机接入（Msg3 contention resolution）出现 2026-09-01 那类异常 ⇒ 立即回退该臂 | 当年症状的**唯一**可复现入口是真腿，不是微基准 |

#### (6) 复现方法

```sh
# 需机器基本空闲（这些探针会开 2× 核数的 spinner；腿在飞时 run_all.sh 会拒绝运行）
bash doc_chinese/macos_thread_priority/wip/sched_microbench/run_all.sh
```

⚠ 方法学留档（`09_ARTIFACT_*`）：我第一版"突发惩罚"探针**测出** 2.02 ms 惩罚，那是**算术假象**
——突发 3 ms 超出了 1 ms 的周期，`mach_wait_until()` 的死期早已过期，`now - next` 自然等于超出量。
把突发塞回周期之内（`05`）后惩罚为 **0**。留档理由：这个错误的方向**恰好会"证实"我们想信的假设**。

### 10.30 2026-10-01 —— ★★★ P4 落地（默认关、逐线程），以及 loopback 上对 2026-09-01 的**复现与归因**：元凶是**范围**，不是约束

§10.29 用微基准排除了四个候选病因，把 P4 从"最后选项"升级为"下一步"。本节是它的落地记录，
以及**在兑现"只飞单线程"之前先做的那个便宜实验**：把各臂放到 loopback 台上跑（不需要电台、不需要
UE、不需要 sudo，18 秒一轮）—— 结果是那次回归第一次被**在本地复现出来**。

#### (1) 开关与语法（`OCUDU_SCHED_TIME_CONSTRAINT`，默认关 = 逐字节不变）

| 值 | 含义 |
|---|---|
| 不设 / `""` / `0` | **关**（交付默认）。不读环境以外的任何东西，不打印，行为与改动前逐字节相同 |
| `1` / `default` | **2026-09-01 那一臂原样复现**：每个**实时意图** worker 拿到同一个 `1 ms/1 ms/1 ms` |
| `NAME=P/C/K[;NAME=…]` | **逐线程**微秒值；`NAME=*` 匹配所有 worker（**含非实时意图线程**）；**同名精确匹配优先于 `*`，与书写顺序无关** |

畸形请求**一律拒绝且不施用**（`constraint<computation`：唯一被实测有害的形状；`period<constraint`：自相矛盾）。
拒绝不是静默的——每个被选中的线程打一行 `[sched_tc]`，所以"腿声称的臂"与"腿真正拿到的臂"不可能不一致。

#### (2) 臂在日志里可证：`[sched]` 新增 `tc=` 字段

`thread_sched_snapshot` 现在回读 Mach 时间约束（`thread_policy_get`，**`get_default` 必须在输入处置 FALSE**
才拿到真实策略，否则连被约束的线程都报"未设置"）。于是同一行里能区分两件以前字面相同的事：

```
[sched] thread=main_pool#0 … eff=USER_INTERACTIVE posix=OTHER/31 tc=none            cpu=0.011ms   ← 默认臂
[sched] thread=main_pool#0 … eff=UNSPECIFIED      posix=OTHER/31 tc=500/200/400us(duty=40%) cpu=0.029ms ← P4 臂
```

**档位被抹掉与"被约束"在同一行可见**——这正是 §10.29 测出的互斥关系在读数上的样子。

#### (3) loopback 三件证据（`wip/sched_microbench/`，`build/apps/gnb/gnb` 为当前 HEAD）

| # | 检查 | 结果 |
|---|---|---|
| ① | **两把钥匙**：关着旋钮跑一次，开着 `OCUDU_SCHED_VERBOSE` 跑一次，去掉 `[sched]` 行并把数字归一化后 `diff` | **完全相同**（"逐字节不变"成立）|
| ② | **逐线程精确选择**：`main_pool#0=500/200/400` | 11 条线程 **11 个不同 id**，其中**恰好 1 条**（`main_pool#0`）回读 `tc=500/200/400us(duty=40%)` + `eff=UNSPECIFIED`，**其余 10 条 `tc=none` + `eff=USER_INTERACTIVE`** ⇒ "单线程先飞"的前提成立 |
| ③ | 全量臂 `*=…` | 每条 worker 都打 `[sched_tc]`，回读 `eff=UNSPECIFIED` + `tc=…`（**连 `rt_intent=0` 的 `io_timer_tick` 也被覆盖**——这是 `*` 与 `1`/`default` 的差别）|

#### (4) ★★★ 复现：**全量臂在 loopback 上 3/3 崩溃，其余臂 3/3 干净**

全量臂在 18 秒内必崩，**同一句 FATAL**（电台缓冲区被写穿——生产者跑到消费者前面）：

```
OCUDU FATAL ERROR: Attempting to write samples [186382380, 186393900) that would overwrite unread sample 17405. buffer size: 2304000
```

| 臂 | 约束范围 | 干净 | 崩溃 |
|---|---|---|---|
| `off`（不设）| — | **3** | 0 |
| **`blanket`（`*=500/200/400`）** | 全部 11 条 worker | **0** | **3** |
| `single` | `main_pool#0` | **3** | 0 |
| `single_b` | `main_pool#0` + `radio` | **3** | 0 |

⇒ **病因是范围，不是约束本身，也不是参数值。** 这就是 2026-09-01 从未拿到的那句归因；
它同时解释了为什么当年"只回退、不归因"：那一臂把两件事（约束 + 范围）一起改了。

#### (5) 二分与参数变体：危险成员是 `lower_phy_*`

| 臂 | 约束的线程 | 干净 | 崩溃 |
|---|---|---|---|
| `pool5` | `main_pool#0..#4`（**`ce`/`t2f` 的载体，也就是本线真正想保护的那 5 条**）| **3** | 0 |
| `io2` | `io_timer_tick` + `io_broker_epoll` | **3** | 0 |
| **`lower3`** | `lower_phy_tx#0` + `lower_phy_rx#0` + `lower_phy_ul#0` | **0** | **3** |
| `lower3_min` | 同上，`1000/100/200` | **0** | **3** |
| `lower3_hist` | 同上，`500/500/500`（历史形状）| **0** | **3** |
| `lower3_loose` | 同上，`2000/50/100`（**只有 2.5% 占空**）| **0** | **3** |

**参数变体是决定性的**：`lower_phy_*` 在**四种完全不同的形状**下全部 0/3 —— 包括那个"几乎不要 CPU"的
`2000/50/100`（2.5% 占空）。所以**不是参数，是线程**：只要给这三条面向电台的线程施加约束，loopback 的
样本流就断（每轮都在同一处、同一句 FATAL，共 **0/12**）。而在同一批实验里，池线程 3/3、io 线程 3/3、
池 + radio 3/3 都干净。

真电台腿（p194）的 worker 集合与 loopback **完全一致**（11 条：5 池 + 3 lower-PHY + radio + 2 io），
所以这个模型是忠实的。**可用结论：本线要保护的池线程 `main_pool#0..#4` 可以约束（3/3 干净）；
面向电台的 `lower_phy_*` 三条不可以（0/12，跨四种参数）。**

#### (6) ★ 对 §10.29 的限定（必须写在同一条记录里）

§10.29 的微基准**只测了"一条线程 + 一群愚蠢的 spinner"**，因此它证明的是"**参数形状**本身无罪"，
**看不见真实软件里的线程间流控**。本节(4)补上的正是那一块：**同一个参数形状，范围一放大就崩**。
两条结论并不矛盾，它们是同一个问题的两个层次：

> 参数形状（`period/computation/constraint` 的取值）无罪 —— 微基准；
> **谁**拿到它（范围）才决定成败 —— loopback，3/3 vs 0/3。

#### (7) Ubuntu 台架抓到的两个**真实缺陷**（都是本节的代码/测试，不是笔误）

| # | 缺陷 | 教训 |
|---|---|---|
| 1 | 我把"回读成功且无约束 = 0"这条 **macOS 语义**断言在了两平台共用的用例里（Linux 上该字段设计为 `-1`）| 平台语义必须落在平台分支里 |
| 2 | ★ **`-1 / 1000 == 0`**（C++ 截断除法）⇒ "没有此读数"（-1）与"读到了但没有约束"（0）**在换算成 µs 后不可区分**——正是本项目"没有读数不许打印成 0"那条规则要防的 | 哨兵值必须有**专用访问器**（新增 `tc_readable()`），断言必须落在**原始 ns** 上 |

修完后台架：`BUILD_RC=0`、定向 **26/26**、全量 **8244/8244** 通过；本机 `ctest -L phy -j 1` 退出码 0。

#### (8) 仍然缺的一块：每线程每 slot 的 CPU（P4 参数的标定依据）

`ce`/`t2f` 的载体是 `main_pool#0..#4`，但**相位事件的 `tcpu=-`**（§10.15(3) 记的那个基线时序缺口），
所以"这条线程每 slot 要多少 CPU"**目前读不出来**，而 `cpu=` 是**进程**在整个窗口的 CPU（窗口 1.3–1.8 ms
却记到 4.4–6.9 ms，因为它是全进程口径）——**不能拿它当线程预算**。

⇒ 标定有两条路，飞行前必须先选一条（见高层 §10 待裁决）：
1. **先修 `tcpu` 的基线时序**（§10.15(3) 已设计好修法），飞一条安静腿**测出**每线程每 slot CPU，再登记参数；
2. 或者**先声明得宽**（微基准证明报大免费：持续超额 4.6×、92% 占空都不被惩罚），用 `period=500 µs`
   加一个保守的 `computation`，并接受"这不是标定值而是一个上界"。

无论哪条，**第一条腿只约束池线程（3/3 干净的那一组）**，且带 §10.29 的预登记回退。

★ **已裁决（2026-10-01 晚，用户）：走第 1 条** —— 先测出每线程每 slot CPU，再登记参数。落地见 §10.31。

### 10.31 2026-10-01 —— P4 标定仪器落地 + 预登记（**待飞**）

#### (1) 做了什么：`tcpu` 那条路走不通，于是加了一个**每线程每 slot CPU 记账**

用户选择"先修 `tcpu` 基线时序"。动手前先查了**为什么 p194 上它还是 `-`**，结论是**修不了**：
`attach_cpu_delta()` 只在 `base.thread_id == self.thread_id` 时填 `tcpu`（`ul_pipeline_probe.h` 第 878 行），
而 UL 池是**工作窃取**的 —— `ce` 窗口由"结束 `t2f` 的那条线程"开、由"结束 `ce` 的那条线程"关，
**两条线程不同**，所以池线程上这个字段是**结构性**的 `-`，不是时序问题。进程口径的 `cpu=` 也不能替代
（窗口 1.3–1.8 ms 却记到 4.4–6.9 ms，因为它是全进程）。

⇒ 新增 **`OCUDU_UL_THREAD_CPU`**（两把钥匙：`OCUDU_FLOW_PROBES` 编译 + 本变量非 0）：
每条线程在自己的 **slot 变化**处读一次**自己的**累计 CPU，把两次之间的差值记进本线程的
`count/sum/max` 与一个**对数直方图**（log2 分桶，40 桶）。一次样本 = "这条线程在两个相邻 slot 变化之间
烧掉的 CPU"，一条腿下来就是它的**每 slot CPU**；直方图让 **p99.9** 在有限状态里可读，`max` 给出要申报的值。
落点：`record_ce_end()`（每 PUSCH 一次，在池线程上，且**在相位门之前**，不继承它的开关）。

**代价与不变式**：关着（默认）只读一次环境变量就返回，**不读时钟、不注册、不打印** ⇒ 交付腿逐字节不变。
开着时每次地标一次 Mach 调用（`this_thread_cpu_ns()`，为此新增的**窄**接口：只要 CPU 计数器，
不连带 `this_thread_sched_snapshot()` 的另外四个调用）。

**单测**（`per_thread_cpu_accounting_files_one_window_per_slot_change`）用**已知 CPU 值**驱动，
不依赖时钟；OFF 臂断言"不注册也不打印"。
★ **反向臂第一次没能打红**：我把 slot 变更规则在 `_for_test` 钩子里**复制了一份**，测试跑的是副本 ——
把实现改成"每次调用都记一笔"后测试**照样通过**。重构成两边共用 `file_thread_cpu_boundary()` 后，
同一处破坏使 3 条断言变红（`slots=4` 而非 3、`5` 而非 4），并且**二进制指纹钉死**：
正确 `f5cdc32c…`（0 失败）→ 破坏 `f2f7569d…`（3 失败）→ 还原后**回到 `f5cdc32c…`**（0 失败）。
教训与 §10.29 的假象同一类：**测试若有一份自己的实现，它测的是那份实现**。

#### (2) 预登记：标定腿（**先飞这条，不含 P4 臂**）

```
### 预登记 2026-10-01 / p195-n78-quiet-calib
* 目的：测出**每条池线程每 slot 的 CPU**（P4 的 `computation` 的唯一依据），不改变任何调度
* 臂：**默认调度 + OCUDU_UL_THREAD_CPU=1**（纯读数；`OCUDU_SCHED_TIME_CONSTRAINT` **不设**）
* 配方：安静机器；LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml OCUDU_METAL_GPU_TIME=1
        OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_UL_TIMING_EVENTS=16 OCUDU_SCHED_VERBOSE=1
        OCUDU_UL_STABILITY_WINDOWS=8 OCUDU_UL_THREAD_CPU=1 LEG_LOGDIR=... 
* 长度：≥ 10 min（要覆盖到尾部：p99.9 需要足够多的 slot）
* 判据（读 `[ul_thread_cpu]`）：① 每条池线程都有 `slots` 且量级 = 腿长/slot 周期；
  ② 打印 `max` 与 p99.9；③ 报告里**不得**出现 `u_thread_cpu` 以外的行为差异（关掉重建一次做 diff）
* 产出：`main_pool#0..#4` 的 max/p99.9 ⇒ 登记进 (3) 的臂
```

#### (3) 标定腿 p195 的读数：仪器成立，**腿本身作废**，而它同时更正了我的参数取法

**仪器出数了**（712 s 的腿，旋钮登记行齐全）：

| 线程 | 窗口数 | mean | p99.9 | max |
|---|---|---|---|---|
| `main_pool#0` | 48 686 | 966.7 µs | 16 777.2 µs | 554 987 µs |
| `main_pool#1` | 48 680 | 995.8 | 16 777.2 | 931 332 |
| `main_pool#2` | 49 079 | 997.0 | 16 777.2 | 1 375 913 |
| `main_pool#3` | 48 822 | 999.3 | 16 777.2 | 1 575 272 |
| `main_pool#4` | 48 818 | 999.1 | 16 777.2 | 1 153 610 |

**结构读数（5 条线程一致，这才是可用部分）**：712 s ÷ ≈48 800 窗口 ⇒ 每条线程**约每 14.6 ms**
处理一次 PUSCH，每次烧 **≈1 ms CPU** ⇒ **单线程占空 ≈6.8%**（5 条合计 ≈0.34 核，与 p194 的进程画像相符）。

★ **腿本身无效**（它自己红了两项，并且撞上一次严重事件）——**不能用它当参照腿**：

| 读数 | p195 | p194（安静参照）|
|---|---|---|
| `AT/BELOW 0` 率 | **0.20998%**（2991/1 424 415）| 0.00047% |
| 电台样本连续性 | **gaps=1** | 0 |
| contract | **1 of 8 项未过** | MET |
| `[RF] Real-time failure` | **196 次（underflow）** | — |
| `recv max` | **1 513 766 µs（1.51 s）** | 2 442 µs |
| `load1`（尾部事件时刻）| **9.11** | ≈3.9 |

⇒ **`load1` 9.11 + 1.5 s 接收停顿 + 196 次 underflow**：机器在那一段并不安静（或发生了系统级事件）。
这条腿的读数只能当"结构性"参考，**不能当安静基线**；需要重飞一条干净的标定/对照腿。

★★ **更正我自己的预登记（(2) 那条写的"computation 按 max 取"是错的）**：`max`（0.55–1.58 s）与
`recv max`（1.51 s）是**同一次事件**的产物，不是"每 slot 需要多少 CPU"；把它填进 `computation` 会得到
一个荒谬的申报。**该覆盖的量是"每次激活的 CPU"（≈1 ms），周期该按激活节奏取**。据此选定：

* `period = 5000 µs`（线程的激活节奏 14.6 ms 之下取一档，且微基准里 `5000/1000/2000` 是被实测过的形状：
  周期内 3 ms 突发/持续 300% 超额 ⇒ wait p50 3.6 / max 12.7 µs，0 次超期）；
* `computation = 1000 µs`（= 实测的每次激活 CPU，**不是** max）；`constraint = 2000 µs`（`≥ computation` 硬性）；
* 范围**只写 5 条池线程**。

**loopback 预验**（本线协议：上电台前先过这道便宜闸门）：`pool5_cal = main_pool#0..#4 = 5000/1000/2000`
**3/3 干净**（同一批的 `off` 也 3/3 干净）。

#### (4) 预登记：P4 第一条腿（**对照腿因 p195 作废而要重飞**）

* 臂 A（对照）：**默认调度**，其余 env 与臂 B **逐字相同**（含 `OCUDU_UL_THREAD_CPU=1`）
* 臂 B（P4）：加
  `OCUDU_SCHED_TIME_CONSTRAINT='main_pool#0=5000/1000/2000;main_pool#1=5000/1000/2000;main_pool#2=5000/1000/2000;main_pool#3=5000/1000/2000;main_pool#4=5000/1000/2000'`
  （**只写这 5 条**；`lower_phy_*` 一律不写 —— loopback 上 0/12；`radio`/`io_*` 也不写）
* 长度 ≥ 10 min，安静机器，同样的停止方式（一次 Ctrl-C）
* 成功判据（本线的目标判据，先写后跑）：
      ① `ce`/`t2f`/`rx_wait` 的 **max 下降**；
      ② 同一次运行内的离散度（C3 带）**收紧**；
      ③ **`min` 不上升**（反作弊：只许 max 降，不许 min 涨）；
      ④ 闸门判据不退化（尤其 `AT/BELOW 0` 的**率**与 slip/recv 风暴、`gaps=0`、contract MET）
* ★ 回退条件（满足任一条即判该臂失败、回退到默认，并记进本节）：
      ① 随机接入 / Msg3 contention resolution 出现 **2026-09-01 那类**异常；
      ② `contract` 判据失败；
      ③ 出现写穿电台缓冲区一类 **FATAL**；
      ④ **`max` 未下降，或 `min` 上升**（用户 2026-10-01 晚追加的第 4 条）


### 10.32 2026-10-02 —— P4 第一对腿（3 min 配对短跑）：**臂安全、机制生效、无功效问题如预登记所料**

**配对**（用户跑；除 `OCUDU_SCHED_TIME_CONSTRAINT` 外 env 逐字相同，含 `OCUDU_UL_THREAD_CPU=1`）：

| | 臂 A（对照）| 臂 B（P4）|
|---|---|---|
| 腿 | `p195-n78-quiet-calib_1002_0729` | `p197-n78-p4pool5_1002_0734` |
| 时长 | 280.5 s | 297.5 s |
| 调度 | 默认（QoS `USER_INTERACTIVE`）| 池 5 条 `tc=5000/1000/2000us(duty=20%)`，**回读 `eff=UNSPECIFIED`** |

**臂确实生效**：`[sched_tc]` **恰好 5 行**，5 条池线程回读 `tc=5000/1000/2000us(duty=20%)`，
其余 6 条（`lower_phy_*`、`radio`、`io_*`）**未被选中**（`tc=none`）。

#### (1) 判据（预登记 §10.31(4)）

| 判据 | 读数 | 判定 |
|---|---|---|
| ① `ce`/`t2f`/`rx_wait` 的 max 下降 | **本长度下不可评**（见 (3)）| ⏸ 留空 |
| ② 同一次运行内离散度收紧 | 见 (2)：体内统计量基本相同 | ⏸ 无功效 |
| ③ **`min` 不上升**（反作弊）| `ul_pipeline` 918→**895**、`t2f` 291.2→**91.9**、`ce` 18.5→**14.8**、`eqd` 334.0→**327.2** | ✅ **五条序列全部下降** |
| ④ 闸门判据不退化 | 对照 **10/10**；臂 **9/10**，唯一 FAIL 是 `DELIVERY leg: probe knobs only` —— **闸门正确地拒绝把带行为旋钮的腿当交付腿**，其余实质判据全过 | ✅ |
| 回退 ①③（RA 异常 / FATAL）| 未出现；UE 正常接入 | ✅ |
| 回退 ②（contract）| 两条腿都 **MET**、`gaps=0` | ✅ |
| 回退 ④ 的 **max 那一半** | 与判据①同一问题：**3 min 无功效**，不判 | ⏸ |

**吞吐未退化**：2.29 → **2.45 Mbit/s**；`AT/BELOW 0` 率反而更好：0.00071% → **0.00034%**。

#### (2) 载荷混淆，以及**抵消它的那个控制**

臂腿的 PUSCH 数只有一半（`ul_pipeline` 66 351 → 38 114），但 **UL 吞吐相同甚至略高** ⇒ 同样数据量
走了更少的 grant：`[ul_mac_pdu_size]` 中位 **1985 B → 3138 B（1.6×）**。
所以 `ce`/`eqd`/`ldpc`/`ul_pipeline` **不能跨腿直接比**（这正是本线一直记着的载荷混淆）。

**抵消办法就在报告里**：`[ul_by_size]` 按 TB 分桶给同一序列的中位/p95 ⇒ **同桶比较即载荷受控**：

| 桶 | 对照 `pipe_med` | 臂 `pipe_med` |
|---|---|---|
| 384–767 B | 1377.0 µs | 1395.0 µs |
| **≥768 B（占 99%）** | **1343.0 µs** | **1337.0 µs**（p95 1562 → 1542）|

⇒ **同桶内两条腿相同（大桶上臂还略好）**：没有退化，也没有可测的收益。

**每 slot 固定工作量的 `t2f` 则完全一致**（中位 492.8 vs 492.7、p95 607.6 vs 608.1、p99 619.5 vs 619.3），
**与载荷无关的 `rx_wait` 体内也一致**（中位 0.0/0.0，p95 159/159，p99 171/171）。

#### (3) 为什么 `max` 在这对腿上不可评（预登记已写，实测也印证）

* 3 min 的腿里，尾部事件是**单次抽样**：`ce` 超 1000 µs 各只有 **1 个**（64852/34554 样本里），
  `rx_wait` 超 1000 µs 是 7/9 个；安静腿 10.7 min 才 7 个 `ce`、26 个 `ul_pipeline` 超阈。
* 更直接：**两条腿的环境负载不同** —— 尾部事件时刻的 `load1` 是 **7.74（对照）vs 10.86（臂）**，
  而安静参照腿 p194 只有 ≈3.9。臂腿 `rx_wait max` 更高（11 920 vs 5 351 µs）与它自己的**更高负载**
  同时出现，这不能归因于臂。
* ⇒ 结论：**这对腿回答的是"安不安全"，不是"有没有效"**；`max` 必须由更长的腿（或多次短腿的
  `max` 分布）来判。

#### (4) 顺带更正一条我自己的读数（仪器语义）

`[ul_thread_cpu]` 的 mean 是"**两次本线程 PUSCH 观察之间**烧掉的 CPU"，因此它随 PUSCH 率变化：
对照（窗口 19.8 ms）mean ≈1.2 ms；臂（窗口 31.3 ms）mean ≈1.9 ms —— 但**两者换算成速率都是 ≈61 µs/ms
（≈6% 占空）**。所以稳定量是**占空 ≈6%**，不是"每次激活 1 ms"；按 6% 算，每 5 ms 周期约需 **300 µs**，
即已申报的 `computation=1000 µs` 是**约 3 倍的宽裕**（方向安全）。

★ 备注（不是判据）：`ivcsw` 率 177.26 → **210.05/ms（+18%）**，与"约束把线程按预算切分"一致，
值得在长腿上继续观察。

**下一步**：飞一对**长腿（≥10 min）**回答 `max`；若你希望腿一直保持短，则判据应从"`max`"改为
"尾部**率**"，并且 `slip/recv` 两条计数界也应同时改成率（否则短腿会把风暴判据变成橡皮图章）。

### 10.33 2026-10-02 —— 判据两处收口：`slip/recv` 也改为**率**；P4 的成功判据由 `max` 改为**尾部率**（长腿预登记）

#### (1) `slip/recv` 改为率（用户裁决"两件都做"的前半）

**理由与 `AT/BELOW 0` 逐字相同，而这一族的数据把它摆得更直白**：

| 腿 | slip（计数）| receive calls | **率** |
|---|---|---|---|
| p182_1726 | **124** | 38 842 391 | **0.000319%** |
| p177 | **22** | 5 922 689 | **0.000371%** |

**计数把这两条腿排反了**：计数差 5.6 倍，率却更低。新界 = 家族（≥5M 次 receive 调用，使率可估；
排除被观察腿 p182_1726）最坏率 ×1.25 ⇒ **slip ≤ 0.0005%、recv ≤ 0.0004%**。判定行现在把三个率连同
各自的分母一起打印。落地：`leg_gate.sh`。

**反向臂**（★ 这次按"判据自己的读数"扫，不再另找数字）：

| 腿 | in-stream `AT/BELOW 0` 率 | 判定 |
|---|---|---|
| p195_0703 | **0.20998%** | ❌ |
| p182_1726 | **0.02555%** | ❌（该腿共 FAIL 4 项）|
| p177 / p176 / p178 | **0.01418% / 0.00270% / 0.00263%** | ❌ |
| p179 0.00235%、p191 0.00225%、p192 0.00085% | | ✅ |
| p189 0.00077%、p190 0.00084%（交付对）| | ✅ |
| p193 0.00078%、p194 0.00047%（安静长腿）| | ✅ |

★ **更正我先前的说法**：我曾写"p187_2040 = 0.0048% 会打红"——**错了**。那是同一行里**另一个口径**
（含 teardown 尾巴）的数字；闸门的 in-stream 值是 **0.00084%，通过**。教训：反向臂要用判据自己的读数。

**slip/recv 的新界在册的腿上都无法触发**（旧的计数界同样无法触发——两者本来就取自家族最坏值），
所以用**合成输入**证明它能打红：把 p197 的日志 copy 一份、只把 `slip(... over 1ms=)` 改成 **500**
（该分母下界 = 42 次）⇒ `[FAIL]`（读数 0.006002% vs 界 0.0005%）。

#### (2) P4 的成功判据：`max` → **尾部率**（用户选 C 的后半）

3 min 配对腿（`p195_0729` 对照 / `p197` 臂）的判定见 §10.32：**安全性成立、`max` 无功效**。
`max` 是不可判的量（重尾里的一次抽样；3 min 里每腿只有 1 个 `ce` 超阈），所以长腿的判据改为：

**预登记 2026-10-02 / `p198-n78-p4ctl` + `p199-n78-p4pool5`（长腿对，≥10 min）**

* 臂 A（对照）：默认调度；臂 B（P4）：加
  `OCUDU_SCHED_TIME_CONSTRAINT='main_pool#0=5000/1000/2000;…;#4=5000/1000/2000'`；其余 env 逐字相同。
* **判据 = 四条载体的"超阈率"**（分子/分母都取该腿报告自己的数）：
  1. `rx_wait > 1000 µs` / receives（与我们的工作量无关）
  2. `t2f > 2000 µs` / samples（每 slot 固定工作量）
  3. `ce > 1000 µs` / samples
  4. `ul_pipeline > 2000 µs` / samples
* **判定规则**：臂 **在任一载体上不得变差**，且 **≥2 条变好**（率更低）。
* **`max` 只记录、不判**（它是主目标的代理量：若尾部率下降，`max` 应当跟着下降；反之不成立）。
* **反作弊**：`min` **不得上升**（§10.32 已实测五条全降）。
* **腿对作废条件**（任一即作废，不进结论）：① 两腿尾部事件时刻的 `load1` 相差 >50%；
  ② 两腿 `[ul_mac_pdu_size]` 中位相差 >30%（载荷几何不同 ⇒ 与载荷相关的载体不可比）；
  ③ 任一腿闸门 FAIL 出实质判据（白名单那条除外）。
* **回退条件**：§10.31(4) 的四条不变（RA/Msg3 异常、contract、FATAL、`max` 未降或 `min` 升）。

### 10.34 2026-10-02 —— 长腿对（p198/p199）**两条腿都作废：电台丢样本**；`ivcsw +19%` 复现；顺带修掉仪器两处自相矛盾

#### (1) 腿与臂

| | 臂 A（对照）| 臂 B（P4）|
|---|---|---|
| 腿 | `p198-n78-p4ctl_1002_1826` | `p199-n78-p4pool5_1002_1858` |
| 时长 | **1905 s（31.8 min）** | **1741 s（29.0 min）** |
| 调度 | 默认 | 池 5 条 `tc=5000/1000/2000us(duty=20%)`（`[sched_tc]` **恰好 5 行** ✓）|

#### (2) ★★ 判定：**两条腿都作废，不给结论**（预登记作废条件 ② 与 ③ 同时命中）

| 闸门 | 对照 p198 | 臂 p199 |
|---|---|---|
| contract | **1 of 8 未过** | **1 of 8 未过** |
| 电台样本连续性 | **gaps=4（6 972 254 样本缺失/重复）** | **gaps=1（3 784 146）** |
| `AT/BELOW 0` 率（界 0.0025%）| **0.01850%**（705/3 810 197）| **0.01209%**（421/3 481 817）|
| PDU 中位 | 1761 B | **2433 B（+38%，越过 30% 作废线）** |
| `load1`（尾部事件）| 10.69 | 7.22（在 50% 内）|

⇒ **连续第三条腿出现电台丢样本**（`p195_0703` gaps=1、p198 gaps=4、p199 gaps=1），三条都有
`[RF] Real-time failure: underflow` 与传输风暴。参照的**安静腿 p193/p194 是 `gaps=0`**。
**这与臂无关**（对照腿更严重），是运行环境/电台通路的问题：腿的数据不可信，`max` 与尾部率都不可用。

#### (3) 能记录的（**是观察，不是结论**）

* **与载荷无关的 `rx_wait`**：率 0.0000769% → **0.0000698%**（臂好 9%）；max 218 101 → 216 170 µs。
* **`t2f`**（每 slot 固定）：地板计数 2 → **6**（n 太小，不可判）；地板行的 max 4197 → **34 331 µs**。
* `ce` 43 → 353（8.8×）、`ul_pipeline` 526 → 2123（4.3×）—— 但**载荷几何差 38% 且两腿都作废**，不能归因。
* **载荷受控比较**（`[ul_by_size]` 同桶）：≥768 B 桶 `pipe_med` **1313.0 → 1306.0**、p95 **1537 → 1523**
  ⇒ **桶内两腿相同（臂略好）**，`iq2llr` 同。
* `min`：`ul_pipeline` 968 → **699**、`ce` 7.4 → 7.4、`t2f` 84 → 92（微升）、`rx_wait` 0 → 0。
* ★ **`ivcsw` 率 171.75 → 204.13/ms（+19%）**，与 3 min 那对的 **+18%** 一致 ⇒ **两对两中，是约束的可复现后果**
  （不是噪声）。含义待定：可能是"按预算被切分"的正常现象，也可能是更多被打断。**记为悬案**，不判。

#### (4) 顺带修掉仪器两处自相矛盾（commit `9cdce678b7`）

1. **同一个计数有两个定义**：`t2f` 的地板行说 6、分窗数组和只有 2（`ce` 353 vs 314）。原因是边界算子：
   地板行用 `>=`（`timing_event_wanted_phase`），分窗用 `>`，而相位时长来自 **µs 分辨率的来源**，
   恰好压在整数阈值上的样本很常见。地板行是**已登记的读数**（所有界与推导都引它），所以改分窗为 `>=`，
   并加 `window_counts_agree_with_the_floor_line`（三个恰好等于阈值的样本把它钉住；反向臂实测打红：`>` 实现得 0）。
2. **测试钩子只清了一半**：`reset_samples_for_test()` 清了向量却没清**计数累加器**，而每条地板行都是
   "计数 / 向量" ⇒ 早先用例的超阈样本留在分子里。这正是新用例在 ctest（每例一进程）通过、在**整二进制**
   （全例一进程）失败的原因。钩子现在把计数与 worst-K 列表一起清，整二进制 **17/17** 恢复全绿。

#### (5) 下一步（诊断结论）

**先解决丢样本，再谈 P4。** 三条腿的 gaps 都在**电台样本通路**上（不是我们的线程）：
建议按顺序查 ① USB 通路/供电与线缆（B200 是否在 hub 上）② 是否有别的进程在用 USB/显示/采集
（`p193/p194` 那次干净的时候机器处于深夜空闲）③ 用 `gaps=0` + `contract MET` 作为**下一次飞腿的前置门槛**
—— 达不到就先别跑 30 分钟，改跑 3 分钟做环境探测。

### 10.35 2026-10-02 —— ★★★ P4 第二对腿（p200/p201）：**第一次有效的一对，四条尾部率全部改善 2–6×**

#### (1) 有效性（预登记的四条作废条件全部解除）

| 检查 | 对照 p200 | 臂 p201 |
|---|---|---|
| 时长 | 633.3 s | 637.3 s |
| `[sched_tc]` | 0（默认调度）| **5** ✓（5 条池线程 `tc=5000/1000/2000us(duty=20%)`）|
| 闸门 | **10/10** | **9/10**（唯一 FAIL = 白名单正确地拒绝把带行为旋钮的腿当交付腿）|
| **电台样本连续性** | **gaps=0** | **gaps=0** |
| `AT/BELOW 0` 率（界 0.0025%）| 通过 | 通过 |
| **PDU 中位**（作废线 30%）| 1953 B | 1985 B（**+1.6%** ✓ 载荷几何匹配）|
| `load1`（尾部事件，作废线 50%）| 5.67 | 4.51（+26% 内；**且对照腿更忙**）|

#### (2) 判据（§10.33 预登记：任一载体不得变差 且 ≥2 条变好）

| 载体 | 对照 | 臂 | 比 |
|---|---|---|---|
| `rx_wait > 1000 µs` / receives | 18 / 17 732 820 = 0.0001015% | 3 / 17 843 235 = **0.0000168%** | **0.17** ✅ |
| `t2f > 2000 µs` / samples | 11 / 351 194 = 0.003132% | 2 / 350 768 = **0.000570%** | **0.18** ✅ |
| `ce > 1000 µs` / samples | 252 / 351 194 = 0.071755% | 83 / 350 768 = **0.023662%** | **0.33** ✅ |
| `ul_pipeline > 2000 µs` / samples | 1828 / 354 187 = 0.516112% | 891 / 353 944 = **0.251735%** | **0.49** ✅ |

⇒ **四条全部变好、无一变差 ⇒ 判据通过（本线第一次）。**

体内统计量同向（`ce` 最明显）：`ul_pipeline` 中位 1313.0 → **1297.0**、p95 1550 → **1518**、p99 1713 → **1593**；
`ce` 中位 66.0 → **58.8**、p95 110.1 → **87.5**、p99 181.2 → **101.2**；`t2f` 中位 490.9 → 490.7（平）。
`max`：`rx_wait` 15 532 → **4218 µs（-73%）**、`t2f` 9503.8 → **7175.3**、`ce` 3588.6 → **2992.5**、
`ul_pipeline` 5643 → 5697（+1%）。

#### (3) ★ 但必须写清两件事：`min` 上升，以及改善集中在**头四分之一**

**① 判据③（`min` 不上升）在 2/4 条载体上被违反**：`ul_pipeline` 808 → **984 µs（+22%）**、
`t2f` 78.6 → **97.7（+24%）**；`ce` 11.7 → **6.7（-43%，下降）**、`rx_wait` 0 → 0。
**这不是作弊**（尾部与体内同时下降，不是靠压扁量程换 `max`），但确实是代价，且 `min` 是**单样本极值**
（n≈354 k），这个 +176 µs 需要重复腿才能确认。

**② 臂的超阈事件 82–93% 集中在头两个窗口**（前 ~2.6 分钟 = 全腿的 1/4），之后几乎干净：

| 序列 | 对照（8 窗）| 臂（8 窗）|
|---|---|---|
| `ul_pipeline > 2000 µs` | 230 14 20 167 **451 267 131 548** | **421 312** 33 61 **2** 17 16 29 |
| `ce > 1000 µs` | 29 0 0 23 57 43 21 57 | **48 29** 0 0 0 0 1 0 |
| `rx_wait > 1000 µs` | **12** 0 0 1 3 0 1 1 | 2 1 0 0 0 0 0 0 |

⇒ **稳态（第 3–8 窗）**：`ul_pipeline` 臂 **158** vs 对照 **1584（10× 好）**；`ce` 臂 **1** vs 对照 **201**；
`rx_wait` 臂 **0** vs 对照 **6**。**所以总计的 2× 改善是被臂自己的启动瞬态拖下来的**——若瞬态可消除，
稳态差距是 10×。
⇒ 两种解释无法用 n=1 分开：**(a) 约束保护了稳态**；**(b) 对照腿恰好在中后段经历了更多外部扰动**
（它的第 8 窗 548 是它自己的最差窗，而臂的第 8 窗只有 29）。**下一对腿要专门看这一点。**

#### (4) 旁证与纵向对照

* **`ivcsw` 率 176.03 → 205.81/ms（+17%）**：**连续三对腿都是 +17~19%**（3 min 对 +18%、长腿对 +19%）
  ⇒ 这是约束的稳定后果，不是噪声。（含义仍待定，不判。）
* **与安静参照的纵向对照**：`ul_pipeline > 2000 µs` 率 —— p194（安静夜腿）= 26/264 856 = **0.0098%**；
  今天对照 = **0.516%**（53×）、臂 = **0.252%**（26×）。
  ⇒ **今天这台机器整体仍远不如昨夜安静**；臂把差距砍了一半，但**没有**回到安静机器的水平。
  这与 §10.34 的诊断一致（Spotlight 索引今天才关掉；p200/p201 是关掉之后飞的，但机器上仍有
  WindowServer ~24%、VM 等负载）。

#### (5) 结论与下一步

**判据通过（四条尾部率 2–6× 改善、无一变差），但 `min` 在两条序列上升，且改善的时间分布不均。**
因此本节的结论只能是"**与机制一致，尚不足以定论**"。下一步按价值排序：
1. **重复一对**（各 ~10.6 min）——检验 2–6× 是否可复现，并看启动瞬态是否复现；
2. 若复现：把"启动瞬态"当成一个可查的问题（约束在线程创建时施加 ⇒ 前 2.6 分钟是否在预算热身？还是
   启动期本来就有 burst？对照腿第 1 窗 230 也高于其后几窗，说明**两腿启动期都更差**，只是臂更差）；
3. 交付问题（**需要用户裁决**）：这条臂要把 `OCUDU_SCHED_TIME_CONSTRAINT` 变成默认（即交付形态的一部分），
   代价是该 5 条线程**永久失去 QoS 档**（§10.29 实测互斥）。在此之前 `lower_phy_*` 仍然**不许**进臂（0/12）。

### 10.36 2026-10-02 —— 网络搬家（一次被误读成 socket bug 的故障）+ 大预算臂的 loopback 预验 + Linux 双配置复核

#### (1) ★ 现象与真因：`Unable to allocate the required NG-U network resources` **不是 socket 问题**

loopback 台架突然起不来，报上面那句；同一时间 **Ubuntu 台架也 ping 不通**。两件事同一个原因：
**Mac 换了网段**（`192.168.0.x` → `192.168.100.x`），而 `gnb_loopback_n78.yml` 里写死了旧地址：

| | 旧（config 里写死的）| 现在 |
|---|---|---|
| 核心网 AMF | `192.168.0.106`（当时的台架）| **`192.168.100.153`** |
| NG-U 绑定 | `192.168.0.231` | **`192.168.100.125`**（本机）|
| Ubuntu 台架 | `192.168.0.106` | **`192.168.100.131`** |

绑定地址在本机不存在 ⇒ `udp_network_gateway` 建不起来 ⇒ `gw->create()` 返回空 ⇒ 那句 ERROR。
**腿不受影响**，因为 `configs/gnb_rf_b200_tdd_n78_20mhz.yml` 用的是新网段（`amf.addrs: 192.168.100.153`）。
⇒ 已把 loopback config 对齐到腿的地址，并在文件里写明"再遇到这个报错，先比对这两行与腿的 config"。

#### (2) 大预算臂的 loopback 预验：**3/3 干净**

`pool5_cal2 = main_pool#0..#4 = 5000/2500/3000`（`computation` 1 ms → 2.5 ms，占空 20% → 50%）
⇒ **3/3 干净**，同批 `off` 对照也 3/3。**理由是它针对 §10.35(3) 定位的启动瞬态**：
约束在线程创建时施加，启动期（UE 接入 + 流量爬升）池线程的瞬时需求很可能超过 1 ms/5 ms，
于是在最需要 CPU 的两分钟里被按预算节流；微基准已证"报大是免费的"。

#### (3) Linux 双配置复核（HEAD `ff080a97ba`，台架 `jwang@192.168.100.131`）

| 配置 | 结果 |
|---|---|
| `FLOW_PROBES=OFF`（项目默认）| `BUILD_RC=0`，**23/23 通过** |
| `FLOW_PROBES=ON` | `BUILD_RC=0`（0 error），**34/34 通过** |

★ 台架上的 `build_fp` 目录**已不存在**（此前建过，后被清掉）⇒ 记录重建配方，免得下次重新发现：
`cmake -S . -B build_fp -DENABLE_FLOW_PROBES=ON -DCMAKE_BUILD_TYPE=Release`
然后 `cmake --build build_fp --target ul_pipeline_probe_test macos_compat_test -j8`。
**这个配置是必须的**：新测试整段在 `#if defined(OCUDU_FLOW_PROBES)` 内，用 OFF 配置编译等于"连语法都没检查"，
而 §10.31–10.33 的三个缺陷全部只在 ON 配置下才暴露。
