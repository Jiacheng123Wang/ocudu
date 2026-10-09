# 深度 3 模型：调研笔记（★ 2026-10-08）

> ★ 用户指示：参考前面的 AI CE 工作、公开论文与开源工程。★ **本文件是调研记录**，
> ★ 结论落 `design_and_implementation.md` 的下一节。

## 1. ★★★ 最重要的发现：**上一轮 AI CE 工作已经把几乎所有基础设施建好了**

★ **不需要从零开始。** 资产清单（★ 全部在**本机**、已核实）：

| ★ 资产 | ★ 位置 | ★ 状态 |
|---|---|---|
| ★★ **CoreML / ANE 推理引擎**（C++，零拷贝 `MLMultiArray`）| `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_coreml_nn_engine.{h,mm}` | ✅ **已入链** |
| ★★ **NN 估计器实现**（★ 经典前级 → NN 网格 → 回退 + **热重载 API**）| `.../port_channel_estimator_helena_impl.*` | ✅ **已入链** |
| ★ **编译好的 `.mlmodelc` 资产**（8 个）| `.../metal/ai_assets/` | ✅ 已在仓库 |
| ★★ **训练工具链** | `.../metal/ai_train/`（21 个脚本）| ✅ 已入仓库 |
| ★★ **Python venv**（TF 2.21 / tf-keras / **coremltools 9.0**）| `~/ai_ce_work/venv` | ✅ **可用**（已实测） |
| ★ **合成数据集**（5.3 GB，11 264 样本）| `~/ai_ce_work/dataset/290525_dataset_ce.mat` | ✅ |
| ★★ **真实采集语料**（★ 约 **7 个站点**，各 2–6 GB）| `~/ai_ce_work/capture/site{1..6}_0831`, `oaiue_helena_retest` 等 | ✅ |
| ★★ **真实训练三元组**（★ 由采集配对＋重建而成）| `realtrain_s56.npz`(4.2G), `realtrain_s56_g13.npz`(3.0G) | ✅ |

★★★ **而且已经实测过 ANE 的性能**（★ 本轮**独立复现**，见 §3）。

## 2. ★★ 上一轮的技术选择与取舍（★ 可直接复用）

### 2.1 ★ 他们选的架构：HELENA（残差 CNN + 轻量注意力）

```
输入 (1, 612, 14, 2) 线性插值 LS 网格（实/虚两通道）
→ Conv2D(32, 12×2) → Conv2D(2, 6×7)
→ 按 PRB 切 patch（51 token × 336 维）→ Dense 336→64
→ 4 头 MHSA → 残差 + LayerNorm → SE 通道重标定
→ Dense → 重构网格 → ★ 加回输入（全局残差）
```
★★ **0.116M 参数、77.2 MFLOPs**；★ **算子全是 MPSGraph/CoreML 原生**
（Conv2D / ReLU / GEMM / Softmax / LayerNorm / Sigmoid / 残差加）—— ★★ **这正是不掉出 ANE 的原因**。

### 2.2 ★★ 他们排除的候选（★ 理由对深度 3 同样成立，★ 直接继承）

| ★ 候选 | ★ 为什么否决 |
|---|---|
| ★ CNN4CE | ★ 在**天线阵列空间**上卷积、Q=2 子载波，★ 与 NR 上行网格**不匹配**；★ **按 SNR 分别训练**（部署不可行）；★ TF1.x EOL、GPL-3.0、无权重 |
| ★ AdaFortiTran | ★ **O(S²) 自注意力**：273 PRB 是 **46.7 GMAC/端口**，★ 超预算 1–3 个数量级 |
| ★ Multi-Task Transformer Receiver | ★ CE 头只输出**单 CFR 向量**（无时间维），★ 生成不出 `612×14` 网格；★ 与 NR 分散 DMRS 不符 |
| ★ 生成式 / GAN / 扩散 | ★ **迭代推理**，★ 时延不可行 |
| ★ bi-LSTM | ★ 273 PRB 下 6552 递归步，★ **串行**，★ GPU 利用率 <10% |

