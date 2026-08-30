# OCUDU PUSCH RX 链 AI 信道估计（AI Channel Estimation）实施规划报告

> 目标：从 147 篇已下载的信道估计/AI 文献中筛选出可落地的 AI 增强信道估计算法，
> 替换 OCUDU（srsRAN 体系）PUSCH RX chain 中的 channel estimation 模块，
> 充分利用 Apple Silicon GPU（Metal/MPS），性能不低于实用 MMSE，
> 推理时延满足 NR slot 内几百微秒的总预算。
>
> 版本：v1.0 ｜ 状态：规划（待评审）

> **MEMO 2026-08-30（metal_mmse 完成后更新）**：本规划 v1.0 撰写时仓库尚无 MMSE
> 实现，故 §0-3 的"L0 基线升级（必做）"列为前置任务。**该基线现已完成**：
> `port_channel_estimator_metal_mmse_impl`（2D 时频块 MMSE，R=R_t⊗R_f，σ² 复用
> 经典噪声估计，τ̄/f_d 为固定可配常量，`channel_statistics_estimator` 接口为 v2
> 估计/AI 预留）已通过全部单测与实链验证（500 ping）。L0 由"待建"变为**现成**，
> G2 的"≥ MMSE"验收基线直接对拍 metal_mmse。另注意：AI 计划 §2.2 的时延锚点已
> 被 2026-08-30 实测更新（CE metal_mmse 实链 median ~255 µs@36 PRB/3 DMRS、
> cpu 63.6 µs；Metal 基础设施的 occupancy/warm-up/粮草先行/bit-exact 经验教训
> 见 `docs/apple_silicon_heterogeneous_gnb_plan.md` 与 CE `PLAN.md` §7.0.15）。

