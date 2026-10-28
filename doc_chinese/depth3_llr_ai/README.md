# `doc_chinese/depth3_llr_ai/` — Depth-3 LLR AI（第二代）工作流

> **工作流目标**：把 PUSCH 接收链的"信道估计 + 均衡 + 解映射"换成**一次神经前向**，输出**标定过的 LLR**
> 交给现成 LDPC 译码器，并要求在**真实空口**上**不输给经典链**。
>
> 本目录是 `doc_chinese/llr_ai_detection/`（第一代）的**接续工作流**，不是重写：第一代建立起来的采集、
> 语料、训练与回放设施全部复用，**变的是设计与判据**，理由是 §19.627 那份对照文献的审查。

## 为什么要有第二代

第一代把网络做成了 **26 185 参数**、在验收集上解出 90.7%、attach 上 69.2%，而**经典链在同样语料上是 100%**
（语料只收录经典链解成功的接收，`llr_ai_detection` §19.626）。对照 `survey/` 的十份调研，五个根本性缺陷已经定位：

| # | 缺陷 | 后果 |
|---|---|---|
| 1 | 为一个**已被自己测量否证**的时延模型（参数↔时延）压缩网络 | 比所有已发表可用接收机小 5–47 倍，attach 输 30.8 pp |
| 2 | 语料被"经典链成功"条件化 | 天花板锁死在 100%，永远测不出真实水平 |
| 3 | 完全没有 **LLR 标定** | 学习 LLR 过自信、量级失配 ⇒ 译码器吃亏 |
| 4 | 门控在 BLER ≈ 10⁻¹ | 10⁻⁴ 之后的 error floor 完全没测 |
| 5 | 输入表示（5 通道平面）从未消融 | 最高杠杆的决定没有依据 |

## 阅读顺序

| 顺序 | 文档 | 内容 |
|---|---|---|
| **1** | **`depth3_llr_ai_high_level_status_and_plan.md`** | 现状（继承的实测事实）、五个缺陷、**预登记判据**、五条工作线、里程碑、风险 |
| **2** | **`depth3_llr_ai_design_and_implementation.md`** | **活文档**：§0 不变式、§1 与第一代的差异及理由、§2 网络、§3 语料（HARQ 标注）、§4 损失与标定、§5 门控、§6 资产复用、§7 memo 区 |
| — | `work_tmp/` | 工作目录（**不进 git**，见 `doc_chinese/.gitignore`）：语料、检查点、日志、临时脚本 |

第一代文档仍然必读，尤其：`llr_ai_detection/model_network_design.md`（网络**是**什么）、
`llr_ai_detection/model_register.md`（模型登记与四门）、`llr_ai_detection/training_corpus_construction.md`（语料怎么建）、
`llr_ai_detection/survey/`（十份文献调研）。

## 复用第一代的资产（不复制，直接引用）

| 资产 | 位置（第一代） | 用法 |
|---|---|---|
| **采集设施** | `llr_ai_detection/wip/run_collect_leg.sh` | 直接调用；已含采集预算、心跳、`-- capture yield --` 收尾判定 |
| **语料（真实）** | `llr_ai_detection/work_tmp/corpus/` | 约 30 万条捕获；**注意**只有 2026-10-26 之后的腿是完整采集（之前的腿被同名覆盖，每条腿只剩 10.24 s） |
| **数据集** | `llr_ai_detection/work_tmp/data/datasets/e17_*` | train 100 004 ／ monitor 1 523 ／ test 3 294 |
| **训练脚本** | `lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py` | 直接扩展（BCE-on-bits 已有） |
| **回放与门控** | `lib/phy/upper/channel_processors/metal/ul_chain_replay` ＋ `work_tmp/probe/*.py` | 四门全部由回放出数 |
| **导出** | `llr_ai_detection/wip/finish_run.sh` | `.keras` → `.mlpackage` → `.mlmodelc` |

**规约**：本目录**不复制**这些设施；只写**新的**设计、判据与脚本。跨代引用一律写明第一代的路径与节号。

## 约定

- 代码与注释用英文；备忘与设计用中文。
- 每条结论必须能指回**一个可复现的脚本**或**一次实测**；估计值要标"估计"。
- 判据**先登记再实验**；追加而非改写；默认值只在明确要求时改。
- `work_tmp/` 不进版本库：语料、日志、检查点都在那里。

## 文档清单（补充）

| 文件 | 性质 | 内容 |
|---|---|---|
| `depth3_llr_ai_model_train_status.md` | ★ **活文档** | 训练运行登记、当前进度、回来检查清单、精度基线 |
| `../llr_ai_detection/machine_compute_capability_and_model_train_memo.md` | **固定文档** | 机器算力、环境安装、训练步骤与断点续训 |
