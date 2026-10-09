# 深度 3 模型：调研笔记（★ 2026-10-08）

> ★ 用户指示：参考前面的 AI CE 工作、公开论文与开源工程。★ **本文件是调研记录**，
> ★ 结论落 `design_and_implementation.md` 的下一节。
>
> **用法**：★ **先读 §0（结论速览）**；★ 要动手时读 **§14（建设计划）**；
> ★ 查证细节时按下表跳转。

| ★ 节 | ★ 内容 | ★ 一句话 |
|---|---|---|
| **§0** | ★★★ **结论速览** | ★ **五条裁决 + 唯一的一票否决** |
| ★ **§1** | ★★★ 上一轮 AI CE 的资产 | ★★ **基础设施已经建好，都在本机** |
| ★ **§2** | ★★ 上一轮的技术选择与被排除的候选 | ★ 可直接继承 |
| ★ **§3** | ★★★ ANE 路径本轮独立复现 | ★ **139.6 µs vs CPU 685 µs（4.9×）** |
| ★ **§4** | ★★★ 上一轮踩过的坑 | ★★ **最值钱的可迁移部分** |
| ★ **§5** | ★★ 深度 3 与 AI CE 的本质差别 | ★ **输出是 LLR、没有真值、动机是"省"不是"准"** |
| ★ **§6–§7** | ★★★ 外部文献调研 | ★★ **准确率动机不成立（文献共识）** |
| ★ **§8–§9** | ★★★ ANE 深潜 | ★★ 派发地板、fp16、CPU 第三臂、编写规则 |
| ★ **§10–§11** | ★★★ 仓库与许可 | ★★ **没有可抄的仓库；一条许可致命** |
| ★ **§12** | ★★★ C 项核查（RADE） | ★★ **它没用 ANE ⇒ 强化"我们是第一个"** |
| ★ **§13** | ★★★ 重新校准（用户更正） | ★★ **派发地板不是硬截止** |
| ★★ **§14** | ★★★ **建设计划（可执行）** | ★★ **分层设计 + 判据 + 步骤** |

---

## 0. ★★★ 结论速览（★ 先读这一节）

### 0.1 ★★★ 五条裁决

| # | ★ 问题 | ★ 裁决 | ★ 依据 |
|---|---|---|---|
| **1** | ★★ **AI 深度 3 会不会更【准】？** | ★★★ **不会** —— ★ 文献天花板就是 **genie/完美 CSI 的 LMMSE**；★ DeepRx 自己写 **"matches"** genie；★ NVIDIA 比自己的 LMMSE 基线**差 ≤1 dB**；★ **ETH 真实试验台上 site-tuned 经典 LMMSE + 经典 IDD 误差最低** | ★ §7.1 |
| **2** | ★★ **那为什么还做？** | ★★★ **因为【省】—— 能量/算力分流。** ★ 实测 **ANE 比 GPU 能效好 13–14.5×** | ★ §7.3 |
| **3** | ★★ **时延是硬约束吗？** | ★★ **不是。** ★ 节奏双峰（0.5 ms / 4 ms），★ 且链里**已有 1893 µs 跳、33–49 ms JIT 税**的先例 ⇒ ★★ **不必在 1 slot 内完成** | ★ §13 |
| **4** | ★★ **派发地板会杀死方案吗？** | ★★ **不会**（★ 我先前框定过紧，已收回）—— ★ 它是**吞吐/调度问题**，★ 且**会随硬件下降** | ★ §13.5 |
| **5** | ★★★ **唯一的一票否决是什么？** | ★★★ **fp16 动态范围**：★ **累加器饱和在 `2^15 = 32768`**（★ fp16 上限的一半），★ **且中间部分积溢出【无法靠后续相消恢复】** —— ★★ **而"除以信道"的均衡正是这种形态** | ★ §7.4 / §8.4 |

### 0.2 ★★★ 三件必须知道的事

1. ★★★ **基础设施已经建好**（§1）：CoreML/ANE 引擎、NN 估计器（含热重载）、
   **8 个编译好的 `.mlmodelc`**、**21 个训练脚本**、可用的 Python venv、
   **5.3 GB 合成集**、**~7 站点真实语料**、**真实训练三元组** —— ★ **都在本机。**
2. ★★★ **没有可抄的仓库**（§10.2）：★★ **不存在"PyTorch + 符合 3GPP + 许可宽松"的
   PUSCH 神经接收机** ⇒ ★ **模型要我们自己写**（★ Sionna 2.x 生成数据 + 自写架构）。
3. ★★★ **必须【先按 ANE 重写再训】**（§10.4）：★★ 所有已发表 DeepRx 都用**空洞卷积**，
   ★ 而 Apple 规则要求分解掉 ⇒ ★★ **改 dilation 就是改架构 ⇒ 必须重训，不是移植。**

### 0.3 ★★★ 三条动机的最后账

| ★ 动机 | ★ 裁决 |
|---|---|
| ★★ **(a) 能量/算力分流** | ★★★ **成立，且是首要理由** |
| ★★ **(b) 跨单元联合处理** | ★★ **降级为探索**；★ 且正解可能是 **IDD**（★ 不需要 ANE、不需要训练）|
| ★ **（c）长期押注** | ★ 合理，★ 但**要表述为"能力/工具投资"**，不是准确率 |

### 0.4 ★★★ 而我们确实在做一件【新的】事

★★ **结构化 arXiv 检索**：`abs:"Apple Neural Engine"` **13 条，无一无线/PHY**；
★ `abs:"CoreML" AND (abs:"wireless" OR "radio" OR "signal")` **恰好 2 条，都不是 PHY**。
★★★ **且"最接近的先例"RADE 的 iOS app 经核查【也没用 ANE】**（★ §12：纯 C 移植，权重烧进源码）。
★★★ **⇒ 没有任何仓库把神经接收机导出到 CoreML/ANE，也没有任何已发表工作把无线 PHY 跑在 ANE 上。**

---

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

## 8. ★★★ 补充调研（★ 改掉我两个计划假设，★ 并加了一个我没考虑的候选）

### 8.1 ★★★ 假设 1 被改：**~0.5 ms 时隙预算 vs 不可流水线的派发地板**

★★ **这才是真正的拦路虎**（★ 不是算力）：

| ★ 事实 | ★ 值 |
|---|---|
| ★★ **派发地板** | ★ M1 **~0.23 ms**（★ M5 拟合 ~0.11 ms）；★ 一个**极小**图的实测 ~**190 µs**，★ 其中 **~98% 是软件/固件派发开销，不是引擎计算** |
| ★★★ **不能流水线** | ★★ **"The driver keeps at most one firmware command in flight at a time, a single-pending-queue scheduler."** ★★ 两条并发提交线程实测 **1.04×** ⇒ ★★ **往返时间【不能】被重叠请求隐藏**；★ 同进程重叠两路是"未完成路径" |
| ★ **阶段拆解** | ★ 用户态绑定 + 主机 fp16 拷贝 **~25 µs**；★ 固件请求构建 **~16 µs**；★ doorbell **~2–3 µs**；★ **固件往返 ~130 µs**；★ 核完成 ~10 µs |
| ★ **一个 3×3 conv（256ch/28×28）在 M1 已 ~0.51 ms** | |

★★★ **⇒ 一次 CoreML 派发在计算之前就吃掉 0.5 ms 时隙的 38–46%。**
★★★ **唯一可行的形态 = 【每时隙一个融合程序】**。
★★ **融合就是杠杆**：★ conv-relu 栈从 1 层到 32 层，**每次调用时延稳定在 ~0.19 ms**
（★ 逐算子 222 µs → 融合后 6.3 µs）。
★ **预热**：★ 首次调用 ~7.6 ms；★ **空闲 5 s 后首次 ~260 ms**（~123×）；★ 间隔 ~100 ms 时惩罚降到 ~0.5 ms
⇒ ★★ **0.5 ms 节奏下保持温热**。★ 好消息：★ 持续运行时 **p99 只比 p50 高 36%**，★ 抖动本身不糟。

