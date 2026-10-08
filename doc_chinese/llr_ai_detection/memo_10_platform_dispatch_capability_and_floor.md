# 平台下发能力与 dispatch 地板（memo 10）

> **定位**：这不是 AI LLR 的算法发现，而是一次 **平台级** 发现——但它直接改写本工作流的**判据设计**，
> 所以必须进 memo 链，而不是躺在某次 debug 的日志里。
>
> 触发事件：M2（`jiachengwang@192.168.100.105`，8 GB）上 `dft_processor_metal_unit_test` 在
> n = 1024/1536/2048 失败，而同一代码在 M4 Pro 上全绿。
>
> 版本：v1.0 ｜ 日期：2026-10-08 ｜ 证据提交：`dbe36fa11f`、`b4d7545073`
> 上位文档：`platform_portability_design_rules.md`、`llr_ai_detection_design_and_implementation.md` §0(I7)

---

## 0. 两条规则（结论先行）

| # | 规则 | 一句话 |
|---|---|---|
| **R1** | ★★★ **能力必须在你实际下发的那个对象上查询** | 设备广告的值不是权威；**pipeline 才是 dispatch 被校验的对象** |
| **R2** | ★★★ **"算了但算错"与"根本没算"必须可区分** | 越界的 dispatch 不报错、不打印、输出原封不动——**它看起来像一个漂亮的数值结果** |

这两条都不是 Metal 特有的。R1 说的是"查询层次选错了"，R2 说的是"降级没有被计数"——
后者正是 `design_and_implementation` §0 **I7（降级必须可查询、可计数）** 的反面教材。

---

## 1. R1：设备上限 ≠ kernel 上限

### 1.1 事实

| | M4 Pro | **M2** |
|---|---|---|
| `MTLDevice.maxThreadsPerThreadgroup` | 1024 | 1024 |
| ★ `MTLComputePipelineState.maxTotalThreadsPerThreadgroup`（`dft_dit`） | **1024** | ★ **896** |
| `staticThreadgroupMemoryLength` | 32768 | 32768 |

同一个 `.metal` 源、同一个 macOS（26.6.2）、同族工具链（`metalfe-32023.921.6` vs `.864`），
**把 M4 编出的 `ocudu_dft.metallib` 拷到 M2 上，失败复现**——所以这不是编译产物差异，是**硬件对 pipeline 的占用判定差异**。

原因：`dft_dit` 把整个工作数组放在静态线程组内存里（`threadgroup float2 buf[MAX_FFT_N]`，`MAX_FFT_N = 4096` ⇒ 恰好 32 KB）。
在 M2 上这 32 KB 让 pipeline 只接受 896 个线程，而设备仍然广告 1024。

### 1.2 失败长什么样（★★ 这才是重点）

引擎下发 `min(n, 1024)`：

- `n = 768` ⇒ 768 ≤ 896 ⇒ **通过**；
- `n = 1024` ⇒ 1024 > 896 ⇒ **dispatch 被拒绝** ⇒ kernel 从未运行 ⇒ 输出保持原内容。

于是测试看到的是：

```
n = 128/384/512/768 : nmse ≈ −130 dB   -> OK
n = 1024/1536/2048  : nmse ≈  +0.97 dB, max_rel_err = 1.000e+00   -> FAIL
```

`max_rel_err = 1.0` 就是"输出全零"的指纹。**全程没有任何 error、warning 或日志**——
如果这不是在单元测试里，而是一次 OTA 飞行，它会表现为"某些带宽下解调突然全错"，
而所有计数器都健康。

### 1.3 为什么不能用"把线程数读出来"来修

第一反应是让 kernel 读 `[[threads_per_threadgroup]]`，让两边自动一致。**这条路已被血泪封死**：

> `ocudu_mmse_ta.metal` 的注释（full_gpu_chain §48.131）：早期版本用 `[[threads_per_threadgroup]]` 做循环步长，
> **第一次 dispatch 就再也没返回**——GPU 卡死、WindowServer 看门狗触发、`kill` 无效、`reboot` 挂起，
> 机器只能断电。

所以该仓库的既定规则是：**循环边界必须是编译期常量，参数只能 break/skip，绝不能是运行时读来的步长**。
`dft_dit` 里的 `const uint threads = min(n, 1024u)` 是**刻意的**——它是 `n` 的确定性函数，主机侧复制同一表达式。

★ 推论：**"clamp 下发线程数"是错的**。我试过把下发改成 `min(n, 512)`，结果**仍然失败**，
因为 kernel 仍以为是 1024——**两边对 stride 的理解必须逐字一致**。那次实验不成立，
不能作为"kernel 支持 threads < n"的证据。

