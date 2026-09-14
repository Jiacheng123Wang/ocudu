# Session Handoff Memo — 2026-09-15-1

> **性质**：状态快照，**冻结不改**。需要修正/更新时写进活文档
> `doc_chinese/full_gpu_chain/s2_full_chain_design.md`，不要回头改本文件。
>
> **代码基线**：`bc63c5329b`（HEAD = origin/apple-silicon = `gnb` 版本戳，三方一致）
> **活文档**：`doc_chinese/full_gpu_chain/s2_full_chain_design.md`，本会话写入 §48.73–§48.97；
> **横向状态视图在 §48.84（每改变模块落点/缓冲归属都要回来更新它）**

---

## 0. 一句话现状

**从"频域网格"到"LLR"，除 K1 之外全部在 GPU 上。K1 已接通到 GPU、算法已验证正确，
3 条验证抓包里 2 条正确，剩 1 条卡在"设备建矩阵 + 设备反演"这个组合上（该组合历史上从未存在过）。**

**默认路径（主机 K1）功能完全正确**：980/980 抓包逐字节一致、`ctest -L phy` 162/162。
**设备 K1 默认关闭**，用 `OCUDU_CE_GPU_INVERT=1` 开启。

---

## 1. 本会话的目标（用户历次定调，优先级最高）

1. **总体目标**：**全 GPU path** —— 把尽可能多的模块搬进 GPU，最终 IQ→LLR 全在 GPU。
2. **顺序**：**先功能正确，再性能优化**。局部性能退化**可以接受**，不作阻塞。
3. **纪律**：**精度问题不能成为把模块退回 CPU 的理由**（用户明确说过两次）。
4. **参考历史**：**"历史上 MMSE CE 全在 GPU，是正确的"** —— 修的时候要**参考历史代码逻辑**，
   不要埋头从当前状态硬修。（我这一轮违反了这条，见 §7。）
5. 曾明确撤销的选项：**"默认改回主机 gather"** 之类的回退 —— 与全 GPU path 冲突，不许提。

---

## 2. 当前代码落点（详见活文档 §48.84）

| 模块 | 在哪 |
|---|---|
| K0-d 相关矩阵 A/R_hp | **GPU（设备建）**，但**覆盖率仅 37.5% 的上机 hop**；且**设备建矩阵写不全 r_hp（见 §5.2）** |
| **K1 矩阵反演 A⁻¹** | **CPU（主机）** ← **本会话的主战场**；已接通 GPU 且验证 2/3 |
| K2a 权重 / K2b 应用 | GPU |
| K3 估计重排（cbf16） | GPU（引擎同一条 CB） |
| K4 噪声方差 | GPU |
| unpack `gpu_h`→`grid_est`、RSRP/TA 统计 | CPU |
| 等化 / 解调 / LDPC | GPU |
| 等化器读估计与噪声 | **设备直读 `gpu_ce`/`gpu_nv`** |

**关键开关**：
- `OCUDU_CE_GPU_INVERT=1` —— 设备反演（默认关，见 §5）
- `OCUDU_CE_CPU_INVERT=1` —— 强制主机反演（逃生口，**必须始终可用**）
- `OCUDU_CE_CORR_DEV=0` —— 关闭设备建矩阵（A/B 用）
- `OCUDU_CE_SPLIT_TAIL=1` —— 用 split 代替 merged 批次
- `OCUDU_CE_DEV_INVERT=1` —— 旧实验路径，**已知不可用，勿碰**
- `OCUDU_INV_RL=1` —— K1b 右看式反演，**数值错误，勿用**

---

## 3. 本会话的**净收益**（已提交、已验证）

| 提交 | 内容 | 效果 |
|---|---|---|
| `2b9cb520e1` | 设备相关矩阵等价性修复（K1 反演已反演的槽位等 2 个缺陷） | 默认路径稳定 |
| `97ba98b006` | 设备建矩阵真正生效 + **`device_corr_builds` 计数器** | 覆盖率可观测（37.5%）|
| `60dbdbe003` | **上机（OTA）验证通过**：`device_corr_builds=26761`、`Real-time failure in RF=0`、0 USB 错误 | 设备建矩阵在真实链路跑通 |
| `67b6377216` | 相关性内核获得 **system stride**（为 merged 铺路） | 零行为变化 |
| `524ebfd2db` | **合并分支两组用同一 `gpu_invert`**（+5 行） | **设备 K1 0/3 → 2/3** |
| `bc63c5329b` | 系统性历史对标（文档 §48.97） | — |