### 8.2 ★★★ 假设 2 被改：**必须把 CPU（BNNSGraph）当作【第三个引擎】来评**

| ★ 事实 | ★ 含义 |
|---|---|
| ★★ **ANE 在"小而派发受限"的算子上【输给 GPU】** | ★ GEMM 地板 **11.4 vs 16.6 GFLOP/s/W**；★ 官方裁决表：**tiny operations → CPU** |
| ★★ **一个 64×256×256 matmul 在 CPU 上 ~0.026 ms** | ★★ **低于 ANE ~0.23 ms 的每次评估地板** |
| ★★ 我们的深度 3 网格很小（★ ~0.12 MB）| ★★ **所以这条直接相关** |
| ★★ Apple 专门发了 WWDC24 session 10211 讲这个 | ★★ **BNNSGraph 提供"实时保证：无运行时内存分配、单线程运行"，明确面向音频/信号处理模型** |

★★★ **⇒ 最小实验应当有【三个】引擎臂：ANE / GPU(MPS) / CPU(BNNSGraph)**，
★ 而**不是只比 ANE vs MPS**。★★ **若这个工作量是派发受限的，CPU 可能直接赢。**

### 8.3 ★★★ Apple 官方的 ANE 编写规则（★ 我们踩得到的）

| ★ 规则 | ★ 内容 |
|---|---|
| ★★ **末轴必须连续且 64 字节对齐** | ★★ **"A singleton last axis (dimension = 1) gets padded to 64 bytes, resulting in 32× memory cost at fp16"** ⇒ ★★ **末轴至少保留 ≥32 个 fp16 元素**；★ 官方推荐布局 **`[1, C, T, 1]` BC1S**。★★ **我们的 `(S, F, C)` 网格在错误布局下会踩这个陷阱** |
| ★★ **用 1×1 `Conv2d` 而不是 `nn.Linear`** | ★★ **"nn.Linear gets decomposed into less efficient ops that may fall back to CPU."** |
| ★★ **任何 Python float 字面量或 fp32 算子** | ★★ 会产生 ANE **无法执行**的 f32 缓冲 |
| ★ **优先 3×3 stride-1 Conv2d** | ★ 自动选 Winograd（~2.25× 乘法缩减）；★ 步长应分解为 2 与 3；★ **softmax 放在【通道】维而非空间维** |
| ★★ **完全静态形状** | ★ 每个静态形状配置导出**一个**函数 |
| ★★ **`EnumeratedShapes` 仍是正确选择** | ★★ 官方维护者（coremltools issue #2370）：★ **`RangeDim` 若 Frequent ⇒ 只有默认形状能跑 ANE；若 InFrequent ⇒ 任何形状都能跑但有性能惩罚（每次新形状都要 ANE 编译）** |
| ★★ **矛盾待测** | ★ Apple 规则说 **"Neural Engine cannot fuse multi-head attention into a single operation"**（★ 无融合 SDPA），★ 而 2026 ANE 论文把 `scaled_dot_product_attention` 列为 M1 起原生 ⇒ ★★ **两个都记，★ 在我们的硅上测** |
| ★ **张量秩 ≤5**；★ dtype fp16/int8/int16；★ **fp32 回退** | ★ 记录 |

### 8.4 ★★★ 数值边界行为（★ 一个 oracle 必须建模的）

| ★ 行为 | ★ 内容 |
|---|---|
| ★★ **NaN 被强制为 `+inf` 且【不传播】** | |
| ★ `log(0)` 返回哨兵 **−45440** | |
| ★ `sqrt(-1)` / `rsqrt(-1)` / `log(-1)` 全部返回 **+0** | |
| ★ 裸 `exp` 在 **~11.094**（= ln 65504）溢出 | ★ **但 softmax 有硬件 max-subtraction，安全** |
| ★★ fp16 非规格化数在逐元素通路存活，★ **但在 M1 的 MAC 内被【冲刷】**（★ M5 保留）| ★★ **这会让"同图同输入"的位确定性只在同一芯片上成立** |
| ★ 舍入 = 就近偶数；★ **固定图+输入时引擎是位确定的** | ★★ **这对 A/B 是好事** |
| ★ 激活 LUT 在 fp16 底部的误差 | ★ sigmoid **0.0034**、tanh **0.0017**、gelu **0.0059**；★ 但 **sin/cos/atan 在参数归约接缝处达 0.04–0.12 绝对误差** ⇒ ★★ **上游先把范围折叠好** |

### 8.5 ★★ 实测 ANE 功率（★ 用来设预期）

★ 空闲 ~0 mW（★ 轨道关断）· **派发地板 ~0.9 W** · 单调用循环 ~755 mW ·
★ 持续 256ch 3×3 conv **1.66–1.78 W** · 计算受限 fp16 matmul **~4.3 W**（int8 ~5.8 W）·
★ 8×1024² matmul 链 ~5.5 W 平坦 · **跨类上限 ~6 W**。
★ 效率 **~2.68 TOPS/W（~0.37 pJ/FLOP）** 在 fp16 最优点，★ **退化到 ~22 pJ/FLOP 在派发地板**。
★ **单核轨道步进 ~10–11 mW**（★ 在一个 ~800 mW 的常开地板上，M1）。
★ M4 交叉验证（★ IPDPS 投稿，未同行评审）：★ **GEMM 在 ANE 5.2 W vs GPU 24 W**；
★ M4 Pro 上 ANE 最高 **3.8 TFLOPs** vs GPU 4.7 TFLOPs。

### 8.6 ★★★ "无先例"这个论断现在是**严格的**

★★ **结构化 arXiv 元数据检索**：★ `abs:"Apple Neural Engine"` **总共只有 13 条结果，无一是无线/PHY**；
★ `abs:"CoreML" AND (abs:"wireless" OR abs:"radio" OR abs:"signal")` **恰好 2 条，都不是 PHY**。
★★★ **⇒ 这是一个【有据可查的空白】，可以照此引用 —— 我们会是第一个。**

★ **找到的最佳端侧 ANE 时延数据点**（arXiv:2604.27279）：★ 616K 参数 CNN 在 iPhone 17 Pro Max
（A19 Pro）上 **0.25 ms / 3 s 窗**（★ SE 3 / M1 Max 上 0.55 ms）；★ 4 Hz 流式仿真只用 0.54% 实时预算。
★★ **注意它基本【就在派发地板上】= 派发受限而非计算受限** ⇒ ★ 可作"ANE 流式时延实际长什么样"的锚点。

★ **最接近的"无线 PHY 跑在 iPhone 上"的真正先例**：★ **RADE**（arXiv:2505.06671, WASPAA 2025），
★ 一个 HF 无线电的 OFDM 神经编解码器，★ 有 iOS app（`github.com/peterbmarks/RADE_decode`）。
★★ **未确认它的神经译码器是跑 CoreML/ANE 还是 CPU/Accelerate** —— ★★ **值得花 10 分钟读那个仓库**，
★ 因为它是"无线 PHY 神经网络跑在 Apple 芯片上"的最佳先例。

★ **量化先例**：★ arXiv:2508.06275（★ 深度神经接收机的训练后量化）：★ **8 位逐通道即可维持 BLER**；
★ 4 位有前景但还需努力。

### 8.7 ★★ 战略：**Core ML 已进入维护期，Apple 在推 Core AI**

★ `apple/coreai-models` 文档称 **Core AI** 是 **iOS 27 / macOS 27+ 的端侧运行时**，
★ 路径 **PyTorch → `.aimodel`（`coreai-torch` / `TorchConverter`）**，★ AOT 用 `coreai-build`。
★★ **Core ML 仍可用，Core AI 尚未随 OS 发布** ⇒ ★ **必须显式决定：现在部署在 Core ML，还是为 Core AI 而建。**
★★ **注意：上文的 ANE 【硬件】约束两者通用。**

