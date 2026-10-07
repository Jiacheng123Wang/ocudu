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
| 具体怎么改、改哪些文件、每一步的判据、**下一步待飞什么**、未决项、memo | **`metal_kernel_fusion_design_and_implementation.md`**（追加式，**活文档**）|
| 上一个会话干了什么、怎么接手 | `session_handoff_<日期>-<序号>.md`（快照）|

★ **`session_handoff_*.md` 只在"准备开新会话"时写** ✓（用户 2026-10-06 明确 ✓）：
同一条对话里继续干活时，结论/现状/下一步/未决项**都写进上面两份活文档** ✓（下一步见实施文档 §2.11 ✓、未决项见 §2.12 ✓）
—— 快照是**一次性**的 ✓，写早了既会过时、又会让同一件事有两个出处 ✗。

## 一句话结论（**从上一工作流继承的最重要一条功课**）

> **瓶颈在 orchestration，不在算力。** 上一工作流的实测：一跳的全部工作量只有 ~85 µs/slot（duty 16–18 %），
> 而**每跨一次模块边界 ~340 µs** ✗、融合 lane 的一次提交 host 要站 ~680 µs ✗、Metal 解码每次 3.5 ms ✗、
> 设备占用率只有 **0.29 %** ✗。⇒ **减少边界比加快边界有效得多**，而"**融合成一条命令/一个 kernel**"就是这条功课的直接应用。

## 现在进展到哪（2026-10-06 末，第 2 次）

★ **M1 已结项（2026-10-06）** ✓✓ —— 融合兑现收益，且在**交付配置**上复现：

| 腿对 | 载荷 | dispatch/跳 | 融合覆盖 | `busy` median | `ul_pipeline` / `ul_gpu_pipeline` median | CRC（对照 / 臂）|
|---|---|---|---|---|---|---|
| `mkf012`/`mkf013` ✓ | 16QAM ✓ | **4.0000 → 3.0000** ✓ | **99.997 %** ✓ | 478.9 → **469.6** ✓ | 1280 → **1259** / 1237 → **1214** ✓ | 100.0 % / 99.9 % ✓ |
| `mkf014`/`mkf015` ✓ | **64QAM（交付 ✓）** | **4.0000 → 3.0000** ✓ | **99.997 %** ✓ | 479.9 → **472.5** ✓ | 1320 → **1304** / 1242 → **1227** ✓ | 98.8 % / **99.9 %** ✓ |

两对的 `pair_check` 都 **PASS** ✓、`gaps=0` ✓、`stale=0`（B 路 ✓）；收益**不随"哪条腿台面更好"翻转** ✓
⇒ 不是环境假象 ✓。✗ **没有动到的**：那 ~380 µs 的"信封"（CB 仍是 **2.00/跳** ✗）
—— 融合只去掉了同一条 CB 里的一个 dispatch ⇒ 下一步是 **M2（折 CE）/ 另一条 CB 路线** ✓。
细节与保留项（B 路 p99.9 +36 µs ✗）见实施文档 memo ✓ 与 §2.12 ✓。
**生效形状** ✓：**1 层 + QPSK/16QAM/64QAM + 无 EVM** ✓（其余自动走两步路线 ✓，日志点名 ✓）；
**QPSK 是 2026-10-07 为 AMC 腿补的** ✓（离线三份语料 × 27 capture 全 0/0/0/0 ✓，实施文档 §2.15 ✓）。
开关：`OCUDU_LANE_FUSE_EQDEMOD`（env ✓）—— ★ **默认开** ✓（用户 2026-10-07 裁决 ✓，两对腿为据 ✓）；
**`=0` 是回退开关** ✓（A/B 时"非空即开、默认关"的旧形状已作废 ✓）。
**M3 也已完成** ✓：三个 metallib → **一个** `ocudu_lane.metallib` ✓（实施文档 §4 ✓）。

★ **接着开工就读 `session_handoff_2026-10-07-1.md`** ✓（2026-10-07 第 1 次快照 ✓：
M1 结项 ✓、M3 完成 ✓、默认翻开关 ✓、AMC 覆盖率 ✓、信封四假设证否 ✓），
★ 但**要连同实施文档 §2.16 一起读** ✓ —— handoff 里那两条已被 B′ 更正 ✗：
① "并发 1 是长尾杠杆（−24 %）" ✗ **方向反了** ✓（对照臂 `mkf021` 自报并发 **2** ✗）；
② `nv` 的 A/B ✗ **不值得飞** ✓（收益上界 ~1 µs ✓）。

