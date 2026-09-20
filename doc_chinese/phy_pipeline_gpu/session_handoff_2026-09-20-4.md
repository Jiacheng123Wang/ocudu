# 交接（入口） — S13：**P0 结案 + P1/P2 已上星（各有 tag）+ P3 已量清 + 三个测量失败已回退**

> ⚠ **本文件已被 `session_handoff_2026-09-20-5.md` 取代（那份是入口）**；本文件保留为上一会话的**过程记录**。
> **两处已被更正**：① §2 候选① 说的"把计数器钩子挂对位置"——计数器在这台设备上**根本不可用**
> （设计文档 §5.8.15 ①），而默认路线的钩子位置本来就是对的；② §4.1 对 `busy split` 的**整块撤回过宽**
> ——它有一条可用协议（`--repeat 1`、N≥9 取中位数，±0.7%），而且正是用它第一次量出了 `ch_wt` 的分段。
>
> **本文件是新会话的唯一入口**：读完它就能开工。上一份 `session_handoff_2026-09-20-3.md` 是同一会话的
> 过程记录（含逐步演化），**不必读**；技术细节都在常驻设计文档
> `gpu_phy_pipeline_design_and_implementation.md`（本会话新增 §5.8.5 P0-(c)、§5.8.6/§5.8.7/§5.8.8（P1/P2）、
> §5.8.9/§5.8.10（P3）、§5.8.11/§5.8.12（链条分段）、§5.8.13（P5 失败）、§5.8.14（仪器撤回））。

---

## 1. 一句话状态

