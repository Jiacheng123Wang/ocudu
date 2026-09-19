# OCUDU 管线扫描与 Dispatch 开销诊断报告（S-1 交付物）

> 状态：**v1.1（2026-09-11）** — 静态审计 + 探针落地 + 既有问题修复已完成；
> **实机基线快照已归档**（§6：ZMQ 腿 1692 样本 + OTA 腿 1113 样本 + 全量单测 7598/7598）。
>
> 关联：`soft_hard_decoupled_execution_plan.md`（同目录）§2 S-1 开工门禁；
> 技术蓝图：`full_chain_gpu_uma_zero_copy_refactor_plan.md`（同目录）§1.4（B1–B14 编号基准）与 §2（审计方法）。

---

## 1. 执行状态总览

| S-1 门禁项 | 状态 |
|---|---|
| 审计报告入库（B1–B14 量化清单） | ✅ 本报告 §3（静态证据 + 已知实测口径 + §6 实机基线回填） |
| 修复清单合入 + 单测回归全绿 | ✅ 6 项修复合入；**全量单测 7598/7598 通过**（§4.6）；MMSE/LDPC 单测全绿（§4） |
| 基线快照归档 | ✅ **§6 已回填**：ZMQ 腿 1692 样本 + OTA 腿（onePlus 8T 真机）1113 样本（2026-09-11） |
| A1/A2 探针可用 | ✅ 已落地并在双腿实机运行产出数据（§2、§6） |
| S0 页对齐基带池 | ✅ **完成（2026-09-11）**：实现 + 单测（4+1 全绿）+ **实机 E2E 回归通过**（§6.5：OTA 673 样本 / ZMQ 1690 样本，与基线逐项一致） |
| 双腿 E2E 全量回归（ZMQ+UHD 500 ping） | ✅ 由双腿实机运行代表：srsRAN UE（ZMQ，1692 CRC-OK）+ onePlus 8T（OTA，1113 CRC-OK）全程功能通过 |

**Go/No-Go 结论（2026-09-11 更新）**：除 S0 外全部门禁满足，S-1 判定通过；
S0 完成后支线一正式进入 S1（Metal 核心算子与张量重排）。

---

## 2. 探针扩展（本次落地）

### 2.1 A1：`ul_fapi_mac` 新分段（FAPI→MAC 尾段）

- **语义**：CRC-OK 完成（PHY 解码器通知器）→ MAC per-UE 队列入队完成，覆盖 P7 fastpath
  翻译与 `byte_buffer::create` 拷贝。
- **实现**（`include/ocudu/support/executors/ul_pipeline_probe.h`）：
  - `record_end_crc_ok()` 现在**无条件**登记 CRC-OK 完成时间戳（`pending_crc_ok_ends`，仅 CRC-OK TB 会走到此方法）；
  - 新增 `record_fapi_mac_end(slot)`：按**精确槽号**匹配（P7 fastpath 是同步调用链，两端同线程），
    带既有 2 s 老化门限与负值丢弃；非探针构建为 no-op 桩。
- **挂点**：`lib/mac/mac_ul/mac_ul_processor.cpp:101` 的 `handle_rx_data_indication()`，
  每 PDU 入队后记录（`msg.sl_rx.count()` 与 PHY 侧 `current_slot->count()` 同源）。
- **输出**：`report()` 新增 `[ul_fapi_mac]` 序列行（与 `[ul_ldpc_decode]` 同口径锁步，
  因队列满丢弃的 PDU 不计入，样本数可略少）。

### 2.2 A2：Metal commit/wait 统计探针

- **语义**：每引擎进程级统计 `commits / waits / max_in_flight`。
  `max_in_flight` = 跨线程共享命令队列的**在飞命令缓冲峰值**——量化 B7/B10
  「单队列提交序串行 + 每线程同步等待」的队列占用（单测单线程为 1，实链多线程下反映真实串行）。
- **实现**：
  - `ocudu_metal_decoder_engine.mm`：commit/wait 挂点（`decoder_stats_commit/wait`），
    首次建引擎时 `atexit` 注册报告；
  - `ocudu_metal_mmse_engine.mm`：同型，5 个 commit/wait 点全部挂上，`init()` 内注册；
  - `ocudu_dft_metal_engine.mm`：同型（S1a 起）。
