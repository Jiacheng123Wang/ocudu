# Session handoff — 2026-09-24 #1

> 读这一份就能开工。**当前目标已从"P0 仪表"切换到"P2-E：输入缓冲寿命解耦"**（见 §6）。
> 本 memo 的每条读数都来自本会话的腿或离线运行，出处见 §4/§5；未验证的事我标了"未验证"。

## 0. 一句话现状

`gpu` 融合车道的 **P0 仪表已补齐并全部验证**（P0-6 车道并发度可读、P0-1 分组 GPU 时间可读，离线和空口双证）；
用它们**定位并单变量证实了 n1 的 2.85 倍时延退化 = "等串行车道"（`ce` 段）**：
把车道并发度 1→2，`ce` 从 **3228 µs 塌到 ~51 µs**、跨度中位 **5268 → ~2360 µs**、`stale` 5→0、池饥饿 102→35 ——**两次独立复现**。
**但同一改动也两次复现了一次 ~5 秒的收包停顿**（`radio sample continuity: 2 gaps`，每次丢 ~1.531 亿样点，伴随 5.0004 秒 residency 异常），
所以并发度目前**只是测量结论、不是交付**。最可能的共因是 **P2-E 要拆的那条链**（输入被持有到整跳完成 → 池抽干 → 接收线程被 `pop_blocking` 卡住）。
**下一步的顺序（用户 2026-09-24 指定）：先做 P0-5**——把相位探针与车道探针**键在同一事件上**（见 **§6.0**），**做完再实现 P2-E**（计划已写好：`doc_chinese/phy_latency/04_p2e_plan.md`，见 §6.1）。

## 1. 仓库/远端/标签状态

* 分支 `apple-silicon`，本会话末次提交 **`386e84193e`**（`phy_latency/04` P2-E 计划），已推送；工作区干净。
* 里程碑 tag（本会话产出，均已推送）：
  * `gpu_phy_iq2llr_full_gpu_pipeline` → **`e232d16003`**（首版；注解含 8 条预登记排除）
  * `gpu_phy_iq2llr_full_gpu_pipeline_p2` → **`6bfc6189dc`**（修订 2：加了默认工况腿与**全绿离线门**；PHY 代码与首版**完全相同**，只多文档与门脚本）
* ⚠ **起腿前必须对齐戳**：`run_leg.sh` 在"二进制戳 ≠ HEAD"时**拒绝起腿**。任何代码/文档提交之后：
  `touch build/hashes.h && cmake --build build --target gnb`
* ⚠ 若在**新 clone/worktree** 里跑：`.metallib` 是构建产物且被 git 忽略，缺了会**静默走宿主路径**（`run_leg.sh` 有预检会拒绝）。

## 2. 目标与阶段划分（当前进度）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 仪表补齐（P0-6 并发度、P0-1 分组 GPU 时间、P0-5 探针配对）| ✅ P0-6/P0-1 完成；**P0-5 未完成 ⇒ 下一会话的第一优先（§6.0）**|
| P1 | 单变量臂（P1-8 并发度已跑；其余见 `01_plan.md`）| P1-8 已跑出结果（§5.3）|
| **P2-E** | **输入缓冲寿命解耦** | **P0-5 之后做**（计划：`04_p2e_plan.md`，见 §6.1）|
| P2 其它 | P2-B（砍 D 项）、P2-A（提前启动估计器）、P2-D（按测得持有期定池容量）、P2-F（并发度作为交付）| 未开工；P2-F 需用户二次裁决 |
| P3 | 验收 V1–V5 | 未开始 |

## 3. 验收判据（**先写死，不许为过关而改**）

| # | 判据 |
|---|---|
| **V1** | `[ul_gpu_pipeline]` 中位 ≤ **2150 µs**（重载基线 2687 的 −20%）|
| **V2** | `starved_events == 0` 且 `held_max < pool` |
| **V3** | RF 失败 ≤ 10、`gaps == 0` |
| **V4** | **不许用提交数换时延**：`cbs/lane ≤ 2.00 (max=2)`、`dropped == 0`（用户裁决：**只约束交付，测量臂放行**）|
| **V5** | 契约 8/8、`crossings 0.00+0.00`、CRC KO% 不劣化 |

## 4. 本会话的 P0 交付（都已提交）

### P0-6 —— 车道的**生效并发度**可读（提交 `470316ab8d`）

两行 stderr，**在电台打开之前**打印 ⇒ 短跑 `gnb -c <配置>` 即可离线读到，**不必飞腿**：

