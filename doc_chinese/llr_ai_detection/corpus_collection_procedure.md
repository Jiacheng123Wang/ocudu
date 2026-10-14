# 深度-3 LLR 语料采集操作手册（★ 2026-10-14 第一轮已完成，§3.1 是它的记录）

> **这份文档是干什么的**：★ 把 §19.22 的采集配方变成**照着敲就能跑、跑完能自己判合格与否**的一份操作步骤 ✓。
> **上位文档**：★ `llr_ai_detection_design_and_implementation.md` ★ **§19.22**（配方来源 ✓）、
> **§19.34**（判据工具与旧语料的 0/120 复测 ✓）。
> **一句话**：★★ **这次要采的不是"更多采集"，★ 是"更多【首传就能译出】的宽分配 256QAM 采集"** ✓。

---

## 0. ★★★ 目标与验收（★ 先看这一节，★ 后面都是为它服务的）

| ★ 项 | ★★ 目标 |
|---|---|
| ★★ **判据** | ★★★ **≥ 100 个**满足下面**全部**条件的采集：★ **≥43 PRB** ∧ ★ **256QAM** ∧ ★ **rv=0** ∧ ★★ **在一次接收内译出【非零】TB** ✓ |
| ★★ **判据怎么读** | ★★ `audit_corpus.py --replay` 数出来的**可译个数** ✓ —— ★★★ **不是目录里有多少文件** ✗ |
| ★★ **规模** | ★ 3000 个候选 ⇒ ★★ 期望留下 **≥1000 个包络内首传** ✓（★ 判据要求 100 ✓，★ 留余量 ✓）|
| ★★ **磁盘** | ★★ **约 1 MB / 采集** ✓（★ 旧语料 1956 个 = **1.8 GB** ✓）⇒ ★ 3000 个约 **3 GB** ✓ |

★★★ **为什么判据是"可译"而不是"采到"** ✗：★★ 判据 1 是"网络 vs 经典链在同一批采集上谁译得出" ✓，
★ 译不出的采集**两侧都是 KO**、★ 没有分辨力 ✓；★★ 而**重传**（rv≠0）在实时链上靠 **HARQ 软合并**才译得出 ✓，
★ **离线重放复现不出合并** ✗ ⇒ ★★ **只有首传成功的采集能用** ✓ ⇒ ★★★ **MCS 是唯一杠杆** ✓。

---

## 1. ★★ 为什么这么采（★ 三条，★ 每条对应一个配置改动）

| ★ | ★★ 事实 | ★★ 处置 |
|---|---|---|
| ★★★ | ★ 交付配置 `max_ue_mcs` 默认 **28** = 256QAM 表顶（★ rate **0.948** ✓）⇒ ★★ 旧语料的包络内首传 **731 个，可译 0 个** ✗ | ★★ **把 MCS 钉在 20**（★ 256QAM 表**第一档**，★ rate **0.683** ✓）|
| ★★ | ★ 配方要**高 SNR** ✓，★ 而旧语料实测 **7.8 / 19.8 / 31.9 dB**（★ 中位 **19.8 dB** ✗）| ★★ **`p0_nominal_with_grant: -70`**（★ 默认 −76，★ 抬 6 dB ✓）|
| ★★ | ★ 预算会被**窄分配**与**重传**吃掉 ✗（★ 旧语料 357/1956 窄于 43 PRB ✓）| ★★ **采集侧新增两个过滤器**：★ `OCUDU_UL_DUMP_MIN_RB=43` ✓、★ `OCUDU_UL_DUMP_RV=0` ✓（★ 被拒的接收**不花预算** ✓）|

★★★ **为什么是 20 而不是"上限 19"** ✓：★★ 256QAM 表里 **MCS 0–19 是 QPSK/16QAM/64QAM，★ 20–27 才是 256QAM** ✓
（★ `lib/ran/pdsch/pdsch_mcs.cpp` 的 `MCS_INDEX_TABLE_2` ✓）★ ⇒ ★★ **卡在上限 19 会让语料里一个 256QAM 都没有** ✗，
★ 而 256QAM 是**当前唯一有模型**的调制 ✓。

---

## 2. ★★ 前置准备（★ 逐条确认）

