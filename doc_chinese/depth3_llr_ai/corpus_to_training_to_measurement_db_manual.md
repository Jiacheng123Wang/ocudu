# 语料采集 → dump → 建库 → 训练 → **测量**：全过程说明书

> **版本**：2026-11-06（香港，**更名**：原名 `corpus_to_training_db_manual.md`；内容已含**测量**与**实链时延**）；
> **2026-11-23 增补 §9.2**（macOS/Apple Silicon 训练路径：两轮改动、实测账、对 CUDA 路径的逐条影响）｜
> **范围**：从 gNB 起飞采集一条腿 → dump → 建库 → 训练数据集 → 训练 → **测量（交付比特 ＋ 实链时延）**。
> **性质**：操作手册。每一步都写清**命令**、**判据**、**产物**与**坑**。
> **本文的权威性来自两处**：① 文档里**时间最晚**的那一份（见 §0 的时间线）；② **2026-11-04 在机器上实测过的**（凡实测处标 ★实测）。

---

## 0. 以谁为准：五份文档的时间线与分工

| 文档 | 最后更新 | ★ 现在还有效的部分 | ★ 已过时 / 不要照抄 |
|---|---|---|---|
| `llr_ai_detection/collection_runbook_2026-10-25.md` | 2026-10-25 | 段的划分逻辑（A/B/C/D）、每段判据、`FLIGHT.txt` 的做法、§7 的坑 | **采集命令**（当时还没有 `run_collect_leg.sh` 的采集默认值）、`OCUDU_UL_DUMP_COUNT=20000`、`MIN_RB/RV` 过滤器 |
| `llr_ai_detection/corpus_collection_procedure.md` | 2026-10-15 | 前置检查的思路（二进制是否带采集功能、目录能否写、AMF/bind 地址）、**iperf3 `-R` 是上行**、手机地址每次 attach 会变 | **采集配置名**（`..._llr_collect.yml` 已被 `pinned_mcs*.yml` 取代）、手动 `export OCUDU_*` 的做法（现在由腿脚本派生） |
| `llr_ai_detection/training_corpus_construction.md` | 2026-10-25 前后 | 语料"形状"与空格的定义、三维分箱口径 | 早期建库/合集的脚本流水线 |
| `depth3_llr_ai/corpus_capture_history.md` | 2026-10/11 | 逐腿历史、事故记录、可追溯性 | —— |
| ★★ `depth3_llr_ai/corpus_capture_and_new_datasets_build.md` | ★ **2026-11-04** | ★★ **唯一与当前代码一致的流程**：`LEG_NAME` 映射（§11.10）、一次性/每腿环境（§11.13）、T7 采集（§11.14）、dump 与建库（§52–§54、§58、§62）、母集直调（§15/§16）、monitor 抽取（§59） | 早期轮次（§1–§40）里的旧路径与旧命名 |

**⇒ 一句话**：**采集看本文 ＋ `corpus_capture_and_new_datasets_build.md` 的 §11.x／§52 之后；建库看本文 ＋ 该文档 §15/§16；训练看 `machine_compute_capability_and_model_train_memo.md`。**

---

## 1. 机器、路径与角色

| 项 | 值 |
|---|---|
| 开发/采集机 | **本机**（Mac，M4 Pro，14 核/64 GB）；gNB 与采集都在这台上跑 |
| 语料根 | ★ **`/Volumes/T7_Corpus/corpus`**（T7 外置盘；`corpus/` 里每个子目录是一条腿）|
| dump 根 | **`~/ai_depth3/dump_s2`**（每条腿 `tb_<腿名>/`）|
| 母集（库）根 | **`~/ai_depth3/datasets_full_s2`**（每个文件 `<腿>_<调制>_<self\|harq>.npz`）|
| 训练机（香港）| **107**（RTX 3060 6 GB，16 线程/15 GB）；**广州不可达** |
| 训练机（广州）| **106**（Xeon 8C/16T，16 GB，28.7 samples/s）、**221**（i7-9750H，32 GB，15.6 samples/s）；均无 GPU |
| 回放/尺子机 | **203**（M2，可跑 `ul_chain_replay` 与 Core ML）|

---

## 2. 准备工作

### 2.1 代码

```bash
cd ~/dev/ocudu && git pull --ff-only          # 当前应在 apple-silicon 分支
git log --oneline -1                          # 记下这个提交，它就是本条腿/这批库的代码版本
```

> ★ 采集出来的语料、dump、库，都要能回答"用哪版代码做的"。**腿脚本会把构建戳与源码脏度写进日志**；见 §2.4 的 `LEG_ALLOW_DIRTY`。

### 2.2 一次性环境变量（每台机器一次）

```bash
# 语料落在 T7 上：腿脚本从 COLLECT_CORPUS 派生 logs/、cap、tb_live 三处路径
export COLLECT_CORPUS=/Volumes/T7_Corpus/corpus

# 每条腿的捕获预算（★ 一条腿 50 000 个接收；1103 那批就是 50 000）
export OCUDU_UL_DUMP_COUNT=50000

# 是否额外写 LLR（0 = 不写；只有要做软比特对照时才设 1）
export OCUDU_UL_DUMP_LLR=0

# 每条调度决策一行（SNR/OLLA/MCS），分析 AMC 行为时必需
export OCUDU_LA_LOG_EVERY=1
```

| 变量 | 默认（脚本内） | 作用 | 坑 |
|---|---|---|---|
| `COLLECT_CORPUS` | `doc_chinese/llr_ai_detection/work_tmp/corpus` | 语料树根 | ★ 不改就会写进**仓库目录** |
| `OCUDU_UL_DUMP_COUNT` | **100 000** | 本腿最多写多少个接收 | 写满后**不再写**，日志有 `budget exhausted` |
| `OCUDU_UL_DUMP_LLR` | **0** | 是否写 LLR | 开它会显著增大体积 |
| `OCUDU_LA_LOG_EVERY` | 1 | LA/AMC 日志频率 | 关掉就没有 MCS/SNR 轨迹 |
| `OCUDU_UL_DUMP_MIN_RB` / `MAX_RB` | **0 / 0** | 宽度过滤 | ★ **默认 0 = 不过滤**；老文档让设 `MIN_RB=43` 是**为旧目标服务**，现在不要设 |
| `OCUDU_UL_DUMP_RV` | 不设 | 只收某个 rv | ★ 老文档设 `RV=0`；**现在不设**（重传也要，建库时按 self/harq 分类）|
| `LEG_CONFIG` | `configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml` | 小区配置（决定调制/带宽/功控）| ★ **腿名不决定调制，配置才决定** |
| `LEG_NAME` | 由 label 派生 | 腿名（＝目录名）| 见 §3.1 的映射 |
| `LEG_ALLOW_DIRTY` | 不设 | 允许带未提交改动起飞 | ★ 只在明确知道改了什么时用 |
| `LEG_SKIP_BUILD` | 不设 | 跳过起飞前的自动构建 | 用旧二进制时才有意义 |
| `LEG_LOGDIR` / `OCUDU_UL_DUMP` | 由 `COLLECT_CORPUS` 派生 | 日志 / 捕获前缀 | ★ `OCUDU_UL_DUMP` 是**文件名前缀**，不是目录 |

### 2.3 配置文件：pinned 与 amc

四种调制各有一个"钉住 MCS"的配置（每个都是 `min_ue_mcs = max_ue_mcs`）：

| 配置 | 钉住的 MCS | 调制 | 用途 |
|---|---|---|---|
| `configs/pinned_mcs02.yml` | 2 | QPSK | 只要 QPSK 的腿 |
| `configs/pinned_mcs06.yml` | 6 | 16QAM | —— |
| `configs/pinned_mcs12.yml` | 12 | 64QAM | —— |
| `configs/pinned_mcs20.yml` | 20 | 256QAM | 只要 256QAM 的腿 |

**★ 1103 那批是 `amc` 腿**：用的是 **`configs/pinned_mcs04.yml`** 的改版 —— 把 `min_ue_mcs/max_ue_mcs` **注释掉**（让 AMC 自己走），
并把 `tx_gain/rx_gain` 抬到 **80/75**。★实测（读 1103a 的 banner）：`cell config : configs/pinned_mcs04.yml`、
`min_rb 0, max_rb 0, rv any`、`budget 50000`。

```bash
# 起飞前核一眼本机地址与配置里的 bind_addrs 是否一致（不一致脚本会拒绝并打印实际 IP）
grep -n "bind_addrs\|amf:" configs/pinned_mcs04.yml | head
ifconfig en0 | grep "inet "
```

### 2.4 硬件与前置检查

| 检查 | 命令 | 通过条件 |
|---|---|---|
| B200 在位、GPSDO 锁星 | 看 gNB 启动日志的 RF/clock 行 | `clock_source=gpsdo` 生效、无 clock 告警 |
| 采集功能编进二进制 | `strings build/apps/gnb/gnb \| grep -c "NOT CAPTURING"` | ≥ 1 |
| 语料盘可写、空间够 | `df -h /Volumes/T7_Corpus` | 每腿约 **4–6 GB**（50 000 个接收）|
| 手机能附着 | 见 §3.3 | gNB 日志出现 UE 接入；手机有 IP |
| 到核心网可达 | `ping -c2 <AMF/CN 地址>` | 通 |

> ★ **历史事故**：语料目录不存在时 `fopen` 静默失败，跑满 8 分钟**一个文件都没写**，gNB 也不报错。
> 现在腿脚本会在起飞前建好 `cap` 的父目录，并且第一个采集落地时打印
> `[ul_capture] capturing: writing <前缀>_<sfn>_<slot>_<rnti>.*`。**看到这一行才算采起来了**；看到
> `NOT CAPTURING` 立刻停腿。

---

## 3. 起飞 gNB、手机接入

### 3.1 命令（★实测，最近一次成功的就是这条形状）

```bash
cd ~/dev/ocudu

# 一条腿：模式 cpu ＋ 腿名
LEG_ALLOW_DIRTY=1 sudo -E bash doc_chinese/llr_ai_detection/wip/run_collect_leg.sh cpu s2_amc_iperf_1103o
```

| 参数 | 含义 |
|---|---|
| `cpu` | 流水线模式。**采集一律用 `cpu`**：语料记录的是 PDU 与网格，与哪个接收机跑无关 |
| `s2_amc_iperf_1103o` | **腿名**（同时是目录名、日志名、后面 dump/建库的名字）|
| `sudo -E` | 保留环境变量（`COLLECT_CORPUS`、`OCUDU_*`）；腿脚本要以 root 跑 gNB |
| `LEG_ALLOW_DIRTY=1` | 允许源码有未提交改动（不设则脚本会拒绝起飞并打印 diff 指纹）|

**腿名规则**（`run_collect_leg.sh` 里实现）：

| 输入 label | 得到的目录名 |
|---|---|
| `w2_harq_16QAM_ping_1030a` | `s2_16QAM_ping_1030a`（`w2_` → `s2_`，`harq_` 去掉）|
| `s2_amc_ping_1103a` | 原样保留（幂等）|
| `LEG_NAME=… ` 显式给出 | 不被覆盖 |

### 3.2 起飞后看什么（前 30 秒）

| 日志行 | 含义 |
|---|---|
| `cell config   : configs/pinned_mcs04.yml` | ★ 用的哪份配置（**这就是本腿调制/带宽的证据**）|
| `[ul_capture] plan: writing /Volumes/.../cap.* budget 50000 reception(s) ... (min_rb 0, max_rb 0, rv any)` | 采集参数 |
| ★ `[ul_capture] capturing: writing ...` | **第一个采集落地** |
| `Cell scheduling was activated` | 小区起来了，**这时才接手机** |
| `[ul_lane_exec] PUSCH/SRS concurrency = 2 ...` | UL 通道并发（与核数/带宽有关）|

### 3.3 手机接入

1. 手机关飞行模式 → 等注册（PLMN 用配置里的那个）。
2. 在 gNB 日志里确认 UE 上下文建立（CU-CP 的 UE 建立行）。
3. 在**核心网侧**确认手机拿到地址（UPF 会话日志里最后一条 `IPv4[…]` 就是它）。
4. **不要**在 gNB 上跑 iperf：gNB 只是管道，**iperf3 的 server 在手机上**。

> ★ 手机地址**每次 attach 都会变**；用之前先 `ping -c2 <手机IP>` 确认，并确认 iperf 端口（默认 5201）能连上。
> 工具 `run_iperf_ladder.sh` 会自动从 UPF 日志取候选地址并逐个验证。

### 3.4 打流量（决定授权宽度）

**为什么要打流量**：调度器只在上行缓冲堆积时给宽分配；流量不足就退化成窄分配。

```bash
# ① 最简单的上行：ping（小包、高频）
ping -i 0.05 -s 32 <手机IP>

# ② iperf3 上行：★ -R 是 reverse ⇒ 手机发、gNB 收 = 上行
iperf3 -c <手机IP> -R -t 300 -P 4

# ③ 阶梯：让"速率 → 授权宽度"自己走出来（推荐用它采宽度谱）
doc_chinese/llr_ai_detection/wip/run_iperf_ladder.sh                 # 地址自动找，默认阶梯
doc_chinese/llr_ai_detection/wip/run_iperf_ladder.sh 10.45.0.9 0.5M 1M 3M
```

**★ 实测规律**（`run_iperf_ladder.sh` 头注释里记的数）：**上行速率 1 → 3 → 5 → 8 Mbps 时，中位授权 43 → 46 → 50 → 51 PRB**；
1 Mbps 时授权从 2 到 51 PRB 都出现，8 Mbps 时基本永远是全带。**⇒ 想要窄授权就飞慢一点。**

### 3.5 采集期间的两个即时检查

```bash
L=/Volumes/T7_Corpus/corpus/<腿名>
# 捕获数（每 30 秒看它涨不涨）
find "$L" -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' ! -name '._*' | wc -l
# 最近 20 个的 SNR（_ce.txt 里 snr 是线性值，dB = 10log10）
for f in $(ls -t "$L"/cap_*_ce.txt | head -20); do
  awk '{for(i=1;i<=NF;i++) if($i ~ /^snr=/){split($i,a,"="); printf "%.1f ", 10*log10(a[2])}}' "$f"
done; echo
```

**判读**：不涨 = 当前流量产生不了新的接收（加大速率/换位置/看手机是否掉线）；SNR 中位远低于目标 = 调距离或衰减。

---

## 4. 收腿

```bash
# ★ 只按一次 Ctrl-C，然后等报告块打完（脚本会校验"有没有报告"，没报告不算一条腿）
```

**降落后 30 秒核对**：

```bash
L=/Volumes/T7_Corpus/corpus/<腿名>
find "$L" -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' ! -name '._*' | wc -l   # 捕获数
find "$L/tb_live" -maxdepth 1 -name 'tb_*.bits' 2>/dev/null | wc -l                 # 活体 TB 数
sync
```

**两个数任一为 0 ⇒ 这条腿白跑**（捕获有、TB 无，后面回收不到）。

**收尾记录**（跟着语料一起进库，事后能对上条件）：

