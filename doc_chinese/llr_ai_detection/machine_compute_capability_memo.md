# 机器算力 memo（香港本地局域网 192.168.31.*）

**目的**：把"哪台机器能跑什么、跑多快、怎么装"写在一处，省掉每次训练前重新试错。

**范围**：本文件只讲**算力与训练环境**。网络/手机接入见 `test_environment/香港测试环境与手机接入问题总结_2026-09-05.md`；
训练配方与判据见 `llr_ai_detection_design_and_implementation_1.md`（尤其 §19.298、§19.323–§19.325）。

---

## 1. 机器清单

| 机器 | 地址 | 系统 | CPU | 内存 | GPU | 角色 |
|---|---|---|---|---|---|---|
| **开发 Mac**（本机） | 192.168.31.121 | macOS 26.6.2 | **Apple M4 Pro，14 核** | **64 GB** | 集成 | 开发、导出 Core ML、飞行；**夜间可训练** |
| NUC-1 | **192.168.31.211** | Ubuntu | 16 线程 | 31 GB | 无 | 训练（长期）、**核心网 Open5GS** |
| NUC-2 | **192.168.31.66** | macOS 13.7.8 | Intel Xeon E-2286M，8 核/16 线程 | **32 GB** | 无 | 训练 |
| Asus notebook | **192.168.31.107** | Ubuntu | 16 线程 | **15 GB** | **RTX 3060 Laptop 6 GB**（compute 8.6） | **主力训练**（最快） |
| M2 MacBook | 192.168.31.203 | macOS 26.6.2 | Apple M2，4P+4E | **8 GB** | 集成 | **仅评估**（内存不足，见 §5） |
| 2016 MacBook Pro | （未接入） | macOS | Intel i7 4C/8T | 16 GB | — | **仅评估/截断训练**（太慢，见 §5） |

登录：三台训练机与 M2 均**免密 SSH**，用户名 `jwang`（211/107）与 `jiachengwang`（66/203）；工作目录 **`~/work/ai_phy_train`**。

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
| 2016 MBP（推算） | ~9–11 | ≈46–54 h |

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

**代码布局**（三台相同，且**不能压平**）：

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
| **M2 / 8 GB（.203）** | 语料 3.9–5.6 GB ＋ TF 2.5 GB 放不下；且机器**已在换页**（swap 6/7.2 GB）。只适合分箱 BER、LLR 诊断这类逐样本评估。 |
| **2016 MBP / i7 4C8T / 16 GB** | 内存够（16 GB ✓），但吞吐推算 9–11 samples/s ⇒ 一条 E 轨 46–54 h。只适合截断训练（`--max-train ≤8000`）。 |

---

## 6. 排期建议（谁跑什么）

| 场景 | 用哪台 |
|---|---|
| 出结论要快（E 轨、A/B 候选） | **107（122/s）**，其次开发 Mac |
| 多条消融并行 | 211 ＋ 66（各 19/s，各可 2 条 × 8 线程） |
| 夜间长跑 | 开发 Mac（过夜）＋ 107（连续） |
| 导出 Core ML / 分桶 / 飞行 | **只能在 macOS**（`coremltools` ＋ `xcrun coremlcompiler`）⇒ 开发 Mac 或 66 |
| 评估（分箱 BER、符号一致） | 任意；内存小的优先 211/107 |

**经验值**：一台机器并行 2 条训练时，每条用 `OMP_NUM_THREADS=8`（16 线程机器），耗时约为单条的 1.3–1.6 倍。

---

## 7. 操作踩坑（都真实发生过）

