# LLR AI detection — 仓库接缝与接口勘察备忘（memo 01）

> 本工作流的模型：**输入时频网格（+信道估计/噪声方差），输出 LLR**。
> 这份备忘不讨论算法选型，只回答三个必须先钉死的问题：
> **模型接到哪里**（§1、§4）、**输出必须满足什么契约**（§2、§3）、**标签从哪来**（§5）。
> 所有结论都给出 `文件:行号`，便于复核；未复核的部分明确标注。
>
> 版本：v1.2 ｜ 状态：勘察完成（代码事实已逐条核对）｜ 日期：2026-10-07
> v1.1：新增 §1.4–§1.6——**接缝有两种深度**，深度 3（CE+均衡+解映射）才是标准 neural receiver 形态，
> 也是本工作流的目标；v1.0 只描述了深度"均衡+解映射"的子集。

---

## 1. 接缝：`channel_equalizer::submit_fused()` 就是 AI detector 的宿主

新工作流最省事、也最正确的接缝**已经存在**，而且已经在被 Metal 融合通路使用。

接口原文（`include/ocudu/phy/upper/equalization/channel_equalizer.h:196-221`）：

```cpp
virtual void submit_fused(span<log_likelihood_ratio>       llrs,
                          span<cf_t>                       eq_symbols,
                          span<float>                      eq_noise_vars,
                          const re_buffer_reader<cbf16_t>& ch_symbols,
                          const ch_est_list&               ch_estimates,
                          span<const float>                noise_var_estimates,
                          float                            tx_scaling,
                          modulation_scheme                mod);
```

配对谓词 `supports_fused_demapping(mod, nof_ports, nof_layers)`（同文件 `:188-194`，默认返回 `false`）。

调用点在 `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:601`（group 融合路径），非融合路径是 `:618`（`submit`）/`:624`（`equalize`）。

### 1.1 为什么这是理想宿主

这个接缝的语义**恰好**等于"AI detector"要做的事：用一次前向传播替换 `equalize + demodulate_soft` 两步。论文里的 detector 也是同构的——输入接收网格 + 信道信息，输出符号/比特判决。

契约里那条最苛刻的约束，对 AI 模型反而是天然成立的。原文（`:174-176`）：

> The two are an ALL-OR-NOTHING pair. A caller that took the fused route must not demap that symbol
> again … so the predicate is the contract, and it must be a property of the backend and of the shape,
> **never of the values**.

一个神经网络的前向就是 shape+config 的函数，不可能"看数值决定走不走融合路"——这正是契约要的性质。

### 1.2 输入形态与零拷贝潜力

| 参数 | 类型 | 说明 |
|---|---|---|
| `ch_symbols` | `re_buffer_reader<cbf16_t>` | 每端口接收 RE |
| `ch_estimates` | `ch_est_list` | 每 (rx_port, layer) 一条；含 `device_slice`（`:55-67`） |
| `noise_var_estimates` | `span<const float>` | 每接收端口噪声方差 |

`ch_est_list::device_slice`（`:48-67`）的存在意味着**估计值可以全程设备驻留**：GPU 后端直接绑定信道估计器产出它的那块缓冲，不经过主机暂存。

★ ANE 同样是统一内存架构，`MLMultiArray` 可以用 `initWithDataPointer` 包住同一块内存（AI CE 的 `ocudu_coreml_nn_engine.mm` 已经这么做了，是零拷贝的）。所以**零拷贝性质原则上可以保持**。但由此产生本工作流第一个真正的工程风险：**GPU→ANE 的时序**（谁先完成、谁等谁）必须在 `phy_pipeline_grid_ready` / `phy_pipeline_crossings` 契约里显式声明，否则就会打破当前融合 lane"一次提交"的结构。这条写进规划的 §风险。

### 1.3 一条不能省的输出

`submit_fused` 的注释明确：融合后端**不写** `eq_symbols`，但**必须写** `eq_noise_vars`——因为后均衡 SINR 是接收链的**上报统计**，不是融合路由可以悄悄停掉的东西（`:204-208`）。AI 路径同样要给得出后均衡噪声方差（可以由经典式 σ²·‖w‖² 算，或由模型附带一路输出；规划里必须选一个）。

