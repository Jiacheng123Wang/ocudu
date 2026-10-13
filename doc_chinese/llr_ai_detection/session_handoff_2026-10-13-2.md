# Session handoff —— `llr_ai_detection`（2026-10-13，第 2 次）

> **用法**（沿用约定 ✓）：新会话**只读这一份**就能接着开工 ✓。
> **结构** = ★★★ 最前面的三件事 ① 本会话做了什么 ② 当前状态 ③ **下一个会话的第一件事（具体命令）**
> ④ 详细执行方案（ANE 重量 / 全量训练 / 标度校准 / 语料采集 / R5 / R6）⑤ 未决问题 ⑥ 纪律 ⑦ 常用命令 ⑧ 提交。
>
> **上位文档**：`llr_ai_detection_design_and_implementation.md`（★ **§12.6–§12.13、§16、§17、§18 = 本会话的记录**）、
> `model_network_design.md`（★ **§0.2 = I/O 契约，⚠ 顶部有按实测的修订块**、★ §45 = R1 的证据与 ANE 实测）。
>
> ★★ **本会话 26 个提交**：★ 范围 `8be50bf1b3` … `23caefffc3`（★ 命令：`git log --oneline 0a78de6383..23caefffc3`）。
> ★★ **工作树干净**。★ 注：`0a78de6383` 是上一会话的最后一个提交，★ 它是**基线**，不属于本会话。

---

## ★★★ 先写在最前面的三件事

### ★★★ 一、本会话最重要的【技术】结论：**把符号轴折叠进通道，是这条路线的天花板**

★ 契约原形状 `[1,612,1,64]` 把 **14 个符号折叠进通道** ⇒ 3×3 卷积**只能沿频率**工作 ✗。
★★★ **同数据、同 60 epoch、同调度的对照**：

| 布局 | train | val | **未编码 BER** | coded BLER |
|---|---|---|---|---|
| folded `[1,612,1,64]` | 0.5692 | 0.5703 | 0.3480 | 1.0000 |
| ★★★ **unfolded** `[1,612,14,8]` | **0.0716** | **0.0684** | ★★★ **0.0254** | — |

★★★ **改善 13 倍**，★ 而宽度(0.059)、LR(0.029)、epoch(0.039)、标签、损失、dense **六个自变量全都只有 0.03 量级** ✗（★ §18.9 的完整扫描）。
★★ **契约已按实测修订**（★ 输入改为 **`[1,612,14,12]`** ✓ 见 §0.2 顶部的修订块 ✓，★ **但 (a)(b) 表格逐行改写仍未做** ⬜）。

### ★★★ 二、本会话最重要的【工程】结论：**整条链路已打通**

★★ **空间化布局 + 800 样本 + 30 epoch + 单头**（★ ≥30 dB 切片）即得：
★★★ **未编码 BER `0.0187`、★ coded BLER `0.4578`** —— ★★★ **coded BLER 首次 < 1.0**（★ 此前**所有**配置都是 1.0000 ✗）。
★★ ⇒ ★★ **契约 → 数据 → 损失 → 网络 → 真实 LDPC 译码，整条链路是通的** ✓（★ §18.12）。
★★★ **而这远不是上限**：★ 只用了 **800/3752 样本、30 epoch、1/4 个头**（★ `--no-aux`）。

### ★★★ 三、下一个会话的第一件事：**按 ①→②→③ 的顺序做，不要先采语料**

