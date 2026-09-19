# session_handoff_2026-09-19-2 — 开工快照

> **这是给"新会话"用的现状快照，不是技术文档。**
> 判据、代码地图、技术事实、口径、批次计划、踩过的坑、否定结果台账——
> **全部在 `gpu_phy_pipeline_design_and_implementation.md`（常驻设计文档）**。**开工先读它。**
>
> 本文只回答两件事：**现在到哪了**、**下一步做什么**。
> **本文写于 2026-09-19 的会话中途（该会话并未结束）**，用于下次新会话恢复状态。
>
> 命名：`session_handoff_<YYYY-MM-DD>-<序号>.md`，新会话永远读**序号最大的那一份**；
> 旧的**保留不删**（它们记录了当时的判断，包括后来被推翻的那些）。

---

## 0. 一句话

分支 `apple-silicon`，HEAD = **`94df4144a2`**，工作树干净。**5a 第 3 步：comb 已修好并送达 kernel，归约仍写 0；下一步是确认 kernel 是否真的执行。**

**⛔ 当前状态：批次 5a 进行中（第 1 步已落，第 2 步未做）。**

## 用户已批准的方案：把上报量搬进设备（含退路）

> **用户原话要点**：rsrp/ta 可以在 GPU 内部算好；**用一个 config 包起来**，
> **default 要坚持设计初衷（CPU 不在流中间，即不执行包裹里的路径）**；
> 如果最后找不到替代方案，**再放出来**。

**⇒ 这比"γ 把上报置 0"好得多**：γ 从"牺牲功能"变成"把功能换个地方算"，且**可逆**。

| 步 | 内容 | 状态 |
|---|---|---|
| **5a** | rsrp + noise_variance 搬设备 | **第 1+2 步完成**：kernel（`73ec94b08e`）+ **引擎侧 K5 阶段**（`74620389ef`），**默认关闭**（`rsrp.dst == nullptr`，实测路径与计数器不变）。**剩第 3 步：估计器侧**（见下）|
| **5b** | TA 搬设备（IDFT + 峰拟合，236 行估计器无设备对应物）| 未开始 |
| **5c** | 开关 `OCUDU_CE_DEV_STATS`（默认 1 = 设备算）| 未开始 |
| **5d** | γ 落地（`OCUDU_CE_HOST_GRID` 默认 0）| 未开始，**等 5a/5b** |

### 5a 关键设计（已写在 kernel 头注释里）

* **读哪里**：设备**已经有**宿主约简用的同一份估计——kernel 走 **reformat（K3）刚写的那个 `h`**，
  在**导频位置**（DM-RS 符号的 comb，**K3 故意排除**、只留数据 RE）取值。
  ⇒ 导频 RE **在 `h` 里可达、在 `gpu_ce` 里不存在**，所以 kernel 直接走 `h`。
* **复用 `mmse_reformat_params` 的几何字段**，两个 kernel 不可能对"哪个子载波是哪个"产生分歧。
* **输出是原始 Σ|h|²**（每 (块槽, 层)），**故意不是最终 rsrp**：宿主仍按原来的归一化
  （逐符号平均 × beta²）处理，两边**逐项可比**。
* **求和的顺序是设备的** ⇒ 与宿主顺序求和**浮点重结合一致，非逐位相同**——
  这正是上报量允许的，也是**下一步做容差对照、而不是声称相等**的原因。

### ⚠ 上一轮的一个判断已被本次侦察修正

我之前测出 `gpu_ls_smoothed` 与宿主 `filtered_pilots_lse` **差 2.14 倍**，据此否掉了"复用
`gpu_ls_smoothed`"这条路。**那不是缺陷，是我选错了源**：
宿主 rsrp 用的是**估计网格**在导频 RE 上的取值（`pending_fill::fill()`），
而 `gpu_ls_smoothed` 是**平滑后的 LSE**——两者本就不是同一个量。
**设备上对应"估计网格"的是 K3 的 `h`（= `gpu_ce` 的来源）**，5a 用的正是它。

### ✅ 5a 已完成的两步

