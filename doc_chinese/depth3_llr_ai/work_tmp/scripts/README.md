# `work_tmp/scripts/` —— 可重复使用的脚本（**不要放在 /tmp**）

> ★ **2026-11-15**：`/tmp` 被清空过一次，之前散落在 `/tmp` 的基准脚本、建库脚本、归档脚本、训练启动脚本全部丢失。
> **凡是要重复用的，一律放这里**（随仓库提交、跟着 git 走）。

| 文件 | 用途 | 出处/说明 |
|---|---|---|
| ★ `bench_depth3_gpu.py` | 量一台新机器的训练步速（samples/s），合成数据、1 分钟 | memo **D.5/D.8** 的算力对比都来自它的前身 |
| ★ `build_w2v3c.py` | 建 **w2v3c** 训练集（w2v1 全量 ＋ s2_amc 2 PRB ≥22 dB，**排除 1102a**）| 结果指纹必须是 `f6c27b8bae52f7d57d19476a2a95606d`（计划 §118）|
| ★ `train_recipes.sh` | 五条正式配方的命令行（c128-sw2s3 / c32-xla / c32-k5 / c32-g4 / c32-cliff）| PK 里的数字都对应其中一条 |
| ★ `archive_legs.sh` | 把一条腿打成一个 `.tar.gz`，多腿并行，**归档后校验条目数** | 语料与 dump 归档都可用；`/tmp` 起家的脚本曾因两驱动竞争产生**截断**归档 |
| `analyse_llr_profile.py`（上一级目录）| 1 分钟结构分析，★ 已加 `--bootstrap`（按接收配对 CI）| 见 architecture_levers §10 |

## 用法示例

```bash
# 新机器算力体检
python bench_depth3_gpu.py --code ~/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train \
    --dense --batch 16 32 --jit

# 建 w2v3c（母集里必须没有 1102a）
python build_w2v3c.py --w2v1 ~/ai_depth3/datasets_train_w2v1 \
    --mother ~/ai_depth3/datasets_full_s2_amc --out ~/ai_depth3/datasets_train_w2v3c

# 跑一条正式配方（续训加 --resume，轮数按总计划给）
bash train_recipes.sh c32-xla --resume --epochs 40

# 归档一批腿（并行、自校验）
bash archive_legs.sh ~/ai_depth3/corpus /Volumes/CORPUS/s2_mcs_corpus_tarballs 's2_mcs*_ping_111[012]*' 12
```

## 规则

1. ★ 脚本里的路径优先用**环境变量**（`TRAIN`/`DATA`/`PY`/`CODE`），默认值写当前机器的习惯路径；
2. ★ 会写文件的地方，文件名带时间或 run 名，不覆盖上一次结果；
3. ★ 任何"跑完就算数"的脚本，都要自己**校验**（条目数、指纹、行数），别信 `mv` 成功。