---

## 4. 已**实测确认**的事实（不要再重新验证）

### 4.1 K1 与它的输入
- **设备 K1 算法正确**：设备反演的 `w[0]=0.250750065`、`h[0]=0.648677528`；
  主机 `w[0]=0.250812382`、`h[0]=0.648476481` —— **差 2.5e-4，正是设备反演 7.67e-4 的 W 误差量级**。
- **精度不是障碍**：`A⁻¹` 的元素级误差（设备 9.7e-1 vs 主机 1.7e-1）**具有误导性**；
  正确的尺子是 **`W = R_hp·A⁻¹` 的误差**：设备 **7.67e-4**、主机 **1.40e-5**，
  折算到 SINR 仅 **≈0.003 dB**。
- **Metal 没有 `double`**（`xcrun metal` 直接报错）⇒ 设备反演永远是 float32，
  "用更高精度救 K1"**从语言层面不存在**。
- `mmse_inv` 内核的 `MAX_N = 54`（`threadgroup float gj[54][108]` = 22.8 KiB）；
  `72` 会让 pipeline 创建失败 ⇒ 设备反演上限就是 54。

### 4.2 同步与消费链（全部实测排除）
- `unpack_engine_group()` 入口 `has_pending()==0`，`h[0]` 正确 ⇒ **同步无问题**。
- `unpack` 的参数（`gb_start/n_blk/nout/st.L/st.nout/st.n_blk/sys_offset`）两侧**逐项相同**。
- **apply 跑满**：内核自报 735 个 threadgroup 写回、块标签 `0/100000/…/700000` 齐全（两侧相同）。
- **`gpu_apply` 的 `threadsPerThreadgroup` 必须 = `nout`(504)**：实测 `tpt=256` → 0.86 dB、
  `tpt=128` → 0.48 dB（静默丢尾部输出）。
- K3 的 dispatch 参数两侧相同（`nof_sub=288, threads=4032, dc_sc=138`）；
  K3 前**已有** `memoryBarrierWithScope:MTLBarrierScopeBuffers`；
  K3 的线程组宽度 64/128/256 **结果相同** ⇒ K3 无罪。

### 4.3 缓冲与对齐
- 页大小 **16384**；`gpu_a/r_hp/w/y/h` **全部 PAGE-OK**（`alloc_aligned` 实际给 16 KB 对齐）。
- `gpu_ce`/`gpu_nv` 走**整页**分配（`alloc_aligned_pages`），等化器**设备直读**，正常。
- DFT 与网格是零拷贝：网格用 `page_aligned_allocator`，CPU/GPU 同一块内存。
- **注意**：零拷贝 wrap 失败时 `wrap_buffer` 会**静默降级为拷贝**（`newBufferWithBytes`）——
  这类"写了但主机看不到"的现象要考虑它。

### 4.4 上机（OTA）实测
- `d8e23f67d1` 那条腿：`device_corr_builds=26761` / `hops_gpu=71384` ⇒ **覆盖率 37.5%**；
  `Real-time failure in RF = 0`、USB 错误 0、崩溃 0。
- 分段耗时**两个工作点差别很大**：离线 replay（宽分配）`pre=6–18µs`、`gpu_path=60–660µs`；
  **OTA（大量窄分配）`pre=0.69µs`、`gpu_path=99.4µs`**。
  ⇒ **"离线 980/980 一致"不能替代上机**，两者必须分别测。

---

## 5. 剩余的两个缺陷（**这就是本会话没做完的事**）

### 5.1 三个 `stage_engine_group()` 调用点的语义不一致（**历史结构被破坏**）
`grep -n "stage_engine_group(args,"` 得到三处，都在
`lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp`：

| 行 | 位置 | 末尾实参（`slots_filled`, `a_rhp_filled`） |
|---|---|---|
| **973** | 合并分支 else（标准组） | `gpu_invert, false`（只传 12 个实参，**`a_rhp_filled` 缺省=false**） |
| **1018** | 合并分支 tail | `gpu_invert, false, false` |
| **1832** | `run_engine_blocks()` | `gpu_invert, false, `**`dev_inv_now`** ← **设备反演时为 true** |

