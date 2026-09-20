# S14：离线 GPU 时间仪器的三件事（计数器不可用 / 等待点实测 / `busy split` 的可用协议）

> 过程证据。结论已进设计文档 §5.8.15。原始数据与命令都在这里，可复跑。

---

## 1. 逐 dispatch 计数器：**这台设备上不可能**（已实测，永久关闭这条路）

### 1.1 先撞到的是一句断言，不是"没打印"

上一会话留下的状态是"采样缓冲建得起来、采样点在编、但 `[disp_time]` 一行都没打"，于是下一步被定为
"查清延迟提交在哪儿被等，再把解析钩子挂过去"。把那个仪器按同样的做法重写一遍（`MTLCounterSampleBuffer`
+ `sampleCountersInBuffer:atSampleIndex:withBarrier:YES`，钩子改成挂在命令缓冲自己的完成回调上，
**所以与"谁等它"完全无关**）之后，拿到的是：

```
-[AGXG16XFamilyComputeContext sampleCountersInBuffer:atSampleIndex:withBarrier:]:1018:
    failed assertion `MTLComputeCommandEncoder:sampleCountersInBuffer:atSampleIndex:withBarrier
    not supported on this device'
...
Abort trap: 6      (exit 134)
```

**它不只是沉默，它把进程打死。** 也就是说"钩子挂哪儿"从来不是（唯一的）问题。

### 1.2 原因：设备没有那个采样点（`wip/metal_counter_caps.mm`）

```
device: Apple M4 Pro (registryID=4294968347)
  supportsCounterSampling:AtStageBoundary    = YES
  supportsCounterSampling:AtDrawBoundary     = no
  supportsCounterSampling:AtBlitBoundary     = no
  supportsCounterSampling:AtDispatchBoundary = no
counterSets:
  timestamp (1 counters)
      GPUTimestamp
sample buffer: built
```

- `sampleCountersInBuffer:` 要求调用点所在的**采样点**被支持；四个点里这台机器只有
  **`AtStageBoundary`**（那是 render encoder 里"两个 pipeline 阶段之间"的概念）。
  **没有 dispatch boundary、也没有 blit boundary** ⇒ 一条纯 compute 的链上**没有任何地方**能放采样点。
- **"有计数器集合"与"能在某处采样"是两个独立的问题**，而上一会话只问了前一个
  （`supported=1` 说的是 `[device counterSets]` 里有 `timestamp`），于是得出了"设备支持"的结论。
  采样缓冲确实建得起来（上面 `sample buffer: built`）——这也正是这个坑难看见的原因。

### 1.3 结论

设计文档 §5.8.14 结尾那条建议（"换仪器：`MTLCounterSampleBuffer` 的逐 dispatch GPU 时间戳"）
**在这台硬件上不成立，不要再按那个写法重试**。同一句话的后半条
（"把被测段单独编成一条命令缓冲隔离测量"）才是这里能走的路——`[ul_gpu_lane]` 的 `busy split` 是
用 `MTLCommandBuffer` 自己的 `GPUStartTime`/`GPUEndTime` 算的，**不需要任何计数器采样缓冲**。

复跑：

```bash
xcrun clang++ -std=c++17 -fobjc-arc -framework Metal -framework Foundation \
    -o /tmp/metal_counter_caps doc_chinese/phy_pipeline_gpu/wip/metal_counter_caps.mm && /tmp/metal_counter_caps
