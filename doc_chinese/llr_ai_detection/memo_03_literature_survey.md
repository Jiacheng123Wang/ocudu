# 文献调研（完整版）

> 调研范围：**把 NR 上行检测链（时频网格 → 软比特 / LLR）换成 AI 模型，并在 Apple Silicon
> （CoreML / Metal / ANE）上、在真实空口 gNB（OCUDU）里跑起来**。
>
> **原始调研材料**（英文，带逐条引用与来源等级）在本目录 `survey/` 下三份：
> - `survey_master_ai_receiver_literature_review.md` — 主线综述（7 条线 + 综合）
> - `survey_line_a_model_based_mimo_detection.md` — 模型驱动/深度展开 MIMO 检测
> - `survey_line_b_llr_output_receivers.md` — **输出 LLR 的神经接收机与编码感知损失**（对本工作流最关键）
>
> 版本：v1.0（完整版，替换 v0.9 interim）｜ 日期：2026-10-07

---

## 0. 方法与可信度标注（★ 引用本文数字前必读）

调研所在的沙箱**封锁了几乎全部 `web_fetch`**（arxiv.org / IEEE / Semantic Scholar / github.com /
huggingface.co 及多数厂商域名均不可达，全部返回 "resolves to a non-public IP address"），
只有 `web_search` 可用——而它的索引**确实包含 arXiv/IEEE PDF 的全文**并能返回命中片段。

因此本文数字分三级：

| 标记 | 含义 |
|---|---|
| **【已核实片段】** | 从索引源中直接读到的原文片段（标题、venue、公式、数字） |
| **【未核实原文】** | 未能从来源读到；来自既有印象或结构性推断，**引用前必须复核** |
| **【推断】** | 调研者/本文的判断，不是论文作者的主张 |

---

## 1. ★★ 改变架构决策的发现：ANE 不能做逐符号引擎

这是本次调研对规划影响最大的一条，**直接推翻了规划 v1.0 把 ANE 当作目标引擎的隐含前提**。

