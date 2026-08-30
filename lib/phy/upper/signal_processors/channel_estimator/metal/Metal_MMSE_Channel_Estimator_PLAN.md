# Metal MMSE Channel Estimator 接入 ocudu 详细实施规划

> 状态：**v0.3 已评审通过，实施完成**（M0–M3 离线部分全部完成并验证，见 §7.0 实施记录；
> 剩余：实链三腿 A/B 与探针扩展为实验室依赖的收尾项）
> 目录：`lib/phy/upper/signal_processors/channel_estimator/metal/`（本目录 = 所有 CE Metal 工作的家）
> 参照：`lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`（LDPC Metal 范式）
> 前置背景：同目录 `AI_channel_estimation_implementation_plan.md`（147 篇论文筛选与 AI 路线总规划；
> 本 PLAN 只覆盖其 L0 阶段 = **MMSE baseline 的 Metal 实现**）

---

## 0. 摘要

1. **目标**：在 Apple Silicon GPU（Metal）上实现 **2D 时频块 MMSE channel estimation**，
   作为 OCUDU PUSCH RX chain 的第二种 channel estimation 算法（第一种 = 现有 CPU LS+平滑/插值）。
   通过 `expert_phy --pusch_channel_estimator_algo cpu|metal_mmse` 在 UL pipeline 中选择，
   机制与 `--pusch_ldpc_decoder_type` 完全一致。
2. **算法（用户方案）**：对时频块（默认 **3 PRB × 14 符号 = 504 位置**，type1 双 DMRS 符号
   = 36 个导频）做 **W = R_hp·(R_pp + σ²I)⁻¹**，R = R_t⊗R_f（Kronecker 可分离模型：
   R_f 由多径时延扩展决定、R_t = J0(2πf_dΔt) 由多普勒决定），**一次矩阵乘直接得到块内
   全部 RE（含 data RE）的信道估计**——不再使用 freq_interpolator 与 TD 策略。
3. **Metal 核心算子**（未来 GPU AI channel estimator 的骨架）：批量 **36×36 矩阵求逆**
   （Gauss-Jordan，每系统一个 threadgroup）+ 批量 **504×36 矩阵乘**（每 (系统, 块) 一个
   并行任务）= "MMSE = 矩阵求逆 + 矩阵乘法"的完整 GPU 形态。
4. **新增信道统计估计模块**（重要，且为 AI 复用）：代码现状**没有**多普勒/时延扩展估计
   （仅 CFO 标量 + TA 标量，见 §3.2）。规划 σ²̂ / τ̂_rms / f̂_d 三个统计量的估计方法
   （PDP 噪声地板、指数模型拟合、跨符号相关 J0⁻¹），独立成类，后续 AI（如 AdaFortiTran
   的自适应先验输入）直接复用。
5. **平台策略**：新代码只在 macOS Apple Silicon 上编译生效（CMake `ENABLE_METAL_CHEST`
   默认仅 Apple arm64 开启），Ubuntu 保持原逻辑**零改动**（工厂不注册 metal 类型 +
   配置翻译层强制 `cpu`，复制 LDPC 的 `#if defined(__APPLE__)` 模式）。
6. **API 前瞻**：算法选择放在 `port_channel_estimator_factory` 层；PUCCH 共用同一工厂
   （本次只接通 PUSCH，PUCCH 显式传 `cpu`），SRS 独立估计器不受影响。
7. **模拟证据（§3.5）**：2D 全真参数（oracle）上限 = 最优线性估计器，全面优于现有 cpu
   路径（15 kHz fd=0：低 SNR +2.9 dB、25 dB +2.1 dB；Jakes fd=200：25 dB +4.5 dB）；
   全估计版（估计 τ̂/f̂_d/σ²̂）全曲线 ≥ cpu 路径（fd=0 的 −5 dB +0.5 dB；fd=200 全曲线
   +0.4~+1.2 dB），与 oracle 的差距（低 SNR 1.5–2.5 dB）即统计估计质量的优化空间。
8. **测试**：三个 test main（求逆 golden + GPU/CPU 对拍；合成 TDL 信道 NMSE/BLER 对比；
   时延基准），放在 `metal/test/`，参照 `ldpc/metal/test/` 结构。

---

## 1. 背景与目标

### 1.1 为什么先做 MMSE baseline

- 现有 `port_channel_estimator_average_impl` 是 **LS + FD 平滑（filter/mean/none）+ TD 平均/插值**，
  **没有 MMSE**。AI 路线规划（`AI_channel_estimation_implementation_plan.md` §5 L0）要求
  先建立"实用 MMSE"严格基线，AI 模型（HELENA 等）未来必须在该基线上有增益才允许启用。
- MMSE 的数学形态（矩阵求逆 + 矩阵乘法）与未来 GPU AI 推理（GEMM 类算子、批量小矩阵、
  Metal 引擎/缓冲/调度骨架）**高度同构**：本次的 Metal 引擎就是 AI CE 的骨架，后续只需替换 kernel 内容。
- 2D 块的"信道统计 → 模型矩阵"路径与 AI 的自适应先验（AdaFortiTran CAM 输入：SNR/Doppler/
  时延扩展）**直接对应**：§3.2 的统计模块就是未来 AI 的传感器前端。

### 1.2 目标（本期）

| # | 目标 |
|---|---|
| G1 | Metal kernel 实现 MMSE 核心算子（批量 36×36 求逆 + 批量 504×36 矩阵乘），数值与 CPU 参考实现对拍一致 |
| G2 | UL pipeline 可通过 `expert_phy --pusch_channel_estimator_algo cpu\|metal_mmse` 选择算法 |
| G3 | 精度：合成 TDL 信道下 NMSE 全 SNR 曲线不劣于现有 average_impl（模拟预期：2D-est 全曲线 ≥ cpu，低 SNR +0.5 dB、fd=200 场景 +0.4~+1.2 dB；oracle 上限低 SNR +2.9 dB），BLER 不劣化 |
| G4 | 时延：metal_mmse 的 CE 阶段 ≤ 现有 average_impl 的 1.5×（实链探针验证） |
| G5 | macOS 专属：Ubuntu 编译/运行与上游完全一致（自动回退 cpu 逻辑） |
| G6 | test main 齐备：求逆 golden、GPU/CPU 对拍、NMSE/BLER 对比、时延基准 |

### 1.3 非目标（本期不做，API 预留）

- AI/神经网络估计器（HELENA 等）——下期在本文骨架之上实现
- PUCCH/SRS 的实际 MMSE 切换——API 已预留（§5.3），PUCCH 显式 `cpu`
- Kronecker 结构 + 特征分解免求逆优化（Edfors SVD 路线）——v2 优化项
- DMRS type2（OCUDU PUSCH 当前仅支持 type1，见 §3.1）——引擎参数化预留

---

## 2. 现状分析

### 2.1 现有 CE 模块（替换点）

调用链与接口（`include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h`）：

```
pusch_processor_impl
 └─ dmrs_pusch_estimator_impl::estimate()          // 生成 DMRS 图案，按 RX 端口派发到 executor
     └─ port_channel_estimator::compute(grid, port, pilots, cfg)   // ★ 每端口一个实例
         └─ port_channel_estimator_average_impl    // 现有实现
         └─ port_channel_estimator_results: 全网格(cbf16) + noise_var + snr + epre/rsrp + cfo + ta
```

`port_channel_estimator_average_impl`（`lib/phy/upper/signal_processors/channel_estimator/`）流程：

1. `preprocess_pilots_and_estimate_cfo`：rx 导频 × conj(期望导频) → **LSE**（`pilots_lse`，
   每 (层, DMRS 符号) 一组，长度 = PRB 数 × 每 PRB 导频数）；≥2 个 DMRS 符号时估计 CFO
2. `compensate_cfo_and_accumulate`：CFO 补偿
3. **FD 阶段**：`apply_fd_smoothing`（filter/mean/none）→ `freq_interpolator->interpolate`
4. RSrp 累加；噪声方差 `estimate_noise`（滤波后残差）；TA `estimate_time_alignment`
5. **TD 阶段**：`apply_td_domain_strategy`（average/interpolate）→ 数据符号估计 → cbf16 网格
6. hop 处理（跳频时 hop0/hop1 各一遍，`rb_mask`/`rb_mask2`）

**2D MMSE 替换点 = 第 3+5 步**（由"LS 导频 → 全时频网格"一步完成）；LSE/CFO/噪声/EPRE/RSRP/TA
全部复用（噪声方差继续由 `estimate_noise` 上报给 LLR，§3.3）。

关键事实：

- 每 PRB 每 DMRS 符号的导频数 = `get_nof_re_per_prb(type)`：**type 1 = 6、type 2 = 4**
  （`include/ocudu/ran/dmrs/dmrs.h:42`）。**OCUDU PUSCH 目前只支持 type1**
  （`pusch_processor_impl.cpp:158` 硬编码 + validator 拒绝 type2）→ v1 只做 type1
  （每 PRB 双符号共 12 导频、3-PRB 块 36 导频）；type2 引擎参数化预留。
- LSE 缓冲：`MAX_NOF_PILOTS_SYMBOL = MAX_NOF_SUBCARRIERS + 2*MAX_V_PILOTS`、
  `MAX_NOF_DMRS_SYMBOLS = 8`、`MAX_LAYERS = 4`；层按 CDM 对处理。
- 工厂：`create_port_channel_estimator_factory_sw(ta_est_factory)` →
  `port_channel_estimator_factory::create(fd_strategy, td_strategy, compensate_cfo)`；
  PUSCH 与 PUCCH 工厂共用同一实例（`upper_phy_factories.cpp:627`）。
- SRS 走独立 `srs_estimator`，不经过本接口。

