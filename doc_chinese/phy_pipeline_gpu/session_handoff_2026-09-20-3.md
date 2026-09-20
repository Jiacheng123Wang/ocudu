# 交接 — **P0 结案 + P1/P2 落地并空中验证（各有 tag）+ P3 实现并量出代价 + 瓶颈分析**

> 上一份：`session_handoff_2026-09-20-2.md`（P0 收尾 + P1 起步；**本文件是新的入口**）。
> 常驻设计文档：`gpu_phy_pipeline_design_and_implementation.md`
> —— 本会话新增/改写的章节：**§5.4**（进度）、**§5.8.2**（P1/P2/P3 行）、**§5.8.4**（barrier 结案）、
> **§5.8.5 P0-(c)**（病因/机制/修法/判据）、**§5.8.6**（P1 空中）、**§5.8.7**（P2 离线）、
> **§5.8.8**（P2 空中）、**§5.8.9**（P3 离线）、**§5.8.10**（P3 空中 + 决策）、**§5.8.11**（★ 瓶颈分析）、
> **§6.3**（新增归档基线中性网）、**§7 坑 34/35/36**、旋钮表（`EDGE_FUSE`/`HOLD_EXTRACTION`/`LANE_ORDER`）。

---

## 1. 一句话状态 + 下一步

**HEAD = `ceeb7f9a77`**（已推送，工作树干净）。tag：**`gpu_lane_commit_p1`**、**`gpu_lane_commit_p2`**。
本会话共 **9 条腿**（P1 两条、P2 两条、P3 两条，另有文档/重跑），全部离线分析、无遗留进程。

| 阶段 | 状态 |
|---|---|
| **P0**（诊断）| ✅ **结案**：融合路线 NaN 的病因是"**同一段内存上两个 `MTLBuffer` 对象 ⇒ 没有任何顺序**"；已修（`8890d283f6`）|
| **P1**（尾组那次提交）| ✅ **离线 + 空中全过**，tag `gpu_lane_commit_p1`：`cbs/lane` **3.73 → 3.00**（受控 A/B）|
| **P2**（权重那次提交）| ✅ **离线 + 空中全过**，tag `gpu_lane_commit_p2`：`cbs/lane` **3.00 → 2.00**、`mmse_ce commits`/跳 2 → 1 |
| **P3**（整跳一次提交）| ✅ **已实现并量清**（`2926c684ab`）：**机制完美**（1.0002 次提交/接收、lane gap **0**、四条标准全过），**慢**（中位 2016 → 3666 µs，但该数被负载混淆，见 §5.8.10 的更正）⇒ **用户裁定：不翻默认，改走跨跳流水** |
| **下一步** | ⚠ **但 §5.8.11 的量测说跨跳流水没有余量**（宿主每跳只有 ~16 µs 的活、等 GPU ~665–887 µs）⇒ **要用户重新定向**（见 §3）|

---

## 2. 本会话的三件技术成果

### 2.1 P0：Metal 的顺序按 `MTLBuffer` 对象，不按地址（`8890d283f6`）

同一段宿主内存上建**两个** `MTLBuffer` 对象，两者之间**没有任何顺序**——`memoryBarrierWithScope` 排不了，
不管 barrier 放哪、dispatch 怎么编。引擎的零拷贝缓存按**指针**建映射，合并批次的**边组**槽位从
`gpu_a`/`gpu_r_hp` 里 `sys_offset` 个 stride 处开始 ⇒ 拿到第二个对象 ⇒ corr 前缀的写与 K1 的读**无序**
⇒ K1 读到未写的槽 → NaN → 权重读到 NaN，而**宿主在完成时刻看到的是写好的槽**（写最终落地、只是晚于读）。
**机制测量**：`wip/metal_alias_order.mm`（同对象 PASS/200；别名对 FAIL/200，两个方向都测）。
**修法**：`wrap()` 返回 `mapped{buf, offset}`，"一段内存一个对象"（与进程级缓存 `wrap_no_copy()` 的
containment lookup 同一条规则）。**判据**：235 个归档 dump 0 差异；融合 == 独立（27 语料 + 20 窄捕获逐字节相同）。

