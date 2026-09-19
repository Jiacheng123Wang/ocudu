# 会话交接（2026-09-15 晚，stamp `abdb910f60`）—— ② 分两段施工的起点

> 快照，不再修改；后续更新写进 `s2_full_chain_design.md`。
> 上一份：`session_handoff_2026-09-15-8.md`（S-7f-5y）。本份覆盖 **S-7f-5z / 5y / 6a / 6b / 6c(失败) / 6d** 这一段。

## 1. 当前状态（先看这 8 行）

| 项 | 值 |
|---|---|
| HEAD | **`abdb910f60`** "tools: the TD replay dedupes the capture, and is not a gate yet" |
| `gnb` stamp | `abdb910f60`（`strings build/apps/gnb/gnb \| grep -c <sha>` == 2 核对过）|
| 工作树 | `lib/`、`include/` **干净**（`git status --short` 空）|
| **PHY 行为** | **等同 S-7f-6b**：① 设备写网格（`--expert_phy.device_resource_grid on`）+ ③ G7 都在，**② 已回滚** |
| 上机状态 | **可正常接入**：`rrcSetupRequest=8`、`RRC Setup finished=2`、`PUSCH crc=OK=2005 / KO=529`、`RLF=0` |
| 下一步唯一入口 | **`s2_full_chain_design.md` §48.157**：② 分两段施工，**下一轮只做 1–3（修路）**，再做 4（通车）|
| 主门 | 离线 `[reuse]` 守卫测试（已就绪）+ 上机判据（见 §5）|
| 忌讳 | 未经上机验证就启用"内核直读调用者缓冲"——本会话已经因此让手机接不上一次（§3.5）|

## 2. 启动命令（与 S-7f-6b 腿逐字相同）

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --expert_phy.device_resource_grid on \
  --log.all_level info --log.filename /tmp/gnb_<腿名>.log
