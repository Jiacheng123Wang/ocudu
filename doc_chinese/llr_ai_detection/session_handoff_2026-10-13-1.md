# Session handoff —— `llr_ai_detection`（2026-10-13，第 1 次）

> **用法**（沿用约定 ✓）：新会话**只读这一份**就能接着开工 ✓。
> 结构 = ① 本会话做了什么 ② **当前状态** ③ ★ **下一个会话的第一件事**（具体命令 ✓）
> ④ ★★★ **R1–R6 详细执行方案** ⑤ 未决问题与重开条件 ⑥ 纪律 ⑦ 常用命令。
>
> **上位文档**：`llr_ai_detection_high_level_status_and_plan.md`、
> `llr_ai_detection_design_and_implementation.md`（★ **§0 不变式 I1–I10**、★ **§1.0 深度表**、
> ★ **§1.5 AI 路径进链**、★ **§3.2 损失**、★ ★★★ **§12 本会话新增：重大偏差与修正规划**）、
> `model_network_design.md`（★ **§0 网络设计的唯一入口**）。
>
> ★★ **本会话全部提交**：**27 个**，★ 范围 **`5517faebf8` … `bc6ee8e01a`**
> （★ 命令：`git log --oneline 3fd5674fb3..bc6ee8e01a`）。★★ **工作树干净**。
> ★ 注：`3fd5674fb3` 是上一个会话的最后一个提交，★ 它是**基线**，不属于本会话。

---

## ★★★ 先写在最前面的两件事

### ★★★ 一、本会话最重要的产出：**发现并记录了重大架构偏差**

★★★ **用户裁定**（原文）：
> *"深度-3 网络必须**包含 CE 作为它的一部分功能**，而**不是把现有的 CE 模块（MMSE 或者 AI CE 等）的输出
> 拿来直接用**。从字面意思讲，深度-3 网络就是一个**较大的网络，包含 CE，EQ，DEMOD** 等 3 部分的主要功能。"*

★★★ **用户是对的，文档里早有明文**（`README.md` 首句、`memo_01` §1.4 深度表、
`design` §1.0、`model_network_design` §0.1 四处一致）：

> **一个网络，一次前向，`网格 → LLR`**；输入是 **DFT 后的时频网格及其几何**，
> 输出是**解扰前软比特（加扰域 LLR）**，交给**经典 LDPC**。

★★★ **本会话实作的是【深度 1 的变体】**：它消费链已算好的信道估计、精修它、再交回**经典均衡器与解映射器**。
★★ 这**违反不变式 I4**（`design` §0 逐字：*"一个网络做深度 3，不是 CE 网络 + EQ/DEM 网络 ——
中间的信道估计会变成有损瓶颈并断掉端到端梯度"*）。

★★★ **⇒ 新会话的第一件事不是接着接线，而是执行 §④ 的 R1–R6 修正规划。**
★★ **详见 `llr_ai_detection_design_and_implementation.md` §12**（偏差表、根因、修正规划）。

### ★★ 二、偏差的根因（★ 记住它，避免重犯）

★★ **我把"臂的边界"当成了"网络的边界"**：

| ★ | ★★ 正确 | ★★ 我做成的 |
|---|---|---|
| ★★ 臂（`pusch_depth3_receiver`）| ★ 替换**整个单元**（CE+EQ+DEMOD）| ★ **拥有**整个单元 ✅ |
| ★★ 网络 | ★ 也从**网格**到 **LLR** | ★ ❌ 只替换了**单元内的 CE 输出**，下游仍走经典 |

★★ 诱因：★★ **训练目标（"链的估计 → 真值信道"）是我自己定的，没有核对 §2.2.1 已经写明的输入契约**
（工业参考 NVIDIA `neural_rx.onnx`：输入 = 网格 I/Q + **导频 LS** + DM-RS 符号/子载波位置）。

★★★ **纪律（本会话新增，与 F7–F13 同级）**：
> ★★ **F14：实现网络之前，先逐字核对【输入/输出契约】那一节。**
> ★ 训练数据的构造**不等于**契约的确认；★ "能训起来、指标在改善"**也不等于**做的是那件事。

---

## ① 本会话（2026-10-13）做了什么

