# 02 —— 仪表与读法（时延工作流）

> 活文档。**每一条读数都要能在这里查到"它是怎么来的、怎么复跑、什么情况下会骗人"。**
> 腿日志位置：`doc_chinese/phy_pipeline_gpu/wip/logs/`（腿脚本 `phy_pipeline_gpu/wip/run_leg.sh`）。

## 1. 复跑配方（重上行，验收用）

```bash
# 手机侧/CN 侧：手机【发】、CN【收】  ← -R 不能省，否则变成下行负载（§5.9.128 ① 的教训）
iperf3 -c <phone-ip> -R -b 40M -P 4 -t 240

# gNB（Mac，B210）：
cd /Users/jiachengwang/dev/ocudu
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>            # 验收腿
  # 诊断腿另加： OCUDU_UL_PHASE_SEGMENTS=1
# 停：一次 Ctrl-C，等收尾块；run_leg.sh 会校验报告是否完整
```
**读之前先确认三件事**：① 横幅里的 commit 与 HEAD 一致（`run_leg.sh` 会拦）；② `.stderr` 里有 `[metal_stats]`（否则收尾被截断，见 §5）；③ `[ul_rx_pool]` 的 `taken` ≈ 槽数（说明是每槽一个）。

## 2. 读数清单（谁印的、量什么、怎么读）

| 读数 | 出处 | 量什么 | 读法 / 坑 |
|---|---|---|---|
| `[ul_pipeline]` | `ul_pipeline_probe`（`include/ocudu/support/executors/ul_pipeline_probe.h`）| 一跳的端到端跨度（**与模式无关**）| 与 `[ul_gpu_pipeline]` **是两个口径**，不要混着比；`stale=` 数的是 >8000 µs 的样本 |
| `[ul_gpu_pipeline]` | 同上 | **车道模式下的同一跨度** | 本工作流的 **V1 就用它**（重载基线中位 2675 µs）|
| `[ul_time_frequency]` / `[ul_channel_estimation]` / `[ul_equalization_demod]` / `[ul_ldpc_decode]` | 同上的 staged probes，`OCUDU_UL_PHASE_SEGMENTS=1` **强制开启**（`gpu` 模式默认关）| 三段 + 解码段 | **三段之和应 ≈ `[ul_gpu_pipeline]`**（实测 2657 vs 2675 = 99.3%）；**它们在时间上顺序相接，不能理解为可重叠的三块** |
| `[ul_gpu_lane] residency` | `lib/phy/metal/ocudu_metal_lane_probe.{h,mm}` | 车道那条命令缓冲的**寿命** | 与负载几乎无关（轻 1121 / 重 1125 µs）|
| `[ul_gpu_lane] busy split` | 同上 | 按**组**分：`ch_wt`（权重）与 `merged_hop`（合并跳）| **`merged_hop` ≈ 1030 µs = 车道 busy 的 97%**；residency 里 ~95% 是 busy ⇒ **执行受限、不是排队受限**。**内部不可再分**（D1 把一跳做成一条缓冲，Metal 只给整条缓冲的时间）⇒ 见 `01_plan.md` P0-1 |
| `[ul_gpu_lane] queue / gap / host` | 同上 | 各阶段之间的**空隙**与排队 | `queue: weights commit → weights start` 等；`gap` 的负值正常（时间基准不同）|
| `[mmse_time_sum] defer_wait distribution` | `port_channel_estimator_metal_mmse_impl.cpp` | 宿主**等延迟链**的时间分布 | **与 residency 是同一窗口的两个视角**（宿主视角 / 设备视角）⇒ **不可相加** |
| `[mmse_time_sum]`（其它字段）| 同上 | 估计器宿主阶段：`pre/stage/submit/unpack/cpl_*/corr/gpu_path/cpu_blocks` | 全部**只有几十 µs** ⇒ 估计器的**宿主**工作不是时延主项 |
| `[ul_rx_pool]` | `lower_phy_baseband_processor.cpp` | 接收缓冲池：`taken/returned/held_end/held_max/pool/free_min/starved_takes/starved_events` | **`held_max == pool` + `free_min == 0` + `starved_events > 0` = 池被抽干**（接收线程会被 `pop_blocking()` 阻塞）⇒ **这就是 underflow 的直接原因** |
| `[metal_stats] burst dispatches` | `ocudu_metal_burst.mm` | 一跳里各模块的 dispatch **次数** | 次数 ≠ 时间；不要用它推断时延 |
| `[metal_stats] gpu busy (front_end/back_end)` | `ocudu_metal_queue.mm` | **每队列**命令缓冲的 GPU 时间 | 只到"队列"这一层（`commits/busy/mean/window`），**不是 kernel 级** |
| `[ul_rx_wait]` | 收包侧 | 接收线程的等待 | 三段覆盖不到时的去处 |

