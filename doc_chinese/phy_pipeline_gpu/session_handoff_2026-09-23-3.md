# 交接（入口） — S36 收口：**MMSE 雷已拆（默认 fenced 合批）+ 边缘块守卫 + 五项收口 + 六遍审计**；下一刀 = 三选一（下行延迟序列 / §8.2 对照仪表 / 先定 A1-2 归属）

> 本 memo 是**交接快照**：写于 2026-09-23 会话末，HEAD = `21d680eef4`。
> **开发过程中不要改本文件**；有需要记的，一律记进 `gpu_phy_pipeline_design_and_implementation.md`。
> 本文件覆盖 `5afb8abf50 → 21d680eef4`（本会话后半段）。**前半段**（`90ca0077ec → 5afb8abf50`：雷的定位、修法、翻默认、空口确认、Test 15 首版）
> 在 `session_handoff_2026-09-23-2.md` 里；**两份合起来才是完整入口**，但本文件已自包含当下状态与下一步。

## 0. 开机三件事（照做，不要跳）

```bash
cd /Users/jiachengwang/dev/ocudu
# 1) 树与戳：工作树只应有两个【用户的】config 改动；戳必须 == HEAD，否则重建（run_leg.sh 会拒绝）
git log --oneline -3; git status --short
grep -oE '[0-9a-f]{10}' build/hashes.h | head -1; git rev-parse --short=10 HEAD
#    不等时： touch build/hashes.h && cmake --build build --target gnb -j 8
# 2) 离线门 —— ★ 过滤器是【扩展后】的那条（旧的漏掉 121 个测试，见 §3.7）
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py | tail -1                       # 期望 captures=47 problems=0
ctest --test-dir build -R "metal|ul_pipeline_probe|puxch|pusch|lower_phy|du_low|o_du" | grep "tests passed"   # 期望 157/157
./build/tests/unittests/support/executors/ul_pipeline_probe_test | grep PASSED        # 6/6
./build/tests/unittests/phy/lower/lower_phy_test | grep PASSED                        # 528/528
bash doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh 32 | grep -c '^PASS'        # 5
bash doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 16 | grep differing               # 4× differing=0
bash doc_chinese/phy_pipeline_gpu/wip/edge_block_arms.sh 6 2>&1 | tail -3              # 默认 6/6 绿、=0 臂 6/6 红
# 3) 动手前先读 §3（本会话有 4 处更正，都是"先比较、后读定义"）
```

## 1. 一句话状态

* **默认路径（不带任何 env）干净且经空中确认**：契约 **8-of-8**、`stale=0`、crossings **0.00+0.00/跳**、
  `cbs/lane=2.00 (max=2)`、`dropped=0`、`gaps=0`；最近五条腿（s62/s63/s64b/s65/s67）反复确认。
* **本会话后半段四件已落地**：① **五项收口**（A1-1…A1-5，§5.9.99–101）；② **六遍全面审计**（§5.9.102–104 + §5.9.103/115/117）；
  ③ **三条契约反向臂 + 一条确定性边缘块臂**（§5.9.99/112/114）；④ **一条**重要发现**：离线门漏掉 121 个测试（§5.9.114）。
* **留下的都是"有读数、有下一步"的开放项**，没有"没人看过的状态句"（§5.9.115 把 13 处旧标记逐条裁决完）。

## 2. 本轮新增的工具与旋钮（新会话会用到的全部）

| 工具/旋钮 | 用途 | 坑 |
|---|---|---|
| `wip/leg_gate.sh [--slot-ms=0.5] <腿> [基线]` | 空中腿的 **9 条预登记判据**机械求值（有效性 2 条 + 硬门 5 条 + 池 + DFT 新格式）| **"读不出"按红算**（§5.9.97 踩过硬门印 `None` 像过）；30 kHz 小区记得 `--slot-ms=0.5` |
| `wip/ul_load.sh [--slot-us=N] <腿…>` | **先印负载、再印车道代价**；余量三个**与配置无关**的口径 + `rx pool verdict` | 旧的 `1 - residency/slot` 只在"一跳约一槽"时才有意义，工具已带条件打印 |
| `wip/edge_block_arms.sh 6` | Test 15 双臂（默认 6/6 绿、`=0` 6/6 红）| 红的判据要分开数（Test 3 先红 ≠ Test 15 抓到）|
| `wip/run_leg.sh` 新增 | `LEG_CONFIG=`（默认不变、文件不存在即拒）；**`--dryrun` 预检**：错选项**启动前**被拒（`exit 109`）；provenance 写进腿自己的 `.stderr`；**不再覆盖**调用者的 `--log.all_level` | 预检不碰射频（本机无 USRP 也 exit 0）|
| 契约反向臂的标准姿势 | `for (c : phy_pipeline_checks()) if (c.name == "…") c.evaluate();` —— **验判决，不验打印** | 臂必须在**计数器干净**的状态跑；不干净就自检 + `GTEST_SKIP` 并说明（`pusch_demodulator_deferred_chain_test` 的 `the_device_estimate_contract_arms` 是模板）|
| `LA UL MCS`（调度器）| UL 链路自适应的输入逐次打印：`mcs / pusch_snr / olla` | ⚠ **本构建没有 `--log.sched_level`**；`SCHED` 没有 per-logger 选项 ⇒ 用 `--log.all_level=debug`（或 `--log.lib_level=debug`，它设的是名为 `ALL` 的 logger）|
| DFT 普通路由三计数 | 契约证据行里的 `Plain route by why: N got their own command buffer (M …before any slot was told), K joined an open block` | 加它时**第一次加错了位置**（加在 grid-write/交棒那条路），证据行立刻暴露 ⇒ 计数要加在 `submit_at` |

