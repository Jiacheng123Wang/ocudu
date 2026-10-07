# 文献调研（interim）

> ⏳ **本备忘是中途版本**：调研子代理正在 4 条线上并行推进（已完成 2 条，其余进行中）。
> 已核实且**会影响规划**的信息先落盘，避免会话中断丢失；完整版将替换 §3。
>
> 同时记录一条**工具限制**：本沙箱封锁了几乎全部 `web_fetch`（arxiv.org / IEEE / Semantic Scholar
> 均不可达，全部返回 "resolves to a non-public IP address"），只有 `web_search` 可用。
> 因此本备忘中的外部数字**只能复核到"存在性与指标定义"层面**，凡未能拉取原文的一律标注【未核实原文】。
>
> 版本：v0.9（interim）｜ 日期：2026-10-07

---

## 1. ★ 最相关的前作：OCUDU dApp 平台上的 neural receiver → LLR（**有实测**）

这一条比参考论文（DEFINED）**更贴近我们的目标**：它已经是"在真实 5G gNB 里内联跑神经接收机、
直接输出 LLR"，而且给出了**实测的接口时延与槽占用预算**。

| 来源 | 关键实测/内容 | 对我们的意义 |
|---|---|---|
| *Real-Time dApps for AI-RAN: Measured Interface Requirements for Inline PHY and Slot-Level Control*（arXiv **2609.07805**, 2026-09）[链接](https://arxiv.org/pdf/2609.07805v1.pdf) | neural-receiver-to-LLR dApp：**每槽输入 1.47 MB / 输出最多 4.4 MB**，占用 **≤ 500 µs** | ★ **这就是 G-A 目标的现成对标基线**：500 µs 占用 vs 我们 eqdem 的 **667.4 µs**（mkf033 中位） |
| *The OCUDU dApp Platform: An Open Runtime and E3 Interface for Real-Time AI-RAN*（arXiv **2609.07843**, 2026-09）[链接](https://arxiv.org/pdf/2609.07843v1.pdf) | 接口时延实测：**直接调用 0.29 µs P99.9（静默）/ 5.2 µs（小区在空口上）** | 走 dApp 路线时，**接口本身**是微秒级，可不计；真正的代价是数据搬运 |
| DeepSig：[Opening the Fastest Part of the 5G Radio to AI: The OCUDU dApp Platform](https://www.deepsig.ai/ocudu-dapp-platform/) | 厂商视角 | 生态背景 |
| [AI-RAN with OCUDU and GNU Radio](https://events.gnuradio.org/event/28/contributions/856/contribution.pdf) | 实践报告 | 生态背景 |
| CNS：[RA-NN on OCUDU 案例研究](https://ocudu.org/wp-content/uploads/sites/32/2026/08/OCUDU_CNS_Case-Study_8.7.26.pdf) | 案例研究 | 生态背景 |

### 1.1 这条前作带来的两个判断

**判断一：接口与预算这一层不必重新发明。** "每槽多少字节、多少 µs 占用"的记账方式已经有人做过并测过，
我们应当**直接复用它的口径**作为对标，而不是自己另立一套（本仓库吃过"自创口径导致数字不可比"的亏）。

**判断二：Apple Silicon 上的硬件实测实现仍是空白。** 这与仓库既有
`apple_silicon_competitiveness_analysis.md` 的判断一致——**硬件能力已经商品化，稀缺的是软件可用性**。
本工作流的差异点正是"在 CoreML/ANE + Metal 上把它真正跑起来并测量"，而不是"再提出一个接口"。

### 1.2 ★ 由此产生一个规划层面的新选择（已写入主规划 §1.2）

| 路线 | 做法 | 代价 | 风险 |
|---|---|---|---|
| **I 内联** | 走 `channel_equalizer::submit_fused()`，AI detector 是一个后端 | 自行处理 GPU↔ANE 同步 | 打破"一次提交"结构（主规划 R6） |
| **D dApp** | 走 OCUDU dApp / E3 接口 | 每槽 **1.47 MB 进 / ≤4.4 MB 出**的搬运 | ★ **与融合 lane 的核心成就正面冲突** |

★ 路线 D 的关键疑点：本仓库 `phy_pipeline_gpu` 的核心成就是**零主机↔设备数据穿越**
（`phy_pipeline_crossings.h` 的整个设计目的）。dApp 路线等于**把这笔穿越加回来**——
在统一内存架构上这是一个**需要论证**而不是默认接受的选择。
⇒ 规划默认走**路线 I**，路线 D 作为对照与"如果内联做不到"的退路。

---

## 2. DEFINED（arXiv 2503.16594）的精确刻画（更正主规划的前提）

子代理核实后，有三处比"论文精读备忘"更精确，**已回写 `memo_02`**：

1. **版本与标题**：v2 = *Transformer-based Wireless Symbol Detection Over Fading Channels*；
   v1 = *Decision Feedback In-Context Learning for Wireless Symbol Detection*；
   前身 = IEEE **ICC 2025** *Decision Feedback In-Context Symbol Detection over Block-Fading
   Channels*（Xplore 11161684）。作者 Fan Li / Yang / Shen（UVA），代码
   [github.com/ShenGroup/DEFINED](https://github.com/ShenGroup/DEFINED)。**引用必须写清版本**。
2. **指标定义**：`gain_DF = (SER_k − SER_{T−1}) / SER_k × 100%`——相对"同样 k 个导频的经典
   MMSE 基线（MMSE-P_k）"的**相对 SER 下降**。⇒ 对比对象确实是经典 MMSE，但"收益"是相对百分比，
   引用时不能写成绝对 SER。
3. **信道分布错配**：论文**显式测试了 Rayleigh(NLOS) 训练 → Rician(LOS) 测试**的场景。
   ★ 这正是 AI CE 失败的那个机制。⇒ 成为主规划 **R5 的对策候选**：即使最终不采用 Transformer，
   "把适配写进输入格式"这一思路也值得借鉴。

---

## 3. 待补（子代理进行中）

- **DeepRx（Nokia）** 与 **Sionna neural receiver** 的具体架构与 LLR 训练方式（BCE on bits）
- LLR **校准 / 量化部署**的文献（训练后标度、温度缩放、量化感知训练）
- 已发表的**复杂度/时延**数字，与我们的 ANE 锚点对照
- 与我们的差异点：**SISO / 25–51 PRB 宽分配 / 3 个 DM-RS 符号 / 15–25 dB** 的 OFDM PUSCH 上，
  是否有人报告过实测收益（若没有，这本身就是一个可发表的空白）

> 完整版将包含 7 条线的全部结论、引用与综合。本文件届时整体替换。
