# Session handoff —— 2026-09-18（S-7g-16/17/18：CE 融合落地 → 前端进裁判 → 前端合批，三件都上机兑现）

> 换会话先读这一份，再读 `s2_full_chain_design.md` 的 **§48.146(a) 状态索引**。
> 本 session 的全部细节：**§48.188**（CE 融合 Step 1b + 两条腿判读）、**§48.189**（前端 fence / 每 slot 等待）、
> **§48.190**（前端进裁判）、**§48.191**（命令缓冲成本、分批 vs 合批的总账、合批实现与判读）。
> 上一份：`session_handoff_2026-09-17-2.md`（Step 1a + 停机三连修）。

---

## 0. 一句话现状

**代码全绿、能干净停机；"从 IQ 开始、在 GPU 里不断流的那一段"本轮从 3 段接到 2 段、前端又从 14 段接到 1 段。**
默认配置下：CE 已融进 lane 的命令缓冲（`cbs/lane` 3→2）、前端 DFT 每 slot 合成**一条**命令缓冲（`dft transforms/commits = 14`）、
网格**每 slot 只等一次**宿主。剩下两个同步点：**K0-a 自有 CB**（每 hop 一次）与 **lane 末尾那次 commit**（硬约束）。
**下一步 = Step 1′**（CE→EQ 用 `MTLEvent`，偿还 CE 融合的 +125 µs 重叠债），方案见 §5。

---

## 1. 代码状态（回来第一件事：核对这一节）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `apple-silicon` / **`4d66f44c70`**（已 push）|
| 里程碑 tag | `gpu_phy_fused_lane_ready` → `690b086679`（**Step 1a 基线，没动**）；本轮尚未打新 tag |
| 旧 tag | `gpu_phy_iq2llr_zero_host_copy → 3d026939eb` 冻结不动 |
| 树里的二进制 | `build/apps/gnb/gnb`，stamp = `4d66f44c70`；核对：`strings build/apps/gnb/gnb \| grep -c 4d66f44c70` == **2** |
| 参考二进制 | `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref` **＋并排 4 个 `.metallib`** |
| 语料 | `doc_chinese/work_tmp/corpus/`（27 个合成捕获）|
| 上机日志 | `doc_chinese/work_tmp/logs/`（**每个腿两份**：`*.log`（ocudulog）与 `*.log.stderr`（计数/契约））|

**本 session 的提交链（最新在前，全部已 push）**

```
4d66f44c70  front-end batching: default on - its leg confirmed the saving and found nothing else
74e16bd712  front-end batching: close the open encoder on teardown, and count transforms, not command buffers
f8dae98daf  front-end batching: one command buffer for the transforms of a block of samples (opt-in)
a9299ac662  dft test: what a command buffer costs, and it is not the transform
7f78554ea9  lane probe: the front end gets a place in the device timeline (S-7g-17, step 3)
e1dd46931e  dft stats: slots_in_flight is the pipeline depth, not commits minus waits
23bcb2ebfe  OFDM demodulator: the grid is waited for once per SLOT, not once per symbol
532e0b4658  front-end fence: the declaration bit's headers (the lib/ commit missed include/)
c2b9b123bb  front-end fence, step 2b: the per-symbol host wait goes away where the grid is read on the device
ecd04d5463  front-end fence, step 2a: an event relates the DFTs to the back-end stages that read the grid
d6bdc7791d  fused lane: the fusion is the default route, not an opt-in experiment
d6b6cad3bc  fused lane test: the estimator's unit test clears the fusion knob up front
f6ea2efad2  fused lane, step 1b: the deferred hop hands its dispatches to the lane's command buffer
9b8b471278  （本 session 起点）fused lane, step 1a
```

