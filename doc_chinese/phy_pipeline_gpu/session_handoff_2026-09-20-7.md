# 交接（入口） — S16：**K1 到头（否证）；收包粒度不是杠杆（否证）；D1 已定价，下一步 = 去掉宿主的每槽 DFT 等待**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **上一份 `session_handoff_2026-09-20-6.md` 不要读**（它描述的是 S15 开工时的状态：那时 K1 路 A 还没做，
> 目标两半已达成那部分仍然成立，但"接下来做 K1 路 A"这条已经执行完了）。
> **技术细节全在常驻设计文档 `gpu_phy_pipeline_design_and_implementation.md` 的 §5.8.15 – §5.8.31 与 §5.9 – §5.9.2**
> ——**本会话新增**：§5.8.28（K1 的墓志铭）、§5.8.29（`[ul_time_frequency]` 为什么是 ~1000 µs）、
> §5.8.30（收包粒度腿对：不是杠杆）、§5.8.31（`[ul_slot_trace]`，含 ⑨⑩ 两条结论）、
> **§5.9 / §5.9.1 / §5.9.2（D1 的范围分析与定价——开工前必读这三节）**。

---

## 1. 一句话状态

**工作树 HEAD**：本会话在 S15 的 `ddc09868e4` 之后加了 **24** 个提交（见 §7 的 `git log ddc09868e4..HEAD`）。
**开工第一件事就是自查戳记**：
```bash
git rev-parse --short=10 HEAD && grep build_info build/hashes.h
```
**两个短哈希必须相同**（不同就 `touch build/hashes.h && cmake --build build --target gnb`，坑 35）。
**本会话末已重戳且一致**；若你（新会话）看到不一致，说明有提交没重戳。
工作树只剩**用户自己的两个 config**（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`gnb_rf_b200_tdd_n78_20mhz.yml`），**别动**。

**门（与 S15 相同，本会话未改门）**：
```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py             # 47 捕获 0 问题  ← 本会话末复跑通过
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 8/8              ← 本会话末复跑通过
ctest --test-dir build -R metal                                   # 9/9              ← 本会话末复跑通过
```
**逐字节网 `neutral_vs_baseline.sh` 仍是"信息"不是门**（当前树 **131 字节**不同——**与 S15 记的是同一个数**，
即本会话的改动**对出厂路径零影响**，这是刻意验证过的，见 §3）。

---

## 2. ★★ 本会话做成了什么：**三条线索，两条否证 + 一条定价**

| # | 线索 | 结论 | 硬证据 |
|---|---|---|---|
| **一** | **K1 路 A** | ❌ **K1 已到头**——栅栏免费、**已经是 1 barrier/pivot**、几何最优、线程少反而慢 | `mem_threadgroup` 207.0 µs vs `mem_none` 207.1 µs（**1.000×**）；三版"更少 barrier"的重写**全部算错**（§5.8.28）|
| **二** | **收包粒度**（用户提出的"桶 vs 杯子"） | ❌ **不是杠杆**——每槽等待两条腿**完全相同** | 每槽等待 **997.9 vs 997.4 µs**（六次复现）；端到端 **−1.6%**（§5.8.30）|
| **三** | **D1 定价** | ✅ **宿主每槽被 DFT 挡住 ~507 µs**；**跨队列关系免费** | `[ul_dft_wait]` median **506.6 vs 504.4 µs**（同队列臂**没有改变它**）；**⇒ D1 收益 = CPU 参与点，不是延迟**（§5.9.2）|

**机制结论（回答"为什么提前做完却不提前"）**：**前端没有提前做完——它在等最后一个样点，不是算得慢。**
`s25` 的逐槽时间轴：`rxwait ~350 µs` + `t2f ~1080 µs` = `pipeline ~1150 µs`（自洽），
其中 ~500 µs 是等后半槽、一个槽的 CPU FFT 只有 ~230 µs 量级（§5.8.31 ⑩）。

**⇒ 排序标准（"CPU 还剩几个参与点"）不变：D1 仍然该做，但理由要写成 CPU 参与点，不能写成延迟。**

---

