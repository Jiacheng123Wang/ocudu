# OCUDU dApp 平台代码调研（memo 06）

> 调研对象：`gitlab.com/ocudu/work_groups/wg2_ai_ran` 组下的 **5 个公开仓库**。
> 目的：**分析对我们工作的启示——哪些可以重用、哪些需要修改**；
> 并在高层架构上保证可扩展性与可移植性（参照 `metal_vs_cuda_architecture.md`）。
>
> 版本：v0.9（初稿）｜ 日期：2026-10-08

---

## 0. ★ 更正：该组是**公开的**，我上一轮判断错了

我上一轮说"`wg2_ai_ran` 是私有的、需要授权"——**错误**。
真实原因是我把**组路径当成了项目路径**（`ocudu/work_groups/wg2_ai_ran` 是**组**，不是项目），
所以拿到 404。用正确的项目路径访问后 API 返回 200。

**组内 5 个公开项目，已全部 clone 到 `~/dev/`：**

| 仓库 | 大小 | 是什么 | 价值 |
|---|---|---|---|
| ★ **`ocudu-dapp-platform`** | 109 M | **带 dApp 运行时的 OCUDU gNB**（完整 RAN fork） | ★★★ 接缝就在这里 |
| ★ **`ocudu-dapp-sdk`** | 3.4 M | **dApp 授权工具包**：ABI 头文件、参考实现、脚手架、验证阶梯 | ★★★ 契约与参考实现 |
| `ocudu-dapp-quickstart` | 232 K | 容器化构建 + **零硬件 E3 测试台**（loopback E3，无需射频/GPU/密钥） | ★★ 复现环境 |
| `ai_ran_benchmarks` | 132 K | **空模板仓库**（只有 GitLab 默认 README） | — |
| `use_case_studies` | 132 K | **空模板仓库** | — |

★ 注意 quickstart 说明：**目录名是契约的一部分**——平台仓库必须 clone 成 `ocudu/`，
容器构建与跨仓文档链接都依赖这个约定。

---

## 1. ★★★★ 深度 3 的**正规契约**：`ocudu/dapp/use_cases/v1/receiver.h`

这是本次调研**最重要的发现**，而且它逐项印证了我们的设计。

### 1.1 输入（`ocudu_dapp_receiver_input_v1`，`include/ocudu/dapp/use_cases/v1/receiver.h:56-61`）

```c
typedef struct {
  ocudu_dapp_invocation_v1     invocation;
  ocudu_dapp_tensor_view_v1    rx_grid;
  ocudu_dapp_typed_metadata_v1 pusch;
  ocudu_dapp_typed_metadata_v1 dmrs;
} ocudu_dapp_receiver_input_v1;
```

★★★ **就是"网格 + 几何"**：`rx_grid` + `pusch` 元数据 + **`dmrs` 元数据**。
与用户描述的形态、与 NVIDIA `neural_rx.onnx` 的输入、与 dApp 论文的 Class A 描述**三方一致**。

### 1.2 ★★★★ 输出（`:62-79`）——逐字，以及对我们的四条硬约束

```c
typedef struct {
  /// Caller-owned writable soft-bit tensor before scrambling reversal.
  ///
  /// Host invocations use I8 [data_re * modulation_order]. Accelerator-resident invocations use F16
  /// [data_re, layer, bit] with OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1. In both cases, bit is
  /// the fastest-varying dimension, positive values favor bit zero, negative values favor bit one, and magnitude
  /// expresses reliability. Values must be finite (and I8 must not use -128). The host retains ownership of storage
  /// and completion signaling.
  ocudu_dapp_tensor_view_v1 llrs;
  ocudu_dapp_typed_metadata_v1 uci;          // v1 必须全零
  ocudu_dapp_typed_metadata_v1 metrics;      // 可选：后均衡噪声等
  uint32_t confidence_q16;                   // ★ 可选置信度 [0,1]（v1 主机不用它 gate 译码）
  uint32_t reserved0;                        // 输出 flags
} ocudu_dapp_receiver_output_v1;
```

★★★★ **四条与我们完全一致 / 必须遵守的约定**：

| # | 契约 | 我们的状态 |
|---|---|---|
| 1 | ★ **"before scrambling reversal"**（解扰**前**的软比特） | ★ **与我们 `memo_01` §3 冻结的"模型输出加扰域 LLR、解扰保持经典"逐字一致** |
| 2 | ★ **"bit is the fastest-varying dimension"** | ★ 与我们的 `llrs[re*Qm + b]` 一致；也与 NVIDIA 的 `[re][bit]` 一致 |
| 3 | ★★ **"positive values favor bit zero, negative values favor bit one"** | ★★ **正是我们仓库的约定**（`log_likelihood_ratio::to_hard_bit()` 返回 `value <= 0` ⇒ 正 LLR = 比特 0）。三个平台同一个符号约定 |
| 4 | ★★ **两种 LLR 表示**：**host → `I8 [data_re × Qm]`**；**accelerator-resident → `F16 [data_re, layer, bit]`** | ★ 我们的链是 **int8**（`LLR_MAX=120`）⇒ **与 host 契约同族**；但我们要走的是 **Class A 加速器驻留**，其契约是 **F16** ⇒ **这是一个必须显式决策的接口点**（见 §5） |

★ 补充约束：**值必须有限**；**I8 不得使用 −128**。
（对照我们：`±127` 保留为"确定比特"、`LLR_MAX=120` ⇒ 我们比它更严。）

### 1.3 ★★ `metrics` 可选输出：正是我们"上报义务"问题的**官方解法**

`receiver_metrics_v1`（`:21-54`）：
- `post_equalization_noise`：**F32 `[data_re, layer]`**；填满则置 `OCUDU_DAPP_RECEIVER_METRICS_VALID_V1`。
  ★ 逐字：*"**Leaving that bit clear is supported and means the receiver produced LLRs without
  scheduler-facing post-EQ noise.**"*
