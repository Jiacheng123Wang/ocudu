# Session handoff —— `metal_kernel_fusion`（2026-10-07，第 1 次）

> 用法（沿用约定 ✓）：新会话**只读这一份**就能接着开工 ✓；
> 结构 = ① 本会话做了什么 ② **当前状态**（代码/开关/腿号/判据）③ **下一个会话的第一件事**（具体命令 ✓）
> ④ 未决问题与重开条件 ✓。
> 状态引用：架构文档 `metal_kernel_fusion_high_level_status_and_plan.md`（§0 一眼版 ✓）、
> 实施文档 `metal_kernel_fusion_design_and_implementation.md`（**§2.10–§2.15 与 memo 是本会话的全部细节 ✓**）。
>
> ★ **一条纪律先写在最前面** ✓（本会话踩了两次 ✓）：**飞之前先读 `gNB options` 与对应自报行** ✗——
> "**旋钮没进进程**" 与 "**旋钮没效果**" 在日志里长得一模一样 ✗（见 §③ 的两条教训 ✓）。

---

## ① 本会话（2026-10-07，第 1 次）做了什么

**一句话** ✓：**M1 结项 ✓✓（融合兑现收益并已翻默认开 ✓）、M3 完成 ✓（三个库合一个 ✓）、
AMC 覆盖率成立 ✓**；随后把 M0 留下的那 **~380–456 µs 信封**做了四轮归因，**四个假设全部被证否** ✗，
并查清**本机拿不到更细的 GPU 计时** ✗ ⇒ 停在"你选 (a) 接受 / (b) 纯经验法"这个决策点上 ✓。

### A. M1.1 接线 → 判决 → 结项（落实上一份 handoff 的"第一件事" ✓）

| 阶段 | 结论 | 腿 |
|---|---|---|
| **接线** ✓ | 融合路线**真的能切** ✓：dispatch/跳 **4.0000 → 3.0000** ✓、demapper 归 0 ✓ | 离线 + §2.10 |
| ★ **接线层不是引擎，是复合工厂** ✓ | 调用方拿的是 `channel_equalizer_metal_or_generic` ✓，它**只转发自己重新声明过的方法** ✗ ⇒ 谓词落基类默认 ⇒ **旋钮曾完全无效** ✗（实测 `y_fused=0` ✓）| §2.10 ✓ |
| ★ **两个"读出来"的结论** ✓ | ① 本树**默认 SINR 方法 `post_equalization` 在 host 上读 `nv`** ✓ ⇒ 融合 kernel **保留 nv 输出** ✓（不写 nv 会让路线在默认配置下永远跑不到 ✗）；② 上一会话 kernel 的 **strides 表长写错**（32 vs 14 ✗）⇒ 静默错 RE ✗，已修 ✓ | §2.10 ✓ |
| **离线证据** ✓ | 27 capture × 四 dump **逐字节一致** ✓（后来三个调制各 27 × 4 = **0/0/0/0** ✓）；单元测试 `[fused]` 回归 ✓ | §2.10/§2.15 ✓ |
| ★ **`mkf010/011` 判不了** ✗ | 接线在空口上被证明 ✓（**10 个融合 dispatch ⇔ 10 个 16QAM 跳** ✓、全 `crc=OK` ✓），**但腿跑的是 64QAM** ✗（PUSCH 用 **MCS 表 2**，`du_high_config.h:286` ✓ ⇒ 钉住的 "MCS 13" 在那里是 64QAM ✗）⇒ 融合只覆盖 **0.009 %** ✗ | memo ✓ |
| ★★ **`mkf012/013`（A 路：UL 钉成 16QAM）⇒ M1 判决：融合有价值** ✓✓ | dispatch/跳 **4.0000 → 3.0000** ✓、覆盖 **99.997 %** ✓、`busy` **478.9 → 469.6 µs** ✓、`ul_pipeline` **1280 → 1259** ✓、`ul_gpu_pipeline` **1237 → 1214** ✓、CRC **100.0 / 99.9 %** ✓、`pair_check` PASS ✓ | memo ✓ |
| ★★ **`mkf014/015`（B 路：**交付配置** 64QAM）⇒ 复现 ⇒ M1 结项** ✓✓ | dispatch 4→3 ✓、覆盖 **99.997 %** ✓（**16QAM 与 64QAM 两条分支都跑过** ✓）、`busy` **479.9 → 472.5** ✓、`ul_pipeline` **1320 → 1304** ✓、`ul_gpu_pipeline` **1242 → 1227** ✓、CRC **98.8 → 99.9 %** ✓、`stale=0` ✓✓ | memo ✓ |