**HEAD = `4f072f4939`**（已推送；工作树只剩用户自己的 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`
（`all_level: debug→info`，**别动它**））。**二进制已按 HEAD 重戳**。门：27 语料 + 20 窄捕获 **235/235 逐字节**、
`ctest -R metal` **9/9**。tag：**`gpu_lane_commit_p1`**、**`gpu_lane_commit_p2`**。

**本会话全部离线**（腿由用户跑，日志在 `doc_chinese/phy_pipeline_gpu/wip/logs/`，root 所属但可读）。

---

## 2. ★ 开工第一步（建议，纯离线，不需要 sudo）

**候选线（按证据强度）**：

| 候选 | 依据 | 第一步 |
|---|---|---|
| **① 把"延迟提交在哪儿被等"查清，再把 §5.8.14 的计数器仪器挂对位置** | 计数器仪器**已经写到能建缓冲、能编采样点**，只是解析钩子挂错了地方（见 §4.3）⇒ **离成功最近** | 读 `encode_run` → `end_stage_async` → **`complete_fd_td_estimation_stage()`**（`pending_fused_burst` / `has_pending()` / `wait_pending()`）这条链，找出 `run_async` 那次提交**实际**被谁 `waitUntilCompleted`；然后把钩子挂到那一处 |
| ② 用**空口腿**给 GPU 优化做 A/B | 空中 `ch_wt` 四条腿 383–394 µs（±2%）**稳定**；离线那个量不稳（§4.1） | 先想清楚"要判的效果 ≥ 多少"（腿间噪声 ~±5%），再定候选（削哪一段/哪个 kernel） |
| ③ 削 DFT（~420 µs/slot，占接收后 ~40%） | §5.8.12 的**空中**分段（DFT ~40% / 权重链 ~40% / 均衡+回读） | 它属 `ocudu_dft_metal_engine`（多模式共用）⇒ 改动面更大，先量再动 |

**⚠ 无论走哪条：先读 §4（三个失败），别重复它们。**

---

## 3. 本会话做成了什么

| 阶段 | 状态 | 证据 |
|---|---|---|
| **P0**（融合 NaN 的病因）| ✅ **结案并修掉**（`8890d283f6`）| **Metal 的访存顺序按 `MTLBuffer` 对象、不按地址**：同一段内存两个对象 ⇒ `memoryBarrierWithScope` 排不了（隔离测量 `wip/metal_alias_order.mm`：同对象 200/200 PASS、别名对 200/200 FAIL）。修法：`wrap()` 返回 `mapped{buf,offset}`，**一段内存一个对象**。判据：235/235 逐字节不变；融合 == 独立（27 语料 + 20 窄捕获四个 dump 逐字节相同）|
| **P1**（尾组那次提交）| ✅ **离线+空中全过**，tag `gpu_lane_commit_p1`（`ba4cd9f781`）| 空中受控 A/B：`cbs/lane` **3.71 → 3.00**；契约 8/8、0 穿越、RF 0、CRC 98.24% |
| **P2**（权重那次提交）| ✅ **离线+空中全过**，tag `gpu_lane_commit_p2`（`3df49a6d3b`）| `cbs/lane` **3.00 → 2.00**、`mmse_ce commits`/跳 **2.000 → 1.000**、`extraction commit → weights commit` 系列**为空**；P2 腿是本线延迟最低（`[ul_gpu_pipeline]` **2103.5/1993.1 µs**）|
| **P3**（整跳一次提交）| ✅ **实现并量清**（`2926c684ab`），**默认未翻**（用户裁定）| `OCUDU_CE_LANE_ORDER=merged`：**1.0002 次提交/接收、`cbs/lane` 1.00（max 1）、lane gap 0.0 µs**，四条标准全过；代价是延迟（中位 2016 → 3666 µs，**该差被负载混淆**，见 §5.8.10 的自我更正）|
| **链条分段仪器** | ✅ 可用（`OCUDU_UL_PHASE_SEGMENTS=1`）| 融合车道里也能打三段分解（和 = `[ul_gpu_pipeline]`，按构造）；单测 `ul_pipeline_probe_test` 钉住"追加而非替换" |
| **两条正面结论（空中，稳）** | ✅ | ① 接收后那 ~1 ms = **DFT ~40% + 权重链 ~40% + 均衡/回读 ~15%**；② **宿主不是瓶颈**（`defer_wait` 665–887 µs vs 宿主 16 µs 的活；前端栅栏开着**无效**）|

---

## 4. ★★ 三个失败（**别重复**）

### 4.1 离线 `busy split` **不能用来判优化**（已撤回整张 bisection 表）

同一配置（默认）连跑 6 次，`ch_wt` = **571 / 554 / 203 / 606 / 625 / 142** µs（4× 散布）；`--repeat 20` 仍
**579 / 476 / 608**（±12%）。要判的效果是 10–20% ⇒ **这个量不可用**。
原因：离线回放不按 slot 节拍喂数据，相邻跳在 GPU 上重叠，"某命令缓冲的 GPU 窗口"里混了排队。
**⇒ 因此撤回**：§5.8.12 的离线 bisection（`CORR_DEV=0` 的 −445 µs、K4 −66、TA −125、求逆 −153 **全是噪声**）
以及"**相关矩阵前缀是 `ch_wt` 里最大单项**"这个结论。
**仍然成立**：§5.8.12 的**空中**分段、空中 `ch_wt`（383–394 µs，±2%）、"宿主不是瓶颈"。

### 4.2 P5（相关矩阵"预算表"优化）**数值错，已整块回退**

把几十个不同的 rt/rf 预算成四张小表（648 float，本应逐位相同）⇒ `noise_variance` 从 `1.258e-01` 变成
**`3.596e+04`**、27 条语料全不同（262,007 字节）⇒ `git checkout` 回退（回退后 235/235）。
**⚠ 顺带作废**："`ch_wt` 620.7 → 597.0 µs" 那个读数（输入是错的）。
**⇒ "那 445 µs 花在哪"至今没有证据**；"每元素两个除法"只是**未验证的假设**。

### 4.3 逐 dispatch 计数器仪器：写到一半，**没验通，已回退**

`OCUDU_METAL_DISPATCH_TIME=1`（`MTLCounterSampleBuffer`，`#import <Metal/MTLCounters.h>`，
`sampleCountersInBuffer:atSampleIndex:withBarrier:YES`，采样点在 `encode_run` 的
scatter/corr_std/corr_edge/inv/weights/apply/reformat）。
**实测**：设备支持（`supported=1`）、采样缓冲**建得起来**（`begin: enabled=1 supported=1 buf=0x…`），
但 **`[disp_time]` 一行都没打** ⇒ 解析钩子（挂在 `end_stage` / `collect_async_stage` / `wait_pending_impl`）
**在这条延迟路径上没被走到**（日志显示只有构造期的同步 `run` 命中）。
**⇒ 下次第一步**：查清 `run_async` 那次提交**到底在哪儿被 `waitUntilCompleted`**
（`encode_run` → `end_stage_async` → `complete_fd_td_estimation_stage()` 的
`pending_fused_burst ? complete_fused_burst() : wait_pending()`），再把钩子挂到那一处。

---

## 5. 三条纪律（本会话用代价换来的）

1. **判据要先自证可重复**：拿一个量做 A/B 前，**同一配置连跑 N 次**看散布；散布 ≥ 要判的效果 ⇒ 这个量还不能用。
   （离线 `busy split` 就是这样被否掉的；空中腿的量是稳的。）
2. **改 kernel 先跑 `wip/neutral_vs_baseline.sh`**：它抓到了 §4.2 的错值，而同一棵树上 `ctest -R metal` **9/9 全过**
   —— 单测的 device-vs-host 对比走另一条路且按容差比 ⇒ **承重的是 dump 网**。
3. **判读门前先确认产物是刚构建的**（坑 35 本会话又踩一次）：`git checkout` 回退 + 删 metallib + 全量构建之后，
   构建**没有重链 `ul_chain_replay`**，于是拿旧二进制跑门，得到"回退了还是全红"的假象。
   删 `.metal`/改被广泛包含的头之后：**删 metallib + 全量构建 + 显式 `--target`**，并核对 mtime。

---

## 6. 未解 / 开放项

1. **§4.3 的钩子位置**（下一步的入口，见 §2 候选①）。
2. **`shared_queue::wrap_no_copy()` 也会发布非零偏移，而引擎的 `wrap_shared()` 丢掉它** —— 静默错址；
   27 语料与单测不触发（现在会打一行 ERROR）。修法同 `wrap()`（9 个调用点），**单开一次改动**。
3. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**（P0-⑦ 旁证）——仍未查。
4. **P3 的延迟代价没有干净口径**：+1650 µs 被负载混淆（两条腿 regime 不同）⇒ 要**同负载口径的 A/B**。
5. **P3 与 GPU 工作的关系**：P3 贵在"提交粒度在突发上行下排队更久"⇒ **把每跳 GPU 工作砍下去，1.00 才可负担**。
6. **§5.6.9 只剩一条真开放项**：1 PRB 的样本量（要新数据）。
7. **§18.7 的 TA fence 实验**（把 TA 移出关键路径，~38 µs）仍未做。

---

## 7. 工具、旋钮与仪表（本会话新增/变更）

| 名字 | 说明 |
|---|---|
| `wip/metal_alias_order.mm` | **机制测量**：同对象/别名对 × 一个 encoder/两个 encoder，各 200 次 |
| `wip/neutral_vs_baseline.sh` | **中性网**：当前构建 vs 归档基线（135 语料 + 100 窄捕获）；缺文件 = 退出 2 |
| `OCUDU_CE_EDGE_FUSE` | **默认 1**（P1 融合）；`=0` 回退独立形态 |
| `OCUDU_CE_HOLD_EXTRACTION` | **默认 1**（P2：一跳一次估计器提交）；`=0` 回到两次 |
| `OCUDU_CE_LANE_ORDER` | `event`（默认）/ `host_wait` / `burst` / **`merged`（P3：整跳一次提交，已验、未翻默认）** |
| `OCUDU_UL_PHASE_SEGMENTS` | **1 = 融合车道里也打三段分解**（`ul_time_frequency`/`ul_channel_estimation`/`ul_equalization_demod`，和 = `[ul_gpu_pipeline]`）|
| `OCUDU_CE_CORR_REPEAT` / `_INV_` / `_W_` / `_REFORMAT_REPEAT` | **保值重复探针**（同一 dispatch 编 N 次，dump 逐位不变，已实测）：取斜率 = 该段 GPU 成本。⚠ 但**离线那个读数量本身不稳**（§4.1）|
| `OCUDU_METAL_GPU_TIME` | 1 = 每队列 GPU busy/window（**扰动被测对象**，只作诊断）|
| `OCUDU_UL_FRONTEND_FENCE` | **默认关** ⇒ 宿主每槽仍等一次 DFT；本会话**开着量过：无效**（假设被证伪）|

---

## 8. 本会话的腿（§12.1 已登记）

| 腿 | commit | 头条 |
|---|---|---|
| `s13p1-rollback_0920_0816` | `21e0b11e39` | 对照：`cbs/lane` 3.71、`ch_est` 1.71、RTF 5 |
| `s13p1-fused_0920_0813` | `21e0b11e39` | ✅ P1：**3.00**、RF 0、CRC 98.24%、契约 8/8 |
| `s13p2-rollback_0920_0842` | `454ab9a752` | 对照：3.00 / 2.000；⚠ 瞬时扰动腿（RTF 17、CRC 59.5%，**CRC 不可用**）|
| `s13p2-onecommit_0920_0840` | `454ab9a752` | ✅ P2：**2.00 / 1.000**、RF 0、CRC 97.76%、2000 µs 级延迟 |
| `s13p3-twostage_0920_0928` | `6a7ec52866` | P3 对照：2.00、lane gap 86.1 µs、RF 0、CRC 82.16% |
| `s13p3-merged_0920_0926` | `6a7ec52866` | P3：**1.00（max 1）/ 1.0002 次提交**、gap **0**、RF 0、CRC 85.01%；延迟 3666 µs ⚠ |
| `s13p4-phases_0920_1013` | `caa62ad502` | ✅ 分段：t2f 1368.8 / ce 38.9 / eqdem 566.7（中位）|
| `s13p4-fence_0920_1015` | `caa62ad502` | ✅ 前端栅栏开着：`signals=26371 waits=31660`，**延迟没变** ⇒ 假设证伪 |

---

## 9. 环境与流程（每会话都适用）

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  **改 `.metal` 后**：`rm -f lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib` →
  **全量** `cmake --build build -j 10` → 再 `--target` 显式重建要判读的产物。
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> [OCUDU_*=…]`
  —— **sudo 由用户执行**（把命令以纯文本贴给用户）；**Ctrl+C 停**（SIGTERM 丢统计）；
  判读 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`。
* **用户裁定（长期有效）**：① `mode=gpu` 不允许宿主兜底（设备覆盖不到 ⇒ 该 PUSCH 失败 + 一行 ERROR）；
  ② dump 不算 CPU in the loop（debug 接触单独打印、不进判据）；③ **控制面融合分阶段做、每阶段一次 OTA**
  （腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）。
* **排序标准是"功能正确前提下 CPU 还剩几个参与点"**，不是收益。
