# 建库 runbook：从 dump ＋ 原始采集到「全库母集」

> **用途**：给定一条已 dump 的腿（`tb_<leg>/`）和它的原始采集（`<leg>/`），一路做到
> `<leg>_<MOD>_<self|harq>.npz`，并用**六项判据**验收；然后把同样的事重复 100 条腿，得到一个可抽取的全库母集。
> **本文是这条流水线的唯一操作入口**：照抄即可复现，不需要再读别的文档。
> **上位文档**：`corpus_to_training_to_measurement_db_manual.md` §7（母集的定义与"为什么不用 select_by_bin"）、
> `single_leg_dump_runbook.md`（dump 从 tar.gz 到 `index.csv`/`tb/`，本文的前一步）、
> `dump_workflow_memo.md`（dump 侧的实测账）。
> **实测日期**：2026-11-27（本机 Apple M4 Pro，14 核／64 GB／macOS；T7 APFS；母集写 Eaget_HD APFS）。
> **实测腿**：`s2_amc_iperf_1103i`（50 000 接收，AMC，dump 解码上限 20）。
> ★ 本文的每个数字都是那一天实测；数字随腿而变，**判据不变**。
> 占位符：`<leg>` ＝ 腿名，`<dump>` ＝ `…/tb_<leg>`，`<cap>` ＝ `…/<leg>`（原始采集目录），`<out>` ＝ 母集根。

---

## 0. 一分钟版（单腿，复制即可跑）

```bash
# ── 变量区：换腿只改第一行 ──────────────────────────────────────────────
LEG=s2_amc_iperf_1103i
REPO=/Users/jiachengwang/dev/ocudu
CORPUS=/Volumes/T7_Corpus/corpus              # 原始采集 + dump 都在这里
OUT=/Volumes/Eaget_HD/datasets_full_s2_amc    # 母集根（一条腿 8 个 npz，平铺）
PY=$HOME/ai_ce_work/venv/bin/python
TOOL=$REPO/lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh
DUMP=$CORPUS/tb_$LEG
mkdir -p ~/ai_depth3/logs_build_full_s2_amc          # tee 的目标目录

# ① 输入验收（三条判据；不等就不要往下跑）
python3 - "$DUMP" <<'PY'
import sys, os, csv
out = sys.argv[1]
us = [l.strip() for l in open(os.path.join(out, "units.txt")) if l.strip()]
with_bin = sum(1 for u in us if os.path.exists(u[:-4] + ".bin"))
rows = sum(1 for _ in open(os.path.join(out, "index.csv"))) - 1
lab = [r["label_from"] for r in csv.DictReader(open(os.path.join(out, "index_harq.csv")))]
print(f"units={len(us)} with_bin={with_bin} index_rows={rows} "
      f"self+harq={sum(1 for x in lab if x in ('self','harq'))} tb={len(os.listdir(os.path.join(out,'tb')))}")
PY
# 本腿预期：units=50000 with_bin=50000 index_rows=50000 self+harq=42749 tb=42749

# ② 记基线（建库会重跑 backfill，理论上不改这些文件；事后用它对账）
( cd "$DUMP" && md5 -r index.csv index_self.csv index_harq_only.csv index_ko.csv units.txt chain_iters.csv ) \
  > /tmp/${LEG}_dump_md5_before.txt

# ③ 建库（约 38 分钟／腿；REQUIRE_ITERATIONS=1 见 §2.3）
cd "$REPO"
TRAIN=$REPO DUMP_ROOT=$CORPUS CORPUS_ROOT=$CORPUS OUT=$OUT PY=$PY \
JOBS=1 CHUNK=8000 REQUIRE_ITERATIONS=1 \
  bash "$TOOL" "$LEG" 2>&1 | tee ~/ai_depth3/logs_build_full_s2_amc/$LEG.log

# ④ 验收（六项里的 1–3 项：个数 / 样本数 / 字段；第 5 项见 §5.5 的 md5 diff）
python3 - "$OUT" "$LEG" "$DUMP" <<'PY'
import sys, os, glob, csv, numpy as np
out, leg, dump = sys.argv[1], sys.argv[2], sys.argv[3]
files = sorted(glob.glob(os.path.join(out, leg + "_*.npz")))
tot = 0; bad = []
for p in files:
    d = np.load(p, allow_pickle=True); n = len(d["X"]); tot += n
    miss = {"iterations", "iterations_max", "unit"} - set(d.files)
    unk = (int((d["iterations"] < 0).sum()) + int((d["iterations_max"] < 0).sum())) if "iterations" in d.files else 2 * n
    if miss or unk:
        bad.append(f"{os.path.basename(p)} missing={sorted(miss)} unknown_iterations={unk}")
self_rows = len(list(csv.DictReader(open(os.path.join(dump, "index_self.csv")))))
harq_rows = len(list(csv.DictReader(open(os.path.join(dump, "index_harq_only.csv")))))
print(f"files={len(files)} samples={tot} | index self={self_rows} harq_first={harq_rows} | "
      f"rejected={self_rows + harq_rows - tot}")
for line in bad: print("  BAD " + line)
print("OK" if files and not bad else "FINDINGS — 见 §9")
PY
# 本腿预期：files=8 samples=41987 | index self=37599 harq_first=4436 | rejected=48，且没有 BAD 行

# ⑤ 审计（第 4 项；预期末尾是 "--- findings ---" 紧跟 "none"）
~/ai_ce_work/venv/bin/python lib/phy/upper/ai_signal_processors/depth3/ai_train/check_depth3_dataset.py \
  --dir "$OUT" 2>&1 | tail -6
```

**收尾**（§7）：确认 `<out>` 里只有 `*.npz` 与两个库级文件（`_LIBRARY.md`、`_manifest_<leg>.csv`），
没有 `stage/`、`rows/`、`*_part*.npz`。

---

## 0.1 与手册 §7 的差异（**以本文为准**）