★★★ **一条对深度 3 极其重要的证据**（★ 他们自己的引用）：
★★ **"DL 在低 SNR（−5 dB 级）对 LMMSE 优势达数 dB，但 15–20 dB 以上优势归零（恰好是部署区间）"**
—— ★★ **这与 P0 的裁决（§12/§13）完全一致，★ 是独立的第三方佐证。**

## 3. ★★★ ANE 路径本轮**独立复现**（★ 关键，因为它决定整个 C2 论证）

★ 我用 `~/ai_ce_work/models/helena_f16_ml.mlpackage` 直接跑 CoreML 基准：

| ★ 计算单元 | ★ p50 | ★ p99 |
|---|---|---|
| ★ `CPU_ONLY` | ★ 685.2 µs | 798.5 µs |
| ★ `CPU_AND_GPU` | ★ 685.3 µs | 727.9 µs |
| ★★ **`CPU_AND_NE`（ANE）** | ★★ **139.6 µs** | ★★ 215.9 µs |
| ★ `ALL` | ★ 141.7 µs | 191.4 µs |

★★★ **ANE = 139.6 µs，比 CPU 快 4.9 倍**（★ 与上一轮记录的 141 µs **一致** ✓）。
★★ **而 `CPU_AND_GPU` 与 `CPU_ONLY` 几乎相同** ⇒ ★★ **这个网络在 GPU 上拿不到收益**，
★★★ **ANE 是它唯一的加速引擎** —— ★ 这正是用户那个论点的**实测支撑**。

★ **工具链可用性**（★ 本机实测）：`python 3.11.15` / `tensorflow 2.21` / `tf_keras 2.21` /
`coremltools 9.0` / `numpy 2.4.6` / `h5py 3.14` ✓

## 4. ★★★ 上一轮踩过的坑（★ 对深度 3 **直接适用**，★ 这是最值钱的部分）

| # | ★ 坑 | ★ 对深度 3 的含义 |
|---|---|---|
| ★★ **18** | ★★★ **模型会系统性破坏"近乎完美"的输入**：HELENA 训练包络 −5..25 dB，★ 而干净信道是 ~45 dB ⇒ ★ **模型把 −45 dB 的输入破坏到 −22…−33 dB**，★ **导致 attach 死循环** | ★★★ **深度 3 也会有这个失效模式** ⇒ ★★ **必须有"输入分布包络"的定义与包络外的安全兜底** |
| ★★ **19** | ★★ 硬门控/硬微调都不行（★ 前者挡死、★ 后者灾难性遗忘）⇒ ★★ **最终方案 = 训练包络扩到 −5..55 dB + 运行时 SNR 软混合（α-blend）** | ★★ **深度 3 的分层兜底应当照搬这个模式** |
| ★ **15** | ★★★ **NMSE 聚合口径必须统一**：★ "逐样本 dB 的均值" 与 "合并误差功率的 dB" 在 SNR 跨 30 dB 时差 **~6 dB** | ★★ **深度 3 的评测也必须先定口径**（★ 这正是本工作流的 **F12**） |
| ★ **8** | ★★ **Metal/ANE 时延必须在空载机器上测**（★ 构建期间测出过 **45× 假回归**）| ★★ **C2 的能量测量同样要在空载下做** |
| ★ **11** | ★★ **孤儿 GPU kernel**：旧二进制 + 新 metallib 错配 ⇒ kernel 在 GPU 上无限执行，★ **只能重启** | ★ **改模型/kernel 后必须重编所有使用方** |
| ★ **4** | ★★ **coremltools 必须固定输入形状**（★ flexible shape 运行时 **SIGBUS**）；★ batch 变化要单独转一版 | ★★ **深度 3 的时宽/带宽变化要有分桶**（★ 他们用 52/106 两桶） |
| ★ **5** | ★ **onnxruntime 的 CoreML EP 数字不可信**（155 节点只分区 36 个）⇒ ★ **以 coremltools 转换 + 直接 predict 为准** | ★★ **别用 ONNX 路线** |
| ★ **3** | ★ **coremltools 9 移除了 ONNX 直转** | ★★ **必须走 TF SavedModel 路线** |
| ★ **12/13** | ★ **真值索引转置**：npy 是 `[n, 子载波, 符号, 2]`（子载波主序），C++ 按符号主序读 ⇒ **NMSE +0.6 dB 的假结果** | ★★★ **布局契约要先写清**（★ 与本工作流 F9 同族） |
| ★ **17** | ★ **pad-aware 训练 vs 全宽训练的权衡**：窄带 +2.5 dB 但全宽 −0.2 dB | ★★ **深度 3 也要决定分桶策略** |

