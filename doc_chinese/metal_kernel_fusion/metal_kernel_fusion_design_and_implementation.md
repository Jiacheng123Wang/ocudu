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

## 2. M1：融合 **均衡 + 解调** —— 落地设计（2026-10-06 定稿，按 M0 的结论 ✓）

> **M0 的结论决定了 M1 的目标怎么写** ✓（实施文档 memo · M0 结项 ✓）：
> 一跳 480 µs 里**内核本体不到 5 µs** ✗、dispatch 只 ~15 µs ✗、CB 数/栅栏不是缺项 ✗、前端搬设备更差 ✗
> ⇒ **M1 的验收不能是"dispatch 4→1"或"CB 2→1"** ✗，只能是 **`busy` 与 `[ul_pipeline]` 的改善** ✓
> —— 也就是说，融合 kernel 必须**真的把计算与访存重排得更省** ✓，否则它会像 dispatch 计数一样"结构达标、性能无感" ✗。

### 2.1 形状：一个 kernel、一 RE 一线程、x̂ 只活在寄存器里

今天两段各自成立（几何**完全一致** ✓：`dispatchThreads:(nof_re, nof_symbols,1)`、256/组 ✓）：

```
均衡:  读网格 y（EQ_DIRECT_GRID ✓）+ 权重 + 噪声  →  写 x̂（中间缓冲，全局内存）
解调:  读 x̂ + 噪声                                →  写 LLR（llr_direct ✓，直写调用者缓冲 ✓）
```
融合后 ✓：
```
lane_grid_to_llr:  读 y + 权重 + 噪声  →  [寄存器内：均衡 → 解调]  →  写 LLR
                   ↑ 无中间缓冲 ✗、无 barrier ✓、无额外 dispatch ✓
```
**每线程的计算顺序**（照抄两段内核的数学 ✓，先求"同数学、只合结构" ✓）：
`y[re]` → 按 `(rb, symbol)` 取权重/噪声 ✓ → `x̂ = w · y` ✓ → 按 `mod` 取每 RE 的比特数 `Qm` ✓ →
逐比特算 LLR ✓ → 写 `llr[re*Qm + b]` ✓。

### 2.2 具体改动（文件级落点）

| 文件 | 改动 |
|---|---|
| **新增** `lib/phy/upper/channel_processors/metal/ocudu_lane_fused.metal` | `kernel void lane_grid_to_llr(...)` ✓：把均衡与解调两段内核的**数学逐行搬进来** ✓，中间量留在寄存器 ✓ |
| `ocudu_equalizer_metal_engine.{h,mm}` | 新增 `enqueue_fused(...)`：绑网格 / 权重 / 噪声 / **LLR** 四个缓冲 + **两个现有 parameter struct**（各占一个 buffer index ✓ ⇒ **不改任何 struct 定义** ✓）+ `mod` 与 `Qm` ✓，然后**只编一次 dispatch** ✓ |
| `ocudu_demod_metal_engine.{h,mm}` | 不改 ✓（融合路线上它不被调用 ✓）；在文件头注明"融合路线下由 lane_grid_to_llr 取代" ✓ |
| **调用方**（PUSCH 的 metal 处理路径，今天先调 eq 再调 demod ✓）| 加**路线选择**：`OCUDU_LANE_FUSE_EQDEMOD` 开着 ⇒ 调 `enqueue_fused` ✓；关着 ⇒ 走今天两步 ✓（**逐字节不变** ✓）|
| CMake | 新 `.metal` 加进 equalizer 的 `ocudu_add_metallib` ✓（M3 再谈把三个库并成一个 ✓）|
| 探针 | `[metal_stats] burst … dispatches` 应从 **4 → 3**/跳 ✓（**这只是"融合生效"的结构证据** ✓，不是收益指标 ✗）|

**开关**：`OCUDU_LANE_FUSE_EQDEMOD`（env ✓，默认**关** ✓）。两把钥匙 ✓：编译期不需要新开关（kernel 编进既有 metallib ✓），
运行时 env 控制路线 ✓ ⇒ 关着时二进制与今天逐字节同路 ✓。

### 2.3 M1 的判据（按 M0 重写 ✓，预登记）

| 类别 | 判据 | 说明 |
|---|---|---|
| **结构证据（必然产物 ✓）** | `[metal_stats] burst dispatches`/跳 **4 → 3** ✓ | 证明融合真的生效 ✓（不达标 = 没接上 ✗）|
| ★ **主判据（性能，报告制 ✓）** | `[ul_gpu_lane] busy` median **< 479.6 µs** ✓ 与 `[ul_pipeline]`/`[ul_gpu_pipeline]` median **< 1321 / 1242.8 µs** ✓ | **允许持平或小幅回退** ✓（用户裁决 ✓），但回退必须写进 memo 并说明是否值得 ✓；**这是判断融合价值的唯一口径** ✓ |
| **辅助** | lane `residency` / `gap` / `host: entry→commit` ✓、`[ul_tail]` 计数 ✓ | 用来解释 busy 的走向（是省了访存还是被 occupancy 吃掉 ✗）|
| **红线** | CRC-OK 与对照腿**同档** ✓、`gaps=0` ✓ | 功能性 ✓（77.8 % 那类退化要立刻停下查 ✗）|
| ✗ **不要求的** | LLR 逐位一致 ✗（只作为出问题时的定位工具 ✓）、dispatch 计数本身 ✗、CB 数 ✗ | 用户裁决 + M0 结论 ✓ |

### 2.5 ★ M1.0 规格（读代码所得 ✓，2026-10-06）

**两段内核的签名与绑定点**（照抄 ✓）：

| | 均衡 `equalize_mxn`（`ocudu_equalizer.metal:118`）| 解调 `demod_soft`（`ocudu_demod.metal:137`）|
|---|---|---|
| b0 | `ushort2* h` cbf16 **权重** `[port][layer][re]` | `float2* symbols` **均衡后符号** `[symbol][mod symbol]` |
| b1 | `ushort2* y` cbf16 **网格** `[port][re]` | `float* noise_var` `[symbol][mod symbol]` |
| b2 | `float2* eq` **输出** `[re][layer]` | `char* llrs_base` **输出** `[symbol][mod symbol][bit]` |
| b3 | `float* nv` **输出** 噪声 `[re][layer]` | `constant demod_params& p` |
| b4 | `constant equalize_params& p` | — |
| b5 | `float* sigma2`（单层路线 ✓）| — |
| 网格 | 1-D `uint re`（2-D 派发 ✓，带 `re >= p.nof_re` 守卫 ✓）| 2-D `uint2 pos`（x=符号内序号 ✓，y=OFDM 符号 ✓）|
| 步长 | 由 `p` 携带 ✓ | `sym_stride` / `nv_stride` / `llr_stride` ✓ |

