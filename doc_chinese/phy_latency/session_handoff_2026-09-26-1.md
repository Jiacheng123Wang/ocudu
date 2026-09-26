# Session handoff — 2026-09-26 #1

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份** —— 现在就是本文件）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），序号是那天的第几份。
> ★ **本会话按用户 2026-09-26 的指示命名为 `2026-09-26-1`**（今天的第一份；本会话的工作从 2026-09-25 晚跨到 09-26 上午，
> 上一份 `-5` 之后那份 `-6` 因**违反"memo 只在交接时写"的规矩被删除**，内容已并入开发文档，见 §5）。
>
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"）：
> **`-5` 之后新增的是 §6.62–§6.71 + §7.6（含 7.6.0/0b/0c/0d/1）+ §7.7**；`high_level_status_and_plan.md` 是高层现状；
> 三类文档分工见 `README.md`。**本 memo 只写"开工要什么"**：现状、读数出处、下一步（含已定好的施工方案）、仍挂着的事、纪律、文件地图、第一句话。

---

## 0. 一句话现状

**目标（V1 时延 + V2 池）早已结项**（§6.43）。本会话做了三件事：
**(1) 杠杆 C（K2 直读 LSE、`scatter` 消失）验收完成**：`p39` 机制全中（`scatter=0`、`lse_applies=142740`、`device_y_writes=0`、−1.455 派发/跳），
**`p40` 反向臂配对 A/B：V1 −21.5 µs**（落在预登记带内）⇒ **V1 中位 1364.2 µs（基线 2675.1 ⇒ −49.0%）**；
**(2) 三条"省时延"矿脉全部量尽**：**消派发 ≤20 µs、减少阶段边界 ≈0、重写 kernel ≈50 µs 封顶且没有值得动的**；
**(3) 转向结构项（单车道）**：`p41` 把 A 项（`P1-7` 符号级收包）**证伪为交付路径**（吞吐 ↓17×、前端批量化被拆），
并按用户裁决**改为"先把单车道自己优化好，再动并发"**（§7.6.1）—— 本会话已把这条路的**尺子补齐、机制读清、改动清单落定**，
**C1 已落地**，C2 待做、C3 判定不需要。

| 判据 | 阈值 | 现状 | 出处 |
|---|---|---|---|
| **V1** `[ul_gpu_pipeline]` 中位 | ≤2150 µs | ✅ **1364.2 µs**（`p39`；基线 2675.1 ⇒ **−49.0%**）；配对 A/B −21.5 µs（`p40` 1385.7）| §6.63/§6.64 |
| **V2** 池 | `starved_events=0` 且 `held_max<pool` | ✅ `p39`/`p40`/`p42` 三腿都 `starved_events=0`、`dropped=0`（`held_max` 9–18 < 32）| §6.63①/§6.64①/§7.6.0d |
| **V4** 提交数 | `cbs/lane ≤2.00`、`dropped=0` | ✅ 三腿都 **2.00 (max=2)**、crossings `0.00+0.00`/跳 | 同上 |
| **V5** 不回归 | 契约 8/8、0 crossings | ✅ 三腿都 **MET 8/8**、**`gaps=0`、`rx_overflows=0`**、`p0_gate` **29/29** | 同上 |
| **V3** 电台 | RF 失败 ≤10 | ⏸ **另案暂停（用户裁决）**：`p42` 1724、`p39` 1448、`p40` 1447 ⇒ 逐腿天气，比较要成对 | §6.40–§6.43、§6.56③ |

**当前状态**：HEAD = `7582d761c1`，`build/hashes.h` 与 `build/apps/gnb/gnb` 的内嵌戳**三者已对齐**，**工作区干净**，**没有腿在跑**，**没有 gNB 在跑**，**树 leg-ready**。
**交付配置**：接收环 **512 帧**、发送环 64、池 **32**、`otw_format: sc12`、`srate 23.04`（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。

---

## 1. 本会话做了什么（提交，逆序节选）

