# 交接（入口） — S34：**L1 离线 harness 两层都建成并判过；D1 的机制现在本机可判；下一刀 = 开放项 #1 的第 2+3 层（落点已核到行，含一个死锁坑）**

> **本文件是新会话的唯一入口**：读完它就能开工。**`-6.md` 及更早的交接保留不删**（它们记录了当时的判断，包括被推翻的）。
> **本文件的权威细节在常驻设计文档**：**§5.9.37–§5.9.43**（本轮 L1 的全部落点、判据、更正与开放项）。
> **⚠ 本文件【更正】了上一轮的两处预估**（§3.1、§3.2），并**更正了我自己在 §5.9.40 写过的一条机制**（§3.5）。

---

## 0. 开机三件事（照做，不要跳）

```bash
cd /Users/jiachengwang/dev/ocudu
git rev-parse --short=10 HEAD && grep -o '[0-9a-f]\{10\}' build/hashes.h | head -1   # 两个短哈希必须相同
git status --short                                                                   # 只应有"用户自己的两个 config"
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py | tail -1                       # 期望 captures=47 problems=0
```
**戳记不同就**：`touch build/hashes.h && cmake --build build --target gnb -j 6`。
**工作树里 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 与 `configs/gnb_rf_b200_tdd_n78_20mhz.yml` 是用户自己改的，永远别动。**

| | 值 |
|---|---|
| **HEAD** | **`bd68ed53ed`**（本会话最后一个提交）|
| 分支 | `apple-silicon`（**本地领先 origin，未推送**；只有 tag 推了）|
| 已打的 tag | **`gpu_phy_d1_handover_p2` → `0ee881c964`（已推，D1 判定时的状态）**；旧 `gpu_phy_d1_handover` → `6fc47e6d13` |
| 门（当前，全部实测）| `value_net` **47/0**、`--self-test` **8/8**、`ctest -R "metal\|ul_pipeline_probe\|puxch\|lower_phy\|du_low\|o_du"` **35/35**、`neutral_vs_baseline` **235 文件 / 131 differing-bytes / 25 captures / 0 missing**、`l1_handover_arms.sh` **4 PASS**、`l1_hop_arms.sh` **rc=0** |
| 构建 | Mac：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON` |

---

## 1. 一句话状态

**D1（一跳一条命令缓冲）已判完并已 tag，不动它。本轮把它的"离线判据"建起来了**：
`. `--dft` 变成**前端 harness**、`--hop-td` 变成**跳 harness**，两者都能在**无空口、无 UE、无捕获文件**的情况下判"交出/认领/MISS 是否改变数据"；
. 顺带量清了 **#1 同槽多 PUSCH 悬崖**（结构性：1 认领 + K−1 必 MISS），
. 并**关掉了**"离线证伪 MISS 设备侧等待"这件事（结案为**离线不可判**，附机制与三轮证据）。
**下一刀是 #1 的实修第 2+3 层**（§4）。

---

## 2. 本轮新增的"工具与开关"（新会话会用到的全部）

### 2.1 两个 harness（都在 `doc_chinese/phy_pipeline_gpu/wip/`）

```bash
cmake --build build --target ul_chain_replay -j 6

# L1a 前端：交出 ⇒ 网格逐字节不变（5 条臂：ref/cand/nogrid/drop/skew）
doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh 32        # rc=0，4 PASS

# L1b 跳：认领/MISS ⇒ 每槽 soft bits 逐字节不变（5 条臂：ref/cand/hostfirst/claim/claimnowait）
doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 16             # rc=0
L1_HOP_PDUS=2 doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 8   # 同槽两跳（量悬崖）

# 5.9.38 发现二的复现率
doc_chinese/phy_pipeline_gpu/wip/l1_hop_rate.sh 50 8           # 0/50
```

### 2.2 `ul_chain_replay` 的新开关（都是本轮加的）

