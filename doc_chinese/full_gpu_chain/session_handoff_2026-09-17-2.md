# Session handoff —— 2026-09-17（融合车道开工：Step 1a 完成，停机三连修，S-7g-13 判为真收益）

> 换会话先读这一份，再读 `s2_full_chain_design.md` 的 **§48.146(a) 状态索引**。
> 本轮的全部细节都在设计文档 **§48.176–§48.187**；本文件是"读它就能接着干"的浓缩版。
> 实验室换环境后的旧交接见 `session_handoff_2026-09-17-1.md`（其 §0b 已被本文件取代）。

---

## 0. 一句话现状

**代码全绿、能干净停机、里程碑 tag 已打；融合车道（把 CE→EQ→demap 合成一条后端命令缓冲）刚做完引擎那一半（默认关）。**
下一步是把适配器接上（Step 1b，§4），判据已定死：**打开开关后 deferred-chain 必须逐字节不变**。

---

## 1. 代码状态（回来第一件事：核对这一节）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `apple-silicon` / **`9b8b471278`**（已 push）|
| 里程碑 tag | **`gpu_phy_fused_lane_ready` → `690b086679`**（annotated，已 push）——融合车道的**基线**；tag message 里写了它断言什么、证据、以及"故意不在里面"的三条 |
| 旧里程碑 tag | `gpu_phy_iq2llr_zero_host_copy → 3d026939eb` **冻结不动**（政策见 §48.181：老 tag 不动、**只在 milestone 打新 tag**、要"最新已验证"指针就用分支）|
| 树里的二进制 | `build/apps/gnb/gnb`，stamp = `9b8b471278`；核对：`strings build/apps/gnb/gnb \| grep -c 9b8b471278` == **2** |
| 参考二进制（对拍用）| `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref`（sha256 `022ac49a…`）**＋必须与它并排放 4 个 `.metallib`**（见 §3.5）|
| 语料 | `doc_chinese/work_tmp/corpus/`（27 个合成捕获）|
| 上机日志 | `doc_chinese/work_tmp/logs/`（该目录**必须存在**：`--log.filename` 用 `fopen`，不建父目录、静默失败）|

**本轮提交链（最新在前，全部已 push）**

```
9b8b471278  fused lane, step 1a: the MMSE engine can encode into the shared burst (off by default)
690b086679  probe: the UL pipeline series starts when the samples start arriving  ← tag gpu_phy_fused_lane_ready
66c973c514  RU: stop the lower PHY before the radio, not after it      ← 三次停机崩溃的根因
d6026d4b22  lower phy test: the symbol-grained receive policy is opt-in
c5c71c229e  lower phy: the whole-slot receive policy is the default again
6183126640  lower phy: a refused uplink task is never fatal
0084a4f44d  lower phy: the logger header is not a flow-probe include
37d97d7987  lower phy: a task refused while stopping must not abort the shutdown
b50c6289d4  GPU-PHY S-7g-13: the receive side asks for whole OFDM symbols, not whole slots
aa37a84003  probe: the UL pipeline start is the slot's FIRST samples, not its latest block
50e989fef3  macos_compat: a wrap mapping does not outlive the allocation it maps
dfbea28562  GPU-PHY S-7g-15: a run may only merge symbols that share one allocation
c9dd22edf0  build: the Metal tree is entered only when a Metal offload is compiled in (S-7g-14)
cbb243010b  （上一轮的 HEAD）
```

