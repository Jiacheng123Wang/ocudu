

---

## 附录 C（2026-11-13）：北京外置 GPU 盒子（192.168.100.153）—— 实测算力

**(a) 硬件与链路**

| 项 | 值 |
|---|---|
| 卡 | **NVIDIA RTX 3060 Ti，8 192 MiB**（转接板：雷电 3 → PCIe x16，XYJ-LINK）|
| 宿主 | Ubuntu 24.04.3，内核 6.8.0-147（CoffeeLake-H 笔记本）|
| 雷电 | TB3，**40 Gb/s**，已授权 ✓ |
| ★ PCIe | ★ **负载下 Gen3 x4**（空闲时降到 Gen1 x4，是 ASPM 省电，不是故障）|
| 驱动 / CUDA | 595.99.02 / CUDA 13.2 |
| 满载 | 1 980 MHz、约 90–200 W、57 °C ✓ |

**(b) 算力实测（同一份 `bench3.py`：c32/b18/dense，`tf.function`）**

| 模式 | batch | 153（3060 Ti eGPU）| 107（3060 Laptop 6 GB）|
|---|---|---|---|
| tf.function | 16 | ★ **36.6 ms/步 ⇒ 438 样本/s** | 端到端 **118 ms/步 ⇒ 136 样本/s**（训练日志实测）|
| tf.function | 32 | 61.6 ms/步 ⇒ 520 样本/s | — |
| tf.function | 64 | 114.4 ms/步 ⇒ 560 样本/s | — |
| ★ **jit_compile** | 16 | ★ **18.0 ms/步 ⇒ 888 样本/s** | —（未测）|
| matmul fp16 | 2048³ | **15.1 TFLOP/s** | — |

**⇒ ★★★ 结论**
1. ★ **纯 GPU 步进：153 比 107 快约 3.2 倍**（36.6 vs 118 ms，注意后者是端到端、含数据加载）；
2. ★ **开 XLA（`--jit`）还能再快一倍**（888 样本/s），代价是数值略有差异、需要重新验证；
3. 3060 Ti 的 FP16 峰值（~16.2 TFLOP/s）实测到 **15.1**，说明**卡跑在满速**，没有因雷电而降算力；
4. 按 438 样本/s 估算，w2v3b（185 227 样本 × 40 轮）在 153 上约 **5.5 h**（107 上是 17 h）。

**(c) 环境（153）**

```
~/work/ai_phy_train/.venv          TF 2.21.0 + tensorflow[and-cuda]（CUDA 12.9 wheels）
~/work/ai_phy_train/lib/phy/upper/...   depth3/ai_train ＋ CE 共享模块（按仓库相对路径摆放）
~/work/ai_phy_train/env.sh          ★ 必须 source：把 nvidia wheels 的 lib 加进 LD_LIBRARY_PATH
```
★ **不 source env.sh 时 TF 会静默跑 CPU**（`GPUs: []`，只有 4 样本/s）——这是本次踩到的坑 ✓


---

## 附录 D（2026-11-13）：**外置 GPU 盒子（eGPU）完整备忘 —— 规格、装机、环境、算力、迁移清单**

> 附录 C 是速记；**这一段是拿去广州/香港能直接照着做的版本**。

### D.1 硬件清单

| 部件 | 规格 | 实测/说明 |
|---|---|---|
| **雷电转 PCIe 转接板** | XYJ-LINK（雷电 3 设备名），桥片 **Intel JHL6340 Alpine Ridge 2C**；1× PCIe x16 槽、1× 雷电口 | `boltctl list` 显示 **TB3，40 Gb/s，已授权** |
| **显卡** | **NVIDIA RTX 3060 Ti（GA104，8 192 MiB，TDP 200 W）**，**双 8-pin** 供电（截图确认为 MSI 卡，subvendor `1462:3909`）| `lspci` 见 `10de:2486`；满载 1 980 MHz、57 °C |
| **电源** | **500 W**，**只**给显卡＋转接板（宿主自己供电）| 总需求 ≈ **275 W**（卡 200 W ＋ 插槽/板 75 W）⇒ 余量充足 |
| **数据线** | **雷电 3 认证线（40 Gb/s）**，建议 ≤0.8 m | 长线会降到 20 Gb/s 甚至不识别 |

### D.2 ★ 供电接法（最容易接错的地方）

转接板上有 **两个** 供电输入，**只接一路**：

