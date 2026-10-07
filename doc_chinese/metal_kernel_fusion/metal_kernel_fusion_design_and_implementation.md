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

**当前状态** ✓（本节的出口 ✓）：融合路线**已接完并验证** ✓；
开关 = `OCUDU_LANE_FUSE_EQDEMOD`（env ✓）—— **2026-10-07 用户裁决改为默认开** ✓（判决见 memo ✓），
**`=0` 是回退开关** ✓（旧形状"非空即开、默认关"是 A/B 期间的约定 ✓，已随判决作废 ✓）；
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

### 2.11 ★★ A 路（把 UL 钉成 16QAM）：**已飞完 ⇒ 融合兑现收益** ✓✓（2026-10-06 第 2 次末；判决见 memo ✓，下面保留当时的计划 ✓）

**`mkf010`/`mkf011` 已经飞过** ✓（结论见 memo ✓）：**接线正确** ✓（10 个融合 dispatch 与 10 个 16QAM 跳一一对应 ✓、
账目闭合 ✓、那 10 跳全 `crc=OK` ✓），**但那两条腿跑的是 64QAM** ✗
⇒ 融合只覆盖 **0.009 %** 的跳 ⇒ **判不了融合** ✗。**别按老计划再飞同样的两条** ✗。

> **结果** ✓：`mkf012`/`mkf013` **跑完** ✓ ⇒ 融合覆盖 **99.997 %** ✓、dispatch/跳 **4.0000 → 3.0000** ✓、
> `busy` **478.9 → 469.6 µs** ✓、`ul_pipeline` **1280 → 1259** ✓、`ul_gpu_pipeline` **1237 → 1214** ✓、
> CRC **100.0 % vs 99.9 %** ✓、`pair_check` **PASS** ✓ ⇒ **M1 判决：融合有价值** ✓✓。
> **下一个动作 = B** ✓（§2.13 ✓：`mkf014`/`mkf015` ✓，用原来的 `gnb_pinned_mcs13.yml` ✓）。

**两条路（用户裁决 ✓：先 A 再 B ✓）**：

| | **A. 把 UL 钉成 16QAM**（零新代码 ✓）| **B. 把融合 kernel 扩到 64QAM**（新代码 ✓）|
|---|---|---|
| 做法 | 一份**本工作流自己的** config 副本 + **`cell_cfg.pusch.mcs_table: qam64`** ✓（表 1 的 index 13 = 16QAM ✓，正是配置注释本来的意思 ✓）；两条腿都带它 ✓ | 照抄 demapper 的 64QAM 分支进 `ocudu_lane_fused.metal` ✓（3 张 8 项表 + `interval_l` ✓ + 一个 `mod` 分支 ✓），scope 从"16QAM"改成"16QAM + 64QAM" ✓ |
| 代价 | **2 条腿** ✓；负载与 M0 基线不同（16QAM R=490/1024 vs 64QAM R=567/1024 ✓ ⇒ 每 RE 载荷 ≈ −42 % ✗）⇒ **对基线的绝对值不可直接比** ✓，但**对内可比** ✓ | 内核改动 + **重跑离线对拍（64QAM 标注语料 ✓）与单元测试** ✓ + 2 条腿 ✓ |
| 好处 | 最快拿到 **M1 的判决** ✓（融合到底值不值 ✓）| 判决是在**交付形状真正的负载**上做的 ✓（64QAM 才是调度器实际给的 ✓），与 M0 基线同负载 ⇒ 可直接比 ✓ |
| 风险 | 判决是在一个"非交付负载"上做的 ✗；且 kernel 仍服务不了实际流量 ✗ | 范围扩大 ✗（M1.0 的 scope 决议要按新事实改 ✓）；仍要一次飞行 ✓ |

**建议顺序** ✓：**先 A 拿判决**（最便宜、是"要不要继续"的证据 ✓），**若融合兑现收益再做 B** ✓
（B 也是 M1.5 / M2 之前必须做的 ✓ —— 否则融合路线在真实配置下覆盖率≈0 ✓）。**两条路的 code/config 都已就绪 ✓**：
A 的 config = `wip/gnb_mcs16qam.yml` ✓，B 只需改 kernel ✓。

**A 的命令** ✓（config 只换 `LEG_CFG` ✓，其余与已飞的两条腿一致 ✓）：

```bash
cd /Users/jiachengwang/dev/ocudu
export LEG_CFG=$PWD/doc_chinese/metal_kernel_fusion/wip/gnb_mcs16qam.yml
export LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs

# 对照腿（`mcs_table: qam64` + MCS 13 = 16QAM ✓，融合关 ✓）
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf012-m1-16qam-fused-off dual quiet gpu
# 臂腿（**一个变量** ✓）
EXTRA_KNOBS="OCUDU_LANE_FUSE_EQDEMOD=1" \
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf013-m1-16qam-fused-on dual quiet gpu

# 判读（注意 pair_check 要显式给 LEG_LOGDIR ✓，它没有泛搜 ✓）
bash doc_chinese/macos_thread_priority/wip/ul_health.sh  mkf012-m1-16qam-fused-off
bash doc_chinese/macos_thread_priority/wip/ul_health.sh  mkf013-m1-16qam-fused-on
bash doc_chinese/macos_thread_priority/wip/dl_gate.sh    mkf013-m1-16qam-fused-on
LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs \
  bash doc_chinese/macos_thread_priority/wip/pair_check.sh mkf012-m1-16qam-fused-off mkf013-m1-16qam-fused-on
```

**判读要点** ✓（先确认**覆盖率** ✓，再看数字 ✓）：
1. 臂腿日志里必须有 `PUSCH: FUSED equalization + demapping chain enabled (… 16QAM)` ✓
   与 `[eq_impl] fused equalizer+demapper (lane_grid_to_llr): available` ✓；
2. `PUSCH: rnti` 行的 `mod=` 应**几乎全是 16QAM** ✓（这就是 A 是否奏效的判据 ✓）；
3. `[metal_stats] burst … dispatches` 应**每跳 3** ✓（`demapper=0` ✓）、
   `eq_batch … sites(y_fused≈跳数 y_batch=0)` ✓；
4. 然后才是 §2.3 的判据 ✓（`busy` ✓ / `[ul_pipeline]` ✓ / CRC 同档 ✓ / `gaps=0` ✓）。
**若出现** `PUSCH: OCUDU_LANE_FUSE_EQDEMOD is set but the fused route is NOT taken (…)` ✓
⇒ 括号里就是原因 ✓（调制不对 ⇒ A 没生效 ✓），**不要**读成"融合无收益" ✗。

### 2.12 未决项与重开条件（M1 期间 ✓）

| 项 | 状态 | 重开条件 |
|---|---|---|
| **融合路线的收益判决（16QAM）** | **已判 ✓：有价值** ✓（`busy` −1.9 % ✓、`ul_gpu_pipeline` −1.9 % ✓、CRC 同档 ✓、dispatch 4→3 ✓ —— memo ✓）| 更大的收益要等 M2/M3 ✓ |
| **融合路线的判决（64QAM，交付配置）** | **已判 ✓：复现** ✓（`busy` −1.5 % ✓、`ul_gpu_pipeline` −1.5 % ✓、CRC 臂更好 ✓、dispatch 4→3 ✓ —— memo ✓）| — |
| 臂腿 `stale=4/5`（对照 0）✗（A 路）| **未复现** ✓（B 路两条腿都 stale=0 ✓）⇒ 按"环境+台面"结案 ✓ | 若再出现 ⇒ 单开一条线查 ✓ |
| 唯一变差的项：B 路 `ul_gpu_pipeline` p99.9 +36 µs ✗ | **记账** ✓ | 若后续要引用 p99.9 ⇒ 先弄清是否与那次 2.3 ms busy 离群同源 ✓ |
| 其它调制（QPSK 等）| **M1 不做** ✗（QPSK 只在开局 3 跳 ✓）| 若某条腿的 PUSCH 主体变成 QPSK/256QAM ⇒ 按同一套加 ✓ |
| ★ **lane 内那 ~380 µs 的归属** | **未归属** ✗（本轮只拿到"主体在等待里"的旁证 ✓）| 给 Q9-B 补中位数 ✓ ⇒ 下次飞行即可读到 ✓；再据此决定 M2 ✓（§2.14 ✓）|
| **融合路线是否翻成默认开** | ★ **已裁决 ✓：默认开** ✓（用户 2026-10-07 ✓，两对腿为据 ✓；`=0` 为回退 ✓）| — |
| lane 外那 ~760 µs（host FFT + 网格打包）| **范围外** ✗（用户已排除 Metal FFT ✓；M0c 证明搬设备更差 ✗）| 用户重开范围时 ✓，且要换一条路（不是 M0c 那条 ✗）|
| "不要 `nv` 输出"的 A/B（省 4 B/RE ✓）| 未做 ✗ | 融合收益兑现之后 ✓；它要**同时**换 SINR 方法（`--pusch_sinr_calc_method=channel_estimator` ✓），所以是**两条腿一对** ✓ |
| **EVM 与融合不兼容** ✓ | 已明确 ✗（EVM 读 `eq` ✗，腿里默认关 ✓）| 若要 `pusch_sinr_calc_method=evm` + 融合 ⇒ kernel 需再写 `eq`（8 B/RE ✗），届时按同一套加 ✓ |
| 其它调制（QPSK/64QAM/256QAM）| **M1 不做** ✗ | 融合在 16QAM 上兑现收益之后 ✓（加分支照抄 demapper ✓）|
| 多层的融合（2/4 层）| **M1 不做** ✗ | 同上 ✓（kernel 目前只有 `L==1` 分支 ✓）|
| helper 抽取为共享头 ✓ | 未做 ✗（当前是**复制** ✓）| 融合被证明有价值之后 ✓（抽取会改写交付形状里的两个 kernel ⇒ 需要自己的 A/B ✓）|
| M2（折 CE）/ M3（metallib 整合）| 未开始 ✓ | M1 出结论之后 ✓ |
| 离线工具收尾竞态 ✗ | **既有** ✗（`ul_chain_replay` 退出时偶发 `mutex lock failed` ✓，**对照臂也有** ✓，dump 已写完 ✓）| 若它开始影响判读（`missing-dumps>0` ✗）⇒ 单开一条线查 ✓ |
| 语料只有 QPSK ✗ | 已绕过 ✓（造了标注 16QAM 的同网格语料 ✓）| 若要有**真实** 16QAM 离线语料 ⇒ 需要一次空中 capture（`OCUDU_UL_DUMP` ✓ + MCS 13 ✓）✓ |

### 2.13 ★ M1.5（B 路）：把融合 kernel 扩到 **64QAM**（2026-10-06 第 2 次末，**已实现并离线验证 ✓**，待飞 `mkf014/015` ✓）

**为什么** ✓：`mkf010/011` 实测 PUSCH 走的是 **64QAM** ✗（MCS 表 2 的 index 13 ✓）
⇒ 只做 16QAM 的融合路线在**真实配置**下覆盖率 ≈ 0（10/108316 ✓）。
**A 路**（把 UL 表换成 qam64 ✓）能让 M1 拿到判决 ✓，但它**改的是被交付配置** ✗
—— 融合要能落地，就必须覆盖调度器实际给的调制 ✓。

**改动** ✓（**照抄 demapper 的分支** ✓，与 16QAM 同一条纪律：复制而不抽取 ✓）：

| 层 | 改动 |
|---|---|
| kernel ✓ | `ocudu_lane_fused.metal`：新增 `MOD_QAM16 = 1` / `MOD_QAM64 = 2` ✓（**与 demapper 引擎同一套编号** ✓）、64QAM 的 3 张 8 项表 + `interval_l` ✓（逐字抄 ✓）、`constant uint& mod [[buffer(8)]]` ✓、`if (mod == MOD_QAM64) { 6 个 LLR }` ✓；**`nv` 的写在分支之前** ✓（与调制无关 ✓）|
| 引擎 ✓ | `eq_pending_t` 加 `mod` ✓；run 谓词加 `same_mod` ✓（**一次 dispatch 只带一种调制** ✓）；`llr_bytes_per_re(mod)` 取代写死的 4 ✓（16QAM=4 / 64QAM=6 ✓）；编码器绑 b8 ✓；`enqueue_fused(..., mod, ...)` ✓ 并**第二次把关**（只放 1/2 过 ✓）|
| 后端 ✓ | `supports_fused_demapping()` 接受 **QAM16 或 QAM64** ✓ + 1 层 ✓ + kernel 在库 ✓；`mod_id_of()` ✓；`run_fused` 用调制算 LLR 尺寸断言 ✓ |
| 调用方 ✓ | **不改** ✓ —— 它本来就是 `supports_fused_demapping(config.modulation, …)` ✓ 与 `submit_fused(…, config.modulation)` ✓ |
| 测试 ✓ | 单元测试的 `[fused]` 段改成**对 16QAM 与 64QAM 各跑一遍** ✓（谓词 ✓ + LLR/nv 与两步逐位一致 ✓ + `eq` 不被写 ✓）|

**离线验证** ✓（B 的出口 ✓，**全部通过 ✓**）：
1. `xcrun metal -c ocudu_lane_fused.metal` **exit 0** ✓（9 104 B AIR ✓）；
2. 单元测试 `[fused]` **对两种调制各一遍** ✓：`16QAM` 与 `64QAM` 的
   **LLR 与 nv 都与两步路线逐位一致** ✓、`eq` 都不被写 ✓、谓词只对 16QAM/64QAM + 1 层放行 ✓ ⇒ **ALL OK** ✓；
   `ctest -R channel_equalizer_metal` **2/2 PASS** ✓；
3. **离线对拍（64QAM 标注语料 ✓）**：27 capture、四个 dump **差异字节 0/0/0/0** ✓✓
   （`ab_dumps.sh "OCUDU_UL_REPLAY_NO_EVM=1" "OCUDU_UL_REPLAY_NO_EVM=1 OCUDU_LANE_FUSE_EQDEMOD=1" --metal "/tmp/q64c/*.bin"` ✓）；
   **16QAM 语料复跑也仍是 0/0/0/0** ✓ ⇒ 第一条分支没有被这次扩展碰坏 ✓；
4. `probes_off_syntax_check.sh` **34 TU 全过** ✓、`gnb` 重编 ✓、指纹重戳 ✓。

**出口腿** ✓：**`mkf014`（对照）/ `mkf015`（臂）** ✓，用**原来的 `gnb_pinned_mcs13.yml`** ✓
（**不加任何 MCS 覆盖** ✓ ⇒ 负载与 M0 基线同形 ✓，可直接比 ✓），一个变量 = 融合开关 ✓。
判读要点同 §2.11 ✓，但**第 2 条改成**：`PUSCH: … mod=` 应几乎全是 **64QAM** ✓、
`[metal_stats] burst … dispatches` 每跳 **3** ✓、`sites(y_fused≈跳数 y_batch=0)` ✓。

**这一步之后** ✓：M1 才有"在交付配置上"的判决 ✓；M2/M3 与"不要 nv"的 A/B 都排在它后面 ✓。

### 2.14 ★★ M1 之后：先**归属**，再决定 M2 / M3（2026-10-06 第 2 次末 ✓）

**M1 已经做完的事** ✓：`dispatches`/跳 **4 → 3** ✓、eq/nv 中间缓冲不再被写/读 ✓、
交付配置上的收益 −1.5～−1.9 % ✓、长尾 −15～−28 % ✓、功能零代价 ✓。
**M1 没能动的事** ✓：一跳仍然是 **2 条命令缓冲**（`cbs/lane = 2.00` ✓，四个腿都是 ✓）、
`[ul_gpu_pipeline]` 1242.8 → **1226.8** µs ✓ —— 因为收益全在 lane 内那 480 µs 里 ✓。

**待归属的两块时间** ✓（M0 的分解 ✓，现在都还"说不出几何" ✗）：

| 块 | 量 | 已知 | 未知 ⇒ 第一步 |
|---|---|---|---|
| lane **之外** | **~760 µs**（61 %）| **CPU 的 FFT + 网格打包** ✓（`gpu` 模式下 DFT 默认走 CPU ✓，代码原话在 M0 memo ✓）；M0c：**把它搬上设备更差** ✗（+197 µs 跨度 ✓）| 这是**范围问题**（用户已排除 Metal FFT ✓）⇒ 若要碰，先与用户重开 ✓，并且**不是**照 M0c 那条路重做 ✗ |
| lane **之内** | **~380 µs**（busy 480 里除内核/dispatch/host 编码之外 ✓）| 不是内核算术 ✓（M0 ✓）；中位 CB 14 % 排队 / 86 % 设备 ✓；**不是 fence** ✓✗（mkf018/019 ✓）；★ **不是兄弟 lane 争用的主体** ✓✗（mkf022 并发 1：窗口只 −10 % ✓，但**长尾 −23 %** ✓）；一跳 `merged_hop` 有 **~6.9 个 dispatch** ✓ ⇒ 剩下的候选 = **这些 dispatch 的空口价格** ✓（空口 66 µs vs 离线 3.7 µs ⇒ 与仓里 `OCUDU_CE_CORR_MERGED` 注释记的"空口 39 / 离线 1.6 µs"同一个 20～25 倍比 ✓）| ★ **下一个测** ✓：`OCUDU_CE_CORR_MERGED=1`（一个 env ✓）⇒ **先读 `ce_sites merged=` 是否真动** ✓，再读窗口 ✓ = 直接给一个 dispatch 定价 ✓ |
| **M2 的前提** | ★ **本工作流已证否** ✗（空口 ✓）—— 那 ~456 µs **不是跨 CB 的等待** ✓ ⇒ **折 CE 不会因为它而回本** ✗ | 只有在"争设备/串行化"被证实**且**折 CE 能改变它时才重开 ✓（§3 ✓）；**在此之前不做 M2** ✗ |
| ★ **那 ~456 µs 信封** | ★★ **五个假设全部证否** ✗✗（M0 内核 ✓、mkf019 fence ✓、mkf022 争用 ✓、mkf023 dispatch 价 ✓、**mkf024 CB 边界 ✓**）⇒ 可加起来的只有 ~7 dispatch × ~7 µs ≈ 50 µs ✗；★ **且"更细的 GPU 计时"这条路本机不存在** ✗（M4 Pro `atDispatchBoundary = NO` ✓，已离线查证 ✓）| ★ **归因的边际收益已为 0** ✗（能想到的结构全量过 ✓，§2.17 ✓）⇒ **要么接受它是平台属性 (a)** ✓、**要么只做纯经验法的结构变体 (b)** ✓ —— 但 (b) 的候选也已用尽 ✗ |

**AMC 腿的命令** ✓（`wip/gnb_amc.yml` ✓ = 钉住版去掉 `min/max_ue_mcs` ✓；★ **路线已默认开，
所以对照臂要显式 `=0`** ✓）：

```bash
cd /Users/jiachengwang/dev/ocudu
export LEG_CFG=$PWD/doc_chinese/metal_kernel_fusion/wip/gnb_amc.yml
export LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf016-m1-amc-fused-off dual quiet gpu
EXTRA_KNOBS="OCUDU_LANE_FUSE_EQDEMOD=0" \
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf017-m1-amc-fused-on dual quiet gpu
```
**判读** ✓：调制直方图（`PUSCH: rnti` 的 `mod=` ✓）→ `sites(y_fused / y_batch)` ✓（覆盖率拆分 ✓）
→ CRC / `gaps` ✓（红线 ✓）→ 再看 `busy` / `ul_pipeline` ✓。
**预期** ✓（kernel 现在三支都有 ✓）：**16QAM/64QAM/QPSK 的跳都融合** ✓，只剩 **256QAM 与多层**退回两步 ✓。

**⇒ 建议的顺序** ✓（每步可停 ✓，一条腿一个变量 ✓）：
1. **（零成本，本会话已完成 ✓）** 归属读数：`ocudu_metal_lane_probe.mm` 新增两条 **全样本**序列 ✓
   —— `commit -> start (Q9-B, the queue)` 与 `start -> end (Q9-B, the device)` ✓
   （原来只有"最慢 8 个"的表 ✗）⇒ **下一次飞行就能读出中位 CB 的时间去向** ✓；
   **不改交付行为** ✓（只加报告 ✓）、`probes_off_syntax_check.sh` 仍要过 ✓（已过 ✓）。
2. **★ 决策点（用户 ✓，已裁决 ✓）**：**翻成默认开** ✓（2026-10-07 ✓）——
   两对腿已证明它是**严格改善** ✓（−1.5～−1.9 % + 长尾 ✓，功能零代价 ✓）；
   `OCUDU_LANE_FUSE_EQDEMOD=0` 为回退 ✓，M4 的"交付默认值不变"由这次裁决取代 ✓（见 memo ✓）；
3. **M3（metallib 整合，3 → 1 ✓）**：与上述独立 ✓、便宜 ✓、不动时间 ✓（G3 ✓）⇒ 可以**并行或先做** ✓；
4. **M2（折 CE）**：**前提要按 M0 的结论重写** ✓ —— 不是"`ch_wt` 那 36.8 µs 值不值" ✗，
   而是"**去掉 CE↔lane 的边界能不能动那 ~380 µs**" ✓；M2 的试点仍应是
   "只折权重计算 + 自己的旋钮 ✓"（§3 ✓）。**在 (1) 给出读数之前不做** ✗。

### 2.15 ★ M1.6：把 **QPSK** 也补进融合 kernel（2026-10-07，**已实现并离线验证 ✓**，待 AMC 腿 ✓）

**为什么** ✓：AMC 腿（把 MCS 钉住取消 ✓）里链路自适应会在**边缘走 QPSK** ✓；
kernel 没有这一支时，那些跳**全部退回两步路线** ✗ ⇒ 覆盖率会在最需要的地方掉下去 ✓。
这也是"覆盖面要跟着调度器实际给的调制走"的最后一块 ✓
（剩下的 256QAM 与多层仍按 §2.12 的重开条件处理 ✓）。

**改动** ✓（**照抄 demapper 的 QPSK 分支** ✓，与前两支同一纪律 ✓）：

| 层 | 改动 |
|---|---|
| kernel ✓ | `MOD_QPSK = 0` ✓、`GAIN_QPSK = 2.82842708f` ✓、`if (mod == MOD_QPSK) { 2 个 LLR }` ✓ —— ★ **range limit 是 `24.0f` 而不是 QAM 的 `20.0f`** ✓（demapper 对两种星座的缩放不同 ✓），这一行最容易抄错 ✓ |
| 引擎 ✓ | `llr_bytes_per_re()` 改成 `switch`（QPSK **2** / 16QAM 4 / 64QAM 6 ✓）；`enqueue_fused` 的第二道闸从"只放 1/2"改成"`mod <= 2`" ✓ |
| 后端 ✓ | `supports_fused_demapping()` 接受 **QPSK/QAM16/QAM64** ✓；`mod_id_of()` 加 QPSK → 0 ✓；`run_fused()` 的 LLR 尺寸断言按调制算 ✓ |
| 调用方 ✓ | **不改** ✓（它本来就是逐跳拿 `config.modulation` 去问 ✓）|
| 测试 ✓ | `[fused]` 段从两调制改成**三调制各跑一遍** ✓（谓词：QPSK/16QAM/64QAM = 是 ✓、256QAM = 否 ✓、2 层 = 否 ✓）|

**离线验证** ✓（**全部通过 ✓**）：
1. `xcrun metal -c` **exit 0** ✓（9 344 B AIR ✓）；
2. 单元测试 **QPSK / 16QAM / 64QAM 三支都与两步路线逐位一致** ✓（LLR 与 nv ✓、`eq` 不被写 ✓）⇒ **ALL OK** ✓；
3. ★ **离线对拍用上了真正的 QPSK 语料** ✓✓ —— 原来的 27 个 capture **本来就是 QPSK** ✓
   （前两轮之所以要"标注"是因为 kernel 当时只做 QAM ✓）⇒ 现在 **QPSK / 16QAM / 64QAM 三份语料
   各 27 capture、四个 dump 全部 0/0/0/0** ✓；
4. ★ **注意 A/B 的对照臂写法变了** ✓：路线默认开之后，对照必须是
   `OCUDU_UL_REPLAY_NO_EVM=1 OCUDU_LANE_FUSE_EQDEMOD=0` ✓、臂是 `OCUDU_UL_REPLAY_NO_EVM=1` ✓
   —— 第一次跑成"两边都默认开"⇒ 那是**自己跟自己比** ✗（工具不报错 ✓，只有读 `y_fused` 才看得出来 ✓）；
5. probes-off **34 TU** ✓、全量构建绿 ✓、指纹重戳 ✓。

### 2.16 ★ B′ 两项的判读：**`nv` 不值得飞 ✗、并发 1 的"长尾杠杆"是假的 ✗**（2026-10-07，**纯离线判读，未飞腿 ✓**）

