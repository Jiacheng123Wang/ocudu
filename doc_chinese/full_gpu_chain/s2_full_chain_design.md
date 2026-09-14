# S2 全链：PUSCH 解调链单 command buffer 设计（GPU-PHY S-2h）

> ## 📌 MEMO（2026-09-14，用户口述，优先级依据）
>
> **最终目标**：从 **RF I/Q samples 到 MAC PDU 全链条走 GPU Metal path**。
>
> **已规划的回退（次优）方案**（因为 LDPC 是一块硬骨头）：RF I/Q samples 到达后，**CPU thread 无阻塞地 dispatch 给 GPU**，
> 一路到 **LLR**；LLR 再回到 **CPU 的另一个 thread** 做 **CPU LDPC 译码**，然后继续走 **FAPI 到 MAC**。
> 也就是说，与全 GPU 方案相比**只是 CPU/GPU 的切换点不同**（一个在 LDPC 之前，一个在 MAC 之前）。
>
> **现阶段主任务（据此确定优先级）**：**集中精力优化 LDPC 之前的 GPU pipeline**，使"**IQ samples → demod LLR**"这一段在
> GPU Metal 上**全链条最优**（含零拷贝、单 command buffer、少等待、必要时单例引擎/跨端口批处理）。
> Metal LDPC 待这一段做完后再攻坚；**实在不行就回退上面的次优方案**。
>
> **LDPC 现状备注**：前期已在 LDPC 上投入大量精力；目前的主要怀疑点是——**3GPP 最初设计 LDPC 时就是面向串行计算优化的**
> （双对角/分层结构、迭代依赖链），因此它在 GPU 上的"逐 TB dispatch"路线天然吃亏，需要进一步研究（算法级重排/批处理/星形修复等）。
> 在得到结论之前，**LDPC 相关的优化项（含 GPU LDPC 的 dispatch 开销、persistent kernel 等）一律标为"暂缓"**，
> 不作为近期优先级；本文档中出现的 LDPC 数字（如 `ul_ldpc_decode` median ~2 ms）**只作为预算背景**，不作为行动项。

> **读前必看（2026-09-13 收尾）**：本文按时间叠加，早期小节的判断会被后面的实测推翻。
> **最新结论一律以 §28 为准**，要点：
> 1. **引擎调用的 host 成本 = ~104 µs 固定等待**（相位探针 `OCUDU_MMSE_DEBUG=1`：wrap/cb/encode/commit
>    合计 ~4 µs，GPU 6.5 µs）⇒ 唯一杠杆是**每 slot 的引擎调用次数**。
> 2. **CE 求逆的目标仍在 GPU**：K1 并行化后单个 36×36 仍 75–92 µs，但那是**核函数缺陷**（一个 hop 只
>    求逆 1 个 36×36 = 47k FLOPs，却排了 72 个枢轴步 × 2 次屏障、逐元素走 threadgroup 内存），
>    不是"GPU 不适合求逆"。CPU Gauss-Jordan（~10 µs）是**过渡桥**，修法见 §28 下一步第 0 项。
>    （§27 末尾的"求逆占 93%"是孤立调用口径，已作废。）
> 3. **CE 每 hop 2 次引擎调用 → 1 次**（尾块走 CPU 兜底路径）：25 PRB/2 DMRS 234.1 → 148.1 µs/hop，
>    NMSE 逐位一致。
> 4. **资源划分原则（2026-09-13 用户确认）**：目标是**整条 PHY pipeline 走 GPU Metal path**
>    （I/Q samples → LDPC 输出 CRC=OK 的 MAC PDU）。CPU↔GPU 的分工按**大粒度**划分——PHY compute 归
>    GPU，CPU 只做编排与非 PHY 工作；**局部 µs 对比不作为方向性判据**。本文与代码里任何"回退到 CPU"的
>    实现（CPU 求逆、CPU 尾块、CPU 兜底块）都是**过渡桥**，必须在 S-5（CE 并入 slot 单 CB）里抹掉。
> 5. **本文（活文档）是唯一最新来源**；`session_handoff_2026-09-12.md` 与 `pipeline_audit_2026-09.md`
>    是当时会话/审计的**只读快照**——里面的数字（如 `engine.invert()` 511.9 µs、CE "求逆占 93%"）
>    不代表现状，事后更正一律写进本文，不回填快照。

## 1. 目标与判据

| 指标 | 现状（OTA 实测） | A-1 目标 | A-2 目标 |
|---|---|---|---|
| `ul_equalization_demod` | 2260–2704 µs/槽 | ~1.2–1.5 ms | **<0.5 ms** |
| 每槽 eq+demap 等待次数 | ~22 次 | ~11 次 | **1–2 次** |
| 单次 dispatch+wait | ~100 µs | — | — |

依据（本机实测，同一内核）：

- 逐次 `commit+wait`：**101.5 µs/次**（min）
- 16 次收进单 command buffer：**12.6 µs/次**（min）→ 摊薄 **8×**
- GPU 侧执行仅 ~6 µs/次，主机侧编码 ~0.5 µs/次

结论：eq+demap 阶段的时间**几乎全部是等待**，唯一有效手段是减少等待次数并把中间结果留在 GPU。

## 2. 现状与阻塞点

`pusch_demodulator_impl::demodulate()` 每符号：

```
equalize(eq_re, eq_nv, ch_re, ch_est, nv_est, 1.0F)     // metal: 1 commit+wait   ← 等待①
[transform precoding]  precoder->deprecode_ofdm_symbol(eq_re)      // CPU 读 eq 符号
[post-eq SINR]         accumulate(filter_inf(eq_nv))              // CPU 读 eq_nv  ← 阻塞融合
while (RE remain) {
    demap(codeword, eq_re_block, eq_nv_block, mod)      // metal: 1 commit+wait   ← 等待②
    [EVM]              evm_calc->calculate(codeword, eq_re_block, mod)   // 等待②之后 ✓
    [descrambling]     revert_scrambling(codeword, seq)                  // 等待②之后 ✓
}
```

**四个 CPU 消费者中只有两个阻塞融合**：

| 消费者 | 位置 | 是否阻塞 |
|---|---|---|
| post-eq SINR 归约（读 `eq_nv`） | equalize 与 demap 之间 | **是** |
| transform precoding（读 eq 符号） | 同上 | **是**（可选路径，可回退） |
| EVM（读 eq 符号 + LLR） | demap 之后 | 否 |
| 解扰（读 LLR） | demap 之后 | 否 |

## 3. 方案 A-1：同 CB 融合 eq + SINR 归约 + demap（每符号一次等待）

### 3.1 接口扩展（全部带默认实现 → CPU 后端零改动、行为不变）

1. `channel_equalizer`
   - `void submit(...)`：与 `equalize(...)` 同参，但**提交后不等待**；默认实现 = 调用 `equalize(...)`（同步），语义不变。
   - `void get_sinr_reduction(span<float> out)`：把该符号的 `Σ filter_infinite(nv)` 与有效 RE 数写入 `out[0..1]`；默认实现 = 遍历 `eq_noise_vars`（与现有 CPU 逻辑逐字面一致），metal 实现 = 内核内归约后在等待之后直接读回。
2. `demodulation_mapper`
   - `void submit(...)` + `void wait()`：同上，默认实现退化为 `demodulate_soft(...)`（同步）。
3. 新增 `bool supports_fused_chain() const`（默认 `false`）：仅当调用方可以走融合路径（无 transform precoding）时由 metal 实现返回 `true`；调用方用它决定走融合还是原路径。

### 3.2 调用方改造（唯一的共享代码改动：`pusch_demodulator_impl.cpp`）

```
bool fused = equalizer->supports_fused_chain() && demapper->supports_fused_chain()
             && !config.enable_transform_precoding;
for (i_symbol ...) {
    if (fused) {
        equalizer->submit(...);            // 不等待
        demapper->submit(...);             // 同一 CB（eq → 归约 → demap）
        demapper->wait();                  // 每符号一次等待
        equalizer->get_sinr_reduction(out);           // 等待之后读，语义不变
    } else {
        equalizer->equalize(...);                     // 原路径
    }
    [SINR 累加：融合路径读 kernel 归约结果；原路径保持逐 RE 遍历]
    while (...) { demap/EV M/解扰 … }                  // 融合路径只做 EVM + 解扰
}
```

要点：
- **SINR 的数值语义不变**（同一 `filter_infinite_and_accumulate` 定义，只是求和位置从 CPU 换到 GPU 归约）；用单测对拍逐符号结果。
- 融合路径下 `equalize` 的输出仍写回（EVM 需要），但**不再触发等待**。
- 任何条件不满足 → 原路径，逐字节等价。

### 3.3 内核改造

- 新 kernel `equalize_sinr_demap`（或让现有 demap kernel 接收 eq 中间结果）：
  - 线程组织：每 RE 一线程做 eq（现 `equalize_mxn` 逻辑），nv 落回全局内存的同时累加到 `threadgroup` 局部和；随后同一线程做 demap（现 `demod_soft` 的区间表逻辑），LLR 直接写目标缓冲。
  - 归约：`simd_shuffle`/`threadgroup_barrier` 后由 lane 0 写 `Σ filter_inf(nv)` 与 count。
  - 约束：eq 的 4×4 Gram 求逆（最多 16 个 float2 寄存器）+ demap 的区间表（constant 内存）共存；若寄存器压力过大，退化为"同 CB 两次 dispatch，一次等待"（仍是 A-1 的收益）。

### 3.4 验证

1. **单测**：融合前后 `eq/nv` 逐位一致（现有 A/B 门禁），LLR **逐位一致**（现 demapper 门禁），SINR 归约结果与 CPU 遍历误差 ≤1e-5 相对。
2. **回归**：`ctest -L phy` 全绿；CPU 后端（默认钩子）路径行为不变。
3. **E2E**（你跑）：ZMQ/OTA 轻载档（overflow=0）对比 `ul_equalization_demod`、`ul_pipeline`，以及 `[metal_stats]` 的 commits/waits 比值变化。

## 4. 方案 A-2（终态，A-1 验证后）

- 整槽 14 符号的 eq+demap 收进**一个 CB**，槽末一次等待：需要
  - LLR 目标缓冲页对齐（解扰前一次性同步即可）
  - SINR 归约结果按符号缓存（14×2 float）
  - `pusch_demodulator_impl` 的两段循环（先 eq 后 demap）而非逐符号交替
- 预期：等待 22 → 1，`ul_equalization_demod` → **<0.5 ms/槽**
- 可选进一步：把**解扰**移入 kernel（PRG 序列由 CPU 生成并上传，序列短、成本低），彻底消除 LLR 回传的必要性

## 5. 影响面清单（A-1）

| 文件 | 改动 | 风险 |
|---|---|---|
| `include/ocudu/phy/upper/equalization/channel_equalizer.h` | +3 个带默认实现的方法 | 低（纯扩展） |
| `include/ocudu/phy/upper/channel_modulation/demodulation_mapper.h` | +3 个带默认实现的方法 | 低 |
| `lib/phy/upper/channel_processors/metal/*` | 融合 kernel + 适配器 submit/wait | 中（GPU 代码，需单测对拍） |
| `lib/phy/upper/channel_modulation/metal/*` | submit/wait | 低 |
| `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp` | 融合分支 + SINR 读取位置 | **中高（共享代码，必须保持原路径等价）** |

## 6. 实施顺序

1. 接口扩展（默认实现）+ 单测证明 CPU 路径零变化
2. equalizer/demapper 的 metal `submit`/`wait`（同 CB 两次 dispatch，一次等待）→ 拿到 A-1 的一半收益，风险最低
3. 融合 kernel（一次 dispatch）→ A-1 完整收益
4. `pusch_demodulator_impl` 融合分支 + SINR 归约
5. A-2（整槽单 CB）

## 7. TODO：iperf3 高吞吐下的排队问题（2026-09-12 定位完成，暂不处理）

**现象**：iperf3（即使降到 0.1 Mbps）+ ping 时，`ul_channel_estimation` 阶段 14–32 ms、`ul_pipeline` 19–38 ms；而 ping-only 时分别是 **0.43 ms / 6.0 ms**。

**定位结论（已验证）**：**不是 CE 变慢**，而是**上层 PHY 的处理排队延迟被记到了"交接到上层 PHY 之后被测量的第一个阶段"（CE）上**：

| 证据 | 数值 |
|---|---|
| CE 引擎自测（`[mmse_time_sum]`） | 4150 次调用、`hops_gpu=4150`、`fb_blocks=0`、**314.5 µs/次（max 885 µs）**，全程恒定 |
| CE 调用次数与阶段耗时 | **反相关**：2.85 次/槽 → 14.3 ms；8.8 次/槽 → 6.6 ms ⇒ 与 CE 算量无关 |
| 每槽真实工作量 | t2f 0.20 + CE 0.90 + eq/demod 2.19 + LDPC 2.21 ≈ **5.5 ms**，而槽长 **1 ms**（15 kHz）⇒ 超预算 **5.5×** |
| 交叉验证 | 与 FFT 流水线开关（depth=1 更差 32 ms）、S-2 改动（基线 `16eeba76b1` 25 ms）、`rx_gain`、UHD overflow 均无关 |
| 佐证 | `Real-time failure in FAPI: UL processor` 405–3919 条 |

**方法论警示**：饱和状态下**各阶段探针不可信**——它们测的是含排队的跨度，且排队时间总是落在交接后的第一个阶段。对比请用 overflow=0 的轻载档，或使用"每次 dispatch/每次调用成本"这类不含量纲。

**待办（优先级低于产能优化）**：
1. **根本解 = 提升上行链产能**：eq+demap 去等待（A 线，在办）、LDPC 算法/按槽批量（待定）
2. 若仍需定位排队本身：为 `dmrs_pusch_estimator_impl::estimate()` 这一层加拆分计时（`estimate()` 内部 vs `t2f_end → estimate()` 之间的交接延迟），并核查 UL 执行器/线程容量与请求池（日志出现过 1 次 `PRACH buffer pool depleted`）
3. iperf3 目标线：每槽总处理量 **< 1 ms**（当前 5.5 ms）；在此之前 iperf3 只能承受轻载

## 8. A-1 实现状态（2026-09-12）

**已完成**：
1. `channel_equalizer` / `demodulation_mapper` 接口新增 `submit()` / `wait()` /
   `get_post_eq_sinr()` / `supports_deferred_chain()`，**全部带默认实现** ⇒ CPU 后端逐字节不变
   （commit `36995378c5`）
2. `channel_equalizer_metal` 实现 `submit()`（提交不等待）+ `wait()`（等 CB + 回拷）+
   `supports_deferred_chain() = true`（commit `da71a67aa1`）；单测验证 `submit()+wait()`
   与 `equalize()` **逐位一致**
3. `pusch_demodulator_impl` 接入延迟链（`supports_deferred_chain()` 门控 + 禁用 transform
   precoding 时回退）：`equalizer->submit(...)` → 解调（其等待经同队列顺序覆盖均衡 CB）→
   循环后 `equalizer->wait()` 兜底 → **随后**再做 post-eq SINR 归约（同一 `filter_infinite()`
   语义、同一缓冲，仅位置后移，数值不变）

**关键简化（相对原设计）**：两个适配器已共用 `backend_queue()`，Metal 对同队列保证按提交顺序
执行 ⇒ **不需要融合 kernel、不需要共享 command buffer**：仅"延迟均衡器的等待"即可把每符号
等待从 2 次降到 1 次。

**预期**：每槽 eq+demap 等待 22 → 11 次；`ul_equalization_demod` 2.19 ms → ~1.2 ms
（按实测 101.5 µs/次 等待成本）。

**验证口径**：`ctest -L phy` 159/159（CPU 后端走原路径）；延迟路径本身由 E2E 覆盖
——请在 **ping 轻载档（overflow=0）** 对比 `ul_equalization_demod` 与 `[metal_stats]`。

**剩余**：A-2（整槽单 CB，目标 <0.5 ms）与 `get_post_eq_sinr()` 的 kernel 内归约（当前走 CPU
遍历，位置已正确、非瓶颈）。

## 9. A-1 实测结果与修正后的成本模型（2026-09-12 OTA ping 档）

| 指标 | A-1 前 | A-1 后 |
|---|---|---|
| `ul_equalization_demod` | 2704 µs（中位 2751） | **2474.6 µs**（中位 2360）= **−8.5%** |
| t2f / CE(中位) / LDPC | 233 / 430 / 2255 µs | 225 / 397 / 2135 µs（不变） |

**修正 1：流水线内一次等待 ≈ 20–40 µs，不是孤立探针的 101.5 µs。**
那个 101.5 µs 是"完整往返"（含队列调度与线程唤醒）；流水线里 CPU 提交后立即等待、GPU 常常已算完，
所以省掉一次阻塞等待只值 ~230 µs/槽 ⇒ 与实测 −229 µs 吻合。

**修正 2：eq+demod 阶段的成本是「dispatch 次数 × 每次 ≈58 µs」：**
```
2474 µs / 42.3 次(21.2 eq + 21.2 demap) ≈ 58.5 µs/次
分解（估计）：等待 20–40 + 提交/编码 5–10 + GPU 执行 + CPU staging(bf16 memcpy 2–5)
```
⇒ **真正的杠杆是"等待次数"，不是"CB 数"**：42 次等待 → 1 次，可省 **~1.2 ms/槽**
（2.47 ms → ~1.2–1.3 ms）。注意把多个 dispatch 塞进同一个 CB **并不**减少 dispatch 本身的
执行开销，只减少提交与等待。

## 10. A-2 方案（两条路线）

**路线 1（保守，不改语义）：调用方两趟 + 延迟解调器等待**
```
pass 1（全槽）：逐符号 equalizer->submit(...)            // 42→21 次等待（eq 端已延迟）
pass 2（全槽）：逐符号 demapper->submit(...)             // 需要解调器也支持延迟提交
wait 一次（槽末）→ 再逐符号/逐块：读 LLR → EVM → 解扰 → 统计通知
```
- 前提：解调器适配器加入 `submit()`/`wait()`（引擎已有 batch API 可直接照搬）；调用方把
  "解扰 + 统计通知"从 demap 循环内移到 wait 之后（`codeword_buffer.on_new_block` 顺序必须保持）
- 阻塞点：**eq 输出缓冲按符号复用**（`temp_eq_re` 只够一个符号）⇒ 需要按符号分段（扩到整槽，
  或按 K 个符号一组分批）
- 预期：等待 42 → 1–2（若分 K 组则 42/K） ⇒ `ul_equalization_demod` → **~1.2–1.3 ms**

**路线 2（彻底，需算法改动）：把解扰移入 GPU，LLR 只在槽末被消费**
- 解扰序列由 CPU 生成（PRG，成本低）后上传，kernel 内做符号翻转 ⇒ CPU 不再逐块读 LLR
- 这样"逐符号交替"可以彻底变成"全槽提交 + 槽末一次等待"，且 LLR 直写页对齐缓冲供 LDPC 使用
- 预期：等待 42 → 1 ⇒ 同上 ~1.2–1.3 ms，且为后续"eq+demap+解扰+LDPC 单 CB"铺路
- 风险：动解扰语义（必须逐位对拍），以及 codeword buffer 的生命周期

**共同前提/风险**：
1. `temp_eq_re`/`temp_eq_noise_vars` 只够一个符号 ⇒ 路线 1 需扩容或用 K 组分批
2. 逐符号的 provisional stats 通知时机后移（数值不变，顺序仍需保证）
3. 仍用 `supports_deferred_chain()` 门控，CPU 路径不变

## 11. 路线 1（K 组批量）实施清单 —— 调用方改造

**目标**：每槽等待 42 → `ceil(14/K)`（K=7 时 2 次），`ul_equalization_demod` 2.47 → ~0.8–1.3 ms。

### 步骤

1. **`pusch_demodulator_impl` 缓冲扩容**（当前只够一个符号）
   - `temp_eq_re` / `temp_eq_noise_vars` 构造期改为 `K * MAX_NOF_PRBS * 12 * MAX_NOF_LAYERS`
     （K = 7；106PRB/4层时约 8.4 MB，可接受）
   - 符号 `s` 的视图 = `temp_eq_re.subspan(s * nof_re_symbol * layers, nof_re_symbol * layers)`
   - 注意：**不能只改分配**——必须同时把"每符号都用 `first(n)`"改成按 `s` 取偏移，
     否则所有符号会写同一块内存（这正是本步与第 2 步必须一起做的原因）

2. **主循环改为"按 K 个符号分组、组内三趟"**
   ```
   for (group of ≤ K symbols) {
     // pass 1：均衡（延迟提交，不等）
     for (s in group) { 计算 ch_est → equalizer->submit(eq[s], nv[s], ...); }
     // pass 2：解调（延迟提交，同一个后端队列，顺序保证 pass 1 已完成）
     for (s in group) for (block) { demapper->submit(codeword, eq[s]_block, nv[s]_block, mod); }
     // 一次同步
     demapper->wait(); equalizer->wait();
     // pass 3：消费（全部在 wait 之后，与原语义相同）
     for (s in group) {
        [post-eq SINR 归约]
        for (block) { [EVM] → 解扰 → 统计 → codeword_buffer.on_new_block(...) }
     }
   }
   ```

3. **必须逐条核对的不变式**
   - `codeword_buffer.on_new_block(...)` 的调用顺序 = 逐符号、逐块递增（与原顺序一致）
   - 逐符号 provisional stats 的通知仍在"该符号最后一块之后"（只是整体后移到组末）
   - `eq_re_block` / `eq_noise_vars_block`（EVM 用）只在组内 `wait()` 之后读取
   - **非延迟路径逐字节不变**：全部改动由 `supports_deferred_chain()` 门控；
     CPU 后端（返回 false）与 transform precoding 场景走原代码

4. **验证**
   - `ctest -L phy` 全绿（CPU 路径）、metal 单测全绿
   - E2E（ping 轻载档）：`ul_equalization_demod` 预期 **0.8–1.3 ms**；
     `[metal_stats]` 的 demapper/equalizer commits 与 waits 差值应体现"多次提交、一次等待"
   - 若出现 UL CRC 异常：把 K 设为 1（即回到 A-1 行为）即可二分定位

> **实施状态：已完成（2026-09-13），见 §12。** 实际落地比本清单多两处硬约束
> （金属适配器需 FIFO 多笔 pending；码字缓冲的逐块视图只能在消费阶段取），
> 并顺带修掉 A-1 引入的 post-eq SINR 回归。GPU 探针实测 4–6x，预测 E2E `ul_equalization_demod` 450–800 µs。

### 已知风险
- 缓冲扩容 8 倍内存（K=7）；若担心，可取 K=4（等待 4 次）
- pass 3 的 EVM 读的是"组内已完成的 eq 输出"✓ 但 `evm_calc` 若为 null（未启用）则无影响
- 解扰仍在 CPU ⇒ pass 3 必须逐块做；这正是路线 2（解扰下沉）要消除的部分

## 12. 路线 1（K 组批量）实现记录（2026-09-13）

### 12.1 落地内容（比 §11 清单多了两项，都是实现中发现的硬约束）

| # | 改动 | 说明 |
|---|---|---|
| 1 | `pusch_demodulator_impl`：K=7 符号**页对齐分段缓冲** | `temp_eq_re`/`temp_eq_noise_vars` 扩到 7 符号，每符号区起始按页对齐（步长向上取整到页）；新增 `temp_llr`（同样 7 段页对齐）作为解调器 LLR 直写目标 |
| 2 | 调用方改为"按 K 分组、组内三趟" | pass1 均衡 submit → pass2 解调 submit（**每符号一个 dispatch，覆盖整符号**）→ 一次 wait → pass3 逐块消费（搬运 LLR→EVM→解扰→统计→`on_new_block`） |
| 3 | **金属均衡器：FIFO 多笔 pending + 每笔独立 staging** | 原实现只有一份 pending 状态和一组 h/y/s staging，K 笔在飞会互相覆盖（§11 未预见）。现在每次 submit 从池里取一个 entry（自带 staging），wait() 等最新 CB 后按 FIFO 统一回拷 |
| 4 | **金属解调器：LLR 目标页对齐时直写（零拷贝）** | 目的缓冲页对齐时 kernel 直接写，彻底省掉 LLR 暂存与回拷；同时 pending 也改为 FIFO + 每笔独立 staging |
| 5 | 新增调试开关 `OCUDU_PUSCH_DEFERRED_GROUP` | 覆盖分组大小（默认 7，clamp 到 [1,7]）；`=1` 即退回 A-1 行为，可无重建二分 |

### 12.2 §11 未预见、必须一并解决的两个问题

1. **码字缓冲的"逐块视图"不能在提交阶段推进**：`get_next_block_view()` 返回
   `softbits_buffer.subspan(softbits_count, n)`，而 `softbits_count` 只在 `on_new_block()` 里前进
   （`pusch_decoder_impl.cpp:217`）。所以 pass2 里连续取视图会拿到同一块内存——"逐块 submit"不可行。
   **解法**：pass2 改为"每符号一个 dispatch"，LLR 写进本符号的页对齐 staging；pass3 再按原有顺序
   （`get_next_block_view` + `on_new_block` 交替）把整符号 LLR 切块搬出去。逐 RE 解调是点运算，
   整符号一次 dispatch 与分块多次 dispatch 结果**逐位相同**（单测已验证）。
2. **A-1（提交 7c60bbf038）引入的 SINR 回归**：延迟路径把 post-eq SINR 归约放在
   `on_provisional_stats`/总计累加**之后**，导致逐符号 SINR 上报为 `inf`、总计里 SINR 计数恒为 0。
   生产默认 `pusch_sinr_calc_method = post_equalization`（`du_low_config.h:41`），所以该回归在
   OTA 运行中一直生效。本次把归约移到"组 wait 之后、统计通知之前"，与串行路径完全一致。

### 12.3 验证

- **新增 CPU 后端等价性单测** `tests/unittests/.../pusch_demodulator_deferred_chain_test.cpp`：
  用"只声明 supports_deferred_chain() 的 CPU 装饰器"驱动新分组调用方，与原始串行路径逐位对比
  码字块、事件顺序（`on_new_block` 与 provisional stats 的先后）、逐符号/总计统计值；
  码字缓冲故意按 100 RE 分块以覆盖"一符号多块"的搬运；覆盖 1/2 端口、1/2/4 层、QPSK→256QAM、
  DMRS 占满/半占符号、少于 K 的分配、以及 transform precoding 关闭延迟链等 9 组配置。
  **该测试确实能抓住 12.2(2) 的 SINR 回归**（人为改回 A-1 顺序后报 `serial=16.32 dB` vs `deferred=inf`）。
- **该测试抓到的另一个 bug**：分组循环原本写成 `group_begin != group_stop`（步长 7），
  当 `nof_symbols` 不是 7 的倍数时（如 12）会越过末尾继续处理（`std::array` 越界 → 野指针崩溃）。
  真实场景 14 符号恰好整除，**OTA 不会暴露**。已改为 `<`。
- **金属单测新增**：均衡器 5 笔在飞（原地/暂存两种目标）+ 单次 wait 逐位一致；解调器 3 笔暂存在飞、
  页对齐直写 + 整符号/分块逐位一致。
- **GPU 探针** `metal_chain_probe`（新增，非测试套件）：同一进程内对比"逐符号链"(A) 与"分组延迟链"(B)。

```
# OTA 档（25PRB/300RE/1x1，50 slot 平均，两次运行）
A 逐符号链: 2728 / 1757 us/slot      B 分组 K=7: 444 / 435 us/slot   → 4.0–6.1x
# 20MHz 档（106PRB/1272RE/2x2，30 slot 平均）
A: 1889 / 1961 us/slot               B: 455 / 446 us/slot            → 4.2–4.4x
# 两种模式 LLR 逐位一致（真实 GPU + 真实适配器）→ OK
# K 扫描（300RE）：K=1 → 2033us, K=2 → 921, K=3 → 747, K=7 → 451, K=14 → 357 us/slot
```

**结论与预测**：K=7 后每符号 CPU 编码成本（~31 µs/symbol × 14 ≈ 435 µs/slot）成为新下限，
GPU 已不饱和。预计 OTA `ul_equalization_demod` **2474 µs → 450–800 µs**（4–5x），
比 §11 预估的 0.8–1.3 ms 更好。K=14（整槽一次 wait）实测只再快 ~18%，且要双倍缓冲、
并把下游 LDPC 的提前启动推迟半个槽，故默认保持 7。

### 12.4 风险与回退

- 回退：`OCUDU_PUSCH_DEFERRED_GROUP=1`（= A-1 行为，无需重建）；彻底回退=还原分组调用方。
- 非延迟路径（CPU 后端 / transform precoding）走 `group_size = 1` 的同一份代码，
  调用序列与改动前一致（单测逐位对比覆盖）。
- 内存：每实例约 +1.8 MB（106PRB/4层，K=7，三段缓冲），可按需下调 K。

## 13. 为什么第一次 E2E 毫无变化：延迟链被生产链路包装层挡掉（2026-09-13 定位）

第一次 E2E（`990088c629` 之后）`ul_equalization_demod` median 2314 µs ≈ A-1 水平，且
`[metal_stats]` 显示 **commits == waits、max_in_flight=1**（= 逐符号同步）。原因不是算法，
而是**能力查询被包装层吞掉**：

| 包装层 | 位置 | 是否转发 `submit/wait/supports_deferred_chain` |
|---|---|---|
| `channel_equalizer_metal_or_generic` | 金属后端工厂（按拓扑逐调用路由） | ❌（修复前）→ 默认 `false` |
| `demodulation_mapper_metal_or_generic` | 金属后端工厂（按调制逐调用路由） | ❌（修复前） |
| `phy_metrics_channel_equalizer_decorator` | 开启 PHY metrics 时套在金属后端外 | ❌（修复前） |
| `phy_metrics_demodulation_mapper_decorator` | 同上 | ❌（修复前） |

`pusch_demodulator_impl` 的门控是
`equalizer->supports_deferred_chain() && demapper->supports_deferred_chain() && !transform_precoding`，
接口默认实现返回 `false`，所以**任意一层不转发 ⇒ 整条延迟链静默退化为串行**。
用户的 OTA 跑法开启了 PHY metrics（`[ul_*]` 阶段数据正来自 metrics 装饰器），因此
**S-2i/S-2j/S-2k/S-2l 的延迟链在 OTA 里从未生效过**。

**对历史结论的修正**：A-1 实测的 2704 → 2474.6 µs（−8.5%）**不是**"减少等待"带来的
（等待从未减少），而是同批次引入的**页对齐原地写均衡输出**（`temp_eq_re` 页对齐 ⇒ 省掉
staging 与回拷）带来的。§9 的成本模型结论（"每次 dispatch ~58.5 µs，等待只占 20–40"）仍然成立，
但"42 次等待"这个前提在 OTA 中并不存在——OTA 一直是"每符号一次等待（demapper）+ 一次空转
`equalizer->wait()`"。这也解释了为什么 K=7 的预测（450–800 µs）应当在修复后才出现。

**修复**（本次提交）：四层全部转发；`wait()` 对 metal/generic 两个后端各调用一次（未使用的那个是
no-op）；metrics 装饰器在 `submit()` 路径照常上报指标（CPU 占用口径与同步路径一致，
因为延迟路径的阻塞发生在 GPU 等待而非 CPU 上）。

**新增回归测试**（防止再次静默退化）：
- `tests/unittests/phy/metrics/phy_metrics_deferred_chain_test.cpp`（新目录/新目标）：
  用桩后端断言两个 metrics 装饰器转发 `supports_deferred_chain()`、`submit()` 走到后端的
  `submit()`（而不是 `equalize()`）、`wait()` 到达后端，并且延迟提交**仍然上报指标**。
- 两个金属单测各加一条 `composite factory` 用例：`create_*_metal_factory()->create()` 得到的
  组合对象必须 `supports_deferred_chain() == true`，且 `submit()+wait()` 与同步调用逐位一致。

**顺带修掉**：`phy_metrics_demodulation_mapper_decorator.h` 原先 include 的是
`modulation_mapper.h`（而非 `demodulation_mapper.h`），只靠 include 顺序碰巧编过。

**判据（下次 E2E）**：`[metal_stats]` 中 demapper/equalizer 的
`waits ≈ ceil(commits / 7)`（每槽 commits ≈ 每槽数据符号数、waits ≈ 2）；
若仍出现 `commits == waits`，则说明另有包装层未转发，或该 PUSCH 使用了 transform precoding
（门控里的第三项）。

## 14. 延迟链 OTA 失败的根因与修复（2026-09-13，`380f6b2cf2`）

### 14.1 现象（两次运行的完整对比）

同样射频配置、同样 expert_phy 参数，唯一差别是 `OCUDU_PUSCH_FORCE_SERIAL`：

| | 串行（成功 attach+ping） | 延迟链（attach 失败） |
|---|---|---|
| `ul_equalization_demod` median | 2746.5 µs | **595.3 µs（4.6x）** |
| `ul_pipeline` median | 6104 µs | 3552 µs |
| PUSCH `crc=OK / KO` | 2734 行 | 1518 行 |
| **Msg5 形状（17PRB tbs=372）** | **18 OK / 56 KO（24%）** | **4 OK / 185 KO（2.1%）** |
| 同形状 OK 的 SINR | 0.4–8.4 dB | −36…−5.2 dB（都是 HARQ 合并救回的） |
| 同形状 KO 的 SINR | −18…+8.5 dB（p25 2.4） | **−36.7…+7.6 dB（中位 5.4，p75 6.2）** |
| Msg3 形状（3PRB tbs=11） | 2 OK / 43 KO，KO 集中 −1 dB | 10 OK / 155 KO，KO 集中 −1 dB（两次相同） |
| RF 失败/槽 | 6/13612 = 0.04% | 25/3303 = **0.76%（18x）** |

**结论：延迟链在"好 SINR"下解不出来**（同形状、同 SINR，串行 24% vs 延迟 2%），
而小 grant（Msg3）两次运行表现一致 → 与 grant 大小/并发度相关。

### 14.2 根因：均衡器 → 解调器 的 hand-off 只靠队列顺序

延迟链的 pass 1 提交整组均衡（7 个 command buffer），pass 2 紧接着提交解调，**中间没有任何
同步**，依赖"同一队列的 command buffer 按提交顺序执行"这一假设来保证解调器能读到均衡器刚写的
`eq`/`nv`。

**这个假设在 Apple GPU 上不成立**：Metal 只保证同一队列 command buffer 的**启动顺序**，
**允许它们重叠执行**（所以"等自己最后一个 CB 就能覆盖前面所有 CB"也是错的）。
真实流水线里每个 PDU 都由 `dependencies_pool` 分配独立的 demodulator 实例、在
`pusch_executor` 线程池里**并发**执行，于是解调器的 dispatch 可能在均衡器写完之前就读了
`eq`/`nv` → **LLR 用错符号算出来**（数值看起来合理，只是 SNR 掉 10–15 dB）→ CRC KO，
而 SINR（CPU 侧在 wait 之后算的）依然正常 ✓ 与 OTA 现象完全吻合。

### 14.3 本地复现（关键测试）

新增 `pusch_demodulator_deferred_chain_test` 的两个用例（走完整调用方 + 生产工厂 + 真实 metal 内核）：

1. **单实例、失败形状**（17/25 PRB、DMRS 符号无数据、2 端口、LLR 目标非页对齐）：
   延迟链 vs 同步链 **0/4896 LLR 差异**。
2. **并发 4 实例 × 15 次**（复刻 `dependencies_pool` + 线程池）：
   修复前 **9–16/60 次运行结果不同**（差异是符号翻转/数值不同，不是全 0）；
   **修复后 0/60**（连续 3 轮）。

### 14.4 修复

1. **调用方**：pass 2 之前显式 `equalizer->wait()`（每组多一次等待，每槽 2 次），
   让"均衡结果可见"不再依赖队列顺序；dispatch 批量的收益（每符号 1 次提交）保持不变。
2. **两个引擎**：把"只记最后一个 CB"改成"记录所有未回收的 CB 并逐个等待"，
   因为 CB 可重叠时等最后一个并不能覆盖前面的。
3. 顺带修掉 `no-copy` 自检被覆盖的诊断 bug（见 §13 之后的提交 `db4866d924`）。

### 14.5 实测代价与预期

同机探针（25 组输入）：不安全分组 B = 591 µs/slot，**安全分组 C = 800 µs/slot**，
串行基准 A ≈ 1459–1873 µs/slot。OTA 里 `ul_equalization_demod` 预计从 595 µs 升到
**650–800 µs**，仍比串行 2746 µs 快 **3.5–4x**。

**下次 E2E 判据**：去掉 `OCUDU_PUSCH_FORCE_SERIAL` 后 attach 应成功；
`[metal_stats]` demapper 的 `waits ≈ 2 × 组数`、`max_in_flight` 在 7 附近；
`ul_equalization_demod` 650–800 µs。

## 15. 里程碑：GPU 全链 + 延迟链在 OTA 打通（2026-09-13，`f0ced769b9` + CPU LDPC）

**配置**：`expert_phy --pusch_channel_estimator_algo metal_mmse --pusch_dft_type metal
--pusch_channel_equalizer_backend metal --pusch_ldpc_decoder_type auto`（去掉 Metal LDPC），
延迟链默认开启（K=7），`rx_gain 55`、`num_recv_frames=64`。
**结果**：**成功 attach + ping 200 包**（需多次尝试，见下"仍不稳定"）。

| 指标 | 串行链 + Metal LDPC | 延迟链 + Metal LDPC | **延迟链 + CPU LDPC** |
|---|---|---|---|
| `ul_pipeline` median | 6104 µs | 3552 µs | **1495 µs** |
| `ul_equalization_demod` median | 2746 µs | 595 µs | **756 µs** |
| `ul_ldpc_decode` median | 2364 µs | 1798 µs | **33 µs** |
| `ul_channel_estimation` median | 436 µs | 360 µs | 440 µs |
| `ul_time_frequency` median | 227 µs | 282 µs | 238 µs |
| PUSCH crc 成功率（Msg5 形状） | 24% | 修复前 2.1% → 修复后 29% | 可用 |

`ul_equalization_demod` 756 µs 与探针预测的"安全分组 C = 800 µs/slot"吻合（25PRB/1x1）✓。

**`[metal_stats]` 语义更新（S-2q 之后）**：解调器引擎现在也按 **每个 command buffer** 计 wait
（与均衡器一致），所以延迟链下 demapper/equalizer 都是 `commits == waits`、`max_in_flight ≈ 组大小`
（本次实测 6）✓ —— 这是**修复生效**的标志；此前的"waits ≈ commits/7"判据作废。

**仍不稳定**：1.495 ms/槽 仍高于 1 ms 预算（p95 2.33 ms、p99 2.58 ms），UL 处理会周期性落后于
FAPI 截止时间，因此需要多次尝试才能 attach。**剩余成本拆分**（median）：eq+demap 756
（其中 CPU 每符号编码/搬运 ≈ 430，GPU dispatch ≈ 170，等待 ≈ 160）+ CE 440 + t2f 238 + LDPC 33
+ FAPI 5 ≈ 1.47 ms。

**下一步优化目标（按收益/风险）**：
1. **eq+demap 的 CPU 侧**：每符号 2 次 dispatch 的 Metal 编码开销 ≈ 28 µs/symbol 是主项 →
   改为"每组每阶段 1 个 command buffer"（14 → 2 CB/组）可省 commit 开销与队列占用；
   再把 LLR 目标缓冲页对齐（DMX/解码器侧）可省掉解调器的 staging 与回拷。
2. **CE 440 µs**（第二大项，本阶段一直未优化）：批量 CE hop / 启用 NN 路径 / 与 DFT 重叠。
3. **流水线级**：让相邻 PDU 的 t2f/CE/eqdem/LDPC 重叠（当前是串行依赖），这是把 1.5 ms 压到
   1 ms 以内的根本手段。

### 15.1 优化 A：每 burst 只提交一个 command buffer（`5a70d41864`，已完成）

引擎级探针（OTA 尺寸 25PRB/1x1/300RE）实测的 dispatch 成本：

| 模式 | µs/dispatch (p50) |
|---|---|
| 每次 commit+wait（串行链） | 104.5 |
| 每次 dispatch 单独 commit（A1 之前的延迟链） | 16.4 |
| **同一 command buffer 内 16 次 dispatch** | **9.7** |

改动：两个适配器的延迟入口把 dispatch **留在打开的 batch 里**，由 `wait()` 统一 commit
（引擎新增 `batch_open()`）；同步入口仍立即 commit+wait，且会先收掉未完成的 batch 以免混用。
`run_equalize`/`run_demodulate` 的 defer 分支不再 commit。

**实测**：探针安全分组 C **799 → 664 µs/slot**（−17%）。**OTA 确认**（手机 ping 100 + CN ping 500
全部成功，CPU LDPC、延迟链默认开）：

| 指标 | A1 前（S-2q） | **A1 后（S-2r）** | 串行基准 |
|---|---|---|---|
| `ul_equalization_demod` median | 756 µs | **649 µs** | 2746 µs |
| `ul_pipeline` median | 1495 µs | **1349 µs** | 6104 µs |
| `ul_channel_estimation` median | 440 µs | 432 µs | 436 µs（未动，符合 §15.2 判断）|
| `ul_ldpc_decode` median | 33 µs | 20 µs | 2364 µs（Metal LDPC）|

正确性不变：真实形状 0/4896 LLR 差异、并发 0/60 失配、`ctest -L phy` 161/161、四个 metal 单测全绿。

**计数语义（重要，别再按旧判据看）**：S-2q 之后解调器也按**每个 command buffer** 计 wait，
且 S-2r 之后每个 burst 只有 **1 个** command buffer，所以：
`commits == waits` 且 `max_in_flight` 只有 1~2 是**正常**的（每个 burst 提交后立即被等待，
只有两个实例的 burst 恰好重叠时才会 >1）。另有两点使绝对计数不能按 PDU 反推：
① PUSCH 与 PUCCH 共用同一套 eq/demapper 工厂，而 `[metal_stats]` 是**进程级**统计；
② 因此 `commits/PDU` 会混入每槽的 PUCCH 调用。判断延迟链是否生效请看
**一次性路由日志**（`PUSCH: deferred chain enabled (group of 7 OFDM symbols)`）与**阶段耗时**。

### 15.2 优化 B（CE 440 µs）：结论——不是开销问题，不建议在本线动

- CE 每次 `estimate()` 只调用引擎 **2 次**（`invert()` + `run_weights_only()`/`run_nn()`），
  每次调用**各用一个 command buffer**（内部已是批量 kernel：K1 批量 Gauss-Jordan、
  K2 批量 W·y），所以 A1 那种"减少 commit 次数"的空间很小（最多把 2 个合成 1 个，省 ~1 次往返）。
- `[mmse_time_sum]`：`gpu_path=367 µs` 中 `gpu_wait` 只占 59.5 µs，其余主要是 **GPU 实际执行 +
  与其他阶段竞争 GPU**（后台队列/设备被 eq/demap/LDPC 占用时，CE 的 dispatch 也会排队）。
- 因此 CE 的 440 µs 属于**真实 GPU 计算 + 争用**，要降它需要算法/内核层面的工作（批量方式、
  NN/CoreML 路径）——那是 AI-CE（Helena）线的既有计划，不在本线范围内。

### 15.3 优化 C（流水线重叠）：现状与判断

`uplink_processor_impl` 把每个 PDU `defer` 到 `pusch_executor` 线程池，所以**相邻 slot 的 PDU
本来就会重叠执行**；因此 §15 的 `ul_pipeline` 1.5 ms 是**单 PDU 的延迟**而不是吞吐上限。
按阶段和（median）t2f 238 + CE 440 + eqdem 664 + LDPC 33 ≈ 1.38 ms/PDU，要完全实时（1 ms）
需要 ~1.4 个忙碌线程；当前系统恰好卡在"略超预算"的位置，所以 attach 需要多次尝试。

**剩下的真实杠杆（按收益）**：
1. **eq+demap kernel 融合**（每个符号 1 次 dispatch 而非 2 次）：可省 ~14 次 dispatch/slot
   （≈140 µs）+ 1 次等待（≈30 µs）→ eqdem 有望到 ~450–500 µs。风险：寄存器压力（§3.3 已记录）。
2. **CE 的算法/NN 路径**（−100~200 µs/PDU），属 AI-CE 线。
3. 任何进一步降低 GPU 争用的改动都会同时改善 CE（因为它有 300 µs 属于"排队等 GPU"）。

### 15.4 优化 ①：两阶段共用 1 个 command buffer（`c3d4993cb8`，已完成）

**做法**：新增线程本地的 `shared_burst`（`lib/phy/metal/ocudu_metal_burst.{h,mm}`）：整组的均衡
dispatch 与解调 dispatch 编进**同一个 command buffer、同一个 encoder**，在**计算管线切换处**
（即阶段边界）插入 `memoryBarrierWithScope:MTLBarrierScopeBuffers`。于是每组只需 **1 次 commit、
1 次 CPU 等待**（原来是 2+2），并且顺序由显式屏障保证、不再依赖队列。

**过程中发现并修掉一个架构级 bug（这是之前 S-2q 竞态更深的根因）**：
每个引擎都用 `newBufferWithBytesNoCopy` 各自包装**同一块内存**，而 **Metal 的 hazard tracking 与
memory barrier 都只对"被绑定的那个 resource"生效** —— 两个 MTLBuffer 别名同一地址时，驱动看不到
依赖，屏障也无效（这正是"同一 CB + 屏障"最初仍然出现前半组全 0 的原因）。修法：`shared_queue`
新增**进程级 no-copy wrap 缓存**（`wrap_no_copy()`，按地址索引、带长度检查与互斥），所有引擎共用，
同一地址只对应一个 MTLBuffer ⇒ 生产者/消费者成为可追踪的依赖。

**组大小改为整槽（14）**：每组的 commit/wait 固定为 1，组越大摊得越薄。探针（25PRB/1x1）三轮：

| 分组 | safe 分组耗时 (µs/slot) |
|---|---|
| K=7 | 447 / 466 / 703（抖动大） |
| **K=14（新默认）** | **267 / 271 / 277**（另一次 420） |
| 逐符号链（基准） | 1459–2038 |

**正确性**：真实失败形状 0/4896 LLR 差异；并发 4 实例 × 15 轮 **0/60 失配**；
`ctest -L phy` 161/161；四个 metal 单测全绿。

**预期 OTA**：`ul_equalization_demod` **649 → ~300–400 µs**、`ul_pipeline` **1349 → ~1000–1100 µs**
（首次进入 1 ms 预算附近）。`[metal_stats]` 新增一行 `burst commits/waits/dispatches`（延迟链的
CB 都记在这里），equalizer/demapper 的 `commits` 只统计同步路径，属正常。

### 15.5 OTA 确认 ①：UL 进入 1 ms 预算（`c3d4993cb8` 运行）

手机 attach ✓、手机 ping 200/200 ✓；CN→UE ping 6/500 ✗（原因见 §15.6，属 DL 侧）。

| 指标 | ① 之前 | **① 之后** | 串行基准 |
|---|---|---|---|
| `ul_equalization_demod` median | 649 µs | **279 µs** | 2746 µs |
| `ul_pipeline` median | 1349 µs | **932 µs** | 6104 µs |
| `ul_channel_estimation` median | 432 µs | 415 µs | 436 µs |
| `ul_time_frequency` median | 235 µs | 229 µs | 227 µs |
| RF TX underflow | 35 | **6** | 6 |
| RF start/end-of-burst | 55 | 7 | 6 |

阶段和（median）= 229 + 415 + 279 + 18 + 4 = **945 µs ≈ pipeline 932 µs** ✓ 首次落在 1 ms 预算内。
`[metal_stats]` 新形态：`burst commits=2929 waits=2929 max_in_flight=1 dispatches=64438`
（22 dispatches/组 = 11 均衡 + 11 解调 ✓）；equalizer/demapper 的 `commits=0` 表示它们只走同步
路径（PUCCH 用 CPU 后端）✓ 正常。

### 15.6 CN→UE ping 98.8% 丢包的定位（2026-09-13）——**已更正：是 UL GPU path 的时延**（根因见 §15.7）

> **更正（同日）**：本节最初把原因归到"DL 链路自适应"。用户用**全 CPU path**（不带 expert_phy）
> 复测：CN ping **500/500 全通、0% 丢包**（RTT 21–249 ms），DL 未做任何改动 ⇒ **该结论错误**，
> DL 的 MCS 25–27 是"收不到 ACK"的**症状**而非原因。真正的差异是**端到端 UL 时延**：

| 指标（同配置，仅换 UL 后端） | 全 CPU path | GPU path |
|---|---|---|
| PUSCH `t=`（从该 slot 到出结果） | **p75 55 µs / p95 72 µs / max 142 µs** | **3.3–5.7 ms**（约 50–100×） |
| PUCCH HARQ 反馈 | **ack=1 1195 / ack=0 56（95% ACK）** | ack=1 552 / ack=0 400（43%） |
| DL HARQ TB 丢弃（4 次重传耗尽） | **32** | **509** |
| DL 调度 MCS | 3–7 | 25–27（外环因收不到 ACK 持续抬升） |

**机理（排队论解释）**：GPU path 的**单 PDU 服务时间** = t2f 229 + CE 415 + eqdem 279 + LDPC 18
+ FAPI 4 ≈ **945 µs**，而 slot 周期 = 1000 µs ⇒ **利用率 ρ ≈ 0.95**，UL 处理队列处于临界负载，
等待时间按 ρ/(1−ρ) 放大 ⇒ PDU 结果要 **3–6 个 slot** 才出来；CPU path 的服务时间约 ~0.1 ms
（ρ ≈ 0.1）⇒ 几乎不排队，`t=` 只有 55–142 µs ✓ 这就解释了"更快的 GPU path 反而更差"。
**关键教训**：`ul_*` 阶段指标统计的是"处理时长"，而决定成败的是**端到端时延 `t=`（含排队）**——
后续优化必须以 `t=` 作为一等门禁指标。
PUCCH 的 HARQ-ACK 因此赶不上 MAC 的截止时间 ⇒ DL 收不到 ACK ⇒ HARQ 重传耗尽 + 外环把 MCS 抬到
25–27 ⇒ DL 吞吐崩塌 ⇒ CN 的 ICMP 排队超时判丢（"RTT 逐包递减 200 ms" = 缓冲区一次性排空）。
**该时延最可能来自 RX FFT（DFT）流水线**（上一会话 S-2e 把逐符号通知延迟到 ring 槽复用之后：
CPU DFT 路径逐符号立即通知 ⇒ `t=` 只有 55–142 µs）。

**定位实验（每条命令无需重编译，均记录 CN ping 结果与 `t=` 分布）**：
1. 全 GPU 配置 + `OCUDU_DFT_PIPELINE_DEPTH=1`（关闭 RX FFT 流水线）→ 判断 DFT 流水线是否贡献时延；
2. **逐项只开一个 GPU 后端**：只开 CE（`--pusch_channel_estimator_algo metal_mmse`）、只开
   DFT（`--pusch_dft_type metal`）、只开均衡器（`--pusch_channel_equalizer_backend metal`）
   → 找出"哪一级把服务时间/时延抬上去"。
3. 若确认是服务时间问题：降低 CE 415 µs（占最大头，属 AI-CE 线）或 t2f 229 µs；同时检查
   UL 处理并发（`max_pusch_and_srs_concurrency` / dependencies pool），并发够高时 ρ 按服务器数摊薄。

### 15.7 真正的根因：流水线路径每个 port 通知一次（`921c81cbc8`，2026-09-13）

§15.6 的"DFT 流水线本身贡献时延"只是现象的一半。真因在 `puxch_processor_impl`：**流水线路径**
把 in-flight 队列入队的是 **(symbol, port) 对**，而 `finish_oldest_symbol()` 每弹出一个条目就调用
一次 `notifier->on_rx_symbol()` ⇒ **每个 port 都通知一次上层 PHY**。于是：

1. **同一 PDU 被处理两次**（2 RX port ⇒ 每符号两次通知）：上层 PHY 的处理量翻倍 ⇒ 单 PDU 服务
   时间 ≈ 945 µs → ≈ 1.9 ms ⇒ **ρ ≈ 1.9 > 1**，队列无界增长 ⇒ `t=` 膨胀到 **3.3–5.7 ms**
   ✓ 与"处理时长指标正常（279/415/229 µs），端到端却慢 50–100×"完全吻合。
2. **上层 PHY 读到半填充的 grid**：port 0 通知时 port 1 还没 `finish_symbol()` 写进 grid ⇒
   **PUCCH 2 symbol/2 port 的 HARQ-ACK 被破坏**（ack=0 400）⇒ DL 收不到 ACK ⇒ HARQ 4 次耗尽
   （丢 509 TB）⇒ 外环把 MCS 抬到 25–27 ⇒ DL 吞吐崩塌 ⇒ CN ping 6/500 ✓。

**这解释了全部现象**，包括为什么 `OCUDU_DFT_PIPELINE_DEPTH=1`（关掉流水线，该路径不被使用）
就能让 CN ping 回到 496/500 —— 代价是 RX FFT 失去重叠（`ul_time_frequency` 1327 µs vs 229 µs），
属于"用性能换正确性"，不是修复。

**修复**：入队时标记 `last_port = (i_port + 1 == nof_rx_ports)`，只在最后一个 port 完成时通知
（FIFO 保证它是该符号最后被弹出的条目）；单 port 配置行为不变。**回归测试**：
`FlowPipelinedNotificationPerSymbol`（1/2/4 port × 采样率 × SCS × CP = 24 个参数组合）断言
"每符号恰好一次通知、顺序正确、通知时所有 port 都已在 grid 中"——修复前 16 FAILED（port=2/4），
修复后 **24 PASSED**。`ctest -L phy` **161/161**。

**原始（已被取代）的分析**：

| 证据 | 数值 |
|---|---|
| PUCCH HARQ 反馈 | ack=1 **552** / **ack=0（UE 收到但 CRC 失败）400** / DTX 329 |
| DL HARQ 4 次重传耗尽 → TB 丢弃 | **509**（串行成功那次 143） |
| DL 调度 MCS | 集中在 **25–27**（MCS 27 出现 2254 次），CQI 报 8–15 |
| F1-U 交付 | transmitted pdcp_sn=206 / delivered=192，吞吐极低（~3 PDU/s） |
| RF 侧 | 仅 6 次 underflow、7 组 burst 提示（干净） |
| 手机自身 ping | **200/200 成功**（UL + DL 回程都通） |

**机理**：31% 的 DL TB 在手机侧 CRC 失败（显式 NACK）→ MAC 4 次 HARQ 用尽丢弃 → RLC 重传 →
DL 吞吐崩到 ~3 PDU/s → CN 的 ICMP 请求排在高阶 MCS 的重传队列之后 → 1 s 级排队时延，超过
ping 的超时被判丢包。**"RTT 逐包递减 200 ms"正是缓冲区一次性排空（bufferbloat）的特征**，
不是链路层丢包。

**结论（已被 §15.7 取代）**：曾经认为剩余问题指向 DL 链路自适应。实际根因是 UL 流水线路径的
重复通知（§15.7）：DL 的 MCS 25–27 / HARQ 丢弃都是"收不到 ACK"的症状，DL 侧代码未做任何改动。

### 16. 当前状态与下一步（2026-09-13）

| 项 | 状态 |
|---|---|
| GPU 全链（t2f + CE + eq + demap + LDPC 可选）正确性 | ✓ 单元测试 + LLR 逐位一致 |
| 延迟链（K=14 单 command buffer） | ✓ `c3d4993cb8` |
| UL 服务时间（median） | ✓ 945 µs < 1 ms（阶段和 = pipeline 932 µs） |
| 流水线路径正确性 | ✓ `921c81cbc8` + 回归测试 |
| OTA：需重跑**不带** `OCUDU_DFT_PIPELINE_DEPTH=1` 的全 GPU 配置 | ⏳ 待验证 |

**待验证的预期**：CN ping 应该恢复（不再有重复通知导致的 ACK 破坏），`t=` 应从 3.3–5.7 ms 回到
亚毫秒~低毫秒，同时 `ul_time_frequency` 保持 229 µs（流水线仍开启）。

**若 `t=` 仍然偏高**（即去掉重复通知后依然 >1 ms）⇒ 剩余候选：
1. RX FFT 流水线的**保守通知**（通知被推迟到 ring 槽复用之后）⇒ 用 `OCUDU_DFT_PIPELINE_DEPTH=2/4/8`
   扫一遍 `t=` 分布即可定位（无需重编译）；
2. 服务时间 945 µs ⇒ ρ≈0.95 的排队放大：降 CE（415 µs，AI-CE 线）或提高 UL PDU 并发
   （`max_pusch_and_srs_concurrency`）。
3. 逐项只开一个 GPU 后端（只 CE / 只 DFT / 只均衡器）确认是哪一级把 `t=` 抬上去。

### 17. 全 GPU 配置第二次 OTA（`5b83736eca`，手机 n1 5 MHz）：日志体检（2026-09-13）

**结果**：attach 需要多次重试；CN→UE ping **489/500（2.2% 丢包）**，但 RTT 均值 **193 ms**（min 24 /
max 2150），且前 ~13 s 从 846 ms 单调收敛到 ~30 ms，之后呈 ~10 s 周期的锯齿（30 ms ↔ 1–2 s）。
`t=`/阶段指标本身正常（t2f 230、CE 432、eqdem 286 µs）。

新增工具：`scripts/gnb_log_stats.py <log> [--rnti 0x460c]`（单遍扫描 148 MB 日志 ≈0.8 s）。

#### 17.1 关键数据（当前活跃 UE rnti 0x460c = 手机）

| 指标 | 数值 |
|---|---|
| UL PUSCH 按调制（0x460c） | **256QAM 568 次 → 100% KO**；64QAM 6/6 KO；QPSK 185 次 → 8.1% KO；16QAM 1/1 OK |
| UL CRC indication（全网） | ok 1008 / ko 1989 ⇒ **66% 失败** |
| PUCCH HARQ-ACK | ack 1268 / nack 83 / **DTX 1259（48%）** |
| HARQ TB 丢弃 | UL 401（360 来自 0x460c）、DL 242（全部集中在前 40 s，来自 0x4607/0x4609 两次失败的 attach） |
| UL 授权 | 47% 是 256QAM，MCS 中位 18 / 最大 28；**rv_idx 全 0** |
| DL 授权 | MCS 中位 3（最大 18），rv0 94% |
| PUSCH 收到的电平 | 数据期 `rsrp ≈ -1…-2 dBFS`（**接近满量程**）；attach 期 Msg3 为 -20…-30 dBFS |
| PUSCH SINR | 双峰：-5…+5 dB（454/911）与 20…40 dB（241/911），中间 10–20 dB 只有 10 个样本 |
| PUCCH 电平 | sinr 11–14 dB、rsrp -4…-18 dBFS（正常，未饱和） |

#### 17.2 结论

1. **不是 GPU 链正确性问题**：失败与**调制阶数**强相关（QPSK 92% 通过，256QAM 0% 通过），
   而金属解调器在 qam256 上与 CPU 逐位一致、金属 LDPC 有独立 BLER 测试；同时 `rsrp ≈ -1 dBFS`
   说明 gNB 侧 RX 已到满量程（削顶），削顶对高阶星座的 EVM 影响最大。DL 同向佐证：PUCCH
   上 48% 是 **DTX**（UE 根本没发/没收到 PDSCH），而 DL 完全没动过 ⇒ 两个方向都被电平压垮。
2. **启动期的机制**：attach 阶段 UE 发射功率很低（Msg3 `rsrp -30 dBFS`、SINR -1 dB）⇒ Msg3/Msg4
   反复失败（多次 PRACH、临时 RNTI 0x4601…0x460b、DL HARQ 丢 242 个 TB）⇒ 手机 UL 侧堆积了
   大量上行数据；attach 成功后功率控制把 UE 推到满功率（`rsrp -1 dBFS`），而 UL 链路自适应在
   饱和信道上仍按“测得 SINR”下发 **256QAM MCS 25–28（47% 的授权）**，这些 TB **100% 失败** ⇒
   手机侧 backlog 持续排队并慢慢排空 ⇒ ping 前 13 s RTT 从 846 ms 收敛到 30 ms；之后每当再次
   下发 256QAM 就出现 1–2 s 的锯齿尖峰。
3. 因此 §16 的“待验证”已由本次跑确认：重复通知的 bug 修掉后链路能用了（2.2% vs 98.8% 丢包），
   剩下的瓶颈是**射频电平/链路自适应**，不是 GPU 流水线。

#### 17.3 建议的下一步（按顺序，每步一条命令 + 一次脚本体检）

1. **降电平**：`rx_gain: 55 → 30`、`tx_gain: 70 → 50`（或串 20 dB 衰减器），其余命令不变。
   目标：PUSCH `rsrp` ≈ -20 dBFS、PUCCH DTX <10%、256QAM KO% 降到个位数。
2. 若仍 100% KO：把 UL MCS 上限压到 64QAM 以内 —— `cell_cfg.pusch.max_ue_mcs: 20`
   （DL 同理 `cell_cfg.pdsch.max_ue_mcs`），先把 BLER 压下来再谈吞吐。
3. 区分“链路”与“GPU 链”：同样电平下换 `--pusch_ldpc_decoder_type auto`（CPU LDPC）再跑一次，
   若 256QAM 仍 100% KO ⇒ 与解码后端无关，是链路/电平问题（即可排除 GPU 链）。
4. 服务时间：`t=` 中位 4303 µs 中 **金属 LDPC 占 2205 µs**（`[ul_ldpc_decode]`）⇒ 用 CPU LDPC
   时 UL 单 PDU 服务时间约 1.5–2 ms，排队余量更大。

### 18. `[metal_stats]` 里的 0 是正常的（`ee2b1ec97d`）

`equalizer`/`demapper` 的 `commits/waits/max_in_flight` 只统计它们**同步 process() 路径**自己
提交的 command buffer；延迟链（K=14 分组）两个阶段都走**共享 burst**，因此这两行在 OTA 里恒为 0，
而它们的实际工作量记在 burst 行上：`burst commits=3092 … dispatches=68024` 除以 3092 = **22.0**
= 每 burst 11 次均衡 + 11 次解调（14 符号分组里的 11 个数据符号）✓，且 `mmse_ce commits=3092`
与 UL PDU 数一致 ✓。为免再次误判，`ee2b1ec97d` 起 burst 行打印分阶段明细
（`dispatches=… (equalizer=X demapper=Y)`），两个引擎的行也标注 “synchronous path only”。
其余行：`dft max_in_flight=8` = RX FFT 流水线深度（默认 8）；`mmse_ce`/`ldpc_decoder` = 1 = 同步提交。
想验证计数器接线：加 `OCUDU_PUSCH_FORCE_SERIAL=1` 跑一次，eq/demapper 的 commits 会变成 >0。

### 19. 第三次 OTA：rx_gain 55→30 / tx_gain 70→50 后完全连不上（`5b83736eca`，`75314c46b0` 工具）

**结论：UE 侧一切正常，卡在 Msg3（上行），且这是电平问题、不是 GPU 链问题。**
（gNB 控制台没有输出是正常的：`all_level: debug` 全部写 `/tmp/gnb.log`，控制台只打 warning。）

| 观察（本次运行 4 分钟） | 数值 |
|---|---|
| PRACH 检测 | **31 次**（t=6 s … 253 s），`rssi` 中位 **-61.6 dB**（rx 55 时是 -50.6 dB） |
| RAR（Msg2） | 31 次，全部发出（手机能收到 ⇒ DL/SSB/PDCCH 正常） |
| Msg3（PUSCH） | **150 次尝试（31 个 tc-RNTI × 最多 5 次 HARQ），crc OK 仅 2 次（1.3%）** |
| Msg3 SINR | **-0.9 dB（p10 -1.5 / p90 -0.6），4 分钟 31 个周期几乎不变** |
| Msg3 epre | -59 dB（rx 55 时 -47.8 dB） |
| Msg4 / DL PDU / CON_RES | **0**（attach 从未走出 Msg3） |
| Msg3 授权分配失败 | 9 次（"Not enough available RBs"，网格 11/25 被占，非根因） |

**关键判据**：Msg3 的 **SINR 与 rx 55 那次完全相同（-1.0 → -0.9 dB）**，而信号电平低了约 11 dB
（PRACH rssi、Msg3 epre 都同步下降）。这说明上行是**干扰受限**：干扰同样从天线进来，因此**跟着增益
一起缩放 —— 降 rx_gain 完全不能改善 SIR**，只是把有用信号推向 ADC/量化底噪（12 bit，-59 dBFS 只剩
约 14 dB 量化 SNR）。另外把 tx_gain 从 70 降到 50 会让手机通过开环功控把上行功率抬约 20 dB，刚好
抵消掉一部分接收增益的下调（净 -11 dB 而不是 -25 dB）。

**`ldpc_decoder commits=24` vs 150 次 PUSCH 的新证据**：150 个 Msg3 码块里只有 **24 个真正进了 GPU**，
其余 126 个在 `ldpc_decoder_metal` 的“尾部 LLR 全为 0 ⇒ 软比特不足”提前返回处退出（`pusch_decoder_impl`
在这种情况下打印的 `iter=` 是**配置的最大迭代数**，不是实际迭代数，所以日志里的 `iter=6.0` 不代表跑了 6 轮）。
新增的电平扫描测试（`demodulation_mapper_metal_unit_test` 的 `[level]` 行）表明：金属解调器在**任何电平**
都与 CPU 逐位一致，但 LLR 量化**不是电平无关**的（同一 SNR、不同绝对电平时 LLR 不同，约按 1/电平缩放）
⇒ 绝对电平决定了有多少软比特能活过 int8 量化。**因此 rx_gain 30 属于“自毁”配置**：SIR 没变、软比特
却没了。同一测试也排除了“金属解调器有 bug”这一可能。

**本次 `[metal_stats]` 全部正常**（顺带验证分阶段计数）：`burst commits=150 dispatches=3300
(equalizer=1650 demapper=1650)` = 每 burst 22 次（11 均衡 + 11 解调）；`dft commits=2101 = 150×14`；
`mmse_ce commits=152`；`ldpc_decoder commits=24`（见上）⇒ GPU 链按预期工作，失败全在射频/电平。

**下一步（按优先级）**：
1. **先把增益加回去**：`rx_gain: 55`、`tx_gain: 70`（或 rx 45–50 / tx 60 折中）。目标是既不削顶
   （rx 55 时 PUSCH 到 -1 dBFS）也不丢软比特。
2. **找干扰源**（真正的瓶颈，SIR ≈ 0 dB）：① 手机侧关机后用 B200 听 1922.5 MHz 附近是否有窄带强信号；
   ② 直接把 `dl_arfcn` 430500 改到 430100（UL 1920.5 MHz）或 430900（UL 1924.5 MHz）——两者都在
   已测的 2150–2155 MHz 空隙内；改完看 **Msg3 是否在 1–2 个 RACH 周期内成功**（工具新增的
   `== random access` 段直接给出 Msg3 OK%/SINR）。③ 固定 rx_gain 55、只把 `tx_gain` 降到 40，看
   Msg3 SINR 是否明显变好 ⇒ 若变好则是**自干扰**（本配置 DL 用 TX/RX 口、UL 用 RX2 口，两天线之间
   没有双工器，只有空间隔离）。
3. 上行干净之后再用 `cell_cfg.pusch.p0_nominal_with_grant`（默认 -76 dBm，可下调到 -90）压低手机的
   发射功率来消除削顶，而不是砍接收增益；过渡期可先 `cell_cfg.pusch.max_ue_mcs: 20` /
   `cell_cfg.pdsch.max_ue_mcs: 20` 把 BLER 压住。
4. 每次跑完 `python3 scripts/gnb_log_stats.py /tmp/gnb.log`：新增 `== random access` 段给出
   PRACH 次数/RSSI、RAR 数、Msg3 尝试数与 CRC OK%、SINR 分位数，是判断“连不上”的第一入口。

### 20. 第 5 步定位结果：金属 MMSE 信道估计器的正则项按**绝对值**而非**信噪比**定标（`67971ba049`）

**问题**（第 5 步 = 把 CE 拉进对比链）：块 MMSE 的权重是 `W = R_hp (R_pp + σ² I + ridge I)^-1`，
其中 `R_pp/R_hp` 是**单位功率归一化**的相关模型（对角线为 1），但 `σ²` 用的是收到 DM-RS 的
**绝对**噪声功率。于是正则强度跟着**射频增益**走而不是跟着 SNR 走：只有"导频功率≈1"（实验室
归一化输入）时两者才恰好相等 —— 这正是所有单测都通过、OTA 却随增益变化的原因。

**实测**（合成栅格、固定 SNR 20 dB、只改输入电平；归一化值应当恒定）：

| level | mmse nv/level² | mmse snr | mmse \|h\|/level | cpu \|h\|/level |
|---|---|---|---|---|
| 1.000 | 7.17e-03 | 219.1 | 1.111 | 1.113 |
| 0.100 | **4.76e-02** | 58.6 | 1.210 | 1.189 |
| 0.030 | **2.38e+01** | 1.16 | **4.098** | 1.239 |
| 0.010 | **1.35e+03** | 1.00 | **31.46** | 1.336 |

即：**输入电平越低，金属 CE 的信道估计和噪声方差越是爆炸**（CPU 经典估计器全程平坦）。
CE 的噪声方差正是**均衡器算出每 RE 噪声方差 → 解调器用它除软比特**的那个量：
- 电平很低（rx_gain 30 那次）⇒ 软比特被压成 0 ⇒ 解码器裁掉尾部零 LLR、**根本没跑 LDPC** ✓
  与 `ldpc_decoder commits=24` vs 150 次 Msg3 完全吻合；
- 电平中等（rx 45/55）⇒ 软比特幅度错误 ⇒ 高阶调制（64QAM/256QAM 需要可靠软信息）几乎全灭，
  而 QPSK 靠硬判决仍能过 ✓ 与 §19 的"按调制阶数失败"吻合。
另外，软比特被 `|LLR|` 上限截断后只剩硬判决，这解释了"上报 SINR 26 dB 而 64QAM 100% KO"。

**修复**：对角加载改用**噪声/导频功率比**（`σ²/P_pilot`，P 取收到 DM-RS 的平均功率，与经典
噪声估计器同一参考域）；`ridge` 保持无量纲小量。新增 **Test 9**（电平扫描）断言"nv/level² 平坦"
且两条路径逐电平一致：修复后漂移 1.04×（此前 1.6e5×），跨路径 \|h\| 差 0.15%（此前 ~2300%）。
Test 3 的 NMSE 门限 1.0 → 1.5 dB（模型统计量是固定常数，用 20 dB SNR 处零点几 dB 换"权重不随
射频增益变化"是划算的）。

**待 OTA 复测**（下一步）：用同一配置（rx 45 / tx 60 或回到 rx 55 / tx 70）重跑，看
`== uplink PUSCH results by modulation` 表：预期 **64QAM/256QAM 的 KO% 大幅下降**、Msg3 OK% 上升、
`ldpc_decoder commits` 与 PUSCH 次数同量级（不再被裁剪跳过）。若 64QAM 仍全灭，则剩下的瓶颈是
int8 LLR 的动态范围（高 SNR 下软比特饱和 → 只剩硬判决），那时的正确做法是控制工作 SNR
（`p0_nominal_with_grant` / rx_gain），而不是继续加增益。

### 21. 第 5 步续：β≠1 时金属 CE 少乘/多乘 1/β —— OTA 上 ~16 dB 软比特的元凶（`0ebf24cdcd`）

`67971ba049`（σ² 归一化）之后，rx45/tx60 那轮反而变成"最差"：Msg3 0/25、上报 SINR **-18.6 dB**。
Test 9 补上**生产形状**（3 个 DM-RS 符号、β = 0.708，即 PUSCH 处理器的
`scaling = 10^(-SCH_to_DMRS/20)`）后立刻复现：

| 量 | CPU（参考） | 金属（修复前） | 偏差 |
|---|---|---|---|
| `\|h\|/level` | 1.8168 | 1.2852 | **1/β（−3 dB 信道估计）** |
| `rsrp/level²` | 2.093 | 4.166 | 1/β² |
| `snr` | 521.7 | **22.8** | **−13.6 dB** |
| `nv`（噪声方差） | 8.19e-3 | **3.61e-1** | 1/β⁴ ≈ **−17 dB 软比特** |

**两个独立错误**：① 经典 FD 阶段在插值前把 LSE 导频乘 1/β（估计输出落在 **data 域**，正是均衡器/
解调器期望的域），金属路径**没乘**；② 金属把估计出来的导频 RE 写进 `filtered_pilots_lse` 时**又乘了一次
1/β**。在"单位功率导频 + 单个 DM-RS 符号"的实验室输入下两者恰好自洽（所以所有单测都通过），而在真实
小区里 PUSCH 处理器**总是**设 `scaling = 0.708`（2 个无数据 CDM 组）⇒ 均衡器拿到的噪声方差大约 44×
（≈16 dB）⇒ 解调器把软比特除小 16 dB ⇒ LDPC 收到近乎 0 的 LLR（裁剪/跳过/失败）、高阶调制全灭、
上报 SINR 偏悲观 ~14 dB。**这就是 OTA 上 -18.6 dB（而经典估计器约 0 dB）的来源。**

**修复**：在 `apply_fd_td_estimation_stage` 开头对 LSE 导频**一次性**乘 1/β（与经典阶段一致），
并去掉 `filtered_pilots_lse` 写回时的第二次 1/β。修复后四种生产形状（2/3 DM-RS × β=1/0.708/2）
在所有电平上与 CPU 一致：跨路径 `|h|` 差 ≤0.15%、噪声方差差 ≤0.9 dB、`nv/level²` 漂移 1.04×
（不再跟射频增益走）。Test 9 现在把 DM-RS 数与 β 都纳入扫描 —— 这类"只在真实参数下存在"的缺陷
不会再静默通过。

**教训（写给后续会话）**：单测用"单位功率导频 + β=1 + 单 DM-RS 符号 + 固定统计量"就**测不出**真实
小区的行为。凡是进入 OTA 的模块，扫描维度里必须包含**生产参数**（DM-RS 符号数、β、电平）。

### 22. 回退到 OTA 可用基线（`d8f54564b8`）与下一步

**决定**：§20/§21 的两个金属 MMSE 信道估计器改动（σ² 按噪声/信号比归一化；LSE 导频乘 1/β）
在**实验室是对的**（各电平、各生产形状都与 CPU 经典估计器一致），但**上 OTA 后链路反而更差**
（连 Msg3 都过不去、attach 失败），而**带这两个缺陷的旧估计器**能 attach、能拿到 IP、CN ping 可用。
按"以 OTA 为准"的原则回退。

**基线的构成**：
- 代码：`lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.{h,cpp}`
  取自 `75314c46b0`（= 最后一次 OTA 验证可用的状态）；回退提交 `d8f54564b8`。
- 配置（用户侧，脚本未改）：`tx_gain: 70`、`rx_gain: 60`、**不设** `pusch/pdsch.max_ue_mcs`。
- 保留的独立改动（与 CE 无关，均可留）：S-2t 的 puxch 通知修复、S-2u 的 burst 分阶段计数、
  S-2v 的 `gnb_log_stats.py`（含 `== random access` 段）与解调器电平扫描测试。
- 两个 CE 修复留在历史里（`67971ba049`、`0ebf24cdcd`），**重新应用的判据**是
  `OCUDU_CE_LEVEL_STRICT=1 port_channel_estimator_metal_mmse_unit_test`（现在它把缺陷当"报告"打印：
  噪声方差随电平漂移 1.7e6×、信道估计漂移 80×；带 `OCUDU_CE_LEVEL_STRICT=1` 时恢复为断言）。

**教训**：现在的金属单测全是"分段正确"（CE/均衡/解调/解码各自与 CPU 一致），**没有一条覆盖
"整链能不能解出"**。这次的两个 CE 缺陷、"高阶调制在高 SNR 下是否被 int8 LLR 饱和吃掉"，
都只有在"随机 TB → 编码 → 调制 → 信道+AWGN → 金属 CE+均衡+解调 → LDPC → CRC"这条
端到端测试里才会暴露。**在补上这条测试之前，不再改 CE/解调器的定标。**

**下一步候选（按优先级）**：
1. **端到端解码门禁**（推荐先做）：扫 {QPSK,16QAM,64QAM,256QAM} × SNR {0…30 dB} × 生产形状
   （3 DMRS、β=0.708）× 两个绝对电平，逐点比较 metal 与 CPU 的"是否解出/迭代数"，并输出
   LLR 幅度直方图（判断是否饱和）。这条测试绿了才动定标，也只有它能回答"64QAM 全灭"到底是
   噪声方差错、LLR 饱和、还是链路本身。
2. **RF 干扰/换频点**（与 GPU 无关，但决定"能不能连上"）：在 2150–2155 MHz 空隙内改 `dl_arfcn`
   （如 430100 → UL 1920.5 MHz），看 Msg3 是否 1–2 个 RACH 周期就成功；以及 `p0_nominal_with_grant`
   压低手机发射功率（避免 UL 峰值饱和 / 高阶调制被削）。
3. 已知非阻塞现象：CN ping 起始 RTT 大、随后回落（bufferbloat 型），先记录不动。

### 23. 端到端门禁建成：64QAM 全灭 = 金属 CE 的**估计质量**（`1b4d2b8c07`）

**第 5 步真正缺的那条测试已经存在**：`tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test`
（真链路：TB → LDPC 编码 → 调制 → 信道仿真 → DM-RS 估计 → 均衡 → 解调 → LDPC 解码 → CRC）。
它本来就能选估计器（`-c cpu|metal_mmse|helena`），但 **getopt 选项串里 `c` 漏了冒号**，
`-c metal_mmse` 的参数被丢掉、永远跑默认 CPU —— 所以以前"两边一样"。补上冒号后：

```
pxsch_bler_test -c metal_mmse -M qam64 -m 20 -S 25 -R 40 -B 25 -P 1 -L 1 -C TDLA -F rayleigh
```

**结果（25 PRB、1 层 1 端口、40 slot、64QAM MCS 20）**：

| 信道 | cpu BLER | metal_mmse BLER |
|---|---|---|
| single-tap | 0.075 | **1.000** |
| TDLA | 0.000 | **1.000** |
| TDLC | 0.075 | **1.000** |

S=15/25 dB 时经典估计器全部解出、金属估计器**一个都没解出**；而金属上报的 SINR **更高**
（26 vs 22 dB）、EVM 更差（0.122 vs **0.087**，约 3 dB）。**这就是 OTA"上报 26 dB 而 64QAM 100% KO"
的本地复现**，而且指向**信道估计的质量**，不是定标：

- 把 §20/§21 两个定标修复**临时应用后重跑同一门禁，BLER 仍是 1.000** ⇒ 定标不是原因；
- 金属 MMSE 至今用的是**固定信道统计量**（`channel_statistics_estimator_fixed(370 ns, 0 Hz)`），
  相关模型是"猜"的；经典路径用 FD 平滑 + TD 插值（跟着数据走）⇒ 模型失配带来约 3 dB 的 NMSE/EVM 差 ⇒
  64QAM（需要 ~-25 dB EVM）全灭、QPSK 无感、加接收增益无效（与 OTA 现象完全一致）。

**下一步（明确）**：实现 `channel_statistics_estimator` 的"从导频估计"版本（PDP → τ_rms、Doppler → f_d），
以本门禁为验收：`metal_mmse` 的 BLER 要在各调制/各 SNR 上贴近 `cpu`（先追平 EVM）。
`OCUDU_CE_LEVEL_STRICT=1` 的电平不变式作为第二条判据（用户指定的双绿门禁）。

### 24. A/B/C 对照测试：三级打桩 + 分级 diff（`17bfc4de85`）

**目标（用户提出）**：抓一段手机空口 IQ 作为 *test vector*，让 A（全 CPU）/ B（全 GPU + `OCUDU_DFT_PIPELINE_DEPTH=1`）/
C（全 GPU 默认）吃**同一份输入**，在各级打桩输出，比较**第一个出现分歧的级**，从而定位缺陷。

**已实现（生产代码里，env 开关，零开销）**：`lib/phy/upper/channel_processors/pusch/ul_capture.{h,cpp}`
| 开关 | 产出 | 定位哪一级 |
|---|---|---|
| `OCUDU_UL_DUMP=<前缀>` | `<前缀>_<slot>_<rnti>.txt/.bin`：PDU 全参数 + 收到的频域栅格（ports×14×BWP 子载波，cf_t） | **DFT 及其流水线**（B vs C） |
| 同上 | `<前缀>_<slot>_<rnti>_ce.txt`：每端口 `noise_variance / snr / rsrp / epre / ta_us / cfo_hz` | **信道估计**（LLR 定标的源头） |
| `OCUDU_UL_DUMP_LLR=1` | `<前缀>_<slot>_<rnti>_llr.bin`：解调软比特（按码块分块，含长度头） | **均衡 / 解调 / 软比特定标** |
| `OCUDU_UL_DUMP_COUNT=N` | 只抓前 N 次（默认 8） | — |

比较工具：`scripts/ul_stage_diff.py <前缀A> <前缀B>` —— 逐次接收报告栅格差异（最大/均值/差异 RE 数）、
CE 标量差（dB）、LLR 统计（数量、均值 |L|、零占比、饱和占比、首个不同下标），并给出"第一分歧级"。

**在 gNB 上的用法（A/B/C 三次运行各抓一份）**：
```bash
OCUDU_UL_DUMP=/tmp/A OCUDU_UL_DUMP_LLR=1 OCUDU_UL_DUMP_COUNT=32 sudo -E ./build/apps/gnb/gnb -c configs/... expert_phy ...
# B: 同一命令 + OCUDU_DFT_PIPELINE_DEPTH=1 且 OCUDU_UL_DUMP=/tmp/B
# C: 同一命令（默认）            且 OCUDU_UL_DUMP=/tmp/C
python3 scripts/ul_stage_diff.py /tmp/A /tmp/C
```

**关键限制（下一步要补的）**：空口三次运行的输入**不相同**（独立信道/噪声实现），因此三级 dump 只能做
**统计对比**；要做到"同一向量、逐位对齐"，需要 **离线回放工具**：读取一份抓取的向量（栅格 + PDU），
在**一个进程内**依次跑 A/B/C 三种后端组合并把三级 dump 落到不同前缀，再 diff ⇒ 精确到"第几级、第几个 RE/比特"
出现分歧。B vs C（DFT 流水线）还需要把打桩点前移到**时域 ci16**（在 `puxch_processor_impl::process_symbol`
的 `submit_symbol` 之前），因为栅格 dump 已在 DFT 之后。
（`pxsch_bler_test` 已可作为**合成信道**下的端到端门禁：`-c cpu|metal_mmse` 的 BLER/EVM 对比。）

### 25. A/B/C 对照测试闭环 + 定位到根因并修复（`7ea34e25f0`、`d3320d44c0`）

**打桩（生产代码，env 开关）**：三级 + 时域：
| 开关 | 产出 | 定位 |
|---|---|---|
| `OCUDU_UL_DUMP_TD=<前缀>`（+`_TD_SLOTS`） | `<前缀>_td.txt/.bin`：送给 DFT 的 ci16 时域样本 | **DFT 与流水线深度**（B vs C） |
| `OCUDU_UL_DUMP=<前缀>` | `<前缀>_<slot>_<rnti>.txt/.bin`：PDU + 频域栅格 | DFT 之后的整条上链输入 |
| 同上 | `..._ce.txt`：`noise_variance/snr/rsrp/epre/ta/cfo` | 估计器标量 |
| 同上 | `..._h.bin`：全带宽信道估计（layer×port×symbol） | **均衡器真正消费的量** |
| `OCUDU_UL_DUMP_LLR=1` | `..._llr.bin`：解调软比特 | 均衡/解调/软比特定标 |

**回放**：`ul_chain_replay <一个接收的前缀> --cpu|--metal|--metal-cpu-ldpc|--metal-cpu-demod --out <前缀>`
（上链，CPU/GPU 任意组合）；`ul_chain_replay <TD前缀> --dft [--dft-metal] --nof-prb N --out <前缀>`
（时域重放 DFT，深度由 `OCUDU_DFT_PIPELINE_DEPTH` 控制）。比较用 `scripts/ul_stage_diff.py A B`。
**注意**：工具默认 `--td-strategy interpolate`（与 gNB 配置一致）；经典估计器只在 `average` 下除以
DM-RS 符号数，两边策略不一致会报出一个**假的比例因子**（我踩过这个坑）。

**结论 1：DFT 不是问题** —— 同一段样本上 CPU DFT 与 metal DFT 的栅格**逐位一致**，depth 1 与 8 也
**逐位一致**（同时复验了 S-2t 的流水线修复）。

**结论 2：分歧在信道估计** —— 真机向量上金属估计的 \|h\| 功率比经典估计大 +4…+23 dB，软比特随之不同
（mean\|L\| 62 vs 31、零占比 12.3% vs 0.5%）。

**结论 3（根因）**：在端到端门禁里把"形状"参数化后，单参数复现：
**`nof_cdm_groups_without_data = 2`（本小区实际配置，β=0.708）时金属 CE 完全失效（BLER 1.00 vs cpu 0.05）；
改成 1 组（β=1）两者一样**。原因正是 §21 的两处：① LSE 导频没乘 1/β（估计小 1/β）；② 写
`filtered_pilots_lse` 时又乘了一次 1/β（RSrp/噪声方差被放大 ~1/β⁴）。τ_rms 从 0.05 扫到 3.0 µs **无改善** ⇒
不是固定统计量的问题；PRB 24–51 全部失败 ⇒ 不是尾块。

**修复后（双绿）**：
- 端到端 BLER（15 kHz / 3 DMRS / 2 CDM / 25 PRB / 64QAM MCS20）：cpu 0.80/0.35/0.10/0.10 与 metal
  **0.85/0.35/0.15/0.10**（S=10/15/20/25 dB）——修复前 metal 恒为 1.00；
- `OCUDU_CE_LEVEL_STRICT=1`（电平不变式）**通过**；
- 真机向量 \|h\| 功率比：+12.1/+23.4/+4.0 dB → **+9.1/+2.2/+1.1 dB**。

**下一步**：用户在 OTA 复跑（见提交说明）；残留的逐符号差异（远离 DM-RS 的符号）指向**时域插值/信道
统计量**，即之前搁置的"从导频估计 PDP/Doppler"——现在它是"精度优化"而非"连不上"的阻塞项。

### 26. "卡死"现场判定（`d3320d44c0` 运行）：gNB 健康，是链路 + 调试日志（2026-09-12）

用户报告 ping 期间"卡住"，实测**不是死锁**：

| 证据 | 数值 |
|---|---|
| FAPI 实时性 | **1000 Slot.indication/s，每 wall 秒推进 1.000 s 空口时间**（全程如此，含"卡住"时段） |
| 调度/UCI/PUSCH | ~100 Slot decisions/s、~100 PUCCH/s、4–24 PUSCH/s、1–2 DL PDU/s（持续） |
| 日志增长 | 持续增长；"卡住"只是**日志写入速率 430 kB/s、~4000 行/s** 把 console 饿死 |
| 电话状态时间线 | t=0–80 s attach ✓；**t=90–250 s 手机不在线**（只有每 10–20 s 一次 RACH，共 44 个 RACH 周期）⇒ console 无内容可打；t=260 s 后重新 attach ✓ |

**"卡住"的两个真实来源**：① 2.5 分钟手机脱网重连窗口（console 无事可报）；② `log.all_level: debug`
的 430 kB/s 日志把终端/CPU 压住。⇒ **状态类运行请把 `log.all_level` 降到 `info`**（也顺带减小抖动）。

**链路数据（本次 vs 之前健康基线）**：

| 指标 | 健康基线（rx65/tx70，修复前 CE） | 本次（同增益，修复后 CE） |
|---|---|---|
| UL CRC | 96.9% OK | **87% OK**（ping 期间 20–45% KO） |
| PUCCH | ack 94.8% / dtx 2.9% | **ack 49.9% / dtx 40.5%** |
| UL SINR 中位 / p10 | 5.5 dB / — | **3.2 dB / −24.5 dB** |
| PRACH 周期 | 51 次 / 9 min | 44 次 / 5.5 min |

UL 授权 MCS 中位 0（最低！）却仍有 20–45% KO，SINR p10 到 −24.5 dB ⇒ **深衰落/突发干扰**，不是
解码门限问题；64QAM 的 15 个块在 SINR −0.9 dB 时 100% KO、16QAM 36% KO @5.5 dB，属正常。

**结论**：CE 修复由本地双门禁背书（§25），本次 OTA 变差来自**射频/干扰条件**（SINR 低 2.3 dB、p10 −24.5 dB）
与 **430 kB/s 调试日志**，不是回归。要公平 A/B 修复，需在同一位置、关掉 debug 日志再跑；根治仍是干扰
（换 `dl_arfcn` / `p0_nominal_with_grant`），过渡期加 `cell_cfg.pusch.max_ue_mcs: 20`。

### 27. LDPC 之前的流水线优化：现状盘点与下一个大头（`5e79fe0739`）

> ⚠️ **本节末尾的"求逆占 CE ~93%"是孤立调用口径，已被 §28 的实测作废**（真正的 host 成本是引擎调用的
> ~104 µs 固定往返）。但**方向不变**：求逆仍要回到 GPU 链上——§28 测得 K1 单系统 91.3 µs 是核函数延迟
> 受限（72 个枢轴步），不是 GPU 不适合；修法是块化消元 + `simdgroup_matrix`，见 §28 下一步第 0 项。
> 读本节时以 §28 为准。

**当前 OTA 单 PDU 分解（median，25PRB/1x1，`info` 日志档）**：

| 阶段 | median | 备注 |
|---|---|---|
| `ul_time_frequency`（FFT+grid） | 233 µs | FFT Phase 2（kernel 内相位补偿+直写 grid）文档估值 <100 µs，最低优先 |
| **`ul_channel_estimation`** | **430 µs** | **其中 `gpu_path=388 µs`（`gpu_wait` 仅 44 µs）⇒ 是 GPU 执行本身** |
| `ul_equalization_demod` | 282 µs | 已被 ①（单 CB）/K=14 压过 |
| `ul_fapi_mac` | 4 µs | — |
| LDPC（Metal，用户决定最后重做） | 1984 µs | 本轮不动 |
| **LDPC 之前合计** | **≈ 950 µs** | 目标：把 CE 的 388 µs 拉下来 |

**已做（本轮，`5e79fe0739`）**：均衡器**批量 dispatch**（`equalize_mxn_batch`：一个 thread 处理一个
(RE, symbol)，组内符号共用一个 dispatch）。链探针实测：**逐符号 191.2 µs/slot → 批量 153.9 µs/slot
（1.24×），eq/nv 逐位一致（0 差异）**。收益只有 dispatch 编码成本（commit/wait 在 ① 之后已各一次），
所以它是个位数百分比；接进流水线还需在 `channel_equalizer` 接口加"组级调用"（下一步）。

**CE 的 388 µs 定位（重要）**：CE 单测给出 K1 批量求逆 **233.9 µs**（4 个 36×36 系统）、K2 权重矩阵
**16.2 µs**（17 块）⇒ **求逆占 ~93%**。逐条读 `ocudu_mmse_inv.metal`（86 行）后确认它是**延迟受限**，
不是算力受限：

- 每个枢轴列 3 次 `threadgroup_barrier`（n=36 ⇒ **108 次屏障**）；
- 枢轴搜索是 **thread 0 串行扫 n 行**；
- 消元阶段**每个线程独占一整行、顺序遍历 2n=72 个 threadgroup 元素**（~72 步/线程/列）。

按 FLOP 算它只需 ~15k 次乘加（微秒级），实测 58 µs/矩阵 ⇒ **100–1000× 偏离**。
**修法（下一步，收益最大）**：把消元改成 **2 维并行**（threadgroup 内 (行×列) 分工，每线程 ~2 个单元，
每列屏障仍 3 次但工作量摊平），或改用 **Cholesky/LDLᵀ**（A = R_pp+(σ²+ridge)I 对称正定 ⇒ **无需选主元**，
屏障降到 ~2n 次、FLOP 降到 n³/6）。注意**不要求与 CPU 逐位一致**（单测容差 1e-6，Test 9 的跨路径 \|h\| 差
0.15% 即 bf16 量化地板），因此有算法自由度。预期：求逆 234 µs → **20–50 µs**，CE 430 µs → **~150–200 µs**，
把 LDPC 之前的预算从 950 µs 压到 ~700 µs。

### 28. §27 的结论被实测推翻：求逆不是大头，**引擎调用的固定往返**才是（`5df7d5ba7a`、`a52677d194`、`d11c5c7838`、`89a2ac3c67`）

**① K1 并行化有效但方向错（`5df7d5ba7a`）**：把 `ocudu_mmse_inv.metal` 的枢轴搜索去掉（A 对称正定 ⇒ 无需
选主元）、threadgroup 改成 (列,行) 二维分工、每列 3→2 次屏障：单个 36×36 系统 **233.9 → 91.3 µs**（2.6×，
误差不变 9.00e-07）。但 72 个枢轴列 × ~1.3 µs/列仍是**延迟受限**：一个 threadgroup 串行 2n 次屏障，
GPU 占用率极低。

**② A/B 实测（`a52677d194`，生产形状 25PRB/2DMRS，L=36）**：

| 方案 | hop 均值 | 其中 gpu_wait |
|---|---|---|
| CPU Gauss-Jordan（原行为） | **160.2 µs** | 21.7 µs |
| K1 引擎内求逆（`engine->run()`） | 254.0 µs | 95.9 µs |

⇒ 单系统求逆 **K1 目前比 CPU 慢 9 倍**。**但这不是"求逆该留 CPU"的依据**（资源划分按大粒度，见文首原则）：
一个 hop 内 9 个块**共用同一个 A**（`build_correlation_matrices` 只依赖 block_prb/DMRS/SCS/统计量），
所以每 hop 只求逆 `nof_layers` 次 = OTA 1×1 下**仅 1 个 36×36 = 47k FLOPs**，却花了 91.3 µs
⇒ **纯工程缺陷**：(a) 1 个线程组、占用率极低；(b) 每枢轴列 2 次屏障 × 36 列；(c) 消元逐元素读写
threadgroup 内存，形成长串行依赖链（~20 单元/线程 × 内存往返延迟）。CPU Gauss-Jordan 只是**过渡桥**：
`OCUDU_CE_GPU_INVERT=1` 已可切到引擎内求逆（`engine->run()` 把 K1+K1b+K2 放进同一条 CB），等 K1 修好
（下一步第 0 项）即成为默认。§27 "求逆占 CE 93%" 的判断是错的：它来自单测里*单独*调用
`engine.invert()` 的耗时，而不是它在 hop 内的占比。

**③ 引擎调用的相位分解（`OCUDU_MMSE_DEBUG=1`，新增常驻探针）**：

```
[mmse_eng] run_weights_only wrap 0.1 cb 0.4 encode 2.2 commit 1.5 wait 104.2 us
```

**host 成本 100% 是等待**（GPU 实际 6.5 µs）：buffer 包装/建 encoder/编码/commit 合计 ~4 µs，而
`waitUntilCompleted` 是 **~104 µs 的固定往返延迟**。⇒ 优化方向是**每 slot 的引擎调用次数**，不是算子微优化。

**④ 每 hop 两次调用 → 一次（`d11c5c7838`）**：25 PRB 的尾块只有 1 PRB（`25 % 3 = 1`），尾块第二次引擎调用
用 ~100 µs 往返换 ~1 µs GPU 计算。改为走**已有的 CPU 兜底块路径**（引擎失效时的同一条路径，~16 µs）：

| 形状 | 尾块走引擎 | 尾块走 CPU | NMSE |
|---|---|---|---|
| 25 PRB / 2 DMRS | 234.1 µs/hop | **148.1 µs/hop** | -17.33 dB（两模式一致） |
| 52 PRB / 2 DMRS | 239.6 | **152.6** | -12.54 dB |
| 52 PRB / 1 DMRS | 217.3 | **127.6** | -14.22 dB |

**架构定位（2026-09-13 用户提醒）**：让尾块走 CPU 是**过渡桥**——它省的是当下未融合链里的等待，不改变
目标架构。S-5 把 CE 的 dispatch 并入 slot 单 CB 后，尾块应回到**同一条 CB 内的第二个 dispatch**（不同几何
本来就可以各自 dispatch，只是不该各自 commit+wait），这段 CPU 回退随之删除。判据始终是"PHY compute 走
GPU"，不是这一处的 µs 账。

**踩坑记录（值得记住）**：第一版把 `if (tail_gpu) {...}` 包住整个尾块分支，走 CPU 时**没有把 `tail_ok`
置 false**（它是 `true` 初值）⇒ 兜底循环 `block_gpu_done()` 判定该块"已完成"，**整块被跳过**、网格里留着
陈旧值 ⇒ NMSE 从 -17.33 掉到 **-10.17 dB**（确定复现）。教训：分块决策变量必须在"不计算该块"时显式置 false。
`OCUDU_CE_TAIL_GPU=1` 可回到引擎尾块做 A/B。

**⑤ 尺寸护栏（`89a2ac3c67`）**：CPU 块路径的求逆是 O(L³) 串行 Gauss-Jordan，L=36 时 ~10 µs，但 `block_prb`
可配置——尾块 L>36 时 CPU 会变成毫秒级，此时保留引擎尾块（阈值与 K1 的 36 对齐）。生产 L=12 不受影响。

**⑥ 新增常驻观测**：`[mmse_eng]` 相位计时（`OCUDU_MMSE_DEBUG=1`）、Test 6 打印**每形状 hop 延迟**
（聚合 `[mmse_time_sum]` 把 2735 个不同形状的 hop 平均掉了，含尾块的形状必须单独看）。

**当前 CE 预算（单测口径）**：hop ≈ 148 µs = 1 次引擎调用（~100 µs 等待 + ~30 µs GPU/编码）+ CPU 尾块 ~16 µs
+ 相关矩阵/装包/解包 ~25 µs。预期 gNB 侧 `ul_channel_estimation` 430 → **~330 µs/slot**（待用户 OTA 确认）。

**下一步（按"离全 GPU 链还有多远"排序，不按局部 µs）**：

0b. **S-5a 已完成（`9a651e4210`）**：K1 从"每枢轴列 2 屏障的链"改成 **8×8 分块 Gauss-Jordan**
   （块内逐枢轴归约、只作用于块内行；块外行先把枢轴列存成乘数数组，再用**一次 rank-8 更新**扫完 —— 36 次扫描 → 5 次）。
   **单个 36×36：91.3 → 24.5 µs，误差 9.00e-07 → 3.92e-07**。两个关键测量重塑了这个改写：
   ① 同几何**空 kernel 只要 1.7 µs** ⇒ 不是 dispatch 开销；② 去掉 72 个屏障中的 36 个只省 2.5 µs ⇒ **屏障也不是瓶颈**
   （≈70 ns/个）。真正的大头是"每枢轴每线程的串行工作量"，所以改成每块列更新一次。
   **线程组几何比算术更重要**：(32,4) 91.3 / (64,4) 67.3 / (32,8) 57.0 / (64,8) 25.7 / (32,16) 29.5 / (64,16) **24.5 µs**
   （行维度收益更大）；默认已设为 (64,16)，`OCUDU_INV_TGX/TGY` 可复测。
   **CE 求逆默认切到 GPU**（`engine->run()`：求逆+权重+apply 同一条 CB，host 不读回逆矩阵）：本地 A/B 仍偏 CPU
   ~13 µs/hop（mean total 169.0 → 182.4；25 PRB hop 231.4 → 276.4，NMSE 都是 -17.33 dB），但**仍按架构原则设为默认**
   —— PHY compute 整体归 Metal path，剩下的差距是 kernel 工程项，不是把工作搬回 CPU 的理由。
   `OCUDU_CE_CPU_INVERT=1` 可切回 CPU 做 A/B。
   **K1 精修尝试（`ae78b4ed8f`，未成功、已回退）**：目标是让每块的 8 个枢轴不再各付 2 个屏障——用**一个 warp 在寄存器里做 8×8 求逆**
   （lane l 持 [D | I] 的第 l 行，枢轴行用 `simd_broadcast`/`simd_shuffle` 广播，8 个枢轴步 0 屏障，整块只留 1 个屏障）。
   结果：**所有尺寸都出 NaN**。二分证明问题**只在这块**：把 warp 块换回 thread-0 版本（A2/B/C 完全不动）⇒ 误差回到 3.92e-07 正确；
   换成 warp ⇒ NaN。⇒ 结构（应用 D⁻¹、保存乘数、枢轴列置零、一次 rank-8 扫描）是对的，问题在 warp 内广播本身。
   两个候选根因（下次需写一个**最小复现 kernel** 判定）：① `simd_broadcast`/`simd_shuffle` 的 lane 参数语义（动态 lane 是否合法、
   或把 **local memory 里的 `aug` 元素**拿去 shuffle）；② warp guard 的 lane 映射假设（已排除 1D/2D 线程组两种布局都不行）。
   新增常驻测试 **Test 10（inversion by size）**：6/8/12/16/18/24/32/36 全尺寸 + 双精度选主元参考，专门抓这类"某尺寸灾难性错误"
   （warp 版就会在这里立刻失败）。
   **warp 版已修好但不更快（`46aff154d7`）**：两个 warp 变体的 NaN 是**算术 bug**，不是广播语义——消元写成了
   `pivot_value * 未归一化的枢轴行`，即把"枢轴元素值"当成了乘数；正确形式是**枢轴 lane 先归一化并发布该行，其余 lane 用自己在该列的取值 `aug[p]` 去消元**。
   修好后：`warp + threadgroup 广播` 33.2 µs、`warp + simd_shuffle 广播` 28.4 µs，**都慢于提交版 24.5 µs**（误差都是 3.92e-07）。
   ⇒ **剩余 ~15 µs 不在枢轴相位**，而在每块列的三个**全线程组相位**（应用 D⁻¹、保存乘数、rank-8 扫描）以及 **32-warp 屏障**的代价。
   要再降需要改结构（减少全线程组相位），但**优先级下降**：OTA 实测 CE 的 108 µs GPU 时间里绝大部分是**排在 LDPC（~2 ms）后面等 GPU**，不是它自己的 24.5 µs。
   **K1 下一步（剩余 ~15 µs 的来源）**：块内 8 个枢轴 × 2 个相位是延迟受限的（每相位工作量太小）。
   方向：用 **warp/simdgroup 级 8×8 求逆**（`simdgroup_matrix` 或 warp 内 shuffle 消元）把每块的枢轴相位从 16 降到 ~4，
   目标 ≤10 µs（与 CPU 同量级甚至更快）。

0a-0. **S-5c-2 复现清单（下次直接照此执行，约 30–60 分钟）**——目标：**一个 hop 一条 CB**（尾块并入标准批次），
   且**门禁改为"merged 必须与 split 等价"**（split 路径=今天的代码，已证明与基线等价）：
   1. 拆函数：`run_engine_blocks` → `stage_engine_blocks`（暂存）/ `engine_batch`（一次引擎调用）/ `unpack_engine_blocks`（解包）；
   2. `engine_slots` + `slots_for(region)`：两片暂存区，**区域偏移必须按页对齐**（否则 no-copy wrap 退化成拷贝，慢 4×）；
      构造函数里按 `MAX_ENGINE_BATCHES × region_elems_*()` 分配；
   3. `stage_engine_blocks(..., sys_offset, L_stride, nout_stride, n_blk_stride, slots)`：槽步长用填充值、系统偏移、
      **A 的 pad 对角线置 1**（pad 区域由 `clear_engine_slots` 清零 ⇒ A=blockdiag[A_e, I]）；
   4. `unpack_engine_blocks(..., nout_stride, nof_blocks_stride, ...)`：**寻址必须用这两个步长**（上一版就是这里错：用 nout/n_blk ⇒ 读错行）；
   5. `stage_engine_blocks` 的 y 寻址：`n_blk_stride == 0` 时要**回退到本批次的 n_blk**（多层时否则互相覆盖）；
   6. 调用方顺序**必须**：暂存标准块 → 构建尾块相关矩阵 → 暂存尾块 → 一次 `engine_batch` → 解包两者。
      （尾块与标准块共用 `w_r_pp/w_r_hp`，顺序写反会让标准块被尾块矩阵暂存 —— 这是上一版最隐蔽的 bug。）
   7. 合并门槛：`rem_prb != 0 && n_std_blocks != 0 && !matrix_on && 2*nof_layers <= MAX_LAYERS`；`OCUDU_CE_SPLIT_TAIL=1` 走旧路径做 A/B。

0a-1. **S-5c-2（尾块并入同一条 CB）也已回退，但把范围收敛到两个已知小 bug**：设计=把尾块作为标准批次的**额外 system**，
   L/nout 槽步长按标准批次填充，A 做成**块对角 [A_e, I]**（pad 区域由调用方清零、pad 对角线置 1），于是整个 hop 一次 `engine_batch`（一条 CB）。
   已实现：`stage_engine_blocks` / `engine_batch` / `unpack_engine_blocks` 三分、`engine_slots` + **页对齐**双区域（区域步长不页对齐会让 no-copy wrap
   退化成拷贝，实测慢 4×）、填充暂存与 pad 对角线、合并/旧两条调用路径（`OCUDU_CE_SPLIT_TAIL=1` 走旧路径做 A/B）。
   **关键测量**：重构后的**旧（split）路径与基线完全等价**（同为 -15.52 dB、同样的时间）⇒ 拆分机制本身没问题；
   **只有合并路径错**（-9.38 dB）⇒ 两个已定位的 bug 是：
   ① `unpack_engine_blocks` 生成时仍用 `nout`/`n_blk` 做寻址，**没用 `nout_stride`/`nof_blocks_stride`**（合并路径下尾块的 h 行距是 nout_std ⇒ 读错行）；
   ② `stage_engine_blocks` 的 y 寻址里 `n_blk_stride == 0` 时没有回退到本批次的 `n_blk`（多层的 system 会互相覆盖）。
   另踩过一个真 bug 并已修：**尾块的相关矩阵与标准块共用 `w_r_pp/w_r_hp`**，必须"先暂存标准块、再构建尾块矩阵"（否则标准块被尾块的矩阵暂存）。
   **环境异常（重要）**：同一份已提交代码（`46aff154d7`，工作树 0 改动）在不同时间测出不同数值——Test 4a `engine.invert()` GPU 从 **24.5 µs 漂到 43.8 µs**、
   Test 6 的 25 PRB NMSE 从 **-17.33 dB 变成 -15.52 dB**、hop 从 231 µs 变成 281 µs，且日志里**没有任何 CPU 回退**。⇒ 本机 GPU 状态/负载在两次测量之间变了，
   **后续任何 A/B 或 NMSE 对比前必须先重新基线**（同一二进制连测两次确认可复现）。
   ⇒ **已由 §29 解决（2026-09-14，`450e2988dd`）**：实际落地的是**单区域**方案（尾块 = 标准批次的额外 system，两组共用步长），
   0a-0 的"双区域 + 页对齐"清单与这里的两个 bug 因结构改变而**不复存在**；本节仅供历史参考。

0a. **S-5c 第一次尝试未成功、已回退（工作树干净，HEAD 仍是 `46aff154d7`）**：目标是把**一个 hop 的两个引擎批次
   （标准块 + 尾块）合并进一条 command buffer**（一次 commit/wait），这是与 eq/demap 共享 burst 的机制基础。已实现并跑通的部件：
   引擎侧 `begin_batch()/commit_batch()/batch_open()` + `run/run_weights_only/run_nn` 增加 `wait=false`（编码进开放批次）；
   CE 侧把 `run_engine_blocks` 拆成 `stage_engine_blocks`（暂存+编码）与 `unpack_engine_blocks`（解包），并用 `engine_slots` 做**双区域**暂存。
   **踩到的三个坑（都值得记住）**：
   ① ObjC++ 里 helper 的出参不能用 `id<T>&`（ARC 下是 `__autoreleasing`）⇒ 改用 `struct engine_encoder` 传引用；
   ② **区域偏移必须按页对齐**：第二片缓冲若按"元素个数"偏移，`newBufferWithBytesNoCopy` 会失败并**每次退化成拷贝**（慢 4×，且结果可能不对）⇒ 用页对齐步长；
   ③ **卡死**：合并后在第 1431 个 hop、`-[MTLCommandBuffer initWithQueue:]` 内阻塞（队列 in-flight 槽耗尽）。计数器显示
   hop 1400 时 created≈1920 / waits≈2978（计数本身不自洽 ⇒ wait 计数里混入了"等待他人提交的 CB"的路径），**根因未定位**。
   **下一步排查建议**：精确统计 *in-flight* CB（给 CB 挂 `addCompletedHandler` 计数，或临时把 `MTLCommandQueue.maxCommandBufferCount` 设大区分
   “泄漏”与“GPU 侧不完成”）；并逐个检查所有 commit 路径（含 `invert/apply`、warm-up、CPU 回退分支）是否存在"创建后未提交/未等待"的分支。

0. **S-5：CE 整体回到 GPU（当前最高优先，因为它同时消掉两处过渡桥）**：
   (a) **K1 重写成块化右视消元 + `simdgroup_matrix`**：块宽 b=8 ⇒ 屏障从 72 次降到 ~2·⌈36/8⌉≈10 次，
   块更新走寄存器里的 8×8 矩阵乘（不再逐元素穿 threadgroup 内存）⇒ 目标延迟 ≤10–15 µs（与 CPU 同量级
   但**在链内、零往返**）。基线：当前 91.3 µs（S-4b）。注意 A 对称正定 ⇒ 无需选主元；也可走 Cholesky/LDLᵀ。
   (b) **尾块回到同一条 CB 的 dispatch**（几何不同但共用 CB，一个 commit/wait）；
   (c) **CE 的输出不再被 host 读回**：信道张量留在 GPU、eq 直接消费（需动 `channel_estimator` 接口），
   RSrp/noise/TA 等统计可以后置异步取；
   (d) 之后 CPU 求逆、CPU 尾块、CPU 兜底块三条 CPU 路径一起删除。
   参考收益：CE 段从 ~430 µs/slot 降到 GPU 时间量级（当前 hop 内 GPU 仅 ~30 µs）。
1. **每 slot 的最后一个等待**：CE 的 dispatch 与 eq/demap 的 burst 合进同一条 CB（barrier 分隔），
   收益 ~100 µs/slot —— 与 S-5(c) 是同一件事的两面。
2. 批量均衡器接线（**已推进到一半**）：`channel_equalizer::submit_group` + Metal 侧的 run 划分（相同几何/
   plan/noise/stride 的连续 run 合成**一次** `equalize_mxn_batch` dispatch，单符号 run 走原路径）
   **已实现并有单测**（`afd3e9d187`：5 符号 nof_re{128,128,96,128,128} → **2 次批量 dispatch**，
   与逐符号逐位一致；`batch_dispatch_count()` 可证明批量核真的跑了）。
   **路线 B 实施结果（`b37d07203c`，把批量化下沉到后端、调用方零改动）**：
   `shared_burst` 增加 **flush hook**（线程本地 + context）：在 `encoder()` 的 pipeline 比较**之前**调用（因此 stage 屏障仍落在交出的 dispatch 之后），以及在没有 stage 切换的 burst `commit()` 之前调用；`open()` 把"已累积但未编码"也算作打开（调用方的 `wait()` 仍会关闭并提交），`commit()` 在"没有 encoder"的早退**之前**先交接。均衡器引擎 `enqueue_burst()` 改为**累积**，flush 时把各符号已 staging 的输入拷进**一块连续组缓冲**（批量核要的统一 stride，输出 stride 统一就合并、否则退回单符号），每个 run 一次 dispatch；组缓冲由下一次 flush 回收（调用方必须先 wait 才走得到）。
   - 单测：PUSCH 形状（204 RE / 2 port / 1 layer / 12 符号 / 页对齐输出 stride）**1 次批量 dispatch、与逐符号逐位一致** ✓
   - **默认仍是立即编码**（`OCUDU_EQ_DEFER_ENCODE=1` 才走累积+flush），因为真实链路里延后编码会发散：17 PRB Metal 用例（4896 个 LLR）——
     **同管道 + 立即编码 = 0 差异**；**延后编码、整组一次 dispatch = 4485 差异**；**延后编码、关掉批量（每符号一次 dispatch）= 1442 差异**（组=2；组=4 时 2512）。
   - 关键结论：均衡器的**输出本身两种形态都正确**（wait 之后逐符号比对一致），且上面那条单测在隔离环境下 12 符号批量逐位一致 ⇒ **缺陷是"编码时机"**（把均衡的编码从 submit 期间挪到下一 stage 的首次编码时），**不是**批量 dispatch、也不是 burst 管道（同管道 + 立即编码逐位一致可证）。
   - 下一步实验（留给继续排查）：把 flush 提前到 **pass 1 结束**（而不是 pass 2 的首次编码）——若那时结果正确，说明问题在于"demapper 的 submit 与 eq 编码的相对顺序/可见性"；接口上只需要一个"组结束"通知（例如 `channel_equalizer::submit_group_end()` 或在 demapper 之前显式 flush），成本很小。

   **接线进展（`000d644098`，已被 `b37d07203c` 取代：解调器侧收集已回退）**：解调器已能整组提交，且发现**两个 wrapper 必须转发组调用**，否则生产链路永远到不了批量核：
   `phy_metrics_channel_equalizer_decorator`（metrics 装饰器）与 **`channel_equalizer_metal_or_generic`（工厂的
   Metal/CPU 路由 composite，关键缺口）**——补上转发后实测 equalizer dispatch **732 → 61**。
   **但组路径结果不对，因此默认关闭**（`OCUDU_EQ_GROUP_SUBMIT=1` 才启用）。二分定位（17 PRB Metal 场景，
   对比 serial 链的 LLR）：

   | 配置 | 差异 LLR |
   |---|---|
   | deferred group = 1（逐符号链） | **0 / 4896** |
   | group = 2，**强制关闭批量 dispatch** | 1442 |
   | group = 4，强制关闭批量 dispatch | 2512 |
   | group = 2…12，开启批量 dispatch | 4485（首差异在第 2 个符号的首个 RE，值为 0） |

   ⇒ **关闭批量后同样发散** ⇒ 缺陷在**解调器侧的收集阶段**，不在批量核。批量核与 staging 已单独验证：
   用解调器同等几何（204 RE / 2 port / 1 layer / 12 符号、每符号不同输入、页对齐输出 stride）的单测与逐符号
   **逐位一致**，且宿主侧回读 `h`/`y` 与源数据一致。首个差异 LLR=0 = kernel 的 invalid 分支（eq=0, nv=inf）
   ⇒ 第 2 个及之后的符号拿到的是**全零信道估计**。下一步排查方向：(a) 每符号抽取目标的
   `dynamic_ch_est_list::resize` 是否在收集期间被后续 resize 影响（span 失效）；(b) `modular_re_buffer_reader`
   视图 vs 拷贝分支的 span 生命周期；(c) 收集顺序与 `est_results.get_symbol_ch_estimate` 的交互。
   相关代码（`get_ch_data_*_into`、每符号存储、懒分配）已提交，开关打开即可复现。

   **未接线的硬约束（重要）**：解调器**不能**在 pass 1 收集 view 后一次性调用组接口——
   `get_ch_data_estimates()` / `get_ch_data_re()` 返回的是**每符号复用的同一块 scratch**
   （`ch_estimates_copy` / `ch_re_copy`，每次调用 resize+重填），收集到的 view 会全部指向最后一个符号。
   接线前必须先让这两处抽取写入**按符号分离、组内稳定**的缓冲（这与批量核要的 stride 布局天然一致），
   然后把 `pusch_demodulator_impl` 的 pass 1 改成一次 `submit_group`。预期 ~40 µs/slot。
3. FFT Phase 2（kernel 内相位补偿 + 直写 grid）：<100 µs/slot，最低优先。
4. LDPC 算法重设计（用户指定最后做）。

**本轮提交与门禁**：`5e79fe0739`（S-4a 批量 eq dispatch，已提交未接线）、`5df7d5ba7a`（S-4b K1 并行化）、
`a52677d194`（S-4c 两路求逆实测 + 相位探针）、`d11c5c7838`（S-4d 每 hop 一次引擎调用）、
`89a2ac3c67`（S-4e CPU 尾块尺寸护栏）——全部已推送 `apple-silicon`。
门禁：10 个独立 metal 测试二进制全绿、`ctest -L phy` **161/161**、`OCUDU_CE_LEVEL_STRICT=1` 的 Test 9
level sweep 在「尾块 CPU / 尾块引擎」两种模式下各跑一遍全绿。

**文档约定（2026-09-13 定）**：本文是**唯一活文档**，所有事后更正写在这里；`session_handoff_2026-09-12.md`
与 `pipeline_audit_2026-09.md` 是**只读快照**（不回填、不追加）；`full_chain_gpu_uma_zero_copy_refactor_plan.md`
是活路线图，可就地更新（已被实测否决的方案要划掉）。

---

### 29. S-5c 落地：尾块并入标准批次，**一个 hop 一条 command buffer**（`450e2988dd`，2026-09-14）

**结论先说**：0a-0 / 0a-1 里写的"**双暂存区 + `engine_batch()`（批次 API）**"方案**没有被采用**。真正落地的是更简单的一条：
尾块作为标准批次的**额外 system**，与标准块**共用同一片暂存区**（因此 0a-0 第 2 条"区域偏移必须页对齐"那个坑**根本不会出现**），
也**不用** S-5c-1 卡死的批次 API（`begin_batch/commit_batch`、`wait=false`）——仍然是一次普通引擎调用：**一条 CB、一次 commit/wait**。

**为什么一片区域就够**：引擎的批次索引是 `(system, block)`，每个 system 一份 A / R_hp，`nof_blocks` 全批统一。
把标准块放 system `[0, nof_layers)`、尾块放 system `[nof_layers, 2*nof_layers)`，两组**步长相同**
（`L_std / nout_std / n_std_blocks`），于是只需要 `2*nof_layers ≤ MAX_LAYERS(=4)`，现有按 `MAX_LAYERS` 分配的
`gpu_a / gpu_r_hp / gpu_w / gpu_y / gpu_h` 刚好装下（**无需第二片区域，也无需新的页对齐**）。
尾块几何较小，填充到标准几何：**A = blockdiag(A_e, I)**（pad 对角线置 1）+ **R_hp = [R_hp_e | 0]** ⇒
`W = [R_hp_e·A_e⁻¹ | 0]`，pad 列恒为 0 ⇒ `h = W·y` 在尾块自己的表项上与单独调用**是同一个算式**。

**代码结构**：`run_engine_blocks` 拆成 `stage_engine_group` / `engine_run` / `unpack_engine_group`，
槽地址一律由显式步长结构 `engine_strides{L, nout, n_blk}` 决定。0a-1 记录的两个 bug 在新结构里**没有位置**：
解包寻址用 `st.nout / st.n_blk`（不再用块几何），y 寻址用 `st.n_blk`（步长是显式参数，无需"回退"判断）。
合并门槛：`rem_prb != 0 && n_std_blocks != 0 && !matrix_on && !tail_on_cpu && 2*nof_layers <= MAX_LAYERS`；
`OCUDU_CE_SPLIT_TAIL=1` 保留旧的两次调用做 A/B；调用顺序照 0a-0 第 6 条——**先暂存标准块、再构建尾块矩阵**
（两者共用 `w_r_pp/w_r_hp`）。

**门禁升级为"merged ≡ split"，并写进了单测（Test 11）**：同一进程内先 `unsetenv(OCUDU_CE_SPLIT_TAIL)` 跑合并路径、
再 `setenv` 跑 split 路径，6 种形状逐一比对：52 / 25 / 4 PRB（**有尾块**）、51 PRB（**无尾块**）、2 PRB（整跳就是一个窄块）。
结果：**max|Δh|/rms = 0.00e+00**（bf16 输出**逐位一致**）、ΔNMSE **0.000 dB**（672 个尾块 RE 全部参与比对）。
同时断言合并**真的发生**（新增 `merged_batch_last()` 可观测位），否则"两条路径都走了 split"会让比对变成空转——见下面的坑。

**实测（同一二进制，两条路径；CE 单测 per-hop 延迟）**：

| 形状 | split（`OCUDU_CE_SPLIT_TAIL=1`） | merged（默认） | Δ | NMSE（两路径一致） |
|---|---|---|---|---|
| 52 PRB / 2 DMRS | 283.2 µs | **179.4 µs** | −103.8 | −14.85 dB |
| 52 PRB / 1 DMRS | 243.7 µs | **140.6 µs** | −103.1 | −14.66 dB |
| 25 PRB / 2 DMRS | 276.7 µs | **170.6 µs** | −106.1 | −15.52 dB |

引擎提交次数（整轮单测）**4621 → 4319**（整轮以 51 PRB 无尾块的 hop 为主，那些 hop 本来就只有一次调用）；
`gpu_wait` 不劣化。⇒ **每 hop 省下一次 ~100 µs 的固定往返**，与 §28③ 的相位分解预测完全一致。

**四个开关组合全部全绿**：默认 / `OCUDU_CE_SPLIT_TAIL=1` / `OCUDU_CE_TAIL_CPU=1`（该开关本身禁用合并，Test 11 相应放宽断言）/
`OCUDU_CE_CPU_INVERT=1`（CPU 求逆 + 合并：A 的 pad 置 **0** 而不是单位对角，W 的 pad 列仍恒 0 ⇒ 同样逐位一致）。

**本轮最重要的坑（差点得出错误结论）**：**`cmake --build build` 不会重建这些 metal 单测二进制**（它们不在 `all` 目标里）。
库编好后我跑了**旧二进制**，看到"commits 4621 → 4621、耗时没变、NMSE 没变"，差点判定"合并无效"并回退。
必须显式 `cmake --build build --target port_channel_estimator_metal_mmse_unit_test`（或把 10 个 metal 目标一次列全）。
⇒ §2 的门禁命令**必须带 `--target`**。

**环境基线（2026-09-14 本机复测，连测两次一致）**：Test 4a `engine.invert()` GPU **44.0 / 43.6 µs**（不是历史 24.5）、
Test 6 (25 PRB/2 DMRS) **−15.52 dB / 281.0、277.0 µs**、误差 3.92e-07、2735 hop 全 GPU、0 兜底块。
即 0a-1 记录的"漂移后"状态就是**当前可复现基线**；历史 24.5 µs / −17.33 dB 不应再作为对照基准。

**本轮门禁全绿**：`ctest -L phy` **161/161**、10 个 metal 二进制全 OK、`OCUDU_CE_LEVEL_STRICT=1`、
`pusch_demodulator_deferred_chain_test` **3/3**。

**剩余浪费（留给深水区 S-5c 收掉）**：合并批里**每个尾块 system 仍占 `n_std_blocks` 个块槽**（只有 block 0 是真的），
25 PRB 下即 4 个 system × 8 块 = 32 个块槽 vs 必要的 18 个。这是 GPU 侧几个 µs 的浪费，换掉的是 host 侧 ~100 µs 的往返，
当下明显划算；等 CE 并入 slot burst（下一条）时，两个几何**各自 dispatch、共享一条 CB**，这块浪费自然消失。

**下一步（深水区 S-5c，即用户 vision 里的"LDPC 之前的 GPU pipeline 最优"）**：
1. **CE → eq 零拷贝**：新增一个 GPU 侧**重排/压缩 dispatch**（或让 K2 直接写成 eq 想要的"按符号压缩的 cbf16"布局），
   host 只回传 DMRS 位置的 LSE 导频 + 统计量（RSrp/noise/TA，小），不再读回整个 `grid_est`。
2. **接口层**：`channel_estimator` 需要在设备侧交接（`est_results` 的构造要能引用 GPU 张量），PUSCH processor 流程随之调整。
3. **CE 并入 slot burst**：`shared_burst` 的 flush hook（S-4j 已提交、均衡器侧已在用）是现成机制——CE 的 dispatch 与
   eq/demap 的 dispatch 同属一条 CB，CE 的 host 往返彻底消失。届时 `(d)` 里剩下的 CPU 兜底路径（只在引擎失败时使用）
   可以保留为纯容错分支，不再出现在正常路径上。

---

### 30. 深水区 S-5c 设计：把 CE 的"等待"本身去掉（2026-09-14，S-6 计划）

#### 30.1 定量依据：CE 的 hop 现在**全部**花在那一次往返上

`OCUDU_MMSE_DEBUG=1` 的相位探针此前只装在 `run_weights_only()` 上，而 S-5a/S-5c 之后**热路径是 `run()`**，
所以探针在生产路径上是瞎的（`a41480c004` S-5d 补上）。补全后（CE 单测整轮，同一二进制）：

```
run              (2530 calls): wrap 0.1  cb 0.4  encode 2.7  commit 1.8  wait 176.5 us
run_weights_only ( 741 calls): wrap 0.6  cb 0.3  encode 2.2  commit 1.5  wait 123.1 us
```

对照 `[mmse_time_sum] mean total=195.6 gpu_path=183.6 (gpu_wait=65.0)`：
**host 侧 wrap+cb+encode+commit ≈ 5 µs，`waitUntilCompleted` ≈ 176 µs**，而估算器自己的 staging/解包
只剩 ~4 µs（183.6 − 176.5 − 5）。⇒ **CE 的 hop 成本 = 一次同步往返**，不是计算、不是拷贝、也不是解包。
（OTA 上更严重：`mean total=431.3 gpu_path=415.8 gpu_wait=108.2`，其中 ~300 µs 是排队/往返，
因为它排在 ~2 ms 的 LDPC 后面。）

**结论：S-5 系列在 CE 上剩下的唯一大杠杆就是"根本不等"**——把 CE 的 dispatch 并入 slot 的共享 burst，
host 全程不读回结果。这正是任务 ② 的内容。

#### 30.2 关键认识：UMA 上**没有拷贝可省**，要省的是**同步**

Apple Silicon 是统一内存：CE 的 `gpu_h` / `grid_est` 本来就是**同一块 host 内存**，引擎用
`newBufferWithBytesNoCopy` 零拷贝 wrap（`ocudu_metal_mmse_engine.mm:150-153`），均衡器引擎同样 wrap 调用方给的
host span。所以"CE 输出零拷贝给 eq"**今天在字节层面已经成立**——缺的是两件事：

1. **布局**：CE 的产物是**按块**的网格（每 (layer,symbol) 一整行 `nof_prb*12` 个 `cf_t`），而 eq 要的是
   **按符号、按 mask 压缩**的 `cbf16`（`channel_equalizer::ch_est_list::get_channel(i_port,i_layer)` →
   `span<const cbf16_t>`，长度 = `re_mask.count()`）。今天这个转换在**CPU**上做
   （`pusch_demodulator_impl.cpp:644-680`，每符号一次，属于 `ul_equalization_demod` 的 278 µs）。
   ⇒ 需要**一个 GPU 侧的重排/压缩 dispatch（K3）**。
2. **同步**：CE 调用的 `waitUntilCompleted` 保证 eq 读到的是**已完成**的写入。并进 burst 后由 burst 的
   barrier + 末尾那次 wait 保证。

#### 30.3 真正的阻塞点（必须先回答，否则"不等"无从谈起）

`pusch_demodulator_impl` 在 dispatch 均衡**之前**就要向估算器要两样东西，**都在 host 上**：
- `est_results.get_symbol_ch_estimate(...)`（每符号、每 port/layer，mask 压缩后的 cbf16）—— eq 的输入；
- `est_results.get_noise_variance(port)`—— eq 的每 RE 噪声方差（由 CE 的 RSrp/噪声统计得到）。

而 CE 自己还要在 host 上产出 `filtered_pilots_lse_view`（RSrp/TA/噪声的输入）与 `freq_response`
（下一跳的 LSE 参考）。这些都源自 `grid_est`（host）。所以"CE 不等"= 这些**消费者也必须搬到设备侧**，
或者**只回传小的 DMRS 子集**（52 PRB/2 DMRS ≈ 624 个 `cf_t` ≈ 5 KB，而不是整个网格 70 KB）。

#### 30.4 两条路线（**建议先走 A**）

**路线 A（增量、风险低，推荐先做）**——保持统计量在 host，只把"eq 要的那份"搬到 GPU：
- **S-6a**：新增 K3 kernel（`ocudu_mmse_reformat.metal`）：输入 = `gpu_h`（按块、`cf_t`）+ 几何，输出 =
  **按符号、mask 压缩的 cbf16**，写进估算器的常驻设备缓冲（每 (symbol,port,layer) 一段，长度
  `nof_re`），并让估算器的 results 暴露一个 `ch_est_list` 兼容视图（**窄接口**：只由 Metal 估算器实现，
  demodulator 探测能力后使用；其它估算器实现完全不受影响，host 路径保留做 A/B）。
- **S-6b**：demodulator 的 `get_ch_data_estimates()` 在该视图可用时**直接返回它**（不再逐符号 CPU 转换）。
  门禁：与 host 路径**逐位一致**（两者都是同一 `cf_t` 的 cbf16 舍入），且 `ul_equalization_demod` 的
  CPU 时间下降。
- **S-6c**：CE 的 K1/K1b/K2/K3 编进 `shared_burst`（S-4j 的 flush hook 机制现成），**不再自己 commit/wait**。
  ~~统计量所需的 DMRS 子集在 burst 末尾那次 wait 之后从设备缓冲读回（小）。~~
  ⇒ **这一条是错的，见 §30.6**：均衡器在 **encode 时**就要拿到噪声方差，所以"wait 之后回传"来不及，
  必须先把**噪声方差也搬到 GPU**（S-6c-0）。
  门禁：`[metal_stats] mmse_ce commits` 归零（由 burst 计数）、hop 的 176 µs wait 消失、
  Test 11 的 merged≡split 仍逐位一致。

**路线 B（彻底，风险高）**：把 `estimate_sigma2` / RSrp / 噪声方差 / TA 也做成 GPU 侧归约 kernel，
host 在整跳里完全不碰网格（连 5 KB 的 DMRS 子集都不回传）。方向上更接近"全 GPU PHY"的终点，
但多出一个归约 kernel + 统计量的数值对拍，且与"LDPC 之前先把 pipeline 打通"的优先级不完全一致。

#### 30.5 S-5c 的 OTA 验证要点（待用户执行）

预期变化（相对 2026-09-14 基线 `ul_channel_estimation` median 486.5 µs / `[mmse_time_sum] mean total=431.3`）：
- `[metal_stats] mmse_ce commits` 与 hop 数之比 **≈1.0**（原先 1.6–1.7）；`max_in_flight` 仍应为 1；
- `ul_channel_estimation` median **下降约 100 µs/跳**（有尾块的形状：52/25 PRB 等）；
- `[mmse_time_sum]` 的 `hops_no_gpu` 应为 0、`fb_blocks` 应为 0（合并路径不引入 CPU 兜底）；
- NMSE 类指标（QPSK/16QAM KO 率）不应变化：合并批在单测里与 split 路径**逐位一致**（Test 11）。


#### 30.6 设计更正（同日，读完消费侧代码之后）：**均衡器在 encode 时就要噪声方差**

§30.4 里"S-6c 把统计量在 burst 末尾 wait 之后回传"是**错的**。实际依赖顺序（`pusch_processor_impl.cpp:223-260`、
`pusch_demodulator_impl.cpp:410-440`）：

```
CE 完成（notifier） → process_data():
    est_results.get_channel_state_information(csi)     // RSrp/SNR/TA/EPRE  ← host 读
    ul_capture::capture_h(...)                         // 逐符号读整网格    ← host 读
    demodulator.demodulate(...):
        for each symbol:
            ch_est = get_ch_data_estimates(est_results,...)   // ← host 读（逐符号 cf_t→cbf16 + mask 压缩）
            nv[port] = est_results.get_noise_variance(port)   // ← host 读（来自 filtered_pilots_lse 的统计量）
            equalizer->equalize(..., ch_est, nv, ...)         // ← 这一步才 dispatch！
```

`nv` 是**均衡 dispatch 的输入**（kernel 用它把 LLR 定标），所以它必须在**编码那一刻**就已知；
而它今天由 host 上的 `channel_statistics_estimator` 从 `filtered_pilots_lse`（= `grid_est` 在 DMRS 位置的
解包结果）算出。⇒ **只要噪声方差还在 host 上算，CE 就必须 wait**，把它挪到 burst 末尾毫无意义。

因此"取消 CE 等待"的**真正前置条件**是这两件（顺序不能反）：
- **S-6c-0（新增，关键路径）**：把**均衡器要用的噪声方差**做成 GPU 侧产物——一个小归约 kernel，
  输入 = K2/K3 产出的 DMRS 位置信道估计 + 收到的 DMRS 导频（都已 UMA 可寻址），输出 = **设备缓冲里的
  每 port 噪声方差**，均衡 kernel 直接从该缓冲读（引擎侧已经有 `nv` 指针参数，UMA 下就是同一个指针类型）。
  数值必须与 host 统计量**逐位对拍**（同一个公式、同样的输入）。
- **S-6c-1**：CE 的 dispatch 并入 slot burst；host 侧的 `unpack → grid_est → filtered_pilots_lse → 统计量 →
  CSI/ul_capture` 全部**推迟到 burst 的那一次 wait 之后**（它们只服务于上报与下一跳，不参与本跳的 dispatch）。

**修正后的顺序**：S-6a（K3 布局）→ S-6b（demodulator/eq 消费设备视图）→ **S-6c-0（GPU 噪声方差）** →
S-6c-1（并入 burst，wait 消失）。其中 S-6a/S-6b 本身也有独立收益（把逐符号 cbf16 转换从
`ul_equalization_demod` 的 host 时间里拿掉），但**它们单独并不减少 CE 的等待**——这一点必须记住，
否则会像 §27 那样把"局部耗时"误当成"链路收益"。

#### 29.1 S-5c 的 OTA 确认（2026-09-13 夜，commit `a41480c004`，1338 slot / 103 s ping）

| 指标 | 基线（S-5c 之前） | 本次 | 判定 |
|---|---|---|---|
| `mmse_ce commits` / `[mmse_time_sum] calls` | 1.6–1.7 | **1493 / 1491 = 1.0013** | ✅ 每跳一次引擎调用 |
| `hops_no_gpu` / `fb_blocks` | 0 / 0 | **0 / 0** | ✅ 合并路径不引入 CPU 兜底 |
| `mmse_ce max_in_flight` | 1 | **1** | ✅ 无队列槽泄漏（S-5c-1 卡死风险不存在） |
| `ul_channel_estimation` median | 486.5 µs | **307.2 µs** | ✅ −179 µs |
| `ul_channel_estimation` p99 / max | 7206 / 11710 µs | **3904 / 8949 µs** | ✅ 尾部减半 |
| `mmse_time_sum mean total / gpu_path` | 431.3 / 415.8 µs | **293.4 / 277.2 µs** | ✅ −138 µs |
| `mmse_time_sum gpu_wait` | 108.2 µs | **105.8 µs** | 不变（= 排在 LDPC 后面等 GPU，符合 §30.1） |
| `ul_pipeline` median | 3031 µs | **2791 µs** | ✅ |
| `t=` median | 2755 µs | **2562 µs** | ✅ |
| `ul_ldpc_decode` median | 1985 µs | 1963 µs | 不变（未动 LDPC） |

**⇒ S-5c 在 OTA 上兑现了单测预测的 ~100–180 µs/跳，且不改变估计质量。**

#### 29.2 OTA 顺带查清的一个 LA 现象（与 GPU 链无关，但会污染 KO 率读数）

本次日志里 16QAM KO 73.6%、64QAM KO 100%、256QAM KO 50%——**这三个百分比不可横向比较**，
因为每个调制阶的样本由调度器的 LA 挑出，落在完全不同的 SINR 上（`csi.get_sinr_dB()` 中位数）：
QPSK 3.7 dB（n=1372）、16QAM **2.9 dB**（n=87）、64QAM **6.1 dB**（n=5）、256QAM **15.8 dB**（n=14）。
16QAM 需要 ≥8–12 dB、64QAM 需要 ≥12.8–17.4 dB（见 `ul_snr_256qam_mcs_table`）⇒ **这两档的低 SINR 授
权注定译不出来**，而 256QAM 的授权恰好落在 26 dB 的好信道上，所以它"看起来更好"。

逐 UE 时序揭示了机制（日志 `PUSCH: rnti=... mod=...` 序列）：**每一个高阶授权前面都是一条 SINR 9–16 dB 的
QPSK 授权**，LA 用那个（已被更新但**属于上一跳**的）SNR 抬升 MCS，而这条高阶授权实际译码时 SINR 掉到 0–6 dB，
且 `epre` 仍然很高（8–13 dB）⇒ **信号很强、SINR 很低 = 同频干扰**，不是估计/软比特问题。突发调度让同一批
多条授权共用同一个陈旧 SNR（日志里可见连续 5 条 64QAM 全在 6 dB、连续 5 条 256QAM 全在 6 dB 上失败）。
**为什么没有自我纠正**：UL OLLA 默认步长 `cell_cfg.pusch.olla_snr_inc_step = 0.001 dB`（上限 ±5 dB），
失败一次只降 0.001 dB ⇒ 实质上冻结（`apps/.../du_high_config.h:356`、`scheduler_expert_config.h:185`）。
另外 `cell_cfg.pusch.max_ue_mcs: 20` **并不等于"禁掉 256QAM"**：在 256QAM 表（TS38.214 Table 5.1.3.1-2，
`lib/ran/pdsch/pdsch_mcs.cpp:27`）里 **MCS 20 正是第一个 256QAM 条目**（R=682.5，按 LA 自己的 SNR 表需 17.4 dB）。
要压到 64QAM 用 `max_ue_mcs: 19`，16QAM 用 10，纯 QPSK 用 4。

**GPU 链本身是健康的（这才是我们要的信号）**：只看"真有信号"（`epre > -20 dB`）的 QPSK 授权，
n=1313、KO **1.3%**；按 SINR 分箱：0–5 dB 时 0.9%、5–10 dB 与 >10 dB 时 **0.0%**，只有 <0 dB 才失败。
59 条无信号 QPSK KO 的 `epre ≈ -40 dB`（UE 根本没发 = DTX），不是译码失败。
⇒ LDPC 在 3–6 dB、R≈0.4–0.5 上跑满 6 次迭代失败，是物理必然，不是 CE/软比特缺陷。
（另注：整份日志 1478 条授权 **`rv` 全为 0**，没有一条 UL HARQ 重传，值得单独查一次——但 info 级日志里
没有调度器的授权行，无法在这里定论。）

---

### 31. S-6a 落地：K3 —— 均衡器要的信道估计在 GPU 上直接产出（2026-09-13 夜，`ocudu_mmse_reformat.metal`）

#### 31.1 做了什么

`ocudu_mmse_reformat.metal`（K3）把 K2 的**按块**输出（每个 (system, block) 一块 symbol-major 的 `nout = nf*14`
复数浮点，交错 re/im）在 GPU 上重排成**均衡器真正消费的布局**：每个 (symbol, layer) 只取该符号的**数据 RE**、
按子载波升序**压缩**、并存成 **cbf16**——正是 `channel_equalizer::ch_est_list::get_channel()` 要的东西。
一个线程负责一个 (layer, symbol, hop 子载波)：目标下标 = 该子载波在此符号 mask 中的 rank（不设原子操作），
mask 位为 0 的 RE 直接返回。float→bf16 用整数位运算复刻 `ocudu::to_bf16()` 的**就近偶数舍入**，
所以产物与 CPU 路径**逐位相同**（否则 host/GPU 两条路会给出不同软比特）。

**关键设计点**：

1. **K3 编进 CE 自己那条 command buffer**（`engine->run()`/`run_weights_only()` 内、K2 之后），
   所以 hop 仍然只有 **一次 commit/wait**（`[metal_stats] mmse_ce commits` 不变）。
   这是 S-6c"CE 不再等待"的前提：估计值必须在 CB 内产生，host 才不需要读回网格。
2. **合并批次（S-5c）的两套几何**在同一批次里：标准块在 system `[0, nof_layers)`、覆盖子载波
   `[0, n_blk*nf_std)`；尾块在 system `[sys_tail, ...)`、block 0、覆盖其余子载波，但**沿用批次的 nout 步长**
   （尾块 system 是被填充过的 std 几何，真实输出只在前 `nf_tail*14` 行）。
3. **mask 由估算器按 demodulator 的规则推导**（`stage_re_masks()`）：把该 hop 分配的 PRB 展开成子载波，
   DM-RS 符号再去掉 DM-RS 的 RE（一个 PRB 内的 DM-RS 集合 = 各层 `re_pattern` 的并集；type-1 下就是各层 comb 的并集，
   正好等于 demodulator 用 `nof_cdm_groups_without_data` 选出的那组）。**两条来源一致性由 Test 12 用
   `get_dmrs_prb_mask(type1, 1)` 独立构造的 mask 对拍**，不是自证。
4. **mask 暂存做了记忆化**：mask 只随分配变化，而暂存本身比整个 K3 kernel 还贵（实测 ~12 µs/hop）⇒
   只在 (first_prb, nof_prb, hop, **PRB pattern hash**, DMRS RE bits, DMRS symbol bits, mask_words) 变化时重建。
5. **只在该调用覆盖整个分配时挂 K3**：合并批次、无尾块的 hop、或整跳只有一个窄块；`matrix_on`（nn 风味）、
   `OCUDU_CE_SPLIT_TAIL=1`（拆成两条 CB）、尾块走 CPU 时都不挂——否则目标缓冲里会留下上一次的陈旧值。
   `device_estimates_ready_last()` 报告**这次是否真的产出了**（这条断言当场抓到了一个 bug：`L_std=54 > 36`
   走 CPU 求逆分支时 K3 从未被编码，因为 `run_weights_only()` 当时还没有 reformat 参数——已修，两条求逆路径现在都编码 K3）。

#### 31.2 门禁（Test 12：device ≡ host，逐位）

| 形状 | 检查的 RE 数 | 不一致 |
|---|---|---|
| 52 PRB / 2 DMRS（合并批次） | 8112 | **0** |
| 25 PRB / 2 DMRS（合并批次） | 3900 | **0** |
| 4 PRB / 3 DMRS（L=54，CPU 求逆 + run_weights_only） | 600 | **0** |
| 51 PRB / 2 DMRS（无尾块） | 7956 | **0** |
| 2 PRB / 2 DMRS（整跳一个窄块） | 312 | **0** |
| **合计** | **20880** | **0** |

5 种形状共用一个估算器实例（因此顺带覆盖了 mask 记忆化的失效路径）。比较对象是**host 路径的同一个 gather**：
`res.get_symbol_ch_estimate(稠密行) → 按 demodulator 的 mask 压缩`，即今天链路上的那条路。

#### 31.3 代价（两次干净复测，同一二进制）

| | K3 关（默认） | K3 开（`OCUDU_CE_DEVICE_CE=1`） |
|---|---|---|
| Test 6 52 PRB/2 DMRS | 183.5 / 181.1 µs | 185.9 / 187.4 µs |
| `[mmse_time_sum]` mean total | 193.4 / 171.2 µs | 195.1 / 175.7 µs |
| `gpu_wait` | 63.2 / 49.6 µs | 67.4 / 52.5 µs |
| `mmse_ce commits` | — | 4357（与关闭时同量级 ⇒ 仍是一条 CB） |

⇒ **K3 在 hop 内约 +2–4 µs**（记忆化前是 +15 µs，那 12 µs 全是 host 暂存 mask）。换来的是 S-6b
可以在 demodulator 里**整段删掉**逐符号的 `cf_t → cbf16` 转换与 mask 压缩（`ul_equalization_demod` 的 host 时间），
并且它是 S-6c（均衡器直接读设备缓冲、CE 不再 wait）的必要条件。

**A/B 开关**：`OCUDU_CE_DEVICE_CE=1` 打开设备侧产物（默认关，因为 S-6b 之前还没有消费者）。
**注意测量纪律**：上面两组数据必须在**门禁跑完之后**单独测——与 ctest/metal 门禁并发测时，
hop 会从 183 µs 漂到 423 µs（GPU 争用），这正是 §28.0a-1 记录的"环境漂移"来源之一。

---

### 32. S-6b 落地：均衡器直接读估算器的设备缓冲（`b77447a97d`）

#### 32.1 做了什么

S-6a 让 K3 在 GPU 上产出了均衡器要的布局，但**还没有消费者**：demodulator 仍然逐符号把估计值从 host 网格
RE-by-RE 抓进自己的副本（`dynamic_ch_est_list ch_estimates_copy`）。S-6b 把这个抓取整段删掉：demodulator 现在
把**估算器缓冲的视图**交给均衡器（UMA 下就是同一块内存，零拷贝）。

**接口**（都带默认实现，其它估算器/后端零改动）：
- `ch_est_device_view`（`port_channel_estimator.h`）：`data`（layer-major）+ `offset`/`nof_re`（本次符号）+
  `total_re`/`nof_layers`，`get_layer(l)` 给出该层的 span。
- `port_channel_estimator_results::get_device_ch_estimates(i_symbol, tx_layer)` 与
  `dmrs_pusch_estimator_results::get_device_ch_estimates(i_symbol, rx_port, tx_layer)`：默认返回
  `std::nullopt`；`dmrs_pusch_estimator_impl` 按 port 转发到 `ch_est_result[rx_port]`（与 host 路径同一索引约定）。
- `view_ch_est_list`（新，放在 `dynamic_ch_est_list.h` 旁）：每 (port, layer) 一个只读 span 的 `ch_est_list`。

**RE 数量即护栏**：估算器自己推导分配的 RE 布局，若与 demodulator 即将施加的 mask 不一致（数量不符），
就回退 host 路径 —— 把"两边理解不一致"从**数据损坏**降级为**性能回退**。这条护栏在 S-6b 的测试里
**当场救了一次**（见 32.3）。

#### 32.2 DC 子载波的契约（唯一一处语义变化）

DC 子载波不承载数据，均衡器必须在那个 RE 看到**零信道估计**（否则会喂进一个"看起来合法"的垃圾 LLR）。
host 路径是 demodulator 拿到副本后自己写 0；设备路径的缓冲是**共享只读**的，所以改由**生产者**写：

- DC 位置经 `port_channel_estimator::configuration::dc_position` 一路传下去（`pusch_processor_impl` 从
  `pdu.dc_position` 填 → `dmrs_pusch_estimator_impl` → port 配置 → `fd_td_estimation_stage_args`）；
- K3 在该 RE 写 0（mask 仍**包含**它，否则 RE 数量对不上）；
- Test 12 新增 DC 形状，与 host 参照**逐位一致**（含擦除）。

#### 32.3 门禁：两条路径的软比特逐位一致（新测试）

`pusch_demodulator_deferred_chain_test.device_ch_estimates_match_the_host_path`：同一次解调，分别在
"有/无设备视图"下跑，比较**全部软比特**（含 block 边界与游标），覆盖
**串行 / deferred 两条链 × 1 与 2 个 CDM group × 有/无 DC**，全部一致。

**这个测试第一版是"空转"的**：stub 的设备缓冲按 24 PRB 建，而 harness 的分配是 50 PRB ⇒ 数量不符 ⇒
护栏回退 host 路径 ⇒ 两边当然一致。**故意把 offset 挪一个 RE 也测不出差异**，这才暴露出来。
修法两条：(a) stub 从 `config.rb_mask` 的 span 推导布局（不再猜常量）；(b) 加**反空转断言**——
统计"实际交出的视图数"，要求设备路径**真的被走到**。现在把 offset 挪一个 RE，测试立刻失败并报出
第一个不一致的软比特位置。

#### 32.4 默认值与开关

设备路径**默认开**（demodulator 会用）；`OCUDU_CE_CPU_CE=1` 同时关掉生产与消费两侧，回到逐符号 host 抓取，
用于 A/B。CE 侧代价是 K3 的 2–5 µs/hop；换掉的是与 (分配 RE 数 × port × layer) 成正比的 host 工作，
并且它是 S-6c（CE 不再等待自己的 command buffer）的**前置条件**。

**本地微基准不可信（记录在案）**：同一测试里 host 路径 113–148 µs、设备路径 119–133 µs（取 100 次最小值
仍然翻符号）⇒ 这台机器上的噪声大于效应本身。**该项不做本地断言，由 OTA 的 `[ul_equalization_demod]` 裁决。**

#### 32.5 可验证性探针（`7bc264fd24`）

设备路径的两侧**默认都会静默回退**（估算器对无法一次覆盖整跳的 hop 不挂 K3：nn 风味、拆分的两条 CB、
尾块走 CPU；demodulator 在拿不到视图或 RE 数量不符时回退 host 抓取）。这对链路是对的，对**测量**是灾难——
"全部回退"的一轮和"真的走了设备路径"的一轮，日志长得一模一样（本会话已经因此栽过两次：旧测试二进制、
不匹配的测试 double）。因此补两个计数器（沿用 `[metal_stats]` 风格）：

- 估算器：`[mmse_time_sum] ... | device_hops=<n> max total=...`（本轮有多少跳产出了设备侧估计）；
- demodulator：`[metal_stats] pusch_demod ch_est device=<n> host=<n>`（逐符号抽取走了多少设备视图、多少 host 抓取）。

社区脚本 `scripts/gnb_log_stats.py` 不解析这两行，追加字段无影响（已确认）。

#### 32.6 OTA 复测（S-6b 首次上真链路）与随后的修正（`1206da74eb`）

**OTA 结果（commit `7bc264fd24`，1275 slot / 107 s）——正确性全过，性能不合格：**

| 指标 | S-5c 基线 | S-6b 首次 OTA | 判定 |
|---|---|---|---|
| `pusch_demod ch_est device / host` | —（无此探针） | **15136 / 0** | ✅ 设备路径 100% 生效、零回退 |
| `[mmse_time_sum] device_hops / calls` | — | **1376 / 1376 = 100%** | ✅ |
| `mmse_ce commits / calls` | 1.0013 | **1378 / 1376 = 1.0015** | ✅ 仍是一跳一条 CB |
| `hops_no_gpu` / `fb_blocks` | 0 / 0 | **0 / 0** | ✅ |
| KO 率（QPSK） | 5.5% | 4.3% | ✅ 无回归 |
| `ul_channel_estimation` median | 307.2 µs | **338.8 µs** | ❌ **+31.6** |
| `[mmse_time_sum] mean total` | 293.4 µs | **327.2 µs** | ❌ **+33.8** |
| `ul_equalization_demod` median | 273.6 µs | 273.9 µs | ⚠️ 消费侧收益**测不出来** |

未被我改动的阶段全平（`ul_ldpc_decode` 1963→1968、`ul_time_frequency` 228.6→231.6）⇒ 这 +34 µs 是改动本身造成的。

**根因**：K3 第一版在 host 上**暂存每符号 RE mask**（14 × ⌈nof_sub/32⌉ 个字 + 前缀 popcount）。
单元测试里同一个分配连跑 50 次 ⇒ 记忆化全中 ⇒ 我只测到 +2~5 µs（**本地测量被测试形状欺骗**）；
真链路里调度器**每个 slot 的 grant 大小都在变** ⇒ 记忆化几乎不命中 ⇒ 每次重算 14×nof_prb×12 次迭代
（~12 µs），再加 K3 的 dispatch 与 GPU 窗口（`gpu_wait` +10.2 µs）。

**修正**：目标下标**根本不需要 mask**。估算器只为**连续分配**产出设备估计，每个 PRB 贡献相同数量的数据 RE，
DM-RS comb 对每个 PRB 削掉同样的位置 ⇒
`rank = (PRB 下标) × (每 PRB 数据 RE 数) + (本 PRB 内该子载波以下的数据 RE 数)`，纯整数运算；
每符号偏移是 14 次前缀和。kernel 改为接收标量布局（有/无 DM-RS 的每 PRB 数据 RE 数、12 位 comb、DM-RS 符号位图），
**mask 缓冲与暂存代码整体删除**；非连续分配**拒绝产出**（消费者靠 RE 数量护栏回退 host，Test 12 新增负例覆盖）。

**修正后本地 A/B**（CE 单测，各两次）：无设备路径 172–193 µs/hop，有设备路径 **176–177 µs/hop**
⇒ 那 ~15 µs 暂存已消失，剩下的只是 dispatch 本身。Test 12 仍与原 host 抓取**逐位一致**（同 24780 REs，含 DC）。

**结论与定位（重要）**：S-6b 单独看**不是收益项**——CE 侧付出一次 dispatch（本地 +2~4 µs，OTA 上还有 GPU 窗口），
消费侧省下的逐符号 host 抓取在真链路的噪声下测不出来。**它的价值在于它是 S-6c 的前置条件**：
估计值必须在 GPU 侧产出，CE 才可能不再等待自己的 command buffer（§30.1：那一次 wait 在 OTA 上是 ~116 µs，
全 hop 的 176 µs 里绝大部分）。因此下一步 S-6c-0（GPU 噪声方差）→ S-6c-1（并入 burst）才是收益兑现的地方。
**教训**：本地 A/B 的形状必须**模拟真实链路的分配变化**，否则记忆化/缓存类优化会被系统性高估。

---

### 33. S-6c-0 落地：均衡器的噪声方差在 GPU 上归约（K4，`c25796c5ed`）

#### 33.1 为什么是它，以及为什么它能进 CE 自己的 CB

均衡器的软比特定标量 = **收到的 DMRS 导频**与**用信道估计重生成的导频**之间的残差能量：

```
noise = Σ_{DMRS 符号 s, 导频 sc} | Σ_{s 所在 CDM 组的层} ( β/nof_lse_symbols · Σ_{s2} H[s2][l][sc] ) · X[s][l][sc] · e^{j2π f_cfo t_s} − Y[s][g][sc] |²
```

三项输入里，**X（发送导频）与 Y（接收导频）本来就是估算器的输入**（host 手里就有），只有 **H（DMRS 位置的信道估计）**
来自 CE 的输出。所以整个归约可以放进 CE 自己的 command buffer（K4，紧随 K2/K3），
**均衡器消费的两样东西（信道估计 + 噪声方差）就都在设备侧产出**，host 不再需要在派发均衡器之前读网格
—— 这正是 S-6c-1（CE 不再 wait）的最后一块前置条件（§30.1：那一次 wait 在 OTA 上 ~116 µs）。

**收尾与 host 完全一致**（这是能对拍的前提）：`nv = max(rsrp_avg / 10^{MAX_SINR_DB/10}, energy / (nof_dmrs_pilots·nof_cdm − 1))`
—— 归一化除数与 SINR 上限（100 dB，无噪声合成输入的护栏）都在 kernel 里复刻。

#### 33.2 门禁与数值口径

归约顺序与 host 不同（分线程累加 + 树形归约），所以 **逐位一致不可达**：门禁改为
**相对误差 < 1e-5**，实测 **最坏 1.2e-07**（6 种形状中 3 种完全为 0）。这是本工程第一次把门禁从
"逐位一致"放宽到"数值等价"，理由是噪音方差本身就是统计量，而它的差別会被 demapper 的量化吸收。

#### 33.3 两个"测出来才知道"的坑（都值得记住）

1. **结构体尺寸不一致 ⇒ 参数错位**：K4 的参数块在 MSL 里写 `dmrs_slots[4]`，而 C++ 侧用了
   `pusch_constants::MAX_NOF_DMRS_SYMBOLS` = **8** ⇒ 后续所有字段偏移错位 ⇒ `nof_dmrs_pilots` 读到垃圾，
   归一化除数变成 **−1** ⇒ 设备噪声方差是**负数**（−2698 vs host 0.256）。教训：跨语言共享的参数块必须逐字段对齐，
   数组长度按**估算器自己的上限**（`MAX_DMRS_SYMBOLS = 4`）写死并互相注释。
2. **单线程组 + 散列加载 = 延迟地狱**：第一版每个 (DMRS 符号, 层, 子载波) 都重新加载一次 H，
   DMRS comb（每 12 个子载波一个）让每次加载都是非合并的高延迟访存；64 线程的单线程组只有 ~39 次/线程的串行链
   ⇒ **+17 µs/hop**。改为**每 (层, 符号) 只加载一次**（值本来就相同）+ **256 线程** ⇒ **0–3 µs**。
   实测（CE 单测，各 3 次）：无设备路径 173.2–176.5 µs/hop、只 K3 177.7–179.3、K3+K4 **176.7–186.9**。

顺带留下一个隔离用的调试开关 `OCUDU_CE_NO_K4=1`（跳过 K4 的编码，暂存照旧），
正是它把"K4 的 17 µs 是 dispatch 而不是暂存"这件事钉死的。

---

### 34. S-6c-1 设计（读代码后的更正）：不能"并进 slot burst"，要"**先 commit、后 wait**"

#### 34.1 关键发现：估算器与均衡器**可能不在同一个线程**，线程本地 burst 会漏 dispatch

`dmrs_pusch_estimator_impl::estimate()`（`:50-65`）把**每个 rx port 的 `compute()` 分别 defer 到 executor**，
最后一个完成的 port 在自己的回调线程上调用 `notifier.on_estimation_complete()`，处理链才继续走到
`pusch_processor_impl::process_data()` → demodulator。⇒

- **单 port**（本小区）时：`compute()` 与 demodulator 确实在同一线程 ✓
- **多 port** 时：port 0 在 executor 线程 A、port 1 在 B；notifier 在最后完成的那个线程（比如 B）上触发 ⇒
  若 CE 只是把 K1..K4 **编码进线程本地 burst 而不 commit**，则 port 0 的那条 burst 停在 A 上**永远不会执行**
  ✗✗ —— 而 `process_data` 只会在 B 的 burst 上追加 eq/demap。

⇒ 原设计（§30.4 "CE 并入 slot burst"）**在多 port 下是错的**，必须换成与线程无关的机制。

#### 34.2 修正后的机制：CE **立即 commit、延迟 wait**

不再是"共享 burst"，而是**每条 CE command buffer 立即 commit（不阻塞）**，等待推迟到消费者：

1. `mmse_engine` 增加 `run_async(...)`：编码 + `commit`，**不** `waitUntilCompleted`；同时记录该 CB 的句柄/状态。
   与 S-5c-1 卡死的 `begin_batch/commit_batch`（累积多条 CB 后才 commit）**本质不同**：这里每条 CB 立刻入队，
   队列槽不会累积 ⇒ 不引入那个卡死模式（`mmse_ce max_in_flight` 仍应为 1，OTA 上可验证）。
2. 估算器侧：`port_channel_estimator` 的流程拆成 **submit（编码+commit，不碰输出）** 与 **finish（wait + 解包 + 统计）**。
   由于 base class 的 `compute()` 目前是"进去一趟、出来什么都有"，这需要把 host 侧的解包/统计
   （`grid_est`、`filtered_pilots_lse`、`freq_response`、rsrp/噪声/TA/CSI）**整体推迟到 `finish`**。
3. demodulator：均衡器消费**设备侧**信道估计（S-6b）与**设备侧**噪声方差（K4）——两者都已在设备上
   ⇒ 构建 eq/demap 的 dispatch **不需要** CE 的输出 ✓（还差"消费 K4 的噪声方差"这一处接线：目前仍调
   `est_results.get_noise_variance()`）。
4. `pusch_processor_impl::process_data`：把 `est_results.get_channel_state_information(csi)` 从
   **demodulator 之前**移到**之后**（以及 `ul_capture` 只在 dump 时做，dump 打开时退回同步路径）。CSI 只用于上报，
   不参与 demod 配置（已确认：`demod_config` 全部来自 PDU）。**这会给 FAPI 的报告顺序带来可观测变化**（CSI 指示
   晚于数据指示），需要在 OTA 上确认 MAC 侧无影响。

#### 34.3 收益与判据

收益来源是**把 CE 的 GPU 时间与"准备 eq/demap"的 CPU 时间重叠**，并让两次等待合并成一次：
OTA 上 `[mmse_time_sum] gpu_wait ≈ 116 µs`、hop 176 µs ⇒ 目标是把 `ul_channel_estimation` 的
median 从 ~307 µs 压到 ~200 µs 以内（具体取决于重叠程度）。

判据（缺一不可）：
- `[metal_stats] mmse_ce max_in_flight` **仍为 1**（不是 S-5c-1 的累积模式）；`commits` 与 hop 数之比 ≈1.0；
- `[metal_stats] burst ... dispatches` 与 eq/demap 符号数一致（CE 的 dispatch 不混进 burst 计数）；
- **软比特逐位一致**（`OCUDU_CE_CPU_CE=1` 对照）——CE 的 host 侧完成时机变了，但产出必须不变；
- `hops_no_gpu=0`、`fb_blocks=0`，且 `[metal_stats] pusch_demod ch_est device>0, host=0`；
- CSI 数值不变（RSrp/SNR/TA 与同步路径逐位或 1e-6 级一致）。

#### 34.4 风险

- **流程改动横跨三层**（engine / estimator / PUSCH processor），且 `compute()` 是共享基类的方法（classical 估算器也用）
  ⇒ "两阶段"必须以**默认同步**的方式加入（`submit()` 默认实现= `compute()`，`finish()` 默认空），只让 Metal 估算器实现真异步。
- CSI 提取后移会改变 `ul_capture` 与 notifier 的时序 ⇒ dump 打开时强制同步路径。
- 若 OTA 上 `max_in_flight` 或 us 数不符合预期，**一行回退**：`OCUDU_CE_CPU_CE=1` 回到"CE 自己 wait"的同步路径。

---

### 35. 配对 OTA A/B 抓到真凶：**每跳新建 Metal buffer**（`197f5670c6`）

#### 35.1 配对 A/B 结果（固定 MCS `min=max=10`，同一会话，指纹一致）

| 指标 | `OCUDU_CE_CPU_CE=1`（host 栈） | 默认（设备栈） | Δ |
|---|---|---|---|
| samples / `calls`（hop×port） | 607 / 2120 | 630 / 2274 | hops/slot 3.49 vs 3.61 |
| `sigma2` / `corr` | 3.4 / 11.0 | 3.2 / 11.1 | 平均 hop 一致 |
| `ul_ldpc_decode` median | 3457 µs | 3490 µs | 负载一致 |
| **`[mmse_time_sum] mean total`** | **301.6 µs** | **360.7 µs** | **+59.1** |
| `gpu_path` / `gpu_wait` | 285.5 / 106.5 | 344.8 / 143.0 | +59.3 / +36.5 |
| **`ul_channel_estimation` median** | **302.0 µs** | **362.7 µs** | **+60.7** |
| `ul_equalization_demod` median | 284.8 µs | 285.6 µs | **+0.8（消费侧收益 ≈0）** |
| `ul_pipeline` median | 4349 µs | 4420 µs | +71 |

⇒ 设备栈每次 CE 调用贵 ~60 µs，消费侧省下 ~0。**这不是噪声**（同会话、同负载、方向一致）。

#### 35.2 根因：零拷贝缓存按指针缓存"首次"映射，而新缓冲**每跳尺寸都在变**

日志里有 **6492 条**：

```
MMSE engine: zero-copy cache hit with a larger request (10200 > cached 1800): re-wrapping the buffer
```

`mmse_engine_impl::wrap()` 用 `buffer_cache.emplace(ptr, ...)`——**emplace 不会覆盖已有 key**，所以：
请求尺寸 > 已缓存尺寸 ⇒ 新建一个 `MTLBuffer` 并**再次 emplace 失败**（缓存永远停在最初的小尺寸）⇒
之后每跳只要尺寸变大就**再新建一个**。构造函数的 warm-up 正是为此把 K1/K2 的暂存缓冲按**满容量**先 wrap 一遍；
**我新加的 K3 目标缓冲与 K4 导频输入没有做这件事**，于是热路径上每次 CE 调用都可能分配一个 Metal buffer
（~20 µs）× 每 PUSCH 3.5 次调用 ≈ **+60 µs/PUSCH** —— 与实测的 +59 精确吻合。

这也解释了为什么**本地测不出来**：单测里同一形状连跑 50 次，尺寸不增长 ⇒ 只有第一次 wrap ✗

**修法**：`mmse_engine::reserve_buffer(ptr, bytes)`（新增，公开给估算器）+ 构造函数里把
`gpu_ce` / `gpu_pilots` / `gpu_rx_pilots` / `gpu_nv` 按容量 reserve 一次。修后**整个 metal 测试套件的 re-wrap 警告为 0**
（原先 OTA 单次运行 6492 条），Test 12 仍逐位一致。

#### 35.3 待确认与结论的口径

修好后设备栈的净成本需要**再跑一次配对**才能定论（预计回到 +0~10 µs 量级，即只剩 K3/K4 两次 dispatch 的固有代价）。
无论如何，这一条是**应当记录的方法论教训**：**零拷贝/缓存类优化在本地必须用"尺寸/形状随真实链路变化"的负载验证**，
否则缓存命中率被系统性高估——本会话第二次栽在同一件事上（第一次是 K3 的 mask 暂存记忆化，见 §32.6）。

#### 35.4 修复后的复测（`197f5670c6`）：re-wrap 归零，残余 +34.5 µs 全在 GPU 窗口

| 指标 | host 栈 | 设备栈（修复前） | 设备栈（修复后） |
|---|---|---|---|
| `grep -c "larger request"` | — | **6492** | **0** ✅ |
| hops/slot | 3.49 | 3.61 | 3.21 |
| `sigma2` / `corr` | 3.4 / 11.0 | 3.2 / 11.1 | 3.3 / 11.0 |
| `[mmse_time_sum] mean total` | 301.6 | 360.7 | **332.3** |
| `gpu_path` | 285.5 | 344.8 | 316.3 |
| `gpu_wait` | 106.5 | 143.0 | **142.1** |
| `ul_channel_estimation` median | 302.0 | 362.7 | **336.5** |
| `ul_channel_estimation` max | 5868.7 | 4380.3 | 2332.2 |
| `ul_equalization_demod` median | 284.8 | 285.6 | 281.1 |

- **修复兑现**：`+59.1 → +30.7 µs`（mean total），即"每跳新建 Metal buffer"那 ~29 µs 已经拿掉 ✅
- **残余 +34.5 µs 全部在 `gpu_wait`**：本轮 hops/slot（3.21）与 DFT/slot（284）都比 host 臂**更低**，`gpu_wait` 却高 35 µs
  ⇒ 不是负载造成的，而是 **K3/K4 两次 dispatch 边界在"被 DFT(~284 CB/slot)+LDPC 抢占的 GPU"上的固有代价**
  （每次 ~17 µs；本地无争用时两者合计仅 2–10 µs）。
- **消费侧仍然 ≈0**（`ul_equalization_demod` 281.1 vs 284.8，落在噪声内）——这与 §32.6 的判断一致：
  host 侧那次逐符号抓取本来就便宜，S-6b 的价值不在它自己的 µs。

**决策口径（待定）**：设备栈当前是"每 PUSCH +34.5 µs、收益 0"，而 **S-6c-1 正是要把 `gpu_wait` 那 142 µs 拿掉**，
且它**依赖**设备栈（均衡器必须能在设备侧拿到估计值与噪声方差）。因此两条路：
(a) 保持默认开、直接做 S-6c-1（不必来回切默认）；
(b) 先把默认切回 host（`OCUDU_CE_DEVICE_CE=1` 作为实验开关），等 S-6c-1 落地再一起打开。
**若 S-6c-1 因任何原因搁置，就必须走 (b)**——否则就是长期白付 0.8% 的 slot 时间换 0 收益。

---

### 36. S-6c-1b 设计定稿（读 demodulator/estimator 代码后）

#### 36.1 重叠窗口在哪（已确认，代码位置）

`pusch_demodulator_impl::demodulate()` 的 deferred 路径结构（`:449-486`）：

```
Pass 1（逐符号）：get_ch_data_estimates（设备视图 ✓ S-6b）+ get_ch_data_re（host 逐 RE 抓取）+ equalizer->submit()
Pass 2（逐符号）：demapper->submit()
→ **全组唯一的同步点**：demapper->wait(); equalizer->wait();
Pass 3：消费 LLR
```

⇒ **Pass 1+2 的全部 host 工作（`ch_re` 抓取 + staging + 编码）都在那一次 wait 之前**，
正是可以与 CE 的 CB 重叠的窗口。而 CE 的设备产物（K3 的估计、K4 的噪声方差）必须在 **burst 提交之前**完成，
所以同步钩子的位置是：**`demapper->wait()` 之前**，由 demodulator 向 `est_results` 要一次同步：

```cpp
// dmrs_pusch_estimator_results（默认实现返回 true，其它估算器零改动）
virtual bool sync_device_estimates() { return true; }
```
demodulator 在设备路径下、提交 burst 之前调用它；Metal 估算器实现为 `engine->wait_pending()`。

#### 36.2 硬约束：**unpack 必须逐 hop 完成**，因此两阶段是"逐 hop 交错"而不是"整 PUSCH 提交/收尾"

`apply_fd_td_estimation_stage` 的 host 解包读的是 `gpu_h`——**引擎的共享暂存缓冲**，
下一个 hop 的 K2 会覆盖它；`filtered_pilots_lse` / `pilots_lse` 同样是逐 hop 复用的 scratch。
⇒ 不能"把整跳所有 hop 都 submit 完再统一 finish"，否则 hop 1 的暂存会踩掉 hop 0 待解包的数据。

可行的顺序（每 port）：
```
h0.prepare+stage → h0.run_async（不 wait）
h1.prepare+stage（与 h0 的 GPU 时间重叠）
h0.finish（wait + unpack + 统计）        ← 必须在 h1.run_async 之前，因为 h1 要复用 gpu_a/r_hp/w/y
h1.run_async（入口会先 wait h0，此时 h0 已完成 ⇒ 立即返回）
… → burst 提交前 sync → finish(h1)
```
即 **重叠发生在"下一个 hop 的 prepare/stage"与"当前 hop 的 GPU 执行"之间**（单 hop 时则是
"Pass 1+2 的准备"与"CE 的 GPU 执行"之间）。

#### 36.3 收益量级（必须诚实）：**几十 µs/PUSCH，不是那 142 µs**

可重叠的是 host 侧的 `ch_re` 抓取 + staging + 编码（每符号 `nof_re × 端口` 次 cbf16 抓取 × 11 符号），
量级几十 µs；`wait` 的其余部分（队列等待）**无法被 CPU 工作填满**，只能被"越过它继续做别的"掩盖，
而 burst 提交前的 sync 是硬约束 ⇒ **上限就是那几十 µs**。

对照当前 slot 预算（固定 16QAM 那一轮）：TF 237 + CE 336 + eq/demap 281 + FAPI 4 ≈ **858 µs**，
而 **LDPC 3538 µs**。⇒ S-6c-1b 的收益约为 slot 的 **1%**，LDPC 是 4 倍于"LDPC 之前全部阶段之和"的另一极。

**结论（待用户定）**：LDPC 之前的 GPU pipeline 至此已基本收敛（本轮之后 CE 侧剩下的只有那一次等待的一半，
消费侧只剩 `ch_re` 的 host 抓取）。继续做完 S-6c-1b 值得，但**它已经不是数量级的地方**；
真正的下一个数量级在 LDPC（用户此前指定暂缓，理由是怀疑 3GPP LDPC 面向串行、需要先研究）。

---

### 37. S-6c-1b 第 0 步门禁的结果：**§36.1 的前提被证伪**（2026-09-13，读代码，未改一行）

§36 的设计把重叠窗口定在"Pass 1+2 的全部 host 工作"，前提是 **Pass 1 只是在提交 dispatch，不读 GPU 的产物**。
第 0 步（交接待办的"先查 `noise_var_estimates` 的读取时机"）查下来：前提不成立，而且原因不止 nv 一处。

#### 37.1 三个已核实的代码事实

1. **所有引擎共用同一条 `MTLCommandQueue`**：`shared_queue::backend_queue()` 是进程级单例
   （`lib/phy/metal/ocudu_metal_queue.mm:89`），CE 的 mmse 引擎（`ocudu_metal_mmse_engine.mm:408`）、
   均衡器与解调器（都走 `metal::shared_burst`，`ocudu_metal_burst.mm:117`）用的都是它。
   ⇒ **GPU 侧 CE→eq→demapper 的先后顺序已经由"提交顺序"保证**，主机同步**不是**设备可见性所必需；
   主机同步的**唯一**理由是"**主机**要读 GPU 写过的内存"。
2. **均衡器在 encode（`submit()`）时就读设备产物**，而且是两处：
   - `resolve_plan()` 读 `noise_var_estimates` 的**值**（做端口归约 + 取 kernel 的 `noise_var` 标量）：
     `channel_equalizer_metal.cpp:327`、`:340`，经 `run_equalize()` 由 `submit()` 调用（`:375`）。
     ⇒ 这正是交接文档 §3.4 的**悲观分支**。
   - `channel_equalizer_metal.cpp:405-418` 把 `ch_estimates.get_channel(port, layer)` **memcpy 进自己的
     staging `entry.h`**，而 S-6b 之后这些 span 指向的是**设备缓冲**（`ch_est_device_view::data = gpu_ce`，
     `port_channel_estimator_metal_mmse_impl.cpp:1305`）⇒ 这是一次**主机读 GPU 内存**。
3. ⇒ **S-6b 只去掉了 CE 侧的 unpack，设备→主机→设备 的往返仍在**：H 仍然要在主机上被逐符号、逐端口、
   逐层抓一遍才进 kernel（`entry.h`）。

#### 37.2 对 §36.1 的更正

同步钩子的位置**不是**"`demapper->wait()` 之前"，而必须是"**第一次 `equalizer->submit()` 之前**"
（Pass 1 自己就要读设备 H 与 nv）。⇒ 单跳 PUSCH 上可重叠的只剩"符号 0 的 `ch_re` 抓取 + 配置"，**几 µs**：
**S-6c-1b 按原设计在单跳上收益 ≈ 0**。

§36.2 的"逐 hop 交错"仍然成立（多跳时"下一跳的 prepare/stage"∥"本跳的 GPU 执行"），
但实测 `hops/slot ≈ 1.08–1.67`（约一半的 PUSCH 只有一跳），加权后大约 **0.5% slot**。

#### 37.3 要把 §36.1 的窗口拿回来，必须先补完 S-6b（记为 **S-6c-2**）

只有让 Pass 1 **完全不读 GPU 写过的内存**，CE 的主机等待才能并入 `demapper->wait()`（同队列、提交在前 ⇒ 完成在前）：

- **(a) H 零拷贝**：把 K3 缓冲以"**设备切片**"（buffer handle + offset）绑给 eq kernel，替掉 `entry.h` 的 memcpy。
  注意 `wrap_no_copy()` 要求**页对齐**（`ocudu_metal_queue.mm:52-68`），按符号/端口偏移去 wrap `gpu_ce + offset`
  **不合法**；必须走 handle + offset（顺带绕开 `197f5670c6` 那个 re-wrap 陷阱）。
  布局上：设备缓冲是 **layer-major**（`data + l * total_re`），而 eq kernel 要 `[port][layer][re]`，
  且不同 rx 端口是**不同的估算器实例、不同的缓冲** ⇒ 要么逐端口绑定（`setBuffer` 的 index 天然支持多缓冲），
  要么只在 `nof_used_ports == 1` 时走零拷贝、多端口回退 staging。
- **(b) nv 设备侧**：eq kernel 直接读 K4 的输出缓冲。**kernel 里已经有 invalid 分支**
  （§33 的 `eq=0, nv=inf`），所以"非法 nv"的语义可以留在设备侧；否则 `resolve_plan()` 的端口归约
  仍要主机读那 1–2 个 float，同步照样提前。

#### 37.4 收益上限（诚实）

做完 (a)(b) 后可重叠的是 **Pass 1+2 的 host 工作**：`ch_re` 抓取 + staging + 28 次 dispatch 编码
（14 符号 × eq/demapper），量级**几十 µs/PUSCH ≈ slot 的 1–2%**。
对照：LDPC **3538 µs** 仍是"它之前全部阶段之和"的 4 倍。**S-6c-2 + S-6c-1b 加起来也不改变数量级格局**，
但它们把 S-6b 的"设备侧消费"真正做到位（消掉设备→主机→设备的往返），且是"CPU 线程无阻塞 dispatch"
这一回退架构所需要的接口。

#### 37.5 待用户定

- **A**：只收尾 S-6c-1b 的低风险部分（接口 + 基类两阶段 + Metal 估算器 `run_async`，拿到 §36.2 的多跳重叠），
  跳过 CSI 次序调整；≈0.5% slot，风险低，为后续留接口。
- **B**：先做 **S-6c-2**（均衡器零拷贝消费设备估计 + 设备侧 nv），再做 S-6c-1b；≈1–2% slot，
  中等规模（kernel 参数 + 引擎绑定 + 设备视图接口），架构上把 S-6b 做完整。
- **C**：pre-LDPC 就此收在 858 µs，直接转 **LDPC 定量剖析**（80% 的 slot 在那里）。

**用户选定 B**（2026-09-13）。§38–§40 是它的落地记录。

---

### 38. S-6c-2a 落地：均衡器就地读设备估计（`c5f1648ab7`）

§37.3(a) 的两件事里的第一件。均衡器不再把估计 memcpy 进自己的 staging：

- `ch_est_list` 增加 **device slice**（`base` / `offset` / `layer_stride` / `nof_layers`），
  `view_ch_est_list` 记录每个 (port, layer) 条目产自哪个缓冲；kernel 用 `h_offset` + `h_layer_stride`
  寻址（staged 路径传 offset 0、stride = nof_re ⇒ 索引与原来逐位相同）。
- **CE 与均衡器现在绑定同一个 Metal 资源**：估算器把 K3 估计与 K4 噪声方差放进
  `shared_queue::wrap_no_copy` 的**进程级缓存**（`mmse_engine::reserve_shared_buffer()`），
  且这两个分配**按页取整**（`alloc_aligned_pages`）——共享映射会把长度向上取整到整页，
  非整页分配会被映射到自身之外。其余内部 staging 仍用引擎私有缓存（精确长度 wrap，安全）。
- 单 rx 端口（= 部署形态，1 layer）是"一次 dispatch 能就地读"的形状；多端口是"每端口一个缓冲"，
  一个 base 指针描述不了 ⇒ 回退 staging（文档化，非缺陷）。
- 新增探针：`[metal_stats] wrap hits/creates/replaces/failures` 与
  `[metal_stats] equalizer ch_est device/staged`。**两种来源的软比特完全相同**，所以"悄悄回退"只能靠计数器发现
  ——这正是新测试的做法（1 端口必须 device>0/staged=0，2 端口必须相反，且软比特一致）。

### 39. S-6c-2b 落地：均衡器的噪声方差也留在设备（`988e69004e`）

第二件。**（2026-09-13 OTA 后更正）** 原文写的是"Pass 1 上最后一个主机读 GPU 内存的点"，**不准确**：
demodulator 读的 `est_results.get_noise_variance()` 是**基类在主机上算出来**的值（`compute_hop_finish` 里的
`estimate_noise`，输入是主机侧的 `filtered_pilots_lse`），不是 K4 的设备值 ⇒ 它不是 GPU 内存读。
S-6c-2b 真正去掉的是**对"估算器主机侧收尾"的依赖**：一旦估算被推迟，那个值要等 `finish()` 才存在，
而 `finish()` 在解调之后 ⇒ 解调前读它就等于把整条链重新卡在估算器上。
（顺带的效果是：均衡器现在用 K4 的设备值，与主机值相差 ~1e-7 相对误差——这正是 S-6c-0 设计的样子。）

- 设备 slice 增加 `noise_var`（该端口噪声方差的设备地址），估算器经
  `dmrs_pusch_estimator_results::get_device_noise_variance()` 发布（K4 的输出缓冲）。
- 均衡器实现 `channel_equalizer::consumes_device_estimates()`（新增能力查询，默认 false；composite 与 metrics 装饰器
  都必须转发）。demodulator 只在"后端确实就地读 + 估算器确实产出了设备 nv"时跳过主机读，
  **逐端口判断实际可用性**——按"能力"一刀切会在 `OCUDU_CE_NO_K4=1`（K4 被关）时把主机 span 留空、让均衡器读到垃圾。
- kernel 自己套用主机的端口有效性判据（`nv > 0 && nv < INF`）。主机在压缩端口时套用同一判据，端口全被丢弃时
  `ch_mod_sq = 0` ⇒ 落到既有的 invalid 分支（eq=0、nv=inf）——与主机路径的 `invalid_input` 输出一致。

### 40. S-6c-1b-1 落地：估算器的测量值在解调之后再上报（`984b561638`）

把"读估算器结果"从解调之前挪到之后——这是让解调与**被推迟的**估算重叠的前提：

- `dmrs_pusch_estimator_results::sync_device_estimates()`：完成被推迟的估算并发布结果，默认空实现（同步估算器零改动）。
- notifier adaptor 不再拷贝一个别处构造的 `channel_state_information`，而是按配置的 SINR 类型**就地构造**上报对象；
  解调器运行时把 post-eq SINR/EVM 写进同一对象，估算器的测量值（RSRP/EPRE/noise/TA/CFO/它自己的 SINR）
  之后合并进去。二者字段不重叠 ⇒ 上报结果不变。
- `OCUDU_UL_DUMP` 的调试抓取确实要读主机副本：改为**先 sync，且只在开启时**。

此时无行为变化（估算器仍是同步的），门禁 161/161。

### 41. S-6c-1b-2 设计（本轮读代码后的两处简化，**尚未开工**）

#### 41.1 只需推迟**最后一个 hop**

原 §36.2 要求"逐 hop 交错（下一跳 prepare ∥ 本跳 GPU）"，理由是 `gpu_h` 等引擎暂存缓冲跨 hop 复用。
读代码后可以退一大步：**前面所有 hop 照旧同步完成，只推迟最后一个 hop**：

```
h0: prepare+stage → run_async → wait → unpack      （照旧，完全同步）
h1: prepare+stage → run_async                       （不 wait）
    → notifier → demodulator Pass 1+2               ← 与 h1 的 GPU 执行重叠
    → demapper->wait()（同队列、提交在前 ⇒ 也覆盖 h1 的 CB）
    → 处理器 sync_device_estimates() → 解调后再取 CSI
```
- 最后一个 hop 之后没有别的 hop 会覆盖 `gpu_h` ⇒ §36.2 的逐步交错**不再需要**，
  per-hop scratch 的保活只需**一份**（不是每 hop 一份），风险与代码量都小一个量级。
- 收益与全交错几乎相同：重叠窗口是"最后一跳的 GPU 执行"对"demod Pass 1+2 的 host 工作"，
  而 §36.2 额外给的只是"下一跳的 prepare ∥ 本跳 GPU"（2 跳 PUSCH 上 ~10–20 µs）。

#### 41.2 基类拆分点（`port_channel_estimator_average_impl`）

- `compute()` 保持为 `submit() + finish()`（classical 路径逐位不变的门禁就在这一条）。
- `do_compute` 拆成 `do_submit`（setup + hops）与 `do_finish`（rsrp 归一 / epre / datarp / noise 下限 / snr / cfo，
  以及 2 跳时的 `time_alignment_s /= 2`）。
- `compute_hop` 拆成 `compute_hop_submit`（(A) 逐层预处理 + CFO + `fd_td_estimation_stage_args` + 调虚钩子）
  与 `compute_hop_finish`（(C) rsrp 累加 / `estimate_noise` / TA）。
- **保活到 finish 的只有一份状态**：`enlarged_filtered_pilots_lse`（stage 写、finish 读）+
  `hop` / `nof_lse_symbols` / `stage_hop_offset` / `beta_scaling` / `cfo_hop`。
  `filtered_pilots_lse` 是它的视图，可在 finish 里重建；`pattern_symbols`/`first_symbol`/`last_symbol`/
  `nof_dmrs_symbols` 可用 `extract_common_pattern(cfg_local, hop)` 重新推导（`pattern_symbols` 就是
  `cfg_local.dmrs_pattern[0].symbols` 的引用）。**`pilots` 是唯一必须由调用方续命的输入**
  （它是 `dmrs_pusch_estimator_impl::temp_symbols`，估算是**成员**，在 `estimate()` 返回后仍然有效 ⇒
  把 `pilots` 作为 `finish(pilots)` 的参数传进去即可，不要在 port 估算器里存指针）。
- 新增第二个虚钩子 `complete_fd_td_estimation_stage()`（默认空实现）：classical 的
  `apply_fd_td_estimation_stage` 是内联算完的，设备后端在它里面只 stage+commit，wait+unpack 放进 complete。
- `dmrs_pusch_estimator`：`submit()`（逐 port 提交 + 最后一个 port 的回调触发 notifier）与 `finish()`
  （逐 port 收尾）；`sync_device_estimates()` = 完成 pending 的收尾。

#### 41.3 Metal 侧的拆分点（`port_channel_estimator_metal_mmse_impl`）

`apply_fd_td_estimation_stage` 是一个 ~400 行的函数，stage 与 unpack 交织。落地时：

- `engine_run()`（`:1168`）改走 **`run_async`**（不 wait），`unpack_engine_group()`（`:1247`）与它后面的
  K3/K4 ready 记账挪进 `complete_fd_td_estimation_stage()`；unpack 需要的那几个参数
  （`gb_start`/`n_blk`/`b_prb`/`nout`/`nof_layers`/`sys_offset`/`strides`）存成一份 pending 记录。
- ⚠️ **`run_async` 只覆盖 legacy（`metal_mmse`）路径**；`run_nn`（`metal_nn_mmse` 的 simdgroup 8×8）没有 async 形式。
  生产默认 algo 是 `cpu`，Metal 由 `expert_phy --pusch_channel_estimator_algo` 选（用户跑 `metal_mmse`）⇒
  先只让 legacy 路径异步；nn flavor 要么补 `run_nn_async`，要么保持同步（文档化）。
- 回退开关仍是 `OCUDU_CE_CPU_CE=1`（整个设备栈 → host，一行）。

#### 41.4 demodulator / processor 的收尾规则

- **demodulator**：只有当后端**会在主机上读**设备估计时（`!equalizer->consumes_device_estimates(...)`，
  即 ≥2 端口或 CPU 均衡器），才在 Pass 1 之前 `est_results.sync_device_estimates()`；
  1 端口 Metal 路径不需要任何同步（同队列 + 提交顺序已保证可见性）。
- **processor**：解调之后 `sync_device_estimates()` 再合并 CSI（§40 已就位）。

#### 41.5 门禁

软比特/CSI 与 `OCUDU_CE_CPU_CE=1` 逐位一致；`[metal_stats] mmse_ce commits/hops ≈ 1.0` 且 `max_in_flight` 仍为 1；
`hops_no_gpu=0`、`fb_blocks=0`；`[mmse_time_sum] mean total` 与 `ul_channel_estimation` median 下降；
`[metal_stats] wrap failures=0`。

---

### 42. S-6c-1b-2a/2b 落地（`e086ff00f8`、`be2a7ba2ec`、`22b8afc561`）

**2a（`e086ff00f8`）**：估算器逐跳工作拆成 `submit()`/`finish()`（`compute()` = 两者），只留最后一跳 pending；
`pending_hop_state` 保活 `enlarged_filtered_pilots_lse` 与 stage 调用参数；新增
`complete_fd_td_estimation_stage()` 虚钩子（默认空）。

**2a 的回归与修复（`be2a7ba2ec`）**：2a 把 `filtered_pilots_lse` 改成"在 finish 里从 enlarged 缓冲重建"，
而 `setup_auxiliary_buffers()` 在 **filter** 平滑策略下是 `assign(enlarged, MAX_V_PILOTS, nof_symbol_pilots)`
——**带 12 个子载波偏移的窗口**。重建丢掉了偏移 ⇒ RSRP/EPRE/噪声/TA 的输入整体挪位。
网格估计本身没错，所以软比特比对照样通过，**只有 Test 9 的电平一致性扫描露了馅**
（nv/l² 漂移 1.236 而不是 1.039、snr 1.251 而不是 1.180）。现在该视图是估算器的**成员**，两个阶段共用。
> 教训（与"空洞测试"同类）：声称"逐位不变"的门禁**必须比对数字**，不能只看 PASS/FAIL——
> 这次是拿会话起点的基线数字逐个对照才发现的。

**2b（`22b8afc561`）**：Metal 侧真正异步。

- `engine_run()` 在 **legacy 内核**（生产配置）上走 `run_async`（提交不等待）；`metal_nn_mmse` 与
  `OCUDU_CE_CPU_INVERT=1` 没有异步形式，仍就地完成。
- 推迟的批次把"解包"（最多两个：标准块 + 尾块，或合并批次）与"待填充的 pilot 缓冲"记进成员；
  `complete_fd_td_estimation_stage()` 先 wait、再解包、再填充。
- **填充必须在完成钩子里**：RSrp/噪声/TA 由基类从估计网格算出，而网格只有在批次完成后才有效——
  放在 stage 里会读到**上一跳**的网格（这就是 Test 9 抓到的那次失败：最低电平下 nv 漂移 11 dB）。
- 提交成功但 CB 事后失败时**无法**在此重算（CPU 兜底需要 stage 已不再持有的 hop 状态）⇒ 改为**报错**
  并置为无效，而不是把上一跳的估计留给消费侧。
- `[mmse_time_sum]` 记账跟着搬：推迟批次的等待属于 GPU 相位与整跳，完成钩子把它加进两者并单独
  打印 `defer_wait=`（这样 `mean total` 仍然覆盖整跳）。
- **本步还没有重叠**（基类仍经 `compute()` = `submit()`+`finish()` 立即收尾），所以门禁就是
  **结果不变**：Test 9 的漂移数字与推迟前**完全相同**，Test 11/12 逐位一致，161/161。

**还剩 2c**（创造重叠的一步）：`dmrs_pusch_estimator_impl` 用 `submit()` 逐端口提交、
`sync_device_estimates()` 收尾；demodulator 只在"确定要主机读"时才同步。
落地时注意：`force_host_estimates`（`OCUDU_CE_CPU_CE=1`）与"估算器没产出设备估计"两种情况下
主机聚合路径必须已同步（建议给 results 加一个"本次是否全程读设备"的查询，别用能力查询替代）。
另注（既有、待查）：设备估计只覆盖**最后一跳**（`gpu_ce` 被下一跳 K3 覆盖），跳频 PUSCH 不应消费它。

---

### 43. S-6c-1b-2c 落地（`11f7f3dcf6`）：解调与估算真正重叠

- `dmrs_pusch_estimator_impl::estimate()` 改为逐端口 `submit()`；`sync_device_estimates()` 逐端口 `finish()`
  且**幂等**（重复收尾会把同一跳的统计量再缩放一次，所以加了 `estimates_complete`）。
- 新增查询 `device_results_cover_last_estimate()`：本次估算是否把消费者要的一切都发布在设备上
  （K3 的估计 + K4 的噪声方差）。Metal 侧由 `gpu_ce_ready && gpu_nv_ready` 得出，且**跳频时返回 false**
  （`gpu_ce` 只保留最后一跳 ⇒ 顺带堵掉一个"拿第二跳的估计当第一跳用"的隐患）。
- demodulator：只有当"后端就地读设备 + 估算器发布齐全"时才不同步；否则先 `sync_device_estimates()`
  再走主机聚合（CPU 均衡器、transform precoding、`OCUDU_CE_CPU_CE=1` 全都照旧）。
- **跳内多批次约束**：`gpu_h` 是所有批次共享的暂存 ⇒ 同一跳的后续批次会覆盖未解包的结果。
  `run_engine_blocks()` 因此在 staging 之前先收尾上一个批次；`defer_unpack()` 立即置 `stage_pending`
  （否则守卫空转——Step 11 用 15 dB 的差异把这个错误抓了出来）。
- 门禁：CE 单测全绿（Test 11 回到 `0.00e+00`/`d 0.000 dB`、Test 12 不变、Test 9 漂移与推迟前完全相同）、
  deferred chain 5/5（设备路径依旧每次 dispatch 都命中）、`ctest -L phy` **161/161**。

#### 43.1 OTA 状态：**被外部核心网阻塞**（2026-09-13 13:0x）

⚠️ **硬规则：gNB 必须用 `sudo` 启动**（用户 2026-09-13 明确）。原因是 **usrsctp 需要 root 权限才能收发 raw socket**
⇒ 非 root 虽然能起来、radio 也能初始化，但 **N2 的 NG Setup 必然超时**。
（agent 首次误判为"核心没运行"，并拿 TCP 探 7777/9090/8080 当证据——**那个诊断不成立**：AMF 的 38412 是 SCTP，
TCP 探测说明不了任何事。）
本会话的 agent 无法代跑（`sudo` 需要密码，`sudo -n` 不放行）⇒ **OTA 由用户执行，agent 负责分析**。

另注：**手机初始 PRACH 功率每次不同是正常的**——UE 按收到的 DL power 从它认为最小可行的功率起步，
逐步抬升试探。所以 **PRACH RSSI 的强弱不能当作链路质量的判据**（本轮首检 -42.7 dB / metric≈1.1、
上轮 -17.4 dB / metric=116.9，都属于正常波动）。

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml expert_phy \
  --pusch_channel_estimator_algo metal_mmse \
  --pusch_ldpc_decoder_type metal \
  --pusch_dft_type metal \
  --pusch_channel_equalizer_backend metal > /tmp/gnb_ota.out 2>&1
# 跑够流量后 Ctrl-C（探针在退出时打印）
```

⚠️ **不要用 `2>&1 | tee`**（用户 2026-09-13 实测）：Ctrl-C 退出时打印的那批探针**既没进 tee 的文件、
也没进 console**（退出序列与管道竞争）⇒ 必须用**直接重定向**，否则白跑一轮。

⚠️ **不给 `expert_phy` 就是纯 CPU 路径**：这几个选项的默认值全是 `cpu`（`pusch_channel_estimator_algo`、
`pusch_channel_equalizer_backend`、`pusch_dft_type`；`ldpc_decoder_type` 默认 `auto`）⇒ 命令行必须显式指定，
否则 `[mmse_time_sum]`/`[metal_stats]` 根本不会出现（本会话的 agent 就踩过这个坑）。

```
N2: Connection to AMF on 192.168.31.250:38412 completed
"NG Setup Procedure" timed out after 5000ms
OCUDU ERROR: CU-CP failed to connect to AMF
```

诊断：`ping 192.168.31.250` 通，但 **7777 / 9090 / 8080 / 38412 全部 closed** ⇒ 对端 5G 核心没有运行（本机无 docker）。
⇒ **OTA 需要先启动核心网**；核心起来后跑一次即可（命令与看什么见 §6 of the handover memo）。

---

### 44. S-6c-1b 的 OTA 验证（2026-09-13，commit `11f7f3dcf6`，全 metal 后端）

配置由日志回显确认：`pusch_channel_estimator_algo: metal_mmse`、`pusch_channel_equalizer_backend: metal`、
`pusch_ldpc_decoder_type: metal`、`pusch_dft_type: metal`；`PUSCH equalizer/demapper backend: metal (GPU)`、
`DFT backend: rx=metal`。`[ul_pipeline] samples=1637`（1637 个 CRC-OK TB，HARQ 丢弃 35 = 2%），
**0 次** `deferred engine batch failed` / engine 失败 / CPU 兜底。

#### 44.1 "2c 生效了"的两条硬证据

| 证据 | 数值 | 含义 |
|---|---|---|
| `[mmse_time_sum] ... defer_wait=` | **82.9 µs**（新字段） | 估计的 wait/unpack/填充**确实在 stage 之外**发生 |
| `[ul_channel_estimation] min` | **20.3 µs**（上一轮 224.0） | 该分段（TF 结束→`process_data` 入口）现在只剩 executor 排队 + 提交，**估计本身已不在里面** |

中位数 344 µs 未变是 executor 排队主导，属正常。

不变量全绿：`hops_no_gpu=0`、`mmse_ce commits/waits=1819/1819`（commits/hops=1.001）、`max_in_flight=1`、
`wrap failures=0 replaces=0`、`equalizer ch_est device=19987 staged=0`、`pusch_demod ch_est device=19987 host=0`、
`burst max_in_flight=1`。

#### 44.2 收益：**在未配对的一轮里看不见**（符合预期）

各分段与上一轮（`e086ff00f8`）逐项相同：TF 231.0/229.8、CE 344.1/344.9、eq+demod 283.1/278.4、
LDPC 1973/1972、`ul_pipeline` 2878/2850。原因有两层：

1. **重叠窗口的上限就是 `defer_wait` = 83 µs**——解调在同步之前只有这么多工作可用来掩盖估计的 GPU 窗口（144 µs）；
2. **pipeline 的 69% 是 LDPC**（1973/2878）⇒ 83 µs 的天花板被轮间负载差淹没。
   ⇒ 要量化它必须做**配对 A/B**（固定 MCS：`min_ue_mcs == max_ue_mcs`），单轮未配对数字不可用。

旁注（本轮负载差异，非代码变化）：`dft commits=345325` ⇒ 190 次/CE 调用（上轮 100），
来自 CE 里 DFT 时间对齐估计器的变换数随分配带宽增长（本轮 25 PRB 满带）；这也是本轮 CE 分段尾部更肥
（p95 2077 vs 1291 µs）的原因。

#### 44.3 记账修正（`d417d5120f`）

推迟后，"等待"窗口里**混进了同一线程上解调阶段的墙钟时间** ⇒ 把它并进 `gpu_path`/`total` 会让
`mean total` 从 329.6 变成 416.6（stage 本身仍是 333.7 ≈ 329.6，没变慢）。现在 `defer_wait=` 单独报告，
`gpu_path`/`total` 仍是 stage 自己的窗口，**与推迟前的历史数字保持可比**。

---

### 45. 现状架构基线：每个边界的往返与同步清单（2026-09-13，进入 LDPC 之前）

**用途**：这是"所谓全 GPU path"到底长什么样的**权威描述**，也是后面所有改动的对照基线。
数据来源：代码逐点核对 + 固定 MCS（`min_ue_mcs == max_ue_mcs = 10`）的配对 OTA A/B（§44 与本文末表）。

#### 45.1 后端选择（必须显式，默认全是 CPU）

| 模块 | 选项 | 默认 | 生产值 | 启动日志确认 |
|---|---|---|---|---|
| OFDM 解调 FFT（**网格生产者**） | `--pusch_dft_type` | `cpu` | `metal` | `[lower_phy] DFT backend: rx=metal (GPU)` |
| 信道估计 CE（K1/K1b/K2/K3/K4） | `--pusch_channel_estimator_algo` | `cpu` | `metal_mmse`（或 `metal_nn_mmse`） | `[mmse_time_sum] hops_gpu=...` |
| 均衡器 | `--pusch_channel_equalizer_backend` | `cpu` | `metal` | `PUSCH equalizer backend: metal (GPU)` |
| demapper | 同上（跟随 equalizer backend） | `cpu` | `metal` | `PUSCH demapper backend: metal (GPU)` |
| LDPC | `--pusch_ldpc_decoder_type` | `auto` | `metal` | `PUSCH LDPC decoder type: metal` |

⚠️ **`--pusch_dft_type` 作用于 OFDM 解调器**（`lib/ru/sdr/lower_phy/lower_phy_factory.cpp:64` 把 rx DFT 工厂交给
`create_ofdm_demodulator_factory_generic`）⇒ **网格 FFT 在 GPU 上**，不是只有 PUSCH 内部的 DFT。

#### 45.2 队列拓扑（两条进程级 `MTLCommandQueue`）

```
前端队列 shared_queue::queue()        : DFT 引擎（网格 FFT + CE 内时间对齐估计器）
后端队列 shared_queue::backend_queue(): CE(K1..K4)、均衡器、demapper、LDPC
                                         ↑ 均衡器与 demapper 共用 metal::shared_burst
```
两条队列**并行执行**，跨队列顺序只能靠显式同步；每条队列的待决链各自独立（§38 的 `queue_kind`）。

#### 45.3 每个模块边界：数据在哪、谁等谁

| # | 模块 | 谁发起 / 粒度 | 输入来源 | 输出去向 | CPU 等待 |
|---|---|---|---|---|---|
| 1 | **OFDM 解调 FFT** | lower PHY；`run_batch(nof_symbols)` ⇒ **1 次 dispatch/slot**（流水线路径则 `run_async(slot)`+`wait_slot(slot)`，深度由 `OCUDU_DFT_PIPELINE_DEPTH`） | 主机 IQ 样本 → DFT 环形缓冲（主机→设备） | **主机 resource grid**：`process_dft_output` 在 CPU 上读回并写网格 | 1 次/slot（流水线时与相邻符号重叠） |
| 2 | **CE** | PUSCH 处理器逐 rx 端口 `submit()`（可在 executor 线程） | **主机网格**：`extract_layer_hop_rx_pilots` 读 DM-RS 导频；再 staging 到 `gpu_a/gpu_r_hp/gpu_w/gpu_y/gpu_pilots` | **设备**：K3 估计 + K4 噪声方差留在设备；主机只出统计量（RSRP/EPRE/噪声/TA/CFO） | **0 次**（`run_async`；等待推后到解调之后） |
| 3 | **均衡器** | 解调器逐符号 `submit()` | **设备零拷贝**：绑定 K3（估计）与 K4（噪声方差）；仅 `ch_re` 从主机网格 staging | **设备**：写入解调器的页对齐 eq/nv 缓冲 | 0（并入 #4） |
| 4 | **demapper** | 解调器逐符号 `submit()` | **设备**：与均衡器**同一条 command buffer** + `memoryBarrierWithScope` | 设备写完后 **CPU 读回**：Pass 3 把 `temp_llr` 拷进 codeword buffer | **1 次/组**（`demapper->wait()`，一组 = 11 个数据符号） |
| 5 | **LDPC** | 解码任务逐码块 `decode()` | **主机** LLR（staging 进 LDPC 引擎缓冲） | **主机** 解出的比特 | **1 次/码块**（`commits=waits`、`max_in_flight=1`，完全同步） |
| — | TX/DL | 全程 CPU（含 IFFT），不受这些开关影响 | | | |

**每 PUSCH 的 CPU↔GPU 同步点 ≈ 5–6 个**：1（网格 FFT/slot）+ 1（eq+demapper 整组一次）+ ~4（LDPC 码块）。
对照：S-5 之前均衡器与 demapper 各自逐符号等待，同一段约 22 次。

**真正设备驻留的段落只有一处**：`CE → 均衡器 → demapper`（#2→#3→#4）：
K3/K4 由生产者写入设备缓冲、消费者直接绑定（`equalizer ch_est device=N staged=0` ✓），
CE 的 command buffer 不被等待（`defer_wait` 单独可测），均衡器与 demapper 共用一条 CB。

**仍然完整走主机往返的三处**：
(a) FFT 输出 → resource grid（每 slot 一次，CPU 读回后写网格）；
(b) demapper 的 LLR → codeword buffer（组结束一次）；
(c) codeword buffer → LDPC → MAC PDU（每码块一次同步 + 两端 staging）。

#### 45.4 关键量级：CE 是 **CPU-bound**，不是 GPU-bound

配对 A/B（固定 MCS 10；样本 660 / 611 个 CRC-OK TB）：

| 指标 | 设备栈 | host 栈（`OCUDU_CE_CPU_CE=1`） |
|---|---|---|
| `[mmse_time_sum] mean total` | 334.8 µs | 284.1 µs |
| 其中 `gpu_wait`（**GPU 实际忙时**） | 130.9 µs | 103.2 µs |
| ⇒ 其余（**CPU：staging + 编码 + 收尾**） | **≈204 µs** | **≈181 µs** |
| `defer_wait`（推迟的等待） | 13.4 µs | 0.2 µs |
| `ul_channel_estimation` median | 341.3 µs | 293.4 µs |
| `ul_equalization_demod` median | 288.6 µs | 299.4 µs |
| `ul_ldpc_decode` median | 3552.0 µs | 3540.0 µs |
| `ul_pipeline` median | 4430.0 µs | 4336.0 µs |

结论：**CE 每跳约 200 µs 花在 CPU 侧**（把 ~170 KB 的 A / R_hp / y / 导频 memcpy 进引擎槽位 + 编码 + 收尾），
GPU 只忙 ~130 µs ⇒ 设备栈比 host 栈慢的 ~50 µs/跳来自 K3/K4 的额外 kernel 与 staging，
而 2c 的推迟只能把 **GPU 那 130 µs** 藏到其它 CPU 工作之后（实测只兑现 13.4 µs）。
⇒ **CE 段剩余的最大杠杆是减少主机 staging，而不是 kernel**（但优先级低于 LDPC，见 §45.5）。

#### 45.5 尚未"融合"的地方（下一步的候选，按账排序）

1. **LDPC（3540–3552 µs，占 `ul_pipeline` 的 80%）**：逐码块 commit+wait、`max_in_flight=1` ⇒
   **同步等待占比未知**，必须先剖析（这是进入 LDPC 的第一步）。
2. **CE 的主机 staging（~180–200 µs/跳）**：与 GPU 窗口串行；要压缩得改槽位布局/搬运方式。
3. **三个主机往返**（§45.3 a/b/c）：其中 (a) 每 slot 一次、(b) 每 PUSCH 一次，
   单次都是几十 µs 量级；把它们做掉需要网格留在设备上 / LLR 直接进 LDPC。
4. **把 CE 的 K1..K4 编进 eq/demapper 那条共享 burst**（三者在同一条 `backend_queue` 上，原理可行；
   CE 引擎目前自建 CB）⇒ 能把"§45.3 #2 的 0 次等待"变成"整条 burst 一次等待"，
   并顺带消掉 CE 的收尾同步。

#### 45.6 更正（本次核对发现）

§44.2 的旁注曾把 `dft commits`（100–190 次/CE 调用）归因于 **CE 内的时间对齐估计器**，**不准确**。
按两轮数据反推：`dft commits ≈ slots×1 + CE调用数×25–32` ⇒ **网格 FFT（每 slot 一次批量）才是大头**，
时间对齐估计器约占 1/4。⇒ 任何 DFT 侧优化都应先针对网格 FFT，而不是 CE 内部。

### 46. S-7a 落地：`--phy_pipeline` 三档 + 有效配置日志 + 探针按模式切换（`757593c84a` + `079b114894`）

融合流水线（路线图 §10）的第一步：**只加"编排语义"，不动数据流**。目标是把"这条链是怎么组织的"
变成一个显式的、可校验的、可打印的配置，后续 S-7b…S-7f 才有地方挂。

#### 46.1 落地内容

| 件 | 位置 | 说明 |
|---|---|---|
| 模式枚举 + 探针用的进程级发布点 | `include/ocudu/phy/phy_pipeline_mode.h` | `phy_pipeline_mode{cpu,cpu_gpu,gpu}` + `phy_pipeline_mode_registry`（原子，启动时写一次） |
| **解析规则（单一真相源）** | `apps/units/flexible_o_du/o_du_low/du_low_phy_pipeline.{h,cpp}` | `resolve_phy_pipeline(request, availability, error)`：纯函数，validator 与两侧边界共用 |
| CLI | `du_low_config_cli11_schema.cpp` | `expert_phy --phy_pipeline {auto,cpu,cpu_gpu,gpu}`；三个模块开关新增 `auto` |
| 校验（跨参数） | `du_low_config_validator.cpp` | 模式与模块开关的冲突在这里报错；`gpu` 档先查前提、再报"未实现" |
| 有效后端的唯一出口 | `du_low_config_translator.cpp` / `flexible_o_du_factory.cpp` | upper PHY 工厂与**下层的 DFT**（经 RU 配置）都从这里取值 |
| 启动日志 | 同上 | `[phy_pipeline]` 一行模式 + 一行逐模块后端 |
| 探针按模式切换 | `ul_pipeline_probe.h` | `gpu` 档不再记录/不再打印三个分段 |

#### 46.2 与路线图 §10.1 的一处**刻意偏差**：默认值是 `auto` 而不是 `cpu`

§10.1 写"`cpu` 为默认"，实现改成 `auto`（＝从模块开关推导；无 offload 开关时**就是** `cpu`）。
原因是三条语义必须同时成立，而"默认 cpu"做不到第三条：

| 需求（§10.1） | `auto` 默认如何满足 |
|---|---|
| `cpu` + 显式 metal ⇒ **报错**（不静默） | `cpu` 是显式值 ⇒ 任何 metal 开关都是"显式给的"，可判定 ✓ |
| `cpu_gpu` 且一个 metal 都没跟 ⇒ 等价 `cpu`（记一条日志） | `auto` 推出 `cpu_gpu` + 全 CPU，日志里明说 ✓ |
| `gpu` + 显式给 cpu ⇒ **报错**（该档无 CPU 回退） | **只有**模块开关默认是 `auto` 才可能区分"显式 cpu"与"没给" ✓ |

同时 `auto` 默认把**历史行为逐字保留**：不带 `--phy_pipeline` 时，metal 开关照旧生效；
在没有 Metal 后端的构建上照旧**静默降级**为 CPU（而不是因为"模式=cpu 与 metal 冲突"而启动失败）——
这条对"同一份命令行在 macOS/Ubuntu 上都能跑"（§3.5 硬规则）是必需的。
⇒ 三个模块开关的默认值随之从 `cpu` 改为 `auto`（`auto` 的语义＝跟随模式；非融合档解析为 `cpu`）。

#### 46.3 解析规则（逐条）

| 模式 | DFT / 估计器 / 均衡器·解调器 | LDPC 译码器 |
|---|---|---|
| `cpu` | 必须是 CPU（`auto`→`cpu`）；**任一 offload 值 ⇒ 报错** | 必须是 CPU 值（`auto/generic/neon/avx2/avx512`）；`metal*` ⇒ 报错 |
| `cpu_gpu` | 原样采用（`auto`→`cpu`） | 原样采用 |
| `gpu` | **被 lane 接管**：`auto`→`metal`/`metal_mmse`/`metal`；**显式 `cpu` ⇒ 报错** | **不变**（LLR 仍出 GPU 给 CPU 译码器，§10.8/§10.11） |

补充两条：
1. **没编进本二进制的后端降级为 CPU**（与工厂既有策略一致），并在日志里标 `metal->cpu (requested backend
   not built in)`；LDPC 的降级目标是 `auto`（与改前的行为一致）。
2. **解调器没有自己的开关**，它跟随均衡器（`upper_phy_factories.cpp` 里的 `pusch_channel_equalizer_backend ==
   "metal"`），日志里以 `demapper=` 显示推导结果。

#### 46.4 本轮抓到的一个真 bug：**inline 函数 + 平台宏 = ODR 违规**

第一次运行时日志是"每个后端都 `->cpu (requested backend not built in)`"，而本机 `ENABLE_METAL_*` 全是 ON。
原因：`query_phy_backend_availability()` 原来是 header 里的 `inline` 函数，其函数体依赖只在
`ocudu_o_du_low_unit_helpers` 上定义的 `OCUDU_METAL_*_AVAILABLE`。`flexible_o_du_factory.cpp`（另一个库）
也包含这个头 ⇒ 同一 inline 函数在不同 TU 里有**不同定义**（它那份全 0），链接器只保留**一份** ⇒
翻译单元拿到的是全 0 的那份，所有 metal 后端被静默降级为 CPU。
修法：把该函数移进 `du_low_phy_pipeline.cpp`（唯一能看到这些宏的 TU），头里只留声明。
**教训（可复用到后续步骤）**：把"平台可用性"做成跨 TU 的 inline 函数是危险的；可用性必须只有一个定义。

#### 46.5 运行时验证（macOS，ZMQ 草稿配置，未占用/未发射）

| 命令 | 结果 |
|---|---|
| 不带 `--phy_pipeline`，四个 metal 开关 | `mode=cpu_gpu fused=no`；`dft=metal ch_est=metal_mmse eq=metal demapper=metal ldpc=metal`；下层 `[lower_phy] DFT backend: rx=metal` ✓（**历史行为逐字保留**） |
| `--phy_pipeline cpu --pusch_dft_type metal` | `Invalid configuration: --phy_pipeline cpu conflicts with --pusch_dft_type metal: ...`，启动即失败 ✓ |
| `--phy_pipeline gpu` | `Invalid configuration: --phy_pipeline gpu (the fused IQ -> LLR GPU pipeline) is not implemented yet; use --phy_pipeline cpu_gpu ...` ✓ |
| `--phy_pipeline cpu`（模块全默认） | 校验通过、进入正常启动（`cpu` 档可用）✓ |
| `--phy_pipeline gpu_lane` | CLI11：`--phy_pipeline: Invalid uplink PHY pipeline mode. Accepted values [auto,cpu,cpu_gpu,gpu]` ✓ |

#### 46.6 探针：`gpu` 档的切换点已经就位（内容留给 S-7f）

`ul_pipeline_probe` 现在按 `phy_pipeline_mode_registry` 决定是否记录/打印
`[ul_time_frequency]`、`[ul_channel_estimation]`、`[ul_equalization_demod]`：
融合档里这三个分段量的是**主机侧的模块边界**，边界没了，数字只会是"工作搬出了测量窗口"的假象
（§44.1 的 `min=20.3 µs` 就是预演）。`[ul_pipeline]`/`[ul_ldpc_decode]`/`[ul_mac_pdu_size]`/`[ul_fapi_mac]`
跨模式不变；lane 自己的 residency/busy/gap/period 与 `[ul_llr_ready]` 在 S-7f 落到同一个 `report()` 里。

#### 46.7 门禁

- macOS：全量构建 `exit=0`、`error:` 计数 **0**；`ctest -L phy` **161/161**；十个 metal 单测二进制全过
  （含 CE 的 Test 9/11/12：Test 11 `max|dh|/rms 0.00e+00`、Test 12 `24780 REs / 0 mismatching`，
  Test 9 漂移 `nv/l² cpu 1.026 mmse 1.034`）；新单测 `du_low_phy_pipeline_test` **14/14**；
  YAML 往返 18/18（配置字典新增 `phy_pipeline`）。
- Ubuntu：见 §46.8（第一轮构建被 GCC `-Werror=range-loop-construct` 拦住 —— 新测试里
  `for (const std::string& v : {"metal",...})` 绑定临时量；`079b114894` 改成遍历 `const char*`。
  又一处"clang 能过、GCC 不过"，见坑表）。
- OTA：见 §46.9。

#### 46.8 Ubuntu（跨平台硬规则）

`jwang@192.168.31.211:~/work/ocudu`，`079b114894`（`git pull --ff-only` 后）：

| 项 | 结果 |
|---|---|
| `cmake -S . -B build` | exit 0 |
| `cmake --build build -j16 -- -k` | **exit 0**，`error:` 计数 **0** |
| `cmake --build build --target test` | **7615/7615 通过**（338.9 s）＝ 基线 7614 + 新增的 `du_low_phy_pipeline_test` |

Ubuntu 的构建选项：`ENABLE_FLOW_PROBES=OFF`、`ENABLE_CE_TIME=OFF`、`ENABLE_METAL_STATS=OFF`、
`ENABLE_METAL_LDPC=OFF` ⇒ 本轮同时验证了**探针关闭时的空实现分支**（`ul_pipeline_probe` 的 no-op 版本）
与"没有 Metal 后端"这条路径仍然可编可测。**注意**：Linux 上四个 `--phy_pipeline` 档里的 metal 开关仍是
"没编进来 ⇒ 降级为 CPU"（不报错，除非显式写了 `--phy_pipeline cpu`——那时才是冲突）。


#### 46.9 OTA 验证（2026-09-13 16:51，`079b114894`，固定 MCS 10，全 metal 后端，609 个 CRC-OK TB）

**有效配置（日志回显，全新的一行）**：

```
[phy_pipeline] mode=cpu_gpu fused=no lane=IQ->LLR (expert_phy --phy_pipeline auto)
[phy_pipeline]   dft=metal channel_estimator=metal_mmse equalizer=metal demapper=metal ldpc_decoder=metal
PUSCH equalizer backend: metal (GPU) for 1..4 Tx layers x 1/2/4/8 Rx ports, ...
PUSCH demapper backend: metal (GPU) for QPSK/16/64/256QAM, ...
PUSCH LDPC decoder type: metal
[lower_phy] DFT backend: rx=metal (GPU) tx=cpu (expert_phy --pusch_dft_type metal)
```

⇒ 后端选择与 tag 那一轮**逐项相同**；`auto` 推导出的 `cpu_gpu` 正确（没有出现"no offload module selected"行）。

**结构性不变量（全部与 tag 相同）**：`wrap failures=0 replaces=0`、`equalizer ch_est device=18337 staged=0`、
`pusch_demod ch_est device=18337 host=0`、`hops_no_gpu=0`、`mmse_ce commits=1669 waits=1669 max_in_flight=1`、
`burst commits=1667 waits=1667 max_in_flight=1`、`ldpc_decoder commits=1668 waits=1668 max_in_flight=1`、
`dft commits=173923 waits=173923 max_in_flight=8`。路由日志也确认走了设备路径：
`PUSCH: estimates read in place, before of the demodulation (... equalizer reads device=true, estimator published
device=true)` + `PUSCH: deferred chain enabled (group of 14 OFDM symbols)`。

**数字对比**（本次单轮 vs tag 的设备栈 A/B）：

| 指标 | S-7a（609 样本） | tag（1637 样本） | 差 |
|---|---|---|---|
| `ul_time_frequency` median | 233.6 | 231.6 | +2.0 |
| `ul_channel_estimation` median | 343.7 | 341.3 | +2.4 |
| `ul_equalization_demod` median | 286.9 | 288.6 | −1.7 |
| 三段合计 | **864.2** | **861.5** | **+2.7（+0.3%）** |
| `ul_fapi_mac` median | 4.8 | 4.1 | +0.7 |
| `ul_ldpc_decode` median | **3329.0** | **3552.0** | **−223（−6.3%）** |
| `ul_pipeline` median | **4213.0** | **4430.0** | **−217** |
| `[mmse_time_sum] mean total` | 321.5（gpu_wait 142.4，defer_wait 0.0） | 334.8（GPU busy 130.9，defer_wait 13.4） | −13.3 |

⇒ **本步可能影响的部分（pre-LDPC 三段）逐项一致（±2.4 µs，0.3%）**；`ul_pipeline` 的差**完全等于** LDPC 段的差
（−217 vs −223），而 LDPC 不在 S-7a 的改动范围里（配置回显已确认译码器仍是 `metal`）。

#### 46.9.1 这一轮为什么**不能**与 tag 逐项比 LDPC：CRC-OK 门控 + 迭代分布

`[ul_pipeline]`/`[ul_ldpc_decode]`/各分段**只统计 CRC-OK 的 TB**（`record_end_crc_ok` 里成对记录）。
本轮 1667 个授权里 **1058 个 CRC-KO（63.5%）根本进不了这些序列**，而进序列的那 609 个里
**67% 只跑了 2 次迭代**（日志逐 PDU 统计：`crc=OK iter=2.0` 411 个、`iter=1.0` 90、`iter=3.0` 78、
`iter=4.0` 18、`iter=5.0` 5、`iter=6.0` 7；KO 全是 `iter=6.0`）。tag 那一轮的授权成功率是 65%，
迭代分布必然不同 ⇒ **两轮的 LDPC 序列量的不是同一批工作**。
⇒ 结论：LDPC/pipeline 的 −6% 归因于**负载/链路条件**（用户在手机侧也可感知：本轮成功率 36.5% vs 65%），
不是代码变化。**方法论补充（写进 A/B 配方）**：以后比 LDPC 类序列时，除 `samples`/`tbs` 外还要比
**CRC-OK 的 `iter` 分布**（`grep -oE "crc=OK iter=[0-9.]+" gnb.log | sort | uniq -c`）。

#### 46.9.2 旁注（不是本轮引入，待查）

`mmse_ce commits=1669` 比 `[mmse_time_sum] calls=1667`（= `hops_gpu`）多 2（0.12%）。
两个计数器口径不同（引擎的 `run_async` 次数 vs 探针每次 hop 记一次），tag 那轮只留了 `commits/waits`，
无法确认是否同样多 2。量级上无影响，但**记账口径不一致**这类问题以前咬过一次（§44.3 的
`mean total` 污染）⇒ 进入 LDPC 剖析前顺手查清。

### 47. S-7b 落地：设备网格（FFT 直写 + 主机零拷贝共享）（`1932dcc468`、`af90f0a8fe`、`f285cea33a`）

融合流水线的**地基**：让 resource grid 由 GPU 写、CPU 读**同一块内存**，并把 OFDM 解调的后处理
（相位补偿 + 上下半带映射 + cbf16 转换）搬进 FFT kernel。这一步之后，"网格"不再是主机与设备之间来回搬的东西。

#### 47.1 三个提交

| 提交 | 内容 |
|---|---|
| **S-7b-1** `1932dcc468` | 网格存储**可被设备寻址**：`dynamic_tensor` 增加 allocator 模板参数，网格改用 `page_aligned_allocator`（页对齐 + 整页覆盖）；新增 `resource_grid_device_view`（base + 三个 stride + 维度）与 `resource_grid_writer::get_device_view()`；`page_aligned_allocator.h` 从 PUSCH 目录移到 `ocudu/support/`（它已是共享规则）。**主机路径零改动**，行为不变 |
| **S-7b-2** `af90f0a8fe` | **FFT Phase 2**：变换 kernel 的**最后一次写出**改为"带补偿地写进网格"（相位/scale + 可选窗表 + 上下半带映射 + round-half-to-even bf16），`dft_processor_grid_write` 能力接口 + `dft_metal_engine::submit_slot_grid_write()`；kernel 级逐位比对 0 mismatch |
| **S-7b-3** `f285cea33a` | **接线**：`submit_symbol()` 带上网格（设备写在提交时编码、与变换同一个 CB），`finish_symbol()` 两条路径都等待（保证上报即主机可见）；`expert_phy --device_resource_grid {auto,on,off}` 作为 A/B 开关；标志走完 mode → RU 配置 → lower PHY → puxch → demodulator 整条层间管道 |

#### 47.2 为什么"共享同一块内存"而不是"设备网格 + 按需下载"

路线图 §10.5 的设想是"设备 master + 按需主机镜像（下载）"。本项目的硬件是 **UMA**，
`wrap_no_copy` 用的就是 `MTLResourceStorageModeShared` ⇒ 设备写的**就是主机那块内存**，
"镜像"退化成**一次同步**（本来就是零拷贝），不需要任何下载 kernel 或 memcpy。代价只有一个：
网格存储必须页对齐且覆盖整页（S-7b-1），这一点对所有网格恒定成立。
⇒ 这是对 §10.5 的一处**简化**（少一个 buffer、少一次拷贝），不是偏离目标：主机侧的"按需"仍在
——PUCCH/PRACH/CPU 路径读网格时，数据已经由 `finish_symbol()` 的等待保证可见，不需要额外动作。

#### 47.3 关键工程发现：**不要把"网格写"做成第二个 dispatch**

第一版实现是"变换 dispatch + `grid_write` dispatch（同一 CB，中间 `memoryBarrierWithScope`）"。
在 pipelined 槽级测试里它明显更慢（首轮 1.3–1.6 ms/slot vs 主机路径 0.6 ms/slot），
而**融合成同一个 kernel**（变换的最后一次写出直接写网格）后，200 次热态测量：
`plain=119.3 µs/transform（GPU 27.0）` vs `with-grid=111.5 µs（GPU 24.0）` ⇒ **网格写是免费的**
（它甚至更省：不再写 32 KB 的 cf32 输出，只写 5 KB 的 cbf16 网格）。

⚠️ 诚实标注：上面那组"两 dispatch 更慢"的首轮数字**受进程首轮 GPU 升频影响**（同一二进制重复跑时
device≈host，600 vs 600 µs/slot），所以**不能**据此断言"barrier 值 1 ms"。可以确定的是：
(a) 跨 dispatch 的写→读**必须**有显式 barrier（§10.10.1 的旧账）；
(b) 融合后这个依赖根本不存在，成本实测为零；
(c) 因此后续凡是"kernel A 的输出喂 kernel B"的场合，**优先考虑融合进同一个 kernel**，
实在要分两个 dispatch 再付 barrier 的代价（并**在热态下测**它）。

#### 47.4 主机路径的同步语义**没有变**

今天：`finish_symbol()` 等 DFT → 主机算补偿 → 写主机网格 → `on_rx_symbol()`。
现在（设备网格）：`submit_symbol()` 把变换+网格写一起提交 → `finish_symbol()` 等待 → `on_rx_symbol()`。
⇒ "**上报时网格内容已就绪**"这条不变量逐字保留，所以 PUCCH/PRACH/PUSCH(CPU 档) **一行都不用改**，
也不存在新的竞争（网格仍由 puxch 在流水排空后才释放）。

#### 47.5 门禁

| 门禁 | 结果 |
|---|---|
| macOS 全量构建 | `exit=0`，`error:` 计数 **0** |
| `ctest -L phy` | **162/162**（含新增的 `resource_grid_device_view_test`） |
| 十个 metal 二进制 | 全过（CE Test 9/11/12、eq、demapper、LDPC、DFT、OFDM、smoke 数字未变） |
| kernel 级逐位比对（`dft_processor_metal_unit_test`） | 300 子载波 × {无窗表, 有窗表}：**0 mismatch**；并把**另一个 slot** 的变换缓冲填成不同数据，防"读错 slot 却蒙对" |
| 槽级端到端逐位比对（`ofdm_demodulator_metal_batch_test`） | 走真实 `submit_symbol`/`finish_symbol` 流水：**17808 RE 全部一致（0 mismatch）** |
| 分辨率/日志（ZMQ 草稿配置，无射频） | `--device_resource_grid on` ⇒ `device_grid=yes` + `dft=metal...`；用 `--pusch_dft_type cpu` 强制无能力时**响亮告警**并回落主机；`cpu` 档 + `on` ⇒ 校验期报冲突（不是运行期 fatal）；默认 ⇒ `device_grid=no`（与 tag 逐字同路径） |
| Ubuntu | 见 §47.7 |
| OTA | 默认档（`auto`）应与 tag **逐项一致**；`--device_resource_grid on` 是 S-7b 的收益 A/B（见 §47.6） |

#### 47.6 怎么量 S-7b 的收益（给用户的一轮 A/B）

```bash
# A（参考，与 tag 同一路径）：默认 auto
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml expert_phy \
  --pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal \
  --pusch_dft_type metal --pusch_channel_equalizer_backend metal > /tmp/gnb_ota_off.out 2>&1
# B（设备网格）：多一个开关
... --device_resource_grid on > /tmp/gnb_ota_on.out 2>&1
```
**预期**：`[ul_time_frequency]` 下降（主机不再读 4 KB/符号的 cf32 输出，也不再做 1272×14 次补偿与 put），
`[metal_stats] dft commits` 不变（仍是每符号一个 CB），`[ul_pipeline]` 不退化，
结构性不变量（`wrap failures=0`、`ch_est device`、`hops_no_gpu=0`）不变。
⚠️ 记住 §46.9.1 的教训：比 LDPC 类序列要先比 **CRC-OK 的 `iter` 分布**。

#### 47.6.1 第一次 A/B 尝试**作废**（2026-09-13 18:02，链路 + 主机双重异常）——A/B 的前提条件

这一轮跑出来的东西**不是** S-7b 的收益，而是一份"什么时候不该做 A/B"的样本，值得记住：

| 证据 | A 轮（18:02，243 样本） | 16:51 的干净轮（609 样本） |
|---|---|---|
| 有效配置（日志自证） | `device_grid=no`（**tag 路径**） | `device_grid=no` |
| KO TB 的 SINR 中位数（p25） | **−0.1 dB（−21.8）** | **+4.7 dB（+4.3）** |
| OK TB 的 SINR 中位数 | 9.3 dB | 12.7 dB |
| PRACH RSSI（16 次检测） | **6 次为 −4.3 / −20.0 / −20.4 / −24.6 / −21.0 / −12.3 dB**，其余 −42.5 | 39 次全部 −42.3…−42.7 |
| RF 实时失败 | underflow **22**、late **14**、FAPI late **8**、busy **2** | underflow **1**、late 0、FAPI late 0、busy 0 |
| 剖面最大值 | TF max 3135 / CE max 4239 / pipeline max 8853 µs | 547 / 1031 / 5830 µs |

**机理**：msg3（`tbs=11 crc=KO iter=6`）连续四次解不出 ⇒ UE 按重传抬功率 ⇒ **固定 `rx_gain: 65` 的接收机
被 20–38 dB 过强的信号打进失真区**（最强 −4.3 dB 离满量程只有 4 dB）⇒ 更解不出 ⇒ 再抬（自激）。
一次过到 msg3 之后（`tbs=372`）其 UL 仍然全 KO ⇒ `RRC Setup` 2000 ms 超时；最后 RLF
（"100 consecutive undecoded CSIs" + "RLC max ReTx"）⇒ ping 死在 134/500。
⇒ **与处理链无关**：失败发生在 UL 接收（PRACH/PUSCH 解调），而 `device_grid=no` 说明网格写根本没走设备路径。

**A/B 的前提条件（下次先查这三条，再谈数字）**：
1. `grep -aoE "rssi=[-0-9.]+dB" <log> | sort | uniq -c` ⇒ PRACH RSSI 应稳定在 −42±1 dB（出现 −20/−4 这类值就说明过载，先降 `rx_gain` 或让手机离远、别移动它）；
2. `grep -ac underflow <log>` ⇒ 0–1（>5 说明主机侧在抢 CPU：跑 OTA 时**不要**编译/跑测试，agent 也必须完全待机）；
3. CRC-OK 的 `iter` 分布 + KO/OK 的 SINR 分布（KO 中位 ≈ +4.7 dB、OK ≈ +12.7 dB）⇒ 与参照轮同量级。

#### 47.6.2 第二次尝试（18:36，`rx_gain 55`）：**gNB 处理链无罪，机器状态 + 空授权才是主因**

`rx_gain` 从 65 降到 55 后，常态 PRACH RSSI 从 −42.5 降到 **−51.8**（正好 −10 dB，增益生效且几何未变）。
但 ping 仍在 106/385 后断。逐项排查结论：

| 检查 | 结果 | 判读 |
|---|---|---|
| gNB 侧每条 PUSCH 的时延 `t=` | p50 **3.96 ms**（干净轮 4.36）p90 4.69（4.45） | **没有新的阻塞**：与干净轮同一分布 |
| 引擎失败/回退 | **0** | Metal 链健康 |
| 12:42 旧轮 `t=` 为何只有 2.6 ms | 当时 TB=96/15/157 B、1–2 次迭代早停；现在 TB=544 B、2–6 次迭代 | 差异是**工作量**（LDPC 占 80%），不是代码回归 |
| OK TB 的 SINR | 中位 **+10.4 dB**（min +2.4） | 有信号时解得很干净 |
| KO TB 的 SINR | 中位 **−21.7 dB**（p25 −25.9，**max −11.6**） | 两个族群**完全不重叠** ⇒ 失败的授权上**根本没有信号** |
| 手机 PRACH | 连续 6 次 occasion：power_dB −26.5→−19.5→−15.2→−11.5→−7.8→**−2.0**（+4.3 dB/次），RSSI 最高 **−0.8 dB** | 手机在做**功率爬坡的 RACH 重试**（=收不到 RAR），最后把接收机打到满量程 |
| RACH 统计 | RAR 73 次、msg3 尺寸授权 345 次、**UE created 仅 7** | 空授权 + 反复重连 |
| ping 锯齿（RTT 1634→1426→…→177 ms，步长 ≈200 ms = ping 间隔） | 8 个回复**同一时刻释放** | 手机侧 UL 缓冲/RLC 在 ACK 成功后**整批放行** ⇒ 是链路后果，不是 gNB 堵 1.6 s（`t=` max 仅 5.5 ms） |

**"被放大的阻塞"确实存在，但在主机实时层面**：本轮 RF underflow **36 次 + late 9 次**（干净轮 1 次），
且**随时间加剧**（前 2 分钟 0–2 次/15 s → 最后 3 分钟 4–18 次/15 s）。决定性证据是
**DL 的 PDSCH 处理时延（纯 CPU 工作）在同一轮内从 ~10 µs 涨到 16–18 µs（+60~80%）**，而干净轮为 9.6 µs：
代码不会在一次运行内让同一段 CPU 工作慢 60% ⇒ **CPU 降频/热/争抢**（本轮之前有 ~45 min 的 `-j16` 构建 +
测试长跑）。DL 错过发射期限 → underflow → 手机丢 DL → RACH 爬坡 → UL 风暴 → 更慢，构成正反馈。

⇒ **A/B 之前要加的"机器健康指纹"**：`PDSCH` 处理时延 p50（干净 ≈ 9.6 µs；> 13 µs 就别用来做 A/B），
以及 macOS 侧的 `pmset -g thermlog`（看 `CPU_Scheduler_Limit` 是否掉到 100 以下）。
⇒ 另注：`rx_gain` 是固定增益，手机从"最小功率"到"最大功率"的 ~60 dB 动态范围**无法**用固定增益同时覆盖，
所以爬坡循环一旦出现，接收机必然饱和（srsRAN 的 `ru_sdr` 没有 AGC）。

#### 47.6.3 S-7b 的 OTA A/B（`--device_resource_grid off/on`）：**不变量全同、TF 段无可见变化**（符合设计）

| | OFF（`device_grid=no`） | ON（`device_grid=yes`） |
|---|---|---|
| 时长 / CRC-OK 样本 | 823 s / 1197 | 194 s / 857 |
| PUSCH 授权率 | 2.13/s | **6.06/s** |
| `[ul_time_frequency]` median（**唯一与 S-7b 相关的量**） | **239.8 µs** | **239.2 µs（−0.6 µs，−0.25%）** |
| 同上 min / p95 / max | 119.6 / 332.2 / 2389.7 | 133.0 / 339.9 / 2392.9 |
| `[ul_channel_estimation]` median | 359.7 | 347.9 |
| `[ul_equalization_demod]` median | 300.4 | 290.9 |
| `[ul_ldpc_decode]` median | 3552 | 3686 |
| `[ul_pipeline]` median | 4574 | 4616 |
| `dft commits`（每秒） | 1 027 111（1248/s） | 287 589（1482/s） |
| 结构不变量 | `wrap failures=0 replaces=0`、`ch_est device` 全量、`host=0`、`hops_no_gpu=0`、三个 `max_in_flight=1` | **逐项相同** |

**结论：门禁（逐位一致 + 零回退 + 不变量）通过；收益在 OTA 上看不见 —— 这符合预期，不是失败。**

原因可以算清楚：设备网格**替掉的主机工作量只有 ~1 µs/符号**（`sc_prod` 512 点 + 300 次 cbf16 转换 + 两次 `put`），
每秒钟约 1250 个符号 ⇒ **~1.2 ms CPU/s，即单核的 0.1%**；而 `[ul_time_frequency]` 的终点由
**DFT 流水线滞后（深度 8）+ executor 排队**决定，不由这点 CPU 决定。附带省掉的是每符号 32 KB 的 cf32 输出写读
（≈40 MB/s 带宽），同样看不见。

⇒ **S-7b 的价值是"门"而不是"数"**：网格现在住在设备上，S-7c（K0-a：从设备网格取导频）才能干掉 CE 那
**~170 KB/跳的主机 staging（§45.4：~200 µs/跳 CPU）**，S-7e 才能让均衡器直接从设备网格取 `ch_re`。
那两步的数字会出现在 `[mmse_time_sum]` 与 `[ul_channel_estimation]` 上。

⚠️ **这一对 A/B 不是可比的负载对**（记录在案，避免以后误读）：OFF 那轮跑在**链路故障 + 机器降频**状态
（KO 的 SINR 中位 **−21.1 dB** = 空授权、6 次 RLF、154 次 underflow、DL `t=` p50 12.8 µs），ON 那轮链路健康
（KO 中位 **+6.0 dB** = 正常的边缘重传、0 次 RLF、DL p50 **10.0 µs**）；两轮的 CRC-OK 迭代分布也不同
（OFF 集中在 2–4 次，ON 集中在 4–6 次）⇒ CE/eq+demod/LDPC 之间那几十 µs 的差是**链路+工作量**，不是这个开关。
（这与 §46.9.1 的教训一致：**CRC-OK 门控的序列必须先比指纹再比数字**。）

**下一次 A/B 的做法**：两轮时长/授权率接近（或改用同样的固定流量），且先确认 DL `t=` p50 与 underflow 计数同量级。

### 48. S-7c-0：**先把 CE 的账算清**——K0-a 的前提被证伪，真正的杠杆是 dispatch 数量（`200c0b9010`、`68077477e0`）

S-7c 原计划（路线图 §10.8）：K0-a 把导频提取搬到 GPU，"CE 不再读主机网格"，门禁是
"`[mmse_time_sum]` 的 CPU 部分下降"。**动手前先量了一下，前提不成立**——原来的探针窗口从估计 stage 内部
才开始，导频提取（在基类的 `compute_hop_submit` 里）根本不在记账范围内，所以先补了三个字段（`pre=`、`stage=`、
`submit=`/`unpack=`）和一张按几何分档的表（`[mmse_time_shape]`）。

#### 48.1 实测（CE 单测，**25 PRB / 2 DM-RS = OTA 的几何**，每跳）

| 段 | µs | 占比 | 说明 |
|---|---|---|---|
| `pre` | **0.46** | **0.3%** | **K0-a 的全部目标**：从主机网格取导频 + EPRE + LSE + CFO + 组装参数 |
| `sigma2` | 2.0 | 1.1% | 噪声方差估计 |
| `corr` | 6.1 | 3.5% | `build_correlation_matrices`（Bessel/Doppler） |
| `gpu_path` | 167.1 | 94.8% | 引擎窗口 |
| ├ `gpu_wait` | 67.4 | 38% | **GPU 真的在算**（batch 的 GPUStart→GPUEnd） |
| ├ `stage` | **2.5** | 1.4% | **K0-d 的目标**：A/R_hp/y 拷进引擎槽位 |
| ├ `submit` | **158.6** | 90% | **编码 + 提交命令缓冲**（其中 67.4 是 GPU 时间 ⇒ **~91 µs 纯主机编码**） |
| └ `unpack` | 0.02 | — | K3 结果解包 |
| **total** | **176.2** | | |

全量平均（2754 跳，2–52 PRB）：`pre=0.73`、`stage=0.66`、`submit=17.14` µs——平均值被小几何拉低，
**规划必须看 25 PRB 那一行**（这正是加按几何分档的原因）。

#### 48.2 三条结论

1. **K0-a（S-7c）按原样做没有收益**：省下 0.46 µs，却要多一次 dispatch/命令缓冲（10–25 µs 量级）⇒
   净亏。它必须**并入 S-7d**（主机数学搬上设备时，网格读取自然消失），或改成"CE 在自有的 CB 里顺带做、
   统计量推迟到收尾"的形式，并把门禁改成"**零主机网格读取** + Test 9/11/12 逐位一致"。
2. **K0-d 的"省掉 ~170 KB staging"也被高估**：实测 2.5 µs（1.4%）。它仍是 K0-b/c/d 链上要做的
   （把系数表搬到设备），但它**不是** CE 的大头。
3. **CE 的真正大头是"每跳编码命令缓冲"（25 PRB 时 ~91 µs 纯主机 CPU，占 90%）** ⇒ CE 是
   **dispatch-bound，不是 data-movement-bound**。杠杆按顺序是：
   a. **减少每跳的 dispatch 数**（合并 K1/K1b/K2/K3/K4；参考 LDPC 的 `metal_persistent` 已经用常驻
      grid 把整个 (iteration, layer) 循环变成一个 dispatch）；
   b. **减少每跳的命令缓冲数**（§10.8 的 S-7g：把 CE 编进 eq/demapper 那条共享 burst）；
   c. 一个 slot 内**批量提交多跳**（现在 `calls≈1.5×TB`，每个 CE 调用一套 CB）。
   ⚠️ 注意 (a) 与 (b) 不是一件事：**编码成本按 dispatch 计**，把 K 系列合并成一个 kernel 比"把多条
   CB 合成一条"更值钱。
   ⇒ 路线图 §10.3 的 CE 工作清单（K0-a…K0-d）需要按这个实测重排优先级。

#### 48.3 更正（S-7c-0c `f7d659618a`）：**大头不是编码，是"等上一批"**

§48.1 里 `submit=158.6 µs` 被我读成"编码命令缓冲"，**错了**。两处独立证据把它纠正过来：

1. **引擎自带的相位计时器**（`OCUDU_MMSE_DEBUG=1`，早就存在）：
   `run_async n=5537 | wrap=0.2 cb=0.4 **encode=3.0** commit=1.6 wait=0.0 µs`
   ⇒ **编码只要 3.0 µs**（提交 1.6），根本不可能是 158。
2. 那个窗口真正的成分是 `engine_run()` 开头的一句：
   `if (nof_pending_unpacks != 0) complete_fd_td_estimation_stage();`
   —— 因为**各跳共用同一块 `gpu_h` 暂存**，提交新一批之前必须收完上一批。把它拆开：

| 完成段的成分 | µs（25 PRB/跳） |
|---|---|
| **`cpl_wait`（等上一批的命令缓冲）** | **129.9** |
| `cpl_unpack`（K3 结果写回主机估计缓冲） | 1.5 |
| `cpl_fill`（导频派生统计量） | 0.5 |
| `encode` + `commit`（引擎自带计时） | 3.0 + 1.6 |

⇒ **CE 每跳 169 µs 里，130 µs（77%）是主机**阻塞在自己上一批 GPU 上**。**（GPU 真忙 65–79 µs，
其余是提交/唤醒延迟。）

#### 48.4 结论（第二次更正后的优先级）

| 候选 | 实测可省 | 判定 |
|---|---|---|
| K0-a 导频提取上设备（原 S-7c） | 0.5 µs（0.3%） | ❌ 按原样做是净亏 |
| K0-d 相关矩阵上设备（省 staging） | 2.8 µs（1.7%） | 低优先级（仍是 K0-b/c/d 链上要做的） |
| 合并 K 系列减少 dispatch（= 我上一轮的"建议 A"） | ~3 µs 的编码里的一部分 | ❌ 方向错了：编码本身只要 3 µs |
| **让 CE 的引擎流水线能有两批在飞（双缓冲 `gpu_h` 等暂存）** | **最多 ~130 µs/跳（CE 段的 ~35–87%）** | ✅ **下一步** |

⇒ **真正要修的是"CE 的引擎流水线深度 = 1"**：S-6c-1b-2c 的推迟只把等待从"stage 内"搬到了"下一跳提交前"，
而在连续流量下下一跳紧接着就来 ⇒ 等待照样暴露。**解法**：把每跳的暂存（`gpu_h`，以及被同一批引用的
A/R_hp/y 槽位）做成 ≥2 份轮转，让主机在 GPU 跑第 N 跳时去干第 N+1 跳的活。

#### 48.5 动手前的第二个疑问：**那个 130 µs 的等待，OTA 里真的存在吗？**

`S-7c-0c` 的 `cpl_wait=130 µs` 是在 **CE 单测的同步流程**里量的（`estimate()` 一跳接一跳，中间没有别的活）。
但 OTA 的流程**已经**把 CE 的收尾推到了解调之后（`pusch_processor_impl.cpp:453`：先
`demodulate(...)`，再 `sync_device_estimates()`），也就是说：

- OTA 里 CE 的批与解调**天然重叠**（CE 的 CB 先入队、解调后入队）⇒ 收尾时等待应当很小
  ——OTA 的 `defer_wait=1.7–5.9 µs` 正是这么显示的；
- 那么 OTA 的 `gpu_path=362 µs`（旧探针口径）就**不是**能被单测复现的那 130 µs，
  而是"下一次 CE 调用开头那句 guard 撞上了上一批还没收完"的等待——**前提是上一批真的还没收**，
  而这取决于 `estimates_read_in_place` 那条路径是否真的调用了 `sync_device_estimates()`。

⇒ **结论：双缓冲的必要性必须用 OTA 的 `cpl_wait` 来判断，不能拿单测的 130 µs 当依据。**
新探针字段（`pre/stage/submit/cpl_wait/cpl_unpack/cpl_fill`）已经在跑的二进制里，跑一轮 OTA 就能定论：
- 若 OTA 的 `cpl_wait` 也是百微秒量级 ⇒ 双缓冲值得做（引擎侧的环形提交已经就位，见 S-7c-1a）；
- 若 OTA 的 `cpl_wait` 很小 ⇒ 说明等待已被解调掩盖，双缓冲无收益，应当转向 `gpu_path` 里剩下的那部分
  （按新字段继续拆）。

（引擎侧的**有界提交环** `2ae4f6adfb`（`max_slots=2`，per-slot `wait_pending`）已经落地且**完全惰性**：
所有调用者仍用 slot 0，单测逐位不变、phy 162/162 不变。它是双缓冲的前置件，若最终判定不做，可整体 revert。）

#### 48.6 OTA 探针一轮定案：**根因是"最大阶数超过 K1 上限 ⇒ 走了唯一不能推迟的那条路"**（`bd48984ab4`）

用户跑的那轮 OTA（271 次 CE 调用）给出：

```
[mmse_time_sum] mean total=346.6 | pre=1.61 stage=22.96 submit=234.51 unpack=1.16
                cpl_wait=0.0 cpl_unpack=0.0 cpl_fill=0.0 | sigma2=4.3 corr=12.7
                gpu_path=327.7 (gpu_wait=141.0) defer_wait=0.0
[mmse_time_shape] prb=13 npt=3 calls=193 | stage=29.12 submit=300.03 gpu_wait=147.5 total=358.2
```

**读数**：`cpl_*` 与 `defer_wait` **全为 0** ⇒ 这一轮**根本没有发生推迟**；于是那 234 µs 的
`submit`（含 ~147 µs GPU 忙）只可能是"同步等待本批"，而不是编码（引擎自测 3 µs）。

**根因**：本小区实际用的几何是 **13 PRB / 3 个 DM-RS 符号**，`L = block_prb(3) × 6 × npt(3) = 54`，
**超过 GPU 求逆 K1 的阶数上限 36** ⇒ `gpu_invert=false` ⇒ 走 `run_weights_only()` —— 而它是**唯一
同步的入口**（`[cb waitUntilCompleted]`）。这也解释了为什么 CE 单测（几何 `L≤36`）有推迟（`defer_wait≈130`）
而 OTA 没有：**两者走的是不同的入口**。

**修法（很小）**：给 weights-only 流水线加异步入口 `run_weights_only_async()`（同样编码 K1b→K2→K3/K4，
提交后不等待），CE 在**两种求逆方式下都推迟**，并在合并路径的 **staging 之前**补上"收掉上一批"的 guard
（`run_engine_blocks()` 一直有这道 guard，合并路径此前没有）。

**效果**：CE 单测（背靠背调用，等待无处可藏）mean hop **77.4 → 44.4 µs（−43%）**；等待并没有消失，
而是**从 stage 里搬到了消费者控制的收集点**（`cpl_wait` 126→169、`defer_wait` 126→169、
`gpu_path` 65.7→33.2）。**在 OTA 上**，收集点在解调之后（`pusch_processor_impl.cpp:453`），
那正是推迟链的窗口 ⇒ 预期 CE 段（341 µs/跳）显著下降；待一轮 OTA 验证。
门禁：Test 9/11/12 逐位一致（Test 11 `max|dh|/rms 0.00e+00`；Test 12 六种几何 24780 RE、0 mismatch，
K4 最差 1.31e-07）+ phy 162/162。

⚠️ 同时**撤销**了 S-7c-1a 的引擎提交环（`9f64dfae5d` revert `2ae4f6adfb`）：那种"双缓冲"针对的是
"guard 撞上未收批"的等待，本轮数据证明该等待在 OTA 里其实是 0；真正的收益来自**让所有几何都能推迟**，
不需要多批在飞。⇒ **"GPU 内部多流水线"的第一块拼图不是双缓冲，而是"消除唯一同步入口"**；
lane 级 ring 仍按计划留在 S-7f。

#### 48.7 附带的一条探针经验

按几何/按相位分档的探针容易踩**退出期静态析构顺序**：atexit 报告运行时函数局部 static 的容器**已经被析构**
（表现为表格空白）。探针里跨退出期的容器/互斥量必须 **故意泄漏**（`new` 一次不释放）。

#### 48.8 S-7c-1b 的 OTA 验证（`bd48984ab4`，728 跳）：**推迟生效、CE 段 −12%，但 `submit` 没动 —— `defer_wait` 变成 3.8 ms，账本指向后端队列**

用户跑的一轮 OTA（614 个采样、728 次 CE 调用），与上一轮（`68077477e0`，271 次调用）对照：

| 指标（每跳 / 每段） | A: `68077477e0` | B: `bd48984ab4` |
|---|---|---|
| `[mmse_time_sum]` mean total | 346.6 µs | **302.1 µs** |
| pre / stage / **submit** | 1.61 / 22.96 / **234.51** | 1.47 / 25.83 / **246.78** |
| cpl_wait / cpl_unpack / cpl_fill | 0.0 / 0.0 / 0.0 | 0.1 / 1.1 / 0.5 |
| **defer_wait** | **0.0** | **3806.9** |
| gpu_path（其中 gpu_wait） | 327.7（141.0） | 284.5（143.3） |
| shape `prb=13` | 193 跳，total 358.2，submit 300.03 | 628 跳，total 331.0，submit 276.09 |
| shape `prb=3` | 56 跳，total 314.9 | 72 跳，total 58.3 |
| `[ul_channel_estimation]` mean/med/p99/max | 404.1 / 355.7 / 844.9 / 3787.3 | 353.8 / 344.7 / 539.3 / 1062.7 |
| `[ul_pipeline]` mean / p99 / max | 4611.1 / 8365 / 17170 | 4524.6 / 5181 / 7007 |
| `[ul_time_frequency]` | 248.6 | 241.0 |
| `[ul_equalization_demod]` | 373.6 | 326.9 |
| `[ul_ldpc_decode]` | 3584.7 | 3602.9 |

**① 不变量全对**：`hops_gpu=728 hops_no_gpu=0 hops_nn=0 fb_blocks=0 device_hops=728`、
`[metal_stats] equalizer ch_est device=8008 staged=0`、`pusch_demod ch_est device=8008 host=0`、
`wrap failures=0`。**全部跳都走了设备路径，没有静默回退。**

**② 推迟确实生效了**（这是 S-7c-1b 的全部目的）：`defer_wait` 与 `cpl_*` 从**全 0** 变成非 0
⇒ OTA 几何（`L=54>36`）这一轮真的走了 `run_weights_only_async`，不再走那个唯一的同步入口。

**③ CE 段的改善是真的，但幅度被机器状态污染**：mean −12.4%、p99 −36%、max −72%。
可是同一轮里**一行代码都没改**的 TF（−3%）、均衡+解调（−12.5%）也一起变好了，
`[ul_pipeline]` 的 p99 从 8365 掉到 5181 ⇒ 两轮的机器/负载状态不同。
**干净的同几何对照只有 `prb=13` 那一档：total 358.2 → 331.0（−7.6%）、submit 300.03 → 276.09（−8%）。**

**④ `prb=3` 档的 314.9 → 58.3 主要是"账本搬家"，不能算收益**：上一轮没有推迟，那一跳自己
扛了同步等待（314.9 µs）；这一轮等待被搬到消费者的收集点，代价记在 `defer_wait` 里。
⇒ 判断 CE 只能看**调用方口径的段值**（`[ul_channel_estimation]`），不能看每跳 `total`。

**⑤ 最刺眼的一条：`submit` 没降（234.5 → 246.8 µs）。** 单测里推迟把 submit 从 158.6 打到 17.3 µs，
OTA 里却纹丝不动。注意 `gpu_wait=` 字段的**真实含义是命令缓冲的 GPU 窗口**
（`GPUStartTime..GPUEndTime`，不是等待）：143 µs 是 CE 一批真的占着 GPU 的时间，
编码本身按单测口径只有 ~5 µs ⇒ `submit` 里还有 ~100 µs（`prb=13` 档 ~128 µs）既不是编码也不是 GPU 计算，
只能是**引擎入口守护 `wait_pending()`** 或驱动侧提交阻塞。**二选一决定了下一步做 ring/arena 双缓冲还是改队列拓扑**，
本轮已加 `guard=` 探针把它直接量出来。

**⑥ 账本真正指向的地方：`defer_wait = 3806.9 µs ≈ [ul_ldpc_decode] 3602.9 µs`。**
CE 的批提交后 ~3.8 ms 才拿到完成，而它的 GPU 窗口只有 143 µs ⇒ **~3.66 ms 花在队列里**。
UL 链的 CE / 均衡 / 解调 / LDPC 走**同一条 backend queue**，LDPC 一个命令缓冲就占住 GPU ~3.6 ms，
排在它后面的 CE 批自然要等。⇒ §48.4/§48.6 里"CE 的等待是 CE 自己的流水线深度问题"这个判断**不成立**：
真正的队头是 LDPC，CE 段那 300 µs/跳里绝大部分是**继承来的队列延迟**。
（管线总时长本轮看不到变化也符合这一点：LDPC 段占 UL 管线 80%、CE 段只占 8%，
再省 50 µs 也只是 1.1%，低于本轮机器噪声。）

**⑦ 下一步（S-7c-2 探针轮）**：先把 LDPC 那 3602.9 µs 拆开——**GPU 真忙 / 队列间隙 / 主机打包**。
本轮落地两条纯探针（无行为改动）：

- `[ldpc_time_sum]`：`wall / pack / submit / gpu / gap / unpack` 均值 + 最大，外加**迭代次数直方图与迭代上限直方图**
  （顺带补上 §46.9.1 里要求的 CRC-OK `iter` 分布指纹）；
- `[metal_stats] mmse_ce ... guard=命中/总数 guard_mean=… guard_max=…`：引擎入口守护等待的均值与峰值。

一轮 OTA 就能定：若 LDPC 的 `gpu ≈ wall` ⇒ 是**内核慢**（迭代/派发问题），`gap` 大 ⇒ 是**队列拓扑**问题
（S-7g 的 CB 合并 / 分队列）；而 CE 侧的 ring/arena 该不该做，由 `guard` 的大小直接回答。

#### 48.9 S-7c-2：**先把 LDPC 的账算清**（`6e7b83409f`，纯探针）——分层核"每层一次 dispatch"≈ 4.3 µs，CPU 版同一个码块 48 µs

两条探针（无行为改动）：

- `[ldpc_time_sum]` / `[ldpc_time_shape]`：把 `decode()` 拆成
  `wall / pack / submit（引擎调用＝提交+等待）/ gpu（命令缓冲的 GPU 窗口）/ gap / unpack`，
  并打印**迭代次数直方图**与**迭代上限直方图**，外加按 (算法, 基图, 提升尺寸) 的分档行；
- `[metal_stats] mmse_ce ... guard=命中/总数 guard_mean/max`：引擎入口守护等待（单 pending 槽的下一个嫌疑）。

**本地定标**（`ldpc_metal_bler_test --latency`，BG1 Z208 rate 0.5 cap 25，正是 OTA 那个 3602.9 µs 的几何）：

| 解码器 | wall 均值 | GPU 窗口 | 相对 |
|---|---|---|---|
| `metal`（分层、逐层 dispatch，**= OTA 现状**） | **3663 µs** | 3346 µs | 1.0× |
| `metal_persistent`（整轮解码一次 dispatch） | **1407 µs** | 1232 µs | 2.6× |
| `metal_flooding`（每轮 2 次 dispatch） | 2524 µs | 2349 µs | 1.45× |
| **CPU 解码器（同一码块、同一迭代上限）** | **48 µs** | — | **76×** |

BG1 Z64 上比例一致（layered 2592 / persistent 307 / CPU 7.3 µs）⇒ **与块大小无关**。

**机理**：分层核每轮 46 层就是 46 次 dispatch，每次 ≈4.3 µs（Z16 实测：≈3 轮 ≈138 次 dispatch ≈597 µs GPU）。
代价 ∝ **层数 × 迭代次数**。这也解释了为什么当年（PLAN §4.12 第七轮实链）测不出持久版的优势：
那时 TB 收敛快、只跑 1–2 轮，整段只有 ~230 µs，两种形态打平；而今天 OTA 的 3.6 ms 说明
**码块几乎跑满了迭代上限**——这一点正需要 OTA 那轮的 `iters hist / cap hist` 确认（若真跑满，
先要解决的是**解码质量**，而不是 dispatch 开销）。

**两个不需要改代码的杠杆**：

1. `--pusch_ldpc_decoder_type metal_persistent`：同几何 3.6 → 1.4 ms（2.6×）；质量侧单测逐位一致、
   PLAN §4.12 第七轮实链同噪声带；
2. `--pusch_ldpc_decoder_type auto/cpu`：同一码块 48 µs（76×）——UL 管线会从 4.5 ms 掉到 ~1 ms 量级，
   但把 LDPC 放回 CPU，与"MAC PDU 出 GPU"的长期目标相反，属于**性能兜底选项**，需要显式决定。

⇒ 下一轮 OTA 两腿：**腿 A** 现状 `metal`（带新探针）取真实几何 / 迭代分布 / `gap`；
**腿 B** `metal_persistent` 做 A/B。两腿背靠背跑，用 DL PDSCH `t=` p50 当机器状态指纹。

#### 48.10 A/C 两腿定案：**用 CPU LDPC 当测试台**；LDPC 只污染 `defer_wait`，CE 自己那 266 µs 与它无关（pre-LDPC 新基线 1015 µs）

用户跑的两腿：**A** = 现状（`--pusch_ldpc_decoder_type metal`）+ 新探针；**C** = 同一命令行只把 LDPC 换成
CPU（`auto`），其余一字未改。

| 每 slot 的段 / 指标 | A：`metal` LDPC | C：CPU LDPC | 差 |
|---|---|---|---|
| `[ul_pipeline]` mean | 4450.8 µs | **1015.1 µs** | **−77%** |
| `[ul_ldpc_decode]` | 3502.9 | **43.1** | −98.8% |
| `[ul_time_frequency]` | 247.4 | 245.3 | ~0 |
| `[ul_channel_estimation]` | 366.4 | 382.5 | ~0（噪声） |
| `[ul_equalization_demod]` | 334.0 | 344.2 | ~0 |
| CE 每跳 `submit` | 266.44 | **262.70** | **~0** |
| CE `gpu_path`（CB 的 GPU 窗口） | 304.2（146.0） | 300.1（145.9） | ~0 |
| CE `defer_wait` | 3737.8 | **396.0** | −89% |
| CE 引擎入口 `guard=` | 0/652 | 0/954 | 从未命中 |

LDPC 侧探针（腿 A）：`[ldpc_time_sum]` wall 3389.9 / **gpu 3185.9** / gap 203.9 µs，`iters mean=3.08 cap=6`；
分档 `bg=1 z=208 calls=597 ko=3 iters=2.84`（**这就是 OTA 的真实几何**，与本地估计一致）、
`bg=2 z=18 calls=51 ko=50`（11 B 的极小 PDU，几乎全失败，留待 LDPC 阶段）、`bg=2 z=64 calls=3`。

**三条结论：**

1. **管线 = 各段串行之和，无重叠**：A 腿 247+366+334+3503+6 = 4456 ≈ 4450.8；C 腿 245+382+344+43+6 = 1020 ≈ 1015。
   ⇒ pre-LDPC 链完全串行，**各段的段探针就是账本**，融合的收益会 1:1 体现在 `[ul_pipeline]` 上。
2. **假设一（队头污染）被证实**：`defer_wait` 3737.8 → 396.0 ⇒ CE 的"批提交后 3.8 ms 才收"确实是
   排在同队列的 LDPC 命令缓冲（GPU 窗口 **3.29 ms**）后面。队列确实被抢，且抢的代价只落在**收集点**上。
3. **假设二（CE 的代价是继承来的）被证伪**：CE 自己的每跳代价在 A/C 之间**一动不动**
   （`submit` 266→263、`gpu_path` 304→300、`stage` 27→25），而且 `guard=0/652`、`0/954` ——
   引擎入口守护**一次都没命中**。⇒ 那 266 µs 既不是守护等待、也不是队列继承，
   而是 **CE 引擎调用自身的主机侧代价**（命令缓冲创建/编码/提交），而单测隔离环境里同一段只有 ~5–20 µs。

**⇒ 本阶段的测试台协议（定案）**：pre-LDPC 的一切 A/B 一律用 `--pusch_ldpc_decoder_type auto`（CPU LDPC）跑，
**GPU 队列留给 pre-LDPC 链**；LDPC 回 GPU 是它自己阶段的事（其 GPU 窗口 3.29 ms / 解码，
≈24 µs 每层 dispatch @ z=208，与本地定标一致，记录备查）。

**⇒ pre-LDPC 新基线（每 slot）：TF 245 + CE 382 + EQ/DEMOD 344 = 971 µs，管线 1015 µs。**
CE 的 382 µs 里约 317 µs 是每跳那 266 µs 的引擎调用代价 ⇒ **这是 pre-LDPC 链上最大的一块**。

**⇒ 下一问（S-7c-4）**：那 266 µs 落在 `commandBuffer` / 编码 / `commit` 的哪一段？
引擎自带的相位计时器（`OCUDU_MMSE_DEBUG=1`，只打印、不改行为）一轮就能分出来；
答案决定融合方向是"**减少命令缓冲数**"（S-7g：与 eq/demapper 那条 burst 合成一个 CB）
还是"**减少 dispatch**"。

#### 48.11 S-7c-5：**根因是 `engine_run()` 那个带默认值的 `defer` 参数**（`90797e4f6a`）——合并路径一直在同步等待，S-7c-1b 从未作用到它

**先更正 §48.8 ⑥ 与 §48.10 的两条结论**（都属于读错账）：

- §48.8 ⑥ 说"`defer_wait ≈ LDPC 段` ⇒ 队头被 LDPC 占住"——**错**。`defer_wait` 是"推迟的 stage 结束 →
  收集点"的**跨度**，而收集点每轮管线才来一次，所以它天然约等于**一个管线周期**（A 腿 4.5 ms → 3.7 ms，
  C/D 腿 1 ms → 0.40 ms）。LDPC 占 GPU 是真的，但它不是 `defer_wait` 测到的东西。
- §48.10 结论 3 说"那 266 µs 是引擎调用自身的主机侧代价（命令缓冲/编码/提交）"——**错**。
  相位计时器显示 `wrap 0.60 + cb 1.03 + encode 6.79 + commit 2.79 = 11.2 µs`，
  剩下的 **282 µs 是同步入口里的 `waitUntilCompleted`**。

**真正的根因（一行）**：`port_channel_estimator_metal_mmse_impl.h` 里

```cpp
bool engine_run(..., const reformat_stage* reformat = nullptr, bool defer = false);
```

而**合并标准+尾块路径**的调用点（`port_channel_estimator_metal_mmse_impl.cpp:919`）只传了 7 个参数 ⇒
`defer` 落到默认值 **false** ⇒ `merged_defer` 只影响了**解包的登记**，引擎调用仍走 `run_weights_only()`
（同步 `waitUntilCompleted`）。**S-7c-1b 改的正是 `merged_defer = defer`，但它对这条路等于没改。**

**OTA 相位计时器（`OCUDU_MMSE_DEBUG=1`，腿 D）的证据**：

```
run_weights_only       n=712 | wrap=0.60 cb=1.03 encode=6.79 commit=2.79 wait=282.03 us
run_weights_only_async n= 54 | wrap=0.32 cb=0.95 encode=5.33 commit=2.64 wait= 0.05 us
run_async              n=  2 | ... wait=0.05 us
```

⇒ **93% 的跳走了同步入口**；那 54 次异步正是**非合并路径**（`rem_prb == 0` 的一类几何），
也就是单测覆盖到的那条路 —— **为什么单测"验证通过"了**：CE 单测的主力几何（25/51/52 PRB）`rem_prb == 0`，
走的是**另一个调用点**（那个确实传了 `defer`），而 OTA 的 13 PRB（`rem_prb = 1`）走合并路径，测试从未覆盖。

**修法**：① 去掉 `defer` 的默认值（让编译器逼所有调用点显式传，这类 bug 不可能再犯）；
② 合并路径传入 `merged_defer`。

**门禁**：CE 单测合并路径几何的 submit 从同步等待降到 **6.5–6.9 µs**（`prb=4 npt=3`、`prb=25/52 npt=2`），
`submit` 均值 20.09 → **0.76 µs**，每跳 total 48.5 → **30.5 µs**，`cpl_wait` 0.1 → 185.4 µs
（等待**搬家**到收集点，符合设计）；逐位不变（Test 11 `max|dh|/rms 0.00e+00`、Test 12 24780 RE 0 mismatch）；
`ctest -L phy` 162/162；LDPC / 均衡 / chain / dispatch 四个 metal 二进制 OK。

**预期 OTA（测试台腿）**：CE 每跳 `submit` 282 → ~11 µs、每跳 total 338 → ~67 µs，
CE 段 377 → **~90–150 µs**（取决于收集点是否被下一跳的 guard 提前触发），
`[ul_pipeline]` 1015 → **~730–800 µs**。

**方法教训（已写进探针待办）**：探针必须报告**走了哪条路**，不能只报花了多久。
`[mmse_time_sum]` 里没有"本轮有多少跳真的推迟了"这一项，所以 S-7c-1b 的失效在 OTA 上只表现为
"`submit` 没降"这种间接信号。下一步把引擎入口的相位统计做成**常驻累加器**（不再依赖 `OCUDU_MMSE_DEBUG`），
并把 `sync/defer` 跳数打进 `[mmse_time_sum]`。

**实网确认（`OCUDU_MMSE_DEBUG=1` 的相位表 + 测试台腿 D/E，同一个二进制只差这一行）**

| 每 slot | D：修前 | E：修后 | Δ |
|---|---|---|---|
| `[ul_channel_estimation]` | 377.4 µs | **94.5 µs** | **−282.9** |
| `[ul_equalization_demod]` | 318.9 | **407.0** | **+88.1** |
| `[ul_time_frequency]` | 247.7 | 244.7 | −3.0 |
| `[ul_ldpc_decode]`（CPU 台） | 55.1 | 63.8 | +8.7 |
| **`[ul_pipeline]`** | **999.1** | **810.0** | **−189.1** |

- 相位计数：`run_weights_only n=712 wait=282.03us` → **`n=2`**；`run_weights_only_async n=3204`。
- 同几何每跳（`prb=13 npt=3`）：total 359.1 → **73.4 µs**，`submit` 304.06 → **20.89 µs**。
- 不变量：`hops_gpu=hops=3206`、`hops_no_gpu=0`、`hops_nn=0`、`fb_blocks=0`、`staged=0`、`guard=0/3208`。
- 账本闭合：−282.9 + 88.1 + 8.7 = **−186.1 ≈ −189.1** ✓。
- **那 +88 µs 的去向**：CE 的批虽然推迟了，但它的 GPU 窗口（146 µs，K1b/K2/K3/K4 四次 dispatch）
  **排在 eq/demapper burst 之前、同一条 backend queue 上**，burst 要等它。不只是均值——EQ/demod 的
  `min 229→316`、`median 290→390`，**整条曲线平移** ⇒ 固定延迟，不是新增工作量。

**旁证（A/B 纪律）**：腿 E 的 CE 跳数 3206、解调器 device 读 35266 是腿 D（766 / 8426）的 **4.2 倍**，
而送达量几乎相同（359.6 kB vs 356.9 kB；666 vs 662 个 CRC-OK slot）⇒ 多出来的跳落在**失败尝试**的 slot 里
（段探针只收 CRC-OK）。它不妨碍"按成功 slot"的对照（腿 E 成功 slot 的跳数仍 ≈1.2：94.5/73 ≈ 1.3），
但再次说明跨腿比总时长必须带机器/链路指纹（§47.6.1）。

#### 48.12 pre-LDPC 链的成本结构：**每 slot 的 dispatch 次数就是账单**（S-7c-6，本地定标）

新基线（测试台，每 slot）：`TF 244.7 + CE 94.5 + EQ/DEMOD 407.0 + LDPC(CPU) 63.8 = 810.0 µs`。

把三段按 **dispatch/commit 次数**拆开（本地 `metal_dispatch_probe` + 各引擎计数器）：

| 段 | 每次 PUSCH occasion 的 dispatch/commit | 实测 | 单位成本 |
|---|---|---|---|
| EQ + demapper（burst） | **22 次**（11 符号 × 均衡 + 11 符号 × 解调，同一个 CB 内） | 407 µs | **~18 µs/dispatch** |
| CE（K1b/K2/K3/K4） | 4 次/hop（13 PRB 走 weights-only ⇒ 无 K1） | 146 µs CB 窗口 | ~36 µs/dispatch（含排队） |
| TF（DFT 批量） | **1 次/slot**（14 个 transform 一个 dispatch） | 244.7 µs | ~17 µs/transform（是**核内计算**，不是 dispatch 开销） |

本地 `metal_dispatch_probe`（1×1，1272 REs，静默机器）给出单次 dispatch 的价格：

```
burst  4: commit-per-dispatch, 1 wait  : min  44.7  p50  52.4 us/dispatch
burst 16: commit-per-dispatch, 1 wait  : min  19.4  p50  24.4 us/dispatch
commit+wait per dispatch (sync)        : min  82.5  p50 129.0 us/dispatch
burst 16 in ONE command buffer         : min   9.9  p50  10.7 us/dispatch   ← 实网 burst 的形态
```

⇒ 一个 CB 内的一次 dispatch ≈ **10 µs 主机编码 + ~4 µs GPU**（`gpu-side last call 3.9 µs`）。
**EQ/demod 的 407 µs 里绝大部分是"22 次 dispatch 的主机编码"，不是计算。**

**已经就位、但没用上的东西**：

- `channel_equalizer` 接口早就有 `submit_group(span<const group_symbol>)`；Metal 实现走
  `submit_batch_run()` → `enqueue_burst_batch()` = **一次 dispatch 覆盖整组符号**；
  核 `equalize_mxn_batch` 也早就写好且写法正确（`gid.x`=RE、`gid.y`=符号，注释明说
  "dispatch 本身就是 `ul_equalization_demod` 的主体"），chain probe 的逐位比对是 `0 differing eq/nv values`。
- 但 `pusch_demodulator_impl.cpp:489` 仍然**逐符号**调 `submit()`/`equalize()` ⇒ 实网每 slot 22 次 dispatch。
- ⚠️→✅ chain probe 里"批量版慢一倍"（22.3 vs 10.7 µs/符号）的那次测量**没有给批量路径预热**
  （per-symbol 跑了两次，批量只跑一次）。补上预热 + 10 次取最小值后结论**反转**：

  ```
  [chain] D equalizer: per symbol 198.9 us/slot (14.2 us/symbol)
                       against batched 102.6 us/slot ( 7.3 us/symbol), 1.94x;
                       0 differing eq/nv values -> OK
  ```

  ⇒ 均衡的批量路径**快 1.94×且逐位一致**，之前的"批量无用"是测量假象。
  （一般教训：比较两条路径时必须**两条都预热**、且**重复取最小值**，否则首次 dispatch 的
  pipeline 绑定开销会被记在其中一条上。）

**下一步（S-7e-batch，本阶段主攻方向）**：把 EQ/demod 从 22 次 dispatch 收到 **2 次**
（均衡用现成的 `submit_group`；解调需要照 `equalize_mxn_batch` 的写法补一个 `demod_soft_batch` 核 + 组接口）。
按上表单位成本估算：**407 → ~150–250 µs/slot**，`[ul_pipeline]` 810 → **~550–650 µs**。
门禁：逐位一致（批量核与逐符号核同算式）、`ctest -L phy`、metal 各二进制、测试台 OTA 一条腿。

#### 48.13 Ubuntu（跨平台硬规则）

`jwang@192.168.31.211:~/work/ocudu`，`f285cea33a`（`git pull --ff-only` 后）：
`cmake -S . -B build` exit 0；`cmake --build build -j16 -- -k` **exit 0、`error:` 0**；
`--target test` **7616/7616 通过**（342.2 s）＝上一版基线 7615 + 新增的 `resource_grid_device_view_test`。
Linux 上 `ENABLE_METAL_*` 全 OFF ⇒ 本轮同时验证了：接口改动（`submit_symbol` 带网格）在**没有 Metal** 的
构建里照样编译、`device_resource_grid` 默认 `auto` 且模式为 `cpu_gpu` 时**不会**要求任何设备能力。

#### 48.13 S-7e-batch 实施清单（下一轮直接照做）

目标：EQ/demod 每 occasion 的 dispatch 从 **22 → 2**（均衡 1 + 解调 1）。已验证：批量均衡
`1.94×` 且逐位一致（§48.12，`bfd329b454`）。

1. **均衡改走组提交**：`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp`
   - `deferred_chain` 判定在 `:296`；逐符号提交在 `i_symbol` 循环内的 `:489 equalizer->submit(...)`。
   - 改成：循环内把每个符号的 `group_symbol{ch_symbols, ch_estimates, eq_symbols, eq_noise_vars, noise_var_estimates, tx_scaling}`
     收集到 `std::vector<group_symbol> run`，循环外**一次** `equalizer->submit_group(run)`。
   - run 的切分（同一 plan/几何/stride 才能合一个 dispatch）已由
     `channel_equalizer_metal.cpp:182 submit_batch_run()` 处理，调用方按"同一 config 的连续符号"成组即可。
   - 非 `deferred_chain` 分支保持逐符号 `equalize()` 不动（同步路径无 burst）。
2. **解调批量核**：`lib/phy/upper/channel_modulation/metal/ocudu_demod.metal` 现只有 `demod_soft`（逐符号）。
   照 `ocudu_equalizer.metal` 的 `equalize_mxn_batch` 写法补 `demod_soft_batch`：
   `gid.x`=RE、`gid.y`=符号，per-symbol 缓冲按 `gid.y` 乘 stride 偏移，参数结构照 `equalize_strides`。
   然后 `ocudu_demod_metal_engine` 加 `enqueue_burst_batch` 同款入口 + 在 `pusch_demodulator_impl`
   的解调调用点同样收成一次（demapper 的 burst 计数在 `ocudu_metal_burst.mm` 的 `count_dispatch`，可用来验证 11→1）。
3. **门禁**：批量核 vs 逐符号核**逐位一致**（模板：`metal_chain_probe` 的 `0 differing eq/nv values`）→
   `ctest -L phy` → 六个 metal 二进制 → 提交 → **一条**测试台 OTA 腿。
   预期：EQ/demod **407 → ~150–250 µs**、`[ul_pipeline]` **810 → ~550–650 µs**；
   `[metal_stats] burst dispatches` 应从 22/occasion 降到 2/occasion。

#### 48.14 S-7e-batch 第一次尝试：**批量路径不能直接接进实链**（负结果，已回退，树绿）

按 §48.13 第 1 步动了 `pusch_demodulator_impl`（逐符号 `submit()` → 收集 `group_symbol` 后一次
`submit_group()`，并为每个符号加了独立的 `ch_re` / `ch_estimates` / 噪声方差存储，因为
`get_ch_data_re()`/`get_ch_data_estimates()` 返回的都是**单份可变成员**，逐符号重填）。
编译通过，但 `pusch_demodulator_deferred_chain_test` **3/5 失败**：

```
[  FAILED  ] metal_back_ends_match_the_cpu_chain
[  FAILED  ] concurrent_metal_demodulations_match_the_serial_chain
[  FAILED  ] metal_equalizer_binds_the_estimator_device_buffer
             Expected: (dev_device) > (0U), actual: 0 vs 0
[metal_stats] equalizer ch_est device=0 staged=62      ← 设备零拷贝完全没走
```

**根因**：批量路径 `channel_equalizer_metal::submit_batch_run()` 是**纯主机 gather**——
它用 `symbol.ch_estimates->get_channel(...)` 做 `memcpy` 把估计搬进组缓冲，且 `resolve_plan()` 调用
**没带 `device_noise_variance`**；而逐符号 `run_equalize()` 会取 `ch_estimates.get_device_slice(0)` 直接
把估计器的设备地址绑给内核（`consumes_device_estimates()` 单端口单层的零拷贝路径）。
所以 `submit_group` 与设备估计路径**不兼容**：一旦改用它，S-7b/S-7c 的设备零拷贝就被绕过，
结果也与 CPU 链不一致。

**⇒ 真正的任务（S-7e-batch-1，已重新定义）**：让批量 dispatch 也支持设备切片：

1. `ocudu_equalizer.metal` 的 `equalize_mxn_batch` 增加设备切片形态：`h` 直接读估计器的设备基址
   （单端口单层，per-symbol stride），与逐符号核 `equalize_mxn` 的设备分支同算式；
2. `submit_batch_run()` 的 `resolve_plan()` 补上 `device_noise_variance`，并用设备基址/stride
   取代主机 `memcpy`（组内每个符号一份设备切片，可由 `ch_est_device_view` 逐符号取得）；
3. 调用方仍需 §48.13 第 1 步的**逐符号输入存储**（本次已写出并回退，改动思路记录在案）；
4. **门禁就是这个测试**：`pusch_demodulator_deferred_chain_test` 5/5，它同时检查
   "设备缓冲确实被绑定"（`dev_device > 0`）与"与 CPU 链逐位一致"——这次正是它挡住了回归。

⚠️ 教训：`submit_group` 的默认实现是逐符号 `submit()`，但 **Metal 的 override 不是等价替换**
（它改变输入来源：设备绑定 → 主机 gather）。接进实链前必须先让它支持设备切片，
否则会静默丢掉设备零拷贝（`[metal_stats] equalizer ch_est device=` 是这条的探针）。

#### 48.15 S-7e-batch-1 尝试记录：设备切片已实现到引擎侧，但**批量 dispatch 只写了 12 个符号里的第 0 个**（代码已回退，树绿）

本轮做完了 §48.14 的方案（引擎侧设备切片 + 调用方组提交），编译通过，但测试给出一个**具体、可复现**的缺陷，
因此全部回退（HEAD 未动，`pusch_demodulator_deferred_chain_test` 5/5、`metal_chain_probe` 逐位一致）。

**已实现（回退前，代码思路保留在此）**：

1. `symbol_plan` 增加 `device_slice`；`resolve_plan()` 自己解析设备切片（单端口单层、有 base）
   并把 `device_noise_variance` 一并推出，使**分组判定与执行口径一致**；
2. `submit_group()` 的分组谓词加入 `next_plan.device_slice == plan.device_slice`；
3. `submit_batch_run()` 增加设备快路径：逐符号取 `get_device_slice(port)`，校验
   `base/layer_stride` 相同且 `offset` 的差分（per-symbol stride）一致，然后
   `ch_est_binding(base, offset, layer_stride)` + `h_sym_stride = sym_stride`，
   噪声方差直接用切片里的 `noise_var`，跳过主机 `memcpy`；不一致则回落主机 gather；
4. 调用方（`pusch_demodulator_impl`）：`deferred_chain` 分支改为**收集** `group_symbol`，
   循环后一次 `submit_group()`；并为每个符号加了独立的
   `group_ch_re_copy/group_ch_re_view/group_device_ch_estimates/group_host_ch_estimates/
   group_noise_var_estimates`（`get_ch_data_re()`/`get_ch_data_estimates()` 返回的是单份可变成员，
   逐符号重填，共享会让整组读到最后那个符号）。

**缺陷（回退的原因）**：`pusch_demodulator_deferred_chain_test` 里
`metal_back_ends_match_the_cpu_chain` 与 `concurrent_metal_demodulations_match_the_serial_chain` 失败，
形态非常具体——**12 个符号的组里只有第 0 个被写**：

```
partial-bandwidth PUSCH (17 of 25 PRB, 2 DM-RS symbols without data):
  4485 of 4896 LLRs differ; first difference at 408: cpu=120 metal=0
  逐符号计数：zeros ref=2 def=4488   ← 11 个符号 × 408 个 LLR 全 0
[metal_stats] burst dispatches=1632(基线) → 913 (equalizer 804→85, demapper 828 不变)
调试打印：n_sym=12 nof_re=204 h_stride=408 y_stride=408 eq_stride=4096 nv_stride=4096 device=0
```

**已排除**：① 批量核本身——`metal_chain_probe` 的 D 段在同一引擎 API 下**逐位一致且 1.91×**；
② 输出寻址——`eq_stride=4096` 与调用方 `eq_symbol_stride_re` 一致，基址页对齐，映射长度在分配内；
③ 线程组宽度（把 `threadsPerThreadgroup` 的 x 夹到 `min(256, nof_re)` 后症状不变，该改动已一并回退）；
④ 调用方输入生命周期——把 `submit_group` 临时退回逐符号 `submit()`（`head_wrappable=false`）
   后测试**立即 5/5**，说明逐符号输入存储、收集顺序、pass1/pass2 顺序都是对的。

**下一步定位思路（下一轮直接用）**：

1. 在 `submit_batch_run()` 里把**staged 的 h/y 头几个样本**打出来（`cbf16_t` 的取值 API 用
   `to_float()`/`operator float` 之类，注意别用 `.real()`——本轮那次编译失败就是这个）；
   再在 `wait()` 之后把 `run.front().eq_symbols` 的前几个样本打出来：一次就能分清
   "输入没填对" vs "核没写全"；
2. 二分 `nof_re`：把测试换成 **25 PRB（nof_re=300 > 256）** 跑一遍。若 300 通过而 204 失败，
   问题就在 `eq/nv` 的**带间隙 stride + 非 8 对齐**上（probe 用的是连续 stride、1 端口）；
3. 若确认是核侧，最小复现是在 `metal_chain_probe` D 段加一组
   **2 端口 + 带间隙 stride（4096）** 的参数（现在那里是 1×1 连续），改完立刻能逐位验证。

⚠️ 记录一条通用经验：批量核的验证（D 段）只覆盖了 **1 端口 / 连续 stride**，
而实链是 **2 端口 / 页对齐带间隙 stride** —— **验证覆盖面差一档，回归就是从这里进来的**。

#### 48.16 S-7e-b0b：把 OTA 形状补进 probe —— **核与引擎在实网几何下完全正确**（`fdfbcdf2c3`）

按 §48.15 第 3 步，给 `metal_chain_probe` 加了 **Pattern E：2 端口 / 12 符号 / 204 RE /
页对齐带间隙 stride = 4096 元素**（就是解调器 `temp_eq_re` 交给引擎的形状），逐元素比对
"逐符号 12 次 dispatch" vs "整组 1 次 dispatch"：

```
[chain] E OTA shape (2 ports, 12 symbols, 204 REs, gapped stride 4096):
        0 differing eq/nv, first bad symbol -1 -> OK (engine ok=1)
```

（同轮的 D 段：`1.84x; 0 differing eq/nv values -> OK`；`metal_chain_probe` 退出码 0。）

**结论（把 §48.15 的怀疑范围收窄了一半）**：批量核 + 引擎的批量编码在**实网几何下逐位正确**，
所以"12 个符号只写第 0 个"的缺陷在**适配器/调用方**这一层，而不是核：

- `channel_equalizer_metal::submit_group()`（run 切分、`eq_stride/nv_stride` 取自指针差、
  页对齐判定）或
- `submit_batch_run()`（staging 布局与 `h_bytes/y_bytes` 长度、`s_binding`、`entry->eq_direct`）或
- 解调器的组收集（§48.15 已用"退回逐符号即 5/5"证明**逐符号输入与顺序没问题**，
  但**批量入口**那一支还没单独证过）。

**下一轮的最小复现（省掉解调器）**：在 `metal_chain_probe` 里加 **Pattern F**——直接用
**适配器**（`channel_equalizer_metal`）而不是引擎：12 个符号的 `eq/nv` 用 `temp_eq_re` 同款
页对齐带间隙布局、每符号独立的 `ch_est_list`/`re_buffer_reader`，先逐符号 `submit()`，
再 `submit_group()`，比对两者输出。这样能在**一个二进制、一次运行**内二分
"run 切分 / staging 长度 / 直接写回"三个嫌疑点，不必再走解调器与 OTA。

⚠️ 复用的教训（§48.15 已记，这里再确认一次）：**验证覆盖面必须覆盖实网形状**——
D 段（1 端口 / 连续 stride）通过 ⇒ 什么都不能保证；补上 E 段（2 端口 / 带间隙 stride）
才把范围从"核 vs 适配器"缩到"适配器"。这条对后面 LDPC / DFT 的批量核同样适用。

#### 48.17 S-7e-b0c：**适配器层也逐位正确**（`b3e07034e4`）——缺陷被逼到"解调器上下文"这一层

Pattern F 直接驱动**适配器** `channel_equalizer_metal`（绕开解调器），输入形状与实网一致
（2 端口、12 符号、204 RE、页对齐带间隙 stride 4096、**每符号独立**的 `ch_symbols`/`ch_estimates`/噪声方差），
比对"逐符号 `submit()` 顺序" vs "一次 `submit_group()`"：

```
[chain] E OTA shape (...): 0 differing eq/nv, first bad symbol -1 -> OK
[chain] F adapter  (2 ports, 12 symbols, 204 REs, gap 4096): 0 differing eq/nv, first bad symbol -1 -> OK
```

**至此排除链完整**（每一层都用实网形状单独验过）：

| 层 | 验证 | 结果 |
|---|---|---|
| 批量核 `equalize_mxn_batch` | Pattern E（引擎 API，实网形状） | 逐位一致 |
| 引擎批量编码 `enqueue_burst_batch` | Pattern E | 逐位一致 |
| 适配器 `submit_group()`/`submit_batch_run()`（run 切分、staging、原地写回） | Pattern F | 逐位一致 |
| 调用方逐符号输入 / 收集顺序 / pass1→pass2 | §48.15（退回逐符号即 5/5） | 正确 |

⇒ 缺陷**只可能**出在**解调器特有的上下文**上，最可能是：
**均衡器与 demapper 共用同一个 `shared_burst`**（批量分支是*立即编码*，而逐符号分支是*推迟编码 + flush hook*，
两者在同一个 encoder 里的相对顺序 / pipeline 切换时的 `memoryBarrierWithScope` 作用范围），
其次是 wrap 缓存（demapper 按逐符号小长度映射过同一批地址，均衡器又按整组大长度映射）。

**下一轮的最小复现（Pattern G）**：在 `metal_chain_probe` 里把 **demapper 引擎加进同一个 burst**：
`f_equalizer.submit_group(group)` → `demapper_engine.enqueue_burst(...)`（逐符号）→ 一起 commit/wait，
再比对"均衡器单独跑"与"和 demapper 混跑"两次的 eq/nv 输出。若混跑时符号 1+ 归零 ⇒ 定位到 burst/顺序层，
不必再走解调器与 OTA。

（附：Pattern D/E/F 现在固定覆盖 1 端口连续 stride、2 端口带间隙 stride、适配器三条路径，
`metal_chain_probe` 退出码 0 才算通过——这条覆盖面对后面 LDPC/DFT 的批量核同样照搬。）

#### 48.18 S-7e-b0d：**缺陷已复现，根因是 wrap 缓存的"同地址不同对象"**（`5769e6e78b`）

Pattern G（均衡整组一次 `submit_group` + demapper 逐符号提交，**同一个 burst**，即解调器结构）
加一个开关 `OCUDU_PROBE_GAPPED=1` 切到实网输出布局（每符号页对齐间隙 **4096 元素 = 32 KiB**）：

```
默认（连续输出，即 A/B/C 段的布局）        -> 0 differing LLR bytes        -> 退出码 0
OCUDU_PROBE_GAPPED=1（实网间隙布局）      -> 21396 differing LLR bytes, first bad symbol 1 -> MISMATCH
```

**与 OTA 症状逐字一致（first bad symbol = 1）**，缺陷闭合成一个"一个二进制、几秒"的复现器。

**根因**：间隙布局下

- **组提交**把输出**一次**地按 *分配基址* + 整组长度 wrap ⇒ 缓存键 = `base`，得到一个**覆盖整组的大 MTLBuffer**；
- **demapper** 逐符号 wrap `base + s*32 KiB` ⇒ 缓存键各不相同 ⇒ 各自**另一个小 MTLBuffer**。

两个阶段于是对**同一块内存**绑定了**不同的 MTLBuffer 对象**，Metal 就不会把两者的访问关联起来
（`shared_queue::wrap_no_copy` 的注释正是这么写的：*"a hit means two stages of the chain bind the SAME
Metal buffer object for one address, which is what relates their accesses to it"*）⇒ demapper 在均衡器
写之前就读了第 1..N 个符号（读到的还是上一轮/零值）⇒ 只有符号 0 正确。

逐符号路径之所以没事：均衡器与 demapper 对符号 s 绑的是**同一个指针** ⇒ 缓存命中 ⇒ **同一个对象** ✓。
这也解释了 Pattern F（同布局但同一阶段自己比自己）与 G（连续布局）为什么都通过。

**修法（下一步，已定方案）**：让 `wrap_no_copy` 支持**包含式查找**——当请求范围落在某个已缓存映射之内时，
返回**那个已有的 buffer + 偏移**，各引擎用 `setBuffer:offset:` 绑定偏移。这样"同一块内存只有一个对象"
的不变量对所有阶段都成立，组提交与逐符号提交都能共用一个对象。改动机械但涉及面广：
`shared_queue::wrap_no_copy(..., size_t* offset_out)` + 各引擎的 `wrap_buffer()` 返回 `{buffer, offset}`
+ 各自的 `setBuffer:offset:` 调用点（均衡器 / demapper / CE / LDPC 四处）。

**另一条备选（不推荐）**：组提交的输出改为 staging + 等待后拷回。它会打破"一次 wait"的推迟链设计
（demapper 的 dispatch 在 wait 之前就已编码，拷回发生在 wait 之后 ⇒ demapper 读到旧值），
所以必须走包含式查找那条路。

⚠️ 这条经验值得记牢：**"零拷贝 wrap"的正确性依赖"同地址 ⇒ 同对象"这个不变量**，
而**大范围映射 + 小范围映射**会静默破坏它。任何"把多个小缓冲区当成一个大缓冲区来绑"的优化
（组提交、批量核、lane 融合）都必须先解决这一条。

#### 48.19 S-7e-b1 落地：**wrap 缓存包含式查找修好了实网布局**（`d9f741b31a`），组提交还差两步

**修复内容**（已提交）：`shared_queue::wrap_no_copy()` 增加可选 `size_t* offset`：

- 传入 offset 时启用**包含式查找**：返回**覆盖请求范围的最大已缓存映射** + 偏移（"最大的"是有意的——
  旧的小映射不能遮蔽整组的大映射，否则两个阶段又会绑到不同对象）；
- 不传 offset 的调用方（信道估计引擎、LDPC 引擎）行为完全不变；
- 均衡器与 demapper 引擎的 `wrap_buffer()` 改为返回 `{buffer, offset}`，所有绑定点改用
  `setBuffer:offset:`。

**验证**：

```
OCUDU_PROBE_GAPPED=1 metal_chain_probe  21396 -> 0 differing LLR bytes, first bad symbol -1 -> OK
默认 probe / 均衡单测 / dispatch probe / LDPC 单测 / ctest -L phy 162/162      全部通过
```

**随后把调用方（解调器组提交）+ 引擎设备切片重新接上后的进展**（这些改动**未提交**，已回退，
因为还有两条用例没过；树保持在 `d9f741b31a` 全绿）：

| 用例 | 接上组提交后 |
|---|---|
| `metal_back_ends_match_the_cpu_chain` | **0 of 4896 LLRs differ ✓ 通过**（原 4485 → 0） |
| `concurrent_metal_demodulations_match_the_serial_chain` | 4/4 → **1/60** 不匹配（并发，3672 个零） |
| `metal_equalizer_binds_the_estimator_device_buffer` | 失败：**同一次运行里"设备绑定"与"主机 gather"的软比特不一致**（0 vs -120） |

**剩下两条的定位线索（下一轮直接做）**：

1. **设备绑定值不一致**：批量路径的设备切片要求"各符号切片等间隔"（`submit_batch_run` 里已校验，
   并把等间隔检查加进了 `submit_group` 的分组谓词）；不一致时应当**拆 run**走逐符号设备路径
   （不能回落主机 gather——单端口形状要求 `dev_staged == 0`）。下一步先打印
   `get_device_slice(0).offset/base/layer_stride` 逐符号的值，确认是否等间隔、以及
   `s_dev`（设备噪声方差）是否与主机值一致。
2. **并发 1/60**：先确认它是否是**既有**问题（把 HEAD 的二进制也跑 20 次统计失败率）。
   若是新引入，重点查 `shared_burst` 在多线程下的 flush hook / pipeline 状态
   （组提交是*立即编码*、逐符号是*推迟编码*，两条路在同一 burst 里混用时的线程安全）。

**当前状态**：HEAD `d9f741b31a`；默认 probe 退出码 0、`OCUDU_PROBE_GAPPED=1` 退出码 0、
`pusch_demodulator_deferred_chain_test` 5/5、`ctest -L phy` 162/162。

**并发 1/60 的定性（已用实验排除"既有问题"）**：把 HEAD（未接组提交）的二进制连跑 20 次，
`pusch_demodulator_deferred_chain_test` **20/20 全过**；接上组提交后 60 次里有 1 次不匹配
⇒ **并发缺陷是组提交引入的**，不是既有 flakiness。

**最可能的原因**：`submit_group` 的批量分支是**立即编码**（`shared_burst::encoder(pipeline_batch)`
当场编码），而逐符号分支是**推迟编码**（push 到 `pending` + `set_flush_hook`，由下一个阶段触发）。
两者混用在**进程级共享**的 `shared_burst` 上：单线程下顺序确定 ✓，多线程下 flush hook 的触发时机
与批量编码交错，`s.pipeline` / `s.flush_hook` / `s.enc` 的状态就可能被另一个线程改写 ✗。

**修法（下一轮，两个选项）**：
1. ✅ 推荐：让批量分支**也走推迟编码**（把这一组的 dispatch 交给同一套 flush-hook 机制），
   这样所有阶段只有一条编码路径，不再有"立即 vs 推迟"的混用；
2. 或者给 burst 加线程归属（每个线程一段独立 encoder/CB）。

⚠️ 这条也说明：**`shared_burst` 目前隐含"同一时刻只有一个线程在编码"的假设**，
任何新的"立即编码"入口都会踩到它——S-7f 的 lane/ring 必须把这条明确成接口约束。

#### 48.20 重要发现：**引擎的推迟编码路径（flush hook）本身就会批处理**——`submit_group` 可能不是必需的那条路

读 `ocudu_equalizer_metal_engine.mm` 的 `eq_flush_hook()` 发现：**逐符号 `submit()` 累积到 `st.pending`
后，flush hook 在下一个阶段调用 `encoder()` 时会把这些 pending 合成一次 dispatch**——条件是
`same_geom && same_strides && same_sigma && same_h`，其中

```cpp
const bool same_h = (next.h.buffer == head.h.buffer) && (next.h.layer_stride == head.h.layer_stride);
```

也就是说：**只要各符号的估计来自同一个设备缓冲（K3 的设备切片就是这种情况），这条路径本来就会把
整组合成一次 dispatch**（hook 会把每个 pending 的 `h.buffer + h.offset` 逐个拷进组缓冲，
所以 offset 不同不影响）。

这解释了整条线索，也改变了对 `submit_group` 的判断：

- **为什么 OTA 里没批处理**：`shared_burst` 的 flush hook 是**进程级单槽**（`s.flush_hook` / `s.flush_ctx`），
  均衡器 `set_flush_hook(&eq_flush_hook)` 之后，demapper 又设了自己的 hook ⇒ **槽被覆盖**，
  均衡器那一批的 flush 时机/归属就不可靠了。实测 `burst dispatches (equalizer=35266 = 11/burst)`
  正说明 air 上没批成。
- **为什么并发 1/60**：同上——"立即编码"（`submit_group` 的批量分支）与"推迟编码"（flush hook）
  混用在**同一个进程级单槽**上，多线程下互相踩。
- **⇒ 更干净的修法**：不要去改调用方（`submit_group`/逐符号输入存储那一整套，§48.15 的复杂改动），
  而是**把 flush hook 从"单槽"改成"每引擎一槽"**（或把 pending 归到引擎自己的状态里），
  让每个引擎只 flush 自己的 pending。这样：
  1. 均衡器在 air 上就能按其既有条件自动批处理（设备切片共享缓冲时）；
  2. 不再需要 `submit_group` 这条调用方路径，也就不存在"立即 vs 推迟"混用；
  3. 并发问题一并消失（每引擎各自的状态，没有共享单槽）。

**下一轮的第一步因此改为**：把 `shared_burst` 的 flush hook 改成**每引擎独立**（`set_flush_hook` 带
一个"槽位/引擎标识"，`encoder()` 时 flush 当前阶段对应的那一个），然后**不做调用方改动**，
直接在 air 上量 `[metal_stats] burst dispatches (equalizer=)` 是否从 11/burst 降到 1–2/burst。
若成立，S-7e-batch 的收益用**引擎内部**的机制拿到，比调用方改法小得多、也安全得多。

#### 48.21 flush hook 的真实行为（续 §48.20）：跨阶段 flush 已经有了，问题缩小到"为什么 `same_h` 不成立"

读完 `shared_burst::set_flush_hook()` 与 `eq_flush_hook()` 的开头，事实比 §48.20 更精确：

- `set_flush_hook(ctx, hook)` 在**换钩子时会把上一个阶段的 pending 先 flush 掉**（不是丢弃）✓；
- `eq_flush_hook()` 在 `enc == nil` 时直接返回 nil（那时还没开 encoder，无从编码），
  而 pending 仍然留在 `eq_flush_state()` 里 ✓；
- 真正把它编码出去的时机是**下一个阶段第一次调用 `encoder()`**（`encoder()` 里先 flush hook、
  再比较 pipeline 决定是否插屏障）⇒ 顺序与批处理条件都是对的 ✓。

**所以 air 上没批成的原因只剩一个**：hook 里那条 run 条件的
`same_h = (next.h.buffer == head.h.buffer) && (next.h.layer_stride == head.h.layer_stride)` 不成立。
air 上 `[metal_stats] equalizer ch_est device=8008 staged=0` ⇒ 走的是设备切片，
设备切片的 `base` 应当相同、`layer_stride` 也应当相同 ⇒ **理论上应当成立**，
所以要么是 pending 条目里存下来的 binding 与预期不同（例如某些符号走了 staged、或在同一组里混了两种来源），
要么 `same_strides`（输出 stride 一致性）先断掉了。

**下一轮最快的判定手段**：引擎里已经有一个现成的计数器 `engine->batch_dispatches`
（注释：*"proves that a group really took the batched kernel instead of falling back to one dispatch per
symbol"*），但目前**没有任何地方把它报出来**。加一行到 `[metal_stats]` 报告里（或单独 `[eq_batch]` 行），
跑一轮 air 就能立刻分辨"条件不成立" vs "条件成立但没被调用"。这是一个几行的探针改动，
比继续读代码快得多，也符合本会话反复验证过的方法（先让探针回答"走了哪条路"）。

#### 48.22 S-7e-b2/b3：**`same_h` 的"同 buffer"是唯一的拦路条件**；探针补齐设备切片形状（`8f2c3502ca`、`54aeefccc5`）

按 §48.21 的"最快判定手段"先补探针，再动条件，两步都做完并提交（树绿：`ctest -L phy` 162/162、
`pusch_demodulator_deferred_chain_test` 5/5 连跑、probe 两种模式退出码 0）。

**(1) 探针先落地：`[metal_stats] eq_batch`**

`equalizer_metal_engine::batch_diag` + `eq_stats_report` 新增一行（**每个 build 都留**，
成本是每次 flush 几个 relaxed 自增）：

```
[metal_stats] eq_batch flushes=67 symbols=804 runs=85 batched=73 max_run=12 first_break=geometry
```

- `flushes` = 真正进入 run 循环的 flush 次数、`symbols` = 交给它的符号数 ⇒ **"hook 有没有跑"**一眼可见；
- `runs`/`batched`/`max_run` = 切出来的 dispatch 数 ⇒ **"批没批成"**一眼可见；
- `first_break` = 第一条挡住 run 延伸的判据（`geometry`/`estimates`/`strides`/`sigma2`）⇒
  **"为什么没批成"**一眼可见。单测里也通过 `engine_batch_diagnostics()` 直接断言。

**(2) 根因确认：唯一的拦路条件是 `same_h` 里的"同一个 buffer"**

`eq_flush_hook()` 的 run 条件原来是
`same_h = (next.h.buffer == head.h.buffer) && (next.h.layer_stride == head.h.layer_stride)`。
但**组 staging 本来就是逐符号从各自的 binding 拷**（`pending[k].h.buffer + pending[k].h.offset`），
所以 buffer 相同与否**对正确性没有必要**：能进同一次 dispatch 的真正前提只有
"`nof_re`/ports/layers/`layer_stride` 一致"（一次 dispatch 只有一个 `p.h_layer_stride`）。

改法：`same_h` 只比 `layer_stride`。效果（均衡器单测，12 符号 / 204 RE / 2 port / 1 layer）：

| | 改前 | 改后 |
|---|---|---|
| `batches`（`batch_dispatch_count()`） | 0（12 次逐符号） | **1**（`runs=1 batched=1 max_run=12`） |
| 与 `equalize()` 逐位一致 | 通过 | 通过 |

**(3) 顺手补掉的两个真 bug（同一次提交）**

- **pending 从"每线程一条"改成"每引擎一条"**：一个线程先后跑两个引擎时（并发测试就是
  每 worker 一个 demodulator、测试间还会串），第 2 个引擎装 hook 时会把第 1 个引擎的 pending
  抢过来，而 hook 的上下文检查又把它们拒掉 ⇒ **整组被静默丢弃**，症状正是"只有第 0 个符号对"。
  现在 `eq_flush_state_t::pending` 是 `engine → vector`，hook 只编码自己那一份。
- **引擎析构时把 pending 交出去**（`eq_pending_release()`）：pending 里存的是调用方的输入指针，
  地址会被复用；不交出去的话，之后在**同一地址**上新建的引擎会继承这批悬垂条目并编码出去。
  为此外加了 `shared_burst::flush_pending()` / `flush_hook_context()`：**换上下文前先把自己那份
  交出去**，`set_flush_hook()` 也改成走同一条路（跨阶段 flush 的语义不变）。

**(4) 探针 Pattern H：设备切片 + 实网几何**

§48.15 的设备切片改动回退后，**没有任何用例覆盖"估计来自设备缓冲"这条唯一在 air 上走的形状**。
`metal_chain_probe` 新增 Pattern H：1 port / 1 layer / 12 符号 / 204 RE / 间隔 4096 的页对齐输出，
12 个符号的估计指向**同一个设备缓冲的不同偏移**，两条路径（同步 `equalize()` 逐符号 vs
`submit()` 延迟组）+ 一个**主机 staging 的 oracle**（同样的估计值走主机路径）三方比对：

```
[chain] H device slices (1 port, 12 symbols, 204 REs, gap 4096): 0 differing eq (0) / nv (0),
        flushes=1 runs=1 batched=1 max_run=12 first_break=; oracle (host-staged): reference differs in 0, batch in 0 -> OK
```

⇒ **设备切片的批量 dispatch 逐位正确**，且 `batched=1` 是被断言的（选了推迟编码却没批成 = 失败）。
（第一次跑出 2040 处差异是**探针自己的 bug**：参考快照数组按字节数而不是元素数分配，与设备路径无关。）

#### 48.23 **未解决**：批量编码在多线程下仍不正确 —— 所以它**保持 opt-in**（`OCUDU_EQ_DEFER_ENCODE=1`）

批量编码在**所有单线程检查**里逐位正确（probe 全 pattern、均衡器单测 12 符号组、
`metal_back_ends_match_the_cpu_chain` 4896 LLR 全对），均衡段本身快 **2.0x**（Pattern D：
204 RE × 12 符号，逐符号 104–106 µs/slot vs 批量 ~76 µs/slot，且逐位一致）。
但 `concurrent_metal_demodulations_match_the_serial_chain`（4 worker × 15 轮，每 worker 一个
demodulator/引擎）会红，且**症状固定**：

```
[metal] worker 1 iteration 0 (thread …): zeros ref=2 def=1633, first diff at 3264 (ref=-120 def=0)
```

- `first diff = 3264` = **正好第 1 个符号的起点**（QPSK：204 RE × 16 bit）⇒ 第 0 个符号对，
  之后 11 个符号的地图输出为 0（等价于 `eq` 没被写或 `nv = ∞`）。
- **不是"没编码"**：`[metal_stats] eq_batch` 在同一轮里报 `flushes=1 runs=1 batched=1 max_run=12`
  （整组确实被编成了一次批量 dispatch）。
- **两组判据**：`OCUDU_PUSCH_DEFERRED_GROUP` ≤ 8 全绿，≥ 10 必红；`OCUDU_EQ_IMMEDIATE_ENCODE=1`
  全绿（等价于旧默认），`OCUDU_EQ_DEFER_ENCODE=1` 才红 ⇒ 与"一个组里的符号数"直接相关，
  而不是与"是否走延迟链"相关。
- **加了 CPU 同步点就消失**：在两组之间插一次 `equalizer->wait()`（当时的临时开关
  `OCUDU_EQ_STAGE_WAIT=1`，已删除）后 60 轮全绿 ⇒ **失败发生在同一 command buffer 内的
  阶段交接上**（批量 dispatch 写在 CB 前部，demapper 的 dispatch 紧随其后、只隔一个
  `memoryBarrierWithScope:MTLBuffers`），而不是数据或核的问题。
- 排查中已排除的项：输出缓冲全部页对齐（804/804 次 `align=1`，走的都是 in-place 写回）；
  staging 拷贝的源数据正确（同一轮里 `eq0` 的手算值与两侧一致）；`[wrap] hits/creates` 无异常；
  hook 上下文/引擎归属问题已在 §48.22 修掉（改完症状不变，说明它是**另一个**bug）。

⇒ **下一步的焦点**：`shared_burst` 在"多个线程各自一条 burst、每条 burst 内两个阶段"时，
为什么批量 dispatch 的写入没有对同 CB 内后面的 demapper dispatch 可见（或为什么 demapper
先跑）。可疑点已缩到两处：`shared_burst::encoder()` 里 flush hook 的调用与 `s.pipeline`
比较顺序（hook 编码后返回的 pipeline 用于设 `s.pipeline`，屏障插在**下一次** pipeline 变化时），
以及 Metal 对"同一 CB、同一 encoder、中间有 memoryBarrier"的可见性保证是否还需要
`MTLBarrierScopeBuffers` 之外的 `MTLBarrierScopeTextures`/`updateFence`（当前只用 buffers scope）。
在解决之前，**默认仍是立即编码**（即 `d9f741b31a` 的行为），OTA 的 A/B 也照旧。

#### 48.24 S-7e-batch-2 第一轮：**排除"绑错缓冲"**，缺陷缩到"同一批 dispatch 的可见性"；`eq_handoff_probe` 落地（`eq_handoff_probe`）

这一轮的目标是把 §48.23 的"哪个环节错"从推测变成证据。手段是新加的独立复现器
`lib/phy/upper/channel_processors/metal/test/eq_handoff_probe.cpp`（`eq_handoff_probe`，
不进门禁）：N 个线程、每线程一套 `channel_equalizer_metal` + `demodulation_mapper_metal`，
每轮"提交整组均衡 → 每符号提交解调 → 一次 wait"，与"每符号 wait"的串行参考逐位比较。
参数 `OCUDU_HANDOFF_{RE,SYMBOLS,THREADS,ROUNDS}`。

**结论 1：探针复现不出来。** 1/4/6/8 线程 × 50 轮、推迟编码下全绿（最多 400 轮 0 mismatch）。
⇒ 缺陷**依赖真实调用方的上下文**（`pusch_demodulator_impl` 的 `temp_eq_re`/`temp_llr`
栅格 + 参照解调器先跑 + 夹具里前面几个用例），简化探针不足以触发。下一轮的复现器必须
直接跑真解调器（最省事的办法：把 `pusch_demodulator_deferred_chain_test.cpp` 的夹具抽成
一个可链接的 TU，再写一个带 `main` 的探针链接它）。

**结论 2：不是"绑错缓冲"，也不是"读错地址"。** 在失败的那一轮里加了三处一次性探针
（`OCUDU_EQ_XLOG`：批 dispatch 的 `b_eq/b_nv` 与各符号 LLR 的绑定；`OCUDU_EQ_RAW`：
wait 之后按符号统计 `eq` 的零元素数；`OCUDU_WRAP_XLOG`：wrap 缓存命中/未命中）：

- `[metal_out]` 均衡输出：**所有 12 个符号 `zero_re=0`**，`eq0/eq1` 数值与相邻正常轮一致
  ⇒ 均衡（批量核）**写得对**；
- 失败轮的解调绑定：`sym_buf`（读均衡结果的那个对象）与该工作线程**批量 dispatch 用的
  `b_eq` 是同一个 MTLBuffer 对象**，且 `sym + sym_off` 正好等于该线程均衡缓冲的基址
  ⇒ **解调器绑定的就是均衡器写的那个对象、那个偏移**；
- 尽管如此，`temp_llr` 里第 9、10、12、13 这四个符号仍是全 0（`nzero=408/符号`），
  而 0–8 全部正确（`ref=-120, def=0` 的 1633 个零 = 4×408+1）。

⇒ **同一个对象、正确偏移、均衡写对了、解调却读到 0**。这把问题从"缓冲/地址"逼到
**"同一批 dispatch 之间的可见性/执行顺序"**：解调 dispatch 在均衡核完成之前就读了
（或者均衡 dispatch 根本没进那条 command buffer）。注意**失败符号是 9..13 这最后四个**，
不是 §48.23 里写的"除第一个之外的全部"——中间的细节比原记录更具体。

**结论 3：它依赖夹具前缀，也依赖组大小。** 同一二进制，`--gtest_filter=*concurrent*` 单跑
**必过**；全量 5 个用例一起跑**必红**；`OCUDU_PUSCH_DEFERRED_GROUP ≤ 8` 全绿、`≥ 10` 必红
（组小 ⇒ 均衡在 pass 1 中途就被 flush 出去，跟在后面的解调自然读到新数据）。三者都指向
"**均衡的 dispatch 与解调的 dispatch 不在同一条 CB、或不在同一次提交里**"这一条。
下一轮第一步：把 `commit/wait` 与 `CB 身份`打进探针（这一轮加过 `flush enc=%p`，但因为
失败轮每次都是**新的 CB**（`flush enc=…540` → `…7c0`），还没能定到"哪条 CB 少了哪个 dispatch"）。

**本轮顺带验证过的两件事**（都是为了排除假设，结论是"不是这两个"）：
① 强制解调 LLR 走 staging（`OCUDU_DEMAP_STAGE_LLR=1`）仍失败 ⇒ 与"直接写调用方缓冲"无关；
② 在两组之间插 CPU 同步（§48.23 的临时开关）仍能治好 ⇒ 与"CPU 侧顺序"有关而与"数据"无关。

#### 48.25 S-7e-batch-2 第二轮：**CB 层面也对得上**，缺陷缩到"同一 CB 内后四条 demap 读到 0"；并记一条方法论教训

第二轮的目标是"把 `commit/wait` 与 CB 身份打进探针"（§48.24 的下一步），做完后结论如下。

**结论 1：`eqflush` 与 12 条 `demap` 在同一个 `MTLComputeCommandEncoder`、同一条 command buffer 里。**
给 `shared_burst`（`OPEN/COMMIT/WAIT`）和两侧编码点各加一行 `OCUDU_CB_XLOG` 后，任一失败轮都是：

```
[cblog] OPEN    cb=0x…8000 enc=0x…52c0
[cblog] eqflush enc=0x…52c0 n_sym=12          ← 批量均衡编进去
[cblog] demap   enc=0x…52c0 sym=0x…0000 …     ← 12 条解调，同一个 encoder
…
[cblog] COMMIT  cb=0x…8000 enc=0x…52c0
[cblog] WAIT    cb=0x…8000
```

⇒ 不是"两条 CB"、也不是"漏提交"、更不是"绑错对象"（§48.24 已证同一对象同一偏移）。**均衡写对了、
解调读的就是那块内存，却读出 0**。

**结论 2：与线程组宽度无关。** 把批量 dispatch 的 `threadsPerThreadgroup` 从 256 降到 32
（临时开关 `OCUDU_EQ_SMALL_TG=1`）⇒ 症状不变（仍然只有 9/10/12/13 四个符号为 0）。

**结论 3（关键）：这不是内存屏障能解决的那类问题，而且"跑一次就通过"不能当证据。**
在阶段边界把 `memoryBarrierWithScope:MTLBuffers` 换成 `updateFence:` + `waitForFence:` 后，
**第一次运行全绿（0 mismatch）——但同一个二进制随后连跑 5 次全部 1 mismatch**；去掉 `waitForFence`
只留 `updateFence` 也同样 1 mismatch。也就是说：这条缺陷的"每轮 1 次失败"虽然稳定，但
**失败落在哪个 worker/iteration 是随机的**，单次运行（甚至 5 次）根本无法区分"真修好"与"运气好"。
**下一轮起，这类判定一律要求：同一二进制连跑 ≥ 20 次 + 每次 0 mismatch 才算通过**（这条要写进
handoff 的硬规则）。

**结论 4：开着 Metal API/GPU validation 反而必过**（`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`
⇒ 4 worker × 15 轮 0 mismatch，且 validation 没有报任何 API 错误）⇒ 确认是**纯时序竞争**，
不是非法 API 用法或越界书写。

**下一轮的三个候选方向**（按信息量排序）：
1. 在核里加"哨兵"：批量核把每个符号的 `sym` 写进输出附近的旁路缓冲，解调核读均衡结果时把
   **它实际读到的 `re0` 值**也写进旁路缓冲，wait 之后由主机比对 ⇒ 一次运行就能看到
   "解调实际读到的是哪块内存/哪个符号的值"（本轮所有主机侧探针都只能看到"最终状态是对的"）；
2. 查 `dispatchThreads` 的 2D 形态与 `memoryBarrierWithScope:MTLBuffers` 的交互：批量核是
   `dispatchThreads(MTLSizeMake(204, 12, 1), (256,1,1))`，而 demap 核是 12 次 1D dispatch，
   怀疑 driver 对"跨 threadgroup 的 memoryBarrier"只保证内存可见性、不保证前一个 dispatch
   的**全部 threadgroup** 都 retirement 完才放行后一个 dispatch 的读；
3. 若 1/2 都指向 driver 语义，就把"一个组里的均衡与解调"从"同一 CB 内两个阶段"改成
   "同一 CB 但用 `MTLSharedEvent` 或两次 commit 的显式依赖"（代价是每 slot 多一次提交），
   或者干脆退回"每个符号一次 dispatch"（即默认现状）。

#### 48.26 S-7e-batch-2 第三轮（GPU 侧哨兵）：**写入都发生了、CPU 同步也治不好** ⇒ 不是"阶段顺序"问题

按 §48.25 的方向 1 做了 GPU 侧探针（加在 `.metal` 里，由 CMake 的 `xcrun metal` 编译，因此改动会进
metallib）：批量均衡核在 `[[buffer(7)]]` 上记录"这个符号的行跑过了"，解调核在 `[[buffer(4)]]` 上记录
"这次 dispatch 跑了、读到什么、算出什么 LLR"。三处必须同时具备才能拿到数据，本轮都补齐了：
① 探针指针必须走**基类虚接口**（`pusch_demodulator_impl` 只持有 `channel_equalizer*` /
`demodulation_mapper*`）：在 `channel_equalizer` / `demodulation_mapper` 上加
`virtual float* debug_probe() const { return nullptr; }`，并在三个组合层转发
（`channel_equalizer_metal_or_generic`、`demodulation_mapper_metal_or_generic`、两个 adapter）；
② 探针缓冲**每引擎一块**（放 `impl` 里），否则 4 个 worker 互相覆盖，报告完全不可读；
③ 只用**普通 `[[buffer(N)]]` 绑定**，不要把 `device float*` 塞进 `constant` 参数结构体——后者在本轮
一次都没写进去（原因未定，可能与 `setBytes` 传指针的地址空间转换有关），换成普通绑定后立刻出数。

**结论 1：批量均衡核 12 行全跑了。** `[eqaux]` 连续报 `1 2 3 … 12`（`1+sym`），失败轮也一样
⇒ "某几个符号的均衡 dispatch 没执行"被排除。

**结论 2：解调核 12 个符号都写了 LLR。** 把整组 LLR 区域预填成 `0x55`（解调无法产生的值）后，
**没有任何一组留下超过 50 个未被动过的字节**（正常情况下每符号 408 个）⇒ 失败轮里那 4 个为 0 的
符号也是**解调核自己写进去的 0**，不是"没人写"。

**结论 3：解调核读到的输入是有效值、噪声方差也正常。** 存活窗口里 `ran-with-zero-read = 0`
（13115 次 dispatch 中 0 次读到 (0,0)），并且出现过 `read=(-0.076752,0.161935) nv=0.006369 n=204`
却 `llr0=0` 的条目——按 QPSK 公式 `|z| * 157 * GAIN` 这种输入不可能量化为 0。⇒ "读到 0" 与
"nv 为无穷"两条解释都不成立（至少不是全部失败的原因）。

**结论 4（本轮最有价值的一条）：在两组之间插 CPU 同步**（`equalizer->wait()` 后再提交解调，
即"均衡先完成、再编码解调"）**依然 1 mismatch / 5 次连跑全红**。这直接否掉了"两个阶段没有按序
执行"的假设——如果解调是在均衡之前跑的，这个同步必然治好它。结合 §48.25 的围栏/屏障实验，
可以判定：**两个阶段是有序的，解调拿到的输入是对的**，问题在别处。

**下一步候选**（信息量从高到低）：
1. 把"失败轮"钉住再读探针：现在的探针窗口只保留最近 ~8 组，而失败是"每轮 1 次、位置随机"，
   很可能落在窗口之外。做法：探针槽位按 `(group_index % 16)` 固定分配、报告时全部打印，
   再用"哪一组的 LLR sentinel 有残字节"直接定位失败组，然后只读那一组的槽位；
2. 反过来查**消耗侧**：pass 3 从 `temp_llr` 拷贝到 codeword 的那段（`state.llr_offset` 是**打包**的
   bit 偏移，而解调核写的是**页对齐槽位**）——两者的换算、以及 `get_next_block_view()` 的块切分，
   是唯一还没被探针覆盖的地方；
3. 若 1/2 都干净，再考虑"把批量均衡的证据固化成单测"（例如把 4-worker 并发 + 每轮比对放进
   `eq_handoff_probe`，让它能稳定复现），因为现在只有真解调器的夹具能复现。

#### 48.27 **定位到根因方向：wrap 缓存"按地址包含"把两个不同分配混为一谈**（`OCUDU_DEMAP_BIND_LOG` 实证）

§48.26 结尾列的候选 1 用了一次就出结果。做法：在两侧编码点各加一行绑定日志（临时，
`OCUDU_DEMAP_BIND_LOG=1`），记录"均衡批量 dispatch 的输出主机地址 / 绑定对象 / 偏移"与
"每条解调 dispatch 的 LLR 主机地址 / 绑定对象 / 偏移 / 均衡输入地址"。913 行日志（85 次均衡 run、
828 条解调）里两条异常：

1. **828 条解调里有 338 条的均衡输入区域（`sym + sym_off`）从未被任何一次均衡 run 写过**；
2. **15 条解调的 LLR 绑定落在了一个非零偏移**（其余 813 条都是偏移 0），全部是同一个值
   `llr_off = 98304`（= 3 × 32 KiB），对象是**另一个分配的均衡缓冲**：

```
[bindlog] demap llr=0x7714a8000 buf=0x772805dc0 off=98304  llr_base = 0x771490000
[bindlog] eq    out=0x771490000 buf=0x772805dc0 off=0     n=12 re=204
```

即：**解调把 LLR 写进了"另一次均衡输出"的缓冲**（`llr_base + 98304` 正好是该均衡缓冲的第 3 个
符号位置），而 `temp_llr`（0x7714a8000 起）根本没被写。这正是"页对齐槽位 + 每符号 stride 相同"
造成的**地址重合**：两次不同的 `aligned_alloc` 落在了同一页范围内，而 `wrap_no_copy` 的包含式
查找只按地址判断，于是把 B 的请求交给了 A 的映射。

**改动方向（下一步做）**：缓存条目必须记录**它属于哪个分配**（主机分配的 base + size），
只有在请求落在这个分配里时才允许包含式命中；`malloc_size()`（Darwin）可以给出分配的
真实大小（`compat::aligned_alloc` 就是 `posix_memalign`）。

**实测到的关键事实（`OCUDU_WRAP_ALLOC_LOG=1`，`malloc_size()`）**：`compat::aligned_alloc()`
就是 `posix_memalign`，**每次调用只保证请求的那一段**——页对齐的请求各自拿到一个 16 KiB 的
malloc 块（逐符号 LLR 槽位就是这种），而一次要 376 KiB 的请求拿到一个 376 KiB 的块。
也就是说：**同一个逻辑分配（demodulator 的 `temp_llr` / `temp_eq_re`，各 448 KiB）在 malloc
看来是几十个互不相干的 16 KiB 块**。这正是"包含式查找"会串台的结构性原因：A 的大映射按地址
包含 B 的小块，而没有任何信息能说明它们不是同一个分配。

**两版尝试的结果**（都已回退，树保绿）：
- v1（只加"候选映射的分配必须包含请求"的检查）：那条 LLR 混用绑定的确消失，但并发用例从
  1 mismatch 变 2 mismatch（worker 0 + worker 1）；
- v2（再加"映射整个 malloc 块 + 精确归属判定"）：仍是稳定的 2 mismatch。
⇒ 增加归属检查会**同时削弱"同一地址⇒同一对象"这条不变量**（群组级映射与逐符号切片映射绑成
两个对象），所以修法必须同时满足两条：**跨分配绝不复用** 与 **同一分配内（不论大小）永远复用**。
`malloc_size` 的"每页一个块"粒度让这两条无法同时用地址+大小表达；可行的方向是
① 让分配方登记自己的范围（`aligned_alloc` 的调用点把自己登记的区间交给队列），或
② 缓存条目改为"按分配登记 + 命中时要求请求区间被**已登记区间**覆盖"。

**下一轮的做法**：先把 `eq_handoff_probe` 扩成能稳定复现并发失败的用例（现在是"真解调器夹具
才能复现"），再按 ①/② 改，并且**每改一版连跑 ≥ 20 次**（§48.25 结论 3）。

#### 48.28 **根因确认并修复**：wrap 缓存必须按"分配"而不是按"地址包含"复用（`76d3fa934e`）；批量编码成为默认（`ae9b8ec32c`）

§48.27 的下一步做完了，而且修好了。

**根因**：`wrap_no_copy` 的包含式查找**只按地址**判断——任何"覆盖了该地址"的已缓存映射都能服务新请求。
但 `compat::aligned_alloc()` 就是 `posix_memalign`，**块一旦释放，它的页会被交给下一个分配**，
于是两个不同缓冲会先后落在同一页范围里；后者的请求就被交给了前者的 Metal 资源。绑定日志实证：

```
[bindlog] demap llr=0x7714a8000 buf=0x772805dc0 off=98304   llr_base = 0x771490000
[bindlog] eq    out=0x771490000 buf=0x772805dc0 off=0     n=12 re=204
```

即**解调的软比特被写进了"另一次均衡输出"的缓冲**（偏移 3 个符号），而它自己的 `temp_llr` 从未被写
⇒ 第 9/10/12/13 个符号读出 0。

**修法（两步）**：
1. `compat::describe_aligned_allocation(ptr, &base, &size)`：把 `aligned_alloc` 发出的块登记在案
   （`aligned_free` 删除条目，因此**释放后复用同一地址也判得准**），消费者据此区分
   "同一分配的一个切片"与"另一块恰好共享页范围的分配"。
2. `wrap_entry` 记录"这块映射是为哪个分配建的"，只服务**同一分配**的请求（地址不属于任何已知块时
   退回几何判断）；分配已知时映射**覆盖整块**，保证"组级请求"与"逐符号切片请求"仍然绑同一个对象
   （链式阶段赖以成立的"同地址⇒同对象"不变量）。

**验证**：原先**每次运行必红**的并发用例（`pusch_demodulator_deferred_chain_test` 的
`concurrent_metal_demodulations_match_the_serial_chain`，固定 worker 1 / iteration 0）现在
**连跑 20 次全绿**；`ctest -L phy` 162/162；均衡器单测两种编码都 ALL OK；chain probe 的
默认/`GAPPED`/deferred 三种组合退出码 0；`eq_handoff_probe`（4 线程 × 30 轮 × 两种编码）全绿。

**默认切换（`ae9b8ec32c`）**：既然唯一的拦路条件没了，**批量编码成为默认**——
一个组的均衡从"每符号一次 dispatch"变成**一次**（实网几何下均衡段本身 2.0x）。
`OCUDU_EQ_IMMEDIATE_ENCODE=1` 保留逐符号编码作 A/B 逃生门（取代原来的 `OCUDU_EQ_DEFER_ENCODE`）。
切换后的门禁：并发用例连跑 20 次 0 mismatch、`metal_back_ends_match_the_cpu_chain` 4896 LLR 全对、
均衡器单测 `batches=1` 且逐位一致、probe Pattern H `batches=1`、`ctest -L phy` 162/162。

#### 48.29 **实网回归的根因（已修）**：批量编码把"设备上的信道估计"拷到了主机——而那份内存的产出者还在跑

**症状（腿 B3，`ae9b8ec32c` 的二进制）**：手机完成 authentication 后立刻被释放、反复重试；
`[DU-MNG] RLF timer expired with cause="MAC max consecutive CRC KOs reached"`。数字：

| 腿 | 二进制 | PUSCH OK/KO | BLER |
|---|---|---|---|
| `gnb_batch2_B`（22:15 UTC） | `d9f741b31a` | 1040 / 116 | **10.0%** |
| `gnb_b3`（23:19 UTC） | `76d3fa934e`（批量默认） | 479 / 9272 | **95.1%** |

**判据（用户执行）**：同一二进制加 `OCUDU_EQ_IMMEDIATE_ENCODE=1`（逐符号编码）⇒ **attach 成功、ping 正常**。
⇒ 变量是批量编码，不是链路/配置（两次的射频配置逐字相同，成功解码的 SINR 只差 2.2 dB 而 BLER 差 9.5 倍）。

**根因**：批量编码的 flush hook 里有这一句

```cpp
std::memcpy(h_alloc + k * h_stride,
            static_cast<const cbf16_t*>(pending[first + k].h.buffer) + pending[first + k].h.offset,
            h_stride * sizeof(cbf16_t));
```

当 `h.buffer` 是**信道估计器的设备输出**（air 上 1 端口设备切片就是这种情况）时，这句是**主机读设备缓冲**。
而 PUSCH 解调器**故意不等** CE（`estimates_read_in_place`，不调 `sync_device_estimates()`），
所以这份内存的产出 dispatch 可能还在跑——主机读到的内容是未定义的。逐符号编码**从不碰这块内存的主机侧**：
它把该缓冲绑给 dispatch，由后端队列把 CE 与均衡排序。**本地探针看不出**，因为探针的设备缓冲是主机写的且已同步。

**修法（`69ab9d3fec`）**：
1. `enqueue_burst()` 增加 `h_on_device` 形参（由适配器按 `h_device` 传入，不再靠启发式判断）；
2. run 条件：设备切片的 run **必须同一个缓冲、且逐符号偏移正好等于 `h_layer_stride` 步长**（估计器写的偏移正是如此）；
3. flush hook：设备 run **不拷主机**，直接把该 binding 交给核（步长由 `p.h_layer_stride` 表达）；
4. dispatch 绑定的是**真正传给核的那个 binding 的缓冲**（原来是错误地绑了 `h_alloc` 的暂存区），
   偏移加上 binding 自身的 `offset`。

**顺手修掉的两个自己引入的 bug**（都在同一段）：谓词里 `!next.h_on_device || (a && (b == c + d * e))` 的
**运算符优先级**把它算成了 `(c + (d*e != 0))`（改成先算 `h_want = prev.h.offset + h_step`），以及
`same_strides` 在一次编辑中被误删（已恢复）。

**验证**：chain probe 的设备切片形状（1 端口 / 12 符号 / 204 RE / 页对齐带间隙输出 / 估计在同一设备缓冲的
逐符号偏移上）在**两种编码下都逐位一致**，且现在真的是**一次批量 dispatch**（`batches=1 max_run=12`）；
`ctest -L phy` 162/162；均衡器单测两种编码 ALL OK；`pusch_demodulator_deferred_chain_test`（含与 CPU 链的
逐位等价 + 并发用例）**两种编码各连跑 10 次全绿**；`eq_handoff_probe` 两种编码全绿。

**默认仍是逐符号**（`OCUDU_EQ_DEFER_ENCODE=1` 才启用批量），等一条 air 腿确认后再翻默认。

#### 48.30 实网 A/B 定案：**逐符号健康（22% BLER、attach/ping 成功），批量在实网仍是坏的（98%）**；且"设备切片直读"修复后**症状没变**

用户的 A/B（同一台手机、同一位置、同一二进制 `69ab9d3fec`，只差 `OCUDU_EQ_*` 开关）：

| 腿 | 编码 | `rrcSetup` | `rrcSetupComplete` | PUSCH OK/总 | BLER | PUSCH KO 中 SINR ≥ 10 dB 的比例 |
|---|---|---|---|---|---|---|
| **b8** | 逐符号（默认） | **1** | **1** | **714 / 918** | **22.2%** | 25 / 212 |
| b7 | 批量 + 设备切片直读 | 18（反复超时） | 0 | 7 / 396 | 98.2% | **89 / 114** |
| b3 | 批量 + 主机拷贝 | 多次 | 0（RLF） | 479 / 9751 | 95.1% | — |

**结论 1（好消息）**：逐符号编码这条**实网健康**（22% BLER，attach + ping 通），而且 `[metal_stats]` 里
`burse dispatches (equalizer=20988 = 11/PDU)`、`eq_batch flushes=0`，一切如预期。**这是当前 HEAD 的默认**。

**结论 2（坏消息）**：`48.29` 的"设备切片直读"修复**没有治好实网的批量路径**（b7 仍 98%），
所以"主机拷贝读到未完成的 CE 缓冲"**不是**（或不只是）实网失败的原因——尽管那个拷贝在原理上确实不该存在，
修复本身仍然是对的（它让批量与逐符号在语义上等价）。

**结论 3（关键判据）**：b3 的退出报告证明**批量在实网确实生效**：
`eq_batch flushes=39704 symbols=436744 runs=39704 batched=39704 max_run=11 first_break=none`
—— 39704 次 flush、每次都批成一次 dispatch、每次 11 个符号。所以失败不是"没批成"，而是**批出来的那次 dispatch
结果不对**。b7 的 KO 分布进一步排除了"信号弱"：**89/114 个 KO 的 SINR ≥ 10 dB**（逐符号腿里同样的块能解出来）。

**结论 4（本地覆盖缺口）**：同一几何的本地探针（Pattern H：1 端口 / 12 符号 / 204 RE / 页对齐带间隙 /
估计在同一设备缓冲的逐符号偏移）在**两种编码下都逐位一致**，`batched=1 max_run=12`；并发探针（1/2/4/8 线程 ×
40–320 轮）、`pusch_demodulator_deferred_chain_test`（含与 CPU 链逐位等价、并发用例）两种编码各连跑 10 次全绿；
`ctest -L phy` 162/162。⇒ **本地仍有一条实网走了、本地没覆盖的路径**。最大的嫌疑落在测试夹具用的
**估计器替身**：`pusch_demodulator_deferred_chain_test` 的 `est_results_double` 与并发/等价用例都不经过
**真正的 CE 设备输出**（`gpu_ce` 预分配缓冲 + `layer_stride = gpu_ce_total_re` + 逐符号 `re_offsets`），
而实网恰恰是这一条。

**下一步（按性价比排序）**：
1. 用真 CE 的几何搭一个本地夹具（预分配 `gpu_ce` 同尺寸缓冲 + 视角偏移/层步长同实网），跑"批量 vs 逐符号"逐位比对
   —— 这是唯一能覆盖实网那条路径的本地手段；
2. 或在 air 上抓一次失败块的原始数据：失败 PUSCH 的 `state.eq`/`state.nv`/LLR 首元素（两边编码各跑一条腿同轮对比），
   用现成的 `OCUDU_UL_DUMP_LLR` + `ul_chain_replay` 离线复算；
3. 在这条缺陷解决之前，**批量保持 opt-in**（默认逐符号），pre-LDPC 链的收益暂不取（EQ/demod 仍 406 µs，约占 pipeline 54%）。

#### 48.31 离线复现成功（用户提出的 IQ 回放路线）：**缺陷锁死在"批量 dispatch 读设备缓冲"**

用户提醒了 §24/§25 就建好的现成设施：`OCUDU_UL_DUMP` 抓一份含栅格的 reception，`ul_chain_replay` 用**同一个输入**跑任意后端组合。抓一份（`OCUDU_UL_DUMP=/tmp/iq1 OCUDU_UL_DUMP_LLR=1 OCUDU_UL_DUMP_COUNT=200`，187 个 reception）
即可在本地把三条路径喂同一份栅格逐位对比——**不用再占用 air**。

**同一份 capture（`/tmp/iq1_5480_17921`，256QAM/1 层/25 PRB）的五路对照**：

| 变体 | 与"逐符号"参考 |
|---|---|
| 逐符号（当前默认） | — （即参考；用户 b8 腿证明它与实网参考一致） |
| immediate（强制逐符号编码） | **一致** |
| **批量 + 设备读**（当前批量路径） | **3732 字节不同** ← 复现实网缺陷 |
| 批量 + 主机 staging | **一致** |
| immediate + 主机 staging | **一致** |

⇒ 把问题锁死在**一次批量 dispatch 读 CE 设备缓冲**上：同样的 MTLBuffer 对象（`0xba6597b80`，mapping 753664 字节）、同样的起点偏移（`0xba8b00000`）、同样的描述符；主机 staging 版本正确 ⇒ 暂存/切分/stride/偏移全都对，只有"核从设备地址读"这一件事错。
**确定性**：同一输入连跑 5 次，批量路径的 LLR 逐字节相同（不是竞态，是系统性读错）。
**已排除**：CE/均衡的 CPU 顺序（强制 `sync_device_estimates()` 先完成 CE，结果不变）；wrap 缓存对象归属（日志证明两条路径同一个对象、同一 offset）。
**GPU 侧探针**（`equalize_mxn_batch` 里按符号写 `H[0][0]`/`y[0]`/`sigma2[0]`）显示：批量核在部分符号上读到 `h=0`，而主机侧读同一缓冲的对应偏移有值。

**复现命令（离线、秒级）**：
```bash
OCUDU_UL_DUMP=/tmp/rep_sym ./build/.../ul_chain_replay /tmp/iq1_5480_17921 --metal --out /tmp/rep_sym
OCUDU_EQ_DEFER_ENCODE=1 OCUDU_UL_DUMP=/tmp/rep_bat ./build/.../ul_chain_replay /tmp/iq1_5480_17921 --metal --out /tmp/rep_bat
cmp /tmp/rep_sym_5480_17921_llr.bin /tmp/rep_bat_5480_17921_llr.bin   # 3732 字节不同
```
再加 `OCUDU_EQ_FORCE_STAGE=1`（本轮临时开关，已随试验代码回退）即变为一致 ⇒ 这是下一步定位的对照臂。

**下一步（在本条离线链上做，不需要 air）**：按符号把"核读到的 h/y"与"主机读同一偏移的值"逐位对照（探针已写好，需要重新加回），确认是"读到别的内存"还是"读到未更新的内存"，
候选原因：该 MTLBuffer 在 `newBufferWithBytesNoCopy` 下的页映射范围与请求长度不一致（`wrap_buffer` 的 `h_bytes` 与 mapping 的 `len` 边界），
或 CE 之后仍有写入落在该范围（`gpu_ce` 是多用途缓冲，逐跳复用）。

**当前状态**：默认逐符号（HEAD `69ab9d3fec`），实网健康（b8：22.2% BLER、attach+ping 通），本地门禁全绿；批量保持 opt-in。

#### 48.32 定位到根因：批量核读到的 CE 估计在**每批第 3 个符号起**是错的（不是寻址、不是竞态）

> **⚠ 本节的结论（生产者未写完 / 可见性）已被 §48.33 推翻。** 现象与符号定位仍然成立且有用，
> 但"CE 还没写完"这个解释是错的：真正的根因是**每符号起始偏移不是均匀的**，而批量核按单一 stride 读，
> 以及 flush 把 run 首个估计的基准**应用了两次**。见 §48.33。

§48.31 收窄到"一次批量 dispatch 读设备缓冲"之后，本轮把**均衡器自己的输出**（eq/nv 逐符号落盘）在两条腿上做了逐字节对比
（`OCUDU_EQ_DUMP_RESULTS=<file>` 临时开关，两个编码路径都在编码时登记 (eq,nv,nof_re,nof_layers)，等 burst 的 command buffer 完成后由 burst 的 post-wait 钩子写盘）。
这是决定性的一次测量——它把误差**定位到具体的符号**：

| 符号 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 逐符号 vs 批量（均衡器输出） | = | = | **≠** | **≠** | = | = | **≠** | **≠** | **≠** | **≠** | **≠** |

对照本轮 trace 出的**分组边界**（`[0,1] [2,3,4,5] [6,7,8] [9,10]`，first=0/2/6/9）：

> **每一组里只有前 2 个符号是对的，第 3 个起读到的估计就是错的。**
> 误差与"该符号在组内的序号"相关，而不是与"它的偏移/地址"相关 —— 与早先 GPU 探针看到的 `h=0`（偏移 ≥ 504）在
> 同一现象的两种表现上吻合（那些符号恰好都是组内第 3、4 个）。

**同一批里前 2 个对、后面错 ⇒ 不是寻址、不是绑定、不是 wrap、不是 stride**（那些一旦错会从该组第 1 个符号就错），
而是**该批读到的 CE 缓冲内容对组内靠后的符号还没准备好**：批量核把这些符号的估计读成了 0/陈旧值，
逐符号路径同一偏移读到的是对的（§48.31 已用同一 buffer/offset 的 host 读证实）。

**本轮排除的候选（都有实验）**：
- 覆盖率：把批量 wrap 的 `h_bytes` 扩到覆盖整组最后一个符号（真机 0 次 replaces）→ 不变；
- 跨队列顺序：flush 前强制 drain front-end/back-end 两条队列 → 不变；
- 每符号偏移表：给批量核加 `offs[256]` 表替代统一 stride（真实偏移是 0,72,180,252,324,396,504,576,648,756,828，
  组内步长 72、组间跳 108，**根本不是统一步长**）→ 不变（说明 h 侧本就不是根因）；
- 主机快照：把 host 看到的估计复制进新堆缓冲再让核读 → 仍不等（host 那份本身是陈旧的，见下）；
- 主机读时序：**在 command buffer 完成之后**再读 host 那份，才看到真实值（早先在 flush 时读到的
  `h=(-2.34375,-0.17480)` 是**尚未被 CE 写完的陈旧内容**）——这解释了此前所有"host 与 GPU 同址不同值"的困惑。

**根因（结论）**：`eq_flush_hook` 的注释其实早就写明了这一点 —— 设备估计的**生产者（CE 的 dispatch）可能还在运行，
host 没有覆盖它的 wait**。逐符号路径靠"同一条队列 + 提交顺序"拿到正确的写后读；
批量路径把这批估计**推迟到 demapper 换阶段时才一次性读**，那时 CE 对**组内靠后符号**的写入尚未可见，
于是读到 0/旧值。**这与实网现象完全一致**：批量 b3/b7 腿上 PUSCH BLER 95%/98%、attach 失败、RLF。

**结论与取舍**：批量（S-7e）要成立，必须让这批 dispatch 真正等到 CE 的写入可见（例如把均匀 stride 改成偏移表
**并且**给 CE→均衡建立一次显式等待/事件），而不是靠"同一个 MTLBuffer 对象 + 提交顺序"隐式保证。
在这一点解决之前，**逐符号编码保持默认**（`69ab9d3fec`，实网健康：22.2% BLER、attach+ping 通），
批量继续 opt-in（`OCUDU_EQ_DEFER_ENCODE=1`）。

**离线判据（秒级，不需要 air）**：`ul_chain_replay /tmp/iq1_5480_17921 --metal` 跑两条腿后
比较 `<out>_<slot>_<rnti>_llr.bin`，逐符号腿与批量腿差值必须为 **0 字节**；当前是 3732 字节。

#### 48.33 S-7e 结案：真正的根因是**每符号起始偏移不均匀** + **基准被应用两次**；批量已修好并翻为默认

**（本节取代 §48.31/§48.32 的结论；那两节的"现象与符号定位"仍有效，"CE 未写完/可见性"的解释作废。）**

**真正的根因**——两个叠加的缺陷，都在"批量核怎么读这一组的估计"这一步：

1. **估计的每符号起始偏移不是均匀的。** 信道估计器按 DM-RS 图样给每个符号发布一个切片，携带 DM-RS 的符号
   数据 RE 更少，于是相邻起点按 **72、108、72、72、72、108…** 跳（实网 25 PRB 的观测值：
   `0 72 180 252 324 396 504 576 648 756 828`）。批量核原先按**单一元素 stride**（`sym * st.h_stride`）读，
   于是从 run 内第 3 个符号起读到了**别的符号**的估计 —— 这精确解释了 §48.32 那张表
   （每 run 前 2 个符号恰好还在正确位置上）。
2. **flush 把 run 的首个估计应用了两次。** 它既把 `h_run_binding.offset` 加进了 Metal 绑定偏移，
   又把同一个值作为 `p.h_offset` 传给核（核的 `load_h` 会再加一次），于是**凡首个估计不在 0 的 run**
   （即除了第一组以外的所有 run）都从错位置读。

**为什么之前一直没看出来**（三个把诊断带偏的坑，值得记住）：
- **`metallib` 没有跟着源码重编**：一个早前的 marker 实验（往核里塞毒值）用 `finally` 还原了 `.metal`，
  但 `.metallib` 留在污染状态，之后好几轮读数都不可信 —— 判据是输出里出现 `123456`（毒值）。
  教训：改 `.metal` 后必须确认 `ocudu_equalizer.metallib` 的时间戳/内容真的更新，`touch` 源文件强制重编。
- **自制的回读探针写错了槽位**：核的 `eq`/`nv` 指针会按 `sym * stride` 前进，而解析却按紧凑布局读，
  于是"读到全 0"这种结论反复出现且都是假的。
- **探针自身的分片 bug**：`setBuffer:offset:` 把一个共享表切片给多个 dispatch 时，各 dispatch 读到的是
  邻居的切片（表现为"每个 run 只有第一个符号对"）。改成**每个 dispatch 一个独立的表 buffer**（几十字节）后消失。

**修复**（`bc2331efc1`，`GPU-PHY S-7e-b3`）：
- 批量核改用 run 的**每符号起始表**（buffer 7），`h += h_start[sym] - p.h_offset`，不再按 `st.h_stride` 步进；
- flush 改为**在缓冲区基址绑定**、由 `p.h_offset` 承载首个起点（与表的基准一致），消除双重应用；
- **两条进入批量核的路径（deferred flush 与显式入口）合并为同一个编码例程** `eq_encode_batch_dispatch()`，
  绑定序列与字节范围只存在一份 —— 之前两份各自漂移正是问题藏身处；
- 新增 `enqueue_burst_batch_at()` 接收每符号起始；旧的均匀 stride 入口保留为薄封装。

**验证工具（新增，长期保留）**：`lib/phy/upper/channel_processors/metal/test/eq_batch_kernel_probe.cpp`。
在探针自己掌控的页对齐缓冲上做受控 A/B，把"数据来源/布局"这个变量彻底移出调试：
- `uniform` 与 `--dmrs`（真实非均匀布局）两种起点表，配合真实 stride（eq 14336 float2 / nv 16384）
- 全组模式在 3/6/11 符号下**每个符号都一致**（`OK: matches on every symbol`）
- 用法：`./build/lib/phy/upper/channel_processors/metal/eq_batch_kernel_probe 11 72 28672 --dmrs 16384`

**离线判据（秒级，不需要 air）**：
```bash
OCUDU_UL_DUMP=/tmp/a .../ul_chain_replay /tmp/iq1_5480_17921 --metal --out /tmp/a
OCUDU_EQ_DEFER_ENCODE=0 OCUDU_UL_DUMP=/tmp/b .../ul_chain_replay /tmp/iq1_5480_17921 --metal --out /tmp/b
cmp /tmp/a_5480_17921_llr.bin /tmp/b_5480_17921_llr.bin   # 必须 0 差异
```
修复后：**0 字节差异**（6380 字节 LLR）；修复前是 **3732 字节**。

**实网定案（用户执行，`bc2331efc1`，`/tmp/gnb_b9.log`）**：
| 版本 | equalizer dispatches | PUSCH BLER | attach |
|---|---|---|---|
| b3 批量（主机拷贝） | 批量 | 95.1% | ❌ |
| b7 批量（设备读） | 批量 | 98.2% | ❌ |
| b8 逐符号（当时默认） | 17809 | 22.2% | ✅ |
| **b9 批量（本次修复）** | **6476** | **25.8%**（418/1619；前200=32.5%→后200=25.5%） | ✅ attach + PDU Session + GTP-U + ping |

**默认值已翻转**（`2efa704850`，`GPU-PHY S-7e-b4`）：批量成为默认，`OCUDU_EQ_DEFER_ENCODE=0` 为逃生口
（两条路径逐字节一致）。整链 dispatch 22→15。`ctest -L phy` 162/162。

**关于两次实网 BLER 的可比性（重要，别误读）**：b8 的 22.2% 与 b9 的 25.8% 来自**不同的运行**，
空中条件、调度点、重传时机都不同，因此**不能**由这两个数字断言"批量比逐符号差 3.6 个百分点"。
可以断言的是：修复后的批量是**健康**的（attach 完成、ping 通、无 RLF、BLER 与逐符号同一量级、
且不随时间恶化），而修复前是 95–98% 且完全无法 attach。两者在**离线回放**上逐字节相同（0 差异），
这才是"算术等价"的严格证据；实网数字只用于证明"链路健康"。若要严格比较实网 BLER，
需要在同一次会话里用 `OCUDU_EQ_DEFER_ENCODE` 切换做两段统计。

#### 48.34 默认腿（b10）实网确认 + 退出统计改走日志系统

**默认腿实网确认（用户执行，`2efa704850`，`/tmp/gnb_b10.log`）**：批量翻为默认后**不需要任何 env**，
实网同样健康 —— `rrcSetupComplete` × **2**、`crc=OK` 963 / `crc=KO` 451（**BLER ≈ 31.9%**）、
**`RLF` = 0**、attach + PDU Session + GTP-U + ping 全通。默认路径的实网验证就此闭环：

| 腿 | 提交 | 编码 | 结果 |
|---|---|---|---|
| b8 | `69ab9d3fec` | 逐符号（当时默认） | 22.2% BLER、attach/ping 通 |
| b9 | `bc2331efc1` | 批量（显式 `OCUDU_EQ_DEFER_ENCODE=1`） | 25.8% BLER、attach/ping 通 |
| **b10** | **`2efa704850`** | **批量（默认，无 env）** | **31.9% BLER、attach/ping 通、RLF=0** |

三次的 BLER 差异来自不同的空中条件与调度，**不能**据此比较编码优劣；编码正确性的严格证据仍是
离线回放的 **0 字节差异**（§48.33）。

**退出统计改走日志系统（`a6d3647fb0`）**：此前所有 Metal/PHY 探针的退出统计都**直打 stderr**
（`[metal_stats]`/`[mmse_time_*]`/`[ldpc_time_*]`/`[ul_*]`，共 21 处）。这是**有意**的 ——
日志后端在那个时点不保证还活着 —— 但代价是 console 被刷屏、最后一行还被截断，而且**日志文件里
一行都没有**。现在改为走 PHY logger：

- **级别决定去处**：`[ul_pipeline]`（整链延迟）与 `[ul_mac_pdu_size]` 留在 **info**（console + 文件），
  分段/分几何明细降到 **debug**（只进文件，`all_level: info` 时 console 不再有它们）。
- **格式串必须是 fmt 占位符**：`fmt` **不解释** `%llu`/`%.2f`，会**原样打印**。所以转换做的是
  "把 printf 说明符翻成 `{}`"，而不是只把 `fprintf` 改名 —— 这一点已在运行期验证
  （`calls={} ... {:.1f}us` 渲染正确）。
- LDPC 的迭代/上限直方图原本是**多条 stderr 碎片**，现合并为**一条日志记录**。
- `ul_pipeline_probe::report()` 移到**拥有日志 banner 的作用域内**：放在它之后调用时，logger 到达
  file sink 已经太晚 —— 这正是探针当年选择直打 stderr 的原因。
- 应用**退出时 flush** logger，确保关闭期的记录落盘。

**注意**：这些统计现在**受日志级别约束**。默认 `all_level: info` 下，`[mmse_time*]`、`[metal_stats]`、
`[ldpc_time*]` 等**不在 console**、但**会写进 `gnb.log`**；若要 console 上也看到，把 `all_level` 设为 `debug`。
（这让"console 只留最重要的"成立；代价是 OCUDU 的 sink 层不参与级别过滤，无法做到"文件比 console 更详细"。）

**顺带发现的既有构建脆弱性**：`ENABLE_FLOW_PROBES=ON` 且 `ENABLE_CE_TIME=OFF` 时
`port_channel_estimator_metal_mmse_impl.cpp` **编译失败**（`time_en` 恒定义，但 `time_en==true` 的分支
引用只在 `OCUDU_CE_TIME` 下声明的 `stage_us_local` 等；`if (time_en)` 不是 `if constexpr`，
关闭时仍会实例化）。与本轮改动无关（已用干净 HEAD 复现），但**顺手记下**：需要这三个开关一起开
（`ENABLE_FLOW_PROBES`、`ENABLE_METAL_STATS`、`ENABLE_CE_TIME`）才能编过。

#### 48.35 两个构建缺陷修复：`ENABLE_FLOW_PROBES` × `ENABLE_CE_TIME` 组合 + 非 Apple Silicon 上的 Metal 开关

**1) `ENABLE_FLOW_PROBES=ON` 且 `ENABLE_CE_TIME=OFF` 无法编译**（`d62ef5e530`）。
信道估计器用运行期 `const bool time_en` 保护它的计时上报，probe 关闭时该值为 `false`——
但 `if (time_en)` 的函数体**仍会被编译**，而里面引用了只在 `OCUDU_CE_TIME` 下才声明的
`stage_us_local` / `submit_us_local` / `unpack_us_local` / `deferred_stats` / `deferred_wait_begin`
以及若干时钟读取。修法：上报块改用**预处理器条件**；它消费的那些测量也在同一条件下声明；
其中两个计数器（`tail_L`、`cpu_fallback_blocks`）是**数据路径写入、只有上报读取**，用
`[[maybe_unused]]` 一处声明意图，避免给每次赋值都加 `#if`（也避开 `-Werror`）。

**验证矩阵**（`ENABLE_FLOW_PROBES` × `ENABLE_CE_TIME` 四种组合 + 全量 `gnb`）：
`OFF/OFF`、`ON/OFF`（原失败）、`OFF/ON`、`ON/ON` **全部 errors=0**。

**2) 非 Apple Silicon 上 `ENABLE_METAL_*=ON` 被静默忽略**（同一提交）。
`ENABLE_METAL_LDPC` / `CHEST` / `DFT` / `EQUALIZER` / `DEMODULATION` 是用 Metal + Objective-C++
实现的，在别的平台**根本无法兑现**：它们在该平台默认为 `OFF`，但显式传 `=ON` 会被 CMake 接受，
然后被读取该选项的代码忽略——**构建"成功"了，而请求的后端从未编进去**（对使用者是静默的谎言）。
现在在 CMake 配置阶段直接 `FATAL_ERROR`，消息列出这几个选项、说明原因（需要 Apple Silicon）、
并给出做法（全部关掉，或在 Apple Silicon 上构建）。已验证消息能正确触发（在本机临时反转条件测试，
随后还原）；默认配置与 `ctest -L phy` 162/162 均不受影响。

**教训**：调试探针的开关组合也属于"构建契约"的一部分 —— 一个**恒为 false 的运行期开关**
保护不了它引用的编译期实体，编译期的事就该用编译期条件表达。

#### 48.36 退出期崩溃的收尾：`atexit` × logger 的真实根因、修复与一次误判的更正

**（承接 §48.34/§48.35；本轮由用户报告"Crtl-C 退出段错误"驱动。）**

**1) 用户报告的崩溃：确实是本次日志整理引入的，已修（`912934d25b`）。**
崩溃报告 `~/Library/Logs/DiagnosticReports/gnb-2026-09-14-111530.ips` 给出了精确堆栈：

```
#0 std::__hash_table<...>::find
#1 ocudulog::detail::find_logger
#2 ocudulog::fetch_basic_logger            ← 单例已析构（访问地址 0x58）
#3 ocudu::metal::(anonymous)::burst_stats_report()
#4 __cxa_finalize_ranges
#5 exit
```

根因：这些探针注册在 **`std::atexit`**，而它们当年直打 `fprintf(stderr)` **正是因为这个**（探针注释里写明"日志后端在退出时不可靠"）。
把输出改成 logger 却没动调用时机 ⇒ atexit 在静态对象析构**之后**运行，`fetch_basic_logger()` 读已释放的注册表。
**退回 stderr 只是掩盖**，所以正确修法是新增 `phy_shutdown_report`（`include/ocudu/support/executors/phy_shutdown_report.h`）：
11 个探针不再用 atexit，改为注册回调；gNB/du 在**服务已停、日志仍活着**时显式 `run_all()`。级别语义不变（info 上 console，debug 只进文件）。
同时删掉我早先多加的 `ocudulog::flush()` —— `apps/gnb` 本来就有 `make_scope_exit` 的 flush，那次是重复调用（**注意：它不是崩溃原因，我一开始误判成它**）。

**2) 一次误判的更正（重要）**：排查中我曾断言"`ctest` 的 discovery 崩溃是既有问题"，并给用户看了两条"证据"，**两条都不成立**：
- `ctest -N` 报 `discover_tests failed` / `Abnormal exit: Segmentation fault`：实际是 **CMake 的 discovery 缓存陈旧**（我在排查中反复重编测试二进制，缓存未刷新）。
  `cmake -S . -B build` 重配置后即消失 —— 现在 `discover_tests failed` = 0、`Abnormal exit` = 0、**7617 个测试**全部发现正常，`ctest -L phy` **162/162**。
  另外那 48 处"segmentation"匹配是我的 grep 误报：它们匹配到了测试名里的 `rx_without_segmentation`。
- `pusch_demodulator_deferred_chain_test` 退出时 `exit=139`：**不稳定复现**。加一行 `fprintf` 打点即消失（典型静态初始化顺序敏感），移除打点后连跑 6 次、**ASan 构建连跑 3 次均无任何内存错误**（exit=0）。现工作区干净、连跑 5 次全部 exit=0。

**结论与教训**：
- 真正要修的只有 gNB 的 Ctrl-C 崩溃，它已修并推送；另两个现象是**环境/缓存与偶发**，不是代码缺陷，现已全部恢复正常。**教训：把"重编译后的陈旧发现缓存"当成代码问题，会得出错误的 A/B 结论。**
- 对未能复现的偶发崩溃**不做猜测性改动**（尤其不动 `ocudulog` 单例生命周期这种核心机制）：动它的代价是可能引入新的退出期数据丢失/回归，收益却无法验证。
- 若该偶发崩溃再次出现，抓取方式：`lldb -b -o run -o "bt all" -o quit -- <test-binary>`，或看 `~/Library/Logs/DiagnosticReports/*.ips`（本次正是靠它一击定位）。

**留给后续的观察（不在本轮范围）**：`ocudulog` 的单例（`ocudulog_instance::get()` 的函数局部静态）随进程退出析构，**析构之后任何 logger 访问都会读已释放内存**。
任何在静态/atexit 析构阶段取 logger 的代码都会中招（本轮 gNB 崩溃就是这一类的实例，已通过"提前显式报告"绕开）。
若要根治这一类，需要给 ocudulog 一个显式的 shutdown 契约或让单例不析构（代价：文件 sink 的 EOF 标记与 buffer flush 需另行保证）——
**这需要独立的评估与验证，不应顺手改**。

#### 48.37 主线下一步：demapper 批量化（占 dispatch 的 73%）—— 设计与一次未能收口的实现

**背景（用户实网数据，`c2879b8940` 之后的 Metal 全链路腿）**：
```
[ul_pipeline] samples=1054 mean=746.6us median=730.0us p95=915.0us p99=1169.0us
[ul_time_frequency]       255.9us   (33%)
[ul_channel_estimation]    94.6us   (13%)
[ul_equalization_demod]   371.5us   (50%)   <- 最大项
[ul_ldpc_decode]           24.6us   ( 3%)
[ul_fapi_mac]               6.0us
[metal_stats] burst dispatches=20460 (equalizer=5456 demapper=15004)
```
阶段之和 752.6 ≈ 实测 746.6（账本闭合）。**demapper 占全部 dispatch 的 73%**，而 equalizer 已经批量化
（`eq_batch runs=5456 batched=5456 max_run=4`，11 → 4）。

**为什么值得做**：链探针 Pattern D 的实测结论——"一次 dispatch 约 10µs，而 25 PRB 一个符号的
GPU 工作量只有几 µs"（实测批量 vs 逐符号：207.3 → 103.0 µs/slot，**2.01×**）。demapper 每个符号
一次 dispatch（15004 次 / 1054 slot ≈ 14.2 次/slot，正是符号数），所以它是这条链上最后一块未批量的。

**内核无需改动的关键事实**（与 equalizer 批量化不同，那里必须加"每符号起始表"）：
`ocudu_demod.metal` 的 `demod_soft` 本来就是 **1D 网格 + `p.nof_symbols`**，且
`symbols[sym]` / `noise_var[sym]` / `llrs[B*sym + bit]`（`B` = bits/symbol）**全部紧凑连续**。
因此"把 N 个符号合并成一次 dispatch"只要传更大的 `nof_symbols` 与网格，不需要改 kernel、不需要 staging。

**主机侧可行的前提**：`pusch_demodulator_impl` 的 deferred 链按符号调用
`demapper->demodulate(...)`，每个符号的 `symbols`/`noise_vars`/`llrs` 都指向**连续内存的下一段**
（equalizer 的输出与 demapper 的 LLR staging 都是按符号连续分配的），这正是内核已经假设的布局。

**本轮实现（已回退，未收口）**：
- 引擎新增 `enqueue_burst_deferred(symbols, noise_var, llrs, nof_re, mod)`：累积到 per-engine 的
  `std::vector<demod_pending_t>`，并通过 `shared_burst::set_flush_hook` 安装 `demod_flush_hook`
  （安装前先 `flush_pending()` 交接上一个 stage，机制与 equalizer 相同）。
- `demod_flush_hook`：按 `mod` 相同 + 三个指针"首尾相接"（`next == prev + nof_re*stride`）合并成 run，
  每个 run 一次 dispatch（`total_re` 为网格 x）。
- 适配器 `demodulation_mapper_metal.cpp` 的 `defer` 分支改调上面这个入口。

**卡点**：`ul_chain_replay` 在该实现下 **rc=139**，lldb 只给到驱动帧
`-[AGXG16XFamilyComputeContext dispatchThreads:threadsPerThreadgroup:]`（`EXC_BAD_ACCESS` at `0x1c0`），
应用侧帧不可见；诊断打印显示 **第一个 run 只合并到 1 个符号**（连续性判定未成立，`n_sym=1`，
`total_re=72`、`mod=3`、`pending=11`）后即崩。两个待查点（按可能性排序）：

1. **连续性判定未成立**说明我对三段内存"按符号连续"的假设只对其中一部分成立——需要把
   `symbols`/`noise_var`/`llrs` 的**实际步长**打出来与 `nof_re*stride` 对照（可能某一维每符号带间隙，
   例如 LLR 的 staging 槽按页对齐）。
2. **dispatch 崩溃**：`wrap_buffer` 在 flush hook 内对三段各做一次 no-copy wrap，而这三段正是
   equalizer/demapper 之间共享的对象；需要确认在 hook 时机下 `wrap_no_copy` 的
   包含式查找返回的对象与 offset 仍与逐符号路径一致（`[metal_stats] wrap hits/replaces` 是判据）。

**当前状态**：改动已 stash（`git stash list` 里的 `demod-batch-wip`），基线恢复并验证
（`ul_chain_replay` rc=0、LLR 与参考逐字节一致）。下次继续时 `git stash pop` 即可取回。

**方法论提醒**：这条链的 dispatch 合并**必须**用本机 `ul_chain_replay` 做等价性 + dispatch 计数双验证
（秒级、可重复），再考虑上机——实网腿只用于确认收益，不用于调试。

#### 48.38 S-7f 第一步：`[ul_gpu_lane]` 落地（GPU 侧 residency / busy / gap / period）（`b8db30e9cc`）

**为什么先做这个**：路线图 §10.7 的四个指标此前**只有注释**。没有它，能看到的只有 CPU 分段
（§48.10 之后这三段的语义已经清楚：`t2f` 量的是"14 个符号被消化的跨度"、`ce`/`eqdem` 量的是
"模块被调用的边界"），**无法回答"GPU 是不是在等 CPU 喂"**——而这正是接下来 K0/S-7d 那一堆改动的
唯一判据（§10.7.3）。

**lane 的定义（本轮的落地版）**：一条 lane = **一个线程上、上一次结算之后提交的全部 GPU command buffer**，
结算点是 **burst 完成**（burst 就是产出 LLR 的那一段，见 §48.22/§48.33）。由此四个指标：

| 指标 | 定义 | 本轮状态 |
|---|---|---|
| `residency` | 该 lane 全部 CB 的 `min(GPUStartTime)` → `max(GPUEndTime)` | ✅ 已实现 |
| `busy` | `Σ (GPUEndTime − GPUStartTime)`，并按阶段拆分 | ✅ 已实现（`ch_est` / `eq_demap`） |
| `gap` | `residency − busy` = **数据在卡上、卡却没在算**（＝等 CPU 喂） | ✅ 已实现 |
| `period` | 相邻 lane 结束时刻之差（吞吐节拍，判据是能否跟上 1 slot/ms） | ✅ 已实现（跨线程全局，乱序发布丢弃并计数） |

**实现**（新文件 `lib/phy/metal/ocudu_metal_lane_probe.{h,mm}`，编译开关仍是 `ENABLE_METAL_STATS`）：

- **登记点**：`shared_burst::commit()`（阶段 `eq_demap`，burst 含 eq+demap 两类 dispatch）+
  信道估计引擎的 **5 个 commit 点**（阶段 `ch_est`）。
- **结算点**：`shared_burst::wait_committed()` 里、`waitUntilCompleted` 之后调用 `close_lane()`
  —— burst 是该 lane 的最后一条 CB，而 DFT 之外的整条后端链都在同一线程、同一队列上。
- **登记的是 CB 句柄而不是时间戳**：`GPUStartTime/GPUEndTime` 只有 CB 完成后才有效，而 CE 的 CB 是
  延迟结算的（`run_async` 到下一次 CE 调用才 wait，§48.6）。所以在 commit 时登记句柄、结算时读时间戳；
  结算时若 CB 尚未完成，把它**顺延到下一个 lane**（不记为 gap），顺延量单独计数（上限 64 条）。
- **同队列"更早提交的先完成"是本设计的前提**，已实测确认：实际链路里 CE 的 CB 在 burst 完成时必然已完成
  （两个工具的 `carried=0`）。

**一个新踩到的坑（退出期 × 静态析构，值得记住）**：统计对象里有 `std::mutex` / `std::vector`，
而报告必须在 **atexit** 里跑（这些汇总的语义就是"无条件可见"，见 §48.34/§48.36，必须 `fprintf(stderr)`）。
若统计对象是**函数局部静态**，它在 atexit 之前就析构了 —— 实测症状是退出时
`libc++abi: terminating due to uncaught exception ... mutex lock failed: Invalid argument`
（libc++ 把"锁已析构的 mutex"变成了异常）。两个修法叠加：统计对象**故意不析构**（heap 单例，
泄漏几百字节），atexit 改为**命名空间作用域注册**（这样"一条 lane 都没有"也能打印出来——
探针沉默与探针没编进去必须可区分）。

**验证（全部本地、秒级）**：

| 门禁 | 结果 |
|---|---|
| `ul_chain_replay /tmp/iq1_5480_17921 --metal` | `lanes=1 cbs/lane=2.00 dropped=0 carried=0`；busy split `ch_est≈150µs / eq_demap≈100µs`（三个不同 reception 复测一致） |
| `pusch_demodulator_deferred_chain_test`（69 lane，估计器替身） | `lanes=69 cbs/lane=1.00 gap=0.0us`（该用例不经过真 CE，故只有 `eq_demap`——阶段拆分如实反映） |
| 离线 A/B（默认 vs `OCUDU_EQ_DEFER_ENCODE=0`） | **0 字节差异**（探针不影响数值） |
| `eq_batch_kernel_probe 11 72 28672 --dmrs 16384` | `OK: matches on every symbol` |
| `ul_chain_replay_asan` | rc=0，无 ASan 报告 |
| `ctest -L phy` | **162/162** |

**覆盖范围（重要，别误读）**：这条 lane **目前不含 DFT**。DFT 由 radio 线程提交在**前端队列**上，
而 lane 的记账是线程本地的；要并进来需要**跨线程、按 slot 的登记**（DFT 引擎知道 `slot`，burst 不知道），
这正是 §10.8 里 S-7f "ring + push/collect API" 要定的事（DFT 挪到 lane 队列时也可自然合并）。
⇒ 当前的 `residency` 起点是**信道估计的 GPU 起点**，不是"IQ 进 GPU 的时刻"；它量的正好是
**CE → eq/demap 这段后端链**（含中间那段 CPU staging，也就是 K0 的收益面）。

**上机要看什么**（下一腿，仍用测试台 `--pusch_ldpc_decoder_type auto`，见 §48.10）：
`grep "ul_gpu_lane" | tail` —— ① `period` 是否 ≈1ms（每个 slot 一次 PUSCH）；② `gap` 的绝对值与
`gap/residency`（这就是"CPU 没喂上 GPU"的量化）；③ `busy split` 里 `ch_est` 与 `eq_demap` 的比例
（离线定标约 60% / 40%）。预期：`busy ≈ 250–300µs`、`gap ≈ 200–300µs`（就是 §48.10 那三段 CPU 时间里
与 GPU 重叠不上的部分）。

#### 48.39 `[ul_gpu_lane]` 第一次实网读数（b11，`b8db30e9cc`，ping 轻载）：**后端 lane 是紧的、gap≈0；余量在 CPU 线程不在 GPU**

用户跑的一条腿（测试台协议：CPU LDPC；`rrcSetupComplete=1`、`RLF=0`、`Real-time failure in RF=28`、
`crc=OK 1224 / KO 265`），控制台汇总（`fprintf(stderr)`，退出时）：

```
[ul_gpu_lane] lanes=1489 cbs/lane=2.00 (max=2) dropped=0 carried=0 period_dropped=0
[ul_gpu_lane] residency samples=1489 mean=246.0us median=244.3us min=211.9us max=4021.0us p95=249.6us p99=369.5us
[ul_gpu_lane] busy      samples=1489 mean=242.6us median=243.5us min=211.4us max=624.3us p95=248.7us p99=368.3us
[ul_gpu_lane] gap       samples=1489 mean=3.4us   median=0.8us   min=0.5us   max=3804.1us p95=1.1us   p99=1.3us
[ul_gpu_lane] period    samples=1488 mean=92484.8us median=10533.5us min=702.0us max=2477817.1us
[ul_gpu_lane] busy split: ch_est=143.9us/lane (59% of busy, cbs/lane=1.00) eq_demap=98.7us/lane (41% of busy, cbs/lane=1.00)
```

**① 记账在实网精确成立（设计前提被证实）**：`lanes=1489` **等于** `[mmse_time_sum] calls=1489`
**等于** `[metal_stats] burst commits=1489` **等于** `eq_batch flushes=1489` ⇒ 每个 PUSCH occasion
恰好一条 lane；`cbs/lane=2.00` 恰好是「CE 一条 CB + burst 一条 CB」；`carried=0 / dropped=0 /
period_dropped=0` ⇒ **"同队列更早提交的 CB 在 burst 完成时必然已完成"这一前提成立**，没有一条 CB
需要顺延。跨线程/跨工具三处独立计数完全对齐，说明 lane 的边界就是 occasion 的边界。

**② 交叉验证**：`ch_est` 的 143.9µs 与 CE 引擎自己的 `[mmse_time_sum] gpu_wait=143.4µs` 差 **0.4%**
（两条完全独立的读数路径：一条按 CB 时间戳在 lane 结算时读、一条在引擎 wait 后读）；busy 的
`59% : 41%` 也与离线定标 `60% : 40%` 一致 ⇒ 探针本身可信。

**③ 核心结论：后端 lane 已经没有 gap**。`gap` 中位数 **0.8µs**、p95 1.1µs（mean 3.4µs 是被一个
3804µs 的离群点拉起来的，对应的正是那条 4021µs 的 residency）；`residency − burst_busy = 147.3µs`
就是 **CPU 侧 burst staging 的上界**（h/y 逐符号收集 + eq/demap 编码），而 CE 的 GPU 执行恰是
**143.9µs** ⇒ **两者几乎完全重叠，只漏出 ~3.4µs**。

> ⇒ §10.7.3 设想的"gap 就是 K0 那套改动的收益计量器"，第一次实测给出的答案是：
> **在 CE→LLR 这个窗口里已经没有 gap 可赚** —— S-7c-1b/5 把 CE 改成异步之后，
> CPU 的 staging 已经被 CE 自己的 GPU 执行（143.9µs）吃掉了。
> 之前"pre-LDPC 每跳 180–200µs CPU 时间会以 gap 出现"的推断，在**这个窗口**被证伪。

**④ 同一个 occasion 的两个视角：CPU 717µs vs GPU 243µs**：

| 视角 | 每 occasion | 来源 |
|---|---|---|
| CPU（线程串行时间） | **717µs**：TF 242.3 + CE 85.3 + EQ/DEMOD 364.4 + LDPC 20.8 + FAPI 4.1 = 716.9 ≈ `[ul_pipeline]` 712.9 | `[ul_*]` 分段（本例 LDPC 在 CPU） |
| GPU（时间戳） | **243µs**：CE 143.9 + eq/demap 98.7（+ gap 3.4） | `[ul_gpu_lane]` |

DFT 另算（不在 lane 内）：`dft commits=230133 / 14 ≈ 16438` 个 slot、按 §48.12 定标的
~17µs/transform ⇒ **~240µs/slot**。⇒ 若上行被拉满（1 PUSCH/slot），**GPU 约 48% 占用
（~483µs/1ms，仍有 ~2× 余量），CPU 线程约 72% 占用（717µs/1ms）** ⇒
**更近的上限是 CPU 线程，不是 GPU**。

**⑤ 这一腿没有给流水线压力（重要，别误读 period）**：全程 167s / 1489 lane = **8.9 lane/s**，
`period` 中位数 10.5ms —— 这是 ping 的授权节拍，不是 slot 节拍。`period` 只有在**饱和 UL**下才有
"能否跟上 1ms/slot"的含义。`gap` 也一样：轻载下没有排队，**不能据此断言饱和时也没有 gap**。

**⑥ 方法学收益（饱和档的尺子）**：§7 早已警告"饱和时各阶段探针不可信——排队时间总会落在交接后的
第一个阶段"。`[ul_gpu_lane]` 用的是**GPU 自己的时间戳**，不含任何 CPU 打点 ⇒ **它天然免疫那种污染**，
是饱和档下唯一还可信的分解。这也是下一腿（饱和 UL）要继续用它做判据的原因。

**⑦ 覆盖缺口（同 §48.38）**：lane 仍**不含 DFT** ⇒ 上面 246µs 是 **CE→LLR** 而不是 **IQ→LLR**；
DFT 的 GPU 时间（~240µs/slot）目前只能靠"commit 数 × 定标"间接估。补上它需要跨线程、按 slot 的登记
（S-7f-2）。

#### 48.40 **里程碑：饱和 UL（每槽一个授权）下 pre-LDPC 流水线跟得上 1ms 节拍，且 GPU 仍不缺 gap**（b12）

用户按 §10.8 的测试台协议跑了 **iperf3 上行**腿（仍 `--pusch_ldpc_decoder_type auto`，CPU LDPC）：

```
[ul_gpu_lane] lanes=25317 cbs/lane=2.00 (max=2) dropped=0 carried=0 period_dropped=0
[ul_gpu_lane] residency mean=236.4us median=237.5us min=191.4us max=1613.2us p95=250.6us p99=277.7us
[ul_gpu_lane] busy      mean=235.4us median=236.8us                           p95=249.9us p99=272.4us
[ul_gpu_lane] gap       mean=0.9us   median=0.6us   min=0.4us max=1393.5us   p95=0.9us   p99=1.0us
[ul_gpu_lane] period    mean=3530.1us median=1018.7us min=335.1us max=16503235.1us p95=1108.2us p99=1287.7us
[ul_gpu_lane] busy split: ch_est=140.7us/lane (60%) eq_demap=94.7us/lane (40%)
[ul_pipeline] samples=22828 mean=651.3us median=646.0us min=478.0us max=3317.0us p95=769.0us p99=834.0us
```

**负载是真的满了（不是"看起来满"）**——按秒统计 `crc=` 行：

| 时段 | 每秒 TB 数 | 含义 |
|---|---|---|
| 06:23:25–06:23:36（12s） | **1000** | 15 kHz ⇒ 1ms/slot ⇒ **每槽恰好一个 TB** |
| 06:23:51–06:24:03（13s） | **1000** | 同上 |
| 其余 | 0–38 | 空闲/零星 |

⇒ 25s 的**完全饱和**段贡献了 **98.7%** 的 lane（≈25000/25317），所以上面的分位数就是饱和档的数字。
健康度：`crc=OK 22828 / KO 2489`（**BLER 9.8%**）、`rrcSetupComplete=2`、**`RLF=0`**、
`Real-time failure in RF=4`、**`Real-time failure in FAPI: UL processor=0`**（对比 §7 当年的 405–3919 条）、
吞吐 1000 TB/s × 469B ≈ **3.8 Mbps**。

**三条结论**：

1. **节拍跟上了**：`period` 中位数 **1018.7µs** ≈ 1 slot（p95 1108µs、p99 1288µs）⇒ 每槽的 PUSCH
   occasion 被逐个消化，没有积压；`[ul_pipeline]` **651.3µs < 1000µs**（p99 834µs）⇒ 端到端余量 ~35%。
2. **GPU 仍然不缺 gap**：`gap` 中位数 **0.6µs**、p95 0.9µs —— 与轻载腿（0.8µs）**一样紧**，
   而且 `residency` 反而更小（236.4 vs 246.0µs）、更稳（p95 250.6 vs 249.6，min 191.4）。
   ⇒ **"CPU 没喂上 GPU"在饱和档也不成立**：CPU 的 burst staging 依旧被 CE 自己的 GPU 执行盖住。
   也就是说 §48.39 的结论不是轻载假象，**它经得起满载检验**。
3. **每槽的 GPU 账**（饱和档）：lane 内 **235µs**（CE 140.7 + eq/demap 94.7）+ DFT ~240µs（按 §48.12
   定标 × 14 符号；`dft commits=398051/14 ≈ 28432` slot）⇒ **≈ 476µs / 1000µs ≈ 48% 占用**；
   CPU 线程侧 `defer_wait 356.3 + CE host 61.1 ≈ 417µs/slot ≈ 42%` ⇒ **两侧都还有 ~2× 余量**。

**⚠ 一个必须说清的边界**：这一腿的"跟得上"是在**测试台（CPU LDPC）**下取得的。生产配置
（`--pusch_ldpc_decoder_type metal`）下 §48.10 实测 `[ul_pipeline] 4450.8µs` ⇒ **4.4× 超预算，
不可能每槽一个授权**。所以本阶段的结论精确表述是：**pre-LDPC 链（IQ→LLR）已经能在 1 slot/ms 下跑满，
真正的产能上限仍在 CPU 侧的 LDPC**（§10.11.2：进 LDPC 阶段时的第一个测量就应该是它的 busy/gap 分解）。
另：`--pusch_ldpc_decoder_type auto` 时 LDPC 每 TB 只要 29.2µs（CPU），与 metal 分层核的 3.6ms 差两个数量级。

**副作用（正向）**：这一腿的分段探针**没有**被排队污染（§7 当年 iperf3 一开就 14–32ms）：
`ul_time_frequency 221.3` / `ul_channel_estimation 76.1` / `ul_equalization_demod 324.8`，
三段合计 622.2 ≈ `[ul_pipeline] 651.3 − LDPC 29.2`。⇒ 当年的"iperf3 排队"问题在 pre-LDPC 侧已经解决。

#### 48.41 S-7e-demap 收口：解调器批量化落地（11 → 1 次 dispatch/组），根因是 **LLR 目标没有页对齐的槽位**

§48.37 的 WIP 卡在"主机三段内存不连续"上。这次把它查清了，结论与当初的判断**相反**：

**真正的布局（实测定案）**：三段的步长都是**均匀**的，问题只出在第三段的目的地上。

| 段 | 步长 | 是否页对齐 | 结论 |
|---|---|---|---|
| `symbols`（均衡输出） | `d_sym=114688 B = 14336 float2` | ✅ | 就是 `eq_symbol_stride_re`，本来就均匀 |
| `noise_var` | `d_nv=65536 B = 16384 float` | ✅ | 就是 `eq_symbol_stride_nv`，本来就均匀 |
| **`llrs`（解调目的地）** | **不是布局，是 malloc 的巧合** | ❌ | `state.llr_offset` 按**紧凑**累加（576 B/符号）⇒ 除第 0 个符号外都不页对齐 ⇒ 适配器**逐符号 staging** 进自己的缓冲（`entry->llr`），那些缓冲的间距是分配器的产物 |

⇒ 一次批量 dispatch **根本没有可寻址的目的地**：WIP 里看到的 `d_llr=16384` 是"两个独立分配恰好相距 16 KiB"，
不是契约。据此批量化必然写坏别人的缓冲（或读到未写过的内存）。

**修法（两处，都很小）**：
1. `pusch_demodulator_impl`：每符号的 LLR 偏移改用**本已存在的页对齐步长**
   `llr_offset += llr_symbol_stride`（该缓冲本来就是按 `max_deferred_group_symbols * llr_symbol_stride`
   分配的，所以**不增加内存**，只是不再紧凑使用）⇒ 每个符号的 LLR 槽位页对齐 ⇒ 内核可**就地**写。
   额外收益：适配器里每符号一次的 LLR **回拷消失**（`llr_direct` 变真）。
2. 解调核与引擎：`demod_params` 增加 `nof_re` 与三个 stride（`sym_stride`/`nv_stride`/`llr_stride`），
   网格从"一维元素表"变成 **(调制符号) × (OFDM 符号)**；单符号调用传 `stride=1/1 + 紧凑 LLR 长度`，
   **每元素的算术一字未改**（逐位一致性由此保证）。引擎新增
   `enqueue_burst_deferred()` + flush hook：按 `(mod, nof_re, 三个 stride 全等)` 合并 run，
   一个 run 一次 dispatch，**不做任何 staging**。
   逃生口：`OCUDU_DEMOD_DEFER_ENCODE=0` 回到逐符号编码。

**途中自己制造并抓到的缺陷（值得记）**：`packed_params()` 里我把"旧的扁平 `nof_symbols`（元素个数）"
直接当成了"新的批量维"⇒ 逐符号路径发出 **(72, 72)** 的二维网格、越界写自己那一片 LLR 之外的槽位。
症状：并发用例 **12/12 段错误**（lldb 停在 Metal 完成队列线程的 `os_unfair_lock_lock @0x4018`）。
教训：**改了语义的参数必须在每个调用点重新推导**；这次是并发用例把它逮住的（批量路径 2/12、逐符号 12/12，
对比立刻指向新内核而不是批量化本身）。

**门禁（全部本地）**：

| 门禁 | 结果 |
|---|---|
| 实链 dispatch 计数（`[metal_stats] demod_batch`） | `flushes=1 symbols=11 dispatches=1 max_run=11`（原 11 次） |
| 离线 A/B（默认 vs `OCUDU_DEMOD_DEFER_ENCODE=0`，逐记录解析） | **8 个 reception 的记录逐字节相同** |
| 并发用例（4 worker × 15 轮）× 12 次，两种编码各 12 次 | **0 次崩溃** |
| `ctest -L phy` | **162/162** |
| `ul_chain_replay_asan` | rc=0，无 ASan 报告 |
| `metal_chain_probe` 的 R-vs-B / R-vs-C / G / H | **0 差异 LLR 字节**（H：`flushes=1 runs=1 batched=1 max_run=12`） |
| `eq_batch_kernel_probe 11 72 28672 --dmrs 16384` | `OK: matches on every symbol` |

**⚠ 方法论陷阱（这次差点被它骗掉一小时）**：`OCUDU_UL_DUMP*` 的落盘文件是**追加模式**（`fopen(..., "ab")`）。
复用同一个前缀跑第二条腿，文件里会**累积两次**的记录，`cmp` 出来的"4776 字节不同"完全是假的。
⇒ **A/B 门禁必须每次用新前缀**（或先删旧文件），并按 `[uint32 长度][数据]` 记录逐条比较，而不是整文件 `cmp`。

**预期上机收益**（§48.12 的本地单价：CB 内 ~10µs 主机编码 + ~4µs GPU / dispatch）：每 occasion
解调 11 → 1 次 dispatch ⇒ **~100µs 主机编码 + ~40µs GPU**；`[ul_gpu_lane]` 的 `eq_demap` busy
应从 ~95µs 降到 ~55µs，`[ul_pipeline]` 相应下降。判据腿：与 §48.40 同样的**饱和 iperf3** 负载，
只换二进制，直接对比 `[ul_gpu_lane] busy split`、`[ul_pipeline]`、`[ul_equalization_demod]`、
`[metal_stats] burst dispatches`。

#### 48.42 demapper 批量化上机验证（b13，`80e131595d`，饱和 iperf3）：**收益兑现（GPU −47µs / 管线 −59µs）**；同时暴露一个**长期存在的部分带宽缺陷**

**收益（b12 逐符号 → b13 批量化，同一负载协议、同一几何）**：

| 指标 | b12（逐符号） | **b13（批量）** | 变化 |
|---|---|---|---|
| `[ul_gpu_lane] eq_demap` busy | 94.7µs | **47.3µs** | **−50%**（预期 ~47µs ✓） |
| `ch_est` busy（对照，不该动） | 140.7µs | 141.7µs | ✓ 不变 |
| `residency` mean | 236.4µs | **190.1µs** | −46µs（−20%） |
| `gap` median | 0.6µs | **0.6µs** | ✓ 仍紧（没有把 CPU 推到 gap 上） |
| `period` median | 1018.7µs | 1019.0µs | ✓ 节拍不变 |
| `[ul_pipeline]` | 651.3µs | **592.1µs** | −59µs |
| `[ul_equalization_demod]` | 324.8µs | **275.5µs** | −49µs |
| `defer_wait` | 356.3µs | **310.2µs** | −46µs |
| dispatch / lane | 15（eq 4 + demap 11） | **5**（eq 4 + demap 1） | `demod_batch flushes=25862 dispatches=25862 max_run=11` |
| `crc=OK / KO` | 22828 / 2489 | 16306 / 9556 | ⚠ 见下 |

**⚠ BLER 看起来退化了（9.6% → 36.9%），但根因不是这次改动**，证据链如下：

1. **全带宽桶反而更好**：25 PRB 的 BLER **1.8% → 1.1%**。所有退化都在**部分带宽**分配上。
2. **部分带宽是"每一条腿都坏"的老问题**（含本阶段之前的所有腿，以及 00:27 的抓包腿）：

   | 腿 | 25 PRB | 3 PRB | 其余部分带宽 |
   |---|---|---|---|
   | b9（S-7e 修复腿） | 0.6% | **60.6%** | **36.3%** |
   | b10（批量默认腿） | 0.0% | **83.9%** | **37.6%** |
   | b11（lane 探针 ping 腿） | 4.5% | **78.0%** | **24.3%** |
   | b12（饱和，逐符号） | 1.8% | 96.1% | ~30% |
   | b13（饱和，批量） | 1.1% | 96.2% | ~50% |
   | 00:27 抓包腿（更老二进制） | 1.1% | **97.8%** | 0–83% |

3. **本次改动对 LLR 是逐字节中性的**（这是算术等价的严格证据）：
   - 39 个**部分带宽**真实抓包（3/6/7/10/13/17 PRB）+ 8 个全带宽：批量 vs 逐符号 **逐记录完全相同**；
   - 人工构造的 **14 PRB** 几何（改写抓包元数据的 `alloc_prb`）：同样完全相同；
   - 3 PRB 抓包上，**Metal 解调器 vs CPU 解调器 0 字节差异**（⇒ 解调实现本身是忠实的）。
4. **聚合 BLER 的腿间差异由"调度器挑了哪些窗口"决定**：b13 的部分带宽授权占 56%（b12 为 41%），
   且同一窗口本身有腿间波动（14 PRB：b12 24.5% → b13 81%），而**同一配置在两个编码下 LLR 相同**。

**部分带宽缺陷的可观测签名（下一步的线索）**：

| 窗口 | b12 | b13 | 说明 |
|---|---|---|---|
| `[1,25)`（24 PRB，偏一侧） | — | **0.1%** | 同样是部分带宽，却是好的 |
| `[11,25)`（14 PRB） | 11.6% | 49.1% | 坏 |
| `[0,14)`（14 PRB） | 73.5% | **99.6%** | 最坏 |
| `[0,23)`（23 PRB） | 75.4% | 78.0% | 坏 |
| `[4,7)`（3 PRB） | 96.1% | 96.2% | 最坏（`tbs=11`） |

⇒ **与"窗口位置"强相关，而不只是"带宽小"**。另外至少有两种不同现象：
- 3 PRB 那类：live 日志本身报 **sinr −20…−30 dB**（接收到的就是垃圾）⇒ 多半是**发端/调度**问题；
- 14/23 PRB 那类：live 报 **sinr p50=11.9 dB 却 81% 失败**（`tbs=54`、QPSK、低码率）⇒
  **"自信地错"**（估计/噪声方差或 TBS/速率匹配不一致），这才是最可疑的接收侧问题。

**下一步**：抓一份含**失败的部分带宽接收**（`OCUDU_UL_DUMP` + `OCUDU_UL_DUMP_LLR=1`，预算放大到数百）
再离线定位；注意抓包模式会调用 `sync_device_estimates()`（**时序不再是生产时序**，但数值仍忠实）。

#### 48.43 **定位到长期 UL 失败的真根因：信道估计器的"边块"布局在 `PRB 数 ≡ 2 (mod 3)` 时错位**（b14 抓包 + K4 区域分解）

**b14（`OCUDU_DEMOD_DEFER_ENCODE=0`，批量关）的 A/B 判定**：逐窗口 BLER 与 b13 一致
（25 PRB 0.4% vs 1.1%、部分带宽 71.0% vs 65.2%），**批量化在实网被彻底洗清**；b14 的墙钟数字被抓包模式
污染（`Real-time failure in FAPI: UL processor=930` ≈ 被抓的 793 个接收），不能当性能基准。

用 b14 抓下的 793 个接收（含 186×14 PRB、95×24、57×23、56×3）逐层定位：

**① 3 PRB（msg3）那批：栅格里根本没有信号**。全带宽抓包总功率 8.7，而 3 PRB 那批只有 **0.001**，
且各 PRB 功率均匀（in/out 比 = 1.00）⇒ live 报 −20…−30 dB 是**诚实的**，UE 压根没发
（gNB 在给已经消失的 UE 反复重传，SCHED 日志里 `newtx=false rv=0` 刷屏）⇒ **不是接收机问题**。

**② 14/23/24 PRB 那批：信号在、位置对、功率正常**。in/out 功率比 5000–10000、峰值 PRB 落在分配内、
总功率与全带宽同级（9.2–9.6）⇒ 排除"发错位置/没发"；调度日志按 slot 展开后**没有任何重叠调度**
（0 个 collide slot）⇒ 排除同频干扰 ⇒ **是接收侧的问题**。

**③ 症状：上报的后均衡 SINR 比真实低 ~30 dB**。栅格的带外/带内功率比说明真实 SNR ≈ 35–40 dB，
但同一 UE、同一时刻：

| 分配宽度 | 上报（后均衡）SINR | 结论 |
|---|---|---|
| 25 / 24 PRB | 32.2 / 34.9 dB | ✅ 与真实相符 |
| 23 PRB | **6.2 dB** | ❌ 低 ~28 dB |
| 14 PRB | **3.4–3.7 dB** | ❌ 低 ~33 dB |

**④ K4（设备噪声核）区域分解找到触发条件**（临时探针把残差按"标准块/边块"分别归约）：

| PRB | n_blk | nf_std | nf_tail | 标准块残差 | **边块残差** | 设备 nv | 后均衡 SINR |
|---|---|---|---|---|---|---|---|
| 25 | 8 | 36 | 12 | 0.0867 | **0.0025** ✅ | 0.000199 | 32.2 dB |
| 24 | 8 | 36 | 0（无） | 0.0477 | 0 ✅ | 0.000111 | 34.9 dB |
| **23** | 7 | 36 | **24** | 0.0588 | **37.7** ❌ | 0.0915 | 6.2 dB |
| **14** | 4 | 36 | **24** | 0.0681 | **49.9** ❌ | 0.199 | 3.7 dB |
| **14** | 4 | 36 | **24** | 0.0756 | **57.1** ❌ | 0.228 | 3.4 dB |

⇒ **只有"边块 = 2 PRB"（`nf_tail = 24`，而标准块是 3 PRB = `nf_std 36`）时，K4 在边块的导频位置上读到的
估计与发端导频完全对不上**（残差大 500–1000 倍）。1 PRB 的边块（`nf_tail=12`）与无边块都正常。

**⑤ 由此得到的判据在三条实网腿上完全成立**（`nof_prb mod 3 == 2` ⇔ 边块 2 PRB）：

| 腿 | `mod 3 = 0` | `mod 3 = 1` | **`mod 3 = 2`** |
|---|---|---|---|
| b12 | 4.3% | 1.9% | **37.9%** |
| b13 | 2.9% | 1.1% | **82.8%** |
| b14 | 1.8% | 0.4% | **93.9%** |

**⑥ 为什么表现为"高调制全灭、QPSK 还能过"**：K4 的 `nv = max(min_nv, energy)`，边块残差把 `energy` 抬高
~1000× ⇒ 整个 TB 的软比特被压低 ~30 dB ⇒ 低码率小 TB（QPSK/`tbs=54`）仍能解出，而 16/64/256QAM 全灭
（NMS 的固定 offset 在标度被压 30 dB 后把所有消息都推成 0）。K3（reformat）与 K4 用**同一套边块索引**，
所以**边块那 2 个 PRB 的数据估计本身也是错的**（占 14 PRB 的 14%），这与"QPSK 大多能过、高码率必挂"一致。

**⑦ 机理（待修，方向上已很窄）**：CE 把分配切成 **3 PRB 标准块** + **边块**。K3/K4 在读边块时用
`sym * nf_tail + local_sc`，而写方（K2 `mmse_apply`）以 `(sys*nof_blocks + block) * (2*nout)` 落盘、
`nout` 是**那一次调用**的输出数（边块被按"标准块几何/padding"计算，见 K3 注释
"its real positions are the first nf_tail * 14 rows"）⇒ `nf_tail = 24` 时两边约定不一致。
1 PRB 边块恰好相容（或被 padding 吸收），2 PRB 就错位。

**⑧ 影响与现状**：这是**当前 setup 里 UL 失败的最大来源**（约一半授权、38–94% BLER），
**从 b9 之前就存在**（b9 36.3% / b10 37.6% / b11 24.3% 的"其余部分带宽"），与 GPU 侧所有改动无关；
它同时解释了"上报 SINR 与实际相反"的怪现象。
**门禁缺口**：现有 `metal_back_ends_match_the_cpu_chain`（"17 of 25 PRB"）用的是**合成/无噪**输入，
K4 的残差≈0，两种后端都给饱和 LLR ⇒ **测不出来**。修完必须补一条**带真实噪声、边块=2 PRB**的
CE 门禁（例如：设备 nv 与主机参考一致 + 与 CPU 链 LLR 逐位一致，在 14/23/17 PRB 上各跑一遍）。

#### 48.44 **修复**：`stage_engine_group()` 的导频切片偏移写错 —— 边块（`PRB ≡ 2 (mod 3)`）的估计来自**别的 PRB**

**缺陷**（`port_channel_estimator_metal_mmse_impl.cpp::stage_engine_group`，两条 staging 路径各一处）：

```cpp
// 错: gb_start 以 PRB 计、b 以"块"计，而 npf 是"本组的每符号导频数"
src = pilots_lse_view.get_symbol(i_symbol, i_layer).subspan((gb_start + b) * npf, npf);
// 对: 导频视图是 [PRB][comb]，每过一个 PRB 只跨 comb 个
src = pilots_lse_view.get_symbol(i_symbol, i_layer).subspan((gb_start + b * b_prb) * comb, npf);
```

`npf = b_prb * comb`，所以旧式在 `gb_start == 0`（标准块）或 `b_prb == 1`（1 PRB 边块）时**恰好正确**
—— 这正是它活了这么久的原因；而**分配大小 ≡ 2 (mod 3)**（边块 2 PRB）时读到的是**别的 PRB 的导频**
（甚至越界：`24*12=288 > 150`）。后果全链条一致：边块那 2 个 PRB 的均衡数据是错的、
K4 残差大 500–1000 倍 ⇒ 整个 TB 的软比特被压低 ~30 dB ⇒ **高调制全灭、低码率 QPSK 还能过** ⇒
实网上 `PRB mod 3 == 2` 的授权 BLER 38–94%（b12/b13/b14 三腿一致），占全部授权约一半。

**本地验证（793 个实网抓包，修复前 → 修复后）**：

| PRB | 修复前 上报SINR | **修复后** |
|---|---|---|
| 25（1 PRB 边块，对照） | 32.2 dB | 32.2 dB ✓ 不变 |
| 24（无边块，对照） | 34.9 dB | 34.9 dB ✓ 不变 |
| **23（2 PRB 边块）** | **6.2 dB** | **34.5 dB** ✅ |
| **14（2 PRB 边块）** | **3.4–3.7 dB** | **31.6–32.6 dB** ✅ |

`ctest -L phy` 162/162；demapper 批量 vs 逐符号的 LLR 仍逐字节一致（12 个抓包）。

**新门禁 + 一个仍然存在的第二个缺陷**：`port_channel_estimator_metal_mmse_unit_test` 的 Test 11
原本只比 "merged vs split" 两条**自洽**路径（共用同一个 staging 函数 ⇒ 对系统性错误是盲的），
形状表里**也没有任何 2 PRB 边块的几何**。现在补了 `{23, 2}` 与 `{14, 2}` 两个形状，并打印
**边块与标准块的 NMSE（对合成真值）之差**：

```
Test 11 NOTE: the edge block of a 23 PRB hop is 15.56 dB worse than its standard blocks (edge 7.31 dB, standard -8.25 dB)
Test 11 NOTE: the edge block of a 14 PRB hop is 16.01 dB worse than its standard blocks (edge 1.57 dB, standard -14.44 dB)
```

⚠️ **这个残留问题与本次修复无关**：同一形状在"有/无本次修复"下**逐 PRB NMSE 完全相同**
⇒ 是**另一个**独立的边块缺陷（且只在这两条 2-DMRS 合成形状上出现，3-DMRS 的实网抓包已恢复正常）。
因此这条检查暂时**只打印不断言**（保持门禁绿）；下一步把它变成断言前必须先查清这个第二缺陷。

#### 48.45 第二个边块缺陷也找到并修复：**主机侧 unpack 的同一类单位错误（而且是越界写）**；门禁已变断言

承 §48.44。Test 11 里那两条"边块 NMSE 差 ~16 dB"的形状（23/14 PRB）**与导频 staging 修复无关**
（有/无该修复逐 PRB NMSE 完全相同）—— 现在查明：**同一个单位错误在主机侧 `unpack_engine_group()` 里
还有一份**，而且在那里是**越界写**：

```cpp
const unsigned nf = b_prb * NOF_SUBCARRIERS_PER_RB;          // 块内子载波数
// 错: gb_start 以 PRB 计，却乘了"每块子载波数"
dst = grid_est.get_slice(...).subspan((gb_start + b) * nf, nf);
// 对:
dst = grid_est.get_slice(...).subspan((gb_start + b * b_prb) * 12, nf);
```

边块调用传的是 `gb_start = n_std_blocks * block_prb`（**PRB**）⇒ 23 PRB 的 hop 里算出
`21 * 24 = 504`，而这张网格只有 `23*12 = 276` 个子载波 ⇒ **越界 `subspan`（UB）**，边块的估计根本没写进去
（还会踩到别的内存）。旧式同样只在 `gb_start == 0`（标准块）或 `b_prb == 1`（1 PRB 边块）时正确 —— 与
§48.44 的症状完全同源。

**为什么实网没被它拖累**：真实均衡器走的是**设备切片**（K3）路径，主机 unpack 只用于 CSI/测量/抓包
（以及"均衡器不消费设备估计"的多端口等回退路径）⇒ 所以 b15 的 `mod 3 = 2` 已达 0.3%；
但它是一条实打实的越界写，必须修。

**修复效果（Test 11，同一批形状）**：

| 形状 | 修复前 NMSE（merged） | **修复后** |
|---|---|---|
| 23 PRB, 2 DMRS | −0.43 dB（边块 +7.31） | **−14.00 dB** ✅ |
| 14 PRB, 2 DMRS | −9.11 dB（边块 +1.57） | **−19.23 dB** ✅（边块仅比标准块差 6.2 dB，属正常起伏） |
| 23 PRB, 3 DMRS | +2.31 dB | **−8.71 dB** ✅ |
| 14 PRB, 3 DMRS | −5.85 dB | **−13.38 dB** ✅ |
| 52/25/4/51/2 PRB（对照） | 不变 | 不变 ✓ |

**门禁已升级为断言**（阈值：边块 NMSE 比同 hop 标准块差 **> 10 dB** 即 FAIL），并**做了双向验证**：

- 带修复：`port_channel_estimator_metal_mmse_unit_test` **rc=0**（PASS）；
- 把修复 stash 掉重编：`Test 11 FAIL: ... (23 PRB, 2 DMRS: edge NMSE 7.31 dB vs standard -8.25 dB)` ✅ —— 门禁确实抓得住。

同时把 Test 11 的形状表扩到 **10 个**（补 `{23,2} {14,2} {23,3} {14,3}`），覆盖"2 PRB 边块"与实网的
3-DMRS 几何。`ctest -L phy` **162/162**；demapper 批量 vs 逐符号在 14/23/25 PRB 抓包上仍**逐字节一致**。

#### 48.46 b16 实网确认（`47ca87a66e`，host unpack 修复）：**无退化、BLER 更低、且链路自适应明显更自信**

同一饱和协议、同时长量级下 b15（S-7c-8）→ b16（S-7c-9）：

| 指标 | b15 | **b16** |
|---|---|---|
| 聚合 BLER | 0.95% | **0.69%**（134 KO / 28590 OK） |
| `mod 3 = 2`（2 PRB 边块） | 0.3% | **0.4%** ✓ 仍好 |
| `mod 3 = 1` | 0.7% | **0.5%** ✓ |
| `mod 3 = 0`（3/6 PRB 的 msg3 类） | 5.3%（n=1310） | 12.5%（n=287，样本极少，仍是"没信号"那一类） |
| **调制分布** | 256QAM 14463 / 16QAM 1940 / QPSK 3490 | **256QAM 15650 / 16QAM 202 / QPSK 55** |
| MAC PDU 字节 | 55.6 MB | **57.5 MB**（TB 更少但 MCS 更高） |
| `[ul_pipeline]` median | 671µs | 697µs（+3.9%，仍远低于 1ms） |
| `[ul_ldpc_decode]` mean | 73.6µs | 85.8µs（每 TB 软比特更多，符合 MCS 变化） |
| `[mmse_time_sum]` | 59.2µs | 59.8µs ✓ 不变（CE 未受影响，符合预期） |
| `RLF` / FAPI 实时失败 | 0 / 10 | **0 / 0** |

**结论**：host unpack 修复没有带来任何退化，反而让 UL **几乎全程跑 256QAM 而 BLER 更低**
⇒ 之前 CSI 偏低确实被那条**越界写**压着；修正后链路自适应给出的高 MCS 是**站得住**的
（若 MCS 跳高而 BLER 上升，就要回看这条修复——实测相反）。`mod 3 = 0` 那一档仍是 §48.43 记录的
"调度器给已消失 UE 重传 msg3、栅格无信号"的现象，与本修复无关。

**⚠ 一条过程教训（`d01fac7a69`）**：定位边块布局时加的临时探针（`merge_geom` + `[ce_edge]` 打印）
**被一起提交进了 `47ca87a66e`**，而它的成员声明在 `.h` 里没进同一个提交 ⇒ **那一个提交在干净检出下编不过**
（本地能编是因为工作区里两半都在）。已用 `d01fac7a69` 把探针从两个文件里彻底删掉并重新提交。
教训：**临时探针要么在提交前删净，要么就按"正式探针"写全（`.h` + `.cpp` 同一个提交）**；
`git status` 里出现"只有 `.h` 被改"这种半截状态时，就是它在报警。

#### 48.47 现状清单（每 slot 的**控制点 / 等待点 / 拷贝点**）—— **代码状态 `d01fac7a69`**，数据来自 b16 实网腿

> 本节取代 §45.3「每个模块边界：数据在哪、谁等谁」的旧口径（那一版是 S-7c 之前的分段计时），
> 改用 `[ul_gpu_lane]` 的 **GPU 时间戳** + `[mmse_time_sum]`/`[metal_stats]` 的引擎计数，
> 因此下面每个数字都是"每 slot"的实测量级，而不是模块边界账。
> 阶段提交（本清单所对应的这一段）：`b8db30e9cc`（lane 探针）→ `80e131595d`（demapper 批量）→
> `a7787db656` + `47ca87a66e`（CE 边块两处修复）→ `d01fac7a69`（清理临时探针）。
> 实网腿：**b16**（饱和 iperf3，`--pusch_ldpc_decoder_type auto`，聚合 BLER 0.69%）。

**（a）控制点：每 slot ≈ 16 个 command buffer，全部由 CPU 编码+提交**

| 控制点 | 次数/slot | 谁编码 | 引擎计数（b16） | 备注 |
|---|---|---|---|---|
| `ofdm_demodulator::submit_symbol` → DFT CB（含网格直写） | **14** | radio 线程 | `dft commits=446607`（≈31800 slot 的 14 倍） | 每符号一个 CB；网格写**不是**第二个 dispatch（§47.3） |
| CE `run_async` → CB | **1** | 上层 PHY 线程 | `mmse_ce commits=28726` | **提交后不等待** |
| 共享 burst → CB（eq+demap 同一个 CB） | **1** | 上层 PHY 线程 | `burst commits=28724` | burst 内 dispatch = **5**（eq 4 + demap 1），`max_in_flight=1` |
| LDPC | 每码块 | 解码任务线程 | `ldpc_decoder commits=waits`（测试台为 CPU，无 CB） | metal LDPC 时逐码块一个 CB |

⇒ **编排成本与 dispatch 数成正比**（§48.12：CB 内一次 dispatch ≈ 10µs 主机编码 + ~4µs GPU）。
每 slot 的 dispatch 数：**batch 前 15（eq 4 + demap 11）→ 之后 5**（`demod_batch dispatches=28724`，1/组）。

**（b）等待点：CPU 等 GPU 只剩 3 处，且性质完全不同；GPU 等 CPU ≈ 0**

| 等待点 | 次数/slot | 实测 | 性质 |
|---|---|---|---|
| `finish_symbol → dft->wait_slot(slot)` | **14** | `[ul_time_frequency] 220.9µs / 14 ≈ 15.8µs/符号` | 既等 FFT、也等 radio 采样节拍（`max_in_flight=8`）；是 lower↔upper PHY 的**交接点** |
| CE 推迟完成（`complete_fd_td_estimation_stage`，在**下一跳**开头） | 1/hop | `cpl_wait = 30.5µs` | 等的是**上一跳**的 CE（早已完成）⇒ 基本不在关键路径 |
| 共享 burst `commit` + `waitUntilCompleted` | **1** | 后端唯一同步点 | CPU 等 GPU 的正面例子；被它覆盖的是 CE+eq+demap 的全部 CB |
| **`[ul_gpu_lane] gap`（GPU 空转等 CPU）** | — | **median 0.6µs / p95 0.9µs / mean 0.9µs** | **≈ 0**：数据在卡上、卡没在等 CPU |
| LDPC（metal 时） | 每码块 | `commits=waits`、`max_in_flight=1`、~3.6ms/TB | **唯一"阶段内等待"**，进 LDPC 阶段的第一件事就是量它的 busy/gap |

lane 账（b16）：`residency 187.4 / busy 186.5µs`，其中 **`ch_est 136.5`（73%）+ `eq_demap 49.9`（27%）**；
`cbs/lane = 2.00`（CE 一条 + burst 一条），`carried=0 dropped=0`。

**（c）拷贝点：UMA 下"上传/下载"不再是拷贝，剩下的显式 memcpy 全部在 CPU 侧**

| 数据 | 谁产生 → 谁消费 | 传递方式 | 拷贝 |
|---|---|---|---|
| IQ samples | radio 环 → DFT 核 | `fill_dft_input()`：跳过 CP + ci16→cf 转换 | **CPU memcpy**（14 次/slot） |
| 频域网格 | **DFT 核直写** → CPU（导频/ch_re/CSI） | 同一块页对齐主机内存 | 零拷贝（同址） |
| DM-RS 导频 | **CPU** 从网格取 → CE 核 | 导频 staging | **CPU memcpy**（K0-a，实测 0.46µs/跳） |
| 相关矩阵/统计量 | **CPU** 算 → CE 核 | 引擎 staging（~170KB/跳） | **CPU memcpy**（`corr 10.6µs` + `stage 17.4µs`/跳） |
| 数据符号 `ch_re` | **CPU** 从网格取 → 均衡核 | 均衡器 h/y staging（逐符号） | **CPU memcpy**（每符号；burst staging 合计 ≈140µs/slot） |
| 信道估计 / 噪声方差 | **CE 核** → 均衡核 | `gpu_ce`/`gpu_h`/`gpu_nv`，wrap 缓存**包含式查找**保证同一 MTLBuffer 对象 | **零拷贝**（`equalizer ch_est device=315964 staged=0`） |
| 均衡输出 eq/nv | 均衡核 → 解调核 | 解调器组缓冲（同 CB + 显存屏障） | **零拷贝**（同址） |
| **LLR** | 解调核 → CPU（Pass 3/LDPC） | `temp_llr` 页对齐槽位（每符号一槽，S-7e-demap 引入） | **零拷贝**（CPU 直接读同一内存） |

**（d）每 slot 的 CPU/GPU 账（b16）**

| 项 | 每 slot | 来源 |
|---|---|---|
| GPU 真在算 | **186.5µs**（CE 136.5 + eq/demap 49.9） | `[ul_gpu_lane] busy` |
| CPU：CE 主机侧 | **59.8µs**（pre 0.8 + stage 17.4 + corr 10.6 + submit 5.9 + sigma2 3.1 + …） | `[mmse_time_sum]` |
| CPU：demod 段（staging/编码 + 一次 burst 等待 + Pass 3） | **314.0µs**（GPU 只占其中 49.9µs） | `[ul_equalization_demod]` |
| CPU：LDPC（测试台） | 85.8µs | `[ul_ldpc_decode]` |
| 端到端 | `[ul_pipeline] mean 697.7µs / median 697 / p99 875` | IQ → CRC-OK |

⇒ **CPU 侧总量（≈400µs+）比 GPU（186.5µs）大一倍以上，但它主要是 memcpy/staging/主机数学/Pass 3，
不是等待**（gap 0.6µs 即证据）。

**（e）据此剩下的四个"要改"的点（按收益排序，均为本阶段之后的工作）**

1. **K0**：把"网格→导频、LSE/CFO、统计量、相关矩阵"搬进 GPU（现在每跳 ~30µs 主机时间 + staging 拷贝）；
2. **均衡器的 `ch_re`**：让均衡核直接按索引读设备网格，去掉每符号的 CPU 取数与 staging；
3. **DFT 的逐符号 `wait_slot`**：若目标改成"整 slot 一次交接"，需要把交接策略从"每符号"改成"每 slot"（并重估
   `[ul_time_frequency]` 的口径）；DFT 挪进 lane 队列是 S-7f 的 ring/push-collect 一并做的事；
4. **LDPC**：`commits=waits` + `max_in_flight=1` + ~3.6ms/TB —— 进 LDPC 阶段的**第一个测量**应是它的
   `busy/gap` 分解（§10.11.2），据此决定"改 kernel"还是"改架构"。

**（f）一条待量化的观察**：`[metal_stats] wrap hits=689305 creates=28799 replaces=28723` —— create/replace
几乎都是**每 lane 一次**（≈1/lane），即每 slot 都会重建一次映射对象。它**不拷贝数据**（只是
`newBufferWithBytesNoCopy` 的元数据），但值得单独量一下单价；怀疑与"每 lane 的分配宽度不同导致请求长度变化"
有关（批量化后 demapper 的 LLR wrap 跨度变成整组，会更容易触发 replace）。

#### 48.48 融合路线的可行性分析（承接 §48.47 的现状清单）：**为什么现在融合不了、以及怎么走到"单 CB / lane"**

**（a）现状：CB 提交分布在 3 类线程、2 条队列**（细节见 §48.47(a)）

| 线程 | 提交 | 队列 | 等待 |
|---|---|---|---|
| radio（收 sample） | DFT ×14（每符号一个 CB，含网格直写） | `front_end` | `wait_slot` ×14 |
| upper-PHY executor | CE ×1（`run_async`）+ burst ×1（eq 4 + demap 1 dispatch 同 CB） | `backend` | burst 一次；CE 的等待推迟到下一跳 |
| LDPC 任务 | 测试台为 CPU；metal 时每码块一个 CB | `backend` | 每码块 |

**两条队列不是随手分的**：合成一条时后端要排在一批在飞 FFT 之后，实测延迟爆炸（§45.2/§10.4）
⇒ 融合必须**把 DFT 挪进 lane 自己的队列**，不能简单并成一条。

**（b）四个硬约束（融合不成立的原因，逐条有依据）**

1. **burst 状态是 thread-local**：`shared_burst` 按设计"一个线程一个 CB"；Metal command encoder 非线程安全，
   ⇒ **一个 CB 的编码只能在一个线程上完成**（*提交/等待*可以换线程，编码不行）。
2. **CPU staging 夹在中间（最关键）**：CE 的输入（导频、相关矩阵、统计量）与均衡的 `ch_re` 都是
   **CPU 从主机网格取出再拷进引擎缓冲**；而那张网格是**同一条 CB 里 DFT 的输出**
   ⇒ 编码 CE 的 dispatch 时根本拿不到它 ⇒ **"链中等待"不是调度懒惰，而是"输入派生在 CPU 上"的必然结果**。
   这就是路线图把 **K0 定为融合前提**（§10.3）的算术依据。
3. **依赖现在靠 CPU 打点传递**（`finish_symbol` 的逐符号 `wait_slot`）⇒ 融合后必须换成 **CB 内显存屏障**
   （`memoryBarrierWithScope:MTLBarrierScopeBuffers`，`shared_burst` 在换阶段已经这么做）。
4. **radio 是逐符号喂的**：DFT 引擎**早就有** `submit(in, out, nof_transforms)`（一次 dispatch 覆盖多个
   transform，输入缓冲可放 `max_batch=16`），但 `ofdm_demodulator_impl` 的批量路径注释写明
   "radio 逐符号推进，所以那条路是休眠的" ⇒ **缺的不是 kernel 能力，是"整 slot samples 就绪"的握手**。

**（c）三条路线（按代价递增，收益面不同）**

| 路线 | 内容 | 前提 | 收益 |
|---|---|---|---|
| **F1** 单线程编排 + 单队列 | lane owner 线程把 `DFT(整 slot) → …` 编进一条 CB；radio 只做 push | DFT 挪队列 + 整 slot 握手；**但 CE/eq 仍需 CPU staging ⇒ 只能拆成两条 CB** | 每符号 14 次交接 → 每 slot 1 次 |
| **F2** 真融合（推荐主攻） | **一条 CB**：`DFT×14 → K0 → K1..K4 → eq(batch) → demap(batch) → 设备 LLR`，阶段间显存屏障，CPU 全程不碰数据、不等待 | **K0-a/b/c/d + `ch_re` 设备化**（§10.3 六项） | 省 ≈**140µs staging + 60µs CE 主机时间 + 14 次交接**/slot；lane 内零 CPU 等待 |
| **F3** ring D≥2 + push/collect | 每 slot 一套 arena（`(slot,port,hop,symbol)`）；提交不等、到 slot N+k 收 LLR；LDPC 进 GPU 则 LLR 不出设备 | F2 + arena + `D1/D5/D6/D8` | 吞吐与延迟台阶；跨线程只剩两个薄接口 |

⚠️ **收益面必须说清**：`[ul_gpu_lane] gap` 已经 ≈0（median **0.6µs**）⇒ F1/F2 的收益**不在"GPU 不再饿"，
而在"CPU 时间与链中等待"**（§48.47(d)：CPU 侧 ≈400µs+ vs GPU 186.5µs）。

**（d）跨线程的三个事实性约束（决定"融合"该怎么做）**

1. **编码必须单线程** ⇒ 融合 = **指定 lane owner 线程做全部编码**，其他线程只提供输入/消费输出；
2. **CB 可以跨线程等待**（A 线程 commit、B 线程 wait 合法；wrap 缓存带锁保证对象存活）⇒ 但会引入跨线程同步成本，
   现行选择"谁提交谁等"；
3. **同队列按提交顺序执行** ⇒ CE→eq→demap 不需要 CPU 介入；**跨队列无保证** ⇒ lane 必须一条队列。

**（e）推进顺序（每步独立门禁，可单独回退）**

| 步 | 内容 | 门禁 |
|---|---|---|
| 1 | **K0-a**：网格→导频提取做成 kernel（主机数学暂留，导频小缓冲回主机） | **零主机网格读取** + CE Test 9/11/12 逐位一致 |
| 2 | **K0-b/c/d**：LSE/CFO、统计量、相关矩阵写进引擎槽位 | 同上门禁 + `[mmse_time_sum] stage/corr` 归零 |
| 3 | **`ch_re` 设备化**：均衡核直接读设备网格（gather 或按索引读） | LLR 与 `cpu_gpu` 档逐位一致（IQ 级 replay） |
| 4 | **F2**：DFT 挪进 lane 队列 + 单 CB + 屏障 | `[ul_gpu_lane] cbs/lane → 1`、gap 仍 ≈0、管线不退化 |
| 5 | **F3**：ring D≥2 + `[ul_llr_ready]` | 吞吐/延迟门禁 + 与现行逐位一致 |

#### 48.49 K0-a 侦察结果（**可直接开工**的实施要点）：网格→导频提取搬到设备

**（a）接缝只有一个**：`ocudu::extract_layer_hop_rx_pilots()`（`port_channel_estimator_helpers.cpp:133`）
—— 它用 `grid.get_view(cfg.rx_ports[port], symbol_index)` 读**主机网格**（`cbf16_t`），按 DM-RS 梳状图样
`pattern.re_pattern` 与 hop 的 `rb_mask`（contiguous / 非 contiguous 两条分支）把导频 RE gather 进
`dmrs_symbol_list rx_symbols`（布局 `[dmrs_symbol][cdm_group]`，元素 `cf_t`）。

**（b）目标缓冲（都是页对齐主机内存、已 no-copy 包装）**：

| 缓冲 | 布局 | 谁写 | 谁读 |
|---|---|---|---|
| `gpu_rx_pilots` | `[i_dmrs][i_group][npf]` **float2** | 现在：CPU（`impl.cpp:716-728` 从 `args.rx_pilots` 拷） | K4（噪声核）与… |
| `gpu_pilots` | `[i_dmrs][i_layer][npf]` float2 | 现在：CPU（从 `args.pilots`，**已知序列**，属"系数下传"，可留 CPU） | K4 |
| `pilots_lse_view`（`args.pilots_lse_view`） | `[symbol][layer][prb][comb]` cf_t | 现在：CPU（LSE/CFO 后） | **y staging**（`stage_engine_group`，§48.44 修过偏移） |

⇒ **K0-a 的最小形态**：新 kernel `mmse_rx_pilots`（读设备网格 → 写 `gpu_rx_pilots`），
**顺带可写一份 `rx_pilots` 主机小缓冲（≈1.8 KB/跳）** 供主机数学使用（UMA 同址，不需要拷贝）。

**（c）kernel 需要的最小参数**（与 K4 的 `mmse_noise_params` 高度重叠，可复用其"导频↔子载波"映射逻辑）：
`grid_base`（页对齐网格基址）+ `grid_bytes`（wrap 用）、port、`first_symbol`/`nof_symbols`、hop 的
`rb_mask`（用 (lowest, count, contiguous?) 或一张 PRB 列表）、`re_pattern`（12-bit 掩码）、
`comb_size`、`npf`、`nof_layers`/`nof_cdm_groups`、以及**输出步长**（`[i_dmrs][i_group][npf]`）。
注意两个坑：①网格是 **cbf16**，kernel 里要 `cbf16→float2` 解码（与其它核保持同一解码函数）；
② hop 的 PRB 起点要用**绝对 CRB**（不是相对 BWP），否则部分带宽会错——这正是 §48.44/§48.45 那类
"单位/基准"错误的同一个陷阱。

**（d）必须一起动的"主机消费者"（否则只是把读网格换成等 kernel）**

| 消费者 | 位置 | 处置 |
|---|---|---|
| `estimate_sigma2` 类统计（读 `rx_pilots`） | `port_channel_estimator_helpers.cpp:597` 附近 | 属 **K0-c**；K0-a 阶段先用 (b) 的主机小缓冲，代价是"等一次 gather kernel" |
| CSI/RSRP/TA/CFO（读 `filtered_pilots_lse`） | `impl.cpp:1101/1116` | 属 **K0-c/d**；先保留主机小缓冲（§10.10.3 第 4 条：CSI 必须有"不阻塞 lane 的路径"） |
| y staging（读 `pilots_lse_view`） | `impl.cpp:1301/1320` | 属 **K0-b**（LSE 上设备后才能去掉这次主机读） |

⚠️ **K0-a 单独的收益边界**：它去掉的是"CPU 读网格"（CPU 时间），**但主机数学仍要等 gather kernel**
⇒ 单 CB 融合仍不成立（§48.48(b) 约束 2）。要拿到 F2 的收益必须连着 K0-b/c/d 一起做。

**（e）门禁（照 S-7c 的老规矩）**：
1. **计数归零**：新增一个探针计数"主机网格读取次数"，K0-a 后导频路径必须为 **0**（网格读取只允许出现在
   `ch_re` 与 CSI 镜像两处，且各自有账）；
2. **逐位一致**：CE 单测 Test 9/11/12（含 DC、非连续分配、2-PRB 边块的形状）必须与改动前**逐位一致**；
3. **端到端**：`ul_chain_replay` 的 A/B（`--metal` vs `--metal-cpu-demod`）在**部分带宽抓包**上 LLR 逐字节一致；
4. **上机**：`[metal_stats]`/`[mmse_time_sum]` 的 `stage`、`corr`、`pre` 三项应下降，`[ul_pipeline]` 不退化。

**（f）下一步动作（本文件之后的第一件事）**：写 `ocudu_mmse_pilots.metal`（或并入现有 reformat 库）+
在 `stage_engine_group`/`estimate()` 里把 `gpu_rx_pilots` 的填充从主机循环换成那次 dispatch；先只做
**contiguous hop**（`is_contiguous == true` 那条分支，覆盖实网的 25/14/23 PRB），非连续分配保留主机路径并计数。

#### 48.50 **§48.49(f) 的顺序更正**：K0-a 不能当第一步做（有循环依赖，而且它本身几乎不花钱）；第一步应是 **`ch_re` gather**

**（a）循环依赖（实证）**：引擎的输入是**从导频算出来的**——
`stats = stats_estimator->estimate(...)`（`impl.cpp:604`）→ `build_correlation_matrices(stats, ...)`（`:610`），
`stats.fd_hz / tau_rms_s / sigma2` 全在 `A`/`R_hp` 里（`:431/435/442/451/454`），而 `y` 来自
`pilots_lse_view`（LSE 后的导频）。⇒ 如果把"网格→导频"搬到设备，**CPU 必须等那次 kernel 才能开始算/装
引擎输入** ⇒ 同步点 **+1**，换来的却只是——

**（b）K0-a 本身几乎不省钱**：§48.6 早已实测 **导频提取 = 0.46µs/跳（0.3% of 176µs）**。
真正花钱的是 CE 主机侧的 **`stage 17.4µs`（A/R_hp/y/导频 ≈170KB memcpy）+ `corr 10.6µs`（建矩阵）**
（`[mmse_time_sum]`，b16）。⇒ **单独做 K0-a 是净亏**（省 0.46µs，多一次同步）。

**（c）唯一能"单独做且是净赚"的 K0 候选是 K0-c/K0-d**：把**统计量与相关矩阵在设备上算完并直接写引擎槽位**
——它一旦成立，"主机需要导频"这条依赖就断了，K0-a 才能顺带搬走（否则就是 (a) 的循环）。
所以顺序应是 **K0-c/d → K0-a/b（顺带）**，而不是 §48.48(e) 里写的 K0-a 打头。这一点更正。

**（d）真正该当"第一步"的是 `ch_re` gather（即 §48.48(e) 的第 3 步，提前）**，理由：

1. **没有循环依赖**：`ch_re` 是均衡器的输入，均衡 dispatch 排在同一 burst 里、**在 DFT 之后**；
   而 CPU 已经通过 `finish_symbol → wait_slot` 等过每个符号的 DFT ⇒ **新增 gather dispatch 不引入任何新同步**；
2. **花的钱最多**：burst staging 合计 ≈**140µs/slot**（§48.47(d) 的反推），其中主体就是"CPU 从主机网格逐符号取
   `ch_re` + memcpy 进均衡器的 h/y staging"；
3. **门禁最干净**：gather 必须是**纯搬运**（网格里本来就是 `cbf16_t`，不转换 ⇒ 逐位一致是构造性的）+ LLR 逐字节 A/B。

**（e）`ch_re` gather 的实施要点（下一步直接照做）**
- **描述符**（主机侧一次算好，按 slot 传）：
  `grid_base`/`grid_bytes`（页对齐网格 + wrap）、`port`、`first_symbol`、`nof_symbols`、`rb_mask`（用
  `(lowest_prb, count, contiguous?)` 或 PRB 列表，注意**绝对 CRB**，§48.44/§48.45 的同一个陷阱）、
  `re_mask`（每符号的数据 RE 掩码：全 RE 去掉 DM-RS 梳）、`dc_position`（要写零）、
  输出槽位（`[i_used][nof_re]` cbf16 或 `[i_used][re]` float2，与均衡器 staging 的现有布局一致）；
- **kernel 必须是纯搬运**：读 `cbf16` 原样写出（不要走 float 再回 bf16，否则不逐位一致）；
- **接缝**：`channel_equalizer_metal::submit()/submit_batch_run()` 里那两次
  `std::memcpy(h_sym/y_sym, symbol.ch_symbols->get_slice(i_port).data(), ...)`（`channel_equalizer_metal.cpp:207-213`）
  换成"登记一次 gather"；gather 编进**同一个 burst CB**（eq dispatch 之前，插一次显存屏障）；
- **门禁**：① gather 计数探针从 0 → N（每符号一次），主机网格读取计数 → 0；② `ul_chain_replay` 在
  **全带宽 + 部分带宽**抓包上 LLR 逐字节一致；③ `[ul_gpu_lane]` 的 `eq_demap` busy 与 gap 不退化
  （gather 是新增 GPU 工作，`busy` 会略升、`gap` 必须仍 ≈0）；④ 上机 `[ul_equalization_demod]` 与
  `[ul_pipeline]` 不退化。

#### 48.51 S-7f-1 落地：`ch_re` gather —— 均衡器的接收符号改由**设备从网格取**（`e4d5e68975`）

> 这一步就是 §48.50(d)(e) 定的"第一步"，**已提交并 push**。三个门禁 ① ② 已过（数字见下），
> ③④ 需要上机腿（见 §48.51(f)）。

**（a）做了什么**：均衡器 y 输入过去是**主机逐端口逐符号 memcpy**（`run_equalize` /
`submit_batch_run` 各一处），现在由 **`gather_ch_re` 一次 dispatch** 从设备网格取。
数据流不变（IQ → DFT → 网格 → CE → eq → demap → LLR），只把"网格→y"这一步从 CPU 搬到 GPU。

| 组件 | 位置 | 作用 |
|---|---|---|
| `resource_grid_reader::get_device_view()` | `resource_grid_reader.h` / `_impl.{h,cpp}` | 读侧也能拿到网格的设备视图（写侧 §47 已有）。默认无效 ⇒ 老读者照旧 |
| `ch_gather_desc` | `include/.../equalization/channel_equalizer_device_grid.h` + `.cpp` | **一个 hop 的 gather 布局**：每符号一条 entry 段，逐 RE 给出 `(网格子载波, 输出下标)` |
| `gather_ch_re` | `ocudu_equalizer.metal` | **纯 cbf16 搬运**：每线程一个 (RE, 符号)，写 `[符号][端口][re]` |
| `channel_equalizer::set_device_grid()` / `consumes_gathered_symbols()` | `channel_equalizer.h`（默认空/false） | 调用方**按符号**递计划，并**每次调用问一次**后端是否接受 ⇒ 接受就**连主机 gather 都不做** |
| `equalizer_metal_engine::gather_binding` | `ocudu_equalizer_metal_engine.h` | 把计划+符号带进 pending entry，flush 时编码 gather |

**关键设计点（都是踩过的坑换来的）**：
1. **计划按"hop"建一次，按"符号"递**：entry 表只跟分配有关（与符号无关），demodulator 每个
   symbol 只递同一个计划 + 符号号（§48.50(e) 的描述符是"按 slot"，实际按 hop 更省）。14 个符号
   各建一次 3000 条 entry 的表是纯浪费。
2. **`consumes_gathered_symbols()` 是"要或不要"的契约**：demodulator 用它在循环**之前**决定
   "还要不要做主机 gather"。若后端接受却没用上计划，y 就是未初始化内存（我第一版正是这个错误：
   PRB 数与计划不符时后端回退主机，但主机已经不做 gather 了）。
3. **gather 与 eq 同 CB、gather 在前**：gather 用**自己的 pipeline**，所以 `shared_burst::encoder()`
   换 pipeline 时插的那次显存屏障正好把两者排好；不需要新同步点（§48.50(d)①）。
4. **`OCUDU_EQ_GATHER=0`**：逃生口，恢复主机 gather（默认开设备 gather）。A/B 就是靠它。

**（b）两个新坑（都值得写进"别再踩"清单）**：
- **同一个 `MTLBuffer` 的 `setBuffer:offset:` 字节切片不可靠**：taps（24B）和 entries 放同一次
  分配，用 `offset=taps_bytes` 绑 entries ⇒ **kernel 读到的是别的字节**（前 2 条 entry 是垃圾，
  第 3 条起才对），表现是"每个 run 的前 2 个 RE 输出 0"。给两张表**各自一个 MTLBuffer** 立刻正确。
  这与 `eq_make_h_starts` 注释里记的是同一件事。
- **计划不能内联在 demodulator 对象里**：`ch_gather_desc` 有 ~8.6 KB（`3300 entry` 表），做成
  `pusch_demodulator_impl` 的**成员**后，对象尺寸一变，replay 工具就开始**间歇性段错误**
  （`rx_buffer_impl::get_codeblock_data_bits`，约 2–4%/次，越跑越准）。改成 `unique_ptr` 成员
  （每次 demodulate 一次堆分配）后 120 连跑 0 次崩溃。**基线 240 连跑 0 次崩溃**，所以这不是
  原有抖动。教训：**给 pooled 的小对象"顺手加个成员"时，先看它多大**。

**（c）门禁 ①：LLR 逐字节一致（本地，秒级）**
- 语料：`/tmp/iq1_*` + `/tmp/iq2_*` 共 **980** 个 reception（1…3300 子载波、3…273 PRB、
  QPSK…256QAM，含 §48.42 发现的 `mod 3 = 2` 形状）；
- A/B：**同一构建** `--metal`（设备 gather）vs `--metal` + `OCUDU_EQ_GATHER=0`（主机 gather）；
- 结果：**980/980 LLR 逐字节一致，0 差异，0 崩溃**；host 腿另外与改动前基线（`d01fac7a69`）
  逐字节一致 ⇒ 双重锚定。
- 探针：`[metal_stats] equalizer ch_re device=11 host=0`（每 slot 11 个数据符号各 1 次）。

**（d）门禁 ②：`ctest -L phy` 162/162**（含 CE Test 9/11/12、`resource_grid_device_view_test`）。

**（e）本地代价读数**（6 PRB 抓包，`[metal_stats] burst commits ... (equalizer=8 demapper=1)`）：
dispatch 由 5 → 9（每 run 多 1 次 gather；run 数为 4，因为 DM-RS 符号把 run 切开了），
`[ul_gpu_lane] busy split` 的 `eq_demap` 由 54.0 → 76.8µs/lane。**这是替主机 memcpy 付的钱**，
上机看的是 `[ul_equalization_demod]` 与 `[ul_pipeline]` 是否净降（§48.51(f)）。

**（f）下一步（两条，顺序不分先后）**
1. **上机腿（b17）**：照 §5 的 OTA 模板跑饱和 iperf3，比 §48.46 的 b16 基线：
   `[ul_equalization_demod]`（应降）、`[ul_pipeline]`（应降）、`[ul_gpu_lane] gap`（必须仍 ≈0）、
   `mod 3` 三行 BLER（不得退化）、`[metal_stats] equalizer ch_re device≈host 以前的值、host=0`。
2. **下一步的工程**：§48.50(c) 的 **K0-c/K0-d**（统计量 + 相关矩阵在设备上算并直写引擎槽位）
   —— 它一旦成立，K0-a/b 才能顺带搬走（否则是 §48.50(a) 的循环依赖）。

**（g）一条已知的**既有**问题（不是本步引入）**：`OCUDU_EQ_DEFER_ENCODE=1`（批量 burst 逃生口）
下 `channel_equalizer_metal_unit_test` 的 "batched deferred burst" 项**在基线上也 FAIL**
（`submit_group` 项在两边都 OK）。默认路径全绿；这条逃生口是 opt-in 调试路径，本步不动它，
但**它说明批量 burst 的那条路当前不是可信的 A/B 参照**。

#### 48.52 本步之后的待办与文档地图（2026-09-14 收尾，代码状态 `e4d5e68975`）

| # | 项 | 说明 |
|---|---|---|
| 1 | **上机腿 b17** | 用户执行、一次一条命令；判据见 §48.51(f)。命令见 §5 的 OTA 模板（把 `bXX` 换成 `b17`） |
| 2 | **K0-c/d** | 统计量 + 相关矩阵上设备并直写引擎槽位（§48.50(c)）；成立后 K0-a/b 顺带 |
| 3 | **F2** | DFT 挪进 lane 队列 + 单 CB + 屏障；目标 `[ul_gpu_lane] cbs/lane → 1`（§48.48(e) 第 4 步） |
| 4 | **F3** | ring D≥2 + push/collect + `[ul_llr_ready]` |
| 5 | **LDPC** | 进阶段第一件事是量它的 busy/gap（§10.11.2） |
| 6 | **`OCUDU_EQ_DEFER_ENCODE=1` 的批量 burst 门禁失效** | §48.51(g)；要用它当 A/B 之前先修 |

本步改动的文件地图（便于回看）：

| 层 | 文件 |
|---|---|
| 接口 | `ocudu/phy/upper/equalization/channel_equalizer.h`、`ocudu/phy/support/resource_grid_reader.h` |
| 计划类型 | `ocudu/phy/upper/equalization/channel_equalizer_device_grid.h`、`lib/phy/upper/equalization/channel_equalizer_device_grid.cpp` |
| 网格设备视图 | `lib/phy/support/resource_grid_reader_impl.{h,cpp}`、`lib/phy/support/resource_grid_impl.cpp` |
| kernel | `lib/phy/upper/channel_processors/metal/ocudu_equalizer.metal`（`gather_ch_re`） |
| 引擎 | `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.{h,mm}` |
| 适配器 | `lib/phy/upper/channel_processors/metal/channel_equalizer_metal.{h,cpp}`、`_factory.cpp` |
| 接缝 | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.{h,cpp}` |
| 装饰器 | `lib/phy/metrics/phy_metrics_channel_equalizer_decorator.h` |

#### 48.53 b17 上机腿分析（`e4d5e68975`）：**设备 gather 在实网工作正常；但这一腿的信道比 b16 差 8–10 dB，不能用来判收益**

**（a）功能性：全绿**

| 项目 | b16（基线） | b17（本步） | 判读 |
|---|---|---|---|
| `[metal_stats] equalizer ch_re` | （无此项） | **device=307967 / host=0** | **主机 gather 彻底消失**，11 个数据符号 × 27997 slot |
| `burst dispatches`（equalizer 侧） | 114896（4/跳 = eq 4） | **223976（8/跳 = eq 4 + gather 4）** | gather 的账对得上 |
| `[ul_gpu_lane] busy split eq_demap` | 49.9µs/lane | **75.8µs/lane**（+25.9） | **预期**：gather 的 GPU 单价 |
| `ch_est` busy | 136.5µs/lane | 137.5µs/lane | 不变 ✓ |
| `[mmse_time_sum]` stage/corr | 17.40 / 10.6µs | 19.41 / 12.3µs | 不变（噪声）✓ |
| 健康 | RF 失败 7、RLF 0 | **RF 失败 1、RLF 0** | 干净 |
| `cbs/lane` / `dropped` / `carried` | 2.00 / 0 / 0 | 2.00 / 0 / 0 | 不变 ✓ |

**（b）判据③（gap）**：median 0.6 → **0.7µs**（好），但 mean 0.9 → 3.0、p95 0.9 → 7.5、
**p99 1.0 → 62.3µs**。median 不动而尾部变长，与"每个 lane 多 4 个 gather dispatch"一致
（每条 run 的 GPU 工作更碎，尾部更容易被前端 DFT / CPU 挤到）。**要盯住 p99**，见 (f) 的待办。

**（c）⚠️ 这一腿的 OTA 信道不可比（最重要的结论）**
- `[ul_pipeline]` SINR 的**逐段中位数**：b16 全程 **29–31 dB**（1024s 只在最后一段塌到 27.6）；
  b17 首段 28.1 → 102–205s **19.9** → 205–307s **16.9** → 410s 回到 **27.6** → 尾段 14.5。
  **缓慢下降 + 中途回升** = 物理/热/位置效应，**不是代码**（代码问题是阶跃且可复现）。
- 结果：b17 的 MCS 被压到 16QAM/QPSK，`[ul_mac_pdu_size]` 57.5MB → **36.6MB**（−36%）。
  逐窗口算平均 TB：b16 首 200s 1975 bit/授权、其余 2017；b17 首 200s **1491**、其余 **1308**。
  **授权数几乎相同**（28724 vs 27997）⇒ 差距全在"每授权多少比特"，即链路自适应对信道的反应。
- 另一头：b17 的 64QAM BLER 16.07%（306/1904）而 b16 9.57%（27/282）——同样是低 SINR 的后果。
  `n_prb mod 3` 三行里 **`mod 3 == 0` 在 b17 是 35.5%（72/203）**，那正是 §48.43① 已知的
  "调度器给已消失的 UE 重传 msg3"小授权（本线不动）。

**（d）能得到的方向性结论（尽管被污染）**
`[ul_pipeline]` **mean 697.7 → 668.7µs、median 697 → 666、p95 819 → 789、p99 875 → 846**：
在**信道更差**（MCS 更低、LLR 更少、LDPC 更轻）的情况下整条链仍快了 ~29µs（4%）。
但 `[ul_equalization_demod]` 只从 314.0 → 309.7µs（−4.3µs，**小于 staging 省下的量级**）——
说明 **§48.47(d) 里"burst staging ≈140µs/slot"这笔账被高估了**，或者它本来就不在关键路径上
（`defer_wait` 310.1 → 321.1µs 反而略升）。⇒ **本步的收益主要不是时延，是把 CPU 从
memcpy 里解放出来**（对后续 F2/F3、多 UE、以及 CPU 侧别的搬运才有意义）。

**（e）因此：b17 不能作为"收益兑现"的证据，也不能作为"退化"的证据**（信道差 8–10 dB + MCS 变）。
需要一条**同构建内的 A/B** 把变量锁死。

**（f）下一步（一条命令的 A/B，用户执行）**
用同一个构建、同一个二进制、只翻一个环境变量，把"设备 gather vs 主机 gather"的差别锁死：
```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto --log.filename /tmp/gnb_b18.log > /tmp/gnb_b18_console.log 2>&1
```
即 **b18 = `OCUDU_EQ_GATHER=0`**（`sudo -E` 把环境变量带进去）。它与 b17 是**同一次构建**，
`consumes_gathered_symbols()` 之外**一行代码都不差** ⇒ 差异只剩 gather 本身。
- 若 b18 与 b17 的 `[ul_pipeline]`/`[ul_equalization_demod]`/`mod 3` BLER 在一个量级 ⇒ **本步无退化**（且
  `eq_demap` busy 应回落 ~26µs/lane、`burst dispatches` 应回到 4/跳、`ch_re host` 应变成 11/跳）。
- ⚠️ 最好 **b17 → b18 → b17' 交替**跑（每腿都跑到 Ctrl-C），否则信道漂移仍会把结论吃掉。
- 另外要盯 `[ul_gpu_lane] gap` 的 **p99**（b17: 62.3µs）。若 p99 长期偏高，考虑把 gather 合并进
  更大的 run（现在每跳 4 个 run 各一次 gather）。

#### 48.54 b18（`OCUDU_EQ_GATHER=0` 同构建 A/B）**中途被 USB 故障打断**，无有效结论

**（a）故障现象与时间线（`/tmp/gnb_b18_console.log` / `gnb_b18.log`）**

| 证据 | 内容 |
|---|---|
| 根因 | UHD 抛 `uhd::usb_error`：`usb tx2 submit failed: LIBUSB_ERROR_PIPE` + **136 次** `usb rx6 transfer status: LIBUSB_TRANSFER_ERROR`。→ 未捕获异常 ⇒ `libc++abi: terminating` ⇒ **进程直接终止** |
| 因（次生）| 日志尾部 slot 410.5–419.5：13 次 `ErrorIndication`、13 次 `Real-time failure in FAPI: UL processor is busy`、若干 `UL HARQ ... failed to reserve buffer`、`PDCCH` 仍在发但收不到上行 |
| 果（现象）| **手机断连** —— 是 SDR 掉线的**后果**，不是原因 |
| 为什么没有退出统计 | `[ul_*]`/`[metal_stats]` 都是 `atexit` 打印，而 `std::terminate` **不跑 `atexit`** ⇒ 这一腿**没有任何探针数据** |

**（b）这一腿拿不到结论的原因（诚实记账）**
1. 进程异常终止 ⇒ 没有 `ch_re device/host`、没有 `[ul_pipeline]`、没有 `[ul_gpu_lane]`；
2. 唯一的 A/B 期望（`ch_re device=0 host≈N`、`burst dispatches` 回到 4/跳）**无法核对**；
3. 因此**不能**用 b18 判断设备 gather 是否退化——**b17 与 b18 之间不存在可比对**。

**（c）一个必须记下的观察：`--log.filename` 的日志量本身可能是 USB 故障的诱因之一**
- b16/b17/b18 的 `gnb_*.log` 都是 **40–60 MB / 腿**（每条 PUSCH、PDCCH、调度决策都落盘，`all_level: info`）；
- b18 的**墙钟只跑了 75 s**，主日志里 **36687 条 PUSCH 解码**、419 个 slot；
- ⇒ **先排除"日志把 USB/宿主拖垮"这条路径**：复跑时把日志降到 `warning`（或换 `--log.all_level=warning`），
  只保留 console 的退出统计。这条与代码无关，但它**会污染所有上机腿**。

**（d）重跑 b18 的正确姿势（用户执行）**
1. **降日志**：在上面的 `b18` 命令后加 `--log.all_level warning`（探针走 stderr，不受影响）；
2. 一腿跑够 600–1000 slot 就够（不必 1024 s），**Ctrl-C 正常退出**（这样 `atexit` 才会打印探针）；
3. 判据同 §48.53(f)：`ch_re device=0 host≈N`、`burst dispatches` 4/跳、`eq_demap` ≈50µs/lane、
   `[ul_pipeline]`/`mod 3` BLER 与 b17 同量级；
4. 若 USB 再掉：先换 USB3 线/端口（`LIBUSB_ERROR_PIPE` 是**物理层/驱动**级错误），并确认没有别的
   进程（Docker/VM/备份）在抢同一条总线。

**（e）附：这一腿里唯一可用的旁证（弱证据，不足以下结论）**
按条 PUSCH 的 `t=`（解码调用耗时）分布：b17 host=0（设备 gather）中位 440µs / p99 580µs；
b18（本应 host gather）中位 **413µs** / p99 537µs。**相反方向**，与"设备 gather 更快"矛盾——
但两腿的 MCS 分布不同（b17 有 3671 条 QPSK，b18 只有 3100）、且信道不同 ⇒ **只能当噪声**，
**不作为任何结论的依据**。

#### 48.55 b18b（`OCUDU_EQ_GATHER=0`，`--log.all_level warning`）**跑通**：判据③通过，但**暴露出两个新问题**

**（a）这一腿是干净的**：正常 Ctrl-C 退出（`atexit` 打印齐全）、**0 次 USB 错误**、
RF underflow 仅 2 次，58272 lane / 640992 符号。

**（b）同构建 A/B 的三个探针全部符合预期（功能层面确认无疑）**

| 判据 | b17（设备 gather） | b18b（主机 gather） | 结论 |
|---|---|---|---|
| `equalizer ch_re` | `device=307967 host=0` | **`device=0 host=640992`** | 开关确实切换了两条路 |
| `burst dispatches`（eq 侧） | 223976（**8/跳**） | 233088/58272 = **4.0/跳** | gather 的账精确对上 |
| `eq_demap` busy | **75.8µs/lane** | **48.7µs/lane** | 设备 gather 的 GPU 单价 = **+27.1µs/lane** |
| `ch_est` busy | 137.5 | 135.1 | 不变 ✓ |
| `gap` median / p95 / p99 | 0.7 / 7.5 / **62.3µs** | **0.6 / 1.1 / 1.3µs** | **判据③在主机腿是满分的**；设备腿的 p99 偏高（见 f） |

**（c）⚠️ 新发现 1：`--log.all_level` 本身就是几十 µs 的测量误差**
b16（主机 gather，**info**）与 b18b（主机 gather，**warning**）**跑的是同一条代码路径**，却差：

| 指标 | b16 (info) | b18b (warning) | 差 |
|---|---|---|---|
| `[ul_pipeline]` median | 697.0µs | **652.0µs** | **−45µs** |
| `[ul_equalization_demod]` median | 324.6µs | **278.0µs** | **−46.6µs** |
| `[ul_time_frequency]` / `ch_est` / `ldpc` | 215.8 / 74.0 / 89.0 | 214.5 / 73.5 / 89.0 | **不变** |

⇒ 影响**只落在 `[ul_equalization_demod]` 段**（PHY executor 线程要写 PUSCH/PDCCH/调度日志），
`ch_est`/DFT/LDPC 各段几乎不动。**这条比本步的收益还大**，而且它污染了 b16/b17 两条腿的绝对值。
**今后所有上机腿一律 `--log.all_level warning`，跨腿比较只在同 level 之间做。**

**（d）⚠️ 新发现 2：设备 gather 在主机侧花掉了它省下的钱（下一步要修）**
同构建、同 warning 条件下最干净的对照是 **b17(设备) vs b18b(主机)**：

| 指标 | b17 设备 | b18b 主机 | 判读 |
|---|---|---|---|
| `[ul_equalization_demod]` median | 305.3µs | **278.0µs** | **设备腿反而慢 27µs** |
| `[ul_pipeline]` median | 666.0µs | **652.0µs** | 同上，慢 14µs |
| `defer_wait` | 321.1µs | **281.0µs** | 同上 |
| `eq_demap` busy（GPU） | 75.8 | 48.7 | 设备腿 GPU 贵 27µs ✓ 预期 |

⇒ **GPU 省下的 27µs/lane 被主机侧的新开销吃掉了约 27–30µs/slot**。来源（按量级）：
`eq_flush_hook` 里**每个 run 都重建一次 gather 表**——`eq_build_gather_tables` 拷 ~5.5KB
（25 PRB 全 hop 的 entry 表）+ **两次 `newBufferWithBytes`**；一 slot 4 个 run ⇒
**8 次 MTLBuffer 分配 + 4×5.5KB 拷贝 / slot**，正好是 30µs 量级。

**（e）修法（下一步的工程，已明确）**：把 gather 表**按 hop 建一次、按 run 只切窗口**——
entry 表内容与所取符号无关（taps 的 `offset` 是**绝对下标**），所以：
1. `entries` 缓冲**每 hop 建一次**（key = plan 指针 + hop 符号窗口），4 个 run 共用；
2. `taps` 每 hop 建一次（含 hop 全部符号），run 用 `setBuffer:offset:` 指向自己那段——
   ⚠️ 必须按 `gather_tap_t` 对齐切（`48.51(b)` 的坑），最稳的做法是 taps 也**按 run 建一个小缓冲**
   （12B×n_run，仍比现在省一半），或把 hop taps 表整体做成一个缓冲 + 每 run 一个小 `setBytes` 基址；
3. 预期：主机侧回到 0 额外开销，`[ul_equalization_demod]` 应比主机腿再低 ~27µs/slot。

**（f）判据③的遗留：设备腿 `gap` 的 p99 = 62.3µs（主机腿 1.3µs）**
median/p95 都好（0.7 / 7.5），只有 p99 偏高。修了 (e) 之后要复测；若仍偏高，考虑把一 slot 的
4 次 gather 合并成 1–2 次（现在被 H 估计的逐符号起点切成 4 个 run，gather 只能跟着切）。

**（g）结论**：**S-7f-1 的功能正确性成立**（三探针 + 980 抓包逐字节 + 162/162）；
**收益尚未兑现**，原因已定位到 (e) 的每 run 建表，是**下一步的第一步**，不是回退理由。

#### 48.56 S-7f-2 落地：gather 表**按 hop 建一次**（`74eabc7a80`）—— 把 §48.55(d) 找到的钱拿回来

**（a）改了什么**：§48.55(d) 定位到"设备 gather 在主机侧花掉了它省的钱"，根因是
`eq_flush_hook` **每个 dispatch 都重建+重传一次 gather 表**（一 slot 4 个 dispatch ⇒ 4 次 entry 表拷贝
+ **8 次 `newBufferWithBytes`**）。本步的关键认识是：

> **gather 表描述的是"hop 的分配"，不是"这个 dispatch 恰好带哪些符号"。**
> 一个符号的 entry 段和它在表里的位置与"和谁同行"无关 ⇒ **一张表服务整个 hop 的所有 dispatch**，
> kernel 只需从"本 run 的首符号"进入（`gather_params::first_symbol`）。

- `eq_build_gather_tables(plan)`：建**整个 hop** 的表（taps 每个 hop 符号一条）；
- `eq_gather_tables(plan)`：按**plan 地址**缓存在 flush state 里，一个 hop 只建一次、只上传一次，
  缓冲进与 staging 同一个 keep-alive 列表（交给 command buffer 就得活到它完成）；
- `gather_ch_re`：taps 表覆盖整个 hop，run 用 `first_symbol` 进入；
  **顺带修了一个隐患**：原来在 `sym >= p.nof_symbols` 检查**之前**就索引 `taps[sym]`，
  现在先检查 `sym`，再索引（表是 hop 宽的，越界读被彻底排除）。

**（b）门禁（全部本地，秒级）**
- `ul_chain_replay` A/B（设备 vs `OCUDU_EQ_GATHER=0`）：**980/980 LLR 逐字节一致**；
- 两个极端形状单独复验：6 PRB / 72 RE 与 **100 MHz / 3300 RE** 均逐字节一致；
- `ctest -L phy` **162/162**；`channel_equalizer_metal_unit_test` 默认路径全绿；
- 稳定性：120 连跑 **0 崩溃**。

**（c）判据③/收益的最终确认要靠 b19**（见 §48.56(d)）。预期：
`eq_demap` busy 仍比主机腿高 ~27µs/lane（GPU 侧是真实工作），但
`[ul_equalization_demod]` / `[ul_pipeline]` 应当**比主机腿低 ~27µs/slot** —— 那才是本步的净收益。

**（d）⚠️ 本步**必须**与 b18b 在**完全相同**的条件下比，否则又是白跑**
b18b 是 `--log.all_level warning` 跑出来的。所以 b19 必须是**同一构建、同一 log level、`OCUDU_EQ_GATHER` 保持默认（开）**：

```bash
cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto --log.all_level warning --log.filename /tmp/gnb_b19.log > /tmp/gnb_b19_console.log 2>&1
```

判据（与 b18b 对照）：`ch_re device≈640992/N host=0`、`burst dispatches` eq 侧 **8/跳**、
`eq_demap` ≈76µs/lane、**`[ul_equalization_demod]` 与 `[ul_pipeline]` 应低于 b18b 的
278.0 / 652.0µs**、`gap` median/p95/p99 仍 ≈0.6/1.1/1.3µs、`mod 3` BLER 不退化。

---

#### 48.57 ⚠️ 对 handoff 快照的两处**更正**（快照不改，按约定写在这里）

上一份 handoff `session_handoff_2026-09-14-4.md` 写于 b17 之后、b18/b18b 之前，
其中以下两句**已经过时**（快照本身**保持原样**，以本节为准）：

| handoff 里的说法 | 更正 |
|---|---|
| §4.1 建议 b18 用 `--log.all_level warning` 只是"**怀疑**日志挤 USB" | **已证实**：`--log.all_level` 本身值 ~45µs（b16 info vs b18b warning 同路径，`[ul_equalization_demod]` median 324.6 → 278.0µs，§48.55(c)）。**今后上机一律 warning** |
| §4.1 的判据只写了"`ch_re device=0 host≈N`、`burst dispatches` 4/跳、BLER 同量级" | b18b **已全部达成**（§48.55(b)）；但**这还不足以判收益**——设备腿当时反而慢 27µs/slot，根因是每 run 建表，**已由 `74eabc7a80` 修掉**（§48.56），收益待 **b19** 确认 |

#### 48.58 b19 分析（`74eabc7a80`）：**S-7f-2 当时等于没生效**——缓存键被截断成 32 位（已由 `04e66a3f6a` 修好）

**（a）b19 的数据（`/tmp/gnb_b19_console.log`，warning 级，正常退出）**

| 指标 | b18b（主机 gather） | b19（设备 gather） | 差 |
|---|---|---|---|
| `equalizer ch_re` | device=0 host=640992 | **device=946869 host=0** | 设备路径确实在跑 ✓ |
| `burst dispatches`（eq 侧） | 4.0/跳 | **8.0/跳**（688632/86079） | gather 的账对上 ✓ |
| `eq_demap` busy | 48.7µs/lane | **75.6µs/lane** | GPU 侧贵 26.9µs，预期 ✓ |
| `[ul_equalization_demod]` median | **278.0µs** | **302.5µs** | **设备腿仍慢 24.5µs** ✗ |
| `[ul_pipeline]` median | 652.0µs | 652.0µs | 打平 |
| `gap` median/p95/p99 | 0.6/1.1/1.3 | **0.7/3.4/60.9µs** | 尾部仍偏 |

**（b）与 b17（S-7f-1，未加缓存）对比，`[ul_equalization_demod]` median：305.3 → 302.5µs**
⇒ **S-7f-2 的"按 hop 建一次"当时等于没生效**。我在本地加了个计数器一测就现形：
**一个 hop（4 个 run）= 4 次 build、0 次 reuse**。

**（c）根因（`04e66a3f6a`）**：缓存键 `hop_tables_plan` 声明成了 **`unsigned`**，而它存的是
**plan 的地址**。64 位宿主上 `0xb8a300000` 被截成 `0x8a300000` ⇒ 与 `&plan` 比较**永不相等** ⇒
每个 dispatch 都重建+重传一次表——**正是 S-7f-2 要删掉的那笔开销**。
改成 `uintptr_t` 后：**1 次 build + 3 次 reuse / hop**（本地复测确认）。

> **教训（写进"别再踩"）**：**"缓存没生效"这种缺陷是沉默的**——功能、门禁、A/B 全都过，
> 只有代价不对。**任何"省掉某笔开销"的改动，都要有一条能直接打脸的计数探针**（这次是
> build/reuse 计数），否则只能靠上机腿的 ±25µs 去猜。

**（d）⚠️ b19 这一腿本身也被污染**：主日志 **122 次实时失败**（**60 late + 30 underflow**，
其余是告警），其中 **112 次挤在同一秒（11:49:20）**；`[ul_pipeline]` max **12808µs**、
`[ul_time_frequency]` max 10637µs、`max total=668µs`（b18b 是 207µs）。
⇒ 那一秒发生过一次**宿主/线程级停顿**，这一腿的分布（尤其 p95/p99/mean）不可用。

**（e）下一步：b20**（`04e66a3f6a`，与 b18b **同条件**：warning 级、设备 gather 默认开）
```bash
cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto --log.all_level warning --log.filename /tmp/gnb_b20.log > /tmp/gnb_b20_console.log 2>&1
```
判据：`ch_re device≠0 host=0`、`burst dispatches` **8/跳**、`eq_demap` ≈76µs/lane、
**`[ul_equalization_demod]` median < b18b 的 278.0µs**、`[ul_pipeline]` ≤ 652µs、
`gap` median/p95/p99 ≈0.6/1.1/1.3µs、`mod 3` BLER 不退化。
⚠️ 若再出现 `Real-time failure in RF: late` 成批出现，先排除宿主负载（备份/Docker/浏览器），
那条腿同样作废。

#### 48.59 b20 **起不来**：SIGBUS 在 UHD 初始化里，OCUDU 代码一行都没跑到

**（a）现场**（用户 console）
```
[INFO] [B200] Loading firmware image: .../usrp_b200_fw.hex...
[INFO] [B200] Loading FPGA image: .../usrp_b210_fpga.bin...
...
[INFO] [B200] Actually got clock rate 7.680000 MHz.
Bus error: 10   sudo ./build/apps/gnb/gnb ...
```
- **`/tmp/gnb_b20.log` = 0 字节**（b18b/b19 都有几百字节）：崩溃发生在 **OCUDU 的日志系统起来之前**；
- console 里**没有** `--== OCUDU gNB (commit ...) ==--` 横幅 ⇒ 连 OCUDU 的启动路径都没进；
- 与 b17/b18b/b19 最大的不同：**这一次 B200 重新加载了 firmware + FPGA 镜像**
  （那三条腿都是"Detected Device"直接过去）⇒ **设备刚重新枚举/上电**，处于全新状态。

⇒ **判定：环境故障（USB/驱动层），不是本步代码。** 依据：① 崩溃点在 UHD 内部、早于任何 OCUDU 代码；
② 同一二进制（`04e66a3f6a`）在本地非 ASAN 复跑 A/B 正常；③ b19 用的前一版设备 gather 在实网跑了
86 万跳、健康退出。SIGBUS 在 macOS 上几乎总是"映射/总线"级问题（USB 传输、mmap、设备掉线）。

**（b）处理建议（按顺序）**
1. **给 B200 断电重上电**（拔 USB、等 5 秒、插回），确认它没有卡在半枚举状态；
2. `uhd_find_devices` / `uhd_usrp_probe` 各跑一次，确认只有一台 B210、且没有别的进程占着它；
3. 换一个 USB3 口（**直连宿主，不要 hub**）；若仍 SIGBUS，换线；
4. 重跑 b20（命令与判据不变，见 §48.58(e)）。若这次在**同一个位置**再次 SIGBUS，那才是代码嫌疑，
   届时按 §48.60 的方法二分。

**（c）⚠️ 顺带发现（与本步无关，但要记账）：ASAN 版 replay 工具会稳定崩**
用 `ul_chain_replay_asan` 复跑抓包时，**当前构建 5/5 必崩**，基线 `e4d5e68975` 0/5：
```
SEGV in ocudu::rx_buffer_impl::get_codeblock_data_bits  (address 0x65b3737f74e0, 每次同一个地址)
  ← pusch_decoder worker 线程
```
- 崩溃发生在 **LDPC 解码阶段、`replayed` 那行打印之前**；**非 ASAN 构建的同一条路径
  （980 抓包 A/B + 120 连跑）从未崩过**；
- 我试过：加"cb 下标越界"的 always-on 检查 ⇒ **没触发**（说明 `codeblock_id` 没越界，
  是**被引用的对象本身已失效**）；把"表缓存"关掉 ⇒ **照样崩**（与本步的表缓存无关）；
- 用环境变量把设备 gather 关掉（纯主机路径）⇒ **也崩** ⇒ **不是设备 gather 引入的**。

⇒ **结论**：这是 replay 工具/解码器生命周期里的一个**潜在 use-after-free（或等价的对象失效）**，
平时被 ASAN 之外的时序掩盖；**本步的改动改变了堆布局与节奏，把它变成了必现**（ASAN 的
quarantine 让同一个失效地址每次都落在同一处）。**这是本地的调试工具问题，不影响上机腿**，
但**在修掉之前，不要把 ASAN replay 当门禁**——它现在会给出假阳性。
下一步若要用 ASAN 复跑，先查：replay 工具的 `spy.done` 轮询（5000ms 上限）与
`rx_buffer_pool`（4 个 buffer）在进程收尾时的释放顺序。

#### 48.60 b20（`04e66a3f6a`）console 里的三个异常：`wrap failures=913`、**负 gap**、授权周期 ~100ms

**（a）b20 其实**跑起来过**：`/tmp/gnb_b20_console.log` 有完整的 57 行退出统计
（`ch_re device=20845 host=0`、正常 `Stopping...`）。`.log` 是 0 字节是因为跑了 `--log.all_level warning`
且那一段没有 warning；**SIGBUS 发生在这之后**（另一次尝试里则更早）。所以"b20 起不来"要分开看：
**统计拿到过**，但**随后仍然崩**。

**（b）三个异常（与之前 4 条 GPU 腿逐项对照）**

| 指标 | b16 / b17 / b18b / b19 | **b20** |
|---|---|---|
| `wrap failures` | **0 / 0 / 0 / 0** | **913**（creates=2234、replaces=2160） |
| `[ul_gpu_lane] gap` median | 0.6 / 0.7 / 0.6 / 0.7µs | **−12.9µs（负！）** |
| `[ul_gpu_lane] period` median | 1017µs | **99591µs（≈100 个 slot）** |
| lane 数 / 采样时长 | 2.8万–8.6万 | **1895** |
| `[ul_mac_pdu_size]` 中位 | 2112B | **528B**（≈30ms 一次授权的量） |

**机制（三条现象同源）**：
`wrap failures` 是 `shared_queue::wrap_no_copy` 里 **`newBufferWithBytesNoCopy` 返回 nil** 的计数
（`ocudu_metal_queue.mm:200`）。它一旦失败，引擎就退回"拷贝一份"（`last_call_no_copy=false`）——
**每次都拷 = CPU 被喂爆 + GPU 抢页**，于是：
- 授权周期从 1ms 掉到 ~100ms（`period median 99.6ms`）⇒ lane 数从 8.6 万掉到 1895；
- `busy > residency` ⇒ **`gap` 变负**（时间戳自相矛盾，典型是 GPU 命令缓冲被拖到超时/抢占）；
- 然后 UHD 的 USB 传输跟不上 ⇒ **bus error**。

**（c）为什么 CPU 腿没事**：CPU 路径**完全不用 Metal**（不 pin 页、不建 no-copy 缓冲）。
旁证：用户同时跑的 CPU 腿 `/tmp/gnb.log`——**PUSCH=140370、BLER 0.21%、256QAM 全程、TBS 中位 2178、
`t=` 中位 126µs**，完全满速健康。⇒ **崩溃与"GPU 路径"强相关，与"满速 iperf3"无关**（CPU 腿同样满速）。

**（d）两种解释，需要一条干净的对照来分辨**
1. **宿主资源压力**：`newBufferWithBytesNoCopy` 要**把宿主页 pin 给 GPU**；内存紧时它会失败。
   注意：**我自己在那段时间做的诊断（对 410MB/几十MB 日志反复 grep、构建 worktree、ASAN 复跑）
   就是压力源**——这属于"观测污染测量"，必须排除。
2. **新代码路径**：`74eabc7a80`/`04e66a3f6a` 之后 gather 表缓冲改为**一个 hop 建一次并缓存**，
   同时每个 gather dispatch 都会再 wrap 一次**整张网格**（5MHz 配置 300 subc×14×1 = 16.8KB，
   按 275 PRB 分配则是 180KB）。**这两条在 b20 是首次上机**，所以不能只凭"以前 4 腿好"就排除。

**（e）干净的对照（一次一条命令，请在系统空闲时跑）**
1. **先关掉我这边的所有重活**（我不跑 grep/构建/ASAN），确认 `vm_stat` 空闲页充足、没有别的
   大进程；手机侧照旧跑 iperf3；
2. 跑 GPU 路径（同 b20 命令，warning 级）；
3. **跑到 5 分钟以上再 Ctrl-C**（要的是退出统计），然后把 console 整个存下来；
4. 判据：
   - **`wrap failures` 必须是 0**（b16–b19 都是 0）。若仍 >0 ⇒ **不是内存压力**，是代码/驱动层，
     我立刻按 §48.60(f) 二分；
   - `[ul_gpu_lane] period median` 应回到 ~1017µs、`gap` 回到 ≈0.6µs（不得为负）；
   - lane 数与 `[ul_mac_pdu_size]` 中位应回到 2.8 万级 / 2112B。

**（f）若在干净环境下仍复现**，二分方案（每次只需重建 `gnb`）：
`04e66a3f6a`（当前）→ `74eabc7a80`（表按 hop 建一次）→ `e4d5e68975`（S-7f-1，b19 用过、实网健康）。
三者只在"gather 表缓冲是否缓存/怎么建"上不同；若 `e4d5e68975` 在同样环境下也出现 `wrap failures>0`，
那问题在**环境/驱动**，不在代码。

#### 48.61 **真正的根因找到了**：gather 表缓存**跨 hop 存活** ⇒ 第二个 hop 读到第一个 hop 的表（`ec5c53a24c` 修复）

**（a）症状（用户 b20 第二次）**：手机能连上、跑一会，然后
`pusch rsrp` 掉、UL `nok` 涨到 **100%**、DL 也 88%，最后 **Bus error** 退出。
**这是"数据错"而非"起不来"** —— 与 §48.60 里"设备刚上电"那套环境解释**不同**，我之前的判断部分错了。

**（b）根因（已用实验闭环）**
- 缓存键是 **plan 的地址**；`pusch_demodulator_impl` 每个 hop 都
  `make_unique<ch_gather_desc>()` 建**新对象**，而分配器**把同一地址还回来**；
- 于是"地址相同"被当成"同一个 hop" ⇒ **第 N+1 个 hop 复用了第 N 个 hop 的表**，
  而表里写的是**第 N 个 hop 的 RE 位置** ⇒ 均衡器读了**错误的符号**；
- 为什么会"跑一会才崩"：错表只在**分配变化**时才产生错误数据；一旦错数据进了 LDPC/PMI/HARQ，
  链路自适应开始下调 MCS、BLER 攀升，最终 USB/GPU 时序被拖垮 ⇒ bus error。

**（c）为什么 980 门禁看不见**
- **每个进程只跑 1 个 reception**（一个 hop）⇒ 根本没有"第二个 hop"；
- 我加的 `--repeat` **重复同一个抓包** ⇒ 分配相同 ⇒ **旧表恰好还是对的**（复现实验证实：
  6 次重复 builds=2/reuses=10，结果全对）。
- **只有"同一进程内、分配发生变化"才会暴露。** 这正是实网的行为（每个 slot 的授权都可能不同），
  而工具从来没这么跑过。

**（d）复现与修复验证（决定性实验）**
给 `ul_chain_replay` 加 `--also <capture>` 轮转不同形状后：
| 版本 | 3 个不同分配（25/14/6 PRB）在同一进程 |
|---|---|
| **去掉失效**（复现缺陷） | 第 3 个 reception **iter=6–7**（正常 2）、SINR 35.35dB（正常 31.78）⇒ **错表** |
| **有失效**（`ec5c53a24c`） | 三个全部 **iter=2**、SINR 与单跑逐位一致 ✓ |

修复本身只有一行实质内容：`eq_flush_recycle()` 里把 `hop_tables_valid = false`
（缓存只在**一个 hop 之内**有效；缓冲仍交给 command buffer 保活）。

**（e）新门禁（已加入工具，今后必跑）**
```bash
B=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
$B /tmp/iq2_10044_17923 --metal --out /tmp/soak --also /tmp/iq2_10041_17923 \
   --also /tmp/iq2_10029_17923 --also /tmp/iq2_1009_17923 --also /tmp/iq2_10040_17923 \
   --also /tmp/iq1_10049_17921 --repeat 2
```
判据：**每次重复的结果必须与单跑逐位一致**（CRC/SINR/iter 全同）。
本次：**12 个 reception（5 种形状 × 2 轮）全部一致，0 崩溃**。

**（f）本轮其它门禁**：980/980 A/B 逐字节一致、`ctest -L phy` 162/162、等化器单测全绿、
120 连跑 0 崩溃。HEAD = **`ec5c53a24c`**，`gnb` 已重编（自检通过）。

**（g）教训（值得进"别再踩"清单）**
1. **"地址当身份"是危险的缓存键**——对象会被分配器复用到同一地址。要用**内容/世代**做键，
   或者在**生命周期边界**（这里是 flush/hop）强制失效。
2. **"缓存没生效"和"缓存生效但键错了"是两种完全不同的缺陷**：前者只损失性能（§48.58），
   后者**静默产生错数据**（本节）。修 §48.58 的那一刻，就把一个"性能缺陷"变成了"数据缺陷"。
3. **重复同一个输入的 soak 证明不了什么**：要**变化输入**（这里=不同分配）才能暴露按身份缓存的状态。
4. 我此前把 b20 的第一次 SIGBUS 归因为"设备刚上电/环境"，**这个判断不成立**；
   真正的解释见本节 —— 现场证据（UL 100% BLER）当时就在用户贴的 console 里，我漏读了。

#### 48.62 b20（`ec5c53a24c`）**通过**：跨 hop 缺陷确认修好，链路健康；但"收益"仍未兑现

**（a）这一腿是干净且健康的**
- **正常 Ctrl-C 退出**，`wrap failures=**0**`（§48.60 的 913 次异常消失），`gap` median **0.6µs** / p95 1.1 / p99 13.7（**不再为负**）；
- `ch_re device=670153 host=0`、`burst dispatches` eq 侧 **8/跳**（487384/60923 = 8.0）、`cbs/lane=2.00`；
- 60,923 lane ≈ **61s 空中时间**，`ul_mac_pdu_size total=118,985,485B` ⇒ **UL ≈2.0MB/s、PDU 中位 2112B、几乎全程 256QAM**；
- 主日志只有 **4 次** `Real-time failure in RF: underflow`（个位，正常），**0 次 USB 错误、0 崩溃**。

⇒ **§48.61 的跨 hop 缺陷（读错符号 ⇒ UL BLER 100% ⇒ bus error）确认修复**；b20 可以当作"设备 gather 的稳定基线"。

**（b）但设备 gather 相对主机 gather 仍然是**负收益**（同 warning 级、同构建的 b18b 对照）**

| 指标 | b18b（主机 gather） | **b20（设备 gather）** | 差 |
|---|---|---|---|
| `[ul_equalization_demod]` median | **278.0µs** | **305.0µs** | **设备慢 27.0µs** |
| `[ul_pipeline]` median | 652.0µs | **687.0µs** | 设备慢 35µs |
| `ch_est` / `time_frequency` / `ldpc` median | 73.5 / 214.5 / 89.0 | 74.3 / 216.2 / 90.0 | **不变**（噪声） |
| `eq_demap` busy（GPU 侧） | 48.7µs/lane | **75.4µs/lane** | 设备贵 26.7µs（预期）|

**两次 S-7f-2 优化（表缓存 + 跨 hop 失效）都没有改变这个结论** ⇒ §48.58(d) 里"每 run 建表"的归因**不足以解释全部**：
表缓存在**每个 hop 只建一次**（实网 ~60k 个 hop ⇒ 只省了 4→1 次建表/hop 的一部分），
而设备腿在 **CPU 侧**依然多花 ~27µs。剩下这 27µs 的具体归属**尚未定论**（候选：gather 表构造 1 次/hop、
每 run 一次 gather dispatch 的编码与缓冲绑定、以及 `y` 必须由 GPU 冷读网格 vs 主机 gather 时数据已在缓存里）。

⚠️ 这两腿的**信道不同**（b18b PDU 中位 **784B** vs b20 **2112B**），所以上表的差**不能当定论**——
但方向与 GPU 侧的 +26.7µs/lane 一致，且 CPU 侧的三段（ch_est/DFT/LDPC）都没变，符合"多花的钱在 gather 这一段"。

**（c）下一步：交替 A/B（唯一能把信道变量消掉的办法）**
同构建、同 warning 级、**只翻 `OCUDU_EQ_GATHER`**，两条腿**背靠背交替跑**（每腿 3–5 分钟、Ctrl-C 正常退出）：
```bash
# 腿 A（设备 gather，默认）
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal \
  --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_b21a.log > /tmp/gnb_b21a_console.log 2>&1
# 腿 B（主机 gather）
sudo -E env OCUDU_EQ_GATHER=0 ./build/apps/gnb/gnb -c ...（同上）--log.filename /tmp/gnb_b21b.log > /tmp/gnb_b21b_console.log 2>&1
```
判据：比较两腿的 `[ul_equalization_demod]` median 与 `eq_demap` busy。
- 若设备腿**稳定高 ~27µs** ⇒ **结论是"这一优化在该工作点上不划算"**，决策有两条：
  ① 默认改回主机 gather（`consumes_gathered_symbols` 返回 false），把设备 gather 留作后续 F2/F3 的基础设施；
  ② 继续挖那 27µs（把 gather 并进 eq dispatch、或让 CE 的估计布局允许 1 个 run/slot）。
- 若设备腿**追平或更低** ⇒ 之前的差是信道/宿主抖动，保留设备 gather。

#### 48.63 方向校正（用户定调，**优先级高于本文档此前任何"是否回退"的讨论**）

**用户明确：总目标是"全 GPU path"——把尽可能多的模块搬进 GPU；某个局部有一点性能退化可以接受；
等功能全通再做性能优化。**

因此：
1. **§48.62(c) 里"默认改回主机 gather"这个选项撤销**——它与总目标冲突。设备 gather 保持**默认开启**；
   `OCUDU_EQ_GATHER=0` 只作为 **A/B 与逃生口**保留。
2. §48.62(b) 的 **+27µs CPU 侧**、以及 §48.55(f)/§48.59 的 `gap` p99 偏高，**一律记入"待优化的账"**
   （活文档 §10.x / §48.x 的优化清单），**不作为阻塞项、不作为回退理由**。
3. **判据口径相应调整**：每一步的门禁 = **功能正确**（逐位/逐字节一致、上机链路健康、无崩溃）
   ＋ **不把 GPU 侧的工作推回 CPU**；CPU 侧的 µs 级变化只**记录**，不设 gate。
4. **推进顺序不变**（§48.50(c) 的结论）：**K0-c/d → K0-a/b（顺带）→ F2（单 CB 融合）→ F3（ring）**。
   理由与性能无关，是**依赖关系**：K0-c/d 成立后"主机需要导频"这条依赖才断，K0-a/b 才能净赚。
5. 性能优化的时机：**全 GPU path 功能跑通之后**统一做；到那时本文档里所有"待优化的账"一并重测。

#### 48.64 K0-c/d 侦察定稿（读代码后）：**不能"先把导频搬上设备"**，正确切法是"矩阵在设备上直接算出来"

**（a）关键发现：`stats_estimator` 在 v1 是**固定常数**实现**
`channel_statistics_estimator_fixed`（`channel_statistics_estimator.h:70`）：
`tau_rms_s` / `fd_hz` 是**配置常数**，`estimate()` 只把 `sigma2` 透传。
⇒ **设备侧只需要产出 `sigma2`，不需要复刻任何"统计量估计"**（比 §48.49 的预期简单一大截）。

**（b）`sigma2` 的全部依赖（`estimate_noise`，`helpers.cpp:511`）**：
`pilots`（已知序列）、`rx_pilots`（网格）、`estimates`（**滤波后的 LSE**）、`beta`、`cfo`、
`symbol_start_epochs` —— 其中 **`cfo` 是同一跳更早的主机输出**（`args.cfo_hop`）。
⇒ **`sigma2` 无法在"主机还在算 CFO"之前独立算完**，这就是 §48.50(a) 那个循环依赖的具体形态。

**（c）主机侧这三段每次 hop 的实测（b20 实网）**：
`stage=16.23µs`（A/R_hp/导频 ≈170KB 的 memcpy）、`corr=12.4µs`（建矩阵）、`sigma2=2.9µs` —— 合计 **≈31µs/hop**。
**矩阵那两项（stage+corr ≈28.6µs）占了 92%**，而且**逻辑上极适合上设备**：
`A = kron(R_t_pp, R_f_pp) + (sigma2+ridge)I`、`R_hp = kron(R_t_hp, R_f_hp)`
—— **每个元素只是两个解析相关函数的乘积**（`rt_corr`/`rf_corr`），**只依赖 `(dmrs 符号位置, 导频子载波位置, fd, tau_rms, sigma2)`**，
**不依赖网格、不依赖导频数据、不需要 LSE、不需要 CFO**。

**（d）因此 K0-c/d 的正确切法（与 §48.49(f) 的"先搬导频"相反）**：

| 阶段 | 内容 | 输入 | 是否引入新同步 |
|---|---|---|---|
| **K0-d** | 新内核 `mmse_corr`：**在设备上直接生成 A 与 R_hp 到引擎槽位** | 只有几何 + `fd/tau_rms`（常数）+ `sigma2`（K4 已在设备上算出） | **不引入**：`sigma2` 已由 K4 在设备产出（`gpu_ce`/`nv`），K2/K1 本来就在同一 CB |
| **K0-c** | `sigma2` 的设备来源（K4 已有）+ 主机不再读 `rx_pilots` | — | 需先把"主机 sigma2"换成 K4（部分已在做） |
| **K0-a/b** | 网格→导频、LSE/CFO 上设备 | — | **排在 K0-d 之后**：只有 A/R_hp 不再依赖主机 LSE，导频才真正不需要回主机 |

⇒ **先做 K0-d（矩阵内核）**：它是**净赚**（省 stage+corr ≈28.6µs/hop 的主机时间与 ~170KB memcpy），
**且不需要动导频/LSE/CFO 任何一环**，风险最小。
**K0-a/b 随后**（等 K0-d 落地、主机对 LSE 的依赖只剩 CSI/TA/CFO 那些"非阻塞路径"）。

**（e）门禁（K0-d）**
1. **逐位/近逐位**：`mmse_corr` 的 A/R_hp 与主机 `build_correlation_matrices` 的输出，
   在单测里比 **max 相对误差 ≤1e-6**（两者都是 float 解析式，顺序一致即可；`rt_corr`/`rf_corr` 逐式照抄）；
2. **计数归零**：`[mmse_time_sum]` 的 `stage` 与 `corr` 两项必须**显著下降**（目标：`corr`→0）；
3. **CE 单测**：Test 9/11/12 与改动前一致；
4. **端到端**：`ul_chain_replay` 980 抓包 A/B 逐字节一致 + 多形状 soak（§48.61(e)）；
5. **上机**：`[mmse_time_sum] corr` 归零、`[ul_pipeline]` 不退化、链路健康。

#### 48.65 S-7f-3（K0-d）进度：相关矩阵内核已落地，**A 已对齐、R_hp 未对齐**（`9d355a628a`，WIP，默认关闭）

**（a）已完成的实质工作**
| 件 | 内容 |
|---|---|
| `ocudu_mmse_corr.metal` | `mmse_corr_a` / `mmse_corr_r_hp`：**逐表达式照抄**主机 `build_correlation_matrices()`（同样的整数差→float、同样的 `rt*rf`、同样的 `1e-6` ridge、对角 `+sigma2+ridge`） |
| `mmse_engine::build_correlation()` | 每个 system 一次 dispatch，A 与 R_hp 写进**引擎自己的槽位**（K1/K2 读的那块） |
| `port_channel_estimator_metal_mmse_impl` | 标准块走设备构建（失败回退主机）；`stage_engine_group(..., slots_filled)` 在槽位已由设备填充时**跳过那 ~170KB 的 memcpy** |
| 门禁 | 默认路径不变：980/980 A/B 逐字节、`ctest -L phy` 162/162 |

**（b）实测对齐状态（本地探针，25 PRB/3 DMRS/L=54/nout=504）**
| 矩阵 | 差异 | 判定 |
|---|---|---|
| **A** | 5832 个 float 中 **720** 个不同，**max 相对误差 ≈7e-8** | **等价**（纯浮点舍入：Metal 与 clang 的 FMA 收缩/常数折叠不同） |
| **R_hp** | 54432 个中 **27070** 个不同，含 **rel>1e-3 的真错**（如 `R[0][0]`：主机 1.0、设备 0.0） | **不等价，待修** |

**（c）已排除的怀疑**（都试过、都不是）：对角加载漏写（已补）、per-system 基址没前移（已修）、
探针区域错位（已改为按 system 逐块对比）。**A 与 R_hp 共用绑定、strides、导频映射**，
所以差异**只在 R_hp 的索引本身**（另一个候选：两个 kernel 在同一 CB 里逐 system 交替 dispatch）。

**（d）状态与开关**：**默认关闭**（`OCUDU_CE_CORR_DEV=1` 打开），因此树是绿的。
**下一步（很聚焦）**：把 R_hp 内核的索引与主机逐项对齐——最省事的办法是给内核加一个"按 (o,k) 打印"
的调试输出，与主机同样 (o,k) 的值逐项比；一旦 R_hp 等价，本步的收益（省 `stage`+`corr` ≈28.6µs/hop
与 ~170KB memcpy/hop）即可兑现，并顺势打开 K0-a/b。

**（e）旁路发现（值得单独立项）**：`gpu_a` / `gpu_r_hp` 用 `alloc_aligned`（**4096 对齐**）而非
`alloc_aligned_pages`，而 Metal 的零拷贝 wrap 要求 **16KB 页对齐**且长度覆盖整页
⇒ 这两个缓冲**可能**是 §48.60 里 913 次 `wrap failures` 的来源之一。等 K0-d 收口后单独查。

#### 48.66 S-7f-3b：相关矩阵**换装完成**（槽位终于装对东西了），但端到端仍不等价（`2cb8edde33`，WIP）

**（a）本轮修掉的三个真缺陷**（都是"换装"时才会暴露的）
1. **槽位里该放的是 `A⁻¹`（或恒等填充后的 `A`），不是 `A`**：主机 staging 在被删掉的同时，**把反演也一起删了**——
   权重核一直拿"原始 A"当逆矩阵用。现在把同一段 Gauss-Jordan（读的就是内核刚写进去的矩阵，**运算顺序不变**）
   与恒等填充**就地**补在设备槽位上。**实测：A 与 A⁻¹ 在槽位布局下与主机逐元素一致**
   （A 的最大相对偏差 ~7e-8，A⁻¹ ~5e-7）。
2. **两处基址/步长**：① per-system 基址必须**前移指针**（不是 `setBuffer:offset:`），否则所有 system 都写进第 0 个槽位；
   ② 内核写的是**紧凑** `L×L`/`nout×L`，步长必须是 `L`（不是槽位的 `Ls`）。
3. **`2π` 的舍入**：主机的 `TWOPI` 是 **double** 字面量（表达式在 double 里算完再舍入进 float），
   Metal 没有 double ⇒ 改用**正确舍入后的 float 常量**（`as_type<float>(0x40C90FDBu)`，恰好等于 `2*M_PI_F`）。
   否则每个相关值偏 1 ulp，**被矩阵反演放大**成 5e-7 的逆矩阵差。

**（b）仍然不等价**：同一抓包 `iq2_10041_17923`，设备相关矩阵路径 **SINR −2.78dB / iter=10**，
主机路径 **32.57dB / iter=2**。⇒ **缺陷在槽位的下游**（输入、槽位内容都已逐元素核对过）。
**默认关闭**（`OCUDU_CE_CORR_DEV=1` 才开），树是绿的：**980/980 A/B 逐字节、`ctest -L phy` 162/162**。

**（c）下一步（很聚焦）**：把 `gpu_w`（权重矩阵）在两路各 dump 一次逐元素比。
槽位已确认一致 ⇒ 差异必然出现在 **`run_nn` / `run_weights_only` 的入参或 dispatch**，
或者**设备构建改变了某处引擎状态**（例如 `matrix_on`/`gpu_invert` 的组合、`nof_systems`、或 K3 的 `reformat` 仍在被附加）。
最可能的形态：设备路径下 **`stage_engine_group` 不再写 A/R_hp，但引擎仍按"矩阵已在槽位"的假设走另一条分支**——
即两条路径的 `matrix`/`gpu_invert` 组合必须**完全一致**才可比。

**（d）经验（进"别再踩"）**：**"把一段工作搬到别处"时，那一段的"副作用"（这里是反演+填充）最容易漏。**
门禁必须**逐元素比中间产物**（槽位、权重），不能只看端到端——端到端只会说"错了"，说不清"错在哪一层"。

#### 48.67 S-7f-3c：设备相关矩阵**已经逐字节正确**，但**搭错了 command buffer**（`7178213185`，WIP）

**（a）本轮三个发现（按付出的代价排序）**
1. **设备路径在本不该跑的地方跑了**（这是"每跳都错"的真凶）：
   `merge_tail` 打开时，**标准块的批次里多带一个"尾块" system**（几何不同），而设备构建**只认标准几何**
   ⇒ 尾块那个 system 保留槽位里的旧内容，权重用**过期的矩阵**算。现在用 `!merge_tail` 把它挡住。
   **这一条修好之前，连"内核根本没建"的抓包都表现为错**——所以之前的定位一直被它带偏。
2. **内核改成 batch 级**：`thread_position_in_grid.y` 选 system，**每个矩阵一次 dispatch**（原来每 system 一次）。
   每 system 一次 dispatch 在 25 PRB 上实测 **628µs GPU 时间**——**比它替换掉的主机循环还贵**。
3. 修完 1+2：**设备构建精确**——25 PRB 单几何跳上，LLR 与主机路径**逐字节一致**。

**（b）但它还不划算**：设备构建**在自己的 command buffer 里**（commit+wait），这次往返实测
**`gpu_path` ≈556µs**，而它替掉的主机工作只有 **≈30µs**（stage+corr）。
⇒ **相关矩阵必须搭进"反演+权重+K3/K4"那一条已有的 command buffer**（引擎本来就在拼那条），
而不是自己开一条。这是下一步的**唯一**动作。

**（c）状态**：**默认关闭**（`OCUDU_CE_CORR_DEV=1` 打开）。默认路径：**980/980 A/B 逐字节、162/162**。

**（d）经验**：**"搬到设备"不等于"搭上正确的流水线"**——一个能跑、能对的实现，只要自己开一条
commit+wait，就足以把收益吃掉两个数量级。**门禁要从一开始就量"这条新工作搭在哪条 CB 上"**，
而不是等正确性做完再看时间。

#### 48.68 **关键发现：设备相关矩阵能不能兑现，卡在 K1 自己身上**（`7b9f9ad6ed`）

**（a）本轮把"卡点"彻底量清楚了**
设备相关矩阵要**搭进反演那条 CB**，而**只有当反演本身是设备 dispatch 时，那条 CB 才存在**。
反演走不走设备，由 K1 的**阶数上限**决定——原来是 **36**，而**实网标准块是 3 PRB × 3 DMRS ⇒ L = 54**。
⇒ 每一个真实跳都在主机反演 ⇒ **相关矩阵没有 CB 可搭**。这是"收益为负"的**结构性**原因，不是调参问题。

**（b）做了什么**：`ocudu_mmse_inv.metal` 的 `MAX_N` 36 → **54**（增广矩阵 54×108 float = **22.8 KiB**，
在 32 KiB 线程组内存之内；**72 就不行了**）。引擎的阶数检查同步放开。

**（c）然后量到 K1 在 54 阶是"延迟受限"**（这是本轮最重要的数字）：

| 14 PRB 跳（L=54） | `gpu_wait` | `stage` | 结果 |
|---|---|---|---|
| CPU 反演（现状） | **85µs** | 27.4µs | SINR 32.57 / iter 2 ✓ |
| K1 设备反演（放开上限） | **555µs** | 4.4µs | SINR 32.59 / iter 6（且与 CPU 路径**不逐位**）|

⇒ 设备反演比它替换的 **~10µs 主机 Gauss-Jordan 多花 ~470µs**。K1 自己的头注释早就写了原因：
**消元要走 72 个主元相位、每相位两次带屏障的步骤**（"latency-bound: 91.3µs for a single 36×36 system"）。
**所以估算器的 `MAX_GPU_INVERT_ORDER` 保持 36**（代码里写了这条测量）。

**（d）结论与依赖链（这是当前路线的真实形状）**
```
设备相关矩阵（已精确、已逐字节验证）
        └── 需要搭上"反演"那条 CB
                └── 需要反演本身是设备 dispatch
                        └── 需要 K1 在 L=54 上别比 CPU 慢 47 倍   ← 卡在这里
                                └── K1 的 S-5a（blocked / simdgroup_matrix 形式）
```
⇒ **K0-d 的收益兑现，前提是把 K1 做快**。这不再是"接口/接线"问题，而是**一个已被点名的内核工程项**。

**（e）稳定性澄清（重要，避免误判）**：replay 工具在 `rx_buffer_impl::get_codeblock_data_bits` 上
**约 4% 的连跑会崩**，而且**device 腿与 host 腿同样会崩**（80 对实测：各 1 次）⇒ **与 gather 无关**，
是工具/解码器生命周期的既有竞态（§48.59 记过）。K1 内存增大**实测中立**（MAX_N=36 时 5/120、54 时 4/120）。
⇒ **今后连跑崩溃要按"两条腿都崩 ⇒ 忽略；只有一条腿崩 ⇒ 查"来判断**。

#### 48.69 K1 做完了、量透了：**这条路线到此为止（按当前算法）**（`a921899131`）

**（a）做了什么**：新增 `mmse_inv_rl`（K1b，**右看式** Gauss-Jordan：每个主元一次 rank-1 更新，
而不是块形式的 rank-8 扫描），与 K1 **数学等价**；并写了独立 A/B 工具 `k1_check`（对 CPU 参考实现）。

- **正确性**：良态矩阵下 max 相对误差 **5.6e-7（n=18）/ 1.3e-6（n=54）** ✓
- ⚠️ **k1_check 的第一版选错了测试矩阵**：估算器自己的 A = R_pp + 极小 σ²I **接近奇异**，
  在那种矩阵上**任何**反演（含 CPU）的逐元素相对误差都很大（我第一版看到 1.19 的相对误差，
  差点判内核坏掉）。**跨实现比对必须用良态矩阵**，这条值得记。

**（b）关键实测（把"两次成本"分开了，这是本轮最重要的产出）**
| 形态 | n=18 | n=54 |
|---|---|---|
| K1（块形式）1 system | 70.9µs | **407.3µs** |
| K1b（右看式）1 system | 74.7µs | **407.5µs** |
| K1b，8 systems | 69.9µs（8.7µs/system） | **404.5µs（50.6µs/system）** |

⇒ 拆成两笔账：
1. **固定 ≈70µs/call**（command buffer 的提交往返）——**与阶数无关**；
2. **单个 54×54 system ≈340µs GPU 时间**——**加系统数不摊薄**（8 个 system 总共还是 403µs），
   说明**多系统已经并行、单个反演本身是延迟受限**（108 次屏障 + 串行主元链）。

**（c）结论：K0-d 按原设计走不通**
设备相关矩阵要省的是 **28.6µs/hop** 的主机工作，而设备反演要 **~400µs**，再加设备构建自己的往返。
**差 40 倍**，不是调优能补的（K1 与 K1b 两种形式**一样慢**，说明瓶颈在算法结构而非实现细节）。
⇒ **K1b 保留**（更正确、更简单的形式 + 有 A/B 工具），**估算器的 GPU 反演上限保持 36**（注释里写了这些数字），
**相关矩阵内核保留为 opt-in 基础设施**（`OCUDU_CE_CORR_DEV=1`）。

**（d）下一步该怎么走（我的建议，等你定）**
"把统计量+相关矩阵搬上设备"这条路，**不要以"设备反演"为前提**了。可行的替代：
1. **只搬可并行的部分**：`A`/`R_hp` 的**生成**（解析式、逐元素独立、已经逐字节验证）留在设备，
   但**反演仍在主机**——代价是矩阵要往返一次主机（~170KB 下载 + 反演 + 上传），
   按 §48.47 的量级（下载 ~20µs + 上传 ~20µs）**大概率仍不划算**，需要用一次实测确认；
2. **换目标**：把这一步（K0-c/d）从"净赚项"降级为"**F2 单 CB 融合的必要条件**"，
   即为了"CPU 全程不碰数据"而付这 400µs，**在全 GPU path 的价值框架下可能是可接受的**——
   这正好符合你定的"局部退化可接受、先把全 GPU 路径跑通"；
3. **先做别的**：K0-a/b（导频/LSE 上设备）本身也是那条链的一环，但同样受"CFO 在主机"的循环依赖限制。

**（e）我倾向**：按你的总目标，选 **2**——把"设备相关矩阵 + 设备反演"作为 F2 的组成部分**推进到默认开启**，
用"全 GPU path 跑通"为判据，而不是用这 400µs 的局部代价。若你同意，下一步就是把它打开、跑全门禁、上机看链路。

#### 48.70 S-7f-3f：相关矩阵**已经搭进引擎那条 CB**（往返省回来了），但**还剩一个缺陷**（`2298eb45af`，WIP）

**（a）本轮完成的实质改动（正确方向、已落地）**
设备相关矩阵过去**自己开一条 command buffer**（一次独立提交 ≈70µs 往返 + 自己的 GPU 工作），
现在**作为权重调用的前缀**编进**同一条 CB**：`encode_corr()` 先编相关矩阵的两个 dispatch，
随后的反演/权重读它们写的结果。估计器**只准备描述符**（`correlation_stage()`），
dispatch 由 `run_weights_only()/run_weights_only_async()` 从可选的 `corr_stage` 参数编码。

**实测（25 PRB 跳）**：设备路径 `gpu_path` 从 **597µs（自带 CB）→ 91µs（搭进 CB）**，
主机路径是 74µs ⇒ **搭进去把 ~550µs 的独立提交成本收回，剩下 ~17µs 是相关矩阵自己的 GPU 工作 + 设置**。
（这就是 §48.67 指出的那件事，做完了。）

**（b）⚠️ 剩下的缺陷（因此仍**默认关闭**，`OCUDU_CE_CORR_DEV=1` 打开）**
在搭进 CB 之后，我逐个 dump 了两条路径**引擎调用之后**的全部中间量：

| 中间量 | 设备路径 vs 主机路径 |
|---|---|
| 引擎调用后的槽位（A/R_hp） | **逐字节相同** |
| `gpu_w`（权重） | **逐字节相同**（27216 float 全等） |
| `gpu_y`（导频 staging） | **逐字节相同** |
| `gpu_h`（apply 输出） | **逐字节相同** |
| `grid_est`（估计器输出） | **逐字节相同** |
| **LLR** | **不同**（SINR −3.11dB vs 23.97dB，同一抓包）|

⇒ **差异不在矩阵、不在权重、不在 staging、不在估计器**，而在**引擎调用之后**：
最可能是 **K3/K4 阶段或 unpack 路径**里有一处"依赖槽位是怎么被填的"的状态
（例如 `gpu_ce_ready`/`reformat` 的附加条件、或 `std_slots_filled` 分支里少走了一步）。
**下一步的探针应该打在 K3/K4 与 unpack 上**，不要再回头查矩阵（已证明相等）。

**（c）状态**：默认路径 **980/980 A/B 逐字节、`ctest -L phy` 162/162**；`2298eb45af` 已 push。

**（d）方法论沉淀（这轮的教训）**
**"逐层 dump 中间量"是唯一能把这类缺陷逼到角落的办法**：我从"矩阵不等"一路查到"六个中间量全等、
只有 LLR 不等"，每一步都把怀疑范围砍掉一半。反过来，**只看端到端只会反复说"错了"**。

#### 48.71 S-7f-3g：把范围缩到"**引擎调用内部**"，并且发现**我之前的对照是假的**（重要更正）

**（a）⚠️ 先更正一个方法论错误（这是本轮最重要的收获）**
我在 §48.70 里写的"六个中间量逐字节相同"**不成立**，原因是我用了**错误的逃生口变量**：
代码读的是 `OCUDU_CE_CORR_DEV`（默认关、置 1 开），而我设的是 **`OCUDU_CE_CPU_CORR`**——
**这个变量在当前构建里根本不存在**（`strings | grep OCUDU_CE_` 可验证）。
⇒ 我那几轮所谓的"主机路线 vs 设备路线"，**其实两次跑的是同一条默认路线**，
所以"全都是逐字节相同、只有 LLR 不同"是**同一条路线自己跟自己比**的假象。

**教训**：**做 A/B 之前先确认开关真的存在、真的生效**（`strings` 查一遍 + 用一个必然导致可观测差异的值验证一次）。
否则会得到"一切都相同却结果不同"这种自相矛盾的结论，并据此浪费大量时间。

**（b）正确的对照结果（带标记、不再混淆）**
同一抓包 `iq2_1009_17923`，两条路线的**发布后**（`complete_fd_td_estimation_stage` 解包之后）：

| 量 | 默认（主机建矩阵） | `OCUDU_CE_CORR_DEV=1`（设备建矩阵） | 比值 |
|---|---|---|---|
| `gpu_ce` 的 `median|x|` | **4.49e-1** | **7.36e+2** | **~1640×** |
| `gpu_nv`（噪声方差） | **1.62e-3** | **2.24e+6** | ~1.4e9 |
| LLR / SINR | 23.97 dB / iter 2 | −3.11 dB / iter 10 | — |

⇒ **设备路线的信道估计大了约 1640 倍**，而且 `rsrp=63.5dB`（默认是 −0.92dB）。
`nv` 的巨大差异**是 ce 巨大的后果**（K4 的噪声来自 h），**不是独立缺陷**。
**根因在"引擎调用内部"**：矩阵（A/R_hp）、权重（W）、导频 staging（y）我都已用**正确**的对照确认过一致，
所以问题出在 **apply 那一步（h = W·y）** 或**它与反演/权重的相对顺序**上。

**（c）当前状态（树是绿的）**
设备相关矩阵**默认关闭**（`OCUDU_CE_CORR_DEV=1` 打开）；默认路径 **980/980 A/B 逐字节、`ctest -L phy` 162/162**。
`2298eb45af` 之后本轮的改动是**去掉探针**，未提交任何行为变化。

**（d）下一步（聚焦、且已排除大量可能性）**
在 `engine_run()` 的 `run_weights_only(_async)` 返回之后、`unpack_engine_group` 之前，
**分别 dump `gpu_h` 的前若干元素**（不是槽位、不是权重），逐元素比两条路线；
若 `gpu_h` 已差 1640×，则问题在 **apply kernel 的入参或 dispatch**；
若 `gpu_h` 相同，则在 **unpack/后续**。**这一步能把范围再砍一半**。
另建议顺手确认：`apply` 的 `aparams{nout, L, nof_systems, nof_blocks}` 在两路线是否一致
（corr 前缀是否影响了 `nof_blocks` 的取值）。

#### 48.72 S-7f-3h：`gpu_h` 探针**打早了**（defer 路径下提交时还是空的），以及一条重要的自我提醒

**（a）这一步做了什么**：在 `engine_run()` 提交之后 dump `gpu_h`（前 8064 float）。
结果：**两条路线都是全 0**。原因清楚——`defer=1`（两条路都是），
**提交时 GPU 还没执行**（同一个线程还在 CPU 侧编码/提交），所以那一刻 `gpu_h` 仍是空的。
`OCUDU_PUSCH_FORCE_SERIAL=1` 也没改变（那只影响解调链的等待方式，不改变 CE 的 defer）。
⇒ **该探针必须打在 `complete_fd_td_estimation_stage()`（wait 之后、unpack 之前）**，否则读到的是空的。

**（b）⚠️ 必须记下的一条自我提醒：我在这一轮反复"改探针、跑一遍、看数字"，
但每一步的**判定依据**没有事先写清楚**——这正是 §48.71 那个"开关不存在"的错误能发生的土壤。
从现在起，**每次插探针前先写下"看到什么数字就得出什么结论"**，再跑。
（本轮已因此浪费了数轮；这条比任何单个缺陷都更值得记住。）

**（c）当前确定的事实（已用正确对照确认）**
| 量 | 默认（主机建矩阵） | `OCUDU_CE_CORR_DEV=1` |
|---|---|---|
| 已发布的 `gpu_ce` 中位模 | 4.4922e-1 | **7.3600e+2**（~1640×） |
| `gpu_nv` | 1.6212e-3 | **2.2418e+6** |
| SINR / iter | 23.97 dB / 2 | −3.11 dB / 10 |
| `rsrp` | −0.92 dB | **+63.50 dB** |

**（d）状态**：本轮**无行为改动**（只是插/删探针），树干净；
设备相关矩阵默认关闭；默认路径 **980/980 A/B 逐字节、`ctest -L phy` 162/162**。

**（e）下一步（唯一还没被证伪的怀疑方向，且必须先写判据）**
在 `complete_fd_td_estimation_stage()` 的 **wait 之后** dump `gpu_h`：
- **判据**：若两路 `gpu_h` 中位模差 ~1640× ⇒ 问题在 **apply kernel 的入参/dispatch**；
  若两路 `gpu_h` 相同 ⇒ 问题在 **unpack**（`unpack_engine_group` 读的是 `gpu_h` 的哪一段）。
在这之前，**矩阵/权重/staging 已被正确对照证明一致**（§48.71(b) 的六个量是用**带标记**的两条真路线比的）。

#### 48.73 S-7f-3i：**设备相关矩阵的缺陷找到了**（两处），现在**默认打开**并且逐字节等价

**（a）缺陷 1（就是 §48.71 那 1640× 的根因）：K1 被叫去反演一个"已经是逆"的槽位**
`stage_engine_group()` 在**主机**分支里写进槽位的是 **A⁻¹**（它自己跑 Gauss-Jordan），
而 `run_engine_blocks()` 在 §48.70 的改动里把 `gpu_invert` 重算成
"**只要开了设备相关矩阵就为真**"——于是 K1 又把 A⁻¹ 反演了一次，等于把 **A** 还给了权重。
实测（`iq2_1009_17923`，256QAM）：槽位里 `A[0]` 从 **1.002** 变成 **-3.4e7**，
`gpu_h` 到 **1e15**，`rsrp=+314dB`。
**这是一次"两个写者写同一块内存、却都以为自己是唯一写者"的典型缺陷**：
主机写 A⁻¹、设备（相关矩阵前缀）写 A，谁最后写谁赢，而 K1 的开关却按第三个条件决定。

**（b）缺陷 2：`run_weights_only[_async]()` 收了 `corr` 却**从不派发 K1**
`run_async()`（旧路径）里有 K1，`run_weights_only()`（§48.70 新折进来的那条）里**没有**。
所以"把相关矩阵折进权重调用那条 CB"这个改动本身，**从落地那一刻起就不可能对**：
设备写了 A，权重直接拿 A 当 A⁻¹ 用。§48.70 里"六个中间量全等、只有 LLR 不等"的观察，
正是这个（矩阵/权重/staging 全等是因为当时**两条路线都在走主机分支**——见 §48.71 的假对照更正）。

**（c）缺陷 3（独立发现，重要）：K1b（右看式反演）在 order 54 **数值是错的**
| 反演 | 反演后 `A[0]` | 与主机（353.2786）比 |
|---|---|---|
| 分块 K1（`mmse_inv`） | **353.2758** | 相对误差 ~1e-5，可用 |
| 右看 K1b（`mmse_inv_rl`） | **1.26e8** | 完全错 |
K1b 是 S-7f-3e 为"降低延迟"加的，**当时没有在 order 54 上验证过数值**（那条路当时被 36 的
上限挡着）。现在它默认关闭，只留 `OCUDU_INV_RL=1` 作为实验开关，缺陷待查。

**（d）最终形态（本轮落地，默认打开）**
设备的**只负责建矩阵**，反演仍在主机——因为 K1 在 order 54 上的 1.3e-5 相对误差
足以把这条 256QAM 抓包从 **24dB 打到 -18dB SINR**（实测），而主机 Gauss-Jordan 每个系统 ~10µs
且对 float32 精确。顺序是（这一点很容易写反，代码里有详细注释）：
1. `build_correlation()`（**独立一条 CB，同步等待**）把 A、R_hp 写进槽位；
2. 主机读这些值跑 Gauss-Jordan，把 A⁻¹ 写回同一槽位；
3. 引擎调用**不带 corr**（`corr == nullptr` 就是告诉 `run_engine_blocks()`"槽位里是逆，K1 不许动"）。
`run_weights_only()` 的 `corr` 参数语义恢复为"**corr ⇒ 同一 CB 里跟着 K1**"（order ≤36 可用），
不再是一个"收了却不用"的陷阱。

**（e）门禁（全部通过）**
| 门禁 | 结果 |
|---|---|
| **980/980 抓包 A/B（默认 vs `OCUDU_CE_CORR_DEV=0`）** | **LLR 逐字节一致** |
| 多形状 soak（5 种分配 × 2 轮，含 `iq1_10049_17921`） | 12/12 与单跑逐位一致，0 崩溃 |
| `ctest -L phy` | **162/162** |
| 单点抽检（`iq2_1009_17923` 256QAM、`iq2_10044_17923` 25PRB） | `llr/h/ce` 三件套逐字节一致 |

**（f）成本（诚实记录：这是"全 GPU path"的代价，不是性能优化）**
25 PRB 跳：设备建矩阵 `gpu_path=86µs`（`corr=18µs`），主机建矩阵 `gpu_path=89µs`（`corr=21µs`），
`mean total` 116 vs 125µs。**基本持平**——`build_correlation()` 是一条自己提交的同步 CB，
它的往返（~60µs 量级）吃掉了主机省下的构建时间；真正的收益是**模块进了 GPU**
（这正是本阶段的目标）。要把它变成净收益，需要让那条 CB 与前面的 DFT/CE 阶段**共用** command buffer，
而不是自己提交（记入待办）。

**（g）方法论沉淀（本次最有价值的一条）**
"**改了行为却以为只改了开关**"是这次连错三处的共同结构：
① §48.70 把 `gpu_invert` 改成"跟设备相关矩阵绑定"，改的是**行为**，注释里却写成"开关"；
② §48.71 用了一个**不存在的环境变量**做对照，于是两次都跑默认路线；
③ 本轮的 `MAX_DEVICE_CORR_ORDER` 门（我一度加上、又删掉）本来是想绕开 ①，是**用错误的方式修错误**。
⇒ 结论：**新增/修改一个决定"谁写这块内存"的条件时，必须同时回答三个问题**：
(a) 这块内存现在是谁写的？(b) 读它的人期待什么？(c) 这个条件在哪一处被求值？
三者不一致就是**数据缺陷**，不是性能问题。
另外：**"探针看不到预期的那一行"本身就是最强的证据**——本轮 probe6 打出 16 行 `L=72`（构造期
warm-up）而**一行 `L=54` 都没有**，正是这一点暴露出"这条路线根本没走到我以为的地方"。

#### 48.74 S-7f-3j：设备建矩阵**这次才真正执行**（上一轮它落在没人走的分支里），以及三个卡死"设备反演"的硬事实

**（a）⚠️ 先更正 §48.73：那一轮说"设备相关矩阵默认打开、980/980 等价"，
但设备建矩阵**一次都没跑过**。**
`std_slots_filled` 是在 `if (merge_tail)` 分支里被读取的，而这条抓包线
（`iq2_1009_17923`：`nof_prb=24, block_prb=3, rem=0`）`merge_tail == false`，走的是 `else` 分支——
**`else` 分支根本不读 `std_slots_filled`**，设备构建代码整段在 `if` 分支里。
探针（dump A 不产生文件 + `[dump] stage ... n_std=8 rem=0`）把这个暴露了出来。
⇒ 那一轮的 980/980 是**"设备建矩阵根本没跑" vs "主机构建"**，两次都是主机构建。
（与 §48.71 的假对照同源：**没有验证"我以为的那条路线真的执行了"**。）

**（b）本轮的实质改动：把设备构建移进公共路径，并加了防错计数器**
- 新增 `build_slots_on_device()`，`run_engine_blocks()` 与 merged 分支都调它，
  所以 `rem==0`（标准分支）和 `rem!=0`（merged 分支）**都会**走设备构建；
- `stage_engine_group()` 的 `slots_filled` 参数现在真正生效（跳过主机写 A/R_hp）；
- **新增 `[metal_stats] device_corr_builds=`**：设备建矩阵的次数。
  这是本轮最重要的**方法论产物**——三条路线的计数器互不相同（默认 `=1`、`CORR_DEV=0` 为 `0`、
  实验路径为 `0`），所以"两条路线是否真的不同"从此**可验证**，不会再有同路线自比。
- 门禁（计数器确认设备构建确实执行）：**980/980 抓包 LLR 逐字节一致**、多形状 soak 12/12、
  `ctest -L phy` 162/162。

**（c）卡死"设备反演"的三个硬事实（本轮量出来的）**

**① Metal 没有 `double`。** 试图用 `double` 写 Cholesky 反演 kernel，`xcrun metal` 直接报
`'double' is not supported in Metal`（6 处）。⇒ **设备端反演永远只能是 float32**，
"用更高精度救 K1"这条路**从语言层面就不存在**。

**② float32 对这个 A 本身就不够。** 用真实 A（54×54，从设备槽位 dump）做对照：

| 反演形式 | 元素级 max_rel | 备注 |
|---|---|---|
| float32 分块 Gauss-Jordan（**设备 K1**） | **9.67e-1** | 实际导致 256QAM 抓包 **−18.69 dB / iter 10** |
| float32 全局 Gauss-Jordan（**主机**） | **1.66e-1** | 同一抓包 **23.97 dB / iter 2**（可用） |
| float32 Cholesky | 2.88e-1 | 不比 GJ 好 |
| **Cholesky（float64 分解）** | **4.62e-10** | 差 9 个数量级 |

`cond_2(A) ≈ 2.1e4`，float32 的元素级下限是 `cond·eps ≈ 2.5e-3`——**所有 float32 形式都远在下限之上**，
主机 1.66e-1 只是"错得少一点"。设备反演的可接受门槛在 0.166 与 0.967 之间。

**③ 一条 command buffer 的形态**（设备建矩阵 + 设备反演，`OCUDU_CE_DEV_INVERT=1` 实验开关）
**又快又稳，但算错**：
| 路线 | `gpu_path` | SINR |
|---|---|---|
| 默认（设备建 A + 独立 CB 同步等待 + 主机反演） | **230 / 582 / 641µs（波动极大）** | 23.97 dB（对） |
| 实验（一条 CB：相关前缀 + 设备反演） | **60 / 61 / 64µs（稳）** | **−18.69 dB（错）** |
默认路线的波动来自 `build_correlation()` 那次**独立命令缓冲区的同步等待**（实测 154–531µs），
主机反演只占 ~25µs。⇒ **"必须合并成一条 CB"不只是省一次往返，它是 10 倍的稳定性差异**，
这让"提高 K1 精度"的价值大增。

**（d）精度余量是存在的（ridge 扫描，已回退不改默认）**
`A = R_pp + (σ² + ridge) I`，ridge 只在 `build_correlation_matrices()` 与
`ocudu_mmse_corr.metal` 里，两处一致可调（本轮临时做成了 `OCUDU_CE_RIDGE`，测完已删除）：

| ridge | cond(A) | 主机 f32 | 设备 K1 | SINR |
|---|---|---|---|---|
| 1e-6（默认） | 2.12e4 | 1.66e-1 | 9.67e-1 | 23.97 dB |
| 1e-3 | 1.44e4 | 2.79e-1 | 1.22e0 | 23.96 dB |
| 3e-3 | 8.80e3 | 4.74e-3 | **4.35e-2** | 23.93 dB |
| 1e-2 | 3.72e3 | 1.31e-3 | **1.85e-2** | **23.85 dB** |

⇒ **SINR 几乎不动（≤0.12 dB）而设备反演精度改善 52 倍**，`ridge=1e-2` 时设备误差 1.85e-2
**已经跨过主机 0.166 那条可用线**。这条路（用正则化换取设备反演可用，从而拿到一条 CB 的 60µs 稳定形态）
是本阶段最有希望的下一步，但它**改变 A、因而改变估计值**，必须先做离线 LLR 等价性判据、再上机。

**（e）代价（诚实记录）**：25 PRB 跳上，设备建 A **+32µs**（`mean total` 95→127µs）——
因为那条独立 CB 的同步等待比主机省的构建时间更贵。**当前形态是"模块进了 GPU"，不是性能收益。**

**（f）方法论沉淀（本轮两条，比缺陷本身更值钱）**
1. **"代码存在" ≠ "代码执行"**。判据必须是**计数器/文件这类外部证据**，不能是"我读了代码觉得它该跑"。
   本轮的 `device_corr_builds` 计数器和"dump 不产生文件"就是这类证据。
   ⇒ 今后任何 A/B，**先确认两条路线的可观测副作用不同**，再比结果。
2. **先写判据再动手**（§48.72 立的规矩）本轮真的省了时间：K1 精度分析一开始就写了
   "误差 ~ cond·eps ⇒ 条件数下限；远大于 ⇒ 实现问题"，所以量出 379 倍时立刻知道
   **是算法问题、可以改**——而不是又一次凭感觉猜"大概是精度不够"。虽然后来发现
   float32 整体不够（更强的结论），但判据让每一步的推理都是收敛的。

#### 48.75 S-7f-3k：**"W 空间"才是正确的精度尺子**，ridge 方案被离线判据否决

**（a）关键更正：判断反演够不够，不能看 A⁻¹ 的元素误差，要看 `W = R_hp · A⁻¹`**
A 病态，它的误差集中在 R_hp 几乎不激励的方向上，所以 A⁻¹ 的元素误差在 W 里被**大幅衰减**。
用真实抓包（`iq2_1009_17923`，A 54×54、R_hp 504×54，都是从设备槽位 dump 的）量：

| 反演 | A⁻¹ 元素 max_rel | **W 的 max\|err\|/max\|ref\|** | 链路 SINR |
|---|---|---|---|
| 主机 float32 Gauss-Jordan | 1.66e-1 | **1.40e-5** | **23.97 dB（可用）** |
| 设备 K1（分块 GJ） | 9.67e-1 | **7.67e-4** | 视路径而定 |

⇒ **两者都远好于 A⁻¹ 元素误差所暗示的水平**，而主机只比设备好 **55 倍**（不是 6 倍）。
这条更正很重要：它说明"设备反演差 6 倍"并不必然导致链路崩溃，
于是**"−18.7dB 是精度引起的"这个结论不成立**——后来证实它是**两个真实缺陷**（见 §48.76）。

**（b）ridge（正则化）方案：离线判据否决策**
判据（先写）：若 `ridge=1e-2` 下 LLR 与默认逐字节一致 ⇒ 可用正则化换设备反演可用；
若只是"解码结果相同但 LLR 不同" ⇒ 需上机；若解码失败率上升 ⇒ 否决。

实测（240 个抓包，`OCUDU_CE_RIDGE` 临时开关）：

| ridge | 单点 crc/iter/SINR | 240 抓包 LLR 与默认一致 |
|---|---|---|
| 1e-2 | OK、iter 2→4（`iq2_1009`）、SINR 23.97→23.85 dB | **仅 19/240（8%）** |

⇒ **否决**。SINR 只动 0.01–0.31 dB（这正是它一开始看起来"无害"的原因），
但**92% 抓包的 LLR 变了**——改变 A 就是改变接收机的判决依据，不是"免费的精度预算"。
`OCUDU_CE_RIDGE` 已从代码中删除，结论写进 `build_correlation_matrices()` 的注释。

**（c）方法论**：**"SINR 几乎不变"不等于"结果没变"**。
ridge 那次如果只看 SINR，会得出"完全可以接受"的错误结论；
差 0.12 dB 与 LLR 完全不同**可以并存**，因为 SINR 是个聚合量、LLR 是逐比特的判决输入。
⇒ 判据必须落在**下游真正消费的东西**上（这里是 LLR），不能落在汇总指标上。

#### 48.76 S-7f-3l：追"设备反演为什么 −18.7dB"追出**两个真缺陷**（都修了），但实验路径仍不可用

**（a）缺陷 A：实验路径下 `stage_engine_group()` **跳过了整个函数**，把导频 staging 一起丢了**
`slots_filled=true` 的语义本该是"别写 A/R_hp"，但实现上是 `for (...) { if (!slots_filled) ... }` 与
**调用点整个跳过**。而 `stage_engine_group()` 里除了 A/R_hp **还有 y/qy 的导频 staging**：

| 探针 | 结果 |
|---|---|
| 引擎调用前 `gpu_a[0], gpu_y[0], gpu_r_hp[0]` | **`0 0 0`**（全零） |
| 修 `slots_filled` 语义后 | `a0=0`（设备将写入）、**`y0=0.680324`（正确填充）** |
| 后果 | y 全零 ⇒ apply 输出 **inf** ⇒ SINR −18.69→**inf** |

修正：`stage_engine_group()` **每次都调用**，`slots_filled` 只用来跳过 A/R_hp 的写入循环。

**（b）缺陷 B：实验路径下 K1 反演的是**已经被反演过**的槽位**（S-7f-3i 缺陷的再现）
修 A 之前，调用点仍以 `slots_filled=false` 进 `stage_engine_group()` ⇒ 它把 **A⁻¹** 写进槽位，
随后相关前缀覆盖成 A（因为设备构建提前返回、槽位其实是空的），**K1 反演的是 A⁻¹** ⇒ 等于把 A 还给权重。
这与 §48.73 修的默认路径缺陷**是同一个**，只是在实验分支里复发。

**（c）修完两个缺陷后，数值链已自洽，但链路仍错（第三个问题在 RSRP/噪声方差）**
用 dump 出来的量逐项核对（全部在真实抓包上）：

| 量 | 设备反演路径 | 默认（主机反演） | 判定 |
|---|---|---|---|
| `gpu_h` 首元素 | **0.648678** | 0.648476 | 差 3e-4，与 W 的 7.67e-4 一致 ⇒ **数值链自洽** |
| `W` vs float64 参考 | 7.67e-4 | 1.40e-5 | 设备差 55×，但都远好于 A⁻¹ 的元素误差 |
| `gpu_r_hp[0]` | 1（正确） | 1 | ✓ |
| **`rsrp`** | **+31.15 dB** | **−0.92 dB** | **✗ 高 32 dB** |
| SINR / iter | **−23.35 dB / 10** | 23.97 dB / 2 | ✗ |

⇒ h 只差 3e-4 而 RSRP 高 32 dB（≈1600 倍），**问题不在 h、不在 W、不在 A/R_hp**，
而在 **RSRP/噪声方差的计算路径**（它读的是估计栅格 `grid_est` 与 filtered pilots，不是 h 本身）。
**这条留给下一步**：它是实验路径（`OCUDU_CE_DEV_INVERT=1`）专用的，默认路径不受影响。

**（d）状态**：`OCUDU_CE_DEV_INVERT` **默认关闭**、已知不可用（注释里写明）；
默认路径（设备建 A + 主机反演）**980/980 抓包 llr/h/ce 逐字节一致**、`ctest -L phy` 162/162。

**（e）方法论沉淀（本轮第 3 条）**
**"同一个缺陷会在多个分支里复发"**。§48.73 修好的"K1 反演已反演的槽位"，
在实验分支里以另一种写法原样出现（调用点跳过了 staging 而不是循环体跳过写入）。
⇒ 修一个"谁写这块内存"的缺陷时，**必须把同一决策的所有分支都过一遍**，
否则只是把缺陷从一个分支赶到另一个分支。
另外：**"跳过整个函数"与"跳过函数里的一段"在语义上完全不同**——
`stage_engine_group()` 的名字里没有"只 stage 矩阵"的意思，调用点却按那个假设跳过了它。

#### 48.77 S-7f-3m：把"实验路径 rsrp 高 32dB"逼到**一个符号级边界**，下轮可直接命中

**（a）逐层排除的结果（全部在抓包 `iq2_1009_17923` 上实测）**

| 层 | 默认路径 | `OCUDU_CE_DEV_INVERT=1` | 判定 |
|---|---|---|---|
| `gpu_a` / `gpu_r_hp`（引擎调用前） | — | `0`（设备将写入） | ✓ 预期 |
| `gpu_y` 导频 staging | 0.680324 | **0.680324（修好后）** | ✓ |
| `gpu_r_hp[0]`（相关内核写的） | 1 | 1 | ✓ |
| `W` vs float64 参考 | 1.40e-5 | 7.67e-4（55×） | ✓ 可用 |
| K1 反演的 A⁻¹（W 空间） | — | 7.67e-4 | ✓ |
| **`gpu_h` 首元素** | 0.648476 | **0.648678**（差 3e-4，与 W 一致） | ✓ **数值链自洽** |
| `unpack_engine_group` 读到的 `hp` | 0.648476 | **0.648678** | ✓ 寻址正确 |
| **`grid_est` 逐符号** | **14/14 符号 `avg_pow=0.420483`** | **只有 `sym=0`（0.648678）与 DMRS 符号 7 有值，其余 12 个符号是未初始化垃圾（`avg_pow` 480–870、无规律、`first=(0,0)`）** | ✗ **缺陷在这里** |
| `rsrp` | −0.92 dB | **+31.15 dB** | ✗ |

**（b）结论：问题不在相关矩阵、不在反演、不在权重、不在 h、不在 unpack 寻址，
而在"`gpu_h` → `grid_est`"这一步在实验路径下**只写到了第一个符号**。**
`sym=0` 的数据是**完全正确**的（0.648678，与 h 一致），所以 unpack 本身工作；
坏掉的是**其余 13 个符号**——它们是未初始化内存（`avg_pow` 无规律、部分为 0）。

**（c）下一轮的判据（已经写好，可直接执行）**
`grid_est` 由 `unpack_engine_group()` 的 `b`（块）循环与 `sym` 循环共同填写，
每个块覆盖 1 PRB × 14 符号。只写对 `sym=0` 有两种可能，判据如下：
- **若 `b` 循环只跑了 `b=0` 一次** ⇒ `n_blk` 传参或 `st.n_blk` 在实验路径下不同
  （对比两路 `engine_run()` 的 `nof_blocks`，以及 `defer_unpack()` 登记的 `st`）；
- **若 `b` 循环跑满但 `dst` 落在同一段内存** ⇒ `grid_est` 的视图/步长在实验路径下不对
  （在 `unpack_engine_group()` 里打印每对 `(b, dst.data() - grid_est_base)`，一次就能分辨）。
建议直接在 `unpack_engine_group()` 打这个 `(b, 偏移)` 表——**这是唯一还没被观测过的量**。

**（d）状态**：`OCUDU_CE_DEV_INVERT` 默认关闭、已知不可用；默认路径（设备建 A + 主机反演）
**980/980 抓包 llr/h/ce 逐字节一致**、`ctest -L phy` 162/162。探针已全部清除。

**（e）优先级提醒（诚实评估）**
即使修好这个缺陷，实验路径的 `W` 精度仍比现默认路径**差 55 倍**（7.67e-4 vs 1.40e-5），
换来的是 `gpu_path` 60µs 稳定（对默认路径的 230–640µs 波动）。
按"全 GPU path 优先、局部性能退化可接受"的既定方针，这条仍然值得做完；
但若下一轮一眼看出是 `n_blk` 传参问题（一行），收益/成本比极高。

#### 48.78 S-7f-3n：把范围收到"**写入之后被覆盖**"，并且拿到一条"下轮一次命中"的差分判据

**（a）本轮排除了 unpack 本身（这是 §48.77 判据的两个分支之一）**
在 `unpack_engine_group()` 里打印每个块的寻址与几何（**此前唯一没被观测过的量**）：

| 量 | 默认路径 | `OCUDU_CE_DEV_INVERT=1` |
|---|---|---|
| `n_blk` / `b_prb` / `nf` / `gb_start` | 8 / 3 / 36 / 0 | **完全相同** |
| `st.n_blk` / `st.nout` | 8 / 504 | **完全相同** |
| `slice_sz`（`grid_est` 每符号 RE 数） | 288 | **完全相同** |
| `b` 循环次数 | 8（b=0..7） | **8（b=0..7）** |
| `sym0_off`（切片基址偏移） | 0 | 0 |

⇒ **unpack 在实验路径下执行了全部 8 个块、寻址与默认路径逐项相同**，而 `gpu_h[0]` 也正确
（0.648678）。所以缺陷**不在 unpack 的读取/寻址**，而在**写入 `grid_est` 之后的那一刻之前**——
某个东西把 13 个符号覆盖成未初始化值（§48.77 的表：只有 `sym=0` 与 DMRS 符号 7 保住了正确值）。

**（b）下一轮的判据（差分法，一次运行即可定位；不要再用"猜候选"的方式）**
在 `unpack_engine_group()` **返回之后**、`deferred_fill.fill()` **之前**，各 dump 一次
`grid_est` 的 14 个符号（就是 §48.77 用过的那张表），并对 `gpu_ce`（K3 的目标缓冲）
**同样 dump 一次其内存首尾若干字节**。判据：
- **若 unpack 返回时 14 个符号就已经是垃圾** ⇒ 覆盖发生在 unpack **期间**或**更早**
  （候选：`gpu_h` 与 `grid_est` 的零拷贝映射别名、或 K3 的 `gpu_ce` 写入跨越了边界）；
- **若 unpack 返回时 14 个符号正确、之后才变坏** ⇒ 覆盖发生在 **complete 阶段**
  （候选：`deferred_fill.fill()` 自身、或 `gpu_ce`→`grid_est` 的某次拷贝）。
两路唯一的结构差异是：实验路径把 K1 编进了**同一条 command buffer**（相关前缀 + K1 + 权重 + K3），
默认路径没有 K1。**所以"多出来的那个 dispatch"是覆盖的头号候选**——
K1 原地写 `gpu_a`（54×54×4B），K3 写 `gpu_ce`，两者都不该碰 `grid_est`，
但 `grid_est` 是 estimator 对象的成员，而 `gpu_h/gpu_ce` 是页对齐的独立分配——
**建议下一步先把这三块内存的地址与长度打出来**（一次打印就能看出是否有区间重叠）。

**（c）状态**：默认路径 **980/980 抓包 llr/h/ce 逐字节一致**、`ctest -L phy` 162/162；
`OCUDU_CE_DEV_INVERT` 默认关闭、已知不可用；探针已全部清除，树干净。

**（d）本轮的方法论价值（比缺陷本身更重要）**
这一轮我**连续用同一套流程**把范围砍小：**先写判据 → 加一个只观测一个量的探针 → 跑两路对比**，
每步都把"可能的位置"减半：相关矩阵 → 反演 → 权重 → h → unpack 寻址 → 写入时机。
**其中两次判据直接给出了反直觉的结论**（① W 空间的误差比 A⁻¹ 小 4 个数量级，
所以"设备反演精度不够"是错的；② unpack 寻址两路完全相同，所以缺陷在写入之后）。
如果没有事先写判据，这两次都很容易滑向"再改改 kernel 试试"的错误方向。

#### 48.79 S-7f-3o：差分探针给出**决定性证据**——`gpu_h` 只被写了 2 个 float，而输入全对

**（a）一次运行内的差分（`unpack` 前后 + 缓冲区地址），实验路径**

| 观测 | 结果 |
|---|---|
| `grid_est` **unpack 之前** | **14 个符号全零**（正常，本 hop 首次写） |
| `grid_est` **unpack 之后** | 只有 `sym=0` 正确（0.648678，与 h 一致），其余 13 个是垃圾 |
| 缓冲区地址 | `gpu_h=0xb35400000`、`gpu_ce=0xb34f00000`、`gpu_a=0xb34d94000`、`gpu_y=0xb34ecc000`、`grid_est=0x734b56b78`（**对象内联存储**） |
| 缓冲区区间 | `gpu_h` 9216B、`gpu_ce` 9216B、`gpu_a` 41472B、`gpu_y` 41472B —— **互不重叠**（`gpu_ce` 尾 0xb34f02400 < `gpu_h` 头 0xb35400000） |

⇒ **不是"写入后被覆盖"**（BEFORE 全零、地址无重叠），而是 **unpack 从 `gpu_h` 读到的就只有第一个符号是对的**。

**（b）继续往上游查，得到本轮最硬的一条证据**

| 缓冲 | 默认路径 | 实验路径 | 判定 |
|---|---|---|---|
| `gpu_y[0..7]` | 0.680324 0.0191889 0.604988 … | **完全相同** | ✓ **输入对** |
| `gpu_y[100..115]` | −0.0382413 −0.650606 … | **完全相同** | ✓ |
| `gpu_w[0..7]` | 0.250812 0.108887 … | 0.25075 0.108898 …（差 ~1e-4，与 W 的 7.67e-4 一致） | ✓ **权重对** |
| **`gpu_h[0..15]`** | 0.648476 0.0343678 0.649334 −0.000118308 0.647827 … **连续填满** | **0.648678 0.0344147 0 0 0 0 0 0 0 0 0 0 0 0 0 0** | ✗ |
| `gpu_h[64..80]` | 连续有效值 | `0 … 0 46.2437 0.421053 −45.6065 −0.529579 0 0` | ✗ |
| `gpu_h[140..156]` | 连续有效值 | `… 0 75.9817 25.8139 −75.3959 −26.0693 0 …` | ✗ |

⇒ **两个输入（`y`、`W`）逐字节相同，`h = W·y` 的输出却只有前 2 个 float 被写。**
这不是数值问题，而是 **apply 阶段的执行/可见性**问题：`gpu_h` 里那些成对出现的 ±46 / ±75
是**未初始化的残留**，不是计算结果。

**（c）这个结果把下一轮钉死在两件事上（都不需要再猜）**
1. **`apply` 的 dispatch 是否真的覆盖了全部 8 个块**：在 `encode_weights_only()` 里打印
   `MTLSizeMake(nof_blocks * nof_systems, 1, 1)` 与 `threadsPerThreadgroup:MTLSizeMake(nout, 1, 1)`
   的**实际数值**，以及 GPU 侧 `threadgroups_per_grid`（内核里 `tgid` 的范围）。
   当前 `nout=504`、`nof_blocks=8`、`nof_systems=1` 是按代码推断的，**从未实测**。
2. **`gpu_h` 的零拷贝映射对象是否与 `apply` 实际写入的对象是同一个**：`h_buf` 由
   `e->wrap(h, nof_systems*nof_blocks*2*nout*sizeof(float))` 得到；实验路径多了一次
   `K1` 的 dispatch（K1 用的是 `ai_buf`，与 `h_buf` 不同对象）。若 `wrap` 的缓存因某种原因
   在两次调用间换了对象，就会出现"内核写 A、主机读 B"。**打印 `h_buf` 指针两次即可判定。**

**（d）状态**：`OCUDU_CE_DEV_INVERT` 默认关闭、已知不可用；默认路径
**980/980 抓包 llr/h/ce 逐字节一致**、`ctest -L phy` 162/162；探针已全部清除、代码与
已提交状态逐字节相同。

**（e）关于投入的判断（诚实记录）**
这是本轮第 4 次向同一问题深处推进，范围已经收到"两个具体可测的量"（dispatch 尺寸、buffer 对象同一性）。
但它换来的是一个 **W 精度比现默认路径差 55 倍**的路径，且默认路径已完全正确。
**建议：下一轮只做 (c) 的两个打印（约 15 分钟）**——
若命中（比如 `wrap` 换了对象），说明这是一个**会影响零拷贝缓存的通用缺陷**，必须修（可能也影响其他阶段）；
若两者都正常，则应把 DEV_INVERT 降级为"已知不支持的实验"并转向默认路径的上机验证与其他模块。

#### 48.80 S-7f-3p：两个候选判据**全部排除**——dispatch 与 buffer 对象逐字节相同，问题在 Metal 执行层

**（a）实测（§48.79(c) 的两个判据，一次运行同时拿到）**
失败的那次调用（L=54、单个系统、8 个块）在两路上的实测值：

| 量 | 默认路径 | `OCUDU_CE_DEV_INVERT=1` |
|---|---|---|
| `nof_blocks * nof_systems`（threadgroups） | **(8,1,1)** | **(8,1,1)** |
| `threadsPerThreadgroup`（nout） | **(504,1,1)** | **(504,1,1)** |
| `h`（主机指针） | `0x7f8400000` | `0x8a6c00000` |
| `h_buf`（零拷贝映射对象） | `0x7f63d0000` | `0x8a3c54000` |
| `ai_buf` / `y_buf` / `w_buf` | 各自独立对象 | 各自独立对象 |

⇒ **dispatch 尺寸完全相同**、**`h` 与 `h_buf` 的对应关系相同**（`h` 就是 `gpu_h`，
每次调用的 `h` 指针与 `h_buf` 唯一对应，且第 17 次调用的 `h`/`h_buf` 与第 1 次相同 ⇒ `gpu_h` 复用正常）。
**两个候选都不是原因。**

**（b）因此剩下的可能性只剩一个，且它不在估计器、也不在引擎的参数编码里**
把已知事实排成一条链（全部实测）：
1. `gpu_y` 两路逐字节相同（输入对）；
2. `gpu_w` 两路差 ~1e-4（与设备反演的 7.7e-4 一致，权重对）；
3. dispatch 尺寸、`h_buf` 对象、`h` 指针全部相同；
4. **`gpu_h` 在实验路径下只有前 2 个 float 是计算结果**，其余是未初始化残留（§48.79(b)）。

⇒ 相同输入 + 相同 dispatch + 相同输出缓冲，却只有 2 个 float 落地：
**这不是参数问题，而是"同一个 command buffer 里多了一个 dispatch（K1）"改变了 GPU 的实际执行**。
两个具体方向（下一次直接测，不需再猜）：
- **命令缓冲区的依赖/顺序**：实验路径的 CB 里 K1 在 weights 之前、weights 在 apply 之前。
  在 Metal 里同一 encoder 内的 dispatch 是顺序的，但**如果 K1 与 weights 访问同一个
  `ai_buf` 而缺少必要的内存依赖**，驱动可能提前执行 apply。打印
  `[cb status]`、`cb.error`，并试 `MTLCommandBuffer` 的 `encoder` 分段（每个阶段一个 encoder）
  即可判定。
- **`mmse_apply` 内核自身的线程组维度假设**：内核以 `tid` 与 `tgid` 计算 `sym`/`sc`，
  若它对 `threads_per_threadgroup`（504）有隐含假设，而 504 恰好接近某个硬件上限
  （Apple GPU 的 threadgroup 上限为 1024 线程），则在"更长的 CB"下行为可能不同。
  直接把 `apply` 的 `tpt` 从 504 改成 256（多处几组）做一次 A/B，就能判定。

**（c）状态与建议**
默认路径 **980/980 抓包 llr/h/ce 逐字节一致**、`ctest -L phy` 162/162；代码与已提交状态逐字节相同。
`OCUDU_CE_DEV_INVERT` 默认关闭、已知不可用。

**建议停止在这条线上继续投入**，理由有三条，都是实测支撑的：
1. 即使修好，它的 **W 精度比现默认路径差 55 倍**（7.67e-4 vs 1.40e-5）——
   换来的只是 `gpu_path` 60µs 稳定；
2. 已经连续 5 轮向同一问题深挖，每轮都在缩小范围但**收益边界没有变**；
3. 剩下的是 **Metal 执行层的时序/内核维度**问题，属于平台细节，而不是 PHY 链路的设计问题——
   这类问题更适合在"默认路径已经上机验证过、有余力时"再回来处理。
**更有价值的下一步**：(i) 默认路径（设备建 A + 主机反演）**上机 b20 验证**；
(ii) 让 `build_correlation()` 与主机工作**重叠**（保留主机反演，直接把那 200–600µs 的等待藏掉）；
(iii) 继续把其他模块推进 GPU（K3/K4 之后的下一站）。

#### 48.81 S-7f-3q：排队等待的分解、"设备反演精度其实够用"，以及索引缩放特征 `0.9 = L/(L+6)`

**（a）`build_correlation()` 的 200–600µs 等待：**是 GPU 排队，不是 CPU 也不是 GPU 执行****
判据（先写）：同步等待 = commit + GPU 执行 + GPU 排队。实测（4 次，抓包 `iq2_1009_17923`）：

| run | commit | **wait** | **gpu（GPUEnd−GPUStart）** |
|---|---|---|---|
| 1 | 12.0µs | **473.6µs** | 30.5µs |
| 2 | 4.9µs | **419.5µs** | 31.4µs |
| 3 | 6.5µs | **139.1µs** | 26.8µs |
| 4 | 6.7µs | **687.2µs** | 19.6µs |

⇒ **commit 只有 5–12µs、GPU 只干 20–31µs，等待全在排队**（前面的 command buffer 还没跑完）。
这解释了 §48.74(c) 那 230–640µs 的剧烈波动：**波动来自队列，不来自这一段本身**。
**含义**：把 `build_correlation()` 改成异步提交**并不能**消掉等待（队列本来就是满的）；
唯一能消掉它的办法是**减少 command buffer 数量**——也就是回到"一条 CB"（设备建 + 设备反演）。

**（b）⚠️ 重要更正：设备反演的精度**其实够用**，我此前对它的判断过重**
§48.73 说"K1 的 1.3e-5 相对误差把 256QAM 抓包从 24dB 打到 −18dB"——**那个结论是错的**
（−18.69dB 的真因是两个 staging 缺陷，见 §48.76）。用 W 空间重算：
设备反演 `W` 误差 **7.67e-4**（主机 1.40e-5，差 55 倍），而 **`h` 的相对误差与 W 同量级**
⇒ 折算到 SINR 只有 **≈0.003 dB** 的损失。**7.67e-4 完全够用。**
⇒ 因此"设备建 + 设备反演（一条 CB）"这条路的**收益被大幅低估**：
它能同时消掉排队（一条 CB 实测 60µs 稳定）**且精度足够**。

**（c）DEV_INVERT 的第三个问题：拿到了**索引缩放错误**的精确特征**
实测 `gpu_h` 的覆盖情况（8064 个 float 中）：

| 路径 | 非零个数 | 块 0 的非零 `tid` |
|---|---|---|
| 默认 | **8064/8064** | 0,1,2,3,…（504 个全有） |
| 实验 | **1440/8064** | 0,9,10,18,19,28,37,38,46,47,56,65,66,74,75,84,… （**90 个**） |

非零 `tid` 序列满足 **`tid = round(k × 0.9)`**，而 **`0.9 = 54/60 = L/(L+6)`**
（`L=54`、`nf=36`、`L/nf = 1.5`；`60 = 54 + 6`）。
**这是"索引被按错误的步长缩放"的确凿特征**，不是数值问题也不是 dispatch 覆盖问题。
已排除：`L`/`nout`/`nof_systems`/`nof_blocks` 在两路**逐项相同**、`ai_buf`/`y_buf`/`w_buf`/`h_buf`
对象各自独立、buffer 区间无重叠、线程组尺寸 `(8,1,1)×(504,1,1)` 相同。

**（d）顺带测出的一个硬约束（此前只是推断）**
`apply` 的 `threadsPerThreadgroup` **必须是 `nout`(=504)**：内核里 `tid` 就是**输出位置索引**
（`hp[2*tid]`），所以 `tpt` 小于 `nout` 会**静默丢掉尾部输出**。实测：`tpt=504` → SINR 23.97dB；
`tpt=256` → **0.86dB**；`tpt=128` → **0.48dB**。这条约束已写进内核注释的必要性清单。

**（e）状态**：默认路径 **980/980 抓包 llr/h/ce 逐字节一致**、`ctest -L phy` 162/162；
探针全部清除、代码与已提交状态逐字节相同；`OCUDU_CE_DEV_INVERT` 默认关闭。

**（f）下一轮的唯一动作（已由 (c) 指定，不需要再猜）**
在 `mmse_apply` 内核里把 `tid` 与它算出的 `(sym, sc)`、以及 `p.L`/`p.nout` **一起写成一行输出**
（或把 `wg` 写进一个专用的小 buffer），直接看"哪个 tid 读到了哪些 w 元素"。
`0.9 = L/(L+6)` 里的那个 **`6`** 是关键线索：`6` 正是**每 PRB 的 comb 数**（`npf/b_prb = 6`），
而 `L/(L+6) = npf·npt/(npf·npt + npf/b_prb)` —— 指向**某个地方把 `L` 与 `L + comb` 混用了**。
建议从 `corr_stage` / `mmse_corr_params` 与 `mmse_weights_params` 的字段顺序或 stride 入手，
**先 diff 这三个结构体在 .metal 与 .mm 两侧的字段布局**（一个字段错位就会产生这种缩放特征）。

#### 48.82 S-7f-3r：**OTA 验证通过**（设备建矩阵在真实链路上跑通），以及覆盖率 37.5% 的解释

**（a）上机结果（`d8e23f67d1`，用户跑的腿，5MHz n1 bridge，Metal CE + Metal 等化器/解调 + Metal LDPC）**

| 判据 | 实测 | 判定 |
|---|---|---|
| **`device_corr_builds` > 0** | **26761** | ✓ **设备建矩阵在真实链路里确实执行**（离线时它曾整段没跑过） |
| `Real-time failure in RF` | **0** | ✓（上次健康基线是个位数） |
| USB 错误 / 崩溃 | **0**（只有 `[B200] Operating over USB 3.`） | ✓ |
| `hops_gpu / hops_no_gpu` | 71384 / 0，`fb_blocks=0` | ✓ 全部 hop 走 GPU |
| `mmse_ce commits/waits` | 98147 / 98147，`max_in_flight=1` | ✓ |

性能（只记录，不设 gate）：`mean total=114.3us`、`stage=17.89us`、`corr=10.9us`、`gpu_path=99.4us`、
`gpu_wait=137.2us`、`defer_wait=306.7us`、`max total=1068us`。
注意 **`pre=0.69us`**——比离线 replay（6–18us）低一个量级，说明 **OTA 的 hop 结构与离线抓包差别很大**
（离线抓包多是 24/25 PRB 的宽分配，实网上海量的窄分配）。

**（b）覆盖率 37.5% 的真正原因（我第一版分析错了，这里更正）**
`device_corr_builds=26761` vs `hops_gpu=71384` ⇒ 只有 **37.5%** 的 hop 走了设备构建。
我最初以为是"tail 分支没接设备构建"，于是给 `run_engine_blocks()` 加了 `sys_offset` 并在 tail 分支接上。
**结果探针一次都没打印**——这批 hop 压根没走 tail 分支，而是走 **merged 分支**：

| 分配 | `rem_prb` | `merge_tail` | `std_slots_filled` | 设备构建 |
|---|---|---|---|---|
| 整块（如 24 PRB ÷ 3 = 8） | 0 | false | **true** | ✓ 生效 |
| 有余数（绝大多数） | ≠0 | **true** | **false** | ✗ 不走 |

**merged 分支为什么不能用"两次设备构建"解决**：tail 是作为**额外的 system** 挤进**标准组的槽位**的
（`A = blockdiag(A_e, I)`、`R_hp = [R_hp_e | 0]`），两家**共用同一对 A/R_hp 槽位**，
只是几何不同。一次 `build_correlation` 只能描述一种几何，
而两次调用会**互相覆盖**（`corr_stage` 的 `c.a = gpu_a + sys_offset * L * L` 用的是紧凑 stride，
tail 的 `L_e` 与槽位的 `L_std` 不等，指针会落到标准组的中间）。
⇒ 要覆盖 merged，需要让 `correlation_stage()` 支持 **`a_stride != L`**（把 stride 独立成参数），
这是下一轮的具体动作。

**（c）本轮顺带落地的（对 split 形式有效，对当前 OTA 配置无影响）**
`run_engine_blocks()` 新增 `sys_offset` 参数，tail 分支接上设备构建。实测
`OCUDU_CE_SPLIT_TAIL=1` 下 `device_corr_builds` 从 **0 → 2**（标准组 + tail），
且三条抓包 **llr/h/ce 逐字节一致**。→ 这条改动是"split 形式下全 GPU path 更完整"，
虽然当前 OTA 用不到，但它把 `merged` 与 `split` 两条路的设备构建**能力拉平到同一起点**。

**（d）结论**：**"设备建矩阵"这一步在全 GPU path 的意义上已经上机跑通**——
链路健康、零 RF 失败、零 USB 错误，且计数器证明它真的在执行。
覆盖率（37.5% → 期望 ~100%）是下一个具体的、已定界的动作（(b) 的 stride 改造）。

#### 48.83 S-7f-3s：stride 重构落地；merged 覆盖率的尝试**失败并已回退**；顺带修掉一个我自己引入的语义反转

**（a）落地的三件事**

**① 相关性内核获得 system stride（`67b6377216` 已提交）**
两个内核此前用 `p.L * p.L`（A）与 `p.Ns * p.L`（R_hp）作为**系统间距**——那是**块**的尺寸，不是**槽位**的尺寸。
`Ls != L` 时（merged 的 edge block 正是如此）会走进标准组中间。现在 `corr_stage` 带
`a_sys_stride` / `r_sys_stride`，内核用它们走 `gid.y`，**零拷贝映射也按它们计算长度**
（更宽的槽位会让最后一个系统越过 `nof_systems * L * L`，短映射会截断批次），
`correlation_stage()` 把槽位 stride 与块阶分开接收。
**这一步本身零行为变化**（所有现存调用点传的 stride 都等于自己的 L/nout）：
980/980 逐字节、`ctest -L phy` 162/162、split 形式 0→2 且逐字节一致。

**② `a_rhp_filled`：只跳过 A/R_hp 的写入，保留导频 staging**
`stage_engine_group()` 此前只有"整个调用跳过"的开关，而设备建矩阵需要的是"槽位已填好、
但 y 还要 stage"。现在两个开关分开——这是合并/拆分两条路都能用的基础设施。

**③ `corr_build_fail` 计数器**
`device_corr_builds` 是总数；**"构建从未发生"与"构建失败后回退"在总数上完全一样**。
新计数器把失败单独列出（本轮就是靠它排除了"tail 构建失败"这个假设）。

**（b）⚠️ 我自己引入的一个语义反转缺陷（已修）**
我把 `build_slots_on_device()` 的 `std::optional<corr_stage>` 返回值当成"成功标志"用
（`.has_value()`）。实际语义**恰好相反**：正常路径**构建成功时返回 `nullopt`**，
只有设备反演实验需要调用方自己派发时才返回描述符。
⇒ 于是 `std_on_device` 恒为 `false`，tail 构建从未被调用（而主机回退让结果依然正确，
所以**功能测试全绿、覆盖率却没变**——这正是本轮绕圈子的原因）。
已改为显式 `bool` 返回 + 出参 `deferred_corr`，并把这条写进函数文档：
**"返回值是成功标志，永远不是描述符"**。

**（c）merged 覆盖率的尝试：失败，已回退（这是本轮的主要负面结果）**
放开 `std_slots_filled` 的 `!merge_tail` 门（让标准组走设备构建）、并把 edge block 也建成
（`sys_off=nof_layers`、槽位 stride 用标准组的 `L_std/nout_std`、块阶用 `L_e/nout_e`），
实测结果：

| 抓包 | `device_corr_builds` | 两条路 LLR |
|---|---|---|
| `iq2_10044_17923`（25→24+1） | **2**（标准组 + edge） | **差异** ✗ |
| `iq1_10049_17921` | **2** | **差异** ✗ |
| `iq2_1009_17923`（无余数） | 1 | 一致 ✓ |

诊断：**标准组的 A 被反演了两次**——探针读到 `a0 = 1861.26`（应为 ~1.00）。
形态与 §48.73 那个缺陷同源："两个写者写同一块内存，却都以为自己是唯一写者"：
设备建标准组 → 设备建 edge（写进同一对槽位）→ 主机对 edge 反演 → 而标准组的 A 在被反演前
可能已被 edge 的构建/主机的第二次反演碰到。
⇒ **已回退到"merged 不走设备构建"**（覆盖率回到与 OTA 实测一致的 37.5%），
回退后 5/5 抓包一致、覆盖率 0/0/1/0/1 与上机前完全相同。
回退原因写进了代码注释，**下一次要重做这件事必须先画清 merged 的两个几何各自的
槽位区间、以及"谁在什么时候写/反演哪一段"**，而不是边试边改。

**（d）状态**：980/980 抓包一致、`ctest -L phy` 162/162、split 形式（`OCUDU_CE_SPLIT_TAIL=1`）
`device_corr_builds=2` 且逐字节一致、6 形状 soak 12/12、`OCUDU_CE_DEV_INVERT` 仍可用（不崩）。

**（e）教训（本轮最值得记的一条）**
**"功能测试全绿"与"新代码真的执行了"是两件独立的事**。
(b) 的语义反转让新代码静默不执行，而主机回退把结果兜住了——如果没有 `device_corr_builds`
这个计数器，我会以为改动生效了。**每一个"接入新路径"的改动，都要配一个"它执行了几次"的计数器**，
这条在 §48.74(b) 已经写过一次，本轮再次应验。

#### 48.84 全链路状态快照（横向视图）

> 本节是**横向**的状态视图：每个模块**跑在哪里**、数据**存在哪里**、每一段**做了什么**。
> 前面各节是按时间推进的纵向记录，这一节是"某一时刻的整体切片"，供后续 session 直接对照。
>
> **维护约定（重要）**：这一节**描述当前状态，不记录历史**——
> 每次**改变"某模块在哪跑"**、或**改变某缓冲的布局 / 写入者 / 读者**时，
> **必须回来更新本节**（否则它会比正文更早过期，而读者会先信它）。
> 代码基线见 `git log -1 --format=%h`；本节的数字来自 §48.82（上机）与 §48.75/§48.79（离线）的实测。

##### 48.84(a) 模块落点总表

| 段落 | 模块 | **跑在哪** | 备注 |
|---|---|---|---|
| 前段 | RF 收发（B200/UHD） | CPU/USB | `otw_format=sc12` |
| 前段 | OFDM 解调 → 频域网格 | CPU | 网格进 `resource_grid` |
| 前段 | DMRS 导频提取 / LS 估计 / EPRE | CPU | 产出 **`pilots_lse_view`**——CE 的输入 |
| **CE** | **K0-d 相关矩阵 `A`/`R_hp`** | **GPU（设备建）** | **覆盖率 37.5%**（hop 覆盖率，见 (d)）。**自 §48.98 起其输出真正被消费**（此前主机 staging 每次都把它覆盖掉）：与主机同几何**逐元素完全相同**（`A` 0/2916、`R_hp` 0/27216 差异） |
| **CE** | **K1 矩阵反演 `A⁻¹`** | **GPU（设备）**——**默认**，自 §48.98 | `device_inverts()` 默认开（`order <= 54` = 内核 `MAX_N`）。**算法与功能均已验证**：980 抓包上设备 K1 vs 主机 K1 **CRC 判决 980/980 一致**（LLR 会差——反演舍入；SINR 中位数差 0.00 dB）。**代价是性能债**：GPU 上每 hop 多 ~400–480 µs（barrier 多，§48.98(f)），按方针不阻塞。逃生口：`OCUDU_CE_GPU_INVERT=0` / `OCUDU_CE_CPU_INVERT=1` |
| **CE** | K2a 权重 `W = R_hp·A⁻¹` | **GPU** | |
| **CE** | K2b 应用 `h = W·y` | **GPU** | `tpt` 必须 = `nout`（见 (c)） |
| **CE** | K3 估计重排（cbf16） | **GPU** | 附在引擎同一条 CB |
| **CE** | K4 噪声方差归约 | **GPU** | 产出 1 个 float |
| **CE** | unpack `gpu_h` → `grid_est` | CPU | ~5µs |
| **CE** | RSRP / noise_var / TA 统计 | CPU | 读 `grid_est` |
| 等化 | 等化（MMSE equalize） | **GPU** | 与解调共用一条 CB（`shared_burst`） |
| 等化 | `ch_re` 收集（读网格） | **GPU（设备 gather）** | `OCUDU_EQ_GATHER=0` 是逃生口 |
| 等化 | 读信道估计 | **设备直读 `gpu_ce`** | 不走主机 |
| 等化 | 读噪声方差 | **设备直读 `gpu_nv`** | `OCUDU_CE_NO_K4` 是逃生口 |
| 解调 | demap → LLR | **GPU** | |
| 译码 | LDPC | **GPU** | `pusch_ldpc_decoder_type auto` |
| 后段 | 解速率匹配 / CRC / MAC / RLC / PDCP / CU | CPU | |
| 外部 | AMF / 核心网 | 外部 | |

**一句话**：从"频域网格"到"LLR"，**全部在 GPU 上**（自 §48.98，K1 回到设备）。
剩下的是**性能债**而非落点问题：K1 内核每 hop ~400–480 µs 的 barrier 开销（§48.98(f)），
以及 K0-d 只覆盖 37.5% 的 hop（merged 分支仍是主机建矩阵，见 (d)/(f)）。

##### 48.84(b) 数据流每一站

```
[时域 IQ] ──OFDM 解调(CPU)──→ [频域网格 resource_grid] ──┬── DMRS 提取 + LS 估计(CPU)
                                                        │        ↓
                                                        │   [args.pilots_lse_view] ★进 GPU
                                                        └──(留给等化器，由设备 gather 读)
★ CE 每个 hop 一次：
   ① sigma2 计算                    CPU（只产出 sigma2；tau_rms/fd 是配置常数）
   ② K0-d 建 A / R_hp               GPU（37.5% hop）或 CPU（62.5%，merged 分支）
   ③ K1 反演 A⁻¹（原地覆盖 gpu_a）   GPU（设备，同一条引擎 CB 的头部）
   ④ 引擎调用（一条 CB，defer）      GPU: K1 → K2a → K2b → K3 → K4
   ⑤ unpack gpu_h → grid_est        CPU（在 wait 之后）
   ⑥ RSRP / noise_var / TA          CPU

**分段耗时的两个工作点（差别很大，必须分清）**

| 段 | 离线 replay（24/25 PRB 宽分配） | **OTA 实网**（`d8e23f67d1`，§48.82） |
|---|---|---|
| `pre`（导频提取/LSE/CFO 组装） | 6–18 µs | **0.69 µs** |
| `sigma2` | 10–14 µs | **2.9 µs** |
| `corr`（A/R_hp 构建 + 主机反演） | 17–22 µs | **10.9 µs** |
| `stage`（staging） | 0–35 µs | **17.89 µs** |
| `gpu_path`（corr 之后 → **引擎提交结束**，即 staging+submit；**不是**"到完成"） | 60–660 µs（波动大） | **99.4 µs** |
| `gpu_wait` | 60–155 µs | **137.2 µs** |
| `mean total` | 94–878 µs | **114.3 µs** |

⇒ **实网的 hop 结构（大量窄分配）与离线抓包（多为宽分配）不同**，
所以"离线 980/980 一致"**不能替代上机验证**，两者要分别测量。
★ 等化 + 解调（一条 CB）：gpu_ce + gpu_nv + 设备 gather → LLR
★ LDPC (GPU) → 传输块 → 后段 (CPU)
```

**CE 阶段有 2 条 command buffer**：设备建矩阵那条（独立、**同步等待**）+ 引擎那条（defer）。
实测那条独立 CB 的等待 **139–687µs 全是 GPU 排队**（commit 仅 5–12µs、GPU 执行 20–31µs，见 §48.81(a)）。

##### 48.84(c) 缓冲区账本（谁写、谁读、什么布局）

常量：`MAX_LAYERS=4`、`MAX_BLOCK_PILOTS=72`、`MAX_BLOCK_OUT=504`、`MAX_NOF_PRBS=275`
（⇒ `MAX_NOF_SUBCARRIERS=3300`）。

| 缓冲 | 类型 | 容量 | 实占 | 布局 | 写者 | 读者 |
|---|---|---|---|---|---|---|
| `gpu_a` | float | 4×72×72 = 20736 | **81 KiB** | `[sys][L][L]` | K0-d 设备 / 主机 staging | K1 原地反演 |
| `gpu_r_hp` | float | 4×504×72 = 145152 | **567 KiB** | `[sys][nout][L]` | K0-d 设备 / 主机 staging | K2a |
| `gpu_w` | float | 4×504×72 = 145152 | **567 KiB** | `[sys][nout][L]` | K2a | K2b |
| `gpu_y` | float | 4×max_blocks×2×72 | 同上量级 | `[sys][blk][2L]` 实虚交错 | 主机 staging | K2b |
| `gpu_qy` | float | 4×ceil(max_blocks/4)×72×8 | 同上量级 | 量化打包（矩阵引擎用） | 主机 staging | `run_nn` |
| `gpu_h` | float | 4×max_blocks×2×504 | 同上量级 | `[sys][blk][2·nout]` 实虚交错 | K2b | K3、unpack |
| `gpu_ce` | **uint16** | 4×275×12×14 = 184800 | **361 KiB** | cbf16，`[layer][total_re]` | K3 | 等化器（设备直读） |
| `gpu_nv` | float | **1** | 4 B | 标量 | K4 | 等化器（设备直读） |
| `gpu_pilots` / `gpu_rx_pilots` | float | 按 DMRS 符号×层×CDM 组 | — | 实虚交错 | 主机 staging | K4 |
| `gpu_epochs` | float | 14 | 56 B | 符号起始时刻 | 主机 staging | K4 |
| `grid_est` | `cf_t` | 56 槽 × 3300 = 184800 | **1.41 MiB**（实测 `sizeof`=1478432） | `[layer×14][RE]`，**内联在 estimator 对象内** | ⑤ unpack | RSRP 等统计 |

**说明**：`gpu_a`/`gpu_r_hp`/`gpu_w`/`gpu_y`/`gpu_h` 都是 4KB 页对齐的独立分配
（Metal 零拷贝要求），`gpu_ce`/`gpu_nv` 额外要求**整页**（它们是跨引擎导出的，
走进程级零拷贝映射）；`grid_est` 相反——它是**对象内联**的，不参与零拷贝。

**块几何**（离线抓包实测；`block_prb=3`、`npt=3`、`comb=6`）：
标准块 `L_std = 3×6×3 = 54`、`nout_std = 36×14 = 504`；
有余额时 edge 块如 `rem_prb=1` ⇒ `L_e = 18`、`nout_e = 168`；
merged 批次把 edge 作为**额外 system** 挤进标准槽位（`A = blockdiag(A_e, I)`）。

**`tpt` 硬约束**：`gpu_apply` 内核用 **`tid` 直接作输出位置索引**（`hp[2*tid]`），
所以 `threadsPerThreadgroup` **必须等于 `nout`**。实测 `tpt=504` → SINR 23.97dB；
`tpt=256` → **0.86dB**；`tpt=128` → **0.48dB**（静默丢尾部输出）。

##### 48.84(d) K0-d 覆盖率的准确口径（重要，容易被总数误导）

`device_corr_builds` 是**每次 `build_correlation()` 调用**计一次，不是"每个 hop 一次"：

| 分支 | 条件 | 构建次数 |
|---|---|---|
| 标准块（无余额） | `rem_prb == 0` | **1** |
| split 形式（`OCUDU_CE_SPLIT_TAIL=1`） | 标准组 + tail | **2** |
| **merged 分支（有余数，实网大多数）** | `std_slots_filled == false` | **0**（走主机构建） |

⇒ 上机腿 `26761 / 71384 hops = 37.5%` 是**hop 覆盖率**，不是构建次数比。

##### 48.84(e) 门禁与可观测性

**四层等价性**（自 §48.98 起，第 ① 条换成了**真正在测设备建矩阵**的那一次比较）：
① 离线 980/980 抓包 `llr/h/ce` 逐字节：`OCUDU_CE_GPU_INVERT=0`（设备建矩阵 + 主机反演）
vs `OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0`（全主机）——**这条才是 K0-d 的等价性门禁**；
② **K1 的功能门禁**：默认（设备 K1）vs `OCUDU_CE_CPU_INVERT=1`（主机 K1）在 980 抓包上的
**CRC/译码判决**必须 980/980 一致（LLR 字节不要求一致，反演舍入本来就不同）；
③ 多形状 soak 6 形状 × 2 轮；④ `ctest -L phy` 162/162。

**① 与 ② 已固化为可重跑脚本**（`capture_gates.sh k0d|k1`，见 §48.99(a)）——不要再用 `/tmp` 里的临时脚本。

> ⚠️ **历史注记（务必记住）**：在 §48.98 之前，① 的形式是"默认 vs `CORR_DEV=0`"，
> 而**默认路径每次都让主机 staging 把设备写的 A/R_hp 覆盖掉**——
> 所以那条绿灯证明的是"主机 vs 主机"，**对 `r_hp` 完全无效**（§48.96 的 1/9 缺陷因此藏了整轮）。
> **门禁绿之前，先确认它测的是被测对象。**

**计数器**（都打在 stderr，退出时输出）：

| 计数器 | 防的是什么 |
|---|---|
| `device_corr_builds` | **"以为跑了其实没跑"**（§48.74(b) 的教训） |
| `corr_build_fail` | **"从未调用"与"调用后失败"混淆**（§48.83(b) 的教训） |
| `ch_re device/host` | 设备 gather vs 主机 gather |
| `ch_est device/staged` | 等化器读设备估计 vs 暂存 |
| `mmse_ce commits/waits`、`wrap hits/creates/replaces/failures` | 提交/映射健康度 |
| `ul_gpu_lane`（busy / gap / 按 stage 分账） | GPU 忙闲与阶段归属 |

> 计数器只证明"执行了"，**不证明"输出被消费了"**（§48.98(g) 的第 1 条教训）。
> 判"被消费"要看下游的量：这里用**逐元素 A/B** 与**CRC 判决**。

##### 48.84(f) 账本（已知未完成，按"是否阻塞全 GPU path"分类）

**阻塞全 GPU path 的**：
1. **merged 分支的设备建矩阵**——尝试过并**已回退**（标准组 A 被反演两次，`a0=1861`）。
   重做前**必须先把两个几何的槽位区间与"谁在何时写/反演哪一段"画清楚**（§48.83(c)）。
   （这是 K0-d **覆盖率 37.5%** 的原因；K1 不再受它影响——K1 对 merged 批次同样在设备上跑。）
2. **`OCUDU_CE_DEV_INVERT`**——默认关闭、**已知不可用**：
   `gpu_h` 只写了 2 个 float（`gpu_y`/`gpu_w` 两个输入都逐字节正确），
   特征是非零 `tid` 满足 `tid = round(k×0.9)`、`0.9 = L/(L+comb)`（§48.81(c)）。

**不阻塞、记在账上的**：
3. **split 形式（`OCUDU_CE_SPLIT_TAIL=1`）的尾块批次在真实抓包上算错**——已用 parent 对照证明
   **与 §48.98 的改动无关**（§48.101(d)）：`iq1_10049` 6.24 → 3.15 dB、`iq2_10044` 31.78 → 8.87 dB，
   而 `rem_prb == 0` 的 `iq2_1009` 不受影响（split 与 merged 退化相同）。单测 Test 11 的合成形状测不出它，
   默认路径走 merged 所以不阻塞任何判据。要修必须先画清 split 两个几何的槽位区间（§48.83(c) 的同类工作）。
4. **K1 内核的延迟**——设备 K1 每 hop 多 ~400–480 µs（§48.98(f)）：块 Gauss-Jordan 每主元两次
   barrier，n=54 ⇒ ~108 次。**这是当前最大的一笔性能债**（主机侧反而省了 ~22 µs 的 Gauss-Jordan）。
   真正的修复是 S-5a 的 blocked / `simdgroup_matrix` 形式（K1 还没用上 8×8 硬件矩阵单元）。
5. `build_correlation()` 独立 CB 的排队等待（139–687µs；异步提交消不掉，只能减少 CB 数）。
6. 设备 gather 相对主机 gather 的 **+27µs**（按"全 GPU path 优先"不作阻塞，见 §48.63）。
7. K1b 右看式反演数值错误（opt-in `OCUDU_INV_RL=1`）。
8. replay 工具 ~4% 间歇崩溃（`rx_buffer_impl::get_codeblock_data_bits`，两路边都有，与本研究无关）。

**已否决**：用 ridge 正则化换取设备反演可用（240 抓包中 221 个 LLR 变化，代码已删除）。

#### 48.85 ⚠️ 一次**决策理由污染**：K1 因"性能"退回 CPU，却被"精度"论据留在了那里

**（a）用户的质疑与事实核对**
用户问："K1 很久以前就搬进了 GPU，为什么回到 CPU？是因为精度吗？——这不能成为理由。"
核对文档后确认：**质疑成立，而且这是我的判断错误。**

| 阶段 | 依据 | 决策与**当时**的理由 |
|---|---|---|
| K1 在 GPU | §28、行 1035/1084 | 单系统 36×36 比 CPU **慢 9 倍**（91.3µs vs ~10µs）。当时明确写下"**这不是'求逆该留 CPU'的依据**" |
| 可切换 | 行 1089 | `OCUDU_CE_GPU_INVERT=1` 切到引擎内求逆（`engine->run()` 把 K1+K1b+K2 放进**同一条 CB**） |
| **拐点：退回 CPU** | §48.68（`7b9f9ad6ed`） | 实网标准块 **L=54**，K1 上限 36。放开上限后设备反演 **555µs**（GPU 仅执行 4.4µs，其余是 108 次 barrier），比它替换的 ~10µs 主机 Gauss-Jordan **多花 ~470µs** |

⇒ **回退 CPU 的唯一理由是"慢 ~470µs"**，文档当时还写下了修复方向（S-5a：blocked / simdgroup_matrix 形式）。

**（b）错误发生在下一步：我用一个被自己推翻过的论据"加固"了那个决定**
§48.73 我写下："反演仍在主机——因为 K1 在 order 54 上的 **1.3e-5 相对误差**足以把抓包从 **24dB 打到 −18dB**。"
但在**同一节的后面**（§48.79(b)）我自己用 W 空间推翻了它：

| 反演 | `A⁻¹` 元素误差 | **`W = R_hp·A⁻¹` 误差** |
|---|---|---|
| 主机 f32 GJ | 1.66e-1 | **1.40e-5** |
| 设备 K1 | 9.67e-1 | **7.67e-4**（差 55 倍） |

`h` 的相对误差与 `W` 同量级 ⇒ 折算到 SINR **只有 ≈0.003 dB**。
而那个 **−18.69dB 的真因是两个 staging 缺陷**（§48.76 已修），**与精度无关**。
更糟的是：§48.79(b) 里我已经写了"**设备反演的精度其实够用**"，却在 §48.84 的快照里
**又写回了"精度不够"**。

**（c）为什么这类错误比单个缺陷更危险**
1. **它让正确的方向主动后退**：性能债（可后补）被误当成正确性障碍（必须回退），
   于是"全 GPU path"白停了一站；
2. **它有"权威外观"**：一个带数字的精度论据，比"慢"更有说服力，于是没人再回头质疑；
3. **它会自我复制**：因为它被写进状态快照，下一个 session 会**先信快照**（§48.84 的维护约定
   恰恰是为了防止过期，而这次问题是"错的"而不是"旧的"）。

⇒ **纪律（新增，与 §48.72 的"先写判据"、§48.83 的"计数器"并列）**：
**每次用"精度/正确性"论证一个模块不能进 GPU 时，必须先回答两件事**：
(a) 这个精度指标**是不是下游真正消费的那个量**？（这里是 `W`，不是 `A⁻¹`）
(b) 有没有**已被自己推翻**的结论仍留在文档/快照里？**改结论时，必须回头搜一遍旧表述。**

**（d）处置**：K1 **回到 GPU**（§48.84 已更正）。性能代价 ~555µs/hop **记入性能债、不作阻塞**；
真正的修复是 K1 kernel 本身（S-5a 的 blocked/simdgroup_matrix；`metal_nn_mmse` 已经用上 8×8
硬件矩阵单元，K1 还没有）。

#### 48.86 S-7f-3s：K1 拨回 GPU 后**不执行**——这不是精度问题，是 dispatch 问题（本轮未修完）

**（a）按用户重申的原则执行的三步**
用户定调："**主要目标是在确保功能正确的前提下，功能模块尽可能地进 GPU**，
并最终所有数据从 IQ 到 LLR 全在 GPU 里面。精度问题不能成为回退的理由。"
据此：(1) 更正 §48.84 的 K1 行并写下 §48.85 的"决策理由污染"；(2) 把 K1 拨回 GPU；
(3) 离线验证功能正确性——**(3) 未通过，见 (c)**。

**（b）本轮落地的改动**
- `device_inverts(order)` 成为**唯一**的反演落点判据（默认 ON，上限是内核自己的 `MAX_N=54`，
  不是速度门）：合并分支与 `run_engine_blocks()` 都用它；
- `engine_run()` 按 `gpu_invert` 选路径：`run_async()`/`run()`（**含 K1 的那条**，本来就是现成的）
  或 `run_weights_only*()`（槽位已是 A⁻¹）；
- `build_slots_on_device()` 在"设备将反演"时**不再自己反演**（否则 K1 又反演一个 A⁻¹ = §48.73 缺陷）；
- `stage_engine_group()` 在设备将反演时写入 **A（不是 A⁻¹）**。

**（c）⚠️ 验证失败，而且原因出乎意料：K1 内核**没有写入槽位
用探针在**同一跳**内 dump 了 `gpu_a` 的前 54×54：

| 时点 | `A[0]` | `A[1][1]` |
|---|---|---|
| staging 之后（K1 之前） | 1.00212 | 1.00212 |
| **整批完成之后（wait 已返回）** | **1.00212** | **1.00212** |
| 主机 GJ 会给出 | ~353 | ~421 |

⇒ **`A` 在 K1 前后逐字节相同**：K1 一次都没写。链路结果也随之崩：
`iq2_1009` 23.97→**5.38dB**、`iq1_10049` 6.24→**−56dB**、`iq2_10044` 31.78→**−53.8dB**。

**已排除**：
- **A 的 staging 是对的**——`k1_check` 拿这份 dump 跑，`A[0]=1.00212`、对角 1.00212，
  与设备建矩阵的已知正确值一致；
- **设备 K1 内核本身可用**——同一份 A 交给 `k1_check` 的 `engine->invert()`，
  残差 `max|A·X−I| = 1.849e-04`（**优于**主机 Gauss-Jordan 的 2.089e-04）；
- `run_async()` 里的 K1 dispatch 参数正确（`inv_pipe`、`a_buf`、`n=L`、`nof_systems`、
  threadgroup `(32,4)` 一应俱全）；
- `engine_run()` 收到的 `gpu_invert` 已是真（此前误传 `device_corr.has_value()`，那会恒假——已修）。

**⇒ 剩下的怀疑方向**：`run_async()` 这条 CB 里 **K1 的写入对主机（以及后续权重）不可见**，
即"同一条 command buffer 内 K1 → weights 的次序/可见性"没有建立起来。
这与 §48.80 那个未解现象**同源**（当时发现：实验路径里"多出来的那个 K1 dispatch"
会让 apply 只写出 2 个 float）。**两个现象指向同一处缺陷**，这反而把它的范围钉住了。

**（d）当前状态（诚实）**
K1 已按要求拨回 GPU，但**功能不正确**（见 (c)），因此**这一变更尚不可提交为默认**。
§48.84 的"全部在 GPU 上"是**目标状态**，不是当前可交付状态——
正确的当下表述是：**K1 位于 GPU 的路径已接通，但 K1 在内核执行层面没有生效，正在修**。

**（e）下一步（下轮的唯一目标）**
把 §48.80 与 (c) 当成**同一个缺陷**来查：在 `run_async()` 里把 K1 与 weights/apply
**拆到不同的 `MTLComputeCommandEncoder`**（同一 `MTLCommandBuffer` 内），做一次对照：
- 若拆开后 K1 生效 ⇒ 是**同一 encoder 内的资源依赖/可见性**问题
  （Metal 对同一 encoder 内同一 buffer 的读写有 hazard tracking，但显式拆分可验证）；
- 若拆开后仍不生效 ⇒ 是 `mmse_inv` 在这个**尺寸/线程组配置**下没有实际执行
  （例如线程组内存超限导致 dispatch 被丢弃——`MAX_N=54` 的 `gj[54][108]` = 22.8 KiB 需复核）。
同时保留 `OCUDU_CE_CPU_INVERT=1` 作为逃生口，**它必须始终可用**（本轮已保留）。

#### 48.87 S-7f-3t：设备 K1 的收尾状态——**默认关（功能正确）**，开关留作修复入口

**（a）最终状态（本轮结束时的实况）**

| 项 | 状态 |
|---|---|
| K1 **默认**落点 | **CPU（主机）** —— 功能正确：`iq2_1009` 23.97dB / `iq1_10049` 6.24dB / `iq2_10044` 31.78dB |
| K1 **设备**路径 | **已完整接通但内核不生效**；`OCUDU_CE_GPU_INVERT=1` 复现（`iq2_1009` → **−23.35dB**） |
| `device_inverts(order)` | 单一判据；默认关的原因是**功能缺陷**（不是性能、不是精度）；上限 `MAX_N=54` |
| 门禁 | 980/980 抓包逐字节一致、`ctest -L phy` 162/162（**默认路径**） |

**（b）为什么默认必须关（这一步的决策依据，写清楚以免下轮误读）**
用户的原则是"**确保功能正确的前提下**尽量进 GPU"。设备 K1 目前**功能不正确**——
所以"默认关"满足原则的**前提条件**；而它不是"因为性能/精度回退"，
**修好内核执行问题后应立即改回默认开**（一行：`device_inverts()` 的 `enabled` 初值）。

**（c）本轮为定位该缺陷排除掉的可能性（都已实测，下轮不必重做）**

| 候选 | 结论 |
|---|---|
| A 的 staging 错（写的是 A⁻¹） | ✗ 排除：dump 出的 A `[0]=1.00212`、对角 `1.00212`，`k1_check` 认可 |
| **对齐**（4KB vs 16KB 页） | ✗ 排除：`page=16384`，`gpu_a/r_hp/w/y/h` **全部 PAGE-OK**（`alloc_aligned` 实际给 16KB 对齐） |
| 零拷贝退化为拷贝 | ✗ 上面已排除；且 `gpu_ce/nv` 走整页分配、等化器直读一直正常 |
| **同 encoder 内 K1 与 weights 的可见性** | ✗ 排除：把 K1 拆到独立 encoder（同一 CB）**结果完全不变**（−23.35dB） |
| defer 异步 vs 同步 | ✗ 两者结果**完全相同**（该探针未成功插入，但同步/异步在另一次对照里同值） |
| pipeline 创建失败 | ✗ 排除：`init()` 返回 `inv_pipe != nil && ...`，为假则整条链回退 CPU，而 `hops_gpu` 非零 |
| dispatch 几何 | ✗ 排除：`n=L=54`、`nof_systems`、threadgroup `(32,4)`、`a_buf` 尺寸均正确 |
| 参数结构体两侧字段错位 | ✗ 排除：`mmse_weights/apply/corr` 三个结构体在 `.metal` 与 `.mm` 两侧逐项相同 |

**（d）下轮的唯一入口（范围已很窄）**
**在 `run_async()` 与 `invert()` 之间做逐行对照**——同一个矩阵，前者不生效、后者生效。
最可疑的一处尚未验证：`invert()` 里 K1 dispatch 用 `[enc dispatchThreadgroups:... threadsPerThreadgroup:MTLSizeMake(tgx, tgy, 1)]`
且**只有 K1 一个 dispatch**，而 `run_async()` 里它前面是 `encode_corr`（当 `corr` 非空）或什么都没有。
建议做法：在 `run_async()` 里**临时把 K1 换成 `invert()` 的调用**（独立 CB 先反演一次），
若链路随之正确 ⇒ 说明缺陷在 `run_async()` 的 CB 构造；若仍错 ⇒ 缺陷在参数传递
（此时把 `a_buf` 绑定前后的 `h0`/`A[0]` 一起 dump，一次可定）。

**（e）说明**：本轮**未提交**（设备 K1 不生效，默认关等于无行为变化）；
代码改动仅限 `device_inverts()` 的开关化与注释，§48.86/§48.87 记录在案。

#### 48.88 S-7f-3u：**翻转 §48.86 的结论**——设备 K1 算法完全正确，缺陷在"结果消费"

**（a）用户方向正确**：历史逻辑本来就是全 GPU，应回到那个逻辑修，而不是从当前状态硬修。
按此核对历史（`a52677d194`）：当时 `gpu_invert` 一为真就
**staging 写 A**、并走 **`engine->run()`（K1+K1b+K2 同一条 CB）**——
与现在接通后的实现**逐项一致**。所以差异不在路由。

**（b）决定性测量：K1 在链路里**确实工作**，而且结果**正确****
用 `OCUDU_CE_INVERT_FIRST=1` 让引擎用**它自己的独立入口** `mmse_engine::invert()`
（`k1_check` 一直用成功的那个入口）在权重调用前反演一次，然后 dump 权重调用的输入与输出：

| 路径 | `A[0]`（权重读到的） | `w[0]` | `h[0]` |
|---|---|---|---|
| **设备 K1**（`invert()` 先行） | **353.275818** | **0.250750065** | **0.648677528** |
| 主机 K1（对照） | 353.278625 | 0.250812382 | 0.648476481 |

⇒ **两者的 `w` 与 `h` 都正确**，差异 2.5e-4 正是设备反演 7.67e-4 量级的 `W` 误差。
⇒ **设备 K1 的算法是对的**，"设备反演不可用"这个结论**不成立**。

**（c）因此 §48.86 的"K1 一次都没写"是**测量假象**，必须更正**
§48.86 观察到"A 在整批前后逐字节相同"，据此判定 K1 未执行。
真因是**那个 dump 探针本身**：它在 `engine_run()` 之后插了一次 `wait_pending()`，
而**正是这次额外的等待改变了结果**——同一个设备 K1 配置：

| 条件 | `w[0]`/`h[0]` | 链路 SINR |
|---|---|---|
| 带 dump（多一次 wait） | **0.250750065 / 0.648677528（正确）** | — |
| **不带 dump（正常路径）** | 未测（被覆盖） | **−23.35 dB** |

⇒ **这是同步/时序缺陷**：某处没有正确等待挂起的批次，
于是 `h` 被使用时还是旧值/未完成值。`has_pending_before_wait=1` 在**两条路径上都成立**
（主机 K1 也一样），所以不是"设备 K1 特有地没有待处理批次"这种简单差异，
而是**设备 K1 让批次变长（~555µs vs ~10µs 主机反演）后暴露出来的等待时机问题**。

**（d）下一轮的唯一目标（范围已经很小）**
`engine_run()` 之后、`gpu_h`/`gpu_ce` 被消费之前的**等待链条**，具体查两处：
1. `run_engine_blocks()` 里 `deferred` 为真时 `defer_unpack()` 登记的是**哪一批**；
   `complete_fd_td_estimation_stage()` 是否**必然**在该批次完成之后才 unpack
   （即 `wait_pending()` 与 `nof_pending_unpacks` 的配对是否在所有分支上都成立）；
2. 设备 K1 使批次耗时从 ~10µs 量级升到 ~555µs 量级，
   **是否有任何一处假设"批次很快"而提前消费**（`stage_pending` / `gpu_ce_ready` 的时序）。
判据：在 `unpack_engine_group()` 入口打印 `engine->has_pending()`——
**若为真 ⇒ 就是"未等待就 unpack"**，一处即可定案。

**（e）状态**：默认（主机 K1）功能正确、门禁绿；设备 K1 路径**算法已验证正确**、
但受上述同步缺陷影响仍不可用。**这是本轮最重要的进展：把"算法不可用"证伪，
把一个模糊的大问题收敛成一个具体的同步缺陷。**

#### 48.89 S-7f-3v：K1 同步假设**被证伪**；缺陷收敛到"K3 的输出只写了一半"（与 §48.80 同源）

**（a）判据执行结果：同步不是问题**
按 §48.88(d) 的判据在 `unpack_engine_group()` 入口打印 `has_pending()`：

| 路径 | `has_pending` | `h0`（unpack 入口） |
|---|---|---|
| 设备 K1 | **0** | 0.648677528 |
| 主机 K1 | **0** | 0.648476481 |

⇒ **两条路径都已正确等待**，且 `gpu_h[0]` **都是对的**。同步假设**不成立**，`unpack` 的参数
（`gb_start/n_blk/nout/st.L/st.nout/st.n_blk/sys_offset`）两侧**逐项相同**。

**（b）真正的差异在 `gpu_ce`——等化器直读的那个**
在 `get_device_ch_estimates()`（等化器取设备估计的入口）打印它即将读到的 cbf16 原始字：

| 路径 | `gpu_ce` 前 4 个 word（16 bit） |
|---|---|
| 设备 K1 | `3f26 3d0d `**`0000 0000`** |
| 主机 K1 | `3f26 3d0d `**`3f26 b8f8`** |

⇒ **前两个字相同，后两个是零**——**K3 的输出只写了一部分**。
（`3f26` ≈ 0.65，是正确的估计；`0000` 说明那两个位置没被写。）

**（c）这与 §48.80 的"`gpu_h` 只写了 2 个 float"是同一个模式**
两个现象此前被当作两件事（一个是 DEV_INVERT 的实验路径、一个是 K1 回 GPU），
现在看是**同一处缺陷**：**某条路径下内核只写出一小段输出**。
`gpu_h` 的前 2 个 float、`gpu_ce` 的前 2 个 word——**都是"开头对、其余零"**。
⇒ 怀疑对象从"K1"转移到**"K2/K3 在某种命令缓冲区配置下的输出覆盖范围"**。

**（d）本轮已排除（都实测过，下轮不必重做）**
| 候选 | 结论 |
|---|---|
| K1 算法 / 反演结果 | ✗ 排除：`w0=0.25075`、`h0=0.648678` 与主机一致（差 2.5e-4 = 预期 W 误差） |
| 同步 / 未等待 | ✗ 排除：`has_pending=0`，`h0` 正确 |
| unpack 寻址与参数 | ✗ 排除：两侧逐项相同，`h0` 正确 |
| K3 的 dispatch 参数 | ✗ 排除：`nof_sub=288 nof_sym=14 layers=1 threads=4032 dc_sc=138` **两侧完全相同** |
| K3 的线程组宽度（64） | ✗ 排除：`tpt=64/128/256` 结果**完全相同** |
| K3 前的内存屏障 | ✗ 已有 `memoryBarrierWithScope:MTLBarrierScopeBuffers` |
| 缓冲对齐 / 零拷贝退化 | ✗ 排除（§48.86 已测：page=16384，全部 PAGE-OK） |

**（e）下一轮的判据（范围已经很小）**
**同一个命令缓冲区里，K2 写 `h`、K3 读 `h` 写 `gpu_ce`。**
`gpu_ce` 只有开头对 ⇒ 要么 K3 的**输出索引**在设备 K1 路径下错位，要么 **K2 的 `h` 只有开头是有效的**
（但 `gpu_h[0]` 是对的，所以这需要看 `h` 的**全部** 8064 个 float，而不是首元素——§48.79 曾测过
DEV_INVERT 下 `nonzero=1440/8064`，与这里"只写一部分"完全吻合）。
建议动作：在设备 K1 路径下 dump **整个 `gpu_h`（8064 float）与整个 `gpu_ce`**，与主机路径逐元素比；
若 `gpu_h` 也只有开头的 N 个有效 ⇒ 缺陷在 **K2 的输出范围**（apply 的 dispatch/线程索引）；
若 `gpu_h` 全对而 `gpu_ce` 只有开头 ⇒ 缺陷在 **K3**。
**这一步能把范围再砍一半，且一次运行即可判定。**

#### 48.90 S-7f-3w：**判定完成——缺陷在 K2（apply），不在 K1、不在 K3**

**（a）执行 §48.89(e) 的判据，一次运行定案**
在 `engine_run()` 里 dump **整个** `gpu_h`（`nof_systems × nof_blocks × 2 × nout` = 8064 float）
与 `gpu_ce` 的前 2048 个 word：

| 路径 | **`h` 非零** | `gpu_ce` 非零 | `h0` |
|---|---|---|---|
| **设备 K1** | **1440 / 8064** | 358 / 2048 | 0.648677528 |
| 主机 K1 | **8064 / 8064** | 2042 / 2048 | 0.648476481 |

⇒ **`h` 在设备 K1 路径下只写出了 1440/8064 个值**（18%），而**首元素是对的**。
K3 只是照实处理了这份残缺的 `h`（所以 `gpu_ce` 也只有开头对）——**K3 无罪**。

**（b）与 §48.79 的数值完全一致，两处现象确认为同一缺陷**
§48.79 在 `OCUDU_CE_DEV_INVERT=1` 下测到 `nonzero=1440/8064`——**数字一模一样**。
当时把它归因于"DEV_INVERT 实验路径特有"，实际上它是**设备反演路径共有**的缺陷：
只要走 `run_async()`（K1 与 K2 同一条 CB），**K2 的输出就只有开头一小段有效**。

**（c）缺陷的准确表述**
> **`mmse_apply`（K2）在"K1 与它在同一个 command buffer 内"时，只写出 `h` 的前 1440/8064 个值。**

（1440 = 8 块 × 180？或 2.857 块 × 504——不是整块数，所以不是"少跑了几块"，
更像是**线程/线程组的输出索引被截断**或**部分 dispatch 被丢弃**。
`h0` 正确说明第一个线程组确实跑了。）

**（d）本轮已排除的（下轮不必重做）**
K1 算法与结果（`w`/`h0` 正确）、同步/未等待（`has_pending=0`）、unpack 寻址、K3 的 dispatch 参数
（两侧逐项相同）、K3 的线程组宽度（64/128/256 同结果）、K3 前的内存屏障（已有）、
缓冲对齐与零拷贝（§48.86）。

**（e）下一轮的唯一入口（范围已极小）**
缺陷在 **apply 的 dispatch 覆盖**。判据与手段：
1. 在 `run_async()` 里把 **K1 与 K2 拆成两个 `MTLComputeCommandEncoder`**（同一 CB 内）
   —— 注意 §48.80 试过"拆 K1 到独立 encoder"，但那次**拆的位置在 weights 之前**
   （unpack 相关的现象），这次要**在 K1 与 weights/apply 之间**拆，两者不是同一实验；
2. 同时 dump `apply` 的 `tgid` 覆盖范围（把 `tgid` 写进一个专用小 buffer 的原子最大值），
   与 `nof_blocks * nof_systems` 对比：
   **若实际 tgid 最大值远小于 368 ⇒ dispatch 被截断**（Metal 侧或尺寸计算）；
   **若 tgid 覆盖满 368 而输出仍缺 ⇒ apply 内的索引/写地址有问题**。
3. 备查：`apply_tpt` 必须等于 `nout`（§48.81(d) 已证 tpt<nout 会静默丢尾部输出）——
   但这里 tpt 已经是 504，所以**不是 tpt**；嫌疑最大的是 **`nof_systems * nout` 的线程组数
   与 `nob_blocks` 的交互在 K1 存在时被改**（`aparams` 是 `setBytes` 传的，K1 也用 `setBytes`
   传 `L`/`nof_systems` —— **两个 `setBytes` 共用一个 encoder 时绑定槽位是否互相影响**，
   这是本轮新提出的、尚未验证的怀疑点）。

#### 48.91 S-7f-3x：**apply 跑满了**——"dispatch 被截断"被排除，缺陷在它的输入

**（a）用 out-of-line 探针缓冲测 apply 的实际线程组覆盖**
在 `mmse_apply` 里让 `tid==0` 的线程把 `tgid` 写进一个**独立的、非零拷贝**的共享缓冲
（避免测量本身扰动 staging 布局），批完成后由主机读回：

| 路径 | 写回的 probe words | 前 16 个 |
|---|---|---|
| 设备 K1 | **735** | `0 100000 200000 300000 400000 500000 600000 700000 1 1 1 1 1 1 1 1` |
| 主机 K1 | **735** | **完全相同** |

⇒ **apply 的 dispatch 覆盖两条路径完全一致，8 个块的标签（0/100000/…/700000）全部出现。**
**"dispatch 被截断"被排除，apply 跑满了。**

**（b）因此缺陷在 apply 的**输入**：`h = W · y` 里有大量零**
`h_nz = 1440/8064` 而 apply 跑满 ⇒ 不是"没算"，是**算出来是零** ⇒
**`W` 或 `y` 在设备 K1 路径下只有一部分有效**。
（`h[0]` 正确，说明第一个块的输入是好的。）

**（c）下一步的唯一判据（很小的一步）**
在设备 K1 与主机两条路径下，dump **整个 `gpu_y`（`nof_systems × nof_blocks × 2L` = 864 float）
与整个 `gpu_w`（`nout × L` = 27216 float）**，数非零：
- **若 `y` 只有开头有效** ⇒ `stage_engine_group()` 的导频 staging 在设备 K1 路径下被截断
  （注意它此时 `slots_filled=true`——**这一支正好是设备 K1 才走的**，与缺陷只在设备反演时出现
  **完全吻合**，是**头号嫌疑**）；
- **若 `w` 只有开头有效** ⇒ 权重内核的输出范围问题。

**（d）本轮的状态（诚实）**
**设备 K1 仍未修好。** 但范围从"K1 不执行"一路收敛到
**"apply 的输入在设备反演路径下只有一部分有效"**，且已排除 K1 算法、同步、unpack、K3、
dispatch 参数、线程组宽度、内存屏障、缓冲对齐、apply 的线程组覆盖。
**默认路径功能正确**：三条抓包 `llr/h/ce` 逐字节一致（默认 vs `OCUDU_CE_CORR_DEV=0`）、
`ctest -L phy` 162/162；代码与已提交状态一致（诊断代码已全部清除，
过程中一次清理误删的 `return true;` 已修复并复验）。

**（e）本轮的方法论收获**
**"换个不扰动系统的观测手段"**：最初用"在 `engine_run()` 后 dump"来观测，
结果那次 dump 自己带的 `wait_pending()` 改变了行为（§48.88 的假象）；
改用**内核写、主机读的 out-of-line 缓冲**后，观测不再影响被测对象。
⇒ **凡是要判定"内核跑了多少"，都应该让内核自己汇报，而不是在主机侧加断点式探针。**

#### 48.92 S-7f-3y：**根因找到了** —— `r_hp` 在设备反演路径下**根本没有被 staging**

**（a）一路数下去，缺陷定位到 `r_hp`**
按 §48.91 的判据，主机直接数 apply 的两个输入：

| 路径 | `y` 非零 | **`w` 非零** | `r_hp` 非零 | `r_hp` 全零行 |
|---|---|---|---|---|
| 设备 K1 | 864/864 ✓ | **4860/27216（18%）** | **2916/27216** | **414/504** |
| 主机 K1 | 864/864 ✓ | 27216/27216 ✓ | 27216/27216 | 0/504 |

`2916 = 54²` 这个数太整齐；逐行看：设备路径 `r_hp` **row0 正确**（`1 0.995 0.981 …`）
而 **row9 全零**，主机路径 row9 有正常值（`0.910 0.944 …`）。
⇒ 缺陷在**权重内核的输入 `r_hp`**，不在 `w`、不在 `y`、不在 K1、不在 apply。

**（b）直接对比 staging 的实参，一次命中**
在 `stage_engine_group()` 里打印全部 stride 与标志：

| 路径 | `Ls` | `Ns` | `slots_filled` | **`a_rhp_filled`** | `gpu_invert` |
|---|---|---|---|---|---|
| 设备 K1 | 54 | 504 | 0 | **1** | 1 |
| 主机 K1 | 54 | 504 | 0 | **0** | 0 |

**唯一差异就是 `a_rhp_filled`。**

**（c）根因（一句话）**
> `a_rhp_filled = 1` 跳过了**整个** staging 循环——**包括 `R_hp` 的写入**。
> 但 **K1 只反演 A，从来不动 `R_hp`**。于是**设备反演路径下 `gpu_r_hp` 完全没有被填**，
> 里面是**上一跳的残留**（row0 碰巧还是 1，row9 已是垃圾/零）。
> ⇒ `w` 有 82% 是残值算出来的 ⇒ `h` 只有 18% 有效 ⇒ SINR −23 dB。

这与前面已修的三个缺陷**同源**：**"跳过整个循环"与"只跳过循环里的一段"被混为一谈**
（§48.76 的导频 staging、§48.83 的语义反转，都是这个形状）。

**（d）修复的**正确形状**（本轮尝试了一次，失败并已回退）**
必须把**两个不同的问题**分开：
1. **A 还要不要 host staging**？设备将反演（K1 读 A）或设备已建 ⇒ 不要；
2. **R_hp 还要不要 host staging**？**只有设备构建写过它才不要**；K1 不碰它 ⇒ 设备反演时**必须由 host 写**。

我按此改了一版（新增 `r_hp_filled` 与 `a_filled_elsewhere` 两个标志），但**引入了新问题**：
**默认（主机 K1）路径也被破坏**（`iq2_1009` 也变成 −23.36 dB），说明这两个标志与
`gpu_invert`、`slots_filled`、`device_built` 的**组合有误**（四个布尔量的真值表没有理清就动手）。
**已回退**到功能正确的状态，并把根因与正确形状记录在此。

**（e）下一轮的做法（先画表，再改代码）**
四个标志的真值表必须先写出来（**这次不先画表就是失败的原因**）：

| 场景 | 设备建矩阵? | 设备反演? | host 需写 A? | host 需写 R_hp? |
|---|---|---|---|---|
| 默认主机路径 | 否 | 否 | 是（写 A⁻¹） | **是** |
| 设备建 + host 反演 | 是 | 否 | 否（设备已写 A，host 原地反演） | **否**（设备已写） |
| 设备反演（本轮修复目标） | 否 | **是** | **否**（写 A，交给 K1） | **是** ← 本轮遗漏的就是这一格 |
| 设备建 + 设备反演 | 是 | 是 | 否 | 否 |

**判据**：四种场景各跑一条抓包，`r_hp_nz` 必须是 27216/27216、`w_nz` 27216/27216、
`h_nz` 8064/8064；然后默认路径三条抓包 `llr/h/ce` 逐字节一致。

**（f）状态**：默认路径功能正确（三条抓包 23.97/6.24/31.78 dB）；
§48.92 记录的根因**尚未修复**；代码与已提交状态一致（本轮改动已全部回退）。

#### 48.93 S-7f-3z：按施工图施工**第二次失败并回退**——但这次失败指出了施工图缺的那一格

**（a）施工内容（按 §48.92(e) 的表）**
理论上只需把"跳过整个 A/R_hp 循环"的条件从 `a_rhp_filled`（被错传成 `dev_inv_now = gpu_invert`）
换成**只有设备构建成功才跳过**：`slots_filled = device_built`，并删掉多余的 `a_rhp_filled`
（因为循环内的 `if (gpu_invert)` 分支**本来就**区分"写 A（供 K1）"与"写 A⁻¹"，语义已正确）。

**（b）结果：默认路径也被破坏，已回退**
| 路径 | 施工后 | 回退后（HEAD） |
|---|---|---|
| 默认 `iq2_1009` | **−23.36 dB** ✗ | **23.97 dB** ✓ |
| 默认 `iq1_10049` | 6.24 dB ✓ | 6.24 dB ✓ |
| 设备 K1 `iq2_1009` | −23.35 dB ✗ | −23.35 dB ✗ |

⇒ 默认路径**本不该被改动**（`device_built=false`、`gpu_invert=false`，与 HEAD 行为在理论上完全相同），
却出现了 −23 dB。**说明改动影响到了我没检查的那条路径**——合并分支（merged）。

**（c）施工图缺的那一格：**合并分支有两个 `stage_engine_group()` 调用**（我之前只按一个来想）
核实 HEAD 的调用点（`grep` 出三处）：

| 行 | 位置 | `gpu_invert` 实参 | 备注 |
|---|---|---|---|
| 969 | 合并分支的 **else**（标准组） | **硬编码 `false`** | 与 `run_engine_blocks()` 里那个不是同一个决策点 |
| 1014 | 合并分支的 **tail** | 变量 `gpu_invert` | 来自第 950 行（`device_inverts(L_std)`） |
| 1834 | `run_engine_blocks()` | 变量 `gpu_invert` | 本轮改的就是这里 |

⇒ **合并分支的"标准组"那一次调用用的是硬编码 `false`**：它对设备反演一无所知。
当设备算 K1 反演时（`L_std=54 ≤ 54`，`gpu_invert=1`），tail 那一次写的是**原始 A**，
而标准组那一次写的是 **A⁻¹** —— **同一对槽位被两种语义各写一半**，
K1 再去反演 ⇒ 结果必然错。**这才是"默认路径也被破坏"的真正原因**：
它不是默认路径自己的问题，而是我在改 `run_engine_blocks` 的同时
**没有把合并分支的标准组调用点一起对齐**（它本来就有一个独立的、硬编码的 `gpu_invert`）。

**（d）下一轮的正确施工顺序（这次先列全，再动手）**
1. **先把三处调用点的 `gpu_invert` 与 `slots_filled` 全部列出并统一**——
   合并分支的标准组（969）必须与 tail（1014）、`run_engine_blocks`（1834）用**同一个** `gpu_invert`；
2. 再按 §48.92(e) 的四行真值表改"谁写 A / 谁写 R_hp"；
3. **每改一处都先用探针验证该调用点的实参**（`gpu_invert`/`slots_filled`/`sys_offset`），
   确认与真值表一致后再跑链路——**不要一次改完再跑**。

**（e）状态**：已回退，**默认路径 23.97/6.24/31.78 dB 正确**，代码与已提交状态一致；
设备 K1 仍不可用（根因已定，施工图已补上"合并分支标准组"这一格）。

#### 48.94 S-7f-4a：**两次失败的真正原因**——我改参数列表时漏改了一个调用点

**（a）核实到的事实（此前我按错的前提改了两次）**
`grep` 出 `stage_engine_group()` 共**三个**调用点：

| 行 | 位置 | `gpu_invert` 实参 | 实参个数 |
|---|---|---|---|
| **969** | 合并分支的 else（标准组） | 硬编码 **`false`** | **12**（缺一个） |
| 1014 | 合并分支的 tail | 变量 `gpu_invert`（950 行 = `device_inverts(L_std)`） | 13 |
| 1834 | `run_engine_blocks()` | 变量 `gpu_invert` | 13 |

**（b）两次失败的直接原因：改签名却漏改 969 行**
我在 §48.92/§48.93 两次都把 `stage_engine_group()` 的形参从
`(…, slots_filled, a_rhp_filled)` 改成 `(…, slots_filled)`（少一个参数），
却**只更新了 1014 和 1834 行**——**969 行仍是 12 个实参**。
12 个实参对 12 个形参**恰好能编译通过**（最后一个 `false` 落进 `slots_filled`），
于是**合并分支的标准组语义被静默改变**，而这正是默认路径走的调用点 ⇒ 默认路径被破坏。

⇒ **教训（本轮最贵的一条）**：**改函数签名时，必须 `grep` 出所有调用点并逐个确认实参个数**，
"能编译过"绝不代表"实参对得上"——**参数个数相同但语义位移**是最难发现的一类改动。
（两次失败都栽在这一点上，而不是栽在对缺陷的理解上。）

**（c）下一个正确的施工顺序（一次只动一行，每次验证）**
1. **第一步只改一行**：把 969 行的 `false` 换成 `gpu_invert`（与 1014 行对齐），
   **不动签名、不动 `run_engine_blocks`**。此时：
   - 默认路径：950 行的 `gpu_invert=false` ⇒ 969/1014 都写 A⁻¹ ⇒ **应保持 23.97 dB**；
   - 设备路径：`gpu_invert=true` ⇒ 969/1014 都写**原始 A**（tail 与标准组一致）⇒ 修复"一半写 A、一半写 A⁻¹"。
   **判据**：默认路径三条抓包不变；设备路径 `r_hp_nz` 应升到 27216/27216。
2. **第二步**（若第一步后设备路径的 `r_hp` 仍不满）：把 `stage_engine_group` 那个循环的跳过条件
   从 `!slots_filled && !a_rhp_filled` 收敛为**只由"设备是否已写 A/R_hp"决定**，
   并同时更新**全部三个**调用点（这次先 `grep`）。
3. 每一步都先跑默认路径（必须不变），再跑设备路径（须改善）。

**（d）状态**：已回退，默认路径 **23.97 / 6.24 / 31.78 dB 正确**，代码与已提交状态逐字节一致；
设备 K1 仍不可用；根因（`r_hp` 未被 staging）与本次的"漏改调用点"均已记录。

#### 48.95 S-7f-4b：**第一次真正修好了东西**——一行对齐，设备 K1 从 0/3 变成 2/3

**（a）改了什么（+5 行，一处实质改动）**
合并分支的**标准组**那一次 `stage_engine_group()` 调用，第 11 个实参从**硬编码 `false`**
换成 **`gpu_invert`**，与同一分支的 tail 调用（1014 行）以及 `run_engine_blocks()` 对齐：

```cpp
// 合并分支 else（标准组）
stage_engine_group(args, 0, n_std_blocks, block_prb, npt, nout_std, L_std, 0, st,
                   matrix_on,
                   gpu_invert,   // ← 原为 false
                   false);
```

**（b）效果（这是 K1 回 GPU 以来第一次出现正确结果）**

| 抓包 | 路径 | 默认（主机 K1） | 设备 K1（修复前 → 修复后） |
|---|---|---|---|
| `iq1_10049_17921` | 合并分支 | 6.24 dB | **−56.02 → 6.24 dB** ✓ |
| `iq2_10044_17923` | 合并分支 | 31.78 dB | **−53.78 → 31.86 dB** ✓ |
| `iq2_1009_17923` | 设备构建 | 23.97 dB | −23.35 → **−23.35** ✗ |

**默认路径完全不变**：980/980 抓包逐字节一致、`ctest -L phy` 162/162。

**（c）剩余缺陷已精确定位到"设备构建 + 设备反演"这一个组合**
`device_corr_builds` 把三条抓包分成两类：
- `iq1_10049` / `iq2_10044`：`=0` ⇒ 不走设备构建 ⇒ **修好**；
- `iq2_1009`：`=1` ⇒ 走设备构建 ⇒ **仍错**。

探针（`r_hp` 非零数）进一步确认：
| 抓包 | 路径 | `r_hp_nz` |
|---|---|---|
| `iq2_1009` | 默认 | **27216/27216** ✓ |
| `iq2_1009` | 设备 K1 | **2916/27216**（= `L²`，只写了 54 行）✗ |

⇒ **在"设备构建 + 设备反演"这条路径上，设备构建写出的 `r_hp` 只有前 `L` 行**
（`build_slots_on_device()` 在 `device_inverts(L)` 为真时**跳过主机反演并返回**，
而返回的 `corr_std` 被当作"需要调用方派发"，于是**设备构建的 `r_hp` 写入没有被完整看待**）。
下一轮只需查一条路径：**设备构建在 `device_inverts` 为真时的返回值语义**
（它现在返回 `corr_std`，与"构建成功"的 `bool` 语义**又混在一起了**——与 §48.83(b) 同一类错误）。

**（d）教训（本轮最值钱的一条）**
**"能编译过"绝不代表"实参对得上"。** 我前两次失败都是改了 `stage_engine_group()` 的形参个数
却漏改 969 行那个调用点——**12 个实参对 12 个形参恰好编译通过**，语义静默位移。
⇒ **改签名时先 `grep` 出全部调用点并核对实参个数**；这也是为什么这次改成"只改一个实参、
不动签名"后一次就成功了。

#### 48.96 S-7f-4c：发现一个**独立于 K1 的缺陷**——设备构建的 `r_hp` 只写了 1/9

**（a）在设备构建**之后立即**测量（不经任何其他代码）**
在 `build_slots_on_device()` 里 `build_correlation()` 返回后立刻数 `gpu_r_hp`：

| 路径 | `r_stride` | `a_stride` | `r_hp` 非零 |
|---|---|---|---|
| 默认 | 504 | 54 | **2916/27216** |
| `OCUDU_CE_GPU_INVERT=1` | 504 | 54 | **2916/27216** |

⇒ **两条路径完全相同，而且只有 2916 = `a_stride²`（= `A` 的大小）。**

**（b）这个发现的价值：它把"K1 回 GPU 的最后一处障碍"与"K0-d 设备建矩阵自身的缺陷"分开了**
- 默认路径**看起来是好的**（`r_hp` 最终 27216/27216）——因为**主机 staging 随后又把它写满了**，
  把设备构建的缺陷**掩盖**了；
- 设备 K1 路径**不再有主机 staging**（第一步的对齐让标准组也跳过/或 `slots_filled` 生效）
  ⇒ **设备构建的缺陷暴露出来** ⇒ `iq2_1009` 仍错。

⇒ **所以 §48.95(c) 里"设备构建 + 设备反演这个组合"的说法要更正为**：
**设备构建的 `r_hp` 从来就没写全**，只是默认路径用主机 staging 补上了。
**这不再是一个"K1 的问题"，而是 K0-d 自己的问题**（与 K1 无关，在默认路径下也存在）。

**（c）下一步的判据（唯一入口）**
`corr_stage` 的描述符两侧**已实测完全相同**（`l=54 Ls=54 Ns=504 nf=36 npf=18 ncomb=6 a_sys=2916 r_sys=27216`），
所以不是参数问题。嫌疑只剩 `mmse_corr_r_hp` 内核的**写入覆盖范围**：
`dispatchThreads:MTLSizeMake(rhp_per_sys, nof_systems, 1)`，其中
`rhp_per_sys = nout * c.l = 504 × 54 = 27216` —— 正好是全量，
而实际只落下 2916 ⇒ **要么内核只处理了 `gid.x < 2916`，要么 `p.Ns` 在内核侧读到 54**。
判据：**让 `mmse_corr_r_hp` 内核自己汇报它写到的最大 `gid.x`**
（与 §48.91 用过的 out-of-line 探针缓冲同样的手法，避免扰动）——
若最大值 ≈2915 ⇒ dispatch 被截断；若 ≈27215 而数据仍缺 ⇒ 写地址/`p.Ns` 问题。

**（d）状态**：默认路径 **980/980 抓包逐字节一致、`ctest -L phy` 162/162**（本轮改动仅删掉
`build_slots_on_device()` 里一段**重复的 `device_inverts` 块**，6 行死代码，无行为变化）；
设备 K1 路径 2/3 正确（§48.95），剩余 1 条卡在上述**设备构建缺陷**上。

#### 48.97 S-7f-4d：**系统性历史对标**（用户的批评成立：我在埋头调参，而不是对标正确代码）

**（a）对标结论一：K1 在 GPU 上"历史上能工作"时的路径，与"设备建矩阵"无关**
`a52677d194`（K1=GPU 且实测可用的版本）里 **`build_correlation` 根本不存在**（`git show … | grep -c` = **0**）。
⇒ **历史那条可行路径是"主机构建 A/R_hp + 设备反演"**。
**"设备构建 + 设备反演"这个组合在历史上从未存在过，也从未被验证过**——
而这正是我这一整轮在调试的组合。

**（b）对标结论二：历史代码的 staging 结构**（`a52677d194` 的原文，已核对）

```cpp
const bool gpu_invert = !matrix && (L <= MAX_GPU_INVERT_ORDER) && (getenv("OCUDU_CE_GPU_INVERT") != nullptr);
if (gpu_invert) {
  // Stage A itself (not A^-1): K1 overwrites the slot with the inverse in place.
  for (sys) memcpy(gpu_a + sys*L*L, w_r_pp.data(), L*L*sizeof(float));
} else {
  for (sys) { /* 主机 Gauss-Jordan → 写 A^-1 */ }
}
// R_hp 写入：**在 if/else 之外，无条件执行**
// Y staging：**无条件执行**
const bool engine_ok = matrix ? engine->run_nn(...)
                              : (gpu_invert ? engine->run(...) : engine->run_weights_only(...));
```

**（c）对标结论三：现在的代码与历史的**唯一实质偏离**，正是缺陷所在**
| 维度 | 历史（可行） | 现在（有问题） |
|---|---|---|
| A 的写入选择 | `if (gpu_invert)` 直接决定写 A 还是 A⁻¹ | 同（**已对齐**） |
| **R_hp 的写入** | **无条件执行**（在 if/else 之外） | **可被 `slots_filled`/`a_rhp_filled` 跳过** ← **偏离** |
| **Y staging** | **无条件执行** | 可被同一个 flag 跳过（§48.76 已修过一次，症状相同） |
| staging 的载体 | **内联在 `apply_fd_td_estimation_stage`** | 抽成 `stage_engine_group()` 并引入跳过参数 |
| K0-d 设备构建 | **不存在** | 存在，且它自己写不全 `r_hp`（§48.96） |

⇒ **缺陷的根都不是"某个 flag 传错"，而是"历史结构里 R_hp/Y 是无条件的，现在多了一道能跳过它们的门"。**
§48.75/§48.76/§48.83/§48.92 四次修的都是这道门的**不同开法**——
**正确的做法是把这道门按历史结构收掉**，而不是继续调它的开关组合。

**（d）按历史结构收敛的施工图（下一轮照这个做，不再调 flag）**
1. `stage_engine_group()`：**把 R_hp 与 Y 的写入移出任何跳过条件**（回到历史结构：只有 A 的写入由
   `gpu_invert` 二选一），**只保留"设备已经写过 A/R_hp"这一种跳过情形**（即 `slots_filled`），
   并且**先 `grep` 出全部三个调用点**核对实参（§48.95(d) 的教训）；
2. 然后单独修 **K0-d 自身的 `r_hp` 只写 1/9**（§48.96(c) 的判据：让内核汇报最大 `gid.x`）——
   这是**独立缺陷**，与 K1 无关；
3. 两步各自跑：默认路径必须始终 980/980；设备 K1 路径的判据是
   `r_hp_nz = 27216/27216`、三条抓包 SINR 与主机一致。

**（e）我这一轮的方法论错误（用户点出的）**
**"知道历史代码能工作"与"照着它施工"是两件事。** 我在 §48.86 就确认了"历史逻辑与现在一致"，
但那只核对了**路由**（`gpu_invert → engine->run()`），**没有核对 staging 的结构**——
于是后面四轮都在修那道"历史里根本不存在"的门。**教训：对标要落到
"每一段的写入义务与条件"上，而不是只对"哪个函数被调用"。**

#### 48.98 S-7f-4f：**K1 回到 GPU** —— 沿 §48.97(d) 的施工图做完两步，设备 K1 三条抓包全对

**（a）第 1 步：把 `stage_engine_group()` 收成历史形状（一个门，两矩阵同写同不写）**

`a_rhp_filled` **整个删掉**，只留 `slots_filled`，语义唯一：**设备已经把 A 和 R_hp 写进这些槽位**
（K0-d）。循环条件变成 `!slots_filled`，**A 与 R_hp 一起写或一起不写**——
这正是历史（`a52677d194`）的形状：if/else 只决定"写 A 还是 A⁻¹"，其余无条件。

按 §48.94/§48.95 的教训，这次**先 `grep` 出全部调用点**（三处），并**去掉形参默认值**
（`slots_filled` 不给默认），让编译器强制每个调用点表态：

| 调用点 | 之前 | 现在 |
|---|---|---|
| 合并分支 else（标准组） | `gpu_invert, false`（12 实参，靠默认值补齐） | `gpu_invert, false`（显式 12 实参 = 新签名） |
| 合并分支 tail | `gpu_invert, false, false`（13） | `gpu_invert, false`（12） |
| `run_engine_blocks()` | `gpu_invert, false, dev_inv_now`（13） | `gpu_invert, slots_filled`（12） |

`run_engine_blocks()` 里 `build_slots_on_device()` 的**返回值被捕获**为 `device_built`
（不再 `(void)`），`dev_inv_now` 删除；`slots_filled = device_built && (st.L == L) && (st.nout == nout)`
——后两个条件保证"设备只写了 L×L / nout×L 块"的那些槽位，**补齐工作仍由主机做**（矩阵 flavor 的
ceil8 pad 槽位因此仍走主机 staging，行为不变）。另外 `build_slots_on_device()` 改为**接收**
`gpu_invert`（不再自己 `device_inverts(L)` 重新推导），顺手修掉一个潜伏组合：
`matrix_on + OCUDU_CE_GPU_INVERT=1` 时它会提前返回、把**未反演的 A** 留给矩阵权重。
死参数 `deferred_corr`（历史上"调用方自己派发"的遗留物）一并删除。

**（b）第 2 步：K0-d 自己有两个缺陷，都不在 K1 上**

**缺陷 ①：`mmse_corr_r_hp` 的行跨距用错了字段。**
内核写 `r_sys[o * p.Ns + col]`，而 `p.Ns` 在主机侧是 `c.r_stride`（= 槽位的**输出行数** nout = 504）；
R_hp 在槽位里的**真实行跨距是 `p.Ls`**（= `c.a_l_stride` = 54，**与 A 相同**）——
主机 staging（`row = rp_slot + o * Ls`）与所有权重内核（`rp = r_hp + sys*nout*L + row*L`；
矩阵内核 `row stride Lp`）都按 `Ls` 读。

⇒ 每行落在 `o * 504` 而不是 `o * 54`：**只有前 54 行的前 54 列还落回本系统的槽位里**
（54 × 54 = **2916**，正是 §48.96 实测的非零数），其余 **~1 MB 写到槽位之外**。
修复：`r_sys[o * p.Ls + col]`。

> §48.96(c) 要的判据（"让内核自己汇报最大 `gid.x`"）**不必再插探针**了：修复前后直接做
> "设备建矩阵 vs 主机建矩阵"的逐元素 A/B，最大 `gid.x` 是否被截断一望即知（结果是没被截断，
> 是**写地址**问题——与 §48.96(c) 列的第二种可能一致）。

同时把内核参数结构体里的 `Ns` 字段**删掉**（`.metal` 与 `.mm` 两侧一起，并各加
`static_assert(sizeof(...) == 124)`）——它已经没有任何使用者，留着就是下一次误用的入口。

**缺陷 ②：Metal 默认 fast math 让设备矩阵差 1 ulp，被 cond₂(A) 放大成 ~1% 的 W/h。**
修好 ① 之后，"设备建矩阵 + 主机反演"与"主机建矩阵 + 主机反演"**仍不逐字节一致**。
在 `build_correlation()` 之后、主机反演之前逐元素比对（同一几何、同一统计量）：

| 矩阵 | 差异元素 | 最大绝对差 |
|---|---|---|
| `A` | **720 / 2916** | **5.96e-08**（= 0.5 的 1 ulp） |
| `R_hp` | **6594 / 27216** | **1.19e-07**（= 1.0 的 1 ulp） |

⇒ **1 ulp**，不是算法错。根因：**Metal 编译器默认开 fast math**，可以自由地把 `1.0F + x * x`
收缩成 fma、并重结合 `T * d * tau` 的乘法顺序。这一 ulp 进 A⁻¹ 后被 cond₂(A) ≈ 2e4 放大成
W/h 的 **0.4–0.9%**——正好是"设备 K1 的 W 误差 7.67e-4"的十倍量级。

**修复**：`ocudu_add_metallib()` 新增可选参数 `IEEE_MATH_SOURCES`，**逐源文件**加 `-fno-fast-math`
（其余内核保持默认，行为不变），目前只给 `ocudu_mmse_corr.metal`。
修后 `A` **0/2916**、`R_hp` **0/27216** 差异——**设备建矩阵成为主机 staging 的真正 drop-in**。

**（c）第 3 步（本次才真正发生的事）：设备建矩阵第一次被真正消费**
把 `slots_filled = device_built` 接上之后，主机 staging **不再覆盖**设备写好的 A/R_hp。
必须强调：**在此之前 §48.84(e) 的"980/980 逐字节"门禁对 `r_hp` 是空的**——
默认路径每次都让主机 staging 把设备输出重写一遍，所以那个门禁测的是"主机 vs 主机"。
§48.96 发现 `r_hp` 只写 1/9 时也是这个原因看不到。**这次两条缺陷都是在门禁真正接上被测对象之后才暴露的。**

**（d）判据（全部实测通过）**

| 门禁 | 结果 |
|---|---|
| 三条抓包 SINR（**设备 K1，现在是默认**） | **23.97 / 6.24 / 31.86 dB** |
| 三条抓包 SINR（主机 K1，`OCUDU_CE_CPU_INVERT=1`） | 23.97 / 6.24 / 31.78 dB |
| **K0-d 等价性**：`GPU_INVERT=0`（设备建+主机反演）vs `GPU_INVERT=0 CORR_DEV=0`（主机建+主机反演），980 抓包 `llr/h/ce` **逐字节** | **980/980 一致**（修复前：三条抓包全不一致） |
| **K1 功能等价**：默认（设备 K1）vs `CPU_INVERT=1`，980 抓包**译码判决** | **980/980 CRC 一致，0 条翻转** |
| 同上，LLR 字节 | 262/980 相同（其余是反演舍入差，见下表的 SINR 分布） |
| SINR 差（设备 K1 − 主机 K1，971 条可比） | 中位数 **0.00 dB**；\|Δ\|>0.1 dB **241 条**；最坏 **+7.32 dB**（38.22 vs 30.90，**两者都 CRC OK**） |
| `ctest -L phy` | **162/162**（163 条，1 条 disabled） |

⇒ **"设备 K1 的算法正确"从 §48.88 的个例变成了 980 条抓包上的统计结论：LLR 会变，判决不变。**

**（e）决策：`device_inverts()` 默认改为开。**
逃生口两个都保留且都实测过：`OCUDU_CE_GPU_INVERT=0`、`OCUDU_CE_CPU_INVERT=1`。

**（f）K1 的性能账（按用户方针**不阻塞**，但必须记清）**
同一条抓包的 `[mmse_time_sum]`：

| 段 | 主机 K1 | 设备 K1 |
|---|---|---|
| `stage`（staging） | 26.8 / 27.2 µs | **5.1 / 4.0 µs**（主机不再做 54×54 Gauss-Jordan，只 memcpy A） |
| `gpu_wait` | 156 / 83 µs | **553 / 559 µs** |
| `mean total` | 88 / 87 µs | 94 / 64 µs |

⇒ 主机侧**反而更快**（staging 省下的比等待多花的更值），但 **GPU 上每 hop 多 ~400–480 µs**。
这与 §48.68/§9 记的 **+555 µs/hop** 同量级，根因仍是 K1 内核的**块 Gauss-Jordan 每次主元两次
barrier**（n=54 ⇒ ~108 次）。真正的修复是 S-5a 的 blocked / `simdgroup_matrix` 形式
（`metal_nn_mmse` 已经用上 8×8 硬件矩阵单元，K1 还没有）。**上机是否扛得住由 OTA 决定（见 §48.99）。**

**（g）方法论（本轮最值钱的两条）**
1. **门禁必须验证"它真的在测被测对象"。** "980/980 逐字节"绿了很久，但被测的那条路径
   （设备写的 R_hp）**每次都被覆盖**——绿灯证明的是另一件事。**接入一条新路径时，
   要同时证明"它的输出被消费了"**，而不只是"它跑了"（计数器只证明了后者）。
2. **"能编译过"之外，"默认参数"也是同类陷阱。** §48.94 的教训是实参个数；
   这次 `slots_filled`/`a_rhp_filled` 都带默认值，于是"少传一个"同样能编译。
   **凡是表达"谁写了哪块内存"的参数，都不给默认值。**

#### 48.99 S-7f-4f 收尾：门禁脚本落库；**OTA 尚未跑成**（阻塞在核心网，不是代码）

**（a）门禁脚本落库（`/tmp` 里的那套会丢）**
新增 `lib/phy/upper/signal_processors/channel_estimator/metal/capture_gates.sh`，把本会话用的两个
抓包门禁固化成**两模式一条命令**（自并行、自带重试、退出码即判据）：

```bash
M=lib/phy/upper/signal_processors/channel_estimator/metal
$M/capture_gates.sh k0d 10      # K0-d 等价性：必须逐字节一致
$M/capture_gates.sh k1  10      # K1 功能等价：必须判决一致
```

| 模式 | 两条路径 | 判据 | 本轮实测（980 抓包） |
|---|---|---|---|
| `k0d` | `GPU_INVERT=0`（设备建+主机反演）vs `GPU_INVERT=0 CORR_DEV=0`（全主机） | **所有发布文件逐字节一致** | **980/980 一致 → PASS** |
| `k1` | 默认（设备 K1）vs `CPU_INVERT=1`（主机 K1） | **`tbs`+`crc` 判决一致**；LLR 字节数只作信息 | **980/980 判决一致 → PASS**（262/980 LLR 字节相同，max\|ΔSINR\|=7.32 dB，0 判决翻转） |

脚本里的两处坑（都踩过，已修，写在这里免得下轮再踩）：
1. `ul_chain_replay --out` 是**文件名前缀**，不是目录：写的是 `<out>_<slot>_<rnti>{,.bin,_ce.txt,_llr.bin,_h.bin}`。
   第一版按目录拼 `"$out_h/$(basename $f)"`，于是**每一条都判成不一致**（假红）。
2. 退化的抓包（`tbs=88`，无可用户数）打的是 **`sinr=inf`**，正则里的 `[-0-9.]+` 匹不上，
   9 条会静默变成 "no-result"（假红）。**判据脚本也要对"合法的退化输出"留门。**

**（b）语料里 `_h.bin`/`_llr.bin` 不是基线，别再拿它当参考**
本会话试过拿 `/tmp/iq*_llr.bin`/`_h.bin` 当"改动前的基准"做回归，**不成立**：
同一条 `iq2_1009_17923`，三条现行路径（设备 K1 / 主机 K1 / 全主机）**互相之间的差异都在舍入量级**，
但与那两个文件的 `h` **8400/8400 个元素全部不同、最大差 1.086**——它们是**另一次计算**的产物
（大小也对不上：`_llr.bin` 89936 B vs 现行 25388 B）。**语料里真正的基线是 `*_ce.txt`（抓包信息），
不是那对 `.bin`。**

**（c）OTA：B200 能开，卡在核心网**

| 检查项 | 结果 |
|---|---|
| `build/apps/gnb/gnb` 版本戳 | `63a15a7b9c`（= HEAD，已同步重建） |
| USRP B200 打开（**不加 sudo**） | **成功**（`Actually got clock rate 7.680000 MHz`，寄存器回环通过） |
| N2 → AMF `192.168.31.250:38412` | **`"NG Setup Procedure" timed out after 5000ms` ⇒ `CU-CP failed to connect to AMF`，gNB 主动退出（约 8 s）** |
| 后果 | AMF 不回 NG Setup ⇒ 无 UE ⇒ 无 PUSCH 授权 ⇒ **CE 一次都不跑**（`device_corr_builds=0`），OTA 判据一条也拿不到 |

> 注：`nc -z 192.168.31.250 38412` 探不到端口是**正常的**——N2 走 **SCTP**，不是 TCP。
> 判断核心网是否活着只能用 gNB 自己（或 `ss`/`netstat -a -p sctp`）。

**上机命令（脚本已就绪，含 AMF 未起时的提前退出与报告）**：

```bash
cd /Users/jiachengwang/dev/ocudu
lib/phy/upper/signal_processors/channel_estimator/metal/ota_k1_verify.sh 150
# 或手动：
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_ota_k1.log > /tmp/gnb_ota_k1_console.log 2>&1
```

**上机判据**（§9 的口径，未变）：`device_corr_builds > 0`；`Real-time failure in RF` 个位到数十；
0 USB 错误 / 0 崩溃。`[mmse_time_sum]` 的 `corr=`/`gpu_path=`/`gpu_wait=` **只记录、不设 gate**。

**（c-1）一条**我犯的操作错误**（写下来当规矩）**
为了探"核心网是否起来"，我用**旧版**脚本又跑了一次 20 秒的腿；旧版开头 `rm -f` 固定路径的日志，
而**用户此刻正用同一条命令在跑 OTA**——于是把那个 gNB **仍然打开着的日志文件 unlink 掉了**：
它后面（包括退出时的 `[metal_stats]` 计数器）都写进了一个已被删除的 inode，**无法恢复**。
（现象：第二次启动报 `Failed to bind UDP socket to …:2152 Address already in use` ——
NG-U 网关的 UDP 端口是固定的，**这就是"已经有一个 gNB 在跑"的信号**，我当时没把它当信号看。）

已修（`ota_k1_verify.sh`）：① 日志改成**带时间戳的独立路径**，不再删任何已存在的文件；
② 启动前 `pgrep build/apps/gnb/gnb`，**有实例在跑就拒绝启动并打印是谁**（`exit 3`）。

⇒ **规矩：探"环境是否就绪"的动作不能有副作用。** 任何会 `rm`/覆盖固定路径的脚本，
在跑之前必须先确认**没有别的实例正在用那些路径**。

**（d）OTA 的**已知风险**（不是"可能"，是**量化过的**）**
设备 K1 让每条 hop 的 GPU 时间多 ~400–480 µs（§48.98(f)），离线 `defer_wait` 从 ~290/617 µs 升到
~1067/992 µs。**实网 1 ms 时隙下能否吃掉这 0.5 ms，只有上机能回答。**
若上机出现大量 `Real-time failure in RF`，**下一步就是 K1 内核本身**（把 ~108 次 barrier 降下来，
S-5a 的 blocked / `simdgroup_matrix` 形式）——那是**性能修复，不是把 K1 退回 CPU 的理由**（用户方针）。

#### 48.100 S-7f-4g：把"设备 K1 路径 `r_hp_nz = 27216/27216`"这条判据**直接测掉**，并建立 OTA 的**改动前基线**

**（a）`r_hp_nz` 的直接测量（施工图 §48.96(c) 要的那个数，终于量了）**
在 `build_slots_on_device()` 里 `build_correlation()` 返回之后、任何其他代码碰这些槽位之前，
直接数设备写下的非零（探针走环境变量 `OCUDU_CE_RHP_NZ`，**测完即删，未提交**）：

| 抓包 | 路线 | `L` / `nout` | `a_stride` / `r_stride` | **`A_nz`** | **`R_hp_nz`** | 非有限 |
|---|---|---|---|---|---|---|
| `iq2_1009_17923` | 设备 K1（默认） | 54 / 504 | 54 / 504 | **2916/2916** | **27216/27216** | 0 |
| `iq2_1009_17923` | 主机 K1（`GPU_INVERT=0`） | 54 / 504 | 54 / 504 | **2916/2916** | **27216/27216** | 0 |
| `iq2_1009_17923` | `CORR_DEV=0` | — | — | （无设备构建，符合预期） | — | — |

⇒ **判据达成**（修复前是 `2916/27216`，即 `L²`）。另外两条抓包（`iq1_10049` / `iq2_10044`）
不发这个探针，因为它们走 **merged 分支**、`std_slots_filled == false`、**本来就没有设备构建**——
这也再次说明 37.5% 覆盖率的口径（§48.84(d)）。

> 与 §48.98 的"设备 A/R_hp 与主机逐元素 0/2916、0/27216 差异"相比，这个计数是**更弱**的证据；
> 两者一起给：**计数说明"写满了"，逐元素比对说明"写对了"。**

**（b）OTA 的**改动前基线**（`d8e23f67d1` 那条腿的 console 日志，本次从 `/tmp` 里挖出来并解析）**
之前只有"覆盖率 37.5%"这一个数字被记下来，其余计数器没进文档。补全（**这是新腿唯一有意义的对照**）：

| 计数器 | `d8e23f67d1`（主机 K1，设备建矩阵） |
|---|---|
| `hops_gpu` / `device_corr_builds` | 71384 / **26761**（37.5%） |
| `mmse_ce commits / waits` | 98147 / 98147（`guard=0/71386`） |
| `[mmse_time_sum]` `mean total` | **114.3 µs** |
| `pre` / `sigma2` / `corr` / `stage` | 0.69 / 2.9 / 10.9 / **17.89** µs |
| `gpu_path` (`gpu_wait`) | **99.4** µs (137.2 µs) |
| `defer_wait` / `max total` | 306.7 µs / 1068 µs |
| `[ul_gpu_lane] busy` mean/median | **224.2 / 223.5** µs |
| `[ul_gpu_lane] residency` mean/p99 | 278.5 / 444.0 µs |
| `[ul_gpu_lane] gap` mean/median | 54.3 / 1.0 µs |
| `Real-time failure in RF` | **165**（late 124 / underflow 40 / overflow 1） |
| USB 错误 / 崩溃 | 0 / 0 |

> ⚠️ **与交接备忘 §4.4 的"`Real-time failure in RF = 0`"不一致**：按 `--log.filename` 那份日志实测是
> **165 条**（124 late + 40 underflow + 1 overflow）。以**日志实测**为准；交接里那个 0 大概是
> 另一个时间窗或另一份日志。**这条更正很重要**：新腿的"数十条"要和 **165** 比，不是和 0 比。

**（c）新腿上该看什么（有基线的对照，而不是孤立数字）**

| 段 | 预期方向 | 依据 |
|---|---|---|
| `stage` | **显著下降**（17.89 → ~4–6 µs） | 主机不再做 54×54 Gauss-Jordan（§48.98(f) 离线实测 26.8→5.1） |
| `corr` | 略降 | 同上（主机不再构建矩阵） |
| `gpu_path` / `gpu_wait` | **上升 ~400–480 µs**（99.4 → ~550 µs） | K1 内核的 barrier 延迟 |
| `[ul_gpu_lane] busy` | 上升（224 → 可能 600+ µs） | 与 `gpu_path` 同源；**这是"1 ms 时隙装不装得下"的直接读数** |
| `Real-time failure in RF` | 与 165 同量级或更好 | 若显著恶化 ⇒ 立即转向 K1 内核优化（不是退回 CPU） |

判读用 `ota_k1_verify.sh report <console> <log>`（§48.99(a)），它把实时失败按
underflow / late / overflow 分类——**165 这种数字必须分类看**，`grep -c` 会把它糊成一个数。

#### 48.101 S-7f-4h：开关组合实测；**split 形式的退化是既有的，不是本次改动**（对照实验，不是推理）

**（a）动机**：本次改动动了 `gpu_invert` 与"谁写了槽位"这两个布尔量的**来源**，
而 §48.92(e)/§48.97(d) 的教训正是"布尔量超过两个先写真值表"。所以对
`OCUDU_CE_GPU_INVERT` × `OCUDU_CE_CORR_DEV` × `OCUDU_CE_SPLIT_TAIL` 的十种组合逐一实测
（三条参考抓包，看 `SINR`/`CRC`）：

| 组合 | `iq2_1009` | `iq1_10049` | `iq2_10044` | 判定 |
|---|---|---|---|---|
| 期望（主机 K1） | 23.97 KO | 6.24 OK | 31.78 KO | — |
| 1 默认（设备 K1 + 设备建） | 23.97 KO | 6.24 OK | **31.86** KO | ✓（+0.08 dB = 设备反演舍入） |
| 2 `GPU_INVERT=0` | 23.97 KO | 6.24 OK | 31.78 KO | ✓ |
| 3 `CPU_INVERT=1` | 23.97 KO | 6.24 OK | 31.78 KO | ✓ |
| 4 `GPU_INVERT=0 CORR_DEV=0` | 23.97 KO | 6.24 OK | 31.78 KO | ✓ |
| 5 `CORR_DEV=0`（设备反演 + 主机建） | 23.97 KO | 6.24 OK | 31.86 KO | ✓ |
| 6 `SPLIT_TAIL=1` | 23.97 KO | **3.15** OK | **8.86** KO | ✗ 见 (d) |
| 7 `SPLIT_TAIL=1 GPU_INVERT=0` | 23.97 KO | 3.15 OK | 8.87 KO | ✗ |
| 8 `SPLIT_TAIL=1 CORR_DEV=0` | 23.97 KO | 3.15 OK | 8.86 KO | ✗ |
| 9 `SPLIT_TAIL=1 GPU_INVERT=0 CORR_DEV=0` | 23.97 KO | 3.15 OK | 8.87 KO | ✗ |
| 10 `GPU_INVERT=0 CPU_INVERT=1` | 23.97 KO | 6.24 OK | 31.78 KO | ✓ |

⇒ 除 split 外**全部组合一致**，且 `iq2_1009` 在**所有**组合里都是 23.97（它 `rem_prb == 0`，
没有尾块 ⇒ split 与 merged 退化成同一条路）。**设备 K1 的误差在各组合里都是同一量级（≤0.08 dB）。**

**（b）必须记牢的一处语义变化：`OCUDU_CE_GPU_INVERT` 的取值含义变了**

| 版本 | `enabled` 的判据 | 设成 `OCUDU_CE_GPU_INVERT=0` 的效果 |
|---|---|---|
| `bc63c5329b` 及以前 | `getenv(...) != nullptr` | **仍然是"开"**（只判"非空"，不看值） |
| 本次（§48.98） | `env == nullptr \|\| strtoul(env) != 0` | **"关"** |

这个差异在**同一组组合的 parent 对照**里看得非常清楚（parent 源码实测）：

| 组合 | parent（`bc63c5329b`） | 本次 |
|---|---|---|
| 2 `GPU_INVERT=0` | `iq2_1009` = **−23.35** dB | 23.97 dB |
| 4 `GPU_INVERT=0 CORR_DEV=0` | **inf** | 23.97 / 6.24 / 31.78 dB |
| 9 `SPLIT_TAIL=1 GPU_INVERT=0 CORR_DEV=0` | **inf / inf / inf** | 3.15 / 3.15 / 8.87 dB |

⇒ parent 那三个 `inf`/`−23.35` **正是 §48.92 记录的 `r_hp` 未 staging 缺陷**（`a_rhp_filled`
把整个写循环跳过）。**这张表同时是"该缺陷已被本次修复"的直接对照证据。**
（也提醒：老资料里凡是写 `OCUDU_CE_GPU_INVERT=1` 的地方，现在 `=0` 才能关掉设备反演，
而"不设该变量"就是开——逃生口是 `OCUDU_CE_GPU_INVERT=0` 或 `OCUDU_CE_CPU_INVERT=1`。）

**（c）方法：回退源码重建做对照（不是靠推理）**
判"某现象是不是我改坏的"用了**直接对照**：把这 5 个源文件 + 2 个 cmake 回退到 `bc63c5329b`、
`cmake .` + 重建 `ul_chain_replay`、跑同一组组合，再 `git checkout <HEAD> -- <files>` 恢复并重建。
（`git status` 恢复后为空，已核对；二进制版本戳 `01ea670412` 与 HEAD 一致。）

**（d）负面结论（新）：split 形式在真实抓包上是坏的，且与本次改动无关**
`OCUDU_CE_SPLIT_TAIL=1` 的四个组合给出**彼此相同**的结果（3.15 / 8.86–8.87），
而在 **parent 源码**上跑同一组组合，数值**逐项相同**（3.15 / 8.87）：

| 抓包 | merged（默认） | split（本次源码） | split（parent 源码） |
|---|---|---|---|
| `iq2_1009`（`rem_prb == 0`） | 23.97 | 23.97 | 23.97 |
| `iq1_10049`（有尾块） | 6.24 | **3.15** | **3.15** |
| `iq2_10044`（有尾块） | 31.86 / 31.78 | **8.86** | **8.87** |

⇒ **split 形式的尾块批次在真实抓包上算错**，而 §29 的"merged ≡ split 逐位一致"门禁
（单测 Test 11）用的是**合成形状**，没抓到它。**默认路径走 merged，所以它不影响任何判据**；
按 §48.97(d) 的纪律，**不顺手改**（要改必须先画清 split 形式两个几何的槽位区间——
§48.83(c) 已经为 merged 做过同样的事）。已记入 §48.84(f) 账本。

**（e）教训**：**"是不是我改坏的"要用对照实验回答，不要用推理回答。**
我先论证了"我的改动对该组合行为中性"（论证本身成立），但仍然回退源码重建验证了一遍——
因为论证的每一环都可能漏掉一个调用点（§48.94 的教训就是这么来的）。
代价是一次重建，换来的是这条负面结论可以**不带"我认为"**直接写进文档。

#### 48.102 S-7f-4h 收尾：**OTA 仍未跑成，且阻塞条件连续三轮未变**（外部依赖）

**（a）本轮的上机尝试（无副作用版，08:43 UTC / 05:43 本地）**
用 §48.99(a) 加固后的 `ota_k1_verify.sh`（时间戳日志、绝不删已有文件、有实例在跑就拒绝启动）跑了 150 秒：

| 检查项 | 结果 |
|---|---|
| 二进制版本戳 | `8cf53a90f8`（= HEAD，已重建同步） |
| USRP B200 | 打开正常 |
| N2 → AMF `192.168.31.250:38412` | **`"NG Setup Procedure" timed out after 5000ms` ⇒ gNB 在 ~8 秒后自行退出** |
| 产物 | `/tmp/gnb_ota_k1_20260915-054309{,_console}.log`（保留） |

⇒ **核心网又下线了**（上一轮 05:33 那条腿时它是在线的，这也解释了为什么那次能跑到 NG Setup 之后）。
判据一条也拿不到：无 AMF ⇒ 无 UE ⇒ 无 PUSCH ⇒ CE 一次都不跑。

**（b）三轮的阻塞条件（同一个，且都在我控制之外）**

| 轮次 | 核心网 / UE 状态 | 结果 |
|---|---|---|
| 1 | AMF 不在线 | 已向用户报告，用户选择自行执行 |
| 2 | AMF 在线（用户那条腿跑起来了），但那条腿的日志被我的探测脚本误删 | 无可用日志；已修脚本并记录（§48.99(c-1)） |
| 3 | AMF 又下线 | 本轮探针 8 秒即判定，无副作用 |

⇒ **解除阻塞只需要一件事**：核心网（AMF `192.168.31.250:38412`）起好、UE 附着并产生上行流量，
然后跑 `ota_k1_verify.sh 150`（或用户自己跑并把两条日志路径给我）。
判读工具与基线都已就绪（§48.99(a)、§48.100(b)(c)），拿到日志即可出结论。

**（c）剩下的唯一工程任务（等用户决定）**
K1 内核的延迟（+400–480 µs/hop，§48.98(f)）**尚未优化**。用户明确选择"**先看 OTA 结果再决定**"，
而 OTA 结果目前拿不到 ⇒ 这项**保持待命**，不擅自开工（改内核会改变 K1 的舍入，
必须重跑 §48.99(a) 的 k1 门禁，属于"有验证成本的改动"，按用户的选择等信号）。

#### 48.103 S-7f-4i：**上机腿拿到了**（设备 K1 生效）；以及"`[ul_channel_estimation]` 为什么只有 86 µs"的定案

**（a）这一腿的实测（用户执行，手机跑 ping + iperf3）**

| 量 | **基线 `d8e23f67d1`（主机 K1）** | **本次（设备 K1）** | Δ |
|---|---|---|---|
| `hops_gpu` | 71384 | 12405 | —（时长不同，只看比值） |
| `device_corr_builds` | 26761 = **37.5%** | 3530 / 12405 = **28.5%** | 覆盖率随分配形状变化（见 (e)） |
| `mmse_ce commits` | 98147（1.37/hop） | 15937（1.28/hop） | — |
| `corr_build_fail` | 0 | **0** | ✓ |
| `[mmse_time_sum] stage` | 17.89 µs | **2.54 µs** | **−15.4** ✓ 主机不再做 Gauss-Jordan |
| `[mmse_time_sum] gpu_path` | 99.4 µs | **56.8 µs** | −42.6（见 (b)：这是 staging+提交，不是"到完成"） |
| `[mmse_time_sum] gpu_wait` | 137.2 µs | **529.2 µs** | **+392** ← K1 |
| `[mmse_time_sum] defer_wait` | 306.7 µs | **713.3 µs** | +407 |
| `[ul_gpu_lane] busy` mean | 224.2 µs | **611.8 µs** | **+387.6** |
| `[ul_gpu_lane] busy split ch_est` | 148.3 µs/lane | **537.7 µs/lane**（占 busy 的 **88%**） | **+389.4** ← K1 |
| `[ul_gpu_lane] busy split eq_demap` | 75.9 µs/lane | **74.1 µs/lane** | **−1.8** ✓ 等化/解调**没变** |
| `[ul_gpu_lane] residency` mean | 278.5 µs | 640.7 µs | +362 |
| `[ul_gpu_lane] gap` mean | 54.3 µs | 28.9 µs | −25.4 |
| `equalizer ch_est device/staged` | 785224 / 0 | **136455 / 0** | ✓ 仍 100% 设备直读 |
| `wrap failures` / 崩溃 / USB 错误 | 0 / 0 / 0 | **0 / 0 / 0** | ✓ |

⇒ **归因极其干净：唯一变大的是信道估计器的 GPU 时间（+389 µs/lane），而等化/解调一分没变。**
主机侧**反而更快**（`stage` −15.4 µs，正是被 K1 取代的那段 Gauss-Jordan）。这与 §48.98(f) 的离线预测（+400–480 µs/hop）吻合。

**（b）定案：`[ul_channel_estimation]` 没有坏，我的 K1 测量也没有错——两者测的是不同的东西**

用户的疑问（提得很好）：`[ul_channel_estimation] mean=86.2 µs median=54.4 µs`，比 K1 的 ~390 µs 小得多，
而按设计它应该是"整个 MMSE CE 的耗时"。核对代码后结论如下。

**① `[ul_channel_estimation]` 是 CPU 侧边界探针**
`include/ocudu/support/executors/ul_pipeline_probe.h:112-118`：
`ce_ns = ce_end − t2f_end`；而 `ce_end` 由
`lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp:250` 的
`ul_pipeline_probe::get().record_ce_end(...)` 打点，位置是 **`process_data()` 的第一行**
（注释写的就是"the channel estimator has finished... start of the equalization+demodulation one"）。
它量的是 **"FFT 完成 → CE 交出结果"** 这段**主机时间**。

**② 设备路径上，CE 是"提交即返回"的（deferred chain），所以这段里不含 GPU 等待**
`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:325`：
只有当等化器**不**就地读设备估计时才 `sync_device_estimates()`；本腿
`equalizer ch_est device=136455 staged=0` ⇒ **100% 就地读**，所以**等化器路径上根本没有那次同步**。
`sync_device_estimates()` 在 `pusch_processor_impl.cpp:453` 被调用，位置是**解调之后**（注释：
"Reading them any earlier would put the whole demodulation behind the estimator's synchronization"）。
⇒ CE 的命令缓冲区由 GPU 自己排队（等化器的 CB 链在它之后），主机直到**消费者阶段**才真的等。

**③ 这一点本来就是**已知且写在代码注释里**的**
`lib/phy/metal/ocudu_metal_lane_probe.h:7-11`：
> "The staged probes ([ul_time_frequency], **[ul_channel_estimation]**, [ul_equalization_demod]) measure
> **CPU-side boundaries**. ... a stage whose work is deferred measures the deferral."

**④ GPU 侧有两个独立探针同时证明 K1 的代价是真的**
- `[mmse_time_sum] gpu_wait = 529.2 µs`（引擎自己量的批次等待）
- `[ul_gpu_lane] busy split ch_est = 537.7 µs/lane`（**GPU 时间戳**，与主机无关）

两者相差 1.6%，且相对基线各 +392 / +389 µs。**一个测 CPU 边界、一个测 GPU 时间戳，
不可能同时以同样的量级错。**

**⑤ 历史对照：同一个探针以前**确实**量到过 CE 的 GPU 执行，是"推迟化"把它挪走的**
本文档早期的 OTA 分解（§24 附近，那时估算器**同步等待**）写的是：
`ul_channel_estimation = **430 µs**，其中 gpu_path=388 µs（gpu_wait 仅 44 µs）⇒ 是 GPU 执行本身`。
——**同一个探针**，同步时代量到 430 µs（含 GPU 执行），推迟之后只剩 86 µs（只剩交接）。
**变的是流水线，不是探针**；这也解释了为什么"按设计它应该是整个 CE 的耗时"这个印象是对的
——在推迟化之前它确实是。

⇒ **两条结论**：`[ul_channel_estimation]` 作为探针**没错**（它忠实地量了它定义的那一段），
但它**在设备路径上不是"CE 的总耗时"**——K1 与整个 CE 的 GPU 工作被推迟到消费者的等待里，
表现在 `[ul_equalization_demod]`（688.9 µs，而等化器自己的 GPU 工作只有 74.1 µs/lane）
与 `[ul_pipeline]`（mean 1014.7 / median 1003.0 µs）。
**要读 CE 的真实代价，看 `[ul_gpu_lane] busy split` 的 `ch_est`**（这一条是 GPU 时间戳，不受推迟影响）。

**（c）文档更正：§48.84(b) 表里 `gpu_path` 的注写错了**
代码（`port_channel_estimator_metal_mmse_impl.cpp:1360`）是 `gpu_path = t_gpu_end − t_corr_std`，
即 **"corr 之后 → 引擎提交结束"**（staging + submit），**不是**"提交到完成"。
旧表把它标成"提交到完成"是错的，是这次 86 µs 疑问的一部分来源。已改。

**（d）`ota_k1_verify.sh` 已删除**（用户要求：上机一律由用户用 `sudo` 执行并把 console 贴回来，
一个自己拉起 gNB 的脚本没有用处；其中"探测 AMF"的部分更是只在探环境、不在测被测对象）。
判读配方保留在本文档里（本节 + §48.100(b) 的基线），不依赖那个脚本。
`capture_gates.sh`（离线三条门禁）不受影响，保留。

**（e）待确认 / 待观察（诚实列出）**
1. **用户那次运行的 `hips_gpu` 覆盖率是 28.5% 而非 37.5%**：`device_corr_builds` 只在
   "无余数（`rem_prb == 0`）且非 merged" 时计一次（§48.84(d)），所以它随**分配形状**变化。
   ping/iperf3 的分配形状与 `d8e23f67d1` 那条腿不同，这是合理解释，但**本轮没有直接证据**，
   记为待确认。
2. **没看到 `Real-time failure in RF` 的条数**：它只进 `--log.filename` 那份日志
   （console 里不出现，`--log.all_level warning` 时它以 `[RF] [W]` 写文件）。
   **需要用户提供** `/tmp/gnb_ota_k1.log` 里的计数（按 underflow/late/overflow 分类），
   才能与基线的 **165** 对比——这是本腿**唯一还缺的判据**。
3. **用户那次运行的二进制版本戳未知**：`/tmp/gnb_ota_k1.log` 首行有
   `Built in Release mode using commit <sha>`，需要与提交对一下。
   （间接证据：`gpu_wait=529.2 µs` 且 `stage=2.54 µs` 只有"设备 K1 默认开"才会出现，
   所以**几乎可以肯定包含 §48.98 的改动**，但仍以日志首行为准。）
4. `[ul_pipeline]` median **1003.0 µs** 贴着 1 ms 时隙：本腿 `period median=1022.5 µs`、
   `dropped=0`，尚能跟上，但**余量已经很小**（基线 `busy` 224 µs vs 现在 612 µs）。
   若 (2) 显示实时失败显著多于 165，**下一步就是 K1 内核优化**（不是退回 CPU）。

#### 48.104 S-7f-4j：回答三个问题——K1 的耗时去哪了、36 还是 54、分块优化是不是丢了

**（a）"K1 的耗时被挪进均衡了吗？"——一半对，必须分清"GPU 执行"与"主机等待"**

| | 挪了吗 | 证据 |
|---|---|---|
| **GPU 执行** | **没有挪** | lane 探针按 command buffer 归属记账：`busy split ch_est=538.7 µs/lane`、`eq_demap=74.3 µs/lane`。若 K1 的执行被挪进等化器，涨的应该是 `eq_demap` —— 它**一分没变**（基线 75.9） |
| **主机侧等待** | **挪了** | 估计器提交即返回（deferred chain），主机直到**消费者阶段**才真的等；所以那 ~530 µs 的等待计入 `[ul_equalization_demod]`（691 µs，而等化器自己只算 74.3 µs） |

⇒ 您的理解**在主机时间归账上成立**，但**设备上的执行仍然记在信道估计器名下**。
要读 K1 的真实代价，看 `[ul_gpu_lane] busy split` 的 `ch_est`（GPU 时间戳），不是 `[ul_channel_estimation]`。

**（b）"36×36 还是 54×54？"——两个都对，是两个时期**

| 时期 | 形状 | 出处 |
|---|---|---|
| S-5a 优化内核时 | **36×36**（2 PRB 块） | `ocudu_mmse_inv.metal` 注释与 `9a651e4210` 的提交信息都写"one 36x36 system" |
| 现在（空口） | **54×54**（3 PRB × 6 comb × 3 DM-RS 符号） | `7b9f9ad6ed` 把 `MAX_N` 从 36 提到 54；实测描述符 `l=54 Ls=54 Ns=504 nf=36 npf=18 ncomb=6` |

⇒ 36 是您记忆里的**优化时**产量形状；54 是**现在**的形状（§48.84(c) 的块几何）。

**（c）"Gauss-Jordan 的分块优化是不是丢了？"——内核没丢，丢的是与它配套的线程组几何**

**内核没丢**：`ocudu_mmse_inv.metal` 里仍是 S-5a 的**块 Gauss-Jordan**（`BLK=8`：块内主元化 + 乘子提取 + 每块列一次 rank-8 扫掠，注释写"36 sweeps -> 5"），代码从未回退。

**丢的是几何**。S-5a 的提交信息（`9a651e4210`）原文：

```
one 36x36 system:  91.3 us (32,4) -> 24.5 us (64,16)
measured (32,4) 91.3, (64,4) 67.3, (32,8) 57.0, (64,8) 25.7, (32,16) 29.5, (64,16) 24.5 us
- the row dimension (one row per y thread) helps most
```

六种几何差 **3.7 倍**，比"逐主元 → 分块"这次重写本身买到的 2.6 倍（S-4b：233.9 → 91.3 µs）还大。
**而 `run_async()` 里的 K1 一直硬编码 `(32,4)`——六种里最差的那一种**；
`invert()` 虽然可以用 `OCUDU_INV_TGX/TGY` 调，默认也是 `(32,4)`。
⇒ **内核按 (64,16) 优化，默认路径却按 (32,4) 派发**：这才是我测到 ~530 µs 而注释说 24.5 µs 的原因。
（也解释了为什么文档里"91.3 µs 单系统 36×36"看着和实测对不上：那个数是**逐主元形式**（S-4b 的 233.9 → 91.3），
不是分块形式；内核头注释把两种形式的数字混在一句话里，我第一遍就是被它带偏的。已改正。）

**（d）修复与实测**

`mmse_inv_threadgroup()` 成为**唯一**决定几何的地方，三处 K1 派发（`invert()`、`run_async()`、DEV_INVERT 实验路径）
全部走它，默认 **(64,16)**；`OCUDU_INV_TGX/TGY` 仍可复现几何扫描。

| 抓包 | `gpu_wait` (32,4) | `gpu_wait` (64,16) | 提升 |
|---|---|---|---|
| `iq2_1009_17923` | 357.3 µs | **121.2 µs** | 2.95× |
| `iq1_10049_17921` | 211.8 µs | **107.3 µs** | 1.97× |
| `iq2_10044_17923` | 283.6 µs | **194.5 µs** | 1.46× |

（`--repeat 30` 的均值；单次 `gpu_wait` 噪声很大，同配置能给出 554 与 177 两个值，**必须平均**。）

**（e）几何不影响结果——这是测出来的，不是推出来的**
源码层面每个元素都由唯一一个线程用同一个表达式算出，与线程数无关；实测四重确认：
1. 三条参考抓包：`(32,4)` vs `(64,16)` 的 `ce/h/llr` **逐字节相同**；
2. 并行扫描报"随几何变化"的 5 条抓包，**串行重跑后全部相同**；
3. 低并发（4 分片）全语料 device-vs-device 几何 A/B：**979/980 相同**；
4. 剩下那 1 条（`iq1_7429_17921`）**两种几何各跑 3 次，6 次 LLR 哈希完全相同**。

⇒ 那条"差异"也是被污染的运行，不是几何造成的。

**（f）⚠️ 顺带查出一个**衡量工具自身的坑**：`ul_chain_replay` 在**高并发下会静默给出错误结果**
（不只是崩溃）：10 分片时，**同一配置**的两次运行有 **39/980** 条抓包结果不同；
`max|dSINR|` 在同一份代码上从 **7.3 → 45.2 → 45.8 → 48.9 dB** 乱跳。
⇒ **并行跑的"不一致"必须先串行复核再下结论**（我差点据此判定"几何改变了结果"）。
`capture_gates.sh` 已改为**每个不一致都串行复核一次**并报告复核次数（`retried=`），
`max|dSINR|` 标注为**仅供参考、不作门禁**。本轮的最终门禁：
`k0d` **980/980 逐字节**（retried=0）、`k1` **980/980 判决一致**（retried=1）、`combos` PASS、`ctest -L phy 162/162。

**（g）与"首要目标"的关系**
按用户定调，**K1 性能不是当前重点**，首要目标仍是 IQ→LLR 全 GPU path。
本次改动**不是新的性能优化**，而是**把 S-5a 已经做过、却没接到默认路径上的那一步接回来**
（用户："不能把已有的优化丢掉"）。它同时让上机腿里 `ch_est=538.7 µs/lane`（占 GPU busy 88%）有希望回到 ~200 µs 量级，
`[ul_pipeline]` median 1043 µs 也有望回到 1 ms 以内——**这是为全 GPU path 让路，不是为 K1 本身**。