| 输入 | 用途 | 接法 |
|---|---|---|
| ★ **CPU 4+4Pin（8-pin EPS）** | PCIe 插槽 12 V 主供电（≤75 W）| ★ **接这一路**；没有 8-pin 线时用 **大 4D（Molex）**，二选一 |
| **PD 供电（板标 `PD 12V`）** | 无 ATX 电源时的替代（只够低功耗卡）| ★ **空着不接**；两路**不要同时接**（会反灌）|
| **显卡双 8-pin** | 显卡自身主供电 | ★ **两个都接**，最好走电源的**两根独立 PCIe 线** |

**菊花链的判定**：单根 8+8 菊花链整根额定 **150 W**。
- 3060 Ti（200 W）＋插槽 75 W ⇒ 线缆负载 ≈ **125 W** ⇒ ★ **可接受，不必换线**；
- 3070 Ti / 3080（290–320 W）⇒ 线缆 ≈ 215–245 W ⇒ ★ **必须加第二根线，或把卡限到 ~220 W**。

**Ubuntu 上按卡限功耗**（3060 Ti 建议 200 W 即可，不必降）：

```bash
sudo nvidia-smi -pm 1          # 持久模式
sudo nvidia-smi -pl 200        # 3070Ti 建议 220，3080 建议 250
```

### D.3 软件规格

| 层 | 版本 | 备注 |
|---|---|---|
| 宿主 OS | **Ubuntu 24.04.3 LTS**，内核 **6.8.0-147** | 152/153/106 都是 Ubuntu |
| GPU 驱动 | **nvidia-driver-595-open**（`595.99.02`，CUDA 13.2）| `ubuntu-drivers devices` 的推荐项 |
| Python | 3.12.3（系统）＋ `~/work/ai_phy_train/.venv` | venv 内独立 |
| 训练框架 | **TensorFlow 2.21.0** ＋ `tensorflow[and-cuda]`（CUDA 12.9 wheels、cuDNN 9.28）| 驱动向后兼容 CUDA 12 运行时 |
| 代码布局 | `~/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/` ＋ `.../signal_processors/channel_estimator/metal/ai_train/` | ★ 必须是这个**相对路径**，`depth3_paths.py` 按它找 CE 共享模块 |

### D.4 ★ 环境准备（新宿主从零开始）

```bash
# 1) 驱动（需要 sudo；装完必须重启）
sudo apt update
sudo apt install -y nvidia-driver-595-open
sudo reboot

# 2) 验证硬件链路
nvidia-smi                                  # 应显示 RTX 3060 Ti / 8192 MiB / 200 W
boltctl list                                # 应显示 TB3 设备且 authorized（未授权则：sudo boltctl authorize <uuid>）
lspci -nn | grep -i nvidia                  # 应见 10de:2486

# 3) 训练环境
mkdir -p ~/work/ai_phy_train && cd ~/work/ai_phy_train
python3 -m venv .venv
.venv/bin/pip install -U pip
.venv/bin/pip install "tensorflow[and-cuda]" numpy     # 约 3 GB 下载，10–20 min

# 4) 代码（从 Mac/107 同步，保持仓库相对路径）
#    只需两棵子树：depth3/ai_train 与 channel_estimator/metal/ai_train
rsync -a <源>:~/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train/ \
      ~/work/ai_phy_train/lib/phy/upper/ai_signal_processors/depth3/ai_train/
rsync -a <源>:~/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/ \
      ~/work/ai_phy_train/lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/

# 5) ★ 让 TF 找到 CUDA（不 source 就会静默跑 CPU！）
cat > ~/work/ai_phy_train/env.sh <<'EOF'
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
EOF
source ~/work/ai_phy_train/env.sh

# 6) 冒烟
.venv/bin/python -c "import tensorflow as tf; print(tf.__version__, tf.config.list_physical_devices('GPU'))"
#   期望：2.21.0 [PhysicalDevice(name='/physical_device:GPU:0', device_type='GPU')]
```

★ **本次踩到的坑**：不 source `env.sh` 时，TF 打印 `GPUs: []` 并**静默走 CPU**（实测只有 **3.6–4 样本/s**，而 GPU 上是 **438**）。看日志若出现 `Skipping registering GPU devices...`，就是这个原因。

### D.5 算力实测（可复现）

