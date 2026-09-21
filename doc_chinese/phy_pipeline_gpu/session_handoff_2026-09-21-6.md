> **⚠ 本文件已被 `session_handoff_2026-09-21-7.md` 取代**（保留是为了记录当时的判断，包括被推翻的）。

# 交接（入口） — S33：**D1 已完成并已 tag（`gpu_phy_d1_handover_p2`）；下一件事 = L1 离线 harness（落点已定到行）；Ubuntu 构建已修好**

> **本文件是新会话的唯一入口**：读完它就能开工。**`-5.md` 及更早的交接保留不删**（它们记录了当时的判断，包括被推翻的）。
> **本文件的权威细节在常驻设计文档**：**§5.9.20–§5.9.36**（本轮 D1 的全部结论、更正与判据）。
> **⚠ 本文件同时【更正】两处旧记录**（§2）：`ul_chain_replay --dft` **不是空壳**；`s37` 那一对腿**不算数**。

---

## 0. 开机三件事（照做，不要跳）

```bash
cd /Users/jiachengwang/dev/ocudu
git rev-parse --short=10 HEAD && grep -o '[0-9a-f]\{10\}' build/hashes.h | head -1   # 两个短哈希必须相同
git status --short                                                                   # 只应有"用户自己的两个 config"
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py | tail -1                       # 期望 captures=47 problems=0
```
**戳记不同就**：`touch build/hashes.h && cmake --build build --target gnb -j 6`。
**工作树里 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 与 `configs/gnb_rf_b200_tdd_n78_20mhz.yml` 是用户自己改的，永远别动。**

| | 值 |
|---|---|
| **HEAD** | **`4b3a7c9ca6`**（本轮最后一个提交；代码提交是 `c4662f3155`）|
| 分支 | `apple-silicon`（**本地领先 origin 若干提交，未推送**；只有 tag 推了）|
| 已打的 tag | **`gpu_phy_d1_handover_p2` → `0ee881c964`（已推）**；旧的 `gpu_phy_d1_handover` → `6fc47e6d13`（s41 判定时的状态，保留）|
| 门（当前）| `value_net` **47/0**、`--self-test` **8/8**、`ctest -R metal` **10/10**、`ctest -R "ul_pipeline_probe\|puxch\|lower_phy"` **5/5**、`ctest -R "du_low\|o_du"` **20/20**、`neutral_vs_baseline` **131 differing-bytes / 235 文件** |

---

## 1. 这条线是什么、到哪了（一句话）

**D1 = "一跳一条 GPU 命令缓冲"**：把前端（OFDM 解调/DFT）每时隙一次的 CPU 提交拿掉，
让**前端把该槽的块"交出"（deposit）但**不提交**，由读这份网格的消费者（PUSCH 的"跳"）**认领**、
把自己的估计/均衡/解映射**编进同一条缓冲**一起提交一次。

**7 对空口腿（s38–s44）判完，结论：**

| 目标（用户裁定）| 状态 |
|---|---|
| **⑥ CPU 参与点** | ✅ **每上行时隙 1.452 → 1.049 次提交**（地板 **1.000**；多出来的 0.049 = 跳的错过率）|
| **⑦ GPU 一旦发动就跑完 IQ→LLR** | ✅ 一跳 **0 次 host↔device 数据搬运**、`ce device estimates … device, 0 host`、`dft commits=1`（引擎自己不再提交）、`gpu_wait=0`、次序（含栅格产出等待）**编码在 GPU 里** |
| 数据正确性 | ✅ 与对照持平或更好；**s36 的"高 sinr + CRC 全错"签名消失** |
| 饱和爬坡的拒收 | ✅ **两臂都 0**（`UL processor is busy`；s40 曾是 84/716）|
| 残留 | ⚠ **单车道排队：`ul_channel_estimation` p95 ≈ 3.4 ms**（对照 241 µs）——**已归档为将来优化**（§5.9.28/§5.9.33/§5.9.35）|

**⚠ 不要重开的事**：D1 的机制**已经判过并且已 tag**。若要动它，先读 §5.9.20–§5.9.35 的读数，别重跑一遍已判过的腿。

---

## 2. 两处【更正】（旧记录有错，务必知道）

