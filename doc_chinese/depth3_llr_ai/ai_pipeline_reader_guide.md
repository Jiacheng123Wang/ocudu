# depth3 AI LLR 接收机 —— **从代码读出来的**导读

> 状态：导读（v1.0，2026-11-06）
> 定位：**解释 AI 设计的 PHY pipeline 的"是什么"与"在哪里"**，不是计划、不是设计提案。
>
> **方法**：**以代码为准**。文档只用来解释"为什么"，且**凡发现文档与代码不一致的，一律以代码为准并单列**（§11）。
> 每条结论后面带 `文件:行`。
>
> **⚠ 复核状态标记（本文通篇遵守）**
>
> | 标记 | 含义 |
> |---|---|
> | **【已复核】** | 本文作者**亲自打开文件核过** |
> | 【转述】 | 来自逐文件比对，**未二次复核**；引用前请自己核一遍 |
> | 【实测】 | 本仓库跑出来的数（标出处） |
> | 【未测】 | 只有注释/文档声明，**没有读数** |
>
> 关联：`depth3_llr_ai_high_level_status_and_plan.md`（预登记判据与五条工作线）、
> `depth3_llr_ai_design_and_implementation.md`（活文档）、`ane_latency_curve.md`（宽度-延迟曲线）、
> `depth3_gate_and_model_pk_npu.md`（npu 口径）、`phy_pipeline_defaults_inventory.md`（默认值清单，**注意 §11-1 有一条已过期**）。

---

## 0. 一句话

> **depth3 = 把"信道估计 + 均衡 + 解映射"这三段合成**一个**可整体替换的单元**，
> 换成一次**在 ANE 上**的神经前向：输入是 `[1, 612, 14, 16]` fp16 的**空间平面**
> （612 子载波 × 14 符号 × 12 契约通道 + 4 个 Qm one-hot），输出是**标定过的 LLR + 上报头**，
> LLR 交给**现成的** LDPC。周围（DFT、解复用、解扰、速率匹配、LDPC、CRC）**不动**。
>
> 它的成本模型**不是 FLOPs**：dense 3×3 多 **5.7× FLOPs 而延迟不变**（【实测】）；
> 真正的杠杆是**只算分配那点带宽**——八个窄带桶把 c128 从 **2 824 µs 打到 266 µs**（【实测】）。

---

## 1. 它替代了什么，以及为什么**必须**整体替换

`lib/phy/upper/channel_processors/pusch/pusch_depth3_receiver.h:7-12`（**理由原文**）：

> The AI receiver replaces channel estimation + equalization + demapping **as ONE unit, never one module at a
> time**. A per-module backend knob would **silently be a depth-2 experiment**（一个学习型解映射器放在经典信道估计
> 后面）——那是**另一个、更容易**的实验，却会被当成这个实验报出去。

**被替换掉的计算量**（`llr_ai_detection/README.md`）：`ce` **73.3 µs** + `eqdem` **667.4 µs**；
设备侧融合单元 `merged_hop` 中位 **449.8 µs**。【实测】

---

## 2. 在 pipeline 里的位置：两个接缝、**同步**、在关键路径上

### 2.1 两个入口，而且它们**不相邻**

| 入口 | 输入 → 输出 | 调用方 |
|---|---|---|
| `estimate()` | grid + DM-RS 配置 → 信道估计及其统计量 | `process_estimate`（`pusch_processor_impl.cpp:477`）|
| `demodulate()` | grid + 那些估计 → 软比特 | `process_data`（`:904-912`）|

**为什么必须是同一个对象的两个方法**（`pusch_depth3_receiver.h:14-23`）：估计值经 **DM-RS 估计器 notifier 回调**
回到处理器（这正是"延迟完成的后端"能工作的机制），所以接缝把估计值**当普通参数跨两个方法传**，与经典路径一致；
把估计值藏起来的抽象要重接那条回调，**比替换本身改动更大**。

**还有一条只有读代码才知道的原因**（`pusch_depth3_input_plane.h:34-43`）：**DM-RS 参考序列要重建**，
而 `pusch_demodulator::configuration` **不含** slot / `n_id` / `n_scid`（经典解调器不需要——它读插值结果）。
这三个量**只在估计器的配置里**，经 `estimate()` 到达 ⇒ **这才是两个入口必须同属一个对象的原因**。

### 2.2 前向是**同步**的，且**在 LDPC 之前**

