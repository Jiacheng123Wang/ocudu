# macOS 不运行测试用例逐条审计报告（34 项，2026-09-02）

> 本文是 `SUMMARY.md` 第 2 节（"Tests that do not run on macOS (34 ...)"）的**详细版**，在 2026-09-02
> 对全部 34 个不运行用例逐条重新审计后的基础上撰写。审计基线：macOS 26.5.2 (arm64) `build/` 全量
> `make test`（7608 注册用例，7598 可运行，7574 通过 + 24 运行时跳过 + 10 ctest 禁用，0 失败）；
> Ubuntu 参照机 7608/7608 全跑。用例编号以当前 7608 用例运行为准（与旧版 SUMMARY.md 的编号相比，
> 因新增 `macos_compat_test` 等 18 个用例而整体后移）。

## 0. 审计方法

对每个不运行用例：

1. 在**当前源码**中定位其跳过/禁用条件（`GTEST_SKIP()` 文案、`#if defined(__APPLE__)` 守卫、CMake `DISABLED TRUE` 注释）；
2. 验证原因是否**还成立**（对关键前提做了实测：mbedTLS 瓶装配置、lo0 别名、usrsctp 传输设计、OFH 库的 AF_PACKET 守卫、AVX2 内联汇编等）；
3. 与 2026-08 首次扫描时记录的原因**逐字比对**，标注"未变 / 已精炼"；
4. 评估 macOS 上的**替代覆盖**、对移植版本的**影响**、以及未来**启用选项**。

## 1. 总体结论（先看这里）

- **34 项全部维持不运行，判定全部合理**：24 项运行时跳过（原因写在 `GTEST_SKIP()` 文案里，随每次 `make test` 输出可见）、10 项 ctest 禁用（原因写在 CMake 注释里）。没有任何一项是"跑不起来却默默消失"。
- **原因仍成立的比例 34/34**；其中 **15 项 SCTP 的原因文字被精炼**（旧版"不能绑定 IPv4+IPv6 同一 socket" → 新版统一为"usrsctp 走 UDP 封装、单一通配 socket、无法关联不同本地地址"，并补充了"root 也不行的原因"），其余 19 项原因未变。
- **每一类都有 macOS 替代覆盖**（详见各小节），不存在"整块功能在 macOS 完全裸奔"的情况；对移植版本**无重大影响**。
- 两个**启用候选**（低优先、非阻塞）：`du_high_benchmark`（现可经 `ocudu::compat` 调度层启用，§G）、RLC 压力测试 ×7（可经 pthread_barrier 仿真启用，§F）。另有 3 个理论启用路径（SCTP 多地址、mbedTLS 自编译、亚微秒时钟），代价高/收益低，见各节。

## 2. 逐条审计

### A. SCTP shim 结构性限制 —— 15 例（2565, 2972-2975, 2996-3001, 3008, 3017-3019）

| 用例 | 当前原因 | 原因变化 |
|---|---|---|
| 2565 `e2ap_network_adapter_test.when_e2_setup_response_received_then_ric_connected` | E2 agent 绑 127.0.0.101、RIC 监听 127.0.0.1：usrsctp 的 UDP 封装走**单一通配 socket**，出包永远用默认源地址，绑在其他本地地址上的关联收不到应答；root 也无效（原生 SCTP 报文 macOS 不会环回给本地 raw socket） | 精炼（补充 root 无效的机理） |
| 2972 `sctp_socket_test.bindx_with_mixed_ipv4_and_ipv6_addresses` | 同上（**旧版原因是"不能在同一 socket 绑 IPv4+IPv6"——已更正为源地址/通配 socket 问题**） | **更正** |
| 2973-2975 `connectx_*` ×3 | usrsctp 无 `sctp_connectx()`，shim 只用地址列表第一项 | 未变 |
| 2996-3001 `sctp_network_server_peer_test.*` ×6 | 三个 peer 分别绑 127.0.0.1/2/3（含 127.0.0.4/5 多宿主场景）：shim 无法关联不同本地地址 | 精炼（措辞统一） |
| 3008, 3017-3019 `sctp_network_client_test.*` ×4 | 客户端绑 127.0.0.2 / 多宿主服务器 / 混合 v4+v6 场景：shim 无法关联不同本地地址 | 精炼（措辞统一） |

