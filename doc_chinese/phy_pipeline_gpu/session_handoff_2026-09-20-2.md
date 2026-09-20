# 交接 — **P0 结案**（融合 NaN = 同一段内存两个 `MTLBuffer` 对象）+ **P1 已落地（离线判据全过，空口腿待跑）**

> 上一份：`session_handoff_2026-09-20-1.md`（S13 收尾 + strict + 控制面融合 P0 起步；**本文件是新的入口**）。
> 常驻设计文档：`gpu_phy_pipeline_design_and_implementation.md`
> —— 本会话改写的章节：**§5.4**（进度表）、**§5.8.2**（P1 行标为已做）、**§5.8.4**（barrier 假定结案）、
> **§5.8.5 新增 P0-(c)**（病因/机制/修法/判据 + P0 两步的收尾）、**§6.3**（新增"与归档基线逐字节比"的那张网）、
> **§7 坑 34/35/36**、**§7 前的旋钮表**（`OCUDU_CE_EDGE_FUSE` 默认翻 1）。
> 本轮工具：`wip/metal_alias_order.mm`（机制测量）、`wip/neutral_vs_baseline.sh`（235 dump 中性网）。

---

## 1. 一句话状态 + 下一步

**HEAD = `31acd1caf5`**（三个提交，**已推送**、工作树干净）。**本会话全部离线**，没有占空口时间。

**本会话完成**：① **P0 结案**——融合路线 NaN 的病因是"**同一段内存上两个 `MTLBuffer` 对象 ⇒ 没有任何顺序**"
（`memoryBarrierWithScope` 排不了别名对，隔离测量 200/200）；② **修掉**（引擎零拷贝缓存改成"一段内存一个对象"，
interior pointer 走偏移绑定）；③ **P1 落地**：`EDGE_FUSE` 转正为默认，离线判据全过。

**P1 的空口腿已经跑完并通过（§5.8.6）**：tag **`gpu_lane_commit_p1`**。受控 A/B（同一二进制 `21e0b11e39`，只差 `OCUDU_CE_EDGE_FUSE`）：

| | `EDGE_FUSE=0` | 默认（融合）|
|---|---|---|
| **`cbs/lane`** | 3.71（max 4）| **3.00（max 3）** |
| busy split `ch_est` | 1.71 | **1.00** |
| `mmse_ce commits`÷lane | 2.706 | **2.000** |
| CRC / RF failure | 98.51% / **5 underflow** | 98.24% / **0** |
| 契约 / 穿越 | 8/8 / 0.00+0.00 | 8/8 / 0.00+0.00 |

**P2 也已经落地并通过离线判据（§5.8.7）**：提取的命令缓冲**留给**权重阶段，权重在同一条缓冲上开
第二个 encoder ⇒ **一跳一次估计器提交**。`OCUDU_CE_HOLD_EXTRACTION=0` 是一行回退/对照臂。

| | `HOLD_EXTRACTION=0` | 默认（合并）|
|---|---|---|
| **`cbs/lane`** | 3.00（max 3）| **2.00（max 2）** |
| `mmse_ce commits`（replay）| 18 | **17** |
| `extraction commit → weights commit` | 有样本 | **为空** |
| 两臂 dump（27 语料）| — | **逐字节相同** |

235 个归档 dump 仍 0 差异、`ctest -R metal` 9/9。**下一步：P2 的空口腿**（头条 `cbs/lane` 3.00 → ≈2.00），
然后 **P3**（把 lane burst 也并进同一条缓冲 ⇒ 1.00；形状仍待 §5.8.5 P0-(b) 的选项 1/2 决策）。

> ⚠ 本会话**没有**改 `.metal`、也没有改任何被广泛包含的头：三个提交都是宿主侧（`.mm` / `.cpp` / 文档）。
> 但按纪律**全量 `cmake --build build` 已跑过**，`ctest -R metal` 9/9。

---

## 2. 本会话做了什么（提交清单，全部已推送）

| 提交 | 内容 |
|---|---|
| `8890d283f6` | **一段内存一个 `MTLBuffer` 对象**：`wrap()` 返回 `mapped{buf, offset}` + 覆盖查找；34 处声明 / ~50 处绑定改走偏移（含两个自带偏移的 helper）；`wrap_cover` / `wrap_cover_off` 计数器；`wrap_shared()` 的丢偏移**记录并加一次 ERROR 提示** |
| `ba4cd9f781` | **S13-P1**：`OCUDU_CE_EDGE_FUSE` 默认翻 **1**（尾组 corr 编进批次自己的命令缓冲），`=0` 一行回退 |
| `31acd1caf5` | 文档：§5.8.5 **P0-(c)**（病因/机制/修法/判据）、§5.4 进度表、§5.8.2 P1 行、§5.8.4 barrier 结案、§6.3 中性网、坑 34/35/36、旋钮表 |
| （工具）| `wip/metal_alias_order.mm`、`wip/neutral_vs_baseline.sh`、`wip/ab_dumps.sh`（过滤 `_h.bin`/`_llr.bin`）|

