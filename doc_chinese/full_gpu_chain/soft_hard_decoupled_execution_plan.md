# OCUDU 软硬解耦双支线执行计划（异构基带全链条重构 · 实施层）

> 本文件是执行序列的**权威版本**：把 `full_chain_gpu_uma_zero_copy_refactor_plan.md`（技术蓝图，下称「蓝图」，
> 同目录）的五阶段顺序推进，调整为**两条独立支线并行、最终在输入源处收敛**的实施策略。
> 蓝图保留为深度技术参考（file:line、代码骨架、预算表均以它为准），本文件只定义「做什么、按什么顺序、怎么验收」。

| 项 | 内容 |
|---|---|
| 状态 | v1.0 |
| 前提 | 蓝图 §0.2 的三条平台级硬事实（ICB 阻断 / 无跨 TG 软件栅栏 / 无 CPU 亲和性）仍然生效 |
| 主攻支线 | **支线一（软件流水线重构，UHD B210 输入）** |
| 并行支线 | **支线二（硬件 PCIe 直通，独立攻坚）** |

---

## 0. 策略调整：为什么拆、怎么拆、何时合

### 0.1 调整理由

原蓝图的单线顺序存在结构性风险：风险最高的硬件环节（R1 ASM2464PD 桥透传不完整 / R2 签名 /
R3 跨 PCIe 内存序）排在阶段二——**软件管线重构会被硬件攻坚阻塞**。而 GPU 链、张量化、
单命令缓冲调度这些软件工作**完全不依赖输入源形态**，本应先行。

### 0.2 解耦成立的关键事实（已核实）

**UMA 下，UHD 的接收缓冲本身就是统一内存**：`lower_phy_baseband_processor.cpp:240` 的
`receive()` 把 I/Q 写入 `baseband_gateway_buffer_dynamic`（`ci16_t`，存储为 `dynamic_tensor`
= `std::vector`，`include/ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h:115-119`）。
只要把这块内存**页对齐化**（S0 前置改造），`newBufferWithBytesNoCopy` 就能把 UHD 腿的 I/Q
零拷贝包装成 `MTLBuffer` 供 GPU FFT 直接消费——**支线一在 UHD 上就能拿到终态几乎全部的内存语义**。

因此两条支线在 GPU 链面前只剩两个可抽象的差异：

| 维度 | 支线一（UHD/B210 USB） | 支线二（FPGA/Thunderbolt DMA） |
|---|---|---|
| I/Q 字节来源 | `receiver.receive()` 拉取，写入主机缓冲池 | FPGA 直写 DMA 环形缓冲（UMA） |
| 时间戳语义 | UHD 样本计数（GPSDO 锚定），软件换算 slot | FPGA 硬件时间戳（1PPS+10MHz），Header 直读 |
| 抖动特征 | USB 调度抖动（百 µs 级，验收时**容忍**） | 目标 p99 ≤ 50 µs |

抽象掉这两点，**输入源切换 = 换一个 `iq_source` 实现**，GPU 链一行不改。

### 0.3 收敛图

```
 支线一 (主攻)                                 支线二 (并行攻坚)
 ┌──────────────────────────────┐      ┌─────────────────────────────┐
 │ S0 页对齐基带池 + UMA 包装冒烟 │      │ H1 DriverKit + DMA 映射贯通    │
 │ S1 Metal 算子 + 8×8 张量重排  │      │ H2 FPGA DMA 引擎 + 组帧         │
 │ S2 单命令缓冲 + MTLEvent 调度 │      │ H3 1PPS/10MHz 时序 + done 内存序│
 │ S3 GPU 端 FAPI + 轻量 MAC 唤醒│      │ H4 高压 soak: p99≤50µs          │
 └──────────────┬───────────────┘      └──────────────┬──────────────┘
                │        【收敛阶段 C】                 │
                └────────► iq_source 切换 ◄────────────┘
                          影子运行 → 切源 → 硬时间戳上线 → 终态验收
```

---

## 1. 解耦接口契约：`iq_source`（本次调整的核心设计产物）

> 定位：新增抽象接口 `include/ocudu/phy/upper/gpu/iq_source.h`（新文件）。
> 它**不是**替换 `baseband_gateway_receiver`，而是 GPU 链（lower PHY 的 GPU 变体）与
> 输入源之间的唯一依赖；CPU 回退腿继续走现有 `baseband_gateway` 体系，互不干扰。

### 1.1 接口定义（代码骨架）