```bash
cat > "$L/FLIGHT.txt" <<EOF
date=$(date -Iseconds)
leg=<腿名>
config=$(grep -m1 "cell config" "$L"/logs/*.stderr | awk '{print $4}')
position=<室内/走廊/…>
distance_m=<估计>
attenuator_db=<若使用>
observed_pusch_snr_db=<从 LA 日志读>
traffic=<ping -s 32 | iperf3 -R -P4 | ladder>
notes=
EOF
```

---

## 5. 重复下一条腿

1. **换腿名**（不许复用已有目录）：`s2_amc_ping_1103b`、`s2_amc_iperf_1103i` …
2. 若要换调制：换 `LEG_CONFIG`（pinned_mcs02/06/12/20），**并让腿名里的调制前缀与之一致**。
3. 重新起飞（同 §3.1 的命令，只改腿名）。
4. 每腿结束都做 §4 的核对与 `FLIGHT.txt`。

> ★ **不要**在同一个目录上续采：`OCUDU_UL_DUMP` 指向已存在目录会接着编号，事后无法归因。

---

## 6. dump（把捕获变成"可解码的 TB ＋ 索引"）

**这一步做什么**：对每个捕获跑**经典链（CPU 路径）**，把 CRC 通过的 TB 比特写出来，并给每个单元一行索引。

```bash
cd ~/dev/ocudu

# ★ 每条腿一个输出目录：dump_s2/tb_<腿名>（★ 绝不能给 dump 根目录）
LIB=lib/phy/upper/ai_signal_processors/depth3/ai_train
bash $LIB/dump_tb.sh \
  --corpus /Volumes/T7_Corpus/corpus \
  --out    ~/ai_depth3/dump_s2/tb_s2_amc_iperf_1103o \
  --jobs   3 \
  --binary build/lib/phy/upper/channel_processors/ul_chain_replay_cpu \
  /Volumes/T7_Corpus/corpus/s2_amc_iperf_1103o
```

| 项 | 说明 |
|---|---|
| CPU 路径 | 工具内部钉死 `--phy_pipeline cpu --pusch_receiver_backend classic`，二进制优先用 `ul_chain_replay_cpu` |
| `--jobs N` | N 个单元并行（每单元一个进程）|
| 并行度 | ★实测：**4 条腿 × 3 worker** 在 14 核上已到负载上限（17.8/14），**不要更高** |
| 单单元 | ★实测 **47 ms**（5 PRB 256QAM）|
| 产物 | `<out>/index.csv`、`<out>/units.txt`、`<out>/tb/<单元>.bits`（另有 `stage/`、`rows/` 草稿，**可删**）|
| 判据（三条） | `index.csv` 行数 ＝ **有 `.bin` 的单元数** ＝ `units.txt` 行数；不等就重跑 |
| 耗时/体积 | ★实测：50 000 单元/腿，4 腿并行 **2 h 8 min**（15 腿）；契约产物约 **190 MB/腿**（`stage/` 另有 ~5.5 GB/腿 草稿）|

**对账命令**：

```bash
python3 - ~/ai_depth3/dump_s2/tb_<腿> <<'PY'
import sys, os
out = sys.argv[1]
us = [l.strip() for l in open(os.path.join(out, "units.txt")) if l.strip()]
with_bin = sum(1 for u in us if os.path.exists(u[:-4] + ".bin"))
rows = sum(1 for _ in open(os.path.join(out, "index.csv"))) - 1
ok = sum(1 for l in open(os.path.join(out, "index.csv")) if ",OK," in l)
print(f"units={len(us)} with_bin={with_bin} index_rows={rows} crc_ok={ok}")
PY
```

---

## 7. 建库（母集）

> ★★ **2026-11-27 起，建库的完整操作、验收与实测见 `mother_set_build_runbook.md`（以那份为准）。**
> 本节保留"母集是什么、为什么不用 `select_by_bin`"的论述；但下面的**命令、环境变量默认值、路径与实测数已过时**
> （旧 `OUT=~/ai_depth3/datasets_full_s2`、单腿 22 min/11.4 GB 的旧账、以及"npz 还没有 `iterations` 字段"的旧结论）。

**母集 ＝ 一条腿里所有可解码的 TB**，按 **(调制 × 来源)** 写成 `<腿>_<MOD>_<self|harq>.npz`。

```bash
bash ~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh <腿> [<腿> ...]
```

**环境变量**（这套组合 ★实测可用）：

```bash
export TRAIN=/Users/jiachengwang/dev/ocudu          # 只用于 PY 默认值与 build/ 检查
export DUMP_ROOT=/Users/jiachengwang/ai_depth3/dump_s2
export CORPUS_ROOT=/Volumes/T7_Corpus/corpus
export OUT=/Users/jiachengwang/ai_depth3/datasets_full_s2
export PY=/Users/jiachengwang/ai_ce_work/venv/bin/python
export JOBS=1        # 一条腿内几对同时建（内存 × JOBS）
export CHUNK=8000    # ★ 单进程样本上限；多腿并行时必须设，否则单进程峰值 ~11 GB
```

| 步 | 脚本做什么 | 判据 |
|---|---|---|
| 1 | `harq_backfill.py` 给每个单元打 **self / harq** 标 | 写出 `index_self.csv`、`index_harq_only.csv` |
| 2 | 扫捕获头统计**实际存在的调制**（★ 不从腿名推）| `modulations present:` 列出几种 |
| 3 | 每个 (调制, 来源) 直接调 `build_depth3_dataset.py`（**★ 不走 `select_by_bin`**）| 每个 npz 写出 |
| 4 | `check_depth3_dataset.py` 审计本腿 | **`findings: none`** |

> ★ **为什么不用 `select_by_bin.py`**：它是**抽样**工具（按箱配额 2500/250），
> 而且"无限配额"这条路**以前静默沿用抽样账本**、不报错地浪费过数小时（文档 §15）。**母集就要全量，直接调 builder。**

**★实测（2026-11-04）**：

| 项 | 值 |
|---|---|
| 单腿（单调制）| **22 min**、峰值 **11.4 GB**、产物 2.47 GB / 42 797 样本、审计 `findings: none` |
| 4 腿并行（`CHUNK=8000`）| 每腿 **39–41 min**、单进程 2–3 GB、`files=8`（4 调制 × self/harq）|
| 输出体积 | 约 **3–6 GB/腿**（视 harq 群体大小）|

---

## 7.1 分箱盘点（**抽取之前必须做的一步**）

母集建好之后、抽训练集之前，先回答一个问题：**这份语料到底覆盖了哪些格子、每格有多少**。
不看这一步就抽，等于闭着眼睛配额。

### 7.1.1 箱的定义（四个工具共用一套边界）

**4 调制 × 4 宽度 × 7 SNR × 2 群体 ＝ 112 箱**：

| 维 | 边界 | 代码出处 |
|---|---|---|
| 调制 | QPSK / 16QAM / 64QAM / 256QAM | — |
| 宽度（`alloc_nof_prb`）| `<=3`、`<=8`、`<=24`、`>24` | `census_bins.WIDTH_EDGES = [3, 8, 24]` |
| SNR（`snr_nominal`，dB）| `<0`、`0-6`、`6-10`、`10-14`、`14-20`、`20-30`、`>=30` | `census_bins.SNR_EDGES = [0,6,10,14,20,30]` |
| 群体 | `self`（首传自己解出）/ `harq`（靠重传捞回）| 文件名 `_self.npz` / `_harq.npz` |

★ **边界只有一个出处**：`census_bins.py` 的 `width_band()` / `snr_band()`。
`census_s2_mother.py`、`select_by_bin.py`、`split_depth3_stratified.py` 都是同一套，**所以四者的箱号不可能漂移**。

### 7.1.2 两种盘点，回答两个不同的问题

| 盘点 | 工具 | 读什么 | 回答 |
|---|---|---|---|
| **捕获侧** | `ai_train/census_bins.py` | 每条腿的 `cap_*.txt` 侧车（**不含** `_ce.txt`）| ★ **采集覆盖了什么**（含解不出的样本）|
| ★ **母集侧** | ★ **`ai_train/census_mother_set.py`**（2026-11-04 入库）| 母集 npz 里的 `qm` / `alloc_nof_prb` / `snr_nominal` | ★★ **抽取能用到什么**（只含可解码样本）|

★ **抽训练集要看母集侧的箱** —— 抽取工具配额的对象就是母集的样本；
捕获侧的箱用来判断"**下一腿该补哪个格子**"。

### 7.1.3 命令

```bash
PY=~/ai_ce_work/venv/bin/python
LIB=~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train

# ① 捕获侧（可增量：--ledger 记住已数过的腿；--force 重数时是"替换"不是"累加"）
$PY $LIB/census_bins.py --jobs 14 \
    --csv ~/ai_depth3/census_captures.csv \
    --ledger ~/ai_depth3/census_ledger.json \
    /Volumes/T7_Corpus/corpus/s2_amc_ping_1103a /Volumes/T7_Corpus/corpus/s2_amc_iperf_1103i

# ② 母集侧（一次扫完整个目录；322 个 npz / 2.49 M 样本约 3 分钟）
$PY $LIB/census_mother_set.py --dir ~/ai_depth3/datasets_full_s2 \
    --csv ~/ai_depth3/census_s2_bins.csv --caps 2500:250,8000:800   # 逐箱表 ＋ 均匀抽取投影
```

**输出格式**：`census_bins.py --csv` 写 `leg,modulation,width,snr,n`（另有 `ALL` 汇总行）；
`census_s2_mother.py` 写 `modulation,width,snr,population,samples`。

### 7.1.4 怎么用这份盘点选抽取（**均匀，不按尺子加权**）

**★★★ 口径（2026-11-04 用户定）**：**训练集按 bin 均匀抽取**（每箱等量、不足全取），
**不要按尺子的构成去加权** —— 尺子只是**一条腿的随机抽样**，按它加权等于**让模型迎合尺子**，不公平也不可外推。

**均匀抽取的总量与构成可以事先算出来**：`总量 = Σ_箱 min(该箱样本数, cap)`。

```bash
# 例：算 8000/800 能抽多少、构成如何（读 census_s2_bins.csv 即可）
python3 - <<'PY'
import csv, collections
c = {(r["modulation"], r["width"], r["snr"], r["population"]): int(r["samples"])
     for r in csv.DictReader(open("/Users/jiachengwang/ai_depth3/census_s2_bins.csv"))}
for cs, ch in ((2500, 250), (5000, 500), (8000, 800)):
    take = {k: min(v, cs if k[3] == "self" else ch) for k, v in c.items()}
    tot = sum(take.values()); by = collections.Counter()
    for k, v in take.items(): by[k[0]] += v
    print(f"cap {cs}/{ch}: {tot:,} samples, mix " + " ".join(f"{m} {100*by[m]/tot:.0f}%" for m in
          ("QPSK", "16QAM", "64QAM", "256QAM")))
PY
```

### 7.1.5 空箱就是采集的缺口（但先分清"难产生"与"漏采"）

**★ 低阶调制 × 低 SNR**、**256QAM × <0 dB** 这些格子**物理上就难产生**（低 SNR 用低阶、高 SNR 才用高阶）⇒
**空着是正常的**，不要为了填满它去硬采；但它们正是"模型可能遇到而语料没见过"的边缘 ⇒
**要补就用衰减把高阶调制压到低 SNR**，而不是换位置碰运气。

**★ 2026-11-04 的 s2 实例**（详见 `corpus_capture_and_new_datasets_build.md` §65）：
2 488 175 个样本、**197/224 箱非空**、27 个空箱；均匀抽取 2 500/250 → **140 940**，8 000/800 → **342 368**。

## 8. 训练集 / 验证集 / monitor 集的抽取

母集是"什么都有"的库；训练要的是**按箱受控**的抽取。

### 8.1 训练集：按三维分箱抽（`draw_train_set_w2.py`）

```bash
PY=~/ai_ce_work/venv/bin/python
LIB=~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train

# 先看抽多少（不写盘）
$PY $LIB/draw_train_set_w2.py --in ~/ai_depth3/datasets_full_s2 --out /tmp/dry \
    --self-cap 2500 --harq-cap 250 --seed 20261103 --dry-run

# 实抽：输出目录是 mmap 友好的"一数组一 .npy"
$PY $LIB/draw_train_set_w2.py --in ~/ai_depth3/datasets_full_s2 \
    --out <训练机>/data/datasets_train_w2 \
    --self-cap 2500 --harq-cap 250 --seed 20261103
```

| 参数 | 含义 | ★ 坑 |
|---|---|---|
| `--self-cap` / `--harq-cap` | **每箱**上限（不是总量）| ★ 箱有 **112 个**（4 调制 × 4 宽度 × 7 SNR × 2 来源）⇒ 总量上界 = 112 × cap |
| `--seed` | 可复现 | 记进文档 |
| `--dry-run` | 只报数不写 | 先跑它 |

### 8.1b ★★★★★ 2026-11-22 **抽取原则补一条：难度维（临界样本）——以前完全没做**

**(a) 现有原则与其缺口**

| 维度 | 现状 | 缺口 |
|---|---|---|
| **来源**（self/harq） | ✓ 已分（文件名 `_self` / `_harq`）| — |
| **调制 × 宽度 × SNR** | ✓ 已分箱、按箱均匀抽（§7.1.4、§8.1）| — |
| ★ **难度**（这张 TB 有多难解）| ★ **完全没做** | ★ 训练集里"临界样本"只占 ~2%，而 AI 的失分**几乎全在那里** |

**★ 依据（2026-11-22 实测）**

| 项 | 数据 |
|---|---|
| AI（C）在 1102a 全腿的失分 | **118 个单元**，其中 **2/3 需要 CPU 4–6 次 LDPC 迭代**（两者都 OK 的组中位数只有 2 次）|
| 失分单元的配方 | **256QAM × 窄带（≤3 或 ≤8 PRB）× 24–28 dB** |
| 这些单元在训练集里多吗 | ★ **临界样本（≥4 次迭代）只占 CRC-OK 的 0.7–2.2%**（1121a 2.0%、1121c 2.2%、1103h 0.7%）|
| 三格样本总量 | w2v3c 里 QPSK/>24 8 651、256QAM/≤8 22 950、256QAM/≤3 36 347 ⇒ **不缺数据，缺的是"临界"这一维的权重** |

**(b) ★ 临界样本的定义（唯一出处：dump 的 `iterations`）**

| 项 | 规则 |
|---|---|
| 定义 | ★ `iterations`（经典链解出这张 TB 用的 LDPC 迭代次数）**≥ 4** 记为"临界"；**≥ 5** 记为"极临界" |
| 出处 | ★ **dump `index.csv` 的 `iterations` 列**（§6）；母集 npz **目前没有**这个字段 |
| join 方式 | 母集 npz 的 `unit`（去掉 `@...` 后缀）↔ `index.csv` 的 `unit`；★ **实测 100% 命中（579/579）** |

**(c) ★ 抽取规则（在现有三维分箱之上加一维权重，不改箱）**

```
每个箱（调制 × 宽度 × SNR × 来源）内：
  1. 先按现有 cap 均匀抽（self 2500 / harq 250 之类）—— 保持"覆盖面"
  2. 再对箱内的临界样本加权：iterations>=4 的样本 ×4，iterations>=5 的 ×8（同一箱内替换抽样）
  3. 每箱仍然受 cap 约束（★ 不能让临界样本把整箱挤满，否则覆盖面会丢）
```

