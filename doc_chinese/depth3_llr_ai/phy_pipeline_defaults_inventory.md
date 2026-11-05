# PHY pipeline 各模块默认设置清单（2026-11-05 按**代码**核实，不以注释为准）

> **方法**：每一条默认值都追到一段**可执行的代码**（文件:行）。注释只在能表明"这是刻意选择"时引用。
> **凡发现注释与代码不一致的，已改注释**（见 §7）。

---

## 0. 先回答三个问题

| 问题 | 代码的答案 |
|---|---|
| ★ `OCUDU_DEPTH3_FALLBACK` 默认是 `none`（PURE）吗？ | ★ **不是**。成员默认 `fallback_enabled = true`（`pusch_depth3_receiver_impl.cpp:2124`），env 只用来**关闭**它（`:396`）⇒ **app 默认 fallback 开** ✓<br>**但**：测量脚本 `run_arm_ext.py:67` **总是**设 `none` ⇒ **所有 PK 测量都是 PURE** ✓ —— 这就是"默认 pure"印象的来源 ✓ |
| ★ 网络会拒收/擦除吗？ | ★★ **策略性拒收已默认关闭**：域门 `domain_gate` 成员默认 `"none"`（`:2175`，2026-11-03 用户指示），startup grace 默认 `0`（`du_low_config.h:95`）⇒ **正常接收一律计算** ✓<br>**剩下的拒收只有物理失败**（见 §5）|
| ★ 注释与代码不一致？ | ★ **发现 3 处，已改**（§7）|

---

## 1. pipeline 模式

| 旋钮 | 默认 | 解析逻辑 | 代码 |
|---|---|---|---|
| `--expert_phy.phy_pipeline`（实链）<br>`--phy_pipeline`（回放）| **`auto`** | ★ `auto` ⇒ **有任一模块旋钮非 CPU 就是 `cpu_gpu`，否则 `cpu`** | `du_low_config.h:31`；`du_low_phy_pipeline.h:213–218` |
| 回放器 | **无默认**（必须显式给） | 不给模式即报错，避免"静默跑 CPU 链" | `ul_chain_replay.cpp` 的 `mode_given` 检查 |

**四种模式的模块归属**（`du_low_phy_pipeline.h` 的 `switch`）：

| 模式 | DFT | 信道估计 | 均衡 | 解映射 | LDPC | `lane_fused` |
|---|---|---|---|---|---|---|
| `cpu` | **cpu** | **cpu** | **cpu** | **cpu**（跟随均衡）| 旋钮决定（默认 CPU）| false |
| `cpu_gpu` | 跟随旋钮（`auto`⇒cpu）| 同上 | 同上 | 同上 | 同上 | false |
| `gpu` | ★ **cpu**（`lane_defaults::dft`，2026-09-30 起）| ★ **metal_mmse** | ★ **metal** | ★ **metal**（跟随均衡）| **旋钮决定，默认 CPU** | true |
| **`npu`** | **cpu** | **cpu** | **cpu** | **cpu** | **旋钮决定，默认 CPU** | false |

---

## 2. 每模块默认（实链 app）

