# memo 07 —— 上报义务清单（★ **占位大纲，尚未开始**）

> **状态**：⏳ **计划产出，尚未落盘内容**。
> **对应功课**：主规划 §9.2 的 **A1**（必做）。
> **文件名即计划**：本文件将成为"**CE 到底欠了谁什么**"的权威清单。
>
> ⚠ **本文件当前只有大纲。** 在看到"状态：✅ 完成"之前，
> 任何地方引用 `memo_07` 都应读作"该功课的产出将写在这里"。
>
> 版本：v0.0（占位）｜ 日期：2026-10-08

---

## 为什么必须做这件事

深度 3 的网络**替换掉 CE**，因此**继承 CE 的全部上报义务**。
这不是推论，而是有代码与文档双重证据：

- 代码：`pusch_processor_impl.cpp` 末尾
  `est_results.get_channel_state_information(notifier_adaptor.get_channel_state_information());`
  ——CE 的测量量**合并进 CSI 上报**；
- 平台：OCUDU dApp 的 `known_limitations.md` 逐字承认，在 resident Class-A 内联路径上
  **常规 CSI 测量核被跳过**，`EPRE / RSRP / CFO / TA` 对这些授权**缺失**
  （`doc_chinese/llr_ai_detection/memo_06_ocudu_dapp_platform_survey.md` §4）。

★ **不知道消费者，就不知道噪声头与辅助头要输出什么**——所以 A1 是 A6 与 §2.3 的前置。

---

## 大纲（待填写）

### 1. 逐字段的消费者清单

对 `dmrs_pusch_estimator_results` 的**每一个** getter，回答四问：

| getter | 谁读它 | 用来做什么 | 精度要求 | 是否影响后续槽 |
|---|---|---|---|---|
| `get_rsrp(rx_port, tx_layer)` | ? | ? | ? | ? |
| `get_rsrp_all_ports(tx_layer)` | ? | ? | ? | ? |
| `get_epre(rx_port)` | ? | ? | ? | ? |
| `get_noise_variance(rx_port)` | ? | ? | ? | ? |
| `get_snr(rx_port)` | ? | ? | ? | ? |
| `get_layer_average_snr(tx_layer)` | ? | ? | ? | ? |
| `get_time_alignment(rx_port)` | ? | ? | ? | ★ **重点** |
| `get_cfo_Hz(rx_port)` | ? | ? | ? | ★ **重点** |
| `get_symbol_ch_estimate(...)` | ? | ? | ? | ? |
| `get_channel_state_information(...)` | ? | ? | ? | ? |

★ **线索**：先搜 `get_channel_state_information` 的**接收者**
（`notifier_adaptor.get_channel_state_information()` 流到哪里），再顺着它找下游消费者。

### 2. ★★ TA / CFO 的专项结论（对应功课 A4）

**这是本备忘最关键的未知**：

- 若 TA/CFO **只被上报**（进 CSI）⇒ 深度 3 只需保证"有值且量级正确"；
- 若它们被用于**补偿**或**影响后续槽**（定时推进、频偏跟踪）⇒
  深度 3 的网络**必须继续产出它们**，或保留一个轻量的 DM-RS 经典块。

★ 判据：**能否在不产出 TA/CFO 的情况下，让链路连续多槽不退化？**

### 3. 精度要求与容差

对每个上报量，找出下游对**精度/量级**的隐含要求（门限、比较、上报量化）。
★ 这些要求将直接变成模型辅助头的**训练目标与验收判据**。

### 4. ★ 与主规划 §2.3 的对接

结论要能回答：
- 主头（LLR）+ 辅助头（后均衡噪声）**是否足够**？
- 若不够，还需要哪些头？各自的最小规格是什么？
- 哪些量**可以由 net 的信道估计输出现算**（经典测量核跑在 net 输出上 = 路线 a），
  哪些**必须由 net 直接产出**？

### 5. 与平台契约的对照

逐项对照 `memo_06` §1.3 的 `ocudu_dapp_receiver_metrics_v1`
（`post_equalization_noise` / `equalized_symbols` 的有效位设计），
确认我们的输出集合是它的**超集还是子集**，并说明差异的理由。

---

## 产出的用途

| 下游 | 用它的方式 |
|---|---|
| 主规划 §2.3（多任务头） | 决定**需要几个头、每个头的目标** |
| 主规划 G3 判据 | 上报量的**数值判据**（含"低噪声区不得近零常数"） |
| `memo_09` §4.5/4.6 | 采集时必须记录哪些上报量 |
| **A6 实现方案** | 采集字段补齐时要**同时记录**这些量，避免二次重飞 |
