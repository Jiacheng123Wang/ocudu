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

## 2. ★★ dApp 前作：必须分清"预算"与"实测"（v1.0 勘误）

> ⚠ **勘误**：本备忘 v0.9（中期版）把 dApp 论文里的 **"≤500 µs 占用"** 当成了"某个
> neural-receiver dApp 的实测占用"。**这是错的。** 获得 `curl` 直读原文的能力后
> （`web_fetch` 被封，但 shell 的 `curl` 可用）已核对：**500 µs 是该用例的"预算/截止期"**，
> 实测值是另一组数字。以下全部为**直读原文**【已核实原文】。

### 2.1 他们具体建了什么、测了什么

论文 *The OCUDU dApp Platform*（arXiv 2609.07843，O'Shea / Pennybacker / Kharchenko，2026-09-07）：

- 平台 + SDK + 零硬件 quickstart，**BSD-3-Clause-Clear**，是 **OCUDU AI-RAN Working Group 2 的
  preview release**；原文写明是 *"ahead of **upstreaming into** the OCUDU mainline"*。
- **后端声明**：*"a package declares a **CPU (x86, ARM) or CUDA** backend … the SDK ships every
  reference in both variants"* ⇒ **没有 Metal / ANE / CoreML 后端**。
- **Class A（驻留内联 L1）契约**——神经接收机是旗舰应用：

  | 项 | 原文 |
  |---|---|
  | 输入 | *"GPU-resident slot grid (**complex BF16**), DM-RS pilot tensor, compact data-RE indices, typed PUSCH and DM-RS metadata"* |
  | 输出 | *"channel estimates plus noise; equalized symbols plus post-equalization noise; **FP16 soft bits before descrambling**"* |
  | 预算 | *"**100 µs for estimation, 150 µs to completion** for the deeper two"* |
  | 执行 | *"in the DU process, enqueued on the PUSCH lane's own **CUDA stream**"* |
  | 失败处理 | *"the **conventional stage runs in the same invocation**. Late completion: the result is used, an incident is recorded, and **eight in a row open the lane breaker** so later grants take the conventional path"* |
  | 权重管理 | 两个预分配设备权重 bank + 单次原子交换切换，"no allocation on the hot path" |

  ★★ **这张契约表独立验证了我们的接口设计**：输入是 **BF16 网格 + DM-RS 导频张量**，
  输出是 **解扰前的软比特**——与 `memo_01` §3 冻结的"**模型输出加扰域 LLR、解扰保持经典**"
  **完全一致**。两个独立团队在同一接缝上收敛到同一契约，可当作我们契约的外部背书。
  ★ 同时它给 **R9（逐槽信任/回滚）一个具体工程范例**：经典路径在同一次调用里兜底、
  **连续 8 次迟到才打开 lane breaker**、权重双 bank 原子切换。

- **实测部署**：*"On a GB10 gNB with attached handsets, dApps of all three classes,
  **including an out-of-tree neural equalizer, ran together on a live cell without a single fallback**"*。
  且该神经均衡器**不在 SDK 里**：*"a package built out of source tree, against the public SDK alone,
  **by a separate team**"*。

论文 *Real-Time dApps for AI-RAN*（arXiv 2609.07805，同作者）：

| 行 | "字节 / 预算"（**预算，非实测**） | 驻留/有界（in-process ABI）列的**实测** |
|---|---|---|
| **A-03 neural receiver to LLRs** | *"**1.47 MB in, up to 4.4 MB out per slot; 500 µs occupancy**"* | ★ *"Feasible at the live shape: **81.6 µs / 112 µs P50 / P99.9 (meas.)**, 92.5 / 105.1 µs runtime checkpoint; **273-PRB kernels not yet qualified**"* |
| **A-02 neural equalizer** | *"68.5 KB in (live); same-invocation consumption"* | ★ *"Feasible: **45–52 µs P50**; **260,000+ invocations on air, 0 fallbacks**"* |
| B-01 scheduler intents | *"4.2 KB in, 2.3 KB out; 100 µs to commit"* | *"direct call **0.288 µs P99.9 quiet, 5.22 µs with the DU on the air**, 20,000 validated calls (meas.)"* |