| 开关 | 作用 |
|---|---|
| `--synth N` | **合成 N 个槽**的时域语料（随机 ci16，种子固定）⇒ **不需要任何捕获文件** |
| `--synth-first-slot S` | 合成语料的起始槽号（默认 1；**故意不用 0**，0 是"未设置"的值）|
| `--reuse-grid` | **一块网格服务所有槽**（接收链就是这样；每槽新建网格会掩盖键为什么需要槽那一半）|
| `--hop-td N` | **L1b**：捕获文件只提供 PDU，**网格由前端产出**，真接收机跑在上面 |
| `--hop-pdus K` | **一槽 K 个跳**（多 PUSCH 形状）；K 个跳**先全部提交、再统一等待** |

### 2.3 新增的 plain-C++ 接口

* `include/ocudu/phy/phy_pipeline_grid_ready.h`：
  * `grid_handover_counts` + **`grid_ready_hook::counts()`** —— 交出的计数器，**运行中**可读
    （`[metal_stats] dft handover` 那行是 **atexit 打的，harness 解析不到**）；
  * **`grid_ready_hook::claim()`** —— 非阻塞那一半（认领+兜底提交，返回代际，**不等**）；
  * **`slot_hop_plan_hook`**（`set(slot, hop_count, hop_index)`）—— #1 第 1 层，上层公布跳计划；thread-local 记录，**目前无人消费**。
* `lib/phy/metal/ocudu_metal_burst.mm`：`grid_handover_counts_hook` / `grid_ready_claim_hook` / `slot_hop_plan_set_hook`。

### 2.4 环境变量（A/B 旋钮）

| 变量 | 作用 |
|---|---|
| `OCUDU_DFT_RELEASE_BLOCK=1` | 武装交出（**D1 的总开关**；同时把前端块选到 backend queue，见 §3.5）|
| `OCUDU_GPU_STRICT=1` | 离线 harness 用的 strict 覆盖（`phy_pipeline_strict_enabled()`）|
| `OCUDU_L1_DROP_CONSUMER_WAIT=1` | **反向臂**：L1a 的消费者不等待 ⇒ dump 全 0（**必须红**）|
| `OCUDU_L1_CONSUMER_SLOT_SKEW=n` | **反向臂**：消费者槽号偏移 ⇒ 工具 **rc=1 拒绝**（fail-open 陷阱的证明）|
| `OCUDU_L1_HOST_FIRST=1` | L1b：宿主先 `wait` ⇒ 跳 MISS（但**证伪不了**设备侧等待，原因见 §3.5）|
| `OCUDU_L1_CLAIM_ONLY=1` | L1b：宿主只 `claim` 不等 ⇒ 跳 MISS 且提交在飞 |
| `OCUDU_L1_DROP_MISS_WAIT=1` | **反向臂**：去掉 MISS 的设备侧等待（`grid_devwaited` 应变 0）|
| ~~`OCUDU_L1_STALL_GRID_COMMIT_MS`~~ / ~~`OCUDU_L1_DEFER_COMMIT`~~ | **已回滚**，见 §3.6，**别再用** |

---

## 3. 本轮最重要的技术结论（新会话必须知道，否则会重复踩）

### 3.1 【更正 -6.md §4.3 第 1 步】语料根本不用录，也不用合成 OFDM 信号

判据是"**同一份输入跑两遍，逐字节相同**"⇒ **输入内容不影响判据**。
`ofdm_demodulator_metal_batch_test` 用的本来就是**均匀随机 ci16**。
⇒ `--synth` ≈ 30 行，**不需要** Ubuntu 那台机器，也**不需要** `OCUDU_UL_DUMP_TD`。
**真实空口语料只对"数据正确性"那一半有意义**（那半已有频域捕获 `OCUDU_UL_DUMP`）。

### 3.2 【更正 -6.md §4.3 第 2 步】`--dft` 上要补的是**三个洞**，补一个也判不了

| # | 洞 | 后果 |
|---|---|---|
| 1 | `demod_config.grid_consumed_on_device` **没设**（默认 false）| `wait_per_slot` 为假 ⇒ **`release_block()` 根本走不到**，`handed` 恒 0 |
| 2 | **从不调用 `set_lane_slot()`** | 键的另一半恒为 0，消费者永远配不上 |
| 3 | 宿主直接读网格 | 武装后读到没人写过的内存 |

