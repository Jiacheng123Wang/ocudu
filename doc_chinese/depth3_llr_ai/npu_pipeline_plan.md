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

---

## 9. ★★★★★ 2026-11-05 21:00 追加：**统一回放与实链的 pipeline 词汇表**，并把 **Metal LDPC 从默认里拿掉**

### 9.1 用户的第三问：`gpu` 是不是就是 fused lane？——**实链是，回放不一定**

| 环境 | `phy_pipeline gpu` 实际含义 | 证据 |
|---|---|---|
| ★ **实链（apps / gNB）** | ★★ **fused lane**：IQ→LLR 在一条 device pipeline 里，宿主只两次数据跨界 | 枚举注释（`phy_pipeline_mode.h:19`）＋ 实链日志 `[phy_pipeline] mode=gpu fused=yes … lane=IQ->LLR`（`du_low_config_translator.cpp:54–58`）|
| **回放器** | ★ 只设 `use_metal_ce = use_metal_demod = true`（**模块级 Metal**）；**从不发布 pipeline 模式** | `grep phy_pipeline_mode_registry::set` 只出现在 apps 与单测；回放器里一次都没有 |

**⇒ ⇒ ★★★ 结论**：**实链的 `gpu` 是 fused lane，它确实就是 depth3 的 Metal 对应物**（两者都是"整条 LLR 链一次交付"）✓
**但回放器的 `gpu` **没有**发布这个模式 ⇒ 探针/契约看不到、fused 的严格策略也不会生效 ⇒
**回放器的 "gpu" 与实链的 "gpu" 不是同一件事** ✗ —— **这正是要统一词汇表的直接理由** ✓

### 9.2 ★★ 另一个必须修的默认：**历史测量里解码器是 Metal**

**⇒ 我们所有已记录的 AI 测量，命令都是 `--metal --phy_pipeline gpu --pusch_receiver_backend ai`**；
**而 `--metal` 这个别名会把 `use_metal_decoder` 也置真** ⇒ **那些数字用的是 Metal LDPC** ✓
**⇒ 与实链默认（CPU 解码器）不一致** ⇒ 又一个口径差异 ✓
**⇒ 与 §2.3 的 A vs C 一致**：Metal LDPC 与 CPU LDPC 在 20 个单元里差 9 处（含 1 个判决翻转）✓

### 9.3 统一后的词汇表（回放器向实链看齐）

| 项 | 统一后的规则 |
|---|---|
| pipeline 选项 | ★ **只有** `--phy_pipeline auto|cpu|cpu_gpu|gpu|npu`（与 `--expert_phy.phy_pipeline` 同名同值）|
| 旧的四个别名 | ★ **删除**（`--cpu`、`--metal`、`--metal-cpu-ldpc`、`--metal-cpu-demod`）；若保留则**必须报错并指路**，不允许静默别名 |
| 解码器 | ★★ **由 `--pusch_ldpc_decoder_type` 单独决定，默认 CPU**（`auto` → CPU）；**`gpu` 不再隐含 Metal 解码器** |
| 模式发布 | ★★ 回放器**必须调用 `phy_pipeline_mode_registry::set(mode)`** —— 让契约/探针在回放里与实链行为一致 |
| `npu` | 新增（§3.1）：全 CPU ＋ `backend=ai`；`gpu+ai` 报错 |

### 9.4 这对改动清单与里程碑的影响

**⇒ 新增 M0（前置）**：

| 里程碑 | 内容 | 验收 |
|---|---|---|
| ★ **M0** | 统一词汇表：删四个别名（或改成显式报错）、**发布模式**、**解码器默认 CPU**、日志打印解析后的 `mode/decoder` | 同一份命令在实链与回放里**逐字相同**；`--metal` 之类**报错并指路**；`gpu` 下**默认 CPU 解码器** |

**⇒ 改动文件增加**：`ul_chain_replay.cpp`（解析与发布）、`run_arm_ext.py`（去掉 `--metal`，改传 `--phy_pipeline` ＋ `--pusch_ldpc_decoder_type`）；
**⇒ 规模从 4 个文件升到 5 个，仍约半天到一天** ✓

