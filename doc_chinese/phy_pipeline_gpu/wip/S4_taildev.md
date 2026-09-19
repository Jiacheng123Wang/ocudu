# S4 — 合并 edge 块的相关矩阵搬上设备（`OCUDU_CE_TAIL_DEV`）

> # ⛔ 更正（S5 期间发现）：**本步的等价性从未被证明，`k0d` 那一次是空的**
>
> 本文件 §3.1 声称"`k0d`：设备建 vs 宿主建，`TAIL_DEV` 开 **0/27**"。**那个结论不成立。**
>
> `k0d` 的两条臂是 `OCUDU_CE_GPU_INVERT=0` 与 `OCUDU_CE_GPU_INVERT=0 OCUDU_CE_CORR_DEV=0`。
> 用门自己的覆盖计数器一查（**这正是它 `REPORT_COUNTER="device_corr_builds"` 存在的理由**）：
>
> | 捕获 | PRB | arm A `device_corr_builds` | arm B |
> |---|---|---|---|
> | `syn015_10` | 10（**有余数，有 edge**）| **0** | **0** |
> | `syn003_3` | 3（无余数）| 1 | 0 |
>
> **⇒ 在有余数的形态上，`OCUDU_CE_GPU_INVERT=0` 下设备一条相关矩阵都不建**
> （`dev_corr_std_merged` 要求 `gpu_invert_std`，`std_slots_filled` 要求 `!merge_tail`），
> 两条臂**都是宿主建** ⇒ 那是一次**宿主对宿主**的比较，**与 edge 的设备建毫无关系**。
> 门的注释里其实写明了这个陷阱（*"a capture whose hops are all merged is legitimately
> host-against-host. The coverage is REPORTED instead"*）——**我没有去看那个计数器。**
>
> **正确的判据（S5 期间用干净工具 `wip/ab_dumps.sh` 实测）**：
>
> ```
> default vs OCUDU_CE_TAIL_DEV=1      captures=27 missing=0  differ=15  bytes=6043
>                                     _llr.bin 1400   _h.bin 4295   _ce.txt 348
> 换宿主求逆再测一次（OCUDU_CE_GPU_INVERT=0）                differ=15  bytes=6040
> ```
>
> **⇒ 设备建的 edge 与宿主建的结果不同。** `TAIL_DEV` 旋钮**默认关**，所以**发布出去的默认路径没有受影响**
> （回退复跑三张网全绿），**但本文件 §3.1 与提交 `532a231782` 的 message 都高估了验证强度**。
> **下一步是查清设备建为什么不同**（pad？几何？R_hp 的补零范围？）。
>
> ## 已排除的原因（S5 期间逐条测过）
>
> | 假设 | 怎么测的 | 结果 |
> |---|---|---|
> | 是**宿主求逆 vs 设备求逆**的差别 | `GPU_INVERT=0` 下重测 | **仍差 6040 字节** ⇒ 不是 |
> | 是 **σ² 商取自设备**（`sigma2_from_device`）| `K0A_RATIO_DEV=0`（核改读宿主商）下重测 | **仍差 6043 字节** ⇒ 不是 |
> | 是 **DM-RS 符号表**不同（宿主用 `dmrs_sym`、我的设备调用用 `pattern_symbols`）| 读代码：`:1016` 的 `dmrs_sym` **就是**由 `pattern_symbols.for_each` 构造的 | **同源** ⇒ 不是 |
> | 是 **nout/L 几何算法**不同（`build_correlation_matrices` 的 out 参数 vs `correlation_stage`）| 读代码：两者都是 `L = npt*npf`、`nout = (n_prb*12)*14` | **同式** ⇒ 不是 |
> | 是**超大切片的 pad** | 逐元素比对：宿主是 `memset(整槽)+写块+对角置 1`；我的是 `写块（核）+清补集+对角置 1` | **等价**（前提是核写满 L×L 块）⇒ 很可能不是 |
> | 差异在哪儿 | `cmp -l` 定位 `_h.bin` | **只在文件尾部**（偏移 8603 → 13440，共 260 字节）⇒ **就是 edge 块那一段** |
>
> ## 下一个实验（最便宜、能把假设空间一分为二）
>
> **让宿主和设备都建，但让设备的值胜出**（保留 `build_correlation_matrices(...)`，
> 同时 `build_slots_on_device(...)`，`stage_engine_group(..., slots_filled=true)`），再与 `TAIL_DEV=0` 比。
>
> **✅ 已做（临时补丁，已回退）：结果 6043 字节，与不做补丁时完全一样。**
>
> ⇒ **设备算出的块值本身就是错的**，不是"跳过宿主建"的副作用。假设空间减半。
>
> ## 再排除一轮（同一轮内）
>
> | 假设 | 怎么测的 | 结果 |
> |---|---|---|
> | 对角**岭** `ridge` 不一致 | 读代码：核参数 `p.ridge = 1e-6F`（引擎 `:1294`），与宿主 `:778` 的 `1e-6F` 同值 | ⇒ 不是 |
> | 核写块的范围 | 读核：`mmse_corr_a` **只写 L×L 块**，注释明写 *"The slot is assumed to be zeroed by the caller"* | 与我的 pad 契约一致 |
>
> ## 剩下的唯一结构性差异
>
> **edge 的槽位是超大的（`Ls = L_std > L_e`），而标准组是精确的（`Ls == L`）。**
> 标准组的设备建**已被 `k0d` 证明与宿主一致**（`syn003_3`：arm A `device_corr_builds=1`），
> edge 的从来没有被任何门覆盖过。⇒ **嫌疑集中在"核在 `Ls > L` 下的行为"或"我的 pad"**。
>
> **下一个实验**：把 edge 的设备建放到**自己的精确槽位**（`a_stride = L_e`、`r_stride = nout_e`，
> 另开一段不共享标准槽的存储）跑一次；若与宿主建一致 ⇒ 缺陷在 `Ls > L` 路径。
> 这需要给 edge 单独一段槽位，不是一行改动——**动手前先想清楚它的代价**。