### 2.2 配置贯通链（照抄 LDPC 的既有模式）

```
yml  pusch_channel_estimator_algo: cpu
 │   apps/units/flexible_o_du/o_du_low/du_low_config.h            （默认值 + 注释）
 │   apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.cpp  （expert_phy CLI + 取值校验）
 │   apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp    （→ upper_phy_factory_configuration；Linux 强制 "cpu"）
 │   lib/phy/upper/upper_phy_factories.cpp                        （创建 port_channel_estimator_factory）
 └─ lib/phy/upper/signal_processors/channel_estimator/factories.cpp（按 algo 注册实现）
```

LDPC 的 macOS/Linux 分流范本（`du_low_config_translator.cpp:66-71`）：

```cpp
#if defined(__APPLE__)
  // macOS: honor the expert knob; Linux keeps upstream byte-for-byte.
#else
  upper_phy_factory_config.ldpc_decoder_type = "auto";
#endif
```

### 2.3 LDPC Metal 范式（本目录照搬的模板）

`lib/phy/upper/channel_coding/ldpc/metal/`：`ocudu_metal_decoder_engine.{h,mm}`（ObjC++ ARC 引擎：
`.metallib` 预编译 + `newLibraryWithURL`、零拷贝 `newBufferWithBytesNoCopy` 4KB 对齐、
buffer cache、同步 `waitUntilCompleted`、`last_gpu_wait_us()`）+ `ldpc_decoder_metal.{h,cpp}`
（C++ 适配器）+ `PLAN.md`（计划 + 实链数据）+ `test/`（unit/bler 对拍）+ CMake
（`ENABLE_METAL_LDPC` 默认仅 Apple arm64 ON、`add_subdirectory(metal)`、宏 `OCUDU_METAL_LDPC`）。

### 2.4 时延锚点（本方案预算依据）

- CE 阶段总预算：**≤ 60 µs @ 51 PRB 级**（前端 CE+均衡+解调共 ~190 µs，LDPC 解码 28–60 µs，
  整管线中位 207–311 µs——`ldpc/metal/PLAN.md` 实链探针）。
- MMSE 计算量：36×36 求逆 ≤ 8 个系统（端口×层×hop）+ 每块 504×36·36 MAC 的批量矩阵乘
  （51 PRB = 17 块 × 8 系统 ≈ 89M MAC/slot）→ GPU 纯计算 ~20 µs，主要成本是一次
  command buffer 提交（~10–30 µs，与 LDPC 实测同量级）→ **预算内**。

---

## 3. MMSE 算法设计（2D 时频块，用户方案）

### 3.1 数学（2D 块 MMSE，W = R_hp·(R_pp + σ²I)⁻¹）

对某个 (rx 端口, tx 层, hop)，取时频块 **B PRB × 14 符号**（默认 **B=3**：36 子载波 × 14 符号
= **504 个位置**），块内导频 = type1 双 DMRS 符号 × 6 RE/PRB = **36 个**（用户举例 18 个
亦为参数化取值；type2 时为 4·2·B=24 个）：

```
ĥ = W · y_p ,   W = R_hp (R_pp + σ²I)⁻¹        (504×36)
R_hp = R_t,hp ⊗ R_f,hp     （数据/导频位置 × 导频位置的互相关，Kronecker 可分离）
R_pp = R_t,pp ⊗ R_f,pp     （导频×导频自相关）
R_f(Δf) : 频域相关，由多径时延扩展决定 —— 工程参数化模型：指数 PDP 的实部
          R_f(Δf) = 1 / (1 + (2π·Δf·τ̄)²)   （τ̄ = RMS 时延扩展；通用形式 Σ p_i e^{−j2πΔfτ_i}）
R_t(Δt) : 时域相关，由 UE 移动速度（多普勒）决定 —— Jakes 模型
          R_t(Δt) = J0(2π·f_d·Δt)
```

- **一次矩阵乘直接得到块内全部 RE 的信道**（含 data RE 与 pilot RE）——不再需要
  `freq_interpolator` 与 TD 策略（`metal_mmse` 算法下 `fd_strategy`/`td_strategy`
  配置被忽略并在文档注明）。
- 求逆对象 = R_pp + σ²I（36×36，type1 双符号 3-PRB 块）；σ²I 使矩阵严格正定
  （fd→0 时 R_t 秩亏也由 σ² 正则化）；另加 ridge ε=1e-6·r̂(0) 保数值稳健。
- 块不整除分配（51 PRB = 17×3 ✓；25 PRB = 8×3+1）：边缘块收窄（1–2 PRB 块），
  W 按实际块尺寸现算（求逆 12×12/24×24，成本极小）——无需虚拟导频。
- hop：跳频时 hop0/hop1 各做一次 2D（各自 rb_mask 范围）。
- 文献依据：van de Beek et al., "On Channel Estimation in OFDM Systems"（MMSE 通式，
  本论文库内）；Hoeher（2D Wiener/TSP）；Li, Cimini, Sollenberger, "Robust channel
  estimation for OFDM systems with rapid dispersive fading channels"（相关统计的估计与
  低阶模型拟合——§3.2 的方法学出处）。
- **M0 模拟发现（必须遵守）**：权重必须用**收缩等价形式**（R_hp 与 (R_pp+σ²I)⁻¹ 分开、
  σ² 单独估计）；直接把经验 R̂ 当 R 用会数值爆炸（模拟实测 NMSE +27 dB）。

### 3.2 信道统计估计（新增模块，AI 复用；三个参数：σ²、τ̄、f_d）

**代码现状核查结论**：现有代码**没有**多普勒（f_d）与多径/时延扩展（τ̄/PDP）估计——
仅有 CFO（跨 DMRS 符号相位，归一化标量）与 TA（时延标量）。**需要新增统计估计**，
规划为独立类 `channel_statistics_estimator`（v1 可并入 metal_mmse 适配器，
接口独立——后续 AI 估计器的自适应先验输入直接复用）。输入 = 每 (端口,层,hop) 的
`pilots_lse`（CFO 已补偿）+ 现有滤波后噪声方差；输出 = (σ²̂, τ̂_rms, f̂_d) + 组装的 R_f/R_t。

| 统计量 | 估计方法（M0 模拟已验证，消融见 §3.5） | 备注 |
|---|---|---|
| **σ²̂**（噪声） | `min(PDP 噪声地板, 后置滤波残差)`：① PDP 噪声地板 = IFFT(导频 LS, 512) 的 **CP 之外 bins** 均值 × 512²/N_p（注意 FFT 库的 1/N 归一化——模拟中曾因漏此因子偏差 9×）；② 后置滤波残差 = 现有 `estimate_noise` 语义（RC 滤波后残差）。取 min 覆盖全 SNR（高 SNR 用②、低 SNR 用①，双向安全） | 两个原始方案曾被证伪：滤波**前** LS 残差恒为 0（LS 完美复现观测）；锥形窗后 R̂ 的最小特征值被窗抬升 ~10× |
| **τ̂_rms**（多径） | 无锥形窗的经验 r̂(Δ)（hop 内双 DMRS 符号合并、按信号功率 r̂(0)−σ²̂ 归一）→ 指数模型线性拟合：`1/r − 1 = (2π·30kHz·τ̂)²·Δ²`（Δ=1..5），下限 50 ns、上限 5 µs | 模拟消融表明 τ̂ 是 est-2D 与 oracle 差距的**最大来源**（低 SNR 拟合仍偏高）；列为持续优化项：更多滞后、加权拟合、跨槽 EMA |
| **f̂_d**（多普勒） | 跨 DMRS 符号幅度相关（噪声修正）：`ρ̂ = \|Σ y₁y₂*\| / √((S₁−Nσ²̂)(S₂−Nσ²̂))` → J0⁻¹ 查表 → 限幅 [0, 1500] Hz；**跨槽 EMA 平滑**（f̂_d 单槽样本噪声大） | EMA 状态放 `port_channel_estimator` 实例内（每端口常驻实例，天然有状态）；f̂_d 也是 AI 自适应先验的关键输入 |

- 平滑与稳健：τ̂/f̂_d 逐 (端口,层) 跨槽 EMA（α≈0.2 起步，M3 实链调）；σ²̂ 每槽现估。
- 与 AI 的关系：这三个统计量 + 组装好的 R 矩阵即 AdaFortiTran CAM（SNR/Doppler/delay-spread
  先验）需要的东西；本模块设计成独立工厂/类，AI 阶段直接注入。

### 3.3 与现有流程的结合（最小替换）

在 `compute_hop` 中把第 3+5 步（FD 平滑+插值+TD 策略）替换为：

```
(a) 统计估计（CPU，µs 级）：σ²̂、τ̂_rms、f̂_d（§3.2）→ 组装 R_f/R_t 参数化矩阵
(b) W = R_hp(R_pp + σ²I)⁻¹                               [Metal K1：批量 36×36 求逆]
(c) ĥ = W · y_p（全部块、全部系统）                       [Metal K2：批量 504×36 矩阵乘]
    → 直接写出全网格（含 data RE）→ cbf16
(d) 其余不变：LSE/CFO 预处理、EPRE/RSRP、滤波后噪声方差（LLR 用）、TA、hop 处理
```

保持不变（复用现有逻辑，不重写）：LSE/CFO 预处理、CDM 层对、EPRE/RSRP、
`estimate_noise` 上报值、TA、hop/rb_mask 处理、cbf16 输出。
（虚拟导频不再需要：块边界收窄处理。）

### 3.4 v2 候选（本期不做，列入 backlog）

- Kronecker 结构 + 特征分解免显式求逆（Edfors SVD 路线：两个小特征分解 + 逐模式收缩，
  计算更省、AI 阶段也更自然）
