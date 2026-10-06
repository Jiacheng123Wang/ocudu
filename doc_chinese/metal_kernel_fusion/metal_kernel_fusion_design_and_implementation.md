# Metal kernel 融合 —— 实施设计与步骤（活文档，追加式）

> **用法**：这是**动手改代码的人**的文档 ✓。每一段实作、每一次飞行、每一个决策都**追加**在这里
> （更正写在后面，不改历史 ✓）。**结构性调整**回写到
> `metal_kernel_fusion_high_level_status_and_plan.md`（架构文档，可重写）✓。
>
> **上游功课**：`../macos_thread_priority/workstream_summary_and_lessons.md` §4bis、§7 ——
> "**减少边界比加快边界有效**"、"每跨一次模块边界 ~340 µs ✗"、"设备占用 0.29 % ✗"。

---

## 0. 不变式（每一步都要满足，抄自架构文档 §2.2）

1. ★ **链路仍然工作** ✓：CRC-OK 率与对照腿**同一档** ✓（PHY 口径）；`ping`/`iperf3` 表现**记录**在案 ✓
   （应用层口径，**不做硬性要求** ✓）—— **不要求 LLR 逐位一致** ✗（用户 2026-10-06 裁决：允许计算顺序变化、
   浮点舍入差异，甚至为融合收益做简单算法微调 ✓；**也允许一定程度的性能回退** ✓，只要对最终目标有益 ✓）；
2. `gaps=0` ✓（腿的有效性 ✓ —— 电台样本流必须连续 ✓）；
3. **Linux 行为一字不变** ✓（平台守卫内 ✓）；
4. **关着开关逐字节不变** ✓（两把钥匙：编译开关 + env ✓）；
5. **默认构建（probes-OFF）能编** ✓ ⇒ 动了探针就重跑 `probes_off_syntax_check.sh` ✓。

---

## 1. M0：基线与成本分解（**先量再改** ✓）

### 1.1 要量清的三件事

| 量 | 为什么 | 手段 |
|---|---|---|
| **每次 dispatch 的固定成本** | 融合的**主要收益假设**（H1 ✓）—— 今天只有 1 条命令/跳 ✓，"4→1"省的是 **GPU 侧 launch/barrier**，不是 host 提交 ✗ | ① `[metal_stats] burst … dispatches` 与 `[ul_gpu_lane] busy` 的对照 ✓；② **消去法**：`OCUDU_LANE_ABLATE_STAGE=eq` / `=demap` / `=ce` 逐阶段消去，看设备时间怎么掉 ✓（`_EVERY=8` 用于保持链路可用 ✓）；③ 若需要，写一个最小微基准（多 dispatch vs 单 dispatch 同工作量 ✓）|
| **中间缓冲的代价** | H2 ✓ | `[metal_stats]` 的 `staged=` / `device=` / `host=` 计数 ✓；消去法下设备时间变化 ✓ |
| **CE 值不值得折进来** | M2 的优先级（它只占设备时间 **8 %** ✓）| 消去 `ce` 阶段 ⇒ 上限就是它 ✓ |

### 1.2 一条对照腿（今天的形状）

```bash
cd /Users/jiachengwang/dev/ocudu
# （可选参考，不是起飞条件 ✗）宿主扰动风险：bash doc_chinese/macos_thread_priority/wip/preflight_quiet.sh
sudo -E env LEG_CFG=$PWD/doc_chinese/macos_thread_priority/wip/gnb_pinned_mcs13.yml \
     LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs \
  bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf-m0-base dual quiet gpu
# 业务：在 TRAFFIC NOW 提示时，在 CORE 侧跑标准的 iperf3（密集 ⇒ 跳数多 ⇒ 设备时间统计有效 ✓）：
#       iperf3 -u -b 30M -l 1400 -R -P 4 -t 180 -c <server>
# ping 尾巴可留到需要引用时延时再测 ✓
```
**登记读数**（写进 §5 memo ✓）：`[ul_pipeline]` / `[ul_gpu_pipeline]` 的 median/p95/p99 + `[ul_tail]` 计数 ✓、
`[ul_gpu_lane] busy` 与 busy split ✓、`[metal_stats] burst`（commits/dispatches ✓）、
`[metal_stats] mmse_ce` ✓、健康 + `dl_gate.sh` ✓。
★ 业务要**与判读一致**：ping（稀疏，看尾 ✓）与 iperf3（密集，看体 ✓）各一条更稳 ✓ —— 但一次只动一个变量 ✓。

