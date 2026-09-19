# 交接 — S13 收尾（G3a 决策 + CPU 参考链勘误）+ strict 策略 + **控制面融合 P0（未完成）**

> 上一份：`session_handoff_2026-09-19-8.md`（5g 收尾 + S13 起步；**本文件是新的入口**）。
> 常驻设计文档：`gpu_phy_pipeline_design_and_implementation.md`
> —— 本会话新增/改写的章节：**§1.8**（用户裁定：`mode=gpu` 不允许宿主兜底）、**§1.9**（用户裁定：dump 不算 CPU in the loop）、
> **§3.2**（契约 8 条逐项表）、**§5.4**（控制面现状 + 当前任务）、**§5.7**（G3a 决策）、
> **§5.8**（★ 控制面融合的分阶段计划 + **§5.8.5 的 P0 结果 P0-①…P0-⑦，务必先读**）。
> 本轮方案文件：`wip/S13_fallback_coverage.md`（S13 方案 + §7 结论：G3a 不做）。

---

## 1. 一句话状态 + 下一步

**HEAD = `f1f2643cf3`**（工作树干净、已推送、远端一致）。**本会话全部离线**，最后一次空口腿是本会话开头分析的
`narrow-fix_0920_0508`；此后没有再占空口时间。

**本会话完成**：① 契约 8/8 的逐项解释与勘误（一个"CPU 参考臂"其实没跑 `--cpu`）；② G3a（稀疏 RB 掩码）**用两侧证据决策为"不做"**；
③ **strict 策略落地**（`mode=gpu` 下设备覆盖不到的跳**失败 + ERROR**，不再由宿主兜底）；④ **dump 不再是 CPU in the loop**
（判据排除 + release 构建不含）；⑤ 控制面融合的**分阶段计划**（每阶段一次 OTA）与 **P0 调查**（进行中，见 §5）。

**下一步只有一件（P0 的两步，都在离线）**：

1. **给"读探针"一块专用 scratch** ⇒ 让"权重那一跳究竟读到了什么"变成**可信读数**（详 §5.A）；
2. **顺"一个命令缓冲内部的顺序"查下去**：缺陷已确认落在**那唯一一个** `run()` 命令缓冲内部（详 §5.B）。

> ⚠ 在 1 之前**不要**再去改 kernel 或加 barrier：P0-③ 已经用一次实验证明"我以为是修法的东西"会把结论带偏，
> 而 P0-⑤ 证明"求逆→权重之间补 barrier"**无效**（全量重建后复测）。

---

## 2. 本会话做了什么（提交清单，全部已推送）

| 提交 | 内容 |
|---|---|
| `e62f22cd58` | 文档：`8 of 8` 到底是什么（8 条逐项 + 证据）；**§3.2** 新增；§2 明确标为历史基线；坑 28 |
| `bf20b47eb9` | **窄跳的"CPU 参考臂"从来没跑过 `--cpu`**（错标成 `host` 臂的重复）；`ul_chain_replay` 改为**必须显式给模式**；`wip/narrow_arms.sh`（身份指纹 + 串行） |
| `ced2fe7f21` | 精确 A/B 是 `CORR_DEV=0` **加上** `HOST_SCALARS=1`（20/20 三个数据 dump 逐字节相同） |
| `355f070a01` | **G3a 决策：不做**（空口 103,056 次授权 0 次非连续；阳性对照 `corpus_sparse/`）；工具不再静默压平分配；坑 30 |
| `824ab88c8b` | 文档：用户裁定——`mode=gpu` 下回退是缺陷；排序标准改为"CPU 还剩几个参与点" |
| `6b69446335` | 文档：strict 失败的响度＝**该 PUSCH 失败 + 一行 ERROR**（复用 `on_sch({})` = CRC KO 路径） |
| `9e603e18d5` | **strict 策略实现**：两层兜底（估计器 + 解调器）一起堵；旋钮豁免；单测钉住分类不变式 |
| `7be33da9b1` | 文档：Metal 单测 ~10% 偶发的**同口径基线对照**（1/10 vs 1/12）+ 坑 31 |
| `e614876916` | 文档：**控制面融合的分阶段计划**（P0-P4、每阶段一次 OTA、tag、独立回滚） |
| `55caad98d0` | 文档：**P0 结果**（EDGE_FUSE 融合坏成 NaN；地板是队列数）|
| `193a4cfb35` | **dump 不算 CPU in the loop**：判据按"总数 − debug"判 + 单独打印；`ENABLE_UL_CAPTURE`（默认 OFF）；坑 32 |
| `43e1231ca9` | **能看见融合路径的自检**（`pending_corr_check`，完成时刻统一比较）|
| `3b4401692c` | 文档：P0-② 病灶收窄到"求逆读到的边组 pad 是空的"；坑 33 |
| `800f37e648` | 文档：pad-ownership 尝试**已回退**（**其中"45/135 被弄坏"那句后来被更正为陈旧构建假象**）|
| `2c89720f15` | **pad 缺陷已修**（`mmse_corr_a` 覆盖整槽并自写 blockdiag 恒等）⇒ A 槽位干净、**既有路线 0/135 0/100**；链式探针 |
| `feb823496a` | 链式探针的**判决性对照**：宿主用完成时刻槽位能算出有限 W，而设备 W 是 NaN ⇒ 权重读的不是这份数据 |
| `80430d94c6` | **在 kernel 里的读探针**：权重看到的 A **整槽 2916/2916 NaN**（探针因有竞争已撤，见 §6）；权重段 `lane_order` 与估计器不同 |
| `f1f2643cf3` | 文档：**每跳只有一次 `run()`**，两臂只差 `corr_edge` ⇒ 缺陷在**一个命令缓冲内部** |

