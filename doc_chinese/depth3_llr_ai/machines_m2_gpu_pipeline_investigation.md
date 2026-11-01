# M2 MacBook（203）GPU 路径调查备忘录

**本文档记录一个**独立的**问题：M2 MacBook 上的 `--phy_pipeline gpu` 路径导致解码失败。
**它于 2026-11-01 首次发现，与 depth3 的门控表无关，因此单独成文**（门控表见 `depth3_gate_and_model_pk.md`）。

**结论先行**：★ **CoreML、模型、ANE 在 203 上实测正常** ✓；★ **缺陷在 M2 的 GPU 管线路径上** ✓；
**★ 但注意**：depth3 网络的输入是 **IFFT/DFT 之后的 T-F 数据** ⇒ **CE/EQ/DEMOD 与 depth3 无关** ✓
⇒ **下一步应重点检查** IFFT/DFT 模块**（默认走 CPU 的 DSP 硬件加速 ✓）。

---

## 6. ★★★★ 2026-11-01 12:20 **M2 MacBook（203）上的门控环境搭建与首个问题**

**(a) ★ 目的**

**⇒ 门控测量必须走** `ul_chain_replay` 的 **GPU 路径 ＋ AI 后端** ✓（建库时用的是 CPU 路径 ✗）
**⇒ 而 GPU 路径只在 **Apple Silicon 的 macOS** 上可用 ✓ ⇒ 目标是 **M2 MacBook Pro（192.168.31.203）** ✓

**(b) ★ 已完成**

| 步 | 结果 |
|---|---|
| ★ **代码同步** | ★ **GitHub pull 成功**（`ecffd04d9f` → **`a20e3df4b4`**）✓，本地 0 改动 ✓ |
| ★ **构建** | ★ **两个目标都建成**：`ul_chain_replay_cpu`（3.1 MB，CPU 版）＋ ★ **`ul_chain_replay`（3.2 MB，Metal 版）** ✓ |
| ★ **GPU 路径** | ★ **可用**（`--metal` ＋ `--phy_pipeline gpu` 都能跑 ✓）|
| ★ **AI 后端** | ★ **能绑定模型**（`OCUDU_REPLAY_MODEL=<*.mlmodelc>` ✓，日志确认 "depth-3 receiver model: …" ✓）|
| 测试语料 | 从 T7 的 `s2_mcs00_ping_1031a` 取 **40 个单元**送到 203 ✓ |

**(c) ★ 机器**：M2 / macOS 26.6.2 / **8 GB 内存** ⚠ / 221 Gi 可用 ✓

**(d) ★★ 发现的问题：AI 臂能绑模型，但解码全 KO**

**⇒ 5 个单元对比**：

| 单元 | CPU（经典）| ★ GPU ＋ AI |
|---|---|---|
| `cap_10139_18477_17971` | crc=**OK** | ★ crc=**KO** |
| `cap_10215_19997_17971` | OK | **KO** |
| `cap_1025_27_17921` | OK | **KO** |
| `cap_1035_227_17921` | OK | **KO** |
| `cap_10381_2837_17971` | OK | **KO** |

**⇒ 换过三种模型都一样**：`d3_modcond_r7` ✓、`d3_e4.mlmodelc` ✓、`buckets/base_w612.mlmodelc` ✓ ⇒ **全部 KO** ✗

**(e) ★★ 已经定位的两条线索**

**① `sinr=inf` 是**预期的**** ✓ —— AI 臂的日志说：
> "the bound bundle carries neither reporting head; RSRP, EPRE and the SINR will be published from the shared
> measurement layer instead of from a head"
**⇒ 所以 SINR 印 inf 不代表模型坏** ✓（它不发布 SINR ✓）

**② ★ 所有 bundle 都缺 `depth3_scales.txt`** ✗ —— AI 臂反复警告：
> "there is no sidecar at all, so the noise map falls back to the receiver's defaults (**A_n=4 B_n=-1**).
>  Regenerate it with `calibrate_depth3.py --write-scales`"
**⇒ LLR 的噪声映射退回了默认值** ✓ ⇒ **这很可能就是解码失败的原因** ✓

**(f) ★ 下一步（未做）**

