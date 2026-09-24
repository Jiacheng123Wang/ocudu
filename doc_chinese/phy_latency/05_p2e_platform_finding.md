# 05 —— P2-E 的实施结果：机制**已按计划实现**，但**这台机器把"命令缓冲中途的事件信号"推迟到整条缓冲完成**（计划前提被证伪）

> 2026-09-25。本文只讲 P2-E；P0-5（探针配对）见 §4 与 `session_handoff_2026-09-25-1.md`。
> 所有读数都来自本机的离线测量或本仓的测试，出处逐条给出；**未飞的腿一律标注"未验证"**。

## 0. 一句话

`OCUDU_DFT_RELEASE_TOKENS_EARLY`（E-1/E-2，默认关）已实现并可读（计数器 `tokens_early=`），
但**在这台机器上开它不会让输入提前回来**：macOS 26.6.2 / Apple Silicon 只在**命令缓冲完成**时才发布
"在 encoder 边界之后编码的 `MTLSharedEvent` 信号"，而 P2-E 的整个机制就架在"中途信号会立刻发布"之上。
⇒ **P2-E 按计划无法交付**，需要用户裁决（§5）。

## 1. 计划要求的 vs 实现的

| | 计划（`04_p2e_plan.md` §2）| 实现 |
|---|---|---|
| **E-1** | 在块关闭/交出路径（`release_block()`）里、**最后一次读输入的 dispatch 之后**发信号 | ✅ `release_block()` 在 `endEncoding` 之后、交出之前 `encodeSignalEvent:`（`encodeSignalEvent` 是命令缓冲级 API，Metal 规定**有 encoder 活动时不得调用**，故只能"关编码器后、commit 前"；位置仍在所有前端 dispatch 之后、adopter 的 dispatch 之前） |
| **E-2** | 开关打开时改由 `notifyListener:atValue:` 释放该块 tokens，**保留完成处理器作兜底** | ✅ `arm_tokens_on_complete(cb, tokens, early_generation)`：完成处理器**两条臂都留着**，`early_generation != 0` 时再加一个事件通知；两条路都汇入幂等的 `release_block_tokens()`，谁先到谁算 |
| 开关 | `OCUDU_DFT_RELEASE_TOKENS_EARLY=1`，**默认关** | ✅ 每次调用读取（单测可在同一进程里开关）；反向臂 = 现行为 |
| 不新增提交 | 只在既有缓冲里多一个信号 | ✅ `cbs/lane` 不变（默认关时逐字节等价，见 §3） |
| 可读性 | ——（计划未要求）| ✅ 新增计数器：`token_early_signals` / `token_sets_by_event` / `token_sets_by_complete`，并打印在既有的 `[metal_stats] dft handover … (armed=…) tokens_early=signals:N,by_event:M,by_complete:K` 行尾；`token_release_stats()` 可供单测读 |

代码位置：`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.{h,mm}`（`release_tokens_early_requested()`、
`token_release_event()`、`token_release_listener()`、`encode_token_release_signal()`、`block_token_set::early_armed`）。

## 2. 证伪：平台把中途信号推迟到完成（三种测法 + 一条对照）

**平台**：macOS 26.6.2（build 25G83）/ Apple Silicon。**载体**：`dft_release_adopt_metal_test` 新增的 premise 一节
（每次运行都测，见 §6），以及三份一次性最小程序（同机、同一周）。

| 测法 | 形状 | 读数 |
|---|---|---|
| **A 宿主轮询 `signaledValue`** | `[短 encoder] → signal → [长 encoder（~100 ms）]`，宿主在缓冲运行期间轮询 | 值**只在完成时**出现（97.1 ms 见值 / 97.4 ms 完成，margin 0.3 ms） |
| **B `notifyListener` 投递时刻** | 同上，通知块里记时间戳 | 通知在完成时刻到达（105.8 ms vs 完成 105.9 ms） |
| **C 另一条命令缓冲 `encodeWaitForEvent`** | A 缓冲中途发信号，B 缓冲只等待该信号 | B 的完成时刻 ≈ A 的完成时刻（77.1 vs 77.0 ms）⇒ GPU 侧等待也没提前满足 |
| **对照（关键）** | `signal → [长 encoder（~80 ms）]`（信号在**第一个 encoder 之前**） | 值在 **2.04 ms** 就可见 ⇒ **信号本身的发布是即时的**，被推迟的是"encoder 边界之后"的那一个 |