---

## 3. 病因与修法（**这一节是本次会话的全部价值，别丢**）

**Metal 的访存顺序是"按 `MTLBuffer` 对象"成立的，不是"按地址"。** 同一段宿主内存上建**两个** `MTLBuffer`
对象，两者之间**没有任何顺序**：`memoryBarrierWithScope:MTLBarrierScopeBuffers` 排不了它们——不管 barrier 放在哪、
dispatch 怎么编、三种车道序都一样。

* **机制测量（隔离）**：`wip/metal_alias_order.mm`，一个 writer + 一个 barrier + 一个 reader，四种组合各 200 次：
  **同对象 PASS/200；别名对（页对齐切片写/整体读、非页对齐 interior 两个方向）FAIL/200**。
  跑法：`clang++ -std=c++17 -fobjc-arc -O2 -framework Metal -framework Foundation wip/metal_alias_order.mm -o /tmp/x && /tmp/x`。
* **它怎么变成 NaN 的**：合并批次的**边组**槽位从 `gpu_a`/`gpu_r_hp` 里 `sys_offset` 个 stride 处开始
  （`correlation_stage()`），于是 corr 前缀拿到**第二个对象**（缓存按指针建映射），而 K1/权重读的是**第一个**
  （批次基址那个）。K1 读到**还没被写**的槽 → 算出 NaN → 权重读到 NaN → W/h 全 NaN；
  **宿主在完成时刻看到的却是写好的槽**（写最终落地，只是晚于读）⇒ "槽位检查说 A 干净、W 全 NaN"看着自相矛盾。
* **修法**：`mmse_engine_impl::wrap()` 对任何"**已被现有映射覆盖**"的请求返回**该映射 + 偏移**
  （`setBuffer:m.buf offset:m.offset`）。构造期 warm-up（按容量建映射）保证矩阵链的 interior pointer
  全被每个分配的第一个映射接住。**这与进程级缓存 `shared_queue::wrap_no_copy()` 早就在做的 containment lookup
  是同一条规则**——引擎私有缓存是唯一没做的那个。
* **每个 P0 线索的去向**：pad（症状）、"权重读到的 A 未写"（直接指纹）、"求逆→权重补 barrier 无效"
  （缺的顺序在更上游）、`lane_order` 线索（**作废**：dispatch 顺序没问题）、"切 pipeline 会顺带插 barrier"
  （**结案**：barrier 够用，只要对象是同一个）。
* **交接 §5.A 的"给读探针一块专用 scratch"不再需要**：它要回答的问题现在有更强答案——**融合路线与独立路线
  逐字节相同**（27 语料 + 20 条真实窄捕获），加上那 30 行机制测量。

---

## 4. 判据（全过，可复跑）

| 判据 | 命令 | 结果 |
|---|---|---|
| **中性**（既有路线不动）| `bash doc_chinese/phy_pipeline_gpu/wip/neutral_vs_baseline.sh` | **235 个 dump（27 语料 × 5 + 20 窄捕获 × 5）与归档基线 0 差异** |
| **融合 == 独立** | `bash wip/ab_dumps.sh "OCUDU_CE_EDGE_FUSE=1" "OCUDU_CE_TAIL_DEV=1" --metal`（及窄捕获 glob）| 27 + 20 条，四个 dump **逐字节相同** |
| **默认 == 回退**（P1 的 A/B）| `bash wip/ab_dumps.sh "OCUDU_CE_EDGE_FUSE=0" ""` | 27 语料 **0 差异** |
| 提交数 | 同一捕获两臂 | 合并跳 `mmse_ce commits` **19 → 18**、`[ul_gpu_lane] cbs/lane` **4.00 → 3.00**、busy split `ch_est` **2.00 → 1.00**；**不走合并路线的窄跳两臂都是 3.00/18**（并集没变）|
| 单测 | 先全量 `cmake --build build -j 10`，再 `ctest -R metal` | **9/9**；估计器单测连跑 **0/20** 红 |

---

## 5. 本会话新增的坑（§7 表）

| # | 一句话 |
|---|---|
| 36 | **★★ Metal 的顺序按 `MTLBuffer` 对象，不按地址**：别名对之间 barrier 无效（症状＝"读数随时序漂移"、每个探针指认不同嫌疑人）|
| 35 | **`ctest` 不构建**：它跑上一次链接出来的二进制 ⇒ 陈旧测试二进制 + 刚重建的 `.metallib` = **host/kernel 版本不匹配**，会打出 `Test 12 ... nan` / `NMSE merged nan`（看着像刚做的改动弄坏了数值；重新链接后 0/15、0/20 全过）|
| 34 | **把"某个探针的读数"当成"缺陷的位置"**：多个读数互相矛盾时，先怀疑**测量所依赖的前提**（"同一段内存＝同一个资源"当时不成立）|

