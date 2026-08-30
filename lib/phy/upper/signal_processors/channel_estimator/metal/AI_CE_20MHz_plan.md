# 20 MHz（n78 / 106 PRB）实机 HELENA CE 实施方案

## 0. 现状盘点

- 已跑通：10 MHz ZMQ E2E（52-PRB HELENA，worker keep-alive 修复后无尖峰，
  full-BW predict 实测 350–570 µs，全链路 p95 ~0.7 ms < 1 ms slot 预算）。
- 已有实机资产（见 `configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17` 头注）：
  B200 + UHD、n78 TDD 20 MHz（dl_arfcn 627264 / 3408.96 MHz，gpsdo 时钟）、
  iPhone 17 可 attach、open5gs AMF 192.168.100.153。
- 已知空口问题：3408.96 MHz 紧邻 China Telecom 强载波（3415–3485）→ DL CQI 2、
  HARQ-ACK KO；干净备选 3518.4（634560）/ 3583.2（638880），但 iPhone 对 >3500
  频段无反应（机型扫描范围限制）。
- 与 10 MHz 的关键差异：106 PRB（1272 SC，NFFT 2048）；手机几乎不拿满带宽，
  授权是**任意宽度 + 任意 CRB**；真信道 / 真 CFO / 邻道干扰。

## 1. 架构决策

### 1.1 宽度分桶（bucket），而非逐宽度模型

架构宽度无关性已确认（`ai_train/helena_arch.py`）：conv 局部性 + 无位置编码的
内容型 MHA（axis=PRB），唯一宽度相关层是 `Reshape(prb,336)`；所有可训练权重
可跨宽度逐层迁移；网格是授权局部坐标 → CRB 偏移天然不变。

v1 两桶 + classical 兜底：

| 授权宽度 | 引擎 |
|---|---|
| n_subc ≤ 624（≤52 PRB） | 52 模型（已有 `helena_pusch52_ml.mlmodelc`） |
| 625–1272（53–106 PRB） | 106 模型（新训练，52 权重迁移初始化） |
| < 6 PRB（72 SC） | classical 兜底（LS 点太少，HELENA 无优势；阈值由 G-4 实测 A/B 数据定） |

- 填充：授权 < 桶宽 → 零填充到桶宽网格，输出只取有效区。
- 训练 pad-aware：随机宽度（1..桶宽）+ 零填充，NMSE 只在有效区统计。
- v2（按需）：加 12 / 25 PRB 桶，把小授权填充浪费（≤4×）压到 ≤2×。

### 1.2 延迟预算（R2 风险前置）

- 52 模型 E2E 实测 350–570 µs；106 模型 MHA 注意力矩阵 106²/52² ≈ 4.15×，
  conv 部分 ≈2× → 粗估 0.7–1.2 ms，**可能超 1 ms slot 预算**（20 MHz 下
  TF/LDPC 也各翻 2–4×）。
- **Day-1 探针**：`transfer_from_51(model_52, prb=106)` 直接转出 106 模型
  （零训练）→ 转 CoreML → 实测 ANE 延迟。一天内拿到预算答案，再决定：
  a) fp32 预算内 → 正常训练；
  b) 超 → fp16 量化（ANE 原生，通常 ~2× 加速）或层宽减半（conv 32→16 重训）
     或大授权继续 classical。

### 1.3 训练与转换

- 数据：`ai_train/gen_pusch_dataset.py --nfft 2048 --prb 106`
  （TDL-A/B/C/D + EPA/EVA/ETU、随机延迟/多普勒/SNR、随机宽度+零填充、
  CFO 抖动先取 ±50 Hz 量级对齐 B200+gpsdo 残差，G-4 实测后回填）。
- 迁移：`transfer_from_51(model_52, 106)`（函数已通用，按层名拷贝）。
- 转换：`convert_coreml.py` @106 → 固定 shape `(1,1272,14,2)` →
  `xcrun coremlcompiler` → `helena_pusch106_ml.mlmodelc`。
