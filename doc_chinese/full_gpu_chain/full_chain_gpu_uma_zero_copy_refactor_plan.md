# OCUDU 全链路 GPU/UMA 零拷贝直连重构工程蓝图

> FPGA Thunderbolt-5 PCIe DMA → UMA 零拷贝映射 → 纯 GPU 内部状态流转 → 显存内 FAPI 组装 → MAC 极速提取

| 项 | 内容 |
|---|---|
| 文档状态 | **v1.0（可实施工程方案）** |
| 代码基线 | `apple-silicon` 分支，HEAD `b30a167178`（2026-09 扫描结论） |
| 目标硬件 | Mac mini **M4 Pro** + AD9361 (FMC) + FPGA (XDMA + 1PPS/10MHz) + **ASM2464PD** PCIe↔USB4 桥 + Thunderbolt 5 线缆 |
| 关联文档 | `doc_chinese/apple_silicon_heterogeneous_gnb_plan.md`（路线图，本文是其 §3「整条 UL 链一次 dispatch」的工程化落地）；`lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`；`lib/phy/upper/signal_processors/channel_estimator/metal/Metal_MMSE_Channel_Estimator_PLAN.md`、`AI_channel_estimation_implementation_plan.md`；`doc_chinese/build_macOS_note.md` |
| **执行序列权威** | **`doc_chinese/full_gpu_chain/soft_hard_decoupled_execution_plan.md`**（软硬解耦双支线执行计划：本文五阶段的技术内容有效，但执行顺序与里程碑以该文件为准——支线一=UHD 输入下先行软件管线，支线二=硬件 PCIe 直通并行攻坚，最终在 `iq_source` 接口处收敛） |

---

## 0. 执行摘要

### 0.1 一句话目标

把当前「CPU 主导、模块级 Metal/ANE 局部加速」的 RX 管线，改造为
「**FPGA DMA 直写 UMA → CPU 只做一次零拷贝包装与一次提交 → FFT/CE/均衡/解调/LDPC/CRC/FAPI
全程在 GPU 内部流转 → GPU 完成回调一次性唤醒 MAC**」的全链路零拷贝管线，
同时保留 CPU/GPU/ANE 的 CLI 级 A/B 路由能力。

### 0.2 代码扫描得出的三个「平台级硬事实」（一切设计的前提）

以下三条是本次全库扫描 + 既有 PLAN 记录中已被实测证实的约束，本蓝图的每一阶段都必须遵守：

1. **ICB（Indirect Command Buffer）在本平台被阻断**（`ldpc/metal/PLAN.md:593-607`，darwin 25 / AGX G16X 最小复现实测）：
   CPU 编码的 ICB 内，`setComputePipelineState` 导致 GPU Address Fault、`setKernelBuffer` 被静默忽略，
   只有**裸 dispatch 命令**可靠执行。因此第三阶段**不得把第一版全链管线押在 ICB 上**，
   必须先走「CPU 编码 + 单命令缓冲 + GPU 内部 MTLEvent 依赖」路线；ICB 保留为平台修复后的演进项
   （恢复方案已在 PLAN.md 中演练完整：dispatch-only ICB + 每命令 offset 绑定 `layer_starts` 缓冲）。
2. **Metal（本机 MSL 目标）不支持跨 threadgroup 软件栅栏**：只有 relaxed 原子操作，无 acquire/release/seq_cst、
   无 `atomic_fence`（`PLAN.md:892-898`）。GPU 内部跨 threadgroup 的依赖必须用
   **MTLEvent / MTLFence / 命令缓冲内顺序执行**表达，或沿用 single-threadgroup 持久 kernel（persistent 范式）。
3. **Apple Silicon 上没有 CPU 亲和性**：`compat::set_thread_affinity` 是 no-op
   （`include/ocudu/support/macos_compat.h:165-172`），配置里的 `cpu_mask` 只影响 Mach affinity tag 与 QoS class。
   「把 L1 钉在 P 核」的 x86 式推理在 Mac 上不成立——本蓝图用 **QoS 分类 + Mach time constraint + GPU 卸载**
   而非绑核来保证实时性。

### 0.3 现状一句话诊断（扫描结论浓缩）

- RX 数据面：**拉模型，无线程**。`lib/radio/` 不含任何线程，唯一 `receive()` 调用在
  `lib/phy/lower/lower_phy_baseband_processor.cpp:240`，跑在 lower-PHY RX executor 上；
  UHD(B200/USB3) 路径在 OCUDU 内**零拷贝**，ZMQ 路径有 4 次拷贝 + 互斥环。
- FFT/时频转换在 CPU（`lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:52-53`，
  `modulation/ofdm_demodulator_impl.cpp:107-131`），FFT 输出经 `grid.put` 落 CPU 资源网格。
- CE/LDPC 的 Metal 实现**都是「同步 commit + waitUntilCompleted」**：CE 每 (port, slot) 2–4 个命令缓冲、
  LDPC 每解码 1 个命令缓冲 + 290 次 dispatch（layered BG1/mi=6）；**两个模块之间没有任何 GPU 侧数据交接**——
  CE 结果必须先落回 CPU 网格（`port_channel_estimator_metal_mmse_impl.cpp:678-689` 的 `gpu_h→grid_est` 解包），
  均衡/解调再在 CPU 上消费。
- FAPI 出口是**进程内 fastpath 同步调用链**（无队列、无线程跳变），整条 RX FAPI 链上**只有一次 TB 拷贝**
  （`lib/fapi_adaptor/mac/p7/fapi_to_mac_indications_fastpath_translator.cpp:88` 的 `byte_buffer::create`）。
- 因此真正的重构对象不是「拷贝」（现网拷贝已经很少），而是：
  **（a）模块间 GPU↔CPU 往返**（CE→均衡→解调→LDPC 的中间态回传 CPU）；
  **（b）同步 wait**（每模块每槽 1–4 次阻塞执行器线程）；
  **（c）CPU 侧打包/解包**（LLR 拷贝打包、逐 bit 重打包、网格解包、CPU 求逆/相关矩阵）；
  **（d）以太网/USB 协议栈**（ZMQ 4 拷贝、UHD USB 传输、split-7.2 的 eCPRI 解包）。

### 0.4 五阶段总览

| 阶段 | 主题 | 核心交付物 | 门禁（DoD） |
|---|---|---|---|
| 一 | 管线扫描与 Dispatch 开销诊断 | 逐瓶颈量化报告（B1–B14）+ 目标生命周期图 | 每个瓶颈有测量值与消除归属 |
| 二 | PCIDriverKit 与 DMA 零拷贝底座 | `ru_fpga_dma` RU 单元 + dext + UMA 环形缓冲 + `MTLBuffer` 零拷贝包装 | FPGA DMA 写入的 I/Q 被 GPU kernel 直接读取，全程 0 次 `memcpy` |
| 三 | Metal Kernel 串联与单提交调度 | FFT/均衡 Metal 化 + 全链单命令缓冲 + `addCompletedHandler` 回调协议 | 每槽 1 次 commit、1 次回调、中间态 0 次回传 CPU |
| 四 | 张量化与 ANE 深度融合 | 8×8 `simdgroup_matrix` 网格重排 + Hybrid Routing 决策器 | `metal_nn_mmse` v2 达成 A/B 门禁；ANE 与 GPU 链可并行/竞争 |
| 五 | 高速 FAPI 组装与 MAC 交互 | GPU 内 FAPI 记录直写 + 极轻量 MAC 唤醒 | CRC=OK 的 PDU 从 GPU 到 MAC 调度 ≤ 1 次线程跳变、0 次全量拷贝 |

### 0.5 端到端 KPI（重构验收线）

| KPI | 现状（实测口径） | 阶段二后 | 阶段三后（终态） |
|---|---|---|---|
| I/Q 进入 PHY 前的协议栈 | USB3(UHD)/TCP(ZMQ) | **TB5 PCIe DMA** | TB5 PCIe DMA |
| I/Q 全程 `memcpy` 次数 | 0（UHD）/ 4（ZMQ） | **0** | **0** |
| 每槽 (port) 命令缓冲提交数 | 2–4（CE）+ 1×CB（LDPC） | 同左 | **1（全链）** |
| CPU 阻塞等待 GPU | 每次提交 `waitUntilCompleted` | 同左 | **0**（单次 `addCompletedHandler`） |
| CE 输出→LDPC 输入的中间态回传 CPU | 是（网格解包→CPU 均衡） | 是 | **否（GPU 内部流转）** |
| CE 阶段时延 | ~255 µs（metal_mmse, 36PRB/3DMRS） | 不变 | **≤ 30 µs**（GPU 内链式，无往返） |
| LDPC 阶段时延 | ~730 µs 中位（persistent 实链） | 不变 | **≤ 50 µs**（小 TB）/ ≤ 200 µs（大 TB，无 CPU 编码开销） |
| 槽内 CPU 占用 | 高（编码 290 dispatch + 打包/解包） | 不变 | 每槽 ≤ 1 次任务（回调） |
| E2E（I/Q 入 → FAPI 出） | 管线中位 ~1133 µs（ZMQ 口径） | — | **≤ 200 µs（30 kHz 0.5 ms 槽预算内）** |

---

## 1. 现状基线盘点（2026-09 全库扫描结论）

> 本节所有 file:line 均以 `apple-silicon` HEAD `b30a167178` 为准，由三个独立深扫 + 主链自读交叉验证。

### 1.1 当前 E2E 拓扑与 RX 数据通路

macOS 上两条既有实链：
- **真 OTA**：`gnb -c configs/gnb_uhd_oaiue.yaml`（`device_driver: uhd`，B200/B210，USB 3.0，`otw_format: sc12`），
  验证记录见 `doc_chinese/macos_compat_refactor/phase5_report.md:25-31` 与 `configs/OAI_UE_ZMQ_UHD_E2E_memo.md`。
- **时延压测**：`gnb -c configs/gnb_zmq.yaml`（`device_driver: zmq`，TCP 前传），`tests/ci/macos_e2e/README.md`。

```
                    split 8 (ru_sdr)                            split 7.2 (ru_ofh)
  空中 I/Q ──> AD9361/B200 ──> UHD(USB3) ──┐          RU: eCPRI/UDP ──> DPDK/以太网 ──> PCIe DMA
                                            │                                     │
                          ZMQ(TCP) ──> 4×拷贝+互斥环 ──┐                            │
                                                      ▼                            ▼
                                     lib/radio::baseband_gateway_receiver::receive()
                                            (唯一调用点: lower_phy_baseband_processor.cpp:240,
                                             跑在 lower-PHY RX executor / phy_worker 上)
                                                      │
                              CPU: CFO补偿 → OFDM 解调(FFT, ci16→cf32+grid.put)
                              (puxch_processor_impl.cpp:52-53, ofdm_demodulator_impl.cpp:107-131)
                                                      │ on_new_uplink_symbol (ru_adapters.h:79)
                                                      ▼
                        upper_phy_rx_symbol_handler_impl.cpp:33  (无执行器, 内联调用方线程)
                                                      │
                        uplink_processor_impl::handle_rx_symbol → process_symbol_pdus
                        (FSM 锁; rm_buffer_pool.run_slot; 按末符号扇出)
              ┌───────────────┬────────────────┬─────────────────┬──────────────┐
              ▼               ▼                ▼                 ▼              ▼
      prach_executor   pucch_executor    pusch_executor    srs_executor   (PRACH 用 execute
      (execute 阻塞)   (defer, 高优)    (defer, 中优)      (defer, 中优)     阻塞在调用线程)
                                              │  uplink_processor_impl.cpp:315
                                              │  rx_payload_pool.acquire_payload_buffer(:302)
                                              ▼
                     pusch_processor_impl::process (:119-196, pusch 线程)
                       └─ dmrs_pusch_estimator: 每 RX 端口一个 defer 到
                          pusch_ch_estimator_executor (dmrs_pusch_estimator_impl.cpp:52-67)
                            └─ [CE: cpu | metal_mmse | metal_nn_mmse | helena(ANE)]
                            └─ on_estimation_complete → process_data (:198-409, 估计器线程)
                                 ├─ 均衡+解调 → LLR → 速率匹配软缓冲 (pusch_decoder_buffer)
                                 └─ LDPC: 单码块 TB 内联解码; 多码块才 defer 到
                                    pusch_decoder_executor (pusch_decoder_impl.cpp:392-401)
                                     └─ [LDPC: auto(neon) | metal | metal_flooding |
                                          metal_persistent | metal_async | metal_lls]
                                     └─ join_and_notify (:404-524): TB 拼接 + CRC24A
                                                       │ on_sch (uplink_processor_impl.h:94-117)
                                                       │ CRC 门控 :108 (span 置空即 KO)
                                                       ▼
        upper_phy_rx_results_notifier_wrapper (内联转发)
          → phy_to_fapi_results_event_fastpath_translator.cpp:105
            ├─ notify_crc_indication (:213-248)   ← CRC 是独立消息, tb_crc_status_ok:22
            └─ notify_rx_data_indication (:250-264) ← span 赋值, 无拷贝
              → fapi_to_mac_indications_fastpath_translator.cpp:80-104
                 ├─ byte_buffer::create(span) :88   ← 全链唯一 TB 拷贝
                 └─ mac_ul_processor::handle_rx_data_indication (同步调用, :99)
                    └─ mac_ul_pdu_executor(ue_index) per-UE strand (medium 池) :118-121
                       └─ pdu_rx_handler: 解子 PDU → RLC / MAC CE → 调度器
```

### 1.2 CPU 调度与线程模型（执行器全景）

**执行器基座**（`include/ocudu/support/executors/`，实现 `lib/support/executors/`）：

- `task_executor::execute()`（可内联）/ `defer()`（必入队）契约（`task_executor.h:10-21`）。
  **信道处理永远走 `defer`**（`uplink_processor_impl.cpp:315,345,461`），只有 PRACH 用 `execute`（`:248`）。
- 六种队列策略（`adt/detail/concurrent_queue_params.h:23-30`）+ 三种等待策略（cv / sleep / non_blocking），
  组合被硬编码（`task_execution_manager.cpp:174-330`）：`locking_mpsc`→cv 或 sleep；`lockfree_*`→必须 sleep；
  池的 `moodycamel_*`→sleep。
- 内联规则（决定「谁在跑你的任务」）：单 worker executor 仅当调用者即 worker 线程内联
  （`task_worker.h:129-134`）；**`task_worker_pool_executor` 永不内联**（`task_worker_pool.h:228-233`）；
  优先池仅 `prio==max` 内联（`task_worker_pool.h:267-280`）。
- `unique_thread`：15 字符截断命名、256 槽线程索引池、macOS 下 16 MiB 栈
  （`unique_thread.cpp:238,244-256`）；`thread_resource_preinitializer` 观察者
  （`worker_manager.cpp:22-40`）预初始化 byte-buffer TLS 与定时器队列。

**gNB 执行器清单**（`apps/services/worker_manager/worker_manager.cpp`，队列深度 2048，backoff 50 µs）：

| 执行器 | 承载 | 备注 |
|---|---|---|
| `main_pool::rt_prio_exec` | upper-PHY **DL**、MAC 调度、OFH DL/UL strand | `moodycamel_lockfree_bounded_mpmc`, batch 16 |
| `main_pool::high_prio_exec` | 控制面/定时器 | moodycamel 无界 |
| `main_pool::medium_prio_exec` | PCAP/CU-UP、**PUSCH/SRS/信道估计/LDPC 解码**、MAC per-UE UL strand | moodycamel 无界 |
| `main_pool::low_prio_exec` | 外部网络 RX | bounded, batch 16 |
| `phy_exec`（sequential 模式） | 全部 upper PHY | `locking_mpsc`+cv，`phy_worker` 单线程 |
| `radio_exec` | ZMQ 收发驱动 | `lockfree_mpmc`, sleep 50 µs, RT `max()-1` |
| `lower_phy#N` / `lower_phy_tx#N` / `lower_phy_rx#N` / `lower_phy_ul#N` | split-8 低 PHY 剖面（single/dual/triple） | `lockfree_mpmc`, sleep 1–10 µs, RT |
| `ru_timing` / `ru_txrx_#N` | split-7.2 OFH | `locking_mpsc` q=4 / `lockfree_mpmc` 1 µs |

**du_low 映射**（`lib/du/du_low/du_low_executor_mapper.cpp:87-127`）：PUCCH/PRACH→non-RT 高优；
**PUSCH 信道估计/PUSCH/PUSCH 解码/SRS → `non_rt_medium_prio_exec` 的 fork-limiter 三条腿**（batch 16/1/…）。
关键事实：**单码块 TB 的 LDPC 解码内联在信道估计线程上执行**（`pusch_decoder_impl.cpp:392-401`，
`nof_codeblocks > 1` 才 defer）——这意味着现网 Metal LDPC 的 `waitUntilCompleted` 经常阻塞的是
medium 池里正在跑估计器的那个 worker。

