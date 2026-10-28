# 机器算力与模型训练 memo（香港 192.168.31.* ＋ 广州 192.168.0.*）

**目的**：把"哪台机器能跑什么、跑多快、怎么装"写在一处，省掉每次训练前重新试错。

**范围**：本文件讲**算力、训练环境**与**模型训练的完整步骤（含断点续训）**。网络/手机接入见 `test_environment/香港测试环境与手机接入问题总结_2026-09-05.md`；
训练配方与判据见 `llr_ai_detection_design_and_implementation_1.md`（尤其 §19.298、§19.323–§19.325）。

---

## 1. 机器清单

| 机器 | 地址 | 系统 | CPU | 内存 | GPU | 角色 |
|---|---|---|---|---|---|---|
| **开发 Mac**（本机） | 192.168.31.121 | macOS 26.6.2 | **Apple M4 Pro，14 核** | **64 GB** | 集成 | 开发、导出 Core ML、飞行；**夜间可训练** |
| NUC-1 | **192.168.31.211** | Ubuntu | 16 线程 | 31 GB | 无 | 训练（长期）、**核心网 Open5GS** |
| NUC-2 | **192.168.31.66** | macOS 13.7.8 | Intel Xeon E-2286M，8 核/16 线程 | **32 GB** | 无 | **训练**（★ x86 ⇒ 不能导出/分桶/飞行）|
| Asus notebook | **192.168.31.107** | Ubuntu | 16 线程 | **15 GB** | **RTX 3060 Laptop 6 GB**（compute 8.6） | **主力训练**（最快） |
| M2 MacBook | 192.168.31.203 | macOS 26.6.2 | Apple M2，4P+4E | **8 GB** | 集成 | ★ **可导出 Core ML／分桶／飞行**（Apple Silicon），但内存 8 GB ⇒ 只适合评估与小规模训练（见 §5）|
| 2016 MacBook Pro | （未接入） | macOS | Intel i7 4C/8T | 16 GB | — | **仅评估/截断训练**（太慢，见 §5） |

登录：三台训练机与 M2 均**免密 SSH**，用户名 `jwang`（211/107）与 `jiachengwang`（66/203）；工作目录 **`~/work/ai_phy_train`**。

**★ 上表是香港的机器（192.168.31.*）。2026-10-28 起人已移到广州（192.168.0.*），香港那批从广州**不可达**，
上面的数字只能在回到香港或打通专线后使用。广州的两台见下表与 §9.1。**

| 机器 | 地址 | 系统 | CPU | 内存 | GPU | 磁盘可用 | 角色 |
|---|---|---|---|---|---|---|---|
| **广州-106** | **192.168.0.106**（`jwang`） | Ubuntu 24.04.3 | Xeon E-2286M **8C/8T（无 HT）** | **16 GB** | 无 | **416 G** | **语料仓库** ＋ 小切片训练 ＋ 消融 |
| **广州-221** | **192.168.0.221**（`jiachengwang`） | macOS 12.7.6 | i7-9750H **6C/12T** | **32 GB** | 无 | **255 G** | ★ **主力训练**（能装下 e17 全量）；★ **x86 ⇒ 不能导出/分桶/飞行** |

---

## 2. 实测算力（唯一可信的口径）

**测量方法**（同一份代码、同一批超参、同一份语料）：

```bash
time .venv/bin/python -u lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py \
  --train data/<corpus>.npz --val data/<val>.npz --out runs/_speed \
  --modcond --layout spatial --blocks 18 --c-hid 32 --in-channels 12 \
  --no-reporting-heads --w-ch 0.1 --w-noise 0.3 --batch 8 --epochs 1 \
  --max-train 2000 --max-val 128
```

| 机器 | **samples/s** | E4 全量（43 902 × 40 epoch） |
|---|---|---|
| **Asus 107（RTX 3060）** | **~122** | **≈4 h** |
| 开发 Mac（M4 Pro） | ~89 | ≈5.5 h |
| NUC-1 211（Ubuntu） | ~19 | ≈26 h |
| NUC-2 66（Intel Mac） | ~19 | ≈26 h |
| **广州 221（i7-9750H）** | **15.6** | ≈31 h |
| **广州 106（Xeon 8C/8T）** | **12.7** | ≈38 h |
| 2016 MBP（推算） | ~9–11 | ≈46–54 h |

**模型宽度的影响（2026-10-28，221 实测；`e17_train` 5 000 样本切片，batch 8，`--blocks 18`）**：

| `--c-hid` | 参数量 | 1 epoch（5 000 样本）| **峰值内存** | samples/s | 40 epoch 跑完 10 万样本 |
|---|---|---|---|---|---|
| 32（现役）| 26 185 | 282 s | **13.4 GB** | 17.7 | ≈63 h |
| **64** | **89 225** | 632 s | **14.8 GB** | 7.9 | ≈141 h（**6 天**）|
| **128** | **325 897** | 1 242 s | **15.1 GB** | 4.0 | ≈278 h（**11.6 天**）|