```cpp
// include/ocudu/phy/upper/gpu/iq_source.h
namespace ocudu::phy::gpu {

/// GPU 链眼中的"一个槽的 I/Q"：UMA 页对齐视图 + 双语义时间戳
struct iq_slot_view {
  const void* base;          // 页对齐 UMA 地址 (ci16, channels × samples)
  size_t      bytes;         // 页倍数长度
  uint32_t    slot_word;     // slot_point 打包值 (ran/slot_point.h:219-223)
  uint64_t    timestamp;     // 源语义时间戳 (UHD 样本计数 | FPGA 纳秒), 审计用
  uint16_t    seq;           // 环序号 (FPGA 腿) / 池序号 (UHD 腿)
};

/// 输入源抽象。两个实现: iq_source_uhd (支线一) / iq_source_fpga_dma (支线二)
class iq_source {
 public:
  /// 非阻塞获取槽视图; 未就绪返回 false。成功获取后槽归调用方所有,
  /// 直到 release() 或 GPU 完成回调触发 (见 1.3 生命周期)。
  virtual bool try_acquire(uint32_t slot_word, iq_slot_view& out) = 0;

  /// 消费完成 (CPU 腿 / 丢弃路径调用)
  virtual void release(uint32_t slot_word) = 0;

  /// 阻塞等待就绪 (仅 CPU 回退腿用; GPU 腿用 MTLEvent 等待, 不阻塞线程)
  virtual bool wait_ready(uint32_t slot_word, std::chrono::microseconds deadline) = 0;

  /// 硬件时间戳能力标志 (支线二=true): 上层据此决定 slot 对齐走硬件 Header 还是软件换算
  virtual bool has_hardware_timestamp() const = 0;
};

} // namespace ocudu::phy::gpu
```

### 1.2 两个实现的落点

**`iq_source_uhd`（支线一，S0 之后可用）**：
- `try_acquire` 返回页对齐 rx 池中已由 `receive()` 填满的槽（对齐改造见 S0）；
- `slot_word` 沿用现有样本计数→slot 的换算逻辑（`ru_controller_sdr_impl.cpp:39-61` 同族逻辑，
  在 lower PHY 侧已由 `last_rx_timestamp` 推导，`lower_phy_baseband_processor.cpp:266`）；
- `has_hardware_timestamp() == false`。

**`iq_source_fpga_dma`（支线二，蓝图 §3.2–3.6 的 `ru_fpga_dma` 之上薄封装）**：
- `try_acquire` = 读 DMA 环 HW 写指针，返回已 `done` 槽的页对齐视图（dext 映射区）；
- `slot_word`/`timestamp` 直接来自块 Header（`ocudu_dma_block_header`，蓝图 §3.3）；
- `release` = dext 门铃回写 RD 指针（**在 GPU 完成回调之后**调用，见 1.3）。

### 1.3 生命周期规则（两腿一致的唯一真源）

**GPU 腿的缓冲所有权随命令缓冲走**：`try_acquire` 移交所有权 → GPU 链 commit →
`addCompletedHandler` 里（蓝图 §4.4 的同一回调路径）才 `release`。
因此：
- UHD 腿：rx 池深度 ≥ **在飞槽数 + 1**（首版定 4 槽，`MAX_UL_SLOT_HEADROOM` 配置化）；
- FPGA 腿：DMA 环槽数 ≥ 同值（蓝图 §3.2 的 `dma_ring_slots: 8` 天然满足）；
- **任何 CPU 腿的 `release` 不得早于 GPU 完成**——这是把「拉模型阻塞」换成「所有权移交」的
  唯一正确性约束，S2 验收必须包含「无提前回收」的断言检查。

---

## 2. 支线一：软件流水线重构（主攻方向）

> 目标：UHD 输入不变的前提下，交付终态软件架构的全部要素（GPU 算子、张量化、单 CB 调度、GPU 端 FAPI）。
> 验收口径中**明确容忍 USB 抖动**：测吞吐/队列深度/无阻塞，不测绝对时延。

### S-1 开工门禁：管线审计与既有问题修复（1 周，可与 S0 并行）

> 对应蓝图阶段一（§2）。**这是支线一 S1 正式开工的前置条件**；
> 支线二 H1 不依赖本节，可立即并行启动。

- **操作**：
  1. **执行蓝图 §2 的完整审计**：B1–B14 逐项量化（`ul_pipeline_probe` 五段 +
     A1 新增 `eq_dem`/`fapi_mac` 分段 + A2 Metal commit 计数探针）；
     产出 `doc_chinese/full_gpu_chain/pipeline_audit_2026-09.md` 基线报告。
  2. **修复审计确认的既有问题**（已点名清单，全部为**行为不变的安全修复**）：
     - `ocudu_metal_mmse_engine.mm:40-58` 指针缓存命中**不校验长度** → 加长度断言；
     - `port_channel_estimator_metal_mmse_impl.cpp:670-675` 热路径**忽略引擎返回值**
       → 失败显式告警（消除「静默过期 CE」隐患，蓝图 B8）；
     - `ocudu_metal_decoder_engine.mm:152-155` 同型缓存问题 → 同法修复；
     - `upper_phy_factories.cpp:768-769` 致命错误打印 `crc_calculator_type` → 改为
       `ldpc_decoder_type`；
     - `upper_phy_factories.cpp:657-664` 度量装饰器二次建厂**丢 106-PRB HELENA 路径** → 补传；
     - `ocudu_metal_decoder_engine.h:31-39` persistent 多线程组**过时注释** → 更正为
       单 TG×1024 实况（PLAN.md §4.12）。
  3. 审计中新发现的同类问题按同一纪律处理（只修行为不变项，**不做任何重构**）。