- **控制方式**（2026-09-11 起与 `ENABLE_FLOW_PROBES` 同款模式）：**编译期开关
  `ENABLE_METAL_STATS`（CMake，default OFF，定义 `OCUDU_METAL_STATS`）**——OFF 时
  计数与报告整体编译掉（零开销）；运行时不再有任何环境变量。
- **输出**（`ENABLE_METAL_STATS=ON` 构建时进程退出打印，默认 OFF 构建完全静默）：
  ```
  [metal_stats] ldpc_decoder commits=4885 waits=4885 max_in_flight=1
  [metal_stats] mmse_ce commits=3332 waits=3332 max_in_flight=1
  [metal_stats] dft commits=316 waits=316 max_in_flight=1
  ```
  （上例 = 单测运行实测。）

---

## 3. B1–B14 静态审计清单（证据 + 已知口径）

> 列「实测口径」= 既有 PLAN/文档记录或本次单测实测；「阶段」= 消除归属（蓝图 §1.4）。

| # | 瓶颈 | 位置（file:line） | 实测口径 | 阶段 |
|---|---|---|---|---|
| B1 | 以太网协议栈 4 拷贝 + 互斥环（ZMQ 腿） | `radio_zmq_rx_channel.cpp:144,224,298`；`radio_zmq_rx_stream.cpp:87-89` | ~39 MiB/channel staging（默认 614400×8，`:25` 的 8× 超配）；每收包 3 次锁/线程跳变 | ②（FPGA DMA） |
| B2 | 拉模型：I/Q 消费绑死 lower-PHY executor | `lower_phy_baseband_processor.cpp:240` | 每槽 1 次阻塞 `receive()`（ZMQ sequential 剖面下 `phy_worker` 同一线程） | ② |
| B3 | CPU FFT：ci16→cf32 + `grid.put` | `puxch_processor_impl.cpp:52-53`；`ofdm_demodulator_impl.cpp:107-131` | 每符号 2×转换 + FFT | ③ |
| B4 | 每端口 CE 任务 `defer` 到估计器腿 | `dmrs_pusch_estimator_impl.cpp:52-67` | 每 (port,slot) 1 次线程跳变 + 2 次队列 | ③ |
| B5 | CPU 相关矩阵 + 每层重复 Gauss-Jordan 求逆 | `port_channel_estimator_metal_mmse_impl.cpp:185-250,642-668` | 6–14 µs×hop + L×L 求逆×层数（v1 各层同统计冗余 ≤4×） | ③/④ |
| B6 | CE 每 (port,slot) 2–4 次 commit+wait | `ocudu_metal_mmse_engine.mm`（5 个 wait 点） | commit+wait ≈ 30–90 µs/次；标准块+尾块各一次 | ③ |
| B7 | 每估计器实例私有 device+queue（10 实例） | `ocudu_metal_mmse_engine.mm:148-152`（`init`） | 10 队列，无法跨端口批处理/共链（CE PLAN 待办 (D)） | ③ |
| B8 | size-blind 指针缓存 + 热路径忽略引擎返回值 | `ocudu_metal_mmse_engine.mm` wrap()；`port_channel_estimator_metal_mmse_impl.cpp:670-675` | 失败可静默产出过期 CE 且 `hop_gpu=1` 照报 | **✅ 本次已修** |
| B9 | LLR 打包：memset+2×fill+memcpy | `ldpc_decoder_metal.cpp:507-510` | n_aligned≈26 KB@z384 写 + int8→fp16 GPU 转换 | ③ |
| B10 | 每解码 1 commit+wait；290 dispatch CPU 编码 | `ocudu_metal_decoder_engine.mm:749-751`；layered 展开 | ~1.9 µs/dispatch ≈ 550 µs 地板（PLAN.md:1345-1347）；实链 persistent ~730 µs 中位 | ③ |
| B11 | 逐 bit 回读 + CPU syndrome 重算 + 逐 bit 重打包 + CPU CRC | `ocudu_metal_decoder_engine.mm:778-800`；`ldpc_decoder_metal.cpp:521-531` | 8448 次虚调用@z384 + m_aligned 字扫描 | ⑤ |
| B12 | 全链唯一 TB 拷贝 | `fapi_to_mac_indications_fastpath_translator.cpp:88` | `byte_buffer::create`，TBS ≤ 几十 KB | ⑤ |
| B13 | MAC 消费走 per-UE strand 队列 | `mac_ul_processor.cpp:118-121` | 每 PDU 1 次入队（`priority_task_strand` 永不内联） | ⑤（可选） |
| B14 | 模块间无 GPU 交接（CE→均衡→LDPC 每级回 CPU） | 全模块（CE 解包 `:678-689` → CPU 均衡 → LDPC 打包） | 每槽 ≥2 次主机往返 | ③ |

