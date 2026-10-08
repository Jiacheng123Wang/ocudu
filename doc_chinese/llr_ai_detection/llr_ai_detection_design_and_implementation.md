# AI LLR Detection —— 实施设计与步骤（**活文档，追加式**）

> **本文件是什么**：本工作流的**详细设计与实施计划**，以及**每次实作/飞行/决策的追加记录**（§11 Memo 区）。
> **高层视图**在 `llr_ai_detection_high_level_status_and_plan.md`（状态、判据、里程碑）；
> **调研证据**在 `memo_01`–`memo_09`。
>
> ★ **纪律**（继承上一个工作流）：
> - **判据预先登记**，判读前不得更改；
> - **每个数字可追溯到一次 leg / 一个 commit**；
> - **追加式**：已写下的判断不删，纠正以"更正"形式追加（保留被证伪的过程本身是资产）；
> - **开关没生效 ≡ 没有效果**——每次引用一个开关，先证明它真的动了。
>
> 版本：v1.0 ｜ 状态：**设计（未开工）** ｜ 日期：2026-10-08
> 前身文档：`AI_LLR_detection_master_plan.md`（v2.2）—— 已拆分为**本文件 + 高层文档**，
> 原章节映射见 `AI_LLR_detection_master_plan.md`（现为重定向存根）。

---

## 0. 不变式（**每一步都要满足**）

★ 这些是**贯穿全工作流的硬约束**。任何一次实作/飞行若违反其中一条，**先停下来**。
它们不是"目标"（目标见高层文档 §2），而是**做事的边界**。

| # | 不变式 | 为什么 | 谁能验证 |
|---|---|---|---|
| **I1** | ★★ **经典路径始终在链上；回退 = 不启用融合** | **"能飞腿、拿 OTA 第一手资料"是本工作流唯一的硬约束**（高层文档 §2）。任何一步让 OTA 飞不起来，都是负分 | 关掉开关后与 HEAD **bit-exact** |
| **I2** | ★★ **模型输出加扰域 LLR**（解扰保持经典、精确） | 与平台契约逐字一致（"before scrambling reversal"）；把纯计算的扰码交给模型是浪费容量 | 与经典 demapper 输出**同域对拍** |
| **I3** | ★ **正 LLR = 比特 0**；值有限；I8 不得用 −128 | 本仓库 `log_likelihood_ratio::to_hard_bit()` 的约定；也是平台契约的约定。★ 搞反会让**每个软比特反号而训练损失看起来正常** | 单测 + 决策级对拍 |
| **I4** | ★★ **一个网络**做深度 3，**不是** CE 网络 + EQ/DEM 网络 | 中间的信道估计会变成**有损瓶颈**并断掉端到端梯度；且每多一个网络多付一次 dispatch 地板 | 架构评审 |
| **I5** | ★ **采集可以慢，生产不能慢** | 穿越是**手段**，性能（首先是时延）是**目的**。采集路径可用 `scoped_debug_touches` 付费，生产路径不行 | 穿越计数（`total − debug`） |
| **I6** | ★ **配置按算法/能力命名，不按厂商 API 命名**；**能力运行时协商**，不编译期按平台名判断 | `platform_portability_design_rules.md` R9/R13；把 C 层（API 形状）误当 A 层是唯一会锁死我们的错误 | 代码评审 |
| **I7** | ★ **降级路径可查询，且降级次数可读** | 同文档 R14；静默回退是缺陷（R8） | 计数器 + 契约测试 |
| **I8** | ★★ **数值判据用"容差 + 决策级"，不用逐字节** | 同文档 R12。★ **这一条直接为 AI 路径的门禁提供了平台中立的依据**——AI 永远不可能 bit-exact | 门禁定义 |
| **I9** | ★★ **每个数字可追溯到一次 leg / 一个 commit**；判据**预先登记** | 继承上一个工作流的纪律；事后改判据等于没有判据 | 文档 |
| **I10** | ★ **开关没生效 ≡ 没有效果** | 上一个工作流为此返工三次（"旋钮从未生效"） | 每次引用开关前先证明它动了 |

### 0.1 设计边界（按 `platform_portability_design_rules.md`）

#### 0.1.1 依赖的三层分解——**只有前两层是硬的**

| 层 | 问的问题 | 例子 | 性质 |
|---|---|---|---|
| **A 硬件能力** | 这块硅**能不能**支撑这个数据流 | 统一可寻址内存、可编程设备、**必须查证的顺序语义**、值语义完成凭据、**推理引擎**、可恢复性、测量可复现性、数值范围 | **硬依赖**。缺了要重新设计 |
| **B 软件可应用性** | 这套栈**今天能不能用** | 驱动、本平台能否生成模型、引擎选择是否只是一次配置变更、可观测性 | **硬依赖，但会变**（时滞） |
| **C API 形状** | 调用的**写法** | `MTLBuffer` vs `cudaMalloc`；`.mlmodelc` vs `.onnx` | ★★ **不是依赖**。必须隔离在实现目录内 |

> ★★ **把 C 层误当 A 层，是唯一会把我们锁死的错误。**

⇒ ★ **本工作流的架构约束**（照抄该文档的判据 P0）：
**换引擎/换后端所需改动的文件，应当全部落在实现目录内**；若为了换后端而修改了
`include/ocudu/phy/` 下的判据、契约、模式、接口或配置语义，那是一次**架构回退**，不是移植。

⇒ ★ **具体到本工作流的四条**：
1. **配置与开关按"算法/能力"命名，不按厂商 API 命名**（R9）——
   例如用 `pusch_detector_backend = "ai"` + 能力协商，而不是 `"coreml"` / `"ane"`；
2. **能力在运行时协商**，不在编译期按平台名判断（R13）；
3. **降级路径必须可查询，且降级次数可读**（R14）——我们的回退计数器就是它；
4. **数值判据用"容差 + 决策级"，不用逐字节**（★★ **R12**）——
   ★ 这一条**直接为 AI 路径的门禁提供了平台中立的依据**：AI 永远不可能 bit-exact，
   而 R12 早就规定数值判据应当是容差+决策级的。**我们不需要为 AI 新造一套门禁哲学，只需要正确地用 R12。**

#### 0.1.2 ★★ 不被当前平台的能力限制

当前平台是 macOS / M4 Pro，但**架构设计不能以它的能力为界**。该文档已经量出 Apple 在
**C4③（提交内可见性）、C7（线程亲和）、C10（GPU 可恢复性）、C11（测量可复现性）四项上是负分**，
且 **C12（设备端 fp64）从语言层面不存在**；而 Linux 在 C7/C11 上**直接更优**。
⇒ ★ **"移植"不是单向下坡**：候选平台在若干项上会比现状更好。

