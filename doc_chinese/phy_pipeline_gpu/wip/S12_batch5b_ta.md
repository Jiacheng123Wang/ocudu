# S12 — 批次 5b：把 TA（时间对齐）搬到设备

> 工作文档。判据、批次状态在常驻设计文档 `gpu_phy_pipeline_design_and_implementation.md`；
> 本文只记**做法、取舍、进度、踩过的坑**。

---

## 1. 为什么做 / 边界

**根因**：宿主每跳回读整个 DM-RS 网格（`pending_fill::fill()`），只为了算**上报量**
（rsrp / noise_variance / ta）。5a 已经把 **rsrp** 搬到设备并接进发布路径；
`noise_variance` 走的是设备 K4（`gpu_nv`）；**剩下 `ta_us` 还在读回读网格**。

**5b 做完** ⇒ `OCUDU_CE_HOST_GRID` 默认改 0，读侧落到 **0.10/跳**（离线实测）。

**边界**：TA 是**上报量**（喂链路自适应 / 定时提前），**不进 LLR 路径** ⇒
搬到设备对车道零代价（与 5a 同理）。

---

## 2. 宿主算法（读 `time_alignment_estimator_dft_impl.cpp` 的结论）

历史交接说"236 行 IDFT + 峰拟合，设备无对应物"。**读完发现不是**：
236 行里大部分是**宿主 DFT 脚手架**（每个 2 的幂一个 processor、输入缓冲管理）。
**算法本体只有三段**：

```
输入：每个 DM-RS 符号、每层的导频估计（comb 上的 cf_t）

1) 写进一个补零的 IDFT 输入，长度 = get_idft(所需 RE 数) 选的 2 的幂
2) 对每个符号做 IDFT → correlation += |idft_out|²        ← 功率延迟谱
3) 在半 CP 窗口内找峰：
     延迟侧峰 = max(correlation.first(max_ta_samples))
     超前侧峰 = max(correlation.last(max_ta_samples))    ← 注意是环形尾部
     取较大者 → idx
     小数部分 = curve_fitting_fractional_max(峰周围 5 个点)   ← 抛物线插值
   t_align = (idx + fractional) / sampling_rate_Hz
```

**关键参数**（`estimate_ta_correlation`）：

* `sampling_rate_Hz = correlation.size() * scs_khz * 1000 * stride`；
* `max_ta_samples = floor(半CP时长 × sampling_rate_Hz)`（半 CP = `kappa(144) / 2^(μ+1)`）；
* **`stride` 按 DM-RS pattern 选**（`estimate_time_alignment()`）：
  * 全 1（PUCCH fmt 1/3/4）→ `stride = 1`
  * `re_pattern_pusch_0/1`（comb-2）→ `stride = 2`
  * `re_pattern_pucch_f2`（comb-4：`0b010010010010`）→ `stride = 3`
* **小数插值只在 `correlation.size() != max_dft_size` 时做**——即 IDFT 撑满时整数分辨率就够。

**IDFT 尺寸**（`get_idft`）：
`nof_required_re = 所需RE数 × max_dft_size / max_nof_re` → 向上取 2 的幂，下限 `min_dft_size`。

**⚠ 更正（2026-09-19，接线时才发现）**：`min_dft_size` = **128**，不是 2048。
它由 `1 / (15000 × from_timing_advance(1, kHz15))` 决定（1024·Tc 的 TA 步长 ⇒ 125 ⇒ pow2(7)=128），
所以 §4.5 实测的"4 PRB → 128"是对的，而本文早先写的"下限 2048"是**读错了**。
设备侧现在**不自己算**这个尺寸：走新接口 `time_alignment_estimator::get_idft_size(nof_re)`
（宿主实现里 `get_idft()` 与它共用同一段定尺寸代码）。

---

## 3. 设备侧可行性

**设备已经有 IDFT 引擎**：`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.{h,mm}`，
`init(size, inverse=true)` + `run(in, out, nof_transforms)`，车道在用它做 Rx DFT。
⇒ **不需要新写 IDFT**，只需要一个"谱 → 峰"的归约 kernel。

