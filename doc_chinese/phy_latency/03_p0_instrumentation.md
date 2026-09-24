# 03 —— P0 仪表：已完成什么、怎么读、还缺哪一次读数

> 活文档。本文只记录 **P0（仪表）** 的落地与读法；现象与预算在 `00_status.md`，计划在 `01_plan.md`，复跑与坑在 `02_measurement.md`。
> 最近更新：**2026-09-24**（P0-6 完成、P0-1 实现并离线验证；空口读数待飞）。

## 1. P0-6 ✅ 车道的**生效并发度**现在可读

**为什么必须有它**：时延工作第一个问题是"`ce` 段 901 µs 能否归因为**单车道排队**"（`00_status.md` Q8）。
判它的前提是知道车道执行器的生效并发度，而这个值**过去在任何腿、任何 dump 里都没有**：
配置默认 `concurrency_auto`，YAML dump 把哨兵原样写回；而历史上所有"单车道"读数都来自 **n1** 腿，
两个配置的推导值本不必相同（`derive_pusch_and_srs_concurrency()` 随带宽与 TDD 上行占比变）。

**怎么读**（两行，stderr，**在电台打开之前**打印 ⇒ 短跑 `gnb -c <配置>` 即可离线读到，不必飞腿）：

```
[ul_lane_exec] PUSCH/SRS concurrency = 1 (auto-derived; bw=20MHz layers=1 ul_ratio=0.30; available cpus=14)
[ul_lane_exec] PUSCH lane executor: max_pusch_and_srs_concurrency=1, medium pool max_concurrency=5 -> pusch_executor.max_concurrency=1 (a serialising STRAND: ONE PUSCH hop at a time …)
```

**实测（2026-09-24）**

