# Session handoff — 2026-09-25 #3

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 这条会话的第三份**（继 `…-1.md`、`…-2.md`）。
>
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"，
> **本会话的细节在 §6.19–§6.29**）；**高层现状与计划**在 `high_level_status_and_plan.md`；三分工与旧名映射见 `README.md`。
> **本 memo 只写"开工要什么"**：现状、读数出处、下一步、仍挂着的事。

---

## 0. 一句话现状（★ 断流已结案并两次空口验证；**主线 V1 仍未达，且杠杆方向在本会话末被更正**）

**① 断流/停顿：根因结案 + 两道修复都已在空口验证。**

* **根因**：Metal 对"**未被满足的设备侧事件等待**"有 **5.00 s 硬上界**——一个消费者若在承载其信号的
  命令缓冲**提交之前**就被提交，整条队列被按住 **5.00 s**（signaller 也跑不了），随后等待被**丢弃**（栅栏失效）。
  离线复现（四条臂）见 `wip/metal_wait_timeout_probe.mm`，推导见开发文档 **§6.20**。
* **修复 A（提交握手）**：设备侧等待只对"**已提交**"的承载者编码；等不到（>2 ms）⇒ 退回有界主机等待。
  空口验证 **腿 `p15-conc2`**：`handshake waits=24, timeouts=0, max=165us`（窗口真被踩到 24 次、全部关掉）、
  `waiter-committed-first=0 / 106,016 waits`、**23/23 全绿**、契约 **8/8**、**0 gaps**、满速上行 **105 s 无停顿**、
  四个"5 秒"读数全部落回 ms 级（§6.22）。
* **修复 B（池干 ⇒ 丢该槽，用户裁决 (b)）**：`pop_rx_buffer_or_reserve()` + 预留缓冲 + `rx_park_budget = 1 ms`。
  空口验证 **腿 `p21-n78-forcedrop`**（`OCUDU_UL_RX_POOL_DROP_FORCE=20` 强制走该路）：`dropped=20`、
  `drop_park_max=1001us`、`pop_blocking max` **3969 → 950 µs**、**0 gaps**、`assembled=0`、
  无 `Unexpected symbol index`、V1 **无惩罚**（§6.28）。

**② 但主线判据 V1 未达**（`[ul_gpu_pipeline]` 中位 ≤ **2150 µs**，重载基线 `s82` = 2675.1）：

| 腿 | p16 | p17 | p19 | p20 | p21（修复 B 后）|
|---|---|---|---|---|---|
| V1 中位 | 2428.1 | 2435.4 | 2413.1 | 2416.4 | **2440.0 µs** |
| vs 基线 | −9.2% | −8.9% | −9.8% | −9.7% | **−8.8%** |

⇒ 两条修复**都不惩罚中位**，但也**都没推动它**；−9% 就是并发 2 在 n78 上的全部收益（§6.23 ④）。

**③ 方向更正（§6.29，本会话最后一件事——下一条腿之前先读它）**：§6.25 ② 把拆分臂读成
"设备执行 87% 是前端 **DFT**（941 µs）"是**错的**。离线微基准（`wip/dft_kernel_cost.mm`，加载**生产** metallib）：
一个时隙的 **14 个 n=768 变换，放进一次派发只要 13.75 µs**（0.98 µs/变换）；**单 threadgroup 派发是延迟绑定的**（12.18 µs/次、互不重叠）。
前端走的正是单 threadgroup 派发（`dispatchThreadgroups:MTLSizeMake(1,1,1)`，每符号一次）⇒ 一个时隙 14 × 12.2 ≈ **171 µs**。
**所以 941 µs 是"占用窗口"（同队列缓冲重叠时包含别的缓冲的执行），不是算力**；真实算力 ≈ 171（前端，未批量化）
＋ ~140（CE+EQ+demap）≈ **300 µs/时隙**。

> ⇒ **V1 的第一杠杆 = 把前端 14 次派发合成 1 次**（离线 171 → 13.75 µs，见 §3.1，**不用烧腿**）；
> **第二杠杆 = 按修正后的算力重核 C 项**（§3.2，也不用烧腿）。**"优化 FFT kernel"不是杠杆**（kernel 已经很快）。