**未提交 / 不要提交**：`configs/*.yml`（用户本地环境改动）、`doc_chinese/`（gitignored）、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh`。

---

## 2. 本 session 做完的事

### 2.1 S-7g-16：CE 融合（Step 1a 补齐 + Step 1b + 翻默认）
- **发现 Step 1a 漏了热路径**：它只把四个走 `begin_stage()/end_stage()` 的独立入口接进 burst，
  而 air 每 hop 走的是 `run_async()`（自建 CB + commit + `pending_cb`）。补齐后：两个 `*_async` 权重入口也进 stage 体系，
  每次 pipeline 切换都经 `shared_burst::encoder()`（**换 stage 才插 barrier**），`*_async` 收尾用 `end_stage_async()`（burst 模式下**不发布 pending_cb**）。
- **新硬约束 §48.188(c)**：**宿主在 lane commit 之前要读的东西，不许进 burst**。K0-a（`gpu_ls_cfo`/`gpu_ls_sigma2`/LSE 宿主在本 hop 内读）
  与独立 K0-d（宿主读 A 就地求逆）**因此不融合**；`begin_stage()` 改成显式传 `fuse`。反例：4 DMRS shape（L=72 > 54 ⇒ 宿主求逆）在错误融合下输出全错。
- **完成路径要能自己 commit**：`complete_fused_burst()` = `commit()` + `wait_committed()`——宿主读回路线（`OCUDU_CE_CPU_CE=1`、dump）
  在提交均衡之前就完成估计，那时没人提交过 burst。
- 顺手修：`build_correlation()` 二次 `endEncoding`（`OCUDU_CE_GPU_INVERT=0` 一开就 abort）、`shared_burst::size()` 恒为 0。
- CE 单元测试新增 **Test 13**（4 shape × 4 路线逐字节 + "旋钮真生效"断言 + 钉住默认值），**并加入 `run_metal_engines.sh`**。
- **默认翻"融合开"**（`d6bdc7791d`，`OCUDU_CE_FUSED_BURST=0` 为逃生门）。

### 2.2 S-7g-17：前端（fence 机制 + 每 slot 一次等待 + 进裁判）
- **侦察**：唯一付费点是 `ofdm_symbol_demodulator_impl::finish_symbol()` 的 `dft->wait_slot(slot)`（每符号一次）；
  根因是 DFT 在 front_end 队列、CE/EQ 在 back_end 队列，而 `shared_queue` 只保证**同队列内**按提交顺序完成。
- **Step 2a**：`shared_queue` 加 `MTLSharedEvent` + **代际**；DFT 两个 commit 点编 signal；后端在 CB 创建时 wait（`begin_stage()` 与 `burst_ensure_open()`）。
  代际在 commit **之前**取 ⇒ 不会等一个没人发的信号。`OCUDU_UL_FRONTEND_FENCE`（**默认关，且实测负收益，不要开**）。
- **Step 2b 全跳过 = 负收益（已回退为"每 slot 一次"）**：fence 腿把 `dft waits` 从 1:1 打到 **1**，但等的是"**最新已提交代际**"，
  而前端不再被节流（`max_in_flight` 涨到 17 万）⇒ 等的是不需要等的工作；K0-a 同步收集又把 GPU 等待变成宿主阻塞。
  实测 `[ul_pipeline] mean 2593 → 3523 µs`、`crc OK 91.9% → 59.9%`、4 次 RF 失败。**⇒ 纪律：同步点的价值 = 它与真实依赖的距离。**
- **每 slot 一次等待**（`23bcb2ebfe`，默认行为）：网格只在 slot 完成后被消费 ⇒ 等待属于该 slot 的**最后一个符号**；
  `dft waits` 1:1 → **1/14**、`[ul_pipeline]` 回到 2631 µs、`crc 95.9%`、失败 0.00000%。
- **Step 3：前端进设备侧裁判**（`7f78554ea9`）：`set_lane_slot()` 从 FSM→解调器→DFT 引擎；探针按 slot 分组，
  输出 `[ul_gpu_lane] dft slots/cbs/residency/busy/gap`（与后端 lane 并列 ⇒ 整条 IQ→LLR 第一次有裁判）。
- 顺手修：`dft max_in_flight` 语义（改为按 `slot_pending[]` 统计真实流水线深度 `slots_in_flight`）。

### 2.3 S-7g-18：前端合批（**默认开**）
- **读数**：前端每 slot **531.6 µs** GPU（与后端 lane 509.7 µs 相当）⇒ 一个上行 slot 合计 ≈1.04 ms GPU vs 1 ms slot 周期。
- **测量**：DFT 单测新增 per-command-buffer 成本读数 ⇒ **CB 的 GPU 时长与装几个变换无关（≈12.7 µs）**，FFT 本身 <1 µs
  （air 每 CB ~38 µs，多出的 ~25 µs 是同一 CB 里的网格写）。
- **总账（§48.191(d)）**：分批 commit **是否免费**取决于接收块策略——
  `whole slots`（**默认**，我们跑的）"前端要等整个 slot 的样本才能变换第一个符号"⇒ 合批**不新增等待**；
  `whole symbols`（`OCUDU_UL_RX_SYMBOLS=N>0`，实验）"符号一到就变换"⇒ 合批会白等。
  ⇒ **规则：只合批"已经到达的样本"**。
- **实现**：`begin_block()`/`commit_open()`（一个 CB 装一块的变换）；等待路径先关块（`wait_slot`/`wait_all`）；
  解调器在 **slot 变化**时开块、在 **slot 最后一个符号**处关块；**符号粒度策略下拒绝生效**（反例门已验证）。
- **上机兑现**（`gnb_batch_0918_0617`）：`dft transforms/commits = 14.00`、`dft slots == cbs`、前端 busy **531.6 → 424.0 µs**、
  端到端 `[ul_pipeline]` **−105 µs**（1:1 对应 ⇒ 前端在关键路径上）、契约/红线/后端 lane 不动、停机正常。
- **翻默认**（`4d66f44c70`）；存档腿（`gnb_default_0918_0627`，无任何旋钮）复现：`253471/18106 = 14.00`、`dft busy 422.8 µs`、
  `gap 0.0`、契约 7/7、**失败率 0/122898 = 0.00000%**、`cbs/lane=2.00`。

---

## 3. 新会话必须知道的硬约束与口径

1. **每 lane 一次同步是硬约束**（LLR 交给 CPU 的 LDPC）；融合只去掉"每 stage / 每符号一次"的那些。
2. **宿主在 lane commit 之前要读的东西不许进 burst**（K0-a 的 CFO/σ²/LSE、独立 K0-d 的 A）。融合的判据是"**谁在什么时候读**"，不是"这个 stage 看起来在不在设备侧"。
3. **同步点的价值 = 它与真实依赖的距离**：等"最新"往往等于等"不需要等的"（fence 腿的教训）。
4. **接收块策略决定"分批 commit 是否免费"**：`whole slots`（默认）前端本来就等整块；`whole symbols`（实验）才有"到达即变换"的重叠。**合批只合已到达的样本。**
5. **指标口径**：
   - `[mmse_time_sum] gpu_wait` 是**命令缓冲的 GPU 时长**（不是宿主阻塞）；宿主阻塞看 `cpl_wait`。
   - `[ul_pipeline]`/`[ul_time_frequency]` 的端点在两条接收策略下分别是 slot 的**末**与**首** ⇒ **不可跨策略比**。
   - 跨策略比较**只认 GPU 侧序列**：`[ul_gpu_lane] dft residency/busy/gap`（前端）与 `residency/busy/gap`（后端 lane）。
   - `[metal_stats] dft commits/transforms/waits/slots_in_flight`：`transforms/commits` = 每 CB 几个变换（合批后 ≈14）。
6. **`thread_local` 的析构早于 `atexit`**：凡在 `report()`（atexit）里读的线程局部状态，必须 `new` 出来故意不回收。
7. **release 未 `endEncoding` 的 encoder 会 abort**（Metal 验证层）：任何"打开的命令缓冲/encoder"都要有析构/停机路径关闭它。
8. **契约里的分母要跟着策略改**：合批后"commits"（CB 数）≠"transforms"（变换数），两个数都要报。
9. **停止顺序**：`ru_controller_sdr_impl::stop()` 先停 lower PHY（消费者）、再停电台（来源）。
10. **`--log.filename` 不建父目录**；每次跑用不同文件名。参考二进制必须带着它自己那批 `.metallib`。

---

## 4. 本 session 的七条腿（同 5 MHz/n1 配置，只差被验证的机制）

| 腿 | 日志 | `dft commits/waits` | `cbs/lane` | lane busy/gap µs | `dft busy`/slot | `[ul_pipeline]` mean | crc OK | RF 失败 |
|---|---|---|---|---|---|---|---|---|
| baseline（CE 融合关）| `gnb_baseline_0917_2245` | 189715/189715 | 3.00 | 515.3/115.4 | — | 2482.8 | 92.9% | 0/51993 |
| fused（CE 融合默认）| `gnb_fused_0917_2241` | 216539/216539 | 2.00 | 500.3/246.6 | — | 2593.4 | 91.9% | 0/87304 |
| fence（全跳过宿主等待）| `gnb_fence_0917_2318` | 201237/**1** | 2.00 | 483.1/322.7 | — | **3523.1** | **59.9%** | **4**/131126 |
| slotwait（每 slot 一次）| `gnb_slotwait_0917_2330` | 191647/13690 | 2.00 | 492.2/259.1 | 531.6 | 2631.2 | 95.9% | 0/74794 |
| frontend（+前端裁判）| `gnb_frontend_0917_2347` | 173335/12382 | 2.00 | 509.7/265.7 | 531.6 | 2663.0 | 93.6% | 0/66033 |
| batch（合批，显式开）| `gnb_batch_0918_0617` | 12274/12274（**14.0 变换/CB**）| 2.00 | 502.8/246.6 | **424.0** | 2526.1 | 84.2% | 1/68030 |
| **default（合批默认，无旋钮）** | `gnb_default_0918_0627` | 18106/18106（14.0 变换/CB）| 2.00 | 510.7/295.8 | **422.8** | 2660.0 | 87.9% | **0/122898** |

> 跨腿的 `[ul_pipeline]` 受流量影响很大（每腿 PDU 总量 260–455 KB 不等），**不要跨腿比它**；机制判据用 `dft busy`/`transforms/commits`/`cbs/lane`。

---

## 5. 下一步

### 5.1 Step 1′（**首选**）：CE→EQ 用 `MTLEvent`，偿还 +125 µs 重叠债
- **现状**：CE 的 `run_async`/`encode_weights_only` 编进 lane 的**同一条 CB**（`ocudu_metal_mmse_engine.mm:436/469`）⇒ 只有宿主把整个 group 的 EQ/demap 编完才 commit
  ⇒ **CE 的 GPU 工作不再与宿主编码重叠**（`[ul_equalization_demod]` 652 vs baseline 527，+125 µs）。
- **做法**：CE 放回**自己的 CB**（早 commit ⇒ 重叠回来）；CE→EQ 的顺序改由 **`MTLEvent`** 保证
  （两条 CB 都在 back-end 队列，跨 CB 只有"开始顺序"、没有"完成顺序" ⇒ 事件正合适）。复用前端 fence 的成熟模式：
  `shared_queue` 再加一组"后端 stage fence"（事件 + 代际）；CE 的 CB 末尾 signal、lane burst 开头 wait；
  适配器完成路径不再等 CE 的 CB。
- **自查重点**（上一条腿崩过的地方）：**事件与 encoder 的生命周期**、停机路径、`wait_all`/`wait_slot` 是否会漏掉打开的 CB。
- **判据**：离线逐字节不变（replay 走的就是融合 lane）+ 上机 `[ul_equalization_demod]` 652 → ~527、`[ul_pipeline]` 再降 ~125 µs、红线/契约不动。

### 5.2 其余（次序可按取舍调整）
- **Step 4**：lane 并发 `max_in_flight` 1→N（跨 lane 重叠；后端 lane `gap` 仍有 ~250–295 µs，是下一笔可观的量）。
- **K0-a 融合**：它的 `gpu_ls_cfo`/`gpu_ls_sigma2`/LSE 不是"读回来看看"，而是**编码期输入**（喂 A 的对角、噪声核的 `cfo`/`pilots_power`）
  ⇒ 要么让内核改从设备内存读（内核签名改动），要么保留这每 hop 一次的宿主同步；**独立设计**。
- **fence 精确目标**（按 slot 的代际环形表）：去掉最后 1 次/slot 宿主等待；收益小、风险是过度等待，优先级低。
- **顺带**：用新口径确认 S-7g-13（`OCUDU_UL_RX_SYMBOLS=4`：预期 `=0`≈2400–2500 µs、`=4`≈1800 µs）+ 干净环境复核失败率；
  全绿后按 §48.181 政策打新里程碑 tag。

---

## 6. 关键文件与路径

| 内容 | 路径 |
|---|---|
| 设计文档（唯一活文档）| `doc_chinese/full_gpu_chain/s2_full_chain_design.md`（**§48.146(a) 状态索引**；本 session：§48.188–§48.191）|
| 上机腿脚本 | `doc_chinese/full_gpu_chain/wip/run_air_leg.sh <label> [OCUDU_*=…]`（root、knob、日志名、**stderr 双捕获**）|
| 腿判据脚本 | `doc_chinese/full_gpu_chain/wip/air_leg_report.sh <log…>`（一次出表：crc/失败率/契约/metal_stats/lane/dft fence 行）|
| 融合自对拍 | `doc_chinese/full_gpu_chain/wip/ab_fused_lane.sh <N>`（同一二进制 `OCUDU_CE_FUSED_BURST=0/1`，4 条路线逐字节）|
| 每腿门 | `run_ab_all.sh <ref> 27` → `run_gates.sh` → `run_metal_engines.sh` → `/tmp/build_{nostats,nometal}`（**串行**）→ **Ubuntu** |
| 本轮改动的代码 | `lib/phy/metal/ocudu_metal_burst.{h,mm}`、`ocudu_metal_lane_probe.{h,mm}`、`ocudu_metal_queue.{h,mm}`、`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.{h,mm}` + `dft_processor_metal.{h,cpp}` + `test/dft_processor_metal_unit_test.cpp`、`lib/phy/lower/modulation/ofdm_demodulator_impl.{h,cpp}` + `include/ocudu/phy/lower/modulation/ofdm_demodulator.h`、`lib/phy/lower/{lower_phy_factory.cpp,lower_phy_baseband_processor.cpp}`、`lib/phy/upper/signal_processors/channel_estimator/{port_channel_estimator_average_impl.{h,cpp},metal/*}`、`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp`、`apps/units/flexible_o_du/split_8/helpers/ru_sdr_config_translator.cpp`（只读参考）|
| 上机日志 | `doc_chinese/work_tmp/logs/gnb_<label>_<MMDD>_<HHMM>.log(.stderr)` |
| 参考二进制 | `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref` + 4 个 `.metallib` |

**开关台账（默认值从代码核实）**：`OCUDU_CE_FUSED_BURST`（**默认开**，`=0` 逃生门）、`OCUDU_DFT_OPEN_BLOCK`（**默认开**，`=0` 逃生门）、
`OCUDU_UL_RX_SYMBOLS`（默认 unset/0 = whole-slot 块；>0 = 符号粒度实验）、`OCUDU_UL_FRONTEND_FENCE`（**默认关；开着只会过度等待，别开**）、
`OCUDU_METAL_GPU_TIME`（opt-in，会扰动）、`OCUDU_CE_CPU_CE/CPU_LS/GPU_INVERT/CPU_INVERT/CORR_DEV/SPLIT_TAIL/DEV_SIGMA2/DEV_Y`（A/B 或逃生）。

---

## 7. 回来的第一个动作（TL;DR）

1. `git log --oneline -3` 确认 HEAD = **`4d66f44c70`**；`cmake -P build/build_info.cmake && cmake --build build --target gnb -j 14`，核对 stamp（`strings … | grep -c` == 2）。
2. 读设计文档 **§48.191(g)**（存档腿读数）与 **§48.188(c)/§48.189(f)/§48.191(d)**（三条硬约束）。
3. 做 **Step 1′**（§5.1）；每步先离线逐字节，再上机一条腿（模板见 §6 的脚本）。
4. 之后 Step 4（lane 并发）→ K0-a（独立设计）→ 顺带确认 S-7g-13 与失败率 → 打里程碑 tag。
