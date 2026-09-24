# Metal/UMA 架构 vs 上游 CUDA 架构 —— 对照分析与抽象层评估

> 状态：分析备忘（v1.0，2026-09-22）
> 目的：回答两个问题 ——
> **(a)** 上游 `ocudu/ocudu` main 上的 CUDA 架构，与我们的 Apple Silicon 异构设计是否一致？
> **(b)** 如果设计一个"硬件中立的 device-backend 接口层"，它能否满足我们"CPU / GPU 对等路径 + 按业务类型路由 + ANE 可插入"的架构需求？
>
> 结论先行：**(a) 不一致，且冲突在最根本的内存模型一层；(b) 不能满足** —— 而且"device-backend"这个框架本身就是错的（见 §5）。
>
> **⚠️ 本文的用途不是为上游合并做准备**（见 §7）：评价标准是给用户创造的价值，不是评审者的认可。
> 拿 CUDA 做对照，是为了**不采纳错误的抽象**、并**认出哪些工程手法真的可借鉴**。
>
> **两份文档的权威性归属**（据 `doc_chinese/README.md`）：
> - **`phy_pipeline_gpu/`** = **当前**的融合车道 port（batch 5a–5g，IQ → LLR 单设备流水线，
>   空中实测**零 host↔device 数据跨越**）。本文中"路由的真实形状"（§1.3）、
>   "严格性维度"（§1.6）与"判据体系"（§1.7）以它为准。
> - **`full_gpu_chain/`** = **更早的**全链 GPU/UMA 零拷贝尝试（融合车道之前），
>   但其中的 **FPGA DMA / UMA 零拷贝 / ANE 融合 / 五阶段长期蓝图**仍是前瞻性架构规划。
>   本文的"目标"（§1.1）、"两次跨越"（§1.4）、"三个平台级硬事实"（§1.5）与
>   "Hybrid Routing"（§6.2/§6.3）引自它。

---

## 0. 一句话结论

**上游 CUDA 的 ADT 把"设备内存是与主机分离的地址空间、必须显式拷贝"固化成了类型契约**；
而我们的设计目标恰恰是**同一地址空间 + 零拷贝 + 单次提交 + 单次回调**。
两者不是"实现不同"，而是**方向相反**。

同时，我们此前设想的"硬件中立的 device-backend 接口层"**不能满足我们的架构需求**，
因为它预设了"主机为主动、加速器为被委托方"的非对称关系，而我们的模型是
**"一个写者、多类读者，读者的属地决定路径"**（§2.3）。

---

## 1. 我们的高层设计（准确复述）

### 1.1 目标（`full_chain_gpu_uma_zero_copy_refactor_plan.md` §0.1）

> 把当前「CPU 主导、模块级 Metal/ANE 局部加速」的 RX 管线，改造为
> 「**FPGA DMA 直写 UMA → CPU 只做一次零拷贝包装与一次提交 → FFT/CE/均衡/解调/LDPC/CRC/FAPI
> 全程在 GPU 内部流转 → GPU 完成回调一次性唤醒 MAC**」的全链路零拷贝管线，
> 同时保留 **CPU/GPU/ANE 的 CLI 级 A/B 路由能力**。

### 1.2 双重异构（`apple_silicon_heterogeneous_gnb_plan.md` §0）

| 层 | 异构维度 | 组成 | 优势 |
|---|---|---|---|
| **算力异构** | 计算资源 | P 核 / E 核 / **GPU** / **NPU(ANE)** | 时延敏感走 CPU 核、高并发走 GPU、AI 推理走 NPU |
| **存储异构** | 数据存放 | 寄存器/缓存 / **统一内存(UMA)** / flash(mmap 页缓存) | 热数据驻统一内存零拷贝、冷数据驻 flash 按需换页 |

**⇒ 这不是"算力 offloading"。** 目标形态里 GPU 不是被委托的加速器，而是与 CPU **同等地位的候选计算路径**。

### 1.3 路由的真实形状（`session_handoff_2026-09-21-6.md` §3.5）

这一节是对"路由到 CPU/GPU path"最精确的表述，必须逐字理解：

> **有没有 PUSCH 是 gNB 事先知道的**（上行是它自己授权的，`UL_TTI.request` 在样本到达前就填好了 PDU 仓库）。
> **分支点是资源网格，不是 FFT kernel**：一个写者（FFT+写网格）、三类读者——
> PUSCH（**设备**读者，在 GPU 内继续）、PUCCH/SRS（**宿主**读者，先等栅格产出栅栏）、PRACH（**不碰网格**，时域）。
> `s44` 的 30635 块网格去向：**跳 41% / 宿主读者 50% / 无人认领（扫掠）9%**。

**三条由此推出的设计约束：**

