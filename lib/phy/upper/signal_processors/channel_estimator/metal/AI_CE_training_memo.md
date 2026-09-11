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

## 7. 工作目录（2026-08-30 起固定，不再使用 /tmp）

**工作目录：`/Users/jiachengwang/ai_ce_work/`**（OneDrive 之外；/tmp 重启不可靠，
已全部迁入，以后训练/转换/对拍一律在本目录下进行）：

```
/Users/jiachengwang/ai_ce_work/
├── dataset/   290525_dataset_ce.mat(5.0G) pusch_ce_dataset.npz(2.1G)
├── models/    helena_g2_savedmodel(作者集 35ep) helena_pusch_sm(PUSCH 10ep)
│              helena_{fixed,f16,g2,b4}_ml.mlpackage(Core ML 转换)
├── repo/      helena_repo/(官方仓库 102M)
├── venv/      python3.11 虚拟环境(tf-keras/coremltools/onnxruntime/h5py)
└── work/      R_test.npy Y_test.npy snr_test.npy 等中间产物
```

- 脚本默认路径指向本目录（`AI_CE_WORK` 环境变量可覆盖，默认 `~/ai_ce_work`）；
- 训练/评估代码在 git 仓库 `lib/phy/upper/signal_processors/channel_estimator/
  metal/ai_train/`（已入库，随仓库备份）。

## 8. 当前状态与续接点（重启后从这里继续）

- **已完成**：G1（ANE 141 µs 定案）；G2-A（作者集 35ep 重训 −15.03 dB）；Phase B
  微调（PUSCH 10ep）→ **HELENA −20.91 dB**（输入 −11.42 dB，+9.5 dB）；
  C++ 头对头工具已入库并**修复两处 harness bug**（见 §9 第 13/14 条）。
- **G2 对拍定案（修正版：统一 pooled 聚合，2026-08-30）**：

| 估计器 | NMSE（pooled，同一 4000 样本测试集） |
|---|---|
| C++ 输入（srsRAN 插值）/ numpy 输入 | −4.61 / −5.59 dB |
| cpu-average | −10.95 dB |
| metal_mmse（GPU 与 CPU 回退逐位一致） | −12.62 dB |
| **HELENA（C++ 集成路径，Phase C 再微调）** | **−15.20 dB** |
| HELENA（numpy 输入参考上限） | −17.11 dB |

  **HELENA 超 metal_mmse +2.6 dB（pooled）**——G2 精度门禁通过。⚠️ 修正说明：
  早前"−20.86 vs −12.62 = +8.2 dB"是**聚合口径混用**（Python eval 用逐样本
  dB 的均值，C++ bench 用合并误差功率的 dB；SNR 跨 30 dB 时两者差 ~6 dB）。
  统一 pooled 口径后真实增益为 +2.6 dB；Phase C 已把 C++/numpy 输入语义差
  压缩到 1.9 dB（−15.20 vs −17.11 上限）。metal_mmse −12.6 与单测 veha
  口径 −12.7 交叉验证 ✓。
- **系统集成里程碑（2026-08-30，Phase C 完成）**：`ocudu_coreml_nn_engine`
  （Core ML/ANE，零拷贝 MLMultiArray + outputBackings）+ `port_channel_estimator_helena_impl`
  （经典前级 → NN 网格 → 回退 + 热重载 API）已入库入链；bench 带 `--dump`
  模式（导出 C++ 链真实输入网格）用于输入对齐再微调。Phase C（40k C++ 输入
  再微调 10ep）后 C++ 路径 **−15.20 dB**（pooled），超 metal_mmse +2.6 dB。
- **下一步**：工厂/配置贯通（`pusch_channel_estimator_algorithm: helena`）→
  A/B 阴影模式与探针 → E2E 三腿门禁（G3-G5）。
