# d3_modcond_c32h — 调制条件化深度-3 接收机模型（N78 / 20 MHz / TDD / 30 kHz / 51 PRB）

**本目录自 2026-10-19 起是默认模型**（`OCUDU_DEPTH3_DEFAULT_MODEL`）。它是 c32 宽度的重训版本，
一次补上三个缺口：DM-RS 2,7,11 的覆盖、64QAM 上报头的动态范围、以及声明的 SINR 校准。

## 契约（产物内自声明，臂与引擎都会读）

| 键 | 值 | 含义 |
|---|---|---|
| `depth3_layout` | `modcond` | 调制条件化契约（Qm 作为 4 个 one-hot 平面通道） |
| `depth3_qm_set` | `2,4,6,8` | 一个 bundle 服务四种调制 |
| `depth3_head_stride` | `8` | 主头每符号 8 个通道 |
| `depth3_snr_range` | `12.00,48.00` | 上报头训练区间（dB）；区间外整次回落经典并计数 |
| `depth3_nsc` | `612` | 平面宽度 |
| `depth3_sinr_cal` | `2:1.2678,-3.8298;4:1.0911,-1.3579;6:1.1251,-1.6222;8:1.2822,-4.0420` | 按 Qm 的公布 SINR 仿射校准 |

输入 `[1, 612, 14, 16]`（12 契约通道 + 4 个 Qm one-hot），输出四个头（llr / post_eq_noise / ch_est / dmrs_noise）。

## 训练

- 架构：`--modcond --layout spatial --blocks 18 --c-hid 32 --in-channels 12`，**26 284 参数**。
- 数据：`d3_dmrs2711h_train.npz`，**30 777 样本**，包含
  - 四种调制 × **两种 DM-RS 图案**（2,11 与 2,7,11）：2,7,11 上 64QAM 3 667、256QAM 1 680、16QAM 160、QPSK 144；
  - **高 SNR 补充**（26/30/34 dB）：64QAM 2 700、16QAM 900 —— 上一版 64QAM 只到 24 dB，上报头因此在
    24.4 dB 饱和（设计 §19.220(e)）。
- 40 epoch、`--w-noise 0.3`、best val loss **0.11016**。
- 校准：按 Qm 对**真值**拟合（合成样本自带 `snr_nominal` 与 `sigma2_over_r2`），残差 64QAM **1.89 dB**、
  16QAM 1.30 dB（上一版 4.24 dB，斜率 1.27 ⇒ 压缩而非偏置）。

## 验证（详见 `doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md` §19.220–19.225）

- **格子**：630 条首传，经典链 81.4% → 臂 **91.0%**；目标格 64QAM 5–8 PRB 87.4% → **96.4%（+28）**；
  **没有格子相对经典链回退**（代价记在 256QAM ≤4：81.0% → 73.8%）。
- **比特一致性**：99.88%（最差 99.58%，门限 99.5）；留出切片（80 条从未训练）**98.8%** vs 经典 91.2%。
- **上报量程**（对经典链，同接收）：64QAM sd 比 **0.95**、r **+0.92**；16QAM 1.02/+0.98；256QAM 0.78/+0.92。
- **校准后公布值**：64QAM 29.06 vs 经典 29.20 dB；256QAM 30.44 vs 30.96；16QAM 19.11 vs 19.66。
- **校准不动解出**：550 个接收、8 个格子，未校准与校准逐格相同。
- **活网 A/B**（六对相邻）：五对为正，均值 +27.8%；12 条腿逐腿重放，臂解出更好 11 条、公布值差
  ≤1.2 dB。
- **时延**：`forward` p50 **436–786 µs**（c64 是 1 764–1 931）；ANE 上 441–448 µs（c64 926–942）。

## 回滚

`depth3_modcond_ft` 仍在树里。回滚只需把 `apps/units/flexible_o_du/o_du_low/CMakeLists.txt` 的
`OCUDU_DEPTH3_DEFAULT_MODEL` 指回它并重新配置——注意它是 **CACHE** 变量，已有 build 目录需要
`cmake -U OCUDU_DEPTH3_DEFAULT_MODEL -S . -B build` 才会重新取默认值。