- `equalized_symbols`：**CF32 `[data_re, layer]`**，可选。★ 逐字：
  *"A receiver may leave this unfilled even when requested (**for example, direct-bit neural receivers
  without an explicit symbol estimate**)."* —— ★★ **官方明确预期"不产出符号的神经接收机"**，就是我们。

★★ **这印证并细化了 `memo_01` §1.5 的判断**：深度 3 **继承 CE 的上报义务**，
而平台的解法是**把这些量做成"可选 + 有效位标记"的输出张量**，而不是强制。
⇒ 我们的规划路线 (a)（net 附带输出信道估计与噪声）**与该契约同形**，可直接对齐。

### 1.3bis ★★★★ 张量级输入契约（SDK 侧准入检查，`class_a_cuda_contract.cuh:120-180`）

★★ **这就是"网格 + 几何"的逐字段形态**，也是用户描述的"哪些是 DMRS RE、哪些是 data RE、
它们之间的相对几何关系"的**官方编码方式**：

| 张量 | 元素类型 | rank | 布局 | 含义 |
|---|---|---|---|---|
| `input.rx_grid` | ★ **`CBF16`（复数 bf16，4 B 打包对）** | 3 | **`[port, symbol, subcarrier]`** | 接收网格 |
| `dmrs->reference_symbols` | **CF32** | 2 | **`[layer, pilot]`** | ★ **已知的 DM-RS 符号值**（即 DeepRx 用的"已知星座点"） |
| ★ `dmrs->coordinates` | **U16** | 2 | **`[pilot, 2]` = (symbol, subcarrier)** | ★★ **每个导频的时频坐标——这就是"几何"** |
| ★ `pusch->data_re_indices` | **U32** | 1 | `..._PUSCH_DATA_RE_INDICES_U32_V1` | ★★ **要出 LLR 的 data RE 索引** |

**准入包络**（同处 `:123-133`）：1–2 层、1–64 端口、DM-RS type 1/2、`nof_pilots ≤ 4096`、
data-RE 选择必须完整、必需 flags `LAYER_COMPLETE | COORDINATES_SHARED_ACROSS_LAYERS`。

★ 三个独立来源在此**完全收敛**：dApp 论文的 *"explicit DM-RS pilot tensor with bounded coordinates,
compact ordered data-RE indices"*、本契约的 `coordinates` + `data_re_indices`、
以及 NVIDIA `neural_rx.onnx` 的 `dmrs_ofdm_pos` + `dmrs_subcarrier_pos`。
⇒ **"网格 + 导频坐标 + 数据 RE 索引"是跨平台的共同输入形态**，我们照此冻结即可。

★ 另注意 `rx_grid` 的元素类型是 **CBF16**——与我们链上 `cbf16_t` 的信道估计**同精度**。

### 1.4 C ABI（`:81-88`）

```c
typedef ocudu_dapp_status_v1 (*ocudu_dapp_receiver_invoke_v1)(void*,
                                                              const ocudu_dapp_receiver_input_v1*,
                                                              ocudu_dapp_receiver_output_v1*);
typedef struct {
  ocudu_dapp_worker_api_v1      worker;
  ocudu_dapp_receiver_invoke_v1 invoke;
  uint64_t                      reserved[8];
} ocudu_dapp_receiver_interface_v1;
```

---

### 1.5 ★★ 输出侧的其余字段（SDK 侧核对）

- `output.llrs`：**F16, rank 3, `[data_re, layer, bit]`**，`ACCESS_WRITE`，
  布局 `OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1`（`algorithm.cu:246-250`）。
- `output.metrics.post_equalization_noise`：**F32 rank 2**，布局 `..._POST_EQ_NOISE_DATA_RE_LAYER_V1`。
- `output.metrics.equalized_symbols`：可选 **CF32 rank 2**，`device_ordinal` 必须与 llrs 相同，且**做了不相交检查**。
- `output.reserved0`：承载 `..._METRICS_VALID_V1` / `..._EQUALIZED_SYMBOLS_VALID_V1`。
- `output.confidence_q16`：**存在，但参考实现根本不写它**（`abi_layout_v1.lp64.txt:125` 有偏移 360）。

---

## 2. ★★★★ 接缝在平台里的实现：`pusch_dapp_demodulator.h`

`include/ocudu/phy/upper/dapp/pusch_dapp_demodulator.h`（131 行）——

### 2.1 两个深度 = 两个 mode

```c
enum class pusch_dapp_demodulator_mode : uint8_t { inactive, equalizer, receiver, conflict };
```
★★ 即 **`equalizer`（CE+EQ，深度 2）** 与 **`receiver`（CE+EQ+解映射，深度 3）**，
`conflict` 表示两者同时激活（互斥保护）。

### 2.2 两个 invoke 重载 + 一个**延迟**契约

```c
virtual dapp::native_invocation_result invoke(uint32_t lane,
    const ocudu_dapp_equalizer_input_v1&, ocudu_dapp_equalizer_output_v1&) noexcept = 0;  // 深度 2
virtual dapp::native_invocation_result invoke(uint32_t lane,
    const ocudu_dapp_receiver_input_v1&, ocudu_dapp_receiver_output_v1&) noexcept = 0;   // ★ 深度 3
```
★★ 以及 **`invoke_receiver_deferred(lane, in, out, ticket, worker)`**，注释逐字：
> *"Completion-aware dispatch is a **separate contract**: never silently call ordinary invoke().
> Before dispatch rejection leaves ticket/owner untouched. A possibly submitted operation must **retain
> its original worker and ticket, and must forbid fallback**. Caller retains tensor/metadata storage
> independently."*

⇒ ★ 这是**异步/完成感知**的路径，与我们融合 lane 的 `submit_fused` + `wait()` 结构同形，
但**契约更严**：提交过的操作**禁止回退**，且必须持有 worker 与 ticket。

### 2.3 ★★ 真实的死线数值与**它们的实测理由**