| 手册 §7 / §8 的说法 | 现在的事实（2026-11-27 实测） |
|---|---|
| `OUT=~/ai_depth3/datasets_full_s2` | 库根是**可配置**的；本批全库在 `/Volumes/Eaget_HD/datasets_full_s2_amc` |
| 单腿（单调制）22 min、峰值 11.4 GB | **8 个 (调制×群体) 一条腿 38 min 12 s**；`CHUNK=8000` 时**单进程峰值 2.17 GiB**（实测 2 328 526 848 B）|
| §8.1b「母集 npz 目前没有 `iterations`，builder 必须改」 | **已改完**：npz 现在有 `iterations`/`iterations_max`（共 **25 个成员**），抽屉也已有 `--w-critical/--w-critical5/--keep-unit` |
| `census_s2_mother.py` | 文件名是 **`census_mother_set.py`** |
| 建库第 1 步「`harq_backfill.py` 打 self/harq 标」 | 它还会读 `chain_iters.csv`（合并解码的迭代数）并写 `index_ko.csv` |
| dump 产物「另有 `stage/`、`rows/` 草稿，可删」 | dump 已经**随跑随删**（memo §10）；建库只读契约产物，见不到草稿 |
| 判据只有 `findings: none` | 本文用**六项**（§5），其中"样本数对账"和"dump 未被改动"是新增的 |
| §7 的 `TRAIN` 只用于 PY 默认值 | 仍然对，但 `TRAIN` 必须是**同时含 `build/` 与 `lib/` 的仓库根**，否则 builder 拒绝 import（`build_mother_set.sh:59`）|

---

## 1. 机器、路径与角色

### 1.1 五处路径

| 角色 | 位置 | 内容 |
|---|---|---|
| **原始采集** | `/Volumes/T7_Corpus/corpus/<leg>/` | `cap_*.txt/.bin/_h.bin/_ce.txt` ＋ **`tb_live/`** ＋ `logs/`（一条腿 ≈ 9.6 GB／29 万文件）|
| **dump** | `/Volumes/T7_Corpus/corpus/tb_<leg>/` | `index.csv`、`tb/`、五个群体索引、`chain_iters.csv`、`dump_meta.txt`（≈ 184 MiB）|
| **母集** | `/Volumes/Eaget_HD/datasets_full_s2_amc/` | `<leg>_<MOD>_<self\|harq>.npz`，平铺（本腿 8 个／2.3 GB）|
| 代码仓 | `/Users/jiachengwang/dev/ocudu` | `build_mother_set.sh`、`build_depth3_dataset.py`、`check_depth3_dataset.py`、`census_mother_set.py` |
| Python | `~/ai_ce_work/venv/bin/python`（3.11.15）| numpy 足够；不需要 GPU |
| 归档机 211 | `jwang@192.168.31.211:/mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz` | 原始采集归档，**102 个包**（95 ping ＋ 7 iperf）；采集被删后从这里恢复 |

约定：`build_mother_set.sh` 按 `DUMP=$DUMP_ROOT/tb_$LEG`、`CAP=$CORPUS_ROOT/$LEG` 找输入（`:65-66`），
**两个根可以不同**（本机现在都在 T7；历史上 dump 在 `~/ai_depth3/dump_s2*`，捕获在 T7）。

### 1.2 一条腿的容量预算（实测）

| 阶段 | 占用 | 说明 |
|---|---|---|
| 原始采集 | **9.6 GB** | 293 510 文件；50 000 个接收 × 4 文件 ＋ `tb_live` 93 507 ＋ `logs` 3 |
| dump | **184 MiB** | `tb/` 42 749 块 172 MiB ＋ 索引 5.2 MiB ＋ `units.txt` 3.4 ＋ `chain_iters` 0.43 |
| 母集 | **2.3 GB** | 8 个 npz；见 §1.3 |
| 建库临时 | **≈ 1.4 GB** | `QPSK_self` 分 3 片时，3 个 `_part*.npz` 同时存在（443+449+440 MB），合并后即删 |
| 峰值 | **≈ 12.5 GB** | 开跑前 T7 留 12 GiB、母集盘留 5 GiB 即可（不是 20 GiB）|

### 1.3 产物构成（本腿实测）

| npz | 样本 | 体积 | 备注 |
|---|---|---|---|
| `QPSK_self` | 21 358 | 1.33 GB | 分 3 片建（7 120/7 120/7 118），流式合并 |
| `QPSK_harq` | 166 | 9.2 MB | |
| `16QAM_self` | 6 894 | 360 MB | |
| `16QAM_harq` | 158 | 9.2 MB | 候选 160，2 个无迭代数被丢 |
| `64QAM_self` | 7 473 | 364 MB | 候选 7 476，3 个 DM-RS 拒 |
| `64QAM_harq` | 1 246 | 73 MB | 候选 1 255，5 个 DM-RS ＋ 4 个无迭代数 |
| `256QAM_self` | 1 862 | 69 MB | 候选 1 871，9 个 DM-RS 拒 |
| `256QAM_harq` | 2 830 | 148 MB | 候选 2 839，7 个 DM-RS ＋ 2 个无迭代数 |
| **合计** | **41 987** | **2.3 GB** | self 37 587 ＋ harq 4 400 |

---

## 2. 步骤 0：代码、Python 与三个开关

### 2.1 为什么先做

`build_mother_set.sh` 只负责**编排**（打标 → 数调制 → 逐对建 → 审计），真正的样本由
`build_depth3_dataset.py` 产出。三件事容易在别的机器上漏，且**都不会报错**：

| 漏了 | 症状 |
|---|---|
| `TRAIN` 不是含 `build/` ＋ `lib/` 的仓库根 | builder 直接拒绝 import（有守卫，会报错，算幸运的）|
| `PY` 指到没有 numpy 的解释器 | 同上，报错 |
| `REQUIRE_ITERATIONS` 没设 | 少数字段为 `-1` 的样本混进母集（**静默**，见 §2.3）|

### 2.2 前置检查（一条命令一批）

```bash
cd /Users/jiachengwang/dev/ocudu
git log -1 --format='%h %ad %s' --date=iso          # 记下这个提交 = 本批库的代码版本
~/ai_ce_work/venv/bin/python -c "import numpy; print('numpy', numpy.__version__)"
bash -n lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh && echo "script ok"
df -h /Volumes/T7_Corpus /Volumes/Eaget_HD | tail -2   # T7 ≥ 12 GiB、母集盘 ≥ 5 GiB
ls -d /Volumes/T7_Corpus/corpus/tb_<leg> /Volumes/T7_Corpus/corpus/<leg>   # 两个输入都在
```

### 2.3 三个开关（决定产物内容，不只是速度）

| 变量 | 本批取值 | 作用 | 不这么设会怎样 |
|---|---|---|---|
| **`REQUIRE_ITERATIONS`** | **1** | 丢掉 `iterations`/`iterations_max` 为 `-1`（未知）的样本 | 母集里混入"难度未知"的样本；抽屉把 `-1` 当**容易**，加权会偏 |
| **`CHUNK`** | **8000** | 单进程最多组装 8 000 样本，超出就切片 ＋ 流式合并 | 一条腿单进程峰值 ~11–25 GB；多腿并行会 OOM |
| **`GAP_SLOTS`** | 200（默认）| 传给 `harq_backfill.py` 的链间隙护栏 | 调小会切断真链，harq 变少（文档注明：规则本身没有间隙护栏，这里故意放宽到"惰性"）|

