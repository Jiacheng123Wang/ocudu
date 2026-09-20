# 交接（入口） — S14：**离线 GPU 时间仪器定案**——计数器永久关闭、等待点实测、`busy split` 有协议且**第一次量出 `ch_wt` 的分段**

> **本文件是新会话的唯一入口**：读完它就能开工。上一份 `session_handoff_2026-09-20-4.md` 是上一会话的
> 过程记录（**不必读**）；技术细节都在常驻设计文档
> `gpu_phy_pipeline_design_and_implementation.md`（本会话新增 **§5.8.15**，并更正了 §5.8.14 的结尾
> 与 §5.8.14 开头那句"每一段都保值"）。过程证据与复跑命令：
> `wip/S14_instruments.md`；机制测量：`wip/metal_counter_caps.mm`。

---

## 1. 一句话状态

**本会话的代码提交 = `b47f8ccad7`**（`phy: a wait trace for the estimator's submissions, and the route it
names`）；本备忘录以及 S14 的设计文档/证据都在它之后的 docs 提交里。
**工作树 HEAD 以 `git log -1 --oneline` 为准**（就是本备忘录所在的那个提交），
**`build/apps/gnb/gnb` 已按它重戳**——自查：`grep build_info build/hashes.h` 的短哈希 == HEAD。
**⚠ 之后再有任何提交，戳记就落后了，上腿前按 §9 重戳**（坑 35）。
工作树只剩用户自己的 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`，**别动它**。
门：27 语料 + 20 窄捕获 **235/235 逐字节**、`ctest -R metal` **9/9**（重链之后复跑过）。
tag 未变：**`gpu_lane_commit_p1`**、**`gpu_lane_commit_p2`**。

**本会话全部离线**（没有 OTA；腿仍由用户跑，日志在 `doc_chinese/phy_pipeline_gpu/wip/logs/`）。

**新增的唯一代码**：`OCUDU_CE_WAIT_TRACE=1`（引擎 11 个等待点 + 发布点 + 完成点的打点，关掉时零行为）。

---

## 2. ★ 开工第一步（建议，纯离线，不需要 sudo）

| 候选 | 依据 | 第一步 |
|---|---|---|
| **① 修 `OCUDU_CE_INVERT_FIRST`，让它给出 K1 的干净隔离测量** | 它现在产出 **NaN**（见 §4.2），而它正是量 K1 的现成仪器：修好后 `ch_wt` 与 `ch_est` 分开，K1 单独一条命令缓冲 | 把 `engine_run()` 里那次 `engine->invert(gpu_a, …)` 移到**相关矩阵前缀之后**（前缀在 `encode_run()` 里建 A，见 §4.2）；判据：`_h.bin`/`_llr.bin`/`_ce.txt` 与默认臂**逐字节相同**，且 `busy split` 里出现一条独立的求逆项 |
| **② 削 K1（**`ch_wt` 的最大单项，198 µs / 36%**）** | 这是本会话第一次量出来的靶子（§3），而且树里有两条现成支路：`OCUDU_INV_TGX/TGY` 几何 sweep（S-5a 在**单个** 36×36 上量过 24.5–91.3 µs、3.7× 散布，而那个好几何未必是这个批量的好几何）与 `OCUDU_INV_RL`（K1b 右看，**数值错**，需要一个修 | 先做 ①（否则量不准），再用 **§7 的协议**跑几何 sweep；算法层面（Gauss-Jordan → 用 Hermitian/Cholesky 结构）先算出理论收益再动手 |
| **③ 查那 163 µs 未归属（29%）** | 第二大桶，且是**纯开销**（y scatter、barrier、编码/发射）| 用同一协议逐项关掉/重复（`OCUDU_CE_DEV_Y=0`、`EDGE_FUSE=0`、去掉 scatter），先看它是不是"每个 dispatch 的固定成本 × dispatch 数" |
| **④ DFT（空中 ~40%）** | §5.8.12 的**空中**分段说 DFT ~40%，而它**从来没被离线量过** | 现在有协议了（§7），先量 DFT 的分段再谈优化；它属 `ocudu_dft_metal_engine`（多模式共用）⇒ 改动面更大 |

**⚠ 无论走哪条：先读 §4（本会话的四个失败/禁用项），别重复它们。**

---

## 3. 本会话做成了什么

| 事项 | 状态 | 证据 |
|---|---|---|
| **计数器仪器：这台设备上不可能** | ✅ **定案（永久关闭这条路）** | `sampleCountersInBuffer:` 在 compute 编码器上**断言 + SIGABRT**（exit 134）；`supportsCounterSampling` 只有 `AtStageBoundary=YES`，`AtDrawBoundary/AtBlitBoundary/AtDispatchBoundary` **全 no**（`wip/metal_counter_caps.mm`，Apple M4 Pro）|
| **"延迟提交在哪儿被等"** | ✅ **实测有答案** | `OCUDU_CE_WAIT_TRACE=1`：`event`（默认）→ `end_stage_async` 发布 → **`wait_pending_impl`** 收集（由 `complete_fd_td_estimation_stage` 调用）；`host_wait` → `collect_async_stage`；`burst`/`merged` → **什么都不发布**。**默认路线上上一会话挂的钩子本来就是对的** |
| **离线 `busy split` 的可用协议** | ✅ **有协议** | `--repeat 1`、一个进程一跳、N≥9 取中位数 ⇒ **±0.7%**；`--repeat 20` 是**双峰**（12 次里 3 次低 45%）⇒ 不可用。上一会话的撤回**对**，但原因要更正 |
| **★ `ch_wt`（≈554 µs）的分段成本** | ✅ **第一次量出来** | K1 求逆 **198.3 µs（35.8%）** > 重排 96.7（17.4%，其中 TA 链 56.5）> K2 59.8（10.8%）> 相关前缀 36.6（6.6%）；已量出合计 **70.6%**，未归属 163.0（29.4%）|
| **门** | ✅ | `neutral_vs_baseline.sh` **235/235、0 字节**；`ctest -R metal` **9/9**（重链后复跑）|

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

1. **K1 求逆没动过**（198 µs，`ch_wt` 的 36%）——见 §2 候选 ②。
2. **`OCUDU_CE_INVERT_FIRST` 是 NaN 旋钮**——见 §4.2，同时是候选 ①。
3. **`_INV_` 探针需要干净形态**：让重复**恢复原状**（把 A 存一份，或 K1 编两次算一次重复）。
4. **163 µs 未归属**（`ch_wt` 的 29%）：y scatter、barrier、编码/发射开销——第二大桶。
5. **离线 `ch_wt` 554 µs vs 空中 383–394 µs**：捕获几何不同，**口径没对齐过**（新开放项）。
6. **`shared_queue::wrap_no_copy()` 也会发布非零偏移，而引擎的 `wrap_shared()` 丢掉它**
   （静默错址；9 个调用点）——修法同 `wrap()`，单开一次改动。
7. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**（P0-⑦ 旁证）——仍未查。
8. **P3 的延迟代价没有干净口径**（+1650 µs 被负载混淆）⇒ 要**同负载口径的 A/B**。
9. **P3 与 GPU 工作的关系**：把每跳 GPU 工作砍下去，1.00 才可负担 —— K1 现在是最大的那一块。
10. **§5.6.9 只剩一条真开放项**：1 PRB 的样本量（要新数据）。
11. **§18.7 的 TA fence 实验**仍未做；本会话给了它一个数：**TA 链 = 56.5 µs = `ch_wt` 的 10.2%**。

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
