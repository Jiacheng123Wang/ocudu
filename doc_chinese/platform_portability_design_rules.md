# 平台可移植性设计要点 —— 依赖的是"所需的硬件能力 + 软件的可应用性"，不是 Apple Silicon / macOS

> 状态：设计约束文档（**v1.1**，2026-09-25）
> **v1.1 的改动**：①能力契约由 C1–C9 扩到 **C1–C13**（新增顺序语义、可恢复性、测量可复现性、数值范围、严格 IEEE 选项）；
> ②设计规则由 R1–R17 扩到 **R1–R24**（新增生命周期与安全）；
> ③新增 **§4.5 证据规则**（15 条）与 **§6.4 语义泄漏**（12 条 🔴）；
> ④**自我更正**：R5 原先写得太宽，提交内屏障**只对同一句柄有效**，别名对上六种原语全部被证伪。
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
> | **A 层：硬件能力** | 统一可寻址内存、可编程设备、**必须查证的顺序语义**、**值语义的完成凭据**、推理引擎、**可恢复性**、**测量可复现性**、数值范围……（§2 的 **C1–C13**） | **五家商品硅在"有没有硬件"这一问上成立**（`apple_silicon_competitiveness_analysis.md` §5.3.2）；**但 C4/C10/C11/C12/C13 五项在 Apple 上是负分或不合格**——见 §2.1 |
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

**这条判据的价值在于：它今天就是失败可判的——而且它今天就失败，失败点已经全部列出。**
账本分两半，**性质完全不同**：

| 账本 | 条目 | 性质 | 修法 |
|---|---|---|---|
| **§6.2 命名与注册债** | **16 条**，无 🔴 | 只随**部署规模**变贵；改名即可 | 其中约 8 条合计是**一次零行为变化的提交** |
| **§6.4 ★ 语义泄漏** | **12 条，全部 🔴** | `#if` 改变的不是"编哪个实现"，是"**程序是什么意思**" | 不能靠改名；**其中一条已经让判据在默认构建里不存在** |

**⇒ 这是本文 v1.1 最重要的更正：**

> **可移植性的主要障碍不是词汇，是语义。**
> 词汇债会让你**多花几天**；语义债会让你**在日志全绿的情况下失去判据**——
> 而失去判据比失去性能贵得多，因为性能可以用新后端赢回来，判据要从头建立信任。

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
| **C1** | **统一可寻址内存**：设备可直接读写宿主分配的内存，无需显式拷贝 | **竞争力分析 §2.4**"路由边际成本为零"是整个异构设计的地基；设备写者需要一个**页对齐宿主分配**的地址才能寻址 | `resource_grid_device_view`（base + strides）；`page_aligned_allocator`；`compat::page_size()`、`compat::describe_aligned_allocation()` | **每一次路由 = 一次拷贝**，穿越次数从 2 变成 2+K（K = 参与路由的模块数）。**这是唯一会让整个设计前提失效的一项** | 相干互联（NVLink-C2C / CXL）。**⚠ 但必须核实"设备写对宿主可见"是否自动成立**；若需要显式 flush/invalidate，它**不等于** C1，要按 C1′ 记账 |
| **C2** | **可编程设备**：可提交自定义数据并行 kernel（不是固定功能单元） | 链式 kernel 编排；位精确对拍（`metal_mmse` 与 `metal_nn_mmse` 是同一套数学的两个 kernel，用于把差异归因到 kernel 本身） | 每个模块一个 `*_engine`，各自编译自己的 kernel 库 | 只剩"跑厂商预置算子图"的能力 ⇒ **CPU 卸载没了，只剩 AI PHY 一条腿** | 无。固定功能替代不了这一项 |
| **C3** | **提交单元 + 提交内顺序**：能一次提交一批工作，且批内顺序由调用方指定 | 一跳一次提交（`cbs/lane` 2.70 → 2.00）；一个命令缓冲 + pipeline 变化处**显式屏障** | `shared_burst`（`ocudu_metal_burst.h/.mm`）；每个引擎的 `submit()` / `commit()` | 每阶段一次提交 ⇒ CPU 编码开销与提交数成正比（§6.3 量化：LDPC 290 dispatch/解码 ≈ 550 µs + CE ≈ 204 µs/跳） | 无 |
| **C4** | ★ **顺序语义必须查证，而且答案多半是"不保证"**——三个层次都要问：①提交**之间**保证什么；②提交**之内**的屏障是否真的保证生产者→消费者**可见性**；③提交**之内**编码的完成信号是**在完成时**发布还是**在编码点**发布 | ①我们依赖"提交之间只保证开始顺序"这条否定式事实；②**设计文档 §5.9.88/§5.9.89**：**六种提交内排序原语全部被证伪**（scope 栅栏 / 资源栅栏 / 栅栏位置 / encoder 边界 / 线程组类型 / 内核内栅栏），**只有"命令缓冲完成（commit+wait）"真的排得住**——而代价是实测到的脏数据（`A⁻¹` worst **5049** 对干净态 1e-3–2e-2，W **16884/18144** 个元素错）；③**`phy_latency` §6.4.3**：**命令缓冲中途编码的信号只在完成时才可见**（三组探针：97.1 ms 见值 / 97.4 ms 完成；105.8 vs 105.9；77.1 vs 77.0 ms——**GPU 侧等待也没提前满足**），对照组"在第一个 encoder 之前编码"⇒ **2.04 ms** 就可见 | `ocudu_metal_queue.h`（两处 "only orders the **STARTS** of its command buffers"）；`shared_burst` 头注释（"they may overlap"）；`OCUDU_CE_CORR_FENCED`（被证伪后的**每跳一条缓冲**构建） | 把这三条里的任何一条当成"平台保证"，得到的是**静默错数据**（`sinr=37.6 dB` + CRC 全错是它的签名，见 R8 与 L1），**不报错、不可复现、离线测不出** | —（这不是"能力"，是**必须逐条查证的语义**。换平台时**必须重问一遍**，不能沿用） |
| **C5** | **可命名、可跨提交请求的完成通知，且是值语义（不是对象语义）** | 一个 generation 既能被**宿主阻塞等待**，也能被**编码进设备命令缓冲**；而"等待一个尚未 commit 的命令缓冲对象"在平台上**是非法的** | `grid_ready_signal/wait/encode_wait`；`backend_stage_signal/wait_generation`（`ocudu_metal_queue.h`） | **程序照样跑，但宿主的参与点无法消除**——恰好是主判据（§1.2 的"CPU 彻底甩手"）要消除的东西。上游 CUDA 的 `cuda_event` 只有"等 / 完成没" ⇒ **宿主轮询**（`metal_vs_cuda_architecture.md`）。**⚠ 结合 C4③**：即使有信号，它的**可见时刻**也可能被平台推迟到整条提交完成——那么"提前释放"这个设计**在本平台上不存在** | 事件 / fence / 信号量，**只要能用单调整数命名**；纯轮询不可接受。**并且要问：信号最早在什么时刻可见** |
| **C6** | **一个推理引擎**（与通用计算单元**并列**，不是它的一个切片） | ANE 191 µs vs GPU 同类功能块 570–620 µs/授权（竞争力分析 §5.3.4） | Core ML（一个 API 覆盖 ANE/GPU/CPU） | AI PHY 只能挤进 GPU 队列 ⇒ 既慢，又与 PHY 主体争用（`full_gpu_chain/` 风险登记 R8：ANE 与 GPU 争内存带宽） | 任何独立调度的推理单元。**"GPU 的一个 SM 分区"不算**（见 `apple_silicon_competitiveness_analysis.md` 的分区分析） |
| **C7** | **单调时间基 + 可用的线程调度设施** | 实时性判据、lane probe、超时上界（`grid_ready_wait(gen, timeout_ms)`） | `compat::get_monotonic_time_us()`；`darwin_thread_scheduling.h/.cpp`；`compat::posix_realtime_priority_is_enforceable()` | 时延轴无法测量 ⇒ §9 的那场决定性实验**无法在新区块上复现** | 任何单调时钟。**注意 Apple 在 C7 上是"负分"**：`set_thread_affinity` 在 Apple Silicon 上是 no-op（`darwin_thread_scheduling.cpp:82`：`THREAD_AFFINITY_POLICY` 仅 XNU-on-Intel 实现），我们用 QoS + Mach time constraint 顶。**换到 Linux 反而多一项能力** |
| **C8** | **并发宿主提交**：多线程各自提交而不互相阻塞到不可用 | 单队列时"后端排在在飞的 FFT 之后，实测延迟爆炸" ⇒ 必须是**两个队列**；而车道的有效 PUSCH 并发实测是 **1**（一条串行链，commit `470316ab8d`） | `shared_queue::queue()` / `backend_queue()` + `queue_kind` 区分 | 单提交通道 ⇒ 前端流把后端挡住。**并发度是宿主属性，不是设备属性**（见 R11） | 多 stream / 多 queue / 多 context。**必须查证"队列之间保证什么"**（C4 的镜像问题）——本平台实测**跨队列栅栏的价值为 0**（`phy_pipeline_gpu/session_handoff_2026-09-21-1.md` §2：同队列臂 `[ul_dft_wait]` **506.8 → 505.8**），⇒ **需要顺序的东西必须在同一条提交里**。另需查：**在飞提交有上限吗**（本仓库见过队列 in-flight 槽耗尽，`initWithQueue:` 在第 1431 跳阻塞，根因未定位） |
| **C9** | **可观测性**：能给一次提交挂时间戳或完成回调，**且细粒度探针不会打死进程** | 整条"测量 → 归因 → 优化"的循环都建在它上面：lane probe（1067 行）、lane clock、`arm_gpu_time()` | `ocudu_metal_lane_probe.{h,mm}`、`ocudu_metal_lane_clock.h`；`arm_gpu_time`（注释："Metal requires a completed handler to be installed **before** commit()"） | 没有它，**换平台时同时失去判据**。**设计文档 §5.8.14/§5.8.15** 记录过我们的**仪器本身**出过问题（离线 busy split 不能作优化判据、计数器永久关闭）——**仪器要在每个平台上重新建立信任** | vendor profiler + 我们自己的计数器。**两者都要**。**⚠ 细粒度是奢侈品**：本设备**每个 dispatch 的硬件计数器不可能**（`AtStageBoundary` 支持，`AtDispatchBoundary` 不支持，且在 compute encoder 上采样会**断言并打死进程**，SIGABRT）。⇒ 可用的只有**每条提交的起止时间**（`busy split` 就是这么做出来的）+ 隔离微基准 + 成对空中腿 |
| **C10** | ★ **可恢复性**：设备出故障后能不能被复位 | 这是最容易漏掉的一项，因为它在**顺利时完全不可见** | **本平台是不合格的**：GPU 挂死**不可恢复**——无看门狗复位；`kill -9` 释放进程但**不释放 GPU**；**优雅关机路径本身要用 GPU，所以 `reboot` 也会卡死**，唯一出路是强制断电；`recoveryCount=0` ⇒ **macOS 不会自己复位 GPU**（`full_gpu_chain/s2_full_chain_design.md` §48.133 演练 + `phy_pipeline_gpu/wip/S12_incident_gpu_hang_2026-09-19.md`） | 一次内核写错 ⇒ **整机断电**。⇒ 逼出 R20（"内核必须对任意参数终止"）这条**纯平台中立**的安全规则 | CUDA：Xid + `nvidia-smi --gpu-reset` + 抢占超时；Vulkan：`VK_ERROR_DEVICE_LOST` + 设备重建；Windows：WDDM TDR。**候选平台在这一项上大概率优于 Apple——这是加分项** |
| **C11** | ★ **测量可复现性**：这块平台能不能给出**可比较**的数字 | 没有它，§7 的六步 dry-run 会在第 6 步**永远停住**：能跑，但说不出快了还是慢了 | 可用的：**同二进制背靠背成对腿**（B9：本机热降频实测 **1092 µs → 3560 µs = 3.3×**，跨时间对比一律无效）；**空臂**判据（`wip/S14_instruments.md` §12：空臂宽度 **−248.8 … +319.8 µs**，与效应同宽 ⇒ 该时段什么都测不出）；`--repeat 1` + 每进程一跳 + N≥9 + 中位数（`--repeat 20` 实测**双峰**，3/12 偏低 45%） | 把"**绝对 µs**"换成"**比值与份额**"，并且每次都先跑空臂。**这不是可选的方法学偏好，是 C11 缺失的直接后果** | 独占 GPU 的部署环境会直接补上这一项。**⚠ 注意**：本机之所以不可复现，一个重要原因是 **macOS 在进程间分时 GPU，宿主自己的 GUI 就是干扰源**（实测 Chrome GPU 进程 37–44% CPU、WindowServer 32.1%）⇒ **目标部署形态（无 GUI 的边缘节点）本身就在改善 C11** |
| **C12** | ★ **数值范围**：设备端**有没有 fp64**（以及更一般地，能不能表达这个问题的条件数） | 设备端矩阵反演是整条链里条件数最差的一步 | **本平台从语言层面没有**：`'double' is not supported in Metal`（6 处），⇒"用更高精度救 K1"这条路**不存在**。而 `cond₂(A) ≈ 2.1e4`、float32 元素级下限 `cond·eps ≈ 2.5e-3`，"**所有 float32 形式都远在下限之上**"。实测：设备端 blocked GJ **9.67e-1**（−18.69 dB）、float32 Cholesky **2.88e-1**、**float64 Cholesky 4.62e-10** | **算法必须绕开那一步**（本仓库被迫如此） | CUDA 有完整 fp64（1/2–1/64 速率）⇒ **在有 fp64 的平台上，整个设备端反演的绕行方案可以直接删掉**。**这是全套材料里最大的一处能力差，而且是算法性的**。Vulkan `shaderFloat64` / OpenCL `cl_khr_fp64` 为可选扩展 |
| **C13** | ★ **逐 kernel 的严格 IEEE 编译选项**（"告诉编译器不要重结合"这件事本身是一项能力） | 位精确契约、以及任何被条件数放大的量 | 本平台的默认 fast math **改变了算术**：`ocudu_mmse_pilots_power.metal:18-21` 记录**同一次除法在 298151/2²⁰（28.4%）个输入上与宿主不同**，**在 27/27 条语料上翻转 LLR 判决（32613 个软比特）**；加 `-fno-fast-math` 后同一扫描 **0/2²⁰**——"**在这个 GPU 上，那个编译选项就是全部差别，不是硬件**" | 数值敏感的 kernel 必须能**逐个**声明严格语义；做不到就只能整体放弃位精确契约 | 任何有 per-TU / per-kernel 编译选项的平台都满足（CUDA `-use_fast_math` 是 opt-in，Vulkan `RelaxedPrecision` 是 opt-in，OpenCL `-cl-fast-relaxed-math` 是 opt-in）。**⚠ Apple 是唯一"默认宽松"的一家**⇒ 这条在候选平台上大概率**更省事**。构建机制照抄 `cmake/modules/ocudu_metal.cmake:14-20` 的按源文件 `-fno-fast-math` |