| # | ★★ 动作 | ★★ 为什么是这个顺序 |
|---|---|---|
| ★★★ **①** | ★★★ **在【空闲机器】上重测空间化布局的 ANE 时延**（★ 几分钟）| ★★ (612,14) 上的 3×3 ⇒ **每层算力约 14×** ✗ ⇒ ★★★ **§45.7 的 365 µs 与 §45.9 的账【都不再适用】** ✗；★★ **这个数字决定"空间化"是否可用**，从而决定后面训什么 ✓ |
| ★★★ **②** | ★★★ **全量训练：3752 样本 + 四头 + 长 epoch**（★ 后台 2–3 小时）| ★★ 现有数据**远超**用掉的量（★ 3752 vs 800 ✓）；★★ **不需要新语料** ✓ |
| ★★ **③** | ★★ **R4.5 标度校准**（★ 与②并行）| ★★ 输出是 `tanh·S`，`S` 现在是我给的初值 `20` ✗ ⇒ ★★ **校准可能不重训就改善 coded BLER** ✓；★ 判据已在契约里（★ 量化前后 CRC 差异 ≤ 阈值 ✓）|
| ★★ ④ | ★★ 若精度仍不足 ⇒ **再采语料**（★ 但**必须先修 TB 转储** ✗ 见 §⑤）| ★★ 采集解决的是**分布覆盖** ✗，★ 当前瓶颈是**没训够** ✗ |

---

## ① 本会话（2026-10-13）做了什么

**一句话**：★★ **按 `design` §12 的 R1–R6 修正规划，把"深度 1 变体"改回真正的深度 3** ——
★★ **R1（契约）、R2（目标）、R3（数据集）完成并各自留下自检**；★★ **R4 的诊断走完九项对照、根因锁定并打通**（★ coded BLER 首次 < 1.0）；
★★ **R5 完成引擎通路与计划冻结**（★ 未接链）；★ **R6 只有契约层面**。

### A. ★★★ 三个工具件（★ 全部可复用，★ 都有 `--selfcheck`）

| ★ | ★★ 件 | ★★★ 作用 |
|---|---|---|
| ★★★ | ★ `ai_train/depth3.py` | ★★ **契约的可执行实现**：`pack_input` / `label_plane` / `llr_gather` / `llr_from_logits`（★ 全向量化）★ + 自检 |
| ★★★ | ★ `ai_train/depth3_loss.py` | ★★ **逐比特 BCE on 加扰域编码比特**（★ 标签 `1−b` ✓）+ 辅助头（★ NMSE / `log10` 噪声）★ + 自检 |
| ★★★ | ★ `ai_train/build_depth3_dataset.py` | ★★ **数据集构建**（★ `real` / `synthetic` 两源 ✓）+ ★★ **能证伪标签的自检** |
| ★★★ | ★ `ai_train/ldpc_decode.py` | ★★ **归一化 min-sum + 接收侧逆链**（★ 解扰/解交织/速率解匹配）⇒ ★★ **coded BLER 可测** |
| ★★★ | ★ `ai_train/train_depth3.py` | ★★ **训练脚本**（★ 四头、★ 折叠 LayerScale、★ `--export-probe`/`--export-only`、★ `CodedBler`）|
| ★★★ | ★ `ai_train/eval_depth3.py` | ★★ **三方对照**（★ 网络 / **genie** / 完美）+ ★ **逐 SNR 分档** |
| ★★★ | ★ `ocudu_coreml_nn_engine::init_depth3/predict_depth3` | ★★ **引擎的 depth-3 通路**（★ fp16 `[1,612,1,64]` + **四个命名头** ✓ 零拷贝 ✓ + ★ `depth3_engine_check` 验证）|

### B. ★★★ 数据集（★ 现在可用）

| ★ 文件 | ★★ 内容 |
|---|---|
| ★★ `work_tmp/data/datasets/d3_cap001_rv0.npz` | ★★ **3752 样本**（★ 536 单元 × 7 档 12–48 dB）|
| ★★ `work_tmp/data/datasets/d3_cap002_rv0.npz` | ★★ **3437 样本**（★ 491 单元 × 7 档）|
| ★★ 字段 | ★ `X[612,64]`（★ **折叠布局** ⚠ 见 §③①）、★ `H/r[612,14,2]`、★ `r`、★ `sigma2_over_r2`、★ `bits`（★ 加扰域编码比特 ✓ 主头标签）、★ `tb_bits`/`nof_tb_bits`、★ `alloc_*`/`dmrs_bitmap`/`qm`、★ `rnti`/`n_id`/`rv`、★ `snr_nominal` |
| ★★ 自检 | ★★ `residual/sigma2` = **0.971–1.029**、★ 导频比 **1.005**（★ 60 抽样 ✓）|