```c
std::chrono::nanoseconds demodulator_deadline{std::chrono::microseconds(2000)};
std::chrono::nanoseconds channel_estimator_deadline{std::chrono::microseconds(800)};
float                    noise_variance_floor{1.0e-6F};
```
★ `demodulator_deadline` 的注释逐字（**这是别人踩过的坑**）：
> *"Soft wall for one Class-A equalizer/receiver invoke (enqueue through GPU completion).
> **Not a 3GPP slot deadline.** **500 µs (one 30 kHz slot) and then 1000 µs both still classified good
> PBC results as late under 4-UE OTA contention; 2000 µs keeps the tripwire without disabling a
> working EQ.** Tests may inject a tighter budget."*

★★ **对我们的 G5 极其重要**：他们**先试过 500 µs、再试 1000 µs，在 4-UE 空口争用下都太紧**，
最后用 2000 µs。⇒ 我们规划里"目标 ≤ 500 µs"的那条**需要一个明确的争用条件**，
否则会重演"死线定得太紧，把能工作的实现判成失败"。

### 2.4 ★★ 逐 UE 路由 —— `phy_routing_policy` 的生产实现

```c
virtual dapp::pusch_route resolve_route(uint32_t cell_index, uint16_t rnti) const noexcept;
virtual pusch_dapp_demodulator_target& route(const dapp::pusch_route&) noexcept;
```
> *"Per-UE routing (see `dapp::pusch_route_table`): the route of one grant. A target without a route
> table always answers the primary route. … **A routed UE sees only its instance's hook; a route to the
> channel estimator or to the stock receiver is an inactive target.**"*

★★★ **这正是 `metal_vs_cuda_architecture.md` §6.3 提出的"路由策略是独立注入的决策对象"**
——而且它已经**在生产里**，粒度是 **per-UE**。我们的 `phy_routing_policy` 应当与它对齐。

### 2.5 回退与上报 API

```c
virtual void report_invalid_output(...) noexcept;
virtual void report_completion_deadline(..., uint64_t, uint64_t) noexcept;
virtual void report_fallback(uint32_t, pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason) noexcept;
```
★ 以及工厂签名把**经典实现作为 `fallback` 注入**：
```c
create_pusch_dapp_demodulator_factory(std::shared_ptr<pusch_demodulator_factory> fallback,
                                      std::shared_ptr<pusch_dapp_demodulator_target> target, ...);
```
⇒ 与我们的"谓词返回 false ⇒ 经典路径"同构，但他们是在**构造期注入**回退实现。

### 2.6 ★ 后端无关：**CPU 适配器存在**

```c
/// Creates an initial one-layer CPU Class-A CE+EQ/receiver adapter with fixed state per demodulator object.
std::shared_ptr<pusch_demodulator_factory> create_pusch_dapp_demodulator_factory(...);
```
★ **CPU 适配器与 CUDA 参考并存** ⇒ 这个接缝**在设计上就是后端中立的**。
这是**可移植性的最强信号**：契约不假设 CUDA。

### 2.7 注入点：`upper_phy_dapp_hook_provider`

`include/ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h`：
> *"Optional **construction-time integration** between upper-PHY factories and the runtime dApp hook registry."*

方法返回 `dmrs_pusch_estimator_factory` / **`pusch_demodulator_factory`** / `srs_estimator_factory` / `wrap_phy_tap`。
⇒ ★ 这是"把 hook 注入工厂"的地方，**与我们的 `upper_phy_factories.cpp` 后端选择同构**
（`pusch_channel_equalizer_backend == "metal"` 那条路径）。

---

## 2bis. ★★★ 完成/fence 语义（SDK 侧代码级，我们最该抄的部分）

**两道不同的 fence**：

**(a) lane 内的模块 fence**（`reference/common/module_support.cpp:342-354`）——
`invoke` **即使算法返回失败或抛异常，也仍然** `cudaEventRecord(lane->completion[slot], execution_stream)`。
理由逐字：

> *"A failed or throwing implementation may **already have enqueued work**. Fence every admitted call so
> model retirement cannot recycle its device state while such work still references it."*

★ 记录失败会**毒化该 slot**，并把 OK 结果降级为 `INTERNAL_ERROR`。
**迟到检测**用 `cudaEventQuery`：`cudaErrorNotReady` ⇒ 暂不可复用；**其它任何错误 ⇒ 永久毒化**。
WARM/RETIRE 在有未完成调用时返回 `CONFLICT`。

**(b) 面向上主机的 completion 扩展**（`docs/receiver_completion.md`）——
独立的导出查询；`host_supports_receiver_completion` 要求 `struct_size`/`abi_major`/能力位匹配；
invoke 发布一个**非零不透明 receipt**：

> *"A partial submission is **DEVICE_ERROR**, never BYPASS/RESOURCE/INVALID_INPUT/UNSUPPORTED"*；
> *"Inspect is nonblocking and non-acknowledging. Only OK/ready/valid authorizes… **do not use learned
> confidence as proof of execution or as a physical SINR/noise measurement**"*。

★★ **生产主机目前还没有宣告支持 completion** ⇒ 查询被拒是预期行为。

★★ **迟到 ≠ 回退**：主机只把 module+continuation 包在计时事件里，增加
`class_a.completion.deadline_misses` 与一条 E3 incident（带 `observedLatencyNs`/`deadlineNs`），
**不会**增加 `class_a.fallback.deadline`。**连续 8 次 miss 才 latch**。
⇒ 与 `known_limitations.md` 的 *"deadlines are **advisory** … the breaker does not open on
deadline misses alone"* 一致，也**修正了 dApp 论文给人的"8 次迟到打开 breaker"的印象**。

⚠ **一处数值不一致待核实**：SDK 文档写 CE 预算 **~300 µs**（`validation_ladder.md:49`），
而平台头文件 `pusch_dapp_demodulator.h` 里 `channel_estimator_deadline` 默认 **800 µs**。
两者可能是"文档示例 vs 平台默认"，但**引用前必须核实**。

