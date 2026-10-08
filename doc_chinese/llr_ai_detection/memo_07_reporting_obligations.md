# memo 07 —— 上报义务清单（★ **A1 + A4 已完成**）

> **状态**：✅ **完成**（代码级，逐条 `文件:行号`）。
> **对应功课**：主规划 §9.2 的 **A1（必做）** + **A4（并入 A1）**。
> **产出**：本文 = "**CE 到底欠了谁什么**"的权威清单，**深度 3 的最小上报集合由此确定**。
>
> ★ **本 memo 只读代码，未飞腿、未跑测试** —— 每个结论都指向一个提交里的 `文件:行号`，
> 复现命令见 §9。★ **它不含任何性能数字**，因此不受"数字前必须全量构建"那条纪律约束
> （`memo_10` §7.1 管的是**测量值**）。
>
> 版本：v1.0 ｜ 日期：2026-10-08 ｜ 变更：v0.0（占位）→ v1.0（内容落盘）

---

## 0. ★★ 一页结论（先读这一节）

| # | 结论 | 对深度 3 的后果 |
|---|---|---|
| **C1** | ★★ **TA 是闭环控制量，不是上报量。** CSI 的 `get_time_alignment()` → FAPI `timing_advance_offset` → 调度器 `n_ta_diff` → **触发 TA_CMD MAC CE 下发给 UE** | ★★ **深度 3 必须继续产出可用的 TA**（或保留一个轻量 DM-RS 经典块）。这是本 memo 最重要的结论，**A4 的答案是"被用于补偿"** |
| **C2** | ★★ **后均衡噪声（不是 DM-RS 噪声）是唯一的链路自适应输入。** `SINR_dB = -10·log10(mean(σ²_post))` → PUSCH SNR → **UL MCS** + **UL OLLA** + TA 有效性门 | ★ 噪声头是**必需**，且必须**逐 RE**输出（平台契约 `[data_re, layer]` 与此一致）。★ **全零/近零常数会得到 `sinr = +inf`**（代码里有一条显式的 `infinity` 分支） |
| **C3** | ★★ **PUSCH 路径上 CFO 从未被写进 CSI。** `set_cfo` 在 `lib/phy/upper/channel_processors/pusch/` 下**一次都没被调用**；CSI 的 `cfo_Hz` 永远是 `nullopt` | ★ **CFO 不是上报义务** —— 报出去的路径不存在 |
| **C4** | ★★ **但 CFO 在估计器内部被消费：它是 DM-RS 信道估计的相位补偿量**（受一个开关控制）。★ **本仓库默认是关的**（`pusch_channel_estimator_cfo_compensation = false`）| ★ **不是义务，但必须显式记账**：深度 3 换掉估计器后，这个"开关的意义"要有等价物或被判为无意义。见 §4.3 |
| **C4bis** | ★ **硬件 CFO 补偿（RU 层）与 CSI 的 CFO 字段没有任何连接**：由 NTN 多普勒适配器或操作员命令独立驱动 | 与 C4 分开：**不能**把"RU 能补 CFO"当成"估计器的 CFO 没被用" |
| **C5** | ★ **RSRP 与 EPRE 有真实消费者，且走的是"端口平均"这一份**（`get_rsrp_dB()` → FAPI `rsrp` → MAC `ul_rsrp_dBFS` → 调度器指标 → E2 KPM；EPRE 被填进 FAPI 的 `rssi` 字段）。★ **逐端口的那一份（`get_port_rsrp_dB`）没有生产消费者** | ★ 深度 3 的输出必须能支撑**端口平均 RSRP 与 EPRE**，否则调度器指标与 E2 上报退化 |
| **C6** | ★ **`get_rsrp_all_ports(tx_layer)` 与 `get_layer_average_snr(tx_layer)` 没有外部消费者** —— 它们只被 `get_channel_state_information()` 自己调用 | 这两个 getter **不是**上报义务，**不要**为它们设计头 |
| **C7** | ★ **深度 3 的最小上报集合 = {逐 RE 后均衡噪声、RSRP（端口平均）、EPRE、TA} + {信道估计与 DM-RS 噪声这两个内部输入}**；`sinr_ch_estimator` 只在配置非默认时才是义务，**CFO 与逐端口 RSRP 不是** | 见 §7 的规格表 |
| **C8** | ★★ **每一个"必须有值"的量都有一条"没值就静默降级"的路径** —— `nullopt` 不会报错，只会让 FAPI 少填一个可选字段 | ⇒ **必须加"缺字段即计数并告警"**，否则深度 3 的出问题方式就是"**什么都没发生**" |

---

## 1. 上报路径全链（逐跳，`文件:行号`）

```
① CE 测量
   lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.cpp:358
   dmrs_pusch_estimator_impl::get_channel_state_information(csi)   ← 把 5 个量写进 CSI
        │
② 合并进本次传输的 CSI 对象（★ 在 demodulate 之后，为了不把 demod 压到估计的同步点后面）
   lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp:566-567
   (void)est_results.sync_device_estimates();
   est_results.get_channel_state_information(notifier_adaptor.get_channel_state_information());
        │
③ 解调器先把 post-EQ SINR / EVM 写进同一个对象（不同字段，互不覆盖）
   lib/phy/upper/channel_processors/pusch/pusch_processor_notifier_adaptor.h:152-171
        │
④ 随结果打包：UCI 路径与 SCH 路径各带一份 CSI 的**值拷贝**
   ...notifier_adaptor.h:256  (on_sch)   →  ...:318 (on_uci)
   lib/phy/upper/uplink_processor_impl.h:75  (on_uci)  /  :106 (on_sch)
        │
⑤ PHY → FAPI 翻译（**生产路径**）
   lib/fapi_adaptor/phy/p7/phy_to_fapi_results_event_fastpath_translator.cpp
     :176-192  UCI PUSCH PDU   → sinr / TA / rssi(=EPRE) / rsrp
     :232-254  CRC.indication  → sinr / TA / rssi(=EPRE) / rsrp
        │
⑥ FAPI → MAC 翻译
   lib/fapi_adaptor/mac/p7/fapi_to_mac_indications_fastpath_translator.cpp:113-127
     pdu.ul_sinr_dB / pdu.ul_rsrp_dBFS / pdu.time_advance_offset
        │
⑦ MAC → 调度器
   lib/mac/mac_sched/ocudu_scheduler_adapter.cpp:396-417 (CRC) / :429-433 (UCI)
        │
⑧ 调度器消费（★ 见 §3、§4）
```