### 8.8 ★★★ 证据质量警告（★ 必须随结论一起引用）

★★ **最丰富的 ANE 表征**（★ 功率、roofline、派发地板、算子-设备矩阵、fp16 数值）
★★ **主要依赖【一篇】302 页单作者 arXiv 预印本**（arXiv:2606.22283，★ **未同行评审**，
★ 作者自述部分来自反编译、部分为预测，★ **且功率数字是他自己标注的 `powermetrics` 建模估计，
不是瓦特计实测**）。★ 测量机器是 M1/M2/M5 Mac，★ **不是 iPhone**。

★★ **两处子论断在来源之间冲突，应当【分别报告而不是取平均】**：
1. ★ **池化窗 >13 不支持**（hollance）★ vs **"accepts sizes of 128 and beyond"**（该论文）；
2. ★ **派发地板 0.23 ms（经 CoreML）** ★ vs **~70 µs（经私有 e5rt 路径，ANEForge，arXiv:2606.17090）**。

★ **~250 MB 分片上限**为社区来源。★ **没有 Apple 文档说过"灵活形状一律强制 CPU/GPU 回退"。**

### 8.9 ★★★ 对我们计划的三条硬修正

| ★ # | ★ 修正 |
|---|---|
| ★★ **1** | ★★★ **"每时隙一个融合程序"不是优化，是【可行性前提】** —— ★ 0.5 ms 预算下，多次派发直接把时隙吃光 |
| ★★ **2** | ★★ **实验必须有三条引擎臂：ANE / MPS(GPU) / CPU(BNNSGraph)**，★ 而我们的网格很小（~0.12 MB）⇒ ★ **CPU 可能直接赢** |
| ★★ **3** | ★★ **布局与算子形式要按 Apple 官方规则重写**：★ 末轴 ≥32 fp16 元素且 64B 对齐、★ 官方推荐 `[1,C,T,1]`、★ **用 1×1 Conv2d 取代 Linear**、★ 无 fp32 字面量 |

## 9. ★★ 补充之二（★ 调研完结，★ 含一处对社区流传说法的更正）

### 9.1 ★★ 更正：目标应当是 `ct.target.iOS17`，**不是 iOS18**

★ CoreML 社区（ane-book）流传"要拿到 ANE 原生的 matmul/conv lowering 必须
`minimum_deployment_target=iOS18`"。★★ **那是错的**。★ 经 coremltools 源码核实
（`converters/mil/mil/ops/defs/__init__.py`）：★ **`conv` / `conv_quantized` / `conv_transpose` /
`linear` / `matmul` / `einsum` / `layer_norm` / `softmax` / 全部激活 / 全部池化
都在【iOS15】opset 基线里。**

| ★ opset | ★ 它真正【新增】的东西 |
|---|---|
| ★ iOS16 | ★ gather / gather_nd / topk |
| ★ iOS17 | ★ 更丰富的激活集（clamped_relu、elu、leaky_relu、prelu、scaled_tanh、sigmoid_hard…）|
| ★ iOS18 | ★ **`scaled_dot_product_attention`**、`read_state`、`slice_update`、`gru`、分块/LUT 压缩 |

★★ **实际影响**：★ **CNN 类目标 iOS17 即可，不损失任何东西**；
★ **只有明确需要 MLState（`read_state`）、MLTensor 或【融合 SDPA】时才选 iOS18**。
★★ **选 iOS18 要付出一年的设备覆盖，而 conv 侧零收益。**
★ （社区来源对**硬件**行为的描述仍然对 —— 1×1 Conv2d 作为 matmul lowering、BC1S 布局 ——
★ **错的只是"需要 iOS18"这个框定**。）

### 9.2 ★★★ Apple 亲口写下了对我测量最要紧的那个 caveat

★★ **原文**：*"Core ML decides where each operation runs by weighing more than raw compute speed,
including the cost of moving data between compute units and how long a compute unit takes to ramp up.
As a result, **an operation that's compatible with the Neural Engine can still run on the CPU, when
that's the faster choice overall.**"*

★★★ **⇒ "算子与 ANE 兼容"【永远不蕴含】"该算子在 ANE 上跑了"** ——
★ 这正是**必须用 `MLComputePlan` 而不是靠推断**的原因。
★ 并且该 Report 还暴露一个 **"First" 预测统计**（*"the cost of the very first prediction, which
can be higher than later ones if it requires specialization specific to that input's shape"*）
—— ★★ **那就是【按形状的 ANE 重编译成本】，★ 如果我们继续用 EnumeratedShapes 分宽度桶，
★ 这个数就是必须盯的那个。**
★ 声明限制：★ **它不测内存、不测功率**；★ 结果只对**该设备 + 该 compute-unit 配置**成立。

### 9.3 ★★ API 陷阱

★ **`MLModel.get_compute_plan()` 不存在**（★ 会失败）。★ 正确调用：
`coremltools.models.compute_plan.MLComputePlan.load_from_path(model.get_compiled_model_path())`。
★★ 注意 **coremltools 自己的 docstring 也是错的**（★ 例子调 `get_compiled_path()`，★ 那也不存在）。

### 9.4 ★★ `MLState` 让"不能重叠"更硬

★ Apple 原文：★ *"The client shall not read or write the buffers while a prediction is in-flight"*；
★ *"Each stateful prediction that uses the same MLState must be serialized. Otherwise, if two such
predictions run concurrently, the behavior is undefined."*
★★ 结合 ANE 驱动的单挂起队列（★ 实测 **1.04×** 串行化）⇒
★★★ **无论有没有 state，都【不能重叠时隙】。** ★ 这**强化了"每时隙一个融合程序"的结论**，
★ 并**排除**了有状态的逐符号流水线设计。

### 9.5 ★★★ ONNX 这条路**彻底关掉**

★★ **`onnx2torch` 不是一个经过验证的桥** —— ★ 没有任何 Apple / coremltools / ONNX / onnx2torch
文档断言 `ONNX → onnx2torch → ct.convert` 这条链；★ 它返回一个裸 `nn.Module`
（★ 不是 TorchScript / ExportedProgram），★ 你得自己再导出一次（★ 又多一个会引入错误的 tracing 步）；
★ 它测过的模型清单全是视觉模型，★ **没有信号处理或复值先例**。★★ **只可作最后手段。**
★★★ **⇒ 结论定案：进入 CoreML 没有受支持的 ONNX 路径，必须从【原始 PyTorch 模型】转换。**

### 9.6 ★★★ 调研最终结论（★ 三份报告合起来）

| ★ 项 | ★ 裁决 |
|---|---|
| ★★★ **准确率动机** | ★★★ **不成立** —— 文献天花板就是 genie LMMSE；★ ETH 真实试验台上 **site-specific LMMSE + IDD 误差最低**，★ 胜过所有被测神经接收机 |
| ★★★ **能量/算力分流动机** | ★★★ **成立且是首要理由** —— ★ 实测 ANE 比 GPU 好 **2–14.5×**（GFLOP/s/W）|
| ★★ **跨单元联合处理** | ★★ **降级为探索**；★ 且正解可能是 **IDD**（★ 不需要 ANE、不需要训练）|
| ★★★ **两个可能杀死 ANE 方案的东西** | ★★★ **① 0.5 ms 时隙 vs ~0.19–0.23 ms 不可流水线的派发地板；② fp16 动态范围（2^15 = 32768 累加器上限），而"相消密集的均衡"没有 fp16 安全形式** |
| ★★★ **必须加第三个引擎臂** | ★★★ **CPU / BNNSGraph** —— ★ 小网格若派发受限，**CPU 可能直接赢** |
| ★★★ **先例检索** | ★★★ **零篇**神经接收机或任何无线 PHY 跑在 ANE/CoreML 上（★ 结构化 arXiv 检索已doc）⇒ **我们是第一个** |
| ★ **唯一值得花 10 分钟核实的线索** | ★ **RADE 的 iOS app**（`github.com/peterbmarks/RADE_decode`）—— ★ 未确认它的神经译码器跑 CoreML/ANE 还是 CPU/Accelerate |

