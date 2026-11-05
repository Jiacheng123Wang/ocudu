# 门控链路测量 runbook：在固定尺子上量交付比特

> **用途**：从一个 `.keras` 模型出发，产出"它比经典链好还是差、差多少、在哪些格子上"的**可复现**结论。
> **口径固定**：尺子 `ruler_s2_amc_5000`（5 000 个未按 CRC 过滤的接收）、指标**交付比特**、统计**配对**。
> **本文是操作的唯一入口**；方法论与事故史见 `depth3_llr_ai/corpus_to_training_db_manual.md` §10（指针）、
> `depth3_llr_ai/depth3_gate_and_model_pk.md`（结果与逐节分析）、`depth3_llr_ai/depth3_llr_ai_design_and_implementation.md` §16–§17（为什么这么量）。
> **实测日期**：2026-11-04（广州，回放机 203 ＝ `192.168.0.228`）。

---

## 0. 一分钟版

```bash
# ① 恢复尺子并校验（10 000 个文件必须全 OK）
cd ~/dev/ocudu/lib/phy/upper/ai_signal_processors/test
cat ruler_s2_amc_5000.tar.gz.part* > /tmp/ruler.tar.gz && tar -xzf /tmp/ruler.tar.gz -C /tmp
mkdir -p /tmp/rv && ln -sfn "$PWD/ruler_s2_amc_5000_corpus" /tmp/rv/corpus
(cd /tmp/rv && shasum -a 256 -c "$OLDPWD/ruler_s2_amc_5000_SHA256SUMS")   # 10 000 × OK

# ② 模型 → Core ML（在 Apple Silicon 上做；x86 与 Linux 做不了）
train_depth3.py --export-only <模型>.keras --export-probe /tmp/m.mlpackage \
  --modcond --layout spatial --modulation 256QAM --c-hid <32|128> --blocks 18 --snr-range 0,48
xcrun coremlcompiler compile /tmp/m.mlpackage <bundle 目录>/
calibrate_depth3.py --model <模型>.keras --data <与模型同分布的 monitor>.npz \
  --cond --layout spatial --samples 60 --write-scales <bundle 目录>

# ③ 跑一条臂（回放机；串行、PURE、每单元一进程）
python3 run_arm_ext.py --arm ai --bundle <bundle>.mlmodelc --units ruler_s2_amc_5000_units.txt \
  --corpus /tmp/rv/corpus --out <臂>.csv --logs logs_<臂> --tmp tmp_<臂> --warmup 0 --guard 1

# ④ 算分（在开发机上）
python3 analyze_e17_1104.py     # 模板；换 ARMS 列表即可
```

---

## 1. 尺子：`ruler_s2_amc_5000`

### 1.1 它是什么

| 项 | 值 |
|---|---|
| 内容 | **5 000 个接收单元**，每个两份文件：`<unit>.txt`（PDU 侧车）＋ `<unit>.bin`（接收网格）|
| 来源 | 腿 `s2_amc_ping_1102a`（2026-11-02 采集，50 000 个捕获），**均匀随机抽 5 000、seed 20261102、不按 CRC 过滤** |
| 构成 | 约 **69% 经典链能解、31% 不能**；256QAM 约占 38% |
| 归档位置 | **`lib/phy/upper/ai_signal_processors/test/`**（仓库内）|
| 归档内容 | `ruler_s2_amc_5000_units.txt`、`_index.csv`、`_clean_cpu.csv`、`_clean_ai.csv`、`_corpus/`（10 000 文件）、`_SHA256SUMS`（10 000 条）、分片 tar（`part00` 90 MB ＋ `part01` 69 MB）、`materialise.sh`、`HOW_TO_RESTORE.md` |

### 1.2 为什么只有 `.txt` 与 `.bin`

`ul_chain_replay <base>` 只打开 `<base>.txt` 与 `<base>.bin`。采集目录里另有 `_h.bin`（逐符号信道）与 `_ce.txt`（估计器标量），
那是**分析的产物、不是重放的输入**。官方测试集也是每单元只有这两份。

### 1.3 怎么还原（两条路，任选其一）

