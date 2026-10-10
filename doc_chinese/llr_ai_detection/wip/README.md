# `llr_ai_detection/wip/` —— 本工作流的**工作目录约定**

> 版本：v1.0 ｜ 日期：2026-10-08
> 目的：把本工作流的**目录、命名与出处**三件事一次定清楚，避免重演历史工作流里
> "腿名不可比、语料散落 `/tmp`、工具找不到出处"这几类问题。

---

## 1. 目录地图（哪些进 git，哪些不进）

| 路径 | 进 git？ | 放什么 | 为什么 |
|---|---|---|---|
| `llr_ai_detection/wip/` | ✅ **进** | 工具、脚本、arm 配置（YAML）、探针、一次性实验配方 | 这些是"下一腿还要用、而且要被人读懂"的东西 |
| `llr_ai_detection/wip/logs/` | ❌ **不进** | ★ **新飞腿的日志** | 由 `doc_chinese/.gitignore:17` 的 `**/logs/` 规则忽略（**已核实**） |
| `llr_ai_detection/work_tmp/` | ❌ **不进** | 捕获语料、参考二进制、仪器输出、构建配方产物等**易失工作产物** | 由 `doc_chinese/.gitignore:25` 的 `**/work_tmp/` 规则忽略（**已核实**） |
| `llr_ai_detection/*.md` | ✅ **进** | memo、规划、索引 | 文档是这个仓库的记录 |
| `llr_ai_detection/ref_paper/*.pdf` | ❌ **不进** | 论文原文 | 第三方版权 + 体积；见该目录的 `README.md` |
| `llr_ai_detection/survey/` | ✅ **进** | 英文原始调研材料 | 带逐条引用，是 `memo_03` 的证据底稿 |

### 1.1 ★ 为什么要有 `work_tmp/`（一次真实的教训）

`/tmp` 会被重启清空。`doc_chinese/work_tmp/README.md` 记着那次事故：
**2026-09-16 的一次重启一次性带走了 237 个捕获语料、参考二进制、门脚本、上机日志**，
导致那一轮几乎无法复跑对拍。

⇒ **凡"下一腿还要用、且不能指望 `/tmp` 还在"的东西，都放 `work_tmp/`。**

★ 历史包袱说明：AI CE 线的语料在 **`~/ai_ce_work/`**（仓库外），那是**历史原因**。
本工作流**统一用 `llr_ai_detection/work_tmp/`**，不再往仓库外放东西。
（`memo_04`、`memo_09` 里引用的 `/Users/jiachengwang/ai_ce_work/capture/` 是**历史语料**，
按 §3 的规则注明出处即可，不要继续往里写。）

---

## 2. ★★ leg 命名：`aillr0NN-<suffix>`

### 2.1 规则

```
aillr0NN-<suffix>
  │    │   └── 这一腿在做什么（小写、连字符），例如 `baseline` / `ane-proto` / `corpus-fields`
  │    └────── 单调递增的三位序号，由 next_leg_label.sh **计算**，不靠记忆
  └─────────── 本工作流前缀
```

**为什么改前缀**：`mkf` 是**上一个工作流**（`metal_kernel_fusion`）的缩写。
保留编号、更新名字——这样**腿名同时说明了"第几腿"和"属于哪个工作流"**。

### 2.2 序号由工具算，不靠记忆

```bash
bash doc_chinese/llr_ai_detection/wip/next_leg_label.sh                 # → 001
bash doc_chinese/llr_ai_detection/wip/next_leg_label.sh baseline        # → aillr001-baseline
```

★ 工具从 **leg 日志本身**数（扫 `doc_chinese/*/wip/logs/gnb_*_aillr*.log.stderr`），
所以序号**不可能与实际飞过的腿漂移**；新工作流目录出现时也无需改它。

★ **已飞过的腿名不改**——**腿名是证据**。（这正是 `mkf` 那边"前七腿保持无编号形式、
只计数不改名"的同一条纪律。）

### 2.3 与上一个工作流的关系

| | 上一个工作流 | 本工作流 |
|---|---|---|
| 前缀 | `mkf`（metal kernel fusion） | **`aillr`（AI LLR detection）** |
| 编号 | `mkf001` … `mkf033`+ | 从 **`aillr001`** 重新开始 |
| 日志 | `doc_chinese/metal_kernel_fusion/wip/logs/`（170 个文件） | `doc_chinese/llr_ai_detection/wip/logs/` |
| 工具 | `doc_chinese/metal_kernel_fusion/wip/next_leg_label.sh` | 本目录的 `next_leg_label.sh`（同一逻辑，换前缀） |