`JOBS` 是**一条腿内几对同时建**；本批用 **1**（并行放在腿之间，见 §6）。

### 2.4 哪些情况会产出 `-1`

| 情况 | 会不会进母集 |
|---|---|
| dump 是 **7 列**的旧索引（2026-11-26 之前，**没有 `iterations_max`**）| 两列都**按列名**解析 ⇒ `iterations_max = -1` ⇒ `REQUIRE_ITERATIONS=1` 下**一个样本都建不出来**（`no samples produced` 失败 —— 这是**好事**，逼你换新 dump）|
| self 样本 | 永远有值（自己解出）|
| harq 样本的**合并解码**没被记录：标签来自空中链 `tb_live`，离线链回放没解出，且采集早于 `dd_meta` 的迭代列 | **丢弃**（本腿 8 个，占 0.02%）|
| 重传单元（`new_data=0`）| 它们本来就不是样本（`no-buffer`），在 `index_ko.csv` 里 |

---

## 3. 步骤 1：输入验收

### 3.1 dump 的三条判据（与 dump runbook §6 相同）

```bash
DUMP=/Volumes/T7_Corpus/corpus/tb_<leg>
python3 - "$DUMP" <<'PY'
import sys, os, csv
out = sys.argv[1]
us = [l.strip() for l in open(os.path.join(out, "units.txt")) if l.strip()]
with_bin = sum(1 for u in us if os.path.exists(u[:-4] + ".bin"))
rows = sum(1 for _ in open(os.path.join(out, "index.csv"))) - 1
ok = sum(1 for l in open(os.path.join(out, "index.csv")) if ",OK," in l)
print(f"units={len(us)} with_bin={with_bin} index_rows={rows} crc_ok={ok}")
PY
```

| # | 判据 | 不等的含义 |
|---|---|---|
| 1 | `index.csv` 行数 ＝ 有 `.bin` 的单元数 | 有单元没跑完／清单与语料不符 |
| 2 | `index.csv` 行数 ＝ `units.txt` 行数 | 拼装丢了行 |
| 3 | 通过 1、2 且 `crc_ok > 0` | 整腿全 KO ⇒ 配置/二进制/解包有问题 |

本腿：`units=50000 with_bin=50000 index_rows=50000 crc_ok=37599`。

**另外必看一眼索引的表头**（决定"能不能建出带难度标签的母集"）：

```bash
head -1 "$DUMP/index.csv"
# 必须是：unit,tb_bits,crc,sinr_db,iterations,iterations_max,slot,tbs_bytes   ← 8 列
# 7 列（没有 iterations_max）的是 2026-11-26 之前的 dump：建库会被 REQUIRE_ITERATIONS=1 全部拒绝（§2.4）
```

### 3.2 采集目录必须**同一条腿、且含 `tb_live/`**

`build_mother_set.sh` 需要 `<cap>` 做三件事：读 `.txt` 侧车决定**调制/带宽/邻道**（census）、
读 `.bin`/`_h.bin` 建平面、读 `tb_live/` 给 **harq 找 donor**。缺了会怎样（本腿实测）：

| 输入状态 | self | harq 首传 | ko | 说明 |
|---|---|---|---|---|
| dump ＋ 采集（含 `tb_live/`）| 37 599 | **4 436** | 7 251 | 标准 |
| dump ＋ 采集，但**删掉 `tb_live/`** | 37 599 | **4 425**（−11）| 7 612 | 少掉的 11 个正好是"只能靠 live 解码、离线链回放没复现"的那批 |
| 只有 dump（没有采集目录）| — | — | — | 脚本直接 `SKIP: no capture directory`，一步都建不了 |

⇒ **建库要在原始采集还在盘上的时候做**。2026-11-27 12:45 起 dump 批处理**保留**采集（`dump_workflow_memo.md` §15），
但 T7 只有 660 GiB、放不下 102 条腿的采集，所以操作者会在"该腿建库完成"之后才删。
如果发现采集已被删（或该腿从未解包），先按 `single_leg_dump_runbook.md` §3.2 从 211 重新解包（**2–4 分钟/腿**），再建库。

### 3.3 记录 dump 基线（证明建库不改 dump）

`build_mother_set.sh` 第 1 步会**在 dump 目录上重跑** `harq_backfill.py`（写 `index_self.csv`、
`index_harq_only.csv`、`index_harq.csv`、`index_ko.csv`）。这是幂等的（本腿 7 个文件 md5 前后**逐字节相同**），
但要留证据：

```bash
( cd "$DUMP" && md5 -r index.csv index_self.csv index_harq_only.csv index_ko.csv units.txt chain_iters.csv ) \
  > /tmp/<leg>_dump_md5_before.txt
# 建库后
diff <(sort /tmp/<leg>_dump_md5_before.txt) <(cd "$DUMP" && md5 -r index.csv index_self.csv index_harq_only.csv index_ko.csv units.txt chain_iters.csv | sort) \
  && echo "dump unchanged"
```

> ★ 若 `index_*.csv` 变了，先看是不是 dump 目录里缺 `chain_iters.csv`（缺了会把 harq 的合并迭代数**抹掉**、
> 退回"首传失败值"）。它必须和 `index.csv` 同目录。

### 3.4 `dump_meta.txt` 的 `ldpc_iterations`：这是"这份库能不能用来测量"的标记

| 值 | 含义 | 能用它做什么 |
|---|---|---|
| **6** | 与空中链（`pusch_dec_max_iterations`）一致＝**参考判决** | 训练 ＋ 门控/尺子/验收对比 |
| **20**（本批默认）| 更强的解码器：多出空中链解不出的样本 | **训练可以**；**不能**拿去和经典链做门控/尺子对比 |

本腿是 `ldpc_iterations=20`：比 cap 6 多 **852** 个有 ground truth 的样本（其中 824 个从"借 donor 标签"升级为"自己解出"）。
这份母集因此**不适合**做"AI vs 经典链"的测量数据集；测量要用 cap 6 的 dump 重新建的那一份。

---

## 4. 步骤 2：跑建库

### 4.1 命令（本腿实跑的那条）

