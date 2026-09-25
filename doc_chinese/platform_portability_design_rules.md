# 平台可移植性设计要点 —— 依赖的是"所需的硬件能力 + 软件的可应用性"，不是 Apple Silicon / macOS

> 状态：设计约束文档（v1.0，2026-09-25）
> 定位：**约束，不是愿望清单。** 本文只写三类东西：
> **(A)** 我们的代码**实际**依赖了平台的什么（带文件:行号）；
> **(B)** 为了让下一块平台出现时"搬得动"，高层设计**必须守住**的规则（每条附"我们在哪里学到它"）；
> **(C)** 今天已经欠下的账（泄漏清单，带替换形状）。
>
> 关联文档：
> - `apple_silicon_competitiveness_analysis.md` §5.3 —— **为什么**要做这件事：
>   能力类别的**硬件半**已在五家商品硅上成立，**软件半**今天只有 Apple 成立
> - `metal_vs_cuda_architecture.md` —— 抽象**不要**做在哪一层（上游 CUDA 的 "device backend" 抽象层 8/10 条不满足）
> - `phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` —— 当前融合车道的判据权威
> - `apple_silicon_heterogeneous_gnb_plan.md` —— 长期异构规划（CPU / GPU / ANE 路由）
>
> **⚠ 数字分栏约定**：本文沿用竞争力分析的分栏——**【官方】**（厂商文档，带链接）、
> **【实测】**（本仓库测量，标来源）、**【推算】**（折算，不得当实测引用）、
> **【待核实】**（无一手出处，不得进入对外材料）。

---

## 0. 一句话结论，和一条今天就能跑的判据

> **我们的核心依赖不是 Apple Silicon，也不是 macOS，而是两件可以分别查证的事：**
>
> | 层 | 内容 | 今天的状况 |
> |---|---|---|
> | **A 层：硬件能力** | 统一可寻址内存、可编程设备、提交内顺序、**值语义的完成凭据**、推理引擎……（§2 的 C1–C9） | **五家商品硅都已成立**（竞争力分析 §5.3.2） |
> | **B 层：软件可应用性** | 驱动、可编程性、**能在本平台生成模型**、引擎选择是一次配置变更、可观测性（§3 的 S1–S6） | **今天只有 Apple 成立**，且是**会被补齐的时滞** |
> | **C 层：API 形状** | `MTLBuffer`、`dispatchThreads`、`.mlmodelc`…… | **不是依赖**，是**实现细节**。把 C 层误当 A 层，是**唯一**会把我们锁死的错误 |

**⇒ 于是可移植性工作的目标不是"写一个跨平台后端"，而是三件更小的事：**

1. **把代码对平台的依赖压缩到 A 层**——A 的每一项都有**厂商中立**的接口表示，
   并且**能力在运行时协商**，不在编译期按平台名判断；
2. **让 B 层的缺口在换平台时表现为"一张待办清单"**，而不是"一次重新设计"；
3. **绝不让平台的名字、API 或调度语义渗进判据、配置、接口与测量口径**。

### 0.1 ★ 一条今天就能跑的判据

> **判据 P0**：把一个候选平台的实现换进来时，**需要改的文件应当全部落在 `*/metal/` 之内**，
> 外加一张**可枚举**的少量旁路清单。
>
> **如果一个改动为了换后端而修改了 `include/ocudu/phy/` 下的判据、契约、模式、接口或配置语义，
> 那是一次架构回退，不是一次移植。**

**这条判据的价值在于：它今天就是失败可判的。** §6.2 列出了今天会让它失败的全部位置——
**数量不多，而且每一条都有一个明确的替换形状**。这也是本文存在的理由：
把"可移植性"从一种态度，变成一张**会随提交变长变短的清单**。

---

## 1. 依赖的正确分解

### 1.1 三层，只有前两层是硬的

| 层 | 问的问题 | 举例 | 性质 |
|---|---|---|---|
| **A 硬件能力** | 这块硅**能不能**支撑这个数据流 | 设备能不能直接读宿主内存；能不能提交一批自定义 kernel | **硬依赖**。缺了要重新设计 |
| **B 软件可应用性** | 这套栈**今天能不能用** | 驱动在不在；能不能在本平台编译模型；profiler 有没有 | **硬依赖**，但是**会变的**（时滞） |
| **C API 形状** | 调用的**写法**长什么样 | `[cb commit]` vs `cudaStreamSynchronize`；`.mlmodelc` vs `.onnx` | **不是依赖**。必须被隔离在实现目录内 |

### 1.2 为什么 C 层最容易被误当成 A 层

**因为 C 层是唯一"看得见"的一层。** 写代码时接触到的是 API，于是"移植"很自然地被理解成
"把 API 换一遍"。**那个理解会导出错误的结论**：

| 错误结论 | 为什么错 |
|---|---|
| "我们没有用 CUDA，所以我们不可移植" | 用不用 CUDA 是 C 层的事。**可移植性取决于 A 层的接口是否中立**，与当前后端无关 |
| "要可移植，就得先写一个跨平台 device 抽象层" | 上游 CUDA 那套 device 抽象层被逐条评过：**8/10 条不满足，其中一条语义反转**（`metal_vs_cuda_architecture.md` §5）。抽象做在 C 层等于把**平台差异最大的那一层**拉平——而调度语义的差异恰恰是我们需要保留的自由度 |
| "等有第二个平台了我们再重构" | 到那时，**判据已经跟着平台走了**。重写判据的成本远高于重写后端 |

**⇒ 结论**：可移植性的战场在 **A 层的接口**（`resource_grid_device_view`、`dft_processor`、
`grid_ready_hook`），不在 C 层的封装。

---

## 2. A 层：硬件能力契约（C1–C9）

**读法**：这是一份**给候选平台的问卷**。每一行的"缺失时退化成什么"是**最重要的列**——
它说明这一项**不是性能差异，而是设计差异**。

