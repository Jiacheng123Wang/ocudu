# 交接（入口） — S15：**目标两半已达成（保留 `merged`）；门已换成"值在容差内"；下一件事 = K1 路 A**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **上一份 `session_handoff_2026-09-20-5.md` 不要读**（它描述的是本会话早期的状态：那时门还是"逐字节不变"、
> `merged` 还没翻默认——两件都已经被推翻/推进）。**技术细节全在常驻设计文档
> `gpu_phy_pipeline_design_and_implementation.md` 的 §5.8.15 – §5.8.27**（按顺序读即可，每节都短）。

---

## 1. 一句话状态

**工作树 HEAD = `edd116513b`**；**`build/apps/gnb/gnb` 已按它重戳**——
自查：`grep build_info build/hashes.h` 的短哈希 == HEAD。**之后再有任何提交，上腿前按 §9 重戳**（坑 35）。
工作树只剩**用户自己的两个 config**（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`gnb_rf_b200_tdd_n78_20mhz.yml`），**别动**。

**门（本会话换过，见 §2）**：
```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py            # 47 捕获 0 问题
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 8/8
ctest --test-dir build -R metal                                   # 9/9
```
**逐字节网 `neutral_vs_baseline.sh` 现在是"信息"，不是门**（当前树 131 字节不同，是**预期的**：见 §3）。

**tag**：`gpu_lane_commit_p1`、`gpu_lane_commit_p2`（本会话没有新 tag——`merged` 是**改默认**，不是新阶段）。

---

## 2. ★★ 本会话改变了三件判断（**先读这段，否则会用错门/走错路**）

### （一）门：从"逐字节不变"改成"值在容差内 + 判决级判据"（用户裁定，设计文档 §5.8.19）

用户原话：「逐字节不变」太严格、没必要——只要**算法逻辑正确、误差在范围内**即可；而在 GPU 上为了并行度
**改变计算顺序是常态**。

* **门 = `wip/value_net.py`**：按字段容差（`_ce.txt` 六个字段各自相对/绝对容差；`_h.bin` med ≤1e-2 /
  p99 ≤2e-2 / max ≤5e-2；`_llr.bin` **符号一致率 ≥99.9%**）+ **NaN/Inf 硬判**（P0 的签名）+
  **数量级哨兵**（变化 ≥10× 即红，P5 的签名）。
* **`--self-test` 8/8**：注入 P0/P5/20% 漂移/高置信翻转必须红；合法重排形态（1e-6 网格漂移、±1 LLR 步、
  1e-5 标量、0.5% 网格漂移）必须过。**这条必须先跑**（坑 19：看不见失败的门 = 没测）。
* **逐字节网降级为信息**：仍然跑、仍然报"移了多少字节"，但**不阻断**。别删它。

### （二）`merged`（P3）**已经是默认**，用户裁定**保留不回退**（§5.8.26/§5.8.27）

* 默认 = **整跳一次提交**：`cbs/lane` **1.00（max 1）**、`mmse_ce commits`/跳 ≈**0**、lane `gap` **0.0**。
* 代价（**同负载口径，首次量干净**）：`[ul_gpu_pipeline]` 中位 **1907.5 → 2378.3 µs（+470.8）**，
  其中 **+131 µs 是后端 GPU 跨度**（一条缓冲里估计器的尾巴与均衡的起步不能再重叠），其余 ~340 µs 是排队。
  `min` 几乎不变（+7.5）。**回退是一行：`OCUDU_CE_LANE_ORDER=event`。**
* **翻默认时门抓到一个真缺陷并已修**：`end_stage_async()` 原来只在 `event` 序拉 back-end fence，
  而 `merged` 有一条回退路径（提取没能 hold 住缓冲时）会走到它 ⇒ lane 会等一个**永远不会被 signal 的
  generation** ⇒ 均衡器可能**在估计器写 h 之前读 h**。现条件已含 `merged`。详见 §5.8.26 ②。

### （三）★★ 目标的两半**都已达成**

| | 状态 |
|---|---|
| **数据面** | ✅ `0.00 read + 0.00 write / 跳`，契约 **8/8**（两条腿都是） |
| **控制面** | ✅ **每次接收 1 次提交**（`cbs/lane` 1.00）；估计器自己**不再提交**（15104 跳里 2 次） |

**⇒ 剩下的是"把已达成的目标变便宜"（K1，本会话的成本优化）与"补最后一个目标性差距"（D1：DFT 还不在车道里）。**
**用户已裁定顺序：先 K1 路 A，再 DFT/D1。**

---

## 3. 本会话做成了什么（都已经在空中验证或已记录）