```bash
cd ~/dev/ocudu

# ① ★★★ gNB 必须是【本次构建】—— 新的两个采集过滤器要真的在二进制里
strings build/apps/gnb/gnb | grep -E "OCUDU_UL_DUMP_(MIN_RB|RV)$"
# ★★ 必须打印出这两行；★ 打印不出来说明二进制旧了 ⇒
#    cmake --build build --target ocudu_versioning && cmake --build build --target gnb

# ② ★★ 采集配置在
ls -la configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml
# ★★ 它是交付配置的副本，★ 只改了【三行】（★ 见文件头说明 ✓）：
#    cell_cfg.pusch.min_ue_mcs: 20 / max_ue_mcs: 20 / p0_nominal_with_grant: -70
# ★★ 同时核一眼 AMF 地址（★ `cu_cp.amf.addrs` / `bind_addrs` ✓）：
grep -A2 "^  amf:" configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml
# ★★ 必须与【你交付配置里正在用的那两个地址】一致 ✓（★ 本文件是它的副本 ✓，★
#    但交付配置的地址是【本地未提交】的改动 ⇒ 你若再改交付配置，★ 这里也要跟着改 ✓）

# ③ ★★★ 磁盘【和目录】—— `fopen` 【不会】自动建目录 ✗✗
# ★★ 语料放在【工作流目录里】（★ 与 cap001/cap002 同一处 ✓，★ 已被 .gitignore 忽略 ✓）：
CORPUS=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
df -h "$CORPUS"                 # ★ 每条腿约 2–3 GB；★ 6 条腿约 11 GB
mkdir -p "$CORPUS/<语料目录>"
ls -d "$CORPUS"/*/              # ★ 应当看到 aillr_cap001 aillr_cap002（旧语料）
# ★★ gNB 以 root 跑 ⇒ 采到的文件是 root 的 ✓ ⇒ 顺手确认 root 能写、并记下要 chown ✓：
sudo touch "$CORPUS/.write_test" && sudo rm "$CORPUS/.write_test" && echo "root can write here OK"
# ★★★ 这一步漏了会【静默丢掉整条腿】✗：★ 2026-10-14 的第一条 cap003 腿就是这么丢的 ——
#     ★ 跑了 8 分钟、99672 次接收、★ 目录不存在 ⇒ 3000 个 fopen 全部返回 null ⇒ ★ **一个文件都没写** ✗，
#     而 gNB 当时**什么都没打印** ✗（★ 现已修：见下面 ④ ✓）。

# ④ ★★ 采集代码是编进去的，★ 并且现在会【自己报告】能不能写
grep -m1 ENABLE_UL_CAPTURE build/CMakeCache.txt      # ★ 必须是 ON
strings build/apps/gnb/gnb | grep -c "NOT CAPTURING" # ★ 必须是 1（本次构建新增的保护）
```

★★★ **新增的两条自报（★ 2026-10-14，★ 就是为了上面那次静默丢失 ✓）**：

| ★ 输出 | ★★ 含义 |
|---|---|
| ★★ `[ul_capture] capturing: writing <前缀>_<slot>_<rnti>.* (budget N reception(s) that pass the filters)` | ★★ **第一个采集落地了** ✓ —— ★ 一条腿开头看到这一行，★ 说明目录、权限、前缀都对 ✓ |
| ★★★ `[ul_capture] NOT CAPTURING: cannot write '<前缀>': <原因>` | ★★★ **整条腿正在丢采集** ✗ —— ★★ 立即停腿、★ 建目录、★ 重跑 ✓ |

★★ **另外要准备好**：★ B200 ＋ **GPSDO 锁星**（★ 配置里 `clock_source=gpsdo,time_source=gpsdo` ✓）、
★ 手机已能附着、★ iperf3 可用 ✓。

---

## 3. ★★★ 采集命令（★ 一条，★ 在你原命令上只加两个过滤器 ＋ 换配置）

```bash
cd ~/dev/ocudu

sudo -E env \
  LEG_CONFIG=$HOME/dev/ocudu/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml \
  OCUDU_UL_DUMP=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus/<语料目录>/C \
  OCUDU_UL_DUMP_COUNT=3000 \
  OCUDU_UL_DUMP_MIN_RB=43 \
  OCUDU_UL_DUMP_RV=0 \
  OCUDU_UL_DUMP_LLR=1 \
  OCUDU_RECEIVER_PROBE=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <腿标签>
```

### ★★ 与你原来那条命令的逐项对照

