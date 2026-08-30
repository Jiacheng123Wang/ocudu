# Apple Silicon 异构 gNB 长期规划（Metal GPU / 性能核 / NPU 协同）

> 状态：2026-08-30 立项。本文档记录 OCUDU 在 Apple Silicon 上的高层路线图，
> 供后续各 PHY 模块（FFT / Channel Estimation / Equalization / MIMO Detection /
> LDPC …）的 Metal 化工作统一参照。各模块的实现级记录在其 `metal/PLAN.md`
> （CE：`lib/phy/upper/signal_processors/channel_estimator/metal/PLAN.md`，
> LDPC：`lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`）。

## 0. "Heterogeneous" 的两层含义

本规划的 **heterogeneous（异构）** 是双重异构，且恰好映射到 V2X 的需求异构：

| 层 | 异构维度 | 组成 | 优势 |
|---|---|---|---|
| **算力异构** | 计算资源 | P 性能核 / E 能效核 / **GPU** / **NPU** | 时延敏感走 CPU 核、高并发走 GPU、AI 推理走 NPU |
| **存储异构** | 数据存放 | 寄存器/缓存 / **统一内存**（RAM）/ **flash**（mmap 页缓存） | 热数据驻统一内存零拷贝、冷数据（如 GB 级矩阵/模型权重）驻 flash 按需换页 |

**需求异构的天然匹配**：

- **V2X 控制信令**：极低时延、极高可靠性、小包——走 P/E 核（CPU 路径，µs 级链）
  + 热缓存（全部战备状态常驻内存，粮草先行）；
- **V2X 摄像头数据**：极高带宽、海量数据、时延相对宽松——走 GPU（高并发吞吐）
  + 大对象经统一内存/flash 分层（页缓存弹性驻留，§2.2 的 mmap 方案）。

一台 gnb 内**算力与存储双层异构协同**，是对需求异构的结构性应答——这也是 OCUDU
在 Apple Silicon 上相对纯 x86 方案的核心竞争力。

## 1. 定位：GPU Metal 的核心优势与适用场景

- **Metal 的优势是高并发，不是单链路低时延**。Apple GPU 以成千上万的线程隐藏延迟；
  它的价值在**多用户、大带宽、高吞吐**场景，而不是一个 UE 的一条小包链路。
- 因此**各模块的 Metal 实现允许比 CPU 路径慢 10× 以上**（模块级对拍口径，如当前
  CE metal ~200 µs vs cpu ~64 µs、LDPC metal ~550-981 µs vs cpu ~10-70 µs）。
  这是算法骨架迁移期的预期状态，不是缺陷。
- 但有一条硬约束：**E2E 链的时延必须在槽预算内**（15 kHz = 1 ms、30 kHz = 0.5 ms），
  保证 E2E 测试本身可以运行、attach/数据面可以验证。

## 2. E2E 测试的组合策略（预算控制）

- E2E 链中可以**只开某一模块的 Metal，其余模块走 CPU 路径**，用
  `expert_phy --pusch_channel_estimator_algo metal_mmse`、
  `expert_phy --pusch_ldpc_decoder_type metal` 等开关独立控制，把总时延控制在
  预算内完成功能验证（当前实践：CE-only Metal 已全通；CE+LDPC 双 Metal 已全通，
  见 §5）。
- **所有 Metal 模块同时打开是更好但非必需的目标**——它需要每模块的槽内时延都
  收敛（当前 LDPC layered 变体的 dispatch 链开销是主要缺口，`metal_persistent`
  单 dispatch 变体是现成缓解，见 LDPC PLAN §4.16/§4.17）。

### 2.1 铁律：兵马未动，粮草先行（初始化绝不允许落在数据包路径上）

统计口径只覆盖 crc=OK 的 PUSCH，意味着"引擎初始化发生在包到达之后"的惰性设计
是**逻辑错误**：gnb 必须在收到第一个数据包之前进入最高战备，包一到就全速运转。

- **一切可预先准备的资源（引擎、pipeline、矩阵、缓冲区、内核 JIT、槽构造）必须
  在启动期/构造期完成**；数据包处理路径上只允许出现真正数据依赖的计算。
