# OCUDU_* 环境旋钮清单 + 验收腿白名单（**生成物** + 人工判读）

> 范围是**整棵树**（`lib/`、`apps/`、`include/`、`tests/` 里的 `getenv("OCUDU_*")`），不限于某一条工作线。
> 2026-10-01 从 `phy_latency/knob_inventory.md` 上移到本目录并改名（旧路径只作历史）。

> 生成方式：`python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/ocudu_env_knobs_inventory_and_leg_whitelist.md`
> 本次生成：commit `d73d0864bd`。**不要手改正文**——改生成器或改人工判读小节。
> （生成器把**生成那一刻的 HEAD**写进这一行；要把这一行也追平 HEAD，就重跑生成器再提交一次——那一次是纯文档差异。）
>
> **默认值**是**从守卫表达式读出来的**（`ON` = 不设或非 0 都开；`OFF` = 必须显式置 1；`AUTO` = 由别处推导；`= 14` / `= "vdsp"` = 默认是一个**值**而不是开关，腿不设它时用的就是这个值；`?` = 需要读注释）。
> 生成器只认**本旋钮自己那次读取**的守卫（窗口在下一个旋钮的读取处截断），并且只认几种写法：`?` 里绝大多数是「置位即开」的探针/实验选择器（`static const bool x = (std::getenv("X") != nullptr);`），生成器**故意不把它们判成 `ON`** —— §1 是验收腿白名单，**宁可漏，不可错**。
> **飞过的腿数**来自 `logs/*.log.stderr` 顶部的 `knob : NAME=VALUE` 登记行 —— 这是**唯一能区分「新仪器」与「已退役」的一列**，源码里两者长得一样。

