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
| `<ds>` | ★ **训练语料** | `w2`（166 495）｜`e17`（99 097）｜`e17x40k`（43 523）｜`s2x8k`（8k 抽取）｜`e4`（旧 43 902）|
| `c<C>` | ★ **宽度**（通道数）| `c32`｜`c128`｜`c256`｜`c512` |
| `b<B>` | ★ **块数** | `b12`｜`b18` |
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

**⇒ 不进名字、进 sidecar 的**：`batch`（8/16）、`workers`、`max-val`、`epochs`（总计划）、`snr-range`、`layout`、`nsc`。

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