```bash
cd ~/dev/ocudu/lib/phy/upper/ai_signal_processors/test

# 方式一（推荐，不依赖语料）：从分片 tar 还原
cat ruler_s2_amc_5000.tar.gz.part* > /tmp/ruler.tar.gz
shasum -a 256 /tmp/ruler.tar.gz        # 期望 2431dc6dea9d1e8778b5fa93…
tar -xzf /tmp/ruler.tar.gz -C /tmp     # → /tmp/ruler_s2_amc_5000/corpus/（10 000 文件）

# 方式二（需要原始捕获还在）：从语料重建
./materialise.sh /Volumes/T7_Corpus/corpus/s2_amc_ping_1102a /tmp/ruler_s2_amc_5000/corpus
```

### 1.4 校验（**必须做，10 000 条全 OK 才算对**）

`SHA256SUMS` 里的路径是 `corpus/<unit>.<ext>`，所以要在**含 `corpus/` 的目录**里校验。归档里解压出来的目录叫
`ruler_s2_amc_5000_corpus/`（被 `.gitignore` 排除），因此用一个临时目录做软链即可：

```bash
mkdir -p /tmp/rv && ln -sfn <解压出的 corpus 目录> /tmp/rv/corpus
(cd /tmp/rv && shasum -a 256 -c <仓库>/ruler_s2_amc_5000_SHA256SUMS)
# 实测（2026-11-04）：10 000 条全部 OK，耗时 3.7 s
```

### 1.5 ★ 本次核实：我们历次回放用的就是这把尺子

| 文件 | sha256 前 24 位 | 结论 |
|---|---|---|
| 归档 `ruler_s2_amc_5000_units.txt` | `47dcad0d742d0d55ff67358f` | ★ **与每次回放日志里记的 `UNITS_SHA` 完全相同** |
| 归档 `_clean_cpu.csv` | `17300b0b67e3155057b453f5` | ★ 与 `exp_bits_1102/clean_cpu.csv` 相同 |
| 归档 `_clean_ai.csv` | `5a2ed5bf7cb5163f6087476c` | ★ 与 `exp_bits_1102/clean_ai.csv` 相同 |

**⇒ 所以"旧的临时副本"和这把归档尺子是同一批数据** ✓。但从现在起**只认归档**：回放前按 §1.4 校验，
回放用 `--corpus /tmp/rv/corpus`（或任何通过校验的副本），并在日志里记 `UNITS_SHA`。

---

## 2. 口径：交付比特，不是块数

```
交付比特 = Σ_{crc=OK 的块} (tbs_bytes × 8)
块率     = crc=OK 的块数 / 5 000            ← 只作对照，不要用它下结论
```

**为什么不用块数**：块的大小差三个数量级。在本尺子里，**≥500 字节的块只占块数的 33.8%，却贡献 54.4% 的比特** ——
按块计数会系统性低估大块（大块正是高阶调制与宽分配的产物）。

**为什么要不按 CRC 过滤**：旧口径用的是 dump 的 `crc=OK` 子集，那是**经典链**跑出来的 ⇒ 经典链在它上面 100% 是**构造使然**，
不是测量。这把尺子对整个 50 000 总体抽样，两条臂看同一个无偏样本。

---

## 3. 四条硬要求（缺一条，结论就不成立）

| # | 要求 | 为什么 |
|---|---|---|
| **1** | **`OCUDU_DEPTH3_FALLBACK=none`（PURE）** | 否则网络拒收/放弃的单元会被经典链接手，两条臂不再独立 |
| **2** | **`depth3_scales.txt` 与 bundle 同目录** | 缺了会静默退回默认标定，比较变成"模型 vs 默认标定" |
| **3** | **每单元一进程、串行** | 两个 replay 并发会互相覆盖信道数据（实测出现 −36 dB 的 SINR，结论符号翻转）|
| **4** | **守卫：每单元前查有没有第二个 replay** | 同上；而且**守卫必须先被看到真的触发过**再用 |

`run_arm_ext.py --guard 1` 实现了 1/3/4；`OCUDU_DEPTH3_FALLBACK=none` 由脚本设置。**2** 靠你把 scales 放对位置。

