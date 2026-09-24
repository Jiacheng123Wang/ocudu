# Session handoff — 2026-09-25 #1

> 读这一份就能开工。**上一份（`session_handoff_2026-09-24-1.md`）的任务顺序（先 P0-5、再 P2-E）本会话都做了**：
> P0-5 **完成并提交**；P2-E **实现完成但被平台证伪**（`05_p2e_platform_finding.md`），**需要用户裁决**。
> 本 memo 的每条读数都来自本会话的离线运行，出处见 §4；**没飞的腿一律标"未验证"**。

## 0. 一句话现状

1. **P0-5 已交付**（提交 `747b9d475b`）：相位探针与车道探针**按 slot 配对**，
   `[ul_gpu_lane] paired with the phase segments (P0-5): samples=… of phase_samples=…` 一行把
   "配对样本数 / 相位样本数 / 为什么没配上"全打出来，并在**配对样本上**重算
   `busy/residency` 与 `eq_demap/residency`（`paired ratios` / `paired reading`）。离线全绿（§4）；
   **那条 n1 腿（`OCUDU_UL_PHASE_SEGMENTS=1`）还没飞**，`p0_gate.sh` 的新 **C2**（配对 n == 相位 n）等它。
2. **P2-E 机制已实现（默认关），但在这台机器上不生效**：macOS 26.6.2 / Apple Silicon
   **只在命令缓冲完成时**才发布"encoder 边界之后编码的 `MTLSharedEvent` 信号"，
   因此 `notifyListener` 无法在整跳结束前释放输入 tokens ⇒ **"不新增提交"与"提前释放"在本平台不能同时成立**。
   三种测法 + 对照见 `05_p2e_platform_finding.md` §2；需要用户在 §5 的选项里裁决。
   ⚠ **本会话没有飞任何腿**：P2-E 的反向臂**不要**照原计划去飞（机制不动持有期，飞了也判不出因果）。

## 1. 仓库/远端状态

* 分支 `apple-silicon`，本会话末次提交 **`747b9d475b`**（P0-5）+ 本次的文档提交，**已推送**（见 §6）。
* 工作区干净；`build/` 全量构建过（`cmake --build build -j 8`），`build/hashes.h` 已 `touch` 并对齐 HEAD。
* ⚠ 起腿前照旧：`touch build/hashes.h && cmake --build build --target gnb`（`run_leg.sh` 硬检查戳）。
* ⚠ `.metallib` 是构建产物、被 git 忽略；本机 8 个 metallib 的时间戳是 **9-24 14:51**（对 `value_net` 的陈旧基线很关键，见 §4.2）。

## 2. 目标与阶段划分（当前进度）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | P0-6 并发度、P0-1 分组 GPU 时间、**P0-5 探针配对** | ✅ **全部完成**（P0-5 的腿待飞）|
| P1 | 单变量臂（P1-8 并发度已跑）| 见 `01_plan.md` |
| **P2-E** | 输入缓冲寿命解耦 | ⛔ **机制被平台证伪**（`05_p2e_platform_finding.md`），**待用户裁决** |
| P2 其它 | P2-B、P2-A、P2-D（按测得持有期定池容量）、P2-F（并发度作为交付）| 未开工；P2-F 需用户二次裁决 |
| P3 | 验收 V1–V5 | 未开始 |

## 3. 验收判据（`03`/`04` 里先写死的，未改）

V1 中位 ≤2150 µs；V2 `starved_events == 0` 且 `held_max < pool`；V3 RF 失败 ≤10、`gaps == 0`；
V4 `cbs/lane ≤ 2.00 (max=2)`、`dropped == 0`（用户裁决：只约束交付）；V5 契约 8/8、crossings 0.00+0.00、CRC KO% 不劣化。
**本会话没有为过关改任何阈值**；P2-E 的选项 (b)（前端自成一次提交）会让 `cbs/lane` 2.00→3.00 ⇒ **必须用户先裁决**。

## 4. 本会话的关键读数（出处逐条）

### 4.1 P0-5（提交 `747b9d475b`，详见 `03_p0_instrumentation.md` 之外的 `05` §4）

* 判据（§6.0 写死）：① 配对 n == 相位 n；② 两个比值在配对样本上重算并打印；③ 零数据面 + 不动提交数。
* 离线：`ctest -L phy` **193/193**；新增单测例（`ul_pipeline_probe_test`，标签 `support`，**不动 193 的计数**）
  钉住 hook 契约（一次一通告、值等于序列值、未定稿不通告、通告数 == 记录数）；
  `l1_handover_arms` **5 PASS**、`l1_hop_arms` **4/4 differing=0**、`edge_block_arms` 默认 **6/6 绿** + 回退臂 **6/6 红**、
  `ab_dumps` arm2 **0 B**；`ul_chain_replay` 3 capture × 4 dump 与 **pristine HEAD 二进制逐字节相同**。
