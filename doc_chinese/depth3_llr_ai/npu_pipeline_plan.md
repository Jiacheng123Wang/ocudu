# `npu` PHY pipeline 规划（**v2**，2026-11-05 20:45 重写）

> **v2 取代 v1**。v1 写于同日 20:08，当时的前提是"要写一份估计器拷贝、要改契约探针、约 1–2 天工作量"。
> **两个实测把前提推翻了**：
> 1. `--phy_pipeline cpu --pusch_receiver_backend ai` **今天就能跑**（无需任何代码）；
> 2. **AI 臂的 LLR 与前端无关** —— 前端换成 Metal 或 CPU，20/20 个单元**逐字相同**（§2.3）。
>
> ⇒ 所以 `npu` 的真实内容只有一件事：**把 LDPC 解码器从 Metal 换成 CPU**（那是 gNB 空中链用的那一个），
> **加上命名与拒绝非法组合**。**估计器拷贝不做**（理由见 §3.3）。

**状态**：规划（未实现）。**决策者**：用户。**规模**：★ **4 个文件、约半天**（不含测量）。

---

## 1. 目标

新增 `--phy_pipeline npu`，语义是：

> **PHY 全部模块在 CPU/DSP 上（FFT、估计、均衡/解映射、LDPC），只有"网络"在 ANE（Core ML）跑；
> 路径里不得出现任何 Metal 模块。**

**⇒ 要回答的问题**：**AI 相对真实 gNB 的 CPU 链好多少** —— 这个问题至今**没有测过** ✓

**⇒ 为什么必须问**：同一条腿（`s2_amc_ping_1102a`）、同一 5 000 个单元：

| 链 | 二进制 | 解码/5 000 | 交付比特 |
|---|---|---|---|
| **dump / 语料标签（≈ 真 gNB）** | `ul_chain_replay_cpu`（`b68ef309…`）| ★ **4 105（82.1%）** | ★ **11 565 432** |
| 尺子 `clean_cpu.csv`（Metal 链的经典路）| `metal/ul_chain_replay`（`dbe8f06d…` / 203 上 `d785f039…`）| 3 465（69.3%） | 8 933 800 |
| 差 | | **+640 单元** | ★★ **+29.5%** |

**⇒ 今天所有 "vs CPU" 的增益（+13.39%、+20.32% …）都是相对**弱 29.5% 的那条链** ✓
**⇒ `npu` 口径才是"相对真 gNB 链"的数字** ✓

---

## 2. 现状与证据（全部已实测）

### 2.1 pipeline 现在有三种模式

`include/ocudu/phy/phy_pipeline_mode.h`：

```cpp
enum class phy_pipeline_mode { cpu, cpu_gpu, gpu };
```

- 模式是**编排标签**，不是数据通路输入；被契约/探针读取（`phy_pipeline_contract.h`、`phy_pipeline_strict.h`、`phy_pipeline_crossings.h`、`ul_pipeline_probe.h`）
- **空中链（apps）也有同一个旋钮**：`--expert_phy.phy_pipeline`（`auto|cpu|cpu_gpu|gpu`）＋ `--expert_phy.pusch_ldpc_decoder_type`

### 2.2 回放器的四个 bool 与两个词汇表

`lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp`：

| 旋钮 | 行 | 设置 |
|---|---|---|
| `--phy_pipeline cpu` | 528–532 | CE/demod = CPU（**不动解码器**）|
| `--phy_pipeline cpu_gpu` | 533–535 | 同上（模块级卸载是**逐模块 opt-in**）|
| `--phy_pipeline gpu` | 536–540 | CE/demod = Metal（**不动解码器**）|
| `--metal`（旧别名）| 574–577 | **CE/demod/LDPC 三者全 Metal** |
| `--metal-cpu-ldpc`（旧别名）| 580–583 | CE/demod = Metal，**LDPC = CPU** |
| `--metal-cpu-demod`（旧别名）| 584–587 | 只有 CE = Metal |
| `--pusch_ldpc_decoder_type` | 546–563 | `auto|generic|neon|avx2|avx512|metal|metal_flooding|…`（**与空中链同名**）|

**⇒ ★★ 重要澄清（用户问的）**：`--metal-cpu-ldpc` **是回放器自己的旧别名，空中链没有这个名字** ✓
它的等价空中写法是：**`--expert_phy.phy_pipeline cpu_gpu` ＋ Metal 的 CE/demod 旋钮 ＋ `--pusch_ldpc_decoder_type generic`（CPU 解码器）** ✓
代码里明确写了这个历史（523–526 行）：回放器当年自造了 `--cpu/--metal/--metal-cpu-ldpc/--metal-cpu-demod`，
与空中链的词汇表对不上，所以**旧名字保留为别名** ✓