```

---

## 2. "延迟提交在哪儿被等"：**实测有答案了**（`OCUDU_CE_WAIT_TRACE=1`）

读代码读不出来（一个 pending 槽、多个入口能收它，路线取决于 lane order、提取有没有 hold 住缓冲、
以及上一批是否还没收），所以改成打点：**引擎的 11 个 `waitUntilCompleted` 站点全部命名**，
发布点也命名，配对的 cb 指针就是这一跳走的路。

### 2.1 离线回放根本不走延迟路线

`ul_chain_replay` 走的是 `compute()`（`do_submit(..., deferred=false)`），
trace 全程只有 `end_stage`（16 次）⇒ **`ul_chain_replay` 无法回答这个问题**。

### 2.2 单测走（Test 13 用 `submit()`/`finish()`）

```bash
OCUDU_CE_WAIT_TRACE=1 ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
```

| trace 行 | 次数 |
|---|---|
| `end_stage_async: published as pending` | 5774 |
| `collect_async_stage`（host_wait 路线：就地等）| 5766 |
| `collect_async_stage: event order, collected later by wait_pending()` | 8 |
| `complete_fd_td_estimation_stage: pending_fused_burst=… has_pending=…` | 2782 |
| **`wait_pending_impl`** | **8** |
| `end_stage_async: burst order, nothing published` | 8 |
| `collect_async_stage: burst order, the lane commits and waits` | 8 |
| `lane_fence_selftest` / `encode_weights_only` / `run_epoch_probe` / `run_ta_place` | 4 / 26 / 10 / 1 |

配对（原始日志 16097–16100 行，默认 `event` 序）：

```
[ce_wait] end_stage_async: published as pending cb=0xb4475c000
[ce_wait] collect_async_stage: event order, collected later by wait_pending()
[ce_wait] complete_fd_td_estimation_stage: pending_fused_burst=0 has_pending=1
[ce_wait] wait_pending_impl cb=0xb4475c000          <-- 同一个 cb
```

**⇒ 三条路线的收集点：**

| 路线 | 发布 | 收集 |
|---|---|---|
| `event`（默认）| `end_stage_async` 发布为 pending | **`wait_pending_impl`**（由 `complete_fd_td_estimation_stage` 调用）|
| `host_wait` | 同上 | `collect_async_stage`（就地等）|
| `burst` / `merged` | **什么都不发布** | 没有——lane 的 `shared_burst` 自己 commit + wait |

**⇒ 默认路线上，上一会话挂的三个钩子里 `wait_pending_impl` 本来就是对的。**
仪器沉默与"挂错地方"无关（§1 已给出真正原因）。

trace 还顺带把入口守卫看清楚了：`end_stage_async` 发布 X 之后，紧接着的
`wait_pending_impl` 常常报的是**另一个 cb**——那是**上一跳**的 pending 被守卫收掉。

---

## 3. `busy split` 的撤回是对的，但**原因和协议都要改写**

### 3.1 上一会话的散布是真的（`--repeat 20`）

同一捕获 `syn004_4`、默认配置、`--repeat 20`、12 次：

```
544.4 545.0 545.3 548.0 550.1 301.7 278.7 547.2 553.0 545.1 545.6 371.7   (ch_wt µs/lane)
```

**双峰**：3 次落在 279–372，9 次落在 544–553。§4.1 记的 571/554/**203**/606/625/**142** 就是这一族。

### 3.2 但同一配置 `--repeat 1`（一个进程一跳）是稳的

同一捕获、默认配置、`--repeat 1`、12 次：

```
553.7 555.3 554.4 557.1 559.7 554.7 553.7 556.3 570.7 553.5 555.1 555.8
```

12/12 落在 **553.5–570.7（±1.5%）**。后来 9 次一组的复测是 **553.9–561.5（±0.7%）**。

**两次运行的工作量完全相同**：低值那次 `device_corr_builds=40`、`cbs/lane=2.00`、`carried=0`，
与高值那次逐项相同——变的只是**被测出来的 GPU 跨度**。这就是 §4.1 说的那个机制
（离线回放不按 slot 节拍喂数据 ⇒ 相邻跳、以及前端 DFT 队列与后端车道在设备上重叠），
**它污染的是"某个命令缓冲的 GPU 跨度"，而 `--repeat 1` 把这个重叠固定住了**。

### 3.3 ⇒ 可用协议（写进设计文档）

> **`--repeat 1`，一个进程一跳，取 N≥9 次的【中位数】。**
> 不要用 `--repeat 20` 读这个量；中位数用来挡掉残余的向上离群（实测 ~1/9 次）。

在这个协议下，§7 的保值重复探针才**真的**能用：斜率 = 该段多编一次的 GPU 成本。

---

## 4. ★ 第一次量出 `ch_wt`（≈554 µs）里各段各占多少

捕获 `syn004_4`，`--repeat 1`，每个臂 **9 次**，取中位数（µs）：

| 臂 | 9 次原始值 | 中位 | Δ |
|---|---|---|---|
| **base** | 554.3 555.0 561.5 554.4 553.9 555.8 554.3 554.4 555.5 | **554.4** | — |
| `OCUDU_CE_CORR_REPEAT=2` | 592.5 589.4 591.2 589.1 589.3 588.9 591.0 591.2 602.5 | 591.0 | **+36.6** |
| `OCUDU_CE_INV_REPEAT=3` | 953.5 950.2 949.7 951.0 952.7 952.8 951.0 949.2 952.9 | 951.0 | **+396.6（2 次 ⇒ 198.3/次）** |
| `OCUDU_CE_W_REPEAT=2` | 617.8 615.0 613.8 613.9 614.8 613.4 614.2 612.4 615.5 | 614.2 | **+59.8** |
| `OCUDU_CE_REFORMAT_REPEAT=2` | 648.8 650.9 651.9 657.6 651.1 650.9 669.3 649.1 662.1 | 651.1 | **+96.7** |
| `OCUDU_CE_DEV_TA=0` | 497.9 497.7 495.9 498.3 496.0 502.3 **648.8** 498.8 497.9 | 497.9 | **−56.5** |

| 段 | µs | 占 `ch_wt` |
|---|---|---|
| **K1 求逆** | **198.3** | **35.8%** |
| 重排（`encode_reformat` 全部：K3 + rSRP + 噪声方差 + TA 链）| 96.7 | 17.4% |
| 　其中 TA 链（`DEV_TA=0`）| 56.5 | 10.2% |
| K2 权重 | 59.8 | 10.8% |
| 相关矩阵前缀（K0-d：A + R_hp 一趟）| 36.6 | 6.6% |
| **已量出的合计** | **391.4** | **70.6%** |
| 未归属（y scatter、barrier、编码/发射开销）| 163.0 | 29.4% |

**头条：最大单项是 K1 求逆（~198 µs，36%），不是相关矩阵前缀（~37 µs，6.6%）。**
这正好把 §5.8.13/§5.8.14 作废的那张 bisection 表的两个数分开判了：
`CORR_DEV=0 −445` 是噪声（真值 37）；`INV_RL −153` 方向是对的（真值 ~198，同一量级）。

### 4.1 保值性复验（每个臂与 base 逐字节比）

| 臂 | `_h.bin`+`_llr.bin`+捕获 | `_ce.txt` |
|---|---|---|
| `CORR_REPEAT=2` | **0** | 0 |
| `INV_REPEAT=3` | **0** | **7**（`noise_variance` 1.258405298e-01→1.258405447e-01、`rsrp` 1.506316196e-02→1.506313402e-02，末位）|
| `W_REPEAT=2` | **0** | 0 |
| `REFORMAT_REPEAT=2` | **0** | 0 |
| `DEV_TA=0` | **0** | 0 |

---

## 5. ★★ 顺带发现的缺陷：`OCUDU_CE_INV_REPEAT` **不是保值的**

`mmse_inv`（K1）是**原地** Gauss-Jordan：每多编一次就在 `A` 与 `A⁻¹` 之间翻一次面。所以

```
INV_REPEAT=2  h.bin diff=2688  llr.bin diff=914  ce.txt diff=34     <-- 数值全错
INV_REPEAT=3  h.bin diff=   0  llr.bin diff=  0  ce.txt diff= 7     <-- 数据逐位同，两个标量末位
INV_REPEAT=4  h.bin diff=2688  llr.bin diff=914  ce.txt diff=32     <-- 数值全错
INV_REPEAT=5  h.bin diff=   0  llr.bin diff=  0  ce.txt diff= 8
```

`INV_REPEAT=2` 的 `_ce.txt`：`noise_variance` **1.258e-01 → 4.601e+03**、`rsrp` **1.506e-02 → 4.548e+03**。

- **偶数次是错的**（把 A 反了回来）；
- 奇数次（≥3）数据逐位相同，但**两个发布标量差 ~1e-7 相对**（往返不是逐位可逆）；
- 上一会话"n=4 逐位不变"的验证**不成立**（n=4 正是错的那一侧），所以
  §5.8.14 里"每一段都保值、已实测"这句话**对 `_INV_` 是错的**；
- **任何用偶数 `INV_REPEAT` 读到的数都是无效的**（包括本会话第一版表里的 `INV_REPEAT=2`）。

**正确的用法（本次采用）**：`INV_REPEAT=3`（或任何 ≥3 的奇数），并把 `_ce.txt` 的 7 字节差
当成"已知且已解释"；**别用 2**。要一个真正干净的求逆探针，得让重复**恢复原状**
（例如 K1 编两次算一次"重复"，或先把 A 存一份）——那是下一次的候选改动。

---

## 6. 复跑清单

```bash
# 设备能力（§1）
xcrun clang++ -std=c++17 -fobjc-arc -framework Metal -framework Foundation \
  -o /tmp/metal_counter_caps doc_chinese/phy_pipeline_gpu/wip/metal_counter_caps.mm && /tmp/metal_counter_caps