**一句话**：★★ **打通了合成数据闭环并把 DM-RS/TB/度量全部做成可信工具；训练出一个 +1.93 dB 的精修器；
把它接进 PHY pipeline 并由 `--expert_phy.pusch_receiver_backend` 驱动（`ai` 臂从 identity 变成真计算）；
★ 最后经用户指出，发现整条路线做的是【深度 1 变体】而非深度 3，并写下修正规划。**

### A. ★★★ 训练数据闭环（★ 全部可复用）

| ★ 项 | ★★ 内容 |
|---|---|
| ★★★ **DM-RS 生成器** | ★ `ai_train/dmrs.py`：★★ **逐点复现链自己的 dump**（6 个序列，误差 `1.7e-08`）。★ 四个约定由 dump 判定（★ 见 §35）|
| ★★★ **TB dump** | ★ 3941 单元 → **1153 CRC-OK → 1144 个 TB**（★ `work_tmp/data/tb_corpus/`）|
| ★★★ **合成核心** | ★ `ai_train/synth.py`（★ `encode_symbols`/`build_clean`），★ `build_synth_dataset.py`（★ 真值已知的监督对）|
| ★★★ **数据集** | ★ 覆盖 **12–48 dB**：★ `train_full.npz`(945) / `val_full.npz`(756)；★ 另有两语料各自的数据集 |

### B. ★★★ 精修器（★ 深度 1 变体，★ 保留为消融臂）

| ★ | ★★ 数字 |
|---|---|
| ★★ 训练 | ★ 945 训练 / 756 验证（**不同采集会话**），★ 18 blocks，★ 150 epoch，★ cosine |
| ★★★ **整体改善** | ★★ **+1.70 dB**（★ 功率聚合 **+2.24**），★ **78.3% 样本改善** |
| ★★ 分档 | ★ 12 dB **+3.37** / 20 dB **+1.86** / 48 dB **+0.81**（★ **全档位正收益**）|
| ★★ 容量拐点 | ★★ **9 blocks 拿到 18 blocks 的 94%** |
| ★★ 产物 | ★ `work_tmp/data/final/refiner_612.{keras,mlmodelc}`（★ 260 KB）|

### C. ★★★ 部署侧实测（★ 三条都对规划有影响）

| ★ 结论 | ★★ 数字 |
|---|---|
| ★★★ **域内 CPU 很快** | ★ `pusch_processor_benchmark -c cpu` ⇒ **107.8 / 122.5 / 120.5 µs**（★ 三次）★ vs `-c metal_mmse` **1696.8 µs** ⇒ ★★ **这个规模下 Metal 慢 11–14 倍** |
| ★★ **CoreML 推理** | ★ **1724 µs**（★ 612 宽，★ 200 次 p50），★ 放置 `gpu=113`（★ 残差连接使它上不了 ANE，§38.4）|
| ★★★ **CPU LDPC 比 GPU 快 20–100 倍** | ★ p50 **4.8–29.3 µs**（★ CPU）★ vs **641–974 µs**（★ metal）★ ⇒ LDPC 应跑 CPU |
| ★★ **零拷贝可破** | ★★ 用户裁定：★ **深度-3 是中间态，最终 CP/FFT 与 LDPC 进网络后自然零拷贝** ⇒ 当前破坏它可接受 |

### D. ★★★ 接缝与控制变量（★ 这部分**不需要重做**）

★★ 控制变量 **`--expert_phy.pusch_receiver_backend = auto|classic|ai`** 与 §1.5 **完全一致**，且：

| ★ 组合 | ★★ 实测结果 |
|---|---|
| ★ `classic` / `auto` | ★★ **判据 0 回归门通过**（★ `crc=OK sinr=24.18`，★ 与既有基线逐位相同）|
| ★★ `ai` + 有效模型 | ★★ **AI ARM ACTIVE**，★ refined 552 子载波，★ **sinr −3.16 → −0.56**，★ `crc=OK` |
| ★ `ai` + 无模型/坏路径 | ★★ **AI ARM INACTIVE**（★ 明确写"must NOT be reported as an AI result"）|
| ★★ `cpu`+`ai`、`cpu_gpu`+`ai` | ★★ **报错退出**（★ 冲突矩阵，★ 单测 25/25）|

★★ **激活通路**：★ CLI → validator → `resolve_phy_pipeline` → upper PHY factory → PUSCH factory
★ → `pusch_depth3_receiver`（**臂决定**）★ → `port_channel_estimator`（**估计器细化**）。
★★ **环境变量已从决策中移除**（★ 只剩一个 verbose 诊断开关）。