```
[ul_lane_exec] PUSCH/SRS concurrency = 1 (auto-derived; bw=20MHz layers=1 ul_ratio=0.30; available cpus=14)
[ul_lane_exec] PUSCH lane executor: max_pusch_and_srs_concurrency=1, medium pool max_concurrency=5
               -> pusch_executor.max_concurrency=1 (a serialising STRAND: ONE PUSCH hop at a time …)
```

* **实测**：n78（`bw=20MHz layers=1 ul_ratio=0.30`）与 n1（`bw=5MHz ul_ratio=1.00`）**生效值都是 1 ⇒ 都是串行 strand**；上限 = 中等池 `max_concurrency=5`（**超过池子是配置错误，不夹取**）。
* **教训**：n78 的 `ul_ratio` 是 **0.30**（TDD 图案来自"公共小区"层），据此推出来是 **1** 而不是我曾手算的 3 ⇒ **推导输入必须读出来**。
* 单测：`tests/unittests/du_low/du_low_executor_mapper_test.cpp`（4 例；标签不含 `phy`，不改审计门 193 的计数）。

### P0-1 —— 诊断开关 `OCUDU_LANE_DIAG_SPLIT`（默认关，提交 `4dcb03e3d5`、`f148808e3d`、`ebf3951920`）

* 在 `shared_burst::adopt()` 处**不再接管前端那条缓冲**，而是**提交它**（其 GPU 时间即成为 `gpu_lane_probe` 里**早已存在却从未注册**的 `dft` 段）→ 用 `MTLSharedEvent` 排序后开新缓冲继续。
* **离线验证载体**：`dft_release_adopt_metal_test`（它走 release/adopt 路径；CE 单测与 `ul_chain_replay` **都到不了 `adopt()`**）
  * OFF：`cbs/lane=1.00` + 两条断言全 PASS（含 *"one command buffer, one commit"*）
  * ON：`cbs/lane=2.00` 且 `busy split: **dft=37.5us/lane (83% of busy, cbs/lane=1.00)** eq_demap=7.5us/lane (17%)`
* **空口**（`s86-diagsplit`，n78 加压）：`dft=937.9 µs/lane (88%)` + `ch_wt=41.9 (4%)` + `merged_hop=87.8 (8%)`，各组之和 **1067.6** vs 出厂臂 `merged_hop` **1032.7** ⇒ **比值 1.034（B2 通过，±10%）**。
* **该臂的第二重身份（写死）**：事件排序比生产路线**更严格**，会**掩盖坑 36 的别名陷阱**（测试因此拒绝为其"共享臂"背书）⇒ **不得**用它判生产正确性，**只**用于分组 GPU 时间。
* **污染项**：一跳 2 条缓冲（`cbs/lane≈2`，V4 不管诊断臂）；前端输入缓冲**更早释放**（正是 P2-E 的题目）⇒ 不得用该臂读跨度/池/V1–V5。
* **⚠ 命名陷阱**：拆分臂里第二个缓冲仍被标成 `merged_hop`，含义是"**前端之后的全部**"（请当 `rest_after_front_end` 读）。
* **已修的缺陷**：`adopt()` 原先是"先提交旧缓冲、再建新缓冲"，建失败会穿透到已提交缓冲上（二次提交 = Metal 硬错误）；现改为**先建后提交**，建失败则**完全跳过拆分**、退回生产形态（提交 `ebf3951920`）。

### P0 读数门（只读日志、**可在用户飞腿时安全运行**）

`bash doc_chinese/phy_pipeline_gpu/wip/p0_gate.sh <腿> [--vs=<出厂臂>]`
判据：**A1** 两行 `[ul_lane_exec]` 存在且"生效值 == `pusch_executor.max_concurrency`"；**A2** 形态与值一致（`≤1`⇒STRAND，`>1`⇒fork）；
**B1** 拆分臂必须有 `dft=`、出厂臂必须没有；**B2** 各段之和在出厂臂 `merged_hop` 的 ±10% 内；
**C1**（只在腿自报 `OCUDU_UL_PHASE_SEGMENTS=1` 时判）相位分段确实被记录；P0-5 的配对**只报不判**（没登记过阈值）。

## 5. 本会话的关键读数（下一步判断全靠它们）

### 5.1 未解释 → **已定位**的 n1 退化（`00_status.md` §7 与 §7.1）

| 腿 | 小区/工况 | 跨度中位 | 池 `starved_takes` |
|---|---|---|---|
| `s47`/`s49`/`s51` | n1，**合并前** | **1844 / 1849 / 1853 µs** | — |
| `s61`/`s62` | n1，合并前 | 1896 / 1820 µs | 39 / 1 |
| `s69`/`s71` | n78，合并后 | 3113 / 3116 µs | 0 / 21 |
| `s83-tag`/`s84-p0`/`s84b-p0` | **n1，本会话** | **5263 / 5260 / 5251 µs** | 696 / 1192 / **38454** |

