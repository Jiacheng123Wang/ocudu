# Session handoff — 2026-09-25 #4

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 的第四份**（继 `…-1.md`、`…-2.md`、`…-3.md`）。
>
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"）；
> **本会话新增的细节在 §6.40–§6.47**；高层现状与计划在 `high_level_status_and_plan.md`；三类文档分工见 `README.md`。
> **本 memo 只写"开工要什么"**：现状、读数出处、下一步、仍挂着的事、纪律、文件地图、第一句话。

---

## 0. 一句话现状

**目标（V1 时延 + V2 池症状）已达成并结项**（开发文档 **§6.43**）；本会话之后又**继续压 V1**，已经拿到 **1463.5 µs**（基线 2675.1，−45%），
并且把"下一步怎么压"变成了一条**可预算的路**：**一跳自己的窗口 592 µs 里有 10 次派发，每去掉 1 次 ≈ −12 µs**（空口校准）。

| 判据 | 阈值 | 现状 | 出处 |
|---|---|---|---|
| **V1** `[ul_gpu_pipeline]` 中位 | ≤2150 µs | ✅ **1463.5 µs**（`p31`；基线 2675.1 ⇒ **−45%**）| §6.31/§6.32/§6.39/§6.46 |
| **V2** 池 | `starved_events=0` 且 `held_max<pool` | ✅ `starved_events=0`、`held_max=10–12 < pool=16`、`pop_blocking` max **23–25 µs**（原 486 µs–5 s）| §6.37–§6.39 |
| **V4** 提交数 | `cbs/lane ≤2.00`、`dropped=0` | ✅ 全程 **2.00 (max=2)**（合并派发**不动**它）| §6.30/§6.46 |
| **V5** 不回归 | 契约 8/8、0 crossings | ✅ 确认腿 8/8；⚠ **偶发 1–3 gap**（电台侧，见 §3.4）| §6.32/§6.42 |
| **V3** 电台 | RF 失败 ≤10 | ⏸ **另案暂停（用户裁决）**：700–1500；探针证明**宿主侧只解释 ~4%** | §6.40–§6.42、§6.43 ⑤ |

**当前状态**：`HEAD = 戳 = 3c6ada96b6`，**二进制已携带该戳**（`cmake --build build --target gnb` 为 no-op），工作区干净，**没有腿在跑**。

---

## 1. 本会话（2026-09-25 #4）做了什么

> 本会话是从 `…-3.md` 开工的：它先把**目标收口**（V1/V2 达成、V3 另案），然后按用户裁决**继续压 V1**，中途还修了 Ubuntu 构建。

| 提交（节选，逆序）| 内容 | 出处 |
|---|---|---|
| `3c6ada96b6` | **§6.47**：去掉 `y_gather` 的**施工方案**（变体 A 只动引擎：把网格直接绑成 y；变体 B 动 kernel + 预登记） | §6.47 |
| `cb6b182885` | **腿 `p31-n78-eqtable` 逐项命中**：设备侧 gather 表 3.0→**1.0/跳**、派发 12→**10/跳**、`merged_hop` **−24.4 µs**、**V1 1488.6→1463.5**；**校准出 ≈12 µs/派发** | §6.46 |
| `b61ba9b505` | **修**：设备侧 gather 表**每 run 重建**（与"一跳一次"的注释矛盾）⇒ 加同一跳内缓存 | §6.45 |
| `d9c051dc4c` | **流程事故 + 守卫**：`p29` 是**旧二进制**飞的（我只刷新了戳、没重建 `gnb`）⇒ `run_leg.sh` 改为**在二进制里找戳字符串** | §6.44 ⑥ |
| `e3d4658f58` | **仪器**：均衡器按**派发点**拆分 `sites(ch_gather, y_gather, y_batch, run, single)` | §6.44 ③ |
| `261d8d60d1` | **结项**：目标达成（V1/V2）、V3 与残留 gap 另案暂停 | §6.43 |
| `f1c7dc1d49`+ | **Ubuntu 构建修复**：`ocudu_lower_phy` 与 `gnb_base` 显式声明 `ocudu_phy_support`（ld64 不管归档顺序、GNU ld 单趟解析） | §6.43 ⑦ 7 |
| （更早）`12ca4f86f0`、`0b0c371093`、`de21dcc741`、`dae5811b75`、`501b792d01`、`92fa8a90de`、`a90246143a`、`77e2deda2c` | V2 一整条链：归因（§6.34）→ 清扫第三入口点（§6.35）→ P1-4 证明容量（§6.37）→ P2-D 池定尺 16（§6.38）→ `p27` 确认 V2 达成（§6.39） | §6.34–§6.39 |

