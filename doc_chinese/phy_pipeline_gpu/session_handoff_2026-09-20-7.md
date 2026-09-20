# 交接（入口） — S16：**K1 路 A 走完了，结论是"K1 到头了"（否证性的，可复跑）；下一件事 = D1（DFT 进车道）**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **上一份 `session_handoff_2026-09-20-6.md` 不要读**（它描述的是 S15 开工时的状态：那时 K1 路 A 还没做，
> 目标两半已达成那部分仍然成立，但"接下来做 K1 路 A"这条已经执行完了）。
> **技术细节全在常驻设计文档 `gpu_phy_pipeline_design_and_implementation.md` 的 §5.8.15 – §5.8.28**
> ——**本会话新增的是 §5.8.28**，它必须读（它是 K1 的墓志铭）。

---

## 1. 一句话状态

**工作树 HEAD = `5a1f8b2f67`**（S15 的 `ddc09868e4` 之后本会话的 4 个提交，见 §7），
**`build/apps/gnb/gnb` 已按它重戳**——自查：**`grep build_info build/hashes.h` 的短哈希必须 == `git rev-parse --short=10 HEAD`**。
**之后再有任何提交，上腿前按 §9 重戳**（坑 35）。
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

## 2. ★★ 本会话做成了什么（一句话版）

**执行了 S15 定下的"K1 路 A"，结论是：K1 已经没有可买的余量了，而且这个结论是靠四个否证性测量得到的。**

| # | 结论 | 支撑读数 |
|---|---|---|
| **一** | **barrier 的【内存栅栏】不花钱**——所有成本都在"线程组级同步"本身 | `mem_threadgroup` 207.0 µs vs `mem_none` 207.1 µs（**1.000×**，交错 A/B，n=54） |
| **二** | **K1 已经是【1 barrier/pivot】的最优形状**：第一道 barrier 同时干"归一化"和"供消元"两件事 | 数一遍：`n=54,BLK=8` 共 **115** 道；三条"更少 barrier"的重写**全部算错**（见 §4） |
| **三** | **几何、线程数也到头**：1024 线程的 (64,16) 是最优，**线程少反而慢 2.4×** | 32×4（128 线程）是 (64,16) 的 **2.42×**；n=8 只要 26.6 µs ⇒ 瓶颈不是线程数 |
| **四** | **两台"离线绝对 µs"仪器被判不可信**（**这正是路 A 存在的理由**）| `last_gpu_wait_us()` 对 1/2/4/8 个 system **都读 206 µs**；消融探针首轮"大效应"几分钟后复跑消失 |

**⇒ 建议（待用户裁定）：把 K1 记为"已到头"，按 S15 已定的顺序转 D1（DFT 进车道）。**
理由：排序标准是"**功能正确前提下 CPU 还剩几个参与点**"，K1 属于**成本优化**（可选项），
D1 才是**剩下的目标性差距**（§9 用户裁定 ⑤）。

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

## 5. ★ 开工第一步：**D1 —— 把 DFT 搬进车道**（用户已定顺序，S15 裁定 ⑤）

### 5.1 为什么是它（不是收益，是排序标准）

* **排序标准 = "功能正确前提下 CPU 还剩几个参与点"**。目标两半（数据面 ✅ / 控制面 ✅）都已达成，
  **D1 是最后一个目标性差距**；
* 现状读数（§5.8.27 ⑤）：**`dft commits` ≈ 27668 ≈ 1/槽**——**每槽一次前端 CPU 提交**，
  **它还没搬进车道**；
* 设计文档原话：DFT 是 "**the stage that moves onto the lane's queue when the fused pipeline lands**"。

### 5.2 开工前必须先做的（否则会用错口径）

1. **先读 §5.8.27 ⑤ 的"计数器口径"**：`commit` 是**谁调用**的动作，每个引擎各记自己的账。
   `merged` 下 `mmse_ce commits=2`、`equalizer/demapper commits=0` 都**不是"没干活"**；
2. **D1 的判据是结构计数**（`dft commits` 每槽 → 每跳一次，或并入车道那次提交），
   **和 `cbs/lane`、lane `gap` 一起读**——**这不需要 GPU 计时**，所以不受 §2-四那些仪器问题影响；
3. `merged` 下 `burst dispatches` 的 `channel_estimator=0` 是**纯计数缺口**（§8 开放项 3，一行修），
   做 D1 时会碰同一段代码，**顺手修掉**。

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
| `OCUDU_CE_LANE_ORDER` | `merged`（**默认**）/ `event`（P2 回退，一行） |
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

1. **D1：DFT 还不在车道里**（每槽一次前端 CPU 提交；`dft commits` ≈1/槽）。**用户排的第一件事**（§5）。
2. **`merged` 下 `burst dispatches` 的 `channel_estimator=0` 是计数缺口**（`merged` 分支提前返回，
   没走到 `end_stage_async()` 里那次 `count_dispatch`）——**纯计数**，做 D1 时顺手修。
3. **`merged` 的 +471 µs 里，~340 µs 的排队与 64QAM 层 CRC 变差（`reTx` 93→179）机制上一致，
   但没有单变量隔离**（S15 开放项 5，仍未做）。
4. **`shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉**（静默错址，9 个调用点）。
5. **`end_stage(..., WEIGHTS_STAGE)` 在一跳里被调用 16 次**（P0-⑦ 旁证）——未查。
6. **§18.7 的 TA fence 实验仍未做**；TA 链 = **80.1 µs**（重排 116.9 里的大头）。
7. **K1 那条"每块列收尾 barrier"的 6% 理论移除**：**已推理为安全但未上腿**（低于一条腿对的分辨率
   ~27 µs）。**只有在 D1 做完、且用户想要极限压延迟时才值得回头做**。
8. `OCUDU_CE_INVERT_FIRST=1` **现在产出 NaN**（S15 已记，**仍禁用**）——它是 K1 的隔离测量工具，
   修好能把 K1 从 `ch_wt` 里单独拆出来。**K1 已"到头"，所以它的优先级降低了**。

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