用户裁决走 **B′** ✓（"先做**已验证可行性**的小额收益实验" ✓）。两项都在**读现成数据**这一步就被判完了 ✓
—— **一条腿都没花** ✓。★ 第二项推翻了 handoff §①E 里的一条结论 ✗ ⇒ **先读本节再看那一处** ✓。

#### (1) ✗ `nv` 输出的 A/B：**不飞** ✓（收益上界 ~1 µs，落在噪声底里 ✓）

handoff §④ 把它列为"未做 ✓、可做 ✓"，但当时只写了"省 4 B/RE ✓"**没有分母** ✗ ⇒ 容易被读成一笔大钱 ✓。
把分母补齐（全部是已测数字 ✓）：

| 量 | 值 | 出处 |
|---|---|---|
| 融合**总**收益 | **−7.4 / −7.6 µs/跳** | `mkf012/013`、`mkf014/015` ✓ |
| 该收益对应的**中间流量**变化 | 去掉 eq 写 + eq 读 + nv 读、加上 nv 写 ⇒ **净 −16 B/RE** | kernel 注释 ✓ |
| ⇒ `nv` 写在其中的份额 | **4 / 16 = 25 %** | — |
| ⇒ **`nv` 的收益上界** | **≤ 7.6 × 25 % ≈ 1.9 µs** | 归谬 ✓ |

★ **更强的一刀** ✓：`merged_hop`（≈412 µs）里 **~6.9 个 dispatch** ✓、**一个边界 ≈ 6.5 µs** ✓（`mkf023` ✓）
⇒ 7.6 µs 的融合收益里**大头是那次 dispatch 消除** ✓、**整段 16 B/RE 流量只剩 ~1 µs** ✓
⇒ `nv` 那 4 B 只值 **~0.3 µs** ✓。**两种算法都落在噪声底下** ✓ ⇒ 飞一对腿（两条 × ~4 分钟 + 判读 ✓）
只为测一个 ≤2 µs 的量 ✗ —— **不做** ✓，等它被别的改动顺带带出来再说 ✓。

#### (2) ★★ 更正：**并发 1 不是"−24 % 长尾杠杆"，它在交付配置上全面更差** ✗✗

**错在哪** ✗：那个"−23～24 % 长尾"是拿 `mkf022`（臂）去比 **`mkf021`（对照）** ✓，
而 **`mkf021` 自报 `PUSCH/SRS concurrency = 2`** ✓ ⇒ **对照臂根本没进并发 1** ✗。
handoff §①E 只把 `mkf020/021` 记成"因命令错未生效" ✓、**却让 `mkf021` 去当了对照** ✗ ——
★ **"旋钮没进进程"与"没效果"在日志里一样** ✓ 这条纪律，这次是**用在对照臂上**才看出来的 ✓。

**四条腿并排** ✓（并发取自各自的自报行 ✓，不是命令行 ✓）：

| 腿 | 自报并发 | `ul_to_lane` p50 | `busy` p50 | `ul_gpu_pipeline` p50 / p99 | `ul_pipeline` p50 | 超 2000 µs |
|---|---|---|---|---|---|---|
| `mkf018` ✓ | 2 | 22 µs | 472.6 µs | 1227.0 / 1526.1 µs | 1305.0 µs | 35 |
| `mkf020` ✓ | 2 | 22 µs | 473.5 µs | 1227.8 / 1528.1 µs | 1306.0 µs | 39 |
| `mkf021` ✓ | **2** ✗ | 22 µs | 472.8 µs | 1226.2 / 1526.2 µs | 1303.0 µs | 27 |
| **`mkf022`** ✓ | **1** ✓ | **351 µs** ✗ | **433.1 µs** ✓ | **1455.3 / 1787.1 µs** ✗ | **1534.0 µs** ✗ | **318** ✗ |

**更正后的读法** ✓（拿同配置的三条并发 2 腿当基线 ✓，它们彼此重复到 ±1 µs ✓）：

* ✗ **p50 更差 +18.5 %** ✓（1226.5 → 1455.3 µs）、**p99 更差 +17 %** ✓（1526.1 → 1787.1 µs）
  —— 方向上**正好反过来** ✗（原话是"p95/p99 −23～24 %" ✗）；
* ✗ ★ **超预算窗口 27～39 → 318（≈10 倍）** ✓ —— 交付配置的真正判据落在 2000 µs 这条线上 ✓
  （`print_window_stability(..., 2000.0)` ✓）⇒ **并发 1 是唯一一个把超预算窗口翻十倍的改动** ✗；
* ✓ **唯一真的变好的** ✓：`busy` **472.8 → 433.1 µs（−8.4 %）** ✓
  ⇒ ★ **设备侧真的更省** ✓（串行化让设备少了争用 ✓、每跳的 dispatch 挤在一起 ✓）。

**机制** ✓（不是巧合 ✓，自报行与代码注释都写明了 ✓）：
`max_pusch_and_srs_concurrency <= 1` **不是"上限 1"** ✗，而是 `create_task_fork_limiter()` 返回一个
**STRAND** ✓ —— `du_low_executor_mapper.cpp:227` 的原话：
"*a serialising STRAND: ONE PUSCH hop at a time*" ✓。
⇒ 车道**串行** ✓ ⇒ 一跳处理完、下一跳还没到时 **lane 线程停摆** ✓（停顿分类给出同一个方向 ✓：
`exec.park` **1 → 5** ✓、`uninstrumented` **7 → 94** ✓），
下一跳到达要先唤醒 ✓ ⇒ `ul_to_lane`（lower_phy_ul#0 → 车道开跑 ✓）**22 → 351 µs** ✓。
★ 而这 **+329 µs 的等待** 正是 `ul_pipeline` 那 **+228 µs** 的来源 ✓
（设备侧省下的 44 µs 远远不够抵 ✓）⇒ **净负** ✗。

**⇒ 本项的结论** ✓：**并发 1 不进交付配置** ✗。它换来的是**设备侧 −8.4 % 的 busy** ✓
（这也解释了 §①E 里"窗口只 −10 %"那个观察 ✓：busy 确实降了 ✓），
代价却是**端到端 p50/p99 各差 ~17～19 %** ✓ 与**超预算窗口 ×10** ✓ —— **账面明确为负** ✓，
**不需要再飞腿验证** ✓。

### 2.17 ★★ B 路结构变体：**跨 CB 边界在空口上也测不出来** ✗ —— 第五个假设证否 ✓（`mkf024-split` ✓，2026-10-07）

**为什么做这个** ✓：§①E 的四轮归因之后，"一跳 ≈412/456 µs 的信封"还剩**唯一一个没被消去的结构**
✗ —— **命令缓冲边界**本身 ✓（fence 已被 `mkf018/019` 消去 ✓、dispatch 边界已被 `mkf023` 定价 ✓，
但"**CB 与 CB 之间**"从没单独量过 ✓）。而本路线**每跳本来就跨一次** ✓
（CE 的权重自己提交一条 ✓、lane burst 再一条 ✓，所以 `cbs/lane` 的基线是 **2.00** ✓）。

**臂** ✓：`OCUDU_LANE_SPLIT_CB=N` ✓ —— 把 lane **自己的** burst 在第 N 个 dispatch 之后**切成两条** ✓。
挂在 `count_dispatch()` 上 ✓（车道每个 dispatch 都经过的唯一一点 ✓ ⇒ 进度精确 ✓、关掉时只花一次比较 ✓）。
★ **第一半刻意不编 fence** ✓：lane burst 创建时已经拿到 CE 的 stage fence 与网格 fence ✓，
第二半只读**同一 burst 第一半**写的东西 ✓、同一条队列 ⇒ 顺序由提交顺序给 ✓ ⇒ **不欠新的 fence** ✓；
若编一个，测到的就变成 **fence 而不是边界** ✗ —— 那正是 `mkf019` 存在的意义 ✓。

**臂自己在两项上可验证** ✓（这是本工作流反复买来的教训 ✓）：

| 检查 | 期望 | 实测 |
|---|---|---|
| 切分计数 ✓ | 非零 ✓ | **108 343** ✓（= 该腿的 lane 数 108 343 ✓ ⇒ **每跳都切到了** ✓）|
| `cbs/lane` ✓ | 1.00 → 3.00 ✓ | **3.00（max=6）** ✓ |
| 数值正确性 ✓ | 逐位一致 ✓ | **两份语料 × 27 capture × 4 dump = 0/0/0/0** ✓（`q16c` 与 `q64c` ✓）|

**★ 空口结果：边界几乎不要钱** ✗✓（拿三条同配置并发 2 腿当基线 ✓，它们彼此 ±1 µs ✓）：

| 读数 | 基线簇（`mkf018/020/021` ✓）| **`mkf024-split`** ✓ | 差 |
|---|---|---|---|
| `busy` median ✓ | 472.6 / 473.5 / 472.8 | **472.8** | **±0.2 µs** ✓（噪声底内 ✓）|
| `busy` mean ✓ | 492.0–494.3 | **496.7** | +3～+5 ✓ |
| `busy` p95 / p99 ✓ | ~627 / ~699 | **630.4 / 697.6** | = ✓ |
| `merged_hop` ✓ | 455.9 / 457.0 / 455.9 | **457.3** | **+0.3～+1.4 µs** ✓ |
| `ul_pipeline` median ✓ | 1303.0–1306.0 | **1308.0** | +2～+5 ✓ |
| `ul_gpu_pipeline` median ✓ | 1226.2–1227.8 | **1231.5** | +3.7～+5.3 ✓ |
| `start -> end` median ✓ | 221–223 ✓（**均值** 246–247 ✓）| **39.9**（均值 165.0 ✓）| ✗ **换义，见下 ✓ 不可直读** ✗ |

**⇒ ★ 加了一整条 CB 边界（一次 commit ＋ 一次 `endEncoding` ＋ 一次设备侧缓冲切换 ＋ 一次库登记 ✓，
后半的采样数因此翻了一倍：`burst commits` 216 686 = 2 × 108 343 ✓），代价在噪声底里测不出来 ✗**
（≤1 µs ✓）。★ 这是**真结果不是"臂没生效"** ✗：切分计数与 `cbs/lane` 都证明它**确实切了** ✓
—— 这正是"臂不响"与"边界不贵"必须分开的那条线 ✓。

**⇒ 第五个假设证否** ✗：`446 µs` 的信封**不是被 CB 边界吃掉的** ✓。
可加项到此仍是 **~7 dispatch × ~7 µs ≈ 50 µs** ✓ ⇒ ★ **归因的边际收益已经到零** ✓：
能想到的结构（内核 ✓ / fence ✓ / 争用 ✓ / dispatch 边界 ✓ / CB 边界 ✓）**五个全部被腿量过并证否** ✗，
而平台**不给逐 dispatch 计时** ✗（§①E 那条 ✓）。

**★ `start -> end` 这个读数要小心 ✓（我第一版写错过 ✗，核对五条腿才改对 ✓）**：
它的中位从基线簇的 **221.4 / 223.2 µs** 掉到 **39.9 µs** ✓，但**不可直读** ✗ ——
**切分改变了采样** ✓（基线每跳**一条** CB、切分后**两条** ✓，样本 216 801 → 325 032 ✓），
而**两侧 p95/p99 几乎不动** ✓（561.6/619.5 → 547.4/605.5 ✓）。
⇒ 中位下落是**"一条大 CB 变成两条小 CB"的采样后果** ✓，不是设备变快 ✓。
★ 而且**这个现象已被记录过** ✓：`mkf023` 那条 memo 已经写下"设备 median 221 → 366 ✗ 是**分布形状变化** ✓
（与 `mkf021` 的 373 同一现象 ✓）—— **记下不解释** ✓，**引用时只用均值** ✓（均值可加且闭合 ✓）" ✓。
**本条给那条纪律补了第二个实例与一个机制** ✓：形状变化来自**CB 条数/每 CB 工作量**的改变 ✓
⇒ **比"不解释"更进一步：要说明采样变了** ✓。
★ **★ 由此得到一条口径修正** ✓：**均值**在基线上是 246–247 µs ✓ 而**端到端 `busy` 是 472.8** ✓
⇒ 两跳 CB 的均值之和 ≈ 493 ✓ 与 busy 自洽 ✓；但**单条 CB 的窗口里含等待** ✓
⇒ 把它读成"内核算了多久"会把 §①E 那条"**中位 CB 86 % 在设备**"读歪 ✗ ⇒ **那个分解口径需要重读** ✗。
★ **五个假设的证否不依赖这个口径** ✓（内核 ✓ M0 消融、fence ✓ 空口、争用 ✓、dispatch 价 ✓ 计数+窗口、
CB 边界 ✓ 本条 ✓ 都是**独立的结构对照** ✓）⇒ **结论不动** ✓、动的是分解口径 ✓。

**★ 教训（第四条方法论 ✓）**：**引用一条读数前先核对它在哪条腿上** ✗ ——
第一版我把基线写成 **366 µs** ✗，那是**并发 1 的 `mkf022`** ✓、**不属于基线簇** ✓
（基线簇 = `mkf018/020/021` ✓）。★ 这**正是**同一轮在并发 1 上刚抓到的**同一类错误** ✓（拿错腿当基线 ✓）
—— **一天之内第二次** ✗ ⇒ 已并入纪律 ✓（判读清单里加一条：**每个基线数字都要能指回腿号** ✓）。
（这条读数此前被当成"设备时间"，并据此推过"86 % 在设备" ✓ —— 那个**分解口径**现在需要重读 ✓）。

**红线** ✓：`stale=0` ✓✓、**`gaps=0`** ✓、`rx_overflows=0` ✓、停顿 profile 与簇同级 ✓
（`phy_series=12` ✓ vs `mkf021` 的 11 ✓、`exec.park=1` ✓）——
CRC 该腿 **95.6 %（marginal）** ✗，但**同轮的 `mkf023` 是 93.1 %（DEGRADED）** ✓、
且**离线两份语料 0 差异** ✓ ⇒ **判定为环境/流量，与臂无关** ✓。

### 2.18 ★★ M4 验收：**三项全部结清** ✓ —— 但"探针 OFF 可编"这一项**原本不通过** ✗，修出一类**既有缺陷** ✓（2026-10-07）

**判据** ✓（架构文档 §7 那条 ✓）：① ~~交付形状默认值不变~~ ✓ **已被用户裁决取代** ✓（融合**翻默认开** ✓）；
② **Linux 不变** ✓；③ **探针 OFF 可编** ✓；④ **§2.2 红线** ✓；⑤ **一张"融合前后"对照表**（含**允许的回退** ✓）。

#### ① Linux 不变 ✓（**静态核验** ✓，本机没有 Linux 工具链 ✓）

| 检查 | 结果 |
|---|---|
| Metal 目录的进入条件 ✓ | `lib/phy/CMakeLists.txt:12` `if(METAL_OFFLOADS_ENABLED)` ✓；该变量由五个 `ENABLE_METAL_*` 聚合 ✓，而非 Apple 平台上它们**默认 OFF** ✓ 且**显式打开会 `FATAL_ERROR`** ✓ ⇒ 全部 metal 目录（含 `lib/phy/metal` ✓）**在 Linux 上根本不进入** ✓ |
| 新增 include 是否泄漏平台依赖 ✓ | 逐条查过 ✓：`log_likelihood_ratio.h` 与 `modulation_scheme.h` 是**平台无关类型** ✓（加在共享接口 `channel_equalizer.h` 里 ✓）；`demodulation_mapper_metal_factory.h` 只出现在**Metal 门控的测试**里 ✓（它在 Metal 关闭时**自带 stub** ✓，`upper_phy_factories.cpp` 引用它属**既有** ✓）|
| 接口向后兼容 ✓ | 新增的 `supports_fused_demapping()` / `submit_fused()` 在基类**都有默认实现** ✓（谓词返回 `false` ✓）⇒ 既有后端**不受影响** ✓、CPU 路线**永远不走融合** ✓ |
| 调用方是否含平台代码 ✓ | `pusch_demodulator_impl.cpp` 的改动只有 `#include`、`std::getenv` 与接口调用 ✓，**无 Metal 类型、无 `#ifdef __APPLE__`** ✓ |

★ **诚实标注** ✓：这是**静态核验** ✓，不是一次 Linux 构建 ✗（本机不能）——但它核的是**确定性**的 CMake 门控 ✓，
不是概率性的运行行为 ✓。**若要更强的证据，必须在 Linux 上真编一次** ✓（记在 §2.12 ✓）。

#### ② ★★ 探针 OFF 可编：**不通过 → 已修复** ✓✗

**发现** ✓：`ENABLE_FLOW_PROBES=OFF` + `ENABLE_METAL_STATS=OFF` 的**全新 configure + build 直接失败** ✗。
逐层剥开是**同一类缺陷** ✗ —— ★ **运行时功能被声明在探针的条件编译块里，而调用点留在块外** ✓：
★ **清单分两批** ✓：`gnb` 目标暴露 **6 处** ✓；★ **`BUILD_TESTING=On` 的全量构建**（**CI 真正构建的范围** ✓）
又暴露 **2 处**（都在测试侧 ✓，见下 7–8 ✓）⇒ **共 8 处 / 6 个文件** ✓。

| # | 位置 | 被误吞的东西 ✓ | 修法 ✓ |
|---|---|---|---|
| 1 | `ul_stall_watchdog.h` ✓ | `impl* p` 字段 ✓（整个 `.cpp` 在 `#if OCUDU_FLOW_PROBES && __APPLE__` 里 ✓）| `[[maybe_unused]]` ✓（**不**去 guard 那个 include ✓ —— 它**故意**不与探针绑定 ✓）|
| 2 | `ocudu_dft_metal_engine.mm` ✓ | `batch_stats()` 里的 `dft_stats()` 调用 ✓（**同一个调用**在 `token_release_stats()` 里却是 guard 的 ✓）| 两个调用一起收进 guard ✓ |
| 3 | `port_channel_estimator_metal_mmse_impl.cpp` ✓ | ★ **匿名命名空间开在 `OCUDU_CE_TIME` 块里** ✗ ⇒ 关掉 CE 计时 **连带删掉** `note_sigma2()` ✓、MMSE 矩阵缓存 ✓、`matrix_cache_enabled()` ✓ | 命名空间移到块外 ✓；**只有** `mmse_time_stats` 与它的报告留在 guard 内 ✓（这些是**运行时读数** ✓，不是计时插桩 ✓）|
| 4 | `ocudu_metal_mmse_engine.mm` ✓ | `mmse_stats()` 及其计数器类型 ✓（15 个调用点里**有 4 个是普通运行时代码** ✓ —— 就是 MISS 路径的网格等待计数 ✓）| 类型与访问器移出 ✓、**那 4 个点逐个 guard** ✓（保留**分支逻辑**、只护住计数 ✓）|
| 5 | `ocudu_metal_mmse_engine.mm` ✓ | `drop_miss_wait_armed()` ✓ —— ★ **运行时 falsification 旋钮** ✓（L1b 伪证臂 ✓），与 METAL_STATS **毫无关系** ✗ | 移出 guard ✓ |
| 6 | `ocudu_metal_lane_probe.h` ✓ | 探针关闭版枚举**没有 `merged_hop` 标签** ✓，而 `set_commit_label()` **无条件**从运行时代码调用 ✓ | 补上 ✓，**且与插桩版同位置** ✓（枚举成员随调试开关漂移是给下一个读者的陷阱 ✓）|
| ★ 7 | `tests/unittests/support/macos_compat_test.cpp` ✓ | `[sched]` 回读 lambda **无条件定义、只在 `OCUDU_FLOW_PROBES` 下被调用** ✓ ⇒ `-Wunused-variable` ✗ | `[[maybe_unused]]` ✓（定义必须在该 guard **之前** ✓ ⇒ 不能挪 ✓）|
| ★ 8 | `pusch_demodulator_impl.cpp` ✓ | ★ **"ce device estimates" 契约检查与它读的计数器一起在 `METAL_STATS` 块内** ✗ ⇒ 探针关闭时**根本不注册** ✓，而**测试断言它必须注册** ✗ | 计数器与注册**无条件化** ✓、两个计数点**保持 guard** ✓（探针关闭时该检查报 **0 device / 0 host** ✓ = 不计数构建的**事实** ✓）|

★ **7 与 8 是"全量构建"（`BUILD_TESTING=On`）才暴露的** ✓ —— **`gnb` 目标看不到它们** ✓
（一个在测试里 ✓、一个在测试的**期望**里 ✓：缺陷在**对交付物的断言**上 ✓，不在交付物里 ✓）。
★ **这正是 CI 的范围** ✓：`.github/workflows/ccpp.yml` 用**默认选项**（探针因此是 OFF ✓）+
`-DBUILD_TESTING=On` 构建 ✓ —— ⇒ ★ **本工作流的分支"ahead of origin by 22"从未推送** ✓
⇒ **CI 从未跑过它** ✓ ⇒ **这解释了这个缺陷为什么没人报** ✓（**一旦推送，CI 会直接红** ✓）。

**验证三重** ✓（全部通过 ✓）：
1. **三种配置全新编译 0 error** ✓：探针全关 ✓ / **交付默认** ✓ / **全开（含 `ENABLE_CE_TIME`）** ✓；
   ★ 外加 **`BUILD_TESTING=On` 的探针全关全量构建 0 error** ✓（**CI 的范围** ✓）；
2. ★ **行为不变** ✓：**交付形态**的输出与改动前**逐位一致** ✓（四个 dump ✓，对照来自 stash 后重建 ✓）；
3. **相关单测在两种形态下都 PASS** ✓（探针 ON ✓ 含 `[fused]` 段 ✓；探针 OFF ✓ 全量 8254 项**无真实失败** ✓
   —— 那 9 项 "failed" **全是 `Skipped`** ✓（SCTP/E2AP 在本机跳过 ✓）、metal 单测在该形态下**本就不构建** ✓）。

**★ 为什么它一直没被发现** ✗（**与 M3 那次同一族** ✓）：
`ul_stall_watchdog.h` 是 **10-04** 的 ✓，而包含它的 `ngap_cause_converters.cpp.o` 是 **09-25** 的 ✓ ——
★ **增量构建从未重编这些 TU** ✗ ⇒ 树看起来是健康的 ✓，而**全新 configure + build 几秒内就失败** ✓。
**教训** ✓：**"构建目录是最新的"不等于"这棵树编得过"** ✗ —— **验收必须至少做一次全新构建** ✓。

**★ 既有的检查工具为什么没拦住** ✗（查清了 ✓，两个盲点 ✓）：
`wip/probes_off_syntax_check.sh` 的 M3 记录写着"**34 TU 全过 ✓**"，与这次的真实构建**直接矛盾** ✗：
1. ★ **它只去掉 `-DOCUDU_FLOW_PROBES` 一个键** ✗ —— 而这 8 处里绝大多数属于 `OCUDU_METAL_STATS` / `OCUDU_CE_TIME` ✓，
   它**根本不覆盖** ✓（它报的 "the default configuration is safe" ✓ 对那两个键而言是**空话** ✗）；
2. ★ **它只做 `-fsyntax-only`** ✗ —— 第 1 处那个 `-Wunused-private-field` **只在真的 codegen 时**才报 ✓，
   语法检查**看不见** ✓。

**已修工具** ✓（**实测证明它能抓到** ✓✓）：改成对**三个键各来一遍** ✓ + **真编译**（`-c -o /dev/null` ✓）+
文件过滤加宽到 Metal 调试臂 ✓。★ **在修复前的代码上回归**：改进版抓到 **3 处 FAIL** ✓
（`METAL_STATS` 抓 2 ✓、`CE_TIME` 抓 1 ✓），而旧版对同一份代码报 **"34 TU 全过、safe"** ✗ ——
**这就是"检查工具没覆盖"的实证** ✓。修复后：**114 次编译 × 3 键，全 OK** ✓。
（★ 残留盲点 ✓：`ul_stall_watchdog.h` 那种"没被任何 TU 直接使用的头文件"仍不在扫描范围 ✓
⇒ **全新构建仍是最终权威** ✓，脚本是~10 秒的快速反馈 ✓。）

#### ③ 融合前后性能对照表 ✓（**M1 的两对腿** ✓，交付配置在列 ✓）

| | **16QAM**（`mkf012/013` ✓）| | | **64QAM**（`mkf014/015` ✓）| | |
|---|---|---|---|---|---|---|
| | OFF | **ON** | 差 | OFF | **ON** | 差 |
| `busy` p50 / mean (µs) ✓ | 478.9 / 501.6 | **469.6 / 488.3** | **−9.3 / −13.3** ✓ | 479.8 / 506.6 | **472.5 / 493.6** | **−7.3 / −13.0** ✓ |
| `ul_pipeline` p50 (µs) ✓ | 1280 | **1259** | −21 ✓ | 1320 | **1304** | −16 ✓ |
| `ul_gpu_pipeline` p50 (µs) ✓ | 1237.2 | **1214.1** | −23.1 ✓ | 1242.2 | **1226.8** | −15.4 ✓ |
| dispatch / 跳 ✓ | 4.0000 | **3.0000** | 结构 ✓ | 4.0000 | **3.0000** | 结构 ✓ |
| 融合覆盖率 ✓ | 0 % | **99.997 %** | — | 0 % | **99.997 %** | — |
| CRC / 判决 ✓ | 100.0 % CLEAN | 99.9 % CLEAN | 同档 ✓ | 98.8 % marginal | **99.9 % CLEAN** | 臂更好 ✓ |
| `stale` ✓ | 0 | 4（A 路未复现）| — | 0 | **0** | ✓ |

