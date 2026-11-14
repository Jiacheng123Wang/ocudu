# `work_tmp/` —— 工作目录：**按用途分类的脚本** ＋ 大体量产物

> ★ **2026-11-15 重组**：原来把脚本堆在 `tools/`、`tmp_rescue/`、`scripts/` 这类**泛名**目录里，
> 现在按**用途**分目录；数据类目录保持原样（它们是产物，不是代码）。
> ★ **规则**：脚本随仓库走（可 grep、可复用、可审计）；**大产物不进 git**（见 `doc_chinese/.gitignore` 的例外表）。

## 1. 脚本目录（按用途）

| 目录 | 管什么 | 关键脚本 |
|---|---|---|
| ★ **`corpus_pipeline/`** | **语料采集 → dump → 归档 → 搬运** | `dump_one_leg.sh`、`dump_all_legs.sh`、`dump_legs_parallel.sh`、`pause_dump.sh`、★ `archive_legs.sh`（自校验归档）、`tar_corpus.sh`、`bin_s2.py` |
| ★ **`dataset_build/`** | **建库 / 校验 / 切分 / 分类 / 抽样普查** | ★ `build_w2v3c.py`（当前训练集）、`build_mother_all.sh`、`build_full_*`、`build_parallel_211.sh`、`check_build*.py`、`verify_dumps.py`、`split_leg_by_run.py`、`classify_rv*.py`、`rv_crosstab.py`、`clean_stage.sh` |
| ★ **`measurement/`** | **探针 / 尺子 / 全腿测量** | ★ `probe_snapshot.sh`、`probe_watch.sh`、★ `measure_arm.py`、`measure_arm_203.py`、`measure_abs.py`、`measure_full.py`、`measure_thresholds.py`、`check_tables.py`、`plan.swift`（ANE 算子闸门）|
| ★ **`analysis/`** | **误差结构 / 模型筛查 / 数据集验证** | ★ `analyse_llr_profile.py`（含 `--bootstrap`）、`narrow_models.py`、`classify_harq_chains.py`、`verify_w2_train.py`、`verify_test_sets.py`、`probe_label_roundtrip_deadend.py`、`remote_verify.sh` |
| ★ **`training/`** | **训练配方 / 启动 / 续训 / 改名 / 远程拉起** | ★ `train_recipes.sh`（五条正式配方）、`rename_run.sh`、`fly.sh`、`fly_attach.sh` |
| ★ **`benchmarks/`** | **机器算力体检** | ★ `bench_depth3_gpu.py`（samples/s，合成数据 1 分钟）|
| `inventory/` | 清单与台账（CSV/JSON）| `built_*_inventory.csv`、`corpus_census_*.csv`、`census_ledger.json` |
| `history/` | ★ **一次性/历史脚本**（push_*、ship_*、dsh_*、worker*、备份驱动…）| 见 `history/README.md`；**保留不删**，供追溯当时的做法 |

## 2. 数据目录（产物，**不进 git**）

| 目录 | 内容 |
|---|---|
| `artifacts/` | 门控回放 CSV 与逐格表 |
| `calibration/` | 斜率/钳位扫描结果与图 |
| `logs/` | 运行日志 |
| `s2_dump/` | s2 dump 的中间件与运行日志 |

## 3. ★ 旧路径 → 新路径（2026-11-15 之前写的文档按左列找）

| 旧 | 新 |
|---|---|
| `work_tmp/tools/probe_snapshot.sh` | `work_tmp/measurement/probe_snapshot.sh` |
| `work_tmp/tools/measure_arm.py`、`measure_*` | `work_tmp/measurement/` |
| `work_tmp/tools/analyse_llr_profile.py` | `work_tmp/analysis/analyse_llr_profile.py` |
| `work_tmp/tools/dump_*.sh`、`archive_legs*.sh`、`pause_dump.sh`、`tar_corpus.sh` | `work_tmp/corpus_pipeline/` |
| `work_tmp/tools/build_*.sh`、`verify_dumps.py`、`clean_stage.sh` | `work_tmp/dataset_build/` |
| `work_tmp/tools/fly*.sh`、`rename_run.sh` | `work_tmp/training/` |
| `work_tmp/tools/measure_thresholds.py` | `work_tmp/measurement/measure_thresholds.py` |
| `work_tmp/tmp_rescue/*` | `work_tmp/dataset_build/`（建库/校验）或 `work_tmp/history/`（一次性）|
| `work_tmp/scripts/*` | 已按用途拆到 `benchmarks/`、`dataset_build/`、`training/`、`corpus_pipeline/` |
| 顶层 `classify_harq_chains.py`、`verify_w2_train.py` | `work_tmp/analysis/` |
| 顶层 `built_*_inventory.csv`、`census_ledger.json`、`corpus_census_*.csv` | `work_tmp/inventory/` |

## 4. 三条规则

1. ★ **凡是要重复用的，放这里，不放 `/tmp`**（2026-11-15 `/tmp` 被清空过，基准/建库/归档脚本全丢）；
2. ★ 路径优先用**环境变量**（`TRAIN`/`DATA`/`PY`/`CODE`），默认值写当前机器的习惯路径；
3. ★ "跑完就算数"的脚本必须**自校验**（条目数/指纹/行数），别信 `mv` 成功。

## 5. 附：语料规模的三个数（2026-10，别混用）

| 数 | 含义 | 出处 |
|---|---|---|
| **140 438** | 候选池（全部来源的样本总数）| `build_e17_from_datasets.py` 的池子 |
| **64 220** | 两次源索引的捕获数（全部 `crc=OK`）| 2026-10-26 的腿 |
| **49 730** | 实际成为样本的条数（四个 `e17_*` 数据集之和）| `data/datasets/e17_*/` |
