# 交接（入口） — S19：**D1 第 1 步已完成且逐字节零影响；真正的门槛是【一段内存只能有一个 `MTLBuffer` 对象】，不是栅栏**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **不要读 `session_handoff_2026-09-21-1.md`**（S17 的状态：那时 D1 第 1 步还没做，
> 而且它的"两条硬约束"漏了本会话发现的那一条）。**本文件已包含全部结论。**
> **技术细节全在常驻设计文档 `gpu_phy_pipeline_design_and_implementation.md`**，
> 本会话新增的是 **§5.9.5**（**★ 它就是 D1 第 2 步的开工依据**）。

---

## 0. 相对上一份（`-1.md`）的变化

| 项 | 变化 |
|---|---|
| **D1 第 1 步** | ✅ **做完了**（`-1.md` 说"可以立刻开工"）：`release_block()` + 默认关闭的旋钮 + 机制单测 |
| **★ 新增一条硬约束** | **别名对（同一段内存的两个 `MTLBuffer` 对象）之间没有任何顺序，栅栏也救不了**。`-1.md` §4.4 只列了"栅栏"和"`wait_all()`"两条，**漏了这条**——而它是唯一会让 D1 **静默算错**而不是变慢的那条 |
| **门** | `ctest -R metal` **9/9 → 10/10**（新增的机制单测进来了）。其余同数 |
| **新旋钮** | `OCUDU_DFT_RELEASE_BLOCK`（默认关）；`[metal_stats] dft` 多了 `released=` / `released_waits=` |
| **一条已知偶发** | `ul_pipeline_probe_test.one_report_shape_per_pipeline_mode` 偶发失败（**与本线改动无关**，见 §5.3）|

---

## 1. 一句话状态

**工作树 HEAD = `186566113d`**（S17 的 `b3404fcc03` + **本会话 1 个代码提交**；快照提交跟在它后面）。
**开机第一件事就是自查戳记**：
```bash
git rev-parse --short=10 HEAD && grep build_info build/hashes.h
```
**两个短哈希必须相同**（不同就 `touch build/hashes.h && cmake --build build --target gnb`，坑 35 那条不是这里的坑，是**流程**坑）。
工作树只剩**用户自己的两个 config**（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`gnb_rf_b200_tdd_n78_20mhz.yml`），**别动**。
**提交全部只在本地，没有推送。**

**门（本会话末次复跑，全部在"旋钮关"的出厂路径上）**：

```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py             # 47 捕获 0 问题
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 8/8
ctest --test-dir build -R metal                                   # 10/10（9 + 新增的机制单测）
ctest --test-dir build -R "ul_pipeline_probe|puxch|lower_phy"     # 5/5（⚠ 见 §5.3 的偶发）
bash doc_chinese/phy_pipeline_gpu/wip/neutral_vs_baseline.sh      # 131 differing-bytes（= S15/S17 同数）
```

**★ `neutral_vs_baseline.sh` 这次比 `value_net` 更有说服力**：它走的是 `ul_chain_replay` 的
**真会用到 Metal DFT + 网格写**的那条路，逐字节对比归档基线（235 个 dump）。
**131 = 上一会话同数 ⇒ 本步一个字节都没动出厂路径。**

---

## 2. ★★ 本会话做了什么（**一句话：第 1 步交付了，而且发现第 2 步真正的坑不在预期的地方**）

### 2.1 交付物

| 项 | 内容 |
|---|---|
| **`dft_metal_engine::release_block()`** | 结束 encoder、**不提交**、把命令缓冲交给调用方（不透明 `void*`，实为 `id<MTLCommandBuffer>`）。**不提交、不计数、不发前端栅栏、不进前端链** |
| **`OCUDU_DFT_RELEASE_BLOCK`** | **默认关**。关着时 `release_block()` 返回 `nullptr` 且**不碰任何状态** |
| **队列随旋钮一起选** | 武装时块建在**后端队列**（缓冲属于创建它的队列，而车道在后端队列提交）|
| **网格改走进程级 cache** | 武装时 `submit_slot_grid_write()` 的网格用 `shared_queue::wrap_no_copy()`，**偏移跟着 `setBuffer:offset:` 走**；映射失败**拒绝**dispatch，**绝不退化成副本** |
| **`wait_slot()` 不再假装** | 交出去的槽位：**报错 + 返回 false**，而不是返回一个被满足的等待 |
| **仪表** | `[metal_stats] dft ... released=N released_waits=M`（**M 必须是 0**）|
| **机制单测** | **新增 ctest `dft_release_adopt_metal_test`** |

### 2.2 ★★★ 发现：`-1.md` §4.4 漏掉的那条硬约束（**这是本会话最重要的产出**）

> **别名对（同一段内存上的两个 `MTLBuffer` 对象）之间没有任何顺序；`memoryBarrierWithScope:MTLBarrierScopeBuffers` 排不了它们，两个 encoder 也排不了。**

