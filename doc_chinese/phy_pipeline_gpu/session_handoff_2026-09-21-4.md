# 交接（入口） — S20：**空口腿否证了 D1 第 2 步的接线：变换不能比输入活得久，而"宿主等待"正是输入生命期的锚**（已回滚）

> **本文件是新会话的唯一入口**：读完它就能开工。
> **⚠ 本文件【撤回】`session_handoff_2026-09-21-3.md` 的 §4**（那份说"跑腿对就能判"——跑了，判了，**结论是坏的**）。
> `-3.md` **保留不删**（它记录了当时的判断），但**它的腿法/判据/结论都不要再用**。
> **技术细节全在常驻设计文档**：本会话的是 **§5.9.5**（第 1 步 + 对象同一性）、**§5.9.6**（第 2 步接线）、
> **§5.9.7**（**★ 腿的结果与那条新硬约束——下一份工作的依据**）。

---

## 0. 相对上一份（`-3.md`）的变化

| 项 | `-3.md` 说 | 现在 |
|---|---|---|
| 第 2 步 | 接线做完，跑个腿对判 | **腿跑了：候选臂上行彻底坏掉 ⇒ 已回滚**（调用点撤掉；机制保留、默认关闭）|
| 下一步 | 跑腿对 | **把接收缓冲的生命期挂到"被认领缓冲的完成"上**（§4）|
| 新知识 | —— | **★ 第四条硬约束**：被交出去的缓冲里，"读输入"的 dispatch 不能在输入生命期之后才执行（§2.2）|
| 门 | 同数 | **同数**（回滚后 `value_net` 47/0、`ctest -R metal` 10/10、`neutral_vs_baseline` 131）|

---

## 1. 一句话状态

**工作树 HEAD = `1a38a9804b`**（= 第 2 步接线 + 回滚 + 第 3 步的 1/2/4 小步 + 池子泄漏修复 + **方案 A 的机制** + 再次关掉交出；快照提交跟在它后面）。
**开机第一件事就是自查戳记**：
```bash
git rev-parse --short=10 HEAD && grep build_info build/hashes.h
```
**两个短哈希必须相同**（不同就 `touch build/hashes.h && cmake --build build --target gnb`）。
工作树只剩**用户自己的两个 config**，**别动**。**提交全部只在本地，没有推送。**

**门（回滚后复跑）**：
```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py             # 47 捕获 0 问题
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 8/8
ctest --test-dir build -R metal                                   # 10/10
bash doc_chinese/phy_pipeline_gpu/wip/neutral_vs_baseline.sh      # 131 differing-bytes（= S15/S17/S18 同数）
```

**当前状态：`OCUDU_DFT_RELEASE_BLOCK=1` 已经**不会再交出任何东西**（低层 PHY 的调用点已撤）。**

---

## 2. ★★ 腿的结果（`gpu` 模式，2026-09-21 10:48/10:51，commit `997570ced4`）

### 2.1 对照臂（`gnb_gpu_s32-d1-step2-base`）：**完全健康**

| 读数 | 值 |
|---|---|
| 契约 | **8 of 8 MET**（`ce device estimates` **160072 device / 0 host**、crossings **0**）|
| `dft commits` / `transforms` | 30024 / 420323 → **2.06 次/跳**（14552 跳）|
| `released` / `released_waits` | **0 / 0** |
| `[metal_stats] dft handover ...` | **没有这一行**（旋钮没武装就不打，符合设计）|

### 2.2 候选臂（`gnb_gpu_s32-d1-step2`，`OCUDU_DFT_RELEASE_BLOCK=1`）：**否证**

| 读数 | 值 |
|---|---|
| **CRC** | **`crc=KO` 942 / `crc=OK` 46**（QPSK），而 **`sinr=37.6 dB`** ⇒ **数据错，不是无线弱** |
| UE 行为 | 反复重连：`ue=0,1,2,…,8` 每 ~10 s 一个 ⇒ **msg3 上不去 ⇒ attach 永远完不成** |
| 进程退出 | 日志**在一行中间截断**、**没有关闭过程**（对照臂有 "CU-CP stopped successfully"）⇒ 没有 `[metal_stats]` 块 ⇒ **`handed=` 读数丢了** |

