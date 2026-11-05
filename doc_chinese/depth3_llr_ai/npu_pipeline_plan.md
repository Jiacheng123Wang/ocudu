# `npu` PHY pipeline 规划（第四条 pipeline，让网络路径彻底脱离 GPU 路径）

**状态**：★ **规划（未实现）**，2026-11-05 提出。**决策者**：用户。**执行**：待批准后实施。
**目标一句话**：新增 `--phy_pipeline npu`，使 **depth3 网络臂的前端（FFT、估计、解调、LDPC）全部走 CPU/DSP**，
**与 Metal/GPU 路径毫无关系** —— 从而能回答"**AI 相对真实 gNB 的 CPU 链好多少**"这个至今没有测过的问题。

---

## 1. 为什么必须做（这条是全部动机）

**⇒ 现状**：AI 臂只能在 `--phy_pipeline gpu`（Metal 融合通道）里跑；它的"经典链参照"是
**Metal 链的 `--pusch_receiver_backend classic`**，**不是**真实 gNB 跑的 CPU 链 ✓

**⇒ 实测差异（同一条腿 `s2_amc_ping_1102a`、同一 5 000 个单元）**：

| 链 | 二进制 | 调用方式 | 解码/5 000 | 交付比特 |
|---|---|---|---|---|
| **dump / 语料标签**（= 真 gNB 风格）| `ul_chain_replay_cpu`（`b68ef309…`）| `--phy_pipeline cpu --pusch_receiver_backend classic` | ★ **4 105（82.1%）** | ★ **11 565 432** |
| **尺子 `clean_cpu.csv`**（我们的"CPU 参照"）| `metal/ul_chain_replay`（`dbe8f06d…` / 203 上 `d785f039…`）| `--metal --phy_pipeline gpu --pusch_receiver_backend classic` | 3 465（69.3%） | 8 933 800 |
| 差 | | | **+640 个单元** | ★★ **+29.5%** |

**⇒ ★★★ 含义**：今天所有 "vs CPU" 的增益（+13.39%、+20.32% …）都是**相对一条弱 29.5% 的经典链**；
若拿真 gNB 的 CPU 链当基线，`c32@60` 的 10 129 952 bit **反而低于** 11 565 432（−12%）✓
**⇒ 但这个 −12% 也不公平**：AI 臂的前端是 Metal 估计器，两条链前端不同 ⇒ **必须把 AI 臂的前端换成 CPU**，
这就是 `npu` pipeline 的全部理由 ✓

---

## 2. 现状（已核实的代码与行为，2026-11-05）

### 2.1 pipeline 只有三种模式

`include/ocudu/phy/phy_pipeline_mode.h`：

```cpp
enum class phy_pipeline_mode { cpu, cpu_gpu, gpu };
```

- `cpu`：全链 CPU（每个模块的 backend 旋钮必须解析成 CPU 实现）
- `cpu_gpu`：模块级卸载（每个模块各自跨界）
- `gpu`：融合通道（IQ→LLR 在一个 device lane 里；LDPC 仍在 CPU）

**⇒ 模式是"编排标签"，不是数据通路的输入**（数据通路取各自配置）；它主要被**契约/探针**读取
（`phy_pipeline_contract.h`、`phy_pipeline_strict.h`、`phy_pipeline_crossings.h`、`ul_pipeline_probe.h`）✓

### 2.2 回放器的开关已经把模块分配到四个 bool

`lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp`：

| 旋钮 | 行 | 作用 |
|---|---|---|
| `--cpu` / `--metal` / `--metal-cpu-ldpc` / `--metal-cpu-demod` | 528–540, 574–585 | 设置 `use_metal_ce` / `use_metal_demod` / `use_metal_decoder` / `dft_metal` |
| `--phy_pipeline <mode>` | 528–540 | 与上面四个 bool 联动 |
| `--pusch_receiver_backend classic\|ai` | 568 | 选经典接收机或 depth3 臂 |
| 估计器选择 | 1165–1168 | `use_metal_ce` ⇒ `port_channel_estimator_algorithm::metal_mmse`，否则 ⇒ 默认（CPU）|
| 警告 | 1301–1308 | `ai` 但没有 metal CE/demod 时打 WARNING（"两层独立设置"）|