### 2.2 修正后的四条关键认识

1. ★ **"live shape" = 51 PRB、两个接收端口**（原文：*"the released testbed shape (**51 PRB, two
   receive ports**)"*）——**几乎就是我们的形态**（我们实测 25–51 PRB、1 端口，`memo_04` §2）。
   所以 **81.6 µs P50 / 112 µs P99.9** 是**可直接与我们的 667.4 µs eqdem 对比的实测数字**，
   远比"500 µs 预算"有用。
2. **273 PRB 的 kernel "尚未 qualified"** ⇒ 更大的包络还没验，**不能假设线性外推**。
3. **0.288 / 5.22 µs 是 Class B 的调度器直接调用**，**不是**神经接收机的接口开销——
   引用时不要张冠李戴（v0.9 中期版正是这么写的，已改）。
4. ★★ **外部 dApp 框架对神经接收机是 "Inexpressible"（不可表达）**，理由与速度无关：
   *"an indication carries data outward, and **nothing brings a channel estimate, an equalized tensor,
   or an LLR tensor back into the PUSCH chain of the same slot**"*——39 个用例里 **13 个 Class A 行中
   有 11 个**在两种外部框架下都不可表达。D2H 导出 1.47 MB **仅传输就 73.5 µs**（"before inference"）。

---

## 3. 路线决策与 G5 基线（修正后）

### 3.1 内联是唯一正确路线（论据升级）

| 事实 | 后果 |
|---|---|
| dApp 后端只有 **CPU (x86, ARM) 或 CUDA**，无 Metal/ANE/CoreML | 路线 D **今天在 Apple Silicon 上不存在** |
| 接缝**不在本 checkout**（无 `lib/phy/upper/dapp`），是独立预览仓库 | 走 dApp 需要我们自己拥有一个 Metal 后端 |
| ★ **更根本**：外部 dApp 对神经接收机 **Inexpressible**——没有把 LLR 张量送回同一槽的回路 | **即使有 Metal 后端，外部 dApp 路线在结构上也是错的** |

⇒ 结论不变（**走内联 `submit_fused`**），但论据从"CUDA 专有"升级为"**结构上不可表达**"。
这同时避免了把每槽 MB 级搬运重新加回一条以"零主机↔设备数据穿越"为核心成就的 lane。
★ 并且现在有了**同类实现的实测对照**：同一个接缝上，别人做到 **81.6/112 µs**（51 PRB/2 端口）。

### 3.2 G5 的对标基线（修正后，全部为实测或预算且已标明）

| 基线 | 数值 | 性质 |
|---|---|---|
| 我们的现网 eqdem | **667.4 µs**（mkf033 中位） | 实测（本仓库） |
| ★ 同类内联神经接收机→LLR | **81.6 µs P50 / 112 µs P99.9**（51 PRB / 2 端口） | 实测（GB10） |
| 同类内联**神经均衡器** | **45–52 µs P50**，26 万+ 次空口调用、**0 fallback** | 实测（GB10） |
| Class A 生产预算 | 估计 **100 µs** / 完成 **150 µs** | 契约（非实测） |
| A-03 用例预算 | 1.47 MB 进 / ≤4.4 MB 出 / 槽；**500 µs 占用** | **预算（非实测）** |
| Class B 调度器直接调用 | 0.288 µs P99.9 静默 / 5.22 µs 空口 | 实测（GB10），**与本工作流无关** |
| 现网 SCF FAPI 类接口 | *见 2609.07805 §V* | — |

★ 三条纪律：① **不要把预算当实测引用**；② **不要把 Class B 调度器的数字当成神经接收机的数字**；
③ 81.6/112 µs 是**别家硬件 + 别家包络**，对我们**只能是目标，不是预期**。

### 3.3 最接近的合规性前作

*Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR*（IEEE Xplore 11140048；
[NVIDIA Research](https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr)）。

