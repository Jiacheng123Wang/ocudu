# Session handover #3（2026-09-14，本会话结束时的快照，**只写不改**）

> 本文件是**快照**：写下之后不再回填/修改。后续进展写进新的 handoff 或活文档
> `s2_full_chain_design.md`（§48.x 起）与 `full_chain_gpu_uma_zero_copy_refactor_plan.md`（§10.x）。
>
> 上一份快照是 `session_handoff_2026-09-14-2.md`（HEAD `2ed3fff676`）。
> 命名规则：handoff 按**日期 + 当日序号**，每个日期从 `-1` 开始；本文件是当日的 `-3`。

---

## 0. 一句话现状

**HEAD = `d01fac7a69`**（`apple-silicon`，已 push）。本会话做了四件事：
① 落地 `[ul_gpu_lane]` 探针（GPU 侧 residency/busy/**gap**/period），并用它建立了"流水线是否被喂饱"的判据；
② 收口 **demapper 批量化**（11→1 dispatch/组，页对齐 LLR 槽位）；
③④ 找到并修好**两个长期存在的 UL 缺陷**——信道估计器**边块导频偏移**（`PRB mod 3 == 2`）与
**主机 unpack 的越界写**——实网聚合 BLER **39.1% → 0.69%**、同一时长吞吐 **2×**；
最后把"融合路线"的可行性分析与下一步实施计划写进活文档（§48.47–§48.50）。

树状态：`ctest -L phy` **162/162**；工作区干净（只有用户自己的 `configs/*`）。
下一步**明确且可立即开工**：**`ch_re` gather（§48.50(e)）**，见 §4。

---

## 1. 阶段目标与硬规则（用户已明确，**不要违反**）

- 阶段目标：**融合优化 LDPC 之前的 Metal GPU PHY pipeline**；核心是让 **GPU thread group 的流水线跑起来**，
  数据流是 **IQ samples 进 → LLR 出**。**本阶段不做 LDPC 优化**。
- **流水线视角**（用户定调）：空中按符号到达；GPU 内 thread group 就是流水线；**单模块分段耗时没有意义**
  （量的是填充/staging）。允许为适应流水线改调度，但**判据必须量化**（现在用 `[ul_gpu_lane] gap`）。
- 提交信息/注释**不得引用 `doc_chinese/`**；前缀 `GPU-PHY S-<id>:`（构建修复用 `build:`）。每步提交 + push。
- 后端选择只走 `expert_phy` CLI；env 只用于 debug。TX/DL 保持 CPU。
- gNB 必须 `sudo`；**不要用 `| tee`**；每条腿自己的 `log.filename`；
  **OTA 一律由用户跑、一次一条命令**；**用户跑 OTA 时 agent 完全空闲（不编译/不测试）**。
- **用中文对话**。

---

## 2. 本会话的提交（时间顺序，接 `2ed3fff676` 之后）

| commit | 内容 | 结果 |
|---|---|---|
| `b8db30e9cc` | `[ul_gpu_lane]` 探针（residency/busy/gap/period），登记点 `shared_burst::commit()` + CE 5 处，结算点 `burst_wait_committed()` | ✅ 首次拿到 GPU 侧真相 |
| `80e131595d` | **demapper 批量化**：核加 `nof_re`+三 stride、网格改二维；`pusch_demodulator_impl` 的 LLR 改**页对齐槽位**（`llr_offset += llr_symbol_stride`）；引擎 `enqueue_burst_deferred` + flush hook；逃生口 `OCUDU_DEMOD_DEFER_ENCODE=0` | ✅ 实网 `eq_demap` 94.7→47.3µs，管线 −59µs |
| `a7787db656` | **CE 边块导频偏移修复**：`stage_engine_group` 的 `(gb_start + b)*npf` → `(gb_start + b*b_prb)*comb`；Test 11 加 23/14 PRB 形状 + 边块 NMSE 打印 | ✅ 实网 `mod 3 = 2` BLER 94.5% → **0.3%**，聚合 39.1% → **0.9%** |
| `47ca87a66e` | **主机 unpack 同类单位错误（越界写）**：`unpack_engine_group` 的 `(gb_start + b)*nf` → `(gb_start + b*b_prb)*12`；Test 11 的 NOTE **升级为断言**（边块比标准块差 >10 dB ⇒ FAIL） | ✅ 形状表 6→10，双向验证过门禁 |
| `d01fac7a69` | 清掉混进上一个提交的临时探针（`merge_geom`） | ✅ 树干净 |

