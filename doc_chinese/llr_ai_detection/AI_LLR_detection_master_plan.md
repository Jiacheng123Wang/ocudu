# AI LLR Detection 工作流 — 详细规划

> **工作流名称**：`llr_ai_detection`
> **一句话**：把 PUSCH 接收链的"**信道估计 + 均衡 + 解映射**"换成一次神经前向，
> 输入是 DFT 后的时频网格**及其几何**（哪些是 DM-RS RE、哪些是 data RE、它们的相对关系），
> 输出是**加扰域 LLR**，交给现有 LDPC 译码器。
> （实测：`ce` **73.3 µs** + `eqdem` **667.4 µs**；设备侧的融合单元 `merged_hop` 中位 **449.8 µs**。）
> **前身**：`metal_kernel_fusion`（已收官：融合清单穷尽，性能目标在吞吐/占用维度关闭）。
> 本工作流继承它的问题陈述——"一格 667 µs 里只有几十 µs 是算力"——并换一条完全不同的路去解决它。
>
> 版本：v1.6 ｜ 状态：**规划（待 P0 裁决）** ｜ 日期：2026-10-07
> 配套文档：`memo_01_repo_seams_and_llr_contract.md`（接缝与契约）、
> `memo_02_paper_2503.16594_reading.md`（参考论文）、`memo_03_literature_survey.md`（文献调研，完整版）、
> `memo_04_data_labels_and_operating_point.md`（数据与工作点实测）
>
> ★ **v1.4 变更（范围澄清）**：本工作流的目标是 **"信道估计 + 均衡 + 解映射"融合成一个 net**
> （输入网格 + 几何，输出 LLR）——这是**标准 neural receiver 形态**，也是 OCUDU dApp 的 **depth 3**
> （预算 **150 µs**）。`memo_01` v1.0 描述的 `submit_fused` 只是它的**子集**（不含 CE）。
> 详见 `memo_01` §1.4–§1.6；第 1 章的接缝选择据此改写。
>
> **v1.2 变更（由完整版文献调研触发，均为架构级）**：
> ① ~~ANE 不再是目标引擎~~ —— ★ **v1.6 撤销此条**。原论证有两处硬伤：**调用粒度搞错**
> （是**每跳 1 次** dispatch，不是每符号 14 次——`merged_hop` 一个 CB 覆盖整跳）
> 与**拿 M1 的数字给 M4 Pro 定价**（我们实测 ANE **141 µs**，已含 dispatch；
> 且 ANE 比本机 MPS 路径 **快 4.9×**：141 vs 690 µs）。**ANE 仍是首选引擎**，
> 与仓库架构原则 *"AI 推理走 NPU"*（`apple_silicon_heterogeneous_gnb_plan.md:17`）一致。
> 详见 `memo_03` §1。
> ② ★ **路线 D（dApp）在 Apple Silicon 上不可用**（运行时只声明 CPU/CUDA，无 Metal/ANE/CoreML 后端，
> 且接缝不在本 checkout）⇒ **内联是唯一路线**（§1.3）。
> ③ 采纳 dApp 论文的 **Class A/B/C 时序契约**作为 G5 规格（§4）。
> ④ 新增 **R9：逐槽信任/回滚**（前作实测 60.5% 平均回滚率，`memo_03` §4.4）。
> ⑤ 定位澄清：我们属于 **LINE B（输出 LLR 的神经接收机）**，不是 LINE A（符号输出的深度展开检测器）。
>
> **v1.3 变更（文献调研 7 条线全部完成，5 条改变默认值）**：
> ① ★ **损失函数**：**逐比特 BCE(Binary Cross-Entropy, 二元交叉熵)是唯一主损失**——
> 即对每个编码比特的模型 logit 与真值比特做二元交叉熵。文献主流如此(Sionna/NVIDIA NRX);
> "对参考 LLR 做 MSE" 存在但属少数派(NVIDIA `LLRNet`),且本质是蒸馏 ⇒ 只作热身与消融(§3.2)。
> ② ★ **主指标**：**互信息不能预测 BLER**（可有近最优 MI 却落在错误平台上）⇒
> 门禁一律用**真实 LDPC 译码器之后的 coded BLER**，BER/MI 仅作诊断（§4）。
> ③ ★ **量化**：8-bit 基本免费（与 FP32 差 ≤0.05 dB）；**INT4 损失 3.3–3.7 dB 并跌破 LS-LMMSE**；
> FP4(E2M1) 可用（§3.4 新增）。fp16 的约束是**量程不是尾数**（LLR 头必须 clamp）。
> ④ ★ **信任/回滚**：**"AI 差就退回经典"是错的**——500 Hz Doppler 下**经典先崩**。
> 改为**逐时隙神经+经典并行仲裁**（代价 <5% 时延）（§1.3）。
> ⑤ ★ **ANE 的真实约束**（是设计约束，不是"所以别用 ANE"）：**自定义 Metal kernel 不能跑在 ANE
> 常驻图里** ⇒ 模型必须可由 **CoreML 原生算子**表达；任何自定义 Metal 前/后处理必须是**独立阶段**。
> 另加 fp16 量程（LLR 头 clamp）、工作集悬崖、`EnumeratedShapes` 宽度分桶（§2.6）。

---

## 0. 定位：两个目标必须分开排序，否则一定会自欺