### C. ★★★ 走通的实测数字（★ 引用时照此）

| ★ 项 | ★★ 数字 |
|---|---|
| ★★★ **最优配置（★ 空间化）** | ★★ 800 样本 / 30 epoch / **单头** / ≥30 dB ⇒ ★★ **BER 0.0187、★ coded BLER 0.4578** |
| ★★ 折叠布局全量 | ★ 3752 样本 / 150 epoch ⇒ ★ BER 0.2921、★ coded BLER **1.0000** |
| ★★ **genie 基线**（★ 真值信道 + 零迫 + max-log）| ★★ uBER **0.047**、★ BLER 0.3333（★ 12–48 dB 混合）|
| ★★ **只用输入面的解析接收机** | ★★ BER **0.0629** ⇒ ★ **输入面信息充足** ✓ |
| ★★ 引擎（★ 折叠布局 64 通道）| ★★ 四头**全部被写入**、★ p50 **345.8 µs**（⚠ 空间化后必须重量 ✗）|

---

## ② 当前状态

### ★★ 已就绪（★ 直接可用）

★★ 见 §①A 的七个工具件 + §①B 的两个数据集 + ★ `--export-only` 导出链（★ 训练好的 `.keras` → 契约形状的 fp16 `.mlpackage` ✓）。

### ★★★ 必须做（★ 按依赖）

| ★ | ★★ 缺什么 | ★★ 详见 |
|---|---|---|
| ★★★ | ★★ **§0.2(a)(b) 的表格逐行改写**（★ 现在只有顶部修订块）| §④D |
| ★★★ | ★★ **按新形状（`[1,612,14,12]`）重建数据集** | §④D |
| ★★★ | ★★ **全量 + 四头 + 长训练** | §④B |
| ★★ | ★★ **空间化布局的 ANE 时延** | §④A |
| ★★ | ★★ **R4.5 标度校准** | §④C |
| ★★ | ★★ **R5.1–R5.6（★ 组装器按新布局）** | §④E |
| ★ | ★ **R6 链上接入** | §④F |

---

## ③ ★★★ 下一个会话的第一件事（★ 具体命令 ✓）

```bash
cd /Users/jiachengwang/dev/ocudu
W=doc_chinese/llr_ai_detection/work_tmp
P=lib/phy/upper/signal_processors/channel_estimator/metal/ai_train
PY=~/ai_ce_work/venv/bin/python

# ① ★★★ 先确认三个自检都过（★ 环境是否健康）
$PY $P/depth3.py --selfcheck
$PY $P/depth3_loss.py --selfcheck
$PY $P/ldpc_decode.py --selfcheck

# ② ★★★ 空间化布局的 ANE 时延（★ 必须先【确认无后台任务】再测；★ 至少两遍 ✓ F15）
#    需要一张【空间化】的 fp16 图：现在只导出了折叠布局的 ⇒ 先导出（★ 见 §④A 的命令）
cd $W/probe && ./probe <空间化模型>.mlmodelc 612 300     # 两遍

# ③ ★★★ 全量训练（★ 后台；★ 空间化布局；★ 四头；★ 需要先给 train_depth3 补"四头 + unfolded"的组合）
$PY $P/train_depth3.py --train $W/data/datasets/d3_cap001_rv0.npz \
  --val $W/data/datasets/d3_cap002_rv0.npz --out $W/data/runs/d3_full_unfolded \
  --epochs 100 --batch 8 --layout unfolded --bler-samples 24 --bler-every 20 \
  --export-probe $W/probe/d3_full_unfolded.mlpackage
```

★★★ **⚠ 两条必须先处理的技术债（★ 否则 ③ 跑不了）**：
1. ★★★ **`--layout unfolded` 目前【只建 LLR 头】**（★ 代码里显式 `parser.error` 要求配 `--no-aux` ✗）
   ⇒ ★★ **要跑"四头 + 空间化"，必须先给 `build()` 的 unfolded 分支补上三个辅助头**
   （★ `post_eq_noise` [612,14,1] ✓ `ch_est` [612,14,2] ✓ `dmrs_noise` [1,1,1,1] ✓，
   ★ 通道序仍要满足 `rename_outputs` 的**按通道数匹配** ✓）；
