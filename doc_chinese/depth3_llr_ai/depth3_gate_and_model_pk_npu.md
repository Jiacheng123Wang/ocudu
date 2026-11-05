# depth3 门控 PK —— **npu 口径**（2026-11-05 起）

> **这份文档只收 npu 口径的数字。**
> 旧的 gpu 口径数字已冻结并归档在 `depth3_gate_and_model_pk.md` **§52**，**不与本表混用、不换算**（理由见那里的 (d)）。

---

## 1. 口径定义（每张表都必须带这四行）

| 项 | 值 |
|---|---|
| **pipeline** | ★ `--phy_pipeline npu` ＝ **PHY 全部模块在 CPU/DSP（FFT、估计、均衡/解映射、LDPC），只有网络在 ANE** |
| **解码器** | ★ **CPU LDPC**（`--pusch_ldpc_decoder_type generic`；Metal LDPC 虽可用，但性能差、不作为默认）|
| **模式发布** | ★ 回放器**发布** pipeline 模式（`phy_pipeline_mode_registry::set`）⇒ **严格策略生效**：device 拒绝的 hop **必须失败**，不许回退宿主 |
| **HARQ** | ★ **首传口径**：重传（`new_data=0`，rv2 为主）三条臂都是 0 比特 ⇒ 数字不含 HARQ 合并收益（见 PK §51）|

**⇒ `npu` 口径要回答的问题**：**AI 相对"真 gNB 的 CPU 链"好多少** ✓

---

## 2. 参照基线（两条，不能混）

| 基线 | 定义 | 同 5 000 单元（腿 `s2_amc_ping_1102a`）|
|---|---|---|
| **`cpu-dump`**（≈ 真 gNB）| `ul_chain_replay_cpu`（`b68ef309…`），`--phy_pipeline cpu --pusch_receiver_backend classic` | **4 105 解码 / 11 565 432 bit** |
| `cpu-metal-harness`（旧参照）| `metal/ul_chain_replay`（`d785f039…` 203 / `dbe8f06d…` 本机），经典路 | 3 465 解码 / 8 933 800 bit |

**⇒ ★★ npu 口径的 "vs CPU" 一律指 `cpu-dump` 基线**（同一个 CPU 链、同一把尺子）；
`cpu-metal-harness` 只用于与旧口径对账 ✓

---

## 3. 测量协议（与既有尺子一致，不重复发明）

| 项 | 值 |
|---|---|
| 尺子 | `ruler_s2_amc_5000`，5 000 单元；`units sha256` **`47dcad0d742d0d55ff67358f…`** |
| 模式 | `OCUDU_DEPTH3_FALLBACK=none`（PURE）、每单元一进程、**串行**、`--guard 1` |
| 必须记录 | `BIN_SHA`、`BUNDLE_FILES`、`SIDECAR`、`UNITS_SHA`、`PIPELINE npu`、`DECODER generic` |
| 指标 | **交付比特**（`crc=OK` 的 `tbs_bytes×8` 之和）；块率仅作对照 |
| 统计 | 单元 bootstrap 2 000 次（seed 20261102）＋ McNemar（连续性校正）|

---

## 4. 结果表（**待填**，每测一臂追加一行）

| 臂 | 模型 / 轮次 | 交付比特 | vs `cpu-dump` | 95% CI | McNemar | 备注 |
|---|---|---|---|---|---|---|
| _（待测）_ | | | | | | |

**⇒ 第一臂建议**：`w2c32_b18_e60`（gpu 口径 +13.39% 的那条），
**目的**：把"换到 npu 口径后，同一个模型的增益变成多少"作为本表的锚点 ✓

**⇒ 第二臂**：`w2c32_b18_e46`（gpu 口径 +9.86%）—— 检验"val 选出的最佳轮"在 npu 口径下是否仍不如第 60 轮 ✓

---

## 5. 前置条件（实现完成后才能开始填表）

| # | 条件 | 状态 |
|---|---|---|
| 1 | `--phy_pipeline npu` 可用（回放器解析 ＋ 模式发布 ＋ 解码器默认 CPU）| 🔄 实现中（M0）|
| 2 | 旧别名（`--cpu/--metal/--metal-cpu-ldpc/--metal-cpu-demod`）被**拒绝**并指路 | 🔄 实现中 |
| 3 | `npu + classic` ≡ `cpu-dump`（同一 5 000 单元逐单元一致）| 待验 |
| 4 | `npu + ai` ≡ `--metal-cpu-ldpc + ai`（20/20 单元）| ★ **已验证**（计划文档 §2.3）|
| 5 | 测量脚本 `run_arm_ext.py` 透传 `--phy_pipeline npu` 并记录 | 待改 |

---

## 6. 记录规则

1. ★ **每臂一行**，带模型文件与轮次；表格只增不改；
2. ★ 引用本表必须写 **"npu 口径"**；
3. ★ **不得**与 `depth3_gate_and_model_pk.md` §52 的 gpu 口径数字同列比较；
4. ★ 若某臂只在 500 单元探针上测过，**单独一列标注"探针 500"**，不与 5 000 混；
5. ★ 任何"比 CPU 好 X%"必须同时给出**该尺子的 256QAM 占比（39.2%）**（否则增益不可解读，见配方 memo §5.8）。