### 2.1 ⚠ 两个探针的**样本数不同** ⇒ 不要逐样本对比

实测（腿 `s82`）：相位三段各 **97331** 个样本，而 `[ul_gpu_lane] residency` 有 **140204** 个（= 授权数）。
**它们不是同一个总体**（前者大概只覆盖"走完整条链并与解码配对上的那些槽/跳"，后者按跳计）。
⇒ **可以比"比例与量级"，不可以比"同一跳的两个读数"**；也不要拿"eq_demap ≈ residency"当结论
（在 `00_status.md` §4 里那句话已按此降级为"指示性"）。要逐跳对齐，得先让两个探针用同一个配对键（登记为 P0 的一部分）。

## 3. 本工作流的读法总则

1. **先读定义，再比较**：两个数不一样时，第一件事是问"它们是不是同一个量"（本线四次更正都栽在这里）。
2. **单位对齐**：`staged` 数的是 wrap 次数；`busy split` 的 `cbs/lane` 是**每组**的，两个组相加才是总的（`ch_wt 1.00 + merged_hop 1.00 = 2.00`）。
3. **"读不出"按红算**：契约有**两种打印形式**（`MET (8 of 8 checks applicable)` 与 `NOT MET: 1 of 8 applicable checks failed`），
   解析器只认第一种时，**恰好在契约有话要说时读不出**（§5.9.125 ⑥ 的 bug）。
4. **单次绿等于零证据，单次红也不是证据**：先复跑一次（门已内置；`value_net`、边缘块臂、MMSE 单测都有偶发）。

## 4. 已知的坑（都实测过）

| 坑 | 症状 | 规避 |
|---|---|---|
| **`-R` 的方向** | 以为在跑下行，其实是上行（或反之）| 认准：`-R` = server 发、client 收；**写指令时把这句写进命令注释** |
| **收尾被截断** | 洪泛腿 Ctrl-C 后停在 `Could not stop application after 5 seconds`，`.stderr` 有契约但**没有 `[metal_stats]`** | `run_leg.sh` 现在**同时要求**契约行与 `[metal_stats]`；见到强制退出会点名 |
| **日志洪泛** | 下行饱和时 RF 告警按槽刷，单腿 `.log` 到 **637 MB**（~160 MB/s 峰值）| 见洪泛就尽快 Ctrl-C；磁盘 258 GB 未构成风险但别无谓跑 |
| **缺设备内核的静默回退** | 新建 worktree 没有 `.metallib`，引擎静默走宿主路径而腿仍自称 gpu | `run_leg.sh` 在非 cpu 模式**预检四个内核**并拒绝启动 |
| **并行跑** | 两个门/两个回放同时跑 ⇒ 假失败（实测四条同时出现）| `milestone_audit.sh` 有**互斥锁**；回放类工具**串行**用 |
| **合并后的行号漂移** | 引用主文档行号会指错 | **按句子/章节名检索**，不按行号 |
| ★ **零流量的腿会"静默少判两条"** | 没有 PUSCH 跳时，契约里 `ce device estimates`（`device==0 && host==0` ⇒ `nullopt`）与 `host sample assembly`（`symbols==0` ⇒ `nullopt`）会**退出 applicable 集合**，于是打印成 `MET (6 of 6 checks applicable)`；而里程碑判据要的是 **`MET (8 of 8`** ⇒ **直接 FAIL**，且看起来像"契约坏了" | **默认腿也必须跑正常流量**：`接入 + PDU session + ping/短 iperf3`（历史默认腿就是这么跑的，`s71` 记录 `[ul_pipeline] samples=7703` / 106 s ≈ **75 跳/s**）。⚠ 只有 PRACH（attach 本身）**不产生**这些跳——PRACH 喂的是 A1-2 的"普通路由"，不是融合车道的计数器 |
| ★ **别把重上行 A/B 的门用在默认腿上** | `leg_gate.sh` 里有两条 **`VALIDITY`** 判据（`UL >= 2.0 Mbit/s`、`UL grant in >= 50% of slots`）是 **§5.9.96/§5.9.101 为重上行腿预登记**的；拿它去判一条无负载的默认腿，**必然两条红** | 默认腿用 `milestone_audit.sh`（它按**工况**选腿与判据）；`leg_gate.sh` 只用于**加压腿**。这与"判据绑错工况"是同一类错误，只是方向相反 |