| 模块 | 旋钮 | 默认值 | `auto` 解析到 | 代码 |
|---|---|---|---|---|
| **DFT** | `--pusch_dft_type` | `auto` | 模式决定：`cpu/cpu_gpu/npu`⇒`cpu`；`gpu`⇒`cpu`（lane 默认）| `du_low_config.h:208`；`du_low_phy_pipeline.h:156` |
| **信道估计** | `--pusch_channel_estimator_algo` | `auto` | `cpu/cpu_gpu/npu`⇒`cpu`；`gpu`⇒`metal_mmse` | `du_low_config.h:168`；`:155` |
| **CE 频域平滑** | `--pusch_channel_estimator_fd_strategy` | 见配置 | 透传 | `du_low_config.h` |
| **CE 时域策略** | `--pusch_channel_estimator_td_strategy` | **`interpolate`** | — | `du_low_config.h:159` |
| **均衡** | `--pusch_channel_equalizer_backend` | `auto` | `cpu/cpu_gpu/npu`⇒`cpu`；`gpu`⇒`metal` | `du_low_config.h:195`；`:157` |
| **解映射** | ★ **无独立旋钮** | — | ★ **跟随均衡**：`equalizer=="metal"` ⇒ Metal 解映射，否则 CPU generic | `upper_phy_factories.cpp:823` |
| **LDPC 解码器** | `--pusch_ldpc_decoder_type` | **`auto`** | ★ `auto` ⇒ **aarch64: neon**；x86: avx512→avx2→generic；**Metal 只有显式要求才用** | `du_low_config.h:135`；`channel_coding_factories.cpp:88–160` |
| **LDPC 编码/速率匹配** | （无旋钮）| **硬编码 `auto`** | — | `du_low_config_translator.cpp:173,179` |
| **device resource grid** | `--device_resource_grid` | `auto` | `gpu`⇒**on**，其余⇒off | `du_low_config.h:43`；`du_low_phy_pipeline.h:220+` |
| **depth3 接收机** | `--pusch_receiver_backend` | `auto` | ★ `auto`⇒**`classic`**；**`npu`⇒`ai`**（2026-11-05 起）| `du_low_config.h:66`；`du_low_phy_pipeline.h:246+` |
| **depth3 模型** | `--pusch_receiver_model` | **空** | ★ 空⇒**构建默认 E4**（`OCUDU_DEPTH3_DEFAULT_MODEL`＝`d3_e4.mlmodelc`）| `du_low_config.h:72`；`du_low_phy_pipeline.cpp:36` |
| **上报量来源** | `--pusch_reporting_source` | **`classic`** | PURE 下被强制为 `net` | `du_low_config.h:90`；`pusch_depth3_receiver_impl.cpp:408` |
| **startup grace** | `--depth3_startup_grace_s` | **`0`**（关闭）| — | `du_low_config.h:95` |

---

## 3. depth3 臂内部开关（环境变量，构造函数读一次）

| 环境变量 | 默认 | 语义 | 代码 |
|---|---|---|---|
| **`OCUDU_DEPTH3_FALLBACK`** | ★ **开**（`true`）| `none`/`0`/`n*` ⇒ **PURE：不调经典估计器、不交接** | `:2124`、`:396–414` |
| **`OCUDU_DEPTH3_DOMAIN_GATE`** | ★ **`none`（关）** | `none` 关；`net` 用臂自己的平面残差；`estimator` 用经典估计器 SNR | `:2175`、`:303–315` |
| **`OCUDU_DEPTH3_COMPUTE_UNITS`** | **`all`** | Core ML 计算单元（`all`/`cpu_only`/…）；非 `all` 会降速，代码会强制改回并告警 | `:44–58`、`:383–391` |
| **`OCUDU_DEPTH3_SHARED_SINR_CAL`** | **每调制一组常数**（QPSK 0.982/+1.27，16QAM 1.055/−0.13，64QAM 1.073/−0.35，256QAM 0.963/+2.97）| 共享测量层的 SINR 校准 | `:333–350` |
| **`OCUDU_DEPTH3_PREPARE_FULL_PLANE`** | **关** | 早期测量是否组装完整 14 符号平面（默认只组装 DM-RS 两符号）| `:831–836` |
| **`OCUDU_DEPTH3_GATE_REPORT`** | **开** | 是否打印域门统计 | `:317–319` |

**⇒ PURE（fallback=none）时会连带强制**：`domain_gate` ⇒ `net`（若原为 `estimator`）、`reporting_source` ⇒ `net` ✓（`:400–414`）

---

## 4. 回放器侧默认

| 项 | 默认 | 说明 |
|---|---|---|
| `--phy_pipeline` | **必须显式** | 与实链同名同值（`auto/cpu/cpu_gpu/gpu/npu`）|
| `--pusch_ldpc_decoder_type` | `auto` | ⇒ neon / avx / generic；**Metal 需显式** |
| `--pusch_receiver_backend` | 空 | 空 ⇒ 无 depth3 臂（经典链）；`ai` ⇒ 臂 |
| `OCUDU_REPLAY_MODEL` | 空 | 空 ⇒ `ai_identity`（**委托经典链，不是 AI 结果**）|
| `OCUDU_DEPTH3_FALLBACK` | ★ **由脚本设 `none`** | `run_arm_ext.py:67` ⇒ **所有测量都是 PURE** |

