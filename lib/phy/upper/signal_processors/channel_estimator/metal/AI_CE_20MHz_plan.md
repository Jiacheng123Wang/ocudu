# 20 MHz（n78 / 106 PRB）实机 HELENA CE 实施方案

> **⚠️ 重要勘误（2026-08-31，site6 实机数据证实）**：本方案标题的
> "20 MHz = 106 PRB"只对 **15 kHz SCS** 成立；实际 E2E 配置是 **30 kHz SCS**
> （n78 TDD 的 srsRAN 唯一支持组合——`lib/ran/band_helper.cpp` 的 SSB 表只有
> n78@30 kHz case C），20 MHz@30 kHz = **51 PRB**（srsRAN 带宽表
> `{MHz20, 106, 51, 24}` 第 2 列 = 30 kHz 取值）。因此**本实机系统的 PUSCH
> 授权永远 ≤51 PRB，106 桶（53–106 PRB）在本硬件上不会被触发**——所有实机
> 采集（site1..site6）的最大授权都是 51 PRB，与调度无关，是载波 numerology
> 的天花板。106 模型的合成训练/转换/入库仍然有效（为未来 40 MHz 载波或
> 15 kHz 重配做准备），但"实机 106 数据微调"在当前 B200 + n78@30 kHz +
> iPhone 组合上不可行。52 桶模型是当前实机系统的完整覆盖面。
> （B210 USB3 也无法承载 40 MHz 载波所需的 38+ MHz 采样率。）

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
- **G-3 20 MHz ZMQ E2E**：**通过（2026-08-30）**。接线完成（分桶分发 + 路径
  四件套 + `gnb_zmq_oaiue.yaml` expert_phy helena）。**C++ 零填充路径已验证**
  （30 PRB→52 桶 helena −13.25 vs mmse −12.32；60 PRB→106 桶 −13.04 vs −12.05）。
  **空闲唤醒无尖峰已证**（`--idle 25`：52 引擎 106 µs、106 引擎 163 µs）。
  **联合 E2E：UE attach 成功（IP 10.45.0.4），500 ping 0% 丢包（avg 74.6 ms）**。
  途中定位并修复 attach 死循环根因：NN 在高 SNR OOD 破坏输入（6 PRB：−45→−22 dB
  NMSE），SRB1 PUSCH/PUCCH 解调失败 → **SNR 门控（>25 dB classical bypass，
  commit `ac5226fd1f`）**；ZMQ 45 dB 下门控生效（1512 次 bypass），NN 不参与。
  剩余可选腿：UE `r=106` 验证 106 桶分派（门控下 NN 仍 bypass，仅验证分派路径；
  106 引擎的预测正确性由 head2head 合成集证明）。

  **gnb 侧预算探针（ZMQ E2E 停止时输出，802 个 UL 槽，r=51/门控全开）**：

  | 阶段 | mean | median | max | p95 |
  |---|---|---|---|---|
  | ul_pipeline（总） | 288.6 µs | 287.0 | 415.0 | 322.0 |
  | ul_time_frequency | 169.1 | 166.5 | 251.2 | 190.4 |
  | ul_channel_estimation | 72.0 | 70.2 | 121.1 | 99.4 |
  | ul_equalization_demod | 22.7 | 21.6 | 43.2 | 34.4 |
  | ul_ldpc_decode | 24.8 | 24.0 | 52.0 | 33.0 |

  **预算判定：总流水线 mean 288.6 µs / max 415 µs，1 ms slot 预算内留 ~2.4× 余量** ✓
  （此样本为 ping 流量、授权 ≤51 PRB 且 NN 被门控 bypass；106 全带宽授权的预算需
  r=106 高负载跑测或 G-4 实机补充）。

- **高 SNR 修复后的重跑（commit `31272e7d0d`，α-blend + −5..55 dB 重训）**：
  803 个 UL 槽，NN 实际预测 9 次（6/18 PRB 授权，α=0.04-0.19，predict 265-451 µs），
  attach ×2 成功、ping 流量 487 KB 正常。**之前打崩 attach 的 6-PRB 授权在
  α-blend 下全部解码成功**。预算：CE max 503.5 µs（含 NN）、pipeline max 729 µs、
  mean 284 µs——1 ms 预算内 ✓。注：ping 数据授权为 5 PRB（<6 PRB 阈值走
  classical，795/805 槽），NN 参与的是信令授权；高负载数据授权的 NN 预算待
  G-4 实机（真实信道下 α=1、全带宽授权）补充。