**⇒ ★★ 关键发现（已实测）**：**AI 臂现在就能在 `--phy_pipeline cpu` 下跑** ✓

```bash
OCUDU_DEPTH3_FALLBACK=none OCUDU_REPLAY_MODEL=<bundle>.mlmodelc \
  ul_chain_replay <unit> --out /tmp/x --phy_pipeline cpu --pusch_receiver_backend ai
```

输出：`depth-3 receiver arm: ai`、`(cpu CE, cpu equalizer/demapper, cpu LDPC, 6 decoder iteration(s))`、
`crc=OK iterations=2 sinr=30.31` —— **与该单元在 Metal 链上的结果逐字相同**（同一 bundle）✓
**⇒ 即**：`npu` 不是一个从零开始的新链路，而是**"把 AI 臂钉在 CPU 前端上"的正式化与保证** ✓

### 2.3 depth3 臂与估计器的关系

`lib/phy/upper/channel_processors/pusch/processor_factories.cpp`（~170–215 行）：
depth3 单元是**"估计 + 均衡 + 解映射"作为一个整体**构造的：

```cpp
unit->set_depth3(create_pusch_depth3_receiver(depth3_kind, unit->get_estimator(), unit->get_demodulator(), …));
```

**⇒ 臂只是"引用"估计器与解调器，不拥有它们**（注释明确：拥有会让严格策略去问没人执行的实例）✓
**⇒ 所以"估计器用哪一份实现"由 pipeline 的模块旋钮决定** —— 这正是 `npu` 要固定的那一项 ✓

---

## 3. 设计

### 3.1 语义（写进 enum 注释，作为契约）

| 模式 | 语义 |
|---|---|
| `cpu` | 全链 CPU |
| `cpu_gpu` | 模块级卸载 |
| `gpu` | 融合 lane（Metal）|
| ★ **`npu`** | ★★ **PHY 模块全在 CPU/DSP（FFT、估计、解调、LDPC），只有"网络"跑在 ANE（Core ML）**；<br>**不得出现任何 Metal 模块**；估计器使用 **NPU 专用的一份拷贝**（见 3.3）|

**⇒ 与 `cpu` 的区别**：`cpu` 里 `pusch_receiver_backend` 可以是 `classic`；`npu` 里它**必须是 `ai`**（否则等价于 `cpu`），
违反时**明确报错**而不是警告（沿用"旋钮不能被静默忽略"的既有纪律）✓

### 3.2 回放器的行为

`--phy_pipeline npu` ⇒

| 项 | 值 |
|---|---|
| `dft_metal` | **false**（FFT 走 DSP/CPU）|
| `use_metal_ce` | **false**（并改用 NPU 拷贝的估计器，见 3.3）|
| `use_metal_demod` | **false** |
| `use_metal_decoder` | **false**（LDPC 在 CPU）|
| `pusch_receiver_backend` | **必须是 `ai`**，否则 `error` |
| 模型 | 仍由 `OCUDU_REPLAY_MODEL` / `--expert_phy.pusch_receiver_model` 给 |

**⇒ 日志必须打印一行"npu 口径"**（例如 `[phy_pipeline] npu: all PHY modules on CPU/DSP, network on ANE`），
且 `BIN_SHA`/`MODEL`/`SCALES` 照旧记（现有 `run_arm_ext.py` 已记）✓

### 3.3 估计器"拷贝"（用户明确要求：拷贝，不是复用）

**⇒ 做法**：新增一份 **NPU 专用**的端口信道估计器实现（克隆现有 CPU 实现），
在 `port_channel_estimator_algorithm` 里加一个取值（如 `cpu_npu_copy` / `npu`），工厂按它构造 ✓

**⇒ 为什么拷贝而不是复用**（写进代码注释）：
1. ★ NPU 路径将来要**为网络臂单独调整**（例如给 AI 单元提供额外中间量、改平滑/插值策略），
   **这些改动不能波及现有 `cpu` 路径**（那是 gNB 的基准，必须保持可比）✓
