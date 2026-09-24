# 04 —— P2-E 实施计划：**输入缓冲寿命解耦**（把 keepalive 从"整跳完成"挪到"最后一个读输入的 dispatch"）

> 立项依据：`00_status.md` §7.1——n1 的 2.85 倍退化已定位为 `ce`（等串行车道），而把并发度提到 2 虽把 `ce` 从 3228 µs 压到 ~51 µs，却**两次复现**同一次 **~5 秒收包停顿**（`radio sample continuity: 2 gaps`，每次丢 ~1.531 亿样点，并伴随 5.0004 秒 residency 异常）。
> 最可能的共因就是本项要拆的链：**输入被持有到整跳命令缓冲完成 ⇒ 池被抽干 ⇒ 接收线程在 `pop_blocking()` 上被卡住**。

## 1. 机制（读码核实）

* 接收缓冲（输入样点）的存活由 **keepalive token** 保证：`dft_metal_engine::retain_for_block()` 每符号 attach 一个（14/槽），计数在
  `keepalives` / `keepalives_released` / **`keepalives_in_flight_max`**（`ocudu_dft_metal_engine.mm:114-118`，attach 处 `:161-170`）。
* 释放**挂在整条命令缓冲的完成上**（`arm_tokens_on_complete()` 的 `addCompletedHandler`）——而 D1 之后"整条命令缓冲"= **整跳**
  （前端变换 + 估计器 + 均衡 + 解映射）⇒ **持有期 = 整跳跨度**（§5.9.129 ② 的原文）。
* 但**真正读"接收样点"的只有前端那一段**（DFT / 网格写）；估计器、均衡器、解映射读的是**网格**。
  ⇒ **输入只需要活到"最后一个读它的 dispatch"**，现在的绑定方式是**多持有**。
* 机制已存在：本代码库**已经**在命令缓冲**中途**发信号（`ocudu_metal_queue.mm:464/528` 的 `encodeSignalEvent`），
  且 P0-1 的诊断拆分（`OCUDU_LANE_DIAG_SPLIT`）已证明"在 `adopt()` 处切开后，前端那一组恰好是 `dft`"（离线 `dft=37.5us/lane (83%)`、空口 `dft=937.9us/lane (88%)`）。

## 2. 改动（形态，两处锚点）

| # | 位置 | 改什么 |
|---|---|---|
| **E-1** | `ocudu_dft_metal_engine.mm` 的 **块关闭/交出路径**（`release_block()`，`~:1160-1235`；`open_transforms` 归零处 `:1122`/`:1170`）| 在**该块最后一次网格写 dispatch 之后**（即关编码器之前）`encodeSignalEvent:token_event value:generation` |
| **E-2** | `arm_tokens_on_complete()`（同一文件）| 开关打开时，把**该块的 tokens** 改为由 `notifyListener:atValue:`（事件到达该 generation）释放，而不是由 `addCompletedHandler`（整缓冲完成）释放；**保留**完成处理器作为兜底（若事件未到达就仍按完成释放，防止泄漏） |

**开关**：`OCUDU_DFT_RELEASE_TOKENS_EARLY=1`，**默认关**；反向臂 = 现行为（`=0`）。
**不新增提交、不新增命令缓冲**（只在既有缓冲内多一个信号）⇒ **不动 V4**（`cbs/lane` 保持 2.00）。

## 3. 必须先验证的前提（否则会读越界）

* **"谁读输入"的完整清单**：必须确认**除前端那一段之外没有任何 dispatch（或设备侧读者）读接收样点**。
  可用 P0-1 的拆分臂交叉验证：`dft` 组 = 前端，其余组若在 `dft` 之后仍读输入，则 token 不能早于它们释放。
* 反向臂（既有网）：`value_net`（容差 + NO-NaN + LLR 符号）、`ab_dumps`（逐字节）、
  `l1_handover_arms` / `l1_hop_arms`、`edge_block_arms`、`dft_release_adopt_metal_test`、`ctest -L phy`、契约 8/8。

## 4. 预期（可证伪）

| 量 | 现在 | 预期 | 判据 |
|---|---|---|---|
| 输入持有期 | ≈ 整跳跨度（n1 ~5.25 ms）| ≈ 前端那一段（`t2f` 内的前端部分，n1 ~41 µs 主机 + 网格写）| 直方图（P0-2，尚未实现）|
| `[ul_rx_pool] held_max` | 8（=池）| **≤3** | **V2** |
| `starved_events` | 368–1364 | **0** | **V2** |
| 那次 **~5 秒停顿** | 两次复现 | **消失** | `radio sample continuity: gaps=0` 且 residency `max` 回到 ms 量级 |
| 跨度中位（n1，并发 1）| 5251–5268 µs | **下降**（不预设幅度）| V1 用重载基线，不在本条单独声称 |
| `cbs/lane` | 2.00 | **2.00（不变）** | **V4（关键：不靠加提交）** |

## 5. 判读（先写死）

* 5 秒停顿**消失** + 池 `held_max/starved` 改善 ⇒ **因果链坐实**（持有期 → 池 → 收包阻塞），本项成为并发度之外的第二个候选交付。
* 5 秒停顿**仍在** ⇒ 与持有期无关，回到"并发 2 下另有他因"（下一个嫌疑：并发 2 时同一 strand 上的邻居任务 / 池与解码线程的互动），并把本项按"更正确的结构"保留（它仍减少多持有）。
* 若 `value_net`/`ab_dumps` 出现**任何**逐字节/容差差异 ⇒ 立即回退开关并重新审计"谁读输入"。

## 6. 与其它项的关系

* **P1-8（并发度）**：本条**不替代**它；两者独立。若本条让 5 秒停顿消失，则 **P1-8 的交付可行性**需要重评（重跑并发 2 + 本项开）。
* **P2-D（按测得持有期定池容量）**：本项改变持有期分布 ⇒ P2-D 必须在**本项之后**重算。
* **不做**：加大池当解法（用户裁定，§5.9.130 ①）；用提交数换时延（V4）。