⇒ **规则（本平台）**：**信号一旦编码在"已经创建过 encoder"之后，就只在整条命令缓冲完成时发布**；
编码在第一个 encoder 之前则立即发布。这不是"通知投递慢"（测法 A 与通知无关），也不是"宿主读得慢"（测法 C 是 GPU 侧）。

* 这条规则同样解释了**现有 D1 机制为什么是保守的**：`shared_queue::grid_ready_signal(cb)` 在合并车道上也是
  **中途**信号（前端 dispatch 之后、adopter dispatch 之前），而 PUCCH 的宿主读用 `grid_ready_wait()` 等它
  ⇒ 在本平台上它实际等价于"等整跳完成"。**（未验证：这一条是同一平台规则的推论，没有飞腿读数；登记为待验证。）**
  它是**保守**（更晚满足），不是错误。

## 3. 为什么这挡住 P2-E（不是"再调一下参数"）

输入 token 的释放是**宿主动作**（把 `rx_buffer_handle` 还给接收池，见 `puxch_processor_impl.cpp` 的
`release_symbol_input()`），所以必须有一个**宿主可见的时刻**。本平台上该时刻只能来自：

| 选项 | 代价 | 结论 |
|---|---|---|
| (a) 命令缓冲完成 | 现状 = **整跳**（D1 之后整条缓冲就是整跳）| 这就是要拆的东西 |
| (b) 让前端块**自成一次提交**（= P0-1 拆分臂的形态）| `cbs/lane` **+1**：n1/n78 现在都已是 **2.00 (max=2)**，会变 3.00 ⇒ **V4 不再满足** | 需要用户裁决（与 P2-F 同类） |
| (c) 由**第三次提交**只带信号+等待来"提前完成" | 同样是 +1 提交 | 同 (b) |
| (d) 宿主把输入**拷进引擎自己的 ring**（放弃零拷贝） | 破"零拷贝 radio input"契约（`dft radio inputs` 一行会变），且 D1 的立项目的之一就是去掉这次拷贝 | 不建议 |
| (e) 加一个宿主**自旋轮询**线程读设备侧写完的标志位 | 在收包 RT 路径上多一个自旋线程；需要 kernel 内 barrier + 可见性论证 | 作为最后手段登记，未实现 |
| (f) `commitAndContinue`（"提交并继续"） | **macOS SDK 里不存在**（`MTLCommandBuffer.h` 只有 `enqueue`/`commit`；已查 SDK 头文件）| 不可用 |

⇒ **"不新增提交、不新增命令缓冲"（`04_p2e_plan.md` §2 的硬约束）与本平台的信号语义不能同时成立。**

## 4. 与 P0-5 的关系（本轮的另一半）

P0-5（相位探针 ↔ 车道探针按 **slot** 配对）已完成并提交（`747b9d475b`）：
`[ul_gpu_lane] paired with the phase segments (P0-5): samples=… of phase_samples=…` +
`paired ratios (P0-5)` + `paired reading (P0-5)`，`p0_gate.sh` 新增 **C2**（只对声明了 `OCUDU_UL_PHASE_SEGMENTS=1` 的腿判：
**配对样本数 == 相位三段样本数**）。离线已验证：`ctest -L phy` 193/193、单测新增一例（hook 契约）、
3 capture × 4 dump 与 pristine HEAD 二进制**逐字节相同**；**那条腿未飞（未验证）**。

为什么在这里提它：§2 的结论让 P2-E 的"预期持有期"读数**暂时无用武之地**（机制不生效时池数字不会变），
而 P0-5 给出的**分母**（`busy/residency`、`eq_demap/residency` 的配对分布）在**不动机制**的前提下就能判读
"这一跳的 residency 里到底有多少是前端" —— 这是下一步判断"值不值得为前端单独花一次提交"的直接输入。

## 5. 需要用户裁决的选项