**输入在设备上已存在**：`gpu_ls_out`（K0-a 的 LSE，`rx · conj(ref)`），
5a 的 K5 已经在读它旁边的 `h`。TA 用的是**滤波后的** LSE（宿主 `filtered_pilots_lse`），
与设备 `gpu_ls_out` 的关系需要先核（§4 第一步）。

---

## 4. 判据（**先立判据再动手**，§1.4/§1.6）

| 层级 | 判据 |
|---|---|
| **第一步（算法级）** | 设备 TA **== 宿主 TA**，在同一份输入上，多种几何（PUSCH comb-2 / PUCCH fmt2 comb-4）× 多个真值时延；容差 = 一个时延分辨率（`1/sampling_rate`）|
| **第二步（数据级）** | `_llr.bin` / `_h.bin` / `.bin` **逐字节不变**（TA 不进 LLR，应当如此）|
| **第三步（空口）** | 上行 TA 上报**无跳变**；`host device data crossings` 从 1.39 → ~0.06 读/跳 |

**验证场**：离线回放 harness 已经打印 `ta_us`（`_ce.txt`）⇒ 可直接 A/B：
`OCUDU_CE_DEV_STATS=1`（设备 TA）vs `=0`（宿主 TA）。

---

## 4.5 ★ 实测常量（**从真实调用路径打印出来的，不是推算的**）

在 `time_alignment_estimator_dft_impl.cpp::estimate_ta_correlation()` 里临时打印
（`OCUDU_TA_CHECK=1`，跑完已移除），跑 `ul_chain_replay`：

```
syn004_4 (4 PRB 上行): profile=128 stride=2 scs_khz=30 mu=1 half_cp=1.1719us
                       sampling=7.6800 MHz  max_ta_samples=9
syn025_25 (25 PRB)   : profile=256 stride=2 scs_khz=30 mu=1 half_cp=1.1719us
                       sampling=15.3600 MHz max_ta_samples=18
```

**这些数字是设备侧必须复现的**：

| 量 | 值 / 公式 |
|---|---|
| **SCS** | **30 kHz（μ=1）** ← 空口就是它。**推算时用 15 kHz 会把每个常量都搞错** |
| 半 CP | `144κ / 2^(μ+1)` = **1.1719 µs**（μ=1）|
| PUSCH 导频间距 | **stride = 2**（comb-2，每 PRB 6 个导频）；PUCCH fmt2 是 stride = 3 |
| 每 slice 导频数 | `nof_prb × 6`（PUSCH comb-2）|
| IDFT 尺寸 | `max(min_dft_size(2048), pow2ceil(nof_re × 4096 / 3300))`，本例 4PRB→**128**、25PRB→**256** |
| 采样率 | `IDFT尺寸 × scs_khz × 1000 × stride` |
| `max_ta_samples` | `floor(半CP秒数 × 采样率)`，**必须 ≤ IDFT 尺寸**（宿主用 `correlation.first(n)` 切）|
| 小数插值 | 仅当 `IDFT尺寸 != max_dft_size(4096)` 时做（本例都做）|

**⇒ 每跳的设备工作量**：`nof_layers × npt` 次 **128~256 点 IDFT**（实为 4096 点处理器、只填前 N 个），
加一次 |·|² 累加和一个 5 点抛物线拟合。**车道已经在做 4096 点 DFT** ⇒ 规模完全可行。

## 4.6 ★★ 判决性校验已通过（2026-09-19）

**做法改了**（原计划写独立工具 `s12_ta_check`，段错误且脚手架问题多，**已删除**）：
把校验放进**已经在跑 Metal 的单元测试** `port_channel_estimator_metal_mmse_unit_test.cpp`，
在 `main()` 最前面跑。**这是 5a 的教训换来的选择**：不要再造新脚手架。

**两段校验**：

```
S12 [128]  PASS: profile worst rel 3.90e-06, peak host 100  device 100
S12 [256]  PASS: profile worst rel 5.99e-06, peak host 181  device 181
S12 [2048] PASS: profile worst rel 2.52e-05, peak host 1135 device 1135
S12 PASS: the device IDFT reproduces the host power delay profile

  S12 TA true  -400.0 ns | host   -399.00 | device   -400.00 | diff   +1.00 (res 8.14) OK
  ...
  S12 TA true  +400.0 ns | host   +399.00 | device   +400.00 | diff   -1.00 (res 8.14) OK
S12 TA PASS: size=2048 max_ta_samples=144, worst |diff| 4.39 ns over 9 delays
```

