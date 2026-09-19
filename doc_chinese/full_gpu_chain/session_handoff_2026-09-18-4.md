# Session handoff —— 2026-09-18（S-7g-20/21：K0-a 上机验证 + gap 拆解完成 + 秒级 ping 尖峰根因消除）

> **这是给"手动 compact"用的备忘**，替代 `session_handoff_2026-09-18-3.md` 作续接入口。
> 细节仍以设计文档 **§48.146(a) 状态索引** 与 **§48.192–§48.194** 为准。
> 上一份：`session_handoff_2026-09-18-2.md`（S-7g-19：Step 1′ 三腿判读 + K0-a 侦察）。

---

## 0. 一句话现状

**K0-a 已上机验证、默认开、三张网+七门+Ubuntu 全绿；估计器的两条 CB 已分开计量（`ch_est`/`ch_wt`），
lane gap 也已拆成两个逐位闭合的设备空洞。但 gap 的归属在本 session 里被更正了两次，现行结论是
**"~105 µs 的队列等待 ×2"**（宿主每跳只花 ~18 µs），嫌疑指向**前端那条 423 µs 的整块 DFT CB**；
Step 1（CFO 搬上设备）**按原设计不成立、已回退**（核只在 ≥2 DMRS 符号时写那个标量）。
⇒ 回来第一件事是**直接量 Q1**（见 §5.1）。**
另：本 session 后半段顺带查清并**消除了困扰 OTA 的秒级 ping/iperf 尖峰**（MAC 层 RLF 阈值），
并把它固化进 testbed 配置。

---

## 1. 代码状态（回来第一件事：核对这一节）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `apple-silicon` / **`832832c442`**（已 push；Ubuntu 164/164）|
| **工作树** | **干净**（`git status --short lib/` 为空）|
| 未跟踪/不入库 | `configs/*.yml`（**用户本地，已含本 session 的 RLF 改动**）、`doc_chinese/`（gitignored）、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh` |
| 参考二进制 | `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref` **＋并排 4 个 `.metallib`** |
| 语料 | `doc_chinese/work_tmp/corpus/`（27 个合成捕获）|
| 上机日志 | `doc_chinese/work_tmp/logs/`（每腿两份：`*.log`(ocudulog) 与 `*.log.stderr`(计数/契约/`[ul_gpu_lane]`)）|
| Ubuntu | `jwang@192.168.100.131:~/work/ocudu`，**已更新到 `2eca4a7965`，构建 RC=0、`ctest -L phy` 164/164** |
| 本机二进制 | `build/apps/gnb/gnb`。**注意**：`build/hashes.h` 是 configure 期生成，重打 stamp 后要 `touch build/hashes.h && cmake --build` 才会真正重链 |

**本 session 的提交链（最新在前）**

```
08299c55a7  mac: the RLF thresholds are logged at startup, because a leg has to be able to read them back
2c5284f61d  mmse lane probe: split the lane's GPU gap into the host, the queue and the dependency
d49f3a16fd  mmse ce: the correlation stage loads A's diagonal from the ratio the extraction computed
74ba4c5251  （本 session 起点）mmse stats: the host-wait lane order collected its command buffer twice
```

---

## 2. 本 session 做完的事

### 2.1 K0-a：`sigma2_rel` 的商搬上设备（`d49f3a16fd`，默认开）

**要点（§48.194）**
- 提取的 CB 现在写 `out[2]`（商）与 `out[3]`（均值）；A 核经 `corr_stage::sigma2_dev` +
  `sigma2_slot` 读 `scalars[p.sigma2_slot]`，**宿主不再把商传进核**。
- **真凶是 fast-math，不是硬件**：默认 fast math 下 2²⁰ 对随机样本有 **28.4%** 的商与宿主不同
  （翻 LLR 判决 32613 位、27/27 捕获）；`-fno-fast-math` 下 **0/2²⁰**。
- 因此把有契约的那个核**拆成独立文件** `ocudu_mmse_pilots_power.metal`（否则整个 pilots 文件严格化会
  改动 LSE 的最后一位，27/27 捕获都动）。
- 新门 **`ratdev`**（设备商 vs 宿主商必须逐字节相同），已进 `run_gates.sh`（**六门 → 七门**）。
- 顺带修掉两个真缺陷：`wrap` 长度 2→4 floats 的越界写；宿主/核**字段名不一致**（`static_assert(sizeof)`
  看不见）→ 现在两侧都有 `offsetof` 断言。

### 2.2 lane gap 拆解（`2c5284f61d` + 本轮）

`[ul_gpu_lane]` 新增两条序列：

```
gap: stage entry -> extraction commit (host)      ← 宿主（~10 µs）
gap: commit -> first command buffer starts (queue) ← 队列（恒 0.0）
gap: stage entry -> extraction commit (host) 之外的部分 = 依赖串行化
```

四条腿实测：residency 705–744、busy 505–523、**gap 193–239**、宿主 9.9–13.8、**queue 0.0**。
⇒ **lane 内已无宿主/队列开销可削；95% 是数据依赖。**

### 2.3 ⛔ 两个被实测否决的计划（**不要再按原样做**）

| 计划 | 否决依据 |
|---|---|
| §48.193(d) **`hop_stage` 接手** | ⚠ **理由已被更正，见 §2.6**：当初以"宿主读标量实测 0.1 µs"否掉——读本身确实 0.1 µs，但**它强制的那次同步值 125 µs/lane**。**这块现在是第一杠杆**，不是"不要做"。 |
| **Step 4：lane 并发 `max_in_flight` 1→N** | 否决依据里的"后端 GPU 占用率仅 0.9%"**算错了**（漏掉前端）。本腿重算 ≈ **11%**（前端 5.43 s + 后端 1.05 s / 59 s）。**结论仍成立**（确实没有队列要缓解），但数字不能再引用。 |

**顺带更正**：§48.193(c) 的"宿主不再等提取的 ~94 µs"从来没有被测到过这个量级。

### 2.4 秒级 ping/iperf 尖峰：根因 + 消除（**支线，已完成**）

**用户观察**：ping 起始 RTT 1.5–4.4 s、线性衰减；iperf3 出现"某秒传输 0 + 数百重传"。

**完整因果链（日志逐条支撑）**

```
① gNB 的 MAC 判 RLF 并启动 4 s 释放定时器
   "ue=0: RLF detected. Cause: 100 consecutive undecoded CSIs"
   （rlf_detector.h：UE 的 CSI PUCCH 解不出就 +1，一旦解出清零；== 阈值就报 RLF）
