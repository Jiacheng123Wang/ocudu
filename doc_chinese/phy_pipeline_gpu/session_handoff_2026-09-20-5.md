# 交接（入口） — S14：**门变更（逐字节 → 容差 + 判决级）+ 抽取并行化（171→118 µs）+ 离线计时双峰根因**

> ## ★★ 本会话稍后发生了三件改变判断的事（先读这段，再读下面）
>
> **（一）门变了（用户裁定，§5.8.19）**：「逐字节不变」太严格、没必要——只要**算法逻辑正确、误差在范围内**即可，
> 而在 GPU 上为了并行度**改变计算顺序是常态**。**⇒ 门现在是 `python3 wip/value_net.py`**
> （按字段容差 + **NaN/Inf 硬判**（P0 的签名）+ **数量级哨兵**（P5 的签名）+ **LLR 符号一致率 ≥99.9%**
> （判决级））；**`wip/neutral_vs_baseline.sh`（逐字节）降级为"信息"**，仍然跑、仍然报，但**不再阻断**。
> 门的自证：`value_net.py --self-test` **8/8**（注入 P0/P5/20% 漂移/高置信翻转必须红；合法重排形态必须过）。
>
> **（二）已经在新门下落地两个改动**：`mmse_pilots_cfo` 从**单线程**改成线程组归约（§5.8.20）；
> `sigma2`/`epre` 的 **256 项 threadgroup 树（每级一道 barrier）改成 SIMD 组内归约**（§5.8.21）。
> **合起来：导频抽取 171.3 → ~118 µs（−31%）**；而**全 46 捕获 `_llr.bin` 差 0 字节**（旧门会拦下它）。
>
> **（三）★★ 离线 GPU 计时的"双峰"根因查清了：宿主自己的 GUI 在抢 GPU**（Chrome GPU 进程 + WindowServer
> 分时占用；**空臂** base-vs-base 的宽度可达 ±250 µs）。⇒ **绝对 µs 是模式相关的、比值不是**；
> **跑任何 A/B 之前先跑空臂**，空臂宽度 ≥ 要判的效应就作废（§5.8.20 ④）。

> **本文件是新会话的唯一入口**：读完它就能开工。上一份 `session_handoff_2026-09-20-4.md` 是上一会话的
> 过程记录（**不必读**）；技术细节都在常驻设计文档
> `gpu_phy_pipeline_design_and_implementation.md`（本会话新增 **§5.8.15**（仪器）、**§5.8.16**（K1 画像）、
> **§5.8.17**（空口分段重做）、**§5.8.18**（抽取逐 dispatch）、**§5.8.19**（★ 门变更）、
> **§5.8.20**（CFO 并行化 + 双峰根因）、**§5.8.21**（SIMD 归约），并更正了 §5.8.14 的两处）。
> 过程证据与复跑命令：`wip/S14_instruments.md`；**门**：`wip/value_net.py`；
> 现状/差距/规划一页纸：`wip/S14_status_and_plan.md`；机制测量：`wip/metal_counter_caps.mm`；
> 手机掉线的判读方法：`wip/S14_phone_drops.md`。

---

## 1. 一句话状态

**工作树 HEAD 以 `git log -1 --oneline` 为准**；**`build/apps/gnb/gnb` 已按它重戳**——
自查：`grep build_info build/hashes.h` 的短哈希 == HEAD。
**⚠ 之后再有任何提交，戳记就落后了，上腿前按 §9 重戳**（坑 35）。
工作树只剩用户自己的两个 config（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`gnb_rf_b200_tdd_n78_20mhz.yml`），**别动**。
**门：`python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py` → 47 捕获 0 问题 + `--self-test` 8/8；
`ctest -R metal` 9/9。**（逐字节网 235 文件里 **159 字节**不同——这是"信息"，不是失败。）
tag 未变：**`gpu_lane_commit_p1`**、**`gpu_lane_commit_p2`**。

**本会话全部离线**（没有 OTA；腿仍由用户跑，日志在 `doc_chinese/phy_pipeline_gpu/wip/logs/`）。

**本会话的代码改动**：① `OCUDU_CE_WAIT_TRACE=1`（等待点打点）；② 抽取的 6 个逐 dispatch 重复探针
`OCUDU_CE_{LSE,CFO,SMOOTH,SIGMA2,POWER,EPRE}_REPEAT`；③ **`mmse_pilots_cfo` 并行化**；
④ **`sigma2`/`epre` 改 SIMD 组内归约**。K1 的两次尝试**量输并已回退**（`ocudu_mmse_inv.metal` 与 HEAD 一致）。