### 2.3 ★★★ 决定性实验：AI 臂的 LLR 与前端无关

20 个单元、同一 bundle（`w2c32_b18_e46`）：

| 组 | 配置 | 前端 | LDPC |
|---|---|---|---|
| **A** | `--metal-cpu-ldpc` ＋ `backend=ai` | Metal | **CPU** |
| **B** | `--phy_pipeline cpu` ＋ `backend=ai` | **CPU** | CPU |
| **C** | `--metal --phy_pipeline gpu` ＋ `backend=ai` | Metal | **Metal** |

| 比较 | 差异 | 结论 |
|---|---|---|
| ★ **A vs B**（只差前端）| ★★ **0 处** | ★★★ **前端不影响 AI 臂输出** ✓ |
| **A vs C**（只差解码器）| **9 处** | ★★ **差异全部来自 LDPC 解码器** ✓ |

**⇒ ⇒ ★★★ 两个直接后果**：
1. **`npu` 不需要改网络、不需要改估计器、不需要重训** ✓
2. **`npu` 与今天 `gpu` 口径的差异，只会有解码器这一项**（Metal → CPU）——而这正是 gNB 跑的那一个 ✓

---

## 3. 设计

### 3.1 `npu` 的语义（写进 enum 注释）

| 模式 | 语义 |
|---|---|
| `cpu` | 全链 CPU（含 CPU LDPC）|
| `cpu_gpu` | 模块级卸载 |
| `gpu` | 融合 lane（CE/demod 在 Metal；**解码器仍由 `--pusch_ldpc_decoder_type` 决定**）|
| ★ **`npu`** | ★ **= 全 CPU 模块（FFT/CE/均衡/解映射/LDPC）＋ `pusch_receiver_backend=ai`**；<br>**禁止任何 Metal 模块**；`backend` 不是 `ai` 时**报错**（不是警告）|

### 3.2 回放器的行为

`--phy_pipeline npu` ⇒

| 项 | 值 |
|---|---|
| `use_metal_ce` / `use_metal_demod` / `use_metal_decoder` | **全 false** |
| `dft_metal` | **false**（FFT 走 DSP/CPU）|
| `pusch_receiver_backend` | **必须 `ai`**，否则 `error` |
| 日志 | 打一行 `[phy_pipeline] npu: PHY on CPU/DSP, network on ANE` |
| 与 `--metal*` 别名同时给出 | **报错**（互斥，避免静默覆盖）|

**⇒ `gpu + ai` 组合**：按用户决定**取消** ⇒ 回放器里直接**报错**：
`--phy_pipeline gpu with --pusch_receiver_backend ai is no longer supported; use --phy_pipeline npu`

### 3.3 为什么**不做**估计器拷贝（v1 的方案作废）

**⇒ v1 计划克隆一份 CPU 估计器给 NPU 路径**，理由是"将来给网络路径单独调参时不波及 CPU 基准" ✓
**⇒ 现在不需要了**，因为：
1. ★★ **AI 臂根本不消费前端**（§2.3，A/B = 0 处差异）⇒ 估计器只服务**经典链与上报**；
2. ★★ **`npu` 下估计器就用现有的 CPU 实现**，它同时服务"经典臂"与"AI 臂的上报"⇒ **两条臂共享同一份前端，
   这恰恰是公平比较要求的** ✓
3. ★ 真要对 NPU 路径单独改估计器，那是**将来的独立需求**，到时有实测理由再做，不在这次范围内 ✓

### 3.4 `--pusch_receiver_backend` 旋钮：**保留**

**⇒ 实测规模**：`grep` 出 **261 处引用**，分布在
`apps/units/flexible_o_du/o_du_low/{du_low_config.h,du_low_config_yaml_writer.cpp,du_low_phy_pipeline.h}`、
`upper_phy_factories.cpp`、`pusch/processor_factories.cpp`、`pusch_processor_impl.cpp`、
以及 `tests/unittests/apps/units/flexible_du/...` —— 它还是**应用层 YAML 配置**的一部分（能读写回去）✓
**⇒ 决定**：**保留旋钮**，让 `npu` 模式**隐含**它（`npu` ⇒ backend 必须是 `ai`）；
**删除旋钮另立任务**（含 YAML 兼容与测试面）✓

---

## 4. 改动清单

