# `doc_chinese/phy_latency/wip/` —— **本阶段（时延优化）的工具**

> 2026-09-25 用户要求按**开发阶段**分目录：新工具放**本阶段自己的** `wip/`，
> 而不是继续堆进上一阶段（融合车道建设）的 `doc_chinese/phy_pipeline_gpu/wip/`。

## 三个目录，各管什么（别混）

| 目录 | 内容 | 进 git？ |
|---|---|---|
| **`doc_chinese/phy_latency/wip/`**（本目录）| **时延阶段自己的工具**：`p0_gate.sh`（P0 读数门：P0-6/P0-1/P0-5 的 A1/A2/B1/B2/C1/**C2/C2b**）、`leg_census.py`（腿普查：UL 静默表 + RF/pool 事件）| ✅ 进 |
| `doc_chinese/phy_pipeline_gpu/wip/` | **上一阶段的门与工具，仍然在用**：`run_leg.sh`（起腿）、`milestone_audit.sh`（里程碑门）、`leg_gate.sh`、`ab_dumps.sh`、`value_net.py`、`l1_*_arms.sh`、`edge_block_arms.sh`、`ul_load.sh`、`wall_ab.sh`，以及**腿日志** `logs/` | ✅ 进 |
| `doc_chinese/work_tmp/` | **跑出来要看的东西**（git 忽略）：语料 `corpus/`、归档基线 `determinism/`、本阶段的 `p0_7/`（普查输出、原始记录）、`ref/`（一次性二进制）、dump 比对；**`/tmp` 不可靠**（macOS 重启清掉），需要留存的一律放这里 | ❌ 不进 |

**规矩**：**门与判据只许依赖被跟踪的文件**（所以门放 `wip/`，不放 `work_tmp/`）；
`work_tmp/` 里的东西可以被删而**不破坏任何判据**。

## 本阶段的工具

```bash
# P0 读数门（只读日志，不碰 GPU —— 用户飞腿时可以安全跑）
bash doc_chinese/phy_latency/wip/p0_gate.sh <腿> [--vs=<出厂臂>]

# 腿普查（UL 静默 + RF/pool 事件；停顿时段的定位）
python3 doc_chinese/phy_latency/wip/leg_census.py doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_<腿>.log
```

起腿、里程碑门、离线网仍在 `doc_chinese/phy_pipeline_gpu/wip/`（见上表）。
