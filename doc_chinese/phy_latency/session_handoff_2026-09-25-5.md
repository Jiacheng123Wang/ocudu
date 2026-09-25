# Session handoff — 2026-09-25 #5

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 的第五份**（继 `…-1.md` … `…-4.md`）。
>
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"）：
> **上一份 memo（`-4`）之后新增的是 §6.48–§6.61**（本会话的产出）；`high_level_status_and_plan.md` 是高层现状；
> 三类文档分工见 `README.md`。**本 memo 只写"开工要什么"**：现状、读数出处、下一步（含已定好的施工方案）、
> 仍挂着的事、纪律、文件地图、第一句话。

---

## 0. 一句话现状

**目标（V1 时延 + V2 池）已结项**（§6.43），此后按用户裁决**继续压 V1**，本轮把一跳的**真实派发**从 12 次压到
**4 次（车道口径）**，**V1 中位 1488.6 → 1371.9 µs**（基线 2675.1 ⇒ **−48.7%**），**V2/V4/V5 全程保持**；
**gap 的成因已钉死并已用"接收环 + 池"治住**（丢样换成迟到）。
**当前正在做**：开发文档 **§6.61** 的 ① 之后 —— **消掉信道估计的 `scatter` 派发（改寻址）**，
方案、判据、预登记都已写好（见 §3.1），**只差施工**。

| 判据 | 阈值 | 现状 | 出处 |
|---|---|---|---|
| **V1** `[ul_gpu_pipeline]` 中位 | ≤2150 µs | ✅ **1371.9 µs**（`p38`；基线 2675.1 ⇒ **−48.7%**）| §6.46–§6.60 |
| **V2** 池 | `starved_events=0` 且 `held_max<pool` | ✅ `p38`：`starved_events=0`、`held_max=10 < 32`、`free_min=22`、`dropped=0` | §6.37–§6.39、§6.60 |
| **V4** 提交数 | `cbs/lane ≤2.00`、`dropped=0` | ✅ `p38` **2.00 (max=2)**、crossings `0.00+0.00`/跳 | §6.30/§6.46/§6.60 |
| **V5** 不回归 | 契约 8/8、0 crossings | ✅ `p37`/`p38` 契约 **MET 8/8**、**`gaps=0`、`rx_overflows=0`** | §6.53/§6.57/§6.60 |
| **V3** 电台 | RF 失败 ≤10 | ⏸ **另案暂停（用户裁决）**：`p38` 1546（`p37` 880、`p35` 710）⇒ **逐腿天气不同，比较要成对** | §6.40–§6.43、§6.56③ |

**当前状态**：`HEAD = 戳 = 6dc68355d3`，**二进制已携带该戳**，工作区干净，**没有腿在跑**，**没有 gNB 在跑**。
**交付配置**：接收环 **512 帧**、发送环 64、池 **32**、`otw_format: sc12`、`srate 23.04`（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。

---

## 1. 本会话（2026-09-25 #5）做了什么

> 从 `-4.md` 开工：先按用户裁决**继续压 V1**（去掉 `y_gather`、再做整跳一个 run），
> 中途把 **gap** 的成因与仪器补齐、按裁决做了 **A（环+池）/B（环 512）/C（TX 环，已退役）**，
> 最后做完 **①（信道估计/解映射派发路径）的读码**并把下一步定成"消掉 `scatter`"。