### 9.5 ★★ 对历史数字的影响（必须写进 PK 文档）

**⇒ 历史 gpu 口径 = **Metal CE/demod ＋ Metal LDPC ＋ AI** ✓
**⇒ npu 口径 = **CPU 全套 ＋ AI** ✓
**⇒ 两者差异**（§2.3）**只来自解码器**，但足以让个别单元判决翻转 ⇒ **新口径要重新测，不能从旧数字换算** ✓

★ 状态 ✓：★★★ **"实链 gpu = fused lane（= depth3 的 Metal 对应物）"已确认，并指出回放器没发布模式这一差异** ✓；
★★ **历史测量用的是 Metal LDPC 这一事实已查明** ✓；★★★ **统一词汇表（删别名、发布模式、解码器默认 CPU）已写入设计，并新增 M0 里程碑** ✓。

---

## 10. ★★★★★ 2026-11-05 21:10 **更正 §9.1：回放与实链用的是**同一批功能模块**，差别只在"模式是否发布"这一层策略**

**(a) ★★★ 用户问得对：既然是同一批模块，为什么说"不一样"？**

**⇒ 我 §9.1 的措辞**过重**了 ✗ —— 准确的说法是：**

| 层 | 实链 `gpu` | 回放 `--phy_pipeline gpu` |
|---|---|---|
| **功能模块**（CE / 均衡 / 解调 / 解码器）| Metal 实现 | ★ **同一批 Metal 实现**（同一个工厂、同样的三个 bool）✓ |
| **数据通路数学** | 不因模式分支 | ★ **同样不分支** ✓ |
| ★ **pipeline 模式是否发布** | **发布**（`du_low_config_translator.cpp:51`）| ★★ **不发布**（刻意如此）|
| ★ **由模式派生的策略** | **生效**：`phy_pipeline_strict_enabled()` 为真 ⇒ **device 拒绝的 hop 必须失败（CRC KO），不许回退到宿主** | ★★ **不生效** ⇒ 走"回退到宿主"的容错路线 |
| 探针 | `in_fused_lane()` 为真 ⇒ 分段时延探针自关 | 为假 ⇒ 探针照常 |

**⇒ ⇒ ★★★ 所以"不一样"指的是**编排策略**，不是**功能模块** ✓
**⇒ 在同一台健康设备上，两条路的**数值结果应当相同**；差别只在**某次 hop 被 device 拒绝**时：
实链**失败**（让 MAC 重传），回放**回退宿主**把它算出来 ✓

**(b) ★★★ 代码里写明这是**故意的**（`phy_pipeline_strict.h` 的注释）**

> "`OCUDU_GPU_STRICT` overrides the decision. It exists because the **offline harness (`ul_chain_replay`)
> deliberately never publishes a pipeline mode**, so without the override the fused lane's own policy could not
> be exercised offline at all — and a policy that cannot be tested is the failure mode this whole line keeps correcting."

**⇒ ★★ 所以回放器**有一个专门的开关 `OCUDU_GPU_STRICT=1`** 用来**在离线下打开 fused 的策略** ✓
**⇒ ★★ 这意味着**：离线上我们**一直跑的是"模块级 Metal + 宿主回退"**；
严格 fused 策略（device 拒绝即失败）**从没在离线上跑过**，除非显式设了 `OCUDU_GPU_STRICT=1` ✓

**(c) ★★★ 对统一方案的影响（修订 §9.3/§9.4）**

| 项 | 修订后的做法 |
|---|---|
| 词汇表 | 仍然统一为 `auto|cpu|cpu_gpu|gpu|npu` ＋ `--pusch_ldpc_decoder_type` ✓ |
| ★ **模式发布** | ★★ **回放器改为"发布模式"**（与实链一致）——这才是"同一件事"的**唯一缺口** ✓ |
| ★ **严格策略** | ★ 发布后**默认生效**；离线下若还要"回退宿主"的容错行为，**显式**用 `OCUDU_GPU_STRICT=0` 关掉（把例外变成显式选择）✓ |
| 解码器 | 仍由 `--pusch_ldpc_decoder_type` 决定、默认 CPU ✓ |
| `npu` | 新增，全 CPU ＋ `backend=ai` ✓ |