### 2.2 P1/P2：控制面从 3.73 做到 2.00（各一次 OTA，各有 tag）

* **P1**（`ba4cd9f781`）：尾组 corr 编进批次自己的命令缓冲（`OCUDU_CE_EDGE_FUSE` 默认 1）。
  空中受控 A/B：`cbs/lane` **3.71 → 3.00**，三处计数同减 0.71；契约 8/8、0 穿越、RF 0、CRC 98.24%。
* **P2**（`3df49a6d3b`）：提取的命令缓冲**留给**权重（`pilots_stage::hold_for_weights`，权重在同一条缓冲上
  开第二个 encoder）。空中受控 A/B：`cbs/lane` **3.00 → 2.00**、`mmse_ce commits`/跳 **2.000 → 1.000**、
  `extraction commit → weights commit` 系列**为空**。P2 腿是本线延迟最低的一条（mean/median **2103.5/1993.1 µs**）。
  **机制依据**：一条命令缓冲里**两个 encoder 之间有序**、**别名对跨 encoder 也无序**（`metal_alias_order.mm` case F/G）。

### 2.3 P3：整跳一次提交（`2926c684ab`，默认**未翻**）

`shared_burst::adopt()` + 第四种车道序 **`merged`**：提取留缓冲 → 权重接第二个 encoder → eq_demap 接第三个
⇒ lane 那一次 commit 覆盖整跳。**空中**：每次接收 **1.0002 次**提交、`cbs/lane` 1.00（max 1）、
`mmse_ce commits=2`（估计器不再自己提交）、**lane gap 0.0 µs**、RF 0、CRC 85.01%、契约 8/8 ✓；
**但**中位延迟 2016.5 → **3666.2 µs**（⚠ 该差被负载混淆：两条腿 9.06 vs 4.97 slot/跳、LDPC 345 vs 190 µs）。
**默认仍是 `event`**（= P2 的两段式）；`OCUDU_CE_LANE_ORDER=merged` 是可用选项，`=event` 是回退。
**本阶段头条离线读不出来**（离线 harness 都物化宿主网格 ⇒ 中途读 ⇒ 必须早提交 ⇒ 跳被劈成两半）。

---

## 3. ★ 下一步要用户定向：跨跳流水**没有余量**（§5.8.11）

用户选了"先不翻 P3，去做跨跳流水"。**但 P3 两条腿的每跳分解说这条路没有余量**：

| 读数 | 值 |
|---|---|
| 估计器**宿主侧全部工作**/跳 | **16.3–17.7 µs**（`submit` ≈ 10 µs）|
| 同一跳里**等 GPU** | **664.6–887.0 µs**（`defer_wait`）|
| 宿主编码整条 lane | 提取 25.2 + 均衡/解映射 68.7 ≈ **~100 µs** |
| 后端 GPU 工作/跳 | **470.3 µs**（`ch_wt` **394.5 = 84%**、`eq_demap` 75.8）|
| 端到端 | min **1355.7/1357.1 µs**（两腿几乎相同）、median 2016.5 / 3666.2 |

⇒ ① 跨跳流水最多藏住 ~100 µs（对 2000–3700 µs 的延迟）；② P3 的代价是**提交粒度**（一次更大的提交在突发上行下
排队更久），不是宿主重叠——两条腿的 **min 几乎相同**；③ 延迟的大头**不在这一跳自己的 GPU 工作里**
（最好的跳也要 2.9× 它的 470 µs）⇒ 是**链条结构**（DFT ~420 µs/槽 → 前端栅栏 → 估计器 → 权重 → 均衡 →
解映射 → LLR 回读）。

**用户裁定：先补仪表（D）** —— 已做（`2e81c7cd49`）：`OCUDU_UL_PHASE_SEGMENTS=1` 让**融合车道里也记录/打印三段分解**
（`ul_time_frequency` / `ul_channel_estimation` / `ul_equalization_demod`），它们的两端就是 `[ul_gpu_pipeline]` 的两端、
三段之和按构造等于它；单测 `ul_pipeline_probe_test` 新增一节（开关必须**追加**总量而不是替换它，且三段必须是**子段**）。