---

## 3. 关键结论（可直接复用，**不必重测**）

### 3.1 两个长期 UL 缺陷（本会话最大收获；根因、证据、修法）

**缺陷 A：CE"边块"的导频切片偏移写错（`a7787db656`）**
- 位置：`port_channel_estimator_metal_mmse_impl.cpp::stage_engine_group()`（两条 staging 路径各一处）。
- 公式：`gb_start` 以 **PRB** 计、`b` 以**块**计（块宽 `b_prb`），导频视图是 `[prb][comb]`
  ⇒ 正确偏移 `(gb_start + b*b_prb)*comb`；旧式 `(gb_start + b)*npf`（`npf = b_prb*comb`）
  只在 `gb_start == 0`（标准块）或 `b_prb == 1`（1 PRB 边块）时正确。
- 触发条件：**分配大小 ≡ 2 (mod 3)**（标准块 = 3 PRB，边块 = 2 PRB）。旧式此时读到**别的 PRB 的导频**
  （16 PRB 余数时甚至越界：`24*12=288 > 150`）。
- 后果链：边块那 2 个 PRB 均衡数据错 + K4 噪声残差大 500–1000 倍 ⇒ 整个 TB 软比特被压低 ~30 dB
  ⇒ **高调制全灭、低码率 QPSK 还能过** ⇒ 实网 `mod 3 = 2` 的授权 BLER **38–94%**（b12/b13/b14 三腿一致）。
- 判据（实网日志即可算，秒级）：按 `nof_prb mod 3` 分组统计 BLER。

**缺陷 B：主机 unpack 的同一类单位错误，而且越界写（`47ca87a66e`）**
- 位置：`unpack_engine_group()`：`nf = b_prb*12` 却用 `(gb_start + b)*nf` 定位。
  边块 `gb_start = n_std_blocks*block_prb`（PRB）⇒ 23 PRB 的 hop 里算出 **504**，而网格只有 **276** 个子载波
  ⇒ **越界 `subspan`（UB）**，边块估计既没写进网格又踩到别处内存。
- 为什么实网没被拖累：真均衡器走**设备切片（K3）**路径；主机 unpack 只服务 CSI/测量/抓包与回退路径。
- 但它同时解释了"CSI 偏低"：修好后 b16 几乎全程 256QAM 而 BLER **更低**（0.69%）。

**两个缺陷的共同教训（写文档时值得强调）**：**单位/基准错误**（PRB vs 块、相对 vs 绝对 CRB）
在这条链上已经咬了三次（§48.33 的"每符号起始不均匀"、§48.44、§48.45）。改任何"偏移"公式时，
先写清**每个变量的单位**。

### 3.2 探针语义与实测基线（**别再误读**）

- `[ul_gpu_lane]`：lane = 一个线程上"上次结算之后提交的全部 CB"，结算点 = **burst 完成**（产出 LLR）。
  `residency`（首 CB `GPUStartTime`→末 CB `GPUEndTime`）/ `busy`（Σ CB GPU 时间，按阶段拆）/
  **`gap = residency − busy`（= 卡在等 CPU）** / `period`。`cbs/lane = 2.00`（CE 一条 + burst 一条）。
- **b16 基线（饱和 iperf3，`--pusch_ldpc_decoder_type auto`，HEAD 同族）**：
  `lanes=28724`；`residency 187.4 / busy 186.5µs`（`ch_est 136.5` + `eq_demap 49.9`）；
  **`gap median 0.6µs / p95 0.9µs`**；`period median 1017µs`；
  `[ul_pipeline] mean 697.7 / median 697 / p99 875µs`；
  `[ul_channel_estimation] 76.1µs`、`[ul_equalization_demod] 314.0µs`、`[ul_ldpc_decode] 85.8µs`；
  `[mmse_time_sum] total 59.8µs`（stage 17.4 + corr 10.6 + submit 5.9 + sigma2 3.1 + pre 0.8, gpu_wait 136）；
  聚合 **BLER 0.69%**、`RLF=0`、`FAPI 实时失败=0`、几乎全程 256QAM、`[ul_mac_pdu_size] total 57.5MB`。