★ **允许的回退** ✓（判据要求列出来 ✓）：**唯一一项** ✓ —— 64QAM 的 `ul_gpu_pipeline` **p99.9 1897 → 1926 µs（+29 µs）** ✗
（A 路的 p99.9 是**改善** 1895 → 1835 ✓ ⇒ 不是路线性质 ✓）。★ **引用 p99.9 前先弄清它与那次 2.3 ms busy 离群是否同源** ✓
（§2.12 的重开条件 ✓）。

**⇒ M4 结论** ✓：**三项结清** ✓ —— Linux 不变（静态核验 ✓，诚实标注 ✓）、探针 OFF **已可编** ✓、
对照表在案 ✓；红线**同档** ✓（CRC ✓、`gaps=0` ✓）。

### 2.19 ★★★ 用户澄清后的重新定位：**工作流的主要目标原本是整合 CE** ✓ —— 但"CE 零碎 = 那几百 µs"这个前提**已被实测证否** ✗✗（2026-10-07）

> **用户的澄清**（原文要点 ✓）：**本工作流的主要目标原本就是整合 CE** ✓ ——
> "CE 的 kernel 太零碎" ✓、且**一直怀疑那几百 µs 的"时间未解之谜"就是因为这些零碎的 CE kernel 的缠绕** ✓；
> **原计划允许修改算法** ✓、**甚至容忍一定的性能下降** ✓。
> ⇒ 本节是对这个前提的**重新核算** ✓ 与**融合规划** ✓（规划见 §2.20 ✓）。

#### (1) "27 个 kernel" 是**库里的总数**，不是一跳跑的个数 ✓（差 4.6 倍 ✓）

| 量 | 值 | 出处 |
|---|---|---|
| 库里 CE 的 kernel 入口 | **29 个** ✓（12 个 `.metal` ✓）| `metal-nm lib/phy/metal/ocudu_lane.metallib` ✓ |
| 其中**每跳真的跑**的 | ★ **5.91 个** ✓ | 微基准 §6.65② 的"每跳次数"列 ✓ |
| lane burst 上的 `channel_estimator` dispatch | **2.00/跳** ✓ | `[metal_stats] burst … channel_estimator=216774 / 108388` ✓ |
| CE 自己那条 CB（`ch_wt`）| **1.00/跳、36.8 µs** ✓ | `[ul_gpu_lane] busy split` ✓ |

★ **两者差在哪** ✓：那 23 个是**替代路线** ✓（`mmse_inv` / `mmse_inv_memnone` / `mmse_inv_rl` 三选一 ✓、
`mmse_weights` / `mmse_weights_matrix` / `mmse_weights_tile` 三选一 ✓、
`mmse_apply` / `mmse_apply_lse` / `mmse_apply_matrix` 三选一 ✓、`mmse_corr_a` / `mmse_corr_a_rhp` / `mmse_corr_r_hp` ✓、
`mmse_ta_*` 三个 ✓、`mmse_pilots_*` 七个 ✓）—— 由旋钮或几何选择 ✓。
⇒ ★ **"零碎"要按"每跳 5.91 个"来算** ✓，不是 27 ✓。

#### (2) ★★ 那几百 µs **不是 CE 内核吃掉的** ✗✗ —— 实测账（微基准 §6.65，**上一工作流的工具** ✓）

| kernel | µs/派发 | 每跳次数 | 每跳 µs |
|---|---|---|---|
| 空派发地板 ✓ | **1.26–1.38** | — | — |
| `mmse_reformat` | 1.52 | 1.00 | 1.5 |
| `mmse_corr_a` | 1.69 | 1.456 | 2.5 |
| `mmse_corr_r_hp` | 6.07 | 1.456 | 8.8 |
| `mmse_apply_lse` | 7.42 | 1.00 | 7.4 |
| `mmse_weights` | **13.18** | 1.00 | 13.2 |
| `mmse_pilots_lse` / `apply_cfo` | 未测（~4–8 估）| 1.00 / 1.00 | ~4–8 |
| **CE 合计** | | **5.91** | ★ **≈38 µs/跳** ✓ |

★ **同一工具还量掉了全部"边界"** ✓（§6.66①）：`memoryBarrierWithScope` ✓、pipeline 切换 ✓、
**encoder 边界** ✓（`endEncoding` + 新 encoder ✓）**四者都 ≤0.3 µs** ✓（噪声量级 ✓）、
宿主 encode 一个派发 **0.18 µs** ✓ ⇒ ★ **"减少边界"这条线没有可赚的量** ✗。

**⇒ 归谬** ✓：CE 的**全部算力只有 38 µs/跳** ✓，占 `merged_hop`（456.8 µs ✓）的 **8.3 %** ✓
⇒ **即使把 CE 全部抹掉，也拿不到那 ~400 µs** ✗ —— **差一个量级** ✓。
★ 而 M0 的消去三条腿**早就显示过**同一件事 ✓（消去后 `busy` **477–480** ✓、不动 ✓）——
**那正是"内核不是成本"的实测** ✓，只是当时读的是 EQ/DEMOD ✓。

#### (3) ★★ 那 ~464 µs 是**等本槽样点到达** ✓ —— **策略问题，不是 CE 问题** ✓（微基准 §6.65③ ✓）

| 项 | 量 | 依据 |
|---|---|---|
| 等本槽样点 | ★ **≈464–473 µs** ✓ | 前端 lane 的 residency 中位 **463.9 µs** ✓ ≈ **一个 30 kHz 时隙 466.7 µs** ✓；与 §3.4 登记的"**A 项 ≈473 µs（零算力）**"✓ **独立吻合** ✓ |
| CE 算力 | ≈38 µs ✓ | §6.65② ✓ |
| 均衡 / 解映射 | ≈1.4–1.6 / 1.25–1.37 µs ✓ | 同工具 ✓ |
| 合计 | ≈470 µs ✓ | ⇒ **账正好闭合** ✓ |

⇒ ★ **一跳 ~456 µs 的构成 = "等整槽样点（~464 µs，零算力）" + "全部 kernels（~40 µs）"** ✓
—— **这与"五个假设证否"完全一致** ✓（§2.17 ✓）：那五个假设都在找"kernel/边界"的账 ✓，
而**真正的账在"等样点"上** ✓。★ **我们（本工作流）五轮归因的结论"平台属性"虽然方向没错 ✓，但太笼统** ✗
—— 上一工作流早已把这一项命名为 **A 项**并量过 ✓，**本次重新核算把它接回来了** ✓。

#### (4) ★ 打破它的路**也已经飞过并给出否定结果** ✗（`OCUDU_UL_RX_SYMBOLS=1` ✓）

符号级收包（`P1-7` ✓）是唯一能碰那 ~464 µs 的旋钮 ✓（§6.65④ ✓）—— **腿 `p41-n78-rxsym` 飞过** ✓：

| 读数 | `RX_SYMBOLS=1`（符号级）| 整槽 | 判读 |
|---|---|---|---|
| `[ul_gpu_pipeline]` 中位 ✓ | **2043.1 µs** ✗ | 1366.8 | **+676 µs（+49 %）** ✗ |
| `[ul_rx_wait]` 中位 ✓ | 0.0 | 473.0 | **收包侧确实更早拿到样点** ✓（A 项被**真的**消除了 ✓）|
| ★ `[ul_gpu_lane] dft cbs` ✓ | **2 497 686**（**14.0 cb/槽**）✗ | 60 868（0.39/槽）| **前端每符号一条 CB** ✗ |
| ★ `[ul_dft_wait]` 中位 ✓ | **577.6 µs** ✗ | no samples | ⇒ **前后端抢同一条车道** ✗ |
| UL 每跳 TB 中位 / 总量 ✓ | **217 B / 18.2 MB** ✗ | 3329 B / 333.5 MB | **塌 15× / 18×** ✗ |

★ **因果链** ✓：符号级 ⇒ 成批与 D1 交棒**同时关掉** ✓ ⇒ 每符号一条 CB ✓ ⇒ 前端与后端抢车道 ✓
⇒ 跳变慢 ✓ ⇒ **调度器给的传输块变小** ✓（CRC-OK **比率**几乎不变 ✓ ⇒ 不是解码质量 ✓）。
⇒ ★ **A 项不是交付路径** ✓（"必须等整槽"是**策略**不是物理 ✓），**但它的收益是真的** ✓
（`ul_rx_wait` 473 → 0 ✓）—— **代价（前端 14 CB/符号）远大于收益** ✗。
★ **而 `C1 + 半槽收包` 的组合仍待一条腿验收** ✓（`p43` ✓，§6.70⑤ ✓）—— **这是唯一还没结清的 A 项变体** ✓。

#### (5) 那么"整合 CE"这个目标本身该怎么看 ✓

★ **分两类目标，结论相反** ✓：

| 目标 | 判断 | 依据 |
|---|---|---|
| ★ **结构收敛** ✓（kernel 数、dispatch 数、代码形态 ✓）| **成立且值得做** ✓ | 与 M1 融合 EQ+DEMOD 同一性质 ✓：把 5.91 个派发收敛到更少 ✓、把零散的数据流收进一处 ✓ |
| ✗ **性能（那几百 µs）** | **不成立** ✗ | CE 全部算力 38 µs ✓；边界 ≤0.3 µs ✓；可赚总量 **≤ 45 µs** ✓（见 §2.20 的账 ✓）|

⇒ ★ **诚实的期望值** ✓：整合 CE 能兑现的是**结构** ✓（+ 最多几十 µs 的性能 ✓），
**不能**兑现"解开那几百 µs 之谜" ✗ —— 那个谜的答案已经在上面 (3) 里 ✓，而它**不在 CE 的账上** ✓。

### 2.20 CE 融合的规划（**2026-10-07 起草 ✓**，判据驱动 ✓、每步可停 ✓）

#### (0) ★ 先重新审视那条授权 ✓ —— 它建立在已被证否的前提上 ✗

用户的授权是"**可以改算法、甚至容忍一定的性能下降**" ✓，而**理由**是"CE 零碎 ⇒ 那几百 µs" ✗。
★ **该前提已被 §2.19(2)(3) 证否** ✓（CE 全部算力 **38 µs** ✓、那 ~464 µs 是**等样点** ✓）
⇒ ★ **授权的代价基准变了** ✓，必须重新陈述 ✓：

| 问题 | 答案 |
|---|---|
| 融合 CE 最多能赚多少性能？ ✓ | ★ **≤ 45 µs/跳** ✓（见 (1) 的账 ✓）|
| 若算法改写让 CE 变慢 X µs，值得吗？ ✓ | ★ **只有 X < 45 µs 才可能值** ✗ —— 否则**净亏** ✓ |
| 那"容忍性能下降"还成立吗？ ✓ | ★ **要看为了什么** ✓：为了**结构收敛**（交付物更简单 ✓）⇒ **小幅度下降可以谈** ✓；为了**性能**⇒ **没有空间** ✗ |

⇒ ★ **本节把目标显式分成两类** ✓，**建议只对"结构"下注** ✗（性能那一栏是空的 ✓）：
**P1 = 结构收敛**（低风险 ✓、可停 ✓、**建议做** ✓）；
**P2 = 算法改写**（中高风险 ✓、**性能上界 45 µs** ✗、**建议先不做** ✗）；
**P3 = 撬动"等样点"** ✓（**量级最大** ✓ 但**不是 CE 的事** ✗，且 `RX_SYMBOLS=1` 已证否 ✓，只剩 `C1+半槽` 未结清 ✓）。

#### (1) 账：融合 CE 的性能上界 ✓（把每一项都算出来 ✓）

| 机制 | 上界 | 依据 |
|---|---|---|
| 派发地板 ✓（5.91 → 1 个派发 ✓）| **~6.6 µs** ✓ | 地板 **1.26–1.38 µs** × 4.91 ✓ |
| 边界 ✓（barrier / pipeline 切换 / encoder ✓）| ★ **≈0–1.8 µs** ✓ | 四者各 **≤0.3 µs** ✓ × ~6 ✓ |
| host encode ✓（5.91 → 1 ✓）| **~0.9 µs** ✓ | **0.18 µs** × 4.91 ✓ |
| **小计（纯边界账 ✓）** | ★ **≈8–9 µs** ✓ | 三者相加 ✓ |
| ★ **算力本身** ✓（唯一的大头 ✓）| **≤38 µs** ✓ | 把 CE 算力**全部抹掉**的极端值 ✓ |
| ⇒ **绝对上界（= 把 CE 变成零 ✓）** | ★ **≤45 µs/跳** ✓ | = 38 + 9 ✓ |

★ **参照系** ✓：M1 融合 EQ+DEMOD 实测 **−7.4 / −9.3 µs** ✓ ——
**那个融合消掉了一个 dispatch + 20 B/RE 访存 ✓**；CE 融合面对的是**同样的边界量级** ✓
⇒ ★ **预期落在 7–20 µs 是合理的** ✓、**45 µs 是天花板** ✗。

#### (2) P1：结构收敛（**建议做** ✓，低风险 ✓，不改数学 ✓）

**目标** ✓：把**每跳 5.91 个派发**收敛 ✓，且**逐位一致** ✓（与 M1 同一纪律 ✓）。

**候选切点** ✓（按"融合障碍从小到大"排 ✓）：

| # | 融合什么 | 障碍 | 预期 |
|---|---|---|---|
| ~~**P1-a**~~ ✗ | ~~`mmse_reformat` + `mmse_pilots_lse` + `mmse_pilots_apply_cfo`~~ ✗ —— ★★ **这一条写错了** ✗✗（**读码阶段抓到 ✓**）：`mmse_reformat` 读的是 **`gpu_h`** ✓，而 `gpu_h` 由 **`mmse_apply_lse`** 产出 ✓ ⇒ ★ **reformat 在 apply 的下游、属于"权重"阶段** ✓，与 `pilots_*`（**抽取**阶段 ✓）之间隔着 `pilots_apply_cfo → fd_smooth → sigma2 → power → corr_a → inv → weights → apply_lse` **一整条链** ✗ ⇒ **根本不能融合** ✗。★ **教训** ✓：**"在同一个 CB 里" ≠ "相邻" ≠ "无依赖"** ✓ —— 我把三条**读取不同上游**的 kernel 当成了可合并 ✗ | — |

★ **⇒ P1-a 的更正版本** ✓（**只合并"同阶段 + 真相邻 + 无中间依赖"的** ✓，待依赖分析定稿 ✓）：
候选 = **`pilots_lse` + `pilots_cfo`** ✓（同在抽取阶段的头部 ✓）、**`apply_lse` + `reformat`** ✓（同在权重阶段尾部 ✓）、
**`reformat` + `rsrp`** ✓（**同读 `gpu_h`、互不读对方输出** ✓ —— 引擎注释自己写着"K3 与 K4 写读不相交的缓冲，所以次序自由" ✓）。
★ **而凡是"读上游输出"的相邻对都不能合并** ✗ —— 这正是 P1-a 初版犯的错 ✓。
| **P1-b** ✓ | `mmse_corr_a` + `mmse_corr_r_hp` ✓（同位置、同几何 ✓；★ **`OCUDU_CE_CORR_MERGED` 已经做过** ✓ ⇒ **直接复用** ✓）| 无 ✓（已有实现 ✓）| **零新代码** ✓（`mkf023` 已实测 −6.5 µs ✓）|
| **P1-c** ✓ | `corr` + `inv` + `weights` + `apply` 折进**一个** kernel ✓ | ★ **矩阵求逆需要 threadgroup 协作** ✗（每 system 一个 threadgroup ✓、矩阵在 **threadgroup memory** ✓、受 `MAX_N` / 32 KB 限制 ✓）| **需新 kernel** ✓：`mmse_inv` → `mmse_weights` 可直接续跑 ✓（**同一 threadgroup、同一矩阵 ✓**）；`apply` 的网格不同 ✗（`nout` vs 矩阵组 ✓）⇒ 要分叉 ✓ |
| **P1-d** ✓ | CE 折进 **lane burst**（CB 2.00 → 1.00 ✓）| 网格/编码次序 ✓；**且 M2 的前提已被 `mkf018/019` 证否** ✗（那道 fence 不花钱 ✓）⇒ ★ **收益只剩"少一条 CB"** ✓（≈边界账 ✓）| **优先级最低** ✗ |

★ **建议的施工顺序** ✓：**P1-b（零代码 ✓，先兑现 6.5 µs ✓）→ P1-a（稳 ✓）→ P1-c（真正的融合 ✓）**，
**P1-d 不做** ✗（前提已证否 ✓、收益最小 ✓）。
每步判据沿用本工作流的一套 ✓：**逐位一致 ✓** + `busy` / `ul_pipeline` ✓ + **`ce_sites` / `burst dispatches` 计数自证生效 ✓**。

#### (3) P2：算法改写（**建议先不做** ✗，除非 P1 之后仍有明确目标 ✓）

★ **可改的地方与它的真实收益** ✓（按微基准的"块尺寸扫描" ✓）：

| 算法项 | 现状 | 可改 | 收益 |
|---|---|---|---|
| `mmse_weights` ✓ | **13.2 µs** ✓（最大头 ✓）| R_hp·A⁻¹ 的代数重排 ✓、或用 **LDLᵀ / Cholesky** 代替显式求逆 ✓（A 是对称正定 ✓，kernel 注释自己写了 ✓）| **省掉 inv 内核** ✓ —— 但它不在已知 6 核里 ✓，量待测 ✓ |
| `mmse_corr_r_hp` ✓ | **6.07 µs** ✓（随块线性 ✓）| 利用 R_pp 的 **Toeplitz/共轭对称**结构 ✓（现在按稠密算 ✓）| 可能**大幅**省 ✓ —— ★ **这正是"改算法"真正值钱的地方** ✓ |
| `mmse_apply_lse` ✓ | **7.42 µs** ✓（随输出行数线性 ✓）| 无（就是把权重乘上去 ✓）| 小 ✓ |

★ **重要** ✓：`corr_r_hp` 的**结构化**（Toeplitz ✓）是**唯一**可能带来"量级"收益的算法改动 ✓，
因为它是**纯算力**且**随块线性** ✓ ⇒ ★ **若要做 P2，从这里开始** ✓，**不要**从"把 kernel 合并"开始 ✓
（后者只值 9 µs ✓）。

#### (3bis) ★★★ 但 P2 的**真正障碍不是工程量，是"精度墙"** ✗✗（读码发现 ✓，**这是本节最重要的一条** ✓）

★ **树里已经有一个实验，做的就是"融合 CE 最需要的那一步"** ✓ ——
`OCUDU_CE_DEV_INVERT=1` ✓（`ocudu_metal_mmse_engine.mm:4587` ✓）：★ **在同一条命令缓冲里把刚建好的 A 就地求逆** ✓，
注释原文的目的就是"**让整个矩阵路径留在设备上——没有宿主求逆、没有单独的一次往返**" ✓
—— ★ **那正是"把 corr+inv+weights 折进一个 kernel"要做的事** ✓。

★ **而它默认是关的 ✗，原因不是慢，是不准** ✗——★ **但"不准"这三个字必须写全 ✓**
（2026-10-07 更正 ✓：本节初稿只引了误差数字 ✓、**漏了系统级证据** ✗，是**用户的质疑**逼出来的 ✓）：

**① 根因是条件数 ✓，而且它有一个被点名的来源 ✓**（`port_channel_estimator_mmse_impl.cpp:1340-1347` ✓ 原文 ✓）：

> "Diagonal ridge: a singularity guard only. It is NOT what sets A's conditioning - **sigma2** (the
> noise-to-pilot-power ratio, **~1e-3**) dominates it … That is why ★ **cond₂(A) reaches ~2e4 on a real
> capture** ✓, and why **float32 inversion of A is the accuracy limit of this estimator**"

| 实现 | 逐元素相对误差 ✓ | 与 cond₂ 的关系 |
|---|---|---|
| **float64 分解** ✓ | **4.6e-10** ✓ | ★ **这是 cond₂ ≈ 2e4 下的 float64 极限** ✓（`ε₆₄·cond ≈ 2.2e-16 × 2e4` ✓）|
| **宿主 Gauss-Jordan（float32 ✓）** | **1.7e-1** ✓ | 与 float32 极限同量级 ✓ |
| **设备 kernel（float32 ✓）** | **9.7e-1** ✗ | ★ **比宿主差 5.7×** ✓ —— **算法实现（分块 Gauss-Jordan）确实又丢了一截** ✓ |

★ **⇒ 用户的两点质疑：一点成立、一点不成立** ✓✓（都要写下来 ✓）：

| 质疑 ✓ | 判断 |
|---|---|
| "**没选主元**" ✓ | ★ **是设计选择，不是缺陷** ✓ —— kernel 头注释写明 A **对称正定** ⇒ "Gaussian elimination therefore needs **NO pivoting**" ✓（SPD 无主元增长 ✓，数学上正确 ✓）。★ **但它确实丢精度** ✓（9.7e-1 vs 宿主 1.7e-1 ✓）⇒ **换成分解法/带主元是真正的算法改进空间** ✓ |
| "**应该换矩阵分解法**" ✓ | ★ **成立** ✓ —— 但在 float32 下**买不回设备端** ✗（见 ② ✓）|
| "**8 位都能跑，不该有精度问题**" ✓ | ★ **这里有个关键区别** ✓：8 位是**链路数据/LLR 量化** ✓（LDPC 擅长吸收前端误差 ✓）；**矩阵求逆是中间计算** ✓，它面对的是 **cond₂ ≈ 2e4** 的病态问题 ✓ ⇒ **两者不是一回事** ✗ |

**② ★★ 但这不是纸上数字：系统级实测已经做过了** ✓（**这是本节初稿漏引的那条** ✗，`impl.cpp:4633-4635` ✓ 原文 ✓）：

> "a device inversion puts the **256QAM capture at −18.7 dB of SINR instead of 24 dB** (measured on
> **air data**)"

⇒ ★ **设备端求逆在真实数据上把 SINR 打到 −18.7 dB** ✗（可用需要 ~24 dB ✓）⇒ **差 40 dB** ✗
—— ★ **这不是精度表上的难看数字 ✓，是一个判决级的失败** ✓。

**③ 而"提高正则把精度买回来"这条路，已经有代价读数了** ✓（**也不是猜测** ✓，同一条注释 ✓）：

| 手段 ✓ | 效果 ✓ | 代价 ✓ |
|---|---|---|
| ridge **1e-6 → 1e-2** ✓ | ★ 设备误差 **9.7e-1 → 1.8e-2** ✓（**有效** ✓）| ★ **240 个 capture 里 221 个的 LLR 变了** ✗（SINR 只动 0.01–0.31 dB ⇒ **当初看着无害** ✗）⇒ **它改变了接收机的判决** ✗ |

**⇒ 结论（比初稿更准 ✓、也更站得住 ✓）** ✓：
★ **"融合 corr+inv+weights" 的障碍确实是数值的 ✓，而且它是精度墙不是性能墙** ✓ ——
**但要说清三件事** ✓：**根因是 cond₂(A)≈2e4（sigma2 主导 ✓）** ✓；
**算法实现确实还有空间** ✓（设备比宿主差 5.7× ✓ ⇒ 分解法/主元）✓；
★ **而 lift 不到"能用"** ✗（float32 极限 + 系统级 −18.7 dB ✓）。
⇒ ★ **正确的算法工作顺序不变、但理由更硬** ✓：**先降条件数（Toeplitz/结构）或换分解法** ✓，
**再看设备端能不能用** ✓；★ **不要先写融合 kernel** ✗ —— 那会得到一个**快而 SINR −18.7 dB** 的路径 ✗。

★ **诚实标注** ✓：★ **本节初稿把这个数字当成"墙"直接引用了 ✓，但没引它的系统级证据与算法空间** ✗ ——
**是用户要求核实"为什么差别这么大"才补全的** ✓。★ **"历史结论要复核"这条纪律生效了一次** ✓

#### (3ter) ★★★ 更正：**"精度墙"这个结论本身是错的** ✗✗ —— 它引的正是那条**已被驳倒的论据** ✓（2026-10-07，用户第二次追问 ✓）