# 等待点（§2）——必须用单测，回放工具不走延迟路线
OCUDU_CE_WAIT_TRACE=1 ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test 2>&1 | grep -o 'ce_wait\] [a-z_ :,]*' | sort | uniq -c | sort -rn

# 稳定性协议（§3）：--repeat 1，N=9，读中位数
for i in $(seq 1 9); do
  ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay doc_chinese/work_tmp/corpus/syn004_4 --metal --out /tmp/b 2>&1 | grep -o 'ch_wt=[0-9.]*'
done

# 分段（§4）：把上面那条加上 env 前缀，逐臂替换
OCUDU_CE_CORR_REPEAT=2 / OCUDU_CE_INV_REPEAT=3 / OCUDU_CE_W_REPEAT=2 / OCUDU_CE_REFORMAT_REPEAT=2 / OCUDU_CE_DEV_TA=0

# 门（本会话全过）
bash doc_chinese/phy_pipeline_gpu/wip/neutral_vs_baseline.sh      # 235/235、0 字节
ctest --test-dir build -R metal                                    # 9/9
```

---

## 7. 交叉验证没做成：`OCUDU_CE_INVERT_FIRST=1` 现在产出 NaN（第二个缺陷）

想用树里现成的"把 K1 放进独立命令缓冲"旋钮独立验证 §4 的 198 µs：

```
OCUDU_CE_INVERT_FIRST=1 → [ul_gpu_lane] lanes=1 cbs/lane=4.00 (max=4)
   busy split: ch_est=305.6us/lane (46%, cbs/lane=2.00) ch_wt=246.4us/lane (37%) eq_demap=107.0us/lane (16%)