---

## 2ter. ★★★ 模型生命周期：逐字段的代码机制（最可复用的一段）

ABI：`management->manage_model(context, const ocudu_dapp_model_request_v1*, uint64_t* generation)`；
请求 = `{struct_size, action, model_id, model_version, expected_generation, artifact(blob view), dry_run}`。

★★ **generation 是乐观并发令牌**（`reference/common/module_support.cpp:179-181`）：

```c
if (request->expected_generation != state.model_generation) return OCUDU_DAPP_CONFLICT_V1;
// 成功且非 dry-run 时：
*generation = ++state.model_generation;
```

动作：`STAGE → VALIDATE → WARM → ACTIVATE → ROLLBACK | RETIRE`，每个动作由 `phase` 门控
（STAGE→1、VALIDATE→2、WARM→3）。ACTIVATE 把 staged→active、active→previous；
ROLLBACK 交换 active↔previous；**RETIRE 拒绝退休当前 active 的模型**。

★★★ **发布的是"准备好的设备状态"，不是权重 blob**（`module_support.cpp:23-30,202-241`）：
- 实例持有 `std::array<receiver_cuda_state,2> slots` + **`std::atomic<uint32_t> active_slot`**；
- **WARM 在调用委托之前**就解码+校验产物（magic/version/count、有限且两两不同的电平），
  并用 `cudaMalloc`+H2D 建好 `receiver_cuda_state`；
- ACTIVATE 用 **`active_slot.exchange(next, acq_rel)`** 发布；worker 每次调用读取。

⇒ ★ 这就是 dApp 论文说的"两个预分配权重 bank + 单次原子交换"的**代码本体**，
也是我们 CoreML 引擎需要补的"模型代际 + 无热路分配切换"。

⚠ **权重体积限制**：模型 blob 被拷进 `std::vector<uint8_t>` 且**上限 1 MiB**，
参考实现的 `decode_model` 硬性要求 **264 字节**。文档明确邀请替换
（*"a commercial receiver should replace the artifact definition, validation, and prepared device state
with its own bounded weights"*）。★ 我们的 1.45e5 参数 fp16 ≈ 290 KB，**在 1 MiB 之内**；更大的模型就会撞墙。

---

## 2quater. ★★★★ 调用链已在代码里确认：**接收机接缝是真的**（平台侧）

**结论先行**：平台**不只是**有信道估计或均衡钩子——**它有一个完整的"接收机"接缝**。

### 调用点（`lib/phy/upper/channel_processors/pusch/pusch_demodulator_gpu_impl.cpp:3049-3075`）

```cpp
} else if (resident_mode == pusch_resident_dapp_mode::receiver) {
  const auto invoked = resident_dapp_->invoke_receiver(buffers, config);        // :3050
  ...
  status = scrambler_descramble_llr_half_inplace(                                // :3061
      scr_handle_, d_llrs_half_[buf_idx], static_cast<int>(num_llrs), stream_);
  dapp_inline_used = status == NR_LDPC_SUCCESS;                                  // :3064
```

★★★ **这条链把 `memo_01` §3 的分析逐字证实了**：模型交出**解扰前**的 FP16 LLR ⇒
**主机在同一个 stream 上就地解扰** ⇒ 之后才进 LDPC。
模式互斥：`{inactive, channel_estimator, equalizer, receiver, conflict}`
（`include/ocudu/phy/upper/dapp/pusch_resident_dapp.h:21`；`equalizer` 分支 `:3035`、`channel_estimator` `:2982`）。

### LLR 去向（★ 主机侧完全绕开常规路径）

```
就地解扰(:3061) → last_resident_llrs_ (:3527) → get_resident_softbits() (pusch_demodulator_gpu_impl.h:113)
→ pusch_processor_impl.cpp:223 → pusch_decoder_impl.cpp:839
   gpu_decoder->decode_resident_softbits(..., nullptr,  // ★ "No scrambling needed - already descrambled by E2E kernel"
```
常规路径被跳过：`if (!dapp_inline_used && ...)`（`:3141,:3153,:3156`）。

### 缓冲区结构体（`pusch_resident_dapp.h:25-42`）

`grid_cbf16`、`data_re_indices`、`equalized_cf32`、`post_eq_noise_f32`、`llrs_f16`、
网格维度、**`execution_stream`**、`codeword`、`consumer_streams[3]`。
签名：`invoke_receiver(const pusch_resident_dapp_buffers&, const pusch_demodulator::configuration&)`。

### 与论文的**五处偏差**（代码为准）

| 论文说 | 代码事实 |
|---|---|
| 网格 complex BF16 ✓ | `OCUDU_DAPP_ELEMENT_CBF16_V1`，rank 3，`..._RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1`，**整个 slot** `{ports, symbols, subcarriers}` ✓ |
| DM-RS pilot tensor | ★ DM-RS **参考符号是 CF32**（不是 BF16），形状 `{nof_tx_layers, nof_pilots}`；另有 `coordinates` U16 `{nof_pilots,2}`，以及 **`reserved[0]` 里的 OFDM 符号掩码** |
| 模块持有网格 | ★ 模块拿到的是**借来的 lease**（`abi/v1/memory.h:76-90`），**不是所有权** |
| — | ★ ABI 另有**host 版** LLR 输出 `I8 [data_re × Qm]`；只有 accelerator 版是 F16 rank-3 |
| — | ★ bit 最快变化已由 `view.byte_strides[2] == sizeof(uint16_t)` **强制** |

### ★★ breaker 的真实语义（修正论文的印象）

`lib/dapp/runtime/circuit_breaker.h:14-19`：`breaker_policy{ recoverable_failure_threshold{8} }`，
**第 8 次**连续可恢复失败时打开（`circuit_breaker.cpp:23`）。
- ★ **迟到**确实会喂它：`native_worker.cpp:932` 在 `report_completion_deadline` 里
  `breaker.observe(OCUDU_DAPP_DEADLINE_V1)` ⇒ 论文的"连续 8 次迟到"**方向正确**；