| # | 能力（厂商中立表述） | 本仓库的证据：它为什么是必需的 | 我们当前的表达 | **缺失时退化成什么** | 可接受的替代 |
|---|---|---|---|---|---|
| **C1** | **统一可寻址内存**：设备可直接读写宿主分配的内存，无需显式拷贝 | §2.4"路由边际成本为零"是整个异构设计的地基；设备写者需要一个**页对齐宿主分配**的地址才能寻址 | `resource_grid_device_view`（base + strides）；`page_aligned_allocator`；`compat::page_size()`、`compat::describe_aligned_allocation()` | **每一次路由 = 一次拷贝**，穿越次数从 2 变成 2+K（K = 参与路由的模块数）。**这是唯一会让整个设计前提失效的一项** | 相干互联（NVLink-C2C / CXL）。**⚠ 但必须核实"设备写对宿主可见"是否自动成立**；若需要显式 flush/invalidate，它**不等于** C1，要按 C1′ 记账 |
| **C2** | **可编程设备**：可提交自定义数据并行 kernel（不是固定功能单元） | 链式 kernel 编排；位精确对拍（`metal_mmse` 与 `metal_nn_mmse` 是同一套数学的两个 kernel，用于把差异归因到 kernel 本身） | 每个模块一个 `*_engine`，各自编译自己的 kernel 库 | 只剩"跑厂商预置算子图"的能力 ⇒ **CPU 卸载没了，只剩 AI PHY 一条腿** | 无。固定功能替代不了这一项 |
| **C3** | **提交单元 + 提交内顺序**：能一次提交一批工作，且批内顺序由调用方指定 | 一跳一次提交（`cbs/lane` 2.70 → 2.00）；一个命令缓冲 + pipeline 变化处**显式屏障** | `shared_burst`（`ocudu_metal_burst.h/.mm`）；每个引擎的 `submit()` / `commit()` | 每阶段一次提交 ⇒ CPU 编码开销与提交数成正比（§6.3 量化：LDPC 290 dispatch/解码 ≈ 550 µs + CE ≈ 204 µs/跳） | 无 |
| **C4** | **负向能力**：平台**必须明确说出它不保证什么** | 我们依赖的是"**提交之间只保证开始顺序**"这一条**否定式**事实——正是它逼出了 C5 | `ocudu_metal_queue.h`（两处："only orders the STARTS of its command buffers"）；`shared_burst` 头注释（"they may overlap"） | 若把"提交之间有数据依赖顺序"当默认，得到**偶发读到旧数据**——不报错、不可复现 | —（这一项不是"能力"，是**必须查证的语义**） |
| **C5** | **可命名、可跨提交请求的完成通知，且是值语义（不是对象语义）** | 一个 generation 既能被**宿主阻塞等待**，也能被**编码进设备命令缓冲**；而"等待一个尚未 commit 的命令缓冲对象"在平台上**是非法的** | `grid_ready_signal/wait/encode_wait`；`backend_stage_signal/wait_generation`（`ocudu_metal_queue.h`） | **程序照样跑，但宿主的参与点无法消除**——恰好是主判据（§1.2 的"CPU 彻底甩手"）要消除的东西。上游 CUDA 的 `cuda_event` 只有"等 / 完成没" ⇒ **宿主轮询**（`metal_vs_cuda_architecture.md`） | 事件 / fence / 信号量，**只要能用单调整数命名**；纯轮询不可接受 |
| **C6** | **一个推理引擎**（与通用计算单元**并列**，不是它的一个切片） | ANE 191 µs vs GPU 同类功能块 570–620 µs/授权（竞争力分析 §5.3.4） | Core ML（一个 API 覆盖 ANE/GPU/CPU） | AI PHY 只能挤进 GPU 队列 ⇒ 既慢，又与 PHY 主体争用（`full_gpu_chain/` 风险登记 R8：ANE 与 GPU 争内存带宽） | 任何独立调度的推理单元。**"GPU 的一个 SM 分区"不算**（见 `apple_silicon_competitiveness_analysis.md` 的分区分析） |
| **C7** | **单调时间基 + 可用的线程调度设施** | 实时性判据、lane probe、超时上界（`grid_ready_wait(gen, timeout_ms)`） | `compat::get_monotonic_time_us()`；`darwin_thread_scheduling.h/.cpp`；`compat::posix_realtime_priority_is_enforceable()` | 时延轴无法测量 ⇒ §9 的那场决定性实验**无法在新区块上复现** | 任何单调时钟。**注意 Apple 在 C7 上是"负分"**：`set_thread_affinity` 在 Apple Silicon 上是 no-op（`darwin_thread_scheduling.cpp:82`：`THREAD_AFFINITY_POLICY` 仅 XNU-on-Intel 实现），我们用 QoS + Mach time constraint 顶。**换到 Linux 反而多一项能力** |
| **C8** | **并发宿主提交**：多线程各自提交而不互相阻塞到不可用 | 单队列时"后端排在在飞的 FFT 之后，实测延迟爆炸" ⇒ 必须是**两个队列**；而车道的有效 PUSCH 并发实测是 **1**（一条串行链，commit `470316ab8d`） | `shared_queue::queue()` / `backend_queue()` + `queue_kind` 区分 | 单提交通道 ⇒ 前端流把后端挡住。**并发度是宿主属性，不是设备属性**（见 R11） | 多 stream / 多 queue / 多 context。**必须查证"队列之间保证什么"**（C4 的镜像问题） |
| **C9** | **可观测性**：能给一次提交挂时间戳或完成回调 | 整条"测量 → 归因 → 优化"的循环都建在它上面：lane probe（1067 行）、lane clock、`arm_gpu_time()` | `ocudu_metal_lane_probe.{h,mm}`、`ocudu_metal_lane_clock.h`；`arm_gpu_time`（注释："Metal requires a completed handler to be installed **before** commit()"） | 没有它，**换平台时同时失去判据**。§5.8.14/5.8.15 记录过我们的**仪器本身**出过问题（离线 busy split 不能作优化判据、计数器永久关闭）——**仪器要在每个平台上重新建立信任** | vendor profiler + 我们自己的计数器。**两者都要** |

### 2.1 把这九项压缩成一句话

> **我们要的是一台"能和 CPU 共享地址空间、能跑我们自己的 kernel、能一次提交一批、
> 能用整数命名完成、能被观测、并且旁边还有一个独立推理单元"的设备。**
> **这句话里没有一个字提到 Apple。**

### 2.2 C1 的判定细则（最容易含糊的一项）

"统一内存"这个词被厂商用得很松。**判定 C1 成立的最小条件**：

| 判据 | 说明 |
|---|---|
| ① 设备能接受一个**宿主分配的指针**作为 kernel 参数 | 不是"设备内存能被映射到宿主"，而是**反向**：宿主内存能被设备直接寻址 |
| ② **设备写入对该宿主指针之后立即可见**，无需显式 flush/invalidate/barrier | 需要 flush 的是 C1′，不是 C1。它会**多出一个宿主参与点** |
| ③ 分配有可确定的**页对齐与长度**，且能由内部指针反查所属分配 | 这是 `wrap_no_copy()` 与 `describe_aligned_allocation()` 存在的原因；缺了就只能靠拷贝 |
| ④ 分配粒度不会把"同一块内存"切成多个**互不相关的资源对象** | 见 R4：这在没有 hazard tracking 的平台上会变成**不可见的依赖缺失** |