## 10. ★★★ 更正（★ 三条事实错误，★ 其中一条是许可致命的）

### 10.1 ★★★ 更正 1（**许可致命**）：`NVlabs/neural_rx` 是**非商用**，不是宽松许可

★ 我前文写"NVIDIA 专有许可（非 OSI）"—— ★★ **不够准确，实际是不可商用**。
★ 其 `LICENSE.txt` §3.3 Use Limitation 原文：
★★ *"The Work and any derivative works thereof only may be used or intended for use
**non-commercially**... 'non-commercially' means for **research or evaluation purposes only**."*
★ 源码头带 `SPDX-License-Identifier: LicenseRef-NvidiaProprietary`。
★★ 注意文件名是 **`LICENSE.txt` 而不是 `LICENSE`** —— ★ 直接访问 `.../LICENSE` 会 404，
★ 这就是它被广泛误读成"无许可"或"宽松"的原因。
★★★ **⇒ 任何源自 `neural_rx` 的东西都不能交付。★ 只能当【架构参考】。**

### 10.2 ★★★ 更正 2：**两个 3GPP-PUSCH 神经接收机仓库都是 TensorFlow/Keras，不是 PyTorch**

| ★ 仓库 | ★ 真实技术栈 |
|---|---|
| ★ `Rohde-Schwarz/NeuralReceiver` | ★ **TF/Keras + Sionna `>=1.2.1,<2.0.0`**（★ TensorFlow 时代的 Sionna）★ 我前文写"PyTorch（Sionna 2.0.1）"是错的 —— ★ 被它 README 文献区引用 Sionna 2.0.1 误导，★ 那与它自己的依赖钉**自相矛盾** |
| ★ `NVlabs/neural_rx` | ★ **TF/Keras + Sionna 0.18**（★ `requirements.txt`: `sionna==0.18.0, onnx==1.16.2, tf2onnx, polygraphy`）★ 我前文写 PyTorch 是错的 |

★★★ **而这是真正有价值的结论**：
★★★ **不存在任何"PyTorch + 符合 3GPP + 许可宽松"的 PUSCH 神经接收机仓库。**
★ 最接近的 PyTorch 3GPP-PUSCH DeepRx 是 MathWorks 的 `deeprx.py`（+ `deeprx_30k.pth`，
★ 11 个 ResNet 块，**1.2325M 可学习参数**，输入 `[312 14 10]`）—— ★★ **不是开源的**。

★★ **实际含义**：★★★ **模型要我们自己写。**
★ 用 **Sionna 2.x**（Apache-2.0，**PyTorch 原生**）做**符合 3GPP 的数据生成**，
★ 用 R&S / neural_rx 作**架构参考**。
★★ **注意**：★ Sionna 1.x 与 2.x **共用 `sionna.phy` API 但【后端不同】**（★ 1.x = TF，★ 2.x = PyTorch）
⇒ ★★ **1.x 时代的神经接收机代码必须【移植】，不能复用。**

### 10.3 ★★ 更正 3：`sionna-rk`（Research Kit）**不是 OAI 的 fork**

★ `NVlabs/sionna-rk`：★ Apache-2.0，★ 2025-04 建，★ 2026-07 活跃，★ VERSION 1.3.2。
★★ **它 clone 的是【上游】`gitlab.eurecom.fr/oai/openairinterface5g` 的 tag `2025.w34`**，
★ 然后打一个 **93 887 字节、39 个文件**的补丁。★ `github.com/NVIDIA/openairinterface5g`
★ 与 `NVlabs/openairinterface5g` **都 404** ⇒ **不存在 NVIDIA fork**。
★★ **补丁【不 vendored 神经网络】**，★ 它加的是 **`dlopen` 钩子 + 树外插件构建路径**
（★ 动 `common/utils/load_module_shlib.c`、`nr_ulsch_demodulation.c`、`nr_ulsch_llr_computation.c`）
⇒ ★★ **`libreceiver_neural_rx.so` 由此注入，无需上游任何东西。**
★★★ **这对我们是【最有价值的一条工程先例】**：★ 它证明**"不 fork 上游、用 dlopen 插件接入神经接收机"是可行的**。
★ 许可混合注意：★ sionna-rk 的 Apache-2.0 代码**坐在 OAI Public License 1.1 的代码之上**。

### 10.4 ★★★ 一个我们**没预料到的 ANE 阻碍**：**空洞卷积（dilation）**

★★ **每一份已发表的 DeepRx 实现（MathWorks、R&S、neural_rx）都在残差块里用空洞卷积。**
★★ Apple 的 ANE 规则说**大 dilation 率很贵**，★ 必须**分解成 dilation-2/3 的链**。
★★★ **而去掉或重构 dilation【会改变架构】⇒ 【必须重训】。**
★★★ **⇒ 不能简单地移植一个 checkpoint。要为这件事留预算。**

### 10.5 ★★ 这一模型类的 ANE **重写清单**（★ 源码级阅读得到）

| ★ 项 | ★ 内容 |
|---|---|
| ★★ **4D BC1S `(B,C,1,S)` 布局** | ★ 已发表模型是 3D channels-last `[F,S,C]` ⇒ ★ **子载波要映射到 64 字节对齐的末轴** |
| ★★ **标准 6 通道（`4·Nrx+2`, Nrx=1）是【糟糕】的 ANE 通道数** | ★★ **要 pad 到 8 或 16** |
| ★ **`nn.Linear` → 1×1 `Conv2d`** | ★ 否则分解为低效算子并可能落 CPU |
| ★ **无 fp32 字面量** | ★ 会产生 ANE 无法执行的 f32 缓冲 |
| ★★ **dilation 要分解掉** | ★ ⇒ **需要重训** |
| ★★ **FFT 放主机** | ★ coremltools 的 torch 前端**确实注册了 `torch.fft.*`**，★ 但它**降为 O(N²) 稠密 DFT matmul 且带 fp32 cast** —— ★★ **4096 点时是 radix-2 的 ~340 倍数学量，且触发 fp32 回退规则** |
| ★★ **R&S 的 ONNX 导出形状注意** | ★ `utils/onnx_export.py` 用 `tf2onnx`、**3 个输入**（`rxgrid`/`h_hat`/`pilots`），★ 各 `[None,1,14,n_subcarriers,2]` ⇒ ★★ **batch 维是动态 `None`**，★ 而 **CoreML 要静态或 EnumeratedShapes** |
| ★★ **neural_rx 的 ONNX 有两个 int32 输入** | ★（`dmrs_ofdm_pos`/`dmrs_subcarrier_pos`）用于索引 ⇒ ★★ **Gather 类算子 ⇒ CoreML 上落 CPU** |

### 10.6 ★★★ 许可结论表（★ 决定我们能碰什么）

| ★ 类别 | ★ 仓库 |
|---|---|
| ★★ **可商用 + 3GPP NR** | ★ **`Rohde-Schwarz/NeuralReceiver`**（Apache-2.0，**TF**）、★ **Sionna 核心 + `sionna-rk` + `pyAerial`**（Apache-2.0）、★ `obiedeh/ai-phy-neural-receiver-benchmark`（MIT，ONNX 且有验证过的对齐）|
| ★★★ **不可商用 ⇒ 不能交付** | ★★ **`NVlabs/neural_rx`**（★ 见 10.1）、★ `SAIC-MONTREAL/CeBed`（CC BY-NC 4.0）|
| ★★ **Copyleft 阻碍** | ★ `dianixn/Channelformer`（GPL-2.0）|
| ★★ **无许可文件 ⇒ 保留所有权利，【不要复用】** | ★ 原版 CsiNet（`sydney222`/`Wind0ranger`）、★ `G-ALI007/DeepRx-OFDM-PyTorch`、★ 若干 ChannelNet/CsiNet 副本 |

### 10.7 ★★ 另两条值得知道的

