# Session handoff —— 2026-10-01-2（macOS PHY 线程运行稳定性：**悬案已结，仪器已落地**）

> **新会话先读这一份**，然后按需打开它引用的文件。
> 上一份：`session_handoff_2026-10-01-1.md`（同目录，规划期）。
> 上游（历史）：`../phy_latency/session_handoff_2026-09-27-3.md`；本线入口 = `../phy_latency/` 开发文档 §6.248/§6.249。

---

## 1b. 2026-10-01 深夜更新（本文件写完之后又飞了两对腿）

* **P0/P1/P3 全部落地**，`[sched]` 自读在 **loopback 与真电台腿**上逐字复现同一结论（§2 的表）；
* ★ 读 **p183** 时发现并修掉**我自己**的一个热路径 bug（相位事件的"关着"臂没真关：每 hop 4 次 Mach 快照 + 6 次读钟），
  带能打红的反向臂（开发文档 **10.13**）；
* **`p185-n78-default` + `p186-n78-stress`（HEAD `1b00af6780`）= 当前 GREEN 对**：全量审计 **33 PASS / 0 FAIL / 0 RED**，
  `leg_gate` 各 **9/10**（唯一 FAIL 是登记的 `slip over 1ms <= 10` 读到 12 —— 平台近期常态，**等用户裁决**，开发文档 10.15(5)）；
* 腿日志目录问题已按"让工具支持文档"解决（`LEG_LOGDIR` + **8 个脚本**都会找两个目录 —— 少补一个就会造出一条假 FAIL，实测过），
  新腿现在真的落在 `doc_chinese/macos_thread_priority/wip/logs/`；
* 新仪器在真腿上的首读：进程 **194.65 次非自愿切换/ms**；`win=2160us` 里**进程 4.33 ms / 本线程 0.12 ms** ⇒
  接收尾巴是**阻塞等待**而不是宿主饿着；相位尾部首次带**线程名**（ce on `main_pool#0/#1/#4`、eqd on `#1..#4`）；
* **已知缺口**：相位事件的 `cpu=`/`tcpu=` 多为 `-`（基线时序），修法已设计，**与下一次改动合并成一次重飞**。

## 1c. 2026-10-01 深夜第二段（两项裁决已落地，等最后一对腿）

* **裁决 ①（判据）**：`slip/recv over 1ms` 由 10/10 **前瞻性**重登记为 **22/19**（出处 = 最近 9 条家族腿最坏值；
  `AT/BELOW 0 <= 10` 不变）。落地 `leg_gate.sh` + 高层 §5.2 + 开发文档 §10.16(1)。p185/p186 由 9/10 → **10/10**。
* **裁决 ②（默认）**：`OCUDU_SCHED_SKIP_POSIX_RT` 反转为 **`OCUDU_SCHED_POSIX_RT`**，**新默认 = 不调用 POSIX**（QoS 档存活）；
  `=1` = 恢复历史行为，作为对照臂留在同一二进制上。两条臂都在 loopback 复验（开发文档 §10.16(2)）。
* **相位基线缺口已修**（§10.16(3)）：`cpu=`/`tcpu=` 不再是 `-`；单测抓到我第二版实现里一个"纳秒级却致命"的缺陷。
* 回归：`ctest -L phy -j 1` = **208/208**；探针单测整二进制 14/14、ctest 9/9。
* Ubuntu 台架同一 commit：`BUILD_RC=0`（0 warning）+ `ctest -L phy` **184/184**。
* ⚠ **因此 p185/p186 已过期**（这两项改动都动了 `lib/`/`utils/`）⇒ **需要最后一对腿**：

```bash
cd /Users/jiachengwang/dev/ocudu
# 腿 ①：default（新默认 = QoS 档存活；带读档与事件表）
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml \
OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_UL_TIMING_EVENTS=16 OCUDU_SCHED_VERBOSE=1 \
LEG_LOGDIR=doc_chinese/macos_thread_priority/wip/logs \
  sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p187-n78-default --regime=default
# 腿 ②：stress（同上 + --regime=stress；先判载荷）
```
**这一对腿要看的新东西**：① `[sched]` 里 RT 线程应变成 `eff=USER_INTERACTIVE posix=OTHER/31`（**默认臂**，
不再是 `UNSPECIFIED`）；② 相位块（`ce`/`eqd`/`ldpc`）的事件行应开始带 **`cpu=<数值>`**（不再是 `-`）；
③ `leg_gate` 两条都应是 **10/10**。

