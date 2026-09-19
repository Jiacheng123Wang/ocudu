# Session handoff —— 2026-09-18（S-7g-19：Step 1′ 落地并三腿判读；K0-a 侦察完成、未落地）

> 换会话先读这一份，再读 `s2_full_chain_design.md` 的 **§48.146(a) 状态索引**。
> 本 session 的全部细节：**§48.192**（Step 1′：机制、三种序、两个"由构造排除"的陷阱、离线门、三腿判读、对"重叠债"的更正）、
> **§48.193**（K0-a 融合侦察：谁读了什么、空气路线判定、**提取不能进 lane burst**、施工图与逐层判据）。
> 上一份：`session_handoff_2026-09-18-1.md`（S-7g-16/17/18：CE 融合、前端 fence/进裁判、前端合批）。

---

## 0. 一句话现状

**Step 1′ 已完成并上机三腿判读：估计器的 deferred hop 回到自有命令缓冲、编完即提交，lane burst 用后端 `MTLSharedEvent` fence
精确等它 —— 宿主在 lane 中间不再有任何同步点，且 `event`（默认）是同环境三条腿里 lane 最紧的一条。**
下一步 **K0-a 融合**已侦察到"要动两件事"，其中一件（提取与权重共用一个命令缓冲）**必须按 §48.193(d) 的方式做，不能把 K0-a 塞进 lane burst**（会成环）。

---

## 1. 代码状态（回来第一件事：核对这一节）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `apple-silicon` / **`74ba4c5251`**（已 push，与 origin 同步）|
| 里程碑 tag | `gpu_phy_fused_lane_ready` → `690b086679`（未动）；`gpu_phy_iq2llr_zero_host_copy` → `3d026939eb`（冻结）。**本 session 未打新 tag**（见 §5.4）|
| 树里的二进制 | `build/apps/gnb/gnb`，stamp = `74ba4c5251`；核对 `strings build/apps/gnb/gnb \| grep -c 74ba4c5251` == **2** |
| 参考二进制 | `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref` **＋并排 4 个 `.metallib`** |
| 语料 | `doc_chinese/work_tmp/corpus/`（27 个合成捕获）|
| 上机日志 | `doc_chinese/work_tmp/logs/`（**每腿两份**：`*.log`(ocudulog) 与 `*.log.stderr`(计数/契约)）|
| Ubuntu | `jwang@192.168.100.131:~/work/ocudu`，HEAD = `74ba4c5251`（已构建，`ctest -L phy` **164/164**）|

**本 session 的提交链（最新在前）**

```
74ba4c5251  mmse stats: the host-wait lane order collected its command buffer twice
d02f338c9e  fused lane, step 1': the estimator commits its own command buffer early, and the lane is ordered after it by an event
4d66f44c70  （本 session 起点）front-end batching: default on
```