### 2.1 把这十三项压缩成一句话

> **我们要的是一台"能和 CPU 共享地址空间、能跑我们自己的 kernel、能被我们判断出它保证了什么顺序、
> 能用整数命名完成、出故障能被复位、能被观测、测出来的数字能互相比较、
> 并且旁边还有一个独立推理单元"的设备。**
> **这句话里没有一个字提到 Apple。**

**⚠ 两处反直觉的地方，值得单独记住：**

1. **C4 和 C11 是"否定式能力"**——它们不是"平台能做什么"，而是"平台**不保证**什么、**测不准**什么。
   这两项在顺利时**完全不可见**，但它们是**每一次静默错数据和每一次错误的性能结论的来源**。
   **换平台时，回答"它不保证什么"比回答"它有什么"更重要。**
2. **Apple 在 C4③、C7、C10、C11 四项上是"负分"**，而 Linux 在 C7（线程亲和）和 C11（独占 GPU）上
   **直接更优**。⇒ **"移植"不是单向下坡**：候选平台在若干项上会**比现状更好**，
   所以 §7 的 dry-run 要同时记录"新平台多给了什么"。

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

**为什么"按地址做 containment"还不够——三个实测到的坑：**

| 坑 | 证据 | 含义 |
|---|---|---|
| **分配器的块粒度会打败按地址的身份判定** | `full_gpu_chain/s2_full_chain_design.md` §48.27：`compat::aligned_alloc()` 就是 `posix_memalign`，**每次调用只保证请求的那一段** ⇒ "同一个逻辑分配（`temp_llr` / `temp_eq_re`，各 448 KiB）在 malloc 看来是**几十个互不相干的 16 KiB 块**" | 身份必须**在分配时登记 (base, size)**，按**分配**做键，不能靠地址包含推断 |
| **映射可能活过它的分配**，同一地址被复用后得到**尺寸不同的陈旧映射** | `full_gpu_chain/session_handoff_2026-09-17-2.md` §2.2 + `s2` §48.176(c)：`[wrap_create] ptr=0xaf074c000 alloc_size=229376` 之后同一地址变成 `alloc_size=458752`（"先是一个 224 KiB 的分配、被 free，再被一个新的 448 KiB 分配复用"）；修法 = **free-observer**（空中实测 `purges=7412`） | 需要一个"分配被释放 ⇒ 失效所有派生句柄"的**通知**，否则缓存会交给设备一段已经不属于你的内存 |
| **"命中"也可能是一次分配** | `full_gpu_chain/session_handoff_2026-09-13-2.md` §5.3：`buffer_cache.emplace` **不覆盖已有 key** ⇒ 请求尺寸一旦超过缓存尺寸就**新建 MTLBuffer**；空中单次运行 **6492 条** `zero-copy cache hit with a larger request`，每跳一次 Metal 分配 ⇒ **+60 µs/PUSCH** | "缓存命中"这句话在两类计数器里**意思不同**：对**穿越计数**它是"免费"，对**时延计数**它可能是一次分配。**两者必须分开统计**（见 R26） |