## 5. 工况（regime）是**声明**，不是**症状**——加压腿必须自报

2026-09-24 实测：里程碑门 `milestone_audit.sh` 原来**自动取 mtime 最新的腿**，而最新腿是故意加压的 `s82`，
于是它把两条**默认工况**的判据判成了 FAIL：

* A1-2 的 **C5** 预登记原文是"**腿仍然有效**：契约 8/8、`stale=0`、crossings `0.00+0.00`"（§5.9.118 ③）——这是**默认工况**判据；
* 而 wall A/B 的 **R4** 在加压工况下写的是**相反**的话："`stale > 0` ⇒ 与本线同类，**PASS**"（§5.9.127）。

⇒ **两条都不是发现，是判据绑错了工况。** 之所以一直没暴露：更早一次审计时"最新腿"恰好是一条加压但 `stale=0` 的腿，
默认判据**靠运气**过了。

**规矩（已落到脚本）**：

1. **加压腿必须声明**：`run_leg.sh gpu <label> --regime=stress`，它会把 `[leg] regime=stress` 写进**腿自己的 stderr**（记录源）；
   早于该开关的腿在 `milestone_audit.sh` 的 `STRESS_LEGS` 里按标签声明，并附负载证据来源。
   **"工况"说的是"有没有负载发生器在持续打流量"，不是"有没有流量"**：默认腿自己的配方
   （`接入 + PDU session + ping/短 iperf3`）**仍然算默认工况**——它正是历史默认腿（`s47/s67/s69/s71`）的跑法，
   也是契约 8/8 与 `stale=0` 的取证条件。把"任何流量都算加压"会让默认工况**永远取不到证**。
   反之 `ul_load.sh` / `wall_ab.sh` 的持续配方（如 `iperf3 -R -b 40M -P 4 -t 240`）**才算加压**。
2. **门同时审计两条腿**：默认腿判"契约 / `stale=0` / A1-2 5/5"，压力腿只判**负载改不了的性质**
   （契约名字、MET+mode=gpu、crossings、`gaps`），压力腿的 `stale`/RF/池数字**只报不判**——
   那一档的判据是预登记的 **V1–V5**（§5.9.130 ⑤），也是本工作流的目标。
3. **绝不允许**按腿自己的 `stale` 读数去判定它属于哪个工况：那会让判据**不可否证**。
4. **门的另一条新检查**："**这条腿跑的是哪个提交**"。`run_leg.sh` 在二进制戳 ≠ HEAD 时**拒绝起腿**，
   但这个事实过去只留在控制台和腿的 `.stdout` banner 里，门里的 `binary stamp == HEAD` 说的是**当前构建**、不是**这条腿**。
   现在门会读腿自己的 commit banner，并要求：相等，**或** 该 commit 到 HEAD 的 diff **不触及代码**。
   实测：`s82` 跑 `33de116ce3`，到 HEAD 只差 4 个文件（全在 `doc_chinese/phy_latency/`，**0 代码**）⇒ **它是 HEAD 的证据**；
   而当时最新的**默认**腿 `s71` 跑 `4c4880012c`，到 HEAD 差 **1526 个文件（1463 个代码）**⇒ **它不是**——这句话现在由门说出来。