★ **用户的追问** ✓："历史上因为求逆精度问题把求逆放到 CPU，这隐含了 GPU↔CPU 的切换，是这样吗？" ✓
—— ★ **这个追问把真相挖出来了** ✓：**既不是"在 CPU"，而我引的论据是过期且已被明确驳倒的** ✗✗。

**① 现状 ✓：求逆在 GPU 上 ✓，而且是默认 ✓**（`port_channel_estimator_metal_mmse_impl.h:495-503` ✓）：

| 判定 | 值 |
|---|---|
| `OCUDU_CE_GPU_INVERT` **未设（默认 ✓）** | ★ **enabled = true** ✓ |
| `OCUDU_CE_CPU_INVERT` 未设 ✓ | ✓ |
| `L = 54` ✓（空口实测：`y_direct … L=54` ✓）| ★ **54 ≤ 54 ⇒ 在设备上求逆** ✓ |
| ⇒ **一跳的 GPU↔CPU 往返** | ★ **不存在** ✗（**求逆、相关矩阵、权重全在设备 ✓**）|

**② ★ 那个"精度理由"是记录在案的、已被驳倒的论据** ✗（`f2376a1f42` ✓，2026-09-15 ✓ 原文 ✓）：

> "K1 was moved to the host on a **PERFORMANCE** argument (**555 µs at order 54** against ~10 µs of host
> Gauss-Jordan) and then KEPT there by a precision one **that does not hold**: the device inverse's
> **W error is 7.67e-4** against the host's **1.40e-5**, **worth about 0.003 dB of SINR**, and the
> ★ **−18.7 dB that looked like an accuracy failure was two staging defects fixed in S-7f-3k** ✗.
> ★ **Using a refuted argument to defend an earlier decision is recorded as its own failure mode** ✓"

⇒ ★★ **我上一轮引用的 `9.7e-1` / `1.7e-1` / `−18.7 dB` 全部来自注释 `97ba98b006`（2026-09-14 ✓）** ✗，
**而它比 `f2376a1f42`（09-15 ✓）早一天 ✓** ⇒ ★ **我引的是"注释在前、驳倒在后"里那条被驳倒的** ✗✗。

**③ 两个数字为什么不矛盾 ✓（这才是关键读数 ✓）**：

| 量 | 值 | 含义 |
|---|---|---|
| **A⁻¹ 的逐元素相对误差** ✓ | 设备 **9.7e-1** / 宿主 **1.7e-1** ✓ | ★ **病态矩阵元素的相对误差 —— 看着吓人** ✗ |
| ★ **最终 W 的误差** ✓ | 设备 **7.67e-4** / 宿主 **1.40e-5** ✓ | ★ **真正进入数据路径的量** ✓ |
| ★ **换算成 SINR** ✓ | ★ **≈ 0.003 dB** ✓ | ★ **完全可忽略** ✓ |

⇒ ★ **"元素相对误差大" ≠ "结果不好"** ✓：逆的元素被 cond₂≈2e4 放大 ✓，但**乘上 R_hp 之后误差回到 7.67e-4** ✓。

**④ 现状的理由 ✓**（`device_inverts()` 自己的注释 ✓ 原文 ✓）：设备路径"**measures the same as the host's on
every capture, so there is no functional reason left to keep it off**" ✓；剩下的代价被明确记为
★ "**a PERFORMANCE debt, tracked separately, NOT a blocking defect**" ✓
（且它**不是 barrier**：S-5a 实测 36 个 barrier 只值 2.5 µs ✓，而**线程组几何让同一 kernel 差 3.7×** ✓
—— **换几何把整引擎等待从 357/212/284 µs 降到 121/107/195 µs** ✓）。
★ **门槛 54 的含义也不是精度 ✓**：它是"**the kernel's own `MAX_N` (mmse_inv.metal: 54) — above it the
kernel **cannot be dispatched at all**" ✓ ⇒ ★ **是内核容量，不是精度阈值** ✓。

**⑤ ⇒ 对 §2.20 的连带更正** ✓：

| 原判断 ✗ | 更正 ✓ |
|---|---|
| "融合 corr+inv+weights 撞**精度墙**" ✗ | ★ **不撞** ✓ —— **设备求逆已经在默认路径上跑 ✓、误差值 0.003 dB ✓** ⇒ ★ **这一步本来就已经在设备上 ✓、已经在同一条 CB 里（K1）✓** |
| "先降条件数/换分解法，再谈设备端求逆" ✗ | ★ **顺序反了** ✓ —— **设备求逆已在用** ✓ ⇒ 真正缺的是**把 corr 那条独立 CB 并进来** ✓（= §2.20 的 seam 7 ✓）、以及 **K1 的性能债**（几何 ✓）|
| "不要先写融合 kernel" ✓ | ★ **仍然成立 ✓，但理由变了** ✓：不是精度 ✗，而是 **corr→K1 的边界有实测的 28% 失败率** ✓（`E:4004-4013` ✓）|

★ **诚实标注** ✓：★ **同一个问题用户问了两次 ✓，第一次我补了"系统级证据"却仍是被驳倒的那条** ✗，
**第二次才挖到 `f2376a1f42`** ✓ ⇒ ★ **教训升级** ✓：
**不只"复核历史结论" ✗，还要"查结论有没有被后来的提交推翻"** ✓
—— ★ **引一条注释前，先 `git log -S` 它，看它后面有没有被驳倒** ✓。

#### (4) P3：撬动"等样点"（**量级最大 ✓，但不是 CE 的事** ✗）

★ 唯一能动 **~464 µs** 的东西 ✓（§2.19(3) ✓）。已测：
`RX_SYMBOLS=1` **证否** ✗（前端 14 CB/符号 ⇒ +676 µs ✓）；
★ **`C1 + 半槽收包`（`p43` 那条腿）仍待验收** ✓ —— **这是整个"时间之谜"里唯一还没结清的变体** ✓，
且**不需要动 CE** ✓ ⇒ ★ **它的优先级高于 P1/P2** ✓（因为量级差一个数量级 ✓）。

#### (5) ⇒ 规划结论（一句话 ✓）

★ **把 CE 融合进来是值得做的结构工作 ✓，但它的性能上界是 ~45 µs（现实预期 7–20 µs）** ✓，
**而不是那几百 µs** ✗ —— 那几百 µs 的答案已经在 §2.19(3) 里 ✓：**等整槽样点** ✓。
⇒ ★ **建议顺序** ✓：**先结清 `C1+半槽` 那条腿** ✓（量级最大 ✓、不动 CE ✓）⇒
**再做 P1-b / P1-a / P1-c** ✓（结构收敛 ✓、逐位一致 ✓）⇒
**P2 只在"Toeplitz 结构化"上开一枪** ✓（唯一可能值钱的算法改动 ✓）。

#### (6) ★★★ `p44-n78-q24` 判读（2026-10-07 ✓，用户飞腿 ✓）：**fence 被证否 ✗、一跳账闭合 85.7 % ✓、且 ★ 这个工作流的性能目标到此已无意义** ✗✗

> ★ **修正在先** ✓（本工作流第三次"引错腿/引错状态" ✗）：★ **`C1+半槽收包`（`p43-n78-halfslot`）早已飞过并验收通过** ✓（09-26 ✓、
> 在 `doc_chinese/phy_pipeline_gpu/wip/logs/` ✓ —— **不在本工作流目录** ✗，我因此漏了它 ✓）；
> ★ **P1-b（corr 合并）也早已飞过** ✓ —— **`mkf023` 就是它** ✓ ⇒ §2.20 把它列为"待做"是错的 ✗。

**① 为什么飞这条** ✓：`Q24`（§6.73 ✓）量的是 **`signaller 的 GPUEndTime − waiter 的 GPUStartTime`** ✓，
正是为回答"**cb 驻留 575 µs 而执行只有 ~67 µs**"那个形状造的 ✓。★ **它已实现但从未飞过** ✓
—— `p43` 日志里 `Q24` 出现 **0 次** ✗（Q24 在 `p43` 之后才落地 ✓）。

★ **而 09-26 已有一条 `p44-n78-fencewait` 飞过** ✓ —— ★ **它的读数是坏的** ✗：

| | 09-26 `p44-n78-fencewait` ✗ | **今天 `p44-n78-q24`** ✓ |
|---|---|---|
| Q24 mean / median | **3 499 661 / 771 492 µs**（3.5 **秒** / 771 **毫秒**）✗ | **31.7 / 36.0 µs** ✓ |
| Q24 max | **31 135 186 µs**（31 秒）✗ | 742.2 µs ✓ |
| 交叉校验 | **无 Q24b** ✗ | ★ **Q24b 与 Q24 完全一致** ✓（`39545 (36.4%) mean=31.7us`）|

⇒ ★ **今天这条腿把一个坏读数补成了好读数** ✓（一跳窗口才 575 µs，3.5 秒的"等待"不可能是真的 ✗）。

**② 预登记判据逐条** ✓（全部 PASS ✓）：

| 判据 | 实测 |
|---|---|
| Q24 必须出现 ✓ | `waits=108502 resolved=108502 no-signal=0 unresolved=0 inconsistent=0 dropped=0 lost=0` ✓ |
| Q24b 一致 ✓ | ★ **两侧同值** ✓ |
| 红线 ✓ | `stale=0` ✓、**`gaps=0`** ✓、`rx_overflows=0` ✓ |

**③ ⇒ fence 被证否** ✗：**一次等待 = 一跳**（`waits=108502` = 跳数 ✓）⇒
一跳的 fence 总账 = **11.6 µs** ✓（`sum=1 254 190 µs / 108 502` ✓）⇒ ★ **只占 575 µs 的 2.0 %** ✗。
★ **与 `mkf018/019` 的空口 fence 消融互相印证** ✓（拿掉它什么都不动 ✓）——**两条独立的证据、同一个结论** ✓。

**④ ★ 一跳的账闭合到 85.7 %** ✓（`Q9-F3` 逐标签，**全部 GPU 时间戳** ✓）：

| 项 | µs/跳 | 占比 | 出处 |
|---|---|---|---|
| **fence 等待** ✓ | **11.6** | 1.8 % | Q24 ✓ |
| **队列等待**（`commit→start` ✓）| **69.0**（`ce_wt` 25.5 + `merged` 43.5 ✓）| 10.9 % | Q9-F3 逐标签 ✓ |
| **设备执行**（`start→end` ✓）| **463.0**（`ce_wt` 32.8 + `merged` 430.2 ✓）| **73.0 %** | 同上 ✓ |
| **小计** ✓ | **543.6** | ★ **85.7 %** | |
| 未解释 ✗ | 90.4 | 14.3 % | |

★ **对照离线算力** ✓：热跳纯算力 **~67 µs**（44.6+22.2 ✓）⇒ ★ **463 µs 的"执行"里有 ~400 µs 是设备驻留而非算力** ✗
—— ★ **与 `p43` 的"575 µs 窗口只有 ~67 µs 执行"同源** ✓，但**现在它是逐标签、带队列/执行分解的** ✓。

**⑤ ★★ 而且最后两个候选也被分开了** ✓（这一步是本条腿最值钱的产出 ✓）：

| 读数 | 值 | 排除 |
|---|---|---|
| ★ **最大空洞** ✓ | **24 385 ms（24.4 秒）** ✗ | 不是"设备被别的 lane 抢"那种小间隙 ✓ |
| `back_end` 占用率 ✓ | **21.0 %**（busy 53.4 s / window 254.9 s ✓）| ★ **设备 79 % 时间是空的** ✓ |
| 最慢执行的 `idle_before` ✓ | **0.00 ms** ✓ | ★ **工作一到就开工** ✓ ⇒ **GPU 不排队** ✓ |

**⑥ ★★★ 于是设备的时间账是** ✓：**执行合计 53.4 s，其中 `463 µs × 108 502 跳 = 50.2 s` = 94 %** ✓
⇒ ★ **设备的时间几乎全花在这些跳上** ✓。而**提交率 = 426 跳/s ⇒ 一跳周期 2350 µs** ✓
（**与业务一致** ✓：`--smoke=40` + 一跳一 PUSCH ⇒ 每 ~2.3 ms 一次 ✓）。

⇒ ★★★ **结论（本工作流性能目标的终点 ✓）**：
一跳 **执行 463 µs + 空闲 1887 µs** ✓ ⇒ ★ **单跳执行时间即使砍半，设备占用率也只从 21 % 升到 30 %** ✗
⇒ ★ **一跳延迟的性能优化已经没有意义** ✗✓ —— **设备有 4.7× 余量** ✓、且**那 79 % 空闲是业务节奏** ✓（不是缺陷 ✓）。
★ **这正是"信封是平台属性"的最终、带证据的版本** ✓（比 M0 时的推测硬得多 ✓）：
**不是"400 µs 无归属" ✗，而是"一跳 634 µs，其中 463 µs 设备执行、69 µs 队列、12 µs fence、90 µs 未解释，
而设备整体只用掉 21 % —— 它远低于业务到达率"** ✓。

⇒ ★★ **对 §2.20 规划的意义** ✓：**CE 融合的目标现在纯粹是"结构收敛"** ✓（性能动机**已归零** ✗）；
★ **`C1+半槽` 已结清 ✓、P1-b 已完成 ✓** ⇒ **下一个真正可做的是 P1-a** ✓
（`reformat + pilots_lse + pilots_cfo` 合一 ✓，局部算子 ✓、可保逐位一致 ✓）。
★ **而"那 ~550 µs"这个问题本身可以关掉了** ✓ —— 它不是缺陷 ✓，是**一跳的固定成本** ✓（其中 73 % 是设备驻留 ✓）。

### 2.21 ★★ CE 融合的依赖分析结果与施工顺序（2026-10-07 ✓，**静态分析，未改 kernel** ✓）

用户裁决继续推进 CE 整合后 ✓，先做了**一次严格的依赖分析** ✓（逐 kernel 的读写缓冲/局部性/网格/归约锁 ✓），
**没有凭猜测定接缝** ✗ —— ★ **这个决定当场救了一次** ✓：本工作流原写的 **P1-a（`reformat`+`pilots_lse`+`pilots_cfo` 合一）是错的** ✗
（§2.20 已标 ✗），因为 `reformat` 读 `gpu_h`、而 `gpu_h` 由 `apply_lse` 产出 ⇒ **它在 apply 的下游、属"权重"阶段** ✓。

#### (1) ★ 真正的锁是"**每文件一个数学模式**"，不是"每文件一个 kernel" ✓✓

| 文件 | 在 `IEEE_MATH_SOURCES`（`-fno-fast-math`）| 含义 |
|---|---|---|
| `ocudu_mmse_corr.metal` ✓ | **是** ✓（**三个** kernel ✓）| 相关 kernel **彼此可融** ✓（O1 已这么做 ✓）|
| `ocudu_mmse_pilots_power.metal` ✓ | **是** ✓（**一个** kernel ✓）| ★ **双向锁死** ✗（`:23-27` 原文"flag 是按文件的，所以该 kernel 必须单独在这里" ✓）|
| `ocudu_mmse_apply_lse.metal` ✓ | **是** ✓（**一个** kernel ✓）| ★ **双向锁死** ✗（`:20-42` ✓）|
| `ocudu_mmse_reformat.metal` ✓ | 否 ✗（**两个** kernel ✓ `reformat`+`noise` ✓）| ★ **"每文件一 kernel"不是规则 ✓，"每文件一数学模式"才是** ✓ |

⇒ ★ **硬墙** ✓：`pilots_power` 与 `apply_lse` **与任何 kernel 都不能融** ✗（跨过严格边界会改浮点语义 ✓，
且两处都有实测：`pilots_power` 的记录是 **298151/2²⁰ 个商的舍入不同、27/27 capture 的 LLR 翻转** ✓；
`apply_lse` 是 **13/27 capture 的 `_ce.txt` 动了 40 字节** ✓）。

#### (2) ★★ 而分析顺手抓出一个**必须先修的真缺陷** ✓✗（F1 ✓）

★ `ocudu_mmse_weights.metal:126-136` 的注释写着 ✓：
> "`acc += rp[k] * a_smem[...]`, **NOT fma()** … the metallib **is compiled with -fno-fast-math** exactly so
> the contraction does not happen behind our back … Writing fma() here would be a deliberate difference in
> **the last bit of every element**" ✓

★ **而这句话在构建上是假的** ✗：`IEEE_MATH_SOURCES` **只列了三个文件 ✓、没有 weights** ✗
⇒ `mmse_weights` / `mmse_weights_tile` 一直在 **Metal 默认快数学**下编译 ✓
⇒ ★ **那句"与 CPU 参考逐位一致"的合同，靠的是"编译器恰好没做 fma 收缩"** ✗。

★ **已修** ✓（`lib/phy/metal/CMakeLists.txt` ✓：把 `ocudu_mmse_weights.metal` 加进严格清单 ✓，注释写明原因 ✓）：

| 验证 | 结果 |
|---|---|
| metallib 重建 ✓ | **201 148 → 200 988 B** ✓（**标志确实影响了产物** ✓）|
| ★ **数值对拍** ✓ | ★ **6 个 capture × 5 个 dump = 30 个 dump，0 差异** ✓（`OCUDU_LANE_METALLIB_PATH` 切换两版库 ✓）|

⇒ ★ **结论** ✓：**当前编译器恰好没收缩** ✓ ⇒ 加标志**不改变任何输出** ✓，
但把"恰好"变成"**由构建保证**" ✓ —— ★ 而这**对融合是前提** ✓：
**融合会把 kernel 放进新的编译上下文 ✓，靠现状产物"恰好一致"的位不能假设能活下来** ✗。

#### (3) ★ 可融合清单（按"能保逐位一致"的置信度 ✓，**全部已核对依赖** ✓）

| # | 合并什么 | 省派发 | 几何 | 归约顺序 | 置信度 |
|---|---|---|---|---|---|
| **1** ✓ | `corr_a` + `corr_r_hp`（**每组** ✓）| **2** | `tgs_wide*2*nof_systems` × 256 ✓ | ★ **不碰任何归约** ✓ | ★ **最高** ✓ —— ★ **代码已存在** ✓（`mmse_corr_a_rhp` ✓，`OCUDU_CE_CORR_MERGED` 背后 ✓）⇒ ★ **把它转默认** ✓、**先修 F2** ✗ |
| **2** ✓ | `cfo` + `apply_cfo` ✓ | **1** | 1 tg × 256 ✓ | ★ **不变** ✓（cfo 保 256 线程/`i+=256`/同一 8 级树 ✓）| **高** ✓（同文件同快数学 ✓）|
| **3** ✓ | ★ **四个相关派发 → 1** ✓ | **3** | 一个 1-D 网格 + (group,matrix,system) 解码 ✓ | 不碰归约 ✓ | **高**（算术）/ **中高**（工程）✓ |
| 4–6 | cfo+apply_cfo+(fd_smooth)+(sigma2)+(epre) ✓ | 2–5 | 1 tg × 256 ✓ | ★ **只有恰好 256 线程时才不变** ✗（改线程数会重划 SIMD 组 ✓）| 中 ✓；★ **第 6 档大概净亏** ✗（LSE 的并行度就在这里 ✓）|
| 7–9 | rsrp/ta_chain/noise/reformat 的各种组合 ✓ | 1–2 | 需重排 ✓ | noise **必须钉在 256** ✗ | 中 ✓ |
| 10 | `inv` + `weights` ✓ | 1 | `nof_systems` × (64,16) ✓ | 不变 ✓ | ★ **被 F1 挡住过** ✓ ⇒ **F1 修好后可重新评估** ✓ |

#### (4) ★ "看着能融其实不能"的对 ✓（**D 组，这是本次分析最有价值的部分** ✓）

| 对 | 为什么不能 |
|---|---|
| **`sigma2` + `power`** ✗ | 最诱人（相邻 ✓ 同 1×256 ✓ 同参数块 ✓）但：① **真 RAW 依赖** ✓；② ★ **跨严格边界** ✗ —— 合进 `pilots.metal` ⇒ 除法走快数学 ✓（27/27 LLR 翻转 ✓）；合进严格文件 ⇒ **数据路径的归约被重舍入** ✓（27/27 capture 动过 ✓）|
| **`pilots_power` + `epre`** ✗ | 无依赖但同样撞严格边界 ✓ |
| ★ **`apply_lse` + `reformat`** ✗ | 最"显然"的一个（"直接从 K2 写 bf16" ✓）但：`round(round(lse*inv_beta)*w)` 的两次舍入**必须在原处** ✓ ⇒ 换文件会重结合 ✓（实测 **13/27 capture 动 40 字节** ✓）|
| **`apply_lse` + `weights`** ✗ | 同严格边界 + `weights` 的 no-fma 合同（F1 ✓）|
| **`corr_r_hp`(edge) + `pilots_lse`** ✗ | 无依赖 ✓ 但**不同命令缓冲** ✗ **且不同数学模式** ✗（重舍入相关矩阵会被 cond₂≈2e4 放大成 W/h 的 ~1% ✓）|
| ★ **`corr_a` + `inv`** ✗ | ★ **结构上最有趣的一对** ✓（能把那个 **27% 失败率的"地雷"边界** ✓ 变成**一个 threadgroup barrier** ✓）但需要把 K1 的消元放在 `corr.metal` 的 `-fno-fast-math` 下编译 ✓ ⇒ **未测** ✗ |

#### (5) ★ 三条附带发现 ✓（都会影响后续施工 ✓）

* **F2** ✗：`encode_corr` 的 merged 分支**只在 `sigma2_dev != nullptr` 时绑 buffer(3)** ✓，
  而**注释与 kernel 都要求"即使不用也必须绑"** ✓（"an unbound device pointer is undefined in MSL" ✓）
  ⇒ ★ **今天潜伏** ✓（只在 `sigma2_from_device != 0` 时解引用 ✓）⇒ ★ **把 O1 转默认前必须修** ✗。
* **F3** ✓：★ **cb#2 里有几对"生产者→消费者"之间没有 barrier** ✗（`lse→cfo` ✓、非 burst 模式下
  `weights→apply_lse` 与 `apply_lse→reformat` ✓），而 `fd_smooth→sigma2`、`sigma2→power`、`epre` **都编了** ✓。
  ★ 结合 `impl.cpp:2211-2215` 的记录（"**一个 encoder 内两次派发之间的 barrier 在这个平台上不交付那次写**" ✓）
  ⇒ ★ **这些无 barrier 的对正是"融合能治"的形状** ✓ —— **一个 dispatch 内的 threadgroup barrier
  比跨派发的 `MTLBarrierScopeBuffers` 更强也更便宜** ✓ ⇒ ★ **这是"支持融合"的论据** ✓（未实测 ✓）。
* **O1 的空口收益没有记录在案** ✗（`corr.metal:268-271` 记着它**曾测得比两个派发更慢** ✓，
  在 `my_tgs` guard 加上之前 ✓）⇒ ★ **把它转默认之前要先确认它在空口上真的赢** ✓。

#### (6) ⇒ 施工顺序（据本次分析定稿 ✓）

1. ★ **F1 已修 ✓**（`weights` 进严格清单 ✓、30 dump 对拍 0 差异 ✓）；
2. ★ **修 F2** ✓（merged 分支必须绑 buffer(3) ✓）—— ★ **这是 O1 转默认的前置** ✓；
3. ★ **RANK 1：把 O1 转默认** ✓（省 2 派发 ✓、**零算术暴露** ✓、代码已存在 ✓）——
   ★ 但**先离线对拍 + 一条腿确认它真的赢** ✓（因为历史上曾测得更慢 ✗）；
4. ★ **RANK 2：`cfo` + `apply_cfo` 新 kernel** ✓（省 1 派发 ✓、同文件同快数学 ✓、归约顺序不变 ✓）；
5. 之后才考虑 RANK 3（四个相关派发合一 ✓）与 `corr_a`+`inv`（★ **需要先测严格模式下的 K1** ✓）。

### 2.22 ★★ RANK 3：两个相关组合成**一个派发**（CE 融合第 3 项，2026-10-07 ✓）

> **性质** ✓：本条**不是**性能项 ✓ —— §2.20(6) 已把性能目标关闭 ✓（设备余量 **4.7×** ✓）。
> 它的价值是**结构收敛** ✓：把"一跳两个相关组、两次派发"变成一次 ✓，且**碰不到任何算术** ✓。

#### (1) 要融的是什么 ✓（**读码确认，不是猜** ✓）

一跳最多带**两个相关组** ✓：**标准块** ✓（`nof_layers` 个 system ✓）与**边缘块** ✓
（`rem_prb != 0` 时 ✓，塞进**同一批 slot 的后面那些 system** ✓，`sys_offset = nof_layers` ✓、
`aL_stride = L_std` 而**块阶是 `L_e`** ✓）。两个组今天各走一次 **O1 派发** ✓
（`encode_corr` ✓），★ **而且它们已经被编进同一条命令缓冲、背靠背、中间没有 barrier** ✓
（`flush_correlations_fenced()` ✓ 把队列里的 `queued` 个 stage 依次 `encode_corr` ✓）。