⇒ **本工作流的做法**：
- 契约、判据、配置语义**按能力表述**（"一个独立调度的推理引擎"而不是"ANE"）；
- 当前实现落在实现目录内，**不代表架构的天花板**；
- 记录"当前平台多给了什么、少给了什么"，而不是把当前的数字写成设计的边界。

#### 0.1.3 ★★★ 唯一的硬约束：**能飞腿实验、拿 OTA 第一手资料**

> 用户裁定（2026-10-08）：**"主要的限制是要能够进行飞腿实验，拿到 OTA 的第一手资料，
> 而不是像学术论文那样，仅仅是仿真结果。"**

★★ **这条约束的含义**（不是"性能目标"，是**方法论目标**）：
- **链路必须在每一步都保持 OTA 可飞** ⇒ 我们的"**经典路径始终在链上、回退只是不启用融合**"
  不只是稳健性设计，而是**这条约束的直接要求**；
- **任何让 OTA 飞不起来的"架构先进性"都是负分**；
- 判据的**最终裁决者是空口数据**，不是仿真曲线——这与 `memo_03` §6 认定的
  "本工作流的贡献是硬件实测 + 真实 LDPC 在环 + 诚实校准"完全一致。

#### 0.1.4 ★★ 穿越是手段，不是目的

> 用户裁定（2026-10-08）：**"采集路径可以（取回设备缓冲），但最后的生产路径应该尽可能少
> （主要考量是时间延迟，如果能够在架构上有其他方面的收益也可以，根据最终性能综合评估）。"**

⇒ ★ **`phy_pipeline_crossings` 的"零穿越"是手段，目的是性能（首先是时延）。**
- **采集/调试路径**：穿越**允许**，且已有现成机制把它从**被裁决的**计数里分离出来
  （`phy_pipeline_crossings::scoped_debug_touches`，`include/ocudu/phy/phy_pipeline_crossings.h:218`；
  被裁决数 = `total − debug`，两者永远一致）；
- **生产路径**：穿越**尽可能少**，但**若有其它架构收益，按最终综合性能评估**——
  即"少穿越"不是不可违反的教条，而是**当前最优的工程选择**。

★ 这条澄清避免了两种错误：① 为了"零穿越"而拒绝一切采集；② 为了"架构优美"而无视时延。

#### 0.1.5 ★★★ 一条改变 latency 目标的事实：**500 µs 不是本系统的绑定约束**

> 用户指出（2026-10-08）：**"metal LDPC 的时间消耗高达 3000 µs 以上，已经远超 500 µs 的 slot 长度，
> 但是我们的手机 OTA 还是能够成功起飞。"**

★★ **这是一条第一手的实测事实，而且它推翻了一个隐含前提。**
本仓库此前所有"时隙预算"的讨论都默认 500 µs 是硬约束，但**系统在 3000+ µs 的 LDPC 下仍然工作**。

⇒ **三条推论**：
1. ★★ **本试验台的"实时"不是产品级硬实时**：UE 侧容忍了这个延迟（HARQ/处理能力/测试配置），
   所以**绑定约束不是"某个 µs 数"，而是"是否劣化 OTA 结果"**。
2. ★★ 这与平台自己的表述一致：`pusch_dapp_demodulator.h` 逐字写着死线
   *"**Not a 3GPP slot deadline**"*，默认 **2000 µs**（因为 500/1000 µs 在 4-UE 空口下太紧）。
3. ★★ **G5 判据据此改写**：从"p99 ≤ 某个 µs 数"改为
   **"① OTA 结果不劣化（CRC/BLER 是最终裁决）；② 给出发动机时延的完整分布（含同步）；③ 若某段确实超出时隙，如实记录它为何仍能工作。"**

★ **但不要因此放弃时延工程**：时延仍是 G-A 的第一目标，只是**它的判据从"绝对值"变成"相对基线与 OTA 结果"**。
换句话说：**我们要赢的是"eqdem 的 667 µs 里有多少是真的算力"，而不是"跑进 500 µs"。**

### 0.2 明确不做

- 不做端到端（**不替换 LDPC 译码器**）。译码器保持经典，LLR 是链条的接口。
- 不重构导频/参考信号，不动下行，不动 CE（CE 线由 `ai_ce` 独立负责）。
- 不追求"超过经典"作为 P0 之前的目标。

---

## 1. 接口契约

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

⇒ **本设计的选型（路线 a）**：net **附带输出信道估计与噪声**，经典测量核**跑在 net 的输出上**。
理由：上报口径不变（对调度器零影响），同时把最贵的那部分算力移走。

### 1.1 ★★★ 冻结项：接口契约以**生产契约**为准（`memo_06` §1）

平台的正规契约（`include/ocudu/dapp/use_cases/v1/receiver.h`）与我们的设计**逐项一致**，
因此**直接以它冻结**，而不是自创：

**输入**（`ocudu_dapp_receiver_input_v1`）：

| 张量 | 元素类型 | rank | 布局 | 含义 |
|---|---|---|---|---|
| `rx_grid` | ★ **CBF16**（复数 bf16） | 3 | `[port, symbol, subcarrier]` | DFT 后的接收网格（整槽） |
| `dmrs.reference_symbols` | **CF32** | 2 | `[layer, pilot]` | **已知的 DM-RS 符号值** |
| ★ `dmrs.coordinates` | **U16** | 2 | `[pilot, 2]` = (symbol, subcarrier) | ★ **每个导频的时频坐标 = "几何"** |
| ★ `pusch.data_re_indices` | **U32** | 1 | `..._PUSCH_DATA_RE_INDICES_U32_V1` | ★ **要出 LLR 的 data RE 索引** |
| （另）`dmrs` 的 OFDM 符号掩码 | — | — | 在 `reserved[0]` 里 | DM-RS 的符号位置 |

**准入包络**：1–2 层、1–64 端口、DM-RS type 1/2、`nof_pilots ≤ 4096`、data-RE 选择完整、
必需 flags `LAYER_COMPLETE | COORDINATES_SHARED_ACROSS_LAYERS`。

**输出**（`ocudu_dapp_receiver_output_v1`）：

| 字段 | 契约（逐字要点） |
|---|---|
| `llrs` | ★ **"soft-bit tensor before scrambling reversal"**；**bit 最快变化**；**正 = 比特 0，负 = 比特 1**；值必须有限；**I8 不得用 −128**。★ **host = `I8 [data_re × Qm]`；accelerator = `F16 [data_re, layer, bit]`** |
| `metrics.post_equalization_noise` | ★★ **F32 `[data_re, layer]`——主机 SINR 的唯一来源**（见 §2.3） |
| `metrics.equalized_symbols` | 可选 CF32；★ 官方明确允许*"direct-bit neural receivers without an explicit symbol estimate"*不填 |
| `reserved0` | 有效位 flags（`..._METRICS_VALID_V1` / `..._EQUALIZED_SYMBOLS_VALID_V1`） |
| `confidence_q16` | 存在，但★ **平台明文"the v1 host does not gate decoding on it"** |