| | 目标 | 内容 | 状态 |
|---|---|---|---|
| **G-A** | **时延** | `ce`+`eqdem` 两段 → 一次神经前向 + 同步开销，**CRC 不明显掉** | **第一目标**。★ **默认引擎 = ANE**（AI 推理专用；本机实测 141 µs vs MPS 690 µs），Metal/MPS 为对照臂，由 P4/P5 实测裁决是否例外（见 §2.6） |
| **G-B** | **精度** | 在少导频 / 高阶调制区间**超过**经典 MMSE + LLR | **待证伪的第二目标**，由 P0 裁决 |

★ 为什么必须分开：`memo_04` 实测我们的工作点是 **10.7% 导频密度、15–25 dB、宽分配、SISO**，
而参考论文（DEFINED）报告 43.7% / 71.1% 收益的区间是**极少导频**。**"AI detector 能提高精度"
是一个假设，不是前提。** 如果把它和时延目标混在一起讲，P0 一旦显示精度空间很小，
整个工作流就会被误判为失败——而它其实可能仍然值钱（时延）。

### 明确不做

- 不做端到端（**不替换 LDPC 译码器**）。译码器保持经典，LLR 是链条的接口。
- 不重构导频/参考信号，不动下行，不动 CE（CE 线由 `ai_ce` 独立负责）。
- 不追求"超过经典"作为 P0 之前的目标。

---

## 1. 接口契约（先钉死，再写代码）

### 1.0 ★ 先选深度：本工作流 = **深度 3**（CE + 均衡 + 解映射）

OCUDU dApp 的 Class A 契约把内联替换分成**三个深度**（`memo_01` §1.4，dApp 论文逐字）：

| 深度 | 替换 | 输出 | 生产预算 |
|---|---|---|---|
| 1 | 仅 CE | 信道估计 + 噪声 | **100 µs**（"estimation"） |
| 2 | CE + 均衡 | 均衡符号 + 后均衡噪声 | **150 µs**（"to completion"） |
| **3 ★ 本工作流** | **CE + 均衡 + 解映射** | **解扰前软比特（= 加扰域 LLR）** | **150 µs** |

选择深度 3 的四条理由：

1. **它是文献里 "neural receiver" 的标准范围**：Sionna 逐字 *"substitutes channel estimation,
   equalization, and demapping"*；DeepRx 逐字 *"the whole receiver pipeline from frequency domain
   signal stream to uncoded bits"*，且输入构造显式用到导频位置。
2. **它匹配我们的融合单元**：`merged_hop`（"the whole hop's buffer"）**已经**把
   CE 抽取 + CE 权重 + 均衡 + 解映射放在一个命令缓冲里（mkf033 中位 449.8 µs）
   ⇒ 替换的是一个**已经存在的融合单元**，不需要把两个独立阶段缝起来。
3. **dApp 论文明确说"不存在只替换均衡的钩子"**（*"no hook replaces equalization alone, because the
   equalizer interface also implements channel estimation"*）——浅接缝在架构上就是别扭的。
4. 收益更大：`ce` 73.3 µs + `eqdem` 667.4 µs 两段一起被替换。

★ **从第一天就上深度 3，且是"一个网络"**（§2.1 给出四条理由）：
我们**已经有 AI CE 模块**（HELENA + CoreML 引擎 + `ai_train` 工具链 + 采集/标签管线），
所以不存在"还没有 CE 能力、只能先做浅一层"的问题。深度 3 是**起点**，不是终点。

★ **但深度 3 继承了 CE 的"上报义务"**（见 `memo_01` §1.5）：CE 还产出 RSRP / EPRE / 噪声 / SNR /
TA / CFO，`pusch_processor_impl.cpp` 末尾把它们**合并进 CSI 上报**。dApp 论文的对应约束逐字：
*"A module that succeeds at the second or third depth **also supplies the scheduler's uplink SINR from
its own post-equalization noise**, because the conventional measurement kernels are skipped on that path."*

⇒ **本规划的选型（路线 a）**：net **附带输出信道估计与噪声**，经典测量核**跑在 net 的输出上**。
理由：上报口径不变（对调度器零影响），同时把最贵的那部分算力移走。

### 1.1 冻结项：宿主接缝（深度 3）

| 项 | 值 |
|---|---|
| 要替换的两处 | `pusch_processor_impl.cpp:264` `estimator.estimate(notifier, grid, ch_est_config)` **+** `:559` `demodulator.demodulate(buffer, notifier, grid, est_results, demod_config)` |
| 输入 1 | `resource_grid_reader& grid`（DFT 后的时频网格） |
| 输入 2（几何） | `dmrs_pusch_estimator::configuration`（`symbols_mask` = DM-RS 位置、`crb_bitmap` = 分配、`first_symbol`、`scaling`、`sequence_config`）+ `pusch_demodulator::configuration`（`rb_mask`、`modulation`、`start_symbol_index`、`nof_symbols`、`dmrs_symb_pos`、`dmrs_type`、`nof_cdm_groups_without_data`、`n_id`、`nof_tx_layers`、`dc_position`、`rx_ports`） |
| 输出 1 | 加扰域 LLR（int8，RE-major）→ 交给既有 `revert_scrambling` |
| 输出 2 | ★ **后均衡噪声方差**（上报统计，必须写）+ ★ **信道估计与噪声**（供经典测量核出 RSRP/EPRE/SNR/TA/CFO） |
| 回退 | 深度 3 不可用 ⇒ **经典 CE + 经典 eq/demap**（与 dApp 的"经典阶段在同一次调用里兜底"同构） |

★ **仍可复用的既有事实**（`memo_01` §1.2、§2、§3）：`ch_est_list::device_slice` 的零拷贝思路、
LLR 量化（`LLR_MAX=120`、`range_limit` 24/24/20/20）、**模型输出加扰域 LLR**、
以及后端开关的形状（`upper_phy_factories.cpp:743-756`）。