---

## ② 当前状态

### ★★★ 一句话

★★ **数据与工具链已可信；接缝与控制变量已就位；但网络的【契约】是错的那一个（深度 1 而非深度 3）
⇒ 需要按 §④ 重做契约、损失、数据集与网络头，接缝本身改动不大。**

### ★★ 已就绪（★ 可直接复用）

| ★ | ★★ 位置 |
|---|---|
| ★★★ 合成数据核心 | ★ `ai_train/synth.py`、`build_synth_dataset.py` |
| ★★★ DM-RS / TB / 度量工具 | ★ `ai_train/dmrs.py`、`measure_ce_error.py`、`measure_oracle_gain.py`、`eval_refiner.py`、`work_tmp/dump_tb.sh` |
| ★★★ 数据 | ★ `work_tmp/data/{tb_corpus,datasets,final}/` |
| ★★ 网络拓扑骨架 | ★ `ai_train/train_refiner.py` 的 `build()`（★ depthwise 3×3 + 1×1、★ dilation 1/2/4/8、★ 通道 64）|
| ★★★ 接缝与控制变量 | ★ `pusch_depth3_receiver*`、★ `--expert_phy.pusch_receiver_backend`、★ 冲突矩阵、★ 单测 |
| ★★ 报告义务清单 | ★ `memo_07` §7 的 **R1–R5** |

### ★★★ 必须重做（★ 见 §④）

| ★ | ★★ 缺什么 |
|---|---|
| ★★★ **I/O 契约** | ★★ 输入应是 `[1,612,1,64]`（网格+导频 LS+几何+元数据，★ 符号轴**折叠进通道**）；★★ 输出应是 **LLR**（主头）+ **后均衡噪声**（★ §2.3 逐字"**不是可选**"）+ 信道估计与噪声 |
| ★★★ **训练目标** | ★★ 主损失应为 **逐比特 BCE on 编码比特**（★ §3.2）；★ 现用的是"信道估计的 MSE" |
| ★★ **数据集** | ★ 需增补：★ **真值编码比特**、★ 原始网格、★ 导频 LS、★ DM-RS 掩码、★ 元数据 |
| ★★ **接缝** | ★ `ai` 臂的 `demodulate()` **必须停止转发经典解调器** |
| ★★ **标度校准** | ★ §3.3：★ 训练后单独拟合标度/温度，★ 验收"量化前后 CRC 差异 ≤ 阈值" |

### ★★ 过时/待更正的地方（★ 新会话请注意）

| ★ | ★★ 内容 |
|---|---|
| ★★★ **§41 的"LDPC 占 87%"** | ★★ **已作废**（★ 用户纠正：★ LDPC **不在深度-3 之内**）。★ §41.6 已记更正，★ 但 §41.1–41.5 仍留在文中 |
| ★★★ **§43–44 的接线** | ★★ 描述的是**深度 1 变体**的接线。★ 源码注释已改正（`port_channel_estimator_metal_mmse_impl.{h,cpp}` 写着 "DEPTH-1 VARIANT, NOT DEPTH 3"），★ **但文档 §43/§44 仍在** |
| ★★ **`model_network_design` §38.4** | ★★ "残差连接把网络踢出 ANE"—— ★ 对**当前残差结构**成立；★ 深度 3 换头后**需要重测** |

---

## ③ ★★ 下一个会话的第一件事（具体命令 ✓）

★★★ **不要接着接线。** 先做 **§④ 的 R1（冻结 I/O 契约）** —— 它是后面全部工作的前提。

```bash
cd /Users/jiachengwang/dev/ocudu

# ① 先读这三节（★ 契约的权威）
#    llr_ai_detection_design_and_implementation.md  §1.0（深度表）、§2.2（输入表示）、§2.2.1（工业参考张量规格）、§2.3（输出头）
#    model_network_design.md                        §0.1/§0.2（形态与张量形状）、§0.3（拓扑）
#    memo_01_repo_seams_and_llr_contract.md         §1.4（两种深度）、§1.5（上报义务）

# ② 确认接缝提供的输入确实齐全（★ 已核对，此处复核）
grep -n "struct configuration" -A 30 \
  include/ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h | head -30

# ③ 把 R1 的契约写进 model_network_design.md 的 §0.2（★ 那一节改设计必须先改）
#    输入通道的【具体布局】必须定死：网格 I/Q、导频 LS、DM-RS 掩码、data-RE 掩码、元数据各占哪几个通道
```