**我们仓库里对应的两处替换点**（自建接缝时）：
`pusch_processor_impl.cpp:264` `estimator.estimate(notifier, grid, ch_est_config)` **+**
`:559` `demodulator.demodulate(buffer, notifier, grid, est_results, demod_config)`。

★ **可直接复用的既有事实**（`memo_01` §1.2、§2、§3）：`ch_est_list::device_slice` 零拷贝思路、
LLR 量化（`LLR_MAX=120`、`range_limit` 24/24/20/20）、**模型输出加扰域 LLR**、
后端开关形状（`upper_phy_factories.cpp:743-756`）。

### 1.1bis ★ 两个必须显式决策的接口点

| # | 决策 | 选项 | 建议 |
|---|---|---|---|
| 1 | ★★ **LLR 位宽** | 契约：host `I8` / accelerator `F16`；**我们链上是 int8** | 走 accelerator 就面对 F16 ⇒ ★ **在 P4 之前定**；见 §3.4 的"LLR-vs-精度曲线"正好可以裁决它 |
| 2 | ★ ★ **输入是否含"导频处 LS 估计"** | 工业形态（`neural_rx.onnx`）**含**；平台的 `dmrs.reference_symbols` 给了**已知导频值**，LS 估计要自己算 | ★ **采用工业形态**：喂 LS 估计（见 §2.2.1） |

### 1.2 契约测试（是单测，不是文档）

1. **ALL-OR-NOTHING**：谓词为真时调用方**不得**再 demap 该符号（构造一个会断言二次 demap 的假 demapper）。
2. **谓词是 shape 的属性**：同一 shape 下对任意数值输入，谓词结果恒定。
3. **`eq_noise_vars` 非空且合理**：与经典后均衡噪声方差在同一量级（逐 RE 相对误差统计）。
4. **量化边界**：±120 裁剪、±127 语义、`quantize` 的 mid-tread 步长。
5. **加扰域一致性**：把经典 demapper 的输出与模型输出放在**同一域**对拍（同为加扰域）。
6. **回退等价**：开关关闭时，与 HEAD 的经典路径 **bit-exact**（用 `ab_dumps.sh` 的 0 差异控制臂）。
7. ★ **几何张量自检**：`dmrs.coordinates` 的 (symbol, subcarrier) 与 `pusch.data_re_indices`
   必须与**经典路径实际使用的 RE 集合**逐项一致（防止"少算/多算 RE"这类静默错误）。
8. ★ **LLR 符号约定**：正 = 比特 0。★ 这是一个"训练全绿、译码全崩"级别的坑
   （`memo_01` §2 的 BCE 恒等式那一节）。
9. ★ **fence 纪律**（照 `memo_06` §2bis 采纳）：**实现即使失败或抛异常也必须记录完成 fence**
   （可能已入队工作）；失败**毒化**该 slot；迟到用**事件查询**而非等待。
10. ★ **不越界持有借来的张量指针**：不得超出本次调用的完成事件存活。

### 1.3 ★★ 路线选择：**自建接缝** 还是 **采用平台的 dApp ABI**（v2.0 重写）

`memo_06` 的代码调研把这个问题从"要不要走 dApp"变成了一个**具体的工程取舍**：

| | 路线 A：**自建接缝**（默认） | 路线 B：**采用平台 dApp ABI** |
|---|---|---|
| 做法 | 在我们 fork 里把 `estimator.estimate` + `demodulator.demodulate` 两处合并成一个 AI 后端 | vendored `ocudu-dapp-sdk`（3.4 M，明文"不复制 OCUDU ABI 头、不要求 OCUDU 源码树"），实现一个 Class-A 模块 |
| 契约 | **照抄 `receiver.h` 的字段语义**（不引入 C ABI） | 直接用平台 C ABI + 生命周期 + 打包 + 验证阶梯 |
| 成本 | 自己写接缝 + 自己写模型生命周期 | ★ **要改平台运行时**（见下） |
| 收益 | 立刻可做；无外部依赖 | ★ 与上游同构：签名包、SBOM、ABI 指纹门、模型 stage/warm/activate、验证阶梯**全部现成** |

★★ **采用平台 ABI 的两个结构性阻塞**（`memo_06` §4ter，不是接线问题）：

| # | 阻塞 | 后果 |
|---|---|---|
| 1 | `include/ocudu/dapp/management/types.h:29` 有 `other_accelerator`，**但** `instance_manager_helpers.inc:42-43` 把它映射成 `std::nullopt`，且 `instance_manager_lifecycle.cpp:66` 在无 ABI backend 时**拒绝 in-process 放置** | ★ Class A **只能 in-process** ⇒ **Metal 后端的 Class A 包当前无法加载** |
| 2 | `class_a_l1_path_incompatibility`（`instance_manager_helpers.inc:284-293`）把"加速 L1"与"CUDA"**当同义词** | 同上 |

★ **但方向是对的**：`abi/v1/memory.h:36-43` **已显式枚举 `HOST / CUDA / HIP / SYCL`** 并自述 backend-neutral
⇒ **Metal 是"第五个 id"，不是逆着契约来**。

★★ **本设计的裁决（v2.0）**：
1. **主线走路线 A（自建接缝）**——立刻可做，不被平台的 CUDA 绑定卡住；
2. **但契约字段、生命周期机制、fence 纪律、验证阶梯全部按路线 B 的形态设计**，
   使得将来若平台开放非 CUDA 后端，我们**只需换壳**；
3. ★ **引入 `ocudu-dapp-sdk` 作为"规范参照"而非依赖**：读它的参考实现与文档，不链接。
4. ⚠ **许可差异**：他们是 **BSD-3-Clause-Clear**（含专利条款），我们是 BSD-3-Clause-Open-MPI；
   vendoring 前必须过一遍。

★ **仍然成立的两条硬事实**（`memo_03` §3.1）：
① 外部 dApp 框架对神经接收机 **Inexpressible**（没有把 LLR 张量送回同一槽 PUSCH 链的回路）⇒ **必须内联**；
② 内联避免把每槽 MB 级搬运加回一条以"零主机↔设备数据穿越"为核心成就的 lane（`phy_pipeline_crossings.h`）。

#### 1.3bis ★★ 可复用 / 需重写的清单（路线 B 若启动时用）

