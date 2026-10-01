# Session handoff —— 2026-10-01-1（macOS PHY 线程运行稳定性）

> **新会话先读这一份**，然后按需打开它引用的文件。
> 上游快照：`../phy_latency/session_handoff_2026-09-27-3.md`（以及同目录更早的）；
> 本线的**入口记录**：`../phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` 的 **§6.248**（ms 级停顿收口）与 **§6.249**（macOS 测量规划）。

---

## 1. 一句话状态

交付侧**绿**（全量审计 **33 PASS / 0 FAIL / 0 RED**：`p181-n78-default` + `p182-n78-stress`，两条都是当前 HEAD、都真加载、`leg_gate` 都 **10 of 10**）；
**新开的这条线**（macOS 线程运行稳定性）目前**只完成了规划**：目录/三份文档已建，**P0 还没做**，**P1 未开工**（动代码会让上面那对腿过期 ⇒ 需用户裁决）。

---

## 2. 这条线在做什么（30 秒版）

* 现象：gnb 常驻 **~200% CPU**（= **1.92 核平均**），**P 核 99.55%**；但**尾部尖峰出现在每一条插桩序列上**：
  `ce` max ≈ **20× 中位**、`ldpc` ≈ 8–15×、`[ul_time_frequency]` 1.4–3.4×、V1 ≈ 4×、`[ul_rx_wait]` ~2 ms。
* 疑点：macOS **没有硬实时**。我们标的 "RT" 实际只有 **QoS 类**一层可能生效；而 `taskinfo` 显示 **UI/IN 计费 = 0 s、有效 QoS 天花板 = `THREAD_QOS_LEGACY`** ⇒ **"请求的 QoS 有没有生效"目前没有读数**。
* 目标：让关键线程的稳定性**可测 / 可归因 / 可设置**，**同时保证 Linux（Ubuntu 台架）行为逐字不变**。

---

## 3. 已验证的事实（可直接引用；出处见高层 §2/§3）

### 3.1 线程地图（代码 + 真机栈一致）
| 线程 | 角色 | 优先级（本机数值）|
|---|---|---|
| `lower_phy_tx#0` | DL 链（停在 `uhd::…::send_packet_streamer::send`）| `max()` = **46** |
| `lower_phy_rx#0` | 收包（停在 `uhd::…::recv_packet_streamer::recv`；`[ul_rx_wait]` 量的就是它）| **45** |
| `lower_phy_ul#0` | 上行前端（**host 变换**：`puxch_processor_impl::process_symbol`）| **45** |
| `radio` | UHD 异步消息/TX 控制 | **45**（Linux 上是 `no_realtime()`）|
| **`main_pool#0..#4`** | **CE/EQ/demap 的编码 + `commit()`**（`ocudu_equalizer_metal_engine.mm` 的 `shared_burst::commit()`）、**LDPC**、DL 处理 | **44** |
| `ru_timing` / 支持面（io_broker、io_timer、SCTP×5、main、phy_worker）| — | 46 / `no_realtime()` |

* `max() = sched_get_priority_max(SCHED_FIFO) − 1`；本机 `…max(SCHED_FIFO)=47` ⇒ 46。
* 池 = **5**（`cells×(dl+ul)+cells+2`）；线程数 **18（loopback）/ 24（真电台）**。
* **CE/EQ/demap 的提交在池线程**，不是 UL/RX 线程；front-end 变换在 UL 线程（交付形态 = host 写网格 ⇒ **不提交给 GPU**）。

### 3.2 macOS 的四层机制，只有一层可能生效
1. **QoS 类**（`pthread_set_qos_class_self_np`）——**唯一可能生效**；悬案见上。
2. Mach affinity tag —— **Apple Silicon 不支持**（`KERN_NOT_SUPPORTED`）⇒ 空操作。
3. `pthread_setschedparam(SCHED_FIFO, 46)` —— 实测**无特权成功并读回**，但内核**只记录**。
4. **Mach 时间约束** —— 实现存在、**生产未用**（`bind_thread_to_performance_core()` 只在单测）；2026-09-01 自动施用与一次 **OAI-UE 随机接入回归**同时出现 ⇒ 回退为 opt-in。

### 3.3 进程画像（用户跑的 `taskinfo`，真电台腿 pid 85078）
`CPU 686 s / 357 s = 1.92 核`、`P-time 99.55% / E-time 0.45%`、`QoS: UI 0.000 IN 0.000 DF 94.78 UN 591.23`、`eff ceiling = THREAD_QOS_LEGACY`、`runningboard managed NO`、`csw 194 k/s`、`wakeups 153 k/s`、`mach msgs 420 k/s`。

