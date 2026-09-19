# 交接 — 批次 5b→5g：上报量与写侧搬迁（**读侧归零、写侧归零、契约 8/8**）

> ### ⛔ 本文件已被 `session_handoff_2026-09-19-8.md` 取代（2026-09-19 晚）。
> -8 是**新的入口**：那里有 5g 收尾、里程碑 tag、ctest 注册、doc_chinese 入库、**S13-P1 的发现**
> （契约漏计了每跳的一条设备↔宿主往返）以及用户新增的两个测量需求。本文件保留作为 5b→5g 的过程记录。

> 上一份：`session_handoff_2026-09-19-6.md`（5b 完成时写的，之后 5c/5d/5e/5f 都在本文件里）。
> **常驻设计文档**：`gpu_phy_pipeline_design_and_implementation.md`
> —— 批次表在 §5，阶段总结 **§18**（5b+5c+5d，含 §18.7 fence 备选），写侧 **§19**（5e+5f+5g，含 §19.0 一览表与 §19.6 的 1-ULP 追查）。
> **本文件是入口**：先读 §1（一句话+下一步），再按需读 §4（下一步怎么做）/§6（硬纪律）/§7（命令）。

---

## 1. 一句话状态 + 下一步

> **⚠⚠ 2026-09-19 晚 勘误（S13-P1）：下面的"读 0.00"是"被审计站点上为 0"。**
> P1 给"宿主抽接收导频"补上站点后，**同一个 replay 显示每跳 1 次设备→宿主读 + 1 次宿主→设备写**
> （`ce: rx pilots staged (host)` 432 B；网格抽取 432 B），且这是**默认路径、早于 5a–5g**。
> dump 不受影响（27 捕获仍逐字节相同），**但"零穿越"这句话要等 S13-P2 消掉这条往返之后才成立**。
> 详见设计文档 §19.6.2b 的勘误块与 `wip/S13_fallback_coverage.md` §5b。

> **★★ 2026-09-19 更新（5g 完成，空中腿 `5g-epochs` 已跑：契约第一次 8/8）**
> * HEAD = **`45fa002d6c`**（5g：epochs 不再上传；空口 **读 0.00 + 写 0.00/跳**、分项表空）；
> * **契约 8/8 MET** —— **车道的"只有两个 crossing（IQ 上传 / LLR 下载）"判据第一次完全达成**；
>   写侧历史：903（5d）→ 501（5e）→ 1（5f）→ **0**（5g）；
> * 离线判据（27 捕获四个 dump 与前一版逐字节相同、CE `140 of 140`、等化器 `ALL OK`）+
>   空中判据（0 gaps、0 RT failure、CRC 79.19% vs 5e 79.24%、分层同型）全过 → 记录见 **§4.5 / 设计文档 §19.6**；
> * **这一腿同时补上了 5f-1 的空中判据**（§8 的第 2 项可以关掉）；
> * 硬纪律（新）：**给"输出出自长累加"的 kernel 加新算术会动发布位**（CFO 1 ulp、K4 2 ulp）——
>   这类 kernel 要**收参数**；只有 K0-a 的 `apply_cfo`/`sigma2` 还在设备上自算。

**HEAD = `45fa002d6c`**（工作树干净，只有用户的 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 未提交 —— **不要动它**）。

* **读侧**：融合车道的设备→宿主数据读 **归零（0.00/跳、0 字节）** —— 5a(rsrp)+5b(TA)+5c(默认翻 0)；
* **写侧**：**归零** —— 903 次/3.38 MB（5d）→ 501 次/5.5 KB（5e）→ 1 次（5f）→ **0**（5g）；
* **契约 8/8 MET**（空口腿 `5g-epochs_0919_1945`）：**车道的"只有两个 crossing（IQ 上传 / LLR 下载）"
  这个模式目标第一次完全达成**，而且它是**数出来的**（`phy_pipeline_crossings` 在读写点自增），不是推断的。

**⇒ 下一步不再是本线的主线**：5a–5g 的搬迁做完了。可选后续见 §8 的诚实清单
（稀疏 RB 掩码回退、>2048 变换、分裂尾块、`nof_layers>1`、TA 尾延迟的 §18.7 fence 备选等）。

（5f-1 此前只有离线判据的问题已随 `5g-epochs` 这一腿解决：该二进制包含 `9e36fef3ed`。）

---

## 2. 本轮做完的事（提交清单）