*后果*：当平台把 hazard tracking 挂在**资源对象**而不是地址上时，两次独立包装 = **依赖不可见**。
失败模式是**偶发读到旧数据，不报错**。修补必须发生在地址层，且必须是**进程级单例**——
**但"地址"必须是"登记过的分配"，不是"某次 wrap 调用的指针"。**

**⇒ 两条由此派生的接口纪律：**

1. **句柄必须带着 offset 走完全程。** `wrap_no_copy` 会返回非零偏移，而引擎的 `wrap_shared()` 曾经
   **把它丢掉**——那会是一次"绑到映射基址"的**静默错址**（设计文档 §5.8.5 顺带发现；
   §5.9.57 改成**拒绝**而不是警告，新计数器 `wrap_shared_refused` **必须为 0**，涉及 9 个调用点）。
   ⇒ **凡拒绝，必须计数；凡计数为 0，必须能证明它测到了东西。**
2. **交给设备的是 `{BASE, 槽号}`，不是元素地址。** `full_gpu_chain/session_handoff_2026-09-18-3.md` §2.3：
   第一版交 `&gpu_ls_sigma2[2]` 而核里又读 `[2]` ⇒ 越界读；核新写 `out[2]/out[3]` 但
   `wrap(..., 2 * sizeof(float))` 只给 8 字节 ⇒ **症状不是崩**，是 CE 单测在 SNR 20 dB **退 3.94 dB**、
   `nv/l²` **漂 18 倍**。⇒ **"写到包装长度之外"是静默数值污染，不是故障。**

### R5 —— 一个提交 + 显式屏障，优先于多个提交 + 更多栅栏 —— **⚠ 但这条有一个必须写明的条件**

*证据*：`shared_burst`（`ocudu_metal_burst.h`）：一个命令缓冲编码 burst 的全部 dispatch，
在**pipeline 变化处**插显式内存屏障；对照是每阶段一次提交。
结构面的收益是实打实的：`cbs/lane` **2.70 → 2.00**（5.9.94 空中 A/B）。

**⚠ 条件（v1.1 补充，这条曾被我写得太宽）**：**提交内的屏障只对"同一个资源句柄"的访问有效。**
一旦生产者与消费者**各自包装了同一段内存**，提交内的屏障**在六种原语下全部失效**
（scope 栅栏 / 资源栅栏 / 栅栏位置 / encoder 边界 / 线程组类型 / 内核内栅栏；
设计文档 §5.9.88–§5.9.89），唯一排得住的是**命令缓冲的完成边界**：

| 形状 | 提交内屏障 | 提交边界 |
|---|---|---|
| **同一句柄**上的生产者 → 消费者 | ✅ 有效（`shared_burst` 的存产线形状） | ✅ |
| **别名对**（两个句柄、同一段内存） | ❌ **六种原语全部证伪** | ✅（代价：多一次提交） |

失效时的表现是**静默错数据**：`A⁻¹` 的 worst 从干净的 1e-3–2e-2 变成 **5049**，
W 有 **16884 / 18144** 个元素错，`nv` 差 128 倍。**没有报错。**

**⇒ 所以 R5 的正确表述是两条一起用，缺一不可：**

> **① 先把句柄唯一化（R4），提交内的屏障才是有效的；**
> **② 凡是不是同一句柄的依赖，都必须升级到提交边界，并为它付提交数。**
> **③ 付了之后要想办法赚回来**——`OCUDU_CE_CORR_FENCED` 的代价实测 **+39.5 µs/跳 = +17.7%**，
> 这是"每跳一条缓冲"的明码标价，不是意外。

*后果*：**提交次数是平台无关的成本，栅栏是平台相关的机制。** 但"用一个屏障把两件事排起来"
这个动作**只有在句柄唯一的前提下才成立**——把它当成通用手段，会得到一个
**只在别名对出现时失败**的缺陷，也就是最难查的那一类。

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

*证据（反例一）*：`ENABLE_METAL_STATS` 默认 **OFF** ⇒ 依赖它的那条检查
（"host sample assembly"）在默认构建里**根本不注册**（5.9.122 审计）。
于是"8 条检查"在默认 Linux 形状下**不是 8 条**。

*证据（反例二，三态设计自身的失败模式）*：三态允许"不适用"，但**不适用会缩小分母**。
`phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` §4.4：
一条**没有 PUSCH 跳**的腿让 `ce device estimates` 与 `host sample assembly` **退出 applicable 集合**，
于是打印成 `MET (6 of 6 checks applicable)`——**看起来全绿**，
而里程碑判据要的是 `MET (8 of 8)` ⇒ **直接 FAIL**。

*证据（反例三，空转的绿）*：多处记录过"门绿了很久、但测的不是被测对象"：
`ctest -L phy 162/162` 绿而**信道估计器单测根本没注册**；
某 CE 单测的 grid **没有 device view** ⇒ `device_sigma2=0`，是**空转绿**；
某抓包门 `device-built-captures=258/980`（其余 722 个过去是**静默的主机 vs 主机**）。

*后果*：**一个"不注册"的检查和一个"通过"的检查，在日志里长得一样。**
这是最危险的一种绿色，也是换平台时最容易踩的坑——
**你会同时失去后端和判据，而日志仍然全绿。**

**⇒ 三条可执行的补充：**

1. **分母必须被断言，不只是被打印。** `MET (N of M)` 里的 **M 是期望值**，
   换平台时"applicable 数变少"必须是**红**，不是"更简洁的报告"。
2. **每条路径都要一个"它到底跑了几次"的计数器**，并在判据里断言**非空转**。
3. **判据必须同时绑定三个维度：regime（模式）、traffic（流量）、geometry（几何）**
   ——否则"这条腿没触发"会被读成"这条腿通过了"。

### R8 —— 静默回退是缺陷

*证据（三处）*：
① `upper_phy_factories.h:384` 的 `\c metal:` 写着 "with a **transparent per-topology fallback**
to the CPU implementation"；
② 契约自己的注释记录了那条失败腿（S-7f-6a）："**device=0 with the host silently covering for it**"
——这正是 `mode=cpu_gpu` 那条检查存在的理由；
③ commit `7da6b2d953`：`run_leg.sh` 现在**拒绝**一条设备 kernel 缺失的 `gpu` 腿，
因为"the engine falls back to the host paths **SILENTLY** and the leg still calls itself gpu"。

*证据（最尖锐的一例——**静默降级比"探针说谎"更难发现**）*：
`full_gpu_chain/session_handoff_2026-09-17-2.md` §3.5 记录了内核查找顺序是
"编译期烧入的绝对路径 → 可执行文件旁 → cwd"，**丢了内核会静默回落宿主估计**，
而它的**指纹只有一处**：`_ce.txt` 的 `noise_variance`/`rsrp` 差 **~1 ULP**，
**而 `llr` / `h` / grid 与判决全部一致**。
同目录 `session_handoff_2026-09-13-5.md` §7.2 记录了 ODR 事故：
**所有后端静默变 CPU**，编译、单测、启动**全都不报错**，
只有启动日志里两行 `metal->cpu (requested backend not built in)` 露馅。

*证据（构建物也会静默降级）*：编译产物被写回**源码树**
（`full_gpu_chain/s2_full_chain_design.md` §48.146(e)10）⇒ 两个 build 目录**抢同一份 `.metallib`**，
加载到写了一半的内核，表现为**假回归**（`Test 3` 假报 NMSE 回归 **1.5 dB**，构建一结束重跑全绿）。
另有一次被污染的标记 `123456` 留在 `.metallib` 里 ⇒ "之后好几轮读数全部不可信"。

*后果*：**回退合法，不可观测的回退不合法。** 换平台时，"这个模块没跑在设备上"
必须是一条**报错或一个计数器**，不能是"结果看起来对"。

**⇒ 三条可执行的补充：**

1. **启动时打印"有效配置"**（不是请求的配置）——每一个后端选择都印出它**实际**解析成了什么。
2. **每条路径一个执行计数器**，且**内核构建物缺失必须是致命错误**，不是回落。
3. **设备内核构建物不得在构建目录之间共享**；给它**打戳并校验**它随源变化而变。

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