**M1 的净结论** ✓：结构收敛达成 ✓（4→3 dispatch ✓、eq/nv 中间缓冲不再被写也不再被读 ✓）、
**功能零代价** ✓、性能一致改善（`busy` −1.5～−1.9 % ✓、长尾 −15～−28 % ✓），
**大小与 M0 预言一致** ✓（内核 < 1 % ⇒ 只能省 dispatch 与访存 ✓）。
✗ **没动到的**：M0 指认的那 ~380 µs 信封（CB 仍是 **2.00/跳** ✓）。

### B. 用户裁决：**融合路线翻默认开** ✓

`OCUDU_LANE_FUSE_EQDEMOD` 从"非空即开、默认关"改为 **"未设或非零 = 开，`=0` = 关"** ✓
（与 `OCUDU_EQ_DEFER_ENCODE` 同形状 ✓）。**M4 的"交付默认值不变"由此被取代** ✓（架构文档 §7 已标注 ✓）。
离线验证 ✓：默认在 16QAM/64QAM 语料上 `y_fused=1` ✓；`=0` 回 `y_batch=1 y_fused=0` ✓。

### C. M3：**3 个 metallib → 1 个** ✓

`lib/phy/metal/ocudu_lane.metallib`（**201 148 B** ✓），15 个 `.metal` ✓（CE 12 + EQ 2 + DEMOD 1 ✓）、
**34 个 kernel 入口点逐名核对全在** ✓、三个引擎各加载它 ✓（**加载 3 → 1** ✓）、
`IEEE_MATH_SOURCES` 那三个严格文件照旧 ✓。
★ **两处只有读代码才看得见的坑** ✓：① `ocudu_metal_burst.mm` 的 **ablation 臂自带 resolver** ✗
（只在 `OCUDU_LANE_ABLATE` 打开时走到 ⇒ 会**静默失效** ✗，已改 ✓）；
② 改库路径后**只重编 gnb 不够** ✗（三个引擎的单测二进制里烘着**已删掉的**旧路径 ⇒ 假失败 ✗，
**全量 `cmake --build` 后 5/5 PASS** ✓）。
★ **跑腿时又撞上第三处** ✓：`run_leg.sh` 的**起飞前闸门**还在按旧名字查那三个库 ✗
⇒ **每条 gpu 腿都被拒** ✗（`REFUSING … 3 device kernel(s) missing` ✓）—— 已改成
**"一个 stage 一个库"** ✓（`ocudu_dft.metallib` + `ocudu_lane.metallib` ✓）；同轮扫掉其它活引用 ✓
（`ab_replay_bins.sh` ✓、`build_macOS_note.md` ✓、`ce_kernel_cost.mm` ✓、设计文档里的补建命令 ✓；
**历史会话快照不动** ✓ —— 那是记录 ✓）。

### D. QPSK 补齐 + AMC 腿：**覆盖率问题回答得干干净净** ✓✓

- **QPSK 补进 kernel** ✓（`MOD_QPSK` ✓、★ **range limit 24 而不是 QAM 的 20** ✓）：
  三调制离线各 27 × 4 **0/0/0/0** ✓；★ **QPSK 那份用的是真正的原始语料** ✓（前两轮要"标注"只因当时只做 QAM ✓）。
- **`gnb_amc.yml`** ✓（钉住版去掉 `min/max_ue_mcs` ✓）⇒ `mkf016/017` ✓：
  ★ **账目精确闭合** ✓✓ —— `y_fused = 103 549` = **QPSK 8 378 + 16QAM 30 540 + 64QAM 64 631** ✓、
  `y_batch = 5 082` = **256QAM** ✓ ⇒ **覆盖率 95.32 %** ✓、**AMC 下逐跳路由按设计工作** ✓、
  dispatch/跳 **4.0000 → 3.0471** ✓。
- ✗ **但这一对判不了性能/功能** ✓：两腿**信道不同**（AMC 使然 ✓）⇒ 负载差 **~11 %** ✗；
  CRC **95.0 / 90.0 %** 都 DEGRADED ✗；`pair_check` B/hop **−27 %** ✗；对照腿一次 **59.5 ms 停顿** ✗。
  ⇒ **性能证据仍是钉住 MCS 的那两对** ✓（这也正是上一工作流钉住 MCS 的理由 ✓）。