合计 **123** 个旋钮：**20** 个默认 `ON`（`OCUDU_DFT_RELEASE_TOKENS_EARLY` 于 2026-09-28 由 OFF 改为 ON：**理由 = 输入保持**，见开发文档 6.157；6.156 当初写的"吃掉接收尾巴 60-70×"**已被 6.157 撤回**）（= 交付形态的一部分）；**28** 个有腿登记行、**87** 个只在记录里出现过、**8** 个两处都没有；其中 **19** 个的首个读取点在 `test/`（离线臂）。

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
| `OCUDU_CE_Y_DIRECT` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3704` | 1 | 信道估计直接读 y（省一次 gather） |
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
| `OCUDU_LANE_ABLATE` | OFF | `lib/phy/metal/ocudu_metal_burst.mm:450` | 16 | 消去法总开关：各阶段的绑定换成 `lane_ablate_noop`（只换 kernel，网格/屏障/提交结构不变） |
| `OCUDU_LANE_ABLATE_EVERY` | ? | `lib/phy/metal/ocudu_metal_burst.mm:455` | 15 | **修饰符**（默认 1 = 每跳都消去；只在 `OCUDU_LANE_ABLATE=1` 时有意义）：`=8` = 每 8 跳消去 1 跳，全消去手机接不进来（p79） |
| `OCUDU_LANE_ABLATE_STAGE` | OFF | `lib/phy/metal/ocudu_metal_burst.mm:284` | 10 | **修饰符**（只在 `OCUDU_LANE_ABLATE=1` 时有意义）：只消去哪一**阶段族**——`front_end`/`ce`/`eq`/`demap`（`|` 或 `,` 组合），不设 = `all` = 历史行为；拼错的名字按 `all` 处理并打 WARNING。族级账单靠它，覆盖度看报告里的 `Q9-F5 ablation coverage`（开发文档 6.162） |
| `OCUDU_METAL_GPU_TIME` | OFF | `lib/phy/metal/ocudu_metal_queue.mm:325` | 114 | **探针**：给每条 cb 装 GPU 时间戳（per-label 表的来源；验收腿一直带着它） |
| `OCUDU_UL_PHASE_SEGMENTS` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:1814` | 155 | **探针**：上行相位分段读数（验收腿一直带着它） |
| `OCUDU_UL_TIMING_EVENTS` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:939` | 5 | **探针（新，开发文档 6.240–6.243）**：打印**最慢的 K 次接收等待**与**最迟的 K 次 DL 交接**，各带**宿主墙钟**（`wall=` UTC + `epoch_ms=` + 窗口两端 `began_ms=`/`due_ms=`）、当时的 `load1`，以及**本进程在那个窗口里的 CPU 时间与自愿/非自愿切换增量**（`cpu=`/`ivcsw=`/`nvcsw=`/`base_age=`）—— 用来把 `[ul_rx_wait]` 的尖峰、DL 的迟到和 `.log` 里 `[RF] Real-time failure in RF` 的行对到**同一条时间轴**上。★ 判读只看 **`cpu=` 对窗口**：`cpu ≈ wait` ⇒ 进程一直有 CPU ⇒ 是**电台/USB 侧**晚；`cpu ≈ 0` 或 `ivcsw>0` ⇒ 进程没被调度 ⇒ **宿主调度**。（`load1` 是 60 s 平均，**看不见 10 ms 级事件**，别用它判 —— 纪律 78） |
| `OCUDU_UL_SLOT_TRACE` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:905` | 11 | **探针**：每**时隙**时间线（`=N` = 最多记 N 个时隙，非数字 = 开且用默认上限）。和上面两个一样被两条闸门当「任意值」接受，但它比相位分段宽得多，**验收腿不需要它**——只在追「某个时隙为什么晚」时开（开发文档 6.145⑹⑴；`=64` 曾打出 512 行，见 `ul_pipeline_probe.h` 的注） |
| `OCUDU_DFT_BACKEND` | = "vdsp" | `lib/phy/generic_functions/generic_functions_factories.cpp:195` | 2 | 前端变换的**后端选择**：`=vdsp`（Apple 上**不设就是它**，所以白名单接受）｜`=generic`（**A/B 对照臂**，n78 p170/p171、n1 p172/p173 用它跑 generic 那一侧）。非 Apple 平台根本不编进这条分支，所以这一行的「默认」只在 Apple 上有意义 |
| `OCUDU_DFT_BATCH_SYMBOLS` | AUTO | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:106` | 17 | 前端批量：不设 = `AUTO`（= 一个时隙自己的符号数，n78 上是 **14**）｜`=1` = 每符号对照臂｜`=7`/`=2` 是中间臂。白名单只接受与 AUTO 等价的 `=14` |
| `OCUDU_CE_LANE_ORDER` | ? | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1574` | 1 | 信道估计的四种车道顺序：`merged`（**默认**，估计器的派发搭车道共享 cb）｜`event`｜`wait`/`host_wait`｜`burst`（旧名 `OCUDU_CE_FUSED_BURST`）。拼错的值打 error 并按 `merged` 跑（代码里那条 warning 的原文就写着 "using merged"）。白名单只接受 `=merged` |

> ⚠ **验收腿的旋钮白名单**（`milestone_audit.sh` 的 `kNOB_ANY`/`kNOB_EQ` 与 `leg_gate.sh` 的 `KNOB_ANY`/`KNOB_EQ` **就是它**，两边逐字一致）。
> **任意值**（探针）：`OCUDU_METAL_GPU_TIME`、`OCUDU_UL_PHASE_SEGMENTS`、`OCUDU_UL_SLOT_TRACE`、`OCUDU_UL_TIMING_EVENTS`（2026-10-01 加入：只**打印**最慢的接收等待 / 迟到交接及其宿主墙钟与进程 CPU 增量，不改变任何交付决定；关着不读时钟、不打印，开着最多存 64 条事件 —— 见开发文档 6.240/6.241）。
> **视为「等于交付默认」**：`OCUDU_DFT_BATCH_SYMBOLS=14`、`OCUDU_DFT_OPEN_BLOCK=1`、`OCUDU_DFT_RELEASE_BLOCK=1`、`OCUDU_CE_LANE_ORDER=merged`、`OCUDU_DFT_BACKEND=vdsp`（2026-10-01 加入：Apple 上这就是不设它时的值）。其余一律判 FAIL（**fail-closed**）。
> `OCUDU_DFT_BACKEND=generic` **故意不**在白名单里：那是一条 A/B **臂**——臂可以满足其余所有判据（p84 就是这样），闸门拦的就是它。
> 6.215 起交付车道的网格由 **host** 写，所以 `OCUDU_DFT_BATCH_SYMBOLS`/`OCUDU_DFT_OPEN_BLOCK`/`OCUDU_DFT_RELEASE_BLOCK` 对交付腿是 **MOOT**（那个引擎根本不在路上）；**最有力的交付腿是一个旋钮都不设**，白名单只是给「已经设了」的腿留出等于默认的写法。
> `OCUDU_UL_RX_SYMBOLS` 也不在白名单里，但它的**默认值就是 `= 1`**（每符号一跳）——交付腿不设它即得交付形态，设 `=7`/`=14` 才是改形状。