依据：**坑 35**（S13-P0 融合 NaN，一天半）＋ `wip/metal_alias_order.mm` 的 **case G（别名对，200/200 FAIL）**
与 **case F（同一对象，200/200 PASS）**。

**⇒ 逐引擎核对"谁和谁绑同一个对象"，DFT 是唯一的例外**：

| 引擎 | 网格用哪个 wrap | |
|---|---|---|
| 估计器 / 均衡器 / 解映射器 | `shared_queue::wrap_no_copy()`（**进程级**、按地址、带 containment）| ✅ |
| **DFT（`dft_engine_impl::wrap_buffer()`）** | **引擎私有 `buffer_cache`**（按指针、无 containment、**不共享**）| ❌ |

**今天不出问题，只是因为 DFT 与消费者之间有"命令缓冲级"的同步**（前端栅栏 / 宿主 `wait_slot()`）。
**D1 把它们放进同一条缓冲后，这层同步就没了** ⇒ **P0 的签名：静默错数据**。
**⇒ "交出缓冲"的接口必须同时把网格的 wrap 换成进程级的**，否则交出去的缓冲里消费者读不到 DFT 写的东西。

### 2.3 机制单测的读数（`dft_release_adopt_metal_test`，一个自由变量）

| 臂 | 读者绑的对象 | 结果 |
|---|---|---|
| **shared** | 消费者的 mapping（**DFT 就写在这个对象上**）| **20/20 读到块写的值** ✅ |
| **private** | 另一个 `newBufferWithBytesNoCopy` 对象（引擎私有 cache 的形状）| **20/20 读到的还是写前内容**，0/20 侥幸 ✅ **陷阱当场复现** |
| **arm 0（默认）** | —— | 旋钮没设 ⇒ `release_block()` 拒绝；`commit_open()`/`wait_slot()` 照旧，300 个元素全写对 ✅ |

**private 臂不能省**：Metal 的顺序是**按对象**的，"读者读到了数据"完全可能只是驱动碰巧串行化了——
**空洞断言比没有断言更糟**。另外单测还钉住了：释放后 `cb.status == NotEnqueued`（引擎确实没提交）、
`has_open()==false`、`wait_slot()` 返回 false、以及**非零绑定偏移**那条路。

---

## 3. ★ 用户的裁定（**长期有效，原样带过来**）

| # | 裁定 |
|---|---|
| ① | `mode=gpu` 不允许宿主兜底 |
| ② | dump 不算 CPU in the loop |
| ③ | 控制面融合分阶段做、**每阶段一次 OTA**（腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）|
| ④ | **`merged` 保留为默认** |
| ⑤ | 顺序：~~先 K1 路 A~~（已做完）→ **DFT/D1** |
| ⑥ | **★ 排序标准是"CPU 还剩几个参与点"（个数），不是耗时**。所以 **A（提交数）优先于 B（宿主空等）** |
| ⑦ | **★ 判据是"GPU 一旦发动，就能不停顿地跑完 IQ → LLR"**；中途若还需要 CPU 才能继续，那一点**就算参与点** |

---

## 4. ★★★ D1 第 2 步的开工依据

### 4.1 目标形态

```
今天：[DFT 缓冲(前端队列) commit] ──宿主等 ~507µs──→ [网格] → 车道缓冲打开 → 权重 → 均衡 → commit
目标：[一条缓冲] DFT 14 个变换 → 提取 → 权重 → 均衡 → 解映射 → 【一次 commit】→ GPU 一口气跑到 LLR
```
**⇒ 提交数 2.83 → 1.00/跳。**

### 4.2 ✅ 第 1 步已经替第 2 步解决掉的三件事

| 事 | 状态 |
|---|---|
| **对象同一性**（2.2 那条）| ✅ 武装时网格已走进程级 cache；单测证明消费者读得到 |
| **队列**（缓冲属于创建它的队列）| ✅ 武装旋钮**自动**把块建在后端队列，不需要人肉对齐两个旋钮 |
| **句柄的生命周期** | ✅ 引擎在释放下一个块之前**强引用**着它（句柄是 +0 引用）|

### 4.3 ⚠ 第 2 步仍必须正面回答的三条（§5.9.5 ⑤）

| # | 事 | 为什么 |
|---|---|---|
| 1 | **解绑的缓冲上没有前端栅栏信号** | 前端代际是"每次前端提交 signal 下一代"。块被交出去后谁提交它谁负责；**代际还要不要推进、`front_end_wait()` 会不会等一个没人 signal 的代际（P0 的签名），必须正面回答**（§5.9.4 ⑤-1 仍然有效）|
| 2 | **`wait_all()` 不覆盖它** | 它 drain 的是前端链，而交出去的块由车道在后端队列提交（§5.9.4 ⑤-2）|
| 3 | **C++ 侧还差一座桥** | `release_block()` 返回 ObjC 类型（不透明 `void*`），`dft_processor_grid_write` 是纯 C++ 接口 ⇒ 要把"取句柄 → `shared_burst::adopt()`"放进一个 **ObjC++ 的接缝** |