**当前状态**：**没有任何腿在跑**；工作区干净，**HEAD = 本 memo 的提交**（写这份时 HEAD 是 `cdb8bd2440`，其后只加了文档）；
⚠ **构建戳停在 `028a30c4b7`**（此后三个提交都只改文档与一个不进 gnb 的离线 .mm）⇒ **飞腿前必须重建**（§3.3 第 0 步）。

---

## 1. 本会话（2026-09-25 #3）做了什么

| 提交 | 内容 | 出处 |
|---|---|---|
| `bf33445b89` | **Q9-F + Q9-F2 + Q9-F3** 三条仪器（提交票号 / 前端 deposit 块入探针 / 队列占用时间线）；`state()` 改为故意不析构（修 atexit 崩溃）；门加 **D11/D12**；metal 测试 **arm 14** | §6.19 |
| `d54dccdac4` | 交接 memo **`…-2026-09-25-2.md`** + README 索引 | — |
| `69e6684221` | 门的两个**读不出**的字段（D11 `kind`、D12 洞）修好 + **`p0_gate_selftest.sh`**（本可抓到它们） | §6.19 ⑤ |
| `18881ec040` | **5 秒的根因**（`wip/metal_wait_timeout_probe.mm` 四臂离线复现）**＋ 修复 A（提交握手）**；门加 **D13**；metal 测试 **arm 15** | §6.20/§6.21 |
| `ba74626db8` | **腿 `p15-conc2`**：修复 A 空口验证（24 次窗口全关、0 停顿） | §6.22 |
| `f348c54a90` | **腿 `p16-n78-conc2`**：n78 重载下**停顿没了**，但 **V1/V2/V3 未达**；12.3 Mbit/s 是**链路**的限制 | §6.23 |
| `3917060b1c` | **按需 dump**：接收线程 park 超 **20 ms** 时把 P0 读数**当场**打出来（防"停机吃掉报告"）；门加 **D14**；metal 测试 **arm 16** | §6.24 |
| `7f681f4a8e` | §5.4 记录**CE Metal 测试**的争用 flake（串行 ctest 跟在其他 Metal 测试后） | §5.4 |
| `7666360289` / `8836a235f4` | **腿 `p17`（确认）+ `p18`（Q1 拆分臂）**：V1 复现 2435；拆分臂给出 `dft=941.2 µs`（**后被 §6.29 更正**） | §6.25 |
| `5170a2bcb1` | **修复 B**（用户裁决 (b)：池干丢该槽，不 park 电台） | §6.26 |
| `e86f2f8fa7` | **腿 p19/p20 的 A/B 判不了修复 B**（两条腿都没到阈值）——但抓到它**自己的缺陷**：10 ms 等待片让 1 ms 预算不可达 ⇒ `wait_slice=min(slice,budget)` + `static_assert` | §6.27 |
| `028a30c4b7` | **腿 `p21-n78-forcedrop`**：修复 B 空口验证（`dropped=20`、park ≤1001 µs、0 gaps、无惩罚） | §6.28 |
| `cdb8bd2440` | **§6.29 更正**：941 µs 是**占用窗口**不是算力；`wip/dft_kernel_cost.mm` 离线微基准 | §6.29 |

**腿一览（`doc_chinese/phy_pipeline_gpu/wip/logs/`，本会话全部）**

| 腿 | 工况 | 一句话读数 |
|---|---|---|
| `p14-conc2` | n1 满速 | 5 s 停顿；**报告被停机吃掉**（⇒ 催生按需 dump，§6.24）|
| `p15-conc2` | n1 满速 105 s | 修复 A 通过：24 次窗口全关、0 gaps、23/23 |
| `p16-n78-conc2` | n78 stress conc2 | 0 gaps、契约 8/8；**V1 2428 / V2 `starved=114` / V3 RF 失败 1490** |
| `p17-n78-conc2` | 同上（确认）| V1 **2435.4**（逐位复现 ⇒ "未达"是确定的）；park 3969 µs ↔ 一个 gap（D4 的**主机侧**成因）|
| `p18-n78-split` | 同上 + `OCUDU_LANE_DIAG_SPLIT=1` | **测量臂**（多一次提交）：V1 2577.8 不可比；`dft` 窗口 941.2 µs/lane（87%）⇒ §6.29 更正的对象 |
| `p19-n78-drop` / `p20-n78-nodrop` | 修复 B 的 A/B | **两条腿都 `dropped=0`**（都没触发）⇒ 判不了；V1 2413.1 / 2416.4 |
| `p21-n78-forcedrop` | 同上 + `OCUDU_UL_RX_POOL_DROP_FORCE=20` | 修复 B 通过：`dropped=20`、park 1001 µs、0 gaps、V1 2440.0 |