**★ 结论一：32 GB 内存**完全够用** c64/c128**——峰值只从 13.4 涨到 15.1 GB，因为**主项是语料本身**
（5.5 GB 的 npz 在内存里物化后约 11 GB），而**宽度每翻一倍只多约 0.5–1 GB**。
**⇒ 限制宽度的不是内存，是时间**（c64 慢 2.2×，c128 慢 4.4×）。

**★ 结论二：宽度带来质量**——同一条 5 000 样本切片、只跑 1 epoch：
c32 的 `coded BLER 1.0000 / BER 0.4457`；c64 降到 `0.8974 / 0.1467`；**c128 直接 `coded BLER 0.0000 / BER 0.0146`**。
**⇒ 1 epoch 当然不算收敛，但"更宽"明显更快离开欠拟合区。**

**两条重要结论**：
1. **瓶颈不在 GPU**。3060 只比 M4 Pro 快 1.4×，因为数据管道（float16→float32、逐样本 numpy）是 CPU 侧开销。
   想让 3060 跑出 10× 需要改输入管道，不在当前计划内。
2. **CPU 侧的两台 NUC 慢 5–6×**，适合跑"多快好省"的并行消融（一条腿一条机器），不适合跑必须尽快出结果的实验。

---

## 3. 内存要求（决定"能不能跑"，不是"快不快"）

语料在内存里的大小（float16 存储，实测）：

| 语料 | 样本 | `X` | `H` | 合计 | 最小机器内存 |
|---|---|---|---|---|---|
| 旧语料（A 轨基线） | 30 777 | 2.4 GB | 1.1 GB | **≈3.9 GB** | 8 GB 起 |
| **E3**（＋合成窄分配） | 39 662 | 3.1 GB | 1.4 GB | **≈5.1 GB** | 12 GB 起 |
| **E4**（＋真实低 SNR） | 43 902 | 3.4 GB | 1.5 GB | **≈5.6 GB** | **16 GB 起** |

外加 TensorFlow 自身约 2.5–3 GB。⇒ **E4 需要 ≥16 GB**；15 GB 的 107 号机跑 E4 时峰值约 9 GB，**不能并行第二条**。

**★ e17 语料（100 004 样本）实测：16 GB 装不下**（2026-10-28，106 上被 OOM 杀掉）：

```
Out of memory: Killed process (python) total-vm:18435688kB
```

| 语料 | 样本 | npz 磁盘 | 说明 |
|---|---|---|---|
| `e17_train` | **100 004** | **5.5 GB** | ★ **需要 >16 GB 内存**（`np.load` 全量物化 ＋ TF 张量）|
| `e17_monitor` | 1 523 | 86 MB | 验证集，无压力 |
| `e17_test` | 3 294 | 155 MB | — |

**★ 内存到底被谁吃掉（实测，`e17_train/depth3.npz` 逐个数组量出来的）**：

| 数组 | dtype | 形状 | 常驻内存 |
|---|---|---|---|
| **`X`**（网格） | float16 | (100 004, 612, 64) | ★ **7.30 GB** |
| **`H`**（信道） | float16 | (100 004, 612, 14, 2) | ★ **3.19 GB** |
| `bits` | uint8 | (100 004, 7 344) | 0.68 GB |
| `tb_bits` | uint8 | (100 004, 6 787) | 0.63 GB |
| **合计** | | | ★ **11.81 GB** |

| 项 | 值 | 占比 |
|---|---|---|
| **语料常驻** | **11.81 GB** | ★ **≈99%** |
| TensorFlow 自身 | 2.5–3 GB | — |
| **模型参数**（c32→c128）| **26 k → 326 k 参数 ＝ 0.0001 → 0.0013 GB** | ★ **可以忽略** |

**⇒ 吃内存的是**语料**，不是模型。** npz 在磁盘上只有 5.5 GB，是因为它**压缩存储**；`np.load` 后按原 dtype 展开就是 11.8 GB，
再加 TF 与 float32 转换 ⇒ 峰值 13–15 GB（221 实测 13.4/14.8/15.1 GB，见 §2 的宽度表）。

**在 16 GB 机器上想训，只有三条路**：①**先切片**成小 npz（2 万样本 ≈ 2.4 GB）；②加 swap（需 sudo）；③只当语料仓库。

**⇒ 结论：`e17_train` 只能在 ≥32 GB 的机器上直接训（221 ✓、开发 Mac ✓）；106（16 GB）只能做
①语料仓库 ②小切片训练（例如先抽 2 万样本另存一个 npz）③并行消融。**

---

## 4. 环境安装（每台机器只需一次）

三台训练机都**不要用系统 Python**（3.9/3.12 要么太旧，要么受 PEP 668 限制）：

```bash
# 1) 装 uv（无需 sudo，落在 ~/.local/bin）
curl -LsSf https://astral.sh/uv/install.sh | sh

# 2) 建 venv（211/66 用 3.11 或 3.12 均可；66 是 macOS x86_64，上限 TF 2.16.2）
cd ~/work/ai_phy_train
~/.local/bin/uv venv --python 3.12 .venv
~/.local/bin/uv pip install --python .venv/bin/python "tensorflow[and-cuda]" numpy   # 107（有 N 卡）
~/.local/bin/uv pip install --python .venv/bin/python tensorflow numpy               # 211 / 66（纯 CPU）
```

