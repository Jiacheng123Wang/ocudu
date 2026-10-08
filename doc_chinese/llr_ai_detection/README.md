# `doc_chinese/llr_ai_detection/` — AI LLR Detection 工作流

> **工作流目标**：把 PUSCH 接收链的"**信道估计 + 均衡 + 解映射**"换成一次神经前向，
> 输入是 DFT 后的时频网格**及其几何**（哪些是 DM-RS RE、哪些是 data RE、二者的相对关系），
> 输出是**加扰域 LLR**，交给现有 LDPC 译码器。
> （实测：`ce` **73.3 µs** + `eqdem` **667.4 µs** 两段；设备侧融合单元 `merged_hop` 中位 **449.8 µs**。）
>
> 前身工作流：`metal_kernel_fusion`（GPU 融合 lane，已收官）。本工作流继承它的问题陈述，
> 换一条完全不同的路去解决它：**不重写链路结构，把这段计算换成学习型的一次前向**。

## 阅读顺序与**文档编号总表**

★ **编号是连续的；"已落盘"与"计划产出"分列，避免"引用了但找不到"的困惑。**

| # | 文档 | 内容 | 状态 |
|---|---|---|---|
| 0 | **`AI_LLR_detection_master_plan.md`** | **主规划**：目标、设计边界、接口契约、模型、训练、门禁 G0–G6、风险、**§9 开工前功课** | ✅ v2.1 |
| 01 | `memo_01_repo_seams_and_llr_contract.md` | 仓库接缝与 LLR 契约（含 §1.4–§1.6 **两种深度**） | ✅ v1.2 |
| 02 | `memo_02_paper_2503.16594_reading.md` | 论文 DEFINED 精读 | ✅ v1.2 |
| 03 | `memo_03_literature_survey.md` | 公开文献调研（7 条线 + 勘误 + 决策级结论） | ✅ v1.0 |
| 04 | `memo_04_data_labels_and_operating_point.md` | 数据、标签与**工作点实测** | ✅ v1.1 |
| 05 | `memo_05_reference_repo_survey.md` | **NVIDIA `aerial-cuda-accelerated-ran`** 代码调研 | ✅ v0.9 |
| 06 | `memo_06_ocudu_dapp_platform_survey.md` | **OCUDU dApp 平台**代码调研（契约、接缝、fence、生命周期、可移植性） | ✅ v0.9 |
| **07** | `memo_07_reporting_obligations.md` | ★ **上报义务清单**（A1 的产出）：`get_channel_state_information` 的每个消费字段被谁读、精度、是否影响后续槽 | 🟡 **占位大纲已落盘**，内容待填 |
| **08** | `memo_08_p0_design.md` | ★ **P0 实验设计与判据预登记**（A3 的产出）：genie 上界、经典链基线、BLER 操作点与样本量 | 🟡 **占位大纲已落盘**，内容待填 |
| 09 | `memo_09_corpus_design.md` | ★★ **语料设计与采集方案**（三层结构、不可恢复字段、覆盖度账本、飞行绑定） | ✅ v1.0 |

> ★ **关于 07/08**：它们是 **§9.2 功课 A1/A3 的产出物**——
> **先有计划、再有文档**。现已落盘**占位大纲**（把要回答的**问题**先写清楚），
> 但**内容未填**；文件头明确标着 `★ 占位大纲，尚未开始`。
> 看到"状态：✅ 完成"之前，引用 `memo_07`/`memo_08` 都应读作"该功课的产出将写在这里"。

### 辅助目录

| 目录 | 内容 |
|---|---|
| `survey/` | ★ **英文原始调研材料**（11 份，带逐条引用与来源等级）；`memo_03` 是它们的中文综合 |
| `ref_paper/` | **参考论文原文 PDF（38 篇）**，文件名 = 论文标题；`README.md` 是按用途分组的索引。**PDF 不进 git** |
| `wip/` | ★ **进 git 的工具与约定**：`README.md`（目录地图、**leg 命名**、**历史工具出处**）、`next_leg_label.sh`、**`A6_capture_fields_plan.md`**（A6 实现方案） |
| `wip/logs/` | ★ **新飞腿的日志**（**不进 git**，由 `**/logs/` 忽略——已核实） |
| `work_tmp/` | ★ **临时工作产物**（语料、参考二进制；**不进 git**，由 `**/work_tmp/` 忽略——已核实） |

### ★★ leg 命名：`aillr0NN-<suffix>`

```bash
bash doc_chinese/llr_ai_detection/wip/next_leg_label.sh            # → 001
bash doc_chinese/llr_ai_detection/wip/next_leg_label.sh baseline   # → aillr001-baseline
```

`mkf` 是**上一个工作流**（metal kernel fusion）的缩写；**本工作流从 `aillr001` 重新开始**，
序号由工具从 leg 日志算出，**不靠记忆**。详见 `wip/README.md` §2。

## ★ 当前状态（2026-10-08）

**调研阶段，未开工。** 用户裁定：**先把功课做足**。

- 已完成 6 份 memo（仓库接缝 / 论文 / 文献 / 数据 / NVIDIA aerial / OCUDU dApp 平台）——
  见主规划 §9.1；
- ★ **开工前功课清单**见主规划 **§9.2**（必做 A1–A4、应做 B1–B8、可选 C1–C4）；
- ★ **开工判据（Definition of Ready）**见主规划 **§9.3**——七条全部为真才开始写模型代码；
- ★★ **设计边界**见主规划 **§0.2**：按 A/B/C 三层分解设计（**C 层 API 形状不是依赖**）、
  **不被当前平台能力限制**、**唯一硬约束是"能飞腿实验、拿 OTA 第一手资料"**；
- ★★ **语料不是约束**（可补充采集），语料设计见主规划 **§3.1** 与 **`memo_09`**——
  目标是**覆盖部署环境**，因为 AI CE 最大的坑就是"训练环境 ≠ 部署环境"；
- ★ **下一步功课**：`memo_09` §11 的 **P0 批**可以用现有存量直接解锁；
  **P1 批**阻塞在"补 4 个采集字段"上（`dmrs_type`、`nof_cdm_groups_without_data`、`n_rapid`、逐 RE 后均衡噪声）。

★ 最有价值的一条纪律：**功课没做完就开工，等于把"值不值得做"这个问题推迟到已经投入之后才回答。**

## 本目录的写作纪律

- 数字分等级：【实测】（本仓库跑出来的，可追溯到一次运行）、【已核实片段】/【读全文】/【检索】
  （外部来源）、【未核实原文】/【估算】（**引用前必须复核**，不得当作实测引用）。
- 每个结论给 `文件:行号` 或数据来源，便于复核。
- 判据在实验之前写死；每个数字可追溯到一次 leg / commit（继承 `metal_kernel_fusion` 的纪律）。