## 3. 本会话对【代码】做了什么（都已离线验证，出厂路径零影响）

| 交付物 | 说明 | 验证 |
|---|---|---|
| **`wip/value_net.py --env K=V`** | 门现在**能带着旋钮跑**（可重复；另有 `--env-passthrough`）。**没有它，门只验出厂路径**——候选在旋钮后面，门看不见它。拼错（少 `=`）**返回 2 并报错**，不静默 | `--self-test` 8/8；`--env OCUDU_CE_INV_BARRIERS=2` 跑出 47/0 |
| **`mmse_inv_pipeline()`（engine 新增）** | **三个 K1 dispatch 共用一个选择函数**：独立入口 `invert()`（**`k1_check` 走的就是它**）+ `run_async()` 内联 + `run_weights_only()` 内联。**修掉了开放项 2**：原来 `OCUDU_INV_RL` 只作用于内联那一个，台架**看不见任何 K1 旋钮** | `k1_check` 是独立二进制、不随 `gnb` 重建，实测横幅出现 ✅ |
| **`[inv_kernel]` 选择横幅** | 任何非默认 K1 kernel 被选中时**一次性打到 stderr**。理由=坑 4：**不存在的/拼错的/元库没有的旋钮是静默空操作**，读数看起来像"这个候选没变化" | 见上 |
| **`mmse_inv_memnone`（K1m）+ `OCUDU_INV_MEMNONE`** | **探针，不是候选**（结果未定义）。它回答"那 2 µs/pivot 是栅栏还是同步"——**答案是同步** | §5.8.28 ② |
| **新增文档 §5.8.28** | K1 路 A 的完整执行记录（含**三版错误重写各自的错因**、两台不可信仪器的判定）| — |
| **★ 删掉了什么** | 1) 三版错误的 K1s 重写（**不留错误代码**）；2) **K1x 消融探针**——它的读数**不可复现**（首轮说"pivot 和 sweep 各占 200 µs"，几分钟后复跑五个臂全部 ~340 µs），**结论不可复现的仪器不该留在树上** | — |

---

## 4. ★ K1 的四个否证（**别再重复这些**）

1. **"把 barrier 变便宜"整族**：栅栏免费（1.000×）。少 fence 流量 / `simdgroup_barrier` 替代 /
   scratch 重写——**全部无效**。
2. **"少 barrier、同工作量"三版重写**（都算错，错因各不同，**照抄会再踩**）：
   * **K1s-1**（把正规化行写进别处 + 删"消元后"那道 barrier）：**写后读**——消元 pass 读的**行 p 原始值**
     会被下一个 pivot 的 phase A 覆盖 ⇒ n=54 全 NaN、n=18 相对误差 3.8e5。
   * **K1s-2**（barrier 挪到两 pass 之间 = 1 道/pivot 的正确位置）：**仍错**——`mult` 里该放的不是
     `a_pj/a_pp`，而是**对角块累积到最后的状态**（实测 pivot 5 之后主元已变成 −18420）。扫掠乘的是后者。
   * **K1s-3**（+ 区分"块内已消元/未消元"+ 末 pivot 采集）：**仍错**（相对误差 7e4…1e11）——
     块内**未消元**的后继行不能整行改写成 `pivot×inv`：**它们身上有前面 pivot 的消元累积**。
   ⇒ **结论**：K1 用"原地写回正规化行"把两件事合并成一次写，**115 道里没有一道是白放的**。
   唯一理论上可去的是**每块列收尾那 1 道**（7 道 = **6%**）——**低于空口噪声，我没为它上腿，不作为候选**。
3. **几何/线程数**：默认 (64,16)**最优**；线程少**更慢**（32×4 = 2.42×）。**别再扫几何**。
4. **算法量**：$n^{1.27}$（S15 已记）——**算术量不是主导**。换算法（Cholesky/Newton–Schulz）只买到算术量，
   且**必然改变舍入** ⇒ 要重新定基线 + 至少 2 条腿，**收益上限很低**（K1 已是 1 道 barrier/pivot）。

---

## 5. ★ 开工第一步：**去掉宿主的每槽 DFT 等待**（D1 的最小形状）