### 2.3 ★ 机制（**代码里写着，不是推测**）

`puxch_processor_impl::finish_symbol()` **返回**，就是告诉接收链"**这个符号的样本已经没人读了**"：

```cpp
// finish_oldest_symbol():
demodulator->finish_symbol(current_grid.get().get_writer(), entry.slot);
...
// Retire the entry: its samples belong to the radio again from here on.
in_flight[in_flight_begin].owner.reset();      // ← 接收缓冲引用在这里放掉
```

**这句话今天成立，只因为 `finish_symbol()` 里那一次 `dft->wait_slot(slot)` 等到了变换做完。**
第 2 步拿掉了那次等待 ⇒ **接收缓冲在变换真正执行之前就回池并被下一批样本覆盖** ⇒
零拷贝的 `grid_write::time_samples`（420322/420323 个变换读的是**电台缓冲**）**读到过期样本** ⇒
**静默错数据**（`sinr=37.6 dB` + CRC 全错就是它的签名）。

### 2.4 ★★ ⇒ 新硬约束（**§5.9.4 逐点核实里漏掉的第四条**）

> **一跳一条缓冲的前提是：被交出去的缓冲里，那些"读输入"的 dispatch，不能在输入生命期之后才执行。**

接收链的输入是**电台的零拷贝缓冲**，生命期**按设计**锚在 `finish_symbol()` 返回上，池子只有 4 个
（`finish_oldest_symbol()` 注释原话："four buffers: the receive loop would block on the fifth"）。
**⇒ 不把这条生命期搬走，D1 的"一次提交"和"拿掉 507 µs 宿主等待"都不成立**（B 也要 A）。

---

## 3. ★ 用户的裁定（**长期有效，原样带过来**）

| # | 裁定 |
|---|---|
| ① | `mode=gpu` 不允许宿主兜底 |
| ② | dump 不算 CPU in the loop |
| ③ | 控制面融合分阶段做、**每阶段一次 OTA**（**腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag**）|
| ④ | **`merged` 保留为默认** |
| ⑤ | 顺序：~~K1 路 A~~（做完）→ **DFT/D1**（第 1 步 ✅、第 2 步 ❌ 已回滚、**下一阶段 = 输入生命期**）|
| ⑥ | **排序标准是"CPU 还剩几个参与点"（个数），不是耗时**；A（提交数）优先于 B（宿主空等）|
| ⑦ | **"GPU 一旦发动，就能不停顿地跑完 IQ → LLR"**；中途若还需要 CPU，那一点就算参与点 |

**⇒ 本阶段按 ③ 应当【不 tag】、回滚**（用户执行；回滚已经在代码里）。

---

## 4. ★★★ 下一步：**把输入的生命期搬到"被认领缓冲的完成"上**

> **本会话已完成 4.1 的【第 1、2、4 步】**（引擎侧 keep-alive §5.9.8、低层 PHY 交接 §5.9.9、
> **交出点接回 §5.9.10**）。
> **⇒ 下一会话：直接跑腿对（§4.3）**，判"输入生命期接上之后，交出行不行"。

### 4.1 要做什么