| 配置 | 推导输入 | 生效值 | 形态 |
|---|---|---|---|
| n78 `configs/gnb_rf_b200_tdd_n78_20mhz.yml` | `bw=20MHz layers=1 ul_ratio=0.30`、cpus=14 | **1** | **串行 strand** |
| n1 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` | `bw=5MHz layers=1 ul_ratio=1.00`、cpus=14 | **1** | **串行 strand** |

> ✅ **空口确认（2026-09-24，用户腿 `s84-p0_0924_2303` 的 `.stderr`，非我的短跑）**——两行确实落在**腿自己的 stderr**（门读的就是这个文件）：
> `[leg] regime=default` / `cell config : configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` /
> `[ul_lane_exec] PUSCH/SRS concurrency = 1 (auto-derived; bw=5MHz layers=1 ul_ratio=1.00; available cpus=14)` /
> `[ul_lane_exec] PUSCH lane executor: … -> pusch_executor.max_concurrency=1 (a serialising STRAND …)`。
> （读于腿运行中：启动行已落盘，契约行只在收尾时打印。）

**三条结论**

1. **C 项（`ce` 901 µs）"单车道排队"的归因成立**——车道确实是串行 strand；一跳占住它 ≈1125 µs，而重载下每 ~1.5 ms 要求一跳。
2. **P1-8 有真杠杆**：可把并发度提到 **2..5**（上限 = 中等池的 `max_concurrency`；`mapper` 对**超过池子**的值直接报配置错误，不夹取）。
   用户已裁决 V4 **只约束交付**（§5.9.132 ①），故该臂作为**纯测量臂**放行。
3. ⚠ **教训（写死）**：该值的推导输入必须**读出来**，不能从配置字面推断——我曾据"小区级未设 TDD 图案"推 `ul_ratio=1.0` ⇒ 2.5 ⇒ 3 路 fork，
   实测是 **0.30**（图案来自**公共小区**那一层）⇒ 0.75 → ceil **1**。

**规则的单测**：`tests/unittests/du_low/du_low_executor_mapper_test.cpp`（4 例：`1→strand`、`3→3 路 fork`、`12=池→fork 12`、`0=无限制→池值`）。
标签 `du_low;du_high`，**故意不含 `phy`**（不改动审计门 `ctest -L phy` 的 193 计数）。

## 2. P0-1 ✅ 诊断开关 `OCUDU_LANE_DIAG_SPLIT`（**默认关**）

**它做什么**：在 `shared_burst::adopt()`（前端把命令缓冲交给估计器的那一点）**不再接管**那条缓冲，而是
`encodeSignalEvent` → `arm_gpu_time` → `gpu_lane_probe::register_commit(cb, stage::dft)` → 提交它，
并为后续阶段开一条 `encodeWaitForEvent` 的新缓冲。
于是 **`dft` 段第一次有读数**——该标签在 `gpu_lane_probe::stage` 里**一直存在**（注释原文 *"per-symbol DFTs; not registered yet"*），只是从未被注册过。

**为什么是这一处**：分组读数其实早有（`OCUDU_CE_LANE_ORDER=event` 路线：`ch_wt≈483 µs` 87% + `eq_demap≈71 µs` 13%，§5.9.66 ②），
缺的只是**前端那 14 个 transform 在 `merged_hop`（≈1030 µs）里占多少**。

**离线验证载体**：`dft_release_adopt_metal_test`（`lib/phy/generic_functions/metal/test/`）——
它专门演练"前端交出未提交的块 → 车道接管 → 消费者读网格"，并打印 `[ul_gpu_lane]`。
（CE 单测与 `ul_chain_replay` **都到不了 `adopt()`**：它们的 `busy split` 是 `ch_est`+`ch_wt`，没有 `merged_hop`。）

| 同一二进制 | `cbs/lane` | 断言 |
|---|---|---|
| **OFF（出厂默认）** | **1.00**（max=1）| 两条全 PASS，含 *"one command buffer, one commit"* ⇒ **出厂形态一字未变** |
| **ON** | **2.00**（max=2）| 拆分生效；residency 44 → 172 µs（多一次提交的代价）|

**★ 该臂的第二重身份（必须记住）**：ON 时那条测试的**别名陷阱 FAIL**——因为陷阱要求两次 dispatch 在**同一条**缓冲里
（坑 36：Metal 按 `MTLBuffer`**对象**排序，barrier 排不了两个对象），而拆分用**事件**给出了真实的跨缓冲顺序，**把陷阱掩盖了**。
⇒ **拆分臂的 ordering 语义比生产路线更严格**：**不得**用它判生产正确性；它**只**用于**分组 GPU 时间**。

**读该臂前的两条静态核实（读码，2026-09-24）**：① `stage_name(stage::dft)` 返回字面 **`dft`**
（`ocudu_metal_lane_probe.mm:189-190`）⇒ `busy split` 里的 token 就是 `dft=`；
② `register_commit()` 只是把 `{cb, stage, now}` 压进**本线程的 `pending`**（`:304-310`），
**不要求车道已开启**，由 `close_lane()`（跳尾 `wait_committed()` 调用）归属 ⇒ 前端那次提交会被正确计入。

⚠ **命名陷阱（必须记住）**：拆分臂里第二个缓冲仍被估计器标成 **`merged_hop`**（它并不知道前端已被分出去）⇒
**开关 ON 时读到的 `merged_hop` 含义是「前端之后的全部」，不是整跳**。
判据里的「各段之和 ≈ 出厂臂的 `merged_hop`」算术仍成立，但**名字会骗人**：读该臂时请把它当 `rest_after_front_end`。

**该臂的污染项（写死）**：① 一跳 **2 条缓冲** ⇒ `cbs/lane≈2`，**V4 不适用于诊断臂**；
② 前端输入缓冲**更早释放**（正是 **P2-E** 的题目）⇒ **不得**用该臂读跨度、`cbs/lane`、池、V1–V5。

## 3. 还缺的读数（需要一条空口腿；跑法与判据）

```bash
# 0) 起腿前确认（run_leg.sh 也会硬检查这两条）
pgrep -x gnb || echo ok; pgrep -x ul_chain_replay || echo ok; lsof -nP -iUDP:2152 || echo ok

