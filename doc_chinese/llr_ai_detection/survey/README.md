# `survey/` — 原始调研材料（英文）

这三份是**原始调研笔记**，由并行调研代理产出，**保持英文原样**（含逐条引用、URL 与来源等级标注），
作为 `../memo_03_literature_survey.md` 的证据底稿。**结论以 `memo_03`（中文）为准**；
需要追溯某条引用的原始片段时来这里。

| 文件 | 内容 |
|---|---|
| `survey_master_ai_receiver_literature_review.md` | 主线综述：Transformer/ICL 检测、模型驱动 DL、LLR 输出接收机、译码器在环、真实硬件时延、**Apple Silicon 章节**、综合与空白 |
| `survey_line_a_model_based_mimo_detection.md` | LINE A：模型驱动 / 深度展开 MIMO 检测（DetNet、OAMP-Net2、MMNet…）——**输出软符号，不产出标定好的 LLR** |
| `survey_line_a_part1_detnet_oampnet.md` | LINE A 的 Part 1 细化稿（DetNet / OAMP-Net 逐条证据） |
| `survey_line_b_llr_output_receivers.md` | ★ LINE B：**输出 LLR** 的神经接收机与编码感知损失、**LLR 校准/失配/裁剪**、LLR→译码器接口（**修订稿**，方法学标注更严：凡未取证的一律标注，不从记忆补全） |
| `survey_line_c_decoder_in_the_loop.md` | LINE C：**译码器在环**（端到端、外信息、以及文献报告的问题） |

> ⚠ 调研仍在进行中，**同一主题可能有修订稿**（文件名相同即已覆盖为最新稿）。
> 全部调研线结束后会做一次统一整理；若某条结论与 `memo_03` 冲突，**以 `memo_03` 为准**。

## 阅读这些文件时必须知道的三件事

1. **来源等级标注**：每份文件顶部都有 method note。标 `[unverified]` 的数字**不是**从来源读到的，
   是回忆或结构性推断——**引用前必须复核**。
2. **本次调研的网络限制**：产出这些文件的沙箱封锁了几乎所有 `web_fetch`
   （arxiv.org / IEEE / Semantic Scholar / github.com / huggingface.co 均不可达），
   只能通过 `web_search` 的索引片段取证。因此**公式、片段、标题可信度较高，数字可信度较低**。
3. ⚠ **这些文件里的"本文/我们"指的是调研代理，不是本工作流**。判断与建议以 `memo_03` 与主规划为准；
   若两者冲突，**以 `memo_03` 为准**（它是在读过这三份材料之后写的综合，并已标注了哪些结论被降级）。
