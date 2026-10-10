# `ref_paper/` — 参考论文原文（PDF）

工作流引用过的论文原文，**文件名 = 论文标题**。标题由 `fetch_refs.sh` 从 arXiv API 取回后落盘，
**不是手打的**，所以文件名不会与论文漂移。`:` 写作 `-`（macOS 把 `:` 当路径分隔符显示）。

- **148 篇**，用 `bash fetch_refs.sh` 复原（**幂等 + 可续传**：已存在的跳过，中断后重跑只补缺的）。
  抓取日志见 `download_log.txt`（**制表符分隔** —— 标题就是文件名，空格分隔会把多词标题截断）。
- ⚠ **PDF 不入 git**（规则在 `doc_chinese/.gitignore`）：第三方版权 + 体积（**当前 ~283 MB**）。
  **进 git 的是这份 `README.md`、`survey_citations.md`（自动生成）、`download_log.txt` 与两个脚本。**
  换机器跑一次 `fetch_refs.sh` 即可复原。
- ★★ **逐篇"被哪份文档引用"见 `survey_citations.md`**（`python3 build_survey_citations.py` 重新生成，
  它扫 `doc_chinese/` 下所有 `.md` 把 arXiv ID 映射到引用文件与行；★★ **同时校验"logged 的标题确实是磁盘上的文件"**，
  所以索引不会谎报存档）。

> ★★★ **取景说明（2026-10-10 用户更正）**：**ANE 只是无线 PHY 网络的一种承载方式。**
> 本目录的**主用途**是支撑**算法层面与前向网络架构设计**的调研；
> ★ 载体维度（Apple Silicon / ANE / CoreML）的条目保留，但**只作可行性依据，不作为贡献依据**。
> 更正原文与重检结论见 `../model_build_reconnaissance.md` §17。

## 全部调研材料的出处（★ 逐篇说明在这里，不在本文件）

★★ **本文件是"最该先读的那几十篇"的导读**；★ **141 篇的逐篇要点在 `../survey/` 里**，
按线组织，每条带引用与原文片段（用 `survey_citations.md` 反查）：

| 文件 | 覆盖 |
|---|---|
| `../survey/survey_master_ai_receiver_literature_review.md` | 总览 |
| `../survey/survey_line_a_part1_detnet_oampnet.md`、`_part2.md`、`_model_based_mimo_detection.md` | **LINE A**：模型驱动 / 展开式检测（**我们不是这一线**）|
| `../survey/survey_line_b_llr_output_receivers.md`、`survey_line_b2_neural_receivers.md` | **LINE B**：输出 LLR 的接收机（**我们属于这一线**）|
| `../survey/survey_line_a_b_output_types_llr_calibration_deep_dive.md` | **输出类型与 LLR 标定/量化**深挖（★ **量化四篇在这里**）|
| `../survey/survey_line_c_decoder_in_the_loop.md` | **LINE C**：译码器在环 / IDD |
| `../survey/survey_line6_complexity_latency_energy.md` | 复杂度 / 时延 / 能耗（真实硬件）|
| `../survey/survey_line7_apple_silicon_inference.md` | Apple Silicon 推理（★ 载体维度）|

## 按用途分组（★ 人工维护的导读部分）

### ① 本工作流的直接对位前作（★ 最该先读）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `The OCUDU dApp Platform-An Open Runtime and E3 Interface for Real-Time AI-RAN.pdf` | 2609.07843 | ★ **同一个接缝上的内联神经接收机**。Class A 契约（BF16 网格 + DM-RS 导频张量入，**解扰前软比特**出，100/150 µs）**独立验证了 `memo_01` 冻结的契约**；回退设计（经典同一调用兜底、8 次迟到开 lane breaker、权重双 bank）是 R9 的范例 |
| `Real-Time dApps for AI-RAN-Measured Interface Requirements for Inline PHY and Slot-Level Control.pdf` | 2609.07805 | ★ 神经接收机→LLR 的**实测**：**81.6/112 µs P50/P99.9 @ 51 PRB/2 端口**；神经均衡器 45–52 µs、26 万+ 次空口调用 0 fallback；外部 dApp 对神经接收机 **Inexpressible** |
| `Decision Feedback In-Context Learning for Wireless Symbol Detection.pdf` | 2503.16594 | 用户指定的参考论文（DEFINED）。★ 注意：**靶心是导频稀缺区**、**无任何时延/FLOPs 数据**、检测自回归串行（`memo_02`） |
| `Decision Feedback In-Context Symbol Detection over Block-Fading Channels.pdf` | 2411.07600 | DEFINED 的 **IEEE ICC 2025** 前身版本 |

