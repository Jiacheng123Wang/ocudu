# Session handoff —— `llr_ai_detection`（2026-10-14，第 1 次）

> **用法**（沿用约定 ✓）：新会话**只读这一份**就能接着开工 ✓。
> **结构** = ★★★ 最先的三件事 ① 本会话做了什么 ② 当前状态 ③ **下一个会话的第一件事（具体命令）**
> ④ 详细执行方案 ⑤ 未决问题 ⑥ 纪律（F15–F28）⑦ 常用命令 ⑧ 提交。
>
> **上位文档**：`llr_ai_detection_design_and_implementation.md`（★ **§19.x = 本会话的完整记录，23 节**）、
> `model_network_design.md`（★ **§0.2 = I/O 契约，本会话已按空间布局逐行改写** ✓、§45 = R1 的证据）。
>
> ★★ **本会话 23 个提交**：★ 范围 `2e6df5948e` … `46ba0e26a4`（★ 命令：`git log --oneline 43133f3cfb..46ba0e26a4`）。
> ★★ **工作树干净** ✓。★ 注：`43133f3cfb` 是上一会话的最后一个提交（**基线**，不属于本会话）。

---

## ★★★ 先写在最前面的三件事

### ★★★ 一、【判据 1 第一次有了有效读数，而且是正面的】

★★★ **35 个窄分配可译采集上：经典链 `31/35` ↔ 网络 `32/35`** ✓✓，★ 其中 **2 个采集网络译出而经典没译出** ✓、**1 个相反** ✗。
★★ **同一批采集、同一个旧模型**（★ 只在 ≥43 PRB 训过）：★★ **经典 31、网络 `7`** ✗✗ ⇒ ★★★ **微调把它从 7/35 提到 32/35** ✓✓✓。
★★★ **⇒ C1（不劣化）在【同分布 + 可译】池上成立** ✓，★ 且**净领先一个** ✓ —— 此前两次"读数"都是伪影（★ 包络越界 ✗、★ 全零字 ✗）。

> ★★★ **这两个数字已被 §19.29 更正过一次** ✗：★★ 原先写的 **`40/44 ↔ 40/44`**（★ 对照组 `8`）用的是
> **含 9 个重复名的清单** ✗（★ 44 行 = **35 个唯一采集**）★ ⇒ ★★ 更正后**结论方向不变、而且更强** ✓✓。
> ★★ 同一节还更正了 **`_llr.bin` 被当平面数组读** ✗（★ "逐位相同 50–88%" 是**两份记录混读**的伪影 ✓）。

### ★★★ 二、【这条路上最贵的两个坑，都是"判据本身坏了"】

| ★ | ★★ 坑 | ★★★ 现在的状态 |
|---|---|---|
| ★★★ | **`crc=OK` 会在【全零字】上通过**（★ 全零 TB 同时过 LDPC 校验子与 CRC-24A ✗）★ ⇒ 所有 rv=2 的"译出"都是**全零字** ✗ | ★★ 已修：`compare_ai_arm.py` **要求 TB 非零** ✓，否则判 `NULL` ✓（★ **F24/F28**）|
| ★★★ | **精度包络**：旧模型只在 **≥43 PRB** 训过，★ 而语料里**可译的都是 4–5 PRB** ✗ ⇒ "AI 差 32 个接收" ✗ | ★★ 已修：混合宽度微调（★ 窄 BLER **0.5245→0.4200**，★ 宽**未劣化** ✓），★ `MIN_PRB` 43 → **4** ✓（★ **F23**）|

### ★★★ 三、【下一个会话的第一件事：按 ①→②→③ 顺序，不要先采语料】

| # | ★★ 动作 | ★★ 为什么是这个顺序 |
|---|---|---|
| ★★★ **①** | ★★ **把深度 3 臂登记进 crossing 审计的模块清单**（★ 现在清单是 `equalizer, demapper, dft, channel_estimator` ✓，**不含本臂** ✗）⇒ 然后重跑 R5.5 | ★★ 现在 R5.5 **记为"未测"而不是 0** ✓；★★ 这是把它变成数字的**唯一正确修法** ✓（★ 也让"AI 臂强制把设备网格读回主机"这件事**可见** ✓）|
| ★★ **②** | ★★ **R6 的头 3/4 → 测量核**（★ 方案见 §④C）| ★★ 现在 RSRP/EPRE/SNR/TA **仍由经典估计器提供** ✓（★ 暂态 ✓）；★★ 头 2 → SINR 已接 ✓（★ 实测 24.18 → 34.14 dB ✓）|
| ★★★ **③** | ★★★ **按 §19.22 的配方采集新语料**（★ 包络内 + 保守 MCS + 高 SNR）| ★★★ **判据 1 的样本只有 35 个、且只有 256QAM** ✗ ⇒ ★★ 要 ≥100 个、★ 并覆盖其它调制（★ QPSK/64QAM/16QAM **现在无数据** ✗：24/12/0 个单元 ✗）|