---

## 4. 准备一台回放机

| 需要 | 说明 |
|---|---|
| `ul_chain_replay` | Metal 构建：`build/lib/phy/upper/channel_processors/metal/ul_chain_replay`（★ 记 sha256）|
| 尺子 | 按 §1.3 还原、§1.4 校验过的 `corpus/` 与 `units.txt` |
| `run_arm_ext.py` | 从任意一次测量的目录里拷（它在仓库外，属于测量工具）|
| bundle ＋ scales | 见 §5 |
| 磁盘 | 每臂的 CSV 约 1 MB；临时目录 `tmp_*` 会写每单元的中间文件（可清）|

**机器**：2026-11-04 用的是 **203（M2，广州 `192.168.0.228`）**；香港时期同机在 `192.168.31.203`。

---

## 5. 把模型变成 `.mlmodelc`

**必须在 Apple Silicon 的 macOS 上做**（x86 与 Linux 没有 `coremlcompiler`）。

```bash
cd ~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train
PY=~/ai_ce_work/venv/bin/python

# ① 导出 fp16 部署图（--export-only 从 .keras 读架构，所以只给与架构有关的开关）
$PY train_depth3.py --export-only <模型>.keras --export-probe /tmp/m.mlpackage \
    --modcond --layout spatial --modulation 256QAM --c-hid <32|128> --blocks 18 --snr-range 0,48
# 期望输出：wrote the fp16 deployment graph to /tmp/m.mlpackage (input [1, 612, 14, 16], depth3_nsc=612)

# ② 编译
xcrun coremlcompiler compile /tmp/m.mlpackage <bundle 目录>/

# ③ 标定（与模型同分布的 monitor 最好；60 个样本够）
$PY calibrate_depth3.py --model <模型>.keras --data <monitor>.npz \
    --cond --layout spatial --samples 60 --write-scales <bundle 目录>
# 期望：chosen S_llr = 20（平台期上）、写出 depth3_scales.txt（s_llr / a_noise / b_noise）
```

**要记下来的声明**（`metadata.json`）：`depth3_layout`、`depth3_qm_set=2,4,6,8`、`depth3_head_stride`、
`depth3_nsc=612`、`depth3_snr_range`，以及输入/输出形状。

> ★ 标定用的 monitor 应当与**模型的训练分布**一致。2026-11-04 那两条 c128（256QAM 语料）用的是
> `e17_monitor.npz`，`s_llr` 仍落在 **20**（与其他臂相同）⇒ 结论对 sidecar 不敏感。

---

## 6. 跑一条臂（AI）

```bash
cd <测量目录>          # 里面放 units.txt、run_arm_ext.py、bundle 的路径由 --bundle 给
python3 run_arm_ext.py --arm ai \
  --bundle   <bundle>.mlmodelc \
  --units    ruler_s2_amc_5000_units.txt \
  --corpus   <通过校验的 corpus 目录> \
  --out      <臂>.csv --logs logs_<臂> --tmp tmp_<臂> \
  --warmup 0 --guard 1
```

**CSV 字段**：`unit,arm,rc,wall_s,status,crc,tbs_bytes,tbs_bits,result_line,arm_line,timeout`。
**成功判据**：`status=OK` 且 `crc=OK`；`rc≠0` 的行代表该单元重放失败（不是"解码失败"）。

**开跑前把环境写进日志**（这是可复现的唯一凭据）：

```bash
{
  echo "START $(date '+%F %T %Z')"; echo "HOST $(hostname)"
  echo "UNITS_SHA $(shasum -a 256 units.txt | awk '{print $1}')  count $(grep -c . units.txt)"
  echo "BIN_SHA $(shasum -a 256 "$BIN" | awk '{print $1}')"
  echo "BUNDLE $BUNDLE"
  echo "BUNDLE_FILES $(cd "$BUNDLE" && find . -type f | sort | xargs shasum -a 256 | shasum -a 256 | awk '{print $1}')"
  echo "SIDECAR $(cat "$BUNDLE"/../depth3_scales.txt | tr '\n' ' ')"
  echo "OTHER_REPLAYS $(ps -axo command | grep -F "$BIN" | grep -v grep | wc -l)"
} > run_<臂>.log 2>&1
```

