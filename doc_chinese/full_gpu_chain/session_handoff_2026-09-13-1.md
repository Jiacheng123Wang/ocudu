# 交接：GPU-PHY 全链会话状态（2026-09-14，供新会话接续）

> **本文是本次会话切换的快照（只读，不回填）**。活文档是 `s2_full_chain_design.md`（所有事后更正写那里）。
> 本 memo 目标：让新会话**不需要本会话的任何上下文**即可继续。

---

## 0. 一句话现状

`apple-silicon` 分支 HEAD = **`46aff154d7`**（工作树干净，`lib/` 0 改动；只有用户的 `configs/*`、`scripts/switch_gnb_plmn.sh` 与若干 `.csv` 是本地未跟踪改动，**不要动、不要提交**）。
本轮已完成并推送：K1 分块重写（求逆回 GPU，91.3 → 24.5 µs，CE 默认走 GPU 求逆）、warp 版求逆（**修好但不更快**，未进主干）、Test 10（按尺寸的求逆回归测试）、批量均衡器的后端化尝试（提交了一部分、默认关闭）。
**未完成、下一步要做**：① S-5c-2（一个 hop 一条 command buffer）；② 之后的深水区 S-5c（CE 输出留 GPU、与 eq/demap 共享 burst、零拷贝）。

---

## 1. 用户 vision 与优先级（2026-09-14 口述，已写入活文档文首 MEMO）

- **最终目标**：RF **I/Q samples → MAC PDU 全链条 GPU Metal path**。
- **已规划的回退（次优）方案**（因为 LDPC 是硬骨头）：I/Q 到达后 **CPU thread 无阻塞 dispatch 给 GPU**，一路到 **LLR**，再回到 **CPU 另一个 thread 做 CPU LDPC 译码**，继续走 **FAPI 到 MAC**。与全 GPU 方案**只是 CPU/GPU 切换点不同**。
- **现阶段主任务**：**集中优化 LDPC 之前的 GPU pipeline**，使 **IQ → demod LLR** 这一段在 GPU 上全链条最优（零拷贝、单 command buffer、少等待、必要时单例引擎/跨端口批处理）。
- **LDPC 暂缓**：前期已投入大量精力；主要怀疑 **3GPP LDPC 设计本身面向串行计算**（双对角/分层迭代依赖链），GPU 上"逐 TB dispatch"天然吃亏，待研究。**在得出结论前，LDPC 相关优化项一律不作为行动项**；文档里的 LDPC 数字（`ul_ldpc_decode` median ~2 ms）只作预算背景。

---

## 2. 工作方法与硬规则（必须遵守）

1. 每步 = **代码 + 构建 + 单测 + A/B 门禁 + commit + push**；提交信息前缀 **`GPU-PHY S-<id>:`**，**禁止**在提交信息/代码注释里引用 `doc_chinese/`。
2. Backend 选择只通过 `expert_phy` CLI（env 变量只用于 debug/A-B）。
3. **TX/DL 保持 CPU**，不动。
4. `doc_chinese/full_gpu_chain/` 下文档**不提交**（git-ignored）；`s2_full_chain_design.md` 是**唯一活文档**，`session_handoff_*.md`/`pipeline_audit_*.md` 是**只读快照**。
5. E2E（OTA/ping）由用户跑；本地能做的一律本地门禁；**需要 OTA 时明确告知用户**。

**常用门禁命令**：
```bash
cd /Users/jiachengwang/dev/ocudu && cmake --build build -j8
ctest --test-dir build -L phy -j8 | grep -E "tests passed|tests failed"     # 期望 161/161
# 10 个独立 metal 测试二进制（不走 ctest），逐个跑：
for f in $(find build -type f -perm +111 -path "*metal*" \( -name "*_test" -o -name "*probe*" \) | grep -v dSYM | sort); do echo "== $f"; $f 2>&1 | tail -2; done
OCUDU_CE_LEVEL_STRICT=1 ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test | tail -1
./build/tests/unittests/phy/upper/channel_processors/pusch/pusch_demodulator_deferred_chain_test   # 3/3，含真实 Metal 后端
```