* **第一段**：设备 IDFT 的**功率延迟谱**与宿主 IDFT 逐点一致（误差 ~1e-5，float32 底线），
  **峰值 bin 完全相同**（128/256/2048 三个尺寸）。
* **第二段**：**完整设备侧 TA**（IDFT → |·|² 累加 → 半 CP 环形窗口找峰 → 抛物线插值）
  与**宿主估计器**在同样 9 个已知时延上一致，最差 **4.39 ns < 1 个分辨率（8.14 ns）**。

⇒ **5b 的算法级判据达成。** 剩下的全是工程：把这段逻辑放进 kernel 并接进命令缓冲。

**⚠ 一个细节**：设备路径的结果**比宿主更接近真值**（host −399.00 vs device −400.00 @ true −400）。
两者用的是同一个抛物线插值，差异来自宿主对**它自己那份** IDFT 输出的处理——不是缺陷，
但**接进上报路径时要知道两侧不会逐位相同**（与 5a 一样，容差按分辨率给）。

## 4.7 ★★ K6 kernel 已通过与宿主一致（2026-09-19）

**kernel**：`ocudu_mmse_ta.metal` 的 `mmse_ta_profile` —— 谱累加 + 半 CP 环形窗口找峰 +
抛物线插值，输出**秒**。加进 metallib，引擎侧加了正式入口
`mmse_engine::run_ta_profile(slices, size, nof_slices, stride, scs_hz, window, ta_seconds)`。

**判据（同一份谱喂两侧）**：

```
S12 K6 true  -400.0 ns | host   -399.00 | K6   -400.00 | diff   +1.00 (res 8.14) OK
   ... 9 个时延，从 -400 到 +400 ns ...
S12 K6 PASS: size=2048 window=144, worst |diff| 4.39 ns over 9 delays
```

设备侧**比宿主更接近真值**（同一抛物线插值，输入是两份不同的 IDFT 输出）。

### 写 kernel 时踩的四个坑（都已在 kernel 注释里）

| # | 坑 | 症状 | 正解 |
|---|---|---|---|
| 1 | **`profile` 数组按 threadgroup 大小开（256）** | `size=2048` 时**越界读**，搜索在垃圾里"找到"峰 | 按 **最大变换尺寸 4096** 开（`mmse_ta_max_size`）|
| 2 | **有符号 `idx` 打包进 uint 并加 `size` 偏置** | 所有**负时延读回 0** | 偏置用 **window**（不是 size），`tg_idx_biased = idx + window` |
| 3 | **重建公式写成 `size - from_end`** | 正时延全部**镜像**（差 ±1024 tap）| 延迟侧 `idx = delay_idx`；只有超前侧才是 `-(window - advance_idx)` |
| 4 | **`half` 作为变量名** | MSL 编译错（`half` 是内建类型）`nof_half` ||

**外加一个宿主侧的坑**：zero-copy wrap **要求页对齐**，栈上的 4 字节 `float` 会被拒 ⇒
输出槽必须由**引擎持有**（`mmse_engine_impl::ta_out`）。

## 5. 进度

- [x] 读透宿主算法（§2）
- [x] 确认设备有 IDFT 引擎（§3）；TA 的输入 K5 已经在读同一份 `h`
- [x] **实测常量**（§4.5）—— 省掉了后面所有"常量错了"的返工
- [x] **★ 算法级判据通过**（§4.6）：设备 IDFT 谱 == 宿主；完整设备 TA == 宿主 TA
- [x] 独立工具 `s12_ta_check.cpp` **已删除**（改在单元测试里做，见 §7）
- [x] **★ K6 kernel 通过**（§4.7）：与宿主一致，最差 4.39 ns < 1 分辨率
- [x] **★ 接进车道**（§6.5）：K7 + `dft_dit` + K6 三条 dispatch 进 reformat 自己的命令缓冲
- [x] **★ 离线 A/B 通过**：27 捕获 `ta_us` 设备 vs 宿主 **逐位相同**（`wip/ab_ta.sh`）
- [x] **★ 数据不变 A/B 通过**：`_llr.bin`/`_h.bin`/`.bin` 全 0 差异；`_ce.txt` 除 `ta_us` 外逐字节相同
- [x] **★ 真实调用路径判据通过**：单测 32 跳，`[ta_check]` 全部 < 1 分辨率（最差 5.45 ns / 65.10 ns）
- [ ] 翻 `OCUDU_CE_HOST_GRID` 默认 → 0（现在**按跳判定**：设备覆盖到的跳不回读）
- [ ] 空中腿