| 提交 | 内容 | 出处 |
|---|---|---|
| `7582d761c1` | **C3 判定不需要 + H1 推翻**：`process_symbol_boundary` 按**时间戳**锚定网格行（不是填充偏移）| §7.7 |
| `a20c2ecaf3` | ★ **C1 落地**：`block_batching_enabled()` 去掉 `OCUDU_UL_RX_SYMBOLS` 那条 `return`（它就是 `p41` `batched=0/0`/`released=0` 的成因）| §7.7 |
| `7ee001ea72` | **S-B 改动清单**落定（C1/C2/C3）+ 离线网的判据 | §7.7 |
| `0ba6cd30db` | Q21 第一轮：排除丢样/接收阻塞/估计器变慢，锁定"H1 网格相位"| §7.7 |
| `e1bdd89ab5` | **阶梯闭合**：LLR 那一级**不用补**（`record_ldpc_start` 就是 V1 终点；D10 `handler lag` 已在量）| §7.6.0d |
| `d016137102`/`e426e24749` | **`p42` 验证腿**：新尺子亮了（host gap **中位 45.8 µs**）、V1 1366.8 不变；**830 µs 分解第一版**成立 | §7.6.0c/0d |
| `90cc95e9ec` | ★ **修**：`merged` 路**从来没有**打 `mark_extraction_commit`（五段尺子恒 "no samples" 的根因）| §7.6.0b |
| `e5c05cfd64`/`21901bc784`/`8ec0354680` | 池的真相：**FFT 前的 IQ 自由表**（一缓冲=一槽）、持有期中位 819 µs、"链>时隙"为何不塌（到达率 13–27% + HARQ 截止 + 池解耦）| §7.6.0 |
| `27f7a007eb`/`bea54dbaf7` | **§7.6 结构性候选 S-A…S-E** + **单车道优先的数字依据**（V1 ≈ 2×窗口 + C；放大 1.2–2.6×）| §7.6/§7.6.1 |
| `a3a4a9ef2a`/`d3aeb9bafc` | **`p41`（A 项 / `P1-7`）**：吞吐 ↓17×、`batched=0/0`、`released=0` ⇒ **交付路径证伪** | §6.70/§6.71 |
| `b87815bd24` | **算力账单补全**：均衡 1.4–1.6 + 解映射 1.25–1.37（贴派发地板）⇒ 一跳全部算力 **≈51 µs** | §6.69 |
| `171d54d194`/`fec7359dd0`/`f2e5d87ce0` | CE 六个 kernel 的算力（**≈38 µs/跳**）、**空派发地板 1.3–1.4 µs**、**四种阶段边界 ≤0.3 µs**、宿主 encode 1.3–1.6 µs/派发 | §6.65–§6.67 |
| `70fefc4511`/`045378f2e4` | `p40` 反向臂：**配对 A/B −21.5 µs 落在预登记带内**，并**更正 §6.63 三处**（V1 判定、标尺、A 项的账）| §6.64 |
| `5a375cb1ba`/`7db01fa5c8` | **杠杆 C 落地**（K2 直读 LSE；新 kernel 独立文件 + `-fno-fast-math` 修掉重结合）| §6.62 |
| `968762061b`/`923bb82721` | **文档规矩**：memo 只在交接时写；把误建误改的 `-6` 内容并入开发文档并删除 | README/§5.4 |

**本会话的腿**（全部 n78 加压、并发 2、`OCUDU_UL_PHASE_SEGMENTS=1`、交付配置）：
`p39-n78-noscatter`（杠杆 C 验收）、`p40-n78-noscatter`（`OCUDU_CE_Y_DIRECT=0` 反向臂）、
`p41-n78-rxsym`（`OCUDU_UL_RX_SYMBOLS=1`，A 项）、`p42-n78-hostgap`（验证 merged 路的尺子修补）。
**离线载体**：`ul_chain_replay` 27 语料 + `wip/ce_kernel_cost.mm`（新工具）+ `ctest -L phy -j 1`（193/193）。

---

## 2. 本会话的增量结论（开新会话前必读的 12 条）