---

## 3. B 层：软件可应用性契约（S1–S6）

**A 层回答"这块硅行不行"，B 层回答"今天行不行"。** B 层是会变的，所以必须**逐条查证**，
不能从规格表推断（这正是竞争力分析 §5.3 的更正内容）。

| # | 适用性维度 | 判据（怎么查） | 2026-09 的状况 | 换平台时要问的问题 |
|---|---|---|---|---|
| **S1** | **驱动与平台使能** | 目标内核上有没有可用驱动；**整个平台**（不只是加速器）使能到什么程度 | Apple ✅；Intel ✅；AMD ⚠️ 仅 STX/KRK；Qualcomm ❌（X1E 支持不完整到整机项目被取消） | 驱动在**我们的**内核版本上吗？旧一代硅还支持吗？ |
| **S2** | **可编程性** | 这套栈允许我们跑**自己的** kernel，还是只能跑厂商预置算子图 | Apple ✅（MSL）；Intel/AMD/Qualcomm 的 NPU **只能跑推理图** | 我们要卸载的模块里，有几个是"自定义计算"而不是"标准算子"？（答案是**全部**——§2） |
| **S3** | **本地模型生成** | 模型能不能**在这块平台上**编译/量化，不必回到另一个 OS | Apple ✅（`coremltools` + `xcrun coremlcompiler`）；**AMD ❌（Linux 上不能生成模型，必须回 Windows）**；Qualcomm ❌ | 迭代一个模型要跨几次操作系统？ |
| **S4** | **引擎选择是一次配置变更，不是一次重写** | 换引擎要不要改模型、改量化、改训练 | Apple ✅（`computeUnits` 一行）；其他平台**每个引擎一套 API** | "让这个模型跑在另一个引擎上"要几天？ |
| **S5** | **可观测性工具** | profiler / 计数器 / 时间戳在**Linux**上可用吗 | Apple ✅（Instruments + 我们自己的探针）；AMD/Qualcomm ⚠️ | 没有 profiler 时，我们自己的计数器够不够定位？ |
| **S6** | **供应链** | 这个栈还在维护吗？旧平台会不会被放弃？ | **AMD 把 Phoenix/Hawk Point 的 NPU 在 Linux 上直接放弃**（竞争力分析 §5.3.3） | 我们的部署周期（5–10 年）内，这块硅还会被支持吗？ |

**⇒ 三条直接后果：**

1. **B 层的缺口会以"工程工期"的形式出现，不会以"技术不可能"的形式出现。**
   所以它**不改变架构**，只改变时间表。
2. **B 层的每一项都有一个"最小可用"版本**，可以逐项关闭：
   例如 S3 缺了，可以接受"在 Apple 上生成、把产物 vendored 进仓库"——**代价是模型迭代变慢，
   不是不能跑**。§7 的 dry-run 要把这些降级路径写清楚。
3. **S2 是最硬的一条。** 一个只能跑推理图的 NPU 替代不了 GPU 在 PHY 里的位置
   （它只能替代 ANE 的位置）。**这一条如果搞错，会把"两个引擎"误算成"三个引擎"。**

---

## 4. 高层设计必须守住的规则（R1–R17）

**每条格式**：**规则** → *我们在哪里学到它* → **违反的后果**。
这些不是风格建议，每一条都对应一次**实测到的故障或一次差点犯的错**。

### R1 —— 抽象做在"车道 / 读者属地"，不做在 "device backend"

*证据*：`metal_vs_cuda_architecture.md` §5/§7——上游 CUDA 那套 device 抽象层被逐条评估，
**8/10 条不满足，其中一条语义反转**。

*后果*：device-backend 抽象会把**平台差异最大的那一层**（调度语义、内存可见性、生命周期）
拉平成一个最小公倍数，于是每个平台都在用不属于它的模型跑；而"一个写者、多类读者、
属地决定路径"这个结构**不随硬件改变**，它才是应该被抽象的东西。

### R2 —— 完成凭据用**值语义**（generation），不用平台对象

*证据*：`ocudu_metal_queue.h` 的 `grid_ready_signal/wait/encode_wait` 三件套。
注释写明为什么不能用命令缓冲对象："at the moment a consumer asks, the buffer may still be
open in the lane (encoded into, not committed), so `waitUntilCompleted` would be **invalid**."

*后果*：用平台对象做凭据时，"**等待一个还没提交的东西**"这件事**无法表达**。
而 D1 交接（把未提交的命令缓冲交给消费者）恰恰需要它。

### R3 —— 顺序令牌必须绑定到**生产者-消费者对**，不能绑定到"队列上最新的东西"

*证据*：Q9-C（`ocudu_metal_queue.h`）。车道 burst 曾等"创建时最新的 generation"；
两个车道线程下，那个"最新"可能是**另一条车道**的 estimator，而它的 signaller 在这条**串行**队列里
排在等待者**之后** ⇒ 双方互等。空中实测：`commit->start = 5.0028 s`，`start->end = 1.24 ms`
——**耗时全在队列排队，不在设备**——同时接收池被抽干、射频停摆（腿 `p09-conc2`）。
修法：等**自己这一跳**的 generation，并用 `own` / `newest` / `cross_lane` 三个计数器把
"旧规则本来会等一个外来 generation"这件事**变成可读的数字**。

*后果*：跨实例共享的"最新值"是一个**隐藏的全局耦合**。它**在单实例测试里永远不会出现**，
只在并发腿上以"偶发停顿"的形式出现——**这是最难查的一类缺陷**。

### R4 —— 别名内存的"资源身份"要由我们在**地址层**规范化

*证据*：`shared_queue::wrap_no_copy()` 维护**进程级** address→buffer 缓存，并支持 **containment 查询**
（一个落在更大映射内的请求返回那个映射 + 非零 offset）。
注释给出理由："Metal relates memory accesses (hazard tracking, memory barriers) through the
**resource** they are bound to, so two engines that wrap the same memory separately would leave the
producer/consumer dependency between their stages **invisible to the driver**."

*后果*：当平台把 hazard tracking 挂在**资源对象**而不是地址上时，两次独立包装 = **依赖不可见**。
失败模式是**偶发读到旧数据，不报错**。修补必须发生在地址层，且必须是**进程级单例**。

### R5 —— 一个提交 + 显式屏障，优先于多个提交 + 更多栅栏

*证据*：`shared_burst`（`ocudu_metal_burst.h`）：一个命令缓冲编码 burst 的全部 dispatch，
在**pipeline 变化处**插显式内存屏障；对照是每阶段一次提交。
结构面的收益是实打实的：`cbs/lane` **2.70 → 2.00**（5.9.94 空中 A/B）。