**两条仪器腿已跑完并判读（§5.8.12）**：接收之后那 ~1 ms 的分解是 **DFT ~40% + 估计器权重链 ~40%**，
宿主侧**任何等待都不是瓶颈**（前端栅栏开着跑的读数 `signals=26371 waits=31660`，延迟**没变** ⇒ 假设被证伪）；
权重链里最大的一项是**相关矩阵前缀（K0-d）**：同一捕获上 `ch_wt` 620.7 µs 里它占 **~445 µs（72%）**
（K4 66、TA 125、求逆 153 作对比）⇒ 下一步的靶子是**相关 kernel 的算法结构**（预计算 rt/rf 向量再逐元素相乘，
每个元素少两个除法，**逐位相同**），而**降低每跳 GPU 工作也是让 P3 的 1.00 变得可负担的前提**。

**（历史记录）当时准备的两条腿命令**：

```
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s13p4-phases OCUDU_UL_PHASE_SEGMENTS=1
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s13p4-fence  OCUDU_UL_PHASE_SEGMENTS=1 OCUDU_UL_FRONTEND_FENCE=1
```

**第二条为什么值得跑**：前端栅栏**默认关**（`OCUDU_UL_FRONTEND_FENCE`）⇒ 宿主在 `finish_symbol()` 里
**每槽等一次 DFT 完成**（`dft->wait_slot`）。打开它之后，后端读者改为等**共享事件**（`front_end_wait`），宿主不必等
⇒ 若"DFT 那段在关键路径上"成立，这一条腿的 `ul_time_frequency` 与 `[ul_gpu_pipeline]` 应当**同时下降**，
而契约/穿越/CRC 不变。读数：`[metal_stats] front_end fence signals/waits`（默认腿 0、栅栏腿非 0）。

**三个候选（按数据支持度）**：A 削 `ch_wt`（后端 84%；`OCUDU_INV_RL` / `metal_nn_mmse` 两条现成支路）；
B 削前端 DFT（~420 µs/槽，在链条最前）；C 把后端一部分**搬到空的前端队列**（不是合并队列，是再平衡；
两队列间已有 `MTLEvent` 栅栏）。**三者都还只是估算**——现在**没有逐 dispatch 的 GPU 计时**，
所以第一步应该是**补一个能看见链条分段的仪表**，再动 kernel/队列。

---

## 4. 工具、旋钮与仪表（本会话新增/变更）

| 名字 | 说明 |
|---|---|
| `wip/metal_alias_order.mm` | **机制测量**：同对象/别名对 × 一个 encoder/两个 encoder，各 200 次 |
| `wip/neutral_vs_baseline.sh` | **中性网**：235 个归档 dump（135 语料 + 100 窄捕获），缺文件 = 退出 2 |
| `OCUDU_CE_EDGE_FUSE` | **默认 1**（P1 融合）；`=0` 回退独立形态 |
| `OCUDU_CE_HOLD_EXTRACTION` | **默认 1**（P2 一跳一次估计器提交）；`=0` 回到两次 |
| `OCUDU_CE_LANE_ORDER` | `event`（默认）/ `host_wait` / `burst` / **`merged`（P3，整跳一次提交，已验但未翻默认）** |
| `[metal_stats] wrap_cover / wrap_cover_off` | "一段内存一个对象"的读数（修复前这些请求会各建一个对象）|
| `OCUDU_CE_WRAP_MAP=1` | 每次零拷贝映射一行（含 offset）；`cover` + 非零 offset = 走偏移绑定 |
| `engine_submission_pending()` | 单测钩子：完成之后引擎是否还有未回收的提交（P3 的"完成走哪条路"断言）|

---

## 5. 本会话新增的坑（§7 表）