**逐线程数学（骨架 ✓）**：
* 均衡：加载 `H[port][layer]` ✓ → **单层 SIMO 合并**（`L == 1` ✓ = 我们的 1T1R ✓，CPU 侧对应 `equalize_zf_1xn` ✓）
  ⇒ 得 `x̂`（`eq`）与噪声（`nv`）✓；
* 解调：`z = symbols[...]` ✓ → `rcp = rcp_noise_safe(noise_var[...])` ✓ → 按 `p.mod` 分支（QPSK/16QAM/… ✓）
  算 LLR ✓ → **`quantize_llr(x, 24.0f)`** ✓ → 写 `char` LLR ✓。

**★ 融合映射（M1 的设计结论 ✓）**：用**解调那套 2-D 网格**（x=re ✓、y=OFDM 符号 ✓），
把均衡的 b0/b1/b2/b3 按**同一套 stride** 索引 ✓ ⇒ 一个线程做完 `y → x̂ → LLR` ✓，
`eq`/`nv` **不落全局内存** ✓（解调需要的噪声就是均衡算出的那个 ✓，天然衔接 ✓）。

**⇒ 待确认（下一个动作，仍是读代码 ✓）**：
1. **均衡的"符号轴"语义** ✗：它用 2-D 派发（`MTLSizeMake(nof_re, n_sym, 1)` ✓）但 kernel 只取 1-D `re` ✓
   并有 `re >= p.nof_re` 守卫 ⇒ **符号是怎么进到每个线程的**？读 `equalize_params` 与主机侧绑定即可定 ✓
   （这决定融合 kernel 里网格/权重按哪个 stride 索引 ✗）；
2. **`nv` 与 `noise_var` 的排布差异** ✗：前者 `[re][layer]` ✓、后者 `[symbol][mod symbol]` ✓
   ⇒ 融合 kernel 里把 `nv` 直接算在寄存器里 ✓ 就绕过了这个差异 ✓（但若要走"半融合"退路，需写明换算 ✓）；
3. **`quantize_llr` 的语义** ✓：逐位一致**不要求** ✗（用户裁决 ✓），但**必须一致到"同一件事"** ✓ ⇒
   融合版照抄 ✓，不做"顺便优化"✗。

### 2.6 ★★ M1.0 结论（待确认点已关闭 ✓）：融合对象是 **`equalize_mxn_batch` + `demod_soft`**

**待确认点 1（符号轴怎么进线程）已答 ✓**：车道/延迟编码路线用的**不是** 1-D 的 `equalize_mxn` ✗，
而是 **`equalize_mxn_batch`** ✓（引擎原话："One dispatch for a whole group of symbols, same arithmetic" ✓）：

```metal
kernel void equalize_mxn_batch(device const ushort2* h [[buffer(0)]],  // cbf16 [symbol][port][layer][re]
                               device const ushort2* y [[buffer(1)]],  // cbf16 [symbol][port][re]
                               device float2*       eq [[buffer(2)]], // [symbol][re][layer]
                               device float*        nv [[buffer(3)]], // [symbol][re][layer]
                               constant equalize_params&  p [[buffer(4)]],
                               device const float* sigma2 [[buffer(5)]],
                               constant equalize_strides& st [[buffer(6)]],
                               uint2 gid [[thread_position_in_grid]])   // gid.x = re, gid.y = symbol ✓
```
⇒ **符号轴由 `gid.y` + `equalize_strides`（`h_starts[]` / `y_starts[]` / `eq_stride` / `nv_stride` / `nof_symbols` ✓）携带** ✓
—— 与 `demod_soft` 的 `sym_stride` / `nv_stride` / `llr_stride` **是同一套"整段符号按常量 stride 排布"的约定** ✓✓
（两者的理由也一样：延迟链的均衡符号放在**按页对齐的每符号槽**里 ✓）。

**⇒ 于是融合的形状完全确定** ✓✓（两个 kernel 的网格语义**逐字对齐** ✓）：

| | 均衡 `equalize_mxn_batch` | 解调 `demod_soft` | 融合 `lane_grid_to_llr` |
|---|---|---|---|
| 网格 | `uint2 gid`（re, symbol）✓ | `uint2 pos`（re, symbol）✓ | **`uint2 gid`（同一套 ✓）** |
| 权重/输入 | `h`、`y`、`sigma2` ✓ | `symbols`（= 均衡的输出 ✗）| `h`、`y`、`sigma2` ✓（**不再读 symbols** ✓）|
| 噪声 | 算出来写 `nv` ✗ | 读 `noise_var` ✗ | **寄存器内传递 ✓✓**（两个 stride 约定随之消失 ✓）|
| 输出 | `eq`、`nv` ✗ | `llrs` ✓ | **只写 `llrs`** ✓ |
| 参数 | `p` + `st` ✓ | `p`（demod）✓ | **两个 struct 各占一个 index ✓**（不改定义 ✓）+ LLR 的 `llr_stride` ✓ |

**待确认点 2（`nv` vs `noise_var` 排布差异）自动消失** ✓：融合版把噪声留在寄存器 ✓（上面的表 ✓）。

**待确认点 3（`quantize_llr` 语义）** ✓：**照抄** ✓（`quantize_llr(x, 24.0f)` ✓）；逐位一致不要求 ✗，但"算的是同一件事"是硬要求 ✓。

### 2.7 M1.1 的具体产出（下一步动手 ✓）

1. `ocudu_lane_fused.metal`：`kernel void lane_grid_to_llr(… gid)` ✓ ——
   **上表两列逐行合并**：取 `h`/`y`/`sigma2` 的索引方式照抄 `equalize_mxn_batch` 的 `gid.y`+`st` 段 ✓，
   单层 SIMO 合并（`L==1` ✓）算出 `x̂` 与噪声**留在寄存器** ✓，
   再照抄 `demod_soft` 的 `p.mod` 分支与 `quantize_llr` ✓ 写 `llrs_base + gid.y * llr_stride + …` ✓；
2. 均衡引擎加 `enqueue_fused`（绑 `h`/`y`/`sigma2`/`llrs` + `p`/`st`/`demod_params` ✓，**一次 dispatch** ✓）；
3. 调用方按 `OCUDU_LANE_FUSE_EQDEMOD` 选路线 ✓（关着 = 今天两步 ✓ 逐字节不变 ✓）；
4. **离线先对拍**（`ul_chain_replay` ✓）：确认"同一件事" ✓（不是逐位 ✗）；
5. 再飞 **`mkf010`（对照）** 与 **`mkf011`（臂）** ✓，判据见 §2.3 ✓。