*证据*：**设计文档 §1.5**（用户裁定：数值不必逐字节复现）+ **§1.6** 的适用范围表：
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
而它会安静地吃掉整个异构路由的架构优势（竞争力分析 §2.4）。

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

### R18 —— 内核必须对**任意参数**终止（因为设备挂死是不可恢复的）

*证据*：C10 的演练——一个不终止的内核让整机冻结，`kill -9` 释放不了 GPU，`reboot` 也卡死，
**只能强制断电**；`recoveryCount=0` ⇒ 平台**不会自己复位**。
触发原因之一是"循环上界来自**参数**"；另一处是读标量 `[[threads_per_threadgroup]]` 并用它做循环上界。
**已在两个 `.metal` 文件里各写下一次**（"it has happened twice, both times costing a hard power cycle"）。

*由此形成的防御式写法（**逐字可移植，任何加速器平台都应照抄**）*：

| 规则 | 说明 |
|---|---|
| 所有循环上界是**编译期常量** | 参数只能**提前退出或跳过**，不能改变循环次数 |
| **每个下标都夹紧** | 读**和**写都夹。越界写在设备上是**静默数值污染**，不是崩溃 |
| 线程组大小是**编译期常量** | 不用 `[[threads_per_threadgroup]]` 决定迭代次数 |
| **绝不在屏障之前提前 return** | 半数组的线程走了 ⇒ 剩下的线程永远等不到 |

> **判据原文**：*"参数错 ⇒ 结果错（可见、可测），绝不允许 ⇒ 不终止。"*

### R19 —— 异步读必须在**提交侧**持有它的输入，且持有**恰好释放一次**

*证据*：`full_gpu_chain` D1 第 2 步的空中腿**否证了那条接线**：
"**接收缓冲在变换真正执行之前就被放回电台的池子并被下一批样本覆盖**" ⇒
零拷贝的 `grid_write::time_samples` 读到**过期样本**。
"**这不是'变慢'，是静默错数据**（`sinr=37.6 dB` + CRC 全错，正是它的签名）"。
修法：`retain_for_block()` + 完成回调 + 丢弃钩子。

*为什么必须有**三**条释放路径*：一条腿里**块 ≈ 2 × 跳**，**多数块根本没有消费者**——
没人认领的块永远不会提交 ⇒ 只在完成回调里释放 = **每个未消费的槽漏一个接收缓冲** ⇒ 池枯竭 ⇒ 接收停摆。
⇒ **正常完成 / 被取代或淘汰 / 被丢弃，三条都要有**，并把 `keepalives=released/attached` 打成一对
**退出时必须相等**的数。

### R20 —— 注册表必须有一个**有界**的保留窗口，且淘汰时**提交**而不是丢弃

*证据*：`gpu_phy_pipeline` §5.9.15 ③——按**地址**做键的交接表因为分配器复用地址而**串槽**
（"找到的就是【后一个槽】的块 ⇒ 这正好解释 **72% 坏 / 28% 好**"）；
§5.9.17 ②③——修好键之后**孤儿块永远不会被取代**：
`handed=64 taken=39 fallback=21 superseded=0` ⇒ `64−39−21=4` 个块悬着，
`896−840=56=4×14` 个凭据卡住 ⇒ **4 个接收缓冲被永久占住**（池子 8 ⇒ 枯竭）。
修法 = **扫掠**："落后最新 deposit 超过 **2 个槽**、且未被认领、未产出的记录，由注册表**自己提交**"。

*两条接口纪律*：①键必须是 **`(分配身份, 逻辑世代)`**，不是裸指针；
②**等待必须 fail-closed**——`grid_ready_hook::wait()` 在"找不到记录"时**返回 true（fail-open）**，
于是 harness "看着在跑、其实什么都没测"。

### R21 —— 池的下界是**流水线跨度**；不足的池是**死锁**，不是"慢一点"

*证据*：`lower_phy_test` / `radio_ssb_zmq_*` **挂死**（接收循环 `pop_blocking` 与变换**互等**）；
修法 = 按流水线跨度定池：`max(8, rx_to_tx_max_delay/rx_buffer_size, ceil(8 符号 × 最大符号长 / 缓冲大小) + 8)`，
"单线程执行器（单测）下 **4 个死锁、9 个通过**"。
**生产池 8 个 × 每槽 0.5 ms ⇒ 持有跨 8 个槽（≈4 ms）就把池吃光**，
而尾部 max 跨度（≈7.9 ms ≈ 16 槽）**必然**吃光；`free_min==0` 被命名为 **DRAINED**。

*推论*：**不要按稳态定池。** 要么声明"设计允许的最大同时持有数"，要么暴露一个
**空闲水位**（`free_min`）并把三种状态命名出来（`≥2` OK / `==1` TOUCHED / `==0` DRAINED）。

### R22 —— 提交的可见性顺序：命令缓冲必须在它的句柄**可见之前**冻结

*证据*：`shared_burst::deposit_released()` 把条目**先发布进表**（锁内）、**再**（锁外）挂完成回调 ⇒
另一线程可以在中间 `take_released()` 认领并**提交**该缓冲 ⇒ `addCompletedHandler` 落在已提交的缓冲上，
**`Abort trap: 6`**，两条腿复现。另一半：**二次提交 = 平台硬错误**。
⇒ **发表顺序是契约的一部分**，不是实现细节。

### R23 —— 编码是**单线程**的（提交/等待可以换线程）

*证据*：命令编码器**非线程安全** ⇒ **一个命令缓冲的编码只能在一个线程上完成**，而提交/等待可以换。
配合 R12（提交上下文线程局部）：**交接的是"提交"，不是"编码器"。**
另有一条工程纪律：任何打开的命令缓冲/编码器都必须有**析构与停机路径**去关闭它
（"release 未 `endEncoding` 的 encoder 会 abort"——实测在 ^C 上崩过一条腿）。

### R24 —— 平台缺的能力要有**可查询**的降级路径——见 R14；这条补充"**能力要按能力问，不按版本猜**"

*证据*：中立代码里出现过 `// older metallib` 这样的**构建产物版本**作为回退理由。
⇒ **回退的触发条件必须是能力查询，不能是版本或平台猜测**；
而"缺内核就静默回落宿主"必须取消（R8 / §6.4 S10）。

---

## 4.5 ★ 证据规则：怎么让**新平台上的读数可信**

**为什么证据规则属于可移植性文档**：换平台时最先失去的不是性能，是**判断性能的能力**。
下面每一条都是本仓库在**同一条平台上**已经踩过的坑——
**换到新平台后，它们会以完全相同的形式再出现一遍**，因为它们是**方法**问题，不是平台问题。

