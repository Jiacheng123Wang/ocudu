# G-5 每站在线自适应：数据采集与处理全记录（memo）

> 状态：2026-08-31。逐步操作命令见同目录 ai_train/AI_CE_G5_manual.md；代码结构与开源依赖见 ai_train/README.md。覆盖从"为什么"到"怎么跑"的完整链路：设计动机 → 采集钩子 →
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
sudo OCUDU_CE_TIME=1 OCUDU_HELENA_DUMP_DIR=/Users/jiachengwang/ai_ce_work/capture/site<N>_<MMDD> \
  ./build/apps/gnb/gnb -c configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17 \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena
# 手机 attach 后 iperf3 上行 30s×3（服务端在 127：iperf3 -s -B 10.45.0.1 -p 5201）
# 停 gnb（Ctrl-C）后校验：ls <目录> | wc -l
```

注意事项：iPhone17 配置（gpsdo + rx_gain 60 + debug 日志）是唯一可用的实机
配置（stock 配置无 gpsdo 会时钟漂移导致接入不稳）；3408.96 有邻道干扰，夜间
波动大，采集时的 CRC-OK 率 16~40% 属正常，**干扰样本本身就是稀缺训练料**。

## 3. 数据格式规范（全量）

### 3.1 meta.csv（CE 输入侧，每授权一行）

`idx, prb, snr_db, alpha, engine_nsc, t_us`
- `dump_<idx:08d>_prb<N>.f32`：layer-0 的 classical 插值 LS 输入网格，
  `[prb*12, 14, 2]` float32（子载波主序，re/im 交替）。
- **每个 PUSCH 授权都落盘**（2026-08-31 起）：prb<6、跳频、alpha=0 的回退槽
  也 dump，其 `engine_nsc=0` 标记 NN 未运行（旧钩子只在 NN 活跃槽 dump，这些
  槽的 TB 无法重建标签）。
- `t_us` = steady-clock 微秒——rx↔ce 同槽关联的配对键。

### 3.2 rx_meta.csv（接收侧，每授权一行，16 列）

`idx, t_us, n_prb, n_syms, n_ports, k0, mod, dmrs_mask, n_id, n_scid,
scrambling_id, rnti, n_layers, rv, new_data, slot`
- `mod` = srsRAN `modulation_scheme` 枚举 = **每符号比特数**（2=QPSK, 4=16QAM,
  6=64QAM, 8=256QAM——不是 1/2/3/4！曾踩坑）。
- `dmrs_mask` = 槽内 DMRS 符号位掩码（2180 = 符号 {2,7,11}）。
- `slot`（2026-08-31 起）= `slot_point::system_slot()`——与 dd_meta 的 slot 列
  精确配对（单 UE 采集），免疫异步解码的时延抖动。
- `rx_<idx:08d>_prb<N>.f32`：解调前接收网格 RE，`[sym][port][sc]` 顺序、
  float32 (re,im) 对、sc 为授权局部子载波（k0 起）。

### 3.3 dd_meta.csv（解码侧，每 CRC-OK 槽一行）

`idx, t_us, tb_bits, nof_cbs, slot`；`tb_<idx:08d>_tbs<N>.bits` = 解码 TB 的
原始字节。`slot` = 该 TB 解码所在授权的系统槽号（2026-08-31 起）。

### 3.4 派生文件

- `pairs.npz`（`pair_capture.py`）：`tb_idx, rx_idx, ce_idx, n_prb`，形状
  `(N,K)`——每 TB 最多 K=4 个候选（有 slot 列时按 slot 精确配对优先，其余按
  时间最近补齐；旧格式按时间最近）。**配对只出候选，内容验证在
  build_labels 做**（见 §6 的 529 µs 根因）。
- `labels.npz`（`build_labels.py`）：`X, Y, width, snr_train, rank`——X = CE
  输入（零填充到桶宽 52×12=624 或 `--bucket 106` 的 1272）、Y = 平滑后的 DD
  标签、rank = 被接受的候选序（0=时间最近）。**已按质量门（<-10 dB）过滤**。

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
| site6_0831 | 08-31 | 75,183 | 75,183 | 42,853 | 42,720 | **v2 钩子（16 列 rx_meta + 5 列 dd_meta + 全授权 dump）**；CE↔rx 1:1；slot 精确配对 38,138（89%）；engine_nsc=0 回退槽 314；256QAM 37.5k 授权；OnePlus 8T 全缓冲 UL |

**可重建标签的主数据集 = site5（9,355 三元组）**；site2-4 的 rx 缺 mod/rv 列，
仅可用于分布分析。合计可用训练三元组 ~23k（site3+4+5 的紧配对）。
2026-08-31 配对修复后 site5 重建得 **7,884 个过门标签**（labels_v2.npz，
见 §6 第 4 点）。site6 是首个 v2 格式采集：全授权 dump 使 CE↔rx 计数 1:1，
slot 列使 89% 的 TB 免于时间配对（剩余走内容验证候选）。

## 5. 处理管线（代码清单，均在本仓库 `ai_train/` 或 `~/ai_ce_work/work/`）

| 文件 | 用途 | 状态 |
|---|---|---|
| `pair_capture.py` | 每 TB 多候选配对（slot 精确优先 + 时间最近补齐）→ pairs.npz | ✓ 跑通 |
| `nr_ldpc.py`（work 目录） | 5G NR LDPC 编码器（BG1/BG2 完整表 + 提升 + 双对角编码 + H·c=0 自校验；子代理交付，含 4 处规范纠正） | ✓ 自校验全过 |
| `dd_label.py` | CRC16/24A/24B、分段（38.212 §6.2.2）、32 列交织 + rv 速率匹配、gold 加扰、调制、平滑 | ✓ 合成闭环 −148 dB |
| `build_labels.py` | 端到端标签重建 + **候选内容验证**（每个候选重编码重建标签，"标签 vs 输入"最负者胜 + <-10 dB 质量门）→ labels.npz | ✓ 跑通（见 §6） |
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

- **配对错位根因与修复（2026-08-31，已入库 commit `b85690c857`）**：

  1. **根因定位**（site5 逐对分析）：TB 落盘发生在 LDPC 解码**完成**时（比授权
     晚 0.2–3 个时隙，p50≈481 µs）；当相邻时隙都有授权时，"时间最近"会把
     **下一个授权**的网格误配给该 TB（pair 2963 类的 529 µs 错位），重编码后
     标签是噪声（"标签 vs 输入"≈0 dB），被质量门正确拒收但白白损失标签。
     site5 有 9,522/9,964 个 TB 的 ±4 ms 窗口内有 ≥2 个授权网格——误配面巨大。
  2. **离线修复**（对旧采集有效）：`pair_capture.py` 每 TB 出 K=4 候选；
     `build_labels.py` 对每个候选重编码重建标签，取"标签 vs 输入"最负者
     （错配候选 ≈0 dB，真候选 ≈−20 dB，天然分离），过 <-10 dB 门才收。
  3. **采集侧根治**（对后续采集）：
     - rx_meta/dd_meta 增 slot 列 → 配对按 slot 精确（不再依赖时戳）；
     - CE 输入 dump 扩到**每个授权**（prb<6/跳频/alpha=0 回退槽也有，
       engine_nsc=0 标记）——旧钩子这些槽没有 X，其 TB 无法重建标签；
       这是 site5 残留损失的主因（prb<6 与跳频授权约 1/4）。
  4. **site5 重建结果（labels_v2，2026-08-31）**：9,964 TB → **7,884 个过门标签
     （基线 2,298 → 3.4×）**；接受候选序直方图 {0:2,467, 1:2,274, 2:3,062,
     3:81}——**5,417 个（69%）经内容验证从非最近候选救回**，且各 rank 的
     "标签 vs 输入"中位一致 ≈−20 dB（rank 0 p50 −19.9 / rank 2 p50 −20.1），
     验证本身无偏。残余损失 2,080+81 = 旧钩子 prb<6/跳频槽无 CE 输入
     （新钩子已根治）+ 少量构建失败。宽度分布 51(3,173)/43(1,933)/48(1,096)/
     25(827)/50/44/46，SNR 20.2–30.2 dB（p50 26.1）——52 桶二次微调的原料。

- **OAI UE + helena 接入失败根因（2026-08-31，证据链完整）**：
  - 现象：cpu CE 可接入；helena 在首个 NN 数据授权后连续 45 次解码失败
    （18 PRB，SNR 6–11 dB，alpha=1，Msg3=4 PRB 走 classical 已通过）；
    nopath 腿 UE 连 SSB 都没同步（RF 抖动，无 PUSCH——该实验被 RF 条件作废）。
  - 定位：dump 时间戳 → helena 阶段 307–542 µs/授权（时序偏紧但非主因）；
    **主因 = 输入尺度 OOD**：OAI UE 的 UL 数字电平 |X|≈0.003（rx_gain 40 +
    UE 功率），比模型训练尺度（手机采集 |X|≈0.10–0.25）低 ~34 dB。离线把
    失败授权的 dump 喂给部署模型：**输出放大输入 2.2×，|NN−X|²/|NN|² =
    −0.5 dB（破坏性）**；按活动区 RMS 归一化到训练尺度后 = −14.9 dB、
    增益 0.94（正常去噪）。
  - 修复（commit `5b82284025`）：helena impl 在 NN 前把活动区 RMS 归一化到
    `kHelenaInputRms=0.15`、predict 后还原（G-5 dump 保持原始尺度不变）；
    `gnb_uhd_oaiue.yaml` rx_gain 40→60。手机路径在 0.15 尺度附近等效无变化
    （增益 ≈1），无回归风险。
  - **重测实机验证（2026-08-31）**：UE 接入成功，100 ping + iperf3 ×3
    （−w 256K/128K/无 w）全部跑通。**首传 CRC OK 率 90.5%**（135,034 授权，
    nd=1 OK 122,271；nd=0 重传率 11.7%）——同机参照：classical 90.7%，
    helena 修复前 0/45。8 小时采集 152,874 授权 / 134,181 CRC-OK TB（17 GB），
    是低 SNR（12–20 dB）真实数据的新料源，已进入配对→重建（→real4）。
  - 教训：**NN 部署必须做输入尺度归一化**——数字电平随 rx_gain/UE 功率
    变化可达 ~40 dB，模型只在训练尺度内有效；这类故障（低电平+低 SNR 时
    alpha=1 全权信任 NN）在合成闭环与高 SNR 实机上都测不出来。

- **106 桶实机天花板（2026-08-31，site6 实机证实）**："20 MHz = 106 PRB"只对
  15 kHz SCS 成立；本系统 20 MHz@**30 kHz = 51 PRB**（srsRAN 带宽表
  `{MHz20,106,51,24}`；band_helper 只有 n78@30 kHz 的 SSB 配置，15 kHz 无法
  配置）。因此 PUSCH 授权 ≤51 PRB 是载波 numerology 的天花板，**53–106 桶在
  本硬件（B200 + n78@30 kHz + iPhone）永不被触发**——site1..6 最大授权全部
  51 PRB 即此因，与调度/流量无关。106 模型的合成训练/入库仍有效（为 40 MHz
  载波或 15 kHz 重配备用）；"实机 106 数据微调"在现硬件上不可行。详见
  `AI_CE_20MHz_plan.md` 顶部勘误。

- **52 模型二次微调（helena_pusch52_sm_real2，7,884 标签 → 7,096 训练 /
  788 验证，2026-08-31）**：val −19.37 dB（DD 标签噪声底 ~−19.5）。三模型
  同集对照（合成 pusch52pad_hi / 真实 holdout）：

  | 模型 | 合成 pooled | 真实 holdout | 45 dB 探针 |
  |---|---|---|---|
  | init（sm_hi，合成基线） | −19.93 | −18.49 | −24.6..−35.5 |
  | real（2,069 标签） | −19.16 | −19.39 | −28.1..−34.5 |
  | **real2（7,096 标签）** | −18.71 | **−19.48** | −27.4..−33.9 |

  结论：real2 在真实 holdout 上 +0.99 dB vs init、与 real 双双贴标签噪声底
  （离线无法再分辨）；合成集 −1.22 dB 代价（"真实分布越强、合成越弱"）。
  3.4× 标签的宽度/SNR 覆盖差异只能由实机 A/B 裁决。已转 CoreML 入库
  `ai_assets/helena_pusch52_real2_ml.mlmodelc` 作 A/B 第三腿候选。

- **52 模型三次微调（real3，2026-08-31）**：site5+site6 合并 28,693 训练 /
  3,188 验证（site6 重建得 23,997 个过门标签，slot 精确配对 rank{0:23995,
  1:2}）。同一 s56 holdout 四模型对照：init −17.75 / real(2,069) −18.65 /
  real2(7,096) −18.74 / **real3(28,693) −18.84**（+1.09 dB vs init，最佳）；
  合成回归 −17.99（−1.94 dB vs init——真实标签越多合成越弱，趋势延续）；
  45 dB 探针 −25.1/−29.8/−33.5（无高 SNR 破坏）。ANE p50 171 µs。入库
  `ai_assets/helena_pusch52_real3_ml.mlmodelc`，作 A/B 主候选（对
  incumbent + real2 三腿）。

- **real3 实机 A/B 回退（2026-08-31 晚，背靠背）**：A 腿 incumbent **81.1%**
  （180,115 OK/41,872 KO）vs B 腿 real3 **68.0%**（148,700 OK/69,982 KO）——
  **real3 实机回退 13 pp，不晋升**。两点观察：① incumbent 相对首轮 A/B 的
  39.3% 大涨（输入归一化修复+当晚环境），归一化对合成模型实机有效；
  ② real3 在原始尺度（0.1–0.67）训练，运行时喂 0.15 归一化输入——尺度错配
  （离线 norm 评估 −18.84→−15.52）。**条件不对等加重了结论**：两腿 SINR
  中位 12.5 vs 21.4 dB（B 腿信号好 9 dB 仍输 13 pp）——real3 的真实退化比
  表面数字更严重。教训：**训练/评估必须镜像运行时的输入归一化**
  （train_pad/eval_pad 已加 --norm）；**A/B 必须记录每腿 SINR 分布做可比性
  校验**（新增 `ab_report.py`）。解法：real3n（s56 + --norm 0.15）→ real4
  （全量 134k OAI UE 标签 + --norm 0.15）。DD 标签噪声底（p50 −15.7 dB）
  已无法离线分辨模型优劣，实机 A/B 是唯一裁决。

## 7. 下一步计划（对齐 §9 门控）

1. ~~**site5 标签重建**~~ ✅ 7,884 个过门标签（3.4×，见 §6）；
2. **52 模型二次微调（进行中）**：realtrain_v2（7,096 训练 / 788 验证）→
   `train_pad.py`（从 `init_models/helena_pusch52_sm_hi`，lr 1e-5，6 ep）→
   合成集 head2head 回归（不得劣化）→ 45 dB 探针（保持恒等）→ 转换入库
   `helena_pusch52_real2.mlmodelc` → 实机 A/B（若显著优于 helena_pusch52_real）；
3. **106 模型全套（受 §6 天花板约束，待用户决策）**：现硬件 20 MHz@30 kHz
   = 51 PRB，53–106 桶不会被触发。可行分支：(a) 接受现实——106 模型保持合成
   版备用，实机闭环收敛到 52 桶（site6 数据仍可增强 52 微调）；(b) 未来换
   40 MHz 载波的无线电（B210 不行）+ 干净频段再重启 106 实机流。
   若走 (a)：site6 → 配对/重建（52 桶）→ 52 real3 微调 → A/B。
4. **周期化**：白天采数 → 夜间训练 → 次日晋升（脚本化 sidecar）。