| 提交（节选，逆序）| 内容 | 出处 |
|---|---|---|
| `6dc68355d3` | 文档收口：§6.61 的**机制更正**（`merge_tail` 只并槽位、两组同一提交）与**杠杆重排**（首选 C）| §6.61 |
| `7c9d51c9f3` | ① 机制查实：CE **6–9 次真实派发/跳**（车道只数 2）；`merge_tail` 只并布局；融合形式因**精度**不用 | §6.61 |
| `44a7643529` | ① 开工：`[metal_stats] ce_sites` 逐站点计数（离线 replay 即可读，**不需要腿**）| §6.61① |
| `5d7a01366e` | **腿 `p38-n78-wholehop`：§3.2 预登记全中** —— run 3→1/跳、派发 6→4/跳、`merged_hop` −14.1 µs、**V1 1371.9（新最好）** | §6.60 |
| `36b49ea2a5` | **§3.2 施工**：`.metal` 加 `y_starts[]`（逐符号起点）+ 宿主表 + 三处谓词放开；语料 run 4→1、派发 12→4、**3×135 dump 逐字节相同** | §6.59 |
| `ac80ee7847` | §3.2 读码结论：**只解 `estimates` 断因不值派发**（跨空档的 run 表达不出来）⇒ 方案改为"把 `h_starts` 搬到 y 上" | §6.58 |
| `46f53334db`/`97d2e60efa` | **腿 `p37-n78-ring512`（B 验收）**：`gaps=0`、契约 8/8、V1 1408.2；**池峰值测得 13 ⇒ P2-D 重算仍得 32**；⚠ 深环把丢样换成迟到（`stale` 2→13）| §6.57 |
| `ee432c9b33` | **更正"UL 静默"**（不是现象，是工具误报）+ **B 落地（环 512）** + **C 退役**（TX 环不是 underflow 杠杆）| §6.56 |
| `f5389b7ca5`/`30cb928274` | **腿 `p35`（A 验收：V2+V5 首次同时绿）+ `p36` 失败复盘**（**臂本身致命**，不是手机）+ **离线电台台架** | §6.55 |
| `ab6d2703b5`/`9e1d511548` | 裁决 **A**（池按 P2-D 用新测量重算 16→32、环回 256、启动行宣布真实尺寸）+ 裁决 **C**（`load1` 上下文、`mk_arm_cfg.sh`、传输臂）| §6.54 |
| `e514761103`/`01fe73ad9c`/`87ad66639f` | **腿 `p32b`（ABA 对照）** + `p34` 的 64 vs 256 对照 ⇒ **环的作用与代价都量出**（丢样 ↔ 迟到）| §6.49–§6.53 |
| `3b2b1b151d`/`2db3259a9d` | **①后半落地**：均衡直接在网格里读（去 `y_gather`）——离线 135 dump 逐字节相同 | §6.48 |
| `9633a4e4f3` | **gap 成因收口**（每次都是接收环溢出）+ RX 仪器（`rx_overflows`/`gap_us`/`[ul_rx_timing]`/门 D19）+ 环 64→256 | §6.51 |

**本会话的腿**（n78 加压、并发 2、`OCUDU_UL_PHASE_SEGMENTS=1`）：
`p30`（站点表）、**`p31`**（建表一跳一次 ⇒ V1 1463.5）、**`p32`**（直读网格 ⇒ 1411.3）、
**`p32b`**（ABA 对照）、**`p33`**（256 帧环 + RX 仪器 ⇒ 0 gaps）、**`p34`**（64 帧对照 ⇒ 5 段丢样）、
**`p35`**（池 32 ⇒ **V2+V5 同时绿**）、**`p36`**（`recv_frame_size` 臂 ⇒ **失败：臂致命**）、
**`p37`**（**B 验收**：512 帧环 ⇒ 0 gaps、池峰值 13）、**`p38`**（**§3.2 验收**：整跳一个 run ⇒ **V1 1371.9**）。

---

## 2. 本会话的增量结论（开新会话前必读的 12 条）