1. **路由决策点在资源网格，不在 IQ 入口，也不在某模块的工厂里。**
2. **决策是"提前已知"的**（`UL_TTI.request` 先于样本到达），所以路由可以是**静态且可判定**的，
   不需要运行期探测。
3. **同一份网格会被不同属地的读者消费**（设备读者 / 宿主读者），所以"网格的属地"
   不是网格自身的属性，而是**读者集合的属性**。

### 1.4 唯一允许的两次跨越（蓝图 §10.2）

```
host IQ (radio) ──upload──▶ device grid (每 slot, 每端口)
                                 │ (GPU 内部一路向前)
                                 ├─▶ K0-a 导频提取 / K0-b LSE+CFO / K0-c 统计量 / K0-d 相关矩阵
                                 ├─▶ K1/K1b/K2 权重+估计 ─▶ K3 设备估计 + K4 设备噪声
                                 ├─▶ 均衡(读设备估计) ─▶ 解调(同一 CB)
                                 └─▶ 解扩/合并 ─▶ device LLR buffer
                                                      │ download (每 slot 一次)
                                                      ▼
                                                 host LLR ─▶ LDPC(CPU)
```

> **允许的主机↔设备数据跨越只有两处**：IQ 上传（不可避免，样本本来就在主机）与 LLR 下载（流水线产物）。
> 其余"数据"（网格、导频、估计、噪声、均衡输出、LLR 中间态）**全部留在设备**。

### 1.5 三个平台级硬事实（蓝图 §0.2，一切设计前提）

| # | 事实 | 设计后果 |
|---|---|---|
| 1 | **ICB 在本平台被阻断**（darwin 25 / AGX G16X 实测：`setComputePipelineState` 触发 GPU Address Fault，`setKernelBuffer` 被静默忽略） | 第一版全链**不得押在 ICB 上**；必须走"CPU 编码 + 单命令缓冲 + GPU 内部 MTLEvent 依赖" |
| 2 | **Metal MSL 无跨 threadgroup 软件栅栏**（只有 relaxed 原子，无 acquire/release/seq_cst、无 `atomic_fence`） | 跨 TG 依赖必须用 **MTLEvent / MTLFence / 命令缓冲内顺序**表达，或沿用单 TG 持久 kernel |
| 3 | **Apple Silicon 上没有 CPU 亲和性**（`compat::set_thread_affinity` 是 no-op） | 用 **QoS 分类 + Mach time constraint + GPU 卸载**替代 x86 式绑核 |

### 1.6 严格性维度：`cpu_gpu` 与 `gpu` 的语义差别（`gpu_phy_pipeline_design_and_implementation.md` §1.8）

用户裁定（2026-09-20）：

> "在原来的多模块串联（`cpu_gpu`）里，CPU 和 GPU 本来都要参与，所以设计了一个'CPU 托底'的回退来
> 增强 pipeline 的健壮性。但我们现在是 GPU only —— **有错误就应该直接报错，而不是还是 CPU 来托底**。"
>
> "我们的首要目标不是收益，而是在确保功能正确的前提下，**让 CPU 全程靠边站**。"

**⇒ 回退在 `cpu_gpu` 里是健壮性，在 `gpu` 里是缺陷。** 历史证据：S13-P2c 的窄跳病
（2 PRB 恒 0% CRC）**正是靠这条静默回退活了很久**；若当时就是"拒绝即报错"，第一次窄跳就会当场暴露。

**任何抽象层都必须能表达这两种语义。**

### 1.7 判据体系（§3.1 / §3.2）——我们已有的硬件中立抽象

| 机制 | 位置 | 性质 |
|---|---|---|
| 穿越计数器 | `phy_pipeline_crossings.h`：`count_host_read(bytes)` / `count_host_write(bytes)` / `count_device_hop()` / `declare_reporter(name)` | 数的是**跨越次数**，不是 API 调用 —— **硬件中立** |
| 契约 8 条检查 | `phy_pipeline_contract.h`：三态 `std::optional<bool>`（`true`=OK / `false`=FAILED / `nullopt`=不适用且不进分母） | 报告，不 abort |
| 承重项 | 第 5 条 `host device data crossings`：`mode==gpu ⇒ reads==0 && writes==0` | 直接对应"CPU 靠边站" |
| 守卫项 | 第 6/7/8 条（CFO 往返 / 基带指标 / 样本组装）：零分子即通过 | 价值在"发生了就报红" |
| 精度判据 | §1.5 裁定：**不必逐字节复现**；用 NMSE / 逐 RE 相对误差分位数 / 符号翻转比例，最终口径是**带蒙特卡洛误差棒的 SNR 位移** | 容忍 0.5 dB |

> **注意**：`8/8` 不等于"整条车道零穿越"（第 5 条只数申报过的模块，且 IQ 上传与 LLR 下载**不计**），
> 也不等于数值正确（8 条全是结构性计数）。判读时必须连读同一行的 scope 后半句。