**关键结构性事实**：③ 里解调器写的是 **post-equalization** SINR，
而 ① 里估计器写的是 **channel_estimator** SINR。两者写进**不同的 `optional` 字段**，
最后由 `channel_state_information::get_sinr_dB()` 按 `sinr_report_type` **选一个**
（`include/ocudu/phy/upper/channel_state_information.h:67-78`）。

★ 工程默认值是 **`post_equalization`**：
`apps/units/flexible_o_du/o_du_low/du_low_config.h:68`
`std::string pusch_sinr_calc_method = "post_equalization";`
（合法性检查在 `du_low_config_validator.cpp:127`）。

⇒ ★★ **默认配置下，上报给调度器的 SINR 来自均衡器的噪声，不是来自估计器。**
这一条把"噪声头"从"辅助头"钉成"**唯一 SLA 来源**"，与平台 `known_limitations.md` 的
声明（`memo_06` §4）**完全同形**。

---

## 2. 逐字段消费者清单（A1 的正表）

★ 表注：
**"写入者"** = 谁调用 `set_*`；**"读出者"** = 谁调用 `get_*`（**已排除** `tests/` 与
`lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp`——后者是**测试工具**，
`lib/phy/upper/channel_processors/metal/CMakeLists.txt:82` 建的是 executable，不在链上）。

| CSI 字段（getter） | 写入者 | ★ 生产读出者 | 用来做什么 | 精度要求 | ★ 是否影响后续槽 |
|---|---|---|---|---|---|
| ★★ **`get_sinr_dB()`**（默认选 `post_equalization`）| 解调器 `notifier_adaptor.h:153/165`；估计器 `dmrs_pusch_estimator_impl.cpp:396`（备选）| FAPI 翻译 `...fastpath_translator.cpp:192, :251` | ① **UL MCS 选择**；② **UL OLLA**；③ **TA 有效性门**；④ CG 的 DTX 判定；⑤ 调度器/E2 指标 | ★ **亚 dB 有用**：`lib/scheduler/support/mcs_calculator.cpp:58-72` 的 MCS 门限**最密间隔 0.179 dB**（MCS 21↔22），次密 0.35 dB | ★★ **是**（见 §3） |
| **`get_rsrp_dB()`** | 估计器 `:379-380`（内部走 `set_rsrp_lin`，**只取 layer 0、端口平均**）| FAPI 翻译 `:180-183, :236-239`（另 PUCCH 侧 `:334, :456`）| FAPI `rsrp` → MAC `ul_rsrp_dBFS` → `cell_metrics_handler.cpp:190-192` → **E2 KPM** | 日志/显示 `{:.1f} dB`（`fapi_power_unit.h` 的 formatter）；**存的是 float，无量化** | ⬜ 只进指标与日志 |
| **`get_epre_dB()`** | 估计器 `:382-384`（**多端口线性平均后转 dB**）| FAPI 翻译 `:187-190, :243-246`，**填进 FAPI 的 `rssi` 字段**（注释逐字："Populate the RSSI field with the EPRE value … which has an equivalent definition"）| FAPI `rssi` → MAC `rssi_dBFS` | 同上 | ⬜ |
| ★ **`get_time_alignment()`** | 估计器 `:386-387` ——★ **取"SNR 最好的那个端口"的值** | FAPI 翻译 `:192, :252` | FAPI `timing_advance_offset` → MAC `time_advance_offset` → 调度器 `n_ta_diff` → **TA_CMD MAC CE 下发 UE** | ★ **比 TA 命令量化细 16–50× 就够**（见 §4.2） | ★★ **是（闭环）** |
| ★ **`get_cfo_Hz()`** | 估计器内部由 `preprocess_pilots_and_estimate_cfo()` 写（`port_channel_estimator_average_impl.cpp:344, 585`）；★ **PUSCH 的 CSI 路径上无人写入** | ★ **不进 CSI、不进 FAPI**。消费者只有：① 估计器自己（§4.3）；② 日志 `channel_state_information_formatters.h:134`；③ 采集 `ul_capture.cpp:216` | — | — | ⬜ **否**（跨槽意义上）|
| **`get_port_rsrp_dB()`** | 随 `set_rsrp_lin`（`:216-233`）| ★ **无生产消费者**：`formatters.h:110` 只在 verbose 日志里逐端口打印 | — | — | ⬜ 否 |
| **`get_total_evm()` / `get_symbol_evm()`** | 解调器 `notifier_adaptor.h:156/168` | 仅日志（`formatters.h:89-95`）与 `sinr_evm` 备选路径 | — | — | ⬜ 否（默认路径不开 EVM，见 `upper_phy_factories.cpp:926-930`）|

### 2.1 `dmrs_pusch_estimator_results` 上**未被 CSI 消费**的 getter