**耗时**：5 000 单元 ≈ **23–25 分钟**（中位 0.27 s/单元，串行）。

---

## 7. 跑经典链（参照）

```bash
ul_chain_replay <unit> --out /tmp/c --metal --phy_pipeline gpu --pusch_receiver_backend classic
```

**已有 `clean_cpu.csv` 时不要重跑**：它就是这把尺子的经典链参照答案（sha256 `17300b0b…`），
只有当你改了二进制、改了 `corpus`、或要验证它时才重跑。

> ★ **二进制哈希要一致**：不同构建之间可能差在"1-PRB 单元算不算"这种地方。2026-11-04 用的
> `ul_chain_replay` sha256 是 `d785f039a2f8bab0…`（会算 5 个 1-PRB 单元）。**换二进制就要在报告里注明**。

---

## 8. 算分

### 8.1 汇总表

| 列 | 算法 |
|---|---|
| 交付比特 | `Σ tbs_bits`（`status=OK` 的行）|
| 块率 | `count(status=OK) / 5000` |
| bits/单元 | 交付比特 / 5 000 |
| bits/s | 交付比特 / `Σ wall_s`（**只作同批次相对值**）|
| 中位耗时 | `median(wall_s)` |

### 8.2 置信区间与配对检验

```python
# 有放回重采样：每遍"抽 5 000 次"（不是"取那 5 000 个"），对每遍重算总差，取 2.5/97.5 分位
rng = random.Random(20261102)
d = [bits[a][u] - bits[b][u] for u in units]
reps = sorted(sum(d[i] for i in rng.choices(range(len(d)), k=len(d))) for _ in range(2000))
ci = (reps[50], reps[1949])

# McNemar（连续性校正、双侧）：只用判决不一致的单元
x = #{a 解出、b 没解出}; y = #{b 解出、a 没解出}
z = (abs(x - y) - 1) / sqrt(x + y); p = erfc(abs(z)/sqrt(2))
```

**读法**：CI 跨 0 且 p 大 ⇒ **"没有证据说两条臂不同"**，不是"证明相等"（本尺子的分辨率约 ±2.8%）。

### 8.3 分解

按 **调制**（QPSK/16QAM/64QAM/256QAM）与**宽度**（`<=3`、`<=8`、`<=24`、`>24` PRB）各算一次交付比特差值 ——
这是唯一能指出"强在哪、弱在哪"的视角。尺子的单元表里带 `mod` 与 `alloc_nof_rb`。

**模板**：`~/ai_depth3/exp_bits_1102/analyze_e17_1104.py`（换 `ARMS` 列表即可复用；含 bootstrap、McNemar、按调制分解）。

---

## 9. 归档：一次测量要留下什么

| 产物 | 位置（2026-11-04 的做法）|
|---|---|
| 逐单元 CSV | `~/ai_depth3/exp_bits_1102/<臂>.csv`（开发机）＋ 回放机 `<测量目录>/<臂>.csv` |
| 运行日志（含 UNITS_SHA/BIN_SHA/BUNDLE_FILES/SIDECAR）| 回放机 `<测量目录>/run_<臂>.log` |
| 分析报告 | `~/ai_depth3/exp_bits_1102/report_<批次>.md` |
| 模型与 bundle | `~/ai_depth3/gate_ai_models/`（Keras 与 `.mlmodelc` 分开放，见该目录 `MANIFEST.md`）|
| 结论 | `doc_chinese/depth3_llr_ai/depth3_gate_and_model_pk.md`（追加成节）|

**一个测量"可复现"的最低要求**：能拿 `UNITS_SHA` 找回尺子、拿 `BIN_SHA` 找回二进制、拿 `BUNDLE_FILES` 找回模型、
拿 `SIDECAR` 找回标定。

---

## 10. 已做过的测量（**示例**；权威结果不在这里）

