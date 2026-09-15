# 交接：GPU-PHY S-1a.2 ~ S-2k 会话状态（2026-09-12）

> 本文档是 compact 上下文前的完整状态记录。**新会话请先读本文档，再读
> `soft_hard_decoupled_execution_plan.md`（进度日志）与 `s2_full_chain_design.md`（设计+A-2清单）**。

## 1. 本会话完成的提交（全部已推送 apple-silicon）

| commit | 内容 | 状态 |
|---|---|---|
| `16eeba76b1` | S-1a.2 批量 DFT：`run_batch()` + 槽解调器批处理（**保留但惰性**，见 §5） | 已验证 |
| `e356f0d287` | S-2c 共享 Metal command queue + DFT 不等待提交 `submit()` | 已验证 |
| `b4cca140cc` | S-1a.3 修正批处理适用范围（仅"独立 transform 流"：多端口/多载波） | 注释 |
| `2254e2c6bc` | S-2d DFT ring 提交（in-flight slot + `wait_slot()`） | 已验证 |
| `b1d78b0836` | S-2e **RX 逐符号提交 + lag 化 finish/通知**（puxch 流水线） | E2E ✅ |
| `283e215f4b` | S-2f DFT 等待统计修正 | 已验证 |
| `d92d596eff` | S-2g **前端/后端双队列**（修队头阻塞） | E2E ✅ |
| `9291ffad3f` | S-1c.1 CE 聚合耗时报告 `[mmse_time_sum]` | 已验证 |
| `36995378c5` | S-2h 接口 `submit/wait/get_post_eq_sinr/supports_deferred_chain`（默认实现） | 已验证 |
| `da71a67aa1` | S-2i 均衡器延迟提交 `submit()/wait()` | 单测 ✅ |
| `7c60bbf038` | S-2j `pusch_demodulator_impl` 接入延迟均衡（A-1） | E2E ✅ |
| `f1d6d695d2` | S-2k 解调器延迟提交 + 引擎 batch API | 单测 ✅ |

## 2. 实测收益（OTA，ping 轻载档 = 唯一可信对比口径）

| 指标 | 最初 | 现在 | 说明 |
|---|---|---|---|
| `ul_time_frequency` | 1221–1788 µs | **203–233 µs** | S-2e FFT 流水线，**6×** |
| `ul_channel_estimation`（中位） | 430–545 µs | 397 µs | 不变（CE 本体健康） |
| `ul_equalization_demod` | 2704 µs | **2474 µs** | S-2j 仅 −8.5%（原因见 §4） |
| `ul_ldpc_decode` | 1743–3086 µs | 2135 µs | 未动 |
| `ul_pipeline` | 6881–8352 µs | 6019–6324 µs | — |

ZMQ 同向：`t2f` 2597 → 635 µs，`pipeline` 10035 → 5986 µs，`CE` 465 µs。

**机制证据**：`[metal_stats] dft commits == waits`、`max_in_flight = 8`（ring 流水线）。

## 3. 关键代码位置（新会话必读）

| 功能 | 文件 |
|---|---|
| 双队列（前端=DFT，后端=CE/均衡/解调/LDPC） | `lib/phy/metal/ocudu_metal_queue.{h,mm}` |
| DFT ring 提交 + 批量 + `run_async/wait_slot` | `lib/phy/generic_functions/metal/{ocudu_dft_metal_engine.*, dft_processor_metal.*, ocudu_dft.metal}` |
| RX 流水线：ring + lag 化 finish/通知 + 槽末 drain | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.{h,cpp}` |
| OFDM 解调器：`submit_symbol/finish_symbol`、`get_pipeline_depth()`、`demodulate_batch()` | `lib/phy/lower/modulation/ofdm_demodulator_impl.{h,cpp}` + 接口 `allowlist...ofdm_demodulator.h` |
| 均衡器延迟提交 | `lib/phy/upper/channel_processors/metal/channel_equalizer_metal.*` |
| 解调器延迟提交 + batch 引擎 | `lib/phy/upper/channel_modulation/metal/{demodulation_mapper_metal.*, ocudu_demod_metal_engine.*}` |
| PUSCH 解调链接入（A-1 门控 + SINR 后移） | `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp` |
| CE 聚合耗时探针 | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp` |