- 评估：3-way pooled NMSE 分宽度段（≤12 / 13–25 / 26–52 / 53–106），
  达标线对齐现网 −13.9 ~ −15.2 dB。

## 2. 运行时接线（gnb 侧改动）

- 参数四件套（复制 52 模式）：`OCUDU_HELENA_MODEL_PATH_106` 宏 +
  `factories` / `upper_phy_factories` / `du_low_config.{cli11_schema,yaml_writer,translator}`。
- `port_channel_estimator_helena_impl`：三引擎分发（52 / 106 / classical），
  `nn_in/nn_out` 扩到 1272×14×2（142.5 KB），桶内零填充 + 有效区裁剪，
  `[helena_time]` 加桶标识。worker keep-alive 随引擎自动生效，无新增工作。
- 配置：复用 `gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`
  （`channel_bandwidth_MHz: 20` 自动推导 106 PRB），
  `expert_phy --pusch_channel_estimator_algo helena`。

## 3. 阶段门控（G 门沿 AI 实施计划 §9 惯例）

- **G-1 空口链路加固**（不依赖新训练）：**已完成（用户实测确认）**——n78 20 MHz
  实机（B200 + iPhone 17）传统 CPU CE 路径 E2E 已跑通、无问题。空口风险已去，
  HELENA 实机 A/B（G-4）前置条件满足。
- **G-2 106 模型**：**已完成**。day-1 探针（ANE fp32 p50=191 µs，fp32 先行）；
  106 C++ 通路 3-way head2head（cpu −10.89 / metal_mmse −12.51 / 零训练迁移
  helena −13.87）；pad-aware 训练后：106 模型分宽度段 −16.97/−17.58/−17.60 dB
  （val −17.52），52 模型窄带 +2.5 dB（见训练备忘 §8）；两模型已转 CoreML 并
  提交 ai_assets（commit `0658b4bb6e`）。全宽 head2head 终值：52 → −13.69，
  106 → −13.77 dB（vs metal_mmse −12.5，增益 ~1.2 dB 维持）。
- **G-3 20 MHz ZMQ E2E**：接线完成（分桶分发 + 路径四件套 + `gnb_zmq_oaiue.yaml`
  已加 expert_phy helena）；待跑：OAI UE 侧 `r=106` 确认 → E2E（无尖峰 + 预算内）。
- **G-4 实机 A/B**：helena vs metal_mmse 双跑 shadow；真信道无 ground truth →
  用 BLER / HARQ 重传 / CQI-MCS / 吞吐 / ping 间接指标 + 保存 IQ/LS 网格离线
  分析（两估计器一致性 + TD 功率剖面合理性）。
- **G-5 每站自适应启动**：白天采真信道数据（LS 网格 + metal_mmse 教师输出），
  夜间蒸馏训练，A/B 晋升 + 热加载（沿 §9，真数据不再依赖合成信道）。

## 4. 风险清单

| # | 风险 | 前置措施 |
|---|---|---|
| R1 | 3408.96 邻强载波干扰（DL 已实测恶化） | 实机 E2E 已跑通（用户确认）；频点问题按需再调 |
| R2 | 106 模型 ANE 延迟超 1 ms 预算 | 探针 p50≈191 µs（空闲机下限）；G-3 实测 E2E 争用数；超则 fp16 / 裁剪 / 混合 classical |
| R3 | 真实授权宽度分布未知（桶边界、classical 阈值） | G-4 A/B 数据驱动；v2 增桶 |
| R4 | B200 残差 CFO 与训练数据失配 | gpsdo 已上；训练加 CFO 抖动；G-4 实测回填 |
| R5 | 手机行为不稳（attach / band 锁 / 功率） | 实机 E2E 已跑通，风险已去 |

## 5. 推进顺序（可并行）

- 用户侧：G-1 已完成（n78 20 MHz 实机 classical E2E 跑通）；汇合点 G-4。
- 我侧：G-2（day-1 探针完成 → pad-aware 训练 106）→ G-3（20 MHz ZMQ 接线）。
- 汇合：G-4（实机 HELENA A/B），随后 G-5。
