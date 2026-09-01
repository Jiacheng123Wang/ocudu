# OCUDU macOS 移植重构 — 阶段一报告：基础抽象层（内存与高精度时钟）

> 分支：`apple-silicon`　日期：2026-09-01
> 阶段零 Checkpoint 已通过（D1-D5 批示均已采纳）。本文档为阶段一 Checkpoint 产物。

---

## 1. 交付物清单

### 新增文件

| 文件 | 说明 |
|---|---|
| `include/ocudu/support/macos_compat.h` | 公共头（位于全局 include 路径，D4 方案） |
| `utils/macos_compat/macos_compat.cpp` | 平台 API 映射实现（全平台编译，内部按平台分派） |
| `utils/macos_compat/CMakeLists.txt` | 独立 `ocudu_macos_compat` 静态库目标（D4） |
| `tests/unittests/support/macos_compat_test.cpp` | 9 个 gtest 用例 |
| `docs/macos_compat_refactor/phase0_audit_report.md` | 阶段零报告（上轮交付） |

### 修改文件

| 文件 | 变更 |
|---|---|
| `utils/CMakeLists.txt` | `add_subdirectory(macos_compat)` |
| `lib/support/CMakeLists.txt` | `ocudu_support` 增加 `PUBLIC ocudu_macos_compat`（头文件内联调用 compat 符号的消费者必须链接到实现） |
| `include/ocudu/support/memory_pool/bounded_object_pool.h` | 移除 `#if defined(__APPLE__)` + `<sched.h>`，改调 `compat::get_current_cpu()` |
| `lib/support/tracing/event_tracing.cpp` | 删除本地 `get_current_cpu()` 宏分叉函数与 `<sched.h>`，改调 `compat::get_current_cpu()` |

**宏清理成果**：核心代码净移除 2 处 `#if defined(__APPLE__)` 块 + 2 处 `<sched.h>` 依赖；`__APPLE__` 仅存在于兼容层实现 `utils/macos_compat/macos_compat.cpp`（1 处，平台分派本应在此）。

## 2. API 设计与平台映射

```cpp
namespace ocudu::compat {
  void*    aligned_alloc(size_t alignment, size_t size);  // 对齐分配
  void     aligned_free(void* ptr);                        // 配对释放
  size_t   page_size();                                    // OS 页大小
  uint64_t get_monotonic_time_us();                        // 单调时钟 µs
  unsigned get_current_cpu();                              // CPU 定位（内存池用）
}
```

| API | Linux | macOS (Apple Silicon) |
|---|---|---|
| `aligned_alloc` | `posix_memalign`（对齐归一为 2 的幂、下限 64B 缓存行；size 向上取整到对齐倍数） | 同左（两平台 API 一致；为后续 Metal UMA 预分配池替换留出入口） |
| `page_size` | `sysconf(_SC_PAGESIZE)` → 4096 | `sysconf(_SC_PAGESIZE)` → **16384**（16K 页，测试中验证） |
| `get_monotonic_time_us` | `clock_gettime(CLOCK_MONOTONIC)` | **`mach_absolute_time()` + `mach_timebase_info` 换算**（Mach API 收敛于兼容层，业务代码零硬编码） |
| `get_current_cpu` | `sched_getcpu()`（失败返回 0） | 固定 0（与原宏分叉行为逐字节一致；内存池 CPU 感知分布退化为固定偏移） |

**设计决策记录**：
1. **命名空间 `ocudu::compat`**（附录参考为 `utils::compat`）：仓库全部公共头位于 `include/ocudu/**`、命名空间统一为 `ocudu`（与 D4 批准的 `darwin_thread_scheduling.h` 同层同域），沿用仓库惯例。如需改回 `utils::compat` 请批示。
2. **`get_current_cpu` 提前纳入阶段一**：`bounded_object_pool`（U-Plane 内存池）的宏属于"内存相关散落宏"，按阶段零 §5 映射归入阶段一。
3. **`page_size()` 为附录之外新增**：直接支撑验收标准"16K/4K page size"，后续 Metal UMA 池亦需此信息。
4. **`complex_vector_multiply`（vDSP）与调度 API（`bind_thread_to_performance_core` 等）留待后续阶段**：分别属于 SIMD 入口清理与阶段二（D5 聚合转发）范畴，不在本阶段引入死代码。

## 3. 测试证据

### 3.1 新 gtest（macos_compat_test，9 用例，含 ctest 注册 #7390-7398）