### ② LLR 输出接收机与损失（我们属于这一线，LINE B）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `DeepRx-Fully Convolutional Deep Learning Receiver.pdf` | 2005.01494 | ★ 全卷积 ResNet，从频域网格到 **"soft bits compatible with 5G channel coding"**——A2 形态的原始出处 |
| `DeepRx MIMO-Convolutional MIMO Detection with Learned Multiplicative Transformations.pdf` | 2010.16283 | DeepRx 的 MIMO 扩展；★ 也是"神经接收机在 ~14 dB 出现 **BER floor**"的来源 |
| `Energy Efficiency in AI for 5G and Beyond-A DeepRx Case Study.pdf` | 2507.10409 | DeepRx 的 FLOPs/Watt 与**知识蒸馏压缩**（对应 R2 模型过大） |
| `Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR.pdf` | 2409.02912 | ★ **合规 + 实时**的标杆：`ReadoutLLRs` 输出**编码比特**的 LLR、**对 LDPC 编码后比特做 BCE**；A100 上 1 ms；**~350 µs/迭代 + 270 µs 开销 → 最多 2 次迭代**；**实时模型仅 1.4e5 权重** |
| `Sionna Research Kit-A GPU-Accelerated Research Platform for AI-RAN.pdf` | 2505.15848 | ★ 神经解映射器的**具体接口**：float16 入、**int16 LLR 出（`np.ldexp(llrs, 8)`，2⁸ 缩放）**；实测 **2.42× 相对 OAI 的标度失配**；`LDPC5GDecoder` 内部 **`llr_max = 20.0`** |
| `Neural Augmentation of MIMO-OFDM Receivers for Universal LLR Reconstruction.pdf` | 2606.29345 | LLR 重构作为模块化阶段；★ **互信息不能预测 BLER** 的来源 |
| `CMDNet-Learning a Probabilistic Relaxation of Discrete Variables for Soft Detection with Low Complexity.pdf` | 2102.12756 | ★★ **最该读的一条负面证据**：实测 DetNet 的"软"输出**其实是硬的**——*"~97% LLRs being −1 and 1"*；且它**带真实 128×64 rate-1/2 LDPC 译码器并报 coded FER** |
| `DUIDD-Deep-Unfolded Interleaved Detection and Decoding for MIMO Wireless Systems.pdf` | 2212.07816 | 外信息 LLR 反馈给检测器（IDD 式深度展开）——LINE A 里少数 LLR-native 的分支 |
| `Trainable Communication Systems-Concepts and Prototype.pdf` | 1911.13055 | Cammerer 的可微 IDD；★ **OTA 实测 +1.3 dB（vs 256QAM + 802.11n LDPC）**；也是"只最小化最终损失会导致性能差"的来源 |
| `A Neural Receiver for 5G NR Multi-user MIMO.pdf` | 2312.02601 | ★ **NVIDIA 的 NR MU-MIMO 神经接收机**（Globecom 2023）——§7.2 那条"比自己的 LMMSE+K-best 基线**差 ≤1 dB**、卖复杂度"的原文 |
| `Novel Deep Neural OFDM Receiver Architectures for LLR Estimation.pdf` | 2503.20500 | 直接以 **LLR 估计**为目标的 OFDM 接收机架构对比（★ 架构维度：DAT/RDNLA）|
| `Hybrid Neural-Traditional OFDM Receiver with Learnable Decider.pdf` | 2509.18574 | ★ **混合式**：神经与传统之间有一个**可学习裁决器**——与我们的**逐槽并行仲裁**（§13.2 P0 子原则 2）同题 |
| `EqDeepRx-Learning a Scalable and Interference Mitigating MIMO Receiver.pdf` | 2602.11834 | ★ **可扩展 + 抗干扰**的 DeepRx 变体 |
| `Neural Network Approaches for Data Estimation in Unique Word OFDM Systems.pdf` | 2211.06054 | 独特字 OFDM 的数据估计（★ 与"CP 里也有信息"同族）|
| `SICNN-Soft Interference Cancellation Inspired Neural Network Equalizers.pdf` | 2308.12591 | ★ **以均衡器为对象**的神经网络（L2 层的对位工作）|

### ③ 量化 / LLR 标定 / 失配（★★ 对应 S0 与 D3 的位宽决定）