1. **`ul_chain_replay --dft` 不是空壳**（`-5.md`/`-4.md` 里写的"`return 0` 占位"是误读）：
   该工具 **842 行**，频域回放（`OCUDU_UL_DUMP`）与时域回放（`--dft [--dft-metal] [--device-grid]`）**都已实现**，
   第 507 行的 `return 0` 是 `--dft` 的**正常出口**。**L1 缺的只有两件**（见 §4）。
2. **`s37` 那一对腿不算数**：候选臂 `handed=0 released=0`（旋钮设了、当时 `grid_has_host_consumers()` 还是 `true`）⇒ 两条都是对照臂。
   已加**开工告警**（`ofdm_demodulator_impl` 构造时打印）防重演：**上腿后第一件事**
   `grep -c "will NOT exercise D1" <候选臂日志>` **必须是 0**。

---

## 3. 本轮最重要的技术结论（新会话必须知道，否则会重复踩）

### 3.1 真障碍：`MTLBuffer` **对象**的唯一性（§5.9.20）
Metal **只按 `MTLBuffer` 对象**关联两次访问——同对象才自动插屏障；**两个对象盖同一段内存 ⇒ 两者之间完全没有顺序**
（`wip/metal_alias_order.mm`：同对象 200/200 PASS、别名对 200/200 FAIL，两方向都是）。
"一跳一条缓冲"把**前端写网格**与**后端估计器读网格**放进同一条缓冲 ⇒ 这一条成为硬约束。
**修法**：`mmse_engine_impl::wrap_grid()`（估计器读网格改走**进程级** `shared_queue::wrap_no_copy` 并**保留 offset**；
**不能**用 `wrap_shared()`——它**丢掉非零 offset**，会绑错地址，那是个尚未修的洞，见 §5）。

### 3.2 MISS 路径的次序：**让 GPU 等，不要让 CPU 等**（§5.9.23–24）
* `shared_burst::grid_production_generation(storage, slot)`：返回该 (存储,槽) 的生产代际，**未认领就先兜底提交**；
* `shared_queue::grid_ready_encode_wait(cb, generation)`：`[cb encodeWaitForEvent: grid_event value: gen]`
  （`gen==0` 或本进程没武装过 ⇒ **不编码**，绝不为没人 signal 的值等待，否则缓冲挂死）；
* 跳 MISS 时把代际记在 `pending_grid_wait`，`begin_stage()` 在**开编码器之前**（紧挨 `front_end_wait`）编码它；
  burst 路由交给 `shared_burst::set_grid_wait()`，由 `burst_ensure_open()` 在另两个 fence 同处消费；
* **宿主等待已删除**（它曾把 `s39` 拖成一次 13 s 停顿）。

### 3.3 饱和爬坡拒收的根因与修法（§5.9.25–27）
`UL processor is busy` = `get_pdu_slot_repository()` 返回空（**两个原因**：网格引用未放 / `start_new_slot()` 拒绝）。
**判据是反向的**：对照臂**更晚**才放开网格却拒收得少 ⇒ 是"上一个槽的任务还在飞"。
修法：**`nof_ul_rg` = 6 × 每帧时隙数**（`du_low_config_translator.cpp`；复用周期 10 ms → 60 ms）。
⚠ `Failed to allocate UL resource grid = 0` **不能**用来排除网格引用那条（翻译器先打 "busy" 就 return 了，那一行走不到）。

### 3.4 车道与解码池分开（§5.9.34）
原来 `pusch_ch_estimator_executor` / `pusch_executor` / `pusch_decoder_executor` / `srs_executor`
是**同一个中优先级池的三个 fork-limiter 视图** ⇒ 一次 LDPC 解码占掉车道要用的名额。
现改为 `pusch_decoder_executor` 与 `srs_executor` 走 **`non_rt_low_prio_exec`**（池本来就在，不新增线程）。
**效果**：极值全消（`ul_pipeline` max 69794 → **7032 µs**、LDPC 离群 26135 → **168 µs**、EQ-demap 66.9 → 3.0 ms），
**但** `ul_channel_estimation` 的 p95 没动 ⇒ 那 3.4 ms 是**单车道（并发=1，按设计）的固有排队**。