---

## ① 本会话做了什么

**一句话**：★★ **从"评估训练算力"开始，把契约按空间布局落地、把接缝真正切到网络、
拿到判据 1 的第一个有效读数，并在这个过程中修掉了三个"判据本身"的缺陷** ✓。

### A. ★★★ 三个最重要的【技术】结论

| ★ | ★★ 结论 | ★★★ 证据 |
|---|---|---|
| ★★★ | ★★★ **符号轴空间化是对的**（★ 上一会话的结论被独立确认 ✓）| ★★ ANE 上空间化只贵 **2.7×**（★ 折叠 4 头 360 µs ↔ 空间化 **961–1095 µs** ✓，★ **不是 FLOPs 的 14×** ✓）；★★ 时延随宽度**近似线性**（★ `c_hid` 64→32 ⇒ **441 µs** ✓、★ 18→9 块 ⇒ **302 µs** ✓）⇒ ★★ **预登记的"符号轴分块"退路不需要** ✓ |
| ★★★ | ★★★ **辅助头在空间化布局下【抢主干容量】** ✗（★ 折叠布局下那条"无害"结论不成立 ✗）| ★★ 同预算 BER：★ 四头（权重 0.1）**0.0813** ↔ 权重 0.01 **0.0299** ↔ **无辅助 0.0117** ✓ ⇒ ★★ **两段制训练** ✓ |
| ★★★ | ★★★ **GPU 训练只有"手写纯 PyTorch"这一条路，收益 2.5×** | ★★ 八条后端路径实测（★ §19.1）：TF-CPU **25.2 ms/样本** 是现状；★ TF-Metal **0.55×** ✗、★ Keras-torch+MPS **0.72×** ✗、★ XLA **0.19×** ✗；★ 裸 PyTorch+MPS fp32 **2.55×** ✓ / fp16 **4.07×** ✓ ⇒ ★★ **用户裁定：不移植** ✓ |

### B. ★★★ 模型与部署的现状

| ★ | ★★ 项 | ★★★ 数值 |
|---|---|---|
| ★★ 输入 | ★ `[1, 612, 14, 12]` fp16 | ★ 每符号 12 通道 ✓ |
| ★★ **参数量** | ★★ **89 164** | ★★ fp32 348 KB / fp16 174 KB ✓；★ 98.2% 在 18 个残差块里 ✓ |
| ★★ 感受野 | ★ **频率 127**、★ **时间 37 符号**（> 14 ⇒ 覆盖整 slot ✓）| ★ dilation 循环 (1,2,4,8) ✓ |
| ★★ 算力 | ★ **1.47 GFLOP / 样本**（★ 733 M MACs ✓）| ★ 输入 201 KB / 输出 184 KB ✓ |
| ★★★ **ANE 时延** | ★★★ **953–958 µs**（★ 300 次 ×2 遍 ✓，★ 新旧模型相同 ✓）| ★★ 四个头全部在 ANE ✓ |
| ★★ 部署产物 | ★ `d3_mixed.mlmodelc`（★ **本会话最新、当前最好** ✓）| ★ 288 KB ✓ |
| ★★ sidecar | ★ `depth3_scales.txt`（★ `s_llr=20` ✓）| ★★ R4.5 已拟合 ✓ |

### C. ★★★ 精度（离线，★ 合成验证集）

| ★ 模型 | ★★ uBER | ★★ coded BLER | ★★ 说明 |
|---|---|---|---|
| ★★ `d3_full_spatial_aux`（★ 宽分配）| ★★ **0.0496** | ★ 0.4097 | ★ 1024 样本 / 6949 码块 ✓；★★ vs genie **0.0472 / 0.3792** ⇒ **差 5% / 8%** ✓ |
| ★★★ **`d3_mixed_narrow`（★ 当前最好）** | ★★ **0.0495**（★ 宽）/ 0.0500（★ 窄）| ★★ **0.3998**（★ 宽）/ **0.4200**（★ 窄）| ★★ 窄分配离 genie 从 **+30% 降到 +3.7%** ✓✓，★ 宽**未劣化** ✓ |