### 1.4 采用的修法（`dbe36fa11f`）

**按尺寸拒绝，而不是纠正**：

1. `init()` 在创建 pipeline 时记下 `maxTotalThreadsPerThreadgroup`；
2. 若该尺寸需要的线程组放不下 ⇒ **拒绝该尺寸**，工厂回落到默认 DFT 实现；
3. kernel **一字未改**。

结果（M2）：`128..768` 走 GPU，`1024..4096` 降级，**全部 −130 dB**；
（M4 Pro）：**0 个尺寸降级**，全套仍走 GPU，数值与改动前一致。

★ 注意这是**按尺寸**的降级，不是整机降级：一台 pipeline 上限低的机器，
**仍然保留它能跑的所有尺寸**的 Metal 变换。

### 1.5 别人有没有同样的问题：审计

写了一个按 kernel 逐函数查询 pipeline 上限的工具（`pipeaudit`），在**两台机器的全部 9 个 metallib** 上跑：

- M4 Pro：**全部 1024**；
- M2：★ **`dft_dit` 是唯一一个低于 1024 的**（896），其余含 LDPC 的
  `nmsl_persistent_decode`（下发 1024 线程）**都是 1024**。

所以：LDPC 那条 1024 线程的 persistent dispatch 在 M2 上**安全**；
这次不需要改任何其他引擎。**这个结论只能靠审计得到，不能靠"看起来一样"推断。**

---

## 2. R2：dispatch 地板，以及"同步往返"才是成本

同一份测试在两台机器上的 `[time]` 行（单元测试内测量，**非 OTA**）：

| size | M4 Pro 同步往返 | M4 GPU-only | M2 同步往返 | M2 GPU-only | CPU generic |
|---|---|---|---|---|---|
| 512 | 104.5 µs | 9.2 µs | **191.0 µs** | **35.6 µs** | 0.9 µs (M4) / 1.3 µs (M2) |
| 768 | 110.6 µs | 11.7 µs | **200.4 µs** | **45.5 µs** | 1.4 µs (M4) / 2.0 µs (M2) |
| 1024 | 109.0 µs | 12.8 µs | （降级） | — | 1.9 µs (M4) |
| 2048 | 122.9 µs | 21.3 µs | （降级） | — | 4.0 µs (M4) |

M4 上 per-command-buffer 成本（size 1024）：

| 一个 command buffer 里的变换数 | host 总耗时 | 最后一个 command buffer 的 GPU 时间 | 摊到每个变换 |
|---|---|---|---|
| 1 | 100.3 µs | 12.9 µs | 12.9 µs |
| 2 | 107.3 µs | 12.7 µs | 6.4 µs |
| 4 | 117.8 µs | 13.0 µs | 3.2 µs |
| 14 | 113.9 µs | 12.7 µs | **0.9 µs** |

★★ 三点结论：

1. **算术便宜，往返贵**：一次 512 点 FFT 的 GPU 算术是 9 µs 量级，
   但**一次同步往返是 100–200 µs**——比 CPU 直接算（0.9–1.3 µs）慢 **100 倍以上**。
   单次同步下发 GPU 做 FFT 是**负优化**；只有在"一个 command buffer 装一整个 slot"时才成立（14 个变换摊到 0.9 µs/个）。
2. **M2 的往返地板约为 M4 的 2 倍**，GPU-only 部分约为 **4 倍**。
   ⇒ 换机器会把地板整体抬高，**任何"打满预算"的设计都会先在低端机上碎掉**。
3. 与 ANE 的既有实测对照：HELENA 整个模型在 **M4 Pro ANE 上 141 µs p50 / 208 µs p99**——
   即**一次同步 Metal DFT 往返的成本就和整个 AI 模型在 ANE 上的一次推理同量级**。
   这是"AI 推理走 NPU"这条引擎方针（`apple_silicon_heterogeneous_gnb_plan.md:17,124`）
   在数字上的又一次印证：**决定可行性的不是算术量，而是下发结构**。

---

## 3. 对本工作流的直接影响

