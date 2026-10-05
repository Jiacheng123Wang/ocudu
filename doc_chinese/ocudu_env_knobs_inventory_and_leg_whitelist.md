# OCUDU_* 环境旋钮清单 + 验收腿白名单（**生成物** + 人工判读）

> 范围是**整棵树**（`lib/`、`apps/`、`include/`、`tests/` 里的 `getenv("OCUDU_*")`），不限于某一条工作线。
> 2026-10-01 从 `phy_latency/knob_inventory.md` 上移到本目录并改名（旧路径只作历史）。

> 生成方式：`python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/ocudu_env_knobs_inventory_and_leg_whitelist.md`
> 本次生成：commit `3bbdb7d6f7`。**不要手改正文**——改生成器或改人工判读小节。
> （生成器把**生成那一刻的 HEAD**写进这一行；要把这一行也追平 HEAD，就重跑生成器再提交一次——那一次是纯文档差异。）
>
> **默认值**是**从守卫表达式读出来的**（`ON` = 不设或非 0 都开；`OFF` = 必须显式置 1；`AUTO` = 由别处推导；`= 14` / `= "vdsp"` = 默认是一个**值**而不是开关，腿不设它时用的就是这个值；`?` = 需要读注释）。
> 生成器只认**本旋钮自己那次读取**的守卫（窗口在下一个旋钮的读取处截断），并且只认几种写法：`?` 里绝大多数是「置位即开」的探针/实验选择器（`static const bool x = (std::getenv("X") != nullptr);`），生成器**故意不把它们判成 `ON`** —— §1 是验收腿白名单，**宁可漏，不可错**。
> **飞过的腿数**来自**两个日志根**（`phy_pipeline_gpu/wip/logs` 与 `macos_thread_priority/wip/logs`，2026-10-05 起）里 `*.log.stderr` 顶部的 `knob : NAME=VALUE` 登记行 —— 这是**唯一能区分「新仪器」与「已退役」的一列**，源码里两者长得一样；只扫一个根会把另一条工作线飞过的旋钮报成「从没飞过」✗。

合计 **143** 个旋钮：**20** 个默认 `ON`（`OCUDU_DFT_RELEASE_TOKENS_EARLY` 于 2026-09-28 由 OFF 改为 ON：**理由 = 输入保持**，见开发文档 6.157；6.156 当初写的"吃掉接收尾巴 60-70×"**已被 6.157 撤回**）（= 交付形态的一部分）；**45** 个有腿登记行、**90** 个只在记录里出现过、**8** 个两处都没有；其中 **19** 个的首个读取点在 `test/`（离线臂）。

## 1. 交付形态的一部分（默认 `ON`）——**验收腿上不许出现「改成 OFF」的值**

| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |
|---|---|---|---|---|
| `OCUDU_CE_CORR_DEV` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:447` | 0 | — |
| `OCUDU_CE_CORR_FENCED` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:438` | 0 | — |
| `OCUDU_CE_DEV_SIGMA2` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:964` | 0 | — |
| `OCUDU_CE_DEV_STATS` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:754` | 0 | — |
| `OCUDU_CE_DEV_TA` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:805` | 0 | — |
| `OCUDU_CE_DEV_Y` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:953` | 0 | — |
| `OCUDU_CE_EDGE_FUSE` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:595` | 0 | — |
| `OCUDU_CE_GPU_INVERT` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:498` | 0 | — |
| `OCUDU_CE_HOLD_EXTRACTION` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:632` | 0 | — |
| `OCUDU_CE_K0A_RATIO_DEV` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:501` | 0 | — |
| `OCUDU_CE_TAIL_DEV` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:604` | 0 | — |
| `OCUDU_CE_Y_DIRECT` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3705` | 1 | 信道估计直接读 y（省一次 gather） |
| `OCUDU_DEMOD_DEFER_ENCODE` | ON | `lib/phy/upper/channel_modulation/metal/demodulation_mapper_metal.cpp:199` | 0 | 解映射延迟编码（融合车道的分组形状） |
| `OCUDU_DFT_OPEN_BLOCK` | ON | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1174` | 0 | 前端一个时隙的变换合成一条派发（P2-B′，V1 −38.7% 的那一刀） |
| `OCUDU_DFT_RELEASE_BLOCK` | ON | `include/ocudu/phy/phy_pipeline_grid_ready.h:293` | 9 | D1 交棒：前端块**不提交**就交给车道（融合车道的定义之一，5.9.49 起默认开） |
| `OCUDU_DFT_RELEASE_TOKENS_EARLY` | ON | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1251` | 1 | P2-E：输入令牌在"最后一个读输入的派发"之后释放（默认开的理由 = 去掉输入保持；**不是接收尾巴的修复** —— 见开发文档 6.157） |
| `OCUDU_EQ_DEFER_ENCODE` | ON | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1861` | 0 | — |
| `OCUDU_EQ_DEV_TABLES` | ON | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:442` | 0 | — |
| `OCUDU_EQ_DIRECT_GRID` | ON | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1296` | 1 | 均衡直接读网格（`y_gather` 消失，派发 10→6/跳） |
| `OCUDU_EQ_GATHER` | ON | `lib/phy/upper/channel_processors/metal/channel_equalizer_metal_factory.cpp:87` | 0 | — |

## 2. 探针、实验臂与消去法（**不是**交付形态；默认 `OFF` 或需要显式给值）

| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |
|---|---|---|---|---|
| `OCUDU_LANE_ABLATE` | OFF | `lib/phy/metal/ocudu_metal_burst.mm:541` | 16 | 消去法总开关：各阶段的绑定换成 `lane_ablate_noop`（只换 kernel，网格/屏障/提交结构不变） |
| `OCUDU_LANE_ABLATE_EVERY` | ? | `lib/phy/metal/ocudu_metal_burst.mm:546` | 15 | **修饰符**（默认 1 = 每跳都消去；只在 `OCUDU_LANE_ABLATE=1` 时有意义）：`=8` = 每 8 跳消去 1 跳，全消去手机接不进来（p79） |
| `OCUDU_LANE_ABLATE_STAGE` | OFF | `lib/phy/metal/ocudu_metal_burst.mm:375` | 10 | **修饰符**（只在 `OCUDU_LANE_ABLATE=1` 时有意义）：只消去哪一**阶段族**——`front_end`/`ce`/`eq`/`demap`（`|` 或 `,` 组合），不设 = `all` = 历史行为；拼错的名字按 `all` 处理并打 WARNING。族级账单靠它，覆盖度看报告里的 `Q9-F5 ablation coverage`（开发文档 6.162） |
| `OCUDU_METAL_GPU_TIME` | OFF | `lib/phy/metal/ocudu_metal_queue.mm:325` | 186 | **探针**：给每条 cb 装 GPU 时间戳（per-label 表的来源；验收腿一直带着它） |
| `OCUDU_UL_PHASE_SEGMENTS` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:2932` | 284 | **探针**：上行相位分段读数（验收腿一直带着它） |
| `OCUDU_UL_TIMING_EVENTS` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:1591` | 133 | **探针（新，开发文档 6.240–6.243）**：打印**最慢的 K 次接收等待**与**最迟的 K 次 DL 交接**，各带**宿主墙钟**（`wall=` UTC + `epoch_ms=` + 窗口两端 `began_ms=`/`due_ms=`）、当时的 `load1`，以及**本进程在那个窗口里的 CPU 时间与自愿/非自愿切换增量**（`cpu=`/`ivcsw=`/`nvcsw=`/`base_age=`）—— 用来把 `[ul_rx_wait]` 的尖峰、DL 的迟到和 `.log` 里 `[RF] Real-time failure in RF` 的行对到**同一条时间轴**上。★ 判读只看 **`cpu=` 对窗口**：`cpu ≈ wait` ⇒ 进程一直有 CPU ⇒ 是**电台/USB 侧**晚；`cpu ≈ 0` 或 `ivcsw>0` ⇒ 进程没被调度 ⇒ **宿主调度**。（`load1` 是 60 s 平均，**看不见 10 ms 级事件**，别用它判 —— 纪律 78） |
| `OCUDU_UL_SLOT_TRACE` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:1557` | 11 | **探针**：每**时隙**时间线（`=N` = 最多记 N 个时隙，非数字 = 开且用默认上限）。和上面两个一样被两条闸门当「任意值」接受，但它比相位分段宽得多，**验收腿不需要它**——只在追「某个时隙为什么晚」时开（开发文档 6.145⑹⑴；`=64` 曾打出 512 行，见 `ul_pipeline_probe.h` 的注） |
| `OCUDU_DFT_BACKEND` | = "vdsp" | `lib/phy/generic_functions/generic_functions_factories.cpp:195` | 2 | 前端变换的**后端选择**：`=vdsp`（Apple 上**不设就是它**，所以白名单接受）｜`=generic`（**A/B 对照臂**，n78 p170/p171、n1 p172/p173 用它跑 generic 那一侧）。非 Apple 平台根本不编进这条分支，所以这一行的「默认」只在 Apple 上有意义 |
| `OCUDU_DFT_BATCH_SYMBOLS` | AUTO | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:106` | 17 | 前端批量：不设 = `AUTO`（= 一个时隙自己的符号数，n78 上是 **14**）｜`=1` = 每符号对照臂｜`=7`/`=2` 是中间臂。白名单只接受与 AUTO 等价的 `=14` |
| `OCUDU_CE_LANE_ORDER` | ? | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1574` | 1 | 信道估计的四种车道顺序：`merged`（**默认**，估计器的派发搭车道共享 cb）｜`event`｜`wait`/`host_wait`｜`burst`（旧名 `OCUDU_CE_FUSED_BURST`）。拼错的值打 error 并按 `merged` 跑（代码里那条 warning 的原文就写着 "using merged"）。白名单只接受 `=merged` |
| `OCUDU_UL_STABILITY_WINDOWS` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:2107` | 123 | **探针（只打印，白名单可带）**：`=K` 把**本次运行**按时间顺序切成 K 个等样本窗口，逐序列打印每个窗口的 median/p95 与**相对整腿值的最大偏离** —— 这就是本线定义的**运行稳定性**（用户 2026-10-01：稳定性 = *同一次运行内*统计量变化不大；**跨腿**差异是环境造成的，本来就会变）。★ 它**不采新数据**：直接重切探针已经保存的样本向量（本就按时间顺序追加），**报告期零热路径成本**；关着（不设或 `=1`）一个字都不打印。反向臂已实测：把实现改成「先排序再切片」，单测的**交替**输入立刻变红（开发文档 10.20） |
| `OCUDU_SCHED_VERBOSE` | OFF | `lib/support/scheduling/thread_sched_snapshot.cpp:231` | 129 | **探针（只打印，白名单可带）**：每个 worker 线程创建后**回读**它真正拿到的调度状态，一行 `[sched] thread=… id=… rt_intent=… req=… eff=… run=… posix=…/…`。★ 它回答的是本线开线时的悬案「我们请求的 QoS 到底生效没有」——**第一次跑就给了答案**：请求 `USER_INTERACTIVE` 的线程回读 `eff=UNSPECIFIED`，而**不调用** `pthread_setschedparam` 的非实时线程回读 `eff=USER_INITIATED`（开发文档 10.5）。两把钥匙：`ENABLE_FLOW_PROBES` 编译 + 本变量非 `0`；两者缺一即**一个字都不打印**（默认关） |
| `OCUDU_UL_WATCHDOG` | ? | `lib/support/executors/ul_stall_watchdog.cpp:591` | 99 | **探针（只打印，白名单可带；macos_thread_priority 开发文档 10.47 / 规划 D.1）**：一个 **1 ms 唤醒**的看门狗线程，记录**自己的迟到量**（log2 直方图 + max），并在可疑时**采样**：(a) 系统忙闲用 `host_statistics(HOST_CPU_LOAD_INFO)` 的**每毫秒增量**（不是 `load1`——它 60 s 平均、看不见 10 ms 事件）；(b) 本进程每条线程的 `run_state` 与 **CPU 增量**（`thread_extended_info` + `thread_identifier_info`）。★ 它存在的理由：**三种停顿（进程被挂起 / 阻塞在驱动 / CPU 被抢）留下的指纹完全相同**，而**只有第三类**是调度手段能治的；不知道是哪一类，任何杠杆决策都是盲投，P4 的负结果也只能被记录、无法被解释。判读（规划 D.1）：看门狗自己迟到 + 系统空闲 ⇒ **①被挂起**；迟到 + 系统忙 ⇒ **饱和**；准时 + 目标线程 CPU 冻结且 `WAITING/UNINTERRUPTIBLE` ⇒ **②驱动阻塞**；准时 + CPU 冻结且 `RUNNING`（可运行却没被调度）⇒ **③CPU 被抢**；CPU 在涨 ⇒ **活儿本身慢**。两把钥匙：`ENABLE_FLOW_PROBES` 编译 + 本变量非 0；关着只读一次环境变量（不建线程、不读时钟、不打印）。反向臂已实测：注入 20 ms 迟到必须出现在 max 上，丢掉读数会让单测 3 条断言变红（开发文档 10.47） |
| `OCUDU_SCHED_ATTR_QOS` | OFF | `utils/macos_compat/macos_compat.cpp:449` | 0 | **实验臂（改 macOS 调度，**不在**白名单，fail-closed）**：把 QoS 类**声明在线程属性上**（`pthread_attr_set_qos_class_np`），让关键线程**从第一条指令**就在目标档上。默认关 = 历史行为。★ 注意它与 `OCUDU_SCHED_POSIX_RT=1` **不能同时用**：只要那个 POSIX 调用还在（现在只剩对照臂才调用），attr 上声明的档**同样会被抹掉**（实测，开发文档 10.5）。默认已经跳过那个调用，所以这一臂现在才有意义 —— 它买的是「起跑那一刻就在 P 核」 |
| `OCUDU_UL_THREAD_CPU` | ? | `include/ocudu/support/executors/ul_pipeline_probe.h:3004` | 117 | **探针（只打印，白名单可带；macos_thread_priority 开发文档 10.31）**：每条线程在自己的 **slot 变化**处读一次**自己的**累计 CPU，把两次之间的差值记进本线程的 count/sum/max + 一个 log2 直方图（40 桶），关停时每线程打一行 `[ul_thread_cpu] thread=… slots=… mean=… p99.9<=… max=… -> declare computation >= …`。★ 它存在的理由：**P4（Mach 时间约束）要申报「每 period 需要多少 CPU」，唯一诚实的来源就是线程自己每 slot 烧掉多少** —— 而相位事件的 `tcpu=` 在池线程上是**结构性**的 `-`（工作窃取 ⇒ 开窗与关窗不是同一条线程，`attach_cpu_delta` 只认同线程基线），进程口径的 `cpu=` 又是全进程（窗口 1.3–1.8 ms 却记到 4.4–6.9 ms）。两把钥匙：`ENABLE_FLOW_PROBES` 编译 + 本变量非 0；关着只读一次环境变量就返回，**不读时钟、不注册、不打印**（交付腿逐字节不变）；开着每次地标一次 Mach 调用（为此加了窄接口 `this_thread_cpu_ns()`）。反向臂已实测：把「按 slot 变化记一笔」改成「每次调用记一笔」，单测 3 条断言变红（开发文档 10.31(1)） |
| `OCUDU_SCHED_POSIX_RT` | OFF | `utils/macos_compat/macos_compat.cpp:641` | 1 | **对照臂（改 macOS 调度，**不在**白名单，fail-closed）**：`=1` = **恢复历史行为**，即对实时意图线程调用 `pthread_setschedparam(SCHED_FIFO,prio)`。★ **不设它才是新默认**（2026-10-01 用户裁决）：实测 Darwin 上线程**要么**由 QoS 管、**要么**是显式调度，那个 POSIX 调用会把刚设好的 QoS 类**静默抹掉且不可恢复**（再设返回 EPERM）；默认跳过它以后，`[sched]` 回读 `eff=USER_INTERACTIVE`（真腿读数见开发文档 10.15 与本次裁决 10.16）。保留这个臂是为了能**在同一个二进制上**做 A/B 推翻默认，而不是靠重新编译 |
| `OCUDU_UL_INLINE_PUSCH` | ? | `lib/du/du_low/du_low_executor_mapper.cpp:262` | 12 | **交付形状的一部分（2026-10-05 采纳）**：把 PUSCH 链（时间-频率之后的 CE、均衡、解映射）**内联到产出网格的那条线程**上 ⇒ 网格→lane 的派发消失；**解码仍留在池**（实例数 = 配置的 `max_pusch_and_srs_concurrency` ✓，p288 那次把它一起降到 1 的事故已修）。★ 与 `OCUDU_UL_PACED_LANE` **互斥**（两者都要拥有 `pusch_executor` ⇒ 同时给会 `report_fatal_error` ✓，而不是静默取最后一个写入者）。★ 在 **gpu/融合 lane** 下它会把 **Metal 提交+等待一起搬到调用线程**（实测 680 µs 塞 500 µs 时隙 ⇒ rx 线程必然落后 ✗）⇒ 那里它是**诊断臂**、不是交付形状，横幅会明说这一句（计划文档 11.94） |
| `OCUDU_UL_INLINE_DECODE` | ? | `lib/du/du_low/du_low_executor_mapper.cpp:333` | 10 | **交付形状的一部分（2026-10-05 采纳）**：LLR→CRC 判决之间也无派发 ⇒ **IQ→CRC OK 全在一个线程上**（= 一个预留主体 ✓）。代价：一个 TB 的码块**串行**解码（`pusch_decoder_executor={}` ✓ 有意为之）—— 在 528 B（单码块）TB 上实测无差别 ✓，大 TB 上才有 ✗。与 `OCUDU_UL_INLINE_PUSCH` **独立**（回答不同问题：一个管到 LLR，一个管到 CRC） |
| `OCUDU_UL_HANDOFF_PROBE` | ? | `lib/support/executors/handoff_probe.cpp:26` | 36 | **探针（只打印，白名单可带）**：两个交接点的延迟直方图 —— `rx_to_ul`（网格压给上行执行器）与 `ul_to_lane`（PUSCH lane 派发），关停时打 `[ul_handoff]`（p50/p90/p99/p99.9/max/mean + `over4ms` 计数 + 32×8 µs 直方图）✓。两把钥匙：`ENABLE_FLOW_PROBES` 编译 + 本变量非 0；关着只读一次环境变量（一个可预测分支 ✓）。★ 它是把「合并 rx+UL 把交接从 9 µs 变成 1 µs」量出来的那件仪器（计划文档 11.6x） |
| `OCUDU_UL_LANE_GRID` | ? | `utils/macos_compat/macos_compat.cpp:1119` | 44 | **节拍记账 + 探针（只记录与打印，白名单可带）**：武装 lane 的时隙网格，关停时打 `[lane_grid]`（armed/re-armed/clamped/**late**/unarmed ✓ + **FRONTIER 交付滞后**分布 + 相对最好偏移的**绝对滞后**及其 32 个 1 s 窗口最小值 —— 后者是**棘轮探测器** ✓）。★ 它**本身不改变调度**：消费它的是 `OCUDU_UL_PACED_LANE`/`OCUDU_UL_PACED_COMMIT` 那条臂（默认关 ✓）⇒ 验收腿带着它无害（`run_leg.sh` 的标准 KNOBS 一直带 ✓）。★ 读法：`late` 是「网格时刻已经过去」的跳数 = 该臂的判词 ✓ |
| `OCUDU_UL_SLOT_GRID` | ? | `include/ocudu/support/executors/ul_pipeline_probe.h:564` | 95 | **探针（只记录与打印，白名单可带）**：每时隙记一次地标（CE / COMMIT）并打印 `[ul_slot_grid]`（拟合周期 + 残差的 p50/p95/p99/max + 前后半均值漂移 ✓）—— 用来把「抖动」与「漂移」分开 ✓。★ 它是白名单的一次**漏登记**（不是判断）：`fly_leg.sh` 的标准 KNOBS 每条腿都带它，而两条闸门都没列，于是**交付腿自己会被判 FAIL** ✗ —— 2026-10-05 在 p297 上跑闸门时发现并补上 ✓（教训：动过闸门就要拿一条已知好腿回测 ✓） |
| `OCUDU_UL_LANE_GRID_GAIN_SHIFT` | ? | `utils/macos_compat/macos_compat.cpp:944` | 0 | **修饰符**（只在 lane grid 开着时有意义）：网格对到达的增益，默认 `1/65536` 是**设计而非调参** —— 网格必须是到达的**时钟**、不是它的跟随器，否则 `late` 与滞后读数会被它自己吸收掉 ✗（1/64 时 100 µs 的滞后偏移只让网格动 1.5 µs） |
| `OCUDU_UL_RX_POLL_WAIT` | ? | `utils/macos_compat/macos_compat.cpp:830` | 1 | **改行为（不在白名单，fail-closed）**：接收侧的等待策略（轮询 vs 阻塞）。不设 = 交付形态 ✓；只在 Apple 上编进调用点 ✓ |
| `OCUDU_PHY_BLOCKING_WAIT` | ? | `apps/services/worker_manager/worker_manager.cpp:46` | 0 | **改行为（不在白名单，fail-closed）**：四个 lower-PHY worker 的等待策略 —— 不设 = lockfree 队列 + 10/50 µs 轮询（交付默认 ✓）；`=1` = locking 队列 + 条件变量唤醒（**零轮询**，代价是每次唤醒一次系统调用）。启动横幅打印 `Lower PHY worker wait: …`，所以腿自己能证明它拿到的是哪一支 ✓ |
| `OCUDU_UL_LANE_CONCURRENCY` | ? | `lib/du/du_low/du_low_executor_mapper.cpp:119` | 3 | **实验臂（不在白名单）**：覆盖 lane 的并发度（`max_pusch_and_srs_concurrency`）。`=1` = 把 lane **串行化**，用来把「只有一个 worker」与「跑在网格上」这两件事分开 ✓（第一轮带宽扫描把两者混在一起了） |
| `OCUDU_UL_PACED_LANE` | ? | `lib/du/du_low/du_low_executor_mapper.cpp:172` | 10 | **实验臂（不在白名单）**：把 PUSCH lane 放到它自己的**网格节拍线程**（`pusch_lane`，lead/band 见启动打印 ✓）上。与 `OCUDU_UL_INLINE_PUSCH` **互斥** ✓。配套声明：`OCUDU_SCHED_TIME_CONSTRAINT=pusch_lane=<period>/<computation>/<constraint>`（周期取**实测的 UL 时隙周期**，别假设 ✓） |
| `OCUDU_UL_PACED_COMMIT` | ? | `lib/phy/metal/ocudu_metal_burst.mm:67` | 1 | **实验臂（不在白名单）**：把 lane 的**提交**交给网格节拍线程（`lane_commit`）⇒ 提交**时刻**周期化，而 hop 不必在该线程上 ✓（提交只有一行 `[cb commit]`，实测 14–47 µs ✓） |
| `OCUDU_UL_PACED_LEAD_US` | ? | `lib/support/executors/paced_task_executor.cpp:223` | 10 | **修饰符**（只在 paced lane/commit 开着时有意义）：节拍线程的 lead（µs）✓ |
| `OCUDU_UL_PACED_WAIT_US` | ? | `lib/support/executors/paced_task_executor.cpp:224` | 10 | **修饰符**（只在 paced lane/commit 开着时有意义）：节拍线程的等待/band（µs）✓ |
| `OCUDU_SCHED_TIME_CONSTRAINT` | ? | `utils/macos_compat/macos_compat.cpp:148` | 35 | **已采纳为交付形状的一部分（2026-10-05），但只接受一个值**：`lower_phy_rx#0=1000/300/500` ✓（与上面两个 INLINE 开关一起构成 UL 延迟的交付形状；该线程实测 duty 16.4–17.7 %，声明 30 % ⇒ 1.7 倍余量 ✓）。语法：不设/`0` = **关**（默认，逐字节不变）｜`1`/`default` = **把 2026-09-01 那一臂原样复现**（每个实时意图 worker 拿到同一个 `1 ms/1 ms/1 ms`，含其作用域）✗ 不许｜`NAME=P/C/K[;NAME=…]` = **逐线程**微秒值，`NAME=*` 匹配所有 worker（**同名精确匹配优先于 `*`**，与书写顺序无关）。畸形请求**一律拒绝且不施用**：`constraint<computation` 是唯一被实测有害的形状（p50 793 µs / max 7.3 ms），`period<constraint` 自相矛盾。★ **代价是必然的：施加它就等于删掉该线程的 QoS 档**（Darwin 上两者双向互斥且不可逆，实测开发文档 10.29）—— 对 rx 线程这笔交易划算 ✓，对池线程**不划算** ✗（见下）。★ **三对独立配对的判决**（计划文档 11.91/11.95/11.96）：rx 线程声明使 **max −8…−24 %**、跨腿 max 离散度 **204 → 10 µs** ✓、最坏线程 CPU 窗口与 watchdog late max **减半** ✓，而 median/p95/p99 与运行内稳定性（最差窗口 1.2–1.4 %）都在噪声内 ✓；**池线程声明一律拒绝** ✗ —— cpu+inline 下池不在 UL 链上（声明只换来 DL 低尾劣化 **2.9 倍**：`below1ms/1k` 7.07 → 20.52、p1 1005 → 975 µs，UL 侧零收益 ✓）；gpu 下池=lane，但它**阻塞在 GPU 等待上，预留对它无效**（UL 侧仍零收益）而 DL 代价照旧 ✗。施加时每个被选中的线程打一行 `[sched_tc]`，且 `[sched]` 行多一个 `tc=` 字段（`tc=none` / `tc=1000/300/500us(duty=30%)` ✓）；`ps -M` 里这些线程读作 `PRI 97R` ⇒ 两套独立读数可互证 ✓ |

