# depth3 模型命名约定（2026-11-06 制定）

> **动机**：现在同时存在三套命名（`w2c32_b18_e60`、`c128_e17_40k`、`w2_cond_c128_b18_e9`），
> 而 `c128_e17_40k` 里的 `40k` **既是语料规模又像轮数** —— 光看名字分不清 ✓
> **目标**：名字本身说明"**什么数据、多大网络、什么配方、第几轮**"，其余细节交给**同名 sidecar** ✓

---

## 1. 名字格式

```
d3_<ds>_c<C>b<B>_<recipe>_e<EP>[best]
```

| 段 | 含义 | 取值 |
|---|---|---|
| `d3` | 家族前缀（depth3）| 固定 |
| `<ds>` | ★ **训练语料** | `w2`（166 495）｜★ `w2v3c`（**183 624** = w2v1 全量 ＋ s2_amc 2PRB 补集，**不含 1102a**）｜`e17`（99 097）｜`e17x40k`（43 523）｜`s2x8k`（8k 抽取）｜`e4`（旧 43 902）|
| `c<C>` | ★ **宽度**（通道数）| `c32`｜`c128`｜`c256`｜`c512` |
| `b<B>` | ★ **块数** | `b12`｜`b18`｜★ `b36`（2026-11-24 起的深网络，见 §10）|
| `<recipe>` | ★ **配方标签**，`-` 连接 | 见 §2 |
| `e<EP>` | ★ **快照所处的训练轮**（**不是**总计划轮数）| `e9`｜`e33`｜`e60` |
| `best` | ★★ **仅当该快照是 val 选出的 best 时追加**（用户 2026-11-06 指示）| `e33best`｜`e6best` |

**⇒ ★★★ 为什么必须区分**：`best` 是**按 val 选的**，而 val 选轮在尺子上已被证伪三次（配方 memo §6）⇒
`e6best` 与 `e9`（同一 run 的第 9 轮）**是两个不同的东西**，名字上必须一眼看出来 ✓

**⇒ 例**：
```
d3_w2_c32b18_e2e-cond-norep_e60        ← 现在的 w2c32_b18_e60（第 60 轮快照，非 best）
d3_w2_c128b18_e2e-cond-norep_e33best   ← w2c128_e2e 的 best（val 0.094690 @33）
d3_w2_c128b18_e2e-cond-norep_e40       ← 同一个 run 的 last（第 40 轮）
d3_e17x40k_c128b18_2st-cond_e9best     ← 现在的 c128_e17_40k（best @9）
d3_w2_c128b18_2st-cond_e6best          ← 现在的 w2_cond_c128_b18（best @6）
d3_w2_c128b18_2st-cond_e9              ← 现在的 w2_cond_c128_b18_e9（同 run 的第 9 轮）
```

---

## 2. 配方标签（这些**确实**在变，不是"只有一种配方"）

| 标签 | 含义 | 反例（证明它是变量）|
|---|---|---|
| `e2e` | 从零端到端训练 | `w2c128_e2e` |
| `2st` | **两段式**（先预训再微调）| `w2_cond_c128_b18`：两段式 +7.93%，同宽端到端是 −（见 §4）|
| `ft@<src>` | 从某 run `--init` 暖启动 | `w2c32_b18_e61`（从 e21 的 `depth3_last`）|
| `cond` | **调制条件化**（一个 bundle 算 4 种调制）| 与 per-modulation bundle 并列存在 |
| `norep` | **无 reporting heads** | 近期 run 全部 `--no-reporting-heads` ✓ |
| `lr<v>` | 学习率**非** 1e-3 时才写 | 默认 1e-3 省略 |
| `s<seed>` | 种子**非** 20261105 时才写 | 默认省略 |
| `pat<n>` | 早停耐心**非** 0 时才写 | `pat3`（早期）／省略＝不早停 |
| ★ `k<n>` | ★ **主干空间核边长非 3 时才写**（architecture_levers L2 的大核）| `k5`／`k7`；省略＝3 |
| ★ `g<n>` | ★ **主干卷积分组数非 1 时才写**（architecture_levers L3）| `g4`（g=32 即 depthwise，`dw`）；省略＝1（dense）|
| ★ `nzllr` | ★ **噪声头回灌给解调器**（architecture_levers L6）| 省略＝噪声头只作旁支输出 |
| ★ `dense` | ★ **主干是 dense（全通道混合）时必须写**，`dw` 反之 | 2026-11-16 用户要求：`c32b18` 之后直接写 `dense`，不要靠参数数量猜 |
| ★ `ceh` | ★ **训练期信道精化头**（导出时丢弃）| 2026-11-16 起 |
| ★ `llrp` | ★ **LLR 平面残差后处理**（lever B，~104 参数）| 2026-11-18 起 |
| ★ `cst` | ★ **星座感知头**（lever A：解析 4·s/σ² ＋ 残差）| 2026-11-18 起 |