**⚠ 107 号机的关键一条**：`tensorflow[and-cuda]` 装好后 **GPU 默认不注册**（`list_physical_devices("GPU")` 为空，
训练静默回落 CPU，慢 5×）。必须把 venv 里的 CUDA 库放进 `LD_LIBRARY_PATH`：

```bash
NV=$(ls -d .venv/lib/python3.12/site-packages/nvidia/*/lib | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
# 验证：python -c "import tensorflow as tf; print(tf.config.list_physical_devices('GPU'))"
```

这条已封装进 `~/work/ai_phy_train/run_train.sh`（107 号机）。

| 机器 | Python | TensorFlow | 备注 |
|---|---|---|---|
| 211 | 3.12 | 2.21（CPU） | — |
| 66 | 3.10.5（`/usr/local/bin/python3.10`） | **2.16.2**（x86_64 上限，Keras 3 ✓） | 系统自带 3.9 太旧 |
| 107 | 3.12 | 2.21（GPU，需 `LD_LIBRARY_PATH`） | 驱动 580 |

**★ 广州（以及任何境内网络）必须用镜像装包**：

```bash
./.venv/bin/pip install -i https://pypi.tuna.tsinghua.edu.cn/simple tensorflow==2.16.2   # 实测 24.7 MB/s
```

默认 PyPI 在广州实测**卡死**（572 MB 的 TF 轮子 40 分钟无进展），换清华镜像后两三分钟装完。

**⚠ macOS 上缺两个常用命令**：`timeout` 与 `setsid` 都没有 ⇒ 后台启动用 `nohup cmd > log 2>&1 &` 即可，不要套 `setsid`。

**代码布局**（四台相同，且**不能压平**）：

```
~/work/ai_phy_train/
  lib/phy/upper/ai_signal_processors/depth3/ai_train/               # depth3 训练代码
  lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/ # 借用的 DSP 工具（dmrs/synth/...）
  data/  runs/  logs/
```

`depth3_paths.py` 会**向上三级**再进 `signal_processors/...`，压平就报
`the AI-CE training tree is not at ...`。

---

## 5. 两台"评估机"为什么不进训练轮转

| 机器 | 原因 |
|---|---|
| **M2 / 8 GB（.203）** | 语料 3.9–5.6 GB ＋ TF 2.5 GB 放不下；且机器**已在换页**（swap 6/7.2 GB）。只适合分箱 BER、LLR 诊断这类逐样本评估。★ **但它是 Apple Silicon ⇒ 是"导出 Core ML／分桶／飞行"的可用机器**（与 66/221 相反），只是不能在上面跑大训练。 |
| **2016 MBP / i7 4C8T / 16 GB** | 内存够（16 GB ✓），但吞吐推算 9–11 samples/s ⇒ 一条 E 轨 46–54 h。只适合截断训练（`--max-train ≤8000`）。 |

---

## 6. 排期建议（谁跑什么）

| 场景 | 用哪台 |
|---|---|
| ★ **在广州（2026-10-28 起）** | **221（32 GB，15.6/s）＝主力训练**；**106（16 GB，12.7/s）＝语料仓库 ＋ 小切片**；导出/飞行回 **Apple Silicon** |
| 出结论要快（E 轨、A/B 候选） | **107（122/s）**，其次开发 Mac（**仅在香港可达**）|
| 多条消融并行 | 211 ＋ 66（各 19/s，各可 2 条 × 8 线程；**香港**）|
| 夜间长跑 | 开发 Mac（过夜）＋ 107（连续；**香港**）|
| ★ **导出 Core ML / 分桶 / 飞行** | ★ **只能在 Apple Silicon 的 macOS 上**（`coremltools` ＋ `xcrun coremlcompiler`，目标是 ANE）⇒ **开发 Mac（M4 Pro，64 GB，首选）** 或 **M2 MacBook Pro（8 GB，可用但吃紧）** |
| ✗ **不能做上面三件事的** | ★ **66（香港 macOS 13）与 221（广州 macOS 12）都是 x86_64** ⇒ **只能训练**，**不能**导出 Core ML、**不能**分桶、**不能**飞行 |
| 评估（分箱 BER、符号一致） | 任意；内存小的优先 211/107（香港）或 106（广州）|

**经验值**：一台机器并行 2 条训练时，每条用 `OMP_NUM_THREADS=8`（16 线程机器），耗时约为单条的 1.3–1.6 倍。

---

## 7. 操作踩坑（都真实发生过）