**macOS 实时性事实**：CPU 亲和性是 no-op（`macos_compat.h:165-172`）；`apply_worker_thread_scheduling`
映射 QoS class（RT 意图→`USER_INTERACTIVE`）+ Mach affinity tag（`:142-149`）；可选 Mach time constraint
（period=computation=constraint=1 ms，`:117-125`）。**本蓝图的实时性杠杆 = QoS + Mach 约束 + 把重活移出 CPU。**

### 1.3 已落地 Metal/ANE 模块现状

#### 1.3.1 LDPC Metal 解码器（`lib/phy/upper/channel_coding/ldpc/metal/`）

- **资源模型**：device/command queue/pipeline 按**算法家族进程级单例**共享
  （`ocudu_metal_decoder_engine.mm:196-225`）；per-(BG,Z) 引擎槽持有缓冲
  （`ldpc_decoder_metal.cpp:127-138`）；全 102 槽构造期预建（`:318-324`，启动 ~12 ms）。
- **零拷贝路径**：宿主机 4 KB 对齐 LLR 暂存区被 `newBufferWithBytesNoCopy`（`Shared`）包装并按指针缓存
  （`ocudu_metal_decoder_engine.mm:150-167`，长度**向上取整到 4 KB**，缓存命中**不校验长度**）。
- **每解码的上传** = 1×`memset` + 2×`std::fill`（打孔/尾擦除偏置）+ 1×`memcpy`
  （`ldpc_decoder_metal.cpp:507-510`）；int8→fp16 转换在 GPU（1 次 dispatch）。
- **调度形态**：每解码 1 个命令缓冲、1 次 commit、**1 次 `waitUntilCompleted`**（`:520-521,749-751`）；
  layered 全链展开 = **290 dispatch**（BG1, mi=6：46 层 + syndrome + ET 门 × 6 轮），
  实测 ~1.9 µs/dispatch ≈ **550 µs 编码地板**（`PLAN.md:1345-1347`）；`metal_persistent` 单 dispatch
  但每轮 46 次 × ~8.7 µs device 级 barrier（`PLAN.md:1458-1461`）。
- **结果回读**：`buf_llr_fp16.contents` 逐 bit 提取硬判决（`.mm:778-781`）+ CPU 侧重算最终 syndrome
  （`.mm:794-797`，因为 kernel 每轮清零 `error_count`）+ 适配器**逐 bit** `output.insert` 重打包
  K 位（`ldpc_decoder_metal.cpp:521-523`，z=384 时 8448 次虚调用）+ CPU 侧 CRC（`:527-531`）。
- **并发约束**：全家族共享**一条** MTLCommandQueue ⇒ 不同池线程的解码按提交序串行；
  `decode_mtx` 贯穿整个 GPU 往返（`:443`）。**任何尺寸下 GPU 解码时延都无 CPU 交叉点**
  （`PLAN.md:569-573`，最优 1.9×）——单模块 GPU 化在「提交+等待」范式下天然劣势，这正是本蓝图要消灭的范式本身。

#### 1.3.2 MMSE 信道估计 Metal（`lib/phy/upper/signal_processors/channel_estimator/metal/`）

- **资源模型与 LDPC 相反**：**每个估计器实例私有 device + queue**（`ocudu_metal_mmse_engine.mm:148-152`），
  实链 10 实例 = 10 队列（`Metal_MMSE_Channel_Estimator_PLAN.md:672`）；待办 (D) 已点名
  「引擎单例 + 跨端口批处理 + 持久 command buffer」。
- **CPU/GPU 分工**：每 (hop, port)——CPU 跑 `estimate_sigma2`（`:129-183`）、
  `build_correlation_matrices`（`:185-250`，R=R_t⊗R_f 全部 CPU）、~~**`gauss_jordan_invert` 每层一次**~~
  （v1 各层统计相同却重复求逆最多 4×）、全部 staging/解包（`:572-712`）；
  GPU 只跑 K1b（W=R_hp·A⁻¹）+ K2（h=W·y）两个 dispatch（`run_weights_only`/`run_nn`）。
  ~~GPU 求逆 kernel `mmse_inv` 存在但**刻意不用**（实测 369-382 µs vs CPU µs 级，PLAN:544-548）。~~
  **⇒ 已更新（`9a651e4210` S-5a）**：K1 重写为分块 Gauss-Jordan（b=8，无选主元，A 对称正定）后
  **已回到 GPU 链并成为默认**（单个 36×36 系统 91.3 → **24.5 µs**；本机 2026-09-14 基线 44 µs），
  与 K1b/K2 同在 `engine->run()` 的一条 CB 里；`OCUDU_CE_CPU_INVERT=1` 仅作 A/B 保留。
  **2026-09-13 复测**：K1 并行化后单个 36×36 系统仍需 91.3 µs GPU，CPU Gauss-Jordan 同矩阵 ~10 µs。
  **但求逆仍必须回到 GPU 链上**（目标：PHY compute 全走 Metal path，见 §0.1；CPU↔GPU 按大粒度划分，
  不用局部 µs 定方向）。91.3 µs 是**核函数缺陷**：一个 hop 内 9 个块共用同一个 A ⇒ 每 hop 只求逆 1 个
  36×36 = 47k FLOPs，却排了 72 个枢轴步 × 2 次屏障、消元逐元素穿 threadgroup 内存。
  **修法**：块化右视消元（b=8）+ `simdgroup_matrix` ⇒ 屏障 ~10 次、更新走寄存器，目标 ≤10–15 µs 延迟，
  并与 K1b/K2 同 CB（`engine->run()` 已具备）。CPU 求逆（`OCUDU_CE_CPU_INVERT`）与 CPU 尾块
  （`OCUDU_CE_TAIL_CPU`）是**过渡桥**，S-5 抹掉。
- **提交形态**：~~标准块 + 尾块**各一次** commit+wait~~ ⇒ ~~2026-09-13 起尾块（≤ block_prb-1 PRB）走 CPU
  兜底块路径~~（`OCUDU_CE_TAIL_CPU=1`，仅 A/B）⇒ **2026-09-14（`450e2988dd` S-5c）：尾块并入标准批次的同一条
  command buffer**（尾块作为标准批次的额外 system，A 填充为 blockdiag(A_e, I)、R_hp 填充为 [R_hp_e | 0]），
  **每 (port, slot) 恰好一次引擎调用 / 一次 commit+wait**；实测每 hop **省 ~100 µs**
  （25 PRB/2 DMRS 276.7 → 170.6 µs，NMSE 与 split 路径**逐位一致**，单测 Test 11 常驻对拍）。
  `OCUDU_CE_SPLIT_TAIL=1` 保留两条 CB 的旧路径做 A/B；`merged_batch_last()` 可观测合并是否真的发生。
  下一步（深水区 S-5c）：CE 输出留 GPU（重排 dispatch 后直接给 eq 消费）+ CE dispatch 并入 slot burst。
  `cache_mutex` 保护 size-blind 的指针缓存（`.mm:40-58`），最大尺寸靠构造期满容量 warm-up 隐式确立
  （`.cpp:102-116`）。
- **已知隐患（重构前必修）**：~~热路径**忽略** `run_nn/run_weights_only` 返回值（`.cpp:670-675`）——
  wrap/commit 失败会静默产出**过期信道估计**且 `hop_gpu=1` 照报。~~
  **⇒ 已修（S-1 审计）**：`engine_run()` 检查返回值、记 error 并按批回退到 CPU 参考路径。
  缓存命中不校验长度同样需要加断言。
- **实测**：gpu_wait 765→25.3 µs（K1b 占用率修复后）；CE E2E 稳态中位 ~255 µs / p99 457 µs
  （36 PRB/3 DMRS）；首槽 ~2.6–3 ms = **per-thread** Metal 驱动初始化税（构造期 warm-up 跑在主线程，
  盖不住执行器线程，`Metal_MMSE_Channel_Estimator_PLAN.md:793-805`）。

#### 1.3.3 `metal_nn_mmse`（simdgroup_matrix 8×8，A/B 孪生）

- 与 `metal_mmse` 共用同一实现类，仅 `use_matrix_engine` 布尔区分（`factories.cpp:57-69`）；
  `mmse_weights_matrix`/`mmse_apply_matrix` 用 `simdgroup_matrix<float,8,8>` 硬件矩阵单元。
- **零填充策略在宿主侧完成**：`Lp=ceil8(L)`、`Np=ceil8(nout)`，kernel 无分支
  （`ocudu_mmse_weights_matrix.metal:11-28`）；apply 侧把 4 块×(re,im) 打包成 8 列走 GEMM
  （`ocudu_mmse_apply_matrix.metal:12-14,60-81`）。非 8 对齐维度（L=54→Lp=56）已过 A/B 对拍
  （单测 Test 8，`port_channel_estimator_metal_mmse_unit_test.cpp:1062`）。

#### 1.3.4 ANE/CoreML HELENA（AI 信道估计）

- 预编译 `.mlmodelc` + `MLComputeUnitsAll`（ANE>GPU>CPU，`ocudu_coreml_nn_engine.mm:137-145`）；
  输入输出 `initWithDataPointer:…:deallocator:nil:` + `outputBackings` **双向零拷贝**
  （`:76-101`）。
- 每引擎一条专用 worker 线程，调用侧 mutex+condvar 阻塞往返（`:219-231`），无超时；
  每 (port, hop, layer) 一次同步 `predictionFromFeatures:options:error:`（`:103-113`）。
- 每推断 ~10 个 ObjC 临时对象、worker 线程**无 `@autoreleasepool`**（结构性泄漏隐患）；
  2 s 间隔 keep-alive 防 ANE 深度掉电（`:47-51`）；per-thread ANE 首调 ~12 ms 税已用构造期 warm-up 转移。
- **实测**：ANE p50 **141 µs**（612 子载波 batch1，含往返拷贝口径）；106-PRB 模型 191 µs；
  实链 265–451 µs/授权。**优于 metal_mmse 基线（~255 µs）**——ANE 是 CE 的可行主路径，
  也是第四阶段 Hybrid Routing 的天然竞争者。

#### 1.3.5 构建与 CLI 路由（现状接口）

- CMake：`ENABLE_METAL_CHEST` / `ENABLE_METAL_LDPC`（默认仅 Apple arm64 ON，`CMakeLists.txt:23-38`）；
  `ocudu_add_metallib()`（`cmake/modules/ocudu_metal.cmake:50-74`）离线 `xcrun metal/metallib`，
  产物**回写源码树**（git 忽略）；`.mm` 按文件 `-fobjc-arc`（各 `metal/CMakeLists.txt`）；C++/ObjC 边界 =
  `void* impl` pimpl + 手工 ABI 结构体双写（无 static_assert 强制，`ocudu_metal_decoder_engine.mm:25-36`）。
- CLI（`expert_phy` 子命令，`apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.cpp`）：
  `--pusch_ldpc_decoder_type {auto,generic,neon,avx2,avx512,metal,metal_flooding,metal_persistent,metal_async,metal_lls}`
  （`:167-175`，默认 auto→**NEON**，Metal 全系显式 opt-in）；`--pusch_channel_estimator_algo {cpu,metal_mmse,metal_nn_mmse,helena}`
  （`:188-193`）；`--pusch_ldpc_decoder_offset [-1,64]`；`--pusch_channel_estimator_helena_model_path{,_52,_106}`。
  非 Apple 构建由 `du_low_config_translator.cpp:79-83,92-99` 把开关重写为 `cpu`/`auto`。
- 已知小 bug（顺手修）：`upper_phy_factories.cpp:768-769` 致命错误消息打印 `crc_calculator_type`
  而非 `ldpc_decoder_type`；度量装饰器路径二次建厂时丢 106-PRB HELENA 路径（`:657-664`）。

### 1.4 Dispatch 与拷贝瓶颈清单（B1–B14）

> 编号在全文通用。每个瓶颈标注「消除阶段」：②=PCIDriverKit/DMA，③=Kernel Chaining，④=张量/ANE，⑤=FAPI。

| # | 位置（file:line） | 性质 | 量级/口径 | 消除阶段 |
|---|---|---|---|---|
| **B1** | `radio_zmq_rx_channel.cpp:144,224,298` + `radio_zmq_rx_stream.cpp:87-89` | 以太网协议栈：zmq_recv→staging→互斥环→cf32→ci16，4 拷贝 + 锁 | ~39 MiB/channel staging（默认 614400×8）；每次收包 3 次跨线程/锁 | ② |
| **B2** | `lower_phy_baseband_processor.cpp:240` 拉模型 | I/Q 消费绑死在 lower-PHY executor 线程，DMA 到达无异步事件 | 每槽 1 次阻塞 `receive()` | ② |
| **B3** | `uplink_processor_impl.cpp:194,214-216` + `ofdm_demodulator_impl.cpp:107-131` | CPU FFT：ci16→cf32 转换 + `grid.put` 落 CPU 网格 | 每符号 2×转换 + FFT（fftz/vDSP 路径） | ③ |
| **B4** | `dmrs_pusch_estimator_impl.cpp:52-67` | 每端口一次 `defer` 到 ch-estimator 腿（fork-limiter），CE 与均衡/解码分线程 | 每 (port, slot) 1 次线程跳变 + 2 次队列 | ③（并入单链后消失） |
| **B5** | `port_channel_estimator_metal_mmse_impl.cpp:185-250,642-668` | CPU 相关矩阵 + **每层重复** Gauss-Jordan 求逆（v1 各层同统计） | 6–14 µs×hop + L×L 求逆×层数 | ③（求逆进 GPU 链）：**方向不变**；前置条件是先把 K1 从 91.3 µs 修到 ≤10–15 µs（块化 + `simdgroup_matrix`，见 §1.3.2 复测），否则只是把延迟搬个地方 |
| **B6** | `ocudu_metal_mmse_engine.mm:211,255,326,390,482` | CE 每 (port,slot) **2–4 次 commit+waitUntilCompleted** | commit+wait ≈ 30–90 µs/次 | ③ |
| **B7** | `ocudu_metal_mmse_engine.mm:148-152` + 实例私有 queue | 10 实例 10 队列，无法跨端口批处理、无法与 LDPC 共链 | — | ③ |
| **B8** | `ocudu_metal_mmse_engine.mm:40-58` + `.cpp:670-675` | size-blind 指针缓存；热路径忽略引擎返回值 → 失败静默产出过期 CE | 正确性隐患 | ②/③（重构前必修） |
| **B9** | `ldpc_decoder_metal.cpp:507-510` | LLR 打包：memset+2×fill+memcpy 进对齐暂存区（虽零拷贝包装，仍是 CPU 往返内存） | n_aligned(≈26 KB@z384) 写 | ③（均衡/解调直接写 GPU 布局，免打包） |
| **B10** | `ocudu_metal_decoder_engine.mm:749-751` | 每解码 1 次 commit+waitUntilCompleted；290 dispatch 全 CPU 编码 | ~550 µs 地板（PLAN.md:1345-1347） | ③ |
| **B11** | `ocudu_metal_decoder_engine.mm:778-781,794-797` + `ldpc_decoder_metal.cpp:521-523,527-531` | 结果回读：逐 bit 硬判决 + CPU 重算 syndrome + 逐 bit 重打包 + CPU CRC | 8448 次虚调用@z384 + m_aligned 字扫描 | ⑤（GPU 直写 TB + CRC24A kernel） |
| **B12** | `fapi_to_mac_indications_fastpath_translator.cpp:88` | **全链唯一 TB 拷贝**：`byte_buffer::create` 从 PHY payload 池拷入 segment 池 | TBS ≤ 几十 KB | ⑤（span 直通或零拷贝 byte_buffer） |
| **B13** | `mac_ul_processor.cpp:118-121` | MAC 消费走 per-UE strand（medium 池）队列 | 每 PDU 1 次队列 | ⑤（可优化为直呼，见 6.3） |
| **B14** | 全模块 | **模块间无 GPU 交接**：CE→均衡→解调→LDPC 每级落回 CPU 再上 GPU | 每槽 ≥2 次主机往返 | ③ |

### 1.5 目标生命周期图（重构终态）

```
 空中 ──> AD9361(FMC) ──> FPGA: DDC/组帧 + 硬件时间戳 + XDMA DMA IP
                                        │ (ASM2464PD PCIe↔USB4 桥, TB5 线缆, 80Gbps)
                                        ▼
                        Mac mini M4 Pro 统一内存 (UMA) DMA 环形缓冲
                        ┌────────────────────────────────────────────┐
                        │ ① dext(PCIDriverKit) 申请 IOBufferMemoryDescriptor │
                        │    连续物理内存, FPGA 直接写入; 用户态 CreateMappingInTask │
                        │ ② 应用: newBufferWithBytesNoCopy → MTLBuffer  │
                        │    (StorageModeShared, 零拷贝, 每槽一次包装)      │
                        │ ③ CPU: 读 FPGA 时间戳 Header → 槽对齐 → 编码 1 个 │
                        │    命令缓冲(全链 kernel + MTLEvent 依赖) → commit │
                        │    → 立即返回(不等!)                              │
                        │ ④ GPU 内部闭环:                                  │
                        │    [等待时间戳 event] → FFT → RG 抽取 → MMSE CE    │
                        │    (8×8 simdgroup) → MIMO 均衡 → 解调 LLR          │
                        │    → LDPC(单 dispatch 或事件链) → CRC24A           │
                        │    → FAPI 记录直写 UMA 环                         │
                        │ ⑤ addCompletedHandler: 轻量置位 → executor 入队    │
                        └────────────────────────────────────────────┘
                                        │ (0 次数据拷贝, 1 次线程唤醒)
                                        ▼
              MAC per-UE strand 消费 FAPI 记录 → MAC PDU → RLC/调度
```

