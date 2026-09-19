# S11 — 批次 2 侦察：交接备忘 §5.1 的前提被推翻，读侧的真实归属被查明

> 日期：2026-09-19（第 2 个会话）。基线：`76fc488385`（批次 0、1 已完成）。
> 本文记录**测量**，不是推理。每条都有命令与输出。

---

## 0. 一句话

> **交接备忘 §5.1 说"宿主回读 h → 算 3 个统计量 → 传回设备"是错的。**
> 读侧的真实归属是：**宿主回读网格只为了算上报量（rsrp / 噪声方差 / 时间提前）**，
> 而**发布路径（`_llr` / `_h`）对这次回读的依赖是 0**。
>
> **判据（27 捕获，全过）**：关掉这次回读 ⇒ **`_llr.bin` 0 字节、`_h.bin` 0 字节、`.bin` 0 字节**；
> 读计数 **3.10 → 1.10/跳**。唯一变动在 `_ce.txt`（调试上报），**1058 字节**。

---

## 1. 前提为什么错（代码，不是注释）

交接备忘 §5.1 的依据是 `:1583`：

```cpp
const channel_statistics stats = stats_estimator->estimate(stats_in);
```

它据此推断宿主从回读的网格算出 `fd_hz / tau_rms_s / sigma2`。**实际是**：

| 事实 | 代码位置 |
|---|---|
| 唯一接线进去的实现是 `channel_statistics_estimator_fixed` | `factories.cpp:66` |
| `tau_rms_s` / `fd_hz` 是**构造期常量**，`estimate()` 只是把 `input.sigma2` 传出去 | `channel_statistics_estimator.h:83-89` |
| **`consumes_pilots()` 返回 false** ⇒ `stats_in.pilots_lse` 被**故意置空**，注释写明"一个没人会看的逐跳回读" | `channel_statistics_estimator.h:93`、`port_channel_estimator_metal_mmse_impl.cpp:1567` |
| `sigma2` 不来自回读网格：默认 `OCUDU_CE_HOST_SCALARS=0` ⇒ 取固定 0；A 的对角加载来自**设备自己的** `kRatioSlot`（`OCUDU_CE_K0A_RATIO_DEV` 默认 1） | `:1395`、`:1443` |

⇒ **`stats_in.pilots_lse` 这条链是死的**，宿主并没有"从回读网格算统计量"。

**回读网格的真实消费者**只有一条：

```
unpack_engine_group()  →  grid_est
    →  pending_fill::fill()                              (:3322)
    →  filtered_pilots_lse
    →  rsrp / noise_var / time_alignment                 (average_impl.cpp:519-552)
    →  get_channel_state_information()  →  CSI  →  FAPI 上报
    →  _ce.txt（调试 dump）
```

---

## 2. 逐跳读的精确构成（实测）

`--repeat 1` → 5 读；`--repeat 2` → 8 读；即 **3N + 2**。

| # | 位置 | 内容 | 归属 |
|---|---|---|---|
| 1 | `:1158` | `gpu_ls_cfo[slot] = gpu_ls_cfo[prev]` 进位（宿主手工步进设备数组） | **批次 3** |
| 2 | `:1348` | `pilots_power = gpu_ls_sigma2[kPowerSum] / nof_power_pilots` | **批次 3** |
| 3 | `:2864` × **2 次/跳** | `unpack_engine_group()` → `grid_est`（2 层 × 10 PRB × 1 符号 × 8 B = 1280 B） | **批次 2** |

2 次/跳是因为合并批次会 `defer_unpack()` 两次（标准组 `:2018` + edge 组 `:2019`），
`complete_fd_td_estimation_stage()` 再按 `nof_pending_unpacks` 循环回读。

⇒ **杀掉 unpack 只到 1.10/跳，不是 0。** 剩下的 1.10 是批次 3 的两个点。
（交接备忘把读侧整体记作"批次 2"，实际是 **2.10 → 批次 2**，**1.00 → 批次 3**。）

---

## 3. 测量：判据（`OCUDU_CE_HOST_GRID`，默认 1 = 保持现状）

> 提交：`965b0f0951`。**本文所有数字都是这个提交上实测的。**

新增 A/B 旋钮 `OCUDU_CE_HOST_GRID`（默认 1，即保持现状）：

* **1（默认）**：完成时按 DM-RS 符号回读 `grid_est`（未 hop 且设备结果覆盖时只回读 DM-RS）。
* **0**：**整个网格都不回读**，一个描述符都不丢——真出现的宿主消费者
  （`OCUDU_UL_DUMP` 的 `_h` 抓取、`get_symbol_ch_estimate()`）由 `materialize_host_grid()`
  **按需**物化，与"非 DM-RS 符号"本来就走的是同一条路。

**顺带修掉一个真实缺陷**：原代码里 `host_grid_pending = !publish_all_grid` 与"是否真的回读了"
无关——把回读关掉之后，它会变成 `false`，于是**按需物化也不会发生**，宿主消费者会读到
**上一个跳的残留网格**。第一次做这个 A/B 时正是这样：`_h.bin` 差 **201278 字节**。
改成 `host_grid_pending = !host_grid_published() || !publish_all_grid` 之后才归零。

