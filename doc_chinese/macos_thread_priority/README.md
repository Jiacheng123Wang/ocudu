# `doc_chinese/macos_thread_priority/` —— macOS 上 PHY **关键线程的运行稳定性**工作流

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
| `wip/*.sh`、`wip/*.py` | ✅ **跟踪** | 腿配方与旁观脚本（它们产出证据，属于"人读了才懂"的东西）|
| `wip/logs/` | ❌ **忽略**（`doc_chinese/.gitignore` 的 `**/logs/`）| 本线新飞的腿（三件套：`.log` / `.log.stderr` / `.log.stdout`）|
| `work_tmp/` | ❌ **忽略**（`**/work_tmp/`）| `taskinfo`/`powermetrics`/`sample` 的采样文件、dump、scratch |

## 与本仓其他目录的关系

* `../phy_latency/`：上一段工作（PHY 端到端时延、ms 级停顿的收口、`OCUDU_UL_TIMING_EVENTS` 仪器）。**本线的引用来源**。
* `../phy_pipeline_gpu/`：更早的阶段（融合车道、门与腿脚本的**历史**位置）。
* `../macos_compat_refactor/`、`../test_environment/`：macOS 兼容层与台架的历史记录。

## 相关的树级文档

* **`../ocudu_env_knobs_inventory_and_leg_whitelist.md`**（生成物）：**整棵树**的 `OCUDU_*` 旋钮默认值/读取点/飞过的腿 + **验收腿白名单**。
  本线的任何新旋钮都必须在**改生成器**（`../phy_latency/wip/gen_knob_inventory.py`）之后重新生成它，并按需加入白名单。
* `../README.md`：`doc_chinese/` 的总索引。

## 引用规则（写文档时必须守）

1. **一台仪器两把钥匙**：编译期开关 + env，默认关；**关着时报告逐字节不变**。
2. **反向臂**：每个新字段都要有一条能把它打红的反向臂（纪律 79）。
3. **判据先登记再飞**，且**不追溯**已判的腿。
4. **Linux 逐字不变**：`#if defined(__APPLE__)` 之外零行为改动，`#else` 只写 no-op；Ubuntu 台架复核记进开发文档。
5. 引用"停顿/尾部"必须同时给 **`cpu`/`win`/`ivcsw`/`base_age` + 同秒的 `[RF]` 行**。