### 1.3 M0 的出口

* 一条**可引用**的基线 ✓（闸门 PASS + 健康 CLEAN ✓）；
* 一张"**成本分解表**"：`merged_hop` = 383 µs 里，dispatch 固定成本 / 读 y / 均衡 / 解调 / 写 LLR 各占多少 ✓；
* 结论：**M1 的理论上限**（若上限 < 5 % ⇒ 诚实收工 ✗✓，这也是允许的结论 ✓）。

### 1.4 （可选）逐位对拍：**定位差异的工具**，不是验收红线 ✓

> ★ 用户 2026-10-06 裁决：**不要求 LLR 与今天逐位一致** ✗ —— 融合可能改变计算顺序、引入浮点舍入差异，
> 也可能为融合收益做简单算法微调 ✓；判据是 **CRC-OK 同一档 + `gaps=0`** ✓（架构文档 §2.1bis ✓）。
> 下面这套工具因此**只在出问题时用来定位** ✓（"差异出现在哪一段"），**不是每一步都要跑** ✓。


融合是**替换实现**（不是加一条可选路径 ✓）⇒ **必须用两个二进制的对拍** ✓：

```bash
# 旧二进制留一份
cp build/lib/phy/upper/channel_processors/metal/ul_chain_replay /tmp/replay_before
# 改完再编一份，然后：
bash doc_chinese/phy_pipeline_gpu/wip/ab_replay_bins.sh /tmp/replay_before \
     build/lib/phy/upper/channel_processors/metal/ul_chain_replay
# 出口：每个 capture 两边都出 dump，且四个 dump 逐字节相同 ✓
```
✗ **不要**用 `ab_dumps.sh`（它比的是**同一个二进制**的两个环境 ⇒ 两臂跑的都是新代码，
会"互相一致但与出厂不一致" ✗✓ —— 这正是 `ab_replay_bins.sh` 头部记的那次教训 ✓）。
相关按键：`OCUDU_UL_CAPTURE`（编译期抓取 ✓）+ `ul_chain_replay`（重放二进制 ✓）。

---

## 2. M1：融合 **均衡 + 解调**（2 dispatch → 1，去两份中间缓冲）

### 2.1 为什么先做这一步

* 它占设备时间的 **92 %** ✓（`merged_hop` 383 µs vs `ch_wt` 31 µs ✓）；
* **几何天然一致** ✓✓：两个引擎都是"一 RE 一线程"、256/组、`(nof_re, nof_symbols)` ✓
  ⇒ 同一个线程里顺序做两段，**不需要 barrier、不需要 threadgroup memory** ✓；
* 收益确定（H1+H2+H3 ✓），风险最低 ✓。

### 2.2 具体改动（预计落点，动手时校正 ✓）

| 文件 | 改动 |
|---|---|
| **新** `lib/phy/channel_processors/metal/ocudu_lane_fused.metal`（或放在 equalizer 的 metal 目录，动手时定 ✓）| 新 kernel `lane_grid_to_llr`：入参 = 网格、权重、噪声方差、参数结构；出参 = LLR ✓ |
| `ocudu_equalizer_metal_engine.mm` / `ocudu_demod_metal_engine.mm` | 抽出一个"两段共用"的编码路径：融合开着时，把权重/噪声绑好、**只编一次 dispatch** ✓ |
| 引擎的 pipeline 加载 | 新 kernel 的 `newComputePipelineStateWithFunction` ✓ |
| 开关 | **`OCUDU_LANE_FUSE_EQDEMOD`**（env，默认 **关** ✓，关着逐字节走今天两条 dispatch ✓）|
| 探针 | `[metal_stats] burst dispatches` 应降到 **2/跳** ✓（M1）→ **1/跳** ✓（M2）；可加一个"融合了几跳"的计数便于自证 ✓ |

### 2.3 M1 的判据（预登记）

| 项 | 出口 |
|---|---|
| G1 | `dispatches`/跳 = **2**（M1）/ **1**（M2）✓ |
| G4 | `[ul_gpu_lane] busy` 的 `merged_hop` —— **记录并报告** ✓（目标：不升；小幅上升可接受 ✓）|
| G5 | `[ul_gpu_pipeline]`/`[ul_pipeline]` median/p95/p99 —— **记录并报告** ✓（目标：改善；回退要写进 memo 并说明是否值得 ✓）|
| 硬判据 | **CRC-OK 与对照腿同一档** ✓、**`gaps=0`** ✓、G1（2→1 ✓）✓ |
| 记录项 | G4/G5 的融合前后对照 ✓（**允许持平或小幅回退** ✓）；出问题时用 §1.4 的两二进制对拍定位 ✓ |