### 1.2 契约测试（是单测，不是文档）

1. **ALL-OR-NOTHING**：谓词为真时调用方**不得**再 demap 该符号（构造一个会断言二次 demap 的假 demapper）。
2. **谓词是 shape 的属性**：同一 shape 下对任意数值输入，谓词结果恒定。
3. **`eq_noise_vars` 非空且合理**：与经典后均衡噪声方差在同一量级（逐 RE 相对误差统计）。
4. **量化边界**：±120 裁剪、±127 语义、`quantize` 的 mid-tread 步长。
5. **加扰域一致性**：把经典 demapper 的输出与模型输出放在**同一域**对拍（同为加扰域）。
6. **回退等价**：开关关闭时，与 HEAD 的经典路径 **bit-exact**（用 `ab_dumps.sh` 的 0 差异控制臂）。

### 1.3 路线选择：内联（I）还是 dApp（D）—— **内联是唯一可用路线**

文献调研（`memo_03` §2、§3）查清了三件事：

| 事实 | 后果 |
|---|---|
| OCUDU dApp 运行时声明后端为 **"CPU (x86, ARM) 或 CUDA"**，**没有 Metal / ANE / CoreML 后端** | 路线 D **今天在 Apple Silicon 上不存在** |
| dApp 接缝**不在本 checkout**（无 `lib/phy/upper/dapp`），平台是独立预览仓库 `gitlab.com/ocudu/work_groups/wg2_ai_ran` | 走 dApp 需要**我们自己拥有一个 Metal 后端** |
| dApp 的实测记账：neural-receiver→LLR **每槽 1.47 MB 进 / 最多 4.4 MB 出、占用 ≤500 µs**；Class B 直接调用 **0.29 µs P50 / 5.2 µs P99.9** | 这笔账**用作 G5 的对标基线**（§4），而不是我们要走的路线 |

⇒ **结论：走内联（I）**。这不只是偏好——它同时避免把每槽 MB 级搬运重新加回一条
以"零主机↔设备数据穿越"为核心成就的 lane（`phy_pipeline_crossings.h` 的整个设计目的）。
路线 D 保留为**对照臂**，仅在"内联做不到"时才复审——且复审必须显式，不得悄悄改道。

### 1.4 ★ 逐槽信任 / 回滚（v1.3 重写：朴素回滚是错的）

前作实测：神经接收机需要一个**逐槽**的"是否信任自己"的判决。但**"AI 差就回退经典"这个朴素做法
被证明是错的**——*When Does a Neural Receiver Help?*（arXiv 2605.26157）在 **500 Hz Doppler** 下
观察到 ★ **经典接收机崩溃、而神经接收机能工作**（`memo_03` §7bis.G.3）。
所以回退的判据不能是"单方面不信任 AI"。

该论文的解法是 **逐时隙"神经 + 经典并行仲裁"**，代价 **<5% 时延**。其它可引用的实测细节：

| 事实 | 数字 |
|---|---|
| 16 个场景的结果分布 | **3/16 增益 1.0–2.0 dB；10/16 打平（±0.2 dB）**；QPSK 反而差 ~2 dB |
| 分布外配置的下场 | **DMRS AddPos=2 从 4 dB 起静默钉在 100% BLER** |
| "自信地判错"的比特比例 | **平台在 ~7%** ⇒ 给任何"有界 LLR 残差修正"设了上限 |

⇒ 设计调整：

| 项 | 设计 |
|---|---|
| 结构 | **并行仲裁**（神经与经典都算，逐槽选），而不是"先 AI 后回退" |
| 判据 | 必须**廉价且推理时可得**；不能依赖事后 CRC（那时已太晚） |
| 成本 | ★ 内联路线的优势仍在：**经典路径始终在链上**，仲裁不需要额外的数据搬运 |
| 度量 | 回滚率/仲裁率必须与收益**一起报**（只报收益 = 不诚实的比较） |
| 参考实现 | dApp 平台的做法：经典阶段在**同一次调用**里兜底、**连续 8 次迟到才打开 lane breaker**、权重双 bank 原子切换（`memo_03` §2.1） |

⇒ **G6 的判据必须同时包含仲裁/回滚率**，否则统计门不成立。

## 2. 模型设计：★ 一个网络做深度 3（不是 CE 与 EQ/DEM 两个网络）

任务重述：**一个**网络，`f(时频网格, DM-RS 位置张量, data-RE 索引, PUSCH/DM-RS 元数据)`
→ 逐 RE 逐比特**浮点 LLR**（主输出），**同一个 trunk 另出**后均衡噪声方差（上报 SINR）
与信道估计 + 噪声（供经典测量核出 RSRP/EPRE/SNR/TA/CFO）。

### 2.1 ★ 为什么必须是"一个网络"（四条理由）

| # | 理由 | 依据 |
|---|---|---|
| 1 | **文献里就是这个形态** | **DeepRx** 是**单个**全卷积（ResNet）网络，*"executes the whole receiver pipeline from frequency domain signal stream to uncoded bits"*；**Sionna** 的 neural receiver 也是**一个**网络替换三步（`memo_03`） |
| 2 | **两个网络 = 中间插一个有损瓶颈** | CE 的输出（`cbf16` 信道估计）成为不可逆的信息瓶颈；端到端梯度在此断掉。而"估计误差如何在解调里传播"恰恰是联合网络能学、级联学不到的东西 |
| 3 | ★ **dispatch 次数本身就是成本**（我们自己的实测） | mkf023：一个边界 ≈ **6.5 µs**；`merged_hop` 一跳有 ~6.9 个 dispatch。加上 ANE 的**单次 dispatch 地板**（M1 0.23 ms / M5 Pro ≈70 µs）⇒ **多一个网络就多付一次地板**。`memo_03` §1 |
| 4 | **dApp 的 depth 3 契约本身就是"一个模块多路输出"** | 同一段原文里，**一个** module 的输出同时含 channel estimates / equalized symbols / soft bits 三种深度，且 *"A module that succeeds at the second or third depth **also supplies** the scheduler's uplink SINR"* ——是**一个模块出多路**，不是三个模块串联 |

