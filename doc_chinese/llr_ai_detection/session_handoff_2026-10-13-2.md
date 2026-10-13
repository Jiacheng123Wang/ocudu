# Session handoff —— `llr_ai_detection`（2026-10-13，第 2 次）

> **用法**（沿用约定 ✓）：新会话**只读这一份**就能接着开工 ✓。
> **上位文档**：`llr_ai_detection_design_and_implementation.md`（★ **§12.6–§12.9 = 本会话记录**）、
> `model_network_design.md`（★ **§0.2 = I/O 契约的唯一权威**、★ **§45 = R1 的证据与决定**、★ §45.7/§45.8 = ANE 实测）。
>
> ★★ **本会话 5 个提交**：★ `8be50bf1b3`（R1 契约 + 实现）、`3ddc3bdb86`（R2 损失）、
> `b8b49407a6`（R3 数据集 + 两个缺陷）、`bf6da6d8c6`（契约的 ANE 复验 + R3 全量）、`c0b043154a`（R4 开工）。
> ★★ **工作树干净**。★ 上一会话的最后一个提交是 `0a78de6383`（★ 基线）。

---

## ① 本会话做了什么（一句话）

★★★ **按 `design` §12 的 R1–R6 修正规划执行：R1 契约、R2 目标、R3 数据集【已完成】，
R4 已开工并跑通"能训起来"的小样验证；过程中抓到两个【真缺陷】，其中一个把上一会话的一个谜团解开了。**