## 4. 修正后的成本模型（重要！决策依据）

**eq+demap 阶段**（OTA，2474 µs/槽，42.3 次 dispatch = 21.2 eq + 21.2 demap）：

```
每次 dispatch ≈ 58.5 µs  = 等待 20–40 + 提交/编码 5–10 + GPU 执行 + CPU staging(bf16 memcpy 2–5)
```

- **孤立探针里的 101.5 µs/次是"完整往返"**（含队列调度+线程唤醒），**不能**直接用于流水线内估算
  ⇒ A-1 只省 8.5% 就是这个原因
- **杠杆 = 减少"等待次数"**（42 → 1~2），而把多次 dispatch 塞进一个 CB **不减少** dispatch 本身开销
- 实测支撑：同内核 16 次收进单 CB = 12.6 µs/次（孤立）、逐次 commit+wait = 101.5 µs/次

## 5. 惰性/未启用的部分（不要误判为 bug）

1. **槽解调器批量路径**（`demodulate_batch`、`run_batch`）：gNB RX 走 `puxch` 的逐符号路径，
   批处理只在 `get_max_batch()>1` 时生效；保留给多端口/多载波，代码内有 TODO 说明
2. **解调器 `submit()/wait()`**（S-2k）：已实现并单测通过，但**调用方尚未使用**（路线 1 才用）
3. **`get_post_eq_sinr()`**：metal 仍返回 false（走 CPU 遍历），位置已正确、非瓶颈

## 6. 已查清的"非问题"（避免重复排查）

| 现象 | 结论 | 证据 |
|---|---|---|
| iperf3 下 CE 阶段 14–32 ms | **上层 PHY 排队延迟记在"交接后第一个阶段"（CE）上**；每槽真实工作量 5.5 ms vs 槽长 1 ms | CE 自测恒定 314 µs/次、`hops_gpu=4150`、`fb_blocks=0`；与流水线开关/基线/射频增益无关（详见 `s2_full_chain_design.md` §7） |
| UHD overflow 3655 条 | **结果**而非原因：处理跟不上→射频溢出 | 0.1 Mbps 时仅 7 条，CE 仍 20 ms |
| `rx_gain` 50/60 调整 | 无效（50 断链、60 仍慢） | — |
| 饱和态各阶段探针 | **不可信**（测的是含排队的跨度） | 方法论写入 §7 |

## 7. 下一步（按优先级）

### A-2 路线 1（K 组批量）——**清单见 `s2_full_chain_design.md` §11，可直接执行**
1. `pusch_demodulator_impl` 缓冲扩到 K 个符号（K=7；**必须同时把每符号 `first(n)` 改成按符号取偏移**）
2. 主循环改成"按 K 个符号分组、组内三趟"：pass1 全组 eq `submit` → pass2 全组 demap `submit`
   → **一次 wait** → pass3 逐符号 SINR/EVM/解扰/统计/`on_new_block`
3. 核对不变式：`on_new_block` 顺序、provisional stats 时机、EVM 只在 wait 后读、非延迟路径逐字节不变
4. 验证：`ctest -L phy` 全绿 → ping 轻载档 E2E（预期 `ul_equalization_demod` 0.8–1.3 ms）
5. 回退：K=1 即回到 A-1 行为

### A-2 路线 2（稍后）：解扰下沉 GPU（PRG 序列由 CPU 生成上传）→ 彻底消除逐块 CPU 消费

### LDPC（用户决定稍后）
- 已证：逐 TB GPU 译码无解（per-dispatch 下限 8 µs vs CPU 7 µs/TB）；星形修复分组上限
  已证为 BG1 46→13、BG2 42→16（净 ~2–2.6×）；persistent 受 1 threadgroup 限制；
  候选：星形修复 / 按槽批量译码 / 维持 CPU 译码