**⇒ 不进名字、进 sidecar 的**：`batch`（8/16）、`workers`、`max-val`、`epochs`（总计划）、`snr-range`、`layout`、`nsc`。

**⇒ ★★★ 2026-11-16 之后新增的架构 token 与续训后缀，统一补在 §10**（`dskip`／`dskip-entry`／`se`／`mlp2`／
`w<max>`／`suminit`／`cliff`／`g<n>`／`symaux`，以及 `_e<EP>_decay`）—— 看 §2 这张表时**必须同时看 §10** ✓

**⇒ 为什么 batch 不进名字**：它是**显存约束**（6 GB 上 c128 只能 batch 12），不是配方意图 ✓

---

## 3. Sidecar：`model.json`（每个 bundle 目录必带）

```json
{
  "name": "d3_w2_c32b18_e2e-cond-norep_e60",
  "aliases": ["w2c32_b18_e60"],
  "dataset": {"train": "datasets_train_w2", "train_samples": 166495,
              "val": "datasets_monitor_w2", "val_samples": 731},
  "arch": {"c_hid": 32, "blocks": 18, "cond": true, "reporting_heads": false,
           "layout": "spatial", "nsc": 612, "plane_channels": 16},
  "train": {"mode": "e2e", "init_from": null, "batch": 16, "lr": 1e-3,
            "epochs_planned": 60, "epochs_run": 60, "snapshot_epoch": 60, "is_best": false,
            "early_stop_patience": 0, "seed": 20261105, "gpu": "RTX 3060"},
  "export": {"snr_range": [-0.18, 34.70], "modulation_arg": "QPSK", "quantisation": "fp16"},
  "artifact": {"mlmodelc_sha256": "<sha>", "keras_sha256": "<sha>", "bytes": 1605771},
  "measured": {
    "probe500_npu": {"bits": 999200, "vs_cpu": -0.131, "date": "2026-11-06"},
    "ruler5000_npu": {"bits": 10214096, "vs_cpu": -0.1168, "decoder": "generic",
                      "host": "mac|203", "bin_sha": "3b66bf45"}
  }
}
```

**⇒ ★★ 规则**：
1. **name 与目录名一致**；`aliases` 保留旧名，**永远不删**（历史文档引用的就是旧名）✓
2. `measured` **只增不改**：每次测一臂追加一项（含日期/主机/`BIN_SHA`/解码器）✓
3. `artifact.mlmodelc_sha256` 必须与测量记录里的 `BUNDLE_FILES` 对得上 ✓

---

## 4. 现有 bundle → 规范名（迁移表）

| 现名 | 规范名 | 语料 | 备注 |
|---|---|---|---|
| `w2c32_b18_e13/e20/e40/e46/e60` | `d3_w2_c32b18_e2e-cond-norep_e<EP>` | w2 | e2e 续训各轮 |
| `w2c128_e2e`（best/last）| `d3_w2_c128b18_e2e-cond-norep_e33best` / `_e40` | w2 | 40 轮到 02:48 完成 |
| `w2_cond_c128_b18` | `d3_w2_c128b18_2st-cond_e6best` | w2 | 两段式 |
| `w2_cond_c128_b18_e9` | `d3_w2_c128b18_2st-cond_e9` | w2 | 两段式 |
| `c128_e17` | `d3_e17_c128b18_2st-cond_e5best` | e17 | best @5 |
| `c128_e17_40k` | `d3_e17x40k_c128b18_2st-cond_e9best` | e17 40k | gpu 口径最强（+14.44%）；npu 口径 −10.0% |
| `d3_e4`（构建默认）| `d3_e4_c32b18_2st-cond_e40` | 旧 w2+HK | 有 attach 数据（无 reporting heads）|