---

## 2. 已证明 / 已证伪（本会话增量）

| # | 结论 | 证据 | 出处 |
|---|---|---|---|
| 15 | **那 5 秒是 Metal 的等待上界**，不是"队列次序"本身；signaller 永不提交时等待者也在 5.000 s 被放行（栅栏失效）| 离线四臂 A2/A4/A5/B（`wip/metal_wait_timeout_probe.mm`）| §6.20 |
| 16 | **触发条件 = 消费者先于 signaller 提交**；修复 A 覆盖四个提交点 | `p15`：窗口被踩 24 次、`waiter-committed-first=0`、0 停顿 | §6.21/§6.22 |
| 17 | **n78 重载下停顿消失**（0 gaps），但 **V1/V2/V3 未达**；12.3 Mbit/s 是链路自身限制（BLER 21%、256QAM 40%），优于 cpu 腿与历史重载腿 | `p16` + `[ul_gpu_pipeline]`/契约 | §6.23 |
| 18 | **修复 B 有效**：池干时不再 park 电台（`pop_blocking max` 3969→950 µs），代价是丢一个块（`dropped=n`、`assembled=0`）| `p21`（强制臂 20 次）| §6.28 |
| 19 | **A/B 必须读"机制计数器"**：p19/p20 都没到阈值 ⇒ 只看症状（0 gaps、跨度）会误判"修复无效" | 两腿 `dropped=0`、`drop_park_max=0us` | §6.27 |
| 20 | **修复 B 自己的缺陷**（被 p19/p20 抓到）：`pop_wait_for()` 等满 10 ms 片 ⇒ 1 ms 预算**永不可达** ⇒ 改成 `wait_slice=min(rx_reap_slice, rx_park_budget)` + `static_assert` | 代码 + 自测 | §6.27 |
| 21 | ⚠ **更正 §6.25 ②**：`busy`/`busy split` 是**每条命令缓冲占用窗口的加和**，同队列重叠时**大于**算力 ⇒ **不能当算力账单**；941 µs ≠ DFT 算力 | 离线微基准：14 变换/派发 = 13.75 µs；单 threadgroup 派发 12.18 µs 且不重叠 | §6.29 |
| 22 | **前端每符号一次单 threadgroup 派发**是那 171 µs 的来源（不是 kernel 慢）| `ocudu_dft_metal_engine.mm` 的 `dispatchThreadgroups:MTLSizeMake(1,1,1)` + 微基准 | §6.29 |
| 23 | **kernel 已支持多 threadgroup**（`batch_offset = base + tgid*n`），`submit_at()` 已经按 `nof_transforms` 一次派发——但它**写侧是关的**（`gw.active=0`）⇒ 前端批量化要补的是**每符号写参数表** | `ocudu_dft.metal` + `ocudu_dft_metal_engine.mm` | §6.29 ③ / 本 memo §3.1 |
| 24 | **报告可以在"正在停顿"时读**：接收线程 park 超 20 ms ⇒ 当场 dump（`p0_dump_reports`）| `p17` 现场 dump；门 **D14** | §6.24 |
| 25 | `p15` 与 `p16–p21` 的**握手都是 `waits:0`**（除 p15 的 24）⇒ Q9-G 窗口是**工况相关**的，重载腿不踩它；"没停顿"因此有两重保证（握手 + 池不再 park）| 各腿 `handshake=` 行 | §6.22–§6.28 |
| 26 | **D4（gaps）是 flaky 且至少两个成因**：主机侧 park（`p17`：3.97 ms park ↔ 4.38 ms 缝隙）与电台侧（`p20`：park 仅 235 µs 却丢 163,495 样点、2 次 RF overflow）| 两腿对照 | §6.25/§6.27 |
| 27 | **CE Metal 单测的争用 flake**：串行 `ctest -L phy` 紧跟其他 Metal 测试时 `port_channel_estimator_metal_mmse_unit_test_ta_chain` 会 Bus error/红；**重跑即绿**，按"保留第一次读数"记录 | 今日两次 | §5.4 |