| | 内容 |
|---|---|
| ✅ **原样复用** | **全部** `include/ocudu/dapp/abi/v1/*.h` 与 `use_cases/v1/*.h`（**纯 C**）；`circuit_breaker.cpp`、`incident*.cpp`、`guarded_call.h`、`instance_runtime.h`；`module_loader.cpp`、`abi_validation.cpp`、`native_worker.cpp`、`native_hook_point.cpp`、`native_hook_slot.cpp`、`native_instance.cpp`、`instance_manager*.cpp`、`package_*.cpp`、`artifact_verifier.cpp`、`lifecycle.cpp`；`pusch_dapp_demodulator.cpp`、`native_pusch_dapp_demodulator_target.cpp`、`pusch_route_table.cpp`、`upper_phy_dapp_*.cpp` |
| ❌ **必须重写** | `lib/phy/upper/dapp/pusch_resident_dapp.cpp`（**1401 行热适配器**：stream→`cudaStream_t`、`cudaMemcpyAsync`、`cudaEventRecord`，整体在 `#ifdef ENABLE_CUDA` 下）；`native_receiver_resident_adapter.cpp:59-67`（硬编码 `MEM_ACCELERATOR_DEVICE_V1` + `BACKEND_CUDA_V1`）；`pusch_resident_dapp_cuda.cu/.h`、`srs_*_cuda.cu`、`class_c_cuda_export_pool.cpp`、`native_cuda_control_context.cpp`；`pusch_demodulator_gpu_impl.cpp`（**4058 行**） |

### 1.4 ★★ 逐槽信任 / 回滚（v2.0 修正：平台**没有** shadow 模式）

★★ **必须分清"文献建议"与"平台实现"**：

| 来源 | 做法 |
|---|---|
| ★ **文献建议**（calibration-drift, arXiv 2605.26157） | 逐时隙**神经+经典并行仲裁**，代价 <5% 时延；因为 **500 Hz Doppler 下经典先崩**，"AI 差就退回经典"是错的 |
| ★★ **平台实现**（`memo_06` §2quater） | ★ **没有 shadow / 没有对比模式 / 没有基于 LLR 置信度的门控**。回退**严格只在失败或 profile 被拒时**发生 |

⇒ **本设计的裁决**：
1. **近期按平台形态做**（失败即回退），因为它**简单、可验证、且是生产已验证的**；
2. ★ **但明确记录：shadow/并行仲裁要我们自己建**，它不是"平台能力"；
3. ★ `confidence_q16` **平台不用于 gate** ⇒ 置信度门控若要做，也是我们自己的机制；
4. **回退/仲裁率必须与收益一起报**（只报收益 = 不诚实的比较）。

**参考实现的具体机制**（`memo_06` §2quater，可直接抄）：
- `breaker_policy{ recoverable_failure_threshold{8} }`，**第 8 次**连续可恢复失败打开；
- ★ **迟到确实喂 breaker**（`native_worker.cpp:932`），但**计数器共享**（deadline/invalid-input/invalid-output/resource 同一个 latch）；
- **按 lane 独立**；恢复**只能靠运维探针**（`begin_authorized_probe()` → half_open → `close_after_successful_probe()`）；
- 打开期间 `guarded_call.h:29` 返回 `OCUDU_DAPP_BYPASS_V1`；
- ★★ **completion 契约在生产里未启用**（`host_capabilities{}` 从不填充，加载器拒绝需要它的接收机）⇒ **生产是 invoke-only**。

## 2. 模型设计：一个网络做深度 3

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
| ★ **导频处的 LS 信道估计** | `[2, N_dmrs_re, layers, ports]`（实/虚） | ★ **工业参考形态的必备输入**（见 §2.2.1） |

★ **导频掩码显式输入是关键设计选择**：DeepRx 的做法就是 *"constructing the input … in a very
specific manner using both the data and pilot symbols"*，而它的性能被归因于
*"learning to utilize the known constellation points of the unknown data symbols, together with the
local symbol distribution"*。让网络知道"哪些是已知的、哪些是待判的"，是这件事成立的前提。

★ **接缝已经提供了全部这些**（`memo_01` §1.1）：`resource_grid_reader& grid` +
`dmrs_pusch_estimator::configuration`（`symbols_mask` / `crb_bitmap` / `first_symbol` / `scaling`）+
`pusch_demodulator::configuration`（`rb_mask` / `modulation` / `nof_symbols` / `dmrs_symb_pos` /
`dmrs_type` / `nof_cdm_groups_without_data` / `n_id` / `nof_tx_layers` / `dc_position` / `rx_ports`）。

#### 2.2.1 ★★ 工业参考形态：NVIDIA `neural_rx.onnx` 的精确张量规格（`memo_05` §1.2）

这是一份**可直接对照**的工业实现（该 repo 的 `pyaerial/models/` 里就带着 ONNX 权重）：

| 张量 | 形状（不含 batch） | dtype | 含义 |
|---|---|---|---|
| `rx_slot_real` / `rx_slot_imag` | `(3276, 12, 4)` | fp32 | 接收网格 `[子载波, 符号, 端口]` |
| ★ `h_hat_real` / `h_hat_imag` | `(4914, 1, 4)` | fp32 | **导频处的 LS 信道估计** |
| `active_dmrs_ports` | `(1,)` | fp32 | 激活 DM-RS 端口数 |
| ★ `dmrs_ofdm_pos` | `(3,)` | **int32** | **DM-RS 的 OFDM 符号位置** |
| ★ `dmrs_subcarrier_pos` | `(6,)` | **int32** | **DM-RS 在 PRB 内的子载波位置**（`{0,2,4,6,8,10}`） |
| `output_1` | `(8,1,3276,12)` | fp32 | **LLR** `[Qm, 层, 子载波, 符号]` |
| `output_2` | — | fp32 | 另一路读出（图中有 `readout_ch_est` 分支） |

- 位置核验：3276 = 273 PRB × 12 ✓；**4914 = 3 × 1638 = 3 个 DM-RS 符号 × (3276/2)** ✓
  ⇒ **type-1 DM-RS comb-2 图案**，与 `dmrs_subcarrier_pos` 一致 ✓。
- ★ 论文原文（`example_neural_receiver.ipynb`）：*"the neural network is used to **replace channel
  estimation, noise and interference estimation and channel equalization**, and thus outputs
  log-likelihood ratios directly … **Also, the neural receiver takes LS channel estimates as inputs**"*。
- ★ **参数 145 232 ≈ 1.45e5**，与 §2.6 引用的"实时模型 1.4e5 权重"**完全吻合**。