| ★ 环境变量 | ★★ 原来 | ★★★ 现在 | ★★ 为什么 |
|---|---|---|---|
| `LEG_CONFIG` | `gnb_rf_b200_tdd_n78_20mhz.yml` | ★★ **`..._llr_collect.yml`** | ★★ MCS 钉 20 ＋ 功控 −70 ✓ |
| `OCUDU_UL_DUMP` | `.../aillr_cap002/C` | ★ `.../<语料目录>/C` | ★ 新目录，★ 不覆盖旧语料 ✓ |
| `OCUDU_UL_DUMP_COUNT` | 2000 | ★ **3000** | ★ 过滤器会让预算只花在合格接收上 ✓，★ 多留余量 ✓ |
| ★★ **`OCUDU_UL_DUMP_MIN_RB`** | — | ★★ **新增 `43`** | ★★ **窄于 43 PRB 的接收不占预算** ✓（★ 旧语料 357 个白占 ✗）|
| ★★ **`OCUDU_UL_DUMP_RV`** | — | ★★ **新增 `0`** | ★★ **重传不占预算** ✓（★ 重放复现不出合并 ✗，★ 见 §0）|
| `OCUDU_UL_DUMP_LLR` | 1 | ★ 1 | ★ 保留 ✓（★ 软比特对照要用 ✓）|
| `OCUDU_RECEIVER_PROBE` | 1 | ★ 1 | ★ 保留 ✓ |
| ★ 腿模式 / 标签 | `gpu` / `aillr-cap002` | ★ `gpu` / `aillr-cap003` | ★★ 模式保持一致（★ 语料要和交付形态可比 ✓）|

> ★ **注**：★ `LEG_CONFIG` **必须写在 `bash` 之前** ✓（★ 腿脚本只接受 `OCUDU_*=…` 与 `--…` 作为尾参 ✓）；
> ★ 四个 `OCUDU_*` 也可以写成尾参（★ 例如 `… run_leg.sh gpu aillr-cap003 OCUDU_UL_DUMP_RV=0` ✓）★ —— 两种都行 ✓。

---

## 3.1 ★★★ 一次坐下把【该抓的全部抓完】：★ 6 条腿

★★ **为什么一次抓完** ✓：★★ 每条腿都要现场（★ 电台 ＋ 手机 ＋ 人 ✓），★ 而**契约是"一个调制一个模型"** ✓
⇒ ★★ **另外三个调制现在【一个样本都没有】** ✗（★ §④D ✓），★ 补齐它们只需要**换配置再飞一条腿** ✓。

| ★ 腿 | ★★ 语料目录（★ 最终名 ✓）| ★★ 配置文件 | ★★ 调制 / MCS | ★★ 用途 | ★★★ 实测 |
|---|---|---|---|---|---|
| ★★ 1 | `aillr_256qam_wide_s1` | `..._llr_collect.yml` | 256QAM / 20 | ★★ **判据 1 的主池（会话 1）** ✓ | 2770 个，**66.7%** ✓ |
| ★★ 2 | `aillr_256qam_wide_s2` | 同上 | 256QAM / 20 | ★★ **第二场** ⇒ 会话级 train/val（★ 从不随机划分 ✓）| 2640 个，**50.3%** ✓ |
| ★ 3 | `aillr_64qam_wide` | `..._collect_64qam.yml` | 64QAM / 14 | ★ 第二个调制 | 2000 个，**92.1%** ✓ |
| ★ 4 | `aillr_16qam_wide` | `..._collect_16qam.yml` | 16QAM / 9 | ★ 第三个 | 2000 个，**91.7%** ✓ |
| ★ 5 | `aillr_qpsk_wide` | `..._collect_qpsk.yml` | QPSK / 4 | ★ 第四个 | 1997 个，**99.9%** ✓ |
| ★ 6 | `aillr_256qam_narrow` | `..._llr_collect.yml` ＋ `MAX_RB=26` | 256QAM | ★★ 窄分配池 ⇒ 重新裁决 `MIN_PRB` ✓ | 1217 个，**47.4%** ✓ |

> ★★★ **分母是【语料本身】** ✓（★ 前 N 个合格接收 ✓，★ 不是整条腿 ✗）—— ★ 详见设计文档 §19.39 ✓。
> ★★ **上表的数就是工具默认口径**（★ `leg_verdict.py --first` 默认取语料的采集数 ✓）。

★★★ **⇒ 本轮已完成** ✓（★ 2026-10-14 ✓）：★★ **六个组合各一批、★ 全部 KEEP** ✓，
★ 共 **12 624** 个采集、★ **14 GB** ✓。★★ 被重飞取代的三批已删 ✓（★ 设计文档 §19.38 有改名映射 ✓）。
★★ **下一次采集（如果还要）**：★ 直接用下面的 `collect()` 函数 ✓，★ 目录名按 `调制_宽度[_场次]` 起 ✓。

### ★★ 一次性执行（★ 把 6 条腿挨着飞）