数据生命周期八步：**DMA 写 UMA（FPGA→RAM）→ 零拷贝包装（无 memcpy）→ 单次提交（CPU 1 次编码）
→ GPU 内部状态流转（中间态绝不出 GPU）→ GPU 完成回调（1 次唤醒）→ 轻量 FAPI 展开（CPU 读 8 字节元数据）
→ MAC 提取（span 直通）→ 槽尾统一释放**。CPU 在整个数据面上只剩：时间戳对齐、一次 ICB/命令缓冲编码、
一次回调处理；控制面（PUCCH/PRACH/调度）继续走现有 CPU 执行器体系。

---

## 2. 第一阶段：管线扫描与 Dispatch 开销诊断（Pipeline Audit）

> 目标：把 §1.4 的 B1–B14 从「代码审查结论」升级为「逐项可复现的量化基线」，并为第三阶段的改造确立前后对照口径。

### 2.1 复用现有探针：`ul_pipeline_probe`

项目已有现成的分段探针（`include/ocudu/support/executors/ul_pipeline_probe.h`，
`ENABLE_FLOW_PROBES` 构建开启），挂点完整覆盖 RX 主链：

- `record_start`：I/Q 收齐（`lower_phy_baseband_processor.cpp:261`）
- `record_t2f_end`：整槽 FFT 完成（`puxch_processor_impl.cpp:63`）
- `record_ce_end`：PUSCH 信道估计完成（`pusch_processor_impl.cpp:212`）
- `record_ldpc_start`：首码块解码开始（`pusch_decoder_impl.cpp:356`）
- `record_end_crc_ok`：TB CRC 通过（`pusch_processor_notifier_adaptor.h:239`）

**阶段一动作 A1**：在 `ul_phase_durations` 中新增两个分段——`eq_dem`（均衡+解调，从
`record_ce_end` 到 `record_ldpc_start` 的差值）与 `fapi_mac`（从 `record_end_crc_ok`
到 `mac_ul_processor.cpp:118` 入队完成，在 fastpath 翻译器前后各打一点）。
这使「t2f / ce / eqdem / ldpc / fapi 出口」五段形成完整闭环，可直接对拍第三阶段的 GPU 链。

**阶段一动作 A2**：为 Metal 侧补齐三个一次性统计：
1. **每槽命令缓冲计数**：在 `ocudu_metal_mmse_engine.mm` 与 `ocudu_metal_decoder_engine.mm`
   的 commit 点挂计数器（`g_cb_commit_count` 原子计数，`ENABLE_METAL_STATS=ON` 构建时打印每槽均值）；
2. **编码 vs 等待拆分**：复用 `last_gpu_us`（GPUStartTime/EndTime）与总墙钟之差 =
   CPU 编码 + 等待 + 打包/解包；
3. **队列深度**：`waitUntilCompleted` 返回后记录同队列在飞命令缓冲数（引擎内计数即可）。

### 2.2 逐瓶颈测量方案

| 瓶颈 | 测量方法 | 目标口径 |
|---|---|---|
| B1 (ZMQ 4 拷贝) | `ENABLE_FLOW_PROBES` + `radio_exec` 线程上 `os_signpost` 四点（zmq_recv 返回/入环/出环/convert 完） | 证明「4 拷贝 + 锁」贡献 ≥ X µs/槽 |
| B2 (拉模型) | 记录 `receive()` 阻塞时长分布（lower-PHY 线程） | 每槽阻塞 p50/p99 |
| B3 (CPU FFT) | t2f 分段已覆盖；另用 Instruments 的 `os_signpost` 圈出 `ofdm_demodulator_impl` 单符号开销 | 每符号 µs |
| B4 (估计器线程跳变) | `dmrs_pusch_estimator_impl.cpp:63` defer 前后打点 | 入队+唤醒 µs |
| B5 (CPU 求逆/相关) | 在 `run_engine_blocks` 内对 `build_correlation_matrices`/`gauss_jordan_invert` 计时（已有点位可扩） | 每 hop µs |
| B6 (CE commit+wait) | A2 计数器 + `last_gpu_us` 拆分 | 每槽 CB 数、编码 µs、等待 µs |
| B7 (10 队列) | 打印引擎实例数（已有日志）并统计跨端口序列化损失 | 实例数→合并后的预期收益 |
| B8 (返回值忽略) | **静态修复前置**：给 `run_nn/run_weights_only` 加返回值检查 + 缓存命中长度断言，跑单测 Test 8 回归 | 0 静默失败 |
| B9 (LLR 打包) | `decode()` 入口到 `engine->decode` 之间计时 | 打包 µs |
| B10 (290 dispatch) | `last_gpu_us` 与整段墙钟差值；persistent 变体对照 | 编码地板 µs |
| B11 (逐 bit 回读/CRC) | 结果段计时（wait 返回 → `decode()` 返回） | µs |
| B12 (唯一 TB 拷贝) | fastpath 翻译器 `byte_buffer::create` 前后打点 | 每 PDU µs |
| B13 (MAC strand) | `mac_ul_processor.cpp:118` execute 前后 | 入队 µs |
| B14 (模块间往返) | 综合：CE 解包段 + 均衡/解调段 + LDPC 打包段之和 | 证明「GPU 内链式」可消除的总量 |

### 2.3 阶段一交付物与门禁

- **交付物 1**：`doc_chinese/full_gpu_chain/pipeline_audit_2026-09.md`——每个瓶颈一行「位置 / 测量值 / 消除归属 / 回滚风险」，
  作为后续四个阶段的验收基线快照。
- **交付物 2**：目标生命周期图（§1.5 的定稿版，细化到 kernel 列表与缓冲清单）——本蓝图 §4 的图即初稿。
- **门禁**：B8 修复合入（两个引擎的缓存长度断言 + MMSE 返回值检查 + `upper_phy_factories.cpp:768-769`
  错误消息修正）；审计数据在 30 kHz 20 MHz（51 PRB）与 15 kHz 各采 ≥ 200 槽。

---

## 3. 第二阶段：基础环境与 PCIDriverKit 构建（DMA & Zero-Copy）

> 目标：把「空中 I/Q → UMA」这段彻底搬出以太网/USB 协议栈，并在用户态拿到
> 可被 `newBufferWithBytesNoCopy` 直接包装的物理连续内存。

### 3.1 硬件拓扑与带宽预算

```
 ┌────────────┐   FMC/CMOS    ┌──────────────────────────────────────────┐
 │ AD9361     │ ────────────> │ FPGA:                                     │
 │ (2RX/2TX,  │               │  DDC/组帧/1PPS+10MHz 驯服 → 硬件时间戳引擎 │
 │  FMC 子卡)  │               │  └─ XDMA/DMA IP (PCIe Gen3/4 x4 Endpoint) │
 └────────────┘               │     C2H 描述符环 + BAR0 寄存器窗           │
                              └───────────────────┬──────────────────────┘
                                                  │ PCIe x4
                              ┌───────────────────▼──────────────────────┐
                              │ ASM2464PD 桥接裸板 (PCIe↔USB4/TB)         │
                              └───────────────────┬──────────────────────┘
                                                  │ Thunderbolt 5 线缆
                                                  ▼
                                   Mac mini M4 Pro (PCIe EP 直通呈现)
```

**带宽预算（20 MHz/30 kHz 场景）**：23.04 Msps × 4 B/样点 (ci16) ≈ **92 MB/s 每 RX 流**，
2RX + 2TX 全双工 ≈ 368 MB/s ≈ 3 Gbps 量级；Thunderbolt 5 单向 ≥ 60 Gbps、PCIe Gen3 x4 ≈ 32 Gbps——
**裕度 > 10×**，瓶颈绝不在链路而在软件。50 m/s 自动驾驶场景的附加约束是**时延抖动**而非带宽：
DMA 到达必须可预测（定长块 + 硬件时间戳 + 环形缓冲无分配）。

**时钟体系（关键设计决策）**：
- FPGA 侧以 **1PPS + 10 MHz**（GNSS 驯服）为根时钟，维护 64-bit 纳秒计数器 + slot 计数器
  （10 MHz 计 0.1 µs/拍，slot = 由 SCS 配置字换算的计数器窗口）。
- **时间戳由硬件打在每块 DMA Header 上**（FPGA 写 Header 时锁存计数器），CPU/GPU 只消费时间戳，
  绝不在软件里推算到达时刻——这是把「空口帧对齐」从 CPU 调度器中彻底剥离的前提。
- 现有 SDR 路径的时序源（`ru_controller_sdr_impl.cpp:39-61` 的样本时间戳→`on_tti_boundary`）
  在 FPGA 路径中被「Header 时间戳 → `slot_point` 换算」替代（§3.6）。

### 3.2 软件侧：切断以太网/USB 接收路径

**原则：不删、只断。** UHD/ZMQ/OFH 三条腿全部保留为回退与 A/B 对照腿（§7 里程碑 M1 明确要求
「同配置双腿可切换」），新增第四条腿 `fpga_dma`。

1. **新增 RU 单元 `ru_fpga_dma`**（`lib/ru/` 下新增目录，结构参照 `lib/ru/sdr/`）：
   - 复用 `ru_uplink_plane_rx_symbol_notifier` / `ru_timing_notifier` / `ru_downlink_plane_handler`
     全套适配器（`include/ocudu/ru/ru_adapters.h`），保证 upper PHY 侧零改动；
   - 实现 `baseband_gateway_receiver::receive()`：**从 DMA 环形缓冲的「已就绪」槽返回一个
     ci16 跨度视图**（指向 UMA 映射区，零拷贝），并把该槽的硬件时间戳换算为 `metadata.ts`。
     这一步让 split-8 的 lower PHY（含 CPU FFT 回退腿）在阶段二就能跑起来——
     **阶段二验证「DMA 底座」，不验证「GPU 链」**，两条线解耦。
2. **radio factory 注册**：`lib/radio/radio_factory.cpp:32-42` 的注册表新增 `fpga_dma`
   （编译开关 `ENABLE_FPGA_DMA`，dext 未加载时启动即报错并提示回退 `uhd`/`zmq`）。
3. **配置面**：`apps/gnb` 的 RU 配置（`gnb_appconfig`）新增
   `ru_fpga_dma: {device_driver: fpga_dma, dma_ring_slots: 8, dma_block_samples: 23040, timestamp_mode: hw}`；
   示例配置 `configs/gnb_fpga_dma_tdd_n78_20mhz.yml`（对照现有
   `configs/gnb_rf_b200_tdd_n78_20mhz.yml` 的 `ru_sdr` 段）。
4. **接收模块切断清单**（逐项可开关）：
   - ZMQ：`ENABLE_ZEROMQ=OFF` 或配置不选 `zmq` → 4 拷贝路径整体消失（B1）；
   - UHD：`ENABLE_UHD=OFF`（FPGA 腿验证通过后默认关）；
   - split-7.2 OFH：保持构建（`gnb_split_7_2` 已 EXCLUDE_FROM_ALL），不作为 FPGA 腿依赖。

### 3.3 FPGA XDMA 握手与 DMA 块 Header 设计

**BAR0 寄存器窗（dext ↔ FPGA 握手，首版最小集）**：

| 偏移 | 寄存器 | 方向 | 语义 |
|---|---|---|---|
| 0x00 | MAGIC/VER | R | `0x4F435544` ("OCUD") + 版本 |
| 0x04 | CAPS | R | bit0: C2H DMA；bit1: 硬件时间戳；bit2: 1PPS 锁定 |
| 0x08 | TIMESTAMP_NS_LO/HI | R | 当前 FPGA 纳秒计数（调试/对时） |
| 0x10 | SLOT_CFG | W | numerology(3b) + 每槽符号数/循环前缀表索引 |
| 0x14 | RING_BASE_LO/HI + LEN | W | dext 申请的 DMA 环物理基址与长度（IOBufferMemoryDescriptor 的物理地址段） |
| 0x1C | RING_PTR_WR（FPGA 写指针） | R | FPGA 已写完的块序号（含时间戳） |
| 0x20 | RING_PTR_RD（主机读指针） | W | 主机已消费块序号（dext 门铃回写） |
| 0x24 | INTR_EN / INTR_STAT | W/R | 中断使能（每 N 块 / 每槽完成）与状态 |
| 0x2C | STATUS | R | DMA 错误 / 溢出 / 时钟失锁位 |

**DMA 块 Header（FPGA 写入每块头部，UMA 内 ABI，`ocudu_fpga_dma.h`）**：

```c
// 8 字节对齐; 每块 = header + N×channels×samples×ci16 载荷
struct ocudu_dma_block_header {          // 32 B
  uint32_t magic;                        // 0x444D4131 ('DMA1')
  uint16_t seq;                          // 块序号 (环内回绕)
  uint16_t channels;                     // 通道数 (2)
  uint32_t samples;                      // 每通道样点数 (如 23040 = 1 符号@23.04Msps/SCS 取决于 FPGA 组帧)
  uint64_t timestamp_ns;                 // FPGA 硬件时间戳 (1PPS 驯服计数)
  uint32_t flags;                        // bit0: 槽首块; bit1: 槽尾块; bit2: 1PPS 沿所在块
  uint32_t reserved;
};
```

**关键取舍**：
- **块 = 槽的整数分之一**（首版：每槽 1 块，23040 样点@30 kHz；后续按符号组帧细粒度化），
  使「时间戳→slot_point」换算无跨块拼接；
- C2H 描述符环由 FPGA 主动推送，主机消费端**只递增 RD 指针**（一次 8 B 门铃写），无每块中断——
  中断仅用于「槽完成」（默认每槽一次，可用 `INTR_EN` 降为每 N 槽）。

### 3.4 PCIDriverKit Dext 开发步骤（D1–D12）

> 全部 API 为 DriverKit 用户态驱动框架（无需 KEXT/root）；dext 跑在独立于应用的用户空间进程。

- **D1 工程骨架**：`driverkit/OCUDUFPGADext/`——`Info.plist`（`IOKitPersonalities` 匹配
  `IOPCIClassMatch 0x12000000` + `IOPCIVendorDeviceMatch` 填 ASM2464PD 透传后的
  vendor/device ID；`OSBundleUsageDescription`）、`entitlements`（
  `com.apple.developer.driverkit`、`com.apple.developer.driverkit.userclient-access`、
  `com.apple.developer.driverkit.allow-any-userclient-access`）。
- **D2 设备接入**：`init()` 里 `IOService::CopyProvider` 拿 `IOPCIDevice`，`Open(provider, 0)`；
  使能 Bus Master 与 Memory Space（写 PCI 命令寄存器）。
- **D3 BAR 映射**：`provider->MemoryRead64/Write64(kIOPCIConfigSpaceBaseAddress0…)` 读 BAR0
  基址/长度 → `IODMACommand`/`IOMemoryDescriptor::withPhysicalAddress` 或直接
  `provider->MapDeviceMemoryWithIndex(0, &bar0)` 得到寄存器窗；**校验 MAGIC**，随后按 §3.3
  完成 XDMA 握手（读 CAPS、写 SLOT_CFG）。
- **D4 DMA 环形内存申请**：`IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut | kIOMemoryMapperNone, size, alignment=page, &ring_md)`；
  用 `IODMACommand::Create` + `PrepareForDMA` 生成 IOMMU 段，`Gen64IOVMSegments` 取
  **物理地址段表**（FPGA 需要的是 IOMMU 之后的地址，不是虚地址）。
- **D5 环基址下发给 FPGA**：`MapDeviceMemoryWithIndex` 或 BAR0 写入 RING_BASE/LEN + 使能；
  **中断源**：`provider->RegisterInterrupt(index, 0, 0, handler)`（返回
  `IOInterruptDispatchSource`，在专用 dispatch queue 上做**deferred dispatch**——
  中断上下文内只读 WR 指针并推进「就绪块」计数）。
- **D6 用户态映射（零拷贝的关键一环）**：dext 通过 `IOUserClient` 子类暴露
  `CopyClientMemoryForType`；dext 侧 `ring_md->CreateMappingInTask(client_task, 0, 0, 0, 0, &client_va)`，
  用户态应用拿到**同一物理页的直接映射**（写时共享语义下由驱动保证不可写或只读视图，视设计）。
- **D7 用户态 Client 协议**：应用侧 `IOServiceOpen` → 自定义外部方法（`ExternalMethod`）：
  `kMethodGetRingLayout`（返回 va、size、块数、header ABI 版本）、`kMethodDoorbell`（回写 RD）、
  `kMethodGetTimestamp`、`kMethodGetStats`。
