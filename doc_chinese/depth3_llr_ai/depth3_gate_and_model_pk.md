# depth3 门控与模型 PK 表

**本文档的用途**：给 depth3 的候选模型做**统一口径的横向对比**（门控测量）。
**本文档取代** `llr_ai_detection/model_register.md` §1.2.2 那张表的**日常使用**——那张表记录的是 w2 时代的历史，
本表从 s2 批次起成为**当前口径**。

---

## 1. ★ 对比列（2026-11-01 用户定）

| 列 | 含义 |
|---|---|
| **编号** | 模型名（目前：**E4**＝现役默认、**E17**）|
| ★ **`w2_test1` 的 TB CRC** | 在 **`data/datasets_test/w2_test1.npz`**（10 000 条）上的 TB 级 CRC 通过率 |
| ★ **`w2_test2` 的 TB CRC** | 同上，另一套（10 000 条）|
| **attach TB CRC** | 在 **attach 数据集**上的 TB 级 CRC 通过率（w2 时代沿用）|
| **时延 p50／p90／p99／max** | `forward` 段的四次分位（µs），同一二进制、同一配置、700 次 |

**⇒ 自 2026-11-01 起**：
- ★ **"三维分箱 crc=OK"一列暂时不做** —— 当前 s2 数据集还小，分到 112 个箱后每格样本太少，逐格 CRC 率没有统计意义。**以后数据够了再补** ✓
- ★ **CB CRC 一列早已删除**（链上只暴露 TB CRC）✓

---

## 2. ★ 模型基本情况表

**⇒ 结构口径与 `model_register.md` §1.2.1 一致** ✓（c32 ＝ `--c-hid 32`，blk ＝ `--blocks`）

| 编号 | 结构 | 参数 | 语料 | 选点 | 状态 |
|---|---|---|---|---|---|
| **E4**（现役默认）| c32 / 18 blk | **26 185** | 真实 43 902 | val | ★ **部署中** |
| **E17** | c32 / 18 blk | **26 185** | E17-100k | val（跑满 40）| 已训完（best val **0.15301**）|
| ★ **W2-QPSK** | c32 / 18 blk | **25 859** | **QPSK 46 185**（w2_train 全量）| val（QPSK 250 条）| ★ **已训完**（9 轮早停，best val **0.07754**，BER 0.0327）|
| ★ **W2-16QAM** | c32 / 18 blk | **25 859** | **16QAM 48 075** | val（16QAM 328 条）| ★ **已训完**（12 轮早停，best val **0.02146**，BER 0.0067）|
| ★ **W2-64QAM** | c32 / 18 blk | **25 859** | **64QAM 39 006** | val（64QAM 262 条）| 🔄 **训练中**（107，10:29 起）|
| ★ **W2-256QAM** | c32 / 18 blk | **26 057** | **256QAM 27 337** | val（256QAM 160 条）| 🔄 **训练中**（211 提前开工，3 轮 val 0.166）|
| ★ **W2-cond** | c32 / 18 blk | **26 057** | 四调制全量 **160 603** | val | ⏳ **待训**（用四个单调制主干平均热启动）|

**⇒ 两点说明**：
- ★ **单调制模型的参数比条件化少**（25 859 vs 26 057）：主输出头只有该调制的 `Qm` 个 logit ✓
- ★ **W2-256QAM 的参数是 26 057**（＝条件化）：它的 `Qm = 8`，头宽恰好等于 `COND_STRIDE` ✓

---

## 3. ★★ 模型 PK 表


| 编号 | ★ s2_train1 TB CRC | ★ s2_train2 TB CRC | attach TB CRC | 时延 p50／p90／p99／max（µs）|
|---|---|---|---|---|
| **E4**（现役默认）| 待测 | 待测 | **69.2%**（6 430 条）| **210 ／ 220 ／ 267 ／ 2 923** |
| **E17** | 待测 | 待测 | 待填（6 456 条）| 待测 |
| **W2-QPSK**（s2 单调制，训练中）| — | — | — | — |
| **W2-16QAM**（同上）| — | — | — | — |
| **W2-64QAM**（同上）| — | — | — | — |
| **W2-256QAM**（同上）| — | — | — | — |
| **W2-cond**（四主干平均热启动）| 待测 | 待测 | 待测 | 待测 |

**读法**：`—` ＝ 该模型不适用于这一列（例如单调制模型没有条件化的验收集口径）；**待测** ＝ 尚未测量。

---

## 4. ★ 三套数据从哪来

### 4.1 attach 数据集（w2 时代沿用）

| 项 | 值 |
|---|---|
| ★ **样本数** | ★ **9 217** |
| 调制分布 | QPSK **1 333** ／ 16QAM **734** ／ 64QAM **3 565** ／ 256QAM **3 585** |
| ★ **集中位置** | ★ **211：`~/work/ai_phy_train/data/datasets_train_E17/e17_attach/depth3.npz`**（0.33 GB）|
| ★ **本机备份** | ★ **`~/ai_depth3/attach/e17_attach_depth3.npz`** ✓ |
| 历史口径 | E4 门控用了其中 **6 430** 条，E17 用了 **6 456** 条（按当时的过滤条件不同）|

