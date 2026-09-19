# S12 事故报告：新一轮 GPU 挂死 + 内核终止性审查（2026-09-19）

> **这一节是硬约束，优先级与 `full_gpu_chain/s2_full_chain_design.md` §48.131(c) 同级。**
> 前一次同类事故：`s2_full_chain_design.md` §48.131（2026-09-15，S-7f-5w），
> 本文是**同一条纪律的第二次触犯**，代价同样是**用户强制断电**。

---

## 1. 事故

* **时间**：2026-09-19 下午。
* **触发**：`port_channel_estimator_metal_mmse_unit_test` 运行到新的 S12 chain 用例，
  第一次 `mmse_engine::run_ta_place()` 调用（K7 kernel）**挂住不返回**。
* **后果**：GPU 被孤儿内核占住 → WindowServer 看门狗超时 → **`reboot` 也卡死 → 用户强制断电**。
  （机理与 §48.133 实测结论完全一致：`kill -9` 不释放 GPU；优雅关机路径本身要用 GPU。）
* **代码侧证据**：`[chain] delay -400 ns: placing` 打印后进程僵死；
  在 `run_ta_place()` 的 `[cb waitUntilCompleted]` 上不再返回。

**⚠ 与 §48.133 第 5 条一致**：`recoveryCount` 不会自己涨，**macOS 不复位 GPU**，只能重启。

---

## 2. 根因：**我又违反了 §48.131(c) 的三条硬约束**

§48.131(c) 原文（**"任何 GPU 内核都必须满足"**）：

1. 内核必须对**【任意】参数值都终止**：所有循环上界取**编译期常量**，参数只用于**提前退出/跳过**；
2. **所有下标都必须被编译期上界夹住**（读和写都一样）；
3. **线程组大小是编译期常量**，内核里**不再读 `[[threads_per_threadgroup]]`**。

### 2.1 K7（`mmse_ta_place`）—— **三条全违反**

```metal
const uint nof_pilots = (p.nof_pilots < p.size) ? p.nof_pilots : p.size;
const uint slice_stride_in  = nof_pilots * 2u;
const uint slice_stride_out = p.size * 2u;
for (uint slice = 0; slice != p.nof_slices; ++slice) {      // ← 违反 1：上界是参数
  device const float* src = lse + slice * slice_stride_in;  // ← 违反 2：下标未夹
  device float*       out = dst + slice * slice_stride_out; // ← 违反 2：写下标未夹
  for (uint j = tid; j != nof_pilots; j += tg_size) {       // ← 违反 1（nof_pilots 是参数）
    const uint pos = perm[j];                               // ← 违反 2：读下标未夹
    out[2u * pos]      = src[2u * j];                       // ← 违反 2：写下标未夹
    out[2u * pos + 1u] = src[2u * j + 1u];
  }
}
```

**这个 kernel 为什么能挂死（机制）**：`p.nof_slices == 0` ⇒ **外层是死循环**（`slice != 0` 永不成立）。
按纪律，**"参数错 ⇒ 不终止"这个结构本身就是缺陷**，无论这次参数是不是真的错了。

**⚠ 但"参数确实错了"这一步我没有证据，不要当成结论**：我核对了参数结构体的两侧定义
（MSL 的 `struct mmse_ta_place_params { uint size; uint nof_pilots; uint nof_slices; uint pad0; }`
与宿主的 `struct { uint32_t …; } p{dft_size, nof_pilots, nof_slices, 0u}`），**字段顺序与宽度一致**，
所以"字段错位"这个猜测**在我自己的代码里不成立**。挂死的**确切**触发条件**尚未定位** ——
它可能是参数、可能是越界读写（本 kernel 有 5 处未夹的下标，见上），也可能是别的东西。

**⇒ 结论不受影响**：**不夹参数、不夹下标、循环上界取参数的 kernel 不允许存在**，
因为它的失败模式是"整机死"而不是"结果错"。**先按 §3 重写，再用 L1/L2 阶梯找真相。**

### 2.2 K6（`mmse_ta_profile`）—— 违反 1 和 3

| 位置 | 违规 |
|---|---|
| `for (uint tap = tid; tap < p.size; tap += mmse_ta_tg_size)` | 违反 1（`p.size`）|
| `for (uint s = 0; s != p.nof_slices; ++s)` | 违反 1 |
| `for (uint i = 0u; i != p.nof_taps; ++i)` | 违反 1 |
| `uint tid [[...]], uint tid [[threads_per_threadgroup]]` （在 K7 里）| 违反 3 |
| `profile[tap]`、`profile[p.size - window + tap]` | 违反 2（`p.size` 未夹；即使 `p.size > 4096` 也越界）|