★★ **R1 的产出**：★ `model_network_design.md` §0.2 的**完整张量表**（★ 每个输入通道是什么、
★ 主头/辅助头的形状与语义、★ 量化约定）。★ **在它写清楚之前不要写训练代码。**

---

## ④ ★★★ R1–R6 详细执行方案

> ★★★ **排序理由**：★ R1 是**契约**（后面全部依赖它）；★ R2 是**目标**；★ R3 是**数据**；
> ★ R4 是**训练**；★ R5 是**接缝**；★ R6 是**上报**。
> ★★ **R1–R4 可在离线完成**（★ 不需要进链），★ **R5–R6 才动链** ⇒ ★★ **风险集中在 R5，把它放最后。**

### ★★★ R1. 冻结 I/O 契约（★ 先做，★ 预计半天）

| ★ 步骤 | ★★ 内容 | ★★ 产出 |
|---|---|---|
| ★ **R1.1** | ★★ 读 §2.2 + §2.2.1，**逐字列出**权威输入清单：★ 网格 I/Q、★ **导频 LS**、★ DM-RS 符号位置、★ DM-RS 子载波位置、★ 元数据（★ allocation shape / `nof_tx_layers` / `rx_ports` / `modulation` / `nof_cdm_groups_without_data` / `dc_position` / `scaling`）| ★ 清单 |
| ★★ **R1.2** | ★★ **定死通道预算**：★ §0.2 说 `[1,612,1,64]`，★ 而输入合计 `C_in×14 = 42 → pad 64`。★★ **必须明确 42 个通道怎么分**（网格 2×14=28？导频 LS？掩码？元数据广播？）| ★★ 通道分配表 |
| ★★ **R1.3** | ★★ 定死**输出**：★ 主头 LLR（★ `C_out` 容纳 256QAM 的 8 bit/RE ⇒ 每 RE 8 个 logit 怎么排在通道维）、★ **辅助头 1 后均衡噪声**（★ 形状、`int8`/`fp16`、★ **不得输出近零常数**——`memo_07` C2 的 `+inf` 分支）、★ 辅助头 2 信道估计与噪声 | ★★ 头部表 |
| ★★ **R1.4** | ★★ 定死**量化与标度**（★ §3.3）：★ 模型输出 tanh 有界 + **模型外乘尺度并 clamp 到 int8**；★ `LLR_MAX=120`、**不得用 −128**（★ I3）| ★ 量化约定 |
| ★★★ **R1.5** | ★★★ **写进 `model_network_design.md` §0.2**（★ 规则：改设计必须同时改本节），★ 并在 `design` §12.4 的 R1 行标记完成 | ★★ 文档 |

★★★ **R1 的验收**：★ **一份能被第三方照着写出训练代码的张量表**，★ 且它与 §2.2.1 的工业参考**逐项对得上**。

### ★★★ R2. 重做训练目标（★ 预计半天）

| ★ 步骤 | ★★ 内容 |
|---|---|
| ★★ **R2.1** | ★★ 主损失 = **逐比特 BCE on 【编码比特】**（★ §3.2 逐字）。★ **关键恒等式**：★ **logit 本身就是 LLR** ⇒ BCE 训出的 `z` 直接当 LLR 用，★ 不需概率→LLR 换算 |
| ★★ **R2.2** | ★★ **不得**用 MSE-on-参考-LLR 作主损失（★ §3.2 第 2 条：★ 那是蒸馏，★ 会把教师上限变成学生上限）|
| ★★ **R2.3** | ★★ 辅助损失：★ 后均衡噪声回归 + 信道估计 NMSE，★ **权重小但必须存在** |
| ★★★ **R2.4** | ★★★ **判据**：★ **主头 coded BLER 不因加辅助头而变差**（★ §2.3 逐字要求加/不加两臂对照）|
| ★ **R2.5** | ★ 损失实现后**先做单位检查**：★ 用"完美预测"喂进去，★ 损失应≈0；★ 用"随机 logit"应≈`ln2` |

### ★★★ R3. 扩展数据集（★ 预计 1 天）

★★★ **现有合成管线已经知道全部所需信息**（★ 网格、导频、真值信道在构造时都已知）
★ ⇒ ★★ **是"增补字段"，不是"重建"**。