| 坑 | 现象 | 对策 |
|---|---|---|
| `python` 不带 `-u` | 日志只有 TF 启动几行，进程其实在跑（数百 % CPU） | **一律 `python -u`** |
| 一条 ssh 里用 `\` 续行拼多条 `nohup` | 第二条的重定向失败 ⇒ 训练根本没起来 | **一条 ssh 一条命令** |
| NUC-1 同时跑核心网 | 训练吃满 CPU 会让 Open5GS 抖动 | 飞行/接入测试时把 211 上的训练限到 1 条 |
| 107 GPU 静默回落 | 见 §4 | `LD_LIBRARY_PATH` |
| **驱动与用户态库版本不匹配** | `nvidia-smi` 报 `Driver/library version mismatch`，CUDA 失效，训练**照常跑但慢 4.4×**（实测 28 vs 122 samples/s，2026-10-23） | **重启机器**
| ★ **默认 PyPI 在国内装不动** | `pip install tensorflow` 40 分钟无进展（572 MB 轮子） | ★ `-i https://pypi.tuna.tsinghua.edu.cn/simple`（24.7 MB/s）|
| ★ **删了大文件但 `df` 不变** | APFS **本地 Time Machine 快照**仍引用那些块；221 上删 48 G 毫无变化 | ★ `tmutil listlocalsnapshots /` ⇒ `sudo tmutil deletelocalsnapshots <date>`；或关掉本机自动备份（221 上由此 **26 G → 255 G**）|
| ★ **macOS 没有 `timeout` / `setsid`** | 脚本里用了就 `command not found` | 用 `nohup cmd > log 2>&1 &` |
| ★ **`pkill -f <名字>` 会杀掉自己** | 通过 ssh 执行的命令里含该字符串 ⇒ 把自己的会话打断（多次踩） | 用括号技巧 `pkill -f "final_queue[.]sh"` 或**按 PID 杀** |
| ★ **`ls dir/*.bin` 在几万文件时静默返回 0** | `ARG_MAX` 溢出，`ls` 失败但被 `2>/dev/null` 吞掉 | 一律 `find dir -name "*.bin" | wc -l` |
| ★ **shell 变量不联动** | 只改 `LABEL` 不改 `L`，`wc -l $L/...` 读到上一条腿 | `LABEL=...; L=.../$LABEL` **两行一起写** |对齐版本；开训前先跑 `list_physical_devices("GPU")` 确认非空 |

---

## 8. 模型训练：完整步骤（含断点续训）

**一句话**：一条命令启动，日志看 epoch 行，产物是 `depth3_best.keras`；**中途断了不用重跑**，加 `--resume`
（或用下面的自动重启包装脚本）从上一个 epoch 继续。

### 8.1 一次训练的全部输入

| 输入 | 位置 | 说明 |
|---|---|---|
| 训练代码 | `~/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/` | ＋ 同级的 `.../metal/ai_train/`（借用的 DSP 工具），**布局不能压平** |
| 训练语料 | `data/e17_train/depth3.npz` | 100 004 样本，5.5 GB |
| 验证语料 | `data/e17_monitor/depth3.npz` | 1 523 样本 |
| Python 环境 | `.venv/` | §4；TF 2.16.2（221）/ 2.21（106）|
| 输出目录 | `runs/<名字>/` | 全部产物与检查点 |

### 8.2 标准命令（改三个地方即可复用）

```bash
cd ~/work/ai_phy_train
./.venv/bin/python -u lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py \
  --train data/e17_train/depth3.npz --val data/e17_monitor/depth3.npz \
  --out runs/c128_e17 \
  --modcond --layout spatial --blocks 18 --c-hid 128 --in-channels 12 \
  --no-reporting-heads --w-ch 0.1 --w-noise 0.3 \
  --batch 8 --epochs 60 --patience 3 --bler-every 5 --bler-samples 8 \
  --resume
```

| 参数 | 含义 | 怎么选 |
|---|---|---|
| `--train` / `--val` | 训练／验证 npz | 验证集**永远不要**与训练集重叠 |
| `--out` | 产物目录 | 一次实验一个目录，断点续训靠它 |
| `--layout spatial` | 输入平面布局 | 与部署一致，别改 |
| `--blocks` | 残差块数 | 现役 18 |
| **`--c-hid`** | ★ **通道宽度** | **32 / 64 / 128**（见 §2 的宽度表）|
| `--in-channels` | 输入通道 | 12 |
| `--no-reporting-heads` | 关掉上报头 | 只训 LLR 主头时用 |
| `--w-ch` / `--w-noise` | 辅助损失权重 | 0.1 / 0.3 |
| `--batch` | 批大小 | 8（内存宽裕可 16）|
| `--epochs` | 上限 | 60；早停会提前结束 |
| **`--patience`** | ★ **早停耐心** | 3（验证损失 3 个 epoch 不改善就停）|
| `--bler-every` / `--bler-samples` | 每 N 个 epoch 解一次 LDPC 算 BLER／每次解几块 | 5 / 8（解码很贵）|
| **`--resume`** | ★ **断点续训** | 有检查点就接着跑 |

### 8.3 训练过程中看什么

```bash
tail -f runs_c128.log
```

**每行 epoch 都要读这六个字段**：

```
epoch   12  train 0.04521  val 0.09311  uncoded BER 0.0149  coded BLER 0.0000  mean|LLR| 31.2  (25234s)
```

| 字段 | 含义 | 健康的样子 |
|---|---|---|
| `train` / `val` | 训练／验证损失 | 两者一起降；val 长期不降 ⇒ 过拟合或该早停 |
| **`uncoded BER`** | **解码前**误比特率（LLR 取符号与真值比）| 从 0.5 往下降（0.5＝瞎猜）|
| **`coded BLER`** | **解码后**误块率（过真实 LDPC ＋ 重编码校验）| ★ **这才是判据**；越低越好 |
| `mean|LLR|` | LLR 平均幅度 | 从 0 涨到 20–40（涨不动＝没学到）|
| 括号里的秒数 | 本 epoch 墙钟时间 | 与 §2 的 samples/s 对照 |