| # | 结论 | 出处 |
|---|---|---|
| 1 | ★ **标尺：每次派发 5–14 µs（配对口径 10–14）**。四次估计按设计强度排：11.4/14.1（`p39`vs`p40` 配对）、7.05（§3.2 那对）、5.1/5.3（异二进制）。**预登记只登记增量**（§6.63 判过一次假 MISS，就是锚了绝对区间）| §6.64③ |
| 2 | ★★ **一跳的全部 GPU 算力 ≈51 µs**（CE ≈38 + 均衡/解映射 2.8 + 前端执行 10.6/槽），占窗口 534 的 ~10% ⇒ **没有值得重写的 kernel**（最大单个 `mmse_weights` 13.2 µs，且不在任何消派发名单里）| §6.65/§6.69 |
| 3 | **派发线 ≤20 µs、阶段边界 ≈0 是真的"≈0"**（barrier / pipeline 切换 / encoder 边界各 ≤0.3 µs；宿主 encode 1.3–1.6 µs/派发）⇒ **"减少边界"这条臂不必飞** | §6.66/§6.67 |
| 4 | ★ **V1 的构成（`p42` 中位）**：1366.8 = **本跳窗口 541.8 + 同伴窗口 541.8 + 宿主 45.8 + 队列 39.5 + 出手(handler lag) 54.1 + 残差 ~144** ⇒ **V1 ≈ 2×窗口 + ~284 µs**：**窗口类改动的 V1 收益 ≈ 窗口降幅 × 1.2–2.6** | §7.6.0d |
| 5 | ★ **单车道是"串行化受限"不是"算力受限"**：一槽 500 µs 里真执行 ~78 µs（余量 ~6×），而**跳内是一条串行链且链长 534 µs > 一槽** ⇒ 用户裁决：**先把单车道链缩短，再动并发**（并发只是遮住等待，且要付 V4 + 偶发停顿）| §7.6.1 |
| 6 | **池是 FFT 前的 IQ 自由表**（一缓冲 = 一槽 11520 样点），**不是队列**：持有期中位 **819 µs ≈ 1.6 槽**、p99 1.58 ms、max 20.3 ms（吸收了那次 40 ms 宿主停顿）；**32 买的是"允许车道滞后 ~16 ms 才丢样"**；P2-D 定尺规则用 p99 只给 8–9，今天的 32 是按**峰值**定的 | §7.6.0 |
| 7 | **"链 > 时隙"为何不塌**：到达率只有容量的 **13–27%**（`period` 中位 435 µs / 均值 2011 µs）+ **每跳截止是 HARQ 时间线**（`k2=4`、探针把 8000 µs 定为 UL HARQ RTT）+ **池把收包与处理解耦**；塌的条件只有三条（利用率越 100%、在飞>池、跨度>8 ms）| §7.6.0 |
| 8 | ★★ **A 项 / `P1-7`（符号级收包）= 交付路径被证伪**：吞吐 **↓17×**（TBS 4–6 kbit → ~200 bit、MCS 落底；电台干净、契约 8/8、池绿）、**前端批量化被拆**（`batched=0/0`、`released=0`、提交 20.5/跳）、跨度净增 ~200 µs ⇒ **代码侧 V1 空间收口** | §6.71 |
| 9 | **`p41` 的"批量化被关"是显式开关**：`block_batching_enabled()` 里 `OCUDU_UL_RX_SYMBOLS > 0 ⇒ return false` —— 它把"收包粒度"和"成批"耦合在一起，**这就是 C1 要拆的那条** | §7.7 |
| 10 | ★ **网格行是按时间戳锚定的**（`process_symbol_boundary`：`i_slot`/`i_symbol` 由 timestamp 模子帧符号图案算出）⇒ **"整槽被按符号旋转"（H1）被推翻**、**C3 不需要做**；`p41` 的 MCS 塌陷留给 **H4：PHY 内部对齐丢弃（`process_alignment`/`max_phase_blocks`）留下的网格洞，`gaps` 看不见** | §7.7 |
| 11 | ★ **仪器课**：**融合路整跳一个命令缓冲 ⇒ 分阶段边界"构造上不存在"**（probe 的五段只能靠 `OCUDU_LANE_DIAG_SPLIT=1` 诊断臂，结构变了只作指示）；**`merged` 路从来没有打 `mark_extraction_commit`**（已修，`p42` 验证 host gap 中位 45.8 µs）；**RX 策略是空口专属**（replay 的 dump 在开关开/关下逐字节相同 ⇒ **离线验不了**）| §7.6.0b/§7.6.0d/§7.7 |
| 12 | **两条死路（别重走）**：① `stage_repeat` 旋钮 + `[metal_stats] gpu busy` 的斜率（并集读数、抖动几十 %，首轮给出 −130 µs/派发）；② 逐 commit 的 `start->end`（携带 fence 等待，`ce_weights` 读 745–750 µs 而整跳只有 534）。**预算一律用 §6.66③ 的 ≈5–20 µs，不要用 10–14 µs/派发去外推** | §6.65①/§6.67② |

**仍然成立的老结论（别重犯）**：`busy`/`busy split` 是**占用窗口**不是算力；**窗口 ≠ 关键路径代价**；池容量是 **2 的幂**；
`starved_events` 是"进入 nearly-dry 的**次数**"；**单腿不是证据，比较要成对**；**"读不出"按 RED**。