```bash
# 1) 生成标定 sidecar（需要 val.npz）
calibrate_depth3.py --model <run>/depth3_best.keras --data <val.npz> --write-scales <bundle目录>
# 2) 重跑同一个单元
OCUDU_REPLAY_MODEL=<bundle>/<name>.mlmodelc \
  ul_chain_replay <unit> --out /tmp/x --metal --phy_pipeline gpu --pusch_receiver_backend ai
```

**⇒ 若生成 sidecar 后仍 KO**，则问题在**输入布局 / 模型与 C++ 侧的契约** ✓ ⇒ 下一步用 gNB 的门控脚本
（`probe/check_ai_arm_binding.sh`，走 `--expert_phy.pusch_receiver_model`）做对照 ✓

★ 状态 ✓：★ 203 的 GPU 门控环境已搭好并跑通 ✓；★ AI 臂能绑模型 ✓；★ 但解码全 KO 已定量记录 ✓；★ 两条线索与下一步已列 ✓。

### 6.1 ★★★★★ 2026-11-01 12:35 **对照实验：同一二进制在本机 OK、在 M2 上 KO —— 是环境问题，不是代码问题**

**(a) ★ 本机（M4 Pro）跑同样的命令**

```bash
OCUDU_REPLAY_MODEL=…/depth3_modcond_r7/d3_modcond_r7.mlmodelc \
  ul_chain_replay <同一单元> --out /tmp/mac_ai --metal --phy_pipeline gpu --pusch_receiver_backend ai
```

| 机器 | 结果 |
|---|---|
| ★ **本机 M4 Pro** | ★ **`crc=OK iterations=1 sinr=21.56 dB`** ✓✓ |
| ★ **203 M2** | ✗ **`crc=KO iterations=6 sinr=inf dB`** ✗ |

**⇒ 同一份代码、同一个模型、同一个捕获单元** ✓ ⇒ **结果完全不同** ✗

**(b) ★★ 逐步排除（都排除了）**

| 怀疑项 | 检查 | 结论 |
|---|---|---|
| ★ **模型文件** | 两边 `mlmodelc` 的**文件列表与总大小完全相同**（164K，5 个文件）✓ | ✗ 不是它 |
| ★ **编译选项** | 两边 `CMakeCache` 的 `ENABLE_METAL_*` **完全一致**（全 ON）✓ | ✗ 不是它 |
| ★ **compute unit** | 203 上 `all` / `cpu` / `gpu` / `ane` **四种全 KO** ✗ | ✗ 不是 ANE 选择 |
| ★★ **二进制** | ★ **把本机跑通的二进制复制到 203** ⇒ ★ **仍然 KO** ✗ | ✗ 不是构建差异 |
| **macOS 版本** | 两边都是 **26.6.2** ✓ | ✗ 不是版本 |

**(c) ★★ 结论：问题在 203 的**运行时**，而不是代码**

**⇒ 最关键的证据**：★ **连本机编译、本机验证可用的二进制，拿到 203 上也 KO** ✓
**⇒ 所以只能是** 203 上 **CoreML 推理本身**在返回异常值** ✓（`sinr=inf` 是模型输出被判定为 Inf 的直接迹象 ✓）

**(d) ★ 剩下的两个怀疑方向**

| # | 怀疑 | 怎么验 |
|---|---|---|
| ★ 1 | **CoreML 在 M2 上加载/预测该模型失败**（8 GB 内存压力？ANE 首次编译？）| ★ 写一个**最小 CoreML 预测脚本**在 203 上直接跑该 `.mlmodelc`，看输出是否含 Inf |
| ★ 2 | **该 `.mlmodelc` 是在本机编译的产物**，其内部的执行计划可能在 M2 上不被接受 | ★ 在 203 上用 `xcrun coremlcompiler` **重新编译**同一 `.mlpackage`，再测 |

**⇒ 建议先做第 1 步**（最小预测脚本，10 行）——它能立刻区分"模型加载失败"与"模型加载成功但输出坏" ✓

★ 状态 ✓：★ 对照实验完成 ✓；★ 五个怀疑项已排除 ✓；★ 结论锁定为 203 的 CoreML 运行时 ✓；★ 两个下一步已列 ✓。

### 6.2 ★★★★★ 2026-11-01 12:45 **定位：CoreML 在两台机器上**完全一致**；问题在 M2 的 `--phy_pipeline gpu`**

**(a) ★★ 最小 CoreML 探针（Swift，10 行核心逻辑）**