## 6. ⛔ 接进车道：为什么不是"加两个 dispatch"（**下一轮从这里开始**）

我按"两队列"的思路写完了接线，**在自检时发现它是错的，已撤回**（未留在树里）。
真正的障碍比队列更具体，有三条：

### 6.1 ★ `dft_dit` 的输入必须是**数字反转序**，因此**不能原位跑**

kernel 的取数是 `buf[i] = in[batch_offset + perm[i]]` —— 它要求 `in` **已经**按数字反转序摆好。
这条我最初读漏了，它有两个后果：

1. **摆放导频（零填充 + 放到正确位置）不能在设备上顺手做完**：要么宿主侧组装，
   要么**再写一个摆放 kernel**（读 LSE → 写变换输入）。`dft_dit` 自己不接受 subcarrier 步长。
2. **变换必须"读 staging、写 spectra"两个缓冲**，不能原位 ⇒ 至少 3 个 dispatch
   （摆放 → 变换 → 归约）+ 2 次 barrier。

### 6.2 一个 blit 换不来零等待

我试过"宿主组装 + 一次 blit"，但 `blitCommandEncoder` 只能**自成命令缓冲 commit+wait**，
那就是**每跳一次等待** —— 正好抵消搬 TA 的意义。

### 6.3 队列的问题仍然在

`run_ta_profile()`（已验证的那个入口）在引擎的 **backend** 队列上；DFT 引擎在 **front-end** 队列。
不同队列之间**没有顺序保证**。所以"把 IDFT 交给 DFT 引擎、再调 K6"这条捷径**不成立**。

### 6.4 ⇒ 实际做法（**已完成**，保留下面的原始计划作对照）

**写一个摆放 kernel，三个 dispatch 全编码进车道那条命令缓冲**：

```
K7 (新)  : 读 LSE 的导频 → 按 perm 表写进变换输入缓冲（零填充）
dft_dit  : 读变换输入 → 写 spectra（复用现有 kernel，只传 base/twiddle/perm/radix/inverse）
K6       : 读 spectra → 输出 ta 秒                        ← 已完成并验证
```

* 三条都在**同一编码器**里，靠 encoder 的顺序 + 中间两次 `memoryBarrierWithScope` 保证可见性；
* **零等待**，这才是搬 TA 的意义；
* K7 很小（一次 scatter，和已有的 `mmse_pilots_scatter_y` 同形状）；
* twiddle/perm 表由引擎持有。**我写过的接线尝试留在了
  `wip/S12_engine_wiring_attempt.mm.txt`**（`load_ta_dft_pipeline` / `build_ta_tables` / `encode_ta` /
  `assemble_ta_input` 四个函数，直接可取用）—— 注意表构造必须和 `dft_metal_engine::init` 一致。

**⚠ 别忘**：K6 的 `mmse_ta_tg_size` 是 256，`dft_dit` 的线程组由它自己算（`min(n,1024)`），
两者在同一个编码器里用不同的 `threadsPerThreadgroup` 是正常的。

---

### 6.5 ✅ 接线实际是怎么做的（2026-09-19，与 6.4 的差异都在这里）

1. **K7 的输入改成了 `h`**，不是"另一份 LSE"。宿主 TA 的输入 `filtered_pilots_lse` 在这个后端里
   就是网格（= `h` 的宿主副本）按 comb 采样出来的 ⇒ 两件事：K7 直接读 K5 读的那个 buffer，
   **几何共用**（`hop_geometry` 一个结构体，K5 与 K7 由同一个 `geo` 变量生成参数块）；
2. **K7 现在写满整个 slice**（导频或 0），不再依赖"调用方先清零"——宿主本来就是每次调用清零，
   把这件事放进 kernel 就不再需要每跳一次宿主写；
