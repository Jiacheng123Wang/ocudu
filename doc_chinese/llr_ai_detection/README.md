# `doc_chinese/llr_ai_detection/` — AI LLR Detection 工作流

> **工作流目标**：把 PUSCH 接收链的"均衡 + 解映射"（实测中位 **667.4 µs**）换成一次神经前向，
> **直接输出加扰域 LLR**，交给现有 LDPC 译码器。
>
> 前身工作流：`metal_kernel_fusion`（GPU 融合 lane，已收官）。本工作流继承它的问题陈述，
> 并换一条完全不同的路去解决它：**不动 GPU 内核，换执行引擎（ANE）**。

## 阅读顺序

| # | 文档 | 回答什么问题 | 状态 |
|---|---|---|---|
| 0 | **`AI_LLR_detection_master_plan.md`** | **主规划**：目标、接口契约、模型候选、训练、阶段门禁 G0–G6、风险、交付物 | ✅ v1.0 |
| 1 | `memo_01_repo_seams_and_llr_contract.md` | 模型**接到哪里**（`submit_fused` 接缝）、输出**必须满足什么契约**（LLR 量化 / 解扰域）、后端开关先例 | ✅ v1.0 |
| 2 | `memo_02_paper_2503.16594_reading.md` | 参考论文 DEFINED 的精读、它对我们工作的三处必须改动、以及那个必须先回答的质疑 | ✅ v1.0 |
| 3 | `memo_03_literature_survey.md` | 公开文献调研：**OCUDU dApp 上 neural-receiver→LLR 的实测前作**（≤500 µs 槽占用基线）、内联 vs dApp 路线、DEFINED 的精确刻画 | 🟡 v0.9 中期（完整版待补） |
| 4 | `memo_04_data_labels_and_operating_point.md` | **我们的工作点在哪**（实测）、监督信号从哪来、P0 前置实验、语料缺口 | ✅ v1.0 |

## 三句话结论（先读这个）

1. **接缝已经存在**：`channel_equalizer::submit_fused()` 就是 AI detector 的宿主——它的语义
   恰好是"用一次前向替换 `equalize + demodulate_soft`"，而且它那条"谓词只能是 shape 的属性、
   不能看数值"的契约，神经网络天然满足（memo 01 §1）。
2. **输出必须是加扰域 LLR**，解扰保持经典；量化由既有的 `log_likelihood_ratio::quantize()`
   一处完成，但**标度校准是独立步骤**，做不好会让 LDPC 直接崩掉（memo 01 §2、§3）。
3. ★ **"AI detector 能提高精度"是待证伪的假设，不是前提**：实测我们的工作点是
   **10.7% 导频密度 / 15–25 dB / 宽分配 / SISO**，而参考论文报告最大收益的区间是**极少导频**。
   规划里的 **P0** 就是为裁决这件事设计的——**在写模型代码之前完成**（memo 04 §4）。

## 本目录的写作纪律

- 数字分等级：【实测】（本仓库跑出来的，可追溯到一次运行）、【读全文】/【检索】（外部来源）、
  【估算】（推算，不得当作实测引用）。
- 每个结论给 `文件:行号` 或数据来源，便于复核。
- 判据在实验之前写死；每个数字可追溯到一次 leg / commit（继承 `metal_kernel_fusion` 的纪律）。