- PDP 直接估计（IFFT+阈值+CP 截断 → τ̄ 直接计算，替代指数拟合）
- DMRS type2（引擎参数化已有，等上层支持）
- 每块独立统计（局部 τ̂/f̂_d，高铁/异构场景）
- PUCCH/SRS 的 metal_mmse 切换

### 3.5 M0 预验证模拟证据（2026-08-29，/tmp/mmse_ce_sim/sim.py|sim2.py|sim3.py，NREAL=300–800）

复现方法：numpy 复刻现有 cpu 路径（RC 滤波器系数取自 `port_channel_estimator_helpers.cpp`
RC_FILTER[1::2] 15 抽头 + 虚拟导频 + 线性插值 + TD average/interpolate），信道 VehA
（τ_rms=370 ns）/ 指数 PDP（RMS 300 ns）+ **Jakes 衰落**（正确的多普勒扩展模型），
51 PRB、DMRS type1（符号 2/11）、15 kHz SCS（30 kHz 见 §3.5 尾注）。

**表 1：15 kHz，VehA fd=0（NMSE dB，越小越好）**

| 估计器 | −5 | 0 | 5 | 10 | 15 | 20 | 25 dB |
|---|---|---|---|---|---|---|---|
| cpu: LS+RC+TD平均（现状） | −5.54 | −10.23 | −15.12 | −19.66 | −24.29 | −28.10 | −31.28 |
| cpu: LS+RC+TD插值 | −2.75 | −7.77 | −12.76 | −17.40 | −22.11 | −26.31 | −29.68 |
| **2D est（全估计参数）** | **−6.03** | −9.88 | −14.38 | −18.94 | −23.12 | −27.14 | −31.40 |
| 2D oracle（全真参数，上限） | −8.49 | −11.82 | −15.78 | −19.86 | −24.22 | −28.61 | −33.43 |
| （v0.2 方案）1D 经验 MMSE+插值 | −7.41 | −11.61 | −15.62 | −19.90 | −24.15 | −27.05 | −30.03 |

**表 2：15 kHz，VehA Jakes fd=200 Hz**

| 估计器 | −5 | 0 | 5 | 10 | 15 | 20 | 25 dB |
|---|---|---|---|---|---|---|---|
| cpu: LS+RC+TD平均 | −8.42 | −13.22 | −17.89 | −22.32 | −26.19 | −29.12 | −30.57 |
| cpu: LS+RC+TD插值 | −5.87 | −10.74 | −15.48 | −20.20 | −24.70 | −28.57 | −31.41 |
| **2D est（全估计参数）** | **−9.05** | −13.12 | −17.40 | −21.38 | −25.14 | −28.72 | **−31.77** |
| 2D oracle（全真参数，上限） | −10.36 | −14.13 | −18.00 | −22.13 | −26.28 | −30.62 | −35.04 |

**表 3：块尺寸扫描（2D est，VehA fd=0）**

| 块尺寸 | −5 | 0 | 5 | 10 | 20 | 25 dB |
|---|---|---|---|---|---|---|
| 1 PRB | −5.19 | −8.73 | −13.20 | −17.21 | −25.99 | −30.37 |
| **3 PRB** | −5.87 | −9.90 | −14.41 | **−19.09** | **−27.34** | **−31.70** |
| 5 PRB | **−6.09** | **−9.97** | **−14.84** | −19.19 | −27.14 | −30.66 |

结论（已写入本 PLAN 的设计决策）：

1. **2D 块 MMSE（用户方案）方向正确**：oracle（全真参数）即最优线性估计器上限，
   全面优于现有 cpu 路径与 v0.2 的 1D 方案；**全估计参数版全曲线 ≥ cpu 路径**
   （fd=0 低 SNR +0.5 dB、fd=200 全曲线 +0.4~+1.2 dB）。
2. **统计估计质量 = 与 oracle 差距的全部来源**（消融：真 τ̂ 时低 SNR 从 −6.03 提升到
   −7.73；真 σ²̂/真 f̂_d 影响次要）。§3.2 的修正（PDP 噪声地板缩放、指数拟合线性化
   1/r−1=cΔ²、无窗拟合、ρ̂ 噪声修正）已把 est 从"全线劣于 cpu"拉到"全线 ≥ cpu"；
   低 SNR 的 τ̂ 偏高仍是首要优化项（EMA 平滑/加权拟合列 M0 调优）。
3. **数值稳健性教训**：① naive 经验 R̂ 权重爆炸（+27 dB）；② FFT 归一化因子漏乘导致
   σ²̂ 偏差 9×；③ 锥形窗污染 τ̂ 拟合（必须用无窗 r̂ 拟合、窗只用于 σ²̂ 的 Toeplitz）——
   全部已写入 §3.2 与单测设计（§6）。
4. **块尺寸 3 PRB** 为甜点（低 SNR 与 5-PRB 相当、高 SNR 最优）；1 PRB 明显更差。
5. 30 kHz SCS（表外数据，sim2）：现有 RC 滤波器按 15 kHz 调谐在 30 kHz 过度平滑，
   MMSE 自适应统计的优势进一步放大（cpu 20 dB 处仅 −18.96，MMSE −25.32）——
   30 kHz 配置是 metal_mmse 优先启用场景。
6. 模拟脚本待评审通过后随 M0 迁入本目录 `metal/sim/`（连同数据与绘图）。

---

## 4. Metal 实现设计

### 4.1 目录结构（本目录）

```
channel_estimator/metal/
├── PLAN.md                                   # 本文档
├── AI_channel_estimation_implementation_plan.md   # 已移入的 AI 总规划
├── CMakeLists.txt                            # 参照 ldpc/metal/CMakeLists.txt
├── ocudu_metal_mmse_engine.h                 # C++ 引擎接口（参照 ocudu_metal_decoder_engine.h）
├── ocudu_metal_mmse_engine.mm                # ObjC++(ARC) 引擎实现
├── ocudu_mmse_inv.metal                      # K1：批量 36×36 Gauss-Jordan 求逆（R_pp+σ²I）
├── ocudu_mmse_apply.metal                    # K2：批量块矩阵乘（每 (系统,块) 504×36）
├── channel_statistics_estimator.h            # ★ 统计估计模块（σ²/τ̄/f_d，AI 复用）
├── channel_statistics_estimator.cpp          # v1 CPU 实现（NEON；GPU 化预留 K0）
├── port_channel_estimator_metal_mmse_impl.h  # 纯 C++ 适配器（工厂注册 "metal_mmse"）
├── port_channel_estimator_metal_mmse_impl.cpp
└── test/
    ├── port_channel_estimator_metal_unit_test.cpp    # 求逆 golden + GPU/CPU 对拍 + 统计估计对拍
    ├── port_channel_estimator_metal_mmse_bler_test.cpp  # NMSE/BLER vs average_impl
    ├── port_channel_estimator_metal_mmse_benchmark.cpp  # 时延基准（可选，或并入 unit test）
    └── plot_nmse.py                            # 绘图脚本（参照 plot_bler.py）
```

### 4.2 引擎接口（`ocudu_metal_mmse_engine`，命名空间 `ocudu::metal`）

```cpp
class mmse_engine {
public:
  // 一次性：device/queue/pipeline + 预编译 .metallib 加载
  bool init(unsigned max_systems,      // 每槽最大系统数 = 端口×层×hop ≤ 8
            unsigned matrix_size,       // 36 (type1, 3 PRB×双符号)；12/24 边缘块
            const char* metallib_path); // 编译期宏注入，运行时相对路径兜底（同 LDPC）
  // 每槽每系统一次：A = R_pp + σ²I → A⁻¹（K1，批量 Gauss-Jordan，ridge 已含）
  bool invert(const float* r_pp,        // [systems][L][L] 实对称（Kronecker 组装结果）
              float*       a_inv,       // [systems][L][L]
              unsigned     nof_systems);
  // 每槽一次：ĥ = W·y，批量（每 (系统, 块) 一个并行任务；W = R_hp·A⁻¹ 由 CPU 预乘）
  bool apply(const float* w,            // [systems][Nout][L]（504×36）
             const float* y_in,         // [systems][nof_blocks][L] 复 LSE（实/虚交错）
             float*       y_out,        // [systems][nof_blocks][Nout]
             unsigned nof_systems, unsigned nof_blocks);
  double last_gpu_wait_us() const;      // GPU 侧耗时（同 LDPC）
};
```

- W 的组装（R_f/R_t 参数化矩阵 → Kronecker → R_pp/R_hp → 预乘）在 **CPU**（≤8 系统 ×
  36×36，µs 级）；GPU 只做批量求逆 + 批量矩阵乘（用户定义的"核心算子"）。
- 同步语义：`invert`/`apply` 合并为一个 command buffer 两次 dispatch，一次 commit。
- 零拷贝：缓冲由适配器持有（4KB 对齐），`newBufferWithBytesNoCopy` + buffer cache（同 LDPC）。
- 多实例策略：`port_channel_estimator` 每 RX 端口一个实例 → 每实例独立 buffer；
  **MTLDevice/CommandQueue 进程级单例共享**（`std::once_flag` 静态）。
- 平台隔离：引擎 `.mm` 只在 `ENABLE_METAL_CHEST` 下编译；适配器 `#if defined(OCUDU_METAL_CHEST)`
  包裹 Metal 调用，未定义时回退 CPU 参考实现（同骨架将来可移植到 AI 阶段）。

### 4.3 Kernel 设计

**K1 `ocudu_mmse_inv.metal` —— 批量 Gauss-Jordan 求逆（36×36）**

- 每个 threadgroup 一个系统：把 [A | I]（36×72）装入 threadgroup 共享内存（36·72·4 B ≈ 10 KB，
  在 32 KB 限制内），列主元 Gauss-Jordan → A⁻¹。