**验证（原因还成立？）**：✅ 成立。当前 `lib/gateways/sctp_socket_usrsctp.cpp` 仍为每进程单端口 UDP 封装（`g_udp_encaps_port` 全局 + `usrsctp_init(port)` 单 socket）；`tests/unittests/gateways/sctp_test_helpers.h` 的两个跳过宏（`OCUDU_SKIP_IF_NO_SCTP_MULTI_LOCAL_ADDRESS` / `OCUDU_SKIP_IF_NO_SCTP_CONNECTX`）当前文案即上表。15 例在 Ubuntu 全部通过（多宿主/混合地址族是内核 SCTP 的能力），确认是 macOS 端 usrsctp 传输的结构性边界而非测试问题。lo0 别名（127.0.0.2/3、127.0.1.1、127.0.0.101，现由 LaunchDaemon 常驻）已齐备，**并非**跳过原因。

**macOS 替代覆盖**：✅ 有。同二进制/同标签下仍在跑的 SCTP 用例（63 项通过）覆盖：单地址 bind/connect、1/4/8/32 客户端关联、收发数据、通知（COMM_UP/COMM_LOST/SHUTDOWN）、EOF 握手、RTO/INITMSG/NODELAY 选项、停机清理路径（含 §7d 修复的并发停机竞态回归点）。**未覆盖**的只有：多宿主（bindx）、混合地址族、多目的地 connectx、双端不同本地地址的拓扑。

**影响评估**：中低。gNB 生产配置的多宿主（一节点绑多个地址做冗余）在 macOS 上不可用 → macOS gNB 实际为**单宿主**部署；这是当前最普遍的部署形态，OTA 双 UE 全流程已验证。对移植版本不构成阻塞。

**启用选项**（低优先 TODO，见 e2ap 测试内注释）：① shim 为每个本地地址建独立 UDP 封装 socket；② usrsctp AF_CONN 传输；③ 测试改单回环地址拓扑（会损失多宿主断言价值，不推荐）。成本高（需改 usrsctp 集成），收益有限，维持跳过。

### B. NIA2-CMAC 引擎（mbedTLS）—— 8 例（7292-7299）

**当前原因**：Homebrew `mbedtls@2` 2.28.10 瓶装版 `config.h` 中 `//#define MBEDTLS_CMAC_C`（CMAC 被注释掉），`integrity_engine_nia2_cmac` 未编译进 macOS 构建。

**验证（原因还成立？）**：✅ 成立，实测 `/opt/homebrew/opt/mbedtls@2/include/mbedtls/config.h:2878` 确为 `//#define MBEDTLS_CMAC_C`；本机还装有 `mbedtls@4`（4.x 已移除传统 CMAC 模块，改用 PSA API，同样不提供 `MBEDTLS_CMAC_C`）。构建实际链接 `mbedtls@2`。原因未变。

**macOS 替代覆盖**：✅ **完全等效**。这是 34 项里覆盖最好的一类：macOS 生产路径使用 `integrity_engine_nia2_non_cmac`（工厂在 `security_engine_impl.cpp` 中按 `#ifdef MBEDTLS_CMAC_C` 选择），而**同一测试套件**的 8 个 `integrity_engine_nia2_non_cmac/*` 用例在 macOS 上运行并通过——用的是与跳过的 8 例**完全相同的 3GPP 官方 NIA2 测试向量**。即：macOS 上实际跑的 NIA2 代码被官方向量验证过，跳过的只是"mbedTLS-CMAC 实现"这一未用于 macOS 的代码路径。

**影响评估**：无功能影响。macOS gNB 的 NIA2 完整性保护可用（软件实现），与 Ubuntu 的差异仅为底层实现选择不同。**跳过理由充分且正确**。

