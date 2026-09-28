# OCUDU_* 旋钮清单（**生成物** + 人工判读）

> 生成方式：`python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/phy_latency/knob_inventory.md`
> 本次生成：commit `da268c8df8`。**不要手改正文**——改生成器或改人工判读小节。
> （生成器把**生成那一刻的 HEAD**写进这一行；要把这一行也追平 HEAD，就重跑生成器再提交一次——那一次是纯文档差异。）
>
> **默认值**是**从守卫表达式读出来的**（`ON` = 不设或非 0 都开；`OFF` = 必须显式置 1；`AUTO` = 由别处推导；`?` = 需要读注释）。
> **飞过的腿数**来自 `logs/*.log.stderr` 顶部的 `knob : NAME=VALUE` 登记行 —— 这是**唯一能区分「新仪器」与「已退役」的一列**，源码里两者长得一样。

合计 **115** 个旋钮：**20** 个默认 `ON`（`OCUDU_DFT_RELEASE_TOKENS_EARLY` 于 2026-09-28 由 OFF 改为 ON：**理由 = 输入保持**，见开发文档 6.157；6.156 当初写的"吃掉接收尾巴 60-70×"**已被 6.157 撤回**）（= 交付形态的一部分）；**20** 个有腿登记行、**87** 个只在记录里出现过、**8** 个两处都没有；其中 **19** 个的首个读取点在 `test/`（离线臂）。

## 1. 交付形态的一部分（默认 `ON`）——**验收腿上不许出现「改成 OFF」的值**

| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |
|---|---|---|---|---|
| `OCUDU_CE_CORR_DEV` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:322` | 0 | — |
| `OCUDU_CE_CORR_FENCED` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:313` | 0 | — |
| `OCUDU_CE_DEV_SIGMA2` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:839` | 0 | — |
| `OCUDU_CE_DEV_STATS` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:629` | 0 | — |
| `OCUDU_CE_DEV_TA` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:680` | 0 | — |
| `OCUDU_CE_DEV_Y` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:828` | 0 | — |
| `OCUDU_CE_EDGE_CHECK` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:368` | 0 | — |
| `OCUDU_CE_EDGE_FUSE` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:470` | 0 | — |
| `OCUDU_CE_GPU_INVERT` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:498` | 0 | — |
| `OCUDU_CE_HOLD_EXTRACTION` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:507` | 0 | — |
| `OCUDU_CE_K0A_RATIO_DEV` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:376` | 0 | — |
| `OCUDU_CE_TAIL_DEV` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:479` | 0 | — |
| `OCUDU_CE_Y_DIRECT` | ON | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3588` | 1 | 信道估计直接读 y（省一次 gather） |
| `OCUDU_DEMOD_DEFER_ENCODE` | ON | `lib/phy/upper/channel_modulation/metal/demodulation_mapper_metal.cpp:199` | 0 | 解映射延迟编码（融合车道的分组形状） |
| `OCUDU_DFT_OPEN_BLOCK` | ON | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1162` | 0 | 前端一个时隙的变换合成一条派发（P2-B′，V1 −38.7% 的那一刀） |
| `OCUDU_DFT_RELEASE_TOKENS_EARLY` | ON | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1239` | 1 | P2-E：输入令牌在"最后一个读输入的派发"之后释放（默认开的理由 = 去掉输入保持；**不是接收尾巴的修复** —— 见开发文档 6.157） |
| `OCUDU_EQ_DEFER_ENCODE` | ON | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1852` | 0 | — |
| `OCUDU_EQ_DEV_TABLES` | ON | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:442` | 0 | — |
| `OCUDU_EQ_DIRECT_GRID` | ON | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1296` | 1 | 均衡直接读网格（`y_gather` 消失，派发 10→6/跳） |
| `OCUDU_EQ_GATHER` | ON | `lib/phy/upper/channel_processors/metal/channel_equalizer_metal_factory.cpp:87` | 0 | — |

## 2. 本轮（2026-09-27）新增的仪器（默认 `OFF`）

| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |
|---|---|---|---|---|
| `OCUDU_LANE_ABLATE` | OFF | `lib/phy/metal/ocudu_metal_burst.mm:227` | 6 | 消去法总开关：各阶段的绑定换成 `lane_ablate_noop`（只换 kernel，网格/屏障/提交结构不变） |
| `OCUDU_LANE_ABLATE_EVERY` | ? | `lib/phy/metal/ocudu_metal_burst.mm:232` | 5 | **修饰符**（默认 1 = 每跳都消去；只在 `OCUDU_LANE_ABLATE=1` 时有意义）：`=8` = 每 8 跳消去 1 跳，全消去手机接不进来（p79） |
| `OCUDU_METAL_GPU_TIME` | OFF | `lib/phy/metal/ocudu_metal_queue.mm:325` | 55 | **探针**：给每条 cb 装 GPU 时间戳（per-label 表的来源；验收腿一直带着它） |
| `OCUDU_UL_PHASE_SEGMENTS` | OFF | `include/ocudu/support/executors/ul_pipeline_probe.h:1365` | 99 | **探针**：上行相位分段读数（验收腿一直带着它） |

> ⚠ **验收腿的旋钮白名单**（`milestone_audit.sh` / `leg_gate.sh` 判的就是它）：`OCUDU_METAL_GPU_TIME`、`OCUDU_UL_PHASE_SEGMENTS` 任意值；
> `OCUDU_DFT_BATCH_SYMBOLS=14`、`OCUDU_DFT_OPEN_BLOCK=1`、`OCUDU_DFT_RELEASE_BLOCK=1`、`OCUDU_CE_LANE_ORDER=merged` 视为「等于交付默认」。其余一律判 FAIL（**fail-closed**）。

## 3. 全部旋钮（自动生成）