- ★ **但计数器是共享的**：invalid-input / invalid-output / resource / deadline 都递增**同一个** latch
  ⇒ **任意 8 次混合失败**都会打开，不是"8 次迟到"；
- **按 lane 独立**（每个 slot 有自己的 worker + breaker）；
- **恢复只能靠运维探针**（`begin_authorized_probe()` → half_open → `close_after_successful_probe()`）；
- 打开期间 `guarded_call.h:29` 返回 `OCUDU_DAPP_BYPASS_V1`。

### ★★★ 两个决定性的**负面结论**（照我们的规划）

1. ★★ **没有 shadow / 对比模式，也没有基于 LLR 置信度的门控。**
   回退**严格只在失败或 profile 被拒时**发生。
   ⇒ ★ **这修正了我们的规划 §1.4**：我此前根据 calibration-drift 论文写了"改为逐时隙神经+经典**并行仲裁**"——
   **平台并不是这么做的**。准确表述应是：
   (a) 文献（arXiv 2605.26157）**建议**并行仲裁；
   (b) 平台**只实现"失败即回退"**；
   (c) **shadow 模式要我们自己建**。
2. ★★ **`OCUDU_DAPP_HOST_CAP_RECEIVER_COMPLETION_V1` 在生产里从不宣告**——
   `apps/services/dapp/dapp_service.h:190 host_capabilities{}` **从未被填充**，唯一的 setter 是**测试夹具**
   （`tests/integrationtests/.../pusch_test_dapp_fixture.h:42`）。
   `module_loader.cpp:157-160` 因此**拒绝需要 completion 的接收机**。
   ⇒ **生产路径是 invoke-only**；那套延迟/completion 契约**目前只存在于测试里**。

---

## 3. ★★ SDK：官方把"可替换的算法核心"直接点名到我们的用例

`ocudu-dapp-sdk/docs/extension_model.md` 逐字：

> *"They are deliberately split into a **stable integration shell and a replaceable algorithm core** so
> another team can improve performance **without recreating the dangerous parts of the host boundary**."*

**可替换的算法扩展点**（原文表格）：

| 参考 | 主要扩展点 |
|---|---|
| Channel estimator | pilot processing, interpolation, denoising, **learned estimation**, signal/noise estimation |
| Equalizer | channel use, MMSE/IRC/MIMO detection, residual-noise and SINR estimation |
| ★★ **Receiver** | ★★ **joint CE/EQ/demapping, learned receiver stages, model artifacts and persistent GPU state** |
| Scheduler / Spectrum / SRS-ISAC | … |

★★★ **"Receiver → joint CE/EQ/demapping, learned receiver stages" 就是我们的深度 3**，
而且是**官方支持的扩展点**，带参考包。

### 3.1 三个深度在 SDK 里的**必需输出**

`docs/class_a_cuda_authoring.md` 原文表格：

| 起始产物 | 替换 | **必需输出** |
|---|---|---|
| `ref_channel_estimator_cuda` | 信道估计 | `[port, layer, symbol, subcarrier]` CF32 信道 + `[port, layer]` F32 噪声 |
| `ref_equalizer_cuda` | CE + 均衡 | `[data_re, layer]` CF32 符号 + **F32 后均衡噪声（必需）** |
| ★ `ref_receiver_cuda` | ★ **CE + 均衡 + 解映射** | ★ **`[data_re, layer, bit]` FP16 解扰前 LLR** |

★ 而且有**脚手架生成器**：
```bash
python3 tools/scaffold_class_a_cuda.py --kind receiver --target my_receiver_cuda \
  --global-name com.example.ran.my-receiver.cuda --package-id <UUID> --vendor "..." --version v1 \
  --output ../my-receiver-dapp
```
⇒ 会生成带 manifest-v1、SPDX SBOM、哈希绑定、独立 ABI 的完整工程。

### 3.2 ★ 模型生命周期（现成的，我们要的）

```bash
ocudu-dappctl model stage --instance INSTANCE --model receiver-levels \
  --version 1 --model-generation 1 --artifact receiver-model.bin
# 然后 validate → warm → activate，用返回的 model generation
```
⇒ **stage / validate / warm / activate / rollback + generation 检查**，与 dApp 论文描述一致。
★ 参考实现还带一个模型产物生成器（`make_receiver_cuda_model`），便于在不复现私有权重布局的情况下演练状态管理。

### 3.3 ★ 调用契约（逐字要点，对我们的 Metal/CoreML 实现直接适用）

- *"enqueue only on `invocation.execution_stream`; **never synchronize that stream**"*；
- *"allocate and upload model state in **lifecycle** or model-management operations, **not in invocation**"*；
- *"keep mutable scratch **per admitted worker/lane**, since concurrent PUSCH lanes may invoke the same
  module instance"*；
- *"keep the ABI callback `noexcept` … `std::bad_alloc` → `RESOURCE`，其余异常 → `EXCEPTION`"*；
- ★ *"if persistent device/model state can be referenced by queued work, **record its lane-local
  completion fence even when the implementation returns failure or throws**, since it may have enqueued
  work before failing"*；
- ★ *"**do not retain borrowed tensor pointers beyond the completion event**"*；
- *"write only caller-owned outputs and return an explicit unsupported/error status **before launching**
  when the requested profile cannot be handled"*。

★★ 这七条**几乎逐条对应我们融合 lane 已经付过学费的教训**
（跨 TG 依赖要用 MTLEvent/命令缓冲顺序表达、fence 归属、借来的指针不能越界存活）。
⇒ **建议直接采纳为我们的 AI 后端实现规约。**

### 3.4 参考实现的文件切分（"壳 vs 核心"）