| # | 选项 | 代价 / 收益 |
|---|---|---|
| **1** | **接受 +1 提交**：把前端块做成独立提交（P0-1 拆分臂的生产化，去掉诊断身份） | 输入在前端结束时回来（**持有期 ≈ 前端那段**）；代价 `cbs/lane` 2.00→3.00，**V4 需用户重新裁决**（V4 原文 `cbs/lane ≤ 2.00 (max=2)`）；另需处理 §5.9.138 ③ 的"排序语义变严"问题 |
| **1b** | 变体（**未验证，仅登记**）：把前端块做成**车道两次提交中的第一次**（即用它替换现有的 `ch_wt` 提交，而不是在它之外再加一次）| 若成立则 `cbs/lane` 仍 2.00；但需要重排估计器内部的提交点（`ch_wt` 现在承担"提取完成即提交、让权重段的编码与之重叠"的角色），**可行性未做任何验证**，风险与工作量都远大于 1 |
| **2** | **不改持有期，改它对池的影响**：P2-D（按测得持有期定池容量）/ 提高并发度（P1-8 → P2-F）| 不动提交数、不动引擎；但 P2-D 的输入（持有期直方图）还在，P1-8 的 5 秒停顿仍未解释 |
| **3** | **保留本开关（默认关）作为仪器**，先不交付 | 成本已付（代码+计数器+单测）；任何平台/任何一天，一条腿 + `OCUDU_DFT_RELEASE_TOKENS_EARLY=1` 就能回答"这台机器会不会提前发布中途信号"（`by_event > 0`） |
| **4** | 宿主拷贝输入（放弃零拷贝）| 契约会红，不建议 |

**我的建议**：先做 **3**（保留仪器；零风险、默认关），把 **1** 与 **2** 交给用户裁决 ——
因为 **1** 触碰 V4 的提交数上限（用户此前明确"只约束交付"并保留二次裁决），
而 **2** 里的 P2-D 现在**有 P0-5 的配对分布可用**，比 P2-E 更便宜。

## 6. 复现方法（离线，不飞腿）

```bash
# 1) 平台前提 + 两条臂（每次运行都打印判定行）
./build/lib/phy/generic_functions/metal/dft_release_adopt_metal_test
#    期望读到（本机 2026-09-25）：
#    [dft-release] P2-E premise: a signal encoded MID-buffer was published ONLY AT COMPLETION
#                  (signal seen at 97.1 ms, buffer completed at 97.4 ms, margin 0.3 ms) ...
#    [dft-release] P2-E counters: early signals=20, released by the event=0..4, by a completion=36..40
#    （by_event 的 0..4 是"通知在完成时刻与完成处理器抢 token"的竞态，**不是**提前发布的证据）
# 2) 引擎自己的行（任意 arm，开关开时）
grep -a "tokens_early=" <leg>.log.stderr
# 3) 反向臂（两条腿，需飞腿 —— **本轮未飞**）：n1 默认配方 + OCUDU_DFT_RELEASE_TOKENS_EARLY=1，
#    看 [ul_rx_pool] held_max / starved_events 与 [metal_stats] dft handover … tokens_early=
#    ⚠ 若 by_event == 0，**池数字不能用来判断"持有期是否重要"**（机制根本没生效）；
#      引擎在那种情况下会自己打一行 "P2-E: N early token-release signal(s) encoded and NOT ONE released a block …"。
```

## 7. 离线验证（本轮实际跑过）

| 网 | 结果 |
|---|---|
| 与 pristine HEAD 二进制逐字节比对（`ul_chain_replay`，3 capture × 4 dump，默认关）| **0 differing**（P0-5 + P2-E 都未动数据面）|
| `ctest -L phy` | **100% passed out of 193**（含扩展后的 `dft_release_adopt_metal_test`）|
| `ab_dumps` arm2（historical，sigma2 固定）| 0 differing bytes |
| `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` vs 默认）| **红，且与 pristine HEAD 二进制读数完全相同（15/27 capture，159068 B）⇒ 改前就红，与本轮无关** |
| `value_net.py`（47 capture）| **红，183 problems，pristine HEAD 二进制给出同一份失败清单**（归档基线是 9-20 05:26，而 CE 的 `.metal` 源在 9-20 10:27–21:41 改过、`.metallib` 9-24 14:51 重建 ⇒ **基线陈旧**，非缺陷）|

## 8. 明确没做的事

* **没飞腿**：P0-5 的那条 n1 腿与 P2-E 的反向臂都还没跑（用户没有飞；本会话只做离线）。
* **没改 V1–V5、没改判据、没动提交数**（默认关时行为与改前逐字节相同，见 §7 第一行）。
* **没把开关默认打开**：`OCUDU_DFT_RELEASE_TOKENS_EARLY` 默认关，且 §2 已说明本平台开它也不生效。
