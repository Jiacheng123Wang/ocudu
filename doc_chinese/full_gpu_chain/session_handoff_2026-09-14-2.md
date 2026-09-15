# Session handover #2（2026-09-14，本会话结束时的快照，**只写不改**）

> 本文件是**快照**：写下之后不再回填/修改。后续进展写进新的 handoff 或活文档
> `s2_full_chain_design.md`（§48.x 起）与
> `full_chain_gpu_uma_zero_copy_refactor_plan.md`（§10.x）。
>
> 上一份快照是 `session_handoff_2026-09-14-1.md`（HEAD `2efa704850`）。
> **命名规则（用户 2026-09-14 定稿）**：handoff 按**日期 + 当日序号**命名，每个日期从 `-1` 开始。
> 原 `session_handoff_2026-09-14-7.md` 已更名为 `session_handoff_2026-09-14-1.md`。

---

## 0. 一句话现状

**HEAD = `2ed3fff676`**（`apple-silicon`，已 push）。本会话把 **S-7e（批量均衡）收口**：
找到并修好了实网缺陷的真根因（估计的每符号起始偏移**不均匀** + flush 基准被应用两次），
离线逐字节验证、实网 attach+ping 通过，**批量已翻为默认**（`OCUDU_EQ_DEFER_ENCODE=0` 为逃生口）。
中途引入过一次退出崩溃（把 `atexit` 探针改成 logger）已修并**整体回退**；console 输出回到
`fprintf(stderr)` 原状（用户要求保留）。**主线已切换到"GPU 融合流水线"视角**，下一步是
实现缺失的 `[ul_gpu_lane]` 探针。

树状态：`ctest -L phy` **162/162**；工作区干净（只有用户自己的 `configs/*` 与未跟踪的
`bler_metal_bg1_z64_*.csv`、`scripts/switch_gnb_plmn.sh`，**不要动**）。
`git stash@{0}: demod-batch-wip2` 存着**未收口**的 demapper 批量化 WIP（见 §4.3）。

---

## 1. 阶段目标与硬规则（用户已明确）

- 阶段目标：**融合优化 LDPC 之前的 Metal GPU PHY pipeline**。本阶段**不做 LDPC 优化**。
- **本阶段的核心任务（用户 2026-09-14 定调，极重要）**：让**GPU thread group 的流水线跑起来**，
  数据流是 **IQ samples 进 → LLR 出**。
  - 空中信号是**一个 OFDM 符号一个符号到达**的；第一个符号的 samples 收齐即可送 GPU，
    **不必等整个 slot 的 14 个符号**。GPU 内部 thread group 就是一条流水线：某个符号 FFT 完
    交给 CE，自己立刻开始下一个符号。
  - ⇒ **在 slot 时间尺度上，各功能模块（T2F/CE/Demod…）是准并行的**（依赖处才等待）。
  - ⇒ **单模块的分段耗时探针已经失去意义**（量的是流水线填充/CPU 侧 staging，不是数据流）。
    这与 `full_chain_gpu_uma_zero_copy_refactor_plan.md` §10.7（用户已定稿）完全一致。
  - 允许为适应流水线**修改调度**；判据是"某些地方调度不合理"的**量化**（见 §3.3）。
- 提交信息/注释**不得引用 `doc_chinese/`**；前缀 `GPU-PHY S-<id>:`（构建修复用 `build:`）。
- 后端选择只走 `expert_phy` CLI；env 只用于 debug。TX/DL 保持 CPU。每完成一步：提交 + push。
- gNB 必须 `sudo`；**不要用 `| tee`**（会丢退出期探针）；每条腿自己的 `log.filename`；
  **OTA 一律由用户跑，一次给一条命令**；**用户跑 OTA 时 agent 必须完全空闲（不编译/不测试）**。
- 工作方式：代码 + 构建 + 单测 + 本地 A/B 门禁 + 提交 + 文档。

---

## 2. 本会话的提交（时间顺序，接 `2efa704850` 之后）

