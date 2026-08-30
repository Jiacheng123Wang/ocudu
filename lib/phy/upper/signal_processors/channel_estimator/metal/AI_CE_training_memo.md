# AI-CE 训练备忘（HELENA）

> 本文档是 AI 信道估计（HELENA）**训练侧**的独立 memo：环境、数据、流程、脚本、结果、
> 坑与教训——随 G2/Phase B 进展持续更新。架构与验收标准见同目录
> `AI_channel_estimation_implementation_plan.md`。
>
> 更新记录：2026-08-30 初版（G1 延迟原型 + G2 阶段 A 重训完成）。

## 1. 环境

| 项 | 值 |
|---|---|
| 机器 | M4 Pro，macOS 26.5（Darwin 25.5） |
| Python | **3.11**（3.14 无 wheel；`/opt/homebrew/bin/python3.11`） |
| venv | `/tmp/helena_venv`（`python3.11 -m venv`） |
| 依赖 | onnx 1.22 / onnxruntime 1.29 / coremltools 9.0 / tensorflow 2.21 / **tf-keras 2.21** / h5py |
| 训练设备 | **M4 Pro CPU**（0.116M 参数模型，25 s/epoch，无需 GPU） |

## 2. 资产获取

- **模型/代码**：官方仓库 `https://github.com/miguelhdo/HELENA_Channel_Estimation`
  （浅克隆到 `/tmp/helena_repo`）：`models/*.keras`（Keras-2 格式权重）+
  `code/output_files_paper/HELENA.onnx`（预导出）+ `code/eval_models.py`（仅评估，无训练脚本）。
- **数据集**：Zenodo `10.5281/zenodo.15210986`（记录 15210986），文件
  `290525_dataset_ce.mat`，**5,318,544,756 字节**（HDF5/MATLAB v7.3）。
  - 下载坑：Zenodo API 服务端**频繁断连**（curl 每次 ~180 MB 即断）。最终方案：
    `curl -L -C - --retry 3 --retry-all-errors` 的**断点续传循环**（20 次迭代拉满 5.3 GB）；
    aria2c 8 连接也断（protocol error），且 **`-o /tmp/xx` 被解析成 CWD 相对路径**，
    把 5.3 GB 写进了仓库 `tmp/` 并一度被 git add 收进 commit——已修正并 `tmp/` 入
    `.gitignore`。**教训：大数据集只放仓库外，git add 前先查 status。**
- 数据集内容：11,264 样本 × [612,14,2]（51 PRB × 14 符号复网格），SNR 0–36 dB，4 种
  信道 profile。HDF5 结构为 object-reference 间接寻址（见 §3 加载代码）。

## 3. 数据格式（重要发现）

- HDF5 键：`#refs#` / `training_data_CE`（后者是 6 个 object reference）。
- 前 5 个对象 = 网格数组 `(2,14,612,n)` → 转置 `(3,2,1,0)` 得 `[n,612,14,2]`；
  顺序：**trainData（模型输入）/ trainLabels（真值）/ trainPractical（作者实用 MMSE）/
  trainLinearInterpolation / trainLS**；第 6 个 = `(4,n)→(n,4)`：SNRdB / profileIdx /
  delaySpread / dopplerShift（**SNR 在列 3**，初版脚本误写成行 3 引发越界）。
- **模型输入 = LS 的线性插值网格**（实测 `|trainData − trainLinearInterpolation| = 0`），
  稠密（稀疏率 0），**不是稀疏 LS 网格**。输入范围 ±15.4、|X| 均值 0.655；标签 ±3.2、
  |Y| 均值 0.574——同尺度（无需归一化层），与 OCUDU 经典 LS+插值输出同构：
  部署直接复用现有 LS+插值阶段。
- 训练/验证/测试划分：作者按 70/15/15、random_state=42。

## 4. 训练流程

脚本（本仓库 `ai_train/`，或 /tmp 原型 `train_helena_g2.py`）：

1. 加载 HDF5（§3 的 object-reference 转置逻辑）；
2. `tf_keras.models.load_model(010625_HELENA_CE_model.keras)` 作初始化；
3. `Adam(learning_rate=3.2768e-4)`、`loss='mse'`、`metrics=['mae']`
   （超参取自 .keras 内 compile_config：作者原配置）；
4. `model.fit(train, batch_size=32, epochs=35, validation_data=val)`。

**M4 Pro CPU 实测**：247 step/epoch × ~95-104 ms = **~25 s/epoch**；35 epoch ≈ 13 min。
loss 曲线：epoch1 val_loss 0.203 → epoch5 0.063 → epoch35 0.0203（仍在缓降——
论文的 −16.78 dB 需要更多轮次逼近）。