**历史（`a52677d194`，K1 在 GPU 能工作的版本）的结构是**：

```cpp
if (gpu_invert) { for(sys) memcpy(gpu_a + sys*L*L, w_r_pp, L*L*4); }   // 只有 A 由这个 if/else 决定
else            { for(sys) { 主机 Gauss-Jordan → 写 A^-1 } }
// R_hp 的写入：在 if/else 之外，无条件执行
// Y staging：无条件执行
```

**偏离**：现在 R_hp 与 Y 的写入被放进了 `stage_engine_group()`，并多了一道
**能跳过整个循环**的门（`!slots_filled && !a_rhp_filled`）。
**1832 传 `dev_inv_now` 导致设备反演时 R_hp/Y 都不写。**
（§48.75/§48.76/§48.83/§48.92/§48.95 修的都是这道门的**不同开法**，而这道门在能工作的版本里不存在。）

### 5.2 设备建矩阵自身**写不全 `r_hp`**
在 `build_slots_on_device()` 里 `build_correlation()` 返回后**立即**数 `gpu_r_hp`：

| 路径 | `r_stride` | `a_stride` | `r_hp` 非零 |
|---|---|---|---|
| 默认 | 504 | 54 | **2916/27216** |
| `OCUDU_CE_GPU_INVERT=1` | 504 | 54 | **2916/27216** |

**2916 = `a_stride²` = `A` 的大小** ⇒ 设备把 `A` 写对了、`r_hp` 只写了 **1/9**。
**默认路径一直靠主机 staging 紧接着把 `r_hp` 写满，掩盖了它** ⇒
**设备建矩阵从未被真正端到端验证过**（980/980 全绿是被覆盖前的值）。
`corr_stage` 的描述符两侧**已实测完全相同**
（`l=54 Ls=54 Ns=504 nf=36 npf=18 ncomb=6 a_sys=2916 r_sys=27216`）⇒ **不是参数问题**。

---

## 6. 下一步的**施工图**（按历史结构收敛，**不要再调 flag**）

### 第 1 步：统一三处 `a_rhp_filled`，让 R_hp 无条件写入
**做法**：把 973/1018/1832 **三处一起改**（`grep` 出来逐个数实参！），
使 `a_rhp_filled` **只表达"设备已经写过 A 和 R_hp"**（即 `device_built`），
**不再由 `dev_inv_now` 驱动**；
其中 1832 行需要先 `device_built = build_slots_on_device(...)`（**捕获返回值**，
别用 `(void)`），并**删掉不再使用的 `dev_inv_now`**。

> ⚠️ **注意** `build_slots_on_device()` 的返回值语义（**已在 `bc63c5329b` 核实**）：
> **`true` = 成功（设备已把 A/R_hp 写好并处理完）、`false` = 失败**。
> 出参 `deferred_corr` 是历史遗留的"设备反演实验要调用方自己派发的描述符"，
> **当前已完全不被函数体使用，可以删掉**。
> **返回值不是描述符** —— 我曾在 §48.83 把 `std::optional` 的 `has_value()` 当成功标志用，
> 语义恰好相反（成功时返回 `nullopt`），导致新代码静默不执行。
> **历史坑：`init()` 的返回值也与设备反演无关，别混用。**

### 第 2 步：单独修 K0-d 的 `r_hp` 只写 1/9（独立缺陷）
**判据**：让 `mmse_corr_r_hp` 内核**自己汇报**它写到的最大 `gid.x`
（用 §48.91 的 **out-of-line 探针缓冲**手法：内核写、主机读，**不要用主机侧断点式探针**，
因为那种探针自带的 `wait_pending()` 会改变被测行为 —— §48.88 我踩过）。
- 最大值 ≈ **2915** ⇒ dispatch 被截断
- 最大值 ≈ **27215** 而数据仍缺 ⇒ 写地址 / `p.Ns` 问题

### 每一步的判据
- **默认路径（硬性）**：三条抓包 `iq2_1009_17923`=23.97 / `iq1_10049_17921`=6.24 /
  `iq2_10044_17923`=31.78 dB **不变**；且 `980/980 A/B 逐字节` + `ctest -L phy 162/162`