2. ★★ **`eval_depth3.py` 与 `CodedBler` 假定折叠布局**（★ `llr_gather` 走 `[612,112]` ✓）
   ⇒ ★★ 空间化后 `llr` 的输出已是 `[B,612,14*qm]` ✓ ⇒ ★ **需要一条按 layout 分支的 gather** ✓。

★ **本轮（800 样本）的可用模型**：`work_tmp/data/runs/d3_unfolded_800/depth3_best.keras`
（★ 可先用它做 `eval_depth3.py` 的对照，★ 但它只有 LLR 头 ✓ 且只覆盖 ≥30 dB ✓）。

---

## ④ ★★★ 详细执行方案

### ★★★ A. 空间化布局的 ANE 时延（★ 先做，★ 决定架构）

| ★ 步 | ★★ 内容 |
|---|---|
| ★ 1 | ★★ 给 `train_depth3.py` 的 unfolded 分支补三个辅助头（见 §③ 的技术债 1）|
| ★ 2 | ★★ `--export-only` 导出一张空间化 fp16 图 → `xcrun coremlcompiler compile` |
| ★ 3 | ★★ **确认无后台任务**，然后 `./probe <model>.mlmodelc 612 300` **跑两遍** |
| ★★ 4 | ★★ **判据**：★ 与折叠布局的 **345.8 µs** 比 ✓。★★ 若 **> ~1 ms** ⇒ ★ **"整个符号轴空间化"不可用** ✗ ⇒ ★★ **退路**：★ ① 符号轴**分块**（★ 2–4 个符号一组 ✓ 算力只增 2–4× ✓）；★ ② **浅层空间化 + 深层折叠**；★ ③ 回到折叠布局但**加深 1×1 栈**（★ 已知收益有限 ✗）|

### ★★★ B. 全量训练（★ 后台）

| ★ 步 | ★★ 内容 |
|---|---|
| ★★ B1 | ★★ **按 §④A 的结论定形状**（★ 空间化 / 分块 / 折叠 ✓）|
| ★★ B2 | ★★ **100–150 epoch、batch 8、全量 3752**（★ 参考：★ 空间化 60 epoch/400 样本 = 687 s ⇒ ★ 全量 100 epoch ≈ **3–4 小时** ✗ ⇒ ★ 后台 ✓）|
| ★★ B3 | ★★ **判据**（★ §2.3 / R2.4）：★★ **加上辅助头后主头 coded BLER 不变差** ✓ + ★★ **`eval_depth3.py` 的逐 SNR 对照要显示网络逼近 genie** ✓ |
| ★★ B4 | ★★ 记录时必须写明**切片与配置**（★ 上一轮的 0.4578 是 **≥30 dB + 单头** ✗ 不能当总体成绩 ✓）|

### ★★ C. R4.5 标度校准（★ 独立步骤，★ §3.3）

| ★ 步 | ★★ 内容 |
|---|---|
| ★★ C1 | ★★ 拟合 `{S_llr, A_n, B_n, A_d, B_d, C_H}`（★ 现在 `S_llr = range_limit = 20` 是**初值** ✗）|
| ★★ C2 | ★★ 判据：★ 最大化量化后 LLR 与真值比特的互信息 ✓（★ 或直接扫 `S` 看 coded BLER ✓）|
| ★★★ C3 | ★★★ **验收：量化前 vs 量化后的 CRC 差异 ≤ 预登记阈值** ✓（★ 用 `ldpc_decode` 做离线 ✓，★ 最终以链为准 ✓）|

### ★★ D. 契约与数据集的收尾（★ 不依赖训练）