1. **`73ec94b08e`** — `ocudu_mmse_rsrp.metal`：K5 归约 kernel，走 **K3 刚读的那个 `h`**、
   在**导频 RE**（K3 排除的位置）取值，输出**每 (块槽, 层) 的原始 Σ|h|²**。
   复用 `mmse_reformat_params` 的几何字段。
2. **`74620389ef`** — **引擎侧 K5 阶段**：`reformat_stage::rsrp_stage_t{dst, n_blk, pilot_re_bits}`、
   `rsrp_available()`、pipeline 编译、以及在**与 reformat 同一条命令缓冲**里 encode
   （⇒ 取这个值**不需要宿主等待**）。
   **⚠ `pilot_re_bits` 是每层独立的**，不是 reformat 的 `dmrs_re_bits`（那是各层并集，
   用来跳过导频 RE）；用并集会把**另一层的导频算进本层的功率**。
   **默认关闭**：`rsrp.dst == nullptr` 且有 guard ⇒ 实测路径与计数器**未变**。

### ⚠ 5a 第 3 步：宿主接线已写，**但对照探针报 0 —— 设备归约没出值**

`205eb117f8` 加了：轮转块（`kRsrpBlocks`×`kRsrpSlots`）、**每层导频 comb**
（`gpu_ce_pilot_re_bits`）、`OCUDU_CE_DEV_STATS` 门（**默认 1 = 设备算**，0 = 退路）、
以及 `OCUDU_CE_RSRP_CHECK` 对照探针。

**探针结果（5a 的判据，当前不通过）**：

```
[rsrp_check] hops=1 layers=1 npt=3 blocks=1 | non-identical 1 | bit-identical 0
             | worst rel 0.000e+00 (dev 0.000000e+00 host 0.000000e+00)
```

**两侧都是 0**。设备侧为 0 ⇒ **归约没有交出值**：要么 kernel 在这条路线上**没被 dispatch**，
要么它**读错了地方**。**⇒ 设备值现在不可用于发布。**

**⚙ 已知会影响判读的一点**：`OCUDU_CE_RSRP_CHECK` 探针会**读宿主网格**，
所以在 `OCUDU_CE_HOST_GRID` 默认（=1）下它照常工作；**当 γ 落地（默认改 0）后，
探针必须自己触发一次按需物化**，否则宿主侧恒为 0，对照失效。**下一轮修 kernel 时一并处理。**

### 🔍 进展：comb 已修好并确认送达 kernel —— 但归约仍写 0（`94df4144a2`）

**已修好并逐项确认的**（每一条都有打印证据，不再是推断）：

```
[comb]        layer=0 npat=1 size=12 bits=0x555 syms=3 npt=3     ← 宿主 comb 正确
[rsrp_params] ... dmrs_sym_bits=0x884 pilot0=0x555 pilot1=0      ← 参数正确送达 kernel
[k5]          entered, dst=0x863f30010 n_blk=1                   ← encode 块确实进入
[rsrp_check]  dev 0.000000e+00 host 5.435613990e-01 | host_nre 72 dev_nre 0
```

**comb 为空的原因**（已修）：它被建在 `device_estimate_offsets()` 里，而那个函数在
**几何不连续时 `return 0` 早退**。已移到 `apply_fd_td_estimation_stage()`，在**每条路径上都建**。

**⚠ 我自己的两个诊断曾经骗了我，都已修**：

1. `[rsrp_params]` 打印**放在填 combs 的循环之前** ⇒ 永远打 0，把我引向宿主侧白查一轮；
2. 探针用 `rsrp_base_`（**正在 staging 的那一跳**的块）读，而**较早那一跳的 completion 跑在
   后续跳已 staging 之后** ⇒ 读到别的块。已改为 `rsrp_block_last`（**本跳预留的块**）。

**剩下唯一的假设（已收窄）**：**kernel 自身**。本例 `is_std = (0*36) < 36 = true`，
入口条件应当成立 ⇒ 指向 **`h` 的行索引**：`sys` / `blk` / `nf` / `sc0` / 行跨步
都需要与 **K2 实际写的内容**核对。