- **设备 K1**：`r_hp_nz = 27216/27216`、三条抓包 SINR 与主机一致（±0.1 dB）

### 验证命令
```bash
cd /Users/jiachengwang/dev/ocudu
B=build/lib/phy/upper/channel_processors/metal/ul_chain_replay

# 默认 vs 设备 K1（三条抓包）
for c in iq2_1009_17923 iq1_10049_17921 iq2_10044_17923; do
  echo -n "$c 默认: "; $B /tmp/$c --metal --out /tmp/a 2>/dev/null | grep -o "sinr=[-0-9.a-z]* dB"
  echo -n "$c 设备K1: "; OCUDU_CE_GPU_INVERT=1 $B /tmp/$c --metal --out /tmp/b 2>/dev/null | grep -o "sinr=[-0-9.a-z]* dB"
done

# 980 抓包 A/B（默认 vs OCUDU_CE_CORR_DEV=0）—— 用 /tmp/ce_corr_ab.sh（若还在）
for i in 0 1 2 3 4 5 6 7 8 9; do /tmp/ce_corr_ab.sh $i 98 & done; wait
cd build && ctest -L phy -j 6 2>&1 | grep -E "100%|Failed"
```
`/tmp/ce_corr_ab.sh` 是本会话写的 980 抓包 A/B 脚本（分 10 片并行）；
**若 /tmp 被清理，需要重写**：逐个抓包跑默认与 `OCUDU_CE_CORR_DEV=0`，比较 `_llr.bin`。
抓包语料：`/tmp/iq1_*` 与 `/tmp/iq2_*`（共 980 个 `*_ce.txt` 基准），**务必保留**。

---

## 7. 纪律与教训（**本会话花掉最多时间的几条**）

1. **先写判据，再插探针**（§48.72 立的规矩）。每次插探针前先写下"看到什么数字就得出什么结论"。
2. **计数器防"以为跑了其实没跑"**：`device_corr_builds`、`corr_build_fail`
   就是为此加的（§48.74、§48.83）。**每接入一条新路径，都配一个"它执行了几次"的计数器。**
3. **改函数签名时，先 `grep` 出所有调用点并核对实参个数**。
   **"能编译过"绝不代表"实参对得上"** —— 12 个实参对 12 个形参会静默位移语义。
   **我因此连续失败两次**（§48.94/§48.95）。
4. **布尔量超过两个时，先写真值表再改代码**（§48.92 我没写表就动手，失败）。
5. **对标要落到"每一段的写入义务与条件"上**，不是只对"哪个函数被调用"（§48.97 的教训）。
6. **不要用已被自己推翻的结论去维持旧决定**（§48.85 的"决策理由污染"：
   K1 因**性能**退回 CPU，却被我**用错误的精度论据**留在那里，白停了一站）。
7. **观测手段不能扰动被测对象**：主机侧 dump 探针自带的 `wait_pending()` 会改变结果
   （§48.88 的假象）；**要判定"内核跑了多少"，让内核自己汇报**（out-of-line 缓冲，§48.91）。
8. **"SINR 几乎不变"不等于"结果没变"**：ridge 实验中 SINR 只动 0.31 dB，
   但 **240 抓包里 221 个 LLR 变了**（§48.75）。判据要落在下游真正消费的量上。
9. **清理代码后要检查函数完整性**：我用脚本清理时**误删过 `build_correlation()` 的 `return true;`**。
10. **诚实记录负面结果**：本会话有多次"修了但坏"（§48.93、§48.95 前的两次尝试），
    都写进文档并回退 —— **不许把半成品当成果提交**。

---

## 8. 关键文件