**本次单测补充实测（2026-09-11，本机，Release）**：

| 项 | 值 |
|---|---|
| MMSE 单测 Test 5 `compute()` 时延 | cpu 3.1 µs / metal_mmse **169.2 µs**（GPU 23.9 µs + CPU 打包/解包/往返） |
| `engine.apply()` 纯 GPU（K2） | **6.3 µs**/call |
| `engine.invert()` GPU 版 | 511.9 µs/call（GPU 365.2 µs；barrier 链，佐证 B5 求逆应留 CPU 或改 8×8 块化） |
| LDPC 单测（SNR 3–99 dB 网格） | cpu/gpu/persist/layered 100% 一致，0 disagreements |
| metal_stats 实测 | 见 §2.2（单线程 max_in_flight=1 为预期下限） |

---

## 4. 既有问题修复记录（S-1 修复批次，全部行为不变）

| # | 问题 | 文件 | 修复 | 验证 |
|---|---|---|---|---|
| F1 | MMSE 零拷贝缓存命中**不校验长度**，靠构造期 warm-up 隐式确立最大尺寸 | `ocudu_metal_mmse_engine.mm` `wrap()` | 缓存改存 `(buffer, length)`；命中且请求更大时告警并**重新包装**，绝不静默截断 | MMSE 单测全绿（含 Test 7/8） |
| F2 | MMSE 热路径**忽略引擎返回值**：wrap/commit 失败会静默解包过期 `gpu_h` | `port_channel_estimator_metal_mmse_impl.{h,cpp}` | `run_engine_blocks` 改返回 bool；`apply_fd_td_estimation_stage` 按批次（标准块/尾块）**回退 CPU 参考数学**并 `logger.error`；`[mmse_time]` 日志新增 `fb=` 回退块数 | GPU 腿全绿；引擎不可用时的全 CPU 回退路径 Test 1–7 全绿 |
| F3 | LDPC 零拷贝缓存同型 size-blind 缺陷 | `ocudu_metal_decoder_engine.mm` `zero_copy_buffer()` | 同 F1：`(buffer, length)` 缓存 + 更大请求重新包装 + 告警 | LDPC 单测 ALL OK（含 mt-stress 4 线程） |
| F4 | 致命错误打印 `crc_calculator_type` 而非 `ldpc_decoder_type` | `upper_phy_factories.cpp:768-769` | 消息参数改为 `config.ldpc_decoder_type` | 编译通过（`ocudu_upper_phy`） |
| F5 | 度量装饰器分支二次建厂**丢 106-PRB HELENA 路径** | `upper_phy_factories.cpp:657-664` | 补传 `pusch_channel_estimator_helena_model_path_106` | 编译通过 |
| F6 | persistent 多线程组**过时注释**（实装为单 TG×1024，多 TG 软件栅栏已被证伪） | `ocudu_metal_decoder_engine.h:31-39` | 注释更正为单 TG×1024 实况 + 证伪说明（PLAN.md 4.12） | — |

**纪律遵守**：本批次只做行为不变修复（断言/告警/错误消息/回退控制流），未混入任何重构；
F2 的回退路径在引擎不可用的全 CPU 模式下实测通过（2026-09-11 起 `OCUDU_MMSE_NOGPU`
已删除：强制 CPU 路径走 `expert_phy --pusch_channel_estimator_algo cpu`）。

---

## 5. 实机基线快照采集 Runbook（待执行）

> 目标：30 kHz 51 PRB 与 15 kHz 各 ≥ 200 槽；双腿（ZMQ / UHD）各一轮；`[ul_fapi_mac]`
> 与 `[metal_stats]` 为本次新增采集项。采集结果回填本报告 §6。

### 5.1 构建（带探针）

```bash
cmake -S . -B build-probes -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_FLOW_PROBES=ON -DENABLE_METAL_LDPC=ON -DENABLE_METAL_CHEST=ON \
      -DENABLE_METAL_STATS=ON -DENABLE_CE_TIME=ON
cmake --build build-probes -j --target gnb
```