### 1.8 ⚠ 审计裁决（5.9.120，8 轴）：契约的可信度边界

**上面这节把契约当作"我们已有的硬件中立抽象"。但项目自己在 5.9.112–5.9.120 对这套仪器做了
逐项审计，结论必须一并读——否则会把这套检查当成比它实际更强的证据。** 审计原文见
`phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` §5.9.120。

| 审计轴 | 裁决 |
|---|---|
| **A1/A5 契约八项 + 反向臂** | **名字 8/8 齐、1:1 对应**；但**六条可 `nullopt`**、**两条"永远适用且零分子也算过"**、**一条既无臂也无配方**；分母 `N` 今天是**可缩的**（注册跟着代码路径走） |
| **A2 目标属性重测** | 旧差距表**逐行消失**；但目标里"**恰好两次穿越**"这半句**没有任何计数器** |
| **A3 离线门本身** | 门**既是瞎的又是宽的**：`-R` 大小写敏感（漏 466 条）、`o_du` 是子串陷阱（19 条噪声）；项目自己的 `-L phy` 是 **175/175 全过** |
| **A4 腿证据台账** | 132 条腿可读；**没有一条是重上行腿**；"0 RF 实时失败"**在最近 6 条默认腿里有 4 条不成立**且**无判据** |
| **A6 文档/代码一致** | 查到 4 处陈旧断言（含 §3.2 表与目标文档引用的已删拒绝串），**当日全部更正** |
| **A7 构建/可运行/回滚** | `gpu` 模式**仅靠配置即可选**（无拒绝）；冲突规则有单测；`--dryrun` **不跑配置校验器**（注释已更正）；**HEAD 上没有里程碑 tag** |

**另有具体更正（影响本文件前面引用过的两处）：**

1. **第 2 条检查 `dft radio inputs` 曾经"没有测量它所声称的东西"**（5.9.99），
   且该检查的**分母不是总数**，所以它的 OK **不可信**；5.9.111 又撤回了 5.9.99 的归因
   （PRACH 有**自己的** `ofdm_prach_demodulator_impl`，从不碰 rx DFT 引擎），
   5.9.118/5.9.119 最终把它归到"PRACH 解调器自己的引擎实例 + 一次进程级预热"，
   并指出"11346 槽 × 14"是一次**单位错误**（正确口径是 `12×N+1`）。
2. **`cbs/lane` 的口径与实现机制都已变化**（见下面的 §1.9）。

**⇒ 对 §5 评估的影响：不变，反而被强化。**
审计恰恰在证明本文件 §5.2 #8 的判断——**"一个不能失败的检查不是证据"**。
反向臂（5.9.112、5.9.114）正是在给每条检查补"它能不能变红"的证明，
而 5.9.120(2) 连自己的两条发现都撤回了。**这套仪器在自我加固，而不是在被当作结论使用。**

### 1.9 D1 交接的实现机制已变更（5.9.87–5.9.94）：同一个成果，不同的机制

本文件 §1.4/§1.5 描述的"每跳一次提交"在**实现上换了机制**，记在这里以免继续引用旧口径：

| | 旧机制（前缀形式） | **新机制（当前默认）** |
|---|---|---|
| 做法 | K0-d 相关构建作为**引擎命令缓冲的 prefix**（中间插 `memoryBarrierWithScope`） | `flush_correlations_fenced()` / `queue_correlation_fenced()` —— **带栅栏的相关构建**，批处理为**每跳一条命令缓冲** |
| 状态 | ❌ **有雷**：5.9.87 隔离出"prefix 的 A 从未到达 K1"，5.9.88 逐跳确认链路断在 prefix | ✅ 默认路径（5.9.93 落地，5.9.90/5.9.92 空中 A/B） |
| 反向臂 | — | **`OCUDU_CE_CORR_FENCED=0`** 保留旧 prefix 形式，即反向臂 |
| 观测 | （离线单测读到 3.00 → 2.00） | **空中确认 `cbs/lane` 2.70 → 2.00**，且"槽级跨度**优于** prefix 腿"（5.9.94） |

**⇒ 结论不变（每跳一次提交、`cbs/lane` = 2.00），但机制换了，而且旧机制本身有缺陷。**
**引用时不要说"K0-d 作为 prefix 骑在引擎的命令缓冲上"**——那个形式现在是**反向臂**，不是生产路径。

**注意口径**：`cbs/lane` 的分母是**车道**（每线程每槽），所以它度量的是"提交形状"，
不是"提交总数"；`[ul_gpu_lane]` 表头行同时给出 `lanes` 与 `max`，判读时要一起看。



---

## 2. 上游 CUDA 架构（代码实测事实）

### 2.1 内存模型：独立设备地址空间（决定性）