| 事项 | 结果 |
|---|---|
| **导频抽取并行化**（CFO 单线程→线程组归约；`sigma2`/`epre` 的 256 项树→**SIMD 组内归约**）| **171.3 → ~118 µs（−31%）**；空口腿 `s14-extract-parallel_0920_1809`：`ch_wt` **340.7**（历史最好）、契约 8/8、穿越 0、RF 0；**全 46 捕获 `_llr.bin` 差 0 字节** |
| **K1 的 barrier 斜率（空口腿对）** | **0.22–0.31 µs/道**（`eq_demap` 做负载对照校正后 0.216）⇒ K1 的 122 道 = **27–38 µs（26–37%）** |
| **K1 的结构事实** | 每跳只反 **1 个（每层）54×54 矩阵**（`nof_systems = nof_layers`；8 个频块共用同一个 A），**一个线程组** ⇒ 其余 14–18 核空着；`nof_systems` 1→8 **成本不变**（并行免费） |
| **双峰根因** | **宿主 GUI（Chrome GPU 进程 + WindowServer）分时占 GPU**；空臂 base-vs-base 宽度可达 **±250 µs** |
| **CRC 判据更正** | **必须按【调制】分层**：同一二进制，好腿 96.6% vs 另一腿 81.1%，差别几乎全在"92–95% 是 QPSK"vs"94% 是 256QAM"（§5.8.22） |

---

## 4. ★ 开工第一步：**K1 路 A**（用户已定顺序）

### 4.0 什么是"路 A"（§5.8.25 ④）

> **把一只候选 kernel 做成【默认关闭】的旋钮；离线【只验值】；速度【只由空口腿对判】。**

**为什么必须这样**：离线**绝对 µs 已不可信**——同一个 kernel、同一个 `k1_check`，跨区块读出
**47.4 / 108 / 120 / 190 / 271 µs**（§5.8.25 ③），根因是宿主 GUI；而**空口 `ch_wt` 跨腿只有 ±8%**
（383.1 / 379.0 / 384.9 / 394.5 / 329.7 / 340.7）⇒ **一条腿对能判 ≥40 µs 的效应**。
用户明确表示 Chrome/WindowServer **不能关**。

### 4.1 ★ "缝"已经在树里了（**不要另起架构**）

| 已有的 | 位置 |
|---|---|
| 两只求逆 kernel | `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.metal`：`mmse_inv`（默认）与 `mmse_inv_rl`（K1b） |
| 两个 pipeline 槽 | `ocudu_metal_mmse_engine.mm`：`inv_pipe` / `inv_rl_pipe`（启动时从 metallib 加载，约 1624/1634 行） |
| 一个选择旋钮 | 同文件 ~3029：`use_rl = (inv_rl_pipe != nil) && getenv("OCUDU_INV_RL")`，用于**跳内 K1**（~3112 行 `stage_pipeline`） |

**⚠ 两个已知问题**：
1. **`mmse_inv_rl` 数值是错的**（order 54 下逆矩阵大约 **1e8 倍**，见文件头注释）⇒ **不能当候选**，
   只能当"这条缝被用过一次、那次候选死在数值上"的记录；
2. **`OCUDU_INV_RL` 只作用于跳内 K1**：独立入口 `mmse_engine::invert()`（~2352 行）硬编码 `e->inv_pipe`
   ⇒ **`k1_check` 忽略这个旋钮**。要一行修（让 `invert()` 也认旋钮），**建议顺手做**，因为 `k1_check`
   是判 K1 的现成台架。

### 4.2 四步（前两步不碰数据面，可以立刻做）

| 步 | 做什么 | 判据 |
|---|---|---|
| **1** | `wip/value_net.py` 加 **`--env OCUDU_*=…` 透传**（现在它只跑默认旋钮，而候选在旋钮后面，离线门必须**带着旋钮**跑才能验候选的值）| `--env OCUDU_CE_INV_X=1` 时 47 捕获 0 问题 |
| **2** | **写候选 kernel**，加进那条缝（新 kernel 名 + 新 `*_pipe` + 新旋钮，**默认关闭**）；同时让 `invert()` 认旋钮 | 出厂路径零影响：不带旋钮时 `value_net` 仍 47/0、`ctest -R metal` 9/9 |
| **3** | **离线验值**：`python3 wip/value_net.py --env OCUDU_CE_INV_X=1` | **47 捕获 0 问题**；外加 `ctest -R metal` 9/9 |
| **4** | **空口腿对**：对照（不设）+ 候选（设旋钮），**背靠背、同一手机同一业务、中间不改状态**；再写一个像 `wip/k1_barrier_slope.sh` 的判读脚本（读 `ch_wt`，**`eq_demap` 当负载对照通道**，四条标准照跑）| **`ch_wt` 的下降 ≥40 µs** 才算赢（空口噪声 ±8% ≈ 27 µs）|

### 4.3 候选队列（按机制证据强度；**我无法离线排序它们，这正是路 A 存在的理由**）

