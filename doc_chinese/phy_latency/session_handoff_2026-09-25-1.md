# Session handoff — 2026-09-25 #1

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 这条会话**的第一份，交给**下一个会话**开工用。
> （历史上曾有一个同名文件，内容是"应并入开发文档"的稿子，已删除并并入开发文档 §5.3/§6.3/§6.4/§8；**本文件与它无关**。）
>
> **细节一律在开发文档**：`gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"，
> 已并入原 `00_status.md` … `05_*`）；**高层现状与计划**在 `high_level_status_and_plan.md`；三分工与旧名映射见 `README.md`。
> **本 memo 只写"开工要什么"**：现状、已证明/已证伪、仪器与判据、下一步、仍挂着的事。读数与出处一律指到开发文档的节号。

---

## 0. 一句话现状

**主线任务（缩短 `[ul_gpu_pipeline]` 端到端时延、让接收池不再被抽干）仍在进行中**，但**症状的根因层级已经被一条腿一条腿地向下推了 5 层**：

> 「注册表 sweep 跨环绕失效」（**已修，§6.10/§6.11**）→ 「输入 token 等到命令缓冲完成才回池」（**已量化，§6.12**）
> → 「认领之后 5.000 s 才完成」（**Q9-B 定位，§6.13**）→ 「跨车道 stage-fence 夹死」（**已修，但默认路径零执行，§6.14**）
> → 「三类设备侧栅栏全部排除 + 主机完成处理器不晚」（**Q9-D/Q9-E 两条负结果，§6.16/§6.17**）
> ⇒ **当前嫌疑：命令缓冲在队列里的次序（在等一个还没被提交的生产者）**，见 §4 的 Q9-G 假设。

**用户可见症状**：上行 `iperf3 -R -t 100` **仍会断流**（4 条腿里 3 条有、1 条全绿），每次断流由 **1–2 次约 5.000 秒的整链停顿**引起。
**判据**：`p0_gate.sh` 的 D1–D4（= F1–F4，§3.1 预登记）在停顿腿上红、在好腿上全绿。

---

## 1. 本会话（2026-09-25）做了什么（提交一览）

| 提交 | 内容 | 出处 |
|---|---|---|
| `3d00eafe97` | **Q9 修复**：sweep 改为「slot 窗口（环绕安全守护）+ 10 ms 单调时钟期限」，并在**每个注册表入口**都查 | §6.10 结案 / §6.11 修复 |
| `5ff8759804` | **Q9-A**（干池的接收线程自己驱动 sweep）＋ **Q9-B**（`registry commit->completion` ＋ lane 的 `commit->completion` 最慢表） | §6.13 |
| `5f51b0e9fe` | **Q9-C**：lane burst 等"本跳自己"的 generation，不再等"全局最新"（跨车道夹死）；带 `own/newest/cross_lane` 计数 | §6.14 |
| `2be02bfdb5` / `894d00918b` | **Xcode 26 SDK 的构建阻塞点**（见 §5.4） | §6.13 ⑤ |
| `2faaf3ab9c` | **Q9-D**：三道设备侧栅栏的**次序仪**（`signaller-first/after` + 最长等待 + 种类/slot） | §6.16 |
| `8f1e50042e` | **Q9-E**：**完成处理器滞后仪**（`handler lag = now − GPUEndTime`） | §6.17 |
| `95b9418d7b` 等 | 文档：§6.15（第一条全绿腿 + 归因警告）、§6.16、§6.17、高层文档同步、`p0_gate.sh` 的 D 组扩到 D10 | 开发文档 §6 |

**工具也在本会话扩了**：`doc_chinese/phy_latency/wip/p0_gate.sh` 现在含 **D1–D10**（D1–D4 是判据，D5–D10 是"这 5 秒在哪里"的读数）；
`dft_release_adopt_metal_test` 新增 **arm 11（Q9 回归）、arm 12（Q9-A 回归）、arm 13（Q9-D 自测）**。

---

## 2. 七条腿的读数（同一配方：n1 默认 + `max_pusch_and_srs_concurrency=2` + `OCUDU_UL_PHASE_SEGMENTS=1` + 上行 `iperf3 -R -t 100`）

| 腿 | 二进制 | `p0_gate` | `input hold` max | `pop_blocking` max | gaps | `starved_events` | 用户侧 iperf3 |
|---|---|---|---|---|---|---|---|
| `p07-conc2` | `78cb3fe0d1`（修 Q9 前）| 4 红 | 30.72 s | 4.998 s ×2 | 2 / 9.97 s | 383 | 断流两次 |
| `p08-conc2` | `bbc2ddf96f`（Q9 修复）| 4 红 | 5.95 s | 4.997 s ×2 | 3 / 9.98 s | 6 | 断流 |
| `p09-conc2` | `3affbf424f`（+A/B）| 4 红 | 5.00 s | 4.998 s ×3 | 3 / 14.9 s | 4 | 断流（且提前崩）|
| **`p10-conc2`** | `a49c4e0dde`（+Q9-C）| **18/18 全绿** | **18.9 ms** | **17 µs** | **0** | **0**（`held_max=4/8`）| **不断流**（100 s 满速，0 次 RF 实时失败）|
| `p11-conc2` | 与 p10 同源（Q9-D 前）| 14/18 | 5.003 s | 4.9946 s ×1 | 1 / 4.98 s | 1 | 断流（68 s 起）|
| `p12-conc2` | `2faaf3ab9c`（+Q9-D）| 14/18 | 5.0026 s | 4.9942 s ×1 | 1 / 4.98 s | 1 | 断流（49 s 起，89 s 起部分恢复）|
| **`p13-conc2`** | `8f1e50042e`（+Q9-E）| D1/D3/D4 红 | **5.0047 s** | 4.9976 s ×2 | **2 / 9.98 s** | 2 | **两次断流**（见 §3）|

> ⚠ **`p10` 的全绿只是"那次没触发"**：那条腿上 Q9-C 零执行（见 §4 第 5 行），此后 p11/p12/p13 都复现了停顿。

---

## 3. `p13-conc2` 的判读（最新一条腿，也是本会话的最后一条）

**用户侧 iperf3（原文摘要）**：`0–15 s` 约 4–8 Mbit/s（`5.77–6.09 s` 有一个 35.5 Mbit/s 的突发）→
**`15.08–25.49 s` 掉到 271 kbit/s** → **`25.49–54 s` 全零** → `54–65 s` 恢复到约 4 Mbit/s →
**`65.06–75.07 s` 掉到 62.5 kbit/s** → **`75–100 s` 全零**。汇总 `sender 18.9 MBytes / Retr 4064`、`receiver 13.2 MBytes / 1.10 Mbits/s`。
⇒ **两次断流**（第一次 ~15 s 起，第二次 ~65 s 起），与腿自己的 **2 次 park / 2 个 gap** 一一对应。

**腿的读数（`p0_gate.sh p13-conc2`）**

* **D1 红**：`input hold max = 5004.7 ms`（slot 9612）；**D3 红**：`pop_blocking max = 4997.6 ms`，`over 1s = 2`；
  **D4 红**：`radio continuity: 2 gaps / 153,152,535 samples（≈9.98 s）`；**D2 这次是绿的**（`wait max = 19.97 ms`）。
* `[ul_rx_pool] taken=302205 returned=302205 held_end=0 held_max=8 pool=8 free_min=0 starved_takes=34 starved_events=2`。
* **Q9-B**：`registry commit->completion=20236 max=5001219.0us`；lane 侧 `commit -> completion max = 5004221.4 us`，
  最慢两行是：
  ```
  slot=9612 stage=merged_hop commit->start=5002977.3us start->end=1244.1us commit->end=5004221.4us
  slot=1022 stage=merged_hop commit->start=5002737.0us start->end=1242.6us commit->end=5003979.6us
  ```
  ⇒ **`commit->start ≈ 5.0027 s`、`start->end ≈ 1.24 ms`**：命令缓冲**在队列里等了 5 秒才开始跑**，一旦开始只跑 1.24 ms。
* **Q9-D（栅栏次序）**：`fence order (Q9-D): waits=27957 signaller-first=27957 signaller-after=0` ⇒ **三类设备侧栅栏全部排除**。
* **Q9-E（完成处理器滞后）**：`handler lag=48123 max=2037.0us mean=53.9us at slot=6143`
  ⇒ **主机侧处理器只晚 2 ms（均值 54 µs）** ⇒ **"GPU 早跑完、主机晚看"这一支也被排除**。
* 受害者构成：`slowest` 表里 `slot=1022/9612` 是 `swept=0`（**跳认领**、`registry_commit=0`），
  `9613/9614/9615/9616/1023/1024` 是 `swept=1`（**sweep 认领**、`registry_commit=1`、`commit->completion ≈ 5.0003–5.0012 s`）。
* 其他：`[ul_gpu_pipeline]` 中位 2369.5 µs、`stale=0`、`lanes=27922`、`cbs/lane=2.00 dropped=0`、`carried=2726`、
  `burst commits=27922 waits=27922`、契约 `NOT MET: 1 of 8`（只有 continuity 一条）。
* 停顿时刻（主日志）：`03:17:48.97` 与 `03:18:26.56`（相隔 37.6 s）；两次的起点都是
  `[ul_rx_pool] the receive pool is EMPTY (held=8/8)`（**池先被按满，随后才是 `RF: late`/underflow**）。

**⇒ p13 把"5 秒在哪里"缩到了一个位置**：**命令缓冲在队列里等待开始**（不是设备侧事件等待、不是主机处理器滞后、也不是 GPU 执行慢）。
**能这样等的只有一种东西：排在同一条串行队列前面的某个命令缓冲没跑完。**

---

## 4. 已证明 / 已证伪（新会话最需要的一张表）

| # | 结论 | 证据 | 出处 |
|---|---|---|---|
| 1 | **sweep 的 slot 比较跨环绕失效**是真缺陷，且**已修好** | `starved_events` 383 → 6、`oldest unclaimed age` 77.9 s → 5.95 s、无 10.24 s 整数倍等待、`late_time=15` | §6.10/§6.11/§6.12 |
| 2 | **输入 token 的生命周期 = 命令缓冲的完成**（token 在完成处理器里回池） | `input hold` max 与 `deposit->completion` max 逐位相同 | §6.12 |
| 3 | 受害块**认领很快（~3 ms）但完成很晚（~5.000 s）** | P0-7 `slowest` 表 + Q9-B | §6.12/§6.13 |
| 4 | **干池时"没人来问"这一洞确实存在**（Q9-A 有效，但常常无事可做） | `dry-pool reaps=500…1471` 却只救回 3–9 块 ⇒ 持有者是 **claimed** 的块 | §6.15/§6.16 |
| 5 | ❌ **跨车道 stage-fence 夹死**（Q9-C 修的缺陷）**不是空口默认路径的原因** | `lane fence … own=0 newest=0`（`burst_ensure_open()` 在默认 `merged` 路径上**一次都没跑**，EQ/demap 走 `adopt()`）| §6.14/§6.16 |
| 6 | ❌ **三类设备侧栅栏都不是肇事者** | `fence order (Q9-D): waits=56054/27957 signaller-first 全部、signaller-after=0` | §6.16/§6.17 |
| 7 | ❌ **主机完成处理器滞后**也不是 | p13 `handler lag max=2.0 ms`、`mean=53.9 µs` | §6.17 |
| 8 | ✅ **整条停顿链可以完全不存在** | `p10-conc2`：18/18、`held_max=4/8`、`pop_blocking max=17 µs`、0 gaps、满速 100 s 无 RF 失败 | §6.15 |
| 9 | **触发是间歇的、形状是确定的** | p11/p12 的事件计数几乎逐位相同（`RF: late = 7248` 两次相同）；受害者总是**连续 8 个 slot**（= 一个池）| §6.16/§6.17 |
| 10 | **Q9-D 有已登记的盲区** | 它比的是"等待 vs generation **发出**"，而 grid 栅栏的 generation 在 **deposit 时**发出、**提交时**才生效 | §6.17 ③ |

**当前主假设（Q9-G，尚未验证、也尚未实现）**：`claim_grid_production()` 对一个
**已被别人认领但尚未提交**（`claimed==true && produced==false`）的条目，会**直接把 generation 返回**给调用者
（不自己提交）；于是**跳把"等这个生产者"的等待编码进自己的命令缓冲并提交**，而那个生产者的提交由**另一个线程**
（sweep 在 `deposit_released()` 里"锁内认领 → 解锁后提交"之间有几微秒窗口）晚一步发出 ⇒
**等待者的提交可能排在生产者之前**（同一条串行后端队列）⇒ 队首在等一个"排在它后面"的事件 ⇒
**整条队列冻结**，直到某个**更高 generation** 的生产者完成（约 5 s 后）把事件值推过等待值才解开。
它解释了：为什么受害者总是**连续 8 个 slot 的落单前端块**（它们全在队列后面）、为什么 `commit->start ≈ 5 s` 而
`start->end ≈ 1.2 ms`、为什么 D9 读成"安全形状"（generation 早在 deposit 时就发出了）、以及为什么主机处理器不晚。

---

## 5. 工具、判据、跑腿（新会话照抄即可）

### 5.1 起腿与判读

```bash
# 起腿前：进程检查（按名字！）
pgrep -x gnb || echo ok; pgrep -x ul_chain_replay || echo ok; lsof -nP -iUDP:2152 || echo ok
# 构建（改了代码之后必须先做，run_leg.sh 会硬检查二进制戳 = HEAD）
touch build/hashes.h && cmake --build build --target gnb