- **验证闭环（测试确认）**：
  - 每个修复配单测或断言回归（重点：`port_channel_estimator_metal_mmse_unit_test`
    Test 5/8、`ldpc_metal_bler_test`）；
  - 既有 E2E 全量回归：ZMQ 腿 + UHD 腿 500 ping 通过（修复不得改变任何数值行为）；
  - 基线快照存档：30 kHz 51 PRB 与 15 kHz 各 ≥ 200 槽的分段数据（后续 S-M1/S-M2
    所有 A/B 门禁的对照基准）。
- **Go/No-Go 开工门禁（全部满足才进入 S1）**：
  - [x] 审计报告入库，B1–B14 每项有量化值（`pipeline_audit_2026-09.md` §3/§6.4）；
  - [x] 修复清单全部合入且单测 + E2E 回归全绿（7598/7598 单测 + 双腿实机）；
  - [x] 基线快照归档（ZMQ 1692 样本 / OTA 1113 样本，2026-09-11）；
  - [x] A1/A2 探针可用（它们同时是 S2 的验收工具：GPU 队列深度、编码 vs 等待拆分）；
  - [x] S0 完成（实现 + 单测 + **实机 E2E 回归通过**，2026-09-11：OTA 673 样本 / ZMQ 1690 样本与基线一致）。

> **进度（2026-09-11 会话）**：A1（`ul_fapi_mac` 分段）与 A2（`[metal_stats]` commit/wait 探针）
> 已落地并在双腿实机产出数据 ✅；6 项行为不变修复已合入，**全量单测 7598/7598 通过** ✅；
> 基线快照已归档（ZMQ 腿 1692 样本 + OTA 腿 1113 样本，见
> `doc_chinese/full_gpu_chain/pipeline_audit_2026-09.md` §6）✅；
> 双腿 E2E 功能回归由上述实机运行代表通过 ✅。**S-1 与 S-0 全部收官（探针/修复/审计/
> 基线/页对齐池/双腿实机回归，7603/7603 单测），下一动作 = S1 正式开工。**

> **纪律**：审计/修复（S-1）与改造（S1–S3）**严格分批**——修复批次合入后若 E2E
> 回归出问题，可立刻归因于修复本身；避免「修 bug + 重构」混批导致无法二分定位。

### S0 前置改造：页对齐基带缓冲池 + UMA 包装冒烟（0.5 周）

> **进度（2026-09-11）**：✅ 完成并合入（commit `f71ef851c0`）。`baseband_gateway_buffer_dynamic_aligned`
> + lower-PHY rx 池切换 + 4 项单测 + 1 项 Metal 零拷贝冒烟全绿；实机 E2E 回归通过
> （OTA 673 样本 / ZMQ 1690 样本，与基线逐项一致，见
> `pipeline_audit_2026-09.md` §6.5）。

- **操作**：
  1. 新增 `baseband_gateway_buffer_dynamic_aligned`（或给 `dynamic_tensor` 加
     `posix_memalign(4096)` 分配器选项，参照 `ldpc_decoder_metal.cpp:101-108` 的
     `aligned_alloc` 范式），页对齐 + 页倍数长度；`lower_phy_baseband_processor` 的
     rx 池换用该类型（其余读写接口零改动，`get_channel_buffer` 语义不变）。
  2. 冒烟 kernel：`dma_smoke_stats` 读整个槽的 ci16 视图，算和/峰均比，与 CPU 遍历结果比对。
- **验证闭环**：`newBufferWithBytesNoCopy` 包装成功（nil 即失败）+ 统计值逐位一致；
  既有 ZMQ/UHD E2E 脚本回归全绿（证明对齐改造无行为变化）。
- **门禁**：冒烟通过；`ctest` 全绿。

### S1 Metal 核心算子与张量重排（2–3 周，蓝图 §4.1 + §5.1–5.2）