*后果*：**提交次数是平台无关的成本，栅栏是平台相关的机制。** 优化前者比堆后者稳；
堆栅栏会把"平台 A 的同步原语"写进设计，而那个原语在平台 B 上可能**语义不同**。

### R6 —— 平台缺的能力，变成"宿主的参与点"并**计数**，不要藏

*证据*：ICB 在本平台被阻断（darwin 25 / AGX G16X 实测）⇒ 被迫 **CPU 编码 + 单命令缓冲 + event 依赖**。
我们把每一次宿主接触都做成了**可读的数字**（`phy_pipeline_crossings`）。
判据（设计文档 §1.2 用户裁定原文）：

> **"一旦流开始，CPU 只能在出口等，中间的每一次接触都算——无论它搬的是样本、标量还是索引表。"**

*后果*：把不可移除的宿主接触**变成计数器**，它就从"架构缺陷"变成"**待办项**"；
藏起来它就会变成"我们以为已经 offload 了"。**这条判据本身是平台中立的**——
它说的是"CPU 的参与点在哪里"，不是"用了哪个 API"。**这是本设计最可移植的一块。**

### R7 —— 判据在**所有平台**注册；探针缺席时报"不适用"，而不是**不出现**

*证据（正例）*：`phy_pipeline_contract.h` 用三态 `std::optional<bool>`：`true`/`false`/`nullopt`，
且注释写明"a contract with nothing to check is **not evidence of anything**"。
`phy_pipeline_mode_registry::is_published()` 让单测直接返回 `nullopt`，而不是假装通过。

*证据（反例）*：`ENABLE_METAL_STATS` 默认 **OFF** ⇒ 依赖它的那条检查
（"host sample assembly"）在默认构建里**根本不注册**（5.9.122 审计）。
于是"8 条检查"在默认 Linux 形状下**不是 8 条**。

*后果*：**一个"不注册"的检查和一个"通过"的检查，在日志里长得一样。**
这是最危险的一种绿色，也是换平台时最容易踩的坑——
**你会同时失去后端和判据，而日志仍然全绿。**

### R8 —— 静默回退是缺陷

*证据（三处）*：
① `upper_phy_factories.h:384` 的 `\c metal:` 写着 "with a **transparent per-topology fallback**
to the CPU implementation"；
② 契约自己的注释记录了那条失败腿（S-7f-6a）："**device=0 with the host silently covering for it**"
——这正是 `mode=cpu_gpu` 那条检查存在的理由；
③ commit `7da6b2d953`：`run_leg.sh` 现在**拒绝**一条设备 kernel 缺失的 `gpu` 腿，
因为"the engine falls back to the host paths **SILENTLY** and the leg still calls itself gpu"。

*后果*：**回退合法，不可观测的回退不合法。** 换平台时，"这个模块没跑在设备上"
必须是一条**报错或一个计数器**，不能是"结果看起来对"。

### R9 —— 配置与标识符按**算法 / 能力**命名，不按**厂商 API** 命名

*证据（正例，照这个做）*：
- `helena` —— 按**算法/模型**命名。搬到 ONNX / 别的 NPU 上，这个名字**仍然是对的**；
- `resource_grid_device_view` —— 按**能力**命名，且注释把术语定义死了：
  **"'Device' here means 'addressable by the accelerator', not 'a different memory'"**；
- `phy_pipeline_mode::{cpu, cpu_gpu, gpu}` —— 按**编排形状**命名，不按后端 API 命名。

*证据（反例，要改的）*：见 §6.2 的泄漏清单——`port_channel_estimator_algorithm::metal_mmse` /
`metal_nn_mmse`（`port_channel_estimator_parameters.h:32,35`）、
`pusch_channel_equalizer_backend = "metal"`（`upper_phy_factories.h:383`）、
`lower_phy_configuration.h:65`、`du_low_config.h:110`，以及
`factories.cpp:44` 里写死的字符串 `"only available on Apple Silicon macOS builds"`。

*后果*：一个叫 `metal_mmse` 的枚举值如果哪天在 CUDA 上跑，**它就在说谎**；
而 YAML 里那个词是**用户可见**的，改名是**破坏性变更**——
于是这个命名债会**随部署规模一起变贵**。**现在改是零成本，发布后改是迁移成本。**

### R10 —— 度量口径跟着编排形状走；形状一变，**每个标签都要重新推导**

*证据*：`ocudu_metal_burst.h` 的 stage label 段。平台给不出比命令缓冲更细的时间戳，
所以一个缓冲只能标一个 stage；合并路由下沿用 `equalizer_demapper` 这个标签，
让 `eq_demap` 变成了**四个阶段之和**——**恰好就是延迟优化正要瞄准的那个数**（5.9.61）。
修法："Whoever makes a buffer carry more **says so here, at the point where it does**."

*后果*：编排一变，**旧标签下的历史数字不再可比**。继续画曲线等于用两个不同口径做趋势判断。
这条对换平台尤其致命：**新平台的编排形状几乎必然与旧平台不同。**

### R11 —— 提交上下文是**线程局部**的（并发粒度是宿主属性，不是设备属性）

*证据*：`ocudu_metal_burst.h`："The state is thread local **on purpose**: the stages of one
demodulation run on the same thread, while several demodulations run at the same time on different
threads and **must not share a command buffer**."

*后果*：把提交上下文做成全局的，会在并发腿上以"**另一个槽的命令混进本槽**"的形式失败。
而并发的**实际**粒度由宿主决定（本仓库实测车道有效 PUSCH 并发 = 1，commit `470316ab8d`），
**不能从设备规格推断**。

### R12 —— 数值判据用"**容差 + 决策级**"，不用逐字节

*证据*：设计文档 §1.5（用户裁定：数值不必逐字节复现）+ §1.6 的适用范围表：
**结构性改动**用**穿越计数**判（数值本来就该不变），**上报量**（`rsrp`/`snr`/`ta_us`）
用"**逻辑正确 + 精度足够**"判——因为它们喂链路自适应选 MCS，**精度不足会让 MAC 选错 MCS**（吞吐掉，但 BLER 曲线不动）。

*后果*：逐字节判据把**累加顺序**这一**平台属性**变成了**正确性条件**，
于是任何平台更换都会表现为"大面积回归"，把真正的缺陷淹没。

### R13 —— 能力用**运行时协商**，不用编译期平台判断

