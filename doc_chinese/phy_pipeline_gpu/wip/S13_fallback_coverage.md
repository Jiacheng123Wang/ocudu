# S13 — 把"8/8"的适用范围堵上：回退路径的可见性与收窄

> 起点：`gpu_phy_iq2llr_zero_data_crossings`（`6603a13539`）。契约 8/8、读 0.00 + 写 0.00/跳，
> 但这是在**连续分配 + 变换 ≤2048 + 每个 stage 都命中设备**的输入下测出来的。
> 本文件是方案（先写判据再动手），不是结果。

---

## 1. 问题：契约的"零"有成立条件，而条件里有一部分**不可见**

`host device data crossings` 的判据是"宿主读/写设备数据的**次数**为 0"。它在**每一条审计过的路径**上是可信的
（数出来的）。但设备侧每个 stage 都有一条"这个跳我覆盖不了 → 交回宿主"的分支，而这些分支的可见性**参差不齐**：

| 设备 stage | 拒绝后的行为 | 现在可见吗 |
|---|---|---|
| K0-a 导频提取（LS）| 宿主前置级重建 LSE | **静默**（无计数、无日志）；但 `device_ls_valid=false` 会连带影响 K4 的 CFO 来源 |
| K0-a 的 engine 调用失败 | 同上（宿主前置级）| 有 warning ✓ |
| K0-d 相关矩阵（A/R_hp）| 宿主构建 | `corr_build_fail` 计数 ✓（但见 §2 盲点 1）|
| glue #2（设备写 y）| 宿主 staging y | **静默**（见 §2 盲点 3）|
| sigma2 | 宿主 `estimate_sigma2()` | 一次 warning ✓ + `sigma2_done` ✓ |
| K3（设备估计）| 等化器用宿主 gather | `equalizer ch_est device/staged` 计数 ✓ |
| K5 rsrp / K6-K7 TA | 宿主计算 | `device_rsrp_valid`/`device_ta_valid` → `host_grid_wanted` → **回读**（契约可见）✓ |
| 等化器 gather 表 | 宿主建表上传 | 写侧站点 ✓ |
| DFT 零拷贝 wrap | 拷一份 | `wrap_copies` + 契约的 `dft radio inputs` ✓ |

**空口实测（这一腿，`5g-epochs`）**：0 条回退 warning；PUSCH 分配形状**全是连续区间**
（`rb=[a..b)` / `rb=[a, b)`，没有位图式的稀疏分配）。所以**当前流量下没有回退发生** ——
问题不是"正在发生的 bug"，而是**"8/8 这句话的适用范围没有被量清"**，而适用范围里正好有**我们审计不到的读/写**。

---

## 2. 已证实的三个盲点（读代码确认，不是推测）

1. **A/R_hp 由宿主构建时，那次"交回去"没有计数。**
   `stage_engine_group()` 里 `memcpy(a_slot, w_r_pp…)` / `memcpy(r_slot…)` 把宿主算出的 A/R_hp 写进
   `gpu_a`/`gpu_r_hp`（设备要读的缓冲），**这一段没有任何 `count_host_write_site`**。
   按契约定义（"handing device-DERIVED data back"）这是一次写穿越 —— 它没被数。

2. **宿主前置级读的是设备常驻的资源网格。**
   回退时基类 `compute_hop_submit()` 从 `args.grid` 抽导频，而在 gpu 车道里这个网格是 **DFT 写在设备上**的
   （S-7b）⇒ 一次未计数的设备→宿主读。
   ⚠ 注意：该代码在**基类**（CPU 车道共用），CPU 车道里网格是宿主内存 ⇒ 计数必须**以"本车道的网格是设备常驻"为条件**，
   不能无条件加（否则会把 CPU 腿的宿主内存访问也算成穿越）。

3. **glue #2 回退时，宿主 staging y：读设备 LSE + 写设备缓冲，两次都没数。**
   `stage_engine_group()` 的 staging 循环里 `ls_pilot(args, …, /*scaled=*/true)` 读的是 `gpu_ls_out`
   （设备产生的 LSE），写进 `gpu_y`（设备消费）。现在只有**外围 pad 槽**的 memset 挂了站点
   （`ce: y pad rows cleared (host)`，且只在 `pad_y_slots` 为真时），**逐导频的读与写没有站点**，
   循环内的 pad **行** memset 也没有。

**结论**：存在"跳确实回退了、宿主确实碰了设备数据、而契约仍显示 0.00 + 0.00"的路径。
这正是"8/8 只在被审计的路径上成立"这句话的漏洞。

---

## 3. 目标（可判据化）

**G1（仪表完备性）**：对**每一条**"设备→宿主回退"与"宿主 staging 设备缓冲"的路径，
要么有计数/日志，要么有理由说明它不构成穿越。判据：把每个回退用现成旋钮**强制触发一次**，
`[metal_stats]`/契约行的对应数字必须**按预期变化**；旋钮关掉后必须回到 0 且 27 捕获 dump 逐字节不变。

**G2（契约自证）**：回退跳一旦发生，`host device data crossings` **必须变红**（因为回退必然要宿主碰设备数据）。
判据：强制回退的离线跑，契约从 8/8 变 `NOT MET`，且写侧分项表点名到站点。