**⇒ 教训（与 §5.9.19 同源）：判一个"关掉时也正确"的机制，光加判据不够，必须同时确认判据会被触发。**
**⇒ 落地的断言**：武装时要求 `handed>0`、`handed==槽数`、**`not_found==0`**、`unproduced==0`、
`ready_timeouts==0`、`fallback+late >= handed`；未武装时必须 `handed==0`（否则"对照臂"不是对照臂）。
**脚本还拒绝"全 0 的基准"**——两份空网格也会"相同"，那正是 §5.9.19 的空判。

### 3.3 L1a 与 L1b 的实测（都 rc=0）

**L1a（前端，32 槽）**：`ref handed=0 not_found=32`；`cand handed=32 fallback=32 not_found=0 unproduced=0`
⇒ **differing=0**；`nogrid` 也 0；`drop` **differing=32（dump 全 0）**；`skew` **rc=1 被拒**。

**L1b（跳，16 槽）**：`cand` **`taken=16`**、`dft commits` **9→1**；`hostfirst`/`claim` `taken=0 fallback=16`、
`grid_devwaited=16`；**四条臂每槽 soft bits 全部逐字节相同**；
三臂路由计数一致（`ch_re device=88 host=0`、`ch_est device=88 host=0`、`corr_builds/y_writes/sigma2=8/8/8`）
⇒ **没有跳偷偷回退宿主，一致不是"两条宿主路互相印证"**。

### 3.4 同槽多 PUSCH 悬崖：**结构性的，已量化**

`take_released()` 对已认领的块返回 nil ⇒ 一槽 K 个跳**恰好一个能合并**，其余 K−1 必 MISS。
`L1_HOP_PDUS=2` ⇒ `adopted=8 / missed=8`（8 槽）。

**提交数基线（8 槽，`l1_hop_arms.sh` 现在直接打印）**：

| | `dft commits` | `burst commits` | 合计 | 每槽 |
|---|---|---|---|---|
| `K=1` 武装 | 1 | 16 | **17** | 2.12 |
| `K=2` 武装 | 1 | 32 | **33** | **4.12** |

⇒ **每多一个同槽跳，每槽多 2 次提交。**

### 3.5 ★ MISS 设备侧等待为什么证伪不了：**武装把前端搬到了 backend queue**

**（这条更正了我在 §5.9.40 写的"跨队列 + 空载"）**：

```cpp
// ocudu_dft_metal_engine.mm，队列选择
if (block_release_requested()) {        // OCUDU_DFT_RELEASE_BLOCK=1
  // a block that may be RELEASED belongs to whoever commits it, and that is the lane, on the back-end
  // queue - a command buffer is bound to the queue that created it, so a block created on the front-end
  // queue could not be adopted into the lane's chain.
  return shared_queue::backend_queue();
}
```

**⇒ 一旦武装，前端的块就建在 BACK-END queue 上（与跳同一条）**，这是**必需**而非巧合。
同队列上 Metal 的 hazard tracking（网格缓冲是 `newBufferWithBytesNoCopy` + `MTLResourceStorageModeShared`，
即 **tracked**）按**提交顺序**就把写与读排好了 ⇒ **设备侧等待在"提交被及时发出"时是冗余的**。
（harness 里 `front_end fence signals=0 waits=0`，跨队列栅栏压根没武装 ⇒ 它不是排序者。）

**它真正覆盖的只有一个竞态**（引擎注释原话：*"the commit has to happen BEFORE this hop submits"*）：
**别人的提交在本跳提交之后才发出**。harness 同步发提交 ⇒ 这个竞态永不发生。

### 3.6 关掉这条：三轮尝试，全部落空，**已回滚**

