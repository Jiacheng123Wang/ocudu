# 00 — 目标、现状与差距

> 本文所有断言都带**可核对的出处**（文件:行 或 腿日志里的原文）。
> 凡是我**没有**核过的，一律写进 §7「尚未确认」，不当作已知。

---

## 1. 目标

### 1.1 代码里的原文

`include/ocudu/phy/phy_pipeline_mode.h`：

| 模式 | 定义（原文） |
|---|---|
| `cpu` | 整条链在 CPU 上跑；任何"要求 offload 后端"的旋钮都是配置冲突，不被静默忽略 |
| `cpu_gpu` | **模块级卸载。每个模块跟自己的 backend 旋钮走，所以每个模块边界都保留自己的 host↔device 穿越** |
| **`gpu`** | **融合车道。整条 IQ → LLR 链跑在一条设备侧流水线里，只有两次 host↔device 数据穿越（IQ 上传、LLR 下载）。车道接管它包含的模块的 backend；LDPC 解码器不属于车道（LLR 仍离开设备给 CPU 解码器）** |

**⇒ 首要目标 = `phy_pipeline_mode::gpu` 成立。**
**LDPC 明确不在目标内**（模式定义原文就写着），因此 **LDPC 维持现状，不动**。

### 1.2 目标被编码成了什么（说明它不是口号，是可执行的判据）

| 机制 | 位置 |
|---|---|
| 模式枚举 + 字符串 | `include/ocudu/phy/phy_pipeline_mode.h` |
| 模式 → 后端解析（含冲突检查） | `apps/units/flexible_o_du/o_du_low/du_low_phy_pipeline.h`（`resolve_phy_pipeline`） |
| 模式 → 配置校验 | `apps/units/flexible_o_du/o_du_low/du_low_config_validator.cpp` |
| 运行时契约（7 条检查） | `include/ocudu/phy/phy_pipeline_contract.h` + 各处 `register_phy_pipeline_check` |
| 退出时打印契约 | `apps/gnb/gnb.cpp:654` |

**契约的语义**（`phy_pipeline_contract.h` 原文）：*"每条检查陈述所选模式声称的性质，用探针测到的东西表达
——所以这个声称在**每一次运行**里被验证，而不是在文档里被论证。"*

---

## 2. 现状：今天跑的是什么

**所有空口腿的模式都是 `cpu_gpu`**，而它是被腿脚本**写死**的。

腿 `gnb_fence_0918_1805` 的契约原文（`doc_chinese/work_tmp/logs/gnb_fence_0918_1805.log.stderr`）：

```
[phy_pipeline] contract (mode=cpu_gpu):
[phy_pipeline]   radio sample continuity: 0 gaps over 76775 blocks (0 samples missing or repeated), 0 timestamp-0 blocks -> OK
[phy_pipeline]   dft radio inputs: 227234 of 227235 transforms read the radio buffer -> OK
[phy_pipeline]   zero-copy wraps: 33356 hits, 12729 creates, 0 replaces, 0 failures, 0 misaligned -> OK
[phy_pipeline]   ce device estimates: 17479 device, 0 host -> OK
[phy_pipeline]   cfo compensation: 0 round trips over 1074822 symbols, 0 commands, offset 0.000 Hz -> OK
[phy_pipeline]   baseband metrics: 0 symbols measured for 1074822 processed (metrics disabled) -> OK
[phy_pipeline]   host sample assembly: 1074822 of 1074822 symbols read where the radio put it, 0 copied into a symbol buffer (mode=cpu_gpu) -> OK
[phy_pipeline] contract MET (7 of 7 checks applicable)
```

**7/7 全过。** 写死模式的地方是旧脚本 `doc_chinese/full_gpu_chain/wip/run_air_leg.sh:74`：

```
  --expert_phy.phy_pipeline cpu_gpu \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal \
  --expert_phy.pusch_dft_type metal \
  --expert_phy.device_resource_grid on \
```

**四个模块各自 offload + 模式标成 `cpu_gpu`** = 模式定义里"每个模块边界一次穿越"的那个形态。

---

## 3. 差距的定量口径：**每跳还剩几次数据穿越**