**仍然绿**（每次改动后都重跑）：`ctest -L phy` **193/193**、`lower_phy_test` **528/528**（含 `OCUDU_UL_RX_POOL_DROP_FORCE=3` ⇒ `dropped=3`）、
metal 测试 **arm 10–16 rc=0**、`l1_handover_arms.sh` **5 PASS**、`p0_gate_selftest.sh` D11–D15 全绿。

---

## 3. 下一步（开工清单）

> **顺序是有理由的**：§3.1/§3.2 **不用烧腿**（离线可判 / 纯读），做完再飞一条 `p22` 才有信息量。
> **不要**在做完 §3.2 之前再飞"和 p16 一样的腿"——那只会再得到一次 2428。

### 3.0 开工先做（两分钟）

```bash
cd /Users/jiachengwang/dev/ocudu
git log --oneline -1                    # 期望 cdb8bd2440 或其后
pgrep -x gnb; pgrep -x ul_chain_replay; lsof -nP -iUDP:2152    # 都要干净（飞腿前）
```

### 3.1 第一件（**推荐**，离线可判）：把前端 14 次单 threadgroup 派发合成 **1 次 14-threadgroup 派发**

* **为什么**：那是 §6.29 认定的 V1 第一杠杆（离线 171 µs → 13.75 µs），而且它**同时**省掉 13 次
  `setBytes`/`encode` 的主机工作（B 项的一部分）。
* **代码入口**：`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm`（前端编码路径的
  `dispatchThreadgroups:MTLSizeMake(1, 1, 1)`）＋ `lib/phy/generic_functions/metal/ocudu_dft.metal`（kernel 已用 `tgid` 索引输入偏移）。
* **要做三件事**：① 一个**整槽**的输入 wrap（14 个符号的样点在一次派发里按 `base + tgid*n` 索引）；
  ② 每符号的写参数（`active / nof_subc / dst_offset / map_offset / phase_re,im / apply_window`）**不能再放 `setBytes`**
  ⇒ 绑一张 **14 项的常量表**（小 MTLBuffer，`tgid` 索引）；
  ③ 记账照抄到位：`open_transforms += 14`、`note_slot_submission()` 仍**每符号**记（等待/交棒的粒度是**槽**）。
* **验收（离线，先做）**：`doc_chinese/phy_latency/wip/dft_kernel_cost.mm`
  —— ① `xforms=14` 行必须保持 **~14 µs**；② **新增一条"写侧打开 + per-tgid 表"的臂**（现有 14 行是 `grid_write` 可切的一个轴，
  但只有**一个** `dst_offset` ⇒ 还没覆盖"14 个符号各有各的目标"）；③ 反向读数已有：**200 次单 threadgroup 派发 = 200 × 12.2 µs 不重叠**。
* **建议加一条 A/B 旋钮**（**不预设默认值，先按"关"落地**）：`OCUDU_DFT_BATCH_SYMBOLS=1|14`
  ⇒ 一条腿内同配方对比，避免"回退提交"。⚠ 批量化**不应改 `cbs/lane`**（仍是一跳一个前端块、一次提交）——但这要用 **V4 读数**证明。
* **空口验收**：`p22-n78-conc2`（配方与 p16/p17 **逐字相同**），读 **V1 中位**、`[ul_gpu_lane]` 前端块窗口（Q9-F2 的 carried 块）、契约 8/8、D1–D15。
* **预登记的两种结果（都要接受）**：
  * V1 下降接近 **~157 µs**（前端窗口那一段在关键路径上）⇒ 杠杆确认，继续同样的"窗口变短"改造（CE/EQ/demap）；
  * V1 **不动** ⇒ 前端窗口**已被前一跳重叠掉**，关键路径是 **A（等样点 ~473）与 C（排队）** ⇒ 立刻转 §3.2 的结论去打 A/C，
    **不要**再在前端上花时间（这条读数本身就是答案）。

### 3.2 第二件（不用烧腿，纯读+算）：按修正后的算力**重核 C 项**，并更新 §7.1 的预算表

* 已知：算力 ≈ **300 µs/时隙/车道**，而需求 ≈ **520 跳/s**、2 条车道 ⇒ 算力上"不该排队"；
  但实测跨度 **2428 µs**、车道占用 ~72% ⇒ **C 是"驻留窗口在排队"，不是"算力在排队"**。