### 3.5 分流的形状（回答"FFT 之后往哪走"）
**有没有 PUSCH 是 gNB 事先知道的**（上行是它自己授权的，`UL_TTI.request` 在样本到达前就填好了 PDU 仓库）。
**分支点是资源网格，不是 FFT kernel**：一个写者（FFT+写网格）、三类读者——
PUSCH（**设备**读者，在 GPU 内继续）、PUCCH/SRS（**宿主**读者，先等栅格产出栅栏）、PRACH（**不碰网格**，时域）。
`s44` 的 30635 块网格去向：**跳 41% / 宿主读者 50% / 无人认领（扫掠）9%**。

### 3.6 计数器的含义（判读时会用到）
* `[metal_stats] dft handover handed/taken/fallback/late/not_found/timeouts/keepalives`（`unproduced`）；
* `[metal_stats] mmse_ce … grid_shared / grid_failed / grid_devwaited / grid_wait_unencoded`；
  **`grid_shared == hops`**、`grid_devwaited ≈ 跳数 − taken`、**`grid_wait_unencoded` 必须 0**；
* `[metal_stats] dft commits`（武装后应为 1）、`burst commits`（= 跳数）、`equalizer ch_re device/host`；
* 相位分段：**`OCUDU_UL_PHASE_SEGMENTS=1`**（`gpu` 模式默认不记录，**两条臂都要带**）⇒
  `[ul_time_frequency]`（IQ→交出）/`[ul_channel_estimation]`（→估计器段完）/`[ul_equalization_demod]`（→首次解码）；
* 探针配对是**"槽、槽−1、槽−2 + 2 s 陈旧门限"**（`find_fresh`）⇒ 它**配不上**很久以前的起点；
  `[ul_ldpc_decode]` 是**按槽精确配对**的（更可信）。

---

## 4. ★★★ 下一步：**L1 离线 harness**（用户已定：这是最先做的，为了少跑 OTA）

**目标**：把它做成"**机制可以在本机判**"的东西，OTA 只留给本机判不了的（UHD 时序、FAPI 截止、负载调度、RF）。

### 4.1 落点（精确到行）
`lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp` 的 `--dft` 分支：
* 在 **`write_grid(current_slot)`（第 504 行）之前**、流水线 drain（第 497–499 行）之后，
  加"**工具充当消费者**"：取 `grid->get_writer().get_device_view().base`，
  调 **`grid_ready_hook::wait(base, slot)`**（未武装时是空操作 ⇒ 参考路径逐字节不变），超时 `return 1` 并打错；
* **必须同时断言"交出真的发生了"**（例如打印/解析 `[metal_stats] dft handover`，要求 `handed>0 && taken>0`），
  否则会重演 `ofdm_demodulator_metal_batch_test` 武装节那次的**空判**（关着的时候它什么都没跑）。

### 4.2 ⚠ 先核对一个会"静默不测"的坑
键是 `(网格存储, 接收槽)`，接收链用的是 **`slot_point::to_uint()`**（`puxch_processor_impl.cpp:154` →`set_lane_slot`），
而 replay 的槽号来自**捕获文件**。**若编号基准不同，`grid_ready_hook::wait()` 会"找不到记录"→ 返回 true（fail-open）**，
harness 看着在跑、其实什么都没测。**核对方法**：先跑一次并确认 `taken > 0`。

### 4.3 最小可用版的四步
1. **语料**：`--dft` 需要 `OCUDU_UL_DUMP_TD`（时域捕获），**Mac 上现在没有**。两条路：
   （a）从 **Ubuntu 那台**（ZMQ/srsUE 台子）录一份拷过来；（b）加 `--synth` 合成一份（工作量更大）；
2. 加 §4.1 的"消费者等待 + 断言"（约 6–20 行）；
3. **A/B**：同一份语料跑两次（`--dft --dft-metal --device-grid`，不带/带 `OCUDU_DFT_RELEASE_BLOCK=1`），
   用 `scripts/ul_stage_diff.py` 比 dump ⇒ **期望 0 差异**，且武装那次 `handed>0`；
4. **两条反向臂（决定它有没有资格判事）**：
   （i）把第 2 步的等待去掉 ⇒ dump **必须不一致**；
   （ii）把 MISS 的设备侧等待去掉（`pending_grid_wait` 那段）⇒ "跳 MISS"那条臂**必须不一致**。