> **★ 先读 §5.9 / §5.9.1 / §5.9.2**：D1 的目标、**两个前提更正**、以及**已经量完的定价**都在那里。
> **不要重做 §5.9 ⑤ 的路②**——§5.9.2 ③ 已经把它否掉（跨队列关系免费）。

### 5.1 D1 的真实收益（已定价，别再假设）

| D1 想消的 | 实测 | 判定 |
|---|---|---|
| **宿主每槽阻塞于 DFT** | **~507 µs/槽**（`[ul_dft_wait]`）| ✅ **值得消——这是 D1 的唯一理由** |
| 跨队列栅栏 | **0** | ❌ 不构成理由 |
| 端到端延迟 | **未测出效应** | ❌ **不能当理由** |

### 5.2 ★★ 两个必须先知道的前提（否则会用错模式/跑空腿）

1. **`cpu` 模式下 D1 的对象【不存在】**：`phy_pipeline_mode::cpu` 强制所有后端走 CPU
   （`du_low_phy_pipeline.h` 第 163–172 行：任何非 CPU 的 `--pusch_dft_type` 直接报冲突拒绝启动）
   ⇒ **没有 Metal DFT、没有前端队列**。**D1 的腿必须用 `cpu_gpu` 或 `gpu`。**
   （`s17`–`s26` 那批 `cpu` 腿对"收包粒度/等待"的结论**仍然成立**，只是不能判 D1。）
2. **前端栅栏默认没武装**（`OCUDU_UL_FRONTEND_FENCE`，默认 false）⇒ 默认下网格顺序由
   **宿主每槽一次 `wait_slot()`** 提供。**凡是以该开关为基础的臂都必须显式设为 1。**

### 5.3 ⇒ 下一步的确切形状（改动点已定位到行）

`lib/phy/lower/modulation/ofdm_demodulator_impl.cpp`（`finish_symbol()`）：

```cpp
const bool wait_per_slot = pipeline_slots[slot].device_write && grid_consumed_on_device;
if (!wait_per_slot || last_symbol_of_slot) {
  dft->wait_slot(slot);          // <-- 宿主每槽等一次 DFT：要消的就是它
}
```

**做法**：加默认关闭的旋钮（如 `OCUDU_DFT_HOST_WAIT=0`）跳过这个等待，顺序交给**已武装的前端栅栏**。

**⚠ 上一行必须先 `end_block()`**（它已经在上面几行）——把 DFT 的命令缓冲**提交**，否则栅栏等的是一个
**永远不会被 signal 的代际**（**P0 的签名：静默错数据**）。§5.8.26 ② 记过同一个形状的缺陷。

**判据（全是结构性的，不需要 GPU 计时）**：
1. **`[ul_dft_wait]` 降到 0**（宿主是否真的退出）——**这就是 D1 的目标本身**；
2. 契约的 `ce device estimates` / `equalizer ch_re` **仍 host=0**（没有宿主消费者漏网）；
3. `[ul_pipeline]` / CRC **不劣化**（**不预设改善**，见 5.1）。

### 5.4 腿法

```bash
# 对照：Metal DFT + 默认宿主等待
sudo -E bash wip/run_leg.sh cpu_gpu s28-d1-wait
# 候选：跳过宿主等待，顺序交给栅栏
sudo -E bash wip/run_leg.sh cpu_gpu s28-d1-nowait OCUDU_UL_FRONTEND_FENCE=1 OCUDU_DFT_HOST_WAIT=0
```

**判读前先看 `[ul_dft_wait]` 是否有样本**（`cpu_gpu` 下必须有；`no samples` 说明模式又选错了）。

---

## 6. 工具、旋钮与仪表（本会话的更新已并入）

