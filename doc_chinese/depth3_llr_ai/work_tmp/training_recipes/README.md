# 训练配方脚本（可直接在其他机器重跑）

> 目的：任何一台机器都能用**同一份脚本 ＋ 同一份数据 ＋ 同一个 seed** 复现一次训练。
> 本目录只放**配方**，不放结果；结果与判读写在 `../*.md`。

## 1. 当前最好的配方（正在 106 上跑）

| 项 | 值 |
|---|---|
| 脚本 | `ceb36_const_80.sh` |
| 结构 | `--c-hid 32 --blocks 36 --dense-blocks --dense-skip-entry --se`（496 345 参数）|
| 训练 | `--batch 16 --lr 1e-3`（**恒定**）`--epochs 80 --seed 20261105` |
| 损失 | `--loss-norm bits --snr-weight 2 --snr-sigma 3 --smooth-k 0` |
| 数据 | `datasets_train_w2v3c`（训练）＋ `datasets_monitor_1102a`（监控，`--no-val` 下不选优）|
| 输出 | `runs/d3_w2v3c_c32b36dense_…-dskip-entry-se_e80`（每轮一个快照）|

## 2. 三台机器上**唯一**的差别

| 机器 | venv | 数据根目录 | 备注 |
|---|---|---|---|
| **106**（3060 Ti）| `~/work/ai_phy_train/.venv` | `~/work/ai_phy_train/data` | ★ **本配方在此跑过** |
| **107**（3060 Laptop）| `~/work/ai_phy_train/.venv` | 同上 | venv 名同为 `.venv` |
| **126**（GTX 1070）| `~/work/ai_phy_train/.venv216` | 同上 | ★ **venv 名不同**（历史上最容易踩的坑）|

★ **脚本里出现的 `.venv` 与 `data/` 是相对 `$HOME/work/ai_phy_train` 的**，换机器只改 venv 名即可。

## 3. 启动方式（后台，不占终端）

```bash
cd ~/work/ai_phy_train
nohup bash ceb36_const_80.sh > logs/ceb36_e80.log 2>&1 < /dev/null &
```

★ **不要**用 `taskpolicy -b`（macOS）或 `nice` 之外的调度手段；★ 训练机上 `TF_GPU_ALLOCATOR=cuda_malloc_async` 已在脚本里。

## 4. 复现时要核对的四件事

1. ★ **venv 名**（`.venv` 还是 `.venv216`）；
2. ★ **数据目录存在**：`data/datasets_train_w2v3c`（19 个 `.npy`）；
3. ★ **`seed` 与结构参数一字不差**（否则不是同一配方）；
4. ★ **`--snapshot-every 1`**（末端 SWA 需要 e37–e40 / e77–e80 的逐轮快照）。

## 5. ★★★★ 2026-11-27 e80 → e120 的**衰减续训**（211，正在跑）

| 项 | 值 |
|---|---|
| 脚本 | `ceb36_decay_120.sh` |
| 父 run | `runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e80`（211，**恒定 LR**，80 轮，485 833 参数）|
| 本段 | e81–e120，**cosine 1e-3 → 1e-4**（`--lr-decay-from 81`），其余 argv **与父一字不差** |
| 输出 | `runs/d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry_e120_decay` |
| 状态来源 | 把父目录的 `depth3_last.keras` ＋ `resume.json` **拷进新目录**，再 `--resume`（日志应打印 `next epoch 81`）|
| 参照 | 107 上同日程的 `…-dskip-entry-**se**_e120_decay`（496 345 参数；本条**不加 `--se`**，与 211 父 run 一致）|
| 目的 | 同一条轨迹上量"多 40 轮 ＋ 末端衰减"值多少 pp（§94/§99/§100/§101 的 +0.38/+0.57 pp 量级），并得到**末端平**的 e120 权重供尺子选 |
| ETA | 40 轮 × ~27 min ≈ **18 h**（3060 Ti 实测父 run 40 037 s / 25 轮）|

## 6. ★★★★ 2026-11-27 **"更长的衰减"**：e40 → e120，衰减从 e41 起（126，正在跑）

| 项 | 值 |
|---|---|
| 脚本 | `ceb18_decay41_120.sh` |
| 父 run | 126 的 `…-dskip-entry-se_e80_fixed` 的 **e40 快照**（CeB：18 块 ＋ `--dense-skip-entry --se`，248 593 参数）|
| 本段 | e41–e120，**cosine 1e-3 → 1e-4，衰减跨 79 轮**（`--lr-decay-from 41`）|
| 输出 | `runs/…-dskip-entry-se_e120_decay41` |
| 要回答 | ★ memo §73/§88 的"**更长的衰减（60–80 轮）**"从未测过：以前所有衰减只覆盖 e81–e120。若 18 块 ＋ 长衰减能追平 36 块 80 轮（11 961 632），则**深度可以不要**（MACs 减半）|
| 中间读数 | e77–e80（跑约 18 h）即可与**恒定 LR** 的 CeB e77–e80（11 871 320）对比，单独量"40 轮衰减"值多少 |
| 机器 | 126（GTX 1070 / Pascal）⇒ `.venv216` = TF 2.16.1 ＋ cuDNN 8.9.7（cuDNN 9 无 sm_61）|
| ETA | 80 轮 × ~27 min ≈ **36 h** |

## 7. ★★★★ 2026-11-27 **码字约束损失**（§93 #1；211，排队中）

| 项 | 值 |
|---|---|
| 脚本 | `ceb18_code_loss_40.sh` |
| 实现 | `code_loss.py`（新）＋ `train_depth3.py` 的 `--code-loss-*`；**默认关**，不开就是原来的训练 |
| 做什么 | 训练步里把网络 LLR 按链路顺序 gather → 解扰 → 解交织 → 存入码块循环缓冲（与 `ldpc_decode.rate_dematch` **同一份**索引代码）→ 跑 k 次归一化 min-sum → 对**后验 LLR** 在发射比特上算 BCE，加权进总损失 |
| 对照臂 | ★ **Ce+B e40**（18 块 / dense / `--dense-skip-entry --se`，248 593 参数，同 seed 同数据，e37–e40 SWA ＝ **11 856 112（+2.51%）**）|
| 配方 | λ=0.1、k=2、每批 2 个样本、从第 5 轮开始；其余与对照臂**逐字相同** |
| 自检 | `python code_loss.py --selfcheck`：完美信道精确解码；5 条含噪信道与 `ldpc_decode.MinSum` **逐位一致**；坏信道两边都判失败 |
| 读数 | 每轮日志行尾多一个 `code <值>`（起点 ≈ ln2 = 0.693）；**判据仍是尺子**（e37–e40 末端 SWA）|
| 机器 | 211（3060 Ti）；211 的 e120 衰减 run 跑完后由 watcher 自动起（先跑完 GPU 冒烟，XLA 不过就退到 `--no-jit`）|