* 腿上的读法：`bash doc_chinese/phy_pipeline_gpu/wip/p0_gate.sh <腿>` → **C2**（新）+ `[INFO] P0-5: the paired ratios…`。

### 4.2 ⚠ 两条**改前就红**的网（本会话查清，**不是**本会话改动引起）

| 网 | 读数 | 结论 |
|---|---|---|
| `value_net.py`（47 capture）| **183 problems** | 用 **pristine HEAD 二进制**跑出**同一份**失败清单 ⇒ **归档基线陈旧**：基线是 9-20 05:26，而 CE 的 `.metal` 源在 9-20 10:27–21:41 改过、`.metallib` 在 9-24 14:51 重建 |
| `ab_dumps` arm1（`OCUDU_CE_EDGE_FUSE=0` vs 默认）| **15/27 capture、159068 B** | **pristine HEAD 二进制**在 `syn004_4` 上复现同样差值（h=2632, llr=924, ce=46）⇒ 改前就红；**该判据在 HEAD 是否曾经绿过，本会话没有查清**，登记为待办 |

⇒ 里程碑门若因这两条而红，**不是本会话的代码**；但也**不要**为了让它绿而改阈值（判据纪律）。

### 4.3 P2-E：平台证据（详见 `05_p2e_platform_finding.md`）

| 测法 | 读数 |
|---|---|
| 宿主轮询 `signaledValue`（信号在 encoder 边界之间，后面还有 ~100 ms 的活）| 只在**完成时**见值（97.1 ms vs 完成 97.4 ms）|
| `notifyListener` 投递时刻 | 105.8 ms vs 完成 105.9 ms |
| 另一条命令缓冲 `encodeWaitForEvent`（GPU 侧）| 77.1 ms vs A 完成 77.0 ms ⇒ 也没提前 |
| **对照**：信号编码在**第一个 encoder 之前** | **2.04 ms** 就可见（缓冲 81 ms）⇒ 发布是即时的 |

* 单测（每次运行都测）：`[dft-release] P2-E premise: … ONLY AT COMPLETION (… margin 0.3 ms)`。
* 引擎退出行：`[metal_stats] dft handover … tokens_early=signals:20,by_event:0..4,by_complete:36..40`；
  `by_event` 的少量非零是"通知在完成时刻与完成处理器抢 token"的竞态，**不是**提前发布。
* `by_event == 0` 时引擎自己打警告行：**池数字应读作未变**，不是"持有期不重要"。

## 5. 下一步（按优先级）

1. **等用户裁决 P2-E**（`05_p2e_platform_finding.md` §5 的四个选项）。我的建议：**保留开关（默认关）作仪器**，
   然后二选一：**接受 +1 提交把前端块做成独立提交**（V4 需重新裁决）**或**转 **P2-D**（P0-5 的配对分布正好是它的输入）。
2. **飞 P0-5 的那条腿**（n1 默认配方 + `OCUDU_UL_PHASE_SEGMENTS=1`）：
   `sudo -E OCUDU_UL_PHASE_SEGMENTS=1 bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>`
   （标准流量配方；起腿前 `pgrep -x gnb`、`lsof -nP -iUDP:2152`），腿后跑 `p0_gate.sh` 读 **C2**。
3. **两条陈旧网的处理**（§4.2）：`value_net` 需要重建归档基线（**要用户裁决**：重建=承认基线过期；
   不重建=该网在 HEAD 不可用），`ab_dumps` arm1 需要查清是"判据曾绿"还是"一直红"。
4. `s84b-p0` 第一次尝试没有日志（上一份 memo §9.5，**未验证**）——仍挂着。
5. P2-F（并发度作为交付）仍需用户二次裁决；§5.3 的 **5 秒收包停顿**仍未有解释（P2-E 没动持有期，所以它既没坐实也没排除）。

## 6. 文档地图（本会话新增/改动）

| 文件 | 内容 |
|---|---|
| **`doc_chinese/phy_latency/05_p2e_platform_finding.md`** | **P2-E 的实现 + 平台证伪 + 选项 + 复现方法**（本会话新写）|
| `doc_chinese/phy_latency/04_p2e_plan.md` | 顶部加了"实测结果"指向（原文保留）|
| `doc_chinese/phy_latency/00_status.md` | §7.2（P2-E 平台证伪与它对 §7.1 因果链的影响）|
| `doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` | §5.9.139（P0-5）、§5.9.140（P2-E 平台发现）|
| **`doc_chinese/phy_latency/session_handoff_2026-09-25-1.md`** | **本 memo** |