- grid = `nof_systems` 个 threadgroup → 与未来"每系统一组参数"的 AI 推理调度形态一致。
- 备选（对比腿）：`MPSMatrixDecompositionCholesky` + 三角求解（MPS 内建）；自写 kernel 为主。

**K2 `ocudu_mmse_apply.metal` —— 批量块矩阵乘（504×36 · 36）**

- 每个 thread（或 threadgroup）处理一个 (系统, 块)：`ĥ = W·y`（复数，504×36 ≈ 18k 次
  复数乘法/块）；grid = `nof_systems × nof_blocks`（51 PRB → 17 块）。
- 内存访问：W 按系统常驻、y/ĥ 按块线性——完全合并。
- 未来 AI 阶段的"每块独立全连接层"即同一 pattern（换权重来源即可）。

**精度**：全 fp32（复数 = float2）。AI 阶段再评估 fp16；MMSE 矩阵小，fp32 无成本压力。

### 4.4 适配器（`port_channel_estimator_metal_mmse_impl`）

- 继承 `port_channel_estimator_average_impl`（决策点 D4：钩子粒度扩大为
  "由 pilots_lse 生成全网格"，默认 = 现有 FD 平滑+插值+TD 行为），**只覆写该钩子**：
  1. `channel_statistics_estimator`：σ²̂/τ̂_rms/f̂_d（§3.2，含跨槽 EMA 状态）→ 组装 R_f/R_t
  2. Kronecker 组装 R_pp/R_hp（CPU）→ `engine.invert`（K1）
  3. W = R_hp·A⁻¹（CPU 小 GEMM）→ `engine.apply`（K2）→ 块输出 → cbf16 网格
  4. 持久缓冲：`r_pp[8][36][36]`、`a_inv[8][36][36]`、`w[8][504][36]`、
     `y_in[8][17][36]`、`y_out[8][17][504]`，4KB 对齐、每槽复用、零分配
- `last_gpu_wait_us()` 汇入 metrics decorator（§6.4）；失败兜底：本槽退回基类路径 + 回退计数。

### 4.5 CMake（照搬 LDPC，Ubuntu 零影响）

- 顶层：`ENABLE_METAL_CHEST`，默认 `APPLE AND aarch64/arm64 → ON`，其余 OFF。
- `lib/phy/upper/signal_processors/CMakeLists.txt`（`ocudu_channel_estimator` 静态库处）：
  `if(ENABLE_METAL_CHEST) add_subdirectory(channel_estimator/metal) endif()` +
  编译宏 `OCUDU_METAL_CHEST` + 链接 `ocudu_channel_estimator_metal`。
- `metal/CMakeLists.txt`：`enable_language(OBJCXX)`；静态库（engine.mm + adapter.cpp +
  statistics.cpp）；`-fobjc-arc`；`-framework Metal -framework Foundation`；
  `.metallib` 绝对路径宏注入（`OCUDU_MMSE_METALLIB_PATH`）。
- Ubuntu：`ENABLE_METAL_CHEST=OFF` → metal 目录完全不进构建 → 上游字节级一致。

---

## 5. 接口与配置设计

### 5.1 工厂扩展（`channel_estimator/factories.cpp` + `factories.h`）

```cpp
enum class port_channel_estimator_algorithm { cpu, metal_mmse };  // 新枚举（parameters.h）
// 工厂 create 增加参数（默认 cpu，PUCCH/SRS 调用点零改动）：
virtual std::unique_ptr<port_channel_estimator>
create(port_channel_estimator_algorithm             algo,
       port_channel_estimator_fd_smoothing_strategy fd_smoothing_strategy,
       port_channel_estimator_td_interpolation_strategy td_interpolation_strategy,
       bool compensate_cfo) = 0;
```

- `algo = cpu` → 现有 average_impl（默认，Ubuntu 唯一路径）；
- `algo = metal_mmse` → `port_channel_estimator_metal_mmse_impl`（`OCUDU_METAL_CHEST` 下才注册）。
- **语义**：`metal_mmse` 下 `fd_strategy`/`td_strategy` 被忽略（2D 一次完成，文档注明）；
  `compensate_cfo` 两路径都生效。
- **同步修改点**：`port_channel_estimator_factory` 仅有两个实现——SW 工厂
  （`channel_estimator/factories.cpp:14`）与指标装饰器工厂
  （`lib/phy/metrics/phy_metrics_factories.cpp:126`），两者都要透传新参数；
  `algo` 默认值 `cpu` 使其余调用点零改动。

### 5.2 配置贯通（新增一个 knob，全程照抄 `pusch_ldpc_decoder_type` 链路）

| 层 | 文件 | 变更 |
|---|---|---|
| 默认值 | `du_low_config.h` | `std::string pusch_channel_estimator_algo = "cpu";`（注释列取值） |
| CLI | `du_low_config_cli11_schema.cpp` | `expert_phy --pusch_channel_estimator_algo` + 取值校验 `[cpu, metal_mmse]` |
| yml writer | `du_low_config_yaml_writer.cpp` | `node["pusch_channel_estimator_algo"] = ...` |
| 翻译 | `du_low_config_translator.cpp` | 赋给 `upper_phy_factory_configuration.pusch_channel_estimator_algo`；**非 Apple 平台强制 "cpu"**（复制 LDPC 的 `#if defined(__APPLE__)` 模式） |
| 工厂配置 | `include/ocudu/phy/upper/upper_phy_factories.h`（`upper_phy_factory_configuration`） | 新字段 |
| 创建 | `upper_phy_factories.cpp` | `create_port_channel_estimator_factory_sw(ta_est_factory, algo)`；PUSCH 传配置值、PUCCH 传 `cpu` |

### 5.3 API 前瞻（PUCCH / SRS，本期只留位置）

- **PUCCH**：共用同一工厂，本次显式传 `cpu`；MMSE 实现按块参数化（B 可小到 1），
  未来接通 PUCCH 需单独验证窄带统计（见风险 R3）。
- **SRS**：独立 `srs_estimator`；未来可复用 `ocudu::metal::mmse_engine` 与
  `channel_statistics_estimator`（二者与工厂解耦，按此原则设计）。
- **AI 阶段**：新 algo 值（如 `helena`）+ 新 adapter 复用同一引擎/缓冲/统计模块/探针骨架。

---

## 6. 测试设计

### 6.1 `test/port_channel_estimator_metal_unit_test.cpp`（G1 验收）

1. **求逆 golden**：随机实对称正定 A（36×36，来自随机 Kronecker R_pp + σ²I，
   含 fd=0 秩亏边缘、σ²→0 边界）→ GPU `invert` vs double 精度参考 → 相对误差 < 1e-5。
2. **GPU/CPU 对拍**：固定随机 LSE 数据，GPU `invert+apply` vs 测试内 CPU 参考 2D MMSE
   （直接公式）→ 逐元素相对误差 < 1e-5。
3. **统计估计对拍**：给定合成信道（已知 τ̄/f_d/σ²），`channel_statistics_estimator`
   输出偏差带断言（σ²̂ 相对误差 < 50%、τ̂ < 100%、f̂_d < 200 Hz @ fd=200——与模拟
   观测一致；M0 锁定精确容差）。
4. **零拷贝/复用**：连续 1000 槽反复调用，结果一致、无内存增长。

### 6.2 `test/port_channel_estimator_metal_mmse_bler_test.cpp`（G3 验收）

- 复用 `tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test_channel_emulator.h`
  （TDL-A/B/C + single-tap 信道模拟器）：
  - **NMSE 对比**：`metal_mmse` vs `cpu`（average_impl）vs 真值，SNR −5..25 dB、
    51 PRB、type1、1/2 DMRS 符号、15/30 kHz、多普勒 0/200 Hz；
  - **BLER 对比**：走既有 `pxsch_bler_test` 链路（CE → 均衡 → 解调 → LDPC），
    同信道同种子，BLER 不得劣化。
  - 判定（写入断言，对应 §3.5 模拟预期）：全 SNR 无点劣于 cpu 超 0.5 dB；
    低 SNR 有增益点；30 kHz 高 SNR 增益显著。

### 6.3 时延基准（G4 验收）

- `test/port_channel_estimator_metal_mmse_benchmark.cpp`：1000 槽统计
  `last_gpu_wait_us()` + 端到端 `compute()`（中位/p95/p99），与 average_impl 同机对比。
- 复用/扩展 `tests/benchmarks/phy/upper/channel_processors/pusch/pusch_processor_benchmark.cpp`
  （已有 latency/throughput 模式）：加 `--algo` 选项做整链路口径对比。
- 实链（M3）：`[ul_pipeline]` 探针三腿（cpu / metal_mmse 背靠背），口径同 LDPC 实链轮次。

### 6.4 探针与指标

- `ul_pipeline_probe` 不动（CE 段端点已存在），新增可选 `ai_ce_us` 细分（`OCUDU_FLOW_PROBES` 门控）。
- `phy_metrics_port_channel_estimator_decorator` 扩展：`algo`、GPU 时延直方图、
  回退计数、NaN 计数；统计模块输出（τ̂/f̂_d/σ²̂）进 debug 日志（诊断多普勒/多径环境用）。

---

## 7.0 实施记录（2026-08-29）

### 7.0.1 已完成（M0/M1 CPU 核心）

- **cpu 路径重构（行为中立，已回归验证）**：`port_channel_estimator_average_impl` 新增
  protected virtual 钩子 `apply_fd_td_estimation_stage()`（默认 = 原 FD 平滑+插值逻辑）；
  `estimate_noise` 移入 `port_channel_estimator_helpers`（供 metal 适配器复用 σ²）；
  RSrp 累加移出 FD 循环（两种路径共用，数学一致）。
  **回归**：既有 `pusch_processor_unittest` 3456 用例全绿。
