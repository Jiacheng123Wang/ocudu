# 交接（入口） — S35：**console 三族已收干净、D1 竞态已修、#13 的尾巴已定位到"延迟批次等待"**；下一刀 = **拆掉 MMSE 那颗雷（已收敛到"边缘块那一跳"）**，前置是**修正版边缘块用例**

> 本 memo 是**交接快照**：写于 2026-09-23 会话末，HEAD = `90ca0077ec`。
> **开发过程中不要改本文件**；有需要记的，一律记进 `gpu_phy_pipeline_design_and_implementation.md`。
> 本文件覆盖的区间：`b911f554a1`（9-22 末）→ `90ca0077ec`（本 memo）。

## 0. 开机三件事（照做，不要跳）

```bash
cd /Users/jiachengwang/dev/ocudu
# 1) 树与戳：工作树只应有两个【用户的】config 改动；戳必须 == HEAD，否则重建（run_leg.sh 会拒绝）
git log --oneline -3; git status --short
grep -oE '[0-9a-f]{10}' build/hashes.h | head -1; git rev-parse --short=10 HEAD
#    不等时： touch build/hashes.h && cmake --build build --target gnb -j 8
# 2) 离线门（约 1 分钟；ctest 会因两个既有偶发读作 35/36，见 §5）
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py | tail -1          # 期望 captures=47 problems=0
ctest --test-dir build -R "metal|ul_pipeline_probe|puxch|lower_phy|du_low|o_du" | grep "tests passed"
./build/tests/unittests/support/executors/ul_pipeline_probe_test | grep PASSED   # 期望 6/6（四条反向臂）
./build/tests/unittests/phy/lower/lower_phy_test | grep PASSED                   # 期望 528/528
bash doc_chinese/phy_pipeline_gpu/wip/l1_handover_arms.sh 32 | grep -c '^PASS'   # 期望 5
bash doc_chinese/phy_pipeline_gpu/wip/l1_hop_arms.sh 16 | grep differing          # 期望四条 differing=0
# 3) 动手前先读 §3（那颗雷），否则会重复踩
```

## 1. 一句话状态

* **里程碑仍成立**：默认路径（不带任何 env）`phy_pipeline_mode::gpu` —— 契约 **8/8**、
  每跳宿主机数据穿越 **0.00 读 + 0.00 写**、`cbs/lane=1.00`、`dropped=0`、`gaps=0`；
  最近六条腿（s53/s55/s56/s57/s59/s60）反复确认。
* **本会话三件已落地**：① console 三族（`[ul_rx_pool]` / `[dft_handover]` / `[d1_handover]`）全部改到
  ocudulog "PHY" 的 **debug** 级，console 上只剩报告块；② D1 交棒的 Metal 竞态**已修**（三腿无断言）；
  ③ #13 的尾巴**已定位**（不是活量、不是等样本、不是车道、不是解码，而是**延迟批次等待**）。
* **当前唯一的高优先级雷**：**MMSE 设备侧相关矩阵构建的非确定性**（默认路径 ~26%，`nv` 可错到 101×），
  已收敛到"**边缘块那一跳**"。**在拆掉它之前，LA 与高阶调制的任何结论都不可信。**

## 2. 本轮新增的"工具与开关"（新会话会用到的全部）

### 2.1 三条新仪表（都在关机报告里，默认不刷 console）

| 行 | 含义 | 关键读数（健康腿） |
|---|---|---|
| `[ul_pipeline] stale=` | 超过阈值（`OCUDU_UL_STALE_US`，默认 8000）的样本**被移出主序列** | **看 `stale`，不要看 `max`**（`max` 是构造性封顶）|
| `[mmse_time_sum] defer_wait distribution:` | 每跳"延迟阶段结束→批次完成"的**分布** | 中位 ~840 / p95 ~1381 / p99 ~1752 / max ~3731 µs |
| `[ul_by_size]` | 每跳两条跨度按**该槽 TB 字节数**分四档 | 中位平坦（x1.03）⇒ 尾巴与活量无关 |