## 1d. 2026-10-01 收尾：**稳定性定义的层次被用户更正**，新仪器已就绪（等一对腿）

* 用户定义（最终版）：稳定性 = **同一次运行内** mean/median/min/max/p95/p99 变化不大；
  **跨运行**的差异是环境（无线/业务类型）造成的，本来就会变。⇒ 我先前登记成"跨腿离散"是**错的层次**，已更正（高层 §5.3、开发文档 §10.20）；
* 新仪器 **`OCUDU_UL_STABILITY_WINDOWS=K`**：把**本次运行**按**记录顺序**切成 K 个等样本窗口，
  逐序列打印每个窗口的 median/p95 与相对整腿值的**最大偏离**。**热路径零成本**（重切已有向量），
  关着逐字节不变，**已在两条闸门的 probe 白名单**里；
* loopback 实测：`ul_rx_wait n=448054`，八个窗口 median 全 34.0、p95 全 38.0、最坏偏离 **0.0%** —— 而同一条腿 **max=7868 µs**
  ⇒ "分布稳、尾部有尖峰"第一次被量化在**同一次运行**里；
* 反向臂付了学费：第一版用"快半段+慢半段"的**台阶**输入，**根本不可能变红**（排序二值序列会得到同样的窗口图案）；
  换成**交替**输入后才真正可判（sabotage 实测变红）。详见开发文档 §10.20；
* 回归：本机 `ctest -L phy` **209/209**；Ubuntu **`BUILD_RC=0` + `ctest -L phy` 184/184**。

## 1. 一句话状态

**悬案结了，答案比"被钳"更硬**：`[sched]` 自读（P1，`OCUDU_SCHED_VERBOSE=1`）**第一次运行就显示** ——
我们请求的 `USER_INTERACTIVE` QoS 类**从来没有生效过**，因为**紧随其后的 `pthread_setschedparam(SCHED_FIFO,prio)`
把它静默抹掉，而且不可恢复**（再设返回 EPERM）。今天 macOS 上**数据面线程处于"无 QoS 档"**，
**比它们本该压制的 `io_timer`/`io_broker` 还低一档**（那两条不调用 POSIX，档留着）。
⇒ **P0 完成**、**P1+P3 代码已落地并本地/台架验证**、**P2 配方与预登记已写好**；**P4 建议暂不施用**。
⚠ 本次动了 `lib/`+`utils/`+`include/` ⇒ **`p181`/`p182` 按审计规则过期，需重飞一对腿**（用户操作）。

---

## 2. ★ 本会话的核心发现（开发文档 §10.5 有完整读数）

| 事实 | 读数 |
|---|---|
| 请求 vs 实际（真实进程，loopback）| `[sched] thread=main_pool#0 rt_intent=1 req=USER_INTERACTIVE eff=UNSPECIFIED posix=FIFO/44` |
| 同一条腿上的自洽反证 | `io_timer_tick`（`rt_intent=0`，**不调用** POSIX）回读 `req=USER_INITIATED eff=USER_INITIATED` |
| 机制（微实验）| `set_qos(USER_INTERACTIVE)` → 0，回读 USER_INTERACTIVE；随后 `setschedparam(SCHED_FIFO,46)` → 0，回读 **UNSPECIFIED**；**再** `set_qos` → **1 (EPERM)**；`SCHED_OTHER` 之后再试仍 EPERM |
| attr-QoS（P3）| 属性上声明的档**同样**被那个 POSIX 调用抹掉 ⇒ **P3 单独做没有意义** |
| Mach 时间约束（P4）| `thread_policy_set(TIME_CONSTRAINT)` 成功，但**同样把 QoS 档清 0**，且此后 `setschedparam(SCHED_FIFO)` **返回 EINVAL** ⇒ **与 QoS 互斥** |

**规则一句话**：Darwin 上线程**要么由 QoS 管、要么是显式调度**（POSIX 参数 / Mach 时间约束），不能两者兼有；
显式调度那一侧**静默清掉** QoS 档。我们过去两条路都走 ⇒ 净效果 = 无档。
**追认两件旧事**：① `taskinfo` 的 `UI/IN 计费 0 s` + `ceiling THREAD_QOS_LEGACY`；② 2026-09-01 那次
"自动施用时间约束 + OAI-UE 随机接入回归" —— 当时被记为"改了运行语义"，现在知道改的是**QoS 档被清掉**。

**A/B 开关（默认关，同一个二进制两条臂）**：`OCUDU_SCHED_SKIP_POSIX_RT=1` ⇒ 跳过 POSIX 调用 ⇒
`eff=USER_INTERACTIVE posix=OTHER/31`（已在 loopback 上验证）。