| 坑 | 现象 | 对策 |
|---|---|---|
| `python` 不带 `-u` | 日志只有 TF 启动几行，进程其实在跑（数百 % CPU） | **一律 `python -u`** |
| 一条 ssh 里用 `\` 续行拼多条 `nohup` | 第二条的重定向失败 ⇒ 训练根本没起来 | **一条 ssh 一条命令** |
| NUC-1 同时跑核心网 | 训练吃满 CPU 会让 Open5GS 抖动 | 飞行/接入测试时把 211 上的训练限到 1 条 |
| 107 GPU 静默回落 | 见 §4 | `LD_LIBRARY_PATH` |
| **驱动与用户态库版本不匹配** | `nvidia-smi` 报 `Driver/library version mismatch`，CUDA 失效，训练**照常跑但慢 4.4×**（实测 28 vs 122 samples/s，2026-10-23） | **重启机器**对齐版本；开训前先跑 `list_physical_devices("GPU")` 确认非空 |

---

## 9. 跨地点环境（初步，待实地评估后补充）

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
| **广州** | **2 台 NUC**：1 台 Ubuntu、1 台 **macOS 12** | — | 待测：吞吐、内存、Python/TF 版本、磁盘 |
| **北京** | **4 台 NUC**：**3 台 Ubuntu**、**1 台 macOS 13** | — | 待测同上 |
| **北京** | **1 台带独立 N 卡的工作站**：**16 CPU / 32 线程** | — | 用户描述为"前面评估过的那台机器（192.168.31.223）"；香港侧同类机型实测为 **RTX 3060 / ~122 samples/s**（§2），北京这台待实测确认卡型与内存 |

### 9.1 广州实测（2026-10-28，本机已移入 192.168.0.*）

| 机器 | 地址／用户 | 系统 | CPU | 内存 | GPU | 磁盘可用 | Python | TF | samples/s |
|---|---|---|---|---|---|---|---|---|---|
| **广州-NUC-1** | **192.168.0.106**／`jwang` | **Ubuntu 24.04.3**（x86_64） | **Xeon E-2286M，8 核 8 线程**（**无 HT**） | **16 GB**（15.3 GiB） | **无独显**（Intel UHD 630） | **429 G** | **3.12.3** | **2.21.0**（cp312） | ★ **12.7**（1 epoch × 2 000 样本 = 157 s） |
| **广州-Mac-NUC** | **192.168.0.221**／`jiachengwang` | **macOS 12.7.6**（x86_64） | **i7-9750H，6 核 12 线程** | **32 GB** | **无独显**（UHD 630） | ★ **37 G**（很紧） | **3.10.5**（系统另有 3.7.3） | **2.16.2**（x86 macOS 上限） | ★ **15.6**（128 s） |

**这两台的关键差异（会直接决定分工）**：

| 项 | 106（Ubuntu） | 221（macOS 12） |
|---|---|---|
| 内存 | **16 GB** ⇒ 与香港 NUC 同档，能跑 E4 级别语料但吃紧 | **32 GB** ⇒ 内存不愁 |
| 磁盘 | **429 G** ⇒ 可放整套语料到 2027 | ★ **37 G** ⇒ **只能放 1–2 套语料**，训练产物要勤清理 |
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

- **成果取回**：`runs/<name>/depth3_best.keras` ＋ `train.log`，用 `rsync` 拉回本机再导出 Core ML
  （导出只能在 macOS，见 §6）。
- **一周任务的纪律**：① 每条都要写 `train.log`；② 队列脚本里**不要**用相对路径以外的假设（`cd` 到工作目录）；
  ③ 离开前确认 `nohup` 已脱离终端（`ps` 里父进程是 1）；④ 留一份 `disk` 余量检查（E 轨语料 5 GB/条产物）。

**地点切换时的三件事**：

1. 改本机 IP 段对应的机器清单（§9 表）；
2. 用 §2 的命令**重测一遍吞吐**（不同地点机器不同，旧数字不可假设）；
3. 把**未完成的任务**从离开地的机器上 `rsync` 到目的地的机器（训练不可迁移，但语料与 `.keras` 可以）。

---

## 8. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-23 | 建立本 memo；实测四台；确认 107 为最快（122/s）；记录 107 的 `LD_LIBRARY_PATH` 坑；评估 M2 与 2016 MBP 为评估机 |
| 2026-10-23 | 补 66 号机内存 32 GB；新增 §9 跨地点环境（广州 192.168.0.*、北京 192.168.100.*，初步）与 §10 跨地点排期策略（本地 ≤24 h、离开前提交一周量） |
