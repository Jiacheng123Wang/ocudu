# 交接：GPU-PHY 全链会话状态（2026-09-15，供新会话接续）

> **本文是本次会话切换的快照（只读，不回填）**。活文档是 `s2_full_chain_design.md`（本轮新增 §29–§36，共 1864 行）。
> 目标：让新会话**不需要本会话的任何上下文**即可继续。

---

## 0. 一句话现状

`apple-silicon` 分支 HEAD = **`da46c753e9`**（工作树干净，`lib/`+`include/`+`tests/` 零改动，**全部已推送**）。
本轮已完成并推送：S-5c（尾块并入标准批次 ⇒ 一跳一条 CB）、S-5d（相位探针补到 `run()`）、
S-6a（K3：均衡器的信道估计在 GPU 上产出）、S-6b（均衡器直接读设备缓冲 + 布局改纯算术 + 可验证性探针）、
S-6c-0（K4：噪声方差 GPU 归约 + 零拷贝缓存预约修复）、S-6c-1a（引擎 `run_async`/`wait_pending` + Test 4c）。
**未完成、下一步唯一要做**：**S-6c-1b**（估算器两阶段拆分，让 CE 不再等待自己的 command buffer）。
设计已定稿（活文档 §34/§36 + 本文 §3），**代码一行未动**。