---

## 5. 拒收/擦除（degradation）清单 —— 哪些是策略，哪些是物理

**⇒ 统一入口**：`degrade_or_fail(reason, …)`（`:1720–1732`）：**fallback 开 ⇒ 交给经典链；fallback 关（PURE）⇒ 擦除（写零 ⇒ 必 KO）并计数** ✓

| # | 原因 | 触发条件 | 性质 | 默认会发生吗 |
|---|---|---|---|---|
| 1 | `startup_grace` | 启动宽限期内 | ★ **策略** | ★ **不会**（默认 0 s）✓ |
| 2 | `measurement_outside_domain` | 上报量超出模型训练域 | ★ **策略** | ★ **不会**（域门默认关）✓ |
| 3 | `modulation_mismatch` | 绑定的模型不覆盖该 `qm` | 物理（缺模型）| 不会（E4 覆盖 Qm 2/4/6/8）|
| 4 | `no_sequence_context` | 没先调 `estimate()` | 物理（调用顺序）| 不会（链固定先 estimate）|
| 5 | `plane_refused`（3 处）| 平面组装失败（栅格/分配不符）| 物理 | 罕见 |
| 6 | `forward_failed` | Core ML 前向失败 | 物理 | 罕见 |
| 7 | `no_data_resource_elements` | 该接收没有数据 RE | 物理（空接收）| 罕见 |
| 8 | （域门 `net` 分支）| 仅当显式开域门 | 策略 | 不会 |

**⇒ 结论**：**1、2 两条策略性拒收已默认关闭** ⇒ 正常接收**一律计算** ✓；剩下的都是"真的算不了" ✓

---

## 6. 一句话总表（npu 口径下实际生效的默认）

```
--phy_pipeline npu
  ⇒ DFT cpu | 估计 cpu | 均衡 cpu | 解映射 cpu（跟随均衡）| LDPC neon(auto) | lane_fused=false
  ⇒ receiver = ai（隐含）| model = 构建默认 E4
  ⇒ fallback = none（隐含 PURE）⇒ 不调经典估计器、上报量来自共享测量层
  ⇒ domain_gate = none | startup_grace = 0 | compute_units = all
```

---

## 7. 注释与代码不一致（已修正）

| # | 位置 | 旧注释的问题 | 已改为 |
|---|---|---|---|
| 1 | `pusch_depth3_receiver.h`（`ai_network`）| 写"`estimate()` still runs the classical estimator"，**与 PURE 的实际代码不符**（PURE 下直接 return，不调估计器）| 明确写**取决于 fallback**：开⇒转发经典；关⇒不调、用共享测量层 |
| 2 | `du_low_config.h`（`gpu` 模式说明）| 写"lane takes over the DFT…"，**DFT 自 2026-09-30 起默认就是 CPU** | 改为"接管估计/均衡/解映射；DFT 默认 CPU（`--pusch_dft_type metal` 才上设备）；补 `npu` 说明" |
| 3 | `include/ocudu/phy/phy_pipeline_mode.h`（`gpu` 条目）| 同上"takes over the backends of the modules it contains" | 同上，点名 DFT 与 LDPC 都不属于 lane |

**⇒ 另核对了 `du_low_config.h:29` 与 `phy_pipeline_mode.h:19` 的其余措辞**：LDPC"不属于 lane"**与代码一致**（`du_low_phy_pipeline.h` 的 `gpu` case 不动 `out.ldpc`）✓ 保留。

---

## 8. 由此产生的两个待办

1. ★ **PURE 下的实链吞吐/丢块率**必须实测：策略性拒收虽已关，但物理失败在 PURE 下会"整接收擦除"⇒ 只有实测能说明代价 ✓
2. ★ **经典估计器在 PURE 下已不跑**（§3），所以"npu 下还有 CE"只可能出现在**没走 PURE** 的运行（fallback 开着）⇒ 看日志的 `PURE mode` 行即可判定 ✓