空口神经接收机试验台（USRP + OAI）：[NI/Ettus gNB 侧](https://github.com/EttusResearch/ni-5g-oai-neural-receiver-testbed-ran)、
[UE 侧](https://github.com/EttusResearch/ni-5g-oai-neural-receiver-testbed-ran-ue)、
[Ettus KB](https://kb.ettus.com/index.php?title=5G_OAI_Neural_Receiver_Testbed_with_USRP_X410&oldid=6235)。

### 3.4 "是否 upstream 已有此项工作"（回答用户提问）

| 问题 | 事实 |
|---|---|
| 我们的 checkout 里有 dApp 代码吗？ | **没有**（全仓 `find` 无 `*dapp*`；本仓库 remote 是用户自己的 GitHub/GitLab fork） |
| dApp 平台在哪？ | **OCUDU 的独立预览仓库**（AI-RAN WG2）：平台 + SDK + quickstart |
| 进 OCUDU mainline 了吗？ | **没有**——原文 "ahead of **upstreaming into** the OCUDU mainline" |
| 我们能直接用吗？ | **不能**：① 不在我们的树里；② 后端只有 CPU(x86/ARM) 与 CUDA，**Apple Silicon 无路径**；③ 绑在 NVIDIA 的接收链与 **CUDA stream** 上 |

⇒ 一句话：**OCUDU 生态里确实已经有"内联神经接收机 → LLR"的真实工作与实测数据，
但它在主线之外、独立仓库、且 CUDA 专有。** 对我们而言它是
**前作 + 对标基线 + 契约背书**，不是可复用的代码。

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

## 7. 核实状态（v1.0 更新：多数已从原文直读）

调研代理发现 **`web_fetch` 被 DNS 封锁、但 shell 的 `curl` 可用**，随后**大部分结论改为直读原文**
（arXiv HTML/PDF、Apple DocC JSON API、coremltools 文档、3GPP/ETSI PDF）。
因此 v0.9 中期版里"未能核实"的条目，绝大多数**已经升级为【已核实原文】**——
包括 ANE 的两篇论文、dApp 两篇论文、DEFINED 的图注数字。

**仍然只能标为未核实/需注意的**：

| 条目 | 状态 |
|---|---|
| ANE 的 **M5 Pro ≈70 µs 地板** | 论文为直读，但该数字本身带条件（固定形状、预热）；**我们自己的 M4 Pro 上必须实测** |
| **NVIDIA GB10 的 81.6/112 µs、45–52 µs** | 直读原文；但**是别家硬件 + 别家包络**（51 PRB/2 端口），对我们**只能是目标** |
| 厂商数字（SoftBank +30%、R&S+Nokia、T-Mobile/Ericsson） | 仅核实标题存在，**测量条件未核实** |
| `survey/` 中各论文标注【unverified】的个别数字 | 见各文件自己的 "could not verify" 清单 |

★ **引用纪律**：凡进入对外材料（论文/报告/PPT）的数字，**必须回到 `survey/` 里对应的原文证据行**，
不得直接引用本备忘的转述。

## 7bis. 最终调研（7 条线全部完成）新增的**决策级**结论

> 以下来自调研代理的最终交付（全部 7 条线 + 综合），原始文件见 `survey/`。
> ★ 其中多条**改变了主规划的默认值**，已在主规划 v1.3 中落实。

### A. ★★ Apple Silicon 的现状：唯一一个"神经接收机在 Apple 硬件上"的数字，是灾难性的

- **文献里没有任何神经接收机（乃至任何无线 PHY 接收机）跑在 Apple Silicon 上。**
- 唯一一个 Apple 硬件上的神经接收机时延：**M3 Ultra，CPU 模式，1.23 M 参数 DeepRx 前向
  = 72.10 ms/slot**，+9.12 ms LDPC = **81.23 ms 全链**，对照 **1 ms 时隙预算**；
  同模型在 RTX 6000 上前向 **24.10 ms**。
- ★ **"72 ms → 1 ms"这个差距，正是 Metal/CoreML 实现要回答的问题，而没有人发表过它能不能关掉。**
  这是本工作流**最锋利的开放性陈述**（比"没人做过"更具体）。

### B. ★ ANE 的四条硬约束（v1.2 只知道 dispatch 地板，现在知道更多）

1. **Core ML 没有"只用 ANE"的模式**，也**没有运行时 API 告诉你哪个单元跑了**；
   `MLComputePlan` 只是离线估算。
2. ★ **自定义 Metal kernel 不能跑在 ANE 常驻图里**（custom layer 只能 CPU/GPU）。
   ⇒ 主规划里"Metal 主路径 + ANE 单阶段卸载"**不能是同一个图内混合**，必须是两个独立阶段。
3. **`EnumeratedShapes`（≤128）是 ANE 认可的形状路径**；无界 `RangeDim` 会被拒。
4. 设备特化缓存**以 `mlmodelc` 路径为键**。

⇒ 加上 §1 的地板（M1 0.23 ms / M5 Pro ≈70 µs）与工作集悬崖（2 MB / 4.72 MB），
**ANE 唯一可行的形态**是：**每个时隙一个融合的、固定形状的、预热的程序**。

### C. ★ fp16 的约束是"量程"而不是"尾数"

- 朴素 fp16 SAR 流水线**全是 NaN**，因为中间量达到 **5e6 ≫ 65504**；用 **1/N 块浮点缩放**才修好
  （arXiv 2605.28451）。
- ★ **没有任何人发表过 LLR 的"精度-性能"曲线。**
- ⇒ 工程结论：**LLR 头必须有显式 clamp/tanh，绝不能接近 65504**。
  这正好接上主规划 §3.3 的标度校准——两者是同一件事的两面。

### D. ★ 量化指导（可直接写进 G3 判据）

| 位宽 | 代价 |
|---|---|
| **8-bit** | **基本免费**（与 FP32 差 0.05 dB 以内） |
| INT4 | **损失 3.3–3.7 dB，并跌破 LS-LMMSE 基线** |
| FP4 (E2M1) | 可用 |

### E. ★ 损失函数：主流是逐比特 BCE（⚠ 本条措辞已被 §M 修正——MSE-on-LLR **存在但属少数派**）

实际观察到的损失：**逐比特 BCE（对编码比特）**为主；soft-BCE（其梯度等价于到 MAP 后验的 KL）；
符号后验交叉熵（CMDNet）；Donsker-Varadhan 互信息下界（Fritschek）；可达速率类目标。
★ 早期结论是"bit-wise MSE against true LLRs 一个实例都没找到"——**该结论在 §M 被修正**：
NVIDIA Aerial 的 `LLRNet` 就是用 MSE 对参考 LLR 训练的。准确表述见 §M。

⇒ 主规划 §3.2 的损失优先级**据此调整**：**BCE 为唯一主损失**，
"LLR 回归/KL"**降级为消融项**（而不是与 BCE 并列的主候选）。

### F. ★ 评价指标：**必须用真实 LDPC 译码器之后的 coded BLER**

- ★ **互信息（MI）不能预测 BLER**：一个接收机可以有近最优的 MI 却仍落在错误平台上
  （arXiv 2606.29345）。
- ⇒ 主规划 G2/G3/G6 的判据**以 coded BLER 为唯一主指标**，BER/MI 仅作诊断。

### G. ★★ 负面证据（比 §5 更硬，必须在 P0/G 门禁里正面处理）

1. **ETH Zurich 2026（arXiv 2609.04004）**：标准合规 5G NR 试验台 + 商用 UE，
   decoder-in-the-loop 的 DUIDD 从**站点特化微调**只得到 **0.004 绝对 BLER** 的改善；
   而 **"站点特化 LMMSE + 经典 IDD"是所有被测接收机里错误率最低的**。
2. **Calibration-drift（arXiv 2605.26157）16 个场景**：**3/16 增益 1.0–2.0 dB；10/16 打平（±0.2 dB）；
   QPSK 反而差 ~2 dB**（标定不佳，训练以 16QAM 为主）；64QAM 在该参考模型里是**架构性失败**；
   **DMRS AddPos=2（分布外）从 4 dB 起静默地钉在 100% BLER**；
   "自信地判错"的比特比例**平台在 ~7%**，这给任何"有界 LLR 残差修正"设了上限。
3. ★★ **朴素回滚会失败**：在 500 Hz Doppler 下**经典接收机崩溃而神经接收机能工作**
   —— 所以"AI 不行就回退经典"是错的。他们的解法是**逐时隙"神经+经典并行仲裁"**，
   代价 **<5% 时延**。
4. **Nokia OTA（arXiv 2408.04182）**：LOS 训练的 DeepRx 模型
   *"failed the over-the-air tests despite converging well during training"*；
   **宽随机化胜过参数匹配**（0–30 m/s 训练的模型打败了按实际步行速度训练的模型）。
5. **训练不稳定性**：只最小化最终损失 "leads to poor performance"，需要对所有展开迭代做多损失；
   min-sum 的折点处需要次梯度。

### H. 其它可引用的实测锚点

- 标准合规实时 NRX：**A100 + TensorRT < 1 ms，代价是 SNR 损失 < 0.7 dB**（arXiv 2409.02912）。
- Sionna Research Kit 实时 TensorRT 接收机：**Jetson AGX Orin + 商用 UE**（arXiv 2505.15848）。
- **DGX Spark 上 LDPC：CPU = 0.71 ms/码字 @20 迭代（超过 0.5 ms 时隙），GPU 只占时隙的 6–24%**
  （arXiv 2602.04652）★ 这条对我们的"整链预算"很重要：**译码器本身就可能吃掉整个时隙**。
- **尾延迟才是门禁**：HELENA 在 RTX PRO 4500 上 P99 = 0.0595 ms，但
  **在 10 W Jetson Orin NX 上没有任何模型满足 P99 预算**。
- DeepRx MIMO（2010.16283）在 ~14 dB 出现 *"a BER floor"*；★ 但**没有人把神经接收机的 BLER
  画到 1e-5 做错误平台研究**。

### I. 最终"文献空白"清单（我们的贡献点）

★ **完全不存在**：Apple Silicon 上的任何神经接收机；任何 PHY 负载的 ANE 时延；
活跃 RAN 里 transformer 检测器的 **P99/P99.9 每时隙时延分布**；
任何 **LLR-vs-精度**曲线；任何针对学习型接收机 LLR 的**温度缩放/有原则的标定**；
任何 **1e-5 的神经接收机 BLER 错误平台**研究；任何 AI PHY 在宿主机器上的**功耗/热**数据。

⇒ 本工作流的贡献**不是新架构**，而是上面这一串的组合。与 §6 的判断一致。

### J. 由证据直接推出的五条"立即行动"（已并入主规划）

1. **P99.9 尾延迟**作为验收门禁（不是均值）。
2. **Metal/MPS 为引擎**，ANE 只作**一个**融合阶段（且不能与 Metal 自定义 kernel 同图）。
3. 内部规格直接采用 **OCUDU Class A/B/C 契约**。
4. **每个时隙都保持经典接收机武装**——但注意 §G.3：回滚判据不能是"AI 差就退回经典"。
5. LLR 头**从第一天就带显式 clamp 与逐调制标度**。

## 7ter. 最终补充（LINE A+B 全文核对后的追加，含**引用勘误**）

### K. ★★ 最有说服力的一条负面证据：DetNet 的"软"输出其实是硬的

**CMDNet**（IEEE TCOM 69(12):8214–8227, 2021，arXiv 2102.12756）用直方图实测：

> "the soft output version of DetNet should deliver accurate probabilities or LLRs … Indeed, we visualize
> with an exemplary histogram of LLRs that **this is not the case** … DetNet mostly provides hard
> decisions with **∼97 % LLRs being −1 and 1**"

并补一句：*"In coded systems with soft decoders usually employed today, delivering soft information is a
strict requirement."* CMDNet 自己**带真实的 128×64 rate-1/2 LDPC 译码器（BP, 10 迭代）并报 coded FER**。

⇒ ★ 这是"**LINE A ≠ 我们**"最硬的证据：符号输出的检测器**不是"软输出弱一点"，而是根本不产出可用的软信息**。
⇒ 也给出一个我们**应当复现的测量**：Baumgartner 等（arXiv 2211.06054）的
**逐 |LLR| 分桶经验错误率** `P_emp,k = (#该桶内错误硬判决)/(#该桶内比特)`。

### L. ★ LLR 接口的具体数字（可以直接抄）

| 来源 | 事实 |
|---|---|
| **Sionna Research Kit** 神经解映射器 | float16 入；**int16 LLR 出，用 `np.rint(np.ldexp(llrs, 8))`（即 2⁸ 缩放）**；教程**实测到相对 OAI 参考有 2.42× 的标度失配**；并指出 min-sum *"is known to be robust against mis-scaling of the LLRs"*；`LDPC5GDecoder` 内部 **`llr_max = 20.0`**、20 迭代 |
| **NVIDIA 合规实时 NRX**（2409.02912） | `ReadoutLLRs` 输出**编码比特**的 LLR，训练用 **对 LDPC 编码后真值比特的 BCE**；*"the code rate and coding scheme is transparent to the NRX"*；**float16 权重、无 QAT**、接口处**未声明 LLR 位宽/饱和**；预算：A100 上 1 ms，**~350 µs/迭代 + 270 µs 开销 @132 PRB/2 UE ⇒ 最多 2 次迭代**；**实时模型只有 1.4e5 权重**（2 迭代）vs 4.4e5（8 迭代）；性能代价 **<0.7 dB** |
| **CENTRIC PoC**（Zenodo 12731570） | LLR 进"标准合规 LDPC 译码器"，KPI = **LDPC 之后的 BLER**；**<1 dB 相对 LMMSE+K-Best**；A100 上 132 PRB **1 ms**；**未描述 LLR 量化/裁剪** |

★★ **两条对我们直接有用的对比**：
1. **他们的 `llr_max = 20.0`，我们的 `LLR_MAX = 120`**（`memo_01` §2）。
   两者不是同一层的东西（他们是译码器内部裁剪，我们是 int8 量化上限），但
   **"LLR 动态范围该取多大"是一个必须自己测的量**，不能照抄。
2. ★ **实时神经接收机的参数量锚点是 1.4e5**（NVIDIA，2 迭代，<0.7 dB 代价）——
   与 HELENA 的 1.16e5 同量级。这给规划 §2.3 的"尺寸预算"一个**可引用的上界**：
   **能进 1 ms 时隙的模型在 10⁵ 量级，不是 10⁶**。

### M. ★ 勘误：MSE-on-LLR **确实存在**（修正 §7bis.E 的措辞）

§7bis.E 写的是"对真值 LLR 做 MSE 一个实例都没有"。**这句过强了**：
NVIDIA Aerial 的 **`LLRNet`**（"Machine LLRning"，Shental & Hoydis, IEEE Globecom Wkshps 2019,
arXiv 1907.01512）**就是用 MSE 对参考 LLR 训练**的。

修正后的准确表述：
- **主流是逐比特 BCE**（Sionna、NVIDIA NRX、CMDNet 的符号后验交叉熵）；
- **MSE-on-reference-LLR 存在，但属于少数派**（LLRNet 一线，且它需要"参考 LLR"作为监督，
  本质是**蒸馏**——会有教师上限）。
⇒ 主规划 §3.2 据此改为：**BCE 为唯一主损失；MSE/回归对参考 LLR 只作热身与消融**
（不仅因为少人用，更因为它把教师的上限变成学生的上限）。

### N. ★ 勘误：**译码器放进训练**的收益是 mixed-to-weak（放进评测是必须的）

| 来源 | 结果 |
|---|---|
| "A Neural Receiver for 5G NR Multi-user MIMO"（IEEE Globecom Wkshps 2023, arXiv 2312.02601） | 训练里用了**可微 LDPC 译码器**，却报告 *"we empirically **did not observe any gains** by doing so"* |
| ETH 2026（2609.04004） | 站点特化微调只买到 **0.004 绝对 BLER** |
| Cammerer TCOM 2020（1911.13055） | OTA **+1.3 dB（vs 256QAM + 802.11n LDPC）**；+0.6 dB 是对 **AWGN-MAP demapper** 的——两个数字都对，回答的是不同问题 |

⇒ ★ **区分两件事**：**译码器在环评测（mandatory）** vs **译码器在环训练（收益未证实）**。
主规划 §3.2 第 3 条据此降级。

### O. 引用勘误（全文核对后确认，勿再传播）

| 常见说法 | 事实 |
|---|---|
| DetNet 2017 发表在 IEEE SPL | **IEEE SPAWC 2017**（arXiv 1706.01151）；期刊版 "Learning to Detect", IEEE TSP 67(10):2554, 2019 |
| "DetNet with one-bit quantization, IEEE TSP 2019" | ★ **不存在**（该 TSP 论文全文 zero occurrences of "one-bit"/"quantiz"/"ADC"/"low-resolution"）。低分辨率展开的真实出处是 **LoRD-Net**（IEEE TSP 69:5651, 2021）等 |
| OAMP-Net 是 GLOBECOM | **IEEE GlobalSIP 2018**（arXiv 1809.09336）；OAMP-Net2 = **IEEE TSP 68:1702–1715, 2020**（不是 JSTSP）。混淆源：arXiv 1907.09439 的 **v1→v2 改了标题** |
| MMNet 假设 CDL/Kronecker 信道 | MMNet = **IEEE TWC 19(8):5635–5648, 2020**；信道是 **i.i.d. 高斯 + 3GPP 3D MIMO（TR 36.873）经 QuaDRiGa**——**不是** CDL/Kronecker（Kronecker 只出现在它的 prior work 里） |
| "End-to-End Learning for OFDM: From Neural Receivers to Hardware Feasibility" | ★ **不存在**；真实标题是 *…to **Pilotless Communication***（IEEE TWC 2021, DOI 10.1109/TWC.2021.3101364） |
| 深度展开综述 arXiv 2502.05952 有期刊版 | **仅预印本**。要引期刊综述用 **IEEE Communications Magazine 2026, DOI 10.1109/MCOM.001.2500444**；Model-Based Deep Learning = **Proc. IEEE 111(5):465–499, 2023** |

### P. MMNet / OAMP-Net2 的输出类型（LINE A 的完整画像）

| 模型 | 输出 | 译码器在环 |
|---|---|---|
| **DetNet** | 近似**符号后验概率** P(x=s\|y)（§IV "Soft decision output"），**明确不是逐比特 LLR**；原文 *"a full iterative decoding scheme is outside the scope of this paper"*。运行时（batch-1）：**0.0045 s**（对照 SDR 0.009、AMP 0.005、球形译码 0.001） | **无** |
| **OAMP-Net2** | 条件均值 E{x\|r,τ}，**外加式 (29) 的 LLR 读出**，并声称 soft-in/soft-out turbo——但原文说 *"specific experimental results are outside the scope of this paper and will be conducted in the future"*。每层 4 个可训练参数 | **未做实验** |
| **MMNet** | 内部软符号，**对外硬符号判决**；★ 全文 **LLR / log-likelihood / LDPC / channel-decoder 零次出现，"BER" 也是零次**——**只报 SER** | **无** |

⇒ ★ **结论**：LINE A 成熟，但**几乎不产出标定好的 LLR、几乎不带译码器在环**（`memo_03` §4.1 的判断被全文核对确认）。

> ★ **原文 PDF 已下载到 `ref_paper/`**（38 篇，文件名 = 论文标题，索引见 `ref_paper/README.md`）。
> 写进对外材料前请打开原文核对——本节只是指路。

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