| # | 规则 | 本仓库的证据 |
|---|---|---|
| **E1** | **两个探针可能量的是不同总体。** 比较前必须**按稳定键配对**，并打印配对账 | 相位三段各 **97331** 个样本，而 `[ul_gpu_lane]` residency 有 **140204** 个（= 授权数）——"**它们不是同一个总体**"。修法：按**槽**配对，得到 `samples=73529 of phase_samples=73529`，并打印"no lane for the slot=0, lane older than 2s=0" |
| **E2** | **比值必须连同分子与分母一起报**——分母可以骗人 | `cbs/lane` 在 `event` 臂是 **4.00**、`burst` 臂是 **2.00**，而**乘积恒为 80**："`burst` 只是把 4 次提交**换了分组**"，只看 `cbs/lane` 会读出"减半"的假象 |
| **E3** | **单位要从物理结构推出，并对真实周期做整除断言** | "11346 槽 × 14"是一次**单位错误**（正确口径 `12×N+1`）：`158845 = 14×11346 + 1`（**不整**）而 `= 12×13237 + 1`（**整**）。时间换算差 **23 倍**（132.4 s 对 5.7 s）。根因是 PRACH 解调器**另一个引擎实例**（每 10 ms occasion **12 个符号** ⇒ **1200 transforms/s**） |
| **E4** | **判据的分辨率必须匹配事件的重数**：用汇总统计否定单发异常**不成立** | 失败态每次只有 **1 跳**异常，它的 `worst` 在 1853 跳的 max/median 里**完全看不见**；换逐跳配对后那一跳的 `A⁻¹ worst=5049`（邻居 1e-3）立刻现形。均值把单跳**摊薄 1/n** ⇒ 实测均值判据 12 跑只抓到 1 次 |
| **E5** | **一个检查如果会夸大自己的范围，比没有检查更糟**——它会**终止搜索** | 本仓库 `declare_reporter()` 的原文与理由；以及"绿了很久但测的不是被测对象"的多次记录（`ctest -L phy 162/162` 绿而 CE 单测**根本没注册**；某 CE 单测 grid 无 device view ⇒ `device_sigma2=0` **空转绿**） |
| **E6** | **门可以是"瞎的"而不是"宽的"**：新增测试后必须**证明它在选择器里**；门必须有**反向臂证明它能变红** | `ctest -R metal` **9/9** 绿，而**新测试根本没被跑到**；审计 A3：门**既瞎又宽**（`-R` 大小写敏感漏 **466** 条、`o_du` 子串陷阱 **19** 条噪声）。⇒ *"一个不能失败的检查不是证据"* |
| **E7** | **派生量可能是一个恒等式，不是一次测量**——引用前要证明**它能为非平凡值** | `dft gap` **恒为 0**（每隙只有一条提交 ⇒ `residency ≡ busy`）；另一条队列指标"定义让它恒等于 0"（1978 样本 **min = max = 0.0**） |
| **E8** | **同一瞬间的快照才能相比**；跨停机时刻比较会产生**假 FAIL** | 车道 `samples=`（atexit 打印）对 `[ul_time_frequency] samples=`（停机开始时打印）⇒ **73529 vs 73528 = 假 FAIL**。修法：只比同一行上的两个数 + 加**单调性不变式**代替阈值 |
| **E9** | **测量不得改变被测对象**；计数（atomic）常开，**计时回调 opt-in** | GPU 时间探针挂在实时上行依赖的提交路径上 ⇒ 改为 `OCUDU_METAL_GPU_TIME=1` opt-in；**仅日志级别一项就值 ~45 µs**（`[ul_pipeline]` **697 → 652 µs**）⇒ 上机一律 `--log.all_level warning` |
| **E10** | **环境漂移会让跨时间 A/B 失效**，即使代码**一字未改**；GPU 门必须**串行** | 同一份未改动代码不同时间：`invert()` **24.5 → 43.8 µs**、NMSE **−17.33 → −15.52 dB**、hop **231 → 281 µs**；并发时 hop 从 **183 漂到 423 µs**；`ctest -j 6` 让两个 CE Metal 单测因争用而红，串行 **2/2 全绿** |
| **E11** | **门本身可能是 flaky 的**，用它做裁决前必须先量化它的抖动 | "**这道门本身是 flaky 的**——基线 2/24 轮、改动后 3/28 轮出现一次不匹配（Fisher **p≈1.0**）⇒ **它红了不要直接当回归**" |
| **E12** | **首次读数必须保留**，重跑只能加注不能覆盖 | `ab_dumps`：隔离下确定，但在重 GPU 活动后读到 **2728** ⇒ 规则 = 首次非零时重跑一次，**detail 里保留首次读数**（`[flake rule: the FIRST read was 551]`） |
| **E13** | **缺失的文件绝不能读成"相等"** | `cmp -l` 对两个不存在的文件 ⇒ "432 次全部相同"是**空洞的**。修法：先证明两侧都**存在**，某侧缺文件 ⇒ 退出码 2，**"没有数据"绝不读成"相等"** |
| **E14** | **旁证不是证据**：引用别人目录里的读数前，先确认那个文件**存在** | 本仓库多处注释引用 `wip/metal_alias_order.mm`、`wip/S14_instruments.md`、`wip/S12_incident_gpu_hang_2026-09-19.md`——**这些文件确实存在**（已核）。**但这个"核实"动作本身必须做**，因为注释里的引用**不保证**指向真实文件 |
| **E15** | **测量记录必须带 provenance 对**：宿主二进制 + 内核库是**两个**版本 | `ctest` **不构建** ⇒ 它跑的是上次链接的二进制，与刚重建的 `.metallib` 组成**版本不匹配**的一对；只重建一个目标 ⇒ 27 个捕获的 `_llr.bin` **全部不同**，**全量构建后 0/135 差异** |

**⇒ 这一节的元规则：**

> **换平台时，先建立"能判断对错"的能力，再建立"能跑得快"的能力。**
> **一个读数的可信度不是它看起来多干净，而是它附带了多少条能被证伪的约束。**

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
| **设备虚接口族** | `include/ocudu/phy/upper/channel_processors/channel_equalizer.h`（`device_slice`、`consumes_device_estimates`、`set_device_grid`）、`include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h`（`ch_est_device_view`、`get_device_ch_estimates`、`device_results_cover_last_estimate`） | **这是"能力查询"做得最对的一处**：`device` 是厂商中立的词；每个能力都是一个**带 CPU 安全默认值**的虚函数；**调用方问，而不是假设**。⚠ 一个保留：它们暴露**裸 `const float*`**，即"设备地址就是宿主可解引用的指针"——这对离散显存平台不成立（见 §6.4 S1） |
| **平台兼容层的 API** | `include/ocudu/support/macos_compat.h` 的 `aligned_alloc/free`、`describe_aligned_allocation`、`register_aligned_free_observer`、`page_size`、`get_monotonic_time_us`、`set_thread_name`、`sendmmsg/recvmmsg`、`le32_to_host` 等 | **这些签名是纯能力形状的**，第三个平台可以逐个实现。⚠ **但同一层里另有四个函数是语义泄漏**（见 §6.4 S4）——所以"这一层是干净的"这句话**只对一部分成立** |
| 模块接口 | `dft_processor`（含 `run_async`/`wait`/`begin_block`/`release_block`/`retain_input`/`get_grid_write`）、`port_channel_estimator`、`pusch_channel_equalizer` 等上游接口 | **端口就在这里**。新平台实现同一个接口。**这些名字全部是设备中立的** |
| ISA 分支 | `_avx2`/`_avx512`/`_neon` 文件 + `cpu_supports_feature()` 运行时检查 | **对照样板**：ISA 分支**配了运行时检查**。与之相对，`#if defined(OCUDU_METAL_*)` 分支**没有运行时对应物**——这就是 D8/D9 的问题所在 |
| 可用性常量的**机制** | `apps/units/flexible_o_du/o_du_low/CMakeLists.txt:17-25` + `du_low_phy_pipeline.cpp:4-33` 的 `OCUDU_METAL_*_AVAILABLE=$<BOOL:...>` 0/1 常量 | **机制是对的**：一个 TU 里的 0/1 常量，解析器里**没有平台宏**。错的是**词汇**（厂商名）和**真源**（编译期常量而非注册表） |

### 6.2 债：命名与注册（会随部署规模变贵，但不改变语义）

**严重度**：🔴 阻止 / 🟠 真实债 / 🟡 表面债。**本表只有 🟠/🟡——🔴 在 §6.4。**
（判定"是否泄漏"的方法：只算**实际代码**里的使用点；只在注释里出现的平台名算 🟡。）