**⇒ ★★ 这样 `--phy_pipeline gpu` 在两处才是**字面同一件事**：同一批 Metal 模块 ＋ 同一个已发布模式 ＋ 同一套严格策略 ✓

★ 状态 ✓：★★★ **§9.1 的过重措辞已更正：模块相同、差在"模式是否发布"及其派生策略** ✓；
★★ **`OCUDU_GPU_STRICT` 的存在与"回放刻意不发布"已在代码注释中确认并记录** ✓；
★★ **统一方案因此收敛为一件事：回放器发布模式（＋默认严格），例外改为显式** ✓。

---

## 11. ★★★★★ 2026-11-05 21:25 **更正 §6："CPU 前端较慢"是错的 —— 实测 CPU 前端比 Metal 快 2.8 倍**

**(a) ★★★ 我此前的说法**：`npu` 口径 5 000 单元"预计 40–70 min/臂"（暗示 CPU 前端更慢）✗
**⇒ 用户当场质疑"应该是更快" —— 他判断正确** ✓ 下面是实测。

**(b) ★★★★ 同机同批对照（本机 M4 Pro；30 个单元；**中位 wall/单元**，含进程启动）**

| 配置 | 中位 | 相对全 CPU |
|---|---|---|
| ★ **cpu + classic**（全 CPU 经典链）| ★ **33 ms** | 1.0× |
| ★★ **npu + ai**（全 CPU ＋ 网络）| ★★ **85 ms** | 2.6× |
| **gpu + classic**（Metal 前端）| **94 ms** | ★ **2.8×** |
| gpu + classic ＋ **Metal LDPC** | 111 ms | 3.4× |

**⇒ ⇒ ★★★ 三个结论**：
1. ★★★ **CPU 前端比 Metal 前端快 2.8 倍**（33 vs 94 ms）——**我错了，用户对了** ✓
2. ★★ **`npu + ai`（85 ms）比"Metal 前端 ＋ 经典接收机"（94 ms）还快** ✓
   ⇒ 即**把网络放在 CPU 前端上跑，单价比用 Metal 跑经典链还便宜** ✓
3. ★ **Metal LDPC 在同一前端上再多 ~17 ms**（94 → 111），与"Metal LDPC 性能较差、默认不用"一致 ✓

**(c) ★★★ 为什么 CPU 更快（三条原因，都是"per-unit 一进程"造成的）**

| # | 原因 | 说明 |
|---|---|---|
| ★ 1 | **每单元一个新进程** | Metal lane 每次要建 device 上下文/命令缓冲/做 host↔device 拷贝 ⇒ **单 slot 的固定开销极大**；CPU 路径没有这个固定成本 |
| ★ 2 | **张量太小** | 612×14 的栅格 ⇒ Metal kernel 是**launch-bound**，与训练时"小 1×1 卷积 GPU 更慢"是同一个道理 ✓ |
| ★ 3 | **Metal LDPC 另加成本** | 同一前端上 +17 ms ✓ |

**(d) ★★★ 修正后的时间预算（5 000 单元，串行、一进程一单元）**

| 机器 | 单价（含启动）| 5 000 单元 |
|---|---|---|
| ★ **本机（M4 Pro）** | 85 ms | ★ **约 7 min** |
| 203（M2，估计比本机慢 1.5–2×）| 130–170 ms | **11–14 min** |
| 旧 gpu 口径（203，实测）| 270 ms | 22.5 min |

**⇒ ★★ 所以 `npu` 口径不但更**正确**，也**更快**（本机 7 min/臂，203 约 12–15 min）✓
**⇒ ★ 这也意味着**：以后 5 000 单元的重测成本很低，**可以放心把每条臂都重跑一遍** ✓

★ 状态 ✓：★★★ **"CPU 前端较慢"已用同机实测推翻（快 2.8×）** ✓；★★ **npu+ai 85 ms 甚至快于 Metal 前端＋经典 94 ms** ✓；
★★ **原因（每单元一进程的固定开销、张量太小、Metal LDPC 附加成本）已写明** ✓；★★ **时间预算改为 7 min（本机）/12–15 min（203）** ✓。

---