⇒ ★ **隔开两者的东西是"几何"，不是"顺序"** ✓ ⇒ **几何就是一个参数块** ✓ ⇒ 可以合成一个网格 ✓。

#### (2) 实现 ✓

| 侧 | 改动 |
|---|---|
| **kernel** ✓ | `mmse_corr_a_rhp` 的**工作项整体**抽成 `static inline mmse_corr_a_rhp_block()` ✓（`i` / `block` / `lane` 由调用者给 ✓）；单组 kernel 只负责解码 ✓；★ **新增 `mmse_corr_a_rhp_pair`** ✓：网格 = 两个组各自 O1 网格的**拼接** ✓，`tgid` 自己说明它属于哪一组 ✓（`nof_tgs0` 是唯一跨界传递的那个数 ✓，组 1 的宽度**各自从各自参数推** ✓、用**同一个表达式** ✓）|
| **host** ✓ | `encode_corr` 的装填部分抽成 `pack_corr_group()` ✓（两条路线共用一份参数/映射构造 ✓）；★ **F2 的"必须绑"规则收进 `bind_corr_scalars()`** ✓（第三条路线出现时它才有资格叫"规则" ✓）；新增 `encode_corr_pair()` ✓、`corr_pair_pipe` ✓、`OCUDU_CE_CORR_PAIR` ✓ |
| **计数** ✓ | 新增 `ce_site_diag().corr_pair` ✓（**派发**数 ✓）；★ `corr_a`/`corr_rhp`/`corr_merged` **改成计"组构建"** ✓（一对派发记 2 ✓）⇒ 三者**仍然彼此相等** ✓、**跨融合/非融合可比** ✓ |

★ **为什么把 kernel 抽成函数而不是复制一份** ✓：融合的全部主张就是"**算的是同一件事**" ✓，
而**保证这一点的最强形式是"只有一份实现"** ✓，不是"两份长得一样" ✗。

★ **自证不变式** ✓（腿上一眼可验 ✓）：
```
merged == corr_a == corr_rhp        # 组构建数
merged == 2 * corr_pair             # 臂：每个 pair 派发恰好两个组；对照 corr_pair = 0
merged - corr_pair                  # 该跳的相关派发数（两种路线都对）
```

#### (3) ★★ 一个必须先讲的坑 ✓：**第一次 A/B 是空的** ✗

★ 第一次对拍我按常规跑了 `ab_dumps.sh … --metal` ✓（即 `--repeat 1` ✓）⇒ **0 差异** ✓ ——
★ **但那不是证据** ✗：`corr_pair = 0` ✓ ⇒ **融合根本没生效** ✓。
**原因** ✓：边缘组的**融合形态**要求"上一跳的 sigma2 块" ✓（`fused_edge_ordered` ✓），
**进程第一跳没有** ✓ ⇒ 它走**独立命令缓冲**的形态 ✓ ⇒ 只排进**一个** stage ✓ ⇒ 组不成对 ✓。
**改成 `--repeat 8` 后** ✓：`merged=16` ✓、`corr_pair=7` ✓（第 1 跳两个单派发 + 后 7 跳各 1 个 pair 派发 = 16 组 / 9 派发 ✓）。
★ **这正是本工作流第 ① 条纪律的第三次现身** ✓：**"旋钮没生效"与"没效果"在读数上一样** ✓ ——
★ **计数自证必须放进"将要引用的那次运行"里** ✓，不是"另一次运行里看到过" ✗。

#### (4) 离线证据 ✓（**已做** ✓）

| 语料 ✓ | 配置 ✓ | 结果 ✓ |
|---|---|---|
| `/tmp/q64c` ✓ | `--metal --repeat 8` ✓ | **27 capture × 4 dump = 0 差异** ✓（`corr_pair=7` ✓）|
| `/tmp/q16c` ✓ | 同上 ✓ | **0 差异** ✓ |
| **`work_tmp/corpus`（真 QPSK）** ✓ | 同上 ✓ | **0 差异** ✓ |
| 单测 ✓ | —— | **5/5 PASS** ✓ |
| 探针 OFF ✓ | `probes_off_syntax_check.sh build` ✓ | **114 编译 × 3 键全 OK** ✓ |
| 全量构建 ✓ | 交付默认 ✓ | **0 error** ✓ |

★ **对照臂显式写 `=0`** ✓；两臂**都带 `OCUDU_UL_REPLAY_NO_EVM=1`** ✓。

★ **顺带修掉一处潜伏** ✓：`corr_stage::nof_systems` ✓ —— ★ **它的注释整段就是为"合并批的两个几何"写的** ✓，
却**从来没人读** ✗（`flush_correlations_fenced()` 把**一个** `fallback_nof_systems` 发给每个 stage ✓）。
今天**每个调用点两者恰好相等** ✓（标准前缀 = `nof_layers` ✓、边缘组由 `correlation_stage()` 填 0 ✓、
standalone 路线透传 ✓）⇒ **改动是恒等的** ✓（三份语料 A/B 复验 0 差异 ✓），
但**融合让"两组各自的 system 数"变成承重信息** ✓ ⇒ 现在按字段读 ✓（`stage_systems()` ✓），
`build_correlation()` 那条无队列的入口**保持传参**不动 ✓。

#### (5) 预登记判据 ✓（腿 `mkf028-rank3` ✓，`EXTRA_KNOBS="OCUDU_CE_CORR_PAIR=1"` ✓）

| 判据 ✓ | 预登记 ✓ | 判读 ✓ |
|---|---|---|
| ★ **结构自证** ✓ | `corr_pair` ≠ 0 ✓ **且** `merged == 2 * corr_pair + (单组派发数)` ✓ | 与 `corr_pair=0` 的对照臂逐项比 ✓ |
| **红线** ✓ | CRC 同档 ✓ + `gaps=0` ✓ + `stale=0` ✓ | 与**同轮对照腿**比 ✓（不用历史值 ✓）|
| **不劣化** ✓ | `busy` / `ce` 自身 / `ch_wt` ✓ | ★ `ch_wt` **本就该持平** ✓（相关构建在**自己的 CB** ✓、不在 `ch_wt` 那条 ✓）|
| ★ **预期收益** ✓ | **≈ −0.5 派发/跳** ✓ | ★ **不要指望微秒** ✓；空口上只有 `rem_prb != 0` 的跳（离线语料里是**每一跳**）才配对 ✓ |

#### (6) ★ 默认值：**本项先关** ✗（与 RANK 1 ✓/RANK 2 ✓ 不同 ✓）

★ **理由只有一条** ✓：RANK 2 的红线**还没有一条干净的腿** ✓（`mkf026` 那轮被环境污染 ✓）。
若 RANK 3 也默认开 ✗，下一条腿就要**同时**为两件事作答 ✓ —— 而"一条腿两个变量"正是本工作流
反复付出代价的形状 ✗。⇒ ★ **关着实现** ✓、**先飞对照腿**（= 现状 ✓，替 RANK 2 补红线 ✓）、
**再飞 RANK 3 臂** ✓ ⇒ 两件事各有一条腿、互为对照 ✓、`pair_check.sh` 可用 ✓。
★ **验过之后按 RANK 1/2 的同一格式转默认** ✓（`corr_pair_enabled()` 的注释里已写明这一步 ✓）。

★ **回退** ✓：`OCUDU_CE_CORR_PAIR=0` ✓；★ 且**它同时受 `OCUDU_CE_CORR_MERGED` 把关** ✓ ——
pair kernel **本身就是合并构建** ✓，O1 的回退必须能一起退 ✓（离线已验证 `OCUDU_CE_CORR_MERGED=0` ⇒ `merged=0` ✓、`corr_pair=0` ✓）。

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

## 4. M3：metallib 整合 —— **已完成** ✓（2026-10-07）

**结果** ✓：**3 个库 → 1 个** ✓ —— `lib/phy/metal/ocudu_lane.metallib`（**200 892 B** ✓），
由 **15 个 `.metal`** 编成 ✓（CE 12 + EQ 2 + DEMOD 1 ✓），**34 个 kernel 入口点全部在内** ✓
（逐名核对 ✓：`equalize_mxn`/`equalize_mxn_batch`/`gather_ch_re`/`eq_build_gather`/`lane_grid_to_llr`/
`demod_soft`/`mmse_*` ✓）。三个引擎各自加载它 ✓（**加载次数 3 → 1** ✓、**要同步的文件 3 → 1** ✓）。

**落点** ✓（都在 `CMakeLists.txt` + 三个 resolver ✓，数据路径一行没改 ✓）：

| 文件 | 改动 |
|---|---|
| `lib/phy/metal/CMakeLists.txt` ✓ | **新增** `ocudu_add_metallib(TARGET ocudu_metallib_lane OUTPUT …/ocudu_lane.metallib SOURCES <15>)` ✓；`IEEE_MATH_SOURCES` 照抄 CE 那三个 ✓（`mmse_corr` / `mmse_pilots_power` / `mmse_apply_lse` ✓ —— **严格 IEEE 的契约必须跟着文件走** ✓）|
| 三个引擎的 `CMakeLists.txt` ✓ | 各自删掉自己的 `ocudu_add_metallib` ✓、`OCUDU_*_METALLIB_PATH` → **`OCUDU_LANE_METALLIB_PATH`**（由 `lib/phy/metal` 发布 ✓，该目录在 `upper` **之前**进入 ✓ ⇒ 三个引擎都能读到 ✓）、依赖改为 `ocudu_metallib_lane` ✓ |
| 三个引擎的 resolver ✓ | 首选与回退都指向 `ocudu_lane.metallib` ✓；CE 的 `load_library()` 默认路径同样 ✓ |
| ★ `lib/phy/metal/ocudu_metal_burst.mm` ✓ | **ablation 臂自己的 loader 也换了** ✓ —— 见下 ✓ |

**★ 差点漏掉的一处（读代码才发现的 ✓）**：`ocudu_metal_burst.mm` 里 **ablation 臂**（`OCUDU_LANE_ABLATE` ✓）
**自带一个 metallib resolver** ✗，它按名字找 `ocudu_demod.metallib` ✓ —— 库一合并、旧文件一删，
该臂就会**只打印一句 warning 然后静默失效** ✗（**只在该旋钮打开时才走到** ⇒ 默认路线与腿都看不出来 ✗）。
已指向 `ocudu_lane.metallib` ✓，并**离线验证**：`OCUDU_LANE_ABLATE=1` 下打印 `ABLATION ON` ✓、
覆盖表出现 ✓、不再有 `not found` ✓。

**★ 验证时踩到的坑（写下来 ✓）**：改完库路径后，**只重编 `gnb` 不够** ✗ ——
`demodulation_mapper_metal_unit_test` / `port_channel_estimator_metal_mmse_unit_test*` **仍是旧二进制** ✓
（里面烘的是**已被删掉的** `ocudu_demod.metallib` / `ocudu_mmse.metallib` 路径 ✗）
⇒ 三个测试**假失败**（SEGFAULT / Failed ✓），**重编测试目标后 5/5 PASS** ✓。
**教训** ✓：换库路径 = 换**所有链接引擎的二进制**里的字符串 ⇒ 用**全量 `cmake --build`** 验证 ✓，
不要只编你想到的那个目标 ✓（本工作流的"每个数字都要能说出它的口径"在这里变成"每个二进制都要重链"✓）。

**离线验证** ✓（M3 的出口 ✓，全部通过 ✓）：
1. `ocudu_metallib_lane` 编成 ✓、34 个 kernel 全在 ✓（`strings` 逐名 ✓）；
2. `ctest -R "demodulation_mapper_metal_unit_test|channel_equalizer_metal|port_channel_estimator_metal_mmse"` **5/5 PASS** ✓；
3. **全链路对拍** ✓（CE + EQ + DEMAP 全部走同一个库 ✓）：16QAM 与 64QAM 两份语料 × 27 capture
   ⇒ 四个 dump **0/0/0/0** ✓✓（`ab_dumps.sh` ✓）；
4. `probes_off_syntax_check.sh` **34 TU 全过** ✓；
5. **全量 `cmake --build` 绿** ✓、旧路径字符串在 build 树里**已清零** ✓（`grep -rl` 于 `*.o` ✓）。

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

### 2026-10-06 · ★ 四条腿共同的旁证：lane 的窗口**大部分不是设备执行**（下一步的线索 ✓，尚未成立为结论 ✗）

**读到的** ✓（`[ul_gpu_lane] commit -> completion (Q9-B)` ✓，四条腿都有 ✓）：

| 腿 | 每 CB 的 commit→completion median | 同腿 lane busy median | cbs/lane |
|---|---|---|---|
| mkf012（16QAM 对照）| 392.0 µs | 478.9 | 2.00 |
| mkf013（16QAM 臂）| **383.8** | 469.6 | 2.00 |
| mkf014（64QAM 对照）| 393.3 µs | 479.9 | 2.00 |
| mkf015（64QAM 臂）| **386.3** | 472.5 | 2.00 |

而 Q9-B 自己写明"commit->start is the queue, start->end is the device" ✓，
它列的**最慢**那些 CB 一律是 **commit→start 主导** ✓
（例如 mkf015 `slot=8577 ch_wt`：commit→start **2516.0 µs** / start→end 30.9 µs ✓；
同槽 `merged_hop`：1951.1 / 1439.0 ✓）。

**能说什么 / 不能说什么** ✓（纪律要求分清 ✓）：
* 能说 ✓：**每个 CB 的"提交→完成"中位数（≈386–393 µs）与 lane busy 中位数（≈470–480 µs）同量级** ✓
  ⇒ lane 那 ~480 µs 的主体是"**缓冲在队列里等待 + 提交/完成的往返**" ✓，而不是内核（< 5 µs ✓，M0 消去法 ✓）；
* **不能说** ✗：中位 CB 是否也由排队主导 —— Q9-B 只打印**最慢的 8 个** ✗
  ⇒ 这正是**下一步该补的那一次测量** ✓（把 commit→start / start→end 的**中位数**打出来，或给一个直方图 ✓）。
* 融合把这一项也压小了 ✓（392.0 → 383.8 ✓、393.3 → 386.3 ✓）—— 方向与 `busy` 一致 ✓，量级也一样小 ✓。

**⇒ 它对"下一步"的意义** ✓：M0 的结论是"靶子 = orchestration ✓，不是算力 ✓"，
而这条读数把 orchestration **指向了队列/在飞缓冲** ✓ —— 那么能动的杠杆是
**每条 hop 的命令缓冲数与它们之间的串行依赖** ✓（= M2 的领域 ✓），
而**不是**继续在同一个 CB 里合并 dispatch ✓（M1 已经把那一个做到了，收益 ~2 % ✓）。
**但它必须先成为一条"能说出几何与口径"的读数** ✓（本工作流的纪律 ✓）—— 见 §2.14 ✓。

### 2026-10-06 · ★★★★ mkf014/mkf015（B 路：**交付配置** 64QAM）**飞完 ⇒ 判决在交付形状上复现** ✓✓ ⇒ **M1 结项**

**覆盖率** ✓：两条腿的 PUSCH 都是 **64QAM** ✓（对照 108 392 + 11×16QAM + 3×QPSK ✓；臂 108 375 + 10 + 3 ✓）；
臂的融合覆盖 **108 385 / 108 388 = 99.997 %** ✓ —— **那 10 个 16QAM 跳也被融合吃下了** ✓
（`y_fused = 108 375 + 10` ✓）⇒ **两条分支都在空口上跑过** ✓，只剩开局 3 个 QPSK 跳走两步路线 ✓。

**结构** ✓：`burst dispatches`/跳 **4.0000 → 3.0000** ✓（433 622 → 325 165 ✓）；
demapper dispatches **108 406 → 3** ✓；`sites(y_fused/y_batch)` = **108 385 / 3** ✓。

**性能（对照 → 臂 ✓）**：

| 读数 | M0 基线 | 对照 | 臂 | |
|---|---|---|---|---|
| `busy` median ✓ | 479.6 | 479.9 | **472.5** | **−7.4 µs（−1.5 %）✓ < 479.6 ✓** |
| busy mean / p95 / p99 ✓ | — | 506.0 / 640.0 / 712.0 | 493.6 / 627.9 / 703.1 | 都更低 ✓ |
| `merged_hop` 段 ✓ | 465.6 | 468.6 | **456.8** | −11.8 µs ✓ |
| `[ul_pipeline]` median ✓ | 1321 | 1320.0 | **1304.0** | **−16.0 µs ✓ < 1321 ✓** |
| `[ul_gpu_pipeline]` median ✓ | 1242.8 | 1242.2 | **1226.8** | **−15.4 µs ✓ < 1242.8 ✓** |
| `≥1500 µs` 计数（ul / ul_gpu）| — | 10 743 / 2230 | **9086 / 1874** | −15 % / −16 % ✓ |
| `defer99`（pair_check）✓ | — | 896.7 | **885.8** | −1.2 % ✓ |
| p99.9（ul_gpu）✗ | — | 1823.5 | 1859.8 | **+36 µs（唯一变差的项 ✗）** |
| `stale` ✓ | 0 | **0** | **0** | **A 路那条保留没有复现 ✓✓** |

**功能与闸门** ✓：

| | 对照 | 臂 |
|---|---|---|
| CRC steady ✓ | 98.8 %（marginal ✗）| **99.9 %（CLEAN ✓）** |
| `dl_gate` ✓ | marginal / HEALTH(marginal) ✗ | **CLEAN / PASS ✓** |
| `pair_check` ✓ | **PASS** ✓（0 gaps ✓、222.9 vs 213.9 s ✓、B/hop 2635 vs 2725 ✓、defer99 ✓）|
| watchdog late max ✓ | 1651.9 µs（suspended 4 / saturated 1，9 series）| **401.6 µs（0 / 0，4 series）** ✓ |

**两对放在一起看** ✓（这是结论稳的证据 ✓）：
* **A**：臂的台面**更差**（stalls 24 vs 10、late max 6.8 vs 12.5 ms ✗）而**臂仍更快** ✓；
* **B**：臂的台面**更好**（late max 0.4 vs 1.7 ms ✓）且**臂更快** ✓；
⇒ 收益**不随"哪条腿的台面更好"翻转** ✓ ⇒ 不是环境假象 ✓。
* 两对的 CRC 都出现"一条腿 marginal、另一条 CLEAN" ✓，且**方向相反** ✓（A：臂略低 ✓；B：对照低 ✓）
  ⇒ 正是本仓 config 注释记录过的"每对总有一条腿被信道拉低" ✓，**与融合无关** ✓。

**为什么 B 的收益略小（−1.5 % vs −1.9 %）** ✓：融合**省掉的**是固定的
（eq 写 8 + eq 读 8 + nv 读 4 = **20 B/RE** ✓），而**新写的** LLR 随调制增长（16QAM 4 → 64QAM **6 B/RE** ✓）
⇒ 省/写之比在 64QAM 下略小 ✓。方向与机理一致 ✓，不是噪声 ✓（每一项读数同向 ✓）。

**⇒ ★ M1 结项** ✓✓：
1. **结构收敛达成** ✓：一跳 **4 → 3 dispatch** ✓、eq/nv 中间缓冲**不再被写也不再被读** ✓
   （融合 kernel 只写 LLR 与 nv ✓）；metallib 仍是 3 个（M3 才是整合 ✓）。
2. **功能零代价** ✓：两对腿 CRC 同档 ✓、`gaps=0` ✓、臂腿一律 CLEAN 或更好 ✓。
3. **性能改善虽小但一致** ✓（`busy` −1.5～−1.9 % ✓、`ul_gpu_pipeline` −1.2～−1.9 % ✓、长尾 −15～−28 % ✓），
   与 M0 的预言一致 ✓（内核 < 1 % ⇒ 只能省 dispatch 与访存 ✓）。
4. ✗ 没有兑现的：M0 指认的"~380 µs 信封"**没有被这次融合动到** ✓ —— 因为 CB 数仍是 2.00/跳 ✓
   （融合只去掉了同一条 CB 里的一个 dispatch ✓；G0 撤回的结论仍然有效 ✓）。
   ⇒ **要动那 380 µs，下一步是 M2（折 CE，去掉 stage 边界）或另一条 CB 路线** ✓。

### 2026-10-06 · ★★★★ mkf012/mkf013（A 路：UL 钉成 16QAM）**飞完 ⇒ M1 兑现收益** ✓✓

**一句话** ✓：**融合路线在 99.997 % 的跳上生效，功能零代价，性能每一项都更好** ✓
（`busy` −9.3 µs / −1.9 %、`ul_gpu_pipeline` −23.1 µs / −1.9 %、p99.9 −60 µs、≥1500 µs 计数 −28 %）✓✓。

**覆盖率** ✓（A 路奏效的证明 ✓）：两条腿的 PUSCH 都是 **16QAM** ✓
（对照 108 579 × 16QAM + 3 × QPSK ✓；臂 108 512 + 3 ✓）—— config 只多了一行 `cell_cfg.pusch.mcs_table: qam64` ✓。

**结构** ✓（判据的必然产物 ✓）：

| | mkf012（对照）| mkf013（臂）|
|---|---|---|
| `burst dispatches` / 跳 ✓ | 434 328 / 108 582 = **4.0000** ✓ | 325 546 / 108 515 = **3.0000** ✓ |
| demapper dispatches ✓ | 108 582 ✓ | **3** ✓（= 3 个 QPSK 跳 ✓）|
| `sites(y_fused / y_batch)` ✓ | 0 / 108 582 ✓ | **108 512 / 3** ✓ |

⇒ **4 → 3 达成** ✓、**融合覆盖 108 512 / 108 515 = 99.997 %** ✓。

**功能（红线 ✓）**：CRC steady **100.0 %（对照）vs 99.9 %（臂）** ✓ 同档 ✓；
`pair_check` **PASS** ✓（0 gaps ✓、时长 202.7 vs 208.7 s ✓、B/hop 1567 vs 1565 ✓、defer99 888.8 vs 873.3 µs ✓）；
`gaps=0` ✓（红线 ✓）。

**性能（报告制 ✓，对照 → 臂）**：

| 读数 | M0 基线 | 对照 | 臂 | 判据 |
|---|---|---|---|---|
| `[ul_gpu_lane] busy` median ✓ | 479.6 | 478.9 | **469.6** | **< 479.6 ✓** |
| busy mean / p95 / p99 | — | 501.6 / 634.5 / 704.8 | 488.3 / 616.3 / 688.8 | 都更低 ✓ |
| busy split `merged_hop` ✓ | 465.6 | 465.7 | **451.4** | −14.3 µs ✓ |
| `[ul_pipeline]` median ✓ | 1321 | 1280.0 | **1259.0** | **< 1321 ✓** |
| `[ul_gpu_pipeline]` median ✓ | 1242.8 | 1237.2 | **1214.1** | **< 1242.8 ✓** |
| p99.9（ul / ul_gpu）| — | 1895 / 1852 | **1835 / 1806** | 更低 ✓ |
| `≥1500 µs` 计数（ul / ul_gpu）| — | 6165 / 2031 | **4431 / 1474** | −28 % / −27 % ✓ |
| `stale` ✗ | 0 | **0** | **4 / 5** | 见下 ✓ |
| busy max ✗ | — | 2243.5 | 5395.6（一个离群 ✓）| 见下 ✓ |

**两点必须一起报的保留** ✓（都不改结论 ✓，但引用数字时要带上 ✓）：
1. **`dl_gate` 把两条腿都判成 `STALL - re-fly`** ✓ —— **对照腿的停顿反而更糟** ✓
   （watchdog late max **12 504.8 µs** ✓、frontier 2870、AT/BELOW0 8 ✓；
   臂 6 757.1 µs、frontier 2477、AT/BELOW0 25 ✓）⇒ 这是**台面环境** ✓，
   正是用户裁决"环境不是起飞条件"所指的那件事 ✓；**引用时延数字时**要带这条 ✓（README 的参考项规则 ✓）。
2. **臂的 `stale=4/5` 而对照是 0** ✗ —— 4 个跳的跨度 > 8 ms（HARQ RTT ✓，10.1–13.3 ms ✓），
   与上面那次 6.8 ms 的停顿同一量级 ✓ ⇒ **最可能是同一个环境现象** ✓，
   但**没有**证据说融合路线对它免疫 ✗ ⇒ 记为**待观察** ✓（§2.12 ✓）；
   **本轮不重飞** ✓：红线（CRC/gaps）与主判据都过了 ✓，重飞是"引用 p99.9 之前"的动作 ✓。

**⇒ M1 的判决（在 16QAM 负载上 ✓）**：**融合有价值** ✓ —— 收益不大（~2 %）但与 M0 的预言一致 ✓
（内核工作量 < 1 % ✓ ⇒ 收益只能来自**省掉的那次 dispatch、那段中间缓冲与它的访存** ✓），
**且没有任何功能代价** ✓。