> **G1 延迟原型实测记录（2026-08-30，M4 Pro，macOS 26.5）**：
> 官方仓库 [HELENA_Channel_Estimation](https://github.com/miguelhdo/HELENA_Channel_Estimation)
> 自带 `HELENA.onnx` 与 `010625_HELENA_CE_model.keras`（116,290 参数 ✓）。经
> SavedModel → coremltools 9 转换（**必须固定输入形状 (1,612,14,2)**，flexible
> shape 版本会 SIGBUS），实测（batch 1，随机输入，200 次）：

| 计算单元 | p50 | p95 | p99 |
|---|---|---|---|
| CPU_ONLY | 693 µs | 744 | 908 |
| CPU_AND_GPU（MPS 路径） | 690 µs | 711 | 747 |
| **CPU_AND_NE（ANE）** | **141 µs** | 179 | **208** |
| ANE + fp16 权重 | 142 µs | 175 | 196 |
| ANE batch-4（4 端口批） | 794 µs（每端口 199 µs） | – | – |

- **ANE 是唯一可行运行时**：141 µs p50 / 208 µs p99，**已优于 metal_mmse 基线
  （~255 µs）**；MPS/GPU 路径无收益（MHA 分解落 CPU，690 µs ≈ CPU）；fp16 无
  额外收益；batch-4 无批处理红利（4 次单发 564 µs 更优）。
- **G1 严格口径未过**（原门禁 p99 ≤ 100 µs），**但决策口径已变化**：基线
  metal_mmse 本身 255 µs，HELENA@ANE 在时延上已达标（比基线快 1.8×）。时延
  数字含 CPU→ANE→CPU 往返拷贝（Python predict 全链路）。
- **裁决**：运行时选 **Core ML（ANE）**，放弃 MPS 手写路径；下一步 G2——在
  我方合成数据集上重训/微调（§7）后对拍 metal_mmse 的 NMSE，并验证真实
  DMRS 网格数值（作者训练集的归一化与我方不同，直接权重不可用）。

> **G2 阶段 A 记录（2026-08-30）：作者数据集重训通过，精度方向确认**
> 数据集 Zenodo 10.5281/zenodo.15210986（5.32 GB，11,264 样本，SNR 0-36 dB，
> 4 种信道 profile）。**关键格式发现：HELENA 输入 = LS 的线性插值网格**
> （|X−Y_lin|=0，稠密 612×14×2，非稀疏 LS）——与 OCUDU 经典路径的插值输出
> 同构，部署时直接复用现有 LS+插值阶段，无需额外归一化层（作者 LS 幅度均值
> 0.655 vs 标签 0.574，与我方 |h|~0.57 的 LS 同尺度）。
> 重训（tf-keras/M4 Pro CPU，Adam 3.28e-4，batch 32，35 epoch，~13 min）：

| 估计器（作者测试集） | 总 NMSE | SNR 0-5 dB |
|---|---|---|
| LS | −0.15 dB | −0.15 |
| 线性插值 | −6.72 dB | −4.17 |
| PracticalMMSE（作者基线） | −14.27 dB | −12.71 |
| **HELENA（35 epoch）** | **−15.03 dB** | **−13.68** |

> HELENA 超作者 PracticalMMSE **+0.76 dB**（低 SNR +1 dB）；论文原报 −16.78 dB
> 尚未追平（需更多 epoch/原训练超参）。重训模型转 Core ML 后 ANE 时延不变
> （p50 141.5 µs）。**下一步 Phase B**：我方 PUSCH 合成集（TDL-A..E、DMRS
> type1/2）微调 + 与 metal_mmse 在统一仿真链上 NMSE 对拍。

---

## 0. 结论速览（TL;DR）

1. **文献筛选**：147 篇论文全部完成全文级扫描与评分（12+8+2+2 个子代理并行）。真正与
   "NR PUSCH DMRS 时频网格估计 + 实时 + GPU" 匹配的高分论文只有少数几篇；约 2/3 与目标无关
   （UAV 安全、RIS 级联信道、OTFS、反向散射、定位等）。
2. **主选算法：HELENA**（High-Efficiency Learning-based channel Estimation using dual
   Neural Attention，arXiv:2506.13408v2）。理由：输入即"DMRS 位置 LS + 补零"的稀疏网格
   （与 OCUDU 现有 LS 阶段无缝衔接）、仅 0.116M 参数 / 77 MFLOPs（51 PRB）、
   NMSE −16.78 dB，比 srsRAN 现行实用估计器（−12.25 dB）好约 **4.5 dB**、比 LS（−3.56 dB）
   好 13 dB；MIT 开源 + 权重 + ONNX 齐全。风险：论文在 V100 上单网格 0.175 ms，
   Apple MPS 上是否满足预算**必须先做原型实测**（决策门 G1）。
3. **基线升级（L0 已完成，2026-08-30）**：~~现行 `port_channel_estimator_average_impl` 是 LS+平均/插值，
   并不是 MMSE。先实现一个实用的经典 MMSE（基于估计 PDP 的 Wiener 平滑）作为
   严格验收基线——AI 必须在该基线上继续有增益才允许默认启用（满足"不低于 MMSE"的硬指标）。~~
   **现已落地**：`metal_mmse`（2D 时频块 MMSE，`expert_phy --pusch_channel_estimator_algo
   metal_mmse`）实链 500 ping 验证通过——G2 的"≥ MMSE"基线直接对拍它即可。
4. **备选/兜底 AI**：DMRS-only 2D CNN 去噪器（CNN4CE 思想）+ 经典插值，全 PRB 范围内
   都在时延预算内（5–50 MMAC），作为 HELENA 的降级备选。
5. **否定项**（读过但不采用）：AdaFortiTran（自注意力 O(S²) 在 273 PRB 达 46.7 GMAC/端口，
   超预算 1–3 个数量级）、Multi-Task Transformer Receiver（结构上只能输出单 CFR 向量、
   无时间维）、生成式/score-based 模型（迭代推理 A100 上 1.5–2 s/次，不可蒸馏）、
   bi-LSTM（串行、GPU 利用率低）。
6. **复用既有资产**：仓库已有 **Metal LDPC GPU 解码器**（`.metallib` 预编译、零拷贝、
   工厂注册、A/B 探针文化），AI CE 模块完全沿用这套范式；实链探针给出 M4 Pro 上
   UL 管线中位数 207–311 µs、LDPC 解码 28–60 µs、前端（CE+均衡+解调）约 190 µs 的实测锚点。

---

## 1. 背景与目标

### 1.1 目标

- 用 **AI 增强的信道估计**替换 OCUDU gNB 的 PUSCH（上行）接收链中现有
  channel estimation 模块（`port_channel_estimator_average_impl`，LS+平滑/插值）。
- 推理放到 **Apple Silicon GPU**（Metal/MPSGraph），复用并扩展已有的 Metal 基础设施。
- **性能硬指标**：部署 SNR 区间内估计精度 ≥ 实用 MMSE（并有清晰的低 SNR/低导频开销增益更好）。
- **时延硬指标**：单 slot 内 CE 阶段控制在 **≤ 50–100 µs（30 kHz SCS）**，不得挤爆
  总 RX 链预算（NR slot = 1 ms@15 kHz / 0.5 ms@30 kHz；整条 UL 管线实测中位 207–311 µs）。
- **泛化硬指标**：离线训练、在线使用，对未见过的 3GPP TDL/CDL 实现、SNR/Doppler/时延扩展变化保持性能。
- **工程约束**：保持 `port_channel_estimator` 接口与 cbf16 输出格式不变；
  噪声方差/SNR/CFO/TA 等旁路指标继续由经典路径提供（LLR 解调依赖它们）；
  必须有优雅降级（A/B 自动回退到经典估计器）。
- 加分项：论文作者 GitHub 参考代码（主选 HELENA 满足）。

### 1.2 论文语料

- 目录 `/Users/jiachengwang/OneDrive/newWork/paper/AI_RAN/channel_estimation/`：147 个 PDF，
  来自一篇综述论文《A Survey of AI Enabled Channel Estimation Methods》的参考文献列表
  （付费文献已替换为内容相近的开放获取版本，映射见同目录 `OA替换说明.md`）。
- 所有 PDF 均已提取全文（`/tmp/papers_txt/`）供结构化评审。

---

## 2. 现状剖析（OCUDU PUSCH RX chain）

### 2.1 调用链与接口

```
pusch_processor_impl::handle_ul_pusch()
 └─ dmrs_pusch_estimator_impl::estimate()          // 生成 DMRS 符号/图案，按 RX 端口派发任务
     └─ port_channel_estimator::compute(grid, port, pilots, cfg)   // ★ 待替换模块
         └─ port_channel_estimator_average_impl    // LS @ DMRS RE + FD 平滑 + TD 平均/插值
     └─ notifier.on_estimation_complete()          // → 均衡(equalizer: ZF/MMSE) → 解调 → LDPC
```

关键接口（`include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h`）：

```cpp
virtual const port_channel_estimator_results& compute(
    const resource_grid_reader& grid, unsigned port,
    const dmrs_symbol_list& pilots, const configuration& cfg) = 0;

// results 必须提供：
get_symbol_ch_estimate(span<cbf16_t>, i_symbol, tx_layer);  // 全时频网格（cbf16=2×bf16）
get_epre(); get_noise_variance();   // ★ LLR 解调依赖噪声方差
get_snr(); get_rsrp(tx_layer); get_cfo_Hz(); get_time_alignment();
```

约束与观察：

- 现有算法 = LS@DMRS RE + FD 平滑（none/mean/filter）+ TD 策略（interpolate/average）。
  **没有 MMSE 实现**，即"不低于 MMSE"目前没有严格基线（见 §6 的 L0 基线升级）。
- 噪声方差由 DMRS 残差（EVM 法）估计、SNR=EPRE/noise、CFO 由跨 DMRS 符号相位估计、
  TA 由时域估计——这些**不需要 NN 提供**，AI 模块只需输出信道网格。
- 每 RX 端口一个任务、由 CPU task executor 并行；`port_channel_estimator_factory_sw::create()`
  目前只返回 average 实现——**工厂是插入点**。
- 数据格式：输入 `cf_t`（复数 float），输出 `cbf16_t`（复数 bfloat16）。GPU 内部用 fp16/fp32，
  边界处转换。
- 配置入口：`pusch_channel_estimator_td_strategy: average`（yml）等，需扩展
  `pusch_channel_estimator_algorithm: average | mmse | helena`。

### 2.2 实测时延锚点（M4 Pro 实链，`ldpc/metal/PLAN.md` 探针数据）

| 测量 | 数值 |
|---|---|
| UL RX 管线（IQ 收到 → CRC OK）median | 207–311 µs（ping 小包 ~207–229 µs；iperf3 大 TB ~288–311 µs） |
| LDPC 解码 median | 28–39 µs（小 TB）/ 60 µs（大 TB） |
| 前端（CE+均衡+解调+解复用） | ≈ 190 µs（主导项） |
| Metal GPU 固定开销 | 290 次 dispatch ≈ 35 µs → 每 slot 必须**单次批处理**提交，避免多命令缓冲 |

结论：**CE 阶段预算定在 ≤ 50–100 µs（30 kHz SCS、51 PRB 级典型配置）**；
273 PRB（100 MHz）× 4 端口是压力工况，允许单独评估。

### 2.3 可复用的 Metal 基础设施（重要先例）

仓库已有完整的 Metal GPU 集成范式（`lib/phy/upper/channel_coding/ldpc/metal/`）：

- ObjC++ `.mm` 引擎（ARC）+ 纯 C++ 适配器；CMake `enable_language(OBJCXX)`、
  链接 `-framework Metal -framework Foundation`；
- shader 离线 `xcrun metal/metallib` 预编译成 `.metallib`，运行时 `newLibraryWithURL` 加载；
- 零拷贝 `newBufferWithBytesNoCopy`（4KB 对齐）+ per-engine buffer cache；
- 同步 `waitUntilCompleted` + `last_gpu_wait_us()` 计时；
- 工厂按字符串注册（`"metal"`/`"metal_flooding"`…），CLI/yml → schema → 验证器 → 工厂贯通；
- 实链 A/B 文化：`[ul_pipeline]`/`[ul_ldpc_decode]` 探针序列、三腿同噪声带对比、
  BLER 对拍、ping/iperf3 E2E 门禁。

AI CE 模块照搬这套模式即可，新增量主要是 MPSGraph（卷积/注意力）而非手写 kernel。

---

## 3. 文献筛选方法与结果

### 3.1 方法

1. 147 篇 PDF 全部提取全文（`/tmp/papers_txt/`）。
2. 用 workflow 分 12 个子代理并行扫描（每代理 ~13 篇），按统一评分卡输出结构化结果；
   两轮补扫覆盖全部 147 篇（零遗漏）。
3. 评分维度（0–10）：① 是否输入=DMRS 导频 LS 稀疏网格（与 NR 接口一致）；
   ② 是否有 vs LS/LMMSE/MMSE 的定量证据及 SNR 条件；③ 复杂度（参数量/FLOPs/实测时延）
   是否落在数百 µs 预算内；④ 泛化证据（训练/测试信道模型、Doppler/时延扩展失配测试）；
   ⑤ GitHub 参考代码。
4. 对得分前 6 的候选做第二轮深度核查：精读论文 + 克隆/检索 GitHub 仓库 +
   按 NR 网格（51/273 PRB、DMRS type 1/2、2–4 RX 端口）估算 MACs/时延 +
   设计 OCUDU 集成方案。

### 3.2 筛选结果统计

- 与目标直接相关并给出高分的：约 15 篇（score ≥ 4）；有参考代码的：HELENA（MIT+权重）、
  CNN4CE（GPL-3.0、无权重）、AdaFortiTran（MIT、无权重）、score-based-channels（官方 PyTorch）、
  ChannelNet/DeepPilotDesign（Soltani）、GM-LAMP（清华主页脚本）。
- 其余约 130 篇：与目标无关（UAV 轨迹/安全、RIS 级联信道、OTFS、反向散射、定位、THz、综述/教材等）
  或证据不足（无 MMSE 对比、无复杂度数据）。

### 3.3 高分论文清单（score ≥ 4，详见附录 A）

| 论文 | 分数 | 关键证据 | 代码 |
|---|---|---|---|
| **HELENA**（dual Neural Attention） | **8** | vs srsRAN 实用估计器 NMSE +4.5 dB；0.116M 参数/77 MFLOPs；0.175 ms V100 | MIT 仓库+权重+ONNX |
| Deep CNN-based CE for mmWave M-MIMO（CNN4CE） | 6 | 优于非理想 MMSE、逼近理想 MMSE；147 µs（1080Ti） | GPL-3.0 仓库（无权重） |
| ML-based 5G-and-Beyond CE for MIMO-OFDM | 6 | TDL-C + NR DMRS type1；DL 在低 SNR 优于 LMMSE | 无 |
| AdaFortiTran（自适应 Transformer） | 5 | 与 LMMSE 持平、对 Doppler/SNR 鲁棒；0.12–0.28M 参数 | MIT 仓库（无权重） |
| Deep learning-aided 5G channel estimation | 5 | TDL-A+5G 导频；低 SNR 优于 LMMSE（高 SNR 反退化） | 无 |
| DL CE for doubly selective fading | 5 | 优于 BEM-LMMSE；Doppler 失配鲁棒性有量化 | 无 |
| Wideband CE with GAN | 5 | −5 dB SNR 下匹配 LS@20 dB/LMMSE@2.5 dB；省 70% 导频 | 无 |
| A Study on MIMO CE by 2D/3D CNN | 4 | 5G-NR 8×8 数据集；省 62.5% 导频 | 无 |
| Deep learning based CE for IEEE 802.11p（STA-DNN） | 4 | 优于 STA/MMSE-VP；复杂度 −55.7% vs AE-DNN | 无 |
| DL for beamspace CE（GM-LAMP） | 4 | 优于 AMP/OMP；O(T·M·N) 低复杂度 | 清华主页脚本 |
| DL for CE: Interpretation, performance, comparison | 4 | 理论：ReLU DNN 渐近逼近 MMSE；失配敏感警告 | 无 |
| DL for joint CE & detection（CENet） | 4 | 优于 LS/MMSE+高斯插值；SAN 重 | 无 |
| DL joint pilot design + CE（multiuser MIMO） | 4 | 优于 LMMSE 方案；需重设计导频（与 NR 冲突） | 无 |
| Multi-Task Transformer Receiver | 4 | 少导频下优于 LS/LMMSE；结构不匹配 NR | 无 |
| Power of deep learning（Ye & Li） | 4 | 奠基工作；端到端检测非网格输出 | 社区移植 |
| Towards DL-aided CE & CSI feedback for 6G | 4 | 案例：DL 时域 CE 优于 LS/LMMSE；含训练方法论 | 无 |

---

## 4. 候选算法深度评估

### 4.1 HELENA —— 主选（PROMISING，待原型门禁后转 RECOMMENDED）

**架构**（已从 .keras config + ONNX 图逐层验证）：

```
输入 (1, 612, 14, 2) 稀疏 LS 网格（DMRS 位置有值、其余为零；实/虚两通道）
→ Conv2D(32, 12×2, ReLU, same) → Conv2D(2, 6×7, ReLU, same)
→ 按 PRB 切 patch（51 个 token，每 token 336 维）→ Dense 336→64
→ 4 头 MHSA(64) → 残差 + LayerNorm → SE 通道重标定
→ 重构头 Dense 64→336 → 重构网格 + 全局残差(稀疏输入) → 输出全网格
```

- 参数量 0.116M；FLOPs 77.2M（51 PRB）；操作集极简：Conv2D、ReLU、GEMM、Softmax、
  LayerNorm、Sigmoid、残差加——**全部是 MPSGraph 原生算子**，无 GELU/上采样/复杂控制流。
- 精度：论文 TensorRT fp16，0.175 ms（V100，单网格 51 PRB）。注意该测值受
  host↔device 拷贝 + ~50 次 kernel launch 影响，FLOPs 地板仅 ~5 µs；
  Apple 统一内存可省去拷贝，**真实 MPS 延迟必须实测**（决策门 G1）。
- 性能：NMSE −16.78 dB vs srsRAN 实用估计器 −12.25 dB vs LS −3.56 dB（TDL-A..E、
  SNR 0–20 dB、51 RB、SISO 下行）；比 CE-ViT 仅差 3.1%，参数少 8×、快 45%。
- **代码**：`https://github.com/miguelhdo/HELENA_Channel_Estimation`（MIT；权重 .keras+
  ONNX 齐备；评估脚本齐备；**缺训练脚本**，架构需从配置重建；Zenodo 数据集链接齐全）。
- 泛化缺口（必须补训）：下行 PDSCH SISO 单 DMRS 符号 → 上行 PUSCH 多端口/多层 +
  DMRS type 1/2（1–2 符号）；形状硬编码 51 PRB（**权重本身与 PRB 数无关**——卷积平移
  等变、注意力/Dense 按 token，动态 PRB 只需重实现，不需重训）；无直接 MMSE 对比点。
- MACs 估算（按端口，动态 N=PRB 数）：51 PRB ≈ 43.1 MMAC（卷积占 84%）；
  273 PRB ≈ 261.5 MMAC（自注意力 N² 增长，从 1.3M → 38.2M MAC，占 15%）。
- MPS 时延估算：单次前向受 dispatch 主导（15–20 kernel），估 100–300 µs（51 PRB）；
  **4 端口 × 1–2 层合并成 batch 提交一次**可摊薄到 ~50–150 µs；273 PRB×8 模型 ≈ 4.2 GFLOP
  风险 >1 ms（100 MHz 多端口是硬工况）。CPU 回退不现实（BNNS 需 ms 级）→ **CPU 回退 = 经典估计器**。

### 4.2 CNN4CE（SF/SFT/SPR-CNN）—— 备选（DMRS-only 去噪变体）

- 9 层 3×3 conv（64 通道）+ BN + ReLU + tanh 输出，~0.30M 参数；优于非理想 MMSE、
  逼近理想 MMSE；147 µs（1080Ti）≈ 1 TMAC/s 有效吞吐。
- 硬伤：在 32×16 **天线阵列空间**上卷积、Q=2 子载波——与 NR 上行 2–4 端口 +
  612–3276 子载波的时频网格完全不同，权重不可迁移；**按 SNR 分别训练**（部署不可行）；
  只输出导频子载波、从不评估数据 RE 插值；仓库 GPL-3.0、无权重、TF1.x EOL。
- 适配成本：全网格版 5.1–27.3 GFLOP/端口 → 0.5–13.6 ms（超预算 3–90×）。
  **唯一在预算内的形态：DMRS-only 去噪**（NN 只去噪 DMRS RE 上的 LS，经典插值填数据 RE）：
  5–50 MMAC → 10–50 µs（含 dispatch）。作为 HELENA 之外的**降级备选/对比腿**保留。

### 4.3 ML-based 5G-and-Beyond（FDNN/CNN/bi-LSTM）—— 证据补充，不主选

- TDL-C + NR DMRS type1 + 4×4 MIMO（NFFT=256）；DL 在低 SNR（−5 dB 级）对 LMMSE 优势
  达数 dB，但 **15–20 dB 以上优势归零**（恰好是部署区间）；bi-LSTM 最优但串行
  （273 PRB 下 6552 递归步，multi-ms，GPU 利用率 <10%）→ 直接否决；
- CNN 改为全卷积 FCN 形态：19.1–102.1 MMAC/端口/DMRS 符号 → 4 端口批处理 20–60 µs@51 PRB、
  110–330 µs@273 PRB（临界）；无代码、无实测时延。
- 价值：为"LS 网格 → CNN 去噪"路线提供 NR DMRS type1 的独立证据。

### 4.4 AdaFortiTran —— 否决为主选（注意力不可行）

- 结构优雅（CNN 局部 + Transformer 全局 + SNR/Doppler/时延先验 CAM），仅跟踪 LMMSE（不超越）；
- 但 O(S²) 自注意力：51 PRB 1.88 GMAC/端口、273 PRB **46.7 GMAC/端口**（98% 在注意力）→
  MPS 上 0.8–2.5 ms / 20–50 ms，超预算 1–3 个数量级；上采样器/CAM 硬编码网格尺寸；
  无权重、无 FLOPs 数据；CAM 需要 OCUDU 没有的时延扩展/最大 Doppler 在线估计。
- 保留价值：若未来砍掉注意力（CNN-only / 线性注意力）可作备选，当前不进入实施。

### 4.5 Multi-Task Transformer Receiver —— 否决（结构不匹配）

- 8 层 ×384 维 GPT-2 式 ICL 接收机，14.2M 参数；少导频下优于 LS/LMMSE（SISO 64 子载波）；
- 但 CE 头只输出**单 CFR 向量（无时间维）**，无法生成 612×14/3276×14 网格；
  ICL prompt 依赖"整符号导频"，与 NR 分散 DMRS 不符；QPSK+4bit 量化与 64/256-QAM 冲突；
  无代码、无绝对数值；改造即重写。计算量虽小（<0.2 GFLOP/slot），但没有可用形态 → 不采用。

### 4.6 生成式/score-based 家族（GAN/WGAN/扩散）—— 否决（迭代推理不可行）

- 精度最好的一族：WGAN 生成器先验在 −5 dB 下匹配 LS@20 dB / LMMSE@2.5 dB、省 70% 导频；
  score-based 在分布内逼近近似 MMSE（4× 少 FLOPs）。
- 但推理是**迭代的**：100 步 Adam 或 246–2045 步 Langevin，A100 实测 1.5–2 s/次（16×64）；
  即便假想一步蒸馏（该方向在信道估计上**无已发表成果**），退化为普通大卷积去噪器
  （0.5–2.8 GFLOP/端口/层），MPS 上仍超预算 3–30×，且 GAN 本身有高 SNR 误差地板
  （违反"≥MMSE"）。仅作研究跟踪（官方代码 utcsilab/score-based-channels 存在）。

### 4.7 候选对比总表

| 候选 | 参数 | 51 PRB/端口 | 273 PRB/端口 | vs MMSE 证据 | 代码/权重 | 结论 |
|---|---|---|---|---|---|---|
| HELENA | 0.116M | ~43 MMAC（估 50–150 µs 批处理） | ~262 MMAC（风险 >1 ms） | 间接（+4.5 dB vs srsRAN 实用） | MIT+权重+ONNX | **主选（G1 门禁）** |
| CNN4CE 全网格 | 0.30M | 5.1 GFLOP（0.5–1.3 ms） | 27.3 GFLOP（2.7–6.8 ms） | 直接（优于非理想 MMSE） | GPL 无权重 | 超预算 |
| CNN4CE DMRS-only | ~0.1M | 5–50 MMAC（10–50 µs） | 同左线性 | 直接 | 自训 | **备选/降级腿** |
| ML5GB-FCN | 小 | 19–102 MMAC（20–60 µs） | 临界（110–330 µs） | 低 SNR 优于 LMMSE、高 SNR 持平 | 无 | 证据补充 |
| ML5GB-bi-LSTM | 小 | 串行 multi-ms | 更差 | 同上 | 无 | 否决 |
| AdaFortiTran | 0.12–0.28M | 1.88 GMAC（0.8–2.5 ms） | 46.7 GMAC（20–50 ms） | 仅跟踪 LMMSE | MIT 无权重 | 否决 |
| MTTR | 14.2M | <0.2 GFLOP 但无法输出网格 | — | 少导频优于 LS/LMMSE | 无 | 否决 |
| GAN/score-based | ~2M | 迭代不可行 | 迭代不可行 | 精度最好（非实时） | 官方 PyTorch（score） | 研究跟踪 |

---

## 5. 推荐技术方案（分层）

```
L0 经典加固（必做基线）：实用 MMSE（PDP/Wiener 平滑）FD 策略 + 现有 TD 插值
     → 确立"≥ MMSE"验收基准，CPU 上 µs 级，与 AI 无关、独立可用
L1 主选 AI：HELENA 移植 MPSGraph（fp16、动态 PRB、端口批处理）+ PUSCH DMRS type1/2 重训
     → A/B 阴影模式与自动回退，达标后默认启用（先 51 PRB 级配置）
L2 备选 AI：DMRS-only 2D-CNN 去噪 + 经典插值（CNN4CE 思想，自训权重）
     → L1 不达标时的降级腿；或作为低端机型路径
L3 跟踪：ICL（MTTR 思想重设计）、生成式一步蒸馏、AdaFortiTran 无注意力变体
     → 不进主线，每季度评估一次
```

选择逻辑：

1. 文献总体结论是"轻量 CNN/注意力去噪 + LS 输入"是唯一同时满足（精度≥MMSE、百 µs 级、
   泛化证据）的路线；HELENA 是该路线目前证据最强、代码最全的实例。
2. 严格满足"≥MMSE"的办法不是赌某篇论文的曲线，而是：**先实现 L0 实用 MMSE 基线，
   再用 A/B 门禁证明 L1 ≥ L0**（详见 §9 验收标准）。
3. 理论依据（"Interpretation"论文）：充分训练下 ReLU 网络渐近逼近 MMSE 且不需要信道统计先验；
   同时该文警告训练/部署失配会使 DL 迅速劣化——所以重训数据必须覆盖部署分布（§7）。

---

## 6. 系统架构设计

### 6.1 新模块 `port_channel_estimator_helena_impl`

完全实现现有 `port_channel_estimator`/`port_channel_estimator_results` 接口，替换
`port_channel_estimator_average_impl` 的工厂默认路径（可配置切换）。

每端口（推荐所有端口合并一批提交 GPU）处理流程：

```
1. LS@DMRS（复用现有逻辑）：接收 DMRS ÷ 期望符号 → 复数 LS 写入清零网格的 DMRS 位置
   （DMRS type1=6 RE/PRB、type2=4 RE/PRB；1 或 2 个 DMRS 符号 = 填充相应 RE）
2. 归一化：按槽估计尺度（导频 |LS| 均值）缩放，使输入幅值 ~O(1)（训练分布对齐）
3. 打包：实/虚拆两通道 → fp16 张量 [batch=端口×层, H=12×PRB, W=14, C=2]
4. NN 前向（MPSGraph，动态 PRB；权重 fp16；LayerNorm 统计与最终残差加保持 fp32）
5. 反归一化 → 复数 → cbf16（2×bf16）写回结果网格
6. 经典旁路（并行，CPU）：噪声方差（DMRS 残差 EVM）、EPRE、SNR、RSRP、
   CFO（跨 DMRS 符号相位）、TA（时域估计）—— 全部沿用 average_impl 逻辑，
   NN 不产出这些量，LLR 解调路径零改动
7. 缓冲复用：稀疏 LS 网格 / fp16 输入输出 / cbf16 输出 全部槽级预分配、零每槽分配
```

### 6.2 Metal 集成（沿用 LDPC Metal 范式）

- 新增 `lib/phy/upper/signal_processors/channel_estimator/metal/`：
  - `ocudu_metal_nn_engine.mm`（ObjC++ ARC）：MPSGraph 构建与执行封装；
    统一内存零拷贝（`newBufferWithBytesNoCopy`，4KB 对齐）；同步
    `waitUntilCompleted`；`last_gpu_wait_us()` 计时；per-engine buffer cache。
  - `port_channel_estimator_helena_impl.{h,cpp}`：纯 C++ 适配器。
  - CMake：`enable_language(OBJCXX)` + `-framework Metal -framework Foundation`；
    权重离线转成 C 数组/`.bin` + manifest（形状/PRB 约束/校验和），初始化时一次加载。
- 图策略：**每 (PRB 数, DMRS 图案, 层数) 组合构建一次 MPSGraph 并缓存**；
  每 slot 一次 `encode → commit → waitUntilCompleted`（4 端口合并 batch）；
  与 LDPC 解码器共享同一个 MTLCommandQueue（或独立队列，避免与 LDPC 竞争，实测后定）。
- 精度：卷积/GEMM/注意力 fp16；LayerNorm 统计 fp32；残差加与反归一化 fp32；
  边界 cbf16↔fp16 转换用 NEON（CPU 侧已有 cbf16 工具）。
- 备选执行后端：若 MPSGraph 图构建复杂，可走 PyTorch MPS 导出 → coremltools（fp16）
  → Core ML（GPU/ANE）；**决策门 G1 同时评估两条路**，选延迟优者。注：ANE 需拷贝进出、
  且对动态形状支持弱，仅当 MPS 延迟不达标时评估。
- CPU 回退：**不是 CPU 版 NN**（BNNS 需 ms 级，超预算），而是直接回退到 L0 经典 MMSE
  估计器——LS 阶段两者共享，回退成本≈0。

### 6.3 配置贯通（照搬 LDPC 方案）

- yml/CLI：`pusch_channel_estimator_algorithm: average | mmse | helena`
- `pusch_channel_estimator_ai_model_path`（权重 manifest 路径）
- `pusch_channel_estimator_ai_batch_ports: true|false`
- `pusch_channel_estimator_ai_timeout`：`std::chrono::microseconds` 类型
  （遵循 AGENTS.md：时间参数用 chrono 强类型），超时即本槽回退经典路径
- 工厂：`port_channel_estimator_factory_sw::create()` 增加 algorithm 参数；
  `dmrs_pusch_estimator_impl` 构造处透传。

### 6.4 可观测性

- 复用 `ul_pipeline_probe`：在 CE 段内加 `ai_ce_us` 子相位（NN 前向 + 打包转换分开计时）；
- 扩展 `phy_metrics_port_channel_estimator_decorator`：NN 时延直方图、回退计数、
  与经典估计器的 NMSE 差值、NaN/越界计数。
- 阴影模式：双估计器同时运行（经典 + NN），只上报、不影响热路径（上线前 A/B 用）。

---

## 7. 训练与数据管线

### 7.1 数据集（PUSCH 上行合成）

覆盖矩阵（合成器选 MATLAB 5G Toolbox——HELENA 原管线——或开源 Sionna + PyTorch）：

| 维度 | 取值 |
|---|---|
| 信道模型 | 3GPP TDL-A..E + CDL-A/B/C（训练含多种；留 CDL-D/E 做失配测试） |
| SCS / 网格 | 15/30 kHz；25 / 51 / 106 / 273 PRB（训练至少 51 PRB 全量 + 273 PRB 部分） |
| DMRS | PUSCH type 1（6 RE/PRB）与 type 2（4 RE/PRB）；1 与 2 个 DMRS 符号 |
| 端口/层 | 1–2 层 × 1–4 RX 端口（按端口独立实例合成；如需空间相关可加 RX 相关矩阵） |
| SNR | −5..25 dB（重点 0–20 dB 部署区；混合 SNR 训练避免 per-SNR 网络） |
| Doppler / 时延扩展 | 0–400 Hz（30 kHz 归一化）；30–1000 ns |
| 规模 | 训练 ≥ 100k 实现；验证/测试各 15%（跨模型划分，测试含未训练过的 TDL/CDL） |
| 标签 | 真值信道网格（仿真器可获取） |

要点：

- **只改"DMRS 图案 + 信道分布"，不改 HELENA 架构**：重训后权重覆盖 PUSCH type1/2；
  多端口=批处理维度（或每端口独立），层数=额外通道/独立实例，二选一实测定。
- 归一化层固定进图（训练与推理同式：按导频 |LS| 均值归一），消除增益漂移问题。
- fp16 量化感知训练（QAT 或训练后 PTQ + 校准），保证 MPS fp16 推理与训练一致。
- 评估指标：NMSE（vs 真值）、**vs L0 实用 MMSE 的 ΔNMSE**、端到端 BLER/吞吐
  （离线仿真链：CE → 均衡 → 解调 → LDPC，复用 ocudu 现有测试设施）。

### 7.2 训练资源与流程

- 单 GPU（Apple MPS 或云 NVIDIA）均可；77 MFLOPs 模型训练成本极低（单卡数小时/轮）。
- 流程：数据合成 → 预训练（HELENA 权重做初始化，TDL-A..E）→ PUSCH 图案微调 →
  泛化测试（失配 TDL/CDL、Doppler/时延扩展外推）→ PTQ/QAT → 导出 ONNX → coremltools/MPSGraph。
- 权重资产管理：版本号 + 校验和 + 训练配置快照进仓库 LFS 或独立资产库；
  回滚机制（A/B 开关 + 上一版权重常驻）。

---

## 8. 性能与时延预算分析

### 8.1 计算量（NN 前向，fp16）

| 配置 | HELENA MACs/端口 | 批处理（4 端口×2 层） | 预算评估 |
|---|---|---|---|
| 51 PRB | ~43 MMAC | ~0.35 GMAC | 可行（dispatch 主导，需批处理） |
| 273 PRB | ~262 MMAC | ~2.1 GMAC | 风险区（注意力 N²；需 G1 实测，必要时切 L2/经典） |

DMRS type 1/2、1–2 符号只改变稀疏输入的非零位置，不改变计算量（输出网格同尺寸）。

### 8.2 时延构成（MPS，估算待 G1 实测）

- 固定开销：一次 MPSGraph encode+commit ≈ 10–40 µs（LDPC 实测 290 dispatch ≈ 35 µs 佐证
  单命令缓冲开销量级）；打包/归一化/cbf16 转换（CPU NEON）≈ 5–20 µs；
- 计算：51 PRB 单端口 ~20–30 µs、批处理 4–8 模型 ~50–80 µs → **合计 ~60–150 µs，落预算**；
- 273 PRB 批处理 ~0.5–1 ms → **超预算**（该工况首期保持 L0 经典 MMSE，见上线策略）。

### 8.3 与总预算的关系

- 实测整条 UL 管线中位 207–311 µs，前端 ~190 µs；HELENA 即使按悲观 150 µs 也只占
  前端预算内可接受比例，且 GPU 与 CPU 可并行（LS/噪声方差在 CPU、NN 在 GPU），
  理论上还有流水线重叠空间（LDPC 已证明 GPU 腿与 CPU 腿延迟平齐）。

---

## 9. 验证与 A/B 上线方案

### 9.0 在线自适应训练（per-site fine-tuning，2026-08-30 立项）

**动机**：离线合成集（TDL-C/D/E 等"规整"模型）上的结果只是一般性参考；真实传播环境
因站点而异（室内/室外/城区/高速），且每个 gNB 的 UE 群体有其行为特征（用户主要活跃在
家/学校/办公室范围）。**每个 gNB 应该拥有适配自身环境的专属权重**——白天通信、夜间
训练，模型随站点生长。这是 AI-CE 相对经典 MMSE（固定物理假设：指数 PDP + Jakes）的
本质优势。

**架构（三层闭环）**：

1. **白天：数据收集（gNB 内，零额外硬件）**。训练标签不需要真值信道——用
   **决策导向（decision-directed）信道样本**：crc=OK 的 slot → 数据符号经
   重编码得到无噪发射符号 x → 数据 RE 处 ĥ = rx/x（等效高 SNR 信道样本），
   与 DMRS RE 的 LS 样本合并成"准真值"标签网格。输入 = 插值 LS 网格（与推理
   同格式）。收集开关 + 上限（如每夜 ≤ 2 万槽）→ 落 **flash**（站点数据目录）。
2. **夜间/低负载：训练 sidecar 服务**（独立进程，复用 `ai_train/` 管线：
   TF → coremltools → .mlmodelc）。数据混合 = 站点数据 + 一定比例离线预训练
   数据（**防灾难性遗忘/分布坍缩**）。M4 Pro CPU 训练成本已实测（40k 样本
   ~20 min/10 epoch），夜间数小时绰绰有余。
3. **上线：A/B 门禁 + 原子热替换**。新权重先进入**阴影模式**（与现任权重同槽
   并行推理，只比 NMSE 不上线）；在验证缓冲上达标（≥ 现任权重 N dB）→
   原子替换模型文件 → gNB 引擎**热重载**（重新 init，毫秒级）；不达标保留
   旧权重，训练数据留档下轮再训。

**对集成的设计要求（现在就预留）**：
- `port_channel_estimator_helena_impl`：模型**热重载 API**（reload(path)）+ 数据
  收集钩子（crc-OK 槽的输入/标签网格按需落盘）+ 阴影模式开关；
- 权重资产版本化（版本号 + 校验和 + 训练配置快照，随模型文件管理）；
- 回退语义不变：任何时刻回退到经典 MMSE（L0）。

**与异构规划的联系**（`docs/apple_silicon_heterogeneous_gnb_plan.md`）：这是
"算力异构 × 存储异构"的教科书式协同——白天热数据走 ANE 推理、夜间冷数据驻 flash、
训练用空闲 CPU——同一台 gNB 的算力与存储按时间维度复用。

### 9.1 验收门禁（Gate）

- **G1 延迟原型**：MPS 单网格前向 p99 ≤ 100 µs（51 PRB）；4 端口批处理 p99 ≤ 150 µs；
  273 PRB 数据作为扩展决策依据。不达标 → 评估 Core ML/ANE 或降级 L2。
- **G2 精度**：重训后 NMSE ≥ L0 实用 MMSE 于全部部署 SNR（0–20 dB），低 SNR 有 ≥1 dB
  增益；端到端 BLER ≤ 经典路径（同均衡器/解调器）。
- **G3 泛化**：未训练 TDL/CDL 模型、Doppler 0–400 Hz、时延扩展 30–1000 ns 外推测试通过。
- **G4 鲁棒**：阴影模式 24 h 浸泡无 NaN/崩溃；自动回退正确触发；时延 p99 稳定。
- **G5 端到端**：ZMQ 与 RF 实链（B200）与经典腿 A/B：ping RTT、iperf3 UL、BLER 曲线
  三腿同噪声带对比（沿用 LDPC Metal 的方法论）。

### 9.2 上线策略

1. 默认 `average`（或升级后的 L0 `mmse`）不变；`helena` 仅经
   `expert_phy --pusch_channel_estimator_algorithm helena` 开启。
2. 阴影模式（双估计器并行上报）跑 ≥1 周实链 → 数据评审。
3. 灰度：先 51 PRB/≤2 端口/30 kHz 配置默认启用；273 PRB/4 端口维持 L0。
4. 运行时护栏（每槽）：NN 延迟超时、NaN/幅值越界、与经典估计 NMSE 滑窗对比劣化 →
   自动回退 L0 并告警；权重缺失/MPS 不可用 → 启动即回退。

---

## 10. 里程碑与工作量

| 里程碑 | 内容 | 产出 | 工期（2 人：PHY C++/Metal + ML） |
|---|---|---|---|
| M0 | L0 实用 MMSE 基线（PDP/Wiener 平滑）+ 单测 + BLER 对比 | `mmse` 策略合入 | 1–2 周 |
| M1 | HELENA 权重解析 + MPSGraph 移植 + 延迟原型 | **G1 决策数据** | 2–3 周 |
| M2 | PUSCH 训练数据管线 + HELENA 重训（type1/2、多端口批处理、QAT） | 训练产出 + 权重资产 | 3–4 周 |
| M3 | `port_channel_estimator_helena_impl` + 工厂/配置/探针/装饰器 | 代码合入 + 单测 | 3–4 周 |
| M4 | 离线验收（G2/G3）+ 阴影模式 A/B（G4） | 验收报告 | 2–3 周 |
| M5 | 实链 E2E 三腿对比 + 灰度上线（G5） | 默认启用（51 PRB 级） | 2–4 周 |

合计约 **14–20 周**（关键路径：G1 延迟原型 ← M1，最早可砍；若 G1 失败走 L2 备选，
M2–M5 工作量相当但训练更简单）。

---

## 11. 风险与缓解

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| 1 | MPS 延迟不达标（dispatch 主导小模型） | G1 失败 | 端口批处理、图融合、Core ML/ANE 备选、L2 降级 |
| 2 | 273 PRB × 4 端口超预算 | 100 MHz 不可用 | 该工况维持 L0 经典 MMSE；跟踪线性注意力变体 |
| 3 | 下行 SISO → 上行 PUSCH 多端口/多层域迁移失败 | 精度不达标 | M2 重训矩阵全覆盖；L2 备选；A/B 门禁不放行 |
| 4 | 训练/部署失配导致线上劣化 | 性能回退 | 失配测试集（G3）+ 运行时自动回退护栏 |
| 5 | fp16/cbf16 量化损失（低 SNR） | 精度损失 | 残差加/归一化保持 fp32；QAT；G2 量化验收 |
| 6 | HELENA 仓库无训练脚本 | 复现成本 | 架构已从 .keras config 全量还原，超参已知；自建训练管线 |
| 7 | MPS 与 LDPC 解码器共享 GPU 的抖动 | p99 时延 | 独立命令队列/交错调度实测；探针监控 |
| 8 | 许可/权重资产 | 合规 | HELENA MIT；重训权重自有；CNN4CE GPL 仅作思想参考不拷贝代码 |
| 9 | 论文无直接"≥MMSE"数据 | 验收依据不足 | L0 实用 MMSE 基线（M0）+ A/B 门禁补上该证据链 |

---

## 12. 结论

在 147 篇论文中，**HELENA** 是唯一同时满足"输入与 NR DMRS-LS 接口一致、参数/FLOPs 极小、
对 srsRAN 实用估计器有 +4.5 dB NMSE 增益、开源权重与模型齐备"的候选，推荐作为主选算法；
其两大风险（Apple MPS 实际时延、PUSCH 域迁移）分别用 **G1 延迟原型**与 **PUSCH 重训矩阵**
前置化解。CNN4CE 思想的 DMRS-only CNN 去噪器作为在预算内必然可跑的降级备选。
同时建议先落地 **L0 实用 MMSE 基线**，使"不低于 MMSE"成为可执行、可测量的验收标准，
并以仓库既有的 Metal LDPC 范式 + 实链探针 A/B 文化完成集成与灰度上线。
总体工期约 14–20 周（PHY C++/Metal + ML 两人）。

---

## 附录 A：高分论文明细（score ≥ 4，扫描结论摘录）

| 文件 | 分数 | 一句话结论 |
|---|---|---|
| 2506.13408v2 HELENA | 8 | 主选；LS 稀疏输入→全网格，0.116M/77 MFLOPs，vs srsRAN 实用 +4.5 dB，MIT 权重 |
| Deep CNN-based channel estimation for mmWave massive MIMO systems | 6 | 优于非理想 MMSE；备选 DMRS-only 变体；GPL 无权重 |
| Machine learning-based 5G-and-beyond channel estimation for MIMO-OFDM | 6 | TDL-C+NR DMRS type1；低 SNR 优于 LMMSE；无代码 |
| 2505.09076v1 AdaFortiTran | 5 | 跟踪 LMMSE；注意力超预算；MIT 无权重 |
| Deep learning-aided 5G channel estimation | 5 | 小 FC 网络低 SNR 优于 LMMSE、高 SNR 退化；无代码 |
| Deep learning-based channel estimation for doubly selective fading channels | 5 | 优于 BEM-LMMSE；Doppler 失配鲁棒；无时延数据 |
| Wideband channel estimation with a generative adversarial network | 5 | 低 SNR 增益大；迭代推理不可实时 |
| A Study on MIMO Channel Estimation by 2D and 3D CNN | 4 | 5G-NR 8×8 数据集；0.47–2.3 GFLOP 偏重 |
| Deep learning based channel estimation schemes for IEEE 802.11p | 4 | LS 之上 DNN 去噪；复杂度 −55.7%；无代码 |
| Deep learning for beamspace channel estimation（GM-LAMP） | 4 | 展开 AMP 低复杂度；稀疏波束域不匹配 NR 密集网格 |
| Deep learning for channel estimation Interpretation performance and comparison | 4 | 理论：DL→MMSE 渐近等价；失配警告 |
| Deep learning for joint channel estimation and signal detection（CENet） | 4 | 学习插值优于 LS/MMSE+插值；SAN 重、无复杂度数据 |
| Deep learning-based joint pilot design and CE for multiuser MIMO | 4 | 优于 LMMSE 方案；需非正交导频（与 NR 冲突） |
| Multi-Task Transformer Receiver | 4 | ICL 少导频优于 LS/LMMSE；结构不匹配 NR |
| Power of deep learning for CE and signal detection in OFDM | 4 | 奠基；端到端输出比特而非网格；无复杂度数据 |
| Towards DL-aided wireless CE and CSI feedback for 6G | 4 | 案例 DL 时域 CE 优于 LS/LMMSE；方法论参考 |

## 附录 B：关键 GitHub 仓库

| 仓库 | 内容 | 许可 | 用途 |
|---|---|---|---|
| miguelhdo/HELENA_Channel_Estimation | 权重(.keras/ONNX)+评估+数据集链接 | MIT | **主选** |
| back2yes/CNN4CE | TF1 训练/测试脚本（无权重） | GPL-3.0 | 思想参考（DMRS-only 变体） |
| BerkIGuler/AdaFortiTran | PyTorch 训练/评估（无权重） | MIT | 跟踪 |
| utcsilab/score-based-channels | RefineNet+Langevin（A100 研究码） | — | 研究跟踪 |
| Mehran-Soltani/ChannelNet、DeepPilotDesign | SRCNN/DnCNN 式 CE | — | 基线参考（仅与 MMSE 相当） |

## 附录 C：主要参考文献

1. M. Camelo Botero et al., "HELENA: High-Efficiency Learning-based channel Estimation using
   dual Neural Attention," arXiv:2506.13408v2, 2026.（主选）
2. P. Dong, H. Zhang, G. Y. Li et al., "Deep CNN-Based Channel Estimation for mmWave Massive
   MIMO Systems," IEEE JSTSP, 13(5), 2019.（备选思想）
3. H. A. Le et al., "Machine Learning-Based 5G-and-Beyond Channel Estimation for MIMO-OFDM
   Communication Systems," Sensors 21, 4861, 2021.（NR DMRS type1 证据）
4. B. Guler, H. Jafarkhani, "AdaFortiTran: An Adaptive Transformer Model for Robust OFDM
   Channel Estimation," arXiv:2505.09076, 2025.（否决记录）
5. K. Kou et al., "Multi-Task Transformer Receiver for OFDM Channel Estimation and Symbol
   Detection," NeurIPS 2025 AI4NextG Workshop.（否决记录）
6. E. Balevi, J. G. Andrews, "Wideband Channel Estimation With a Generative Adversarial
   Network," IEEE TWC, 2020；M. Arvinte, J. Tamir, "MIMO Channel Estimation using Score-Based
   Generative Models," IEEE TWC, 2023.（生成式家族，否决记录）
7. "Deep Learning for Channel Estimation: Interpretation, Performance, and Comparison"（理论依据）
8. "A Survey of Artificial Intelligence Enabled Channel Estimation Methods"（综述背景）
9. "A Survey on Deep Learning Based Channel Estimation in Doubly Dispersive Environments"（含复杂度分析+开源代码）
10. OCUDU 仓库内 `lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`（Metal 集成范式与实测时延锚点）
11. J.-J. van de Beek, O. Edfors, M. Sandell et al., "On Channel Estimation in OFDM Systems,"
    IEEE VTC 1995（MMSE/维纳滤波基线的经典依据，已在文献目录中）