---

## 3. 下一步任务 ①：S-5c-2 —— 一个 hop 一条 command buffer（尾块并入标准批次）

**目标**：CE 现在 per hop 发 2 次引擎调用（标准块 + 尾块）⇒ 2 条 CB、2 次 commit/wait。合并成 **1 条 CB / 1 次等待**，同时减少 OTA 上看到的"排在 LDPC 后面等 GPU"的暴露（`[mmse_time_sum] gpu_wait` 实测 108 µs）。

**数学方案（已验证推导）**：把尾块当作标准批次的**额外 system**，槽步长按标准批次（`L_stride=L_std`, `nout_stride=nout_std`），**A 做成块对角 `[A_e, I]`**（pad 区域由调用方清零、pad 对角线置 1）⇒ `W = [R_hp_e·A_e⁻¹, 0]`、pad 输出恒为 0，与单独调用**数学等价**（不要求逐位一致）。

**实施清单（上次已写过一版，回退了；照此重做约 30–60 分钟）**：
1. 拆函数：`run_engine_blocks` → `stage_engine_blocks`（只暂存）/ `engine_batch`（只发一次引擎调用）/ `unpack_engine_blocks`（只解包）。文件：`lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.{h,cpp}`。
2. 新增 `struct engine_slots {a,r_hp,w,y,h,qy;}` + `MAX_ENGINE_BATCHES = 2` + `slots_for(region)` + `clear_engine_slots()`；构造函数里按 `MAX_ENGINE_BATCHES × region_elems_*()` 分配。
   **区域偏移必须页对齐**（`region_elems_x()` 用 `compat::page_size()` 向上取整到页的**元素数**）；否则 `newBufferWithBytesNoCopy` 失败并**每次退化成拷贝**（实测慢 4×，且结果可能不对）。
3. `stage_engine_blocks(args, gb_start, n_blk, b_prb, nout, L, npt, matrix, sys_offset, L_stride, nout_stride, n_blk_stride, slots)`：
   - 槽步长 `Ls/Ns`：`(L_stride?L_stride:Lp)`、`(nout_stride?nout_stride:Np)`；所有**槽地址**用 `Ls/Ns` 与 `sys_offset+sys`；
   - A 的 **pad 对角线置 1**（`for k in [L, Ls): slot[k*Ls+k] = 1`）；
   - **y 寻址的 `n_blk_stride` 回退**：`blk_stride = n_blk_stride ? n_blk_stride : n_blk`，地址 `slots.y + ((sys_offset+i_layer)*blk_stride + b)*2*Ls`。
4. `unpack_engine_blocks(gb_start, n_blk, b_prb, nout, nout_stride, nof_layers, nof_blocks_stride, slots)`：
   **寻址必须用 `nout_stride` / `nof_blocks_stride`**：`hp = slots.h + ((i_layer*nof_blocks_stride + b) * 2 * nout_stride)`。
   （上次的合并路径 -9.38 dB 就是这里用了 `nout`/`n_blk` 读错行 —— 这是**最关键的已知 bug**。）
5. 调用方顺序（`cpp` 里 `apply_fd_td_estimation_stage` 的 engine 段）**必须**：
   **暂存标准块 → 构建尾块相关矩阵 → `clear_engine_slots(tail_slots)` → 暂存尾块 → 一次 `engine_batch(std_slots, nout_std, L_std, n_std_blocks, 2*nof_layers, matrix_on)` → 解包两者**。
   ⚠️ 尾块与标准块**共用** `w_r_pp/w_r_hp`；顺序写反会让标准块被尾块矩阵暂存（上次最隐蔽的 bug，症状：Test 6 NMSE 从 -17.33 掉到 -4.31/-5.47 dB）。