### 3.1 定义（先把尺子定死，否则又会滑回微秒）

> **一次「数据穿越」** = 宿主**读走**设备产出的数据，**或**把**由设备产出算出来的东西写回**设备。
> **控制面不算**（提交、fence、descriptor、模式选择）。
> 目标允许的两次是：**IQ 上传** 与 **LLR 下载**。

### 3.2 今天的账（逐跳）

| # | 穿越 | 方向 | 每跳次数 | 依据 | 目标 |
|---|---|---|---|---|---|
| 1 | IQ 上传 | host→dev | 1（**零拷贝**，`wrap_copies=0`）| `[ul_host] in_place==symbols`、`dft … radio_inputs=227234 wrap_copies=0` | ✅ 允许 |
| 2 | **提取标量回读**：CFO / σ² / power sum | dev→host | **3** | 代码 `:1104`、`:1203`、`:1231` | ❌ 要消 |
| 3 | **宿主统计量 → 权重 CB 参数** | host→dev | **1 组** | `:1402` → `:1429` → `:1457`（`correlation_stage(stats,…)`）| ❌ 要消 |
| 4 | 宿主建相关矩阵（**视路线**）| host→dev | 0 或 1 组 | `:1440`(`host_builds_std`)、`:1720`(合并 edge)、`:1858`(分裂 tail)、`:1953`(CPU 回退) | ❌ 要消 |
| 5 | LLR 下载 | dev→host | 1 | `burst dispatches=14301 (demapper=1589)` → LDPC | ✅ 允许 |
| — | 每跳 `waitUntilCompleted` | 同步 | 1 | `:1035` 附近的 `[s.cb waitUntilCompleted]` | 由 2/3 导致，随之消失 |

**⇒ 今天：2 次允许的 + 4 个待消的（3 次标量回读 + 1 组参数写回）+ 视路线的宿主建矩阵。**
**⇒ 目标：只剩前两项。**

### 3.3 关键洞察：**其中一次回读对 GPU 已经是死的**

- 核在 `sigma2_from_device` 置位时**根本不读** `p.sigma2`
  （`ocudu_mmse_corr.metal:151`：`(p.sigma2_from_device != 0u) ? scalars[p.sigma2_slot] : p.sigma2`）。
- 而 `OCUDU_CE_K0A_RATIO_DEV` **默认 1**，腿日志 `device_sigma2=1589=lanes` 证明它每跳都在生效。
- Step 1（`2eca4a7965`）之后，CFO 也由**设备自己**在 K4 里读（轮转槽位）。

⇒ **宿主回读那三个标量，算出来的东西主要喂给了 GPU 已经不再读的参数位和宿主上报。**
这条是"可以直接砍"的最强候选——但**必须先用 A/B 证明**（见 `01_plan.md` S3）。

---

## 4. 已经达成、并且**没有退步**的部分（这是好消息）

对 `doc_chinese/work_tmp/logs/` 下**全部 24 条可用腿**（`gnb_fused_0917_2241` → `gnb_fence_0918_1805`）逐条扫描：

| 计数器 | 24 条腿的结果 | 含义 |
|---|---|---|
| `[ul_host] in_place` vs `symbols` | **每条都相等** | 每个符号都在**电台放它的地方**被处理 |
| `[ul_host] assembled` | **每条都是 0** | **零拷贝进符号缓冲，从未回退** |
| `[ul_host] cfo_round_trips` | **每条都是 0** | 从未因 CFO 补偿而往返 |
| `device_sigma2` | **每条都 == lanes** | K0-a 每跳在设备上产出比值 |
| `device_y_writes` | 每条都 > 0 | glue #2 在设备上写 y |
| `device_corr_builds` | 每条都 > 0 | K0-d 在设备上建矩阵 |

**⇒ 「拷贝意义上的 CPU 不在数据流里」已经做到，并且 168 天以来没有一条腿退步。**
**剩下的不是拷贝，是"每个模块边界/估计器内部的那次往返"。**

---

## 5. 挡在目标前面的那道闸：一句占位拒绝