`reference/receiver_cuda/` 只有 4 个文件：
`module.cpp`（query trampoline）、`module_support.cpp`（生命周期/ABI）、`reference_api.cpp`、
★ **`algorithm.cu`（要替换的算法核心）**。
⇒ **要改的就是 `algorithm.cu`**，其余保持。

---

## 4. ★★ `known_limitations.md`：官方的诚实清单（挑出与我们直接相关的）

| 条目 | 对我们的意义 |
|---|---|
| ★★ *"On the resident Class-A inline path the **conventional CSI measurement kernels are skipped**: `pusch.symbols` telemetry and the E3 diagnostics carry **post-EQ SINR only, with EPRE / RSRP / CFO / TA absent** for those grants. **Planned fix**: the Class-A CE/EQ output contract gains **reserved-slot fields so the dApp reports its own DMRS-derived measurements**."* | ★★★ **这正是 `memo_01` §1.5 的"上报义务"，而且是一个生产平台承认的现有缺口**。我们的路线 (a) 与它计划的修法**同形** |
| ★ *"Third-party module deadlines are **advisory**: the boundary is measured and recorded, but an invocation is **not preempted** and the **breaker does not open on deadline misses alone**."* | 修正 dApp 论文给人的印象（"连续 8 次迟到打开 lane breaker"）：**光靠死线超时不触发 breaker** |
| *"Host inline Class-C copies the whole resource grid on the UL-PHY thread (~734 KB, 50-100 µs/slot/cell)"* | 网格搬运代价的实测锚点 |
| *"E3AP over SCTP has **no TLS or token authentication** in this release; the gNB binds loopback by default"* | 若将来走 E3 需外部边界 |
| *"**Schema drift** in the reference client tarball: ships a trimmed `class-c-publication-v1.fbs` with **different enumerator numbering** than the normative schema … or **enumerator values will silently disagree**"* | ★ 一个"静默不一致"的真实案例——值得引以为戒 |
| *"The Class C process boundary provides **fault containment, not a hostile multi-tenant sandbox**"* | 安全边界定位 |
| ★ *"Optional advisory confidence in Q16 [0,1] … **the v1 host does not gate decoding on it**"*（`receiver.h:76`） | ★ 平台**提供了置信度通道但不用于 gate** ⇒ 我们的**信任/回滚（规划 §1.4）目前没有平台级支撑**，得自己做 |

---

## 4bis. ★★★★ 两条**无法绕开**的硬约束（直接改我们的架构）

### (1) ★★★ 网络**必须**输出逐 RE 的噪声/不确定度，不能只出 LLR

主机**只从我们的逐 RE 后均衡噪声**推导调度器 SINR：

```
SINR_dB = -10 · log10( mean(σ²_post) )
```
（`class_a_cuda_authoring.md:285-295`），而**常规 CSI 核在这条路径上已经被跳过**
（`known_limitations.md:33-38`）。

⇒ ★★ **我在规划 §2.3 里设计的"辅助头 1：后均衡噪声方差"不是可选优化，而是硬需求。**
只出 LLR 的神经接收机会让**上行链路自适应失去输入**。
★ 并且文档给出一个具体的坑（逐字）：*"Do not leave a constant near-zero floor that would
**invent huge SINR**"* —— 常数近零底噪会被算成极大 SINR，直接把调度器带偏。

⇒ **规划据此修改**：把"后均衡噪声头"从"辅助头"提升为**与 LLR 头并列的必需输出**，
并把"噪声头在低噪声区不得输出近零常数"写成 G3 的一个**数值判据**。

### (2) 模型产物体积被限制在 **1 MiB** 且**带内拷贝**

模型 blob 会被拷进 `std::vector<uint8_t>` 且**上限 1 MiB**（`reference/common/module_support.cpp:173,220`）。
⇒ 我们的 1.45e5 参数 fp16 ≈ **290 KB，在限内**；但更大的模型必须**抬高上限或改用带外资源**。
★ 这是一条**现在就要记下、否则会在 P4 撞墙**的约束。

### (3) 后端枚举在本树里是**封闭的**

全树只有 `OCUDU_DAPP_BACKEND_CUDA_V1` 与 `..._HOST_V1`（`class_a_cuda_contract.cuh:48`、
`class_c_algorithms.h:21`）。⇒ ★ **新增一个 Metal 后端 id 需要同时改 OCUDU 主机侧头文件**，
不是"只写一个模块"就能接上的。这是**采用平台 ABI 路线的真实成本**。

---

## 4ter. ★★★ 后端可移植性：ABI 是中立的，**运行时不是**

### (1) ★★ ABI 早已枚举了非 CUDA 加速器

`include/ocudu/dapp/abi/v1/memory.h:36-43` **显式列出 `HOST / CUDA / HIP / SYCL`**，
并自述为 backend-neutral。⇒ ★ **dApp ABI 在设计上就是后端中立的**——
Metal 不是"逆着契约来"，而是"**第五个 id**"。

### (2) ★★★ 但有两处**结构性阻塞**（不是接线问题）

| # | 阻塞 | 后果 |
|---|---|---|
| 1 | `include/ocudu/dapp/management/types.h:29`：`enum class backend : uint8_t { cpu, cuda, other_accelerator };`，**但** `lib/dapp/runtime/instance_manager_helpers.inc:42-43` 把 `other_accelerator → std::nullopt`；`instance_manager_lifecycle.cpp:66` 在 `!abi_backend(selected_backend)` 时**拒绝 in-process 放置** | ★ Class A **只能 in-process** ⇒ **Metal 后端的 Class A 包当前无法加载** |
| 2 | `class_a_l1_path_incompatibility`（`instance_manager_helpers.inc:284-293`）把"加速 L1"与"CUDA"**当成同义词**（`selected == cpu && cuda_l1` 直接拒绝） | 同上 |

⇒ ★ **采用平台 ABI 的真实成本**：不是"写一个模块"，而是**改运行时的后端枚举与放置校验**。

### (3) 可原样复用 vs 必须重写