★★★ **这一组是 2026-10-10 按主轴重新检索后补入的重点** —— **"神经接收机的量化"已有四篇**，
★ **且其中三篇出自同一组（Aalto/Nokia：Yellapragada / Ollila / Costa）**：
**它们已经覆盖了 PTQ、QAT、4/6/8-bit、可学习裁剪与 per-channel 尺度。**
★★ **⇒ "做量化"本身不能当卖点；我们剩下的空间在【激活与饱和】而不是【权重】**（见下方逐条差别）。

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Efficient Deep Neural Receiver with Post-Training Quantization.pdf` | 2508.06275 | ★★★ **最直接的对位前作**：**同为深度 3 + 64QAM + LLR 输出**（CE/解映射/均衡/解映射一体），128 通道 × 8 ResNet 块；★★ **做 PTQ：int8（per-tensor 与 per-channel）BLER 与 float32 相当；4-bit 【per-channel】仍可用，而 4-bit per-tensor 显著退化**。★ **限定：它只量化【权重】** |
| `Efficient Quantization-Aware Neural Receivers-Beyond Post-Training Quantization.pdf` | 2509.13786 | ★★★ **同组把 PTQ 推进到 QAT**：**可学习裁剪 + per-channel 尺度**，4-bit 与 8-bit QAT **在 10% BLER 目标上与 FP32 相当**，★ **且 QAT 在 PTQ 达不到目标的 NLoS 场景仍然达标**，压缩 8×（3GPP TR 38.901 CDL-B/D，速度至 40 m/s）|
| `Floating-Point Microformat Quantization and Pruning for Efficient MU-MIMO Neural Receivers.pdf` | 2609.31177 | ★★★ **"格式 vs 位宽"最锐利的一篇，★ 且它量化了【权重 + 激活】**：**INT8/INT4 vs FP8(E4M3)/FP4(E2M1) 权重，配 INT8 post-ReLU 激活**；★ **8-bit 权重+激活在 10%/1% BLER 上距 FP32 仅 0.05 dB**；★ **4-bit 时 uniform INT4 掉 3.3–3.7 dB 并跌破 LS-LMMSE，而 FP4 只掉 1.3–1.4 dB 且仍超 LS-LMMSE ~0.5 dB**（FP4 的近零网格更贴合训练后权重分布，且避免 INT4 的残差路径过度剪枝）。★ **⚠ 其 66× bit-op / 8.8× 存储来自【解析代价模型】，不是实测**（`survey_line6` §244）|
| ☐ *（非 arXiv，未存档）* **`Data-Free Quantization of Neural Receivers: When 4-Bit Succeeds, Why 6-Bit Matters for 6G`** | — | ★★★ **NeurIPS 2025 AI4NextG workshop poster**（同一组，**workshop 而非主会**）：★ **data-free per-channel PTQ 到 int8/int6/int4**；★★ **int8 与 int6 接近 float32**（**NLoS 高移动性下对 LS 最多 +4.9 dB**），★ **int4 在 LoS 各移动性下仍超传统接收机 1.7–2.6 dB、模型小 8×**，★ **并称 int4 有"great robustness"**。★★ **它与 2508.06275 的"4-bit 退化"结论相反** ⇒ **量化下限取决于方案（per-tensor/per-channel / 可学习裁剪 / PTQ/QAT / data-free 校准），★ 不是单一数字**。★ 链接：[NeurIPS 虚拟页](https://neurips.cc/virtual/2025/loc/san-diego/123232) · [OpenReview](https://openreview.net/forum?id=4NsiKauceB)（OpenReview 的 PDF 直链对本机返回 403 + 浏览器校验，**未能存档**）|
| `Learning Successive Interference Cancellation for Low-Complexity Soft-Output MIMO Detection.pdf` | 2601.16586 | ★ 最直接的标定原文：*"the computed LLRs are subject to **scaling and clipping**, which is a well-known…"*；**逐信道模型 + 逐调制**选裁剪电平、跨度约 20×；回退 LLR 用 α=0.2 缩放并裁到 0.1·L_max |
| `Complexity Adjusted Soft-Output Sphere Decoding by Adaptive LLR Clipping.pdf` | 1011.2113 | 经典"裁 LLR"参考 |
| `Learning Quantization in LDPC Decoders.pdf` | 2208.05186 | 学习型 LLR 量化/压扩映射 |
| `MF-QAT-Multi-Format Quantization-Aware Training for Elastic Inference.pdf` | 2604.00529 | ★ **多格式 QAT**（弹性推理）——载体侧的量化方法（`survey_line7`）|
| `Bit Error and Block Error Rate Training for ML-Assisted Communication.pdf` | 2210.14103 | 直接用 BER/BLER 做训练目标 |
| `Deep Learning for Channel Coding via Neural Mutual Information Estimation.pdf` | 1903.02865 | Donsker-Varadhan 互信息下界的损失 |

### ④ 神经接收机的现实检验（★ 风险与门禁的依据）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `When Does a Neural Receiver Help? Calibration-Drift Benchmarking and Detect-and-Rollback for 5G-6G NR.pdf` | 2605.26157 | ★★ 16 场景：**3/16 增益、10/16 打平、QPSK 反差 ~2 dB、DMRS AddPos=2 静默 100% BLER、自信判错比例平台 ~7%**；★ **500 Hz Doppler 下经典先崩** ⇒ **朴素回滚是错的**，改为逐槽并行仲裁（<5% 时延）；也是 Apple M3 Ultra **CPU 72.10 ms/槽** 的来源 |
| `On the Impact of Site-Specific Training for a Real-World 5G NR System.pdf` | 2609.04004 | ★ ETH 2026：站点特化微调只买到 **0.004 绝对 BLER**；**站点特化 LMMSE + 经典 IDD 是所有被测接收机里最好的** |
| `Site-Specific Finetuning of Neural Receivers with Real-World 5G NR Measurements.pdf` | 2603.09644 | ★★ **同题的【正面】结果，★ 与 ETH 那条必须一起读**：★ 真实空口 5G NR PUSCH（整时隙 273 PRB）；★ **站点特化微调给 1.26 dB（Shallow NRX，2 次迭代）与 1.05 dB（Deep NRX，8 次迭代）**；★ **微调后的 Shallow NRX 甚至超过未微调的 Deep NRX，而推理时延不到其 1/3（0.7 ms vs 2.2 ms，NVIDIA GH200）**；★ **实测数据集 BLER 减半以上**，且**跨部署场景/射频单元/终端类型仍成立**；★ 真值标签来自 **HARQ 提取失败传输的比特标签**。★★ **⇒ 与 ETH 的 0.004 绝对 BLER 形成对照：★ 站点特化的收益高度依赖方法（★ 标签获取方式是关键）** |
| `Adapting to Reality-Over-the-Air Validation of AI-Based Receivers Trained with Simulated Channels.pdf` | 2408.04182 | ★ Nokia OTA：LOS 训练的 DeepRx *"failed the over-the-air tests despite converging well during training"*；**宽随机化胜过参数匹配** |
| `Input-Correlated Supervision Noise Limits the Benefits of OTA Training for Learned Receivers.pdf` | 2608.12918 | ★ OTA 训练本身有负面结果 ⇒ P1 必须把"仿真训练 vs OTA 训练"当受控变量 |
| `Has The Physical Layer Matured?.pdf` | 2609.28852 | ★ **对 PHY 领域"是否已成熟"的反思**（含 LLR→译码器接口的通行写法）——写相关工作时的定位参考 |
| `A Compute&Memory Efficient Model-Driven Neural 5G Receiver for Edge AI-assisted RAN.pdf` | 2508.12892 | ★ **边缘 RAN** 的算力/内存效率（★ 与我们的载体动机同题）|

### ⑤ Apple Silicon（★ 载体维度 —— 只作可行性依据）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Apple Neural Engine-Architecture, Programming, and Performance.pdf` | 2606.22283 | ★ ANE 逆向工程：**单次 dispatch 地板 M1 0.23–0.26 ms**、工作集悬崖 ~2 MB、roofline |
| `ANEForge-Python for direct computation on the Apple Neural Engine.pdf` | 2606.17090 | **M5 Pro 地板 ≈70 µs**、悬崖 4.72 MB |
| `Range, Not Precision-Block-Floating-Point Half-Precision FFT and SAR Imaging on Apple Silicon.pdf` | 2605.28451 | ★ **fp16 的约束是量程不是尾数**（中间量 5e6 ≫ 65504 → 全 NaN，用块浮点缩放修好）⇒ LLR 头必须 clamp |
| `Bandwidth, Not FLOPS-FFT Kernels, Matrix Units and SAR Imaging on Apple M6.pdf` | 2609.32237 | ★ Apple GPU 的 DSP 是**带宽瓶颈而非算力瓶颈**——与本仓库 `metal_kernel_fusion` 的结论独立吻合 |
| `How Weight Encoding Affects Language Model Placement and Performance on the Apple Neural Engine.pdf` | 2608.22110 | ★ **权重编码方式影响 ANE 放置**——对 int8 导出有直接的实现含义（`survey_line7`）|
| `From 8 Seconds to 370ms-Kernel-Fused SAR Imaging on Apple Silicon via Single-Dispatch FFT Pipelines.pdf` | 2604.03585 | ★ **单次 dispatch 的融合 FFT 管线**（★ 融合与派发的量化案例）|
| `Production-Grade Local LLM Inference on Apple Silicon-A Comparative Study of MLX, MLC-LLM, Ollama, llama.cpp, and PyTorch MPS.pdf` | 2511.05502 | ★ 端侧推理栈对比（载体参照，★ **非本领域**）|
| `VkVIO-Cross-platform GPU Acceleration for Visual-Inertial Odometry with Vulkan.pdf` | 2609.30459 | ★ 跨平台 GPU 加速（载体参照，★ **非本领域**）|