| # | 做法 | 结果 |
|---|---|---|
| 1 | 异线程延后提交（`OCUDU_L1_STALL_GRID_COMMIT_MS`）| **rc=139**；`unproduced=8` |
| 2 | **同线程**认领/提交拆分（`claim_grid_deferred`/`commit_grid_deferred`）| **rc=139**，崩在**跳自己的管线**（`pusch_decoder_impl::join_and_notify` ← … ← `pusch_processor_impl::process_data`），**不是**在延后的提交里 |
| 3 | 天然两跳（真并发的设备消费者）| 5 次 × 16 槽 × 2 跳 **0 差异** |

**⇒ 交出的块必须在认领时就提交**（延后 100 ms 会以与目标竞态无关的方式打断跳的链条：那时
`close_held_buffer`、令牌、引擎状态都已往前走过了）。**⇒ "提交晚于本跳提交"这个竞态无法由 harness 制造。**
**§5.9.36 ④-4 第二条反向臂：关闭为"离线不可判"**，不再作为待办挂着。
要真正压这条等待，只能在**有负载的 GPU**上做（空口腿 / 并发实例）。

### 3.7 其它（判读时会用到）

* **5.9.38 发现二**：一次 8 槽 `hostfirst` 的槽 2 估计器标量不同（`noise_variance` 相对差 4.6e-4、
  `ta_us` 位移、47/792 soft bit 差 1 LSB、**网格捕获逐字节相同**）。当时两臂各自重跑完全一致，
  但随后 **`l1_hop_rate.sh 50 8` ⇒ 0/50**、16 槽 0/16 ⇒ 出现率 **< 6%（95% 上界）**。
  **仍记 OPEN（未解释），但不能当机制缺陷的证据。**
* **`OCUDU_UL_DUMP` 的网格捕获在"认领臂"里不可比**：它是**宿主读**，而认领臂的网格要到**那一跳提交**才产出
  （16 槽时 16 个网格全不同，而同一批运行的 soft bits 与估计器标量全相同）。
  **网格捕获只能用于非认领臂**；认领臂要比"提交之后才读"的阶段。harness 脚本已注明。
* **bash 陷阱（本会话踩到）**：批量比对时 `"${x/ref_/$arm_}"` 里的 `$arm_` 被解析成**空变量 `arm_`**
  ⇒ 替换出**不存在的路径**，而 `cmp` 对不存在文件同样返回非 0 ⇒ **"路径写错"被读成"内容不同"**
  （一度四条臂全报"8/8 不同"）。**"有差异"必须先证明"两份文件都真的读到了"**——
  用脚本里那种**前缀剥离 + 存在性检查**，或直接在 python 里比数值。
* **`OCUDU_UL_DUMP_COUNT` 数的是接收次数（跳），不是槽数** ⇒ `--hop-pdus K` 时必须 ×K。
* **`[l1_multi]` 的错过数**用 `跳数 − taken`，**不是** `handed − taken`（`handed` 是"每槽一次的交出"）。

---

## 4. ★★★ 下一刀：开放项 #1 的第 2+3 层（**落点已核到行，含一个死锁坑**）

**目标**：让**一块网格服务 K 个跳** ⇒ 一槽提交数从 K 收回 **1**。

### 4.1 侦察结论（决定了它可行）

`lib/phy/upper/uplink_processor_impl.cpp:229` 的 `for (pdu : pusch_pdus) process_pusch(pdu)`，
每个都 defer 到 `task_executors.pusch_executor`，而 **`max_pusch_and_srs_concurrency` 默认 1**
（`du_low_executor_mapper.h:78`）⇒ **一槽的 K 个跳本来就在同一条车道线程上顺序执行**。
**⇒ 合并 K 个跳不需要跨线程。缺的只是"这个槽有几个跳"这个计数**（第 1 层已交付）。

### 4.2 四步改法（第 2 层与第 3 层**必须同一次落地**）

