# 交接：GPU-PHY 全链会话状态（2026-09-13 #3，供新会话接续）

> **本文是本次会话切换的快照（只读，不回填）**。活文档是 `s2_full_chain_design.md`（本轮新增 §37–§41，共 2022 行）。
> 目标：让新会话**不需要本会话的任何上下文**即可继续。
> 前两轮依次读 `session_handoff_2026-09-13-1.md`（S-4b…S-5c）、`session_handoff_2026-09-13-2.md`（S-5c…S-6c-1a，含用户 vision、硬规则、坑表、OTA 基线）。

---

## 0. 一句话现状

`apple-silicon` 分支 HEAD = **`11f7f3dcf6`**（工作树干净，`lib/`+`include/`+`tests/` 零改动，**全部已推送**）。
本轮完成并推送 9 笔：

| commit | 内容 |
|---|---|
| `c5f1648ab7` | **S-6c-2a** 均衡器就地读设备估计（设备 slice + kernel `h_offset`/`h_layer_stride`；CE 与 eq 共用同一 Metal 资源） |
| `988e69004e` | **S-6c-2b** 均衡器的噪声方差也留在设备（迁移主机端口判据进 kernel；能力查询 + 逐端口实际可用性） |
| `984b561638` | **S-6c-1b-1** 估算器测量值改为解调之后再上报（为"推迟估算"铺路，行为不变） |
| `83ffbd3d0d` | **S-6c-3** command-buffer 待决链按队列分开（DFT 独占前端队列；把隐含不变量变成强制的） |
| `e086ff00f8` | **S-6c-1b-2a** 估算器逐跳工作拆成 `submit()`/`finish()`（纯结构） |
| `be2a7ba2ec` | **S-6c-1b-2a-fix** 修复 2a 引入的回归：filter 策略下 `filtered_pilots_lse` 的 12 子载波窗口偏移丢失 |
| `22b8afc561` | **S-6c-1b-2b** Metal 侧真正异步：`run_async` + 完成钩子（解包与 pilot 缓冲填充都搬进去） |
| `11f7f3dcf6` | **S-6c-1b-2c** 解调与估算真正重叠（`submit()` 逐端口 + `sync_device_estimates()` 幂等收尾 + "本次是否全程读设备"查询 + 跳内多批次先收尾） |
| — | 本轮还更新了活文档 §37（第 0 步证伪结论）与 §38–§41 |

**S-6c-1b 全链已完成并在 OTA 上验证**（2a/2a-fix/2b/2c/2d，活文档 §42–§44，HEAD `d417d5120f`）：
`defer_wait=`（真异步的证据）+ `ul_channel_estimation min` 224→20.3 µs（估计工作移出该窗口）+
1637 个 CRC-OK TB + 不变量全绿；收益在未配对的一轮里看不见（重叠上限 83 µs，LDPC 占 69%）。**下一步**：
`apple-silicon` 的 pre-LDPC 部分到此收尾 ⇒ 转 **LDPC 定量剖析**（§4）。若要先给 2c 一个可信的收益数字，
再做**配对 A/B**（`cell_cfg.pusch.min_ue_mcs == max_ue_mcs` 固定 MCS）跑两轮（设备栈 vs `OCUDU_CE_CPU_CE=1`）。

⚠️ **两条 OTA 硬规则**：
1. **gNB 必须用 `sudo` 启动**：**usrsctp 需要 root 才能收发 raw socket**，非 root 起得来、radio 也正常，
   但 N2 的 `NG Setup` 必然超时（agent 曾误判为"核心没运行"，并用 TCP 探 7777/9090/8080 当证据——**不成立**，
   AMF 的 38412 是 SCTP）。agent 无法代跑（`sudo` 要密码）⇒ OTA 由用户执行、agent 分析。
2. **手机初始 PRACH 功率每次不同是正常的**（UE 按 DL power 从最小可行功率起步逐步抬升）⇒
   **PRACH RSSI 不能当链路质量判据**。

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml expert_phy \
  --pusch_channel_estimator_algo metal_mmse \
  --pusch_ldpc_decoder_type metal \
  --pusch_dft_type metal \
  --pusch_channel_equalizer_backend metal 2>&1 | tee /tmp/gnb_ota_2c.out