| 批次 | 提交 | 做了什么 | 判据 |
|---|---|---|---|
| 5a | `94df4144a2` | rsrp 走设备 + 接发布路径 | 7 语料设备==宿主；空口 rsrp 无跳变（§17.8）|
| **5b** | `1826e01e07` | `ta_us` 走设备：K7 摆放 + `dft_dit` + K6 归约，三条 dispatch 编进重放自己的命令缓冲 | 27 捕获 `ta_us` 逐位相同；空口 TA 命令无跳变（§17.9）|
| **5c** | `986c991742` | `OCUDU_CE_HOST_GRID` 默认 **0**：设备覆盖到的跳不回读 | 27 捕获四个 dump 逐字节相同；空口**读 0.00/跳**（§17.10）|
| **5d** | `503990ca5f` | TA 链**合并成一条 dispatch**（`mmse_ta_chain`；蝶形抽到 `ocudu_dft_butterflies.h` 共用）| S12 chain 4.39 ns/8.14 ns；与三条 dispatch 路线差 <1 ps（§17.10.5）|
| 5d+ | `413ef13f94` | 启动打印 `[ta_impl] …`（腿自证加载了哪种实现）| — |
| **5e** | `57a5ca6cbc` | 写侧**按站点记账**（`count_host_write_site`）+ 量清 903 次是谁 | 分项表印在契约行下面；**分项≠总数 = 还有匿名写点**（§19.0b）|
| **5e** | `6e53109ffa` | 等化器 gather 表改由**设备**建（`eq_build_gather`）| `OCUDU_EQ_TABLE_CHECK` 27/27 逐元素一致；27 捕获四个 dump 全 0 差异（§19.3/§19.3a）|
| 5e+ | `5bd33639ab` | 启动打印 `[eq_impl] …` | — |
| **5f-1** | `9e36fef3ed` | `h_starts` 进参数块（`equalize_strides`）+ **修掉一个既有批处理缺陷** | 等化器单测两种模式全过；27 捕获 A/B 全 0 差异；**replay 写 5 → 1/跳**（§19.5）|

---

## 3. 五条空中腿的数字（判据看这里）

| 腿 | 提交 | TA | 读/跳 | 写次数/腿 | 写字节/腿 | 契约 | CRC | RF fail | `ch_est` | `ch_wt` |
|---|---|---|---|---|---|---|---|---|---|---|
| `5a-pubpath_0919_1230` | `94df4144a2` | 宿主 | 1.39 | — | — | 7/8 | 73.35% | 0 | 124.3 | 352.3 |
| `5b-devta_0919_1650` | `1826e01e07` | 3 dispatch | 1.39 | 903 | 3.29 MB | 7/8 | 66.63%* | 42† | 119.4 | 399.8 |
| `5c-hostgrid_0919_1730` | `986c991742` | 3 dispatch | **0.00** | 903 | 3.38 MB | 7/8 | 80.08% | 1 | 123.6 | 406.1 |
| `5d-fused_0919_1820` | `503990ca5f` | **1 dispatch** | **0.00** | 897 | 3.29 MB | 7/8 | **81.23%** | **0** | 121.4 | 404.4 |
| `5d-devtaoff_0919_1920` | `413ef13f94` | **关** | 1.45 | 895 | 3.40 MB | 7/8 | 81.34% | **0** | 130.9 | **366.2** |
| `5e-devtables_0919_2000` | `5bd33639ab` | 1 dispatch | **0.00** | **501** | **5 556 B** | 7/8 | 79.24% | **0** | 115.8 | 385.9 |
| `5g-epochs_0919_1945` | **`45fa002d6c`** | 1 dispatch | **0.00** | **0** | **0 B** | **8/8 ✅** | 79.19% | **0** | 118.7 | **379.9** |

\* 只有真正跑流量的那一分钟是 73.90%（前 8 分钟是 UE 反复重接）† 42 次 underflow 全伴随重接/长空闲

**结论**：读 0.00；写字节 3.38 MB → 5.5 KB（5e）；5f-1 后空中预计只剩 **56 B（1 次）**。
`ch_wt` 比 5a 高 ~35 µs = TA 链的**尾延迟**（§18.7：已定位、已接受，不做 fence 实验）。

---

## 4. ★ 下一步：5f-2（epochs 进 kernel）

### 4.1 目标