6. 合并门槛：`rem_prb != 0 && n_std_blocks != 0 && !matrix_on && (2*nof_layers <= MAX_LAYERS(=4))`；`OCUDU_CE_SPLIT_TAIL=1` 强制旧路径（A/B 用）。旧路径保留（matrix 风格、单尾块 hop、层数超限时用）。
7. **门禁（升级）**：以 **split 路径为参照**，`merged` 必须与 `split` 的 NMSE/hop 时间等价（split 已证明与基线等价：同一二进制下同为 -15.52 dB / 同时间）。
   建议流程：`./..._test | grep -E "Test 6 \(25 PRB|mean total"`，再 `OCUDU_CE_SPLIT_TAIL=1 ./..._test | grep ...` 对比；随后跑 §2 的全部门禁。

**判定成功**：`[metal_stats] mmse_ce commits` 与 hop 数的比值从 ~1.6–1.7 降到 ~1.0（注意：该计数包含单测里 Test 5 的批量调用，**只做相对比较**）；`gpu_wait`/hop 时间不劣化；Test 6 NMSE 与 split 路径一致。

---

## 4. 之后的任务 ②：深水区 S-5c（CE 输出留 GPU、与 eq/demap 共享 burst、零拷贝）

现状与阻塞点（详细分析见活文档 §28）：
- 每 hop：CE 1–2 条 CB（含 host 等待）→ host 解包 `grid_est` → 统计量（RSrp/noise/TA）与下一跳 LSE 导频；然后 demodulator 的 deferred 组（K=14）把 eq+demap 编码进**另一条** CB（`[metal_stats] burst commits≈组数`）。
- 要融合，必须让 **CE 的 dispatch 与 eq/demap 同属一条 CB**，而这要求 **CE 的 host 往返消失**：
  - eq 需要的是"按符号压缩后的 cbf16 信道估计"，而 CE 的 GPU 产物是 per-block 布局 ⇒ 需要**一个 GPU 侧的重排/压缩 dispatch**（新 kernel），或让 K2 直接写成 eq 想要的布局；
  - host 仍需要网格的一部分（DMRS 位置的 LSE 导频 + 统计量）⇒ 可从 GPU **只回传 DMRS 子集**（小）而不是整网格；
  - 接口层要动：`channel_estimator` 需要在设备侧交接（或 PUSCH processor 改流程），`est_results` 的构造要能引用 GPU 张量。
- 建议顺序：先把 **S-5c-2（一条 CB/hop）** 做完 → 再做"**CE 输出零拷贝给 eq**"（新增 CE→eq 的重排 dispatch + eq 直接读设备缓冲）→ 最后把 CE 也并入 slot burst。

---

## 5. 未决/已知坑（务必先读，避免重走）

1. **环境不可复现（重要）**：同一份**未改动**代码，不同时间测出：`Test 4a engine.invert()` GPU **24.5 → 43.8 µs**；Test 6（25 PRB/2 DMRS）NMSE **-17.33 → -15.52 dB**；hop **231 → 281 µs**；CE `gpu_wait` **26.5 → 61.9 µs**；日志**无 CPU 回退**、工作树干净。
   ⇒ **任何 A/B 或 NMSE 对比前，先在同一二进制上连测两次确认可复现**；怀疑与"被 `kill -9` 掉的 Metal 进程残留/GPU 负载"有关。
2. **NaN 吞掉误差**：测试里用 `std::max(err, |a-b|)` 比较时，NaN 会让 `max` 保持旧值 ⇒ 看起来"误差 0.0"。S-5c-1/warp 两个 bug 都被这个假象掩盖过。比较误差时用 `if (!(err < tol))` 或显式查 NaN。
3. **S-5c-1（批次 API）卡死未解**：`begin_batch()/commit_batch()`（`run*()` 加 `wait=false`）实现后，在第 **1431** 个 hop 卡在 `-[MTLCommandBuffer initWithQueue:]`（队列 in-flight 槽耗尽）。计数器不自洽（hop 1400 时 created≈1920 / waits≈2978，wait 计数混入"等待他人提交的 CB"的路径）。**根因未定位**。
   ⇒ S-5c-2 的设计**刻意不用批次 API**（一次普通引擎调用即一条 CB），因此不引入这个风险；若要回到批次 API 路线，先做：给 CB 挂 `addCompletedHandler` 精确统计 in-flight，或临时调大 `maxCommandBufferCount` 区分"泄漏"与"GPU 侧不完成"，并逐个核对所有 commit 路径（含 `invert/apply`、warm-up、CPU 回退分支）。