**测量脚本**（本机 `~/work/ai_phy_train/bench2.py`、`bench3.py`）：
- `bench2.py`：纯 matmul（2048³/4096³ fp16）＋ **预先造好 batch** 的 depth3 步进；
- `bench3.py`：**`@tf.function`** 与 **`jit_compile=True`** 两种编译模式的 depth3 步进（与训练器 `train_depth3.py` 同一形状）。

**结果（RTX 3060 Ti，Ubuntu 24.04，TF 2.21）**

| 测量 | 结果 |
|---|---|
| matmul fp16 2048³ | **15.1 TFLOP/s**（3060 Ti 峰值 ≈ 16.2 ⇒ ★ 满速）|
| matmul fp16 4096³ | 11.5 TFLOP/s |
| depth3 c32/b18/dense，batch 16，`tf.function` | **36.6 ms/步 ⇒ 438 样本/s** |
| 同上 batch 32 / 64 | 61.6 / 114.4 ms ⇒ **520 / 560 样本/s** |
| ★ 同上 batch 16，**`jit_compile=True`** | ★ **18.0 ms/步 ⇒ 888 样本/s**（batch 32/64 约 964）|
| 满载状态 | 1 980 MHz、57 °C、PCIe **Gen3 x4**（空闲降 Gen1 x4 是省电）|

**与 107（ASUS，RTX 3060 Laptop 6 GB）对比**

| | 153（3060 Ti eGPU）| 107（3060 Laptop）|
|---|---|---|
| 步进（batch 16）| ★ **36.6 ms（纯 GPU）** | **118 ms（端到端，含数据加载）** |
| 样本/s | ★ **438** | **136** |
| 换算 w2v3b（185 227×40 轮）| ★ **约 5.5 h** | **17 h** |
| 开 `--jit` | 约 **2.5–3 h** | 未测 |

### D.6 ★ 迁移清单（带到广州/香港，接 211 / 106 / 107）

**要带**：转接板 ＋ 显卡 ＋ 500 W 电源 ＋ **TB3 认证线** ＋ 8-pin/大4D 线 ＋（可选）机箱风扇。

**到新宿主后按这个顺序做**：

| 步 | 动作 | 判据 |
|---|---|---|
| 1 | 接电：CPU 4+4 → 板；卡双 8-pin → 电源；PD 口空 | 板上电源灯亮 |
| 2 | TB3 线接宿主 | `boltctl list` 见设备（未授权则 `sudo boltctl authorize <uuid>`）|
| 3 | `nvidia-smi` | 显示卡型号、显存、功耗上限 |
| 4 | 装驱动（若宿主没有）| `nvidia-smi` 可用 |
| 5 | 建 venv ＋ 装 TF（D.4 第 3 步）| `pip show tensorflow` 有版本 |
| 6 | 同步两棵代码子树 ＋ `env.sh` | `source env.sh` 后 `GPU: ['/physical_device:GPU:0']` |
| 7 | 跑 `bench3.py` | batch 16 应 ≈ **36 ms/步**（±20%）|

**宿主差异提醒**

| 宿主 | 注意 |
|---|---|
| 211 / 106（Ubuntu，无 GPU）| 直接按 D.4；106 是 8C/16T，**CPU 数据管线**可能成为瓶颈，训练时把 `--workers` 调到 6–8 |
| 107（Ubuntu，自带 3060 Laptop）| 接上 eGPU 后 `CUDA_VISIBLE_DEVICES=0` 指哪张卡要**先确认**（`nvidia-smi -L` 的顺序可能变）|
| Mac | ★ **macOS 不支持 NVIDIA eGPU**，盒子**不能**接 Mac 训练（只能用 AMD 卡）|

### D.7 故障对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| `nvidia-smi` 不存在 | 驱动没装 | `sudo apt install nvidia-driver-595-open` ＋ 重启 |
| `boltctl` 看不到设备 | 线/授权 | 换 TB3 认证线；`sudo boltctl authorize <uuid>` |
| `GPUs: []`，样本/s 只有 4 | ★ **没 source env.sh** | `source ~/work/ai_phy_train/env.sh` |
| PCIe 显示 Gen1 x4 | 空闲省电（ASPM）| 负载下再看，应是 **Gen3 x4** |
| 训练中掉卡/重启 | 供电（菊花链过载）| 限功耗 `-pl 220` 或加第二根 PCIe 线 |
| 温度 > 85 °C 降频 | 盒子散热不足 | 加风扇/开盖；`nvidia-smi dmon -s pucvmet` 观察 |