- **20 MHz（106 PRB）pad-aware 训练（2026-08-30，G-2 完成）**：手机任意宽度
  授权 → 分桶（≤52 PRB → 52 模型，53–106 → 106 模型，<6 → classical）+ 桶内
  零填充。宽度无关架构（无位置编码的内容 MHA）使 52→106 **零训练迁移**即达
  全宽 −13.87 dB（追平 52 训练模型）；106 pad-aware 微调 8ep 后分宽度段
  −16.97/−17.58/−17.60 dB（val −17.52）。52 模型补 pad-aware 微调后，与旧
  全宽模型的同域对照（6-12/13-25/26-52 PRB）：−15.70/−16.76/−17.23 vs
  −13.16/−15.30/−16.49——窄带 +2.5 dB，全宽仅 −0.2 dB。详见
  `AI_CE_20MHz_plan.md`。
- **G-4 实机 A/B（2026-08-30，OnePlus 8T + iPhone17 配置 + 3408.96）**：
  cpu 首传 CRC 90.7%（CE 25 µs）> helena 30.6%（NN α≈1 跑 48417 次，HARQ 补齐
  51.3 MB）> metal_mmse attach 失败（GPU 首调 2.57 ms + 稳态 ~580 µs/授权，
  时序爆掉）。**合成训练模型在真实信道+邻道干扰上劣于 classical**——合成↔真实
  分布差 + 干扰 OOD，G-5 每站自适应的必要性被实机数据坐实。
- **G-5 数据采集钩子 v1（已入库，commit `9f69154317`）**：
  `OCUDU_HELENA_DUMP_DIR` 环境变量 → 每个 NN 活跃槽把 NN 输入网格（classical
  插值 LS，[prb*12,14,2] float32）落盘为 `<dir>/dump_<idx>_prb<N>.f32` +
  `<dir>/meta.csv`（idx,prb,snr_db,alpha,engine_nsc）。采集方式：先建目录，变量必须在 `sudo` **之后**（macOS sudo 的 env_reset 会过滤 sudo 前的变量——踩过坑：目录空）：`sudo OCUDU_HELENA_DUMP_DIR=<绝对路径> ./build/apps/gnb/gnb ...`。
  （2026-09-11 注：`[helena_time]` 打印已由 `ENABLE_CE_TIME=ON` 编译期开关控制，
  运行命令不再传 `OCUDU_CE_TIME=1`。）
  **下一步**：UL-SCH 的 CRC-OK 决策导向标签钩子（重编码→重调制→H=Y/X̂），
  与 dump 配对成真实信道训练集。
- **首批真实采集（site1_0831，2026-08-31）**：55,064 个 NN 活跃槽。宽度 25 PRB(18,077)/51 PRB(18,261)/43(7,099)/46(6,267)/50(3,676) + 6-24 PRB 长尾；SNR -2.1~33.9 dB（中位 24.7）；alpha 0.65~1.00（32,299 槽 α=1.00）；全部 52 桶。格式校验通过（prb×12×14×2 float32，幅值 0.10-0.25）。等待 CRC-OK 标签钩子配对。
- **标签侧钩子（已入库）**：同目录追加三组文件，以 steady-clock 微秒时间戳为
  配对键：`rx_<idx>_prb<N>.f32`（接收网格，[port][symbol][re] float32 re,im 对，
  解调前）+ `rx_meta.csv`（idx,t_us,n_prb,n_syms,n_ports,k0）；
  `tb_<idx>_tbs<N>.bits`（CRC-OK 槽的解码 TB 字节）+ `dd_meta.csv`
  （idx,t_us,tb_bits,nof_cbs）；CE 侧 meta.csv 新增第 6 列 t_us。
- **site3_0831（2026-08-31 凌晨，突发强干扰时段）**：23,010 CE 输入 + 23,860 rx
  + 4,856 CRC-OK 标签（13 列新 rx_meta）。iperf 三跑 111K/297K/3.67M bps——突发
  邻道干扰导致 42 次掉线重连；数据含干扰条件下的真实分布（CRC-OK 率 ~20%），
  是训练鲁棒性的稀缺样本。
- **site4_0831（链路稍好）**：58,777 CE 输入 + 59,058 rx + 10,622 CRC-OK 标签，
  配对 9,832 个紧三元组（92.6%，dt p50 386/579 µs）；iperf 三跑 3.70/3.29/3.57
  Mbps（干扰仍存）。site3+site4 合计 ~13.9k 训练三元组，足够微调。