> ★★★ **本文件只管"怎么量"，不记 PK 结果。** 下面这张表是**本批（2026-11-04）留下的例子**，
> 用来说明"结果长什么样、该看哪几列"；**新增的臂不再往这里加** ——
> **所有 PK 结果只进 `doc_chinese/depth3_llr_ai/depth3_gate_and_model_pk.md`** ✓



| 臂 | 模型 | 交付比特 | vs CPU | 结论 | 出处 |
|---|---|---|---|---|---|
| 经典链 | — | **8 933 800** | — | 参照 | §37 |
| E4 | c32/18 | 8 844 864 | −1.00%（CI 含 0，p=0.108）| 与 CPU 打平 | §37 |
| E17 | c32/18 | 8 947 360 | +0.15%（CI 含 0，p=0.376）| 与 CPU 打平 | §40 |
| W2-cond | c32 条件化（两段）| 7 245 496 | **−18.90%** | 明确落后 | §39–§40 |
| W2-c128（第 6 轮）| c128 条件化（两段）| **9 642 152** | **+7.93%** | 唯一超过经典链 | §43 |
| W2-c128（第 9 轮）| 同上 | 9 377 648 | +4.97% | 过拟合，比第 6 轮差 | §43 |
| 60 轮 c32（第 13 轮）| c32 条件化（一路到底）| 8 190 840 | −8.32% | 差距全在 256QAM×窄带 | §45 |
| c128_e17 | c128（99 097 样本）| 8 603 720 | **−3.69%** | 不如经典链 | 本文件 §11 |
| **c128_e17_40k** | c128（43 523 样本）| ★ **10 224 160** | ★★ **+14.44%** | ★★ **目前最强臂** | 本文件 §11 |

---

## 11. 示例：2026-11-04 的一批（两条广州模型）

> ★★ 本节是**完整走一遍的示例**（模型 → bundle → 标定 → 回放 → 算分 → 归档）。
> **它的数字已归档在 PK 文档 §46**；此处保留，是为了让照着做的人看到每一步的产物长什么样。

**模型**：广州两台各训完一条 c128（256QAM 语料，99 097 vs 43 523 样本；`--modcond --layout spatial --c-hid 128 --blocks 18`）。
**存放**：`~/ai_depth3/gate_ai_models/keras/c128_e17.keras`、`…/c128_e17_40k.keras`（详见该目录 `MANIFEST.md`）。

| 项 | c128_e17 | c128_e17_40k |
|---|---|---|
| 训练机 | 221（i7-9750H）| 106（Xeon 8C/16T）|
| best epoch | 5（val 0.08853 / BER 2.855%）| 9（val 0.08587 / BER 2.816%）|
| 导出 | `--export-only … --modcond --layout spatial --c-hid 128 --blocks 18 --snr-range 0,48` → 输入 `[1,612,14,16]` ✓ | 同左 |
| 标定 | `s_llr=20 / a_noise=4 / b_noise=-1`（e17_monitor，60 样本）| 同左 |
| 回放 | 203（`192.168.0.228`）、`BIN_SHA d785f039…`、串行、guard=1 | 同左 |
| 起止 | 2026-11-04 21:45:22 → 22:08:39 | 22:08:39 → 约 22:32 |

**结果**（同一把尺子、5 000 单元、串行、PURE、`BIN_SHA d785f039…`；CSV 在 `~/ai_depth3/exp_bits_1102/`）：

| 臂 | 块 | 块率 | 交付比特 | vs CPU | bits/单元 | 中位耗时 |
|---|---|---|---|---|---|---|
| CPU 经典链 | 3 465 | 69.30% | 8 933 800 | — | 1 786.8 | 210 ms |
| `c128_e17`（99 097）| 3 375 | 67.50% | 8 603 720 | **−3.69%** | 1 720.7 | 242 ms |
| ★ `c128_e17_40k`（43 523）| 3 802 | **76.04%** | ★ **10 224 160** | ★ **+14.44%** | 2 044.8 | 270 ms |