### ⑥ 复杂度 / 时延 / 能耗（真实硬件）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Six Times to Spare-Characterizing GPU-Accelerated 5G LDPC Decoding for Edge-RSU Communications.pdf` | 2602.04652 | ★ DGX Spark 上 **LDPC：CPU 0.71 ms/码字 @20 迭代（超过 0.5 ms 时隙），GPU 只占 6–24%** ⇒ 整链预算 |
| `Computationally Efficient Neural Receivers via Axial Self-Attention.pdf` | 2510.12941 | ★★ **架构维度的对位工作**：轴向（时间 × 频率）自注意力把复杂度从 **`O((TF)²)` 降到 `O(T²F + TF²)`**，减少 FLOPs 与注意力矩阵乘法次数；★ **已被 IEEE SPAWC 2026 接收**（v3）⇒ 这是"算子结构换效率"的活跃范例 |
| `A Compute&Memory Efficient Model-Driven Neural 5G Receiver for Edge AI-assisted RAN.pdf` | 2508.12892 | ★ **面向边缘 RAN 的算力/内存效率** |
| `Common-Loss Parameter-Efficiency Analysis of MLP and KAN Neural Receivers for Digital Communications.pdf` | 2609.25847 | ★★ **参数量效率**的横向比较（MLP vs KAN）⇒ ★ **直接对应 G-1（同判据下报参数量/FLOPs 曲线）** |
| `SpikingRx-From Neural to Spiking Receiver.pdf` | 2409.05610 | 能耗方向的替代路线 |
| `NVIDIA AI Aerial-AI-Native Wireless Communications.pdf` | 2510.01533 | NVIDIA 的 CUDA AI-RAN 栈全貌（对照我们的 Metal 路线） |
| `AI-ML Life Cycle Management for Interoperable AI Native RAN.pdf` | 2507.18538 | 模型生命周期管理（运维侧） |
| `AtlasRAN-Timing-Aware Evaluation of Open-source 5G Platforms for Integrated Wireless Testbeds.pdf` | 2603.14661 | ★ 开源 5G 平台的**时序感知**评测（★ 我们的活体 gNB 场景同题）|
| `VERITAS-Verifying the Performance of AI-native Transceiver Actions in Base-Stations.pdf` | 2501.09761 | ★ **AI 原生收发动作的性能验证**（★ 与我们的判据体系同题）|