**最省的下一步**：先让 kernel 返回一个**已知常数**（确认 dispatch 真的执行了），
再逐步收窄它的读取。**不要又一次从参数侧猜。**

### 📜 早期记录：comb 为空的第一次定位

调参 dump 给出了确凿证据：

```
[rsrp_params] nout_stride=504 n_blk=1 nf_std=36 sc_tail_base=36 nf_tail=12 sys_tail=1
              layers=1 symbols=14 dc_sc=4294967295 dmrs_sym_bits=0x884 pilot0=0 pilot1=0
[rsrp_check] ... dev 0.000000e+00 host 5.435613990e-01 | host_nre 72 dev_nre 0
```

**`dmrs_sym_bits=0x884` 正确**（符号 2/7/11）、几何正确、`n_blk=1` 正确。
**但 `pilot0=0`** ⇒ kernel 一个 RE 都没匹配上（`nre=0`）。

**根因**：我把每层 comb 的构建**放进了 `device_estimate_offsets()`**，
而那个函数在**几何不连续时会 `return 0` 早退**（`port_channel_estimator_metal_mmse_impl.cpp:3014`）：

```cpp
if ((hop_rb_mask.find_highest() + 1 - first_prb) != nof_prb) {
  return 0;   // ← 早退时 gpu_ce_pilot_re_bits 从没被写
}
```

**⇒ 修法**：把 comb 构建移到**不依赖那次早退**的地方
（在 `apply_fd_td_estimation_stage` 里直接从 `args.dmrs_patterns` 填，
紧挨着实际使用点），并且**在早退分支里也要填**。

**⚠ 顺带记一个坑**：这个 kernel 之前还犯过一个错——用 `device float2* out` 却写
`out[idx]`（float2 指针的 `[]` 按 8 字节跨步），结果是**写到了错误的位置、值读出来是 0**。
**已改成 `device float*` + `out[2*idx]` / `out[2*idx+1]`。**

**下一轮从这里开始（按顺序）**：
0. **（已确认）K5 会被 encode、kernel 会跑** —— 早期它产出过 `3.325204e-01`，
   所以"没被 dispatch"这个假设已被排除。
1. **把每层 comb 的构建移出 `device_estimate_offsets()`**（见上），
   并在其早退分支里也填。
2. 重跑探针：期望 `dev_nre == host_nre`（本例应为 72），然后 `worst rel` 落在
   浮点重结合允许的范围内。
3. 若 RE 数对上了但功率仍差常数倍 ⇒ 才是缩放问题（那时再查 `beta`/归一化）。
4. 修好后**探针必须先通过**（`bit-identical` 或 `worst rel` 在浮点允许范围内），
   才接发布路径。

### ⛔ 5a 剩第 3 步的其余工作

1. 分配**轮转输出块**（仿 `gpu_ls_sigma2` 的 `kSigma2Blocks`：宿主读它时**别的跳可能已经跑过**，
   所以要多个块轮转），`alloc_aligned_pages<float>`；
2. 从本跳的 DM-RS pattern 填 `reformat.rsrp.dst / n_blk / pilot_re_bits`（**每层一个 comb**）；
3. 加 `OCUDU_CE_DEV_STATS` 门（默认 1 = 设备算；0 = 宿主算，即退路）；
4. **容差对照探针**（如 `OCUDU_CE_RSRP_CHECK`）：与宿主的 `filtered_pilots_lse` 归约**逐值比较**
   —— **这是 5a 的唯一正确性判据，不能跳过**。
   ⚠ 先建探针、再接发布路径。**别跳过对照**（本线 §9.1 空门的教训）。

**⚠ 5b（TA）是这批最容易出错的部分**：IDFT 尺寸、峰拟合、`max_ta`、单位换算。
**先做"只计算不发布"的对照探针**，与宿主逐值吻合后再接入发布路径。

## 当前判据（未变）

```
离线:  2.10 read + 0.65 write /跳   （申报含 demapper，是上界）
空中:  1.33 read + 0.27 write /跳   （腿 ota-b4，RTF 1，gaps 0，契约 7/8）
```