★ 两个序列**互相独立**：`aillr001` 与 `mkf001` 没有关系。

---

## 3. ★★ 历史工具的出处（引用必须注明）

本工作流的 memo 大量引用了上一个工作流的工具。**按用户要求：在工作 memo 中注明出处。**
下表是权威对照，引用时请用**完整路径**，不要只写文件名。

| 工具 | 出处（`git ls-files` 核实） | 用途 |
|---|---|---|
| **`fly_leg.sh`** | `doc_chinese/macos_thread_priority/wip/fly_leg.sh` | ★ 飞腿主脚本（`fly_leg.sh <label> <dual 或 triple> <quiet 或 stress> [cpu 或 cpu_gpu 或 gpu]`） |
| **`ul_health.sh`** | `doc_chinese/macos_thread_priority/wip/ul_health.sh` | 腿健康检查 |
| **`pair_check.sh`** | `doc_chinese/macos_thread_priority/wip/pair_check.sh` | ★ 成对腿判据（**注意：它不渲染结论、恒 `exit 0`**，判据是 ±10% B/hop、±20% defer99/duration） |
| **`probes_off_syntax_check.sh`** | `doc_chinese/macos_thread_priority/wip/probes_off_syntax_check.sh` | ★ 探针关闭版的语法/编译检查（防"M4 缺陷类"复发） |
| **`ab_dumps.sh`** | `doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh` | ★ 离线 bit-exact A/B |
| `narrow_arms.sh` / `neutral_vs_baseline.sh` / `l1_hop_arms.sh` | `doc_chinese/phy_pipeline_gpu/wip/` | 窄带 arm、中性对照、L1 hop arm |
| **`next_leg_label.sh`** | `doc_chinese/metal_kernel_fusion/wip/next_leg_label.sh` | 腿序号计算（本目录已复制并改前缀） |

### 3.1 历史 leg 日志的位置

| 目录 | 文件数 | 属于 |
|---|---|---|
| `doc_chinese/phy_pipeline_gpu/wip/logs/` | 1141 | 融合 lane 早期 |
| `doc_chinese/macos_thread_priority/wip/logs/` | 389 | 线程/实时性 |
| `doc_chinese/metal_kernel_fusion/wip/logs/` | 170 | ★ **融合工作流（上一个）** |
| `doc_chinese/work_tmp/logs/` | 57 | 早期杂项 |

★ 这些**都可以参考**——但引用时注明"出自哪个工作流的哪条腿"。

### 3.2 历史语料与参考二进制

| 路径 | 内容 | 出处 |
|---|---|---|
| `doc_chinese/work_tmp/corpus/` | **27 个合成捕获**（54 文件） | 生成器在树里：`lib/phy/upper/signal_processors/channel_estimator/metal/make_synthetic_capture.py` |
| `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref` | 三网对拍的**参考二进制**（含 sha256） | 提交 `1247eddf09`，**2026-09-17 重建** |
| `~/ai_ce_work/capture/` | ★ AI CE 语料（152,874+ 次接收） | **仓库外，历史原因**；见 §1.1 |

★ **参考二进制必须重建**：`doc_chinese/work_tmp/README.md` 记着"同一个提交、
不同日期构建的产物不一样"——离线 A/B 用错参考会得到**假红**。

---

## 4. 本工作流的额外纪律

| # | 纪律 | 理由 |
|---|---|---|
| 1 | ★★ **语料按 `memo_09` 的设计落在 `work_tmp/corpus/`**，不落 git | 语料是长期资产，但不该进历史（体积 + 不可复现） |
| 2 | ★ **每腿与一次"采集意图"绑定**（`memo_09` §8） | 否则语料变成"顺便录的一堆东西" |
| 3 | ★ **性能腿与采集腿分开飞** | 采集本身有成本（~50–100 µs/slot） |
| 4 | ★ **引用历史工具/日志必须写完整路径** | 见 §3 |
| 5 | ★ **腿名不改**，序号由工具算 | 腿名是证据 |
| 6 | ★ **每个数字可追溯到一次 leg** | 继承上一个工作流的纪律 |
| ★ 7 | ★★ **文档一律"增补进现有文件"，不新建 memo 文件**（用户裁定 2026-10-08）| ① 见 §4.1；★ `memo_01`–`memo_10` 是**已闭合的证据链**，继续加号会让"读哪一份"重新变成问题 |

### 4.1 ★★ 文档落点规则（用户裁定 2026-10-08）