*证据（正例）*：`dft_processor_grid_write::supports_grid_write(view)`；
`make_resource_grid_device_view()` 在存储**不是页对齐**时返回**无效视图**，调用方继续走宿主
（注释："a device engine cannot map it without a copy, so its writers keep running on the host"）。

*证据（反例）*：顶层 `CMakeLists.txt` 用 `if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")`
决定五个后端的默认值。

*后果*：编译期平台判断把"**这块板子行不行**"这个**运行时**问题提前固化，
于是同一平台家族里能力不同的 SKU（或驱动版本不同的机器）只能靠**重编译**区分。

> **⚠ 但要保留反例里的好习惯**：同一段 CMake 在非 Apple 平台上把
> `ENABLE_METAL_*` **显式拒绝**（`message(FATAL_ERROR)`），注释写明理由：
> "Enabling one anywhere else used to be accepted silently and then ignored… the build reported
> success while the requested backend was never compiled in."
> **这是 R8 在构建期的正确应用。** 目标不是删掉这个判断，而是把它从**平台名**换成**能力探测**。

### R14 —— 平台缺的能力要有一条**可查询的**降级路径，且降级的**次数**要可读

*证据*：`shared_queue::notify_wrap_misaligned()`。绑定的**元素**对齐（`float2` 要 8 字节）
在**切片**上无法保证（基址的页对齐由分配保证，切片的元素对齐不是），
引擎必须**改为拷贝**——"and **say so here** - this counter is part of the 'zero-copy wraps' contract check"。

*后果*：零拷贝的失败模式是"**看起来对、只是慢一点**"。不计数就**永远发现不了**，
而它会安静地吃掉整个 §2.4 的架构优势。

### R15 —— 契约与判据必须与平台解耦，并且**在每条腿上自动生效**

*证据*：`phy_pipeline_crossings.h` 是 **519 行纯 C++**，零 Metal，
只用 `<atomic>/<cstdint>/<cstdio>/<cstring>/<mutex>`；
`register_phy_pipeline_check` + `std::atexit` 让它在**每个**进程退出时自动打印。
`phy_pipeline_mode.h` 里 `cpu/cpu_gpu/gpu` 三个名字里**没有一个是厂商名**。

*后果*：判据挂在某个平台的探针上，**换平台就同时失去判据**——
而**失去判据比失去性能更贵**：性能可以用新的后端赢回来，判据要从头建立信任。

### R16 —— 头文件的**作用域/包含约束**也是接口契约的一部分

*证据*：`phy_pipeline_crossings.h:192-195` 明确记录了约束的**理由**：
"this header is included from translation units that sit **INSIDE a namespace**, where pulling in
`<string>/<vector>/<algorithm>` makes libc++ fail with errors like
\"no template named 'basic_ostream'\". Keep this header to `<atomic>/<cstdint>/<cstdio>/<cstring>/<mutex>`
and no more."

*后果*：这类约束**不是风格问题，是"能否被包含"的问题**。它会在**换工具链**时
以编译错误的形式出现——所以在写下它的那一天就要把理由写在旁边。

### R17 —— 平台挡住一条捷径时，选**可移植的那条**，并把失去的收益记成"加分项"而不是"缺陷"

*证据*：ICB（indirect command buffer，让 GPU 用预录制命令重编码命令缓冲）在本平台被阻断
⇒ 被迫采用 **CPU 编码 + 单命令缓冲 + MTLEvent 依赖**。
**这个形状在 CUDA 上同样成立**（一条命令缓冲 + event 排序）。
而它放弃的收益是**可量化的**（§6.3：LDPC 分层解码 290 dispatch/解码 ≈ **550 µs** CPU 编码；
CE 主机耗时段 ≈ **204 µs/跳**）——**几百微秒量级，不是数量级**。

*后果*：押在专有捷径上的调度层是**不可移植的**，而且它的收益通常**远小于**它带来的锁定。
反过来：**约束倒逼出的方案更可移植**，所以约束一旦放宽，那是加分项，不是补课。

---

## 5. 参考模式：本仓库已经做对的五个样板

**这一节的目的**：可移植性不是一个抽象目标，它在本仓库里**已经有具体的形状**。
新增模块时**照抄这五个形状**，比重新讨论"要不要抽象"便宜得多。

### 5.1 能力接口样板 —— `dft_processor_grid_write`

`include/ocudu/phy/generic_functions/dft_processor_grid_write.h`

| 特征 | 为什么这样是对的 |
|---|---|
| 它实现的是**上游已有的中立接口**（`dft_processor`），外加一个**可选能力接口** | 端口在**模块接口**这一层，不在"device backend"层（R1） |
| 接口里**没有 Metal**：只有 `resource_grid_device_view`、`span<const cf_t>`、`cf_t` | 新平台实现同一个接口即可 |
| **`supports_grid_write(view)`** —— 能力在运行时查询 | R13 |
| "The engine **never decides** *whether* the chain wants the grid written from the device: **the caller asks for it.**" | **策略在调用方，能力在实现方。** 这条把"平台判断"从设备层移走了 |
| 不支持时："is submitted through the plain entry point, and the caller post-processes it on the host, **exactly as before this capability existed**" | 降级路径**明确**（R14） |
| `submit_grid_write` 里写明了拒绝条件，并接到 `notify_wrap_misaligned()` 计数 | 拒绝**可观测**（R8/R14） |

### 5.2 "设备可寻址"的抽象样板 —— `resource_grid_device_view`

`include/ocudu/phy/support/resource_grid_device_view.h`

- **纯 POD**：`base` + 三个 stride + 三个 extent。零 Metal，零平台类型。
- **术语在注释里被定义死了**——这一句是整个可移植性哲学：
  > **"'Device' here means 'addressable by the accelerator', not 'a different memory'."**
- **能力的判定是运行时的**：`make_resource_grid_device_view()` 在 base 非页对齐时返回**无效视图**，
  调用方据此继续走宿主。
- **它同时说明了 C1 是硬依赖**：有了 C1，`base` 就是网格自己的缓冲，**没有"发布回宿主"这一步**。

### 5.3 "把加速器查不到的事实发布出去"样板 —— `grid_ready_hook` / `slot_hop_plan_hook` / `handover_reap_hook`

`include/ocudu/phy/phy_pipeline_grid_ready.h`

**这三个 hook 解决的是同一类问题，而且是可移植性最强的一类：**
**上层知道一个加速器无法自行发现的事实，必须有一个平台中立的通道把它交下去。**