**⇒ 下一步 = B（§2.13 ✓）**：把这套收益搬到**交付配置**上（PUSCH 实际是 64QAM ✓），
然后飞 **`mkf014`/`mkf015`**（用原来的 `gnb_pinned_mcs13.yml` ✓，不加任何 MCS 覆盖 ✓）。

### 2026-10-06 · ★★★ mkf010/mkf011 飞完：**接线正确 ✓，但腿的调制不是 16QAM ✗ ⇒ 这一对判不了融合**（§2.11 已按此重写 ✓）

**腿** ✓：`mkf010-m1-fused-off`（23:42）与 `mkf011-m1-fused-on`（23:46）✓，都是 `dual / quiet / gpu` ✓、
`gnb_pinned_mcs13.yml` ✓、臂带 `OCUDU_LANE_FUSE_EQDEMOD=1` ✓（stderr 里 `knob : OCUDU_LANE_FUSE_EQDEMOD=1` ✓）。

**★ 好的一面（接线在空口上被证明 ✓✓）**：
```
控制: [metal_stats] burst … dispatches=433572 (equalizer=108393 demapper=108393 channel_estimator=216786)  ⇒ 4.0000/跳 ✓
      eq_batch … sites(y_batch=108393 y_fused=0)
臂  : [metal_stats] burst … dispatches=433252 (equalizer=108316 demapper=108306 channel_estimator=216630)
      eq_batch … sites(y_batch=108306 y_fused=10)          ⇒ demapper 恰好少 10 次 ✓ = y_fused ✓
```
⇒ **每一个融合 dispatch 恰好替换一个 demapper dispatch** ✓（10 = 10 ✓，账目闭合 ✓）；
那 10 跳**全部 `crc=OK`** ✓（两条腿里 16QAM 的 PUSCH 都是 10 跳、都 OK ✓）
⇒ 融合 kernel 在空口上产出的软比特**可解** ✓。

**★ 坏的一面（M1 的前提被证否 ✗）**：PUSCH 的调制分布（按 `PUSCH: rnti` 行统计 ✓）：

| 腿 | 64QAM | 16QAM | QPSK |
|---|---|---|---|
| mkf010（对照）| **108 380** | 10 | 3 |
| mkf011（臂）| **108 303** | 10 | 3 |

⇒ **腿跑的是 64QAM，不是 16QAM** ✗ ⇒ 融合路线只覆盖 **10 / 108 316 ≈ 0.009 %** 的跳 ✗
⇒ 这一对**不能判融合** ✗（`busy` 478.9 vs 479.9 µs 的差就是噪声 ✓：两条腿 99.99 % 的跳走同一条路 ✓）。

**根因** ✓（读代码得到 ✓，不是猜 ✓）：cell config 把 `min_ue_mcs = max_ue_mcs = 13` ✓，
配置注释写"MCS 13 = 16QAM ✓" —— 那是 **MCS 表 1（qam64）** 的映射 ✓
（`lib/ran/pdsch/pdsch_mcs.cpp` 的 `MCS_INDEX_TABLE_1`：10–16 = 16QAM ✓、17+ = 64QAM ✓）；
**而 PUSCH 的表默认是表 2（qam256）** ✗（`du_high_config.h:286`：`pusch_mcs_table mcs_table = pusch_mcs_table::qam256;` ✓），
表 2 的 index 13 = **64QAM** ✓（`MCS_INDEX_TABLE_2`：5–10 = 16QAM、11–19 = 64QAM ✓）
⇒ **同一个"MCS 13"在两条表里是两个调制** ✗✓。
**证据链** ✓：① 观察到的调制就是 64QAM ✓；② MCS **确实被钉住** ✓（同一 grant 宽度的 TBS 完全确定 ✓：
51 PRB → 3072 / 1505 / 2754 / 2562 / 3009 分别对应不同的 PRB 起止与 RV ✓，没有链路自适应的散布 ✓）
⇒ 只能是"表 2 + index 13" ✓（表 1 会给 16QAM ✗、表 3 会给 QPSK ✗，都与观察不符 ✗）。
★ 这条**不是本工作流引入的** ✗：M0 的腿（mkf001–mkf009）与上一工作流的腿**同样是 64QAM** ✓
—— 也就是说 **M1.0 决议里那句"本工作流的判据腿全部是 MCS 13 = 16QAM"从一开始就不成立** ✗✗。

**其余读数** ✓（都正常，且与 M0 基线逐项吻合 ✓ —— 因为两条腿 99.99 % 同路 ✓）：

| 读数 | M0 基线 ✓ | mkf010（对照）| mkf011（臂）|
|---|---|---|---|
| `[ul_gpu_lane] busy` median | 479.6 µs | **478.9** | **479.9** |
| split `merged_hop` / `ch_wt` | 465.6 / 36.9 | 464.2 / 36.6 | 465.7 / 36.8 |
| `cbs/lane` | 2.00 | 2.00 | 2.00 |
| `[ul_pipeline]` median | 1321 | 1323.0 | 1319.0 |
| `[ul_gpu_pipeline]` median | 1242.8 | 1240.5 | 1241.3 |
| `stale` | 0 | 0 | 0 |
| CRC steady | 99.1 % | 98.7 %（marginal ✗）| **99.2 %（CLEAN ✓）** |

`pair_check` ✓：同一对 **PASS** ✓（0 gaps ✓、B/hop 2638 vs 2692 ✓、defer99 888.4 vs 888.7 µs ✓）；
`dl_gate`（臂）✓：**CLEAN / PASS** ✓。
⇒ **新二进制的默认路线与 M0 基线一致** ✓（作为"接线没有碰坏默认路径"的回归证据有效 ✓），
**但它不是融合的判决** ✗ —— 按本工作流的纪律，**不许把它读成"融合无收益"** ✗。

**工具坑（顺手记 ✓）**：`pair_check.sh` 只认 `LEG_LOGDIR`（默认它自己的 `logs/` ✗），
**没有**上一会话给 `ul_health`/`dl_gate`/驱动做的"泛搜 `doc_chinese/*/wip/logs`" ✓
⇒ 本工作流的腿要 `LEG_LOGDIR=…/metal_kernel_fusion/wip/logs bash pair_check.sh …` ✓
（已写进 §2.11 的命令 ✓；要不要给它补上泛搜，等有空统一做 ✓）。

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

### 2026-10-07 · ★★ 用户裁决：融合路线**翻成默认开** ✓；★ M3（metallib 整合）**完成** ✓

**① 翻默认开** ✓（用户 2026-10-07 ✓，依据 = 两对腿的判决 ✓）：
`OCUDU_LANE_FUSE_EQDEMOD` 从"非空即开、默认关"改为**"未设或非零 = 开、`=0` = 关"** ✓
（与 `OCUDU_EQ_DEFER_ENCODE` / `OCUDU_EQ_DIRECT_GRID` 同一形状 ✓）。
**为什么可以翻** ✓：两对腿在 16QAM 与 **64QAM（交付配置）** 上都测到严格改善 ✓
（dispatch/跳 **4.0000 → 3.0000** ✓、`busy` −1.5～−1.9 % ✓、长尾 −15～−28 % ✓、CRC 同档 ✓、`gaps=0` ✓）；
**要一起记住的前提** ✓：路线只在 **1 层 + (16QAM|64QAM) + 无 EVM** 时生效 ✓，
其余形状**自动**走两步路线 ✓ 并在日志里点名 ✓。
**离线验证** ✓（不用电台 ✓）：默认（不带 env）在 16QAM 与 64QAM 语料上都是 `y_fused=1` ✓；
`OCUDU_LANE_FUSE_EQDEMOD=0` 回到 `y_batch=1, y_fused=0` ✓。
★ **M4 的"交付默认值不变"由此被用户裁决取代** ✓ —— 记在架构文档 §7 与 §0 ✓，不是被遗忘 ✗。

**② M3 完成** ✓（3 个 metallib → 1 个 `ocudu_lane.metallib` ✓，详细规格见 §4 ✓）：
15 个 `.metal` → 1 个库（**200 892 B** ✓）、**34 个 kernel 入口点全在** ✓、三个引擎各加载它 ✓
（**加载 3 → 1** ✓）、`IEEE_MATH_SOURCES` 那三个严格文件照旧 ✓。
**两处只有读代码才看得见的坑** ✓：① `ocudu_metal_burst.mm` 的 **ablation 臂自带 resolver** ✗
（只在 `OCUDU_LANE_ABLATE` 打开时走到 ⇒ 会静默失效 ✗，已改 ✓ 并离线验证 `ABLATION ON` ✓）；
② 改库路径后**只重编 gnb 不够** ✗ —— 三个引擎的**单测二进制**里烘着**已删掉的**旧路径 ✓
⇒ 假失败（SEGFAULT/Failed ✗），**全量重编后 5/5 PASS** ✓（教训写进 §4 ✓）。
**离线验证** ✓：`ctest` 5/5 ✓、**全链路对拍（CE+EQ+DEMAP 同一个库 ✓）两份语料 × 27 capture = 0/0/0/0** ✓、
probes-off 34 TU ✓、全量 `cmake --build` 绿 ✓、build 树里旧路径字符串清零 ✓。

### 2026-10-07 · ★★ fence 消去臂（离线）：**lane 窗口的约 1/3 是那两道等待** ✓ —— M2 的前提**成立** ✓
> ★★ **本条的推论已被空口腿 `mkf018`/`mkf019` 推翻** ✗ —— 见下面那条 memo ✓：
> 空口上这条等待**几乎不花钱**（`busy` 492.2 → 492.0 ✓、`merged_hop` 455.9 → 455.5 ✓），
> 而离线那 32 % 之所以存在，是因为**离线路线**上那道等待是"**指名自己那一代**、且真的在等" ✓，
> 空口路线走的是"**等全局最新**"的回退 ✓ —— **同一个变量名，两条不同的等待** ✗。
> **保留本条的原因** ✓：它记录了**离线工具的口径边界** ✓，以及"**先交替再比**"这条纪律 ✓。

**为什么做** ✓：上一轮的中位数读数是"中位 CB 14 % 排队 / 86 % 设备（209 µs）" ✓，
而 M0 的消去法说**取掉内核本体窗口不动** ✗ ⇒ 问题变成"**设备在等什么**" ✓。
lane 的每条 CB 上只编了两道等待 ✓（`burst_ensure_open()` ✓）：
**估计器的 stage fence** ✓（burst 读估计器写的东西 ✓）与 **grid 生产 fence（D1）** ✓。

**臂** ✓：新旋钮 `OCUDU_LANE_ABLATE_FENCE=1` ✓（`ocudu_metal_burst.mm` ✓）——
**只把这两道等待不编码** ✓，其余一字不改 ✓；等待被**消费掉**（设等待的状态机照常 ✓）✓；
响亮自报 ✓ + **计数** ✓（不计数就等于"以为旋钮生效了" ✗）。
★ **它不是一条可用路线** ✓：没有 stage fence，均衡器可能读到估计器还没写完的内存 ✓ ⇒
**CRC 无意义** ✓（与 `OCUDU_LANE_ABLATE` 同一契约 ✓）；默认关 ✓。

**离线测量** ✓（`--repeat 16` ✓，**交替 3 轮** ✓）：★ **必须交替** ✗ ——
第一次我先跑 3 次基线再跑 3 次臂 ✓，基线给出 **285.9 → 160.6 → 112.6 µs 的单调下降** ✗
（机器预热/顺序效应 ✓），而臂稳定在 ~120 ✓ ⇒ 那种比法**把顺序效应当成了旋钮效应** ✗
（本工作流"背靠背一对"的纪律，在离线工具上同样适用 ✓）。

| 轮 | 基线 `busy` mean | 臂 `busy` mean | 基线 `merged_hop` | 臂 `merged_hop` | 配对差（`merged_hop`）|
|---|---|---|---|---|---|
| 1 | 409.1 | 174.4 | 353.6 | 146.4 | **−207.2** ✓ |
| 2 | 293.6 | 166.3 | 271.9 | 152.8 | **−119.1** ✓ |
| 3 | 287.9 | 198.1 | 264.3 | 181.2 | **−83.1** ✓ |

⇒ **3/3 同向** ✓，`merged_hop` 窗口被削掉 **83～207 µs**（平均 ≈ **136 µs ≈ 基线的 32 %** ✓）；
`queue` mean 没有一致方向 ✓（168→74、96→78、69→95 ✓）⇒ 削掉的是**设备侧等待** ✓，与 `start→end` 的口径一致 ✓。

**⇒ 两条结论** ✓：
1. ★ **M2 的前提成立** ✓：那 ~380 µs 里**确实有一大块是"等依赖"** ✓（离线 ≈ 136 µs ✓）；
   §2.14 里那条"先证实再决定 M2"的步骤**已完成** ✓ ——
   **但要害是"跨 CB 的等待"** ✓：M2 折 CE 的价值在于把这条依赖变成 **kernel 内** ✓（不再需要 fence ✓），
   **不是**"少一条 CB"本身 ✓（M0b 已经证明后者会牺牲重叠 ✗）。
2. ★ **M0b 的结论被正确限定** ✓：它只证明"event/burst 旋钮下 busy 无法区分" ✓，
   **不能**推出"fence 不花钱" ✗（那个旋钮根本没分开 CB 数 ✗）—— 本轮把 fence 的价钱**量出来了** ✓。

**⇒ 下一步 = 一条空口腿** ✓（结构测量 ✓，一个变量 ✓）：`OCUDU_LANE_ABLATE_FENCE=1` ✓ vs 正常，
用钉住 MCS 的 config ✓（CRC 应当在臂上**塌掉** ✓ —— 那正是"臂真的生效了"的证明 ✓，
与 M0 消去三条腿时 CRC 掉到 92.7/98.6/97.8 % 同一类证据 ✓）；
读数：`[ul_gpu_lane] busy` + `busy split`（`merged_hop` ✓）+ Q9-B 的 **队列/设备均值** ✓。

### 2026-10-07 · ★★★ mkf023（**corr 合并**）：**空口一个 dispatch 边界 ≈ 6.5 µs** ✓ —— 不是 39 µs ✗ ⇒ **四个假设全部被证否** ✗✗

**① 旋钮确实生效 ✓**（这次先读计数 ✓）：`ce_sites … merged=157 299` ✓（108 366 跳 ⇒ **1.45/跳** ✓）——
即**约 45 % 的跳由"两个 corr dispatch"变成"一个"** ✓（`corr_a`/`corr_rhp` 各自的 1.45/跳保留 ✓，
它们是**工作量**计数 ✓）。⇒ 这是一次**结构成立**的测量 ✓，不是 p146/p147 那种"旋钮没动" ✗。

**② 结果：动的是它该动的那一段，量级很小 ✓**：

| 读数 | 基线（4 腿簇 ✓，并发 2）| **mkf023** | 差 |
|---|---|---|---|
| `busy split ch_wt` ✓（corr 内核所在的段 ✓）| 36.3–37.3 | **30.4** | **−6.3 µs** ✓ |
| `busy split merged_hop` ✓ | 455.5–457.0 | **454.7** | **−1.6 µs** ✗（不动 ✓）|
| `busy` mean ✓ | 492.0–494.3 | **485.1** | **−7…−9 µs** ✓（= 两段之和 ✓）|
| `busy` median / p95 / p99 ✓ | 472.3–473.6 / ~627 / ~699 | **463.8 / 618.2 / 679.4** | −9 / −9 / −20 |
| Q9-B 队列 / 设备 mean ✓ | 102.6–103.8 / 246.0–247.1 | 103.1 / 242.5 | = / −4 |

**⇒ ★ 那条边界值 ≈ 6.5 µs** ✓（空口 ✓）—— **不是仓里注释写的 ~39 µs** ✗。
而且它**与融合自己的收益自洽** ✓：融合拿掉**一个** dispatch ⇒ `busy` −7.4 / −7.6 µs ✓（两对腿 ✓）
⇒ **本路线上一个 dispatch 边界 ≈ 7 µs** ✓✓（两条独立的腿、两次不同的合并，都指向同一个数 ✓）。
★ 顺带修正记录 ✓：`OCUDU_CE_CORR_MERGED` 注释里"空口 ~39 µs / 离线 ~1.6 µs"那句
**在本路线/本二进制上不复现** ✗（本条给出的是 ~6.5 µs ✓，与离线 1.6 µs 同量级 ✓，
只差 ~4 倍而非 ~25 倍 ✓）—— 那句大概来自**另一种结构**（旧的两库/cpu_gpu 形态里跨 **CB** 的边界 ✗，
不是同一条 CB 内的 dispatch ✓）。**引用它时要带上这个限定** ✓。

**③ ⇒ 那 ~456 µs 的信封，四个假设全部被证否** ✗✗（每一个都有腿 ✓）：

| 假设 | 腿 | 结论 |
|---|---|---|
| 内核工作量 ✓ | M0 消去三条腿 ✓ | ✗（消去后 busy 477–480 ✓）|
| 跨 CB 的 **fence** ✓ | `mkf018/019` ✓ | ✗（拿掉后 492.2 → 492.0 ✓、CRC 也不塌 ✓）|
| **兄弟 lane 争设备** ✓ | `mkf022` ✓ | ✗（并发减半只 −10 % ✓）|
| **dispatch 边界的价钱** ✓ | `mkf023` ✓ | ✗（一个边界 ~6.5 µs ✓ ⇒ 7 个 ≈ 46 µs，解释不了 456 ✓）|
| ★ **CB 边界本身** ✓ | **`mkf024-split`** ✓（§2.17 ✓）| ✗（**加一整条 CB 边界，端到端 ±1 µs 内不动** ✓ —— 见下面那条 memo ✓）|

⇒ **算术摆在这里** ✓：一跳 `merged_hop` ≈ **456 µs**，而**能加起来的项**只有
**~7 个 dispatch × ~7 µs ≈ 50 µs** ＋ 队列/宿主等（另计 ✓）⇒ **仍然有 ~400 µs 说不出归属** ✗。
**这不是失败** ✓：**四个看似合理的解释被四条腿逐个排除** ✓ —— 这本身就是可交付的知识 ✓
（下一个会话不必再试这四个 ✗）。
★ **2026-10-07 补** ✓：**第五个（CB 边界本身）也已被 `mkf024-split` 证否** ✗（§2.17 ✓）⇒
**能想到的结构全部量过了** ✓ ⇒ **继续归因的边际收益为零** ✗（详见 §2.17 与它那条 memo ✓）。

**④ ★★ 想用的那台仪器，本机没有** ✗✓（**两分钟离线查清 ✓，省掉一整条线** ✓）：
仓里**从来没有**逐 dispatch 的 GPU 计时 ✗（`grep sampleCounters|MTLCounterSampleBuffer` 在 `lib/` **0 命中** ✓），
所以一切只能读**整条 CB** 的窗口 ✓。自然的下一手是 **Metal 的 `MTLCounterSampleBuffer`** ✓
（在 **dispatch 边界**打点 ✓ = "一条 CB 里那 ~7 个 dispatch 各自花了多少" ✓）。
**离线查证** ✓（最小 `.mm` ✓，`/tmp/cscheck.mm` ✓，不需电台 ✓）：
```
device: Apple M4 Pro
atDispatchBoundary: NO        ← ★ 不支持 ✗
atStageBoundary:    YES
counter set: timestamp / GPUTimestamp
```
⇒ ★ **`MTLDevice.supportsCounterSampling(MTLCounterSamplingPointAtDispatchBoundary)` 在 M4 Pro 上是 `NO`** ✗
⇒ **逐 dispatch 的 GPU 计时在这台机器上拿不到** ✗（而这正是那个信封唯一缺的分解 ✓）。
**`atStageBoundary` 有** ✓，但本 lane 的 burst **一条 CB 一个 encoder** ✓（`shared_burst` 全程复用 `s.enc` ✓）
⇒ stage 边界 ≈ CB 自己的起止 ✓ ⇒ **不比已有的 `GPUStartTime`/`GPUEndTime` 多任何信息** ✗。
**⇒ 结论** ✓：**这个信封不能再用"更细的 GPU 计时"去归因了** ✗ —— 平台不给 ✓。
**记账** ✓：把这条**否定的可行性**写下来 ✓，下一个会话就不必再花时间试它 ✗
（与"四个假设已证否"同一类可交付知识 ✓）。

⚠ **本腿的环境注记** ✓：`stale=2`（>8 ms，max 15.3 ms ✓）且队列 max = **8022.7 µs** ✓
⇒ 该腿有一次停顿 ✓（属环境 ✓；均值仍在簇内 ✓ ⇒ 结论不受影响 ✓）。

### 2026-10-07 · ★★ M4 验收：**三项结清** ✓ —— 探针 OFF **原本编不过** ✗，修出一类**既有缺陷**（**8 处 / 6 文件** ✓）

**① Linux 不变** ✓（**静态核验** ✓，本机无 Linux 工具链 ⇒ **诚实标注为静态** ✓）：
`METAL_OFFLOADS_ENABLED` 非 Apple 恒 OFF ✓（显式打开会 `FATAL_ERROR` ✓）⇒ 全部 metal 目录不进入 ✓；
新增 include 无平台泄漏 ✓（两条是平台无关类型 ✓、一条只在 Metal 门控测试里 ✓）；
接口新方法**有默认实现** ✓ ⇒ 既有后端与 CPU 路线不受影响 ✓。

**② ★ 探针 OFF：不通过 → 已修复** ✓✗ —— 缺陷模式**八次重复** ✓（`gnb` 暴露 6 ✓、**`BUILD_TESTING=On` 全量**又暴露 2 ✓）：
★ **运行时功能声明在探针条件块里、调用点在块外** ✗。
`ul_stall_watchdog.h` 的 `p` 字段 ✓、`dft_metal_engine.mm` 的 `dft_stats()` 调用 ✓、
`port_channel_estimator_metal_mmse_impl.cpp` 的**匿名命名空间开在 `OCUDU_CE_TIME` 块内** ✓
（连带删掉 `note_sigma2` ✓、矩阵缓存 ✓、`matrix_cache_enabled` ✓）、
`mmse_engine.mm` 的 `mmse_stats()` 类型+访问器 ✓（15 个调用点里 **4 个是运行时代码** ✓）与
`drop_miss_wait_armed()` ✓（**运行时伪证旋钮** ✓，与探针无关 ✗）、
`lane_probe.h` 探针关闭版缺 `merged_hop` 标签 ✓（`set_commit_label()` 是**无条件**调用的 ✓）。
★ **另有两处只在 `BUILD_TESTING=On` 的全量构建下暴露** ✓（**`gnb` 看不到** ✓，**CI 的范围** ✓）：
`macos_compat_test.cpp` 的回读 lambda **无条件定义、只在探针下调用** ✓（`-Wunused-variable` ✗）；
★ `pusch_demodulator_impl.cpp` 的 **"ce device estimates" 契约检查与计数器同在 `METAL_STATS` 块内** ✗
⇒ 探针关闭时**不注册** ✓，而**测试断言它必须注册** ✗ ⇒ 那是**测试对交付物的期望** ✓、不是交付物的缺陷 ✓
（修法：计数器与注册无条件化 ✓、计数点保持 guard ✓ ⇒ 探针关闭时报 **0/0** ✓ = 事实 ✓）。
⇒ **共 8 处 / 6 文件** ✓。

★ **为什么没人报** ✓：`.github/workflows/ccpp.yml` 用**默认选项**（探针 OFF ✓）+ `BUILD_TESTING=On` 构建 ✓，
而**本分支 `ahead of origin by 22` 从未推送** ✓ ⇒ **CI 从未跑过它** ✓ ⇒ ★ **这棵树一旦推送，CI 会直接红** ✗
—— 这一项修复因此**不只是形式验收** ✓，它**解锁了 CI** ✓。

**验证三重** ✓：三配置全新编译 **0 error** ✓（全关 ✓ / 交付默认 ✓ / 全开含 `ENABLE_CE_TIME` ✓）、
★ **`BUILD_TESTING=On` 的探针全关全量构建 0 error** ✓（**CI 范围** ✓）、
★ **交付形态输出与改动前逐位一致** ✓（四 dump ✓）、
**单测两种形态都 PASS** ✓（ON：含 `[fused]` ✓；OFF：全量 **8254 项无真实失败** ✓ —— 9 项 "failed" 全是 `Skipped` ✓）。

**★ 为什么没被发现** ✗：`ul_stall_watchdog.h` 是 **10-04** ✓、包含它的 `.o` 是 **09-25** ✓
⇒ ★ **增量构建从未重编** ✗ ⇒ 树看着健康、**全新构建几秒就失败** ✓。
**教训** ✓：**"构建目录是最新的" ≠ "这棵树编得过"** ✗ —— 验收**至少要做一次全新构建** ✓
（与 M3 那次"单测二进制烘着已删库路径"**同一族** ✓）。