**未提交 / 不要提交**：`configs/*.yml`（用户本地环境改动）、`doc_chinese/`（gitignored）、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh`。

---

## 2. 本 session 做完的事

### 2.1 S-7g-19 / Step 1′（commit `d02f338c9e`）

**背景**：Step 1b（S-7g-16）把估计器的 dispatches 搬进 lane 的共享命令缓冲，去掉了两段之间那次宿主等待，
代价是估计器的 GPU 工作不再与"宿主编码均衡/解映射"重叠（当时记为 +125 µs 延时债）。

**做法**
- 估计器 deferred hop 回到**自有命令缓冲**：`collect_async_stage()` 在 `event` 序下**编完即返回、不等待**；
- lane burst 开 CB 时（`burst_ensure_open()`）编一条 `backend_stage_wait()`：`shared_queue` 新增**后端 stage fence**
  （`MTLSharedEvent` + 代际：`backend_stage_signal/wait/generation/nof_*`），与前端 fence 同构但**独立计数**；
- 信号在 `[enc endEncoding]` 之后、`[cb commit]` 之前编码（命令缓冲级 API），失败路径不编信号 ⇒ 不会留下"没人发的代际"；
- **目标精确**：等的是"本 hop 那条估计器 CB"，不是"后端最新一代"（后者是前端 fence 翻车的原因，§48.189(f)）。
  之所以"最新"恰好等于"本 hop"：接收链先估计后解调（`pusch_processor_impl.cpp:220` 在 `:447` 之前）。

**三种序（`OCUDU_CE_LANE_ORDER`，默认 `event`）**

| 序 | 谁的命令缓冲 | 谁等 | 说明 |
|---|---|---|---|
| **`event`（默认）** | 估计器自有 CB，编完即提交 | lane burst 用后端 fence 等它（GPU 侧）| 重叠 + 顺序都有，中间无宿主等待 |
| `wait`（逃生门）| 估计器自有 CB | **宿主**在 `collect_async_stage()` 等 | S-7g-16 之前的行为 |
| `burst` | lane 的共享 CB | 无人等（lane 的 commit 覆盖）| Step 1b，一次提交但无重叠 |

废弃别名 `OCUDU_CE_FUSED_BURST` 仍读（新名未设时 `1`→`burst`、`0`→`wait`）。

**两个"由构造排除"的陷阱（都写进了代码注释）**
1. `run()` = `run_async()` + `wait_pending()`，所以"引擎侧一个序标志"会让**同步入口也加入 burst** ⇒ `end_stage()` 不提交、
   `wait_pending()` 无物可等 ⇒ 内联路线每跳**全零**（Test 3 掉到 0.00 dB）。现在同步契约是**编码函数的参数**
   （`encode_run(..., wait_for_completion)`），与 `encode_weights_only()` 对称。
2. **"走了哪个入口" ≠ "这些 dispatch 能不能进 lane burst"**：内联路线也会走异步入口再自己收尾。
   只有适配器知道 hop 是否已完成 ⇒ 门放在适配器（`args.deferred`），非 deferred hop 无论旋钮怎么设都拿不到 burst 序。

### 2.2 统计修复（commit `74ba4c5251`）

`wait` 序暴露：`collect_async_stage()` 等完自有 CB 没清 `pending_cb` ⇒ 下一次 `wait_pending()` 重复等一次
（`[metal_stats] mmse_ce` 打出 `waits>commits`、`max_in_flight=2^64-1`）。修法：等完即清（提交已被取走）。
`event` 序不受影响（stage 里不取），所以它的 `waits==commits` 一直是对的。

---

## 3. 本 session 必须知道的硬约束与口径

1. **每 lane 一次同步是硬约束**（LLR 交给 CPU 的 LDPC）；lane 内还剩 **K0-a 那一次宿主同步** ⇒ 这是下一个里程碑的判据（§48.193）。
2. **宿主在 lane commit 之前要读的东西不许融合**（K0-a 的 `sigma2_rel` 喂 A 的对角、CFO 喂噪声 reformat、LSE 喂 provider）。
3. **同步点的价值 = 它与真实依赖的距离**（前端 fence 的教训）。
4. **接收块策略决定"分批 commit 是否免费"**（`whole slots` 默认；合批只合已到达的样本）。
5. **相位账不可跨路线比（本 session 新纪律 #16）**：`[ul_channel_estimation]` 与 `[ul_equalization_demod]` 的分界是
   `record_ce_end()`＝**估计器 stage 从宿主侧返回的那一刻**，而三条路线在那里做的事完全不同 ⇒
   **可比的是 `ce + eq` 之和**。同理 `defer_wait`/`gpu_wait` 逐路线语义不同（`gpu_wait` 是 CB 的 GPU 时长，不是宿主阻塞）。
6. **lane busy 在本设置下与流量无关**（9 条腿跨 2.2× 流量区间，busy 只在 492–515 µs 之间，因为每 hop 分配几何相同）
   ⇒ **lane `gap`/`residency` 的差异是机制差异**，可以跨腿比；`[ul_pipeline]`/PDU 是流量相关的，跨腿比要小心。
7. **跨腿比较必须配同环境对照腿**：本 session 用 event/burst/wait 三条腿同 session 连续跑才把"债"量准。
8. **`thread_local` 的析构早于 `atexit`**；**release 未 `endEncoding` 的 encoder 会 abort**（任何打开的 CB 都要有析构/停机路径）。
9. **契约里的分母要跟着策略改**（`transforms` vs `commits`）。
10. **停止顺序**：`ru_controller_sdr_impl::stop()` 先停 lower PHY 再停电台。
11. **`--log.filename` 不建父目录**；参考二进制必须带它自己那批 `.metallib`；`[metal_stats]`/`[ul_gpu_lane]`/`[mmse_time_sum]` 走 **stderr**。
12. **两条配置构建门必须串行**（它们和主 build 抢同一份源码树 metallib）。

---

## 4. 本 session 的腿（同 session、同二进制 `d02f338c9e`、同环境，只差 `OCUDU_CE_LANE_ORDER`）

| 量 | **event（默认）**`gnb_event_0918_0658` | burst `gnb_burstctrl_0918_0716` | wait `gnb_waitctrl_0918_0717` |
|---|---|---|---|
| hops（lanes）| 970 | 812 | 833 |
| 流量 B/pdu | 370.2 | **531.3** | **553.0** |
| `[ul_pipeline] mean` | **2516.1** | 2567.0 | 2675.1 |
| `[ul_channel_estimation]` | 275.8 | 245.8 | **684.8** |
| `[ul_equalization_demod]` | 612.3 | 662.3 | **312.4** |
| **ce + eq** | **888.1** | 908.1 | 997.2 |
| lane busy / gap / residency | 505.9 / **239.3** / **745.2** | 499.7 / 268.0 / 767.7 | 504.9 / **359.1** / **864.0** |
| `cpl_wait` / `gpu_wait` / `defer_wait` | 0.6 / 320.7 / 652.4 | 0.1 / 0.0 / 695.5 | 0.6 / 335.4 / **342.5** |
| `cbs/lane` | 3.00 | 2.00 | 3.00 |
| `lane fence signals/waits/skipped` | **970/970/0** | 0/0/812 | 0/0/833 |
| `burst channel_estimator=` | **0** | 812 | 0 |
| `mmse_ce commits`（每 hop）| 1942（=2/hop）| 814 | 1668 |
| crc OK/KO | 855/115（88.1%）| 733/79（90.3%）| 757/76（90.9%）|
| RF 失败 | **0/66024** | **0/60763** | **0/78297** |
| 契约 | 7/7 | 7/7 | 7/7 |

**判读（§48.192(g)）**
- **burst vs wait 是流量匹配的一对**（差 4%），四个判据同向：`ce+eq` **−89**、`[ul_pipeline]` **−108**、lane gap **−91**、residency **−96 µs**
  ⇒ 把 CE 收进 lane **相对"有排序的融合前路线"本来就赢 ~90–110 µs**。
- **event 比 burst 再省 ~22–29 µs**（lane residency/gap；busy 与流量无关 ⇒ 这是机制差异），
  与"burst 腿每 lane 9 条 dispatch（8 EQ + 1 demap）的宿主编码成本 ≈22 µs"**对得上**。
- ⇒ **`event` 保留为默认**（唯一"宿主不阻塞 + 顺序有保证"的路线）；`wait`/`burst` 是逃生门与对照。

**⚠ 对 §48.188(i) "+125 µs 重叠债"的更正**：那份债是拿 **S-7g-16 之前的 baseline 腿**量的，而那条路线**根本没有排序**
（`run_async()` 提交后不等、完成时才 `wait_pending()`；它的 lane gap 只有 115.4 µs，比"有排序的 wait 路线"的 359.1 还小 ——
正是"没付排序成本"的指纹）。有排序的融合前参照是 `OCUDU_CE_LANE_ORDER=wait`：
**融合方向相对它赢 ~90–110 µs，Step 1′ 再叠加 ~22–29 µs**。

更早的腿（跨 session，**不要跨腿比 `[ul_pipeline]`**）：baseline `gnb_baseline_0917_2245`、fused `gnb_fused_0917_2241`、
fence `gnb_fence_0917_2318`、slotwait `gnb_slotwait_0917_2330`、frontend `gnb_frontend_0917_2347`、
batch `gnb_batch_0918_0617`、default `gnb_default_0918_0627`（前端合批的存档腿，`dft transforms/commits=14.00`、`dft busy 422.8`、失败 0/122898）。

**本 session 跑过的门（全绿）**：`ctest -L phy` 163/163 · 序 A/B `ab_fused_lane.sh 27`（4 路线 × 27 捕获 × 3 序逐字节，基准=默认序）
· `run_ab_all.sh … 27`（strict/bounded/CPU 各 27/27）· 六门 `run_gates.sh` · Metal 自测 6/6 · `nostats`/`nometal` 构建 RC=0 · **Ubuntu 164/164**。

---

## 5. 下一步

### 5.1 **K0-a 融合（Step 3）—— 先读 §48.193，再动手**

**已侦察清楚的四件事（不要重新假设）**
1. 空气路线上 K0-a 在 hop 内被宿主读的**只有 `sigma2` + `pilots_power` 的比值**（喂相关矩阵 A 的对角加载）。
   LSE 只有 `consumes_pilots()` 为真时读（空气 provider 是固定常数型、为 false）；CFO 只喂噪声 reformat（`compensate_cfo_flag`）
   与上报值（可延到**完成时**读）。空气 `pusch_channel_estimator_cfo_compensation` **默认 false**。
2. `pusch_channel_estimator_mmse_block_prb = 3` ⇒ **51 PRB 的授权余数为 0 ⇒ 非 merged 路线 ⇒ 全部相关矩阵本来就由设备构建**
   （`std_slots_filled=true`、`host_builds_std=false`）。所以空气默认配置下可行性成立。
3. **⚠ 提取不能进 lane burst**：`event` 序下权重在自有 CB 里、且那条 CB 编的 fence 正是 lane burst 要等的信号，
   若 K0-a 在 burst 里 ⇒ **权重 CB 要排在 burst 前、burst 又要等权重 CB ⇒ 成环**。正确做法 = **提取与权重共用一个命令缓冲**
   （`hop_stage` 接手 + 段间 `memoryBarrierWithScope`；hop 仍只有一次提交、**不引入 fence ⇒ 没有"生产者失败 ⇒ 消费者永久挂住"**）。
4. **两个待查清的点（本轮就卡在这里）**：
   - 单测里 gate 打印 `geom=0`（`ls_geometry_of(args).ok` 为 false）：要弄清哪些几何不 ok、空气路线是否恒 ok，
     否则门会**静默不生效**；顺带核实 `args.compensate_cfo_flag` 与构造参数的关系（单测传 false 但后续 hop 打印为 1）。
   - **merged 路线（rem≠0）仍不行**：tail 的 A/R_hp 由**宿主** `build_correlation_matrices()` 构建（读 `stats.sigma2`）。
     要覆盖全部 hop 得把 merged tail 也搬到设备（`corr_stage` 的 `nof_systems`/`a_sys`/`r_sys` 已支持，是**再编一条 tail 描述符**），
     并用 `run_gates.sh` 的 **k0d 门**逐捕获确认（S-7f-5v 的坑：一个描述符给整批用同一几何 ⇒ 覆写 edge 块的槽）。

**施工图（§48.193(c)(d) 有完整清单，逐层判据见 §48.193(f)）**
1. **plumbing（默认关也行，可离线证明）**：`mmse_sigma2_params` 加 `nof_power_pilots`，`mmse_pilots_power` 多写
   `out[2] = out[0] / fmax(out[1]/nof_power_pilots, 1e-30F)`（与宿主**同样两次 float 运算** ⇒ 逐位相同）；
   `mmse_corr_params` 加 `sigma2_from_device` + `scalars[[buffer(2)]]`，A 核读 `scalars[2]`；`corr_stage::sigma2_dev`。
   判据：三张网 + 序 A/B **逐字节相同**。
2. **`hop_stage` 接手**（`build_pilots_lse(s, fuse)` + `take_hop_stage()` + `mmse_engine_impl::{hop_stage,hop_open}`，
   析构里关掉未接手的 encoder；`mmse_stats_extraction_handoff()` 计数 + `extraction_handoffs()` 桥）。
   判据：CE 单测 Test 13 全绿 **且** 新增 Test 14（融合 vs 不融合逐字节 + `k0a_fused` 计数断言）。
3. 查清 §5.1.4 第一点后翻默认开（`OCUDU_CE_K0A_FUSE`，逃生门 `=0`）。
   判据（一条上机腿）：`[metal_stats] mmse_ce … k0a_fused ≈ hops`、`[ul_channel_estimation]` **≈276 → ≈180 µs**
   （宿主不再等提取的 ~94 µs，`ch_est` 的 GPU 时间见 §48.192(g) 的 `busy split`）、`[ul_pipeline]`/契约/红线/失败率不动。
4. （可选，覆盖 rem≠0）merged tail 的设备构建：判据 = k0d 门 + 三条腿。

> 本轮把 1+2 写过一遍（回退前 CE 单测 13 全绿、Test 14 抓出 `geom=0`），代码**已回退**，只在 §48.193 留下设计与清单。

### 5.2 **Step 4**：lane 并发 `max_in_flight` 1→N（跨 lane 重叠；后端 lane `gap` 仍有 ~240 µs）

### 5.3 顺带
- 用新口径确认 S-7g-13（`OCUDU_UL_RX_SYMBOLS=4`）+ 干净环境复核失败率；
- K0-a 落地后再考虑里程碑 tag（见 5.4）。

### 5.4 tag 政策（§48.181）
政策要求里程碑是"**整条 IQ→LLR 链跑在一条设备侧 lane 上**"，而现在 lane 中间**还剩 K0-a 那一次宿主同步**
⇒ 本 session **没有打 tag**（不要为每一腿打 tag；每腿结论记在 §48.146(a) 与 §48.192/§48.193）。

---

## 6. 关键文件与路径

| 内容 | 路径 |
|---|---|
| 设计文档（唯一活文档）| `doc_chinese/full_gpu_chain/s2_full_chain_design.md`（**§48.146(a) 状态索引**；本 session：§48.192、§48.193）|
| 上机腿脚本 | `doc_chinese/full_gpu_chain/wip/run_air_leg.sh <label> [OCUDU_*=…]`（root、knob、日志名、stderr 双捕获；banner 已列现行旋钮）|
| 腿判据脚本 | `doc_chinese/full_gpu_chain/wip/air_leg_report.sh <log…>`（crc/失败率/契约/metal_stats/lane/dft/**lane fence**）|
| lane 序 A/B | `doc_chinese/full_gpu_chain/wip/ab_fused_lane.sh <N>`（**同一二进制 × 3 序 × 4 路线逐字节**，基准=默认序）|
| 每腿门 | `run_ab_all.sh <ref> 27` → `ab_fused_lane.sh 27` → `run_gates.sh` → `run_metal_engines.sh` → `/tmp/build_{nostats,nometal}`（**串行**）→ Ubuntu |
| 本 session 改动的代码 | `lib/phy/metal/ocudu_metal_queue.{h,mm}`（后端 stage fence + 两条 fence 的 `[metal_stats]` 行）、`lib/phy/metal/ocudu_metal_burst.mm`（开 burst 时编等待）、`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.{h,mm}`（`ce_lane_order`/`set_lane_order`/`encode_run`/`collect_async_stage`/`lane_fence_selftest`/计数修复）、`.../port_channel_estimator_metal_mmse_impl.{h,cpp}`（`ce_lane_order_from_env`、三腿 A/B 支持）、`.../metal/test/port_channel_estimator_metal_mmse_unit_test.cpp`（Test 13 重写）|
| K0-a 施工图 | 设计文档 §48.193（含 `mmse_pilots.metal` / `ocudu_mmse_corr.metal` 要改的字段与常量）|

**开关台账（默认值从代码核实）**：`OCUDU_CE_LANE_ORDER`（**默认 `event`**；`wait`/`burst` 为逃生门与对照）、
`OCUDU_CE_FUSED_BURST`（旧别名，新名未设时 `1`→`burst`、`0`→`wait`）、`OCUDU_DFT_OPEN_BLOCK`（**默认开**，`=0` 逃生门）、
`OCUDU_UL_RX_SYMBOLS`（unset/0 = whole-slot 块；>0 = 符号粒度实验）、`OCUDU_UL_FRONTEND_FENCE`（**默认关；开着只会过度等待，别开**）、
`OCUDU_METAL_GPU_TIME`（opt-in，会扰动）、`OCUDU_CE_CPU_CE/CPU_LS/GPU_INVERT/CPU_INVERT/CORR_DEV/SPLIT_TAIL/DEV_SIGMA2/DEV_Y`（A/B 或逃生）。
**K0-a 的 `OCUDU_CE_K0A_FUSE` 目前不在树里**（随回退移除；§48.193 的施工图里定义）。

---

## 7. 回来的第一个动作（TL;DR）

1. `git log --oneline -3` 确认 HEAD = **`74ba4c5251`**；`cmake -P build/build_info.cmake && cmake --build build --target gnb -j 14`，核对 stamp（`strings … | grep -c` == 2）。
2. 读 **§48.192(g)**（三腿判读与"债"的更正）与 **§48.193(a)–(f)**（K0-a 侦察与施工图）。
3. 从 **§5.1 的第 1 步**开工：先做 plumbing（`sigma2_dev`），用三张网 + 序 A/B 的**逐字节相同**作判据；
   第 2 步的 `hop_stage` 接手用 CE 单测 Test 13/14 判据；**先查清 `geom.ok` 那件事**（§5.1.4）再翻默认开。
4. 上机腿用 `sudo -E bash doc_chinese/full_gpu_chain/wip/run_air_leg.sh <label> [OCUDU_*=…]`（需要你手动跑），跑完 `air_leg_report.sh` 判读。
