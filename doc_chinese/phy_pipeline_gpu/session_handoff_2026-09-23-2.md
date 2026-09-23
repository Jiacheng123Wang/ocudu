# 交接（入口） — S36：**MMSE 那颗雷已拆（fenced + 合批已成默认）、边缘块守卫（Test 15）已落地、空口已确认**；下一刀 = **回到被雷堵住的主线：LA / 高阶调制的结论重建**

> 本 memo 是**交接快照**：写于 2026-09-23 会话末（12:45），HEAD = `5afb8abf50`。
> **开发过程中不要改本文件**；有需要记的，一律记进 `gpu_phy_pipeline_design_and_implementation.md`。
> 本文件覆盖的区间：`90ca0077ec`（上一份 memo `session_handoff_2026-09-23-1.md` 的 HEAD）→ `5afb8abf50`（本 memo）。

## 0. 开机三件事（照做，不要跳）

```bash
cd /Users/jiachengwang/dev/ocudu
# 1) 树与戳：工作树只应有两个【用户的】config 改动；戳必须 == HEAD，否则重建（run_leg.sh 会拒绝）
git log --oneline -3; git status --short
grep -oE '[0-9a-f]{10}' build/hashes.h | head -1; git rev-parse --short=10 HEAD
#    不等时： touch build/hashes.h && cmake --build build --target gnb -j 8
# 2) 离线门（约 1.5 分钟）
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py | tail -1        # 期望 captures=47 problems=0
ctest --test-dir build -R "metal|ul_pipeline_probe|puxch|lower_phy|du_low|o_du" | grep "tests passed"   # 期望 36/36（现在含 Test 15）
bash doc_chinese/phy_pipeline_gpu/wip/edge_block_arms.sh 6            # 期望：默认 6/6 绿；=0 臂 6/6 红（其中 5 次由 (d) 的 0/480 抓住）
./build/tests/unittests/support/executors/ul_pipeline_probe_test | grep PASSED   # 期望 6/6
./build/tests/unittests/phy/lower/lower_phy_test | grep PASSED                   # 期望 528/528
bash doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh 32 | grep -c '^PASS'   # 期望 5
bash doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 16 | grep differing          # 期望 4× differing=0
# 3) 动手前先读 §3 —— 那颗雷【已经修好】，不要再修一遍；§4 是下一刀
```

⚠ **`lib/` 是 `EXCLUDE_FROM_ALL`**：`cmake --build build` **不会**构建金属单测。改了单测要点名 target
（`cmake --build build --target port_channel_estimator_metal_mmse_unit_test`），否则 ctest 读的是**旧二进制**。

## 1. 一句话状态

* **里程碑仍成立**，且比上一份 memo 更强：默认路径（不带任何 env）`phy_pipeline_mode::gpu` ——
  契约 **8/8**、每跳宿主机数据穿越 **0.00 读 + 0.00 写**、`dropped=0`、`gaps=0`；
  `cbs/lane` 从 **1.00 → 2.00**（这多出来的 1.00 就是修雷**必须**付的那条命令缓冲区，已合批到最小）。
* **上一份 memo 的"唯一高优先级雷"已消失**：① 雷**修好并翻默认**（`107d499d28`）；
  ② **空口确认腿**（`3e987189f1`，`s62-default-fenced`）硬门全过、**槽级 `span` 反而更好**；
  ③ **边缘块守卫 Test 15 落地**（`5afb8abf50`），并带一条**确定性反向臂**。
* ⇒ `-1` memo 那句"**在拆掉它之前，LA 与高阶调制的任何结论都不作数**"**现在失效了**：那批结论可以开始重建（§4）。
* 本轮改动**只碰了单测 + 脚本 + 记录**：`git diff 3e987189f1..HEAD` = **3 文件 / 400 行纯新增 / 0 删除**
  ⇒ **生产代码与空口已确认的 `3e987189f1` 逐字节相同**（所以本轮没再跑空口腿，**也不需要**）。