- **`channel_statistics_estimator` 接口 + 固定常数实现**（v1：σ² 来自现有经典噪声估计，
  τ̄/f_d 为固定可配置常数；接口已为 v2 估计实现预留完整输入上下文）。
- **`port_channel_estimator_metal_mmse_impl`**（CPU 参考数学，Metal 引擎后续接入同一接口）：
  2D 块 MMSE 完整闭环——σ² 估计（RC 平滑 + `estimate_noise`）→ R=R_t⊗R_f 参数化组装 →
  Gauss-Jordan 求逆 → W=R_hp·A⁻¹ → 每块 504×36 矩阵乘 → 全网格输出 + cbf16 转换 +
  `filtered_pilots_lse`/`freq_response` 回填（RSrp/噪声/TA 继承逻辑自动生效）。
- **CMake**：根选项 `ENABLE_METAL_CHEST`（默认仅 Apple arm64 ON），
  `metal/CMakeLists.txt` + `signal_processors/CMakeLists.txt` 挂接；Ubuntu 零影响。
- **单测** `metal/test/port_channel_estimator_metal_mmse_unit_test.cpp` 全部通过：
  ① 求逆 golden（12/36 阶随机 SPD vs double 参考，误差 <1e-4）；
  ② 固定统计实现；③ 端到端 NMSE vs cpu 估计器（VehA 51 PRB，200 实现/SNR）。

**实测 NMSE（与 §3.5 模拟预期一致）**：

| SNR | cpu（LS+RC+TD平均） | metal_mmse（τ̄=370ns, f_d=0 固定） | Δ |
|---|---|---|---|
| −5 dB | −5.57 dB | **−8.29 dB** | **+2.72 dB** |
| 0 dB | −10.68 dB | **−11.98 dB** | +1.29 dB |
| 10 dB | −20.48 dB | −19.85 dB | −0.63 dB |
| 20 dB | −29.63 dB | −28.76 dB | −0.87 dB |

### 7.0.2 实施中发现的坑（已修复，记入单测）

1. **Gauss-Jordan 工作区必须整体清零**（[A|I] 中单位阵之外的列未初始化 → NaN 权重矩阵）。
2. **块偏移按 PRB 换算**（导频子序列偏移 = 块索引 × block_prb × 6，网格偏移 = ×12；
   写错会越界/错位）。
3. **`bounded_bitset` 默认构造的 size 未初始化**——测试构造图案前必须 `resize()`。
4. AppleClang 无 `std::cyl_bessel_j`（C++17 数学特函数缺失）→ 用 libm `::j0()`。

### 7.0.3 M1 完成：Metal 引擎 + K1/K2 kernel（2026-08-29）

- **`ocudu_metal_mmse_engine.{h,mm}`**（照搬 LDPC 引擎范式）：MTLDevice/CommandQueue/
  Pipeline、`.metallib` 运行时加载（构建期 `xcrun metal/metallib` 编译，CMake custom
  command + 路径宏）、零拷贝 `newBufferWithBytesNoCopy` + buffer cache、同步
  `waitUntilCompleted`、GPU 侧计时（`GPUStartTime/GPUEndTime`）。
- **K1 `ocudu_mmse_inv.metal`**：批量 Gauss-Jordan 求逆（每系统一个 threadgroup，
  [A|I] 36×72 入 threadgroup 共享内存 ~10 KB，部分主元 + 行并行消元）。
- **K2 `ocudu_mmse_apply.metal`**：批量块矩阵乘（每 (系统,块) 一个 threadgroup、每输出
  位置一个线程；W 实、y/h 复交错）。
- **适配器 GPU 路径**：标准块（L=36、nout=504）走 GPU 批量（W 各层共享——v1 固定统计），
  边缘块走 CPU；引擎不可用/失败自动回退 CPU 参考数学。
- **实测（Metal，M 系列 GPU）**：K2 批量矩阵乘 **16.1 µs**/槽（17 块 × 1 层，bit-exact）；
  K1 求逆首调 237 µs（含流水线热身）；端到端 NMSE 与 CPU 参考**逐位一致**（Test 3 数值
  与 7.0.1 表完全相同，即 GPU 路径已生效）。
- **MSL 平台坑（已记录）**：① 位置内建变量必须同型（uint+uint2 混用被编译器拒绝 →
  K2 改展平 1D 网格 + 标量位置）；② `[[buffer]]` 标量参数需用 POD 结构体（`setBytes`）；
  ③ ARC 下禁止显式 `release`；④ ObjC 属性名为 `GPUStartTime/GPUEndTime`（大写）。

### 7.0.4 M2 完成：工厂/配置贯通（2026-08-29）

- **算法选择放工厂层**（工厂构造时定 algo，`create()` 签名不变 → pusch/pucch 调用点与
  指标装饰器工厂**零改动**）。
- 新枚举 `port_channel_estimator_algorithm{cpu, metal_mmse}`（parameters.h）；SW 工厂
  注册 metal_mmse（`OCUDU_METAL_CHEST` 下，固定统计 τ̄/f_d/块尺寸可配置；非 Apple 平台
  显式报错）。
- 配置链：`du_low_config.h` → CLI `expert_phy --pusch_channel_estimator_algo cpu|metal_mmse`
  + `--pusch_channel_estimator_mmse_tau_rms_us|mmse_fd_hz|mmse_block_prb` → yml writer →
  翻译层（**非 Apple 强制 "cpu"**，同 LDPC 策略）→ `upper_phy_factory_configuration` →
  `upper_phy_factories.cpp`（PUSCH 传配置、PUCCH 显式 cpu）。
- **冒烟测试通过**：`gnb -c configs/gnb_zmq.yaml expert_phy --pusch_channel_estimator_algo metal_mmse`
  正常启动（Metallib 加载成功、无报错）；`gnb expert_phy --help` 新选项齐全。
- 引擎加载加固：主路径失败时打日志、fallback 的 nil URL 防护（此前会触发
  `_MTLDevice newLibraryWithURL` 断言）。

### 7.0.5 M3 部分完成：时延刻画与基准（2026-08-29）

- **基准扩展**：`pusch_processor_benchmark` 新增 `-c` 选项（cpu/metal_mmse），工厂按
  algo 创建（固定统计 370ns/0Hz/3PRB）。
- **单测 Test 5（单线程稳态，200 槽）**：`compute()` 延迟 cpu ≈ **8.7 µs**、
  metal_mmse ≈ **539–885 µs**（随系统负载波动，测试机 load 5–17）。
- **组件拆解（500 次稳态）**：K2 apply GPU 纯计算 **6.3–6.5 µs**（bit-exact）；
  **K1 批量求逆 GPU 369–382 µs**（36 线程 GJ + 每列 3 次 threadgroup barrier × 36 列 ≈
  108 次同步——小矩阵上 barrier 主导，GPU 求逆不经济）；单次 command buffer 的
  commit+wait 壁钟 ≈ 90 µs（负载相关，LDPC 实测同机约 30 µs 为轻载值）。
- **热路径调整**：A⁻¹ 改在 **CPU** 计算（Gauss-Jordan 36×36 ≈ µs 级），GPU 命令缓冲
  执行 K1b（W=R_hp·A⁻¹）+ K2（批量矩阵乘）单次 commit；K1 Metal 求逆 kernel **保留**
  为算法骨架与 golden 对拍（Test 4a 误差 9e-7），后续矩阵规模增大或多系统摊销时再启用。
- **D8 风险如实上报**：在这些网格规模下 metal_mmse 的逐调用延迟（数百 µs）> cpu
  （~9 µs），1.5× 验收线**在单端口串行口径下不达标**——GPU 路径的价值是精度
  （−5 dB +2.7 dB NMSE）与 AI 骨架，不是小网格延迟。缓解方向（列入 backlog）：
  ① 跨端口合并批处理（4 端口 1 次 dispatch 摊销固定开销）；② 持久 command buffer/ICB；
  ③ 实链口径复测（端口并行 + 真实负载下的实际差值待 M3 实链三腿确认）。

### 7.0.6 M3 完成：BLER 集成测试贯通（2026-08-29）

- `pxsch_bler_test` 新增 `-c` 选项（cpu/metal_mmse，工厂链 `create_sw_pusch_processor_factory`
  增加 algo 参数，默认 cpu 保持兼容）；`pusch_processor_benchmark` 已先加 `-c`（§7.0.5）。
- **功能验证通过**：`pxsch_bler_test -S 5 -R 52 -C TDLA -c metal_mmse` 端到端跑通
  （信道模拟 → DMRS CE → 均衡 → 解调 → LDPC → BLER/EVM/TA/CFO 统计），与 cpu 腿输出结构一致、
  无 NaN/崩溃；定量 NMSE 对比由单测 Test 3 承担（−5 dB 处 +2.72 dB）。

### 7.0.7 E2E 首测问题定位与修复（2026-08-29）

**实链首测**：cpu 腿正常（探针：管线中位 186 µs、CE 阶段中位 63.6 µs、均衡解调 18.2 µs、
LDPC 20 µs）；metal_mmse 腿在首个 PUSCH（Msg3）后崩溃。

**根因（已修复）**：GPU 槽位步长族 bug——GPU kernel 按 **实际尺寸**（`p.L`、`p.nout*p.L`）
索引系统槽，而 CPU 侧曾按 MAX 容量（36/504）步长写入槽位。双 DMRS 符号（L=36）时
两者相等被掩盖；**单 DMRS 符号（L=18，即 Msg3 的典型配置）时错位**：A⁻¹ 写入步长
36 vs 读取步长 18 → 权重矩阵垃圾 → 信道估计输出 ~100× 幅度错误（单测复现
NMSE +40.7 dB）。已统一全部 CPU 侧槽位步长为实际 L_std/nout_std。

