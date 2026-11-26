# 困难样本捞取（hard-sample harvest）工具集

> 设计文档：`../../hard_sample_harvest_plan.md`
> 与建库流程（`../../corpus_to_training_to_measurement_db_manual.md` §7）**同构**，唯一区别是 **LDPC 迭代上限 6 → 20**。

| 文件 | 作用 |
|---|---|
| `extract_units.py` | 从语料 tar 里**只解出需要的单元**（4 个文件），避免解整腿 |
| `harvest_leg.sh` | 单机跑一腿：读 dump 索引 → 只对 KO 单元重放（cap 20）→ 写 `index_hard.csv` |
| `harvest_worker.sh` | 工作机端到端：拉 tar → 捞 → 回传 211 → **校验字节数** → 删本地临时 |
| `harvest_hard.py` | 实验版：**rv0 单独 → 失败再用供体链式**，直接产出 `self` / `harq` 两群 |
| `pilot_harvest.py` | 试点用：同一单元在多个 cap 下重放，算翻转率 |

## 关键事实（实测）

- ★ **只跑 KO 单元**：索引里 `crc=OK` 的一律不碰；
- ★ **`crc=no` 的两类**：`new_data=0` 的重传**无法单独重放**（HARQ 软缓冲预留失败 `cannot reserve a receive buffer`），
  `new_data=1` 的首传才是真候选；
- ★ **harq 样本的定义**（与全库母集一致）：**rv0 采集 ＋ 供体软合并** 后 `crc=OK`，重传本身**永远不是样本**；
- ★ 产物按 `self` / `harq` 分群，与 `<腿>_<MOD>_self.npz` / `_harq.npz` 同构。