# 跑够流量后 Ctrl-C（探针在退出时打印）
```

⚠️ **不给 `expert_phy` 就是纯 CPU 路径**：这几个选项的默认值全是 `cpu`（`pusch_channel_estimator_algo`、
`pusch_channel_equalizer_backend`、`pusch_dft_type`；`ldpc_decoder_type` 默认 `auto`）⇒ 命令行必须显式指定，
否则 `[mmse_time_sum]`/`[metal_stats]` 根本不会出现（本会话的 agent 就踩过这个坑）。

agent 拿到 `/tmp/gnb_ota_2c.out`（或用户粘贴的数字）后看：**`[mmse_time_sum] ... defer_wait=`（本次改动的直接证据，
应当很大：覆盖了解调那段时间）**、`ul_pipeline` median（真指标，应下降）、`[metal_stats] wrap failures=0`、
`equalizer ch_est device>0 staged=0`、`pusch_demod ch_est host=0`、`hops_no_gpu=0`、`mmse_ce commits/hops≈1.0`、
`max_in_flight=1`、KO 表与 `OCUDU_CE_CPU_CE=1` 对照一致。
注意 `ul_channel_estimation` 分段会变小（CE 的设备工作搬到了 `record_ce_end` 之后），**不要单独比它**。

`submit()`/`finish()` 与 `complete_fd_td_estimation_stage()` 的骨架**已经在主干**（`e086ff00f8`），
所以 2b 只需动 Metal 侧：现在 `apply_fd_td_estimation_stage` 仍然是"跑完+解包"（同步），
`complete_fd_td_estimation_stage()` 是默认空实现。

用户的本地文件**不要动、不要提交**：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`、
`configs/gnb_zmq.yaml`、`scripts/switch_gnb_plmn.sh`、`bler_metal_bg1_z64_*.csv`、`configs/*.swp`。

---

## 1. 本轮最重要的结论：§36.1 的前提被证伪，路线改为 B

第 0 步门禁（"先查均衡器何时读 `noise_var_estimates` 再写代码"）的结果比预判更严重：

1. **所有引擎共用同一条 `MTLCommandQueue`**（`shared_queue::backend_queue()` 是进程级单例，
   `lib/phy/metal/ocudu_metal_queue.mm:89`；CE 引擎、eq、demapper 都走它）⇒ GPU 侧 CE→eq→demapper 的顺序
   **已由提交顺序保证**，主机同步的**唯一**理由是"**主机**要读 GPU 写过的内存"。
2. 均衡器在 **`submit()`（encode）时**既读 nv 的值（悲观分支），**也把 H memcpy 进自己的 staging**
   （`channel_equalizer_metal.cpp:405-418`）——而 S-6b 之后那些 span 指向 `gpu_ce`（设备缓冲）。
   ⇒ **S-6b 只去掉了 CE 侧的 unpack，设备→主机→设备的往返一直在**。
3. 所以按原设计，sync 必须提前到"第一次 `equalizer->submit()` 之前"⇒ 单跳 PUSCH 上 S-6c-1b 收益 ≈ 0。
   要把 §36.1 的窗口拿回来，必须**先**让 Pass 1 完全不读 GPU 内存 = S-6c-2（H + nv）。

**用户选了路线 B**：先补完 S-6b（S-6c-2），再做 S-6c-1b。**S-6c-2 本轮已做完**，
所以现在 Pass 1（1 端口部署形态）**不再依赖任何设备产物**，S-6c-1b 的重叠窗口是真实存在的。

> ⚠️ **2026-09-13 OTA 后的更正**：S-6c-2b 的动机描述里"主机读 GPU 内存"这一条**不准确**——
> `get_noise_variance()` 返回的是基类在**主机**上算出的值（`compute_hop_finish` 的 `estimate_noise`），
> 不是 K4 的设备值。它真正的价值是去掉"对估算器**主机侧收尾**的依赖"（推迟后那个值要等 `finish()`）。
> S-6c-2a（H 的 memcpy）才是真正的 GPU 内存读，那条动机成立。活文档 §39 已更正。

---

## 2. 工作方法与硬规则（未变）

1. 每步 = **代码 + 构建 + 单测 + A/B 门禁 + commit + push**；提交前缀 **`GPU-PHY S-<id>:`**；**禁止**在提交信息/注释里引用 `doc_chinese/`。
2. Backend 选择只通过 `expert_phy` CLI（env 只用于 debug/A-B）。**生产默认 algo 是 `cpu`**，Metal 靠 `--pusch_channel_estimator_algo metal_mmse`（或 `metal_nn_mmse`）选。
3. **TX/DL 保持 CPU**，不动。
4. `doc_chinese/` 不提交；`s2_full_chain_design.md` 是**唯一活文档**；`session_handoff_*.md` 是**只读快照**。
5. E2E（OTA）由用户跑；**需要 OTA 时明确告知**。