> ⚠ **验收腿的旋钮白名单**（`milestone_audit.sh` 的 `kNOB_ANY`/`kNOB_EQ` 与 `leg_gate.sh` 的 `KNOB_ANY`/`KNOB_EQ` **就是它**，两边逐字一致）。
> **任意值**（探针）：`OCUDU_METAL_GPU_TIME`、`OCUDU_UL_PHASE_SEGMENTS`、`OCUDU_UL_SLOT_TRACE`、`OCUDU_UL_TIMING_EVENTS`（2026-10-01 加入：只**打印**最慢的接收等待 / 迟到交接及其宿主墙钟与进程 CPU 增量，不改变任何交付决定；关着不读时钟、不打印，开着最多存 64 条事件 —— 见开发文档 6.240/6.241）、`OCUDU_SCHED_VERBOSE`（2026-10-01 加入：每个 worker 线程**回读一次**自己的 QoS/POSIX 档并打一行，只打印、不改调度；默认关时一个字都不打印 —— 见 `doc_chinese/macos_thread_priority/` 开发文档 10.5）、`OCUDU_UL_THREAD_CPU`（2026-10-01 加入：**每线程每 slot CPU 记账**，关停时每线程打一行；P4 的 `computation` 只能从这个读数来，因为相位事件的 `tcpu=` 在池线程上是结构性的 `-`。关着不读时钟、不注册、不打印 —— 见该目录开发文档 10.31）、`OCUDU_UL_HANDOFF_PROBE`（2026-10-05 加入：两个交接点的延迟直方图 + `over4ms` 计数，只打印；关着只读一次环境变量 —— 计划文档 11.6x）、`OCUDU_UL_LANE_GRID`（2026-10-05 加入：节拍记账与 `[lane_grid]` 报告（含棘轮探测器），**本身不改调度** —— 消费它的是默认关的 paced-lane/commit 臂 —— `run_leg.sh` 的标准 KNOBS 一直带着它；计划文档 11.8x）、`OCUDU_UL_SLOT_GRID`（2026-10-05 加入，**补的是一次漏登记而不是一次判断**：`fly_leg.sh` 的标准 KNOBS 每条腿都带它，两条闸门却都没列，于是**交付腿自己会被判 FAIL** ✗ —— 在 p297 上跑闸门时发现 ✓）。
> **视为「等于交付默认」**：`OCUDU_DFT_BATCH_SYMBOLS=14`、`OCUDU_DFT_OPEN_BLOCK=1`、`OCUDU_DFT_RELEASE_BLOCK=1`、`OCUDU_CE_LANE_ORDER=merged`、`OCUDU_DFT_BACKEND=vdsp`（2026-10-01 加入：Apple 上这就是不设它时的值）；**`OCUDU_UL_INLINE_PUSCH=1`、`OCUDU_UL_INLINE_DECODE=1`、`OCUDU_SCHED_TIME_CONSTRAINT=lower_phy_rx#0=1000/300/500`**（2026-10-05 加入：这三个一起**就是** UL 延迟的交付形状 —— IQ→CRC OK 全在一个线程上、该线程拿 30 % duty 的预留。三对独立配对实测：max −8…−24 % 且跨腿离散度 204 → 10 µs，median/p95/p99 与运行内稳定性在噪声内 ✓ —— 计划文档 11.99）。其余一律判 FAIL（**fail-closed**）。
> ⚠ **`OCUDU_SCHED_TIME_CONSTRAINT` 只接受上面那一个值** ✗：`=1`/`=default`（2026-09-01 那一臂）**不许**；**任何池线程**（`main_pool#*`）的声明也**不许** —— cpu+inline 下池不在 UL 链上（声明只换来 DL 低尾劣化 2.9 倍、UL 侧零收益），gpu 下池=lane 但阻塞在 GPU 等待上、预留无效而 DL 代价照旧（计划文档 11.95/11.96）。`OCUDU_PHY_BLOCKING_WAIT`、`OCUDU_UL_RX_POLL_WAIT` 同属**改行为**的旋钮：不在白名单，默认（不设）才是交付形态。
> `OCUDU_SCHED_POSIX_RT`（改 macOS 调度）与 `OCUDU_SCHED_ATTR_QOS` 同样**故意不**在白名单里（臂，fail-closed）；`OCUDU_DFT_BACKEND=generic` **故意不**在白名单里：那是一条 A/B **臂**——臂可以满足其余所有判据（p84 就是这样），闸门拦的就是它。
> 6.215 起交付车道的网格由 **host** 写，所以 `OCUDU_DFT_BATCH_SYMBOLS`/`OCUDU_DFT_OPEN_BLOCK`/`OCUDU_DFT_RELEASE_BLOCK` 对交付腿是 **MOOT**（那个引擎根本不在路上）；**最有力的交付腿是一个旋钮都不设**，白名单只是给「已经设了」的腿留出等于默认的写法。
> `OCUDU_UL_RX_SYMBOLS` 也不在白名单里，但它的**默认值就是 `= 1`**（每符号一跳）——交付腿不设它即得交付形态，设 `=7`/`=14` 才是改形状。