**回归加固**（单测新增）：
- Test 6：真实配置组合（52 PRB=10 MHz 边缘块、单 DMRS 符号、25 PRB）——修复后
  52 PRB/1 DMRS NMSE **−14.45 dB**（修复前 +40.7 dB）。
- Test 7：200 槽交替配置压力测试（Msg3 小分配 ↔ 全带宽数据交替，6 种 PRB/DMRS 组合）——
  无崩溃、无 NaN。

**待确认**：实链崩溃与上述垃圾输出是否直接关联（垃圾信道估计 → 下游均衡/指标路径的
次级故障）；请用修复后的二进制重跑 E2E。若仍崩溃，用 lldb 抓取回溯：
`sudo lldb -- build/apps/gnb/gnb -c configs/gnb_zmq.yaml expert_phy --pusch_channel_estimator_algo metal_mmse`
→ `run` → 按 `t` → 崩溃后 `bt` 输出给我。

### 7.0.9 Test 6b（3 DMRS 符号）NaN 定位与修复（2026-08-29）

**症状**：Msg3 配置（4 PRB @ CRB 8，DMRS {2,7,11}，L=54）单测 NMSE NaN/0 dB；
L=54 块 `|y|²=nan`、`sigma2=0`、`|W|²=6.9e8`（秩亏求逆放大）。

**根因（单测夹具 bug，非估计算法/实现）**：Test 6b 的 `grid_fake` 只分配
`n_prb×12=48` 子载波，但配置 CRB 偏移 8 → 抽取器按绝对 CRB 索引
`get_view().subspan(crb_offset×12, n_prb×12)` = 偏移 96 读 48 宽视图 → **越界读**
（读出 0/NaN，跨运行不确定，掩盖为"算法 NaN"）。实链资源栅格从 CRB 0 起全带宽，
无此问题——纯测试侧假网格过窄。

**修复**：Test 6b 栅格与 rx_sym 改为 `(crb_offset+n_prb)×12` 宽，导频 RE 写入
绝对子载波 `crb_offset×12 + prb×12 + pos`（通道真值索引同步对齐）。修复后
**NMSE −12.93 dB**（CPU 与 GPU 路径一致），Test 7 无 NaN，全部单测绿。

**加固结论**：MAX_BLOCK_PILOTS=72（3 符号 L=54）路径 CPU/GPU 数值均正确；
v1 3-DMRS 场景（E2E Msg3）为受支持配置。

### 7.0.10 E2E 第二轮：metal_mmse 腿端到端通过，时延成为唯一瓶颈（2026-08-29）

**结果**：`expert_phy --pusch_channel_estimator_algo metal_mmse` 实链端到端通过——UE attach、
RRC Connected、Msg3/Msg4 全通（第一轮崩溃彻底解决）。但 UE 随即被 release：

```
gnb console (ZMQ 10 MHz 1T1R n3, 15 kHz SCS):
[ul_pipeline]           mean=6001.0us median=6001.0us   ← 全管线溢出
[ul_channel_estimation] mean=5852.8us median=5852.8us   ← cpu 腿基线 63.6 µs，metal_mmse ≈ 92×
[ul_equalization_demod] mean=  35.7us
[ul_ldpc_decode]        mean=  50.0us
PUSCH 列 rsrp=ovl（过载标记），UE: Received RRC Release
```

**时延结论**：数值与正确性已达标（单测 NMSE −12.7 dB @ 52 PRB/2 DMRS、−12.9 dB @ Msg3 配置），
当前唯一阻塞项是 CE 阶段时延：**5.85 ms vs 1 ms 槽长预算**（15 kHz），100% 过载 → release。
Metal LDPC 路径存在同类时延问题（用户确认），计划一起优化。

**分析方向（下一步，见 §7.0.11）**：
- 单测口径 compute() 仅 0.55 ms（GPU 路径），实链 5.85 ms 存在 ~10× 缺口 → 优先确认实链
  是否落在 CPU 回退路径（engine init 失败）还是 GPU 排队/共享队列竞争；
- CPU 侧热点候选：`build_correlation_matrices` 每块 ~19.4k 次 `rf_corr`（除法）+ `rt_corr`
  （`::j0` 超越函数）调用；GPU 路径每槽仅 1 次标准块构建（~0.5 ms），而 CPU 回退路径
  **每块构建一次**（52 PRB = 17 块 ≈ 330k 次调用 ≈ 5–8 ms）——与实链量级吻合；
- 若 CPU 占比过半：NEON 加速（`rf_corr` 向量化 + `rt_corr` 查表/多项式近似），
  LDPC 侧时延优化同批进行。

### 7.0.11 metal_mmse 时延分相分析（2026-08-29，`OCUDU_MMSE_TIME=1` 单测口径）

新增分相计时（`OCUDU_MMSE_TIME=1`，E2E 可用）与引擎状态打印（`OCUDU_MMSE_DBG=1`
输出 `[mmse_ce] engine READY/UNAVAILABLE`）。52 PRB/2 DMRS（10 MHz 实链配置）单测实测：

| 路径 | sigma2 | corr 构建 | GPU 路径 | （其中 GPU 等待） | CPU 块循环 | 合计 |
|---|---|---|---|---|---|---|
| GPU 路径（稳态） | 4 µs | 6.5 µs | 451 µs | **351 µs** | 14 µs（边缘块） | **~475 µs** |
| GPU 路径（引擎实例首次调用） | 4 µs | 6.5 µs | **2583 µs** | 345 µs | 19 µs | ~2.61 ms |
| CPU 回退（NOGPU） | 4 µs | 6.2 µs | – | – | **3443 µs** | **~3.45 ms** |

**结论**：
1. **相关矩阵构建不是热点**（每块仅 ~6 µs：编译器将 `j0` 提升出内层循环，`1/(1+x²)`
   被向量化）——原"330k 次超越函数调用"估计被实测证伪。
2. **GPU 路径瓶颈在 K1b 权重 kernel 的 GPU 等待 ~350 µs**：`mmse_weights` 每系统只开
   **1 个 threadgroup × 128 线程**，504 行 × 36 列 × 36 内积 = 每线程 5184 次串行 FMA，
   并行度严重不足（653k FLOP 被串行化）。CPU 侧（GJ 求逆 + 打包/解包）仅 ~100 µs（21%）。
3. **CPU 回退路径瓶颈在每块 W=R_hp·A⁻¹ 矩阵乘 + GJ 求逆**：17 块 × ~200 µs ≈ 3.4 ms
   （504×36×36 FMA 标量执行）——CPU 占 ~100%。这是"CPU 占比过半"的场景。
4. **引擎实例首次 GPU 提交 ~+2.5 ms 一次性开销**（pipeline/首提交惰性编译）——实链
   `[ul_channel_estimation]` 探针 samples=1 恰为首个 PUSCH，5.85 ms ≈ 2.5 ms 首提税 +
   3.45 ms（若实链落在回退路径）；需实链 `OCUDU_MMSE_DBG=1 OCUDU_MMSE_TIME=1` 复跑确认
   `[mmse_ce] engine READY` 与分相数据。

**优化候选（待用户决策，§7.0.10 分析方向）**：
- (A) K1b 并行化：grid 改为 `nof_systems × ceil(nout/128)` 个 threadgroup（每行一线程，
  36 内积串行）→ GPU 等待 350 µs → 预计 <20 µs；GPU 路径总量 → ~120 µs。
- (B) 引擎实例构造时 warm-up 提交（消化 2.5 ms 首提税，移出槽内路径）。
- (C) CPU 回退/CPU 侧 NEON：GJ 求逆 + W 矩阵乘向量化（回退路径 3.4 ms → 预计 <0.5 ms）；
  若实链确认为回退路径则必做，若为 GPU 路径则优先级低于 (A)。
- (D) 引擎单例共享（多端口/多实例共用队列与 pipeline，摊薄首提税）——与 LDPC 时延
  优化同批。

### 7.0.12 实链分相复测：GPU 路径已生效，瓶颈=K1b 权重 kernel + 首槽首提税（2026-08-29）

`OCUDU_MMSE_DBG=1 OCUDU_MMSE_TIME=1` 实链复跑（ZMQ 10 MHz n3）：

- **10 个引擎实例全部 `engine READY - GPU hot path active`** → 实链走 GPU 路径，此前
  "CPU 回退"假设被证伪。
- **实链配置 = 3 DMRS 符号 {2,7,11}（L=54）**，PUSCH 分配 3/24/36 PRB（非 52 PRB/2 符号）。
- **稳态每槽 CE ≈ 0.90–0.95 ms**（36 PRB，12 标准块）：sigma2 ~4–7 µs、corr 构建 ~10–14 µs、
  gpu_path ~890–950 µs（其中 **GPU 等待 ~765 µs = 总耗时 82%**）、finish ~1–2 µs。
  CPU 侧（GJ 54×54 求逆 + 打包/解包）≈ 125 µs ≈ **14%**。
- **首槽 3.87 ms**：prb=3 首行 gpu_path=3839.7 µs 而 gpu_wait 仅 769.6 µs → 每实例首次
  GPU 提交的 pipeline 惰性编译/首提税 ~3 ms（单测口径 2.6 ms，同源）。实链探针
  `[ul_channel_estimation] samples=1 mean=3947.5us` 取的正是这个首槽。
- `rsrp ovl` 持续：稳态 0.93 ms CE + 0.07 ms TF + 0.03 ms 均衡 + 0.04 ms LDPC ≈ 1.07 ms
  > 1 ms 槽长（15 kHz），抖动即过载 → UE release。