| ★ 项 | ★ 内容 |
|---|---|
| ★★ **OAI 主线 PHY 【零 ML】** | ★ 对 `openair1/PHY`（`develop`，commit `f8f7695`，524 个文件）直接 grep：★ `neural`=0、`tensorrt`=0、`onnx`=0、`tensorflow`=0、`pytorch`=0、`cupy`=0、`"machine learning"`=0 ⇒ ★★ **OAI 的真正贡献是【可插拔性】（那个通用 dlopen loader），不是 ML** |
| ★★ **OAI 许可要注意** | ★ `develop` 自 tag `2026.w14` 起用 **CSSL v1.0**；★ tag `2025.w34`/`v2.1.0`/`v2.2.0` 用 **OAI Public License v1.1** ⇒ ★★ **两者都【未经 OSI 认证】；需要法务审阅并刻意钉住 OAI 修订版** |
| ★★ **`pyAerial` 是开源的**（Apache-2.0，★ SPDX 已在四处核实）| ★ 但 (a) 它的 PUSCH 神经接收机是**仅推理**（★ 训练 notebook 只有 LLRNet，一个解映射器）；★ (b) **NVIDIA GPU 是硬要求**（cuPHY 后端、`cupy-cuda13x`、TensorRT，**无 CPU 路径**）；★ 随包提供 `neural_rx.onnx`（663 830 字节）|
| ★★ **没有人在任何地方发表过 NPU 神经接收机时延** | ★ `neural_rx` 在 A100 上 ~1 ms 仍是唯一可比数字 ⇒ ★★ **任何 NPU 声称都是外推**。★ Apple 在积极申请专利（US 12,323,357 B2，"DMRS overhead adaptation with AI-based channel estimation"，受让人 Apple Inc. —— ★ 仅专利索引来源，全文未核实），★ **但未发表任何实现** |
| ★ 另有两处"记得但不成立" | ★ **Sionna 没有 CSI-feedback 教程**（★ 两次核实，各路径与 v1.2.0 tag 全 404）⇒ ★ "Sionna CsiNet 例子"这个前提是假的；★ **`ReEsNet` 无法核实存在**（★ arXiv 全文零命中）|

### 10.8 ★★★ 修正后的建设计划（★ 这一节替换我 §7.8 的"改造现成仓库"）

★★★ **不存在可抄的 PyTorch 3GPP-PUSCH 神经接收机 ⇒ 【模型要我们自己写】。**

| ★ 步 | ★ 内容 |
|---|---|
| ★★ **1** | ★ **用 Sionna 2.x**（★ Apache-2.0、★ **PyTorch 原生**）做**符合 3GPP 的数据生成**（★ PUSCH/DMRS/LDPC/TB）|
| ★★ **2** | ★ **自己写模型**，★ 架构参考 R&S / neural_rx（★ 后者**只看不抄**，★ 许可不允许）|
| ★★★ **3** | ★★★ **在"对一个 checkpoint 产生感情"【之前】就先按 ANE 重写** —— ★ 布局 BC1S、★ 通道 pad 到 8/16、★ `Linear`→1×1 `Conv2d`、★ **dilation 分解掉（⇒ 必须重训）**、★ 无 fp32 字面量、★ FFT 留主机 |
| ★★ **4** | ★★ **三级基线 + 三条引擎臂**（★ 见 §7.8）|
| ★★ **5** | ★★ **主指标 = 经真实 LDPC 译码后的 coded BLER** + 逐槽 P50/P99 + 三引擎功率 |

★★★ **而 §10.7 那条 OAI/dlopen 先例给了我们一条省事的路**：
★★ **不必 fork OCUDU 上游 —— 用 dlopen 插件注入神经接收机是已被验证可行的**
（★ 上游 OAI 主线 PHY **零 ML**，★ 其贡献正是那个通用 loader）。

## 11. ★★ 仓库调研收尾（★ 调研全部结束，★ 本节是最后的增量）

### 11.1 ★★★ Sionna 2.x 的 **PyTorch 复数支持是已知弱点**（★ 正打在我们的路径上）

★ 既然结论是"用 Sionna 2.x 在 PyTorch 里训"，★ 这条就很重要：

| ★ 证据 | ★ 内容 |
|---|---|
| ★★ Sionna **v2.2.0 "Part 4" notebook 自带一个 warnings filter** | ★★ 针对 ***"Torchinductor does not support code generation for complex operators"*** |
| ★★ Sionna issue **#1149** | ★ 记录了**广播复数张量产生大量中间临时量** |

★★★ **⇒ 我们打算用的那条 PyTorch 原生 Sionna 线，有已知的复数张量编译与内存问题。**
★★ **实际缓解（★ 与 ANE 的要求【正好一致】）**：
★★★ **把复数张量限制在 I/O 边界，内部一律用实值对（real-valued pairs）** ——
★ 而**这本来就是 ANE 要求的形态**（★ 无原生复数）。
★★ **⇒ 值得先做一个 spike 再承诺。**

### 11.2 ★★ 合规上的一个**假阳性**（★ 会出现在 CI 里）

★ **GitHub licensee 对 Sionna、`sionna-rk`、**Aerial** 都报 `NOASSERTION`**，
★ 而**三者的 LICENSE 文件都写着 Apache-2.0**（★ Sionna 的带 `SPDX-License-Identifier: Apache-2.0`；
★ `sionna-rk` 是 1 489 字节的 Apache-2.0 声明；★ `pyAerial` 的 SPDX 标在
`pyproject.toml`/`setup.py`/`CMakeLists.txt`/源码里）。

★★ **⇒ 若在 CI 里跑合规扫描，我们最重要的三个依赖都会被标成 unknown/blocked。**
★ 注意：★ `pyaerial/LICENSE` 与 `pyaerial/NOTICE` **都 404** ⇒ ★ **pyaerial 没有单独的许可文件，
也没有专有保留条款**。★★ **需要人工复核来清掉这些假阳性。**

### 11.3 ★★ ONNX → CoreML 的**历史**天花板（★ 关掉这个问题）

★ Apple 对 coremltools **4.0 和 5.1** 的文档都写：
★★ *"ONNX to Core ML supports ONNX Opset version 10 and older."*
★★ **⇒ 从来不是"支持到 15/17、18–21 不支持"，而是【opset ≤10】，然后在 6.0 被【整体移除】。**

### 11.4 ★★ Core AI 的具体情况（★ 若选后继路径）

| ★ 项 | ★ 内容 |
|---|---|
| ★ `github.com/apple/coreai-models` | ★ **BSD-3-Clause**（★ "Copyright 2026 Apple Inc."）|
| ★ 工具链 | ★★ **只支持 PyTorch / `torch.export` → `.aimodel`**，★★ **不摄入 ONNX** |
| ★ ANE 放置命令 | ★ `xcrun coreai-build compile model.aimodel --preferred-compute neural-engine` |
| ★ 要求 | ★ 报告称 iOS/macOS **27.0+** |

★★ **⇒ Core AI 是一条【真正独立的】工具链，不是我们 CoreML 工作的 drop-in。**

### 11.5 ★ 验证 API 的版本细节

★ `MLComputePlan` 的 Python 绑定**自 coremltools 8.1 起存在**（★ 我们是 9.0 ✓）。
★★ **用 `ComputeUnit.CPU_AND_NE` 以防【静默落到 GPU】**。

### 11.6 ★★ 学习型信道估计仓库的许可（★ 若日后超出深度 3 的范围）