**⇒ ★★ 注意 `c128_e17` vs `c128_e17_40k`**：两者**只差语料**（99k vs 40k），
结果却是 **−3.69% vs +14.44%** ⇒ **语料是最大的一个变量**，名字里必须能看出来 ✓（旧名看不出来 ✗）

---

## 4.1 ★★★ sidecar 不用手工编 —— **训练器把 `argv:` 写进日志了**

**⇒ 实测**：`train_depth3.py` 在每个 run 的日志开头写一行完整命令行，例如
```
argv: --train …/datasets_train_w2 --val …/datasets_monitor_w2 --out …/runs/w2c128_e2e       --modulation QPSK --modcond --no-reporting-heads --c-hid 128 --blocks 18 --epochs 40       --patience 0 --snapshot-every 5 --batch 16 --workers 4 --max-val 1000 --bler-samples 0 --batch 12
```
**⇒ ★★ 所以 `model.json` 的 `train` 段可以**从日志机械生成**，不需要回忆 ✓
**⇒ ★★★ 顺带发现一个**必须记录**的细节**：那行里 `--batch` 出现**两次**（16 然后 12），
**后者生效** ⇒ 实际 batch 是 **12**（因为 6 GB 卡上 batch 16 在反向传播 OOM，脚本用第二个 `--batch 12` 覆盖）✓
—— 若只看配方表（写着 16）就会记错 ✗ **这正是 sidecar 要解决的** ✓

**⇒ 生成方式（建议）**：`make_model_json.py <run 目录> <bundle 目录>` 从 `argv:` ＋ `history.json`
＋ `mlmodelc` 的 sha 生成，人工只补 `measured` ✓

## 5. 使用规则

1. ★ **一个新 artifact 一个新名**；改名必须写进 §4 的迁移表，`aliases` 保留旧名；
2. ★ 报数时写：**规范名 ＋ `mlmodelc` sha ＋ 口径（npu/gpu）＋ 解码器 ＋ 单元集**；
3. ★ **`e<EP>` 是快照轮**，不是 `epochs_planned`；写论文/报告时用 sidecar 的 `epochs_run`；
4. ★ 侧车**随 bundle 一起拷**（`scp -r <bundle> <host>:`），否则换机后就只剩一个光秃秃的目录 ✗

---

## 7. ★★★★ 2026-11-06 17:00 **新增"架构"段：`c<C>b<B><arch>`**

**⇒ 动机**：dense / 瓶颈 dense 现在是**真实变量**（参数差 7×），而 §1 的格式里没有它的位置，
   两条只差架构的 run 会得到**同样的名字** ✗

**⇒ 规则**：在 `b<B>` 后**紧跟架构标记**（省略＝depthwise，保持旧名不变）：

| 标记 | 含义 | c32/b18 参数 |
|---|---|---|
| （省略）| depthwise 3×3 | **26 185** |
| `dense` | 全 dense 3×3 | **186 889** |
| `bn<N>` | 瓶颈 dense（宽 `c/N`）| bn2 **62 473**｜bn4 **21 865** |

**⇒ 例（2026-11-06 的三个 2×2 训练）**：

| 旧目录名 | ★ 规范名 |
|---|---|
| `w2v1c32d18_e20`（107）| ★ **`d3_w2v1_c32b18dw_e2e-cond-norep_e20`** |
| `densew2c32_b18_e10`（211）| ★ **`d3_w2_c32b18dense_e2e-cond-norep_e10`** |
| `w2v1dense32_b18_e10`（66）| ★ **`d3_w2v1_c32b18dense_e2e-cond-norep_e10`** |

**⇒ 说明**：`dw` 显式写出来（用户指定），与"省略＝dw"并不冲突 —— **新 run 写 `dw`，旧名不动** ✓