### 2.8 ★ M1.1 实现清单（2026-10-06 定，含一个**范围决定** ✓）

**范围决定 ✓**：**M1 只覆盖 `MOD_QAM16`** ✓（本工作流的判据腿全部是 MCS 13 = 16QAM ✓）；
**其它调制留在今天的两步路线** ✓ —— 调用方在选路线时检查 `mod == MOD_QAM16` ✓。
理由 ✓：把 kernel 从"四种调制的全量合并"缩到"只合并会被飞的那一种" ✓，让第一版**每一行都被腿覆盖** ✓
（用户裁决允许"简单算法/实现调整换取融合收益" ✓，这里是它的保守版 ✓）。后续要扩调制，按同一套加 ✓。

**要逐字搬进 `ocudu_lane_fused.metal` 的符号**（**复制，不改原文件** ✓ —— 抽取到共享头是后续动作 ✓，
因为那会改动在交付形状里的两个 kernel ✗，按纪律需要单独一次 A/B ✓）：

| 来源 | 符号 |
|---|---|
| `ocudu_demod.metal` | 常量 `LLR_MAX_F` / `GAIN_FIRST_16` / `THR_16` / `CONST_0_8` / `NEAR_ZERO` ✓；`quantize_llr` ✓ / `rcp_noise_safe` ✓ / `qam16_01` ✓ / `qam16_23` ✓ |
| `ocudu_equalizer.metal` | `equalize_params` ✓ / `equalize_strides` ✓ / `MAX_PORTS` ✓；`bf16_to_f` ✓；网格与权重的**索引方式**（`st.h_starts[]`/`st.y_starts[]` ✓ + `p.h_offset` ✓ + `gid.y` ✓）；单层 SIMO 合并的**那一整段**（`L==1` 分支 ✓，含 `tx_scaling`、`sigma2[port]`、`d > 0` 守卫与 `eq=0 / nv=INFINITY` 的退化分支 ✓）|

**逐行合并的骨架**（M1.0 §2.6 的表 ✓）：
```
h  += st.h_starts[min(sym, eq_max_run_symbols-1)] - p.h_offset;     // 照抄 batch
y  += st.y_starts[min(sym, eq_max_run_symbols-1)];
// 单层 SIMO 合并（照抄 L==1 分支）→ x_hat（寄存器）、nvar（寄存器）
const float rcp = rcp_noise_safe(nvar);
device char* llrs = llrs_base + sym * llr_stride;                    // 照抄 demod
llrs[4*re+0] = quantize_llr(qam16_01(x_hat.x, rcp), 20.0f);          // 四比特照抄
llrs[4*re+1] = quantize_llr(qam16_01(x_hat.y, rcp), 20.0f);
llrs[4*re+2] = quantize_llr(qam16_23(x_hat.x, rcp), 20.0f);
llrs[4*re+3] = quantize_llr(qam16_23(x_hat.y, rcp), 20.0f);
```
**CMake**：`ocudu_lane_fused.metal` 加进 equalizer 的 `ocudu_add_metallib` ✓（M3 再合并三个库 ✓）。
**编译自检** ✓（不需要电台、不需要飞腿 ✓）：`xcrun -sdk macosx metal -c` 编一次 ✓ —— 与
`probes_off_syntax_check.sh` 同一类"先证明它能编"的动作 ✓。
**接线（这一步之后做 ✓）**：均衡引擎加 `enqueue_fused` ✓ → 调用方（`pusch_demodulator_impl` ✓）
按 `OCUDU_LANE_FUSE_EQDEMOD && mod==QAM16` 选路线 ✓ → 关着时逐字节走今天两步 ✓。

### 2.9 M1.1 进展（2026-10-06）：**kernel 已写并已证明能编/能打包** ✓（接线未完）

| 步骤 | 状态 |
|---|---|
| 写 `ocudu_lane_fused.metal`（16QAM 融合 ✓，§2.8 骨架 ✓）| **完成 ✓**（新文件 ✓，**不改动两个现有 kernel** ✓）|
| 加进 `ocudu_metallib_equalizer` ✓ | **完成 ✓**（CMake 已改 ✓，与两个原 kernel 同库 ⇒ 运行时切换不需要第二个库 ✓）|
| 编译自检（不需要电台 ✓）| **完成 ✓**：`xcrun metal -c -std=metal3.0 ocudu_lane_fused.metal` **exit 0**（6576 B AIR ✓）|
| 打包自检 ✓ | **完成 ✓**：`metal -c` 两个源 + `metallib` ⇒ 35 143 B 合并库 ✓，`lane_grid_to_llr` **确实在里面** ✓ |
| **接线（下一步 ✓）** | 未做 ✗：均衡引擎 `enqueue_fused` ✓ → 调用方（`pusch_demodulator_impl` ✓）按 `OCUDU_LANE_FUSE_EQDEMOD && mod==QAM16` 选路线 ✓ |

**实现时的三个细节决定**（都写进了 kernel 的注释 ✓）：
1. `quantize_llr` 的**取整**照抄原实现（`rint` 后钳位再转 `char` ✓）—— 逐位一致不要求 ✗，但"同一件事"是硬要求 ✓；
2. 均衡的**退化分支**（`d <= 0` ⇒ `eq=0, nv=INFINITY` ✓）在融合版里显式保留 ✓
   ⇒ `rcp = rcp_noise_safe(INFINITY) = 0` ✓ ⇒ 四个 LLR 走 `NEAR_ZERO` 守卫后为 0 ✓ = 与两步路线同结果 ✓；
3. `eq_max_run_symbols = 32` 的常量在融合文件里**独立声明** ✓（复制而非引用 ✓，与原文件保持一致 ✓）。

### 2.10 ★★ M1.1 接线完成（2026-10-06，本会话）：融合路线**已能跑到**，离线判据**逐字节一致** ✓✓

**结论** ✓：`OCUDU_LANE_FUSE_EQDEMOD` 现在**真的切路线** ✓ —— 一跳 `[metal_stats] burst dispatches` **4 → 3** ✓、
**demapper 的 dispatch 归 0** ✓（`demap batch flushes=0`）、`sites(... y_fused=1 y_batch=0)` ✓。
**离线对拍** ✓（`ab_dumps.sh`，27 个 capture，QAM16 标注语料 ✓）：**四个 dump 全部逐字节相同** ✓
（`_llr.bin` / `_h.bin` / `.bin` / `_ce.txt` 差异字节数 **0/0/0/0** ✓）⇒ **"算的是同一件事"成立到逐位** ✓。