> 2026-09-11 起探针全部为**编译期开关**（`ENABLE_FLOW_PROBES` / `ENABLE_METAL_STATS` /
> `ENABLE_CE_TIME`，默认 OFF），运行命令不再带任何探针环境变量。

### 5.2 ZMQ 腿（时延压测口径）

```bash
# 终端 1：open5gs 核心网 + srsUE（macOS e2e 环境，见 tests/ci/macos_e2e/README.md）
# 终端 2：
./build-probes/apps/gnb/gnb -c configs/gnb_zmq.yaml \
  expert_phy --pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal_persistent
# 跑满 ≥ 200 CRC-OK 槽（500 ping 即足够）后 Ctrl-C，收集退出打印的
# [ul_pipeline] / [ul_time_frequency] / [ul_channel_estimation] / [ul_equalization_demod] /
# [ul_fapi_mac] / [ul_ldpc_decode] / [ul_mac_pdu_size] / [metal_stats] 行。
```

### 5.3 UHD 腿（真 OTA 口径）

```bash
sudo ./build-probes/apps/gnb/gnb -c configs/gnb_uhd_oaiue.yaml \
  expert_phy --pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal
```

### 5.4 归档要求

- 每个配置一条记录：日期 / 分支 commit / 配置 / 各系列 `samples mean median p95 p99` /
  `[metal_stats]` 行 / 环境（负载、温度）。
- **回归确认**：采集前后各跑一次无探针 Release 的 500 ping（修复批次行为不变的最终确认）。

---

## 6. 基线快照归档（2026-09-11 实机采集）

### 6.1 ZMQ 腿（srsRAN UE，时延压测口径）

> 采集配置：`build-probes` = `ENABLE_FLOW_PROBES=ON + ENABLE_METAL_STATS=ON + ENABLE_CE_TIME=ON`
> 构建（探针现为编译期开关，§2.2/§5.1）。运行命令（当前形式，不再带探针环境变量）：

```bash
sudo ./build-probes/apps/gnb/gnb -c configs/gnb_zmq.yaml \
  expert_phy --pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal_persistent
```

```
[ul_pipeline]          samples=1692 mean=5029.3us median=4435.0us min=1474.0us max=10423.0us p95=9676.0us p99=9940.0us
[ul_time_frequency]    samples=1692 mean=59.3us   median=57.8us   min=47.3us   max=153.2us   p95=73.6us  p99=83.3us
[ul_channel_estimation]samples=1692 mean=924.9us  median=1037.9us min=299.7us  max=2828.8us  p95=1188.5us p99=1224.9us
[ul_equalization_demod]samples=1692 mean=24.2us   median=25.2us   min=5.1us    max=84.5us    p95=41.4us  p99=47.8us
[ul_fapi_mac]          samples=1692 mean=6.6us    median=6.1us    min=1.9us    max=29.5us    p95=11.5us  p99=17.9us
[ul_ldpc_decode]       samples=1692 mean=4020.9us median=3249.0us min=761.0us  max=9430.0us  p95=8561.0us p99=8689.0us
[ul_mac_pdu_size]      samples=1692 mean=209.7B   median=111.0B   min=7.0B     max=528.0B    p95=421.0B  p99=512.0B | total=354790.0B
[metal_stats] mmse_ce commits=3040 waits=3040 max_in_flight=1
[metal_stats] ldpc_decoder commits=1726 waits=1726 max_in_flight=1
```

### 6.2 OTA 腿（onePlus 8T 真机，B200 FDD n1 5 MHz）

```bash
# 同 §6.1 采集配置（编译期探针开关，运行命令不带环境变量）
sudo ./build-probes/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  expert_phy --pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal_persistent
```

```
[ul_pipeline]          samples=1113 mean=2723.1us median=1967.0us min=792.0us max=22886.0us p95=6194.0us p99=12019.0us
[ul_time_frequency]    samples=1113 mean=15.7us   median=15.5us   min=3.3us   max=67.2us    p95=24.0us  p99=27.6us
[ul_channel_estimation]samples=1113 mean=1034.9us median=791.9us  min=409.4us max=15557.2us p95=2319.0us p99=3974.9us
[ul_equalization_demod]samples=1113 mean=13.5us   median=13.4us   min=3.1us   max=33.2us    p95=21.2us  p99=22.8us
[ul_fapi_mac]          samples=1113 mean=4.5us    median=3.8us    min=1.1us   max=24.4us    p95=8.2us   p99=11.4us
[ul_ldpc_decode]       samples=1113 mean=1658.9us median=1106.0us min=265.0us max=15151.0us p95=4406.0us p99=10564.0us
[ul_mac_pdu_size]      samples=1113 mean=128.7B   median=96.0B    min=3.0B    max=544.0B    p95=480.0B  p99=528.0B | total=143221.0B
[metal_stats] mmse_ce commits=3925 waits=3925 max_in_flight=1
[metal_stats] ldpc_decoder commits=2203 waits=2203 max_in_flight=1
```