**本会话的腿**：`p25`（(A) 验证）、`p26`（P1-4 池 16 ⇒ `starved_events=0`）、`p27`（交付定尺 ⇒ **V2 达成**）、`p28`（TX 探针首读）、
`p29`（**旧二进制**，仅作基线）、`p30`（站点表）、`p31`（**一跳一次建表 ⇒ V1 1463.5**）。

---

## 2. 已证明 / 已证伪（本会话增量）

| # | 结论 | 证据 | 出处 |
|---|---|---|---|
| 28 | **V2 的根因是容量，不是尾巴**：池 8 ⇒ `starved_events` 38–107；池 16 ⇒ **0**、`held_max=11`（**峰值自己升到 11** ⇒ 池 8 把流水线**夹住**了）| P1-4（`p26`）、`p27` | §6.37/§6.39 |
| 29 | **清扫的第三入口点有效**：普通取缓冲也驱动清扫（1 ms 节流）⇒ 未被认领块的最久年龄 96→**10 ms**（= 清扫自己的截止）| `p25`：`take sweeps=297854 recovering 52`、`dry-pool reaps=0` | §6.35/§6.36 |
| 30 | ⚠ **更正 §6.34**：饥饿**不是**未被认领块造成的——谁持有那 6 个在飞块**不影响持有几个缓冲**；饥饿 = **结构性峰值 = 池尺寸** | 9 条腿每槽饥饿率 0.62–1.72e-4（`p25` 1.37 在带内）；(A) 之后饥饿未降 | §6.36 ③ |
| 31 | **V3 的失败是 UHD 的 TX 侧实时失败且随负载出现**（前 150 s 仅 ~7 次、后 113 s 505–632 次），与 UL 收包路径无关 | `p28`：`gaps=0`… （`p27`）`pop_blocking` max 23 µs、`starved=0` | §6.40 |
| 32 | **TX 探针首读：宿主只解释 ~4%**：递交中位**提前 1512 µs**、只有 **52/685435 次越过 0**（最差 −4.2 ms），而同腿 **1064 次 RF 失败** | `p28` + D17 | §6.41/§6.42 |
| 33 | **一跳自己的窗口里只有 10–12 次派发**（均衡 7–9 = runs×[建表+gather y+均衡]、信道估计 2、解映射 1），而窗口 592–616 µs、算力估计只有 ~150 µs | `[metal_stats] burst dispatches=` / `eq_batch` | §6.44 |
| 34 | **能去掉一次派发 ≈ 一跳 −12 µs**（2 次 ⇒ `merged_hop` −24.4、V1 −25.1），与离线 12.18 µs/次 1:1 | `p30`→`p31` | §6.46 ② |
| 35 | **派发 ≠ 命令缓冲** ⇒ 合并派发**不触 V4**（`cbs/lane` 全程 2.00）| `p31` | §6.44 ④ |
| 36 | **设备侧 gather 表每个 run 重建**（3.0/跳），与"一跳一次"的注释矛盾 ⇒ 加缓存后 1.0/跳 | `p30`→`p31` | §6.45 |
| 37 | **未声明的静态库依赖在 macOS 能链、Linux 不能**：`ocudu_lower_phy`/`gnb_base` 用了 `ocudu_phy_support` 的符号却没声明 | Ubuntu 构建 `undefined reference`；修后 `BUILD=0`、`ctest -L phy` **184/184** | §6.43 ⑦ 7 |
| 38 | **流程事故（教训）**：只跑 `ocudu_versioning`（刷新戳）而**不重建 `gnb`** ⇒ 守卫（戳=HEAD）通过但二进制是旧的；`p29` 因此白飞 | `hashes.h` 17:22 vs `gnb` 17:05 | §6.44 ⑥ |