- `[metal_stats] burst dispatches`：`demod_batch dispatches=28724`（1/组）、`eq_batch batched`（4/组）
  ⇒ burst 内 **5** 次 dispatch（批量化前 15）。
- **分段探针不能当模块耗力**：`[ul_time_frequency]`（220.9µs）量的是"14 个符号被消化的跨度"，
  `[ul_channel_estimation]`/`[ul_equalization_demod]` 是 CPU 边界。**单模块数字不要用来驱动优化。**

### 3.3 方法与陷阱（**本会话踩过，别再踩**）

1. **`metallib` 不跟着源码重编**（老坑，§48.33 记过）：改 `.metal` 后 `touch` 源文件强制重编，
   并留意输出里是否出现旧行为。
2. **`OCUDU_UL_DUMP*` 的落盘是追加模式（`fopen "ab"`）**：A/B 门禁**每次必须用新前缀**，
   并按 `[uint32 长度][数据]` **逐记录**比较，不要整文件 `cmp`（我因此差点追一个假的"4776 字节不同"）。
   另外 capture 的 key 是 `slot_rnti`，**slot 计数 10.24s 回绕** ⇒ 同一 key 会被多次追加（一个文件里有 2–5 份）。
3. **临时探针必须删净再提交**：本会话有一次把 `merge_geom` 探针提交进去，且 `.h`/`.cpp` 只进了一半
   ⇒ **那一个提交在干净检出下编不过**（`d01fac7a69` 才修好）。`git status` 里出现"只有 `.h` 被改"就是警报。
4. **`ul_chain_replay --cpu` 不是逐位 oracle**：`--cpu` 用的是 CPU 的 *average* CE，与 Metal 的 *mmse* CE 是
   不同算法 ⇒ GPU-vs-CPU 的 LLR 差异无意义（整带宽也差 ~94%）。**有效的 oracle 是 Metal-vs-Metal**
   （`--metal` vs `--metal-cpu-demod`，或 `OCUDU_DEMOD_DEFER_ENCODE=0` / `OCUDU_EQ_DEFER_ENCODE=0`）。
5. **合成几何**：改写抓包 `.txt` 的 `alloc_nof_rb`/`alloc_prb` 即可合成任意分配做 A/B（LLR 不要求信号有效），
   但**不能当作"与主机参考一致"的证明**（导频被错配，值放大）。
6. **`metal_back_ends_match_the_cpu_chain` 抓不到 CE 类缺陷**：它用 `est_results_double` 替身 ⇒ 绕开真 CE
   的设备路径；Test 11/12 才是真 CE 的门禁（但**只比 merged↔split 是盲的**，见 3.4）。
7. **Test 11 的 NMSE 门禁口径**：绝对 NMSE 取决于 bf16 与合成噪声 ⇒ 判据是**边块 vs 同 hop 标准块的差**
   （阈值 +10 dB）。形状表的块宽是 **3 PRB**（`nf_std=36`）。

### 3.4 现状清单（**已写进活文档 §48.47，直接引用**）

