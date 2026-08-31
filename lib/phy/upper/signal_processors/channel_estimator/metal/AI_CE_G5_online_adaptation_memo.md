# G-5 每站在线自适应：数据采集与处理全记录（memo）

> 状态：2026-08-31。覆盖从"为什么"到"怎么跑"的完整链路：设计动机 → 采集钩子 →
> 数据格式 → 档案 → 处理管线 → 当前卡点 → 下一步计划。与
> `AI_CE_training_memo.md`（训练/转换/坑）和 `AI_CE_20MHz_plan.md`（G-1..G-5 门控）
> 互补，本文件是 G-5 的主文档。

## 1. 设计动机（为什么要采真实数据）

G-4 实机 A/B（OnePlus 8T + iPhone17 配置 + 3408.96 MHz）的实测结论：

| 腿 | 首传 CRC OK | UL 吞吐（iperf 中位） |
|---|---|---|
| cpu（classical） | 90.7% | 16.5 Mbps |
| helena（合成训练，α≈1） | 30.6% | ~6.3 Mbps |
| metal_mmse | attach 失败（时延） | — |

**根因**：HELENA 用合成 TDL 信道（SNR −5..25 dB、无邻道干扰）训练；真实空口
（0~34 dB、3408.96 邻道强干扰、真实 CFO/时偏）是训练分布之外（OOD）。模型
在合成集上超 classical 1.2~2.6 dB，但在真实分布上劣化——**分布差只能靠本站
真实数据补**。这就是每站自适应的必要性（"粮草先行"：数据先备，夜间训练）。

**标签来源（决策导向 DD）**：真实信道没有 ground truth。对 **CRC 解码成功的槽**：
解码比特 → 重编码（CRC→LDPC→速率匹配→加扰→调制）→ 重建发射符号 X̂ →
`H = Y_rx / X̂`（每个数据 RE 的 LS）→ FD/TD 平滑 → 高质量标签。
设计取舍：不需要 UE 侧配合、不需要槽号（数据加扰 c_init 无槽号项；
DMRS RE 的标签由平滑补齐）、标签质量显著优于插值 LS。

**运行时安全（已入库）**：SNR 软混合 α-blend——输出 = 输入 + α·(NN−输入)，
α = clip((50−snr)/25, 0, 1)。高 SNR（>50 dB，如 ZMQ）NN 被完全阻尼，
训练包络内（≤25 dB）全权重。G-4 实测真机 α=0.65~1.00。

## 2. 采集钩子（gnb 侧，env 门控）

统一开关：`OCUDU_HELENA_DUMP_DIR` 环境变量（**必须在 `sudo` 之后**——macOS
sudo 的 env_reset 会过滤 sudo 前的变量，曾导致目录空采集）。

| 钩子 | 位置 | 输出 |
|---|---|---|
| CE 输入网格 | `port_channel_estimator_helena_impl.cpp`（NN 活跃槽，第一层） | `dump_<idx>_prb<N>.f32` + `meta.csv` |
| 接收网格 | `pusch_processor_impl.cpp`（解调前，每个 PUSCH 授权） | `rx_<idx>_prb<N>.f32` + `rx_meta.csv` |
| 解码 TB | `pusch_decoder_impl.cpp`（CRC-OK 槽） | `tb_<idx>_tbs<N>.bits` + `dd_meta.csv` |

关键 commit：钩子 v1 `9f69154317`；rx_meta 重编码配置扩展 `bdbae8b13a`
（+rv/new_data `06bb040c51`）。

### 采集操作手册

```bash
git pull && cmake --build build --target gnb -j8
mkdir -p ~/ai_ce_work/capture/site<N>_<MMDD>
sudo OCUDU_MMSE_TIME=1 OCUDU_HELENA_DUMP_DIR=/Users/jiachengwang/ai_ce_work/capture/site<N>_<MMDD> \
  ./build/apps/gnb/gnb -c configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17 \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena
# 手机 attach 后 iperf3 上行 30s×3（服务端在 127：iperf3 -s -B 10.45.0.1 -p 5201）
# 停 gnb（Ctrl-C）后校验：ls <目录> | wc -l
```