```

读数很漂亮（`ch_wt` 554.4 → 246.4），**但值不能用**：

```
_h.bin diff=2688   _llr.bin diff=914   ce.txt diff=72
port=0 noise_variance=nan snr=0.000000 rsrp=nan epre=1.454060674e-01 ta_us=-1.171875 cfo_hz=na
```

原因（代码顺序）：`engine_run()` 里

```cpp
if (gpu_invert && (std::getenv("OCUDU_CE_INVERT_FIRST") != nullptr)) {
    if (!engine->invert(gpu_a, L, nof_systems)) { ... }     // <-- 这时 A 还没被建出来
}
const bool k1_inline = gpu_invert && (std::getenv("OCUDU_CE_INVERT_FIRST") == nullptr);
```

相关矩阵前缀（K0-d）是 P1 时代**搬进 `encode_run()`** 的，也就是说 `A` 是在那次 `invert()` **之后**
才被写进 `gpu_a` 的 ⇒ 它反的是上一跳/垃圾矩阵，而 `k1_inline` 又被关掉 ⇒ 权重拿到 NaN 而不是 `A⁻¹`。
**⇒ 这个旋钮在"设备建相关矩阵"的路由上已经坏了**（注释仍写着它是可用的 TEMPORARY experiment）。

⇒ **198 µs 只有重复探针一个来源**；交叉验证要等这个旋钮修好（未做）。

---

## 8. K1 画像（§5.8.16 的原始数据）

协议：`--repeat 1`、一个进程一跳、N=7–9、取中位数。**空口几何 = 25 PRB，所以离线该用 `syn027_25`。**

**K1 = Δ(ch_wt) / 2，臂 `OCUDU_CE_INV_REPEAT=3`：**

| 捕获 | 分配 | n | base | INV_REPEAT=3 | K1 |
|---|---|---|---|---|---|
| `narrow_cap/cap_3139_17922` | 1 PRB | 18 | 291.0 | 371.8 | **40.4** |
| `narrow_cap/cap_3322_17922` | 2 PRB | 36 | 360.1 | 561.8 | **100.9** |
| `corpus/syn004_4` | 4 PRB | 54 | 556.8 | 951.4 | **197.3** |
| `corpus/syn013_10` | 10 PRB | 54 | 569.2 | 965.8 | **198.3** |
| `corpus/syn027_25` | 25 PRB | 54 | 629.4 | 1024.3 | **197.5** |

（n 的由来：块固定 3 PRB，`n = npt(3) × block_prb × comb(6)`；1 PRB 与 2 PRB 的跳没有标准块，
整跳走 tail ⇒ n = 3×1×6 = 18 / 3×2×6 = 36。`dmrs_symbols=2,7,11`、`cdm_groups_without_data=2`。）

**几何 sweep（`syn004_4`，每格 9 次中位）**：见 §5.8.16 ②。保值性：7 种几何 × 4 dump = **0 字节**。

**两个否证：**

1. **2→1 barrier/pivot（逐位保值）反而更慢**，两次尝试都回退：
   `syn004_4` base 556.8 → 606.7（无 `tid.y==0` 保护）→ 581.6（加保护）；
   `syn027_25` base 629.4 → 677.9 → 649.1。K1 197.3 → 216.2 / 197.5 → 217.6。
2. **K1b（`OCUDU_INV_RL=1`，~14 barriers）比 K1（~122 barriers）贵 2.1 倍**：
   `syn004_4`，`OCUDU_INV_RL=1` base = 765.0、`+INV_REPEAT=3` = 1609.1 ⇒ K1b = **422.1 µs**。

**结构事实**：`mmse_inv` grid = `MTLSizeMake(nof_systems,1,1)`；调用点传 `2 * nof_layers`
⇒ 单层 UE **2 个线程组**。**固定 n 扫 `nof_systems`** 是下一步最省的实验。

**复跑：**

```bash
BIN=./build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CAP=doc_chinese/work_tmp/corpus/syn027_25          # 空口几何
for i in $(seq 1 9); do $BIN $CAP --metal --out /tmp/a 2>&1 | grep -o 'ch_wt=[0-9.]*'; done
for i in $(seq 1 9); do OCUDU_CE_INV_REPEAT=3 $BIN $CAP --metal --out /tmp/b 2>&1 | grep -o 'ch_wt=[0-9.]*'; done
# K1 = (median_b - median_a) / 2