```bash
cd /Users/jiachengwang/dev/ocudu
TRAIN=$PWD \
DUMP_ROOT=/Volumes/T7_Corpus/corpus \
CORPUS_ROOT=/Volumes/T7_Corpus/corpus \
OUT=/Volumes/Eaget_HD/datasets_full_s2_amc \
PY=$HOME/ai_ce_work/venv/bin/python \
JOBS=1 CHUNK=8000 REQUIRE_ITERATIONS=1 \
bash lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh <leg> \
  2>&1 | tee ~/ai_depth3/logs_build_full_s2_amc/<leg>.log
```

建议在 `tmux` 里跑；单腿 38 分钟，中途断网/锁屏都不影响（纯本地 CPU＋磁盘）。

### 4.2 环境变量表

| 变量 | 本批取值 | 作用 | 坑 |
|---|---|---|---|
| `TRAIN` | 仓库根 | 默认值来源 ＋ builder 的"仓库根"判据 | 必须是含 `build/` 与 `lib/` 的目录；否则 builder 拒绝 import |
| `DUMP_ROOT` | `/Volumes/T7_Corpus/corpus` | dump 在 `$DUMP_ROOT/tb_<leg>` | 给错＝`SKIP: no index.csv` |
| `CORPUS_ROOT` | `/Volumes/T7_Corpus/corpus` | 采集在 `$CORPUS_ROOT/<leg>` | 给错＝`SKIP: no capture directory` |
| `OUT` | `/Volumes/Eaget_HD/datasets_full_s2_amc` | 母集根（**平铺**）| ★ 不要给成"每条腿一个子目录"：下游 `draw_train_set_w2.py` 用 `glob(*.npz)` 扫平铺目录 |
| `PY` | `~/ai_ce_work/venv/bin/python` | 解释器 | 默认是 `$TRAIN/.venv/bin/python`，仓库里没有 `.venv` |
| `JOBS` | **1** | 一条腿内几对同时建 | 并行放在腿之间更划算（§6），腿内并行只省墙钟、不改内容 |
| `CHUNK` | **8000** | 单进程样本上限 | 0 = 不限（峰值可到 11–25 GB）|
| `REQUIRE_ITERATIONS` | **1** | 丢无迭代数的样本 | 见 §2.3 |
| `GAP_SLOTS` | 200 | 链间隙护栏 | 调小会切断真链 |
| `MODS` | 默认四种 | 只建指定的调制 | 调试用；正常不要设 |

### 4.3 脚本内部做四步（每步都有判据）

| 步 | 做什么 | 判据／产物 |
|---|---|---|
| 1 | `harq_backfill.py`：按 HARQ 链给每个单元打 **self / harq / none**；donor 取 live 优先、链回放兜底；合并迭代数按 **live > 链回放 > donor 自解** 的优先级填 | 打印 `self=… harq=… ko=…`；写出四个索引 |
| 2 | 扫 `cap_*.txt` 侧车统计**实际存在的调制**（★ 不从腿名推）| `modulations present:` 列出几种 ⇒ 就建几种 × 2 个群体 |
| 3 | 每个 (调制, 来源) 直接调 `build_depth3_dataset.py`（**不走 `select_by_bin`/配额**）| 每个 npz 写出；`--count-only` 先算片数 |
| 4 | `check_depth3_dataset.py` 只审计**本腿**的 npz | **`findings: none`** |

> ★ **为什么不走 `select_by_bin.py`**：它是**抽样**工具（按箱配额），而且"无限配额"曾经静默沿用抽样账本、
> 不报错地浪费过数小时。母集要**全量**，所以脚本从不碰配额机制。

### 4.4 进度怎么看

日志里每 25 个单元一行 `... N/M units, N samples`；每个 npz 写完一行 `wrote …: N samples`。
不想盯日志就看目录（npz 出现＝该对完成）：

```bash
watch -n 30 'ls -lh /Volumes/Eaget_HD/datasets_full_s2_amc/*.npz | tail -5; echo; ls /Volumes/Eaget_HD/datasets_full_s2_amc/*_part*.npz 2>/dev/null | wc -l'
```

### 4.5 典型耗时（本腿实测，`JOBS=1 CHUNK=8000`）

| 阶段 | 耗时 | 备注 |
|---|---|---|
| 打标 ＋ 调制普查 | **≈ 1 min** | 读 5 万个 PDU 侧车 |
| `QPSK_self`（3 片）| **≈ 16 min** | 7 120 样本/片；单片 8 000 样本实测 **229.6 s**、峰值 **2.17 GiB** |
| 其余 7 对 | **≈ 21 min** | QPSK_harq 11:57 → 256QAM_harq 12:18 |
| 审计 ＋ 合并 | **< 1 min** | |
| **合计** | **38 min 12 s**（11:40:55 → 12:19:07）| 41 987 样本 ⇒ **≈ 54 ms/样本**（含空转与 I/O）|

---

## 5. 步骤 3：六项验收

### 5.1 契约：npz 个数 ＝ 2 × 调制数

```bash
ls /Volumes/Eaget_HD/datasets_full_s2_amc/<leg>_*.npz | wc -l
grep -A6 'modulations present' ~/ai_depth3/logs_build_full_s2_amc/<leg>.log
```

本腿：4 种调制 ⇒ **8 个**。某调制**没有 harq** 时脚本容忍（`no samples produced` 不算失败），
这时会少一个文件 —— 以日志里的 `populations:` 行和 `no samples produced` 为准。

### 5.2 样本数对账（**新增判据，最容易发现"建少了"**）

样本数必须等于该 (调制, 群体) 的 index 行数，**减去**两类被拒：

| 类别 | 本腿 | 原因 |
|---|---|---|
| DM-RS 一致性检查 | **40** | `the pilot LS does not match the data-aided channel`（信道与导频的自洽检查；§18(c) 记过）|
| 无迭代数（`REQUIRE_ITERATIONS=1`）| **8** | 见 §2.4 |