```bash
cd ~/dev/ocudu

collect() {                       # 用法: collect <标签> <配置> <目录> [额外的 OCUDU_* 覆盖...]
  local label=$1 cfg=$2 dir=$3; shift 3
  mkdir -p "$dir"                 # ★★ 必须 —— fopen 不会建目录 ✗
  sudo -E env "LEG_CONFIG=$HOME/dev/ocudu/$cfg" \
       "OCUDU_UL_DUMP=$dir/C" OCUDU_UL_DUMP_COUNT=${COUNT:-3000} \
       OCUDU_UL_DUMP_MIN_RB=43 OCUDU_UL_DUMP_RV=0 \
       OCUDU_UL_DUMP_LLR=1 OCUDU_RECEIVER_PROBE=1 "$@" \
       bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu "$label"
}

C=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
collect aillr-256qam-s1 configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml       $C/aillr_256qam_wide_s1
collect aillr-256qam-s2 configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml       $C/aillr_256qam_wide_s2
COUNT=2000 collect aillr-64qam configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_64qam.yml $C/aillr_64qam_wide
COUNT=2000 collect aillr-16qam configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_16qam.yml $C/aillr_16qam_wide
COUNT=2000 collect aillr-qpsk   configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_qpsk.yml  $C/aillr_qpsk_wide
# 窄分配：把下限撤掉、改用上限（★ 后写的赋值覆盖前面的 ✓）
COUNT=2000 collect aillr-narrow configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml $C/aillr_256qam_narrow \
        OCUDU_UL_DUMP_MIN_RB=0 OCUDU_UL_DUMP_MAX_RB=25
```

★★ **每条腿的操作节奏** ✓：★ ① 起腿 ⇒ ★ ② 开 iperf3（★ §4 ✓）⇒ ★ ③ 盯到 **`[ul_capture] capturing:`** 出现 ✓
（★ 没出现就说明目录/权限不对 ⇒ ★ **立刻停腿** ✓，★ 大概念见到 `NOT CAPTURING` ✓）⇒
★ ④ **预算填得很快**（★ 实测 ~310 次接收/秒 ✓，★ 3000 个合格接收通常 **1–3 分钟** 就满 ✓）⇒
★ ⑤ **Ctrl-C 停腿**（★ 按协议：★ 只按一次 Ctrl-C，★ 不要 SIGTERM ✓）⇒ ★ ⑥ 把 iperf3 停掉 ⇒ ★ ⑦ 跑验收（★ §5 ✓）。

★★ **腿与腿之间**：★ 手机重新附着通常几秒 ✓；★★ **每条腿必须换目录** ✓（★ 否则会追加进同一批 ✓）；
★★★ **gNB 以 root 跑 ⇒ 采集文件是 root 的** ✓ ⇒ ★★ 每条腿结束后把属主收回来 ✓（★ 否则以后 `rm -rf` 删语料要 sudo ✓）：

```bash
CORPUS=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
sudo chown -R "$(whoami)":staff "$CORPUS"
```

★★ **不要混用的两件事** ✗：★ **调制腿之间必须换配置文件** ✓（★ MCS 在 YAML 里、★ 不在环境变量里 ✓），
★ 而**采集过滤器在环境里** ✓（★ 所以窄分配那条腿只改 `OCUDU_UL_DUMP_*`、★ 不换配置 ✓）。

---

## 4. ★★ 采集期间：★ 把上行流量打满（★ 决定分配宽度）

★★ **调度器只在【上行缓冲堆积】时给宽分配** ✓ ⇒ ★★ **流量不够，就会退化成窄分配，★ 然后被 `MIN_RB=43` 全滤掉** ✗。

```
# ★★ 在【跑 gNB 的这台机器】上执行；★ `<手机IP>` 是【手机的地址】，★★ 不是 gNB 的 ✗ ——
# ★★ gNB 只是 CN 与手机之间的管道，★ 它【不是】iperf3 的端点 ✓；★ iperf3 server 跑在手机上 ✓。
# ★★ `-R` = reverse ⇒ 【手机发、gNB 收】= 上行 ✓。
iperf3 -c <手机IP> -R -t 300 -P 4
```

★★★ **2026-10-14 更正**：★ 上一版这里写的是 `<gNB_IP>` ✗ —— ★★ 那是错的 ✓（★ 用户指出 ✓）。

★★ **判读（★ 采集中随时看）**：★ 采到的文件应当**持续增长** ✓；★ 若几分钟不涨，★ 说明**当前流量产生不了 ≥43 PRB 的 rv=0 接收** ✗
⇒ ★★ 加大负载（★ `-P 8`、★ UDP `-u -b 0`）★ 或**把手机靠近** ✓。

★★ **建议同时看链自己的测量**（★ 直接读采集目录 ✓）：