```

console **不要重定向**（`[metal_stats]` / `[mmse_time_sum]` / `[ul_gpu_lane]` 在 stderr）。
`debug` 级会到 100+ MB/分钟（本会话出现过 176 MB / 2 分钟），只在定点抓问题时开。
失败率分母用**墙钟时隙**（`elapsed × 1000`，elapsed = 首行 `Built in Release mode` 到 `Stopping...`），
**不要**用 `[ul_pipeline] samples`。参考：0.0018%–0.0116%，预算 0.1148%。

## 3. 这一会话做了什么（每腿：改了什么 + 判据 + 上机结论）

| 腿 | commit | 一句话 | 判据/结果 | 上机 |
|---|---|---|---|---|
| S-7f-5z | `f2299bb29d` | **G4**：设备 LS 导频不再回拷宿主，消费者按"域"取数（`ls_pilot(args,sym,lay,j,scaled)`）| 与上腿二进制对拍 **30/30 + 7/7×6 逐字节相同**；`cpl_unpack` 0.45→0.20、`cpl_fill` 0.20→0.15 µs | ✔ PASS |
| S-7f-6a | `0af4fe3616` | **G6**：宿主估计按需解包（完成时只解 DM-RS 符号，其余等 `get_symbol_ch_estimate()`）| 10 配置 × 30 捕获 × 4 dump 逐字节相同；`cpl_unpack` 0.45→0.20 | ✔ PASS |
| S-7f-6b | `96d7ac5d4f` | **③/G7**：删死拷贝 `stats_in.pilots_lse`（`consumes_pilots()` 默认 true，v1 fixed 估计器返回 false）+ **① `--device_resource_grid on`** | 7 配置 × 30 捕获逐字节相同；`[phy_pipeline] … device_grid=yes`、0 条回退告警 | ✔ PASS（PUSCH OK 823）|
| S-7f-6c | `e16df70717` | **②**：DFT 内核直读射频 int16 样本（去 CP + 定标进内核）| 离线单测/端到端**全绿**（`mismatching=0`）| ✘ **失败**：手机接不上（§3.5）|
| （回滚）| `f090524fab` | revert ② | 源码逐字节回到 S-7f-6b | ✔ 恢复（§3.5 复测）|
| S-7f-6d | `028a27647e` | **守卫测试** `[reuse]`：提交后立刻覆写样本，网格必须仍逐字节正确 | 在"环拷贝"设计上 PASS、在"直读"设计上必 FAIL | — |
| 工具 | `7d63e3cb0a`、`abdb910f60` | TD 回放：变换长度由 CP 推导（552→512）、`--device-grid`、按 `(slot,symbol,port)` 去重 | 见 §6.3（该模式**仍不确定**，暂不作门）| — |

**胶水总账（§48.138(b) 的原始清单）**：G5（每跳 wrap 重映射，S-7f-5x）✔关；G3（host pre-stage 重算 LSE，S-7f-5y）✔关；
G4（回拷 + 就地缩放，S-7f-5z）✔关；G6（完成回读，S-7f-6a）✔关。**⇒ 清单已清空**，
但 §48.152 的**代码级复核**指出：IQ 入口仍有多处宿主过手（见 §4）。

## 4. 用户的目标与"还差什么"（§48.152 的复核结论）

目标：**IQ samples → demapper LLR 全程 GPU，CPU 不参与数据处理**。当前实况：

| # | 阶段 | 宿主是否碰"值" | 状态 |
|---|---|---|---|
| 1 | IQ → DFT 输入：去 CP + ci16→cf_t 定标 | **碰（逐样本）** | ② 想消灭的就是它；**已回滚，待重做**（§48.157）|
| 2 | DFT 输出 → 网格 | **曾经碰（逐 RE）** | ✔ 已由 ① 消灭（设备写网格，上机验证）|
| 3 | CE：DM-RS 参考 staging + `pilots_power` 归约 | **碰** | 未做（归约小；参考 staging 是每跳的常数表）|
| 4 | 相关模型（A/R_hp/W）| 不碰值（只传统计量）；矩阵求值在设备 | ✔ |
| 5 | 均衡 / 解调 | 不碰 | ✔（`ch_re/ch_est device=… host=0`、`LLRs written in place`）|
| 6 | demapper 之后（LLR 域）：`memcpy` + 逐 bit 解扰 + SINR/EVM 归约 | **碰** | 按 MEMO 的次优方案，LLR 交回 CPU 是**有意为之**（LDPC 在 CPU）|
| 7 | **上游还有两处**（§48.154(e)）：样本拷进 `temp_buffer`（FSM 对齐）+ 每样本 ci16→cf_t→**CFO**→cf_t→ci16 | **碰** | 与 **PRACH 共用**补偿后样本 ⇒ 有硬约束，独立课题 |

## 5. 下一轮的任务（唯一入口：§48.157）

### 5.1 只做 1–3（"修路"，行为不变）

不变式：**一个样本缓冲，只有在读过它的变换们全部完成后，才可以被重写。**

| # | 文件 | 改动 |
|---|---|---|
| 1 | `lib/phy/lower/processors/uplink/uplink_processor_impl.{h,cpp}` | `temp_buffer`（单缓冲，`:135`）→ **8 个槽**（`ofdm_symbol_demodulator_impl::max_pipeline_depth`），符号 S 装配进**获配的槽** |
| 2 | `lib/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h`（及实现）| 新增 `unsigned acquire_symbol_slot()`（环满时先 `finish_oldest_symbol()`）；`process_symbol(reader, ctx, slot)` |
| 3 | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp` | 环槽由"端口循环内自算"（`:121-135`）改为"每**符号**获配 `nof_rx_ports` 个连续槽"，端口 i 用 `base + i` |

**两个必须处理的坑**：① 一个符号的多个端口读**同一份**装配缓冲 ⇒ 释放判据是"整组端口的变换都完成"（推荐在 `acquire` 里一次等掉整组，`finish` 记账保持现状）；② 对齐缓冲 `resize()` 不重分配 ⇒ 每槽按 `2 * dft_size`（4096 B/channel，页整数倍）构造一次。

**禁止的简化**：把源缓冲变成 1 深（= 把 DFT 从"与电台重叠"改回串行，71 µs 符号周期里吃掉抖动余量 ⇒ 威胁硬约束）；"只搬不转"（ring 槽存 ci16 + 宿主 memcpy）只能当过渡，不算达标。

### 5.2 然后才做 4（"通车"）

`ofdm_symbol_demodulator_impl::submit_symbol()` 把 `fill_dft_input(...)` 换成 `submit_grid_write(..., time_input)`——
**内核/引擎那一半已经写好并被验证过**（见 §6.2），回滚只回滚了"调用它"这件事。通车时：
`time_window_start = cp.get_length(symbol_index, scs).to_samples(rate) - nof_samples_window_offset`；
`time_gain = 1.0f / ocuduvec::scaling_factor_ci16_to_cf`；引擎会 wrap **整个分配**并把"切片偏移 + 窗口起点"当数字传给内核。