| ★ | ★★ 阶段 | ★★★ 结果 |
|---|---|---|
| ★★★ **R1** | ★★ 冻结 I/O 契约 | ★★ `model_network_design.md` **§0.2 整体重写**（★ 64 通道**用满**、★ 绝对 BWP 窗口、★ **四个命名头**、★ 量化/标度/**符号**约定）；★ **可执行实现 = `ai_train/depth3.py`**（★ `--selfcheck` 全过）|
| ★★★ **R2** | ★★ 重做训练目标 | ★★ `ai_train/depth3_loss.py`：★ **逐比特 BCE on 加扰域编码比特**（★ 标签 `1−b`）+ ★ 辅助头（★ NMSE `0.1` / `log10` 噪声 `0.01`）；★★ **单位检查全过**（★ 完美→`9.4e-14`、★ 零 logit→`ln2`、★ 梯度 `σ(z)−y`、★ **反号可检出**）|
| ★★★ **R3** | ★★ 扩展数据集 | ★★ `ai_train/build_depth3_dataset.py`：★ **两条来源**（★ `synthetic` 可用 / ★ `real` **被阻塞**）+ ★ **自检**；★★ **两个语料 3752 + 3437 样本**（★ 12–48 dB）；★★★ **自检抓到两个真缺陷**（★ 见 §③）|
| ★★ **R4** | ★★ 重训网络（★ 开工）| ★★ `ai_train/train_depth3.py`：★ **无残差会塌缩**（★ val 停 `1.277`、★ BER `0.498`）⇒ ★★ **残差 + LayerScale + fp32 就训起来**（★ val `0.537` < `ln2`、★ BER `0.335` 且降）；★★ **ANE 复验**：★ 残差结构**仍在 ANE**（★ `ane=158`），★ 但 **+200 µs** |

---

## ② ★★★ 当前状态（★ 一句话）

★★★ **契约、目标、数据、训练脚本都就位且都自证过；网络在小样上已经能训起来；
下一步是把 R4.4 的全量训练跑完 —— 但在那之前有两个【必须先做】的东西（★ 见 §③）。**

### ★★ 已就绪（★ 直接可用）

| ★ | ★★ 位置 |
|---|---|
| ★★★ **契约 + 打包入口** | ★ `ai_train/depth3.py`（★ `pack_input`/`label_plane`/`llr_gather`/`llr_from_logits`，★ 全向量化）|
| ★★★ **损失** | ★ `ai_train/depth3_loss.py` |
| ★★★ **数据集** | ★ `work_tmp/data/datasets/d3_cap001_synth.npz`（3752）、`d3_cap002_synth.npz`（3437）|
| ★★★ **训练脚本** | ★ `ai_train/train_depth3.py`（★ `--residual` 默认开、★ `--dtype float32` 默认、★ `--no-aux` 消融）|
| ★★ **ANE 探针** | ★ `work_tmp/probe/build_depth3_probe.py` + `probe`（★ Swift，★ 报 placement 与 p50）|
| ★★ **修正后的合成核心** | ★ `ai_train/synth.py`（★ **导频已乘 DM-RS 幅度**）、★ `ai_train/dmrs.py`（★ `amplitude_scaling`）|

---

## ③ ★★★ 两个真缺陷（★ 本会话最重要的产出之一）

### ★★★ 缺陷一：**TB dump 1068/1144 是全零** ⇒ ★★ `real` 源被阻塞

★★ **现象**：★ `OCUDU_HELENA_DUMP_DIR=<dir> ul_chain_replay <unit> --metal` ⇒ ★ `crc=OK`，★ **dump 出的 TB 全零**
（★ 普查：★ 无文件 2757 / **全零 1068** / 有内容 **62**）。★★ **而 LLR dump 是正常的**（★ `±120`、★ 均值 `|LLR|=58`）。
★★★ **机制**：★★ **全零字同时通过 LDPC 校验子（`H·0=0`）与 CRC-24A（全零消息的 CRC 是 0）**
⇒ ★★ **"没收敛/返回零"的译码会被报成 `crc=OK`**（★ 判据在 `pusch_decoder_impl.cpp:477-483`）。

★★★ **影响**：
1. ★★ **深度 3 的"真实数据标签"路线当前不可用**（★ 标签就是 TB）；★ 已加**硬守卫**（★ 全零 TB 直接拒绝）；
2. ★★ **"1153 个 CRC-OK"不能再当作"载荷已知"**；★ 上一会话的合成数据集其**数据 RE 与采集无关**（★ 信道目标不受影响）；
3. ★★ **下一步（★ 谁做谁先看）**：★ 查 metal LDPC 译码路径为什么在收敛前返回零（★ 建议先跑
   `build/lib/phy/upper/coding/ldpc/metal/ldpc_metal_bler_test`，★ 它是一个**知道真值**的 BLER 测试）。

### ★★★ 缺陷二：**`build_clean` 漏了 DM-RS 的 3 dB 功率提升** ⇒ ★★ 解开了 `1/1.43` 之谜

★★ 链的 DM-RS 比数据高 3 dB（★ `sch_dmrs_power.h:20` `{NAN,0,−3,−4.77}`），★ 接收侧用 `scaling = 1.4125` 去掉它；
★★★ **而我们的 `build_clean` 没乘这个幅度** ⇒ ★★ **信道估计整体偏小 `1.4125` 倍**。
★★★ **`1.4125 ≈ 1.43`** ⇒ ★★ **§39.4 记的"链有 `1/1.43` 尺度约定"其实是我们造网格时漏了一个因子**（★ 不是链的怪癖）。
★ 修好后导频一致性比从 `3.7` 变到 **`1.018`** ✓。

---

## ④ ★★★ 下一个会话的第一件事（★ 按顺序）

```bash
cd /Users/jiachengwang/dev/ocudu

# ① 先做 R4.0：训练期的 coded BLER 指标（★ nr_ldpc.py 只有编码器，没有译码器）
#    → 用 nr_ldpc.build_H 写 numpy min-sum 译码器；★ 最终验收仍以【链的真实 LDPC + CRC】为准

# ② 再做 §45.8 的优化：把 LayerScale 折进它前面的 1×1 pointwise 卷积
#    （γ·ReLU(Wx+b) = ReLU((γW)x+γb)，γ>0 用正值参数化）
#    → 预期把残差的 +200 µs 拉回 +20 µs 量级（★ 然后重跑探针确认）

# ③ 然后才是 R4.4 全量训练（★ 两个数据集已就绪，★ 训练/验证按【采集会话】分）
~/ai_ce_work/venv/bin/python lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/train_depth3.py \
  --train doc_chinese/llr_ai_detection/work_tmp/data/datasets/d3_cap001_synth.npz \
  --val   doc_chinese/llr_ai_detection/work_tmp/data/datasets/d3_cap002_synth.npz \
  --out doc_chinese/llr_ai_detection/work_tmp/data/runs/d3_full --epochs 150 --batch 8

# ④ R4.5 标度校准（★ §3.3 的独立步骤）：拟合 {S_llr, A_n, B_n, A_d, B_d, C_H}，★ 验收 = 量化前后 CRC 差异
# ⑤ 之后才是 R5（改接缝）与 R6（上报义务）—— ★ 见 `design` §12.4
```

★★★ **R4.4 的两条硬约束（★ 本轮实测得出，★ 别再踩）**：
1. ★★★ **必须带残差块 + LayerScale**（★ 无残差会塌缩：★ val 停在 `1.277`、★ BER `0.498`）；
2. ★★ **训练用 fp32**（★ fp16 训练会停）；★ **部署的 fp16 由转换决定**。

---

## ⑤ ★★ 未决问题与重开条件

| ★ | ★★ 问题 | ★★ 重开条件 / 下一步 |
|---|---|---|
| ★★★ | ★★ **TB dump 的零值**（★ 缺陷一）| ★★ **阻塞 `real` 源**；★ 先查 metal LDPC 译码路径（★ 见 §③）|
| ★★ | ★★ **`crc=OK` 这个判据本身** | ★★ 需要一条**不会在零字上通过**的判据（★ 例如要求 TB 至少一个非零字节 + CRC 重算）|
| ★★ | ★★ **辅助头的 42 µs**（★ §45.7）| ★ 可接受；★ 若 R5 的预算紧，★ 再考虑折进主干 |
| ★★ | ★★ **残差的 +200 µs**（★ §45.8）| ★★ **有明确优化**（★ 折 LayerScale），★ 见 §④② |
| ★★ | ★★ **coded BLER 未测**（★ R2.4/R4 的判据）| ★★ 需要 R4.0 的译码器，★ 或跑链 |
| ★ | ★ **真实损伤（CFO/时偏/干扰）不在合成数据里** | ★★ 这是 `real` 源的独占价值 ⇒ ★★ **缺陷一值得优先修** |

---

## ⑥ ★★ 纪律（★ 本会话新增两条）

| # | ★★ 纪律 | ★★ 出处 |
|---|---|---|
| ★★★ **F15（新）** | ★★★ **时延数字必须在【机器空闲】时测，★ 且至少两遍** | ★ 本轮第一次测残差头得到 `+315 µs`，★ **真值是 `+42 µs`** —— 后台在跑数据集构建 |
| ★★★ **F16（新）** | ★★★ **数据集的"自检"必须能【证伪标签】**，★ 不能只查形状/范围 | ★★ 本轮两个缺陷**都是**"把比特重新调制、穿过信道、与网格相减"这一步抓到的 |
| ★★ **F14（上一会话）** | ★★ 实现网络前**逐字核对 I/O 契约** | ★ 本会话 R1 正是执行它 |
| ★★ **F13** | ★★ 看起来像尺度问题，**先怀疑自己的度量** | ★★ 本轮复现：★ 自检先报 `0.289`，★ 最后发现是**我的自检**把未归一化的 `H` 又乘了一次 `r` |

---

## ⑦ 常用命令

```bash
W=doc_chinese/llr_ai_detection/work_tmp
P=lib/phy/upper/signal_processors/channel_estimator/metal/ai_train
PY=~/ai_ce_work/venv/bin/python

# ★★ 三个自检（★ 改动契约/损失/数据后必须全过）
$PY $P/depth3.py --selfcheck
$PY $P/depth3_loss.py --selfcheck
$PY $P/build_depth3_dataset.py --selfcheck --out $W/data/datasets/d3_cap001_synth.npz

# ★★ 重建数据集（★ 两个语料各一条）
$PY $P/build_depth3_dataset.py --units $W/data/tb_corpus/index.csv --tb $W/data/tb_corpus/tb \
  --corpus $W/corpus/aillr_cap001 --out $W/data/datasets --name d3_cap001_synth \
  --source synthetic --modulation 256QAM --snrs 12,18,24,30,36,42,48

# ★★★ 训练（★ 小样先验证；★ 机器要空）
$PY $P/train_depth3.py --train $W/data/datasets/d3_cap001_synth.npz \
  --val $W/data/datasets/d3_cap002_synth.npz --out $W/data/runs/d3_smoke \
  --epochs 25 --batch 8 --max-train 300 --max-val 48

# ★★★ ANE 复验（★ 建图 + 编译 + placement + p50；★ 必须两遍、★ 机器空闲）
cd $W/probe && $PY build_depth3_probe.py [--residual] --out m.mlpackage && ./probe m.mlpackage 612 300

# ★★ TB dump 复现（★ 缺陷一）
mkdir -p /tmp/tbd && OCUDU_HELENA_DUMP_DIR=/tmp/tbd \
  ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay $W/corpus/aillr_cap002/C_1147_17921 \
  --metal --out /tmp/stage_x
```

★ **提交纪律**：★ `git commit -F <文件>`（★ 消息文件放 `work_tmp/tmp/`，★ **不要放 `/tmp`**）。

---

## ⑧ ★★ 本会话的提交

| ★ commit | ★★ 内容 |
|---|---|
| `8be50bf1b3` | ★★★ **R1：契约冻结 + `depth3.py`**（★ 实作时抓到 `scaling=1.4125` 与 LS 去加重两处）|
| `3ddc3bdb86` | ★★ **R2：`depth3_loss.py`** |
| `b8b49407a6` | ★★★ **R3：数据集 + 两个真缺陷** |
| `bf6da6d8c6` | ★★ ANE 复验（★ 四头契约在 ANE 上）+ R3 全量（★ 3752/3437）|
| `c0b043154a` | ★★ **R4 开工**：训练脚本 + 塌缩的复现与修复 + 残差结构的 ANE 成本 |

---

## ⑨ ★★★ 同一会话第 2 轮补充（★ 2026-10-13 下午）

> ★★ 第 1 轮做完 R1–R3 并开了 R4；★★ **第 2 轮把 R4 的【判据】和【参照系】补齐**，
> ★★★ **并因此又抓到两个真错**（★ 一个在译码器里，★ 一个在基线里）。

### ★★★ 本轮新增的可复用件

| ★ | ★★ 件 | ★★★ 说明 |
|---|---|---|
| ★★★ **R4.0 译码器** | ★ `ai_train/ldpc_decode.py`（★ `--selfcheck`）| ★★ **归一化 min-sum + 接收侧逆链**（★ 解扰/解交织/速率解匹配）⇒ ★★ **coded BLER 终于可测** |
| ★★★ **参照系** | ★ `ai_train/eval_depth3.py` | ★★ **网络 / genie 信道 / 完美 LLR** 三方对照（★ §2.5 消融 4 的上界）|
| ★★ **折叠 LayerScale** | ★ `train_depth3.py::FoldedScaleConv` | ★★ 残差从 **+160 µs 降到 ~0**（★ 365 µs vs 不折叠 529 µs，★ 与无残差打平）|
| ★★ **rv=0 数据集** | ★ `work_tmp/data/datasets/d3_{cap001,cap002}_rv0.npz` | ★★ 3752 / 3437 样本（★ 见下面的 rv 发现）|

### ★★★ 本轮抓到的两个真错（★ 都"看起来在工作"）

| ★ # | ★★ 错 | ★★★ 为什么危险 / 怎么发现 |
|---|---|---|
| ★★★ **1** | ★★★ **min-sum 的变量→校验消息用错了求和**（★ 用了"同一校验的其它边"，★ 应为"该变量的其它校验"）| ★★ 判决在第 1 次迭代后**完全冻结**，★ 而**自检仍然通过** —— ★★ 因为自检用的码率太低，"信道本身就是答案" ⇒ ★★ **自检工况必须贴近部署工况（★ 码率、块数、rv）** |
| ★★★ **2** | ★★★ **genie 基线的星座标号我自己推错了** ⇒ ★★ **BER 0.56（★ 比掷硬币还差）** | ★★ "知道信道的接收机不可能比掷硬币差" ⇒ ★★ **是基线错，不是网络好**。★ 修法：★★ **标号直接由 `dd_label.modulate` 生成** |

### ★★★ rv 的发现（★ 与 §12.8 的零 TB 缺陷【互为佐证】）

★★ 传输里**系统比特占比**：★ **rv=0 → 0.79**、★ rv=3 → 0.58、★ **rv=1 → 0.078**、★ **rv=2 → 0.000**。
★★★ **rv=1/2 是 HARQ 重传**，★ 单时隙回放**没有软缓冲历史** ⇒ ★★ **本来就不可能单独译出**，
★ 而链却报了 `crc=OK` ⇒ ★★★ **这就是 §12.8 的"全零字通过"机制**（★ **两个独立证据指向同一件事**）。
★★ **处置**：★ 合成源固定 **rv=0**；★ BLER 指标**跳过并计数** `systematic_share < 0.2` 的块。

### ★★★ 判据结论

| ★ 判据 | ★★ 结果 |
|---|---|
| ★★★ **R2.4（★ 辅助头不拖累主头）** | ★★★ **成立**：★ 带辅助头 val 0.49909 / uBER 0.3160，★ 去掉后 0.49813 / 0.3149 ⇒ ★★ 差异在同种子噪声内（★ 见 §12.13）。★ **但两者 cBLER 都是 1.0** ⇒ ★ 判据在当前训练量下还分辨不出好坏 |
| ★★★ **coded BLER 指标本身** | ★★★ **可信**：★ 完美 LLR → `0.0000`；★ genie → uBER 0.0546 / cBLER 0.3333；★ 未训练网络 → 0.4999 / 1.0 |

### ★★★ 下一步（★ 更新）

1. ★★★ **等 R4.4 全量训练跑完**（★ `work_tmp/data/runs/d3_full/`，★ 150 epoch、★ 3752 训练样本），
   ★★ 用 `eval_depth3.py` 出**三方对照**（★ 网络 vs genie）；
2. ★★ **R4.5 标度校准**（★ §3.3）：★ 拟合 `{S_llr, A_n, B_n, A_d, B_d, C_H}`，★ 验收 = **量化前后 CRC 差异**；
3. ★★ **R5 改接缝**：★ `ai_arm::demodulate()` **停止转发经典解调器**；★ 引擎要扩成
   **fp16 输入 `[1,612,1,64]` + 四个输出**（★ 见 §0.2(e)）；★ **自己做解扰**（★ §0.2(g)）；
4. ★ **R6 上报义务**。

★ **仍在飞的作业**：★ `d3_full`（★ 后台）。