| # | 位置 | 改动 |
|---|---|---|
| 1 | `ocudu_metal_burst.mm` `burst_state` | 加 `unsigned slot_hops_remaining`、`const void* adopted_grid`、`uint64_t adopted_slot` |
| 2 | `ocudu_metal_burst.mm:347` `shared_burst::commit()` **开头** | `slot_hops_remaining != 0` ⇒ **减一并且不提交**（burst 保持打开）|
| 3 | `ocudu_metal_mmse_engine.mm:1203+` `begin_stage_on_handed()` | **先判"本线程已为这个 (grid, slot) 认领过块"**（`adopted_grid/adopted_slot`）⇒ **直接返回同一个 encoder 追加**，**不要再走 take/MISS 分支** |
| 4 | 同上，`hop_index == 0`（来自第 1 层的 `slot_hop_plan_hook`）| 认领者设 `slot_hops_remaining = hop_count` 并记 `(grid, slot)`；最后一跳提交后清标记 |

### 4.3 ⚠ 死锁坑（第 3 步为什么不能省）

今天第二个跳 MISS 时会 `pending_grid_wait = grid_production_generation(...)`，
而 burst 路由会把它变成 `shared_burst::set_grid_wait()` ⇒ **那个等待被编码在它自己所在的命令缓冲里**，
而它要等的事件**只有这条缓冲自己提交后才会 signal** ⇒ **自己等自己，挂死**。
**⇒ 任何一半单独上都会死锁或毫无作用。别"先切一小刀看看"。**

### 4.4 判据（脚本已就位，直接打出来）

```bash
L1_HOP_PDUS=1 doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 8   # 记下 cand 的合计（今天 = 17）
L1_HOP_PDUS=2 doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 8   # 今天 = 33；修好后必须 = 17
```
* **`cand`（武装）在 K=2 时的合计必须等于 K=1 的（17，而不是 33）**；
* **每跳 soft bits 仍与 `ref` 逐字节相同**；
* **回退宿主仍为 0**（`ch_re device=N host=0`、`ch_est device=N host=0`）；
* `[l1_multi]` 应从 `adopted=1 / missed=K−1` 变成 **`adopted=1 / missed=0`**。
* **落地后按腿法跑一对 `gpu` 模式空口腿**（热路径改动，离线判据不能替代空口）。

---

## 5. 其余开放项（优先级从高到低）

1. ~~同槽多 PUSCH~~ → **本文件 §4，就是下一刀**（第 1 层已落：`d9f4a2029f`）。
2. **`not_found` 的小洞**：跳若"无记录可等"就**直接读网格**（理论上可能读到没人写过的那份）。
   `s44` 的 `not_found=1324` 里多数是"已产出后被淘汰"（无害），但"从未 deposit"那一路没有守护。
3. **认领宽限期**：把 1.049 推到 **1.000**（那 4.9% 是 PUCCH 先兜底提交抢走的）。
4. **单车道并发**：`max_pusch_and_srs_concurrency` 1 → 2（一行配置）；目标是把
   `ul_channel_estimation` p95（3.4 ms）压下来。**注意**：这会让 §4 的"同线程顺序"假设失效，
   与 #1 有交互，**先做 #1 再考虑**。
5. **前端栅栏代际 + `wait_all()` 不覆盖交出去的块**（§5.9.4 ⑤-1/⑤-2）。
6. **`wrap_shared()` 丢非零 offset**（9 个调用点，**静默错址**）。
7. **探针的 `stale=` 分流**（跨度 > 上行 HARQ RTT 8 ms 的样本单独计数 + 槽距）。
8. `burst dispatches` 的 `channel_estimator=0` 计数缺口；`ul_pipeline_probe_test` 时序脆断言。
9. **PRACH-only 时隙是否白做 FFT + 白交一次**——**先数**再决定。
10. **下行方向的同口径记账**。
11. §18.7 的 TA fence 实验。

---

## 6. 腿法（别退化）

