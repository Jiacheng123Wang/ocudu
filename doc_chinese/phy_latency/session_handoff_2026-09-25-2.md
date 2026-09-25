# Session handoff — 2026-09-25 #2

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 这条会话的第二份**（继 `…-1.md`，那份的 §6.1 就是本会话的开工令）。
>
> **细节一律在开发文档**：`gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"，
> **本轮的细节在 §6.19**）；**高层现状与计划**在 `high_level_status_and_plan.md`；三分工与旧名映射见 `README.md`。
> **本 memo 只写"开工要什么"**：现状、已证明/已证伪、仪器与判据、下一步、仍挂着的事。

---

## 0. 一句话现状

**上一会话把 5 秒钉在"队列次序"上；本会话把"次序"做成了读数，并补上了三个此前完全看不见的洞——离线全绿，空口腿待飞。**

> p13 的结论（**未变**）：`commit->start = 5.0027 s` 而 `start->end = 1.24 ms` ⇒ 命令缓冲**在队列里等**；
> Q9-D 排除三类设备侧栅栏、Q9-E 排除主机完成处理器滞后 ⇒ **只剩"排在同一条队列前面的缓冲"**。

**本会话落地的三条仪器（提交 `bf33445b89`，开发文档 §6.19）**：

| 编号 | 是什么 | 它回答 |
|---|---|---|
| **Q9-F** | **提交票号**：每个可能带栅栏的 `[cb commit]` 之前取全局票号；比较"等待者 vs signaller 的**提交**次序"（Q9-D 的盲区：Q9-D 比的是 generation **发出**）| **等待者是否排在 signaller 前面**（`waiter-committed-first`），**same-queue / cross-queue 分开计**（前者才是解不开的），并量出最长一次 |
| **Q9-F2** | **前端（deposit）块**进 lane 探针前端系列（在 **deposit 时刻**注册，因为交棒默认开时**不是** DFT 引擎提交它们）；跑完就"抽干"、给出自己的 `deposit->start` / `start->end` | **受害的那群块自己的 GPU 窗口**（此前空气腿上**一行都没有**：前端系列是空的）|
| **Q9-F3** | **队列占用时间线**（`OCUDU_METAL_GPU_TIME=1`）：每条命令缓冲一条记录（label/slot/窗口），报告求并集、报**洞**与**洞后第一条的 label/slot** | **等待期间设备在干什么**：队列被夹住（有大洞）还是设备被别的活占着（无洞）|

**判据**：`p0_gate.sh` 的 **D1–D4（= F1–F4）不变**（阈值一字未动）；**新增 D11（Q9-F）/ D12（Q9-F2+F3）都是 INFO 读数**。

---

## 1. 本会话（2026-09-25 #2）做了什么（提交一览）

| 提交 | 内容 | 出处 |
|---|---|---|
| `bf33445b89` | **Q9-F + Q9-F2 + Q9-F3** 三条仪器；`state()` 改为**故意不析构**（修 atexit 崩溃）；`p0_gate.sh` 加 D11/D12；metal 测试加 **arm 14**；开发文档 **§6.19**、高层文档同步 | §6.19 |

**离线证据（都在这一条提交里）**：

* `dft_release_adopt_metal_test` **arm 14**：用**真实命令缓冲**造出两种次序 ⇒ `commits 53->57, waits 0->2, waiter-first 0->1 (same-queue 0->1), longest ~30 ms`；
  同一臂还证明占用探针**每条提交都留了记录**（`0 -> 4 records`；`largest=26 ms` 就是该臂故意插的 25 ms）。
* `ctest -L phy` **193/193**、`lower_phy_test` **528/528**、`l1_handover_arms.sh` **5 PASS**、`l1_hop_arms.sh` 保持它自己的 `OPEN`。
* ⚠ **网不许与并行构建同时跑**：本轮有一次网撞上 `cmake --build`（二进制被重链接）⇒ 一次 `Bus error` + 一次 `token released 0 times`；
  **构建结束后串行重跑三次全绿**（教训已写进 §6.19 ⑤）。

---

## 2. 已证明 / 已证伪（相对上一份 memo 的增量）

| # | 结论 | 证据 | 出处 |
|---|---|---|---|
| 11 | **Q9-D 的盲区是真的可测的**：本机自测里"等待者先提交"被单独计数、并按**同队列**归类、量出时长 | arm 14 的 `waiter-first 0->1 (same-queue 0->1) longest ~30 ms` | §6.19 ⑤ |
| 12 | **前端块此前完全没有读数**（交棒默认开 ⇒ `commit_front_end()` 不是提交它们的那条路）⇒ 空气腿的 `[ul_gpu_lane] dft slots=` 行**根本没打印过** | p13 的 stderr 里没有该行（本会话复核）| §6.19 ③ |
| 13 | **`[metal_stats] gpu busy` 行一直是 0**，因为 p07–p13 都没带 `OCUDU_METAL_GPU_TIME=1` | p13 stderr：`gpu busy (front_end): commits=0` / `(back_end): commits=0` | §6.19 ④ |
| 14 | 报告期加锁会**炸进程**：atexit 处理器在 `state()` 的析构**之后**才跑 | `mutex lock failed: Invalid argument`（修法 = `state()` 故意不析构）| §6.19 ⑤ |

**当前主假设（Q9-G，仍未验证）**：`claim_grid_production()` 对 `claimed && !produced` 的条目**直接返回 generation**、
不自己提交；而 sweep 是"**锁内认领 → 解锁后提交**"⇒ **等待者可能先提交**，同一条串行队列上等一个排在自己后面的事件。

---

## 3. 下一步（开工清单）

### 3.1 第一件：飞 `p14-conc2`（**用户飞**，我已在离线侧准备好）

```bash
# 起腿前：pgrep -x gnb / pgrep -x ul_chain_replay / lsof -nP -iUDP:2152 都要干净
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p14-conc2 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量：CN 侧（10.45.0.1）上行 iperf3 -c <gNB-ip> -R -t 100      ← -R 不能省
# 判读：bash doc_chinese/phy_latency/wip/p0_gate.sh p14-conc2      # D1–D12
```

> ⚠ **测量臂声明**：`OCUDU_METAL_GPU_TIME=1` 给**每条**命令缓冲加一个完成处理器（诊断腿；不改提交 ⇒ `cbs/lane` 不受影响）。

**分支表（开发文档 §6.19 ⑥ 的简版）**

| D11 | D12 | 下一步 |
|---|---|---|
| `waiter-committed-first > 0`、`max` ≈ 停顿（秒级）、worst kind = `grid` | 洞 ≈ 停顿、洞后 label = `merged_hop`/`late_handed` | **Q9-G 成立** ⇒ 做**提交握手**：`claimed && !produced` 时，返回 generation 之前**等它"已提交"**（新增 `handed_entry::commit_done`，提交方在 `commit_dropped()` **之后、锁内**置位；等待**有界**，超界退回今天行为并计数）。**不能把 `[cb commit]` 挪进锁内**（完成处理器可能同线程内联 ⇒ 死锁）|
| `> 0` 但只有 µs/ms | 洞小 | 倒置有、但没吃到停顿 ⇒ 握手仍推荐（关窗口），**继续找 5 秒**（D12 的洞后 label + `start->end` 最大的块）|
| `= 0` | **洞 ≈ 5 s**、洞后 label 明确 | 次序**不是**原因 ⇒ 那条 label 的缓冲就是阻塞者，用它自己的 `commit->start`/`start->end` 定性；再谈"前端块独立队列"（**结构性，先请用户裁**）|
| `= 0` | **无洞**（`holes>100us=0`）| 设备一直在跑别的活 ⇒ 是**吞吐/排队**（§7.1 的 C 项：单车道串行），不是栅栏 |
| 读不出 | — | 腿没带 `OCUDU_METAL_GPU_TIME=1`，或二进制戳 ≠ HEAD ⇒ **按 RED 算**，重飞 |

### 3.2 第二件（可选、便宜）：`OCUDU_CE_LANE_ORDER=event` 臂

让 Q9-C 真正执行（默认 `merged` 路径上它零执行）：`own=` 应显著 >0；`cross_lane>0` 且**无 5 s 停顿**才是 Q9-C 的空口证据。
**测量臂**（会改提交形态，`cbs/lane` 不保证仍 2.00，必须在腿报告里写清）。

### 3.3 仍挂着的事

* **用户裁决**：**P2-E 选项 (b)**（前端 DFT 单独一次提交，+1 提交；§6.4.6 曾裁"不做"）——**若 Q9-G 的握手不奏效**，它就是下一个候选；
  **P2-F**（并发度作为交付）也待二次裁决。
* **两条陈旧网**：`value_net.py`（归档基线陈旧）与 `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` 差 15/27 capture）——改前就红，待裁决。
* **Q12**：腿 `s84b-p0` 无日志（未验证）；**P1-6**（`OCUDU_UL_SLOT_TRACE` 拆 B 项 48 µs）未做；**P2-D** 未开工。
* 文档：**开发文档 §6 是追加式**（新记录接 §6.20 起），`high_level_status_and_plan.md` 与 `README.md` 是活文档。

---

## 4. 纪律（每一条都是花过代价的，见开发文档 §5.2/§6.5）

1. **判据不许为过关改阈值**；**"读不出"按 RED 算**；**单条腿/单次读数不是证据**（p10 全绿、p11 复现就是教训）。
2. **用户飞腿时绝不碰 GPU**（门/单测/replay/短跑 gnb 都算）；进程检查**按名字**（`pgrep -x gnb`）。
3. **腿必须 Ctrl-C 停**（`kill -9` 会丢掉 atexit 的报告行）——Q9-F/Q9-F3 的报告**全在 atexit**。
4. **代码改动会让"腿的提交证据"失效** ⇒ 改完代码要重新构建（戳 = HEAD）再飞。
5. **网不许与并行构建同时跑**（本会话新增的一条：重链接中的二进制会读成"偶发失败"）。
6. **需要留存的开发产物一律放 `doc_chinese/work_tmp/`（git 忽略）**；`/tmp` 会被系统清掉。
7. 引用外部材料**用章节名/腿名，不用行号**（会漂移）。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§1 流程、§2 现象/机制、§3 判据、§4 仪表手册、§5 跑腿规范、**§6 追加式实施记录（§6.10–§6.19 是本轮全部细节，§6.19 = 本会话）**、§7 杠杆、§8 未决、§9 证据索引 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5、下一步、待裁决 |
| `doc_chinese/phy_latency/wip/p0_gate.sh` | **P0 门（D1–D12，只读日志）**——判据与读数的唯一入口 |
| `doc_chinese/phy_latency/wip/leg_census.py` | 腿普查（UL 静默表 + RF/pool 事件）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` | 起腿（硬检查进程与二进制戳）；**腿日志在 `…/wip/logs/`** |
| `lib/phy/metal/ocudu_metal_queue.{h,mm}` | 两道队列 + 三道设备侧栅栏 + **Q9-D 次序仪 + Q9-F 提交次序仪 + Q9-F3 占用时间线** |
| `lib/phy/metal/ocudu_metal_burst.{h,mm}` | 交棒注册表（Q9 修复、Q9-A reap、Q9-B/Q9-E 计数、**Q9-B 的提交点带 label**）|
| `lib/phy/metal/ocudu_metal_lane_probe.{h,mm}` | 车道探针（Q9-B 最慢表 + **Q9-F2 前端 carried 读数**）|
| `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm` | 交棒 deposit（**Q9-F2 的注册点**）、`commit_late_handed_block`（**Q9-F 的关键提交点**）|
| `lib/phy/generic_functions/metal/test/dft_release_adopt_metal_test.mm` | 离线臂（**arm 14 = Q9-F + Q9-F3 自测**）|

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-25-2.md`。腿 `p14-conc2` 的日志应当已经在
> `doc_chinese/phy_pipeline_gpu/wip/logs/`：先用 `bash doc_chinese/phy_latency/wip/p0_gate.sh p14-conc2` 读 **D1–D12**，
> 按 §3.1 的分支表选修法（**D11 `waiter-committed-first > 0` 且 `max` ≈ 停顿 ⇒ 实现 Q9-G 的提交握手**；
> `= 0` ⇒ 看 D12 的洞与洞后 label），把结果写进开发文档 **§6.20**，并同步高层文档。
> **若腿还没飞**：先确认它需要的构建戳 = HEAD，再把 §3.1 的命令原样交给用户。