| 旋钮 | 默认 | 范围 | 首次读取点 | 站点 | 模块 | 腿登记行 | 记录提及(次/文件) | 腿上出现过的值 |
|---|---|---|---|---|---|---|---|---|
| `OCUDU_CE_CFO_CARRY_HOST` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:602` | 1 | `lib/phy/upper/signal_processors` | 0 | 14/5 | — |
| `OCUDU_CE_CHAIN_MAP` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3912` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_CORR_BARRIER_AFTER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3882` | 2 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_CORR_CHECK` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:356` | 1 | `lib/phy/upper/signal_processors` | 0 | 5/3 | — |
| `OCUDU_CE_CORR_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:322` | 1 | `lib/phy/upper/signal_processors` | 0 | 43/9 | — |
| `OCUDU_CE_CORR_FENCE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3879` | 1 | `lib/phy/upper/signal_processors` | 0 | 13/1 | — |
| `OCUDU_CE_CORR_FENCED` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:313` | 1 | `lib/phy/upper/signal_processors` | 0 | 22/7 | — |
| `OCUDU_CE_CORR_SEGMENT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3867` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/2 | — |
| `OCUDU_CE_CORR_STANDALONE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4469` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/2 | — |
| `OCUDU_CE_CORR_UNIFORM` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:643` | 1 | `lib/phy/upper/signal_processors` | 0 | 4/1 | — |
| `OCUDU_CE_CPU_CE` | ? | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:41` | 2 | `lib/phy/upper/channel_processors` | 0 | 37/11 | — |
| `OCUDU_CE_CPU_INVERT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:502` | 1 | `lib/phy/upper/signal_processors` | 0 | 20/8 | — |
| `OCUDU_CE_CPU_LS` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:330` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/16 | — |
| `OCUDU_CE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2067` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_DEV_INVERT` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:4406` | 1 | `lib/phy/upper/signal_processors` | 0 | 24/8 | — |
| `OCUDU_CE_DEV_SIGMA2` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:839` | 1 | `lib/phy/upper/signal_processors` | 0 | 33/10 | — |
| `OCUDU_CE_DEV_STATS` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:629` | 1 | `lib/phy/upper/signal_processors` | 0 | 17/7 | — |
| `OCUDU_CE_DEV_TA` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:680` | 1 | `lib/phy/upper/signal_processors` | 0 | 15/7 | — |
| `OCUDU_CE_DEV_Y` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:828` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/11 | — |
| `OCUDU_CE_DFT_TIME` | OFF | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:693` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_CE_EDGE_CHECK` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:368` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/5 | — |
| `OCUDU_CE_EDGE_DIAG` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:2651` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_CE_EDGE_FUSE` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:470` | 1 | `lib/phy/upper/signal_processors` | 0 | 36/13 | — |
| `OCUDU_CE_FD_HZ` | ? | test | `tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test_factories.cpp:260` | 1 | `tests/integrationtests` | 0 | 0/0 | — |
| `OCUDU_CE_FUSED_BURST` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1464` | 1 | `lib/phy/upper/signal_processors` | 0 | 20/7 | — |
| `OCUDU_CE_GPU_INVERT` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.h:498` | 1 | `lib/phy/upper/signal_processors` | 0 | 37/11 | — |
| `OCUDU_CE_HOLD_EXTRACTION` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:507` | 1 | `lib/phy/upper/signal_processors` | 0 | 19/7 | — |
| `OCUDU_CE_HOST_GRID` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:574` | 1 | `lib/phy/upper/signal_processors` | 0 | 47/9 | — |
| `OCUDU_CE_HOST_SCALARS` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:488` | 1 | `lib/phy/upper/signal_processors` | 0 | 40/9 | — |
| `OCUDU_CE_HOST_Y_PADS` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:710` | 1 | `lib/phy/upper/signal_processors` | 0 | 9/4 | — |
| `OCUDU_CE_INVERT_FIRST` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:3651` | 2 | `lib/phy/upper/signal_processors` | 0 | 17/7 | — |
| `OCUDU_CE_INV_BARRIERS` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:662` | 1 | `lib/phy/upper/signal_processors` | 0 | 13/6 | — |
| `OCUDU_CE_K0A_RATIO_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2029` | 1 | `lib/phy/upper/signal_processors` | 0 | 7/4 | — |
| `OCUDU_CE_K0A_RATIO_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:376` | 1 | `lib/phy/upper/signal_processors` | 0 | 16/10 | — |
| `OCUDU_CE_LANE_ORDER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1437` | 1 | `lib/phy/upper/signal_processors` | 1 | 66/21 | event |
| `OCUDU_CE_LS_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:549` | 1 | `lib/phy/upper/signal_processors` | 0 | 14/6 | — |
| `OCUDU_CE_NO_K4` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1863` | 1 | `lib/phy/upper/signal_processors` | 0 | 11/7 | — |
| `OCUDU_CE_NV_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4907` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/3 | — |
| `OCUDU_CE_NV_OVERRIDE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4962` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/3 | — |
| `OCUDU_CE_NV_ROUTE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp:316` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/1 | — |
| `OCUDU_CE_PAD_SENTINEL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4326` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_PP_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1917` | 2 | `lib/phy/upper/signal_processors` | 0 | 4/2 | — |
| `OCUDU_CE_PP_PERTURB` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1990` | 2 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_CE_RSRP_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1776` | 5 | `lib/phy/upper/signal_processors` | 0 | 20/9 | — |
| `OCUDU_CE_SIGMA2_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:1975` | 1 | `lib/phy/upper/signal_processors` | 0 | 7/3 | — |
| `OCUDU_CE_SPLIT_TAIL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2115` | 1 | `lib/phy/upper/signal_processors` | 0 | 26/9 | — |
| `OCUDU_CE_TAIL_CPU` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2113` | 2 | `lib/phy/upper/signal_processors` | 0 | 6/4 | — |
| `OCUDU_CE_TAIL_DEV` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:479` | 1 | `lib/phy/upper/signal_processors` | 0 | 21/8 | — |
| `OCUDU_CE_TAU_RMS_US` | ? | test | `tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test_factories.cpp:256` | 1 | `tests/integrationtests` | 0 | 0/0 | — |
| `OCUDU_CE_TA_CHAIN` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1129` | 1 | `lib/phy/upper/signal_processors` | 0 | 6/3 | — |
| `OCUDU_CE_TA_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:1455` | 7 | `lib/phy/upper/signal_processors` | 0 | 8/4 | — |
| `OCUDU_CE_TA_ONLY` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1125` | 1 | `lib/phy/upper/signal_processors` | 0 | 0/0 | — |
| `OCUDU_CE_WAIT_TRACE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:379` | 3 | `lib/phy/upper/signal_processors` | 0 | 13/6 | — |
| `OCUDU_CE_WEIGHTS_BARRIER` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3946` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_CE_WRAP_MAP` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:592` | 1 | `lib/phy/upper/signal_processors` | 0 | 5/4 | — |
| `OCUDU_CE_Y_CHECK` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:2941` | 1 | `lib/phy/upper/signal_processors` | 0 | 10/3 | — |
| `OCUDU_CE_Y_DIRECT` | ON | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:3588` | 1 | `lib/phy/upper/signal_processors` | 1 | 14/2 | 0 |
| `OCUDU_CE_Y_HASH` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp:4940` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/1 | — |
| `OCUDU_D1_HANDED_BOUND` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:40` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 0 | 7/3 | — |
| `OCUDU_DEMOD_DEFER_ENCODE` | ON | lib | `lib/phy/upper/channel_modulation/metal/demodulation_mapper_metal.cpp:199` | 1 | `lib/phy/upper/channel_modulation` | 0 | 8/2 | — |
| `OCUDU_DFT_BACKEND_QUEUE` | OFF | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1754` | 1 | `lib/phy/generic_functions/metal` | 1 | 13/5 | 1 |
| `OCUDU_DFT_BATCH_SYMBOLS` | AUTO | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:106` | 1 | `lib/phy/generic_functions/metal` | 17 | 34/5 | 1,14,2,7 |
| `OCUDU_DFT_GRID_AUDIT` | OFF | lib | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:166` | 2 | `lib/phy/lower/modulation` | 2 | 2/2 | 1 |
| `OCUDU_DFT_OPEN_BLOCK` | ON | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1162` | 2 | `lib/phy/generic_functions/metal` | 0 | 32/12 | — |
| `OCUDU_DFT_PIPELINE_DEPTH` | ? | lib | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:577` | 1 | `lib/phy/lower/modulation` | 0 | 18/6 | — |
| `OCUDU_DFT_RELEASE_BLOCK` | OFF | include | `include/ocudu/phy/phy_pipeline_grid_ready.h:293` | 1 | `include/ocudu` | 6 | 62/11 | 0 |
| `OCUDU_DFT_RELEASE_TOKENS_EARLY` | ON | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1239` | 1 | `lib/phy/generic_functions/metal` | 1 | 15/6 | 1 |
| `OCUDU_DFT_STAGE_INPUT` | OFF | lib | `lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm:1563` | 2 | `lib/phy/generic_functions/metal` | 3 | 16/4 | 1 |
| `OCUDU_DFT_WAIT_PER_SLOT` | OFF | lib | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:159` | 1 | `lib/phy/lower/modulation` | 3 | 5/2 | 1 |
| `OCUDU_EQ_DEFER_ENCODE` | ON | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1852` | 5 | `lib/phy/upper/channel_processors` | 0 | 37/11 | — |
| `OCUDU_EQ_DEV_TABLES` | ON | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:442` | 1 | `lib/phy/upper/channel_processors` | 0 | 4/3 | — |
| `OCUDU_EQ_DIRECT_GRID` | ON | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1296` | 1 | `lib/phy/upper/channel_processors` | 1 | 6/1 | 0 |
| `OCUDU_EQ_GATHER` | ON | lib | `lib/phy/upper/channel_processors/metal/channel_equalizer_metal_factory.cpp:87` | 1 | `lib/phy/upper/channel_processors` | 0 | 18/2 | — |
| `OCUDU_EQ_TABLE_CHECK` | ? | lib | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:1112` | 1 | `lib/phy/upper/channel_processors` | 0 | 5/3 | — |
| `OCUDU_GPU_STRICT` | ? | include | `include/ocudu/phy/phy_pipeline_strict.h:31` | 1 | `include/ocudu` | 0 | 12/7 | — |
| `OCUDU_HANDOFF_ADDR` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:155` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HANDOFF_DM_DEFER` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:233` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HANDOFF_EQ_DEFER` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:232` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HANDOFF_WIDE` | ? | test | `lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp:231` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_HELENA_DUMP_DIR` | OFF | lib | `lib/phy/upper/channel_processors/pusch/pusch_decoder_impl.cpp:505` | 5 | `lib/phy/upper/channel_processors` | 0 | 7/3 | — |
| `OCUDU_HELENA_FORCE_NN` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_helena_impl.cpp:143` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_INV_MEMNONE` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2908` | 1 | `lib/phy/upper/signal_processors` | 0 | 4/3 | — |
| `OCUDU_INV_RL` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2912` | 1 | `lib/phy/upper/signal_processors` | 0 | 21/9 | — |
| `OCUDU_INV_SYSTEMS` | ? | test | `lib/phy/upper/signal_processors/channel_estimator/metal/test/port_channel_estimator_metal_mmse_unit_test.cpp:1448` | 1 | `lib/phy/upper/signal_processors` | 0 | 1/1 | — |
| `OCUDU_INV_TGX` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2871` | 1 | `lib/phy/upper/signal_processors` | 0 | 10/8 | — |
| `OCUDU_INV_TGY` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:2872` | 1 | `lib/phy/upper/signal_processors` | 0 | 2/2 | — |
| `OCUDU_L1_CLAIM_ONLY` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:1149` | 1 | `lib/phy/upper/channel_processors` | 0 | 2/2 | — |
| `OCUDU_L1_CONSUMER_SLOT_SKEW` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:725` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_L1_DROP_CONSUMER_WAIT` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:713` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_L1_DROP_MISS_WAIT` | OFF | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:195` | 1 | `lib/phy/upper/signal_processors` | 0 | 3/2 | — |
| `OCUDU_L1_HOST_FIRST` | OFF | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:1134` | 1 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_LANE_ABLATE` | OFF | lib | `lib/phy/metal/ocudu_metal_burst.mm:227` | 2 | `lib/phy/metal/ocudu_metal_burst.mm` | 6 | 37/4 | 1 |
| `OCUDU_LANE_ABLATE_EVERY` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:232` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 5 | 8/3 | 8 |
| `OCUDU_LANE_DIAG_SPLIT` | ? | lib | `lib/phy/metal/ocudu_metal_burst.mm:594` | 1 | `lib/phy/metal/ocudu_metal_burst.mm` | 3 | 29/7 | 1 |
| `OCUDU_METAL_GPU_TIME` | OFF | lib | `lib/phy/metal/ocudu_metal_queue.mm:325` | 2 | `lib/phy/metal/ocudu_metal_queue.mm` | 55 | 88/21 | 1 |
| `OCUDU_MMSE_DEBUG` | ? | lib | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm:404` | 2 | `lib/phy/upper/signal_processors` | 0 | 21/7 | — |
| `OCUDU_PROBE_GAPPED` | ? | test | `lib/phy/upper/channel_processors/metal/test/metal_chain_probe.cpp:268` | 1 | `lib/phy/upper/channel_processors` | 0 | 8/2 | — |
| `OCUDU_PROBE_RE` | ? | test | `lib/phy/upper/channel_processors/metal/test/metal_dispatch_probe.cpp:57` | 1 | `lib/phy/upper/channel_processors` | 0 | 0/0 | — |
| `OCUDU_PUSCH_DEFERRED_GROUP` | ? | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:413` | 2 | `lib/phy/upper/channel_processors` | 0 | 8/4 | — |
| `OCUDU_PUSCH_FORCE_SERIAL` | OFF | lib | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:303` | 2 | `lib/phy/upper/channel_processors` | 0 | 6/3 | — |
| `OCUDU_REPLAY_TRACE` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:292` | 3 | `lib/phy/upper/channel_processors` | 0 | 1/1 | — |
| `OCUDU_UL_DUMP` | ? | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:31` | 2 | `lib/phy/upper/channel_processors` | 0 | 73/11 | — |
| `OCUDU_UL_DUMP_COUNT` | ? | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:37` | 1 | `lib/phy/upper/channel_processors` | 0 | 10/5 | — |
| `OCUDU_UL_DUMP_LLR` | ? | test | `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp:502` | 2 | `lib/phy/upper/channel_processors` | 0 | 7/2 | — |
| `OCUDU_UL_DUMP_MAX_RB` | ? | lib | `lib/phy/upper/channel_processors/pusch/ul_capture.cpp:52` | 1 | `lib/phy/upper/channel_processors` | 0 | 3/2 | — |
| `OCUDU_UL_DUMP_TD` | ? | lib | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:35` | 2 | `lib/phy/lower/processors` | 0 | 14/7 | — |
| `OCUDU_UL_DUMP_TD_SLOTS` | ? | lib | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:45` | 1 | `lib/phy/lower/processors` | 0 | 2/2 | — |
| `OCUDU_UL_PHASE_SEGMENTS` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:1365` | 1 | `include/ocudu` | 99 | 95/18 | 1 |
| `OCUDU_UL_RX_POOL_DROP` | ? | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1048` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 1 | 17/2 | 0 |
| `OCUDU_UL_RX_POOL_DROP_FORCE` | ? | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:1061` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 1 | 11/3 | 20 |
| `OCUDU_UL_RX_POOL_SIZE` | ? | lib | `lib/phy/lower/lower_phy_factory.cpp:226` | 1 | `lib/phy/lower/lower_phy_factory.cpp` | 1 | 8/4 | 12 |
| `OCUDU_UL_RX_SYMBOLS` | ? | lib | `lib/phy/lower/lower_phy_baseband_processor.cpp:841` | 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp` | 2 | 53/13 | 1,7 |
| `OCUDU_UL_SLOT_TRACE` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:703` | 1 | `include/ocudu` | 7 | 38/13 | 512 |
| `OCUDU_UL_STALE_US` | OFF | include | `include/ocudu/support/executors/ul_pipeline_probe.h:1440` | 1 | `include/ocudu` | 0 | 4/3 | — |
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