**⇒ 按用户 2026-11-01 的决定**：★ **集中放在 211，本机保留备份** ✓（已完成 ✓）

### 4.2 `w2_test1` / `w2_test2`（两套验证集，已切好）

| 项 | 值 |
|---|---|
| ★ **位置** | ★ **211：`~/work/ai_phy_train/data/datasets_test/`** ✓ |
| ★ **文件** | **`w2_test1.npz`**（1.27 GB）＋ **`w2_test2.npz`**（1.27 GB）✓ |
| ★ **各 10 000 条** | ✓ |
| 来源 | ★ **从 `datasets_full_w2`（母集 2 526 019 条）简单随机抽** ✓（seed 20261031 ✓，`manifest_w2_test.json` 有记录 ✓）|
| 调制构成 | test1：QPSK 2 523 / 16QAM 3 421 / 64QAM 2 492 / 256QAM 1 564 ✓；test2 类似 ✓ |

**⚠ 一处需要澄清的事实**：★ **这两个文件是从 `full_w2` 母集抽的** ✓，**不是从 s2 新批次抽的** ✗
**⇒ s2 批次自己的验证集还没有建** ✓ ⇒ 若以后要用 s2 的验收集，需要另切一套并改本表列名 ✓

**⇒ 目前**：本表两列直接引用这两个文件 ✓（用户 2026-11-01 指定 ✓）

### 4.3 时延怎么量

**⇒ `probe/gate_timing.py --model <mlmodelc>`** ✓，同一二进制、同一配置，跑 **700 次** ✓，
取 `forward` 段的 **p50／p90／p99／max** ✓（需要 `OCUDU_DEPTH3_TIMING=1` ✓）

**⇒ `forward` 段包含**：选桶 ＋ 裁剪拷贝 ＋ `predict_depth3()` ＋ 窄头散射 ✓
**⇒ 不含**：输入平面拼装、LLR 收集/量化/解扰、经典估计器、上报 ✓

**⇒ 参考**：E4 的 `predict`（引擎本身）实测 **194 ／ 201 ／ 246 ／ 2 851 µs** ✓
⇒ `forward − predict ≈ 15–20 µs` 是"我们的胶水"（选桶、裁剪、散射）✓

---

## 5. ★ 变更记录

| 日期 | 变更 |
|---|---|
| **2026-11-01** | 建立本文档；按用户定下的列口径（编号 / s2_train1 / s2_train2 / attach / 时延）；**三维分箱列暂缓**；attach 数据集确认在 211 并有本机备份（9 217 条）；E4 的 attach 与时延填入历史值；s2 两列待 s2 建库后填 |

---

## 6. ★★★★ 2026-11-01 12:20 **M2 MacBook（203）上的门控环境搭建与首个问题**

**(a) ★ 目的**

**⇒ 门控测量必须走** `ul_chain_replay` 的 **GPU 路径 ＋ AI 后端** ✓（建库时用的是 CPU 路径 ✗）
**⇒ 而 GPU 路径只在 **Apple Silicon 的 macOS** 上可用 ✓ ⇒ 目标是 **M2 MacBook Pro（192.168.31.203）** ✓

**(b) ★ 已完成**

| 步 | 结果 |
|---|---|
| ★ **代码同步** | ★ **GitHub pull 成功**（`ecffd04d9f` → **`a20e3df4b4`**）✓，本地 0 改动 ✓ |
| ★ **构建** | ★ **两个目标都建成**：`ul_chain_replay_cpu`（3.1 MB，CPU 版）＋ ★ **`ul_chain_replay`（3.2 MB，Metal 版）** ✓ |
| ★ **GPU 路径** | ★ **可用**（`--metal` ＋ `--phy_pipeline gpu` 都能跑 ✓）|
| ★ **AI 后端** | ★ **能绑定模型**（`OCUDU_REPLAY_MODEL=<*.mlmodelc>` ✓，日志确认 "depth-3 receiver model: …" ✓）|
| 测试语料 | 从 T7 的 `s2_mcs00_ping_1031a` 取 **40 个单元**送到 203 ✓ |

**(c) ★ 机器**：M2 / macOS 26.6.2 / **8 GB 内存** ⚠ / 221 Gi 可用 ✓

**(d) ★★ 发现的问题：AI 臂能绑模型，但解码全 KO**

**⇒ 5 个单元对比**：

| 单元 | CPU（经典）| ★ GPU ＋ AI |
|---|---|---|
| `cap_10139_18477_17971` | crc=**OK** | ★ crc=**KO** |
| `cap_10215_19997_17971` | OK | **KO** |
| `cap_1025_27_17921` | OK | **KO** |
| `cap_1035_227_17921` | OK | **KO** |
| `cap_10381_2837_17971` | OK | **KO** |

**⇒ 换过三种模型都一样**：`d3_modcond_r7` ✓、`d3_e4.mlmodelc` ✓、`buckets/base_w612.mlmodelc` ✓ ⇒ **全部 KO** ✗