## 2. 本轮新增的工具与开关

### 2.1 Test 15（边缘块守卫）+ 双臂脚本（新）

* `bash doc_chinese/phy_pipeline_gpu/wip/edge_block_arms.sh 6` —— 跑两个臂，**逐跑报是哪条判据判红的**。
* 四条判据（限全部来自 step-1 实测，论证见记录 §5.9.95）：
  **(a)** `rem_prb=0` 对照自检 1.10（它红了就喊"本用例没在测边缘块"，不是缺陷）；
  **(b)** 四个分配均值互比 1.10；**(c)** 单分配逐实现 band 8.0（干净 1.750，污染跳 14.7–160.1）；
  **(d)** **结构**：`lane_fence_nof_waits()` 增量 == 跳数（480/480）。
* ⚠ **只有 (d) 是确定性的**：雷是 ~**1/19000 跳**的竞态，480 跳的用例在**数值层**只能 ~2.5%/跑 抓到它。
  所以"改动前稳定复现"是靠 (d) 做到的（前缀形态下该计数**恒为 0**）。
  这条要记住，免得误以为 (a)–(c) 能挡回归 —— 它们挡的是**别的**缺陷。

### 2.2 相关 CE 旋钮（都在 `port_channel_estimator_metal_mmse_impl.cpp` 里读 env）

* **`OCUDU_CE_CORR_FENCED`**：默认 **ON**；`=0` = **回滚到前缀**（= 那颗雷）= Test 15 的确定性反向臂。
* `OCUDU_CE_CORR_STANDALONE`：改走"独立缓冲 + 宿主等待"的旧形态（诊断用）。
* 诊断期那批仍在（`OCUDU_CE_CORR_DEV`、`OCUDU_CE_INVERT_FIRST`、`OCUDU_CE_NV_CHECK`、`OCUDU_CE_CORR_CHECK`、
  `OCUDU_CE_EDGE_CHECK`、`OCUDU_CE_DEV_SIGMA2`、`OCUDU_CE_HOLD_EXTRACTION` …），但**默认路径不再需要任何 knob**。
* ⚠ 两个 check 类旋钮的老陷阱没变：**看的是"检查行的条数"，不是它的 DIFFERENT 结论**（`-1` memo §3.1 ⑤）。

### 2.3 腿法（没变，别退化）

```bash
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s6x-<label> [--log.phy_level=debug]
```

* **一个词一个参数**（`--option=value` 才进 argv）；**必须 Ctrl-C（SIGINT）停**并等收尾块；
  日志只在进程**真的退出**后才完整（读前先 `pgrep -fl build/apps/gnb/gnb`）；
* `--log.phy_level=debug` **只用于诊断崩溃**，不要拿它跑功能/性能腿。

## 3. 那颗雷：机制、修法、代价（**已修；不要再修一遍**）

### 3.1 机制（§5.9.86 → §5.9.92，逐跳证实）

相关矩阵构建作为**前缀**进了权重命令缓冲 ⇒ `memoryBarrierWithScope:MTLBarrierScopeBuffers`
**不保证前缀的写对 K1 可见** ⇒ K1 反演**半写的 A** ⇒ `A⁻¹` 最坏 **5049**（干净 ~2e-2）⇒
W（18144 个元素里 **16884 个错**）/ h（`rsrp` ×2.24、`|h|` ×1.43）/ K4 残差全错 ⇒ `nv` 可错到 **×39989**（一次实测）。
**只有命令缓冲边界（commit + wait）能排序这件事** —— A 是**宿主参数的纯函数**，所以坏值只可能来自"写到一半"。

### 3.2 修法（已是默认，`107d499d28`）