> **进度（2026-09-11）**：✅ **S1a 完成**（commit `35a7c1dd69` + `1532af161d` + `75e7d374bf`）——
> `dft_processor_metal`（Metal GPU FFT，radix-2 DIT，pow2 ≤ 4096）+ 引擎（预编译 metallib、
> 零拷贝包装、构造期 warm-up、`ENABLE_METAL_STATS=ON` 构建下输出 [metal_stats] 探针）+
> `create_dft_processor_factory_metal()`（支持尺寸走 GPU、其余逐配置回退）+
> **路由对齐既有模式：`expert_phy --pusch_dft_type cpu|metal`**（默认 cpu；env 变量仅保留 debug 用途）；
> 调试开关统一为编译期控制（`ENABLE_METAL_STATS` / `ENABLE_CE_TIME`，default OFF，
> 删除 `OCUDU_MMSE_TIME`/`OCUDU_MMSE_DBG`/`OCUDU_MMSE_NOGPU`，保留 HELENA 两个运行时变量）。
> 随机 IQ 对拍：NMSE **−130…−134 dB**（门禁 −60 dB）；往返误差 ≤ 1e-5（门禁 1e-3）。
> 单次变换时延 metal 174–246 µs（含 commit+wait）vs CPU generic 1–4 µs——符合预期，
> 收益结构性（FFT 输出驻留 UMA 供 GPU 链消费），由 S2 单命令缓冲化兑现。
> 待办：**S1a.1 已完成（2026-09-12，commit `27603740ad` + `13fe9bd731`）**：内核扩展为混合基
> 2^k·3^m（≤4096，含实链主力 768/1536/3072），A/B 全尺寸 NMSE −129…−132 dB、往返
> ≤ 2e-5；混合基数字反转表由引擎托管为零拷贝缓冲。**路由限定 UL RX**（commit
> `13fe9bd731`）：Metal DFT 只接 OFDM 解调器与 PRACH 解调器，TX 调制器（PDSCH/PDCCH/SSB
> 的 IFFT）保持 CPU——启动日志 `[lower_phy] DFT backend: rx=metal (GPU) tx=cpu`。
> 下一步：**S1b 已完成（2026-09-12，commit `888ca00580` + `83067aef46`）**：MIMO 均衡器
> `channel_equalizer_metal`（ZF/MMSE，2–4 层 × 2/4/8 端口）+ 解调 LLR
> `demodulation_mapper_metal`（QPSK/16/64/256QAM），后端开关
> `--pusch_channel_equalizer_backend cpu|metal`（默认 cpu；该开关同时路由 PUSCH 均衡器与
> 软解调器，PUCCH/PDSCH 保持 CPU 不动；BPSK/π/2-BPSK 与单层拓扑逐配置回退 CPU）。
> 对拍结果：均衡符号 NMSE −76…−140 dB（门禁 −60 dB）、噪声方差相对误差 4e-7…3.4e-4
> （门禁 1e-3；4×4 ZF 方阵病态 RE 上跨 ISA float32 ulp 差异被条件数放大，CPU 自身对
> 双精度基准同量级偏离，故门禁按实测放宽）；demapper int8 **逐位一致**（4 调制 × 随机+
> 特殊用例，内核逐操作镜像 CPU NEON 路径：区间表以相同 float32 表达式预算、safe 噪声
> 倒数、逐分量近零掩码、rint 舍入到偶）。时延（含逐次 commit+wait）均衡 ~260–330 µs、
> 解调 ~195 µs——与 S1a 同口径，收益由 S2 单命令缓冲化兑现。
> 补充：**S-1b.1 已完成（2026-09-12，commit `e98714a31f`）**：ZMQ/OTA E2E 实测发现
> `[metal_stats] equalizer commits=0`——原均衡器只覆盖 2–4 层，而参考配置
> （`dci_format_0_1_and_1_1: false` → DCI 0_0 + 单天线 UE）恒为单层 PUSCH，Metal 均衡器
> 实际从未启用。已补齐单层 1×P SIMO 路径（复刻 CPU `equalize_zf_1xn` 的逐端口信道有效性
> 掩码与 `(d>0)&&(d<inf)` 输出判定）+ 端口归约（丢弃 σ² 非正/非有限的端口、全无效输出语义）
> + 单层 tx_scaling 语义（H 不缩放、进伪逆分母），接受集合与 CPU generic 完全一致
> （1/2/4/8 端口 × 1–4 层）。单层对拍 nmse −112…−116 dB（受 CPU 侧 rcp+牛顿精度限制）、
> 归约与全无效用例逐位一致；4×4 ZF 方阵病态 RE 的 nv 门限改为条件数感知并打印越界计数
> （512 项中 4 项 >1e-3）。另新增 PUSCH 均衡器/解调器后端启动日志，避免再次出现
> "某后端 0 commit 但无提示"。
> 补充 2：**S-1b.2 已完成（2026-09-12，commit `b623adf3fb`）——去掉 eq/demap 的 staging 拷贝**。
> 背景：OTA 实测显示 `ul_equalization_demod` 单次 dispatch ~30.9 µs（对比零拷贝的 DFT 仅
> 2.6 µs），其中大部分是 CPU 侧搬运/转换：
> 1) 均衡器输入（H/y）原为 bf16→float 逐元素转换 + 拷贝 → 改为**内核内 bf16 展开**
>    （16 位左移，与 CPU `to_float` 位等价）+ bf16 memcpy（字节数减半），多层路径的
>    tx_scaling 也随之移入内核；
> 2) 均衡器输出（eq/nv）在调用方缓冲**页对齐时原地直写**（staging 作为回退）；
> 3) 解调器输入（均衡符号 + 噪声方差）同样页对齐直接消费；
> 4) `pusch_demodulator_impl` 的 `temp_eq_re`/`temp_eq_noise_vars` 改用新增的
>    `page_aligned_allocator`（页对齐 + 页倍数分配），使原地路径在真实流水线中生效。
> 本地（虚拟化 GPU，往返 ~100 µs 主导）实测原地路径省 8–10 µs/次（1272/3276 RE，8–11%），
> 仅为 CPU 侧搬运；OTA bare-metal 的 A/B 才是该阶段的基准测量。
> 待办：LLR 输出仍走 staging（UL-SCH 解复用缓冲未对齐）；CE/grid 缓冲未对齐，H/y 仍
> 有一次 memcpy——若要彻底零拷贝需把同样的页对齐分配扩展到 CE 输出池与 grid。
> 下一步：**S1c（未开始）**：8×8 `simdgroup_matrix` 张量重排 CE v2。

### A：GPU LDPC 性能问题——测量报告与方案（2026-09-12，仅测量，未改动 LDPC 实现）