| ★ 步骤 | ★★ 内容 |
|---|---|
| ★★ **R3.1** | ★★ **真值编码比特**：★ `synth.encode_symbols()` 已产出**调制符号**（★ 经 `scramble`+`modulate`），★ 需**在调制前**把**加扰后的编码比特**一并保留 ⇒ ★★ 这就是 BCE 的目标（★ 注意 **I2：加扰域**）|
| ★★ **R3.2** | ★★ **原始网格**：★ `build_clean()` 已产出干净网格 ⇒ 加噪后就是网络输入（★ 现在只存了信道估计）|
| ★★ **R3.3** | ★★ **导频 LS**：★ `build_synth_dataset.pilot_observations()` **已实现过**（★ `P`/`P_mask` 字段曾在 `aillr_cap001_pilot.npz` 里）★ ⇒ 复用 |
| ★★ **R3.4** | ★★ **DM-RS 掩码 + data-RE 掩码 + 元数据标量**（★ 从 `cfg` 直接可得）|
| ★★ **R3.5** | ★★ 重跑两个语料（★ 命令模板见 §⑦），★ 覆盖 **12–48 dB** |
| ★★★ **R3.6** | ★★★ **自检**：★ 用 `eval_refiner.py` 的 sanity 思路，★ **验证存下来的网格 + 导频 LS 能重建出与链一致的估计**（★ 否则数据集是错的）|

### ★★★ R4. 重训网络（★ 预计 1–2 天）

| ★ 步骤 | ★★ 内容 |
|---|---|
| ★★ **R4.1** | ★★ **复用拓扑骨架**：★ `train_refiner.py::build()` 的 depthwise 3×3 + 1×1 pointwise + dilation 1/2/4/8 + 通道 64（★ 但输入通道从 2 变成 64，★ 头部完全重做）|
| ★★★ **R4.2** | ★★★ **已知教训必须带上**：★ **残差块 + LayerScale**（★ 否则激活塌缩、梯度全零，★ §38.3）、★ **输入按训练尺度归一化**（★ §42，★ HELENA 记录过不做会 attach 失败）|
| ★★ **R4.3** | ★★ **先小样验证**（★ 用户建议的顺序）：★ 用 **1/10 数据**跑通"网格→LLR"，★ 看 **coded BLER** 是否随训练下降 ⇒ ★★ **确认能训起来再投全量** |
| ★★ **R4.4** | ★★ 全量训练，★ 判据见 §2.3 与 R2.4 |
| ★★★ **R4.5** | ★★★ **标度校准**（★ §3.3，★ **独立步骤**）：★ 训练后拟合标度/温度，★ **验收：量化前后 CRC 差异 ≤ 阈值** |
| ★★ **R4.6** | ★★ **与精修器对照**（★ 消融）：★ 回答"联合网络比精修经典估计多拿了多少"（★ §2.5 定义的对照）|

### ★★★ R5. 改接缝（★ 风险最高，★ 放最后；★ 预计 1–2 天）

| ★ 步骤 | ★★ 内容 |
|---|---|
| ★★★ **R5.1** | ★★★ **`ai` 臂的 `demodulate()` 停止转发经典解调器**（★ `pusch_depth3_receiver_impl.h` 的 `pusch_depth3_receiver_ai_identity::demodulate`）|
| ★★ **R5.2** | ★★ **网络在 `estimate()` 侧拿到网格 + 几何**（★ 那里有 `resource_grid_reader& grid` + `dmrs_pusch_estimator::configuration`）；★ `demodulate()` 侧补 `pusch_demodulator::configuration`（★ `rb_mask`/`modulation`/`dmrs_symb_pos`/…）|
| ★★★ **R5.3** | ★★★ **一次前向出 LLR** ⇒ ★★ **均衡与解映射不再运行**（★ 这是深度 3 的定义）|
| ★★ **R5.4** | ★★ **`computes_itself()` 的语义要更新**：★ 现在它答"估计器是否绑定了细化"，★ 应改为"网络是否真的产出了 LLR" |
| ★★ **R5.5** | ★★ **零拷贝的账要重算**：★ 深度 3 的输入是**网格**（设备侧）⇒ ★★ **一次 device→host 拷贝**，★ 与设备路径的零拷贝冲突。★ **必须实测并如实报告**（★ 用户已接受代价，★ 但要数字）|
| ★★★ **R5.6** | ★★★ **判据**：★ **I1（经典路径 bit-exact 回退）**、★ **I7（降级可查询、可计数）**、★ **判据 1（CRC 不劣化）**、★ **判据 2（每跳 dispatch 数不增加）**（★ §1.5.7）|