| commit | 内容 | 结果 |
|---|---|---|
| `a6d3647fb0` | console 整理：21 处 stderr 统计改走 logger | **引入了退出崩溃**，见 §3.4 |
| `d62ef5e530` | 构建修复：①`FLOW_PROBES`×`CE_TIME` 组合可编译 ②非 Apple Silicon 上 `ENABLE_METAL_*=ON` 配置期报错 | **保留**（与输出无关） |
| `912934d25b` | 修退出崩溃：`phy_shutdown_report` 注册表 + gNB/du 显式 `run_all()` | 崩溃修复，但 §3.4 的级别问题仍在 |
| `3fcdfad81e` | 汇总级别改回 `info` | 仍不满足用户"保留原状" |
| `c2879b8940` | **整体回退** console 整理（22 处恢复 `fprintf(stderr)`、删注册表、`report()` 回原位置） | ✅ console 回到工作版本 |
| `2ed3fff676` | 删除 `[mmse_time_shape]` 的打印**及其统计收集**（用户要求，减少输出量） | ✅ |

---

## 3. 本会话的关键结论（可直接复用，不必重测）

### 3.1 S-7e 复盘的三个坑（**下次务必先排除**）

1. **`metallib` 没跟着源码重编**：一个 marker 实验（往核里塞毒值 `123456`）用 `finally` 还原了
   `.metal`，但 `ocudu_equalizer.metallib` 留在污染态，**之后好几轮读数全部不可信**。
   → 改 `.metal` 后**必须**确认 metallib 真的重编（`touch` 源文件强制），并留意输出里是否出现毒值。
2. **自制回读探针写错槽位**：核的 `eq`/`nv` 指针会按 `sym * stride` 前进，而解析按紧凑布局读，
   于是"读到全 0"这类结论反复出现且都是假的。
3. **探针自身的分片 bug**：用 `setBuffer:offset:` 把一个共享表切片给多个 dispatch 时，
   各 dispatch 读到的是**邻居的切片**（表现为"每个 run 只有第一个符号对"）。
   → 改成**每个 dispatch 一个独立的表 buffer**（几十字节）后消失。

### 3.2 实网数字的可比性（**别误读**）

b8（逐符号 22.2%）、b9（批量 25.8%）、b10（批量默认 31.9%）的 BLER 来自**不同运行**，
空中条件/调度/重传时机都不同，**不能**据此比较编码优劣。可断言的是"修复后的批量是健康的"；
**算术等价的严格证据是离线回放的 0 字节差异**。

### 3.3 流水线视角下的三个关键事实（本会话最重要的收获）

1. **分段探针的计时区间**（`ul_pipeline_probe.h`）：
   `t2f = t2f_end − slot_start`，其中 `slot_start` 是**该 slot 第一个符号 IQ 到达**、
   `t2f_end` 是**最后一个符号 DFT 完成**。所以 `ul_time_frequency=255.9µs` **主要量的是
   "14 个符号被消化掉的跨度"，不是 FFT 算力**；`ul_pipeline=746.6µs` 是**首个 IQ → LLR 可读的
   端到端填充时间**。**这些数字不能当作模块耗力来优化。**
2. **DFT 的异步流水线已经存在且正确**：
   ```
   submit_symbol:  fill_dft_input → submit_grid_write → dft->run_async(slot)   ← 不等
   finish_symbol:  dft->wait_slot(slot)                                        ← 才等
   ```
   `dft commits=248347 / 1054 slot ≈ 235 符号/slot`，`max_in_flight=8`
   ⇒ **约 17 个 slot 同时在飞（≈17ms 在途）**。"每符号到达即提交、不等整 slot"已经做到了。
3. **致命缺口：没有任何 GPU 侧指标**。§10.7 规划的 `[ul_gpu_lane]`
   （`residency` / `busy` / `gap` / `period`）**尚未实现** —— 代码里只有注释说它"将会"取代分段探针。
   没有它就无法回答"流水线跑满了吗、gap 在哪"，只能看 CPU 分段（即没有意义的那类数字）。

### 3.4 console/logger 那一串的真实教训（已回退，别再走一遍）

- 探针的退出统计注册在 **`std::atexit`**，而它们当年直打 `fprintf(stderr)` **正是因为**
  atexit 时 `ocudulog` 的单例已析构（崩溃堆栈：`fetch_basic_logger` → `find_logger` →
  `__hash_table::find` 访问 `0x58`）。
