# 从"可测的错过率"到"上层看不见的降级"
## —— macOS PHY 实时性的实现方案（**全部结论从代码读出**）

> 状态：实现方案（v1.0，2026-10-02）
> 定位：**分析 + 可实现方案**，不是设计愿景。每一条都有**代码位置**，并明确区分
> 「读出来的事实」与「我做的推断」。
>
> 起点（用户认可）：**"把'错过截止期的比例'做成有界、可测、可归因的量"**，
> 然后在此之上规划 PHY 的应对——**即使这些错过截止期的空口信号被扔掉，
> 上层应用看到的底层无线连接仍然是稳定、可靠、实时的。**
>
> 关联：`phy_thread_scheduling_plan.md`（三杠杆与看门狗）、`thread_running_high_level_status_and_plan.md`（§4bis）。

---

## 0. 一句话

> **本方案不需要发明新架构：`lower_phy_baseband_processor` 的 RX 侧已经有一个完整的、
> 经过实测标定的"有界等待 + 主动丢弃 + 把故障关在一个块里"的实现（Fix B）。**
> 它的注释把设计规则和代价都写死了：
>
> > the wait is **BOUNDED**. Past the budget the caller gets the RESERVE buffer and is told to **drop the block**:
> > the radio keeps being consumed, so **its ring does not overflow and the samples of every LATER slot survive**
> > — the price is this one block's samples, i.e. **one HARQ retransmission**.
>
> **⇒ 方案 = 把同一个模式搬到 DL TX 侧，并补上它缺的那一半：让"扔了什么"对 MAC 可见。**
> DL 侧今天**完全没有**这个机制：交棒是**无条件**的（`transmitter.transmit()` 不看 margin），
> 而 DL 的内容比 UL 更脆——**TDD 下迟到的 DL 会污染紧随其后的 UL 接收**。

---

## A. "上层看不见"在代码里到底指什么

**先把验收标准落到可测量的东西上。** 从判据与仪器看，上层"看见稳定"= 下列四项同时成立：

| 层 | 可观测 | 仪器（已存在） | 界限 |
|---|---|---|---|
| PHY/空口 | DL 交棒错过截止期的**率** | `[dl_tx_slack]` 的 `late()` + `below_{2,1,0.5}ms` | `AT/BELOW 0` 率 ≤ 0.0005% |
| PHY/空口 | 接收流不连续 | `gaps` / `gap_samples` / **每次 gap 的时长** | `gaps = 0` |
| 传输 | 传输迟到 >1 ms 的率 | `slip` / `recv` | ≤ 0.0005% / 0.0004% |
| 上层 | **HARQ 重传**（丢弃的代价） | ——**尚无专用计数器**（见 §D3） | 待登记 |
| 上层 | 连接是否还在 | RLF / 掉线 | 腿里 UE 是否持续在网 |

**★ 关键认识**：前三项是**因**，HARQ 重传率是**果**。
"上层看不见"的准确含义是：**因存在（有错过），但果可控（重传吸收，连接不断）**。
⇒ **所以方案必须同时做两件事：把"因"压下去（调度线），并让"果"可测、可封顶（本方案）。**

---

## B. ★ 现有代码里的样板：RX 池的 Fix B（**本方案的模板**）

**这一段是读代码得到的，不是读文档得到的。**

### B.1 五要素（`lower_phy_baseband_processor.cpp`）

| # | 要素 | 代码 |
|---|---|---|
| 1 | **等待是有界的**，不是无限阻塞 | `:1264-1266`：`wait_slice = min(rx_reap_slice, rx_park_budget + 1µs)` |
| 2 | **预算耗尽 ⇒ 主动丢弃，但仍然消费电台** | `:1294-1298`：`if (rx_pool_drop_enabled() && (parked_us > rx_park_budget) && (rx_reserve_buffer != nullptr)) { rx_pool_note_dropped(...); dropped = true; return rx_reserve_buffer; }` |
| 3 | **专门留一个不在池里的缓冲**承担丢弃 | `:911`：`rx_reserve_buffer = make_shared<...>(...)`，注释："**ONE buffer of the same shape that is deliberately NOT in the pool**" |
| 4 | **丢弃被计数、等待被计数** | `rx_pool_note_dropped(park_us)` / `rx_pool_note_wait(wait_us)`（`:480`、`:505`） |
| 5 | **代价被明确命名** | `:1291-1293`："the price is this one block's samples, i.e. **one HARQ retransmission**" |