**★ 判据不是 BER，是 coded BLER**（设计 §2.3）：BER 只说"指向对不对"，只有解码才知道码还能不能纠回来。

### 8.4 产物

| 文件 | 用途 |
|---|---|
| ★ `runs/<名字>/depth3_best.keras` | **部署用**（验证损失／低 SNR 带最优的那个 epoch）|
| `depth3_last.keras` | ★ **续训用**（含优化器状态，最后一个 epoch）|
| `resume.json` | `{epoch, best, since}` ⇒ "跑到第几个 epoch"的唯一真相 |
| `history.json` | 每 epoch 的完整指标（画曲线用）|
| `val_llr_stats.npy` | `[val_ber, val_llr_mean, val_bler]` 三列 |

导出 Core ML 用 `depth3_best.keras`，★ **但只能在 Apple Silicon 的 Mac 上做**（开发 Mac 或 M2 MacBook Pro）；**x86 的 macOS（66 / 221）不能导出**——它们只能训练。

### 8.5 ★ 断点续训（2026-10-28 新增）

| 情形 | 行为 |
|---|---|
| **进程崩溃／被杀** | 包装脚本自动重启并 `--resume` ⇒ **最多损失 1 个 epoch** |
| **机器重启／断电** | 检查点在磁盘上；★ **重新执行一次包装脚本**即可继续 |
| **正常早停** | 退出码 0 ⇒ 包装脚本停止，不再重启 |

**机制**：每个 epoch **在最优模型判定之前**写一次 `depth3_last.keras`（`.keras` 格式**同时含权重和优化器状态**）
＋ `resume.json`。`--resume` 读到它们就把 `start_epoch = epoch+1`、`best`、`since` 一并恢复 ⇒
**不是热启动，是真正的续训**（学习率进度、动量都在）。

**自动重启包装脚本**（`~/work/ai_phy_train/run_c128.sh`，改 `--c-hid` 与 `--out` 即可复用）：

```bash
#!/usr/bin/env bash
set -u
cd "$HOME/work/ai_phy_train"
PY=./.venv/bin/python
TRAIN=lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py
OUT=runs/c128_e17
while true; do
  "$PY" -u "$TRAIN" --train data/e17_train/depth3.npz --val data/e17_monitor/depth3.npz \
    --out "$OUT" --modcond --layout spatial --blocks 18 --c-hid 128 --in-channels 12 \
    --no-reporting-heads --w-ch 0.1 --w-noise 0.3 --batch 8 --epochs 60 \
    --patience 3 --bler-every 5 --bler-samples 8 --resume >> runs_c128.log 2>&1
  rc=$?
  echo "trainer exited rc=$rc at $(date +%T)" >> runs_c128.log
  [ "$rc" -eq 0 ] && break      # 0 = 正常结束（含早停）
  sleep 30
done
```

```bash
cd ~/work/ai_phy_train && nohup ./run_c128.sh > /dev/null 2>&1 &
```

**验证过的行为**：

```
$ cat runs/_resume/resume.json
{"epoch": 1, "best": Infinity, "since": 0}
$ ... --epochs 3 --resume
resumed from runs/_resume/depth3_last.keras: next epoch 2, best inf, 0 epoch(s) without improvement
epoch    2 ... epoch 3 ...
```

### 8.6 ★ 从训练到部署的四步（架构决定的分工）

**不能在一台机器上做完**：训练可以在任何 x86 机器上，**导出只能在 Apple Silicon 上**（§6）。

| 步 | 在哪台机器 | 命令／动作 | 产物 |
|---|---|---|---|
| **1. 训练** | **221**（32 GB，能装 11.8 GB 语料）或 106 的小切片 | §8.2 的命令 ＋ `--patience 3 --resume` | `runs/<名字>/depth3_best.keras` |
| **2. 回传** | 从 221 → Apple Silicon Mac | `rsync -a <221>:work/ai_phy_train/runs/<名字>/depth3_best.keras ./` | 本地 `.keras` |
| **3. 导出** | ★ **开发 Mac（M4 Pro）** 或 M2 MacBook | 导出脚本（`export_deployment` 路径，`coremltools` ＋ `xcrun coremlcompiler`）| `.mlmodelc`（ANE 可用）|
| **4. 分桶／飞行／门控** | ★ 同上（Apple Silicon）| 见 llr_ai_detection 的门控流程 | 门控 CSV ＋ 报告 |

**⚠ 两条硬约束**：

1. ★ **66 与 221 是 x86 macOS ⇒ 只能做第 1 步**，导出／分桶／飞行一步都做不了；
2. **106 是 Ubuntu ⇒ 连第 1 步都只能做小切片**（16 GB）。

**⇒ 排队时按这个分工排：221 训练 → 拉回 Apple Silicon → 导出 → 飞行。**

### 8.7 典型耗时（广州 221，100 004 样本）