| ★ 步 | ★★ 内容 |
|---|---|
| ★★ D1 | ★★ **§0.2(a)(b) 逐行改写成 12 通道空间化版本**（★ 规则：改设计必须同时改本节 ✓）|
| ★★ D2 | ★★ **数据集按新形状重建**（★ `pack_input` 输出 `[612,14,12]` ✓ 自检跟着改 ✓ ★ 或用 `unfold_plane` 在训练时重排 ✓ —— ★ 但**部署路径需要数据集与契约一致** ✓）|
| ★★ D3 | ★★ 重建后**重跑自检**（★ `residual/sigma2` 与导频比 ✓）|

### ★★ E. R5 接缝（★ 计划已在 §17，★ 但**组装器要按新布局重写**）

★★ 见 `design` §17 的完整计划（★ 输入来源逐条核对、要改的文件、判据 I1/I7/判据1/判据2 + 新判据 ✓）。
★★★ **相对 §17 的两处更正**：★ ① **输入面组装器输出 `[612,14,12]`** ✗ 不再是 `[612,64]` ✓；
★ ② **`rename_outputs`/引擎的校验按四个头 + 新输入形状** ✓（★ 引擎的 `init_depth3` 现在校验 `[1,nsc,1,64]` ✗ 需同步 ✓）。

### ★ F. R6 上报义务

★★ 契约层面已完成（★ 上报量已成头的形状 ✓）。★ 链上接入：★ 头 1 → `pusch_demodulator_notifier` 的 SINR（★ `memo_07` C2 ✓）、
★ 头 3/4 → 经典测量核（★ RSRP/EPRE/SNR/TA ✓）★ —— ★ **与 R5 同批做** ✓。

---

## ⑤ ★★ 未决问题与重开条件

| ★ | ★★ 问题 | ★★ 重开条件 / 下一步 |
|---|---|---|
| ★★★ | ★★★ **TB dump 1068/1144 是全零**（★ 而链报 `crc=OK`）| ★★★ **阻塞 `real` 数据源** ✗ ⇒ ★★ **采集真实语料的【前提】** ✓。★★ 机制：★ **全零字同时通过 LDPC 校验子与 CRC-24A** ✓；★★ 独立佐证：★ **rv=1/2 的系统比特占比 0.078 / 0.000** ⇒ ★★ 那些采集**本来就不可能单独译出** ✓（★ §12.11）|
| ★★ | ★★ **`crc=OK` 这个判据本身** | ★★ 需要一条**不会在零字上通过**的判据（★ 例如要求 TB 至少一个非零字节 + 重算 CRC ✓）|
| ★★ | ★★ **24 dB 以上无实测数据** | ★★ 合成数据补到了 48 dB ✓，★ 但**真实高 SNR 场景未验证** ⇒ ★★ 采集的**第一优先** ✓（★ 前提是修好 TB 转储 ✗）|
| ★★ | ★★ **空间化布局的 ANE 时延** | ★★ §④A |
| ★★ | ★★ **`_h.bin` 与导频 LS 的一致性**（★ 残差 ~1.0，★ 上一会话未解释）| ★★ 先确认它是否影响新契约（★ 新契约用**导频 LS** ✓ 可能自动绕过 ✓）|
| ★ | ★ **CPU LDPC 的 BLER 缺陷**（★ 10 dB 下 187-188/200）| ★ 单独排查；★ 本工作流不阻塞（★ LDPC 不在深度 3 内 ✓）|

---

## ⑥ ★★ 纪律（★ 本工作流累积；★ ★★ 是本会话新增）