**⇒ ★★ 附带规则（训练中的 run 改名）**：`rename_run.sh` 用 **`mv` ＋ 符号链接**，
   让**正在写快照的进程**继续把数据写进新目录 ✓；`model.json` 从日志里**最后一条** `argv:` 生成
   （日志会累积所有尝试，第一条往往是失败的启动）✓

---

## 8. ★★★★★ 2026-11-07 07:20 **把"非默认值必须进名字"写成**通则**（此前只是零散地写在个别 token 上）

**(a) ★★ 现状（此前的问题）**

**⇒ §2 的表格里，`lr<v>`／`s<seed>`／`pat<n>` **各自**写了"非默认才写"，
   但**没有一条通则**；而且 **`--scale-init` 根本没进表** ⇒
   `d3_w2v1_c32b18dense_sc003_…` 这个名字里的 `sc003` **在约定里找不到依据** ✗

**(b) ★★★ 通则（今后按此判断）**

> **任何训练参数，只要**偏离训练器的默认值**，就必须在 `<recipe>` 段里出现；
> **等于默认值的，一律省略**。**

**⇒ 目的**：**名字能唯一确定"这个模型是怎么训出来的"**（在默认值已知的前提下）✓
**⇒ 因此训练器默认值一旦改变，旧名字的含义也会变 ⇒ 默认值必须随训练器版本一起记录在 `model.json` 里** ✓

**(c) ★★★ 当前默认值表（`train_depth3.py`，2026-11-07）**

| 参数 | 默认 | 非默认时的 token |
|---|---|---|
| `--lr` | **1e-3** | `lr<v>`（如 `lr3e-4`）|
| `--seed` | **20261105** | `s<seed>` |
| `--patience` | **0**（不早停）| `pat<n>` |
| ★ `--scale-init` | **1.0** | ★ **`sc<v>`**（如 `sc003` = 0.03、`sc01` = 0.1）|
| `--blocks` | 18 | 进 `b<B>` 段 |
| `--c-hid` | 32 | 进 `c<C>` 段 |
| `--dense-blocks` | 关 | 进架构段（`dense`）|
| `--dense-bottleneck` | 0 | 进架构段（`bn<N>`）|

**(d) ★★ 仍**不进**名字、只进 `model.json` 的**

**⇒ `batch`（显存约束）、`workers`、`max-val`、`epochs`（总计划）、`snr-range`、`layout`、`nsc`** ✓

**(e) ★★★ 判据（怎么用这张表）**

**⇒ 看到一个名字 ⇒ 能反推出**所有非默认参数**；配合 `model.json` ⇒ **能完整复原训练命令** ✓
**⇒ 反例（教训）**：`w2c32_b18_e61` 里**看不出**它是"从 `--init` 暖启动、lr 仍是 1e-3" ⇒
   §2 早已要求 `ft@<src>`，但当时没遵守 ⇒ **这正是要通则的原因** ✓

---

## 9. ★★★★ 2026-11-07 19:20 **补两个 token：`sw<w>s<s>`（SNR 加权）与 `sc<v>` 的确认（`--scale-init`）**

**(a) ★★★ 背景**

`train_depth3.py` 在 2026-11-07 加了两个训练期旋钮：

| 参数 | 默认 | 含义 |
|---|---|---|
| `--snr-weight` | **0**（关）| 把难组（按 SNR 分带）的损失放大 `w` 倍 |
| `--snr-sigma` | 3.0 | 加权的 SNR 高斯带宽（dB）|

⇒ 按 §8 的通则，**只要 `--snr-weight > 0` 就是非默认** ⇒ 必须进 `<recipe>` ✓

**(b) ★★★ token 规则**