**提交**：`532a231782`（已推送）
**改动**：`port_channel_estimator_metal_mmse_impl.cpp` —— 新增 `edge_build_on_device()`
（`OCUDU_CE_TAIL_DEV`，**默认关**）；`build_slots_on_device()` 现在也写**超大槽位的 pad**；
合并分支在开旋钮时改用设备建。
**类别**：A 类（编排 + 构造位置），默认关 ⇒ 默认路径不变。

---

## 1. 为什么这一步是必需的

S3 的 A/B 量出：**27 个形态里有 15 个**在消费宿主的 `sigma2` ——即"有余数 ⇒ 并进 edge 块 ⇒
宿主建那个块的相关矩阵 ⇒ 宿主必须读回提取的标量"。**穿越 3.00/跳 的数据依赖就在这条分支上。**

**⇒ 把 edge 块的构造搬到设备，是穿越降到 0 的唯一路径。**

---

## 2. 为什么不能直接交给现成的设备路径（**陷阱在此**）

edge 块**住在标准组的槽位里**（stride `L_std`/`nout_std`），而它自己的块阶是 `L_e`/`nout_e`
⇒ **它的槽位是超大的（oversized）**。

而 `run_engine_blocks()` 只在 **`st.L == L`** 时才把槽位报成 "filled" —— 原因正是
**pad 是 `stage_engine_group()` 写的**，而那个函数在 `slots_filled` 时**跳过整个 staging**。

pad 是：
- **A → `blockdiag(A, I)`**：K1 于是只求逆一个可逆的 `Ls × Ls` 系统，
  `W = [R_hp | 0] · blockdiag(A⁻¹, I) = [R_hp·A⁻¹ | 0]`，`h = W·y` 保持未加 pad 系统的值；
- **R_hp → 行 `[0,nout)` × 列 `[0,L)` 之外全零**。

**它们是几何量，不是矩阵数据** ⇒ 搬进 `build_slots_on_device()`（真正填槽位的那个调用），
**宿主因此不必回到"建矩阵"的业务里**。两个循环在槽位精确时（`a_stride==L`、`r_stride==nout`）
是空操作——即改动前就存在的每一条路线。

---

## 3. 实测（全 27 个捕获）

### 3.1 正确性门 `k0d`：设备建 vs 宿主建，逐字节

| | 差异字节 | 捕获 |
|---|---|---|
| `TAIL_DEV` 关 | **0** | 0/27 |
| `TAIL_DEV` **开** | **0** | 0/27 |

（`missing-dumps=0` —— 第一次跑时我漏了存在性检查，`/tmp` 里一个残留目录让 `cmp` 比了空文件、
"假通过"了一次；已重做。**这正是本 session 反复出现的那类陷阱。**）

### 3.2 覆盖真的移动了（否则门可能是空的）

| 捕获 | PRB | `device_corr_builds` 关 → 开 |
|---|---|---|
| `syn003_3` | 3（无余数，**没有 edge**）| 1 → **1** |
| `syn015_10` | 10（有余数）| 1 → **2** |
| `syn025_25` | 25（有余数）| 1 → **2** |

### 3.3 **本步的目的：S3 的 A/B**