2. ★ 拷贝让两条路径**各自可回归**：CPU 路径的数值不变（有既有测试兜底），NPU 路径的改动只影响自己 ✓
3. ★ 代价是一次性的代码重复 —— 用户已明确接受（"拷贝并不是冗余"）✓

**⇒ 文件建议**：
- `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_cpu_npu.{h,cpp}`（克隆自现有 CPU 实现）
- 在 `factories.cpp` 里按算法枚举分派；`npu` 模式选这一份 ✓

### 3.4 契约与探针（不能漏）

`phy_pipeline_strict.h` / `phy_pipeline_crossings.h` / `phy_pipeline_contract.h` / `ul_pipeline_probe.h`
里有对 `gpu` 的**特判**（例如"融合 lane 才关掉分段时延探针"、"gpu 才要求 device 真的跑过"）✓

**⇒ `npu` 的规则**：**按 `cpu` 对待**（无 device 交叉、无融合 lane），
但**额外要求"网络确实被调用"**（AI 臂的 `bound` 状态必须为真）——
否则一个 `npu` 跑出来是"纯 CPU 链"却声称是 AI 结果 ✓

---

## 4. 改动清单（按文件）

| # | 文件 | 改动 | 规模 |
|---|---|---|---|
| 1 | `include/ocudu/phy/phy_pipeline_mode.h` | 加 `npu`：枚举、`to_string`、`from_string`、注释（语义见 3.1）| S |
| 2 | `include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h` | 算法枚举加 `npu` 取值 | S |
| 3 | `include/ocudu/phy/upper/signal_processors/channel_estimator/factories.h` ＋ `lib/.../factories.cpp` | 按算法分派到 NPU 拷贝 | S |
| 4 | ★ 新文件 `port_channel_estimator_cpu_npu.{h,cpp}` | **克隆 CPU 估计器**（保持数值行为一致）| M |
| 5 | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp` | `--phy_pipeline npu` 的解析、强制 CPU 模块、强制 `backend=ai`、日志行 | M |
| 6 | `lib/phy/upper/channel_processors/pusch/processor_factories.cpp` | 确保 `npu` 下 depth3 臂拿到**NPU 拷贝的估计器**（而不是普通 CPU 的）| S |
| 7 | `include/ocudu/phy/phy_pipeline_strict.h`、`phy_pipeline_crossings.h`、`phy_pipeline_contract.h`、`support/executors/ul_pipeline_probe.h` | `npu` 按 `cpu` 处理 ＋ "臂必须已绑定"这一条 | S |
| 8 | `~/ai_depth3/exp_bits_1102/run_arm_ext.py`（测量侧） | 支持 `--phy-pipeline npu`（透传）并在日志里记 `PIPELINE npu` | S |
| 9 | 文档：`gate_measurement_runbook.md`、`depth3_gate_and_model_pk.md` | 新增"npu 口径"，与"gpu 口径"并列 | S |

**规模合计**：S×6 ＋ M×1 ⇒ **约 1–2 天**（不含测量与回归）

---

## 5. 里程碑（每步都有可验收的判据）

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M1** | 枚举 ＋ 回放器解析 ＋ 强制 CPU 模块 ＋ 日志 | `--phy_pipeline npu` 能跑通单个单元；日志有 npu 行；`--phy_pipeline npu` 且 backend=classic 时**报错退出** |
| **M2** | NPU 拷贝的估计器 ＋ 工厂分派 | `npu` 与 `cpu` 两条路径在同一批单元上**逐比特相同**（拷贝未引入差异）|
| **M3** | 契约/探针适配 ＋ `run_arm_ext.py` 透传 | `npu` 下"臂未绑定"会被拒绝；探针不因 `npu` 误报 device 交叉 |
| **M4** | 测量 | 5 000 单元上跑 **npu+classic**（应与 dump 的 4 105 解码一致）与 **npu+ai**，并给出"npu 口径"的增益 |

---

## 6. 验收判据（三个等式，缺一不可）

1. ★★ **`npu+classic` ≡ `cpu` 链**：同一 5 000 单元、逐单元 `crc`/`tbs` 完全一致（对照基准：
   **dump 的 4 105 个解码单元 / 11 565 432 bit**）✓
2. ★★ **`npu+ai` ≡ `gpu+ai`**：同一个 bundle、同一批单元，结果**逐单元相同**
   （因为 AI 的输入是频域栅格，前端估计器不参与它的计算）✓
   —— **若不相同**，说明 AI 臂**确实消费了估计器的输出**，那第 1 条就要改成"npu 口径自身自洽"，并记录差异 ✓
3. ★ **`npu+ai` 与 `npu+classic` 的差值 = 真正要报的"AI vs gNB CPU 链"增益** ✓

---

## 7. 风险与未决问题

| # | 风险 / 未决 | 说明 | 对策 |
|---|---|---|---|
| ★ 1 | **AI 臂是否消费估计器输出** | 若消费，则"npu+ai"与"gpu+ai"不会完全相同（M3 判据 2）| 先做一次单单元对照（已有 §2.2 的证据：结果相同）|
| ★ 2 | **性能** | CPU FFT/CE 比 Metal 慢；`npu` 回放会变慢（估计 1.5–3×）| 只用于**测量**，不做实时；5 000 单元预计 40–70 min/臂 |
| ★ 3 | **ANE 与 CPU 线程争用** | Core ML 推理与 CPU PHY 抢核 | 回放串行（既有纪律）；必要时降 `--jobs` |
| ★ 4 | **契约探针对 `gpu` 的特判** | 漏改会让 `npu` 误报或漏报 | M3 专门列表逐条过 |
| ★ 5 | **`p0_nominal`/SINR 上报** | `npu` 下上报走 CPU 链的估计量，与 gpu 口径**不可直接比 SINR** | 文档注明；SINR 只作参考，交付比特为主 |
| ★ 6 | **两套数字并存** | 之后会有"gpu 口径"与"npu 口径"两套 PK 数字 | ★ **所有表格必须标口径**；`gpu 口径`保留作历史对照 |

---

## 8. 对现有结论的影响（先说清）

- **已记录的 gpu 口径数字全部有效**，但它们回答的是"AI 相对 Metal 链经典路"✓
- **npu 口径**将回答"AI 相对真 gNB CPU 链"，**预期增益会明显变小**（基线强 29.5%）；
  若 npu 口径下 AI **仍然领先**，那才是可以对外讲的结论 ✓
- **HARQ 限定（PK §51）在两种口径下都存在**（都还没有 `--harq-chain`），不因 npu 而改变 ✓

---

## 9. 批准后第一步（建议）

**M1 先行**（枚举＋回放器＋强制 CPU＋日志），它不依赖估计器拷贝，
完成后立刻可以做 §6 的判据 2（`npu+ai` vs `gpu+ai` 同单元对照）——
**这一步就能回答"AI 臂到底消不消费估计器"这个最关键的不确定性** ✓

---

## 10. ★★★★★ 2026-11-05 20:30 **实测修正：`cpu+ai` 与 `gpu+ai` **并不等价** —— 所以 `npu` 不是纯别名**

**(a) ★★★ 我先前说错了一句**

**⇒ 我此前只对照了 **1 个单元**，看到 `cpu+ai` 与 `gpu+ai` 的 `crc`/`sinr` 相同，就写了"逐字相同" ✗
**⇒ 20 个单元逐行对照后：**2 个单元不同** ✓

```
cap_10020_16087_17922   gpu: crc=OK iterations=3   cpu: crc=OK iterations=2   ← 迭代数不同
cap_10052_16727_17922   gpu: crc=KO iterations=6   cpu: crc=OK iterations=6   ← ★ 判决翻转（KO → OK）
```

**⇒ ★★★ 结论**：**AI 臂的输出确实受前端影响**（至少在某些单元上）✓
**⇒ ⇒ 这直接影响本规划的两个判断**：
1. ★ `npu` **不是**"`cpu` ＋ `ai` 换个名字"：**它会产生不同的数字**（这正是要它的理由 —— 那些数字才是对 CPU 链的公平比较）；
2. ★ 我原先在 §6 判据 2 里假设的"`npu+ai ≡ gpu+ai`"**被实测否定** ✗ ⇒ 判据要改成：
   ★ **记录两者的差异，并把 `npu` 的数字作为新的正式口径** ✓

**(b) ★★★ 好消息：**测量今天就能开始，不需要任何代码**

```bash
# 已经可用（本机实测）：
OCUDU_DEPTH3_FALLBACK=none OCUDU_REPLAY_MODEL=<bundle>.mlmodelc \
  ul_chain_replay <unit> --out <o> --phy_pipeline cpu --pusch_receiver_backend ai