**仍然成立的老结论（别重犯）**：`busy`/`busy split` 是**占用窗口**不是算力（§6.29 ④）；**窗口 ≠ 关键路径代价**（§6.31 ③）；
池容量是**2 的幂**（§6.37 ②）；`starved_events` 是"进入 nearly-dry 的**次数**"（§6.36 ③）；夹具里的 `[dl_tx_slack]` **不是**空口读数（§6.41）。

---

## 3. 下一步（开工清单）

> **主线 = 继续压 V1**（用户已裁决）。**工具是一把标尺**：跳内每合并 1 次派发 ⇒ **≈ −12 µs**（`p31` 校准），
> 而**派发 ≠ 命令缓冲** ⇒ 全程**不触 V4**、不需要新裁决。

### 3.1 第一件（**推荐先做**）：去掉 `y_gather` —— 变体 A（只动引擎）

**方案已写好：开发文档 §6.47**（含已查清的事实、两个变体、预登记）。要点：

* `y_gather` 是**纯拷贝**（自身注释自述 *"a plain copy"*）：用 `taps/entries` 把网格里的接收符号搬进均衡 kernel 期望的
  连续布局（`[symbol][port][re]`），**无算术**，两侧都是 `cbf16_t`；而均衡派发**只认"一个缓冲 + 偏移 + 每符号步长"**
  （`setBuffer:b_y … atIndex:1` + `strides.y_stride`）。
* **变体 A（首选）**：当这一 run 在网格里**本来就连续可描述**（判据候选：`nof_ports==1`、`subc_stride==1`、
  `y_stride` 与网格 `symb_stride` 一致）⇒ **跳过 `y_gather`，把网格缓冲直接绑成 `b_y`**
  （偏移 = 该 run 首符号的字节偏移、`y_stride = grid.symb_stride`）。**逐字节相同**（同一批 cbf16、无转换）；
  条件不满足则**退回原路径**。建议加旋钮 `OCUDU_EQ_DIRECT_GRID`（默认开）便于 A/B。
* **变体 B（后备，要动 kernel/metallib）**：给均衡 kernel 加"读时 gather"模式（绑定网格 + taps/entries）。
* **施工第一步**：找到 `ch_gather_desc` 定义 → 在 `eq_flush_hook` 的 `y_gather` 调用点前加判据分支 →
  跑不变量网 → **重建戳与 `gnb` 两者** → 交给用户飞一条腿。

**预登记（腿 `p32-n78-directgrid`）**：`sites(y_gather=)` **3.0 → 0**、`burst dispatches` **10 → 7/跳**、
`merged_hop` 与 **V1 各 ≈ −36 µs**、契约 8/8、`cbs/lane=2.00`、gaps 0、D16 `batch_max=14 batch_src=auto`。
**反例判读**：`y_gather` 仍 3.0 ⇒ 条件没命中（走了回退），先读 `nof_ports`/strides，别当作"没收益"。

### 3.2 第二件：解开 `estimates` 断因，让 run 覆盖整跳

`eq_batch first_break=estimates` ⇒ 12 个符号被切成 **3 个 run**（最长 8）。判据要求各符号的估计切片
**共享一个缓冲且按固定步长前进**（`h_step = head.h.layer_stride ?: nof_re`）⇒ 修**信道估计的输出布局**即可让
run 覆盖整跳：3 → 1 ⇒ 再省 ~2 次派发 ⇒ **≈ −24 µs**（与 3.1 合计 ≈ −60 µs ⇒ V1 ≈ **1400**）。
**出处**：§6.44 ③、§6.45 ①。

### 3.3 第三、四件：剩下 5 次派发，以及"非派发"的那 ~470 µs