⇒ ★ **结论：主线从一开始就是"单个联合网络 = 深度 3"。**
A0/A1 **不是必经台阶**，只在需要**定位问题**时作为消融对照（见 §2.5）。

### 2.2 输入表示：几何怎么进去

正对应 dApp 契约的三件套（`memo_01` §1.4）：*"full-slot device grid … an explicit DM-RS pilot
tensor with bounded coordinates, compact ordered data-RE indices, and typed PUSCH metadata"*。

| 输入 | 形态 | 承载什么 |
|---|---|---|
| 时频网格 | `[2, T, F]`（I/Q 两实通道，或复数） | 观测量 |
| **DM-RS 位置张量** | `[1, T, F]` 0/1 掩码 | **哪些 RE 是导频**——★ 必须显式给，而不是让网络去猜 |
| **data-RE 索引 / 掩码** | `[1, T, F]` 0/1 掩码（或紧凑索引 + 散写） | **哪些 RE 要出 LLR** |
| **元数据** | 标量/嵌入，广播到空间维 | allocation shape、`nof_tx_layers`、`rx_ports`、`modulation`、`nof_cdm_groups_without_data`、`dc_position`、`scaling` |

★ **导频掩码显式输入是关键设计选择**：DeepRx 的做法就是 *"constructing the input … in a very
specific manner using both the data and pilot symbols"*，而它的性能被归因于
*"learning to utilize the known constellation points of the unknown data symbols, together with the
local symbol distribution"*。让网络知道"哪些是已知的、哪些是待判的"，是这件事成立的前提。

★ **接缝已经提供了全部这些**（`memo_01` §1.1）：`resource_grid_reader& grid` +
`dmrs_pusch_estimator::configuration`（`symbols_mask` / `crb_bitmap` / `first_symbol` / `scaling`）+
`pusch_demodulator::configuration`（`rb_mask` / `modulation` / `nof_symbols` / `dmrs_symb_pos` /
`dmrs_type` / `nof_cdm_groups_without_data` / `n_id` / `nof_tx_layers` / `dc_position` / `rx_ports`）。

### 2.3 输出：一个 trunk，多个头

```
                    ┌──────────────┐
  网格 + 几何  ───► │  共享 trunk   │ ──┬──► 主头：逐 RE 逐比特 LLR（Qm 路）
  (2+2+meta)        │ 2D conv/ResNet│   ├──► 辅助头：后均衡噪声方差  → 上报 SINR
                    └──────────────┘   └──► 辅助头：信道估计 + 噪声 → 经典测量核出 RSRP/EPRE/SNR/TA/CFO
```

| 头 | 作用 | 损失 |
|---|---|---|
| **主头** | 逐 RE 逐比特浮点 LLR（再经 `quantize()` 落 int8） | **逐比特 BCE（对编码比特）**——唯一主损失（§3.2） |
| 辅助头 1 | 后均衡噪声方差 | 与经典后均衡噪声的回归（权重小） |
| 辅助头 2 | 信道估计 + 噪声 | NMSE（权重小），且**供经典测量核** |

★ **多任务是"一个网络"的必然结果，不是额外负担**：上报义务（§1.0）要求它顺带给出信道估计与噪声，
而共享 trunk 让这两件事互相正则化——这正是级联方案拿不到的部分。

★ **风险（必须写进 G2/G3）**：多任务权重失衡会让主头变差。判据是
**主头 coded BLER 不因加辅助头而变差**（加与不加辅助头两臂对照）。

### 2.4 ★ 与已有 AI CE 资产的关系（我们有现成的可复用件）

| 已有资产 | 在深度 3 里怎么用 |
|---|---|
| **HELENA 权重**（116 k 参数，输入 LS 线性插值网格 `(1,612,14,2)`，输出信道网格） | ★ **热身初始化 trunk**：它学的正是"从网格到信道"，与联合网络的**前半段同任务**。⚠ 但 **AI CE 的结论是负面的**（"经典 ≥ HELENA 于所有实测区间"）⇒ 只当**初始化**，**不当精度来源**，也不是最终 trunk 的架构 |
| **`ocudu_coreml_nn_engine.{h,mm}`** | 直接复用：链无关、**零拷贝**（`initWithDataPointer` + `outputBackings`）、专用 worker 线程（首次预测在新线程上 ~12 ms ANE 初始化）、**2 s ANE keep-alive** |
| **`ai_train/` 工具链** | 直接复用：`nr_ldpc.py`（BG1/BG2 编码）、`dd_label.py`（TB → 速率匹配/加扰/调制）、`build_labels.py`、`convert_coreml.py`、`train_pad.py` |
| **宽度分桶经验** | 直接沿用（≤624 子载波 → 52 模型；625–1272 → 106 模型；零填充到桶宽） |
| **ANE 实测锚点** | **116 k 参数 → p50 141 µs / p99 208 µs**（M4 Pro，(1,612,14,2)）——尺寸预算的实测起点 |
| **G 门禁范式** | 直接沿用（G1–G5 → 本规划的 G0–G6） |
| **采集与标签管线** | `ul_capture` 五件套 + `ul_chain_replay`（自动设 `OCUDU_UL_DUMP_LLR=1`）（`memo_04` §3） |

