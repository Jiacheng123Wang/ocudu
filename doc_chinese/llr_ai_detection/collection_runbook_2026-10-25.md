# 采集 runbook — 2026-10-25 起飞

> 目标：补齐语料的**空格**，而不是"再多采一点"。
> 语料的形状与已知空格见 `training_corpus_construction.md` §3.4 与 §9。
> 本文是**飞行操作手册**：每一段做什么、看什么、什么时候停。

---

## 0. 一句话策略

**四段飞行，每段只为一个目的**。每段结束立刻做一次**即时检查**（§5），不合格就
在同一位置补采，而不是飞下一段。

---

## 1. 起飞前：环境变量

每段飞行都设同样五个变量，**只改 `OCUDU_UL_DUMP` 和 `OCUDU_UL_DUMP_COUNT`**：

```bash
export OCUDU_UL_DUMP=$HOME/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus/col_1025_ping  # 换成你要采的那一段的名字
export OCUDU_UL_DUMP_COUNT=8000        # 默认只有 8，必须放大
# OCUDU_UL_DUMP_MIN_RB: 不要设。默认 0 = 任何宽度都收（设成 1 也只是排掉 0 PRB，没必要）
# OCUDU_UL_DUMP_MAX_RB: 可选。只收「不超过这个宽度」的接收，用于专门采窄授权的一段
export OCUDU_UL_DUMP_LLR=1             # 写 LLR，之后算符号一致要用
export OCUDU_LA_LOG_EVERY=1            # 每条调度决策一行（SNR/OLLA/MCS）
```

**段名规范**（后面建集靠它区分来源）。

**目录名用描述性的词，不用字母**——字母在日志和文档里看不出是什么，描述性的名字
一眼就能对上：

| 段 | **目录名** | 目的 |
|---|---|---|
| A | **`col_1025_ping`** | 小包 ping，中低 SNR ⇒ QPSK × 5–8、≤4 |
| B | **`col_1025_deep`** | 更远／加衰减，低 SNR ⇒ <0、0–6 dB 两档 |
| C | **`col_1025_iperf`** | 固定包长 iperf，中距离 ⇒ 64/256QAM × 5–8 |
| D | **`col_1025_attach`** | 近距离反复 attach ⇒ QPSK × 20–60 |

**同一段重复采**（例如 A 段补采一次）就加序号：`col_1025_ping2`、`col_1025_ping3`。
**不要**覆盖或续写已有目录。

> **不要复用同一个目录**：`OCUDU_UL_DUMP` 指向已存在的目录会接着编号，事后无法
> 分辨是哪一段。每段一个新目录。

---

## 2. 四段飞行的具体操作

### A 段：小包 ping（最优先）

**要采到的格子**：`QPSK × ≤4 × 6–10 dB`（当前 **0 条**）、`QPSK × 5–8 × 6–10 dB`、
`16QAM × 5–8 × 10–14 dB`。

| 项 | 做法 |
|---|---|
| 位置 | 从中等距离开始（下行 RSRP 约 −95…−100 dBm） |
| 业务 | `ping -i 0.05 -s 32 <gnb>`（小包、高频） |
| 观察 | gNB 日志里的 `LA UL MCS (effective)`：`pusch_snr` 应在 **6–11 dB** |
| 停止 | 该目录 `.txt` 文件数 ≥ 6 000，或 15 分钟 |
| 判据 | 采完看 §5 的宽度分布：**≤4 PRB 占比 ≥ 10%** |

> 如果 `pusch_snr` 高于 12 dB，**加大距离或加衰减**；低于 5 dB 反而采不到可解码的样本。

### B 段：更深（低 SNR 两档）

**要采到的格子**：`<0 dB`、`0–6 dB`（当前两档合计约 1 600，且几乎全是宽 QPSK）。

| 项 | 做法 |
|---|---|
| 位置 | 继续加距离／加衰减，直到 `pusch_snr` 落在 **0–5 dB** |
| 业务 | 同 A（小包 ping） |
| 停止 | 10–15 分钟，或 `.txt` 数 ≥ 5 000 |
| 判据 | 采完 §5 里 **0–6 dB 档 ≥ 800 条** 且 **可解码率 ≥ 60%** |

> 这一档**可解码率会明显下降**，是正常的：低于门限的接收本来就解不出来。
> 但**解不出来的样本也要留着**——它们是"失败样本"，对分析有用。建集时会按
> CRC 过滤。

### C 段：固定包长 iperf（64/256QAM × 5–8）

**要采到的格子**：`64QAM × 5–8 × 6–10`（62 条）、`256QAM × 5–8 × 6–10`（111 条）。

| 项 | 做法 |
|---|---|
| 位置 | 中等偏近（`pusch_snr` 约 **8–12 dB**） |
| 业务 | `iperf3 -c <gnb> -u -b 2M -l 200`（**固定小包长**，逼出 5–8 PRB 授权） |
| 观察 | `LA UL MCS` 的 `mcs=` 是否落在 64QAM/256QAM 对应的区间 |
| 停止 | 12 分钟，或 `.txt` ≥ 6 000 |
| 判据 | 采完 §5 里 **64QAM/256QAM × 5–8 × 6–10 各 ≥ 300 条** |

> **注意**：MCS 由 AMC 决定，**不要钉 MCS**（钉了会与 PDU 不一致，曾经导致 0 条可解码）。
> 若在 10 dB 上 AMC 始终选 QPSK/16QAM，说明这一格**物理上很难产生**，接受它为空，
> 之后用合成补（§7 of the corpus document）。

