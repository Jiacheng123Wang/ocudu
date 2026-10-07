# `ref_paper/` — 参考论文原文（PDF）

工作流引用的论文原文，**文件名 = 论文标题**（由 `fetch_refs.sh` 从 arXiv API 取标题后落盘，
不是手打的，所以文件名不会与论文漂移）。`:` 被替换为 `-`（macOS 会把 `:` 当成路径分隔符显示）。

- **38 篇，约 43 MB**，用 `bash fetch_refs.sh` 重新下载（幂等：已存在的会跳过）。
- ⚠ **PDF 不入 git**（见 `doc_chinese/.gitignore`）：第三方版权 + 体积；
  本目录**被跟踪的是这份 `README.md` 索引**（标题、arXiv ID、为什么引用它）。
  换机器时跑一次 `fetch_refs.sh` 即可复原。

## 按用途分组

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

### ③ LLR 标定 / 量化 / 失配（直接对应规划 §3.3、§3.4）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Learning Successive Interference Cancellation for Low-Complexity Soft-Output MIMO Detection.pdf` | 2601.16586 | ★ 最直接的标定原文：*"the computed LLRs are subject to **scaling and clipping**, which is a well-known…"*；**逐信道模型 + 逐调制**选裁剪电平、跨度约 20×；回退 LLR 用 α=0.2 缩放并裁到 0.1·L_max |
| `Complexity Adjusted Soft-Output Sphere Decoding by Adaptive LLR Clipping.pdf` | 1011.2113 | 经典"裁 LLR"参考 |
| `Learning Quantization in LDPC Decoders.pdf` | 2208.05186 | 学习型 LLR 量化/压扩映射 |
| `Floating-Point Microformat Quantization and Pruning for Efficient MU-MIMO Neural Receivers.pdf` | 2609.31177 | ★ 量化指导：8-bit 基本免费（≤0.05 dB）、**INT4 掉 3.3–3.7 dB 并跌破 LS-LMMSE**、FP4(E2M1) 可用 |
| `Bit Error and Block Error Rate Training for ML-Assisted Communication.pdf` | 2210.14103 | 直接用 BER/BLER 做训练目标 |
| `Deep Learning for Channel Coding via Neural Mutual Information Estimation.pdf` | 1903.02865 | Donsker-Varadhan 互信息下界的损失 |
| `Learning Quantization in LDPC Decoders.pdf`（同上） | 2208.05186 | — |

### ④ 神经接收机的现实检验（★ 风险与门禁的依据）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `When Does a Neural Receiver Help? Calibration-Drift Benchmarking and Detect-and-Rollback for 5G-6G NR.pdf` | 2605.26157 | ★★ 16 场景：**3/16 增益、10/16 打平、QPSK 反差 ~2 dB、DMRS AddPos=2 静默 100% BLER、自信判错比例平台 ~7%**；★ **500 Hz Doppler 下经典先崩** ⇒ **朴素回滚是错的**，改为逐槽并行仲裁（<5% 时延）；也是 Apple M3 Ultra **CPU 72.10 ms/槽** 的来源 |
| `On the Impact of Site-Specific Training for a Real-World 5G NR System.pdf` | 2609.04004 | ★ ETH 2026：站点特化微调只买到 **0.004 绝对 BLER**；**站点特化 LMMSE + 经典 IDD 是所有被测接收机里最好的** |
| `Adapting to Reality-Over-the-Air Validation of AI-Based Receivers Trained with Simulated Channels.pdf` | 2408.04182 | ★ Nokia OTA：LOS 训练的 DeepRx *"failed the over-the-air tests despite converging well during training"*；**宽随机化胜过参数匹配** |
| `Input-Correlated Supervision Noise Limits the Benefits of OTA Training for Learned Receivers.pdf` | 2608.12918 | ★ OTA 训练本身有负面结果 ⇒ P1 必须把"仿真训练 vs OTA 训练"当受控变量 |

### ⑤ Apple Silicon（★ 本工作流的差异化空间）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Apple Neural Engine-Architecture, Programming, and Performance.pdf` | 2606.22283 | ★ ANE 逆向工程：**单次 dispatch 地板 M1 0.23–0.26 ms**、工作集悬崖 ~2 MB、roofline |
| `ANEForge-Python for direct computation on the Apple Neural Engine.pdf` | 2606.17090 | **M5 Pro 地板 ≈70 µs**、悬崖 4.72 MB |
| `Range, Not Precision-Block-Floating-Point Half-Precision FFT and SAR Imaging on Apple Silicon.pdf` | 2605.28451 | ★ **fp16 的约束是量程不是尾数**（中间量 5e6 ≫ 65504 → 全 NaN，用块浮点缩放修好）⇒ LLR 头必须 clamp |
| `Bandwidth, Not FLOPS-FFT Kernels, Matrix Units and SAR Imaging on Apple M6.pdf` | 2609.32237 | ★ Apple GPU 的 DSP 是**带宽瓶颈而非算力瓶颈**——与本仓库 `metal_kernel_fusion` 的结论独立吻合 |

### ⑥ 复杂度 / 时延 / 能耗（真实硬件）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Six Times to Spare-Characterizing GPU-Accelerated 5G LDPC Decoding for Edge-RSU Communications.pdf` | 2602.04652 | ★ DGX Spark 上 **LDPC：CPU 0.71 ms/码字 @20 迭代（超过 0.5 ms 时隙），GPU 只占 6–24%** ⇒ 整链预算 |
| `Computationally Efficient Neural Receivers via Axial Self-Attention.pdf` | 2510.12941 | 轴向自注意力把 O((TF)²) 降到 O(T²F+TF²)——A3 的复杂度缓解 |
| `SpikingRx-From Neural to Spiking Receiver.pdf` | 2409.05610 | 能耗方向的替代路线 |
| `NVIDIA AI Aerial-AI-Native Wireless Communications.pdf` | 2510.01533 | NVIDIA 的 CUDA AI-RAN 栈全貌（对照我们的 Metal 路线） |
| `AI-ML Life Cycle Management for Interoperable AI Native RAN.pdf` | 2507.18538 | 模型生命周期管理（运维侧） |

### ⑦ 符号输出的模型驱动检测（LINE A —— ★ 我们**不是**这一线）

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `Deep MIMO Detection.pdf` | 1706.01151 | DetNet（**IEEE SPAWC 2017**）；输出**符号后验概率**，非逐比特 LLR；"a full iterative decoding scheme is outside the scope of this paper" |
| `LoRD-Net-Unfolded Deep Detection Network with Low-Resolution Receivers.pdf` | 2102.02993 | 低分辨率展开检测的**真实**出处（"DetNet with one-bit quantization, TSP 2019" **不存在**） |
| `Comprehensive Review of Deep Unfolding Techniques for Next-Generation Wireless Communication Systems.pdf` | 2502.05952 | 深度展开综述（⚠ **仅预印本**，无期刊 venue） |

### ⑧ ICL / prompt 式接收机

| 文件 | arXiv | 为什么引用 |
|---|---|---|
| `In-Context Learning for Gradient-Free Receiver Adaptation-Principles, Applications, and Theory.pdf` | 2506.15176 | ★ pilot-prompt 形式化；**无梯度更新**的适配 |
| `Cell-Free Multi-User MIMO Equalization via In-Context Learning.pdf` | 2404.05538 | 确认复数→实数 token 化 = I/Q 拼接 |
| `Chain-of-Thought Enhanced Shallow Transformers for Wireless Symbol Detection.pdf` | 2506.21093 | 浅层 Transformer 路线（⚠ 摘要无参数/FLOPs/时延，**不可用于硬件决策**） |

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