## 5. ★★ 深度 3 与 AI CE 的**本质差别**（★ 必须想清楚的部分）

| ★ 维度 | ★ AI CE（已做） | ★★ 深度 3（要做的） |
|---|---|---|
| ★ 输出 | ★ 信道网格（回归）| ★★ **LLR**（★ 每 RE × 每比特）|
| ★ 损失 | ★ **NMSE**（★ 有监督，真值 = 信道）| ★★ **没有直接真值** ⇒ ★ 要么用 **决策导向标签**（★ 他们有 `build_labels.py`），★ 要么用 **可微的端到端损失**（★ 过 LDPC 不可微 ⇒ 需要近似）|
| ★ 参照物 | ★ `metal_mmse`（★ 赢 +2.6 dB）| ★★ **经典判决器（★ P0 已证明它 = 理想判决器）** ⇒ ★★ **余量按定义是零** |
| ★ 动机 | ★ **更准** | ★★ **更省（C2）** |

★★★ **所以深度 3 的正确定位不是"更准的接收机"，而是"同一个接收机的另一种代价结构"**
（★ 与 `gpu` lane 同构），★ 而**判据是 C2（能量/占用），不是 C1（准确率）**。

## 6. ★★★ 外部文献调研（第一轮，★ 结论比预期更强）

### 6.1 ★★★ 学习的接收机**对"理想 CSI 的 LMMSE"没有优势** —— 论文自己说的

★★ **这是本轮调研最重要的一条**，★ 它把"深度 3 会不会更准"这个问题**从我们的实测升级为文献共识**：

| ★ 论文 | ★ 它自己的表述 |
|---|---|
| ★★★ **DeepRx**（Nokia Bell Labs，IEEE TWC 2021，arXiv:2005.01494）★ **就是"一个 CNN 取代 CE+均衡+解映射、输出 LLR"那篇** | ★★ 编码 BER 相对**实用 LMMSE（LS+插值）**增益 **~2 dB**；★ 而它 **"can essentially match the performance of the LMMSE receiver with FULL channel knowledge"** ⇒ ★★★ **它打不过 genie/完美 CSI 的 LMMSE**。★ 它对 genie 的唯一优势来自**小区间干扰**（+2 dB），★ 而作者紧接着注明：★ 那个差 **"could likely be reduced if the LMMSE receivers were utilizing IRC"**。★ 并且：★★ **"When tested with data without interference, the margin is smaller."** |
| ★★ **NVIDIA 5G NR MU-MIMO 神经接收机**（arXiv:2312.02601，Globecom 2023）| ★★ **比它的 LMMSE+K-best 基线【差】≤1 dB**；★ 声称的收益是**复杂度**，★ 不是准确率 |
| ★★ 其**实时**后续（arXiv:2409.02912）| ★★ NRX **"approach[es] the performance of the LMMSE ... baseline"** |

★★★ **所以文献里的"学习接收机增益"，本质是"对【实用】经典链的差"**，
★ 而 DeepRx 自己**只补到 genie 界为止**。
★★★ **我们在 P0 里测出的"≈0"，不是实现失败，而是论文预测的结果。**
★★ **任何以【准确率】为动机的尝试，都必须换一个机制**：
★ **非线性/PA、干扰、高 Doppler、稀疏导频** —— ★ **而不是"更好的 CE+均衡+解映射"**。

★ **对我们三个动机的裁决**：
| ★ 动机 | ★ 文献支持 |
|---|---|
| ★★ **(a) 能量/算力分流** | ✅ **仍然成立**（★ 而且 NVIDIA 那篇的卖点就是复杂度）|
| ★★ **(b) 跨单元联合处理** | ✅ **仍然成立** |
| ★ **（c）长期押注** | ★ 是押注 |
| ★★ **"做一个更好的深度 3"** | ★★★ **文献【不支持】** |

### 6.2 ★★★ ANE 的硬约束（★ 本轮**在真机上独立验证**）

