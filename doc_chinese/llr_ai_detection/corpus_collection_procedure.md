# 深度-3 LLR 语料采集操作手册（★ `aillr_cap003`，2026-10-14）

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

# ③ ★★ 磁盘
df -h ~/ocudu_capture            # ★ 需要 ≥3 GB 余量
mkdir -p ~/ocudu_capture/aillr_cap003

# ④ ★★ 采集代码是编进去的
grep -m1 ENABLE_UL_CAPTURE build/CMakeCache.txt      # ★ 必须是 ON
```

★★ **另外要准备好**：★ B200 ＋ **GPSDO 锁星**（★ 配置里 `clock_source=gpsdo,time_source=gpsdo` ✓）、
★ 手机已能附着、★ iperf3 可用 ✓。

---

## 3. ★★★ 采集命令（★ 一条，★ 在你原命令上只加两个过滤器 ＋ 换配置）

```bash
cd ~/dev/ocudu

sudo -E env \
  LEG_CONFIG=$HOME/dev/ocudu/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect.yml \
  OCUDU_UL_DUMP=$HOME/ocudu_capture/aillr_cap003/C \
  OCUDU_UL_DUMP_COUNT=3000 \
  OCUDU_UL_DUMP_MIN_RB=43 \
  OCUDU_UL_DUMP_RV=0 \
  OCUDU_UL_DUMP_LLR=1 \
  OCUDU_RECEIVER_PROBE=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu aillr-cap003
```

### ★★ 与你原来那条命令的逐项对照

| ★ 环境变量 | ★★ 原来 | ★★★ 现在 | ★★ 为什么 |
|---|---|---|---|
| `LEG_CONFIG` | `gnb_rf_b200_tdd_n78_20mhz.yml` | ★★ **`..._llr_collect.yml`** | ★★ MCS 钉 20 ＋ 功控 −70 ✓ |
| `OCUDU_UL_DUMP` | `.../aillr_cap002/C` | ★ `.../aillr_cap003/C` | ★ 新目录，★ 不覆盖旧语料 ✓ |
| `OCUDU_UL_DUMP_COUNT` | 2000 | ★ **3000** | ★ 过滤器会让预算只花在合格接收上 ✓，★ 多留余量 ✓ |
| ★★ **`OCUDU_UL_DUMP_MIN_RB`** | — | ★★ **新增 `43`** | ★★ **窄于 43 PRB 的接收不占预算** ✓（★ 旧语料 357 个白占 ✗）|
| ★★ **`OCUDU_UL_DUMP_RV`** | — | ★★ **新增 `0`** | ★★ **重传不占预算** ✓（★ 重放复现不出合并 ✗，★ 见 §0）|
| `OCUDU_UL_DUMP_LLR` | 1 | ★ 1 | ★ 保留 ✓（★ 软比特对照要用 ✓）|
| `OCUDU_RECEIVER_PROBE` | 1 | ★ 1 | ★ 保留 ✓ |
| ★ 腿模式 / 标签 | `gpu` / `aillr-cap002` | ★ `gpu` / `aillr-cap003` | ★★ 模式保持一致（★ 语料要和交付形态可比 ✓）|

> ★ **注**：★ `LEG_CONFIG` **必须写在 `bash` 之前** ✓（★ 腿脚本只接受 `OCUDU_*=…` 与 `--…` 作为尾参 ✓）；
> ★ 四个 `OCUDU_*` 也可以写成尾参（★ 例如 `… run_leg.sh gpu aillr-cap003 OCUDU_UL_DUMP_RV=0` ✓）★ —— 两种都行 ✓。

---

## 4. ★★ 采集期间：★ 把上行流量打满（★ 决定分配宽度）

★★ **调度器只在【上行缓冲堆积】时给宽分配** ✓ ⇒ ★★ **流量不够，就会退化成窄分配，★ 然后被 `MIN_RB=43` 全滤掉** ✗。

```
# 手机侧（gNB 的 IP 按你的部署；-R = 手机发、gNB 收）
iperf3 -c <gNB_IP> -R -t 300 -P 4
```

★★ **判读（★ 采集中随时看）**：★ 采到的文件应当**持续增长** ✓；★ 若几分钟不涨，★ 说明**当前流量产生不了 ≥43 PRB 的 rv=0 接收** ✗
⇒ ★★ 加大负载（★ `-P 8`、★ UDP `-u -b 0`）★ 或**把手机靠近** ✓。

★★ **建议同时看链自己的测量**（★ 直接读采集目录 ✓）：

```bash
# 最近 20 个采集的 SNR（dB）：_ce.txt 的 snr 列是【线性】值，dB = 10*log10
for f in $(ls -t ~/ocudu_capture/aillr_cap003/C_*_ce.txt | head -20); do
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
D=$HOME/ocudu_capture/aillr_cap003

# ① ★ 秒级：配方维度对不对
$PY $W/probe/audit_corpus.py --corpus $D

# ② ★★★ 分钟级：**真正可译的个数**（★ 这一条才是判据 ✓）
$PY $W/probe/audit_corpus.py --corpus $D --replay \
    --tool ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay --limit 200
```

### ★★★ 期望输出长什么样

```
aillr_cap003: <N> captures
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

★★ **目录里多出来的东西是正常的** ✓：★ 每个采集 5 个文件（★ `.txt` / `.bin` / `_h.bin` / `_llr.bin` / `_ce.txt` ✓）；
★★ **`_h.bin` / `_llr.bin` 的来源至今未解释** ✗（★ §19.21 ✓）★ ⇒ ★★ **不要拿它们当保真度参照** ✓。

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
