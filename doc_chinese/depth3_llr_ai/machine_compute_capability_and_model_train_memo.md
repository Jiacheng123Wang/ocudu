

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