```cpp
// include/ocudu/cuda/adt/device_vector.h
/// The memory is only addressable by the device: the host cannot dereference data().
/// Transfers between host and device memory are performed with the helpers in cuda_copy.h.
template <typename T> class device_vector { /* ... */ };
```

```cpp
// lib/cuda/adt/device_vector.cpp
cuda_result result = check_cuda_error(::cudaMalloc(&ptr, size), "device allocation");
```

**全库检索结果**：`lib/cuda/` 与 `include/ocudu/cuda/` 中
`cudaMallocManaged` / `cudaMemAdvise` / `cudaHostAlloc` / `cudaMemPrefetch` **一个都没有**。

⇒ **纯独立设备内存模型。** `device_vector` 是"设备内存的分配器"，不是"设备可寻址的内存视图"。

### 2.2 数据搬运原语就是核心 API

`cuda_copy.h` 提供 `copy_to_device` / `copy_to_host` / 两者的 `_async` 变体 / `device_memset_async`。

也就是说：**"把数据搬到设备上"是这个 ADT 的中心操作**，而不是要被消除的对象。

### 2.3 PRACH 后端的形状：上传 → 计算 → 下载

```cpp
// include/ocudu/cuda/prach/prach_detector_backend.h
cuda_result detect(prach_detector_output&         output,
                   span<const uint32_t>           input,     // ← 主机内存里的样本
                   span<const cf_t>               roots,
                   const prach_detector_geometry& geometry,
                   const cuda_stream&             stream);
/// The call is synchronous: it returns once the results have been read back from the CUDA device.
```

实现内部（`prach_detector_backend.cu`）：

```cpp
result = copy_to_device_async(d_input, input.first(input_size), stream);
...
result = copy_to_host_async(span<result_block>(&h_results, 1), d_results, stream);
```

### 2.4 组件清单与设计亮点（值得单独记录）

| 组件 | 内容 |
|---|---|
| `cuda::device_vector<T>` | RAII、move-only、**不可 resize**："accelerated paths allocate for the largest configuration they will process and keep the block for their lifetime rather than reallocating per transmission" |
| `cuda::cuda_stream` | `cuda_stream_priority{normal, high}`；high = "for work with a hard deadline" |
| `cuda::cuda_event` | "bound the lifetime of a host buffer handed to an asynchronous copy: the buffer may only be reused once the event recorded after the copy has completed, otherwise the copy reads memory the host has already overwritten" |
| `prach_detector_geometry` | 参数由**调用方**推导，后端不查 3GPP 表 —— "so that the accelerated and the generic detectors always agree on the geometry they search" |
| `prach_detector_cuda_configuration` | `min_short_preamble_work=64` / `min_long_preamble_work=4` —— **卸载门限** |
| 装饰器工厂 | `create_prach_detector_factory_cuda(fallback_factory, generator_factory, config)` —— 强制注入 CPU fallback，"It always produces a result" |
| `prach_detector_cuda_factory_unavailable.cpp` | **链接期替换**：CUDA 关闭时编译占位实现，调用方代码无需任何 `#if` |
| `add_cuda_test.cmake` | `RESOURCE_LOCK ocudu_cuda_device`（串行化 GPU 测试，其余照常并行）+ `LABELS "cuda"`（并保留目录级 label） |
| 构建 | `ENABLE_CUDA` 默认 OFF；`CMAKE_CUDA_ARCHITECTURES` 强制要求（FATAL_ERROR 里直接给出 `nvidia-smi` 查询命令）；依赖 **VkFFT**（header-only，不 vendor） |
| 归属 | 版权头为 **DeepSig Inc** —— 外部公司贡献 |
| 规模 | 8 个提交、26 个文件；`.cu` 715 行（真实内核：`kernel_prepare_idft`、`kernel_find_candidates`、warp 归约） |
| **接入状态** | **未接入应用** —— `create_prach_detector_factory_cuda` 只被单元测试调用，无 CLI/YAML 开关 |

---

## 3. 逐层对照

