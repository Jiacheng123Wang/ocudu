# 机器算力 memo（香港本地局域网 192.168.31.*）

**目的**：把"哪台机器能跑什么、跑多快、怎么装"写在一处，省掉每次训练前重新试错。

**范围**：本文件只讲**算力与训练环境**。网络/手机接入见 `test_environment/香港测试环境与手机接入问题总结_2026-09-05.md`；
训练配方与判据见 `llr_ai_detection_design_and_implementation.md`（尤其 §19.298、§19.323–§19.325）。

---

## 1. 机器清单

| 机器 | 地址 | 系统 | CPU | 内存 | GPU | 角色 |
|---|---|---|---|---|---|---|
| **开发 Mac**（本机） | 192.168.31.121 | macOS 26.6.2 | **Apple M4 Pro，14 核** | **64 GB** | 集成 | 开发、导出 Core ML、飞行；**夜间可训练** |
| NUC-1 | **192.168.31.211** | Ubuntu | 16 线程 | 31 GB | 无 | 训练（长期）、**核心网 Open5GS** |
| NUC-2 | **192.168.31.66** | macOS 13.7.8 | Intel Xeon E-2286M，8 核/16 线程 | — | 无 | 训练 |
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

---

## 8. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-23 | 建立本 memo；实测四台；确认 107 为最快（122/s）；记录 107 的 `LD_LIBRARY_PATH` 坑；评估 M2 与 2016 MBP 为评估机 |