| # | 位置 | 泄漏了什么 | 为什么影响移植 | 严重度 | 替换形状 |
|---|---|---|---|---|---|
| D1 | `include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h:28-38` | 枚举值 `metal_mmse`、`metal_nn_mmse`、`helena` —— **平台中立头文件里按 API/运行时命名算法**，而且它们是 `--pusch_channel_estimator_algo` 的**线上格式** | **新平台无法表达自己**，必须先改这个枚举和它的每个 switch | 🟠 | `cpu, mmse_block, mmse_matrix_unit, ml_model`（按数学/能力命名）；`metal_mmse = mmse_block` 保留为**弃用别名**只供 CLI |
| D2 | `include/ocudu/phy/upper/upper_phy_factories.h:380-385`、`include/ocudu/phy/lower/lower_phy_configuration.h:61-71`、`apps/units/flexible_o_du/o_du_low/du_low_config.h:74-164`、`du_low_config_cli11_schema.cpp:193-380`、`du_low_config_validator.cpp:136-143`、`du_low_config_translator.cpp:120-124`、`flexible_o_du_configs.h:56` | backend 取值 `"metal"` —— **用户可见的配置、CLI 白名单、校验器、翻译器里全是厂商名** | 同时改**库 / 低层 PHY / 应用 / CLI / 校验器 / 翻译器**六处 + YAML；**校验器在所有平台上接受 `metal*`** ⇒ Linux 用户得到一个通过校验、然后在深处失败的配置 | 🟠 | `enum class offload_backend { cpu, device }` + 名字→backend **注册表**；校验器按注册表校验（**未知值 fail-closed**） |
| D3 | `lib/phy/upper/signal_processors/channel_estimator/factories.cpp:41-47` | 写死的 `"only available on Apple Silicon macOS builds"`，**而且整段拒绝被 `#if !defined(OCUDU_METAL_CHEST)` 包住** | 错误信息把**能力缺失**说成**平台缺失**；更糟的是**这段校验在唯一可能被误配的平台上被删掉了**（Metal 构建里请求一个无法满足的 algo 会继续走到 `create()` 返回 `nullptr`） | 🟠 | 无条件的**能力检查**：`if (needs_device && !factory_has_device) report_error(...)` |
| D4 | 顶层 `CMakeLists.txt:44,54,62,70,78`（五份）+ `:87-102` | 用 `APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64\|arm64"` 决定五个后端默认值（同一个测试写五遍）；`FATAL_ERROR` 把 Apple Silicon 说成原因 | 编译期**平台名**判断代替**能力**判断（R13）；**全树没有任何能力探测**——没有 `find_program(xcrun)`、没有 Metal 工具链的 `check_cxx_source_compiles`、没有设备查询 | 🟠 | 一个 `ocudu_detect_accelerators()`：探测工具链 + 设备，产出 `OCUDU_ACCEL_<kind>_AVAILABLE`；选项默认值由它派生。**保留** `FATAL_ERROR`（那是 R8 的正确应用），但把理由改成**缺哪项能力** |
| D5 | `include/ocudu/support/macos_compat.h` + `utils/macos_compat/`（`ocudu_macos_compat`） | **名字**是平台名，**大部分 API** 是能力形状 | 一个 CUDA/Windows 移植会去 `#include "macos_compat.h"`，语义上是错的；而且这个头被**中立 PHY 头**包含（`resource_grid_device_view.h:8`、`page_aligned_allocator.h:14`、`pusch_demodulator_impl.h:21`）⇒ **一个平台的名字出现在中立 PHY 的 include 行上** | 🟡 | 改名 `platform_compat.{h,cpp}` / `ocudu_platform_compat`。**纯改名，零行为变化** |
| D6 | `lib/phy/upper/signal_processors/channel_estimator/metal/ai_assets/*.mlmodelc` | 仓库里**只跟踪 CoreML 产物**（`model.mil` + `weights/weight.bin` + `coremldata.bin`），**不跟踪**厂商中立的权重导出。**已训练的权重只存在于厂商包里** | AI PHY 的**已训练产物**是一个厂商格式。换平台时"权重怎么来"没有答案 | 🟠 | **比看起来便宜**：架构（`ai_train/helena_arch.py`）与训练流程（TF/`tf_keras`）**都在仓库里且是平台中立的**，中间产物 SavedModel 已存在；被锁住的只有 `convert_coreml.py`（`coremltools`）+ `run_training.sh:67`（`xcrun coremlcompiler`）这**最后一步**。⇒ 每次训练**多导出一份 ONNX** 即可 |
| D7 | `CMakeLists.txt:25-28` | `option(ENABLE_METAL_STATS ... OFF)` 用**目录级 `add_definitions`** 添加宏 ⇒ 它**到达全树的每一个 target**，而以 Metal 命名的选项在门控**平台中立**的判据 | 见 §6.4 S2/S3（这一条是 🔴 的入口）。命名上，先例已经存在：`ENABLE_FLOW_PROBES`、`ENABLE_CE_TIME` 都是中立名 | 🟠 | `option(ENABLE_PIPELINE_CONTRACT_PROBES ...)`，**按 target** 施加，不在目录级 |
| D8 | `include/ocudu/phy/phy_pipeline_{contract,crossings}.h`、`dft_processor.h:119-152`、`phy_pipeline_ul_slot_plan.h:21`、`port_channel_estimator_average_impl.h/.cpp:634`、`pusch_decoder_impl.h:185-190`、`formatters.h:182-193`、`pusch_decoder_result.h:26-40`、`ldpc_decoder.h:61-67`、`generic_functions_factories.h:104-111` | **中立文件里成体系地出现 Metal 词汇**（含**结构体字段名和日志字段名**）：`ldpc_metal_elapsed`、`get_last_decode_metal_elapsed()`、`metal_t={:.1f}us`、`metal_decode_elapsed_ns`、`create_dft_processor_factory_metal()`，以及中立 CE 基类用 **Metal kernel 文件名**注释自己的扩展钩子 | 日志字段名是**脚本与 A/B 工具解析的对象**；改名字是破坏性变更 ⇒ **越晚越贵**。用 kernel 文件名注释钩子，新后端要**反推**哪个钩子是什么意思 | 🟠 | 统一到 `device`/`dev` 词汇：`ldpc_device_elapsed`、`dev_t=`、`create_dft_processor_factory_device()`；钩子按**能力**描述（"一个在估计值所在处做归约的后端"），不写 kernel 文件名 |
| D9 | `channel_coding_factories.h:38-45`、`upper_phy_factories.h:307-310`、`:348-367` | Metal 专属调参（`ldpc_decoder_offset`，"for the Metal NMS decoders"）+ **三个 `.mlmodelc` 文件系统路径成为中立工厂配置的字段**，且**个数绑定到特定 PRB 数** | 一个通用 API 里出现 Core ML 产物路径；`-1 == unset` 的语义只为 Metal 解码器定义 | 🟠 | `std::vector<model_ref>` / `std::map<bw, model_ref>`，或不透明的 `backend_config`，由后端工厂拥有 |
| D10 | `lib/phy/metal/` 目录名 + `lib/phy/metal/ocudu_metal_queue.h:29-30` 的 `#error` | **共享提交层住在以厂商命名的目录里**（queue/burst/probe，**3951 行 `.h/.mm`**）；`#error` 本身是对的（那是一道**边界**），错的是它暗示这一层属于 Metal | 新平台不知道 `lib/phy/metal/` 是**共享基础设施**而不是某个模块的实现 | 🟡 | 目录/目标改名 `lib/phy/accelerator/`；或至少在 `lib/phy/metal/PORTING.md` 里把这一层命名为**提交层**并在 §7 的清单里标出 |
| D11 | `utils/macos_compat/CMakeLists.txt`、`lib/phy/*/metal/CMakeLists.txt`、`cmake/modules/ocudu_metal.cmake:78,90` | 五个 `metal/` 目录各自**无条件 `enable_language(OBJCXX)`**（全局副作用，只有当父目录先门控才安全）；`xcrun` 与 SDK 名 `macosx` **硬编码**，没有 `find_program`；构建产物**落在源码树**并把**源码树的绝对路径烧进二进制**（`OCUDU_DFT_METALLIB_PATH`）；顶层 `CMakeLists.txt:115-134` 硬编码 `/opt/homebrew` | 五个独立全局副作用 + 不可重定位的产物 + 一台开发机的包管理器路径 | 🟠 | 一个 `ocudu_enable_accel_backend()` 助手调用一次；`find_program` 编译器与 SDK；产物进 `CMAKE_CURRENT_BINARY_DIR` 并安装到资源目录；`find_package` 代替 `/opt/homebrew` |
| D12 | `lib/phy/upper/channel_coding/ldpc/CMakeLists.txt:41-43` | `if(APPLE AND ...)` ⇒ `target_compile_definitions(ocudu_ldpc PRIVATE "__always_inline=__attribute__((always_inline))")` —— **一个改写关键字的宏，按平台名选择，施加在中立 target 上** | 这不是"编译哪个实现"，是"**源码是什么意思**" | 🟠 | 编译器能力检查（`check_cxx_source_compiles`），或直接修源码 |
| D13 | `tests/unittests/gateways/CMakeLists.txt:42-49` | `if (APPLE)` + `enable_language(OBJCXX)` + `-framework Metal` —— 一个 `.mm` 被编进**平台中立的测试目录**（`tests/unittests/gateways/`，全树唯一一个在 `*/metal/*` 之外的 `.mm`） | 测试的归属与后端不一致，新人会以为"网关需要 Metal" | 🟡 | 移到 `lib/phy/.../metal/test/`，按后端的能力标志门控 |
| D14 | `tests/unittests/phy/generic_functions/CMakeLists.txt:20-26`、`tests/benchmarks/du_high/CMakeLists.txt:12`、`tests/integrationtests/{rlc,ofh}/CMakeLists.txt` | `if (APPLE)` / `if (NOT APPLE)` 决定测试的**存在与禁用**，并打上 `macos_unsupported` 标签 | 平台名代替能力（该测试缺的是 AVX2 / epoll / perf events，不是"非 macOS"） | 🟡 | 按 ISA / 系统调用 / 设施的能力门控，标签按**原因**而不是按平台 |
| D15 | ★ **三个硬件上限是硬编码且从未校验的**：`lib/phy/generic_functions/metal/ocudu_dft.metal:138` 与 `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_ta.metal:475` 的 `min(n, 1024u)`（最大线程组）；`ocudu_mmse_pilots.metal:663`/`:806` 的 `threadgroup float red[tg_size / 32u]`（**SIMD 宽度 32 被写进数组上界**）；`ocudu_mmse_inv.metal:42-50` 的 `MAX_N = 54`（**由 32 KB 线程组内存反推**："54×108 floats 是 22.8 KiB，在 32 KB 之内，而 **72 就不行了**"） | **设备属性从未被查询**：全车道对 `threadExecutionWidth`、`maxTotalThreadsPerThreadgroup`、`supportsFamily:`、`hasUnifiedMemory`、`recommendedMaxWorkingSetSize` 是**零命中** | 目标平台的上限只要更小，结果就是**静默错答案或挂死**（见 R18/C10：挂死不可恢复），**而不是编译错误** | 🟠（**对新平台是 🔴**） | 把三个上限变成**启动时查询 + 断言**；`mmse_inv_threadgroup()` 已是"唯一选几何的地方"，把这条纪律推广到线程组大小与 SIMD 宽度 |
| D16 | `lib/phy/upper/channel_coding/ldpc/metal/ocudu_metal_decoder_engine.mm:221`（`(length + 4095) & ~4095`）、`ldpc_decoder_metal.cpp:268,273`（`posix_memalign(&p, 4096, …)`）与 `ocudu_dft_metal_engine.mm:1144`（`compat::page_size()`）**不一致** | **同一个树里两套页粒度**：LDPC 硬编码 4096，DFT 用运行时页大小。在 **16 KiB 页**的机器上 LDPC 的 4096 对齐**不是页对齐** ⇒ `newBufferWithBytesNoCopy` 返回 nil ⇒ **静默走拷贝回退**；而 MMSE 头文件还留着同样的过时声明（`ocudu_metal_decoder_engine.h:74,76,99`、`mmse_engine.h:14` 都写 "4KB-aligned"，而代码用 `compat::page_size()`） | **这是"潜在违约被回退掩盖"的教科书例子**：它今天能工作，**只是因为失败时会静默降级**——也就是 §6.4 S10 那个缺陷**同时**在掩盖一个对齐 bug。⇒ 修掉 S10 会**立刻**把这个 bug 变成可见故障 | 🟠 | 全部改用 `compat::page_size()`；把"页粒度"收进一个 `compat::device_wrap_granularity()`；同时修掉两处过时注释 |