★ **一个必须说清的边界**：**AI CE 的负面结论不是对深度 3 的负面结论。**
它只证明"**单独**把 CE 换成 AI 打不过经典 MMSE"。联合网络的收益（若有）来自
**联合估计与判决**这一段——信道估计与解映射在经典链里被一个显式的中间量切开，
而这个中间量正是最优性损失所在。这恰好是 P0 要检验的命题。

### 2.5 消融/诊断臂（**不是**必经台阶）

| 臂 | 形态 | 用途 |
|---|---|---|
| **主臂** | ★ **单个联合网络 = 深度 3** | 默认路线 |
| 消融 1 | 去掉辅助头（只留 LLR 头） | 量化多任务的影响 |
| 消融 2 | 输入去掉导频掩码 | 证明"几何必须显式给" |
| 消融 3 | 用经典 CE 替换网络的估计部分（= 深度 2） | 定位"增益来自联合还是来自解映射" |
| 消融 4 | 用真实信道（genie）替换估计 | 给出该网络结构的上界 |

★ 消融 3/4 是**诊断**，不是"先做浅一层再加深"的路线图。

### 2.6 ★ 引擎选择：默认 ANE（v1.6 修正）

> ⚠ 本节 v1.2–v1.5 的写法是错的（"Metal/MPS 升为主路径"）。修正后的结论与依据：

| 引擎 | 定位 | 依据 |
|---|---|---|
| ★ **ANE** | **首选** | ① 仓库架构原则 *"AI 推理走 NPU"*（`apple_silicon_heterogeneous_gnb_plan.md:17`，
且该文件已把"**AI 化接收机（信道估计/检测）**"列为 NPU 的承接对象）；
② ★ **本机实测**：HELENA（116 k）在 **M4 Pro** 上 **ANE p50 141 µs / p99 208**，
而**同机 MPS/GPU 路径是 p50 690 µs**——**ANE 快 4.9×**（AI CE G1）；
③ AI CE 的引擎栈（零拷贝 CoreML、专用 worker 线程、2 s ANE keep-alive、宽度分桶）**已经在 ANE 上跑通** |
| Metal / MPS | **对照臂 / 回退** | 深度 3 若用卷积/ResNet 形态，MPS 未必像 HELENA 的 MHA 那样退化到 CPU ⇒ **用测量决定**，不预设 |
| CPU | 经典路径兜底 | 已在链上 |

**为什么 dispatch 地板不构成反对理由**：

1. ★ **粒度是每跳 1 次，不是每符号 14 次**。`submit_fused` 按符号被调用，但后端在**第一个符号**
   触发**整组**工作，靠均匀步长的目的视图写后续符号（`memo_01` §2.4）；到了深度 3，
   替换单元是 `merged_hop`——**一个 CB 覆盖整跳**（`memo_01` §1.6）。
2. 按实测跳率 **~444.7 跳/秒**，即使每次 150 µs，ANE 占空比 **≈6.3%**。
3. ★ **0.23 ms 是 M1 的数字，且是在极小模型上测的地板**；我们在 M4 Pro 上的**整次推理**就是 141 µs。
   代际曲线是 **M1 0.23 ms → M4 Pro ≤141 µs → M5 Pro ~70 µs**——用两代前的芯片给今天的决定定价是不对的。

**ANE 的真实约束（设计要处理的）**：

| # | 约束 | 设计后果 |
|---|---|---|
| 1 | ★ **自定义 Metal kernel 不能跑在 ANE 常驻图里**（custom layer 只能 CPU/GPU） | 模型必须可由 **CoreML 原生算子**表达；自定义 Metal 前/后处理必须是**独立阶段** |
| 2 | **fp16 的约束是量程不是尾数**（中间量 5e6 ≫ 65504 → 全 NaN） | **LLR 头显式 clamp/tanh**（与 §3.3 标度校准是同一件事的两面） |
| 3 | **工作集悬崖 2 MB（M1）/ 4.72 MB（M5）** | 按符号/子带分块；网格本身很小（273 PRB×14×4 complex-fp16 ≈ 0.12 MB） |
| 4 | **`EnumeratedShapes`（≤128）是 ANE 认可路径**；无界 `RangeDim` 被拒 | **宽度分桶**——AI CE 已在做（≤624→52 模型 / 625–1272→106 模型） |
| 5 | **没有 ANE-only 模式，也没有运行时 API 报告哪个单元跑了** | 驻留必须**用测量证明**：对比 ANE / MPS / CPU 三条路径的时延（AI CE 的做法） |

★ **尺寸上界（可引用）**：NVIDIA 合规实时 NRX 的**实时模型只有 1.4e5 权重**（2 次迭代，<0.7 dB 代价），
@132 PRB/2 UE 在 A100 上 1 ms ⇒ **能进 1 ms 时隙的模型在 10⁵ 量级，不是 10⁶**（与 HELENA 的 1.16e5 同量级）。
★ dApp 给 depth 1 = **100 µs**、depth 2/3 = **150 µs** 的预算。

## 3. 训练与损失

### 3.1 标签

| 路线 | 来源 | 用途 | 规模 |
|---|---|---|---|
| **1（主）** | CRC-OK TB → LDPC encode → rate match → scramble → modulate ⇒ **真实发送比特** | 主监督 | ≈ **17.7 万次接收**（134,181 + 42,853） |
| 2（辅） | 经典链 LLR（`_llr.bin` / `ul_chain_replay` 重生成） | 热身、消融 | 任意（可离线批量生成） |