| getter | 消费者 | 结论 |
|---|---|---|
| `get_rsrp(rx_port, tx_layer)` | 仅 `get_channel_state_information()` 自己（`:379` 的 `get_rsrp_all_ports`） | ⬜ **不是上报义务** |
| **`get_rsrp_all_ports(tx_layer)`** | ★ **无**（全树 grep 只命中定义与自身调用） | ⬜ 不是义务（C6） |
| `get_noise_variance(rx_port)` | ★★ **解调器** `pusch_demodulator_impl.cpp:583` → 作为均衡器的**输入噪声方差**；另有 `ul_capture.cpp:220`（采集） | ★ **是内部接口，不是上报** —— 但深度 3 若不产出"DM-RS 域噪声"，**这条链会断**（见 §7 的 R4） |
| `get_snr(rx_port)` | 仅 `get_channel_state_information()` 内部（`:371`，用于**挑"最好端口"**） | ⬜ 不是义务 |
| `get_layer_average_snr(tx_layer)` | 仅 `:397`（写 `sinr_ch_estimator`） | ⬜ 不是义务（C6） |
| `get_epre(rx_port)` | `get_channel_state_information()` `:368`；`ul_capture.cpp:223` | 间接（见 EPRE 行） |
| `get_symbol_ch_estimate(...)` | ★★ **均衡器主输入**（`pusch_demodulator_impl.cpp:571` 起的 `get_ch_data_estimates`）+ `ul_capture` | ★★ **最硬的一条**：深度 3 若不产出信道估计，**经典均衡器无输入**（这正是路线 a 存在的理由） |
| `sync_device_estimates()` / `device_results_cover_last_estimate()` | `pusch_processor_impl.cpp:566`；解调器 `serves_hop_in_place()` | 设备侧契约，见 `memo_06` §2quater |

---

## 3. ★★ 唯一的链路自适应输入：`σ²_post` 怎么变成 MCS

这是 C2 的完整证据链。**每一跳都有 `文件:行号`。**

### 3.1 从逐 RE 噪声到 CSI 里的一个 dB 数

1. 均衡器对每个符号输出逐 RE 的后均衡噪声方差数组 `state.nv`
   （`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:607/618/624`）。
2. 累加时**跳过 inf**（`:233-276` 的 `filter_infinite_and_accumulate`，注释逐字：
   *"Exclude outliers with infinite variance. This makes sure that handling of the DC carrier does not skew
   the SINR results."*）——★ **DC 子载波会置 inf**。
3. 每符号与整次传输各算一次：
   ```
   :742-745   state: mean_noise_var = Σ/count;  sinr_dB = -convert_power_to_dB(mean)
              else:                            sinr_dB = +infinity        ← ★★ 注意这一支
   :773-777   total: 同一算式，同一 else 分支
   ```
   ⇒ ★★ **条件不是"平均值小"，而是 `count != 0 && Σ > 0.0`。**
   **全零噪声张量 ⇒ Σ == 0 ⇒ `sinr_dB = +inf`**（不是"很大的数"，是**真的 `inf`**）。
   ★ 这一支在经典链上是**可达的**（不是理论顾虑），因为条件就是 `Σ > 0`。
4. 该值经 `set_sinr_dB(post_equalization, …)` 进 CSI（`notifier_adaptor.h:153/165`）。
5. `get_sinr_dB()` 按默认 `sinr_report_type = post_equalization` 选中它。

### 3.2 从 dB 数到 MCS

| 跳 | 位置 | 内容 |
|---|---|---|
| 1 | `fapi_adaptor/.../phy_to_fapi_results_event_fastpath_translator.cpp:192, :251` | → FAPI `ul_sinr_metric_dB` |
| 2 | `fapi_adaptor/mac/p7/fapi_to_mac_indications_fastpath_translator.cpp:119` | → MAC `pdu.ul_sinr_dB` |
| 3 | `lib/mac/mac_sched/ocudu_scheduler_adapter.cpp:411` | → 调度器 `ul_crc_pdu_indication.ul_sinr_dB` |
| 4 | ★ `lib/scheduler/ue_context/ue_cell.cpp:189` | `components.channel_state->update_pusch_snr(crc_pdu.ul_sinr_dB.value())` ——**存成 UE 的 PUSCH SNR，跨槽保留** |
| 5 | `lib/scheduler/ue_context/ue_link_adaptation_controller.cpp:106` | `get_effective_snr() = get_pusch_snr() + olla_offset_db()` |
| 6 | `.../ue_link_adaptation_controller.cpp:157` | `map_snr_to_mcs_ul(get_effective_snr(), …)` |
| 7 | `lib/scheduler/support/mcs_calculator.cpp:114-` | SNR 门限表 `ul_snr_mcs_table`（`:58-72`，29 项，**最密间隔 0.179 dB**） |

### 3.3 另外三处消费（都在同一根 SINR 上）

| # | 位置 | 行为 |
|---|---|---|
| a | `.../ue_link_adaptation_controller.cpp:71` | ★ **`pusch_snr_db < olla_ul_min_pusch_snr` 时直接 `return`，不更新 OLLA** —— ★ **假的高 SINR 会让这条拒绝门失效**，假的低 SINR 会让 OLLA 停摆 |
| b | `lib/scheduler/ue_context/ue_cell.cpp:172-175` | CG：CRC KO **且** `ul_sinr_dB < cg_pusch_sinr_threshold_dB` ⇒ 判定为 **DTX（UE 没发）** |
| c | `lib/scheduler/cell_event_manager.cpp:494` | ★ **TA 更新的准入条件之一**（见 §4.1）|

### 3.4 ★ 对本工作流的直接后果（写进 `memo_08` 判据）