> 用户原话：*"请直接加入 `llr_ai_detection_design_and_implementation.md`，
> 并在 `llr_ai_detection_high_level_status_and_plan.md` 增加几句话的 summary，
> **而不是创建一个新的文档 `memo_11`（以后也类似，在现有的 design/implementation 中增加内容，
> 而不是创建新文件）**。"*

⇒ ★★ **本工作流从 2026-10-08 起按下面的规则落文档：**

| 要写的东西 | ★ 落到哪 | 不要做什么 |
|---|---|---|
| 设计、方案、判据、臂定义、实现决策 | ★★ **`llr_ai_detection_design_and_implementation.md`**，**追加一个新小节**（活文档，追加式） | ✗ 不要新建 `memo_11`… |
| 现状、进度、优先级、给下一个会话的结论 | ★ **`llr_ai_detection_high_level_status_and_plan.md`**，**几句话的 summary + 指回设计文档的节号** | ✗ 不要在高层文档里重抄设计的正文 |
| 实测结果、实作记录、决策留痕 | ★ **设计文档 §11 Memo 区**（新的在最上面） | ✗ 不要为一次飞行新建文件 |
| 工具、脚本、臂配置 | ★ `wip/`（进 git） | — |

★ **为什么**（不只是偏好）：
1. ★ **"只读一份就能接着开工"** 是本工作流的既有约定（见 `session_handoff_*.md` 的用法说明）。
   每加一个 `memo_1x`，这条约定就多一个需要同步的地方；
2. ★ **设计文档本来就是"活文档、追加式"**（其开头逐字规定"被证伪的判断以'更正'追加而非删除"），
   ⇒ **新内容进它，正是它设计的用途**；
3. ★ 高层文档只放 summary，**保证"高层永远是短的"** —— 它一旦被正文撑大，就不再有"先读这一节"的价值。

★ **编号规则**：新小节插在**相关章节的末尾**，取**下一个空号**（例如 §1 的下一号是 §1.5，
因为它已有 §1.0–§1.4 与 §1.1bis）。★★ **不重编号已有小节** —— 交叉引用（`§x.y`）遍布全部 memo，
重编号会让它们**静默指错**。

---

### 4.2 ★★ 文档**不进** `wip/`（★ 2026-10-08 判例）

★ 用户问：★ **`wip/` 里的设计文档是否应当上移到 `llr_ai_detection/`？**
★★ **答案：是。** ★ 依据是本文件 §1 的目录地图自己写的定义 ——

| ★ 路径 | ★ 放什么（★ §1 原文）|
|---|---|
| ★ `wip/` | ★★ **"工具、脚本、arm 配置（YAML）、探针、一次性实验配方"** |
| ★ `llr_ai_detection/*.md` | ★★ **"memo、规划、索引 —— 文档是这个仓库的记录"** |

★★ **⇒ 判据很硬**：★ **一个 `.md` 是"给人读的规划/调研"还是"下一腿要用的工具"？**
★ 前者进 `llr_ai_detection/`，★ 后者留 `wip/`。

★★ **本次执行（2026-10-08）** ★ 两份文档都已上移，★ **`wip/` 里现在没有文档**：
```
doc_chinese/llr_ai_detection/wip/model_build_reconnaissance.md
  → doc_chinese/llr_ai_detection/model_build_reconnaissance.md     ✅
doc_chinese/llr_ai_detection/wip/model_network_design.md
  → doc_chinese/llr_ai_detection/model_network_design.md          ✅
```
★ 同时更新了 `llr_ai_detection_design_and_implementation.md` §15 与
`llr_ai_detection_high_level_status_and_plan.md` 第八次更新里的引用。

★★★ **一次真实的返工（★ 记下来，★ 因为它正好是本条判例的反面教材）**：
★ 上移之后，★ **给 `model_network_design.md` 追加 §14 时又写回了 `wip/` 的旧路径**
（★ 追加是用旧路径的字符串做的，★ 而那个路径当时已经不存在）⇒ ★★ **重新造出一个
`wip/model_network_design.md`，★ 且被 commit 收了进去**（★ `26f1e5ee0e`）。
★★ **症状很隐蔽**：★ `git status` 显示干净（★ 因为新文件**被 add 过了**），
★★★ **而正文其实躺在 `wip/` 里、顶层那份缺了 §14。**
★★ **修法**：★ 把 §14 合并回顶层（★ 逐字节校验与已提交版本一致），★ 删掉 `wip/` 那份。
★★★ **教训**：★★ **移动文件之后，追加内容的路径必须重新确认** ——
★ 一个"曾经正确"的路径字符串会在移动后**静默地创建一个新文件**，
★ 而 `git status` **不会**因此报警。