```
record_demod_enter → get_depth3().demodulate(...) → record_demod_return
   ⇒ 然后才 sync_device_estimates() + CSI 合并 ⇒ 然后才是 LDPC
```
`predict_depth3` **阻塞在 worker 的条件变量上**（`ocudu_coreml_nn_engine.mm:869-884`）【已复核】⇒
**没有异步提交、不与 LDPC 重叠**。

**只有"测量那一半"能提前**：`prepare()` 由符号循环在 PDU 的**最后一个 DM-RS 符号**写入时调用
（`uplink_processor_impl.cpp:65-75,178-233`），`demodulate()` 复用缓存值；
**该钩子默认关闭**（`OCUDU_DEPTH3_PREPARE_ON_SYMBOL`）【已复核：默认值在 `:67-74` 的读法】。
**前向本身不可能提前**——它需要数据 RE。

---

## 3. 输入契约：`[1, 612, 14, 16]` fp16

### 3.1 张量的四个轴（**部署形态，从资产里读出来**）【已复核】

`ai_assets/depth3_e4_real_1023/d3_e4.mlmodelc/metadata.json` → `"name":"x","shape":"[1, 612, 14, 16]","dataType":"Float16"`
（`d3_modcond_c32h`、`d3_modcond_r7`、`d3_e1_2heads`、`d3_e17`、`d3_w2cond`、`d3_modcond_ft` 相同）

| 轴 | 长度 | 含义 |
|---|---|---|
| 0 | 1 | batch（ANE 要静态形状）|
| 1 | **612** | 子载波，**51 PRB 窗口内的绝对索引** |
| 2 | **14** | OFDM 符号 —— ★ **是空间轴，不是折进通道** |
| 3 | **16** | **12 契约通道 + 4 个 Qm one-hot** |

**★ 符号轴是空间轴**这一点是契约级的（`pusch_depth3_input_plane.h:7-12`）：所以网络的 **3×3 核同时混时间与频率**。
折叠布局（`[612, 1, 64]`，仍在树里但不再是契约）在同等训练下**未编码 BER 0.3480 vs 空间 0.0254**（13×）【转述】，
这就是契约被改的原因。

### 3.2 12 个契约通道（`depth3.py:84-92`、`pusch_depth3_input_plane.h:71-84`）【已复核：C++ 枚举】

| # | 通道 | 粒度 |
|---|---|---|
| 0–1 | `CH_GRID_RE` / `CH_GRID_IM`：接收网格实/虚部 | 每 (t,f) |
| 2 | `CH_DMRS_MASK` | 每 (t,f) |
| 3 | `CH_ALLOCATION`：分配掩码 | 每 f，**沿 t 广播** |
| 4 | `CH_DC`：直流标记 | 每 f，沿 t 广播 |
| 5–6 | `CH_LS_RE` / `CH_LS_IM`：**去偏置后的导频最小二乘**，只在该导频符号上非零 | 每 (t,f) |
| 7–11 | `CH_NOF_TX_LAYERS`、`CH_NOF_CDM_GROUPS`、`CH_DMRS_TYPE`、`CH_NOF_DMRS_SYMBOLS`、`CH_SCALING` | 标量，广播 |

**三条要点：**

1. **输入不依赖"信道是怎么估的"**（`:9-12` 原文）：*"nothing comes from the estimator's RESULTS, so the arm's
   input does not depend on how the channel was estimated."* 导频 LS 在**导频 RE 上现场重算**（Gold 序列重建）。
2. **归一化**：`r` = 导频 LS 的 **RMS**（`depth3.py:243-247`），**不是码率**；场通道都除以它。
   LLR 与噪声头**不需要还原**，因为两者在公共缩放是不变量（`depth3.py:256-259`）【转述】。
3. **去偏置**：`CH_SCALING`（2 CDM 组时 β≈1.4125）是把 LS 变成**数据信道**估计的那一步；**少了它导频会高 3 dB**（`depth3.py:51-53`）【转述】。

**+4 个 Qm one-hot**：全平面 4 个常数，顺序 `COND_QM_ORDER = (2,4,6,8)`；
**由数据集/加载器追加，不是 `build()` 追加**（`train_depth3.py:395-425`）【已复核：`build()` 里的注释明说 `in_channels` 已是数据的通道数】

### 3.3 拒收是**返回值**，且必须计数

`pusch_depth3_input_plane.h:13-17` 原文：*"A refusal is a **RETURN VALUE** rather than an exception or an assertion:
a reception outside the contract's envelope must **degrade to the classical chain and be counted (I7)**, never be run
through a network that was not trained for it."*