### ★★ R6. 上报义务（★ 与 R4/R5 交织；★ 预计 1 天）

| ★ 步骤 | ★★ 内容 |
|---|---|
| ★★★ **R6.1** | ★★★ **后均衡噪声方差**（★ `memo_07` C2：★ **唯一**的上行链路自适应输入，★ `SINR=-10log10(mean σ²)`）⇒ ★★ **必须从辅助头 1 输出**，★ 且**不得输出近零常数**（★ 会算出 `+inf`，★ 代码里有真实分支）|
| ★★ **R6.2** | ★★ **信道估计 + 噪声** ⇒ 供**经典测量核**出 RSRP/EPRE/SNR（★ `memo_07` R1/R5）|
| ★★★ **R6.3** | ★★★ **TA 是闭环控制量**（★ `memo_07` C1）：★ CSI → FAPI → 调度器 → **TA_CMD MAC CE 下发给 UE** ⇒ ★★ **深度 3 必须继续产出可用 TA** |
| ★★ **R6.4** | ★★ **判据 3**：★ R1–R5 全部有值且有限；★ F1–F6 指纹先跑（★ `memo_08` §9）|

---

## ⑤ 未决问题与重开条件

| ★ | ★★ 问题 | ★★ 重开条件 |
|---|---|---|
| ★★ | ★★ **24 dB 以上无实测数据** | ★ 已用合成数据补（★ §42），★ 但**真实高 SNR 场景是否出现**未验证 |
| ★★ | ★★ **残差结构上不了 ANE**（★ §38.4）| ★ 深度 3 换头后**必须重测**；★ 若仍上不了，★ 需重新设计块结构 |
| ★★ | ★★ **`_h.bin` 与导频 LS 的一致性**（★ 残差 ~1.0，★ 未解释）| ★ 深挖前先确认它**是否影响深度 3**（★ 新契约用导频 LS 作输入，★ 可能自动绕过）|
| ★★ | ★★ **CPU LDPC 的 BLER 缺陷**（★ `PLAN.md` §4.8 已登记：★ 10 dB 下 187-188/200）| ★ 单独排查；★ 本工作流**不阻塞**（LDPC 不在深度 3 内）|
| ★★ | ★★ **§41 的"LDPC 占 87%"** | ★ 已作废；★ 若将来 LDPC 进网络（★ 用户路线图），★ 该分析要重做 |

---

## ⑥ 纪律（★ 本工作流累积，★ 全部有踩坑出处）

| # | ★ 纪律 | ★★ 出处 |
|---|---|---|
| ★★ **F7** | ★★ 比值型判据，先写出**分子分母的单位** | `memo_08` §17.4 |
| ★★ **F8** | ★★ 扫描型实验，先在**不干预档**复现基线 | `memo_08` §18 |
| ★★ **F9** | ★★ 两个量的逐点关系，先证它们**来自同一次事件** | `memo_08` §19.2 |
| ★★ **F10** | ★★ 判决错误率用**残差/噪声比值**，不用绝对阈值 | `memo_08` §21.16 |
| ★★ **F11** | ★★ 清点文件按 **stem 归并**，不用 `ls <glob> \| wc -l` | `memo_08` §21.7bis |
| ★★ **F12** | ★★ 要写进记录的数字，**前面必须有一次全量构建** | `memo_10` §7.1 |
| ★★★ **F13** | ★★★ **看起来像尺度问题，先怀疑自己的度量** | `model_network_design` §37.3/§38.6 |
| ★★★ **F14（本会话新增）** | ★★★ **实现网络之前，逐字核对【输入/输出契约】那一节** | `design` §12.2 |

★ **另有两条本工作流的硬纪律**：

- ★★★ **一个参数只有在多个独立通道上同时成立才算对上**（★ `memo_08` §14.2ter）
- ★★★ **开关没生效 ≡ 没有效果**（★ I10）；★ 引用任何开关前**先证明它动了**

---

## ⑦ 常用命令