注意事项：iPhone17 配置（gpsdo + rx_gain 60 + debug 日志）是唯一可用的实机
配置（stock 配置无 gpsdo 会时钟漂移导致接入不稳）；3408.96 有邻道干扰，夜间
波动大，采集时的 CRC-OK 率 16~40% 属正常，**干扰样本本身就是稀缺训练料**。

## 3. 数据格式规范（全量）

### 3.1 meta.csv（CE 输入侧，每槽一行）

`idx, prb, snr_db, alpha, engine_nsc, t_us`
- `dump_<idx:08d>_prb<N>.f32`：NN 输入网格（classical 插值 LS），
  `[prb*12, 14, 2]` float32（子载波主序，re/im 交替）。
- `t_us` = steady-clock 微秒——**三组文件的配对键**。

### 3.2 rx_meta.csv（接收侧，每授权一行，15 列）

`idx, t_us, n_prb, n_syms, n_ports, k0, mod, dmrs_mask, n_id, n_scid,
scrambling_id, rnti, n_layers, rv, new_data`
- `mod` = srsRAN `modulation_scheme` 枚举 = **每符号比特数**（2=QPSK, 4=16QAM,
  6=64QAM, 8=256QAM——不是 1/2/3/4！曾踩坑）。
- `dmrs_mask` = 槽内 DMRS 符号位掩码（2180 = 符号 {2,7,11}）。
- `rx_<idx:08d>_prb<N>.f32`：解调前接收网格 RE，`[port][sym][sc]` 顺序、
  float32 (re,im) 对、sc 为授权局部子载波（k0 起）。

### 3.3 dd_meta.csv（解码侧，每 CRC-OK 槽一行）

`idx, t_us, tb_bits, nof_cbs`；`tb_<idx:08d>_tbs<N>.bits` = 解码 TB 的原始字节。

### 3.4 派生文件

- `pairs.npz`（`pair_capture.py`）：`tb_idx, rx_idx, ce_idx, n_prb`——按 t_us
  最近邻 + 宽度一致的紧配对（|dt|≤5 ms）。
- `labels.npz`（`build_labels.py`）：`X, Y, width`——X = CE 输入（零填充到
  52 桶宽 624）、Y = 平滑后的 DD 标签、width = 授权 PRB 数。可直接喂
  `train_pad.py`。

### 3.5 实测布局约定（从采集网格测量得出，非假设）

- 数据 RE **只在非 DMRS 符号**（全部 12 RE/PRB）；DMRS 符号只有导频
  （偶数子载波），奇数子载波实测 ~0（2 CDM groups without data 配置）。
- 发射顺序：符号主序、子载波升序；QPSK/64QAM 的 bit→符号映射按
  38.211 §5.1 标准。

## 4. 采集档案

| 站点 | 日期 | CE 输入 | rx | CRC-OK | 紧配对 | 链路条件 |
|---|---|---|---|---|---|---|
| site1_0831 | 08-31 | ~55k | —（钩子 v1，无 rx/tb） | — | — | 首次采集 |
| site2_0831 | 08-31 | 55,511 | 55,691 | 26,668 | ~9.8k | 6 列 rx_meta（无 mod，不可重建） |
| site3_0831 | 08-31 | 23,010 | 23,860 | 4,856 | 4,050 | 突发强干扰，iperf 111K~3.67M bps |
| site4_0831 | 08-31 | 58,777 | 59,058 | 10,622 | 9,832 | 3.3~3.7 Mbps，稍好 |
| site5_0831 | 08-31 | 46,108 | 46,473 | 9,964 | 9,355 | **15 列完整格式**，主料 |

**可重建标签的主数据集 = site5（9,355 三元组）**；site2-4 的 rx 缺 mod/rv 列，
仅可用于分布分析。合计可用训练三元组 ~23k（site3+4+5 的紧配对）。

## 5. 处理管线（代码清单，均在本仓库 `ai_train/` 或 `~/ai_ce_work/work/`）