`upload_symbol_start_epochs()`（`port_channel_estimator_metal_mmse_impl.cpp`）把 14 个符号起始时刻
（float，56 B）写进 `gpu_epochs`（零拷贝缓冲）——**只在 (cp, scs) 变化时**才真的写（内容比较在前），
所以空中是 **1 次/腿**。但**契约要 0**，所以必须让设备自己得到这 14 个数。

### 4.2 读者（改它们就要一起改）

| 读 `epochs` 的 kernel | 位置 | 用途 |
|---|---|---|
| K4（噪声）| `ocudu_mmse_reformat.metal:212`（`constant float* epochs [[buffer(5)]]`）| `phase = 2π·cfo·epochs[sym]` |
| K0-a 导频（三个 kernel）| `ocudu_mmse_pilots.metal:172 / 231 / 561`（buffer 1/2/3）| CFO 的相位斜坡（`epochs[b]-epochs[a]`）|

### 4.3 推荐做法（最小改动、单一实现）

1. **不要**再传表。把**生成参数**传进去：`cp`（normal/extended）+ `scs`，让每个 kernel 用一个小循环
   在自己的 threadgroup/寄存器里算出它需要的少数几个 epoch 值：
   ```
   // 宿主公式（port_channel_estimator_average_impl::initialize_symbol_start_epochs），单位=符号：
   //   e[0] = cp_len(0, scs)[秒] * scs_khz * 1000
   //   e[i] = e[i-1] + cp_len(i, scs)[秒] * scs_khz * 1000 + 1.0
   // normal CP：cp_len(0) 是长 CP（每半帧首个符号），i>0 用短 CP；extended 只有一种。
   ```
   ⇒ kernel 只要 `{cp_is_extended, cp0_len_symbols, cp_len_symbols}` 三个标量（或等价的两个长度 +
   一个标志），就能在 ≤14 次迭代内算出任意 `sym` 的 epoch（编译期上界 ✓）。
2. **或者**（更省事但不如上面干净）：把 14 个 float 放进 kernel **参数块**（setBytes，56 B）——
   与 5f-1 的做法一致（参数=控制，不计入 crossover）。**两者都可以**；选 1 的理由是"设备自算"
   更彻底，选 2 的理由是改动最小（只动参数结构，不动公式）。
   **⚠ 无论选哪个，都必须删掉 `gpu_epochs` 那次 `count_host_write_site("ce: symbol start epochs uploaded")`
   调用**（就是它让契约停在 7/8）。
3. 判据：
   * 单测：CE 单测（`OCUDU_CE_TA_CHAIN=1`）+ 等化器单测（两种模式）全过；
   * 离线：27 捕获 `ul_chain_replay` 四个 dump 与当前 HEAD 逐字节相同（`_llr` 是 CFO 补偿敏感项）；
   * 仪表：replay 的写侧**分项表为空**（`<no host write was attributed to a site>`）、写次数 **0/跳**；
   * 空中：契约 **8/8**（`host device data crossings -> OK`），其余指标不退。

### 4.4 验收后的收尾

* 若契约 8/8：更新设计文档 §19.0 表 + §5 批次表（5g ✅）、交接 §1，**然后可以开始 5c 之后的收尾**
  （读侧+写侧都清零 ⇒ 车道的"两个 crossing"判据**第一次完全达成**）。
* 剩余诚实清单见 §8（稀疏掩码、>2048 变换、分裂尾块、`nof_layers>1`、K5 只回归过 2 种形状等）。

### 4.5 ✅ 已完成（`45fa002d6c`）+ **只剩空中腿**

**提交**：`45fa002d6c`（设计文档 **§19.6** 是完整记录：做法、判据、1-ULP 追查链、三个测量工具的坑）。

**做成了什么**：

| 项 | 结果 |
|---|---|
| 写侧 | replay 27/27 捕获：`1.00 → **0.00** write(s) per hop`，分项表 → `<no host write was attributed to a site>` |
| 四个 dump | 与前一版（HEAD 二进制 + HEAD kernels，两边 metallib 都钉住）**逐字节相同 27/27** |
| epochs 正确性 | GPU 探针全定义域 **140/140** 逐位相同；每条腿日志有 `[epoch_check] … 14 of 14` |
| 单测 | CE `All tests PASSED`（`OCUDU_CE_TA_CHAIN=1`）+ 等化器两模式 `ALL OK`，`recoveryCount=0` |
| 自证行 | `[epoch_impl] symbol start epochs: nothing is uploaded; K0-a's CFO kernels derive them on the device …, while K4 and the CFO estimator take them as parameters` |