| 步 | 内容 | 判据 | 状态 |
|---|---|---|---|
| **1** | **DFT 引擎接受一个 keep-alive**：`retain_for_block()` 把凭据挂到当前打开的块上；引擎在缓冲完成时释放；被顶掉/淘汰/丢弃时也释放 | **机制单测五条臂全过**（含"没人认领也要还"与"提交路径也要还"）| ✅ |
| **2** | **低层 PHY 把接收缓冲的凭据交出去**：`retain_symbol_input()` + `defers_transform_execution()`；**先问再分配**（不延迟的后端连一次 `new` 都没有）| **守卫测试的延迟版**（`DeferredTransformsKeepTheirSamplesUntilTheirBlockCompletes`，已做反向验证）| ✅ |
| **3** | **池深实测**：接收循环会不会被"多留一两个缓冲"挡住 | 空口腿：`Real-time failures`、`ul_rx blocks`、CRC | ✅ **量了：池子被抽干**（§5.9.11）|
| **4** | **把低层 PHY 的 `release_block()` 调用点接回来**（`7fa657794c` 撤掉的那处）| 空口腿对（§4.3）| ✅ |
| **5** | **修掉池子泄漏**：`begin_stage_on_handed()` 少了 `close_held_buffer(e)`（那条 "a held buffer is never left behind" 不变量）| `keepalives released == attached`；`[ul_rx_pool] held` 稳定 | ✅ **已修并验证**（§5.9.12 ①）|
| **6** | **方案 A 的机制**：网格产出栅栏 + 兜底提交 + `ensure_grid_produced()`（用户已裁定 A）| 机制单测第 6 条臂（两种形状）| ✅ **§5.9.13** |
| **7** | **★ 下一件事：把 `ensure_grid_produced()` 接到 PUCCH 那条路上**（**必须在 PUSCH 之后**，否则死锁），然后重新武装交出 + 上腿 | 新的一条腿对 | ⬜ **下一步** |

### 4.1b 现在这条链长什么样（判读时要能背出来）

```
武装时：demodulator->defers_transform_execution() == true
     ⇒ puxch 对【每个变换】调用 retain_symbol_input()：new 一份 rx_buffer_handle + retain_input()
     ⇒ 引擎把它挂在"当前打开的块"上
     ⇒ 块在槽末被 release_block() 交出 ⇒ 凭据跟着缓冲走
     ⇒ 车道认领并提交 ⇒ 【缓冲完成时】凭据放掉 ⇒ 样本回电台池子
没人认领时：deposit 被顶掉/淘汰 ⇒ on_drop 钩子 ⇒ 立刻放掉（§5.9.8 的 arm 3）
```
**★ 未武装时**：`defers_transform_execution()` 假 ⇒ **不分配、不交接**，与改动前逐字节相同（131 同数）。

**★ 什么时候会出现"多留一个缓冲"**：凭据从"槽末"延长到"车道提交完成"（≈1 个槽），
所以同时被持有的槽缓冲从 ~1 个变成 ~2 个，池子是 4 个 ⇒ **余量应当够，但这就是第 3 步要量的东西**。

### 4.2 ⚠ 两条设计约束（**已解决，留档**）

1. **回调在 Metal 的完成线程上跑** ⇒ 结论：**可以直接放**。`rx_buffer_handle` 就是 `std::shared_ptr`，
   池子的 deleter 走 `weak_ptr::lock()` + `push_blocking()`（`blocking_queue` **文档原话 thread-safe**），
   **不碰 puxch 的任何状态**（`puxch_processor_impl.cpp` 的 `release_symbol_input()` 就是一句 `delete`）。
2. **一次提交承载多个符号的输入** ⇒ 结论：**每个变换一份凭据**（与 in-flight entry 自己那份并列），
   `shared_ptr` 的引用计数决定谁最后放手。

### 4.4 ★★★ 现在卡在哪：**网格有宿主消费者（PUCCH），而交出把网格的产出推到车道提交之后**

**这是 D1 的第四条硬约束**（§5.9.4 的逐点核实漏掉的），而且是**结构性**的：

| 事实 | 出处 |
|---|---|
| `pucch_processor_impl` 从 `resource_grid_reader` 读网格，**没有任何 device view** | `pucch_processor_impl.cpp:120/143` |
| 上层 PHY 在槽一交过来就处理它 | 与 PUSCH 同一份 `shared_resource_grid` |
| 交出把网格的产出推到**车道的提交**（晚一个槽）| 本会话的设计 |

**⇒ 宿主消费者读到还没写的内存**：候选臂 PUCCH **`metric=nan sinr=-inf`** vs 对照 **10924 条 `sinr=24.2dB`**。
**⇒ 已再次关掉交出**：`handover_allowed()` = `!grid_has_host_consumers() && ...`，而后者**返回常量 `true`**。