### 4.4 建议的下一步形状（一次一处、验一处）

1. **先只做"接上但不改行为"**：让车道在槽末拿到句柄并 `adopt()`，**宿主等待先不动**。
   判据：**`released_waits` 必须是 0**（说明没有宿主消费者漏网）、`cbs/lane` 仍 **1.00**、
   `dft commits` **下降**、契约 `ce device estimates`/`equalizer ch_re` 仍 **host=0/0**、**CRC 不劣化**；
2. 再把宿主那一次 `wait_slot()` 拿掉（§5.9.2 ⑤ 的 `OCUDU_DFT_HOST_WAIT=0` 形状）；
3. 最后收 `wait_all()` 与栅栏归属。

**⚠ 只报比值/份额，不要报绝对 µs**（§9）；**≥40 µs 的效应只能靠空口腿对**。

### 4.5 腿法（**⚠ 模式必须是 `cpu_gpu` 或 `gpu`**）

```bash
# 对照（Metal DFT + 默认）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh cpu_gpu s31-d1-step2
# 候选（旋钮作为【位置参数】传，别放 sudo 前面）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh cpu_gpu s31-d1-step2b OCUDU_DFT_RELEASE_BLOCK=1
```

**判读前先看两件事**：① `[ul_dft_wait]` 是否有样本（`no samples` ⇒ **模式选错了**，§5.2 老坑）；
② `[dft_release]` 横幅在不在（**报旋钮臂之前先看它**）。

---

## 5. ★★ 本会话踩过的坑

### 5.1 单测名里没有 `metal` ⇒ 它**不进** `ctest -R metal` 这道门

第一版叫 `dft_release_adopt_test`，注册完 `ctest -R metal` **仍然是 9/9**——**新测试根本没被跑到**，
而"9/9 通过"看起来完全正常。**⇒ 改名为 `dft_release_adopt_metal_test` 后才是 10/10。**
**教训：加测试之后要确认它在【门】的选择器里，而不是在 ctest 的总表里。**

### 5.2 ★ 把"新引擎加入一条链"当成纯编排问题（本会话最大的时间去向）

`-1.md` §4.4 把门槛写在"栅栏"上（`adopt()` 不编码缓冲级栅栏、`wait_all()` 只 drain 前端链）。
**开工核对时才发现真正会让 D1 算错的是"别名对无序"**（§2.2），而它**在 `-1.md` 里一个字都没有**。

**⇒ 教训（与坑 4/31/34/35 同族）**：**"这条缓冲会被谁读"要按【对象】逐段核对，不能按【阶段】核对。**
一个新的生产者加入已有的链之前，先回答一句：**"它写的那段内存，和消费者绑的是不是同一个
`MTLBuffer` 对象？"**——不是同一个，后面的栅栏、encoder 边界、queue 顺序**全都无效**。

### 5.3 `ul_pipeline_probe_test.one_report_shape_per_pipeline_mode` 偶发失败（**与本线改动无关**）

门里**失败过 2 次**（头 4 次运行里），此后**连续 42 次通过**（含 25 次专门重跑）。判它不是本步回归：
1. 该二进制**不链接任何 Metal 库**（`link.txt` 里 "metal" 出现 **0** 次），而本步只改了 `ocudu_dft_metal`；
2. 它的断言本身**时序脆**：`sleep_for(7ms)` 后要求 `mean_us([ul_rx_wait]) ≈ 7000 ± 1000`。

**⇒ 记为"已知偶发"。**（下次碰到时把它改成对**记录值**的断言，别对 `sleep` 断言。）

---

## 6. 工具、旋钮与仪表（**本会话新增的全部在这里**）