**未提交 / 不要提交**：`configs/*.yml`（用户本地环境改动）、`doc_chinese/`（gitignored）、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh`。

---

## 2. 本轮做完的事（按主题；判据都过了）

### 2.1 S-7g-14：Linux 构建门复活
`lib/phy/CMakeLists.txt` 曾**无条件** `add_subdirectory(metal)`，而该目录第一行是 `enable_language(OBJCXX)` ⇒
Linux 上 configure 直接死在"Objective-C++ 编译器不可用"（`cc1objplus`）。**从第一个 Metal 提交 `e356f0d287` 起就没绿过**。
修法：顶层派生 `METAL_OFFLOADS_ENABLED` 聚合变量 + gate 子目录 + 在 `lib/phy/metal/CMakeLists.txt` 里断言平台。
**Ubuntu NUC（GCC 13.3 / CMake 3.28 / 12 核）**：全量构建 `RC=0`、`ctest -L phy` **164/164**。
> 这条门**每腿必跑**（`ssh jwang@192.168.100.131`，见 §6）。它第一次派上用场就抓到 `ocudulog.h` 被 `OCUDU_FLOW_PROBES` 包着、
> macOS 靠传递包含侥幸编过、GCC 直接报错（`0084a4f44d`）。

### 2.2 S-7g-15：融合车道的两个前置缺陷
1. **deferred-burst 缺陷**（上一轮记为"稳定复现"，实测是 **24/30** 闪烁）：`demod_flush_hook` 只凭"相邻符号字节间距一致"
   就合并成一个带 stride 的网格，但一个 run 只经**一个映射**绑定、只覆盖**首符号所在分配** ⇒ 三个 staging 分配地址等距时
   内核索引到映射之外 ⇒ LLR 错。修法：run 只能在三个数组都留在首符号分配内时增长（`allocation_bytes_left`）；
   `wrap_no_copy` 对装不下的请求**拒绝**（计数+日志），且检查放在碰缓存之前。
   生产照旧合并（`demod_batch max_run=11`），demapper 单测 **0/40**（原 24/30 失败）。
2. **wrap 映射的生命周期**：`compat::aligned_free` 只删注册表、不通知按地址缓存映射的消费者 ⇒ 映射活过它的分配
   （实测：同一地址先 224 KiB 后 448 KiB）⇒ 离线 `zero-copy wraps` 契约一直 FAILED。
   修法：`compat::register_aligned_free_observer()`（删表之后、`::free` 之前、不持锁回调），wrap 缓存据其清理；
   `[metal_stats] wrap … purges=7421`（上机实测，说明这条路在生产里是活的）⇒ 离线契约 **MET**、`replaces=0`。

### 2.3 S-7g-13：符号对齐接收（**opt-in**，默认仍整 slot）
接收侧从"补满当前 slot"改成"整数个 OFDM 符号 + 一个 slot 缓冲渐进填充"（`uplink_processor_baseband::locate_symbols()`、
`OCUDU_UL_RX_SYMBOLS=0/1/4…`）。**FSM 一行未改**（它本来就按时间戳定位符号网格）。
- 功能：三条上机腿全部 `contract MET 7/7`、`assembled=0`、`in_place` 100%、`gaps=0`；
- **收益（换算后）**：真实端到端 `=0` ≈ **2498 µs** vs `=4` = **1801 µs** ⇒ **快 ~600–700 µs（≈28%）**；
- **为什么默认改回 0**（`c5c71c229e`）：① 新口径（`690b086679`）还没有一条腿实测确认；② 当晚环境不干净（失败率超标）；
- 新增测试：`ReceiveBlocksHoldWholeSymbols`（48/48）、`SymbolGridIsDescribedExactly`（12/12）。

### 2.4 停机三连修（**这是本轮最痛也最重要的部分**）
`^C` 时依次暴露了三个问题，`=0` 从不触发、`=1/4` 必触发：

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 1 | `OCUDU FATAL ERROR: Failed to execute uplink processing task.` + abort | `ul_process` 把"执行器拒绝投递"当致命；而应用**先停执行器、后调 PHY 的 stop()** | `37d97d7987`→`6183126640`：**永不 abort**（计数 + `[error]` 日志）|
| 2 | segfault（调度器 / `du_ue_drb::stop()`）| 同源：PHY 还在给正在拆除的 MAC 喂 slot | 同上 + `04bd939abe`（停机后不再交付上行）|
| 3 | segfault（`process_collecting`，主线程在 `radio_uhd_rx_stream::stop`）| **停机顺序反了**：`ru_controller_sdr_impl::stop()` 先停电台（来源）、后停 PHY（消费者）⇒ 电台垂死时交回残缺块 ⇒ 符号跨块 ⇒ 装配路径 | `66c973c514`：**先停消费者、再停来源** |

修好后 `=0/=1/=4` **三条腿全部干净停机**（一个顺序修复同时解决三个栈）。
另外 `1c94109a7f`：**一收到停止请求就打印 UL 序列 + 契约表**（原来在停机末尾，崩溃就全丢——今天连丢两次）。

### 2.5 测量口径：端点会移动（**这条必须记住**）
- `record_start` 原来按 slot **覆盖** ⇒ 一个 slot 拆成多块时只留最后一块 ⇒ 指标"假变快"。修法（`aa37a84003`）：保留该 slot **最早**一次调用。
- 但整 slot 策略的 T_start 仍在 **slot 末**（`receive()` 要等采样收满才返回）⇒ 它的 `[ul_pipeline]` **不含等采样的 ~1 ms**。
  修法（`690b086679`）：**在向电台要样本之前**记录起点 ⇒ 序列在任何策略下都读同一个物理量（该 slot 首样本进 → CRC-OK 出）。
  **此前记录的序列与之后的不可比**。
- **端点不动的裁判**：`[ul_gpu_lane] residency/busy/gap`（纯 GPU 时间戳）。

### 2.6 融合车道 Step 1a（引擎侧，**默认关**）
CE 引擎原本每个 stage 自建命令缓冲 + commit + `waitUntilCompleted`（=上机 `gpu_wait≈346 µs`）。
本轮加了 **burst 模式**：`set_fused_burst(bool)`；四个 stage（`build_pilots_lse`/`build_correlation`/`invert`/`apply`）
经 `begin_stage()`/`end_stage()` 统一——开时用 `shared_burst::encoder(pipeline)`（pipeline 变化自动插 barrier ⇒ CE→EQ 有序）、
**不 commit 不等待**、`wait_pending()` 无等待返回 true；burst 报告新增 `channel_estimator=…`。
**默认关 ⇒ 行为零变化**（判据：`ctest -L phy` 163/163、CE 单测 5/5、deferred-chain contract MET、demapper 0/20、全量构建 0 error）。

---

## 3. 新会话必须知道的"事实与判据"

1. **每 lane 一次同步是硬约束**（LLR 要交给 CPU 的 LDPC）；融合只去掉"每 stage 一次"的那些等待。
2. **上机基线**（手机 OnePlus 8T，`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`，`cpu_gpu`）：
   - `[ul_gpu_lane] residency≈612–636 / busy≈498–518 / gap≈98–119 µs`；`cbs/lane=3.00`（CE 2 + eq_demap 1）；
   - `[mmse_time_sum] gpu_wait≈333–346 µs`、`defer_wait≈530–559 µs`（**这两笔就是融合车道要消掉的**）；
   - `burst commits==waits`（每 lane 一次）、`dft max_in_flight=8`、`demod_batch max_run=11`、`eq_batch max_run=4`；
   - `crc=OK` 85–86%；失败率当晚不干净（`=0` 21/89574=0.0234%、`=4` 8/57700=0.0139%，预算 0.0116%；**昨天两腿都是 0.00000%**）⇒
     **失败率必须等一次干净环境重测才作数**。
3. **停止顺序不可回退**：`ru_controller_sdr_impl::stop()` 必须先停 lower PHY 再停电台。
4. **测量不得扰动被测对象**：`OCUDU_METAL_GPU_TIME` 仍 opt-in；`[ul_time_frequency]` 等三段是**延迟窗口**，只在同口径内比较。
5. **参考二进制必须带着它自己那批 `.metallib`**（`work_tmp/ref/`，4 个文件）：
   它运行期按"编译期烧入的绝对路径 → 可执行文件旁 → cwd"找内核，**丢了内核会静默回落宿主估计**，
   表现为 `_ce.txt` 的 `noise_variance`/`rsrp` 差 ~1 ULP、而 llr/h/grid 与判决全一致（**指纹**）。
   自检：`bash doc_chinese/full_gpu_chain/wip/ab_tol.sh doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref 5 --metal` ⇒ 必须 `byte-identical 5/5; worst=0`。
6. **`--log.filename` 不建父目录**（`std::fopen`）：`doc_chinese/work_tmp/logs/` 必须存在（已有 README 占位）。
   每次跑用不同文件名，例如 `gnb_<腿>_k${OCUDU_UL_RX_SYMBOLS:-0}_$(date +%H%M).log`（同名会互相覆盖）。

---

## 4. 下一步：融合车道 Step 1b（**接线**，就是这一件事）

**目标**：deferred 路径上让 CE 的派发真正落进 lane 的共享 burst，于是每 lane 只剩**一次** commit/wait
（`mmse_ce commits`→0、`cbs/lane` 3→2、`gpu_wait`→≈0）。

**机制已在代码里确认**（`pusch_demodulator_impl.cpp:526-575`）：
```
equalizer->submit(...) → demapper->submit(...) → demapper->wait();  ← 组内唯一同步点
```
而估算器是 "left running"（`:302-304`），它的完成与标量读回发生在**下一个 hop/slot**，即**在 `demapper->wait()` 之后**
⇒ 那时 burst 已 commit，适配器里补一次 `shared_burst::wait_committed()` 是**立即返回**（不是新增等待）。

**两处改动**（设计方案见 §48.185/§48.186/§48.187）：
1. `port_channel_estimator_metal_mmse_impl.cpp`：deferred 路径 `engine->set_fused_burst(knob && deferred)`，
   同步路径显式 `false`；开关 `OCUDU_CE_FUSED_BURST`，**默认关**。
   （`defer` 的判定点：基类 `port_channel_estimator_average_impl::submit()` → `do_submit()` → 虚函数
   `apply_fd_td_estimation_stage(args)`；args 结构里**没有** deferred 字段，需要在基类或 args 上补一个，
   **不要猜**——猜错会让派发留在一个没人 commit 的 burst 里 = **静默错误的信道估计**。）
2. 完成路径（`complete_fd_td_estimation_stage()`，`:2668` 附近）：burst 模式下先 `shared_burst::wait_committed()`，
   再读 `sigma2`/`pilots_power` 两个标量，并把 unpack 交给已有的延迟设施 `pending_unpack`/`defer_unpack()`
   （`..._mmse_impl.h:466-489`，`max_pending_unpacks=2`）。

**硬门（必须先过）**：打开旋钮后 `build/tests/unittests/phy/upper/channel_processors/pusch/pusch_demodulator_deferred_chain_test`
**逐字节不变**（它同时验证"CE→EQ→demap 共用一个命令缓冲 + 顺序正确 + 标量读点在 commit 之后"）。
然后：四网逐字节（§3.5 的参考）、六门、Metal 自测 5 个、两条配置构建门、**Ubuntu 全量构建 + 164 测试**。

**之后（方案 §48.185(d)）**：
- **Step 2**：DFT→CE 跨队列依赖用 `MTLEvent`（前端 signal、后端 wait；**只在生产者已提交时 wait**）；不把 DFT 折进同一条 CB。
- **Step 3**：lane 仪器覆盖前端（lane 身份跨线程按 slot；今天 `gpu busy(front_end): commits=0` 就是缺它）⇒ 整条 IQ→LLR 有裁判。
- **Step 4**：lane 并发 `max_in_flight` 1→N（前置：1–3 绿 + S-7g-7 生命周期 + `replaces=0`），单独收尾。
- 顺带（与下一次上机合并）：用**新口径**确认 S-7g-13（预期 `=0`≈2400–2500 µs、`=4`≈1800 µs）+ 干净环境重测失败率 ⇒ 再决定默认值。

---

## 5. 关键文件与路径

| 内容 | 路径 | 备注 |
|---|---|---|
| 设计文档（唯一活文档）| `doc_chinese/full_gpu_chain/s2_full_chain_design.md` | gitignored；**§48.146(a) 是状态索引**；本轮新增 §48.176–§48.187 |
| 本轮交接（本文件）| `doc_chinese/full_gpu_chain/session_handoff_2026-09-17-2.md` | 上一轮：`session_handoff_2026-09-17-1.md` |
| 融合车道方案 | 设计文档 **§48.185**（方案）/§48.186（Step 1 细化）/§48.187（CE 标量读回约束）| 含判据表、四步分解、风险对策、回滚 |
| tag 政策 | 设计文档 **§48.181** | 老 tag 不动；只为 milestone 打新 tag |
| 每腿门脚本 | `doc_chinese/full_gpu_chain/wip/{run_ab_all.sh,ab_tol.sh,ab_cpu.sh,ab_strict.sh,run_gates.sh,run_metal_engines.sh}` | 语料缺省优先 `work_tmp/corpus` |
| 参考二进制 + 内核 | `doc_chinese/work_tmp/ref/`（二进制 + 4 个 `.metallib`）| 见 §3.5；旧产物留档 `stale_2026-09-15_…` |
| 上机日志 | `doc_chinese/work_tmp/logs/`（README 说明 fopen 坑）| 同名会覆盖 |
| 上机证据 | `doc_chinese/full_gpu_chain/air_logs/`（S-7g-8/10/15 + `s7g15_legA_evidence.md`）| §48.177 是 S-7g-15 腿 A 判读 |
| 本轮改动的代码 | `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}`、`lib/phy/lower/processors/uplink/{uplink_processor_impl.{h,cpp},uplink_processor_baseband.h}`（`include/`）、`lib/phy/metal/ocudu_metal_{queue.mm,burst.{h,mm}}`、`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.{h,mm}`、`lib/ru/sdr/ru_controller_sdr_impl.cpp`、`apps/gnb/gnb.cpp`、`include/ocudu/support/executors/ul_pipeline_probe.h`、`include/ocudu/support/macos_compat.h` + `utils/macos_compat/macos_compat.cpp`、`CMakeLists.txt` + `lib/phy/CMakeLists.txt` + `lib/phy/metal/CMakeLists.txt` | | |
| 测试 | `tests/unittests/phy/lower/lower_phy_test.cpp`（+`ReceiveBlocksHoldWholeSymbols`）、`.../processors/uplink/lower_phy_uplink_processor_test.cpp`（+`SymbolGridIsDescribedExactly`）、`.../uplink/uplink_processor_notifier_test_doubles.h`、`tests/unittests/gateways/baseband/baseband_gateway_receiver_test_doubles.h` | | |

---

## 6. 环境与命令

- **Mac（gNB/电台）**：`cd /Users/jiachengwang/dev/ocudu`；`sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml …`
- **Ubuntu NUC（Linux 门）**：`ssh jwang@192.168.100.131`（免密）；`cd ~/work/ocudu && git pull --ff-only && cmake --build build -j12 && ctest --test-dir build -L phy -j10`
- **上机命令模板**（`cpu_gpu`，注意日志名加参数与时间）：
```bash
sudo -E OCUDU_UL_RX_SYMBOLS=0 ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.phy_pipeline cpu_gpu --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto --expert_phy.device_resource_grid on \
  --log.all_level info --log.filename doc_chinese/work_tmp/logs/gnb_<腿>_k0_$(date +%H%M).log