### 3.1 读计数（`--repeat 20`）

```
默认：  62 reads (28416 B) / 20 hops = 3.10 read(s) (1420 B) + 2.65 write(s) (882 B) per hop
网关：  22 reads ( 5376 B) / 20 hops = 1.10 read(s) ( 269 B) + 2.65 write(s) (882 B) per hop
```
（网关那 5376 B 是最后一次跳为 `_h` 抓取做的按需物化；无 dump 的稳态是 **1.00 read/hop**。）

### 3.2 数据判据（27 捕获）

```bash
bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh "" "OCUDU_CE_HOST_GRID=0"
```

```
captures=27  missing-dumps=0  captures-with-differences=27  total-differing-bytes=1058
  _llr.bin   0
  _h.bin     0
  .bin       0
  _ce.txt    1058
```

**严格网（`OCUDU_CE_CPU_LS=1`）同样过**：

```
captures=27  missing-dumps=0  captures-with-differences=27  total-differing-bytes=1226
  _llr.bin   0
  _h.bin     0
  .bin       0
  _ce.txt    1226
```

### 3.3 跨跳稳定（批次 0 起的必过项）

网关下 `--repeat 1` vs `--repeat 20`：`_llr.bin` / `_h.bin` / `.bin` / `_ce.txt` **全 0**。

### 3.4 单元测试

`port_channel_estimator_metal_mmse_unit_test` → **All tests PASSED**
（含 Test 11 合并/拆分、Test 12 设备估计与宿主逐值对照、Test 13 三种 lane 序）。

---

## 4. `_ce.txt` 到底变了什么（已被用户裁定为"调试量，可变"）

同一个捕获（`syn004_4`）：

```
默认：  noise_variance=1.696761549e-01  snr=0.044493  rsrp=1.506316196e-02  epre=1.454060525e-01  ta_us=-0.034587
网关：  noise_variance=1.474540234e-01  snr=0.000000  rsrp=0.000000000e+00  epre=1.454060525e-01  ta_us=0.000000
```

| 字段 | 为什么动 |
|---|---|
| `rsrp` / `snr` → **0** | 由 `filtered_pilots_lse` 推得，网关下没人填它（现在是**字面的 0**，不是残值） |
| `ta_us` → **0** | 同上（`estimate_time_alignment(filtered_pilots_lse, …)`） |
| `noise_variance` 变了 | **补报时点**变了：不动的那次是**上一个跳**的残留，动的那次来自本跳设备值 |
| `epre` / `cfo_hz` **不变** | 从 `rx_pilots` / 设备 CFO 来，本来就不经过这次回读 |

⇒ **这次回读对"发布路径"的贡献是 0，对"调试上报"的贡献是 3 个字段。**

---

## 5. 由此确定的批次 2 真实内容

**目标**：把 2.10 读/跳 → 0，让上报量在**设备侧**算出来，而不是"宿主回读网格再算"。

**好消息：设备已经算了**。`mmse_pilots_fd_smooth`（`ocudu_mmse_pilots.metal:447-535`）把
`gpu_ls_smoothed` 填成 **逐 (符号, 层) 的 FD 平滑 LSE**，并且**已经乘了 `inv_beta`**——
这正是宿主的 `filtered_pilots_lse`（`pending_fill::fill()` 从 `grid_est` 采到的就是它）。
噪声方差更是**本来就在设备侧**（`mmse_pilots_sigma2`，`gpu_ls_sigma2[kSigma2]`）。

⇒ 缺的只是一个**逐层的归约**：从 `gpu_ls_smoothed` 求和得到每层 rsrp，写进一个槽位，
宿主按需读**标量**（而不是回读网格）。**这是新 kernel，但只读设备数据、只写设备内存。**

**上报量的可复现性风险**（要提前定）：
* `rsrp` 求和：设备用树形归约、宿主用顺序求和 ⇒ **不是逐位相同的 float**；
* `ta_us`：宿主跑的是 `time_alignment_estimator`，**没有设备实现**，搬过去等于重写一个估计器。

---

## 6. 下一步（三选一，待定）

| | 内容 | 代价 |
|---|---|---|
| **α** | **只做归约**：设备算每层 rsrp + 复用设备噪声方差，宿主读标量。`ta_us` 在 `mode=gpu` 下显式报"不可用" | 一个小 kernel；rsrp 不逐位相同 |
| **β** | **α + 时间提前也搬到设备** | 要重写 `time_alignment_estimator`，工作量大得多 |
| **γ** | **最小落地**：直接把 `OCUDU_CE_HOST_GRID=0` 变成默认，上报量显式置 0/不可用 | 零新代码；但 FAPI 的 rsrp/ta 上报会变 0 |