### B.2 它为什么成立：预算必须**小于资源的物理界**

`:1257-1266` 的注释给出了整个设计最硬的一条规则：

> a slice longer than `rx_park_budget` makes the budget unreachable — the first wait would already have parked
> for the slice, and by the time the drop was decided **the radio's ring (which overflows at ~4.4 ms of park,
> measured)** would have lost its samples anyway. With the drop enabled the slice is therefore `min(reap slice,
> budget)`: the reap runs **ten times as often** while the pool is dry and **the drop lands inside the ring**.

**⇒ 三条可复用的规则（**这是方案的骨架**）：**

| 规则 | 含义 |
|---|---|
| **R1** | **丢弃的判定必须发生在资源的物理界之内**（这里是电台环的 ~4.4 ms）。界外丢弃没有意义——那时已经丢了别人的样本 |
| **R2** | **收缩等待 ⇒ 必须同时提高"解堵"的频率**（`min(reap, budget)`，reap 频率 ×10）。否则丢弃只是更快地放弃 |
| **R3** | **丢弃的粒度 = 一个可恢复单元**，并且这个单元的名字要和上层能吸收它的机制对上（"一个块 = 一次 HARQ 重传"） |

### B.3 这条路径的"因"也已经被计量

`:1676-1700`（接收连续性）：`gaps` / `gap_samples`，并且
**每个 gap 的时长**都被记下来（`ul_rx_note_gap(µs)`），注释说明理由：
"the sizes are what separates **'a slot's worth'** from **'the radio's ring drained'**"。
第一次 discontinuity 打一条 `PHY.warning`。

**⇒ 这套东西就是"可测、可归因"的现成范例。**

---

## C. 对照：DL TX 侧**没有**这套机制，而且后果更坏

### C.1 交棒是无条件的（代码为证）

`lower_phy_baseband_processor.cpp:1008-1115` 的 `dl_process()`：

```cpp
// … 等待（有界，2 * slot_duration 超时）→ 节流 → downlink_processor.process(timestamp)
result.metadata.ts = timestamp + tx_time_offset;
// … [dl_tx_slack] 在这里测 margin（只测！）
const auto tx_call_begin = std::chrono::steady_clock::now();
transmitter.transmit(result.buffer->get_reader(), result.metadata);   // ← 无条件
```

**⇒ `margin_us` 被算出来、被记进分布、被分成 `below_2ms/1ms/500us` 与 `late`——
然后【不被使用】。** 迟到与否，样本一样交出去。

### C.2 "谁迟到"已经被分开了，但只在**事后**

`:80-83` 给出一条把责任劈开的判据：

> a leg with `AT/BELOW 0 = 0` here **and UHD failures in its log** says the lateness is **INSIDE the radio or its
> driver**, not in this hand-over.

`:83-90` 再用 `[dl_tx_call]`（`transmit()` 调用本身的时长）劈开**驱动/USB**与**UHD 的 worker 线程**：

> If `transmit()` itself blocks for milliseconds, the radio or the USB link is pushing back **inside the call
> (and no host scheduling change can help)**; if it returns at once, the samples are sitting in UHD's own queue
> and **its worker thread** is the one that is late.

**⇒ 所以"DL 迟到"其实是三个不同的故障，仪器已经能分开它们——但没有任何一个触发**应对**。**

### C.3 ★ 比"丢一个 TB"更坏的后果：**TDD 下迟到的 DL 会咬到 UL**