`status` 枚举：`wrong_plane_size` / `unsupported_configuration` / `unsupported_bandwidth` / `allocation_outside_window` /
`allocation_too_narrow` / `no_rx_port` / `unsupported_layers` / `no_dmrs_symbol` / `bad_reference_amplitude`。【已复核：枚举名】

### 3.4 ★ 一段值得记住的工程史：`MIN_PRB` **4 → 2 → 1**

`pusch_depth3_input_plane.h:87-104`：

| 地板 | 后果 |
|---|---|
| **4** | **每个 attach 开头的 3-PRB Msg3 被拒收**；而 PURE 腿下拒收 = **擦除接收**（不是交接）⇒ **手机 attach 不上** |
| **2** | 仍拒收调度器能发的 **1-PRB** 分配 ⇒ 实测**五个单元里丢三个，其中三个经典链解得出来** |
| **1（现行）** | 默认**算所有分配**；想恢复地板设 `OCUDU_DEPTH3_MIN_PRB` |

**⇒ 教训（原文意）**：**地板必须由"网络更差在哪"来论证**；包络外只是"没测过"，**不是"已知不行"**。
【已复核：`MIN_PRB = 1`，`pusch_depth3_input_plane.h:104`】

### 3.5 契约有**双胞胎**

C++ 的 `assemble()` 与训练侧的 `depth3.pack_input_spatial`，由 `pusch_depth3_input_plane_check` **逐点比对**。【已复核：该测试文件存在】

---

## 4. 网络

### 4.1 主干（`train_depth3.py:114-302` 的 `build()`；★ **`depth3.py` 不是模型定义，它是可执行的 I/O 契约**）

```
输入 [1, 612, 14, 16]
 └ entry_1x1: Conv2D(c_hid, 1×1) + ReLU                 （仅当 in_channels != c_hid）
 └ ×18 残差块（d = DILATIONS[i % 4] = 1,2,4,8）
     DepthwiseConv2D(3×3, dilation=(d,1))   ← ★ 空洞只在【频率轴】
     → ReLU → FoldedScaleConv(c_hid, 1×1) → ReLU → Add(skip)
 └ 四个头（每 RE 的 1×1 卷积，再 Reshape 回契约的 [width, channels]）
```

- **感受野 = 127 子载波 × 3 符号**：`1 + 2·(1+2+4+8)·4 + 2·(1+2) = 127`；**3×3 永远只跨 ±1 符号**，只有频率可达范围在长。
- **`DILATIONS = (1, 2, 4, 8)`**【已复核：`train_depth3.py:57`】——注意 `ai_train/README.md:18` 写的是 `1,2,3,4`（**错**，见 §11-5）。
- **必须残差**（`build()` docstring）：*"a plain `y = block(y)` stack **collapses geometrically with depth**
  （0.044→0.005→0.0003 over the first four blocks），every gradient past the collapse is **exactly zero**"*；
  本任务上实测 loss 1.277、平均 LLR 1.1、**未编码 BER 0.498（= 瞎猜）**。
  **LayerScale 残差能救；BatchNorm 也能，但在 ANE 上贵得多。**【转述：数来自 docstring/实测记录】

### 4.2 ★ `FoldedScaleConv`：**免费的 LayerScale**

输出通道带一个正的逐通道增益 `γ = softplus(g)`；因为 `γ·relu(Wx+b) = relu((γW)x + γb)`，
**折进核里 ⇒ 零额外算子**。而**单独的乘法在 ANE 上跨 18 个块要 ~200 µs**（`train_depth3.py:66-68`）【转述】。
**这条与"成本是搬运不是乘加"是同一经验的两个面。**

### 4.3 ★ 三种块，以及那个**让所有 "dense" 白训**的缺陷

`build(..., dense=False, dense_bottleneck=0)` 的三路选择：

| 标志 | 块 | 每块权重（C=32）|
|---|---|---|
| （无）| `DepthwiseConv2D(3×3)` → ReLU → FoldedScale | 320 + 1 088 = **1 408** |
| `--dense-blocks` | `Conv2D(C, 3×3)` → ReLU → FoldedScale | 9 248 + 1 088 = **10 336** |
| `--dense-bottleneck N` | `1×1 → C/N` → `3×3 空洞` → `1×1 → C`，`narrow = max(8, C//N)` | N=2: **3 424**；N=4: **1 168** |

**语义**：depthwise = 逐通道 3×3（通道内混频率）**再** 1×1（混通道）；dense = 一个 3×3 **同时**混通道与频率；
bottleneck dense = 折中——挤到 `C/N` 做联合混合，让块自身的 1×1 再展开，权重 `C·C/N` 而不是 `9C²`。
N=4 时**比 depthwise 还便宜**（depthwise 已带 `9C`，而挤压只需 `C²/N`）【转述】。