**K6 侥幸通过测试**：它只被 `run_ta_profile()` 用**我写死的**参数调用过（size=2048、slices=3），
参数从来没错过。**这不是"写对了"，是"还没踩到"。**

### 2.3 K5（`mmse_rsrp`）—— 违反 1（**已上线，必须先报告**）

```metal
for (uint sym = 0; sym != p.nof_symbols; ++sym) {   // ← 违反 1：上界是参数
```

K5 **已经通过 7 种语料判据、跑过空中腿**（腿 `5a-pubpath_0919_1230`）——它在实践中是好的，
但按纪律它**不满足"对任意参数都终止"**。这是**既有技术债**，不是本轮引入的，
但它和 K6/K7 是同一个结构，**应当一起修**（改法见 §3.3）。

---

## 3. 修法（照 §48.131(c) 与 §48.132(a) 的样板）

### 3.1 K7 重写要点

```metal
constant uint mmse_ta_max_size    = 4096;  // 与 dft_metal_engine::max_size 同一批常量
constant uint mmse_ta_max_slices  = 8;     // MAX_DMRS_SYMBOLS(4) * MAX_LAYERS(4)
constant uint mmse_ta_place_tg    = 256;   // 编译期线程组大小，不再读 [[threads_per_threadgroup]]

kernel void mmse_ta_place(...)
{
  // 参数先"关进"常量范围，后面所有循环用常量上界、参数只 break
  const uint size    = (p.size <= mmse_ta_max_size)   ? p.size   : 0u;   // 非法 ⇒ 什么都不做
  const uint pilots  = (p.nof_pilots <= size)         ? p.nof_pilots : 0u;
  const uint slices  = (p.nof_slices <= mmse_ta_max_slices) ? p.nof_slices : 0u;
  if (size == 0u) { return; }                    // 早退在**任何 barrier 之前**（本 kernel 无 barrier）
  for (uint slice = 0; slice != mmse_ta_max_slices; ++slice) {
    if (slice >= slices) break;                  // 参数只用来 break
    ...
    for (uint j = tid; j != mmse_ta_max_size; j += mmse_ta_place_tg) {
      if (j >= pilots) break;
      const uint pos = perm[j];                  // perm 长度是 mmse_ta_max_size ⇒ pos < 常量
      ...
    }
  }
}
```

* **参数非法 ⇒ 内核什么都不做**（结果错，可见），**绝不**不终止；
* `perm` 的读取下标 `j < mmse_ta_max_size`，**被常量夹住**；
* 写下标 `2u * pos` 里 `pos = perm[j]`，而 `perm` 是宿主按 `size ≤ 4096` 造的 ⇒ 需在**宿主侧**
  保证 `perm` 表长 ≥ `mmse_ta_max_size`（或在内核里再夹一次 `pos < size`）。

### 3.2 K6 重写要点

* `p.size` 夹进 `mmse_ta_max_size`；`p.nof_slices` 夹进 `mmse_ta_max_slices`；`nof_taps` 夹成 ∈ {3,5}；
* 三个循环改成"常量上界 + 参数 break"；
* `window` 已经是 `min(p.max_ta_samples, size)`，但 `size` 先要被夹；
* **线程组大小用常量 `mmse_ta_tg_size`**，删掉 `[[threads_per_threadgroup]]`。

### 3.3 K5 的同类修法（顺手，改动很小）

```metal
for (uint sym = 0; sym != mmse_max_slot_symb; ++sym) {   // 常量 14
  if (sym >= p.nof_symbols) break;                       // 参数只用来 break
  ...
}
```
（`ocudu_mmse_pilots.metal` 已经有 `mmse_max_slot_symb` 之类的常量可对齐。）

### 3.4 ★ 参数结构体：不要手写第二份

`mmse_ta_place_params` / `mmse_ta_params` / `mmse_rsrp_params` 都在 `.mm` 里**手写**了一份。
**这正是 §48.131(b) 点名的"字段错位"来源。** 纪律：

* **一侧定义、一侧引用**；做不到时，**在两侧各加一条 `static_assert(sizeof(...))`**，
  并在宿主侧把**每个字段**打印一次（`[ta_params]` 那种一次性 dump，5a 的 `OCUDU_CE_RSRP_CHECK` 已证明有用）。

---

## 4. 验证阶梯（照 §48.132(b)，**不要跳**）

| 步 | 内容 | 需要 GPU | 前置 |
|---|---|---|---|
| **L0** | **纯静态审查**：三条约束逐条核对（本文 §2/§3）**+ 参数结构体尺寸/偏移对拍** | 否 | 现在 |
| **L1** | **CPU 侧内核对拍**：把 K7/K6 逐行转写成 C++，与真实宿主实现对同一合成输入比对（§48.134 的做法）| 否 | L0 |
| **L2** | **单内核、小几何、独立用例**，**一次一个 kernel**（先 K7 单独，再 K6 单独）| **是** | **L1 + 用户报备** |
| **L3** | 整链（K7 → dft_dit → K6）在单测里 | 是 | L2 |
| **L4** | 单抓包 replay / 门禁 / 提交 | 是 | L3 |