每 slot：**控制点 ≈16 个 CB**（DFT 14 + CE 1 + burst 1）；**等待点 3 处**（DFT 逐符号 ×14 交接、
CE 推迟等待 `cpl_wait 30.5µs`、burst 每 slot 1 次）；**GPU 等 CPU ≈0**（gap 0.6µs）；
**拷贝点**：网格/设备估计/均衡输出/**LLR 全部零拷贝**；剩下的显式 memcpy 全在 CPU 侧
（IQ→DFT 输入、导频、相关矩阵、`ch_re`）。每 slot CPU ≈400µs+ vs GPU 186.5µs。

---

## 4. 下一步（**已定，可立即开工**）：`ch_re` gather —— 照活文档 **§48.50(e)** 做

**为什么是它**（§48.50 的论证，不要重新讨论）：
1. **无循环依赖**：`ch_re` 是均衡器输入，均衡 dispatch 在**同一个 burst 里、DFT 之后**；
   CPU 已通过 `finish_symbol → wait_slot` 等过每个符号的 DFT ⇒ **不引入任何新同步**；
2. **钱的量级**：burst staging ≈**140µs/slot**（§48.47(d) 反推），主体就是这件事；
3. **门禁干净**：gather 是**纯搬运**（网格里本来就是 `cbf16_t`，**不要走 float 再回 bf16**，
   逐位一致是构造性的）。

**接缝**：`lib/phy/upper/channel_processors/metal/channel_equalizer_metal.cpp:207-213`
（`submit_batch_run` 里那两次 `std::memcpy(h_sym/y_sym, symbol.ch_symbols->get_slice(i_port).data(), ...)`；
另有 per-symbol 路径的等价 staging，见 `run_equalize`）。

**实施步骤（建议顺序）**：
1. 新 kernel（建议加进 CE/均衡任一已有 metallib，避免新 build 目标）：读**设备网格**（页对齐、no-copy wrap，
   布局 `[port][symbol][subcarrier]` cbf16）→ 按**描述符**写均衡器的 `y` staging（`[i_used][nof_re]` cbf16）。
   描述符内容：`grid_base/grid_bytes`、`port`、`first_symbol/nof_symbols`、hop 的
   `rb_mask`（`(lowest_prb, count, contiguous?)` 或 PRB 列表，**绝对 CRB**）、
   每符号的 `re_mask`（全 RE 去掉 DM-RS 梳）、`dc_position`（写零）、输出槽位与步长。
2. 加**计数探针**：gather 次数（每符号 1）、主机网格读取次数（此路径必须 **0**）。
3. 把接缝处的 memcpy 换成"登记一次 gather"，gather 编进**同一个 burst CB**（eq dispatch 之前插显存屏障）。
4. 门禁（依次跑）：
   - ① `ul_chain_replay` 在**全带宽 + 部分带宽**抓包上 LLR **逐字节一致**（`--metal` vs `--metal-cpu-demod`）；
   - ② `ctest -L phy` 162/162（含 CE Test 9/11/12）；
   - ③ `[ul_gpu_lane]`：`gap` 必须仍 ≈0（gather 会让 `busy` 略升，这是预期的）；
   - ④ 上机：`[ul_equalization_demod]`/`[ul_pipeline]` 不退化、`mod 3` 三行 BLER 不退化。
5. 提交（`GPU-PHY S-7f-x:` 或 `S-7c-x:`）+ push + 更新活文档（新 §48.x）。

**注意**：本地**唯一**能测 dispatch/等价性的手段是 `ul_chain_replay`（秒级）；上机腿只用于确认收益。

---

## 5. 环境与操作要点

- **构建**：`cmake --build build --target gnb ul_chain_replay -j 8`；全量 `cmake --build build -j 8`。
- **离线 A/B（秒级，唯一能调 dispatch 的手段）**：
  ```bash
  B=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
  rm -f /tmp/g1* /tmp/g2*                                   # 必须新前缀（追加模式!）
  OCUDU_UL_DUMP=/tmp/g1 $B /tmp/iq2_10041_17923 --metal --out /tmp/g1
  OCUDU_DEMOD_DEFER_ENCODE=0 OCUDU_UL_DUMP=/tmp/g2 $B /tmp/iq2_10041_17923 --metal --out /tmp/g2
  cmp /tmp/g1_10041_17923_llr.bin /tmp/g2_10041_17923_llr.bin   # 必须 0 差异
  ```
- **不变量计数**：`[metal_stats] burst dispatches`、`eq_batch`、`demod_batch`、`[ul_gpu_lane]`。
- **测试向量/日志（不要删）**：`/tmp/iq1_*`（187 reception，来自 `/tmp/gnb_cap.log`）、
  **`/tmp/iq2_*`（793 reception，含 186×14 PRB / 95×24 / 57×23 / 56×3，来自 b14）**、
  `/tmp/gnb_b9.log`、`/tmp/gnb_b10.log`、`/tmp/gnb_b11..b16.log`（+ 各自 `_console.log`，退出统计在 console 里）。
- **CE 单测**：`build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test`
  （Test 9/11/12；`OCUDU_CE_EDGE_DIAG=1` 打印逐 PRB NMSE）。
- **用户配置（不要动）**：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`（**无 `expert_phy` 段** ⇒ 后端必须走命令行）。
- **OTA 模板（一次一条命令，用户执行）**：
  ```bash
  cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto --log.filename /tmp/gnb_bXX.log > /tmp/gnb_bXX_console.log 2>&1
  ```
  手机侧：attach 后跑满上行 iperf3，**一直跑到 Ctrl-C**（饱和段占比越高越可比）。
- **退出统计**：`[ul_*]`/`[metal_stats]`/`[mmse_*]`/`[ldpc_*]`/`[ul_gpu_lane]` 都是 `fprintf(stderr)` 直打，
  **只在 Ctrl-C 正常退出时出现**、且**不在 `--log.filename` 里**（所以要用 `> console.log 2>&1`）。
- **健康判据**：`grep -c "Real-time failure in RF"` 个位到几十正常；上万 = 宿主机/USB 问题。
- **提交自检**：`git rev-parse --short=10 HEAD` == `strings build/apps/gnb/gnb | grep -E '^[0-9a-f]{10}$'`。

---

## 6. 待办（本会话未做，按优先级）

| # | 项 | 说明 |
|---|---|---|
| 1 | **`ch_re` gather** | §4（本会话定的下一步） |
| 2 | **K0-c/d**（统计量 + 相关矩阵在设备上算并直写引擎槽位） | 省 `corr 10.6 + stage 17.4µs/跳`；**它成立后 K0-a 才能顺带搬走**（§48.50(a) 的循环依赖） |
| 3 | **K0-a/b**（导频提取、LSE/CFO 上设备） | 单独做是**净亏**（0.46µs vs 多一次同步），必须与 2 同期 |
| 4 | **F2**：DFT 挪进 lane 队列 + 单 CB + 屏障 | 目标 `cbs/lane → 1`；前提是 1–3 |
| 5 | **F3**：ring D≥2 + push/collect + `[ul_llr_ready]` | §10.4/§10.5；`D1/D5/D6/D8` 接口已规划 |
| 6 | **LDPC 阶段** | 测试台外的真瓶颈（metal 分层核 ~3.6ms/TB）；**进阶段第一件事是量它的 busy/gap**（§10.11.2） |
| 7 | `mod 3 = 0` 的 3/6 PRB 小授权失败 | 已查明是"调度器给已消失的 UE 重传 msg3、栅格里没信号"（§48.43①），属**调度侧**，本线不动 |
| 8 | `[metal_stats] wrap creates/replaces ≈1/lane` | 待量化（不拷数据，只是每 lane 重建一次映射对象）；怀疑与"每 lane 分配宽度不同导致请求长度变化"有关 |

---

## 7. 文档地图（活文档，随时可读）

| 节 | 内容 |
|---|---|
| §45 | 现状架构基线（**分段计时口径，已被 §48.47 取代**） |
| §48.38 | `[ul_gpu_lane]` 落地（语义、实现要点、退出期 atexit×静态析构的坑） |
| §48.39 / §48.40 | lane 探针第一次实网读数（ping）/ 饱和腿里程碑（gap≈0，余量在 CPU） |
| §48.41 | demapper 批量化收口（LLR 页对齐槽位是真正的 blocker） |
| §48.42 | b13 上机验证 + 部分带宽缺陷的**发现过程**（793 抓包、区域分解、`mod 3` 判据） |
| §48.43 | **缺陷 A+B 的根因与证据链**（`mod 3 = 2` 规则在三腿成立） |
| §48.44 / §48.45 | 两个修复（导频偏移、主机越界写）+ 门禁从 NOTE → 断言的双向验证 |
| §48.46 | b16 实网确认（无退化、BLER 更低、链路自适应更自信） |
| **§48.47** | **现状清单：每 slot 的控制点/等待点/拷贝点（代码状态 `d01fac7a69`）** |
| **§48.48** | **融合路线可行性**（4 个硬约束、F1/F2/F3、跨线程三约束、五步表） |
| §48.49 | K0-a 侦察（接缝 `extract_layer_hop_rx_pilots`、缓冲布局、kernel 参数、四道门禁） |
| **§48.50** | **顺序更正 + 下一步（`ch_re` gather）的完整设计** |