| # | ★★ 纪律 | ★★ 出处 |
|---|---|---|
| ★★★ **F19★** | ★★★ **契约的校验要在【装订处】做**（★ 形状/名字/头数对不上就**拒绝**）| ★★ 本会话抓到三次：★ 三头图、★ `Identity*` 输出名、★ **导出随机权重** ✓ |
| ★★★ **F18★** | ★★★ **"指标能到 0"与"指标会失败"是【两条都要验】的判据** | ★ `ldpc_decode --selfcheck` 第 4 项 + `--bler-check` ✓ |
| ★★★ **F17★** | ★★★ **自检的【工况】必须贴近部署工况**（★ 码率、块数、rv）| ★ min-sum 的求和错误只在**真实码率**下暴露 ✓ |
| ★★★ **F16★** | ★★★ **数据集的自检必须能【证伪标签】**，★ 不能只查形状/范围 | ★★ 两个真缺陷都是"把比特重新调制、穿过信道、与网格相减"抓到的 ✓ |
| ★★★ **F15★** | ★★★ **时延数字必须在【机器空闲】时测，★ 且至少两遍** | ★ 第一次测残差头得 `+315 µs`，★ 真值 `+42 µs`（★ 后台在跑构建 ✗）|
| ★★ **F14** | ★★ 实现网络前**逐字核对 I/O 契约** | ★ `design` §12.2 |
| ★★ **F13** | ★★ 看起来像尺度问题，**先怀疑自己的度量** | ★ 本会话两次：★ 自检把未归一化的 `H` 又乘了 `r` ✗、★ genie 基线的星座标号推错（★ BER 0.56 ✗ 比掷硬币还差 ⇒ **是基线错** ✓）|
| ★★ **F7–F12** | ★★ 比值先定单位 / 扫描先复现基线 / 逐点关系先证同源 / 判决错误率用残差比 / 按 stem 归并 / 记录前先全量构建 | ★ `memo_08`、`memo_10` |

★★ **另两条本工作流的硬纪律**：
- ★★★ **一个参数只有在多个独立通道上同时成立才算对上**（★ `memo_08` §14.2ter）
- ★★★ **开关没生效 ≡ 没有效果**（★ I10）；★ 引用任何开关前**先证明它动了** ✓
  ★★ （★ 本会话的对应物：★★ **模型必须被证明"真的用了输入"** —— 破坏场通道后 BCE 从 0.575 恶化到 1.042 ✓）

---

## ⑦ 常用命令

```bash
cd /Users/jiachengwang/dev/ocudu
W=doc_chinese/llr_ai_detection/work_tmp
P=lib/phy/upper/signal_processors/channel_estimator/metal/ai_train
PY=~/ai_ce_work/venv/bin/python      # ★ 系统 python 没有 TF/Keras

# ★★★ 四个自检（★ 改契约/损失/数据/译码器后必须全过）
$PY $P/depth3.py --selfcheck
$PY $P/depth3_loss.py --selfcheck
$PY $P/ldpc_decode.py --selfcheck
$PY $P/build_depth3_dataset.py --selfcheck --out $W/data/datasets/d3_cap001_rv0.npz

# ★★★ 训练（★ 折叠布局 = 契约原形状；★ 空间化 = 符号轴作空间轴）
$PY $P/train_depth3.py --train $W/data/datasets/d3_cap001_rv0.npz \
  --val $W/data/datasets/d3_cap002_rv0.npz --out $W/data/runs/<名字> \
  --epochs 100 --batch 8 [--layout unfolded --no-aux] [--c-hid 64|128|256] \
  [--snr-min 30] [--target genie] [--llr-linear] [--scale-init 1.0] \
  --bler-samples 24 --bler-every 20 --export-probe $W/probe/<名字>.mlpackage

# ★★★ 只导出（★ 训练长跑时用；★ 从【训练后】的 .keras 导出 fp16 图）
$PY $P/train_depth3.py --export-only $W/data/runs/<名字>/depth3_best.keras \
  --export-probe $W/probe/<名字>.mlpackage

# ★★★ 三方对照（★ 网络 / genie / 完美 + ★ 逐 SNR 分档）
$PY $P/eval_depth3.py --model $W/data/runs/<名字>/depth3_best.keras \
  --data $W/data/datasets/d3_cap002_rv0.npz --samples 96

# ★★★ 引擎的 depth-3 通路（★ 编译 + 检查四个头是否真的被写入）
xcrun coremlcompiler compile $W/probe/<名字>.mlpackage $W/probe/
./build/lib/phy/upper/signal_processors/channel_estimator/metal/depth3_engine_check \
  $W/probe/<名字>.mlmodelc 20 612

# ★★★ ANE 探针（★ 空闲机器、★ 两遍）
cd $W/probe && ./probe <模型>.mlmodelc 612 300 && ./probe <模型>.mlmodelc 612 300

# ★★ 契约的指标检查（★ 完美 LLR 必须解出 0 错误）
$PY $P/train_depth3.py --train ... --val $W/data/datasets/d3_cap002_rv0.npz \
  --out /tmp/x --epochs 1 --max-val 8 --bler-samples 4 --bler-check

# ★★ 数据集重建（★ 两个语料各一条；★ R3 的命令模板）
$PY $P/build_depth3_dataset.py --units $W/data/tb_corpus/index.csv --tb $W/data/tb_corpus/tb \
  --corpus $W/corpus/aillr_cap001 --out $W/data/datasets --name d3_cap001_rv0 \
  --source synthetic --modulation 256QAM --snrs 12,18,24,30,36,42,48

# ★★ 构建（★ 要写进记录的数字之前；★ 引擎改动后）
cmake --build build --target ul_chain_replay depth3_engine_check -j8

# ★★ TB dump 复现（★ 缺陷一的复现命令）
mkdir -p /tmp/tbd && OCUDU_HELENA_DUMP_DIR=/tmp/tbd \
  ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
  $W/corpus/aillr_cap002/C_1147_17921 --metal --out /tmp/stage_x
```

