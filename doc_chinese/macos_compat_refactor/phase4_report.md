# OCUDU macOS 移植重构 — 阶段四报告：L1/PHY 核心数据面宏清理

> 分支：`apple-silicon`　日期：2026-09-01
> 前置：阶段三 Checkpoint 通过（make test 双系统、空口双 UE、优雅退出均确认）；UAF-001 保持 PENDING（历史代码）。
> 本文档为阶段四 Checkpoint 产物。

---

## 1. 交付物清单（A 类 7 个文件宏清零）

### 1.1 兼容层新增 API（`include/ocudu/support/macos_compat.h` + `utils/macos_compat/macos_compat.cpp`）

| API | Linux | macOS |
|---|---|---|
| `lower_phy_stop_chain_end(promise)` | **立即完成**（上游语义：链端检测即完成停止） | no-op（推迟） |
| `lower_phy_stop_task_end(state, wait_stop, stopped, promise)` | no-op | 任务真正结束时完成停止（D1：两种行为统一 API 下） |
| `drain_executor_on_stop(executor)` | no-op | 哨兵任务 + 等待（执行器排空后返回） |
| `wait_for_tx_timestamp()` | `sleep_for(10µs)`（上游） | `cpu_relax()` 自旋（Darwin 短睡眠合并导致超时） |
| `le16_to_host / host_to_le16 / le32_to_host / host_to_le32` | 恒等（LE 主机） | `OSSwap*`（libkern） |
| `recommended_zmq_io_buf_bytes()` | 0（ZMQ 默认） | 8 MiB |
| `log_effective_decoder_backend(str)` | no-op | GNB 日志输出 LDPC 后端（expert knob A/B 可验证） |

**命名教训**：字节序函数最初命名为 `le16toh` 等——与 glibc `<endian.h>` 的**同名宏**在声明/调用处冲突（宏改写符号名导致链接错误），已改为 `le16_to_host` 等宏安全命名。

### 1.2 业务文件宏清点（`__APPLE__` → 0）

| 文件 | 清理前 | 清理后 | 迁移去向 |
|---|---|---|---|
| `lib/phy/lower/lower_phy_baseband_processor.h` | 2 | **0** | FSM 停止语义 → `compat::lower_phy_stop_*`（D1 统一 API） |
| `lib/phy/lower/lower_phy_baseband_processor.cpp` | 6 | **0** | flush 哨兵 → `drain_executor_on_stop`；等待循环 → `wait_for_tx_timestamp`；`on_process_end` 调用解除条件编译 |
| `lib/mac/mac_ul/mac_ul_sch_pdu.h` | 1 | **0** | 字节序宏 → `compat::le16_to_host` 等函数 |
| `lib/phy/upper/upper_phy_factories.cpp` | 1 | **0** | 诊断日志 → `compat::log_effective_decoder_backend` |
| `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp` | 2 | **0** | **Metal 入口路由 CMake 化**：`OCUDU_METAL_CHEST_AVAILABLE`/`OCUDU_METAL_LDPC_AVAILABLE`（0/1 编译期常量，由 `apps/units/flexible_o_du/o_du_low/CMakeLists.txt` 从 `ENABLE_METAL_CHEST`/`ENABLE_METAL_LDPC` 注入） |
| `lib/radio/zmq/radio_zmq_rx_channel.cpp` | 1 | **0** | 8MiB 缓冲 → `recommended_zmq_io_buf_bytes()` |
| `lib/radio/zmq/radio_zmq_tx_channel.cpp` | 1 | **0** | 同上 |

**D2 裁定项**：`include/ocudu/adt/mutexed_mpmc_queue.h` 的 `__APPLE__` 分支**按要求保留现状**（不顺手修复，不纳入本阶段清点）。

### 1.3 行为保真（D1 核心要求）

- **Linux**：全部新增兼容层调用为 **no-op 或上游等价实现**（`stop_chain_end` 即上游 `set_value()` 位置；`wait_for_tx_timestamp` = 上游 `sleep_for(10µs)`；字节序恒等；ZMQ 缓冲 0=不改动；日志 no-op）。Ubuntu 实测 lower_phy 1/1、mac 227/227、phy 160/160 全绿——**Linux 原生行为零变化**。
- **macOS**：停止语义（任务真正结束才完成）、flush 排空、自旋等待、8MiB 缓冲、Metal 路由、诊断日志——全部与原 `#ifdef` 分支**逐行为等价**。
- **Metal 入口路由**：由 OS 宏（`__APPLE__`）改为 **CMake 后端可用性**（`ENABLE_METAL_*`）——语义从"是否 Apple 平台"升级为"是否链接了 Metal 后端"，符合"CMake 驱动的条件编译"验收要求。

## 2. 测试矩阵

| 环境 | 测试 | 结果 |
|---|---|---|
| macOS | `lower_phy_test`（L1 数据面 432 gtest 用例） | **1/1（432 用例全绿）** |
| macOS | `ctest -L mac`（L2 MAC，含 mac_ul_sch_pdu 字节序路径） | **227/227** |
| macOS | `ctest -L phy`（PHY 数学内核，金标准向量 bit-exactness） | **159/159** |
| macOS | `macos_compat_test`（常规 + ASAN） | **18/18 + 18/18** |
| macOS | gNB（FLOW_PROBES=ON + UHD）Ctrl+C 优雅退出 | **3/3 次 ~1s、零强制退出** |
| macOS | `make gnb` 全树编译 | ✓ |
| Ubuntu (x86_64/g++) | `lower_phy_test` / `ctest -L mac` / `ctest -L phy` | **1/1 + 227/227 + 160/160** |
| Ubuntu (x86_64/g++) | `make gnb` 全树编译 | ✓（no-op 兼容层路径 + CMake 常量 `0` 分支验证） |
| macOS | 全量 ctest（-j6） | **100% tests passed out of 7598** |

## 3. 验收标准对照

| 标准 | 状态 |
|---|---|
| 核心 L1 PHY 数据面与 L2 MAC 调度器 .cpp 文件零 `__APPLE__` | ✅ 7 个 A 类文件全部清零（§1.2 清点表） |
| 原有侵入性宏平滑迁移至兼容层 | ✅ 全部迁至 `macos_compat`（§1.1）；无行为变化（双平台实测） |
| 不破坏 Linux / BUILD_TESTING / DPDK / CUDA 编译 | ✅ Ubuntu gnb 全树编译 + PHY/MAC 标签全绿 |
| Metal/AI 入口"清理保留" | ✅ `du_low_config_translator` 改用 CMake 常量路由；`upper_phy_factories` 日志经兼容层；**未触碰任何上游信号处理核心逻辑** |
| 数学结果严格按位一致 | ✅ `ctest -L phy` 159/159（金标准向量），未改动任何数学内核 |

## 4. 遗留与阶段五衔接

1. UAF-001：仍 PENDING（历史代码，见阶段三报告 §2）。
2. B 类可选项（`io_timer_source` timerfd 仿真、`futex_util`、`io_broker_factory`）——零平台宏化可择机处理，不阻塞验收。
3. 上游化 patch 打包（§7c stop_impl + §7d SCTP 停机顺序）——待用户定时机。
4. 阶段五（端到端联调与全量回归）：全量 BUILD_TESTING + 真实 B210 OTA 双 UE 验证。

---

🔴 **Checkpoint 停止点**：阶段四编码与双平台验证完成，**全量 ctest 100% 通过（7598/7598）**。请审查本报告；如通过请回复「**检查通过，请继续**」，我将进入阶段五（端到端联调与全量回归：BUILD_TESTING 全量 + 真实 B210 OTA 双 UE 验证）。