**fenced**：相关构建进**自己的命令缓冲**（`backend_stage_signal()` 武装共享后端 fence；调用方在
**打开编码器之前**编码一次 `backend_stage_wait_generation()`，于是 GPU 自己排序、宿主不等）
+ **合批**：merged 的两个组（标准组 + 边缘块）**共用一条**缓冲 ⇒ 一跳只多一次提交。
**回滚是一行**：`OCUDU_CE_CORR_FENCED=0`。

**两条"让宿主写 A"的路子已被契约第 5 条堵死**（`host device data crossings` 必须 0.00）：**别再试**。

### 3.3 代价（空口三条腿并列，记录 §5.9.94）

| 指标 | `s61-off`（前缀）| `s61-on`（未合批）| **`s62`（默认 = 合批）** |
|---|---|---|---|
| `cbs/lane` | 1.00 | 2.70 | **2.00** |
| `CE mean total` | 16.8 µs | 28.4 | **24.7**（相对前缀 +47%）|
| lane residency median / p99 | 478.8 / 1102.1 | 618.0 / 1459.6 | **552.0 / 1449.3**（+15.3% / +31.5%）|
| pipeline span median / p95 / p99 | 1868 / 5321 / 5642 | 1896 / 5282 / 5604 | **1820 / 5202 / 5565（三档都更好）** |
| 契约 / `stale` / crossings | 8-of-8 / 0 / 0.00 | 同左 | **8-of-8 / 0 / 0.00** |

* ⚠ **归因纪律**：`crc KO`（14.04% → 4.20%）、`RT failures`（50 → 2）、`starved_takes`（326 → 1）
  **看着更好，但不归因于修法** —— 雷只影响 ~0.07% 的跳，而 KO 的差是 ~1400 次；三条腿的话务与时长都不同。
* 车道余量约 **52% → 45%** ⇒ 更重话务（更多 PRB / 更高 MCS）下值得再看一条腿（可选，见 §4）。

### 3.4 那条"未复现的孤例"（记录在 §5.9.93 ②）

速率计曾报过一次 `worst_mmse_drift=1.262`（> 登记线 1.1、< 判据线 1.5 ⇒ **那一跑仍然是绿的**），
此后 **60+ 跑全部 1.054**（含 6 个 spinner 负载下的 20 跑 ⇒ **"并行重建干扰"这个解释已被实测排除**），
原始输出没留（脚本 `mktemp` 清掉了），**连它出现在哪条臂上都无法追**。
**处置：只记录，不建用例、不动门。** 复发时用 `OCUDU_CE_NV_CHECK=1` 一眼分家族：
**单跳爆点** ⇒ 竞态家族（说明 fenced 修法还有洞，要认真查）；**平滑位移** ⇒ 低电平数值家族（要重想用例和线）。

## 4. ★★★ 下一刀：**LA / 高阶调制的结论重建**（本轮解锁）

**为什么现在能做**：`nv` 是软比特的**分母**、也是 **LA 的 SINR 输入**；雷时代默认路径约 **26%/跑**
会把它污染 2×–101×，所以 `-1` memo 明写"在此之前 LA 与高阶调制的任何结论都不作数"。
现在默认路径是干净的：**40 跑 Test 3/9/11 = 0/0/0，每跑最坏漂移恰好 1.054**。

**做法（按顺序）**：

1. **先盘点、再重跑**：把记录里 LA / 高阶调制相关的既有结论列出来，逐条标注"出自雷时代（受污染）"还是"干净树"。
   这一步是重建的**大半**工作 —— 不要一上来就跑腿。
2. **`#12` LA 错配现在可以下结论**（`-1` memo §5 明写"在 §3.1 修好前不要下结论"）。
3. **两个从没被门禁卡过的量，值得在干净树上重新量基线**：`snr` 漂移 **1.261**、`|h|/l` 漂移 **1.156**
   （记录：这两条**在两条路径上完全相同** ⇒ "电平不变性"的破坏不止 `nv` 一个）。
4. **扫描腿**（阶数 / MCS，含 256QAM）：用 §2.3 的腿法跑，**先登记判据再跑**。
5. **可选、不阻塞**：重活腿看车道余量（现在 ~45%）；`1.262` 的复发监视（§3.4）。