### D. ★★★ 接缝（R5/R6）

★★ **AI 臂已经真的在算** ✓：★ 组装 → 一次前向 → gather → 量化（★ 链自己的 `quantize` ✓）→ 解扰 → `on_new_block` ✓，
★★ **不调用经典解调器** ✓；★ **头 2 → `on_end_stats` 的 SINR** 已接 ✓。

★★★ **途中抓到的四个真缺陷（★ 每一个都会让"AI 臂"静默地什么都不做 ✗）**：
1. ★★ **`on_end_codeword()` 没调用** ⇒ 译码器不知道码字结束 ⇒ `crc=KO iterations=0` ✗；
2. ★★ **软比特必须来自 `get_next_block_view()`**，不能用自己的缓冲 ✗；
3. ★★ **链的网格是 3300 子载波（275 PRB）**，★ 契约窗口是 612 ✗ ⇒ 处置：**接受更宽的网格、但分配越出窗口就拒绝** ✓；
4. ★★ **降级日志走 PHY logger 时不可见** ✗ ⇒ 改 `fmt::print(stderr)` ✓（**F20**）。

### E. ★★★ 两个"判据本身坏了"的坑（★ 本会话最贵的教训）

★★★ 见前面的"三件事"之② ✓。★★★ **另加一条**：★★ **语料自带的实时 dump（`_h.bin` / `_llr.bin`）来源与格式【未解释】** ✗
（★ `_h.bin` 与重放信道估计只有 **0.32** 相关 ✗、★ `_llr.bin` 大小差 **27.6×** ✗）⇒ ★★ **不能当保真度金标准** ✓（**F26**）。

### F. ★★ 顺带完成的事

- ★★ **R4.5 标度校准** ✓：★★ 平台 `S_llr ∈ [5,20]`、★ 保留训练值 20、★★ **量化前后 coded BLER 差 `0.0000`** ✓；
  ★★ 结构性解释：★★ **归一化 min-sum 对整体尺度不变** ⇒ **标度只通过量化器起作用** ✓（★ 新增 `calibrate_depth3.py` ✓）。
- ★★★ **INT8 分析**（★ 用户质疑触发 ✓）：★★ 权重 int8 **不省 ANE 算力** ✓ ——
  ★★ 物证：★ 编译后的图里 **18 × `constexpr_lut_to_dense`（pal8）/ 18 × `constexpr_affine_dequantize`（int8）** ✓，
  ★ 即"**解压回 fp16 再算**" ✓，★ 实测还慢 **1–3%** ✓；★ coremltools 9.0 **没有激活量化器** ✗。
  ★★★ **而且更强的一条**：★★ **权重 int8 把接收机弄坏了** —— **coded BLER 0.4045 → 0.9083** ✗✗
  （★ 而 ANE 侧符号一致率还有 **97.9%** ✗）⇒ ★★ **量化判据是 coded BLER，不是符号一致率** ✓（**F27/F28**）。

---

## ② 当前状态

### ★★★ 已就绪（★ 直接可用）

| ★ | ★★ 件 | ★★ 位置 |
|---|---|---|
| ★★★ | ★★ **训练/评估/校准/数据/译码 七个工具**（★ 都有自检 ✓）| `.../metal/ai_train/{depth3,depth3_loss,build_depth3_dataset,ldpc_decode,train_depth3,eval_depth3,calibrate_depth3}.py` |
| ★★★ | ★★ **契约在接缝侧的唯一实现** | `pusch/pusch_depth3_input_plane.{h,cpp}` + `test/pusch_depth3_input_plane_check.cpp` ✓ |
| ★★★ | ★★ **网络臂（★ 真的在算）** | `pusch/pusch_depth3_receiver_impl.cpp` ✓ + 引擎 `ocudu_coreml_nn_engine.{h,mm}` ✓ |
| ★★ | ★★ **离线链重放（★ 支持 `--harq-chain`）** | `ul_chain_replay` ✓ |
| ★★★ | ★★ **验收脚本** | `work_tmp/probe/compare_ai_arm.py`（★ **非零 TB 判据** ✓）、`chain_harq_pairs.py`、`quantize_depth3_probe.py` ✓ |
| ★★ | ★★ 数据集 | `d3_cap00{1,2}_rv0`（★ 宽 ✓）、★ `d3_cap00{1,2}_mixed`（★ ≥4 PRB ✓）、★ `d3_cap002_narrow` ✓ |
| ★★ | ★★ 模型 | ★★ **`work_tmp/probe/d3_mixed.mlmodelc`**（★ 当前最好 ✓）、★ `d3_full_spatial_aux` ✓ |