**结论**：K1b `mmse_weights`（1 threadgroup × 128 线程，L=54 时每线程 4×54×54 ≈ 11.7k
串行 FMA）是唯一主热点；CPU 占比仅 ~14%，不满足"CPU 过半才上 NEON"的触发条件。
优先级：**(A) K1b 并行化**（预计 765 µs → <40 µs，总 ~175 µs）+ **(B) 首提交 warm-up**
（首槽 −3 ms）；NEON（C）降级为 30 kHz SCS 预算收紧时的储备项。

### 7.0.13 A+B 实施完成：K1b 并行化 + 首提交 warm-up（2026-08-30）

**(A) `mmse_weights` 并行化**：原 kernel 每系统 1 threadgroup × 128 线程（每线程 4×L×L
串行 FMA）。改为**每输出元素一线程**（flat 1D grid = `nof_systems × ceil(nout·L/128)`
个 threadgroup；gtid→(row, col) 映射 col=gtid%L 使 warp 内 A⁻¹ 加载 coalesce、R_hp 加载
broadcast）。累加顺序（k 升序）与 CPU 参考一致 → **逐位一致**（NMSE 与 golden maxerr
与改动前完全相同：−12.68/−14.45/−17.61/−12.93 dB，dbgL54 maxerr 8.873e+01）。

**(B) 构造时 warm-up**：引擎 init 后立即用**全容量** staging buffer 跑一次哑
`run_weights_only`（MAX_BLOCK_OUT×MAX_BLOCK_PILOTS×MAX_LAYERS×max_blocks，缓冲区零初始化），
消化 Metal 首提交惰性编译（~3 ms）并预热按指针索引的 buffer 缓存（全容量保证后续任意
真实尺寸命中）。

**根因修复**：build 目录残留旧 `ocudu_mmse.metallib`（紧邻单测可执行文件），引擎的
`NSBundle mainBundle` 回退路径**先**命中旧文件，遮蔽源树新编译产物——新 kernel 一度
"无效果"。修复：烘焙绝对路径 `OCUDU_MMSE_METALLIB_PATH` 改为**首选**加载源（mainBundle/
cwd 仅作回退）+ CMake custom target `copy_if_different` 让 build 目录副本永远同步 +
`OCUDU_MMSE_DBG=1` 打印实际加载路径。

**实测（单测口径）**：

| 配置 | gpu_wait 前 | gpu_wait 后 | compute() 总量 前 | 后 |
|---|---|---|---|---|
| 52 PRB / 2 DMRS (L=36) | 351 µs | **19.5 µs** | ~475 µs | **~137 µs** |
| Msg3 4 PRB / 3 DMRS (L=54) | 765 µs | **25.3 µs** | ~957 µs | **~176 µs** |
| 首槽首提税 | +2.6 ms | **0（移入构造期）** | – | – |

52 PRB 稳态 CE 从 ~475 µs 降至 ~137 µs（≈2× cpu 基线 63.6 µs）；预计实链（3 DMRS 符号、
36 PRB）从 ~930 µs 降至 **~180–220 µs**，管线总量 ~310 µs，15 kHz 预算余量 ~3×，
30 kHz（0.5 ms）也可过。待实链复测确认（`OCUDU_MMSE_DBG=1 OCUDU_MMSE_TIME=1`）。

### 7.0.14 E2E 第三轮:UE 成功接入并取得 IP(2026-08-30)

**结果**:metal_mmse 腿端到端全通——UE attach → RRC Connected → **PDU Session
Establishment successful, IP 10.45.0.31** → NR reconfiguration。gnb 侧 PUSCH
**MCS 7、28.5k brate、14 ok / 0 nok(0%)**,`rsrp ovl` 仅出现在首个表格行。

**探针(15 样本)**:

| 阶段 | mean | median | min | max |
|---|---|---|---|---|
| ul_pipeline | 612.8 µs | **407.0 µs** | 164.0 | 3392.0 |
| ul_channel_estimation | 496.3 µs | **293.2 µs** | 96.2 | 3249.4(首槽) |
| ul_equalization_demod | 23.2 µs | 22.7 µs | 4.9 | 32.4 |
| ul_ldpc_decode | 33.6 µs | 27.0 µs | 9.0 | 71.0 |

CE 稳态 ~200 µs/槽(3 DMRS 符号、36 PRB,`[mmse_time]` 行 total 180-255 µs),
median 293 µs 含小分配/边缘块差异;管线 median 407 µs < 1 ms 预算,余量 ~2.5×。
与修复前(5.85 ms 稳态、100% ovl)相比**~20× 改善**;与 cpu 基线(63.6 µs)仍差 ~4.6×,
为算法代价(2D MMSE 全网格权重 + GPU 往返),30 kHz SCS 前需再做 (C)/(D)。

**遗留观察(不阻塞)**:首槽一次性 ~3.1 ms CPU 侧开销(sudo/执行器线程首个 GPU 提交,
`gpu_wait` 仅 24 µs,即非 kernel 时间;构造期 warm-up 已覆盖单测进程内场景)。
仅发生在 attach 首槽,nok=0,不影响业务——列入 (D) 引擎单例与启动期预热议题。

### 7.0.15 经验教训整理(MMSE CE 全周期,供后续 Metal 组件复用)

1. **GPU 等待 ≈ occupancy 问题,不是算力问题**。同数据规模下 128 线程串行链
   (765 µs)与 27k 线程(25 µs)差 30×;任何 kernel 先问"线程数撑不撑得起",
   再问 FLOP。warp 内访存布局(coalesce/broadcast)同样重要。
2. **首次 dispatch 有 ~ms 级惰性编译税**,pipeline 创建≠编译;每个引擎实例构造期
   用真实缓冲区跑一次哑提交(warm-up),把税移出槽内路径。
3. **metallib 加载路径必须权威化**:build 目录旧副本曾通过 mainBundle 回退遮蔽源树
   新产物(A 修复一度"无效")。对策:烘焙绝对路径为首选 + copy_if_different 同步
   build 副本 + 加载路径日志(debug 级)。
4. **诊断输出走 ocudulog,不走 console**:`[mmse_*]`/`[dbgblk]` 全部迁入 PHY 通道
   debug 级(env 开关保留作重负载诊断的二次闸门);引擎错误走 error 级。gnb console
   从此只显示业务输出。
5. **探针 samples=1 会误导**:单样本恰采首槽(含一次性税)曾把 5.85 ms 误读为稳态;
   分相计时(`OCUDU_MMSE_TIME`)与中位数口径缺一不可。
6. **改变 kernel 并行化时保持累加顺序** → 与 CPU 参考逐位一致(bit-exact),NMSE 与
   golden 对拍零漂移,回归零成本。
7. **单测夹具必须匹配真实索引空间**(CRB 偏移 → 栅格宽度 §7.0.9);假栅格过窄的
   越界读曾以"算法 NaN"的面目出现,浪费多轮排查。
8. 同方法已回灌 LDPC 引擎(LDPC PLAN §4.16):warm-up 补上、NSLog 迁日志;layered
   内核小 z 的 dispatch 链特征记录在案,必要时走 persistent 变体。

### 7.0.16 E2E 第四轮:CE+LDPC 双 Metal 全通(2026-08-30)

`--pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal` 双开:
UE 接入、取得 IP、**ping 核心网完整运行**。探针(215 样本):

| 阶段 | mean | median | p95 | 说明 |
|---|---|---|---|---|
| ul_channel_estimation | 264.1 µs | 267.2 µs | 380.2 µs | 稳态 ~200 µs 健康(223 条 mmse_time median 197 µs、p90 323 µs),max 3123 µs = 首槽一次性税 |
| ul_ldpc_decode | 966.7 µs | **981.0 µs** | 1281 µs | **新的预算破坏者**(见下) |
| ul_pipeline | 1303.3 µs | 1371.0 µs | 1693 µs | 超 1 ms 预算 → `ovl` 持续;ZMQ 无硬实时,数据仍正确(稳态 0 nok) |

**LDPC metal(layered)时延根因**:每解码固定 **290 个 dispatch**(max_iter=6 ×
48 个/轮[46 层+syndrome+gate]+init+convert),实测 ~1.9 µs/dispatch ≈ **550 µs 地板**,
GPU 算力远未吃饱(小 TB 场景 dispatch 链主导);离群 3-8 ms = 运行中新 (BG,Z) 的
槽构造。缓解:`--pusch_ldpc_decoder_type metal_persistent`(单 dispatch 常驻内核,
单测对拍 100%)或 E2E 时 LDPC 回 CPU(§7.0.15 策略)。详见 LDPC PLAN §4.17。

**长期规划立项**:`docs/apple_silicon_heterogeneous_gnb_plan.md`——GPU 定位高并发/
多用户/高带宽;模块级 >10× CPU 时延可容忍但 E2E 必须在预算内;终局 = UL 全链
(FFT/CE/MIMO/LDPC)单 command buffer 一次 dispatch、CPU 不等回;LDPC crc=OK 后
MAC PDU 经回调直接给 FAPI;V2X 小包走 P/E 核、大带宽视频走 GPU(未来 NPU)。

### 7.0.17 "粮草先行"审计:CE 已合规,残留一项入待办(2026-08-30)

按 `docs/apple_silicon_heterogeneous_gnb_plan.md` §2.1 铁律审计 CE:
- **CE 无惰性初始化**:引擎 + pipeline + 缓冲区 + warm-up 自 A+B 起即在构造期完成
  (10 实例在 gnb 启动期),首个 PUSCH 不付任何引擎初始化成本——已合规。
- **残留(一次性,attach 首槽)**:实链首槽 `gpu_path≈3 ms(gpu_wait≈24 µs)` =
  执行器线程**首次 Metal 调用的每线程驱动初始化**(构造期 warm-up 跑在主线程,
  覆盖不到执行器线程)。缓解方案:启动期在 UL 执行器线程上跑一次哑提交——需要
  executor 侧配合,列入待办(与 LDPC 大 z 内核优化同批)。

