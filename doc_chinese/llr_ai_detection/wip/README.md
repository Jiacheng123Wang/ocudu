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
| **`fly_leg.sh`** | `doc_chinese/macos_thread_priority/wip/fly_leg.sh` | ★ 飞腿主脚本（`fly_leg.sh <label> dual\|triple quiet\|stress [cpu\|cpu_gpu\|gpu]`） |
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

---

## 5. 当前内容

| 路径 | 内容 |
|---|---|
| `next_leg_label.sh` | 腿序号计算（`aillr` 前缀版） |
