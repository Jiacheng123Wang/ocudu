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