细节与陷阱（解扰域、`n_rapid`、msgA 排除）见 `memo_04` §3。

★ **一条负面证据要先接受**（`memo_03` §4.3）：*Input-Correlated Supervision Noise Limits the
Benefits of OTA Training for Learned Receivers*（arXiv 2608.12918, 2026）指出**训练信号失配**会削弱
学习型接收机的收益。我们的路线 1 虽然标签是**真值**，但**输入分布来自 OTA**这一侧仍可能带来相关噪声。
⇒ P1 必须把 **"仿真训练 vs OTA 训练"作为受控变量**（两条都跑，同一测试集），
**不得默认 OTA 训练更好**。

### 3.2 损失（按优先级）

1. ★ **逐比特 BCE（Binary Cross-Entropy，二元交叉熵）——唯一主损失。**
   记法：对每个编码比特，模型给一个 logit `z`（实数，可正可负），真值比特 `b ∈ {0,1}`，
   损失 `L = −[b·ln σ(z) + (1−b)·ln(1−σ(z))]`，其中 `σ` 是 sigmoid；对 `z` 的梯度就是
   **`σ(z) − b`**（形式极简，这是它好训的原因之一）。
   ★ **关键恒等式**：**logit 本身就是 LLR**——`ln(P(b=0)/P(b=1)) = … = z`（符号随标签约定，
   见 `memo_01` §2 的"正 LLR = 比特 0"约定）⇒ **BCE 训练出来的 `z` 可以直接当 LLR 用**，
   不需要额外的概率→LLR 换算。这正是"输出 LLR"与"用 BCE 训练"是同一件事的原因。
   文献同做法：Sionna 神经接收机对每 RE 每比特的 logit 做 **log-base-2** 的 BCE（训练时**不带外码**）；
   NVIDIA 合规实时 NRX 的 `ReadoutLLRs` 对 **LDPC 编码后的真值比特**做 BCE
   （*"the code rate and coding scheme is transparent to the NRX"*）；CMDNet 用符号后验交叉熵。
   允许模型超过经典 demapper（无蒸馏天花板）。
2. ★ **MSE-on-参考-LLR 存在但属少数派**（NVIDIA Aerial `LLRNet`，"Machine LLRning"）。
   它需要"参考 LLR"作监督，**本质是蒸馏 ⇒ 会把教师的上限变成学生的上限**。
   ⇒ **LLR 回归 / KL 只作预训练热身与消融对照，不作为主候选**（`memo_03` §7bis.M）。
3. **辅助头损失（多任务）**：后均衡噪声方差的回归 + 信道估计的 NMSE，权重小。
   ★ 判据：**主头 coded BLER 不因加辅助头而变差**（§2.3 的加/不加两臂对照）。
4. **码字级 / 译码器感知**（★ 已按负面证据降级）：文献里**把译码器放进训练**的收益是
   mixed-to-weak——arXiv 2312.02601 用了可微 LDPC 译码器却报告 *"we empirically did not observe
   any gains by doing so"*；ETH 2026 的站点微调只买到 0.004 绝对 BLER。
   ⇒ ★ **区分两件事：译码器在环「评测」是必须的；译码器在环「训练」不是。**
   本工作流把后者列为**可选探索**，先做离线批处理版本，不做 RL。

### 3.3 标度校准（独立步骤，不是训练细节）

经典 demapper 的 LLR 与噪声方差成反比（`demod_BPSK_symbol`：`2√2·(Re+Im)/σ²`）。
模型的浮点输出**整体标度**必须与 `range_limit` 匹配，否则量化后要么大面积饱和、要么全部挤在 0 附近，
两种都会让 LDPC 直接崩掉。

- 做法：训练后单独拟合一个标度/温度参数，最大化量化后 LLR 与真值比特的互信息；
- **验收**：量化**前**与量化**后**的 CRC 差异 ≤ 预登记阈值。

★ **可抄的具体数字**（`memo_03` §7bis.L）：Sionna 的神经解映射器输出 **int16 LLR，用 `np.ldexp(llrs,8)`
（2⁸ 缩放）**，并**实测到相对 OAI 参考 2.42× 的标度失配**；其 `LDPC5GDecoder` 内部裁剪 **`llr_max = 20.0`**，
且指出 **min-sum 对 LLR 标度失配天然鲁棒**（⇒ 我们也可以在译码器侧买鲁棒性）。
★ 注意 **他们的 20.0 与我们的 `LLR_MAX = 120` 不是同一层的东西**（译码器内部裁剪 vs int8 量化上限）——
"LLR 动态范围取多大"是**必须自己测**的量，不能照抄。

★ **这不是我们的特殊困难，而是已被命名的成熟问题**（`memo_03` §4.2）：LLR 的 scaling + clipping
在文献里是标准做法，且有现成技术路线——学习型量化（*Learning Quantization in LDPC Decoders*,
arXiv 2208.05186）、自适应 LLR 裁剪（arXiv 1011.2113）、学习型标度因子（多篇专利）。
⇒ 规划直接采纳"scaling + clipping"作为基线做法，并把"学习型量化"列为可选增强。

### 3.4 ★ 量化与数值（v1.3 新增，可直接写进 G3 判据）

| 位宽 | 代价（文献实测） |
|---|---|
| **8-bit** | **基本免费**——与 FP32 差 **≤0.05 dB** |
| INT4 | ★ **损失 3.3–3.7 dB，并跌破 LS-LMMSE 基线** ⇒ 预计不可用 |
| FP4 (E2M1) | 可用 |