| 参数组合 | token | 例 |
|---|---|---|
| `--snr-weight w --snr-sigma s`（s 非 3.0）| ★ **`sw<w>s<s>`** | `sw2s3` ＝ weight 2、sigma 3（两者都显式）|
| ★ `--loss-norm sample` | ★ **`lns`** | 2026-11-09 新增：LLR 项按**每个接收等权**平均（默认 `bits` 按比特等权，不写 token）|
| ★ `--loss-norm sample` 且是「同一 case 的修版」| ★ **`_fix`**（后缀，放在 `_e<EP>` 之前）| `…-sw2s3_fix_e40` ＝ 与 `…-sw2s3_e20` 同一配方、只改了损失归一化；比 `lns` 更好认，**正式使用这一种** |
| ★ `--loss-norm re`（每个解调判决一票）| ★ **`lre`** | 2026-11-10 新增：按 `(子载波×符号)` 数加权，去掉"256QAM 每 RE 拿 4 倍梯度"的调制偏置 |
| `--snr-weight w`（sigma 保持 3.0）| ★ **`sw<w>`** | `sw2` |
| `--snr-weight 0` | **不写**（默认关）| — |

**⇒ 位置**：`<recipe>` 段的**末尾**（在 `cond`/`norep` 之后），因为它属于"微调旋钮"而不是"训练模式" ✓

**⇒ ★★ `<recipe>` 的完整顺序（以此为准，已有名字都符合）**：

```
d3_<ds>_c<C>b<B><arch>_[sc<v>_]  <mode>[-cond][-norep][-sw<w>s<s>][-lr<v>][-s<seed>][-pat<n>]  _e<EP>[best]
                        ↑ 紧跟架构段        ↑ 模式在最前                        ↑ 旋钮依次在后
```

**⇒ 例**：`d3_w2v1_c32b18dw_e2e-cond-norep-sw2s3_e20`（w2v1、c32/b18/dw、端到端、调制条件化、无 reporting heads、SNR 加权 2 dB 高斯 σ=3、第 20 轮）✓

**(c) ★★ 同时确认 `sc<v>`（此前 §8 只是新增，未有实例进表）**

| 参数 | 默认 | token | 例 |
|---|---|---|---|
| `--scale-init` | **1.0** | ★ **`sc<v>`**（去掉小数点前导 0；**紧跟架构段之后**）| `sc003` = 0.03、`sc01` = 0.1 |

**⇒ 例**：`d3_w2v1_c32b18dense_sc003_e2e-cond-norep_e10`（dense 主干必须配 `sc003`，否则 e10 就崩，见网络改进计划 §25.3）✓

**(c)bis ★★★ `dil<cycle>`：2026-11-07 新增 `--dilations`，非默认时必须写出来**

| 参数 | 默认 | token |
|---|---|---|
| `--dilations` | **`1,2,4,8`**（RF 127 / 612）| ★ **`dil<cycle>`**，紧跟架构段（与 `sc<v>` 同位置）|

**⇒ 例**：`d3_w2v1_c32b18dw_dil12481632_e2e-cond-norep_e20` ＝ c32/b18/dw、dilation 循环 1,2,4,8,16,32（RF **379**）✓
**⇒ 为什么必须写**：它**不加参数、不加 MAC**，两条 run 的参数与时延可以完全相同而**感受野差 3 倍** ——
   名字里不写就再也分不出来（这正是 §8 通则要防的那类事）✓

**(d) ★ 由此产生的两个正式名字（已写进 `depth3_gate_and_model_pk_npu.md` §4.2）**

```
d3_w2v1_c32b18dense_sc003_e2e-cond-norep_e10
d3_w2v1_c32b18dw_e2e-cond-norep-sw2s3_e20
```

---

## 10. ★★★★ 2026-11-27 **补齐 2026-11-16 之后实际在用的 token（C/e 线）＋ 续训的 `_decay` 后缀**

**(a) ★★ 背景：runs/ 里已经用了 40+ 次，文档只写了其中五个**

⇒ §2 只登记了 `nzllr`／`dense`／`ceh`／`llrp`／`cst`，但 107／211 的 `runs/` 里
`dskip`、`dskip-entry`、`se`、`mlp2`、`w8`、`suminit`、`cliff`、`g4`、`symaux` 都在用，
且续训产生了 `_decay`／`_c80`／`e32cont` 三种后缀 ⇒ **光看名字已经无法反推 argv** ✗
本节按 §8 的通则补齐，**每个 token 都给一条真实 argv 作依据**（可在对应机器 `logs/*.log` 的 `argv:` 行核对）。

