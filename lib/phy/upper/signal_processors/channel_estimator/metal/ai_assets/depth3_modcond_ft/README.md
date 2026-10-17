# d3_modcond_ft — 调制条件化深度-3 接收机模型（N78 / 20 MHz / TDD / 30 kHz / 51 PRB）

一份 bundle 替代四套按调制模型：共享主干 + **一个共享上报头集合** + 按 Qm 条件化的宽主头。

## 契约（产物内自声明，臂与引擎都会读）

| 键 | 值 | 含义 |
|---|---|---|
| `depth3_layout` | `modcond` | 调制条件化契约（Qm 作为 4 个 one-hot 平面通道） |
| `depth3_qm_set` | `2,4,6,8` | 一个 bundle 服务四种调制 |
| `depth3_head_stride` | `8` | 主头每符号 8 个通道（bits ≥ Qm 位为 don't-care） |
| `depth3_snr_range` | `12.00,48.00` | 上报头训练区间（dB）；区间外整次回落经典并计数 |
| `depth3_nsc` | `612` | **平面宽度**；本臂装配固定 612，几何不符的产物在绑定时被拒绝 |

输入 `[1, 612, 14, 16]`（12 契约通道 + 4 个 Qm one-hot），输出 4 个头（llr / post_eq_noise / ch_est / dmrs_noise）。

## 训练

- 数据：`d3_modcond_train.npz`（12 000 样本，四调制混合）+ `d3_modcond_val.npz`；**数据集太大未入库**，重建方法见
  `ai_train/merge_depth3_datasets.py` 与 `build_depth3_dataset.py`。
- 主跑：40 epoch，`--modcond --layout spatial --blocks 18`，best val loss **0.00033**。
- 微调：`--w-noise 0.3 --lr 3e-4 --epochs 8`（把 head 4 的权重从默认 0.01 抬起，产出本目录的检查点）。

## 验证（详见 `doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md`）

- **采纳门**：四调制 × 300 捕获，与各调制专用模型**判决级 100% 相同**（1200/1200），零退化（§19.148/§19.154）。
- **上报头精度**：head 3 NMSE −25~−27 dB、功率比 ±0.1 dB；head 4 在 12~30 dB 误差 **+0.26~+1.22 dB**（§19.149）。
- **活网**：`net` 上报下 RSRP/EPRE 与独立估计器一致 ✓，但臂公布 SINR 系统性低 **−2.5 dB** ⇒ `net` 会让
  MCS 更保守、吞吐低 12.6%（§19.161）⇒ **默认仍是 `classic`**，`net` 待标定后才谈采纳。

## 重建

```bash
# 导出（会写入上面全部声明，含 depth3_nsc）
python3 ai_train/train_depth3.py --export-only depth3_best.keras --export-probe d3_modcond_ft.mlpackage \
  --modcond --layout spatial --modulation 256QAM --snr-range "12.00,48.00"
xcrun coremlcompiler compile d3_modcond_ft.mlpackage .
```