**★ 5g 的硬教训（下次直接照做）**：把"设备自算"塞进一个**输出出自长累加**的 kernel 之前先问一句——
它的发布位对**编译形状**敏感吗？是的话给它**参数**，不要给它新算术。本轮实证：
CFO 估计动 1 ulp（`0x3BFB5F2E → 0x3BFB5F2F`，`_h.bin` 2/27 各差 14 B、`rsrp` 9/27 差第 8 位）、
K4 噪声方差动 2 ulp（`_ce.txt` 3/27）。两处都改成收参数后 27/27 全同。

**★ 空中腿已跑：`5g-epochs_0919_1945`（`45fa002d6c`，~73 s，2 次 UE attach，3576 次 PUSCH）**

| 判据 | 结果 |
|---|---|
| **契约** | **8/8 MET（第一次）**：`host device data crossings -> OK`，读 0.00 + 写 0.00/跳，分项表空 |
| 采样 / assembly | 0 gaps over 69549 blocks；973602/973602 in-place（0 拷贝）|
| RT failure | **0 / 69549 slots** |
| CRC | 2832/3576 = **79.19%**（5e 79.24%）；分层同型（24 PRB 97.20% vs 97.95%，25 PRB 97.09% vs 89.78%，宽度 2 两腿都 0%）|
| busy split | ch_est 118.7 / **ch_wt 379.9** / eq_demap 101.1 µs（5e：115.8 / 385.9）|
| 设备侧 | lanes 3576（cbs/lane 3.38，dropped 0）；corr_build_fail 0、y_write_fail 0、eq staged 0 |
| 自证 | `[epoch_impl]` ✓；`[epoch_check] numerology=0 cp=normal 14 of 14` ✓；无任何 MISMATCH/告警 |

⇒ **本批（5g）结束，且 5f-1 的空中判据一并补上。** 后续可选工作见 §8。

**原计划的这一腿判读要点（留档）**：

1. `[epoch_impl]` + `[epoch_check] … 14 of 14` 两行都在（自证）；
2. **契约第一次 8/8**：`host device data crossings -> OK`（读 0.00 + 写 0.00/跳），
   契约行下面**没有**分项表（`<no host write …>`）；
3. `radio sample continuity` 0 gaps、`host sample assembly` 全量；
4. CRC 按分配宽度分层看（对照 5e 腿 79.24% / 5d 腿 81.23%）；
5. RF failures 与上几腿同量级；`busy split` 的 `ch_est/ch_wt/eq_demap` 无新回归
   （5e 腿：115.8 / 385.9 / —）。

---

## 5. 本轮的架构现状（改代码前必读）

### 5.1 融合车道的互联（谁在哪条队列/命令缓冲）

* 前端 DFT（Rx）走**front-end queue**；估计器/等化器/解调器走 **back-end queue**。
  **两个队列之间没有顺序** ⇒ 需要顺序的东西必须在**同一条命令缓冲**里。
* 车道的 stage：`dft` → `ch_est`(提取) → `ch_wt`(重放+权重+TA) → `eq_demap`（burst）。
  `busy split` 看到三段基本串行（和 = busy）。

### 5.2 本轮新增的"设备自算"部件（三个，都遵循同一模式）

| 部件 | kernel | 输入（都是每跳已有的几何/参数）| 输出（设备缓冲，引擎持有）|
|---|---|---|---|
| TA 链 | `mmse_ta_chain`（`ocudu_mmse_ta.metal`）| `h` + `hop_geometry` + 变换尺寸/twiddle/perm | 1 个 float（秒，页对齐旋转槽）|
| gather 表 | `eq_build_gather`（`ocudu_equalizer.metal`）| `rb_words/…/active_re(_dmrs)` | tap 表 + entry 表（一次分配 ≈370 KB）|
| `h_starts` | **无 kernel** | `equalize_strides` 参数块（14×u32）| — |

**共同纪律**：**同一份映射只允许一个实现，或两个实现必须逐字节对拍**——
`ocudu_dft_butterflies.h`（蝶形共用）、`OCUDU_EQ_TABLE_CHECK`（gather 表对拍）、
`OCUDU_CE_TA_CHECK`（TA 对拍）都是这条纪律的产物。**新加一个"设备自算"部件时，先加对拍。**