- **site5_0831（15 列 rx_meta，含 rv/new_data）**：46,108 CE + 46,473 rx + 9,964
  TB，配对 9,355 个紧三元组（93.9%）。site3+4+5 合计 ~23k 三元组。
  离线 sidecar（下一步）：按 t_us 配对 → 重编码/重调制 → H=Y/X̂ → 平滑 →
  与 CE 输入网格组成真实信道训练集。
- **配对错位修复 + 采集钩子 v2（2026-08-31，commit `b85690c857`）**：
  - 根因：TB dump 发生在异步 LDPC 解码完成时（比授权晚 0.2–3 槽）；相邻时隙
    都有授权时按时间最近配对会抓到**下一个授权**的网格（529 µs 错位），标签
    变噪声被质量门拒收。site5 里 9,522/9,964 个 TB 的 ±4 ms 窗口有 ≥2 个网格。
  - 修复（离线，旧数据可用）：`pair_capture.py` 每 TB 出 K=4 候选 +
    `build_labels.py` 逐候选重编码**内容验证**（错配候选"标签 vs 输入"≈0 dB，
    真候选 ≈−20 dB），首过门者收。
  - 采集侧根治：rx_meta/dd_meta 增 slot 列（按 slot 精确配对）；CE 输入 dump
    扩到**每个授权**（prb<6/跳频/alpha=0 回退槽 engine_nsc=0——旧钩子这些槽
    无 X，TB 无法重建，是 site5 残留损失主因）。
  - ai_train 独立化：去掉全部 `~/ai_ce_work`/OneDrive 硬编码路径；
    `init_models/` 存档 52/106 合成基线 SavedModel（微调 --init 直接用）；
    已在全新 venv（仅 numpy）下用采集样本端到端验证（配对→重建→96 标签，
    rank{0:64,1:20,2:12}——1/3 经内容验证从非最近候选救回）。

> G-5（每站在线自适应：数据采集/格式/档案/管线/计划）的完整主文档见
> `AI_CE_G5_online_adaptation_memo.md`——本文件保留训练/转换/坑的通用部分。

## 9. 坑与教训（持续更新）

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
11. **孤儿 GPU kernel（2026-08-30 实锤）**：旧二进制 + 新 metallib 错配（旧参数表
    绑定 + 新 kernel 签名）可让 kernel 读垃圾边界进入死循环；进程被 SIGTERM 杀掉后
    **kernel 仍留在 GPU 上无限执行**（Apple GPU 无看门狗）——powermetrics 显示
    HW active residency 100%/满频、ioreg Renderer 1%、Device 100%，无归属进程。
    **唯一修复 = 重启**。预防：改 kernel 后必须重编译所有使用方；异常超时进程
    kill 后查 GPU 状态；GPU 基准只跑在空载机器上。
12. **C++ 头对头 harness 首跑 NMSE 为正（+0.6 dB）**：单样本估计值量级/相位合理但
    聚合误差 > 信号功率——bug 未定位（候选：深衰样本爆炸、grid_fake 单符号缓冲
    语义、逐符号错位），见 §8 续接点。
13. **真值索引转置（harness 正根因）**：Y_test.npy 是 `[n, 子载波, 符号, 2]`
    （子载波主序），首版 C++ 按符号主序读——跨符号同信道掩盖了错位，NMSE +0.6 dB。
    **教训：npy 与 C++ 互操作先写清布局契约**（生成器注释 + harness 注释双写）。
14. **无 CFO 的合成数据必须关 compensate_cfo**：低 SNR 下噪声主导的 CFO 相位随机
    旋转 DMRS 符号，破坏 TD 平均/MMSE 相干性（本次修复后数字未变说明是次因）。
15. **NMSE 聚合口径必须统一（重大教训）**："逐样本 dB 的均值"（mean-of-dB）与
    "合并误差功率的 dB"（dB-of-mean）在 SNR 跨 30 dB 时相差 ~6 dB——跨工具对比
    （Python eval vs C++ bench）曾因此产生虚假的 7 dB"失配"与错误的 +8.2 dB
    结论。**所有跨实现对比一律用 pooled（Σerr/Σsig 取 dB）口径**。