**当前状态** ✓（本节的出口 ✓）：融合路线**已接完并验证** ✓，**默认关** ✓ ⇒ 默认二进制与 M0 基线**逐字节同路** ✓；
开关 = `OCUDU_LANE_FUSE_EQDEMOD`（env ✓，**非空即开** ✓，值不被读 ✓ ⇒ `=0` 也是臂 ✓，这是腿协议的形状 ✓）；
**适用形状** ✓ = **16QAM ✓ + 1 层 ✓ + `evm_calc == nullptr` ✓**（`channel_equalizer::supports_fused_demapping()` ✓）；
腿号 ✓：已飞到 **mkf009** ✓，下一对 = **mkf010（对照）/ mkf011（臂）** ✓（§2.11 ✓，取号 `bash wip/next_leg_label.sh` ✓）；
判据仍是 §2.3 ✓（结构证据 = dispatch 4→3 ✓；主判据报告制 ✓；红线 = CRC 同档 + `gaps=0` ✓）。

**落点（与 §2.2 的设想不同，因为读代码后事实不同 ✗）**：

| 层 | 文件 | 做了什么 |
|---|---|---|
| kernel | `ocudu_lane_fused.metal` ✓ | 新增 **b7 = nv** 输出（见下"必须记一笔的决定" ✓）；`eq_max_run_symbols` **32 → 14**（**原值是错的** ✗，见下"发现的缺陷" ✓）|
| 引擎 | `ocudu_equalizer_metal_engine.{h,mm}` ✓ | `pipeline_fused`（**可选加载** ✓，metallib 里没有就保持两步路线 ✓，并在 stderr 打出 `[eq_impl] … available/…` ✓ 作为出处 ✓）；`eq_pending_t` 加 `llrs`/`fused` ✓；flush 钩子的 run 谓词加**融合分支的输出步长**（LLR 字节步 + nv 步长 ✓）与 `route` 断点 ✓；`eq_encode_fused_dispatch` ✓（b0 h / b1 y / b2 sigma2 / b3 llrs / b4 p / b5 st / b6 llr_stride / b7 nv ✓）；`supports_fused()` ✓；`enqueue_fused()` ✓ |
| 接口 | `channel_equalizer.h` ✓ | `supports_fused_demapping(mod, **nof_ports**, nof_layers)` + `submit_fused(...)` ✓（都有**默认实现** ✓ ⇒ 老后端不受影响 ✓）|
| 后端 | `channel_equalizer_metal.{h,cpp}` ✓ | 两个 override ✓；`run_fused()` ✓（与 `run_equalize()` 同一套 plan/device-slice/gather 逻辑 ✓，只少写 eq ✓）|
| ★ **复合工厂** | `channel_equalizer_metal_factory.cpp` ✓ | **这一步是接线的真正落点** ✓ —— 见下 ✓ |
| 调用方 | `pusch_demodulator_impl.cpp` ✓ | `fused_chain` 路线选择 ✓ + 一行 self-describing 日志 ✓（设了却没生效时**说出是哪一条不满足** ✓）|

**★★ 发现的第一个坑（接线层的答案 ✓）**：调用方拿到的**不是** `channel_equalizer_metal` ✗，
而是工厂里的**复合包装** `channel_equalizer_metal_or_generic` ✓（它按拓扑在 Metal 与 CPU 之间选 ✓）。
该包装**只转发它自己重新声明过的方法** ✗ ⇒ 没转发的接口方法落到**基类默认实现** ✗
⇒ `supports_fused_demapping()` 永远回答 **false** ✓、`submit_fused()` 永远走老路 ✗
⇒ **旋钮的唯一效果是"什么都没发生"** ✗（实测：`fused=0 y_fused=0` ✓，而引擎里 kernel **available** ✓）。
**修法** ✓：在该包装里补 `supports_fused_demapping()`（= Metal 后端的答案 **AND** 它自己的路由决定 ✓，
因为"调制+形状"属于后端、"拓扑"属于 select() ✓）与 `submit_fused()`（转发 ✓）。
**教训** ✓：**加接口方法时，要顺着"调用方真正持有的对象"一路查转发层** ✓
（这条与上一工作流"接口默认实现是良性回退、也是静默失效点"是同一个形状 ✓）。
**单元测试已加回归** ✓：`[fused] composite factory offers the fused route for 16QAM / 1 layer only` ✓。

**★★ 必须记一笔的决定（`nv` 仍是输出 ✓，与 §2.1 的"只写 LLR"不同 ✗）**：
读调用方发现 —— **本树 PUSCH 的默认 SINR 方法就是 `post_equalization`** ✓
（`du_low_config.h` 默认值 ✓，腿的 cell config 也没改 ✓），
而它**在 host 上归约 `state.nv`** ✓，结果进 `stats.sinr_dB` ✓ ⇒ 上行链路自适应 ✓。
⇒ 融合版若**只写 LLR** ✓，则要么**在默认配置下永远跑不到** ✗，要么**报出 inf 的 SINR** ✗（比慢更糟 ✓）。
**决定** ✓：融合 kernel **写 `nv`**（4 B/RE ✓），**不写 `eq`**（8 B/RE ✗）。
融合省下的 24 B/RE（eq 写 8 + eq 读 8 + nv 读 4 + 第二次 dispatch ✓）里留下 4 B/RE ✓
—— **判据要考的"重排计算/访存"部分没被削弱** ✓；"不要 nv"是一个**后续 A/B** ✓（要连 SINR 方法一起改 ✓）。
**EVM 仍与融合不兼容** ✓（它读 `eq` 8 B/RE ✗）：EVM 只在 `pusch_sinr_calc_method: evm` 或 debug 日志级别下开 ✓，
**腿里是关的** ✓ ⇒ 调用方以 `evm_calc == nullptr` 为门 ✓，并在日志里点名 ✓。

**★★ 发现的第二个缺陷（上一会话留下的 ✗，靠读出来 ✓）**：
`ocudu_lane_fused.metal` 里 `eq_max_run_symbols = 32` ✗，而 `ocudu_equalizer.metal` 与 C++ 侧都是 **14** ✓。
strides 块是**按字节原样传的** ⇒ 表长不同 ⇒ **`y_starts` 从错的偏移读** ✗
—— **不是编译错误，是静默的错 RE** ✗（正是本工作流反复记录的那类形状 ✓）。**已修** ✓ 并在 kernel 里写明"必须一致" ✓。