**⇒ 同一个 `.mlmodelc`、同一个全 1 输入，两台机器的输出**逐值相同** ✓：

| 输出 | M4 Pro | ★ M2 (203) |
|---|---|---|
| `ch_est` | min −2.3418 ／ max +4.5156 | ★ **完全相同** ✓ |
| `llr` | min −0.8008 ／ max +0.8545 | ★ **完全相同** ✓ |
| `post_eq_noise` | −0.4937 ／ +0.2455 | ★ 相同 ✓ |
| NaN / Inf 个数 | **0 / 0** | ★ **0 / 0** ✓ |

**⇒ 结论**：★ **CoreML 与模型在 203 上完全正常** ✓ ⇒ **不是 CoreML、不是模型、不是 ANE** ✓

**(b) ★★★ 隔离实验：换 `--phy_pipeline` 一个参数，结果从 KO 变 OK**

| `--phy_pipeline` | 结果 |
|---|---|
| `cpu` | ★ **`crc=OK iterations=1 sinr=21.91 dB`** ✓ |
| `cpu_gpu` | ★ **`crc=OK`** ✓（源码里它与 `cpu` 等价：两者都把 CE/demod 设为 false ✓）|
| ★ **`gpu`** | ✗ **`crc=KO iterations=6 sinr=inf`** ✗ |

**⇒ 也就是说**：★ **同一条命令、同一个模型，只把"CE/demod 放 GPU"打开，解码就从 OK 变 KO** ✓

**(c) ★ 已排除的模块**

| 模块 | 怎么排除 | 结论 |
|---|---|---|
| ★ **CoreML / 模型** | 最小探针逐值相同 ✓ | ✗ 不是它 |
| ★ **Metal 信道估计（CE）** | `OCUDU_CE_LS_CHECK=1` ⇒ ★ **`max_rel=2.428e-07 bad(>1e-5)=0`** ✓ | ✗ 不是它 |
| ★ **LDPC 解码器** | `--metal-cpu-ldpc` 与 `--pusch_ldpc_decoder_type generic` **都仍 KO** ✗ | ✗ 不是它 |
| ★ **编译/二进制** | 把本机跑通的二进制拷到 203 ⇒ **仍 KO** ✗ | ✗ 不是它 |
| ★ **编译选项** | 两边 `CMakeCache` 的 `ENABLE_METAL_*` 全同 ✓ | ✗ 不是它 |

**(d) ★★ 结论**

> ★ **缺陷在 203 的 `--phy_pipeline gpu`（CE/demod 上 GPU）这条路径上** ✓
> **而 `cpu` / `cpu_gpu` 模式完全正常** ✓ ⇒ ★ **AI 臂本身没有问题** ✓

**(e) ★ 现状与建议**

| 项 | 结论 |
|---|---|
| ★ **203 能否做门控** | ✗ **暂不能**（需要的正是 `gpu` 模式 ✓）|
| ★ **本机能否做门控** | ★ ✅ **能**（`gpu` 模式正常 ✓）|
| **建议** | ★ **门控仍在本机跑** ✓；203 作为**并行/备份**，待 `gpu` 路径修好后再上线 ✓ |
| **若必须用 203** | 可先用 `--phy_pipeline cpu` ✓（AI 臂照常工作 ✓），但那**不是 GPU 路径** ✗ |

★ 状态 ✓：★ CoreML 在 M2 上实测正常 ✓；★ 缺陷定位到 `gpu` 模式的 CE/demod ✓；★ 五个模块已排除 ✓；★ 建议明确 ✓。

---

## 1. ★★★★ 2026-11-01 13:00 **用户的关键提示：depth3 的输入是 IFFT/DFT 之后的 T-F 数据**

**⇒ 这一条把排查范围缩小了一半** ✓：

> ★ **depth3 网络从 IFFT 之后的 T-F 数据就进入网络了** ✓
> **⇒ 因此 **CE / EQ / DEMOD 等模块与 depth3 的输入无关** ✓
> **⇒ 它们即使有问题，也**不会**让 depth3 的 LLR 变坏** ✓

**⇒ 所以我先前"CE/demod 上 GPU 导致 KO"的因果链**不成立** ✗ —— **那只是**相关**，不是因果** ✗
**⇒ 真正需要检查的是 **IFFT / DFT 模块**（以及它产出的网格是否正确）✓

---

## 2. ★★ 已确证的事实（可复现）