### ★★★ 必须做（★ 按依赖）

| ★ | ★★ 缺什么 | ★★ 详见 |
|---|---|---|
| ★★★ | ★★ **R5.5：把本臂登记进 crossing 审计**，然后重跑 | §④A |
| ★★ | ★★ **R5.5 的 `--device-grid`**（★ 现在它只是 DFT 路由的选项 ✗）| §④A |
| ★★★ | ★★ **R6 头 3/4 → 测量核** | §④B |
| ★★★ | ★★ **采集新语料**（★ 配方在 §19.22 ✓）| §④C |
| ★★ | ★★ **判据 1 扩到 ≥100 个样本**（★ 现在 35 个、只有 256QAM ✓）| §④C |
| ★★ | ★★ **其它调制**（★ 现在**无数据** ✗：QPSK 24、64QAM 12、16QAM 0 个单元 ✗）| §④D |
| ★ | ★ **TB dump 缺陷**（★ 94% 全零 ✓ ＋ 分段不符 ✗）⇒ `--source real` 不可用 ✗ | §⑤ |

---

## ③ ★★★ 下一个会话的第一件事（★ 具体命令 ✓）

```bash
cd /Users/jiachengwang/dev/ocudu
W=doc_chinese/llr_ai_detection/work_tmp
P=lib/phy/upper/signal_processors/channel_estimator/metal/ai_train
PY=~/ai_ce_work/venv/bin/python      # ★ 系统 python 没有 TF/Keras

# ① ★★★ 先确认环境健康（四个自检必须全过）
$PY $P/depth3.py --selfcheck
$PY $P/depth3_loss.py --selfcheck
$PY $P/ldpc_decode.py --selfcheck
$PY $P/build_depth3_dataset.py --selfcheck --out $W/data/datasets/d3_cap001_rv0.npz

# ② ★★★ 编译并验证引擎/组装器（★ 本会话改过，先确认没坏）
cmake --build build --target ul_chain_replay depth3_engine_check pusch_depth3_input_plane_check -j8
./build/lib/phy/upper/signal_processors/channel_estimator/metal/depth3_engine_check \
    $W/probe/d3_mixed.mlmodelc 6 612                       # ★ 期望 PASS + 四个头都被写入
./build/lib/phy/upper/channel_processors/pusch/pusch_depth3_input_plane_check \
    $W/tmp/input_plane_fixture.bin                          # ★ 期望 PASS（★ 若无 fixture 先跑 §⑦）

# ③ ★★★ 判据 1 的回归（★ 用当前最好的模型，★ 清单必须【唯一】✓：20 + 15 = 35 个窄分配采集）
$PY $W/probe/compare_ai_arm.py --tool ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    --corpus $W/corpus/aillr_cap001 --model $PWD/$W/probe/d3_mixed.mlmodelc \
    --list $W/tmp/narrow_uniq_cap001.txt --out $W/tmp/again_cap001.csv
$PY $W/probe/compare_ai_arm.py --tool ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    --corpus $W/corpus/aillr_cap002 --model $PWD/$W/probe/d3_mixed.mlmodelc \
    --list $W/tmp/narrow_uniq_cap002.txt --out $W/tmp/again_cap002.csv
# ★★ 期望：cap001 classic 17 / ai 18，cap002 classic 14 / ai 14（★ 合计 31 ↔ 32 ✓，★ 与 §19.29 一致 ✓）
# ★★ 注意：清单若含重复名，脚本会【报出并去重】✓（★ 旧的 narrow_ok_*.txt 就含 9 个重复名 ✗）
```

---

## ④ ★★★ 详细执行方案

### ★★★ A. R5.5 收尾（★ 先做，★ 因为它现在记的是"未测"）

