# OCUDU macOS 移植重构 — 阶段零报告：代码库平台宏调研与分类

> 分支：`apple-silicon`（上游 origin/dev）　日期：2026-09-01
> 本文档是《分阶段执行计划》阶段零的 Checkpoint 产物。用户审查确认后方可进入阶段一编码。

---

## 0. 结论摘要（TL;DR）

| 维度 | 结论 |
|---|---|
| 宏规模 | 核心代码（`lib/`、`include/`、`apps/`，剔除 `external/`、`tests/`、`docs/`）共 **34 个文件、约 130 处** `__APPLE__`/`__MACH__` 平台判定 |
| 已有兼容层雏形 | `lib/support/scheduling/darwin_thread_scheduling.{h,cpp}` 已是高质量部分兼容层：Mach QoS / THREAD_TIME_CONSTRAINT_POLICY / affinity-tag 全部封装、非 Apple 侧为 stub、全平台可编译 —— 这是新兼容层的最佳内核 |
| 重度侵入点 | ① L1 数据面 `lower_phy_baseband_processor.{h,cpp}`（8 处，**含停止状态机语义分叉**）；② SCTP 网关族（~50 处，已是 shim 形态）；③ `unique_thread.cpp`（6 处，线程初始化）；④ `io_timer_source`（7 处，timerfd 仿真） |
| 豁免面 | `external/**`（uWebSockets mmsghdr 仿真等，用户点名豁免）、`lib/asn1/**`（上游 ASN.1 生成码，当前零平台宏）、`utils/trx_ocudu/**`（Linux 专用工具，macOS 不编译）、`tests/**`（测试宏保留） |
| SIMD 宏 | `__AVX2__/__AVX512F__/__ARM_NEON/__x86_64__` 是**上游 CPU 特性分发宏**，非 OS 宏，**不在清理范围**（架构红线：不重构 PHY HAL、不改动信号处理核心） |
| Metal/AI 入口 | 路由已由 CMake per-target 承担（`OCUDU_METAL_LDPC`/`OCUDU_METAL_CHEST`，`ENABLE_METAL_*` 选项），方向正确；仅需清理业务文件中的入口判定宏（见 §3 A-6） |
| 现有测试基线 | macOS：7590/7590 ctest 注册，7556 通过 + 24 skip + 10 disabled（`tests/ci/macos_triage/SUMMARY.md`）；E2E 实链：有线直连后 mac gnb **100 ms 平均 ping / 711 rounds/s，反超 Ubuntu**（137 ms / 444，`tests/ci/macos_e2e/REPORT.md` §7）。重构回归以此为参照系 |

---

## 1. Big Picture（现状确认）

1. **项目**：OCUDU 5G CU/DU（Linux Foundation，BSD-3-Clause-Open-MPI）。当前工作分支 `apple-silicon`，含 macOS 原生 Clang 编译、USRP B210 空口握手、Metal LDPC/MMSE-CE 卸载、AI-CE（HELENA/CoreML）管线。
2. **既有移植策略**：所有共享代码改动以 `#if defined(__APPLE__)` 限定，Linux 侧逐字节保持上游行为（`triage/SUMMARY.md` §5 的 confinement 表）。本次重构的目标正是在**不破坏该行为边界**的前提下，把平台判定收拢到兼容层。
3. **Metal 与 AI 管线**是附着于 macOS L1 主干线的独立扩展（`lib/phy/upper/channel_coding/ldpc/metal/`、`.../channel_estimator/metal/`），其调用入口仅 2 处（`du_low_config_translator.cpp` 的 CE/LDPC 算法路由、`upper_phy_factories.cpp` 的诊断日志）。重构只需"保留并清理入口"，绝不触碰扩展内部与上游数学内核。
4. **已有平台后端路由**：`lib/support/CMakeLists.txt` 按 `if(APPLE)` 选择 `io_broker_kqueue.cpp` vs `io_broker_epoll.cpp` —— 这是用户要求的"CMake 驱动的条件编译"正确范式，后续推广即可。

---

## 2. 扫描方法