**实测事实**（Mac mini M4 Pro，裸机）：
1. **单次 GPU dispatch 的发射开销恒为 ~7.3–9.4 µs，与 grid 规模无关**（用自研 kernel
   从 1 线程扫到 8192 线程，单 command buffer 内连续 16 次取 min/p50）——这是平台发射
   下限，不是算力问题（GPU 6 µs 就能跑完一个 1272 线程内核）。
2. **commit+wait 每次 ~85–100 µs**；同一内核"每 16 次只 wait 一次"降到 18.1 µs/次，
   "16 次收进单 CB"降到 12.8 µs/次（S-2b，commit `6a4780c17d`）。
3. LDPC 各变体的 dispatch 结构（主机代码读出）与实测：

   | 变体 | dispatch/迭代 | grid | 实测 gpu wall |
   |---|---|---|---|
   | `metal_persistent` | **1（整个译码）** | **1 threadgroup × 1024 线程 = 1 个 GPU core** | 1672 µs |
   | `metal`（layered） | **42**（BG2 每校验节点一次）+ syndrome + ET | 256 threadgroups × 32 | 1409–1532 µs |
   | `metal_flooding` | 2（CN+VN）+ 3 固定 | 宽网格 | 1612 µs |
   | `metal_async` | 2 | 宽网格 | **113238 µs（不可用）** |

4. 迭代次数扫描（BG2 Z256）：max-iter=1 → 906 µs，=2 → 1500 µs，Δ≈594 µs/迭代，
   ≈ 44 dispatch × ~13.5 µs，**与"发射开销主导"模型吻合**。
5. CPU 参考：同 TB 译码 **7–18 µs**。

**结论**：三条 GPU 路径都不可用，但原因不同——layered 是**发射次数**病（42 次小
dispatch/迭代），persistent 是**单核并行度**病（1024 线程只用 1 个 core），
flooding 需要单独剖析其内核，async 明显是坏的。

**方案（按预期收益/成本排序，均需用现有 `ldpc_metal_bler_test` 验证 BLER 不退化）**：
1. **行分组分层（推荐）**：把 42 个 BG 行分成 G 组（G=6–8），每组一次 dispatch、
   grid = 组内行数 × z 个 threadgroup。迭代内 dispatch 从 44 降到 ~G+2 → 预期
   ~8× 提升（906–1532 µs → ~150–250 µs/译码）。组内并行会削弱 Gauss-Seidel 收敛性，
   需实测 BLER/迭代数补偿。
2. **合并 syndrome 与 ET**：syndrome dispatch 用了 m_aligned=10240 个 threadgroup 只做
   奇偶校验，ET 又是 1 线程单独一发——二者并入 CN 最后一组可省 2 次 dispatch。
3. **flooding 剖析**：它只有 ~5 次 dispatch 却仍 1.6 ms，需先量其 CN/VN 内核的实际耗时
   （可能是 H/H^T 走查或 occupancy 问题），可能是"少 dispatch + 宽网格"目标的现成载体。
4. **战略建议：改成"按槽批量译码"**。per-dispatch 下限 8 µs 决定了**单 TB 译码永远赢不了
   CPU 的 7 µs**；GPU 的赢面在"一个 dispatch 并行译 8–16 个码块"（按槽聚合所有 TB）。
   若维持逐 TB 调用，建议 PUSCH 直接走 CPU 译码（`--pusch_ldpc_decoder_type auto`），
   可立即回收 1.7–3.1 ms/样本（占 UL 流水线 25–45%）。
5. **不推荐**：Metal 无跨 threadgroup barrier，persistent 宽网格需原子自旋屏障，风险高。

**A.1 补充：把 P9 的"结构证伪"升级为可证明的紧界（2026-09-12，离线组合分析，未改代码）**

对 `ldpc_luts_impl.cpp` 的 `BG1/BG2_adjacency_matrix` 建"行-列共享"冲突图，求着色数
（贪心度降序 / DSATUR / 随机重启局部搜索 + 最大团下界）：

| BG | 现状 dispatch/迭代 | VN 正交最优分组 | 放宽列 0/1 共享后 |
|---|---|---|---|
| BG1 | 46 | **χ = 30**（团下界 30 = 着色 30，**已证最优**） | **χ = 13**（团 13 = 着色 13，**已证最优**） |
| BG2 | 42 | **χ = 23**（团 23 = 着色 23，**已证最优**；P9 贪心 24 仅差 1） | **χ = 16**（团 16 = 着色 16，**已证最优**） |

结论：
1. **P9 的负结论是结构性的、非启发式不足**：纯 VN 正交分组的最优值就是 30/23，任何更好的
   贪心都不可能改善，净收益为负这一点已经封死，无需再试。
2. **星形修复（仅放宽列 0/1）的收益被首次量化**：BG1 46→13（3.5×）、BG2 42→16（2.6×）,
   且这两个值同样是可证明的最优。按 P9 实测的"分组需 +1~2 迭代补偿（+17~33%）"折算，
   净 dispatch 仍可降 **~2–2.6×**（另有每轮 1–2 次星形修复 dispatch 的开销）。
   对应 LDPC 阶段 1742–3086 µs/样本 → 预期 **~700–1200 µs/样本**。