| # | 结论 | 出处 |
|---|---|---|
| 1 | **`y_gather` 可以整条去掉**：一 run 的符号就是网格那一段连续子载波时，均衡直接在网格里读（逐字节相同是构造性的）| §6.48 |
| 2 | **每去掉 1 次派发 ⇒ V1/跨度 ≈13–18 µs，但设备 busy 窗口只有 6.5–9.4 µs** ⇒ **标尺有口径**，不要用 `merged_hop` 降幅反推派发数 | §6.49③/§6.50③/§6.60① |
| 3 | **gap 的成因 = 电台接收环溢出**（11 条腿 `overflow` 次数 == `gaps` 次数，逐事件；两者相隔 ~1 ms）| §6.51② |
| 4 | **环深是"丢样↔迟到"的交换**：64 帧丢 5 段（最大 35.3 ms）、256 帧 0 丢；512 帧把余量抬到 **60.7 ms** | §6.53/§6.57 |
| 5 | **池按 P2-D 规则用新测量重算仍是 32**（256 帧峰值 16 被夹住、512 帧峰值 13，`13+5=18 ⇒ 32`、`16+5=21 ⇒ 32`）| §6.57② |
| 6 | **"秒级 UL 静默"不是现象**：工具只在整段日志上量 PUSCH 间隔，把 `iperf3` 之外的空闲头/尾算成静默；**窗口内每次停顿都有 PUCCH ~100/s**（UE 一直在发，停的是**数据面**）| §6.56① |
| 7 | **`recv_frame_size` 有硬边界（~8 KB）**：8192/8200 健康、**12288/16360 必崩**（每块 overflow、交付率 9.8%）⇒ `p36` 的失败**在臂不在手机** | §6.55③ |
| 8 | **停顿在宿主调度侧，不在 USB 带宽**：交付配置收发同时 100% 交付、0 错误；**CPU 打满 ⇒ `recv` 拖到 28 ms 而电台 0 错误** | §6.55③ |
| 9 | **TX 环不是 underflow 的杠杆**（台架 64/512 都 0 underflow；腿上 90%+ 的 underflow 发生在"递交提前、发送环满"时）⇒ **C 退役** | §6.56③ |
| 10 | **整跳一个 run**：把 `h_starts` 那套搬到 y（`y_starts[]` 逐符号起点）⇒ run 3→1/跳、派发 6→4/跳、**V1 −36.3 µs** | §6.58–§6.60 |
| 11 | ★ **车道里的 "CE 2 次/跳" 是阶段数**：估计器真实是 **6 次（无尾）/ 9 次（带尾）kernel 派发/跳** ⇒ 真实派发账 = **8 或 11 次/跳**，**不是 4** | §6.61①② |
| 12 | ★ **尾巴组不是"merge_tail 失效"**：`merge_tail` 只并**槽位布局**（SPLIT_TAIL 实验证明 `ce_sites` 不变）；两次来自 `build_slots_on_device()` 被调两次，**但两组同一提交**（多付的只是派发）；估计器不用融合形式是**精度**原因（块序 54、K1 逆 1.3e-5 ⇒ 256QAM 24 dB→−18 dB）| §6.61② |

**仍然成立的老结论（别重犯）**：`busy`/`busy split` 是**占用窗口**不是算力（§6.29④）；**窗口 ≠ 关键路径代价**（§6.31③）；
池容量是 **2 的幂**（§6.37②）；`starved_events` 是"进入 nearly-dry 的**次数**"（§6.36③）；夹具里的读数不是空口读数。

---

## 3. 下一步（**已规划好，按顺序做**）

> 用户裁决："从 ① 开始，一项一项地做"。① 的**读码已完成**，下面是排好的施工顺序。

### 3.1 ★ 下一件（施工起点）：**C —— 消掉信道估计的 `scatter` 派发（改寻址）**

**为什么先做它**：① 里四个候选里**只有它覆盖全部跳**（A 只覆盖带尾的 ~40%）、是**纯搬运**
（`scatter` 把 K0-a 的输出搬成 weights/apply 要的布局）、且**方法论已在 §3.2 验证过一次**（改寻址而非搬运 ⇒ 逐字节可证）。

**施工前要读的三处**（这是新会话的第一个动作）：
1. `encode_scatter`（`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm`，
   站点计数就在它旁边）：它写什么、索引怎么算（`gpu_ls_out` → `gpu_y`，参数 `Ls`/`n_blk_slots`/`n_blk_real`/`npf`/`pilot_base`/`inv_beta`）；
2. **weights/apply kernel**（同文件的 `pilots_apply_pipe` 一族）：它读 `gpu_y` 的哪些索引；
3. `pilots_scatter` 的构建处（`port_channel_estimator_metal_mmse_impl.cpp`，`device_y_stage[...]`，含 `sys_offset`）。

**做法（照 §3.2 的形状）**：把"下游要的布局"变成**下游自己的寻址**（下游直接读 `gpu_ls_out`，按 scatter 当初的映射算索引）；
必要时给 kernel 加**逐 system 的小表**（就像 `y_starts`/`h_starts` 那样，代价是几次加法，不是一遍搬运）。
**条件不满足就退回**（照旧发 `scatter`），并加 `miss(...)` 式的原因计数便于反例判读。