| 层 | 上游 CUDA | 我们的 Metal/UMA 设计 | 一致？ |
|---|---|---|---|
| **内存模型** | 独立设备地址空间；`cudaMalloc`；主机**不可**解引用 | **同一地址空间**；`wrap_no_copy` 在主机内存上建 MTLBuffer，零拷贝 | ❌ **根本冲突** |
| **数据搬运原语** | `copy_to_device` / `copy_to_host` 是核心 | **搬运是要被消除的对象**；只允许两次 | ❌ |
| **输入契约** | `detect(..., span<const uint32_t> input, ...)` 主机 span | `iq_slot_view.base` —— 提供**设备可直接寻址的地址** | ❌ |
| **执行同步** | 同步调用（"returns once the results have been read back"） | `addCompletedHandler` 单次回调、0 次 `waitUntilCompleted` | ❌ |
| **路由决策点** | 模块工厂（装饰器注入） | **资源网格**（一个写者、三类读者） | ❌ 位置不同 |
| **路由时机** | 运行期（每次 detect 看门限） | **提前已知**（`UL_TTI.request` 先于样本） | ❌ |
| **严格性语义** | "always produces a result"（永远回退） | `cpu_gpu` 回退=健壮；**`gpu` 回退=缺陷** | ❌ **语义相反** |
| **排序 / 生命周期** | `cuda_event` | §0.2 硬事实 #2 要求 **MTLEvent**；`retain_for_block()` token | ✅ **概念一致** |
| **优先级 / 队列** | `cuda_stream_priority{normal,high}` | 前/后端双队列 + QoS 分类 | ✅ 同构 |
| **资源预分配** | 按最大配置预分配、终生持有 | "粮草先行"：构造期预建 102 个 (BG,z) 槽 | ✅ 原则一致 |
| **卸载门限** | `min_*_preamble_work` | "TBS ≤ ~200 B → CPU"、"小 TB 无交叉点" | ✅ 同构 |
| **门控/不可用** | `_unavailable.cpp` 链接期替换 | 预处理 `#if defined(OCUDU_METAL_*)` + CMake `if()` | ⚠️ 可借鉴 |
| **测试注册** | `add_cuda_test.cmake`（`RESOURCE_LOCK` + `LABELS`） | 已用目录级 `LABELS "phy"`；**缺 `RESOURCE_LOCK`** | ⚠️ 可借鉴 |
| **NPU / ANE** | **无对应物** | ANE 在 `doc_chinese/` 出现 **1115 次**，是三路分流的一路 | ❌ 无法表达 |
| **输入源** | 主机内存 → `cudaMemcpy` | **FPGA XDMA 总线主设备直写 UMA** → PCIDriverKit dext → MTLBuffer | ❌ |
| **度量体系** | 无（未接入应用） | 穿越计数器 + 8 条契约检查 + 精度判据 | ❌ CUDA 侧无此层 |

### 3.1 一个可判定的判据

> **问：`prach_detector_backend::detect()` 的签名能否表达 `iq_slot_view`？**
>
> **答：不能，而且原因是结构性的。**
>
> - `iq_slot_view` 提供的是一个**已经可被设备寻址的地址**，语义是"东西已经在能被 GPU 直接读的地方了"；
> - `detect()` 的契约**要求输入在主机内存里**，因为它内部要 `copy_to_device_async`。
>
> **采用后者的接口 = 强制把离散模型引入我们的设计**，而 §10.2 的全部努力就是要让"数据跨越"
> 从 N 次降到 2 次。这不是风格差异，是方向相反。

### 3.2 一个需要精修的既有表述

我们此前常用"x86 上算力 offloading 受 PCIe 带宽约束"来论证 UMA 的必要性。
但**我们自己的带宽预算**（蓝图 §3.1）算过账：

> 2RX + 2TX 全双工 ≈ 368 MB/s ≈ 3 Gbps；Thunderbolt 5 单向 ≥ 60 Gbps、PCIe Gen3 x4 ≈ 32 Gbps
> —— **裕度 > 10×，瓶颈绝不在链路而在软件**。

⇒ **即使在我们的链路上，带宽也不是绑定约束。** 真正杀死离散模型的是**拷贝本身的时延、CPU 周期与抖动**，
不是 GB/s。这个口径更准确，也更强：

- 有带宽余量 ≠ 拷贝免费 —— 一次 `cudaMemcpy` 仍要走驱动、占 CPU、加时延、引入排序点；
- UMA 的价值是**把一次拷贝变成零次、把一个同步点变成零个**，而不是"省带宽"；
- 所以 KPI 写的是"每槽 1 次 commit、0 次阻塞等待、中间态 0 次回传"，而不是"带宽提升 X 倍"。

**措辞建议**：我们真正依赖的机制是 **"FPGA 作为 PCIe 总线主设备直写主机 RAM（bus-master DMA into UMA）"**。
`doc_chinese/` 中 **RDMA 只出现 1 次**而 **DMA 是主线** —— 建议统一用后者，
否则评审者会误以为我们在做网络侧内存语义。（若 RDMA 确为另一条独立路线，它目前尚未写进设计。）

---

## 4. 结论 (a)：与我们的高层设计不一致

**不一致发生在最根本的一层 —— 内存模型。** 具体三条：

1. **上游 CUDA 的 ADT 把"分离地址空间 + 显式拷贝"固化成了类型契约**（`device_vector` + `cuda_copy`）。
   我们的设计目标是同一地址空间下的零拷贝，两者方向相反。