### 5.3 旋钮一览（默认值 = 出厂行为）

| 旋钮 | 默认 | 作用 |
|---|---|---|
| `OCUDU_CE_DEV_STATS` | 1 | 设备侧 hop 统计（rsrp/K4/TA）总开关 |
| `OCUDU_CE_HOST_GRID` | **0** | 1 = 强制回读网格（A/B 与两个探针需要它）|
| `OCUDU_CE_DEV_TA` | 1 | 0 = 宿主自算 TA（A/B 另一臂）|
| `OCUDU_CE_TA_CHECK` | 未设 | 同跳跑宿主估计器逐跳对拍（`[ta_check] total:`）|
| `OCUDU_EQ_DEV_TABLES` | 1 | 0 = 宿主建 gather 表（A/B 另一臂）|
| `OCUDU_EQ_TABLE_CHECK` | 未设 | 设备表 vs 宿主表逐元素对拍 |
| `OCUDU_EQ_DEFER_ENCODE` | 未设 | 等化器单测里打开**批处理**路径 |
| `OCUDU_CE_RSRP_CHECK` | 未设 | K5 探针（**需要 `HOST_GRID=1`**）|
| `OCUDU_CE_DEV_Y` / `OCUDU_CE_NO_K4` / `OCUDU_CE_CPU_LS` / `OCUDU_CE_LANE_ORDER` | 见代码 | 各阶段的 A/B 逃生门 |

---

## 6. ★ 硬纪律（违反过、代价很大）

1. **GPU 挂死**：kernel 的**每个循环上界必须是编译期常量**、参数只能 break/skip、**每个索引都要夹紧**、
   **不许读 `[[threads_per_threadgroup]]`**、**barrier 前不许早返回**（`full_gpu_chain` §48.131(c)）。
   挂死后 `kill -9` 不释放 GPU、WindowServer 看门狗会**整机卡死**、`reboot` 也可能挂 ⇒ 只能断电（§48.133）。
   **跑任何 dispatch 的二进制前**：限时（`/tmp/limited_run.sh <秒> <日志> <命令…>`）+ 跑完查
   `ioreg -r -c IOAccelerator -d 1 | grep -o 'recoveryCount"=[0-9]*'`。
2. **陈旧产物**（本轮又踩了一次）：
   * 改 `.metal` 后 **先删 metallib** 再 build：`rm -f <dir>/ocudu_*.metallib`；
   * **改完库要重建"你要跑的那个二进制"** —— 本轮改了等化器库却跑了旧的 `ul_chain_replay`，
     差点得出错误结论（"写入还是 5/跳"）。**核 mtime 或 `--target` 显式重建**；
   * 跑腿前 `build/hashes.h` 的戳必须 == HEAD（`touch build/hashes.h && cmake --build build --target gnb`）。
3. **腿的有效性**：先看 `radio sample continuity`（0 gaps）与 `host sample assembly`（全量）；
   采样连续性坏掉的腿，其 CRC/契约数字一律不能当证据。**CRC 要按分配宽度分层看**
   （聚合值会被流量结构骗：5b 腿 66.63% 而同一分钟是 73.90%）。
4. **别造新二进制**：新校验挂在已有 harness 上（5a 的教训）。
5. **`git reset --hard` 前先备份工作树**：`bash doc_chinese/phy_pipeline_gpu/wip/backup_worktree.sh <label>`
   （本轮已备份 `2026-09-19_5e-air`、`5e-provenance`、`5f-part1` 等）。
6. **"XX 非空"当代理判断**是危险模式（5e 的坑 2）；**换了实现之后要重审每个代理判断**。
7. **给"输出出自长累加"的 kernel 加新算术 = 可能动发布位**（5g 实证：CFO 1 ulp、K4 2 ulp）。
   这类 kernel 要**收参数**，不要给它新代码；判据是"四个 dump 逐字节相同"，不是"值看起来对"。
8. **metallib 是运行时文件**（configure 时写死的源码树路径）：比较两个二进制时必须给两边各自钉住
   kernels（`wip/ab_replay_bins.sh` 的 `AB_METALLIB_A/B` + `[epoch_impl]` 配对断言）；
   改 `.metal` 后 `rm metallib` 与 `cmake --build` 要在输出里看到 **Linking Metal library**。

---

## 7. 常用命令