## 12. ★★★★ 2026-11-05 21:45 **实链回归：`npu` 与 `gpu` 都能把小区拉起来、射频零丢块**（未做手机接入：核心网 AMF 没起）

**(a) ★★★ 编译器先抓到一处真问题**

**⇒ 加枚举值后 `make gnb` 直接失败**：
```
du_low_phy_pipeline.h:276:11: error: enumeration value 'npu' not handled in switch [-Werror,-Wswitch]
```
**⇒ 修法**：`npu` 与 `cpu` **共用同一个 case**（模块全部 CPU、禁止 device 写栅格、`lane_fused=false`），
只有"允许 AI 接收机"这一点不同 ✓
**⇒ 另外两处硬编码的模式白名单也要加**（否则 CLI 直接拒 `npu`）：
`du_low_config_cli11_schema.cpp:176` 的校验器、`du_low_phy_pipeline.h:221` 的错误文本 ✓

**(b) ★★★ 单元测试：模式矩阵未变**

`du_low_phy_pipeline_test`：**30/30 通过**（含 `cpu`、`cpu_gpu`、`gpu` 的解析与冲突规则）✓

**(c) ★★★★ 实链试飞（本机 B210，n78 20 MHz，PCI=1）**

| 模式 | 结果 |
|---|---|
| ★ **`gpu`**（生产路径）| 小区起来；**`radio sample continuity: 0 gaps over 537 129 blocks -> OK`** ✓ |
| ★ **`npu`**（新路径）| ★ 小区起来；**`0 gaps over 481 110 blocks -> OK`** ✓，且日志出现 `contract (mode=npu)` ✓ |

**⇒ ★★★ 结论**：**两条路径的 PHY/射频都正常**，新改动没有搞崩实链 ✓

**(d) ★★ 但**没有做成手机接入**：核心网 AMF 没起**

**⇒ 现象**：`"NG Setup Procedure" timed out after 5000ms` ✓
**⇒ 原因**（已核查 211）：只有 `open5gs-smfd/upfd` 在跑，**没有 `open5gs-amfd`**，**38412 端口未监听** ✓
**⇒ 与本次改动无关**：这是核心网的运行状态 ✓ —— **要测手机接入，必须先起 AMF** ✓

★ 状态 ✓：★★★ **编译错误（缺 npu case）＋两处白名单已修，`gnb` 构建通过** ✓；★★ **模式矩阵单测 30/30** ✓；
★★★ **实链在 gpu 与 npu 下都"零丢块"拉起小区** ✓；★★ **手机接入被 AMF 缺失阻塞，原因已查明** ✓。

---

## 13. ★★★★ 2026-11-05 21:40 **实链跑通（手机接入、ping 通）后的一问：npu 下为什么还量到 CE/EQ-DEMOD？**

**(a) ★★★ 实测（用户 2026-11-05 飞行日志）**

```
[ul_pipeline]        mean=585.2us median=564.0us  p99=705.0us
[ul_time_frequency]  mean=512.9us median=489.0us   ← DFT 占大头
[ul_channel_estimation] mean=24.8us median=23.3us
[ul_equalization_demod] mean=20.8us median=19.8us
```

**(b) ★★★★ 两个探针量的**不是**你以为的东西**

| 探针 | 代码位置 | `ai_network` 下量到的是什么 |
|---|---|---|
| **`ul_channel_estimation`** | `pusch_processor_impl.cpp:580`（`record_ce_end`）| ★★ **经典估计器**：**它确实还在跑，而且是**设计如此** ✓ |
| **`ul_equalization_demod`** | 同文件 **904–912**：`record_demod_enter` → **`dependencies->get_depth3().demodulate(...)`** → `record_demod_return` | ★★ **depth3 臂自己的 `demodulate()`**（组平面、前向、量化、解扰），**不是经典解调器** ✓ |

**⇒ 关键代码事实**（`pusch_depth3_receiver.h` 的枚举注释，原文）：
> `ai_network`: `demodulate()` assembles the contract's plane from the grid, runs the forward pass on the ANE and
> delivers the network's own soft bits; **the classical demodulator does not run**. `estimate()` **still runs the
> classical estimator**, because the chain's measurements (RSRP, EPRE, SNR, timing advance) are read from its
> results until the reporting heads replace them.