### E. 那 ~380–456 µs 信封：**四轮归因，四个假设全部证否** ✗✗（本会话最重要的"否定知识" ✓）

| 步骤 | 做了什么 | 结论 |
|---|---|---|
| **归一化读数** ✓ | 给 lane 探针加 **`commit -> start`（队列）/ `start -> end`（设备）全样本序列** ✓ | 中位 CB：**队列 34 µs / 设备 209 µs** ✓（86 % 在设备 ✓）—— 我上一轮"主体是排队"的推测**被推翻** ✗ |
| **fence 臂** ✓ | `OCUDU_LANE_ABLATE_FENCE=1` ✓（只不编码 burst 那两道等待 ✓，响亮自报 ✓ + 计数 ✓）| 离线交替 3 轮：**−32 %** ✓；★ **空口 `mkf018/019`：什么都不动** ✗（`busy` 492.2 → 492.0 ✓、`merged_hop` 455.9 → 455.5 ✓、**CRC 都没塌** ✓ 99.5 → 99.6 %）⇒ **那道等待是保险，不是成本** ✓ |
| **并发** ✓ | `mkf022`（并发 1 ✓；`mkf020/021` 因我的命令错**未生效** ✓，却意外给出**噪声底** ✓）| 窗口只 **−10 %** ✗（不是争用主体 ✓）；★ 但 **p95/p99 −23～24 %** ✓ ⇒ **并发 1 是今天最强的长尾杠杆** ✓ |
| **dispatch 价钱** ✓ | `mkf023`（`OCUDU_CE_CORR_MERGED=1` ✓，`merged=157 299` ✓ 真的生效 ✓）| `ch_wt` 段 **36.3–37.3 → 30.4 µs** ✓ ⇒ **一个 dispatch 边界 ≈ 6.5 µs** ✓（**不是**仓里注释写的 39 µs ✗），**与融合自己的 −7.4/−7.6 µs 自洽** ✓✓ |
| ★ **平台限制** ✓ | 离线最小 `.mm` 查 `MTLCounterSampleBuffer` 可行性 ✓ | **M4 Pro：`atDispatchBoundary = NO`** ✗（只有 `atStageBoundary` ✓，而本 lane 一条 CB 一个 encoder ⇒ 拿不到新信息 ✗）⇒ **逐 dispatch 计时在本机不存在** ✗ |

**⇒ 信封现状** ✓：`merged_hop` ≈ **412 µs（并发 1）/ 456 µs（并发 2）** ✓，
能加起来的只有 **~7 dispatch × ~7 µs ≈ 50 µs** ✗ ⇒ **~400 µs 仍无归属** ✓。
**四条腿排除了四个解释** ✓ —— **下一个会话不必再试这四个** ✗。

### F. 顺手建立的两个"基础设施" ✓

- ★ **噪声底** ✓：**四条同配置腿**（mkf018/019/020/021）的窗口读数重复到 **±1 µs** ✓
  （`busy` mean 492.0–494.3 ✓、`merged_hop` 455.5–457.0 ✓、队列/设备 mean ±0.6 ✓）
  ⇒ 前面那些 −7～−13 µs 的判决是它的 **6～10 倍** ✓ ⇒ **可信** ✓。
- ★ **三条方法论教训**（都写进 memo ✓）：① **离线工具要交替比** ✗（先跑 3 基线再跑 3 臂，
  基线 285.9→160.6→112.6 单调下降 = 预热 ✗，会把顺序效应当成旋钮效应 ✓）；
  ② **计数少报会读成"旋钮没生效"** ✗（`fences skipped: 1` vs 实际 108 428 次 ✓，已修 ✓）；
  ③ ★ **"旋钮没进进程"与"没效果"在日志里一样** ✗ —— **先读 `gNB options` 与自报行** ✓。

**提交** ✓（17 个，`b6cae0c44d` → `6d2005b958` ✓）：接线 ✓ / M1 判决与结项 ✓ / 默认开 ✓ / M3 ✓ /
QPSK ✓ / AMC ✓ / fence 臂 ✓ / 并发 ✓ / corr 合并 ✓ / 平台限制 ✓ —— 全部已提交 ✓，工作树干净 ✓。

---

## ② 当前状态

* **HEAD** = **`6d2005b958`** ✓，工作树**干净** ✓，`build/hashes.h` 指纹 = HEAD ✓ **且已在二进制里** ✓
  （`run_leg.sh` 不会拒绝 ✓）。