2. **它没有 ANE 的位置**（我们的算力异构有 CPU/GPU/ANE 三路，且 ANE 与 GPU **可并行**，因为它们是不同硬件单元）。
3. **它没有"可编程输入源"的位置**（我们的输入是 FPGA DMA 直写 UMA，CUDA 的模型是"主机内存 → memcpy → 设备内存"）。

**并且它当前尚未接入任何应用**（只被单元测试调用），所以"重用"也没有现成的生产语义可借。

---

## 5. 结论 (b)：我们草案的自我评估 —— **"device-backend 接口层"不满足架构需求**

### 5.1 我此前草案的内容（如实记录）

我此前建议的形状大致是：

```
"device-addressable memory view"（不是 device memory allocation）
  + 事件/生命周期令牌（对应 cuda_event / retain_for_block）
  + 卸载门限
  + 工厂装饰器 + 永远回退
```

**这个草案的动机来自"让 Metal 与上游 CUDA 有共同抽象"，而不是来自我们的架构需求。**
这是它失败的根本原因。

### 5.2 逐条评估：它为什么不满足

| # | 我们架构的硬需求 | 草案是否满足 | 缺陷说明 |
|---|---|---|---|
| 1 | **GPU 与 CPU 是同等地位的候选路径** | ❌ | "device-backend" 这个名字就预设了"主机为主动、设备为被委托方"的**非对称**关系。我们需要的是**路径对等**，不是"后端可选" |
| 2 | **路由决策点在资源网格**（一个写者、三类读者） | ❌ | 草案把决策放在**模块工厂**里（"这个模块用哪个 backend"）。而真实的分支点是"**哪些读者消费这份网格，以及它们各自的属地**"。位置错了 |
| 3 | **路由可提前判定**（`UL_TTI.request` 先于样本） | ❌ | 草案是运行期逐调用决策（像 `detect()` 每次看门限）。我们的 PUSCH 存在性**在样本到达前就已知**，应当是**静态规划**而非运行期探测 |
| 4 | **同一份网格被不同属地的读者消费** | ❌ | 草案里"内存"有一个属地属性；我们的模型里**属地属于读者集合**，不属于数据。`grid_has_host_consumers()` 这个函数名恰好说明属地是**推导出来的**，不是给定的 |
| 5 | **严格性有两档**：`cpu_gpu` 允许兜底、`gpu` 禁止兜底 | ❌ **语义相反** | 草案抄了 CUDA 的"always produces a result"（永远回退）。而 `gpu` 模式下**回退是缺陷**，正确行为是"该 PUSCH 失败 + 一行 ERROR"。抄过来会直接违反 §1.6 的裁定 |
| 6 | **ANE 是车道内的模块级参与者** | ❌ | 草案把 ANE 隐含地当作与 GPU 并列的"另一个 backend"。但 §5.3 的设计是：CE 在 GPU 链内用 `metal_nn_mmse`、在 CPU 链内用 `helena`(ANE)，且**两者可同槽并行**。ANE 是**路径内的一个可选环节**，不是一条路径 |
| 7 | **排序/认领以 `(网格存储, 槽)` 为键** | ❌ | 我们的整套顺序模型建立在 `grid_ready_hook::wait(base, slot)`、`deposit_released`/`take_released`（按网格基址配对）之上。草案的事件令牌是**无键的**，无法表达"这一跳认领的是那一块网格" |
| 8 | **已经有硬件中立的度量抽象** | ❌ 重复造轮子 | `phy_pipeline_crossings.h` + 8 条契约检查**已经是**硬件中立的那一层（它数"跨越"，不数 API）。草案没有接入它，反而另起一套 |
| 9 | 事件/生命周期令牌 | ✅ | 这一条是对的，而且与 §0.2 硬事实 #2（必须用 MTLEvent）方向一致 |
| 10 | 资源预分配 / 优先级队列 / 卸载门限 | ✅ | 这三条与我们的做法同构，可以对齐措辞 |

**⇒ 结论：10 条里 8 条不满足，其中第 5 条是语义相反的硬冲突。**

### 5.3 更根本的错因

草案试图找"Metal 与 CUDA 的共同抽象"，于是必然落到两者**唯一能共享的那一层**——
"内存 + 拷入/拷出 + 事件"。但那一层恰好是**我们刻意要消除的那一层**。
换句话说：

> **能被 CUDA 共享的抽象，必然是我们不需要的抽象。**
> 我们真正独特的东西（UMA 零拷贝、网格属地、FPGA 直写、ANE 并行）在 CUDA 模型里没有对应物，
> 所以它们不会出现在任何"共同抽象"里。

---

## 6. 应该是什么形状（建设性提案）

正确的框架不是 **device-backend**，而是 **"网格读者属地 + 车道"**。

### 6.1 三个核心概念

**① 读者属地（reader residency）—— 描述"谁在哪里读这份网格"**