### ⑦ 符号输出的模型驱动检测（LINE A —— ★ 我们**不是**这一线）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Deep MIMO Detection.pdf` | 1706.01151 | DetNet（**IEEE SPAWC 2017**）；输出**符号后验概率**，非逐比特 LLR；"a full iterative decoding scheme is outside the scope of this paper" |
| `LoRD-Net-Unfolded Deep Detection Network with Low-Resolution Receivers.pdf` | 2102.02993 | 低分辨率展开检测的**真实**出处（"DetNet with one-bit quantization, TSP 2019" **不存在**） |
| `Comprehensive Review of Deep Unfolding Techniques for Next-Generation Wireless Communication Systems.pdf` | 2502.05952 | 深度展开综述（⚠ **仅预印本**，无期刊 venue） |
| `A Model-Driven Deep Learning Network for MIMO Detection.pdf` | 1809.09336 | OAMP-Net（★ **GlobalSIP 2018**，不是 GLOBECOM）|
| `Understanding Deep MIMO Detection.pdf` | 2105.05044 | 对展开式检测"到底学到了什么"的分析 |
| `Model-Based Deep Learning.pdf` | 2012.08405 | 模型驱动深度学习的综述性框架 |
| `Model-Based Machine Learning for Communications.pdf` | 2101.04726 | 通信里的模型驱动机器学习 |
| `Adaptive and Flexible Model-Based AI for Deep Receivers in Dynamic Channels.pdf` | 2305.07309 | ★ **动态信道下的自适应**模型驱动接收机 |
| `Deep Unfolding with Kernel-based Quantization in MIMO Detection.pdf` | 2505.12736 | ★ **展开 + 核量化**（★ 量化在另一条线上的用法）|

### ⑧ ICL / prompt 式接收机

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `In-Context Learning for Gradient-Free Receiver Adaptation-Principles, Applications, and Theory.pdf` | 2506.15176 | ★ pilot-prompt 形式化；**无梯度更新**的适配 |
| `Cell-Free Multi-User MIMO Equalization via In-Context Learning.pdf` | 2404.05538 | 确认复数→实数 token 化 = I/Q 拼接 |
| `Chain-of-Thought Enhanced Shallow Transformers for Wireless Symbol Detection.pdf` | 2506.21093 | 浅层 Transformer 路线（⚠ 摘要无参数/FLOPs/时延，**不可用于硬件决策**）|
| `Transformers are Provably Optimal In-context Estimators for Wireless Communications.pdf` | 2311.00226 | ★ ICL 的**理论**侧 |
| `Leveraging Large Language Models for Wireless Symbol Detection via In-Context Learning.pdf` | 2409.00124 | LLM 做符号检测（ICL）|
| `Turbo-ICL-In-Context Learning-Based Turbo Equalization.pdf` | 2505.06175 | ★ ICL 与** Turbo 均衡**结合（★ 与 IDD 同族）|
| `Neural Network-based Information-Theoretic Transceivers for High-Order Modulation Schemes.pdf` | 2506.00368 | ★ **高阶调制**下的信息论收发机（我们的 64QAM 场景对照）|