| 宽度 | 每 epoch | 收敛通常要 | 总时长 |
|---|---|---|---|
| c32 | ≈1.6 h | 20–40 epoch | 1.5–3 天 |
| **c64** | ≈3.5 h | 15–30 epoch | 2–4 天 |
| **c128** | ≈7 h | 8–20 epoch（更宽收敛更快）| **3–6 天** |

**⇒ 排"一周量"时：c128 一条，或 c64 两条串行。**

### 8.8 常见故障

| 现象 | 原因 | 处置 |
|---|---|---|
| `Out of memory: Killed process` | 语料物化 ＋ TF 超出内存 | 换 ≥32 GB 机器，或用 `--max-train` 切片 |
| `the AI-CE training tree is not at ...` | 代码目录被压平 | 保持 §4 的目录层级 |
| `ModuleNotFoundError: tensorflow` | venv 用错 | `./.venv/bin/python` 显式指定 |
| 训练极慢（≈1 sample/s）| 落到了 CPU 而以为在用 GPU | 这些机器**都没有 GPU**，慢是正常的 |
| `timeout: command not found` | macOS 无 `timeout` | 直接跑，或用 `nohup` |

---

## 9. 跨地点环境（2026-10-28 已实地评估广州）

**地点由本机 IP 段推断**——本机（开发 Mac）与 M2 MacBook 会随队移动到广州和北京，所以"在哪"可以直接从网络看出来：

| 地点 | 网段 | 判定 |
|---|---|---|
| **香港** | **192.168.31.*** | 当前所在地 |
| **广州** | **192.168.0.*** | 与香港同网段不同 ⇒ 用 `ipconfig getifaddr en0` 一望即知 |
| **北京** | **192.168.100.*** | 同上 |

**各地机器（初步清单）**

| 地点 | 机器 | 系统 | 备注 |
|---|---|---|---|
| 香港 | 开发 Mac ＋ 3 台训练机 ＋ M2 | 见 §1 | 已实测（§2） |
| **广州** | **2 台 NUC**：**106 = Ubuntu 24.04**、**221 = macOS 12.7.6** | 用户 `jwang` / `jiachengwang` | ★ **已实测（2026-10-28）⇒ 见 §9.1**：106 = 12.7 samples/s、16 GB、416 G；221 = 15.6 samples/s、32 GB、255 G |
| **北京** | **4 台 NUC**：**3 台 Ubuntu**、**1 台 macOS 13** | — | 待测同上 |
| **北京** | **1 台带独立 N 卡的工作站**：**16 CPU / 32 线程** | — | 用户描述为"前面评估过的那台机器（192.168.31.223）"；香港侧同类机型实测为 **RTX 3060 / ~122 samples/s**（§2），北京这台待实测确认卡型与内存 |

### 9.1 广州实测（2026-10-28，本机已移入 192.168.0.*）

| 机器 | 地址／用户 | 系统 | CPU | 内存 | GPU | 磁盘可用 | Python | TF | samples/s |
|---|---|---|---|---|---|---|---|---|---|
| **广州-NUC-1** | **192.168.0.106**／`jwang` | **Ubuntu 24.04.3**（x86_64） | **Xeon E-2286M，8 核 8 线程**（**无 HT**） | **16 GB**（15.3 GiB） | **无独显**（Intel UHD 630） | **416 G** | **3.12.3** | **2.21.0**（cp312） | ★ **12.7**（1 epoch × 2 000 样本 = 157 s） |
| **广州-Mac-NUC** | **192.168.0.221**／`jiachengwang` | **macOS 12.7.6**（x86_64） | **i7-9750H，6 核 12 线程** | **32 GB** | **无独显**（UHD 630） | **255 G**（清理快照后 ✓）| **3.10.5**（系统另有 3.7.3） | **2.16.2**（x86 macOS 上限） | ★ **15.6**（128 s） |

**这两台的关键差异（会直接决定分工）**：

| 项 | 106（Ubuntu） | 221（macOS 12） |
|---|---|---|
| 内存 | **16 GB** ⇒ 与香港 NUC 同档，能跑 E4 级别语料但吃紧 | **32 GB** ⇒ 内存不愁 |
| 磁盘 | **416 G** ⇒ 可放整套语料到 2027 | **255 G**（清理 APFS 快照后；曾误以为只剩 26 G，见 §7）|
| TF 版本 | **2.21.0**（与本机一致）| ★ **2.16.2**（x86 macOS 天花板，Keras 3）|
| Python | 3.12.3 | 3.10.5（**venv 已用 3.10 建**；系统默认 3.7 太旧，且 TF 只到 2.11）|
| 训练代码 | `/work` 不存在 ⇒ 实际目录是 ★ **`~/work/ai_phy_train`** | 同左 |

**⚠️ 路径更正**：两台机器上用户建的是 **`~/work/ai_phy_train`**（家目录下），不是 `/work/ai_phy_train`；`/work` 在两边都不存在，且 `sudo` 需要密码 ⇒ **按家目录使用**。

**⚠️ 221 的 Python 选择**：系统自带 **3.7.3**（TF 上限 2.11，太旧），但机器上另装了 **python.org 3.10.5**（`/Library/Frameworks/Python.framework/Versions/3.10/bin/python3.10`）⇒ **venv 用 3.10 建，TF 装 2.16.2** ✓。