② UE 失去连接 → PRACH → tc-rnti → Msg3（多次失败：5 次授权全 KO、无 PUCCH、gNB 放弃）
③ 重接入耗掉几秒 = 尖峰大小；④ 接入成功后排队包排空 = 线性衰减
```

**量化**：CSI PUCCH 到达 **≈49 Hz** ⇒ 阈值 100 ≈ **2.0 s** 的坏运气；阈值 300 ≈ **6.1 s**。
本测试台 F2/CSI 的 SINR 中位数仅 **−3.9 dB**，2 s 窗口太易被打穿。

**修复与结果**（`cell_cfg.pucch.max_consecutive_kos: 100 → 300`）

| 量 | 基线 | 修复后 |
|---|---|---|
| `RLF detected` / `UE Context Release` | 11 / 10 | **0 / 0** |
| 最长单条连接 | 6–46 s | **168 s** |

**已写进 testbed 配置** `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`（该文件本地、不入库）：
`cell_cfg.pucch` 现在**显式带全 5 个 narrow-BW 资源值** + `max_consecutive_kos: 300`
（资源值照抄上游 `configs/cell_cfg_pucch_narrow_bw.yml`）⇒ **以后跑腿无需任何命令行参数**。

**为什么必须带全 5 个**（上游 `83dcb80860` 的既定设计，不是缺陷）：5 MHz 的 PUCCH 缩量以
`pucch_cfg == du_high_unit_pucch_config{}`（"用户没动过 PUCCH"哨兵，**逐字段全等**）为前提；
而 `max_consecutive_kos` / `sinr_threshold_dB` 这两个 **MAC/RLF 参数住在 PUCCH 资源结构体里**，
设它们就让哨兵为假 ⇒ 缩量不生效 ⇒ 撞 50% PRB 校验被拒。上游为此提供了那个示例配置文件。

**⚠ `sinr_threshold` 是无效参数**（已去掉）：它的作用是"低于阈值的 PUCCH 记为**未解出**"
（`uci_indication_selector.cpp:70-78`），**只会让计数涨得更快**，不是保护。
**起作用的只有 `max_consecutive_kos`。**

**残余 100 ms 级抖动**：与连接无关（那些间隔内 `PRACH=0`、`RLF=0`），用户判断可能来自 CN/回程 ⇒ **暂不动**。

### 2.5 最后的观测（**未提交**）：把 `ch_est` 拆成提取 / 权重两条 CB

原来 `gpu_lane_probe` 把估计器**两条命令缓冲都标成 `ch_est`**，所以只能知道"ch_est 占 85% of busy"，
**无法回答那 95% 的依赖在等哪一条**。改动：

| 文件 | 内容 |
|---|---|
| `ocudu_metal_lane_probe.h` | `stage` 加 `channel_estimator_weights`（放在 `count` 之前，索引不变）|
| `ocudu_metal_lane_probe.mm` | `stage_name()` 加 `"ch_wt"` |
| `ocudu_metal_mmse_engine.mm` | `end_stage`/`end_stage_async` 增加 stage 参数（默认 `channel_estimator`）；权重路径 4 个出口传 `WEIGHTS_STAGE` |

**上机结果（`gnb_wt_split_0918_1521`）**

```
busy split: ch_est=113.4us/lane (22%, cbs=1)  ch_wt=325.7us/lane (64%, cbs=1)  eq_demap=73.7us/lane (14%, cbs=1)
lanes=2807  residency 705.6  busy 512.8  gap 192.8  host 9.8  queue 0.0
```
（离线同向：100.6 / 390.5 / 88.8）

⇒ **权重 CB 占 64–67% 核时间，而 burst 的均衡正等它。**

**权重 CB 内部顺序**：`encode_corr`(corr_a→inv/K1→corr_r_hp) → K2 apply(h) → weights(W) →
`pilots_scatter` → **`encode_reformat`（K3 均衡器估计 + K4 噪声方差）**。
burst 真正需要的是**这条 CB 的末尾**（`gpu_h`/`gpu_ce`/`gpu_nv` 由 reformat 产出）。

**判据已过**：三张网 `AB_ALL_RC=0`、七门 `ALL_GATES_RC=0`、Metal 自测 6/6、单测 13/13。

**已提交**：`d93e004cfd`（三张网 + 序 A/B + 七门 + Metal 自测 + `ctest -L phy` + 两条配置构建门 **全绿**；
Ubuntu `BUILD_RC=0` / `ctest -L phy` **164/164**）。

### 2.6 **gap 的真正划分（本 session 最后一段）：先说结论——是"队列"，不是"宿主"**

> **⚠ 这一节中途被更正过一次，两版都记在这里。以 §2.6b 为准。**

**2.6a（已被更正，保留以示推理陷阱）**：曾用 `hole1(125.3) ≈ 238.3 − 116.6 = 121.7` 断定
"空洞①的 62% 是宿主造成的"。**那个等式不含"提取 CB 自己在队列里等了多久"**，什么也证明不了。

**2.6b（现行结论，已由上机腿实测）**：修好那条恒等式后（`e1bd5dbd70`），腿
`gnb_q1probe_0918_1640`（契约 7/7、`0/59521` 失败、`device_sigma2=953=lanes`、零旋钮）直接量到：

| 量 | mean | median |
|---|---|---|
| residency / busy / **gap** | 716.4 / 505.2 / **211.2** | 686.7 / 510.1 / 175.5 |
| `ch_est` / `ch_wt` / `eq_demap` 跨度 | 94.2 / 335.5 / 75.5 | |
| **Q1 提取 commit→start** | **58.6** | 44.4 |
| **Q2 权重 commit→start** | **64.0** | 44.2 |
| **Q3 burst commit→start** | **374.5** | 355.6 |
| 空洞① / 空洞② | 129.1 / 82.2 | 106.3 / 67.4 |
| 宿主：提取→权重 commit | 217.8 | 198.5 |
| 宿主：权重→burst commit | 107.1 | 101.2 |
| `[mmse_time_sum] total` | **19.8** | |

**时间线三条独立路径闭合到 0.1 µs**（residency 算 716.5 / 测 716.4；Q2 算 64.1 / 测 64.0；
Q3 算 374.7 / 测 374.5）⇒ 口径可信。**判读时必须看这个闭合**，否则又会回到"拿巧合相等当因果"。

**⇒ 归属（终于是有判据的）**

- **空洞① 129.1 = 宿主侧 65.0 + 队列 64.1。** 65.0 = `C2 − E1`，而宿主实际工作只有 **19.8 µs**
  ⇒ **约 45 µs 是 `waitUntilCompleted` 从 GPU 结束到真正返回的唤醒延迟**（它落在 `t_begin` 之前，
  那支探针看不见）。**早先写的"宿主 ≤18 µs"低估了**——方向对，份额错了。
- **空洞② 82.2 全是队列**：burst 比权重 CB 结束**早 292.5 µs** 就已提交，从来不是交棒慢；
  fence 之后又等了 82 µs 才被 GPU 捡起。
- **⇒ gap 211.2 = 宿主/唤醒 65.0（31%）+ 队列 146.3（69%）。**

**仍然成立的部分**（2.6a 里没被推翻的）：`handover_us` 与 gap 窗口不重叠、"queue" 那条是恒等式、
两个设备空洞逐位闭合到 gap 203.3、候选①（拆 reformat）被代码否掉。

**为什么原来的划分不算数**（两条独立原因，详见 §48.194(g-vicies)）：

1. `handover_us`（宿主段）**以提取 CB 的 commit 结束**，而 `residency`/`gap` **从该 CB 的 `GPUStartTime` 开始**
   ⇒ 宿主段整个在 gap 窗口**之外**，占 gap 的 **0%**，不是 5%。
2. "queue" 那条的定义让它**恒等于 0**（提取 CB 就是 lane 的第一条）；1978 样本 **min = max = 0.0**。

**新观测**（只动 `ocudu_metal_lane_probe.mm`：两个设备空洞 + 两个宿主 commit 间隔）——
腿 `gnb_holeprobe_0918_1545`（契约 7/7、`0/62758` 失败、`device_sigma2=1978=lanes`、默认 `event`、零旋钮）：

| 量 | mean | median |
|---|---|---|
| residency / busy / **gap** | 735.2 / 531.8 / **203.3** | 711.5 / 543.7 / **171.2** |
| 空洞① 提取结束 → 权重开始 | **125.3** | 104.8 |
| 空洞② 权重结束 → burst 开始 | **78.0** | 63.6 |
| 宿主：提取 commit → 权重 commit | **238.3** | 227.8 |
| 宿主：权重 commit → burst commit | **94.6** | 86.5 |

**125.3 + 78.0 = 203.3 = gap，逐位闭合。**

- **①是队列**：0.5× 见上表（**不是**"宿主交棒慢"——宿主那一段只有 ≤18.3 µs）。
- **②也是队列**：burst 在权重 CB 提交后仅 94.6 µs 就提交，而权重 CB 自己要跑 340.6 µs
  ⇒ burst 早就在队列里等 fence，fence 之后又等了 ~78 µs 才被 GPU 捡起。

**代码链（宿主为什么"必须"等提取——这条仍然成立，是 Step 2 的动因）**：`gpu_ls_cfo[0]`（设备标量）
→ 宿主在 `waitUntilCompleted` 之后读（`port_channel_estimator_metal_mmse_impl.cpp:1075`）
→ `args.cfo_hop` → `reformat.noise.cfo`（:1596）→ **编进权重 CB**
⇒ 提取跑完之前**权重 CB 根本无法开始编码**。宿主早交棒 ≈220 µs，本可让 Q2 与提取的 GPU 时间重叠。

### 2.6c **Step 1（把 CFO 搬到设备）按原设计不成立，已回退（未提交）**

判据当场否掉（同一轮门里两次、且是间歇性的）：有界网 `--metal-cpu-demod` 那一轮 **25/27**，
`syn025_25` 差 **9483 字节、LLR 翻转 207**；序 A/B **`syn015_10/event` 差 4135 字节**
（参考跑与显式 `event` 跑本该逐字节相同）。**严格网 27/27 干净**——因为它走 `OCUDU_CE_CPU_LS=1`、
**根本不进设备路**，这恰恰是旁证。

**根因在核里，不是时序运气**：`ocudu_mmse_pilots.metal` 的 `mmse_pilots_cfo` 开头
`if ((p.nof_dmrs_symb < 2) || (tid != 0)) { return; }` ⇒ **只有 ≥2 个 DM-RS 符号的跳才写 `gpu_ls_cfo[0]`**。
原设计里宿主在提取完成的瞬间把该值**快照**进 `args.cfo_hop` 当**参数**传下去；改成设备稍后再读，
读的是**那个缓冲区的当时内容**，可能属于更早的跳。**两者不等价**，而 K4 在权重 CB **末尾**读
（距今约 460 µs），窗口比 `corr_a`（同 CB 开头）大一个数量级，所以是它先炸。

**⚠ 连带结论**：`gpu_ls_sigma2` 的固定槽位有**同样的潜在危险**（跨 CB 读、无轮转），
只是 K0-a 的读在权重 CB 开头 + 写入无条件，才没暴露。**Step 2 必须先把槽位轮转做出来**，
否则就是把上面这个不等价放大到全链路。

---

## 3. 本 session 的门（全绿）

| 门 | 结果 |
|---|---|
| 三张网 `run_ab_all.sh <ref> 27` | 严格 27/27、有界 26/27（最差 8 字节、**flips 0**）、CPU 27/27 |
| 序 A/B `ab_fused_lane.sh 27` | 4 路线 × 27 × 3 序逐字节 |
| **七门** `run_gates.sh` | `ALL_GATES_RC=0`（含新门 `ratdev`）|
| Metal 自测 / CE 单测 | **6/6** / **13/13** |
| 两条配置构建门 | `/tmp/build_nometal`、`/tmp/build_nostats` 各 RC=0 |
| **Ubuntu** | `08299c55a7` 与 **`d93e004cfd`** 各：构建 RC=0、**`ctest -L phy` 164/164** |
| 上机干净腿 | `gnb_clean_0918_0918_1507`：契约 7/7、失败 0/71287、`device_sigma2=2156=lanes`、RLF=0 |
| **本 session 新腿** | `gnb_wt_split_0918_1521`（busy 拆分）、`gnb_holeprobe_0918_1545`（空洞观测）、`gnb_q1probe_0918_1640`（Q1/Q2/Q3）、**`gnb_fence_0918_1805`**（三个提交的上机验证：**fence 每 lane 2 次**、契约 7/7、`0/76779`、时间线仍闭合到 0.1 µs）|

---

## 4. 硬约束与纪律（**本 session 新增的尤其重要**）

1. **同一文件的多处结构性改动必须一次做完并带断言**——分轮叠加替换把 `ocudu_metal_mmse_engine.mm`
   改坏过一次（重复参数行 + 非法前置声明），不得不 `git checkout` 重做。教训见 §2.5。
2. **C++ 不允许默认实参出现在定义之后**——要么把默认值放在定义上，要么手动传参。
3. **"旋钮没效果" ≠ "旋钮没送到"**：本 session 三次混淆。现在有防线——
   `rlf_detector.h` 启动时打印实际生效的阈值；`run_air_leg.sh` 把 `--*` 送进 **argv**、`OCUDU_*=` 送进环境、
   其余**报错退出**（旧版把一切含 `=` 的都 export ⇒ CLI 参数被吞）。
4. **验证配置改动要用"真跑路径"**：`--dryrun` **静默忽略未知选项**，只有真跑才报
   `arguments were not expected`（无 SDR 时走到 `CU-CP failed to connect to AMF` 即代表校验已过）。
5. **选项名以 `--help` 为准**，不要从结构体字段名推（本 session 因此写错三个名字）。
6. **同一台机器上不许同时跑两套 GPU 门**（并发会造成假不匹配，串行重跑即绿）。
7. **跑腿前先 `ps aux | grep [g]nb`** 确认无遗留进程（遗留进程会让失败率超预算、该腿作废）。
8. **门必须在 metallib 构建结束之后跑**。
9. **⚠ 改完 `lib/`（或 `git checkout` 回退之后），`cmake --build build` 不够**——本 session 因此白查了一轮：
   `all` **不含** `ul_chain_replay` 与那几个 Metal 自测，它们会停在旧版本，而 `.metal` 已被重新编译
   ⇒ **测试二进制里的宿主结构体与它加载的 metallib 不再匹配**。症状极具误导性：`sinr=inf`、
   `--metal`/`--metal-cpu-ldpc` **全 27 个捕获 0/27**（看起来像灾难性回归），而 `--metal-cpu-demod` 却 26/27 正常。
   **正解**：显式 `--target ul_chain_replay port_channel_estimator_metal_mmse_unit_test
   dft_processor_metal_unit_test ofdm_demodulator_metal_batch_test demodulation_mapper_metal_unit_test
   channel_equalizer_metal_unit_test baseband_gateway_buffer_metal_smoke_test`。
   **`run_leg_gates.sh` 已把它做成 STEP 0**，别再手敲。
10. **⚠ 序 A/B（`ab_fused_lane.sh`）这道门本身是 flaky 的**——本 session 量过：**基线 2/24 轮、改动后 3/28 轮**
    出现一次不匹配（Fisher p≈1.0，即无差别），失败总是落在不同的捕获/序，且它比的是**默认跑 vs 显式
    `event` 跑（同一条代码路径）**。**⇒ 它红了不要直接当回归**：先重跑；要判定回归，拿**捕获网**
    （严格 27/27、有界 flips 0）说话——那三条网在这些轮里一次都没 flake。
11. 沿用：每 lane 一次同步是硬约束；相位账不可跨路线比（比 `ce+eq`）；`[metal_stats]`/`[ul_gpu_lane]`/
   `[mmse_time_sum]` 走 **stderr**；`--log.filename` 不建父目录；两条配置构建门必须串行。

---

## 5. 下一步

### 5.1 第 3 项：lane 是 `116.6(提取) + 125.3(队列①) + 340.6(权重) + 78.0(队列②) + 74.7(burst)`

**判据现状：Q1/Q2 是**从两个独立探针**推出来的（`host:` 间隔 与 `[mmse_time_sum] total`），
**还没有直接量过**。第一步就是把它测实。

0. **✅ 已做（`e1bd5dbd70`）**：把那条**恒等式**修成真测量，并**已上机**（腿 `gnb_q1probe_0918_1640`）：
   **Q1 = 58.6、Q2 = 64.0、Q3 = 374.5 µs**，时间线三条独立路径闭合到 0.1 µs（见 §2.6b）。
   ⇒ **结论：gap 211.2 = 宿主/唤醒 65.0（31%）+ 队列 146.3（69%）。**
1. **Step 2 现在有实测支撑**：它同时消掉那 **65 µs 宿主侧**（~45 µs 是 `waitUntilCompleted` 的唤醒延迟、
   ~20 µs 是工作）**并**让权重 CB 的 64 µs 队列等待与提取剩余的 GPU 时间重叠 ⇒ **上限仍是 ~129 µs/lane**。
2. **队列那 146 µs 是更大的结构项**，来源是前端**每隙一条 423 µs 的整块 DFT**（占 1 ms 时隙 ~42%）。
   削它/拆它是**另一条独立路线**（可与 Step 2 并行评估）。
3. **Step 2（宿主不再等提取）仍值 ~125 µs/lane**，**但前置件是槽位轮转**（见 §2.6c 的连带结论）。
   顺序必须是：**先轮转，再拆同步**；反了就是把不等价放大到全链路。
4. **Step 1 若重做**：正确形状是给 CFO 一个**随跳推进的轮转槽位**，不是"直接读那个单 float 缓冲"。
   **✅ 已按这个形状落地（`2eca4a7965`）**：`kCfoSlots = 8`，转的是**指针**所以提取核与它的参数块一字未改；
   "抄上一槽"保证与旧单缓冲**语义等价**（判据：严格网 27/27、有界网 26/27 flips 0，**与改动前基线逐字相同**）。
   机制（为什么非做不可）见 §48.194(g-tresvicies)：估计器实例来自 `max_nof_concurrent_threads` 的**池**，
   而实例在 lane 还在飞的时候就还回池子 ⇒ 后一跳会覆盖前一跳 K4 还没读的槽。

### 5.1b **Step 2：前半段已落地；后半段经查证做不到（原形状作废）**

**✅ 已落地（`832832c442`）**：提取 commit 时 signal lane fence；权重 CB 编码开头 wait 它。
**惰性**（宿主还在等 ⇒ generation 早已满足）⇒ 捕获网与改动前**逐字节相同**、`FUSED_AB_RC=0`。
⚠ **离线门覆盖不到它**（非延后的跳被适配器强制成 `host_wait`，replay 的跳不是延后的），
唯一上机判据：`[metal_stats] lane fence signals` 从**每 lane 1 次变 2 次**。
**✅ 已上机验证（`gnb_fence_0918_1805`）**：`lane fence signals=3178` / `lanes=1589` ⇒ **正好每 lane 2 次**；
契约 7/7、`Real-time failures 0/76779`、`device_sigma2=1589=lanes`、时间线仍闭合到 0.1 µs（见 §48.194(g-septemvicies)）。

**⛔ 后半段（去掉宿主那次 wait）做不到——是一条真依赖链，不是工作量问题**：

```
build_pilots_lse（WAIT）→ gpu_ls_sigma2[+kSigma2]/[+kPowerSum]（设备标量，CB 完成前无效）
  → sigma2 / pilots_power → sigma2_rel（:1266）→ stats_in.sigma2（:1402）
  → stats_estimator->estimate()（:1429）→ channel_statistics stats
  → correlation_stage(stats,…) / build_correlation_matrices(stats,…)（:1440/:1457/:1720/…）
  → 权重 CB 的参数与矩阵