### 门禁命令（⚠️ metal 单测二进制**不在 `all` 目标**里，必须显式 `--target`）

```bash
cd /Users/jiachengwang/dev/ocudu && cmake --build build -j8
cmake --build build -j8 --target baseband_gateway_buffer_metal_smoke_test channel_equalizer_metal_unit_test \
  demodulation_mapper_metal_unit_test dft_processor_metal_unit_test ldpc_metal_unit_test metal_chain_probe \
  metal_dispatch_probe ofdm_demodulator_metal_batch_test phy_metrics_deferred_chain_test \
  port_channel_estimator_metal_mmse_unit_test pusch_demodulator_deferred_chain_test
ctest --test-dir build -L phy -j8 | grep -E "tests passed|tests failed"        # 期望 161/161
OCUDU_CE_LEVEL_STRICT=1 ./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test | tail -1
./build/lib/phy/upper/channel_processors/metal/channel_equalizer_metal_unit_test | tail -1
./build/tests/unittests/phy/upper/channel_processors/pusch/pusch_demodulator_deferred_chain_test   # 期望 5/5
```

**本轮新探针（S-6c-2 的可验证性）**，OTA/本地都值得看：

```
[metal_stats] wrap hits=<> creates=<> replaces=<> failures=<>     # failures>0 = 某处退回拷贝（不是零拷贝）
[metal_stats] equalizer ch_est device=<> staged=<>                # 1 端口部署下 device 应≈symbols，staged=0
[metal_stats] pusch_demod ch_est device=<> host=<>                # host 应为 0
```

---

## 3. 下一步：S-6c-1b-2（设计见活文档 §41，两处关键简化）

### 3.1 只需推迟**最后一个 hop**（原 §36.2 的逐 hop 交错不必做）

```
h0: prepare+stage → run_async → wait → unpack       （照旧同步）
h1: prepare+stage → run_async                        （不 wait）
    → notifier → demodulator Pass 1+2                ← 与 h1 的 GPU 执行重叠（本轮解锁的窗口）
    → demapper->wait()（同队列、提交在前 ⇒ 覆盖 h1 的 CB）
    → processor sync_device_estimates() → 再合并 CSI（§40 已就位）
```
最后一跳之后没有 hop 会覆盖 `gpu_h` ⇒ 需要保活的 per-hop scratch **只有一份**，风险与代码量小一个量级；
收益与全交错几乎相同（全交错多给的只是 2 跳时"下一跳 prepare ∥ 本跳 GPU"的 ~10–20 µs）。

### 3.2 拆分点（活文档 §41.2/§41.3 有完整清单，这里是提要）

- 基类：`compute()` = `submit()` + `finish()`；`do_compute` → `do_submit`/`do_finish`；
  `compute_hop` → `compute_hop_submit`（(A) 预处理+虚钩子）/`compute_hop_finish`（(C) rsrp/noise/TA）。
  保活状态：`enlarged_filtered_pilots_lse`（**局部变量，必须提升为成员**）+ hop/`nof_lse_symbols`/
  `stage_hop_offset`/`beta_scaling`/`cfo_hop`；其余可重建。
  **`pilots` 用 `finish(pilots)` 参数传**（它是 `dmrs_pusch_estimator_impl::temp_symbols`，估算器的成员，天然活到那时）。
- 新增第二个虚钩子 `complete_fd_td_estimation_stage()`（默认空）：classical 内联算完，设备后端把 wait+unpack 放这里。
- Metal：`engine_run()`（`:1168`）改 `run_async`；`unpack_engine_group()`（`:1247`）+ K3/K4 ready 记账挪进 complete，
  参数存成一份 pending 记录。
  ⚠️ **`run_async` 只覆盖 legacy `metal_mmse`**；`run_nn` 没有 async 形式 ⇒ 先只让 legacy 异步，nn 保持同步（文档化）。
- demodulator：**只在后端会在主机上读设备估计时**（`!equalizer->consumes_device_estimates(nof_ports, nof_layers)`，
  即 ≥2 端口或 CPU 均衡器）才在 Pass 1 前 `sync_device_estimates()`；1 端口 Metal 路径不需要任何同步。