## 5. 评估（G2 阶段 A 结果）

脚本 `eval_helena_g2.py`（per-SNR NMSE，`10log10(Σ|ĥ−h|²/Σ|h|²)`，逐样本）：

| 估计器（作者测试集，n=1690） | 总 NMSE | SNR 0-5 dB |
|---|---|---|
| LS | −0.15 dB | −0.15 |
| 线性插值 | −6.72 dB | −4.17 |
| PracticalMMSE（作者） | −14.27 dB | −12.71 |
| **HELENA（35 epoch 重训）** | **−15.03 dB** | **−13.68** |

**HELENA 超作者 PracticalMMSE +0.76 dB（低 SNR +1.0 dB）**——精度方向确认。
论文原报 −16.78 dB 未追平（欠轮次/原训练细节未知），Phase B 微调后再看。

## 6. 转换与部署（G1 延迟门禁）

脚本流程（`coremltools 9`）：

1. `tf_keras` 载入 .keras → `model.save(savedmodel, save_format='tf')`
   （coremltools 不认 tf_keras 对象，只认 SavedModel/.h5）；
2. `ct.convert(savedmodel, source='tensorflow', inputs=[TensorType(name='input_1',
   shape=(1,612,14,2), dtype=np.float32)], minimum_deployment_target=macOS15)`
   ——**必须固定输入形状**：flexible shape 版本运行时 **SIGBUS**；
3. 实测（batch 1，随机输入，200 次）：

| 计算单元 | p50 | p99 |
|---|---|---|
| CPU_ONLY | 693 µs | 908 |
| CPU_AND_GPU（MPS） | 690 µs | 747 |
| **CPU_AND_NE（ANE）** | **141 µs** | **208** |
| ANE + fp16 | 142 µs | 196 |
| ANE batch-4 | 794 µs 总（无摊销） | – |

**运行时 = Core ML（ANE）**：141 µs p50 优于 metal_mmse 基线（~255 µs）；
MPS/GPU 无收益（MHA 分解落 CPU）。重训后权重同架构，ANE 时延不变。

## 7. 坑与教训（持续更新）

1. **coremltools 9 移除了 ONNX 直转**（`source='onnx'` 报错）→ 走 TF 路线。
2. **TF 2.21/Keras 3 无法载入 Keras-2 格式 .keras**（`keras.src.engine` 反序列化失败）
   → 装 `tf-keras` 独立包。
3. **coremltools 不接受 tf_keras 对象**（类型检查 tf.keras.Model）→ 先存 SavedModel。
4. **flexible input shape → SIGBUS**：转换时必须给固定形状；batch 变化需单独转一版
   （固定 batch-4 模型才能跑 batch 4）。
5. **onnxruntime CoreML EP 的数字不可信**：155 节点只分区 36 个（MHA/Gather 落 CPU），
   ~1 ms 是"被分区污染的 CPU 数字"，不是 MPS/ANE 真实时延——以 coremltools 转换+
   直接 predict 为准。
6. **Zenodo 下载断连**：curl `-C -` 续传循环最稳；aria2c 也断且 `-o` 绝对路径会
   落 CWD（仓库 `tmp/` 事故）——大数据集只放仓库外。
7. **h5py object-reference 格式**：转置 `(3,2,1,0)` 才是 [n,612,14,2]；辅助标签
   `(n,4)` 的 SNR 在**列 3**。
8. **测量纪律**：Metal/ANE 时延测量必须在空载机器上进行（-j8 构建期间测出过 45×
   假回归——统一内存带宽被 CPU 挤占）。
9. **训练极快**：0.116M 参数 + 11k 样本，M4 Pro CPU 25 s/epoch——本项目的训练根本
   不需要 GPU/云资源。
10. **git 纪律**：commit 前必看 `git status`（5.3 GB 数据集误提交一次，已 amend）。

## 8. 脚本清单（本仓库 `lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/`）

| 文件 | 用途 |
|---|---|
| `train_helena.py` | HDF5 加载 + 划分 + 重训/微调（环境变量 HELENA_EPOCHS/DATA/MODEL） |
| `eval_helena.py` | per-SNR NMSE 评估（LS/线性插值/PracticalMMSE/HELENA 对照） |
| `gen_pusch_dataset.py` | Phase B：我方 PUSCH 合成集（TDL-A..E、DMRS {2,7,11} type1、LS+线性插值输入格式） |
| `convert_coreml.py` | SavedModel → Core ML（固定形状）+ ANE/GPU/CPU 时延基准 |