grep 模式清单（附录 B）覆盖：`__APPLE__/__MACH__`、`pthread_setaffinity_np`、`recvmmsg/mmsghdr/sendmmsg`、`memalign/posix_memalign/aligned_alloc`、`mach_absolute_time/thread_policy_set`、`usrsctp`、`__x86_64__/__aarch64__/__AVX*/__ARM_NEON`、`ENABLE_METAL_*`、CMake 中的 `APPLE`。逐文件人工复核了每一处命中，分类如下。

---

## 3. 分类结果

### A 类 —— 核心业务代码（L1 PHY 数据面 / L2 MAC / U-Plane 关键路径），必须清理（阶段四主战场）

| # | 文件 | 命中 | 宏内容 | 迁移方案 |
|---|---|---|---|---|
| 1 | `lib/phy/lower/lower_phy_baseband_processor.cpp`（6 处） | 76, 102, 127, 241, 250, 309 | ① `stop()` 执行器 flush 哨兵任务（macOS 关闭死锁修复）；② `dl_process()`/`ul_process()` 提前返回路径的 `on_process_end()`；③ DL 等待循环 `cpu_relax()` 自旋 vs `sleep_for(10µs)` | 见决策点 **D1**：提取"停止状态机 + flush"语义为兼容层组件，业务文件调用组件接口，零宏、双平台行为严格保持现状 |
| 2 | `lib/phy/lower/lower_phy_baseband_processor.h`（2 处） | 140, 153 | `on_process()`/`on_process_end()` FSM 语义分叉（Linux：状态达标即置位；macOS：任务真正结束才置位） | 同 D1，随组件一起下沉 |
| 3 | `lib/mac/mac_ul/mac_ul_sch_pdu.h`（1 处） | 6 | `le16toh/htole16/le32toh/htole32` 字节序宏：macOS 用 `libkern/OSByteOrder.h`，Linux 用 `<endian.h>` | 兼容层新增 `<ocudu/support/compat/endian.h>` 统一提供；L2 头文件改为单行 include，零宏 |
| 4 | `apps/services/worker_manager/worker_manager.cpp`（1 处） | 553 | radio worker 的 RT 优先级取值：macOS `max()-1`（QoS 提升），Linux `no_realtime()` | `compat::realtime_priority_for_radio_worker()`（内部按平台返回） |
| 5 | `apps/gnb/gnb.cpp`（1 处） | 213 | main 控制环线程 QoS 提升（`QOS_CLASS_USER_INTERACTIVE`） | `compat::elevate_main_thread_for_rt()`（Linux no-op）。属 app 壳，但符合"一行命令启动 + 隐式实时"验收目标 |
| 6 | `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp`（2 处） | 70, 83 | **Metal 入口路由**：① 非 Apple 强制 CE algo=`"cpu"`；② Apple 尊重 expert LDPC 类型 / Linux 保持上游无条件 `"auto"` 覆盖（3f227a41fb） | ① 用 CMake per-target 注入的配置常量（如 `OCUDU_METAL_CHEST_AVAILABLE=0/1`）替换 `#if !defined(__APPLE__)`；② "auto 覆盖"差异由兼容层配置归一化函数承担（Linux 实现=上游覆盖行为）。**绝不改动上游信号处理核心逻辑** |
| 7 | `lib/phy/upper/upper_phy_factories.cpp`（1 处） | 767 | macOS 专属诊断日志（LDPC decoder 类型） | 推荐**直接删除**（纯诊断，Linux 无此日志不构成回归；或迁入 `compat::log_backend_notes()`） |
| 8 | `include/ocudu/adt/mutexed_mpmc_queue.h`（1 处） | 99 | `pop_wait_for` 双签名分叉；**Linux 分支引用的 `bool*` 重载在当前 `blocking_queue.h` 中不存在**（全库仅 line 172 一个 `result` 版重载）—— Linux 分支是潜在死代码/坏代码 | 统一为 `result` 版重载（macOS 行为不变；Linux 修正潜在编译隐患）。见决策点 **D2** |
| 9 | `include/ocudu/support/memory_pool/bounded_object_pool.h`（1 处） | 148 | `sched_getcpu()` vs 常量 0（U-Plane 内存池的 CPU 感知段选择） | `compat::get_current_cpu()` |
| 10 | `lib/support/tracing/event_tracing.cpp`（1 处） | 22 | 同上 `sched_getcpu()` | `compat::get_current_cpu()` |
| 11 | `lib/radio/zmq/radio_zmq_rx_channel.cpp`（1 处）、`radio_zmq_tx_channel.cpp`（1 处） | 44 / 47 | 8 MiB ZMQ socket buffer 调优（U-Plane 传输通道） | `compat::recommended_zmq_io_buf_bytes()`（Linux 返回默认值不设置） |