- 回退：`OCUDU_CE_CPU_CE=1`（一行，无需重编）。

### 3.3 门禁

软比特/CSI 与 `OCUDU_CE_CPU_CE=1` **逐位一致**；`[metal_stats] mmse_ce commits/hops ≈ 1.0`、`max_in_flight` 仍为 1；
`hops_no_gpu=0`、`fb_blocks=0`；`[mmse_time_sum] mean total` 与 `ul_channel_estimation` median **下降**；
`wrap failures=0`。门禁要点：**classical 估算器行为逐位不变**（第 1 个提交的验收标准）。

---

## 3.5 现状架构基线（**开工前先读活文档 §45**）

进 LDPC 之前必须有的对照物，已写进活文档 **§45**（2026-09-13）：

- 五个模块各自的后端选项/默认值/日志确认方式（`--pusch_dft_type` 是**给 OFDM 解调器的** ⇒ 网格 FFT 在 GPU 上）；
- 两条进程级命令队列的拓扑（前端 = DFT，后端 = CE/均衡器/demapper/LDPC）；
- **每个边界的输入/输出在哪、谁等谁**：唯一真正设备驻留的段落是 `CE → 均衡器 → demapper`；
  仍走主机往返的是 (a) FFT 输出→网格（每 slot 一次）、(b) demapper 的 LLR→codeword buffer、
  (c) codeword→LDPC→MAC；每 PUSCH 的 CPU↔GPU 同步点约 5–6 个（S-5 之前约 22 个）；
- **CE 是 CPU-bound**：配对 A/B 里设备栈 `mean total=334.8`、其中 GPU 只忙 130.9 ⇒ 其余 ~204 µs 是 CPU
  （staging/编码/收尾）⇒ CE 段的下一个杠杆是减少主机 staging，不是 kernel；
- 尚未融合的 4 项候选按账排序（LDPC 80% 在第一位）。

## 3.6 终局约束（用户 2026-09-13 补充，规划时必须遵守）

**最终目标是到 MAC PDU 才出 GPU**；现在 LLR 中途交给 CPU（LLR→CPU LDPC）只是因 LDPC 性能不如意的**临时**安排。
⇒ 架构要留门（路线图 §10.11 的 D1–D8）：lane 产物定义为**设备 LLR 缓冲描述符**（不是主机 span）、
主机镜像按需生成、解扰拆成"计划/施加"、**码块+UCI 解复用要有显式布局计划**（今天藏在
`get_next_block_view` 的游标状态机里 ⇒ 设备侧必须事先算好）、LDPC 引擎留"设备切片"输入入口、
arena 预留码块 LLR 区、`[ul_gpu_lane]` 的 residency 定义到"lane 最后一条 CB"（将来自动变成 IQ→PDU）。

## 4. 之后：LDPC（真正的数量级）

每 slot（固定 16QAM 那轮）：`TF 237 + CE 336 + eq/demap 281 + FAPI 4 ≈ 858 µs` vs **`LDPC 3538 µs`**。
先做**定量剖析**（`ul_ldpc_decode` 花在哪：TB/码块数、迭代数、dispatch 次数、`max_in_flight=1` 的同步等待占比）：
现成工具 `ldpc_metal_bler_test`、`[metal_stats] ldpc_decoder`、`ul_chain_replay`。
用户怀疑 3GPP LDPC 的分层/双对角结构面向串行 ⇒ 先研究再决定重写或换并行化策略。

---

## 5. 已知坑（先读 §5 of 2026-09-13-2，本轮新增/加强的几条）

1. **两阶段/推迟类改动必须先证明"锚点不在 GPU 内存上"**：本轮第 0 步就是查这个。判据是
   "主机是否要读 GPU 写过的内存"，不是"调用顺序好不好看"。所有引擎同队列 ⇒ 顺序本身免费。
2. **同一块内存不能被两个引擎各自 wrap 成两个 Metal 对象**（"一个地址一个对象"是共享缓存存在的理由）。
   本轮把 CE 导出的 K3/K4 缓冲改走 `reserve_shared_buffer()`（进程级缓存）**并按页取整分配**
   （共享映射把长度向上取整到整页，非整页分配会被映射到自身之外）。新增会被别的引擎消费的缓冲照此办理。