```
for each reader class of a produced grid:
    PUSCH      → device   (在 GPU 内继续)
    PUCCH/SRS  → host     (先等栅格产出栅栏)
    PRACH      → 不是网格读者：独立的并行链（时域）
```

**⚠️ PRACH 一行在 5.9.111/5.9.119 被更正过，不能按"零成本旁路"理解：**

- PRACH 有**自己的** `ofdm_prach_demodulator_impl` 与**自己的 `dft_processors_table`**，
  **从不碰 rx DFT 引擎**，也不碰资源网格；
- 但它自己的 **1200 变换/s 现在是"每变换一条命令缓冲 + 一次 wait"**（普通路由）；
- 引擎里已有的 block API（`begin_block`/`end_block`）能把**一次 occasion 的 12 个符号并成一条**，
  判据已预先写死（`commits` 从 ~1200/s 降到 ~occasions/s，`wrap_copies` 仍为 0，契约仍 8/8，
  变换总数 `12×N+1` 不变），**且可由 PRACH 现有单测离线驱动**；
- **5.9.119 只登记、未实施**（收益约 1.5% 单线程时间，需自己的臂与反臂）。

**⇒ 对本模型的影响**：读者属地只能描述**网格的读者**，而 PRACH 说明**还存在不为网格服务的并行链**，
它们有自己的提交形状与开销。**任何"统一路径抽象"都必须给这类链留位置**，
否则会像我的草案一样，把 PRACH 误当成免费的旁路。

这不是新机制——`grid_has_host_consumers()` / `host_reads_the_grid()` /
`consumes_device_estimates()` / `consumes_gathered_symbols()` 已经在近似它。
**要做的是把它从散落的谓词提升为一个显式的、可查询的声明**，
并让 `grid_ready_hook` 的认领策略直接由它决定。

**② 车道（lane）—— 与 backend 的区别**

| | device-backend（错误框架） | lane（正确框架） |
|---|---|---|
| 关系 | 主机**委托**设备 | CPU 车道与 GPU 车道是**对等候选** |
| 决策点 | 模块工厂 | 网格的读者集合 |
| 决策时机 | 运行期逐调用 | 提前（`UL_TTI.request` 已知） |
| 输入 | 主机内存 span | 同一个已产出的网格 / IQ |
| 输出 | 结果回主机 | 各自的产物（LLR / PDU），**不互相回传** |
| 回退 | 永远兜底 | **由模式决定**：`cpu_gpu` 兜底、`gpu` 报错 |

**③ 严格性作为车道的显式属性**

```
struct lane_policy {
  bool allow_host_cover;   // cpu_gpu: true   /  gpu: false（拒绝即报错）
  ...
};
```

§1.6 的实现形状已经存在，照抄即可：`pusch_processor_impl.cpp:143-161` 的"拿不到依赖"路径
= `ERROR` + `on_sch({})`（CRC KO ⇒ MAC 重传）+ 直接返回。

### 6.2 ANE 的位置

ANE 不是第四条车道，而是**车道内的一个可选环节**，有自己的属地：

- 在 GPU 车道内：CE 用 `metal_nn_mmse`（张量化后 ≤30 µs，无需 ANE）；
- 在 CPU 车道内、大带宽时：CE 用 `helena`（ANE，141 µs < metal_mmse 255 µs，**且不占 GPU 队列**）；
- **A/B 竞争模式**：GPU 链与 ANE **同槽双跑 shadow**，以 GPU 链为准，ANE 只记 NMSE/时延；
- 统一张量描述符（`tensor_desc.h`）让 **GPU 输出缓冲可直接作为 CoreML 输入**
  （HELENA 的零拷贝双向包装已证明可行）。

⇒ 抽象层需要的是"**一个环节可以声明自己在哪个计算单元上、且其输入可以是另一环节的设备侧产物**"，
而不是"给 ANE 也做一个 backend"。

### 6.3 路由策略是独立注入的决策对象

`phy_routing_policy`（蓝图 §5.3）应当是**独立于工厂的决策对象**，输入是
TBS / MCS/SINR 预估 / UE 数 / 队列深度 / ANE 负载 / 槽预算余量，
输出是 `{ce 算法, ldpc backend, force_cpu_chain}`。
它需要感知 `last_predict_us` 与 ANE keep-alive 状态。

**这个对象与硬件完全无关，也是唯一适合与上游共享的"策略层"。**

### 6.4 真正可以共用/借鉴的最小面（修正后的清单）