**⇒ 关于这张表的四点观察：**

1. **本表 16 条里没有 🔴——但这不是好消息，因为 🔴 在 §6.4。**
   命名债可以慢慢还；**语义债不能**。而 **D15/D16 是"借来的时间"**：
   它们今天不痛，**只是因为 S10 的静默回退在替它们兜底**——所以修 S10 会同时暴露它们。
2. **D1–D5、D10、D11、D13 加起来基本是一次"改名 + 能力探测 + find_program"提交，不含逻辑改动**（D6 是加一行 ONNX 导出）。
   这是可移植性投资里**性价比最高的一笔**，而且**现在做是零风险的**。
3. **D8 是唯一"越晚越贵"的一条**：里面包含**结构体字段名和日志字段名**，
   它们是**脚本与 A/B 工具的解析对象**⇒ 发布后再改是迁移成本，不是重构成本。
4. **本表也是本文档自身可证伪性的来源**：条目会随提交增减，**以 `git log` 为准**。

### 6.3 阻止项（A 层缺项 —— 不是债，是**换平台的前提**）

| 缺项 | 后果 | 现状 |
|---|---|---|
| **缺 C1（统一可寻址内存）** | **竞争力分析 §2.4** 的"路由边际成本为零"失效 ⇒ 整个异构路由的前提失效 ⇒ **不是移植，是重新设计** | 候选平台里 NVIDIA 是离散语义（`metal_vs_cuda_architecture.md` §2.3）；**GB10 的相干统一内存是否满足 §2.2 的四条判据：未核实** |
| **缺 C1′（更隐蔽的一种）**：内存是共享的，但**"可被设备寻址"被定义成"宿主页对齐"** | 在离散显存平台上这个谓词**直接为假** ⇒ 设备路径**静默消失**（走宿主），而没有任何报错。见 §6.4 S1 | **这就是本仓库今天的写法**（`resource_grid_device_view.h:70-72`）。它把 C1 的判定权交给了 `compat::page_size()` |
| **缺 C2（可编程设备）** | 只剩 AI PHY 一条腿（见 §3 的 S2） | Intel / AMD / Qualcomm 的 **NPU** 都是推理图加速器 |
| **缺 C5（值语义完成凭据）** | 宿主参与点无法消除 ⇒ 主判据达不到；**但程序照样跑**，所以这个缺失**不会自己暴露** | 上游 CUDA 的 `cuda_event` 就是这一项的部分缺失（只有"等 / 完成没"） |
| **缺 C6（独立推理引擎）** | AI PHY 挤进 GPU 队列 | 见 C6 行 |
| **缺 C12（数值范围：fp64）** | **设备端矩阵反演这条路从算法上不存在**：`cond₂(A) ≈ 2.1e4` 而 float32 的元素级下限是 `cond·eps ≈ 2.5e-3`，**所有 float32 形式都远在下限之上**；语言层面 `'double' is not supported in Metal`（6 处）。实测：设备端 blocked GJ **9.67e-1**（−18.69 dB）、float32 Cholesky **2.88e-1**，而 **float64 Cholesky 是 4.62e-10** | **这是全套材料里最大的一处"能力差"，而且是算法性的，不是调优性的**：一个有 fp64 的平台（CUDA）会**直接删掉整个设备端反演的绕行方案**。⇒ **候选平台在这一项上会明显优于现状**，§7 的 dry-run 必须把它记为"新平台多给了什么" |
| **缺 C13（逐 kernel 的严格 IEEE 编译选项）** | `ocudu_mmse_pilots_power.metal:18-21`：**Metal 的默认 fast math 让同一次除法在 298151/2²⁰（28.4%）个输入上与宿主不同**，**在 27/27 条语料上翻转了 LLR 判决（32613 个软比特）**；加 `-fno-fast-math` 后同一扫描是 **0/2²⁰**——"**在这个 GPU 上，那个编译选项就是全部差别，不是硬件**" | 一个只说"IEEE fp32"、不说"**并且告诉编译器不要重结合**"的可移植性契约是**不完整的**。本仓库已有按源文件施加该选项的构建机制（`cmake/modules/ocudu_metal.cmake:14-20`），**这是应该被复制到契约里的形状** |

### 6.4 ★ 语义泄漏：`#if` 改变的不是"编译哪个实现"，而是"程序是什么意思"

**这一类比 §6.2 严重一个量级，因为它不能靠改名解决，而且它的失败模式是"看起来正常"。**
判据：如果删掉这个 `#if`（只留一边），**行为会变**而不只是**性能会变**，那它就是语义泄漏。