**启用选项**（可选、非必需）：自编译 mbedtls@2（开启 `MBEDTLS_CMAC_C`）或适配 mbedtls 3/4 的 PSA API，可让 cmac 引擎用例也在 macOS 跑。收益 = 额外覆盖一段 macOS 产品不用的代码，仅为与 Ubuntu 的字节级路径对齐；维持跳过。

### C. R16 参考时间亚秒精度 —— 1 例（6531）

**当前原因**：macOS libc++ 的 `system_clock` 为微秒分辨率，10ns 分量无法表示（解码后比对误差可达 999ns）。`rrc_du_ref_time_r16_test.cpp:68` 运行时跳过，注释同时记录了修复方向（`get_ref_time_r16()` 应返回纳秒分辨率时间点）。原因未变。

**macOS 替代覆盖**：✅ 有。同二进制的另外 2 例（秒级编码/解码往返）在 macOS 通过；`f1ap_ref_time_provider_adapter_test` 的亚秒例同因跳过（gtest 级，不在此 34 项清单内）。

**影响评估**：低。R16 ReferenceTime 的 10ns 分量在 macOS gNB 上丢失——只影响亚微秒级时间同步语义（TSN 类场景），当前 OTA/研究用途不受影响。跳过理由充分；修复是产品代码增强（非测试问题），记录为 TODO。

### D. DFT 处理器 ci16 —— 1 例（5406，ctest Disabled）

**当前原因**：`dft_processor_ci16_avx2.cpp` 使用 `<immintrin.h>` AVX2 内联，x86_64 专属；CMake `elseif (APPLE)` 分支注册 `DISABLED TRUE` 占位。原因未变（已复核 CMake 注释与源码）。

**macOS 替代覆盖**：✅ 有且充分。`dft_processor_test` 在 macOS **运行并通过**（本机构建启用了 FFTW+FFTZ+ARMPL，覆盖 generic/FFTW/FFTZ 等处理器），另有 `dft_processor_generic_benchmark` 可跑。ci16-AVX2 只是 x86 性能变体。

**影响评估**：无。macOS gNB 使用 generic/FFTW 处理器路径（均有测试覆盖）；ci16 不在 macOS 产品路径上。禁用理由充分。

### E. OFH 集成测试 —— 1 例（7560，ctest Disabled）

**当前原因**：通过 AF_PACKET（`linux/if_packet.h`）驱动真实以太网控制器；`lib/ofh/ethernet/ethernet_receiver_impl.cpp:43` 的 AF_PACKET 实现整体 `#ifdef __linux__`，macOS 无此接口。原因未变且**结构性成立**（任何 macOS 版本都不可能有 AF_PACKET）。

**macOS 替代覆盖**：✅ 有且充分。OFH 库在 macOS 完整编译，**150 个 OFH 单元测试**（receiver/transmitter/serdes/ethernet/ecpri/uplane 打包器等）在 macOS 运行并通过——处理逻辑全覆盖，缺的只是"真实网卡上的端到端收发"。

**影响评估**：无。OFH（前传）作为无线传输在 macOS 本就不受支持（macOS 移植使用 UHD/ZMQ 射频），集成测试的缺失与 macOS 产品形态一致。禁用理由充分。

### F. RLC 压力测试 —— 7 例（7568-7574，ctest Disabled）

**当前原因**：`rlc_stress_test.cpp` 使用 `pthread_barrier_{init,wait,destroy}`；macOS 至今未实现 pthread_barrier 族（macOS 26 亦然，已确认）。原因未变。

**macOS 替代覆盖**：✅ 有。同一 RLC 代码的全部常规单元/集成测试在 macOS 通过（`rlc*` 前缀 ctest 用例 285 项全通过，仅此 7 例压力测试被禁用）；压力测试的价值是**多线程负载/竞态**暴露，Linux 侧另有 TSan 运行（label `tsan`）兜底竞态。macOS 缺少的只是"压力负载"维度。

**影响评估**：低。RLC 功能正确性在 macOS 有完整覆盖；竞态检测由 Ubuntu 的 TSan 承担。