3. **探针要能证伪"悄悄回退"**：两种来源产出相同软比特，所以只能靠计数器（本轮新增 `wrap failures=0`、
   `equalizer ch_est device/staged`）。测试里的设备缓冲**必须页对齐**（否则 wrap 失败退回拷贝，测试却照样通过）。
4. **主机读的跳过条件要按"该值是否真的在设备上"判断，不能按"后端能力"一刀切**：本轮踩到
   `OCUDU_CE_NO_K4=1`（K4 关）时主机 span 会留空、均衡器读到垃圾。
5. 其余照旧：环境漂移（A/B 前连测两次、不要与门禁并发）、NaN 吞误差、零拷贝 re-wrap 陷阱
   （`reserve_buffer`/`reserve_shared_buffer` 按容量预约）、缓存类优化必须用尺寸随真实链路变化的负载验证、
   S-5c-1 卡死已结构性排除（Test 4c）、K3 布局是纯算术的⇒要求连续分配、DC 契约、
   `max_ue_mcs` 语义陷阱（MCS 20 = 第一个 256QAM）、OTA 先比负载指纹再比性能、不要开 `log.all_level: debug`。
6. **Python `str.replace` 手术很危险**：本轮两次误伤（一次把 `s_ptr` 改成不存在的 `s_binding`；
   一次本以为改了页对齐实际没匹配上，差点让测试空转）。改完必须 grep 复核。

---

## 6. 本轮 OTA / 本地数据

本轮**没有做 OTA**（全是代码 + 单测门禁）。可用的本地事实：

- CE 单测两次逐位一致（Test 12 的 RE 数与相对误差与上轮完全相同：8112/3900/600/7956/312，
  nv 相对误差 1.31e-07 / 1.02e-07）⇒ 环境可复现。
- deferred chain 测试 5/5，`[metal_stats] equalizer ch_est device=36 staged=828`、`wrap failures=0`。
- 上轮的 OTA 基线/配对 A/B 见 `session_handoff_2026-09-13-2.md` §6（仍可作对比基准）。

**S-6c-1b-2 完成后建议用户跑一次 OTA**，看：软比特一致性（KO 表）、
`[mmse_time_sum] mean total` 与 `ul_channel_estimation` median 是否下降、`wrap failures=0`、
`equalizer ch_est device` 是否随 symbols 增长。

---

## 7. 本轮提交记录（`da46c753e9` → `e086ff00f8`，全部已推送）

| commit | 内容 |
|---|---|
| `c5f1648ab7` | S-6c-2a 均衡器就地读设备估计（设备 slice、kernel `h_offset`/`h_layer_stride`、共享 wrap + 页取整、设备/暂存计数器） |
| `988e69004e` | S-6c-2b 噪声方差留在设备（slice 带 `noise_var`、`consumes_device_estimates()` 能力、kernel 端口有效性判据） |
| `984b561638` | S-6c-1b-1 CSI 在解调之后再合并（`sync_device_estimates()` 钩子、adaptor 就地构造上报对象、调试抓取按需 sync） |
| `e086ff00f8` | S-6c-1b-2a 逐跳拆 `submit()`/`finish()`（`compute()` = 两者；`pending_hop_state` 保活 filtered pilots；只留最后一跳 pending；新增 `complete_fd_td_estimation_stage()` 默认空实现） |

上一轮的提交表见 `session_handoff_2026-09-13-2.md` §7。

---

## 8. 立刻可执行的开工顺序

1. **环境基线**：跑两次 CE 单测 + deferred chain 测试，确认与 §6 一致。
2. ~~**S-6c-1b-2 第 1 步**：基类两阶段拆分~~ ✅ 已完成（`e086ff00f8`，门禁 161/161 + CE 单测逐位一致）。
3. **第 2b 步（下一步）**：Metal 侧 legacy 路径改 `run_async` + 把 `unpack_engine_group()` 与 K3/K4 ready 记账
   挪进 `complete_fd_td_estimation_stage()`（参数存成一份 pending 记录；注意 `run_nn` 没有 async 形式，
   nn flavor 保持同步），门禁 = 设备路径软比特与 `OCUDU_CE_CPU_CE=1` 逐位一致 + `wrap failures=0`。
4. **第 3 步**：estimator 的 `submit()`/`finish()`/`sync_device_estimates()` + demodulator 的 sync 规则，
   门禁加上 `commits/hops`、`max_in_flight`、`hops_no_gpu=0`。
5. 完成后更新活文档（§42）并告知用户跑 OTA；随后转 **LDPC 定量剖析**（§4）。
