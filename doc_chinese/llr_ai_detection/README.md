# `doc_chinese/llr_ai_detection/` — AI LLR Detection 工作流

> **工作流目标**：把 PUSCH 接收链的"**信道估计 + 均衡 + 解映射**"换成一次神经前向，
> 输入是 DFT 后的时频网格**及其几何**（哪些是 DM-RS RE、哪些是 data RE、二者的相对关系），
> 输出是**加扰域 LLR**，交给现有 LDPC 译码器。
> （实测：`ce` **73.3 µs** + `eqdem` **667.4 µs** 两段；设备侧融合单元 `merged_hop` 中位 **449.8 µs**。）
>
> 前身工作流：`metal_kernel_fusion`（GPU 融合 lane，已收官）。本工作流继承它的问题陈述，
> 换一条完全不同的路去解决它：**不重写链路结构，把这段计算换成学习型的一次前向**。

## 阅读顺序

| # | 文档 | 回答什么问题 | 状态 |
|---|---|---|---|
| 0 | **`AI_LLR_detection_master_plan.md`** | **主规划**：目标、接口契约、引擎选择、模型候选、训练、阶段门禁 G0–G6、风险 R1–R9、交付物 | ✅ v1.2 |
| 1 | `memo_01_repo_seams_and_llr_contract.md` | 模型**接到哪里**（`submit_fused` 接缝）、输出**必须满足什么契约**（LLR 量化 / 解扰域）、后端开关先例 | ✅ v1.0 |
| 2 | `memo_02_paper_2503.16594_reading.md` | 参考论文 DEFINED 的精读、它对我们工作的三处必须改动、以及那个必须先回答的质疑 | ✅ v1.1 |
| 3 | `memo_03_literature_survey.md` | 公开文献调研：**ANE 不能做逐符号引擎**、内联 vs dApp、LLR 校准与回滚、本工作流的空白 | ✅ v1.0 |
| 4 | `memo_04_data_labels_and_operating_point.md` | **我们的工作点在哪**（实测）、监督信号从哪来、P0 前置实验、语料缺口 | ✅ v1.0 |
| — | `survey/` | **英文原始调研材料**（11 份，带逐条引用与来源等级），`memo_03` 是它们的中文综合 | ✅ |
| 5 | `memo_05_reference_repo_survey.md` | **参考 repo 代码调研**：NVIDIA `aerial-cuda-accelerated-ran` 里的两个训练好的模型、完整的 ML 设计流程、cuPHY 的 LLR 契约、E3/data lake；以及**哪些可重用 / 需修改**与可移植性分析 | ✅ v0.9 |
| 6 | `memo_06_ocudu_dapp_platform_survey.md` | **OCUDU dApp 平台代码调研**：**深度 3 的正规契约**（`receiver.h`）、平台里的接缝实现（`pusch_dapp_demodulator.h`）、SDK 的"稳定壳/可替换核心"、官方已知限制 | ✅ v0.9 |
| — | `ref_paper/` | **参考论文原文 PDF（38 篇）**，文件名 = 论文标题；`README.md` 是按用途分组的索引 | ✅ |

## 六句话结论（先读这个）

1. ★★ **一个网络,深度 3,从一开始就这样做**。"深度 3"= 单个联合网络同时做
   **信道估计 + 均衡 + 解映射**:输入是网格**及其几何**(DM-RS 位置张量、data-RE 索引、PUSCH 元数据),
   一个共享 trunk 出多路头(主:**逐 RE 逐比特 LLR**;辅:后均衡噪声方差 + 信道估计与噪声)。
   **不是** CE 网络 + EQ/DEM 网络两个级联——中间那层信道估计会成为有损瓶颈、断掉端到端梯度,
   而且每多一个网络就多付一次 dispatch 地板(mkf023 一个边界 ≈6.5 µs;ANE 单次 dispatch 地板 70–230 µs)。
   文献同形:DeepRx 是**单个**全卷积网络,Sionna 的 neural receiver 也是**一个**网络替换三步。
   我们**已经有 AI CE 模块**,不存在"先做浅一层"的必要(memo 01 §1.4–§1.6,规划 §2.1)。