```
[==========] 9 tests from 4 test suites ran. (114 ms total)
[  PASSED  ] 9 tests.
```
覆盖：对齐 {64, 4096, 16384} × 多种 size 的地址对齐断言与全区间写入（memset 验证无欠分配）；非 2 的幂对齐拒绝；256 块互不重叠；`page_size()` 为 2 的幂且 Apple Silicon 上 == 16384；时钟单调不减、随 sleep 前进、与 `steady_clock` 速率一致（100ms 窗口漂移 <10ms）。

### 3.2 迁移代码回归（Release 构建 `build/`）

| 测试 | 结果 |
|---|---|
| `object_pool_test`（bounded_object_pool 迁移验证） | **25/25 通过** |
| `event_tracer_test`（event_tracing 迁移验证） | **10/10 通过** |
| `unique_thread_test` | 通过（exit 0） |
| `timer_test` | 17/17 通过 |
| ctest 组合运行（上述 13 项） | **100% tests passed out of 13** |

### 3.3 ASAN 验证（新构建目录 `build_asan`，`-DENABLE_ASAN=ON`）

```
macos_compat_test:  9/9  PASSED（含 256 块写入/读回，无越界、无泄漏报告）
object_pool_test:   25/25 PASSED
event_tracer_test:  10/10 PASSED
```
**无任何 ASAN 报错**。

### 3.4 全树编译覆盖

`make gnb`（Release）增量全量编译通过：`bounded_object_pool.h` 的全部消费者（scheduler、phy upper/lower、gateways、apps，共 27 个文件的包含关系）均在新头下编译成功，`gnb` 可执行文件已链接 `ocudu_macos_compat`。

### 3.5 Linux 侧验证（Ubuntu 参考机，2026-09-01 补充）

代码经 rsync 同步至 Ubuntu 参考机（`jwang@192.168.100.131:~/work/ocudu`，x86_64，g++，CMake 3.28，基线 `d80845fb44`）：

| 验证项 | 结果 |
|---|---|
| CMake 重配置（含新 `ocudu_macos_compat` 目标） | ✅ 成功 |
| 5 个测试目标构建（g++ Release） | ✅ 成功 |
| `ctest -R "macos_compat\|object_pool\|event_tracer\|unique_thread\|^timer_test"` | ✅ **13/13 通过**（100% tests passed） |
| `macos_compat_test` gtest 明细 | ✅ **9/9 通过**（Linux 分支实测：`clock_gettime` 单调时钟、`sched_getcpu` CPU 定位、`page_size()`=4096、`posix_memalign` 对齐） |
| `make gnb` 全树增量编译（`bounded_object_pool.h` 全部 Linux 消费者） | ✅ exit=0，gnb 链接成功 |

**结论：阶段一改动在 Linux 环境的编译与测试管线零回归；兼容层 Linux 分支（纯 POSIX）行为正确。**

## 4. 验收标准对照

| 标准 | 状态 |
|---|---|
| 基础工具类 gtest 运行 | ✅ 新增 9 用例 + 回归 25+10+17+unique_thread 全绿 |
| 16K/4K page size 与 SIMD 对齐满足 | ✅ 测试断言 64B/4K/16K 对齐；Apple Silicon 16K 页实测通过 |
| 无 ASAN 报错 | ✅ ASAN 构建下 44 个用例全绿 |
| 核心库散落宏迁移 | ✅ 2 处 `__APPLE__` 分叉移除，业务代码零平台 API 硬编码 |
| 不破坏 BUILD_TESTING / Linux / DPDK / CUDA | ✅ ctest 注册总数 7590 → 7599（净增 9，无吞case）；**Linux 侧已在 Ubuntu 参考机实测**（§3.5：13/13 测试通过 + gnb 全树编译零回归） |

## 5. 遗留与阶段二衔接

1. ~~Linux 侧验证~~：**已完成**（§3.5，Ubuntu 参考机 13/13 测试通过 + gnb 全树编译零回归）。
2. `lib/support/cpu_architecture_info.cpp` 的 macOS CPU 拓扑分支（中文注释统一）按阶段零 §5 留待**阶段二**（与线程调度同批处理）。
3. `darwin_thread_scheduling` 聚合转发（D5）与 `bind_thread_to_performance_core()` 组合入口：**阶段二**。
4. `aligned_alloc` 在 Apple 侧对接 Metal UMA 共享缓冲/预分配池：接口已留口，实现随 Metal 侧需求在后续阶段落地。

---

🔴 **Checkpoint 停止点**：阶段一（基础抽象层）已按批准方案完成编码与验证。请审查本报告 + 下方 Diff。如审查通过请回复「**检查通过，请继续**」，我将启动阶段二（实时调度与线程池包装）。