| 参数（建议命名）| 含义 |
|---|---|
| `--w-critical 4` | `iterations>=4` 的样本抽取权重 |
| `--w-critical5 8` | `iterations>=5` 的权重 |
| `--keep-unit` | ★ **训练集保留 `unit` 字段**（现在 `datasets_train_w2v3c` 没有它 ⇒ 无法事后审计/回标）|

**(d) ★ 两个必须改的工具（下一步）**

| 序 | 工具 | 改动 |
|---|---|---|
| ★ 1 | `build_depth3_dataset.py` | ★ 母集 npz 增加 `iterations` 字段（dump 里现成）|
| ★ 2 | `draw_train_set_w2.py`（或后继抽取器）| ★ 增加 `--w-critical*` 与 `--keep-unit` |

**(e) ★ 三个纪律（不许打折）**

1. ★ **评估腿（1102a / 1121a-c 等）绝不能被抽进训练集** —— 困难样本只能来自别的腿；
2. ★ **仍然按箱均匀**，不许按尺子的构成加权（§7.1.4 的结论不变）；
3. ★ **临界加权只在箱内做**，箱的 cap 不变 ⇒ 覆盖面与难度权重互不牺牲。

### 8.2 验证 / monitor 集：同一个工具，小 cap

```bash
# ★实测口径：cap 15/3 × 112 箱 ⇒ 上界 2 016，实际抽出 1 630 个样本
$PY $LIB/draw_train_set_w2.py --in ~/ai_depth3/datasets_full_s2 \
    --out <训练机>/data/datasets_monitor \
    --self-cap 15 --harq-cap 3 --seed 20261104
```

**monitor 的三条硬要求**：
1. **与训练集同源**（同一个母集、同一套箱，**不同种子**）；
2. **不与训练集重叠** —— 用 `X` 行的哈希求交来**测量**，不是靠"应该不会"；
3. **不许静默二选一**：覆盖率与不相交性**两个数都要报**。

### 8.3 在一个 npz 上按箱切 train/test/monitor（另一条路）

```bash
$PY $LIB/split_depth3_stratified.py --datasets <某.npz> --out <目录> \
    --per-bin 200 --test 0.1 --monitor 0.1 --min-to-split 20 --seed 20261027
```

### 8.4 抽完必做的两道检查

```bash
# ① 六项验收：上限 / 覆盖 / 重复 / 对齐＋出处 / 格式（★ 转置是最难发现的错）
$PY $LIB/verify_train_set_w2.py --draw <抽取目录> --mother ~/ai_depth3/datasets_full_s2 \
    --self-cap 2500 --harq-cap 250

# ② npz → mmap 目录（训练器一条代码路径读两者；mmap 后内存不随数据集增长）
$PY $LIB/convert_npz_to_mmap.py --in <某.npz> --out <某目录>
```

---

## 9. 训练

```bash
cd ~/work/ai_phy_train            # 数据装在 <训练机>/data/
export LD_LIBRARY_PATH=$(ls -d .venv/lib/python*/site-packages/nvidia/*/lib | tr '\n' ':')   # ★ 107 必需：否则 TF 看不到 GPU

.venv/bin/python ~/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py \
  --train data/datasets_train_w2 --val data/datasets_monitor_w2 \
  --out runs/w2c32_b18_e21 \
  --modulation QPSK --modcond --no-reporting-heads \
  --c-hid 32 --blocks 18 \
  --epochs 60 --patience 0 --snapshot-every 10 \
  --batch 16 --workers 4 --max-val 1000 --resume --bler-samples 0
```

| 参数 | 含义 | 备注 |
|---|---|---|
| `--modcond` | 调制条件化（12 平面 ＋ 4 个 Qm one-hot = **16 通道**）| 混合调制语料必须开 |
| `--layout spatial` | 平面布局 | 与部署形态一致 |
| `--c-hid` / `--blocks` | 宽度 / 块数 | c32=26 185 参数；c128=325 897 |
| `--batch` | 显存/速度的旋钮 | ★ 6 GB 卡上 c128 **batch 16 OOM、12 可跑**（实测）|
| `--patience` | 早停（**0 = 不停**）| ★★★ **现在的默认做法是 0**：不停、每轮留快照，**事后用尺子选**（见下）|
| `--snapshot-every` | 每 N 轮存一份**可续训的完整站点** | ★★ **研究早停是否有用时设 1**（每轮都留）；产物在 `<out>/snapshots/epoch_NNNN_periodic.{keras,json}` |
| `--resume` | 从 `depth3_last.keras` ＋ `resume.json` 续训 | ★ 状态文件在**最佳决策之后**写；加载时还会用 history 校正 `best/since` |
| `--bler-samples` | 每 `--bler-every` 轮解码多少个验证样本 | 0 = 不解码（选模型靠 val loss，判优劣靠尺子）|
| `--workers` | 组批线程数 | ★ Linux 默认 **0＝串行**（原有行为，未改）；macOS 默认 `min(3, 核数/4)` —— 实测 6 线程反而比串行慢，见 §9.2.3 |
| `--jit` | 用 XLA 编译训练步 | ★ Linux 默认 **on**（未改）；**macOS 默认 off**（`tensorflow-metal` 没有 XLA device），显式给出会先自检、失败自动降级，见 §9.2.3 |
| `--no-gpu` | 隐藏全部 GPU，强制 CPU | 默认关（Ubuntu 命令行照旧选 GPU）；★ macOS 上 c32 用它是**更快**路径（GPU 慢 1.87×）|

**产物**：`depth3_best.keras`（每次 val 改善）、`depth3_last.keras`（每轮）、`resume.json`（续训状态）、
`history.json`（跑完才写）、`snapshots/`。

### 9.1 ★★★★ 不早停，用快照 ＋ 尺子选模型（2026-11-05 定）

**⇒ ★★ 训练时的 `val`（monitor）现在只作**参考**，不再是选模型的依据** ✓
**⇒ ⇒ ★★★ 做法**：**`--patience 0`（不早停）＋ `--snapshot-every 1`（每轮一个快照）** ⇒ 训练跑满预定轮数，
**每轮的模型参数都留在 `<out>/snapshots/epoch_NNNN_periodic.{keras,json}`** ✓
**⇒ ★★ 事后**：用**尺子**（5 000 单元、交付比特）逐快照测量 ⇒ **"早停到底该停在哪一轮"这个问题，从推测变成测量** ✓

**为什么这样更好**：

| 早停（旧）| 快照 ＋ 事后选（新）|
|---|---|
| 用**训练时那份 monitor** 选 ⇒ monitor 一变（例如只有 181 条），选择就抖 | ★ **用尺子选**，与训练时的 monitor 无关 ✓ |
| 早停**只留最好那一轮**，无法回答"再训几轮会不会更好" | ★★ **整条曲线都在**，可以逐点看 ✓ |
| `patience` 是个**猜**出来的超参 | ★ 变成**可测**的量（哪一轮的交付比特最高）✓ |

**代价**：每个快照约 **1.6 MB（c128）／6 MB（c256）／24 MB（c512）** —— 四轮十几个快照也就几百 MB ✓。

**★ 起飞前先看日志头三行**：`parameters N`、`layout`、`train/val` 样本数 —— 架构错在这里就能发现，不必等一轮。

### 9.2 macOS（Apple Silicon）训练路径：两轮改动、实测账、对 CUDA 的影响（2026-11-23）

> **为什么单列**：本机（**Mac M4 Pro，14 核/64 GB，macOS 26.6.2**）既是开发机，也是唯一能回答"这条训练在 Mac 上能不能跑、值不值得跑"的机器。
> 2026-11-22/23 的两轮改动只做一件事：**让同一条命令行在 macOS 上跑得对、跑得稳，同时不动 Ubuntu 的任何一个数值**。
> 本节把**改了什么**、**实测多少**、"**什么时候别用 Mac 训练**"的判据、以及**对 CUDA 路径的逐条影响**写在一起，避免下次重新踩。
>
> ★★ **完整实验记录另有一份**：`macos_apple_silicon_training_memo.md`（每个数字的夹具、FLOP 估算与峰值占比、
> 试过但无效的手段、未解问题、复现命令）。本节是它的**操作摘要**；两份冲突时以 memo 的原始测量为准。

#### 9.2.1 本机唯一能跑 TF 的环境

| 项 | 值 |
|---|---|
| 解释器 | `python3.11` ＋ venv **`~/.venvs/tf-metal-2.18`**（★ 不要装进全局 3.11：TF 2.18 要求 `numpy<2.1`，会降级现有环境）|
| 版本 | `tensorflow 2.18.1` ＋ `tensorflow-metal 1.2.0` ＋ `keras 3.15.1` ＋ `numpy 2.0.2` |
| 设备 | `tf.config.list_physical_devices()` → `CPU:0` ＋ `GPU:0`（Metal 以 PluggableDevice 注册，**0 MB 显存**是它的正常报法，走统一内存）|
| 为什么锁 2.18 | Apple 的对照表把 `tensorflow-metal` 1.1/1.2 配对在 **TF 2.14–2.18**，**2.19+ 没有配对** —— 直接 `pip install tensorflow`（会装到 2.21）**永远看不到 GPU**。见 <https://pypi.org/project/tensorflow-metal/> |
| 唯一可信的判据 | 训练器启动横幅的 `platform: compute placement: a 64x64 matmul lands on …`。`list_physical_devices()` **不能**作为判据：`--no-gpu` 之后插件仍然列出 `GPU:0`，但算子落在 `CPU:0` |

#### 9.2.2 改了什么（`train_depth3.py`，两轮合计）

| 类别 | 改动 | 仅 macOS？ | 为什么 |
|---|---|---|---|
| 平台闸门 | `IS_DARWIN = sys.platform == "darwin"` | — | 所有平台相关决定只读它 |
| XLA | `resolve_xla`：Linux 原样返回请求值；macOS 未显式 `--jit` 则关。`xla_self_test` 先编译一个 2×2 探针；`compile_step` 工厂 ＋ `step` 包装做**一次性**运行期降级 | ★ | `tensorflow-metal` 无 XLA device（见 §9.2.3(e)）|
| 设备选择 | `--no-gpu` ＋ `hide_gpus()`：在任何 op 之前 `set_visible_devices([], "GPU")` | 跨平台 | 本机 c32 上 CPU 比 GPU 快 1.87×；没有它只能卸载插件才能走快路 |
| 数值护栏 | macOS 上 `--dtype float16` 直接报错（`DEPTH3_ALLOW_METAL_FP16=1` 可越过）| ★ | 插件 fp16 段错误（§9.2.3(e)）|
| 诊断 | 启动横幅打印设备、**算子落点**、XLA 生效值、组批线程数、ANE 不参与反传 | ★ | "装了插件 ≠ 用上 GPU"；日志必须自证 |
| 组批 | `pipeline_defaults`：Linux ＝命令行原值；macOS `--workers 0` → `min(3, 核数/4)`，`prefetch` 默认 `2×workers` | ★ | 实测 6 线程比串行还慢（§9.2.3）|
| 数据 | 一维元数据列（`alloc_*`/`qm`/`nof_bits`/`snr_nominal`/UCI…）一次性读进内存；`X/H/bits/tb_bits` 仍 mmap | 跨平台 | 随机页错误的元数据访问占 per-sample 时间的可观部分，10 万样本约 8 MB |
| 数据 | 几何常量缓存（`_GeometryCache`：有界 FIFO ＋ 锁，命中无锁，未命中在锁内构建）；掩码只读 | 跨平台 | 256QAM 掩码 612×112＝274 KB／样本，原来每样本重建；`clear()` 式淘汰会造成未命中风暴 |
| 数据 | `unpackbits(count=nof_bits)`；`X` 用 `asarray` 不再复制；复用的调制名映射 | 跨平台 | 纯 host 侧省工，值不变 |
| Host↔Device | 损失每 `LOSS_ACCUMULATION=64` 个 batch 回读一次（原来是每 batch `float(total)` 同步）；`tensors()` 只打包图里有的 head 的 target；`evaluate` 每 batch 只 `.numpy()` 一次；`--select-on val` 时不算 low-SNR band | 跨平台 | 同步与重复转换是 GPU 队列的停顿；band 只在 `--select-on band` 时有人读 |
| 序列化 | `CUSTOM_OBJECTS` 统一（补上 `MaskedSmooth`）：`--export-only` 不再加载失败 | 跨平台 | 原来是**必然失败**的路径（训练完才炸）|
| 修复 | `genie_labels` 星座图**按 qm 缓存并冻结** | 跨平台 | 原实现只缓存第一次的 qm：`--modcond --target genie` 的混合调制数据会用错星座，**标签是错的且静默** |

#### 9.2.3 实测账（本机）

**(a) 数据流水线（CPU 侧，`Batches.epoch`，best-of-3）**

| 夹具（batch 8 除注明外）| HEAD | 现在（串行）| 现在（多线程）|
|---|---|---|---|
| 51 PRB 全带，8 样本（best-of-3）| 0.744 ms/样本 | 0.650（1.14×）| 6 线程 0.658 |
| 51 PRB 全带，200 样本 | — | 0.650 | **0.412（3 线程，1.58×）** |
| 混合 2–51 PRB，200 样本 | 0.480 | 0.401（1.20×）| **0.310（2 线程，1.55×）**；3 线程 0.360、6 线程 0.516 |
| 1–51 PRB 均匀，batch 16，`--modcond`，400 样本 | — | 0.589 | **0.483（3 线程）** |

线程数**不是越多越好**：同一 51 PRB 200 样本夹具上，2 线程 0.430、3 线程 0.412、4 线程 0.419、**6 线程 0.552（比串行还慢）** —— GIL ＋ 统一内存带宽。
**判据**：160K 样本跑 32 分钟只需 **~80 samples/s ≈ 18 MB/s 磁盘**，而加载实测 2 000 samples/s ⇒ **数据加载不在关键路径上**。

**(b) step 微基准**（c32，18 blocks，spatial，**4 个输出头、非 cond**，随机 target，预热后 10 次均值）

| 配置 | CPU | Metal GPU | GPU/CPU |
|---|---|---|---|
| batch 8（4 头微基准）| 105.5 ms/step | 193.8 | 0.54× |
| batch 16 | 188.3 | 325.7 | 0.58× |
| batch 32 | 438.9 | 578.9 | 0.76× |
| batch 128 | 1 743.4 | 2 025.0 | 0.86× |
| **c128** batch 16 | 954.5 | 738.8 | **1.29×** |
| **c128** batch 32 | 1 880.3 | 1 369.0 | **1.37×** |

端到端（**同一模型** c32/c128、18 blocks、4 个输出头、非 cond；200 样本/epoch；1-vs-7 epoch 斜率；c128 用随机权重仅计时）：

| 配置 | CPU | Metal GPU |
|---|---|---|
| c32 batch 8 | 2.63 s/epoch | 4.93 s/epoch（0.53×）|
| c128 batch 16 | 12.13 s/epoch | 10.08 s/epoch（1.20×）|