**离线怎么跑的（语料是 QPSK ✗ ⇒ 需要一步显式操作 ✓）**：
1. `ul_chain_replay` 的 27 个 capture **全是 QPSK** ✗（`grep modulation= doc_chinese/work_tmp/corpus/*.txt` ✓），
   而融合 kernel 只做 16QAM ✓ ⇒ 直接跑时**路线被正确地拒绝** ✓（好证据：门是有效的 ✓）；
2. 因此造一份**同网格、标注为 16QAM** 的语料 ✓（`/tmp/q16c/`，`sed 's/^modulation=QPSK$/modulation=16QAM/'` ✓）
   —— 判据是**两条路线在同一输入上是否同一件事** ✓，**不是** CRC/解调正确性 ✓（那由腿判 ✓）；
3. 工具默认**开 EVM** ✗（`ul_chain_replay.cpp` 里一直传 `create_evm_calculator_factory()` ✓）
   ⇒ 加了 `OCUDU_UL_REPLAY_NO_EVM` ✓（**两侧都要带** ✓，它决定"哪条路线可达" ✓，不改任何一侧的算术 ✓）；
4. `bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh "OCUDU_UL_REPLAY_NO_EVM=1" \
   "OCUDU_UL_REPLAY_NO_EVM=1 OCUDU_LANE_FUSE_EQDEMOD=1" --metal "/tmp/q16c/*.bin"` ✓。

**注意（不属本工作流、但会被看到 ✓）**：该工具在**退出时**偶发
`system_error: mutex lock failed` 中止 ✓ —— 实测**对照臂也有** ✓（A 4/10、B 0/10 ✓，dump 已写完 ✓）
⇒ **是既有的收尾竞态** ✗，不是融合引入的 ✓；`ab_dumps.sh` 的 `missing-dumps=0` 判据不受影响 ✓。

### 2.11 ★ 下一步（待飞，2026-10-06 第 2 次末）：`mkf010` / `mkf011` 一对

**前置** ✓（改代码后才需要重做 ✓，现在都已就绪 ✓）：`gnb` 已重编 ✓、metallib 已重生成且含 `lane_grid_to_llr` ✓、
`build/hashes.h` 的指纹与 HEAD 一致且**在二进制里** ✓（`run_leg.sh` 会自己拒绝不一致的二进制 ✓）。

用 `fly_leg.sh`（**它自带 protocol driver** ✓ —— 只跑 `run_leg.sh` 会丢 `.protocol.txt` ✗，`mkf-m0-base` 就是这么丢的 ✗）：

```bash
cd /Users/jiachengwang/dev/ocudu
export LEG_CFG=$PWD/doc_chinese/macos_thread_priority/wip/gnb_pinned_mcs13.yml
export LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs

# 对照腿（默认形状 ✓，不带任何 EXTRA_KNOBS ✓）
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf010-m1-fused-off dual quiet gpu

# 臂腿（**一个变量** ✓：融合开 ✓）★ EXTRA_KNOBS 必须在 sudo -E 之后传入（env_reset 会抹掉前面的赋值 ✗）
EXTRA_KNOBS="OCUDU_LANE_FUSE_EQDEMOD=1" \
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf011-m1-fused-on dual quiet gpu

# 判读（功能/结构为主 ✓；时延数字要引用时才过闸门 ✓）
bash doc_chinese/macos_thread_priority/wip/ul_health.sh  mkf010-m1-fused-off
bash doc_chinese/macos_thread_priority/wip/ul_health.sh  mkf011-m1-fused-on
bash doc_chinese/macos_thread_priority/wip/dl_gate.sh    mkf011-m1-fused-on     # 参考项 ✓
bash doc_chinese/macos_thread_priority/wip/pair_check.sh mkf010-m1-fused-off mkf011-m1-fused-on
```

**这对比 M0 基线多带了什么** ✓（**两条腿完全一样** ✓ ⇒ 仍是"一个变量" ✓）：
`fly_leg.sh` 的标准仪表集（`OCUDU_SCHED_VERBOSE` 等 9 个 ✓）+ cell config **`gnb_pinned_mcs13.yml`** ✓
—— **MCS 13 = 16QAM** ✓，正是融合 kernel 的范围 ✓（`mkf008/009` 用的也是它 ✓，且 `*.driver.log` 的命名证明
前几条腿就是 `fly_leg.sh` 飞的 ✓）。业务：cue 时在 **CORE 侧**起
`ping -i 0.1 -c 1800 <UE-IP>` ✓，腿末用 **Ctrl-C** 停 ✓。

**判读要点** ✓（先看"路线真的生效了" ✓，再看数字 ✓）：
1. 臂腿日志里应有 `PUSCH: FUSED equalization + demapping chain enabled (… 16QAM)` ✓
   与 `[eq_impl] fused equalizer+demapper (lane_grid_to_llr): available` ✓
   （metallib 是**运行时**从源码树加载的 ✓ ⇒ 后者是"这个库有没有融合 kernel"的唯一出处 ✓）；
2. `[metal_stats] burst … dispatches` 应**每跳 3** ✓ 且 `(equalizer=… demapper=0 …)` ✓、
   `eq_batch … sites(… y_fused=… y_batch=0)` ✓；
3. 然后才是 `[ul_gpu_lane] busy` ✓ / `[ul_pipeline]` ✓ / CRC ✓（§2.3 的判据 ✓）。
**若出现** `PUSCH: OCUDU_LANE_FUSE_EQDEMOD is set but the fused route is NOT taken (…)` ✓
⇒ **括号里就是原因** ✓（多半是调制不是 16QAM ✗ ⇒ 说明 MCS 没按 MCS 13 跑 ✓）
—— **不要**把它读成"融合无收益" ✗。

### 2.12 未决项与重开条件（M1 期间 ✓）