- **D8 中断→用户态通知**：dext 侧维护「就绪块计数 + 槽完成事件」，经 shared 内存
  （DMA 环内的 `host_doorbell` 区或独立小 `IOBufferMemoryDescriptor`）原子发布；
  应用侧用 `dispatch_semaphore`/`os_unfair_lock`+`ulock` 轮询——**首版不引入 Mach 消息**
  （开销 ≤ 1 µs 级），后续换 `IOUserClient` 异步通知。
- **D9 时钟核对**：应用每槽读 `TIMESTAMP_NS` 与 `mach_continuous_time` 对拍，建立
  FPGA 时间戳 ↔ 主机时钟的双点线性拟合，供上层把硬件时间戳换算为
  `upper_phy_timing_context::time_point`（`include/ocudu/phy/upper/upper_phy_timing_context.h:13-18`）。
- **D10 测试**：`driverkit/OCUDUFPGADext/tests/`——`DriverKit` 单元测试目标 +
  `systemextensionsctl developer on` 下的循环 DMA 回环测试（FPGA loopback 模式）；
  **零拷贝验收脚本**：应用在映射区写入 pattern → FPGA 回环 → 比对，全程
  `dtruss`/Instruments 确认应用侧无 `read/write` 系统调用。
- **D11 签名与分发**：Developer ID + System Extension（`systemextensionsctl` 启用），
  附 `com.apple.security.system-extension` 应用 entitlement；CI 里用
  `xcrun systemextensionsctl` 自动化安装/卸载。
- **D12 安全模式**：dext 崩溃/未加载时 `ru_fpga_dma` 工厂报错并**自动回退**到
  配置里指定的回退腿（如 `ru_sdr`），保证 A/B 实验可重复。

### 3.5 用户态零拷贝 MTLBuffer 包装（核心突破落点）

```objc
// 应用侧（C++ 适配器内）, 与现有引擎 buffer_cache 范式同构
id<MTLBuffer> ocudu_fpga_dma_wrap_buffer(id<MTLDevice> dev, const void* ring_va,
                                         size_t bytes) {
  // ring_va 来自 dext CreateMappingInTask: 页对齐, 大小页倍数 (D4 保证)
  return [dev newBufferWithBytesNoCopy:(void*)ring_va
                                length:bytes
                               options:MTLResourceStorageModeShared
                           deallocator:nil];   // 宿主 = dext 生命周期
}
```

- **对齐约束**（吸取两个既有引擎教训，§1.4 B8）：DMA 环 = **页对齐 + 页倍数**（D4 直接满足），
  包装长度**永远传整环长度**并在缓存命中时**断言长度一致**——修正现有 `buffer_cache`
  的 size-blind 缺陷，新代码不复制这个坑。
- **每个槽一个 `MTLBuffer` 视图**（`offset = slot_index × slot_stride`）或**整环一个 + kernel
  内偏移**：首版选后者（少对象、GPU 端 `setBuffer:offset:` 表达槽偏移），
  DMA 写指针与 GPU 消费指针的同步用 §4 的事件协议。
- **一致性语义**：Apple Silicon 统一内存 + `StorageModeShared` 下 GPU 读 DMA 新数据只需
  「FPGA 写可见」；由于 FPGA 是 PCIe 主设备直写 RAM，需在块 Header 的 `flags` 上加
  **写完成标志**（FPGA 写完载荷后最后写 header.flags 的 done 位），GPU 侧等待 kernel
  以 `device` scope acquire 读取该位——这是**跨 PCIe 设备的内存序**，必须显式处理，
  不能用普通 load。

### 3.6 时序对齐（硬件时间戳 → slot_point）

- 换算：`slot_point`（`include/ocudu/ran/slot_point.h:219-223`，3-bit numerology + 29-bit count，
  `count = sfn×每帧槽数 + slot_index`）由 FPGA `SLOT_CFG` 写入的 numerology 与时间戳推导；
  1PPS 沿 = SFN 边界（与现 UHD `clock_source=gpsdo` 的锚定语义一致，见
  `configs/gnb_uhd_oaiue.yaml:28-29`）。
- RU 侧在 `receive()` 返回的 `metadata.ts` 中携带 FPGA 时间戳（`baseband_gateway_timestamp`），
  lower PHY 的现有「样本时间戳→tti 边界」逻辑（`ru_controller_sdr_impl.cpp:39-61` 的对应物）
  在 `ru_fpga_dma` 中实现为纯查表——**CPU 调度器不再推算时隙，只消费硬件结论**。
- GPU 链的槽启动：§4.3 的「等待时间戳 kernel」消费同一 Header（等待 `done` 位 + 期望槽号）。

### 3.7 阶段二验收（DoD）

1. `gnb -c configs/gnb_fpga_dma_tdd_n78_20mhz.yml` 走 **CPU 回退腿**（FFT/CE/LDPC 全 CPU）
   实链 attach + 500 ping 通过——证明 DMA 底座与既有 PHY 完全兼容；
2. 验收脚本证明 I/Q 从 FPGA 到资源网格 **0 次 memcpy、0 次 read/write 系统调用**；
3. GPU 冒烟：一个只读 DMA 环的统计 kernel 验证 `MTLBuffer` 包装与 done 位等待正确
   （第三阶段的第一个 kernel 雏形）；
4. B1/B2 归零（在 FPGA 腿配置下），B8 修复合入。

---

## 4. 第三阶段：Metal Kernel 串联与 ICB 调度重构（Kernel Chaining）

> 目标：全链 GPU 内闭环——每槽 1 次提交、1 次回调、中间态 0 次回传 CPU。
> 平台约束回顾（§0.2）：ICB 现平台不可用；跨 threadgroup 软件栅栏不可用；
> dispatch 编码成本 ~1.9 µs/个（layered 链 290 个 = 550 µs 地板，必须整体消灭）。

### 4.1 补齐待 Metal 化模块

#### 4.1.1 FFT/IFFT（`lib/phy/generic_functions/dft_processor_*`）

- **接口对齐**：`dft_processor`（`include/ocudu/phy/generic_functions/dft_processor.h`）已有多实现
  （generic/fftw/fftz/avx2），新增 `dft_processor_metal`（工厂注册 `"metal"`），
  引擎范式完全复用 LDPC：pimpl + `void* impl`、`ocudu_add_metallib`、家族级共享
  device/queue/pipeline、构造期 warm-up（`family_warmed` 位掩码模式，
  `ocudu_metal_decoder_engine.mm:483-495`）。
- **内核选择**：N=128…4096（30 kHz 20 MHz 网格 512–1024 点），混合基
  Stockham/radix-4 单 pass 或两 pass（N 大时），bf16 输入（与资源网格 `cbf16_t` 对齐，
  `resource_grid_impl.h:35` 的 `dynamic_tensor<cbf16_t>`），复数交织布局与
  `ofdm_demodulator_impl.cpp` 的现有转换对齐。
- **接入点（阶段 3.1）**：先在 `puxch_processor_impl.cpp:52-53` 的 FFT 调用处换成
  GPU FFT + GPU 侧 `grid.put`（网格改为 UMA 区时），**保留 CPU 腿**（CLI：
  `--pusch_fft_backend cpu|metal`，与现有 A/B 风格一致）。
- **验收**：与 fftz 路径 bit-exact 或 NMSE ≤ -60 dB（复用 CE 的 bit-exact 教训：
  累加顺序决定精度，`Metal_MMSE_Channel_Estimator_PLAN.md:744-764`）。

#### 4.1.2 MIMO 均衡（`channel_equalizer`）

- 现路径：`pusch_demodulator_impl.cpp:344` `equalizer->equalize(...)`（ZF/MMSE，
  `pusch_channel_equalizer_algorithm`），输入 = 信道估计（`cbf16_t`）+ 数据 RE。
- Metal 版 `channel_equalizer_metal`：单 kernel「均衡 + 层合并」（每 RE 复乘加，
  无矩阵求逆——ZF 除法/MMSE 标量闭式），输出**直写 LLR 前的均衡符号缓冲**；
  **与解调融合成第二 kernel**（符号→LLR 查表在 GPU 端做），
  使 CE 输出到 LDPC 输入的中间态完全留在 UMA。

#### 4.1.3 解调 LLR 生成

- `pusch_demodulator_impl` 的软比特量化（含 256QAM 查表）在 GPU 端实现为
  `demod_to_llr` kernel，输出布局**直接等于 LDPC 引擎期望的 int8 LLR 布局**
  （含 2Z 打孔 + 尾偏置 + 32 对齐），**B9 的 CPU 打包整体消失**——
  LDPC kernel 的输入改为「解调 kernel 的输出缓冲」。

### 4.2 引擎接口改造：encode-only + 事件依赖（先于 ICB 的中间态）

把两个既有引擎从「同步函数」重构为「**编码到外部命令缓冲**」的函数族，旧接口保留为薄封装：

```cpp
// ocudu_metal_decoder_engine.h 增补（示意, 命名沿用 ocudu::metal）
class decoder_engine {
 public:
  // ---- 旧接口 (兼容层: 内部 = encode + commit + wait) ----
  int  decode(const void* in_fp16, uint8_t* out_bits, int max_iter, uint32_t* error_count_out);
  // ---- 新接口 (链式) ----
  struct encode_result {
    id<MTLBuffer> out_tb_bits;      // GPU 直写的 TB 字节缓冲 (UMA, 页对齐)
    id<MTLBuffer> out_crc_ok;       // 1 字: 0/1
    id<MTLEvent>  completion_event; // 或外部传入的 event 上 signal
  };
  // 把整个解码编码进 caller 的 command buffer; 不做 commit/wait
  bool encode_decode(id<MTLCommandBuffer> cb, const void* llr_u8 /*零拷贝视图*/,
                     uint32_t max_iter, const encode_result* out,
                     id<MTLEvent> wait_llr_ready, id<MTLEvent> signal_done);
  // CRC24A 尾核 (GPU), 输入 = out_tb_bits; 结果 = out_crc_ok
  static bool encode_crc24a(id<MTLCommandBuffer> cb, const encode_result& r);
};
```

MMSE 引擎同样增补 `encode_weights_apply(cb, ..., wait_ce_input, signal_ce_done)`；
**B7 顺带解决**：链式接口天然要求「一个共享 command queue 的管线对象」，
CE 引擎从「per-instance device+queue」改为**管线级单例**（对齐 LDPC 的家族级共享，
`Metal_MMSE_Channel_Estimator_PLAN.md:811` 待办 (D) 就此落地）。

**依赖表达**（三选一，按成熟度排序）：
1. **命令缓冲内顺序执行**（首版）：全部 kernel 编进同一个 `MTLComputeCommandEncoder`，
   Metal 保证同一 encoder 内顺序——**零额外机制**，但失去并发（FFT 与下一槽 CE 无法重叠）。
2. **MTLEvent wait/signal**（第二版）：跨 encoder 的细粒度依赖（`encodeWaitForEvent:`/
   `encodeSignalEvent:`），允许「槽 N 的 CE」与「槽 N+1 的 FFT」重叠，GPU 内部流水化。
3. **MTLFence + `setBuffer:offset:` 使用追踪**：同一缓冲多 kernel 读写时声明
   `MTLResourceUsage`，供驱动正确插桩内存序（对 UMA Shared 缓冲尤其重要）。

**ET/分支语义（GPU 内解决，不回调 CPU）**：LDPC 的 ET 已 GPU 内部（`nmsl_et_gate`，
`ocudu_metal_decoder_engine.mm:742-745`）；CRC 门控放在 §6.2 的 FAPI 尾核
（`crc_ok=0` 时写 0 长度 span——与现网空 span 语义 bit 级等价，
`uplink_processor_impl.h:108`）。

### 4.3 全链单提交（阶段 3.3 的终态结构）

```
CPU (每槽, 一个 pusch 任务, ~µs 级):
  ① 读 DMA 环 Header: 槽号/时间戳校验
  ② 分配/复用 8 个长驻 MTLBuffer (Shared, 页对齐):
       buf_fft / buf_grid_re / buf_ce_h / buf_eq_sym / buf_llr / buf_tb / buf_fapi_ring
  ③ 编码 1 个 command buffer (多个 compute encoder + MTLEvent):
       [wait: DMA done 位 acquire kernel]  → 1 dispatch
       FFT(N点×符号×端口)                 → RG 抽取 kernel
       CE: 相关矩阵(可 GPU 预计算常量) + 求逆 + K1b/K2   ← B5 的 CPU 部分整体入 GPU
       ⚠️ 求逆搬 GPU 已被实测否决（单系统 K1 = 75–92 µs vs CPU ~10 µs）；
          此处的真实收益只剩"并入同一条 CB、少一次等待"（~100 µs/slot）
       (矩阵求逆 kernel 重写: 现有 mmse_inv 369-382 µs 的 barrier 链改为
        "每系统一线程组 + 无 barrier 高斯-若尔当分块" 或直接改走 8×8 张量块, §5)
       均衡+层合并 → 解调 LLR (int8, LDPC 原生布局)      ← B9 消失
       LDPC (persistent 单 dispatch / layered 事件链)    ← B10 的 290 dispatch
       CRC24A + FAPI 尾核 (§6.2)                         ← B11 的 CPU 回读消失
       [signal: completion event]
  ④ commit → 立即返回 (执行器线程继续跑别的任务)
```

**关于 ICB 的硬核结论（写死在本蓝图里，避免后人重蹈）**：
本平台（darwin 25/AGX G16X）ICB 只支持裸 dispatch（PLAN.md:593-607 实测）。
**M4 Pro + 更新 macOS 上必须重做 §4.2 的最小复现**（三个动作：`setComputePipelineState` /
`setKernelBuffer` / `dispatchThreadgroups` 各一条，MTL_DEBUG_LAYER=1）——
若通过，演进为「**record once per 槽型 + executeCommandsInBuffer 一次**」：
ICB 内只用 dispatch-only 命令，`layer_start` 等每槽变量经 `setKernelBuffer` 的
buffer offset 通道（PLAN.md:606-607 已给恢复设计）。在此之前，全链单 CB 的编码成本
≈ 每槽 10–15 个 dispatch × 1.9 µs ≈ 20–30 µs，可接受；ICB 通过后降为 ~1 µs 级。

### 4.4 CPU 交互协议：一次回调、绝不在驱动线程跑协议栈

```objc
[cmd_buf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
  // 1) 只做两件事: 原子推进 FAPI 环的 CPU 读指针 + 触发门铃
  fapi_ring->cpu_rd.store(hw_wr, std::memory_order_release);
  doorbell->signal();                     // dispatch_semaphore / ulock 唤醒
}];
// 2) 门铃唤醒的是 pusch_executor 上的轻量任务:
//    task_executors.pusch_executor.defer([this, slot]() {
//       drain_fapi_ring();                 // §6.3: 展开 FAPI 记录 → MAC
//       state_machine.on_finish_processing_pdu();  // 保持现有 FSM 不破坏
//    });
```

- **回调线程纪律**：`addCompletedHandler` 跑在 Metal 驱动线程，**禁止**触碰
  `uplink_processor_fsm`/`pdu_repository`/MAC 任何对象（这些对象的线程亲和性由
  `uplink_processor_impl.h:94-117` 与 `mac_ul_processor.cpp:101-126` 决定）；
- **反压与优先级**：LDPC 家族现共享单队列（`ocudu_metal_decoder_engine.mm:225`），
  全链化后按 UE 时延等级**分区为 2 条队列**（控制/小包 → 高优队列；大数据 → 吞吐队列），
  对齐路线图 §3.1/§4 的「防大包饿死控制信令」（`apple_silicon_heterogeneous_gnb_plan.md:117-124`）；
- **缓存一致性窗口**：GPU 写 FAPI 环后 CPU 只在 `drain_fapi_ring` 内**集中一次性**读
  元数据+TB（避免逐字节乒乓，路线图 §3.1.3 已点名）；
- **多槽流水**：双缓冲 FAPI 环 + 双 DMA 块环，槽 N+1 的 FFT 在槽 N 的 LDPC 进行时即可启动
  （MTLEvent 依赖天然支持）。

### 4.5 回退与 A/B 路由（CLI 扩展）

- 新增 `--pusch_ul_pipeline {cpu, gpu_chained}`（默认 `cpu`，即现状全兼容）；
  `gpu_chained` 蕴含 FFT/CE/均衡/解调/LDPC 全 Metal，但**保留**逐模块强制开关
  （`--pusch_fft_backend cpu` 等）以便二分定位；
- 每模块的回退语义沿用现网：CE 引擎失败 → CPU 参考循环（但**先修 B8**，失败必须显式告警）；
  LDPC 失败 → `nullopt` → 上层既有处理（`ldpc_decoder_metal.cpp:515-517`）；
- A/B 口径：同一配置双腿切换必须只差一个开关（里程碑 M5 的验收脚本强制）。

---

## 5. 第四阶段：张量化改造与 ANE 深度融合（Tensor & ANE Routing）