```bash
python3 - /Volumes/Eaget_HD/datasets_full_s2_amc /Volumes/T7_Corpus/corpus/tb_<leg> <leg> <<'PY'
import sys, os, glob, csv, numpy as np
out, dump, leg = sys.argv[1], sys.argv[2], sys.argv[3]
built = {}
for p in glob.glob(os.path.join(out, leg + "_*.npz")):
    mod = [m for m in ("QPSK","16QAM","64QAM","256QAM") if m in os.path.basename(p)][0]
    pop = "harq" if p.endswith("_harq.npz") else "self"
    built[(mod, pop)] = len(np.load(p, allow_pickle=True)["X"])
# index 里每种调制有多少行：读侧车（builder 也是这么判的）
def mod_of(unit):
    with open(os.path.join(os.path.dirname(dump.rstrip("/")), leg, unit + ".txt")) as h:
        for line in h:
            if line.startswith("modulation="):
                return line.strip().split("=", 1)[1]
manifest = []
for pop, idx in (("self", "index_self.csv"), ("harq", "index_harq_only.csv")):
    rows = list(csv.DictReader(open(os.path.join(dump, idx))))
    per = {}
    for r in rows:
        try: m = mod_of(r["unit"])
        except OSError: continue
        per[m] = per.get(m, 0) + 1
    for m, n in sorted(per.items()):
        got = built.get((m, pop), 0)
        print(f"{m:6s} {pop:4s} index={n:6d} built={got:6d} rejected={n - got:3d}")
        manifest.append([leg, m, pop, got, n, n - got])
print("total built:", sum(built.values()))
path = os.path.join(out, f"_manifest_{leg}.csv")          # 一条腿一份，便于全库汇总（§6.6）
with open(path, "w", newline="") as fh:
    w = csv.writer(fh); w.writerow(["leg","modulation","population","samples","index_rows","rejected_total"])
    w.writerows(manifest)
print("wrote", path)
PY
# 本腿：41987（self 37587 = 37599−12；harq 4400 = 4436−28−8），并写出 _manifest_s2_amc_iperf_1103i.csv
```

判据：**每个 `rejected` 都应是"DM-RS 拒 ＋ 无迭代数"的可解释数目**（日志里有 `N unit(s) skipped` 逐对记录）；
出现无法解释的缺额（例如整片没写）就是失败。

### 5.3 字段验收：每个样本都要有 `iterations`/`iterations_max`/`unit`

```bash
python3 - /Volumes/Eaget_HD/datasets_full_s2_amc <leg> <<'PY'
import sys, os, glob, numpy as np
for p in sorted(glob.glob(os.path.join(sys.argv[1], sys.argv[2] + "_*.npz"))):
    d = np.load(p, allow_pickle=True); n = len(d["X"])
    print(f"{os.path.basename(p):40s} n={n:6d} members={len(d.files)} "
          f"it={int((d['iterations']<0).sum())} it_max={int((d['iterations_max']<0).sum())} "
          f"unit={'unit' in d.files}")
PY
# 本腿：8 个文件、25 成员、两个字段缺失全 0
```

**口径**（下游 `draw_train_set_w2.py --w-critical` 依赖）：

| 值 | 含义 |
|---|---|
| 1–20 | 实际 LDPC 迭代数（本批 cap 20）|
| 0 | 解码失败 —— 本库不含 |
| −1 | 未知 —— **本库不含**（`REQUIRE_ITERATIONS=1` 已丢）|

`iterations` 是**码块均值**，`iterations_max` 是**最差码块**的值；后者才能看出"均值掩盖了难码块"。

### 5.4 审计：`findings: none`

```bash
~/ai_ce_work/venv/bin/python \
  lib/phy/upper/ai_signal_processors/depth3/ai_train/check_depth3_dataset.py \
  --dir /Volumes/Eaget_HD/datasets_full_s2_amc 2>&1 | tail -12
```

它检查形状（`(612, 64)`）、dtype（`float16`）、比特平衡（≈0.500）、SNR 合理区间、授权宽度范围。
本腿输出：`shapes {(612, 64)} dtypes {'float16'} findings none`。

### 5.5 dump 未被改动

见 §3.3 的 `diff`。本腿 7 个文件 md5 全同。

### 5.6 分箱盘点 ＋ manifest

```bash
~/ai_ce_work/venv/bin/python \
  lib/phy/upper/ai_signal_processors/depth3/ai_train/census_mother_set.py \
  --dir /Volumes/Eaget_HD/datasets_full_s2_amc --csv /tmp/<leg>_census.csv --caps 2500:250,8000:800
```

本腿：**113/224 箱非空**；均匀抽取投影 **28 469**（2 500/250）／**39 834**（8 000/800）。

manifest（一条腿一份，便于全库汇总）：**§5.2 的对账脚本已经写出** `<out>/_manifest_<leg>.csv`
（列：`leg,modulation,population,samples,index_rows,rejected_total`）。本腿 8 行，合计 41 987。

> ★ 分箱盘点看**母集侧**（只含可解码样本）；捕获侧（`census_bins.py`）用来判断"下一腿该补哪个格子"。
> 抽取时**按箱均匀、不按尺子加权**（手册 §7.1.4 的口径不变）。

---

## 6. 全库：100+ 条腿怎么跑

### 6.1 全库的边界

| 项 | 值 |
|---|---|
| 211 上的 s2_amc 归档 | **102 条腿**（`s2_amc_iperf_1103*` 7 ＋ `s2_amc_ping_1102*` 20 ＋ `ping_1103*` 8 ＋ `ping_1121*` 17 ＋ `ping_1123*` 35 ＋ `ping_1124*` 15）|
| ★ **必须排除** | **`s2_amc_ping_1102a`** —— 评测保留腿（ruler 5 000／probe 500／full leg 的唯一来源）；进了母集，任何 `glob` 抽取都会把它混进训练 |
| 可建库腿数 | **101** |
| 预计体积 | 101 × 2.3 GB ≈ **235 GB**（Eaget_HD 剩 953 GiB）|

### 6.2 顺序：**dump 完成 → 建库 → 回收**（raw 必须留到建库之后）

2026-11-27 12:45 起，dump 批处理**不再自动删**原始采集（`dump_workflow_memo.md` §15）：它在每条腿 dump 完成后保留
`<cap>`，并且在 T7 少于 **20 GiB** 时打印 `WAITING: N GiB free …` **等待**，而不是中止或删除。所以两条流水线可以并行：

```
[211 tar.gz] --解包--> <cap> --dump--> tb_<leg> --build--> 8 npz --回收--> 删 <cap>
```

| 规则 | 说明 |
|---|---|
| dump 一完成就能建库 | 建库只要 `<dump>` 与 `<cap>` 同时存在（§3.2）|
| `<cap>` 只能在**建库完成之后**删 | 它承载 `X`/`H`/`r`/`sigma2` 的原始网格与 `tb_live` 的 harq donor；删早了这条腿就得重新解包 |
| **"建完即删"不是优化，是必需** | 102 腿 × 9.6 GB ≈ 980 GB > T7 的 660 GiB ⇒ 不回收，dump 批处理会停在 `WAITING` |
| 删除命令 | `rm -rf /Volumes/T7_Corpus/corpus/<leg>`（只删采集，**dump 保留**）|