- **代价要折衷**：预构建的代价主要是内存与启动时间。Apple Silicon 统一内存是
  强项，但也要按量权衡——原则是"能便宜预构建的全部预构建，太贵的保留惰性并
  在文档中明示"。
- **当前落地**（2026-08-30）：
  - LDPC layered/persistent：CSR-only 表示（~11 MB 共享矩阵 + ~37 MB/实例引擎
    缓冲）→ **全部 102 个 (BG, z) 槽在构造期一次建成**（见 LDPC PLAN §4.19）；
  - LDPC flooding/LLS/async：需要打包 H/Hᵀ（0/1 已按 uint32 位打包，全量
    H+Hᵀ 双 BG ≈ **1.4 GB**，超 RAM 预构建预算）→ 保持惰性构建 + 构造期只
    预热家族 JIT；
  - CE：引擎与 warm-up 自 A+B 起即在构造期完成（10 实例，启动期），无惰性状态；
    残留 ~3 ms = 执行器线程首次 Metal 调用的每线程驱动初始化（一次性，attach
    首槽），缓解方案（启动期在 UL 执行器线程上跑一次哑提交）列入待办。

### 2.2 离线预计算 + flash 映射的折衷评估（flooding/LLS/async 大矩阵）

对 GB 级矩阵的备选方案：离线算好存入 flash，运行时 `mmap` 直接映射进地址空间
（macOS 文件映射：数据按页从 flash 经页缓存按需调入，无显式读、无常驻拷贝）。

| 方案 | CPU 计算 | 内存 | 首次取用延迟 | 附加成本 |
|---|---|---|---|---|
| 运行时 CPU 构建（现状） | z=352 ~6.8 ms、小 z ~µs | 0（用后即驻留） | 0 | 无 |
| 离线预计算 + mmap | **0** | 弹性（页缓存，仅触达页） | ~5-20 ms/尺寸（NVMe 3-6 GB/s 按需换页） | 文件管理/版本一致性；闪存占用 |

量化结论：**首次取用延迟两者同量级**（mmap 换页 ~5-20 ms vs CPU 构建 0.01-6.8 ms），
mmap 的价值在"CPU 繁忙/实时期零计算"与页缓存弹性驻留，代价是文件管理复杂度。
离线文件规模可控（H/Hᵀ 双 BG 打包全量 ≈ 1.4 GB）。当前裁决：layered 家族用 RAM
全量预构建（已落地，CSR ~11 MB 便宜）；flooding/LLS/async 维持惰性 CPU 构建（这些
是实验模式，不在默认路径）；若未来它们转正且 CPU 预算紧张，再实施离线文件 + mmap
方案。

## 3. 终局架构：整条 UL 链一次 dispatch，扔出去不等

当 UL 链的全部模块（**FFT、CE、MIMO Detection、LDPC**）都 Metal ready 后：

- CPU 侧把整条链的 kernel 序列**编排进一个 command buffer，一次性 dispatch**，
  数据在 GPU 端通过零拷贝 buffer 逐级传递（`newBufferWithBytesNoCopy` 共享内存，
  无 PCIe 拷贝、无中间回读）；
- **扔出去就不等数据回来**：CPU 执行器线程提交后立即返回做其他槽/其他任务，
  GPU 完成后通过回调唤醒下游；
- 每模块的开发因此从"逐模块同步 waitUntilCompleted"演进到"链式 kernel 编排"——
  模块级 GPU 时延相加被**一次 dispatch 的流水线并行**取代（当前每模块一次
  commit+wait 的往返 ~20-30 µs × N 模块，也会随之消失）。

### 3.1 Metal LDPC 完成后的接续：MAC PDU 直接给 FAPI，不等

- Metal LDPC 在 GPU 上跑完（crc=OK → 正确的 MAC PDU 已在 GPU 端/共享内存中），
  **dispatch 它的 CPU 线程不再 `waitUntilCompleted`**：用
  `addCompletedHandler`（或 `waitUntilCompleted` 移到 FAPI 消费前的最后一刻）把
  解码完成事件挂回 OCUDU 的执行器任务图；