* **代码** ✓：融合路线**默认开** ✓；**生效形状 = 1 层 + QPSK/16QAM/64QAM + 无 EVM** ✓
  （其余形状**自动**走两步路线 ✓ 并在日志点名 ✓）。
* **库** ✓：**一个** `lib/phy/metal/ocudu_lane.metallib`（201 148 B ✓，34 kernel ✓）；
  DFT 仍是自己的库 ✓。
* **腿** ✓：本会话飞到 **`mkf023`** ✓（共 14 条 ✓）；**下一个序号 = `mkf024`** ✓
  （`bash wip/next_leg_label.sh <后缀>` ✓）。
* **判据** ✓（M1，已达成 ✓）：结构 = dispatch/跳 4→3 ✓；主判据 = `busy` < 479.6 ✓（实测 469.6/472.5 ✓）、
  `ul_pipeline`/`ul_gpu_pipeline` < 1321/1242.8 ✓（1259/1304 ✓、1214/1227 ✓）；红线 = CRC 同档 ✓ + `gaps=0` ✓。
* **可用的开关**（都默认关 ✓，只影响被测行为/只加报告 ✓）：
  `OCUDU_LANE_FUSE_EQDEMOD`（**默认开** ✓，`=0` 回退 ✓）、
  `OCUDU_LANE_ABLATE_FENCE=1`（结构测量 ✗ 不是可用路线 ✓）、
  `OCUDU_CE_CORR_MERGED=1` ✓、`--expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=N` ✓
  （★ **三层路径** ✓，少一层会被 dry run 拒 ✓）。
* **三个配置文件** ✓（本工作流自己的 ✓）：`wip/gnb_mcs16qam.yml`（A 路 ✓）、
  `wip/gnb_amc.yml`（AMC ✓）、钉住版仍在 `../macos_thread_priority/wip/gnb_pinned_mcs13.yml` ✓（**不改它** ✓）。

---

## ③ ★ 下一个会话的第一件事：**等用户选 (a) 还是 (b)** ✓（本次会话停在决策点 ✓）

> **背景** ✓：信封的归因已经走到"**四个假设证否 + 本机没有更细的计时**" ✓（§①E ✓）。
> 继续之前**先问用户** ✗ —— 这不是技术未知 ✓，是**投入方向** ✓。

**(a) 接受它是本平台的稳定属性** ✓：记为已知（一跳 ≈412 µs @并发 1 / ≈456 µs @并发 2 ✓，
±1 µs 可重复 ✓），**不再追解释** ✗。M1/M3 都已结项 ✓ ⇒ 直接进 **M4 验收** ✓：
Linux 不变 ✓、probes-OFF 可编 ✓、一张"融合前后"性能对照表 ✓（数据已在 §①A 的 memo 里 ✓）。

**(b) 纯经验法** ✓（不再要求先解释 ✓，只做"结构变体 + 量窗口"对着 ±1 µs 基线比 ✓）：
候选（每个一条腿 ✓，`EXTRA_KNOBS` 走 env ✓、gNB 选项走 `--` ✓）：

```bash
cd /Users/jiachengwang/dev/ocudu
export LEG_CFG=$PWD/doc_chinese/macos_thread_priority/wip/gnb_pinned_mcs13.yml
export LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs

# 变体 1：把 lane 的 CB 拆成两条（量"跨 CB 边界"的价钱 —— 顺带验证 39 µs 那个数字的来源）
#   （需要一条新的探针臂，尚未实现 ✗）—— 先实现再飞 ✓
# 变体 2：CE 的 dispatch 数（现在 ~5.9/跳 ✓）—— OCUDU_CE_CORR_MERGED 已试（−6.5 µs ✓），
#   还有 OCUDU_CE_WEIGHTS_TILE=1（K1b threadgroup-memory 版 ✓）等已有旋钮
EXTRA_KNOBS="OCUDU_CE_WEIGHTS_TILE=1" sudo -E bash doc_chinese/macos_thread_priority/wip/fly_leg.sh mkf024-m1-wtile dual quiet gpu
# 变体 3：并发 1 的 −24 % 长尾是否值得留着 ⇒ 与吞吐做取舍（这不是 bug，是配置选择 ✓）
```

★ **无论走哪条，先读三样** ✓：`gNB options`（旋钮进了吗 ✓）、对应自报行（生效了吗 ✓）、
`[metal_stats] …sites`（**计数真的动了吗** ✓ —— mkf023 就是靠它证明旋钮生效的 ✓）。