## 3. 本会话必须知道的结论（多数是**更正**）

1. **契约 `dft radio inputs` 曾经名不副实**（§5.9.99）：旧式 `radio >= 0.99*committed` 比的是**两条不同口径**的计数器
   （n1 腿印 `392714 of 1` 恒过、n78 印 `301658 of 158845`＝190%）。**新判据 = `wrap_copies == 0`**，并配双臂。
2. **``ce device estimates`` 的宽松是有意的**（§5.9.112）：判据是 `device > 0`（"设备路径没整个消失"），
   注释写明宿主可以合法补个别跳；六条腿实测**全是 `N device, 0 host`** ⇒ 不要收紧成 `host == 0`（只会加误报）。
3. **`not_found` 不是功能缺口**（§5.9.117）：它 ≈ `late`（两条腿逐个相等）；真正"网格不可信"走**另一个**计数
   `ready_timeouts`（超时后 `return false`），**五条腿全 0**。`ocudu_metal_burst.mm:714` 是累加点。
4. **UL 链路自适应的读数不可跨 MCS 比较**（§5.9.107–110）：日志里 `PUSCH … sinr=` 是**解调器**的逐符号后均衡统计
   （带 −3.7 dB 经验偏移）；同一秒、同一宽度 51 PRB 下 QPSK/16QAM/64QAM/256QAM **完全重叠**（9.0–15.8 / 11.1–24.5 /
   14.7–25.9 / 8.8–21.7 dB）⇒ 它**不是**被调制缩放的信道量，但**也不能**当中位去比两个小区。
5. **环路是灵敏的**（§5.9.110）：把"上一次读数 → 这一次实际选的调制"配对，单调上升；
   且**高调制只与窄授权配对**（宽度中位 25→11→**5 PRB**），两个小区一样 ⇒ 是**调度器策略**。
6. **n1 的上行之谜是射频环境**（§5.9.106/109）：满缓冲上传时被调度在 **25 PRB @ MCS0**（手机有 **10–15 dB** 功率余量，
   窄授权同秒能读 9.5–13.3 dB、宽授权只 3.5–5.1 dB）；用户已裁定**换环境再测**（怀疑商业宏小区同频干扰）。
7. **离线门曾经漏掉 121 个测试**（§5.9.114）：旧过滤器 `…|puxch|…` 匹配 **0** 个名字带 `pusch` 的测试
   （含拥有设备侧信道估计路径的那个二进制）。加 `pusch` ⇒ **157 个全过** ⇒ **门是瞎的，不是宽的**。**用扩展后的那条。**
8. **RX 池判据**（§5.9.100）：`free_min ≥2` OK / `==1` TOUCHED（设计背压边界）/ **`==0` DRAINED（接收线程被阻塞）**；
   已加**一次性 EMPTY WARNING**（`lower_phy_baseband_processor.cpp:139`）——**正向触发待观测**（池有硬下限 8，造不出来）。
9. **DFT 独立提交的归属仍未定**（§5.9.111/113）：n78 上 11346 个槽的变换走普通路由（每变换一条命令缓冲），
   21547 个槽走交棒；**仪表已就位**（§2 的第三计数），一条 n78 腿即可分开"没人告诉它槽"与"block 开着没用"。

## 4. 下一刀（三选一，按用户偏好排序）