**⇒ ⇒ ★★★ 所以**：
1. **CE 24.8 µs 是真的经典估计器在跑** —— 因为**网络还没有输出上报量**（用户 20:21 指出"原设计有，只是没做"）✓
   ⇒ 一旦网络输出 RSRP/EPRE/SNR/TA，这段就可以拿掉 ⇒ **那是下一项优化，不是 bug** ✓
2. **EQ-DEMOD 20.8 µs 是网络臂自己的开销**（含 ANE 前向）—— **不是**经典解调器 ✓
   ⇒ 也说明**网络前向本身很快**（~20 µs 量级），回放里看到的"50 ms/单元"主要是**进程启动与模型加载**，不是推理 ✓

**(c) ★★ 如何确认那次飞行真的跑的是网络（而不是 `ai_identity` 兜底）**

**⇒ 看启动日志里这两类行**：
- `depth-3 receiver arm: ai (...)`（回放器）/ `[pusch_receiver] backend=...`
- ★ `[pusch_receiver] AI depth-3: ...`（**绑定结果**，逐模型一条：kept / redundant / refused）
**⇒ ★★★ 若日志里没有"绑定成功"的行，那么 `ai_identity` 在跑（委托经典链）⇒ CE 与 EQ-DEMOD 都是经典实现** ✓
—— 这是"npu 下为什么还有 EQ-DEMOD"的**唯一另一种解释**，用绑定行即可排除 ✓

★ 状态 ✓：★★★ **两个探针的代码位置与语义已查明**（CE 在 580 行、EQ-DEMOD 括的是 `get_depth3().demodulate`）✓；
★★ **"CE 仍在跑是因为上报量还没由网络输出"已写明** ✓；★★ **"EQ-DEMOD 是网络臂自身开销、非经典解调器"已写明** ✓；
★★ **如何用绑定行排除 `ai_identity` 兜底** ✓。

---

## 14. ★★★★★ 2026-11-05 21:50 **查证：那次 npu 飞行跑的是**经典接收机**（`receiver=classic`），网络没有参与**

**(a) ★★★ 用户的问题：`depth-3 receiver arm: ai (...)` 打到 console 还是日志？**

| 位置 | 形态 | 去向 |
|---|---|---|
| ★ **回放器**（`ul_chain_replay`）| `depth-3 receiver arm: ai (--pusch_receiver_backend)` | **stdout**（`std::printf`）|
| ★★ **实链（gnb）** | ★ `[phy_pipeline]   receiver=classic\|ai (expert_phy --pusch_receiver_backend …)` | ★ **INFO 日志**（logger）|
| 实链（绑定细节）| `[pusch_receiver] AI depth-3: …`（kept / redundant / refused）| ★ **stderr**（`fmt::print(stderr, …)`）⇒ 没重定向就只在终端 |

**(b) ★★★★★ 查 `/tmp/gnb.log` 的结果（决定性）**

```
2026-11-05T13:22:49.789964 [PHY] [I] [0.0] [phy_pipeline]   receiver=classic (expert_phy --pusch_receiver_backend auto)
```

**⇒ ★★★ 那次飞行的接收机是 **`classic`**（配置里 `pusch_receiver_backend: auto` ⇒ 经典）** ✗ 网络**没有参与** ✓
**⇒ ⇒ ★★★ 所以那两个探针完全解释得通**：

| 探针 | 那次飞行量到的是 |
|---|---|
| `ul_channel_estimation` 24.8 µs | ★ **经典估计器**（因为整条链就是经典的）|
| `ul_equalization_demod` 20.8 µs | ★ **经典解调器**（同上）|

**⇒ ★★ 即 §13 的两种解释里，落到的是"另一种"：不是 AI 臂在跑，而是**根本没开 AI 臂** ✓

**(c) ★★★ 那次飞行的真正价值（仍然很大）**