```bash
# 0) 戳记必须 == HEAD；手机 WiFi 关掉；sudo 由【用户】执行
touch build/hashes.h && cmake --build build --target gnb -j 6
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>-base
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> OCUDU_DFT_RELEASE_BLOCK=1
#    需要相位分段时两条臂都加： OCUDU_UL_PHASE_SEGMENTS=1
```
**判读顺序**：① `grep -c "will NOT exercise D1" <候选臂>` **必须 0**；② `[metal_stats] dft handover`
`handed>0 taken>0 timeouts==0`、`keepalives` 两侧相等；③ **先看** `UL processor is busy` / 实时失败；
④ **CRC 按调制分层**；⑤ 按调制分层的 sinr（"高 sinr + CRC 全错"才是缺陷签名）；
⑥ `grid_shared==hops`、`grid_wait_unencoded==0`；⑦ 端到端只看**中位/p95 与稳态窗口**；
⑧ 慢变化不要用单点比。

**离线优先，且现在离线判据强得多**：上腿前三条自查——模式对吗（D1 必须 `gpu`）、
判据会触发吗（`handed>0`？）、开关武装了吗。**能离线判的不要上腿**；
但**热路径改动（如 §4）离线判据不能替代空口腿**。

---

## 7. 两台机器

| | Mac（`/Users/jiachengwang/dev/ocudu`）| Ubuntu（`ssh jwang@192.168.100.131`，仓库 `~/work/ocudu`）|
|---|---|---|
| 用途 | **空口腿**（B200 + 手机）、**Metal 侧一切**（D1 机制只在这里存在）、**L1 harness** | 构建（12 核）、ZMQ/B210 + srsUE 台子 |
| 构建 | `-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON` | `ENABLE_METAL_*=OFF`、`UHD=ON`；`gnb` exit 0、全量目标 0 errors、`ctest -N` 7618 |
| ⚠ | Linux 上 **Metal 不存在**（D1 机制无法在 Ubuntu 判）| 工作树里 `pusch_demodulator_impl.cpp` 是未提交的同一份修复；下次同步 `git checkout --` 该文件即可 |

**本轮不需要 Ubuntu**：L1 的语料已由 `--synth` 自产（§3.1）。Ubuntu 只剩"真语料 / 端到端负载"用途。

---

## 8. 本轮的提交（`467cb5b74c` → `bd68ed53ed`，共 10 个）

| 提交 | 内容 |
|---|---|
| `467cb5b74c` | L1 第 1 层：前端 harness（`--synth`、消费者等待与断言、`grid_ready_hook::counts()`）|
| `63456b02f2` | L1 第 2 层：跳 harness（`--hop-td`、真接收机跑在前端产出的块上）|
| `3de28abd06` | 判别实验：`claim()` + 证明**单跳形状证伪不了**设备侧等待 |
| `a2c6c63999` | 同槽两跳（`--hop-pdus K`）：**悬崖量化**（1 认领 + K−1 必 MISS）|
| `d60dbcf6de` | 机制更正：武装 ⇒ 前端块在 **backend queue** |
| `b6544d136e` | 结案：MISS 等待**离线不可判**（三轮尝试 + 证据链）|
| `7d4e3fff97` | §5.9.43：#1 侦察（同线程顺序执行）与修法 |
| `d9f4a2029f` | #1 第 1 层：`slot_hop_plan_hook`（**不改行为**）|
| `fa1f7a97a1` | #1 判据：harness 打印**每槽提交数** + 基线（17 / 33）|
| `bd68ed53ed` | §5.9.43 补：第 2+3 层是**一个原子改动** + **死锁坑** |

---

## 9. 一句话给新会话

**L1 离线 harness 两层都建成并判过**（前端：交出 ⇒ 网格逐字节不变、4 条反向臂会红；
跳：认领/MISS ⇒ 每槽 soft bits 逐字节不变；`dft commits` 9→1），
**同槽多 PUSCH 悬崖已量化**（1 认领 + K−1 必 MISS，每槽提交 K=1→17、K=2→33），
**MISS 设备侧等待结案为"离线不可判"**（武装把前端搬到 backend queue ⇒ 同队列 ⇒ 提交序就排好了；三轮尝试已回滚）。
**下一刀 = §4：照四步落第 2+3 层（务必同时落，否则自己等自己会挂死），
判据 = `L1_HOP_PDUS=2` 的合计从 33 回到 17 且每跳 soft bits 不动，然后跑一对 `gpu` 空口腿。**