16. **tf.keras 元素级 sample_weight 的尾维必须是 1**：对 (n, nsc, 14, 2) 的标签
    传 (n, nsc, 14, 2) 的掩码会触发 `Squeeze` 报错（"expected a dimension of 1,
    got 2"）——TF 会 squeeze 权重尾部的 size-1 维再广播。正确形状
    `(n, nsc, 14, 1)`（用 `np.broadcast_to` 零拷贝视图即可）。
17. **pad-aware 训练 vs 全宽训练的权衡**：随机宽度+零填充微调后，全宽 head2head
    掉 ~0.2 dB（全宽在训练分布中占比变小），但窄带（6-12 PRB）+2.5 dB——真实
    手机几乎全是窄/中带授权，权衡明确值得；两模型都要各自做 pad-aware 微调，
    不能只训新宽度。

## 10. 脚本清单（本仓库 `lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/`）

| 文件 | 用途 |
|---|---|
| `train_helena.py` | HDF5 加载 + 划分 + 重训/微调（环境变量 HELENA_EPOCHS/DATA/MODEL） |
| `eval_helena.py` | per-SNR NMSE 评估（LS/线性插值/PracticalMMSE/HELENA 对照） |
| `gen_pusch_dataset.py` | Phase B：我方 PUSCH 合成集（TDL-A..E、DMRS {2,7,11} type1、LS+线性插值输入格式；`--pad-aware --min-prb/--max-prb` 随机宽度+零填充） |
| `convert_coreml.py` | SavedModel → Core ML（固定形状，`--shape`）+ ANE/GPU/CPU 时延基准 |
| `train_pad.py` | pad-aware 微调（元素级损失掩码、`--transfer/--init`） |
| `eval_pad.py` | pad-aware 测试集分宽度段 pooled NMSE |
| `probe106.py` | 52→106 零训练宽度迁移探针（SavedModel 重建 + 保存） |

## 11. 高信噪比 OOD 教训（2026-08-30，20 MHz E2E attach 失败的根因）

18. **模型会系统性破坏近乎完美的输入**：HELENA 训练 SNR 包络 −5..25 dB；ZMQ
    E2E 的信道 ~45 dB（近乎无噪）。实测（SNR 45 dB 合成探针）：输入网格本身
    −45 dB NMSE，模型输出把 6/18/24/52 PRB 分别破坏到 −22/−27/−29/−33 dB
    （去噪偏差在包络外反向作用，窄授权最严重）。后果：SRB1 的 6-PRB PUSCH 与
    PUCCH 解调失败（CRC KO @ 40+ dB SINR）→ RLC max RETX → RRC IDLE → attach
    死循环（33+ 次 UE Context Release）。10 MHz E2E 侥幸存活是因为全宽 52 PRB
    只被破坏到 −33 dB，仍在 64QAM 门限内。
    修复：**SNR 门控**（>25 dB 直接 classical bypass，`kHelenaMaxSnrDb`），
    并给 harness 加 `OCUDU_HELENA_FORCE_NN` 旁路（合成集的噪声估计退化到 100 dB
    上限会误触门控）。教训：**部署模型必须定义输入分布包络并在包络外设安全
    兜底**——干净信道是 classical 的主场，NN 的价值区在低/中信噪比。

19. **高 SNR 修复的最终方案（pitfall 18 的收尾）**：纯高 SNR 专注微调导致灾难性
    遗忘（原包络 −17.3 → −13.1 dB）；权重平均两头折中但都受损。最终采用：
    ① 训练包络扩到 −5..55 dB 重训（包络内无退化）；② **SNR 软混合（α-blend）**：
    输出 = 输入 + α·(NN−输入)，α 在 ≤25 dB=1 → ≥50 dB=0 线性衰减。45 dB 实测
    全宽度 ≥ −34 dB（6 PRB −34、12+ PRB −37~−45，vs 旧模型 −22 打崩 attach），
    包络内模型 100% 无损。教训：**对"包络外行为"优先用运行时软过渡而非硬训**
    ——soft blend 保证单调安全，硬微调/硬门控要么遗忘要么挡死。