★★ **B′ 判读（2026-10-07，未飞腿 ✓，实施文档 §2.16 ✓）**：
① ✗ **`nv` 输出不飞** ✓ —— 收益上界 **~1 µs**（两种算法 ✓）落在噪声底以下 ✗；
② ✗✗ **并发 1 全面更差、不进交付配置** ✗ —— 改正对照后 **p50 +18.5 %、p99 +17 %、
超预算窗口 27–39 → 318（×10）** ✗，唯一真变好的是**设备侧 `busy` −8.4 %** ✓
（机制 = `max_pusch_and_srs_concurrency<=1` 是 **STRAND** ✓ ⇒ 车道串行 ⇒ 线程停摆 ⇒
`ul_to_lane` **22 → 351 µs** ✗）。★ 教训：**"旋钮没进进程"与"没效果"在日志里一样** ✓
—— 这条纪律**同样要用在对照臂上** ✗。

★ **归因（`mkf018`/`mkf019`，2026-10-07）** ✓：空口拿掉每跳那道 stage 等待后**什么都不动** ✓
（`busy` 492.2 → 492.0、`merged_hop` 455.9 → 455.5、CRC 甚至没塌 ✓ ⇒ **是保险不是成本** ✓）
⇒ **M2 的前提被证否** ✗；归因进入下一层（一跳 ~6.9 个 dispatch ⇒ ≈66 µs/dispatch，是离线微基准的 17 倍 ✗）。
★ **AMC 一对（`mkf016`/`mkf017`，2026-10-07）** ✓：覆盖率**精确闭合** ✓✓ ——
`y_fused = 103 549` 正好等于 QPSK+16QAM+64QAM 的跳数 ✓、`y_batch = 5 082` 正好等于 256QAM 跳数 ✓
⇒ **逐跳路由在 AMC 下按设计工作** ✓、覆盖率 **95.32 %** ✓、QPSK 新分支在空口上跑了 8 378 跳 ✓。
**但这一对判不了性能/功能** ✗（两条腿信道不同 ⇒ 负载差 ~11 % ✓；CRC 95.0 %/90.0 % 都 DEGRADED ✗；
`pair_check` B/hop −27 % ✗）⇒ 性能证据仍是**钉住 MCS 的那两对** ✓。
★ **更正** ✓：原来写的"对照腿一次 **59.5 ms 停顿**"✗ **不存在** ✓ —— 那是**子串误读** ✗
（`mkf016` 里的两处是 `159.5us` ✓、`mkf012` 里是窗口读数 `…459.5` ✓、`mkf022` 里是 `2659.5us` ✓）。
**判读时长类数字要连单位一起匹配** ✓，别用裸数字 grep ✗。

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
| mkf008 | `mkf008-m0c-cpu` | `--pusch_dft_type=cpu`（对照）|
| mkf009 | `mkf009-m0c-metal` | `--pusch_dft_type=metal`（臂）|
| mkf010 | `mkf010-m1-fused-off` | **M1.2 对照**：默认形状（融合关 ✓）—— 飞完但**腿是 64QAM** ✗ ⇒ 判不了融合 ✗ |
| mkf011 | `mkf011-m1-fused-on` | **M1.2 臂**：`OCUDU_LANE_FUSE_EQDEMOD=1` ✓ —— 同上（融合只覆盖 10 跳 ✗）|
| mkf012 | `mkf012-m1-16qam-fused-off` | **M1.3 对照**：`wip/gnb_mcs16qam.yml`（UL 表换成 qam64 ⇒ MCS 13 = 16QAM ✓），融合关 ✓ |
| mkf013 | `mkf013-m1-16qam-fused-on` | **M1.3 臂**：同一 config ✓ + `OCUDU_LANE_FUSE_EQDEMOD=1` ✓ |
| mkf014 | `mkf014-m1-64qam-fused-off` | **M1.5 对照**：**交付配置**（`gnb_pinned_mcs13.yml` ✓ = 64QAM ✓），融合关 ✓ |
| mkf015 | `mkf015-m1-64qam-fused-on` | **M1.5 臂**：同一 config ✓ + `OCUDU_LANE_FUSE_EQDEMOD=1` ✓ |

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