# 本阶段的标准腿（= p07…p13 的配方），跑 ~100 s 后用【Ctrl-C】停（报告全在 atexit；kill -9 会丢）
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量（CN 侧 10.45.0.1）：上行 `iperf3 -c <gNB-ip> -R -t 100`

# 判读（只读日志，用户飞腿时也能跑）
bash doc_chinese/phy_latency/wip/p0_gate.sh <label>          # D1–D10
python3 doc_chinese/phy_latency/wip/leg_census.py doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_<label>*.log
```

### 5.2 判据（**已预登记，不许改阈值**）

* **F1–F4 = D1–D4**：`input hold` max **<100 ms**；block lifecycle 的 `wait max` 与 `oldest unclaimed age max` **<100 ms**；
  `pop_blocking` max **<10 ms** 且 `over 1s=0`；`radio sample continuity` **gaps=0**。
* **F5**：用户侧上行 iperf3 **不断流**。**F6**：`late_time ≪ late`。**F7**：V1–V5 不变（尤其 `cbs/lane ≤2.00 dropped=0`）。
  **F8**：契约 8/8、`paired/phase account … EXACT MATCH`。
* **D5–D10 是读数不是判据**（INFO）：`late/late_time`、`registry commit->completion`、`dry-pool reaps`、
  `own/newest/cross_lane`、`fence order`、`handler lag`。判读分支写在 §6.16 ③ 与 §6.17 ④。

### 5.3 现有仪器（都在提交里，都会在腿的 stderr 报告里打印）

| 读数 | 回答什么 |
|---|---|
| `[metal_stats] input hold (P0-2)` | 输入被按住多久（含 `over 100ms/1s` 尾巴）|
| `[metal_stats] block lifecycle (P0-7)` | 每个块：认领等待、deposit→completion、**`registry commit->completion`（Q9-B）**、**`dry-pool reaps`（Q9-A）**、**`handler lag`（Q9-E）**、最慢 8 行 |
| `[ul_gpu_lane] commit -> completion (Q9-B)` + slowest 表 | 每条 cb 拆成 **`commit->start`（队列）/ `start->end`（设备）**，带 slot 与 stage |
| `[metal_stats] fence order (Q9-D)` | 三类设备侧栅栏的等待 vs signaller 次序 + 最长等待 |
| `[metal_stats] lane fence … own/newest/cross_lane`（Q9-C）| burst 的 stage 等待命名了谁的 generation |
| `[ul_rx_pool] …` / `pop_blocking wait (P0-2)` | 池：`held_max/free_min/starved_events` + 阻塞时长 |
| 现成诊断开关 | `OCUDU_CE_WAIT_TRACE=1`（held cb 的 publish/close 指针轨迹）、`OCUDU_METAL_GPU_TIME=1`、`OCUDU_D1_HANDED_BOUND`、`OCUDU_CE_LANE_ORDER=event`、`OCUDU_DFT_RELEASE_TOKENS_EARLY`（P2-E 仪器）|

### 5.4 环境（本机，2026-09-25 起）

* **工具链已换成 Xcode 26 的 SDK**（接受 Xcode 许可后）⇒ 它的 libc++ 更严，**四处**曾把整棵树挡在编译之外，
  已在 `2be02bfdb5` 修好：`future::get()` 的 `[[nodiscard]]`（`io_broker.h`、`task_worker.cpp`）、
  `set::count()` 的 `[[nodiscard]]`（`pucch_resource_manager_test.cpp`）、`flat_map.h` 的 `sort_iter` 缺 `operator[]`。
  ⇒ **不要再"修"这些**；若又冒出同类错误，按同样办法处理并把原因写进开发文档 §6.13 ⑤。
* `ctest -j 6`（整套并行）会让两个 **CE Metal 单测**因 GPU 争用而红；**串行**（`ctest -L phy`、`-R port_channel_estimator_metal_mmse`）全绿。
* **`ctest -L phy` = 193 全绿**、`lower_phy_test` 528/528、`ring_buffer_test` 4/4 是每次改代码后的最低网。

### 5.5 离线回归网（改代码后必跑）

```bash
cd build && ctest -L phy                                   # 193
cd .. && ./build/tests/unittests/phy/lower/lower_phy_test  # 528
./build/lib/phy/generic_functions/metal/dft_release_adopt_metal_test     # rc=0，arm 11/12/13 必须打印通过
bash doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh 8 /tmp/l1_handover_<tag>   # 5 PASS
bash doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh                                  # 4/4 differing=0
# 与 pristine HEAD 二进制的逐字节比对（值中性）：参考二进制在 work_tmp/ref/replay_head_pre_p05
```

---

## 6. 下一步（新会话的开工清单，按优先级）

### 6.1 第一件：验证/否证 Q9-G（**建议先测，再改**）

两条**互不冲突**的小仪器，做进同一次构建、飞一条腿即可定性：

| 编号 | 内容 | 为什么需要 |
|---|---|---|
| **Q9-F** | **提交票号**：给每个 `[cb commit]` 编一个全局递增序号（在 `ocudu_metal_burst.mm` / `ocudu_metal_mmse_engine.mm` 的提交点调用 `shared_queue::note_commit(cb)`），并在**等待编码**时记录等待者的票号；报告"**等待者票号 < signaller 票号**"的次数与最坏一例（slot/种类） | Q9-D 的盲区正是这一支：**"generation 早发出、提交却晚"**。它会把 Q9-G 的假设直接变成读数（若次数 >0 且最坏一例 ≈5 s，假设成立）|
| **Q9-F2** | 把 **deposit（前端）命令缓冲**也注册进 lane 探针的前端组（`register_front_end_commit()`），让空气腿上也能打印前端块的 `residency/busy/gap` | 现在**只有车道 cb 有 GPU 时间**，而受害的正是前端块。它能区分"前端块在跑 5 s"与"在队列里等 5 s"（p13 的 lane 数据已表明是**等**，但需要前端块自己的读数坐实）|

### 6.2 第二件：按读数选修法

* 若 Q9-F 显示 **存在"等待者先提交"** ⇒ **实现 Q9-G 握手**（推荐方案）：
  在 `claim_grid_production()` 里，当目标条目 `claimed && !produced` 时，**在返回 generation 之前等它"已经提交"**
  （新增 `handed_entry::commit_done`，由提交方在 `commit_dropped()` 之后**在锁内**置位；等待为**有界**、只在罕见窗口发生；
  超界则退回今天的行为并计数）。目标是：**等待者编码等待之前，生产者的提交已经发出** ⇒ 次序不可能倒置。
  ⚠ **不能把 `[cb commit]` 挪进锁内**（完成处理器可能同线程内联运行 ⇒ 死锁），必须用"提交后置位"的握手。
* 若 Q9-F 显示 **没有倒置** ⇒ 队列次序不是原因 ⇒ 改用 **GPU 侧证据**：前端块的 `start->end`（若 ≈5 s ⇒ GPU 真的在等/卡，
  指向驱动/硬件层面），并考虑是否要让前端块走**独立队列**（结构性改动，**先与用户确认**）。
* 无论哪一支，**Q9-A（干池驱动 sweep）保留**（已证明有用：p09 救回 9 块、p11/p12/p13 各救回 3–6 块）。

### 6.3 第三件：把"仍未触发"的 Q9-C 空口验证掉（可选但便宜）

`OCUDU_CE_LANE_ORDER=event` 臂（**测量臂**：会改提交形态，`cbs/lane` 不保证仍 2.00，必须在腿报告里写清）：
`event` 序下 `burst_ensure_open()` **必然**挂 stage 栅栏 ⇒ `own=` 应显著 >0；若 `cross_lane>0` 且不再出现 5 s 停顿，
那才是 Q9-C 的空口证据（离线证据已有：估计器单测 `own=3 newest=5 cross_lane=2`）。

### 6.4 仍挂着的事（不需要本任务的技术判断，但别忘了）

* **用户裁决**：**P2-E 选项 (b)**（前端 DFT 单独一次提交，+1 提交；§6.4.6 曾裁"不做"）——**若 Q9-G 的握手不奏效**，
  它就是下一个候选（现在有了空口证据：输入生命周期被"可能跨跳被 hold 的 cb"按住会抽干池）；**P2-F**（并发度作为交付）也待二次裁决。
* **两条陈旧网**：`value_net.py`（归档基线陈旧）与 `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` 差 15/27 capture）——改前就红，待裁决。
* **Q12**：腿 `s84b-p0` 无日志（未验证）；**P1-6**（`OCUDU_UL_SLOT_TRACE` 拆 B 项 48 µs）未做；**P2-D** 未开工。
* 文档：**开发文档 §6 是追加式**（新记录接 §6.18 起），`high_level_status_and_plan.md` 与 `README.md` 是活文档。

---

## 7. 纪律（每一条都是花过代价的，见开发文档 §5.2/§6.5）

1. **判据不许为过关改阈值**；**"读不出"按 RED 算**；**单条腿/单次读数不是证据**（p10 全绿、p11 复现就是教训）。
2. **用户飞腿时绝不碰 GPU**（门/单测/replay/短跑 gnb 都算）；进程检查**按名字**（`pgrep -x gnb`）。
3. **腿必须 Ctrl-C 停**（`kill -9` 会丢掉 atexit 的报告行）。
4. **代码改动会让"腿的提交证据"失效** ⇒ 改完代码要重新构建（戳 = HEAD）再飞。
5. **需要留存的开发产物一律放 `doc_chinese/work_tmp/`（git 忽略）**；`/tmp` 会被系统清掉
   （`/tmp/limited_run.sh` 是唯一允许的一次性中间物，掉了按开发文档 §6.13 ④ 的说明重建）。
6. 引用外部材料**用章节名/腿名，不用行号**（会漂移）。

---

## 8. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§1 流程、§2 现象/机制、§3 判据、§4 仪表手册、§5 跑腿规范、**§6 追加式实施记录（§6.10–§6.17 是本轮全部细节）**、§7 杠杆、§8 未决、§9 证据索引 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5、下一步、待裁决 |
| `doc_chinese/phy_latency/wip/p0_gate.sh` | **P0 门（D1–D10，只读日志）**——判据与读数的唯一入口 |
| `doc_chinese/phy_latency/wip/leg_census.py` | 腿普查（UL 静默表 + RF/pool 事件）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` | 起腿（硬检查进程与二进制戳）；**腿日志在 `…/wip/logs/`** |
| `doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh` / `l1_hop_arms.sh` | 交棒/跳的离线字节网 |
| `doc_chinese/work_tmp/`（git 忽略）| 语料 `corpus/`、参考二进制 `ref/replay_head_pre_p05`、dump、腿的原始记录 |
| `lib/phy/metal/ocudu_metal_burst.{h,mm}` | 交棒注册表（Q9 修复、Q9-A reap、Q9-B/Q9-E 计数）|
| `lib/phy/metal/ocudu_metal_queue.{h,mm}` | 三道设备侧栅栏（Q9-C 选择、Q9-D 次序仪）|
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` | 收包池、`pop_rx_buffer_blocking()`（Q9-A 的入口）|
| `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm` | 估计器/车道序/hold/栅栏的发送点 |
| `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp`、`lib/du/du_low/du_low_executor_mapper.cpp` | `[ul_lane_exec]` 生效并发度的两行出处 |

---

## 9. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-25-1.md`，然后按它的 §6.1 做 **Q9-F（提交票号）＋ Q9-F2（前端块 GPU 时间）** 两条仪器，
> 离线验证（§5.5），把结果写进开发文档 **§6.18**，再请用户飞一条同配方的腿（`p14-conc2`，§5.1 的配方），
> 用 `p0_gate.sh` 的 D1–D10 判读，并按 §6.2 的分支表选修法。