---

## 2. ★ 开工第一步（建议）

| 候选 | 依据 | 第一步 |
|---|---|---|
| **① ★★ 跑一条空口腿，验抽取并行化** | §5.8.20/21 落了两个改动（CFO 并行化、`sigma2`/`epre` 的 SIMD 归约），**离线门全绿、`_llr.bin` 全 46 捕获 0 字节**，但**都还没上过星**。新门之下**每一个利用新自由度的改动都必须上空口腿**（离线不再能承诺"同样的位"）| `touch build/hashes.h && cmake --build build --target gnb`（戳记 == HEAD）→ `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s14-extract-parallel`。**先关手机 WiFi**；跑完 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`，四条标准 + `ch_wt` |
| **② 重排 116.9 µs（其中 TA 链 80.1）** | §5.8.17。**TA 链 = 一个线程组顺序跑 `nof_slices` 个 slice**（1 层 3 符号 = 3 个串行），每个 slice 的变换走 `ocudu_dft_butterflies`，**`log2(size)` 级各带 barrier**（同 K1 那种病）| **⚠ 爆炸半径**：`ocudu_dft_butterflies.h` 是**共享 helper**（`ocudu_dft_metal_engine` 也用）⇒ 改它=动多模式共用的 DFT。**先隔离测量**（把 TA 链单独编一条命令缓冲，或先量 `log2(size)` 级数与耗时的关系），**再决定**是否改共享 helper |
| **③ DFT（空中 ~40%）** | §5.8.12 的空中分段说 DFT ~40%，**从没离线量过**；它和 ② 共用蝶形 → **两者应一起规划** | 先量 DFT 的分段（协议见 §7）|
| **④ 裁定：K1（197.5 µs，31%）** | §5.8.16 ⑦ 把微调走死了；**新门已经打开**，所以现在只剩"换算法值不值"的估收益问题（Newton–Schulz 之类：高度并行、barrier 极少）| 离线估：改一版算法，量 K1 降到多少、`_llr.bin` 动多少，再决定值不值得一条腿 |
| **⑤ 修 `OCUDU_CE_INVERT_FIRST`** | 它现在产出 **NaN**（§4.2）；修好后 K1 能单独占一条命令缓冲 | 把 `engine_run()` 里那次 `engine->invert(gpu_a, …)` 移到相关矩阵前缀**之后** |
| **⑥ 抽取剩下的 `fd_smooth`（边际 27.5）** | per-symbol、3 个线程组 × 128；**但它的"工作"很小（每线程 ~36 次 MAC）却报 27.5 ⇒ 边际数字本身可疑**（§5.8.20 的口径）| **不要凭边际数字动它**；先用整段臂或比值确认它真有那么多 |

**⚠ 无论走哪条：先读 §4（失败/禁用项），别重复它们。**
**⚠ 离线量这套东西用 `corpus/syn027_25`（25 PRB = 空口几何）**；**跑 A/B 之前先跑空臂**（§5.8.20 ④）。
**⚠ K1 的微调不要再试了**：几何、barrier、BLK、并行度四条都已被量掉（§5.8.16）。
**⚠ 报一个旋钮臂的数之前，先在代码里确认那个旋钮名字存在**（§5.8.17 ④：`OCUDU_CE_DEV_INVERT` 根本不存在）。
**⚠ 重复探针给的是"边际成本（上界）"，不是"份额"**——每个分段数都要用整段臂（`CPU_LS=1`、`REFORMAT_REPEAT=2`）或比值交叉验证（§5.8.20 ③）。

---

## 3. 本会话做成了什么

| 事项 | 状态 | 证据 |
|---|---|---|
| **计数器仪器：这台设备上不可能** | ✅ **定案（永久关闭这条路）** | `sampleCountersInBuffer:` 在 compute 编码器上**断言 + SIGABRT**（exit 134）；`supportsCounterSampling` 只有 `AtStageBoundary=YES`，`AtDrawBoundary/AtBlitBoundary/AtDispatchBoundary` **全 no**（`wip/metal_counter_caps.mm`，Apple M4 Pro）|
| **"延迟提交在哪儿被等"** | ✅ **实测有答案** | `OCUDU_CE_WAIT_TRACE=1`：`event`（默认）→ `end_stage_async` 发布 → **`wait_pending_impl`** 收集（由 `complete_fd_td_estimation_stage` 调用）；`host_wait` → `collect_async_stage`；`burst`/`merged` → **什么都不发布**。**默认路线上上一会话挂的钩子本来就是对的** |
| **离线 `busy split` 的可用协议** | ✅ **有协议** | `--repeat 1`、一个进程一跳、N≥9 取中位数 ⇒ **±0.7%**；`--repeat 20` 是**双峰**（12 次里 3 次低 45%）⇒ 不可用。上一会话的撤回**对**，但原因要更正 |
| **★ `ch_wt`（≈554 µs）的分段成本** | ✅ **第一次量出来** | K1 求逆 **198.3 µs（35.8%）** > 重排 96.7（17.4%，其中 TA 链 56.5）> K2 59.8（10.8%）> 相关前缀 36.6（6.6%）；已量出合计 **70.6%**，未归属 163.0（29.4%）|
| **★ K1 画像（§5.8.16）** | ✅ **微调路线全部量掉；两次改动量输并回退；结论是"要么改数值契约，要么换靶子"** | ① K1 = **197 µs/跳**，**与分配宽度无关**（4 / 10 / 25 PRB 都是 197.3 / 198.3 / 197.5）；② 几何默认 **(64,16) 已最优**（最好的替代 128x8 只值 **1.1%**；几何逐位保值：7 种 × 4 dump = 0 字节）；③ **不是 barrier**：2→1 barrier/pivot 反而更慢（197→216），K1b barrier 少 9 倍却贵 **2.1×**（422 vs 197）；④ **并行度免费**：隔离台架 `k1_check` 里 `nof_systems` 1→8 成本**一动不动**（203→203），且它给 n=54 是 **203 µs**、与真跳里重复探针的 **197.3 µs 差 3%**（两套仪器互证）；⑤ ⇒ 剩下的是 **逐 pivot 的串行临界路径（≈3.7 µs/pivot）**，在"逐字节不变"下砍不动 |
| **★ 门变更（§5.8.19）** | ✅ **已落地** | `wip/value_net.py`：47 捕获 0 问题、`--self-test` **8/8**（注入 P0/P5/20% 漂移/高置信翻转必须红）。逐字节网降级为**信息** |
| **★ 抽取并行化（§5.8.20/21）** | ✅ 离线全绿，**待上星** | CFO 单线程→线程组归约；`sigma2`/`epre` 的 256 项树→**SIMD 组内归约**（10 道 barrier → 2）。**抽取 171.3 → ~118 µs（−31%）**；全 46 捕获 **`_llr.bin` 0 字节**、`_h.bin` 28 字节 |
| **★ 双峰根因（§5.8.20）** | ✅ **查清** | 空臂 base-vs-base 宽度 **±250 µs**；`Chrome GPU 进程 37–44% CPU + WindowServer 32%` ⇒ 宿主 GUI 分时占 GPU |
| **门** | ✅ | `value_net.py` 47/47 + self-test 8/8；`ctest -R metal` **9/9** |

---

## 4. ★★ 四个失败/禁用项（**别重复**）

### 4.1 逐 dispatch 计数器在这台设备上**不可能** —— 别再试

上一会话的"设备支持（supported=1）、采样缓冲建得起来、但没打印"是**两个独立问题只问了一个**：
`supported=1` 问的是 `[device counterSets]` 里有没有 `timestamp`，而 `sampleCountersInBuffer:`
要求的是**调用点所在的采样点**被支持。把解析改挂到命令缓冲**自己的完成回调**上（因而与"谁等它"无关）
之后，拿到的是断言 + **进程被打死**：

```
failed assertion `MTLComputeCommandEncoder:sampleCountersInBuffer:...not supported on this device'   (SIGABRT)
```