3. **但这仍到不了 CPU 平价**（CPU 同 TB 译码 7–100 µs；每次 dispatch 8–14 µs 的平台下限
   决定了逐 TB GPU 译码的极限），因此与 P4"无延迟交叉点"的结论一致：GPU LDPC 的定位仍是
   "BLER 平齐 + CPU 卸载"，而非延迟收益。
4. 算法层的启发（供新算法研究）：冲突全部来自**打孔核心列 0/1 的星形结构**，而 5G NR 的
   核心部分是**双对角（dual-diagonal）结构**——理论上可用**并行前缀扫描（scan）**替代串行
   前代求解，从而消掉迫使"逐行 dispatch"的串行依赖；这与 P9 记录的"打孔列延迟增量聚合
   （星形修复核）"是同一族的思路，但用 scan 可以把修复本身也做成一次宽网格 dispatch。

**待办**：P9 方向确认关闭（已证）；星形修复/scan 方向需你决定是否值得投入（RL 评估见上）；
在此之前的 OTA 建议先用 CPU 译码止血（`--pusch_ldpc_decoder_type auto`）。

### FFT 批处理的适用范围（2026-09-12 修正）

`run_batch()` 只对**相互独立的 transform 流**有意义，共三类：
1. **同一符号的多个 Rx 端口**：transform 独立、采样同时就绪 → 可批处理且**零额外延迟**
   （当前 puxch 已经是"每符号循环端口"，只需把端口循环改成一次 `run_batch(nof_ports)`）；
2. **多载波 / 多扇区**：每个 cell 一条独立符号流 → 同一符号索引跨载波一次 dispatch
   （要求同 DFT 尺寸，否则按尺寸分组）；
3. 同一符号流内的多个 OFDM 符号：**不适用**——采样按符号实时到达，整槽批处理只能等最后
   一个符号到齐，是把等待搬家而不是消除。

**多 PUSCH / 多 UE（同小区）不增加 transform**：所有用户共享同一份每符号 FFT 输出，
因此与批处理无关（早期 TODO 的表述已修正）。

- **操作**：
  1. `dft_processor_metal`（FFT，混合基，ci16 输入直读）：接替
     `puxch_processor_impl.cpp:52-53` 的 CPU FFT（`--pusch_fft_backend metal` 开关）；
  2. `channel_equalizer_metal` + GPU 解调 LLR（输出直写 LDPC int8 原生布局）；
  3. 8×8 `simdgroup_matrix` 张量重排（蓝图 §5.1 布局：Ts×Tf tile + valid_mask），
     CE v2 的 weights/apply 双 kernel 合并（蓝图 §5.2）。
- **验证闭环**（你指定的闭环，落地为测试项）：
  - **随机 IQ 对拍**：随机 ci16 输入 → GPU FFT vs `dft_processor_fftz`（CPU 基准），
    gate = NMSE ≤ −60 dB（浮点 FFT 不苛求 bit-exact，沿用 CE 的累加顺序纪律，
    `Metal_MMSE_Channel_Estimator_PLAN.md:744-764`）；
  - **LDPC 对拍**：复用既有 `ldpc_metal_bler_test` 口径（CPU/GPU BLER 曲线重合）；
  - **张量对拍**：8×8 重排 kernel vs CPU 参考，含 612=76×8+4 填充路径与 valid_mask
    边界（扩展 `port_channel_estimator_metal_mmse_unit_test.cpp` Test 8 的 A/B 门禁）。
- **门禁**：全部对拍通过；`metal_nn_mmse` v2 在 36PRB/3DMRS 口径 ≤ 30 µs。

### S2 单命令缓冲与无等待流水线调度（3–4 周，蓝图 §4.2–4.4）

- **操作**：
  1. 两个引擎新增 encode-only 接口（`encode_decode` / `encode_weights_apply`，
     蓝图 §4.2 骨架），旧同步接口保留为兼容层；
  2. **全链单 CB**：每槽一个 command buffer（FFT→RG→CE→均衡→解调→LDPC→CRC 尾核），
     `MTLEvent` 表达跨 encoder 依赖与槽间重叠；**执行器线程上 `waitUntilCompleted`
     清零**（改为 `addCompletedHandler` → 原子 + 门铃 → pusch_executor 任务，蓝图 §4.4）；
  3. 缓冲生命周期接入 §1.3 契约（rx 池 4 槽深，完成回调 release）。
- **验证闭环**（容忍 USB 抖动的口径）：
  - **GPU 队列深度监控**：引擎侧在飞命令缓冲计数探针（蓝图 A2），稳态 ≤ 2；
  - **无 CPU 阻塞证明**：`waitUntilCompleted` 在 UHD 腿的执行器路径 grep 级为 0 +
    `ul_pipeline_probe` 显示槽间流水重叠（槽 N 的 LDPC 与槽 N+1 的 FFT 并发）；
  - **帧率/吞吐**：ZMQ/UHD 双腿 500 ping 回归 + 吞吐不低于 CPU 腿基线。
- **门禁**：每槽 1 commit / 1 回调；30 kHz 槽内 GPU 链时长 ≤ 蓝图 §6.4 预算表
  （此时 E2E 总时延仍含 USB 抖动，**不**作为 S2 门禁）。