| 项 | 状态 | 重开条件 |
|---|---|---|
| **融合路线的收益判决** | **未判** ✗ | `mkf010`/`mkf011` 飞完 ✓ ⇒ 按 §2.3 写 memo ✓ |
| "不要 `nv` 输出"的 A/B（省 4 B/RE ✓）| 未做 ✗ | 融合收益兑现之后 ✓；它要**同时**换 SINR 方法（`--pusch_sinr_calc_method=channel_estimator` ✓），所以是**两条腿一对** ✓ |
| **EVM 与融合不兼容** ✓ | 已明确 ✗（EVM 读 `eq` ✗，腿里默认关 ✓）| 若要 `pusch_sinr_calc_method=evm` + 融合 ⇒ kernel 需再写 `eq`（8 B/RE ✗），届时按同一套加 ✓ |
| 其它调制（QPSK/64QAM/256QAM）| **M1 不做** ✗ | 融合在 16QAM 上兑现收益之后 ✓（加分支照抄 demapper ✓）|
| 多层的融合（2/4 层）| **M1 不做** ✗ | 同上 ✓（kernel 目前只有 `L==1` 分支 ✓）|
| helper 抽取为共享头 ✓ | 未做 ✗（当前是**复制** ✓）| 融合被证明有价值之后 ✓（抽取会改写交付形状里的两个 kernel ⇒ 需要自己的 A/B ✓）|
| M2（折 CE）/ M3（metallib 整合）| 未开始 ✓ | M1 出结论之后 ✓ |
| 离线工具收尾竞态 ✗ | **既有** ✗（`ul_chain_replay` 退出时偶发 `mutex lock failed` ✓，**对照臂也有** ✓，dump 已写完 ✓）| 若它开始影响判读（`missing-dumps>0` ✗）⇒ 单开一条线查 ✓ |
| 语料只有 QPSK ✗ | 已绕过 ✓（造了标注 16QAM 的同网格语料 ✓）| 若要有**真实** 16QAM 离线语料 ⇒ 需要一次空中 capture（`OCUDU_UL_DUMP` ✓ + MCS 13 ✓）✓ |

### 2.4 M1 的执行顺序（每步可停 ✓）

1. **M1.0 读代码**（不飞腿 ✓）：把均衡与解调两段内核的**数学与绑定点逐条抄下来**（含 `mod` 的每种取值、
   `Qm`、权重的索引方式 ✓），写成一张对照表 ⇒ 这是融合 kernel 的规格说明 ✓；
2. **M1.1 写 kernel**：逐行搬数学、x̂ 留寄存器 ✓；**先在单元测试/离线对拍上跑**（`ul_chain_replay` ✓），
   不是为了逐位一致 ✗，而是为了**确认它算的是同一件事** ✓；
3. **M1.2 接路线 + 开关** ✓ **已完成**（2026-10-06 第 2 次 ✓，规格见 §2.10 ✓）：离线已确认 `dispatches` 4→3 ✓
   （结构证据 ✓）与四个 dump 逐字节一致 ✓；
4. **M1.3 判据腿**：`mkf010-…`（对照 = 今天 ✓）与 `mkf011-…`（臂 = 融合 ✓），同配置、一个变量 ✓
   —— **命令与判读要点见 §2.11 ✓**；
5. **M1.4 若 busy 不降** ✗：按序试三件（每件一个变量 ✓）——① 每线程处理 2 个 RE（提高 ILP ✓）；
   ② 权重/噪声放 threadgroup memory（同组共享 ✓）；③ LLR 写回分块（避免 uncoalesced ✗）——
   每件都沿用同一套判据 ✓。

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

### 2026-10-06 · ★★ M0b（`event` vs `burst`）：**假设被否定** ✗ —— 而"缺的那 760 µs"被找到了 ✓✓

**两条腿**（背靠背 ✓、同配置同业务 ✓）：`mkf-m0b-event`（CRC 98.2 % **marginal** ✗，且 `rx_wait` max **13.9 ms** ✗
⇒ 它的时延**不可引用** ✗）与 `mkf-m0b-burst`（**CLEAN 99.3 % ✓、闸门 PASS ✓**）。

| 腿 | `cbs/lane` | commits/跳 | fence signals/waits | busy median | busy p95 | `[ul_pipeline]` median |
|---|---|---|---|---|---|---|
| base（**无旋钮 = 默认**）| 2.00 | 2.00 | 217956 / 108978 | 479.6 | 634.4 | **1321** ✓ |
| `event` | 3.00 ✗ | 3.00 ✗ | 216892 / 216893 | 445.8 | 482.2 | 1509 ✗（腿带停顿 ✗）|
| `burst` | 3.00 ✗ | 3.00 ✗ | 108173 / 216344 | 441.9 | 482.6 | **1627** ✗✗（腿干净 ✓）|

**三条结论，都是否定的** ✗：
1. **预登记的结构预测失败** ✗：代码注释说 `event` = 2.00 submissions、`burst` = 1 —— 实测两条**都是 3.00** ✗
   ⇒ 这个旋钮在**本构建里没有把 CB 数分开** ✗（我据此立的 **G0「CB 2→1」随之撤回** ✗）；
2. `event` 与 `burst` 的 busy **无法区分** ✓（445.8 vs 441.9、p95 482.2 vs 482.6 ✓）
   ⇒ **"第二条 CB + 那条栅栏"不是那 ~380 µs** ✗✓；
3. ★ **单 CB 路线反而更慢** ✗✓：`burst` 干净通过闸门 ✓，`[ul_pipeline]` median **1627 vs 默认 1321（+306 µs）** ✗
   —— 代码注释自己记过这条路线的代价（"估计器要等整组编码完才能开始"，历史值 ~125 µs 延迟债 ✗），
   **登记过但从未飞的那一对，现在飞了，方向与它警告的一致，幅度更大** ✓✓。
   机制也清楚 ✓：`burst` 的**设备 busy 更小**（441.9 < 479.6 ✓）但**端到端更慢** ✗
   ⇒ **设备侧窗口与端到端时延方向相反** ✓ —— 因为牺牲掉的是"估计器 GPU 工作 ↔ host 编码"的重叠 ✓✓。

⇒ **M1 的形状据此收紧** ✓：**保留 CE 的独立 CB 与 fence** ✓（不许为了"单 CB"而合并它 ✗），
**只把 eq+demap 融进同一条 CB / 同一个 kernel** ✓ —— 即"减少边界"要减在**不牺牲重叠**的地方 ✓。

### ★★ 缺的那部分时间找到了（不需要再飞腿 ✓）

M0 基线（`gpu` 模式，`mode options: <none>` ✓）：

```
[ul_gpu_pipeline]（IQ→LLR）median  1242.8 µs
  ├─ lane residency（grid 入口→LLR）median  481.9 µs   ← 只占 39 % ✓（本工作流全部工作的对象 ✓）
  └─ 其余 ~760 µs（61 %）✗                 ← 在 lane 之外
```
而 **DFT/grid 在这个模式下的默认后端是 CPU** ✓ —— 代码原话（`apps/units/flexible_o_du/o_du_low/du_low_phy_pipeline.h:236-239` ✓）：
> "THE DFT IS NOT ONE OF THEM ANY MORE (flipped 2026-09-30). Its DEFAULT in this mode is the **CPU**:
> the grid is written by the **HOST** and the lane keeps the estimator, the equalizer and the demapper on the device."