---

## 3. 本会话的三条用户裁定（**长期有效，别丢**）

1. **`mode=gpu` 不允许宿主兜底**（§1.8）：多模块 `cpu_gpu` 的"CPU 托底"在这里是**缺陷**；
   设备覆盖不到 ⇒ **该 PUSCH 失败 + 一行 ERROR**（不是 abort：会带走整台 gNB，且本线"abort 属于启动期检查"）。
   **判据排序标准**：不是"收益"，是**功能正确前提下 CPU 还剩几个参与点**（数据面已 0；控制面 3.73 次提交/跳）。
2. **dump 不算 CPU in the loop**（§1.9）：debug 捕获造成的宿主接触**不进判据**，但要**单独打印**；
   release 构建（`ENABLE_UL_CAPTURE` 默认 OFF）**不含捕获机制**。
3. **控制面融合要分阶段做、每阶段停下来做一次 OTA**（§5.8）：免得最后一起错、定位不了。

---

## 4. 当前技术状态

| 项 | 状态 |
|---|---|
| 数据面（IQ→LLR 的读/写）| ✅ 空中 **0.00 读 + 0.00 写/跳**、分项表空、契约 **8/8**（腿 `narrow-fix_0920_0508`）|
| 窄跳（S13-P2c）| ✅ 2 PRB `0.0% → 77.5%`、SINR 中位数 `−10.7 → +13.0 dB`；机制=对角加载 |
| CPU 参考链（窄跳）| ✅ **勘误**：真 `--cpu` 是 **OK × 18/20**、与 `dev` 逐条同分布（旧的"全 KO"是错标臂）|
| `CORR_DEV=0` 精确 A/B | ✅ `CORR_DEV=0 + HOST_SCALARS=1` ≡ `dev`（20/20 三个数据 dump）|
| G3a 稀疏掩码 | ✅ **决策：设备侧不实现**（空口 0/103,056）；回退应报错（strict 已实现）；回归件在 `corpus_sparse/` |
| **strict 回退策略** | ✅ 已实现：稀疏/多端口语料 ⇒ 失败 + ERROR；六个旋钮臂 0 条 strict 行；**27 语料 0/135、20 窄捕获 0/100 不变** |
| **控制面融合（cbs/lane 3.73 → 1.00）** | ⏳ **P0 调查中**：见 §5；**P1/P2/P3 未开始**，且 **P1 被 `EDGE_FUSE` 的缺陷挡着** |
| 控制面的形状决定（P3）| ⏳ 待定：**两个 metal 队列**（front_end=DFT / back_end=其余）⇒ 现状地板是 **2 次提交/接收**（`burst` 序，零等待）；要 1.00 必须合并队列 ⇒ 需先量跨 slot 并行损失（**一条空口腿**）|

---

## 5. 下一步（P0 剩余两步，全部离线）

### A. 给读探针一块专用 scratch（优先）

**做什么**：给权重那一跳一块**专用**（与累加器不共享）的记录位置。两种做法：