**★ 既有检查工具为何没拦住** ✗（`wip/probes_off_syntax_check.sh` ✓，M3 记录说"34 TU 全过" ✗ 与真实构建矛盾）：
① 它**只去掉一个键**（`FLOW_PROBES` ✓）⇒ 对其中绝大多数（`METAL_STATS` ✓ / `CE_TIME` ✓）**完全不覆盖** ✗；
② 它**只做 `-fsyntax-only`** ✗ ⇒ `-Wunused-private-field` 那处**只在 codegen 时报** ✓，看不见 ✗。
**已修** ✓：三键各跑一遍 ✓ + **真编译** ✓ + 过滤加宽 ✓。★ **回归实证** ✓：改进版在**修复前**的代码上抓到
**3 处 FAIL** ✓，而旧版对同一代码报 **"34 TU 全过、safe"** ✗。修复后 **114 编译 × 3 键全 OK** ✓。
（残留盲点 ✓：没被 TU 直接使用的头文件不在扫描内 ✓ ⇒ **全新构建是最终权威** ✓。）

**③ 对照表** ✓：两对腿在案 ✓（16QAM `busy` **478.9 → 469.6** ✓、64QAM **479.8 → 472.5** ✓、
dispatch **4 → 3** ✓、覆盖 **99.997 %** ✓、CRC 同档或更好 ✓）。
★ **允许的回退只有一项** ✓：64QAM **p99.9 +29 µs** ✗（A 路 p99.9 是改善 ✓ ⇒ 非路线性质 ✓）。

### 2026-10-07 · ★★ RANK 3 实现完成（**CE 融合第 3 项** ✓）：两组相关**一个派发** ✓，离线逐位一致 ✓，★ **默认先关** ✓

★ **目的** ✓：接 §2.22 ✓。**性质 = 结构收敛** ✓（**不是**性能 ✓ —— 性能目标已于 §2.20(6) 关闭 ✓）。

| 项 ✓ | 结果 ✓ |
|---|---|
| **kernel** ✓ | 工作项抽成 `mmse_corr_a_rhp_block()` ✓、**单组与融合对**都调它 ✓；新增 `mmse_corr_a_rhp_pair` ✓ |
| **host** ✓ | `pack_corr_group()` ✓（参数/映射唯一构造点 ✓）、`bind_corr_scalars()` ✓（F2 规则收一处 ✓）、`encode_corr_pair()` ✓、`corr_pair_pipe` ✓ |
| **计数** ✓ | 新增 `corr_pair`（**派发** ✓）；`corr_a`/`corr_rhp`/`corr_merged` 改计**组构建** ✓ ⇒ 三者仍相等 ✓、跨路线可比 ✓ |
| ★ **离线对拍** ✓ | `q64c` ✓ / `q16c` ✓ / **真 QPSK** ✓ **各 27 capture × 4 dump = 0 差异** ✓（`--repeat 8` ✓、对照显式 `=0` ✓）|
| ★ **计数自证** ✓ | 臂 `merged=16 corr_pair=7` ✓（7 对 + 2 单 = 16 组 / 9 派发 ✓）；对照 `corr_pair=0` ✓ |
| **单测** ✓ | metal 5/5 PASS ✓ |
| **探针 OFF** ✓ | `probes_off_syntax_check.sh build` **114 编译 × 3 键全 OK** ✓ |
| **全量构建** ✓ | 交付默认 0 error ✓；★ **探针 OFF + `BUILD_TESTING=On` 的全新构建**（= CI 范围 ✓）进行中 ✓ |
| **metallib** ✓ | **36 个 kernel** ✓（新增 `mmse_corr_a_rhp_pair` ✓）|
| **提交** ✓ | `7eb73df7ca` ✓ |

★★ **本项最有价值的一条记录是"第一次对拍是空的"** ✗（详见 §2.22(3) ✓）：
`--repeat 1` 下 `corr_pair = 0` ✓ —— 边缘组的融合形态要**上一跳的 sigma2 块** ✓、**进程第一跳没有** ✓
⇒ **融合根本没生效** ✓，而**对拍照样报 0 差异** ✓。★ **这是"旋钮没生效"与"没效果"同形** ✓
在本工作流的**第三次**现身 ✓ ⇒ ★ **计数自证必须取自"将要引用的那一次运行"** ✓。

★ **顺带** ✓：`corr_stage::nof_systems` ✓（注释整段为此而写 ✓）**此前从未被读** ✗ ⇒ 改为按字段读 ✓，
**今天恒等** ✓（三份语料复验 ✓），但融合让"两组的 system 数"成为**承重信息** ✓。

★ **默认关闭的理由** ✓（**唯一** ✓）：RANK 2 的红线**还没有干净的腿** ✓ ⇒ 不让一条腿替两件事作答 ✓。

★ **下一步 = 两条腿** ✓（见下节 ✓）：`mkf027` 对照（补 RANK 2 红线 ✓）→ `mkf028` 臂（`OCUDU_CE_CORR_PAIR=1` ✓）。

### 2026-10-07 · ★★ mkf026-cfo-fused（**CE 融合 RANK 2 的交付验证**）：**结构自证 ✓、性能持平 ✓、★ 红线被环境污染 ✗**（离线功能证据完好 ✓）

★ **目的** ✓：`OCUDU_CE_CFO_FUSED`（`pilots_cfo` + `pilots_apply_cfo` 合一 ✓）转默认后 ✓，验交付红线 ✓
—— `mkf025` 跑在 `92dbdee134` ✓（**只有 O1 默认 ✓、无 RANK 2** ✗）⇒ **本条腿专验 RANK 2** ✓。

| 判据 ✓ | 预登记 ✓ | 实测 ✓ | 判定 |
|---|---|---|---|
| ★ **结构自证** ✓ | `cfo_fused` ≠ 0 ✓ | ★ **`cfo_fused=108 455`** ✓ = **跳数** ✓（`pilots_cfo` 同值 ✓）⇒ ★ **每跳一次、100 % 覆盖** ✓ | ✅ |
| `gaps` ✓ | = 0 ✓ | **0** ✓（`rx_overflows=0` ✓）| ✅ |
| ★ **CRC** ✗ | 同档（`mkf025` 99.5 % ✓）| ★ **95.4 % DEGRADED** ✗ | ✗ **被环境污染** ✓（见下 ✓）|
| ★ **`stale`** ✗ | 0（`mkf025` ✓）| ★ **1**（13 560 µs ✗）| ✗ **被环境污染** ✓ |
| 性能不劣化 ✓ | `busy`/`ul_pipeline` ✓ | **466.8 / 1306.0** ✓（`mkf025` 465.7 / 1306.0 ✓）| ✅ **持平** ✓ |
| ★ **CE 自身耗时** ✓ | —— ✓ | ★ **31.9 µs** ✓（`mkf025` 31.5 ✓）| ✅ **持平** ✓ |
| `ch_wt` 段 ✓ | 不该动 ✓ | **31.4** ✓（`mkf025` 30.9 ✓）| ✅ 持平 ✓（**正确 ✓**：CFO 融合在**抽取**阶段 ✓、不在 `ch_wt` 那条 CB ✓）|

#### ★ 红线为什么判为"环境"而非"融合" ✓（三条独立证据 ✓）

| 证据 ✓ | mkf025 ✓ | mkf026 ✓ |
|---|---|---|
| **同轮**其它腿的 CRC ✓ | —— | ★ **`mkf024` 95.6 %** ✗、**`mkf023` 93.1 % DEGRADED** ✗ ⇒ **那一轮普遍差** ✓ |
| 停顿分类 ✓ | `suspended=1 saturated=0` ✓ | ★ **`suspended=3 saturated=5`** ✗ |
| 停顿计数（`phy_series`）✓ | **4** ✓ | ★ **14** ✗ |
| `load1` ✓ | 3.47 ✓ | **4.03** ✗ |

⇒ ★ **`stale=1` / CRC 95.4 % 落在"环境"这一类** ✓，**不是融合引入** ✓。
★ **而融合侧的读数全部持平** ✓（CE 31.9 vs 31.5 ✓、`busy` 466.8 vs 465.7 ✓、`defer_wait` median 680.7 vs 677.9 ✓）
—— ★ **若融合有副作用，CE 自身耗时不会纹丝不动** ✓。

#### ★ 功能证据（离线 ✓，不依赖腿 ✓）

★ **融合 vs 两派发逐位对拍** ✓：`q64c` 与 `q16c` **各 27 capture × 4 dump = 0 差异** ✓✓
（`OCUDU_CE_CFO_FUSED=0` 为对照臂 ✓）⇒ ★ **功能正确性已在交付形态上证明** ✓；
★ **计数自证** ✓：`cfo_fused=8` vs `0` ✓（`--repeat 8` 下 ✓）。

#### ⇒ 结论与后续 ✓

★ **RANK 2 的结构与功能都已验证** ✓（100 % 覆盖 ✓、逐位一致 ✓、性能持平 ✓）；
✗ **但"红线同档"这一项本条腿给不出干净证据** ✓ ⇒ ★ **记作"待重飞"** ✗（**不是"通过"也不是"否决"** ✓）。
★ **重飞判据** ✓（下一次常规飞行即可 ✓，无需专腿 ✓）：`stale=0` ✓ + CRC **≥ 99 %** ✓ + `cfo_fused` ≠ 0 ✓ + `gaps=0` ✓。
★ **同时更新施工顺序** ✓（§2.21(6) ✓）：**RANK 1 已验收 ✓、RANK 2 已实现待干净红线 ✓** ⇒
下一个可选 = **RANK 3**（四个相关派发合一 ✓，再省 3 派发 ✓，需两个参数块 + 组选择器 ✓）。

### 2026-10-07 · ★★ mkf025-o1-default（**相关矩阵合并转默认后的交付验证**）：**红线通过 ✓、`ch_wt` −5.9 µs ✓**（CI 三元组已全绿 ✓）

★ **目的** ✓：`OCUDU_CE_CORR_MERGED` 从"默认关"转为"**默认开**"（§2.21 施工顺序第 3 步 ✓）后，
★ **交付形态变了** ✓ ⇒ 红线（CRC 同档 + `gaps=0`）**只有腿能证** ✓ —— **不能拿离线对拍替代** ✗。

| 判据 ✓ | 预登记 ✓ | 实测 ✓ | 判定 |
|---|---|---|---|
| ★ **CRC** ✓ | 同档或更好 ✓ | **99.5 % CLEAN** ✓ | ✅（基线簇 99.3–99.8 % ✓）|
| ★ **`gaps`** ✓ | = 0 ✓ | **0** ✓（`rx_overflows=0` ✓）| ✅ |
| ★ **结构自证** ✓ | `merged` ≠ 0 ✓ | ★ **`merged=161 177`** ✓ = **1.49/跳** ✓（= `corr_a` = `corr_rhp` ✓ ⇒ **全部相关构建走合并路径** ✓）| ✅ |
| ★ **`ch_wt`** ✓ | 36.8 → ~30.4 µs ✓ | ★ **30.9 µs/lane** ✓ | ✅ **−5.9 µs** ✓ |
| ★ **`busy` median** ✓ | 472.5 → ~466 ✓ | ★ **465.7 µs** ✓ | ✅ **−6.8 µs** ✓ |
| `ul_pipeline` / `ul_gpu_pipeline` ✓ | 不劣化 ✓ | **1306 / 1228.3** ✓（基线 1303–1306 / 1226–1228 ✓）| ✅ 同档 ✓ |
| `stale` ✓ | 0 ✓ | **0** ✓ | ✅ |

★ **读数** ✓：`busy` mean **488.9** ✓（基线簇 492.0–494.3 ✓）、p95 **624.0** ✓（~627 ✓）、p99 **687.8** ✓（~699 ✓）
⇒ ★ **三个分位数一致改善** ✓，**不是只动均值** ✓。
★ **与 `mkf023` 的自洽** ✓：那条腿（同一个臂、那时还是旋钮 ✓）测得 `ch_wt` **36.3–37.3 → 30.4** ✓、
`busy` mean **−7…−9 µs** ✓ ⇒ ★ **本条腿在"默认形态"下复现了它** ✓（**−5.9 / −6.8** ✓）✓。

★ **记账** ✓：`merged_hop` **458.0 µs** ✓（基线簇 455.9–457.3 ✓）⇒ **合并只动了 `ch_wt` 那一段** ✓，
**没有动 lane burst** ✓ —— ★ 与"省的是相关那条 CB 里的派发"一致 ✓。

⇒ ★ **CE 整合的第一个真融合落地** ✓：**零算术风险** ✓（不碰归约/不改累加顺序/不跨数学模式 ✓）、
**逐位一致** ✓（27 capture × 4 dump ✓）、**空口 −5.9 µs** ✓、**红线同档** ✓。
★ **下一个 = RANK 2** ✓（`pilots_cfo` + `pilots_apply_cfo` 合一 ✓，省 1 派发 ✓，同文件同快数学 ✓，归约顺序不变 ✓）。

### 2026-10-07 · ★★ mkf024-split（**B 路结构变体：把 lane 的 CB 切成两条**）：**跨 CB 边界在空口上测不出来** ✗ —— **第五个假设证否** ✓✓

**臂** ✓：`OCUDU_LANE_SPLIT_CB=1` ✓（挂在 `count_dispatch()` 上 ✓、第一半刻意**不编 fence** ✓、
理由见 §2.17 ✓）。**臂自己证明它响了** ✓：切分计数 **108 343** ✓（= lane 数 ✓ ⇒ **每跳都切** ✓）、
`busy split … merged_hop cbs/lane=2.00` ✓（基线 1.00 ✓）、`cbs/lane` 总计 **3.00** ✓（基线 1.00–2.00 ✓）。
**数值正确性** ✓：`q16c` 与 `q64c` 各 **27 capture × 4 dump = 0/0/0/0** ✓。

| 读数 | 基线簇（`mkf018/020/021` ✓）| **mkf024-split** ✓ | 差 |
|---|---|---|---|
| `busy` median ✓ | 472.6 / 473.5 / 472.8 | **472.8** | **±0.2 µs** ✓ |
| `busy` mean / p95 / p99 ✓ | 492.0–494.3 / ~627 / ~699 | **496.7 / 630.4 / 697.6** | +3～+5 / = / = ✓ |
| `merged_hop` ✓ | 455.9 / 457.0 / 455.9 | **457.3** | +0.3～+1.4 ✓ |
| `ul_pipeline` median ✓ | 1303.0–1306.0 | **1308.0** | +2～+5 ✓ |
| `ul_gpu_pipeline` median ✓ | 1226.2–1227.8 | **1231.5** | +3.7～+5.3 ✓ |
| ★ `start -> end` median ✓ | 221–223 ✓（均值 246–247 ✓）| **39.9**（均值 165.0 ✓）| ✗ **换义（采样变了 ✓），不可直读** ✗ |

**⇒ ★ 结果** ✓：**加一整条 CB 边界（一次 commit ＋ 一次 endEncoding ＋ 一次设备侧缓冲切换 ＋
一次库登记 ✓，采样数因此翻倍：`burst commits` 216 686 = 2 × 108 343 ✓），代价在 ±1 µs 噪声底内测不出来** ✗
⇒ **那 ~400 µs 不是 CB 边界吃掉的** ✓ ⇒ ★ **第五个假设证否** ✗、
**能想到的结构到此全部量过** ✓ ⇒ **继续归因边际收益 = 0** ✗。

**★ 一个读数陷阱 ✓（本条的第一版写错了 ✗，核对五条腿才改对 ✓）**：`start -> end` 的中位从基线簇的
**221.4 / 223.2 µs** 掉到 **39.9 µs** ✓，但**不可直读** ✗ —— **切分改变了采样** ✓
（基线每跳**一条** CB、切分后**两条** ✓，样本 216 801 → 325 032 ✓），而两侧 **p95/p99 几乎不动** ✓
（561.6/619.5 → 547.4/605.5 ✓）⇒ 中位下落是**"一条大 CB 变两条小 CB"的采样后果** ✓，不是设备变快 ✓。
★ 这个现象**已被记录过** ✓：`mkf023` 那条 memo 已写下"设备 median 221 → 366 ✗ 是**分布形状变化**
（与 `mkf021` 的 373 同一现象 ✓）—— **记下不解释** ✓、**引用只用均值** ✓" ✓；本条补上**机制** ✓
（形状变化 = **CB 条数/每 CB 工作量**变了 ✓）与**第二个实例** ✓。
★ **口径修正** ✓：**均值**在基线上是 **246–247 µs** ✓，两端相加与 `busy` 自洽 ✓；
但**单条 CB 的窗口含等待** ✓ ⇒ 把它当"内核算了多久"会把 §①E 那条"**中位 CB 86 % 在设备**"读歪 ✗
⇒ **那个分解口径需重读** ✗。**五个证否不依赖这个口径** ✓（都是独立结构对照 ✓）⇒ **结论不动** ✓。

**★ 教训（第四条方法论 ✓）**：**引用一条读数前先核对它在哪条腿上** ✗ —— 第一版把基线写成 **366 µs** ✗，
那是**并发 1 的 `mkf022`** ✓、不属于基线簇 ✓。★ 与同一轮在并发 1 上抓到的**是同一类错误** ✗
（**一天之内第二次** ✓）⇒ 已并入判读清单：**每个基线数字都要能指回腿号** ✓。

**红线** ✓：`stale=0` ✓、**`gaps=0`** ✓、`rx_overflows=0` ✓、停顿 profile 与簇同级 ✓
（`phy_series=12` ✓ vs `mkf021` 11 ✓）。CRC **95.6 % marginal** ✗ 但**同轮 `mkf023` 是 93.1 % DEGRADED** ✓
＋**离线 0 差异** ✓ ⇒ **环境/流量，与臂无关** ✓。

### 2026-10-07 · ★★ B′（**未飞腿 ✓**）：`nv` 的 A/B **不值得飞** ✗；★ 并发 1 的"长尾杠杆"**是假的** ✗✗（对照臂拿错了 ✓）

用户裁决走 **B′** ✓（只做**已验证可行性**的小额收益实验 ✓）。两项**都没飞腿** ✓ ——
在"读现成数据 + 补分母"这一步就判完了 ✓。细节见 **§2.16** ✓。

**① `nv` 输出（省 4 B/RE ✓）：收益上界 ~1 µs ⇒ 不飞 ✗**

| 量 | 值 | 出处 |
|---|---|---|
| 融合**总**收益 | **−7.4 / −7.6 µs/跳** | `mkf012/013`、`mkf014/015` ✓ |
| 对应的中间流量变化 | 净 **−16 B/RE**（去 eq 写+读、nv 读 ✓，加 nv 写 ✓）| kernel 注释 ✓ |
| ⇒ `nv` 的份额 | **4/16 = 25 %** ⇒ **≤ 1.9 µs** | 归谬 ✓ |
| ★ 用 dispatch 价再算一次 | 7.6 µs 里大头是那次 dispatch 消除 ✓（6.5 µs ✓，`mkf023` ✓）⇒ 16 B/RE 流量只剩 ~1 µs ⇒ **`nv` 只值 ~0.3 µs** | `mkf023` ✓ |

⇒ **两种算法都在噪声底（±1 µs）之下** ✓ ⇒ 不为它飞一对腿 ✗（handoff §④ 那条按此**关闭** ✓）。

**② ★★ 更正：并发 1 全面更差 ✗**（原记录"p95/p99 −23～24 %"✗ —— 那是**拿并发 2 的腿当对照** ✓）

| 腿 | 自报并发 | `ul_to_lane` p50 | `busy` p50 | `ul_gpu_pipeline` p50/p99 | `ul_pipeline` p50 | 超 2000 µs |
|---|---|---|---|---|---|---|
| `mkf018` / `mkf020` / `mkf021` ✓ | 2 / 2 / **2** ✗ | 22 µs | 472.6 / 473.5 / 472.8 | 1227 / 1526 | ~1304 | 35 / 39 / 27 |
| **`mkf022`** ✓ | **1** ✓ | **351 µs** ✗ | **433.1 µs** ✓ | **1455 / 1787** ✗ | **1534** ✗ | **318** ✗ |

* ✗ **p50 +18.5 %、p99 +17 %** ✓（方向与原记录**相反** ✗）、**超预算窗口 ×10** ✗；
* ✓ 唯一真变好：**设备侧 `busy` −8.4 %** ✓（这解释了原记录看到的"窗口 −10 %" ✓）；
* ★ **机制** ✓：`max_pusch_and_srs_concurrency <= 1` 是 **STRAND**（"ONE PUSCH hop at a time" ✗，
  `du_low_executor_mapper.cpp:227` ✓）⇒ 车道串行 ⇒ 线程停摆 ✓（`exec.park` 1→5 ✓、`uninstrumented` 7→94 ✓）
  ⇒ `ul_to_lane` **+329 µs** ✓ = `ul_pipeline` 那 **+228 µs** 的来源 ✓。
* **⇒ 并发 1 不进交付配置** ✗、**不再飞腿** ✓。

★ **教训（第三条方法论 ✓）**：**"旋钮没进进程"与"没效果"在日志里一样** ✓ ——
这一条此前是用在**臂**上的 ✓，这次证明它**同样要用在对照臂上** ✗：
`mkf021` 命令行写着并发 1 ✓、自报行是 **2** ✗ ⇒ 一个**没生效的臂被当成对照** ✓，
于是"更差"被读成了"更好" ✗。**判读前逐腿读自报行** ✓，不是只读 `gNB options` ✓。

### 2026-10-07 · ★★★ mkf022（**并发 1**）：**不是"与并发 lane 争设备"** ✗ —— 但并发是**长尾杠杆** ✓

> ⚠ ★★ **本条的后半句已被推翻** ✗✗（2026-10-07，**B′ 判读** ✓，见 §2.16 与上一条 memo ✓）：
> 本条的对照臂 `mkf021` **自报并发 = 2** ✓ ⇒ **臂与对照都是并发 2 的邻居** ✗，
> 那个"长尾更好"是**方向反了** ✗：改正后 **p50 +18.5 %、p99 +17 %、超预算窗口 ×10** ✗。
> **"不是争设备"这半句仍然成立** ✓（`busy` 确实降了 ✓），但**"长尾杠杆"这半句作废** ✗
> —— ★ **并发 1 不进交付配置** ✗。下表那些"−24 %"的差值**不要引用** ✗。

**旋钮确认生效** ✓（这次先读它 ✓）：`gNB options` 有它 ✓、
`[ul_lane_exec] PUSCH/SRS concurrency = 1 … a serialising STRAND: ONE PUSCH hop at a time` ✓。

| 读数 | 并发 2（4 腿簇 ✓）| **并发 1（mkf022 ✓）** | 差 |
|---|---|---|---|
| `busy` mean ✓ | 492.0–494.3 | **445.4** | **−48 µs（−10 %）** |
| `busy` median ✓ | 472.3–473.6 | **433.2** | **−40 µs（−8.4 %）** |
| `busy` p95 ✓ | ~627 | **474.0** | **−153（−24 %）** |
| `busy` p99 ✓ | ~699 | **541.3** | **−158（−23 %）** |
| `merged_hop` ✓ | 455.5–457.0 | **411.9** | **−44 µs（−9.7 %）** |
| Q9-B 队列 mean ✓ | 102.6–103.8 | **124.2** | **+21（↑）** |
| Q9-B 设备 mean ✓ | 246.0–247.1 | **222.7** | −24 |
| Q9-B 设备 median ✗ | 221–223（021: 373）| **366.0** | 分布形状变了 ✗ |
| `cbs/lane` ✓ | 2.00 | 2.00 | = |
| `stale` ✓ | 0 | 0 | ✓ |

**⇒ 结论** ✓：
1. ★ **那 ~456 µs 的主体不是"与并发那条 lane 争设备"** ✗ —— 并发减半，窗口只掉 **~10 %** ✓
   （若是争用，应当接近减半 ✗）；
2. ★ **但并发 1 是真正的"长尾杠杆"** ✓：p95/p99 **−23～24 %** ✓（设备被独占 ⇒ 不再偶尔交错 ✓）；
   ⇒ 记为一个**可用的调优选项** ✓（代价 = 一次只跑一条 hop ⇒ 吞吐 ✓，不是本次的目标 ✓）；
3. 队列 mean **升**（103 → 124 ✓）而设备 mean **降**（246 → 223 ✓）：
   串行化之后"排队久一点、执行干净一点" ✓，净额 −48 µs ✓；
   设备**中位数** 221 → **366** ✗ 是分布形状变化 ✓（与 mkf021 的 373 同一现象 ✓）——
   **记下不解释** ✓，引用时只用**均值** ✓（均值是可加的 ✓：124.2 + 222.7 = 346.9 = commit→completion mean ✓ 精确闭合 ✓）。