`OCUDU_CE_TAIL_DEV=1` 下，`OCUDU_CE_HOST_SCALARS=0` vs 默认：

| dump | **S4 之前** | **S4 之后** |
|---|---|---|
| `_llr.bin` | 12930 字节 / 15 of 27 | **0 / 0 of 27** |
| `_h.bin` | 13370 字节 / 15 of 27 | **0 / 0 of 27** |
| `.bin` | 0 / 0 of 27 | 0 / 0 of 27 |
| `_ce.txt` | 695 / 27 of 27 | **410 / 27 of 27** |

**⇒ 宿主的标量现在在每一个形态上对 GPU 都是死的。**
穿越那 3.00/跳背后的**数据依赖已经消失**。

剩下 `_ce.txt` 的那 410 字节是**头部的上报统计量**（`noise_variance`/`snr`/`cfo_hz`）——
**旁路输出，不属于 IQ → LLR 链**，是它自己的一个决定，不是核的依赖。

---

## 4. 上机（手机 OTA，`OCUDU_CE_TAIL_DEV=1`）

腿 `gnb_gpu_s4_0918_2004`（版本串 `532a231782`，用户跑）：

| 项 | 值 |
|---|---|
| 契约 | `mode=gpu`，`NOT MET: 1 of 8`（**那行仍是 `3.00 per hop -> FAILED`**，见下）|
| `Real-time failures` | 2 / 64763 = 0.003%（预算 0.0116%）|
| crc OK/KO | 1095 / 207 |
| `lanes` / `device_sigma2` | 1302 / **1302 == lanes** |
| `lane fence` | 2604 = **2×lanes**（**没有多出 fence**）|

### 4.1 覆盖确实上机生效了

| 腿 | `device_corr_builds` / hops |
|---|---|
| S3（`TAIL_DEV` 关）| 2813 / 3174 = **0.886** |
| **S4（`TAIL_DEV=1`）** | 1557 / 1302 = **1.196** |

**⇒ 每跳的设备建 +0.31**，与下面那个新出现的 `+0.28 cbs/lane` 对得上。

### 4.2 **新观察：lane 的命令缓冲数从 3.00 变成了 3.28**

```
S3（之前）： lanes=3174  cbs/lane=3.00 (max=3)   ch_est cbs/lane=1.00
S4（本次）： lanes=1302  cbs/lane=3.28 (max=4)   ch_est cbs/lane=1.28
```

`mmse_ce commits=2967` / 1302 = **2.28 次/跳**（原为 2.00）。

**原因**：`build_slots_on_device()` 走的是 `engine->build_correlation()` —— 一条**独立的命令缓冲**，
而且是**阻塞**的（宿主等它）。所以我的 S4 实现虽然**正确**（`k0d` 证明逐字节相同），却**每跳多了一条 CB**。

**⇒ 这是方向不对的地方**：本线的目标是**融合**（更少的命令缓冲、更少的宿主往返），
而现在的实现是**用"多一条 CB"换掉了"宿主建矩阵"**。
正确的形状是把 edge 的构造**并进引擎自己那条 CB**——`engine_run()` 已经有 `corr_prefix` 机制
（`gpu_invert` 时把构造作为第二条 CB 的前缀），只是它目前只描述**一个几何**。
**把它扩成两个前缀（标准 + edge）即可，不增 CB。这是 S5。**

---

## 5. 本步暴露、留给下一步的

1. **⚠ 多了一条命令缓冲（§4.2）**：`cbs/lane` 3.00 → 3.28、`mmse_ce` 2.00 → 2.28 次/跳。
   **S4 用"多一条 CB"换掉了"宿主建矩阵"——方向不对**，S5 应把 edge 的构造并进引擎自己的 CB
   （`engine_run()` 的 `corr_prefix` 扩成两个几何）。
2. **`TAIL_DEV` 目前默认关**。要真正把穿越降到 0，还需要：
   (a) 处理**统计量**那条（`noise_variance`/`snr`/`cfo_hz`）——设备侧算、或改上报方式、或明确接受；
   (b) 把 `TAIL_DEV` 与 `HOST_SCALARS=0` 变成**默认**，并让契约那行转为 `OK`。
3. **契约那行仍是 `3.00 per hop -> FAILED`**，因为回读还在（旋钮默认关）。
   **S4 移除的是它的理由，不是它本身。**
4. `wip/run_leg.sh` **不会**替跑腿的人传 `OCUDU_CE_TAIL_DEV`——上机验证这一步必须显式传，
   或等 (b) 把它变成默认。

---

## 6. 回退

`git revert 532a231782`。默认关，回退后无行为差异。