```bash
# 最近 20 个采集的 SNR（dB）：_ce.txt 的 snr 列是【线性】值，dB = 10*log10
for f in $(ls -t $HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus/<语料目录>/C_*_ce.txt | head -20); do
  awk '{for(i=1;i<=NF;i++) if($i ~ /^snr=/){split($i,a,"="); printf "%.1f dB\n", 10*log10(a[2])}}' "$f"
done
# ★★ 配方要"高 SNR"：★ 目标【中位 ≥25 dB】✓（★ 旧语料只有 19.8 dB ✗）
```

---

## 5. ★★★ 采后立刻验收（★ **先验证、再建数据集** ✓）

★★★ **顺序不能反** ✗ —— ★★ 旧语料就是**先建了数据集、先训了模型** ✓，★ 才发现**池子里一个可译的都没有** ✗（★ §19.34 ✓）。

```bash
cd ~/dev/ocudu
W=doc_chinese/llr_ai_detection/work_tmp
PY=~/ai_ce_work/venv/bin/python
D=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus/<语料目录>

# ① ★ 秒级：配方维度对不对
$PY $W/probe/audit_corpus.py --corpus $D

# ② ★★★ 分钟级：**真正可译的个数**（★ 这一条才是判据 ✓）
$PY $W/probe/audit_corpus.py --corpus $D --replay \
    --tool ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay --limit 200

# ③ ★★★ 同样分钟级：**离线重放与实时腿逐接收对得上吗**（★ 2026-10-15 新增 ✓）
#    采完必须做这一条 —— 本轮的三个缺陷（★ 码块数 ✗、译码上限 ✗、UCI 没记 ✗）
#    全都是它抓出来的，而"实时译码率"这一条【抓不到】它们 ✓。
$PY $W/probe/replay_vs_live.py --corpus $D --log <腿日志> \
    --tool ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay --count 200
# ★ 期望：★ 对上 90%+ 且【不带 UCI 的接收 100% 一致】✓、
#   ★ 采集时间偏移中位 +1.1…+3.4 ms ✓（★ 这是"采集写在接收开头、日志行在结尾"应有的大小 ✓）。
# ★★ 若带 UCI 的接收大面积不一致 ⇒ ★ 采集侧还没记 UCI（★ 2026-10-15 起会记 ✓）；
#    旧语料用 $PY $W/probe/uci_from_log.py --prepare <新目录> 从腿日志补回来 ✓（★ 实测 187/189 ✓）。
```

### ★★★ 期望输出长什么样

```
<语料目录>: <N> captures
  modulation   : {'256QAM': ...}                       ← ★ 以 256QAM 为主
  rv           : {'0': ...}                            ← ★ 只有 0（过滤器生效）
  code rate    : 256QAM@683(...)                       ← ★★★ 683 = MCS 20；若是 948 ⇒ 配置没生效 ✗
  allocation   : min .. / median 51 / max 51 PRB        ← ★ 中位应当接近 51
  chain SNR    : min .. / median >=25 / max .. dB       ← ★ 配方要高 SNR
  in envelope  : <N> of <N>, of which rv=0: <M>

  classic decoded a non-zero TB on >=100 of 200 ...     ← ★★★ 判据在这里
  READY: ... rv=0 in-envelope captures
```

### ★★ 不合格时怎么办（★ 按现象查）
| ★★ 现象 | ★★ 含义 | ★★ 处置 |
|---|---|---|
| ★ `code rate` 主要是 **948** ✗ | ★ 配置没生效（★ 用了旧 config ✓）| ★ 确认 `LEG_CONFIG` 指向 `..._llr_collect.yml` ✓；★ 看腿日志开头打印的 `CONFIG=` ✓ |
| ★ `allocation` 中位 **< 43** ✗ | ★ 上行负载不够 | ★ 加大 iperf3（★ `-P 8` / UDP）★ 或手机靠近 ✓ |
| ★ `chain SNR` 中位 **< 25 dB** ✗ | ★ 路损大 / 功控没抬 | ★ 手机靠近 ✓；★ 或把 `p0_nominal_with_grant` 再抬到 **−66** ✓ |
| ★★ **可译 < 100 / 200** ✗ | ★★ MCS 20 在这个 SNR 下仍不够 | ★★ **先看 SNR 那一栏** ✓；★ 若 SNR 已经 ≥ 25 dB 仍不可译 ⇒ ★★ **改采 64QAM**（★ `max_ue_mcs: 19` 且不设 `min_ue_mcs` ✓）★ 并**训一个 64QAM 模型** ✓（★ 契约本来就是"一个调制一个模型" ✓）|
| ★ 一个都不译 ✗ | ★ 链本身不健康 | ★★ **先飞一条普通腿**（★ 不带 dump ✓）★ 看 CRC 是否正常 ✓ |