| # | 事实 | 证据 |
|---|---|---|
| ★ 1 | **CoreML、模型、ANE 在 203 上完全正常** | 最小 Swift 探针，两台机器输出**逐值相同**，`nan=0 inf=0` ✓ |
| ★ 2 | **AI 臂在 203 上能绑模型** | 日志：`depth-3 receiver model: … (OCUDU_REPLAY_MODEL)` ✓ |
| ★ 3 | **`--phy_pipeline cpu` 在 203 上解码 OK** | `crc=OK iterations=1 sinr=21.91` ✓ |
| ★ 4 | ★ **`--phy_pipeline gpu` 在 203 上解码 KO** | `crc=KO iterations=6 sinr=inf` ✗ |
| ★ 5 | **本机（M4）同样的 `gpu` 命令解码 OK** | `crc=OK iterations=1 sinr=21.56` ✓ |
| ★ 6 | **本机编译的二进制拷到 203 也 KO** | ✗ 排除"203 的构建有问题" |
| ★ 7 | **两边编译选项完全相同** | `ENABLE_METAL_*` 全 ON ✓ |
| ★ 8 | **Metal CE 自检通过** | `OCUDU_CE_LS_CHECK=1` ⇒ `max_rel=2.4e-07 bad(>1e-5)=0` ✓ |
| ★ 9 | **换解码器（CPU/generic）仍 KO** | ✗ 排除 LDPC |
| ★ 10 | ★ **CPU DFT 的网格在 203 与本机**逐字节相同** | 两次 `--dft --synth 2` 的 `.bin` 用 `cmp` 比对 ⇒ 相同 ✓ |
| ★ 11 | **CPU DFT 与 Metal DFT 的网格差异很大**（本机 `max rel=0.88`）| ⇒ **两者不是同一实现**（可能是缩放/布局约定不同 ✓），**不能直接按字节比较** ✗ |

**⇒ 第 10、11 两条合起来的含义**：
★ **CPU DFT 在 M2 上是对的** ✓（与本机逐字节相同 ✓）
**⇒ 但 `--phy_pipeline gpu` 时 DFT 走的是哪条实现，尚未确认** ✗

---

## 3. ★ 下一步：从 IFFT/DFT 模块开始**逐模块**打开 GPU

**⇒ `--phy_pipeline cpu_gpu` 的意义**：★ **默认什么都不设置时它与 `cpu` 等价** ✓ ⇒ **可以**逐个模块**往 GPU 上挪** ✓

**⇒ 待办的测试矩阵**：

| # | 打开哪个模块 | 怎么开关 | 期望 |
|---|---|---|---|
| ★ 1 | **仅 DFT/IFFT 上 GPU** | `--dft-metal`（合成基准）＋ 捕获回放 | 先确认 DFT 本身在 M2 上算得对 |
| ★ 2 | **仅 CE 上 GPU** | `cpu_gpu` ＋ 单独打开 CE | 与 §2-8 的自检一致 ✓ |
| ★ 3 | **仅 equalizer 上 GPU** | 同上 | — |
| ★ 4 | **仅 demapper 上 GPU** | 同上 | — |
| ★ 5 | **全部上 GPU** | `--phy_pipeline gpu` | ★ 已知 KO ✗ |

**⇒ 关键是第 1 步**：★ **先确认 M2 上 DFT 产出的网格**与 CPU 路径是否等价** ✓
**⇒ 判定方法**：**不能按字节比**（§2-11 ✓）⇒ 应比较**同一捕获在 `--dft` 与 `--dft-metal` 下**AI 臂输出的 LLR**
或**直接比较解码结果** ✓

---

## 4. ★ 一个待确认的环境问题

**⇒ 用户提到**："目前的 default 是用 CPU 的 DSP 硬件加速"** ✓
**⇒ 需要确认**：★ **M2 芯片是否支持那条 DSP 加速路径** ✓（若支持，默认路径应当是安全的 ✓）
**⇒ 检查方式**：★★ 看 `create_dft_processor_factory()` 里的实现选择逻辑，以及 Mac 上该工厂是否选了 vDSP/Accelerate ✓

★ 状态 ✓：★ 用户的关键提示已入档 ✓；★ 11 条已确证事实已列 ✓；★ 逐模块测试矩阵已列 ✓；★ DSP 加速的开放问题已记录 ✓。