### 4.4 再往上（L1 的第二层）
把**上层 PHY 的任务生命周期**也串进来（`uplink_processor_impl` + 执行器 + FSM + PDU 仓库），
才能判 §5 里的"同槽多 PUSCH"与"认领宽限期"。**L2（另一台机器）**：Ubuntu 的 **ZMQ + srsUE** 台子跑端到端负载，
判"拒收/泄漏/吞吐/尾部"这类本机只有 OTA 能看的东西。

---

## 5. 其余开放项（优先级从高到低）

1. **同槽多 PUSCH / 多 UE（结构悬崖）**：注册表按 `(存储, 槽)` 为键 ⇒ **一块只能被一个跳认领**，
   同槽 N 个 PUSCH 时只有第一个能合并，其余 N−1 **必然 MISS**（结构性，不是竞态）。
   现在单 UE 把它藏住了；真实部署会撞上。**要修**：一块网格服务多个跳。
2. **`not_found` 的小洞**：跳若"无记录可等"就**直接读网格**（理论上可能读到没人写过的那份）。
   `s44` 的 `not_found=1324` 里多数是"已产出后被淘汰"（无害），但"从未 deposit"那一路没有守护。
3. **认领宽限期**：把 1.049 推到 **1.000**（现在那 4.9% 是 **PUCCH 先兜底提交抢走的**）。
   给宿主读者的兜底加**有界、很短**的"等跳来认领"窗口；代价是 PUCCH 延迟（要单独一对腿）。
4. **单车道并发**：`max_pusch_and_srs_concurrency` 1 → 2（一行配置），前提 GPU 扛得住；
   目标是把 `ul_channel_estimation` p95（3.4 ms）压下来。
5. **前端栅栏代际 + `wait_all()` 不覆盖交出去的块**（§5.9.4 ⑤-1/⑤-2）——机制内部最后两处次序/归属洞。
6. **`wrap_shared()` 丢非零 offset**（9 个调用点，**静默错址**）——与 D1 无关的既有洞。
7. **探针的 `stale=` 分流**：把跨度 > 上行 HARQ RTT（本配置 8 ms）的样本单独计数，
   并把"起点槽→完成槽"的槽距记下来 ⇒ `max`/p99 才重新可信（§5.9.28 已归档为将来优化）。
8. `burst dispatches` 的 `channel_estimator=0` 计数缺口；`ul_pipeline_probe_test` 的时序脆断言（~1/10）。
9. **PRACH-only 时隙是否会白做 FFT + 白交一次**（PRACH 不读网格）——**先数**再决定。
10. **下行方向的同口径记账**（D1 只是上行那一半）：每时隙几次 CPU 提交、有无 host↔device 数据搬运、
    DL 网格池复用周期。
11. §18.7 的 TA fence 实验仍未做。

---

## 6. 腿法（本轮定的规矩，别退化）

```bash
# 0) 戳记必须 == HEAD；手机 WiFi 关掉；sudo 由【用户】执行
touch build/hashes.h && cmake --build build --target gnb -j 6
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>-base
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> OCUDU_DFT_RELEASE_BLOCK=1
#    需要相位分段时两条臂都加： OCUDU_UL_PHASE_SEGMENTS=1
```
**判读顺序（按本轮学到的）**：
1. `grep -c "will NOT exercise D1" <候选臂>` **必须 0**（否则这一对什么都没判，别急着测手机）；
2. `[metal_stats] dft handover`：`handed>0 taken>0 timeouts==0`、`keepalives` 两侧相等；
3. `SReal-time failures` / `UL processor is busy`（**先看这个**）；
4. **CRC 按调制分层**（不能只比总量——两臂的调制结构可能完全不同）；
5. **按调制分层的 sinr 分布**：KO 的 sinr 中位应低于其门限；"高 sinr + CRC 全错"才是缺陷签名；
6. `grid_shared == hops`、`grid_wait_unencoded == 0`、`grid_devwaited ≈ 跳数 − taken`；
7. 端到端只看**中位/p95 与稳态窗口**（`max` 会被"作废的迟到解码"污染，见 §5.7）；
8. **慢变化不要用单点比**：两条腿的链路可以差 ~5 dB，MCS 结构随之不同。