| 名字 | 说明 |
|---|---|
| **`wip/value_net.py`** | **门**：**新增 `--env K=V`（可重复）与 `--env-passthrough`**；另有 `--what both\|corpus\|narrow`、`--quiet`、`--report-only`、`--self-test` |
| `wip/neutral_vs_baseline.sh` | 逐字节网 = **信息**（不阻断）。**当前树 131 字节**，与 S15 同数 ⇒ 出厂路径未动 |
| **`OCUDU_INV_MEMNONE=1`** | **新增**：选 K1m（**栅栏被去掉、结果未定义**）——只用于"栅栏 vs 同步"的判定，**已判定完，答案=同步** |
| **`[inv_kernel]` 横幅** | **新增**：非默认 K1 kernel 被选中时打 stderr。**报旋钮臂之前先看它**（坑 4） |
| `OCUDU_CE_INV_BARRIERS=K` | K1 的 barrier 探针：**+16.2 µs/道**（背靠背，**上界**）。三档独立线性：15.9 / 16.1 / 16.4 |
| `OCUDU_INV_TGX/TGY` / `OCUDU_INV_RL` | K1 几何（**已证 (64,16) 最优**）/ K1b（**数值错**）。**两个入口都认了**（本会话修） |
| **`k1_check`（构建目标）** | K1 的隔离台架。**⚠ 它的 `last_gpu_wait_us()` 对单发是"队列跨度不是 kernel 时间"**（1/2/4/8 system 全读 206 µs）——**只能当上界** |
| **`[ul_rx_wait]`** | **本会话新增**：包住 `receiver.receive()` 的一条序列，把"等样本"从 `[ul_time_frequency]` 里拆出来（**唯一没有配对的序列**，按**块**计数）。读法：**`[ul_time_frequency] − [ul_rx_wait]` = 前端自己花在样本上的时间** |
| `OCUDU_CE_LANE_ORDER` | `merged`（**默认**）/ `event`（P2 回退，一行） |
| `OCUDU_UL_RX_SYMBOLS=N` | 整符号收包（**实验性**，有已知停机竞态）。**实测拿到 0.5 slot 粒度**（§5.8.30 更正）|
| **`[ul_dft_wait]`** | **新增**：宿主在 DFT `wait_slot()` 里阻塞多久（**不依赖任何开关**）。**只在 `cpu_gpu`/`gpu` 有样本**（`cpu` 模式无 Metal DFT）|
| **`OCUDU_DFT_BACKEND_QUEUE=1`** | **新增**：DFT 提交到后端队列。**实验**，只与 `OCUDU_UL_FRONTEND_FENCE=1` 同用才有意义；**§5.9.2 已证它没有收益**|
| **`OCUDU_UL_SLOT_TRACE=N`** | **新增**：每槽时间轴（**只记有 PUSCH 的槽**），给人眼看的，**不进门**|
| `OCUDU_DFT_OPEN_BLOCK=0` | DFT 的"每槽 14 个变换共用一条命令缓冲"关掉（默认开）。**D1 会碰它** |
| `wip/k1_barrier_slope.sh` / `wip/leg_report.sh` / `wip/run_leg.sh` | 腿对判读模板 / 单腿判读 / 跑腿（`sudo -E bash wip/run_leg.sh gpu <label> [OCUDU_*=…]`，**sudo 由用户执行**）|

---

## 7. 交付物与提交状态

**✅ 已全部提交**（`git log --oneline ddc09868e4..HEAD` 可看全；文档类的 3 个提交只改本文件/README 的指向，
**代码与设计文档全在第一个**）：
`87593ea2cc` phy+docs: K1 path A run - the answer is that K1 is at its floor, and it is a refutation
**tag**：本会话**没有新 tag**（没有可发布的阶段——路 A 的产出是否证性结论，不是新阶段）。
**⚠ 未推送**（本会话只提交到本地工作树，没有 `git push`）。

**该提交包含：**
```
doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md   §5.8.28（新增）
doc_chinese/phy_pipeline_gpu/wip/value_net.py                                --env / --env-passthrough
doc_chinese/phy_pipeline_gpu/README.md                                       入口指向本文件
doc_chinese/phy_pipeline_gpu/session_handoff_2026-09-20-7.md                 本文件
lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_metal_mmse_engine.mm
        mmse_inv_pipeline() 统一三处 K1 选择（修开放项 2）+ [inv_kernel] 横幅 + K1m 管道
lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.metal
        mmse_inv_memnone（探针）+ K1 文件头补记本会话的否证结论
```
**⇒ 新会话若发现这些还没提交，先提交再动别的**（提交信息建议写清"否证性结论 + 工具修复"）。