**缺陷（`84f0df2032`，2026-11-06）**：`build()` 有**两个主干构建器**（空间分支 `:151-219`、折叠分支 `:221-302`），
**空间分支把 3×3 硬编码成 depthwise**、从不读 `dense`/`dense_bottleneck`。而**每个部署 bundle 都用 `--layout spatial`** ⇒
`--dense-blocks` / `--dense-bottleneck` **静默失效**。
**它是被参数量抓到的**：名叫 dense 的 run 打印 **26 185**，与 depthwise 一模一样，而那个宽度的 dense 是 **186 889**。
**连带作废**："dense +5.9%"、"256QAM +19.5%"、"dense 3×3 ANE 延迟一样" 三个结论。
**同一提交还记了第二个陷阱**：`--init` **不拒绝形状不匹配的权重** ⇒ 用 depthwise 检查点暖启动 dense 图，dense 层仍是随机值。【已复核：commit message 原文】

### 4.4 四个头（`outputSchema`）【转述：形状来自 metadata，构造成本处已复核】

| 头 | 形状 | 语义 | 激活 |
|---|---|---|---|
| `llr` | `[1, 612, 112]` | **加扰域 LLR，正 = bit 0**，通道 `c = t*8 + b` | **tanh** |
| `post_eq_noise` | `[1, 612, 14]` | 每 RE 均衡后**方差**，`log10` 形状 | tanh |
| `ch_est` | `[1, 612, 28]` | `Re/Im(Ĥ/r)`，`c = 2t+{0,1}` | **linear** |
| `dmrs_noise` | `[1, 1, 1, 1]` | DM-RS 域方差标量，`log10` 形状 | tanh |

**★ 空间分支的主头是"每 (子载波,符号) 8 通道"，再 Reshape 成 112**（`train_depth3.py:194-199`），
不是直接出 112 通道——这有**真实的参数量后果**（264 vs 3 696 权重），**没有文档说明**。【已复核：`:194-199`】

112 通道在 256QAM 以下有 6 个通道被浪费，**是刻意的**（DM-RS 符号位置逐小区不同，{2,11} 与 {2,7,11} 都在语料里）【转述】。

**"上报头" = `ch_est` + `dmrs_noise`**（可选，`--no-reporting-heads`）；`post_eq_noise` **不可选**。

### 4.5 参数量（**可独立复算**，闭合公式）

```
entry          in_ch*C + C
每块（depthwise） (9C + C) + (C*C + 2C) = C² + 12C
头（空间）       llr C*8+8、noise C*1+1、ch_est C*2+2、dmrs C*1+1
```

| bundle | 配置 | 计算值 | 声明值 |
|---|---|---|---|
| E4 / E17 / W2（2 头）| c32, 18 块, in=16 | 544 + 18·1408 + 264 + 33 = **26 185** | 26 185 |
| c32h / r7（4 头）| 同上 + ch_est 66 + dmrs 33 | **26 284** | 26 284 |
| ft | c64, 18 块, 4 头 | **89 420** | 89 420 |
| dense | c32, 18 块, 2 头 | **186 889** | 186 889 |
| bottleneck-2 / -4 | narrow 16 / 8 | **62 473 / 21 865** | 同 |
| low-rank-8 | c32, rank=8 | **16 969** | 16 969 |

**⇒ 26 185 与"参数量 2.6 万"这个说法吻合**；而它比文献里能工作的接收机小 **5–47 倍**（NRX 1.4–4.4×10⁵、
DeepRx 1.23×10⁶、HELENA 1.1×10⁵、DEFINED 4.2×10⁵）——这被登记为第一代的**根本缺陷之一**。

---

## 5. ★ LLR 尺度链：**tanh 是唯一的饱和点，而且是构造出来的**

```cpp
// train_depth3.py:194-198
llr = Conv2D(head_qm, 1)(y);                                // head_qm = 8，每 RE 8 通道
llr = Activation("linear" if llr_linear else "tanh")(llr);   // ← 默认 tanh
outputs = {"llr": Reshape((width, NOF_SYMBOLS * head_qm))}   // → [612, 112]

// pusch_depth3_receiver_impl.cpp:108 / 1858 / 1921-1922
scales.s_llr = range_limit;                                  // 缺 sidecar 时的默认
const unsigned range_limit = (QPSK) ? 24 : 20;
const float scaled = logit * scales.s_llr;
codeword[...] = log_likelihood_ratio::quantize(scaled, range_limit);
```
【以上全部【已复核】】