```
- **上机验收数（`^C` 当场打印，不怕之后崩）**：UL 序列 + **契约表** + `[ul_host]`/`[metal_stats]`/`[ul_gpu_lane]`；
  另从日志取 `crc OK/KO`、`Real-time failure in RF`（失败率口径：后者 ÷ 墙钟时隙，预算 0.0116%）。
- **每腿离线门**：`ctest -L phy`（Mac 163）→ 四网（`run_ab_all.sh <ref> 27` + `ab_tol.sh <ref> 27 "--metal --device-grid"`）→
  `run_gates.sh`（`ALL_GATES_RC=0`）→ `run_metal_engines.sh`（`METAL_ENGINES_RC=0`）→ `/tmp/build_nostats` 与 `/tmp/build_nometal` 两条构建门 → **Ubuntu 全量构建 + ctest**。

---

## 7. 纪律与血泪清单（本轮新增，接上一份的 9 条）

10. **端点会移动的指标不得跨策略比较**：`[ul_pipeline]`/`[ul_time_frequency]` 的起点是"收到的第一个块"，
    整 slot 与符号策略下分别是 slot 的**末**与**首**——我因此一度把"快 600 µs"误判成"慢 400 µs 的回归"。
    跨策略只用 **GPU 侧 lane 指标**（`residency/busy/gap`，纯 GPU 时间戳）。
11. **归档的参考二进制必须连同它的 `.metallib`**：丢了内核它会**静默回落宿主路径**，产出"1 ULP 的假回归"。
12. **停机顺序**：先停消费者（PHY），再停来源（电台）。
13. **执行器拒绝投递永远不是致命错误**：应用可以先停执行器再调 `stop()`；计数 + 日志，不要 abort。
14. **一收到停止请求就打印证据**：否则停机路径上任何后续崩溃都会带走整腿的判读数据。
15. **"重建一下就变绿"是相关性、不是机制**：上一轮把假回归归因于"产物过期"，真相是内核丢失。
    档案类结论也要落到机制（这次靠"换个变量再复现一次"抓到）。
16. **macOS 的传递包含会在 Linux 上爆炸**：`ocudulog.h` 曾被 `OCUDU_FLOW_PROBES` 包着，GCC 直接报错。
    **每腿必须跑 Ubuntu 构建**。
17. **默认值保守**：新策略一律 opt-in（`OCUDU_UL_RX_SYMBOLS`/`OCUDU_CE_FUSED_BURST`），
    上机验证 + 口径确认之后再改默认；每步都留独立开关以便回滚。

---

## 8. 回来后的第一个动作（TL;DR）

1. `git log --oneline -3` 确认 HEAD = **`9b8b471278`**；`git tag -l --format='%(refname:short) %(objectname:short)'` 确认
   `gpu_phy_fused_lane_ready` 在远端；`cmake -P build/build_info.cmake && cmake --build build --target gnb -j 14`，核对 stamp。
2. 读设计文档 **§48.185**（方案）、**§48.186**（Step 1 细化）、**§48.187**（CE 标量读回约束）。
3. 做 **Step 1b**（§4 的两处改动 + 硬门 deferred-chain 逐字节不变 + 全套离线门 + Ubuntu）。
4. 全绿后给用户一条上机腿（判据：`mmse_ce commits`→0、`cbs/lane` 3→2、`gpu_wait`→≈0、红线不动、记录 lane `residency/busy/gap`）。
5. 继续 Step 2/3/4；顺带用新口径确认 S-7g-13 并在干净环境重测失败率。