## ⚠ 一个必须随结论一起说的话（§16.4）

跨越计数**按定义不含** "IQ 上传"与"LLR 下载"。所以 `reads==0 && writes==0` 的含义是
**"已审计模块在流的中间没有接触"**，**不是**"全程只有两次穿越"——**后者没有独立判据**。
**宣布最终目标达成时必须说清楚这一句**（§9.2 撤回里程碑的教训）。

## ⚠ 未定论的观察（别丢）

最近几腿的上行 PUCCH fmt2 RSRP 在 **−35 dB** 左右（A/B 两腿是 −3.8/−2.9），
**但 CRC 通过率反而更高**。config 不同所以不可比。下次碰 RF 时优先核（§14.1 末尾）。

**上一轮 OTA 已过**（腿 `gnb_gpu_ota-b2-recon_0919_0516`，`965b0f0951`）：
真机 ping/iperf3 通过；契约 **7/8**（唯一 FAILED 是**已知的**跨越检查）；
RT failures 2/85293；`ce device estimates: 24079 device / 0 host`；zero-copy 0 failures / 0 misaligned。

**离线基线（`syn004_4 --repeat 20`）**：

| 配置 | 读/跳 | 写/跳 |
|---|---|---|
| 默认 | 3.10 | **1.65** |
| `OCUDU_CE_HOST_GRID=0` | 1.00 | 1.65 |
| `OCUDU_CE_DEV_Y=0`（参考臂）| 3.10 | 2.65 |

**空中基线（新口径，含 `dft` 申报，`965b0f0951` 那条腿）**：读 2.37 + 写 1.75 每跳（2189 跳）。
详见设计文档 §2.5。**⚠ 不要和离线数字或历史腿的绝对值比**——口径/几何不同，见 §2.4。

**写侧累计 36.00 → 1.65/跳。读侧空中 2.37/跳**（剩 `gpu_ls_cfo` 进位 1 读+1 写、`pilots_power` 1 读，
以及批次 2 的回读）。
回读对**发布路径零依赖**已实测（设计文档 §4.4）；**α 的第一步已试并回退**（§8.2）。

---

## 1. 待用户执行的 OTA（本轮）

```bash
cd /Users/jiachengwang/dev/ocudu
# 1) 打戳并构建（run_leg.sh 会校验二进制戳记 == HEAD，不等就 exit 2）
touch build/hashes.h && cmake --build build --target gnb
# 2) 跑腿
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu ota-b3a
# 3) 出报告
bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh \
  doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_ota-b3a_*.log
# 4) 真机功能测试：手机 ping / iperf3
```

* **改了什么**：批次 3a（`f4dce0e95b`）——尾组 pad 槽改由**设备**清，删掉宿主那次多余的
  `gpu_y` memset。**默认路径的行为确实变了**（少一次宿主写），所以这一轮 OTA 是**必需**的，
  不像上一轮那样只是"确认没变"。
* **预期现象**：功能与上轮**无差别**（少写的是设备本来就要覆盖的内存），
  但**期待你看到契约的跨越数字下降**（写侧应比上轮少约 0.9/跳）。
* **重点盯**：`Real-time failures` 与上轮同量级；契约行；`ce device estimates` 仍是 0 host；
  zero-copy 仍 0 failures；ping 无丢包、iperf3 吞吐与上轮相当。
* **回退**：`git revert f4dce0e95b`（或切回 `965b0f0951`）。

---

## 2. 复现基线（确认环境正确）

```bash
cd /Users/jiachengwang/dev/ocudu
git log --oneline -3                      # 应是 965b0f0951
git status --short lib/ include/          # 应为空

cmake --build build --target ul_chain_replay
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/base 2>&1 >/dev/null \
  | grep -a crossings -A1
```

| 期望 | 读/跳 | 字节/跳 | 写/跳 | 字节/跳 |
|---|---|---|---|---|
| **默认** | 3.10 | 1420 | 2.65 | 882 |
| **`OCUDU_CE_HOST_GRID=0`** | **1.00** | 0 | 2.65 | 882 |