**纪律（雷时代用血换来的）**：单次绿等于零证据（26% 失败率面前）⇒ 新结论要**连续 N 跑**的读数；
任何新判据**先证明它能红**（反向臂），再谈"变好了"。

## 5. 其余开放项

| # | 项 | 现状 |
|---|---|---|
| — | **MMSE Test 3 偶发**（NMSE 差 >1.5 dB）| **默认臂 40 跑 0 次**；前缀臂偶发（实测 1/8；此前 2/20）⇒ 现在是**前缀形态**的问题，不是默认路径 |
| **#13** | "延迟批次等待"（`defer_wait` 中位 ~840 / p95 ~1381 / p99 ~1752 / max ~3731 µs）| OPEN；下一步 = §5.9.81 的**批次三段拆分**（`fill` / `after_fill` / `pre_batch`，判据已预先登记）。注意这个量随话务变（`s62` 腿中位 675.7）|
| **#3** | 认领宽限期 `1.049 → 1.000` | OPEN，需空口 |
| **#10** | 下行方向的同口径记账 | OPEN（离线）|
| **#11** | §18.7 的 TA fence 实验 | OPEN，需空口 |
| **#12** | LA 错配 | **本轮解锁**，见 §4 |
| — | RX 池零余量（`held` 长期 7/8，曾到 8/8）| OPEN，未解释 |
| **#16** | Ubuntu 工作树 `git checkout -- lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp` | **用户侧家务** |
| — | `1.262` 孤例 | 记录了、未复现（§3.4）|

**既有偶发（不是回归，别误判）**：`port_channel_estimator_metal_mmse_unit_test` 的 **Test 3 / Test 9**
只在**前缀形态**（`OCUDU_CE_CORR_FENCED=0`）下偶发（历次实测：Test 3 = 1/8…2/20，Test 9 = 4/8…4/20）。
⇒ **默认路径 ctest 读到 36/36 是期望值**；若读到 35/36，**先看是不是自己带了 `=0`**。

## 6. 纪律（沿用 + 本轮新增）

1. **反向臂**：任何断言/判据都要先证明它**能红**，再谈"修好了"；单次绿在 26% 失败率面前等于零证据。
2. **先证明测试自身确定，再谈被测对象**（那个测试的种子本来就固定）。
3. **测的必须是你以为的那份代码**：还原文件后 `mtime` 可能更旧 ⇒ `touch`；读结果前先确认构建成功。
4. **编辑文档**：插入新段**只用正文行作锚点**；提交前 `git diff --numstat` 的**删除列必须为 0**。
5. **腿的"报告完整"≠"腿有效"**：先看 `contract` 是否 `(mode=gpu)`，再看 `radio sample continuity` 与 `host sample assembly`。
6. **别被 `max` 骗**：`[ul_pipeline] max` 是构造性封顶，要看 `stale`；`[edge_check] DIFFERENT` 是常态，要看行数。
7. **统计量选错，守卫等于没有**：均值把单跳**摊薄 1/n** —— 实测均值判据 12 跑只抓到 1 次，而逐实现
   band 判据对任何污染跳**全权重**灵敏。写守卫前先问"我要抓的事件在这个统计量里占多少权重"。
8. **不可能"稳定复现"的东西，别硬写"稳定复现"**：把反向臂建在**机制**上（本例 = "等待围栏的计数"），
   而不是建在稀有事件上；并把这个选择**写进用例注释**，免得后人以为数值判据在挡回归。
9. **加了断言要核对"判据没有扰动被测量"**：Test 15 落地后，12 个 `nv/l²` 均值与 step-1 记录**逐位相同**才算数。

## 7. 本轮的提交（`90ca0077ec` → `5afb8abf50`：区间内 15 个，其中本会话 14 个）