4. **批量均衡器（S-4h/4i/4j）**：`channel_equalizer::submit_group()` + Metal 的 run 划分（`equalize_mxn_batch`，按几何分 run，单符号 run 走原路径）已提交并有单测（5 符号 nof_re{128,128,96,128,128} → 2 次批量 dispatch，逐位一致）；`shared_burst` 的 **flush hook**（`set_flush_hook`）也已提交。**默认关闭**（`OCUDU_EQ_DEFER_ENCODE=1` 才启用），因为真实链路里延后编码会发散：
   - 17 PRB Metal 用例（4896 LLR）：**同管道 + 立即编码 = 0 差异**；延后编码整组一次 dispatch = **4485 差异**；延后编码**关掉批量**（每符号一次 dispatch）= 1442（组=2）/2512（组=4）。
   - ⇒ 缺陷是"**编码时机**"（把均衡的编码从 `submit()` 挪到下一 stage 首次编码时），**不是**批量核、也不是 burst 管道。均衡器输出本身两种形态都正确；隔离单测里 12 符号批量逐位一致。
   - **下一步实验（若回头修）**：把 flush 提前到 **pass 1 结束**（接口加一个轻量"组结束"通知，如 `channel_equalizer::submit_group_end()`，默认空实现）；若那时结果正确，根因就是"demapper submit 与 eq 编码的相对顺序/可见性"。
5. **K1（求逆）现状**：分块 Gauss-Jordan（b=8，无选主元）**24.5 µs/单个 36×36**（原 91.3 µs），误差 3.92e-07；线程组几何默认 **(64,16)**（`OCUDU_INV_TGX/TGY` 可复测：(32,4) 91.3 / (64,8) 25.7 / (64,16) 24.5）。
   - **warp 版（寄存器内 8×8 求逆 + `simd_shuffle`/threadgroup 广播）已修好但不更快**：simd_shuffle 28.4 µs、threadgroup 广播 33.2 µs（都正确，误差 3.92e-07），未进主干。**bug 的正确形式**：消元乘数必须是"**本行**在枢轴列的取值 `aug[p]`"，且广播的必须是**已归一化**的枢轴行（我原先用枢轴元素值当乘数 ⇒ NaN）。
   - 剩余 ~15 µs **不在枢轴相位**，而在每块列的三个**全线程组相位**（应用 D⁻¹、保存乘数、rank-8 扫描）与 32-warp 屏障代价上。