★★ **目录里多出来的东西是正常的** ✓：★ 每个采集 5 个文件（★ `.txt` / `.bin` / `_h.bin` / `_llr.bin` / `_ce.txt` ✓）。

★★ **`_h.bin` / `_llr.bin` 的来源【已解释】** ✓（★ 2026-10-15，★ §19.45(g) ✓，★ 更正 §19.21 的"未解释" ✗）：
★★ **`.txt` / `.bin` / `_h.bin` / `_ce.txt` 都是【每个接收重写】✓，而 `_llr.bin` 是【追加】** ✓ ——
★ 键（★ slot,rnti ✓）会再次出现（★ 计数器回绕 ✓；★ 且 `OCUDU_UL_DUMP_COUNT` 没花完时 `select()` 一直返回真 ✓）
⇒ ★★ 采集的**网格**是**最后一次**合格接收的 ✓，★★ 而 `_llr.bin` 里**躺着好几次**的 ✓
⇒ ★★ **只有最前面那一组记录属于这个采集** ✓（★ 一组 = 一个数据符号 × 数据符号数 ✓）。
★★ 用法：★ 逐接收对照请用 `probe/llr_live_vs_replay.py` ✓（★ 它自己做这个对齐 ✓）。

★★ **`.txt` 的 UCI 字段（★ 2026-10-15 起）** ✓：★ 采集现在写出**整个 UCI 描述** ✓
（★ `nof_csi_part1` ✓、★ `nof_csi_part2_entries` ✓、★ `alpha_scaling` ✓、★ 三个 `beta_offset_*` ✓）。
★★ **为什么必须** ✓：★ 解复用器只有"没有 UCI"时才直通 ✓ ⇒ ★ 带 UCI 的接收（★ 宽腿里 **43–49%** ✓）
★★ 缺这些字段就**译不出来** ✓ —— ★ 这正是 §19.45(f) 里那条被恢复工具救回来的缺口 ✓。

---

## 5.1 ★★★ 可译率低时：★ 先用【实时日志】分清“电源/环境”与“代码问题”（★ 2026-10-14 ✓）

> ★★★ **行动只有一个：★ 重飞** ✓（★ 见 §5.2 的裁定 ✓）。★ 本节只回答“**还值不值得再跑一遍**” ✓。

★★★ **上一轮 16QAM 只译出 0.1%、★ 64QAM 只译出 0.6%** ✗ —— 而**同一条命令**下 QPSK 是 **99.9%** ✓、
★ 256QAM 是 **66.7%** ✓ ⇒ ★★ 值得先分清原因 ✓。★★ 工具已经有了 ✓：

```bash
cd ~/dev/ocudu
W=doc_chinese/llr_ai_detection/work_tmp
PY=~/ai_ce_work/venv/bin/python
L=doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_aillr-cap0NN_1014_HHMM.log

# ① 实时译码率（**独立于离线重放**的另一个口径 ✓）
$PY $W/probe/live_decode_rate.py --log $L --mod 16QAM --rv 0 --min-prb 43
# ② 三个量：★ 接收功率 rsrp、★ SINR、★ 两者之差 = 推算噪声
$PY $W/probe/live_decode_rate.py --log $L --mod 16QAM --rv 0 --min-prb 43 --power
```

★★★ **判读（★ 三类，★ 互斥 ✓）**：

| ★★ 形态 | ★★ 含义 | ★★ 处置 |
|---|---|---|
| ★★ sinr 低 ✗ **且 rsrp 也低** ✓ | ★★ **路损 / UE 功率不足** | ★ 手机靠近 ✓；★ 或抬 `p0_nominal_with_grant` ✓ |
| ★★★ sinr 低 ✗ **但 rsrp 反而【更高】** ✗ | ★★★ **噪声/压缩**（★ 功放或前端过载 ✓，★ 或外部干扰 ✓）| ★★ **重飞** ✓（★ 不测量 ✓） |
| ★★ sinr 够 ✓ **而仍不译** ✗ | ★★ **才轮到怀疑代码** ✗ | ★ 抓一个这样的采集，★ 用 `ul_chain_replay` 单条复现 ✓ |

★★ **每条腿里【宽分配比窄分配低 2–4 dB】** ✓ 是**功率受限 UE 的正常签名** ✓（★ 同样的总功率摊到更多 PRB ✓）
⇒ ★★ **判断配方够不够，要看【宽分配】的 SINR** ✓，★ 不要看窄的 ✓。