★ **提交纪律**：★ `git commit -F <文件>`，★ 消息文件放 `work_tmp/tmp/`（★ `.gitignore` 已忽略 `work_tmp/` ✓）
★★ **不要把待复用的文件放 `/tmp`**（★ 机器重启会丢 ✓ —— 用户明确要求 ✓）。
★★ **`session_handoff_*` 只在【新会话】开始时写**；★ **工作过程中的记录一律追加进
`llr_ai_detection_design_and_implementation.md`**（★ 活文档 ✓ 用户 2026-10-13 明确 ✓）。

---

## ⑧ ★★ 本会话的提交（★ 26 个，★ 关键节点）

| ★ commit | ★★ 内容 |
|---|---|
| `8be50bf1b3` | ★★★ **R1：契约冻结 + `depth3.py`**（★ 实作时抓到 `scaling=1.4125` 与 LS 去加重 ✗）|
| `3ddc3bdb86` | ★★ R2：`depth3_loss.py` |
| `b8b49407a6` | ★★★ **R3：数据集 + 两个真缺陷**（★ 全零 TB、★ DM-RS 幅度）|
| `bf6da6d8c6` | ★★ 契约的 ANE 复验（★ 四头在 ANE 上）+ R3 全量 |
| `c0b043154a` | ★★ R4 开工：塌缩的复现与修复 + 残差结构成本 |
| `90a00f7179` | ★★★ **R4.0：译码器 + coded BLER 指标** + LayerScale 折叠（★ +0 µs）|
| `c28677b8a2` | ★★ 参照系（genie 基线）+ 抓到基线自己的错 |
| `e7f68f8550` | ★★ R2.4 判据成立（★ 辅助头不拖累主头）|
| `88cc0fedd7` | ★★ 导出时机修复 + `--export-only` |
| `171789f3b5` / `fb95b7550f` | ★ R5 计划冻结（§17）+ 契约校验的负向测试 |
| `24b132fd9c` | ★ §12.4 进度行更新 |
| `8ab0427202` | ★★ R4.4 的负面结果 + 九项诊断链（§18）|
| `9f906d6edd` / `a9be2ebdf4` | ★★ 二分定位 + 三臂对照（§18.6–18.8）|
| `6628602a9b` / `96f0a0fd81` | ★★ 自变量扫描矩阵 + 进度表如实更新 |
| `274434a63d` | ★★★ **不折叠符号轴：用更少资源得到明显更好的结果** |
| `25f48c9497` | ★★★ **同 epoch 对照确认根因 + 契约标注修订** |
| `030573bf09` | ★★★ **整条链路打通：coded BLER 首次 < 1** |
| `23caefffc3` | ★ §12.4：R4.4 路径已打通 |