**L2 起必须**：
* **先确认没有别的 GPU 负载**（用户可能在跑 OTA/VM）；
* **跑之前向用户报备**；
* **限时**：macOS 没有 `timeout`，用后台进程 + 定时 kill 或 `perl -e 'alarm N'`；
* **另存上一版二进制**（§48.130 第 6 条），跑完查
  `ioreg -r -c IOAccelerator -d 1` 的 `Device Utilization %` / `recoveryCount`。

---

## 5. 当前状态与立即行动

* **树**：本轮 5a 的修复 + 5b 的 K6/K7 都在工作树里（未提交）。
  **K7 的接线已完成但不可信**（会挂），**K6 的调用点都要按 §3 重写后才能再上 GPU**。
* **立即行动（离线，不碰 GPU）**：
  1. 按 §3 重写 K7、K6（K5 一起）；
  2. 参数结构体加 `static_assert` + 一次性 dump；
  3. L1 的 CPU 对拍；
  4. **然后停下来，向用户报备**，再上 L2。

**⚠ 在 L2 通过之前，不要跑任何会 dispatch 这三个 kernel 的二进制。**
`ul_chain_replay --metal` **也会**跑 K5 —— 按 §2.3 它是既有技术债、已上线，但**重写 K5 之前不要再跑它**，
以免在"参数意外为 0"的边缘上再挂一次。

---

## 6. ✅ 离线修复已完成（2026-09-19，**未跑任何 GPU**）

### 6.1 三个 kernel 已按 §48.131(c) 重写

| kernel | 改法 |
|---|---|
| **K5** `mmse_rsrp` | 参数先夹（`nof_layers ≤ 4`、`nf ≤ 3300`）；`sym` 循环上界改常量 `mmse_rsrp_max_slot_symb=14`、参数只 `break`；写下标 `min(…, mmse_rsrp_max_ring-2)`。新增常量块 `mmse_rsrp_max_*` |
| **K6** `mmse_ta_profile` | `size ∈ [2, 4096]`、`nof_slices ≤ 16`、`nof_taps ∈ {3,5}` 全部先夹；三个循环改常量上界 + 参数 `break`；`profile[]` 下标由夹后的 `size` 决定；`rate_hz == 0` 时输出 0 而不是 inf |
| **K7** `mmse_ta_place` | **删掉 `[[threads_per_threadgroup]]`**（改用常量 `mmse_ta_tg_size=256`）；`size`/`nof_slices`/`nof_pilots` 全部先夹；两层循环改常量上界 + 参数 `break`；`pos` 夹进 `size` |

**静态复核（纯离线）**：

| 检查 | K5 | K6 | K7 |
|---|---|---|---|
| 循环上界全是编译期常量 | ✅ | ✅ | ✅ |
| 参数只用于 `break`/跳过 | ✅ | ✅ | ✅ |
| 下标全部夹住（读+写） | ✅ | ✅ | ✅ |
| 不读 `[[threads_per_threadgroup]]` | ✅ | ✅ | ✅ |
| barrier 之前无早退、条件对线程组一致 | ✅ | ✅ | 无 barrier |

### 6.2 参数结构体对拍（§48.131(b) 点名的"字段错位"）

三处都加了 `static_assert(sizeof(...))`：`mmse_rsrp_params==56`、`mmse_ta_params==32`、
`mmse_ta_place_params==16` —— **宿主与 MSL 各自的声明必须同宽**，编译期就拦。

### 6.3 宿主侧几何拒绝（§48.132(e)(2)：夹子不能变成静默截断）

`run_ta_place()` / `run_ta_profile()` 现在**拒绝**超出内核常量的几何
（`dft_size > 4096`、`nof_slices > 16`、`nof_pilots > dft_size`）⇒ 调用方退回宿主估计，
而不是拿到一个"看起来有效"的截断结果。

### 6.4 ⚠ 剩余风险（必须如实说）

1. **挂死的"确切"触发条件仍未定位**。我修的是**结构**（终止性不再依赖参数），
   而结构缺陷**确实**能解释挂死，但**没有证据**证明就是它 —— 参数结构体两侧我核对过是一致的。
   真正的定位要靠 L1/L2。
2. **改动很大**（三个 kernel 的循环结构全变）⇒ **数值必须重新验证**，
   不能假设"只加了夹子所以结果不变"。§4.6/4.7 的判据要重跑。
3. **K5 也改了**，而它已经在空中验证过（腿 `5a-pubpath_0919_1230`）⇒
   **K5 的改动必须重新过 5a 的探针**，必要时再跑一条腿。