6. **CE 默认值**（A/B 开关）：GPU 求逆**默认开**（`OCUDU_CE_CPU_INVERT=1` 走 CPU Gauss-Jordan）；尾块走引擎**默认开**（`OCUDU_CE_TAIL_CPU=1` 走 CPU 兜底路径）。
7. **观测/调试开关**：`OCUDU_MMSE_DEBUG=1`（引擎相位：wrap/cb/encode/commit/wait）、`OCUDU_CE_LEVEL_STRICT=1`（Test 9 变成断言）、`OCUDU_INV_SYSTEMS`、`OCUDU_PUSCH_FORCE_SERIAL`、`OCUDU_PUSCH_DEFERRED_GROUP`（组大小，默认 14）、`OCUDU_EQ_GROUP_SUBMIT`、`OCUDU_EQ_DEFER_ENCODE`、`OCUDU_DFT_PIPELINE_DEPTH`。
8. **日志**：不要开 `log.all_level: debug`（实测 ~4000 行/s 会拖死控制台并污染计时）。UL CRC / PUCCH 那些行属 debug 级 ⇒ **只把需要的层开 debug**（如 `log.phy_level` / `log.fapi_level`）。
9. **离线工具**：`ul_chain_replay`（`--cpu/--metal/--metal-cpu-ldpc/--metal-cpu-demod`、`--td-strategy interpolate|average`、`--dft/--dft-metal`）、`scripts/ul_stage_diff.py`（分级 diff）、`scripts/gnb_log_stats.py`（日志统计；正则已修 `[+-]` 号）。采集：`OCUDU_UL_DUMP=<prefix>`、`OCUDU_UL_DUMP_LLR=1`、`OCUDU_UL_DUMP_COUNT`、`OCUDU_UL_DUMP_TD=<prefix>`。
10. **OTA 基线配置**：`tx_gain 70 / rx_gain 65 / log.all_level info / cell_cfg.pusch.max_ue_mcs 20 / dl_arfcn 430500`，`expert_phy` 带 `--pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal --pusch_dft_type metal --pusch_channel_equalizer_backend metal`。
11. **OTA 参考数（2026-09-14 全 GPU，500 ping 通过）**：`ul_pipeline` median 3031 µs；`ul_time_frequency` 231.8；**`ul_channel_estimation` median 486.5（p99 7206, max 11710）**；`ul_equalization_demod` 278.7；`ul_ldpc_decode` **1985**（背景，暂缓）；QPSK KO 7.3%（1323 blk）、16QAM 4.8%；`t=` median 2755 µs。`[mmse_time_sum] mean total=431.3 gpu_path=415.8 (gpu_wait=108.2)` ⇒ CE 的 GPU 时间主要花在**排在 LDPC 后面等 GPU**。

---

## 6. 本轮提交记录（`5df7d5ba7a` → `46aff154d7`，均已推送 `apple-silicon`）

| commit | 内容 |
|---|---|
| `5df7d5ba7a` | S-4b K1 并行化（233.9 → 91.3 µs） |
| `a52677d194` | S-4c 两路求逆实测 + `[mmse_eng]` 相位探针；结论：瓶颈是引擎调用往返（wait ~104 µs）不是求逆 |
| `d11c5c7838` | S-4d 尾块走 CPU（每 hop 省一次等待） |
| `89a2ac3c67` | S-4e CPU 尾块的 L≤36 尺寸护栏 |
| `e29725216d` | S-4f 注释改为"过渡桥 + S-5 抹掉"（架构表述纠正） |
| `f4a204c569` | S-4g 尾块**默认回到引擎**（架构一致性优先，接受 +86 µs/hop） |
| `afd3e9d187` | S-4h `submit_group()` 组接口 + Metal run 划分 + `batch_dispatch_count()` |
| `000d644098` | S-4i 组路径接线（默认关）+ 两个 wrapper 转发修复（composite/metrics） |
| `b37d07203c` | S-4j `shared_burst` flush hook + 后端累积（默认关，`OCUDU_EQ_DEFER_ENCODE=1`） |
| `9a651e4210` | **S-5a K1 分块重写（91.3 → 24.5 µs）+ CE 求逆默认切 GPU** |
| `ae78b4ed8f` | S-5b Test 10（按尺寸求逆回归）+ warp 版状态说明 |
| `46aff154d7` | S-5c Test 10 报告全部尺寸 + warp 版**修好但不更快**的结论 |

---

## 7. 立刻可执行的开工顺序（建议）

1. **环境基线**：跑两次 `port_channel_estimator_metal_mmse_unit_test`，确认 Test 4a GPU 时间与 Test 6 NMSE 可复现；记下本次基线（与 §5.1 对照）。
2. **做 S-5c-2**（§3 清单），门禁 = **merged ≡ split** + §2 全部门禁；通过后 `commit + push`（`GPU-PHY S-5d: one command buffer per hop`）。
3. **深水区 S-5c**（§4）：先"CE 输出零拷贝给 eq"，再"CE 并入 slot burst"。
4. 每完成一步：更新活文档 `s2_full_chain_design.md`（不回填本 memo）；需要 OTA 时明确告诉用户。