- 把输出改成 logger 却**不动调用时机** ⇒ 退出段错误。改用注册表修好了崩溃，但**把级别降到
  `debug`** ⇒ 默认 `all_level: info` 下汇总**既不在 console 也不进日志文件**（两头都消失）。
- 结论：**这些汇总是"无条件可见"的语义，要保留 `fprintf(stderr)`**（用户明确要求保留原状）。
- 顺带修好的**真**问题（保留）：`ENABLE_FLOW_PROBES=ON` + `ENABLE_CE_TIME=OFF` 编译失败
  （`time_en` 恒定义但 `if (time_en)` 体在关闭时仍实例化）；非 Apple Silicon 上
  `ENABLE_METAL_*=ON` 被静默忽略（现配置期 `FATAL_ERROR`）。

### 3.5 环境排障：`RF Real-time failure` 是宿主机/USB 问题，不是代码

一次"手机完全接不上"经查是 **USRP USB 接触不良**：`Real-time failure in RF: underflow/late`
达 **83915 次**（成功腿只有 56/45，差 1500 倍），且**全 CPU 路径同样发生** ⇒ 与 Metal 代码无关。
**重新插拔 USB 后恢复**（用户确认）。判据：`grep -c "Real-time failure in RF" <log>`，
个位数/几十 = 正常；上万 = 宿主机实时性/USB 问题。

---

## 4. 待办 / 下一步（**按优先级**）

### 4.1 第一步（用户已确认方向）：实现 `[ul_gpu_lane]` 探针

**在哪实现**：**`shared_burst` 内部**（不要放在 `shared_queue`，原因见 §4.2）。`shared_burst` 是
PUSCH 链的真正执行者，边界干净。

**要产出什么**（§10.7 已定稿的语义）：
| 指标 | 定义 |
|---|---|
| `residency` | 一个 slot 的数据在 GPU 上占用的总跨度（第一条 CB 的 `GPUStartTime` → 最后一条 CB 的 `GPUEndTime`） |
| `busy` | Σ 各 CB 的 GPU 执行时间 |
| **`gap`** | `residency − busy` = **GPU 空等 CPU 喂数据** ← 这就是"调度不合理"的量化 |
| `period` | 相邻 slot 的 lane-end 间隔（吞吐周期，判据是能否跟上 1ms/slot） |

**实现要点**：在 `shared_burst::commit()` 记下 CB，在 `wait_committed()` 的
`[cb waitUntilCompleted]` 之后读 `GPUStartTime/GPUEndTime` 并按 slot 聚合；
注意 `GPUStartTime/GPUEndTime` **只有 CB 完成后才有效**（`status=4=Completed`）。
`shared_burst` 用的是 `backend_queue()`（`front_end` 是 DFT）。

**⚠ 注意**：`shared_burst` 目前**从不调用** `shared_queue::notify_commit`（只有 DFT 走 front_end 时调）。
这是"乐观语义"的隐患（§48.x 记录过），做 lane 探针时顺带评估是否补上。

### 4.2 已探明但**不要**走的路

在 `shared_queue` 里做 lane 记账（`notify_commit` 记 CB + `wait_all_committed` 读时间戳）：
代码编译通过、诊断确认**被调用且时间戳有效**，但上报时采样数恒为 0 ——
写入侧打印 `state=0x102b7b1f8 size=1`，`atexit` 侧打印**同一地址** `state=0x102b7b1f8 residency=0
commits=0`（连 `commits` 都归零）。**同一个 `state()` 实例在退出时是全新状态**，根因未明。
已整体回退。**除非要专门查这个 `state()` 生命周期问题，否则换 burst 内实现。**

### 4.3 备选：demapper 批量化（WIP 在 `stash@{0}`）

**为什么有价值**：`[metal_stats] burst dispatches=20460 (equalizer=5456 demapper=15004)` ——
**demapper 占全部 dispatch 的 73%**，是这条链上最后一块未批量的（equalizer 已 11→4）。
链探针 Pattern D 实测：一次 dispatch 约 10µs，而 25 PRB 单符号 GPU 工作只有几 µs
（批量在均衡上实测 **2.01×**：207.3 → 103.0 µs/slot）。估算可省 **~100–140µs**。