用户的本地文件**不要动、不要提交**：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`、
`configs/gnb_zmq.yaml`、`scripts/switch_gnb_plmn.sh`、`bler_metal_bg1_z64_*.csv`、`configs/*.swp`。

---

## 1. 用户 vision 与优先级（未变，但本轮拿到了新的定量全景）

- **最终目标**：RF **I/Q samples → MAC PDU 全链条 GPU Metal path**。
- **回退方案**：CPU thread 无阻塞 dispatch → GPU 一路到 **LLR** → 回 CPU 另一 thread 做 **CPU LDPC** → FAPI → MAC。
- **现阶段主任务**：**LDPC 之前的 GPU pipeline 最优**（IQ → demod LLR）。LDPC 暂缓（先做可行性研究）。
- **本轮新增的定量全景**（OTA，固定 16QAM，每 slot）：`TF 237 + CE 336 + eq/demap 281 + FAPI 4 ≈ 858 µs`，
  而 **`LDPC 3538 µs`** ⇒ **LDPC 一极是"它之前所有阶段之和"的 4 倍**；
  LDPC 之前**只剩约 1% 可拿**（就是 S-6c-1b）。**这是下一步之后该转向 LDPC 的定量依据**（本文 §4）。

---

## 2. 工作方法与硬规则（未变）

1. 每步 = **代码 + 构建 + 单测 + A/B 门禁 + commit + push**；提交前缀 **`GPU-PHY S-<id>:`**；**禁止**在提交信息/注释里引用 `doc_chinese/`。
2. Backend 选择只通过 `expert_phy` CLI（env 只用于 debug/A-B）。
3. **TX/DL 保持 CPU**，不动。
4. `doc_chinese/` 不提交；`s2_full_chain_design.md` 是**唯一活文档**；`session_handoff_*.md` 是**只读快照**。
5. E2E（OTA）由用户跑；**需要 OTA 时明确告知**。

### 门禁命令（⚠️ 本轮修正：metal 单测二进制**不在 `all` 目标**里，必须显式 `--target`）

```bash
cd /Users/jiachengwang/dev/ocudu && cmake --build build -j8
cmake --build build -j8 --target baseband_gateway_buffer_metal_smoke_test channel_equalizer_metal_unit_test \
  demodulation_mapper_metal_unit_test dft_processor_metal_unit_test ldpc_metal_unit_test metal_chain_probe \
  metal_dispatch_probe ofdm_demodulator_metal_batch_test phy_metrics_deferred_chain_test \
  port_channel_estimator_metal_mmse_unit_test pusch_demodulator_deferred_chain_test
ctest --test-dir build -L phy -j8 | grep -E "tests passed|tests failed"        # 期望 161/161
# 10 个 metal 二进制逐个跑（期望全 OK）
OCUDU_CE_LEVEL_STRICT=1 ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test | tail -1
./build/tests/unittests/phy/upper/channel_processors/pusch/pusch_demodulator_deferred_chain_test   # 期望 4/4
```
**为什么必须 `--target`**：`cmake --build build` **不会重建**这些二进制，编了库却跑旧二进制会得出"改动无效"的错误结论——本会话差点因此误判 S-5c（见 §5.4）。

---

## 3. 下一步：S-6c-1b —— 估算器两阶段，让 CE 不再等待自己的 CB

### 3.1 目标与机制

现状：CE 的 hop 成本几乎全是 **`waitUntilCompleted`**（配对 A/B：host 栈 `gpu_wait=106.5 µs`、设备栈 `142.1 µs`，
而 hop 总时间 176 µs 里其余只有几 µs）。要让 CE **不等待**：
- 引擎侧**已就位**（S-6c-1a）：`run_async()` 编码+提交不阻塞；`wait_pending()` 等待；**每个入口点先 wait 掉上一次提交**
  ⇒ **一个引擎最多一条 CB 在飞**（这是与当年卡死的 batch API 的本质区别）。
- 还差：**估算器的 host 侧收尾必须能推迟**（否则 `compute()` 返回前就得 wait），
  以及**消费侧的一次显式同步**（均衡器的 dispatch 读设备缓冲，必须在 burst 提交前确保 CE 的 CB 已完成）。

### 3.2 已就位的前置条件（本轮成果，直接用）

| 需要的东西 | 状态 | 位置 |
|---|---|---|
| 均衡器要的信道估计在设备侧 | ✅ K3，逐位一致（Test 12） | `ocudu_mmse_reformat.metal`、`stage_re_masks()` |
| 均衡器直接消费设备缓冲（不再 host 抓取） | ✅ S-6b | `ch_est_device_view`、`view_ch_est_list.h`、`pusch_demodulator_impl::get_ch_data_estimates()` |
| 均衡器要的噪声方差在设备侧 | ✅ K4，相对误差 1.2e-07 | `mmse_noise`、`get_device_noise_variance()` |
| 引擎可"先 commit、后 wait" | ✅ S-6c-1a，3000 次不卡死 | `run_async()` / `wait_pending()` / `has_pending()` |
| CE 侧可观测（本轮新增探针） | ✅ `device_hops`、`pusch_demod ch_est device/host` | `[mmse_time_sum]`、`[metal_stats]` |

### 3.3 两条硬约束（决定方案形状，别再踩）

1. **基类 per-hop scratch 是 `compute()` 的局部变量**：`enlarged_pilots_lse`、`pilots_lse`、
   `enlarged_filtered_pilots_lse`、`filtered_pilots_lse`（`port_channel_estimator_average_impl.cpp:296-298`；
   只有 `pilot_products` 是成员）。而基类的 **rsrp / 噪声 / TA 统计在虚钩子之后**读它们 ⇒
   要推迟 wait，必须**在基类里拆开 submit/finish**，并把这些 scratch **提升为成员**（args 结构化后要保活到 finish）。
2. **`gpu_h` 是跨 hop 复用的引擎暂存缓冲**：下一个 hop 的 K2 会覆盖它 ⇒ **多跳时"解包"不能跨 hop 推迟**，
   只能**逐 hop 交错**（本项目链路是 2 跳频跳，`t_align≠0` 可确认）：

```
h0.prepare+stage → h0.run_async（不 wait）
h1.prepare+stage（与 h0 的 GPU 执行重叠）
h0.finish：wait + unpack + 统计        ← 必须在 h1 的 K2 执行前（gpu_h 会被覆盖）
h1.run_async（入口先 wait h0；此时已完成 ⇒ 立即返回）
… burst 提交前 sync → h1.finish
```
单跳时则是"demodulator 的 Pass 1+2（`ch_re` 抓取 + staging + 编码）与 CE 的 GPU 执行重叠"。

### 3.4 ⚠️ 开工第一件事：核实均衡器**何时**读 `noise_var_estimates`

这决定 sync 钩子的位置，进而决定收益大小：

- 若均衡器 Metal 后端**在 flush/commit 时才**把 `nv` 拷进组缓冲 ⇒ sync 放在 **`demapper->wait()` 之前**即可，
  重叠 = Pass 1+2 的全部 host 工作（**收益最大**，几十 µs/PUSCH）。
- 若它在 **encode（`submit`）时就**读 `nv` ⇒ sync 必须提前到**第一次 `equalizer->submit()` 之前**，
  重叠只剩"demod 配置 + 第一个符号的准备"（**收益明显变小**）。

要看的文件：`lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm`（`enqueue_burst` / `eq_flush_hook` 里
`nv` 的拷贝时机）与 `channel_equalizer_metal.cpp`。**先把这件事查清再动手**。

### 3.5 建议的提交切分（每个提交独立门禁）

1. **接口 + 基类两阶段**（classical 路径必须保持**完全同步**：默认实现 `compute()` = `submit()`+`finish()`）：
   - `port_channel_estimator.h`：`submit()` / `finish()`；`port_channel_estimator_results::sync_device_estimates()`（默认 `true`）。
   - `dmrs_pusch_estimator.h`：同样一对 + `sync_device_estimates()`（默认 `true`）。
   - `port_channel_estimator_average_impl.{h,cpp}`：拆 `compute()`/`compute_hop()`；scratch 提升为成员；
     保持 `port_channel_estimator_average_impl` 的**同步行为逐位不变**（这是该提交的门禁）。
2. **Metal 估算器真异步**：`apply_fd_td_estimation_stage` = stage + `run_async`；新增 finish 段做 wait + 现有 unpack；
   `sync_device_estimates()` → `engine->wait_pending()`。默认**仍走同步**（`OCUDU_CE_CPU_CE=1` 之外的开关，或先只让 submit 生效但 compute 立刻 finish）。
3. **demodulator + processor 接线**：设备路径下先 `sync_device_estimates()`，再提交 burst；
   均衡器消费 K4 的设备噪声方差；`pusch_processor_impl::process_data` 把
   `est_results.get_channel_state_information(csi)` **从 demodulator 之前移到之后**（CSI 只用于上报，
   不参与 demod 配置；`ul_capture` 打开时强制同步路径）。⚠️ 这会让 **FAPI 的 CSI 指示晚于数据指示**，OTA 上确认 MAC 无影响。
4. **打开默认 + 文档**。

### 3.6 门禁（缺一不可）

- **软比特逐位一致**：`OCUDU_CE_CPU_CE=1` 对照（deferred chain 测试 + OTA 的 KO 表）；
- `[metal_stats] mmse_ce commits / hops ≈ 1.0`、**`max_in_flight` 仍为 1**（不是累积模式）；
- `[metal_stats] pusch_demod ch_est device>0 host=0`、`[mmse_time_sum] device_hops ≈ calls`；
- **`[mmse_time_sum] mean total` 与 `ul_channel_estimation` median 应下降**（重叠生效）；`gpu_wait` 可不变（它测的是 GPU 窗口）；
- CSI 数值不变（RSrp/SNR/TA 与 `OCUDU_CE_CPU_CE=1` 对照）；
- `hops_no_gpu=0`、`fb_blocks=0`。
- **回退**：`OCUDU_CE_CPU_CE=1`（host 栈，一行、无需重编）。

### 3.7 收益上限（诚实预期）

可重叠的只有 host 侧准备（Pass 1+2 / 下一个 hop 的 prepare+stage），**几十 µs/PUSCH**；
`wait` 里的队列等待**不可能**被 CPU 工作填满，且 burst 提交前的 sync 是硬约束 ⇒ **拿不到那 142 µs**。

---

## 4. 之后的候选：LDPC（真正的数量级）

- 先做**定量剖析**：`ul_ldpc_decode` median 3538 µs 到底花在哪（TB/码块数、迭代数、dispatch 次数、`max_in_flight=1` 的同步等待占比）。
  现成工具：`ldpc_metal_bler_test`、`[metal_stats] ldpc_decoder`、`ul_chain_replay`。
- 已知事实：现在 LDPC 是**同步**的（`commits=waits`、`max_in_flight=1`），每个 PUSCH 阻塞 CPU 数毫秒。
- 用户怀疑：**3GPP LDPC 的分层/双对角结构面向串行**，GPU 上逐 TB dispatch 天然吃亏 ⇒ 先研究再决定是否重写。
- 与本轮的关系：LDPC 之前的 pipeline 已经收敛，`~858 µs` 里剩下的可优化空间约 1%。

---

## 5. 已知坑（务必先读）

1. **环境漂移（本机）**：同一份未改动代码不同时间可差 20–100%（Test 4a GPU 24.5→44 µs、hop 231→281 µs）。
   **任何 A/B 前先连测两次确认可复现**；且**不要与门禁并发测**（并发时 hop 从 183 漂到 423 µs）。
2. **NaN 吞误差**：用 `std::max(err, |a-b|)` 比较时 NaN 会让它保持旧值（看起来"误差 0"）。比较用 `if (!(err < tol))`。
3. **零拷贝缓存 re-wrap 陷阱（本轮最重要的坑）**：`mmse_engine_impl::wrap()` 用 `buffer_cache.emplace`
   （**不覆盖已有 key**）⇒ 请求尺寸一旦超过缓存尺寸就**新建 MTLBuffer** 且缓存永远停在最初的小尺寸。
   OTA 单次运行 **6492 条** `zero-copy cache hit with a larger request`，每跳一次 Metal 分配 ⇒ **+60 µs/PUSCH**。
   **凡新增会被引擎 wrap 的缓冲，必须在构造函数里按容量 `reserve_buffer()` 一次**（`197f5670c6`）。
4. **本地 A/B 的形状必须随真实链路变化**：本会话**两次**栽在"缓存/记忆化在本地全命中"——
   K3 的 mask 暂存记忆化（本地 +2~5 µs、OTA +34 µs）与上面的 re-wrap。单测里同一形状连跑 50 次会系统性高估命中率。
5. **S-5c-1 的卡死已结构性排除**：当年 `begin_batch/commit_batch` 累积未提交的 CB，队列槽耗尽、在第 1431 跳挂死。
   现在 `run_async` **立即提交**且入口先 wait ⇒ 最多一条在飞；**Test 4c** 用 3000 次延迟提交守住这条不变量。
   ⇒ 若将来有人要"攒多条再提交"，先读这段。
6. **K3 的布局是纯算术的 ⇒ 要求连续分配**：非连续分配**拒绝产出**设备估计（`stage_re_masks()` 里 `find_highest+1-first != nof_prb`），
   消费者靠 **RE 数量护栏**回退 host（Test 12 有负例）。`dc_sc`（DC 擦除）由生产者在 K3 里写 0（host 路径由 demodulator 写）。
7. **DC 契约**：`port_channel_estimator::configuration::dc_position` 一路传到 stage args，K3 在该 RE 写 0；
   mask **仍包含**它（否则 RE 数量对不上）。
8. **批量均衡器的编码时机缺陷（未解，默认关）**：`OCUDU_EQ_DEFER_ENCODE=1` 时延后编码会发散
   （同管道 + 立即编码 = 0 差异；延后整组 = 4485 差异）。缺陷在"编码时机"，不在批量核。
   下一步实验：把 flush 提前到 **pass 1 结束**（接口加轻量 `submit_group_end()`）。
9. **K1（求逆）现状**：分块 Gauss-Jordan（b=8，无选主元）24.5 µs（历史）/44 µs（本机当前），**是默认**；
   剩余 ~15 µs 不在枢轴相位。warp 版修好但不更快，未进主干。
10. **探针/开关表**（env 只用于 debug/A-B）：
    - CE：`OCUDU_CE_CPU_CE=1`（**整个设备栈 → host，一行回退**）、`OCUDU_CE_SPLIT_TAIL=1`、`OCUDU_CE_TAIL_CPU=1`、
      `OCUDU_CE_CPU_INVERT=1`、`OCUDU_CE_NO_K4=1`（跳过 K4 编码，暂存照旧）、`OCUDU_CE_LEVEL_STRICT=1`、
      `OCUDU_MMSE_DEBUG=1`（引擎相位：wrap/cb/encode/commit/wait）、`OCUDU_INV_TGX/TGY`
    - PUSCH：`OCUDU_PUSCH_FORCE_SERIAL`、`OCUDU_PUSCH_DEFERRED_GROUP`、`OCUDU_EQ_GROUP_SUBMIT`、
      `OCUDU_EQ_DEFER_ENCODE`、`OCUDU_DFT_PIPELINE_DEPTH`
    - 编译期：`ENABLE_CE_TIME`（`[mmse_time_sum]`）、`ENABLE_METAL_STATS`（`[metal_stats]`）——两者当前都是 ON
11. **日志**：不要开 `log.all_level: debug`（~4000 行/s 拖死控制台）。只给需要的层开 debug。
12. **OTA 协议（本轮学到）**：
    - **先比"负载指纹"再比性能**：`hops/slot`（`calls/samples`）、`dft commits`/slot、调制分布、`ul_ldpc_decode` mean、
      `sigma2`/`corr`（≈平均 hop 大小）。指纹不一致 ⇒ 数字不可比（本会话两次踩到）。
    - 想做可信 A/B ⇒ **固定 MCS**（`cell_cfg.pusch.min_ue_mcs == max_ue_mcs`），LA 这个混淆变量就没了（上次配对很干净）。
    - **`max_ue_mcs` 的语义陷阱**：256QAM 表里 MCS 20 就是**第一个 256QAM 条目** ⇒ 20 并不保守；
      封顶 64QAM 用 19、16QAM 用 10、QPSK 用 4。
    - 用户当前配置 = 历史基线：`pusch.max_ue_mcs: 20`（`min_ue_mcs` 注释）、`pdsch` 注释（不限制）。
13. **不要动用户的本地文件**（见 §0 列表）。

---

## 6. OTA 基线与本轮数据（对比用）

### 6.1 三个基线（同配置 `max_ue_mcs: 20`，未固定 MCS，**负载不同、仅供粗比**）

| 指标 | A：仅 S-5c | B：S-6b 首测（含 re-wrap 回归） | C：S-6a/b/c-0 |
|---|---|---|---|
| commit | `a41480c004` | `7bc264fd24` | `c25796c5ed` |
| samples / calls | 1338 / 1491 | 1275 / 1376 | 1123 / 1872 |
| hops/slot | 1.11 | 1.08 | 1.67 |
| `[mmse_time_sum] mean total` | 293.4 | 327.2 | 335.6 |
| `gpu_path` / `gpu_wait` | 277.2 / 105.8 | 311.1 / 116.0 | 320.7 / 138.6 |
| `ul_channel_estimation` median | 307.2 | 338.8 | 364.9 |
| `ul_equalization_demod` median | 273.6 | 273.9 | 274.5 |
| `mmse_ce commits/calls` | 1.0013 | 1.0015 | 1.001 |
| QPSK KO | 5.5% | 4.3% | 8.7%（调制混合已变） |

### 6.2 配对 A/B（固定 16QAM，**可用**）

| 指标 | host 栈 | 设备栈（re-wrap 修复前） | 设备栈（修复后 `197f5670c6`） |
|---|---|---|---|
| samples / calls | 607 / 2120 | 630 / 2274 | 546 / 1754 |
| `grep -c "larger request"` | 0 | **6492** | **0** ✅ |
| `mean total` | **301.6** | 360.7 | 332.3 |
| `gpu_path` / `gpu_wait` | 285.5 / **106.5** | 344.8 / 143.0 | 316.3 / **142.1** |
| `ul_channel_estimation` median | **302.0** | 362.7 | 336.5 |
| `ul_equalization_demod` median | 284.8 | 285.6 | 281.1 |

**读法**：设备栈当前每 PUSCH **+34.5 µs**（全在 GPU 窗口 = K3/K4 两次 dispatch 在争用 GPU 上的代价），
消费侧 **≈0**。用户已决定**保持默认开**并继续 S-6c-1b（方向优先），前提是 S-6c-1b 能兑现重叠。

### 6.3 slot 预算（固定 16QAM 那轮，每 PUSCH）

`ul_pipeline` median ≈ 4387 µs = TF 237 + CE 336 + eq/demap 281 + FAPI 4 + **LDPC 3538** + 其它。

---

## 7. 本轮提交记录（`450e2988dd` → `da46c753e9`，全部已推送 `apple-silicon`）

| commit | 内容 |
|---|---|
| `450e2988dd` | S-5c 尾块并入标准批次（一跳一条 CB；Test 11 merged≡split 逐位一致；−100 µs/跳） |
| `a41480c004` | S-5d 相位探针补到 `run()`（原来只装在 `run_weights_only`，生产路径上是瞎的） |
| `cb24036986` | S-6a K3 kernel：均衡器要的信道估计在 GPU 上产出（Test 12 device≡host） |
| `b77447a97d` | S-6b 均衡器直接读估算器的设备缓冲（+ LLR 逐位一致的新门禁、DC 契约） |
| `7bc264fd24` | S-6b 可验证性探针（`device_hops`、`pusch_demod ch_est device/host`） |
| `1206da74eb` | S-6b 布局改**纯算术**、删掉 mask 暂存（OTA 实测 +34 µs → ~0） |
| `c25796c5ed` | S-6c-0 K4：噪声方差 GPU 归约（相对误差 1.2e-07）+ 结构体错位/单线程组两个坑 |
| `197f5670c6` | S-6c-0 零拷贝缓冲按容量预约（re-wrap 6492 → 0，回收 ~29 µs） |
| `da46c753e9` | S-6c-1a 引擎 `run_async`/`wait_pending` + Test 4c（3000 次、in-flight≤1） |

上一轮（S-4b…S-5c）的提交表见 `session_handoff_2026-09-13-1.md`。

---

## 8. 立刻可执行的开工顺序（建议）

1. **环境基线**：跑两次 CE 单测，记下 Test 4a/Test 6/`[mmse_time_sum]`，确认与 §6 的可比性。
2. **S-6c-1b 第 0 步（半小时内能定生死）**：查清均衡器 Metal 后端**何时**读 `noise_var_estimates`（§3.4）。
   这一步决定 sync 钩子位置与收益上限——**先查再写代码**。
3. **按 §3.5 的四个提交推进**，每个提交跑 §2 的完整门禁；
   第 1 个提交的门禁要点：**classical 估算器的行为逐位不变**。
4. 完成后更新活文档（§37），需要 OTA 时明确告知用户（并附"看什么数字 + 回退开关"）。
5. **S-6c-1b 收尾后转向 LDPC**（§4）：先定量剖析，再决定重写或换并行化策略。