| 事实 | 数字 | 来源 |
|---|---|---|
| ANE **单次 dispatch 地板**（M1） | **0.23–0.26 ms**——哪怕只是一个 ReLU / 64 元素线性层 / 小卷积 | *Apple Neural Engine: Architecture, Programming, and Performance*（Bryngelson, 2026-06, ~302 页），arXiv [2606.22283](https://arxiv.org/abs/2606.22283) |
| ANE 单次 dispatch 地板（**M5 Pro**） | **≈ 70 µs**（小融合程序 ≈ 90 µs） | *ANEForge*，arXiv [2606.17090](https://huggingface.co/papers/2606.17090) |
| **我们自己的实测**（M4 Pro，HELENA 116k 参数，(1,612,14,2)） | **p50 141 µs / p99 208 µs** | 本仓库 AI CE G1（`doc_chinese/ai_ce/`） |
| ANE **工作集悬崖** | **2 MB（M1）/ 4.72 MB（M5）**；超过后从 ~12 TFLOP/s fp16 算力屋顶掉到 ~85 GB/s 带宽屋顶（ridge ≈ 141 FLOP/byte） | 同上两篇 |

### 1.1 推论（对规划的硬约束）

1. ★ **ANE 不能按符号调用。** 我们的接缝 `submit_fused` 是**逐符号**调用的
   （`pusch_demodulator_impl.cpp:601`）；一个槽有 14 个符号 ⇒ 14 次 ANE dispatch ⇒
   在 M1 上 **3.2 ms/槽**、在 M5 Pro 上 **≈1 ms/槽**，**全部超预算**。
   ⇒ 若用 ANE，必须是**一次融合的、分块的（chunked）程序**，每个槽（或每个符号组）只 dispatch 一次。
2. ★ **Metal / MPS GPU 才是主路径**，ANE 只作为"**单个大融合阶段的可选卸载**"。
   （v1.0 规划把 ANE 当锚点，是因为只看了 HELENA 的 141 µs —— 那是 **M4 Pro + 单个中等模型**的结果，
   不能外推到"每符号一次"的调用模式。）
3. **分块方向是明确的**：按符号 / 子带切。工作集悬崖针对的是"**在 RE 上跑 transformer**"，
   不是网格本身——273 PRB × 14 符号 × 4 端口 complex-fp16 网格只有 **≈ 0.12 MB**。
   这从另一个角度支持了"**A2 卷积形态优于 A3 Transformer 形态**"（主规划 §2）。
4. **fp16 的"量程 vs 精度"是 CoreML/ANE 的真实约束**，会打到 LLR 与相关求和上——
   方法论先例见 *Range, Not Precision: Block-Floating-Point Half-Precision FFT and SAR Imaging on
   Apple Silicon*，arXiv [2605.28451](https://export.arxiv.org/pdf/2605.28451)。
5. **Apple GPU 的 DSP 是带宽瓶颈而非算力瓶颈**——*Bandwidth, Not FLOPS: FFT Kernels, Matrix Units
   and SAR Imaging on Apple M6*，arXiv [2609.32237](https://ar5iv.labs.arxiv.org/html/2609.32237v1)。
   ★ 这与本仓库 `metal_kernel_fusion` 自己的结论**独立吻合**（"一格 667 µs 里只有几十 µs 是算力"）。

---

## 2. ★ 路线 D（dApp）在 Apple Silicon 上不可用 ⇒ 内联是唯一路线

| 事实 | 结论 |
|---|---|
| OCUDU dApp 平台声明后端为 **"CPU (x86, ARM) 或 CUDA"**——**没有 Metal / ANE / CoreML 后端** | 路线 D **今天在 Apple Silicon 上不存在** |
| dApp 接缝**不在本 checkout**（没有 `lib/phy/upper/dapp`）；平台是独立预览仓库 `gitlab.com/ocudu/work_groups/wg2_ai_ran` | 若要走 dApp，**Metal 后端是我们自己要拥有的扩展**，不是现成能力 |

⇒ 主规划 §1.2 的结论**由"偏好"升级为"被迫"**：**走内联 `submit_fused`**。
这反而是好消息——它避免了把每槽 1.47 MB 进 / 4.4 MB 出的搬运重新加回一条以
"零主机↔设备数据穿越"为核心成就的 lane（`phy_pipeline_crossings.h`）。

### 2.1 ★ 采纳它的三个时序契约（Class A / B / C）作为我们的规格

| 类 | 定义 | 预算 | 我们适用吗 |
|---|---|---|---|
| **Class A** | **驻留在 GPU 接收链上**（零拷贝设备张量） | **估计 100 µs / 完成 150 µs** | ★ **这就是我们的目标类**：AI detector 必须驻留在接收链上、零拷贝 |
| Class B | 在调度器准入的 100 µs 截止期内 | 直接调用 **0.29 µs P50 / 5.2 µs P99.9**（空口在跑） | 若走 dApp 才是这一类 |
| Class C | 永不阻塞的观察者 | — | 不适用 |

★ 把 "Class A" 写进 G5 的判据，等于直接采用一个**已被行业实测过的口径**，
而不是自己另立一套（本仓库吃过"自创口径导致数字不可比"的亏）。

---

## 3. G5 的实测对标基线（全部为**已核实片段**）

| 指标 | 数值 | 出处 |
|---|---|---|
| neural-receiver→LLR 的**槽占用** | **≤ 500 µs** | arXiv [2609.07805](https://arxiv.org/pdf/2609.07805v1.pdf) |
| 每槽数据体量 | **1.47 MB 进 / 最多 4.4 MB 出** | 同上 |
| 接口（Class B 直接调用） | **0.29 µs P99.9（静默）/ 5.2 µs（空口在跑）** | arXiv [2609.07843](https://arxiv.org/pdf/2609.07843v1.pdf) |
| 接收机 kernels（NVIDIA GB10） | **82 µs P50 / 112 µs P99.9** | 同上 |
| 网格拷贝（273 PRB 4 端口） | **50–100 µs / 槽** | 同上 |

★ 两个对照点：
1. 我们的现网 eqdem 是 **667.4 µs**（mkf033 中位）——比前作的 500 µs 占用**还高 33%**。
2. 前作的 273 PRB / 4 端口是**我们（51 PRB / 1 端口）的 ~21 倍体量**，
   它的 82 µs kernel 时间不能直接当我们的预算——**必须实测**。

### 3.1 最接近的合规性前作

*Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR*（IEEE Xplore 11140048；
[NVIDIA Research](https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr)）
——把学习型接收机塞进**符合 NR 规范**的链条，是"合规性"轴上最接近的前作。

空口神经接收机试验台（USRP + OAI）：[NI/Ettus gNB 侧](https://github.com/EttusResearch/ni-5g-oai-neural-receiver-testbed-ran)、
[UE 侧](https://github.com/EttusResearch/ni-5g-oai-neural-receiver-testbed-ran-ue)、
[Ettus KB](https://kb.ettus.com/index.php?title=5G_OAI_Neural_Receiver_Testbed_with_USRP_X410&oldid=6235)
——与本工作流的形态最接近的公开试验台。

---

## 4. ★ LLR 专属结论（直接决定主规划 §1 的契约与 §3.3 的校准）

### 4.1 "模型输出什么"这个问题，文献里有清晰的分野

| 家族 | 输出 | 标定好的逐比特 LLR？ | 译码器在环？ |
|---|---|---|---|
| **LINE A**：DetNet / OAMP-Net / OAMP-Net2 / MMNet | **软符号估计**（`q_k`、`E{xᵢ\|rᵢ}`） | **否**（OAMP-Net2 论文里有逐比特 LLR 表达式，但**不讨论标定**） | 基本没有 |
| **LINE B**：神经解映射器 / 神经接收机 | **逐比特 LLR** | ★ 承认这是真问题：LLR 分布失配、显式 **LLR 校正网络**、学习型 LLR 量化 | **有**（Sionna、5G-NR PUSCH 神经接收机、DUIDD） |

⇒ ★ **我们属于 LINE B，而不是 LINE A**。这一条把模型选型从"AI 检测"这个模糊的词，
收缩到了"**输出 LLR 的神经接收机/解映射器**"这条具体的技术线上——主规划的 A1/A2 正是这条线，
而 A3（DEFINED 式 Transformer 检测器）属于 LINE A 的符号输出范式，**需要额外一步才能接上译码器**。

### 4.2 LLR 标度/裁剪是**已被命名的成熟问题**，不是我们的特殊困难

- ★ 最好的一条命中（arXiv **2601.16586**，2026）原文：
  > "To ensure stable and well-calibrated soft outputs, the computed LLRs are subject to **scaling and
  > clipping**, which is a well-known …"【已核实片段】
- 经典参考：*Complexity Adjusted Soft-Output Sphere Decoding by Adaptive LLR Clipping*（arXiv 1011.2113）。
- 学习型量化：*Learning Quantization in LDPC Decoders*（arXiv 2208.05186）——学习一个按位宽 `w` 与
  阈值 `T` 索引的量化/压扩映射。
- 学习型 LLR 标度因子：多篇专利（CN120956282A、WO2026072512A1 / US20260088929A1、US10784899）。
- 非神经但同类机制：SNR 失配补偿的 LLR 直方图译码。

⇒ ★ **主规划 §3.3 的"标度校准是独立步骤"得到文献支持**：这是标准做法，且有现成的技术路线
（scaling + clipping / 学习型量化 / 温度缩放）。**不要把它当成训练细节。**

### 4.3 OTA 训练本身有已知的负面结果（★ 直接影响我们的标签路线）

*Input-Correlated Supervision Noise Limits the Benefits of OTA Training for Learned Receivers*
（arXiv **2608.12918**，2026）——直接讲**训练信号失配**会削弱学习型接收机的收益。
⇒ 主规划 §3.1 的**路线 1（CRC-OK TB → 真值比特）虽然标签是真值，但"输入分布来自 OTA"这一侧
仍可能带来相关噪声**。P1 必须把"仿真训练 vs OTA 训练"作为**一个受控变量**，而不是默认 OTA 更好。

### 4.4 信任与回滚是运维必需品，不是可选功能

*When Does a Neural Receiver Help? Calibration-Drift Benchmarking and Detect-and-Rollback for
5G/6G NR Uplink*（arXiv **2605.26157**）——**60.5% 的平均回滚率**：神经接收机需要**逐槽**决定
是否信任自己、否则回退到经典接收机。原文片段：
> "The 60.5% mean rollback rate (Table IV) reflects that R5 trusts R1 on the majority of slots —
> slots on which R1 has collapsed…"【已核实片段】

⇒ ★ 主规划新增设计项 **"逐槽信任/回滚"**：AI detector 必须能在**不打断实时性**的前提下，
每槽判定"本槽是否采用 AI 输出"，否则回退经典。**这同时是我们内联路线的一个天然优势**：
经典路径始终在链上，回退只是不启用融合（谓词返回 false）。

---

## 5. 有争议的与负面的证据（规划必须正面回应，不能只引好消息）

1. ★ **"学习型/ICL 接收机在译码器在环、且比较诚实的条件下，是否真的打得过调好的经典链"——
   文献里是有争议的。** 多数收益停在未编码 BER 或 1e-2~1e-3 BLER；
   2026 的 calibration-drift 论文显示神经接收机需要逐槽信任/回滚（60.5% 回滚率）——**很难说是干净的胜利**。
   ⇒ 这正是主规划把 P0（genie 上界）放在最前面的外部理由。
2. **泛化性**：跨 SNR、调制阶数、信道模型、以及**站点**。文献里既有乐观的鲁棒性
   （DEFINED 的 Rayleigh→Rician），也有明确的脆弱性（site-specific 训练）。
3. **Transformer vs CNN/GNN 在同等时延预算下**：Transformer 接收机论文强调可并行性，
   但**几乎没有人拿同等实测时延的 CNN 基线做归一化对比**。
4. **LLR 应该由模型产生，还是由符号估计解析导出**（后者用高斯假设，标定更干净）。

## 6. 文献空白 ⇔ 本工作流可能的贡献

| 空白 | 文献现状 | 硬件实测的 OCUDU + Apple Silicon 实现能补什么 |
|---|---|---|
| 实时 PHY 用 Apple Silicon GPU/ANE | **什么都没找到** | 首个在活体 gNB 中、CoreML/Metal 上 grid→LLR 模型的**实测每槽时延/吞吐**；含 fp16 量程对 LLR 的影响 |
| 运行中的 RAN 里 **Transformer 检测器**的每槽时延 | 只有解析/桌面 GPU 结果；dApp 论文测的是管道不是模型 | 一个**截止期已验证**的数字（每槽占用）与"浅/深、prompt/非 prompt"的时延-精度 Pareto |
| ICL/prompt 检测器在**固定 NR DMRS 预算**下 | DEFINED 一类假设"导频喂 prompt"；未找到合规研究 | **prompt 式检测能否在标准实际给出的 DMRS 密度下存活**——★ 这正好命中我们的 P0 |
| LLR 校准 + **译码器在环的 1e-5 BLER / 错误平台** | 稀薄；多数论文停在 1e-3~1e-4 或未编码 BER | 过真实 NR LDPC 译码器的 BLER/error-floor 曲线 + 校准/标度研究 |
| 信任/回退工程（漂移检测、回滚） | 一篇 2026 论文 | 在不同硬件/模型族上的独立复现 + 可部署的回退设计 |
| AI PHY 的功耗/热 | 什么都没找到 | 功耗/热测量 |

★ **一句话定位**（调研的结论，本文认同）：
文献已经有 (a) 仿真里能工作的 Transformer/ICL 检测器、(b) 通过外信息 LLR 与译码器对话的深度展开检测器、
(c) 唯一的开放 5G 栈上的内联 PHY 时延实测研究。**不存在的**是——
**硬件实测的、实时的、合规的 grid→LLR 模型，跑在 Apple Silicon 上、在空口 gNB 里、
带真实 LDPC 译码器在环、并给出诚实的校准与错误平台数字**。
**贡献在于这个组合，而不是模型架构本身。**

## 7. 仍未能核实（引用前必须复核）

- ANE 两篇论文的 PDF 原文（只有索引片段 + 独立新闻源佐证）；M5 Pro 的 70 µs 地板尤其要复核。
- dApp 论文的完整正文（GB10 的 82 µs/112 µs 等数字来自并行调研线的转述，与已核实片段自洽但未直接读到）。
- SoftBank "+30% 5G 吞吐"、R&S+Nokia、T-Mobile/Ericsson 等**厂商数字的测量条件**（只核实了标题存在）。
- LINE A/B 各论文的具体损失函数与数字（`survey/` 中已逐条标注【未核实原文】）。

## 8. 参考（主线）

- [Real-Time dApps for AI-RAN (2609.07805)](https://arxiv.org/pdf/2609.07805v1.pdf) ·
  [The OCUDU dApp Platform (2609.07843)](https://arxiv.org/pdf/2609.07843v1.pdf)
- [Apple Neural Engine: Architecture, Programming, and Performance (2606.22283)](https://arxiv.org/abs/2606.22283) ·
  [ANEForge (2606.17090)](https://huggingface.co/papers/2606.17090)
- [When Does a Neural Receiver Help? (2605.26157)](https://arxiv.org/pdf/2605.26157) ·
  [Input-Correlated Supervision Noise (2608.12918)](https://browse-export.arxiv.org/pdf/2608.12918)
- [Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR](https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr)
- [Range, Not Precision (2605.28451)](https://export.arxiv.org/pdf/2605.28451) ·
  [Bandwidth, Not FLOPS (2609.32237)](https://ar5iv.labs.arxiv.org/html/2609.32237v1)
- [3GPP TR 38.753（NR 空口 AI/ML 研究）](https://whatthespec.net/friendlyspec/pdf/38753/38753-j00.pdf)