**设计意图**（`:76` 注释）：**`S_llr = range_limit` 让 tanh 成为链上唯一的饱和** ⇒ `|L| ≤ 20`（其他调制）
或 `< 24`（QPSK）⇒ `quantize` **永不钳位**，饱和完全由网络的 tanh 决定。

**已发布 sidecar 实测**：`s_llr=20, a_noise=4, b_noise=-1`（`ai_assets/depth3_e4_real_1023/depth3_scales.txt`）【已复核】。

**而 `s_llr` 不是拍的**：`calibrate_depth3.py` 把验证捕获**解两遍**（量化/非量化）扫一个尺度区间，
取**量化 BLER 的平台**；训练值落在平台上就保留。`a_noise/b_noise` **不是拟合**——它们是训练器的
`NOISE_SCALE_DECADES=4.0 / CENTER=-1.0`，反过来写成 `σ² = 10^(4u−1)`【转述】。

**SINR 校准是另一张表**：逐调制、dB 域仿射，**只作用于"发布的 SINR"**，**不碰 LLR 通路**（实测 550 次接收逐位相同）【转述】。

---

## 6. 窄带桶：**为什么可以裁、裁多少、怎么选**

**`w` = 图的静态子载波宽度**。`base_w{108,180,252,324,396,468,540,612}` = **`108 + 72k`** ⇒
**9/15/21/27/33/39/45/51 PRB**，**72-sc / 6-PRB 间距**。

### 6.1 运行时选择（`pusch_depth3_receiver_impl.cpp:959-990`）【转述】

```
ctx        = 4 PRB × 12 = 48 子载波
first      = max(first_prb*12 − ctx, 0)
last       = min((last_prb+1)*12 + ctx, 612)
needed_nsc = last − first
bucket     = *std::lower_bound(narrow_buckets, needed_nsc)     // 最小的够用的那个
```

### 6.2 为什么可以裁：**全卷积 ⇒ 逐位等价**

主干是全卷积 ⇒ **一组权重可在任意宽度导出**；裁到「分配 + 每侧 **4 PRB** 上下文」**逐位复现全带输出**——
实测 **100.00% 符号一致、零相对差**。4 PRB 也是量出来的：**≥48 子载波边距逐位一致，2 PRB 差 0.04%，0 差 1.5%**。【转述】

**空中实测收益**：`forward` p50 **820 → 457 µs（−44%）**，吞吐与 CRC 不变【转述】；
更宽的对照：c128 从 **2 824 µs（612 全带）→ 266 µs（108 桶）= 10.6×**（`ane_latency_curve.md`）【实测】。

### 6.3 两条不变式

1. **盖不住分配的桶拒收并计数**，绝不静默裁（`depth3_degradation::plane_refused`）。
2. **头输出落回绝对子载波位置**，下游索引不变。

### 6.4 ★ 一个静默陷阱

臂**按名字**探测 `buckets/base_w*.mlmodelc`；**目录名写错不报错**——它会**静默跑全带、只是慢**。
判据是日志那行 `narrow-band buckets bound: 108 180 …`。**`OCUDU_DEPTH3_BUCKETS` 的取值**：
不设 = 探测兄弟 `buckets/`；`none|full|0` = 只全带；`=a,b,…` = 只要这些。【转述】

---

## 7. 运行时：放置、延迟、关键路径

### 7.1 放置：**`cpu_ne` 是被强制的**（★ 与 `phy_pipeline_defaults_inventory.md` §3 相反）

`pusch_depth3_receiver_impl.cpp:377-402`【已复核】：

```cpp
if (requested == "cpu_gpu" || requested == "cpu") {
  fmt::print(stderr, "… REFUSING … (measured 1677 us per reception and a 15.2 percent decode rate
                      on 2026-10-23); using 'cpu_ne'.\n");
  setenv("OCUDU_DEPTH3_COMPUTE_UNITS", "cpu_ne", 1);      // 强制改回
} else {
  setenv("OCUDU_DEPTH3_COMPUTE_UNITS", "cpu_ne", 1);      // 默认就是 cpu_ne
}
```
**理由是一段实测事故**（同处注释）：引擎默认 `all` 把放置交给 Core ML 且**什么都不回报**；
2026-11-05 那次飞行发现它**落在 GPU** 上——预测 **2997 µs** 对快态 ~400 µs，**晚十几个 slot**，上行丢约 **39%** 的块。

