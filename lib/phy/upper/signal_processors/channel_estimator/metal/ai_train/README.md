# ai_train — HELENA 训练与在线自适应工具集

本目录包含 OCUDU HELENA AI 信道估计的全部训练/数据处理/转换工具，**自包含**
（不依赖其他的环境变量；脚本按相对本目录的路径工作）。
配套文档：`AI_CE_G5_manual.md`（G-5 每站在线自适应逐步操作手册）、
`AI_CE_training_memo.md`（训练/转换/坑）、`../AI_CE_20MHz_plan.md`（G-1..G-5 门控）。

## 代码结构

| 文件 | 用途 |
|---|---|
| `helena_arch.py` | HELENA 网络架构重建（宽度参数化）+ 跨宽度权重迁移（`transfer_from_51`） |
| `nr_ldpc.py` | 5G NR LDPC 编码器（BG1/BG2 完整基图、提升、双对角编码、H·c=0 自校验） |
| `dd_label.py` | 决策导向标签原语：CRC16/24A/24B、分段（38.212 §6.2.2）、NR 速率匹配（自然序循环缓冲 + rv 偏移 + Qm 符号粒度）、调制位交织、gold 加扰、调制映射、平滑 |
| `gen_pusch_dataset.py` | 合成 PUSCH 数据集生成（TDL-A..E 信道、DMRS {2,7,11}、LS+插值输入；`--pad-aware` 随机宽度、`--snr-min/--snr-max`） |
| `train_pad.py` | pad-aware 微调（元素级掩码损失、`--transfer/--init` 初始化、`--filter-snr-min`、`--snr-weight`） |
| `eval_pad.py` | pad-aware 测试集分宽度段 pooled NMSE |
| `pair_capture.py` | 采集三元组（tb/rx/ce）配对 → `pairs.npz`：每 TB 最多 K 个候选（新格式采集按 slot 精确配对，旧格式按时间最近排序） |
| `build_labels.py` | 端到端 DD 标签重建 + **候选内容验证**（对每个候选重编码重建标签，取"标签 vs 输入"最负者并过质量门）→ `labels.npz`（`--bucket 52\|106`、`--gate`、`--pairs`） |
| `run_g5_pipeline.sh` | 离线管线 sidecar：采集目录 → 配对 → 重建 → 划分 → 微调 → 转换（`VENV_PY=... run_g5_pipeline.sh <采集目录> --prb 52\|106`） |
| `init_models/` | 微调初始化的 SavedModel（`helena_pusch52_sm_hi` / `helena_pusch106_sm_hi`，合成基线）——`train_pad.py --init` 直接用，独立于 `~/ai_ce_work` |
| `convert_coreml.py` | SavedModel → Core ML（固定形状，`--shape`）+ ANE/GPU/CPU 时延基准（仅 macOS） |
| `train_helena.py` / `eval_helena.py` | 作者原始数据集的训练/评估（历史工具） |
| `probe106.py` | 52→106 零训练宽度迁移探针（延迟预算判定用） |

## 依赖

**Python（训练/数据处理，任意 OS）**：

```bash
python3 -m venv venv && source venv/bin/activate
pip install numpy tensorflow tf-keras h5py      # 训练与评估
```

**macOS 专属（模型转换，coremltools 仅支持 macOS）**：

```bash
pip install coremltools                        # SavedModel -> Core ML
# 编译 mlmodelc 用系统工具（无需 pip）：
xcrun coremlcompiler compile <model.mlpackage> <输出目录>
```

**运行时**：OCUDU gNB（本仓库，Apple Silicon + `-framework CoreML`）。

**快速自检**（验证本目录独立可工作，无需 `~/ai_ce_work`）：

```bash
python3 -m venv /tmp/ai_train_venv && source /tmp/ai_train_venv/bin/activate
pip install numpy
python dd_label.py                      # LDPC/CRC/调制原语自检
# 从任一采集目录跑配对+标签（只需 numpy）：
python pair_capture.py <采集目录> --out <采集目录>/pairs_v2.npz
python build_labels.py <采集目录> --pairs <采集目录>/pairs_v2.npz --out /tmp/labels.npz
```

（训练/转换再加 `pip install tensorflow tf-keras h5py coremltools`。）

## 开源参照（信息性，本目录代码不依赖它们）

- **HELENA 论文模型**：`https://github.com/miguelhdo/HELENA_Channel_Estimation.git`（原始 ViT/CE 模型，仅用于初始权重迁移的历史步骤；当前管线用本目录的 `helena_arch.py` 重建）。
- **5G NR LDPC 基图校验**：NVIDIA Sionna（`https://github.com/NVlabs/sionna`，
  其 `5G_bg1.csv`/`5G_bg2.csv` 与 3GPP TS 38.212 表逐位一致；`nr_ldpc.py`
  的基图据此转录并自校验）。
- **发送链约定对拍**：OAI UE（`https://gitlab.eurecom.fr/oai/openairinterface5g`，
  的 `nr_rate_matching.c`/`nr_gen_mod_table.c`）与 srsRAN/OCUDU 接收侧
  （本仓库 `lib/phy/upper/channel_coding/ldpc/ldpc_rate_dematcher_impl.cpp`）
  ——用于验证 `dd_label.py` 的速率匹配/调制约定，非构建依赖。

## 平台说明

- **gNB 运行时**：Apple Silicon macOS（Core ML/ANE 是 HELENA 的运行后端；
  此限制来自设计目标本身——见 `AI_CE_20MHz_plan.md` §0）。
- **训练/数据处理**：任意 OS（Ubuntu/CentOS 均可，仅需 numpy + TF）。
- **模型转换**：必须 macOS（coremltools 的限制）。