时间账不变：解包 2–4 分钟＋建库 38 分钟 ⇒ 单线程约 42 分钟/腿，**101 腿约 70 小时**；4 路并行约 **18–20 小时**。

### 6.3 并发与资源（实测数）

| 资源 | 实测 | 建议 |
|---|---|---|
| 单进程峰值内存（`CHUNK=8000`）| **2.17 GiB** | 4 路 ＝ ≈ 9 GB，64 GB 机器无压力 |
| 单进程 CPU | ≈ 1 核（BLAS 单线程；`/usr/bin/time`：204.9 s user / 229.6 s real）| 4 路 ≈ 4 核 |
| 磁盘 | 每条在飞腿 9.6 GB 采集 ＋ 0.19 GB dump ＋ 2.3 GB 产物 ＋ 1.4 GB 临时；**采集会一直留到该腿建完** | T7 至少留 `12 GiB × 并行数` 且**建完即删**（否则 dump 批处理停在 20 GiB 守卫）；母集盘留 5 GiB/腿 |
| 与 dump 批处理共存 | dump 用 12 个 replay 进程（14 核）| 建库**再开 2–3 路**为宜；再多两边都慢 |

★ **降优先级只 `nice`，不要 `taskpolicy -b`**（手册 §21：后台 QoS 会把线程压到能效核）。

### 6.4 幂等与可恢复

| 判据 | 做法 |
|---|---|
| 一条腿"已完成" | 进度表里有 `leg,ok` **且** `ls <out>/<leg>_*.npz` 数量 ＝ 2 × 调制数 |
| 半成品 | 不覆盖、不原地续：把该腿的 npz 移到 `<out>/_trash_<leg>_<时间>/` 再重跑 |
| dump 未就绪 | 跳过并记 `dump-not-ready`，下一轮再跑（批处理会持续产出 dump）|
| 采集未解包 | 自动从 211 解包（`ssh cat | tar -xzf - --exclude '._*'`）|
| 解包不完整 | 若解出的单元数 ≠ dump 的 `units.txt` 行数，**换新目录重解**，不要在半个目录上续解 |

### 6.5 监控

```bash
tail -f ~/ai_depth3/logs_build_full_s2_amc/batch.out
ls /Volumes/Eaget_HD/datasets_full_s2_amc/*.npz | wc -l        # 每腿 8 个 ⇒ 8/16/24…
tail -3 ~/ai_depth3/logs_build_full_s2_amc/<leg>.log
uptime                                                          # load 长期 < 4 = 被压到能效核
grep -c WAITING ~/ai_depth3/logs_s2_amc_batch/batch.out         # dump 批处理在等空间 ⇒ 去删已建完的腿
```

### 6.6 全库收尾

1. `check_depth3_dataset.py --dir <out>` 跑**整库**一次（本腿 8 个文件/2.3 GB 约 **20 s**；101 腿约 235 GB ⇒ 约 **30 min**，可先 `--max-files` 抽查）；
2. `census_mother_set.py --dir <out>` 出全库分箱表，回答"哪些格子还是空的"；
3. 汇总 `_manifest_*.csv` → `_manifest_all.csv`；
4. 抽样复核：随机抽 20 个 npz，各抽 5 个样本，`iterations`/`unit` 与 dump 的 `index.csv` join 对得上；
5. 记录**代码版本**（`git log -1`）与每腿 dump 的 `ldpc_iterations`（要混用 cap 6/20 时尤其重要）。

---

## 7. 收尾与交接

一条腿的目录里**只应有**：

| 产物 | 性质 |
|---|---|
| `<leg>_<MOD>_<self\|harq>.npz`（1–8 个）| 母集本体 |
| `_manifest_<leg>.csv` | 逐文件样本数／index 行数／被拒数 |
| `_LIBRARY.md`（库级，只写一次）| 语义、出处、重建命令、下游用法 |

**不该有**：`*_part*.npz`（合并后应删）、`stage/`、`rows/`、`.DS_Store`。
归档前：

```bash
find /Volumes/Eaget_HD/datasets_full_s2_amc -name '.DS_Store' -delete
find /Volumes/Eaget_HD/datasets_full_s2_amc -name '*_part*.npz' -print    # 应为空
```

---

## 8. 时间与容量总账（单腿，本机实测）

| 阶段 | 命令 | 耗时 | 增量占用 |
|---|---|---|---|
| 输入验收 ＋ 基线 md5 | 两条 python/md5 | 秒级 | — |
| 打标 ＋ 调制普查 | `build_mother_set.sh` 第 1–2 步 | ≈ 1 min | 索引 5.2 MiB |
| 建 8 个 npz | 第 3 步（`CHUNK=8000`）| **≈ 37 min** | 2.3 GB（＋临时 1.4 GB）|
| 审计 ＋ 合并 | 第 4 步 | < 1 min | — |
| **合计** | — | **38 min 12 s** | 峰值 ≈ 12.5 GB |
| 全库（101 腿，4 路）| — | **≈ 18–20 h** | ≈ 235 GB |

---

## 9. 坑与排查（全部真实发生过）