```

**⇒ ★★ 也就是说**：要拿到"AI vs 真 CPU 链"的数字，**现在就能跑 5 000 单元**（只是名字还叫 `cpu+ai`）✓

**(c) ★★★ 用户提议的"只改 config"要分三件看**

| 提议 | 规模 | 判断 |
|---|---|---|
| ★ 新增 `--phy_pipeline npu` ≡ `cpu` ＋ `ai` | ★ **S**（枚举 ＋ 回放器解析 ＋ 日志，1 个文件）| ✅ **简单，值得做** |
| ★ 取消 `gpu + ai` 组合 | ★ **S**（回放器里报错拒绝 ＋ 文档）| ✅ **可行，历史数字已记录** |
| ★★ **删除 `--pusch_receiver_backend`** | ★★★ **L：261 处引用** | ⚠ **不建议现在做** |

**⇒ ★★★ "删除 `--pusch_receiver_backend`" 的真实规模**（`grep` 实测）：**261 处引用**，分布在
`apps/units/.../du_low_config.h`、`du_low_config_yaml_writer.cpp`、`du_low_phy_pipeline.h`、
`upper_phy_factories.cpp`、`pusch/processor_factories.cpp`、`pusch_processor_impl.cpp`、
`tests/unittests/apps/units/flexible_du/...` 等 —— 它还是**应用层 YAML 配置**的一部分（能读写回去）✓
**⇒ 建议**：**保留这个旋钮**（作为内部开关），让 `npu` 模式**隐含**它；
删除旋钮作为**独立的重构任务**（有自己的 YAML 兼容/测试面），不要塞进这次 ✓

**(d) ★★★ 修订后的最小范围（建议批准这一版）**

| # | 改动 | 文件 | 规模 |
|---|---|---|---|
| 1 | `phy_pipeline_mode` 加 `npu` ＋ 注释语义 | `include/ocudu/phy/phy_pipeline_mode.h` | S |
| 2 | 回放器：`--phy_pipeline npu` ⇒ 全 CPU 模块 ＋ 强制 `backend=ai`；`gpu`＋`ai` ⇒ **报错拒绝**；日志打 `npu` 行 | `ul_chain_replay.cpp` | S |
| 3 | 测量脚本透传 `--phy-pipeline npu` | `run_arm_ext.py` | S |
| 4 | 文档：runbook ＋ PK 增加 "npu 口径" | 2 份 md | S |

**⇒ 规模：**4 个文件、约半天**（不含测量）** ✓ —— 这才是"只改 config"的真实版本 ✓

**(e) ★★ 顺带纠正一个设计点（用户 20:21 提到）**

**⇒** "网络本来可以输出上报估计量，只是我们目前没做" ✓
**⇒ 这意味着**：现在 `npu` 口径下的 **SINR/epre/rsrp 仍来自 CPU 经典估计器**，而网络只出 LLR ✓
**⇒ 后果**：`npu` 口径下**不要用 SINR 去比较两臂**（两者来源相同，但网络没参与）；**判据仍只看交付比特** ✓

★ 状态 ✓：★★★ **`cpu+ai` 与 `gpu+ai` 不相同的实测证据已记录** ✓；★★ **"测量今天可做、代码非必需"已写明** ✓；
★★★ **三件提议的规模（S / S / L＝261 处）已给出，并建议删除旋钮另立任务** ✓。