**(b) ★★★ 架构 token（★ 位置：**`<recipe>` 段的末尾**，即 `cond`／`norep`／`sw<w>s<s>` 之后；
`dense`／`dw`／`bn<N>`／`sc<v>`／`dil` 仍留在紧跟 `b<B>` 的架构段，见 §7／§9）**

| token | 对应 flag | 含义 | 参数（c32b18，实测）| 真实例（机器）|
|---|---|---|---|---|
| ★ `dskip` | `--dense-skip` | 每块的残差和换成**跨块 dense 连接**（拼接 state＋本块输出，1×1 投回 C）| **224 905**（dense 186 889 之上 +38 016）| `…-sw2s3-dskip_e40`（107）|
| ★ `dskip-entry` | `--dense-skip-entry` | 在 dskip 之上再把**入口特征**（接收信号＋Qm 条件，非线性之前）喂给每块；**隐含 dskip**，每块多一个 C→C 1×1 | **243 337**（dskip 之上 +18 432）；c32**b36** ＝ **485 833** | `…-sw2s3-dskip-entry_e80`（211）|
| ★ `se` | `--se` | 每块 **squeeze-excite** 通道注意力（lever B），不加逐元素 MAC | ★ **+292/块**（b18 +5 256 → 248 593；b36 +10 512 → 496 345）| `…-dskip-entry-se_e40`（107）|
| ★ `mlp2` | `--mlp-expand 2` | 点卷积块内升维 2×（多一个 1×1 ＋ ReLU）| 249 745（与 se 同时；mlp2 自身 +57 600）| `…-mlp2-se_e40`（107）|
| ★ `w<max>` | `--width-weight 1.0 --width-max <max>` | 沿**宽度（PRB）轴**的第二维损失加权（§92，负结果）| 0 | `…-dskip-entry-se_w8_e40`（107）|
| ★ `suminit` | `--dense-skip-init sum` | dense-skip 投影初始化为**相加解**（§86b，−0.22 pp，已关闭）| 0 | `…-dskip-entry-se-suminit_e40`（§80）|
| ★ `cliff` | `--snr-weight 6 --snr-sigma 1.5 --snr-center 25.5` | 只压 256QAM 悬崖的**窄带**加权（§50）| 0 | `…-sc003_cliff_e40`（107）|
| ★ `g<n>` | `--groups n` | 主干卷积**分组**数（g=4；省略＝dense）| 少 | `…-g4_e40`（107）|
| ★ `symaux` | `--sym-aux --w-sym 0.1` | **训练期**符号回归辅助头（导出丢弃）| 0（导出）| `…-llrp-symaux_e40`（107）|

**(b)bis ★★★ 损失项 token（2026-11-27 起）：`code<λ>`**

| token | 对应 flag | 含义 | 例 |
|---|---|---|---|
| ★ `code<λ>` | `--code-loss-weight λ`（＋可选的 `--code-loss-iterations k`、`--code-loss-from e`）| ★ **码字约束损失**（§93 #1）：训练时在真 LDPC 码上跑 k 次 min-sum，按解码后仍错的比特计 BCE。λ 按 `lr3e-4` 的写法（0.1 → `code1e-1`）| `…-se-code1e-1_e40` |

**⇒ 位置**：`<recipe>` 段**最末尾**（它是损失项，不是结构）。**迭代数不进名字**（默认 2），进 sidecar/argv ✓

**⇒ 保留 token（还没人用过，但一旦偏离默认就必须写）：**
`seR<n>`（`--se-ratio` 非 8）、`dskipK<n>`（`--dense-skip-depth` >1）、`mlp<r>`（`--mlp-expand` 非 1/2）、
`ww<v>`（`--width-weight` 非 1.0）、`wch<v>`（`--w-ch`）、`wsym<v>`（`--w-sym`）。

**(c) ★★★ 续训命名：`_e<最终轮>` ＋ 可选 `_decay`（本节为**正式规则**）**

> **格式**：`<§1 的完整名字>_e<本次续训写出的最后一个轮次>[_decay]`