**G3（收窄回退面，按 G1 的量决定）**：
* G3a **稀疏（非连续）RB 掩码**：让 K0-a / K3 / 等化器支持"每 PRB 的 active 位图"（现在 kernel 依赖
  `first_prb + k/ncomb` 的连续假设）。做完，8/8 从"连续分配"扩到"任意分配"。
* G3b **变换 >2048**：融合 TA kernel 的线程组内存上限；生产上 `get_idft()` ≤2048 ⇒ **只要求"拒绝时可见"**，
  不要求实现（除非将来有需求）。
* G3c 其它小门（`nof_device_y_stage` 满、`scatter_available()` 缺、组合超限）：改成"必然不发生"或"可见"。

**非目标**：不改控制面（`cbs/lane`、fence、gap —— 那是候选 1）；不动 CPU 车道的任何行为。

---

## 4. 分阶段与每步判据

### P1 仪表与三个盲点（不改行为）
1. 给"静默拒绝"的门各加一个计数器 + 一次性 warning：
   `device_ls_refused`（几何被拒，区别于 engine 调用失败）、`device_y_refused`（`record_device_y_stage` 的每个
   return false 分支分类）、`device_corr_refused`、`ta_refused`、`sigma2_refused`、`k3_refused`；
   打印进 `[metal_stats] mmse_ce` 行（现有格式）。
2. 补三个盲点的计数（只在**确实构成穿越**时计）：
   * 宿主 staging A/R_hp → `count_host_write_site("ce: A/R_hp staged (host)")`；
   * 宿主 staging y → `count_host_read("ce: y staged from the device LSE")` +
     `count_host_write_site("ce: y staged (host)")`（含 pad 行 memset）；
   * 宿主前置级读设备网格 → **通过一个只被 Metal 后端覆写的虚函数**加计数
     （基类不能无条件加，见 §2 盲点 2 的注意）。
3. **判据（这一步必须能证伪自己）**：
   * 27 捕获 replay：四个 dump 与当前 HEAD **逐字节相同**（插仪表不改行为）；
   * 用 `OCUDU_CE_DEV_Y=0` / `OCUDU_CE_DEV_TA=0` / `OCUDU_CE_DEV_SIGMA2=0` / `OCUDU_CE_CORR_DEV=0` /
     `OCUDU_CE_CPU_LS=1` 各跑一次：**对应的新计数器非 0，且契约变红**（若是穿越）；
   * 关掉旋钮：所有新计数器回到 0，契约回到 8/8。

### P2 造"稀疏分配"语料（先离线）
* 从现有 27 捕获里挑 PUSCH 分配，重排/裁剪出**非连续** rb 掩码（或直接合成：把某个捕获的 rb_mask 改成
  "前 k 个 PRB + 后 m 个 PRB"），跑 `ul_chain_replay`；
* 判据：**当前** HEAD 上这些语料**必须触发回退**（新计数器非 0、契约红）—— 这是对 P2 语料有效性的证明；
* 然后它是 G3a 的回归语料（做完 G3a 后要求：计数器 0、契约 8/8、四个 dump 与"连续分配版本"的语义一致）。

### P3 收窄（G3a/G3b/G3c）
* G3a 的实现路线（初步）：把 kernel 的"连续假设"换成每 PRB 的 active 位图参数 ——
  影响面 `ocudu_mmse_pilots.metal`（提取）、`ocudu_mmse_reformat.metal`（K3 的 dc/偏移）、
  `ocudu_eq_build_gather`（等化器 gather）+ 三处的宿主 staging；
* 判据：P2 语料契约 8/8、27 捕获逐字节不变、CE/等化器单测全过（macOS `ctest -L phy` 172/172）、
  Ubuntu 7617/7617 不变。

### P4 空中腿
* 一腿：新计数器**全 0**、契约 8/8、并按**分配形状分层**统计（确认空口是否真的从不产生稀疏分配；
  若从不产生，就如实写进文档：G3a 的收益是"适用范围"而不是"修了正在发生的错误"）。

---

## 5. 风险与诚实边界

* **R1**：给基类加"读网格"计数，若条件写错，会把 **CPU 车道**的宿主内存访问算成穿越
  ⇒ 契约在 CPU 模式下变红。缓解：条件用"本车道网格设备常驻"的虚函数钩子，且 CPU 模式下必须为 false（有测试）。
* **R2**：加计数会改变 `[metal_stats]` 的输出行 ⇒ 任何**按行号/格式**解析日志的脚本要同步（`leg_report.sh`、
  `wip/*.sh` 的 grep 模式要检查）。
* **R3**：G3a 触及 LLR 关键路径（提取 + gather），必须过完整判据（27 捕获逐字节 + 单测 + 空中腿），
  风险等级等同于 5e。
* **R4**：**当前收益是"证据完整性"，不是性能**。若空口从不出现稀疏分配，G3a 只把 claim 从
  "连续分配下成立"扩到"任意分配下成立"——**判据要分开写**，不要把它说成修 bug。

---

## 6. 第一步（建议直接做 P1）

P1 是纯仪表、行为不变、判据清晰（27 捕获逐字节 + 旋钮强制回退必须让数字动），而且它**先证伪自己**：
如果某个"盲点"其实另有计数路径，P1 的量会立刻显示出来。
P1 做完再决定 G3a 要不要做（看 P2 语料触发的回退跳里，稀疏掩码占多少）。