- **若目标 iperf3：LDPC 的 2.1 ms 是压到 1 ms 以内的必要条件**

### FFT Phase 2（最低优先）：kernel 内做相位/窗补偿 + 直写 grid（收益 <0.1 ms/槽）

## 8. 调试开关与操作要点

| 开关 | 用途 |
|---|---|
| `OCUDU_DFT_PIPELINE_DEPTH=1` | 关闭 RX FFT 流水线（二分定位用，无需重编译；默认 8） |
| `-DENABLE_CE_TIME=ON` + 控制台 stderr | `[mmse_time_sum]` 聚合 CE 耗时（**不受日志级别影响**）；逐 hop `[mmse_time]` 需 `lib_level: debug`（量极大，慎用） |
| `-DENABLE_METAL_STATS=ON` | `[metal_stats]` 各引擎 commits/waits/max_in_flight |
| 构建 | 改 `.metal` 后 metallib 会在构建时重新生成；**E2E 前务必 `cmake --build build -j8`** |

**约定**：提交信息用 `GPU-PHY S-<id>: ...` 前缀；**不得引用 `doc_chinese/`**（该目录 git 未跟踪）；
Metal 后端选择走 `expert_phy` CLI（env 变量仅调试）；TX（PDSCH/PDCCH/SSB IFFT）保持 CPU 不动。

---

## 附录 A：2026-09-13 会话（A-2 路线 1 = K 组批量）——已完成，待 E2E

> 详细实现记录见 `s2_full_chain_design.md` **§12**（含两处 §11 未预见的硬约束、A-1 SINR 回归、
> 单测覆盖、GPU 探针数据表）。

**本会话新增提交**：`GPU-PHY S-2l: deferred K-symbol PUSCH chain (route 1)`（含金属适配器
FIFO 多笔 pending、解调器 LLR 页对齐直写、调用方三趟分组、CPU 等价性单测、`metal_chain_probe`）。

**要点**：
1. **三处必须一起改**（否则会静默出错或崩溃）：
   - 金属均衡器只支持一笔 pending + 一套 h/y/s staging → 改为 FIFO + 每笔独立 staging；
   - 金属解调器 LLR 目标页对齐时直写（`run_demodulate` 自动判定），省掉暂存与回拷；
   - 调用方 pass2 **只能每符号一个 dispatch**——码字缓冲的 `get_next_block_view()` 在
     `on_new_block()` 之前不会前进，逐块提交会全部写进同一块内存；分块搬运放到 pass3。
2. **修掉 A-1 的 post-eq SINR 回归**：延迟路径原先在统计通知/累加之后才做 SINR 归约，
   导致 SINR 恒为 `inf` 且总计丢失。生产默认 `pusch_sinr_calc_method = post_equalization`，
   所以此前 OTA 的 UL SINR 上报一直是错的（不影响 CRC/解码）。现已与串行路径一致。
3. **单测抓到的越界 bug**：分组循环写成 `group_begin != group_stop`（步长 7），
   `nof_symbols` 非 7 倍数时会越界处理（崩溃）。OTA 常用 14 符号恰好整除，不会暴露。
4. **GPU 探针实测**（`metal_chain_probe`，同进程 A/B）：K=7 相对逐符号链 **4.0–6.1x**
   （OTA 档 25PRB：A 1757–2728 µs/slot → B 435–445 µs/slot；20MHz/106PRB/2x2 同样 ~450 µs/slot），
   两种模式 LLR **逐位一致**。新下限是 CPU 每符号编码 ~31 µs。
   K 扫描：1→2033、2→921、3→747、7→451、14→357 µs/slot（K=14 仅再快 ~18%，默认仍取 7）。
5. **预期 E2E**：`ul_equalization_demod` **2474 µs → 450–800 µs**（4–5x）；
   `[metal_stats]` 应显示 demapper/equalizer 的 commits ≈ 14、waits ≈ 2（每槽），
   与 A-1 的 "commits ≈ waits ≈ 14×2" 形成对比。