```
33c26ec070 上一份交接 memo（S35，session_handoff_2026-09-23-1.md）
77c103d6a9 5.9.86     644/715 是提前退出假象；雷钉在设备侧的 h
574c852824 5.9.87     雷隔离到"相关构建骑在权重命令缓冲的前缀里"
a60f9a94bf 5.9.87(5)  三臂批量：把"时序敏感"与"时序就是修法"分开
1c416ce86c 5.9.87(6)  调度类型：第五个便宜假设，实测排除
eb4deaf014 5.9.88     逐跳证实：前缀的 A 从不到达 K1，W 跟着错
44577da387 5.9.88(7)  确认"前缀跑了但没落地"；排除"它从来没跑"
b662f9e1f8 5.9.89     fenced 修法成立且过门；真正的代价是多一次提交
0299ad8d19 5.9.89(7)  空中 A/B 预登记；离线的百分比在腿上行不通
2d481e88b8 5.9.90     fenced 只覆盖了空中 26% 的跳；Test 3 与 Test 11 同源
588656ab7c 5.9.90(6)  空中 A/B 也读车道的结构 ⇒ 时钟噪声下代价仍可见
bcbe572ad7 5.9.91/92  空中 A/B 判读：代价是真的，硬门全过，槽级不动
107d499d28 5.9.93     fenced 合批成为默认，门在默认路径上达成
3e987189f1 5.9.94     空口确认腿：cbs/lane 2.70 → 2.00，槽级 span 反而更好
5afb8abf50 5.9.95     边缘块守卫（Test 15）：限从数据来 + 一条确定性反向臂
```

## 8. 关键坐标（行号对应 `5afb8abf50`）

* 设计文档：`doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md`
  —— 本轮记录 **§5.9.86 → §5.9.95**；**§5.9.88（逐跳链条）、§5.9.93（默认翻 + 门）、§5.9.95（边缘块守卫）** 是必读；
* 修法：`lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp`
  —— `corr_fenced_enabled()` 在 **310**；merged 标准组入队 **2218**、边缘块入队 **2645**、**flush 2730**；
  非 merged 路径的入队/flush 在 **4449–4451** 一带；
* 引擎：`.../metal/ocudu_metal_mmse_engine.mm` —— `flush_correlations_fenced()` 在 **2679**、
  `lane_fence_nof_waits()` 在 **3872**；接口 `.../metal/ocudu_metal_mmse_engine.h`（公开诊断 1005–1011）；
* 用例：`.../metal/test/port_channel_estimator_metal_mmse_unit_test.cpp` —— **Test 15 从 3375 起**，
  (d) 的计数在 **3424**（前）与 **3594**（后）；
* 脚本：`doc_chinese/phy_pipeline_gpu/wip/` —— **`edge_block_arms.sh`（新）**、`corr_fenced_ab.sh`、
  `mmse_outlier_rate.sh`、`mmse_landmine.sh`、`run_leg.sh`、`leg_report.sh`；
* 腿日志：`doc_chinese/phy_pipeline_gpu/wip/logs/`（本轮：`s61-on` / `s61-off`（fenced 未合批 / 前缀）、
  **`s62-default-fenced`（新的默认）**）。

## 9. 一句话给新会话

**那颗雷已经拆掉了**（fenced + 合批，`107d499d28`；空口确认 `3e987189f1`），**边缘块守卫 Test 15 也落地了**
（`5afb8abf50`，带一条**确定性**反向臂：`OCUDU_CE_CORR_FENCED=0` 时"等待围栏"的计数**恒为 0**，所以它每次跑都红）。
**不要再修一遍雷，也不要再重做边缘块用例。**
下一刀 = **§4：回到被雷堵住的主线 —— LA / 高阶调制的结论重建**。
起点**不是跑腿**，而是**先盘点旧结论里哪些出自雷时代**，再在干净树上重建基线；
新结论一律要**连续 N 跑**的读数，且**判据先证明能红**。