**但 Core ML 不报告运行时放置**（`ocudu_coreml_nn_engine.mm:541-544` 明说）⇒
"跑在 ANE 上"是**按算子**测的（`MLComputePlan`：`d3_e4` 上 **101/101 在 ANE**【转述】），**不是按每次推理**。

### 7.2 延迟：★★ **三个互不可比的口径**

| 口径 | 数字 | 出处 |
|---|---|---|
| **模型单独（暖）** E4 `forward` | p50 **210** / p99 **267** / max **2923 µs** | `depth3_gate_and_model_pk.md` 【转述】 |
| **按桶宽** c32 | 612: **387** / 252: **206** / 180: **172** / **108: 137 µs** | `ane_latency_curve.md` 【实测】 |
| **按桶宽** c128 | 612: **2824** / 180: **432** / **108: 266 µs** | 同上【实测】 |
| **空中整臂** `[ul_equalization_demod]`（c32+桶）| **346 µs**，p99 **481** | 同上【实测】 |
| **空中整臂**（c128+桶）| **625 µs**，p95 2207，p99 **4427**，max **6280** | 同上【实测】 |
| **整跳** `[ul_pipeline]`（c32+桶）| **900 µs**（DFT 489 = 54%，臂 346 = 38%）| 同上【实测】 |

**⇒ 三条必须分开引用：模型 ≠ 臂 ≠ 跳。** 空中整臂比模型高约 **200 µs**（平面组装 + 量化 + 解扰）⇒
**不能把模型数说成"接收机耗时"**。

**⇒ "能不能赶上 0.5 ms"取决于口径**：门控 ④ 是 **p99 ≤ 400 µs**，而空中 c32+桶的 p99 是 **481 µs（✗）**；
文档另一处又把 900 µs 说成"≈1.8 slot ⇒ 稳在预算内"。**两句不能同时成立**——见 §11-10。

### 7.3 成本模型：**不是 FLOPs**

同一 c128 架构，depthwise 3×3 → **dense 3×3（5.7× FLOPs）**：**2 824 → 2 831 µs**。【实测】
⇒ **多算 5.7 倍，延迟一点没变。ANE 上真正的成本是"每个算子的数据搬运/调度"，不是乘加。**

**成本账**（`ane_latency_curve.md` §6a）【转述】：输入平面 0.27 MB（~3 µs）、输出 LLR 0.14 MB（~1.4 µs）**都可忽略**；
真正的量是**中间激活每次全部重算**——101 个算子 × 1.7 MB（c128）≈ **170 MB/次** ⇒ 2.8 ms ≈ 60 GB/s。
权重只在构造期加载一次 + 预热 + 每 2 s keep-alive（"粮草先行"）。

---

## 8. **怎么证明 AI 真的跑了**（这一点很容易被糊弄）

按强度排序：

| # | 证据 | 强度 |
|---|---|---|
| 1 | **`[depth3-timing] … N forward(s)`** —— **最强**。N 是完成的真实前向次数（需 `OCUDU_DEPTH3_TIMING=1`），还带按宽度的分配混合与每宽 mean | ★★★ |
| 2 | 启动期**逐模型绑定行** + 覆盖横幅（`AI ARM ACTIVE over N model(s) … the classical demodulator does not run for those receptions`）| ★★ |
| 3 | **反向证据**：任何 `DEGRADED to the classical chain` / `FAILED (no fallback)` 都**证明这一接收不是 AI 结果** | ★★ |
| 4 | **时延指纹**：`[ul_equalization_demod]` ≈ 346 µs（AI）vs ~20 µs（经典解调）——**15× 差别** | ★★ |
| 5 | **API 真相**：`kind()`、`computes_itself()`（= 已绑定 **且** 上次前向成功）、`is_identity()` | ★★★（可编程判）|

**★ 两个陷阱：**
- **`[depth3-measure]` 那行对已发布的 `cond-norep` bundle 什么都不打印**——它没有两个上报头就 early-return。
  **所以不能靠它证明。**【转述】
- **`[depth3-gate]`/`[depth3-cfo]`/`[depth3-sinr]` 不是证据**——被域门 / `OCUDU_DEPTH3_SINR` 门控。

**仪器语义**（`pusch_depth3_measurements.h:30-46` 原文【已复核】）：

> **R6 必须知道自己的测量什么时候可信，而一把取自网络 head 4 的尺子，是网络在审判自己。**