| Hook | 交下去的事实 | 加速器为什么查不到 |
|---|---|---|
| `grid_ready_hook` | "这个网格有**宿主**消费者（PUCCH/SRS），它还没被生产" | 宿主消费者在设备上**不可见** |
| `slot_hop_plan_hook` | "这个槽有 **K** 个 PUSCH 跳，这是第几个" | 只有上层 PHY 知道（`pusch_pdus`），设备无从推断 |
| `handover_reap_hook` | "接收池空了，现在把没人认领的块提交掉" | 这是**生产者侧的死锁解**，设备看不到池子 |

**共同的设计纪律**（每条都能直接照抄）：

1. **用函数指针 + `install()`，不是直接调用**——理由写在注释里：
   "the implementation lives with the Metal engines, while the consumers live in the upper PHY,
   **which a build without Metal must still link**"。
2. **没有实现 = no-op**，且这个 no-op 恰好等于"这个能力存在之前的行为"。
3. **每次交接都提供计数器**，因为交接**成功时是不可见的**：
   > "A hand-over is **INVISIBLE** when it works: the grid comes out right whether a block was
   > handed over and claimed, or committed by the front end as it always was."
   ⇒ "Anything that means to JUDGE the hand-over therefore has to read these numbers instead of
   **trusting its own output**."
4. **诊断字段带语义**：`evicted_unproduced`（"0 by construction since 5.9.62, so anything but 0
   means that invariant broke"）—— 把**不变式**做成一个可读的计数器，而不是一句注释。

### 5.4 判据的平台中立样板 —— `phy_pipeline_mode` + 三态契约

`include/ocudu/phy/phy_pipeline_mode.h`、`phy_pipeline_contract.h`、`phy_pipeline_crossings.h`

| 特征 | 说明 |
|---|---|
| 模式按**编排形状**命名（`cpu` / `cpu_gpu` / `gpu`） | 三个名字里没有厂商名（R9） |
| 判定是**三态**：`true` / `false` / `nullopt`（不适用，**不进分母**） | R7 |
| 检查由**拥有那些计数器的探针自己登记** | 判据与平台解耦（R15） |
| **申报制**：`declare_reporter("module")` 打在被判定的数字**下面** | "a module NOT listed here is **not covered** by this number"——**检查的范围要跟着检查一起印出来** |
| 命中缓存**不得**调用计数器 ⇒ 稳态是**字面的 0**，不是"很小" | 判据要能区分"0"和"接近 0" |
| 调试接触被**单独计数并打印**，而不是静默剔除 | "'0.00' has to stay **checkable**" |

**⇒ 这一节可以总结成一条元规则**：

> **一个检查如果会夸大自己的范围，比没有检查更糟——因为它会终止搜索。**
> （原文：*"A check that overstates its scope is worse than no check, because it ends the search."*）

### 5.5 引擎选择样板 —— `ai_train/convert_coreml.py` 的四行循环

`lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/convert_coreml.py`

**这是"引擎选择是一次配置变更"的最直白证据，而且它就在仓库里：**

```python
for cu in ['CPU_ONLY', 'CPU_AND_GPU', 'CPU_AND_NE', 'ALL']:
    ml = ct.models.MLModel(out_path, compute_units=getattr(ct.ComputeUnit, cu))
    ml.predict({'input_1': x})
    ts = []
    for _ in range(200):
        ...  # p50 / p95 / p99
```

| 特征 | 为什么它是可移植性的资产 |
|---|---|
| **四个引擎的 A/B 是一个四元素循环** | 同一个模型、同一次转换产物、同一段基准代码。**发现"ANE 这条路可用"的成本因此是一行，而不是一套后端** |
| 比较发生在**转换时**，不在**运行期** | 引擎选择是**离线可判定**的，不要求数据路径支持运行时切换 |
| 结论**反过来影响架构** | 正因为这个循环跑得便宜，才知道 ANE（191 µs）与 GPU（570–620 µs/授权）在这个负载上差一个量级，从而确定了"AI PHY 归 ANE"这条分工 |
| 但它**还没有被平台中立化** | `coremltools` + `xcrun coremlcompiler` 是 Apple 工具链（见 §6.2 D6）。**这个循环是一个"应该被复制的形状"，不是一个"已经可移植的实现"** |

**⇒ 元规则**：
> **如果一个引擎选择需要超过一次"配置变更"才能尝试，它就不会被尝试，
> 于是它的可用性就永远不会被发现。**

---

## 6. 可移植性账本

### 6.1 资产：已经是平台中立的（**不要动它们**）

| 资产 | 位置 | 为什么它是资产 |
|---|---|---|
| 主判据 | 设计文档 §1.2 | 说的是"**CPU 的参与点在哪里**"，不是"用了哪个 API"。**换平台后逐字有效** |
| 模式定义 | `include/ocudu/phy/phy_pipeline_mode.h` | `cpu`/`cpu_gpu`/`gpu`，无厂商名 |
| 穿越计数器 + 契约 | `include/ocudu/phy/phy_pipeline_crossings.h`（519 行）、`phy_pipeline_contract.h`（105 行） | **零 Metal**，纯 C++，`atexit` 自动生效 |
| 交接 hook 三件套 | `include/ocudu/phy/phy_pipeline_grid_ready.h` | 函数指针 + no-op 默认，无平台类型 |
| 设备视图 | `include/ocudu/phy/support/resource_grid_device_view.h` | 纯 POD，"device = 可被加速器寻址" |
| 可选能力接口 | `include/ocudu/phy/generic_functions/dft_processor_grid_write.h` | 运行时可查询的能力协商 |
| 平台兼容层 | `include/ocudu/support/macos_compat.h` + `utils/macos_compat/` | **API 是能力形状的**（`page_size`、`get_monotonic_time_us`、`describe_aligned_allocation`、`posix_realtime_priority_is_enforceable`），平台分支在 `.cpp` 里 ⇒ "business code never needs `#ifdef __APPLE__`" |
| 模块接口 | `dft_processor`、`port_channel_estimator`、`pusch_channel_equalizer` 等上游接口 | **端口就在这里**。新平台实现同一个接口 |

### 6.2 债：今天会让判据 P0 失败的位置

**严重度**：🔴 阻止（改不动就会锁死） / 🟠 真实债（现在改零成本，发布后是迁移成本） / 🟡 表面债（影响可读性）