### D 段：近距离反复 attach

**要采到的格子**：`QPSK × 20–60 dB`（当前 **15 条**）。

| 项 | 做法 |
|---|---|
| 位置 | **近距**（`pusch_snr` > 20 dB） |
| 业务 | 反复飞行模式开关／重新注册（每次 attach 产生一次随机接入 + RRC 建立） |
| 停止 | 30–40 次 attach，或 10 分钟 |
| 判据 | 采完 §5 里 **QPSK × 20–30 dB ≥ 200 条** |

> attach 期间的授权固定 QPSK、5–8 PRB，与 SNR 无关——这正是这一格存在的原因。

---

## 3. 还可以加的两段（可选，按时间）

### E 段：干扰（如果现场能做）

用第二台设备在同一频段发数据，制造邻道/同频干扰，重复 A 段。语料里**完全没有干扰
样本**，这是最大的未知之一。**优先级低于 A–D**，但如果现场方便，价值很高。

### F 段：移动（如果方便）

手机缓慢移动（步行速度）穿过 3–4 个位置，每处停 30 秒采一段。用于观察**时变信道**
下的表现。现有语料几乎都是静态的。

---

## 4. 每段结束的收尾（30 秒）

```bash
# 1) 数一下采到多少
ls $OCUDU_UL_DUMP/*.txt | grep -vc _ce

# 2) 存一份飞行记录（位置、距离、衰减、观察到的 SINR、时间段）
cat > $OCUDU_UL_DUMP/FLIGHT.txt <<EOF
date=$(date -Iseconds)
segment=<A|B|C|D>
position=<室内/走廊/…>
distance_m=<估算>
attenuator_db=<若使用>
observed_pusch_snr_db=<从 LA 日志读到的范围>
traffic=<ping -s 32 -i 0.05 | iperf3 -u -l 200 | attach>
notes=
EOF
```

**`FLIGHT.txt` 会跟着捕获一起进语料库**，事后分析时能对上。

---

## 5. 即时检查（每段结束跑一次）

```bash
cd ~/dev/ocudu
python3 doc_chinese/llr_ai_detection/work_tmp/probe/quick_census.py \
  doc_chinese/llr_ai_detection/work_tmp/corpus/col_1025_ping  # 换成你要采的那一段的名字
```

它直接读 sidecar，**不需要建集**，输出授权宽度分布、调制分布与码率分布。

### 5.1 原始捕获的快速检查（宽度与 SNR 分布）

```bash
cd ~/dev/ocudu/doc_chinese/llr_ai_detection/work_tmp/corpus/col_1025_ping  # 换成你要采的那一段的名字
# 授权宽度分布（从 sidecar 读，不需要建集）
awk -F= '/^alloc_nof_rb=/{print $2}' *_ce.txt | sort -n | uniq -c | sort -k2 -n | head -20
# 调制分布
awk -F= '/^modulation=/{print $2}' *_ce.txt | sort | uniq -c
```

**判据**（写在这里，避免事后解释）：

| 段 | 通过条件 |
|---|---|
| A | `alloc_nof_rb ≤ 4` 的占比 ≥ 10% |
| B | 该段可解码率 ≥ 60%，且 `0–6 dB` 档 ≥ 800 |
| C | 64QAM 与 256QAM 在 `5–8 PRB` 上各 ≥ 300 |
| D | QPSK 在 `>20 dB` 上 ≥ 200 |

---

## 6. 采完之后我做什么（不需要你操作）

1. `consolidate_captures.py` 把新目录**并入统一存放点**（硬链接，不复制）；
2. `dump_tb.sh --cpu` 建索引（**用 `find`，不用 `ls`**）；
3. `build_depth3_dataset.py` 每种调制建一次（`--min-prb 1`，**窄授权这次要全收**）；
4. `build_e15_from_datasets.py` 三维分层 ⇒ **E16 语料**（7 档 SNR）；
5. 起训练、出三门。

---

## 7. 坑（都踩过）

| 坑 | 症状 | 避免 |
|---|---|---|
| `OCUDU_UL_DUMP_COUNT` 没改 | 每段只有 8 条 | 设 8000 |
| `OCUDU_UL_DUMP_MIN_RB` > 1 | **全是宽授权**，窄格子一条没有 | **不设**（默认 0 = 任意宽度）|
| 复用同一 dump 目录 | 段与段混在一起，无法归因 | 每段新目录 |
| 钉 MCS | 采到的接收**一条都解不出来** | 不要钉，让 AMC 自由 |
| 只看"采了多少条" | 采了一万条却全在同一种格子 | 每段跑 §5 |
| 没有 `FLIGHT.txt` | 事后不知道哪批是什么条件 | 每段写一份 |
| `ls *.txt` 数条数 | 参数过长（>4 万条时） | 用 `find … | wc -l` |

---

## 8. 优先级（如果只能飞一部分）

| 序 | 段 | 理由 |
|---|---|---|
| 1 | **A**（小包 ping，中低 SNR） | `QPSK × ≤4 × 6–10` 当前 **0 条**，是 attach 的最小授权工况 |
| 2 | **D**（近距离 attach） | `QPSK × 20–60` 只有 15 条，且这一格是"过程决定"的、采集就能补 |
| 3 | **B**（更深） | `<0`、`0–6 dB` 两档几乎空白 |
| 4 | **C**（iperf 固定包长） | 64/256QAM 窄授权低 SNR；可能物理上难产生 |
| 5 | E／F | 干扰、移动，价值高但不阻塞 |