```

`correlation_stage(stats, …)` 在**设备路线**上也调用（`build_slots_on_device()`），
所以 `stats` 不是“只有宿主建矩阵才需要”；而 `stats.sigma2` 要的是**宿主自己的**比值
（设备商差 1 ulp，§48.194 有实测），不能顶替。
⇒ **`wait → 编码权重` 换成 `编码权重 → wait` 不成立**，除非把**统计量/相关模型的推导也搬上设备**
（K0-a 同级的一件事）。

**✅ 旁路 1 已查完（§48.194(g-sesvicies)）：不成立 ⇒ Step 2 到此收口。**

- `correlation_stage()` 从 `stats` 只取 `fd_hz`/`tau_rms_s`/`sigma2`；v1 提供者是
  `channel_statistics_estimator_fixed`，**前两个是构造常量** ⇒ 整条链收敛到 `sigma2_rel` 一个标量。
- 该标量在**核里确实死**（`ocudu_mmse_corr.metal:151`：`sigma2_from_device` 置位时核不读 `p.sigma2`），
  **但宿主建矩阵时不死**：`build_correlation_matrices(stats,…)` 四处调用（`:1440/:1720/:1858/:1953`）
  **全是"宿主自己写这些槽位"的路线**，`:1720` 的注释明写 edge 块 "keeps the host build"。
- 代码自己的记录（`:1858`）：设备建只覆盖 **37.5% 的跳（26761 / 71384）**，其余有 remainder ⇒ 合并 ⇒ 宿主建。
- ⇒ **只有约 37.5% 的跳能用上"推迟等待"**，却要为这少数跳在 600 行函数里穿第二条路径、逐跳切换 lane 结构。
  **收益 ~48 µs，代价与风险更大，而这 129 µs 本来就不是失败源。**

**⇒ Step 2 按原样做不到、按旁路做也不值得**（要真拿到，得把**统计量/相关模型推导搬上设备**，K0-a 同级）。
**下一件该看的是前端那条 423 µs 的整块 DFT**——占 gap 的 **69%**，且没有这条纠缠。

**⚠ 已作废的候选**：①"拆 reformat 成独立 CB"——burst 读的 `gpu_ce`/`gpu_nv` 正是 K3/K4 的输出，
拆出去只会更晚（代码已核）。②"跨 hop 复用 A⁻¹"不受影响，但仍是估计器语义变化，需独立判据。

**⚠ 背景事实**：实时失败 **0/62758**、GPU 占用 ≈ 11% ⇒ **lane 延迟当前不是失败源**，
削它是买 headroom。**但注意**：11% 是**平均值**；前端 DFT 在有时隙的 1 ms 内占 42%，
所以"平均很闲"与"后端要等 105 µs"并不矛盾——这正是 Q1 要证实的东西。

**⚠ 本 session 的教训（写给下一件）**：同一个 gap 我已经**误归属过两次**（先"依赖串行化"、
再"宿主交棒"），两次都是**拿两个量的巧合相等当因果**。凡是要说"X 造成了 Y µs"，
**必须有一个只测 X 的探针**，不能靠减法凑。

### 5.2 收尾

1. **提交 §2.6 的探针改动**（1 个文件 `ocudu_metal_lane_probe.mm` 未提交；门已全绿）。
2. 可选：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 里 `sinr_threshold` 已去掉，无需再动。

---

## 6. 关键路径与开关

| 内容 | 路径 |
|---|---|
| 设计文档（唯一活文档）| `doc_chinese/full_gpu_chain/s2_full_chain_design.md`（**§48.146(a) 状态索引**；本 session：§48.192–§48.194）|
| 上机腿脚本 | `doc_chinese/full_gpu_chain/wip/run_air_leg.sh <label> [OCUDU_*=… 或 --cell_cfg.*=…]`（**已修参数分流**）|
| 腿判据脚本 | `doc_chinese/full_gpu_chain/wip/air_leg_report.sh <log>`（含 `gap:` / `host:` / `busy split` 各行）|
| 每腿门（**一条命令、串行**）| `bash doc_chinese/full_gpu_chain/wip/run_leg_gates.sh <label> [n]` ⇒ `LEG_GATES_RC=0`（三张网 → 序 A/B → 七门 → Metal 自测 → `ctest -L phy` → 两条配置构建门；日志落 `work_tmp/gates/<label>/`）|
| RACH 风暴判读 | `doc_chinese/full_gpu_chain/wip/rachstorm_repro.sh` + `rachstorm_judge.py`（本 session 新增，已自证）|

> **⚠ 两个构建坑（本 session 各踩一次）**：① `cmake --build build`（`all`）**不会**重链
> `port_channel_estimator_metal_mmse_unit_test` / `ul_chain_replay`（它们不在 `all` 里），
> 改了 `lib/` 之后必须**显式** `--target` 它们，否则跑的是旧二进制、看到的还是旧序列；
> ② `build/hashes.h` 是 configure 期生成的，version 串要 `touch build/hashes.h && cmake --build` 才更新。

**开关台账（默认值从代码核实）**：`OCUDU_CE_LANE_ORDER`（**默认 `event`**）、`OCUDU_CE_FUSED_BURST`（旧别名）、
`OCUDU_DFT_OPEN_BLOCK`（默认开）、`OCUDU_UL_RX_SYMBOLS`（unset/0 = whole-slot）、`OCUDU_UL_FRONTEND_FENCE`（默认关，别开）、
`OCUDU_METAL_GPU_TIME`（opt-in）、`OCUDU_CE_CPU_CE/CPU_LS/GPU_INVERT/CPU_INVERT/CORR_DEV/SPLIT_TAIL/DEV_SIGMA2/DEV_Y`（A/B 或逃生）、
`OCUDU_CE_K0A_RATIO_DEV`（**默认 1**）/`OCUDU_CE_K0A_RATIO_CHECK`（探针）。
**`OCUDU_CE_K0A_FUSE` 不在树里**（已否决，不要再加）。
**testbed 侧（yml，不入库）**：`cell_cfg.pucch.max_consecutive_kos: 300` + 5 个 narrow-BW 资源值。

---

## 7. 回来的第一个动作（TL;DR）

1. `git log --oneline -3` 确认 HEAD = **`d93e004cfd`**；`git status --short lib/` 应有 **1 个未提交文件**
   （`ocudu_metal_lane_probe.mm`，§2.6 的空洞观测）——**先跑门再提交**：
   `bash doc_chinese/full_gpu_chain/wip/run_leg_gates.sh <label> 27`（判据 `LEG_GATES_RC=0`）。
2. 读 **§48.194(g-vicies)**（gap 的真正划分：宿主 125 + 设备 78）——**第 3 项的起点**。
   §48.194(g-nonies) 的"宿主 5%/队列 0%/依赖 95%"**已被更正，不要再用**。
3. 第 3 项第一杠杆 = **把 CFO 留在设备**，拆掉"权重 CB 必须等提取跑完才能编码"这条链（§5.1 第 1 条）。
4. 上机腿：`sudo -E bash doc_chinese/full_gpu_chain/wip/run_air_leg.sh <label>`（**无需参数**；
   注意**本 session 的 shell 没有 TTY、sudo 要密码，腿必须由用户在自己的终端跑**），
   跑完 `air_leg_report.sh` 判读；判读前确认 `ps aux | grep [g]nb` 为空。
   要看的四行：`gap: extraction end -> weights start (device)`、
   `gap: weights end -> burst start (device)`、`host: extraction commit -> weights commit`、
   `host: weights commit -> burst commit`。