| 文件 | 用途 | 状态 |
|---|---|---|
| `pair_capture.py` | t_us 配对 → pairs.npz | ✓ 跑通 |
| `nr_ldpc.py`（work 目录） | 5G NR LDPC 编码器（BG1/BG2 完整表 + 提升 + 双对角编码 + H·c=0 自校验；子代理交付，含 4 处规范纠正） | ✓ 自校验全过 |
| `dd_label.py` | CRC16/24A/24B、分段（38.212 §6.2.2）、32 列交织 + rv 速率匹配、gold 加扰、调制、平滑 | ✓ 合成闭环 −148 dB |
| `build_labels.py` | 端到端标签重建（配对→重编码→H=Y/X̂→CFO 每符号去旋转→桶宽填充）→ labels.npz | ⏳ 卡在真实流对拍（见 §6） |
| `train_pad.py` / `eval_pad.py` | pad-aware 微调 / 分宽度段评估（mask 损失） | ✓ 已有 |
| `convert_coreml.py` | SavedModel→CoreML + 时延基准 | ✓ 已有 |

## 6. 当前卡点（诚实现状）

标签重建链已与真实发射流对齐（子代理对拍 OAI UE 发送链 + srsRAN 解速率匹配器）：

- **根因**：NR LDPC 速率匹配是**自然序循环缓冲**（我误用了 LTE 32 列交织器）、
  缺调制位交织（38.212 §5.4.2.2）、单码块多加了 CRC24B、系统位窗口错。
- **证明**：`prove_reconstruction.py`（work 目录）三对样本吻合率 **1.00000**；
  全量构建 9,349/9,355 对成功，mod-2/4/6 标签完美（corr≈0.997，
  |X−Y|²/|Y|² ≈ −23 dB）。
- **剩余 bug 已修**（子代理第二轮）：位交织必须**逐码块**（非整码字）、E_r 必须
  **Qm 符号粒度**分配、k0 要在未压缩缓冲上算——多码块对 0.06→0.99+。
  256QAM 映射本身无误。pair 2963 类重传对（nd=0）的 tb/rx 存在**时间错位**
  （捕获数据问题，非代码），用质量门（<−10 dB）过滤。
- **首个真实数据微调完成（helena_pusch52_sm_real）**：2,069 真实标签，val
  −20.33 dB；合成集回归 −16.21（−0.8 dB 代价）；45 dB 恒等探针持平。
  已转 CoreML 入库 `ai_assets/helena_pusch52_real_ml.mlmodelc`。
- **首次 A/B 晋升结果（2026-08-31，背靠背同条件）——闭环验证成功**：

  | 腿 | 首传 CRC OK | iperf 吞吐中位 | UL 数据量 |
  |---|---|---|---|
  | incumbent | 39.3% | 8.61 Mbps | 100.9 MB |
  | **真实微调** | **69.6%（+30.3pp）** | **15.6 Mbps（+81%）** | 173.7 MB |
  | classical（G-4 参照） | 90.7% | 16.5 Mbps | — |

  结论：2,069 个真实标签的"最小可行微调"把 helena 的实机首传 CRC 从 39% 拉到
  70%、吞吐逼近 classical——**白天采数→夜间训练→次日晋升的闭环第一次转通**。
  剩余差距靠更多标签（256QAM 重传对错位修复后产量可翻数倍）+ 迭代收敛。

## 7. 下一步计划（对齐 §9 门控）

1. **完成标签重建**（子代理修复 → 三对验证 → 全量 site5 建标签）；
2. **真实数据微调**：labels.npz → `train_pad.py`（52 桶，从 helena_pusch52_sm_hi
   初始化，小 lr）→ 合成集 head2head 回归（不得劣化）→ 45 dB 探针（保持恒等）；
3. **转换 + 入库**：convert_coreml → ai_assets 替换（A/B 双模型并存时用
   `--pusch_channel_estimator_helena_model_path_52` 指定新模型路径）；
4. **A/B 晋升**：实机三腿复测（同 iPhone17 配置），指标 = 首传 CRC + iperf 吞吐；
   达标则热加载（`reload()` API 已就绪）；
5. **周期化**：白天采数 → 夜间训练 → 次日晋升（脚本化 sidecar）。