| | 文件 |
|---|---|
| ✅ **原样复用** | **全部** `include/ocudu/dapp/abi/v1/*.h` 与 `use_cases/v1/*.h`（**纯 C**）；`circuit_breaker.cpp`、`incident*.cpp`、`guarded_call.h`、`instance_runtime.h`；`module_loader.cpp`、`abi_validation.cpp`、`native_worker.cpp`、`native_hook_point.cpp`、`native_hook_slot.cpp`、`native_instance.cpp`、`instance_manager*.cpp`、`package_*.cpp`、`artifact_verifier.cpp`、`lifecycle.cpp`；`pusch_dapp_demodulator.cpp`、`native_pusch_dapp_demodulator_target.cpp`、`pusch_route_table.cpp`、`upper_phy_dapp_*.cpp` |
| ❌ **必须重写** | `lib/phy/upper/dapp/pusch_resident_dapp.cpp`（**1401 行热适配器**：stream 转 `cudaStream_t`、`cudaMemcpyAsync`、`cudaEventRecord`，整体在 `#ifdef ENABLE_CUDA` 下）；`native_receiver_resident_adapter.cpp:59-67`（硬编码 `MEM_ACCELERATOR_DEVICE_V1` + `BACKEND_CUDA_V1`）；`pusch_resident_dapp_cuda.cu/.h`、`srs_*_cuda.cu`、`class_c_cuda_export_pool.cpp`、`native_cuda_control_context.cpp`；`pusch_demodulator_gpu_impl.cpp`（**4058 行**） |

★ 构建门：`CMakeLists.txt:95 option(ENABLE_CUDA ... OFF)`、`lib/phy/upper/dapp/CMakeLists.txt:26-36`。

---

## 5. 可以重用 / 需要修改

### 5.1 ✅ 可以直接重用（契约与工程纪律）

| 项 | 说明 |
|---|---|
| ★★ **深度 3 的输入/输出契约** | `rx_grid + pusch + dmrs` → **解扰前** LLR；**bit 最快变化**；**正 = 比特 0**；值有限；I8 不用 −128。**与我们的设计逐项一致** |
| ★★ **"stable shell / replaceable core"的切分** | 壳（ABI/生命周期/校验/fence/回退/打包）不动，只换 `algorithm.cu` 对应的算法核心 |
| ★★ **模型生命周期**（stage/validate/warm/activate/rollback + generation） | 直接照搬概念；我们的 CoreML 引擎需要补"模型代际 + 原子切换" |
| ★★ **调用契约七条**（§3.3） | 建议直接采纳为后端实现规约 |
| ★ **`metrics` 可选 + 有效位的做法** | 解决"上报义务"的官方形态 |
| ★ **逐 UE 路由（`pusch_route_table`）** | 与我们的 `phy_routing_policy` 对齐 |
| ★ **回退在构造期注入**（`fallback` 工厂参数） | 比"运行期判断"更干净 |
| ★ **验证阶梯 / ABI 编译器矩阵**（GCC/Clang × C11/C++17，LP64 布局指纹） | 我们没有这一层；对"将来换平台"有直接价值 |
| ★ **`e3agent-standalone` 式"无依赖测试台 + 漂移检查"**（`memo_05` §2bis.4） | 同组的 quickstart 也体现这个思路 |

### 5.2 ⚠️ 需要修改 / 我们自己要补的

| 项 | 为什么 | 我们怎么做 |
|---|---|---|
| ★★ **LLR 表示** | 契约规定 **host = I8**、**accelerator-resident = F16**；**我们的链是 int8（`LLR_MAX=120`）** | 必须显式决策：① 保持 int8 并说明我们走的是 host 同族表示；② 采用 F16 并加一级转换。**这是一个要在 P4 之前定下来的接口点** |
| ★ **引擎后端** | SDK 参考是 CUDA；契约**在 CPU 上也已实现**（`create_..._factory` 的 CPU 适配器） | ★ **这给我们信心**：契约不假设 CUDA。我们做的是**第三个后端**（Metal/CoreML），而不是"逆着契约来" |
| **`invocation.execution_stream`** | 概念是 CUDA stream | 映射到 Metal command queue / CoreML 引擎句柄。★ 注意 `memo_03` §1.5：**自定义 Metal kernel 不能进 ANE 常驻图** |
| ★ **模型产物 1 MiB 上限 + 带内拷贝** | 硬约束（§4bis.2） | 现模型 290 KB 放得下；更大的必须抬高上限或走带外 |
| ★★ **逐 RE 后均衡噪声是必需输出** | 硬约束（§4bis.1）：主机只从这里推 SINR | 噪声头从"辅助"升为"必需"，并加"不得近零常数"的数值判据 |
| ★ **后端枚举封闭** | 新增 Metal backend id 要改主机头（§4bis.3） | 决定"采用平台 ABI"时把这项成本算进去 |
| **"never synchronize that stream"** | 与 ANE 的同步模型不同（CoreML 的 predict 是阻塞调用） | ★ 这是我们**最需要设计的**一处：要么用专用 worker 线程把 predict 变成异步（AI CE 已有这个模式），要么显式声明一个新的同步点 |
| **`confidence_q16` 不被平台用于 gate** | 平台的信任/回滚没有用上它 | 我们的 §1.4 逐槽信任/回滚**要自己做**，并如实报告它与平台的关系 |
| **死线默认值** | 2000 µs（实测 500/1000 太紧） | ★ 我们的 G5 判据**必须写明争用条件**，不能只写一个 µs 数 |

### 5.3 ❌ 不需要 / 不该抄的

- `ai_ran_benchmarks`、`use_case_studies` —— **空模板**。
- CUDA 专属的 `cuphy::tensor_desc`/stream capture/CUDA graph 手术（`memo_05` §4.3）。
- 不要为了"与平台一致"而接受 **copy-through** 的推理路径（`memo_05` §2.5）——
  契约**不要求**拷贝，它要求的是"caller-owned buffer + inherited stream + 无分配无同步"，
  **这三条在 UMA 上零拷贝即可满足**。