**实测口径**（与 §2 完全相同，2026-10-28）：

| 机器 | 2 000 样本 × 1 epoch | **samples/s** | 24 h 可容纳（40 epoch）|
|---|---|---|---|
| **221（i7-9750H，12 线程）** | **128 s** | ★ **15.6** | ≈**33 700** |
| **106（Xeon E8C/8T，无 HT）** | **157 s** | ★ **12.7** | ≈**27 400** |
| 对照：NUC-1 211 / NUC-2 66 | — | 19 | ≈41 000 |
| 对照：开发 Mac（M4 Pro）| — | 89 | ≈192 000 |
| 对照：107（RTX 3060）| — | 122 | ≈263 000 |

**⇒ 两台广州机器都比香港 NUC 慢 20–33%**：106 是 **8 核 8 线程**（E-2286M 未开 HT，只有 8 个逻辑核），
221 虽然 12 线程但 **TF 2.16.2 + macOS 12** 的数据管道更旧。**⇒ 广州只适合跑"一周量"的排队任务，不适合快出结果。**

**装环境要点（★ 广州网络必须用镜像）**：

```bash
# 默认 PyPI 在广州不可用：572 MB 的 TF 轮子下了 40 分钟没动
# 清华镜像 24.7 MB/s ⇒ 两三分钟装完
./.venv/bin/pip install -i https://pypi.tuna.tsinghua.edu.cn/simple tensorflow        # 106
./.venv/bin/pip install -i https://pypi.tuna.tsinghua.edu.cn/simple tensorflow==2.16.2 # 221
```

| 机器 | venv | 备注 |
|---|---|---|
| 106 | `~/work/ai_phy_train/.venv`（py3.12）| TF **2.21.0**，与开发 Mac 完全一致 ✓ |
| 221 | `~/work/ai_phy_train/.venv`（py3.10）| TF **2.16.2**；系统自带 3.7 只能到 TF 2.11 ✗ ⇒ **必须用 python.org 的 3.10.5** |

**训练代码与语料就位方式**（两台相同）：

```bash
# 代码：从开发机打包传过去（ai_train 目录 ＋ AI-CE 共享模块，共 2.7 MB）
scp /tmp/ai_train_tree.tgz <host>:/tmp/ && ssh <host> 'cd ~/work/ai_phy_train && tar xzf /tmp/ai_train_tree.tgz'
# 语料：任取一份 npz
scp data/datasets/e17_test/depth3.npz <host>:work/ai_phy_train/data/speed_test.npz
```

**★ 磁盘陷阱（2026-10-28，221 实测）**：删掉 `~/anaconda3`(3.4 G) 与 `~/macOS_tool`(45 G) 后 `df` **毫无变化**，
原因不是没删，而是 **APFS 本地 Time Machine 快照**钉住了这些块（`tmutil listlocalsnapshots /` 显示三个，最近一个就生成在
删除之后）。**关掉自动备份后可用空间 26 G → 261 G**（快照共占约 **235 G**）。

| 现象 | 处置 |
|---|---|
| 删大文件后 `df` 不变 | `tmutil listlocalsnapshots /` ⇒ `sudo tmutil deletelocalsnapshots <date>` |
| 只想腾一部分 | `sudo tmutil thinlocalsnapshots / <字节> <保留个数>` |
| 长期 | 关掉本机自动备份，或接外置盘让 TM 有目标（本地快照会自动瘦身）|

**★ 已验证可训练**：两台都完整跑通 1 epoch（`runs/_speed/depth3_best.keras` ＋ `history.json` 产出正常，
`parameters 26185`、`heads ['llr','post_eq_noise']`），Keras 3 兼容 ✓。


**待实地评估时要补的字段**（每台机器一行）：

```text
主机名 / IP / 系统 / CPU 核线程 / 内存 GB / GPU 型号与显存 / 磁盘可用 / Python / TF 版本 /
按 §2 的命令实测 samples/s / 能否 24h 连续跑（散热与稳定性）
```

**注意**：macOS 12 那台的 TensorFlow 上限要单独确认（x86_64 上一般止于 2.16.2；Apple Silicon 上另说）；
带 N 卡的工作站若为 Linux，务必复查 §4 的 `LD_LIBRARY_PATH` 一条。

---

## 10. 跨地点排期策略

**两条规则**（用户 2026-10-23 定）：

1. **在本地的机器上，只跑 24 小时内能出结果的训练**；
2. **离开前提交"一周量"的训练任务**，下次回来取结果。

**24 小时能跑多少**（按 §2 的实测吞吐 × 40 epoch 折算）：

| 机器 | 24 h 可容纳的样本量（40 epoch） | 结论 |
|---|---|---|
| **107（122/s）** | **≈263 000** | 现有任何语料（≤44 k）都在 4–6 h 内 ⇒ **随便跑** |
| 开发 Mac（89/s） | ≈192 000 | 同上，≈5.5 h |
| NUC（19/s） | ≈41 000 | **旧语料（30 777）≈18 h ✓**；**E3/E4（40–44 k）≈25–28 h ✗ 超 24 h** |