**⇒ c32（部署宽度）在任何 batch 下都是 CPU 快；GPU 只有 c128 以上 ＋ 大 batch 才赢。** 原因：18 个 3×3 depthwise ＋ 1×1 的链在 612×14×32 上算术强度低，Metal 每 kernel 的命令开销压过计算；CPU 走的是 Accelerate/Eigen 的成熟 kernel。

**(c) 本机一轮要多久**（160 000 样本，PRB 1–51 均匀分布，c32 真实配方 `--modcond --no-reporting-heads`）

| 配方 | 每样本 | 160K/轮 |
|---|---|---|
| c32 batch 8 | 12.13 ms | **32.3 min** |
| c32 batch 16 | 12.42 | **33.1 min** |
| c32 batch 16，**全 51 PRB**（对照）| 12.32 | 32.9 min ⇒ **PRB 不进入 step 时间**（平面恒为 612×14）|
| c32 batch 8，第二轮改动后复测 | 12.14 | 32.4 min ⇒ 改动前后一致（无回归）|
| c32 batch 16，**GPU**（参考）| 21.5 ms | 57.3 min ⇒ 别用 GPU |

**(d) 为什么没有继续加码（全是实测，不是判断）**

| 探针 | 数字 | 结论 |
|---|---|---|
| `tensors()` 主线程打包（7 个 target，batch 16，19 MB）| stack 0.37 ＋ `tf.constant` 0.37 ＝ **0.74 ms**，对 199 ms 的 step 是 **0.37%** | **不值得**引入后台打包线程（还要在 Metal 上冒"第二个 TF 线程"的风险）|
| `stack` 微优化 | 0.382 → 0.376 ms（1.02×）| 中性，保留只为语义清楚 |
| TF intra-op 线程 | 默认 14 最优；8 → 慢 12%，4 → 慢 55%，2 → 慢 148% | **不能动** |
| `--fuse-relu` | 12.13 → 12.34 ms/样本（**慢 1.7%**）| 不采用 |
| `--jit`（XLA 跑在 CPU）| 12.1 → 89.5 ms/样本（**慢 7.4×**）| 与 README §3 的 6.2× 一致；macOS 默认关是对的 |

**(e) 两个会让进程直接死的坑（本机都发生过）**

| 现象 | 栈/报错 | 触发条件 | 处置 |
|---|---|---|---|
| macOS 弹「Python 意外退出」 | `EXC_BAD_ACCESS` in `metal_plugin::MPSBinaryOpBase<Eigen::half>::Compute` → `MetalBinaryGraph::Compute` → `mpsGraphTensorDataFromTensor` | **`--dtype float16` ＋ Metal GPU**，第一个训练步 | 代码已拦截（macOS 上 `--dtype float16` 一行报错、进程不崩；`DEPTH3_ALLOW_METAL_FP16=1` 可越过）。**未修改的 HEAD 脚本同样崩** ⇒ 插件缺陷，不是训练代码。崩溃报告在 `~/Library/Logs/DiagnosticReports/Python-*.ips` |
| Metal 上开 XLA | `NotFoundError: could not find registered platform with id: 0x…` `[Op:__inference_step_…]` | 用 HEAD 的默认（XLA on）在装了 Metal 的 Mac 上训练 | 现在的默认（macOS 关 XLA）已避开；**不要**在 Metal 上显式 `--jit` |

#### 9.2.4 什么时候用 Mac、什么时候别用

| 场景 | 结论 |
|---|---|
| c32（部署配方）任何 batch | **用 CPU**：`--no-gpu`（或别装 `tensorflow-metal`）。GPU 慢 1.87× |
| c128 且 batch ≥ 16 | 可以用 GPU（1.2–1.37×），但仍要 `--dtype float32` |
| 任何 fp16 | macOS 上**禁止**（段错误）；导出包本来就是 fp16 |
| 需要与 Ubuntu 逐位对齐 | 用 `--init` 固定初始权重再比。★ **Keras 3 的初始化器不受 `tf.random.set_seed` 控制**，同一命令两次运行的初始权重本来就不同（这是既有事实，本机实测两次 `train` 为 3.38864 vs 3.58929）|
| 要跑 160K 样本 | c32/batch 8 约 **32 分钟/轮**；60 轮约 32 小时。数据 ~36 GB > 15 GB 内存 ⇒ 必须用 mmap 目录形式 |

#### 9.2.5 对 Ubuntu / NVIDIA-CUDA 的潜在影响（逐条）

| 改动 | Linux 默认是否生效 | 潜在风险 | 复核方法 |
|---|---|---|---|
| XLA 解析 | **否**：`resolve_xla` 在非 darwin 直接返回请求值（默认仍 `on`）| 无 | Ubuntu 日志头仍应是 `xla: on` |
| `--workers` / `--prefetch` 默认 | **否**：Linux 仍 `0` ＝串行，`prefetch` 仍 `2×workers` | 无 | 日志无 `platform:` 行；去掉 `--workers` 后仍是串行 |
| `--no-gpu` | 默认关 ⇒ **不产生任何 TF 调用** | 无 | 不传该参数时行为逐字未变 |
| 损失回读节流（每 64 batch）| 是 | ★ 两个可感知点：① **NaN/Inf 最多晚 64 步才被看到**（程序本来也不 in-loop 检查）；② 第 64 步才发生一次同步，若显存不足崩溃，前 63 步的 loss 不会出现在日志里 | CUDA 上跑 2 轮，核对 `history.json` 的 `train` 与改动前逐位相同（本机 CPU 已验：逐位相同）|
| 几何缓存 FIFO＋锁 | 是 | 内存上限仍是 64 条（≈22 MB），**未增**；`--workers 0` 时锁永不争用。外部脚本若**写** `sample()["llr_weights"]` 等只读掩码会抛 `ValueError`（仓库内已核查：`eval_depth3`/各 probe 只读）| `grep -rn '\["llr_weights"\]\s*=' lib doc_chinese` 应为空 |
| 元数据常驻内存 | 是 | +~8 MB/10 万样本；万一一维列 > 32 MB 会被跳过（有守卫）| 大语料上观察 RSS |
| `--select-on val` 时不算 band | 是 | `val_band` 内部变 `nan`；它**不在** `history.json`、也不打印，只有 `--select-on band` 读它 | `--select-on band` 跑一轮，`best` 应来自 band |
| `tensors()` 只打包存在的 head | 是 | 将来若新增 head 而忘了登记 target ⇒ **trace 期 KeyError**（响亮失败，不静默）| 新增 head 时同步改 `read_*` |
| `genie_labels` 按 qm 缓存 | 是 | ★ **行为变更（修正）**：`--modcond --target genie` 的混合调制语料以前用错星座、标签是错的。**用该组合训过的旧 run 的数值不可比**，需要重训或至少重新评估 | 用混合 qm 语料跑 `--target genie`，逐样本检查 stride-8 块尾全 0 |
| `CUSTOM_OBJECTS` 补 `MaskedSmooth` | 是 | 只修复原本必然失败的 `--export-only` 路径 | `--smooth-k>0` 训一轮再 `--export-only` |
| `epoch` 索引 `.tolist()`、`stack` itemgetter | 是 | 无（同一批整数、同一数组）| 与 HEAD 的逐样本对比（混合布局/调制/UCI）应逐位相同 |
| 导出契约 | 未改 | 无 | 对比 `.mlpackage` 的 output 名与 `depth3_*` `userDefined` 键 |

**一句话**：跨平台改动都落在 **host 侧与缓存淘汰**上，而它们的不变量（"算出来的数"）已被逐样本 bit-identical 对拍守住；唯一**故意改变数值**的是 `--modcond --target genie` 的标签修正（以前是错的）。

#### 9.2.6 两端的验收命令

```bash
# —— Ubuntu / CUDA：数值与契约不变 ——
# 1) 同一命令、同一 --init 权重，各跑 2 epoch，history 必须逐位相同
python train_depth3.py --train … --val … --out runs/after --modcond --layout spatial \
  --in-channels 12 --c-hid 32 --blocks 18 --batch 8 --epochs 2 --max-train 400 --max-val 100 \
  --init fixed.keras --bler-samples 0
diff <(python -c "import json;print(json.load(open('runs/after/history.json'))['history'])") \
     <(python -c "import json;print(json.load(open('runs/before/history.json'))['history'])")
# 2) 默认路径：不传 --workers 时仍串行；xla: on；日志无 platform: 行
# 3) --select-on band --max-val 1000 ：best 仍来自低 SNR band
# 4) --export-only … --export-probe … --export-buckets 12,24 ：output 名与 depth3_* metadata 未变

# —— macOS：设备与止血逻辑 ——
P=~/.venvs/tf-metal-2.18/bin/python
$P train_depth3.py …                 # xla: off + 原因；compute placement: GPU:0
$P train_depth3.py … --no-gpu        # gpus: … hidden；placement CPU:0；epoch 时间约 GPU 的 0.5×
$P train_depth3.py … --dtype float16 # 一行 parser error，进程不崩（不产生崩溃报告）
# 并发/缓存正确性：把 Batches.GEOMETRY_ENTRIES 临时改小（如 3），6 线程重放 800 个样本，
# 与串行结果逐位对比；缓存长度必须 <= 上限，掩码必须只读（写入应抛 ValueError）
```

---

## 10. 测试与评估（"尺子"）

> ★★ **本节已独立成文**：**`doc_chinese/depth3_llr_ai/gate_measurement_runbook.md`** ✓
> 那里有从 `.keras` 导出 `.mlmodelc`、标定 sidecar、恢复并校验尺子、逐条跑臂、算分（交付比特／bootstrap／McNemar／
> 按调制分解）与归档要求的**完整步骤**，以及 2026-11-04 那批的实测记录。

**这里只留三句必须记住的**：

1. **口径**：**交付比特** ＝ Σ(`crc=OK` 的 `tbs_bytes × 8`)，**不是块数**（大块会被系统性低估）。
2. **尺子**：`ruler_s2_amc_5000`（5 000 个未按 CRC 过滤的接收），归档在
   `lib/phy/upper/ai_signal_processors/test/`，用 `materialise.sh` 或分片 tar 还原，**并用 `SHA256SUMS` 校验 10 000 条**。
3. **四条硬要求**：`OCUDU_DEPTH3_FALLBACK=none`（PURE）、`depth3_scales.txt` 与 bundle 同目录、**每单元一进程串行**、
   **守卫**（每单元前查有没有第二个 replay）。

## 11. 工具清单（一张表）

| 阶段 | 工具 | 输入 → 输出 | 关键点 |
|---|---|---|---|
| 采集 | `wip/run_collect_leg.sh` | 配置＋腿名 → `corpus/<腿>/{cap.*,logs/,tb_live/}` | `cpu` 模式；一次 Ctrl-C；`LEG_ALLOW_DIRTY` |
| 流量 | `ping` / `iperf3 -R` / `wip/run_iperf_ladder.sh` | 上行速率 → 授权宽度 | `-R` 才是上行；server 在手机 |
| dump | `ai_train/dump_tb.sh` | 捕获 → `tb_<腿>/{index.csv,units.txt,tb/}` | CPU 路径；`--jobs`；三条对账 |
| 建库 | `ai_train/build_mother_set.sh` | dump＋捕获 → `datasets_full_s2/<腿>_<MOD>_<pop>.npz` | 不走抽样；`CHUNK` 控内存 |
| 分箱盘点（捕获）| `ai_train/census_bins.py` | 腿目录 → `leg,mod,width,snr,n` CSV ＋ 增量 ledger | `--force` 是替换不是累加；读侧车，不读 `.bin` |
| 分箱盘点（母集）| `ai_train/census_mother_set.py` | 母集 npz → 逐箱 CSV ＋ 表 ＋ 均匀抽取投影 | 复用 `census_bins` 的边界；`--caps` 直接算抽多少；扫一遍约 3 分钟 |
| 抽训练集 | `ai_train/draw_train_set_w2.py` | 母集 → 一数组一 `.npy` 的目录 | cap 是**每箱**；先 `--dry-run` |
| 抽 monitor | 同上（小 cap、不同 seed）| 母集 → `datasets_monitor/` | 同源、测重叠 |
| 验收 | `ai_train/verify_train_set_w2.py` | 抽取目录＋母集 → 六项报告 | 对齐/出处能抓转置 |
| 切分 | `ai_train/split_depth3_stratified.py` | npz → train/test/monitor | 按箱；`--min-to-split` |
| 转格式 | `ai_train/convert_npz_to_mmap.py` | npz → `.npy` 目录 | 训练器两条路径都读 |
| 审计 | `ai_train/check_depth3_dataset.py` | 目录 → 形状/比特平衡/SNR | 期望 `findings: none` |
| 训练 | `ai_train/train_depth3.py` | 数据集 → `runs/<臂>/` | `--resume`／快照／`argv:` 自述 |
| 导出 | `train_depth3.py --export-only` ＋ `coremlcompiler` | `.keras` → `.mlmodelc` | fp16 误差已记录 |
| 标定 | `ai_train/calibrate_depth3.py` | 模型＋monitor → `depth3_scales.txt` | 与 bundle 同级 |
| 尺子 | `run_arm_ext.py`（203）| 单元列表＋bundle → `<臂>.csv` | PURE、串行、守卫、记二进制哈希 |

---

## 12. 2026-11-04 的实测状态（供对照）

| 阶段 | 状态 |
|---|---|
| 采集 | 15 条 1103 腿（ping a–h、iperf i–o），每条 **50 000** 捕获，配置 `pinned_mcs04.yml`（AMC 版）|
| dump | **15/15 完成并对账通过**：750 000 单元、**553 854 crc=OK（73.8%）**、93 GiB（其中 ~90 GiB 是 `stage/` 草稿）|
| 建库 | 进行中：先导腿 22 min、审计 `findings: none`；其余 57 条腿以 4 路并行、`CHUNK=8000` 在跑 |
| 训练 | 香港 107：c32 60 轮跑到第 42 轮（11-04 05:48 最后一次亲眼所见，之后不可达）；c128 端到端已排在队尾 |
| 广州 | 106（HT 已开，28.7 samples/s）与 221（15.6 samples/s）空闲，代码已 pull 到 `ae31c8890f` |

---

## 13. 常见坑（全部真实发生过）