* ⇒ 要重写的是**读法**：`residency` 里有多少是"本跳算力"、多少是"别的跳的重叠"；
  A/B/C/D/E 各自是**窗口**还是**算力**。这一步的产出直接决定下一个改哪里（以及 **P2-F** 并发度值不值）。
* 输入都在手上：`[ul_gpu_lane]` 配对读数、§7.1 的归属表、§6.29 的更正、`wip/dft_kernel_cost.mm`。

### 3.3 第三件：腿 `p22-n78-conc2`（§3.1 的空口验收；做完 3.1 再做）

```bash
cd /Users/jiachengwang/dev/ocudu
# 0) 戳必须 = HEAD（run_leg.sh 会拒跑；§6.29 只改了文档/离线工具，所以戳落后一个提交）
cmake --build build --target ocudu_versioning && cmake --build build --target gnb
grep -oE '[0-9a-f]{10}' build/hashes.h | head -1      # 必须等于 git rev-parse --short=10 HEAD

# 1) 起腿（不要带 OCUDU_METAL_GPU_TIME=1：它会扰动提交路径，判 V1–V5 的腿一律不带）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p22-n78-conc2 \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
#    流量（CN 侧，地址按当次 gNB）：iperf3 -c 10.45.0.4x -R -b 40M -P 4 -t 240

# 2) ⚠ 用【单次 Ctrl-C】停，并**确认进程真的退出**（atexit 才写报告；p14 的教训）

# 3) 判读
bash doc_chinese/phy_latency/wip/p0_gate.sh p22-n78-conc2                      # D1–D15
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 p22-n78-conc2  # V1–V5
#    ⚠ leg_gate 的 `stale=0` 与 `UL grant ≥50%` 两个 FAIL 是【绑错工况/几何】，不是本工作的账（§6.23 ③）
```

### 3.4 仍挂着的事（按优先级）

1. **V2（未达）**：`starved_events` 52–114、`held_max=8/8`、`free_min=0`。根因指向**持有期尾巴 20–105 ms**
   ＝"**一个没被认领的块落在上行静默窗口里，没有任何注册点去评估清扫的 10 ms 截止**"（单块、无害但计数，§6.27 ④）。
2. **V3（未达）**：n78 腿 RF 失败 **1000–1500**（阈值 ≤10），绝大多数是 `underflow`；已确认是 **GPU 模式现象**
   （cpu 腿 276 s 只有 1 次），且**并发 2 把它变成三倍**（1.04% vs 0.34–0.38% 的 grant）。修 B 只改了"池干的后果"。
3. **D4 的第二个成因**（电台侧）：`p20` 是 park 仅 235 µs 却丢 16 万样点的证据 ⇒ 与主机 park 无关的一条路。
4. **MAC 侧恢复**：丢失的 CRC 指示会让 UE 陷入 HARQ/CRC 饥饿、不再被授 Grant（`p14` 的掉线重接链）——**独立缺陷，未修**。
5. **用户裁决仍挂**：**P2-F**（并发度作为**交付**，不是测量臂）需**二次裁决**；**P2-E (b)** 维持原判（⛔ 被平台证伪）；
   **P2-D**（池按流水线深度**定尺**而不是固定 8）**未开工**，且必须在 V1 达成后按**新时延**重算。
6. **两条陈旧网**（改前就红）：`value_net.py`（归档基线陈旧）与 `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` 差 15/27 capture）。
7. **未做的小项**：**Q12**（腿 `s84b-p0` 无日志）、**P1-6**（`OCUDU_UL_SLOT_TRACE` 拆 B 项 48 µs）。

---

## 4. 纪律（每一条都花过代价）