### 5.3 验收（每段各自可验）

| 段 | 门 |
|---|---|
| 1–3 | `[reuse]` 守卫 + 六门 + `ctest -L phy` + replay 对拍 **全部不变**；上机：`PUSCH crc=OK`/失败率与 S-7f-6b 同量级、`RLF=0`、**`dft radio_inputs` 仍为 0**（还没通车，这是**空转判据**）|
| 4 | `[reuse]` 仍须 PASS；上机：**`[metal_stats] dft … radio_inputs=` > 0**（=0 即退回宿主）、**不出现** `OFDM demodulator: the transform input cannot be read from the radio buffer…`、`PUSCH crc=OK` 同量级、失败率不退化 |

## 6. 现场与工具（都在这台机器上）

### 6.1 离线门的现场
- **语料**：`/tmp/iq1_*_ce.txt` + `/tmp/iq2_*_ce.txt` = **237 个捕获**（`dmrs_symbols={2,7,11}`、`nof_tx_layers=1`、`alloc_nof_rb` 3–14）。**/tmp 不抗重启**；没了就用 `lib/.../metal/make_synthetic_capture.py` 造（`combos` 需要真语料）。
- **六门**：`bash /tmp/run_gates_s7f5x.sh <输出目录>`（内部调 `lib/.../metal/capture_gates.sh <mode> 6`，跑 `sig2 k0dm k0d ydev k1 combos`；全跑约 70 秒）。门在重并行下会给出**错的**结果 ⇒ 任何 mismatch 都要串行复检（脚本内建），`combos` 是逐组合串行。
- **二进制对拍**：`bash /tmp/ab_ref.sh <参考二进制> [开关]`（比 `_llr.bin`/`_ce.txt`/`_h.bin`/网格，30 捕获）。**改代码前先 `cp build/lib/phy/upper/channel_processors/metal/ul_chain_replay /tmp/<腿>_ref`**——这是本会话最有效的一招。
  现存参考：`/tmp/ul_chain_replay_s7f6a_ref`（S-7f-6a；③ 之后仍然 30/30 相同，因为 G7 中性）。
- **soak**：`/tmp/soak8.txt` 是 8 个捕获前缀；命令见 §48.140(a)（`--also … --repeat 20 --metal --out /tmp/wb`）。
- **L1 harness**（CE 内核 vs 真宿主，10 几何 + 拒绝探针）：`doc_chinese/full_gpu_chain/wip/s7f5x_l1_{harness.mm,build.sh,run_all.sh}` + README（已从 /tmp 抢救进树）。
- **CE 单测**：`build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test`（含 Test 8/8c 矩阵 flavor parity）。

### 6.2 ② 那一半已经写好且被验证过的代码（回滚前）
- 内核 `lib/phy/generic_functions/metal/ocudu_dft.metal`：`input_params{is_ci16,offset,gain}` + `ocudu_load_input()`（**逐位复现**宿主 `float(x)*(1/32767)`）。
- 引擎 `ocudu_dft_metal_engine.{h,mm}`：`grid_write::time_samples/time_samples_bytes/time_window_start/time_gain`；wrap **整个分配**（`describe_aligned_allocation`）；`[metal_stats] dft … radio_inputs=` 空转计数；拒绝时一次性告警并让调用者回退。
- 接口 `include/ocudu/phy/generic_functions/dft_processor_grid_write.h` 的四个字段；`dft_processor_metal.cpp` 的逐字段转发（**当年就是漏了这里**）。
- 测试：`dft_processor_metal_unit_test` 的 `[ci16]` 用例（页对齐分配 + 前置 CP + **引擎环已填垃圾**）；`ofdm_demodulator_metal_batch_test` 的 `[reuse]` 守卫。
- **重新启用这些代码的方式**：`git revert f090524fab`（即恢复 `e16df70717` 的内容），然后按 §48.157 只取其中的内核/引擎/接口部分、**不要**取调用者那一半。