| # | 判据 | 依据 |
|---|---|---|
| N1 | ★★ **噪声头不得输出"逐 RE 全零 / 近似常数"** —— 否则 `Σ == 0` ⇒ `sinr = +inf` ⇒ MCS 被推到最大档 | §3.1 步骤 3 的 `else` 分支 |
| N2 | ★★ **上报的 SINR 与经典链在同一数据上的分布必须可比**（不是"看起来合理"，而是**逐样本相对误差分布**）| §3.2 全链 |
| N3 | ★ **SINR 的验收容差按 0.2 dB 量级定** —— 因为 MCS 门限最密处就是 0.179 dB | `mcs_calculator.cpp:58-72` |
| N4 | ★ **降级要计数**：噪声头缺失时必须**显式上报缺字段并计数**，不能让它静默变成 `+inf` 或 0 dB | `memo_10` §7 的义务 3 |

---

## 4. ★★ TA / CFO 专项（功课 A4 的答案）

### 4.1 TA **有真实、闭环的下游用途** —— 完整链路

```
dmrs_pusch_estimator_impl.cpp:346-350   get_time_alignment(best_rx_port)
  → :387  csi.set_time_alignment(...)
  → phy_to_fapi_results_event_fastpath_translator.cpp:192 / :252
  → fapi_to_mac_indications_fastpath_translator.cpp:121
  → ocudu_scheduler_adapter.cpp:413
  → cell_event_manager.cpp:494-496
       if (crc.tb_crc_success and crc.time_advance_offset.has_value() and crc.ul_sinr_dB.has_value())
         ue_ev_handler.handle_ul_n_ta_update(ue_index, tag_id, *time_advance_offset, *ul_sinr_dB);
  → ue.h:89   ta_mgr.handle_ul_n_ta_update_indication(tag_id, n_ta_diff.to_Tc(), ul_sinr)
  → ta_management_system.cpp:189-276
       :194  ul_sinr <= update_measurement_ul_sinr_threshold  ⇒ **丢弃本次测量**
       :227  Welford z-score 离群点剔除
       :246  window_sum_samples += n_ta_diff
       :248  last_t_a = compute_new_t_a(...)
       :266  只有 |last_t_a - 31| >= ta_cmd_offset_threshold 才进时间轮
  → ta_management_system.cpp:101-140 (handle_ue_ta_cmds)
       :126-127  lc_ch_mgr.handle_mac_ce_indication({TA_CMD, ta_cmd_ce_payload{tag_id, ta_cmd}})
  → dl_sch_pdu_assembler.cpp:151-161   打包成 **TA_CMD MAC CE** 下发给 UE
```

**结论（C1）**：★ **TA 不是"上报量"，是闭环控制量。** 它**跨槽影响 UE 的发送定时**。
所以：

> ★★ **深度 3 若不产出可用的 TA，TA 环路就断了。** 这不是"某个可选上报字段变空"，
> 而是 **UE 的上行定时不再被纠正**（`ta_cmd` 永远停在 31 ⇒ 不下发）。

**三条必须一起说清的边界**：

| # | 事实 | 位置 | 后果 |
|---|---|---|---|
| B1 | ★ **开环配置下 TA 管理默认是开着的**：`ta_cmd_offset_threshold = 1`、`measurement_period = 80` 槽、`target = 1.0` | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:88-102`；调度器侧默认见 `include/ocudu/scheduler/config/scheduler_expert_config.h:94-116` | 必须按"默认开启"设计 |
| B2 | ★ **CRC 是 TA 的准入条件**（`crc.tb_crc_success and …`）| `cell_event_manager.cpp:494` | ★ **LLR 坏了 ⇒ CRC 挂 ⇒ TA 也没了**。深度 3 的两个输出在这里是**串联**的，不是并联的 |
| B3 | ★ **`ul_sinr_dB` 也是准入条件**，且门限同上（§3.3a）| `cell_event_manager.cpp:494`、`ta_management_system.cpp:194` | ★ **又一个 `+inf` 会绕过的地方**：`inf > threshold` 恒真 ⇒ 门失效 |

★ **还有一条旁路**（降低但不消除 C1 的严重性）：PUCCH F0/F1 与 F2/3/4 也带 TA
（`phy_to_fapi_results_event_fastpath_translator.cpp:346, :469`），
且**不经过 CRC 门**（走 UCI 路径，`cell_event_manager.cpp:791-801`）。
⇒ **若 PUCCH 还在跑，TA 环路仍有一条备份**。但 PUCCH 只在有 UCI 的槽出现，
**不能当作 PUSCH 主路径的替代**。

### 4.2 TA 的精度要求（可计算，不靠猜）

| 项 | 值 | 依据 |
|---|---|---|
| TA 命令的物理量子 | **64·Tc ≈ 32.55 ns** | TS 38.213 §4.2；`phy_time_unit.h` 的 `KAPPA = 64`、`T_C = 1/(480k·4096)` |
| 调度器算 TA 时的取整 | `16·64·Tc / 2^μ`：**520.8 ns @15 kHz**，**260.4 ns @30 kHz** | `ta_management_system.cpp:158-163`：`round(n_ta_diff · slots_per_subframe / (16·64) + offset)` |
| 经典估计器的物理分辨率 | ★ **10.2 – 40.7 ns** | `time_alignment_estimator_dft_impl.cpp:181-261`：相关性 IDFT 尺寸由 `get_idft_size()` 定（`:160-166`，`min_dft_size` 使分辨率不粗于一个 TA 单位），分辨率 = `1/采样率`；25 PRB@30 kHz → IDFT 512；106 PRB@15 kHz → IDFT 2048 |
| 存进 CSI 时的表示精度 | **1 Tc ≈ 0.51 ns**（`from_seconds` 四舍五入到最近 Tc） | `phy_time_unit.h:274-282` |

★★ **结论**：**经典链的 TA 分辨率比它被使用的量子细 16–50 倍。**

⇒ 深度 3 的 TA 精度要求可以写成一个**很宽的**判据：

> **在测得的 TA 上，与经典链的差 ≤ 1 个 TA 单位（64·Tc ≈ 32.6 ns）即可接受**，
> 且**必须"通常非零"**（一个恒为 0 的 TA 会让 TA 环路静默停摆，与"链路本来就很准时"不可区分）。

★ 这条对网络设计是**好消息**：TA 不需要子 Tc 精度，**"量级对、符号对"就够**
（与占位大纲 §2 里"若只被上报 ⇒ 有值且量级正确"那条判据**恰好同形**，
但原因完全不同：**它是被补偿用的，只是补偿的量子很粗**）。

### 4.3 CFO：**不上报，但在估计器内部被消费**（C3 + C4 + C4bis）

★ 这一节在初稿里写错过一次，**更正记录见 §9.4**。结论分三层，必须分开说。

#### 第 1 层：**上报路径确实不存在**（C3，已用 grep 证实）

| 通道 | 事实 | 位置 |
|---|---|---|
| PUSCH → CSI | ★ **从不写入** | `grep -rn "set_cfo" lib/phy/upper/channel_processors/pusch/` → **0 命中** |
| CSI → FAPI | ★ 不传（FAPI 的 UCI/CRC PDU 没有 CFO 字段）| `include/ocudu/fapi/p7/builders/{uci_pusch_pdu_builder.h:32-42, crc_indication_builder.h:35-46}` 只有 sinr/TA/rssi/rsrp |
| CSI → 日志 | 会打印，但 PUSCH 下恒为 `cfo=na`（verbose 级）| `channel_state_information_formatters.h:102, 134-136` |
| CSI → 采集 | `ul_capture::capture_ce` 逐端口写 `cfo_hz` | `ul_capture.cpp:216, 226-233` |

#### 第 2 层：★★ **但 CFO 是估计器自己的输入**（C4）—— 初稿漏掉的一条

经典估计器算出的 CFO **会回过来修正它自己产出的信道估计**：

```
port_channel_estimator_average_impl.cpp
  :344   cfo_Hz = cfo_normalized · scs_Hz                  ← 估计出的 CFO
  :152-156  （写符号估计时）if (compensate_cfo && cfo_normalized.has_value())
                ocuduvec::sc_prod(symbol, symbol, polar(1, +2π·epoch·cfo));   ← 加回相位
  :775-780  （合并 DM-RS 符号时）if (compensate_cfo && …) → polar(1, −2π·epoch·cfo)
  :804-809  同上，逐 DM-RS 符号