---

### 1.4 ★★ 接缝不止一个：两种"深度"（v1.1 新增，回应"要不要包含信道估计"）

★ **本 memo v1.0 只描述了较浅的那个接缝。** 如果目标是"**信道估计 + 均衡 + 解映射**"融合成一个 net
（输入 = DFT 后的时频网格 + 几何，输出 = LLR），那**正是文献里 "neural receiver" 的标准形态**，
它比 v1.0 说的 `submit_fused` **更深**：

| 深度 | 替换什么 | 输入 | 输出 | 我们仓库里的宿主 |
|---|---|---|---|---|
| **深度 1** | 仅 CE | 网格 + DM-RS 几何 | 信道估计 + 噪声 | `dmrs_pusch_estimator::estimate()` |
| **深度 2** | CE + 均衡 | 同上 | 均衡符号 + 后均衡噪声 | — |
| **深度 3** ★ | **CE + 均衡 + 解映射** | **网格 + 几何** | **解扰前软比特（LLR）** | `pusch_processor_impl.cpp:264`（`estimator.estimate`）+ `:559`（`demodulator.demodulate`）**两处合并** |
| （v1.0 描述的） | 仅 均衡 + 解映射 | **已算好的信道估计** | LLR | `channel_equalizer::submit_fused()` |

#### 证据一：OCUDU dApp 的 Class A 契约（逐字，arXiv 2609.07843）

> "The input is the **full-slot device grid in complex BF16**, an **explicit DM-RS pilot tensor with
> bounded coordinates**, **compact ordered data-RE indices**, and **typed PUSCH metadata (allocation
> shape, ports, layers, modulation, noise floor, DM-RS layout)**, so **a module can estimate a channel
> without calling into the private PHY**. The outputs are caller-owned device tensors … : port-and-layer-major
> channel estimates plus noise at the **first depth**; compact equalized symbols in [data_re, layer] order
> plus post-equalization noise at the **second**; **FP16 soft bits in [data_re, layer, bit] order before
> descrambling at the third**. The three depths rejoin the chain at successively deeper points, and
> ★ **no hook replaces equalization alone, because the equalizer interface also implements channel
> estimation.**"

⇒ 三条结论：

1. ★ **你描述的输入与工业界的 Class A 契约逐项对应**：
   "explicit DM-RS pilot tensor with bounded coordinates" = 哪些是 DM-RS RE；
   "compact ordered data-RE indices" = 哪些是 data RE；
   "typed PUSCH metadata (allocation shape, ports, layers, modulation, DM-RS layout)" = 相对几何。
   即**"网格 + 几何"正是官方契约的输入形态**，不是我们自创的。
2. ★ **深度 3 是官方支持的形态**；预算 **depth 1 = 100 µs（estimation）**、
   **depth 2/3 = 150 µs（to completion）**。
3. ★ **"不存在只替换均衡的钩子"**——理由与我们的代码结构一致（见 §1.6）。

#### 证据二：文献里的 neural receiver 本来就是这个范围

- **Sionna**（逐字）：*"The neural receiver substitutes **channel estimation, equalization, and
  demapping**. It takes as input the post-DFT received samples, which form the received resource grid,
  and computes LLRs on the transmitted coded bits."*
- **DeepRx**（摘要逐字）：*"executes the whole receiver pipeline from **frequency domain signal stream
  to uncoded bits**"*，并 *"We facilitate accurate **channel estimation** by constructing the input of
  the convolutional neural network in a very specific manner **using both the data and pilot symbols**."*
  ——即 **DeepRx 包含信道估计**，而且输入构造显式用到导频位置。

⇒ ★ **结论：你的计划（CE+eq+demap）就是标准形态，literature 与 dApp 都包含信道估计。**
v1.0 的 `submit_fused` 只是这个形态的**一个子集**，用它做宿主会**漏掉 CE 那一段**。