**预登记（腿 `p39-n78-noscatter`，暂定）**：
| 读数 | 今天（`p38`）| 预登记 |
|---|---|---|
| `ce_sites scatter=` | 1.0/跳（无尾）、2.0/跳（有尾）| **0**（按跳均值 ≈1.4 → 0）|
| `burst dispatches` | 4.00/跳（车道口径）| **−1…−2/跳** |
| `merged_hop` | 541.1 µs | −7…−19 µs |
| **V1 中位** | 1371.9 µs | **−15…−35 µs ⇒ ≈1337–1357** |
| 契约 / V2 / V4 / D18 | 8/8 / 绿 / 2.00 / 不变 | **同** |
| **不变量** | — | **dump 逐字节相同**：27 条语料 ×2 臂 + **与改前二进制对拍**（`ab_replay_bins.sh` 的形状）；`ctest -L phy` 193/193；`value_net` 与改前逐行相同 |
| 反例判读 | — | `scatter` 计数不变 ⇒ 下游仍走旧寻址；dumps 有差异 ⇒ 寻址映射错（先查 `pilot_base`/`Gb`/`npf`）|

### 3.2 之后（同一条线上的顺序，来自 §6.61③ 的重排）

| # | 内容 | 省 | 覆盖 | 已知约束 |
|---|---|---|---|---|
| **A** | 把"标准组 + 尾巴组"并成**一次派发**（corr 与 scatter 各 2→1）| −3 | 带尾跳（≈40%）| ⚠ **不是纯宿主**：`l`/`npf`/`ncomb`/`pilot_base`/`sigma2` 是**一次调用一份** ⇒ 需要**逐 system 几何**；备选的"尾巴 padded 进标准槽位"形式**代码里已有已知 pad 缺陷**（注释：*"the fused route's pads come out NaN"*）|
| **D** | `pilots_lse` + `pilots_cfo` 融合（CFO 是对 LSE 输出的逐点相位乘）| −1 | 全部跳 | kernel 重写；逐字节或有界容差 + `value_net` |
| — | `reformat`(K3) 也按 C 的办法消掉 | −1 | 全部跳（split 路由下本来就没有）| 与 C 同类，可作 C 之后第二步 |
| ~~B~~ | ~~`R_hp` 按几何缓存~~ | — | — | ❌ **读码后否掉**：`corr_stage` 带 `sigma2`/`fd_hz`/`tau_rms_s`（**每跳**信道统计）|

### 3.3 第二项（V1 的另一半）：`merged_hop` 里**非派发**的 ~490 µs

一跳窗口 541 µs、4 次派发（真实 8–11 次 kernel）≈ 每次 12–18 µs ⇒ 其余 **~350–450 µs 是真实算力 + 派发间依赖等待**。
**只有两条路**：
* **"移除阶段"臂**（现成旋钮：`OCUDU_CE_NO_K4`、`OCUDU_CE_CORR_{FENCE,SEGMENT,UNIFORM,BARRIER_AFTER}`、
  `OCUDU_CE_{WEIGHTS_BARRIER,INV_BARRIERS}`、`OCUDU_EQ_{DEFER_ENCODE,DEV_TABLES}`、`OCUDU_INV_{TGX,TGY,RL,MEMNONE}`）；
* **单 kernel 离线微基准**（照 `wip/dft_kernel_cost.mm` 的形状）——**推荐先做这个**：它能一次量出 CE/均衡/解映射各自的
  算力与窗口，**不用飞腿**，做完才知道该砍哪个 kernel。

### 3.4 第三项（结构项，**需要用户裁决**）：A 项 —— 等本槽最后一个样点 ≈473 µs（零算力）

载体是 `P1-7` 符号级收包（`OCUDU_UL_RX_SYMBOLS=N`），但**触 V4**（提交数），且其代码注释写明**时延收益目前无法判读**
（`[ul_pipeline]` 的窗口会随策略移动，shutdown 还会触发 DU teardown race，与 `OCUDU_UL_RX_POOL_SIZE` 同类）。

### 3.5 另案与可选（都不属于当前主线，需要时再取）

* ⏸ **V3 + gap 残余（暂停）**：现在的真问题是**偶发的 ~40 ms 宿主停顿**（表现为 `stale` 13 次/0.01% 的跳），
  **靶子 = 宿主竞争与线程优先级**（不是电台、不是环深、不是 USB 带宽——三条都已排除）。
  重启入口：`load1`（overflow 事件自带）+ 线程数/优先级臂。