* 三条新腿**中位几乎逐位相同**（负载差 2 倍、饥饿差 55 倍）⇒ **负载无关的结构项**；`[ul_rx_wait]` 全时期都是 **1051–1057 µs**（逐位相同）⇒ **退化不在收样点**；GPU 的活基本不变（`merged_hop` busy 572–603 µs）。

### 5.2 **单变量实验：原因就是 `ce`（等串行车道）**

n1 默认配方 + `OCUDU_UL_PHASE_SEGMENTS=1`：

| 腿 | 车道并发度 | 跨度中位 | `t2f` | **`ce`** | `eq_demap` | `stale` | `starved_takes` |
|---|---|---|---|---|---|---|---|
| `s87-n1phases` | **1**（P0-6 实测）| **5268** | 1093.0 | **3228.3** | 905.5 | 5 | 102 |
| `s88-laneconc2` | **2** | **2400** | 1094.6 | **55.6** | 1232.9 | 0 | 35 |
| `s88b-laneconc2`（复跑）| **2** | **2359** | 1092.5 | **50.6** | 1228.2 | 0 | 35 |

* 三段和占跨度 **99.2%**（5226.8 vs 5268）；`t2f` 不动（= rx_wait 1052 + 前端主机 ~41 µs）。
* ⇒ **`P1-8`（并发度）是真实且可复现的杠杆**：`ce` −58~64×、跨度 −55%、`stale` 0、池饥饿 102→35。

### 5.3 ⚠ 代价也复现两次（这是当前最大的未决问题）

并发度 2 的两条腿都是 **`contract NOT MET: 1 of 8`**，失败项**同一条**：

```
[phy_pipeline]   radio sample continuity: 2 gaps over ~175k blocks (153,162,026 / 153,109,328 samples missing or repeated) -> FAILED
```

* **≈1.531 亿样点 ≈ 5 秒**，并伴随 **5.0004 秒**的 `[ul_gpu_lane] residency max`（5,004,199 / 5,004,062 µs）。
* 设备侧在并发 2 下**全部 OK**：`ce device estimates: 369065 device, 0 host`、`host sample assembly` 仅 2 次拷贝、crossings/wraps 0 ⇒ **红的是收包侧**。
* ⇒ **不是"复跑即消"的偶发项**；**在查清前 P1-8 只能作为测量结论，不能作为交付**。

### 5.4 P0-5（探针配对）仍未完成

`s85-p0phases`（n78 加压 + 相位分段）：三段各 **60389** 样本、中位 524.7 / 908.0 / 1231.0 µs、和 **2663.7** vs `[ul_gpu_pipeline]` **2688.6**（99.1%）；
但**车道 residency 有 142022 样本 ⇒ 相位/车道 = 0.425** ⇒ "busy ≈ residency / eq_demap ≈ residency"这类比较**仍只能算指示性**。要配对需改代码把两者键在同一事件上。

## 6. 下一步（顺序已按用户指示改为：**先 P0-5，再 P2-E**）

### 6.0 ★ 第一优先：**P0-5 —— 把相位探针与车道探针配对**

**要解决的问题**：`[ul_time_frequency]` / `[ul_channel_estimation]` / `[ul_equalization_demod]` 与 `[ul_gpu_lane] residency/busy`
**不是同一个样本总体**（`s85-p0phases`：三段各 **60389**，车道 residency **142022** ⇒ 比值 **0.425**），
所以"residency 里 ~95% 是 busy"、"`eq_demap` ≈ residency"这类说法目前**只是指示性**，
而 D 项（本跳设备执行）的预算恰恰要用这个分母。

**锚点（读码定位）**

| 侧 | 位置 | 现状 |
|---|---|---|
| 相位三段 | `include/ocudu/support/executors/ul_pipeline_probe.h`（`record_start` / `record_t2f_end` / `record_ce_end` / `record_ldpc_start`，以及按 slot 的 `pending_*` map；三段在 `record_ldpc_start` 里组装，且**只为 CRC-OK 的 TB 出样本**）| 按 **slot** 记，且被"必须与一次 CRC-OK 配对"过滤 |
| 车道 | `lib/phy/metal/ocudu_metal_lane_probe.{h,mm}`（`register_commit(cb, stage)` → 本线程 `pending`；`close_lane()` 归属到各段；报告在 `print_*`）| 按**线程/提交**记，每个跳（含未完成/未配对的）都算 |