> 目标：把时频资源重构为 8×8 张量块以最大化 M4 Pro GPU 的 `simdgroup_matrix` 利用率；
> 并把 ANE(HELENA) 从「模块级替换」升级为「与 GPU 链无缝竞争/协同的 Hybrid Routing」。

### 5.1 3GPP PRB 网格 → 8×8 张量化（解耦 12 子载波非 2ⁿ 限制）

**数学事实**：3GPP 资源网格每 PRB 12 子载波。20 MHz/30 kHz = 51 PRB = **612 子载波**；
612 = 76×8 + 4 ⇒ 零填充到 616 = 77×8。时域 14 符号 ⇒ 填充到 16（2×8）。
51 PRB 恰好不是 8 的倍数，是填充开销最坏的常见情形之一——因此**张量化布局必须把
填充率作为一等设计参数**。

**v2 布局（升级现有 v1 的 host 侧零填充，`ocudu_mmse_weights_matrix.metal:11-28`）**：

```
网格 (subcarrier × symbol × port)  ──重排──>  tile 图 (Ts × Tf × port)
Ts = ceil(subcarrier/8) 个 8 子载波块 (频率维)
Tf = ceil(symbol/8) = 2 个 8 符号块 (时间维)
每 tile = 8×8 复数 → 拆 re/im 或直接复数 bf16 双平面
内存布局: [port][Ts][Tf][8][8] 四元组连续 (对齐 GPU tile 内存 64B/128B)
掩码: 常量 kernel 参数 (valid_mask bitmask), 填充区读 0/写丢弃
```

- **CE 应用**：R_t/R_f 相关矩阵在 8×8 块下自然分块（Kronecker 结构保留），
  **2026-09-13 更新（这就是下一步，S-5a）**：并行化后的基线是 91.3 µs/系统（72 枢轴步 × 2 屏障、
  逐元素穿 threadgroup 内存）。本段的块化方案正是修法——目标是 ≤10–15 µs，判据是"在链内、零往返"，
  而不是与 CPU 的孤立 µs 比大小。
  `A⁻¹` 按 8×8 块 Gauss-Jordan 或直接 `simdgroup_matrix` 求逆（分块 2×2 消元，
  `simdgroup_multiply_accumulate` 加速，规避现有 `mmse_inv` 的 barrier 链——
  其 369-382 µs 的根因是 36 线程组 × 3 barrier × 36 列串行，块化后每块独立求逆天然并行）。
- **均衡/解调应用**：按 tile 并行，每 tile 一个 32 线程组；
- **数据精度**：网格已是 `cbf16_t`（brain-float 复值，`resource_grid_impl.h:35`），
  与 M 系 GPU 的 bf16 矩阵单元直接对齐——**全链 bf16 是第四阶段的精度主线**
  （CE 相关矩阵统计用 fp32 累加、K1b/K2 走 bf16 GEMM，端到端 NMSE 门禁 ≤ metal_mmse + 0.5 dB）。

### 5.2 `metal_nn_mmse` 升级路径（v1 → v2 具体改动）

1. **v1 现状（已落地）**：`simdgroup_matrix<float,8,8>` 的 weights/apply 双 kernel +
   host 侧 `Lp/Np = ceil8` 零填充 + 4 块×(re,im) 打包 8 列 GEMM
   （`ocudu_mmse_apply_matrix.metal:12-14,60-81`）；L=54→Lp=56 已过 A/B 对拍。
2. **v2 改动清单**：
   - 填充从「host 逐行 memset」改为「GPU 前置 kernel 一次清零 + 写有效区」，
     staging 拷贝（B5/B6 的一部分）随之消失——CE 输入直接来自 RG 抽取 kernel 的输出；
   - 权重/应用两 kernel 合并为一个 kernel（同一线程组内 `simdgroup_store` 到共享
     tile 后直接 apply），省一次 dispatch 与一次中间缓冲；
   - `nof_blocks`/`L` 等维度改为**常量表索引**（每 (PRB, DMRS 数) 组合一条预编译变体，
     `MTLFunctionConstantValues`），把运行期 `setBytes` 全数消掉；
   - 多端口/多层批处理：`[port][sys][tile]` 三维 grid，跨端口一次 dispatch
     （落实 CE PLAN (D) 的「跨端口批处理」）。
3. **验收**：与 v1 的 NMSE 对拍（单测 Test 8 同口径，`port_channel_estimator_metal_mmse_unit_test.cpp:1062`），
   gate = 差值 ≤ bf16 量化地板；时延目标 = 36 PRB/3 DMRS 槽 ≤ 30 µs。

### 5.3 Hybrid Routing 调度层（CPU / GPU 链 / ANE 三路分流）

**决策器接口**（新类 `phy_routing_policy`，装配点 = `upper_phy_factories.cpp` 的
PUSCH 处理器工厂链，保持 CLI 一键可切）：

```cpp
struct phy_routing_decision {
  channel_estimator_algo ce;   // cpu | metal_mmse | metal_nn_mmse | helena
  ldpc_backend          ldpc;  // neon | metal_persistent | (gpu_chained 时强制 metal)
  bool force_cpu_chain;        // 整链 CPU (控制信道/极小 TBS)
};
// 输入: TBS, MCS/SINR 预估, UE 数, 队列深度, ANE 负载, 槽预算余量
phy_routing_decision decide(const routing_context&);
```

**分流表（V2X 场景基线，可直接照抄再调参）**：

| 流量 | 路由 | 理由 |
|---|---|---|
| PRACH / PUCCH / Msg3 | **CPU 强制** | 控制面低密度、既有 µs 级 CPU 链（`apple_silicon_heterogeneous_gnb_plan.md:121-124` 既定方针）；GPU 链只接 PUSCH |
| PUSCH, TBS ≤ ~200 B | CPU（neon LDPC） | GPU 小 TB 无交叉点（`PLAN.md:569-573`），省回调 |
| PUSCH, 大 TBS / 多 UE | **GPU 链**（gpu_chained） | 吞吐 + 时延双赢，单次回调 |
| CE（GPU 链内） | `metal_nn_mmse`（v2） | 张量化后槽内 ≤ 30 µs，无需 ANE |
| CE（CPU 链内、大带宽） | `helena`（ANE） | ANE 141 µs < metal_mmse 255 µs，且不占 GPU 队列 |
| **A/B 竞争模式**（实验） | GPU 链与 ANE **同槽双跑 shadow** | 结果以 GPU 链为准，ANE 结果只记 NMSE/时延（`AI_CE_20MHz_plan.md:124-129` 的 G-4 双跑实践正规化） |

**ANE 侧配合改造（把 ANE 变成「无缝并行策略」而非阻塞同步调用）**：
- 去掉每推断一次 condvar 往返（`ocudu_coreml_nn_engine.mm:219-231`）：
  worker 改**深度流水**（提交即返回 + 完成队列回调），调用侧不阻塞；
- 每推断 ~10 个 ObjC 临时对象全部上提为成员（`NSArray`/`MLMultiArray`/`MLPredictionOptions`
  复用），worker 循环加 `@autoreleasepool`；
- 预测失败的回退语义从「静默退回 average_impl」改为**显式上报路由决策器**
  （`nn_grid_valid=false` 路径保留，但打点 + 计数）。
- CoreML 接口预留（§5.4）保证未来「ANE 均衡/检测」模块无需再动数据通路。

### 5.4 ANE/CoreML 数据对齐预留（为后期 ANE 介入更多 PHY 环节）

1. **统一张量描述符**（新头 `include/ocudu/phy/upper/metal/tensor_desc.h`）：
   `{shape[4], strides[4], dtype(bf16|fp16|fp32), owner(uma_ptr), alignment}`——
   GPU kernel 与 `MLMultiArray initWithDataPointer` 共用同一描述符，
   **GPU 输出缓冲可直接作为 CoreML 输入**（HELENA 已证明该模式可行，
   `ocudu_coreml_nn_engine.mm:76-101` 的 zero-copy 双向包装）。
2. **NHWC 布局约定**：CoreML 侧保持 `[1, Nsubc, 14, 2]`（现 HELENA 布局），
   GPU 张量化 tile 布局与之的互转在 GPU 端用一个「布局置换 kernel」完成
   （一次 dispatch，无 CPU 参与）。
3. **模型资产治理**：8 个入库 `.mlmodelc` 中 5 个 G-5 微调变体未接 CMake
   （`channel_estimator/metal/CMakeLists.txt:31-33` 只接 3 个）——补一条
   `OCUDU_HELENA_MODEL_VARIANTS` 列表 + 运行期 `reload()` 的线程安全修复
   （现 `reload` 无锁且无调用方，`port_channel_estimator_helena_impl.cpp:80-113`）。
4. **ANE 与 GPU 的并发策略**：CE 在 ANE（helena）与 GPU 链（nn_mmse v2）之间路由时，
   两者可**并行**（不同硬件单元）；`MLComputeUnitsAll` 允许 CoreML 自行落 GPU/CPU——
   路由决策器需感知 `last_predict_us` 与 ANE keep-alive 状态（`ocudu_coreml_nn_engine.mm:47-51`）。

---

## 6. 第五阶段：高速 FAPI 组装与 MAC 交互层（FAPI Offloading）

> 目标：LDPC 完成且 CRC=OK 后，GPU **直接生成** MAC 所需结构，CPU 以一次极轻量回调完成提取。

### 6.1 现状 FAPI RX 通路结论（决定设计的关键事实）

扫描确认（§1.1 表 + 子代理 C 深扫交叉验证）：
- `fapi::rx_data_indication` = `{slot_point slot; pdu{handle, rnti, rapid, harq_id, span<const uint8_t> transport_block}}`
  （`include/ocudu/fapi/p7/messages/rx_data_indication.h:16-28`）——**span 字节视图**，无 SCF-222 码流，
  无 `ulsch_cb_payload` 描述符栈（全库 0 命中）；
- **CRC 不在 Rx_Data 里**：独立 `fapi::crc_indication`（`tb_crc_status_ok`，
  `crc_indication.h:17-33`），是驱动 UL HARQ 的消息；Rx_Data 的 CRC 门控 =
  **空 span**（`uplink_processor_impl.h:108` → `fapi_to_mac_indications_fastpath_translator.cpp:87` 丢弃）；
- PHY→MAC 是**进程内同步 fastpath 调用链**（P7 无队列无线程），**全链唯一 TB 拷贝** =
  `byte_buffer::create`（`fapi_to_mac_indications_fastpath_translator.cpp:88`）；
- 已知寿命契约：`&pdu` 是槽 PDU 仓库引用、payload span 活到 `rx_payload_pool.reset()`
  （`uplink_processor_impl.cpp:108`），fastpath 的同步拷贝正是该契约的依托。

### 6.2 GPU 内 FAPI 记录直写（设计）

**决策：GPU 不重建 C++ 结构体**（`span`/`std::optional` 非 GPU 友好，且 MAC 还依赖
带单位换算的 CRC 消息）。GPU 写**固定布局记录**，CPU 用现有 builder 做零拷贝展开。

**UMA 环形 FAPI 记录区**（页对齐、双缓冲，`StorageModeShared`；每槽每 PDU 一条）：

```c
// ocudu_fapi_gpu.h — GPU 尾核与 CPU shim 的共享 ABI (单条 40 B, 槽头+2 条凑 128B 对齐)
struct ocudu_gpu_fapi_record {
  uint32_t slot_word;      // slot_point 打包值 (3b numerology 低位 + 29b count, slot_point.h:219-223)
  uint16_t rnti;           // enum rnti_t : uint16_t (ran/rnti.h:16)
  uint8_t  harq_id;        // enum harq_id_t : uint8_t, 0..31 (ran/harq_id.h:9-15)
  uint8_t  rapid;          // 0xFF = 无 (msgA 才有)
  uint8_t  crc_ok;         // 1/0 —— CRC24A 尾核结果, 与 crc_indication 语义一致
  uint8_t  reserved[3];
  uint32_t tbs_bytes;      // TB 字节数; crc_ok=0 时 GPU 写 0 (= 空 span 门控语义)
  uint32_t tb_offset;      // TB 字节在 UMA 载荷区的偏移 (页对齐槽内偏移)
  uint32_t sinr_q15;       // 可选: SINR 定点 (供 crc_indication.ul_sinr_metric_dB)
  int64_t  ta_tc;          // 可选: timing advance, Tc 单位 (phy_time_unit.h:15-34)
  uint64_t hw_timestamp;   // 直通 FPGA Header 时间戳 (调试/时延审计)
};
```

**GPU 尾核（LDPC 之后同链两个 kernel）**：
1. `ldpc_crc24a_pack`：硬判决 → 字节打包 → **CRC24A（GPU 版）**→ 按记录布局写环；
   替代 `ldpc_decoder_metal.cpp:521-531` 的逐 bit 重打包 + CPU CRC（B11 全消）。
   多码块 TB 的拼接也在该 kernel 内完成（现 `pusch_decoder_impl.cpp:557` 的
   `ocuduvec::copy_offset` 逐码块拼接移入 GPU）。
2. `fapi_commit`：`threadfence(device)` 后原子推进环写指针（`release`），
   触发（或标记）完成事件——**每槽只此一处跨器件可见性点**。

**CRC=OK 门控的位置不变、语义不变**：`crc_ok=0 ⇒ tbs_bytes=0 ⇒` CPU shim 组装空 span ⇒
`fapi_to_mac_indications_fastpath_translator.cpp:87` 照旧丢弃 ⇒ MAC 只收到 OK PDU，
NACK 由 `crc_indication`（shim 同步生成）承载——**与现网行为逐位兼容**。

### 6.3 极轻量 CPU 通知与 MAC 提取

**通知链（总开销目标 ≤ 5 µs + 1 次线程唤醒）**：
```
GPU 尾核推进写指针 ──> addCompletedHandler (Metal 驱动线程)
   ├─ fapi_ring->hw_wr.store(...)              // 已由 GPU 原子写, 这里只读
   └─ doorbell.signal()                        // dispatch_semaphore, ~1 µs
        └─ pusch_executor 上的 drain 任务 (唯一一次线程跳变):
             drain_fapi_ring():
               for each record:                       // 每槽 ≤ MAX_PUSCH_PDUS_PER_SLOT (16)
                 rx_data_indication msg;              // 栈上构造
                 rx_data_indication_builder(msg)
                    .set_slot(slot_point::from_uint(rec.slot_word))
                    .set_pdu(rnti_t(rec.rnti), harq_id_t(rec.harq_id),
                             span{uma_tb_base + rec.tb_offset, rec.tbs_bytes});
                 p7_notifier->on_rx_data_indication(msg);   // 现有 fastpath 原样复用
                 // crc_indication 同样在此生成 (复用 notify_crc_indication 逻辑)
               state_machine.on_finish_processing_pdu();   // FSM 语义不变
```

- **B12 的处置（唯一 TB 拷贝）分两步**：先保持 `byte_buffer::create`（行为零变化、
  风险最低）；随后引入**零拷贝 `byte_buffer` 视图段**（`byte_buffer` 增加「UMA 外挂段」或
  MAC 侧改为接受 span + 延迟拷贝语义）——注意 TB 字节在 MAC 侧被解子 PDU 后以
  `byte_buffer_view`/`byte_buffer_slice` 引用计数共享（`pdu_rx_handler.cpp:181`），
  生命周期必须由「槽级 UMA 载荷区引用计数」接管（与 `rx_payload_pool` 的槽重置时机对齐）。
- **B13 的优化（可选，实测后决定）**：fastpath 已在调用线程同步执行，
  若 GPU 回调直达 `drain_fapi_ring`（跳过 pusch strand 中转），可再省一次入队；
  但必须保证 `&pdu`/`payload` 的线程亲和与 FSM 不破坏——**首版走 pusch strand，
  稳定后再评估直呼**。
- **时延审计**：`hw_timestamp` 直通字段让「空口 → FAPI 出」全链时延可在
  `ul_pipeline_probe` 之外得到**硬件级锚点**（第三阶段的阶段三验收直接用它）。

### 6.4 时延预算分解（30 kHz / 0.5 ms 槽，50 m/s 自动驾驶场景）

| 段 | 预算 | 依据/实现 |
|---|---|---|
| FPGA 组帧 + DMA 写入 | ≤ 5 µs | 块=槽粒度直写 UMA，无描述符链 |
| 时间戳等待 kernel | ≤ 2 µs | 单 acquire load 自旋（受 `done` 位） |
| FFT + RG 抽取 | ≤ 10 µs | N≤1024 混合基，单槽全符号一批 |
| CE（nn_mmse v2, 8×8） | ≤ 30 µs | K1b 修复后纯计算 19.5–25.3 µs 口径 |
| 均衡 + 解调 LLR | ≤ 10 µs | 与 CE 同链无往返 |
| LDPC（persistent/链式） | ≤ 50 µs（小 TB） | 免 290 dispatch 编码 + 免回读后，小 TB 恢复「算力<调度」 |
| CRC24A + FAPI 尾核 | ≤ 5 µs | GPU 直写 |
| 回调 + 通知 + MAC 入队 | ≤ 5 µs | §6.3 链路 |
| 余量 | ~380 µs | 供重传/多 UE/瞬时抢占 |