| ★ 步 | ★★ 内容 |
|---|---|
| ★★ A1 | ★★ **把深度 3 臂登记进 crossing 审计的模块清单** ✓（★ 现在只有 `equalizer, demapper, dft, channel_estimator` ✗）★ —— 先找到审计的实现（★ `phy_pipeline_crossings` ✓）与各模块登记的位置 ✓ |
| ★★ A2 | ★★ **重跑两个臂**（★ `--metal` ✓），★★ **判据**：★ AI 臂应显示**它自己的**主机读（★ 输入面组装读网格 ✓），★ 且数字与"每接收读一遍网格"（★ 612×14 RE ✓）**量级相符** ✓ |
| ★★ A3 | ★★ **解析值 vs 实测值对照**：★ 解析 ≈ **68 KB（complex64）/ 34 KB（bf16）每时隙** ✓ |
| ★ | ★ A4 | ★ 若审计改动过大 ⇒ **退路：直接给 `assemble()` 计时**（★ `OCUDU_CE_TIME` 同风格 ✓）|

### ★★★ B. R6：头 3/4 → 测量核

| ★ 步 | ★★ 内容 |
|---|---|
| ★★ B1 | ★★ **定接口**：★ 头 3（`ch_est`）/头 4（`dmrs_noise`）走 `pusch_processor_result_data` / `channel_state_information`，★ 还是走**估计器的 notifier** ✓ —— ★★ 推荐后者（★ 因为测量核已经在那里 ✓）|
| ★★★ B2 | ★★★ **头 3 必须乘回 `r`** 才能进测量核 ✓（★ 契约 (d)：★ 头 3 输出的是 `Ĥ/r` ✓）—— ★★ `r` 由组装器返回 ✓（★ 已在 `assemble()` 里算出来 ✓）|
| ★★ B3 | ★★ **头 4 的标度**（★ `σ² = 10^(A_d·u + B_d)` ✓）要与 R4.5 的 sidecar 对齐 ✓（★ 现在 sidecar 只写了 `s_llr` ✓）|
| ★★★ B4 | ★★★ **判据**：★★ **RSRP/EPRE/SNR/TA 的数量级必须与经典估计器一致** ✓ —— ★★ 否则**调度器读到错的信道质量，比读不到更糟** ✗ |

### ★★★ C. 采集新语料（★ 配方来自 §19.22，★ 是判据 1 的样本来源）

| ★ 维度 | ★★ 要求 |
|---|---|
| ★★ 分配宽度 | ★★ **≥43 PRB**（★ 现在模型已覆盖 4–51 ✓，★ 但优先宽 ✓）|
| ★★ MCS | ★★ **保守**（★ 首传就能译出 ✓）—— 语料里宽分配首传大多失败 ✗（★ 高吞吐=高 MCS ✓）|
| ★★ SNR | ★ 高（★ 24 dB 以上，★ 语料里无实测 ✓）|
| ★★ 调制 | ★ 256QAM 起步 ✓ |
| ★★ 标签 | ★★ **自标签**（★ 译码器 CRC-OK 时转储 TB ✓，★ **已实测可用** ✓：真实译码产出 77 个非零 TB ✓）|
| ★★ 规模 | ★★ **≥100 个可译采集**（★ 判据 1 是"逐采集一致"型 ✓）|

### ★★ D. 其它调制（★ 现在【数据不足】✗）

★★ 可用单元（★ `crc=OK` ∧ 连续 ∧ ≥43 PRB ✓）：★ **QPSK 24(cap001) / 0(cap002)** ✗、★ **64QAM 9 / 3** ✗、★ **16QAM 0** ✗
⇒ ★★ **必须等采集** ✓（★ 采集时把调制一起覆盖 ✓）。

---

## ⑤ ★★ 未决问题

| ★ | ★★ 问题 | ★★ 下一步 |
|---|---|---|
| ★★★ | ★★ **语料自带的 `_h.bin` / `_llr.bin` 来源与格式未解释** ✗（★ 相关 0.32 ✗、★ 大小差 27.6× ✗）| ★★ 查采集脚本/实时链配置 ✓（★ 查清前**不能当参照** ✓ F26）|
| ★★★ | ★★ **HARQ 串联【不合并】** ✗（★ 20 对全 KO ✓，★ 而实时译出 40 ✓）| ★★ 三处必要条件已修（★ 共用 processor ✓、★ 缓冲按 harq_id ✓、★ `reserve` 用真实 `new_data` ✓）★ 仍不合并 ⇒ ★ 继续查状态重置点 ✓ |
| ★★ | ★★ **TB dump 缺陷**（★ 94% 全零 ✗ ＋ 非零的也与链分段不符 ✗）| ★★ 修复后才能用 `--source real` ✓ |
| ★★ | ★★ **宽分配 rv=0 可译样本 = 0/10** ✗ | ★★ 只能靠新采集 ✓ |
| ★ | ★ **`MIN_PRB` 放到 4 的依据**是离线验证 + 35 个采集的链级读数 ✓ | ★★ 采集到位后**重新确定** ✓ |