### ⑨ CP / 时域（★★ 直接对应我们的 CP + FFT 前向扩展，`model_network_design.md` §11–§12）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Artificial Intelligence-aided Receiver for A CP-Free OFDM System-Design, Simulation, and Experimental Test.pdf` | 1903.04766 | ★★★ **最直接的先例**：★ **AI 接收机 + 【CP-free】OFDM**（即**不靠 CP 也能判决**，★ 反过来说明 CP 里的信息是可被替代/可被利用的），★ **且含仿真 + 实验验证** ⇒ ★★ **我们的"CP 里也有 data、合并增益 ~0.69 dB/RE"这条要引用它，★ 且必须先读它再动笔** |
| `End-to-end Learning for OFDM-From Neural Receivers to Pilotless Communication.pdf` | 2009.05261 | ★ A2 形态的**端到端 OFDM 学习**；★★ **也是"常见误引"的受害者**（见下方纪律 3：真标题是 *…to **Pilotless Communication***，不是 *…to Hardware Feasibility*）|
| `An Introduction to Deep Learning for the Physical Layer.pdf` | 1702.00832 | ★ 物理层深度学习的**入门范式**（自动编码器视角）|
| `Sionna-An Open-Source Library for Next-Generation Physical Layer Research.pdf` | 2203.11854 | ★★ **我们要用来生成数据的工具本身的论文**（★ Sionna 2.x，Apache-2.0）⇒ ★ **引用它以保证数据生成的可复现** |
| `Model-free Training of End-to-end Communication Systems.pdf` | 1812.05929 | ★ **无模型训练**（★ 训练时不需要可微信道）|
| `End-to-end Optimization of Constellation Shaping for Wiener Phase Noise Channels with a Differentiable Blind Phase Search.pdf` | 2212.03839 | ★ 可微 BPS（★ 相位噪声下的端到端优化）|
| `Uplink OFDM Channel Prediction with Hybrid CNN-LSTM for 6G Non-Terrestrial Networks.pdf` | 2502.09326 | ★ **信道预测**（★ 与"跨时隙连续性"同族）|
| `Scalable Cross-Attention Transformer for Cooperative Multi-AP OFDM Uplink Reception.pdf` | 2602.04728 | ★★ **可扩展 + 交叉注意力**的协作多 AP 上行接收（★ 架构轴的对位）|
| `AdaFortiTran-An Adaptive Transformer Model for Robust OFDM Channel Estimation.pdf` | 2505.09076 | ★ 自适应 Transformer 信道估计 |
| `A Universal Neural Receiver that Learns at the Speed of Wireless.pdf` | 2602.15458 | ★★ **"通用神经接收机"**方向（★ 泛化 vs 我们的小模型专精，★ 是同一取舍的两端）|
| `FM-Receiver-A Foundation Model Enabled Unified Inner and Outer Neural Receiver Towards AI-Native Wireless Communications.pdf` | 2607.12555 | ★ **"内接收机 + 外接收机"统一**（★ 即把译码纳入网络的方向，★ 对应我们的向后扩展）|
| `Large Wireless Foundation Models-Stronger over Bigger.pdf`、`Large AI Models for Wireless Physical Layer.pdf`、`Foundation Models for Wireless Communications-From PHY Intelligence to Network Autonomy.pdf` | 2601.10963 / 2508.02314 / 2606.06239 | ★ 无线**基础模型**三篇（★ 与"小模型"路线形成对照，★ 写相关工作时的另一极）|

### ⑩ 硬件实现 / 加速器 / 定点（★★ 载体侧的算法-硬件协同）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Towards Hardware Implementation of Neural Network-based Communication Algorithms.pdf` | 1902.06939 | ★ 通信算法的**硬件实现**（定点化视角）|
| `A Deep-Unfolding-Optimized Coordinate-Descent Data-Detector ASIC for mmWave Massive MIMO.pdf` | 2501.14861 | ★ **展开式检测器的 ASIC** |
| `Transformer Accelerator (TFA)-A Macro-Op INT8 Hardware Chip for Transformer Inference and Machine Translation.pdf` | 2608.23582 | ★★ **INT8 加速器的硬件视角**（★ 对 int8 路线的算力论证有参考价值）|
| `Implementation of an Adaptive Transformer Accelerator for Accurate Outdoor Localization with Massive MIMO.pdf` | 2605.13507 | ★ 自适应 Transformer 加速器（★ 无线场景）|
| `A Heterogeneous Neural Network Accelerator for End-to-End Multitask RF Signal Recognition.pdf` | 2607.24669 | ★ **异构加速器**（★ 与我们的"异构 gNB 算力版图"同题）|
| `MDTransformer-A Hardware-Software Co-Design of Mode-Division Photonic Transformer Accelerator with Inverse-Designed Coherent Crossbar.pdf` | 2607.26016 | ★ 光计算加速器（★ **仅作加速器设计的旁证**）|