- 需要规划的机制（结合 Apple Silicon 与 OCUDU 调度）：
  1. **回调 → executor 任务唤醒**：Metal 命令缓冲完成回调运行在 GPU 驱动线程，
     应只做轻量动作（置位/入队），由 OCUDU executor 的工作线程执行后续
     (de)segmentation → MAC PDU → FAPI 路径，避免在驱动线程里跑协议栈；
  2. **优先级与反压**：多个 UE/多槽的命令缓冲共享 GPU，提交顺序即执行顺序；
     需要按 UE 的时延等级（见 §4）分区队列或打标记，防止大包 GPU 任务饿死
     控制信令；
  3. **缓存一致性窗口**：GPU 写完、CPU 读 MAC PDU 之前有一次缓存行失效开销，
     读回点应集中（一次性读完整个 PDU 区），避免逐字节乒乓。

## 4. 异构协同的 gNB 总体运行架构（市场定位）

基于 Apple Silicon（P 性能核 / E 能效核 / GPU / NPU）的 gnb 运行形态：

| 流量类型 | 特征 | 承载 | 理由 |
|---|---|---|---|
| **V2X 控制信令 / 小包** | 时延极度敏感（ms 级端到端） | **P 核/E 核（CPU 路径）** | 现有 C++ 链时延最低（CE ~64 µs、LDPC ~10-70 µs），不引入 GPU 往返 |
| **V2X 摄像头视频 / 大包** | 大带宽、高吞吐、时延相对宽松 | **GPU Metal** | 高并发吞吐；后续可叠加 **NPU** 承接 AI 化接收机（信道估计/检测的神经网络推理） |

这样一台 gnb 内**CPU 与 GPU 各司其职、异构协同**：低时延控制面走性能核，高吞吐
数据面走 GPU（未来 +NPU 做 AI PHY），在成本/功耗/性能上与纯 x86 方案形成结构性
差异——这是 OCUDU 在 Apple Silicon 上的核心竞争力路线。

## 5. 现状与下一步（2026-08-30 实链证据，500-ping 更新）

- **CE metal_mmse**：E2E 全通；**500 个 ping 全部通过**（2013 样本）。稳态
  median ~255 µs（3 DMRS 符号、36 PRB），p99 457 µs；首槽一次性 ~2.6 ms（已知，
  per-thread 驱动初始化，attach 时一次）。cpu 基线 63.6 µs。
- **LDPC metal_persistent**：与 CE 双开全通（500 ping，crc=OK 100%）。median
  **730 µs**（min 288 / p95 1746 / p99 2122）：实链 2-5 dB 工作点 iter==max_iter
  （2-4 轮全预算、crc=OK）是正常收敛行为，每轮 ~300-400 µs = 46 次 × 1024 线程
  device 级 barrier + 层计算；**粮草先行已落地**（102 槽启动期 ~12 ms 建齐、
  运行中零惰性构建、首包解码 171× 提速）。
- **管线** median 1133 µs（超 1 ms 槽长 ~13%）：ZMQ 无硬实时、功能达标；压预算
  方向 = LDPC 每轮 barrier 链（多 TG + 设备栅栏 persistent / 小 z 走 layered）。
- **中期**：CE 引擎单例 + 跨端口批处理（CE PLAN (D)）；首槽 per-thread 税缓解
  （启动期执行器线程哑提交）；逐模块把同步 wait 改为回调挂接（§3.1 机制）。
- **下一个任务**：**AI based channel estimation**（HELENA/MPSGraph，见
  `AI_channel_estimation_implementation_plan.md`——L0 基线（metal_mmse）已就绪，
  直接进 G1 延迟原型门禁）。
- **长期**：FFT / Equalization / MIMO Detection 的 Metal 化（复用 CE/LDPC 的
  引擎范式与经验教训：occupancy 优先、构造期 warm-up、metallib 路径权威化、
  ocudulog 诊断、累加顺序保 bit-exact、粮草先行——CE PLAN §7.0.15）；最终拼成
  §3 的单 dispatch 全链编排与 §4 的异构分流架构。
