# 全腿 CPU 解码基线（及逐步加入的 depth3 网络结果）

**用途**：每条**全腿**语料的"经典链（CPU）交付能力"是衡量 depth3 网络的**唯一绝对对照**。
本文档先钉死四条腿的 CPU 基线，之后**逐步**加入较强 depth3 模型的全腿结果。

**口径**（与 PK 一致）：

| 项 | 定义 |
|---|---|
| 交付比特 | 所有 `crc=OK` 单元的 `tbs_bytes × 8` 之和 |
| OK 率 | `crc=OK` 单元数 ÷ 该腿单元数 |
| 尺子 | 5 000 单元子集（`ruler_s2_amc_5000_units.txt`，仅 1102a）|
| 全腿 | 该腿**全部**单元（50 000 或 49 880）|

---



**(★ 目录结构，2026-11-22 00:05 更新：四条腿的测量产物统一在 `full_leg_measurement/`）**

```
~/ai_depth3/full_leg_measurement/
├── corpus/    corpus_s2_amc_ping_<腿>/              ← 原始采集（每腿 9–10 GB，回放用）
├── units/     corpus_s2_amc_ping_<腿>_units.txt      ← 回放用的单元表（测量唯一必需）
├── dumps/     corpus_s2_amc_ping_<腿>/               ← CPU 基线：index*.csv ＋ units.txt
└── results/   full_<模型>.csv                        ← 模型全腿结果（self）与 *_harq.csv
```

| 目录 | 内容 | 用途 |
|---|---|---|
| ★ `full_leg_measurement/corpus/` | 4 条腿的原始采集 | 回放的输入（模型与 CPU 都用它）|
| ★ `full_leg_measurement/units/` | 4 份单元表 | ★ 模型全腿回放的输入 |
| ★ `full_leg_measurement/dumps/` | 4 条腿的 `index.csv` / `index_self.csv` / `index_harq.csv` / `index_harq_only.csv` / `units.txt` | ★ CPU 基线（`stage/rows/tb` 已删，测量不需要）|
| ★ `full_leg_measurement/results/` | `full_A.csv`、`full_c128.csv`、`full_llrp.csv`、`full_swa_s2decay_tail.csv`、`full_swa_s3_tail.csv` … | ★ 模型结果（每单元 status/bits）|
| 训练用 dump（另一条线）| `~/ai_depth3/dump_s2/tb_<腿>/`、`dump_s2_amc/tb_*` | 不是测量，用 `tb_` 前缀 |

**⇒ 规则：测量产物一律在 `full_leg_measurement/` 下（`units/`、`dumps/`、`results/`）；`dump_s2*` 下的 `tb_` 前缀是训练用。**

**(★ 命名约定，2026-11-21 由项目负责人确定）**

| 用途 | 命名 | 例 |
|---|---|---|
| ★ **全腿测量专用**（单元表、测量用 dump）| ★ **`corpus_<腿名>`** | `~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121a_units.txt`、`~/ai_depth3/full_leg_measurement/dumps/corpus_s2_amc_ping_1121a/` |
| 训练数据（母集/数据集）| `datasets_*` / `<腿>_<MOD>_<self|harq>.npz` | `datasets_train_w2v3c` |

**⇒ 一眼就能分清"这条腿是拿来测的"还是"拿来训的"；训练用的 dump 仍沿用 `tb_<腿>` 的历史命名（见数据手册 §6）。**

## 1. ★★★★ 四条腿的 CPU 基线（2026-11-21 dump 实测）

| 腿 | 单元数 | CRC-OK | OK 率 | ★ CPU 交付比特 | bits/OK 单元 | 语料位置 |
|---|---|---|---|---|---|---|
| **1102a** | 50 000 | 40 810 | 81.6% | ★ **114 122 976** | 2 796 | `~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1102a` |
| **1121a** | 50 000 | 38 020 | 76.0% | ★ **649 723 608** | 17 089 | `~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121a` |
| **1121b** | 50 000 | 28 970 | **57.9%** | ★ **323 140 584** | 11 154 | `~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121b` |
| **1121c** | 49 880 | 41 428 | **83.1%** | ★ **562 540 672** | 13 578 | `~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121c` |

**dump 产物**：`~/ai_depth3/full_leg_measurement/dumps/corpus_s2_amc_ping_1121{a,b,c}/index.csv`（1102a 在 `dump_s2_amc/`）。

**⇒ 四条腿的"每 OK 单元比特数"差 6 倍（2 796 vs 17 089）⇒ 它们**不是同一难度的腿**，
⇒ 跨腿比较**只能用"相对 CPU 的百分比"**，绝不能直接比绝对比特数。**

---


## 1b. ★★★★ 四条腿的 **CPU＋HARQ** 交付基线（2026-11-21 23:20 补）

**口径**：`harq_backfill.py` 用**实时链的 HARQ 软合并**结果（`tb_live/` 的 `dd_meta.csv`，飞行中 gNB 的真实解码）
给"首次传输失败、但被同一 HARQ 进程的后续重传救回"的单元补标签 ⇒ 那些块**最终是交付了的**。

| 腿 | 单元 | self OK | ★ HARQ 补回 | ★ 交付 OK | self 比特 | HARQ 补回比特 | ★ **HARQ 总比特** | 相对 self |
|---|---|---|---|---|---|---|---|---|
| **1102a** | 50 000 | 40 810 | 3 496 | 44 306 | 114 122 976 | 10 716 400 | ★ **124 839 376** | **+9.39%** |
| **1121a** | 50 000 | 38 020 | 4 972 | 42 992 | 649 723 608 | 205 299 616 | ★ **855 023 224** | ★ **+31.60%** |
| **1121b** | 50 000 | 28 970 | ★ **8 867** | 37 837 | 323 140 584 | ★ **321 717 488** | ★ **644 858 072** | ★ **+99.56%** |
| **1121c** | 49 880 | 41 428 | 4 094 | 45 522 | 562 540 672 | 130 623 480 | ★ **693 164 152** | ★ **+23.22%** |

**★ 读法（很重要）**

| 腿 | HARQ 增益 | 含义 |
|---|---|---|
| 1102a | **+9.4%** | 经典链的 HARQ 收益中等 |
| 1121a | **+31.6%** | 256QAM 为主 ⇒ 首传失败多，重传救回的多 |
| ★ **1121b** | ★ **+99.6%** | ★ **首传 OK 率只有 57.9% ⇒ 几乎一半的交付来自重传** |
| 1121c | **+23.2%** | 64QAM 为主，居中 |

**⇒ 这彻底改变了"谁强"的结论：只比 self-only 时，1121b 的 CPU 看起来很弱（323 M）；一旦算上 HARQ，它交付
**644 M**，几乎翻倍。⇒ ★ **任何"AI vs CPU"的比较都必须同时给出 self-only 与 ＋HARQ 两列。**


---

## 1c. ★★★★★ 2026-11-22 **四条"困难测试集"（hard test sets）：去掉 1–2 次迭代就过的易 TB**

**为什么**：★ 大多数 TB 经典链 **1–2 次 LDPC 迭代**就解出，CPU 与网络**都有** ⇒ 对区分架构**没有任何信息量**，
只会稀释差异。★ 只保留三类有区分度的单元：

| 类别 | 定义 |
|---|---|
| ★ **KO** | 经典链**始终没解出**（CRC 失败）|
| ★ **self_hard** | 自己那条传输解出，但 **LDPC 迭代 ≥ 3 次** |
| ★ **harq_hard** | 首传失败、靠后续重传救回，且 **LDPC 迭代 ≥ 3 次** |

**⇒ 被去掉的是：self 解出且迭代 ≤ 2 的单元（以及 harq 里 ≤ 2 次的，本批没有）。**

**(a) ★★ 分来源的 CPU 基准（这是这套测试集唯一正确的分母）**

| 腿 | ★ KO（单元 / 比特）| ★ self_hard（单元 / 比特）| ★ harq_hard（单元 / 比特）| ★ 合计（单元 / 比特）|
|---|---|---|---|---|
| **1102a** | 5 694 / **0** | 11 947 / **41 654 784** | 3 496 / **10 716 400** | 21 137 / ★ **52 371 184** |
| **1121a** | 7 008 / **0** | 3 600 / **129 818 936** | 4 954 / **204 468 928** | 15 562 / ★ **334 287 864** |
| **1121b** | 12 163 / **0** | 2 778 / **54 510 536** | 8 865 / **321 673 944** | 23 806 / ★ **376 184 480** |
| **1121c** | 4 358 / **0** | 2 607 / **60 589 264** | 4 085 / **130 294 616** | 11 050 / ★ **190 883 880** |

**★ 三个必须注意的口径**

| # | 口径 |
|---|---|
| ★ 1 | ★ **KO 单元的 CPU 比特恒为 0**（经典链没解出）⇒ 任何模型在这些单元上解出都是**净增益**，是与 CPU 差距的主要来源 |
| ★ 2 | ★ **harq_hard 在 1121b 占 85.5%**（321.7 M / 376.2 M）⇒ ★ **那条腿的"CPU 基准"几乎全部来自重传**；比较必须用**＋HARQ** 口径 |
| 3 | ★ 分母**只能是这张表的合计**，不能再用全腿的 114 M / 650 M / 323 M / 563 M |

**(b) 四个测试集（`~/ai_depth3/full_leg_measurement/testsets_hard/<腿>/`）**

| 腿 | 原单元 | ★ 保留 | 占原腿 | KO | self_hard | harq_hard | ★ CPU 可交付比特 |
|---|---|---|---|---|---|---|---|
| **1102a** | 50 000 | ★ **21 137** | 42.3% | 5 694 | **11 947** | 3 496 | ★ **52 371 184** |
| **1121a** | 50 000 | ★ **15 562** | 31.1% | 7 008 | 3 600 | 4 954 | ★ **334 287 864** |
| **1121b** | 50 000 | ★ **23 806** | 47.6% | 12 163 | 2 778 | 8 865 | ★ **376 184 480** |
| **1121c** | 49 880 | ★ **11 050** | 22.2% | 4 358 | 2 607 | 4 085 | ★ **190 883 880** |

**⇒ 去掉的"易 TB"：1102a 28 863、1121a 34 438、1121b 26 194、1121c 38 830。**

**(c) 每个测试集的三个文件**

| 文件 | 内容 |
|---|---|
| ★ `units.txt` | ★ 保留单元的**裸名**列表（`measure_fulllegen.py` 的第一个参数）|
| `labels.csv` | `unit,category,crc,iterations,tbs_bytes,donor`（★ 便于按类别拆账）|
| `stats.txt` | 该腿的保留比例、三类计数、CPU 可交付比特 |

**(d) 怎么用**

```bash
python3 work_tmp/measurement/measure_fulllegen.py \
    ~/ai_depth3/full_leg_measurement/testsets_hard/<腿>/units.txt \
    <bundle.mlmodelc> <out.csv> \
    ~/ai_depth3/full_leg_measurement/corpus/<腿>
```

**⇒ 生成的脚本：`work_tmp/measurement/make_hard_testsets.py`（阈值 `--min-iterations 3` 可调）。**

**(e) ★ 读这些数字要注意的两件事**

1. ★ **CPU 可交付比特变小**（如 1102a 从 114 M 降到 52 M）——因为易 TB 被剔除，**这是预期的**：
   比较时用"测试集内的 CPU 比特"作分母，**不能**再用全腿的 114 M；
2. ★ **KO 单元占 22–51%** ⇒ 这些测试集**天然偏向困难区间**，任何模型在这里的绝对交付率都会低很多，
   ★ **看相对提升（%）而不是绝对比特**。

## 2. ★ 每条腿的调制构成（决定难度与比特量级）

| 腿 | QPSK | 16QAM | 64QAM | 256QAM | 备注 |
|---|---|---|---|---|---|
| **1102a** | **22 822**（45.6%）| 2 499 | 5 330 | 19 349（38.7%）| ★ **QPSK 为主** ⇒ 每单元比特少 |
| **1121a** | 362（0.7%）| 917 | 8 756 | ★ **39 965（79.9%）** | ★ **256QAM 为主** ⇒ 比特量最大 |
| **1121b** | 7 700（15.4%）| 3 942 | 10 102 | 28 256（56.5%）| ★ **256QAM OK 率只有 37.4%** ⇒ 最难 |
| **1121c** | 205（0.4%）| 2 110 | ★ **34 767（69.7%）** | 12 798（25.7%）| ★ **64QAM 为主** ⇒ 比特量第二 |

**(a) 分调制的 CPU 基线（同一口径）**

| 腿 | 调制 | 单元 | OK | OK 率 | CPU 比特 |
|---|---|---|---|---|---|
| **1102a** | QPSK | 22 822 | 18 141 | 79.5% | 29 498 184 |
| | 16QAM | 2 499 | 2 281 | 91.3% | 8 658 440 |
| | 64QAM | 5 330 | 5 194 | 97.4% | 18 484 472 |
| | 256QAM | 19 349 | 15 194 | 78.5% | 57 481 880 |
| **1121a** | QPSK | 362 | 301 | 83.1% | 559 512 |
| | 16QAM | 917 | 884 | 96.4% | 9 582 624 |
| | 64QAM | 8 756 | 8 070 | 92.2% | 120 760 216 |
| | 256QAM | 39 965 | 28 765 | **72.0%** | 518 821 256 |
| **1121b** | QPSK | 7 700 | 5 310 | 69.0% | 19 161 960 |
| | 16QAM | 3 942 | 3 805 | 96.5% | 36 609 744 |
| | 64QAM | 10 102 | 9 277 | 91.8% | 142 270 800 |
| | 256QAM | 28 256 | 10 578 | ★ **37.4%** | 125 098 080 |
| **1121c** | QPSK | 205 | 5 | ★ **2.4%** | 11 120 |
| | 16QAM | 2 110 | 2 110 | 100.0% | 22 527 328 |
| | 64QAM | 34 767 | 34 642 | 99.6% | 455 758 520 |
| | 256QAM | 12 798 | 4 671 | ★ **36.5%** | 84 243 704 |

**⇒ 三条 1121 腿的**256QAM 悬崖**（OK 率 72% / 37% / 37%）比 1102a（78.5%）更陡 —— 这正是 depth3 应该去打的区间。**

---

## 3. ★★ depth3 网络的全腿结果（逐步加入）

**收录规则**：只收**较强模型**（尺子上 ≥ +1.5% 的，或单模型榜前 3），每条跑**全部四条腿**，
以免"只在容易的腿上好看"。

| 模型 | 算力 | 1121a | 1121b | 1121c | ★ 1102a（self）| ★ 1102a（＋HARQ）| 备注 |
|---|---|---|---|---|---|---|---|
| **CPU（基线）** | — | 649 723 608 | 323 140 584 | 562 540 672 | **114 122 976** | ★ **124 839 376** | §1 / §1b |
| ★★ **C（`--dense-skip`）** | 1× c32 | 待测 | 待测 | 待测 | ★ **116 298 160（+1.91%）** | ★★ **124 381 056（−0.37%）** | ★ **第一个做了 HARQ 对等比较的模型**（2026-11-22，**已更正**，见下方 errata）|
| `s2` 衰减 EMA | 1× c32 | 待测 | 待测 | 待测 | 116 152 792（+1.78%）| 待测（需跑 `measure_harq.py`）| — |
| `c128` 末端 SWA | 1× c128 | 待测 | 待测 | 待测 | 116 040 696（+1.68%）| 待测 | — |
| `A`（mlp-expand 2）| 1× c32 | 待测 | 待测 | 待测 | 115 528 488（+1.23%）| 待测 | — |

**★★ 2026-11-22 07:05 第一个对等结果（C，1102a 腿）**

| 口径 | CPU | ★ C | 差 | 说明 |
|---|---|---|---|---|
| self-only | 114 122 976 | ★ **116 298 160** | ★ **+1.91%** | 都不用 HARQ ⇒ 对等 |
| ★ **＋HARQ** | ★ **124 839 376** | ★ **124 381 056** | ★ **−0.37%** | ★ **CPU 用重传、C 也用重传 ⇒ 对等**（★ 2026-11-22 更正：早先的 126 698 672 重复计了 838 个单元）|

| 细节 | 值 |
|---|---|
| C 的 self OK 单元 | 41 586 |
| HARQ 单元：C 解出 | ★ **3 327 / 3 496（95%）**，贡献 10 400 512 bit |
| CPU 在同一批单元 | 3 496 / 3 496，贡献 10 716 400 bit |
| ★ 读数 | ★★ **更正：HARQ 把 C 从 +1.91% 压到 **−0.37%** —— AI 在"双方都用 HARQ"时**并不领先**。**
| ★★ **errata** | ★ 早先算出的 `C+HARQ = 126 698 672` **重复计入了 838 个单元（2 317 616 比特）**：这些单元 C 在**自己的传输上**就能解（已在 self 里计过），HARQ 跑又解了一次。**去重后 = 116 298 160 ＋ 10 400 512 − 2 317 616 = 124 381 056。** |

**⇒ 下一批要跑的四条腿全腿：`C`（最强结构）与 `s2 衰减 EMA`（最强配方）。**

---

## 4. ★ 测量方法（复现用）

```bash
# 单元表（每腿一份）
~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121a_units.txt      # 50 000
~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121b_units.txt      # 50 000
~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121c_units.txt      # 49 880
~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1102a_units.txt            # 1102a，50 000

# 回放（模型侧）
python3 work_tmp/measurement/measure_fulllegen.py \
    <units.txt> <bundle.mlmodelc> <out.csv> <corpus 目录>

# CPU 基线（本表第 1 节）
bash lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_tb.sh \
    --corpus ~/ai_depth3 --out ~/ai_depth3/full_leg_measurement/dumps/corpus_s2_amc_ping_1121a \
    --jobs 4 --binary build/lib/phy/upper/channel_processors/ul_chain_replay_cpu \
    ~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121a
```

**耗时实测**：CPU dump **3 腿并行（各 4 worker）约 26 分钟**；模型全腿约 **80–95 min/模型/腿**。

---

## 4b. ★★★★ HARQ 测量方法（**对等比较的另一半，必做**）

**为什么必须做**：`self-only` 只统计"该单元自己的首传就解出"的比特；把重传合并算进来后，四条腿的 CPU 交付分别
涨 **+9.4% / +31.6% / +99.6% / +23.2%**（§1b）⇒ **只比 self-only 会高估 AI 的相对优势。**

**(a) CPU 侧：把"重传救回"的块补上（产出本表 §1b）**

```bash
python3 lib/phy/upper/ai_signal_processors/depth3/ai_train/harq_backfill.py     --leg      ~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121a     --index    ~/ai_depth3/full_leg_measurement/dumps/corpus_s2_amc_ping_1121a/index.csv     --out      ~/ai_depth3/full_leg_measurement/dumps/corpus_s2_amc_ping_1121a     --live-tb  ~/ai_depth3/full_leg_measurement/corpus/corpus_s2_amc_ping_1121a/tb_live
```

| 项 | 说明 |
|---|---|
| ★ `--live-tb` | ★ **飞行中 gNB 的真实解码**（`tb_live/dd_meta.csv` ＋ `tb_*.bits`）：这些解码**用了实时链的 HARQ 软合并**，离线回放复现不了 |
| 产物 | `index_self.csv`（自己解出的）、★ `index_harq_only.csv`（**靠重传救回的单元 ＋ donor 列**）|
| 判据 | `label_from ∈ {self, harq, none}`；★ **交付 = self ＋ harq** |
| 实测耗时 | 纯 Python 后处理，**每腿 1–2 分钟** |
| 实测数量 | 1102a 3 496 / 1121a 4 972 / 1121b 8 867 / 1121c 4 094 个 HARQ 单元 |

**(b) AI 侧：把同样的重传单元也跑一遍（`--harq-chain`）**

```bash
python3 work_tmp/measurement/measure_harq.py     ~/ai_depth3/full_leg_measurement/dumps/corpus_s2_amc_ping_1121a/index_harq_only.csv     <bundle.mlmodelc>  <out_harq.csv>  <corpus 目录>  [limit]
```

| 项 | 说明 |
|---|---|
| ★ 底层命令 | `ul_chain_replay <donor> --also <unit> --harq-chain --phy_pipeline npu --pusch_receiver_backend ai --pusch_ldpc_decoder_type generic` |
| ★ 机制 | ★ **把同一 HARQ 进程的多次传输喂给同一个 processor ⇒ 重传与先前的软比特合并**（与后端无关，AI 臂同样可用）|
| 产物 | `unit,status,bits`，与全腿 CSV 同格式 ⇒ **可直接相加** |
| 实测耗时 | 约 **1 s/单元** ⇒ 1102a ~1 h、1121b ~2.5 h（可并行 `--jobs`-style 拆分）|

**(c) ★ 对等比较的三个数字（每腿每模型都要报）**

```
AI self        = measure_fulllegen.py 的 OK 比特和
AI ＋HARQ      = AI self ＋ measure_harq.py 的 OK 比特和
CPU ＋HARQ     = 本表 §1b                       ← 只有这个才与 AI＋HARQ 对等
```

**(d) 已知道的坑**

| 坑 | 说明 |
|---|---|
| ★ 参数顺序 | `--harq-chain` 由工具按 slot 自行排序，**主参数与 `--also` 的先后不影响结果**（本机两种顺序实测同结论）|
| ★ donor 可能跨腿 | `index_harq_only.csv` 的 donor 可能落在**同一腿**内（本表四条腿均如此）；跨腿时需把两条腿的语料都挂上 |
| ★ 首传 OK 的单元**不要**再跑 HARQ | 它们已在 self 里计过，重复计会虚高 |