2. ★ **接缝要选对**:OCUDU dApp 把内联替换分成三个深度——
   ①仅 CE(100 µs)②CE+均衡③**CE+均衡+解映射**(150 µs)。**我们的目标 = 深度 3**,
   OCUDU dApp 把它列为 **depth 3**(预算 **150 µs**),并给出一句关键判断:
   *"no hook replaces equalization alone"*。我早先认定的 `channel_equalizer::submit_fused()`
   **只是它的子集**——它吃的是**已经算好的信道估计**。
   ⇒ 深度 3 的宿主是 `estimator.estimate()` + `demodulator.demodulate()` **两处的合并**;
   好消息是设备侧 `merged_hop` **已经**是深度 3 的形状(memo 01 §1.4–§1.6)。
   ★ 代价:深度 3 **继承 CE 的上报义务**(RSRP/EPRE/噪声/SNR/TA/CFO → CSI)——规划选定
   "net 附带输出信道估计与噪声,经典测量核跑在它上面"(memo 01 §1.5)。
3. **输出必须是加扰域 LLR**，解扰保持经典；量化由既有的 `log_likelihood_ratio::quantize()` 一处完成，
   但**标度校准是独立步骤**（scaling + clipping 在文献里是标准做法），做不好会让 LDPC 直接崩掉
   （memo 01 §2、§3；memo 03 §4.2）。
4. ★ **"AI detector 能提高精度"是待证伪的假设，不是前提**：实测我们的工作点是
   **10.7% 导频密度 / 15–25 dB / 宽分配 / SISO**，而参考论文报告最大收益的区间是**极少导频**。
   规划里的 **P0** 就是为裁决这件事设计的——**在写模型代码之前完成**（memo 04 §4）。
5. ★ **引擎：默认 ANE**（AI 推理专用引擎,与仓库原则"AI 推理走 NPU"一致）。
   本机 **M4 Pro** 实测:HELENA 在 **ANE 上 p50 141 µs / p99 208**,而同机 **MPS/GPU 路径是 690 µs**
   ——**ANE 快 4.9×**。★ 早先"ANE 不能做逐符号引擎、Metal 升主路径"的结论**已撤销**:
   它把调用粒度搞错了(是**每跳 1 次** dispatch,不是每符号 14 次——`merged_hop` 一个 CB 覆盖整跳),
   又拿 **M1** 的数字给 M4 Pro 定价。ANE 的真实约束是设计约束(自定义 Metal kernel 不能进 ANE 图、
   fp16 量程、工作集悬崖、`EnumeratedShapes` 分桶),不是"所以别用"。
   Metal/MPS 保留为**对照臂**,由 P4/P5 实测裁决(memo 03 §1,主规划 §2.6)。
6. ★ **内联是唯一路线**：OCUDU dApp 运行时只声明 CPU/CUDA，没有 Metal/ANE/CoreML 后端
   （memo 03 §2）。这反而避免了把每槽 1.47 MB 进 / 4.4 MB 出的搬运重新加回一条以
   "零主机↔设备数据穿越"为核心成就的 lane。

★ **一个诚实的定位**（memo 03 §6）：文献已经有仿真里能工作的 Transformer/ICL 检测器、
有与译码器通过外信息 LLR 对话的深度展开检测器、有唯一的开放 5G 栈内联 PHY 时延实测。
**不存在**的是——硬件实测的、实时的、合规的 grid→LLR 模型，跑在 Apple Silicon 上、在空口 gNB 里、
带真实 LDPC 译码器在环、并给出诚实的**校准与错误平台**数字。
**贡献在于这个组合，而不是模型架构本身。**

## 本目录的写作纪律

- 数字分等级：【实测】（本仓库跑出来的，可追溯到一次运行）、【已核实片段】/【读全文】/【检索】
  （外部来源）、【未核实原文】/【估算】（**引用前必须复核**，不得当作实测引用）。
- 每个结论给 `文件:行号` 或数据来源，便于复核。
- 判据在实验之前写死；每个数字可追溯到一次 leg / commit（继承 `metal_kernel_fusion` 的纪律）。