### ⑪ 更早的检测 / 译码 / 学习基础（★ 背景，写相关工作时的骨架）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Learning to Detect.pdf` | 1805.07631 | ★ 学习式检测的早期代表 |
| `Communication Algorithms via Deep Learning.pdf` | 1805.09317 | ★ 用深度学习重写通信算法 |
| `Deepcode-Feedback Codes via Deep Learning.pdf` | 1807.00801 | ★ 反馈码的学习式设计 |
| `Learning to Decode Linear Codes Using Deep Learning.pdf`、`Deep Learning Methods for Improved Decoding of Linear Codes.pdf`、`Deep Learning for Decoding of Linear Codes - A Syndrome-Based Approach.pdf`、`Neural Offset Min-Sum Decoding.pdf`、`Learning to Decode Protograph LDPC Codes.pdf`、`Boosted Neural Decoders-Achieving Extreme Reliability of LDPC Codes for 6G Networks.pdf`、`Deep Ensemble of Weighted Viterbi Decoders for Tail-Biting Convolutional Codes.pdf` | 1607.04793 / 1706.07043 / 1802.04741 / 1701.05931 / 2102.03828 / 2405.13413 / 2009.02591 | ★ **神经译码器一族**（★ 向后扩展时的对位工作，★ 当前不是主线）|
| `Machine LLRning-Learning to Softly Demodulate.pdf` | 1907.01512 | ★★ **以 LLR-MSE 为训练目标的软解调原始出处**（★ LLRNet 用的就是它，★ IEEE GC Wkshps 2019）|
| `Deep Learning-Based Communication Over the Air.pdf` | 1707.03384 | ★ **空口上的深度学习**（★ 早期端到端空口验证）|
| `Joint Learning of Probabilistic and Geometric Shaping for Coded Modulation Systems.pdf`、`End-to-end Learning of Probabilistic and Geometric Constellation Shaping with Iterative Receivers.pdf` | 2004.05062 / 2510.22608 | ★ **概率/几何整形 + 迭代接收机**（★ 与 IDD 同族，★ 星座设计一侧）|
| `Neural Mutual Information Estimation for Channel Coding-State-of-the-Art Estimators, Analysis, and Performance Comparison.pdf` | 2006.16015 | ★ 互信息估计器的**系统性比较**（★ 对应 `1903.02865` 那条损失，★ 也是"MI 不能预测 BLER"的旁证）|
| `Model-Driven Deep Learning for MIMO Detection.pdf`、`Deep HyperNetwork-Based MIMO Detection.pdf`、`Multilevel MIMO Detection with Deep Learning.pdf`、`AMP-Inspired Deep Networks for Sparse Linear Inverse Problems.pdf` | 1907.09439 / 2002.02750 / 1812.01571 / 1612.01183 | ★ 模型驱动/展开式检测族（★ LINE A）|
| `Adaptive Neural Signal Detection for Massive MIMO.pdf`、`Leveraging Deep Neural Networks for Massive MIMO Data Detection.pdf`、`On Purely Data-Driven Massive MIMO Detectors.pdf`、`Trainable Projected Gradient Detector for Massive Overloaded MIMO Channels-Data-driven Tuning Approach.pdf`、`Deep Learning-Aided Tabu Search Detection for Large MIMO Systems.pdf`、`Regularized Neural Detection for One-Bit Massive MIMO Communication Systems.pdf`、`Learning-Based One-Bit Maximum Likelihood Detection for Massive MIMO Systems-Dithering-Aided Adaptive Approach.pdf`、`Accelerated and Deep Expectation Maximization for One-Bit MIMO-OFDM Detection.pdf`、`Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems.pdf` | 1906.04610 / 2204.05350 / 2401.07515 / 1812.10044 / 1909.01683 / 2305.15543 / 2304.07696 / 2210.03888 / 2107.11958 | ★ **大规模 MIMO / 低位宽检测一族**（★ 我们不是这一线，★ 但"低位宽"是共同主题）|
| `Graph Neural Network-Enhanced Expectation Propagation Algorithm for MIMO Turbo Receivers.pdf`、`Convolutional Self-Attention-Based Multi-User MIMO Demapper.pdf`、`HybridDeepRx-Deep Learning Receiver for High-EVM Signals.pdf` | 2308.11335 / 2201.11779 / 2106.16079 | ★ 图神经网络 / 卷积自注意力 / 高 EVM（★ **非线性场景**的 DeepRx 变体）|
| `Learning-Enhanced Composite DNA Data Storage Under Sampling Randomness and IDS Errors.pdf` | 2602.11951 | ★ **存储**场景的同一族方法（★ 仅作方法迁移的旁证）|
| `Intelligent Radio Signal Processing-A Survey.pdf`、`Deep Learning-Aided 6G Wireless Networks-A Comprehensive Survey of Revolutionary PHY Architectures.pdf` | 2008.08264 / 2201.03866 | ★ 综述两篇（★ 入门与查漏）|
| `LoFi User Scheduling for Multiuser MIMO Wireless Systems.pdf`、`Adaptive Phase Shift Information Compression for IRS Systems-A Prompt Conditioned Variable Rate Framework.pdf`、`Channel-Adaptive Wireless Image Semantic Transmission with Learnable Prompts.pdf`、`Large Language Models for Wireless Networks-An Overview from the Prompt Engineering Perspective.pdf`、`Edge-Efficient Transformer for End-to-End RF Spectrum Monitoring.pdf`、`Neuromorphic In-Context Learning for Energy-Efficient MIMO Symbol Detection.pdf`、`PolymoRF-Polymorphic Wireless Receivers Through Physical-Layer Deep Learning.pdf`、`Uncertainty-Aware Likelihood Ratio Estimation for Pixel-Wise Out-of-Distribution Detection.pdf`、`Predicting Upcoming Stuttering Events from Three-Second Audio-Stratified Evaluation Reveals Severity-Selective Precursors, and the Model Deploys Fully On-Device.pdf` | 2401.04077 / 2511.03923 / 2411.10178 / 2411.04136 / 2607.18285 / 2404.06469 / 2005.02262 / 2508.00587 / 2604.27279 | ★ **边缘条目**：调度、IRS、语义传输、prompt 综述、频谱监测、神经形态、多态接收机、OOD 似然比，★ **以及两篇与本领域无关但曾用于参照的**（★ 见纪律 5）|
| `CSI-Free Symbol Detection for Atomic MIMO Receivers via In-Context Learning.pdf` | 2507.04040 | ★ **CSI-free** 符号检测（★ 与 CP-free 同族思路）|
| `Toward AI-Native 6G Air Interface-A 3GPP Perspective on Protocol Framework.pdf` | 2606.27466 | ★ **3GPP 视角的 AI 原生空口**（★ 标准化语境）|
| `RADE-A Neural Codec for Transmitting Speech over HF Radio Channels.pdf` | 2505.06671 | ★★ **"最接近的移动端 PHY 神经网络"反例**（★ §12：iOS app 用纯 C，★ **没有用 ANE**）|