★★ **为什么保留为独立文件、而不是并进设计文档**（★ 与 §4.1 的张力，★ 记录在案）：
★ §4.1 说"设计、方案、判据 → 设计文档，追加新小节"，★ **严格按字面这两份也该并进去**。
★ 但：★★ **① 体量**（★ 两份合计 **122 KB**，并进去会让设计文档（现 150 KB）翻倍）；
★★ **② 结构**（★ 调研含 §0 速览 + §1–§16，★ 是**多会话参考**，★ 不是一次追加的小节）；
★★ **③ 先例**（★ `session_handoff_*.md` 同为独立文件）。
★★★ **⇒ 本工作流的判据定为**：★ **"小的追加"进设计文档 §15 这类小节；
"独立的、多会话的参考文档"留在 `llr_ai_detection/` 顶层并在设计文档里【留指针】。**

---

## 5. 当前内容

| 路径 | 内容 |
|---|---|
| `next_leg_label.sh` | 腿序号计算（`aillr` 前缀版） |
| `A6_capture_fields_plan.md` | ★ **A6 实现方案**（采集字段补齐）：JSONL sidecar、布局自描述、A6a/A6b 拆分、验收判据 |

★ **本目录只放"工具、脚本、臂配置"。文档不放这里** —— 见 §4.2。

---

## 6. ★★★ 检索取向纪律（★ 2026-10-08，★ 用户更正）

> ★★★ 用户：★ **"Apple silicon 的 ANE 只是 wireless PHY 的 AI 算法（网络）的一个具体承载方式，
> 我们做文件检索和调研的时候，应该更加专注在算法层面和 inference 前向网络的架构设计方面。"**

### 6.1 ★★★ 纪律

★★★ **任何文献检索必须先覆盖【算法与架构】维度，再覆盖【载体/实现】维度。**
★ **载体维度的空白不等于学术空白** —— ★★ 它只说明"没人把它放到那块硅上"。

| ★ 维度 | ★ 该用的检索词举例 | ★ **它能回答什么** |
|---|---|---|
| ★★★ **算法** | ★ `neural receiver`、`learned demapper`、`LLR estimation`、`quantization-aware`、`fixed-point`、`iterative detection and decoding` | ★★ **有没有人做过这件事，★ 做到什么程度**（★ **这才是贡献的判据**）|
| ★★★ **架构** | ★ `receptive field`、`parameter count`、`FLOPs`、`depthwise separable`、`dilated convolution`、`attention factorization`、`activation quantization` | ★★ **同类问题的最优形态与已知边界** |
| ★★ **载体/实现** | ★ `Apple Neural Engine`、`CoreML`、`NPU inference`、`edge deployment` | ★★ **可行性，★ 不构成贡献** |

### 6.2 ★★ 一次真实的教训（★ 记下来）

★★ **我在 `model_build_reconnaissance.md` §9.3 做过一次结构化 arXiv 检索，★ 全部用载体词**
（★ `abs:"Apple Neural Engine"` 13 条、★ `abs:"CoreML" AND (wireless|radio|signal)` 2 条），
★ 结论写成 **"没有任何已发表工作把无线 PHY 跑在 ANE 上 ⇒ 我们是第一个"**，
★★★ **并把它当成"有据可查的新贡献"。**

★★★ **换成算法/架构词，同一个 arXiv 立刻命中**：
★ **神经网络接收机的 int8 PTQ**（[arXiv:2508.06275](https://ar5iv.labs.arxiv.org/html/2508.06275)，
★ **同为深度 3 + 64QAM + LLR**）、★ **axial self-attention 高效神经接收机**
（[arXiv:2510.12941](https://arxiv.org/abs/2510.12941)，★ SPAWC 2026）、
★ **微格式浮点量化 + 剪枝**（[arXiv:2609.31177](https://arxiv.org/pdf/2609.31177v1.pdf)）
⇒ ★★★ **"空白"是检索取向造出来的，★ 不是领域的事实。**

★★★ **教训（★ 可迁移）**：★★ **检索词的取向决定了你会不会误以为自己是第一个。**
★★ **一个只覆盖载体的检索，★ 会给出"零先例"的假象，★ 而那个"零"只对检索词成立。**

### 6.3 ★★ 相关落点

★ **完整更正与重新检索的结果**：★ `model_build_reconnaissance.md` §17；
★ **对设计的直接影响与四个可检验目标（G-1…G-4）**：★ `model_network_design.md` §17。