★★ 文献（arXiv:2606.22283，302 页实测 ANE 参考）给出三条，★ **我逐条实测**：

| ★ 约束 | ★ 我的实测 | ★ 结论 |
|---|---|---|
| ★★ **ANE 端到端只有 fp16** | ★ 输入放大到 `max\|x\|≈120 695` 时，★ **输出出现 488 个 `inf`**，★ 幅值顶在 **65 504**（★ fp16 上限）；★ 而 `max\|x\|≈40 232` 时**仍然全有限** | ✅ **确认**：★ 上限就是 **fp16 的 65 504**，★ 不是 2^15 |
| ★ **2 MB 片上工作集阈值** | ★ 未单独测（★ 我们的网格 fp32 约 68 KB/份，★ 多份拷贝仍需留意）| 🟡 记录 |
| ★ **~0.23 ms 派发地板（M1）** | ★★ **我的 M4 Pro 实测 p50 = 139.6 µs** ⇒ ★ **本机没有撞到这个地板** | ✅ 本机可行 |
| ★ **自定义 Metal kernel 不能进 ANE 常驻图** | ★ 与既有集成方式一致（★ 前级经典 + NN + 后级经典）| ✅ 已知 |

★★★ **对深度 3 设计的直接影响**（★ 这是本节真正的产出）：
1. ★★ **动态范围必须显式管理**：★ 文献特别点名 **"均衡/除以小信道估计"** 与 **"未截断的 LLR 幅值"**
   是 ANE 上最危险的两处；
2. ★★ **必须在写入前做尺度规划与饱和检查** —— ★ 因为 **fp16 溢出是 `inf`，而 `inf` 会污染后续 LLR 与译码**；
3. ★★ **LLR 必须先截断再交给译码**（★ 与 `_llr.bin` 实测的饱和率 0.398 相呼应）。

### 6.3 ★ 一个**形状匹配**的开源工程（★ 待完整报告确认）

★ 调研提到 **Rohde-Schwarz / NeuralReceiver**：★ *Apache-2.0，5G NR PUSCH，
★ 含训练 + ONNX 导出* —— ★★ **这是唯一一个"管线形状与我们一致"的仓库**。
★★ **注意**：★ 它是 **PyTorch**，★ 而 **coremltools 9 已移除 ONNX 直转**（§4 坑 3）
⇒ ★ **要走它必须再补一截 PyTorch → TF 或 PyTorch → CoreML 的转换**，★ **待完整报告确认可行路径**。

## 7. ★★★ 完整调研报告（★ 定案，★ 含对我前一轮笔记的两处更正）

### 7.1 ★★★ 决定性结论：**在 ~20 dB / 64QAM / 干净信道上，"学习的接收机胜过调好的经典接收机"没有可信证据**

★★★ **文献的天花板【就是】genie/完美 CSI 的 LMMSE，而学习型只能【逼近】它**：

| ★ 证据 | ★ 原文要点 |
|---|---|
| ★★ **DeepRx**（TWC 2021, arXiv:2005.01494）| ★ coded BER **"matches that of the LMMSE receiver with full channel knowledge"**；★ 那 ~2 dB 是**对【实用】LMMSE（LS+插值）**；★ 作者：**"When tested with data without interference, the margin is smaller."** |
| ★★ **NVIDIA NR 接收机**（arXiv:2312.02601）| ★ **"operates less than 1 dB AWAY FROM a baseline using LMMSE + K-best"** ⇒ ★ **更差**，★ 卖**复杂度**不是准确率 |
| ★★★ **ETH Zurich 真实 5G NR 试验台**（arXiv:2609.04004）| ★★ **"site-specific LMMSE + iterative detection and decoding achieves the LOWEST error rate observed in our datasets"** —— ★★ **胜过他们所测的每一个神经接收机，包括他们自己的 decoder-in-the-loop DUIDD** |

★★★ **所以"我们的理想判决器给出 ≈0 增益"正是论文预测的结果。★ 我们已经在天花板上。**
★★★ **不要再提"更好的深度 3"。**