| # | 文件 | 改动 | 规模 |
|---|---|---|---|
| 1 | `include/ocudu/phy/phy_pipeline_mode.h` | 加 `npu`：枚举 ＋ `to_string` ＋ `from_string` ＋ 注释（§3.1 语义）| **S** |
| 2 | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp` | `--phy_pipeline npu` ⇒ 全 CPU ＋ 强制 `ai`；`gpu+ai` ⇒ 报错；与 `--metal*` 互斥；日志行 | **S** |
| 3 | `~/ai_depth3/exp_bits_1102/run_arm_ext.py` | 透传 `--phy-pipeline npu`，日志记 `PIPELINE npu` | **S** |
| 4 | 文档：`gate_measurement_runbook.md`、`depth3_gate_and_model_pk.md` | 增加"npu 口径"，与"gpu 口径"并列 | **S** |

**⇒ 合计：4 个文件，约半天。** 无新算法、无新模块、无重训、无契约改动（除枚举语义）。

---

## 5. 验收判据

| # | 判据 | 状态 |
|---|---|---|
| 1 | ★ `npu+ai` ≡ `--metal-cpu-ldpc` ＋ `ai`（**20/20 单元逐字一致**）| ★ **已验**（§2.3 A vs B）|
| 2 | `npu+classic` ≡ `cpu` 链（同一 5 000 单元逐单元一致；对照 dump：**4 105 解码 / 11 565 432 bit**）| 待做（实现后）|
| 3 | `npu` 下 `backend=classic` ⇒ **报错退出**；`gpu+ai` ⇒ **报错退出** | 待做 |
| 4 | 日志含 `PIPELINE npu`、`BIN_SHA`、`BUNDLE_FILES`、`SIDECAR` | 待做 |

---

## 6. 测量计划（**代码未改之前就能开始**）

**⇒ 关键：`npu` 只是命名；用现有开关就能拿到 npu 口径的数字** ✓

```bash
# 现在就能跑（≡ 未来的 --phy_pipeline npu）
OCUDU_DEPTH3_FALLBACK=none OCUDU_REPLAY_MODEL=<bundle>.mlmodelc \
  ul_chain_replay <unit> --out <o> --phy_pipeline cpu --pusch_receiver_backend ai
```

**⇒ 建议顺序**：
1. **先用现有开关跑一臂 5 000 单元**（`run_arm_ext.py` 里把 `--phy_pipeline cpu` 传下去，并记 `PIPELINE cpu+ai`）
   ⇒ 得到"AI vs gNB CPU 链"的第一个真实数字（预计基线是 11.57 M bit，AI 增益会明显小于 gpu 口径的 +13.39%）；
2. 再把 `npu` 实现（半天），用判据 1/2/3 复核；
3. 之后所有新数字标 **npu 口径**；`gpu` 口径的数字保留为历史对照 ✓

**⇒ 代价**：CPU 前端比 Metal 慢，5 000 单元预计 **40–70 min/臂**（gpu 口径约 24 min）✓

---

## 7. 风险与注意

| # | 风险 | 说明 | 对策 |
|---|---|---|---|
| 1 | **数字会变** | `npu` 与历史 `gpu` 口径的差异**只来自解码器**（§2.3），但足以改变判决（20 单元里 1 个 KO→OK）| ★ 这是**有意的**；文档并列两种口径 |
| 2 | **SINR 仍是经典估计器给的** | 网络目前不输出上报估计量（用户 20:21 指出原设计有）| ★ **npu 口径下不要用 SINR 比较两臂**，只看交付比特 |
| 3 | **`cpu_gpu` 语义** | 该模式**不会**自己打开 Metal 模块（逐模块 opt-in）| 实现时别顺手把 `npu` 写成 `cpu_gpu` 的变体 |
| 4 | **旋钮删除** | 261 处引用 | ★ 另立任务，不在本次范围 |
| 5 | **HARQ 口径** | PK §51 的限定在两种口径下都成立 | 不变 |

---

## 8. v1 作废的原因（留档）

| v1 的假设 | 实际情况 |
|---|---|
| 要克隆 CPU 估计器给 NPU 路径 | ★ 不需要：AI 臂不消费前端（§2.3）|
| 要适配 4 个契约/探针头文件 | ★ 不需要：`npu` 在契约上等价于 `cpu`（无 device 交叉、无融合 lane）|
| 要 1–2 天 | ★ **约半天、4 个文件** |
| `npu+ai ≡ gpu+ai`（预测）| ★ 修正为：**`npu+ai ≡ metal-cpu-ldpc + ai`**；与 `gpu`（Metal 解码器）**不同**，差异来自解码器 |