---

## 3. 下一步（**已排好顺序；前两步不用飞腿**）

> 用户裁决（2026-09-25）：**S-E（并发/队列结构）是下一步方向，但先把"单车道"自己优化好**（§7.6.1）。
> S-B = "**部分槽收包 + 保住批量化与交棒**"，**三处改动 C1/C2/C3 已在 §7.7 定死**。

### 3.1 ★ 第一件（机械活，锚点已定位）：补 **H4 计数**（`process_alignment`/`max_phase_blocks` 的触发数）

* `ul_rx_stats` 里在 `std::atomic<uint64_t> ts0_blocks{0};` 之后加 `phase_drops{0}` / `align_drops{0}`；
* `lower_phy_baseband_processor.cpp` 两处 `++nof_phase_blocks;` 之后各 `ul_rx_counters().<which>.fetch_add(1, ...)`（`establishes_phase` 那条 = `phase_drops`，`!symbol_aligned` 那条 = `align_drops`）；
* `ul_rx_stats_report()` 的 `[ul_rx]` 行**格式串与参数**一起扩成 `… rx_other=%llu phase_drops=%llu align_drops=%llu`。
* ⚠ **编辑锚点必须唯一**：上一次尝试就是栽在 `ts0_blocks` 的打印参数**出现多次**上（脚本自查后中止、未写入任何东西）⇒ 锚点带上相邻行（如格式串或 `c.gap_samples`）再改。
* **验证**：`cmake --build build --target gnb`（不飞腿）；读数在 `p43` 上取（见 3.3）。

### 3.2 第二件（S-B 唯一剩下的改动）：**C2 —— 批的边界**

现在**块只在"下一个时隙开始"时关**（`ofdm_demodulator_impl.h::set_lane_slot()`）⇒ 一槽一批。
**C2 = 把"关块/派发/交棒"的触发从"下一槽"改成"已到样点的最后一组"**（收包侧已有 `position.nof_symbols`）。
**硬约束（§7.7 三条）**：① `batched=` ≠ 0；② `released=` ≠ 0；③ **样点落进网格的符号相位不变**（已由时间戳锚定 ⇒ C1/C2 **不要碰 `rx_offset`/`rx_fill` 的装填语义**）。
**顺带收益**：S-C（等最后一个 DM-RS 符号就开跑估计器，~67 µs）在 C2 的形状里是免费的。