### ⑫ 信道估计 / 其他架构轴（★ 与深度 3 的前级直接相关）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `HELENA-High-Efficiency Learning-based channel Estimation using dual Neural Attention.pdf` | 2506.13408 | ★★ **我们现役的神经 CE**（HELENA）——★ **它的参数量与 82 MFLOP 是我们 v1 的对照锚点** |
| `HELENA for 5G NR LEO NTN Channel Estimation-A Comparative Evaluation.pdf` | 2609.14735 | HELENA 在 **LEO NTN** 上的评测（★ 同族的第二篇）|
| `ReQuestNet-A Foundational Learning model for Channel Estimation.pdf` | 2508.08790 | ★ 信道估计的**基础模型**方向 |
| `Low Complexity Deep Learning Augmented Wireless Channel Estimation for Pilot-Based OFDM on Zynq System on Chip.pdf` | 2403.01098 | ★ **低复杂度**导频 OFDM 信道估计（★ 且落在 Zynq SoC 上）|
| `SwiftChannel-Algorithm-Hardware Co-Design for Deep Learning-Based 5G Channel Estimation.pdf` | 2605.01931 | ★ **算法-硬件协同设计**（★ G-1 的同族）|
| `A Neural Network Aided Approach for LDPC Coded DCO-OFDM with Clipping Distortion.pdf` | 1809.01022 | LDPC 编码 + 削波失真的神经网络辅助 |
| `Deep Learning for Joint Narrowband Interference Cancellation and Soft Demodulation in OFDM Systems.pdf` | 2607.08717 | ★ **干扰消除 + 软解调联合**（★ 与我们的解映射阶段同题）|
| `Interference Cancellation Based Neural Receiver for Superimposed Pilot in Multi-Layer Transmission.pdf` | 2406.18993 | ★ **叠加导频 + 干扰消除**的神经接收机 |
| `Low-Overhead Receiver Design for Data-Dependent Superimposed Training via Deep Learning.pdf` | 2605.29995 | ★ 数据相关叠加训练（★ 与"CP/导频里也有信息"同族）|
| `Uncertainty-Aware and Reliable Neural MIMO Receivers via Modular Bayesian Deep Learning.pdf` | 2302.02436 | ★ **不确定性感知**（★ 与我们的逐槽合法性守卫/仲裁同题）|
| `Robust MIMO Detection With Imperfect CSI-A Neural Network Solution.pdf` | 2307.12575 | ★ **CSI 不完美**时的鲁棒性 |
| `A Unified Transformer Architecture for Low-Latency and Scalable Wireless Signal Processing.pdf` | 2508.17960 | ★ 统一 Transformer，强调**低时延与可扩展** |
| `CPMamba-Selective State Space Models for MIMO Channel Prediction in High-Mobility Environments.pdf` | 2512.16315 | ★ **状态空间模型**（Mamba）用于高移动性信道预测 |
| `Soft Graph Transformer for MIMO Detection.pdf` | 2509.12694 | ★ 图 Transformer + 软输出 |
| `An End-to-End Neural Network Transceiver Design for OFDM System with FPGA-Accelerated Implementation.pdf` | 2512.13263 | ★ **端到端收发机 + FPGA 加速推理**（★ 算法-硬件协同的另一例）|

---

## 引用纪律

1. 写进对外材料前，**必须打开 PDF 核对原文**——本索引只是指路，不是证据。
2. `survey/` 里标注 `[unverified]` 的条目，以本文档目录下的原文为准。
3. 已发现的**常见误引**（全文核对后确认，勿再传播）：
   - DetNet 2017 的 venue 是 **SPAWC**，不是 SPL；
   - **"DetNet with one-bit quantization, IEEE TSP 2019" 不存在**（该文全文零次出现 one-bit/quantiz/ADC）；
   - OAMP-Net 是 **GlobalSIP 2018**，不是 GLOBECOM；OAMP-Net2 是 **TSP 2020**，不是 JSTSP；
   - "End-to-End Learning for OFDM: From Neural Receivers to Hardware Feasibility" **不存在**，
     真实标题是 *…to **Pilotless Communication***（IEEE TWC 2021）。
4. ★ **量化数字的纪律（★ 2026-10-10 新增，★ 因为它是当前最容易误引的一处）**：
   ★★ **"4-bit 可行/不可行"没有单一答案** —— 它取决于 **per-tensor vs per-channel**、
   **是否可学习裁剪**、**PTQ 还是 QAT**、**是否 data-free 校准**。
   ★ 已存档的四篇给出**方向相反**的结论：
   2508.06275（4-bit per-tensor 显著退化）／2509.13786（4-bit QAT 达标）／
   2609.31177（INT4 掉 3.3–3.7 dB）／NeurIPS 2025 poster（4-bit 在 LoS 仍超传统接收机）。
   ⇒ ★★ **引用时必须同时写出【量化方案】与【比特数】，★ 且注明是实测还是解析模型。**
5. ★ **载体与算法的分辨（★ 2026-10-10 新增）**：
   ★★ **本目录里 Apple Silicon / ANE / CoreML 的条目只支持【可行性】**，
   ★ 引用它们**不能**构成算法或架构上的贡献声明（见 `../model_build_reconnaissance.md` §17）。