★ **文献里的增益全部来自"某个被破坏的假设"**：★ 稀疏导频、高 Doppler、**PA 非线性**
（★ HybridDeepRx，arXiv:2106.16079 —— ★ **唯一在高 SNR 显示真实增益的机制**）、
★ 小区间干扰、★ 弱的实用 CE。★★ **这些在我们的场景里都不成立。**

### 7.2 ★★★ 负面/零结果（★ 应当引用，★ 其中一条直接反我）

| ★ 结果 | ★ 内容 |
|---|---|
| ★★ **decoder-in-the-loop 的【训练】无增益** | ★ NVIDIA：**"we empirically did not observe any gains by doing so"**；★ ETH DUIDD 只从站点微调得到 **0.004 绝对 BLER** ⇒ ★★ **decoder-in-the-loop 的【评估】是必须的，【训练】不是** |
| ★★ **标定漂移**（arXiv:2605.26157）| ★ 16 个场景里 **10 个统计打平（±0.2 dB）**；★ QPSK **差 2 dB**；★ 64QAM **架构性失败**；★ **"confidently wrong" 的比特稳定在 ~7%** |
| ★★ **DeepRx MIMO**（arXiv:2010.16283）| ★ **~14 dB 以上出现 BER 地板** —— ★★ **我们在 19–23 dB，即已在它退化的区间之外** |
| ★★ **Nokia OTA**（arXiv:2408.04182）| ★ LOS 训练的 DeepRx **"failed the over-the-air tests despite converging well during training"** |
| ★ **arXiv:2509.18574** | ★ 存在的目的就是**逐块挑选**经典还是神经 ⇒ ★ **证明 DL 不是一致更好** |
| ★★ **arXiv:2606.29345** | ★★ **MI 不能预测 BLER** ⇒ ★★ **唯一主指标 = 经真实 LDPC 译码后的 coded BLER** |

★★★ **而 ETH 那条"site-specific LMMSE + IDD 最低"同时告诉我们**：
★★ **§14.7(3) 那条"跨单元联合处理"的正解可能是 IDD（迭代检测译码），而不是一个更大的神经网络**
—— ★ **而且 IDD 不需要 ANE、不需要训练**。

### 7.3 ★★★ 证据支持什么（★ 三条动机的最终裁决）

| ★ 动机 | ★ 裁决 |
|---|---|
| ★★★ **(a) 能量/算力分流** | ★★★ **强支持 —— 这应当是【首要理由】**。★ 实测（arXiv:2606.22283）：★ conv-resnet 上 **ANE 2063 vs GPU 142 GFLOP/s/W = 14.5×（M1）**；★ **2289 vs 175 = 13×（M5）**；★ ANE 轨道 **<6 W** vs GPU **13–21 W**。★★ **与我们的实测方向一致**（ANE 141 µs vs MPS 690 µs = 4.9×）|
| ★★ **(b) 跨单元联合处理** | ★★ **混合偏弱** ⇒ ★ 降级为"探索" |
| ★ **(c) 长期押注** | ★ 合理，★ 但**要表述为"能力/工具投资"，不是"准确率"** |

### 7.4 ★★★ ANE 硬约束（★ 更正我 6.2 的一处）