### 6.3 TD IQ 对拍通道（暂不可用，但材料在）
- 录制：`sudo env OCUDU_UL_DUMP_TD=/tmp/td1 OCUDU_UL_DUMP_TD_SLOTS=4 ./build/apps/gnb/gnb …` ⇒ `/tmp/td1_td.{txt,bin}`（本会话已录：4 时隙 × 14 符号 × 1 端口，**每时隙两遍**，工具已按 `(slot,symbol,port)` 去重）。
- 回放：`ul_chain_replay /tmp/td1 --dft-metal [--device-grid] --out <前缀>` ⇒ `<前缀>_<slot>_dft.bin`（14 符号 × 300 子载波 cbf16）。
- **已知缺陷（工具侧，与 PHY 无关）**：设备写路径**同二进制跑两遍只有 2/4 相同**；工具的**宿主**网格路径只写出 4/14 个符号。⇒ **该模式目前不能当门**，定位方向写在 §48.156(c)。抓取文件保留。

## 7. 纪律（每条都付过代价，§48.146 有全表）

1. **门要串行复检**；`combos` 不自我复检。
2. **改门之前先测量**（容差不是旋钮）。
3. **"配方"也要先证伪再施工**（§48.142(c) 的配方照做修不掉 G5）。
4. **删 CPU 步骤要问两件事**：它的输出谁读，**它还给谁设了状态**（S-7f-5y 的 `args.cfo_hop` 就是这样被漏掉的）。
5. **改动前先另存上一版二进制**，改完对拍全部发布物（LLR/CE/h/网格）；对拍开关集**必须覆盖 `combos` 门的全部开关**（我漏了 `SPLIT_TAIL`，是门抓到的）。
6. **给"逐字段转发"的 API 加字段时，编译器不会提醒你漏了转发**（`dft_processor_metal.cpp`）。
7. **"结果是零"既可能是没跑，也可能是读错地址**：测试里要写"故意填垃圾/故意覆写"的用例把两者分开。
8. **GPU 运行一律限时**（后台 + 定时 `kill -9`）；挂死只能强制断电；跑完查 `ioreg -r -c IOAccelerator -d 1`。

## 8. 教训（这一会话最贵的一条）

**这次事故的因果链**：DFT 每符号提交一次、ring 里最多 8 个在飞 → 旧路径靠"提交时把样本拷进私有 ring 槽"保证正确 →
我让内核**直读**调用者的装配缓冲（那是**一个**、每符号被覆盖）→ 在飞的变换读到别的符号 → 网格垃圾 →
**PRACH 正常（宿主时域检测）而 PUSCH 全灭（`crc=OK=0/630`）** → 手机接不上，gNB"没反应"。

**为什么离线全绿还是漏了**：我的两个测试都**从不在变换在飞时复用样本缓冲**。不是写得随意，而是**没想到这条契约**——
`time_samples` 的文档只写了"不拷贝"，**没写"调用者必须保证它在变换完成前不被改写"**。

**因此下一次做同类改动（"设备直读调用者缓冲"）的三条硬要求**：
1. 先写下**生命周期契约**（谁保证缓冲在多久内不被改写），再写代码；
2. 离线测试必须**模拟调用者的真实复用模式**（`[reuse]` 就是这个模式的固化）；
3. 上机判据里必须有**空转计数**（`radio_inputs>0`）和**"路径级"症状对照**（PRACH 正常 + PUSCH 全灭 = 网格错）。

## 9. 环境事实（免得重新踩）

- 仓库 `/Users/jiachengwang/dev/ocudu`；构建 `cmake --build build --target gnb -j 14`；**stamp 是 configure 期生成** ⇒ 改完 commit 后要 `cmake -P build/build_info.cmake` 再重建，才与 commit 一致。
- `.metal` 改动会被构建重编成 `lib/.../metal/ocudu_*.metallib`（**源码目录里**，运行时优先加载它）；**改了内核要重建引擎目标**，否则你跑的是旧 metallib（本会话被这个"混搭"骗过一次）。
- 手机侧：CN 端 `ping 10.45.0.x -i 0.2 -c 100` + `iperf3 -c 10.45.0.x`（下行）；gNB 侧 `[ul_mac_pdu_size] total` 是上行的等价证据。
- 文档：`doc_chinese/full_gpu_chain/s2_full_chain_design.md`（**货文档**，随时更新；本会话新增 §48.143–§48.157）；`session_handoff_*.md` 是快照，不修改。