**内核不需要改**（与 equalizer 不同）：`demod_soft` 本来就是 **1D 网格 + `p.nof_symbols`**，
且 `symbols[sym]`/`noise_var[sym]`/`llrs[B*sym+bit]` 全部紧凑连续。

**⚠ 但已探明一个硬障碍**：主机实际布局**不连续**。诊断输出：
```
run first=0 n_sym=1 total_re=72 mod=3 pending=11 sym=576 nv=288 llr=216
  k=0  d_sym=114688  d_nv=65536  d_llr=-47185920   ← 期望 576/288/216
  k=1  d_sym=114688  d_nv=65536  d_llr=16384
```
`d_sym=114688=0x1C000`、`d_nv=65536=0x10000` —— 正是均衡器输出槽的间距：
**每个符号的缓冲是独立槽位/page 对齐分配的，不是连续数组**。所以"只传更大 `nof_symbols`"不成立，
必须先 staging（三份拷贝：symbols/noise_var 拷进、LLR 拷回分散槽位）。staging 一次 72 RE 约 1–2µs，
而省一次 dispatch 约 10µs（净赚 ~8µs × 14 符号/slot）。
另外 WIP 实现下 `ul_chain_replay` **rc=139**（lldb 只见驱动帧
`-[AGXG16XFamilyComputeContext dispatchThreads:threadsPerThreadgroup:]`），**未收口**。

### 4.4 明确不做（本阶段）

LDPC 回 GPU、DL TX、其余 UL 信道（PRACH/PUCCH）的流水线化 —— 见架构文档 §10.7 的 v1 范围。

---

## 5. 环境与操作要点

- **构建**：`cmake --build build --target gnb ul_chain_replay -j 8`。
- **改 `.metal` 后必须确认 metallib 重编**（见 §3.1 第 1 条）。
- **本地 A/B 是秒级的，先用它**（这也是唯一能调 dispatch 的手段）：
  ```bash
  B=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
  OCUDU_UL_DUMP=/tmp/a $B /tmp/iq1_5480_17921 --metal --out /tmp/a      # 默认
  OCUDU_EQ_DEFER_ENCODE=0 OCUDU_UL_DUMP=/tmp/b $B /tmp/iq1_5480_17921 --metal --out /tmp/b
  cmp /tmp/a_5480_17921_llr.bin /tmp/b_5480_17921_llr.bin               # 必须 0 差异
  ```
- **受控探针（新增，长期保留）**：`eq_batch_kernel_probe`
  ```bash
  ./build/lib/phy/upper/channel_processors/metal/eq_batch_kernel_probe 11 72 28672 --dmrs 16384
  # 期望：OK: the batched dispatch matches the per-symbol dispatch on every symbol
  ```
- **测试向量不要删**：`/tmp/iq1_*`（187 个 reception，来自 `/tmp/gnb_cap.log`）、
  `/tmp/gnb_b9.log`、`/tmp/gnb_b10.log`（成功腿的完整日志）。
- **提交自检**：`git rev-parse --short=10 HEAD` 必须等于
  `strings build/apps/gnb/gnb | grep -E '^[0-9a-f]{10}$'`。
- **用户配置（不要动）**：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`
  （AMF 192.168.31.250、`clock_source=internal`、`tx_gain: 70`、`rx_gain: 55`、`max_ue_mcs: 20`）。
  **该文件里没有 `expert_phy` 段** —— Metal 后端必须**靠命令行**传，漏掉会掉回 CPU 路径：
  ```bash
  cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
    --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal \
    --expert_phy.pusch_dft_type metal --log.filename /tmp/gnb_bXX.log
  ```
- **退出统计**：`[ul_pipeline]`/`[metal_stats]`/`[mmse_time_sum]`/`[ldpc_time_sum]` 等是
  `fprintf(stderr)` 直打，**只在 Ctrl-C 正常退出时**出现（不要在退出前 grep）；`[mmse_time_shape]` 已删。