| # | 位置 | 泄漏的是什么 | 为什么是 🔴 | 修法 |
|---|---|---|---|---|
| **S1** | `include/ocudu/phy/support/resource_grid_device_view.h:58-73` | **"设备可寻址"被定义成"宿主页对齐"**：`make_resource_grid_device_view()` 用 `compat::page_size()` 取模，不对就返回**无效视图** | 在**离散显存**平台上这个谓词**恒为假** ⇒ 设备路径**静默消失**（"its writers keep running on the host"），**没有任何报错**。⇒ 这一条**直接挡住 x86+NPU 目标**（与竞争力分析 §5.3 的结论合流） | 有效性来自**后端的能力查询**：`accelerator->supports_host_pointer(base, bytes)`，而不是 `page_size()`。同一处 `device_slice` / `ch_est_device_view` 暴露**裸 `const float*`**，应改为**不透明句柄** |
| **S2** | `lib/phy/lower/lower_phy_baseband_processor.cpp:248-306`、`786-817`；`lib/phy/lower/processors/uplink/uplink_processor_impl.cpp:50-76, 179-251, 275-283`；`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:762-825` | **8 条契约检查里有 5 条被 `#if defined(OCUDU_METAL_STATS)` 编译掉**，包括 `radio sample continuity`、`cfo compensation`、`baseband metrics`、`host sample assembly`、`ce device estimates`。**其中 `786-817` 那段是"接收流不连续"的检测和它的 PHY warning** | **一个正确性诊断被构建选项门控**：默认构建下**丢了一个无线块既不被检测也不被报告**。而且那个选项**默认 OFF**，注释还写错了（`uplink_processor_impl.cpp:46-47` 说"每个非 Apple Silicon 构建"，实际是**每个构建，包括 Apple**，除非显式 `-DENABLE_METAL_STATS=ON`） | 检查与检测**永远注册**；只有**打印**放在探针选项后面。选项改名 `ENABLE_PIPELINE_CONTRACT_PROBES` 并按 target 施加（D7） |
| **S3** | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:743` | **`gpu` 模式自己那条核心判据（`host device data crossings`）的唯一登记点，在 Metal 代码里** | ⇒ **非 Metal 构建下契约从 8 条塌缩到 1 条，而那 1 条也不会注册。** 换平台时**后端与判据同时消失**——这正是 R7 警告的坑，而它已经发生了 | 把登记移到一个**中立且总会执行**的位置（例如紧邻 `phy_pipeline_mode_registry::set()`） |
| **S4** | `utils/macos_compat/macos_compat.cpp:520-571` | **四个中立名字的调用，语义按平台不同**：`lower_phy_stop_chain_end` 在 Linux **立即**完成 promise、在 macOS **推迟到最后一个处理任务**；`drain_executor_on_stop` 是"哨兵排空" vs "no-op"；`wait_for_tx_timestamp` 是 "sleep 10 µs" vs "带 YIELD 的自旋" | **调用方无法表达、也无法查询它拿到的是哪一种**。⇒ 停机语义是**平台私有**的。这是"第三个平台"会直接踩的地方 | 把语义作为**能力数据**传出去（`compat::stop_completion::chain_end` 等由运行时探测决定），或抽成 `lower_phy_platform` 接口、每平台一个实现 |
| **S5** | `include/ocudu/phy/phy_pipeline_strict.h:29-36` | **反兜底策略本身默认是宽松的**：`OCUDU_GPU_STRICT` 环境变量可覆盖；没有发布 mode 的进程（**所有工具与单测**）直接落到 permissive 分支 | "设备拒绝 ⇒ 让这个授权失败"这条保证变成**opt-in**，而它是 `mode=gpu` 的全部意义所在（§1.8 用户裁定） | 做成**流水线配置的一个字段**（头文件自己就说"数据路径始终从自己的配置取 mode"）；env 只留给测试夹具 |
| **S6** | `utils/macos_compat/macos_compat.cpp:618-625` | **同一次运行是否报告"有效后端"，取决于平台**：macOS 打 `PUSCH LDPC decoder type: {}`，Linux 直接 `(void)decoder_type;` | ⇒ **"A/B 旋钮可从启动日志核验"是 Apple 专属性质**。换平台后**你失去了核验手段，而没有任何提示** | 永远打印 |
| **S7** | `utils/macos_compat/macos_compat.cpp:209-220, 222-243` | `get_current_cpu()` 在 macOS **返回 0**（不是"未知"，是一个**假值**），文档承认消费者"退化成固定偏移"；`get_available_cpu_ids()` 在 Apple Silicon 上把 **E 核也报成可用 CPU** | **能力查询给出错误答案而不是"未知"**；内存池的 CPU 分布因此静默改变 | 返回 `std::optional<unsigned>` / 带 `performance_core` 分类的 `cpu_topology`（该层已经知道 P 核的事） |
| **S8** | `include/ocudu/support/cpu_features.h:137-142` | `case cpu_feature::pmull:` 在 `#if defined(__APPLE__)` 下**直接 `return true`** —— **按平台名回答能力问题** | 一个不带 crypto 扩展的 arm64 非 Apple SoC 会被**告知它有** | `return compat::cpu_has_pmull();`（sysctl / `getauxval` 探测） |
| **S9** | `lib/rrc/metrics/rrc_du_metrics_aggregator.h:111-116` | `#if defined(__APPLE__)` 用 `rbegin()->second`，`#else` 用 `end()->second` | **`end()` 解引用是 UB**，而那正是**非 Apple 分支**；而且这个 `#if` **改变的是被上报的指标值**——一个"测量值取决于标准库实现"的泄漏 | 删掉 UB 分支，写一个正确的表达式（任何平台上都没有正确的 `end()` 解引用） |
| **S10** | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:847-876`；`lib/phy/generic_functions/generic_functions_factories.cpp:127-147`；`channel_equalizer_metal_factory.cpp:24-72`；`demodulation_mapper_metal_factory.cpp:26-28` | **逐层、逐尺寸、逐调用的静默回退**：设备估计被拒 ⇒ 静默改走宿主 gather（**揭示它的计数器在 `OCUDU_METAL_STATS` 后面**）；DFT "everything else … **falls back transparently**"（无日志）；`is_supported()` **故意用 CPU 后端的集合回答**，于是"问 metal 后端支不支持这个形状"会被告知"支持"然后拿到 CPU | **回退合法，静默回退不合法（R8）。** 这里三处都是静默的，而且**其中一处连"揭示它的计数器"本身也被编译掉了** | 每次回退一个计数器 + 一次一次性日志；`is_supported()` 回答**自己的**能力，不回答 CPU 的 |
| **S11** | `apps/units/flexible_o_du/o_du_low/du_low_phy_pipeline.h:80-87` | **`phy_pipeline_lane_defaults { dft = "metal"; ch_est = "metal_mmse"; equalizer = "metal"; }`** —— **厂商字符串被硬编码在平台中立的模式解析器里**。配套两处同源问题：`is_cpu_phy_backend()`（`:73-77`）用**显式字符串白名单**判定（`"auto","cpu","generic","neon","avx2","avx512"` 算 CPU，**其余一律算 offload** ⇒ **新后端靠"不在名单里"意外成为设备后端，fail-open**）；模式可用性错误信息写成"requires the **Metal** DFT, channel estimator, …"（`:255-256`） | **这是最严重的一条**：`phy_pipeline_mode` 枚举本身是干净的（`cpu`/`cpu_gpu`/`gpu`，全树无厂商名），但**"gpu 模式"在解析器里的含义就是"那三个 Metal 后端"** ⇒ 即使判据、契约、接口全部中立，**第二个加速器仍然要改中立的应用代码才能被选中**（判据 P0 因此必然失败） | `enum class phy_backend_kind { cpu, device, dsp }` + 一个**进程级 `phy_backend_registry`**，每个后端自注册 `{kind, module, name, availability, is_supported(shape)}`。`lane_defaults = registry.best_for({dft, ch_est, equalizer, demapper})`；`is_cpu_phy_backend` ⇒ `registry.kind_of(v) == cpu`，**未知值 fail-closed**；错误信息与可用性判断都去问注册表 |
| **S12** | `tests/unittests/phy/lower/processors/uplink/lower_phy_uplink_processor_test.cpp:543-550` | `GTEST_SKIP() << "the 'host sample assembly' check is not registered in this build (ENABLE_METAL_STATS off), so this arm has no verdict to move"` —— **那条本来会推动判据结论的测试，自己在缺判据时跳过** | **CI 在没有该判据的情况下是绿的。** 这是 R7/§6.4 S2 的**测试侧共犯**：判据被编译掉之后，**唯一会发现这件事的机制也一起消失了** | 改为**失败**（或在测试构建里让探针默认 ON）；"缺判据"必须是红，不是 skip |

**⇒ 这一节的结论，也是本文 v1.1 最重要的更正：**

> **可移植性的主要障碍不是词汇，是语义。**
> 词汇债（§6.2）**只随部署规模变贵**；语义债（§6.4）**已经让判据在默认构建里不存在**——
> 而且它失败的方式是"**日志全绿**"。
>
> **⇒ 因此 §7 的六步 dry-run 里，第 3 步（先跑 `mode=cpu` 把契约的空转面看清楚）
> 不是准备动作，它是整个移植里最便宜、也最关键的一步。**

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
| **本文的判据 P0（只改 `*/metal/`）今天是否成立** | **今天不成立，而且失败点已经全部列出。** 最严重的一条不是命名，而是 **§6.4 S11**：`phy_pipeline_lane_defaults` 把 `"metal"` / `"metal_mmse"` 硬编码在**平台中立的模式解析器**里 ⇒ 即使判据、契约、接口全部中立，**第二个加速器仍要改中立的应用代码才能被选中**。**这是一个"已知会失败但可枚举"的判据，不是一句口号** |
| **候选平台的 C1 是否成立** | NVIDIA GB10 的"CPU–GPU 相干统一内存"**是否满足 §2.2 的四条判据：未查到一手出处【待核实】** |
| **Intel NPU 的可编程性边界** | 已知是推理图加速器（OpenVINO 模型）；**能否承载非推理型的自定义算子链未核实【待核实】**（决定它能否替代 GPU 而不是只替代 ANE） |
| **Strix Halo 在 Linux 上 NPU 之外的能力** | AMD 的 Linux 文档只说 NPU-only 流，**没说 iGPU 计算在 Linux 上能不能用**【待核实】——这决定"统一内存 + 可编程 GPU"两条在 x86 上是否同时成立 |
| **候选平台的 C11（测量可复现性）** | 本文只知道**本平台不合格**（GUI 分时 GPU + 热降频 3.3×）。**候选平台是否更好，只能在真机上按 §4.5 的协议量**——这是 §7 dry-run 第 6 步的输入 |
| **候选平台的 C4③（完成信号的可见时刻）** | 本平台实测"中途编码的信号只在完成时可见"。**候选平台（CUDA/Vulkan）大概率不同，但这必须实测而不是推断**——它直接决定"提前释放"这个设计在新平台上**是否存在** |

> **⚠ 一条关于本文自身的方法论提醒**：§6.2/§6.4 的多数证据来自本仓库的中文文档与代码注释。
> **引用注释里的文件路径前，先确认那个文件存在**（§4.5 E14）——本仓库出现过注释引用
> 与文件实际位置不一致的情况。本文已核实过它所引用的 `wip/` 文件（存在），
> **但这条核实动作必须随文档演化重复做**。

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