申报模块：`equalizer, dft, channel_estimator`（**`demapper` 未申报 ⇒ 数字仍是下界**）。

```bash
# 本会话新增的 A/B（期望 _llr.bin 0  _h.bin 0  .bin 0  _ce.txt 1058）
bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh "" "OCUDU_CE_HOST_GRID=0"
```

---

## 3. 本会话（2026-09-19 第 2 次）做了什么

| 提交 | 内容 | 效果 |
|---|---|---|
| `965b0f0951` | **批次 2 侦察**：新增 `OCUDU_CE_HOST_GRID` 探针；修掉 `host_grid_pending` 的悬空状态 | 读 3.10 → **1.00**/跳（旋钮关闭时）；发布路径零影响，27 捕获两条网全过 |

**两条被推翻的结论**（详见设计文档 §8）：

1. 上一份快照说"宿主回读 h 是为了算 3 个信道统计量"——**错**，`channel_statistics_estimator_fixed`
   的 tau/fd 是常量且 `consumes_pilots()==false`（设计文档 §4.2）；
2. **本会话新推翻**："设备已有 `filtered_pilots_lse` 的对应物（`gpu_ls_smoothed`），只差一个归约"——
   **错**，实测两者相关但不同（设计文档 §8.2）。探针已回退。

---

## 4. 下一步（**OTA 回来之后**，按顺序）

> **每一大步做完都要回到 §1 的 OTA 停机**（设计文档 §10.1）。下面是**顺序**，不是"一口气做完"。

1. **批次 3b**：`gpu_ls_cfo` 进位（1 读 + 1 写）与 `pilots_power` 读（1 读）。
   **⚠ `gpu_ls_cfo` 的进位是承重的**：`mmse_pilots_cfo` 在 `nof_dmrs_symb < 2` 时**提前返回、不写**
   `out[0]`，所以不能只删掉宿主那次复制——要让 kernel 读上一槽、并在提前返回时也写。
   详见设计文档 §5.3。**⇒ 做完停一次 OTA。**
2. **γ**（`OCUDU_CE_HOST_GRID` 默认改 0）：读 1.00/跳、发布路径零影响已实测。
   **但必须先拿到用户的明确决定**——它会让 `mode=gpu` 下上行 `rsrp` / `ta` 上报失效。
   这些量**不进 LLR 路径，LDPC 曲线和 ping/iperf3 都看不出来**，所以不能用"离线判据全过"
   或"LDPC 没掉"来背书（设计文档 §1.6/§1.7）。**不要自行落。⇒ 落之前先问；做完停一次 OTA。**
3. **建端到端 LDPC 判据工具**：`pxsch_bler_test`（**已构建**）可以 `-c cpu` vs `-c metal_mmse` 出两条
   BLER 曲线；置信带抄 `plot_bler.py` 的 Wilson 区间。详见设计文档 §6.8。
   **这是新主判据的落地前提——没有它，"≤0.5 dB"无法复核。**
   属于**工具**不是功能改动，不触发 OTA。
4. **批次 2 的 α**：等 γ 的产品决定有答案之后再动；**先解决"设备侧 rsrp 的参考是什么"**
   （设计文档 §8.2 有代码形状与踩过的细节）。**判据放宽不解封 §8.2**——那是 2.14 倍的错，不是精度。
   **⇒ 做完停一次 OTA。**
5. **批次 4**：审 `demapper` 并申报（唯一未申报模块，做完数字才是上界）。

---

## 5. 开工纪律（细节见设计文档）

- **⛔ 一个阶段性任务做完就停下来要 OTA，拿到结果才继续**（设计文档 §10.1；**不可跳过**）；
- **构建必须显式 `--target`**，否则拿到"新 metallib + 旧宿主"的假结果；
- **计数必须 `--repeat 20`**，单跳缓存是冷的；
- **代码 > 注释**，交接快照里的"关键代码位置"是线索不是结论；
- **判据不成立就不要说成功**；没有判据的改动不该进树；
- **判据分主次**（设计文档 §1.4）：结构性改动看穿越计数，动数值的看 LDPC ≤0.5 dB。