- `c128_e17_40k − CPU`：**+1 290 360**，CI **[+1 130 360, +1 457 544]**，McNemar **409 : 72**，p = **5.6×10⁻⁵³**
- `c128_e17 − CPU`：−330 080，CI [−486 680, −161 080]，200 : 290，p = 5.8×10⁻⁵
- `c128_e17_40k − W2-c128(第 6 轮)`：**+582 008**，CI [+471 584, +690 456]，310 : 52，p = 1.4×10⁻⁴¹

**按调制**（交付比特）：256QAM 上 40k 臂 **4 540 840**（CPU 3 276 344、W2-c128 4 034 392）⇒ **全部优势来自 256QAM**；
QPSK 2 909 336（CPU 2 880 016）、64QAM 持平、16QAM −5 928。**分析**：`report_e17_1104.md`；**结论**：`depth3_gate_and_model_pk.md` §46。

---

## 11.5 ★★★★★ HARQ 重传：能力已经存在，**但当前这把尺子测不了**（2026-11-05）

**(a) ★★★ 一个必须先纠正的判断**

**⇒ 用户提的判断是对的**：CPU 不是"解不出重传"，而是**回放里没有做软比特合并** ✓
**⇒ 回放器**本来就有这个能力：`--also <base> … --harq-chain` ——
"replays several transmissions of ONE HARQ process through ONE processor, so a retransmission combines with
the earlier soft bits"（`ul_chain_replay.cpp` 第 591 行）✓
**⇒ ★★ depth3 也不需要改网络**：链式接收机由**同一个后端工厂**创建（第 1332–1338 行）⇒ AI 臂的 LLR 走**同一条合并路径** ✓

**(b) ★★★★ 但**为什么不能在这把尺子上直接测**

**⇒ 回放器自己写明了危险**（第 465–470 行）：重传单独回放时，软缓冲是空的，
**decoder 会输出"零字"（null word），而零字同时通过 LDPC syndrome 与 CRC-24A** ⇒
**会报 `crc=OK`，但那是 5 763 个零字节的假成功** ⇒ 建在这种判决上的比较"什么也没比较" ✓
**⇒ 这也是为什么我们的尺子上那 43 个重传是 `cannot reserve a receive buffer`（直接拒绝）而不是假 OK** ✓

**⇒ 真正的障碍是**采样方式**：尺子的单元是**孤立的随机样本** ✓
实测（`ruler_s2_amc_5000` 内 439 个重传）：
- 能按 `(rnti, harq_id)` 找到更早首传的：**423 个** ✓
- 但**槽位间隔中位数 = 80 槽**（最小 1、最大 7 411）✓
- 而回放器的软缓冲 `expire_timeout_slots = **16**` ⇒ **只有 5.2%（22/423）在有效期内** ✗
- 且 `harq_id` 会被**不同 TB 复用** ⇒ 单看 sidecar **无法确认配对**是同一个 TB ✗

**(c) ★★★ 要测 HARQ，需要一把**新尺子**（时序尺子，而不是随机单位）**

| 项 | 做法 |
|---|---|
| 采样方式 | ★ **连续槽位**（同一 UE 的一段连续捕获，覆盖 1 次首传 + 1–3 次重传），**不是**随机抽单元 |
| 回放方式 | `--also <首传> --also <重传1> … --harq-chain`，**一个进程跑完整条链** |
| 软缓冲有效期 | 与**真实 UL HARQ RTT** 一致（当前 16 槽，实测重传间隔中位 80 槽 ⇒ 需核对 gNB 配置）|
| 两条臂 | 都必须用 `--harq-chain`；否则一条有合并、一条没有，比较无意义 |
| 指标 | 仍然**交付比特**；分母里现在包含重传（约 **9%** 的捕获）|

**(d) ★★ 当前口径下的结论**

**⇒ 现在所有"CPU vs depth3"的数字，**分母都不含重传的有效业务量**（那 8.78% 是 0 比特）✓
**⇒ 所以 +13.39% / +20.32% 这类数字描述的是"**首传**上的增益"，**不含 HARQ 合并带来的收益** ✓
**⇒ 这不是错误，是**口径**：报数时必须说清是"首传口径"还是"含 HARQ 口径" ✓