| # | 影响 | 落到哪 |
|---|---|---|
| **1** | ★ 判据必须区分**"跑了且算错"**与**"根本没跑"** | 建议加入 `design_and_implementation` §0 的 I7 落实条款；A/B harness 对"输出恒定/全零/rel_err≡1.0"要有专门指纹，**不得**当作一个数值结果参与 NMSE 门限 |
| **2** | ★ 能力查询要走**设备感知**的入口，且该入口必须**就是**构造判据本身 | 已落为 `dft_processor_metal::is_supported_size_on_this_device()`（定义 = `init()` 的同一谓词，避免第二份规则的漂移） |
| **3** | ★ 降级必须**被计数并打印**，不能静默 skip | 测试现在打印 `[DEL] N of M ... delegated`，M4 打 `0 of 18`；日志里能一眼看出"这台机器实际跑了什么" |
| **4** | ★★ **AI 模型的下发尺寸/线程组配置同样要在 pipeline 上查询**，不能读设备广告值 | 进 `memo_08`（P0 设计）的检查清单 |
| **5** | 任何"AI 推理 141 µs vs 预算 500 µs"的论证，都要说明**这 141 µs 含不含下发** | `memo_03` / 高层规划的延迟判据 G5 |

★ 第 1 条是这次最值钱的一条：**我们自己的 A/B 框架也可能把一个"从未运行的 kernel"记成一次成功的测量**。
AI 侧的风险更高——模型输出即使全零也是有限的、形状正确的张量，host 侧只检查"有限、bit 序"。

---

## 4. 候选新规则（供 `platform_portability_design_rules.md` 采纳）

该文档的三层模型（A 硬件能力 / B 软件适用性 / C API 形状）判定：
**"把 C 层误当 A 层，是唯一会把我们锁死的错误"**。本次发现是同一错误的**第三种形态**：

> **候选规则**：能力查询必须发生在**与判据同一层次的对象**上。
> 设备广告的能力（A 层）**不能**用来预测某个编译产物（B 层）能否接受一次下发；
> 后者只有它自己的 handle 知道。**"在 A 层查、在 B 层用"与"把 C 层当 A 层"是同一类错误的两个方向。**

配套的操作性推论：**任何"能力查询"必须与它守护的那次下发共用同一份判据实现**——
两份实现就是两个会漂移的地方（本次修法中特意让 `is_size_runnable()` = `init()` 的同一谓词）。

---

## 5. 证据出处（可复现）

| 证据 | 位置 |
|---|---|
| 修复提交 | `dbe36fa11f`（metal dft: refuse a size the pipeline cannot dispatch…） |
| 新鲜度检查提交 | `b4d7545073`（cmake: add a metallib freshness check as a test） |
| M4 Pro 审计原始输出 | `/tmp/pipeaudit_m4.txt` |
| M2 审计原始输出 | `/tmp/pipeaudit_m2.txt` |
| M4 Pro 测试日志 | `/tmp/dft_m4_after3.log`（`[DEL] 0 of 18`，`ALL OK`，exit 0） |
| M2 测试日志 | `/tmp/dft_m2_after3.log`（`[DEL] 10 of 18`，`ALL OK`，exit 0） |
| M2 全套日志 | `/tmp/m2_full_after.log` |
| 审计工具 | `/tmp/pipeaudit.mm`（逐函数打印 `maxTotalThreadsPerThreadgroup` / `staticThreadgroupMemoryLength`） |
| `[[threads_per_threadgroup]]` 事故记录 | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_ta.metal:292-305`（full_gpu_chain §48.131/§48.133） |

复现命令：

```bash
# 逐 kernel 审计(任一 Apple Silicon 机器)
clang++ -std=c++17 -fobjc-arc -O1 -framework Metal -framework Foundation -o pipeaudit pipeaudit.mm
./pipeaudit $(find lib -name '*.metallib')

# 测试(M2 需 export PATH=/opt/homebrew/bin:$PATH,cmake 不在非交互 PATH 里)
cmake --build build -j --target dft_processor_metal_unit_test && \
  ./build/lib/phy/generic_functions/metal/dft_processor_metal_unit_test
```

---

## 6. 未决

| # | 事项 | 状态 |
|---|---|---|
| 1 | M2 全套 `cmake --build build --target test` 的最终结果 | 🟡 运行中（`/tmp/m2_full_after.log`）；已确认 DFT 一项由红转绿 |
| 2 | M2 上 Metal DFT 单次往返 191 µs vs CPU 1.3 µs —— **M2 是否应该默认关闭 Metal DFT？** | ⬜ 待裁定。这是一个"能力开关"问题，按 I6（开关按能力而非平台）应做成**按机型/实测**的默认值，而不是编译期开关 |
| 3 | 其余 8 个在 M2 上失败的测试，是否全为本根因 | 🟡 待全套日志确认 |
| 4 | `platform_portability_design_rules.md` 是否采纳 §4 的候选规则 | ⬜ 待用户裁定 |
| 5 | `memo_08`（P0 设计）加入"下发配置需在 pipeline 上查询"的检查项 | ⬜ 随 `memo_08` 正文一起 |