**⇒ 它证明的是**pipeline 模式本身**没问题：`--phy_pipeline npu` ⇒ 全 CPU PHY、射频零丢块、**手机接入、ping 通** ✓
**⇒ 但它**没有测到 AI 臂**：要测，必须显式给接收机和模型：
```bash
gnb -c <config> \
  --expert_phy.phy_pipeline npu \
  --expert_phy.pusch_receiver_backend ai \
  --expert_phy.pusch_receiver_model <bundle>.mlmodelc
```
**⇒ ★★ 缺模型时实链会**报错**（`du_low_phy_pipeline.h`：`ai` 必须有模型，否则拒），不会静默降级 ✓

**(d) ★★ 下一次飞行的检查清单（3 行日志即可判定）**

1. `[phy_pipeline] receiver=ai (...)` ⇒ 接收机是 AI；
2. `[pusch_receiver] AI depth-3: … kept …` ⇒ **模型绑定成功**（在 stderr，记得 `2>&1`）；
3. 飞行后 `[ul_channel_estimation]` 仍在（设计如此，上报量还没由网络输出），
   而 `[ul_equalization_demod]` 变成**网络臂的开销**（§13）✓

★ 状态 ✓：★★★ **"那次 npu 飞行是 classic 接收机"已由 `/tmp/gnb.log` 证实** ✓；
★★ **两个探针因此都是经典链的真实耗时，解释闭合** ✓；★★ **AI 臂上天的确切命令与三行判据已写清** ✓。

---

## 15. ★★★★ 2026-11-05 22:00 **"共享测量层"是什么、是不是 CPU —— 是，而且它**天生只有 CPU**

**(a) ★★★ 它是什么**

**⇒ 函数**：`measure_ul_slow(pilots, scaling, nof_layers)` —— `lib/phy/upper/channel_processors/pusch/pusch_ul_slow_measurements.cpp` ✓
**⇒ 输入**：**DM-RS 导频的 LS 估计**（`ls_re/ls_im`，按符号、再按子载波排序）✓
**⇒ 输出**：**EPRE、噪声方差、SNR、CFO**（全部是栅格单位，与经典估计器的口径一致）✓
**⇒ ★★ 它是一段**普通库函数**：**没有 Metal 版本、没有 backend 旋钮、没有 device 路径** ✓
**⇒ ★★★ 所以"共享测量层 = CPU 路径"**不是配置选择，而是**构造使然** ✓

**(b) ★★★ 为什么叫"共享"**

**⇒ 经典解调器与 depth3 臂**都调它**（`pusch_depth3_measurements.cpp:72` 调 `measure_ul_slow`；经典路的解调器用同一函数）** ⇒
**两条路报出的 RSRP/EPRE/SNR/CFO 不可能互相矛盾** ✓ —— 这正是"共享"的含义 ✓

**(c) ★★★ npu 飞行里那 24.8 µs 的 CE 与这层的关系**

**⇒ AI 臂的 `prepare()` 自己**组装 DM-RS 符号的 LS**（`prepare_assembler->assemble`），再调 `measure_ul_slow` 得到上报量 ✓
**⇒ 但 `estimate()` **仍然调用经典估计器**（`pusch_depth3_receiver.h`：上报量"read from its results **until the reporting heads replace them**"）✓
**⇒ ⇒ ★★ 于是**两套 LS/CE 都在跑**：臂自己的一套（CPU）＋ 经典估计器一套（npu 下也是 CPU）✓
**⇒ ★★★ 这就是**一个明确的优化点**：一旦上报量改由**臂自己的测量**提供，经典估计器那 ~25 µs 可以去掉** ✓
（落地方式是"npu＋ai 且 bundle 无 reporting head 时跳过经典 estimate()"，属下一项工作，不是当前 bug ✓）

**(d) ★★ 一句话回答**

**⇒ ★★★ **是的**：共享测量层现在跑的就是 CPU 路径（而且它只有 CPU 实现）；npu 里那 24.8 µs 的 CE 是**经典估计器**在跑，**
**和共享测量层是两条并行的 CPU 工作**，将来可以由臂自己的测量取代 ✓

★ 状态 ✓：★★ **共享测量层的实现位置、输入输出、CPU-only 性质已核实** ✓；★★ **"共享"指经典路与 AI 臂共用同一函数** ✓；
★★★ **npu 下 CE 与共享测量层是两套并行工作，已标为可优化项** ✓。