| 文件 | 作用 |
|---|---|
| `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.{h,cpp}` | **主战场**：`stage_engine_group()`、`run_engine_blocks()`、`engine_run()`、`build_slots_on_device()`、`device_inverts()` |
| `.../metal/ocudu_metal_mmse_engine.{h,mm}` | 引擎：`run()`/`run_async()`（含 K1）、`run_weights_only*()`、`build_correlation()`、`encode_corr()` |
| `.../metal/ocudu_mmse_inv.metal` | K1 内核（`MAX_N=54`） |
| `.../metal/ocudu_mmse_corr.metal` | K0-d 内核（**`r_hp` 只写 1/9 的嫌疑**） |
| `.../metal/ocudu_mmse_apply.metal` | K2b（`tid` = 输出索引，`tpt` 必须 = `nout`） |
| `.../metal/test/k1_check.cpp` | 独立反演 A/B 工具：`k1_check <A.bin>` 打印 `cond₂`、与 float64 参考的误差、残差、裁决 |
| `.../metal/test/ul_chain_replay.cpp` | 离线链路复算工具（`--metal`、`--also`、`--repeat`） |
| `doc_chinese/full_gpu_chain/s2_full_chain_design.md` | **活文档**：§48.84 横向状态视图、§48.97 历史对标、§48.85 决策理由污染 |

---

## 9. 上机（OTA）准备

**本会话已有一条通过的 OTA 腿**（`d8e23f67d1`）：设备建矩阵在真实链路跑通、链路健康。
**K1 回 GPU 的 OTA 验证尚未做** —— 用户明确说：**"OTA 验证的主要目的是确保 K1 回 GPU 是功能正确的；
现在 K1 都没有回 GPU，就没有必要做 OTA 测试了"**。

⇒ **等 §6 的两步做完、离线 3 条抓包都正确后，再上机**。命令（用户执行，agent 待机）：

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal \
  --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_ota_k1.log \
  > /tmp/gnb_ota_k1_console.log 2>&1
# 跑 2–3 分钟，Ctrl-C 正常退出（计数器在退出时打印）
```
**上机判据**：`[metal_stats] device_corr_builds` **> 0**（证明真的执行）、
`Real-time failure in RF` 个位到数十、**0 USB 错误 / 0 崩溃**、
`[mmse_time_sum]` 的 `corr=`/`gpu_path=`（**只记录，不设 gate**）。
⚠️ 上机前把 `OCUDU_CE_GPU_INVERT=1` 的**默认值**决定清楚：若要上机验证设备 K1，
需把它改成默认开启（`device_inverts()` 里 `enabled` 的初值），或在上机命令里带该环境变量。
**设备 K1 的已知代价**：每 hop 约 **+555 µs**（GPU 只干 4.4 µs，其余是 108 次 barrier 的延迟）——
按用户方针这是**性能债、不作阻塞**，真正的修复是 K1 内核（S-5a 的 blocked/simdgroup_matrix 形式；
`metal_nn_mmse` 已用上 8×8 硬件矩阵单元，K1 还没有）。

---

## 10. 账本（不阻塞，但别忘）

| 项 | 状态 |
|---|---|
| merged 分支的设备建矩阵 | 未覆盖（实网 62.5% 的 hop）；stride 基础设施已就绪（`67b6377216`） |
| K0-d 的 `r_hp` 只写 1/9 | **本会话发现，未修**（见 §5.2） |
| `build_correlation()` 的排队等待 | 实测是 **GPU 排队**（139–687 µs）；异步提交消不掉，只能减少 CB 数 |
| `OCUDU_CE_DEV_INVERT=1` 旧实验路径 | 已知不可用（`gpu_h` 只写 2 个 float、索引缩放特征 `0.9 = L/(L+comb)`） |
| K1b 右看式反演 | 数值错误，opt-in `OCUDU_INV_RL=1` |
| 设备 gather 的 +27 µs | 记在账上，按全 GPU path 方针不阻塞 |
| replay 工具 ~4% 间歇崩溃 | `rx_buffer_impl::get_codeblock_data_bits`，两条路边都有，与本工作无关 |
| ridge 正则化方案 | **已否决**（240 抓包中 221 个 LLR 变化），代码已删 |

---

## 11. 新 session 的**第一个动作**

```bash
cd /Users/jiachengwang/dev/ocudu
git log --oneline -1                    # 应为 bc63c5329b
strings build/apps/gnb/gnb | grep -E '^[0-9a-f]{10}$' | head -1   # 应一致
# 读活文档的两节：§48.84（横向状态）与 §48.97（历史对标）
```

然后**按 §6 的施工图做第 1 步**（三处一起改 `a_rhp_filled`）。
**先 `grep` 三个调用点、数清实参个数，再动手** —— 这是本会话用两次失败换来的教训。