⇒ **本设计的输入契约据此定为**：网格 I/Q + **导频 LS 估计** + **DM-RS 符号位置** + **DM-RS 子载波位置**
（+ §2.2 的元数据）。**深度 3 = 学习"插值 + 噪声估计 + 均衡 + 解映射"的联合，而不是从零学信道估计。**

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
| ★★ 辅助头 1 | **后均衡噪声方差** | ★ **不是可选**——主机**只**从这里推调度器 SINR（`SINR_dB = -10log10(mean σ²_post)`），且常规 CSI 核在该路径已被跳过；★ **低噪声区不得输出近零常数**（会算出极大 SINR）。权重小但**必须存在** |
| 辅助头 2 | 信道估计 + 噪声 | NMSE（权重小），且**供经典测量核** |

★★ **深度 3 的硬需求（`memo_06` §4bis）**：① **逐 RE 后均衡噪声必须输出**（否则上行链路自适应失去输入）；
② **模型产物体积上限 1 MiB**（现模型 290 KB fp16，在限内）；
③ 平台的后端枚举封闭（新增 Metal backend 要改主机头）。

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
| **G 门禁范式** | 直接沿用（G1–G5 → 本设计的 G0–G6） |
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

## 3. 训练、损失与语料

### 3.1 ★★ 语料策略（v2.1 重写：**语料不是约束**）

> 用户裁定（2026-10-08）：**"语料可以补充采集，并且在代价许可的范围内可以不受限。"**

★★ **这改变了本工作流的数据策略的**起点**。`memo_04` §3 把"17.7 万次接收"当作规模上界来论证；
现在正确的表述是：**那批采集是"已有的"，不是"可用的全部"**。语料的规模与覆盖由**部署环境的需要**决定，
而不是由"我们手头有什么"决定。

#### 3.1.0 ★★ 为什么这条如此重要：AI CE 的最大教训

> 用户原话：**"我们在 AI CE 里面踩过最大的坑就是'训练环境不等于部署环境'，
> 以至于得到负面的结论（其实不是负面，只是我们没有更进一步投入资源深挖）。"**

★★ **这条必须被正确地记住**，因为它同时包含一个技术教训和一个方法论教训：

| | 教训 |
|---|---|
| **技术** | **训练分布 ≠ 部署分布** 是 AI PHY 的头号失效模式。文献独立佐证：Nokia OTA 研究发现 LOS 训练的 DeepRx *"failed the over-the-air tests despite converging well during training"*，且**宽随机化胜过参数匹配**（`memo_03` §7bis.G.4） |
| **方法论** | ★★ **那个"负面结论"的成因是投入不足，不是方法失败。** 本仓库文档此前把 "经典 ≥ HELENA 于所有实测区间" 当作**结论**引用——**这个引用方式本身就是那个坑的一部分**。正确表述是：**在当时的语料与投入下，未观察到增益** |

⇒ ★★ **对本工作流的直接要求**：**语料设计必须先于模型设计**，而且它的目标是
**覆盖部署环境**，不是"够大"。见 §3.1.2。

#### 3.1.1 标签

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

#### 3.1.2 ★★ 语料设计的三条要求（P1 的判据）

| # | 要求 | 为什么 |
|---|---|---|
| **1** | ★★ **覆盖部署分布，而非复用现有采集** | 采集的**维度**要覆盖我们实测到的工作点（`memo_04` §2：15–25 dB、25–51 PRB、16/64/256QAM、SISO、DM-RS {2,7,11}），**并且**要覆盖**边界与分布外**情形（低 SNR、窄分配、少导频、msgA、高 Doppler） |
| **2** | ★★ **采集时同步记录训练所需的全部字段** | `memo_04` §5 已列出缺口：`n_rapid`、`dmrs_type`、`nof_cdm_groups_without_data`、逐 RE 后均衡噪声方差。**这些字段如果不在采集时记录，事后再也补不回来** |
| **3** | ★★ **"仿真 vs OTA"必须是受控变量** | `memo_03` §4.3 的负面证据：*Input-Correlated Supervision Noise Limits the Benefits of OTA Training*（arXiv 2608.12918）。⇒ **两条都跑，同一测试集**，不得默认 OTA 更好 |

★ 补充：**"能飞腿实验"这一条（§0.2(3)）意味着采集可以与飞行绑定**——
每一腿都是一次真实空口采集的机会，这比离线攒语料更贴近部署环境，也是本工作流相对论文的根本优势。

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

★★ **代码级更正（`memo_05` §2.1–§2.2）**：
- **cuPHY 全仓没有 `RANGE_LIMIT`**，也没有逐调制 LLR 标度——它按噪声方差**自然标度且不裁**
  （`LLR = 2·A·z/σ²_PAM`）。⇒ ★ **`range_limit` 是"我们的"设计，不是业界标准**；
  模型的输出标度**只能对准我们自己的量化器**，没有可照抄的"业界标度"。
- **NVIDIA 出货的 LDPC SPI 只接受 fp16**（*"Only ::CUPHY_R_16F is supported"*），
  虽然类型枚举里有 fp8 且存在 fp8 代码路径，但**被 SPI 拒绝** ⇒ "fp8 LLR" 参考价值下降。
- **LDPC `clamp_value` 默认 32.0**，规则是**严格小于**类型 max finite（fp16 上界 65504）。
  ★ 32.0 这个默认值说明**典型 LLR 幅度在"几十"量级**——可作我们标度校准的合理性检查。
- ★ 他们还有**两级裁剪**：LDPC 输入 clamp（默认 32）+ rate-match 后的 HARQ buffer clamp（±10000）。
- ★ LLR 布局与我们**同约定**（`[re][bit]`、bit 最快变化）；差别只在**他们恒 pad 到 8**、我们不 pad。

★ **fp16 的约束是"量程"不是"尾数"**：朴素 fp16 流水线曾因中间量达到 **5e6 ≫ 65504** 而**全 NaN**，
用 **1/N 块浮点缩放**才修好（arXiv 2605.28451）。
⇒ **LLR 头必须有显式 clamp / tanh，绝不能接近 65504**——这与 §3.3 的标度校准是同一件事的两面。
★ 并且 **没有任何人发表过 LLR 的"精度-性能"曲线**（`memo_03` §7bis.C）——
**这条曲线本身就是本工作流的一个可交付结果**。
★ 参照 `memo_05` §2.2，建议的对比臂至少三条：**int8-uniform（我们的链） / fp16（NVIDIA 出货口径） /
fp8-E4M3（存在但被拒的路径）**。

### 3.5 数据划分（★ 曾吃过大亏的地方）