★★ **重飞命令（★ 同一配置、★ 换新目录 ✓）**：

```bash
C=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus
COUNT=2000 collect aillr-cap005b configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_16qam.yml $C/aillr_16qam_wide
COUNT=2000 collect aillr-cap006b configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_64qam.yml $C/aillr_64qam_wide
COUNT=3000 collect aillr-cap003b configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml       $C/<语料目录>b
```

★ 附：`..._collect_{16qam,64qam}_p0default.yml` 两个配置仍在 ✓（★ p0 交回 −76 ✓），
★ 但★★ **按裁定它不再是重飞的默认动作** ✗ —— ★ 只在以后真想验证“压缩”假设时才用 ✓。

## 5.2 ★★★ 采集后的判定：★ **一条命令，★ 不合格就重飞**（★ 2026-10-14 用户裁定 ✓）

★★★ **裁定**：★★ 外面的同频干扰**不是我们能控制的** ✓ ⇒
★★ **一旦接收异常，直接重飞手机、重新采集，★ 不做测量** ✓。
★★ ⇒ “这条腿要不要重飞”必须**几秒钟内**、★ **只用腿自己已写下的东西**回答 ✓。

★★★ **判据 = 【实时译码率】本身** ✓（★ 腿日志里的逐接收 CRC ✓，★ 默认 **≥ 30%** ✓）。
★★ **旁边印【后均衡噪声底】** ✓（★ `rsrp − sinr` ✓）**作解释** ✓。

> ★★★ **2026-10-14 更正** ✗：★ 本节的第一版用的是 `_ce.txt` 里的**噪声方差** ✓，
> ★★ 而它是**估计器的残差** ✓ —— ★★ 主项是**内插误差**、★ 随**信号电平**走 ✗，
> ★★ **不是信道的噪声** ✗。★ 它在前六条腿上“分得很干净”是**虚假相关** ✗
> （★ 坏腿恰好嗓门更大 ✓）。★★ **第七条腿（cap005b）就把它推翻了** ✗：
> ★ 它译出 **76.9%** ✓✓（★ 目前最好的 16QAM 腿 ✓）而旧门限判它重飞 ✗。

```bash
W=doc_chinese/llr_ai_detection/work_tmp
PY=~/ai_ce_work/venv/bin/python
L=doc_chinese/phy_pipeline_gpu/wip/logs

# ★★ 每条腿跑完立刻跑这一条（★ 几秒钟 ✓）
$PY $W/probe/leg_verdict.py --corpus $W/corpus/<语料目录> \
    --log "$L/gnb_gpu_aillr-<腿标签>_*.log" --mod 256QAM --rv 0 --min-prb 43
# ★★ 输出：★ 实时译码率（判据 ✓）、★ 后均衡噪声底（解释 ✓）、
#    ★ `_ce.txt` 残差（仅上下文，★ 工具会标注它**不跟踪链路** ✗）
# ★★ ⇒ **KEEP** ✓ 或 **RE-FLY** ✗（★ 退出码 0 / 2 ✓）；
#    ★ 若某个配置合理地就该低于 30%，★ 用 `--min-rate` 改 ✓（★ 它本来就是逐配置的 ✓）。
```

★★★ **七条腿的实测判决（★ 修正后 ✓）**：

| ★ 语料 | ★★ 译码率 | ★★ 后均衡噪声底 | ★★ 判决 |
|---|---|---|---|
| <语料目录> | 10.3% | −12.7 | ★ **RE-FLY** ✗ |
| aillr_256qam_wide_s1 | 67.5% | −22.0 | ★★ **KEEP** ✓ |
| <语料目录>| 0.6% | −2.9 | ★ **RE-FLY** ✗ |
| ★★ **aillr_16qam_wide** | ★★★ **76.9%** ✓✓ | −15.5 | ★★ **KEEP** ✓ |
| <语料目录>| 0.1% | −0.9 | ★ **RE-FLY** ✗ |
| aillr_qpsk_wide | 99.9% | −17.4 | ★★ **KEEP** ✓ |
| aillr_256qam_narrow | 33.8% | −5.1 | ★★ **KEEP** ✓ |

★★★ **RE-FLY 时怎么做** ✓：★ **换一个新目录重飞同一条配置** ✓
—— ★★ **不要删旧的** ✗：★ 它仍可作**训练数据源** ✓，★ 只是**不能当判据池** ✗。

★★ **采集循环（固定 ✓）**：★ **起腿 ⇒ 采集 ⇒ `leg_verdict.py` ⇒ KEEP 进下一腿 / RE-FLY 换目录重飞** ✓。