`[ul_slot_trace]`（`OCUDU_UL_SLOT_TRACE=N`，N≤512）现在**行为正确**：触发判据、绑定、
保留策略（**最慢的 N/4 行抗老化**）、原始时刻快照都已修；**注意**：融合车道里 `t2f`/`ce` 两列恒为 nan
（那两条阶段序列在车道内不记录），可用列是 `rxwait / ldpc / crc_ok / tb_bytes / pipeline`。

### 2.2 CE 的 A/B 旋钮（都在 `port_channel_estimator_metal_mmse_impl.cpp` 里读 env）

* **定位那颗雷用过的**：`OCUDU_CE_CORR_DEV`（=0 相关矩阵由宿主建）、`OCUDU_CE_DEV_SIGMA2`、
  `OCUDU_CE_K0A_RATIO_DEV`、`OCUDU_CE_CFO_CARRY_HOST`、`OCUDU_CE_HOLD_EXTRACTION`、
  `OCUDU_CE_LANE_ORDER=wait`、`OCUDU_CE_FUSED_BURST`、`OCUDU_CE_TAIL_DEV`；
* **check 类（会打印设备 vs 宿主的比对）**：`OCUDU_CE_CORR_CHECK=1`、`OCUDU_CE_EDGE_CHECK=1`
  ⇒ **看的是"检查行的条数"，不是它的 DIFFERENT 结论**（见 §3.1 ⑤）；
* **旁路**：`OCUDU_CE_NV_OVERRIDE=<float>`（用已知 nv 替掉设备值）。

### 2.3 腿法（别退化）

```bash
# 一个词！--option=value 才进 argv；两个词会被 run_leg.sh 拒绝（本会话踩过，已改成硬拒绝）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s61-<label> [--log.phy_level=debug]
```
* **必须 Ctrl-C（SIGINT）停**：SIGTERM 会跳过全部 atexit 报告；
* **日志只在进程真的退出后才完整** ⇒ 读之前先 `pgrep -fl build/apps/gnb/gnb`；
* `--log.phy_level=debug` **只用于诊断崩溃**，不要用它跑功能/性能腿（它把 PHY 热路径日志全打开）。

## 3. 本轮最重要的技术结论（新会话必须知道）

### 3.1 ★★★ MMSE 那颗雷：**设备侧相关矩阵构建的非确定性**（本会话最大发现）

**症状**：`port_channel_estimator_metal_mmse_unit_test` 的 Test 9 偶发红：
`nv/level²` 在 8 个电平上的 max/min 超过 1.5（判据），**实测最大 101.67×**。

**已确定的读数（全部可复现）**：

| 事实 | 读数 |
|---|---|
| 默认（设备相关）失败率 | **10/38 = 26.3%**（40 次/臂）|
| `OCUDU_CE_CORR_DEV=0`（宿主相关）| **0/40**，且每次都是**同一个值 1.054** ⇒ `P(0/40)≈5e-6` |
| 种子 | **本来就固定**（`rng(1234)`）⇒ 输入确定；同输入下**其它所有数值逐位相同**（`SNR+10 NMSE=-20.02` 十次一致）⇒ **非确定性**，不是数值病态 |
| 坏点位置 | 逐电平表：**只有最高电平 `level=1.0`**（`1.385e-2` vs 其余 6.8–7.0e-3）；该值是 **40 次实现的均值** ⇒ 整个电平被污染 |
| 六个 A/B 臂 | `HOLD_EXTRACTION` 26.5%、`LANE_ORDER=wait` 19.4%、`FUSED_BURST=0` 25.0%、`K0A_RATIO_DEV=0`、`DEV_SIGMA2=0`、`CFO_CARRY_HOST=1`（max 327×）—— **都不能修**；只有 `CORR_DEV=0` 能 |
| **★ 最锐利的判据** | 开 check 旋钮后：**通过态 715 条检查行（8/8），失败态 644 条（4/4）**；差的 **71 组全是"边缘块"几何**（`L_e=54`、`a_stride=56`）|
| **⚠ 别被误导** | `[edge_check] … DIFFERENT`（`worst |dev-host|` 高达 79）**在通过态也出现** ⇒ **信号是行数，不是结论** |