* **③**：信道估计的 **2 次**与解映射的 **1 次**能否并/提前（可先读这两处的编码路径）。
* **④ 最大也最难**：`merged_hop` 592 µs 里除 10 次派发（≈120 µs）之外还有 **~470 µs**（真实算力 + 派发之间的依赖等待 +
  宽 kernel 自身窗口）。要拆它只有两条路：**"移除某个阶段"的臂**（现有旋钮：`OCUDU_CE_NO_K4`、`OCUDU_CE_CORR_{FENCE,SEGMENT,UNIFORM,BARRIER_AFTER}`、
  `OCUDU_CE_{WEIGHTS_BARRIER,INV_BARRIERS}`、`OCUDU_EQ_{DEFER_ENCODE,DEV_TABLES}`、`OCUDU_INV_{TGX,TGY,RL,MEMNONE}`）
  或**单 kernel 离线微基准**（§6.29 对 DFT 做过，工具是 `wip/dft_kernel_cost.mm` 的形状）。

### 3.4 另案与可选（都不属于当前主线，需要时再取）

* ⏸ **V3 + 残留 gap（电台/USB 传输侧，暂停）**：重启入口 = 先读 **`[dl_tx_call]`**（`transmit()` 调用自身耗时，§6.42 ④）
  ⇒ "调用内阻塞（电台/USB 背压，宿主无解）" 还是 "瞬间返回、样点躺在 UHD 队列里（CPU 争用）"；
  然后同日**并发 1 vs 2** 或 **CPU 亲和/优先级**臂。判据（≤10）与 gap 的偶发性**都要按对累计**，别用单腿红绿说话。
* **A 项（等样点 ≈473 µs，零算力）**：`P1-7` 符号级收包（`OCUDU_UL_RX_SYMBOLS=N`）是它的载体，
  但**触 V4**（提交数）且其代码注释写明：**它的时延收益目前无法判读**（`[ul_pipeline]` 的窗口会随策略移动，
  且它的 shutdown 仍会触发 DU teardown race，与 `OCUDU_UL_RX_POOL_SIZE` 同类）。⇒ **需要用户裁决**才能动。
* **extended CP 取证腿**：12 符号路径已实现且离线自证（metal **arm 17**），但**没有空口腿**。
* **D4/gap 按对累计**（零腿，方法见 §6.32 ③）；**P1-6**（拆 B/E 项，`OCUDU_UL_SLOT_TRACE`）；
  **两条陈旧网**（`value_net.py`、`ab_dumps` arm1）；**Q12**；**P2-F**（并发度作为交付，需二次裁决）；
  **P2-A**（估计器提前启动）；**MAC 侧从丢失 CRC 指示恢复**（HARQ 饥饿，独立缺陷，未修）。
* **Ubuntu 侧**：本会话修好的两处 CMakeLists 已**就地同步**到 `jwang@192.168.31.211:~/work/ocudu`
  （那边 HEAD 仍是 `261d8d60d1`）；**用户下次 `git pull` 会拿到全部修复**。那边构建命令：
  `cmake -S . -B ./build -DCMAKE_BUILD_TYPE=Release -DENABLE_FLOW_PROBES=ON && cmake --build build -j16`。

---

## 4. 纪律（这一条条都花过代价）

1. **判据不许为过关改阈值**；**"读不出"按 RED 算**；**单条腿不是证据**（要同日 A/B 或复现腿）。
2. **改完代码必须重建两样**：`cmake --build build --target ocudu_versioning && cmake --build build --target gnb`。
   ⚠ **只跑 versioning 会让守卫说谎**（`p29` 的教训，§6.44 ⑥）；`run_leg.sh` 现在用**内容判据**
   （二进制里必须能找到它声称的戳）拦这件事。
3. **测试可执行文件不在默认构建目标里**：改引擎之后必须**显式构建**依赖 `ocudu_dft*` 的 7 个目标再跑 `ctest`
   （`channel_equalizer_*`、`dft_processor_metal_unit_test`、`dft_release_adopt_metal_test`、`helena_head2head_bench`、
   `ofdm_demodulator_metal_batch_test`、`port_channel_estimator_metal_mmse_unit_test`、`ul_chain_replay`）——否则网的绿是旧二进制说的。