**15 kHz / 1 ms 槽**：同链路裕度翻倍，可容纳 4 层 MIMO 与更大 PRB。
**口径说明**：现网管线中位 1133 µs（ZMQ 口径）的构成中，~255 µs CE + ~730 µs LDPC +
协议栈/拷贝/线程跳变；本预算的目标值均出自既有实测（`Metal_MMSE_Channel_Estimator_PLAN.md:708-714`、
`ldpc/metal/PLAN.md:918-935`）的同款口径，非外推。

---

## 7. 里程碑与验收矩阵

> ⚠️ **执行顺序已调整**：本节 M0–M10 保留为技术能力里程碑；实际推进顺序、
> 双支线任务分解（S0–S3 / H1–H4）与收敛步骤见
> `doc_chinese/full_gpu_chain/soft_hard_decoupled_execution_plan.md`（软硬解耦双支线执行计划）。

| 里程碑 | 内容 | 验收（DoD） | 依赖 |
|---|---|---|---|
| **M0** | 阶段一：B1–B14 量化审计 + B8/工厂消息修复 | 审计报告入库；B8 修复单测绿 | — |
| **M1** | 阶段二：dext + DMA 环 + CPU 回退腿实链 | FPGA 腿 500 ping 通过；I/Q 0 拷贝验收脚本通过；双腿 CLI 可切 | M0 |
| **M2** | 阶段二：UMA MTLBuffer 包装 + done 位等待 kernel 冒烟 | GPU 正确读到 FPGA 数据（统计 kernel 比对） | M1 |
| **M3** | 阶段三：FFT/均衡/解调 Metal 化（CPU 腿并存） | 每模块 A/B：NMSE/时延门禁；`--pusch_fft_backend metal` 实链通过 | M2 |
| **M4** | 阶段三：encode-only 引擎接口 + CE/LDPC 链式（模块对级） | CE→均衡→LDPC 两次 GPU 交接消除；每槽 commit 数减半以上 | M3 |
| **M5** | 阶段三：全链单命令缓冲 + 回调协议 | 每槽 1 commit/1 回调；30 kHz 槽内时延 ≤ 预算表；A/B 单开关切换 | M4 |
| **M6** | ICB 最小复现重测（M4 Pro + 新 macOS） | 三命令复现结论入库；若通过，启动 ICB 化（否则明确推迟并记录） | M5（并行） |
| **M7** | 阶段四：CE v2 张量化 + 跨端口批处理 | NMSE ≤ v1+0.5 dB；36PRB/3DMRS ≤ 30 µs | M5 |
| **M8** | 阶段四：Hybrid Routing 决策器 + ANE 流水化 | V2X 分流表落地；ANE 与 GPU 链可并行；shadow A/B 模式可跑 | M7 |
| **M9** | 阶段五：GPU FAPI 直写 + 轻量通知 | CRC=OK PDU 从 GPU 到 MAC ≤ 1 跳变、0 全量拷贝；与现网逐位兼容（500 ping + HARQ 场景回归） | M5 |
| **M10** | 全系统验收（V2X 场景模拟） | 30 kHz 20 MHz 双 UE + 移动衰落模拟：E2E ≤ 200 µs 且 p99 抖动 ≤ 50 µs；CPU 占用下降（对照 M0 基线） | M7+M9 |

**每个里程碑的通用门禁**：对应单测全绿（`ctest`）、非 Apple 构建不回归（`ENABLE_METAL_*=OFF` 路径）、
既有 ZMQ/UHD 配置不受影响（`pusch_ul_pipeline` 默认 `cpu`）。

---

## 8. 风险登记与回滚策略

| # | 风险 | 概率/影响 | 缓解与回滚 |
|---|---|---|---|
| R1 | ASM2464PD 桥的 PCIe 透传不完整（BAR/中断/ATS 特性缺失） | 中/高 | M1 前用 `ioreg`/`pciutils` 特性清单验收；桥固件选型留两套备选（含直接 TB5 扩展坞 PCIe 槽） |
| R2 | dext 签名/公证被系统策略拦截（用户环境无 `systemextensionsctl` 权限） | 中/中 | 开发期 `developer on`；文档给出启用步骤；回退腿 = M1 的 `ru_sdr` |
| R3 | DMA 跨 PCIe 内存序问题（done 位/数据乱序可见） | 低/高 | Header 尾写 done 位 + GPU acquire；FPGA 侧加写合并屏障；R3 专项压力测试 |
| R4 | ICB 平台缺陷长期不修 | 高/中 | 首版全链 CPU 编码单 CB（20–30 µs 编码成本可接受）；M6 定期重测 |
| R5 | GPU 全链时延不及预算（persistent barrier 链 46×8.7 µs 等） | 中/中 | LDPC 保留 layered/async 变体按 TBS 路由；每模块独立 CLI 回退（§4.5） |
| R6 | MAC 侧零拷贝 byte_buffer 改造破坏 RLC 引用计数语义 | 中/高 | B12 分两步（先拷贝后零拷贝）；槽级引用计数测试全覆盖 |
| R7 | 多 UE 共享单 GPU 队列的饥饿/抖动 | 中/中 | §4.4 双队列分区 + 路由决策器反压；p99 抖动作为 M10 硬门禁 |
| R8 | ANE 与 GPU 争抢内存带宽（UMA 瓶颈） | 低/中 | 路由决策器按负载分流；shadow 模式只采样不双份输出 |
| R9 | 回归风险（改引擎接口波及既有 500-ping 基线） | 中/高 | 全部旧接口保留为兼容层；每里程碑跑既有 E2E 脚本（`tests/ci/macos_e2e`） |

**回滚总开关**：`--pusch_ul_pipeline cpu` 一条命令回到 2026-09 基线行为；
FPGA 腿的 CPU 回退（`ru_sdr`）独立可切。

---

## 9. 附录

### 9.1 关键文件地图（改造 touchpoint 全表）

| 层 | 文件 | 改造动作 |
|---|---|---|
| 驱动 | `driverkit/OCUDUFPGADext/`（新建） | D1–D12 全部 |
| RU | `lib/ru/fpga_dma/`（新建）；`lib/ru/sdr/ru_factory_sdr_impl.cpp`（参照）；`apps/gnb/gnb_appconfig*` | 新 RU 单元 + 配置 schema |
| radio | `lib/radio/radio_factory.cpp:32-42` | 注册 `fpga_dma` |
| 低 PHY | `lib/phy/lower/lower_phy_baseband_processor.cpp:240`；`processors/uplink/puxch/puxch_processor_impl.cpp:52-53`；`modulation/ofdm_demodulator_impl.cpp` | DMA 拉模型替换；FFT 后端开关 |
| 通用函数 | `lib/phy/generic_functions/dft_processor_*` | `dft_processor_metal` |
| CE | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.{h,mm}`、`port_channel_estimator_metal_mmse_impl.{h,cpp}`、`ocudu_mmse_*_matrix.metal` | encode-only 接口；引擎单例；v2 张量化；B8 修复 |
| 均衡/解调 | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:344`、`channel_equalizer*` | Metal 均衡+解调 kernel |
| LDPC | `lib/phy/upper/channel_coding/ldpc/metal/ocudu_metal_decoder_engine.{h,mm}`、`ldpc_decoder_metal.cpp` | encode-only 接口；LLR 布局直连解调；TB 直写 |
| 上游 PHY | `lib/phy/upper/uplink_processor_impl.{h,cpp}`（`:108` 门控、`:302/:315` 载荷/入队）、`pusch_decoder_impl.cpp:404-524` | GPU 链接入点；FAPI 环生命周期 |
| FAPI | `lib/fapi_adaptor/phy/p7/phy_to_fapi_results_event_fastpath_translator.cpp`、`lib/fapi_adaptor/mac/p7/fapi_to_mac_indications_fastpath_translator.cpp:88`、`include/ocudu/fapi/p7/messages/{rx_data_indication,crc_indication}.h` | CPU shim（record→msg）；B12 零拷贝演进 |
| MAC | `lib/mac/mac_ul/mac_ul_processor.cpp:101-126` | 保持；B13 可选直呼 |
| 调度 | `lib/phy/upper/signal_processors/channel_estimator/factories.cpp`、`lib/phy/upper/channel_coding/channel_coding_factories.cpp:100-160`、`apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.cpp` | 新 CLI 开关 + routing 决策器接线 |
| 探针 | `include/ocudu/support/executors/ul_pipeline_probe.h` | A1 新分段 |

### 9.2 术语表

| 术语 | 含义 |
|---|---|
| UMA | 统一内存架构（Apple Silicon：CPU/GPU/NPU 共享物理内存） |
| 零拷贝 | 数据不经 `memcpy`，仅以指针/描述符在不同执行单元间传递 |
| ICB | Metal Indirect Command Buffer（GPU 自执行编码命令；本平台现阻断，§0.2/§4.3） |
| fastpath | OCUDU 的进程内 MAC-PHY 直连适配器（无队列无线程的 P7 同步调用链） |
| p7 / p5 | 3GPP FAPI 7.x 数据面 / 5.x 配置面接口（`include/ocudu/fapi/`） |
| slot_point | 槽时刻编码（3-bit numerology + 29-bit count，`ran/slot_point.h:219-223`） |
| XDMA | Xilinx PCIe DMA IP（BAR 寄存器窗 + H2C/C2H 描述符流） |
| dext | DriverKit System Extension（用户态设备驱动） |

### 9.3 参考文献

- `doc_chinese/apple_silicon_heterogeneous_gnb_plan.md`（路线图，§3 终局 = 本蓝图第三阶段）
- `lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`（LDPC 全史：290 dispatch 地板、persistent、
  ICB 负结果 P7、E2E 轮次）
- `lib/phy/upper/signal_processors/channel_estimator/metal/Metal_MMSE_Channel_Estimator_PLAN.md`
  （CE 全史：K1b 占用率修复、10 实例/待办 (D)、时延口径）
- `lib/phy/upper/signal_processors/channel_estimator/metal/AI_channel_estimation_implementation_plan.md`、
  `AI_CE_20MHz_plan.md`（HELENA/ANE 门禁与实测）
- `doc_chinese/build_macOS_note.md` / `docs/build_macOS_note_english.md`（macOS 构建与 E2E 环境）
- Apple：DriverKit / PCIDriverKit 文档（`IOBufferMemoryDescriptor`、`IODMACommand`、
  `CreateMappingInTask`）；Metal Shading Language 规范（`simdgroup_matrix`、`MTLEvent`、ICB）

---

*本文档为工程实施蓝图；每一处代码引用均经 2026-09 全库扫描核实。执行时以各阶段里程碑的门禁为准绳，
任何与实测矛盾的预算数字，修正后需同步回写 §6.4 预算表并在对应 PLAN 中留痕。*

---

# 10. `--phy_pipeline gpu`：IQ → LLR 的融合流水线（2026-09-13 规划，待讨论）

> 目标（用户定义）：**GPU path 是 PHY pipeline 的单行道**——RF I/Q samples 进去，被处理，
> 出来时就是 LLR（交给 CPU 做 LDPC）。数据在 GPU 内一路向前，不再回到 CPU 又进去；
> GPU 处理完一个 slot 就接着处理下一个 slot 的 IQ。
> 参照的"次优方案"：CPU 把 IQ 交给 GPU 后**只等 LLR**，再交给 LDPC。
> v1 范围：**PUSCH RX 链**（其余 UL 信道与 DL TX 不在 v1；DL TX 保持 CPU，§10.7）。

## 10.1 CLI 重构（用户给定语义）

> **已落地（S-7a，`757593c84a`）**：实现见活文档 §46。与下表的一处刻意偏差：`--phy_pipeline` 的默认值是
> **`auto`**（＝从模块开关推导；无 offload 开关时就是 `cpu`）。原因是"`gpu` 档 + 显式 `cpu` ⇒ 报错"必须能区分
> "显式给了 cpu"与"没给"，只有把三个模块开关的默认值改成 `auto` 才做得到；同时 `auto` 默认把改造前的行为
> （含"没有 Metal 后端时静默降级为 CPU"）逐字保留，见活文档 §46.2。

在 `expert_phy` 下新增一个 high-level 选择，四档（**默认 `auto`＝从模块开关推导**，见上面的说明）：

| `--phy_pipeline` | 含义 | 与模块开关的关系 |
|---|---|---|
| `auto`（**默认**，S-7a 新增） | 从模块开关推导：有 offload 开关 ⇒ `cpu_gpu`，否则 ⇒ `cpu` | 模块开关语义不变（改造前的行为逐字保留，含"后端没编进来 ⇒ 降级 CPU"） |
| `cpu`（显式） | 全 CPU（最初的 default） | 模块开关必须都是 CPU；显式给 metal ⇒ **报错**（冲突，不静默） |
| `cpu_gpu` | **模块级混合**（＝目前所谓"全 GPU path"） | 每个模块跟随自己的开关；后面不跟任何 metal ⇒ 等价 `cpu`；把每个模块显式写成 CPU ⇒ **也等价 `cpu`**（记一条日志说明"有效后端＝全 CPU"） |
| `gpu` | **融合流水线**：IQ → LLR | lane 拥有的模块（DFT/CE/eq/demapper）开关被接管：留 `auto` ⇒ 用 lane 自己的后端，写 `cpu` ⇒ **报错**（该档不做 CPU 回退）；**LDPC 不在 lane 内**，开关语义不变 |

实现要点：
- 单一真相源：`du_low_unit_expert_upper_phy_config::phy_pipeline`，**同时**传给 upper PHY 工厂
  （CE/均衡器/demapper/LDPC）与 lower PHY 工厂（DFT/网格）——后者今天从 `ru_sdr` 侧创建，
  是本次要打通的层间管道（§10.6 风险 R3）。**S-7a 的做法**：不是把模式本身传下去，而是让**两侧各自调用
  同一个解析函数**（`resolve_phy_pipeline_or_fatal`）拿到"有效后端"，这样两层不会各写一套规则。
  真正需要"模式本身"（例如 S-7b 的网格设备化开关）时，照 `dft_processor_type` 的样子经 RU 配置传下去。
- 启动时打印**有效配置**（一行/ph 模块）：模式 + 每个模块的后端 + 是否融合。（S-7a 已落地。）
- `gpu` 档的合法性校验：Metal 实现可用（Apple Silicon）、`is_supported` 覆盖当前拓扑。
  **S-7a 的更正**：多端口与跳频**不拒绝**（§10.6 R4）——`gpu` 档的 arena 第一版就按 `(slot,port,hop,symbol)`
  多维设计，v1 只跑 1 port/1 hop。

## 10.2 目标数据流与"唯一允许的两次跨越"

```
host IQ (radio)  ──upload──▶  device grid (每 slot, 每端口)
                                   │  (GPU 内部一路向前)
                                   ├─▶ 导频提取 / CFO / LSE / sigma2 / 相关矩阵  (K0)
                                   ├─▶ 权重+估计 (K1/K1b/K2) ─▶ 设备估计 (K3) + 设备噪声 (K4)
                                   ├─▶ 均衡(读设备估计+设备载波) ─▶ 解调(同一条 CB)
                                   └─▶ 解扩/合并 ─▶ device LLR buffer
                                                        │  download (每 slot 一次)
                                                        ▼
                                                   host LLR ─▶ LDPC(CPU)
```

**允许的主机↔设备数据跨越只有两处**：IQ 上传（不可避免，样本本来就在主机）与 LLR 下载（流水线产物）。
其余"数据"（网格、导频、估计、噪声、均衡输出、LLR 中间态）**全部留在设备**。
主机侧只留：(a) 少量**控制标量**的传递（见 §10.3 的 K0 说明）与 (b) **表/系数下传**（相关矩阵、
扰码序列），这些是"从主机算好传给 GPU 的系数"，不是数据往返。

## 10.3 必须新增的 GPU 工作（按工作量排序，全部有主机参考实现可逐位比对）

| 项 | 现在在哪 | 现状代价 | 说明 |
|---|---|---|---|
| **K0-a 网格→导频提取** | 主机 `extract_layer_hop_rx_pilots`（读**主机网格**） | 数据往返 #2 | 从**设备网格**按 (port, layer, DM-RS 符号) 取出 DM-RS RE，写进引擎的导频槽位 |
| **K0-b LSE + CFO** | 主机 `preprocess_pilots_and_estimate_cfo` / `compensate_cfo_and_accumulate` | 同上的主体 | CDM 正交化、CFO 估计（小归约）、逐符号相位补偿 |
| **K0-c 统计量** | 主机 `estimate_sigma2` + `stats_estimator`（读 LSE 导频） | `sigma2=3.4us`＋ | `pilots_power`、残差噪声、`sigma2_rel`；输入是设备导频 ⇒ 无需回主机 |
| **K0-d 相关矩阵** | 主机 `build_correlation_matrices`（Bessel/Doppler 查表） | `corr=11.5us` | 把系数表**直接写进引擎槽位**（顺带省掉 `stage_engine_group` 的 ~170 KB memcpy） |
| **FFT Phase 2** | 主机 `process_dft_output`（相位补偿 + 上下半带映射） | 数据往返 #1 | kernel 内做相位补偿并**直写设备网格**（路线图早已列出此项） |
| **均衡器的 `ch_re` 来源** | 主机 `get_ch_data_re`（读主机网格） | 数据往返 #3 | 从设备网格取数据符号（gather kernel，或让均衡器 kernel 直接按索引读网格） |
| **LLR 设备侧整理** | 主机 Pass 3（解扰 + 分块 + 码块缓冲） | 数据往返 #4 | 解扰/合并进设备（扰码序列按 slot 预生成下传），产物写进**设备 LLR 缓冲**（可零拷贝映射给主机读） |