| ★ 约束 | ★ 内容 | ★ 我的更正 |
|---|---|---|
| ★★ **fp16 端到端** | ★ 前端接受 fp32/int32/bf16 标注，★ 但**后端不实现它们**；★ **fp32 模型 ⇒ ANE 直接被排除** | ✅ 与我实测一致 |
| ★★★ **累加器饱和在 `2^15 = 32768`**（★ 是 fp16 上限 65504 的**一半**），★ 对 matmul/linear/**任何 ≥2 tap 的卷积** | ★★ **"一个中间部分积超过 2^15 就溢出为 `inf`，即使后续相消本可以把最终结果拉回范围内。"** | ★★★ **更正**：★ 我实测的 65504 是**逐元素通路**的上限，★ **而乘法累加通路的设计限值是 32768** ⇒ ★★ **设计必须用 32768，不是 65504** |
| ★ **在片上工作集 2 MB（M1）**；★ roofline 脊 141 FLOP/byte；★ **0.23 ms 派发地板（M1）**；★ 0.5 pJ/FLOP；★ 85 GB/s DRAM | | ★ M4 Pro 实测 139.6 µs **未撞地板** |
| ★★ **没有原生复数** | ★ 复乘展开为 **4 次实乘 + 2 次实加**；★ **无精度代价**（★ DeepRx：★ 复值网络 **"we have not observed any performance gains"**）| ★ 记录 |
| ★★ **FFT 不要放 ANE** | ★ butterfly 相对稠密 matmul **无增益**；★ GPU 矩阵单元对 FFT 也无帮助（arXiv:2609.32237 "Bandwidth, Not FLOPS"）⇒ ★★ **FFT 留在 Metal**，★ 两篇独立一致 |
| ★★ **完全无路径的算子** | ★ `reduce_prod`、scatter 家族、`mod`、`one_hot`、`non_zero`、`band_part`、`reverse_sequence`、`shape`、`sliding_windows`、逻辑与/或/异或、★ **GRU/LSTM/RNN**、★ 反三角/双曲、★ 多数随机采样 | ★ **模型里不能出现这些** |
| ★ **原生且好** | ★ **2D 卷积（含 dilated/depthwise/grouped/transpose）**、matmul、**融合 SDPA**、layer/instance/group norm、pooling、全部逐元素与激活；★ 3×3 stride-1 自动选 Winograd；★ **张量秩 ≤5**；★ matmul 的深度轴必须 =1；★ fp16 最大核宽 13（M1）/16（A14+）| ★ **HELENA 的算子集正好在这个白名单里** |
| ★★ **`EnumeratedShapes`（≤128）是官方推荐的高性能形状路径** | ★ 无界范围更差 | ★★ **我们上一轮的 52/106 分桶正是这个模式** ✓ |
| ★★★ **`0 mW` 是【预期行为】，不是 bug** | ★ **"firmware holds the engine in a fully gated state … until a job arrives"** ⇒ ★★ **不能从功率轨道推断是否落在 ANE 上**；★ 要用 `MLComputePlan` + 时延对比 | ★★★ **更正我 §14.11(2) 的解读**：★ 我说 0 mW 是"零点读数/机会所在"，★ **那仍然成立，但它不能证明"没在用 ANE"** |
| ★★ **空闲 5 s 后首次调用约 260 ms** | | ★★ **测量时必须保持引擎温热** |

### 7.5 ★★★ 转换管线（★ 更正我 §7.4 的表述）

★★★ **"ONNX → CoreML 已经没有了"**：★ coremltools 官方 —— *"Keras.io and ONNX converters will be
deprecated in coremltools 6"*，★ **支持在 6.0 被【移除】**。
★★ **官方唯一路径 = coremltools Unified Conversion API，【直接从 PyTorch】**
（`torch.jit.trace` / `torch.export`）⇒ `convert_to="mlprogram"` + `compute_precision=FLOAT16`。
★★ **fp32 会排除 ANE。**

★★ **但注意**：★ **我们上一轮的 TF SavedModel 路线是【实测成功】的**（★ 本轮复现 ANE 139.6 µs ✓）
—— ★ 所以**"官方主推 PyTorch"与"TF 路线仍可用"并存**，★ **选哪条取决于模型从哪来**：
★ 复用 HELENA（TF）⇒ TF 路线；★ 用 Sionna/NeuralReceiver（PyTorch）⇒ **必须走 Unified Conversion**。

★ **典型 ANE 阻碍**：★ 自定义层（只落 CPU/GPU ⇒ ★ **我们的 Metal 前后级必须是【独立阶段】，不能同图**）、
★ RNN/LSTM/GRU、gather/scatter、M1 上的 top-k/sort、动态 slice、★ CoreML 4/5 时代的
"Broadcastable"/"ND" 层（★ 可用模型手术退回 rank-3 层类型修复）。

### 7.6 ★★ 仓库（★ 一个关键机会）

| ★ 仓库 | ★ 许可 / 框架 | ★ 价值 |
|---|---|---|
| ★★★ **Rohde-Schwarz/NeuralReceiver** | ★ **Apache-2.0**，★ Python/**PyTorch**（Sionna 2.0.1），★ 2026-05 活跃 | ★★★ **5G NR PUSCH、DeepRx 11 层 CE+均衡+解映射、【含训练】、并导出 ONNX**（★ 神经接收机 **和** LS+ZF 经典基线都导）⇒ ★★ **与我们深度 3 形状最匹配的一个**。⚠️ **它的基线是 LS+ZF 不是 LMMSE** ⇒ ★★ **ZF 在深衰落上放大噪声，其展示的增益被夸大；引用时必须写明基线** |
| ★★ **NVlabs/sionna** | Apache-2.0，★ PyTorch 2.x，★ 2026-09 活跃 | ★ 完整 3GPP NR 模块（PUSCH/DMRS/LDPC/TB）+ OFDM；★ **含训练**；★ **无 CoreML 导出** |
| ★ Sionna Research Kit | Apache-2.0，★ PyTorch + TensorRT | ★ OAI 集成；★ 固定 24-PRB 分块；★ LLR 以 int16 输出；★ **实测 2.42× 的 LLR 尺度失配 vs OAI**；★ `LDPC5GDecoder llr_max=20.0` |
| ★ NVlabs/neural_rx | ★ NVIDIA 专有许可（非 OSI）| ★ NR MU-MIMO PUSCH，★ 含训练 |
| ★ EttusResearch/ni-5g-oai-neural-receiver-testbed-ran | ★ 许可未核实 | ★ OAI + USRP 空口神经接收机试验台 |

★★★ **而【没有任何仓库】把神经接收机导出到 CoreML/ANE** ——
★★★ **这个空白就是我们的贡献机会。**

### 7.7 ★★ 规模与度量纪律

| ★ 项 | ★ 内容 |
|---|---|
| ★★ **实时参数预算锚点是 `1e5`，不是 `1e6`** | ★ NVIDIA 实时 NRX = **1.4e5** 权重（★ 代价 <0.7 dB）；★ 我们的 HELENA = **1.16e5**；★ DeepRx 的 1.2M 是**非实时**，★ 且它报告 **0.1M 就退化、1M 以上饱和** |
| ★★ **知识蒸馏是缩小的成熟手段** | ★ DeepRx KD（arXiv:2507.10409）：★ 11 TFLOPs 学生（教师 30 TFLOPs）★ 在 BER=1e-3 处 **+4 dB** vs 同尺寸从零训；★ 且给出能量锚点 **2 mJ/推理**（Coral Edge TPU）|
| ★★★ **主指标必须是经真实 NR LDPC 译码后的 coded BLER** | ★ **绝不用 NMSE、绝不用 MI、绝不用未编码 BER** |
| ★★ **回退必须是【逐槽神经+经典并行仲裁】**，不是"AI 看着不好就回退" | ★★ 500 Hz Doppler 下经典接收机会崩而神经的还能工作 ⇒ ★ **基于分歧的回退会失效（实测 60.5% 回退率）**；★ 他们的修法代价 <5% 时延 |

### 7.8 ★★★ 推荐的最小决定性实验

★★ 在一个 **~1e5 参数的 DeepRx 式模型**上（★ 改造 Rohde-Schwarz/NeuralReceiver 或在 Sionna 里训），
★ 跑在我们**已有的 OCUDU + CoreML/ANE 栈**上。
★★★ **基线必须【同时】设三级**：★ **LS+ZF**、★ **LS+插值+LMMSE**、★ **我们现役的 Metal MMSE CE**。
★★ 在 **64QAM / 19–23 dB** 上量：**coded BLER + 逐槽 P50/P99 时延 + ANE 功率轨道**。

★★★ **预期结果**：**BLER 与现役链打平；ANE 的时延/能量明显更好。**
★★★ **价值不在准确率**，而在**填补文献空白：ANE 到底能不能承载 PHY 工作负载**
—— ★★ **这是一个站得住、且新颖的贡献。**

### 7.9 ★★ 另有一条外部对照

★★ **OCUDU dApp 先例**（arXiv:2609.07843 / 2609.07805）：★ 内联神经接收机到 LLR，
★ 实测 **81.6 µs P50 / 112 µs P99.9**（51 PRB / 2 端口，GB10）；★ 一个神经均衡器跑过
**26 万次以上的空口调用、0 次回退**。★★ **可用于外部基准，也是"模型输出【解扰域】LLR"
这个契约的独立验证。**