| 名字 | 说明 |
|---|---|
| **`OCUDU_DFT_RELEASE_BLOCK=1`** | **新增**：武装释放路径（**默认关**）。武装时：块建在**后端队列**、网格走**进程级 wrap**、`release_block()` 才肯交出缓冲。**上腿前先看 `[dft_release]` 横幅** |
| **`[metal_stats] dft ... released=N`** | **新增**：交出去的块数（0 ⇒ 这条路径没被走到）|
| **`[metal_stats] dft ... released_waits=M`** | **新增**：**给不出的等待**（交出去的槽位被 `wait_slot()` 点到）。**正确接线时必须 0** |
| **`dft_release_adopt_metal_test`** | **新增 ctest**：release → adopt → 消费者读到 DFT 的网格（一个自由变量、两个方向都测）|
| `OCUDU_DFT_OPEN_BLOCK=0` | DFT 每槽 14 变换共用一条缓冲**关掉**（默认开）。**D1 会碰它** |
| `OCUDU_DFT_BACKEND_QUEUE=1` | DFT 提交到后端队列（**实验**；⚠ **`wait_all()` 语义会被它破坏**）。**武装 release 会自动选它** |
| `OCUDU_UL_FRONTEND_FENCE=1` | 前端栅栏（opt-in，默认关）——**用它的臂必须显式设 1** |
| **`wip/neutral_vs_baseline.sh`** | **信息网**：逐字节对比归档基线（**235 个 dump**，走真的 Metal DFT 路）。**读数 131**；**它比 `value_net` 更能证明"出厂路径没动"** |
| `wip/value_net.py` / `wip/run_leg.sh` / `wip/leg_report.sh` | 门 / 跑腿 / 单腿判读 |

**★ 上腿前的三条自查（长期有效）**：
1. **模式对吗**（D1 必须 `cpu_gpu`/`gpu`；`cpu` 模式下 **Metal DFT 不存在**）；
2. **判据会触发吗**（如 `[ul_dft_wait]` 必须有样本、`released` 必须非 0）；
3. **开关武装了吗**（依赖 `OCUDU_UL_FRONTEND_FENCE` 的臂必须设 1）。

---

## 7. 本会话的腿

**本会话没有空口腿**——第 1 步的判据全是**离线**的（门 + 机制单测 + 逐字节网），
这是它"自包含"的体现。**第 2 步开始才需要上腿。**

---

## 8. 未解 / 开放项（**按建议顺序**）

1. **★ D1 第 2 步**（§4.3 的三条 + §4.4 的三小步）——**第 1 步已把它的三个前提解决了**；
2. **§5.9.5 ⑤-1 的前端栅栏代际问题**：块被交出去后谁 signal 下一代（**P0 的签名风险**）；
3. `s25` 的时间轴只覆盖一次运行的 512 个槽（约 0.5 s），不是全程分布；
4. **`wait_all()` 在 DFT 常驻车道队列后必须改**；
5. **§5.8.12 那条旧读数需要重读**（栅栏默认没武装 ⇒ 很可能是假阴性）；
6. `merged` 下 `burst dispatches` 的 `channel_estimator=0` 是纯计数缺口；
7. `merged` 的 +471 µs 里 ~340 µs 排队与 64QAM CRC 变差的单变量隔离未做；
8. **`shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉**（静默错址，9 个调用点）——
   **本步的单测只覆盖了 `wrap_no_copy()` 自己那条路，`wrap_shared()` 那条仍在**；
9. §18.7 的 TA fence 实验仍未做；TA 链 = 80.1 µs；
10. K1 那条"每块列收尾 barrier"的 6% 理论移除：已推理为安全但**未上腿**；
11. **`ul_pipeline_probe_test` 的时序脆断言**（§5.3）。

---

## 9. 环境与流程

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
* **★ 新增测试之后要重新 `cmake -S . -B build`**（否则 `cmake --build --target <新目标>` 会说没有这个目标）；
* **★ 改 `.metal` 后的正确姿势**（本会话没改 .metal；**机制单测的 reader kernel 是运行时编译的，就是为了不碰它**）：
  ```bash
  rm -f build/lib/phy/generic_functions/metal/ocudu_mmse_inv.air{,.d} lib/.../ocudu_mmse.metallib
  cmake --build build --target ocudu_mmse_metallib -j 10
  xcrun metal-nm lib/…/ocudu_mmse.metallib | grep mmse_inv     # 验证 kernel 真的进去了
  ```
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh <mode> <label> [OCUDU_*=…]`
  —— **sudo 由用户执行**（把命令以纯文本贴给用户）；**Ctrl+C 停**；
  判读 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`；
* **★ 上腿之前关掉手机的 WiFi**；**判读前先看 `Real-time failures` 是否为 0**；**CRC 按调制分层比**；
* **★ 离线绝对 µs 不可信**（宿主 GUI 抢 GPU）：**先跑空臂**，空臂宽度 ≥ 要判的效应 ⇒ 该次读数作废；
  **只报比值/份额**；**判 ≥40 µs 的效应只能靠空口腿对**；
  `last_gpu_wait_us()` 在单发下是**队列跨度不是 kernel 时间**。

---

## 10. 一句话给新会话

**D1 第 1 步已交付：`release_block()` 就位、默认关闭、逐字节零影响（131 = 同数），
而它顺手挖出了第 2 步真正的门槛——"一段内存一个 `MTLBuffer` 对象"，并已经解掉。**
**第 2 步可以开工，但先读 §5.9.5 与本文 §4.3 的三条（栅栏归属、`wait_all()`、C++ 桥），
一次只改一处、`released_waits` 必须为 0。**