**做法（形态）**：让两侧**键在同一个事件上**——最直接的是**按 slot 配对**：
在车道侧为每个 `lane_entry` 记下它所属的 **slot**（`submit_slot_grid_write`/`adopt` 路径已知 slot；若不能，则用"同一线程 + 同一 hop 的提交序列"作键），
在报告里输出**配对后的**分布（`[ul_gpu_lane] paired with the phase segments: n=… residency median … busy …`），
并明确打印"配对样本数 / 相位样本数"。

**判据（先写死）**
1. 配对后的样本数 **== 相位三段的样本数**（同一条腿上，例如 `s85` 型的腿应为 n=60389 量级，而不是 142022）；
2. `busy/residency` 与 `eq_demap/residency` **在配对样本上重算**并打印（**不预设阈值**，但必须说明是否仍 ≈95%）；
3. **零数据面影响**：这是**报告/探针改动** ⇒ `ab_dumps`（逐字节）、`value_net`、`l1_*` 臂、`ctest -L phy`、契约 8/8 必须全绿；**不动提交数**（V4）。

**验证**：离线编译 + 上述网；再飞**一条 n1 默认腿 + `OCUDU_UL_PHASE_SEGMENTS=1`**（约 2 分钟，标准流量配方），看是否打出配对行与配对数。

**为什么它应该先做**：P2-E 的预期里"持有期"要用**直接读数**判断，而 P0-5 决定 `busy/residency` 这个分母能不能用；
且 P0-5 是**报告改动**、风险低、不烧新配方，做完后 P2-E 的判读才不靠指示性数字。

### 6.1 第二优先：**实现 P2-E**（`doc_chinese/phy_latency/04_p2e_plan.md` 是完整计划）

**要解决的问题**：输入缓冲的持有期 = **整跳跨度**（keepalive 挂在整条命令缓冲的完成上，而 D1 之后整条缓冲就是整跳），
而**只有前端那一段读接收样点**（估计器/均衡/解映射读的是**网格**）⇒ **多持有** → 池抽干 → 接收线程被 `pop_blocking` 卡住（§5.3 那次 5 秒停顿的头号嫌疑）。

**两处锚点（读码核实）**

| # | 位置 | 改什么 |
|---|---|---|
| **E-1** | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm` 的块关闭/交出路径（`release_block()`，`~:1160-1235`）| 在**最后一次网格写之后、关编码器之前** `encodeSignalEvent:token_event value:generation` |
| **E-2** | 同文件的 `arm_tokens_on_complete()` | 开关打开时改由 `notifyListener:atValue:` 释放该块 tokens；**保留完成处理器作兜底**（信号没到就按完成释放，防泄漏）|

* 计数与 attach 处：`retain_for_block()`、`dft_stats_keepalive()`（`:114-118`、`:161-170`）。
* 开关：`OCUDU_DFT_RELEASE_TOKENS_EARLY=1`，**默认关**；反向臂 = 现行为。
* **不新增提交、不新增命令缓冲 ⇒ 不动 V4**。
* **前提必须先验**：除前端外**没有**任何 dispatch 读接收样点——用 P0-1 的拆分臂交叉验证（`dft` 组就是前端那一段）。

**验证（离线，不烧腿）**：`value_net`（容差+NO-NaN+LLR 符号）、`ab_dumps`（逐字节）、`l1_handover_arms`/`l1_hop_arms`、`edge_block_arms`、`dft_release_adopt_metal_test`（OFF/ON 两臂）、`ctest -L phy`、契约 8/8。

**预期（先写死）**：持有期 跨度→前端那段；池 `held_max` 8→**≤3**、`starved_events`→**0**（V2）；那次 **5 秒停顿消失**（`gaps=0` 且 residency max 回到 ms 量级）；`cbs/lane` 保持 **2.00**（V4）。
**判读**：停顿消失 ⇒ 因果坐实，本项成为并发度之外的第二个候选交付；停顿仍在 ⇒ 与持有期无关，下一个嫌疑是"并发 2 时同一 strand 上的邻居任务"。

**之后的一条腿（约定）**：n1 默认配方 + `OCUDU_DFT_RELEASE_TOKENS_EARLY=1`，再一条同样加并发 2，看 §5.3 的停顿是否消失。

## 7. 跑腿与判据的操作规范（本会话用血换来的）

```bash
# 0) 起腿前（run_leg.sh 也会硬检查；它现在按进程名精确匹配，不会误伤 shell）
pgrep -x gnb || echo ok; pgrep -x ul_chain_replay || echo ok; lsof -nP -iUDP:2152 || echo ok