---

## 3. M2：把 **CE 的权重**折进同一 kernel（**判据驱动，可以不做** ✓）

* 前置：M0 显示 `ch_wt`（8 % 设备时间 ✓）**或**它带来的中间往返值得 ✗✓；
* 难点：CE 需要**跨线程协作**（每 RB/组的自相关 + 求逆 ✓）⇒ threadgroup memory + `threadgroup_barrier` ✓，
  或"一个线程组负责一个 RB 组 × 若干符号"的映射 ✓；
* 风险：寄存器/共享内存压力可能让 occupancy 变差 ✗ ⇒ **先做一版只折权重计算的 kernel 试点** ✓，
  与 M1 的形状**分开关**（`OCUDU_LANE_FUSE_CE` ✓），这样能单独判它 ✓；
* 出口：`dispatches`/跳 = **1** ✓、CRC-OK 同一档 ✓、`gaps=0` ✓；G4/G5 记录在案 ✓。

---

## 4. M3：metallib 整合（**独立于 kernel 融合，可先做** ✓）

* 目标：CE / EQ / DEMOD 三个库 → **一个** `ocudu_lane.metallib` ✓；
* 落点：三个 metal 目录的 `CMakeLists.txt`（`ocudu_add_metallib` ✓）合并为一个 target ✓，
  三个 `OCUDU_*_METALLIB_PATH` 收敛为一个 ✓，引擎侧**共享一次加载**（device/queue/library ✓）；
* **它不改数据路径** ✗ ⇒ 不进 G5 ✓，只算工程整洁（G3 ✓）与初始化简化 ✓；
* 出口：G3 ✓、启动横幅与探针无回退 ✓、Linux 不变 ✓、probes-OFF 可编 ✓。

---

## 5. Memo 区（**每次实作/飞行/决策追加** ✓）

> 格式建议（照抄上一工作流的习惯 ✓）：**日期 · 目的 · 变量 · 腿号 · 读数 · 结论 · 下一步**，
> 并注明**闸门与健康** ✓；作废的腿也记（写清为什么作废 ✗），**不删** ✓。

### 2026-10-06 · 立项

* 用户立项：gpu 融合 lane 的 **DFT 网格 → LLR** 段做 kernel 融合；**不含** LDPC 与 FFT ✓；
* 现状调研（代码 + 上一工作流实测）见架构文档 §1：**一跳 1 条命令 / 4 次 dispatch / 3 个 metallib** ✓，
  设备时间 `merged_hop` 383 µs（92 %）+ `ch_wt` 31 µs（8 %）✓，**两段几何完全一致** ✓；
* 判据预登记：G1–G5 与红线（架构文档 §2 ✓）；
* 下一步：**M0 基线 + 成本分解**（§1 ✓），出口是一条可引用基线与一张成本分解表 ✓。

### 2026-10-06 · ★ M0 基线到手：腿 `mkf-m0-base`（gpu + dual + n78 + iperf3）

**判读** ✓：`ul_health.sh` **CLEAN**（CRC steady **99.1 %**、retx 1698/107280 ✓）、
`dl_gate.sh` **PASS**（frontier max 1333 µs、DL 迟到 3 ✓）、`gaps=0` ✓、`[ul_rx_pool] held_end=0` ✓。
★ 起飞时 `preflight_quiet.sh` 为 **NO-GO** ✗（`photoanalysisd` 11.3 %、4 卷仍索引）而腿仍 PASS ✓
⇒ **"环境不是起飞条件"得到实测支持** ✓（判据取向见架构文档 §2.3 ✓）。

**结构基线（G1/G2 的起点）** ✓：
```
[metal_stats] burst commits=108978 waits=108978 max_in_flight=1
              dispatches=435912 (equalizer=108978 demapper=108978 channel_estimator=217956)   ⇒ 4.00 dispatch/跳 ✓
[metal_stats] mmse_ce commits=108982 waits=4                                                    ⇒ CE fire-and-forget ✓
mode options : <none>   （gpu 模式自解析后端 ✓ = 融合 lane 本人 ✓）
```
**设备时间基线（G4 的起点）** ✓：`[ul_gpu_lane] busy` median **479.6 µs**；
split = `merged_hop` **465.6 µs（93 %）** + `ch_wt` **36.9 µs（7 %）** ✓。
**性能基线（G5，只记录 ✓）**：`[ul_pipeline]` median 1321 / p95 1557 / p99 1633 / max 4463 µs；
`[ul_gpu_pipeline]` median 1242.8 / p95 1464.6 / p99 1532.2 µs；
`[ul_tail]` p99.9 1925 µs、≥1500 µs 计数 10099、≥2000 µs 计数 73 ✓；
`[ul_handoff] ul_to_lane` p50 22 µs ✓；rx duty 19.2 % ✓；`[ul_ldpc_decode]` median 74 µs ✓（CPU 解码器 ✓）。
**与上一工作流 p305 逐项吻合** ✓（1321 vs 1319、479.6 vs 480.7、465.6/36.9 vs 469/37 ✓）⇒ 基线可信 ✓。