```bash
# ---- 构建（总是显式 --target）----
cmake --build build --target gnb                       # 腿用；跑前 touch build/hashes.h
cmake --build build --target ul_chain_replay           # 离线 A/B 工具（--metal）
cmake --build build --target port_channel_estimator_metal_mmse_unit_test
cmake --build build --target channel_equalizer_metal_unit_test

# ---- 单测：现在都注册进 ctest 了（`8c87761441`），`make test` 就能跑全 ----
cd build && ctest -L phy                      # macOS 172 个（含 8 个新增 Metal 用例）；Linux 164 个
ctest -R "metal_unit_test|ofdm_demodulator_metal_batch"   # 只看 GPU 这 8 个（约 15 s）
# 8 个用例 = 6 个二进制 + 两个"深模式"变体（下面这两行以前只能手跑）：
#   port_channel_estimator_metal_mmse_unit_test_ta_chain     ENVIRONMENT OCUDU_CE_TA_CHAIN=1
#   channel_equalizer_metal_unit_test_defer_encode           ENVIRONMENT OCUDU_EQ_DEFER_ENCODE=1

# ---- 手工单测（要单独跑某个二进制时；都限时，跑完查 recoveryCount）----
UT=build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
OCUDU_CE_TA_CHAIN=1 bash /tmp/limited_run.sh 420 /tmp/ce.log $UT      # 期望 All tests PASSED
EQ=build/lib/phy/upper/channel_processors/metal/channel_equalizer_metal_unit_test
bash /tmp/limited_run.sh 300 /tmp/eq.log $EQ                          # 默认（逐符号）
OCUDU_EQ_DEFER_ENCODE=1 bash /tmp/limited_run.sh 300 /tmp/eqb.log $EQ # 批处理路径

# ---- 离线 A/B（27 捕获，判据：四个 dump 逐字节相同）----
bash doc_chinese/phy_pipeline_gpu/wip/ab_ta.sh          # TA 三臂（探针/宿主/默认）+ 写不了 HOST_GRID
# 两个 BINARY（新旧代码）的对比：metallib 是运行时文件，两边必须各自钉住（5g 的新工具）
AB_METALLIB_A=/tmp/mmse_head.metallib AB_METALLIB_B=/tmp/mmse_new.metallib \
  bash doc_chinese/phy_pipeline_gpu/wip/ab_replay_bins.sh /tmp/replay_head \
       build/lib/phy/upper/channel_processors/metal/ul_chain_replay
# 手工比较某条腿的两臂（替换 <KNOB>）：
for cap in doc_chinese/work_tmp/corpus/*.bin; do ...; done   # 见 ab_ta.sh 的写法

# ---- 空中腿（需要用户手机 ping/iperf3；Ctrl-C 停，不要 kill）----
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> [OCUDU_*=v …]
# 日志：doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_<label>_<date>.log{,.stderr,.stdout}
```

**看腿的顺序**：`[ta_impl]`/`[eq_impl]` 两行自证 → `radio sample continuity` / `host sample assembly`
→ 契约那 8 项 + **写侧分项表** → `busy split`（`ch_est/ch_wt/eq_demap`）→ CRC（**按宽度分层**）→
RF failures（与上几腿同量级）。
</br>

---

## 8. 诚实清单：已知边界与未做项

| # | 项 | 现状 |
|---|---|---|
| 1 | **契约 8/8** | ✅ **已达成**（`5g-epochs_0919_1945`，§4.5）|
| 2 | 5f-1 的空中腿 | ✅ **已补**（同一腿，该二进制包含 `9e36fef3ed`）|
| 3 | TA 的 ~35 µs 尾延迟 | **接受**；若要做见 §18.7（fence + 单独命令缓冲，会引入间歇性错值风险）|
| 4 | 稀疏 RB 掩码（非连续分配）| 设备 **不覆盖** ⇒ 回退宿主（连带保留那一跳的回读）|
| 5 | 变换尺寸 > 2048 | 融合 TA kernel 的线程组内存上限；`get_idft()` 最大正好 2048 ⇒ 生产够用 |
| 6 | 分裂尾块几何 | 重放本身就不挂，TA 自然不挂 |
| 7 | `nof_layers > 1` | 只在单测里出现过（几何带每层 comb）|
| 8 | 5a 的 K5 加固 | 只在 `syn025_25`/`syn004_4` 上回归过，另外 5 种形状没跑过 |
| 9 | **批处理 + 宿主 staging 估计** | 本轮**刚修好**（`9e36fef3ed`）；在这之前所有此类路线都错，历史数据若用到这些路线需重新审视 |
| 10 | `ch_wt` 比 5a 高 ~35 µs | 已定位为 TA 尾延迟；如果将来车道缺余量，§18.7 是现成方案 |

