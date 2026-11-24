# macOS / Apple Silicon 训练性能 memo（两轮改动、实测账、与 CUDA 的 5.2× 差距）

**日期**：2026-11-22 ～ 2026-11-24（测试与实验）；**2026-11-24 收尾检查见 §11**｜**整理**：2026-11-24
**机器**：开发机 **Mac mini M4 Pro**（14 核 = 10P+4E，64 GiB，macOS 26.6.2 / arm64）；
对照机 **广州 106**（`jwang@192.168.0.106`，N 卡/CUDA，实测 **≈28 min/轮**）
**对象**：`lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py`（两轮增量改动）与
`~/ai_depth3/datasets_train_w2v3c`（183 624 样本）上的同一配方
**性质**：**实验记录 ＋ 判据**。所有数字标 ★实测；外推/估算标（推算）。不要把它当"一般性建议"引用——
每一条都有它自己的模型、batch、夹具。

> **文档分工**：操作流程见 `corpus_to_training_to_measurement_db_manual.md`（§9.2 是本文的操作摘要、§24 是索引）；
> 配方见 `depth3_training_recipe_memo.md`；机器算力见 `../llr_ai_detection/machine_compute_capability_and_model_train_memo.md`。
> **本文只回答三件事**：① macOS 上改了什么、为什么；② 实测有多快；③ 与 Ubuntu/CUDA 差多少、差在哪、还能怎么改。

---

## 0. 一页结论

| # | 结论 | 证据 |
|---|---|---|
| 1 | **macOS 上能跑，且数值与 HEAD 逐位相同**（唯一例外：`--modcond --target genie` 的标签修复，见 §7）；所有改动都在 host 侧/缓存/平台闸门，**计算图未变** | 逐样本对拍、`--init` 定权重训练 3 轮 bit-identical（fp32 与 fp16 两套）、Linux 模拟路径同值 |
| 2 | **同一配方一轮：Ubuntu 106 = 28 min，Mac GPU = 2 h 25 min（5.2×）** | ★ 真跑：183 624 样本 / batch 64 / 2 870 步 / 8 697.75 s |
| 3 | **差距不在本仓库代码，在 `tensorflow-metal` 的 kernel**：Mac GPU 只有自身峰值 ≈6%，而 Mac CPU ≈21%、Ubuntu GPU ≈20% | 每样本 ≈24.9 GFLOP（估算）⇒ 2.72 / 0.53 / 0.34 TFLOPS |
| 4 | **Mac 上 GPU 只在重模型上略快**：重模型 47.3 vs CPU 74.2 ms/样本（**1.57×**）；轻模型 GPU 反而只有 CPU 的 **0.53×** | ★ 两组独立测量 |
| 5 | 用 GPU 的另一个理由是**让出 CPU**：0.87 核 vs **6.6–6.9 核**；重模型上还顺带快 1.57×，轻模型上则要付 1.9× 的墙钟代价 | ★ `getrusage` 实测 |
| 6 | **半精度在 macOS 上彻底不可用**：fp16 与 bf16 **都在 `libmetal_plugin` 里段错误**（同一模板） | ★ 两份崩溃栈，已在代码里拦截 fp16 |
| 7 | XLA 在 macOS 上不可用：Metal 无 XLA device；编给 CPU 反而 **慢 7.4×** | ★ 实测 `--jit --no-gpu` |
| 8 | batch 不是 macOS 的杠杆：重模型 16/32/64 每样本 51.4/47.6/47.3 ms（平），只涨内存（12/16/25 GB） | ★ 三档实测 |
| 9 | 真能变快的只有"减 FLOPs"：`--dense-bottleneck 4` **1.66×**（87 min/轮），但那是**另一个模型** | ★ 实测，参数量 496 345 → 166 297 |
| 10 | 因此：**80 轮的长跑留在 106（≈37 h）**，Mac 跑 1–2 轮的 smoke/回归（2.42 h/轮） | 由 1、9 推得 |
| 11 | **收尾检查与跨平台回归已执行**：唯一代码改动是收尾日志文案；训练数学/导出/序列化未动；macOS 逐项 PASS、Ubuntu 侧 3 项需人工（§11）| §11.2／§11.3 的逐条表 ＋ §11.7 的 AST 审计 |

---

## 1. 环境（可复现的前提）

| 项 | 值 | 备注 |
|---|---|---|
| 硬件 | **Mac mini M4 Pro**，14 核（10P+4E），**64 GiB** 统一内存 | 与 GPU 共享同一块内存 |
| 系统 | macOS **26.6.2**（25G83），arm64 | |
| venv | **`~/.venvs/tf-metal-2.18`**（python 3.11.15） | ★ 不要装进全局 3.11：TF 2.18 要求 `numpy<2.1` |
| 版本 | `tensorflow 2.18.1` + **`tensorflow-metal 1.2.0`** + `keras 3.15.1` + `numpy 2.0.2` | |
| 设备 | `tf.config.list_physical_devices()` = `CPU:0` + `GPU:0`；插件自报 `systemMemory: 64.00 GB`、`maxCacheSize: 25.92 GB` | GPU 报"0 MB 显存"是正常（走统一内存） |
| ★ 为什么锁 2.18 | Apple 的对照表把 `tensorflow-metal` 1.1/1.2 配在 **TF 2.14–2.18**；**2.19+ 没有配对** | `pip install tensorflow` 现在装 2.21 ⇒ **永远看不到 GPU** |
| ★ 唯一可信的"用上 GPU 了吗"判据 | 训练器横幅 `platform: compute placement: a 64x64 matmul lands on GPU:0` | `list_physical_devices()` **不能**当判据：`--no-gpu` 后插件仍列出 `GPU:0`，但算子落 CPU |
| 对照机 106 | Ubuntu，N 卡/CUDA，配方同 §2，实测 **28 min/轮**（快照间隔 09:50→13:05，8 轮） | ⚠ **106 的 GPU 型号未知**：旧记录称 106 无 GPU、107 是 RTX 3060 Laptop 6 GB（见机器 memo）。若能 `ssh 106 nvidia-smi`，本 memo §4 的峰值占比可换成硬数字 |
| 训练集 | `~/ai_depth3/datasets_train_w2v3c`：**183 624 样本**，PRB 1–51（均值 14.2），qm 2/4/6/8 = 32 002/32 000/32 012/87 610，`X` 以 **float16** 存（612×64），目录 **22 GB**（mmap） | |
| 验证集 | `~/ai_depth3/datasets_monitor_1102a`（108 样本，`--max-val 1000` 取小值） | |
| 模型（配方） | `--c-hid 32 --blocks 36 --dense-blocks --dense-skip-entry --se --scale-init 0.03`，2 个输出头 ⇒ **496 345 参数** | 与 `training_recipes/ceb36_const_80.sh` 一致 |