**M0 还差一项**（成本分解 ✓）："每次 dispatch 的固定成本"**消去法量不出来** ✗
（`OCUDU_LANE_ABLATE*` 只换 kernel、**不改提交/屏障结构** ✓）⇒ 需要一个**微基准**
（N 次空 kernel vs 1 次做 N 倍活的 kernel，同一 device/queue ✓）。
上一工作流的先验估计可作参照 ✓：Metal 解码器侧实测 **每次 dispatch ≈4.3 µs、每轮 barrier ≈8.7 µs** ✓
（`full_chain_gpu_uma_zero_copy_refactor_plan.md` ✓）⇒ **量级在个位数微秒** ✓，
相对一跳 480 µs 是 **几 %** ✗ ⇒ **M1 的收益主要应来自中间缓冲与 kernel 内部复用，而不是 dispatch 计数本身** ⚠
（这条判断若被微基准推翻，M1 的优先级要重排 ✓）。

### 2026-10-06 · ★★★ M0 第二半：消去三条腿 + 微基准 = **H1 被推翻，靶子改成命令缓冲/栅栏** ✓✓

**消去三条腿**（各一条，`OCUDU_LANE_ABLATE=1 OCUDU_LANE_ABLATE_STAGE=<stage> OCUDU_LANE_ABLATE_EVERY=8` ✓）：

| 腿 | CRC steady | `[ul_gpu_lane] busy` median | merged_hop | ch_wt |
|---|---|---|---|---|
| `mkf-m0-base`（基线）| 99.1 % ✓ | **479.6 µs** | 465.6 | 36.9 |
| `mkf-m0-abl-eq` | **92.7 %** ✗ | **477.4** | 457.6 | 37.4 |
| `mkf-m0-abl-demap` | 98.6 % | **479.7** | 465.7 | 37.0 |
| `mkf-m0-abl-ce` | 97.8 % | **480.0** | 466.8 | 37.2 |

★ **消去确实生效** ✓（三条腿的 CRC 都掉了 ✓，busy split 里被消去的族读数变成 `0.0us/lane (0%)` ✓）
—— 但 **busy 一条都没掉** ✗✓ ⇒ **一跳的内核工作量 < 1 %** ✗✓✓。

**微基准**（`wip/dispatch_cost_probe/` ✓，独立 Metal 程序、不占电台 ✓）：每次 dispatch 边际 **~3.7 µs** ✓
（⇒ 4→1 只值 ~11 µs ✗）；一次 dispatch 做 16 倍活只 **+2 µs** ✓；宿主成本由**命令缓冲数**决定（1 CB 恒定 ~88–106 µs ✓
vs N CB 84→256 µs ✗）；网格 8.5k→1M 线程只从 6.6 到 14.5 µs ✓。

**一跳 480 µs 的账**（M0 基线，全部实测 ✓）：
```
busy 479.6 µs  =  host 编码 83.2 µs  +  dispatch 4×3.7 ≈ 15 µs  +  内核本体 <5 µs  +  ~380 µs ✗ 未解释
                 未解释的那部分 = 2 条命令缓冲 + 二者之间的 stage fence + 提交/完成信封
                 （证据：commit order commits/跳 = 2.00 ✗；lane fence signals=217956 waits=108978 ✗；
                   而设备几乎闲着 ✓ —— 微基准里一次 trivial CB 只要 2–7 µs ✓）
```
⇒ **靶子改写** ✓：不是 dispatch 计数 ✗，而是 **`cbs/跳` 2 → 1 并去掉跨 CB 的 fence** ✓✓
（内核融合的真正价值就在这里 ✓：依赖变成 kernel 内部的数据依赖 ✓）；
⚠ 代价是失去"估计器与 host 编码重叠" ✗（代码注释记的历史值 ~125 µs 延迟债 ✗，但那对有混淆 ✗，
且登记过的"同负载背靠背一对"从未飞 ✓）。