**tag**：本会话**没有新 tag**（没有可发布的阶段——路 A 的产出是否证性结论，不是新阶段）。

---

## 8. 未解 / 开放项（**按建议顺序**）

1. **★ 去掉宿主的每槽 DFT 等待**（§5.3）——D1 的最小形状，判据是 `[ul_dft_wait]` 归零。
2. **"把 DFT 编码进车道那条缓冲"（§5.9 ④ 路①）**：收益是同样的"1.83 提交/跳 → 0"，但要动
   **车道缓冲的打开时机**（循环依赖：缓冲由提取打开、提取要等网格、网格是 DFT 的产物）。
   **在路② 被否掉之后，路① 的收益只剩提交数**——做之前先确认它值得动架构。
3. **时间轴里 "~500 µs 等后半槽 / ~580 µs 其余" 的拆分是推算的**：前端 FFT 的 CPU 成本**从未单独计时**。
   要钉死需一次专门的 DFT 计时（§5.8.31 ⑩ ④）。
4. **`s25` 的时间轴只覆盖一次运行的 512 个槽**（约 0.5 s），不是全程分布。
5. **`merged` 下 `burst dispatches` 的 `channel_estimator=0` 是纯计数缺口**（`merged` 分支提前返回）。
6. **`merged` 的 +471 µs 里 ~340 µs 的排队与 64QAM 层 CRC 变差（`reTx` 93→179）机制上一致，
   但没有单变量隔离**。
7. **`shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉**（静默错址，9 个调用点）。
8. **§18.7 的 TA fence 实验仍未做**；TA 链 = 80.1 µs。
9. **§5.8.12 的"前端栅栏无差别"读数需要重读**：开关默认没武装 ⇒ 那次很可能是**假阴性**
   （两臂是同一个未武装配置，§5.9.1 ②）。
10. **K1 那条"每块列收尾 barrier"的 6% 理论移除**：已推理为安全但**未上腿**（§5.8.28 ⑥）。

---

## 9. 环境与流程（**与 S15 相同，逐条照旧**）

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
* **★ 改 `.metal` 后的正确姿势**（本会话踩过：`.air` 中间产物会**静默陈旧**，
  表现为"新 kernel 在 `strings` 里有、但 `metal-nm` 里没有 ⇒ 旋钮静默选不中"）：
  ```bash
  rm -f build/lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.air \
        build/lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.air.d \
        lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib
  cmake --build build --target ocudu_mmse_metallib -j 10
  xcrun metal-nm lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib | grep mmse_inv
  ```
  **验证 kernel 真的进了元库，再去信任何关于它的读数。**
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> [OCUDU_*=…]`
  ——**sudo 由用户执行**（把命令以纯文本贴给用户）；**Ctrl+C 停**；
  判读 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`。
* **★ 上腿之前关掉手机的 WiFi**；**判读前先看 `Real-time failures` 是否为 0**；**CRC 按调制分层比**。
* **用户裁定（长期有效）**：① `mode=gpu` 不允许宿主兜底；② dump 不算 CPU in the loop；
  ③ **控制面融合分阶段做、每阶段一次 OTA**（腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）；
  ④ **`merged` 保留为默认**；⑤ **顺序：先 K1 路 A，再 DFT/D1**（**路 A 已执行完 ⇒ 只剩 D1**）。
* **排序标准是"功能正确前提下 CPU 还剩几个参与点"，不是收益**——**K1 属于成本优化，按这条标准它是可选项**
  （本会话已把它记为"到头"）；**D1 才是剩下的目标性差距**。
* **★ 本会话新增的口径教训**：**离线绝对 µs 有两台仪器不可信**（§2-四 / §5.8.28 ⑤）——
  `last_gpu_wait_us()` 在单发下是队列跨度（对 8 倍工作量不敏感）；消融类探针的墙钟**跨时段不可复现**。
  **判 ≥40 µs 的效应只能靠空口腿对**（`ch_wt` 跨腿 ±8% ≈ 27 µs）。