> **现状对照（`d01fac7a69`，b16 实网腿实测）**：每 slot 的控制点/等待点/拷贝点清单见活文档
> **§48.47**。一句话：网格、设备估计、均衡输出、LLR **都已零拷贝**；剩下的显式 memcpy 全在 CPU 侧，
> 即下表的 K0-a/b/c/d 与"均衡器的 ch_re 来源"两项；`[ul_gpu_lane] gap` 中位数 **0.6µs** 说明
> "GPU 在等 CPU"已不是问题，成本在 **CPU 自己的 staging/主机数学** 上。

⇒ **本项目的真正大头是 K0（把 CE 的主机前段整体搬到 GPU）**：这正是 CE 那 ~180–200 µs/跳 CPU 时间的来源
（`[mmse_time_sum] mean total=334.8` 里 GPU 只忙 130.9）。它同时是"数据不再回主机"的**前提**，
不是可选项。

## 10.4 同步与流水线模型（决定了"单行道"能否成立）

- **一条队列 + 提交顺序**即可保证 slot 内各阶段的先后（现在 CE/均衡器/demapper/LDPC 已在
  `backend_queue`；**DFT 需要从前端队列挪到后端队列**，或整链统一到一条队列）。
  ⇒ **不需要**把各引擎的编码器合并成一条 command buffer；每个引擎各提交自己的 CB、
  顺序由队列保证，CPU **只在 slot 末尾等一次**。这是"多 CB、一次等待"的方案，风险远低于"合并编码器"。
- **slot 级流水**：CPU 提交 slot N 的 IQ 后**不等待**（lower PHY 侧），到 slot N+k 的处理点才收 LLR。
  每 slot 一套 GPU 资源（网格/导频/估计/均衡/LLR）⇒ 需要**深度 D≥2 的 ring**（§10.5）。
- **允许的等待点**：(1) LLR 下载前（一次/slot）；(2) 其它 UL 信道（PRACH/PUCCH）需要主机网格时按需下载
  （见 §10.5 的"按需镜像"）。**PUSCH 路径不再有中间等待**。
- LDPC 仍在 CPU：LLR 到手后由现有解码任务消费（`ldpc_decoder` 的逐码块同步不动）。
  ⇒ LLR 缓冲要设计成"**设备缓冲 + 可选主机镜像**"，为将来 LDPC 也进 GPU 留门（用户终局）。

## 10.5 缓冲与所有权

| 缓冲 | 位置 | 每 slot | 备注 |
|---|---|---|---|
| IQ（DFT 输入环） | 主机→设备 | ≈ `srate/1000` 样本 | 唯一的强制上传；可零拷贝 wrap 主机环 |
| **设备 resource grid** | 设备（**master**） | 25 PRB×12×14×4B ≈ 16.8 KB/端口 | FFT 直写；**主机镜像按需生成**（PRACH/PUCCH 需要时才下载，PUSCH 不需要） |
| 引擎槽位（A/R_hp/y/导频/K3/K4） | 设备 | 已有 | K0 直接写槽位，省掉 staging memcpy |
| 均衡输出 eq/nv | 设备 | 已有 | 均衡器与 demapper 同 CB |
| **LLR 缓冲** | 设备（+可选主机镜像） | 11 数据符号 × RE × bits | 流水线产物；下载一次/slot |
| ring 深度 D | — | D × 上述 | D≥2 才能"GPU 处理下一 slot 时 CPU 还在收上一 slot" |

## 10.6 风险与硬约束

- **R1（最大）K0 的工作量与数值风险**：CE 主机前段包含 CFO 估计、CDM 正交化、LSE、残差噪声、
  Bessel 相关表——每块都必须**逐位/按容差**与主机参考对齐（Test 9/11/12 那套门禁风格）。
  建议**分层推进**：K0-a → K0-b → K0-c → K0-d，每层单独可验证。
- **R2 共享网格**：网格是全 UL 信道（PUSCH/PUCCH/PRACH）共用的。设备化之后，
  CPU 侧消费者（PUCCH/PRACH 检测器）仍需主机网格 ⇒ **主机镜像按需生成**（那一次下载+等待记在
  需求方账上，不拖累 PUSCH）。
- **R3 层间管道**：DFT/网格在 lower PHY（`ru_sdr` 侧），CE/均衡器/demapper 在 upper PHY。
  融合流水线必须跨这两层（提交点在一侧、收集点在另一侧），是本项目最大的**结构**改动。
- **R4 跳频与多端口（2026-09-13 用户提问后更正，原"直接拒绝"过度保守）**：
  - **跳频**：全库核查 —— **没有任何地方设置 `hopping_symbol_index`**；调度器硬编码
    `intra_slot_freq_hopping = false` / `dmrs_hopping_mode::no_hopping`（`ra_scheduler.cpp:664-668`）；
    而且 **PUSCH demodulator 完全没有 hopping 概念**（`pusch_demodulator.h/.cpp` 里 0 处引用）。
    ⇒ 跳频在这条链上**从未启用**，CE 的 `rb_mask2` 是死代码。v1 只需一条 **assert/明确报错**防将来有人打开它。
  - **多端口**：真正的约束只有"每个 rx 端口是独立 CE 实例、独立设备缓冲 ⇒ 一个 base 指针描述不了多端口"。
    解法 = 设备估计 **arena**：按 `(slot, port, hop, symbol)` 分区，K3 直接写进对应区域，
    均衡器只绑 **一个** buffer 按 port 索引（**不需要**多 buffer 绑定）。这正是 ring 本来就要的布局
    ⇒ **接口与 arena 第一版就按多维设计**，v1 先跑通 1 port/1 hop，但不为单端口特化。
  - 保留的原则（这条才是真的）：**lane 内部禁止静默回退**——超出当前支持范围时**明确报错**，
    不得中途掉回 CPU 路径（会污染数据流且极难定位）。
- **R5 探针语义**：见 §10.7——不重构就会得到"变快了"的假象。
- **R6 回退**：`gpu` 是**新增**档，`cpu_gpu`/`cpu` 原样保留 ⇒ 回退就是一个 CLI 参数（无需重编）。

## 10.7 探针重构（用户定稿：`gpu` 档只量"进 GPU → 出 GPU"）

**用户 2026-09-13 定调**：GPU 数据流是**单行融合**的，所以**分模块耗时在 `gpu` 档失去意义**
（那些分段量的是 CPU 侧的 staging/提交/等待，不是数据流本身）。`gpu` 档要量的是
**数据从进 GPU 那一刻到出 GPU 那一刻的耗时**。

### 10.7.1 `gpu` 档的四个指标（取代全部分段）

| 指标 | 定义 | 为什么是它 |
|---|---|---|
| **`[ul_gpu_lane] residency`** | 每 slot：**该 slot 第一条 CB 的 `GPUStartTime` → 最后一条 CB 的 `GPUEndTime`** | 就是"数据进 GPU → 出 GPU"；纯 GPU 时间戳，**不含任何 CPU 打点** |
| **`lane busy` / `lane gap`** | `busy` = Σ 各 CB 的 GPU 执行时间；`gap = residency − busy` | 一举把"GPU 真的在干活"与"**GPU 在等 CPU 喂它**"分开——gap 就是单行道没跑满的部分，是唯一要盯的优化目标 |
| **`[ul_gpu_lane] period`** | 相邻 slot 的 lane-end 间隔（即流水线的**吞吐周期**） | 单行道的容量指标：是否跟得上 slot 速率（15 kHz SCS 下 1 ms/slot） |
| **`[ul_llr_ready]`** | CPU 视角：IQ 可用 → LLR 可读（含上传/下载/等待） | 流水线对上游的**产物延迟**；与 residency 的差就是两端的搬运与等待 |

**保留的旧指标**（跨模式可比）：`[ul_pipeline]`（IQ→CRC-OK，端到端，仍有效）、
`[ul_ldpc_decode]`、`[ul_mac_pdu_size]`、`[ul_fapi_mac]`。
**删除的**：`[ul_time_frequency]`、`[ul_channel_estimation]`、`[ul_equalization_demod]`
（以及它们的 `record_*` 打点）在 `gpu` 档**不再打印**——写了也是假的（§44.1 的
`ul_channel_estimation min=20.3 µs` 正是这种假象的预演）。S-7a 已把这三条做成按模式开关（活文档 §46.6）。

⚠️ **两轮之间比这些序列的前提（2026-09-13 补，S-7a 的 OTA 踩到）**：`[ul_pipeline]`/`[ul_ldpc_decode]`
与三个分段**只统计 CRC-OK 的 TB** ⇒ 两轮的**授权成功率**与**CRC-OK 的 `iter` 分布**不同时，
它们量的不是同一批工作（实测同一份代码两轮差 6%，而 pre-LDPC 三段只差 0.3%）。
⇒ A/B 的负载指纹除 `samples`/`tbs`/`dft commits` 外，还要比
`grep -oE "crc=OK iter=[0-9.]+" /tmp/gnb.log | sort | uniq -c`（活文档 §46.9.1）。

### 10.7.2 怎么实现（小接口，两处）

1. 每个引擎暴露 **`last_gpu_span()` → `{GPUStartTime, GPUEndTime}`**（`0,0` 表示不可用）——
   现有 `last_gpu_wait_us()` 已经在读这两个字段，等价于把它拆开返回。
2. 新探针 `ul_gpu_lane_probe`：**由调用方**（lower PHY 的 FFT 提交点、PUSCH 处理器的提交点）
   按 slot 登记 span，最后一个 CB 完成后结算 ⇒ residency / busy / gap / period。
   *为什么由调用方登记*：引擎本身不知道 slot；而"哪些 CB 属于同一个 slot 的单行道"只有编排者知道。
3. `cpu` / `cpu_gpu` 档**保持现有分段不变**（那时 CPU 侧边界仍然真实存在）。

> **落地进度（S-7f 第一步已完成，`b8db30e9cc`）**：四个指标已实现，但**登记方式与上面第 2 点不同** ——
> 不是"由调用方按 slot 登记"，而是**由 `shared_burst` 按线程结算**：一条 lane = 一个线程上
> 上一次结算之后提交的全部 CB，结算点是 **burst 完成**（burst 就是产出 LLR 的那一段）。
> 登记点为 `shared_burst::commit()`（eq+demap）与信道估计引擎的 5 个 commit 点；
> 实现与门禁见活文档 §48.38。**已知覆盖缺口**：DFT 在前端队列、由 radio 线程提交，
> 线程本地的记账装不下它 ⇒ 目前的 `residency` 起点是"信道估计的 GPU 起点"，
> 而不是"IQ 进 GPU"。要补上这一步，需要**跨线程、按 slot** 的登记（DFT 引擎知道 `slot`，
> burst 不知道），即下面 §10.8 里 S-7f 的 push/collect API；DFT 挪到 lane 队列时会自然合并。

### 10.7.3 这套指标会立刻暴露什么

- **gap** 直接指出"CPU 没喂上 GPU"的时段——现在 pre-LDPC 每跳 ~180–200 µs 是 CPU 侧
  （§45.4），单行道下这些时间会以 gap 的形式出现 ⇒ gap 就是 K0 那套改动的收益计量器。
- **period** 会给出单行道的容量上限。⚠️ 现实检查：LDPC 仍在 CPU 且占 `ul_pipeline` 的 80%
  （≈3540 µs/授权），所以**整条 UL 的吞吐上限由 CPU 的 LDPC 决定**（≈280 授权/s），
  而不是 GPU 单行道 ⇒ `gpu` 档的 target 是"pre-LDPC 单行道跟得上 slot 速率（≤1 ms/slot 的 GPU 工作）"，
  真正的吞吐瓶颈要等 LDPC 也进 GPU 才解除。

## 10.8 建议的推进顺序（每步独立门禁、可单独回退）