| ★ 类别 | ★ 仓库 |
|---|---|
| ★★ **GPL-2.0，Copyleft 阻碍** | ★ `dianixn/Channelformer`（TWC 2023）、★ `dianixn/Attention_Based_Neural_Networks_for_Wireless_Channel_Estimation` |
| ★★ **MIT，可用** | ★ `Kylin9511/CRNet`、★ `SIJIEJI/CLNet`（★ 80 stars，2026-09 活跃）、★ `tangshunpu/DCRNetV2`（★ 带 Sionna-RT-Mix5 权重）、★ `XML124/CDRN-channel-estimation-IRS`（★ 2021；★ "CDRN" = 卷积去噪残差网络，**不是** diffusion）|
| ★★ **非商用** | ★ `SAIC-MONTREAL/CeBed`（CC BY-NC 4.0）|
| ★★ **无许可 ⇒ 保留所有权利** | ★ 三个 `ChannelNet` 副本（★ 且它是 **VehA 二维图像玩具设置，不是 3GPP，无 DMRS/LDPC**）|
| ★ **代码从未发布** | ★ `ReQuestNet`（Qualcomm，arXiv 2508.08790，仅 CE）；★ **`ReEsNet` 无法核实存在**（★ arXiv 全文零命中）|
| ★★ **不存在** | ★ **GitHub 上没有任何面向 5G NR 的 diffusion 型信道估计器** |

### 11.7 ★★ 一条"真硬件 PoC"的线索（★ 若日后需要）

★ **CENTRIC** EU 项目（★ Horizon 101096379，★ **就是资助 `neural_rx` 的那个 grant**）
★ 有公开交付物 **D2.3 / D3.5 / D5.2**，★ 描述含神经接收机输入堆叠的数字硬件架构。
★★ **已识别，未阅读。**
★ 另有 arXiv 2512.13263（2025-12）**DFT-Net + Demod-Net 在 FPGA 上**，
★ ~1.5 dB BER 增益、执行时间低 66% —— ★ **通用 OFDM，无代码发布**。

### 11.8 ★★★ 底线未变，但建设计划多了一条已知风险 + 一条合规麻烦

| ★ | ★ 内容 |
|---|---|
| ★★★ **已知风险** | ★★ **Sionna 2.x 在 `torch.compile` 下的复数张量处理** ⇒ ★ **先用实值对做 spike**（★ 与 ANE 要求一致，不是额外代价）|
| ★★ **合规麻烦** | ★★ **许可扫描器会对 Sionna / `sionna-rk` / `pyAerial` 三处假阳性**，★ 三者其实都是 Apache-2.0 ⇒ **需人工复核** |

## 12. ★★★ C 项核查完毕：**RADE 的 iOS app【没有】用 ANE**（★ 否定结果，★ 但强化了"我们是第一个"）

★ 我 clone 了 `peterbmarks/RADE_decode`（★ RADE = HF 无线电的 OFDM 神经编解码器，
★ 之前被列为"最接近的无线 PHY 跑在 Apple 芯片上"的先例，★ 建议花 10 分钟核实）。

### 12.1 ★★★ 结论：**零 CoreML / 零 ANE / 零 `.mlmodel*` 文件**

| ★ 检索 | ★ 结果 |
|---|---|
| `coreml` / `MLModel` / `MLComputeUnits` / `neuralengine` / `.mlmodelc` / `.mlpackage` / `compute_units`（★ 扫全部 `.swift`/`.m`/`.mm`/`.h`/`.pbxproj`/`.md`）| ★★ **0 命中** |
| `find . -iname "*.mlmodel*"` | ★★ **0 个文件** |

### 12.2 ★★★ 它的神经译码器**是什么**：**纯 C 移植，权重烧进源码**

```
Libraries/rade/rade_enc_data.c   24 713 342 字节
首行: /* Auto generated from checkpoint checkpoint_epoch_100.pth */
```
★ 目录里是 `rade_dec.c` / `rade_dec_data.c` / `rade_enc_data.c` / `nnet.h` /
★ `kiss_fft.c`（★ 自带的 FFT）—— ★★ **模型被静态转译成普通 C 循环，权重是 C 数组。**

### 12.3 ★★★ 这对我们意味着什么

★★★ **"最接近的先例"其实是【反例】**：★ 它是**把神经网络转成纯 C**，
★ 而不是用 CoreML/ANE。★ **它跑在 CPU 上。**
★★★ **⇒ §8.6 的"零先例"论断【更强了，不是更弱了】**：
★★ **没有任何仓库把神经接收机导出到 CoreML/ANE，也没有任何已发表工作把无线 PHY 神经网络
跑在 ANE 上 —— 现在连"最接近的那个"也被证实【没有】用 ANE。**

