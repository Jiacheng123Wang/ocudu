# Session handoff — 2026-09-24 #2

> 本工作流（GPU PHY 融合车道**时延**）的**会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本会话（09-24 开始）的交接是 **`-09-24-2`**；
> **`session_handoff_2026-09-25-1.md` 是下一个会话的第一份**（现在**不该存在**）。
>
> **细节一律在开发文档**：`gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"，
> 它已并入原 `00_status.md` … `05_p2e_platform_finding.md`）；**高层现状与计划**在 `high_level_status_and_plan.md`；
> 三分工与旧名映射见 `README.md`。**本 memo 只写"开工要什么"，不复制读数**。

## 0. 一句话现状

**P0 仪表全部完成**（P0-5 探针配对已交付，提交 `747b9d475b`）；
**P2-E（输入缓冲寿命解耦）机制已按计划实现（默认关），但被平台证伪**（macOS 26.6.2 只在命令缓冲完成时
发布"中途"事件信号）⇒ **等用户裁决**；
**P1-8（车道并发度 1→2）是实测杠杆**（`ce` −58~64×、跨度 −55%，两次复现），
**代价是一次 ~5 秒收包停顿（两次复现）⇒ 只能算测量结论**。
开发文档 §6.3/§6.4/§6.5/§7.5/§8 是本段结论的出处。

## 1. 本会话（#1 之后）做了什么

| 事 | 结果 | 出处 |
|---|---|---|
| **P0-5** 相位探针 ↔ 车道探针按 **slot** 配对 | ✅ 提交 `747b9d475b`：报告新增 `paired …`/`paired ratios`/`paired reading`，`p0_gate.sh` 新增 **C2**；离线全绿（`ctest -L phy` 193/193、与 pristine HEAD 二进制逐字节相同）| 开发文档 §6.3 |
| **P2-E** E-1/E-2（`OCUDU_DFT_RELEASE_TOKENS_EARLY`，默认关，带 `tokens_early=` 计数器）| ⛔ **实现完成、平台证伪**（三种测法 + 对照；单测每次运行都实测该前提）；提交 `7c40327f81` | 开发文档 §6.4 |
| **文档整理**（用户要求）| 本目录分三类；`00–05` 的内容**整体并入开发文档**（旧文件删除）；新增 `high_level_status_and_plan.md`；本 memo 取代被并入的 `-09-25-1` | `README.md`、设计文档 §5.9.141 |

**本会话没有飞任何腿**（用户未飞；P2-E 的反向臂**不要**照原计划去飞——机制不动持有期，飞了也判不出因果）。

## 2. 仓库/构建状态

分支 **`apple-silicon`**，本会话的提交都已推送（`747b9d475b` P0-5、`7c40327f81` P2-E、`3a7cf5a391` 文档整理 + 本 memo）；
工作区干净。⚠ 起腿前照旧 **`touch build/hashes.h && cmake --build build --target gnb`**（`run_leg.sh` 硬检查戳）；
⚠ `.metallib` 是构建产物、被 git 忽略（本机 8 个时间戳 9-24 14:51，这条是 `value_net` 基线陈旧的证据链）。细节：开发文档 §5.3。

## 3. 下一步（按优先级）

1. **飞 P0-5 的那条腿**（约 2 分钟；n1 默认配方 + 标准流量）：
   `sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>`，
   然后 `bash doc_chinese/phy_pipeline_gpu/wip/p0_gate.sh <label>` 读 **C2**（配对 n == 相位 n）与 `paired ratios/reading`。
   判读先写死：C2 绿 ⇒ D 项预算的分母可用；C2 红 ⇒ 按配对行的分项（`no lane for the slot` / `lane older than 2s` / `awaiting`）查前提。
2. **等用户裁决 P2-E**（开发文档 §6.4.4 的选项表）：建议保留开关（默认关）作仪器，然后二选一——
   **接受 +1 提交**把前端块做成独立提交（V4 需重裁），或转 **P2-D**（P0-5 的配对分布正好是它的输入）。
3. **查那次 ~5 秒收包停顿**（开发文档 §8 **Q9**，本线最大未决项）：下一个嫌疑是"同一 strand 上的邻居任务 /
   池与解码线程的互动"，或与并发度无关的第三个因。
4. **P2-F（并发度作为交付）需用户二次裁决**；两条**改前就红**的网（`value_net` 基线陈旧、`ab_dumps` arm1）待裁决（§8 Q11）。
5. `s84b-p0` 第一次尝试没有日志（**未验证**，最可能被"戳 ≠ HEAD"拒绝）——低优先，要它作证据就重飞（§8 Q12）。

## 4. 纪律（照旧；全文见开发文档 §5.2）

* 起腿前 `pgrep -x gnb` / `pgrep -x ul_chain_replay` / `lsof -nP -iUDP:2152`；**用户飞腿时不要碰 GPU**；进程检查**按名字**。
* **判据有三个绑定维度**（工况 / 流量 / 几何）；**零流量的腿无效**；**不许为过关改阈值**；"读不出"按 RED。
* 代码改动 ⇒ 腿的"提交证据"失效 ⇒ 之后要重新飞腿；`leg_gate.sh` 只用于加压腿。
* 新的会话记录写成**新的** `session_handoff_<会话开始日>-<序号>.md`；**结论与证据要并进开发文档**（memo 只是快照）。

## 5. 读数去哪找

* **开发文档 §9 证据索引**（腿 → 用途）：`s78/s80/s82`（现象）、`s82`（三段基线）、`s87/s88/s88b`（n1 与并发度）、
  `s85`（P0-5 立项）、`s86`（P0-1 空口）、`s84-p0`（P0-6 空口）。
* 离线载体：`dft_release_adopt_metal_test`（P0-1/P2-E）、`ul_pipeline_probe_test`（P0-5 hook 契约）、
  `du_low_executor_mapper_test`（P0-6 规则）、`ul_chain_replay`（逐字节/容差网）。
* 门与工具：`phy_pipeline_gpu/wip/`（`p0_gate.sh` / `leg_gate.sh` / `milestone_audit.sh` / `run_leg.sh` / `ab_dumps.sh` / `value_net.py` / `l1_*_arms.sh` / `edge_block_arms.sh`）。