---

## 6. 对规划的直接影响（待并入）

| # | 变更 | 依据 |
|---|---|---|
| 1 | ★★ **接口契约以 `receiver.h` 为准冻结**：输入 `rx_grid+pusch+dmrs`；输出**解扰前** LLR、bit 最快变化、**正 = 比特 0**、值有限、I8 不用 −128 | §1.1–1.2 |
| 2 | ★★ **LLR 位宽成为显式决策点**（host I8 vs accelerator F16），需在 P4 前定 | §1.2、§5.2 |
| 3 | ★ **"上报义务"的官方解法**：`metrics` 可选张量 + 有效位 ⇒ 采纳（呼应 `memo_01` §1.5） | §1.3、§4 |
| 4 | ★ **后端实现规约采纳 SDK 的调用契约七条**（尤其"失败也要记 fence"、"不越界持有借来的指针"） | §3.3 |
| 5 | ★ **G5 死线必须带争用条件**（他们 500/1000 µs 在 4-UE 空口下太紧，最终 2000 µs） | §2.3 |
| 6 | ★ **信任/回滚要自己做**：平台的 `confidence_q16` 明确"不用于 gate 译码" | §4 |
| 7 | ★ **路由策略对齐 `pusch_route_table` 的 per-UE 粒度** | §2.4 |
| 8 | ★ **验证阶梯 + ABI 稳定性门禁**（编译矩阵 / 布局指纹）值得引入 | §5.1 |
| 9 | ★★ **噪声头是必需输出**（主机 SINR 的唯一来源），且**禁止近零常数底噪** | §4bis.1 |
| 10 | ★ **模型产物体积上限 1 MiB** ⇒ 尺寸预算多了一条外部约束 | §4bis.2 |
| 11 | ★ **模型生命周期采纳"乐观并发 + 预准备状态原子发布"**：`expected_generation` 令牌 + `atomic` slot 交换 + WARM 先建后换 | §2ter |
| 12 | ★ **fence 纪律**：**失败/抛异常也必须记 fence**（可能已入队工作）、**失败毒化 slot**、迟到用事件查询而非等待 | §2bis |
| 13 | ★ **"迟到 ≠ 回退"**：平台把 deadline miss 计入 `completion.deadline_misses` 与 incident，但**不进 fallback 计数**；连续 8 次才 latch | §2bis |
| 14b | ★★ **修正 §1.4**：平台**没有 shadow/并行仲裁**，只有"失败即回退"；★ **shadow 要我们自己建**（文献建议 vs 平台实现的差别必须写清） | §2quater |
| 14c | ★★ **completion 契约在生产里未启用**（能力位从不宣告，加载器拒绝）⇒ 生产是 **invoke-only** | §2quater |
| 14d | ★★ **Metal 后端的两个结构性阻塞**（`other_accelerator → nullopt`、加速 L1 ≡ CUDA）⇒ 采用平台 ABI 需改运行时，不只是写模块 | §4ter |
| 14e | ★ **ABI 已枚举 HOST/CUDA/HIP/SYCL** ⇒ Metal 是"第五个 id"，方向正确 | §4ter |
| 14 | ★ **官方承认深度 3 的对比证据不在开放阶梯内**：*"Vendor-only (not in open ladder): a proprietary neural receiver vs the conventional receiver, side-by-side SER."* ⇒ **这份证据要我们自己做**（正是我们的 G6） | §5.1 |

---

## 7. 未完成 / 待办

- ⏳ 两路并行深挖进行中：①平台侧"模块如何被 PUSCH 链调用"的完整调用栈与回退/breaker 代码；
  ②SDK 侧参考接收机实现、模型生命周期与可移植层的细节。回来后并入 §2/§3。
- ⏳ `ocudu_dapp_equalizer_input_v1` / `..._output_v1`（深度 2）的字段未逐条读。
- ⏳ `pusch_dapp_channel_estimator.h`（深度 1）未读。
### 7.1 ✅ 已解决：dApp 接缝是**平台独有增补**，不在上游

| 核查 | 结果 |
|---|---|
| 我们 fork 里有吗 | **没有** `include/ocudu/dapp`、**没有** `include/ocudu/phy/upper/dapp` |
| **公开上游 `ocudu/ocudu` 里有吗** | ★ **没有**（API 列出 `include/ocudu/` 的 40 个条目，**无 `dapp`**） |
| 平台仓库是上游的 fork 吗 | **不是**（`forked_from_project: None`；独立项目，创建于 2026-05-28） |

★ 顺带确认了一件与本工作流另一条线相关的事：**上游 `include/ocudu/` 里确实有 `cuda`** ——
这正是 `metal_vs_cuda_architecture.md` 分析的"上游 CUDA 增补"。
⇒ **`memo_05` §4.0 的"两个不同 CUDA 代码库"区分得到证实**。

★★ **对我们的战略含义**：
1. **我们无法"对齐上游"**——上游根本没有这套接缝。
2. 两条路：(a) **自己实现等价的接缝**（我们规划的 `estimator.estimate` + `demodulator.demodulate`
   两处合并，本质就是 Depth-3 hook）；(b) **采用平台的 ABI**（把契约头文件 vendored 进来）。
3. ★ **许可差异**：平台/ SDK 是 **BSD-3-Clause-Clear**，我们是 **BSD-3-Clause-Open-MPI**。
   两者都宽松、兼容，但 **"Clear" 变体含专利条款**，vendoring 前应过一遍许可。
4. ★ 平台 109 M 的完整 fork 不好跟；但 **`ocudu-dapp-sdk` 只有 3.4 M 且"不复制 OCUDU ABI 头、
   不要求 OCUDU 源码树"**（它消费安装好的 `OCUDUDAppSDK` CMake 包）
   ⇒ **若要采用平台 ABI，SDK 是唯一值得引入的依赖**。