**下一对（M0b，一个变量，两个现成值）** ✓：
```bash
# 对照 = 今天实际走的路线（由 cbs/跳=2.00 与 fence 计数证明 ✓）
EXTRA_KNOBS="OCUDU_CE_LANE_ORDER=event"   →  label mkf-m0b-event
# 臂 = 单命令缓冲（估计器派发搭车道的共享 CB）
EXTRA_KNOBS="OCUDU_CE_LANE_ORDER=burst"   →  label mkf-m0b-burst
```
**预登记**：臂的 `commit order commits/跳` 应 = **1.00** ✓、`lane fence … signals` 应 ≈ **0–1/跳** ✓；
`busy` 与 `[ul_pipeline]` 的走向**就是"第二条 CB + 栅栏"的价格** ✓（正负都算答案 ✓：
若 busy 降 ⇒ 信封是主要成本 ✓ ⇒ M1 按"单 CB 单 kernel"推进 ✓；若 busy 升 ⇒ 重叠被牺牲 ✗ ⇒
M1 要保留两段重叠、只把 eq+demap 融合 ✓）。

### 2026-10-06 · 工具坑（已修 ✓）：新工作流的日志根对旧读者不可见

`mkf-m0-base` 飞完后驱动报：
```
leg_protocol_driver: REFUSING: no leg 'mkf-m0-base' started within 120 s
                     (looked for gnb_*_mkf-m0-base_*.log.stderr)
```
**根因** ✓：读者的日志根是**硬编码的"两条"**（`phy_pipeline_gpu/wip/logs` + `macos_thread_priority/wip/logs` ✗），
而本工作流的腿写在**第三个**根里 ✗ ⇒ 驱动没记到 cue（`.protocol.txt` 缺失 ✓）、
`ul_health.sh` / `dl_gate.sh` / `rtt_split.py` / 旋钮清单**全都看不见这条腿** ✗。
**修法** ✓：所有读者改为**泛搜 `doc_chinese/*/wip/logs`** ✓（`leg_protocol_driver.sh`、`ul_health.sh`、
`dl_gate.sh`、`rtt_split.py`、`gen_knob_inventory.py` ✓），**下一个工作流开目录时不需要再改任何工具** ✓。
（修复中我自己踩了一下：`ul_health.sh` 的 python 侧变量名是小写 `dirs` ✗ —— 已修并复验 ✓。
教训：**改了读者，就拿一条已知好腿回测** ✓ —— 这次用 `mkf-m0-base` 复验了 ✓。）
**这条腿仍然可用** ✓：结构/功能判据不依赖 cue ✓（缺的只是"业务时刻的精确锚点" ✓）；
但按纪律**注明**：它**没有** `.protocol.txt` ✓，因此**不能**从中引用"负载读数"（扰动/业务窗口）✗。

### 2026-10-06 · ★ 用户两点调整（判据重瞄；已回写架构文档 §2.1bis/§2.2/§2.3 与本文 §0/§1.2/§1.4/§2.3）

1. **不要求 LLR 逐位一致** ✗ —— 融合可能改计算顺序、有浮点舍入差异，甚至为融合收益做简单算法微调 ✓；
   判据以 **CRC-OK 同一档**（PHY 口径）为准，应用层看 `ping`/`iperf3` 表现但**不做硬性要求** ✓；
   **允许一定程度的性能回退** ✓ —— 本工作流是为 high level（Apple Silicon 异构 gNB）**打基础** ✓。
   ⇒ 逐位对拍（§1.4）**降级为出问题时的定位工具** ✓，不再每步验收 ✓。
2. **环境不是起飞条件** ✗ —— **抗干扰能力本身就是健壮性** ✓；
   `preflight_quiet.sh` 与 `dl_gate.sh` 都只是**参考项** ✓：结构/功能判据不受停顿影响，照用 ✓；
   只有**引用时延数字**时才需要闸门 PASS，否则照记并**标注"含停顿、不引用"** ✓。

---

## 6. 会话交接（需要时新建）

命名与结构照抄上一工作流 ✓：`session_handoff_<日期>-<序号>.md`，内容包含：
① 本会话做了什么（引用 memo 条目 ✓）；② **当前状态**（代码/开关/腿号/判据 ✓）；
③ 下一个会话**第一件事**（具体命令 ✓）；④ 未决问题与它们的**重开条件** ✓。