| # | 现象 | 原因 | 处置 |
|---|---|---|---|
| 1 | `SKIP: no index.csv` | `DUMP_ROOT` 给错，或该腿还没 dump | 确认 `<DUMP_ROOT>/tb_<leg>/index.csv`；没 dump 就先跑 dump runbook |
| 2 | `SKIP: no capture directory` | 采集已被删（建完某腿后清理过头 / 该腿从没解包）| 从 211 重解包（2–4 min）|
| 3 | harq 比预期少十几个 | 采集目录里**没有 `tb_live/`** | 本腿实测 4 436 → 4 425；重新解包带 `tb_live` 的那份 |
| 4 | 母集里 `iterations` 全是 −1 | 用了 2026-11-26 之前的旧 dump（7 列索引）| 用新 dump；`REQUIRE_ITERATIONS=1` 会当场失败提醒你 |
| 5 | 少数 harq 样本 `iterations = -1` | 标签来自 live、链回放没解出、采集早于 `dd_meta` 迭代列 | 本批政策：**丢弃**（`REQUIRE_ITERATIONS=1`）；不要填 6 冒充 |
| 6 | `no samples produced` 且 rc≠0 | 该 (调制, 群体) 真的没有候选 | harq 空是正常的（脚本容忍）；self 空要查 index/侧车 |
| 7 | 某腿少建了一个调制 | 腿名不决定调制，**侧车才决定** | 看日志 `modulations present:`，以它为准 |
| 8 | 一个进程 RSS 涨到 10 GB+ | `CHUNK=0`（不限）| 设 `CHUNK=8000`；多腿并行时**必须**设 |
| 9 | 两条腿同时写同一个 `OUT` 时串了 | 同名文件互相覆盖（同名才可能）| 每条腿的 npz 名字带腿名，正常不会撞；**不要**同一腿并行两次 |
| 10 | `findings` 不是 none | 形状/比特平衡/SNR 越界 | 先看二进制与 dump 版本；再看是否解包不全 |
| 11 | 建库后 `index_*.csv` 变了 | dump 目录缺 `chain_iters.csv`（合并迭代数被抹）| 从 dump 归档恢复 `chain_iters.csv`；或重跑 dump 第二段（≈40 s/腿）|
| 12 | T7 满、dump 批处理停在 `WAITING` | 采集 9.6 GB/腿 留太多（102 腿放不下），或 `_part` 没删 | `rm -rf /Volumes/T7_Corpus/corpus/<已建完的腿>`；确认没有残留 `*_part*.npz` |
| 13 | 负载 40、两边都慢 | 建库进程 + dump 的 12 个 replay 抢 14 核 | 建库并行降到 2；不要 `taskpolicy -b` |
| 14 | 1102a 混进母集 | 图省事 `for leg in $(ls 211)` | 驱动脚本里**显式排除**（附录 C 的 `EXCLUDE`），使它连 npz 都不存在 |
| 15 | 抽取时 `glob` 到评估腿 | 1102a 曾建在同一目录 | 目录层面就排除（本批根本不建它）|
| 16 | `_part0.npz` 留在目录里 | 合并失败（某片 rc≠0）| 看日志 `FAILED: … has a failed chunk; not merging`，重跑该腿 |
| 17 | 用了 cap 20 的母集做尺子对比 | **852** 个样本空中链（cap 6）解不出 | 测量另建 cap 6 的母集；`dump_meta.txt` 的 `ldpc_iterations` 是标记 |
| 18 | 旧 7 列 dump 建出来的 `iterations_max` 等于 slot | 以前按**列位置**读第 5 列，而旧索引的第 5 列是 `slot`（`REQUIRE_ITERATIONS` 也拦不住：slot 是正数）| **已改为按列名解析**；旧 dump 现在得到 `-1` 并被拒绝。遇到 7 列索引就换 dump，不要绕过 |

---

## 10. 本次实测指纹（2026-11-27，腿 `s2_amc_iperf_1103i`）

| 项 | 值 |
|---|---|
| 输入采集 | 293 510 文件 / 9 576 543 376 B / 50 000 单元 / `tb_live` 93 507 |
| 输入 dump | `units=50000 with_bin=50000 index_rows=50000 crc_ok=37599`；`self=37599 harq=4436 ko=7251 tb=42749`；`ldpc_iterations=20` |
| 代码 | `7e810b23db` ＋ 工作区两处未提交改动：`--require-iterations`、难度两列**按列名**解析 |
| 命令 | `JOBS=1 CHUNK=8000 REQUIRE_ITERATIONS=1 build_mother_set.sh s2_amc_iperf_1103i` |
| 耗时 | **38 min 12 s**（11:40:55 → 12:19:07）|
| 产物 | 8 个 npz、**41 987** 样本（self 37 587／harq 4 400）、**2.3 GB**、每个 **25 成员** |
| 被拒 | 候选 42 035 → DM-RS 40 ＋ 无迭代数 8 ⇒ 41 987 |
| 字段 | `iterations`/`iterations_max`/`unit` 缺失 **0**；直方到 20 |
| 审计 | `findings: none` |
| dump 完整性 | 建库前后 7 个索引 md5 **逐字节相同** |
| 分箱 | 113/224 非空；投影 28 469（2 500/250）、39 834（8 000/800）|
| 单片基准 | 8 000 样本：**229.6 s**、**2.17 GiB**、产物 498.4 MB |
| 并发基准 | 4 路时预计 ≈ 9 GB 内存、≈ 4 核；与 dump 批处理共存时建议 2–3 路 |

---

## 附录 A — npz 字段字典（25 个成员）

| 成员 | dtype / 形状 | 含义 |
|---|---|---|
| `X` | f16 `(N,612,64)` | 契约输入平面（绝对窗口，含保留信道）|
| `H` | f16 `(N,612,14,2)` | 估计信道（复数按实部/虚部）|
| `bits` | u8 `(N,max_bytes)` | 打孔后的码字比特（按 `nof_bits` 截断，其余补零）|
| `tb_bits` | u8 `(N,max_tb)` | TB 比特（按 `nof_tb_bits`）|
| `r` `sigma2_over_r2` `pilot_ls_ratio` | f32 | 噪声与信道拟合量 |
| `nof_bits` `nof_tb_bits` | i32 | 有效比特数 |
| `alloc_first_prb` `alloc_nof_prb` `qm` `rv` `dmrs_bitmap` | u8/u16 | 分配与调制信息 |
| `snr_nominal` | f32 | dump 侧的 SNR 观测（dB；real 样本不另外注入噪声）|
| `rnti` `n_id` | u16 | 标识 |
| `uci_nof_harq_ack` `uci_ack_re` `uci_ack_rvd_re` `uci_csi1_re` | i32 | UCI 位置 |
| `source` | u8 | 0 = real（本库全部为 0）|
| **`iterations`** | i64 | 解出该 TB 的 LDPC 迭代数（码块均值）|
| **`iterations_max`** | i64 | 最差码块的迭代数 |
| **`unit`** | object | `cap_…@natural`；去掉 `@…` 即 dump 单元名 |

## 附录 B — 下游接口（本库就是为它们服务的）

```bash
LIB=lib/phy/upper/ai_signal_processors/depth3/ai_train
OUT=/Volumes/Eaget_HD/datasets_full_s2_amc

# ① 分箱盘点
$PY $LIB/census_mother_set.py --dir $OUT --csv /tmp/census.csv --caps 2500:250,8000:800

# ② 抽训练集（难度加权；保持每箱 cap 不变）
$PY $LIB/draw_train_set_w2.py --in $OUT --out <目标>/datasets_train \
    --self-cap 2500 --harq-cap 250 --seed 20261103 --w-critical 4 --w-critical5 8 --keep-unit

# ③ 抽 monitor（同源、不同种子、小 cap）
$PY $LIB/draw_train_set_w2.py --in $OUT --out <目标>/datasets_monitor \
    --self-cap 15 --harq-cap 3 --seed 20261104 --keep-unit

# ④ 六项验收（抽取侧）
$PY $LIB/verify_train_set_w2.py --draw <目标>/datasets_train --mother $OUT
```

