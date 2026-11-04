# 语料采集 → dump → 建库 → 训练数据集：全过程说明书

> **版本**：2026-11-04（广州）｜**范围**：从 gNB 起飞采集一条腿，到产出可训练的数据集，再到训练与评估。
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