⇒ **本工作流瞄准的"DFT 网格 → LLR"里，61 % 是 lane 之外的 host FFT + 网格打包** ✗✓✓
（与腿上的旁证一致 ✓：`dft radio inputs: 0 transforms …`、`[ul_dft_wait] no samples` ✓ = 那些计数器属于 Metal/handover 路线 ✓，
本模式走的是 CPU 路线 ✓）。
⇒ **它比融合的目标本身还大（760 vs 480 µs ✗）**，但**在用户划定的范围之外** ✓（用户明确排除 Metal FFT ✓，
理由是 FFT 可 CPU 可 GPU、可 per-symbol 可 per-slot，要保留灵活性 ✓）
⇒ **这是一个需要用户裁决的范围问题** ✓，不是技术未知 ✓：验证它只需**一条腿**（`--pusch_dft_type metal` ✓，
一个变量 ✓，直接量这 760 µs 的归属 ✓）。

### 2026-10-06 · M0c（`--pusch_dft_type=metal`，一个变量）：**路线生效 ✓，但腿作废 ✗ ⇒ 跨度问题未答**

**结构证据（唯一确凿的一件事 ✓✓）**：Metal DFT 路线**真的跑了** ✓
```
[phy_pipeline] dft radio inputs: 1773952 transform(s) went through the two submit routes:
                the hand-over route carried 1525972 (86.0%), the plain route 247980; 0 wrap(s) staged a host copy
```
对照 base 腿是 **0 transforms** ✓ ⇒ 这个计数器就是**路线是否生效的判据** ✓（新工具，记入工具清单 ✓）。

**✗ 腿作废**：健康 **DEGRADED**（CRC steady **72.8 %** ✗、retx **27222** ✗）、闸门 **STALL**（frontier 10.8 ms ✗、
rx_wait max 9.98 ms ✗）⇒ 时延**不可引用** ✗。

**指示性读数（含停顿，**不可引用** ✗）**：

| | base（CPU DFT ✓）| M0c（Metal DFT）|
|---|---|---|
| `[ul_gpu_pipeline]` median | **1242.8** | **1446** ✗（+203）|
| lane residency | 481.9 | **723.7** ✗（+242）|
| lane busy | 479.6 | 539.3 ✗ |
| lane gap | 29.7 | **182.4** ✗（+153）|
| host: entry→commit | 83.2 | 100.8 ✗ |

⇒ 方向与"Metal DFT 会把前端 760 µs 收回来"**相反** ✗（residency 反而涨 ✓ —— 因为 hand-over 路线把 DFT 的块
**交给 lane**，于是它的工作进了 lane 的窗口 ✓）—— **但这**不能定论 ✗：停顿与退化本身就会抬高一切读数 ✗✓。

**✗ 两个候选原因，一条腿分不开**：
(a) **Metal DFT 路线的数值/行为让链路退化** ✗（它曾是 2026-09-30 之前的**默认** ✓ ⇒ 本来能用 ✓，
但此后一直没被使用 ✗ ⇒ 回归是可能的 ✓）；
(b) **恰好赶上坏链路** ✗（本环境的腿近来普遍不干净 ✓）。
★ 注意 CRC 是 **steady 72.8 %**（中段 80 % 窗口 ✓）⇒ **不是单次停顿的产物** ✗✓，但**是"路线"还是"链路"仍分不开** ✗。

**⇒ 下一对（M0c-pair，背靠背、一个变量）** ✓：
```bash
# 对照：CPU DFT（= 今天的默认）
EXTRA_KNOBS="--expert_phy.pusch_dft_type=cpu"   →  mkf-m0c-cpu
# 臂：Metal DFT
EXTRA_KNOBS="--expert_phy.pusch_dft_type=metal" →  mkf-m0c-metal
```
**预登记**：① 若 Metal 腿**再次**稳态 CRC 明显低于对照 ⇒ **是路线** ✗ ⇒ 停止把 DFT 纳入视野 ✓
（并把它作为一条"Metal DFT 回归"的发现交给上游 ✓）；② 若两腿 CRC 同档 ⇒ 这次是**链路** ✗ ⇒
再看跨度（`[ul_gpu_pipeline]` 是否 < 1242 ✓）来回答"那 760 µs 归谁" ✓。

### 2026-10-06 · 腿标签加序号（用户要求 ✓）：`mkf<NNN>-<阶段><臂>`

用户：*"在飞腿的 label 可以参照以前的工作流，可以另外加正一个单调增加的序号，例如 `mkf001-m0c-metal`，便于以后查找"* ✓。
⇒ 约定 `mkf<NNN>-<阶段><臂>` ✓（三位、从 001 起、只增不减 ✓）；**已飞的 7 条不改名** ✓（飞过的标签是证据 ✓），
但**计入序号** ✓；映射表在 `README.md` ✓；下一个序号由 **`wip/next_leg_label.sh`** 自动算出 ✓
（它数各日志根里的腿日志 ✓，所以不会与事实漂移 ✓；写这个工具时又踩了一次"`set --` 吃掉位置参数"✗ —— 已修并复测 ✓）。
⇒ 这一对的标签：**`mkf008-m0c-cpu`**（对照 ✓）与 **`mkf009-m0c-metal`**（臂 ✓）。

### 2026-10-06 · ★★★ mkf008/mkf009（cpu vs metal DFT，背靠背）：**Metal DFT 让跨度更差 ✗ ⇒ M0 结项**

| | **mkf008（CPU DFT）** ✓ | **mkf009（Metal DFT）** | Δ |
|---|---|---|---|
| 路线证据（`dft radio inputs`）| **0 transforms** ✓ | **1 824 145 transforms**（hand-over 路线 ✓）| 路线确认 ✓ |
| `[ul_gpu_pipeline]`（IQ→LLR）median | **1247.7 µs** ✓ | **1444.5 µs** ✗ | **+197 µs** ✗ |
| lane **residency** median | **484.4** | **726.5** ✗ | **+242** ✗ |
| lane busy median | 479.9 | 540.0 ✗ | +60 |
| lane **gap** median | **29.2** | **187.4** ✗ | **+158** ✗ |
| host: entry→commit | 83.2 | 99.7 ✗ | +17 |
| 健康（CRC steady）| 97.1 % marginal ✗ | 94.9 % DEGRADED ✗ | −2.2 |