电台是 TDD（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。DL 交棒迟到时，电台要么 underflow（不发），
要么把这一段**晚发**——而晚发会越过 DL/UL 切换点，**进入随后的 UL 时隙**。

**⇒ 后果不是"一个 PDSCH 丢了"，而是"gNB 自己的上行接收被自己的下行污染"。**
而 UL 侧的处理是**不能靠 HARQ 从污染中恢复**的：污染的是样本，不是 TB。

**★ 这是本方案认为 DL 侧应当优先于 UL 侧修的理由**（与直觉相反：用户观察到的症状在 UL 的 `[ul_rx_wait]`，
但 UL 已经有 Fix B 兜着，DL 没有）。

> **⚠ 标注**：C.3 的**机制是读代码得到的**（TDD 配置 + 无条件的晚交棒）；
> 但"实际上发生了多少次跨到 UL 的晚发"**我没有读数**——`[dl_tx_slack]` 只记 margin，
> 不记"这次交棒是否越过了切换点"。这正是 §D1 要补的。

### C.4 DL 的内容**读不出来**

`include/ocudu/phy/lower/processors/downlink/downlink_processor_baseband.h:23-34`：

```cpp
struct processing_result {
    baseband_gateway_transmitter_metadata metadata;
    baseband_gateway_buffer_ptr            buffer;
};
```
注释：*"If the given timestamp does not match with a slot boundary **or there is no transmit request**,
it returns a valid buffer **filled with zeros**."*

**⇒ 两个事实：**
1. **结果里没有"这个时隙装了什么"**（PDSCH？PDCCH？SSB？）。所以交棒点**无法做内容相关的决策**。
2. **"空时隙发零"已经是既有行为**——也就是说**"干净地什么都不发"这条路在 DL 侧已经存在**，
   只是它今天的触发条件是"没有发送请求"，而不是"来不及了"。

---

## D. 实现方案（四步，每步独立可落地、可判、可回退）

**排序原则**：先让"因"可分类、可归因（D1/D2，**不改空口行为**），再改策略（D3，**改空口行为**），
最后闭环（D4）。这与本仓库"先测量、再登记、再改一处"的纪律一致。

### D1 —— 让 DL 的错过**可分类、可归因**（**不改行为**）

**做什么**：在 `dl_process()` 已经算出 `margin_us` 的那个位置（`:1079-1103`），补三个字段：

| 字段 | 从哪来 | 为什么要 |
|---|---|---|
| **这次交棒是否越过 DL/UL 切换点** | `result.metadata.ts` + 时隙在帧结构里的位置（配置里有） | ★ 判 C.3 是否真的发生。**这是本方案的第一优先读数** |
| **`late` 的分布**（不只计数） | 已有的 `late()` 计数改成直方图 | 现在只有计数；"偶尔晚 100 µs"与"经常晚 5 ms"是两种缺陷 |
| **`transmit()` 是否返回得太晚**（已有 `tx_call_max_us`）| 已有 | 把"驱动晚"与"我们晚"分开（`:83-90` 已设计好） |

**两把钥匙**：编译期 + env，默认关 ⇒ 关着时报告逐字节不变。
**反向臂**：合成一个越过切换点的 `metadata.ts` ⇒ 新计数器必须变红。

**完成判据**：一条安静腿能打印出"错过 N 次，其中 M 次越过切换点"。**M=0 是重要结论**（说明 C.3 不成立）。

### D2 —— 让 DL 内容在交棒点**可知**（**不改行为**，但改接口）

**做什么**：给 `processing_result` 加一个**内容分类**字段。分类从 `downlink_processor` 内部取——
它在 `process()` 时知道自己在调制什么。

**这一层怎么设计才对（三条约束）：**