---

## 2. 两轮改了什么（`train_depth3.py`）

> 原则：**Ubuntu/CUDA 的数值与契约不动**。下面每一条都注明作用域；"跨平台"的改动都不改变算出来的数（§7 给证据）。

### 2.1 平台与设备（★ 仅 macOS）

| 改动 | 内容 | 为什么 |
|---|---|---|
| `IS_DARWIN` ＋ `resolve_xla` | Linux 原样返回请求值；macOS 未**显式**写 `--jit` 就关 | `tensorflow-metal` 无 XLA device；显式给出也有退路 |
| `xla_self_test` | `--jit` 时先编译 2×2 探针，失败则降级并打印原因 | 把"第一批就崩"变成"启动时一行日志" |
| `compile_step` 工厂 ＋ `step` 包装 | 首次调用失败时重建无 XLA 版本（仅 macOS 且 jit 打开、且尚未成功过） | 不半途丢优化器状态 |
| `configure_macos_runtime` | 在任何 op 前请求 memory growth（try/except） | Metal 插件的行为差异 |
| `pipeline_defaults` | macOS `--workers 0` → `min(3, 核数/4)`；prefetch 默认 `2×workers` | 实测 6 线程比串行还慢（§3.1） |
| `report_macos_runtime` ＋ `probe_compute_device` | 启动打印 macOS/设备/**算子落点**/XLA 生效值/线程数/**ANE 不参与反传** | "装了插件 ≠ 用上 GPU"；诊断必须自证 |
| `--no-gpu` ＋ `hide_gpus` | 默认关；给出时在任何 op 前隐藏全部 GPU（Linux 也隐藏） | c32 上 CPU 更快（§3.3），而插件隐藏后仍列出 GPU |
| fp16 护栏 | macOS 上训练路径 `--dtype float16` 直接 `parser.error`（`DEPTH3_ALLOW_METAL_FP16=1` 可越过） | 插件 fp16 段错误（§3.6）|

### 2.2 数据与 host↔device（跨平台，数值中性）

| 改动 | 内容 | 为什么 |
|---|---|---|
| 元数据常驻 | 一维列（`alloc_*`/`qm`/`nof_bits`/`snr_nominal`/UCI…）一次读进内存；`X/H/bits/tb_bits` 仍 mmap；一维列 >32 MB 则跳过 | 随机索引的页错误；10 万样本 ≈8 MB |
| 几何缓存 | `_GeometryCache`：有界 **FIFO ＋ 锁**；命中无锁，未命中在锁内构建；条目数组发布前 `writeable=False` | 256QAM 掩码 612×112＝274 KB/样本，原来每样本重建；`clear()` 淘汰会造成**未命中风暴** |
| `Batches.sample` | `unpackbits(count=nof_bits)`；`X` 用 `asarray`（f32 时零拷贝）；复用 `_MODULATION_OF_QM`；`_build_geometry` 冻结数组 | 纯 host 侧省工 |
| `Batches.epoch` | worker 内 `order[start:stop].tolist()`，去掉逐样本 `int()`；`ahead=max(prefetch, workers, 1)`；批次顺序与串行洗牌一致 | 顺序不变，少一次 Python 转换 |
| `stack` | `map(operator.itemgetter(key), …)` ＋ 一次性 dtype 判定（f32 直接返回） | 去掉盲目的 `astype` 调用（实测基本中性，见 §5） |
| `LOSS_ACCUMULATION=64` | 每 64 个 batch 一次 `tf.stack(...).numpy()`，取代每 batch `float(total)` 同步 | GPU 队列不再每步被抽干；**逐元素仍是同一个 float32**，`np.mean` 不变 |
| `tensors()` | 只打包**图里实际有的 head** 消费的 target；`w_snr` 仅在 `--snr-weight>0` 时构造 | 白拷贝（19 MB/batch 中的一部分）|
| `evaluate()` | 每 batch 一次 `.numpy()`；`--select-on val` 时**不**计算 low-SNR band（`want_band`） | 每行 612×112 的 `sign`＋两次掩码原来是白算 |
| `plane_channels()` | 取代 `main()` 里的 `train.sample(0)` | 少建一整条样本 |
| `threshold_weights` | `--snr-weight` 向量化 | 与原逐样本循环逐位相同 |

### 2.3 修复与序列化（跨平台）

| 改动 | 内容 |
|---|---|
| `CUSTOM_OBJECTS` | 统一三件套（补 `MaskedSmooth`）：`--export-only` 不再在训练完成后加载失败 |
| `_constellation` | 星座图**按 qm 缓存**并冻结；修掉"只缓存第一次 qm"⇒ `--modcond --target genie` 的混合调制标签错误 |
| `tensors`/`xla` docstring | 记录实测（0.74 ms/batch＝0.37%；XLA 在 CPU 上 7.4× 慢） |

**未改**：`build()` 图结构、三个自定义层的 `get_config/build/权重命名`、`export_deployment`/`export_buckets`（输出名与 `depth3_*` `userDefined`）、`ChannelGate` 的 fp32 乘积、fp32 损失/LLR 缩放、resume/snapshot 逻辑、epoch 行格式。

---

## 3. 实测账

### 3.1 数据流水线（`Batches.epoch`，CPU 侧，ms/样本，best-of-3）

| 夹具 | HEAD | 现在（串行） | 现在（多线程） |
|---|---|---|---|
| 51 PRB 全带，batch 8，8 样本 | 0.744 | 0.650（1.14×）| 6 线程 0.658 |
| 51 PRB 全带，batch 8，200 样本 | — | 0.650 | **0.412（3 线程）**；2 线程 0.430、4 线程 0.419、**6 线程 0.552（比串行慢）** |
| 混合 2–51 PRB，batch 8，200 样本 | 0.480 | 0.401（1.20×）| **0.310（2 线程）**；3 线程 0.360、6 线程 0.516 |
| 1–51 PRB 均匀，batch 16，`--modcond`，400 样本 | — | 0.589 | **0.483（3 线程）** |

**判据**：183 624 样本 / 2.42 h ⇒ 只需 **~21 samples/s ≈ 3 MB/s 磁盘**；加载实测 2 000 samples/s ⇒ **数据加载从不进入关键路径**。
**线程数不是越多越好**：GIL ＋ 统一内存带宽，甜点在 2–3。

### 3.2 host→device（为什么不再加异步流水线）

| 项 | ★实测 | 结论 |
|---|---|---|
| `tensors()`（7 个 target，batch 16，19.2 MB）| stack 0.37 ms ＋ `tf.constant` 0.37 ms ＝ **0.74 ms** | 对 199 ms 的 step 是 **0.37%** ⇒ 加后台打包/第二条 TF 线程不划算（Metal 插件已足够脆） |
| `stack` 微优化 | 0.382 → 0.376 ms（1.02×）| 中性；保留只为语义清楚 |
| `evaluate()` | 每 batch 3 次 `.numpy()`；`--select-on val` 时不再逐行 `decode_metrics` | 每轮校验（1 000 样本）省 ~0.3 s |

### 3.3 训练 step / epoch

**(a) 轻模型（c32、18 blocks、**depthwise**、4 头、非 cond、随机 target）** ★ 单步 ms：

| batch | Metal GPU | CPU（`--no-gpu`）| GPU/CPU |
|---|---|---|---|
| 8 | 193.8 | 105.5 | 0.54× |
| 16 | 325.7 | 188.3 | 0.58× |
| 32 | 578.9 | 438.9 | 0.76× |
| 128 | 2 025.0 | 1 743.4 | 0.86× |
| c64 b8 / b16 | 337.1 / 512.1 | 228.7 / 436.2 | 0.68 / 0.85× |
| c128 b8 / b16 / b32 | 447.6 / 738.8 / 1 369.0 | 471.8 / 954.5 / 1 880.3 | 1.05 / **1.29** / **1.37×** |

端到端（200 样本/轮）：c32 b8 GPU 4.93 s vs CPU 2.63 s（**0.53×**）；c128 b16 GPU 10.08 vs CPU 12.13（**1.20×**）。
⇒ **轻模型上 GPU 全面更慢**；只有 c128＋大 batch 才反超。

**(b) 重模型（本 memo 的主角：c32、36 blocks、dense 3×3、SE、dense-skip-entry、2 头）** ★：

| batch | 占用（`phys_footprint`）| 单步 | 每样本 | 整轮 183 624（推算） |
|---|---|---|---|---|
| 16 | 12 GB | 0.82 s | 51.4 ms | 2.62 h |
| **32** | **16 GB** | **1.52 s** | **47.6 ms** | **2.42 h** |
| 64（真跑）| **24–25 GB** | **3.03 s** | **47.3 ms** | **2.42 h（实测 2 h 25 min）** |
| CPU（b16，`--no-gpu`）| RSS 5.2 GB | 1.19 s | **74.2 ms** | 3.78 h |

⇒ **每样本成本对 batch 几乎不敏感**（47.3/47.6/51.4），batch 只换内存；**同模型 GPU 比 CPU 快 1.57×**（47.3 vs 74.2 ms），且只吃 0.87 核（CPU 吃 6.6–6.9 核）。

### 3.4 真跑一轮（用户指定的那轮）

| 项 | ★实测 |
|---|---|
| 命令 | `ceb36_const_80.sh` 配方逐字，`--batch 64 --epochs 1`，输出 `~/ai_depth3/runs/mac_c32b36_b64_e1`（新目录，未碰 106 的 runs）|
| 墙钟 | **8 697.75 s ＝ 2 h 24 min 58 s**（训练循环自计 8 688 s，含 108 样本 monitor 校验 ＋ 快照）|
| 步数 / 每步 | 2 870 步（183 624/64）；**≈3.03 s/步** |
| 吞吐 | **21.1 样本/s** |
| CPU | **7 605.72 s ＝ 整轮 0.87 核**（同期还有 `gnb` 占 2 核、Chrome 2.6 GB） |
| 内存 | maxRSS 22.19 GiB；`footprint` 峰值 **25 GB**（graphics 15 GB ＋ MALLOC_SMALL 8.7 GB）|
| 指标 | train 0.37524 ／ val 0.23110 ／ uncoded BER 0.0988 ／ mean\|LLR\| 4.34 |
| 产物 | `depth3_last.keras` 3 246 554 B、`snapshots/epoch_0001_periodic.keras` 同尺寸、`resume.json`、`history.json`、`val_llr_stats.npy` |
| 注意 | 配方是 `--select-on none` ⇒ 当时**没有** `depth3_best.keras`，而收尾行仍打印 "wrote …"（既有代码的无条件打印）。**2026-11-24 收尾已修**：现在只在真的写了才说 "wrote …"，否则打印 "no best checkpoint kept (select-on none)"；前缀 `best val loss N;` 保持不变，下游 `finalize_conditioned.sh` 的 `*"best val loss"*` 匹配不受影响 |
| 80 轮外推 | 80 × 2.42 h ≈ **193 h ≈ 8 天** |

### 3.5 内存账（为什么重模型"贴着上限"）

`/usr/bin/footprint -p <pid>` 的分解（batch 64，稳定期）：

| 类别 | 占用 | 备注 |
|---|---|---|
| Owned physical footprint (graphics) | **15 GB** | Metal/MPSGraph 的缓冲；**与 batch 同级时是平的**（120 步只 +23 MB）|
| MALLOC_SMALL | 8.7 GB | 主机堆：每 step 要临时 stack ~0.5 GB target ＋ 分配器保留 |
| 其余（MALLOC_LARGE、IOAccelerator、page table…）| <1 GB | |
| **合计 phys_footprint** | **25 GB** | 插件 `maxCacheSize` 25.92 GB 指的是**它自己的缓存**（即上面 15 GB 那块），不是总 footprint ⇒ **没有"再跑半小时就撞上限"** |

**固定开销 ≈7 GB**：由 16/32/64 三档（12/16/25 GB）反解，斜率 ≈0.27 GB/样本、截距 ≈7 GB（MPSGraph 编译产物/管线缓存量级，与 batch 无关）。
⇒ 这也是"为什么 batch 16 也要 12 GB"的原因。
**⚠ 记账口径**：`RSS` 会骗人（batch 64 时 RSS 只有 2.9 GB，Metal 缓冲不计入），**必须用 `footprint`**。

### 3.6 两个会让进程直接死的配置（本机都复现过）

| 配置 | 现象 | 栈/报错 | 处置 |
|---|---|---|---|
| `--dtype float16` ＋ GPU | macOS 弹「Python 意外退出」，exit 139 | `EXC_BAD_ACCESS` in `metal_plugin::MPSBinaryOpBase<Eigen::half, Eigen::half>::Compute` → `MetalBinaryGraph::Compute` → `mpsGraphTensorDataFromTensor` → `objc_msgSend` | 代码已拦截（一行 parser error）；`DEPTH3_ALLOW_METAL_FP16=1` 才会越过 |
| **bfloat16**（本轮用代理改 `build()` 实测）| 同样 exit 139 | **同一路径**，模板换成 `Eigen::bfloat16` | **结论：半精度在这版插件上彻底不可用**，不是 fp16 专属 |
| `--jit` ＋ GPU | `NotFoundError: could not find registered platform with id: 0x…` `[Op:__inference_step_…]` | XLA cluster 与 PluggableDevice 冲突 | macOS 默认已关 XLA；**不要**在 Metal 上显式 `--jit` |

> 这两类崩溃的原始 `.ips` 与崩溃状态已按用户要求清理（9 份 Python ＋ 2 份 gNB ＋ 3 份 `CrashReporter` plist）；
> **栈证据保留在本文与语料手册 §9.2.3(e)**，将来报 Apple 或换插件版本时以它为准。

---

## 4. 与 Ubuntu/CUDA 的 5.2× 差在哪

### 4.1 对齐口径

两边都是**同一配方、同一数据集、同一 batch（16）**；Ubuntu 的一轮时间取自 8 个连续快照的时间戳（27–28 min）。

| | Ubuntu 106（N 卡/CUDA） | Mac GPU（b16） | Mac GPU（b64） | 倍数 |
|---|---|---|---|---|
| 每轮 | **1 680 s（28 min）** | — | **8 688 s** | **5.2×** |
| 每步 | 146 ms | 0.82 s | 3.03 s | 5.6× / 20.7× |
| **每样本** | **9.15 ms** | **51.4 ms** | **47.3 ms** | **5.2–5.6×** |

### 4.2 每 FLOP 的效率（估算，用于定位而不是精确对比）

每样本前向+反向 ≈ **24.9 GFLOP**（36 blocks × [dense 3×3 9·32·32 ＋ pw 1×1 ＋ dproj 1×1 96·32 ＋ SE] × 8 568 个位置 × ≈3，**估算**）：

| 执行体 | 每样本 | 等效算力 | 占自身峰值 |
|---|---|---|---|
| Ubuntu 106 GPU | 9.15 ms | **≈2.72 TFLOPS** | ≈**20%**（若为 3060 级 ~13 TFLOPS）|
| Mac GPU（M4 Pro）| 47.3 ms | **≈0.53 TFLOPS** | ≈**6%**（≈9 TFLOPS）|
| Mac CPU（14 核，`--no-gpu`）| 74.2 ms | **≈0.34 TFLOPS** | ≈**21%**（≈1.6 TFLOPS）|

**⇒ 一句话：Mac 的 CPU kernel 效率是它自己 GPU 的 ≈3.6 倍；Metal 只跑出峰值的 ≈6%。**
（峰值都是估算值，用于说明数量级，不是规格书。）

### 4.3 逐项归因（按影响排序）

| 因素 | 影响 | 证据 |
|---|---|---|
| **Metal/MPSGraph 的卷积/kernel 效率** | **主导（≈4–5×）** | 4.2 的峰值占比；且同一个模型 GPU 只比 CPU 快 1.57×（轻模型甚至 0.53×）——若瓶颈是 host 或调度，GPU 不该这么接近 CPU |
| **XLA 融合**（Ubuntu 默认 `--jit`）| CUDA 上 1.4–1.8×（本项目 README §3 实测 1.43–1.48×），Mac 不可用 | Metal 无 XLA device；Mac 上 `--jit` 只能编给 CPU，实测 **89.5 vs 12.1 ms/样本 ⇒ 7.4× 慢** |
| **半精度 / tf32**（CUDA 常见，Ampere+ 默认 tf32）| 可再 1.5–2×，Mac **完全不可用** | fp16、bf16 均在插件内段错误（§3.6）|
| batch | **不是差距来源** | Mac 16/32/64 每样本 51.4/47.6/47.3；Ubuntu 用 16、Mac 用 64 已对齐 |
| host 侧（loader/tensors/同步）| **<1%** | 0.48 ms/样本、0.74 ms/batch、损失每 64 步才回读 |
| 数据 I/O（22 GB mmap）| 不进入关键路径 | 只需 ~3 MB/s；X 为 float16，转换在 host 侧且每样本 ~0.05 ms |

### 4.4 "不是开销、是算术"的两条独立证据

1. **块数线性**：18 blocks → 36 blocks（同 dense/SE 配方，b16）：433.5 ms → 1 000.5 ms ＝ **2.31×**（近线性）。
2. **减算子无效**：`--fuse-relu`（少 ~180 个 ReLU/融合）b32：**1.50 s vs 1.52 s/步**，无变化。

### 4.5 一个重要的口径提示：轻模型上差距只有 1.4×

同一台 Mac、同一个后端，**c32/18 blocks/depthwise** 上是 **CPU 82–89 samples/s**，而项目旧记录里 107（RTX 3060）是 **122 samples/s** ⇒ 差距只有 **1.37×**。
**⇒ 差距随模型变重而放大**：轻模型两边都被"每 kernel 开销"支配，重模型才是真刀真枪的 FLOPs 竞争，cuDNN+XLA 的优势全出来了。
**⇒ 所以"Mac 比 CUDA 慢 5 倍"只对重模型成立；引用时必须带模型。**

---

## 5. 试过但**无效**的 macOS 加速手段

| 尝试 | 结果 | 判断 |
|---|---|---|
| 加 batch（16→32→64→128→192）| 每样本 51.4/47.6/47.3/—/—（轻模型 41→55→63 samples/s 后饱和）| 只换内存 |
| `--fuse-relu` | 1.50 vs 1.52 s/步（b32）| 中性 |
| `--jit`（GPU 不可用 → CPU）| 89.5 vs 12.1 ms/样本 | **7.4× 慢** |
| fp16 / bf16 | 都段错误 | 不可用 |
| TF intra-op 线程（14/10/8/6/4/2）| 93.2/94.3/104.1/111.6/144.1/230.7 ms（轻模型 b8 CPU）| 默认 14 已最优，**不能动** |
| 后台打包 target（异步流水线）| 收益上限 0.37% | 不值得引入第二条 TF 线程 |
| 改用 CPU（`--no-gpu`）| 重模型 74.2 vs 47.3 ms/样本 | 慢 1.57×，且吃 6.6–6.9 核而非 0.87 核 |

---

## 6. 真能变快的（都要改模型/配方）

| 手段 | ★实测（36 blocks 配方，GPU）| 参数量 | 说明 |
|---|---|---|---|
| 基准（dense 3×3）| **1.52 s/步 @b32 ⇒ 2.42 h/轮** | 496 345 | 用户指定配方 |
| `--dense-bottleneck 2` | 1.445 s/步 ⇒ 2.30 h/轮（**1.05×**）| 247 513 | 几乎无收益：3×3 只占约一半工作量 |
| `--dense-bottleneck 4` | **0.914 s/步 ⇒ 87.5 min/轮（1.66×）** | 166 297 | 最快，但已是另一个模型 |
| 块数 36 → 18 | 433.5 ms vs 1 000.5 ms（**2.31×**）| — | 另一个模型 |
| `--max-train` 子集 / 少轮 | 线性 | — | smoke/回归用 |

**为什么 bottleneck=2 几乎没用**：这个模型里 3×3 之外还有 `dproj 1×1 (96→32)`（3 072 MAC/位置）与 **LLR 头 1×1 (32→112) ＋ 1 568 通道的 BCE/反传**，它们不随 bottleneck 缩小；固定部分约占一半。

---

## 7. 等价性与"零破坏"的证据

| 检查 | 方法 | 结果 |
|---|---|---|
| `Batches.sample()` 逐值 | 与 `git HEAD` 版并排加载，同数据、同布局（folded/spatial/probe8 × cond × 有/无 UCI × genie）| 值、dtype、shape **全等**；epoch 批次顺序与串行一致 |
| 训练数值 | 等价夹具为 **2 blocks / c_hid 16**（同时打开 `--sym-aux/--se/--smooth-k/--llr-post/--noise-to-llr/--ce-head/--mlp-expand/--low-rank`），**`--init` 固定权重**后各跑 3 轮 | `train/val/val_ber/val_llr_mean/val_bler` **逐位相同**（fp32 与 fp16 两套）；最终权重 `max\|Δ\| = 0.0`。★ 36 blocks 的重模型未与 HEAD 做 A/B（改动只涉及 host 侧与缓存，与块数无关）|
| Linux 路径 | 把 `IS_DARWIN=False` 后同命令 | 无 `platform:` 行、`xla: on`、`--workers 0` 串行，history 逐位相同 |
| 并发/缓存 | 6 线程 × 51 种几何、`GEOMETRY_ENTRIES=3` 强制淘汰风暴（800 样本）| 每个样本与串行逐位相同；缓存长度 ≤ 上限；掩码只读（写入抛 `ValueError`）|
| 其他路径 | `--resume`、`--no-val/--monitor-every`、`--sym-aux/--se/--smooth-k/--llr-post/--noise-to-llr/--ce-head/--no-reporting-heads/--mlp-expand/--low-rank/--lr-decay/--snr-weight/--patience/--snapshot-every`、`--no-aux`、空训练集、`--bler-samples>0`、`--dense-skip*` | 全部跑通；`--export-only` 能加载 `MaskedSmooth` 检查点；`--probe-graph` 走到 coremltools 才停（本机未装）|
| **唯一故意改变数值的行为** | `--modcond --target genie` 的星座图按 qm 缓存（原来只缓存第一次的 qm ⇒ 混合调制标签错误）| **旧 run 的数值不可比**，需重训/重评 |

**复现上面的对拍必须用 `--init`**：★ Keras 3 的初始化器不受 `tf.random.set_seed` 控制，同一命令两次运行初始权重本来就不同（本机实测两次 `train` 为 3.38864 vs 3.58929）——这是既有事实，不是本次改动。

---

## 8. 判据与建议

### 8.1 macOS 上选设备

| 场景 | 建议 |
|---|---|
| c32/18 blocks/depthwise（轻）| **CPU 更快**：`--no-gpu`（GPU 慢 1.87×）|
| 36 blocks dense/SE（重）| **用 GPU**：快 1.57×（47.3 vs 74.2 ms/样本）且只占 0.87 核（CPU 要 6.6–6.9 核）|
| 任何 fp16/bf16 | **禁止**（段错误）|
| 任何 `--jit` | **禁止**（Metal 上直接 NotFoundError；CPU 上慢 7.4×）|

### 8.2 macOS 上的 batch

| 模型 | 推荐 batch | footprint | 每轮 | 理由 |
|---|---|---|---|---|
| c32/18 blocks/depthwise | **64** | 6.9 GB | — | 吞吐拐点（70 samples/s）|
| c32/36 blocks dense/SE（本配方）| **32** | 16 GB | 2.42 h | 与 64 同速、省 9 GB |
| 同上，机器要留很多余量 | 16 | 12 GB | 2.62 h | 只慢 8% |

### 8.3 两条路

1. **长跑放 106**（80 × 28 min ≈ 37 h）；Mac 只做 1–2 轮的 smoke/回归（2.42 h/轮）、导出、尺子回放。
2. **在 Mac 上做研究会话**：用 `--dense-bottleneck 4`（87 min/轮）或 `--blocks 18`（~1 h/轮）＋ `--max-train` 子集，**并明确标注"不是 106 上跑的那条模型"**。

### 8.4 Ubuntu/CUDA 侧要复核的（不需要改代码）

1. 用 `--init` 固定权重，跑 2 轮对比 `history.json` —— 应逐位相同（除 §7 的 genie 修正）。
2. 不传 `--workers` 时仍应是串行；日志头仍应是 `xla: on`，且**没有** `platform:` 行。
3. `--select-on band` 仍能选出 band（band 只在需要时计算）。
4. `--export-only … --export-probe/--export-buckets` 的输出名与 `depth3_*` metadata 未变。
5. ⚠ 若你们的臂里用了 `--modcond --target genie`：先确认旧结果是否受影响（§7 最后一行）。

---

## 9. 未解问题 / 下一步

| # | 问题 | 怎么解 |
|---|---|---|
| 1 | **106 的 GPU 到底是什么型号/显存** | `ssh 106 nvidia-smi`；有了它，§4.2 的"20% 峰值"才能写成硬结论 |
| 2 | 106 每轮 28 min 是否含验证/快照（该配方 `--no-val --monitor-every 10`，第 10 轮才校验）| 看 106 的日志；若含校验，纯训练略快 |
| 3 | 更新版 `tensorflow-metal` 是否修好半精度 | 跟踪 PyPI；一旦修好，fp16/bf16 可能直接给 1.5–2×（这是唯一可能显著改变 Mac 结论的外部变量）|
| 4 | 是否值得在 Mac 上换后端（PyTorch-MPS）| 需要重写训练代码与导出链，**当前性价比不划算**；只有当"必须在 Mac 上跑长跑"成为硬需求时再评估 |
| 5 | ~~收尾那行 "wrote …/depth3_best.keras" 在 `--select-on none` 时是假的~~ | **已修（2026-11-24）**：`best_written` 标志 ＋ 分支打印；正常路径文案逐字不变、前缀不变（见语料手册 §9.2 与训练器 2026-11-24 的收尾检查）|

---

## 10. 附录

### 10.1 复现命令

```bash
# 环境（一次性）
python3.11 -m venv ~/.venvs/tf-metal-2.18
~/.venvs/tf-metal-2.18/bin/pip install "tensorflow==2.18.*" "tensorflow-metal==1.2.*"
~/.venvs/tf-metal-2.18/bin/python -c "import tensorflow as tf; print(tf.config.list_physical_devices())"

# 真跑一轮（本 memo §3.4 的那条）
P=~/.venvs/tf-metal-2.18/bin/python
$P ~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py \
  --train ~/ai_depth3/datasets_train_w2v3c --val ~/ai_depth3/datasets_monitor_1102a \
  --out ~/ai_depth3/runs/mac_c32b36_b64_e1 \
  --modulation QPSK --modcond --no-reporting-heads --c-hid 32 --blocks 36 --layout spatial \
  --dense-blocks --dense-skip-entry --se --scale-init 0.03 \
  --snr-weight 2 --snr-sigma 3 --loss-norm bits --smooth-k 0 \
  --snapshot-every 1 --batch 64 --workers 1 --max-val 1000 \
  --no-val --monitor-every 10 --bler-samples 0 --select-on none --seed 20261105 --lr 1e-3 --epochs 1

# 内存（跑起来之后另开终端；RSS 会骗人，必须 footprint）
/usr/bin/footprint -p <训练进程 PID> | head -12

# 设备判据
# 只看启动横幅的 "compute placement: … lands on GPU:0"，不要看 list_physical_devices()
```

### 10.2 产物

| 路径 | 内容 |
|---|---|
| `~/ai_depth3/runs/mac_c32b36_b64_e1/` | 本轮真跑的 `depth3_last.keras`、`snapshots/epoch_0001_periodic.keras`（3 246 554 B）、`resume.json`、`history.json`、`val_llr_stats.npy` |
| `~/.venvs/tf-metal-2.18/` | 唯一能跑 Metal 的 venv（1.3 GB）|
| `doc_chinese/depth3_llr_ai/corpus_to_training_to_measurement_db_manual.md` §9.2 / §24 | 操作摘要与索引 |

---

## 11. 收尾检查与跨平台回归（2026-11-24 执行）

> 本节是一次**收尾验证**的完整记录：一处日志修正 ＋ macOS 逐项实测 ＋ Ubuntu 侧的静态审计与人工清单。
> 结论先行：**训练数学、图结构、导出/序列化、断点结构均未改动**；唯一代码改动是收尾日志文案（§11.1）。
> 证据分两层：**本机可执行的已执行**（标 PASS ＋ 实际输出），**必须在 Linux/CUDA 上跑的标"需人工"**并给命令。

### 11.1 收尾日志修正（本轮唯一的代码改动）

#### 问题（实测三态）

| 场景 | 目录实际内容 | 修正前收尾行 |
|---|---|---|
| 默认 `--select-on val`（val 有限）| `depth3_best.keras` **存在** | `best val loss 4.63567; wrote …/depth3_best.keras and history.json` ✔ |
| **`--select-on none`** | **没有** `depth3_best.keras` | 仍打印 `wrote …/depth3_best.keras` ✘（`best val loss inf`）|
| `--no-val --monitor-every 0`（chosen=nan）| **没有** | 同上 ✘ |

★ 实测来源：**2026-11-23 那轮真跑**（`--select-on none`）的目录里只有
`depth3_last.keras / history.json / resume.json / snapshots / val_llr_stats.npy`，而日志声称写了 best。

#### 下游依赖核查（决定改法的关键）

| 消费者 | 行为 | 影响 |
|---|---|---|
| `doc_chinese/llr_ai_detection/work_tmp/tmp/finalize_conditioned.sh:36` | `case "$last" in *epoch*\|*"best val loss"*) ;; *) exit 1 ;; esac`，随后**自己** `[ -f "$R/depth3_best.keras" ]` | 只依赖**前缀**，且不信任 "wrote" 文案 ⇒ 改文案不会让它挂住或误判 |
| `ai_train/finish_run.sh:27` | `rsync -a "$SRC/depth3_best.keras" …` | 直接操作文件；缺文件本来就会失败 |
| 其它 `.py/.sh` | `grep -rn "best val loss"` 无命中 | 无消费者 |

#### 修改（3 处，仅日志；`train_depth3.py`）

```diff
@@ main(): 2505
     history = []
     best = np.inf
+    # WHETHER A BEST CHECKPOINT EVER REACHED DISK. …
+    best_written = False
     epochs_without_improvement = 0
@@ main(): 2670
             model.save(os.path.join(args.out, "depth3_best.keras"))
+            best_written = True
@@ main(): 2737
-    print(f"best val loss {best:.5f}; wrote {args.out}/depth3_best.keras and history.json")
+    if best_written:
+        print(f"best val loss {best:.5f}; wrote {args.out}/depth3_best.keras and history.json")
+    else:
+        print(f"best val loss {best:.5f}; no best checkpoint kept (select-on {args.select_on}); "
+              f"wrote {args.out}/history.json")
```

- 位置：`train_depth3.py:2510 / 2676 / 2737–2741`
- **正常路径的文案逐字未变**；前缀 `best val loss N;` 不变；`history.json` 写入逻辑未动。
- **没有任何数值路径被触碰**（`py_compile` 通过；`pyflakes` 仅剩两处既有未用导入）。

#### 修正后验证（四种状态，全部 PASS）

| 状态 | 实际输出 |
|---|---|
| 默认（val 有限）| `best val loss 4.63567; wrote /tmp/t3/chk/val/depth3_best.keras and history.json`（文件存在，**与旧版逐字相同**）|
| `--select-on none` | `best val loss inf; no best checkpoint kept (select-on none); wrote /tmp/t3/chk/none/history.json`（目录无 best）|
| `--no-val --monitor-every 0` | `best val loss inf; no best checkpoint kept (select-on val); wrote …/history.json`（目录无 best）|
| `--select-on band` | `best val loss 0.50215; wrote …/depth3_best.keras and history.json`（0.50215 是 **BER**，同轮 val 为 5.12197/5.00984 ⇒ 确由 band 选中）|

### 11.2 macOS 回归（本机已执行，环境 = §1 的 venv）

| # | 检查项 | 结果 | 依据（实际输出） |
|---|---|---|---|
| M1 | 横幅 · 默认 | **PASS** | `xla: off` ＋ 原因行；`platform: compute placement: a 64x64 matmul lands on GPU:0 (the Apple GPU carries the step)` |
| M2 | 横幅 · `--no-gpu` | **PASS** | `gpus: all GPU devices hidden (--no-gpu); the step will run on the CPU`；`platform: Apple GPU: /physical_device:GPU:0 (hidden by --no-gpu), memory growth on`；`placement: … lands on CPU:0` |
| M3 | 显式 `--jit`（自检/降级）| **PASS** | 自检失败：`NotFoundError: could not find registered platform with id: 0xb45ade790 [Op:__inference_probe_10]` ⇒ 打印 `xla: off -- the requested XLA step could not be compiled on this backend (…)` 后**照常完成 epoch**。★ 语义：GPU 可见时探针张量落在 Metal 上，XLA 无法编译 ⇒ `--jit` 在 Mac 上**恒降级**，这正是安全设计；此前观测到 `xla: on` 的那次是 CPU 落点 |
| M4 | 半精度护栏 | **PASS** | `--dtype float16` → **exit 2** ＋ 一行 parser error；`~/Library/Logs/DiagnosticReports/*.ips` 计数 **0**（未进入 `libmetal_plugin`）|
| M5 | 吞吐/CPU · 轻模型（c32/18 blk，b8，256 样本/轮）| **PASS** | GPU `0.18 s/step · 22.4 ms/sample · 101% 单核 · RSS 2.4 GiB`；CPU `0.08 s/step · 10.3 ms/sample · 750% · 1.2 GiB` ⇒ **轻模型 CPU 快 2.2×** |
| M6 | 吞吐/CPU · 重模型（c32/36 blk dense+SE，b16）| **PASS** | GPU `0.99 s/step · 61.8 ms/sample · 127% · 8.5 GiB`；CPU `1.17 s/step · 73.3 ms/sample · 907% · 3.4 GiB` ⇒ **重模型 GPU 快 1.19×**。★ 同日 GPU 步时在 0.82–0.99 s 间漂移（热/负载），**比值**稳定 |
| M7 | `footprint` vs `RSS` | **PASS** | 重模型 b32 实测：`RSS 8.76 GiB` vs **`phys_footprint 16 GB`**（graphics 7 976 MB ＋ MALLOC_SMALL 7 953 MB ＋ MALLOC_LARGE 219 MB ＋ VM_ALLOCATE 205 MB）⇒ RSS 低估 ≈1.8×，且**完全看不到 Metal 缓冲** |
| M8 | `--resume` ＋ 快照 | **PASS** | `resumed from …/depth3_last.keras: next epoch 3, best 6.55649, 0 epoch(s) without improvement, 2 earlier epoch(s) of history carried over`；`snapshots/epoch_0001/0002_periodic.{keras,json}` 齐全 |
| M9 | 含 `MaskedSmooth` 的检查点 | **PASS**（加载）| `load_model(…, CUSTOM_OBJECTS)` → outputs `['llr','post_eq_noise']`，层 `smooth_ms` 存在 |
| M10 | 完整 `--export-only` | **需人工** | 本机 venv 无 coremltools：已**通过加载**，停在 `export_deployment` 的 `import coremltools` ⇒ 导出名称与 `depth3_*` metadata 需在装 coremltools 的机器（如 203/M2）上按 §11.6 复核 |

### 11.3 Ubuntu / CUDA 回归

| # | 检查项 | 结果 | 依据 |
|---|---|---|---|
| U1 | 日志头不变 | **PASS**（模拟）| `IS_DARWIN=False` 跑同命令：`xla: on`，**无 `platform:` / `gpus:` 行**，收尾行与旧版逐字相同 |
| U2 | 默认 `--workers` 仍串行 | **PASS**（单测）| `pipeline_defaults(workers=0) → (0, 0)`；显式 `4 → (4, 8)` |
| U3 | XLA 策略不被 macOS 逻辑影响 | **PASS**（单测）| `resolve_xla(True, []) → (True, None)`；`resolve_xla(False, ["--no-jit"]) → (False, None)` |
| U4 | 导出契约（函数级）| **PASS**（静态）| `export_deployment / rename_outputs / export_buckets / MaskedSmooth / FoldedScaleConv / decode_metrics / unfold_plane / load_snr_nominal / cli_snr_range / cli_sinr_cal` 与 `git HEAD` **逐字节相同** |
| U5 | `ChannelGate` 的 fp32 乘积 | **PASS**（静态）| 与 HEAD 差 5 行，其中**非注释行 0 行**（只加了说明文字）|
| U6 | `build()` 图结构 | **PASS**（静态）| 8 行非注释差异**全部**是同一提交里带进来的 `dense_skip_depth` 改动（签名默认值、`recent` 列表、`visible` 组装）；默认 `depth=1 / entry=False` 时 `recent=[]`、`visible=[y, block]`，与 HEAD **等价** |
| U7 | 数值一致性（`--init` 固定权重）| **需人工在 106 执行** | 命令见 §11.6；本仓库内的同方法记录：bit-identical（§7，含 fp32/fp16 两套）|
| U8 | `--select-on band` | **PASS**（同代码路径，本机）| best=0.50215（BER）而 val=5.12/5.01 ⇒ band 驱动选择，best 已写 |
| U9 | 导出文件名 / 头名称 / `depth3_*` metadata | **需人工** | 函数体未变（U4）是静态保证；依赖升级后仍需按 §11.6 复核实际产物 |
| U10 | 历史 run 是否用过 `--modcond --target genie` | **需人工核查** | 见 §11.4 |

### 11.4 `--modcond --target genie` 的影响核查项

| 项 | 判据 |
|---|---|
| 该臂是否**同时**用了 `--modcond` 与 `--target genie` | 查该 run 的启动日志 `argv:` 行，或启动脚本 |
| 若用过 | 旧实现只缓存**第一次**调用的 qm 星座图 ⇒ 混合调制时标签错误且**静默**；这些 run 的 `train/val/BER` 只代表"用错标签训出来的模型"，**不能与新代码或别的臂比较** |
| 处置 | 重训（推荐）；至少重评：用新代码的 `genie_labels` 重新生成标签，与旧标签逐样本对比，量化受影响比例 |
| 自检 | 新代码下逐样本检查：每个 symbol 的 stride-8 块尾（通道 `qm..7`）应**全为 0** |

### 11.5 macOS 验证命令（可直接复制）

```bash
P=~/.venvs/tf-metal-2.18/bin/python
T=~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py
B="--train ~/ai_depth3/datasets_monitor_1102a --val ~/ai_depth3/datasets_monitor_1102a \
   --modcond --layout spatial --in-channels 12 --c-hid 32 --blocks 18 --no-reporting-heads \
   --batch 8 --max-train 64 --max-val 8 --epochs 1 --bler-samples 0 --group-report 0 --seed 7"

# 1) 设备落点（默认 / 隐藏 / 显式 jit）
$P $T $B --out /tmp/chk/gpu                       | grep -E "^xla:|^platform: compute"
$P $T $B --out /tmp/chk/cpu --no-gpu              | grep -E "^gpus:|^platform: (Apple|compute)"
$P $T $B --out /tmp/chk/jit --jit                 | grep -E "^xla:|^platform: compute"   # 预期：自检失败→xla: off

# 2) 半精度护栏（预期 exit 2，且不产生崩溃报告）
$P $T $B --out /tmp/chk/fp --dtype float16; echo "exit=$?"
ls ~/Library/Logs/DiagnosticReports/*.ips 2>/dev/null | wc -l    # 预期 0

# 3) 收尾行四态
$P $T $B --out /tmp/chk/val                           | tail -2; ls /tmp/chk/val  | grep -c depth3_best   # 1
$P $T $B --out /tmp/chk/none --select-on none         | tail -2; ls /tmp/chk/none | grep -c depth3_best   # 0
$P $T $B --out /tmp/chk/nv --no-val --monitor-every 0 | tail -2
$P $T $B --out /tmp/chk/band --select-on band         | tail -2                                          # best 来自 band

# 4) 内存（跑起来后另开终端；RSS 会骗人）
/usr/bin/footprint -p <训练进程 PID> | head -12

# 5) resume ＋ 快照 ＋ MaskedSmooth 加载
$P $T $B --out /tmp/chk/rs --smooth-k 3 --snapshot-every 1
$P $T $B --out /tmp/chk/rs --smooth-k 3 --epochs 2 --resume | grep -E "^resumed|^epoch"
$P -c "import sys;sys.path.insert(0,'$HOME/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train');\
import depth3_paths;depth3_paths.add_ce_shared();import train_depth3 as T,tensorflow as tf;\
m=tf.keras.models.load_model('/tmp/chk/rs/depth3_last.keras',compile=False,custom_objects=T.CUSTOM_OBJECTS);\
print([l.name for l in m.layers if isinstance(l,T.MaskedSmooth)])"
```

### 11.6 Ubuntu / CUDA 验证命令（需在 106/107 上执行）

```bash
# 0) 环境：LD_LIBRARY_PATH 指向 venv 里的 nvidia/*/lib（否则 TF 看不到 GPU）
export LD_LIBRARY_PATH=$(ls -d .venv/lib/python*/site-packages/nvidia/*/lib | tr '\n' ':')

# 1) 日志头与默认参数：应看到 xla: on，且没有 platform:/gpus: 行；不传 --workers 时串行
.venv/bin/python -u train_depth3.py --train data/<set> --val data/<monitor> --out runs/_chk \
  --modcond --layout spatial --in-channels 12 --c-hid 32 --blocks 36 --dense-blocks \
  --dense-skip-entry --se --batch 16 --max-train 256 --max-val 32 --epochs 1 \
  --bler-samples 0 --group-report 0 2>&1 | tee /tmp/chk.log | head -8
grep -E "^xla:|platform:|^gpus:" /tmp/chk.log        # 期望：只有 xla: on

# 2) 数值一致性：同一 --init、同一 seed、2 轮，逐位对比
for arm in before after; do
  .venv/bin/python -u train_depth3.py … --init fixed.keras --out runs/$arm --epochs 2 --max-train 400
done
diff <(python -c "import json;print(json.load(open('runs/before/history.json'))['history'])") \
     <(python -c "import json;print(json.load(open('runs/after/history.json'))['history'])")

# 3) 选择策略
.venv/bin/python -u train_depth3.py … --select-on band --max-val 1000 --epochs 2 --out runs/band
grep "best val loss" runs/band/*.log     # best 应来自低 SNR band，且 depth3_best.keras 存在

# 4) 导出契约（名称 / 头 / metadata / 桶文件名）
.venv/bin/python -u train_depth3.py --export-only runs/<臂>/depth3_best.keras \
  --export-probe /tmp/<name>.mlpackage --modcond --layout spatial --in-channels 12 \
  --blocks 36 --c-hid 32 --no-reporting-heads
python - <<'PYX'
import coremltools as ct
spec = ct.models.MLModel("/tmp/<name>.mlpackage").get_spec()
print([o.name for o in spec.description.output])                  # llr / post_eq_noise(/ ch_est / dmrs_noise)
print(sorted(spec.description.metadata.userDefined.keys()))       # depth3_layout / depth3_nsc / depth3_snr_range …
PYX
.venv/bin/python -u train_depth3.py --export-only runs/<臂>/depth3_best.keras \
  --export-probe /tmp/<name>.mlpackage --export-buckets 12,24 --modcond …
ls /tmp/<name>_w12.mlpackage /tmp/<name>_w24.mlpackage            # 文件名派生规则未变
```

### 11.7 静态审计口径（§11.3 的 U4–U6 是怎么得出的）

用 AST 把两个版本的**顶层函数/类按名字切成单元**再逐字节比较（`ast.get_source_segment`），比 `git diff` 更容易回答"契约函数到底动没动"：

```python
import ast
def units(path):
    src = open(path).read()
    return {n.name: ast.get_source_segment(src, n) for n in ast.parse(src).body
            if isinstance(n, (ast.FunctionDef, ast.ClassDef))}
cur  = units("<仓库>/train_depth3.py")
head = units("/tmp/head_train_depth3.py")          # 由 git show HEAD:<path> 导出
print("ADDED  :", sorted(set(cur) - set(head)))
print("CHANGED:", sorted(n for n in set(cur) & set(head) if cur[n] != head[n]))
```

本轮结果：

| 分类 | 内容 |
|---|---|
| **逐字节相同**（契约相关）| `export_deployment`、`rename_outputs`、`export_buckets`、`MaskedSmooth`、`FoldedScaleConv`、`decode_metrics`、`unfold_plane`、`load_snr_nominal`、`cli_snr_range`、`cli_sinr_cal` |
| 变化但**仅注释** | `ChannelGate`（5 行，非注释 0 行）|
| 变化 = **同一提交带进来的** `dense_skip_depth` | `build`（8 行非注释；默认参数下与 HEAD 等价）|
| 变化 = 两轮 macOS/host 改动 | `Batches`、`genie_labels`、`stack`、`main` ＋ 新增的 12 个平台/工具函数（`_GeometryCache`、`resolve_xla`、`xla_self_test`、`configure_macos_runtime`、`pipeline_defaults`、`report_macos_runtime`、`probe_compute_device`、`hide_gpus`、`allow_metal_fp16`、`_constellation`、`threshold_weights`、`_flag_was_given`）|

### 11.8 本次检查的边界与遗留

| 项 | 状态 |
|---|---|
| 训练数学 / 模型结构 / 损失 / 梯度 / LLR 缩放 / 序列化 / 断点结构 | **未改动**（§11.7 静态 ＋ §7 数值对拍）|
| 导出与 CoreML 元数据 | 函数体未变；**实际产物需人工复核**（§11.6 第 4 步）|
| Ubuntu 数值 A/B | **需人工**（§11.6 第 2 步）|
| `--modcond --target genie` 历史 run | **需人工核查**（§11.4）|
| 每轮耗时的绝对值 | GPU 步时受热/负载影响可漂 0.82–0.99 s；结论只用**比值**与**同批次内对比** |