**必须按采集时段/会话划分，禁止随机划分。** 同一信道的相邻 slot 若同时出现在训练集与测试集，
会产生严重泄漏，指标会好看而部署会崩——这正是 AI CE "训练/部署错配"那一类错误的近亲。
划分脚本要**检查并打印**：训练/测试集的 slot 范围不重叠、`rnti` 不重叠、`n_prb`/`mod`/SNR 分布对照表。

### 3.6 增强

只能在**物理上成立**的维度上做增强：噪声重采样（人工加噪到目标 SNR）、相位/定时扰动、
功率缩放。**禁止**做会改变标签的增强（例如随机擦除数据 RE 而不更新标签）。

---

## 4. 阶段与门禁 G0–G6

| 阶段 | 内容 | 门禁 | 预登记判据 |
|---|---|---|---|
| **P0** | **值不值得做**：经典链的导频/SNR 扫描 + genie 上界 | **G0** | 给出"检测环节可改善空间"的**量化上界**（memo 04 §4 的 P0-a/b/c）。**若上界很小 ⇒ 工作流只保留 G-A（时延）目标，或终止** |
| **P1** | 离线数据管线：grid + TB → 训练集（划分、增强、统计、复现脚本） | G1 | 数据集可一键复现；泄漏检查通过；**经典链在同一测试集上的 SER/BLER 已记录**（否则后面没有可比基线） |
| **P2** | ★ **单个联合网络（深度 3）离线训练** | G2 | ★ **主指标 = 真实 LDPC 译码器之后的 coded BLER**（**互信息不能预测 BLER**，`memo_03` §7bis.F）；BER/MI 仅作诊断。标度校准后 CRC 不掉 |
| **P3** | 联合网络扩展：覆盖 16/64/256QAM + 多任务头 + 消融臂 | G3 | 同 G2（coded BLER），且**逐调制**分别达标；给出参数量/FLOPs 与 ★ **ANE 实测时延曲线**（对照臂：MPS）、**量化曲线**（8-bit 应基本免费；INT4 预计不可用） |
| **P4** | **接链**：实现 `channel_equalizer` 后端 + 谓词 + 回退 + 契约测试 + crossings 声明 | G4 | §1.1 六项单测全过；开关关闭时与经典 **bit-exact**；开关打开时 CRC ≥ 参考 |
| **P5** | 实时性：端到端时延 + **同步开销** | G5 | ★★ **判据已按 §0.2(4) 改写**：**① OTA 结果不劣化（CRC/BLER 是最终裁决）；② 给出发动机时延的完整分布（含同步）；③ 若某段超出时隙，如实记录它为何仍能工作。** 原"p99 ≤ 某个 µs 数"的表征 **采用 dApp 论文的 Class A 契约**（`memo_03` §2.1）：**驻留接收链、零拷贝设备张量、完成 ≤150 µs**。三个对标基线：① 现网 eqdem **667.4 µs**（mkf033 中位）；② dApp 的 neural-receiver→LLR **≤500 µs 槽占用**；③ NVIDIA GB10 接收机 kernels **82 µs P50 / 112 µs P99.9**（273 PRB 4 端口，我们体量的 ~21 倍，**不可直接套用**）。★ **必须包含 GPU→模型 的等待与 模型→LDPC 的可见性开销**，不得只报前向时间；**必须报 dispatch 次数** |
| **P6** | OTA 实测：真实采集上的 CRC/BLER A/B | G6 | 统计门通过（同批采集、同一译码器、同一 LDPC 配置），**且必须同时报逐槽回滚率**（§1.4） |

### 4.1 借用平台的验证阶梯与 ABI 门禁（`memo_06` §5.1）

| 机制 | 内容 | 我们怎么用 |
|---|---|---|
| **三级阶梯** | `packaging → algorithm → system` | 我们的 G1–G6 与之对齐：打包（P4）、算法（P2/P3）、系统（P5/P6） |
| ★★ **官方承认深度 3 的证据不在开放阶梯内** | 逐字：*"**Vendor-only (not in open ladder): a proprietary neural receiver vs the conventional receiver, side-by-side SER.**"* | ★ **这份 side-by-side SER 证据正是我们的 G6**,要自己产出 |
| ★ **ABI 编译器矩阵门** | 在 **GCC × Clang × C11 × C++17** 下编译公共 C ABI，要求与冻结的 **LP64 布局指纹**逐字节一致 | ★ 我们若定义跨引擎接口，值得引入同等机制 |
| **certifier 覆盖** | 生命周期、caller-owned 输出、golden/canary、六种 shape、全 QAM × DM-RS 1/2、模型生命周期 | 我们的 P4 契约测试应向这个覆盖面看齐 |
| ★ **安全边界定位** | 逐字：*"A malicious or memory-unsafe Class A/B module can compromise the DU."*；*"E3 … is not an acceptable Class A/B tensor transport."* | 若将来做模块化，**签名 = 出处证明，不是沙箱** |

### 4.2 G5 的死线必须带争用条件（别人踩过的坑）

平台的 `demodulator_deadline` 默认 **2000 µs**，注释逐字记录：

> *"**500 µs (one 30 kHz slot) and then 1000 µs both still classified good PBC results as late under
> 4-UE OTA contention; 2000 µs keeps the tripwire without disabling a working EQ.**"*

⇒ ★ **我们的 G5 判据不能只写一个 µs 数**，必须写明：**几 UE、什么负载、哪个 stream 上的争用**。
否则会重演"死线定得比实现能力还紧，把能工作的东西判成失败"。

⚠ 一处待核实的数值不一致：SDK 文档写 CE 预算 **~300 µs**（`validation_ladder.md:49`），
平台头文件 `channel_estimator_deadline` 默认 **800 µs**。引用前必须核准。

★ 门禁纪律（继承自 `metal_kernel_fusion`，都是付过学费的）：

1. **判据在飞之前写死**；事后改判据等于没有判据。
2. **计数器必须能证明开关真的生效**（"开关没生效 ≡ 没有效果"在本仓库出现过三次）。
3. **每个数字都要能追溯到一次 leg / 一个 commit**；引用别人的数字要标来源等级。
4. **禁止用两个测量相减做归因**（本仓库为此错过两次）。

---

## 5. 风险清单