**含义**：`nv` 是解映射软比特的**分母**，也是 **LA 的 SINR 输入**。默认路径 26% 的运行里它可错 2×–101×，
后果正是该测试注释写下的两种（LLR 太大饱和 / 太小被截断跳过解码）。**`CORR_DEV=0` 是诊断不是修法。**

### 3.2 #13 的尾巴：**是"延迟批次等待"，不是计算**

| 候选 | 实测（s59/s60）| 判定 |
|---|---|---|
| 等样本 | `rxwait` 慢行/快行 **逐位相同**（1057 / 1057）| ✗ |
| 这跳的活量 | `tb_bytes` 相同；`[ul_by_size]` 两条腿都 **x1.03** | ✗ |
| 车道 | `residency` **max ~1.3 ms**、`gap ≡ 0` | ✗ |
| LDPC 解码 | 中位 **50–71 µs** | ✗ |
| **延迟批次等待** | `defer_wait` 中位 **840** / p95 1381 / p99 1752 / **max 3731 µs** | **✓** |

账：中位跨度 2019 ≈ `rxwait` 1057 + `defer_wait` 840 + ~121 µs
（**不必再加车道的 602** —— 引擎注释写明"均衡与解映射跑在那段等待里面"）。
慢行**成串**出现（s59：27 段短段；s60：3 段长段；相邻慢行 slot 间隔多为 1）⇒ **共享资源被挡住**的签名。

### 3.3 D1 交棒的 Metal 竞态（已修，`819e1e1f35`）

`shared_burst::deposit_released()` 把条目**先发布进表**（锁内），**再**（锁外）挂完成回调
`mark_handed_produced` ⇒ 另一线程可以在这之间 `take_released()` 认领并**提交**该缓冲 ⇒
`addCompletedHandler` 落在已提交的缓冲上（`Abort trap: 6`，两条腿复现，`.ips` 栈已存档）。
修法：**把挂回调移到"发布之前"**（此时无人能认领 ⇒ 缓冲保证未提交）。三腿（58 / 15,618 / 15,933 跳）无断言。

### 3.4 仪器本身的缺陷（都已修，且都配了反向臂）

`[ul_slot_trace]` 一共修了**四处**：① 触发判据（`7680 < 7680` 恒假 ⇒ `rows=1`）；
② 绑定未被遵守（`=64` 打出 512 行）；③ 原始时刻打印时现查 ⇒ 62% 的行跨 SFN 周期（差 10.238 s）；
④ 淘汰只留最新 ⇒ 对瞬态失明（现改为**最慢的 N/4 抗老化**）。
另：`tf_from_done` 列曾是 `t2f` 的副本（已删）；`stale` 与主序列**互斥**（`max` 是构造性封顶）。

### 3.5 RX 池余量（未解，但已量化）

默认路径上 `held` 长期到 7/8、并有过 8/8；**与 trace 无关**（无 trace 腿也进 82 次饥饿），
但 trace 会把"碰一下"变成"常驻"（约 200×）。`pop_blocking` 把耗尽转成**接收线程阻塞**（所以 `gaps` 仍为 0）。
判据：`[ul_rx_pool]` 汇总行的 `starved_takes / starved_events / held_max`。

## 4. ★★★ 下一刀（按顺序，第 1 步是前置）