### 6.5 下一步（**需要用户报备**，§48.131(c) 第 4 条）

| 步 | 内容 | GPU |
|---|---|---|
| L0 | 静态审查（§6.1–6.3）| 否 | ✅ 已完成 |
| L1 | **单内核、最小几何、独立用例**：先只跑 **K5**（已验证过、最熟），确认重写后数值不变 | 是 | ⬜ 待报备 |
| L2 | 再只跑 **K7**（挂死的那个），最小几何（4 PRB、3 symbol、1 层）| 是 | ⬜ |
| L3 | 再跑 **K6**，再跑整链 | 是 | ⬜ |
| L4 | 5a 探针 + 数据 A/B + 单测 + 空中腿 | 是 | ⬜ |

**L1 起的纪律**（§48.130 第 5/6 条 + §48.131(c) 第 4/5 条）：

* **跑之前向用户报备**；
* **确认没有别的 GPU 负载**（用户在跑 OTA/VM）；
* **限时**：macOS 无 `timeout` ⇒ 后台进程 + 定时 `kill`，或 `perl -e 'alarm N'`；
* **另存上一版二进制**（`cp build/.../*_unit_test /tmp/…_ref`）；
* 跑完查 `ioreg -r -c IOAccelerator -d 1` 的 `Device Utilization %` / `recoveryCount`。

---

## 7. ✅ 验证阶梯全部通过（2026-09-19，一步一个 kernel）

限时工具：`/tmp/limited_run.sh <秒> <输出> <命令…>`（macOS 无 `timeout`，用后台进程 + 定时 `kill -9`）。
另存上一版二进制在 `/tmp/*_ref_before_harden`。**每一步跑完都查了 `ioreg` 的 `recoveryCount`。**

| 步 | 内容 | 结果 | GPU |
|---|---|---|---|
| **L1** | **K5 单独**：7 种语料探针 | ✅ 与加固前**逐位一致**（`dev_nre == host_nre`，worst rel 2.6e-09~3.4e-08）| `recoveryCount=0` |
| **L2** | **K7 单独**（挂死的那个）：最小几何 3 slices × 24 pilots | ✅ **PASS，0 mismatching，未挂死** | `recoveryCount=0` |
| **L3** | 设备 IDFT 谱 / 完整设备 TA / K6 / **整链（K7→IDFT→K6）** | ✅ 四项全 PASS；整链 9 个时延最差 **4.39 ns < 1 分辨率 8.14 ns** | `recoveryCount=0` |
| **L4** | 5a 探针、计数器、27 捕获数据 A/B、13 个测试 | ✅ 探针通过；**2.10 读 + 0.65 写/跳**；`_llr`/`_h`/`.bin` 全 0 差异；All tests PASSED | `recoveryCount=0` |

### 7.1 阶梯中抓到的三个**真缺陷**（都与挂死无关，但都会静默出错）

| # | 位置 | 症状 | 真因 |
|---|---|---|---|
| 1 | **我的测试代码** | L3 **段错误** | `modular_re_buffer_reader<cf_t, 1>` 的模板参数是 **MaxNofSlices 容量**，我塞了 3 个 slice ⇒ 模板内部 `static_tensor` 越界写（release 下断言关闭）。**生产代码用的是 `MAX_NSYMB_PER_SLOT * MAX_LAYERS`，不受影响** |
| 2 | **我的测试代码** | 整链每个时延读回 **0** | K7 **只写导频位置、从不写零**（这是契约：调用方零一次、复用之）⇒ 我在循环**外**清零，于是每一轮都读到**上一轮的残留**。**"零一次"必须由调用方每跳执行** |
| 3 | **★ K7 本身** | 整链全错、谱不平 | **输入该用自然序，不是数字反转序。** `dft_dit` 的装载写成 `in[perm[i]]`，看着像"输入要反转"，**其实那个 gather 本身就产生了第一级蝶形需要的反转序**；喂反转输入 = 双重反转。判据：单位单音的 IDFT 应当**平坦**——自然序 flatness **1.0000**、反转序 **330** |

**第 3 条是本次最有价值的发现**：那行注释（"Digit-reversed load"）把我引错了方向，**是解析判据（单音 IDFT 平坦）把它揪出来的**。
教训：**别读注释推断约定，用解析判据测约定。**

### 7.2 结论

* **挂死已消除**：重写后的 K7 在最小几何与整链上都正常返回并给出正确结果。
* **但"原版为什么挂死"仍未定论** —— 见 §6.4。修的是结构（终止性不再依赖参数），它**能**解释挂死，但没有直接证据。
* **K5 的加固是零行为改动**（L1 与加固前逐位一致、计数器不变、数据 A/B 全 0），所以**5a 的空中验证仍然有效**。