| # | 风险 | 早期信号 | 退路 |
|---|---|---|---|
| **R1** | **精度收益不存在**（工作点在经典链已近最优的区间） | G0 的可改善空间上界很小 | 工作流收缩到 G-A（时延）；或终止并如实记录 |
| **R2** | 模型太大 / **每跳超过 1 次 dispatch**，跑不动 | P2 的宽度-层数-时延曲线超预算；或每跳 dispatch 次数 > 1 | 缩模型/砍层；**保证整跳一次前向**；按符号/子带分块（§2.6） |
| **R3** | **判决反馈的串行性**与实时冲突 | DF 轮数 × 单轮时延 > slot 预算 | 无 DF 的一轮并行检测（A2 本身就没有 DF） |
| **R4** | **LLR 标度/量化不匹配**：离线指标好、CRC 崩 | 量化前后 CRC 差异大 | §3.3 标度校准；逐调制重标定 `range_limit` |
| **R5** | ★★ **训练/部署错配**（AI CE 的头号坑，且**当年的"负面结论"实为投入不足**） | 仿真好、OTA 差 | ★★ **语料设计先行**（§3.1.2）：**覆盖部署分布 + 采集期记录全部字段 + 仿真/OTA 作受控变量**；辅以"把适配写进输入格式"（DEFINED 的 in-context 思路，`memo_02` §3.1）与 OTA 微调 |
| **R6** | **打破融合 lane 的"一次提交"结构** | crossings 计数上升、`gap` 变大 | 显式声明新同步点；或改走 dApp 路线 D——但那时要把 **1.47/4.4 MB 的每槽搬运**计入 G5（§1.4） |
| **R7** | ~~语料不足~~ → ★ **已解除**（v2.1：语料可补充采集、代价许可内不受限） | — | 改为风险 R7′：**语料设计不当**（覆盖不到部署分布、采集时漏记字段）——见 §3.1.2 的三条要求 |
| **R8** | 与 CUDA/NVIDIA 路线相比无优势 | 竞品分析 | 如实记录（仓库已有 `metal_vs_cuda_architecture.md` 的方法论） |
| **R10** | ★★ **采用平台 ABI 时 Metal 后端无法加载** | 尝试走路线 B 时模块被运行时拒绝 | 路线 A 自建接缝；或改平台的 `other_accelerator` 映射与放置校验（`memo_06` §4ter） |
| **R11** | ★ **模型产物超 1 MiB** 被拒 | 权重文件 > 1 MiB | 现模型 290 KB fp16 在限内；增长时抬高上限或走带外资源 |
| **R12** | ★ **噪声头输出近零常数** ⇒ 主机算出极大的 SINR ⇒ 调度器被带偏 | SINR 上报异常偏高 | 写成 G3 的**数值判据**（"低噪声区不得输出近零常数"） |
| **R13** | ★ **completion 契约在生产里未启用** | 依赖 completion 的设计无法上生产 | 按 **invoke-only** 设计主线；completion 只作未来选项 |
| **R14** | ★ **没有 shadow 模式**，无法逐槽对比 | 无法量化"AI vs 经典"的逐槽差异 | ★ **shadow 自己建**（这是本工作流的一项额外工程，不是平台能力） |
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
| 文档 | 本目录 memo 01–04 + 本设计 + 每阶段的测量记录（沿用 leg 协议与日志目录约定） |
| 门禁证据 | G0–G6 的测量记录，每个数字可追溯到一次运行 |

## 7. 与上一个工作流的关系

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

## 8. 尚未确认的部分

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

---

## 9. 开工前功课与判据

> 用户裁定（2026-10-08）：**不要急于开工；把功课做足。**

### 9.1 已完成的调研（可作为事实基础）

| # | 范围 | 产出 | 状态 |
|---|---|---|---|
| 1 | **仓库自身的接缝与 LLR 契约** | `memo_01` | ✅ 代码级 |
| 2 | **参考论文 DEFINED** | `memo_02` | ✅ 全文级 |
| 3 | **公开文献 7 条线** | `memo_03` + `survey/`（11 份原始材料 + 38 篇 PDF） | ✅ 全文级 |
| 4 | **数据、标签与工作点实测** | `memo_04` | ✅ **本仓库实测** |
| 5 | **NVIDIA `aerial-cuda-accelerated-ran`** | `memo_05` | ✅ 代码级（含两个训练好的 ONNX） |
| 6 | **OCUDU dApp 平台 5 仓** | `memo_06` | ✅ 代码级（契约、接缝、fence、生命周期、可移植性） |

### 9.2 ★ 开工前的功课清单（按优先级）

#### A. 必做（不做则无法开工）

| # | 功课 | 为什么必须 | 预计产出 |
|---|---|---|---|
| **A1** | ★★ **我们仓库侧的"上报路径"彻底摸清**：`est_results.get_channel_state_information(...)` 的**每一个消费字段**（RSRP/EPRE/SNR/TA/CFO）分别被谁读、精度要求、是否影响后续槽 | 深度 3 继承 CE 的全部上报义务；平台已承认这是缺口（`memo_06` §4）。**不知道消费者，就不知道噪声头要输出什么** | ★ **计划产出**：`memo_07_reporting_obligations.md`（**尚未落盘**） |
| **A2** | ★★ **`resource_grid_reader` 的内存布局与零拷贝可行性**：port/symbol/subcarrier 的物理排布、能否直接暴露成 `[port, symbol, subcarrier]` 张量（对应平台的 `RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1`） | 这是模型输入的第一步；布局不对就要拷贝，而拷贝会毁掉零拷贝成就 | 同 A1 或独立 memo |
| **A3** | ★★ **P0 的实验设计（判据预登记）**：genie 上界怎么算、用哪批数据、**经典链基线用哪一条**（CPU generic / metal mmse？）、BLER 操作点与样本量 | ★ **P0 是"值不值得做"的裁决**，而**没有定义基线就没有可比性** | ★ **计划产出**：`memo_08_p0_design.md`（**尚未落盘**；**判据必须写在飞之前**） |
| **A5** | ★★ **语料设计与采集方案** | ★ 用户已裁定**语料可补充采集且不受限**；§3.1.0 的教训说明**语料设计必须先于模型设计** | ✅ **`memo_09_corpus_design.md` v1.0**（三层结构、不可恢复字段清单、覆盖度账本、飞行绑定、迭代回路） |
| **A6** | ★★ **采集字段补齐（memo_09 §13.1）**：在"始终编译在内"的那套设施上扩展，补 `dmrs_type`、`nof_cdm_groups_without_data`、`n_rapid`、逐 RE 后均衡噪声，且不拖慢热路径 | ★ **P1 批语料的硬阻塞**；★ **不可恢复字段漏记 = 重飞** | ✅ **方案已出**：`wip/A6_capture_fields_plan.md` |
| **A4** | ★ **TA/CFO 的下游用途**：它们只被上报，还是被用于补偿/影响后续槽？ | 若被用于补偿，深度 3 的网络**必须继续产出它们**（或保留一个轻量 DM-RS 经典块）；若只上报，问题小得多 | 并入 A1 |

#### B. 应做（能显著降低返工）