| 坑 | 症状 | 避免 |
|---|---|---|
| 语料目录不存在 | 跑满一条腿、**一个文件没写**、无报错 | 看 `[ul_capture] capturing:`；`NOT CAPTURING` 立刻停 |
| 复用 dump 目录 | 段与段混在一起，无法归因 | 每腿新目录 |
| 复用 `OCUDU_UL_DUMP` 前缀 | 接着编号，事后分不清 | 同上 |
| `OCUDU_UL_DUMP_COUNT` 没改 | 每腿只有 8 个捕获 | 设 50 000/100 000 |
| 钉 MCS 又混着用配置 | 语料名与内容不符 | 腿名与 `LEG_CONFIG` 一致 |
| 不设 `LD_LIBRARY_PATH`（107）| TF 看不到 GPU、慢 5 倍且不报错 | 用启动脚本里的那一段 |
| 两个 replay 并发 | 互相污染，SINR 不可能 | 串行 ＋ 守卫 |
| `select_by_bin` 建母集 | 静默沿用抽样账本、少建很多 | 母集直接调 `build_mother_set.sh` |
| 单进程建 100k 样本 | 峰值 ~25 GB，机器装不下 | `CHUNK` 切片 ＋ 流式合并 |
| 拿块数比两条臂 | 大块被低估，方向可能反 | 用**交付比特** |
| 用旧 monitor 验证新训练集 | 训练/验证不同源，val 不可信 | monitor 从**同一个母集**抽、不同种子 |
| 只看"采了多少条" | 全在同一种格子 | 每段跑宽度/调制/SNR 分布 |
| 重启训练丢了 `--bler-samples 0` | 默认 24 开启，撞上带控样本 | 用启动脚本（参数写死）；`argv:` 日志自查 |
| macOS 上 `--dtype float16` | 进程**段错误**、弹「Python 意外退出」，无 traceback（Metal 插件的半精度算子）| 训练用默认 float32（导出包本来就是 fp16）；代码已拦截，别用 `DEPTH3_ALLOW_METAL_FP16=1` 绕过 |
| macOS 上显式 `--jit` | `NotFoundError: could not find registered platform with id: …` | 默认已关 XLA；需要 `--jit` 时先看 `xla_self_test` 的结果与回退日志 |
| 以为装了 `tensorflow-metal` 就在用 GPU | epoch 时间反而变长；`list_physical_devices()` 也列出 `GPU:0` | 看横幅 `platform: compute placement:`；c32 用 `--no-gpu`（实测 GPU 慢 1.87×）|
| 用同一 `--seed` 比两条臂 | 两次运行的 loss 不同 —— Keras 3 的初始化器不受 `tf.random.set_seed` 控制 | A/B **必须**用 `--init` 固定同一份权重（§9.2.4）|

---

## 14. 一页速查（从零到一条腿入库）

```bash
# 0) 代码与环境
cd ~/dev/ocudu && git pull --ff-only
export COLLECT_CORPUS=/Volumes/T7_Corpus/corpus OCUDU_UL_DUMP_COUNT=50000 OCUDU_UL_DUMP_LLR=0 OCUDU_LA_LOG_EVERY=1

# 1) 起飞（tmux 里跑）
LEG_ALLOW_DIRTY=1 sudo -E bash doc_chinese/llr_ai_detection/wip/run_collect_leg.sh cpu s2_amc_ping_1103p
# 等 "Cell scheduling was activated" → 接手机 → 打流量
ping -i 0.05 -s 32 <手机IP>            # 或：iperf3 -c <手机IP> -R -t 300 -P 4

# 2) 收腿：一次 Ctrl-C；核对
L=/Volumes/T7_Corpus/corpus/s2_amc_ping_1103p
find "$L" -maxdepth 1 -name 'cap_*.txt' ! -name '*_ce.txt' | wc -l
find "$L/tb_live" -name 'tb_*.bits' | wc -l

# 3) dump
bash lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_tb.sh \
  --corpus /Volumes/T7_Corpus/corpus --out ~/ai_depth3/dump_s2/tb_s2_amc_ping_1103p \
  --jobs 3 --binary build/lib/phy/upper/channel_processors/ul_chain_replay_cpu \
  /Volumes/T7_Corpus/corpus/s2_amc_ping_1103p

# 4) 建库
TRAIN=~/dev/ocudu DUMP_ROOT=~/ai_depth3/dump_s2 CORPUS_ROOT=/Volumes/T7_Corpus/corpus \
OUT=~/ai_depth3/datasets_full_s2 PY=~/ai_ce_work/venv/bin/python JOBS=1 CHUNK=8000 \
bash lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh s2_amc_ping_1103p

# 4.5) 分箱盘点（抽取之前；母集侧）
$PY $LIB/census_mother_set.py --caps 2500:250,8000:800

# 5) 抽训练/monitor 集（在训练机上）
$PY $LIB/draw_train_set_w2.py --in datasets_full_s2 --out data/datasets_train_w2 --self-cap 2500 --harq-cap 250 --seed 20261103
$PY $LIB/draw_train_set_w2.py --in datasets_full_s2 --out data/datasets_monitor  --self-cap 15   --harq-cap 3   --seed 20261104
$PY $LIB/verify_train_set_w2.py --draw data/datasets_train_w2 --mother datasets_full_s2

# 6) 训练
.venv/bin/python ~/work/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py \
  --train data/datasets_train_w2 --val data/datasets_monitor_w2 --out runs/<臂> \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 18 \
  --epochs 60 --patience 0 --snapshot-every 10 --batch 16 --workers 4 --max-val 1000 --resume --bler-samples 0
```

★ 状态 ✓：★ 五份文档的时间线与"以谁为准"已列（§0）✓；★★ **起飞命令按最近一次成功的形状给出并核过脚本接口** ✓；
★★ 采集/ dump /建库/抽取/训练/评估 **六段的命令与判据**齐全 ✓；★★ **2026-11-04 实测数**（dump 15 腿、先导建库 22 min/11.4 GB、106 的 28.7 samples/s）已并入 ✓；
★★ 十三个真实踩过的坑与**一页速查**在末尾 ✓。

---

## 15. ★★★★★ 2026-11-06 **实链时延：窄带桶（narrow-band buckets）—— 没有它，能力再强也上不了实链**

> **为什么这一节必须在这份手册里**：前面 §6–§10 讲的是"**能不能解出来**"（交付比特）；
> 但一个模型**解得再好，只要单次推理超过 slot 预算，它在实链上就是 0** ✓
> **⇒ 2026-11-05 的飞行就是活证据：c128 臂的 `[ul_equalization_demod]` 中位 5.4 ms、`[ul_pipeline]` 5.9 ms，
> UL 丢块 ~39%** —— 模型能力（探针 −3.0%）没有任何意义，因为**它赶不上截止期** ✓

### 15.1 ★★★★ 根因：不是算力，是**每个算子的数据量**；而**桶**把数据量按分配裁剪

| 事实 | 证据（2026-11-06 实测）|
|---|---|
| 成本**不随 FLOPs** | ★ **dense 3×3（5.7× FLOPs）延迟不变**：2 824 → 2 831 µs ✓ |
| 成本**随张量体积** | 宽度扫描 420（c32）→ 1 024（c64）→ 1 312（c96）→ **2 824**（c128）µs；C² 应给 4/9/16×，实测 2.4/3.1/6.7 ✓ |
| 每次**都要重算** | 权重/程序**只加载一次**（构造期绑定 ＋ 预热：即"粮草先行"）；但**中间激活不能跨 slot 缓存**（输入每 slot 都变）⇒ 101 个算子 × 1.7 MB ≈ **170 MB/次**，2.8 ms ≈ **60 GB/s** ✓ |

**⇒ ⇒ ★★★ 所以"少搬"才是解法**：平面只覆盖"分配 ＋ 4 PRB 余量"，运行时选**最小的足够桶宽** ✓

| 桶宽 | **c32** | **c128** |
|---|---|---|
| **108** | ★ **137 µs** | ★ **266 µs** |
| **180** | 172 µs | 432 µs |
| **252** | 206 µs | — |
| **612（全带）** | 387 µs | ★★ **2 824 µs** |

**⇒ ★★★ 5 PRB 的典型分配走 108 桶 ⇒ c32 137 µs、c128 266 µs ⇒ 都在 0.5 ms slot 预算内** ✓
**⇒ ★★★ 而且桶是**逐位等价**的（实测：c32 探针 500 带桶 363 OK / 999 200 bit，与不带桶**完全相同**）⇒ 加桶不改结果，只改速度 ✓

### 15.2 ★★★ 工具（已入库，别再手工摆目录）

```bash
cd lib/phy/upper/ai_signal_processors/depth3/ai_train

# 一条命令：导出 8 个宽度 → 编译 → 摆成 <name>.mlmodelc ＋ buckets/base_w*.mlmodelc → 拷 scales → 打延迟表
./make_bucket_bundle.sh <snap.keras> --name <规范名> \
    --c-hid 128 --blocks 18 \
    --calib ~/ai_depth3/gate_ai_models/calib/datasets_monitor_w2 \
    --measure
```

| 工具 | 作用 |
|---|---|
| ★★ `make_bucket_bundle.sh` | 生成"可飞 bundle"（主 bundle ＋ `buckets/`）|
| ★ `ane_width_sweep.py` | 宽度-延迟扫描（**未训练权重即可**：延迟与权重无关）|
| `coreml_latency_probe`（build 产物）| 单模型逐档延迟（`cpu`/`cpu_ne`/`all`）|

**⇒ ★★ 这些工具本体都放在**仓库** `doc_chinese/depth3_llr_ai/work_tmp/`（按用途分：`corpus_pipeline/` `dataset_build/` `measurement/` `analysis/` `training/` `benchmarks/`）**；`~/ai_depth3/bin/` 只留包装器与主机配置 ✓
**⇒ 规则**：**工具＝代码⇒进仓库；采集/数据集/dump/bundle＝数据⇒留在 `~/ai_depth3/`** ✓

**⇒ ★★ 目录布局（臂按名字探测，写错不报错）**：
```
<bundle-dir>/<name>.mlmodelc               ← 全带（612）
<bundle-dir>/buckets/base_w108.mlmodelc    ← … 180/252/324/396/468/540
<bundle-dir>/depth3_scales.txt             ← 主 bundle 与 buckets/ 各一份
```
**⇒ ★★★ 判据**：飞行日志里必须有
`narrow-band buckets bound: 108 180 252 324 396 468 540 subcarrier(s)`；
**没有这一行 = 在跑全带**（只是慢，不会报错）✗

### 15.3 ★★ 测量协议的变化（2026-11-06 起）：**交付比特是第一道门槛，时延是第二道**

| 门槛 | 指标 | 现状 |
|---|---|---|
| ★ 第一 | **交付比特**（npu 口径）| ★ 探针/尺子/全腿**已按此建表** ✓ |
| ★★ 第二 | **实链时延**：`[ul_equalization_demod]`（臂自身）＋ `[ul_pipeline]`（整跳）＋ UL `nok` | ★ **本次起纳入**：飞行必看 |

**⇒ ★★★ 记录的三个时延数（缺一不可）**：
1. **模型侧**：`coreml_latency_probe` 的 p50（按桶宽分别记）；
2. **臂侧**：飞行日志 `[ul_equalization_demod]` 的 p50（应≈模型侧 ＋ 组平面/量化）；
3. **整跳**：`[ul_pipeline]` 的 p50 与 **UL 丢块率**（是否超 slot 的最终判据）。

**⇒ ★★ 为什么现在才加**：交付比特是"能不能解"的**必要条件**，先排序模型能力；
若先看时延，会把"能力最强但太慢"的模型过早淘汰（c128 正是这种：能力第一、全带最慢、**加桶后两者兼得**）✓

---

## 16. ★★★★★ 2026-11-06 11:00 **删除 T7 原始采集前的证据链（备份校验完成）**

> 用户要删 T7 上的 s2 原始采集（为新采集腾空间）⇒ 删之前必须证明 **211 上的 60 个 tar 逐字节完整** ✓
> 本节是那次校验的完整记录，**删除后若要复盘，看这一节** ✓

### 16.1 结论

| 项 | 结果 |
|---|---|
| ★ **语料 60 个 tar** | ★★★ **60/60 逐腿与本地清单完全一致**（去掉 macOS 元数据成员后）✓ |
| ★ **dump 60 个 tar** | ★★ 60/60 存在，**每个字节数与日志记录完全一致**；日志以 `=== done` 结束 ✓ |
| **扫描方式** | 每个 tar 都跑 **完整 `tar -tvf`**（读到底，截断会报错），**0 个 ERROR** ✓ |
| **判定** | ★★★ **可以删除本地原始采集** ✓ |

**⇒ 总量核对**：tar 内真实内容 **18 819 959 个文件 / 549 699 080 474 字节** ＝ 本地清单**完全相同**（差 0 文件、0 字节）✓

### 16.2 ★★ 为什么"直接比成员数/字节"会全部对不上（重要，别被吓到）

**⇒ macOS 的 `bsdtar` 会把 xattr 存成额外成员 `._<name>`**（163 字节，AppleDouble 头 `00 05 16 07`）✓
**⇒ 所以每个 tar 的成员数 ≈ 2N＋目录项，字节数 = 清单 ＋ 163×(N−k)** ✓

**⇒ 正确的校验式（60 条腿全部满足）**：
```
(tar_bytes − manifest_bytes) 能被 163 整除
members − manifest_files − (tar_bytes − manifest_bytes)/163 == 3      # 腿根 + logs/ + tb_live/
```
**⇒ 具体核对**：AppleDouble 成员共 **18 773 419 个 / 3 060 067 297 字节**；
tar 内成员字节合计 552 759 147 771 ＝ 549 699 080 474 ＋ 3 060 067 297 ✓ 一分不差 ✓

**⇒ 物理 tar 总字节 602 280 632 320 vs 清单 549 699 080 474（+9.57%）** 的构成：
**3.06 GB AppleDouble 内容 ＋ 49.52 GB 的 tar 头/PAX-xattr 头**（约 1 317 B/成员，共 37 593 558 个成员）✓ —— **不是数据缺失** ✓

### 16.3 ★★ 独立旁证

| 检查 | 结果 |
|---|---|
| 抽一个成员算 md5，与本地文件比 | ★ 一致（`df6153954b5ca1e00c7248195e13f387`）✓ |
| **重新在磁盘上独立清点** 3 条腿（1103i、mcs02_ping_1031a、mcs00_ping_1101d）| ★ 与清单**完全一致** ✓ |
| 最大腿 `s2_mcs00_ping_1101a`（17.3 GB、110 万成员）| ★ 真实内容 551 619 文件 / 17 223 253 598 字节 ＝ 清单 ✓，tar 退出码 0 ✓ |
| 唯一一条**没有** `._` 成员的腿（`s2_mcs02_ping_1031a`）| 46 419 成员 ＝ 46 416 文件 ＋ 3 目录，字节与清单**完全相等** ✓ |

### 16.4 ★ 删除后的恢复路径（写下来以免将来找不到）

```bash
# 在 211 上取一条腿回来（tar 内是 <leg>/... 的相对路径）
ssh jwang@192.168.31.211 'mkdir -p /mnt/corpus/restore && tar -C /mnt/corpus/restore -xf /mnt/corpus/s2_corpus_tarballs/<leg>.tar'
# 再 rsync 回任何一台机器
```
**⇒ ★★ `._*` 成员是 macOS 元数据，解出来后可直接忽略/删除** ✓

### 16.5 ★★ 保留项（不要误删）

| 位置 | 内容 |
|---|---|
| 211 `/mnt/corpus/s2_corpus_tarballs/` | ★ 60 个语料 tar（565 GB）|
| 211 `/mnt/corpus/s2_tb_tarballs/` | ★ 60 个 dump tar（9.85 GB）|
| **203 `~/ai_depth3/corpus/s2_amc_ping_1102a/`** | ★ 全腿 50 000 捕获（8.6 GB，验收机在用）|
| 本机 `~/ai_depth3/datasets_*` | 训练/验证/monitor 数据集（npz/npy）|
| 历史遗留（非本批）| `s2_mcs00_ping_1031a.tar.gz`、`tb_s2_mcs00_ping_1031a.tar.gz`（11-01，**不要当成本批的一部分**）|


---

