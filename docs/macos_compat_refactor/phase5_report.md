# OCUDU macOS 移植重构 — 阶段五报告（最终）：端到端联调与全量回归

> 分支：`apple-silicon`　日期：2026-09-01
> 前置：阶段一~四 Checkpoint 全部通过。本文档为最终阶段 Checkpoint 产物，并附全项目重构总账。

---

## 1. CMake 构建脚本审计（本阶段任务）

| 检查项 | 结论 |
|---|---|
| 兼容层目标纳入 | ✅ `ocudu_macos_compat`（`utils/macos_compat/`，全平台无条件编译）经 `ocudu_support` PUBLIC 链接贯通所有核心库与应用 |
| 目标定义顺序 | ✅ **已修正**：`add_subdirectory(utils)` 提前至 `apps`/`lib` 之前（原位于 lib 之后，依赖方向由隐式前向引用承担；现已显式化）。双平台重配置+全树编译通过 |
| 平台源文件路由 | ✅ `lib/gateways/CMakeLists.txt`（SCTP 后端）、`lib/support/CMakeLists.txt`（kqueue/epoll）按 `if(APPLE)` 选择——平台宏不泄漏 |
| Metal 后端路由 | ✅ `ENABLE_METAL_LDPC`/`ENABLE_METAL_CHEST` 选项（默认值按 `APPLE AND arm64`）→ per-target 编译定义 `OCUDU_METAL_*` → `du_low_config_translator` 以 `OCUDU_METAL_*_AVAILABLE`（0/1 常量）路由；**DPDK/CUDA 相关 CMake 路径零改动** |
| DPDK 独立性 | ✅ 未触碰任何 `DPDK_FOUND`/`ENABLE_DPDK` 分支；兼容层不依赖 DPDK/CUDA 符号。注：131 参考机无 DPDK 环境，DPDK 使能编译未实测（结构性无影响，建议 CI 覆盖） |

## 2. 全量 BUILD_TESTING 回归

| 平台 | 结果 |
|---|---|
| macOS（arm64，FLOW_PROBES=ON 构建） | **100% tests passed out of 7598** |
| Ubuntu（x86_64，g++） | **100% tests passed, 0 tests failed out of 7608** |

## 3. OTA 空口人工检查（Checkpoint 要求）—— ✅ 已通过（用户实机确认，2026-09-01）

> 用户确认：最新构建 + B210 实机，双 UE（手机 + OAI UE）接入、双 ping、iperf3、优雅退出全部验证通过。

```bash
# 1. 最新构建启动 gNB（B210 + 真实 5GC）：
sudo ./build/apps/gnb/gnb -c configs/gnb_uhd_oaiue.yaml expert_phy --pusch_channel_estimator_algo metal_mmse
# 2. 双 UE 接入验证：手机 + OAI UE 各自 attach、双 ping 核心网；
# 3. U-Plane 时序验证：iperf3 持续吞吐（U-Plane 1ms/0.5ms 子帧时序平稳，
#    无 PRACH 重试风暴、无 RLC 重传雪崩）；可开启 FLOW_PROBES 构建观察 [ul_pipeline] 统计；
# 4. Ctrl+C 优雅退出（<5s，无 "Forcing exit"）。
```

## 4. 全项目重构总账（阶段一~五）

### 4.1 宏清点（核心业务代码 `__APPLE__` → 0）

| 域 | 文件数 | 清理前（处） | 清理后 |
|---|---|---|---|
| L1 PHY 数据面 | 2（lower_phy_baseband_processor.{h,cpp}） | 8 | **0** |
| L2 MAC | 1（mac_ul_sch_pdu.h） | 1 | **0** |
| PHY 上层工厂 | 1（upper_phy_factories.cpp） | 1 | **0** |
| U-Plane 内存池/追踪 | 2（bounded_object_pool.h、event_tracing.cpp） | 2 | **0** |
| 无线通道（ZMQ） | 2（radio_zmq_{rx,tx}_channel.cpp） | 2 | **0** |
| 配置翻译（Metal 入口） | 1（du_low_config_translator.cpp） | 2 | **0** |
| 线程初始化/调度 | 3（unique_thread.cpp、worker_manager.cpp、gnb.cpp） | 8 | **0** |
| 控制面 SCTP/UDP | 6（sctp_socket/server/client/common、udp_gateway） | 41 | **0** |
| **合计** | **18 个业务文件** | **65** | **0** |

平台宏现仅存在于：`utils/macos_compat/`（兼容层 .cpp，设计所在）、`include/ocudu/gateways/sctp_types.h`（平台类型头）、`lib/support/scheduling/darwin_thread_scheduling.{h,cpp}`（D5 原位保留的 Darwin 模块）、CMake 源文件路由，以及豁免清单（`external/**`、`lib/asn1/**`、`tests/**`、`utils/trx_ocudu/**`、OFH 以太网占位、pcap 诊断路径、resource_usage 诊断路径、futex_util、io_timer_source【B 类可选项】）。

### 4.2 兼容层资产（`utils/macos_compat` + `include/ocudu/support/macos_compat.h`）

**7 组 API**：内存对齐分配（Metal UMA 留口）、单调时钟、CPU 定位/可用核、线程调度（QoS/affinity tag/RT 约束 opt-in）、网络兼容（mmsghdr/recvmmsg/sendmmsg/msg_namelen）、L1 数据面（停止 FSM/执行器排空/步调等待）、字节序与后端诊断。全部 Linux=no-op 或上游等价、macOS=Mach/POSIX 映射，业务代码零平台判定。

### 4.3 测试基线（全阶段累计）

| 环境 | 结果 |
|---|---|
| macOS 全量 ctest | **7598/7598（100%）** |
| Ubuntu 全量 ctest | **7608/7608（100%）** |
| macOS ASAN（关键套件） | 零报错（UAF-001 除外，见下） |
| 双平台 gnb 全树编译 | ✓ |
| 优雅退出（FLOW_PROBES+UHD 复现） | 3/3 ~1s 零强制 |
| 实机 OTA | 用户确认：双 UE 接入 + 双 ping + iperf3（阶段三后） |

### 4.4 遗留账本

| 项 | 状态 |
|---|---|
| **UAF-001**（kqueue 回调内同步擦除，macOS 移植引入） | ⏳ PENDING——三次修复尝试各有回归（故障1/死锁/雷2），已回退；正确方案需在 kqueue 语义下重设计 + 三重回归门禁 |
| **上游化 patch**（§7c stop_impl 无条件清理 + §7d SCTP 停机顺序，均为对上游 Linux OCUDU 的偏离） | 📤 待打包提交上游（含溯源与复现方法，见阶段三报告 §7） |
| B 类可选项（io_timer_source / futex_util / io_broker_factory 零宏化） | 不阻塞验收 |
| DPDK/CUDA 使能编译烟测 | 建议 CI 覆盖（本机无环境） |

---

🔴 **Checkpoint 停止点**：CMake 审计完成（含 utils 顺序修正）；**双平台全量回归全绿（macOS 7598/7598、Ubuntu 7608/7608）**。**请按 §3 执行 OTA 人工检查**（sudo 启动最新 gNB + 双 UE 接入 + ping/iperf3 + 优雅退出），确认后即完成全部五个阶段。