★ **fp16 的约束是"量程"不是"尾数"**：朴素 fp16 流水线曾因中间量达到 **5e6 ≫ 65504** 而**全 NaN**，
用 **1/N 块浮点缩放**才修好（arXiv 2605.28451）。
⇒ **LLR 头必须有显式 clamp / tanh，绝不能接近 65504**——这与 §3.3 的标度校准是同一件事的两面。
★ 并且 **没有任何人发表过 LLR 的"精度-性能"曲线**（`memo_03` §7bis.C）——
**这条曲线本身就是本工作流的一个可交付结果**。

### 3.5 数据划分（★ 曾吃过大亏的地方）

**必须按采集时段/会话划分，禁止随机划分。** 同一信道的相邻 slot 若同时出现在训练集与测试集，
会产生严重泄漏，指标会好看而部署会崩——这正是 AI CE "训练/部署错配"那一类错误的近亲。
划分脚本要**检查并打印**：训练/测试集的 slot 范围不重叠、`rnti` 不重叠、`n_prb`/`mod`/SNR 分布对照表。

### 3.6 增强

只能在**物理上成立**的维度上做增强：噪声重采样（人工加噪到目标 SNR）、相位/定时扰动、
功率缩放。**禁止**做会改变标签的增强（例如随机擦除数据 RE 而不更新标签）。

---

## 4. 阶段与门禁（沿用 AI CE 的 G 门禁范式：门禁是**预先商定**的判据，未记录测量不得前进）

| 阶段 | 内容 | 门禁 | 预登记判据 |
|---|---|---|---|
| **P0** | **值不值得做**：经典链的导频/SNR 扫描 + genie 上界 | **G0** | 给出"检测环节可改善空间"的**量化上界**（memo 04 §4 的 P0-a/b/c）。**若上界很小 ⇒ 工作流只保留 G-A（时延）目标，或终止** |
| **P1** | 离线数据管线：grid + TB → 训练集（划分、增强、统计、复现脚本） | G1 | 数据集可一键复现；泄漏检查通过；**经典链在同一测试集上的 SER/BLER 已记录**（否则后面没有可比基线） |
| **P2** | ★ **单个联合网络（深度 3）离线训练** | G2 | ★ **主指标 = 真实 LDPC 译码器之后的 coded BLER**（**互信息不能预测 BLER**，`memo_03` §7bis.F）；BER/MI 仅作诊断。标度校准后 CRC 不掉 |
| **P3** | 联合网络扩展：覆盖 16/64/256QAM + 多任务头 + 消融臂 | G3 | 同 G2（coded BLER），且**逐调制**分别达标；给出参数量/FLOPs 与 ★ **ANE 实测时延曲线**（对照臂：MPS）、**量化曲线**（8-bit 应基本免费；INT4 预计不可用） |
| **P4** | **接链**：实现 `channel_equalizer` 后端 + 谓词 + 回退 + 契约测试 + crossings 声明 | G4 | §1.1 六项单测全过；开关关闭时与经典 **bit-exact**；开关打开时 CRC ≥ 参考 |
| **P5** | 实时性：端到端时延 + **同步开销** | G5 | **采用 dApp 论文的 Class A 契约**（`memo_03` §2.1）：**驻留接收链、零拷贝设备张量、完成 ≤150 µs**。三个对标基线：① 现网 eqdem **667.4 µs**（mkf033 中位）；② dApp 的 neural-receiver→LLR **≤500 µs 槽占用**；③ NVIDIA GB10 接收机 kernels **82 µs P50 / 112 µs P99.9**（273 PRB 4 端口，我们体量的 ~21 倍，**不可直接套用**）。★ **必须包含 GPU→模型 的等待与 模型→LDPC 的可见性开销**，不得只报前向时间；**必须报 dispatch 次数** |
| **P6** | OTA 实测：真实采集上的 CRC/BLER A/B | G6 | 统计门通过（同批采集、同一译码器、同一 LDPC 配置），**且必须同时报逐槽回滚率**（§1.4） |

★ 门禁纪律（继承自 `metal_kernel_fusion`，都是付过学费的）：

1. **判据在飞之前写死**；事后改判据等于没有判据。
2. **计数器必须能证明开关真的生效**（"开关没生效 ≡ 没有效果"在本仓库出现过三次）。
3. **每个数字都要能追溯到一次 leg / 一个 commit**；引用别人的数字要标来源等级。
4. **禁止用两个测量相减做归因**（本仓库为此错过两次）。

---

## 5. 风险清单（每条给早期信号与退路）