# 几何 sweep：加 OCUDU_INV_TGX=… OCUDU_INV_TGY=…
# K1b：OCUDU_INV_RL=1（数值是错的，只用它的时间）
```

---

## 9. K1 隔离台架（`k1_check`）——§5.8.16 ⑥⑦ 的原始数据

`test/k1_check.cpp` 的**默认模式**（不带参数）已经同时扫 n 与 `nof_systems`，
读 `last_gpu_wait_us()`（`invert()` 自己那条命令缓冲的 `GPUEndTime − GPUStartTime`，纯 GPU 时间）。
它没有进默认构建，要显式建：

```bash
cmake --build build --target k1_check -j 10
./build/lib/phy/upper/signal_processors/channel_estimator/metal/k1_check
```

**结果（µs/call，每格 20 次均值）：**

| n | 1 system | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| **18** | 55.0 | 52.3 | 49.6 | 50.4 | 49.8 |
| **54** | **202.8** | 208.9 | 207.5 | 202.8 | **203.4** |

* **`nof_systems` 1→8 完全免费** ⇒ 并行度不是瓶颈，**一个线程组的临界路径才是**；
* **交叉验证**：隔离台架 n=54 = **203 µs**，真实跳里 `INV_REPEAT=3` 的斜率 = **197.3 µs** ⇒
  两套独立仪器差 **3%**；
* 成本随 n 超线性：18→54（3×）涨 **4.06×** ⇒ 约 **n^1.27**；折算 **≈3.7 µs/pivot**（n=54）；
* 数值仍好：n=54 `max_rel=1.324e-06`、bad=0/2916（n=18：5.628e-07、0/324）——即这个工具
  在计时之外仍然守着它原来的数值判据。

**⇒ 结论**：在"逐元素运算次序不变"（235/235 逐字节）的约束下，K1 的临界路径
（逐 pivot 的串行依赖 + 每 pivot 一道必需的 barrier）**无法再缩短**；
要真砍掉它必须**改数值契约**（换算法），那是用户裁定的问题。见 §5.8.16 ⑦。

---

## 10. `ch_wt` 分段重做（空口几何 `syn027_25`，25 PRB）——§5.8.17 的原始数据

协议：`--repeat 1`、一个进程一跳、**N=7 取中位数**。**每个臂都复验了保值性**（同捕获，四个 dump）。

**保值性：**

| 臂 | 差字节 |
|---|---|
| `CORR_REPEAT=2` / `W_REPEAT=2` / `REFORMAT_REPEAT=2` / `DEV_TA=0` | **0** |
| `INV_REPEAT=3` | 5 |
| `CPU_LS=1` | 24（`_h.bin` 14 + `_ce.txt` 10）|
| `DEV_Y=0` | **21703**（`_h.bin` 16772）—— 不是外科臂：`rsrp=0 ta_us=0` |

**读数（中位 `ch_wt`，µs）：**

| 臂 | 中位 | Δ | 含义 |
|---|---|---|---|
| base | **627.0** | — | |
| `CPU_LS=1` | **464.3** | **−162.7** | **导频抽取（K0-a）** |
| `INV_REPEAT=3` | — | +395 / 2 = **+197.5** | K1 |
| `REFORMAT_REPEAT=2` | — | **+116.9** | 重排一趟 |
| `DEV_TA=0` | 568.6（base 648.7 那次）| **−80.1** | TA 链 |
| `W_REPEAT=2` | — | **+35.6** | K2 |
| `CORR_REPEAT=2` | — | **+35.3** | K0-d 一趟（std+edge）|

**分解闭合（`CPU_INVERT=1`，`cbs/lane=4.00`）：**
`ch_est=196.5（2 cb）` + `ch_wt=252.9` + K1 ≈ 659 ≈ base 的 `ch_wt`。

**两个旋钮陷阱：**

1. `OCUDU_CE_DEV_INVERT` **不存在**（真名 `OCUDU_CE_GPU_INVERT` / `OCUDU_CE_CPU_INVERT`）——
   拼错的旋钮是静默空操作，测出来 Δ=−5.0 µs 差点被当成结论。
2. `OCUDU_CE_DEV_Y=0` 会把设备 rSRP/TA 一起关掉 ⇒ **不能当"y scatter 成本"用**。

**复跑：**

```bash
BIN=./build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CAP=doc_chinese/work_tmp/corpus/syn027_25
med() { sort -g | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
for i in $(seq 1 7); do $BIN $CAP --metal --out /tmp/a 2>&1 | grep -o 'ch_wt=[0-9.]*' | cut -d= -f2; done | med
for i in $(seq 1 7); do OCUDU_CE_CPU_LS=1 $BIN $CAP --metal --out /tmp/b 2>&1 | grep -o 'ch_wt=[0-9.]*' | cut -d= -f2; done | med
```

---

## 11. 导频抽取逐 dispatch 拆开（§5.8.18 的原始数据）

**仪表**：`OCUDU_CE_{LSE,CFO,SMOOTH,SIGMA2,POWER,EPRE}_REPEAT`（同一个 dispatch 编 N 次）。
保值性：**六个全部 0 字节**（同捕获四 dump 逐字节比）。`apply_cfo` 无旋钮（**原地**旋转，编两次转两次）。

**★ 方法修正（比数字重要）**：`OCUDU_CE_POWER_REPEAT=2` 那一臂 **7 次的中位数 = 247.8 µs**
（整臂落在低模式），几分钟后单跑 = **647.0 µs**。⇒ **低模式按分钟成段出现**，
"先 base 后各臂再比中位数"是错的。**必须 base/臂逐次交替配对，报每对差值的中位数。**

**配对读数（`syn027_25`，N=7，单位 µs）**

| 臂 | Δ 中位 | base 中位 | 臂 中位 |
|---|---|---|---|
| `LSE_REPEAT=2` | **+1.5** | 650.3 | 651.8 |
| `CFO_REPEAT=2` | **+72.8** | 626.6 | 706.5 |
| `SMOOTH_REPEAT=2` | **+27.5** | 639.3 | 663.3 |
| `SIGMA2_REPEAT=2` | **+46.6** | 634.9 | 684.9 |
| `POWER_REPEAT=2` | −5.2 | 656.3 | 651.1 |
| `EPRE_REPEAT=2` | **+25.2** | 644.7 | 648.5 |
| `CPU_LS=1` | **−171.3** | 644.2 | 474.6 |
| `INV_REPEAT=3` | **402.6**（⇒ K1 = 201.3）| 653.9 | 1048.1 |

六个之和 **173.6** vs 整段 **171.3** ⇒ **闭合差 1.3%**。

**并行度（读代码）**：`lse` 全 grid（450×3，64/组）· **`cfo` 单线程**（`if (tid != 0) return`）·
`apply_cfo` 全 grid · `smooth` 3 组 × 128 · `sigma2`/`power`/`epre` 各 **1 组 × 256**（`red[256]` 树）。

**`mmse_pilots_cfo` 为什么必须是单线程**：它的注释（原文见设计文档 §5.8.18 ④）说明其答案是
一次 **72 项 atan2 串行累加**，而**那个累加的编译形态本身决定发布位**——批 5g 实测过一次：
epoch 的算法一挪，累加被重编译，CFO 移 1 ulp，翻转了发布网格的单精度值。

**复跑：**

```bash
BIN=./build/lib/phy/upper/channel_processors/metal/ul_chain_replay
CAP=doc_chinese/work_tmp/corpus/syn027_25
med() { sort -g | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
for i in $(seq 1 7); do
  b=$($BIN $CAP --metal --out /tmp/p 2>&1 | grep -o 'ch_wt=[0-9.]*' | cut -d= -f2)
  a=$(OCUDU_CE_CFO_REPEAT=2 $BIN $CAP --metal --out /tmp/p 2>&1 | grep -o 'ch_wt=[0-9.]*' | cut -d= -f2)
  echo "$a $b" | awk '{printf "%.1f\n", $1-$2}'
done | med        # => ~72.8
```

---

## 12. 新门之下的第一个改动（§5.8.20）：CFO 并行化 + **双峰根因 = 宿主 GUI 抢 GPU**

**改动**：`mmse_pilots_cfo` 从 `if (tid != 0) return;`（单线程、~600 次串行 device 访存）改成
线程组内分 stride 部分和 + 树形合并，只有 thread 0 做 `atan2`。算法不变，求和次序变。

**新门**：`python3 wip/value_net.py` → **47 捕获 0 问题**；`--self-test` 8/8；`ctest -R metal` 9/9。
**逐字节（现在是信息）**：235 文件里 93 字节不同 = 18 个窄捕获的 `_ce.txt`，**只动 `noise_variance`**
（6.468808651e-01 → 6.468809247e-01，相对 9.2e-8 ≈ 1.5 ULP）；**`_h.bin`/`_llr.bin`/`.bin` 全 0 字节**。

**收益：抽取 171.3 → ~152 µs（−11%）**，两条模式无关的路子互证：

| 路子 | 读数 |
|---|---|
| 配对 `CPU_LS=1` | **−152.6**（旧 −171.3）|
| 比值 抽取/(2×K1) | **0.3778**（旧 0.4255）⇒ 171.3 × 0.888 = **152.1** |

**⚠ 重复探针的口径**：`CFO_REPEAT=2` 斜率 72.8 → **23.7**，与整段 −19 对不上 ⇒
**它是"边际成本（上界）"，不是"份额"**；第一份的一部分能被前后 dispatch 藏住。

**★★ 双峰根因（本会话最终定案）**：空臂（base vs base，N=9）宽度 **−248.8 … +319.8 µs**，
与任何臂的效应同宽 ⇒ 该时段什么都测不出。进程表：

```
Google Chrome (GPU process)  37–44% CPU
WindowServer                 32.1% CPU
```

⇒ **macOS 在进程间分时 GPU，宿主自己的 GUI 就是干扰源。**
推论：① §4.1 那张撤掉的 bisection（4× 散布）与 §5.8.15 的"双峰"主因在此，不在"回放节拍"；
② **绝对值模式相关、比值不相关** ⇒ 只报份额，绝对 µs 要声明时段；
③ **跑 A/B 前先跑空臂**，空臂宽度 ≥ 效应 ⇒ 读数作废；④ 要绝对 µs 就把 GUI 关掉或用空口腿。

**复跑：**

```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py            # 门
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 门自证看得见失败
# 空臂（判据自证）：
for i in $(seq 1 9); do
  b=$(./build/lib/phy/upper/channel_processors/metal/ul_chain_replay doc_chinese/work_tmp/corpus/syn027_25 --metal --out /tmp/n 2>&1 | grep -o 'ch_wt=[0-9.]*' | cut -d= -f2)
  a=$(./build/lib/phy/upper/channel_processors/metal/ul_chain_replay doc_chinese/work_tmp/corpus/syn027_25 --metal --out /tmp/n 2>&1 | grep -o 'ch_wt=[0-9.]*' | cut -d= -f2)
  echo "$a $b" | awk '{printf "%.1f\n", $1-$2}'
done
```

---

## 13. SIMD 归约（§5.8.21）：抽取 171.3 → ~118 µs（−31%）

**改动**：`mmse_pilots_sigma2` / `mmse_pilots_epre` 的 256 项 threadgroup 树（每级一道 barrier，
sigma2 每个 layer pair 重来 ⇒ 10~20 道/kernel）改成 `simd_sum()`（寄存器内合并，零 barrier）+
一道 barrier 合并 8 个 SIMD 组部分和。`red[]` 从 256 项缩到 8 项。

**收益（配对 + 比值两法，5% 内一致）**

| 阶段 | 抽取 | 抽取/(2×K1) |
|---|---|---|
| 起点 | 171.3 µs | 0.4255 |
| +CFO 并行化 | 152.6 | 0.3778 |
| **+SIMD 归约** | **117.7**（配对，−158.5…−97.7）/ 123.7（比值）| **0.3072** |

**逐字节信息（全 46 捕获合计）**：`_llr.bin` **0 字节**、`_h.bin` **28 字节**、`_ce.txt` 24 个捕获一个标量。
**新门：47 捕获 0 问题；`--self-test` 8/8；`ctest -R metal` 9/9。**

**复跑**：
```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py           # 门
N=9 /tmp/ratio.sh    # 比值法：抽取/(2*K1)（脚本见 §12 的说明）
```

---

## 14. K1 的 barrier 斜率（空口腿对，§5.8.24）

| 腿 | `OCUDU_CE_INV_BARRIERS` | `ch_wt` | `eq_demap` | CRC | RTF |
|---|---|---|---|---|---|
| `s14-k1-bar0_0920_1830` | 关 | **329.7** | 68.1 | 13536/1812 | 1 |
| `s14-k1-bar8_0920_1831` | 8 | **464.6** | 76.7 | 13400/2247 | 0 |

探针每跳给 K1 加 **8 × 54 = 432 道** barrier（保值）。

* **原始斜率** `(464.6 − 329.7)/432` = **0.312 µs/道**；
* **`eq_demap` 是不含 K1 的对照通道**，它 +12.6% ⇒ 负载校正后探针自身 **+93.4 µs** ⇒ **0.216 µs/道**；
* ⇒ **空口 0.22–0.31**，与离线同一区块内的 **0.29–0.30** 一致 ⇒ 可信。

**⇒ K1 的 122 道 = 27–38 µs/跳 = `ch_wt` 的 8–12%；K1 总量（按 31.5% 份额折算）≈104 µs
⇒ barrier 只占 K1 的 26–37%，逐元素工作占 63–74%（≈66–77 µs）。**

**⇒ 靶子是逐元素工作，不是 barrier。** Newton–Schulz（30× 算术）与"少 barrier 同工作"都不做。
方向：**多 dispatch 的分块求逆**（一个 block column 一次 dispatch，矩阵进设备内存，
grid 铺满 GPU），目标 K1 ~104 → ~35–50 µs（未实测）。

**复跑**：`bash wip/k1_barrier_slope.sh <bar0.log> <bar8.log>`（注意读 `busy split` 要取 `.log.stderr`）。