### 1.5 ★ 深度 3 的代价：继承 CE 的"上报义务"（最容易漏的一条）

CE 不只是算信道估计——它还**产出一整套上报量**。`dmrs_pusch_estimator_results` 提供
`get_rsrp` / `get_epre` / `get_noise_variance` / `get_snr` / `get_time_alignment` / `get_cfo_Hz`
（`include/ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h:135-178`），
而 `pusch_processor_impl.cpp` 在末尾把它们**合并进 CSI 上报**：

```cpp
(void)est_results.sync_device_estimates();
est_results.get_channel_state_information(notifier_adaptor.get_channel_state_information());
```

dApp 论文对同一条约束的表述（逐字）：

> "A module that succeeds at the second or third depth **also supplies the scheduler's uplink SINR from
> its own post-equalization noise, because the conventional measurement kernels are skipped on that path.**"

⇒ ★ 深度 3 的 net **必须回答**"RSRP / EPRE / 噪声 / SNR / TA / CFO 从哪来"。三条可选路线：

| 路线 | 做法 | 评价 |
|---|---|---|
| **(a)** | net 附带输出**信道估计与噪声**，经典测量核**跑在 net 的输出上** | ★ **建议**：改动最小，**上报口径不变**，同时把最贵的算力移走 |
| (b) | net 自己出 SINR，其余测量保留一个轻量 DM-RS 经典块 | 可行；TA/CFO 本来就只需导频 |
| (c) | 全部由 net 出 | 最难；TA/CFO 与"解映射"不是同一类任务 |

### 1.6 好消息：我们的融合 lane 已经是"深度 3"的形状

`merged_hop` 是"**the whole hop's buffer**"（`lib/phy/metal/ocudu_metal_queue.mm:538`），
mkf033 实测中位 **449.8 µs**，它**已经**把 **CE 抽取 + CE 权重 + 均衡 + 解映射**放在同一个命令缓冲里。
⇒ 用 AI net 替换这一段，**正好是替换一个已经存在的融合单元**，
而不是要把两个独立阶段缝起来——这比"从 eq+demap 接缝往上够"要自然得多。

★ **而且这个融合单元是"一个网络"，不是两个**：`merged_hop` 之所以是一个 CB，
是因为设备侧已经把这几步合在一起了；换成 AI net 时若拆成"CE 网络 + EQ/DEM 网络"，
等于**把一个已经融合的单元重新切开**，还会多付一次 dispatch 地板
（mkf023：一个边界 ≈6.5 µs；ANE：单次 dispatch 地板 70–230 µs，见 `memo_03` §1）。
⇒ 设计文档 §2.1 给出"必须是一个网络"的四条理由。

## 2. LLR 契约（模型输出必须逐条满足）

`log_likelihood_ratio`（`include/ocudu/phy/upper/log_likelihood_ratio.h`）：

| 条目 | 值 | 出处 |
|---|---|---|
| 存储类型 | `int8_t` | `:20` |
| `LLR_MAX`（有限最大值） | **120** | `:235` |
| `±LLR_INFTY` | **±127**，保留给"确定比特"（无穷确定） | `:230` |
| 量化 | `log_likelihood_ratio::quantize(value, range_limit)`：mid-tread 均匀量化，步长 `range_limit/120`，超限裁剪到 `±120` | `:110-122` |

`range_limit` 按调制阶数取（`lib/phy/upper/channel_modulation/demodulation_mapper_*.cpp`）：

| 调制 | `RANGE_LIMIT_FLOAT` |
|---|---|
| BPSK / PI/2-BPSK | 24 |
| QPSK | 24 |
| 16QAM | 20 |
| 64QAM | 20 |
| 256QAM | 20 |

⇒ **设计决策（建议）**：模型输出**浮点 LLR**，量化交给既有的 `log_likelihood_ratio::quantize()` 一个函数完成。理由：(a) 训练损失可以在浮点域定义（KL / MSE / BCE 都自然）；(b) 量化误差是可单独度量的量，不被混进模型误差。