**(e) ★★★★ 那 43 个重传的 RV 分布（2026-11-05 19:20 实测）**

| RV | 个数 | 占比 |
|---|---|---|
| rv0 | 2 | 4.7% |
| rv1 | 6 | 14.0% |
| ★ **rv2** | ★ **31** | ★ **72.1%** |
| rv3 | 4 | 9.3% |

**RV × 调制**：

| 调制 | rv0 | rv1 | rv2 | rv3 |
|---|---|---|---|---|
| QPSK | 2 | 6 | **17** | 3 |
| 16QAM | 0 | 0 | 1 | 0 |
| 64QAM | 0 | 0 | 0 | 1 |
| **256QAM** | 0 | 0 | ★ **13** | 0 |

**⇒ ★★★ 三个含义**：

1. ★★ **这套流量几乎只用 rv2 重传**（72%）—— 即 **增量冗余（IR）**，不是追尾合并（Chase）✓
2. ★★★ **rv2 不是自解码的版本**（自解码的是 rv0/rv3）⇒ **单独收到 rv2 必然解不出**，
   这正是三条臂在 43 个重传上都是 **0 比特**的原因 —— 不是"网络不行"，是**缺了前一次的软比特** ✓
3. ★★ 这 43 个在整把尺子里有 41 个能找到首传，但**槽位间隔中位 230 槽**（缓冲只活 16 槽），
   **只有 2 个在有效期内** ⇒ **即便开 `--harq-chain` 也合并不了这几个** ✓

## 12. 常见坑

| 坑 | 症状 | 避免 |
|---|---|---|
| 忘了 `OCUDU_DEPTH3_FALLBACK=none` | AI 臂的数字"变好"，其实是经典链在帮忙 | 用 `run_arm_ext.py`，它设了 PURE |
| scales 不在 bundle 旁 | 静默用默认标定 | 开跑前 `SIDECAR` 那一行必须非空 |
| 两个 replay 并发 | 出现 −36 dB 的 SINR、结论符号翻转 | 串行 ＋ `--guard 1` |
| 用 dump 的 `crc=OK` 子集当尺子 | 经典链 100% | 用本尺子（未过滤）|
| 用块数下结论 | 大块被低估 | 用交付比特 |
| 重采样写成"取那 5 000 个" | CI 恒为 0 宽 | 每遍**抽 5 000 次**（有放回）|
| 换了二进制不记录 | 1-PRB 单元算不算会变 | 记 `BIN_SHA` |
| 校验 SHA256SUMS 时目录结构不对 | 一条都不 OK | 在含 `corpus/` 的目录里校验（§1.4）|
| 用 x86/Linux 导 Core ML | 没有 `coremlcompiler` | 回 Apple Silicon |

---

## 13. 一页速查

```bash
# 尺子
cd ~/dev/ocudu/lib/phy/upper/ai_signal_processors/test
cat ruler_s2_amc_5000.tar.gz.part* > /tmp/ruler.tar.gz && tar -xzf /tmp/ruler.tar.gz -C /tmp
mkdir -p /tmp/rv && ln -sfn /tmp/ruler_s2_amc_5000/corpus /tmp/rv/corpus
(cd /tmp/rv && shasum -a 256 -c "$OLDPWD/ruler_s2_amc_5000_SHA256SUMS")     # 10 000 OK

# 模型 → bundle
train_depth3.py --export-only M.keras --export-probe /tmp/m.mlpackage \
  --modcond --layout spatial --modulation 256QAM --c-hid 128 --blocks 18 --snr-range 0,48
xcrun coremlcompiler compile /tmp/m.mlpackage B/
calibrate_depth3.py --model M.keras --data monitor.npz --cond --layout spatial --samples 60 --write-scales B/

# 回放（回放机）
python3 run_arm_ext.py --arm ai --bundle B/M.mlmodelc --units ruler_s2_amc_5000_units.txt \
  --corpus /tmp/rv/corpus --out arm.csv --logs logs_arm --tmp tmp_arm --warmup 0 --guard 1

# 算分（开发机）
python3 analyze_<批次>.py
```