| # | 风险 | 早期信号 | 退路 |
|---|---|---|---|
| **R1** | **精度收益不存在**（工作点在经典链已近最优的区间） | G0 的可改善空间上界很小 | 工作流收缩到 G-A（时延）；或终止并如实记录 |
| **R2** | 模型太大 / **每跳超过 1 次 dispatch**，跑不动 | P2 的宽度-层数-时延曲线超预算；或每跳 dispatch 次数 > 1 | 缩模型/砍层；**保证整跳一次前向**；按符号/子带分块（§2.6） |
| **R3** | **判决反馈的串行性**与实时冲突 | DF 轮数 × 单轮时延 > slot 预算 | 无 DF 的一轮并行检测（A2 本身就没有 DF） |
| **R4** | **LLR 标度/量化不匹配**：离线指标好、CRC 崩 | 量化前后 CRC 差异大 | §3.3 标度校准；逐调制重标定 `range_limit` |
| **R5** | **训练/部署错配**（AI CE 的老坑） | 仿真好、OTA 差 | **"把适配写进输入格式"**——DEFINED 已显式验证 Rayleigh(NLOS) 训练 → Rician(LOS) 测试的错配场景（`memo_02` §3.1）；辅以 OTA 微调 |
| **R6** | **打破融合 lane 的"一次提交"结构** | crossings 计数上升、`gap` 变大 | 显式声明新同步点；或改走 dApp 路线 D——但那时要把 **1.47/4.4 MB 的每槽搬运**计入 G5（§1.4） |
| **R7** | 语料不足（17.7 万次接收 vs 0.4 M 参数量级模型） | 训练/验证曲线分离 | 仿真为主 + OTA 微调；先训小模型 |
| **R8** | 与 CUDA/NVIDIA 路线相比无优势 | 竞品分析 | 如实记录（仓库已有 `metal_vs_cuda_architecture.md` 的方法论） |
| **R9** | ★ **模型在某些槽上"崩"**，而整体指标看不出来 | 逐槽 CRC 方差大、退化槽集中在某些信道实现 | **逐槽信任/回滚**：前作实测**平均 60.5% 回滚率**（`memo_03` §4.4）⇒ 这是运维必需品。★ 内联路线在此有天然优势：**经典路径始终在链上，回退 = 让谓词返回 false**，不需要额外机制 |

★ R6 值得单列一段：当前融合 lane 的核心性质是**一次提交 + 设备排队**（`metal_kernel_fusion` §2.33）。
把 eqdem 交给学习型后端（无论落在 Metal 还是 ANE）都会引入 **GPU → 模型** 与 **模型 → LDPC** 两个新的同步点，
且模型侧**每多一次 dispatch 就多付一次地板开销**（v1.2 的核心教训，§2.3）。
**这两个等待必须被测量并计入 G5**，否则就是"把 667 µs 的空白换成了另一个没被测量的空白"。

---

## 6. 交付物

| 类别 | 内容 |
|---|---|
| 代码 | AI detector 后端（`channel_equalizer` 实现）+ 谓词 + 回退 + 契约单测；训练工具对 `ai_train/` 的扩展 |
| 数据 | 语料索引与标签生成脚本（数据本身不落库，遵守 `doc_chinese/.gitignore` 纪律） |
| 文档 | 本目录 memo 01–04 + 本规划 + 每阶段的测量记录（沿用 leg 协议与日志目录约定） |
| 门禁证据 | G0–G6 的测量记录，每个数字可追溯到一次运行 |

## 7. 与 `metal_kernel_fusion` 的关系

| | 复用 | 差异 |
|---|---|---|
| 交叉计数契约 | `phy_pipeline_crossings` / `phy_pipeline_grid_ready` 的纪律直接用 | AI 路径**必须新增声明**（GPU↔模型）；若走 dApp 路线则**主动增加**每槽 1.47/4.4 MB 搬运（§1.4） |
| 验收方式 | 离线重放、leg 协议、Linux 复验流程 | **门禁从"0 差异"变成"统计不劣"**（AI 不可能 bit-exact） |
| 问题陈述 | eqdem 667 µs、一次提交 + 设备排队 | 本工作流是它的**续集**：不重写链路结构，把这段计算换成学习型的一次前向 |

★ 一句话交接：**上一个工作流证明了"在这条链上，时间不是被算力吃掉的"；
本工作流要证明的是"把这段计算换一种做法（学习型、一次前向替掉均衡+解映射），能否把时间拿回来"。**

★ 而完整版调研给出的两个独立佐证，让这个交接更锋利（`memo_03` §1）：
**Apple GPU 的 DSP 是带宽瓶颈而非算力瓶颈**（*Bandwidth, Not FLOPS*, arXiv 2609.32237），
**ANE 的单次 dispatch 地板本身就是几十到几百微秒**（arXiv 2606.22283 / 2606.17090）。
⇒ 如果搬到新引擎只是把 667 µs 的空白换成另一个空白，结论应当如实记录为——
**瓶颈不在计算引擎，而在链路的时序结构**（slot pacing / 设备排队 / dispatch 粒度）。
这本身仍是有价值的结论，且与本仓库已有的判断一致。

## 8. 本规划中尚未确认的部分（诚实清单）

1. **G-B（精度收益）是否存在** —— 由 P0 裁决，规划不预设结论。
2. A2 的具体网络形态与参数量 —— 待 P0 与 `memo_03` 的 LINE B 结论共同确定（**倾向卷积而非注意力**：
   工作集悬崖与 O(N²) 都指向同一个方向）。
3. **深度 3 在 ANE 上的真实时延曲线** —— 现有锚点是 HELENA（CE 任务、116 k 参数），
   深度 3 更大且输出更宽；**必须实测**，不可外推。MPS 对照臂一并测。
4. 17.7 万次接收是否足够 —— 待 P1 的学习曲线；并需同时给出"仿真训练"臂作对照（§3.1 的负面证据）。
5. 导频密度的精确值 —— `dmrs_type` / `nof_cdm_groups_without_data` 未被采集记录（memo 04 §5）。
6. **信任/回滚判据的具体形式** —— §1.4 定了必须做，但"用哪个廉价指标"要在 P2/P3 用数据选。
7. **ANE 在我们自己的 M4 Pro 上的 dispatch 地板** —— 论文的 M1/M5 Pro 数字已直读原文，
   但**带条件（固定形状、预热）**，必须自测。
8. ★ **ANE 能否关掉"72 ms → 1 ms"这个差距** —— 这是本工作流最核心的未知，
   也是文献里**没有人回答过**的问题（`memo_03` §7bis.A）。
9. **量化曲线与 LLR-vs-精度曲线**（文献空白，见 §3.4）—— 待 P2/P3 产出。