# 1) 默认腿（n1）：CN `ping 10.45.0.10 -i 0.1 -c 100` → 10s 下行 iperf3 → 10s 上行 iperf3 → 空跑到 ~100s，一次 Ctrl-C
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>
# 2) 加压腿（n78，40 Mbit/s 上行配方）+ 相位分段
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml OCUDU_UL_PHASE_SEGMENTS=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label2> --regime=stress
# 3) 诊断拆分臂（只读 busy split）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml OCUDU_LANE_DIAG_SPLIT=1 \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label3> --regime=stress
```

**一条命令读完（新工具，只读日志、不碰 GPU）**：

```bash
bash doc_chinese/phy_pipeline_gpu/wip/p0_gate.sh <腿标签>                    # A1/A2（P0-6）+ B1（拆分臂的 dft=）
bash doc_chinese/phy_pipeline_gpu/wip/p0_gate.sh <拆分臂> --vs=<出厂臂>       # + B2：各段之和 ≈ 出厂臂 merged_hop（±10%）
```

判据：**A1** 两行 `[ul_lane_exec]` 存在且"生效值 == `pusch_executor.max_concurrency`"；
**A2** 形态与值一致（`≤1` ⇒ `STRAND`，`>1` ⇒ `task fork limiter`）；
**B1** 拆分臂**必须**有 `dft=`（`cbs/lane=1.00`）、出厂臂**必须没有**；
**B2** 各段之和在出厂臂 `merged_hop` 的 ±10% 内。**"读不出"按 RED 算**（自测：拿 P0-6 之前的旧腿 `s83-tag` 跑，A1/A2 正确地报 RED）。

**读法（一次一条 grep）**

| 目的 | 命令 | 判据 |
|---|---|---|
| 生效并发度与形态 | `grep -a ul_lane_exec <leg>.stderr` | n78/n1 都应为 `= 1` + `STRAND`（若 n78 变成 >1，则 C 项归因与 P1-8 都要重写）|
| **P0-1 的 `dft=` 段** | `grep -a "busy split" <leg3>.stderr` | 出现 `dft=…`，且 **`dft + (合并段) + eq_demap` 之和 ≈ 出厂臂的 `merged_hop`（±10%）**。**打印语义（读码核实，`ocudu_metal_lane_probe.mm:647-665`）**：该行**每个"至少注册过一次提交"的段各打一个 token**（`if (stage_cbs[i] == 0) continue;`），`%` 是占**所有段 busy 之和**的比例，`cbs/lane` 是该段每"车道"的提交数 ⇒ 开关 ON 时**必然**多出 `dft=`；OFF 时该段不出现（前端缓冲被接管，没有提交）|
| P0-1 候选 A / P0-5 配对 | `<leg2>` 的 `busy split` 与三段 | 配对后 `busy split` 三项之和 ≈ `residency`（±10%）|
| 门 | `bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh` | 新默认腿顶掉无效腿后应回到 GREEN（离线判据本就全绿）|

**这些数字将决定什么（先写死，避免事后解释）**

* 若 **`dft` 大**（≈ 数百 µs）⇒ 前端那 14 个 transform 是 D 项的第二个大头，杠杆在"**重叠/提前**"（符号级收包 S-7g-13、把变换并入车道更早启动），
  而不是继续砍 kernel。
* 若 **`dft` 小**（几十 µs）⇒ `merged_hop − dft` 基本就是**估计器权重链**（离线账单 ≈500 µs：K1 197.5 + 抽取 117 + 重排 116.9 + K2 35.6 + corr 35.3），
  而它被**逐字节不变**钉住 ⇒ 真正的选择变成**用户是否启用 §5.8.19 ① 已放宽的"容差内可变"自由度**（每次改动须飞一条腿）。
* 两种情况下 **C 项（901 µs 排队）** 的杠杆都仍然只有两个：**并发度**（P1-8，1→2..5）或**缩短单跳窗口**（上面的 D 项）。

## 4. 本轮的两个流程教训（都记在我头上）

1. **我为读一行启动打印而短跑的 gnb 没被杀掉**，成孤儿并占住 `192.168.64.1:2152` ⇒ 用户的下一条腿 15 秒即失败、
   **没有任何契约报告**（审计读成 `0 of 8`），且它**抢 GPU** 让我随后的门读数被污染。
   ⇒ 已加 `run_leg.sh` 硬预检（残留进程 / UDP 2152 被占则拒绝启动，并打印持有者）。
2. **那条预检的第一版用 `pgrep -f`（匹配整条命令行）**，结果**匹配到了正在执行该命令的、我自己的 shell**，
   把用户一条正常腿挡了。⇒ 已改为**按进程名精确匹配**（`pgrep -x gnb` / `pgrep -x ul_chain_replay`）。
   **规则**：进程检查永远按名字；命令行的模糊匹配会把"提到这个名字的人"当成"那个进程"。

## 5. 一条待补的离线确认（一条命令，已按用户要求暂缓）

`dft_release_adopt_metal_test` 在 `OCUDU_LANE_DIAG_SPLIT=1` 下的输出**我只看到了前 12 行**，而 `busy split` 那行在 `print_series(…)`/`print_front_end()` **之后** ⇒ 被 `head -12` 截掉了。
⇒ **离线确认 `dft=` 只差一条命令**（同一二进制、约 1 秒）：

```bash
OCUDU_LANE_DIAG_SPLIT=1 build/lib/phy/generic_functions/metal/dft_release_adopt_metal_test 2>&1 | grep -a "busy split"
```

用户已要求我停手等腿，故**暂不执行**（它虽只有约 1 秒且不占任何 socket，但仍会碰 GPU，若与用户正在飞的腿重叠会污染对方的时延读数）。用户给出口令或腿落地后第一条就补它。