---

## ⑥ ★★ 纪律（★ 本会话新增 F20–F28；★ F15–F19 见上一份 memo）

| # | ★★ 纪律 | ★★ 出处 |
|---|---|---|
| ★★★ **F20** | ★★★ **接缝的降级路径必须用【不受日志级别影响】的通道报告** ✗ | ★ PHY logger 没开 ⇒ 臂静默退回经典、输出与经典逐位相同 ✗ |
| ★★ **F21** | ★★ **比较类判据必须先确认【两侧都非退化】**（★ 一边数值趋 0 时"符号一致率"没有意义 ✗）| ★ 97.79% 那一栏 ✗ |
| ★★★ **F22** | ★★★ **AI vs 经典的任何对照必须先剔除【降级】接收**（★ 降级时 AI **就是**经典 ✓）| ★ 76 个里 47 个降级 ⇒ 差点得出相反结论 ✗ |
| ★★★ **F23** | ★★★ **报"性能劣化/不劣化"前必须确认样本在【精度包络】内** | ★ 32 个差距全在窄分配（★ 包络外 ✗）|
| ★★★ **F24** | ★★★ **"译码成功"必须由【非退化输出】证明，★ 不能只看 CRC** | ★ 全零字过 CRC-24A ✗ |
| ★★ **F25** | ★★ **"复现失败"与"修好失败"必须用【同一个判据】验收** | ★ `--harq-chain` 第一次跑"看起来通过"✗ |
| ★★ **F26** | ★★ **外部记录在能被解释之前【不是判据】** | ★ 语料 dump 的大小/相关性异常 ✗ |
| ★★★ **F27** | ★★★ **"位宽减半"≠"算力减半"** —— ★★ 先看**编译后的图**有没有解压算子 ✓ | ★ `constexpr_lut_to_dense` / `constexpr_affine_dequantize` ✓ |
| ★★★ **F28** | ★★★ **量化精度的判据是 coded BLER，★ 不是符号一致率** | ★ 97.9% 一致率 ↔ 0.91 BLER ✗ |
| ★★ 另 | ★★ **`depth3_engine_check` 是【绑定检查】，不是时延仪器**（★ 6 次采样给出 1780 µs ✗，★ 300 次是 953 µs ✓）| §19.28(b) |

---

## ⑦ 常用命令

```bash
cd /Users/jiachengwang/dev/ocudu
W=doc_chinese/llr_ai_detection/work_tmp ; P=lib/phy/upper/signal_processors/channel_estimator/metal/ai_train
PY=~/ai_ce_work/venv/bin/python

# ★★★ 训练（★ 空间化 + 两段制；★ `--init` 可续训）
$PY $P/train_depth3.py --train $W/data/datasets/d3_cap001_mixed.npz \
  --val $W/data/datasets/d3_cap002_mixed.npz --out $W/data/runs/<名> \
  --layout spatial --c-hid 64 --w-ch 0 --w-noise 0 --epochs 40 --lr 3e-4 --init <旧.keras>

# ★★★ 导出 + 编译 + 引擎检查（★ 三件一起做）
$PY $P/train_depth3.py --export-only $W/data/runs/<名>/depth3_best.keras --export-probe $W/probe/<名>.mlpackage
xcrun coremlcompiler compile $W/probe/<名>.mlpackage $W/probe/
./build/lib/phy/upper/signal_processors/channel_estimator/metal/depth3_engine_check $W/probe/<名>.mlmodelc 6 612

# ★★★ 离线三方对照（★ 网络 / genie / 完美 + 逐 SNR）
$PY $P/eval_depth3.py --model $W/data/runs/<名>/depth3_best.keras \
  --data $W/data/datasets/d3_cap002_narrow.npz --samples 231 --layout spatial

# ★★★ R4.5 标度校准（★ 平台 + 量化代价）
$PY $P/calibrate_depth3.py --model $W/data/runs/<名>/depth3_best.keras \
  --data $W/data/datasets/d3_cap002_rv0.npz --samples 96 --write-scales $W/probe

# ★★★ 链级判据 1（★ 非零 TB 判据 ✓；★ --list 用 §③ 的两个清单）
$PY $W/probe/compare_ai_arm.py --tool ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
  --corpus $W/corpus/aillr_cap00X --model $PWD/$W/probe/d3_mixed.mlmodelc --list <清单> --out <csv>

# ★★ ANE 探针（★ 机器必须空闲、★ 至少两遍 ✓；★ 300 次才可信 ✗ 6 次不可信）
cd $W/probe && ./probe <名>.mlpackage 612 300 && ./probe <名>.mlpackage 612 300

# ★★ 组装器 fixture（★ 首次或数据改动后重建）
$PY $W/probe/make_input_plane_fixture.py --out $W/tmp/input_plane_fixture.bin

# ★★ 提交纪律：★ `git commit -F <文件>`，★ 消息文件放 $W/tmp/（★ 已 git-ignore ✓）
```