---

## 9. 本轮踩过的坑（按价值排序，下次别再踩）

| # | 坑 | 症状 | 正解 |
|---|---|---|---|
| 1 | **融合 kernel 里摆放必须过 `perm` 表** | 每个时延读回 ~0（差 200–530 ns）| `dft_dit` 的蝶形假定输入**已是数字反转序**；自然序是**输入缓冲**的序，不是**蝶形**的序 |
| 2 | **`eq_build_gather` 的 `dest` 写成"PRB 内序号"** | 每个 PRB 盖掉第一个 PRB 的槽位；`_llr` 差 3% 而 `.bin` 不变 | `dest` 是**符号内**序号：`rank*n_act + d`。**设备/宿主逐字节对拍第一次跑就抓到** |
| 3 | **"宿主 blob 非空"当"表已建"的代理** | 设备路径不填它 ⇒ **gather dispatch 根本没编码** | 判据改成"两个 buffer 非 nil"；builder 失败时清 `valid` |
| 4 | **既有缺陷：staging 的批处理 run 用错起始** | 12 符号批处理 11/12 错，错的符号值相同 | staged run 用 `k*h_stride`，device run 用估计器绝对起始（**先在基线复现再改**）|
| 5 | **陈旧二进制**（改了库、跑了旧工具）| "写入还是 5/跳"的假结论 | 显式 `--target` 重建**你要跑的东西** |
| 6 | **探针的时机**（读设备表太早）| 设备表全 0，报告"全错" | 表只在**命令缓冲完成后**读（引擎析构/下一跳之后），不要在同一跳的两次 flush 之间读 |
| 7 | 两次 `memoryBarrierWithScope` 与 32 KB 线程组内存 | 被当成 50 µs 的元凶 | **两次实验都否掉了**：代价是"把短链挂在命令缓冲末尾"的**尾延迟** |

---

## 10. 关键文件（本轮新增/改动）

**新增**
* `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_epochs.h` — **5g**：符号起始时刻的
  宿主/设备共用实现（纯 C+++MSL），带完整"为什么逐位相同"的证明
* `doc_chinese/phy_pipeline_gpu/wip/ab_replay_bins.sh` — **两个二进制**（各配自己的 metallib）的
  27 捕获逐字节对比 + 写侧仪表 + `[epoch_impl]` 配对断言
* `lib/phy/generic_functions/metal/ocudu_dft_butterflies.h` — 蝶形共用（`dft_dit` + `mmse_ta_chain`）
* `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_ta.metal` — K6/K7/**`mmse_ta_chain`**
* `doc_chinese/phy_pipeline_gpu/wip/ab_ta.sh` — 27 捕获 TA/`HOST_GRID` 三臂 A/B

**主要改动**
* `ocudu_metal_mmse_engine.{h,mm}` — `hop_geometry`、`encode_ta`（一条 dispatch）、`run_ta_chain`、
  `build_ta_tables`、启动打印 `[ta_impl]`
* `ocudu_mmse_rsrp.metal`（K5 加固）、`ocudu_equalizer.metal`（**`eq_build_gather`** +
  `equalize_strides::h_starts`）、`ocudu_equalizer_metal_engine.mm`（设备表、`[eq_impl]`、
  staged-run 起始修复）
* `port_channel_estimator_metal_mmse_impl.{h,cpp}` — `ta_attach_stage`、写侧分项仪表、
  `OCUDU_EQ_*` 无关；`port_channel_estimator_average_impl.{h,cpp}` — `get_device_ta_seconds()`、
  设备 `noise_variance` 优先；`include/.../time_alignment_estimator.h` — `get_idft_size()`
* `include/ocudu/phy/phy_pipeline_crossings.h` — **按站点记账**（`count_host_write_site` + 分项表打印）
* `include/.../channel_equalizer_device_grid.h` + `.cpp` — 计划携带 `geometry`

**文档**：设计文档 §18（阶段总结）、§19（写侧 5e/5f）；本文件；`wip/S12_batch5b_ta.md`（5b 细节）。