1. **落"修正版"边缘块用例**（设计已定稿在 §5.9.85 ①–⑦，**修正见 ⑧**）。
   * 第一次实现**已实测无效并回退**：它 8/8 次运行、5/5 个切分全红（漂移上万倍），**连"无边缘块"的对照也红**
     ⇒ 量的是"1–6 PRB + 单次实现"下噪声估计自身的方差，不是边缘块；
   * **修正后的做法**：**平均实现数 ≥16（理想 40，Test 9 已证明必需）**，
     或**只在宽带下扫切分**（49/50/51/52 PRB = `k·4+1/2/3/0`），把窄带留给结构性断言（I2/I4）；
   * **必须自带自检**：`rem_prb = 0` 的对照组**必须绿** —— 若它也红，说明测的不是边缘块。
2. **找到能"双向"切换 715/644 的旋钮** ⇒ 定位"边缘块这一跳走不走"由什么决定。
   入口：`OCUDU_CE_EDGE_FUSE` / `CHAIN_MAP` / `SPLIT_TAIL` / `PAD_SENTINEL`；
   **判据**：能切过去、也能切回来的那一个（单向不算）。
3. **修病灶**（必须落在**默认路径**上）。门：**改动前稳定复现**（把 26% 变成确定）+
   **改动后连续 20 次全绿且漂移 ≤1.1**（单次绿等于零证据）。
4. 然后做 **§5.9.81 的批次三段拆分**（`fill` / `after_fill` / `pre_batch`，判据已预先登记）
   ⇒ 把"延迟批次等待"从"知道在哪一段"推进到"知道为什么"。
5. **#3 认领宽限期**（`1.049 → 1.000`）：需要空口调参 + 反向臂，未设计。

## 5. 其余开放项（优先级从高到低）

| # | 项 | 现状 |
|---|---|---|
| — | **MMSE Test 3 偶发**：`metal_mmse` NMSE 比 CPU 差 >1.5 dB（实测 +1.76，1/20）| 既有；与 §3.1 可能同源 |
| **#3** | 认领宽限期 1.049 → 1.000（s47 `fallback=12165`）| OPEN，需空口 |
| **#10** | 下行方向的同口径记账 | OPEN（离线新工作）|
| **#11** | §18.7 的 TA fence 实验 | OPEN，需空口 |
| **#12** | LA 错配（已记账，交调度/解码器侧）| **在 §3.1 修好前不要下结论** |
| **#4** | 单车道并发 1→2 | **已降级**（§5.9.47 ⑤ 一槽至多 1 PUSCH，99.97%）|
| — | RX 池零余量（§3.5）| OPEN，未解释 |
| **#16** | Ubuntu 工作树 `git checkout -- lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp` | **用户侧家务** |

**既有偶发（不是回归，别误判）**：`port_channel_estimator_metal_mmse_unit_test` 的 **Test 9**（本 memo §3.1）
与 **Test 3**（NMSE），二者合计约 35% 的运行会红 ⇒ **ctest 读作 36/36 或 35/36 都算正常**。

## 6. 纪律（本会话反复救命的几条）

1. **反向臂**：任何断言/判据都要先证明它**能红**（临时抽掉修复跑一次），再谈"修好了"；
   单次绿在 26% 失败率面前**等于零证据** ⇒ 这类门要 **20 次**。
2. **先证明测试自身确定，再谈被测对象**（我差点给一个种子本就固定的测试加种子旋钮）。
3. **测的必须是你以为的那份代码**：还原文件后 `mtime` 可能比目标文件旧 ⇒ **`touch`**；
   **读测试结果前先确认构建成功**（`make: Error 2` 之后我读到的是旧二进制）。
4. **编辑文档**：插入新段**只用正文行作锚点**（拿标题当锚点已四次吃掉 `### 5.9 D1` 标题/引言）；
   提交前 `git diff --numstat` 的**删除列必须为 0**。
5. **腿的"报告完整"≠"腿有效"**：argparse 报错时它照样打完整报告（每个计数为 0）⇒ 先看 `contract` 是否 `(mode=gpu)`。
6. **别被 `max` 骗**：`[ul_pipeline] max` 是构造性封顶，要看 `stale`；`[edge_check] DIFFERENT` 是常态，要看行数。