* **extended CP 取证腿**（12 符号路径已实现、metal arm 17 离线自证，**没有空口腿**）。
* **D4/gap 按对累计**；**P1-6**（`OCUDU_UL_SLOT_TRACE` 拆 B/E 项）；**两条陈旧网**（`value_net.py` 的归档基线、`ab_dumps` arm1）；
  **Q12**；**P2-F**（并发度作为交付，需二次裁决）；**P2-A**；**MAC 侧从丢失 CRC 指示恢复**（HARQ 饥饿，独立缺陷，未修）。
* **Ubuntu 侧**（`jwang@192.168.31.211:~/work/ocudu`）：本会话修的两处 CMakeLists 已在那边就地同步；
  那边构建命令 `cmake -S . -B ./build -DCMAKE_BUILD_TYPE=Release -DENABLE_FLOW_PROBES=ON && cmake --build build -j16`。

---

## 4. 纪律（本会话又花过代价的几条，全部更新）

1. **一条臂只改一个变量**。我在 `p33` 把"接收环 64→256"和"RX 仪器"放进**同一个提交**，
   结果那条腿**无法归因环的效果**（后来靠 `p34` 单独对照才补上）。⇒ 改动与仪器**分开提交**。
2. **`cmake --build build --target X --clean-first` 会清整棵树**（不是单个 target）：它删掉了
   （含 `ldpc_metal_unit_test`/`demodulation_mapper_metal_unit_test` 这类**不在默认目标里**的）测试二进制，
   随后的 `ctest` 报了 67 个 "Not Run"，**看起来像大面积回归**。改 `.metal` 只需 `--target ocudu_metallib_equalizer`。
3. **改完代码必须重建两样**：`cmake --build build --target ocudu_versioning && cmake --build build --target gnb`
   （分开两条命令）。`run_leg.sh` 用**内容判据**（二进制里必须能找到它声称的戳）拦旧二进制。
4. **测试可执行文件不在默认构建目标里**：改引擎/估计器之后要**显式构建**那一组目标再跑 `ctest`——
   本会话新增的教训是：**改 `.metal` 还要重建 metallib**，而且**非默认目标的二进制不会被"全量 build"补回来**（见第 2 条）。
5. **先读机制计数器，再看时延**：本会话两次靠它抓到"跑了但没省到"的缺陷（§6.59② 的两个索引错误），
   以及一次靠它抓到"臂本身致命"（`p36` 的 `overflow` 风暴）。
6. **单腿不是证据；比较要成对**：`p38` RF 失败 1546 vs `p37` 880 ⇒ 逐腿天气不同，效应要靠**机制计数 + 配对读数**。
7. **"读不出"按 RED**；**不许为过关改阈值**；**保留第一次读数**（flaky 测试重跑绿也要记）。
8. **用户飞腿时绝不碰 GPU/电台**；腿用**单次 Ctrl-C**停并确认退出；判 V1–V5 的腿**不带 `OCUDU_METAL_GPU_TIME=1`**。
9. **用户自己的工具也要能证伪自己**：`leg_census.py` 的"静默"曾把空闲头/尾报成事件（§6.56①，已修：
   现在**自行推导流量窗口**并打印每次停顿内的 PUCCH 行数 + 判词）；`p0_gate_selftest.sh` 对 D18/D19 都是双向断言。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§3 判据、§4 仪表手册、§5 跑腿规范、**§6 追加式记录（本会话 = §6.48–§6.61）**、§7 杠杆、§8 未决 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5 逐条、下一步 |
