# Metal kernel 融合 —— 实施设计与步骤（活文档，追加式）

> **用法**：这是**动手改代码的人**的文档 ✓。每一段实作、每一次飞行、每一个决策都**追加**在这里
> （更正写在后面，不改历史 ✓）。**结构性调整**回写到
> `metal_kernel_fusion_high_level_status_and_plan.md`（架构文档，可重写）✓。
>
> **上游功课**：`../macos_thread_priority/workstream_summary_and_lessons.md` §4bis、§7 ——
> "**减少边界比加快边界有效**"、"每跨一次模块边界 ~340 µs ✗"、"设备占用 0.29 % ✗"。

---

## 0. 不变式（每一步都要满足，抄自架构文档 §2.2）

1. **数值等价**：LLR 与今天**逐位一致** ✓（方法见 §1.4 —— **必须用两二进制对拍，不能用环境对拍** ✗）；
2. 健康 CLEAN ✓、`gaps=0` ✓、`dl_gate.sh` PASS ✓；
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
# 先看宿主是否安静（上一工作流：最近 5 条腿 4 条撞环境停顿 ✗）
bash doc_chinese/macos_thread_priority/wip/preflight_quiet.sh
sudo -E env LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs \
     EXTRA_KNOBS="OCUDU_METAL_GPU_TIME=0" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu mkf-m0-base --regime=default
```
**登记读数**（写进 §5 memo ✓）：`[ul_pipeline]` / `[ul_gpu_pipeline]` 的 median/p95/p99 + `[ul_tail]` 计数 ✓、
`[ul_gpu_lane] busy` 与 busy split ✓、`[metal_stats] burst`（commits/dispatches ✓）、
`[metal_stats] mmse_ce` ✓、健康 + `dl_gate.sh` ✓。
★ 业务要**与判读一致**：ping（稀疏，看尾 ✓）与 iperf3（密集，看体 ✓）各一条更稳 ✓ —— 但一次只动一个变量 ✓。

### 1.3 M0 的出口

* 一条**可引用**的基线 ✓（闸门 PASS + 健康 CLEAN ✓）；
* 一张"**成本分解表**"：`merged_hop` = 383 µs 里，dispatch 固定成本 / 读 y / 均衡 / 解调 / 写 LLR 各占多少 ✓；
* 结论：**M1 的理论上限**（若上限 < 5 % ⇒ 诚实收工 ✗✓，这也是允许的结论 ✓）。

### 1.4 ★ 数值等价的正确做法（上一工作流的教训，直接用 ✓）

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
| G4 | `[ul_gpu_lane] busy` 的 `merged_hop` **不升** ✓ |
| G5 | `[ul_gpu_pipeline]`/`[ul_pipeline]` median/p95/p99 **不劣化** ✓（目标：降）|
| 红线 | LLR 逐位一致 ✓（§1.4）、健康 CLEAN ✓、`gaps=0` ✓、`dl_gate.sh` PASS ✓ |

---

## 3. M2：把 **CE 的权重**折进同一 kernel（**判据驱动，可以不做** ✓）

* 前置：M0 显示 `ch_wt`（8 % 设备时间 ✓）**或**它带来的中间往返值得 ✗✓；
* 难点：CE 需要**跨线程协作**（每 RB/组的自相关 + 求逆 ✓）⇒ threadgroup memory + `threadgroup_barrier` ✓，
  或"一个线程组负责一个 RB 组 × 若干符号"的映射 ✓；
* 风险：寄存器/共享内存压力可能让 occupancy 变差 ✗ ⇒ **先做一版只折权重计算的 kernel 试点** ✓，
  与 M1 的形状**分开关**（`OCUDU_LANE_FUSE_CE` ✓），这样能单独判它 ✓；
* 出口：`dispatches`/跳 = **1** ✓，且 G4/G5 不劣化 ✓、LLR 逐位一致 ✓。

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

---

## 6. 会话交接（需要时新建）

命名与结构照抄上一工作流 ✓：`session_handoff_<日期>-<序号>.md`，内容包含：
① 本会话做了什么（引用 memo 条目 ✓）；② **当前状态**（代码/开关/腿号/判据 ✓）；
③ 下一个会话**第一件事**（具体命令 ✓）；④ 未决问题与它们的**重开条件** ✓。