### B 类 —— 支撑/基础设施（成为兼容层本体，或保持局部隔离）

| 文件 | 命中 | 处理建议 |
|---|---|---|
| `lib/support/scheduling/darwin_thread_scheduling.{h,cpp}` | 2+2 | **并入新兼容层**（作为其调度子模块）。已有 API：`darwin_qos_class_for_prio` / `set_this_thread_qos_class` / `set_pthread_attr_qos_class` / `set_this_thread_time_constraint` / `set_this_thread_affinity_tag` / affinity-tag 派生。全平台可编译、非 Apple 为 stub —— 直接满足"调用处零宏"要求 |
| `lib/support/executors/unique_thread.cpp` | 6 | 栈大小（16 MiB）、`pthread_setname_np` 签名、QoS+affinity 设置、`available_cpus()`、亲和性 —— 全部下沉为 `compat::initialize_worker_thread(name, prio, mask)`（阶段二） |
| `lib/support/network/io_timer_source.{h,cpp}` | 1+6 | timerfd vs pipe+Mach 线程仿真。→ 拆出 `macos_timer_fd` 实现文件由 CMake 按平台选取，业务文件零宏（阶段一/二顺带） |
| `lib/support/network/io_broker_factory.cpp` | 2 | epoll/kqueue 选择 → 交给 CMake 源文件路由（`lib/support/CMakeLists.txt` 已有范式），factory 内零宏。低优先级 |
| `lib/support/synchronization/futex_util.cpp` | 4 | futex vs `__ulock_wait/wake`。已完全隔离在底层工具文件，属"非核心业务逻辑"——**推荐豁免保留**；追求极致可拆文件+CMake |
| `lib/support/cpu_architecture_info.cpp` | 4 | CPU 拓扑发现（Linux `/proc`+NUMA vs macOS `hardware_concurrency`）。已按函数粒度隔离。可保留或阶段一迁移；顺带把 macOS 分支的中文注释统一为英文 |
| `lib/support/resource_usage/*`（3 文件） | 7+3 | RSS（`task_info`）、sysctl 内存、powercap stub。纯诊断路径 —— **豁免保留** |
| `include/ocudu/support/cpu_features.h` | 2 | `sysctl` vs `auxv` 特性探测（`__aarch64__` 分发 + OS 判定 include）。上游模式，基础设施 —— 豁免或轻微整理 |

### C 类 —— 控制面 SCTP / 网络栈（阶段三主战场）

| 文件 | 命中 | 内容与迁移方案 |
|---|---|---|
| `include/ocudu/gateways/sctp_socket.h` | 1 大块 | usrsctp 头包含 + Linux 兼容结构体重导出 + shim 函数声明。→ 拆出 `sctp_socket_usrsctp.{h,cpp}`（仅 APPLE 编译），公共头零宏 |
| `lib/gateways/sctp_socket.cpp` | 15 | usrsctp shim 实现：RFC 6951 UDP 封装（PID 播种端口）、socketpair 桥、`sa_len`、`SCTP_RECVRCVINFO`、SO_RCVTIMEO 仿真、优雅关闭、RTO 默认值、`sctp_get/setsockopt()`。→ 整体搬迁到 Apple-only 编译单元，Linux 走内核 socket API |
| `lib/gateways/sctp_network_server_impl.{h,cpp}` | 6+14 | `sctp_rcvinfo` vs `sctp_sndrcvinfo` 类型、drain 循环、关联上下文差异（Linux 每关联 fd 订阅 epoll；macOS 单桥接 fd） | 接收/事件处理差异下沉为 sctp socket 包装层统一接口（语义化 drain/订阅 API） |
| `lib/gateways/sctp_network_client_impl.{h,cpp}` | 1+5 | 同上 + 有界析构等待、keepalive token | 同 C-3 |
| `lib/gateways/sctp_network_gateway_common_impl.cpp` | 3 | `getaddrinfo` 提示差异（macOS 无 IPPROTO_SCTP，EAI_BADFLAGS）、sctp 事件 formatter | `compat::resolve_sctp_addr()` + 事件名映射下沉 |
| `lib/gateways/udp_network_gateway_impl.{h,cpp}` | 1+2 | `recvmmsg/sendmmsg` 仿真（首包阻塞 + MSG_DONTWAIT drain）、family 化 `msg_namelen` | 已是"兼容层+薄壳"形态：仿真函数收拢进兼容层 net 子模块，网关文件零宏 |