| # | 一句话 |
|---|---|
| 36 | **★★ Metal 的访存顺序按 `MTLBuffer` 对象**：别名对之间 barrier 无效（症状＝读数随时序漂移、每个探针指认不同嫌疑人）|
| 35 | **`ctest` 不构建**：陈旧测试二进制 + 刚重建的 `.metallib` = host/kernel 错配，会打出假的 NaN 回归 |
| 34 | **把"某个探针的读数"当成"缺陷的位置"**：读数互相矛盾时先怀疑测量的前提 |
| （补）| **一条腿的"结构指纹"和它的"CRC"是两件事**：P2 的 rollback 腿有 17 次 RT failure（两簇）却仍给出正确的旋钮指纹 ⇒ 结构结论可用、数值结论不可用（§5.8.8）|

---

## 6. 腿的历史（本会话 6 条，§12.1 已登记）

| 腿 | commit | 头条 |
|---|---|---|
| `s13p1-rollback_0920_0816` | `21e0b11e39` | P1 对照：`cbs/lane` 3.71、`ch_est` 1.71、RTF 5 |
| `s13p1-fused_0920_0813` | `21e0b11e39` | ✅ P1：**3.00**、RF 0、CRC 98.24%、契约 8/8 |
| `s13p2-rollback_0920_0842` | `454ab9a752` | P2 对照：3.00 / 2.000；⚠ 瞬时扰动腿（RTF 17、CRC 59.5%）|
| `s13p2-onecommit_0920_0840` | `454ab9a752` | ✅ P2：**2.00 / 1.000**、RF 0、CRC 97.76%、`[ul_gpu_pipeline]` 2103.5/1993.1 |
| `s13p3-twostage_0920_0928` | `6a7ec52866` | P3 对照：2.00、lane gap 86.1 µs、RF 0、CRC 82.16%、2803.6/2016.5 |
| `s13p3-merged_0920_0926` | `6a7ec52866` | P3：**1.00（max 1）/ 1.0002 次提交**、gap **0**、RF 0、CRC 85.01%、**4203.5/3666.2** |

---

## 7. 不要相信 / 未解项

1. **P3 的延迟代价没有干净口径**：+1650 µs 被负载混淆（两条腿 regime 不同）；要量化得做**同负载口径的 A/B**
   （一条腿里前后半段切换，或两条腿来回切）。**没有做**。
2. **`defer_wait`（664–887 µs）里去哪儿了**：只有段级读数，没有逐 dispatch 计时 ⇒ §5.8.11 的三个候选都是估算。
3. **进程级缓存 `shared_queue::wrap_no_copy()` 也会发布非零偏移，而引擎的 `wrap_shared()` 丢掉它**
   （静默错址；27 语料与单测不触发，现在会打一行 ERROR）。修法同 `wrap()`，9 个调用点，**单开一次改动**。
4. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**（P0-⑦ 旁证）——仍未查。
5. **§5.6.9 只剩一条真开放项**：1 PRB 的样本量（要新数据）。
6. **`cbs/lane` 的口径**：probe 把"一次 burst 提交"记作一条 lane ⇒ 一律用 `lanes × cbs/lane ÷ 接收次数`。
7. **§18.7 的 TA fence 实验**（把 TA 移出关键路径，~38 µs）仍挂着，未做。

---

## 8. 环境与流程事实

* 构建必须带 `-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  改 `.metal` 先删 `ocudu_mmse.metallib` 再**全量** `cmake --build build -j 10`。
* **判读 `ctest` 前先全量构建**（坑 35）。`ctest -R metal` 是 9 条，红了先单独复跑一条。
* **腿**：`wip/run_leg.sh`（**Ctrl+C 停**），判读 `wip/leg_report.sh`；二进制戳记必须 == HEAD
  （`touch build/hashes.h && cmake --build build --target gnb`）——本会话每次上腿前都已做。
* **sudo 由用户执行**（本会话的腿都是用户跑的，日志在 `wip/logs/`，root 所属但全局可读）。
* tag 纪律：`git tag -a` + `git push origin <tag>`；本会话新增 `gpu_lane_commit_p1`、`gpu_lane_commit_p2`。