### ★ 附：若以后想知道干扰长什么样（★ 不在本工作流里 ✗）

★★ 工具仍在：`probe/spectrum_floor.py` ✓（★ 读 `uhd_rx_cfile` 的原始 IQ ✓，
★ `--series` 每秒一个字符打时间序列 ✓）。★★ 一次 5 s 的抓包已经说明了它的局限 ✗：
★ 底噪平（★ 跨度 1.4 dB ✓）而坏腿在 30–60 分钟之前 ✗ —— ★★ **要看就要看 10 分钟** ✓。
★ ★ ★ **但按裁定不再做这件事** ✓。

---

## 6. ★★ 验收通过之后（★ 这些我来做 ✓）

1. ★★ **自标签**：`work_tmp/probe/self_label_captures.py` ✓（★ 用链自己译出的**非零 TB** 当标签 ✓，★ 实测可用 ✓）
2. ★★ **建数据集**：`build_depth3_dataset.py --source real` ✓
3. ★★★ **判据 1 的第一次 ≥100 样本读数**：`compare_ai_arm.py` ✓ —— ★★ 这是整个工作流一直在等的那个数 ✓
4. ★★ **重训头 3/4**（★ §19.33 的预登记门槛：★ RSRP ±1 dB、★ 噪声 ±1.5 dB、★ SNR ±2 dB ✓）

★★ **把这条也带上**：★ 新语料同样可以用来**重训主头** ✓ —— ★★ 现在的训练集是**合成的**（★ 从每个采集的导频拟合 TDL 重建 ✓），
★ 而 §19.16 早就量过：★★ **合成验证集不能预测链级性能** ✗ ⇒ ★★ 真实网格 ＋ 自标签是这个工作流第一次拿到的**同分布真数据** ✓✓。

---

## 7. ★★ 一句话总结

★★★ **换配置（MCS 20 ＋ p0 −70）＋ 加两个过滤器（MIN_RB=43 / RV=0）＋ 流量打满** ✓，
★★★ **采完先跑 `audit_corpus.py --replay`，可译 ≥100 再往下走** ✓ —— ★★ **不要先建数据集** ✗。

---

## 8. ★★★ 事故记录：★ 第一条 cap003 腿**一个文件都没写**（★ 2026-10-14）

★★ **事实**（★ 从腿日志读出来的 ✓）：★ 腿 `gnb_gpu_aillr-cap003_1014_2146` 跑了 **8 分钟、99672 次接收** ✓，
★ 九个环境变量**全部正确送达** ✓（★ 腿日志开头逐条打印 ✓），★★ **而语料目录（★ 当时在 `$HOME/ocudu_capture/` ✗）不存在** ✗ ——
★★★ **`fopen` 不会创建目录** ✓ ⇒ ★★ **3000 个采集的每一次 fopen 都返回 null，★ 而 gNB 什么都没打印** ✗。

★★ **为什么没被发现**：★ 腿本身**是健康的** ✓（★ 契约 8/9 通过 ✓、★ 计数器全绿 ✓、★ 速率正常 ✓）——
★★ 唯一的症状是"**没有文件**" ✓，★ 而当时没有任何东西会报告它 ✗。

★★★ **⇒ 处置（★ 已落地 ✓）**：
1. ★★ **`ul_capture` 现在自报** ✓：★ 第一个采集落地时打印 `[ul_capture] capturing: writing …` ✓；
★★ 目录不存在或 `fopen` 失败时打印 **`[ul_capture] NOT CAPTURING: … Every capture of this leg is being DROPPED`** ✓
（★ **stderr、★ 不受日志级别影响** ✓ —— ★ 与 F20 同一条理由：★ "语料在不在"不能取决于日志级别 ✓）；
2. ★★ **目录检查在花预算【之前】** ✓（★ 一个被拒的接收不占预算 ✓，★ 与两个宽度过滤器同一处置 ✓）；
3. ★★ 手册把 `mkdir -p` 提到前置检查里 ✓，★ 并写明"**看不到 `capturing:` 就立刻停腿**" ✓。

★★★ **⇒ 纪律 F34**：★★★ **采集器必须【自报它在写】** ✓ —— ★★ 一个"什么都没做"的仪器，
★ 与一个"做了但结果为空"的仪器，★ **在输出上无法区分** ✗ ⇒ ★★ **必须由仪器自己说出前者** ✓。
★★ 本条与 **F20**（降级必须走不受日志级别影响的通道 ✓）是同一族：★ **沉默的失败比失败的失败更贵** ✓。