⇒ **设计文档 §5.8.14 结尾"换仪器用逐 dispatch 时间戳"那条建议作废。** 能用的是它的后半句
（隔离测量），而 `busy split` 就是那条路（`GPUStartTime/GPUEndTime`，不需要采样缓冲）。

### 4.2 `OCUDU_CE_INVERT_FIRST=1` 现在产出 **NaN**（新发现的缺陷）

它把 K1 放进独立命令缓冲，读数很漂亮（`ch_wt` 554.4 → 246.4，多出 `ch_est=305.6`），
**但 `noise_variance=nan rsrp=nan`、`_h.bin` 差 2688 字节**。原因：`engine_run()` 里那次
`engine->invert(gpu_a, …)` 跑在**相关矩阵前缀把 A 写进 `gpu_a` 之前**（前缀是 P1 时代搬进
`encode_run()` 的），而 `k1_inline` 又被这个旋钮关掉 ⇒ 权重拿到 NaN。
**⇒ 修好之前不要用它做 A/B；198 µs 这个数因此只有重复探针一个来源。**

### 4.3 `OCUDU_CE_INV_REPEAT` **只在奇数下保值**（§7 的表要按这条读）

K1 是**原地**求逆，每多编一次在 `A`/`A⁻¹` 之间翻面：