## 17. ★★★★★ 2026-11-13：**`s2_amc_ping_1102a` 是保留评测腿 —— 任何抽取都不得包含它**

**(a) 为什么**

`s2_amc_ping_1102a` 是**探针 500、尺子 5 000、全腿 50 000 的唯一来源**（`units_probe_500.txt`、`ruler_s2_amc_5000_units.txt` 都从这条腿抽）。
它的建库一旦留在母集目录里，任何"glob 母集"的抽取脚本（训练集/补集/monitor）**都会把它混进去**。

**2026-11-13 的真实事故**：`w2v3b` 的 2 PRB 补集从 `datasets_full_s2_amc/*.npz` 里抽，**包含了 1102a 的 1 603 个单元**；
探针 500 里有 **20 个**是训练时见过的 ⇒ 探针"提升 −1.30%"中，**62%（36 280 / 58 552 bit）来自背过的单元**，
剔除后**净增益 ≈ −144 bit（打平）**。详见 `depth3_network_improvement_plan.md` §117。

**(b) 处置（已执行）**

| 项 | 状态 |
|---|---|
| 1102a 的 8 个 npz（1.9 GB）| ★ 已从 `~/ai_depth3/datasets_full_s2_amc/` **移到** ★ `~/ai_depth3/datasets_eval_reserved_1102a/` |
| 母集里 1102a 剩余 | ★ **0**（母集 npz 总数 266）|
| 该目录内的 `README.md` | 写明"保留评测腿，禁止用于训练/抽取" |

**⇒ 从此所有抽母集的脚本（glob `datasets_full_s2_amc/*.npz`）在文件系统层面就抽不到 1102a** ✓

**(c) 规则（写死，别再犯）**

1. ★ **1102a 只用于评测**：探针、尺子、全腿；
2. ★ **禁止**用于：训练集、补集、monitor 集、任何"从母集抽取"的流程；
3. ★ 出"新最好"之前，必须做重叠检查：**`探针单元 ∩ 训练单元` 必须为 0**；
4. ★ 若将来新增评测腿（如某条新腿被选为 ruler），**同样要把它从母集目录移走**并登记在这里。

**(d) 一条腿的判据（供以后选评测腿参考）**

| 要求 | 说明 |
|---|---|
| 有 50 000 个接收 | 与全腿测量口径一致 |
| 覆盖四个调制 | 探针/尺子的调制分布 |
| ★ **与训练语料不同批** | 避免同域记忆（本例的教训）|


---

## 18. ★★★★★ 2026-11-13：**建库必须基于 `crc_ok`（真实样本），"高 MCS 低 SNR" 的腿要用合成路线才吃得到**

**(a) 规则（代码为准）**

`build_depth3_dataset.py` 的 `candidate_units()` 里写得很直白：

```python
# The CRC column decides whether a REAL sample can be built (the payload is the dump).
if need_tb and fields[2] != "OK":
    continue
```

**⇒ 真实样本（`need_tb=True`）的 ground truth 是**dump 里解出来的 TB**（`cap_*.bits`）；
CRC 不过 ⇒ 解出来的比特不是真的 ⇒ 这条单元不能做训练样本。**
所以"按 `crc_ok` 建库"不是可选项，是**前提**（★ 2026-11-13 曾一度误判为"crc_ok 只是测量口径"，已纠正）。

**(b) 但这不代表"高 MCS 低 SNR 的腿没用"**

同一段注释的后半句说明了另一条路：

> *A synthetic sample brings its own payload, so a capture the reference chain could not decode is still a
> perfectly good CHANNEL -- and those are exactly the captures a failing cell is made of.*（design 19.209(e)）

| 路线 | `need_tb` | 需要 crc_ok？| 用途 |
|---|---|---|---|
| ★ **真实样本** | `True` | ★ **必须** | 常规训练集（本流程的默认）|
| ★ **合成样本** | `False`（payload 自己选）| ★ **不需要** | 用捕获当"信道"，专门教**经典链路也解不出的单元** |

**⇒ 结论**：
1. 高 MCS 低 SNR 的腿**保留在语料里**（自由调度极少出现这种组合，是主动采集的稀有样本）；
2. 它们的**真实样本数天然很少**（受 `crc_ok` 限制，mcs20 有 673/50 000 的例子）；
3. 想吃这部分数据，应另建**合成本数据集**（`need_tb=False`），而不是指望真实样本。

**(c) 当前进度（2026-11-13）**

| 项 | 状态 |
|---|---|
| s2_mcs 已完成 dump+建库 | **70 条腿**（mcs00–mcs22 的一部分）|
| ★ **MCS 22 / 24 / 26** | ★ **尚未开始采集** |
| 真实样本总量 | 2 532 426 个 dump 单元 → 建库后按 `crc_ok` 取用 |
| 另有被拒 | dump 的 **DM-RS 一致性检查**拒掉一部分（单腿最多 3 893 个），日志理由 `the DM-RS convention is wrong` |

**(d) 实践口径**

| 场景 | 用什么 |
|---|---|
| 建真实训练集 | ★ 只取 `index.csv` 里 `crc=OK` 的单元（脚本已如此）|
| 统计"这条腿值不值" | 看 **`crc_ok`**（决定真实样本上限），不要看 `units` |
| 想把"经典解不出"的捕获也用上 | ★ 建**合成**数据集（`need_tb=False`）|
| 对账 | `units == rows`（本次 70 条腿全通过 ✓）|


---

## 19. ★★★★★ 2026-11-20 10:40 **新的 held-out 监控集 `datasets_monitor_1102a`（取代旧 monitor_w2）**

**(a) 为什么必须换**

| 事实 | 数值 |
|---|---|
| 旧 `datasets_monitor_w2` | **730** 个样本 |
| ★ 其中出现在训练集 `w2v3c` 里的 | ★ **270 个（37%）** |
| 后果 | ★ **val 不是 held-out** ⇒ 天然偏乐观、与交付不同步 ⇒ 解释了我们记录到的 **≥10 次 val 反转** |

**(b) 新的监控集**

| 项 | 值 |
|---|---|
| 路径 | ★ `~/ai_depth3/datasets_monitor_1102a/`（19 个 `.npy`，**13.7 MB**）|
| 样本数 | ★ **108** |
| 来源 | 1102a **评估保留集**（43 589 样本）——**它不在任何训练集里** |
| ★ 剔除 | ★ **探针 500 ＋ 尺子 5 000 的全部单元**（保证监控与测量互不重叠）|
| ★ 分层 | ★ **调制（4）× PRB 宽窄（2）× SNR 带（4）**，每层 4 个，共 30 层 |
| 调制分布 | 256QAM 34 / QPSK 25 / 64QAM 25 / 16QAM 24 |
| SNR 范围 | −3.2 ～ 31.0 dB |
| ★ 与训练集交集 | ★ **0**（键 = rnti ＋ nof_bits ＋ snr ＋ qm）|
| 构建脚本 | `work_tmp/dataset_build/build_monitor_1102a.py`（含 `bits/tb_bits` 的定宽零填充）|

**(c) 用法（新 run 的标准参数）**

```bash
--val ~/ai_depth3/datasets_monitor_1102a \
--no-val --monitor-every 5 --bler-samples 24 --select-on none
```

**(d) ★★ 但要清楚它的**能力边界**（2026-11-20 实测）**

对 5 个**尺子已知**的 checkpoint，在同一新监控集上算 uncoded BER（24–26 dB 带）：

| 模型 | 尺子（vs CPU）| 24–26 dB BER |
|---|---|---|
| s1_e39 | **+0.24%** | **0.0145**（最低 ✓）|
| s2_e32 | −0.06% | 0.0148 |
| s1_e40 | −0.65% | 0.0148 |
| llrp_b16_e38 | −0.83% | 0.0151 |
| s2_e37 | −0.56% | 0.0154 |

**Spearman（交付比特 vs BER）= −0.62（p=0.27，n=5）⇒ 方向对、但**不显著**、个体排序会错。**

**⇒ 结论：监控集的用途是"**健康与趋势**"（是否发散、是否在改善），**不是"给模型排名"**；
排名仍然只能靠**尺子（5 000 单元）**。**

**(e) ★ 2026-11-20 统一的工作目录（四台机器一致）**

| 路径 | 内容 |
|---|---|
| `$HOME/work/ai_phy_train/data` | 数据集（`datasets_train_w2v3c`、`datasets_monitor_1102a`、语料…）|
| `$HOME/work/ai_phy_train/lib` | depth3 训练/分析代码 |
| `$HOME/work/ai_phy_train/runs` | 训练 run 目录 |

**⇒ Mac 与 203 上这三项是**符号链接**（分别指向 `~/ai_depth3`、`~/dev/ocudu/lib`、`~/ai_depth3/runs`），
所以**同一条命令在四台机器上都能直接跑**，不必再区分 `~/ai_depth3` 与 `~/work/ai_phy_train`。**

---

## 20. ★ 2026-11-22 批处理：MCS 22/24/26/27 共 53 腿的 dump ＋ 归档 ＋ 建库

**任务**：把 `~/ai_depth3/corpus/s2_mcs{22,24,26,27}_*` 共 **53 腿、2 622 624 单元**做完整流程（**amc 腿不做**）。

| 步 | 产物 | 位置 |
|---|---|---|
| dump | `tb_<腿>/`（`index.csv` ＋ `tb/*.bits`）| `~/ai_depth3/dump_s2_mcs/` |
| 归档 | `tb_<腿>.tar.gz` | **`/Volumes/CORPUS/s2_mcs_tb_tarballs/`** |
| 建库 | `<腿>_<MOD>_<self\|harq>.npz` | **`/Volumes/CORPUS/datasets_full_s2_mcs/`** |

**执行器**：`/tmp/mcs_groups.sh`（生成脚本，**每 4 腿一组、组间 `wait` 作屏障**）＋ `/tmp/one_leg.sh`（单腿：dump → tar → build）。
每组 4 腿 × `--jobs 3` = **12 个 replay 进程**，匹配本机 14 核。

**可恢复**：tarball 存在 ⇒ 跳过 dump；npz 存在 ⇒ 跳过 build。

**(a) ★ 踩过的坑（两次把 load 推到 75）**

| 尝试 | 做法 | 结果 |
|---|---|---|
| 1 | `xargs -a`（macOS 无此选项）| 立即退出，没干活 |
| 2 | `xargs -I{} -P 4` | ★ **`-I` 使 `-P` 失效 ⇒ 20 腿同时启动，load 75** |
| 3 | `jobs -rp \| wc -l` 计数 | ★ **命令替换是子 shell，job 表为空 ⇒ 计数恒 0 ⇒ 又是 20 腿** |
| ★ 4 | ★ **静态分组脚本 ＋ `wait`** | ★ **验证：4 个 dump 实例、load ≈ 15** ✓ |

**(b) 实测速率与 ETA**

| 项 | 值 |
|---|---|
| 单腿 dump（50 000 单元，3 worker）| 约 **50 min**（≈1 000 单元/min）|
| 4 腿并行 | 同上（4 × 3 = 12 进程）|
| 53 腿 dump 总计 | ★ **约 11 h** |
| 建库 | 每腿约 40 min（4 腿并行）⇒ 约 **9 h**，与 dump 交错进行 |

**(c) 监控**

```bash
tail -f ~/ai_depth3/logs_mcs_pipeline/<腿>.log
ls /Volumes/CORPUS/s2_mcs_tb_tarballs/tb_s2_mcs2*.tar.gz | wc -l   # 已归档腿数
ls /Volumes/CORPUS/datasets_full_s2_mcs/s2_mcs2*_*.npz | wc -l     # 已建库腿数
```

---

## 21. ★★★ 2026-11-22 11:45 **在 macOS 上"降低优先级"的正确做法：只 `nice`，不要 `taskpolicy -b`**

**事故**：为了让 53 腿建库"让路"，我对整个进程树同时做了 `renice +19` **和** `taskpolicy -b`（后台 QoS）。
结果：★ **10 个性能核几乎完全空闲**（CPU 历史图上前 4 个核在跑、5–14 号核全空），建库速度骤降。

**原因**：

| 手段 | 实际效果 |
|---|---|
| ★ `renice +19` | ★ **只降调度优先级**，仍可跑在任何核上 ✓ 正确做法 |
| ★ `taskpolicy -b`（后台 QoS）| ★ **把线程标记为"后台"**：Apple Silicon 上会被**限制到能效核**并大幅节流 ⇒ **性能核拿不到活** ✗ |

**正确做法**：

```bash
# 只降优先级（对整棵进程树；子进程会继承 nice）
pids=$(ps -eo pid,ppid | ... )        # 或直接对根进程
renice +19 $pids

# 如果之前设过后台 QoS，撤销它
taskpolicy -B -p <pid>                # -B = 取消 background 策略
```

**判据**：`uptime` 的 load 若 **长期 < 能效核数（本机 4）**，说明任务被压到只剩能效核 ⇒ 检查是否有后台 QoS。

**结论**：★ **"降优先级但不停止调度" = `nice`；"停止调度" 才是 `-STOP`/后台 QoS —— 两者不要混用。**

---

## 22. ★★★ 2026-11-22 15:40 **归档前必须删 `stage/` 和 `rows/`（否则 tar.gz 大 200 倍）**

**事实**：`dump_tb.sh` 的产物里，`stage/` 是**每个单元一份的捕获副本**（`<out>/stage/<unit>.bin/_h.bin/_ce.txt` ＋ 回放器的暂存输出），
`rows/` 是**每单元的草稿行**。两者都是**中间产物**：

| 产物 | 建库要不要 | 抽训练集要不要 | 说明 |
|---|---|---|---|
| ★ `index.csv` / `index_*_csv` | ★ **要** | ★ **要** | 每单元判决（crc / iterations / tbs_bytes）|
| ★ `tb/<unit>.bits` | ★ **要**（标签）| 经母集 | 解出的 TB 比特 |
| ★ `units.txt` | ★ **要**（对账）| — | 单元清单 |
| ★ **`stage/`** | ★ **不要** | ★ **不要** | ★ **每腿约 6.7 GB**（≈ 整条语料的拷贝）|
| ★ **`rows/`** | ★ **不要** | ★ **不要** | 每腿约 195 MB |

**实测对比（2026-11-22）**：

| 归档方式 | 大小 |
|---|---|
| 旧归档（归档前删 `stage`）| **6–12 MB / 腿** |
| ★ **本次前 44 腿（误把 `stage` 打进 tar）** | ★ **2.2 GB / 腿（合计 75 GB）** |
| ★ **修正后重建** | ★ **3–80 MB / 腿** |

**⇒ 修正**：`one_leg.sh` 在 `tar` 之前执行 `rm -rf <dump>/stage <dump>/rows`，
并把归档写成 `.tmp` 再 `mv`（中断不会留下半截 tar）。

**⇒ 同时释放**：本机 `dump_s2_mcs/` 里前 44 腿的 `stage/` 已删 ⇒ **释放约 200 GB 内置盘**。

---

## 23. ★★★★★ 2026-11-23 09:00 **规则：读归档要**流式按成员读**，不要解整包**

**(a) 规则**

★ **需要归档里的某个文件时，用 `tar -xOzf <archive> <member>` 直接流到 stdout**，**不要**先 `tar xzf` 解包再读：