所以另配一把**完全不依赖网络、也不依赖经典估计器**的估计：从**平面自己的导频 LS** 减去沿频率三点均值、对残差缩放。
实测（对数域）：**与数据集真实噪声相关 +0.999，p10/p90 散布 0.23 dB**；**head 4 只有 +0.593、±7 dB**。【转述】

---

## 9. 降级与拒收：**策略**与**物理**被分开，策略已默认关

统一入口 `degrade_or_fail()`：**fallback 开 ⇒ 交给经典链；fallback 关（PURE）⇒ 擦除（写零 ⇒ 必 KO）并计数**。

| 类别 | 归因 | 默认会发生吗 |
|---|---|---|
| **策略**（启动宽限、域门、上报量超域）| 我们选的 | **不会**（`domain_gate=none`、`grace=0`）|
| **物理**（模型不覆盖该 Qm、没先调 `estimate()`、平面组装失败、前向失败、无数据 RE）| 真的算不了 | 罕见 |

⇒ **正常接收一律计算**；剩下的都是"真的算不了"。

**而回退的日志是 1/1000 采样的**（第 1 次与每 1000 次）⇒ **一个 fallback 开着、0% 接收被计算的 run，
第一条之后看起来几乎是干净的**。⇒ **判据必须看 `[depth3-timing]` 的 N，不看"没有 DEGRADED 行"。**【转述】

---

## 10. 四门现状（**预登记判据**，不是事后口径）

四门定义在 `depth3_llr_ai_design_and_implementation.md` §194-201 / 高层 §56-68：

| 门 | 通过线 | **当前** |
|---|---|---|
| **G-① 解码** | AI ≥ 经典 − 0.5 pp | **FAIL**：50k 首次传输腿 **39 221 vs 40 810** = **−3.18 pp**（交付比特 **−2.85%**）；而门要求的 **W2 HARQ 混合集还没建** |
| **G-② attach** | AI ≥ 经典 − 2 pp | **未测**（npu 口径下没有 attach 表）|
| **G-③ 分箱** | 无格低于 −5 pp、最弱格 ≠ 0% | **FAIL**：256QAM **−20.3% … −48.9%**；16QAM/64QAM 恰好 0；QPSK **+3 472 … +5 144 bit** |
| **G-④ 时延** | p99 ≤ 400 µs、max ≤ 2 ms | **FAIL**（E4 暖 p99 267 ✓ 但 max 2923 ✗；c32+桶空中 p99 481 ✗；c128+桶 p99 4427 ✗）|

**⇒ 一句话**：**低 SNR 更强（QPSK +2.5%～+230%，16/64QAM **逐比特相同**），差距集中在高 SNR 的 256QAM
（−6.5% 起，分箱到 −20…−49%）；时延在窄带桶下**中位够、尾部不够**。**

---

## 11. ★ 文档 ↔ 代码出入（**引用前必读**）

**复核状态**：✅ = 本文作者已亲自核过；⏳ = 来自逐文件比对，未二次复核。

| # | 文档说 | 代码 | 判定 | 复核 |
|---|---|---|---|---|
| 1 | `phy_pipeline_defaults_inventory.md` §3：`OCUDU_DEPTH3_COMPUTE_UNITS` 默认 **`all`**，非 `all` 会被强制改回 | **相反**：默认强制 `cpu_ne`，`cpu`/`cpu_gpu` 被**拒绝** | **文档过期（2026-11-05 后反转）** | ✅ |
| 2 | 设计文档 §D4.1 约束 3：头**必须线性无界**（或线性＋显式钳位）；tanh 在自信比特处梯度消失，一代已实测改成 clamp | `train_depth3.py:195` 默认 **tanh**；`--llr-linear` 存在但**没有已发布产物用它** | **文档与代码相反**，且代码注释自己在 `:272-276` 承认 tanh 的问题 | ✅ |
| 3 | `ch_est` 输出要 `clamp(v, ±C_H)`，`C_H = 8` | 只乘 `r`；**`C_H` 在两个相关文件里零命中** | **文档无实现** | ✅ |
| 4 | DC 与非数据 RE 要写 `σ² = +inf` 标记 | 只**防御性消费** `isfinite`；DC **会被访问并拿到模型的真实值** | **文档无实现** | ⏳ |
| 5 | `ai_train/README.md:18`：空洞 **1,2,3,4** | **`(1,2,4,8)`**；且只有 1,2,4,8 才得出它自己写的 RF=127 | **文档错** | ✅ |
| 6 | `ai_train/README.md:189`：桶宽是 "(2,4,6,8,…)×3 PRB，即 **36-sc 间距**" | 实际 `108+72k`，**72-sc / 6-PRB 间距** | **文档错** | ✅ |
| 7 | `ai_train/README.md:221`：`MIN_PRB` "今天还是 2" | **1** | **文档过期** | ✅ |
| 8 | `post_eq_noise` "是把 logits 缩放成 LLR 的那个量"（`train_depth3.py:284-287`）| 用 `scales.s_llr`（sidecar 常数）；同文件 `:1903-1906` 明说 LLR 通路看不到它 | **树里两条注释互斥**，接收机是对的 | ⏳ |
| 9 | `depth3_e1_2heads/README.md`："去掉两个上报头，26 185 → **16 969**" | 去掉两头只减 **99**；**16 969 是 `--low-rank 8` 变体**（18×(1088−576)=9 216）。且 E1 的 `weight.bin` 与 E4 **字节数相同** | **README 数字错（或 E1 标错）** | ⏳ |
| 10 | `ane_latency_curve.md` §4 说 >1 slot "需要 ≥2 slot 预算"，§6 又说 900 µs "≈1.8 slot 稳在预算内"，而门控 ④ 是 p99 ≤ 400 µs | —— | **读者无法判断哪个截止期是约束**；唯一硬的看起来是 HARQ RTT 8 ms | ⏳ |
| 11 | `test_shipped_bundle_contract.py`：`c32h` 是 **DEFAULT** | 默认是 `d3_e4.mlmodelc`（`CMakeLists.txt:41-43`）| **测试滞后** | ⏳ |