```bash
# ★ 构建（★ 要写进记录的数字之前）
cd /Users/jiachengwang/dev/ocudu
cmake --build build --target gnb ul_chain_replay -j8

# ★ ML 环境（★ 系统 python 没有 TF/Keras）
~/ai_ce_work/venv/bin/python

# ★★ 合成数据集（★ R3 会改这个工具）
W=doc_chinese/llr_ai_detection/work_tmp
~/ai_ce_work/venv/bin/python lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/build_synth_dataset.py \
  --units $W/data/tb_corpus/index.csv --tb $W/data/tb_corpus/tb \
  --corpus $W/corpus/aillr_cap001 --out $W/data/datasets --name <名字> \
  --snrs 12,16,20,24,30,36,42,48 --truth hbin

# ★★ 数据集自检（★ 唯一权威度量）
~/ai_ce_work/venv/bin/python .../ai_train/build_synth_dataset.py --selfcheck --out <npz>

# ★★ 训练 / 评测（★ R4 会改这两个）
~/ai_ce_work/venv/bin/python .../ai_train/train_refiner.py --train <train.npz> --val <val.npz> \
  --out <dir> --epochs 150 --warmup 5 --patience 30 --layout nhwc
~/ai_ce_work/venv/bin/python .../ai_train/eval_refiner.py --model <best.keras> --data <val.npz> --layout nhwc

# ★★ CoreML 转换（★ 必须先 compile，★ 引擎要 .mlmodelc 不是 .mlpackage）
~/ai_ce_work/venv/bin/python /tmp/convert_refiner.py          # 见 §①D 的脚本模板
xcrun coremlcompiler compile <model>.mlpackage <dir>

# ★★ 接缝测试（★ 不设任何环境变量即为默认关闭）
OCUDU_REPLAY_RECEIVER=ai OCUDU_REPLAY_MODEL=<model.mlmodelc> OCUDU_CE_CPU_CE=1 \
  ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
  $W/corpus/aillr_cap002/C_1147_17921 --metal --out /tmp/x

# ★★ 冲突矩阵单测
./build/tests/unittests/apps/units/flexible_du/o_du_low/du_low_phy_pipeline_test

# ★★ 域内延迟基准（★ 深度-3 的 CPU 与 metal 对比）
./build/tests/benchmarks/phy/upper/channel_processors/pusch/pusch_processor_benchmark \
  -m latency -P scs30_100MHz_256qam_rv0_4port_nlayer -R 30 -c cpu
#   ↑ 换成 -c metal_mmse 就是 metal 版（★ 实测 107-125 µs vs 1697 µs）

# ★★ LDPC 的 CPU/GPU 延迟与 BLER 对比（★ 结论：LDPC 应跑 CPU）
./build/lib/phy/upper/channel_coding/ldpc/metal/ldpc_metal_bler_test
#   ↑ 实测 CPU p50 4.8-29.3 µs  vs  metal p50 641-974 µs
```

★★ **提交纪律**：★ `git commit -F -` 用 heredoc（★ `-m` 带引号会踩 pathspec 错）。

---

## ⑧ ★★ 本会话的提交（★ 关键节点）

★★ 完整 **27 个**：`git log --oneline 3fd5674fb3..bc6ee8e01a`。★ **关键节点**：

| ★ commit | ★★ 内容 |
|---|---|
| `5517faebf8` | ★★ DM-RS：dump 出 PRG 起点，★ 加 dump 验证过的生成器 |
| `61c0060d88` | ★★ 真值已知的监督数据集工具 |
| `239351240a` | ★★ 训练精修器 + 修四个说谎的度量 |
| `436e6e1655` | ★★ 修增益拟合（`np.vdot` 共轭方向），★ 让度量自证 |
| `4e0a04cf98` | ★★ +1.93 dB + 三个否定性对照实验 |
| `39549575a1` | ★★ 24 dB 的"差距"是地板算错 |
| `283e53efcd` / `cabe927bbc` | ★★ 部署前提错了：★ LDPC 不在深度-3 内 |
| `fe0ebb37a8` | ★★ refiner 接进接收链（★ 默认关）|
| `ab0dde0fb1` | ★★ `ai` 臂由控制变量驱动，★ 从 identity 变成真计算 |
| **`ccab797563`** | ★★★ **偏差与修正规划（★ 新会话从这里开始读）** |
| `bc6ee8e01a` | ★★ 源码注明"这是深度 1 变体" |