**⇒ 归因现在的形状** ✓：`merged_hop` ≈ **412–456 µs**，一跳里 **~6.9 个 dispatch** ✓（`ce_sites` ✓），
而它**不是**：内核 ✗（M0 ✓）、fence ✗（mkf019 ✓）、兄弟 lane 争用主体 ✗（mkf022 ✓）。
⇒ **剩下的候选 = "这条 CB 自己那 ~7 个 dispatch 的空口价格"** ✓。
★ **记录里的一条旁证** ✓（前一条工作流的同一现象 ✓）：仓里 `OCUDU_CE_CORR_MERGED`（把两个 corr dispatch 合成一个 ✓）
自己的注释写着"**这两个边界在空口上每个 ~39 µs，而离线只要 ~1.6 µs**" ✓ ——
与我这次量到的"**空口 66 µs/dispatch vs 离线 3.7 µs**"是**同一个 20～25 倍比** ✓✓
⇒ "空口的 dispatch 比离线贵 20 倍"是**本仓已经出现过两次的现象** ✓，不是新谜 ✓。
⚠ **但价格不均匀** ✓：本次融合**拿掉了一个 dispatch** 只换来 **7～13 µs** ✓
（而不是 40 µs ✗）⇒ "窗口 = dispatch 数 × 单价"是**过度简化** ✗，每个边界的价钱要单独量 ✓。

**⇒ 下一个最便宜的测** ✓（一个 env 旋钮 ✓，对着上面那个 ±1 µs 的 4 腿基线比 ✓）：
`OCUDU_CE_CORR_MERGED=1` ✓ ——（a）先看 `ce_sites` 的 `merged=` **是否真的动** ✓
（前一条工作流的 p146/p147 两条腿里 corr_a/corr_rhp **都没减少** ✗ ⇒ **这个旋钮在那条路线上可能压根没生效** ✗，
所以**必须先读它** ✓）；（b）若真动 ⇒ 直接读出"**空口上一个 dispatch 值多少**" ✓，并可能**顺手削掉 ~40 µs** ✓。
★ 顺带提醒 ✓：并发 1 那 −24 % 的长尾值得单独记一笔 ✓ —— 它是**今天所有单变量试验里对 p99 最有效的一个** ✓。

### 2026-10-07 · ★★★ mkf020/mkf021：**旋钮没进进程 ✗ ⇒ 这一对变成了"同配置重复腿"** ✓ —— 却给出了**噪声底** ✓✓

**① 我的命令错了 ✗**（不是代码问题 ✓）：我让用户把
`--expert_execution.max_pusch_and_srs_concurrency=1` 作为**第 5 个位置参数**传给 `fly_leg.sh` ✗，
而 `fly_leg.sh` **只取 4 个位置参数** ✓（`MODE/LABEL/LOAD/PROFILE` ✓），**多余的一律丢弃** ✗。
两条腿的 `gNB options` 都只有 `--expert_execution.threads.lower_phy.execution_profile=dual` ✓、
`[ul_lane_exec] PUSCH/SRS concurrency` 都是 **2** ✓ ⇒ **`mkf021` 并没有跑在并发 1 上** ✗。
★ **正确写法** ✓：经 `fly_leg.sh` 传 gNB 选项要走 **`EXTRA_KNOBS`** ✓ ——
它把内容并进 `KNOBS` ✓，而 `run_leg.sh` 会**按前缀分流** ✓（`OCUDU_*=` → 环境 ✓、`--…` → argv ✓、其它 → 拒绝 ✓），
这正是 `mkf008/009` 当初传 `--expert_phy.pusch_dft_type=cpu` 的方式 ✓。
**教训** ✓（腿协议自己第一段就写着这条 ✓）：**"旋钮没进进程"与"旋钮没效果"在日志里长得一样** ✗
—— 只有 `gNB options` 那一行能分辨 ✓，所以**每次都要读它** ✓。

**② 白捡的收获：四条同配置腿的重复性** ✓✓（钉住 MCS、并发 2、融合开 ✓）：

| 腿 | `busy` mean / median | `merged_hop` | Q9-B 队列 mean | Q9-B 设备 mean | CRC steady |
|---|---|---|---|---|---|
| mkf018 ✓ | 492.2 / 472.6 | 455.9 | 103.8 | 246.1 | 99.5 % ✓ |
| mkf019（fence 臂 ✓）| 492.0 / 472.3 | 455.5 | 103.5 | 246.0 | 99.6 % ✓ |
| **mkf020** ✓ | **494.3 / 473.6** | **457.0** | **103.4** | **247.1** | 99.3 % ✓ |
| **mkf021** ✓ | **492.4 / 472.8** | **455.9** | **102.6** | **246.2** | 99.8 % ✓ |
| 散布 ✓ | **±1.2 / ±0.7 µs** | **±0.8 µs** | **±0.6 µs** | **±0.6 µs** | 99.3–99.8 % |

⇒ ★★ **lane 的窗口读数重复到 ~±1 µs** ✓✓ —— 这一条反过来给前面每个判决**加了底气** ✓：
* 融合的收益（`busy` **−7.4 / −7.6 µs** ✓、`merged_hop` **−11.8 / −12.8 µs** ✓）是**噪声底的 6～10 倍** ✓ ⇒ 真实 ✓；
* fence 臂的"什么都不动"（**−0.2 / −0.4 µs** ✓）**落在噪声底之内** ✓ ⇒ 那条结论**成立且干净** ✓；
* **`merged_hop ≈ 456 µs` 是这个配置的稳定结构常数** ✓（四条腿 455.5–457.0 ✓）——
  **不是噪声、不是 fence、不是内核** ✓ ⇒ 谜团是一个**稳定的路径属性** ✓，值得继续追 ✓。
* 唯一的异常 ✓：mkf021 的**设备中位数 373.4 µs** ✓（其余三条 221–223 ✓），而它的**均值 246.2 正常** ✓
  ⇒ 该腿设备分布的形状变了（双峰？✓）⇒ **记下** ✓，引用"设备中位数"时避开这一条 ✓。

**③ 第二处我自己的错 ✗：选项路径少了一层** ✓（★ **同样是"被响亮的拒绝"接住的** ✓，
没有浪费电台 ✓ —— `run_leg.sh` 的 **dry run** 在起飞前就把它挡下了 ✓）：
先写 `--expert_execution.max_pusch_and_srs_concurrency` ✗ ⇒ gNB 报
`The following argument was not expected` ✓。**正确的路径是三层** ✓：

```
--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=1
      └──────┬──────┘ └──┬──┘ └──┬───┘ └──────────────┬──────────────┘
        expert_execution  threads  upper_phy        选项本体
```
（出处 ✓：`du_low_config_cli11_schema.cpp` 里 `configure_cli11_upper_phy_threads_args()` 挂在
`threads → upper_phy` 之下 ✓；**查法** ✓：`./build/apps/gnb/gnb --help expert_execution.threads.upper_phy` ✓，
比猜快 ✓。另：**配置里本来就写着 `max_pusch_and_srs_concurrency: 2`** ✓ ——
所以 `[ul_lane_exec]` 打的是 "configured" 而不是 "auto-derived" ✓，而 **CLI 覆盖 YAML** ✓。）

**离线自验** ✓（dry run + 真起一次 ✓，都不用电台 ✓）：
`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=1` ⇒ dry run **exit 0** ✓、
实起时打印 `[ul_lane_exec] PUSCH/SRS concurrency = 1 … a serialising STRAND: ONE PUSCH hop at a time` ✓✓
（对照：两层那个路径 dry run **exit 109 / not expected** ✓）。

**③ 下一步** ✓（命令已按 ① 与本节改正 ✓）：并发 1 那条腿**还没飞** ✓ ——

```bash
cd /Users/jiachengwang/dev/ocudu
export LEG_CFG=$PWD/doc_chinese/macos_thread_priority/wip/gnb_pinned_mcs13.yml
export LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs
# ★ gNB 选项走 EXTRA_KNOBS（run_leg.sh 按前缀分流到 argv），不要当位置参数 ✗
# ★ 路径是三层的：expert_execution.threads.upper_phy.<option>（少一层会被 dry run 拒绝 ✓）
EXTRA_KNOBS="--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=1" \
sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf022-m1-conc1 dual quiet gpu
```
（`mkf022` 这个号**还没被占用** ✓ —— 被拒的那次没有产生任何日志文件 ✓。）
**判读** ✓：先看 `gNB options` 那行**有没有它** ✓、再看 `[ul_lane_exec] PUSCH/SRS concurrency = 1` ✓
（**这一步不能省** ✗ —— 本轮就是没读它 ✗）；然后比 `merged_hop`（456 µs ✓）：
**减半 ⇒ 与并发 lane 争设备** ✓、**不动 ⇒ 排除争用** ✓，转查"一条 CB 里多个小 dispatch 的串行化" ✓。

### 2026-10-07 · ★★★ mkf018/mkf019（**fence 消去，空口**）：**那 ~456 µs 不是 fence** ✗ —— 归因进入下一层 ✓

**腿** ✓：`mkf018`（正常 ✓）与 `mkf019`（`OCUDU_LANE_ABLATE_FENCE=1` ✓），钉住 MCS 的 config ✓、背靠背 ✓。

**① 臂真的生效了 ✓（但要靠另一个读数才看得出 ✗）**：
`[metal_stats] lane fence … own=` 从 **1 → 0** ✓（正常腿有 1 次"指名自己那一代" ✓，臂上 0 ✓），
而 `waits=108 400 / 108 428`（**1/跳** ✓）**照常** ✓ —— 说明这条腿走的是
**"等全局最新"的回退分支** ✓（`backend_stage_wait()` ✓），不是 `s.stage_wait` 那条 ✓。
★ **我的计数器写错了** ✗：它只在 `stage_wait/grid_wait` **有待处理值**时才 +1 ✓ ⇒ 空口打出 **`fences skipped: 1`** ✗，
而实际**每跳都跳过了一次等待** ✓。**已修** ✓（改成**每条 CB 记一次** ✓，离线 `--repeat 4` 复验 **4** ✓）。
**这条缺陷本身就是本工作流反复记录的那类** ✓：**计数少报 ⇒ 读成"旋钮没生效"** ✗，
而真相是"**它去掉的东西不花钱**" ✓ —— 两者的结论完全相反 ✓。

**② 空口读数：拿掉那道等待，什么都不动 ✗**（这是本轮的真结论 ✓）：

| 读数 | mkf018（正常 ✓）| mkf019（无 fence ✓）|
|---|---|---|
| `busy` mean / median ✓ | 492.2 / **472.6** | 492.0 / **472.3** |
| `busy split merged_hop` ✓ | **455.9** | **455.5** |
| `busy split ch_wt` ✓ | 36.3 | 36.5 |
| Q9-B 队列 mean ✓ | 103.8 | 103.5 |
| Q9-B 设备 mean ✓ | 246.1 | 246.0 |
| Q9-B 设备 median ✓ | 221.4 | 210.6 |
| CRC steady ✓ | **99.5 % CLEAN** ✓ | **99.6 % CLEAN** ✓ |

⇒ **每一读数都在噪声内** ✓ ⇒ ★ **那 ~456 µs 的 `merged_hop` 窗口不是这道等待** ✗✓。
⇒ **CRC 也没有塌** ✓（99.5 → 99.6 ✓）—— 这本身是第二条信息 ✓：
**这道等待在本路线上的顺序本来就被别的机制保住了** ✓（估计器的 CB 很短、且提交在前 ✓，
lane 的 dispatch 真正跑起来时它早就写完了 ✓），所以 fence **是保险，不是成本** ✓。

**③ 离线那 32 % 为什么不迁移 ✓**：离线跑的是**另一条路线** ✓ ——
那里 `s.stage_wait` 是**真的被指派**的（`own` 那条 ✓）、窗口也只有 ~300 µs ✓；
空口走的是回退 ✓、窗口 456 µs ✓ ⇒ **同名不同物** ✗。
**教训** ✓：**离线工具能测"这条等待值多少"，但测不出"空口用的是哪条等待"** ✗
—— 那个问题只能由**空口上的 self-report**（`own=` ✓）回答 ✓；**先看它，再信数** ✓。

**④ 归因进入下一层 ✓ —— 剩下的候选（按可能性排 ✓）**：
一跳的 `merged_hop` buffer 里**不是 3 个 dispatch** ✗ —— `[metal_stats] ce_sites` 说
**reformat 1/跳 + pilots_lse 1 + pilots_cfo 1 + corr_a 1.45 + corr_rhp 1.45 ≈ 5.9 个估计器 dispatch/跳** ✓
＋ 融合 eq/demap 1 个 ≈ **6.9 个/跳** ✓，而窗口是 **456 µs** ✓
⇒ 即 **≈66 µs/dispatch** ✗ —— 而离线微基准量到的是 **3.7 µs/dispatch** ✓（`wip/dispatch_cost_probe/` ✓）
⇒ **空口比离线大 ~17 倍** ✗ ⇒ 这是一个**只在空口出现**的现象 ✓（与 M0 那 ~380 µs 同一个谜 ✓）。
候选 ✓：(a) **与并发那条 lane 争设备** ✓（本配置 `PUSCH/SRS concurrency = 2` ✓）；
(b) 设备侧对"一条 CB 里多个小 dispatch"的串行化 ✓；(c) CB 的**完成路径**开销 ✓。
★ **下一个最便宜的测** ✓：**把并发变成 1** ✓ —— ★★ **经 `EXTRA_KNOBS` 传** ✓
（`EXTRA_KNOBS="--expert_execution.max_pusch_and_srs_concurrency=1"` ✓；**当位置参数传会被 `fly_leg.sh` 丢掉** ✗，
`mkf021` 就这么白飞了一条 ✗）—— 判读**先读 `gNB options` 与 `[ul_lane_exec]`** ✓，再比 `merged_hop` ✓：
**减半 ⇒ 争设备** ✓；不动 ⇒ 排除 ✓。
★ **噪声底已测** ✓：**四条同配置腿的窗口读数重复到 ±1 µs** ✓（memo ✓）⇒
`merged_hop ≈ 456 µs` 是**稳定常数** ✓、而前面那些 −7～−13 µs 的判决是它的 6～10 倍 ✓ ⇒ 可信 ✓。

### 2026-10-07 · ★★★ mkf016/mkf017（**AMC 一对**）：**覆盖率与逐跳路由完全成立 ✓✓**；性能/功能**判不了** ✗

**这一对是为回答"AMC 下融合还成立吗"而飞的** ✓（`wip/gnb_amc.yml` ✓ = 钉住版去掉 `min/max_ue_mcs` ✓，
**两条腿都带它** ✓ ⇒ 变量仍只有 `OCUDU_LANE_FUSE_EQDEMOD` ✓，对照腿显式 `=0` ✓）。

**① 覆盖率：精确闭合 ✓✓**（这是本轮最硬的读数 ✓）：

| 腿 | 调制分布 | `y_fused` / `y_batch` |
|---|---|---|
| mkf016（对照 ✓）| 64QAM 54 946 / 256QAM 26 155 / 16QAM 19 868 / QPSK 9 179 ✓ | 0 / 110 148 ✓（全两步 ✓）|
| mkf017（臂 ✓）| 64QAM 64 631 / 16QAM 30 540 / QPSK 8 378 / **256QAM 5 082** ✓ | **103 549** / **5 082** ✓ |

而 **64 631 + 30 540 + 8 378 = 103 549** ✓✓、**5 082 = 5 082** ✓✓ ——
⇒ **三种调制（含新补的 QPSK ✓）的跳全部融合** ✓、**只有 256QAM 退回两步** ✓
⇒ **AMC 下的逐跳路由按设计工作** ✓✓，**覆盖率 103 549 / 108 631 = 95.32 %** ✓。
**新代码被腿覆盖** ✓：QPSK 分支在空口上跑了 **8 378 跳** ✓（这正是补它的目的 ✓）。
结构 ✓：`dispatches`/跳 **4.0000 → 3.0471** ✓（只有 256QAM 那 4.68 % 付第二次 dispatch ✓）；
臂的 demapper dispatch = **5 082 = 256QAM 跳数** ✓（账目闭合 ✓）。

**② 但它判不了性能与功能** ✗（三条独立的理由 ✓，每条都够 ✓）：
1. **两条腿的信道不同** ✓ —— 这正是 AMC 在做的事 ✓：对照 256QAM 占 **23.7 %**、臂只占 **4.7 %** ✓；
   平均 **Qm 5.78 vs 5.22 bit/RE-跳** ✓（臂的 LLR 总量少 **~11 %** ✓）⇒ **负载不是同一个** ✗
   ⇒ `[ul_pipeline]` 那 −32 µs 里**混着负载差** ✗（设备侧 `busy` 仍可比 ✓，见下 ✓）；
2. **两条腿 CRC 都 DEGRADED** ✓：**95.0 %（对照）/ 90.0 %（臂）** ✗
   —— 距"CRC 同档"的红线很远 ✗（钉住那两对是 99～100 % ✓）；AMC 本来就把 MCS 顶到失败边缘 ✓
   ⇒ **功能判据无从谈起** ✗（90 % 那条腿也不能拿来比 ✗）；
3. `pair_check` **FAILS 它的带宽比前提** ✓：B/hop **2608 vs 1897 = −27 %** ✗（要求 ±10 % ✓）；
   `dl_gate` 两条腿都是 STALL ✓，且**对照腿有一次 59.5 ms 的停顿** ✓（`wd late 59503.0` ✗）
   ⇒ **时延数字不可引用** ✗。
⇒ **结论** ✓：**AMC 对 = 覆盖率/路由的证据 ✓，不是性能/功能的证据 ✗**；
   性能证据仍是**钉住 MCS 的那两对** ✓ —— 而这也正是上一工作流把 MCS 钉住的理由 ✓
   （config 原话：钉住是"a size-matching device" ✓，现在有了本轮的反面实证 ✓✓）。

**③ 报告制读数（方向一致，但带上面三条保留 ✓）**：`busy` median **478.7 → 471.1 µs**（−1.6 % ✓，
与钉住两对的 −1.5～−1.9 % 同量级 ✓，且**设备侧口径不受负载差影响** ✓）、`merged_hop` 464.3 → **454.3** ✓、
`stale` 对照 **10**（ul_gpu）→ 臂 **0** ✓。

**④ ★★ 归属读数（本轮新加的两条序列 ✓）—— 我上一轮的假设被推翻 ✗✓**：

| 读数（每条 CB）| mkf016 | mkf017 |
|---|---|---|
| commit→completion median ✓ | 392.8 µs | 386.3 µs |
| **commit→start（队列）median** ✓ | **34.2 µs** | **34.0 µs** |
| **start→end（设备）median** ✓ | **209.1 µs** | **206.4 µs** |
| 队列 mean / p99 / max ✓ | 103.4 / 338.7 / 2684.2 | 103.3 / 336.7 / 2688.1 |
| 设备 mean / p99 / max ✓ | 250.5 / 632.7 / 1420.8 | 245.9 / 615.2 / 1376.6 |

⇒ **中位 CB 只有 14 % 的寿命在排队 ✓，86 % 在设备上** ✓ —— 我上一轮据"最慢 8 个"推测的
"主体是排队" ✗ **是错的** ✓（那张表只印最慢的 ✓，最慢的那些确实是排队主导 ✓ ⇒ 排队是**长尾** ✓：
mean 103 µs vs median 34 µs ✓、p99 339 µs ✓、max 2.7 ms ✓）。
⇒ **~380 µs 的正确形状** ✓：**设备"持有"这条 CB 209 µs，而里面的内核合起来只有几微秒** ✓
（M0 消去法 ✓、M0b 的"event/burst 的 busy 无法区分" ✓）⇒ **它不是排队，也不是内核算术** ✓。
**下一步该问的是** ✓：这 209 µs 里设备在等什么 —— 最可能是 **CB 内的 fence 等待** ✓
（burst 会编码"等估计器的信号" ✓）或**小 dispatch 的设备侧串行化** ✓；
**验证手段** ✓：给 lane 探针加一条"**fence 消去/替换**"的臂 ✓（消去内核本体的 M0 臂**不动 fence** ✗，
所以那三条腿**没有**回答这个问题 ✓）—— 与 `OCUDU_LANE_ABLATE` 同形状 ✓、只测窗口 ✓、离线可先验 ✓。

**★ 对 §2.14 的更正** ✓：那里把 M2 的前提写成"去掉 CE↔lane 边界能不能动那 ~380 µs" ✓ ——
**方向仍然对** ✓（边界是 CB + fence ✓），但要补一句：**M0b 只证明了"同一个 stage 划分下 event/burst 的 busy 无法区分" ✓，
没有证明 fence 不花钱** ✗（那个旋钮**根本没把 CB 数分开** ✓：两条腿都是 3.00 ✓）。
所以先做 ④ 的那条**归属臂** ✓，再决定 M2 ✓。

### 2026-10-07 · ★ QPSK 补进融合 kernel：三种调制的覆盖率补齐 ✓（离线三份语料全 0/0/0/0 ✓）

**用户裁决** ✓：先补 QPSK ✓，再飞 AMC ✓。**做了什么** ✓：kernel 加 `MOD_QPSK` 分支 ✓
（照抄 demapper ✓，★ **range limit 24 ✓ 不是 20** ✓）、引擎/后端的 bits-per-RE 与谓词各放一支 ✓、
单测改成三调制各跑一遍 ✓。**调用方一行没改** ✓（逐跳路由本来就是它做的 ✓）。

**验证** ✓：`metal -c` exit 0 ✓；单测三支**逐位一致** ✓；**离线对拍三份语料 × 27 capture = 0/0/0/0** ✓
—— 其中 **QPSK 那份是真正的原始语料** ✓（前两轮要"标注"是因为当时只做 QAM ✓）。
**顺手记一个坑** ✓：路线默认开之后，`ab_dumps.sh` 的**对照臂必须写 `OCUDU_LANE_FUSE_EQDEMOD=0`** ✓；
第一次两边都不带 ⇒ **融合 vs 融合** ✗，工具**不报错** ✓，只有读 `y_fused` 才看得出来 ✗。

**⇒ 下一步** ✓：飞 **`mkf016`/`mkf017`（AMC 一对 ✓）**，读调制直方图 + `y_fused`/`y_batch` 覆盖率拆分 + CRC ✓。

**★★ M3 漏掉的一处（用户一飞就撞上 ✓，2026-10-07）**：`run_leg.sh` 有一道**起飞前闸门** ✓
（"a gpu leg without its DEVICE KERNELS is not a gpu leg" ✓）—— 它**按名字查那三个库** ✗
（`ocudu_mmse/equalizer/demod.metallib` ✓），理由写得很对 ✓（缺库时引擎会**静默**走 host 路径 ✗，
腿却仍自称 gpu ✗）。M3 一合并、旧文件一删 ⇒ **每一条 gpu 腿都被拒绝** ✗
（实测：`REFUSING to run a gpu leg: 3 device kernel(s) missing …` ✓）。
**修法** ✓：闸门改成查**两个**文件 —— `lib/phy/generic_functions/metal/ocudu_dft.metallib` ✓ +
**`lib/phy/metal/ocudu_lane.metallib`** ✓（"一条腿一个 stage 的库"这个不变式 ✓）。
**同一轮扫出的其它活引用** ✓（历史文档**不动** ✓ —— 那是记录 ✓）：
`ab_replay_bins.sh` 的 metallib 配对变量 ✓、`build_macOS_note.md` 的单目标与 `ls` ✓、
`phy_latency/wip/ce_kernel_cost.mm` 的三处加载路径 ✓、
`phy_pipeline_gpu` 设计文档里引用的"补建命令" ✓ —— 全部指向合并后的库 ✓。
**教训** ✓（与 §4 那条同源 ✓）：**M3 改的是"库这个交付物的边界"** ✓ ⇒
凡是**按名字提到那三个文件**的地方（runner 闸门、工具、文档命令）都要跟着改 ✓；
只改引擎与 CMake **不够** ✗ —— 而**闸门**正是那个"改了才不会静默降级"的地方 ✓。

## 6. 会话交接（**只在准备开新会话时**新建 ✗ 不是每段工作结束时）

★ **时机**（用户 2026-10-06 明确 ✓）：`session_handoff_*.md` **只用于新会话交接时的现状快照** ✓；
**同一条对话里继续干活时不要写它** ✗ —— 那一轮的结论、现状、下一步与未决项**全部写进本文** ✓
（实作与数字进 §2.x 与 memo ✓、下一步进 §2.11 这类小节 ✓、未决项进 §2.12 这类表 ✓）。
快照是**一次性**的 ✓：新会话开始后它会过时 ✓，所以它只承载"接手需要的最小充分集" ✓，不与活文档争内容 ✓。

命名与结构照抄上一工作流 ✓：`session_handoff_<日期>-<序号>.md`，内容包含：
① 本会话做了什么（引用 memo 条目 ✓）；② **当前状态**（代码/开关/腿号/判据 ✓）；
③ 下一个会话**第一件事**（具体命令 ✓）；④ 未决问题与它们的**重开条件** ✓。