4. **A/B 先读机制计数器**：`sites(...)`、`batch_max`/`batch_src`、`batched=`、`dropped=`、`take sweeps=`。
5. **用户飞腿时绝不碰 GPU**；腿用**单次 Ctrl-C**停并**确认进程退出**；判 V1–V5 的腿**不带 `OCUDU_METAL_GPU_TIME=1`**。
6. 需要留存的开发产物放 `doc_chinese/work_tmp/`（git 忽略）；引用材料**用章节名/腿名，不用行号**。
7. 网不许与并行构建同时跑；**保留第一次读数**（flaky 测试重跑绿也要记，§5.4）。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§3 判据（V1–V5）、§4 仪表手册、§5 跑腿规范、**§6 追加式记录（本会话 = §6.40–§6.47）**、§7 杠杆、§8 未决 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5 逐条、下一步、"继续压 V1"的当前路线 |
| `doc_chinese/phy_latency/README.md` | 三类文档分工 + **结项状态**（2026-09-25）|
| `doc_chinese/phy_latency/wip/p0_gate.sh` | **P0 门（D1–D17，只读日志）**；`p0_gate_selftest.sh` 是它的双向自测 |
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` | 起腿（**戳 + 内容判据双重守卫**）；腿日志在 `…/wip/logs/` |
| `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm` | **下一刀的主战场**：`eq_flush_hook`（run 划分）、`eq_gather_tables`/`eq_build_gather_on_device`（表，已一跳一次）、`eq_encode_gather_dispatch`（**`y_gather`，要去掉的那个**）、`eq_encode_batch_dispatch`（均衡派发）、`sites(...)` 计数 |
| `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm` | 信道估计（2 次派发/跳；`estimates` 断因的另一端；`OCUDU_CE_*` 旋钮） |
| `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.{h,mm}` + `ocudu_dft.metal` | 前端批量化（§6.30/§6.33：`OCUDU_DFT_BATCH_SYMBOLS` AUTO）；`batch_stats()` |
| `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` + `lib/phy/lower/lower_phy_factory.cpp` | 接收池（**P2-D 定尺 16**，启动行打印依据）、清扫入口点（§6.35）、TX 探针（§6.41） |
| `lib/phy/metal/ocudu_metal_burst.{h,mm}` | 交棒注册表、清扫（第三入口点）、`dispatches`/`eq_batch` 计数 |
| `doc_chinese/phy_latency/wip/dft_kernel_cost.mm` / `metal_wait_timeout_probe.mm` | 两个离线工具（DFT 派发形状 / Metal 5 s 等待上界）；**④ 的"单 kernel 微基准"照第一个的形状做** |
| `lib/phy/generic_functions/metal/test/dft_release_adopt_metal_test.mm` | metal 自测（**arm 10–17**：Q9-F/Q9-F3、握手、按需 dump、批量化 14/12/AUTO、take 清扫） |

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-25-4.md`。
> **目标（V1/V2）已结项**（开发文档 §6.43），现在在**继续压 V1**：`p31` 已到 **1463.5 µs**，并校准出
> **"跳内每合并 1 次派发 ≈ −12 µs"**。**下一步 = 开发文档 §6.47 的变体 A**：在
> `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm` 的 `eq_flush_hook` 里，
> 当这一 run 的 y 在网格里连续可描述时**跳过 `y_gather`、把网格缓冲直接绑成 `b_y`**（`strides.y_stride = grid.symb_stride`，
> 建议加 `OCUDU_EQ_DIRECT_GRID` 旋钮）；先跑不变量网（`channel_equalizer_metal_unit_test`、`ul_chain_replay` 逐字节/容差、
> `value_net`、`l1_handover_arms.sh`），**再重建 `ocudu_versioning` 与 `gnb` 两样**，然后给我 `p32-n78-directgrid` 的腿命令。
> 预登记：`sites(y_gather=)` 3.0 → **0**、派发 10 → **7/跳**、`merged_hop` 与 **V1 各 ≈ −36 µs**、契约 8/8、`cbs/lane=2.00`、0 gaps。
> **V3 与残留 gap 已另案暂停**（电台/USB 传输侧），不要顺手去动它。