| 选项 | 内容 | 需要 |
|---|---|---|
| **C（最快出结论）** | 跑一条 n78 腿读 `Plain route by why:` 三个数 ⇒ **定 A1-2 的归属** ⇒ 再谈修法 | 一条腿（腿法见 §2；`run_leg.sh` 现在会预检参数）|
| **A** | **`#10` 第一步**：给下行加一条与 `[ul_pipeline]` 同形的延迟序列 —— **先出 1 页设计**（两条边界 + 判据），再实现 + 单测 | 纯离线（注意 §5.9.116③ 的更正：下行**今天没有设备侧工作**，穿越记账是空的）|
| **B** | **§8.2**：恢复"设备 vs 宿主逐点对照"仪表（§8.2 记着代码形状），在金属 MMSE 单测里做逐点比对 | 纯离线 |

## 5. 其余开放项

| # | 项 | 现状 / 需要 |
|---|---|---|
| — | **A1-2 归属** | 仪表已就位；**待一条 n78 腿**（见 §4 C）|
| — | **RX 池 EMPTY WARN 正向触发** | 待观测；最小强制法：`--log.phy_level=debug` 跑短腿（近 dry 时每 pop 一行会拖慢上行侧）|
| **#10** | 下行同口径记账 | 盘点已完成（§5.9.116），第一步见 §4 A |
| — | **§8.2 LSE 发散**（设备 vs 宿主）| 需重测（路径已重构多次）；`OCUDU_CE_LS_CHECK` 仍在 |
| **#12** | LA 错配 + **n78 顶格 MCS 的 KO 率 14.2%** | 判据与四个 A/B 已登记（§5.9.101 ②）；n1 侧被"换环境"挡住 |
| **#13** | 延迟批次等待（`defer_wait`）| 下一步 = §5.9.81 的三段拆分；需空口 |
| **#3 / #11** | 认领宽限期 / TA fence 实验 | 需空口 |
| **#16** | Ubuntu 工作树 checkout | 用户侧家务 |
| — | `1.262` 孤例 | 只在复发时查（`OCUDU_CE_NV_CHECK=1` 分家族）|
| — | Test 3/9 偶发 | **只在前缀形态**下（`OCUDU_CE_CORR_FENCED=0`）；默认路径 40 跑 0/0/0 |

## 6. 纪律（沿用 + 本会话新增）

1. **先读定义，再比较**（本会话**四次**更正都栽在这：`dft radio inputs` 的分母、PUSCH `sinr` 的归属、
   LA 的输入、`not_found` 的含义）。看到"两组数不一样"时，第一件事是问**这两个数是不是同一个量**。
2. **统计量选错，守卫等于没有**：均值把单跳摊薄 1/n；**单位也要对齐**（`staged` 数的是 wrap 次数，不是变换数）。
3. **臂要跑在计数器干净的状态**：不干净就**自检 + SKIP 并说明**，别让"判决动不了"的臂假装通过。
4. **计数器的可见性决定臂的可行性**：公开静态（`notify_wrap_misaligned`、`count_host_read_site`）⇒ 可离线做臂；
   TU 内静态 ⇒ 只能从真实路径驱动（腿/夹具）。
5. **改工具前先想"它本该拦住我"**：`run_leg.sh` 的 `--dryrun` 预检就是被一次错选项烧掉一条腿之后加的；
   门过滤器漏模块同理 —— **门/工具的缺口比被测对象的缺陷更贵**。
6. **不许半成品**：新仪表要么设计完整 + 单测，要么只登记（D 的第一步就是这么处理的）。
7. 旧纪律照旧：反向臂先行、单次绿等于零证据、文档只追加（插入用**正文行**作锚点）、
   提交前 `git diff --numstat` 删除列为 0、腿必须 Ctrl-C 停并等收尾块。

## 7. 本区间的提交（`5afb8abf50 → 21d680eef4`，26 个）