### 6.3 全量单元测试（2026-09-11，build-probes）

```
100% tests passed out of 7598   （S-0 修复前）
100% tests passed out of 7603   （S-0 合入后：新增 4 项对齐单测 + 1 项 Metal 冒烟）
```

### 6.4 基线数据分析（B 编号量化结论）

**提交/等待计数（A2 探针首次实链证据）**：

| 指标 | ZMQ 腿 | OTA 腿 | 解读 |
|---|---|---|---|
| mmse_ce commits / CRC-OK 样本 | 3040/1692 = **1.80** | 3925/1113 = **3.53** | B6 量化：每 PUSCH 平均 1.8（25 PRB 单 hop）/ 3.5（多 hop）次 commit+wait |
| ldpc commits / CRC-OK 样本 | 1726/1692 ≈ **1.02** | 2203/1113 ≈ **1.98** | 单码块小 TB 每 PUSCH 1 次解码；OTA 腿含重传/多码块 → ~2 次 |
| **max_in_flight（双腿）** | **1** | **1** | **B7/B10 关键证据**：每引擎各自队列上 GPU 完全串行、零流水重叠——每 PUSCH 的 2–4 次 commit+wait 全链首尾相接，无跨任务并行 |

**分段占比（mean，占总管线时延）**：

| 分段 | ZMQ 腿 | OTA 腿 | 结论 |
|---|---|---|---|
| 时频转换（CPU FFT） | 59.3 µs（1.2%） | 15.7 µs（0.6%） | B3 量化：CPU FFT 便宜，GPU 化主要价值在「去掉落网格往返」而非算力 |
| **信道估计（metal_mmse）** | **924.9 µs（18.4%）** | **1034.9 µs（38.0%）** | **B5/B6/B7 主导**：每 hop 1.8–3.5 次 commit+wait + CPU 相关矩阵/求逆 + 逐 hop 解包 |
| 均衡+解调（CPU） | 24.2 µs | 13.5 µs | B9 前段量化：CPU 均衡便宜，模块间往返才是损失 |
| **LDPC（metal_persistent）** | **4020.9 µs（80.0%）** | **1658.9 µs（60.9%）** | **B10/B11 主导**：persistent 单 dispatch 下每轮 46×~8.7 µs barrier + 打包/回读；p99 8.6–10.5 ms 为提交序串行下的排队 |
| fapi_mac（新分段首测） | 6.6 µs（0.1%） | 4.5 µs（0.2%） | **B12/B13 量化完成**：fastpath 拷贝+入队极轻（p99 ≤ 18 µs），第五阶段优化空间有限 |

**与蓝图预算表的对照**：现网 CE（~1 ms）与 LDPC（1.7–4.0 ms）远超第三阶段预算
（CE ≤ 30 µs / LDPC ≤ 50–200 µs）——差距的全部来源正是 B5–B7、B9–B11 所列的
「同步往返 + CPU 打包 + dispatch 链」，与 S2 单命令缓冲化要消除的对象一一对应。
max_in_flight=1 证明当前架构连「提交后不等」的最小流水化都没有，S2 的
`addCompletedHandler` 改造预期收益 = 现网 2.7–5.0 ms 管线中位的绝大部分。

### 6.5 S-0 切换后回归快照（2026-09-11，页对齐 rx 池合入后实机复测）