**⇒ 用户已裁定：走 A**（机制已实现并离线验过，见 §5.9.13；**还剩"接到 PUCCH 路上"这一步**）：

| 选项 | 内容 | 评价 |
|---|---|---|
| **A（推荐）** | **把等待从生产者搬到消费者**：宿主网格消费者（PUCCH）在**车道提交完成之后**才读网格 | **不新增参与点**（PUCCH 本来就要等 GPU 写完），且保住"一跳一次提交" |
| B | 给 PUCCH 一条设备路径 | 大工程 |
| C | 只融合"网格消费者"，DFT 仍及时提交 | 提交数没有改善 |

### 4.3 腿法（**⚠ 交出当前被关掉**；模式必须是 `gpu`）

```bash
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s33-d1-input-lifetime-base
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s33-d1-input-lifetime OCUDU_DFT_RELEASE_BLOCK=1
```

**判读（按 §2 的教训排序）**：

| 先看 | 期望 |
|---|---|
| `crc=OK/KO`（**按调制分层**）| **不劣化**（这是上次唯一真正抓住问题的读数）|
| `sinr` 与 CRC 是否**同时**变好/变坏 | 高 SINR + 全 KO = 数据错；低 SINR = 无线问题 |
| `real-time failures` | 不劣化（池深问题的签名）|
| `[metal_stats] dft handover ...` | `handed>0`、`taken>0`、**`evicted==0`**、`released_waits==0` |
| `dft commits` | 从 ~2 次/跳**塌下来** |

**★ 关于"日志没有退出块"**：若候选臂再次把上行弄坏，**照旧会没有 `[metal_stats]`/`handover` 读数**
（进程不会干净退出）。**⇒ 判"这一臂坏了"靠 CRC + UE 是否重连，别等仪表。**

---

## 5. 本会话踩过的坑（**新增，都是流程性的**）

### 5.1 ★ **"离线全绿"判不了一条只在真机上成立的假设**

第 2 步离线全绿（47/0、10/10、131、机制单测、武装 A/B、守卫负例）——**因为那些判据都在"有没有接线"这一层**，
而真正的假设（**输入生命期**）**离线根本不在视野里**。
**⇒ 教训：接线类改动，"离线全绿"只说明没接错，不说明它是对的；要先把"这条链上还有谁的生命期/所有权"列一遍。**

### 5.2 ★ 一个"多余的等待"可能是别人的**生命期锚**

`dft->wait_slot(slot)` 在注释里被写成"宿主等 DFT 做完"（性能问题），**它同时也是**
`puxch_processor_impl` 释放接收缓冲的**唯一依据**。
**⇒ 教训：拿掉任何 `wait`/`sync` 之前，先找出"谁把它的返回当成了别的事实的证明"**（这里是"样本已经没人读了"）。

### 5.3 仪表在**退出时**打印 ⇒ 坏掉的臂读不到仪表

候选臂没干净退出 ⇒ `handed=` 丢了 ⇒ 我现在**不能**从日志证明"交出确实发生了"
（只能由 `[dft_release]` 横幅 + 守卫条件推出）。**⇒ 下次给这类关键计数器加**"每 N 跳打一次"的心跳**，
或者把状态写进退出必写的文件**。

### 5.4 `ul_pipeline_probe_test.one_report_shape_per_pipeline_mode` 已知偶发（与本线无关）

`ctest -R "ul_pipeline_probe|puxch|lower_phy"` 里偶发失败；该二进制**不链接任何 Metal 库**，
断言是 `sleep_for(7ms)` 后要求 `mean_us ≈ 7000 ± 1000`——**时序脆**。碰到时改成对**记录值**的断言。

---

## 6. 工具、旋钮与仪表