* **① 引擎自持**（最少改动）：`mmse_engine_impl` 里 `[device newBufferWithLength:64*4 options:MTLResourceStorageModeShared]`
  分配一块，权重 dispatch 作为 `buffer 4` 绑定，完成后宿主从 `probe_buf.contents` 读。
  ⚠ **注意**：引擎目前**没有任何自有分配**（`ocudu_metal_mmse_engine.mm:367` 只有 `newBufferWithBytesNoCopy` 的零拷贝 wrap，
  所有缓冲都是估计器宿主内存的映射）⇒ 这会是它的**第一块自有分配**，写清注释即可；
* **② 估计器自持**（与现有模式一致）：在 `gpu_qy` 旁边 `alloc_aligned<float>` 一块，指针经 `engine_run()` 传进 `run()`
  ⇒ 要动引擎公开接口的签名（`run`/`run_weights_only` 及其 async 孪生），改动面更大。

建议先试 ①（不动接口）。记录内容除原来的六项外，再加 **A 与 R_hp 的前 8 个原始值**，以便区分"零 / NaN / 上一跳残留"。

**为什么**：P0-⑥ 的探针把记录写进 `W` 的头几个元素，**与 kernel 自己的累加器共用地址（有竞争）** ⇒ 样本值不可信
（只有 NaN 计数可信：那个 2916 确实是探针写的）。**可信读数能区分三种病因**：

* A 是**零** ⇒ 生产者根本没跑（写者缺失）；
* A 是 **NaN** ⇒ 求逆先跑了（输入退化）；
* A 是**上一跳的残留** ⇒ 缓冲复用/顺序问题。

**判据（缺一不可）**：

1. 探针**默认关**：27 语料 `determinism/a/` **0/135**、20 窄捕获 `narrow_cmp2/*/dev_*` **0/100**（逐字节）；
2. 融合臂开探针能打出**可信记录**；`TAIL_DEV`/默认臂的对照值同时给出；
3. **改过 `.metal` 或任何被多个库包含的头之后，必须 `rm -f lib/.../metal/*.metallib` + `cmake --build build -j 10` 全量重建**（坑 32 已坑两次）。

### B. 查"一个命令缓冲内部的顺序"

已确认：融合路线每跳**只有一次 `run()`**（`corr=1 corr_edge=1 nsys=2 nblk=1 L=54 nout=504 defer=1`），
一个命令缓冲里按编码顺序是 `[corr 标准][B][corr 尾组][B][求逆][权重][apply][reformat]`，一次提交；
而权重**读到的 A 整槽是 NaN**⇒ 说明 **dispatch 没有按编码顺序可见**。已排除：绑定（`delta=0`）、
"求逆→权重补 barrier"（全量重建后无效）、车道序（三种都 NaN）、`wparams` 几何。

**要读的东西**：`stage_pipeline()` 在 `!st.burst` 时如何保持/切换 encoder（`ocudu_metal_mmse_engine.mm:550`），
以及 `if (!st.burst) { [enc memoryBarrierWithScope:MTLBarrierScopeBuffers]; }` 这族调用点的**语义是否真的够**
（§5.4 从第一天就记着这句嫌疑：**"切 pipeline 会顺带插 barrier"**——**按规矩：读代码 + 实测，别信注释**）。

---

## 6. 工具、旋钮与仪表（本会话新增/变更）

| 名字 | 说明 |
|---|---|
| `wip/narrow_arms.sh` | 窄跳五臂回放（cpu/dev/host/hostsc/exactab），**串行** + 逐臂**身份指纹**核对 + 精确 A/B 自断言 |
| `ul_chain_replay` **必须显式给模式** | `--cpu/--metal/--metal-cpu-ldpc/--metal-cpu-demod`，不给就报错（默认曾是 CPU 链，酿成过"假 CPU 臂"）|
| `OCUDU_GPU_STRICT=1` | 强制 strict（离线 harness 不发布 mode；不可测的策略等于没有策略）|
| `ENABLE_UL_CAPTURE`（CMake，**默认 OFF**）| 关掉后 `ul_capture` 只剩 no-op API；**本机开发构建必须带 `-DENABLE_UL_CAPTURE=ON`**（否则 `ab_dumps.sh` 会以 `missing-dumps` 报错退出——这是设计好的）|
| `OCUDU_CE_EDGE_CHECK=1` | 边组槽位自检（**已在完成时刻统一**：融合与独立都覆盖，含 PAD 区）|
| `OCUDU_CE_CHAIN_MAP=1` | **链式探针**：按系统扫 A/W/h/y 的 NaN 与非零 + **宿主重算 W 的对照** |
| `OCUDU_CE_PAD_SENTINEL=1` | 把 A 的 pad 写成 12345（临时诊断，保留）|
| ~~`OCUDU_CE_W_PROBE`~~ | **已撤**（记录与累加器竞争，会误导；§5.A 重做）|
| ~~`OCUDU_CE_A_MAP` / `OCUDU_CE_ROUTE` / `OCUDU_CE_W_BIND`~~ | 临时打印，已撤 |

