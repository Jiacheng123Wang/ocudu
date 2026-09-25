# Session handoff — 2026-09-25 #2

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 这条会话的第二份**（继 `…-1.md`，那份的 §6.1 就是本会话的开工令）。
>
> **细节一律在开发文档**：`gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"，
> **本轮的细节在 §6.19**）；**高层现状与计划**在 `high_level_status_and_plan.md`；三分工与旧名映射见 `README.md`。
> **本 memo 只写"开工要什么"**：现状、已证明/已证伪、仪器与判据、下一步、仍挂着的事。

---

## 0. 一句话现状（★ 本会话后半段：**根因已结案并已修复 A**）

**根因 = Metal 对"未被满足的设备侧事件等待"有 5.00 s 硬上界**：一个消费者若在承载其信号的命令缓冲**提交之前**
就被提交，队列被按住 **5.00 s**（signaller 也跑不了），随后等待被**丢弃**（栅栏失效）。离线复现见
`wip/metal_wait_timeout_probe.mm`，推导见开发文档 **§6.20**；**修复 A（提交握手）已落地并离线自证（§6.21）**。

> 上一会话把 5 秒钉在"队列次序"上；本会话先做出三条仪器（Q9-F/Q9-F2/Q9-F3，§6.19），
> 再用离线复现把"那 5 秒是什么"彻底问清（§6.20），并做掉修复 A（§6.21）。**下一条腿 = `p15-conc2`（回归）**。

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
| （本会话后续）| **5 秒的根因 + 修复 A**：`wip/metal_wait_timeout_probe.mm` 离线复现 Metal 的 **5.00 s 等待上界**；`handed_entry::commit_issued` + `note_block_commit_issued()`（四个提交点）+ `grid_production_generation()` 的**有界提交握手**（等不到 ⇒ 退回有界主机等待）；metal 测试 **arm 15**（三子例）；门加 **D13** + 自测；开发文档 **§6.20/§6.21** | §6.20/§6.21 |

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

**已结案（替代上一份 memo 的"主假设 Q9-G"）**：Q9-G 的窗口是真的（`claim_grid_production()` 会把一个**尚未提交**的
承载者的 generation 直接交出去），但它的**代价**不是"永远互锁"，而是 **Metal 固定 5.00 s 的等待上界**：

| 离线臂（`wip/metal_wait_timeout_probe.mm`，每臂独占队列/事件）| 等待者完成 | `commit->start` | signaller |
|---|---|---|---|
| A2 signaller 在 +200 ms 提交 | **5.001 s** | 5000668 µs | **5.008 s**（被堵在等待者后面）|
| A4 signaller 在 +8000 ms 提交 | **5.001 s** | 5001325 µs | 8.005 s（⇒ 消费者先跑，**栅栏失效**）|
| A5 **永不提交**（event=0）| **5.000 s** | 5000260 µs | — |
| B signaller **先**提交 | 0.206 s | 484 µs | 0.007 s |

四条空口读数（`commit->start=5002977 µs` 而 `start->end=1244 µs`、`deposit->completion=5.0047 s`、
`input hold=5.0047 s`、`pop_blocking=4.9976 s`）是**同一个 5 秒**；p14 主日志显示停顿期间**整个时隙环（含下行）停住**
（`Slot decisions` 1000/s → 0，恢复时一秒 3288 条），恢复后该 UE 因 HARQ/CRC 饥饿**再也不被授 Grant**、反复重接
（`0x4603 → 0x4611 → 0x4616 → 0x4617`）。**修复 A 已落地**：设备侧等待只对"已提交"的承载者编码；
等不到（>2 ms）⇒ 返回 0 并退回**有界主机等待**，计数 `handshake=waits/timeouts/max`。

---

## 3. 下一步（开工清单）

### 3.1 第一件：飞 `p15-conc2`（**用户飞**，已在离线侧准备好）

```bash
# 起腿前：pgrep -x gnb / pgrep -x ul_chain_replay / lsof -nP -iUDP:2152 都要干净
sudo -E OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_METAL_GPU_TIME=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p15-conc2 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# 流量：CN 侧（10.45.0.1）上行 iperf3 -c <gNB-ip> -R -t 100      ← -R 不能省
# ⚠ 跑完 Ctrl-C 后【确认进程真的退出】（p14 的报告被停机 5 s 宽限吃掉了）
# 判读：bash doc_chinese/phy_latency/wip/p0_gate.sh p15-conc2      # D1–D13
```

> ⚠ **测量臂声明**：`OCUDU_METAL_GPU_TIME=1` 给**每条**命令缓冲加一个完成处理器（诊断腿；不改提交 ⇒ `cbs/lane` 不受影响）。

**分支表（开发文档 §6.19 ⑥ 的简版）**

| 读数 | 通过（修复 A 生效）时应看到 |
|---|---|
| **D1/D3/D4**（判据，阈值不动）| `input hold` / `pop_blocking` / gaps **不再有 ~5 s 尾巴** |
| **D13（新）** | `handshake waits>0` = 那条腿里 Q9-G 窗口**真的被踩到**（旧腿的 5 s 来源）；`timeouts` **必须为 0** |
| D11（Q9-F）| `waiter-committed-first=0`；若非 0 ⇒ 还有一条等待路径没被握手覆盖，把 `worst kind`/`slot` 交回来 |
| D12（Q9-F3）| 运行期不再有 ~5 s 的**洞** |
| 用户侧 iperf3 | 不断流；偶发丢槽也不再让 UE 掉线重接 |
| 读不出 | 腿没带 `OCUDU_METAL_GPU_TIME=1`、或报告被停机吃掉、或戳 ≠ HEAD ⇒ **按 RED 算**，重飞 |

### 3.2 第二件（可选、便宜）：`OCUDU_CE_LANE_ORDER=event` 臂

让 Q9-C 真正执行（默认 `merged` 路径上它零执行）：`own=` 应显著 >0；`cross_lane>0` 且**无 5 s 停顿**才是 Q9-C 的空口证据。
**测量臂**（会改提交形态，`cbs/lane` 不保证仍 2.00，必须在腿报告里写清）。

### 3.2b 修复 B（**需用户裁决**，§6.20 ⑤ B）

握手去掉了**触发**，但池一干仍会把**整个 gNB（含下行）**停住 5 s 并把该 UE 打进"掉线重接"。
池是**上行链路的背压**，不该同时是**电台的时序源**。三个选项：(a) 预留少量只给接收路径的缓冲；
**(b) 池干时收到临时缓冲并丢弃该槽的样点（建议：背压与时序解耦，不增加提交数）**；(c) 缩短持有期（只降概率）。

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