1. **判据不许为过关改阈值**；**"读不出"按 RED 算**；**单条腿/单次读数不是证据**（p16→p17 的逐位复现才是）。
2. **用户飞腿时绝不碰 GPU**（门/单测/replay/短跑 gnb 都算）；进程检查**按名字**（`pgrep -x gnb`）。
3. **腿必须 Ctrl-C 停**并**确认退出**（`kill -9` 丢掉 atexit 的报告行；p14 因此丢过报告）。
4. **代码改动会让"腿的提交证据"失效** ⇒ 改完必须重建（**戳 = HEAD**）再飞；`run_leg.sh` 会拒跑不一致的戳。
5. **网不许与并行构建同时跑**（重链接中的二进制会读成"偶发失败"）。
6. **A/B 腿要先确认"机制真的被触发"**（读计数器：`dropped=`、`handshake=`），否则"没差别"不是结论（§6.27）。
7. **测量臂不进判据**：`OCUDU_LANE_DIAG_SPLIT=1`、`OCUDU_METAL_GPU_TIME=1`、`OCUDU_UL_RX_POOL_DROP_FORCE` 都是诊断臂。
8. **保留第一次读数**（flaky 测试重跑绿也要记，§5.4）。
9. 需要留存的开发产物放 `doc_chinese/work_tmp/`（git 忽略）；引用外部材料**用章节名/腿名，不用行号**。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§3 判据（**V1–V5**）、§4 仪表手册、§5 跑腿规范、**§6 追加式实施记录（本会话 = §6.19–§6.29，§6.29 是方向更正）**、§7 杠杆、§8 未决 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5、下一步、待裁决（已同步到 §6.29）|
| `doc_chinese/phy_latency/wip/p0_gate.sh` | **P0 门（D1–D15，只读日志）**——判据与读数的唯一入口；`p0_gate_selftest.sh` 是它的自测 |
| `doc_chinese/phy_latency/wip/dft_kernel_cost.mm` | **§3.1 的离线验收工具**（生产 metallib 的派发形状/开销；`xforms=14` 行是判据）|
| `doc_chinese/phy_latency/wip/metal_wait_timeout_probe.mm` | Metal 5.00 s 等待上界的离线复现（四臂）|
| `doc_chinese/phy_latency/wip/leg_census.py` | 腿普查（UL 静默表 + RF/pool 事件）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` / `leg_gate.sh` | 起腿（硬检查戳）/ V1–V5 判读；**腿日志在 `…/wip/logs/`** |
| `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm` + `ocudu_dft.metal` + `ocudu_dft.metallib` | **§3.1 的改造对象**（前端派发形状 + kernel 的 `tgid` 索引）|
| `lib/phy/metal/ocudu_metal_queue.{h,mm}` | 两道队列 + 三道设备侧栅栏 + Q9-D 次序仪 + **Q9-F 提交次序仪** + **Q9-F3 占用时间线** |
| `lib/phy/metal/ocudu_metal_burst.{h,mm}` | 交棒注册表：**修复 A 的 `commit_issued` + `note_block_commit_issued()` + `wait_for_block_commit()`（2 ms）+ `handshake_stats()`** |
| `lib/phy/metal/ocudu_metal_lane_probe.{h,mm}` | 车道探针（最慢表 + **Q9-F2 前端 carried 块读数**）|
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` | **修复 B**（`pop_rx_buffer_or_reserve()`、`rx_park_budget=1ms`、`OCUDU_UL_RX_POOL_DROP[_FORCE]`、`dropped=`/`drop_park_max=`）|
| `lib/phy/support/phy_pipeline_report.{h,cpp}` + `include/ocudu/phy/phy_pipeline_report.h` | **按需 dump 注册表**（`register_p0_report` / `p0_dump_reports`，停顿时现场读）|
| `lib/phy/generic_functions/metal/test/dft_release_adopt_metal_test.mm` | 离线臂（**arm 14 = Q9-F/Q9-F3、arm 15 = 握手、arm 16 = 按需 dump**）|

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-25-3.md`。
> **断流已经结案**（根因 = Metal 5.00 s 等待上界 + 消费者先提交；修复 A/B 都已空口验证），**现在只打 V1**
> （`[ul_gpu_pipeline]` 中位 ≤2150 µs；p16–p21 实测 2413–2440，基线 2675，−9%）。
> **先读开发文档 §6.29**（它更正了 §6.25 ②：941 µs 是**占用窗口**不是 DFT 算力）⇒ 按本 memo **§3.1** 做
> "前端 14 次单 threadgroup 派发 → 1 次 14-threadgroup 派发"，**离线验收**用 `wip/dft_kernel_cost.mm`
> （`xforms=14` 行保持 ~14 µs，并新增"写侧打开 + per-tgid 参数表"的臂），再按 **§3.3** 飞 `p22-n78-conc2`。
> **不要**在 §3.2 的重核之前再飞一条"和 p16 一样"的腿。

---

## 7. 追加更正（本 memo 写完之后，同一条会话继续做的事）——**§3.1 已完成，下一步只剩飞腿**

> memo 的规矩是"写完只追加更正"，所以这里不改上面的正文，只把状态推进写清。

* **§3.1（前端 14 次派发 → 1 次）已实现并离线达标**：提交 **`a6b2d3f629`**，开发文档 **§6.30**。
  * 旋钮 **`OCUDU_DFT_BATCH_SYMBOLS=N`（默认 1 = 关）**；`N ≥ 2` 时把**开着的块**里走**电台 int16 输入**的变换**延迟**到块结束，
    用**一次** `MTLSizeMake(N,1,1)` 派发；kernel 用 `grid_write_params` 原有的 `pad` 字段做**多变换标志**
    （`N>1` ⇒ 两个参数块都按 `tgid` 索引）⇒ **kernel 签名不变、既有派发点不用改**。
  * **离线验收（§6.30 ②）**：`wip/dft_kernel_cost.mm` 新增"前端自己的形状"臂（电台 int16 + grid write 打开 + 每符号参数）：
    **14 次派发 159.77 µs/槽 → 1 次派发 10.61 µs/槽（15.06×，n=768）**，n=512 是 141.62 → 10.35（13.68×），
    **两种形状写出同一个网格（逐字节）**。
  * **在线自测（§6.30 ③）**：metal **arm 17** —— 同一条交棒路径、同一台引擎、旋钮 1 vs 14，
    网格**逐字节相同**且计数 `0->1` 次派发 / `0->14` 个变换（对照臂必须 0/0）。
  * **仪器**：`[metal_stats] dft … batched=<d>/<t> batch_max=<N>`；`dft_metal_engine::batch_stats()`；门 **D16**（INFO）。
  * **§3.2（重核 C）也已做完**，结论在 **§6.30 ⑤**：D 的**算力** ~300 → **~150 µs/槽**，
    **C（≈901 µs）是"驻留窗口在排队"、零算力**（ρ ≈ 0.73）⇒ 缩短任何固定窗口都**超线性**缩小 C；
    但 A（473）+ C（901）这 **1374 µs 零算力项**仍在 ⇒ **只砍算力打不到 2150**，剩下的必须来自 A 或 C 的结构项。
* **网（全绿）**：`ctest -L phy` **193/193**、`lower_phy_test` **528/528**（`OCUDU_UL_RX_POOL_DROP_FORCE=3` 同）、
  metal **arm 10–17 全 PASS**（旋钮开/关各一次）、`dft_processor_metal_unit_test` **ALL OK**、
  `l1_handover_arms.sh` **5 PASS**、`p0_gate_selftest.sh` **PASS**。
  ⚠ 一次 CE Metal `Bus error`（§5.4 的争用 flake）**保留第一次读数**，单独重跑 3/3 绿、整套重跑 193/193 绿。
  ⚠ **顺带修了自测自己的夹具前提**（不是门的问题）：`p0_gate_selftest.sh` 原先拿**最新**腿做夹具，
  而 p15 起每条腿都带 6.19 的行 ⇒ 反向判据与 D14/D15 **误红**；现在夹具取**最新的、不含 Q9-F 行的腿**（当前 `p14-conc2`）。
* **下一步（唯一的动作）= 飞 A/B 腿 `p22-n78-batch14`**（命令与预登记见开发文档 **§6.30 ⑥**）：
  配方与 p16–p21 **逐字相同**，只多一条 `OCUDU_DFT_BATCH_SYMBOLS=14`；
  **A 臂 = p16–p21**（默认 `batch_max=1`，无需再飞）。判读顺序：**D16 先确认机制跑了**（`batched=<d>/<t> batch_max=14`；
  `batched=0/0` ⇒ 这条腿不是 A/B）⇒ 再读 **V1 中位**（对照 2413–2440）、`[ul_gpu_lane]` 前端窗口、契约 8/8、
  **V4 `cbs/lane` 应不变**（批量化不改提交数）。
  **预登记**：中位**至少 −149 µs**（叠加 C 的放大可能到 ≈1950–2100）；**若不动 ⇒ 前端窗口已被前一跳完全重叠**，
  下一个杠杆是 **A（P1-7 符号级收包）**与 CE/EQ/demap 链，**不再**在前端上花时间。