### D 类 —— CMake 路由（现状良好，按既定方向扩展）

- 顶层 `CMakeLists.txt`：`ENABLE_METAL_LDPC/ENABLE_METAL_CHEST` 默认值按 `APPLE AND arm64`；`OCUDU_METAL_*` 宏仅 per-target 注入 —— **符合验收标准，保持**。
- `lib/support/CMakeLists.txt`：epoll/kqueue 源文件选择 —— 保持并推广（futex/timerfd/SCTP 后端同理）。
- `tests/**`：`if(NOT APPLE)` 注册 DISABLED 占位 —— 保持不动。
- 新兼容层目标 `ocudu_macos_compat`：需在 `lib` 之前加入构建（顶层 `add_subdirectory` 顺序当前为 apps→…→lib→utils）。位置见决策点 **D4**。

### E 类 —— 豁免清单（绝对不动）

| 范围 | 理由 |
|---|---|
| `external/**`（uWebSockets `bsd.c` 的 mmsghdr 仿真、nlohmann、cameron314、rigtorp、fmt、Backward、CLI、TartanLlama） | 第三方依赖；uWebSockets 为用户点名豁免 |
| `lib/asn1/**` | 上游 ASN.1 编解码生成码；当前零平台宏，永不引入 |
| `utils/trx_ocudu/**` | `ENABLE_TRX_DRIVER` Linux 专用工具（`pthread_setaffinity_np`），macOS 不编译 |
| `tests/**` | 测试内 `__APPLE__` 判定保留；阶段四/五只要求测试通过，不要求测试零宏 |
| `lib/ofh/ethernet/ethernet_transmitter_impl.{h,cpp}` | OFH AF_PACKET 为 Linux 专属能力，macOS 仅保留编译占位 + 运行时报错；`ofh_integration_test` 已在 macOS DISABLED |
| `lib/pcap/backend_pcap_writer.cpp` | 诊断工具路径 udphdr 字段名差异；推荐豁免（追求零宏可极低成本迁移，优先级最低） |
| `lib/rrc/metrics/rrc_du_metrics_aggregator.h` | UB 规避分叉（`end()` 解引用在 libc++/libstdc++ 下读数不同）——见决策点 **D3**，建议不混入本次重构 |

### F 类 —— SIMD 特性宏（明确不在清理范围）

`lib/ocuduvec/*`、`lib/ofh/compression/*`、`lib/phy/**` 中的 `__AVX2__/__AVX512F__/__ARM_NEON/__x86_64__` 是上游 CPU 特性分发（编译期特性宏，非 OS 宏）。架构红线：不改动上游信号处理核心、不强行重构统一 PHY HAL。兼容层仅按规范新增 `utils::compat::complex_vector_multiply`（Apple 侧 vDSP）作为**可选新工具**，不替换任何现有内核。

---

## 4. 设计决策点（需用户拍板）

**D1 — L1 停止状态机分叉（最高风险项）**
`lower_phy_baseband_processor` 的 macOS 分支改变了停止语义（任务真正完成才置位 `stop_control`，`stop()` 用 flush 哨兵等待执行器排空），Linux 保持上游语义。推荐方案：兼容层提供**任务生命周期感知的停止同步组件**（如 `compat::lower_phy_stop_controller` + `compat::executor_drain_on_stop()`），业务文件零宏调用组件，双平台行为与现状严格一致；Linux 侧以 `lower_phy_test`（432 gtest 用例）证明行为等价。备选方案 B：把 flush 逻辑改为无条件实现（代码最干净，但改变 Linux 运行时行为，违反 byte-for-byte 政策）——除非用户豁免，否则不推荐。