**四条结论** ✓：
1. **Metal DFT 把跨度做差了 ~197 µs** ✗✓（机制清楚：hand-over 路线把 DFT 的块**交给 lane** ⇒ 它的工作进了 lane 的
   residency（+242 ✗），而 lane 还要**等**前端交块（gap +158 ✗））⇒ **"把 DFT 放上设备能收回前端 760 µs"被证否** ✗✓；
2. **CPU DFT 腿的跨度与 M0 基线逐微秒吻合** ✓（1247.7 vs 1242.8 ✓）⇒ 基线可复现 ✓✓；
3. ⇒ **维持"DFT 留在 CPU"** ✓（本工作流不把 DFT 纳入视野的正确性由此得到实测支持 ✓）；顺带：Metal DFT 路线的
   **CRC 在两次独立比较里都更低**（99.1 → 72.8 ✗；97.1 → 94.9 ✗）⇒ 方向一致 ✓、第二次差距落在链路噪声内 ✗
   —— 记为"**不建议使用该路线**" ✓，不下更强的结论 ✗；
4. ✗ 两条腿都不健康（97.1 / 94.9 %，且都带停顿 ✗）⇒ 按纪律**幅度不可引用** ✓；但**方向在两个独立会话里复现** ✓
   （本次 +197 µs，上一次单腿 +203 µs ✗）⇒ 结论稳健 ✓。

### ★★ M0 结项（本工作流的测量阶段完成 ✓）

```
一跳 gpu（默认形状）的 480 µs lane busy 的完整分解（全部实测）
  ├─ 内核本体（eq + demap + ce 全加起来）      < 5 µs   ✗ 消去三条腿 ✓
  ├─ dispatch 4 次                             ~15 µs  ✗ 微基准 ✓
  ├─ host 编码                                 83 µs   ✓
  ├─ 剩余 ~380 µs                                        ← 不是 CB 数/栅栏（M0b ✗）、不是内核（✗）
  └─ 而 [ul_gpu_pipeline] 1242.8 µs 里，lane residency 只占 481.9（39 %）✓，
     其余 ~760 µs 是 lane 之外的 host 前端 ✓ —— 而把它搬上设备（Metal DFT）会更差 ✗（本节 ✓）
```
**M0 的净结论** ✓：本工作流要动的对象是 **lane 内那 ~480 µs** ✓，其中**能确定归属的只有不到 100 µs** ✗
（host 83 + dispatch 15 ✓）；**内核融合的价值因此必须由"重排计算/访存"来兑现** ✓，
而不是靠省 launch、省 CB、或搬前端 ✓✓。**M1（eq+demap 融合）按此推进** ✓，
并保留 CE 的独立 CB 与 fence ✓（M0b ✓）。

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

### 2026-10-06 · ★★ M1.1 接线完成 + 离线对拍（详细规格见 §2.10 ✓）

**做了什么** ✓：`enqueue_fused`（引擎）+ `supports_fused_demapping`/`submit_fused`（接口与 Metal 后端）
+ **复合工厂转发** ✓ + 调用方路线选择 ✓；单元测试加 `[fused]` 回归 ✓；离线 A/B 跑通 ✓。

**数字** ✓（都在本机离线复现 ✓，不是腿）：

| 读数 | 对照（两步 ✓） | 融合（✓） |
|---|---|---|
| `[metal_stats] burst dispatches`/hop ✓ | **4**（eq 1 + demap 1 + ce 2 ✓）| **3**（ce 2 + 融合 1 ✓）|
| `demod_batch dispatches` ✓ | 1 ✓ | **0** ✓（demapper 不被调用 ✓）|
| `eq_batch sites(y_batch/y_fused)` ✓ | 1 / 0 ✓ | **0 / 1** ✓ |
| 27 capture 的四个 dump ✓ | — | **逐字节相同 ✓**（0/0/0/0 ✓）|
| 单元测试 `[fused]` ✓ | — | **LLR 与 nv 与两步路线逐位一致 ✓** |

**三个"读出来"的结论** ✓（都不是跑出来的，跑了也只会看到"旋钮没效果" ✗）：
1. **接线层是复合工厂** ✓（`channel_equalizer_metal_or_generic`）—— 接口方法不转发就落基类默认 ✗；
2. **默认 SINR 方法 `post_equalization` 在 host 上读 `nv`** ✓ ⇒ 融合 kernel **保留 nv 输出** ✓（§2.10 ✓）；
3. **上一会话的 kernel 里 strides 表长写错**（32 vs 14 ✗）⇒ 静默错 RE ✗，已修 ✓。

**收敛的动作** ✓：`gnb` 全量重编 ✓、`ctest -R channel_equalizer_metal` **2/2 PASS** ✓、
`probes_off_syntax_check.sh` **34 TU 全过** ✓、`xcrun metal -c` 融合 kernel **exit 0** ✓、
metallib **35 479 B 且含 `lane_grid_to_llr`** ✓。

**下一步** ✓：飞 **`mkf010`（对照）/ `mkf011`（臂）** ✓ —— 两条腿都是 MCS 13 = 16QAM ✓、
`--regime=default` ✓、臂带 `OCUDU_LANE_FUSE_EQDEMOD=1` ✓（**一个变量** ✓），
判据见 §2.3 ✓（**结构证据 4→3 是必然产物** ✓，`busy` 与 `[ul_pipeline]` 是报告制 ✓，红线 = CRC 同档 + `gaps=0` ✓）。

---

## 6. 会话交接（**只在准备开新会话时**新建 ✗ 不是每段工作结束时）

★ **时机**（用户 2026-10-06 明确 ✓）：`session_handoff_*.md` **只用于新会话交接时的现状快照** ✓；
**同一条对话里继续干活时不要写它** ✗ —— 那一轮的结论、现状、下一步与未决项**全部写进本文** ✓
（实作与数字进 §2.x 与 memo ✓、下一步进 §2.11 这类小节 ✓、未决项进 §2.12 这类表 ✓）。
快照是**一次性**的 ✓：新会话开始后它会过时 ✓，所以它只承载"接手需要的最小充分集" ✓，不与活文档争内容 ✓。

命名与结构照抄上一工作流 ✓：`session_handoff_<日期>-<序号>.md`，内容包含：
① 本会话做了什么（引用 memo 条目 ✓）；② **当前状态**（代码/开关/腿号/判据 ✓）；
③ 下一个会话**第一件事**（具体命令 ✓）；④ 未决问题与它们的**重开条件** ✓。