## 3. 全部旋钮（自动生成）

| 旋钮 | 默认 | 范围 | 首次读取点 | 站点 | 模块 | 腿登记行 | 记录提及(次/文件) | 腿上出现过的值 |
|---|---|---|---|---|---|---|---|---|
| `OCUDU_CE_CACHE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3862` | 1 | `lib/phy/upper/signal_processors` | 1 | 2/1 | 1 |
| `OCUDU_CE_CFO_CARRY_HOST` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:727` | 1 | `lib/phy/upper/signal_processors` | 0 | 14/5 | — |
| `OCUDU_CE_CHAIN_MAP` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4160` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_CORR_BARRIER_AFTER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3999` | 2 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_CORR_CHECK` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:481` | 1 | `lib/phy/upper/signal_processors` | 0 | 5/3 | — |
| `OCUDU_CE_CORR_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:447` | 1 | `lib/phy/upper/signal_processors` | 0 | 43/9 | — |
| `OCUDU_CE_CORR_FENCE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3996` | 1 | `lib/phy/upper/signal_processors` | 0 | 13/1 | — |
| `OCUDU_CE_CORR_FENCED` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:438` | 1 | `lib/phy/upper/signal_processors` | 0 | 22/7 | — |
| `OCUDU_CE_CORR_MERGED` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:654` | 1 | `lib/phy/upper/signal_processors` | 1 | 7/1 | 1 |
| `OCUDU_CE_CORR_SEGMENT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3984` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/2 | — |
| `OCUDU_CE_CORR_STANDALONE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4717` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/2 | — |
| `OCUDU_CE_CORR_UNIFORM` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:677` | 1 | `lib/phy/upper/signal_processors` | 0 | 4/1 | — |
| `OCUDU_CE_CPU_CE` | ? | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:41` | 2 | `lib/phy/upper/channel_processors` | 0 | 37/11 | — |
| `OCUDU_CE_CPU_INVERT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:502` | 1 | `lib/phy/upper/signal_processors` | 0 | 20/8 | — |
| `OCUDU_CE_CPU_LS` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:455` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/16 | — |
| `OCUDU_CE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2247` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_DEV_INVERT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:4560` | 1 | `lib/phy/upper/signal_processors` | 0 | 24/8 | — |
| `OCUDU_CE_DEV_SIGMA2` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:964` | 1 | `lib/phy/upper/signal_processors` | 0 | 33/10 | — |
| `OCUDU_CE_DEV_STATS` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:754` | 1 | `lib/phy/upper/signal_processors` | 0 | 17/7 | — |
| `OCUDU_CE_DEV_TA` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:805` | 1 | `lib/phy/upper/signal_processors` | 0 | 15/7 | — |
| `OCUDU_CE_DEV_Y` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:953` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/11 | — |
| `OCUDU_CE_DFT_TIME` | OFF | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:693` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_EDGE_CHECK` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:493` | 1 | `lib/phy/upper/signal_processors` | 0 | 9/7 | — |
| `OCUDU_CE_EDGE_DIAG` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:2651` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_CE_EDGE_FUSE` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:595` | 1 | `lib/phy/upper/signal_processors` | 0 | 36/13 | — |
| `OCUDU_CE_FD_HZ` | = 0.0 | test | `tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test_factories.cpp:260` | 1 | `tests/integrationtests` | 0 | 0/0 | — |
| `OCUDU_CE_FUSED_BURST` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1601` | 1 | `lib/phy/upper/signal_processors` | 0 | 20/7 | — |
| `OCUDU_CE_GPU_INVERT` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:498` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/11 | — |
| `OCUDU_CE_HOLD_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1260` | 6 | `lib/phy/upper/signal_processors` | 2 | 1/1 | 1 |
| `OCUDU_CE_HOLD_EXTRACTION` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:632` | 1 | `lib/phy/upper/signal_processors` | 0 | 19/7 | — |
| `OCUDU_CE_HOST_GRID` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:699` | 1 | `lib/phy/upper/signal_processors` | 0 | 47/9 | — |
| `OCUDU_CE_HOST_SCALARS` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:613` | 1 | `lib/phy/upper/signal_processors` | 0 | 40/9 | — |
| `OCUDU_CE_HOST_Y_PADS` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:835` | 1 | `lib/phy/upper/signal_processors` | 0 | 9/4 | — |
| `OCUDU_CE_INVERT_FIRST` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3831` | 2 | `lib/phy/upper/signal_processors` | 0 | 17/7 | — |
| `OCUDU_CE_INV_BARRIERS` | = 0 | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:696` | 1 | `lib/phy/upper/signal_processors` | 0 | 13/6 | — |
| `OCUDU_CE_K0A_RATIO_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2209` | 1 | `lib/phy/upper/signal_processors` | 0 | 7/4 | — |
| `OCUDU_CE_K0A_RATIO_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:501` | 1 | `lib/phy/upper/signal_processors` | 0 | 17/11 | — |
| `OCUDU_CE_LANE_ORDER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1574` | 1 | `lib/phy/upper/signal_processors` | 1 | 67/21 | event |
| `OCUDU_CE_LS_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:674` | 1 | `lib/phy/upper/signal_processors` | 0 | 14/6 | — |
| `OCUDU_CE_MATRIX_CACHE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:91` | 1 | `lib/phy/upper/signal_processors` | 2 | 4/1 | 1 |
| `OCUDU_CE_NO_K4` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1907` | 1 | `lib/phy/upper/signal_processors` | 0 | 11/7 | — |
| `OCUDU_CE_NV_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:5155` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/3 | — |
| `OCUDU_CE_NV_OVERRIDE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:5210` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/3 | — |
| `OCUDU_CE_NV_ROUTE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp:316` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/1 | — |
| `OCUDU_CE_PAD_SENTINEL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4574` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_PP_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2092` | 2 | `lib/phy/upper/signal_processors` | 0 | 4/2 | — |
| `OCUDU_CE_PP_PERTURB` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2170` | 2 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_CE_RSRP_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1820` | 5 | `lib/phy/upper/signal_processors` | 0 | 20/9 | — |
| `OCUDU_CE_SIGMA2_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2150` | 1 | `lib/phy/upper/signal_processors` | 0 | 7/3 | — |
| `OCUDU_CE_SPLIT_TAIL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2295` | 1 | `lib/phy/upper/signal_processors` | 0 | 26/9 | — |
| `OCUDU_CE_TAIL_CPU` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2293` | 2 | `lib/phy/upper/signal_processors` | 0 | 6/4 | — |
| `OCUDU_CE_TAIL_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:604` | 1 | `lib/phy/upper/signal_processors` | 0 | 21/8 | — |
| `OCUDU_CE_TAU_RMS_US` | = 0.37e-6 | test | `tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test_factories.cpp:256` | 1 | `tests/integrationtests` | 0 | 0/0 | — |
| `OCUDU_CE_TA_CHAIN` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1129` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/3 | — |
| `OCUDU_CE_TA_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1499` | 7 | `lib/phy/upper/signal_processors` | 0 | 8/4 | — |
| `OCUDU_CE_TA_ONLY` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1125` | 1 | `lib/phy/upper/signal_processors` | 0 | 0/0 | — |
| `OCUDU_CE_WAIT_TRACE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:388` | 3 | `lib/phy/upper/signal_processors` | 0 | 13/6 | — |
| `OCUDU_CE_WEIGHTS_BARRIER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:4063` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_WEIGHTS_TILE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:664` | 1 | `lib/phy/upper/signal_processors` | 2 | 4/1 | 1 |
| `OCUDU_CE_WRAP_MAP` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:606` | 1 | `lib/phy/upper/signal_processors` | 0 | 5/4 | — |
| `OCUDU_CE_Y_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3121` | 1 | `lib/phy/upper/signal_processors` | 0 | 10/3 | — |
| `OCUDU_CE_Y_DIRECT` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3705` | 1 | `lib/phy/upper/signal_processors` | 1 | 14/2 | 0 |
| `OCUDU_CE_Y_HASH` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:5188` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/1 | — |
| `OCUDU_D1_HANDED_BOUND` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:131` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 0 | 7/3 | — |
| `OCUDU_DEMOD_DEFER_ENCODE` | ON | lib | `lib/phy/upper/channel_modulation/metal/demodulation_mapper_metal.cpp:199` | 1 | `lib/phy/upper/channel_modulation` | 0 | 8/2 | — |
| `OCUDU_DFT_BACKEND` | = "vdsp" | lib | `lib/phy/generic_functions/generic_functions_factories.cpp:195` | 1 | `lib/phy/generic_functions/generic_functions_factories.cpp` | 2 | 26/3 | generic |
| `OCUDU_DFT_BACKEND_QUEUE` | OFF | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1766` | 1 | `lib/phy/generic_functions/metal` | 1 | 13/5 | 1 |
| `OCUDU_DFT_BATCH_SYMBOLS` | AUTO | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:106` | 1 | `lib/phy/generic_functions/metal` | 17 | 35/5 | 1,14,2,7 |
| `OCUDU_DFT_GRID_AUDIT` | OFF | lib | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:166` | 2 | `lib/phy/lower/modulation` | 2 | 2/2 | 1 |
| `OCUDU_DFT_OPEN_BLOCK` | ON | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1174` | 2 | `lib/phy/generic_functions/metal` | 0 | 32/12 | — |
| `OCUDU_DFT_PIPELINE_DEPTH` | ? | lib | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:577` | 1 | `lib/phy/lower/modulation` | 0 | 18/6 | — |
| `OCUDU_DFT_RELEASE_BLOCK` | ON | include | `include/ocudu/phy/phy_pipeline_grid_ready.h:293` | 1 | `include/ocudu` | 9 | 73/12 | 0,1 |
| `OCUDU_DFT_RELEASE_TOKENS_EARLY` | ON | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1251` | 1 | `lib/phy/generic_functions/metal` | 1 | 18/7 | 1 |
| `OCUDU_DFT_STAGE_INPUT` | OFF | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1575` | 2 | `lib/phy/generic_functions/metal` | 3 | 16/4 | 1 |
| `OCUDU_DFT_WAIT_PER_SLOT` | OFF | lib | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:159` | 1 | `lib/phy/lower/modulation` | 3 | 7/2 | 1 |
| `OCUDU_EQ_DEFER_ENCODE` | ON | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1861` | 5 | `lib/phy/upper/channel_processors` | 0 | 37/11 | — |
| `OCUDU_EQ_DEV_TABLES` | ON | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:442` | 1 | `lib/phy/upper/channel_processors` | 0 | 4/3 | — |
| `OCUDU_EQ_DIRECT_GRID` | ON | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1296` | 1 | `lib/phy/upper/channel_processors` | 1 | 6/1 | 0 |
| `OCUDU_EQ_GATHER` | ON | lib | `lib/phy/upper/channel_processors/metal/channel_equalizer_metal_factory.cpp:87` | 1 | `lib/phy/upper/channel_processors` | 0 | 18/2 | — |
| `OCUDU_EQ_TABLE_CHECK` | ? | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1112` | 1 | `lib/phy/upper/channel_processors` | 0 | 5/3 | — |
| `OCUDU_GPU_STRICT` | ? | include | `include/ocudu/phy/phy_pipeline_strict.h:31` | 1 | `include/ocudu` | 0 | 18/9 | — |
| `OCUDU_HANDOFF_ADDR` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:155` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HANDOFF_DM_DEFER` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:233` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HANDOFF_EQ_DEFER` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:232` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HANDOFF_WIDE` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:231` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HELENA_DUMP_DIR` | OFF | lib | `lib/phy/upper/channel_processors/pusch/pusch_decoder_impl.cpp:505` | 5 | `lib/phy/upper/channel_processors` | 0 | 7/3 | — |
| `OCUDU_HELENA_FORCE_NN` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_helena_impl.cpp:143` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_INV_MEMNONE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3025` | 1 | `lib/phy/upper/signal_processors` | 0 | 4/3 | — |
| `OCUDU_INV_RL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3029` | 1 | `lib/phy/upper/signal_processors` | 0 | 21/9 | — |
| `OCUDU_INV_SYSTEMS` | = 4 | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1448` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_INV_TGX` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2988` | 1 | `lib/phy/upper/signal_processors` | 0 | 10/8 | — |
| `OCUDU_INV_TGY` | = 16 | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2989` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_L1_CLAIM_ONLY` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:1149` | 1 | `lib/phy/upper/channel_processors` | 0 | 2/2 | — |
| `OCUDU_L1_CONSUMER_SLOT_SKEW` | = 0 | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:725` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_L1_DROP_CONSUMER_WAIT` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:713` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_L1_DROP_MISS_WAIT` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:204` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_L1_HOST_FIRST` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:1134` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_LANE_ABLATE` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:541` | 3 | `lib/phy/metal/ocudu_metal_burst.mm` | 16 | 71/5 | 1 |
| `OCUDU_LANE_ABLATE_EVERY` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:546` | 3 | `lib/phy/metal/ocudu_metal_burst.mm` | 15 | 12/3 | 8 |
| `OCUDU_LANE_ABLATE_STAGE` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:375` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 10 | 19/5 | all,ce,demap,eq,front_end |
| `OCUDU_LANE_DIAG_SPLIT` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:975` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 3 | 29/7 | 1 |
| `OCUDU_METAL_GPU_TIME` | OFF | lib | `lib/phy/metal/ocudu_metal_queue.mm:325` | 2 | `lib/phy/metal/ocudu_metal_queue.mm` | 186 | 138/27 | 1 |
| `OCUDU_MMSE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:413` | 2 | `lib/phy/upper/signal_processors` | 0 | 21/7 | — |
| `OCUDU_PHY_BLOCKING_WAIT` | ? | apps | `apps/services/worker_manager/worker_manager.cpp:46` | 1 | `apps/services` | 0 | 5/1 | — |
| `OCUDU_PROBE_GAPPED` | ? | test | `lib/phy/upper/channel_processors/metal/test/metal_chain_probe.cpp:268` | 1 | `lib/phy/upper/channel_processors` | 0 | 8/2 | — |
| `OCUDU_PROBE_RE` | = 1272 | test | `lib/phy/upper/channel_processors/metal/test/metal_dispatch_probe.cpp:57` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_PUSCH_DEFERRED_GROUP` | ? | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:413` | 2 | `lib/phy/upper/channel_processors` | 0 | 8/4 | — |
| `OCUDU_PUSCH_FORCE_SERIAL` | OFF | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:303` | 2 | `lib/phy/upper/channel_processors` | 0 | 6/3 | — |
| `OCUDU_REPLAY_TRACE` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:292` | 3 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_SCHED_ATTR_QOS` | OFF | utils | `utils/macos_compat/macos_compat.cpp:449` | 1 | `utils/macos_compat` | 0 | 10/4 | — |
| `OCUDU_SCHED_POSIX_RT` | OFF | utils | `utils/macos_compat/macos_compat.cpp:641` | 1 | `utils/macos_compat` | 1 | 17/5 | 1 |
| `OCUDU_SCHED_TIME_CONSTRAINT` | ? | utils | `utils/macos_compat/macos_compat.cpp:148` | 1 | `utils/macos_compat` | 35 | 20/4 | lower_phy_rx#0=1000/300/500,lower_phy_rx#0=1000/300/500;main_pool#0=1000/150/300;main_pool#1=1000/150/300;main_pool#2=1000/150/300;main_pool#3=1000/150/300;main_pool#4=1000/150/300,lower_phy_rx#0=1000/300/500;main_pool#0=1000/300/600;main_pool#1=1000/300/600;main_pool#2=1000/300/600;main_pool#3=1000/300/600;main_pool#4=1000/300/600,lower_phy_rx#0=500/250/350,main_pool#0=1000/600/850;main_pool#1=1000/600/850;main_pool#2=1000/600/850;main_pool#3=1000/600/850;main_pool#4=1000/600/850,main_pool#0=5000/1000/2000;main_pool#1=5000/1000/2000;main_pool#2=5000/1000/2000;main_pool#3=5000/1000/2000;main_pool#4=5000/1000/2000 |
| `OCUDU_SCHED_VERBOSE` | OFF | lib | `lib/support/scheduling/thread_sched_snapshot.cpp:231` | 1 | `lib/support` | 129 | 21/4 | 1 |
| `OCUDU_UL_DUMP` | ? | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:31` | 2 | `lib/phy/upper/channel_processors` | 0 | 75/12 | — |
| `OCUDU_UL_DUMP_COUNT` | = 8 | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:37` | 1 | `lib/phy/upper/channel_processors` | 0 | 11/6 | — |
| `OCUDU_UL_DUMP_LLR` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:502` | 2 | `lib/phy/upper/channel_processors` | 0 | 7/2 | — |
| `OCUDU_UL_DUMP_MAX_RB` | = 0 | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:52` | 1 | `lib/phy/upper/channel_processors` | 0 | 3/2 | — |
| `OCUDU_UL_DUMP_TD` | ? | lib | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:35` | 2 | `lib/phy/lower/processors` | 0 | 14/7 | — |
| `OCUDU_UL_DUMP_TD_SLOTS` | = 4 | lib | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:45` | 1 | `lib/phy/lower/processors` | 0 | 2/2 | — |
| `OCUDU_UL_HANDOFF_PROBE` | ? | lib | `lib/support/executors/handoff_probe.cpp:26` | 1 | `lib/support` | 36 | 4/1 | 1 |
| `OCUDU_UL_INLINE_DECODE` | ? | lib | `lib/du/du_low/du_low_executor_mapper.cpp:333` | 1 | `lib/du` | 10 | 2/1 | 1 |
| `OCUDU_UL_INLINE_PUSCH` | ? | lib | `lib/du/du_low/du_low_executor_mapper.cpp:262` | 1 | `lib/du` | 12 | 5/1 | 1 |
| `OCUDU_UL_LANE_CONCURRENCY` | ? | lib | `lib/du/du_low/du_low_executor_mapper.cpp:119` | 1 | `lib/du` | 3 | 5/1 | 1,2 |
| `OCUDU_UL_LANE_GRID` | ? | utils | `utils/macos_compat/macos_compat.cpp:1119` | 1 | `utils/macos_compat` | 44 | 12/1 | 1 |
| `OCUDU_UL_LANE_GRID_GAIN_SHIFT` | ? | utils | `utils/macos_compat/macos_compat.cpp:944` | 1 | `utils/macos_compat` | 0 | 1/1 | — |
| `OCUDU_UL_PACED_COMMIT` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:67` | 2 | `lib/phy/metal/ocudu_metal_burst.mm` | 1 | 2/1 | 500 |
| `OCUDU_UL_PACED_LANE` | ? | lib | `lib/du/du_low/du_low_executor_mapper.cpp:172` | 4 | `lib/du` | 10 | 6/1 | 500 |
| `OCUDU_UL_PACED_LEAD_US` | ? | lib | `lib/support/executors/paced_task_executor.cpp:223` | 2 | `lib/support` | 10 | 1/1 | 0 |
| `OCUDU_UL_PACED_WAIT_US` | ? | lib | `lib/support/executors/paced_task_executor.cpp:224` | 2 | `lib/support` | 10 | 4/1 | 0,1000,125,2000,250,300 |
| `OCUDU_UL_PHASE_SEGMENTS` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:2932` | 1 | `include/ocudu` | 284 | 132/25 | 1 |
| `OCUDU_UL_RX_POLL_WAIT` | ? | utils | `utils/macos_compat/macos_compat.cpp:830` | 1 | `utils/macos_compat` | 1 | 8/4 | 1 |
| `OCUDU_UL_RX_POOL_DROP` | ? | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1274` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 1 | 17/2 | 0 |
| `OCUDU_UL_RX_POOL_DROP_FORCE` | = 0 | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1287` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 1 | 11/3 | 20 |
| `OCUDU_UL_RX_POOL_SIZE` | ? | lib | `lib/phy/lower/lower_phy_factory.cpp:226` | 1 | `lib/phy/lower/lower_phy_factory.cpp` | 1 | 8/4 | 12 |
| `OCUDU_UL_RX_SYMBOLS` | = 1 | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1027` | 2 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 4 | 88/15 | 1,7 |
| `OCUDU_UL_SLOT_GRID` | ? | include | `include/ocudu/support/executors/ul_pipeline_probe.h:564` | 1 | `include/ocudu` | 95 | 7/3 | 1 |
| `OCUDU_UL_SLOT_TRACE` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:1557` | 1 | `include/ocudu` | 11 | 43/14 | 512 |
| `OCUDU_UL_STABILITY_WINDOWS` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:2107` | 1 | `include/ocudu` | 123 | 7/4 | 8 |
| `OCUDU_UL_STALE_US` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:3163` | 1 | `include/ocudu` | 0 | 4/3 | — |
| `OCUDU_UL_THREAD_CPU` | ? | include | `include/ocudu/support/executors/ul_pipeline_probe.h:3004` | 1 | `include/ocudu` | 117 | 7/2 | 1 |
| `OCUDU_UL_TIMING_EVENTS` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:1591` | 1 | `include/ocudu` | 133 | 32/7 | 16 |
| `OCUDU_UL_WATCHDOG` | ? | lib | `lib/support/executors/ul_stall_watchdog.cpp:591` | 1 | `lib/support` | 99 | 4/1 | 1 |
| `OCUDU_USRSCTP_MODE` | ? | lib | `lib/gateways/sctp_socket_usrsctp.cpp:117` | 1 | `lib/gateways` | 0 | 2/1 | — |

### 3.1 既没有腿登记行、也从未在记录里出现过：8 个

* **离线/测试臂 8 个**（首个读取点在 `test/`）——它们本来就不上空口，没有腿、没有记录是**正常**的：

```
OCUDU_CE_FD_HZ
OCUDU_CE_TAU_RMS_US
OCUDU_CE_TA_ONLY
OCUDU_HANDOFF_ADDR
OCUDU_HANDOFF_DM_DEFER
OCUDU_HANDOFF_EQ_DEFER
OCUDU_HANDOFF_WIDE
OCUDU_PROBE_RE
```

* ★ **落在交付代码里的 0 个 = 真正的退役候选**（源码里分不出「新仪器」与「已死」，要读注释再决定）：

```
```