**D2 — `mutexed_mpmc_queue.h` 的 Linux 分支疑似死代码**
Linux 分支调用的 `pop_wait_for(bool*, …)` 重载在库中不存在，说明该 API 在 Linux 侧从未被实例化。推荐：统一为 `result` 版重载（macOS 行为不变、Linux 修正潜在坏代码），跑 adt/scheduler 相关 gtest 验证。

**D3 — `rrc_du_metrics_aggregator` 的 `end()` UB**
现为 macOS 用 `rbegin()`、Linux 保留上游 `end()`（UB）。是否借本次重构改为无条件 `rbegin()`（Linux 行为随之改变，虽然是 UB 修复）？默认：**不动**，保持豁免。

**D4 — 兼容层位置**
规范为 `utils/macos_compat.hpp/cpp`。现状：`utils/` 仅为工具目录、顶层 `add_subdirectory(utils)` 在 `lib` 之后。推荐：源码放 `utils/macos_compat/`，公共头导出至 `include/ocudu/support/macos_compat.h`（与 `darwin_thread_scheduling.h` 同层），CMake 目标 `ocudu_macos_compat` 在顶层提前纳入构建，`lib/support` 与 `apps` 链接。备选：直接放 `lib/support/`。请确认。

**D5 — 现有 `darwin_thread_scheduling` 的去留**
推荐：**原位保留实现**（避免大面积 include 变更），新兼容层聚合转发其 API；阶段性择机合并。备选：整体移动进 `utils/macos_compat/`。

---

## 5. 与六阶段计划的文件级映射

| 阶段 | 工作包 |
|---|---|
| 一（内存+时钟） | 新建 `utils/macos_compat/{macos_compat.hpp, macos_compat.cpp}` 骨架 + `aligned_alloc/aligned_free` + `get_monotonic_time_us()`；迁移 `bounded_object_pool.h`、`event_tracing.cpp` 的 `get_current_cpu()`；新增 `macos_compat_test`（gtest：16K/4K page 对齐、SIMD 对齐、单调性、ASAN 干净） |
| 二（调度/线程池） | `unique_thread.cpp` 宏清理 → `compat::initialize_worker_thread()`；`worker_manager.cpp` radio 优先级；`gnb.cpp` main QoS；新增 `bind_thread_to_performance_core()` 组合入口（QoS+time-constraint）；Activity Monitor P-core 驻留验证 |
| 三（C 面 SCTP） | C 类 6 个文件拆分与包装统一；`sudo` 启动 gNB 观察 usrsctp 绑定端口与 SCTP 握手 |
| 四（L1/L2 净化） | A 类 11 个文件逐项迁移；全量 PHY/MAC gtest + 数学结果 bit-exact 验证；确认核心 lib 目录零平台宏 |
| 五（回归+OTA） | 全量 BUILD_TESTING（macOS 7590 基线）+ Linux（ENABLE_DPDK/ENABLE_CUDA）编译烟测 + B210 实链 1ms/0.5ms 时序 |

## 6. 风险清单

- **R1** lower_phy FSM（最高风险）：任何行为漂移都会破坏 1 ms 时序或引入死锁；Linux 侧必须跑 `lower_phy_test` 对比。
- **R2** SCTP shim 重构：现有 63 pass + 15 skip 是时序/端口/优雅关闭的脆弱平衡，重构必须以现有 shim 为黄金参照，禁止顺手"改进"。
- **R3** 阶段交叉依赖：阶段二与四都触碰 `unique_thread`/`lower_phy`，每阶段落地后立即跑对应标签 gtest。
- **R4** bit-exactness：Metal 与 CPU 路径对累加顺序敏感，阶段四禁止改动任何数学内核，只动宏与路由。
- **R5** 本仓库位于 OneDrive 同步目录，`build*/` 产物多，注意增量编译与缓存漂移（低风险提醒）。