★ **但它另有一条正面价值**：★★ **它证明"把一个训练好的 PHY 神经网络部署到移动设备上"是可行的**
（★ 有 TestFlight 版本）—— ★ 只是**走的不是 NPU 路线**。
★ 参考文献：[RADE 论文](https://ar5iv.labs.arxiv.org/html/2505.06671v2)、
★ [peterbmarks/RADE_decode](https://github.com/peterbmarks/RADE_decode)、
★ [pepefrog1234/RADE_decode](https://github.com/pepefrog1234/RADE_decode)、
★ [drowe67/radae](https://github.com/drowe67/radae)、
★ [TestFlight](https://testflight.apple.com/join/3yT3Q7h9)

---

## 13. ★★★ 重新校准：**派发地板【不是】硬截止**（★ 用户更正，★ 我之前的框定过紧）

> ★ 用户指出三点：★ ① 硬件在快速发展，设计要有前瞻性；★ ② **15 kHz SCS 的 slot 是 1000 µs**；
> ★ ③ **并非每个 slot 都有 PUSCH**，实测多个 slot 才有一次 PUSCH 处理；
> ★ ④ 前面已证明 **Metal LDPC 高达 ~3000 µs 时手机仍能成功起飞** ⇒
> ★★ **一次深度 3 的前向【不必】在 1 个 slot 内完成。**
> ★★ **我接受这个更正，并在此重写判据。**

### 13.1 ★★ 先更正我自己的两处

| # | ★ 我的说法 | ★ 更正 |
|---|---|---|
| ★ **1** | ★ 我一直说"0.5 ms 时隙" | ★★ **那是【我们】的配置**（★ n78 20 MHz，`common_scs: 30` ⇒ slot = **500 µs**，★ 20 slots/帧）。★★ **而 15 kHz（FR1 基准numerology）是 1000 µs/slot、10 slots/帧** ⇒ ★ **一般情形下预算是 1 ms，不是 0.5 ms** |
| ★ **2** | ★★ 我说"一次派发吃掉 0.5 ms 时隙的 38–46% ⇒ 时隙被吃光" | ★★★ **那个推论建立在"必须在【本 slot 内】完成"这个未声明的假设上。★ 该假设是错的。** |

### 13.2 ★★★ 我们自己语料里的 **PUSCH 实际节奏**（★ 实测，不是估计）

★ 从 `aillr_cap002` 的 **1956 个捕获**的 slot 号（★ 跨度 20 472 slots）统计相邻间隔：

| ★ 间隔 | ★ 时间 | ★ 次数 |
|---|---|---|
| ★★ **1 slot** | ★ **0.5 ms** | ★ **1282** |
| ★★ **8 slots** | ★ **4.0 ms** | ★ **639** |
| ★ 80 slots | 40 ms | 8 |
| ★ 20 slots | 10 ms | 8 |
| ★ 其余 | — | 少数 |

★★★ **⇒ 节奏是【双峰】的，而不是均匀的**：
★ **约 2/3 的相邻接收是背靠背（0.5 ms）**，★ **约 1/3 相隔 4 ms**。
★★ **所以"多个 slot 才有一次 PUSCH"与"有背靠背的 PUSCH"两件事【同时成立】。**
★★★ **⇒ 判据不能写成"必须 1 slot 内完成"，也不能写成"总有 4 ms"。**
★★ **正确的判据是：在与 PUSCH 实际节奏和 HARQ 窗口相匹配的预算内完成。**

### 13.3 ★★ 已有的 ~3000 µs 先例（★ 用户指出的关键事实）

★ 实测：★ 跳时间 **1893 µs ≈ `ul_rx_wait` 1057 µs + `defer_wait` 836 µs**
（★ `phy_pipeline_gpu` 设计 §7236）。
★ 另有 Metal LDPC 首解码 **49 ms/33 ms 的 JIT 税**（★ 靠启动预热消除）。
★★★ **⇒ 我们的链【已经】在跑远超 1 个 slot 的跳，★ 而链路是可用的。**
★ **这直接证明"深度 3 不必在 1 slot 内完成"。**

### 13.4 ★★★ 而且**上一轮 AI CE 已经实机验证过这条路**

★ 上一轮的 **G-4 实机 A/B**（★ OnePlus 8T）：
★ **cpu 首传 CRC 90.7%（CE 25 µs）** ★ vs **helena 30.6%（NN 跑了 48 417 次）**。
★★ **NN 臂【成功跑起来了】，只是准确率差**（★ 那正是 pitfall 18 的高 SNR 破坏问题）。
★★★ **⇒ "在真实链里跑一个 ONNX 神经网络前向"这件事【已经做过】，不是未知。**

### 13.5 ★★★ 重写后的判据

| ★ 项 | ★ 旧（过紧） | ★★ 新 |
|---|---|---|
| ★★ **时延约束** | ★ "必须 1 slot（0.5 ms）内" | ★★ **"在 PUSCH 实际节奏 + HARQ 窗口内"**（★ 实测有 4 ms 间隔，★ 也有 0.5 ms 背靠背）|
| ★★ **派发地板（~0.23 ms）的角色** | ★★★ **"致命的可行性拦路虎"** | ★★ **"吞吐/调度问题，不是硬截止"** —— ★ 它在**背靠背 PUSCH** 时最吃紧，★ 而在 4 ms 间隔时**完全不是问题** |
| ★★ **一票否决项** | ★ 派发地板 | ★★ **回到 fp16 动态范围（`2^15 = 32768` 累加器上限）** —— ★★ **这一条与 slot 长度无关，仍然是一票否决级** |
| ★★ **设计形态** | ★ "每 slot 一个融合程序" | ★★ **"融合仍然要（省派发、省能量），但不必为了 1 slot 而极端设计"** |
| ★★★ **前瞻性** | — | ★★★ **应当设计成【可随 ANE 演进受益】的形态**：★ 派发地板会随硬件下降，★ **而算力/能效优势会保留** |

★★★ **⇒ 我此前把派发地板定为"可能杀死方案"，这个框定【收回】。**
★★ **真正的一票否决只剩下 fp16 动态范围一条**（★ 而它有缓解：★ 尺度规划 + 实值对 + 分段）。

---

## 14. ★★★ 建设计划（可执行，★ 汇总 §7–§13 的全部约束）

> ★ 本节是**唯一需要照着做的一节**。★ 它把散落在各节的约束合并成：★ **目标 → 判据 → 分层设计 →
> 步骤 → 验收**。

### 14.1 ★★★ 目标的一句话（★ 不可漂移）

★★★ **不是"造一个更准的接收机"，而是"用 ANE 以更低能量/占用完成同一个深度 3"。**
★★ **准确率判据是"不劣化"，不是"更好"** —— ★ 因为文献与我们的实测都表明更好不存在。

### 14.2 ★★★ 判据（★ 四级，★ 全部预先登记）

| ★ # | ★ 判据 | ★ 通过条件 | ★ 怎么测 |
|---|---|---|---|
| **C1** | ★★ **准确率不劣化** | ★★ **coded BLER 与现役 Metal MMSE 链打平**（★ 不是更好）| ★★ **经真实 NR LDPC 译码后的 coded BLER**（★ **绝不用 NMSE/MI/未编码 BER**）|
| **C2** | ★★★ **能量/占用更省** | ★★ 同功下 **J/接收更低**，★ 或 **GPU 占用显著下降** | ★★ `powermetrics` 三轨道（cpu/gpu/**ane**）+ GPU residency |
| **C3** | ★★ **时延不劣化** | ★★ 在 **PUSCH 实际节奏 + HARQ 窗口**内（★ **不是"1 slot 内"**）| ★ 逐槽 P50/P99；★ 节奏见 §13.2 |
| **C4** | ★ **放置确实在 ANE** | ★★ 用 **`MLComputePlan`** 确认（★ **绝不用功率轨道推断**）| ★ §9.2 |

★★ **三条引擎臂必须都跑**：★★ **ANE / MPS(GPU) / CPU(BNNSGraph)** ——
★ 因为**小而派发受限的网格，CPU 可能直接赢**（★ §8.2）。

### 14.3 ★★★ 分层设计（★ "深度 3 的哪一部分交给网络"）

★★ **不要把整个深度 3 一次性交给网络。** ★ 按**风险与收益**分三层，★ **每层独立可回退**：

| ★ 层 | ★ 网络做什么 | ★ 经典做什么 | ★ 为什么这样分 |
|---|---|---|---|
| ★★ **L1（先做）** | ★★ **只做解映射**：输入（均衡后符号 + 每 RE 噪声方差），★★ **输出 LLR** | ★ 经典 CE + 均衡 | ★★ **最小、最安全**：★ 输入输出契约清晰（★ 与现有 `demodulation_mapper::demodulate_soft` 同形）；★ **能立刻验证"NN → LLR → 译码 → CRC"整条链与 C2**；★ 且**明确不期望准确率增益**（★ P0 已证经典判决=理想）|
| ★★ **L2** | ★★ **解映射 + 均衡**（输入原始网格 + 信道估计）| ★ 经典 CE | ★ 中等风险；★ 能验证"NN 能否在均衡上省能量" |
| ★★★ **L3（最后）** | ★★★ **CE + 均衡 + 解映射（真·深度 3）** | ★ 无（★ 仅前级 DFT 与后级译码）| ★★ **收益最大、风险最大**；★ **必须在 L1/L2 已证明 C2 之后再做** |

★★★ **理由**：★ **L1 就能把"ANE 能不能承载 PHY 工作负载"这个【空白】填上**（★ §0.4），
★ 而**它的失败不会浪费 L2/L3 的投入**。

### 14.4 ★★★ ANE 硬约束（★ 设计输入，★ 逐条落实）

| ★ 约束 | ★ 值 | ★ 设计动作 |
|---|---|---|
| ★★★ **累加器上限** | ★★ **`2^15 = 32768`**（★ 不是 65504）| ★★ **尺度规划**：★ 均衡与 LLR 的中间量先归一化；★ **LLR 先截断再输出** |
| ★★ **fp16 端到端** | ★ fp32 ⇒ **ANE 被排除** | ★★ **无 fp32 字面量**；★ 复数用**实值对**（★ 恰好也是 Sionna 2.x 的缓解，见 §11.1）|
| ★★ **末轴连续 + 64B 对齐** | ★★ 单元素末轴会被 pad 到 64 B（**32× 内存**）| ★★ **末轴 ≥32 个 fp16 元素**；★ 用官方推荐 **`[1, C, T, 1]` BC1S** |
| ★★ **算子白名单** | ★ 原生：2D conv（含 depthwise/grouped）、matmul、**融合 SDPA**（★ 与 Apple 规则冲突，★ 待测）、norm、pooling、逐元素/激活 | ★★ **不用**：RNN/LSTM/GRU、gather/scatter、top-k/sort、动态 slice、`reduce_prod`、`mod`、逻辑运算 |
| ★★ **`nn.Linear`** | ★★ 分解为低效算子，**可能落 CPU** | ★★ **一律用 1×1 `Conv2d`** |
| ★★ **形状** | ★ 必须**完全静态** | ★★ **`EnumeratedShapes`**（★ **不用 `RangeDim`** —— ★ Frequent 时只有默认形状能跑 ANE）|
| ★★ **dilation** | ★ Apple：大 dilation 率贵，★ 要分解成 dilation-2/3 链 | ★★★ **残差块不要用空洞卷积**（★ ⇒ **从头训，不移植 checkpoint**）|
| ★★ **FFT** | ★ coremltools 把 `torch.fft` 降为 **O(N²) 稠密 DFT matmul + fp32 cast** | ★★ **FFT 留在主机/Metal** |
| ★ **目标版本** | — | ★★ **`ct.target.iOS17`**（★ 不是 iOS18；★ conv/matmul 在 iOS15 基线里）|

### 14.5 ★★★ 模型规模与训练

| ★ 项 | ★ 值 / 做法 |
|---|---|
| ★★ **参数预算锚点** | ★★ **`1e5`**（★ 不是 `1e6`）—— ★ NVIDIA 实时 NRX **1.4e5**；★ 我们的 HELENA **1.16e5**；★ DeepRx 的 1.2M 是**非实时** |
| ★★ **数据生成** | ★★ **Sionna 2.x**（★ Apache-2.0、★ PyTorch 原生）★ 3GPP 合规的 PUSCH/DMRS/LDPC/TB |
| ★★ **训练环境** | ★★ **已有**（★ `~/ai_ce_work/venv`，★ TF 2.21 + tf-keras + coremltools 9.0）★ —— ★ **但 Sionna 2.x 是 PyTorch，★ 需要装 PyTorch 侧** |
| ★★★ **真实数据** | ★★★ **必须用真实语料微调** —— ★ 上一轮的教训：★ **合成训练的模型在真实信道+邻道干扰上【劣于】经典**（★ G-4：★ helena 30.6% vs cpu 90.7%）|
| ★★ **防分布外破坏** | ★★ **训练包络要宽**（★ 上一轮扩到 **−5..55 dB**）★ **+ 运行时软混合（α-blend）** ★ —— ★ **绝不用硬门控**（★ 会挡死；★ 硬微调会灾难性遗忘）|
| ★★ **回退** | ★★ **逐槽神经+经典【并行仲裁】**，★ 不是"AI 看着不好就回退"（★ 高 Doppler 下经典会崩而神经能工作，★ 分歧式回退实测 60.5% 回退率）|

### 14.6 ★★★ 步骤（★ 每步独立可交付）

| ★ 步 | ★ 内容 | ★ 产出 | ★ 依赖 |
|---|---|---|---|
| ★★ **S0** | ★★★ **fp16 动态范围专项**：把**均衡与解映射的中间量范围实测出来**，看 `2^15` 是否真会被击穿 | ★★ **唯一一票否决的答案** | ★ 无（★ **用现有语料算，不需要模型**）|
| ★★ **S1** | ★★ **三引擎量测台**：ANE / MPS / **CPU-BNNSGraph**，量**放置（MLComputePlan）+ 三轨道功率 + 逐槽时延** | ★★ **C2/C3/C4 的基线** | ★ 无（★ 可用现有 `.mlmodelc`）|
| ★★ **S2** | ★★ **L1 层**：只做解映射的 NN，★ 输入（均衡符号 + 噪声方差）→ **输出 LLR** | ★★ **"NN→LLR→译码→CRC"整链打通 + C1/C2** | ★ S0、S1 |
| ★ **S3** | ★ **L2 层**：NN 做均衡 + 解映射 | ★ 中等 | S2 |
| ★★ **S4** | ★★ **L3 层**：真·深度 3 | ★★ 最终形态 | S3 |

★★★ **建议从 S0 与 S1 开始** —— ★ **两者都不需要训模型、不需要新数据**，★
★ **且 S0 的结果可能改变整个设计形态**（★ 若 `2^15` 真被击穿，★ L2/L3 的形态要重设计）。

### 14.7 ★★ 已知风险清单（★ 随方案一起引用）

| ★ 风险 | ★ 缓解 |
|---|---|
| ★★★ **fp16 累加器 `2^15`** | ★ 尺度规划 + 实值对 + **分段**（★ S0 先量）|
| ★★ **Sionna 2.x 复数张量在 `torch.compile` 下的编译/内存问题**（★ §11.1）| ★ **复数限制在 I/O 边界，内部实值对**（★ 与 ANE 要求一致）|
| ★★ **许可扫描器对 Sionna/`sionna-rk`/`pyAerial` 假阳性**（★ §11.2）| ★ 人工复核 |
| ★★ **`NVlabs/neural_rx` 非商用**（★ §10.1）| ★★ **只当架构参考，不复用代码** |
| ★★ **派发地板**（★ 非致命）| ★ 融合；★ **设计成可随硬件演进受益** |
| ★★ **高 SNR 分布外破坏**（★ 上一轮的 attach 死循环）| ★ 宽包络训练 + α 软混合 |
| ★ **CoreML 进入维护期、Core AI 未发布**（★ §8.7）| ★ 现在部署 CoreML；★ **设计保持可迁移** |
| ★★ **证据质量**：ANE 的最丰富表征主要来自**一篇未同行评审的预印本**（★ §8.8）| ★ 关键结论**在本机复现**（★ 已做：§3、§9.2、§12）|

### 14.8 ★★★ 一个必须在 L1 就解决的接口问题：**调制方式怎么进网络**

★★ 现有解映射接口是：
```cpp
virtual void demodulate_soft(span<log_likelihood_ratio> llrs,
                             span<const cf_t>           symbols,     // 均衡后符号
                             span<const float>          noise_vars,  // 每 RE 均衡后噪声方差
                             modulation_scheme          mod) = 0;    // QPSK / 16QAM / 64QAM / 256QAM
```
★★★ **问题**：★ `modulation_scheme` **是一个枚举，不是张量** —— ★★ **它进不了神经网络。**
★ 而**它必须进去**，★ 因为**同一组 (symbol, noise_var) 在不同调制下对应完全不同的星座与 LLR**。

★★ **我们的语料里调制是混合的**（★ `aillr_cap002`）：

| ★ 调制 | ★ 数量 | ★ 占比 |
|---|---|---|
| ★ 256QAM | 1563 | ★ **79.9%** |
| ★ 64QAM | 339 | 17.3% |
| ★ 16QAM | 51 | 2.6% |
| ★ QPSK | 3 | 0.2% |

★★★ **三条可选路线（★ 必须选一条）**：

| ★ # | ★ 做法 | ★ 优 | ★ 劣 |
|---|---|---|---|
| ★★ **A** | ★★ **每调制一个模型**（★ 4 个 `.mlmodelc`）| ★★ **零额外输入、每模型最小、无 fp32/条件逻辑**；★ 与 `EnumeratedShapes` 分桶的思路一致（★ §8.3）| ★ **4 份权重**（★ 但 1e5 参数 × 4 仍很小）|
| ★ **B** | ★ **调制作为 one-hot 输入通道** | ★ 一个模型 | ★★ **改变了输入契约**（★ 要在网格上堆一个常量通道）；★ 且**每调制一个"模型内分支"会引入条件逻辑，ANE 不擅长** |
| ★ **C** | ★ **只用峰值调制（256QAM）训一个模型，其它调制回退经典** | ★ 最简单、最快验证 | ★ **覆盖率低**（★ 只有 80%）★ 且**牺牲了低阶调制的省电机会**（★ 而低阶调制恰恰是能效最敏感的场景）|

★★★ **建议 A**：★ **每调制一个模型**。★ 理由：
1. ★★ **它不改变接口契约** —— ★ 选择哪个模型由**上层按 `pdu.modulation` 完成**，
   ★ 对链的改动最小（★ 与 `pusch_receiver_backend` 同构：★ 一个选择点）；
2. ★★ **每个模型都最小**（★ 无 one-hot 通道、无分支），★ 对 ANE 最友好；
3. ★★ **与上一轮的宽度分桶策略一致**（★ 52/106 两个桶，★ `EnumeratedShapes`），
   ★ 是**已被验证可用的模式**。

★★ **而 LLR 的尺度问题在这里同解**：
★★★ **模型的输出必须是【已截断、已按 fp16 安全尺度归一化】的 LLR** ——
★ 因为 **`2^15 = 32768` 的累加器上限**（★ §7.4）★ 与 **LLR 天然会饱和**（★ 实测饱和率 0.398）
★ 这两件事**在输出层就相遇**。★ **截断放模型外一层，或放输出层，二者都必须显式做。**