**⇒ 其中 #2 最要紧**：设计文档明确说 tanh 会让梯度在自信比特处消失、且一代已因此改成 clamp，
而**所有已发布 bundle 都是 tanh**。**这可能是 256QAM 高 SNR 那 −20…−49% 的一个候选解释**
（自信比特多的地方恰是 256QAM 高 SNR），而 `--llr-linear` **已经存在、只要一个字改**。

---

## 12. 两条待办（本文提出，可直接开工）

| # | 项 | 严重度 | 修法 |
|---|---|---|---|
| **A** | **keep-alive 缓冲按 12 通道分配、按 16 通道绑定** ⇒ **68 544 B 越界读 / 每 2 s**，影响已发布的 `cond` 族。`DEPTH3_IN_CHANNELS=12`（`:35`）vs `model3_in_channels=16`（`:643`）；`:226` 用它构形状，`:703-704` 却用 12 分配 | 🟠（**读代码得出，未跑 ASAN**）| `:703-704` 改用 `e->model3_in_channels`（`resize` 在 `:703`，晚于 `:643` 的赋值，顺序成立）|
| **B** | **LLR 头是 tanh**，而设计文档要求线性+钳位（§11-2）| 🟠（可能直接影响 256QAM 高 SNR）| 用已有的 `--llr-linear` 训一臂，按 **G-③ 分箱**判 |

> **A 的复核记录（【已复核】）**：`grep DEPTH3_IN_CHANNELS` → `:35` 定义、`:643` 加 4、`:703-704` **只用 12 分配**；
> `:369` 的 keep-alive 作业设 `in3.plane = e->keepalive_in3.data()` 且**不改通道数**。
> ⇒ 形状 16 的 MLMultiArray 会从 12 通道的 vector 里读 `612×14×4` 个 half-word = **68 544 B** 之后的内存。

---

## 13. 诚实清单

| 项 | 现状 |
|---|---|
| **本文的方法** | 以代码为准；§11 的 11 条里 **6 条我亲自复核**（✅），**5 条待核**（⏳）。**⏳ 的条目引用前请自己核。** |
| **A 是"读代码"而非"实测"** | 未跑 ASAN/guard page。要变成实测，跑一次 `-fsanitize=address` 的 `depth3_engine_check`。 |
| **延迟数字的口径** | §7.2 三张表**互不可比**（模型/臂/跳、冷/暖、全带/窄桶）。引用时必须带口径。 |
| **`s_llr` 平台** | "取量化 BLER 的平台"是**方法**描述，我没有重跑拟合。 |
| **"ANE 上 101/101"** | 来自 `MLComputePlan` 的**按算子**放置，**不是**按推理的运行时放置（Core ML 不报）。 |
| **`npu` 模式** | **代码已实现**（枚举、解析、模式发布、CPU LDPC 默认），但**验收项仍开着**（遗留别名未删、`npu+classic ≡ cpu-dump` 未验、脚本透传未做）。 |
| **本文不评价性能优劣** | 那是四门与 `depth3_network_improvement_plan.md` 的事；本文只解释**结构与位置**。 |