| 步 | 内容 | 门禁 |
|---|---|---|
| **S-7a** ✅ `757593c84a` | CLI：`--phy_pipeline {auto,cpu,cpu_gpu,gpu}`（`gpu` 返回"未实现"错误）+ 有效配置日志 + 探针按模式切换（`gpu` 档的三个分段已按模式关掉，lane 指标留给 S-7f） | macOS：161/161 + 十个 metal 二进制 + 新单测 `du_low_phy_pipeline_test` 14/14 + YAML 往返 18/18；Ubuntu：7615/7615；**OTA 一轮（609 CRC-OK）：后端选择与结构性不变量逐项相同、pre-LDPC 三段 ±0.3%，LDPC/pipeline 的 −6% 由 CRC-OK 门控 + 迭代分布差异解释（活文档 §46.9）** |
| **S-7b** ✅ `1932dcc468`+`af90f0a8fe`+`f285cea33a` | 设备网格：FFT 的**最后一次写出**直写网格（相位补偿 + 上下半带映射 + cbf16）；网格存储页对齐 ⇒ 主机与设备**同一块内存**（UMA，"按需镜像"退化为一次同步，见活文档 §47.2）；`--device_resource_grid {auto,on,off}` 作 A/B 开关，`auto` 保持 `cpu_gpu` 与 tag 逐字同路径 | kernel 级**逐位比对** 0 mismatch（含窗表、含"防读错 slot"）、槽级端到端 **17808 RE 全一致**、`ctest -L phy` 162/162、Ubuntu 7616/7616；**发现：网格写不要做成第二个 dispatch**（活文档 §47.3） |
| **S-7c** ⚠️ **前提被证伪，且真正的瓶颈已定位（S-7c-0/0b/0c/1b/2）** | K0-a：网格→导频提取在 GPU；CE 不再读主机网格（主机数学暂留，导频小缓冲回主机 ≈1.8 KB） | **实测：导频提取只占一跳的 0.3%（0.46 µs / 176 µs @25 PRB）**，而 CE 的 90% 是"每跳编码命令缓冲"（~91 µs 主机 CPU）⇒ 按原样做是净亏。改为：① 并入 S-7d；或 ② 在 CE 自有 CB 内顺带做 + 统计量推迟，门禁改为"零主机网格读取 + Test 9/11/12 逐位一致"。**实测定案（§48.3/§48.6）：CE 每跳 ~150–230 µs 的等待来自"L=54 > K1 阶数上限 36 ⇒ 走唯一同步的 weights-only 入口"** ⇒ `bd48984ab4` 给该入口加异步形式并让两种求逆都推迟（CE 单测 mean hop 77.4→44.4 µs）；双缓冲环已 revert（`9f64dfae5d`），lane 级 ring 仍归 S-7f。**OTA 验证（§48.8，`bd48984ab4`，728 跳）：推迟生效（`defer_wait` 0→3.8 ms）、CE 段 mean 404→354 µs，但 `submit` 没降（234→247 µs），且 `defer_wait ≈ [ul_ldpc_decode] 3.6 ms` ⇒ CE 的等待是"后端队列队头被 LDPC 占住"继承来的，不是 CE 自己的流水线深度** |
| **S-7c-2** ✅ `6e7b83409f` | 纯探针：`[ldpc_time_sum]`/`[ldpc_time_shape]`（wall/pack/submit/**gpu 窗口**/gap/unpack + 迭代与上限直方图，按算法/基图/Z 分档）+ `[metal_stats] mmse_ce guard=`（引擎入口守护） | 本地定标（BG1 Z208 rate 0.5 cap 25，与 OTA 的 3602.9 µs 同几何）：分层 **3663 µs**（GPU 3346）/ 持久 **1407** / 洪泛 2524 / **CPU 48 µs**；机理＝分层核每层一次 dispatch ≈4.3 µs × 46 层 × 迭代数（活文档 §48.9）。门禁：`ctest -L phy` 162/162、LDPC 单测 ALL OK、CE Test 9/11/12 逐位一致（`guard=0/7356`，探针惰性） |
| **S-7c-3** ⏳ 下一步（OTA 两腿） | 腿 A：现状 `metal` + 新探针（取真实几何 / 迭代与上限直方图 / `gap`）；腿 B：`--pusch_ldpc_decoder_type metal_persistent` | `[ul_ldpc_decode]` 与 `[ul_pipeline]` 不退化、CRC-OK TB 数与 ping 健康不退化、`[ldpc_time_shape]` 给出几何与迭代分布；两腿背靠背 + DL PDSCH p50 指纹 |
| **S-7c-3** ✅ OTA A/C 两腿 | 腿 A：现状 `metal` LDPC + 新探针；腿 C：只把 LDPC 换成 CPU（`auto`） | **管线 4450.8 → 1015.1 µs（−77%）；管线 = 各段串行之和（无重叠）；`defer_wait` 3737.8 → 396.0（队头污染证实）、但 CE 每跳 `submit` 266→263 µs 与 `guard=0/652` 不变（"CE 的代价是继承来的"被证伪）⇒ CE 那 266 µs 是引擎调用自身的主机侧代价**。活文档 §48.10 |
| **测试台协议** ✅ 定案 | pre-LDPC 的一切 A/B 一律 `--pusch_ldpc_decoder_type auto`（CPU LDPC）：**GPU 队列留给 pre-LDPC 链**，LDPC 回 GPU 是它自己阶段的事 | pre-LDPC 新基线（每 slot）：TF 245 + CE 382 + EQ/DEMOD 344 = **971 µs / 管线 1015 µs** |
| **S-7c-4** ✅ 腿 D（`OCUDU_MMSE_DEBUG=1`） | 把那 266 µs 分到 `commandBuffer` / 编码 / `commit` | **答案：都不是。相位表 `run_weights_only n=712 wait=282.03us` vs `wrap 0.60 + cb 1.03 + encode 6.79 + commit 2.79` ⇒ 那 266 µs 是同步入口的 `waitUntilCompleted`；93% 的跳没走异步入口** |
| **S-7c-5** ✅ `90797e4f6a` | 根因：`engine_run()` 的 `bool defer = false` 默认值 —— 合并标准+尾块路径（13 PRB，`rem_prb=1`，只有实网走）的调用点少传一个参数，S-7c-1b 的 `merged_defer` 从未作用到它。去掉默认值 + 合并路径传入 `merged_defer` | CE 单测合并路径几何 submit 从同步等待 → **6.5–6.9 µs**，submit 均值 20.09 → 0.76、total 48.5 → 30.5、`cpl_wait` 0.1 → 185.4（等待搬到收集点）；Test 11/12 逐位不变、`ctest -L phy` 162/162、四个 metal 二进制 OK。活文档 §48.11（含对 §48.8⑥/§48.10 两条错读的更正） |
| **S-7c-5 实网确认** ✅ 腿 D/E | 同一二进制只差那一行（测试台 = CPU LDPC） | CE 段 **377.4 → 94.5 µs**、`[ul_pipeline]` **999.1 → 810.0**（−19%）、每跳 359.1 → 73.4 µs；账本闭合 −282.9 + 88.1 + 8.7 = −186.1 ≈ −189.1；`run_weights_only` n=712 → **n=2**；不变量全对。EQ/demod +88 µs 是 CE 的 146 µs CB 排在 burst 前面（min/median 整体平移）。活文档 §48.11 |
| **S-7c-6** ✅ 本地定标 | pre-LDPC 各段的 dispatch/commit 清单（`metal_dispatch_probe` + 各引擎计数器） | **EQ/demod 407 µs = 22 次 dispatch/occasion × ~18 µs**（11 符号均衡 + 11 符号解调，同一个 CB）；CE 4 次/hop；TF 1 次/slot（245 µs 是**核内计算**，~17 µs/transform）。本地 dispatch 单价：CB 内 **~10 µs 主机编码 + ~4 µs GPU**，同步形态 82–129 µs。活文档 §48.12 |
| **S-7e-batch** ⚠️ 部分完成：**引擎内部的批处理已打通、逐位正确，但多线程下仍不正确 ⇒ 保持 opt-in** | EQ/demod 从 22 次 dispatch 收到 **2 次**：`submit_group` 那条调用方路径**不做**（§48.15 已回退），改成**引擎内部**——`enqueue_burst` 累积 + flush hook 编码。`8f2c3502ca`：run 条件不再要求"同一个估计 buffer"（组 staging 本来就是逐符号拷，只比 `layer_stride`）+ `[metal_stats] eq_batch` 探针 + pending 改成**每引擎一条** + 析构时交出 pending + `shared_burst::flush_pending()/flush_hook_context()`。`54aeefccc5`：probe Pattern H（设备切片 + 实网几何 + 主机 oracle） | 单线程全绿：probe 全 pattern 逐位一致、均衡器单测 12 符号组 **batches=1** 且与 `equalize()` 逐位一致、`metal_back_ends_match_the_cpu_chain` 4896 LLR 全对、`ctest -L phy` 162/162；批量在均衡段 **2.0x**。⚠️ **并发用例仍红**（`group ≥ 10` 必红、`≤ 8` 全绿、两组之间插 CPU 同步即消失）⇒ 默认仍是立即编码，`OCUDU_EQ_DEFER_ENCODE=1` 才启用。详见活文档 §48.22/§48.23 |
| **S-7e-batch-2** ⏳ **下一步（本阶段主攻）** | 定位并修掉"同一 CB 内、批量 dispatch 的写入对后面 demapper dispatch 不可见"（§48.23 已把范围缩到 `shared_burst::encoder()` 的 flush/屏障顺序与 Metal 屏障 scope 两处） | 并发 4 worker × 15 轮 0 mismatch（连跑 ≥ 15 次）+ 上表所有单线程门禁 + 测试台 OTA 预期 EQ/demod **407 → ~150–250 µs**、`[ul_pipeline]` **810 → ~550–650 µs** |
| **S-7c-7** ⏳ 后置（防同类 bug） | 引擎入口相位做成常驻累加器（不再依赖 `OCUDU_MMSE_DEBUG`）+ `sync/defer` 跳数打进 `[mmse_time_sum]` | 探针必须报告"走了哪条路"，不能只报"花了多久"（S-7c-5 的教训） |
| **S-7d** | K0-b/c/d：LSE+CFO → 统计量 → 相关矩阵（**直接写引擎槽位**） | 同上 + staging memcpy 消失 |
| **S-7e** | 均衡器 `ch_re` 来自设备网格；LLR 设备侧整理 + 可选主机镜像 | LLR 与 `cpu_gpu` 档**逐位一致**（IQ 级 replay 工具） |
| **S-7f** | ring + push/collect API + `gpu` 档打开 + 新探针 | `[ul_llr_ready]` 有数、`[ul_pipeline]` 不退化、KO 表一致 |
| **S-7g** | （可选，后置）把 CE 的 K1..K4 编进 eq/demapper 那条共享 burst，进一步减少 CB 数与提交开销 | 同上 |

**与 LDPC 的关系**：`gpu` 档交付的是"IQ→LLR"，LDPC 仍在 CPU（用户明确）。但 LLR 缓冲按"设备 master +
可选主机镜像"设计，将来 LDPC 进 GPU 时无需再动接口。注意 LDPC 仍占 `ul_pipeline` 的 80%，
本项目的收益在 **pre-LDPC 的 CPU 时间与主机往返**上，不在总时间的数量级上。

## 10.9 用户已定的问题（2026-09-13）

1. **多 CB + 一次等待**：**接受**（§10.4 的方案）。
2. **PUCCH / PRACH**：都留在 **CPU**。
   - PRACH 需要大 size FFT，**一开始就没进 GPU path**；
   - PUCCH 是时间敏感的控制信令 ⇒ 处理仍走 CPU。GPU path 的强项是高并发/大带宽/高吞吐，对延迟容忍稍高。
   - 需要规划的是 **PUCCH 是否共享 GPU 的 FFT 输出**：结论见 §10.10.2（**共享 + 按需主机镜像**，不另算 FFT）。
3. **跳频/多端口**：见 §10.6 R4 的更正（跳频＝死代码，只需 assert；多端口＝arena 布局解决，**不拒绝**）。
4. 探针：已按用户定调改为"进 GPU → 出 GPU"的单一指标（§10.7）。
5. 推进顺序：暂按 §10.8。

## 10.10 同步模型与新增条目（2026-09-13，回答"如何同步 / 还有什么"）

### 10.10.1 Metal 上的依赖同步：**没有全局栅栏 → 把依赖推到 dispatch / CB 边界**

硬件事实：**Apple Silicon 没有 dispatch 内的全局栅栏**，只有 `threadgroup_barrier`（threadgroup 内）
与 `simdgroup_barrier`（simdgroup 内）⇒ **任何跨 threadgroup 的依赖都不能放在同一个 kernel 里**。
因此分层如下（这也是"多 CB + 一次等待"在 Metal 上成立的原因）：

| 层次 | 保证来自 | 需要做什么 |
|---|---|---|
| kernel **内** | `threadgroup_barrier` / `simdgroup_barrier` | 现有 kernel 已满足（每个 RE/符号互相独立） |
| kernel **间**（同一条 encoder） | Metal 保证 dispatch **按编码顺序执行** | **写→读可见性必须显式**：`memoryBarrierWithScope:MTLBarrierScopeBuffers`（CE 的 K2→K3 已经这么做） |
| **阶段间**（不同引擎/不同 CB） | **同一条队列 + 提交顺序** ⇒ CB 边界就是屏障（同一队列内下一个 CB 不会在前一个完成前开始） | **不等待**：CPU 只在 slot 末尾等一次 |
| **跨队列** | **无顺序保证** | lane 内必须**一条队列**（DFT 需从"前端队列"挪到 lane 的队列） |

⚠️ **要修的旧账**：K1→K1b→K2 目前注释写着"依赖 in-order execution"而**没有显式 barrier**
（`ocudu_metal_queue.mm` 之外的 `ocudu_metal_mmse_engine.mm` 注释亦如此），即依赖"硬件实际会串行相邻
dispatch"这一**未被规范保证**的行为。逐位门禁一直通过，但融合后 kernel 更小更多、交错更密
⇒ **计划加一条：所有跨 kernel 的写→读边界补显式 barrier（成本 ~ns）**。

### 10.10.2 PUCCH 共享 FFT 输出（回答"overhead vs complexity"）

事实：PUCCH 的**处理器**消费 `resource_grid_reader`（所有 format，`pucch_processor_impl.h:50-63`）
⇒ 它**今天就是共用这颗 FFT 的产物**，并不自己算 FFT（它自己的 DFT 只用于 format-1 detector 的序列相关）。

⇒ **共享 + 按需主机镜像**，不另建 FFT 路径：
- overhead：整张 UL 网格 ≈ 25 PRB×12×14×4 B ≈ **16.8 KB/端口/slot** ⇒ 拷贝 1–2 µs + 一个 CB，可忽略；
- **延迟不会变差**：PUCCH 今天也必须等 FFT 完成才能读网格，依赖关系相同，只是从"CPU 写网格"变成
  "CPU 等一次下载"；而且这次等待记在 **PUCCH 的账上**，不拖累 PUSCH 单行道（§10.6 R2）；
- 若让 PUCCH 自己再算一次 FFT ⇒ 多一份算力 + 两套相位补偿/缩放要对齐 ⇒ **不划算**。

### 10.10.3 本轮核对新增的条目（写进 §10.3/§10.8）

1. **barrier 纪律**（§10.10.1 的旧账）：跨 kernel 写→读边界的显式 barrier 审计。
2. **队列隔离**：PRACH/PUCCH 的 DFT 与 lane 的 FFT **不能共用一个引擎/队列**，否则时间敏感的
   RACH/PUCCH 会排到 PUSCH 单行道后面；lane 用自己的队列（或其上独立的 DFT 实例）。
3. **LLR 输出比原计划更省事（S-7e 缩小）**：`temp_llr` 已经是 `page_aligned_allocator` 缓冲、
   解调器**已直接写在设备上**（零拷贝）⇒ lane 的产物就是它，**CPU 每 slot 读一次**；
   **解扰/分块/UCI 解复用仍留在 CPU 的 Pass 3**（那需要 codeword buffer 的游标状态机）
   ⇒ **不需要"设备侧解扰 kernel"**，而 LLR 逐位一致是**构造性**的。
4. **CSI/统计量必须有一条不阻塞 lane 的路径**：UL 的 LA 依赖 CE 的 CSI（RSRP/SNR/TA/CFO），
   它们由主机从 filtered pilots 算出（正是 K0 要搬的东西）。lane 化后要么在 lane 结束的等待点做一次
   小拷贝（filtered pilots ≈ 7 KB）由主机算，要么在 GPU 上归约（RSRP/EPRE 易，TA/CFO 涉及拟合，难）。
   **不明确安排就会让 LA 拿到空 CSI ⇒ MCS 乱跳**（§44 的回归正是这么被发现的）。
5. **radio（实时）线程绝不能被 lane 阻塞**：FFT 的提交点在收 sample 之后 ⇒ 提交路径必须
   **预分配、无锁、无内存分配**；ring 满时**丢帧并计数**（策略要明确）。
6. **失败要响亮**：每个 slot 的 lane 需要一个有效性标志（CB 状态）；失败 ⇒ 该 slot 的 LLR 标为无效，
   绝不让 CPU 读到上一 slot 的数。
7. **arena 布局先行**：`(slot, port, hop, symbol)` 多维索引的 arena 是 lane 的地基，第一版就定下来
   （多端口、ring、将来的多跳都靠它）。

## 10.11 终局约束：**LLR 只是暂时的出口，终点是 MAC PDU**（用户 2026-09-13 补充）

用户明确：**最终目标是到 MAC PDU 才出 GPU**。现在让 LLR 中途交给 CPU，只是因为 **LDPC 当前性能不如意**
的**临时**安排。⇒ 架构规划必须**留好接口**，使 LLR 有可能**根本不出 GPU**，在 GPU 内部直接进 LDPC。

### 10.11.1 现在就要留的 8 个门（不是将来再改）

| # | 决定 | 为什么现在就要 |
|---|---|---|
| **D1** | lane 的产物定义为**设备 LLR 缓冲描述符**（`base` / 每符号 stride / 符号数 / 每符号 RE 数）：`{ device_ptr, symbol_stride, nof_symbols, nof_re_per_symbol, nof_layers, nof_bits }` | 若接口写成"返回一个主机 `span<log_likelihood_ratio>`"，将来 GPU→GPU 就得换接口 |
| **D2** | **主机镜像按需生成**（一次拷贝 CB + 一次等待），主机 span **不是**主接口 | 现在 CPU 要它（LDPC 在 CPU）；将来不要它，只是一个开关 |
| **D3** | 解扰拆成"**计划**（由 `c_init` 生成扰码序列，主机、便宜）" + "**施加**（XOR）"；施加这一步现在在主机，**留一个设备可替换的位置** | 序列下传后，一个 kernel 就能在设备上做 XOR，无需改计划 |
| **D4** | **码块 / UCI 解复用要有一份显式的"布局计划"**：由 (TBS, 调制, UCI 尺寸, rv) 决定的**确定性**映射 ⇒ 描述成 **scatter/gather 描述符**（每个码块：一串 `(符号, 偏移, 长度)` run） | ⚠️ 今天它是 `get_next_block_view` 的**游标状态机**，被解码器的消费节奏驱动 ⇒ 隐式。设备侧必须**事先算好**，所以现在要把它提炼成可用于两条路径的计划（CPU 路径可继续用游标实现，但计划必须存在/可导出） |
| **D5** | LDPC 引擎的输入**留"设备切片"入口**（基址+偏移+长度，§E-6c-2a 的 `ch_est_binding` 同款） | 将来 LLR 在设备上直接交给 LDPC，不需要主机指针 |
| **D6** | arena 预留 **码块 LLR 区**（能容纳 gather 出来的连续码块 LLR） | 免得将来重排 arena |
| **D7** | 探针 `[ul_gpu_lane]` 的 **residency 定义到"lane 最后一条 CB"**（与 lane 的终点无关） | 将来 LDPC 进 lane，指标**自动**变成"IQ→PDU"，无需改口径 |
| **D8** | LDPC 引擎将来需要**异步/可加入 lane** 的形式（现在逐码块 `commits=waits`、`max_in_flight=1`） | 与 S-6c-1a 给 CE 加 `run_async` 同款；LDPC 的性能问题解决后即可加入 |

### 10.11.2 同一条判据也适用于 LDPC 本身

LDPC 现在占 `ul_pipeline` 的 **80%**（≈3540/4430 µs），且**逐码块 commit+wait**
（`ldpc_decoder commits=waits max_in_flight=1`）⇒ 它犯的是**同一种架构错误**：CPU 在阶段内部等待。
⇒ §10.7 的 `busy` / `gap` 分解**同样适用于译码器**，这应当是进入 LDPC 时的**第一个测量**
（"3540 µs 里 GPU 真忙多少、等 CPU 多少"），因为它决定"改 kernel"还是"改架构"。

### 10.11.3 终局链路（写下来备查）

```
IQ(host) → [GPU lane: FFT → K0 → K1/K1b/K2 → K3/K4 → eq → demapper → 设备 LLR]
              ├─(临时) 主机镜像 → CPU: 解扰/解复用 → CPU LDPC → TB → FAPI → MAC
              └─(终局) 设备: 解扰 + 码块 gather → LDPC(设备, 异步) → TB/CRC → MAC PDU 出 GPU
```
两条路共用同一份**布局计划**（D3/D4）与同一个 **arena**（D6）——这正是现在留门的含义。
