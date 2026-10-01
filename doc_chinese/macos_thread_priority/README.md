# `doc_chinese/macos_thread_priority/` —— macOS 上 PHY **关键线程的运行稳定性**工作流

> **先读这一条**：本线的**结论**在 `thread_running_high_level_status_and_plan.md` 的 **§4bis**
> （`max` 不是处理时间，而是外部停顿；优先级改不动它，"安静机器"有效；未知项与重开条件都在那里）。
>
> 本目录于 **2026-10-01** 按用户要求建立：**此后与本项有关的文档都放这里**。
> 上游（历史）记录仍是 `../phy_latency/`（**追加式，不改历史**）；本线的入口在那里的
> **§6.248**（ms 级停顿收口为欠账）与 **§6.249**（macOS 测量规划）。
> 腿日志与门脚本的**历史**仍在 `../phy_pipeline_gpu/wip/`；**本线新飞的腿**放 `wip/logs/`。

## 一句话

交付侧是绿的（全量审计 33 PASS / 0 FAIL / 0 RED）；但**尾部尖峰在每条插桩序列上都出现**，
而 macOS **没有硬实时**（"RT" 实际只有 QoS 一层可能生效，且 `taskinfo` 显示 UI/IN 计费为 0、天花板 `THREAD_QOS_LEGACY`）。
本线 = 把关键线程的稳定性做成**可测 / 可归因 / 可设置**，**并保证 Linux（Ubuntu 台架）行为逐字不变**。

## 文档分三类（沿用 `../phy_latency/README.md` 的约定）

| 类 | 文件 | 性质 | 读者 |
|---|---|---|---|
| **高层现状与计划** | `thread_running_high_level_status_and_plan.md` | **活文档**（可重写）：现状、问题形状、线程地图、目标与判据、三层测量、P0–P4、待裁决 | 任何人（**先读这个**）|
| **开发文档** | `thread_priority_optimization_design_and_implementation.md` | **追加式**（不改历史，更正写在后面）：设计细节、接口、插入点、判据与反向臂、读数与证据、坑与教训 | 动手改代码的人 |
| **会话交接** | `session_handoff_<日期>-<序号>.md` | **快照**：新会话读它就能开工（可引用其他文件）| 下一个会话 |

## 目录约定

| 路径 | 是否跟踪 | 放什么 |
|---|---|---|
| `wip/*.sh`、`wip/*.py` | ✅ **跟踪** | 腿配方与旁观脚本（它们产出证据，属于"人读了才懂"的东西）：`observe_threads.sh`（旁观，默认 light）、`taskpolicy_ab.sh`（P2 档位 A/B）、`spike_census.py`（尖峰普查，读 p99）、`threshold_candidates.py`（按**家族**导出 C1/C2 阈值）、`freeze_census.py`（把每条尾部事件与**该腿自己的**切换率基线并列 → 区分「整进程冻结」与「线程没被调度」，并给出可拿去 `log show` 追查的**本地时刻**）|
| `wip/logs/` | ❌ **忽略**（`doc_chinese/.gitignore` 的 `**/logs/`）| 本线新飞的腿（三件套：`.log` / `.log.stderr` / `.log.stdout`）。★ **飞腿时必须带 `LEG_LOGDIR=doc_chinese/macos_thread_priority/wip/logs`**：`run_leg.sh` 的默认仍指向 `../phy_pipeline_gpu/wip/logs/`（2026-10-01 之前它的 `LOGDIR` 是写死的，所以本目录一直是空的 —— 用户发现，见开发文档 §10.14）；三个门（`leg_gate.sh`/`ul_load.sh`/`milestone_audit.sh`）**两个目录都会找**，所以腿放哪边都能判 |
| `wip/sched_microbench/` | ✅ **跟踪** | P4 的**微基准依据**（2026-10-01）：9 个独立可编译的探针 + `run_all.sh`（一条命令跑完，约 3 分钟）+ `loopback_arm_sweep.sh`（把**各臂放到 loopback 台上跑基率**：无电台、无 UE、无 sudo，18 秒一轮）。★ 它回答了"时间约束值多少"（2× 超订下唤醒尾延迟 **5358 → 9.9 µs**）、"哪种参数有害"（只有 `constraint<computation`），以及**2026-09-01 回归到底是谁的错**（**范围**，不是参数：全量臂 0/3 干净，池线程/单线程/io 3/3 干净，`lower_phy_*` **0/12**）。读数表在它的 `README.md`，判定在开发文档 §10.29/§10.30 |
| `work_tmp/` | ❌ **忽略**（`**/work_tmp/`）| `taskinfo`/`powermetrics`/`sample` 的采样文件、dump、scratch |

## 与本仓其他目录的关系

* `../phy_latency/`：上一段工作（PHY 端到端时延、ms 级停顿的收口、`OCUDU_UL_TIMING_EVENTS` 仪器）。**本线的引用来源**。
* `../phy_pipeline_gpu/`：更早的阶段（融合车道、门与腿脚本的**历史**位置）。
* `../macos_compat_refactor/`、`../test_environment/`：macOS 兼容层与台架的历史记录。

## 相关的树级文档

* **`../ocudu_env_knobs_inventory_and_leg_whitelist.md`**（生成物）：**整棵树**的 `OCUDU_*` 旋钮默认值/读取点/飞过的腿 + **验收腿白名单**。
  本线的任何新旋钮都必须在**改生成器**（`../phy_latency/wip/gen_knob_inventory.py`）之后重新生成它，并按需加入白名单。
  本线现状：`OCUDU_SCHED_VERBOSE`（**只打印 ⇒ 在白名单**）、`OCUDU_SCHED_ATTR_QOS` 与 `OCUDU_SCHED_POSIX_RT`
  （**改调度 ⇒ 故意不在白名单**，fail-closed —— 带它们的腿是**臂**，不得进验收结论）。
  ★ 2026-10-01 用户裁决：**跳过 POSIX 调用已是 macOS 的新默认**（`OCUDU_SCHED_POSIX_RT=1` 是恢复历史行为的对照臂）。
* `../README.md`：`doc_chinese/` 的总索引。

## 引用规则（写文档时必须守）

1. **一台仪器两把钥匙**：编译期开关 + env，默认关；**关着时报告逐字节不变**。
2. **反向臂**：每个新字段都要有一条能把它打红的反向臂（纪律 79）。
3. **判据先登记再飞**，且**不追溯**已判的腿。
4. **Linux 逐字不变**：`#if defined(__APPLE__)` 之外零行为改动，`#else` 只写 no-op；Ubuntu 台架复核记进开发文档。
5. 引用"停顿/尾部"必须同时给 **`cpu`/`win`/`ivcsw`/`base_age` + 同秒的 `[RF]` 行**。