| 约束 | 理由 |
|---|---|
| **分类，不是枚举具体信道** | 交棒点需要的是"**丢了它，上层能不能自己补回来**"，不是"它是 PDSCH 还是 PDCCH"。所以字段应当是 `recoverable_by_harq` / `not_recoverable` / `empty` 三类 |
| **不能要求 DL 处理器去查 3GPP 表** | 这正是 `metal_vs_cuda_architecture.md` §6.4 记的那条可借鉴惯例："**参数由调用方推导、后端不查 3GPP 表**" |
| **默认必须安全** | 分类未知时**按最坏处理**（`not_recoverable`），否则新路径会静默地把自己标成"可以丢" |

**完成判据**：单元测试能对四类时隙（空 / 仅 PDSCH / 含 PDCCH / 含 SSB）各断言出正确的分类。

### D3 —— DL 的期限策略：**内容相关**（**这一步改空口行为**）

**这一步的规则应当由 D1/D2 的数据决定，本文只给骨架与默认值。**

| 内容 | 迟到的默认策略 | 理由（从代码读出） |
|---|---|---|
| **`empty`** | 照发（零） | 已经是零，无损失 |
| **`recoverable_by_harq`**（PDSCH） | **照发**（今天的无条件行为） | 电台晚发/丢弃都导致 HARQ 失败一次；**照发还有可能赶上**。**并且不需要新代码** |
| **`not_recoverable`**（含 PDCCH/SSB） | **交零**（不发这一段），并计数 | 晚发会**污染随后的 UL**（§C.3），而"干净地不发"这条路已经存在（`processing_result` 的零缓冲语义）。⇒ **少发一个 PDCCH 的代价（一个调度机会）小于污染整个 UL 时隙的代价** |

**★ 关键：`not_recoverable` 的判据是"margin ≤ 0 **且** 该时隙的 DL 波形会跨过 DL/UL 切换点"**，
不是"margin ≤ 0"。因为"晚一点但仍在本时隙内"通常仍然能解调。

**两把钥匙 + 预登记**：`OCUDU_DL_TX_DEADLINE_POLICY`（默认 `0` = 今天的无条件行为）。
臂的判据：`AT/BELOW 0` 率不变差、`gaps` 不变差、**越过切换点的计数下降**、UE 不掉线。

### D4 —— 把"扔了什么"送到 MAC（**闭环**）

**为什么需要**：`[dl_tx_slack]` 是**进程级 atexit 报告**，MAC 看不到。
所以即使 PHY 知道"我这个时隙丢了 PDCCH"，调度器下一帧照旧按"上一帧的 PDCCH 成功了"来假设。

**做什么**：把丢弃做成一个**可上报的事件**，而不是一个日志行。落点有两个候选，**需实测选一**：

| 落点 | 优点 | 风险 |
|---|---|---|
| 已有的 **FAPI/PHY 上报通道**（错误指示） | 走既有路径，MAC 已有处理逻辑 | 要确认该通道的语义允许"PHY 主动上报" |
| 一个新的、**只读的**进程级计数器 + MAC 周期性取样 | 最小侵入、不改接口语义 | 引入"取样延迟" |

**★ 这一步的判据必须是端到端的**：**HARQ 重传率**与**RLF/掉线**。
而 `HARQ 重传率` 目前**没有专用计数器** ⇒ **D4 要顺手把它做出来**（这是"果"的唯一直接读数）。

---

## E. 度量与判据（全部用已有仪器 + 三个新增）

| 量 | 现状 | 本方案的动作 |
|---|---|---|
| `[dl_tx_slack]` margin 分布 + `below_{2,1,0.5}ms` + `late` | **已有** | D1 把 `late` 变成分布 |
| `[dl_tx_call]`（`transmit()` 时长） | **已有** | 不动 |
| **越过 DL/UL 切换点的交棒次数** | **没有** | **D1 新增** ★ 第一优先 |
| `gaps` / `gap_samples` / gap 时长 | **已有** | 不动 |
| `slip` / `recv` 率 | **已有** | 不动 |
| `AT/BELOW 0` 率 | **已有** | 作为 D3 的主判据 |
| **DL 内容分类计数** | **没有** | **D2 新增** |
| **HARQ 重传率** | **没有** | **D4 新增** ← "上层看不见"的唯一直接读数 |
| RLF / 掉线 | 腿里可见（UE 是否在网） | 作为回退条件 |