★ **输出标度是接口问题，不是训练细节**：经典 demapper 的 LLR 与噪声方差成反比（例：`demod_BPSK_symbol` 给出 `2√2·(Re+Im)/σ²`，`:19-27`）。如果模型的浮点输出整体标度与 `range_limit` 不匹配，量化后要么大面积饱和到 ±120、要么全部挤在 0 附近——两种都会让 LDPC 译码直接崩掉。所以规划里必须包含**标度校准**这一独立步骤，并且要在真实采集上验证量化前后 CRC 不掉。

---

## 3. 解扰域：模型应当输出**加扰域** LLR

融合通路的实际数据流（`pusch_demodulator_impl.cpp`）：

```
temp_llr（submit_fused 写入，:601）
  → memcpy 到 codeword（:710-712）
  → revert_scrambling(codeword, codeword, scrambling_seq)（:730）
  → ul_capture::capture_llr(codeword)（:764）
  → codeword_buffer.on_new_block(codeword, scrambling_seq)（:767）
```

结论：**`submit_fused` 写出的 LLR 是解扰前的（加扰域）**，解扰是其后一步、与后端实现无关的精确操作。因此：

- 模型输出**加扰域** LLR ⇒ 解扰保持经典 ⇒ 下游（解速率匹配 / LDPC 译码）完全不受影响 ⇒ A/B 对拍干净。
- 反之，若让模型直接输出解扰域 LLR，就必须把扰码序列喂给模型（或让它隐式学一个伪随机函数）。把纯计算、零信息量的活交给模型是浪费容量，**不采纳**。

### 3.1 标签侧的一个陷阱

`_llr.bin` 是**解扰后**的（`:764` 在 `:730` 之后）。所以：

- 拿它当"加扰域模型"的监督信号时，必须按同一序列**翻符号**。
- 序列可复现所需字段**都在** `.txt` 里：`rnti`（`ul_capture.cpp:145`）、`n_id`（`:149`），c_init 按 TS38.211 §6.3.1.1。
- ★ **`n_rapid` 没有被 dump**（msgA on PUSCH 的备用加扰初始化需要它）⇒ msgA 的采集无法精确重放解扰。建议语料构建时**直接排除 msgA**（占比极低），而不是猜。

---

## 4. 后端选型与开关先例

仓库现有的两条"实现可选"先例：

| 位置 | 机制 |
|---|---|
| `lib/phy/upper/upper_phy_factories.cpp:743-756` | 均衡器后端：`config.pusch_channel_equalizer_backend == "metal"` 时建 Metal factory，否则回落 generic；**带透明回退**（factory 为空即回退） |
| `lib/phy/upper/upper_phy_factories.cpp:624-627` | 均衡算法：`config.pusch_channel_equalizer_algorithm == "mmse"` → `mmse`，否则 `zf` |
| `lib/phy/upper/upper_phy_factories.cpp:691-696` | CE 选择器：`--pusch_channel_estimator_algo {auto,cpu,metal_mmse,metal_nn_mmse,helena}` |

⇒ 两条可选路由（规划里决策）：

- **(A) 复用 `pusch_channel_equalizer_backend`，增加一个 `"ai"` 取值。** 倾向此项：AI detector 在**接口层面就是一个 equalizer 后端**（它实现的正是 `channel_equalizer`），新增一个 `pusch_detector_backend` 会引入一个当前并不存在的抽象层。
- **(B) 新增 `pusch_detector_backend` / CLI 开关。** 与 AI CE 的 `--pusch_channel_estimator_algo helena` 对称，可读性好，但多一层概念。

无论哪条，都必须遵守仓库已有的两条纪律：**透明回退**（shape 不支持或模型缺失时回落经典，绝不静默出错）与**可判定性**（谓词是 backend+shape 的属性，不看数值）。

---

## 5. 标签来源（两条路线，优先级不同）

### 路线 1（首选，无采集成本）：CRC-OK TB → 逐 RE 真实比特

已有工具链 `ai_train/build_labels.py` 做的是 `TB → LDPC encode → rate match → scramble → modulate → X̂`。只要 PDU 配置 + CRC 通过的 TB，就能得到**真实发送符号/比特**。

