# depth3 模型命名约定（2026-11-06 制定）

> **动机**：现在同时存在三套命名（`w2c32_b18_e60`、`c128_e17_40k`、`w2_cond_c128_b18_e9`），
> 而 `c128_e17_40k` 里的 `40k` **既是语料规模又像轮数** —— 光看名字分不清 ✓
> **目标**：名字本身说明"**什么数据、多大网络、什么配方、第几轮**"，其余细节交给**同名 sidecar** ✓

---

## 1. 名字格式

```
d3_<ds>_c<C>b<B>_<recipe>_e<EP>
```

| 段 | 含义 | 取值 |
|---|---|---|
| `d3` | 家族前缀（depth3）| 固定 |
| `<ds>` | ★ **训练语料** | `w2`（166 495）｜`e17`（99 097）｜`e17x40k`（43 523）｜`s2x8k`（8k 抽取）｜`e4`（旧 43 902）|
| `c<C>` | ★ **宽度**（通道数）| `c32`｜`c128`｜`c256`｜`c512` |
| `b<B>` | ★ **块数** | `b12`｜`b18` |
| `<recipe>` | ★ **配方标签**，`-` 连接 | 见 §2 |
| `e<EP>` | ★ **快照所处的训练轮**（**不是**总计划轮数）| `e9`｜`e33`｜`e60` |

**⇒ 例**：
```
d3_w2_c32b18_e2e-cond-norep_e60      ← 现在的 w2c32_b18_e60
d3_w2_c128b18_e2e-cond-norep_e33     ← w2c128_e2e 的 best（val 0.094690 @33）
d3_w2_c128b18_e2e-cond-norep_e40     ← 同一个 run 的 last
d3_e17x40k_c128b18_2st-cond_e9       ← 现在的 c128_e17_40k（best @9）
d3_w2_c128b18_2st-cond_e6            ← 现在的 w2_cond_c128_b18（best @6）
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
            "epochs_planned": 60, "epochs_run": 60, "snapshot_epoch": 60,
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
| `w2c128_e2e`（best/last）| `d3_w2_c128b18_e2e-cond-norep_e33` / `_e40` | w2 | 40 轮到 02:48 完成 |
| `w2_cond_c128_b18` | `d3_w2_c128b18_2st-cond_e6` | w2 | 两段式 |
| `w2_cond_c128_b18_e9` | `d3_w2_c128b18_2st-cond_e9` | w2 | 两段式 |
| `c128_e17` | `d3_e17_c128b18_2st-cond_e5` | e17 | best @5 |
| `c128_e17_40k` | `d3_e17x40k_c128b18_2st-cond_e9` | e17 40k | **最强（+14.44%）** |
| `d3_e4`（构建默认）| `d3_e4_c32b18_2st-cond_e40` | 旧 w2+HK | 有 attach 数据 |

**⇒ ★★ 注意 `c128_e17` vs `c128_e17_40k`**：两者**只差语料**（99k vs 40k），
结果却是 **−3.69% vs +14.44%** ⇒ **语料是最大的一个变量**，名字里必须能看出来 ✓（旧名看不出来 ✗）

---

## 5. 使用规则

1. ★ **一个新 artifact 一个新名**；改名必须写进 §4 的迁移表，`aliases` 保留旧名；
2. ★ 报数时写：**规范名 ＋ `mlmodelc` sha ＋ 口径（npu/gpu）＋ 解码器 ＋ 单元集**；
3. ★ **`e<EP>` 是快照轮**，不是 `epochs_planned`；写论文/报告时用 sidecar 的 `epochs_run`；
4. ★ 侧车**随 bundle 一起拷**（`scp -r <bundle> <host>:`），否则换机后就只剩一个光秃秃的目录 ✗