- **G-4 实机 A/B**：helena vs metal_mmse 双跑 shadow；真信道无 ground truth →
  用 BLER / HARQ 重传 / CQI-MCS / 吞吐 / ping 间接指标 + 保存 IQ/LS 网格离线
  分析（两估计器一致性 + TD 功率剖面合理性）。

  **执行清单（G-3 通过后）**：三次同条件实机跑（同一位置/时段，各 ~5 min）：
  `--pusch_channel_estimator_algo cpu | metal_mmse | helena`（配置
  `gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17` + gpsdo）。每轮收集：① UE 侧
  ping 网关 RTT/丢包（100 次）；② UE 侧 iperf3 上行吞吐（3 次取中位）；
  ③ gnb 日志 HARQ/CRC 计数与 `[ul_channel_estimation]` 探针；
  ④ `[helena_time]` 分布（`ENABLE_CE_TIME=ON` 构建；2026-09-11 起由编译期开关
  控制，原 `OCUDU_CE_TIME=1` 环境变量已迁移）。对比口径：同位置同频点，
  手机保持静止。G-5 数据原料（真信道 LS 网格 dump）的 gnb 侧采集钩子为下阶段
  工作，先以日志指标为主。

  **G-4 A/B 正式报告（2026-08-30，匹配条件 = OnePlus 8T + iPhone17 配置 + 3408.96
  MHz；手机侧 iperf 数字未取到，以下为 gnb 侧实测）**：

  | 腿 | attach | 首传 CRC OK | CE mean/max | 流水线 mean/max | UL 数据量 |
  |---|---|---|---|---|---|
  | cpu（classical） | ✓ | **90.7%**（50113/56244） | 25 µs / 132 µs | 222 µs / 513 µs | **195.5 MB** |
  | helena（α≈1 全力） | ✓ | 30.6%（15186/49584） | 221 µs / 836 µs | 411 µs / 1640 µs | 51.3 MB |
  | metal_mmse | ✗（3 次飞行模式全失败） | 6/166 | **1599 µs / 2682 µs** | — | — |

  吞吐补充（iperf3 服务端在 127，UE=OnePlus 8T）：**cpu 腿 16.4/16.9/16.5 Mbps
  （中位 16.5）**；**helena 腿（重跑）6.28/3.75/6.53 Mbps（中位 ~6.3）**——
  helena 的 UL 吞吐约为 classical 的 38%，与首传 CRC 30.6% vs 90.7% 互为印证。
  注：helena 各轮内吞吐呈前高后低衰减（前 10 s 8-15 Mbps → 尾部 1-5 Mbps）。

  结论：
  1. **实机当前冠军是 classical**（首传 90.7% + 16.5 Mbps UL）；helena 合成训练模型在真实信道+
     邻道干扰上劣化明显（30.6%，HARQ 补齐后数据全部送达，NN 以 α≈1 跑了 48417 次）；
  2. **metal_mmse 实机不可用**：GPU 首调 2.57 ms（无预热）+ 稳态 ~570-620 µs/授权，
     attach 时序窗口爆掉（cpu 的 CE 只要 25 µs）——是时延问题，先于解码质量问题；
     修复项：ctor GPU 预热 + 小授权快路径；
  3. 非匹配补充数据（仅参考）：OAI UE @3489.42 下 cpu 首传 98.6%；OAI UE 在
     3489.42 同频干扰下无法接入（同频干扰严重）；stock 配置（无 gpsdo）手机接入
     不稳定（RF 层问题）。所有实机 A/B 必须用 iPhone17 配置 + gpsdo；
  4. **helena 的实机差距 = 合成 TDL 分布与真实信道的分布差 + 干扰 OOD**——正是
     G-5 每站自适应要解决的：真实数据微调 + A/B 晋升。

  **首个实机 HELENA 跑测（2026-08-30，iPhone17 配置 + gpsdo）：attach 稳定、
  手机 ping 全程跑通**。NN 实机参与：606 次预测（52 桶，授权 ≤52 PRB），
  **α=0.90~1.00**（真机 SNR 25-28 dB，NN 全力区）；predict mean 338 µs /
  max 931 µs；CE 探针 mean 82.3 µs / max 627.7 µs；pipeline max 779 µs ✓
  预算内。UL 首传 CRC ~45% OK（HARQ 补齐，高 MCS 27 实信道属正常）。⚠️
  对照提醒：**stock 配置（无 gpsdo）下手机接入不稳定是 RF 层问题**（3408.96
  邻道干扰 + 时钟漂移），与 CE 算法无关——所有实机 A/B 必须用 iPhone17 配置。
  待补：metal_mmse 腿 + iperf3 吞吐三腿对比 + 106 桶实机（大上传触发）。
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