---

## 3. 本会话做了什么（都在 HEAD 上）

* **P0**：`wip/spike_census.py` 升级读 p95/p99；新增 `wip/threshold_candidates.py`（按**家族**过滤：
  `pipeline mode` + `cell config` + **接收策略**）；**阈值规则被第一次运行推翻** ⇒ 改成
  **C1（p99）+ C2（median）**，`max` 降级为读数（开发文档 §10.4、高层 §5.2 已登记）。
* **P1/P3 代码**（一个 commit `a98faa29a6` + 一个修复 `268f3b1760`）：
  * 新 `include/ocudu/support/scheduling/thread_sched_snapshot.h` + `lib/support/scheduling/thread_sched_snapshot.cpp`
    （macOS `THREAD_BASIC_INFO` / Linux `RUSAGE_THREAD`；**平台上给不出的字段是 -1 不是 0**）；
  * `unique_thread` 在**所有**调度调用之后做 `[sched]` 自读（两把钥匙：`ENABLE_FLOW_PROBES` + `OCUDU_SCHED_VERBOSE`）；
  * `OCUDU_UL_TIMING_EVENTS` 的每条事件加 `thread=<名字>#<id>`、`tcpu=`（本线程窗口 CPU，跨线程时打 `-`）、
    `ivcsw_rate=`，报告头加 `leg :` 基线速率；**4 条相位序列（t2f/ce/eqdem/ldpc）的尾部**进同一 worst-K（每序列一个 floor）；
  * `OCUDU_SCHED_ATTR_QOS`（P3）与 `OCUDU_SCHED_SKIP_POSIX_RT`（A/B 杠杆）两个开关，默认关；
  * 单测 4 个新用例（含跨线程 `tcpu=-` 反向臂、关着静默臂、floor/边界臂）。`ctest -L phy` = **207/207**。
* **P2**：新增 `wip/taskpolicy_ab.sh`（`scan`/`set`/`clear`）+ **预登记**写进开发文档 §10.7。
  本机 `taskpolicy -l` 接受 **1..5**（6 及以上被拒），**数字越大越偏延迟**（与常量名相反）。
* **白名单/生成物**：`OCUDU_SCHED_VERBOSE` 进两条闸门的 **probe 白名单**（只打印）；
  两个**改调度**的开关**故意不在**白名单（fail-closed）。`gen_knob_inventory.py` 扫描根加入 `utils/`，
  并新增一条"复合守卫"分类规则（**放在链条最后** —— 放前面会把 `OCUDU_DFT_BACKEND` 读错，试过）；
  清单已重新生成。
* **Ubuntu 复核**（`ssh jwang@192.168.0.106`，已把该检出快进到本 commit）：
  `macos_compat_test` + `ul_pipeline_probe_test` = **22/22 通过**（1 skip = flow probes 关）；
  ★ 台架**抓到两个真实的 Linux 错误**，都已修：
  ① `qos_class_t` 在新单测里声明在 `#if defined(__APPLE__)` 之外（`268f3b1760`）；
  ② **`ENABLE_FLOW_PROBES=OFF`（项目默认、也是台架配置）从 `328d273e0f` 起在 Linux 上编不过**
  —— `spans_stream_start` 只被探针那一臂消费，GCC 以 `-Werror=unused-variable` 拒收；
  修法 = `[[maybe_unused]]`（`94e93cc482`，行为不变）。**这跟本线无关，但只有台架能看见**。
  另做了一次 **Linux + `-DOCUDU_FLOW_PROBES` 的定点编译** ⇒ 通过。
  全量：**`BUILD_RC=0`（0 warning）+ `ctest -L phy` = 184/184 通过**（开发文档 §10.10）。

---

## 4. 下一步（**需要用户**）

| # | 事项 | 说明 |
|---|---|---|
| 1 | ★ **裁决**：是否把 `OCUDU_SCHED_SKIP_POSIX_RT=1` 立为 macOS 新默认 | 今天默认 = 数据面无档（低于 `io_timer`）；跳过 POSIX 后回读 `eff=USER_INTERACTIVE`。代价 = 失去那条**只被内核记录**的 `SCHED_FIFO` 标注。建议**先飞 A/B 腿再定** |
| 2 | ★ **飞腿**（我不能飞：`sudo` + B210 + 手机）| 建议顺序：① 一对**验收腿**（default + stress，HEAD，**不设**新开关）恢复 GREEN；② 一对**读档腿**（同配置 + `OCUDU_SCHED_VERBOSE=1`）；③ **P2 档位 A/B 腿**（stress + `taskpolicy`，预登记见开发文档 §10.7）；④ 可选 B 臂（`OCUDU_SCHED_SKIP_POSIX_RT=1`）|
| 3 | P4（Mach 时间约束）| 建议**维持不施用**：与 QoS 互斥（会再次清档），且缺每线程计算量的 p99（开发文档 §10.8）|