★ 关键判断：**用真实发送比特做监督，比用经典解调器输出做监督更可取**。后者把教师的上限变成学生的上限（蒸馏天花板）；前者允许模型超过经典 demapper。两者可组合：**先蒸馏热身，再用真值比特微调**。

### 路线 2（回归/蒸馏）：参考 LLR

- `ul_capture` 的 `_llr.bin`（★ 注意 `OCUDU_UL_DUMP_LLR` **必须显式设置**，未设置就没有 LLR 文件；`ENABLE_UL_CAPTURE` 默认 OFF）。
- 或对只有 grid 的采集跑 `ul_chain_replay` 重新生成——它自己会设 `OCUDU_UL_DUMP_LLR=1`（`lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:502-504`）。

### 现有语料（来自 AI CE 勘察，本次未逐文件复核）

`/Users/jiachengwang/ai_ce_work/capture/`：

| 目录 | CE dump | rx grid | CRC-OK TB |
|---|---|---|---|
| `oaiue_helena_retest` | 152,874 | 152,874 | 134,181 |
| `site6_0831` | 75,131 | 75,131 | 42,849 |
| `site1_0831` | 55,064 | 0 | 0 |

`realtrain_s56*.npz` 的 `Y` 是**信道估计**，不是 LLR——这批 npz 是信道估计数据集，不能直接当 detector 数据集。

### 若扩语料，需要补的字段

- `n_rapid`（若必须覆盖 msgA）；
- 逐 RE 的后均衡噪声方差（`_ce.txt` 里的 `noise_variance` 是 per-port 的 DM-RS 域估计，不是逐 RE 后均衡噪声方差）。若模型需要真正的逐 RE σ²，要么自己估，要么接受 per-port 常数。

---

## 6. 现状性能锚点（这就是要被替换掉的东西）

| 量 | 中位数 | 出处 |
|---|---|---|
| `ce`（信道估计段） | 73.3 µs | mkf033 |
| **`eqdem`（均衡+解映射段）** | **667.4 µs** | mkf033 |
| `ul_pipeline`（整跳） | 1263.0 µs | mkf033 |
| HELENA@ANE，116k 参数，输入 (1,612,14,2) | p50 **141 µs** / p99 208 µs | AI CE G1 实测 |

⇒ 目标：把"均衡+解映射"这一段从 ~667 µs 变成**一次 ANE 前向**。但必须同时交代**同步代价**（GPU→ANE 的等待、ANE→LDPC 的可见性），否则会重演本仓库已经犯过两次的错误——"用两个测量相减做归因"（见 `phy_pipeline_crossings.h` 开头的记录）。

---

## 7. 需要新建的门类：统计门（AI 路径不可能 bit-exact）

现有工作流的验收方式**直接不可用**：offline bit-exact A/B（`ab_dumps.sh`，要求 0 差异）、CRC 100%、三 leg 复现。

AI 路径必须换成**统计门**，且在飞行前预登记判据：

1. **同批采集上的 CRC / BLER** ≥ 经典参考（同一批 capture，同一 LDPC 译码器）；
2. **LLR 质量**：与真实发送比特的逐比特 BER、或与经典 LLR 的相关性/互信息；
3. **时延分布**：p50/p99，且必须包含同步等待，不能只报 kernel 时间。

★ 借用本仓库已经付过学费的纪律：判据在飞之前写死；计数器必须能证明开关真的生效（"开关没生效 ≡ 没有效果"已经在这个仓库出现过三次）；每个数字都要能追溯到一次 leg/commit。

---

## 8. 本备忘未覆盖（留给后续文档）

| 主题 | 去处 |
|---|---|
| 论文 2503.16594v1 逐节读书笔记 | `memo_02_paper_2503.16594_reading.md` |
| 公开文献调研（架构/数据/损失/部署） | `memo_03_literature_survey.md` |
| 数据与标签工程细节 | `memo_04_data_labels_and_operating_point.md` |
| 设计文档（阶段、门禁、风险、工时） | `AI_LLR_detection_master_plan.md` |