### 3.3 ★ 第三件（**需要你飞腿**，是 C1/C2 唯一的验收途径）：`p43` 半槽臂

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p43-n78-halfslot \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_UL_RX_SYMBOLS=7 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# CN 侧：iperf3 -R -b 40M -P 4 -t 240；【单次 Ctrl-C】停（⚠ 该策略的 shutdown 会延迟报告几分钟才落地，属已知 race）
# 判读：
bash doc_chinese/phy_latency/wip/p0_gate.sh p43-n78-halfslot
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 p43-n78-halfslot
S=$(ls -t doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_p43-n78-halfslot_*.log.stderr | head -1)
grep -aE "dft commits|ul_rx\] blocks|stage entry -> extraction|queue: weights|ul_gpu_pipeline" $S
```

| 读数 | 预登记 |
|---|---|
| `dft … batched=` | **≠ 0**（C1 生效的直接判据；`batch_max` 只是上限，**不是判据**）|
| `dft … released=` | **≠ 0**（D1 交棒仍在）|
| `[ul_rx] … phase_drops` / `align_drops` | **这就是 H4 的判据**：若两个数随腿增长（每槽/每几槽一次）⇒ 符号级路在丢符号 ⇒ **H4 成立**、也解释了 `p41`；若都接近 0 ⇒ H4 被否，MCS 塌陷另找 |
| TBS / 吞吐（iperf3 SUM、调度 `tbs=`）| **不许塌**（`p41` 的复现判据：TBS ~200 bit / 吞吐 ↓17×）|
| V1 中位 | **口径不同，只作参考**（端点会移一个接收窗）——**判据是 `merged_hop` 与整跳提交数**：`merged_hop` 应 **< 541.8**（早开跑），整跳提交数不许暴涨（§8 Q22）|
| 契约 / `cbs/lane` / `gaps` / 池 | 8/8 / 2.00 (max=2) / 0 / `starved=0`、`dropped=0`（**不允许变**）|

### 3.4 之后（S-B 成立才有意义）

* **重定尺池**：按新的 `hold_p99` 重算（P2-D 规则），并明确"**按峰值还是按 p99**"（这就是"worst-case 允许多滞后"的取舍，见 §7.6.0）；
* **回到 S-E**：链缩短后队列压力会自己下来；若链明显短于时隙间隔，甚至可把并发**降回 1**，同时去掉 P1-8 的偶发停顿代价。

### 3.5 另案与可选（都不属于当前主线）

* ⏸ **V3 + gap 残余**（暂停）：真问题是偶发 ~40 ms 宿主停顿（`stale` 一族），靶子 = 宿主竞争/线程优先级；重启入口 `load1` + 线程数/优先级臂。
* **§8 Q19**（那 10–14 µs/派发的归因，诊断、不承诺收益）、**Q20**（已收口）、**Q21/H4**（本 memo 3.3 的判据）、**Q22**（V4 看不见整跳提交数）。
* **两条陈旧网**（`value_net` 归档基线、`ab_dumps` arm1 ⇒ Q11 待裁决）；**P1-6**（`OCUDU_UL_SLOT_TRACE`）；**MAC 侧从丢失 CRC 指示恢复**（独立缺陷，未修）。
* **Ubuntu 侧**（`jwang@192.168.31.211:~/work/ocudu`）：本会话的改动**未同步**（那边没有 Metal，改动不影响其构建；同步时注意 `lib/phy/lower/modulation/ofdm_demodulator_impl.h` 与 `lib/phy/lower/lower_phy_baseband_processor.cpp`）。

---

## 4. 纪律（本会话又花过代价的几条）

1. ★ **`session_handoff_*.md` 只在交接时写一份，开发过程中不创建、不修改**（用户 2026-09-25 明确，§5.4）。
   开发过程中的结论/更正/纪律/仪表/未决**一律记入开发文档**；**快照有误 ⇒ 在开发文档里更正并引用它**，不回头改那份文件。
2. ★ **"离线可验"要在计划前先证实**：本会话两次被它绊到 —— **RX 策略是空口专属**（replay 的 dump 在开关开/关下逐字节相同）、
   **融合路整跳一个命令缓冲**（分阶段边界不存在，只有 `LANE_DIAG_SPLIT` 诊断臂才有，且结构变了）。**先跑一次最小实验，再写预登记**。
3. ★ **编辑脚本的锚点必须唯一，且"写文件"要放最后**：本会话一次 H4 编辑因锚点不唯一而中止 —— 因为写文件在最后一步，
   **整份改动没有落盘**（树干净、无半成品）。这个形状是对的，**继续这么做**。
4. **判 `ctest` 用串行（`-j 1`）**：`-j 4` 会让 Metal 测试互相争用而假红（两条路线都红 + 隔离 6/6 绿已证伪）。
5. **A/B 的 metallib 必须成对，且不许把树里那份当 B 臂**（`cp` 会跳过 ⇒ 两侧跑同一内核、dumps 全 0 却是空结论）。
6. ⚠ **`ul_chain_replay` 不含版本戳**（对它做 `grep -aq "$H"` 恒为假）；开工前对齐三条：`git log -1` = `build/hashes.h` = `gnb` 内嵌戳。
7. **预登记只登记增量**（别锚绝对区间）；**加压腿上 `leg_gate.sh` 的 `stale = 0` 与 `grant ≥50%` 是误绑项**，但**翻红要记、要归因**。
8. **`[metal_stats]` 的报告是 atexit 打的，会延迟**（`p41` 延迟 ~8 分钟）：腿停了先看 stderr 大小、必要时等进程退出再读，**别当成丢失**。
9. **一条臂只改一个变量**；**回放类工具串行用**；**用户飞腿时绝不碰 GPU/电台**；腿用**单次 Ctrl-C** 停并确认退出。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§3 判据、§4 仪表手册（**§4.5 文件地图**）、§5 规范（**§5.4 memo 规矩**）、**§6 追加式记录（本会话 = §6.62–§6.71）**、§7 杠杆（**§7.6 结构候选 / §7.7 Q21+S-B 清单**）、§8 未决（Q1/Q7/Q19/Q20/Q21/Q22）、§9 证据索引 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5 逐条、下一步 |
| `doc_chinese/phy_latency/README.md` | 三类文档分工 + **memo 规矩** + 旧文件名去向（含"`-6` 那次失误"的登记） |
| **`lib/phy/lower/modulation/ofdm_demodulator_impl.h`** | ★ **S-B 的主战场**：`block_batching_enabled()`（**C1 已改**）、`set_lane_slot()`（**C2 要改**）、`defers_transform_execution()` |
| `lib/phy/lower/lower_phy_baseband_processor.cpp` | 收包策略（1180–1290：`symbol_blocks` 分支、`rx_fill`/退役路径）、`[ul_rx]` 报告（**H4 要加计数**）|
| `lib/phy/lower/processors/uplink/uplink_processor_impl.cpp` | `locate_symbols()`（:362）与 **`process_symbol_boundary()`（:419，网格行的锚定在这里）**|
| `lib/phy/metal/ocudu_metal_lane_clock.h` + `ocudu_metal_lane_probe.mm` | 车道尺子（`handover_us` 等）与 `[ul_gpu_lane]` 报告 |
| `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm` | CE 引擎：`ce_sites`/`mmse_ce` 计数、`OCUDU_MMSE_DEBUG` 相位表、**merged 路的 `mark_extraction_commit()`（本会话新增）**、`OCUDU_CE_Y_DIRECT` 双读者分支 |
| `…/metal/ocudu_mmse_apply_lse.metal` + `ocudu_mmse_apply.metal` | K2 直读 LSE 的新 kernel（独立文件 + `-fno-fast-math`）/ 旧 K2（**一行未改**）|
| **`doc_chinese/phy_latency/wip/ce_kernel_cost.mm`** | ★ **本会话新增的离线微基准**：CE/均衡/解映射的算力 + 空派发地板 + 四种阶段边界 + 宿主 encode + 块尺寸扫描 |
| `doc_chinese/phy_latency/wip/p0_gate.sh` + `leg_census.py` | P0 门（D1–D19，含 D16 批量化、D18 直读网格、D19 接收余量）/ 腿普查 |
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` + `ab_dumps.sh` + `ab_replay_bins.sh` | 起腿 / 一个二进制两环境 / 两个二进制（**metallib 必须成对**）|
| `doc_chinese/work_tmp/ref/` | 本会话归档：`replay_pre_c_bc149ab266`、`mmse_pre_c_bc149ab266.metallib`、`mmse_post_c_bc149ab266.metallib`、`value_net_pre_c.txt` |
| `doc_chinese/phy_pipeline_gpu/wip/logs/` | 腿日志：`p37`–`p42`（本会话：`p39`/`p40`/`p41`/`p42`）|

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-26-1.md`。
> V1/V2 早已结项；本会话**验收完杠杆 C**（`p39` 机制全中、`p40` 配对 A/B **−21.5 µs** ⇒ **V1 1364.2 µs，基线 −49.0%**），
> 并把**三条"省时延"矿脉量尽**（消派发 ≤20 µs、边界 ≈0、算力 ≈51 µs 且没有值得重写的 kernel）；
> **A 项 / `P1-7` 已证伪**（吞吐 ↓17×、前端批量化被拆）。**现在的主线 = 按用户裁决"先把单车道链缩短，再动并发"**（§7.6.1），
> **S-B 的三处改动 C1/C2/C3 已定死**（§7.7）：**C1 已落地**、**C3 判定不需要**（网格行由**时间戳**锚定）、**C2 待做**。
> **下一步（我建议的顺序）**：① 补 **H4 计数**（`[ul_rx] phase_drops/align_drops`，锚点要唯一、写文件放最后）；② 做 **C2**（批边界＝"已到样点组"）；
> ③ 给我 **`p43-n78-halfslot`**（`OCUDU_UL_RX_SYMBOLS=7`，配方同 `p42`）—— 它是 C1/C2 唯一的验收途径（**离线已证实验不了**）；
> ④ S-B 成立后再重定尺池、再谈 S-E（并发，甚至可降回 1 以去掉偶发停顿代价）。
> 开工先对齐三条：`git log --oneline -1` = `build/hashes.h` = `gnb` 内嵌戳（**`ul_chain_replay` 不含戳**，别误判）；
> 判 `ctest` 用 **串行**；腿停了若 `[metal_stats]` 没出现，**先等进程退出**（atexit 报告会延迟）。
> 提醒纪律：**memo 只在交接时写**、**"离线可验"要先证实**、**编辑锚点要唯一且写文件放最后**、
> **预登记只登记增量**、**加压腿上 `stale=0`/`grant≥50%` 是误绑项（翻红要记要归因）**。
> **V3 与 gap 残余已另案暂停**（靶子 = 宿主竞争/线程优先级），不要顺手去动。