**离线工具（判读前先看这些 ✓）**：
```bash
bash doc_chinese/macos_thread_priority/wip/ul_health.sh <leg>          # CRC/verdict
bash doc_chinese/macos_thread_priority/wip/dl_gate.sh   <leg>          # 参考项 ✓（STALL 是环境 ✓）
LEG_LOGDIR=$PWD/doc_chinese/metal_kernel_fusion/wip/logs \
  bash doc_chinese/macos_thread_priority/wip/pair_check.sh <legA> <legB>   # ★ 必须显式给 LEG_LOGDIR ✓
bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh "OCUDU_UL_REPLAY_NO_EVM=1 OCUDU_LANE_FUSE_EQDEMOD=0" \
     "OCUDU_UL_REPLAY_NO_EVM=1" --metal "/tmp/q64c/*.bin"                  # ★ 对照臂必须写 =0 ✓
```
★ **离线复现三份语料** ✓：`doc_chinese/work_tmp/corpus`（真 QPSK ✓）、`/tmp/q16c`、`/tmp/q64c`
（后两个是"同网格 + 标注成 16QAM/64QAM"的副本 ✓；**`/tmp` 会被清掉** ⇒ 重建：
`sed 's/^modulation=QPSK$/modulation=16QAM/' <corpus>/<name>.txt > …` ✓）。

---

## ④ 未决问题与重开条件

| 项 | 状态 | 重开条件 |
|---|---|---|
| ★ **信封 400 µs 的归属** | **四假设证否** ✗✗（内核 ✓ / fence ✓ / 争用 ✓ / dispatch 价 ✓）＋**本机无逐 dispatch 计时** ✗ | **用户选 (a) 或 (b)** ✓；若 (b) ⇒ 只做结构变体 ✓ |
| **M2（折 CE）** | **前提已被空口证否** ✗（那 ~456 µs **不是跨 CB 的等待** ✓）⇒ **不做** ✗ | 只有在"争用/串行化"被证实**且**折 CE 能改变它时才重开 ✓（§3 ✓） |
| **M4 验收** | 未开始 ✓（数据齐 ✓） | 用户选 (a) 时 ✓：Linux ✓ / probes-OFF ✓ / 性能对照表 ✓ |
| B 路唯一变差的项 | `ul_gpu_pipeline` **p99.9 +36 µs** ✗（记账 ✓） | 要引用 p99.9 之前先弄清是否与那次 2.3 ms busy 离群同源 ✓ |
| "**不要 nv 输出**"的 A/B（省 4 B/RE ✓）| 未做 ✗ | 融合收益已兑现 ⇒ 可做 ✓；它要**同时**换 SINR 方法（`--pusch_sinr_calc_method=channel_estimator` ✓）⇒ 两条腿一对 ✓ |
| **256QAM / 多层** | **不做** ✗（AMC 腿里 256QAM 占 4.7 % ⇒ 覆盖率天花板 95.3 % ✓）| 若某条腿的 256QAM 占比变得可观 ⇒ 按同一套加（4 张 16 项表 ✓）✓ |
| **AMC 下的性能判决** | **无** ✗（两腿信道不同 ⇒ 负载差 11 % ✗、CRC 都 DEGRADED ✗）| 需要一个**同样信道**的 AMC 对 ✗（OTA 难 ✓）⇒ 或继续用钉住 MCS ✓ |
| helper 抽取为共享头 ✓ | 未做 ✗（当前是**复制** ✓，且现有**三份**拷贝 ✓）| 融合被证明有价值（**已成立 ✓**）⇒ 可做 ✓，但它会改写交付形状里的两个 kernel ⇒ **需要自己的 A/B** ✓ |
| 离线工具收尾竞态 ✗ | **既有** ✗（`ul_chain_replay` 退出时偶发 `mutex lock failed` ✓，**对照臂也有** ✓，dump 已写完 ✓）| 若 `missing-dumps>0` ⇒ 单开一条线查 ✓ |
| `pair_check.sh` 不泛搜日志根 ✗ | **既有** ✗（只认 `LEG_LOGDIR` ✓）| 有空时与其它读者统一补泛搜 ✓（本工作流的命令已显式给 `LEG_LOGDIR` ✓）|
| **会话交接** | 本文件 ✓ | 下次开新会话时再写 `session_handoff_<日期>-<序号>.md` ✓（**同一条对话里不写** ✓）|