| 名字 | 说明 |
|---|---|
| **`OCUDU_DFT_RELEASE_BLOCK=1`** | 交出路径的旋钮（**默认关**）。**当前已无调用点** ⇒ 武装它也不会交出任何东西（回滚后）|
| **`[metal_stats] dft handover handed= taken= superseded= evicted= outstanding= keepalives=released/attached (armed=)`** | **武装了就一定打这一行**（**但只在干净退出时**）。`evicted` 必须 0；`superseded` 允许非 0（地址复用，无害）；**`keepalives` 两个数必须相等**（不相等 = 有接收缓冲没还回去）|
| `OCUDU_GPU_STRICT=1` | strict 的离线/腿上覆盖口（回放**故意不发布模式**）|
| **`wip/neutral_vs_baseline.sh`** | **信息网**：逐字节对比归档基线（235 dump，走真的 Metal DFT 路）|
| `wip/value_net.py` / `wip/run_leg.sh` / `wip/leg_report.sh` | 门 / 跑腿 / 单腿判读 |

**★ 上腿前的三条自查**：
1. **模式对吗**（D1 的腿**必须 `gpu`**；`cpu_gpu` 下守卫拒绝）；
2. **判据会触发吗**（`handed>0`？CRC 分层比？）；
3. **开关武装了吗**。

---

## 7. 本会话的腿

| 腿 | 模式 | 头条 |
|---|---|---|
| `gnb_gpu_s32-d1-step2-base` | `gpu` | **对照：健康**（契约 8/8、host=0、2.06 次/跳、`released=0`）|
| `gnb_gpu_s32-d1-step2` | `gpu` + 旋钮 | ❌ **否证**：`crc=KO 942 / OK 46` @ `sinr=37.6 dB`；UE 反复重连；无干净退出 |

---

## 8. 未解 / 开放项（**按建议顺序**）

1. **★ 输入生命期**（§4）——**A 是"一次提交"与"拿掉宿主等待"共同的前提**；
2. **接到"被认领缓冲完成"上的 keep-alive** 是否与**前端栅栏代际**（§5.9.4 ⑤-1）冲突；
3. **`wait_all()`** 不覆盖交出去的块（§5.9.4 ⑤-2）；
4. **给离线补一个能跑完整 D1 路径的 harness**（`ul_chain_replay --dft` 目前 `return 0`，且本机**没有 TD 语料**）；
5. **关键计数器的心跳打印**（§5.3）；
6. `shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉（静默错址，9 个调用点）；
7. `merged` 下 `burst dispatches` 的 `channel_estimator=0` 是纯计数缺口；
8. §18.7 的 TA fence 实验仍未做；TA 链 = 80.1 µs；
9. `ul_pipeline_probe_test` 的时序脆断言（§5.4）。

---

## 9. 环境与流程

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
* **★ 新增测试/文件之后要 `cmake -S . -B build`**；
* **★ 改 `.metal` 后**：删 `.air{,.d}` 与 `.metallib` 再全量构建，并用 `xcrun metal-nm` 验证 kernel 真进去了；
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  `sudo -E bash …/run_leg.sh <mode> <label> [OCUDU_*=…]`——**sudo 由用户执行**（把命令以纯文本贴给用户）；
  **Ctrl+C 停**；判读 `bash …/leg_report.sh <log>`；
* **★ 上腿之前关掉手机 WiFi**；**判读前先看 `Real-time failures`**；**CRC 按调制分层比**；
* **★ 离线绝对 µs 不可信**；**只报比值/份额**；**判 ≥40 µs 的效应只能靠空口腿对**。

---

## 10. 一句话给新会话

**腿判了：D1 第 2 步的接线是错的——不是慢，是静默错数据（`sinr=37.6 dB` 却 942/46 的 CRC），
因为被交出去的变换会晚于"接收缓冲被回收"才执行，而那次宿主等待正是回收的依据；已回滚。**
**今天三条腿揭出三个缺陷：接收池泄漏（已修，`s33d` 的轨迹证明）、【网格有宿主消费者 PUCCH】（结构性，
交出因此关掉）、以及机制落地时单测抓到的两个真缺陷（锁内释放最后一个引用的自死锁；栅栏与完成回调无先后）。
用户已裁定走 A，**机制已完成并离线验过**（§5.9.13）；下一步是把它接到 PUCCH 那条路上，再重新武装交出、上腿。**