---

## 6. 工具、旋钮与仪表（本会话新增/变更）

| 名字 | 说明 |
|---|---|
| `wip/metal_alias_order.mm` | **机制测量**：别名对 vs 同对象的四种组合，各 200 次 |
| `wip/neutral_vs_baseline.sh` | **中性网**：当前构建 vs 归档基线（135 + 100 dump），缺文件 = 失败（退出 2）|
| `OCUDU_CE_WRAP_MAP=1` | 每次零拷贝映射一行：`ptr/bytes -> contents/length/offset`；`cover` + 非零 offset = 走偏移绑定 |
| `[metal_stats] ... wrap_cover= / wrap_cover_off=` | 被现有映射接住的请求数（融合 syn004_4 跳：**17 / 2** = 边组的 A 与 R_hp）。**P3 之后复查的是这个数，不是"barrier 够不够"** |
| `OCUDU_CE_EDGE_FUSE` | **默认 1**（融合）；`=0` 一行回退到独立形态 |
| `wip/ab_dumps.sh` | 现在过滤 `_h.bin` / `_llr.bin`（语料目录里混着上一腿的结果 dump，会被 `*.bin` 当成捕获）|

---

## 7. 不要相信 / 未解项（**交给下一会话**）

1. **`OCUDU_CE_EDGE_FUSE=1` 的空口行为还没验过**：离线逐字节相同、提交数少一次，但**空中**的
   `cbs/lane`、延迟（`[ul_gpu_pipeline]` mean/median，基线 3493.5 / 2799.6 µs）与 CRC 都要这一条腿给出。
2. **`shared_queue::wrap_no_copy()` 也会发布非零偏移，而引擎的 `wrap_shared()` 丢掉它** —— 那是一次
   "绑到映射基址"的静默错址。**27 语料与单测不触发**（现在会打一行 ERROR）。要修就单开一次改动，
   照 `wrap()` 的做法把偏移传到绑定处（9 个调用点）。
3. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**，而 `run()` 只有 1 次——**仍未查**（P0-⑦ 的旁证）。
4. **`cbs/lane` 的口径**：probe 把"一次 burst 提交"记作一条 lane ⇒ 一律用**"每次接收的提交数"**
   （= `lanes × cbs/lane ÷ 接收次数`）。
5. **P3 的形状仍要用户决策**（§5.8.5 P0-(b)）：两个 metal 队列（front_end=DFT / back_end=其余）是结构事实；
   要 `cbs/lane` 1.00 就得合并它们（代价＝跨 slot 并行），**选项 1/2 的共同前提是一条空口腿**（`burst` 序 vs 默认）。
6. **§5.6.9 只剩一条真开放项**：1 PRB 的样本量（要新数据）。

---

## 8. 工作树产物（`doc_chinese/work_tmp/`，**不进 git**）

| 路径 | 是什么 | 可再生？ |
|---|---|---|
| `narrow_cap/` | **20 条真实 1–2 PRB 空口捕获**（S13-P2c 的判定语料）| **不可再生** |
| `determinism/a/`、`narrow_cmp2/*/dev_*` | **中性网的参照**（27 语料 + 20 窄捕获的基线 dump）——**别再覆盖** | 可再生 |
| `narrow_cmp2/`（其余臂）、`corpus_sparse/`、`corpus_strict/`、`p0*/` | 各批的中间产物 | 可再生，可清理 |

---

## 9. 环境与流程事实

* **本机构建必须带**：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  改 `.metal` 先 `rm -f lib/phy/upper/signal_processors/channel_estimator/metal/*.metallib`，再**全量** `cmake --build build -j 10`。
* **判读 `ctest` 之前先全量构建**（坑 35）：`ctest` 不构建，陈旧二进制会与刚重建的 metallib 组错配。
* **空口腿**：`wip/run_leg.sh`（**必须 Ctrl+C 停**，SIGTERM 丢统计）；判读 `wip/leg_report.sh`；
  停机规则见设计文档 §10.1（阶段完成即停、等手机 ping/iperf3）。
* **单测口径**：`ctest -R metal` 是 9 条；红了先单独复跑那一条（`_ta_chain` 有 ~10% 的既有偶发，坑 31）。
* **tag 纪律**：`git tag -a` + `git push origin <tag>`；已发布的 tag 不改写。**本会话没有新建 tag**
  （P1 的 tag 要等空口腿通过）。