**回退手段**：`OCUDU_PUSCH_DEFERRED_GROUP=1` → 退回 A-1 行为（无需重建）；
`=2/3/5` 可逐步二分；默认 7，clamp 到 [1,7]。

**附录 A 补记（同日，第一次 E2E 之后）**：第一次 E2E 无变化，定位到**四层包装没有转发
`submit/wait/supports_deferred_chain`**（两个金属"metal-or-generic"工厂适配器 + 两个 metrics 装饰器），
`supports_deferred_chain()` 因此恒为 `false`，**延迟链在 OTA 里从未生效**（A-1 的 −8.5% 实为
页对齐原地写均衡输出的收益，不是减少等待）。已全部转发，并新增回归测试：
`tests/unittests/phy/metrics/phy_metrics_deferred_chain_test.cpp` + 两个金属单测的
`composite factory` 用例。详见 `s2_full_chain_design.md` §13。
**判据**：`[metal_stats]` 里 demapper/equalizer 应变成 `waits ≈ commits/7`（每槽 commits≈数据符号数、waits≈2）。

**下一步（优先级不变）**：E2E 出数后 → LDPC（GPU 解码 1.4–1.7 ms vs CPU 8–18 µs，见 PLAN.md 决策数据）
→ 路线 2（解扰下沉，可再消掉 pass3 的逐块搬运）。


---

## 附录 B：2026-09-13 续（延迟链 OTA 失败根因）

`380f6b2cf2`（修复）+ `db4866d924`（探针与诊断）。**根因**：延迟链让解调器读均衡器输出时
只依赖共享队列的顺序，而 Apple GPU **允许同一队列的 command buffer 重叠执行**，并发
demodulator 实例下解调器会读到未写完的 `eq`/`nv` → LLR 用错符号算出（SINR 仍正常，CRC KO），
同形状同 SINR 下串行 24% 成功、延迟链 2%。**修复**：调用方在两组 burst 之间显式
`equalizer->wait()`；两个引擎改为逐个等待所有未回收的 command buffer。
**复现**：`pusch_demodulator_deferred_chain_test` 的并发用例（修复前 9–16/60 失配，修复后 0/60）。
**代价**：探针 591 → 800 µs/slot（串行基准 ~1500–1900），预计 OTA 650–800 µs。
详见 `s2_full_chain_design.md` §14。

---

## 附录 C：2026-09-13 续（`t=` 3.3–5.7 ms / CN ping 6/500 的真根因）

`921c81cbc8`（修复）+ `5b83736eca`（注释）。**根因**：`puxch_processor_impl` 的**流水线路径**
把 in-flight 队列入队为 **(symbol, port) 对**，而 `finish_oldest_symbol()` 每弹出一个条目就
`notifier->on_rx_symbol()` 一次 ⇒ **每个 port 通知一次上层 PHY**。两个后果：
① 上层 PHY 把同一 PDU 处理两遍（2 RX port）⇒ 服务时间 ≈×2 ⇒ ρ>1 ⇒ 排队 ⇒ `t=` 3.3–5.7 ms；
② port 0 通知时 port 1 还没写进 grid ⇒ **PUCCH 2 symbol/2 port 的 HARQ-ACK 被破坏** ⇒ DL 收不到
ACK ⇒ HARQ 丢 509 TB、MCS 25–27 ⇒ DL 崩 ⇒ CN ping 6/500。
**修复**：`last_port = (i_port + 1 == nof_rx_ports)`，只在最后一个 port 完成时通知（FIFO 保证顺序）。
**回归测试**：`FlowPipelinedNotificationPerSymbol`（1/2/4 port × 24 组合）修复前 16 FAILED、
修复后 24 PASSED；`ctest -L phy` 161/161。
**待重跑**：全 GPU 配置**不带** `OCUDU_DFT_PIPELINE_DEPTH=1`，预期 CN ping 恢复、`t=` 回到亚毫秒。
若仍高，按 `s2_full_chain_design.md` §16 的三条候选逐步定位（先扫 `OCUDU_DFT_PIPELINE_DEPTH=2/4/8`）。