| # | 功课 | 为什么 |
|---|---|---|
| **B1** | ★★ **平台的 `resource_grid_tensor_adapter.{h,cpp}` 读完** | 这是"网格 → 张量"的**官方转换**；我们必然要写一个等价物，先看别人怎么做 |
| **B2** | ★ **平台的 `pusch_resident_dapp.cpp`（1401 行）读完** | 热适配器做了哪些校验/转换/计时；这是"接缝实现"的完整样本 |
| **B3** | ★ **SDK 的 `ref_receiver_cuda/algorithm.cu` 读完** | 参考算法本体。即使不抄，也要知道**基线**是什么 |
| **B4** | ★ **平台的深度 1/2 契约读完**（`pusch_dapp_channel_estimator.h`、`ocudu_dapp_equalizer_input_v1/output_v1`） | 三个深度要一起理解，才知道选深度 3 的边界在哪 |
| **B5** | ★ **`neural_rx.onnx` 的完整 dataflow**（CGNN 结构、`readout_ll_rs` vs `readout_ch_est` 与 ONNX 输出名的对应） | ★ 待核实项：notebook 与单测的**输出形状不一致**；且它 1.45e5 参数的架构选择值得理解 |
| **B6** | ★ **`ai_train/` 工具链的能力边界**（能做什么、不能做什么、缺什么） | 决定 P1 要写多少新代码 |
| **B7** | ★ **语料的逐字段可用性核查**（`ul_capture` 五件套 vs 训练需要） | `memo_04` §5 列了缺口，但**没逐字段核** |
| **B8** | ★ **quickstart 的零硬件 E3 测试台**能不能用作我们的 E2E 环境 | 若能，P5/P6 的环境搭建成本大幅下降 |

#### C. 可选（有则更好）

| # | 功课 |
|---|---|
| C1 | 平台 `docs/dapp/` 全目录（我们只读了 SDK 侧 docs） |
| C2 | `cuPHY/examples/ch_est/torch_to_trt_chest_example.py`（端到端 PyTorch→ONNX→TRT 脚本，方法学可移植） |
| C3 | 3GPP 侧：`dmrs_type` / `nof_cdm_groups_without_data` 与我们实测分布的关系（`memo_04` §5 的缺口） |
| C4 | 我们 fork 与平台 fork 的**接缝差异**（若走路线 B，需要知道要移植多少） |

### 9.3 ★★ 开工判据（Definition of Ready）

**以下全部为真，才开始写模型代码：**

1. ☐ **A1–A4 完成**：上报义务清单、网格布局结论、P0 判据预登记、TA/CFO 结论；
2. ☐ **P0 判据已预登记且被复核**（不是"打算怎么做"，而是"写成文的、可证伪的判据"）；
3. ☐ **经典链基线已定义**（哪条链、什么配置、在什么数据上、多少样本）；
4. ☐ **LLR 位宽决策已定**（§1.1bis 决策 1）；
5. ☐ **路线 A/B 已选**（§1.3；默认 A，但要说清理由）；
6. ☐ **G0–G6 的判据全部预登记**，且每条都写明**争用条件**（§4.2）。

★ **纪律**：**功课没做完就开工，等于把"值不值得做"这个问题推迟到已经投入之后才回答**——
而这正是 `metal_kernel_fusion` 那条线用几个月换来的教训。

---

## 10. 工作目录、leg 命名与出处

★ 详细约定见 **`doc_chinese/llr_ai_detection/wip/README.md`**；这里是规划层面的三条要点。

### 10.1 目录三分（已核实忽略规则）

| 路径 | 进 git？ | 放什么 |
|---|---|---|
| `llr_ai_detection/wip/` | ✅ **进** | 工具、脚本、arm 配置（YAML）、探针 |
| `llr_ai_detection/wip/logs/` | ❌ **不进** | ★ **新飞腿的日志**（`doc_chinese/.gitignore:17` 的 `**/logs/`） |
| `llr_ai_detection/work_tmp/` | ❌ **不进** | 语料、参考二进制、仪器输出（`doc_chinese/.gitignore:25` 的 `**/work_tmp/`） |

★ **与 AI CE 历史的区别**：AI CE 的语料在 **`~/ai_ce_work/`**（仓库外），那是**历史原因**。
本工作流**统一用 `work_tmp/`**，不再往仓库外放东西。
`memo_04`/`memo_09` 里引用的 `~/ai_ce_work/capture/` 是**历史语料，只读参考、注明出处**。

★ 为什么要有 `work_tmp/`：`doc_chinese/work_tmp/README.md` 记录过一次事故——
**2026-09-16 的重启一次性带走了 237 个捕获语料、参考二进制、门脚本、上机日志**。

### 10.2 ★★ leg 命名：`aillr0NN-<suffix>`

- `mkf` 是**上一个工作流**的缩写 ⇒ 本工作流改用 **`aillr`**，**编号从 `aillr001` 重新开始**；
- 序号由 `wip/next_leg_label.sh` **从 leg 日志算出**，不靠记忆（两个序列互相独立）；
- ★ **已飞过的腿名不改**——**腿名是证据**。

### 10.3 ★★ 历史工具的出处（引用必须注明完整路径）

本设计的多个判据来自上一个工作流的工具。**在工作 memo 中引用时必须注明出处**：

| 工具 | 出处 |
|---|---|
| `fly_leg.sh` / `ul_health.sh` / `pair_check.sh` / `probes_off_syntax_check.sh` | `doc_chinese/macos_thread_priority/wip/` |
| `ab_dumps.sh` | `doc_chinese/phy_pipeline_gpu/wip/` |
| `next_leg_label.sh`（原始版） | `doc_chinese/metal_kernel_fusion/wip/` |
| 历史 leg 日志 | `phy_pipeline_gpu/wip/logs/`（1141）、`macos_thread_priority/wip/logs/`（389）、**`metal_kernel_fusion/wip/logs/`（170）** |
| 历史语料与参考二进制 | `doc_chinese/work_tmp/corpus/`（27 个合成捕获）、`doc_chinese/work_tmp/ref/` |

★ **参考二进制必须与腿同源重建**——`doc_chinese/work_tmp/README.md` 记着"同一提交、不同日期构建的产物不一样"，
离线 A/B 用错参考会得到**假红**。

---

## 11. Memo 区（**每次实作 / 飞行 / 决策追加**）

★ 追加规则：**新的在最上面**；每条注明日期、类型（实作/飞行/决策/更正）、依据（leg 名或 commit）。
**被证伪的判断不删除**——以"更正"追加，保留推理过程。

| 日期 | 类型 | 内容 | 依据 |
|---|---|---|---|
| 2026-10-08 | 文档 | 本工作流开工前的调研与设计完成，拆分为高层文档 + 本设计文档；开工前功课见 §9 | `memo_01`–`memo_09` |