## 7. 本轮的提交（`83b1dcbcab` → `90ca0077ec`，共 17 个）

```
83b1dcbcab  [ul_rx_pool] 离开 console（PHY debug + 一条 shutdown 汇总）
30ee38473b  [dft_handover] 同法 + 5.9.69（s53 分析、D1 竞态发现）
1f4e713c64  run_leg.sh 裸词改硬拒绝（两词参数曾白跑一条腿）
19f16bae0c  5.9.71（"手机接不上"不是回归 + 我引入的混淆因素）
7a86def88c  [d1_handover] 同法 + 5.9.72/5.9.73
2ecbf8a603  5.9.74（s56 判定 + stale 线索用数据关掉）
579725d7ea  5.9.75（#13 前置：触发判据 + tb_bytes + [ul_by_size]）
a6636178d2  5.9.76（s57：#13 得到否定答案 + 60 ms 卡顿 + 绑定缺陷）
4033be3927  5.9.77（s58 验证 + 原始时刻快照 + 取样偏置）
f9bfecd5b7  5.9.78（trace 保留最慢行 + 反向臂实测）
26c645b066  5.9.79（s59：尾巴定位 + defer_wait 分布 + 既有偶发）
0d59b4942c  5.9.80（s60：defer_wait 就是尾巴）
e42c471b17  5.9.81/5.9.82（两份设计：批次三段 + 那颗雷）
0b3f4edb4c  5.9.83（雷已定位：设备相关构建非确定）
09fe69f99b  5.9.84（收敛到边缘块那一跳：644 vs 715）
71e44e77a8  5.9.85（边缘块专用用例设计）
90ca0077ec  5.9.85(8)（快速层实测无效并回退）
```
（`819e1e1f35` = D1 竞态修复，在本区间的中间。）

## 8. 关键坐标（少走弯路）

* 设计文档：`doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md`
  —— 本轮记录 **§5.9.68 → §5.9.85**；**§5.9.84（雷的定位）与 §5.9.85（用例设计 + ⑧ 修正）** 是下一刀必读；
* 相关源码：`lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp`
  （边缘块在 **2524–2588** 行；切分在 **2046–2047**；`defer_wait` 在 **4613**；统计与报告在 **125–215**）、
  `.../metal/ocudu_mmse_pilots.metal`（相关矩阵 kernel，`denom` 在 583）、
  `.../metal/test/port_channel_estimator_metal_mmse_unit_test.cpp`（Test 3/6/6b/9/12/14）、
  `include/ocudu/support/executors/ul_pipeline_probe.h`（三条新仪表 + 四条反向臂的用例在 `tests/unittests/support/executors/ul_pipeline_probe_test.cpp`）、
  `lib/phy/lower/lower_phy_baseband_processor.cpp`（收包路径与 RX 池汇总）；
* 腿日志：`doc_chinese/phy_pipeline_gpu/wip/logs/`（本轮：s52…s60；`s59-stall`、`s60-defer` 是 #13 的关键两条）。

## 9. 一句话给新会话

**console 三族收干净了、D1 的交棒竞态修好了、#13 的尾巴定位到"延迟批次等待"了；
现在唯一的雷是 MMSE 那颗 —— 它不在算法里，在"边缘块那一跳"的路径上（默认路径 26% 的运行里
设备侧相关矩阵走了另一条路，`nv` 可错到 101×，而 `nv` 是软比特的分母、也是 LA 的输入）。
下一刀 = §4：先落"修正版边缘块用例"（务必带 `rem_prb = 0` 的对照组自检 + 平均 ≥16 次，
第一次实现就是因为少了这两样而被判无效并回退），再看它红，
然后用 §5.9.84 的方法（644 vs 715 的检查行数）找到那个能双向切换的旋钮 ⇒ 修默认路径 ⇒
**连续 20 次全绿且漂移 ≤1.1** 才算修好 —— 在此之前，LA 与高阶调制的任何结论都不作数。**