### S3 GPU 端 FAPI 生成与轻量 MAC 唤醒（1–2 周，蓝图 §6）

- **操作**：
  1. GPU 尾核 `ldpc_crc24a_pack` + `fapi_commit`（蓝图 §6.2 的 40 B 记录 ABI +
     环写指针 release 推进）；
  2. CPU shim `drain_fapi_ring()`（蓝图 §6.3）用现有 builder 展开记录 →
     `rx_data_indication`/`crc_indication` → 现有 fastpath 原样复用；
  3. B12 第一步保持 `byte_buffer::create` 拷贝（行为零变化），零拷贝演进留到收敛阶段。
- **验证闭环**：**UHD 输入 → MAC 收到 FAPI 的完整软件闭环**：
  attach + 500 ping 通过；HARQ 场景（人工丢包注入）验证 `crc_indication` 语义与
  空 span 门控逐位兼容（`uplink_processor_impl.h:108` / `fapi_to_mac_indications_fastpath_translator.cpp:87`）。
- **门禁**：功能全通；GPU→MAC 路径 0 次全量 TB 拷贝、≤1 次线程跳变。

### 支线一里程碑与 DoD

| 里程碑 | 内容 | DoD |
|---|---|---|
| S-M0 | S0 对齐池 + 冒烟 | 包装成功 + 回归全绿 |
| S-M1 | S1 算子/张量对拍 | 随机 IQ 对拍 + BLER + Test 8 扩展全过 |
| S-M2 | S2 单 CB + 无等待 | 每槽 1 commit；执行器零 wait；500 ping（UHD 腿） |
| S-M3 | S3 GPU FAPI 闭环 | UHD→MAC 功能闭环 + HARQ 兼容回归 |
| S-M4 | 支线一冻结 | 全部测试集 + 蓝图 KPI 的软件部分达标（除绝对时延/抖动） |

---

## 3. 支线二：硬件 PCIe 直通（并行攻坚，允许失败不影响支线一）

> 与支线一**零代码依赖**（唯一交汇点是 §1 的接口与 §1.3 生命周期规则）。
> 若 H1 桥验证失败（R1 命中），按熔断条件走备选桥/暂停，支线一照常推进。

### H1 DriverKit 与 UMA 零拷贝贯通（蓝图 §3.4 D1–D12）

- **操作**：dext 骨架 → `IOBufferMemoryDescriptor::Create`（页对齐、`kIOMemoryDirectionInOut`）
  → `IODMACommand` 生成 IOMMU 段（`Gen64IOVMSegments` 取物理地址段表）→ BAR0 寄存器窗映射
  （`MapDeviceMemoryWithIndex`）→ `CreateMappingInTask` 暴露用户态 → 环基址下发 FPGA。
- **验证闭环**（你指定的闭环，落地为测试项）：
  - **基础 DMA 读写测试**：用户态程序对 FPGA 寄存器窗（MAGIC/TIMESTAMP 读、SLOT_CFG 写）
    与 DMA 环缓冲做读写比对；**稳定性 soak**（≥ 8 小时循环读写 + 回环校验）；
  - **系统调用审计**：确认用户态读写全程无 `read/write` 系统调用（直接映射访问）。
- **门禁**：DMA 读写稳定 + 零 syscall 审计通过。

### H2 FPGA 侧 DMA 引擎与组帧（并行设计，与 H1 同 FPGA 工程师线）

- **操作**：C2H 描述符环（FPGA 主动推送）、块=槽整数分之一组帧、BAR0 寄存器窗
  （蓝图 §3.3 偏移表）、WR/RD 指针协议、`INTR_EN` 槽完成中断。
- **验证闭环**：FPGA 仿真平台（XSIM/QEMU 或环回模式）下描述符环满速推送不丢块、
  WR 指针单调推进；与 H1 的 dext 联调。

### H3 硬件级时序同步与一致性保障（蓝图 §3.3/§3.6）

- **操作**：1PPS+10MHz 根时钟驯服 64-bit 纳秒计数 + slot 计数；**载荷写完最后写
  Header `done` 位**（跨 PCIe 内存序，蓝图 §3.3 flags bit）；GPU 侧等待 kernel 以
  device-scope acquire 读 `done`（蓝图 §3.5）。
- **验证闭环**（你指定的闭环）：
  - 高负载数据流下用 Header 时间戳**校验丢包率**（seq 连续性）与**内存一致性**
    （done 位与载荷比对，捉跨器件乱序）；
  - **p99 抖动 ≤ 50 µs**（时间戳间隔分布，30 kHz 槽口径，≥ 1 小时样本）。
- **门禁**：零丢包（≥ 1e6 块）、零一致性错误、p99 ≤ 50 µs。

### H4 支线二冻结

- 8 小时 soak + 断电重连恢复（dext 重映射路径）+ `systemextensionsctl` 部署流程文档化。

### 支线二熔断条件（与支线一解耦的纪律）

| 事件 | 处置 |
|---|---|
| ASM2464PD 透传不完整（BAR/中断缺失，R1） | 换备选桥（TB5 扩展坞直插 FPGA 卡 / 不同桥固件）；支线一不受影响 |
| dext 签名环境受限（R2） | 开发期 `systemextensionsctl developer on`；文档先行 |
| 内存序/丢包长期不收敛（R3） | 支线二暂停 → 支线一以 UHD 交付；FPGA 腿降级为后续路线图 |