**启用选项**（低优先 TODO，可行）：在 `utils/macos_compat` 增加 ~60 行 `pthread_barrier_t` 仿真（互斥锁+条件变量，兼容 barrier 计数），测试代码零改动即可在 macOS 启用这 7 例。收益 = 恢复压力负载覆盖；成本低。**建议列入后续改进项**，本次维持禁用。

### G. du_high_benchmark —— 1 例（7595，ctest Disabled）

**当前原因**：`du_high_benchmark.cpp` 的 `configure_main_thread()`（第 1325-1352 行，全文**唯一** Linux 专属依赖）使用 `sched_get_priority_max(SCHED_FIFO)` + `cpu_set_t`/`CPU_ZERO`/`CPU_SET` + `pthread_setaffinity_np`；CMake `if (NOT APPLE)`。原因未变。

**macOS 替代覆盖**：✅ 有。DU-high 的功能由 48 例集成测试 + 大量单元测试在 macOS 覆盖；benchmark 缺失只影响**性能数字**的采集。

**影响评估**：无功能影响；但对移植版本的性能论证有价值（Apple Silicon 上的 DU-high 吞吐数字）。

**启用选项**（低优先 TODO，现因兼容层而**可行**）：该函数可经 `ocudu::compat` 调度层改写——`compat::set_thread_realtime_priority()` 替代 SCHED_FIFO 段、`compat::set_thread_affinity()` 替代 `cpu_set_t` 段（兼容层已有同名能力，`-a` 选项语义不变，"无亲和"默认路径天然可跑）。改动仅限该测试文件 + CMake 放行，**建议列入后续改进项**，本次维持禁用。

## 3. 与 Ubuntu 的对照

| | macOS (arm64) | Ubuntu (x86_64) |
|---|---|---|
| 注册用例 | 7608 | 7608 |
| 通过 | 7574 | 7608 |
| 运行时跳过 | 24 | 0 |
| ctest 禁用 | 10 | 0 |
| 失败 | 0 | 0 |

24 个跳过 + 10 个禁用全部为 macOS 专属；Ubuntu 无任何跳过/禁用。双边注册数一致（无静默丢例）。

## 4. 判定结论

| 类别 | 例数 | 原因仍成立 | 原因变化 | macOS 替代覆盖 | 对移植版本影响 | skip/disable 判定 |
|---|---|---|---|---|---|---|
| A. SCTP shim 结构性限制 | 15 | ✅ | 精炼（1 例更正） | 有（63 例通过，缺多宿主/混合族/connectx 维度） | 中低（生产多宿主不可用，单宿主部署已验证） | **充分** |
| B. NIA2-CMAC（mbedTLS 瓶装） | 8 | ✅ | 未变 | **完全等效**（同向量 non-cmac 例通过） | 无 | **充分** |
| C. R16 亚秒时钟 | 1 | ✅ | 未变 | 有（同套件 2 例通过） | 低（亚微秒精度丢失） | **充分** |
| D. DFT ci16 (AVX2) | 1 | ✅ | 未变 | 有（generic/FFTW/FFTZ 全测） | 无 | **充分** |
| E. OFH 集成（AF_PACKET） | 1 | ✅ | 未变 | 有（150 例 OFH 单测） | 无（OFH 非 macOS 传输形态） | **充分** |
| F. RLC 压力（pthread_barrier） | 7 | ✅ | 未变 | 有（常规 RLC 全测 + Ubuntu TSan） | 低 | **充分**（建议：compat 层 barrier 仿真后启用） |
| G. du_high_benchmark（CPU 亲和） | 1 | ✅ | 未变 | 有（48 例 DU-high 集成） | 无功能影响 | **充分**（建议：经 compat 层启用） |

**最终判定**：34 项在 macOS 不运行的理由**全部成立且充分**；skip/disable 机制（运行时带文案跳过 vs ctest 禁用占位）选择正确；每类均有替代覆盖，**对 macOS 移植版本无重大影响**。两个低成本启用项（F、G）建议列入后续改进，其余维持现状。
