# S8 — 子句 A 达成：`mode=gpu` 的契约成立

> # ⚠ 撤回（`dbe0686fb8`）
>
> **本文下面宣布的"子句 A 达成"被撤回。**
>
> 那条 `0 host read(s) -> OK` 是**真的**，但它只是**一个模块里 4 个标量点**的状态，
> **不是车道的状态**。现已查明还有一处**每跳都发生**的宿主回读未被计数：
> `unpack_engine_group()` 把 **`gpu_h`（引擎的设备产出）** 读回宿主网格，喂宿主的跳统计。
> 它在车道路线上**照常执行**（`publish_all_grid=false` 只是把符号数从 14 缩到 `npt`，并不跳过调用）。
>
> **实测（离线、单捕获、单跳）**：
>
> | | 之前 | 现在 |
> |---|---|---|
> | host read | 1 (0 bytes) | **5 (6528 bytes)** |
> | host write | 30 (548 bytes) | 30 (548 bytes) |
>
> ⇒ **`mode=gpu` 的契约会重新报 `FAILED`**，这是正确的。
>
> **还有两处未解决，都不能当成"没有"**：
> 1. 均衡器与解调器**没有申报**（未审），所以不在这条计数的覆盖范围内；
> 2. 解调器的噪声方差**回退分支**是否在车道上走宿主副本，未查。
>
> **教训（本线第三次同类）**：一条检查的**措辞**比它的数字更容易骗人。
> `0 host read(s) ... the fused lane (mode=gpu) allows 0` 读起来是车道结论，
> 实际是 4 个点的结论。**宣布里程碑之前，先问这条检查覆盖了哪些模块、哪些方向。**


**空口腿** `gnb_gpu_s5_crossings0_0918_2135`，commit `a656133d70`。

```
[phy_pipeline] contract (mode=gpu):
  radio sample continuity: 0 gaps over 80049 blocks ... -> OK
  dft radio inputs: 208950 of 208951 transforms read the radio buffer -> OK
  zero-copy wraps: 19265 hits, 7361 creates, 0 replaces, 0 failures, 0 misaligned -> OK
  ce device estimates: 10098 device, 0 host -> OK
  host device data crossings: 0 host read(s) of device-produced data over 918 device hop(s)
                              = 0.00 per hop; the fused lane (mode=gpu) allows 0 ... -> OK
  cfo compensation: 0 round trips over 1120602 symbols ... -> OK
  baseband metrics: 0 symbols measured for 1120602 processed ... -> OK
  host sample assembly: 1120602 of 1120602 symbols read where the radio put them,
                        0 copied into a symbol buffer (mode=gpu) -> OK
[phy_pipeline] contract MET (8 of 8 checks applicable)
```

**⇒ 子句 A（只有两次 host↔device 数据穿越）达成。** 子句 B（一条融合的设备侧流水线）**尚未**：
`cbs/lane = 3.11`，因为 `EDGE_FUSE` 仍然默认关（乙未收）。

---

## 达成它的三步（每一步一个 commit、一次上机）

| commit | 做了什么 | 该步的空口判据 |
|---|---|---|
| `93964256aa` | **S4 根因**：`encode_corr()` 用**打包尺寸**申请相关矩阵槽位的映射，而 kernel 按**槽位行距 `Ls`** 写 → 块比槽位窄时（edge 组）超出部分全部丢失 | 默认关，逐字节未变 |
| `7524a9f80c` | **`TAIL_DEV` 默认打开**：设备建 edge 矩阵 | `cbs/lane` 3.00→**3.11**、`ch_est` cbs 1.00→**1.11**、`device_corr_builds` 0.952→**1.078**/跳、rtf 2、**契约仍 1 of 8** |
| `a656133d70` | **S5**：宿主不再回读那三个设备标量 | **穿越 3.00→0.00**、**契约 8 of 8 MET**、rtf 1/80049、`cbs/lane` 仍 3.11 |

**注意 `93964256aa` 这一步的空口证据是"没变"**——那一腿的判据只能是离线逐字节，
而 `TAIL_DEV` 的默认翻转（第二行）才是第一次真正的路线改变。

---

## 各腿对照（同一 testbed、同一配置）

| 腿 | commit | lanes | `cbs/lane` | 穿越/跳 | 契约 | rtf | crc KO 占比 |
|---|---|---|---|---|---|---|---|
| s2 | `ca9760b104` | 3174 | 3.00 | 3.00 | 1 of 8 | 0 | 15.4% |
| s3 | `861a4feeb3` | 2530 | 3.00 | 3.00 | 1 of 8 | 3 | 19.4% |
| s4 | `532a231782` | 1302 | 3.28 | 3.00 | 1 of 8 | 2 | 15.9% |
| s6 | `edf21dd778` | 1074 | 3.00 | 3.00 | 1 of 8 | 2 | 12.0% |
| s7 | `7524a9f80c` | 1118 | 3.11 | 3.00 | 1 of 8 | 2 | 10.4% |
| **s5** | **`a656133d70`** | 918 | 3.11 | **0.00** | **MET 8/8** | **1** | **18.9%** |

**crc KO 占比 18.9% 落在历史区间（10.4%–19.4%）内但偏高端**，且本 testbed 的 KO 本来就高。
单腿不足以判定回归；**若 ping/iperf3 出现任何异常，先重跑一腿再下结论**（本线已有"序 A/B flaky"
的教训，不用单次红绿当判据）。

---

## 契约**没有**覆盖的东西（写下来，免得被当成比实际更多）

`phy_pipeline_crossings` 数的是 **device → host 的*数据回读***。它**不**数：

1. **host → device 的写入**（`stage_engine_group()` 的 staging）。默认路线上设备已经建矩阵、
   `slots_filled` 为真会跳过 staging，但**"默认路线上宿主是否还会往设备写"没有被这条检查回答**；
2. **调试捕获的网格回读**（`materialize_host_grid()`，`ul_capture` 路线）；
3. **IQ 上传与 LLR 下载**——这两条按模式定义**本来就不该数**，它们是允许的那两次穿越。

⇒ **下一步该审的是第 1 条。** 契约转 OK 是"宿主不再往回读"，不是"宿主与设备之间只剩两次穿越"。

---

## 剩余工作

| 项 | 度量 | 现状 | 目标 |
|---|---|---|---|
| 子句 B：融合 | `cbs/lane` | **3.11** | 3.00（收掉乙：`EDGE_FUSE` 的 51810 字节差异）|
| 未计的 host→device 写入 | 待补判据 | **未测** | 先量，再决定 |
| 乙的 51810 | `EDGE_FUSE=1` vs 独立 | **51810 字节** | 0（下一个嫌疑：`st.burst` 时 corr 与 K1 之间那道被跳过的 barrier）|