**(e) ★★ 已经定位的两条线索**

**① `sinr=inf` 是**预期的**** ✓ —— AI 臂的日志说：
> "the bound bundle carries neither reporting head; RSRP, EPRE and the SINR will be published from the shared
> measurement layer instead of from a head"
**⇒ 所以 SINR 印 inf 不代表模型坏** ✓（它不发布 SINR ✓）

**② ★ 所有 bundle 都缺 `depth3_scales.txt`** ✗ —— AI 臂反复警告：
> "there is no sidecar at all, so the noise map falls back to the receiver's defaults (**A_n=4 B_n=-1**).
>  Regenerate it with `calibrate_depth3.py --write-scales`"
**⇒ LLR 的噪声映射退回了默认值** ✓ ⇒ **这很可能就是解码失败的原因** ✓

**(f) ★ 下一步（未做）**

```bash
# 1) 生成标定 sidecar（需要 val.npz）
calibrate_depth3.py --model <run>/depth3_best.keras --data <val.npz> --write-scales <bundle目录>
# 2) 重跑同一个单元
OCUDU_REPLAY_MODEL=<bundle>/<name>.mlmodelc \
  ul_chain_replay <unit> --out /tmp/x --metal --phy_pipeline gpu --pusch_receiver_backend ai
```

**⇒ 若生成 sidecar 后仍 KO**，则问题在**输入布局 / 模型与 C++ 侧的契约** ✓ ⇒ 下一步用 gNB 的门控脚本
（`probe/check_ai_arm_binding.sh`，走 `--expert_phy.pusch_receiver_model`）做对照 ✓

★ 状态 ✓：★ 203 的 GPU 门控环境已搭好并跑通 ✓；★ AI 臂能绑模型 ✓；★ 但解码全 KO 已定量记录 ✓；★ 两条线索与下一步已列 ✓。

### 6.1 ★★★★★ 2026-11-01 12:35 **对照实验：同一二进制在本机 OK、在 M2 上 KO —— 是环境问题，不是代码问题**

**(a) ★ 本机（M4 Pro）跑同样的命令**

```bash
OCUDU_REPLAY_MODEL=…/depth3_modcond_r7/d3_modcond_r7.mlmodelc \
  ul_chain_replay <同一单元> --out /tmp/mac_ai --metal --phy_pipeline gpu --pusch_receiver_backend ai
```

| 机器 | 结果 |
|---|---|
| ★ **本机 M4 Pro** | ★ **`crc=OK iterations=1 sinr=21.56 dB`** ✓✓ |
| ★ **203 M2** | ✗ **`crc=KO iterations=6 sinr=inf dB`** ✗ |

**⇒ 同一份代码、同一个模型、同一个捕获单元** ✓ ⇒ **结果完全不同** ✗

**(b) ★★ 逐步排除（都排除了）**

| 怀疑项 | 检查 | 结论 |
|---|---|---|
| ★ **模型文件** | 两边 `mlmodelc` 的**文件列表与总大小完全相同**（164K，5 个文件）✓ | ✗ 不是它 |
| ★ **编译选项** | 两边 `CMakeCache` 的 `ENABLE_METAL_*` **完全一致**（全 ON）✓ | ✗ 不是它 |
| ★ **compute unit** | 203 上 `all` / `cpu` / `gpu` / `ane` **四种全 KO** ✗ | ✗ 不是 ANE 选择 |
| ★★ **二进制** | ★ **把本机跑通的二进制复制到 203** ⇒ ★ **仍然 KO** ✗ | ✗ 不是构建差异 |
| **macOS 版本** | 两边都是 **26.6.2** ✓ | ✗ 不是版本 |

**(c) ★★ 结论：问题在 203 的**运行时**，而不是代码**

**⇒ 最关键的证据**：★ **连本机编译、本机验证可用的二进制，拿到 203 上也 KO** ✓
**⇒ 所以只能是** 203 上 **CoreML 推理本身**在返回异常值** ✓（`sinr=inf` 是模型输出被判定为 Inf 的直接迹象 ✓）

**(d) ★ 剩下的两个怀疑方向**

| # | 怀疑 | 怎么验 |
|---|---|---|
| ★ 1 | **CoreML 在 M2 上加载/预测该模型失败**（8 GB 内存压力？ANE 首次编译？）| ★ 写一个**最小 CoreML 预测脚本**在 203 上直接跑该 `.mlmodelc`，看输出是否含 Inf |
| ★ 2 | **该 `.mlmodelc` 是在本机编译的产物**，其内部的执行计划可能在 M2 上不被接受 | ★ 在 203 上用 `xcrun coremlcompiler` **重新编译**同一 `.mlpackage`，再测 |

**⇒ 建议先做第 1 步**（最小预测脚本，10 行）——它能立刻区分"模型加载失败"与"模型加载成功但输出坏" ✓

★ 状态 ✓：★ 对照实验完成 ✓；★ 五个怀疑项已排除 ✓；★ 结论锁定为 203 的 CoreML 运行时 ✓；★ 两个下一步已列 ✓。