| # | 位置 | 泄漏了什么 | 为什么影响移植 | 严重度 | 替换形状 |
|---|---|---|---|---|---|
| D1 | `include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h:32,35` | 枚举值 `metal_mmse`、`metal_nn_mmse` —— **平台中立头文件里按 API 命名算法** | 换后端后这两个名字会**说谎**；YAML 里同名，改名是破坏性变更 | 🟠 | 按**算法 + 引擎类**命名：`block_mmse_accelerated` / `block_mmse_matrix_unit`。**对照正例 `helena`（按算法命名，换栈后仍然对）** |
| D2 | `include/ocudu/phy/upper/upper_phy_factories.h:383`、`include/ocudu/phy/lower/lower_phy_configuration.h:65`、`apps/units/flexible_o_du/o_du_low/du_low_config.h:110` | backend 取值 `"metal"` —— **用户可见的配置里带厂商名** | 同上；且它出现在**三层配置**（库 / 低层 PHY / 应用）里，改名要同时改三处 + YAML | 🟠 | `"accelerator"`（或 `"gpu"`），与 `phy_pipeline_mode` 的词汇对齐 |
| D3 | `lib/phy/upper/signal_processors/channel_estimator/factories.cpp:44` | 写死的错误串 `"only available on Apple Silicon macOS builds"` | 错误信息把**能力缺失**说成了**平台缺失** | 🟡 | 说能力：`"requires an accelerator backend with <capability>; this build has none (ENABLE_*_OFFLOAD is off)"` |
| D4 | 顶层 `CMakeLists.txt:48-101` | 用 `APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64/arm64"` 决定五个后端默认值 | 编译期**平台名**判断代替**能力**判断（R13） | 🟠 | 能力探测（编译器/框架/SDK 是否存在），平台名只作为**默认值**的提示。**保留**同处的 `FATAL_ERROR` 拒绝逻辑（那是 R8 的正确应用） |
| D5 | `include/ocudu/support/macos_compat.h` + `utils/macos_compat/` | **名字**是平台名，**API** 是能力形状 | 一个 CUDA/Windows 移植会去 `#include "macos_compat.h"`，这在语义上是错的，会让人误以为它只服务 macOS | 🟡 | 改名 `platform_compat.{h,cpp}` / `ocudu_platform_compat`。**纯改名，零行为变化**——最适合作为一次独立提交 |
| D6 | `lib/phy/upper/signal_processors/channel_estimator/metal/ai_assets/*.mlmodelc` | 仓库里**只跟踪 CoreML 产物**（`model.mil` + `weights/weight.bin` + `coremldata.bin`），**不跟踪**厂商中立的权重要素导出。**已训练的权重只存在于厂商包里** | AI PHY 的**已训练产物**是一个厂商格式。换平台时"权重怎么来"没有答案 | 🟠 | **比看起来便宜**：架构（`ai_train/helena_arch.py`，"HELENA architecture reconstruction"）与训练流程（TF/`tf_keras`）**都在仓库里且是平台中立的**，中间产物 SavedModel 也已经存在；被锁住的只有 `convert_coreml.py`（`coremltools`）+ `run_training.sh:67`（`xcrun coremlcompiler`）这**最后一步**。⇒ 在每次训练时**多导出一份 ONNX**（从同一个 SavedModel）即可，`.mlmodelc` 降级为构建产物 |
| D7 | `CMakeLists.txt:25-28`（`ENABLE_METAL_STATS` 默认 OFF） | 依赖它的那条契约检查在默认构建里**不注册** | 换平台时会**同时失去后端和判据**，而日志仍然全绿（R7） | 🟠 | 让**检查永远注册**，探针缺席时返回 `nullopt`（"不适用"），而不是让检查消失。这正是三态设计本来要支持的用法 |
| D8 | `lib/phy/metal/ocudu_metal_queue.h:29-30` | `#error "ocudu_metal_queue.h is only available to Objective-C++ translation units."` | **这个 `#error` 本身是对的**（它是一道**边界**）；债在于**共享的 queue/burst 基础设施住在 `lib/phy/metal/` 下**，名字暗示它属于 Metal 而不是"加速器提交层" | 🟡 | 目录/目标改名（`lib/phy/accelerator/`），或至少在文档里把这一层命名为**提交层**。**行为不变** |
| D9 | 每个模块各有一份 `lib/**/metal/` 子目录，而它实现的接口在 `include/ocudu/phy/**` | **端口位置只隐含在目录约定里**（"替换 `*/metal/`"），没有一份显式契约说明"最小替换集合是哪些文件、哪些是共享基础设施不能一起换" | 新平台不知道 `lib/phy/metal/`（queue/burst/probe，3951 行）是**共享提交层**而不是某个模块的实现 | 🟡 | 本文 §7 的六步清单即为该契约的初版；可进一步落成 `lib/phy/metal/PORTING.md` |

**⇒ 关于这张表的三点观察：**

1. **这张表里没有 🔴。** 唯一的阻止项在 §6.3（A 层缺项），而**那些不是"债"，是"换平台的前提"**——
   债是可以还的，前提是必须满足的。
2. **九条债里有七条是 🟠/🟡 的命名、注册与工具链问题，每一条都有零行为变化（或接近零）的替换形状。**
   这说明**架构本身已经足够中立**——剩下的债**主要是词汇债**，
   而词汇债**只随部署规模变贵，不随代码规模变贵**。
3. **D1–D5、D8 加起来是一次纯粹的"改名 + CMake 探测"提交，不含任何逻辑改动**（D6 是加一行 ONNX 导出）。
   这是可移植性投资里**性价比最高的一笔**，而且**现在做是零风险的**。

### 6.3 阻止项（A 层缺项 —— 不是债，是**换平台的前提**）

| 缺项 | 后果 | 现状 |
|---|---|---|
| **缺 C1（统一可寻址内存）** | §2.4 的"路由边际成本为零"失效 ⇒ 整个异构路由的前提失效 ⇒ **不是移植，是重新设计** | 候选平台里 NVIDIA 是离散语义（`metal_vs_cuda_architecture.md` §2.3）；**GB10 的相干统一内存是否满足 §2.2 的四条判据：未核实** |
| **缺 C2（可编程设备）** | 只剩 AI PHY 一条腿（见 §3 的 S2） | Intel / AMD / Qualcomm 的 **NPU** 都是推理图加速器 |
| **缺 C5（值语义完成凭据）** | 宿主参与点无法消除 ⇒ 主判据达不到；**但程序照样跑**，所以这个缺失**不会自己暴露** | 上游 CUDA 的 `cuda_event` 就是这一项的部分缺失（只有"等 / 完成没"） |
| **缺 C6（独立推理引擎）** | AI PHY 挤进 GPU 队列 | 见 C6 行 |

---

## 7. 移植 dry-run：换一块平台的六步，与每一步的判据

**这一节把上面的规则变成一个可执行的顺序。** 每一步都有一个**可判定**的完成条件——
不允许用"看起来能跑了"代替。