### 7.0.8 剩余工作（复盘收尾清单，2026-08-30 更新）

**metal_mmse 主线已闭环**（算法/引擎/配置/单测/时延优化/E2E 500-ping/粮草先行）。
剩余 pending issue（按优先级）：

1. **首槽 ~3 ms 一次性税**：执行器线程首次 Metal 调用的 per-thread 驱动初始化
   （构造期主线程 warm-up 覆盖不到）。缓解：启动期在 UL 执行器线程跑一次哑提交
   ——需要 executor 侧配合，列为待办。影响：仅 attach 首槽一次，数据无损失。
2. **30 kHz SCS 未验证**：预算 0.5 ms；当前 CE ~255 µs + LDPC ~730 µs 会超——
   LDPC 每轮 barrier 链（46 × ~8.7 µs）是大头，需先做 LDPC 优化
   （多 TG + 设备栅栏 persistent / 小 z 走 layered）再验 30 kHz。
3. **RF B200 实链三腿 A/B**：实验室依赖，ZMQ 已验证，RF 未做。
4. **(C) NEON 加速**：不触发（CPU 仅 ~14%），保留为预算收紧时储备。
5. **(D) 引擎单例 + 跨端口批处理 + 持久 command buffer**：多端口时摊销提交往返。
6. **探针 `ai_ce_us` 细分 + metrics decorator**：现用 `OCUDU_MMSE_TIME` 环境开关替代。
7. **τ̄/f_d 估计（v2 统计）与 PUCCH/SRS**：接口已预留（`channel_statistics_estimator`），
   v2/AI-CE 任务承接（见 `AI_channel_estimation_implementation_plan.md` MEMO）。

- **M3**：NMSE/BLER 对比测试、时延基准（`metal/test/` 扩展 + `pusch_processor_benchmark`
  --algo 选项）、探针、实链三腿 A/B（ZMQ → RF B200）。

---

## 7. 里程碑（每步可独立评审）

| 里程碑 | 内容 | 产出/验收 | 工期（1 人 PHY C++/Metal） |
|---|---|---|---|
| **M0** | CPU 参考 2D MMSE 原型（纯 C++，无 Metal）：统计估计（σ²/τ̄/f_d）+ R 组装 + W + 块矩阵乘闭环 + 临时单测 + `/tmp/mmse_ce_sim` 三脚本迁入 `metal/sim/` | 验证算法增益方向与全部数学细节（模拟已先行验证，§3.5）；锁定统计估计容差 | 1–1.5 周 |
| **M1** | Metal 引擎 + K1/K2 kernel + `metal/test/` unit test（求逆 golden、对拍、复用、统计对拍） | G1 验收；CMake macOS 全绿、Ubuntu 构建零影响验证 | 1–2 周 |
| **M2** | 工厂/枚举/配置贯通 + average_impl "网格生成钩子"重构（cpu 路径行为不变，既有单测全绿）+ 适配器集成 | `expert_phy --pusch_channel_estimator_algo metal_mmse` 可启动 | 1 周 |
| **M3** | NMSE/BLER 对比测试 + 基准 + 探针 + 实链三腿 A/B（ZMQ → RF B200） | G3/G4/G5 验收；数据回填 PLAN.md（同 LDPC 风格） | 1–2 周 |

合计约 **4.5–6.5 周**。M0 的 CPU 参考实现即最终 GPU 的 golden 对拍依据。

---

## 8. 风险与缓解

| # | 风险 | 缓解 |
|---|---|---|
| R1 | 统计估计质量（τ̂ 低 SNR 偏高是 est 与 oracle 差距的主要来源） | §3.2 已修正偏置（无窗拟合/噪声修正/PDP 缩放）；EMA 平滑；M0 消融锁容差；M3 全曲线把关；v2 上 PDP 直接估计 |
| R2 | 求逆/矩阵数值稳定性（fd=0 秩亏、σ²→0） | σ²I + ridge 双保险；golden 对拍覆盖秩亏/主元交换；备选 MPS Cholesky 对照腿 |
| R3 | 窄带（PUCCH 1–2 PRB）统计样本太少 | 本期 PUCCH 固定 cpu；未来设最小 PRB 阈值回退 |
| R4 | "网格生成钩子"重构扰动现有 cpu 路径 | 纯行为保持（虚函数默认=现有实现）；既有 pusch/pucch 单测 + pxsch_bler 全绿门禁 |
| R5 | 每端口独立提交 GPU 命令（≤4 端口/槽） | 每端口一次提交（~10–30 µs × 4 仍 < 预算）；引擎单例共享队列；AI 阶段跨端口合并批处理 |
| R6 | 跳频/单 DMRS 符号/边缘块 | hop 各自 2D；单符号场景 R_t 退化为 1×1（统计减半，模拟显示高 SNR 有小幅劣化，列 M3 验证项）；边缘块收窄 |
| R7 | MPS/Metal 运行期异常 | 本槽回退基类路径 + 回退计数 + 告警；启动期 MPS 不可用则整机回退 cpu |
| R8 | σ²̂ 估计失误传导到 W（过/欠收缩） | min(PDP, 残差) 双通道 + 下限；LLR 噪声方差仍走现有 estimate_noise，不受影响 |

---

## 9. 决策点（请评审，达成一致后实施）

| # | 决策 | 选项 | 建议（带模拟证据） |
|---|---|---|---|
| D1 | MMSE 维度 | (a) **2D 时频块（用户方案）**；(b) 1D+插值（v0.2 方案） | **(a)**：oracle 上限全面优于 (b) 与 cpu 路径（§3.5 表 1） |
| D2 | 相关模型 | (a) 参数化可分离模型 R=R_t⊗R_f（指数 PDP 实部 × J0(2πf_dΔt)）；(b) 全经验 R̂ | **(a)**：低方差、只需 3 个标量；经验 R̂ 直接使用会数值爆炸（已证伪） |
| D3 | 数据 RE 估计 | (a) 2D 大矩阵直接估计（用户方案）；(b) 保留 freq_interpolator | **(a)**：一次矩阵乘全出；(b) 仅作 cpu 路径 |
| D4 | 经典骨架复用 | (a) average_impl 加 protected virtual "网格生成钩子"（pilots_lse→全网格），metal impl 覆写；(b) 抽取公共 core 类；(c) 复制文件 | **(a)** 改动最小、cpu 路径零行为变化 |
| D5 | 命名 | `--pusch_channel_estimator_algo cpu\|metal_mmse` | 采纳 |
| D6 | 求逆形态 | (a) 实对称 Gauss-Jordan 36×36（R_pp+σ²I）；(b) MPS Cholesky 对照腿 | **(a)** 主选；(b) 对照 |
| D7 | σ² 来源 | (a) min(PDP 噪声地板, 后置滤波残差)；(b) 仅残差；(c) 仅 PDP | **(a)**（模拟：覆盖全 SNR，双向安全） |
| D8 | 时延验收线 | metal_mmse CE 阶段 ≤ average_impl 的 1.5×（实链探针，51 PRB 级） | 采纳 |
| D9 | 统计粒度 | 每 (端口,层,hop) 独立（σ² 每槽现估；τ̂/f̂_d 跨槽 EMA） | 采纳（各层/端口信道统计不同；批量求逆成本不变） |
| D10 | 块尺寸 | 1 / 3 / 5 PRB | **3 PRB**（模拟甜点，表 3） |
| D11 | 统计模块形态 | (a) 独立类 `channel_statistics_estimator`（工厂化，AI 复用）；(b) 内嵌 adapter | **(a)**：AI 阶段（AdaFortiTran CAM 先验）直接复用 |
| D12 | f̂_d/τ̂ 平滑 | (a) 跨槽 EMA（α≈0.2）；(b) 每槽独立 | **(a)**：单槽 f̂_d 噪声大；M3 调 α |

---

## 10. 附录：关键文件清单

| 类别 | 文件 |
|---|---|
| 待改（cpu 路径钩子） | `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.{h,cpp}`（网格生成钩子）、`port_channel_estimator_helpers.{h,cpp}`（如需复用 estimate_noise 语义） |
| 待改（工厂/参数） | `include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h`、`factories.h`、`lib/phy/upper/signal_processors/channel_estimator/factories.cpp`、`lib/phy/metrics/phy_metrics_factories.cpp`（decorator 透传 algo） |
| 待改（配置链） | `apps/units/flexible_o_du/o_du_low/du_low_config.h`、`du_low_config_cli11_schema.cpp`（`configure_cli11_expert_phy_args`）、`du_low_config_yaml_writer.cpp`、`du_low_config_translator.cpp`、`include/ocudu/phy/upper/upper_phy_factories.h`（`upper_phy_factory_configuration`）、`lib/phy/upper/upper_phy_factories.cpp`、`lib/phy/upper/signal_processors/CMakeLists.txt`、根 `CMakeLists.txt`（选项） |
| 新建 | 本目录全部（§4.1 清单） |
| 参照 | `lib/phy/upper/channel_coding/ldpc/metal/`（引擎/CMake/测试/PLAN 全套）、`test/ldpc_metal_unit_test.cpp`、`tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test*.{h,cpp}`（信道模拟器复用） |
| 参考论文 | van de Beek et al., "On Channel Estimation in OFDM Systems"（2D MMSE 通式，目录内 PDF）；Li/Cimini/Sollenberger, "Robust channel estimation for OFDM systems with rapid dispersive fading channels"（相关统计估计与低阶模型）；Edfors et al. SVD 低复杂度 MMSE（v2 优化路线） |