| 候选 | 机制 | 预期 | 风险 |
|---|---|---|---|
| **(c) 访存向量化（float4）** ← **建议第一个做** | 主循环每个元素 8 次**标量** threadgroup 读；按 c 方向向量化可把访存指令数降到 1/4，而**每个元素的乘加次序不变 ⇒ 很可能逐位保值** | 未知 | **小**（改法局部） |
| **(a) 多 dispatch 的分块求逆** | 现在**一个线程组 = 一个 SM** 做完全部逐元素工作（63–74% = 66–77 µs），其余核空着。改成一个 block column 一次 dispatch、矩阵进**设备内存**、grid 铺满 trailing 子矩阵 ⇒ dispatch 边界天然提供跨线程组同步 | 若逐元素是**吞吐**受限：104 → ~40–50 µs；若是**延迟**受限，收益小 | **大**（新 kernel + 设备内存 + 调度循环），但机制最确定 |
| **(b) Cholesky + 三角求逆** | A 是 SPD；Cholesky 每步**1 道 barrier**（GJ 要 2）⇒ 122 → ~54 道（省 ~20 µs），算术量约 1/3 | 104 → ~70–80 µs | 中；上限被 barrier 卡住 |
| **(d) SIMD 组替代 pivot 阶段的 barrier** | pivot 消元是 8 行 × 若干列，可用 `simd_shuffle` 在寄存器里通信 | 最多 ~27 µs | 中；列数超 32 lanes，跨组仍需 barrier |

**已否掉的（别再提）**：**Newton–Schulz**（30× 算术 + 需 3 个 54×54 矩阵同时在场，3×11.7 KB=35 KB **超过
32 KB 线程组上限**）；**几何微调**（默认 (64,16) 已最优，最好的替代只值 1.1%）；**"少 barrier、同工作量"的重写**
（2→1 barrier 的逐位保值改法量到**更慢**）。

### 4.4 K1 的已知账（做候选之前先认清靶子）

* K1 ≈ **104 µs/跳**（空口，按离线份额 31.5% 折算；离线 197.5 µs 是**被 GUI 污染**的高模式读数）；
* 其中 **barrier 27–38 µs（26–37%）**，**剩下的 66–77 µs 没有解释**（按"54×54 GJ ≈ 300k FLOP 在一个 SM 上"
  应该是 ~1 µs 量级，**差两个数量级**）；
* ⇒ **"剩下那 70 µs 是什么"是路 A 要通过测量回答的问题**，候选只是探针。

---

## 5. ★★ 已量掉 / 已禁用（**别重复**）

1. **逐 dispatch 计数器仪器**：这台设备**不可能**（只有 `AtStageBoundary` 一个采样点，compute 编码器上
   `sampleCountersInBuffer:` **断言 + SIGABRT**）。机制测量：`wip/metal_counter_caps.mm`。
2. **离线绝对 µs 判 A/B**：宿主 GUI 分时占 GPU ⇒ **同一个配置跨区块差 2.5–4 倍**。**先跑空臂**
   （base vs base）；空臂宽度 ≥ 要判的效应 ⇒ 该次读数作废。**只报比值/份额，绝对 µs 必须成对。**
3. **重复探针给的是"边际成本（上界）"，不是"份额"**：`CFO_REPEAT=2` 的斜率 72.8→23.7，而整段只降 19。
   **每个分段数都要用整段臂（`CPU_LS=1`、`REFORMAT_REPEAT=2`）或比值交叉验证。**
4. **`OCUDU_CE_DEV_INVERT` 这个旋钮不存在**（真名 `OCUDU_CE_GPU_INVERT`/`CPU_INVERT`）。
   **拼错的旋钮是静默空操作**，读数看起来像"这段不花钱"（实测 Δ=−5.0 µs）。
   ⇒ **报旋钮臂之前先在代码里确认名字存在。**
5. **`OCUDU_CE_DEV_Y=0` 与 `OCUDU_CE_DEV_SIGMA2=0` 都不是外科臂**（分别 21703 / 21692 字节不同，
   连设备 rSRP/TA 或 sigma2 一起关）⇒ **不能当"y scatter / sigma2 的成本"用**。
6. **`OCUDU_CE_INV_REPEAT` 只在【奇数】下数据可用**（偶数会把 A 反转回来）；`OCUDU_CE_INVERT_FIRST=1`
   **现在产出 NaN**（禁用）。
7. **CRC 只有按【调制】分层才能比**（§5.8.22）；**RF failure ≠ 0 的腿不算腿**；
   **上腿前关手机 WiFi**（手机开 WiFi 会自己拆蜂窝 PDU 会话 → "连上就 release"）。

---

## 6. 工具、旋钮与仪表