---

## ⑧ ★★ 本会话的提交（★ 23 个，★ 关键节点）

| ★ commit | ★★ 内容 |
|---|---|
| `2e6df5948e` | ★★★ **空间契约落地 + GPU 问题的答案**（★ §19.1/§19.2）|
| `a54226b3cb` | ★★ 三方对照读到空间布局 + **一个"不生效的开关"**（★ `--snr-min` 被绕过 ✗）|
| `6b3b323fd7` | ★★ R5 的更正后计划（★ 接口已逐条查好 ✓）|
| `30242ae6ab` | ★★★ **全量训练：与 genie 只差 5%；★ 混合 SNR 训练是关键**（★ §19.10）|
| `b0324af7c7` | ★★★ **接缝输入面组装器；★ 与 Python 打包器逐位一致**（★ §19.11）|
| `f756d83ac4` | ★★ 两段制成立 + 产物在 ANE 上跑起来（★ §19.12）|
| `deaac58d42` | ★★★ **接缝切到网络：软比特由网络产出且能译码**（★ §19.14）|
| `5c0d15823b` | ★★ 证据审计：★ **更正"符号一致率"那一栏** ✗（★ §19.15）|
| `f69196716e` | ★★ 判据 1 在"正确的集合"上测 ✗ → ★ **后来被更正为包络越界** ✓ |
| `c10864c30c` | ★★★ **归因更正：那是包络越界；★ 包络内 40/40**（★ §19.17）|
| `476b369d6a` | ★★ 150 个采集"逐采集一致" → ★ **下一提交证明是全零字** ✗ |
| `c02bf27194` | ★★★ **判据同意的是"什么都没有"：`crc=OK` 是全零字**（★ §19.18 F24）|
| `bdcba898cb` / `dab025f8fc` | ★★ HARQ 串联 + ★ 它**卡在重放保真度**上（★ §19.19/§19.20）|
| `0c173b8ac4` | ★★ 语料 dump **不是**保真度金标准（★ §19.21 F26）|
| `ca3ec5db3e` | ★★★ **判据 1 在本语料上不可测 + 采集配方**（★ §19.22）|
| `934e4bd5ee` | ★★ **R4.5：量化代价 0.0000、平台 [5,20]**（★ §19.23）|
| `d0d195119f` | ★★ 第 1/2/3 项：★ 其它调制**数据不足** ✗ + I1/I7 证据 ✓（★ §19.24）|
| `f4d40399ae` | ★★ (a) **先把窄分配缺陷量化**（★ +30% ✗）+ 微调（★ §19.25）|
| `b96f3745ec` | ★★★ **INT8 不省算力的物证 + 它会破坏译码**（★ §19.26 F27/F28）|
| `763a550327` | ★★★ **(a) 成功：★ 窄 +30%→+3.7%、★ 宽未劣化**（★ §19.27）|
| `ee188cde40` | ★★★ **判据 1 的第一个有效读数：★ 35 个采集 31 ↔ 32**（★ §19.28，★ 数字经 §19.29 更正 ✓）|
| `46ba0e26a4` | ★★ R5.5：★ **仪器不覆盖本臂 ⇒ 记为未测** ✗（★ §19.28(e)）|