## 3. 全部旋钮（自动生成）

| 旋钮 | 默认 | 范围 | 首次读取点 | 站点 | 模块 | 腿登记行 | 记录提及(次/文件) | 腿上出现过的值 |
|---|---|---|---|---|---|---|---|---|
| `OCUDU_CE_CACHE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3856` | 1 | `lib/phy/upper/signal_processors` | 1 | 2/1 | 1 |
| `OCUDU_CE_CFO_CARRY_HOST` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:727` | 1 | `lib/phy/upper/signal_processors` | 0 | 14/5 | — |
| `OCUDU_CE_CHAIN_MAP` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4154` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_CORR_BARRIER_AFTER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3998` | 2 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_CORR_CHECK` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:481` | 1 | `lib/phy/upper/signal_processors` | 0 | 5/3 | — |
| `OCUDU_CE_CORR_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:447` | 1 | `lib/phy/upper/signal_processors` | 0 | 43/9 | — |
| `OCUDU_CE_CORR_FENCE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3995` | 1 | `lib/phy/upper/signal_processors` | 0 | 13/1 | — |
| `OCUDU_CE_CORR_FENCED` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:438` | 1 | `lib/phy/upper/signal_processors` | 0 | 22/7 | — |
| `OCUDU_CE_CORR_MERGED` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:653` | 1 | `lib/phy/upper/signal_processors` | 1 | 7/1 | 1 |
| `OCUDU_CE_CORR_SEGMENT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3983` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/2 | — |
| `OCUDU_CE_CORR_STANDALONE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4711` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/2 | — |
| `OCUDU_CE_CORR_UNIFORM` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:676` | 1 | `lib/phy/upper/signal_processors` | 0 | 4/1 | — |
| `OCUDU_CE_CPU_CE` | ? | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:41` | 2 | `lib/phy/upper/channel_processors` | 0 | 37/11 | — |
| `OCUDU_CE_CPU_INVERT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:502` | 1 | `lib/phy/upper/signal_processors` | 0 | 20/8 | — |
| `OCUDU_CE_CPU_LS` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:455` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/16 | — |
| `OCUDU_CE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2241` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_DEV_INVERT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:4558` | 1 | `lib/phy/upper/signal_processors` | 0 | 24/8 | — |
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
| `OCUDU_CE_HOLD_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1259` | 6 | `lib/phy/upper/signal_processors` | 2 | 1/1 | 1 |
| `OCUDU_CE_HOLD_EXTRACTION` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:632` | 1 | `lib/phy/upper/signal_processors` | 0 | 19/7 | — |
| `OCUDU_CE_HOST_GRID` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:699` | 1 | `lib/phy/upper/signal_processors` | 0 | 47/9 | — |
| `OCUDU_CE_HOST_SCALARS` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:613` | 1 | `lib/phy/upper/signal_processors` | 0 | 40/9 | — |
| `OCUDU_CE_HOST_Y_PADS` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:835` | 1 | `lib/phy/upper/signal_processors` | 0 | 9/4 | — |
| `OCUDU_CE_INVERT_FIRST` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3825` | 2 | `lib/phy/upper/signal_processors` | 0 | 17/7 | — |
| `OCUDU_CE_INV_BARRIERS` | = 0 | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:695` | 1 | `lib/phy/upper/signal_processors` | 0 | 13/6 | — |
| `OCUDU_CE_K0A_RATIO_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2203` | 1 | `lib/phy/upper/signal_processors` | 0 | 7/4 | — |
| `OCUDU_CE_K0A_RATIO_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:501` | 1 | `lib/phy/upper/signal_processors` | 0 | 17/11 | — |
| `OCUDU_CE_LANE_ORDER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1574` | 1 | `lib/phy/upper/signal_processors` | 1 | 67/21 | event |
| `OCUDU_CE_LS_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:674` | 1 | `lib/phy/upper/signal_processors` | 0 | 14/6 | — |
| `OCUDU_CE_MATRIX_CACHE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:91` | 1 | `lib/phy/upper/signal_processors` | 2 | 4/1 | 1 |
| `OCUDU_CE_NO_K4` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1906` | 1 | `lib/phy/upper/signal_processors` | 0 | 11/7 | — |
| `OCUDU_CE_NV_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:5149` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/3 | — |
| `OCUDU_CE_NV_OVERRIDE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:5204` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/3 | — |
| `OCUDU_CE_NV_ROUTE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp:316` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/1 | — |
| `OCUDU_CE_PAD_SENTINEL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4568` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_PP_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2086` | 2 | `lib/phy/upper/signal_processors` | 0 | 4/2 | — |
| `OCUDU_CE_PP_PERTURB` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2164` | 2 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_CE_RSRP_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1819` | 5 | `lib/phy/upper/signal_processors` | 0 | 20/9 | — |
| `OCUDU_CE_SIGMA2_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2144` | 1 | `lib/phy/upper/signal_processors` | 0 | 7/3 | — |
| `OCUDU_CE_SPLIT_TAIL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2289` | 1 | `lib/phy/upper/signal_processors` | 0 | 26/9 | — |
| `OCUDU_CE_TAIL_CPU` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2287` | 2 | `lib/phy/upper/signal_processors` | 0 | 6/4 | — |
| `OCUDU_CE_TAIL_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:604` | 1 | `lib/phy/upper/signal_processors` | 0 | 21/8 | — |
| `OCUDU_CE_TAU_RMS_US` | = 0.37e-6 | test | `tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test_factories.cpp:256` | 1 | `tests/integrationtests` | 0 | 0/0 | — |
| `OCUDU_CE_TA_CHAIN` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1129` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/3 | — |
| `OCUDU_CE_TA_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1498` | 7 | `lib/phy/upper/signal_processors` | 0 | 8/4 | — |
| `OCUDU_CE_TA_ONLY` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1125` | 1 | `lib/phy/upper/signal_processors` | 0 | 0/0 | — |
| `OCUDU_CE_WAIT_TRACE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:387` | 3 | `lib/phy/upper/signal_processors` | 0 | 13/6 | — |
| `OCUDU_CE_WEIGHTS_BARRIER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:4062` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_WEIGHTS_TILE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:663` | 1 | `lib/phy/upper/signal_processors` | 2 | 4/1 | 1 |
| `OCUDU_CE_WRAP_MAP` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:605` | 1 | `lib/phy/upper/signal_processors` | 0 | 5/4 | — |
| `OCUDU_CE_Y_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3115` | 1 | `lib/phy/upper/signal_processors` | 0 | 10/3 | — |
| `OCUDU_CE_Y_DIRECT` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3704` | 1 | `lib/phy/upper/signal_processors` | 1 | 14/2 | 0 |
| `OCUDU_CE_Y_HASH` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:5182` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/1 | — |
| `OCUDU_D1_HANDED_BOUND` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:40` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 0 | 7/3 | — |
| `OCUDU_DEMOD_DEFER_ENCODE` | ON | lib | `lib/phy/upper/channel_modulation/metal/demodulation_mapper_metal.cpp:199` | 1 | `lib/phy/upper/channel_modulation` | 0 | 8/2 | — |
| `OCUDU_DFT_BACKEND` | = "vdsp" | lib | `lib/phy/generic_functions/generic_functions_factories.cpp:195` | 1 | `lib/phy/generic_functions/generic_functions_factories.cpp` | 2 | 25/2 | generic |
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
| `OCUDU_INV_MEMNONE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3024` | 1 | `lib/phy/upper/signal_processors` | 0 | 4/3 | — |
| `OCUDU_INV_RL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3028` | 1 | `lib/phy/upper/signal_processors` | 0 | 21/9 | — |
| `OCUDU_INV_SYSTEMS` | = 4 | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1448` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_INV_TGX` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2987` | 1 | `lib/phy/upper/signal_processors` | 0 | 10/8 | — |
| `OCUDU_INV_TGY` | = 16 | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2988` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_L1_CLAIM_ONLY` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:1149` | 1 | `lib/phy/upper/channel_processors` | 0 | 2/2 | — |
| `OCUDU_L1_CONSUMER_SLOT_SKEW` | = 0 | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:725` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_L1_DROP_CONSUMER_WAIT` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:713` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_L1_DROP_MISS_WAIT` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:203` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_L1_HOST_FIRST` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:1134` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_LANE_ABLATE` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:450` | 3 | `lib/phy/metal/ocudu_metal_burst.mm` | 16 | 71/5 | 1 |
| `OCUDU_LANE_ABLATE_EVERY` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:455` | 3 | `lib/phy/metal/ocudu_metal_burst.mm` | 15 | 12/3 | 8 |
| `OCUDU_LANE_ABLATE_STAGE` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:284` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 10 | 19/5 | all,ce,demap,eq,front_end |
| `OCUDU_LANE_DIAG_SPLIT` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:884` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 3 | 29/7 | 1 |
| `OCUDU_METAL_GPU_TIME` | OFF | lib | `lib/phy/metal/ocudu_metal_queue.mm:325` | 2 | `lib/phy/metal/ocudu_metal_queue.mm` | 114 | 130/25 | 1 |
| `OCUDU_MMSE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:412` | 2 | `lib/phy/upper/signal_processors` | 0 | 21/7 | — |
| `OCUDU_PROBE_GAPPED` | ? | test | `lib/phy/upper/channel_processors/metal/test/metal_chain_probe.cpp:268` | 1 | `lib/phy/upper/channel_processors` | 0 | 8/2 | — |
| `OCUDU_PROBE_RE` | = 1272 | test | `lib/phy/upper/channel_processors/metal/test/metal_dispatch_probe.cpp:57` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_PUSCH_DEFERRED_GROUP` | ? | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:413` | 2 | `lib/phy/upper/channel_processors` | 0 | 8/4 | — |
| `OCUDU_PUSCH_FORCE_SERIAL` | OFF | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:303` | 2 | `lib/phy/upper/channel_processors` | 0 | 6/3 | — |
| `OCUDU_REPLAY_TRACE` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:292` | 3 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_UL_DUMP` | ? | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:31` | 2 | `lib/phy/upper/channel_processors` | 0 | 75/12 | — |
| `OCUDU_UL_DUMP_COUNT` | = 8 | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:37` | 1 | `lib/phy/upper/channel_processors` | 0 | 11/6 | — |
| `OCUDU_UL_DUMP_LLR` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:502` | 2 | `lib/phy/upper/channel_processors` | 0 | 7/2 | — |
| `OCUDU_UL_DUMP_MAX_RB` | = 0 | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:52` | 1 | `lib/phy/upper/channel_processors` | 0 | 3/2 | — |
| `OCUDU_UL_DUMP_TD` | ? | lib | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:35` | 2 | `lib/phy/lower/processors` | 0 | 14/7 | — |
| `OCUDU_UL_DUMP_TD_SLOTS` | = 4 | lib | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:45` | 1 | `lib/phy/lower/processors` | 0 | 2/2 | — |
| `OCUDU_UL_PHASE_SEGMENTS` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:1814` | 1 | `include/ocudu` | 155 | 127/23 | 1 |
| `OCUDU_UL_RX_POOL_DROP` | ? | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1190` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 1 | 17/2 | 0 |
| `OCUDU_UL_RX_POOL_DROP_FORCE` | = 0 | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1203` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 1 | 11/3 | 20 |
| `OCUDU_UL_RX_POOL_SIZE` | ? | lib | `lib/phy/lower/lower_phy_factory.cpp:226` | 1 | `lib/phy/lower/lower_phy_factory.cpp` | 1 | 8/4 | 12 |
| `OCUDU_UL_RX_SYMBOLS` | = 1 | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:943` | 2 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 4 | 83/14 | 1,7 |
| `OCUDU_UL_SLOT_TRACE` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:905` | 1 | `include/ocudu` | 11 | 43/14 | 512 |
| `OCUDU_UL_STALE_US` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:1889` | 1 | `include/ocudu` | 0 | 4/3 | — |
| `OCUDU_UL_TIMING_EVENTS` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:939` | 1 | `include/ocudu` | 5 | 21/6 | 16 |
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