```

★ 语义：**多个 DM-RS 符号之间的相位旋转被 CFO 解释掉**，所以"跨符号平均"不会因为频偏而互相抵消。
⇒ **它影响的是 R1（信道估计）本身，进而是 R2/R3/R5，且发生在同一个槽内。**

**开关与默认值**（这条是本节的实用结论）：

| 项 | 值 | 位置 |
|---|---|---|
| 端口估计器构造参数的默认值 | **`compensate_cfo_ = true`** | `port_channel_estimator_average_impl.h:46` |
| ★ **PUSCH 传给它的值** | `config.pusch_channel_estimator_compensate_cfo` | `upper_phy_factories.cpp:855` ← `du_low_config_translator.cpp:147` |
| ★★ **本仓库默认** | ★ **`false`（关）** | `apps/units/flexible_o_du/o_du_low/du_low_config.h:103` |
| ★ 我们飞行的配置 | ★ **没写这一项** ⇒ 取默认 `false` | `configs/gnb_rf_b200_tdd_n78_20mhz.yml:78-84` 的 `expert_phy:` 块里，它**是被注释掉的那一行**（`:80`） |

★★ **所以：默认配置下，CFO 被估计、被存进结果对象，但不用来做补偿。**
★ 而配置文件里留着注释 *"The USRP clock may introduce some CFO: configure the channel estimator to
compensate for it."* —— **说明"打开它"是一个操作员会做的动作**。

#### 第 3 层：**RU 层的硬件补偿是另一条独立的路**（C4bis）

`ntn_ru_doppler_adapter.cpp:20,49`（NTN 多普勒）与操作员命令 `cfo <sector> <Hz>`
（`flexible_o_du_commands.h:171-186`）→ `ru_controller_sdr_impl.cpp:124-138` → `baseband_cfo_processor.h:33`。
★ **这条路与 CSI 的 `cfo_Hz` 字段没有任何数据连接** —— 两者只是"都叫 CFO"。

#### ★★ A4 关于 CFO 的裁决

| # | 裁决 | 理由 |
|---|---|---|
| 1 | ★★ **CFO 不构成深度 3 的上报义务** | 第 1 层：报出去的路径不存在（§4.3 表） |
| 2 | ★★ **但深度 3 必须显式回答"补偿开关还意味着什么"** | 第 2 层：经典链上这个开关**有实际功能**。⇒ ★ 若 P0 基线的 YAML 把它打开，**深度 3 的对照就少了一个环节**；若保持默认关闭，**深度 3 与基线在这一点上等价** |
| 3 | ★ **采集继续记**（`ul_capture` 已在做） | 它是部署分布的一部分（`memo_09` 不可恢复字段原则），也是**将来若要做频偏跟踪的唯一历史数据** |
| 4 | ★ **不要把第 3 层当论据** | 第 3 层是 RU 的独立功能，与估计器的 CFO 无关 |

---

## 5. 精度要求汇总（可直接变成验收判据）

| 上报量 | 下游的隐含精度要求 | 依据 | 建议判据 |
|---|---|---|---|
| ★ **post-EQ SINR** | **0.2 dB 量级**（MCS 门限最密 0.179 dB） | `mcs_calculator.cpp:58-72` | 逐样本相对误差分布；**中位误差 < 0.2 dB**，且**不得出现 `+inf`/`-inf`/NaN** |
| **RSRP** | 宽松：日志 `{:.1f} dB`；E2 KPM 甚至**取整成 integer** | `fapi_power_unit.h` formatter；`e2sm_kpm_du_meas_provider_impl.cpp:350` `(int)ue_metrics.pusch_snr_db` | **1 dB** 量级；★ 但**必须非空**（空则 FAPI 少填字段） |
| **EPRE** | 同 RSRP（填进 `rssi`） | `...fastpath_translator.cpp:185-190` | 同 RSRP |
| ★ **TA** | **≤ 1 TA 单位（64·Tc ≈ 32.6 ns）**，且**通常非零** | §4.2 | 与经典链之差 ≤ 1 个 TA 单位；**分布方差不得为 0** |
| **CFO** | ★ **无上报要求（无人消费）**；★ 但估计器内部有个**默认关闭**的补偿开关会用它 | §4.3 | 只要求"**采集里有**"；★ **P0 必须写明该开关的取值** |
| **`sinr_ch_estimator`** | 只在 `pusch_sinr_calc_method = "channel_estimator"` 时成为义务 | `du_low_config.h:68` 默认是 `post_equalization` | ★ **P0 用默认配置 ⇒ 不是义务**；但**必须在判据里写明"默认配置"这个前提** |

★★ **一条容易漏掉的性质**：`channel_state_information` 的类注释逐字写着
*"The measurements shall not store NaN in their values."*
（`channel_state_information.h:22`），而 `set_*` 系列**确实**对 NaN 做了丢弃
（`:86-104, 109-117, 179-185, 254-261`）。
**但 `inf` 不被丢弃** —— `+inf` 是**合法**值，一路能到调度器。
⇒ ★★ **"输出合法"不等于"输出有意义"**：深度 3 的输出必须在 host 侧**额外做**有限性检查，
**不能**指望 CSI 这一层帮我们挡住（这正是 `memo_10` §7 义务 1 说的事，
此处给出了它在**本条链上**的具体落点）。

---

## 6. 与平台契约的对照（主规划 §2.3 的对接）

| 平台契约字段（`memo_06` §1.3） | 我们链上的对应物 | 关系 |
|---|---|---|
| `metrics.post_equalization_noise` **F32 `[data_re, layer]`** | ★ 解调器的逐 RE `state.nv`（`pusch_demodulator_impl.cpp`）—— **已有的内部量** | ★★ **同一件事**：平台要的就是它。我们的噪声头输出**必须**是它的等价物 |
| 主机算 SINR：`SINR_dB = -10·log10(mean σ²_post)` | ★ **本仓库真的就是这么算的**（`:773-777` 逐字同式） | ★★ **两个独立实现收敛到同一算式** —— 这条给 `memo_06` §4bis(1) 补上了**本仓库侧的证据** |
| `metrics.equalized_symbols`（可选） | 均衡器输出 `state.eq` | ⬜ 我们不填（契约允许） |
| `confidence_q16`（平台不 gate） | 无 | ⬜ 不用 |
| ★ 平台承认 `EPRE / RSRP / CFO / TA` 在 Class-A 内联路径**缺失** | ★ **本仓库这四项全在** | ★★ **我们的路线 a 相对平台现状是"补齐"而不是"对等"** —— 这是一条可以写进 G4 的正面论据 |

★ **一个必须写明的差异**：平台计划用
*"reserved-slot fields so the dApp reports its own **DMRS-derived** measurements"*（`memo_06` §4），
而我们的路线 a（net 输出信道估计 + 噪声 → **经典测量核跑在 net 输出上**）走的是
**"让经典核继续算，只把输入换掉"**。⇒ ★ **两条路的产物语义不同**：
平台是"dApp 自己算 DMRS 测量"，我们是"经典核的算式不变、输入来自 net"。
**后者对上报口径的扰动更小**，但**要求 net 输出的信道估计在 DMRS RE 上足够准**（§7 的 R1）。

---

## 7. ★★ 结论：深度 3 的最小上报集合与头规格

> **这是 A1 对 `memo_08` / 模型设计的实际交付。**

| # | 必须产出 | 规格 | 为什么 | 若缺失 |
|---|---|---|---|---|
| **R1** | ★★ **DM-RS RE 上的信道估计** | 与经典估计器**同形**（让 `dmrs_pusch_estimator_results` 的接口能拿到）；**NMSE 判据** | ① 均衡器的主输入（`pusch_demodulator_impl.cpp:571`）；② EPRE/RSRP/SNR 全从它算（`port_channel_estimator` 侧） | 经典均衡器无输入 ⇒ 整条链断 |
| **R2** | ★★ **逐 RE 后均衡噪声方差**（不是 DM-RS 域噪声） | **F32 逐 RE**（对齐平台 `[data_re, layer]`）；**不得为零、不得近似常数** | ★ **唯一的链路自适应输入**（§3） | MCS/OLLA/TA 门全被 `+inf` 污染 |
| **R3** | ★★ **TA** | 从 R1 的估计上**按经典算式**产出（或保留轻量 DM-RS 经典块）；**精度 ≤ 1 TA 单位** | ★ **闭环控制**（§4.1） | UE 上行定时不再被纠正 |
| **R4** | ★ **DM-RS 域的噪声方差**（`get_noise_variance`） | 标量，逐端口 | 均衡器的**输入**噪声方差（`pusch_demodulator_impl.cpp:583`） | 均衡器退化为固定噪声假设 ⇒ R2 也无法可信 |
| **R5** | ★ **RSRP + EPRE** | 端口平均；layer 0 即可 | FAPI `rsrp` / `rssi`；E2 KPM | 调度器指标与 E2 上报退化（不致命） |
| **R6** | ⬜ **CFO** | ★ **不需要作为输出** | §4.3 第 1 层 | 无影响（★ 但见 §4.3 第 2 层的开关记账） |

### 7.1 ★★ "哪些必须由 net 直接产出，哪些可以在 net 输出上现算"（路线 a 的边界）

| 量 | 做法 | 理由 |
|---|---|---|
| **R1 信道估计** | ★ **net 直接产出**（或在 net 内部算） | 它正是 net 的中间量 |
| **R2 逐 RE 后均衡噪声** | ★ **net 直接产出**（辅助头） | 均衡是 net 内部的事，外面算不出来 |
| **R3 TA** | ★ **在 R1 上现算**（经典 TA 估计器吃 R1 的输出） | TA 是**频域相位斜率**的函数，只依赖 DM-RS RE 上的信道估计 ⇒ **只要 R1 够准，经典 TA 估计器照跑** |
| **R4 DM-RS 噪声** | ★ **net 直接产出**（标量头） | 经典噪声估计器依赖"DM-RS RE 的 LS 估计残差"，若 R1 已经是平滑后的估计，**残差里没有噪声了** ⇒ 必须由 net 给 |
| **R5 RSRP/EPRE** | ★ **在 R1 上现算** | 都是 R1 的功率泛函 |
| **R6 CFO** | ⬜ 不做为输出 | 无上报消费者；★ 开关记账见 §4.3 第 2 层 |

★★ **这张表就是"路线 a"的可执行形式**，也回答了 `memo_07` 占位大纲 §4 的问题：
**主头（LLR）+ 辅助头 1（后均衡噪声）不够**，还需要
**一个"DM-RS 域信道估计 + 噪声"的辅助头**（= 占位大纲里的"辅助头 2"），
**否则 R1/R3/R5 无处可来，R4 也拿不到**。

### 7.2 头的数量与规格（对 §2.3 的修订建议）

| 头 | 输出 | 必需性 | 说明 |
|---|---|---|---|
| **主头** | 逐 RE 逐比特 LLR（加扰域） | ★ 必需 | 不变 |
| **辅助头 1** | ★ **逐 RE 后均衡噪声方差** | ★★ **必需**（硬约束） | 平台契约 + 本仓库默认配置，**两条独立证据** |
| **辅助头 2** | ★ **DM-RS RE 信道估计 + DM-RS 噪声** | ★★ **必需**（本 memo 新增的结论） | 占位大纲称"辅助"，实为**必需**：R1/R3/R4/R5 全依赖它 |

★ **对 G3 的一条新增数值判据**（来自 §3.1 步骤 3 与 §5）：
**上报的 SINR 必须有限且非常数** —— 判据的检查点就放在
"**CSI 里的 `sinr_post_eq_dB` 是不是 `+inf`、以及跨样本方差是否为 0**"，
而不是放在网络输出上（网络输出全零在 host 侧看起来完全合法，见 `memo_10` §7 义务 1）。

---

## 8. ★ 两条"必须写进 `memo_08`"的路由纪律

| # | 纪律 | 依据 |
|---|---|---|
| **D1** | ★★ **任何"缺字段"必须被计数并打印**，不能静默 `nullopt` | 全链上每一个字段都是 `std::optional`，缺失**不报错**、只让 FAPI 少填一项（§1 ③⑤）。这正是 `memo_10` §7 义务 3 在**本条链上**的落点 |
| **D2** | ★★ **"合法"≠"有意义"** —— `+inf` 能一路走到 MCS 选择与 TA 门 | §5 末；`channel_state_information.h:22` 只禁 NaN，不禁 inf；`ta_management_system.cpp:194` 与 `ue_link_adaptation_controller.cpp:71` 的门在 `inf` 下**恒真** |

★ **D2 的一个具体化**（建议直接进 `memo_08`）：
> **P0 的每一个数值输出，在进入任何门限之前，必须先过一遍"有限性 + 非常数性"指纹**：
> `isfinite()` 全部为真、跨样本标准差 > 0、**且不等于"全零输入会得到的那个值"**。
> **不通过者标记为 `Not Run`，不得当作数值结果**（`memo_10` §开头纪律 2 的同一条精神）。

---

## 9. 证据与复现

### 9.1 复现命令（全部只读，无需构建）

```bash
cd ~/dev/ocudu