| 项 | 性质 | 说明 |
|---|---|---|
| **事件 / 生命周期令牌** | ✅ 借鉴 | 与 §0.2 硬事实 #2 一致；对应用 `retain_for_block()` 解决过的 D1 交接故障 |
| **`_unavailable.cpp` 链接期替换** | ✅ 借鉴 | 消除 `lib/phy/metal` 的门控隐患，并让调用方摆脱 `#if` |
| **`add_cuda_test.cmake` 的 `RESOURCE_LOCK`** | ✅ 借鉴 | 你们已解决 LABELS，这是剩下那半 |
| **"参数由调用方推导、后端不查 3GPP 表"** | ✅ 借鉴为规范 | 与"每个模块都要能逐位/精度对拍"同一件事 |
| **装饰器工厂 + 卸载门限** | ⚠️ 部分借鉴 | 形状可用，但**回退语义必须由模式决定**，不能抄"永远兜底" |
| **`device_vector` / `cuda_copy`** | ❌ 不借鉴 | 会把离散模型引入设计 |
| **`prach_detector_backend` 的接口形状** | ❌ 不借鉴 | 主机 span 输入 + 同步返回，方向相反 |

---

## 7. 本文的用途（**不是**为了上游合并）

**⚠️ 先纠正一个隐含前提。** 本文（以及关于"该不该重用 CUDA 抽象"的讨论）曾经隐含地服务于一个目标：
**让评审者容易接受、从而合并到上游**。**那个目标已经作废——它不是评价标准。**

> **评价标准是给用户创造的价值，在 OCUDU 这个例子里就是 E2E performance，
> 特别是在 V2X 这类 URLLC + eMBB 并存的异构边缘场景。**

**⇒ 那么本文的用途是什么？两条，都与"合并"无关：**

1. **不要采纳错误的抽象**（§5）。上游 CUDA 的 lookaside ADT 会把"分离地址空间 + 显式拷贝"
   固化进类型系统，而那正是我们整个设计要消除的东西。**这条结论与服务谁、合并给谁无关。**
2. **知道什么可以借鉴**（§6.4）。事件/生命周期令牌、`_unavailable.cpp` 门控、`RESOURCE_LOCK`
   测试注册、装饰器+门限的形状——这些是**硬件中立的工程手法**，与上游无关也照样值得用。

**⇒ 因此下面这份"上游化建议"降级为附注，不再是本文的目的：**

**与上游共享抽象的时机**（如果将来有必要）：**当上游把第二个 CUDA 模块落地、开始需要跨模块 device
数据流转时。** 那时他们会遇到我们今天遇到的同样问题（中间态回传、同步点数量、栅栏语义），
而**"device-addressable view"（而不是"device memory allocation"）**才是双方都能接受的收敛点。
在他们有那个痛点之前提这件事，既推不动，也不是我们该花时间的地方。

**我们要做的，是把 LLDP（low-level design point）层面的抽象对齐到我们的架构**，而不是对齐到上游：

| 优先级 | 动作 |
|---|---|
| P0 | 把散落的属地谓词（`grid_has_host_consumers` / `host_reads_the_grid` / `consumes_device_*`）提升为**显式的读者属地声明** |
| P0 | 把 §1.6 的严格性做成**车道属性**（`allow_host_cover`），而不是散落的 `phy_pipeline_strict_enabled()` 分支 |
| P1 | 把 `phy_routing_policy` 从蓝图落到代码（先只做 PUSCH 的 TBS 门限 + `force_cpu_chain`） |
| P1 | 借鉴 `_unavailable.cpp` 与 `RESOURCE_LOCK` 两项（高收益、低风险、与硬件无关） |
| P2 | 统一张量描述符 `tensor_desc.h`，让 ANE 成为"可插入的环节"而非"另一条车道" |

---

## 8. 附：本文引用的关键位置

| 主题 | 位置 |
|---|---|
| 五阶段蓝图、两次跨越、Hybrid Routing | `doc_chinese/full_gpu_chain/full_chain_gpu_uma_zero_copy_refactor_plan.md` §0.1/§0.2/§5.3/§10.2 |
| 软硬解耦双支线、`iq_source` / `iq_slot_view` | `doc_chinese/full_gpu_chain/soft_hard_decoupled_execution_plan.md` §1.1 |
| 路由的真实形状（分支点是网格） | `doc_chinese/phy_pipeline_gpu/session_handoff_2026-09-21-6.md` §3.5 |
| `mode=gpu` 不允许宿主兜底 | `doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` §1.8 |
| 数值不必逐字节复现 | 同上 §1.5 |
| 穿越计数器 API 与契约 8 条检查 | 同上 §3.1 / §3.2 |
| 带宽预算 | 蓝图 §3.1 |
| CUDA ADT（内存模型） | `include/ocudu/cuda/adt/device_vector.h`、`lib/cuda/adt/device_vector.cpp` |
| CUDA PRACH 后端接口 | `include/ocudu/cuda/prach/prach_detector_backend.h` |
| CUDA 门控与测试助手 | `lib/phy/upper/channel_processors/prach/prach_detector_cuda_factory_unavailable.cpp`、`cmake/modules/add_cuda_test.cmake` |