| `INV_REPEAT` | `_h.bin`/`_llr.bin` | `_ce.txt` |
|---|---|---|
| **2 / 4（偶）** | **2688 / 914 字节不同，数值全错** | 34 / 32 字节（`noise_variance` 1.258e-01→**4.601e+03**）|
| 3 / 5（奇）| **0 / 0**（数据逐位相同）| 7 / 8 字节（两个标量末位 ~1e-7）|

⇒ 上一会话"n=4 逐位不变"的验证**落在错的那一侧，等于没验**；
**任何用偶数 `INV_REPEAT` 读到的数都无效**。`CORR_REPEAT` / `_W_REPEAT` / `_REFORMAT_REPEAT`
三支的保值性**复验通过**（0 字节）。

### 4.4 离线回放**不走延迟路线** ⇒ 它答不了"谁等提交"这类问题

`ul_chain_replay` 用 `compute()`（`deferred=false`），trace 全程只有 `end_stage`。
走延迟路线的是 `submit()`/`finish()`，即
`port_channel_estimator_metal_mmse_unit_test` 的 Test 13。**问路由问题要挑对工具。**

---

## 5. 三条纪律（本会话新增/修订）

1. **一个量"稳不稳"要连着【读它的那个配置】说。** `busy split` 在 `--repeat 1` 下 ±0.7%，
   在 `--repeat 20` 下双峰。**撤回一个量之前，先找一个让它可重复的协议**；找不到再撤。
   （§5.8.14 撤得对，但把"协议"和"量"一起撤了，于是把 §4.3 那个缺陷一起埋了一整个会话。）
2. **保值探针必须在【实际使用的那个 N】上验。** `_INV_` 的保值性按奇偶分叉，在 n=4 上验等于没验。
3. **"有计数器集合"≠"能在某处采样"，而且这类断言是 SIGABRT 不是静默。**
   问设备能力要问到**采样点**；判读任何腿之前先看**进程退出码**。

---

## 6. 未解 / 开放项

1. **"K1 那 197 µs 到底是什么"没有答案**：已排除 barrier、算术量、几何（§5.8.16 ③②）；剩下
   "每 system 的固定开销"或"逐元素线程组访存"两种。**固定 n 扫 `nof_systems`** 是最省的下一步。
2. **K1 的结构性改法没做**：micro-tuning 已经量到没有余量（几何 1.1%、barrier 负收益）⇒
   只能是"一个 system 多个线程组"或换算法。
3. **`OCUDU_CE_INVERT_FIRST` 是 NaN 旋钮**——见 §4.2，同时是候选 ③。
4. **`_INV_` 探针需要干净形态**：让重复**恢复原状**（把 A 存一份，或 K1 编两次算一次重复）。
5. **163 µs 未归属**（`ch_wt` 的 26%，按空口几何）：y scatter、barrier、编码/发射开销——第二大桶。
6. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**（P0-⑦ 旁证）——仍未查。
7. **`shared_queue::wrap_no_copy()` 也会发布非零偏移，而引擎的 `wrap_shared()` 丢掉它**
   （静默错址；9 个调用点）——修法同 `wrap()`，单开一次改动。
8. **P3 的延迟代价没有干净口径**（+1650 µs 被负载混淆）⇒ 要**同负载口径的 A/B**。
9. **P3 与 GPU 工作的关系**：把每跳 GPU 工作砍下去，1.00 才可负担 —— K1 现在是最大的那一块，
   而它**与分配宽度无关**（每跳固定 197 µs）。