| 段 | 含义 |
|---|---|
| ★ `e<EP>` | ★ **本次 run 写出的最后一个快照的轮次**（不是父 run 的轮次，也不是计划轮数）|
| ★ `_decay` | ★ **本段续训用 cosine 衰减**：`--lr-decay cosine --lr-min <v> --lr-decay-from <本段第一轮>`；**只衰减这一段** |
| ★ `_decay<first>` | ★ 衰减**起点不是常见的 81** 时（不是"80→120 续训"那一段）把起点写出来：`…_e120_decay41` = **e41 起**衰减到 e120 |
| 无 `_decay` | 本段用**恒定 LR**（＝父配方原样 + `--epochs`）|

**⇒ 为什么 `_decay` 放在 `_e<EP>` 之后**：它不是训练旋钮，而是**这一段的性质**（哪一段衰减、衰减到哪一轮），
和 `best` 一样属于"快照段"的修饰语 ✓

**⇒ 真实例（本次，2026-11-27）**：
```
父：d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e80            （211，恒定 LR，80 轮）
子：d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e120_decay     （e81–e120，cosine 1e-3→1e-4）
```

**(d) ★★ 历史后缀（**不要再用于新 run**，只在追溯旧目录时认得）**

| 旧后缀 | 实际含义 | 例 |
|---|---|---|
| `_e<start>_c<end>` | 从父 run 的 `e<start>` 续到 `e<end>`（**不含衰减**）| `…-se_e40_c80`（107，e41→e80）|
| `_e<start>cont` | 从 `e<start>` 续训（计划更长）| `…_s20261106_e32cont`（epochs 72）|
| `-e<n>decay` | 在**同一条 run 内部**从第 n 轮开始衰减（不是续训）| `…-mlp2-se-e30decay_e40`（`--lr-decay-from 30`）|

**(e) ★★ `<recipe>` 的完整顺序（2026-11-27 版，以此为准）**

```
d3_<ds>_c<C>b<B><arch：dense|dw|bn<N>>[sc<v>][dil<cycle>]
   _<mode：e2e|2st|ft@<src>>[-cond][-norep][-sw<w>s<s>]
   [-dskip[-entry]][-se][-mlp<r>][-g<n>][-w<max>][-nzllr][-ceh][-llrp][-symaux]
   _e<EP>[best][_decay]
```

**⇒ ★ 注意两处容易写错的地方**：
1. **`dense` 在架构段（`b36dense`），而 `dskip`／`se`／`mlp2` 等在新 lever 段（末尾）** ——
   历史名字 `…-sw2s3-dskip-entry-se_e40` 就是这个顺序；`mlp2-se-dskip`／`mlp2-se` 之间的次序没有硬性规定，
   但**同类 token 必须出现**（否则无法反推 argv）✓
2. **`sc<v>`／`dil` 紧跟架构段**（§9），**不要**把它们挪到末尾 ✓

**⇒ 自检三步**（写完名字后逐条对）：
1. **架构**：块数/宽度/主干类型（dense/dskip/se/mlp）写了没有？看参数数量能否反推（c32b18dense＝186 889、＋dskip-entry＝243 337、＋se＝248 593；c32b36＋dskip-entry＝485 833、＋se＝496 345）✓
2. **配方**：`sw`/`lr`/`s`/`pat` 里凡是**偏离默认**的写了没有？（默认表见 §8(c)）✓
3. **轮次**：`e<EP>` 是**本次最后一个快照**的轮次；衰减续训再加 `_decay` ✓

**(f) ★ 由本节新增的正式名字**

```
d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e80
d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e120_decay
d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry-se_e120_decay
d3_w2v3c_c32b18dense_e2e-cond-norep-sw2s3-dskip-entry-se_e120_decay41
```
（第三条在 107 上跑，`--se` 使参数为 **496 345**；第一条 211 上是 **485 833** —— 两者**只差 SE**。
第四条是 2026-11-27 增补的**长衰减**：18 块 ＋ SE，父 run 是 126 上 CeB 的 **e40 快照**，`--lr-decay-from 41` ⇒ 衰减跨 **79 轮**。）