```bash
# ★ 正确：只把需要的成员读进内存/管道，磁盘零产物
tar -xOzf /Volumes/CORPUS/s2_mcs_tb_tarballs/tb_s2_mcs00_ping_1031a.tar.gz \
    tb_s2_mcs00_ping_1031a/index.csv | head

# ✗ 错误：先解整包（一条腿约 200 MB，53 条腿约 10 GB；全库 tar 会到 500 GB 级）
mkdir -p /tmp/x && tar xzf <archive> -C /tmp/x
```

**(b) ★ 为什么（实测，2026-11-23 的 `iterations` 回填）**

| 项 | 实测 |
|---|---|
| 用流式读完 **586 个归档**里的 `index.csv` | ★ **零临时文件** |
| `/tmp` 条目数（前后）| **52 597 → 52 597（不变）** |
| 归档文件 mtime | ★ **未被改动** |
| ★ 整轮回填的磁盘增量 | ★ **< 10 MB**（每个 npz 追加一个 `iterations.npy` 成员）|
| 若改为解整包 | ★ **数百 GB 级**且要事后清理 |

**(c) ★ 实现要点（Python）**

```python
proc = subprocess.run(["tar", "-xOzf", archive, member], capture_output=True, text=True)
if proc.returncode == 0 and proc.stdout:
    rows = csv.DictReader(io.StringIO(proc.stdout))    # 直接在内存里解析
```

**⇒ 现成例子：`ai_train/backfill_iterations.py`（本机 dump 优先，缺失时流式读归档）。**

**(d) ★ 什么时候才真的需要解包**

| 场景 | 做法 |
|---|---|
| ★ 只读其中一两个文件 | ★ **流式按成员读**（本规则）|
| 需要同时访问大量成员（如重跑 dump 的全量校验）| 解到**专用临时目录** `…/_extract_tmp/`，**用完立即删**，并在文档里记下占用 |
| 离线机器没有本机 dump、必须重建母集 | 同上，且**优先只解 `index.csv` ＋ `tb/*.bits`**（真正的契约产物），**不要解 `stage/`**（§22）|

**(e) ★ 判据（一条命令自查是否“流式”）**

```bash
# 读归档期间，下面的数量应当不增长
ls /tmp | wc -l
```

---

## 24. ★★★★ 2026-11-23 **macOS 训练路径已单列成节（§9.2）：三句话结论 ＋ 去哪看**

> 本节只是**指针 ＋ 摘要**，细节（改动清单、逐项实测、对 CUDA 的逐条影响、验收命令）全在 **§9.2**。

| 一句话 | 结论 |
|---|---|
| ① Mac 上到底该用谁 | **c32 用 CPU**（`--no-gpu`）：本机 Metal GPU 慢 **1.87×**；只有 c128 且 batch ≥ 16 时 GPU 才赢（1.20–1.37×）|
| ② 本机一轮多久 | 160 000 样本、PRB 1–51、c32：**32–33 分钟/轮**（batch 8 最快）；60 轮约 32 小时 |
| ③ 两个会"崩"的配置 | macOS 上 **`--dtype float16`**（Metal 插件段错误，已被代码拦截）与 **`--jit`**（XLA 在 Metal 上 `NotFoundError`，默认已关）|

**⇒ 装 GPU 插件的前提**：`tensorflow` **2.18** ＋ `tensorflow-metal` **1.2**（Apple 的对照表只到 2.18；装 2.21 永远看不到 GPU）。**用不用得上，只看横幅 `platform: compute placement:`，不看 `list_physical_devices()`。**

> ★ **本节是摘要**：完整实验记录（夹具、FLOP 估算、无效手段、未解问题、复现命令）见 **`macos_apple_silicon_training_memo.md`** ✓

---

## 9.2.5 ★★★★★ 2026-11-23 14:30 **对 Ubuntu/CUDA 路径的逐条复核 ＋ 本机实测速度**

**(a) ★ 复核方法**

1. `train_depth3.py` 的 macOS 改动**尚未提交**（`git diff`：+837 / −157）；逐条读 diff；
2. 检查每个平台相关决定是否**被 `IS_DARWIN` 挡住**（Linux 走原路径）；
3. 检查跨平台改动是否可能改变**数值**（而不是只改 host 侧耗时）；
4. 本机用**同一配方**（c32 / 18 块 / batch 16 / `--dense-skip-entry --se` ＝ Ce＋B）实测。

**(b) ★★ 平台闸门（Linux 全部走原路径）**

| 位置 | Linux 行为 |
|---|---|
| `resolve_xla` | ★ `if not IS_DARWIN: return bool(requested), None` ⇒ **Linux 完全按命令行** |
| `pipeline_defaults` | ★ Linux：`--workers`/`--prefetch` 原值（`0` 仍是串行）|
| `hide_gpus` | 只有写了 `--no-gpu` 才调用 |
| `configure_macos_runtime` / `report_macos_runtime` | 只在 `IS_DARWIN` 时执行 |
| dtype 护栏 | 只在 macOS 报错 |
| `xla_self_test` | 只在 macOS 且 `--jit` 时跑 |
| 验证集 workers | ★ Linux `(0, 0)`（串行，**与改动前一致**）|

**(c) ★★ 跨平台改动：只省 host 时间，不改数值**

| 改动 | 为什么不改数值 |
|---|---|
| ★ `LOSS_ACCUMULATION=64` | 每 64 个 batch 才 `tf.stack(...).numpy()` 回读一次；**列表内容与逐个回读逐元素相同** ⇒ `np.mean(losses)`、history、快照全部不变（注释里也写明了为什么不用 float32 running sum）|
| 元数据列一次性读入内存 | 只是减少随机页错误；数组内容不变 |
| `_GeometryCache`（有界 FIFO ＋ 锁）| 缓存的是**只读**几何（掩码/索引/权重），未命中时在锁内构建；256QAM 掩码 274 KB/样本不再重建 |
| `unpackbits(count=nof_bits)`、`asarray` 不复制 | 与原来结果相同 |
| `tensors()` 只打包图里有的 head | 同一命令行 ⇒ 同一个图 ⇒ 打包的键**与原来一致** |
| `genie_labels` 按 qm 缓存并冻结 | **修复**（原来混合调制会用错星座，标签静默错误）|
| `CUSTOM_OBJECTS` 补 `MaskedSmooth` | **修复** `--export-only` 必然失败的路径 |

**(d) ★ 三条**有意的**跨平台行为变化（不是破坏，但要知道）**

| # | 变化 | 影响 |
|---|---|---|
| ★ 1 | ★ **XLA 默认改为 on**（`2b48a73b2a`）| Linux 上 **变快**（1.3–2×），但**与历史 run 的执行方式不同**；要复现旧行为请写 `--no-jit` |
| ★ 2 | 损失回读从"每 batch 同步"改为"每 64 batch" | ★ **CUDA 上会提升利用率**（注释称旧写法只到 47%）|
| ★ 3 | 数据侧优化 | host 时间下降，所有平台都受益 |

**(e) ★★ 一个必须点出的事实：GPU 机器上跑的是**旧文件**

| 机器 | `train_depth3.py` sha256（前 16）| 含 macOS 改动 |
|---|---|---|
| 本机（工作树）| `898ab426e8cc500c` | ✓ |
| 106 / 107 / 126 | `b67c078b5917ea20` | ★ **✗ 不含**（`IS_DARWIN`／`_GeometryCache` 出现 0 次）|

**⇒ 所以"GPU 上已经跑了几小时没问题"**不能**作为本文件的证据** —— 它们跑的是旧版本。
**⇒ 建议：任一台 GPU 空闲时，用本文件跑一次 20 步的 smoke（`--epochs 1` ＋ 小数据集），确认能训起来再上线。**

**(f) ★★ 本机实测（2026-11-23，c32 / 18 块 / batch 16 / Ce＋B 配方；2 000 样本夹具）**

| 设备 | epoch 2（125 步）| ★ ms/样本 | 全量 183 624 样本/轮（推算）|
|---|---|---|---|
| ★ **Metal GPU**（`GPU:0`）| **50 s** | ★ **25.0 ms** | ★ **≈ 77 min** |
| CPU（14 核，`--no-gpu`）| 77 s | **38.5 ms** | ≈ 118 min |

| 对照 | 值 |
|---|---|
| 3060 Ti（106）/ c32 18 块 batch 16 | **≈ 13 min/轮** |
| **结论** | ★ **Mac 比 3060 Ti 慢约 5–6×** ⇒ **只用于 smoke 与功能验证，不做正式训练** |
| ★ 顺带验证 | ★ **batch 16 时 GPU 比 CPU 快 1.54×**（memo §9.2.3 的"c32 batch 8 上 CPU 更快"只在更小 batch 成立）|

### 9.2.6 ★★★★★ 2026-11-23 14:40 **复核"本机 CPU 线程别开太多、3 最优"这条：**2 略优于 3**，≥4 比串行还慢**

**(a) 实测（`Batches.epoch`，2 000 样本，batch 16，QPSK/cond/spatial，best-of-2）**

| workers | ms/样本 | 相对串行 |
|---|---|---|
| 0（串行）| 0.466 | 1.00× |
| 1 | 0.463 | 1.01× |
| ★ **2** | ★ **0.354** | ★ **1.32×** |
| **3** | **0.392** | **1.19×** |
| 4 | 0.523 | ★ **0.89×**（比串行慢）|
| 6 | 0.668 | ★ **0.70×**（比串行慢）|

**(b) ★ 结论**

1. ★ **memo 的"别开太多"是对的**：★ **4 线程起就比串行还慢**（GIL ＋ 统一内存带宽争用）；
2. ★ **最优是 2**，3 只慢 10%；`pipeline_defaults` 现在给的是 `min(3, 核数/4)` ＝ **3** ⇒ **落在最优区间的边缘**，可接受；
3. ★ **但在当前配方下** loader 只占 epoch 时间的 ~2%（loader 0.47 ms/样本 vs 端到端 25 ms/样本）⇒
   ★ **线程数对总墙钟时间几乎无影响**；它只在"模型很小／batch 很大"时才是瓶颈（memo §9.2.3 的场景）。

**(c) ★ 若要把默认调到 2**

★ 只需把 `pipeline_defaults` 的 `min(3, ...)` 改成 `min(2, ...)`；**收益 ≤ 2% 的 wall time**，
且会让"3 线程"这一条历史记录与默认值不一致 ⇒ **建议保持 3，把实测写在这里备查。**

### 9.2.7 ★★★★★ 2026-11-23 14:55 **"本机比 GPU 慢 5×"这个结论的**确切条件**（以及它其实随卡而变）**

**(a) ★ 本机那一侧的条件**

| 项 | 值 |
|---|---|
| 数据 | ★ **`/tmp/mini_w2v3c` ＝ w2v3c 的前 2 000 个样本**（不是全量 183 624）|
| 计时 | ★ **第 2 轮（warm）**，`epoch 2 累计 − epoch 1 累计` |
| 硬件 | Mac M4 Pro，**Metal GPU:0**，XLA off，`--workers 0`→默认 3 线程 |
| 配方 | ★ **c32 / 18 块 / batch 16**；plain vs **Ce＋B（`--dense-skip-entry --se`）** |
| 外推 | ★ **线性外推**到 183 624 样本（假设每样本耗时与样本数无关）|

| 配方（Mac）| warm epoch（2 000 样本）| ms/样本 | 全量/轮（外推）|
|---|---|---|---|
| plain c32 | 43 s | **21.5** | **65.8 min** |
| ★ **Ce＋B** | 50 s | ★ **25.0** | ★ **76.5 min** |

**(b) ★★ GPU 那一侧的条件（来自正在跑的真实日志，非估算）**

| 机器 | 配方 | 实测 | 折算 18 块 |
|---|---|---|---|
| **106**（3060 Ti）| **Ce＋36 块** | **28.4 min/轮**（4 轮 11 298 s）| ★ **≈ 14.2 min** |
| **107**（3060 Laptop）| ★ **Ce＋B（18 块）** | ★ **20.8 min/轮**（4 轮 4 994 s）| 20.8 min |
| **126**（GTX 1070）|  Ce（18 块）| **32.9 min/轮**（4 轮 7 886 s）| 32.9 min |

**(c) ★★ 因此"5×"只在**对 3060 Ti**时成立**

| 对比 | 比值 |
|---|---|
| ★ Mac（Ce＋B，76.5 min）vs **3060 Ti 的 18 块等价值（14.2 min）** | ★ **5.4×** |
| ★ Mac（Ce＋B，76.5 min）vs **3060 Laptop 的 Ce＋B（20.8 min）** | ★ **3.7×** |
| ★ Mac（plain c32，65.8 min）vs **GTX 1070 的 Ce（32.9 min）** | ★ **2.2×** |

**⇒ 结论要写全：**"本机训练比 **3060 Ti** 慢约 **5×**；比 3060 Laptop 慢约 **3.7×**；比 GTX 1070 慢约 **2.2×**。
之前 §9.2.5 里那个 5× 只对 3060 Ti（且把 Ce＋B 与 plain c32 混在一起比），口径不严，这里更正。**

**(d) ★ 这个外推有多可信**

| 风险 | 说明 |
|---|---|
| 样本数与耗时 | ★ 每样本成本与样本数无关（无跨样本状态）⇒ 线性外推可信 |
| ★ 小夹具的缓存效应 | ★ 2 000 样本可能整段落在页缓存里 ⇒ **外推值偏乐观**（真实全量数据第一次读盘会更慢）|
| 结论 | ★ **"慢 5×"是**下界**（实际可能更慢）**，但用于"别在 Mac 上正式训练"的判断已经足够 |

---

## 24. ★★★★★ 2026-11-24 17:05 **测试集 1102a 的防泄漏规则；amc 母集 iterations 回填与 34/35 的澄清**

**(a) ★ 背景**

★ **`s2_amc_ping_1102a` 是评测语料**（ruler 5 000、HARQ 348、full leg 都从它来）。
★ 由此产生的规则：**任何训练侧数据源都不得包含它。**

**(b) ★★ 三条规则（R1–R3）**

| 规则 | 内容 | 落地 |
|---|---|---|
| ★ **R1** | ★ **训练侧数据源（母集、索引、归档）一律排除 `*1102a*`** | ★ `backfill_iterations.py` 新增 **`--exclude-leg`**，**默认排除 `s2_amc_ping_1102a`** |
| ★ **R2** | ★ **抽集/回填脚本显式断言不含 1102a**（不依赖"母集恰好没有"）| ★ 已实现：排除列表会在日志里**逐次打印**（`excluded legs: [...]`）|
| ★ **R3** | ★ **1102a 只服务评测**：ruler / HARQ / full leg 的 dump 与归档**只读不写进训练产物** | ★ 手册 §24 本节 |

**(c) ★★ 检查结果：amc 母集本来就不含 1102a**

| 检查 | 结果 |
|---|---|
| `/Volumes/CORPUS/datasets_full_s2_amc/` 中匹配 `1102a` | ★ **0** |
| 该母集的腿 | ★ `1102b`–`1102t`、`1103a`–`1103h`、`1103i`–`1103o`（**34 条**）|
| `/Volumes/CORPUS/datasets_full_s2_mcs/` 中匹配 `1102a` | ★ **0** |