| `doc_chinese/phy_latency/README.md` | 三类文档分工 + 结项状态 + 工具清单 |
| `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm` | 均衡引擎：`eq_flush_hook`（run 划分 + **`y_starts` 填充**）、`eq_direct_grid_run`、`eq_symbol_direct_readable`、`sites(...)`/`eq_direct` 计数 |
| `lib/phy/upper/channel_processors/metal/ocudu_equalizer.metal` | 均衡 kernel：**`equalize_strides::y_starts[]`**（§6.59 新增）、`h_starts[]` |
| **`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm`** | **下一刀的主战场**：`encode_scatter`（要消掉的）、`encode_reformat`、`build_pilots_lse`、`encode_corr`、**`ce_sites` 逐站点计数**（§6.61 新增）、`OCUDU_CE_*` 旋钮 |
| `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp` | 估计器宿主侧：`build_slots_on_device()`（标准/尾巴两次调用）、`queue_correlation_fenced`/`flush_correlations_fenced`（**一个提交**）、`device_y_stage[...]`（scatter 的构建）|
| `include/ocudu/phy/upper/equalization/channel_equalizer_device_grid.h` + `lib/phy/upper/equalization/channel_equalizer_device_grid.cpp` | gather plan（`subc_base`/`dense`）|
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` + `lib/phy/lower/lower_phy_factory.cpp` | 接收池（**P2-D：峰值 16+2+3 ⇒ 32**，启动行打印依据）、RX 探针（`rx_overflows`/`gap_us`/`[ul_rx_timing]`/`load1`）、TX 探针 |
| `include/ocudu/gateways/baseband/baseband_gateway_receiver.h` + `lib/radio/uhd/radio_uhd_rx_stream.cpp` | 电台对每块的判词（`rx_error`）从 UHD 带到 PHY |
| `configs/gnb_rf_b200_tdd_n78_20mhz.yml` | 腿配置：**接收环 512 帧**、发送环 64、`otw_format: sc12`（旁边有 64 vs 256 的实测对照）|
| `doc_chinese/phy_latency/wip/p0_gate.sh` + `p0_gate_selftest.sh` | **P0 门（D1–D19）**；D18 = 直读网格分流，D19 = 接收侧余量与归属（自测双向）|
| `doc_chinese/phy_latency/wip/leg_census.py` | 腿普查（**已修**：自行推导流量窗口 + 每次停顿打印 PUCCH 行数与判词）|
| `doc_chinese/phy_latency/wip/uhd_rx_health.cpp` | **离线电台台架**（直连 UHD，无手机无 gNB）：帧长/环深/宿主负载对 RX 的影响，TX 异步错误计数 |
| `doc_chinese/phy_latency/wip/mk_arm_cfg.sh` | 生成"**只改一行**"的臂配置（`sc8` 可生成；`bigframe` 已被**拒绝**——它会崩）|
| `doc_chinese/phy_latency/wip/eq_dense_probe.cpp` | plan 的 `dense` 判据探针 |
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` | 起腿（戳 + 内容判据双重守卫）；腿日志在 `…/wip/logs/` |

**腿配方（n78 加压，供参考）**：
```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
```
CN 侧 `iperf3 -R -b 40M -P 4 -t 240`，**单次 Ctrl-C** 停；判读用
`bash doc_chinese/phy_latency/wip/p0_gate.sh <leg>`（D1–D19）+ `bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 <leg>`
（V1–V5；它的 `stale=0`/`grant ≥50%` 两项在加压腿上是**误绑**，只作参考）。

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-25-5.md`。
> 目标（V1/V2）已结项（§6.43），之后在**继续压 V1**：**`p38` 已到 1371.9 µs（基线 −48.7%）**，
> 一跳的**车道口径派发**已从 12 压到 4；**标尺 = V1/跨度 13–18 µs 每派发**（不要用 `merged_hop` 反推）。
> **下一步 = 施工 §6.61 的首选杠杆 C**：把信道估计的 **`scatter` 派发用"改寻址"消掉**
> （`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm` 的 `encode_scatter`，
> 让 weights/apply 那一侧直接按它的映射读 `gpu_ls_out`；必要时给 kernel 加逐 system 小表，就像 §3.2 给 y 加的 `y_starts`）。
> **先跑不变量网**（27 条语料 ×2 臂 + **与改前二进制对拍**、`ctest -L phy`、`value_net`），
> **再重建 `ocudu_versioning` 与 `gnb` 两样**，然后给我 `p39-n78-noscatter` 的腿命令。
> 预登记：`ce_sites scatter` 1.0/2.0 → **0**、派发 −1…−2/跳、**V1 −15…−35 µs ⇒ ≈1337–1357**、契约 8/8、`cbs/lane=2.00`、0 gaps。
> 提醒三条纪律：**一条臂只改一个变量**、**改 `.metal` 不要用 `--clean-first`**、
> **非默认目标的测试二进制不会被全量 build 补回来**。
> **V3 与 gap 残余已另案暂停**（靶子 = 宿主竞争/线程优先级），不要顺手去动。