| 名字 | 说明 |
|---|---|
| **`wip/value_net.py`** | **门**：`[--env K=V（待加）] [--what both\|corpus\|narrow] [--quiet] [--report-only] [--self-test]` |
| `wip/neutral_vs_baseline.sh` | 逐字节网 = **信息**（不再阻断） |
| **`wip/k1_barrier_slope.sh`** | 腿对判读的模板：`bash wip/k1_barrier_slope.sh <对照腿> <探针腿>`（读 `busy split` 要取 **`.log.stderr`**）|
| `wip/leg_report.sh` | 单腿判读（CRC/RTF/契约/busy split/延迟）|
| `wip/run_leg.sh` | 跑腿：`sudo -E bash wip/run_leg.sh gpu <label> [OCUDU_*=…]`（旋钮作为**位置参数**）|
| **`OCUDU_CE_INV_BARRIERS=K`** | **保值**的 K1 barrier 探针（每 pivot 多 K 道）；空口斜率 0.22–0.31 µs/道 |
| `OCUDU_INV_TGX/TGY` / `OCUDU_INV_RL` | K1 的几何 / K1b（**数值错**）|
| `OCUDU_CE_{LSE,CFO,SMOOTH,SIGMA2,POWER,EPRE}_REPEAT` | 抽取的六个逐 dispatch 重复探针（**全部保值**）|
| `OCUDU_CE_LANE_ORDER` | `merged`（**默认**）/ `event`（P2 回退，一行）/ `host_wait` / `burst` |
| `OCUDU_CE_CPU_LS=1` | 关掉设备导频抽取（**整段臂**，用于量抽取，24 字节差）|
| `OCUDU_CE_WAIT_TRACE=1` | 引擎 11 个 `waitUntilCompleted` + 发布点的打点（问"谁等了这次提交"用它）|
| `k1_check`（构建目标） | K1 的**隔离台架**（纯 GPU 时间，20 次均值）——**离线读数当前不可信**，但接口在 |

---

## 7. 本会话的腿

| 腿 | commit | 头条 |
|---|---|---|
| `s14-extract-parallel_0920_1809` | `402a60adc8` | ✅ **抽取并行化**：`ch_wt` **340.7**（历史最好）、契约 8/8、穿越 0、RF 0 |
| `s14-k1-bar0_0920_1830` / `s14-k1-bar8_0920_1831` | `a01c66bfc5` | barrier 探针对：`ch_wt` 329.7 / 464.6 ⇒ **0.312 µs/道** |
| `s14p3-event_0920_1845` / `s14p3-merged_0920_1847` | `a55742c5f8` | P3 腿对：`cbs/lane` 2.00→**1.00**、`mmse_ce commits` 14248→**2**、gap 87.7→**0.0**；中位 1907.5→2378.3 |

---

## 8. 未解 / 开放项

1. **K1 那 66–77 µs 是什么**（路 A 要回答的）。
2. **`OCUDU_INV_RL` 不作用于独立入口**（`invert()` 硬编码 `inv_pipe`）——一行修。
3. **`merged` 下 `burst dispatches` 的 `channel_estimator=0` 是计数缺口**（`merged` 分支提前返回，
   没走到 `end_stage_async()` 里那次 `count_dispatch`）——纯计数。
4. **D1：DFT 还不在车道里**（每槽一次前端 CPU 提交；`dft commits` ≈1/槽）。文档原话：它是
   "the stage that moves onto the lane's queue when the fused pipeline lands"。**用户排的第二件事。**
5. **`merged` 的 +471 µs 里，~340 µs 的排队与 64QAM 层 CRC 变差（`reTx` 93→179）机制上一致，
   但没有单变量隔离。**
6. `shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉（静默错址，9 个调用点）。
7. `end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次（P0-⑦ 旁证）——未查。
8. §18.7 的 TA fence 实验仍未做；TA 链 = **80.1 µs**（重排 116.9 里的大头）。

---

## 9. 环境与流程

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  **改 `.metal` 后**：`rm -f lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib`
  → **全量** `cmake --build build -j 10` → 再 `--target` 显式重建要判读的产物。
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> [OCUDU_*=…]`
  —— **sudo 由用户执行**（把命令以纯文本贴给用户）；**Ctrl+C 停**；
  判读 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`。
* **★ 上腿之前关掉手机的 WiFi**；**判读前先看 `Real-time failures` 是否为 0**；
  **CRC 按调制分层比**。
* **用户裁定（长期有效）**：① `mode=gpu` 不允许宿主兜底；② dump 不算 CPU in the loop；
  ③ **控制面融合分阶段做、每阶段一次 OTA**（腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）；
  ④ **`merged` 保留为默认**（本会话新裁定）；⑤ **顺序：先 K1 路 A，再 DFT/D1**（本会话新裁定）。
* **排序标准是"功能正确前提下 CPU 还剩几个参与点"，不是收益**——K1 属于**成本优化**，
  按这条标准它是**可选项**；D1 才是剩下的目标性差距。