---

## 附录 A：核心代码平台宏文件级命中表（34 文件）

```
lib/gateways/sctp_socket.cpp                          15   [C 类/阶段三]
lib/gateways/sctp_network_server_impl.cpp             14   [C 类/阶段三]
lib/support/resource_usage/perf_event_powercap_reader_impl.cpp  7   [B 类/豁免]
lib/support/network/io_timer_source.cpp                6   [B 类/兼容层]
lib/support/executors/unique_thread.cpp                6   [B 类/阶段二]
lib/phy/lower/lower_phy_baseband_processor.cpp         6   [A 类/阶段四]
lib/gateways/sctp_network_server_impl.h                6   [C 类/阶段三]
lib/gateways/sctp_network_client_impl.cpp              5   [C 类/阶段三]
lib/support/synchronization/futex_util.cpp             4   [B 类/豁免推荐]
lib/support/cpu_architecture_info.cpp                  4   [B 类/可保留]
lib/support/resource_usage/resource_usage_utils.cpp    3   [B 类/豁免]
lib/pcap/backend_pcap_writer.cpp                       3   [E 类/豁免]
lib/gateways/sctp_network_gateway_common_impl.cpp      3   [C 类/阶段三]
lib/support/scheduling/darwin_thread_scheduling.cpp    2   [B 类/兼容层内核]
lib/support/network/io_broker_factory.cpp              2   [B 类/CMake 路由]
lib/phy/lower/lower_phy_baseband_processor.h           2   [A 类/阶段四]
lib/gateways/udp_network_gateway_impl.cpp              2   [C 类/阶段三]
include/ocudu/support/scheduling/darwin_thread_scheduling.h  2   [B 类/兼容层内核]
include/ocudu/support/cpu_features.h                   2   [B 类/豁免]
apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp  2   [A 类/阶段四]
lib/support/tracing/event_tracing.cpp                  1   [A 类/阶段一]
lib/rrc/metrics/rrc_du_metrics_aggregator.h            1   [E 类/豁免-D3]
lib/radio/zmq/radio_zmq_tx_channel.cpp                 1   [A 类/阶段四]
lib/radio/zmq/radio_zmq_rx_channel.cpp                 1   [A 类/阶段四]
lib/phy/upper/upper_phy_factories.cpp                  1   [A 类/阶段四]
lib/ofh/ethernet/ethernet_transmitter_impl.h           1   [E 类/豁免]
lib/ofh/ethernet/ethernet_transmitter_impl.cpp         1   [E 类/豁免]
lib/mac/mac_ul/mac_ul_sch_pdu.h                        1   [A 类/阶段四]
lib/gateways/udp_network_gateway_impl.h                1   [C 类/阶段三]
lib/gateways/sctp_network_client_impl.h                1   [C 类/阶段三]
include/ocudu/support/memory_pool/bounded_object_pool.h  1   [A 类/阶段一]
include/ocudu/support/io/io_timer_source.h             1   [B 类/兼容层]
include/ocudu/support/cpu_architecture_info.h          1   [B 类/可保留]
include/ocudu/gateways/sctp_socket.h                   1   [C 类/阶段三]
include/ocudu/adt/mutexed_mpmc_queue.h                 1   [A 类/阶段四-D2]
apps/services/worker_manager/worker_manager.cpp        1   [A 类/阶段二]
apps/gnb/gnb.cpp                                       1   [A 类/阶段二]
```

## 附录 B：扫描 grep 模式清单

`__APPLE__|__MACH__`　`pthread_setaffinity_np|sched_setaffinity|sched_setscheduler|pthread_attr_setaffinity_np`　`recvmmsg|mmsghdr|sendmmsg`　`memalign|posix_memalign|aligned_alloc`　`mach_absolute_time|mach_timebase_info|thread_policy_set|THREAD_TIME_CONSTRAINT_POLICY|mach_thread_self|thread_get_assignment`　`usrsctp|ENABLE_METAL|METAL_LDPC`　`__x86_64__|__aarch64__|__arm64__|__i386__`　`immintrin|arm_neon|__AVX|NEON|vDSP_`　CMake：`APPLE`