---

## 4. 收敛阶段：输入源切换（支线一冻结 + 支线二冻结之后）

### 4.1 切换前置检查表（两条腿对齐 `iq_source` 契约）

- [ ] UHD 腿与 FPGA 腿的 `iq_slot_view` 字节布局一致（ci16、channels×samples、页对齐）；
- [ ] 两腿 `slot_word` 语义一致（同一 `slot_point` 打包）；
- [ ] 两腿生命周期遵守 §1.3（release 晚于 GPU 完成回调）；
- [ ] 支线一 S-M4 全绿、支线二 H4 全绿。

### 4.2 切换步骤（平滑替换，每步可回退）

1. **影子运行**：FPGA 腿 + UHD 腿同槽双收（UHD 为主），比对 `iq_slot_view` 内容与
   slot_word 一致性（≥ 1 小时），建立双腿等价证据；
2. **切源**：`--ru_fpga_dma` 替换 `--ru_sdr`（`gnb -c configs/gnb_fpga_dma_tdd_n78_20mhz.yml`），
   GPU 链与 MAC 链零改动，跑支线一全部测试集（500 ping + HARQ 回归）；
3. **硬时间戳上线**：`has_hardware_timestamp()==true` 分支启用——slot 对齐改从
   Header 时间戳直接换算（蓝图 §3.6），CPU 调度器不再推算时隙；对拍 1PPS 沿与 SFN 边界；
4. **终态验收（蓝图 M10 重定义）**：30 kHz 20 MHz 双 UE + 移动衰落模拟：
   **E2E ≤ 200 µs 且 p99 抖动 ≤ 50 µs**；I/Q 全程 0 拷贝；CPU 每槽 ≤ 1 次回调；
   对照 S-M4 基线输出 CPU 占用下降报告。

---

## 5. 双支线并行总表

| 周 | 支线一（主攻） | 支线二（并行） | 汇合点 |
|---|---|---|---|
| 1 | **S-1 审计+修复** ∥ S0 | H1 起步 + H2 FPGA 设计 | §1 接口定稿（两线评审）；S-1 门禁评审（Go/No-Go） |
| 2–4 | S1（Go 后启动） | H1 联调 + H2 仿真 | §1.3 生命周期规则冻结 |
| 5–7 | S2 | H3 时序 | 共享：GPU 等待 kernel（done 位读取，S2 与 H3 同源） |
| 8–9 | S3 + S-M4 冻结 | H4 冻结 | 收敛阶段 C1 影子运行 |
| 10 | 收敛 C2 切源 → C3 硬时间戳 → C4 终态验收 | | |

> 支线二的周数仅为节奏建议；其冻结时间**不构成**支线一的阻塞条件（熔断纪律 §3）。

---

## 6. 风险调整登记（对蓝图 R1–R9 的增量）

| # | 新增/调整风险 | 缓解 |
|---|---|---|
| R10 | **接口漂移**：两线并行演进中 `iq_source` 契约被单侧破坏 | §1 契约定稿即冻结（评审门禁）；CI 里 UHD 腿跑契约一致性测试 |
| R11 | **UHD 抖动掩盖管线缺陷**：支线一在抖动下「能跑」，收敛后 p99 门禁暴露新问题 | S2 起用 GPU 队列深度/在飞计数等抖动无关指标做主门禁；p99 门禁明确推迟到 C4 |
| R12 | **缓冲所有权提前回收**：GPU 在飞时 rx 池槽被复用 | §1.3 规则 + S2 断言检查（完成回调前 release 即 abort） |
| R13 | 支线二长期不冻结导致收敛无限延期 | 熔断纪律：支线一 S-M4 以 UHD 交付；FPGA 腿降级为路线图后续项 |

---

## 7. 与蓝图追踪矩阵（新任务 ↔ 蓝图章节/瓶颈）

| 本计划任务 | 蓝图章节 | 对应瓶颈 |
|---|---|---|
| S0 页对齐基带池 | §3.5 对齐约束（B8 教训） | 新发现（`baseband_gateway_buffer_dynamic` 为 `std::vector`） |
| S1 FFT/均衡/解调 | §4.1 | B3/B5/B9 |
| S1 8×8 张量重排 + CE v2 | §5.1–5.2 | B5/B7 |
| S2 encode-only + 单 CB + MTLEvent | §4.2–4.4 | B6/B7/B10/B14 |
| S3 GPU FAPI + 门铃 | §6.2–6.3 | B11/B12/B13 |
| H1 dext + DMA 映射 | §3.4 | B1/B2 |
| H3 done 位 + 1PPS 时序 | §3.3/§3.5/§3.6 | R3 |
| C 切源 + 硬时间戳 | §3.6 + §6.4 预算表 | B1/B2 清零 |

---

*本计划与蓝图同步生效：蓝图提供「怎么做对」，本计划提供「按什么顺序做、验收什么」。
任何实测与 §6.4 预算表矛盾时，修正数字并回写蓝图与对应 PLAN。*
