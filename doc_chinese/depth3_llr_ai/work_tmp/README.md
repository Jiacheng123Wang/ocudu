# `work_tmp/` — 第二代的工作目录（**不进 git**）

`doc_chinese/.gitignore` 里的 `work_tmp/` 规则会忽略整个目录；这里只放**大而可再生产**的东西：

| 目录 | 内容 |
|---|---|
| `calibration/` | W1 的斜率／钳位扫描结果与图 |
| `corpus_harq/` | W2 的 HARQ 链式标注产物与索引 |
| `runs/` | 训练检查点与 `history.json` |
| `artifacts/gates/` | 四门的回放 CSV（每模型一组）与逐格表 |
| `logs/` | 飞行与回放日志 |
| `probe/` | 只在本代用的临时脚本（稳定的脚本应上移到 `llr_ai_detection/work_tmp/probe/` 或仓库里）|

**第一代的资产不复制进来**，直接引用 `../../llr_ai_detection/work_tmp/...`。

## 语料规模的三个数（别混用）

| 数 | 含义 | 出处 |
|---|---|---|
| **140 438** | **候选池**：全部分源（历史语料 ＋ 四次 e17 构建 ＋ 历史 big/e4）的总样本 | `build_e17_from_datasets.py` 的池子 |
| **64 220** | **两次源索引**的捕获数：`e17_src/index.csv` 54 227 ＋ `e17_attach_src/index.csv` 9 993，**全部 `crc=OK`** | 2026-10-26 的腿 |
| **49 730** | **实际成为样本**的条数（四个 `e17_*` 数据集之和：3 326／8 849／33 353／4 202）| `data/datasets/e17_*/depth3.npz` |
| 3 294 / 100 004 | **验收集 / 训练集**（从 140 438 里按格抽取）| `data/datasets/e17_{test,train}` |