**离线优先（本轮教训）**：能离线判的**不要上腿**。上腿前的三条自查：
模式对吗（D1 必须 `gpu`）、判据会触发吗（`handed>0`？）、开关武装了吗。

---

## 7. 两台机器（都在用）

| | Mac（`/Users/jiachengwang/dev/ocudu`）| Ubuntu（`ssh jwang@192.168.100.131`，仓库 `~/work/ocudu`）|
|---|---|---|
| 用途 | **空口腿**（B200 + 手机）、**Metal 侧一切**（D1 机制只在这里存在）| **构建**（12 核）、**ZMQ/B210 + srsUE 台子**（`~/work/srsRAN_4G`、`srsran_project`、未跟踪的 `gnb_zmq_n1_5mhz.yml`/`gnb_zmq_n78_20mhz.yml`/`gnb_rf_b210_fdd_srsUE*.yml`）|
| 构建 | `-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON` | `ENABLE_METAL_*=OFF`、`UHD=ON`；**已修好**：`gnb` exit 0、**默认全量目标 exit 0 / 0 errors**、`ctest -N` **7618** 个测试 |
| 待办 | L1 harness（§4）| 录一份 `OCUDU_UL_DUMP_TD` 语料给 L1；把 ZMQ 环回跑通 |
| ⚠ 注意 | Linux 上的 **Metal 不存在**（D1 机制**无法**在 Ubuntu 判）| Ubuntu 工作树里 **`pusch_demodulator_impl.cpp` 是未提交的同一份修复**（内容 == Mac 的 `c4662f3155`）；下次同步 `git checkout --` 该文件即可 |

**本轮修好的 Ubuntu 构建问题**：三个变量（`force_host_estimates_now`/`equalizer_reads_device`/`estimator_published`）
只在 `#if defined(OCUDU_METAL_STATS)` 内被读，而 Linux 编译掉那段 ⇒ `-Werror=unused-variable` 失败。
修法：**把声明搬进同一个守卫内**（Metal 构建逐字节不变）。Mac 提交 **`c4662f3155`**。

---

## 8. 本轮的腿（`s38`–`s44`，供新会话引用，不必重跑）

| 腿 | 判什么 | 头条 |
|---|---|---|
| `s38` | 交出第一次真发生 | `dft commits=1`、`handed=25754`；**对象唯一性缺陷暴露**（crc=KO @ 20–38 dB）|
| `s39` | MISS 路径加宿主等待 | 中位 4797→1962 µs、认领 59%→77%；**代价：13 s 停顿（355 次拒收）** |
| `s40` | 设备侧等待（`encodeWaitForEvent`）| `grid_devwaited=843`、`grid_wait_unencoded=0`、中位 1851 µs、94% 认领；拒收 716 |
| `s41` | 6 帧余量（3×）| **拒收 716→91**，对照 84→**0**；提交/时隙 1.036 |
| `s42` | 再放宽到 6 帧 | **两臂都 0 拒收**；提交/时隙 1.022；尾部被切成两半 |
| `s43` | 相位分段 | 前端**更快**（1098 vs 1400 µs）；尾巴在 `ul_channel_estimation`（p95 3387 vs 205）|
| `s44` | 车道/解码池分开 | 极值全消（max 69794→**7032**、LDPC 26 ms→**168 µs**）；**CE p95 没动** ⇒ 单车道排队 |

---

## 9. 一句话给新会话

**D1 完成并已 tag（`gpu_phy_d1_handover_p2` → `0ee881c964`）：每上行时隙的 CPU 提交 1.452 → 1.049（地板 1.000）、
一跳 0 次 host↔device 数据搬运、次序全在 GPU 上、拒收两臂 0、数据与对照持平或更好；
残留只有"单车道排队"（CE p95 3.4 ms），已归档。**
**下一件事是 L1 离线 harness（`ul_chain_replay --dft` 加"消费者等待 + 交出断言"，A/B 0 差异，两条反向臂必须红），
先把语料准备好（Ubuntu 的 ZMQ/srsUE 台子或 `--synth`）；之后按 §5 的优先级推进（**同槽多 PUSCH 是唯一的扩展悬崖**）。**