```
0c9104566a 交接 memo（S36 前半段，session_handoff_2026-09-23-2.md）
db8580e30e 5.9.96     重活腿预登记：先把"余量"的含义钉住
5aff42ad51 5.9.97     s63 无效；s64"接不上"= 上行 30 dB 增益差
c16018001c 5.9.97(6)  s64 契约未过（dft radio inputs）
a216386c0c 5.9.97(7)  更正：s64 的 DFT 整条腿掉出融合路线（每变换一次 commit+wait）
dbdbb9f2c3 5.9.98     n78（rx_gain=70）：接入恢复、契约 8/8、大跳让每跳成本 ×2.4
2e8ec6ae6d 5.9.99     修 dft radio inputs 判据 + 双臂；A1-2 = 覆盖缺口
684d102fa6 5.9.100    RX 池判据 + 一次性 EMPTY WARN
6afafa95c0 5.9.101    A1-3/A1-5 收口
1cd7a76e28 5.9.101(1b) 第 5 遍判据脚本化（wip/leg_gate.sh）
c61d980481 5.9.103    遗留项普查逐条裁决
6a943c4ed7 5.9.102    六遍审计
29bc683f6a 5.9.104    审计最终态复验
b451d97dd7 5.9.105    第 5 遍阻塞记账 + 等价性论证
eddaa02b78 5.9.106    第 5 遍判读（审计闭环）
1ce66ec5dc 5.9.107    更正：n1 上行不是"链路差 16 dB"
b22648e7d5 5.9.107(6) 日志的 sinr 是解调器的量
f2872c4371 5.9.108    UL LA 闭环定位（+ 第二次更正）
49fb0e56fd 5.9.109    成对 LA 腿：机制 B 被否、宽度效应 6–10 dB
5a5006cb2a 5.9.110    离线反演：环路灵敏；高调制只与窄授权配对
6228686759 5.9.111    A1-2 归属未落地（第三次更正）
d4736af7f8 5.9.112    E：ce device estimates 反向臂（独立 ctest 入口）
ab14582332 5.9.113    A：DFT 普通路由三计数 + 双臂；★ 门漏 121 个测试
5148bf437b 5.9.114    B：wraps/crossings 两条臂 + 三条配方
9cc37db2b6 5.9.115/116 C：13 处裁决；D：下行是空的
4d4811b553 5.9.117    C 最后一条：not_found 关闭
21d680eef4 5.9.116(3) D 第一步更正（下行无设备侧工作）
```

## 8. 关键坐标（行号对应 `21d680eef4`）

* 设计文档：`doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md`（11300+ 行）
  —— 本会话记录 **§5.9.96 → §5.9.117**；**§5.9.102（六遍审计）、§5.9.110（LA 反演）、§5.9.114（门缺口）** 最值得先读；
* 契约检查表：`include/ocudu/phy/phy_pipeline_contract.h`（`phy_pipeline_checks()` / `phy_pipeline_check::evaluate`）
  —— 八项的**判决表达式**逐个在：`ocudu_metal_queue.mm:191`（wraps）、`pusch_demodulator_impl.cpp:819`（ce）、
  `lower_phy_baseband_processor.cpp:217`（continuity）、`uplink_processor_impl.cpp:205/216/249`（cfo/metrics/assembly）、
  `phy_pipeline_crossings.h:512`（crossings）、`ocudu_dft_metal_engine.mm:270`（dft radio inputs）；
* DFT 引擎：`lib/phy/generic_functions/metal/ocudu_dft_metal_engine.mm` —— 普通路由三计数在 **`submit_at`**（`block_accumulating` 分支），
  证据行在 **267**；`set_lane_slot` 在 1399；grid-write/交棒在 `submit_slot_grid_write`（1311，**不要**在那里计数）；
* 交棒/网格就绪：`lib/phy/metal/ocudu_metal_burst.mm` —— `claim_grid_production`（704）、`not_found` 累加 **714**、
  `ensure_grid_produced`（728，`generation==0 ⇒ return true`；超时 ⇒ `ready_timeouts` + `false`）；
* UL 链路自适应：`lib/scheduler/ue_context/ue_link_adaptation_controller.cpp:148`（`calculate_ul_mcs`）、`LA UL MCS` 在 **163**；
  `ul_mcs` 默认区间 `include/ocudu/scheduler/config/scheduler_expert_config.h:140`（`{0,28}`）；
* RX 池：`lib/phy/lower/lower_phy_baseband_processor.cpp` —— 计数结构 39、一次性 WARN **139**、汇总 82；
* 脚本：`doc_chinese/phy_pipeline_gpu/wip/` —— `leg_gate.sh`（新）、`ul_load.sh`、`edge_block_arms.sh`、`run_leg.sh`、
  `corr_fenced_ab.sh`、`mmse_outlier_rate.sh`、`l1_*_arms.sh`；
* 腿日志：`doc_chinese/phy_pipeline_gpu/wip/logs/` —— 本轮 `s61-*`、`s62-default-fenced`（默认基线）、`s63-heavy-ul`、
  `s64-heavywide`（n78，两次：gain40 接不上 / gain70 接上）、`s65-heavy-ul`、`s66-la-n1` / `s67-la-n78`（成对 LA 腿）。

## 9. 一句话给新会话

**默认路径的雷已经拆掉并空中确认，边缘块守卫、五项收口与六遍审计都闭环，门扩到 157 项仍全绿。**
**不要再修雷、不要再重做边缘块用例、不要再用旧的 36 项过滤器。**
**先读 §3 的四条更正**（本会话的教训是"先读定义再比较"）。
下一刀三选一（§4）：**跑一条 n78 腿定 A1-2 归属**（最快出结论）、**开下行延迟序列**（`#10` 第一步，先出 1 页设计）、
或 **恢复 §8.2 的设备/宿主逐点对照仪表**。