**飞腿配方**（与上一份相同，只是多两个可用开关）：
```bash
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 \
  OCUDU_UL_TIMING_EVENTS=16 OCUDU_SCHED_VERBOSE=1 \
  sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> --regime=default
```
⚠ **手机移动数据开关必须先打开**（纪律 63）；⚠ `OCUDU_SCHED_VERBOSE` **在白名单里**（只打印），
但 `OCUDU_SCHED_SKIP_POSIX_RT`/`OCUDU_SCHED_ATTR_QOS` **不在** ⇒ 带它们的腿是**臂**，不得进验收结论。

---

## 5. 判据与读数速查（本会话新增/更新）

* **判据**（高层 §5.2，**已登记、不追溯**）：C1 `p99 ≤` 登记值、C2 `median ≤` 登记值（stress 家族 7 条腿导出）；
  `max` **不判**，由事件列表归因。出处：`python3 doc_chinese/macos_thread_priority/wip/threshold_candidates.py --legs 7`。
* **读档**：`grep -a '^\[sched\]' <leg>.log.stderr`（要 `OCUDU_SCHED_VERBOSE=1`）；
  `grep -a -A40 '^\[ul_timing_events\]' <leg>.log.stderr`（要 `OCUDU_UL_TIMING_EVENTS=N`）。
* **新事件行的判读**：`cpu=` 进程 / `tcpu=` **本线程**；`cpu=大 tcpu=0` = 这条线程丢核（调度）；
  两者都 ≈ `win` = 线程一直在跑（活儿/IO 慢）；`tcpu=-` = 没有读数（基线是别的线程记的，**不是 0**）。
* **P2**：`sudo -E bash doc_chinese/macos_thread_priority/wip/taskpolicy_ab.sh <label> scan`。

---

## 6. 仓库状态（交接时刻）

| 项 | 值 |
|---|---|
| HEAD | `95c8ed50bb`（P1+P3 仪器 + 2 个台架抓到的 Linux 修复 + 文档）|
| 三段戳 | 已重打 = HEAD，且二进制内含该戳 ✔ |
| 工作树 | 干净（用户的 `configs/*.yml` 不入库）|
| 本机测试 | `ctest -L phy -j 1` = **207/207**（208 用例，1 禁用）|
| Ubuntu | 检出已快进到同一 commit；全量 build **0 warning**、`ctest -L phy` **184/184**（§10.10）|
| 旧腿 | `p181`/`p182` **已过期**（本次改了 `lib/include/utils`）⇒ 需重飞一对 |
| 新增旋钮 | `OCUDU_SCHED_VERBOSE`（白名单，只打印）、`OCUDU_SCHED_ATTR_QOS`、`OCUDU_SCHED_SKIP_POSIX_RT`（**臂**，fail-closed）|

---

## 7. 纪律（本线，沿用上一份 + 本会话新增）

1. **旧 memo 不改**；要记的写开发文档（追加式）。
2. **两把钥匙**：新仪器 = 编译期开关 + env，默认关；关着时报告**逐字节不变**。
3. **反向臂**：每个新字段都要有能把它打红的臂（本会话 4 个新用例都带）。
4. **判据先登记再飞**，不追溯（本会话把 `max` 从判据降级为读数，就是这条纪律起作用的结果）。
5. **Linux 逐字不变**：`#if defined(__APPLE__)` 之外零行为改动。★ 本会话证明**跑 Ubuntu 是值得的**：它抓到一个 Linux 编译错误。
6. **日志是 root 的**；新腿用 `run_leg.sh`。
7. **上报"停顿"必须同时给** `cpu`/`win`/`ivcsw`/`base_age` + 同秒 `[RF]` 行；**现在还要给 `thread=`/`tcpu=`**。
8. **heavy 观察（taskinfo/powermetrics/sample）只能在 `-diag` 腿上**（§2.4，153 ms 的教训）。
9. ★ **新**：**"设了"不等于"生效"** —— 任何调度/档位改动都必须在**线程内回读**（本会话的全部价值来自这一条）。
