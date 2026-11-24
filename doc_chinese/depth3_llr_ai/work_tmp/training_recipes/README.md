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