**★ 判据的分层**（避免把"因"当"果"）：
**主判据 = HARQ 重传率 + UE 是否持续在网**；`AT/BELOW 0`、`gaps`、切换点计数是**分解**。

---

## F. 不做的事

| ❌ | 为什么 |
|---|---|
| **在没有 D1 的数据之前改 DL 交棒策略** | C.3 的机制成立，但**发生率未知**。先测再改（否则就是"同一对腿里改两件事"） |
| **把"margin ≤ 0 就丢"当成统一策略** | 对 PDSCH 有害（照发还有可能赶上，丢了必然重传）；对含 PDCCH/SSB 的时隙才有意义 |
| **用无界等待换"不错过"** | `:1257-1266` 已经证明：**等待超过资源物理界（电台环 ~4.4 ms）就没有意义**——界外已经丢了别人的样本 |
| **只压低"因"（错过率）而不测"果"（HARQ 重传/RLF）** | 用户的目标是**上层看不见**；只压"因"可能是在优化一个上层本来就能吸收的量 |
| **把 `processing_result` 的分类做成"信道枚举"** | 交棒点需要的是"能不能恢复"，不是"是什么信道"；枚举会让 DL 处理器去查 3GPP 表 |
| **在 loopback 台上判 D3** | loopback 的流控是 `sleep(1µs)` 自旋 + 读者进度断言（`radio_session_realtime_loopback_impl.cpp:196-206` / `loopback_buffer.cpp:31-38`），**它的崩溃判据不反映空口** ⇒ D3 只能上电台判 |

---

## G. 诚实清单

| 项 | 现状 |
|---|---|
| **§B（Fix B 是我的模板）** | **全部从代码读出**：`:911`、`:1257-1266`、`:1291-1298`、`:480`、`:505`。**"4.4 ms 环溢出"是代码注释里的实测值**，我没有复测 |
| **§C.1（交棒无条件）** | **读出来的**（`:1103` 附近，`transmit()` 前没有任何 margin 判断） |
| **§C.3（TDD 反噬 UL）** | **机制是从代码与配置推出的**（TDD + 无界晚交棒 + 零缓冲语义）；**发生率没有读数**。★ 这是 D1 要回答的第一件事，**在它出数之前不要把 C.3 当结论引用** |
| **§C.4（`processing_result` 不含内容）** | **读出来的**（`downlink_processor_baseband.h:23-34`） |
| **D2 的接口形状** | 是**设计建议**，未实现。三条约束（分类而非枚举、不由后端查表、默认最坏）来自本仓库既有惯例，不是实测 |
| **D3 的默认策略** | 是**骨架**。真正的规则必须由 D1（切换点发生率）与 D2（内容分类计数）的数据决定。**本文的默认值（PDSCH 照发、含 PDCCH/SSB 交零）是推断，不是结论** |
| **D4 的落点二选一** | 未定，需实测（既有 FAPI 上报通道 vs 只读计数器） |
| **HARQ 重传率计数器** | **尚不存在**。没有它，D3/D4 的"果"无法判——**所以它是 D4 的必做项，不是可选项** |
| **与调度线的关系** | 本方案**不替代** `phy_thread_scheduling_plan.md`：那条线压"因"（停顿），本方案管"果"（丢弃与吸收）。**两者相乘**：停顿 < 余量 ⇒ 不丢；丢了 ⇒ 本方案保证它被吸收且可见 |
| **我没有验证的** | 电台真机上一次迟到交棒**实际**发生了什么（underflow 丢帧？晚发？跨到 UL？）。`[dl_tx_call]` 与 UHD 的 `underflow/late` 能分开"我们晚"与"驱动晚"，但**"晚发是否越过了切换点"必须新测** |