**⇒ 无需删除任何文件；规则是为了"以后也不会"。**

**(d) ★★ 澄清一个容易误读的数字：34 与 35**

| 集合 | 数量 | 内容 |
|---|---|---|
| 本地 dump 有 `index.csv` 的腿 | **34** | 1102b–1102t、1103a–1103o |
| ★ `s2_amc_tb_tarballs` 里的 tb 归档 | ★ **35** | ★ **以上 34 条 ＋ `s2_amc_ping_1102a`** |
| 差集 | ★ **只有 1102a** | ★ 反向差集为空 |

**⇒ 所以：**35 ＝ 34 ＋ 测试腿**；本次回填 34/34 全部命中本地索引，`--tarball-dir` 一次都没用上（保留它只是与 mcs 场景共用同一工具）。**

**(e) ★ amc 母集的 iterations 回填结果**

| 项 | 值 |
|---|---|
| 文件 | ★ **266 / 266** 全部加上 `iterations` |
| 方式 | ★ **向 zip 追加成员**（不重写已有成员，每个文件只增几 KB）|
| 抽样 31 万样本的分布 | `1: 111 751 / 2: 83 117 / 3: 21 820 / 4: 13 212 / 5: 11 570 / 6: 68 944` |
| ★ **critical（≥4）** | ★ **30.2%**（mcs 母集为 15.8%）|
| 未知 `-1` / 未解码 `0` | **0 / 0** |

**⇒ amc 母集的"临界样本"密度是 mcs 的 1.9 倍 ⇒ 按 `iterations ≥ 4` 抽难题时，amc 语料更划算。**

**(f) ★ 口径（不要混）**

| 值 | 含义 |
|---|---|
| `-1` | 链没有记录迭代数（合成样本或旧 index）⇒ **未知**，不是"容易" |
| `0` | 解码失败（KO）⇒ **真实值** |
| 1–6 | 实际迭代数（6 为封顶）|

---

## 25. ★★★★★★ 2026-11-25 15:25 **新采集（1121/1123/1124，67 条腿）全部完成 dump / 建库 / 归档；全库母集 101 条腿**

**(a) ★★ 结果**

| 项 | 值 |
|---|---|
| ★ 新腿 | ★ **67 条**（1121a–n、1123a–z/aa–ai、1124a–o），**3.35 M 单元、681 GB 原始语料** |
| ★ 母集（全库） | ★ **740 个 npz**，**101 条腿**（旧 34 ＋ 新 67）|
| ★ `iterations` 覆盖 | ★ **新腿 474 个文件全部带该字段**（原生写入，无需回填）|
| ★ critical（≥4）| ★ **新腿 2.78 M 样本中 13.6%**（376 403）|
| ★ dump 归档 | ★ **102 个** `tb_*.tar.gz` |
| ★ 语料归档 | ★ **102 个** `<leg>.tar.gz` |
| ★ 母集体积 | ★ **184 GB**；CORPUS 剩余 **1.8 TB** |
| ★ 测试腿 1102a | ★ **不在母集内（0 个文件）** ✓ |

**(b) ★★ 过程中修掉的三个问题**

| # | 问题 | 处置 |
|---|---|---|
| ★ 1 | ★ 旧 runner 被杀后 **`xargs` 派发器变孤儿**，继续产生 replay、目录清不掉 | ★ 按派发器 pid 杀；archiver 先 `rm -rf stage rows raw` |
| ★ 2 | ★ `index.csv` **边写边刷**，只看它存在会误判“已完成” | ★ 判据改为 **index 行数 ＝ 有 `.bin` 的单元数** |
| ★ 3 | ★ `1124o` 的 9 个单元**采集时没落地 `.bin`**（19 080 里 19 071）⇒ 被判“失败” | ★ 同一处修正解决；该腿按 **19 071** 单元入库 ✓ |

**(c) ★ 下次新采集的推荐流程（已固化为脚本 `/tmp/run_new_amc_flow.sh` 的形状）**

1. ★ **流水化**：每腿一个 worker（dump → 建库 → 归档），**4 腿同时**在飞 ⇒ 三段自然重叠；
2. ★ **全程 `nice -n 19`**（本机同时是开发机与采集机）；
3. ★ **幂等 ＋ 严格完成判定**（见 (b) 2/3）；
4. ★ **归档内容与历史一致**：`index*.csv`、`units.txt`、`tb/`（**不含** `stage/`、`rows/`、`raw/`）；
5. ★ **语料归档一腿一包**。

**(d) ★ 下一步**

★ 全库已可按 `iterations ≥ 4` 抽"难题训练集"；★ **amc 母集的 critical 密度是 mcs 的 1.9 倍**（30.2% vs 15.8%，旧腿口径），
★ 抽集时**必须排除 1102a**（已由 `backfill_iterations.py --exclude-leg` 与母集内容双重保证）。

---

## 26. ★★★★★★ 2026-11-25 20:15 **为什么历史归档里没有 `tb_live/`：它的内容已经进了 **dump 的 `tb/`**（不是被丢弃）**

**(a) ★★ 问题**

★ 新采集的 67 条腿都有 `tb_live/`（`1124l` 一条就 44 GB），但历史归档（mcs 与老 amc，共 102 个）**只留 `logs/`、`tb_live/` 三个空目录条目**（手册 §16.2 的核对式 `members − manifest_files − AppleDouble == 3` 就是证据）。

**(b) ★★ 答案：`tb_live` 的比特已经被 `harq_backfill.py --live-tb` **复制进 dump 的 `tb/`**，而 dump 是被归档的**

| 证据 | 数值 |
|---|---|
| ★ dump 目录里有 `index_harq_only.csv` | ★ **3 884 行**（回填确实跑过）|
| ★ dump 的 `tb/*.bits` | ★ **44 584 个** |
| ★ `index.csv` 里自己解出的单元 | ★ **38 158 个** |
| ★ **差** | ★ **6 426 个** ＝ ★ **从 `tb_live` 恢复出来的 HARQ 样本地** |
| ★ tb 归档内容 | ★ `tb/`（82 743 项）＋ `index.csv`、`index_self.csv`、`index_harq.csv`、`index_harq_only.csv`、`units.txt` ✓ |

**⇒ 所以：★ `tb_live` 的信息**没有丢**——它在 `<dump>/tb/` 里，而 `<dump>` 被完整归档到 `s2_amc_tb_tarballs/tb_<腿>.tar.gz`。**

**(c) ★ 历史做法的逻辑（现在说得通）**

1. ★ 采集时 gNB 写 `tb_live/`（活链合并解码的产物，离线不可重建）；
2. ★ 建库时 `harq_backfill --live-tb` 把其中**每个可用 TB 的比特复制**到 `<dump>/tb/`，并写 `index_harq_only.csv`；
3. ★ 归档时**只归档 dump**（含复制过来的比特）与**原始捕获**，`tb_live/` 原始目录**不需要再存一份**；
4. ★ ⇒ 手册 §16.2 的 "3 个目录条目" 正是这个意思：留空壳、不存内容。

**(d) ★ 因此归档规则（与历史一致，且有据）**

| 目录 | 是否入档 | 理由 |
|---|---|---|
| ★ `cap_*` ＋ `._*` | ✓ | 原始捕获，不可重建 |
| ★ `<dump>/tb/` ＋ 索引 | ✓ | ★ **含 `tb_live` 恢复出的 HARQ 比特** ⇒ `tb_live` 的内容在此 |
| `tb_live/`、`logs/` | ✗（只留空目录条目）| 前者已复制进 dump，后者是运行日志 |

---

## 27. ★★★★★★★ 2026-11-25 20:40 **MEMO：采集腿目录的组成、`tb_live/` 的解剖、以及"归档什么"的最终裁定**

> **一句话**：一条腿里有 **`cap_*`（原始捕获）**、**`logs/`（运行日志）**、**`tb_live/`（活链产物）**；
> **归档只存 `cap_*`，dump 另存 `<dump>/tb/`**。`tb_live/` 的 **TB 比特**已经复制进 dump 的 `tb/`，
> 而它体积的 **88% 是 `rx_*.f32`（AI CE 用的接收样本副本）**，对 depth3 **零价值**。

**(a) ★ 一条腿的目录组成（实测 `s2_amc_ping_1121a`）**

| 目录/文件 | 数量 | 体积 | 性质 |
|---|---|---|---|
| `cap_*.bin`（含 `_h.bin`）| 100 000 | **6.48 GB** | ★ 原始捕获（不可再生）|
| `cap_*.txt` / `_ce.txt` | 100 000 | 0.57 GB | 侧车元数据 |
| ★ **`tb_live/`** | 95 438 | **0.66 GB** | ★ 采集时 gNB 写出的派生产物 |
| `logs/` | — | ~2 GB | 运行日志 |
| 合计 | ~200 000 | **8.0 GB** | — |

**(b) ★★ `tb_live/` 的解剖（`1121a`，逐类型实测）**

| 文件 | 数量 | 体积 | ★ 占比 | 内容 | 对 depth3 |
|---|---|---|---|---|---|
| ★ `rx_*.f32` | 51 650 | ★ **0.578 GB** | ★ **88%** | ★ 每次采集的**接收样本 float32** | ★ **零**（AI CE 线用）|
| `record.jsonl` | 1 | 0.045 GB | 7% | 采集逐次记录 | 零 |
| ★ **`tb_*.bits`** | 43 785 | ★ **0.031 GB** | ★ **5%** | ★ **活链每块解出的 TB 比特** | ★ **有用**（HARQ 标签）|
| `dd_meta.csv` | 2 | 0.005 GB | <1% | TB↔系统时隙 | 有用（配对）|

**⇒ 体量对比：`tb_live` 0.66 GB vs `<dump>/tb/` **6.8 MB**（只存解出的比特）⇒ 约 **100:1**；
★ 极端腿 `1124l`（总 **53 GB**）的 `tb_live` 达 **44 GB**（采集时间长 67 倍），其归档曾达 **23 GB**。**

**(c) ★★ 产生与可重建性**

| 项 | 结论 |
|---|---|
| 产生时机 | ★ **采集飞行中**由 gNB 写（`OCUDU_HELENA_DUMP_DIR`）|
| ★ **TB 比特可否重建** | ★ **不可以**：依赖活链的 **HARQ 软合并**解码，离线回放复现不了（`harq_backfill.py` 明写）|
| ★ 但它的信息在哪 | ★ **`harq_backfill.py --live-tb` 把它复制进 `<dump>/tb/`** ⇒ ★ **已随 dump 归档**（实测：`tb/` 44 584 个 bits vs 自解 38 158 ⇒ 6 426 个来自 tb_live）|
| `rx_*.f32` 可否重建 | 需重新采集；但 depth3 不用它 |

**(d) ★★ 历史归档的标准（实测确认）**

| 归档 | 目录条目（完整扫描）| `tb_live` 内容 |
|---|---|---|
| `s2_mcs27_ping_1120g` | `腿根`、`logs`、`tb_live` ★ **共 3 个** | ★ **空** |
| `s2_mcs00_ping_1031a` | 同上 | 空 |
| `s2_amc_iperf_1103i` | 同上 | 空 |

**⇒ 与手册 §16.2 的核对式 `members − manifest_files − AppleDouble == 3` 完全一致。**

**(e) ★★★ 裁定**

| 内容 | 入**语料**归档 | 入 **dump** 归档 | 理由 |
|---|---|---|---|
| `cap_*` ＋ `._*` | ★ **是** | — | 原始，不可再生 |
| `logs/`、`tb_live/` | ★ **只留空目录条目** | — | 与历史一致 |
| `<dump>/tb/`、`index*.csv`、`units.txt` | — | ★ **是** | ★ **含从 `tb_live` 复制的 HARQ 比特** |
| `tb_live/rx_*.f32` | ✗ | ✗ | ★ 采集数据的 float32 副本，depth3 不用；**若 AI CE 需要，应由那条线自行归档** |

**(f) ★ 本批（67 腿）的体积账**

| 方案 | 语料归档合计 |
|---|---|
| 含 `tb_live` | **245 GB** |
| ★ **按本 memo** | ★ **约 134 GB** |
| ★ 节省 | ★ **约 111 GB**（其中 1124l 一条省 21 GB）|

---

## 28. ★★★★★★★ 2026-11-25 21:15 **ERRATUM（更正 §26/§27）：**所有归档（mcs 与 amc）都包含 `tb_live/` 的**文件**，"只留空目录"的说法是错的**

**(a) ★★ 全量扫描结果（**完整遍历每个包**，不看窗口、不抽样）**

| 归档 | 大小 | `cap_*` | `._*` | ★ **`tb_live/` 文件** | `logs/` 文件 | 目录条目 |
|---|---|---|---|---|---|---|
| ★ `s2_mcs00_ping_1031a`（最老 mcs）| 3.84 GB | 178 432 | 178 432 | ★ **175 224** | **4** | ★ **3** |
| ★ `s2_mcs27_ping_1120g`（最新 mcs）| 4.00 GB | 199 996 | 199 996 | ★ **156 064** | **4** | ★ **3** |
| ★ `s2_amc_ping_1102a`（老 amc）| 3.07 GB | 200 000 | 297 443 | ★ **97 442** | **4** | 3 |
| ★ `s2_amc_ping_1123z`（新 amc）| 3.17 GB | 199 991 | 305 300 | ★ **105 308** | 4 | 3 |

**⇒ 明确结论：**mcs 与 amc 的归档**都包含 `tb_live/` 里的文件**（mcs 每腿 **15.6 万–17.5 万** 个），
★ 与 `cap_*` 数量同量级；`logs/` 每腿 4 个文件；目录条目恒为 3 个。**

**(b) ★★ 我错在哪（两次）**

| 错误 | 原因 |
|---|---|
| ① "历史归档只留 `logs/`、`tb_live/` 空壳" | ★ 依据是**手册 §16.2 的核对式**（`members − manifest_files − AppleDouble == 3`）—— 我把那 3 项理解成"空目录条目"，实际是"3 个**目录条目**（其内容另计）" |
| ② "扫 1103i/mcs27 没找到 tb_live" | ★ **扫描窗口只有 3–4 千项**，而 `tb_live` 的文件在成员顺序里靠后 ⇒ 漏掉了 |

**(c) ★★ 正确的归档规则（最终，不再变）**

★ **归档 ＝ 整条腿目录原样**：`cap_*`、`._*`、`tb_live/*`、`logs/*`、`FLIGHT.txt` 等，**不做任何排除**。
★ 手册 §16.2 的核对式仍然成立：`members − manifest_files − AppleDouble == 3`（3 个目录条目）。

**(d) ★ 体积含义**

| 腿 | 归档 | 其中 `tb_live` 贡献（压缩后）|
|---|---|---|
| 典型 mcs/amc | **3.1–4.0 GB** | ★ 约 **0.6–1.0 GB** |
| ★ `s2_amc_ping_1124l`（超长采集）| ★ **23 GB** | ★ `tb_live` 原始 44 GB（其中 `rx_*.f32` 占 88%）|

**⇒ 所以"新旧归档大小接近（3.07 vs 3.17 GB）"完全正常：**两者成员构成几乎相同**（cap 20 万、tb_live 9.7 万 vs 10.5 万）。**

