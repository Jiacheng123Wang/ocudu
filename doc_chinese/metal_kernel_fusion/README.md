# `doc_chinese/metal_kernel_fusion/` —— GPU 融合 lane 的 **Metal kernel 融合**工作流

> **开工**：2026-10-06（用户立项）。**范围**：gpu（融合 lane）模式下，
> **从 DFT 输出的时频网格到 LLR** 这一段的 Metal kernel 融合 —— 今天由 **CE + 均衡 + 解调** 三个引擎、
> **4 次 dispatch / 2 个中间缓冲 / 3 个 metallib** 完成，目标是把它们**收敛成一个大 kernel**：
> **一跳一条命令、一次 dispatch、一个库**。
>
> **不做**：**Metal LDPC**（以后单开 ✗）、**Metal FFT/DFT**（前一条工作流证明它可 CPU 可 GPU、可 per-symbol 可 per-slot，
> 很灵活 ⇒ 不进本工作流 ✗）。

## 先读哪一份

| 想知道 | 读 |
|---|---|
| 为什么做、现状什么样、目标与判据、总体架构 | **`metal_kernel_fusion_high_level_status_and_plan.md`**（活文档，**先读这个**）|
| 具体怎么改、改哪些文件、每一步的判据、memo | **`metal_kernel_fusion_design_and_implementation.md`**（追加式，**活文档**）|
| 上一个会话干了什么、下一个会话怎么接手 | `session_handoff_<日期>-<序号>.md`（快照）|

## 一句话结论（**从上一工作流继承的最重要一条功课**）

> **瓶颈在 orchestration，不在算力。** 上一工作流的实测：一跳的全部工作量只有 ~85 µs/slot（duty 16–18 %），
> 而**每跨一次模块边界 ~340 µs** ✗、融合 lane 的一次提交 host 要站 ~680 µs ✗、Metal 解码每次 3.5 ms ✗、
> 设备占用率只有 **0.29 %** ✗。⇒ **减少边界比加快边界有效得多**，而"**融合成一条命令/一个 kernel**"就是这条功课的直接应用。

## 现在进展到哪（2026-10-06 末）

**M0 已结项** ✓（基线 + 消去三条腿 + 微基准 + M0b/M0c 两对 → 结论在架构文档 §2.0ter ✓）；
**M1.1 的 kernel 已完成并可编可打包** ✓，**主机侧接线未做** ✗ ⇒ 运行时还走不到它，
当前二进制与 M0 基线逐字节相同 ✓。
★ **接着开工就读 `session_handoff_2026-10-06-1.md`** ✓（含"第一件事"的两处落点与命令 ✓）。

## 腿的命名约定（用户 2026-10-06）：**单调递增序号 + 阶段 + 臂**

```
mkf<NNN>-<阶段><臂>            例如  mkf008-m0c-cpu / mkf009-m0c-metal
     │      └ m0/m0b/m0c = 里程碑；cpu/metal/… = 这一跳动的变量
     └ 本工作流的第 N 条腿（三位、从 001 起、只增不减 ✓）
```
**为什么**：腿是靠标签在几周后找回来的 ✓，而"阶段+臂"说不清**哪一条先飞** ✗ ⇒ 序号让标签同时是**记录里的位置** ✓。

**已飞的 7 条沿用旧标签（不改名 —— 飞过的标签是证据 ✓），但计入序号** ✓：

| 序号 | 标签 | 里程碑 / 变量 |
|---|---|---|
| mkf001 | `mkf-m0-base` | M0 基线（默认形状）|
| mkf002 | `mkf-m0-abl-eq` | 消去 eq 内核本体 |
| mkf003 | `mkf-m0-abl-demap` | 消去 demap 内核本体 |
| mkf004 | `mkf-m0-abl-ce` | 消去 ce 内核本体 |
| mkf005 | `mkf-m0b-event` | `OCUDU_CE_LANE_ORDER=event` |
| mkf006 | `mkf-m0b-burst` | `OCUDU_CE_LANE_ORDER=burst` |
| mkf007 | `mkf-m0c-dftmetal` | `--pusch_dft_type=metal`（腿作废 ✗）|
| mkf008 | （待飞）| `--pusch_dft_type=cpu`（对照）|
| mkf009 | （待飞）| `--pusch_dft_type=metal`（臂）|

**下一个序号不用记** ✓：`bash wip/next_leg_label.sh <后缀>` ⇒ 直接给出完整标签 ✓
（它从各工作流日志根里的腿日志自己数出来 ✓；只给数字就 `bash wip/next_leg_label.sh` ✓）。

## 目录约定（沿用前两条工作流 ✓）

| 路径 | 放什么 |
|---|---|
| `*.md`（本目录）| 架构（可重写）、实施（追加式）、会话交接（快照）|
| `wip/` | **本工作流新造**的工具脚本、判读工具、配置派生 |
| `wip/logs/` | **本工作流新飞的腿**的日志（`.log` / `.log.stderr` / `.protocol.txt` / `.driver.log`）|

## 复用而不是复制（前工作流的遗产）

* **runner 与验收闸门不搬家** ✗：`../phy_pipeline_gpu/wip/run_leg.sh`（唯一 runner）、
  `leg_gate.sh`、`milestone_audit.sh`（验收白名单）；本工作流的腿用 `LEG_LOGDIR` 指到 `wip/logs/` ✓。
* **判读工具直接用** ✓（都在 `../macos_thread_priority/wip/`）：
  `dl_gate.sh`（停顿闸门 + DL 判据）、`ul_health.sh`、`pair_check.sh`、`rtt_split.py`、
  `observe_threads.sh`（LIGHT）、`preflight_quiet.sh`、`probes_off_syntax_check.sh`、
  `../phy_latency/wip/gen_knob_inventory.py`（旋钮清单 + 白名单生成器）。
* **微基准直接用** ✓：`../macos_thread_priority/wip/sched_microbench/`。
* **纪律遗产**（比工具重要）：**一条腿一个变量** ✓、**每个数字都要能说出它的几何与口径** ✓、
  **默认构建（probes-OFF）也是被测对象** ✓。
* ★ **本工作流的判据取向（用户 2026-10-06 裁决）**：**功能性（CRC-OK 同一档）+ 结构性（dispatch/库的收敛）为主** ✓；
  **性能只报告、不设闸** ✓（允许持平甚至小幅回退 ✓ —— 本工作流是为 Apple Silicon 异构 gNB 打基础 ✓）；
  **LLR 逐位一致不要求** ✗（只作为出问题时的定位工具 ✓）；
  **环境不是起飞条件** ✗（抗干扰本身是健壮性 ✓；`preflight_quiet.sh` 与 `dl_gate.sh` 都只是判读参考 ✓）。

## 本工作流的腿怎么飞（与上一工作流同一套）

```bash
cd /Users/jiachengwang/dev/ocudu
sudo -E env LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs \
     EXTRA_KNOBS="<本工作流的开关>" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> --regime=default
# 判读：功能（CRC）与结构（dispatch/库）为主 ✓；时延数字要引用时才过闸门 ✓
bash doc_chinese/macos_thread_priority/wip/ul_health.sh <label>
bash doc_chinese/macos_thread_priority/wip/dl_gate.sh <label>   # 参考项：只在引用时延数字时需要 PASS
```
（或沿用 `../macos_thread_priority/wip/fly_leg.sh` 的包装 + `LEG_CFG`/`LEG_LOGDIR` ✓，
但**它默认的 KNOBS 集是上一工作流的** ⇒ 用它的腿要在文档里写清携带了哪些旋钮 ✓。）