★ 抽取纪律（手册 §8.1b(e)）：评估腿不得进训练集；按箱均匀、不按尺子加权；临界加权只在**箱内**做、cap 不变。

## 附录 C — 批量驱动（可直接存成 `~/ai_depth3/run_mother_set_batch.sh`）

```bash
#!/usr/bin/env bash
# 101 条 amc 腿：解包（缺则从 211 取）→ 建库 → 回收采集，每 CONC 条一组、组间 wait 作屏障。
# 幂等：进度表里 ok 的腿跳过；dump 未就绪的腿记一笔、下轮再跑。
# 采集在该腿建库成功后才删（T7 放不下 102 条腿的 raw；dump 批处理的 20 GiB 守卫会等这个空间）。
# 本驱动在 T7 少于 MIN_FREE_GIB(60 GiB) 时中止（不是等待）：删掉已建完的腿再重跑，进度表可续。
set -uo pipefail

REPO=/Users/jiachengwang/dev/ocudu
CORPUS=/Volumes/T7_Corpus/corpus
OUT=/Volumes/Eaget_HD/datasets_full_s2_amc
PY=$HOME/ai_ce_work/venv/bin/python
TOOL=$REPO/lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh
REMOTE=jwang@192.168.31.211
TARDIR=/mnt/corpus/s2_amc_corpus_tarballs
LOGD=$HOME/ai_depth3/logs_build_full_s2_amc
PROGRESS=$OUT/_progress.csv
CONC=${CONC:-3}
MIN_FREE_GIB=${MIN_FREE_GIB:-60}
RECLAIM=${RECLAIM:-1}      # 建库成功后删掉该腿的 raw（重建时从 211 再解包即可）
# 评测保留腿：永不入库（手册 §17/§24）
EXCLUDE="s2_amc_ping_1102a"

mkdir -p "$LOGD" "$OUT"
[ -f "$PROGRESS" ] || echo "leg,status,seconds,files,samples,finished_at" > "$PROGRESS"
done_leg() { awk -F, -v l="$1" '$1==l && $2=="ok"{f=1} END{exit !f}' "$PROGRESS"; }
mark()     { echo "$1,$2,$3,$4,$5,$(date '+%Y-%m-%dT%H:%M:%S')" >> "$PROGRESS"; }
# "findings: none" 是这一行（5 个空格 + none），紧跟在 "--- findings ---" 之后
findings_ok() { awk '/--- findings ---/{getline; if ($0 ~ /none/) ok=1} END{exit !ok}' "$1"; }

one_leg() {
  local leg=$1 start=$SECONDS rc n
  local cap="$CORPUS/$leg" dump="$CORPUS/tb_$leg" log="$LOGD/$leg.log"

  if [ ! -f "$dump/index.csv" ]; then mark "$leg" dump-not-ready 0 0 0; return 0; fi
  if [ ! -d "$cap" ]; then
    echo "[$(date +%H:%M:%S)] $leg: extracting"
    ssh "$REMOTE" "cat $TARDIR/$leg.tar.gz" | tar -xzf - -C "$CORPUS" --exclude '._*' || {
      mark "$leg" extract-failed 0 0 0; return 1; }
    # 解包完整性：单元数必须与 dump 的 units.txt 一致
    local got want
    got=$(find "$cap" -maxdepth 1 -name '*.txt' ! -name '*_ce.txt' ! -name '._*' | wc -l | tr -d ' ')
    want=$(wc -l < "$dump/units.txt" | tr -d ' ')
    [ "$got" = "$want" ] || { echo "$leg: extracted $got units, dump has $want"; mark "$leg" extract-short 0 0 0; return 1; }
  fi

  nice -n 10 env TRAIN="$REPO" DUMP_ROOT="$CORPUS" CORPUS_ROOT="$CORPUS" OUT="$OUT" PY="$PY" \
      JOBS=1 CHUNK=8000 REQUIRE_ITERATIONS=1 bash "$TOOL" "$leg" > "$log" 2>&1
  rc=$?
  n=$(ls "$OUT/${leg}"_*.npz 2>/dev/null | wc -l | tr -d ' ')
  local samples
  samples=$("$PY" - "$OUT" "$leg" <<'PY'
import sys, glob, os, numpy as np
print(sum(len(np.load(p, allow_pickle=True)["X"]) for p in glob.glob(os.path.join(sys.argv[1], sys.argv[2] + "_*.npz"))))
PY
)
  if [ "$rc" -eq 0 ] && findings_ok "$log"; then
    mark "$leg" ok $((SECONDS - start)) "$n" "$samples"
    # 建库成功 = 该腿的 raw 不再被需要；删掉它，dump 批处理才会被放行
    [ "$RECLAIM" = "1" ] && rm -rf "$cap"
  else
    mark "$leg" failed $((SECONDS - start)) "$n" "$samples"
  fi
}

legs=$(ssh "$REMOTE" "ls $TARDIR" | sed 's/\.tar\.gz$//' | sort)
[ -n "$legs" ] || { echo "no legs from $REMOTE"; exit 2; }

group=()
for leg in $legs; do
  [ "$leg" = "$EXCLUDE" ] && { echo "[skip] $leg (eval-reserved)"; continue; }
  done_leg "$leg" && { echo "[skip] $leg (done)"; continue; }
  free=$(df -g "$CORPUS" | awk 'NR==2{print $4}')
  [ "${free:-0}" -lt "$MIN_FREE_GIB" ] && { echo "ABORT: ${free} GiB free on $CORPUS"; exit 3; }
  group+=("$leg")
  if [ "${#group[@]}" -ge "$CONC" ]; then
    for l in "${group[@]}"; do one_leg "$l" & done
    wait
    group=()
  fi
done
for l in "${group[@]:-}"; do [ -n "$l" ] && one_leg "$l" & done
wait
echo "batch done $(date '+%Y-%m-%d %H:%M:%S')"
```

用法：

```bash
nohup nice -n 10 bash ~/ai_depth3/run_mother_set_batch.sh >> ~/ai_depth3/logs_build_full_s2_amc/batch.out 2>&1 &
```