⇒ 这正是当前分工的由来：**A 轨（旧语料）跑在 NUC 上，E 轨（大语料）跑在 107 上**。

**"一周量"怎么排**（168 h ≈ 11.5 M 样本 @ 19/s）：

- 一台 NUC 一周可串行跑 **3–4 条完整训练**（每条 25–28 h）⇒ 一次离开前把 **E3、E4、以及 1–2 条消融**排队；
- 队列用**顺序脚本**而不是并行（并行会让每条变慢 1.3–1.6 倍且互相拖累）：

```bash
# 离开前一次性提交，nohup 串行
cd ~/work/ai_phy_train
cat > /tmp/weekend_queue.sh <<'SH'
#!/usr/bin/env bash
set -e
./run_train.sh e3_week data/e3_train.npz data/e3_val.npz   > runs/e3_week.log 2>&1
./run_train.sh e4_week data/e4_train.npz data/e4_val.npz   > runs/e4_week.log 2>&1
SH
chmod +x /tmp/weekend_queue.sh
nohup /tmp/weekend_queue.sh > logs/week_queue.log 2>&1 &
```

- **成果取回**：`runs/<name>/depth3_best.keras` ＋ `train.log`，用 `rsync` 拉回**Apple Silicon 的 Mac**再导出 Core ML
  （x86 macOS 只能训练，导出／分桶／飞行都不行）
  （导出只能在 macOS，见 §6）。
- **一周任务的纪律**：① 每条都要写 `train.log`；② 队列脚本里**不要**用相对路径以外的假设（`cd` 到工作目录）；
  ③ 离开前确认 `nohup` 已脱离终端（`ps` 里父进程是 1）；④ 留一份 `disk` 余量检查（E 轨语料 5 GB/条产物）。

**地点切换时的三件事**：

1. 改本机 IP 段对应的机器清单（§9 表）；
2. 用 §2 的命令**重测一遍吞吐**（不同地点机器不同，旧数字不可假设）；
3. 把**未完成的任务**从离开地的机器上 `rsync` 到目的地的机器（训练不可迁移，但语料与 `.keras` 可以）。

---

---

## 11. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-23 | 建立本 memo；实测四台；确认 107 为最快（122/s）；记录 107 的 `LD_LIBRARY_PATH` 坑；评估 M2 与 2016 MBP 为评估机 |
| 2026-10-23 | 补 66 号机内存 32 GB；新增 §9 跨地点环境（广州 192.168.0.*、北京 192.168.100.*，初步）与 §10 跨地点排期策略（本地 ≤24 h、离开前提交一周量） |
| **2026-10-28** | ★ **全面核对过时内容**：广州两台从「待测」改为**已实测**；221 磁盘 **37 G → 255 G**（清理 APFS 快照后）、106 **429 → 416 G**（放下 e17 语料后）；§7 补六条真实踩坑（镜像、APFS 快照、`timeout`/`setsid`、`pkill` 自杀、`ARG_MAX`、变量不联动）|
| **2026-10-28** | ★ 补 **§3 内存去向实测**（`X` 7.30 GB ＋ `H` 3.19 GB ＝ **11.81 GB**，占 99%；模型只占 0.001 GB）与 **§8.6 训练→部署四步**（221 训练 → 回传 Apple Silicon → 导出 → 飞行）；§8 小节重排为 8.1–8.8 |
| **2026-10-28** | ★ **订正角色**：**导出 Core ML／分桶／飞行 只能在 Apple Silicon 的 macOS 上**；**66（香港）与 221（广州）都是 x86 macOS ⇒ 只能训练**；**M2 MacBook Pro（8 GB）可以**导出/分桶/飞行，但内存吃紧 |
| **2026-10-28** | ★ **文件改名**：`machine_compute_capability_memo.md` → **`machine_compute_capability_and_model_train_memo.md`**（范围从「算力」扩到「算力 ＋ 模型训练」）；★ **新增 §8 模型训练完整步骤**（命令、参数、日志字段、产物、断点续训、耗时、故障）；★ §1 加广州两台并标注香港机器**已不可达**；★ §2 吞吐表补广州两行；★ §4 补**镜像装包**与 macOS 缺 `timeout`/`setsid` 两条；★ 变更记录从 §8 改为 §11 并移到文末 |
| **2026-10-28** | ★ **广州实测（§9.1）**：**106** = Ubuntu 24.04.3 / Xeon E-2286M **8 核 8 线程（无 HT）** / **16 GB** / 416 G / py3.12 / **TF 2.21.0** / ★ **12.7 samples/s**；**221** = macOS **12.7.6** / i7-9750H **12 线程** / **32 GB** / ★ 37 G（清理快照后 **255 G**）/ py3.10 / **TF 2.16.2** / ★ **15.6 samples/s**。两台**均无独显**，都慢于香港 NUC 的 19/s。★ 工作目录实为 **`~/work/ai_phy_train`**（`/work` 不存在、`sudo` 需密码）；★ 广州**默认 PyPI 不可用，必须用清华镜像**（24.7 MB/s vs 卡死）；★ 两台均已建 venv ＋ 装 TF ＋ 传训练代码与语料，并**各跑通 1 epoch** |