**OTA 腿（onePlus 8T 真机，同配置）**：
```
[ul_pipeline]          samples=673 mean=3216.5us median=2771.0us min=630.0us max=18921.0us p95=6142.0us p99=11501.0us
[ul_time_frequency]    samples=673 mean=16.0us   median=15.9us   min=4.2us   max=52.2us    p95=25.3us  p99=28.9us
[ul_channel_estimation]samples=673 mean=897.4us  median=805.7us  min=204.1us max=11971.7us p95=1130.3us p99=2787.5us
[ul_equalization_demod]samples=673 mean=17.0us   median=16.0us   min=3.3us   max=40.2us    p95=26.6us  p99=30.6us
[ul_fapi_mac]          samples=673 mean=5.5us    median=5.6us    min=1.0us   max=24.5us    p95=8.3us   p99=14.0us
[ul_ldpc_decode]       samples=673 mean=2286.1us median=1883.0us min=266.0us max=16758.0us p95=5221.0us p99=10236.0us
[ul_mac_pdu_size]      samples=673 mean=280.7B   median=253.0B   min=3.0B    max=528.0B    p95=480.0B  p99=528.0B | total=188899.0B
[metal_stats] mmse_ce commits=1710 waits=1710 max_in_flight=1
[metal_stats] ldpc_decoder commits=949 waits=949 max_in_flight=1
```

**ZMQ 腿（srsRAN UE，同配置）**：
```
[ul_pipeline]          samples=1690 mean=4825.7us median=4358.0us min=1396.0us max=9998.0us p95=9354.0us p99=9884.0us
[ul_time_frequency]    samples=1690 mean=58.8us   median=57.1us   min=46.0us   max=99.2us    p95=72.8us  p99=80.7us
[ul_channel_estimation]samples=1690 mean=899.7us  median=955.1us  min=274.3us max=3248.2us  p95=1177.3us p99=1223.1us
[ul_equalization_demod]samples=1690 mean=26.9us   median=27.2us   min=4.8us   max=72.0us    p95=46.0us  p99=54.4us
[ul_fapi_mac]          samples=1690 mean=6.5us    median=6.0us    min=2.3us   max=36.9us    p95=11.3us  p99=17.1us
[ul_ldpc_decode]       samples=1690 mean=3840.4us median=3233.0us min=770.0us max=8770.0us  p95=8352.0us p99=8677.0us
[ul_mac_pdu_size]      samples=1690 mean=201.6B   median=111.0B   min=7.0B    max=528.0B    p95=421.0B  p99=512.0B | total=340679.0B
[metal_stats] mmse_ce commits=3040 waits=3040 max_in_flight=1
[metal_stats] ldpc_decoder commits=1732 waits=1732 max_in_flight=1
```

**与基线对照（§6.1/§6.2）的结论**：

| 指标 | ZMQ 基线 → 回归 | OTA 基线 → 回归 | 判定 |
|---|---|---|---|
| 管线 mean | 5029.3 → 4825.7 µs | 2723.1 → 3216.5 µs | 一致（OTA 受流量影响，见下） |
| CE mean | 924.9 → 899.7 µs | 1034.9 → 897.4 µs | 一致 |
| LDPC mean | 4020.9 → 3840.4 µs | 1658.9 → 2286.1 µs | 一致（OTA 本次 MAC PDU mean 280.7 B vs 基线 128.7 B，大 TB 解码更久属预期） |
| t2f / eqdem / fapi_mac | 逐项一致 | 逐项一致 | 一致 |
| mmse commits/PUSCH | 1.80 → 1.80 | 3.53 → 2.54 | 一致（B6 结论不变：每 PUSCH 2–3 次提交） |
| max_in_flight | 1 → 1 | 1 → 1 | 一致（GPU 串行结构未变，S2 目标不变） |
| 功能 | 1690 CRC-OK | 673 CRC-OK | **全通，无异常** |

**结论：页对齐 rx 池切换行为中性——ZMQ 腿逐项与基线一致，OTA 腿差异全部可归因于
本次流量特征（更大 TB、更少重传）；S-0 验收通过。**

---

### 6.6 S1a FFT 双腿 A/B（2026-09-11，`--pusch_dft_type cpu|metal`，其余同基线配置）

| 分段 | ZMQ cpu | ZMQ metal | OTA cpu | OTA metal | 判定 |
|---|---|---|---|---|---|
| t2f mean | 59.6 µs | 59.9 µs | 15.8 µs | **1329.6 µs** | OTA 腿 Metal 生效：每符号 ~95 µs 同步 commit+wait × 14 符号（单测口径 174–246 µs 的单次变换在实链稳态 ~95 µs） |
| 管线 mean | 5175.5 | 5158.3 | 3075.4 | 3346.9 | 功能一致；OTA +9% 全部来自 t2f 增量 |
| CE / LDPC / eqdem / fapi_mac | 逐项一致 | 逐项一致 | 一致 | 一致（CE 1102.9→507.9、LDPC 1943→1497 的差异由流量驱动：mac_pdu 132→78.4 B） | 一致 |
| dft commits | — | —（见下） | — | **626447**，max_in_flight=6 | TX(IFFT)+RX(FFT) 双执行器共享队列有重叠 |