10. **§5.6.9 只剩一条真开放项**：1 PRB 的样本量（要新数据）。
11. **§18.7 的 TA fence 实验**仍未做；本会话给了它一个数：**TA 链 = 56.5 µs = `ch_wt` 的 10.2%**。
12. **空中 `ch_wt` 的跨腿稳定性被推翻**：上午 383 µs、下午 564 µs，**kernel 完全相同**
   （契约逐项相同）⇒ 它随腿变；**离线协议不受影响**（§5.8.15 ③）。

---

## 7. 工具、旋钮与仪表（本会话新增/变更）

| 名字 | 说明 |
|---|---|
| **`OCUDU_CE_WAIT_TRACE=1`** | **新**：引擎 11 个 `waitUntilCompleted` + 发布点（`end_stage_async`）+ 完成点全部打点，`[ce_wait] … cb=0x…`；关掉时只多一次缓存的 `getenv`。**问"谁等提交"用它**（要挑走延迟路线的工具，见 §4.4）|
| **`wip/metal_counter_caps.mm`** | **新**：`supportsCounterSampling` 四点 + counter set + 采样缓冲能否建（机制测量，同 `metal_alias_order.mm` 的地位）|
| **`wip/S14_instruments.md`** | **新**：本会话全部原始数据、命令、复跑清单 |
| **`busy split` 的读法** | **新协议**：`--repeat 1`、一个进程一跳、**N≥9 取中位数**。**不要用 `--repeat 20`**；中位数挡掉 ~1/9 次的向上离群 |
| `OCUDU_CE_CORR_REPEAT` / `_W_REPEAT` / `_REFORMAT_REPEAT` | 保值重复探针，**复验通过**（0 字节）；在 §7 的协议下取斜率 |
| **`OCUDU_CE_INV_REPEAT`** | ⚠ **只在 ≥3 的奇数下数据可用**（§4.3）|
| **`OCUDU_CE_INVERT_FIRST`** | ⚠ **NaN，禁用**（§4.2）|
| `OCUDU_CE_EDGE_FUSE` / `_CE_HOLD_EXTRACTION` / `_CE_LANE_ORDER` / `_CE_DEV_TA` / `_UL_PHASE_SEGMENTS` | 同上一会话，未变 |
| `OCUDU_METAL_GPU_TIME` | 1 = 每队列 GPU busy/window（**扰动被测对象**，只作诊断）|

---

## 8. 本会话的腿

**没有。** 全部离线（没有 OTA）——本会话只改了一个关掉即零行为的 trace。

---

## 9. 环境与流程（每会话都适用）

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  **改 `.metal` 后**：`rm -f lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib` →
  **全量** `cmake --build build -j 10` → 再 `--target` 显式重建要判读的产物。
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> [OCUDU_*=…]`
  —— **sudo 由用户执行**（把命令以纯文本贴给用户）；**Ctrl+C 停**（SIGTERM 丢统计）；
  判读 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`。
* **★ 上腿之前先关掉手机的 WiFi**（2026-09-20 定案）：**手机开 WiFi 时会自己拆掉蜂窝的 PDU 会话**
  （表现为"连上就 release"，日志里是 `Rx PDUsessionResourceReleaseCommand`，它前面 4.4 ms 有一条
  手机发出的上行 NAS）⇒ 会白跑一条腿。完整判读方法见 `wip/S14_phone_drops.md`。
* **★ 判读一条腿之前先看 `Real-time failures`**：磁盘上所有腿的规律是 **RTF=0 ⇒ CRC ≥ 85%**、
  **RTF ≥ 200 ⇒ CRC ≈ 30%**。RTF 不为 0 的腿**不算腿**（判据第 3 条就是"RF failure 0"），
  先查主机负载/USB，别拿它做 A/B。
* **用户裁定（长期有效）**：① `mode=gpu` 不允许宿主兜底（设备覆盖不到 ⇒ 该 PUSCH 失败 + 一行 ERROR）；
  ② dump 不算 CPU in the loop（debug 接触单独打印、不进判据）；③ **控制面融合分阶段做、每阶段一次 OTA**
  （腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）。
* **排序标准是"功能正确前提下 CPU 还剩几个参与点"**，不是收益。
* **离线测量的纪律**（本会话的教训）：任何"这不值 X µs"的 A/B **必须先按 §7 的协议自证可重复**，
  并用 `wip/neutral_vs_baseline.sh` **先于**单测确认没有动发布位。