# 1) 默认腿（n1，判契约 8/8、stale=0、A1-2）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>
#    流量：CN `ping 10.45.0.10 -i 0.1 -c 100` → 10s 下行 iperf3 → 10s 上行 iperf3 → 空跑到 ~100s，一次 Ctrl-C
#    ⚠ 零流量的腿无效：没有 PUSCH 跳时契约会少判两条（读成 MET (6 of 6)），而判据要 MET (8 of 8

# 2) 加压腿（n78，判 V1–V5）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> --regime=stress
#    ⚠ 加压配方 `iperf3 -c <phone> -R -b 40M -P 4 -t 240`：`-R` 不能省（否则变成下行负载）

# 3) 判据
bash doc_chinese/phy_pipeline_gpu/wip/p0_gate.sh <腿> [--vs=<出厂臂>]   # P0 读数（只读日志）
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 <腿>    # ⚠ 只用于加压腿（默认腿上它的两条 VALIDITY 必然红）
bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh               # 里程碑门（~3 分钟，互斥锁）
```

**纪律（每条都是本会话栽过的）**

1. **判据有三个绑定维度**：**工况**（默认 vs 加压，`--regime=` 让腿自报）、**流量**（零流量腿无效）、**几何**（A1-2 的 C4 是 n78 PRAT 几何特定的；n1 上记"未判"）。
2. **不许为过关改阈值**；**"读不出"按 RED 算**；**单次红不是证据**（先复跑一次，且 detail 必须保留首次读数）。
3. **任何代码改动都会让"腿的提交证据"失效**（门会报 `the commit it ran, vs HEAD`）⇒ 代码阶段之后要重新飞腿。
4. **不要在用户飞腿时跑任何碰 GPU 的东西**（门/单测/replay/短跑 gnb 都算）；先 `pgrep -x gnb`。**进程检查永远按名字**：`pgrep -f` 曾匹配到我自己的 shell，挡了用户一条腿。
5. 腿要**自己声明工况**：`run_leg.sh` 会把 `[leg] regime=` 写进腿的 stderr（**provenance 的 knob 行已修好换行**，早前粘在上一行导致行首 grep 读不到）。

## 8. 文档地图

| 文件 | 内容 |
|---|---|
| `doc_chinese/phy_latency/00_status.md` | 现象、归属式预算、机制、V1–V5、Q1–Q8、**§7/§7.1 退化定位** |
| `doc_chinese/phy_latency/01_plan.md` | P0/P1/P2 计划、§5.2 **用户的 V4 裁决**、P1-8 的定义 |
| `doc_chinese/phy_latency/02_measurement.md` | 仪表手册、复跑配方、**已知的坑**、工况声明规矩 |
| `doc_chinese/phy_latency/03_p0_instrumentation.md` | P0-6/P0-1 的读数、读法、命名陷阱、臂身份、缺陷与修法 |
| **`doc_chinese/phy_latency/session_handoff_2026-09-24-1.md`** | **本 memo（新会话只读这一份即可开工）** |
| `doc_chinese/phy_latency/04_p2e_plan.md` | P2-E 的完整实施计划（**P0-5 之后**据此开工）|
| `doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` | 主设计文档（**追加式**，§5.9.131–§5.9.138 是本会话记录；行号会漂，按句子检索）|
| `doc_chinese/phy_pipeline_gpu/wip/` | 门与工具：`milestone_audit.sh`、`p0_gate.sh`、`leg_gate.sh`、`run_leg.sh`、`ab_dumps.sh`、`ul_load.sh`、`wall_ab.sh` |

## 9. 仍挂着的事（按优先级）

1. **P0-5 配对**（§6.0）——**第一优先，用户指定**：把相位探针与车道探针键在同一事件上。
2. **实现 P2-E**（§6.1）——P0-5 之后。
3. **查 §5.3 的 5 秒收包停顿**（并发 2 下两次复现）；P2-E 若修掉它即因果坐实，否则转向"strand 上的邻居任务"。
4. **P2-F（并发度作为交付）需用户二次裁决**（V4 只约束交付，测量已放行）。
5. **`s84b-p0` 第一次尝试没有留下任何日志**（本仓与 `ocudu_premerge` 都没有）——最可能是被"戳 ≠ HEAD"拒绝（那种情况不产生日志）；用户未提供终端输出，**未验证**。
6. 里程碑门目前**依赖新腿**转绿：离线判据本来就全绿，红的只有"腿的提交证据"与新腿缺失那一类；本会话末次干净门是 `22 PASS / 3 FAIL / 4 RED`，**全部**来自那条被顶掉的无效腿 `s84-p0_0924_2254`。