### 3.4 ★★ 观察者会扰动被观察者（**本会话最重要的操作结论**）
用户在其腿（`p182-n78-stress_1001_1726`，23 min，戳 `c1b59d0aa5`）运行 6 分钟后跑了 `taskinfo` + `powermetrics` + `sample`：
该腿 `[ul_rx_wait] max` **153 ms**（未观察腿 2.2 ms）、`ce max` **13.3 ms**（1.2 ms）、`[dl_tx_slack] AT/BELOW 0` **709**（1）、
**`1 gaps` + `1 rx_overflows`**（全会话唯一），而 V1 **中位** 1262.4 **不动**。
⇒ **规则**：heavy 观察（`taskinfo`/`powermetrics`/`sample`）**只能另飞诊断腿**（标签带 `-diag`）并声明"尾部含观察代价"；
真实腿只许 `ps -M`。已落进 `wip/observe_threads.sh`（默认 light，`--heavy` 显式开启 + 警告）。

### 3.5 尖峰普查（p163 / p180 / p182）
见高层 §2.1 的表（`[ul_rx_wait]` 760/2049/2159 µs、`t2f` 1658/686/721、**`ce` 1256/1218/1228（≈20× 中位）**、`ldpc` 425/455/444、V1 4374/5033/4916、`[dl_tx_call]` **84498**/232/221）。

---

## 4. 目录与文件（本线）

```
doc_chinese/macos_thread_priority/
├── README.md                                            # 三类文档分工 + wip/logs 约定
├── thread_running_high_level_status_and_plan.md          # ★ 高层（活文档；先读这个）
├── thread_priority_optimization_design_and_implementation.md  # ★ 开发文档（追加式；P0–P4 细节）
├── session_handoff_2026-10-01-1.md                      # 本文件
├── wip/            # 脚本（**跟踪**）：observe_threads.sh、spike_census.py …
│   └── logs/       # 本线新腿（**git ignore**）
└── work_tmp/       # 采样/scratch（**git ignore**）
```
* 历史沿用：腿日志/脚本仍在 `../phy_pipeline_gpu/wip/`（门：`leg_gate.sh`、`milestone_audit.sh`、`run_leg.sh`、`ul_load.sh`…）；**本线新腿放本目录 `wip/logs/`**。
* `doc_chinese/.gitignore` 已覆盖 `**/logs/` 与 `**/work_tmp/` ⇒ 无需额外规则。

---

## 5. 已决定 / 待裁决

**已决定（用户 2026-10-01）**
1. ms 级停顿线（Q27）**收口存档**，写入 `../phy_latency/…` **§6.248**（现象/已确立/已排除/未知/重开条件/第一步）；
2. **开本目录**做"macOS 线程运行稳定性"这条新线；
3. **Ubuntu 台架可用**：`jwang@192.168.0.106:~/work/ocudu`（编译/运行/测试），用于验证"Linux 行为不变"。

**待裁决**
| # | 事项 | 建议 |
|---|---|---|
| 1 | **P1 是否现在动手**（动 `lib/` ⇒ `p181`/`p182` 过期，须重飞一对腿）| **先做 P0 + P2**（零代码），P1 与 P3/P4 合并成一次改动再飞 |
| 2 | `[sched]` 自读（env 门控）是否做 | 建议做 —— 它是回答"我们到底有没有 RT"的唯一读数 |
| 3 | Ubuntu 台架怎么用（我 ssh 跑 / 你跑后贴结果）| 需要你定；能 ssh 最省事 |
| 4 | P4（Mach time constraint）是否列入正式计划 | 建议列为**最后选项**（有 2026-09-01 的回归历史）|

---

## 6. 下一步（按顺序）

1. **P0（零代码，立即可做）**
   * 实现 `wip/observe_threads.sh`（腿开始/中段/结束各一次：`taskinfo` + `powermetrics` + `sample` + `ps -M`，产物进 `work_tmp/`）；
   * 实现 `wip/spike_census.py`（从新旧 logs 提取每序列 `max/p99`，输出"序列 × 腿"表，标出 `max/median` 最大的前 N 条）；
   * 由分布导出**阈值候选**并写入高层 §5.2（**先写规则，再看数**）。
2. **P2（零代码）**：`taskpolicy -l/-t` 档位扫描 + 一对待测档的 A/B（判据 = 尾部率是否跟档位走）。
3. **P1（动代码，需裁决）**：`[sched]` 自读 + 线程级快照 + 事件加"线程名/线程 CPU" + `ivcsw` 归一化；**一次改动合并** P3/P4 的可选项，再飞一对腿（default + stress）。
4. **Ubuntu 复核**：同一 commit 在台架上编译 + 跑单测，结果写进开发文档 §10（Linux 复核）。

---

## 7. 命令速查

### 7.1 飞一条腿（**只能用户飞**：`sudo` + B210 + 手机）
```bash
# n78 20MHz / 30kHz：default 腿（不跑负载器）
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml OCUDU_METAL_GPU_TIME=1 OCUDU_UL_PHASE_SEGMENTS=1 \
  sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> --regime=default
# stress 腿（要真加载；先花 60 秒判载荷：TBS 中位 ≳1.5 kB、≥1000 B ≳75%）
... OCUDU_UL_TIMING_EVENTS=16 sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> --regime=stress
```
⚠ **手机移动数据开关必须先打开**（`../phy_latency` 纪律 63：三次失败都栽在这里）。