# ① 接口面
grep -rn "get_channel_state_information" include/ lib/ apps/

# ② 逐字段消费者（★ 排除 tests/ 与 ul_chain_replay）
for f in get_rsrp get_rsrp_all_ports get_epre get_noise_variance get_snr \
         get_layer_average_snr get_time_alignment get_cfo_Hz get_symbol_ch_estimate; do
  echo "=== $f ==="
  grep -rn "$f" --include=*.cpp --include=*.h lib/ apps/ include/ \
    | grep -v "dmrs_pusch_estimator_impl\|port_channel_estimator_average\|include/ocudu/phy/upper/signal_processors"
done

# ③ ★ CFO 在 PUSCH 上从不写入（应当 0 命中）
grep -rn "set_cfo" lib/phy/upper/channel_processors/pusch/

# ④ ★ TA 闭环的末端
grep -rn "handle_ul_n_ta_update\|handle_mac_ce_indication" lib/scheduler/ lib/mac/

# ⑤ ★ 链路自适应的输入端
grep -rn "get_pusch_snr\|update_pusch_snr\|map_snr_to_mcs_ul" lib/scheduler/

# ⑥ ★ CFO 的三层（§4.3）：上报路径不存在、但估计器内部会用
grep -rn "set_cfo" lib/phy/upper/channel_processors/pusch/          # → 0 命中
grep -rn "compensate_cfo" lib/phy/upper/signal_processors/channel_estimator/ \
                          lib/phy/upper/upper_phy_factories.cpp