**推荐 α**：它把"设备 → 宿主 → 设备"这条链**真正断掉**（宿主只读标量），
判据沿用 §3.2（`_llr`/`_h` 逐字节不变）+ 新 kernel 与宿主 `rsrp` 的**逐值对照**（容差，不是逐位）。
**γ 可以作为 α 的中间态先落**——因为 §3 已经证明它对发布路径是零影响。

---

## 7. 纪律提醒（本线老账）

* `_ce.txt` 现在是**唯一**会因为这次改动变动的产物，而 §3.2 的脚本把它算进"差异"。
  用户已裁定它是**调试量**。**若采纳 α/γ，`ab_dumps.sh` 要能按"发布判据"与"调试判据"分开报**，
  否则以后每一次都会看到一个 1058，并有一天被误读成回归。
* **不要**因为"rsrp 变了"就说批次 2 失败——判据是 `_llr` / `_h` 逐字节 + 读计数。

---

## 8. ★ 做法 α 的第一步（设备侧 rsrp 归约）**已试，判据不成立，已回退**

**背景**：§5 推测"设备已有 `gpu_ls_smoothed`，就是宿主的 `filtered_pilots_lse`，
只差一个逐层归约"。**这个推测被实测推翻了。**

**做了什么**：在 `complete_fd_td_estimation_stage()` 里加了一个**宿主侧对照探针**
（`OCUDU_CE_RSRP_CHECK`）：同一次归约跑两遍——一遍读 `grid_est` 在 DM-RS 导频 RE 上的取值
（宿主生产路径的 `filtered_pilots_lse` 的来源），一遍读 `gpu_ls_smoothed`——逐值比较。

**结果（`syn004_4`，4 PRB / 3 DM-RS / comb 6）**：

```
[rsrp_check] layers=1 npt=3 npf=24 comb=6 lse_sym=3
             | non-identical 1 | bit-identical 0 | worst rel 1.137e+00
             | host 1.084547639e+00  dev 2.317298651e+00
[rsrp_check] beta=1.412538 scale=1.995263
             | host raw 5.435614e-01  dev raw (sum of 3 symbols) 1.0380e-01
             | host pts=72  dev pts=24
```

**逐元素（DM-RS 符号 1、层 0、前 6 个导频）**：

```
host s1[0]=1.090623e-01,1.124588e-01   dev s1[0]=1.030267e-01,9.965159e-02   ← 几乎相同
host s1[1]=1.011895e-01,9.099488e-02   dev s1[1]=7.489450e-02,1.152473e-01
host s1[2]=8.799541e-02,6.880812e-02   dev s1[2]=2.642789e-02,1.141723e-01
host s1[3]=7.028382e-02,5.257165e-02   dev s1[3]=-9.347606e-03,1.121275e-01
host s1[4]=4.895071e-02,4.598481e-02   dev s1[4]=-1.967132e-02,7.584769e-02
host s1[5]=2.526963e-02,4.761536e-02   dev s1[5]=-1.503688e-02,4.148215e-02
```

**判读**：

* 两者**相关但不同**：第 0 个导频几乎相同，之后**逐步发散**，实部包络一个递减、一个振荡；
* ⇒ **不是同一组数**（既不是同一个数组的错位，也不是纯相位旋转）；
* ⇒ `gpu_ls_smoothed` **不能**直接当作宿主的 `filtered_pilots_lse` 来归约。

**原因（读代码，已确认一半）**：
* 设备的 `gpu_ls_out`（`mmse_pilots_lse`）是 **LSE 原始值**：`rx · conj(ref)`，`[symb][layer][pilot]`，
  **未乘 beta、未做 CFO 补偿**（`ocudu_mmse_pilots.metal`）；
* `mmse_pilots_fd_smooth` 只补了 `inv_beta` 这一个因子；
* 而宿主的 `filtered_pilots_lse` 是**估计网格**在导频 RE 上的取值（`pending_fill::fill()`），
  走的是另一条路（interpolator 出来的网格 + 可能的 CFO 补偿）。
* **`compensate_cfo` 的默认是 false**（`du_low_config.h:103`），所以相位旋转**不是** CFO 补偿造成的——
  发散的原因**尚未定位**。

**⇒ 结论（重要）**：**做法 α 的第一步不能按"复用 `gpu_ls_smoothed`"来做。**
设备侧 rsrp 需要**自己的参考推导**（要么在设备上重算宿主那条网格路径，要么给 rsrp 定一个新的、
可以独立验证的定义）。**在没有这个对照之前写 kernel，就是 §8.1 的空门。**

**已回退**：探针代码全部 `git checkout` 掉，工作树回到 `965b0f0951`（只有 §3 那个已提交的网关）。

**给下一会话的提示**：这个探针本身是**有用的**（它证明了"设备已有现成数据"是错觉），
代码形状（`lse_symbols_last_hop` / `beta_scaling_last_hop` / `last_stage_re_pattern` 的取法）
记录在本文里，重做时不必重新摸。但**先解决"宿主的 filtered_pilots_lse 到底是什么"这个问题**——
它是**估计网格的采样**，不是 LSE。若它在设备上没有对应物，批次 2 的成本就要重新估。