### 7.2 判一条腿
```bash
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 <label>          # 空口判据（30kHz ⇒ 0.5 ms/slot）
bash doc_chinese/phy_pipeline_gpu/wip/ul_load.sh --slot-us=500 <label>           # 载荷（资格）
bash doc_chinese/phy_pipeline_gpu/wip/milestone_audit.sh --leg=<default> --stress-leg=<stress>   # 全量审计（不加 --quick）
```

### 7.3 旁观线程（本线 P0）
```bash
# 真实腿（default/stress）上只许零侵入的：
bash doc_chinese/macos_thread_priority/wip/observe_threads.sh <label> mid          # 默认 light：ps -M
# heavy 观察（会扰动！）只能另飞一条诊断腿，并用 --heavy 显式开启：
bash doc_chinese/macos_thread_priority/wip/observe_threads.sh <label>-diag mid --heavy
#   = sudo taskinfo <pid> + sudo powermetrics --samplers tasks --show-process-qos-tiers \
#     --show-process-wait-times --show-process-amp -n 3 + sample <pid> 3 -file …
```
⚠ 见 §3.4：heavy 观察在一条真实腿上产生过 **153 ms** 的停顿与**一次丢样**。

### 7.4 仪器读数
```bash
grep -a -A20 '^\[ul_timing_events\]' doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_<label>*.log.stderr
grep -a 'Real-time failure in RF'     doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_<label>*.log | head -40
```

### 7.5 提交与戳（**重要**）
```bash
# 只改文档：可以直接提交（不必重建/核对三段戳）
git add -A doc_chinese && git commit -m "doc: …"
# 改了 lib/include/apps/tests（代码）：提交后必须重打戳，否则不能飞腿
touch build/hashes.h && cmake --build build --target ocudu_versioning && cmake --build build --target gnb
```
* 审计规则：**腿的提交必须等于 HEAD，或差分不碰代码**；任何 `lib/include/apps/tests` 改动 ⇒ 旧腿**立即失效**。
* `build/hashes.h` 是构建产物；提交前用 `git status` 确认没有把 `build/` 或用户自己的配置带进去。

### 7.6 Ubuntu 复核
```bash
ssh jwang@192.168.0.106 'cd ~/work/ocudu && git fetch && git checkout <commit> && \
  cmake --build build --target ul_pipeline_probe_test macos_compat_test && ctest --test-dir build -R "probe|compat"'
```

---

## 8. 纪律与坑（本线必须守）

1. **旧 memo 不改**（`../phy_latency/session_handoff_*.md` 等）——要记的写开发文档；
2. **两把钥匙**：新仪器 = 编译期开关 + env，**默认关**；关着时报告**逐字节不变**；
3. **反向臂**：每个新字段/新读数都要有能把它**打红**的反向臂（`../phy_latency` 纪律 79）；
4. **判据先登记再飞**（§5.2 第 2 条）；**不追溯**已判的腿；
5. **Linux 逐字不变**：`#if defined(__APPLE__)` 之外零行为改动，`#else` 只写 no-op（本线第 1 不变量）；
6. **日志是 root 的**：`wip/logs/` 里的文件 `sudo` 才能改；写新腿用 `run_leg.sh`（它是 root 跑的）；
7. **腿的资格先于读数**：`ul_load.sh` 的载荷（stress 腿）与 `leg_gate` 的判据（default 腿）；
8. **上报"停顿"必须同时给**：`cpu`/`win`/`ivcsw`/`base_age` + **同秒的 `[RF]` 行**（否则分不清"派生量"与"独立故障"，`../phy_latency` §6.242④）；
9. **`[ul_rx_wait]` 口径跨 6.215 不可比**（整槽→逐符号，纪律 77）；
10. **审计档位**要一起说（`--quick` 不跑四条长臂，纪律 72）。

---

## 9. 当前仓库状态（交接时刻）

| 项 | 值 |
|---|---|
| HEAD | `10f5885dbd`（`doc: plan for measuring PHY-thread timing stability on macOS …`）+ 本目录的提交 |
| 三段戳 | 重打后 = HEAD（若本目录提交后再飞腿，先 `touch build/hashes.h && …` 重打）|
| 工作树 | 干净（用户自己的 `configs/*.yml` 不提交）|
| 全量审计 | **GREEN**（`p181-n78-default` + `p182-n78-stress`）—— ⚠ **任何 `lib/` 改动都会让它失效** |
| 测试 | `ctest -L phy -j 1` = **205/205**（MMSE 那条是登记的偶发 SIGBUS，重跑即过）|
| 关键仪器 | `OCUDU_UL_TIMING_EVENTS`（两把钥匙、6 个反向臂、`win/ivcsw/nvcsw/base_age` 四列已修）|