---

## 7. 本会话新增的坑（§7 表）

| # | 一句话 |
|---|---|
| 28 | 把 `contract MET (8 of 8)` 当成**一项**结论（它是八条，三条是"零分子即通过"，分母还是动态的）|
| 29 | 把**臂的名字**当成**臂的身份**（`cpu` 臂从没跑 `--cpu`）——每个臂都要留并核对身份指纹 |
| 30 | 把一个**表达不了**的输入静默改写成另一个（PRB 列表被压平成连续分配 ⇒ 空结果像干净通过）|
| 31 | 把一次红的 Metal 单测当回归（`_ta_chain` ~10% SIGBUS，**基线同口径 1/10 vs 带改动 1/12**）|
| 32 | **★ 改过被广泛包含的头/`.metal` 之后只用 `--target` 增量构建**（本会话坑了两次；成品的"差异"是陈旧对象）|
| 33 | 拿"另一个可疑臂"当参照（两臂一致只说明同源；判据的参照必须是**已知正确**的一方）|

---

## 8. 工作树产物（`doc_chinese/work_tmp/`，**不进 git**）

| 路径 | 是什么 | 可再生？ |
|---|---|---|
| `narrow_cap/` | **20 条真实 1–2 PRB 空口捕获**（S13-P2c 的判定语料）| **不可再生** |
| `narrow_cmp2/` | 五臂回放结果 + `summary.tsv`（**现在引用的表**）；旧 `narrow_cmp/` 是**臂名错标**的记录 | 可再生 |
| `corpus_sparse/`、`corpus_strict/` | 稀疏分配 / 多端口语料（**路径触发器，不是数值语料**）| 可再生（生成器在树里）|
| `determinism/a/`、`determinism_hostA/` | 快速网的**基线 dump**（27 语料 + 窄捕获）——**判据的参照** | 可再生（但**别再覆盖**）|
| `p0b…p0g/`、`strict_*/`、`corrcheck/`、`toolcheck/`、`chain/` 等 | P0 与各批的中间产物 | 可再生，可清理 |

---

## 9. 环境与流程事实

* **本机构建必须带**：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  **改 `.metal` 先 `rm -f lib/phy/upper/signal_processors/channel_estimator/metal/*.metallib`**，然后**全量** `cmake --build build -j 10`。
* **空口腿**：`wip/run_leg.sh`（**必须 Ctrl+C 停**，SIGTERM 丢统计）；判读 `wip/leg_report.sh`；
  停机规则见 §10.1（阶段完成即停、等手机 ping/iperf3）。
* **Ubuntu**（`jwang@192.168.100.131:~/work/ocudu`）：无 Metal，用于跑全量 `ctest`（上次 7617/7617）；本会话**没跑**。
* **单测口径**：`ctest -R metal` 是 9 条（每次都执行那个 flaky 二进制约 3 次 ⇒ 整套 ~30% 会现一次红）；
  **红了先单独复跑那一条**，复跑也红才是回归。
* **tag 纪律**：`git tag -a` + `git push origin <tag>`；已发布的 tag 不改写。本会话**没有**新建 tag。

---

## 10. 不要相信 / 未解项

1. **`EDGE_FUSE=1` 今天不可用**（P1 被挡）：融合路线边组 NaN。**已修一半**（A 的 pad 缺陷）；**另一半**在"一个命令缓冲内部的顺序"。
2. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**，而 `run()` 只有 1 次——**没查**（P0-⑦ 记为未解旁证，别当结论）。
3. **`cbs/lane` 的口径**：probe 把"一次 burst 提交"记作一条 lane ⇒ **只看 `cbs/lane` 会被 `burst` 序骗**；
   一律用**"每次接收的提交数"**（= `lanes × cbs/lane ÷ 接收次数`）。
4. **§5.6.9 只剩一条真开放项**：1 PRB 的样本量（要新数据）。
5. **两个 metal 队列**（front_end=DFT / back_end=其余）是现状的**结构事实**：P3 要 1.00 就得合并它们（代价是跨 slot 并行），
   这需要**一条空口腿**先把损失量出来（`[ul_gpu_pipeline]` mean/median 对比，基线 mean 3493.5 / median 2799.6 µs）。