`apps/units/flexible_o_du/o_du_low/du_low_config_validator.cpp:87-97`：

```cpp
if (config.phy_pipeline == "gpu") {
    // 第一层：车道所需的四个 Metal 后端必须链进来
    const std::string lane_error = check_phy_pipeline_lane_available(available);
    if (!lane_error.empty()) { fmt::print("Invalid configuration: {}.\n", lane_error); return false; }

    // 第二层：无条件拒绝（占位）
    fmt::print("Invalid configuration: --phy_pipeline gpu (the fused IQ -> LLR GPU pipeline) "
               "is not implemented yet; use --phy_pipeline cpu_gpu for the module-level offload.\n");
    return false;
}
```

> ⚠ **2026-09-23 里程碑审计更正：上面这段引用的是**当时的**代码，占位拒绝**已经删除**。**
> 今天 `du_low_config_validator.cpp` 的 `gpu` 分支只保留第一层（后端可用性检查），并在注释里写明
> "That refusal is gone"；空口腿已在 `mode=gpu` 下跑出 `contract MET (8 of 8)`（s47/s62/s63/s64b/s65/s67/s69）。
> **本文档 §2、§5 的其余内容描述的是"当时"的状态，读它们时请对照 §5.9.54/§5.9.119 的当前读数。**

- **第一层今天会过**：`check_phy_pipeline_lane_available()` 只要求 Metal 的 DFT / ch_est / equalizer / demapper
  链进来，而空口腿正在用它们。
- **第二层是纯占位**，注释写着 *"The lane itself lands in a later step of the GPU pipeline work"*。

**另一个必须知道的细节**：`lane_fused` 这个标志**今天只被打印**——
`du_low_phy_pipeline.h:226` 置位，`du_low_config_translator.cpp:48` 是**唯一**消费点（`? "yes" : "no"`）。
**⇒ 只摘掉拒绝，数据路径不会变**；`gpu` 模式解析出的后端与腿上已显式传的完全一致。
**真正的差距在 §3 的表里，不在模式名上。**

---

## 6. 真正的差距：一句话

> **不是"样本过宿主"（已清零），而是"估计器内部每跳仍有一次 dev→host→dev 的数据往返"：
> 宿主读走 3 个设备标量、算出统计量、再写回权重命令缓冲的参数。**
> 目标要求这条往返消失，只留 IQ 上传与 LLR 下载两次。

---

## 7. 尚未确认（不得当作已知）

| # | 问题 | 为什么重要 | 怎么确认 |
|---|---|---|---|
| 1 | **契约里没有任何一条检查在数"还剩几次穿越"** | 目标的性质**今天不可测**——7 条检查全过，但没有一条陈述"两次穿越"这件事 | 需要新增一条检查（`01_plan.md` S2） |
| 2 | 宿主建矩阵（`:1440/:1720/:1858/:1953`）在**当前 testbed** 上各占多少跳 | 它决定 §3 第 4 行要不要现在做。旧文档引用的 `26761 / 71384`（37.5%）来自 **b22 腿**，**不是我们量的** | 探针计数（S2） |
| 3 | LLR 回传的**形态**（拷贝还是零拷贝 wrap） | 目标是"允许 LLR 下载"，但如果有隐藏拷贝，它也在数据流里 | 读 demapper→LDPC 的交接代码 |
| 4 | IQ 从电台缓冲到 DFT 之间是否有隐藏拷贝 | 目标允许"IQ 上传"，但要确认它是零拷贝（`wrap_copies=0` 是强证据，仍需确认路径） | 读 DFT engine 的输入绑定 |
| 5 | `[ul_host] metrics` 的语义 | 未确认，暂不引用 | 读实现（已读计数器定义，但未确认调用点） |

---

## 8. 术语与出处

| 记号 | 意思 |
|---|---|
| `:NNNN` | `lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_impl.cpp` 的行号 |
| 腿 `gnb_xxx_MMDD_HHMM` | `doc_chinese/work_tmp/logs/gnb_xxx_MMDD_HHMM.log(.stderr)` |
| 契约 7 条 | 每次 gNB 退出时由 `report_phy_pipeline_contract()` 打印 |
