# `history/` —— 一次性与历史脚本（**保留，不删**）

这里的脚本是**当时**为了把某批语料/某个实验推过去而写的一次性驱动：`push_*_chain.sh`（按链路推采集）、
`ship_full_w2.sh`（整批打包外送）、`dsh_*.sh`（DSH 远程巡检）、`worker*.sh`/`orch_leg.sh`（并行 worker）、
`final_queue2_phase2.sh`（收尾队列）、`backup_s2_to_211.sh`、`copy_legs_1028fg.sh`、`prune_mac_full.sh`、
`monitor_builds.sh`、`tm_monitor.sh`、以及 `archive_legs2_old.sh` 与被替换掉的 `README_tools_old.md`。

**为什么留着**：它们记录了"当时那批数据是怎么被搬/被建/被核对的"，出问题时是唯一线索；
**为什么不放进现役目录**：硬编码了当时的腿名、IP、目录布局，直接复用会踩坑。

★ 要复用其中的**做法**，先看 `../corpus_pipeline/` 与 `../dataset_build/` 里的现役版本（已参数化、已自校验）。
