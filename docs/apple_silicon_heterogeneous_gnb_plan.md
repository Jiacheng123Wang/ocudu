# Apple Silicon 异构 gNB 长期规划（Metal GPU / 性能核 / NPU 协同）

> 状态：2026-08-30 立项。本文档记录 OCUDU 在 Apple Silicon 上的高层路线图，
> 供后续各 PHY 模块（FFT / Channel Estimation / Equalization / MIMO Detection /
> LDPC …）的 Metal 化工作统一参照。各模块的实现级记录在其 `metal/PLAN.md`
> （CE：`lib/phy/upper/signal_processors/channel_estimator/metal/PLAN.md`，
> LDPC：`lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`）。

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

## 5. 现状与下一步（2026-08-30 实链证据）

- **CE metal_mmse**：E2E 全通（UE 接入、IP 获取、ping 通）；稳态 ~200 µs/槽
  （3 DMRS 符号、36 PRB），管线预算内；首槽一次性 ~3 ms（已知，attach 时）。
- **LDPC metal（layered）**：E2E 与 CE 双开全通（crc=OK、0 nok 稳态），但每解码
  **~550-981 µs**，根因 = 每解码固定 290 个 dispatch（max_iter=6 × 48/轮）×
  ~1.9 µs/dispatch 的调度链开销（小 TB 的 GPU 算力远未吃饱）；离群值 ~3-8 ms =
  运行中首次出现的 (BG, Z) 槽构造（H 矩阵/CSR 构建，CPU 侧）。
- **立即实验项**：`expert_phy --pusch_ldpc_decoder_type metal_persistent`
  （单 dispatch 常驻内核，迭代/层循环在核内，单测对拍 100% 一致）——预期消除
  dispatch 链开销；若仍超预算，按 §2 策略 E2E 时 LDPC 回 CPU 即可。
- **中期**：CE 引擎单例 + 跨端口批处理（CE PLAN (D)）；LDPC 槽预构建常用
  (BG, Z)；逐模块把同步 wait 改为回调挂接（§3.1 机制）。
- **长期**：FFT / Equalization / MIMO Detection 的 Metal 化（复用 CE/LDPC 的
  引擎范式与经验教训：occupancy 优先、构造期 warm-up、metallib 路径权威化、
  ocudulog 诊断、累加顺序保 bit-exact——CE PLAN §7.0.15）；最终拼成 §3 的
  单 dispatch 全链编排与 §4 的异构分流架构。