3. **DC 不跳过**（与 K5 相反）：K5 求和要排除 DC，而 K7 要**复现宿主的输入**，宿主那份输入里
   DC 位置的 comb 值是照抄的；
4. **slice 用的是"本跳的" DM-RS 符号表**（`ta_stage_t::dmrs_slots[]`），不是 `dmrs_sym_bits`
   （那是**时隙**的：频率跳频时本跳只占一部分，用 popcount 会把另一跳的导频也变换进去）；
5. **变换尺寸问宿主**（`get_idft_size`），不自己推公式；
6. **结果槽页对齐**（8 个槽，每个一页），完成时按暂存时定下的槽读回；
7. **回读按跳判定**：`host_grid_wanted = HOST_GRID || !(device_rsrp_valid && device_ta_valid)`。

**判据（全部通过）**：

```
L2 K7 PASS: 3 slices x 2048 positions (72 pilots, 6072 zeros), 0 mismatching
S12 chain PASS: size=2048 window=144, worst |diff| 4.39 ns over 9 delays, 0 misplaced
[ta_check] hop 0: device -70.63 ns host -70.63 ns diff -0.00 (res 65.10) slices=3 pilots=150 stride=2 OK
Test 13: the device time alignment matches the host's own estimate on 32 hops
[ta_stage] size=256 stride=2 scs=30000 window=18 pilots=150 hop=0      ← §4.5 的实测常量逐项吻合
ab_ta.sh: captures=27 dump-differences=0 ce-differences-outside-ta=0 probe-failures=0
          worst |ta_us difference| = 0 us
```

**新增的环境旋钮**：`OCUDU_CE_DEV_TA`（默认 1；0 = 宿主自算，就是 A/B 的另一臂）、
`OCUDU_CE_TA_CHECK`（探针：同一跳上跑宿主估计器并逐跳比对，`[ta_check] total:` 行是判据）。

## 7. ✅ 已解决：校验工具的问题（记录，避免重走）

最初写了一个独立二进制 `test/s12_ta_check.cpp`，**段错误**且连第一行 printf 都不输出；
lldb/atos 只能看到 `main + N`。期间还踩了"**源码改了但二进制不重建**"
（`.o` 缺失、二进制比源码旧，`cmake --build` 报 "Built target" 却没编译）。

**解法不是修它，而是不写它**：把校验放进 `port_channel_estimator_metal_mmse_unit_test.cpp`
（已经在跑 Metal 引擎、构建链路可靠），在 `main()` 最前面跑。**一次就过**（§4.6）。

**⇒ 纪律：新校验优先挂到已有且已验证的 harness 上，不要为它新造一个二进制。**
（5a 的 `OCUDU_CE_RSRP_CHECK` 也是这个形状。）

## 8. 坑（写下来免得重踩）

1. **`observed_max_advance` 读的是 `correlation.last(max_ta_samples)`**——环形缓冲的**尾部**，
   不是头部。写成"前 max_ta_samples 个"会静默改变符号约定。
2. **`idx` 的符号**：`idx = -(max_ta_samples - advance_idx)`，延迟侧赢时取 `delay_idx`。
3. **小数插值条件**：`correlation.size() != max_dft_size` 才插值。
4. **`get_idft` 的尺寸缩放**：`nof_required_re × max_dft_size / max_nof_re`，不是直接用 RE 数。
5. **IDFT 未归一化**（引擎注释写明）——但峰位置与缩放无关，所以不影响 TA；**只影响阈值类判据**。
6. **SCS 是 30 kHz（μ=1），不是 15**。用 15 推算会把半 CP、采样率、`max_ta_samples` 全部搞错，
   并且让 IDFT 尺寸猜错 ⇒ 工厂返回 null ⇒ **静默的空指针**。
7. **`max_ta_samples` 必须 ≤ IDFT 尺寸**（宿主用 `correlation.first(n)` 切）。
8. **构建可能静默不重建**（§6）——改完源码核 mtime 或删 `CMakeFiles/<target>.dir`。
9. **不要为新校验造独立二进制**（§6）：挂到已有 harness 上，一次就过。
10. **设备路径与宿主路径不会逐位相同**（同一个抛物线插值，输入是两份不同的 IDFT 输出）
    ⇒ 容差按**分辨率**（`1/sampling_rate`）给，不是按 ULP。