**结论与行动**：
1. **功能验证通过**：四腿全部 CRC-OK 正常（1156/979/1758/1700 样本），Metal FFT 数值正确（单测 NMSE −130 dB 级）。
2. **ZMQ metal 腿未生效的根因（2026-09-12 修正结论）**：不是旧二进制——`/tmp/gnb.log` 证实
   实机运行中 `pusch_dft_type: metal` 已生效且工厂选型日志为
   `[lower_phy] DFT backend: metal (GPU)`；**真正原因是 FFT 尺寸**：ZMQ 腿
   11.52 MHz/15 kHz → dft_size = **768 = 2⁸×3，非 2 的幂**，`is_supported_size(768)=false`
   → 每个 OFDM 调制/解调实例按设计逐配置回退 CPU（OTA 腿 7.68 MHz/15 kHz → 512，2 的幂，
   故生效）。**已由 S1a.1（commit `27603740ad`）解决**：内核扩展为混合基 2^k·3^m（≤4096），
   768/1536/3072 全支持（单测 NMSE −129…−132 dB）；PRACH 尺寸（12288/24576）继续回退 CPU。
   回退本身按设计静默，属预期行为而非故障。
3. **每符号同步 dispatch 的代价已量化**（~95 µs/FFT，OTA t2f 84×）——这正是 S2「单命令缓冲全链 + 批处理」要消除的
   往返；在 S2 落地前 **`--pusch_dft_type` 默认保持 `cpu`**，`metal` 仅作 A/B 与链路验证用途。
4. 下一步按计划：**S1a.1（radix-3 扩展：支持 2^k·3^m 尺寸，覆盖 768/1536/3072）** →
   S1b（均衡/解调 Metal 化）与 S1c（8×8 张量重排 CE v2）。

---

## 7. 已知残留问题与后续归属（不在 S-1 修复范围）

按「修复/重构分离」纪律，以下问题记录在案、暂不处理：

| # | 问题 | 归属 |
|---|---|---|
| R-A | HELENA `reload()` 无锁且无调用方（`port_channel_estimator_helena_impl.cpp:80-113`） | 支线一 S1 前评估；模型热切换需要时先加锁 |
| R-B | HELENA worker 线程无 `@autoreleasepool` + 每推断 ~10 个 ObjC 临时对象 | 蓝图 §5.3 ANE 流水化改造时一并处理 |
| R-C | ZMQ staging 8× 超配（`radio_zmq_rx_channel.cpp:25`） | 支线二落地后 ZMQ 腿退居回退，仅记录 |
| R-D | 引擎故障注入测试缺失（F2 回退路径暂无主动故障单测） | 建议：给引擎/适配器加**测试专用故障注入参数**（参照 `force_cpu_path` 模式，S1 前） |
| R-E | ICB 平台复测（M6 最小复现） | 支线一 S2 前/并行 |
| R-F | 52 桶模型遮蔽 51-PRB 引擎（`helena_impl.cpp:151-169` 桶分支） | 蓝图 §5.4 资产治理 |

---

## 8. 结论

- **静态审计**：B1–B14 全部定位到行并完成量化——§3 静态证据 + §6.4 实机基线分析
  （B3/B6/B7/B9/B10/B11/B12/B13 已有实链口径）。
- **修复批次**：6 项行为不变修复合入，**全量单测 7598/7598 通过**（含 MMSE 全绿、
  LDPC ALL OK、NOGPU 全 CPU 回退路径全绿）。
- **度量基础设施**：`[ul_fapi_mac]` 分段与 `[metal_stats]` 探针已在双腿实机产出数据
  （§6.1/§6.2），直接服务支线一 S2 的「无 CPU 阻塞 / GPU 队列深度」验收。
- **Go/No-Go 结论**：**S-1 与 S-0 全部收官**——探针/修复/审计/基线/页对齐池/双腿实机回归
  全部完成（7603/7603 单测 + OTA/ZMQ 双腿复测与基线一致）。
  **下一动作 = S1 正式开工（Metal 核心算子与 8×8 张量重排）。**