grep -rn "pusch_channel_estimator_cfo_compensation" \
     apps/units/flexible_o_du/o_du_low/du_low_config.h configs/

# ⑦ ★ SINR 上报口径的部署取值（默认 post_equalization）
grep -rn "pusch_sinr_calc_method" apps/ configs/                     # → 只在 apps/ 有默认值
```

### 9.2 ★ 本次核对的基线

* **HEAD**：`fb06cac69d`（`doc_chinese/llr_ai_detection/` 的 handoff 提交）
  —— 本 memo 的全部行号以此为基准。
* ★ **本 memo 全部结论都由 §9.1 的命令直接产出**；没有一条来自推测。
* ★ 行号会随上游合并漂移 —— **重开条件**：下一次 `merge main` 之后，
  §1 的七跳与 §4.1 的十二跳必须**重新核对**（判据是"每一跳还能 `sed -n` 出来"）。

### 9.3 ★ 本 memo **没有**覆盖的（如实记账）

| 项 | 为什么 | 归到哪 |
|---|---|---|
| **A2**（`resource_grid_reader` 布局与零拷贝）| A1 只做上报路径 | `memo_08` 之后 |
| **PUCCH 侧的上报路径** | 本文只做 PUSCH（深度 3 的替换点）。★ 已确认 PUCCH 也写 TA/CFO，且 **PUCCH 的 TA 是 PUSCH TA 的旁路**（§4.1 B3） | 若深度 3 将来扩到 PUCCH 再做 |
| **SRS 侧的 TA** | 它走**另一条**入口（`cell_event_manager.cpp:715-723`，SINR 由信道矩阵凑），**不经过 CE 的 CSI** | 与本工作流无关 |
| ★ **`sinr_report_type` 的部署实际取值** | ✅ **已核**：全树 `configs/*.yml` **都没有** `pusch_sinr_calc_method`（§9.1 命令 ⑦）⇒ 取代码默认 **`post_equalization`** | 已完成，无需 P0 再核 |
| **E2 KPM 的 RSRP 语义** | `e2sm_kpm_du_meas_provider_impl.cpp:350` 把 **RSRP 报成 `pusch_snr_db`**（并 `(int)` 取整）——★ **这是上游的一个可疑处**，但**不在本工作流范围内**，仅记录 | 上游 |
| ★ **`ta_management_system.cpp:111` 的 `abs(last_t_a - ta_cmd_offset_zero)`** | `last_t_a` 是 `unsigned`，对无符号量用 `abs()` 是可疑写法；★ 本文只引用**行为**，**不裁定是否为缺陷** | 仅记录 |

### 9.4 ★★ 本 memo 的一处**更正**（留痕，不删旧文）

| 项 | 初稿（错误） | 更正（v1.0 定稿） |
|---|---|---|
| **CFO 的消费者** | "PUSCH 路径上没有消费者"——只查了 `set_cfo` 与 CSI 的 getter | ★★ **漏了估计器内部的回路**：CFO 估计出来后经 `compensate_cfo` **修正它自己产出的信道估计**（§4.3 第 2 层）。上报路径确实不存在（这半条对），但"没人消费"是**错的** |

**为什么会漏**：初稿只沿"`channel_state_information` → FAPI → MAC"这条**上报链**搜；
而这条回路**根本不经过 CSI 对象** —— `cfo_normalized` 是估计器的私有成员，
被同一个类的 `compensate_cfo_and_accumulate()` 消费。

⇒ ★★ **方法学结论（值得带进后面的功课）**：
**"某个量没有被上报"不等于"某个量没有被使用"。**
判据必须是"**它的值有没有进入任何一个下游算式**"，而不是"**有没有出现在上报结构里**"。
★ 这条对本工作流直接相关：**深度 3 要替换的是一个"内部有闭环"的模块**，
**只对照上报结构来划义务边界会漏项。**

★ 触发这次更正的线索来自 `configs/gnb_rf_b200_tdd_n78_20mhz.yml:79-80` 的注释
（*"The USRP clock may introduce some CFO: configure the channel estimator to compensate for it."*）
—— **配置文件里的注释是一条证据来源**，不是装饰。

★ **另外两条只记录、不裁定的观察**：

| 项 | 内容 | 归到哪 |
|---|---|---|
| **E2 KPM 的 RSRP 语义** | `e2sm_kpm_du_meas_provider_impl.cpp:350` 把 **RSRP 报成 `pusch_snr_db`**（并 `(int)` 取整）——★ **这是上游的一个可疑处**，但**不在本工作流范围内**，仅记录 | 上游 |
| ★ **`ta_management_system.cpp:111` 的 `abs(last_t_a - ta_cmd_offset_zero)`** | `last_t_a` 是 `unsigned`，对无符号量用 `abs()` 是可疑写法；★ 本文只引用**行为**，**不裁定是否为缺陷** | 仅记录 |

---

## 10. 产出的用途（回填）

| 下游 | 用它的方式 |
|---|---|
| 主规划 §2.3（多任务头）| ★ **头的数量与必需性已定**：主头 + 后均衡噪声头 + **DM-RS 信道估计/噪声头**（§7.2）|
| 主规划 G3 判据 | ★ **新增**：上报 SINR 必须**有限且非常数**；检查点在 CSI 上（§7.2 末）|
| `memo_08` 判据 | ★ §3.4 的 N1–N4 + §8 的 D1/D2 + §7 的 R1–R6 |
| `memo_09` §4.5/4.6 | ★ **CFO 仍要采集**（唯一的历史数据）；TA 要采"逐端口"，因为上报只用"最好端口" |
| **A6 实现方案** | ★ 采集时要**同时记录** R1–R5 的**经典值**（作为深度 3 的参照），否则将来无法做"上报口径是否漂移"的对照 |
| **A2**（下一步）| ★ R1 要求"net 输出的信道估计与经典估计器**同形**" ⇒ 这正是 `resource_grid_reader` 与 `dmrs_pusch_estimator_results` 的**布局问题** |