| 步 | 做什么 | **完成判据（可判定）** | 常见错误 |
|---|---|---|---|
| **1** | **填 A 层问卷**（§2 的 C1–C9），逐条给出 Yes/No/部分，并注明**出处** | 九项各有结论；C1 按 §2.2 的**四条判据**逐条判定（不是"支持统一内存"一句话） | 用厂商的营销词代替判据；把 C1′（需要 flush）当成 C1 |
| **2** | **填 B 层问卷**（S1–S6），并为每个 ❌ 写出**降级路径** | 每个 ❌ 都有一条"接受它时的代价"（例如 S3 缺 ⇒ 在 Apple 上生成模型、产物 vendored 入库） | 把 B 层缺项当成架构阻止项（它是工期）；或反过来，**忽略 S2**，把只能跑推理图的 NPU 当成通用加速器 |
| **3** | **先跑 `mode=cpu`，把契约的空转面看清楚** | `[phy_pipeline] contract` 打印出**全部**已注册检查，其中后端相关的报 `not applicable`（**不是消失**）。若某条检查**根本没出现** ⇒ 先修 R7/D7 | 直接开始接后端；于是永远不知道"哪些判据在新平台上真的生效了" |
| **4** | **只接一个模块**（建议 DFT） | `dft_processor` 的中立接口 + `dft_processor_grid_write` 的可选能力接口实现完毕；单元测试在**两条路**上跑同一套判据（R12） | 一次接整条车道；失败时无法归因 |
| **5** | **第一天就打开穿越计数** | `mode=gpu` 下 `crossings` 行**有数字**（可以很大），且**每个读都有名字**（"every read has to be attributable"）。这不要求达标，只要求**可读** | 等"优化完了再测量"。**穿越计数是唯一平台中立的判据，必须先于优化存在** |
| **6** | **建立新平台的可观测性**（C9） | lane probe / 时间戳在新平台上产出**与旧平台同口径**的数字（R10：先确认口径，再比数值） | 直接比数值 ⇒ 用两个口径画同一条曲线 |

### 7.1 预计必须重写 / 可以原样保留

| 部分 | 判断 | 依据 |
|---|---|---|
| `*/metal/*.mm`、`*.metal` | **必须重写** | 这就是 C 层 |
| `include/ocudu/phy/phy_pipeline_*.h` | **原样保留**（除非 D1/D2 未先修） | 519 + 105 + 304 行纯 C++，零 Metal |
| `resource_grid_device_view`、`dft_processor_grid_write` | **原样保留** | 纯 POD / 纯虚接口 |
| `phy_pipeline_mode` | **原样保留** | 按编排形状命名 |
| `macos_compat` 的 **API** | **原样保留**；**名字**建议先改（D5） | 签名已是能力形状 |
| AI 模型 | **取决于 D6 是否先修** | 只跟踪 `.mlmodelc` ⇒ 无法重生成 |
| 判据、契约、探针的**口径** | **原样保留**；**实现**按平台重写 | R15 |

---

## 8. 反面清单（明确不要做）

| ❌ | 为什么 |
|---|---|
| **写一个"hardware-neutral device backend"抽象层** | 上游 CUDA 那套被逐条评过：**8/10 条不满足，一条语义反转**（`metal_vs_cuda_architecture.md` §5）。它会把平台差异最大的那一层拉平 |
| **让判据跟着后端走** | 换平台时**同时失去判据**，而失去判据比失去性能更贵（R15） |
| **把"最新"当"正确"** | R3。全局"最新值"是隐藏耦合，单实例测试永远测不出，只在并发腿上偶发 |
| **用平台名命名算法 / 配置 / 库** | R9。**现在改零成本，发布后是迁移成本** |
| **让回退静默** | R8。回退合法，**不可观测的回退不合法** |
| **从 `full_gpu_chain/` 继承结论** | 该目录 README 自己的警告：**"不要从旧目录继承结论，只继承证据。"** |
| **把编译期的平台判断当成能力判断** | R13。同平台家族里能力不同的 SKU 只能靠重编译区分 |
| **用"绿色构建"推断可移植性** | 一条绿色 macOS 构建**不是**关于 Ubuntu 的证据（GCC-only 的 `-Werror=shadow`）；反过来也一样 |
| **把设备规格当成并发/顺序的保证** | R11。并发的实际粒度由宿主决定（本仓库实测车道并发 = 1） |
| **在换平台时"顺便"调数值判据** | R12 + R10。**先把口径对齐，再比数值**；否则新旧数字不可比 |

---

## 9. 诚实清单

### 9.1 尚未证明

| 项 | 现状 |
|---|---|
| **本文的判据 P0（只改 `*/metal/`）今天是否成立** | **今天不成立**——§6.2 列出的 D1/D2 会让它失败。**这是一个"已知会失败但可枚举"的判据，不是一句口号** |
| **候选平台的 C1 是否成立** | NVIDIA GB10 的"CPU–GPU 相干统一内存"**是否满足 §2.2 的四条判据：未查到一手出处【待核实】** |
| **Intel NPU 的可编程性边界** | 已知是推理图加速器（OpenVINO 模型）；**能否承载非推理型的自定义算子链未核实【待核实】**（决定它能否替代 GPU 而不是只替代 ANE） |
| **Strix Halo 在 Linux 上 NPU 之外的能力** | AMD 的 Linux 文档只说 NPU-only 流，**没说 iGPU 计算在 Linux 上能不能用**【待核实】——这决定"统一内存 + 可编程 GPU"两条在 x86 上是否同时成立 |

### 9.2 尚未解决

| 项 | 问题 |
|---|---|
| **D6（模型真源）的迁移成本** | 需要一个"训练 → ONNX → 各平台产物"的流水线，而训练脚本目前依赖 macOS 工具链。**这是 B 层 S3 的具体工程面** |
| **改名类债（D1/D2/D5/D8）的时机** | 单独一次提交、零行为变化，但**会动用户可见的 YAML 词**（D2）⇒ 需要一个迁移说明，或在下一个破坏性窗口做 |
| **同一套单元测试在两条路上的运行方式** | 目前 Metal 测试在 `*/metal/test/` 下且受 `ENABLE_METAL_*` 门控。要做成"两条路跑同一套判据"，需要一个平台中立的测试参数化 |

### 9.3 本文的适用范围

- 本文的**规则**（§4）来自本仓库的**实测与故障**，因此**在换平台后逐条仍然有效**——
  它们约束的是**我们的设计**，不是平台。
- 本文的**账本**（§6）是**有日期的**。它随提交变长变短，**应以 `git log` 为准**，
  不要把它当成永久结论引用。
- 本文**不评价**任何候选平台的性能。那是 `apple_silicon_competitiveness_analysis.md` §9 那场实验的事。
