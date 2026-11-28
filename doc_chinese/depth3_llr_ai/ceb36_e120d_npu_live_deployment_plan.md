# 把 `CeB36_e120d`（尺子 #1）部署到空中实链的 `npu` 后端 —— 方案与步骤（2026-11-28 14:40）

> **一句话**：这个模型的**离线能力已合格**（尺子 5 000 = 11 983 424，+3.61%），但**实链今天还不能飞**，原因**不是**模型精度，而是两件工程事实：
> 1. ★ **全带（612 子载波）一次前向 = 2 868 µs（本机实测）** ⇒ 折成管线 ≈ 4–6 ms，**正是 2026-11-05 那条丢 39% UL 块的腿所处的区间** ⇒ **必须走窄带桶**；
> 2. ★★★ **窄带桶的导出链路对 `--se` / `--dense-skip-entry` 是"静默错"的**：今天实测 `--export-buckets 108` **返回 0、不报错**，却把 **36 个 `dproj*_1x1` + 72 个 `se*_fc*` 权重全部丢掉**（`weight.bin` 1 009 KB → 753 KB；★ **宽度不改变 1×1/dw 的权重形状** ⇒ 少掉的那 25% 只能是层）⇒ 导出的桶是**另一个网络**。
>
> ⇒ 所以本方案的**第一个里程碑不是"飞"，而是"修桶 ＋ 逐桶时延门"**。三道门（G0 等价 / G1 时延 / G2 接入）**全部带数字判据**，任何一门不过就按 §7 的阶梯换候选，而不是硬飞。

**状态**：规划（未实施）。**口径**：空中实链（OTA）＋ `--phy_pipeline npu`（PHY 全 CPU、网络在 ANE）＋ 30 kHz / 20 MHz / n78。
**依据**：`models_above_cpu_delivery.md` §3.2 #1、`architecture_levers_beyond_width.md` §111；运行时语义见 `npu_pipeline_plan.md` 与 `pusch_depth3_receiver.h`。

---

## 0. 结论先行：三道门

| 门 | 要回答的问题 | 判据（数字） | 现状 |
|---|---|---|---|
| ★ **G0 等价** | 桶里的图**是不是同一个网络** | 导出日志**不得出现** `export drops` 丢 `dproj*/se*`；权重数/形状逐一对齐；桶与全带在同一输入上**输出逐元素一致**（容差见 §6 D1） | ★ **通过（见 §11）**：权重与出厂 612 **逐字节相同**；但 SE 模型上桶是**近似**（0.2 LSB RMS、500 单元 −2 个） |
| ★ **G1 时延** | 桶后的单次前向够不够快 | `cpu_ne` 下 `coreml_latency_probe` **p50 ≤ 400 µs**（w108/w180）；> 500 µs 判 NO-GO | ★ **未通过**：w180 = **519–538 µs**（>500），w108 = **393–397 µs**；见 §11 |
| ★ **G2 接入** | 手机能不能接入并跑业务 | attach 成功；`reception FAILED`/`degrad` 计数为 0；`[ul_equalization_demod]` p50 ≤ 600 µs、p99 ≤ 800 µs；nok 率 ≤ 经典臂 + 1 pp | ★ **D4 台上已过（§12）**；**D5 接入被核心网阻塞**（AMF 不在线） |

**⇒ 本方案的核心判断**：G1 是**唯一的真风险**。尺子第一（+3.61%）只比"36 块＋SE＋80 轮"高 **+0.18%（p=0.096，未达显著）**，只比"36 块无 SE＋120 衰减"高 **+0.23%** ⇒ ★ **如果 SE（或 SE＋直连）把桶时延顶到 500 µs 以上，正确的部署选择很可能不是 #1，而是 §7 的 L1 候选**（同样的 36 块、更低的时延、只低 0.23 pp）。

---

## 1. 部署对象（冻结清单）

| 项 | 值 | 出处 |
|---|---|---|
| run | `d3_w2v3c_c32b36dense_e2e-cond-norep-sw2s3-dskip-entry-se_e120_decay`（107 `~/work/ai_phy_train/runs/…`） | 107 实测存在 |
| 架构 | c32 / **36 块** / dense / `--dense-skip-entry --se` / **496 345 参数** / modcond / 无 reporting heads | provenance |
| 权重 | ★ **e117–e120 末端 SWA**，`e120dswa.keras`，sha256 `29637ac0…`（438 个权重张量） | 本机 `~/ai_depth3/probe/d3_w2v3c_c32b36dense_se_e120_decay_e120swa/`（已核对 sha ✓） |
| 部署图 | fp16，输入 `[1, 612, 14, 16]`，`depth3_nsc=612`，四个头 `llr/post_eq_noise/ch_est/dmrs_noise`（本 run 为 `--no-reporting-heads`，后两头缺席属合法子集） | `export.log` |
| sidecar | `s_llr=20 a_noise=4 b_noise=-1`（60 样本，quantised BLER 0.95 = unquantised） | `depth3_scales.txt` |
| bundle（203） | `~/ai_depth3/gate_ai_models/bundles/CeB36_e120d/`，`bundle_files_sha256 cdbb4182…` | 203 实测 |
| bundle（本机） | ★ 已从 203 取回：`~/ai_depth3/gate_ai_models/bundles/CeB36_e120d/`；另有一份本地产物 `~/ai_depth3/probe/…/bundle/e120dswa.mlmodelc` | 本方案执行时完成 |
| 尺子 | **11 983 424（+3.61%）**，OK 4 248/5 000，McNemar 143 : 0，CI [+345 408, +493 160] | CSV 本机 `~/ai_depth3/measure_20261127/ruler_CeB36_e120d.csv`（sha `548e7b7f…`） |
| 全腿 | ★ 1102a 正在 203 上跑（`measure_fulllegen.py`，2026-11-28 12:14 起）；其余三腿未跑 | 203 进程实测 |

---

## 2. 目标环境（★ 香港，`192.168.31.*`）

| 角色 | 主机 | 地址 | 用途 |
|---|---|---|---|
| ★ **飞行机 / 编译** | 本机 Mac mini（M4 Pro，64 GB，macOS 26.6.2，**1 个 ANE**） | **192.168.31.204** | `gnb` 起飞、`coreml_latency_probe`、`ul_chain_replay`、探针/尺子 |
| 回放机 | 203（M2） | **192.168.31.203**（`ssh laptopM2`） | 尺子/全腿回放、bundle 原产地 |
| 训练机 | 107（ASUS） | **192.168.31.107**（`ssh laptopASUS`） | 训练 run、快照 |
| 核心网 | 211（nuc5GC） | **192.168.31.211**（AMF 38412） | attach 必需；`gnb` 绑定 `192.168.31.204` |

**小区/射频**：n78、20 MHz、30 kHz、PCI 1、**51 PRB**、USRP B200（`configs/pinned_mcs*.yml`）。
**⇒ 30 kHz ⇒ 一个 slot 500 µs；本机只有 1 个 ANE** ⇒ 没有"双 ANE 换时间"的余量（`ane_latency_curve.md` §18/§19）。

> ⚠ **配置形态必须先定**：当前工作树的 `configs/pinned_mcs*.yml` 是**未提交的采集态**（`min_ue_mcs=max_ue_mcs=4`、`tx_gain 60 / rx_gain 50`）。
> **钉死 MCS 4 ＝ 只有 QPSK**，而这个模型的主要增益在 **256QAM（+305 496）**，QPSK 只有 **+112 496**（§111(d)）⇒
> ★ **验收腿不能沿用采集态**：要么放开 AMC，要么明确"本轮只验 QPSK 链路的可用性"并**不据此评价模型增益**。
> 飞行前把所用配置的 `git diff` 写进日志头。

---

## 3. 运行时是什么（代码事实，决定步骤）

| 项 | 事实 | 位置 |
|---|---|---|
| 模式 | ★ `--expert_phy.phy_pipeline npu` ＝ **PHY 全 CPU（FFT/CE/均衡/解映射/LDPC）＋ 网络在 ANE**；★ **npu 隐含 `receiver=ai`**，且显式 `classic` 是**冲突报错** | `du_low_phy_pipeline.h:240-290` |
| PURE | ★ **npu 隐含 `OCUDU_DEPTH3_FALLBACK=none`**（显式设置 env 时**不被覆盖**）⇒ 算不了的接收**擦除并计数**，不交经典链 | `du_low_config_translator.cpp:196-205` |
| 模型 | `--expert_phy.pusch_receiver_model <x>.mlmodelc`；缺模型时 `ai` **报错**（不静默降级） | 同上 `:290` |
| 覆盖 | ★ 本 bundle 是 **modcond**：**一个文件算 Qm 2/4/6/8 四种调制**，不需要四个 slot | 启动日志 `computes Qm=…` |
| 桶 | ★ 选**能覆盖 `12×PRB + 96` 的最窄桶**；桶在 `<bundle>/buckets/base_w{108,180,252,324,396,468,540}.mlmodelc`，**612 主文件是全带** | `pusch_depth3_receiver_impl.cpp:958-990` |
| 计算单元 | ★ 默认 **`cpu_ne`**（CPU＋ANE，排除 GPU）；`cpu`/`cpu_gpu` 被拒并改回 | `pusch_depth3_receiver_impl.cpp:377-401` |
| 信封 | ★ **只支持 1 层**、整槽 14 符号、`MIN_PRB=1`、BWP 必须在窗口内（51 PRB） | `pusch_depth3_input_plane.cpp:145-190` |
| 上报 | `--no-reporting-heads` ⇒ RSRP/EPRE/SNR/CFO 来自**共享测量层**（CPU），不来自网络 | `pusch_depth3_measurements.cpp` |
| 计时 | `OCUDU_DEPTH3_TIMING=1` ⇒ 退出时打 `[depth3-timing]`：**每次预测的宽度直方图 + 各宽度 mean predict**，并检测 `SLOW STATE`（≥600 µs 中位） | `fly.sh:65`、`pusch_depth3_receiver_impl.cpp:1382-1400` |

**飞行/采集工具**（都在仓库，符合"工具进仓库、数据进 `~/ai_depth3`"）：

| 工具 | 命令 |
|---|---|
| 起飞（带 summary） | `doc_chinese/depth3_llr_ai/work_tmp/training/fly.sh <leg> [config] [model] [seconds]` |
| 接入专用 | `…/training/fly_attach.sh <bundle> [seconds]` |
| 探针/尺子 | `…/measurement/probe_snapshot.sh`、`…/measurement/measure_arm.py`（`npu` ＋ PURE ＋ generic） |
| 逐单元报表 | `…/measurement/segment_report.py` |

---

## 4. 今天测到的时延（本机，可复现）

命令（每个 bundle 一条；`60` 次计时 ＋ `20` 次预热，`cpu_ne`）：

```bash
OCUDU_DEPTH3_COMPUTE_UNITS=cpu_ne \
  build/lib/phy/upper/signal_processors/channel_estimator/metal/coreml_latency_probe \
  --model <bundle>.mlmodelc --iterations 60 --warmup 20
```

| 模型 | 宽度 | p50 | 说明 |
|---|---|---|---|
| ★★ **`CeB36_e120d`（本方案目标，含 SE＋入口直连）** | **612** | ★ **2 868 µs** | 全带正确图 |
| `CeBd120swa`（b18 ＋ SE ＋ 入口直连） | 612 | 1 518 µs | 同两种杠杆、块数一半 |
| `d3_w2_c32b18_…_e60`（b18 dense，无 SE/无直连） | 612 | 350 µs | 干净基线 |
| 同上 | **108（桶）** | **130 µs** | 桶的真实收益（2.7×） |
| ★ `e120dswa` 的 w108 桶（**今天用现有工具导出的，已丢 SE/dproj**） | 108 | **225 µs** | **不是目标网络**，只能当**下限**参考 |

**⇒ 三条读数**：

1. ★★ **全带不可飞**：2 868 µs ⇒ 按实链换算（见下）EQ-demod ≈ 3.7–5.4 ms ⇒ 管线 **4.3–6.0 ms**，而 2026-11-05 那条 `[ul_equalization_demod] p50 5 385 µs` 的腿 **UL 丢块 39%** ⇒ **612 全带必须避开**。
2. ★★★ **SE＋入口直连很贵**：b18 从 350 → 1 518 µs（**4.3×**，两者叠加后的效果，本次未拆开）；36 块再 ×1.9 ⇒ 2 868 µs。**深度不是主要成本，块内的全局池化/直连投影才是**。⇒ ★ **D2 必须补一次"成本归因"**：同权重分别导出 `无 SE`、`无 SE＋入口直连` 两个变体，各测一张图（每个 ~3 min），才能知道 L1 到底该去掉哪一个。
3. ★★ **桶是唯一出路**：w108 把 612 的成本降 2.7×（b18 实测）；**目标模型的正确桶一旦导出，必须在 400 µs 以内**（G1），否则按 §7 换候选。

**实链口径换算（用两条真实飞行标定）**：

| 来源 | `[depth3-timing]` predict | `[ul_equalization_demod]` p50 | 比 | `[ul_pipeline]` p50 |
|---|---|---|---|---|
| 2026-11-06 c32 b18 桶飞行 | 108:276 µs（mean） | 358.6 µs | **×1.30** | 934 µs（＝ 前端 489 ＋ EQ 359 ＋ 86） |
| 2026-11-05 c128 飞行 | ~2 950 µs | 5 385 µs | **×1.83** | 5 932 µs |

⇒ **预测公式（用于 G1 判据）**：`EQ-demod ≈ 1.3–1.9 × predict`，`pipeline ≈ 489（DFT/前端）＋ EQ-demod ＋ ~85`。
⇒ **G1 的 400 µs 阈值 ⇒ EQ-demod ≤ 600–760 µs ⇒ pipeline ≤ 1.1–1.4 ms（2–3 个 slot）**，与 2026-11-06 那条"能跑通 ping、丢块很少"的腿（934 µs）同档。

> ★ **这个量级不是新发现**：`architecture_levers_beyond_width.md` §104/§103 的"计时注意"已经记过 b36 的**直接 Core ML 前向 2.54 ms（203/M2）**——
> 但当时它是作为"**尺子墙钟不是推理时间**"的例子写的（只占 126 ms/单元的 1.4–2.0%），**没有被换算成槽/管线预算**。本机今天 2.87 ms 与之一致
> ⇒ ★ 差别不在数字，**在于有没有把它当成部署判据**。

> ★ 注意：`coreml_latency_probe` 是**冷启动后预热 20 次**的稳态值；**首次运行/未预热会高一倍以上**（本方案里 225 µs 那条第一次测到 439 µs）。所以 G1 必须按"预热后 p50"记，且行里写明 iterations/warmup。

---

## 5. 关键风险

| # | 风险 | 证据 | 影响 | 处置 |
|---|---|---|---|---|
| ★ R1 | **桶导出静默丢层**（SE / 入口直连 / 未来任何新杠杆） | 今天 `--export-buckets 108` 退出码 0，`export drops training-only layer(s): dproj*_1x1, se*_fc*`（108 个张量） | 桶是**另一个网络**；用它测的时延/精度**全部作废** | G0：修导出 ＋ **禁止丢非训练头** ＋ 数值等价校验（§6 D1） |
| ★ R2 | **全带路径被大分配触发** | 桶只覆盖 ≤ 37 PRB（w540）；更大 ⇒ 回退 612 | 一发大授权就 2.9 ms/次 | 验收流量限制在小分配（ping/UDP 小包）；大分配单独一条"已知限制"腿；可选加"宽带上限→经典"旋钮（§7 L3） |
| ★ R3 | **PURE 擦除** | npu 隐含 `FALLBACK=none` | 算不了就丢 TB，不交经典链 | 首飞显式 `OCUDU_DEPTH3_FALLBACK=estimator`（安全臂）；PURE 只用于测量臂（§6 D6） |
| R4 | **窄带/接入** | 该模型 ≤3 PRB 在尺子上是 **+33 336**（为正，好于 ABCe 的塌陷）；但 c32 家族历史上 2 PRB 有盲区（1/206） | 接入失败＝整条腿不可用 | G2：`fly_attach.sh` ＋ SRB1 重传计数；失败即回安全臂 |
| R5 | **Core ML 放置** | `cpu_ne` 默认；`SLOW STATE` 检测 ≥600 µs | 落到 GPU 路径 ⇒ 慢一倍 | 启动后看 `[depth3-timing]` 是否报 SLOW STATE；报了按 §6 D4 处置（重载/重导） |
| R6 | **口径不可混** | 实链会**擦除**深衰落（不尝试）且带 HARQ 软合并；回放两者都没有 | 实链交付比特 ≠ 尺子；两侧差约 6% | 只做**同日同配置 A/B**，不比绝对值（§6 D6） |
| R7 | **二进制漂移** | `build/apps/gnb/gnb` 是 11-27 08:56 构建，HEAD 11-28 14:19（C++ 未变，但 BIN_SHA 不同） | 腿无法归因 | D0 重建并记 `BIN_SHA` |
| R8 | **机器冲突** | 203 正在跑本模型的 1102a 全腿 | 抢机器 ⇒ 测量污染 | 飞行/建桶都在本机做；不动 203 |
| R9 | **名次不可当结论** | #1 与 #2 差 +0.18%（p=0.096）、与 #3 差 +0.23% | 为 0.2 pp 付 4× 时延不值得 | 部署选型按 **(收益, 时延)** 双目标；G1 不过就走 L1 |

---

## 6. 里程碑与步骤

### D0 冻结与基线（0.5 人日）

**目标**：模型、二进制、参考数字三者可追溯，后面每一腿都能归因。

```bash
cd ~/dev/ocudu
git rev-parse --short HEAD                      # 记 HEAD
cmake --build build --target gnb ul_chain_replay depth3_engine_check coreml_latency_probe -j 10
shasum -a 256 build/apps/gnb/gnb | cut -c1-16   # 记 BIN_SHA
shasum -a 256 ~/ai_depth3/probe/d3_w2v3c_c32b36dense_se_e120_decay_e120swa/e120dswa.keras
# 期望 29637ac004842a60e9fac98f09d63be0e6b39a4c62f1fa47c599ada020bc7d9a
python3 - <<'PY'                                # 记录产物指纹（与 203 的 bundle_files_sha256 cdbb4182… 对账；
import hashlib, pathlib                        # 若文件集合不同会不等，那就只作为"本次冻结"的指纹）
root = pathlib.Path.home()/"ai_depth3/gate_ai_models/bundles/CeB36_e120d"
h = hashlib.sha256()
for p in sorted(x for x in root.rglob('*') if x.is_file()):
    h.update(str(p.relative_to(root)).encode()); h.update(p.read_bytes())
print("BUNDLE_FILES(recorded):", h.hexdigest())
PY
```

**判据**：`BIN_SHA` 与 `BUNDLE_FILES` 写进方案记录；keras sha 与 provenance 一致 ✓（已核对）。
**失败处置**：sha 不一致 ⇒ 停止，先查是不是取错了产物（本机 `probe/` 与 203 的 `bundles/` 两处都有）。

---

### D1 ★ 修桶：让桶等于同一个网络（1–1.5 人日，**本方案的技术核心**）

**为什么必须先做**：今天实测证明，**现有工具对 `--se` / `--dense-skip-entry` 会静默生成错误图**——不是"报错"，是"能加载、能跑、数字好看"。

**(a) 代码事实（问题定位）**

| 位置 | 问题 |
|---|---|
| `train_depth3.py` `export_buckets()` | 宽度/深度/块类型/瓶颈/头 从 **source** 推断；★ **`se` / `dense_skip` / `dense_skip_entry` / `dense_skip_depth` / `mlp_expand` 仍取命令行默认值** ⇒ 重建出来的图没有这些层 |
| 同函数 | 丢层是**合法的**（为砍训练专用头 `ce_head` 而写），所以 `dproj*`/`se*` 被当成"训练专用"静默丢弃，**权重数检查反而能通过**（被丢的层不在目标图里） |
| `make_bucket_bundle.sh` | 只透传 `--dense-blocks` / `--dense-bottleneck`，**没有 `--se` / `--dense-skip-entry`** |

**(b) 修法（最小、可验证）**

1. **从 source 反推**（与已有 width/depth/bottleneck/heads 的做法一致，避免第二个真相源）：
   - `se = any(name.startswith("se") and name.endswith("_fc1"))`；
   - `dense_skip* = any(name.startswith("dproj"))`，再看 `dproj0_1x1` 的 `kernel.shape[2] / c_hid`：**2 ⇒ 纯 dense-skip，3 ⇒ 带入口直连**；`dense_skip_depth = 1 + (concat 宽 - 2c - (3c | 0))/c`（或直接读 `dcat0` 的输入宽度）；
   - `mlp_expand = 2 if any(pw{i}a_1x1)`（宽度比可判）。
2. ★ **把"丢层"变成白名单**：只允许丢 `{ch_est*, dmrs_noise*, ce_*}` 这类**已知训练专用**头；**其它任何层被丢 ⇒ `ValueError` 退出**。今天的静默丢层正是这条缺失。
3. **加数值等价校验**（新增，防再次静默）：同一固定输入，全带图取对应裁剪区域 vs 桶图，**逐元素**比较 LLR 头（fp16 容差 `|Δ| ≤ 2 LSB`，或相对误差 ≤ 1e-3；先测出真实差异再定死）。文档声称"裁剪逐位一致"（`export_buckets` 注释），本条就是把这句话变成**可执行的门**。
4. `make_bucket_bundle.sh` 增加 `--se` / `--dense-skip-entry` / `--dense-skip-depth` / `--mlp-expand` 透传（即使有 (1)，命令行显式值也应能覆盖，便于 A/B）。

**(c) 产物**

```bash
# 修好后，一条命令产出 7 桶 + 全带 + sidecar（PY 指向带 TF+coremltools 的 venv）
PY=~/ai_ce_work/venv/bin/python \
  lib/phy/upper/ai_signal_processors/depth3/ai_train/make_bucket_bundle.sh \
  ~/ai_depth3/probe/d3_w2v3c_c32b36dense_se_e120_decay_e120swa/e120dswa.keras \
  --name CeB36_e120d --c-hid 32 --blocks 36 --calib ~/ai_depth3/datasets_monitor_1102a \
  --se --dense-skip-entry --dense --out ~/ai_depth3/gate_ai_models/bundles/CeB36_e120d --measure
```

**判据（G0，全过才算过）**

| 检查 | 期望 |
|---|---|
| 导出日志 | ★ **没有** `export drops … dproj*/se*`；若打印，必须是白名单头 |
| 权重 | 桶与全带**同一组层名/形状**（导出内建检查通过） |
| 数值 | 全带裁剪 vs 桶，LLR 头逐元素 ≤ 容差（把实测最大差写进日志） |
| 契约 | `depth3_engine_check <每个桶>` 通过（输入 `x`、四头、写入非空） |
| 权重体积（辅助证据） | ★ 同宽度的正确桶应与全带**几乎等大**（1×1/dw 的权重形状与宽度无关）；今天那张错图 `weight.bin` 小 25% ⇒ 体积只能当**辅助**证据，门看层名/张量数 |

**失败处置**：数值等价不过 ⇒ **不要飞**。先查是不是裁剪边界/上下文 4 PRB 的问题，再考虑"只导出宽桶（324/396/540）"这类降低风险的做法。

---

### D2 ★ 时延门（0.5 人日）

```bash
P=build/lib/phy/upper/signal_processors/channel_estimator/metal/coreml_latency_probe
for w in 108 180 252 324 396 468 540; do
  printf 'w%-4s ' $w
  OCUDU_DEPTH3_COMPUTE_UNITS=cpu_ne $P \
    --model ~/ai_depth3/gate_ai_models/bundles/CeB36_e120d/buckets/base_w$w.mlmodelc \
    --iterations 60 --warmup 20 2>&1 | grep '^predict'
done
OCUDU_DEPTH3_COMPUTE_UNITS=cpu_ne $P \
  --model ~/ai_depth3/gate_ai_models/bundles/CeB36_e120d/CeB36_e120d.mlmodelc --iterations 60 --warmup 20
```

| 判据 | 阈值 | 依据 |
|---|---|---|
| ★ **w108 / w180 p50** | **≤ 400 µs 通过；400–500 谨慎（可飞但只看 nok）；> 500 NO-GO** | §4 换算：EQ-demod ≤ 600–760 µs、pipeline ≤ 1.1–1.4 ms |
| p99 | ≤ 1.3 × p50 | 尾延迟进 `[ul_tail]` 的 1.25×/1.5× 计数 |
| 全带（612） | **只记录、不作为验收路径** | 已知 2 868 µs |

**⇒ 这一门就是"这个模型能不能上实链"的答案**。通过 ⇒ D3；不通过 ⇒ §7。

**(b) ★ 成本归因（G1 不过时立刻要做，30 min）**

同一份 `e120dswa.keras`，**只改杠杆**、不重训，各导一张全带图并测：

| 变体 | 杠杆 | 目的 |
|---|---|---|
| `noSE` | 去掉 `--se` | 分离全局池化的代价（对应 L1 候选） |
| `noSEnoSkip` | 再去掉 `--dense-skip-entry` | 分离直连投影的代价（对应 L2/更廉价的替代） |

判据：哪一个变体能把 **全带** 从 2 868 µs 压到 ~800 µs 以下，它就是桶时延的主要来源 ⇒ 换候选时优先去掉它。
（这两张图**只是测量用**，不得作为部署产物——精度与尺子上的模型不同。）

---

### D3 回放复核（0.5 人日，便宜且必要）

目的：确认**本机这份二进制 + 这个 bundle** 与记录的尺子数字同源（避免"飞的是另一个东西"）。

```bash
# 20 个尺子单元做预检（不是 5 000）。measure_arm.py 的模型参数是"相对 bundles/ 的路径"
head -20 ~/ai_depth3/ruler_s2_amc_5000/units.txt > /tmp/pre20.txt
python3 doc_chinese/depth3_llr_ai/work_tmp/measurement/measure_arm.py \
  /tmp/pre20.txt CeB36_e120d/CeB36_e120d.mlmodelc /tmp/pre20.csv
python3 - <<'PY'
import csv, os
rec = {r['unit']: r for r in csv.DictReader(
    open(os.path.expanduser('~/ai_depth3/measure_20261127/ruler_CeB36_e120d.csv')))}
new = list(csv.DictReader(open('/tmp/pre20.csv')))
diff = [(r['unit'], r['status'], rec[r['unit']]['status'])
        for r in new if r['unit'] in rec and r['status'] != rec[r['unit']]['status']]
print(f"20 单元一致 {len(new)-len(diff)}/{len(new)}", diff[:5])
PY
```

> ★ `measure_arm.py` 已写死 `OCUDU_DEPTH3_FALLBACK=none`、`--phy_pipeline npu`、`--pusch_ldpc_decoder_type generic`；语料路径是仓库内
> `lib/phy/upper/ai_signal_processors/test/ruler_s2_amc_5000_corpus`（本机已有 5 000 个捕获）。

**判据**：20/20 状态一致（`OK`/`no`）。不一致 ⇒ 先查 `BIN_SHA`/单元集/sidecar，**不要**继续飞。
**备注**：走桶之后**回放数字仍应逐单元一致**（桶=裁剪，语义不变）——这也是 G0 的间接验证。

---

### D4 台上（无 UE，0.5 小时）

```bash
# 安全臂：显式打开 fallback（npu 只在 env 未设时才强制 PURE）
OCUDU_DEPTH3_FALLBACK=estimator \
  doc_chinese/depth3_llr_ai/work_tmp/training/fly.sh ceb36e120d_bench configs/pinned_mcs04.yml CeB36_e120d 60
# 日志头另存一份"这一腿用的是哪份配置"：
{ git -C ~/dev/ocudu status --porcelain; git -C ~/dev/ocudu diff -- configs/; } \
  > ~/ai_depth3/flight_logs/ceb36e120d_bench/config_$(date +%H%M%S).diff
```

> ★ `fly.sh` 位置约定：bundle 必须在 `~/ai_depth3/gate_ai_models/bundles/<名>/<名>.mlmodelc`；第 4 个参数是"到点自动 SIGINT"，
> **手动停止只按一次 Ctrl-C** 并等 `Stopping...`（summary 在退出路径上打；第二次 Ctrl-C 会杀掉它）。

**判据（三行日志 ＋ 两行探针）**

| 检查 | 期望 |
|---|---|
| `[phy_pipeline] receiver=ai` | 出现 |
| `[pusch_receiver] AI depth-3: … bound …` ＋ `computes Qm=2/4/6/8` | 四调制全覆盖（modcond 一包） |
| `narrow-band buckets bound: 108 … 540` | ★ 出现（不出现＝全带在跑，必然超时） |
| `startup grace = 0 s`、`compute units 'cpu_ne'` | 与预期一致 |
| `radio sample continuity: 0 gaps … OK` | 出现 |
| `[depth3-timing] … SLOW STATE` | ★ **不出现**；出现则换一轮重飞（放置是加载时决定的，重启一次可能换回），仍出现则记 R5 并降低对时延的预期 |

---

### D5 接入门（G2，0.5 人日 ＋ N 次重复）

**前置清单（不满足就别飞，先修环境）**：

| 检查 | 命令/依据 |
|---|---|
| 5GC 在跑（AMF 38412） | `ssh nuc5GC 'pgrep -fl open5gs-amfd'`；`gnb` 日志出现 `NG Setup … completed` |
| 手机在位、无 WiFi 抢占 | 手机开飞行模式再插卡；2.4/5 GHz 关闭（历史坑：干扰导致 attach 失败） |
| `gnb` 绑定地址 | `192.168.31.204`（配置 `cu_cp.amf.bind_addrs`） |

```bash
CFG=configs/pinned_mcs04.yml doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach.sh CeB36_e120d 90
```

| 判据 | 期望 |
|---|---|
| RRC 建立 / `InitialUL` / `RRC Setup` | 成功（`grep -ac` 计数 > 0） |
| `reception FAILED`/`degrad` 行 | ★ **0**（PURE 下这些就是被擦除的 TB） |
| `[ul_equalization_demod]` p50 / p99 | ★ ≤ 600 / ≤ 800 µs |
| `[depth3-timing]` 分配宽度直方图 | 以 108/180 为主（4 PRB 的 ping ⇒ 180） |
| SRB1 重传次数 | ≤ 经典臂同量级（历史教训：2841 次 = 接入失败） |

**失败处置（按顺序）**：① 确认连接收机也是 `ai`（不是 classic 兜底）；② 加 `--expert_phy.depth3_startup_grace_s 20`（接入期走经典链）；③ 回 `OCUDU_DEPTH3_FALLBACK=estimator` 飞一轮；④ 仍失败 ⇒ 记"窄带盲区"，走 §7 L3。

---

### D6 业务与块测量（1 人日）

**三臂、同日、ABBA、每臂 3 次、每次 3 min**：

| 臂 | 命令 | 说明 |
|---|---|---|
| **A 经典** | `gnb -c configs/pinned_mcs04.yml --expert_phy.phy_pipeline cpu` | 基线 |
| **B AI＋安全** | `OCUDU_DEPTH3_FALLBACK=estimator fly.sh …` | 出问题时能回退，量的是"部署形态" |
| **C AI＋PURE** | `fly.sh …`（npu 默认即 PURE） | 量的是"纯网络"能力 |

**流量**：① ping（4 PRB，落 180 桶）；② UDP 上行小包（受控速率，避免大分配触发 612）；③ **单独一条**"大分配压力腿"（iperf UL）——只为记录"全带 2.9 ms 的后果"，不作为验收。

**每次记录**：`[ul_pipeline]`（p50/p95/p99）、`[ul_equalization_demod]`、`[ul_tail]`、`[ul_by_size]`、`[depth3-timing]`（宽度直方图 ＋ 每宽度 mean predict）、`[sched-mcs]` 分布、`[ul_mac_pdu_size] total`、MAC ok/nok 表、`radio sample continuity`、SINR/epre 中位（证明三臂可比）。

**判据**

| 指标 | 期望 |
|---|---|
| UL 块 nok 率 | ≤ 经典臂 **+1 pp** |
| 吞吐（同 SINR 档） | ≥ 经典臂 **95%** |
| `[ul_pipeline]` p50 | ≤ **1.4 ms**；p99 ≤ 2×p50 |
| 无腿级失败 | 无 panic/无 `Real-time failures` 上升 |

---

### D7 判定与归档（0.5 人日）

1. **判定**：G0/G1/G2 与 D6 判据逐条打勾；任一不过 ⇒ 结论是"不可部署（附原因）"，不是"再试试"。
2. **回写文档**（每行带时间戳）：
   - `models_above_cpu_delivery.md`：给 #1 行加"实链"列（时延/接入/nok）；
   - `depth3_gate_and_model_pk_npu.md`：新增"实链"一节（口径：OTA ＋ npu ＋ 30 kHz ＋ 1 ANE）；
   - `gate_measurement_runbook.md`：把"桶导出必须核对 `export drops`"写进坑表；
   - 本文 §7 的结论（选型阶梯）。
3. **归档**：`~/ai_depth3/flight_logs/<leg>/`；bundle 目录保留 `provenance.txt`/`depth3_scales.txt`/导出日志；`BIN_SHA`/`BUNDLE_FILES`/`UNITS_SHA` 写进日志头。

---

## 7. 如果 G1 不过：四条阶梯（按代价排序）

| # | 方案 | 代价 | 预期 |
|---|---|---|---|
| ★ **L1** | 换 **`Ce36_e120d`**（36 块、`--dense-skip-entry`、**无 SE**，485 833，尺子 **+3.38%**） | −0.23 pp | ★ SE＋入口直连叠加是 4.3×（§4，本次未拆开）⇒ **去掉 SE 是最便宜的第一次尝试**；能否压到 ≤300 µs 必须由 D2(b) 的归因实测回答，不要假设 |
| **L2** | 用 **`CeB36_80`**（36 块＋SE＋80 轮恒定，+3.43%） | −0.18 pp，SE 仍在 | 只在"SE 不是成本主因"时才值得 |
| **L3** | 小分配走 AI、大分配走经典（加"宽带上限"旋钮；需 `FALLBACK=estimator`） | 代码改动 ＋ 非 PURE | 保住接入与吞吐，AI 只覆盖 ≤37 PRB |
| **L4** | 换 **15 kHz**（1 ms slot）或等 **M6 双 ANE** | 改小区配置 / 换硬件 | 预算翻倍，但会改变整个现网形态 |

**⇒ 决策规则**：先量 L1；若 L1 的桶 ≤ 300 µs，则**部署 L1 而不是 #1**——因为**多 0.23 pp 不值得把管线从 1.2 ms 顶到 1.6 ms**。

---

## 8. 一览：从今天到可飞，最短路径

```
D0 冻结（0.5d）──► D1 修桶＋等价（1–1.5d）──► G0 ──► D2 逐桶时延（0.5d）──► G1
                                                                              │
                       ┌──────────────────────────────────────────────────────┘
                       ▼
                 G1 过？ ──否──► §7 L1（换无 SE 候选，回到 D1）
                       │是
                       ▼
                 D3 回放 20 单元（0.5d）──► D4 台上无 UE（0.5h）──► D5 接入 G2（0.5d）
                       │
                       ▼
                 D6 三臂业务/块（1d）──► D7 判定＋归档（0.5d）
```

**合计**：约 **5–6 人日**（其中机时：建桶 ~0.5 h、逐桶探测 ~10 min、台上一轮 ~5 min、接入/业务 ~3–4 h），**不含**候选切换的返工。

---

## 9. 不在范围

- 不重训、不并集、不 c128、不改构建默认模型（`OCUDU_DEPTH3_DEFAULT_MODEL` 仍为 `d3_e4`）。
- 不把实链数字与尺子/全腿数字混表（口径不同，见 R6）。
- 不宣称"超过 CPU+HARQ"——本模型 **`＋HARQ` 未测**（表里是"—"）。
- 不改 `MIN_PRB`、不改 1 层信封（那会动模型覆盖与训练分布）。

---

## 10. 附录

### 10.1 旋钮字典（本方案用到的）

| 旋钮 | 默认 | 本方案用法 |
|---|---|---|
| `--expert_phy.phy_pipeline` | `auto` | ★ `npu`（隐含 `ai` ＋ PURE） |
| `--expert_phy.pusch_receiver_model` | 构建默认 E4 | ★ 指向 `CeB36_e120d/CeB36_e120d.mlmodelc` |
| `--expert_phy.depth3_startup_grace_s` | `0` | 接入脆弱时给 20 s（接入期走经典链） |
| `OCUDU_DEPTH3_FALLBACK` | 由模式决定（npu ⇒ `none`） | ★ 安全臂设 `estimator`；测量臂用 `none` |
| `OCUDU_DEPTH3_COMPUTE_UNITS` | 接收机强制 `cpu_ne` | 只用 `cpu_ne`；`cpu/cpu_gpu` 会被拒 |
| `OCUDU_DEPTH3_TIMING` | 关（`fly.sh` 默认开） | ★ 必开：宽度直方图 ＋ SLOW STATE |
| `OCUDU_DEPTH3_BUCKETS` | 空＝用 bundle 的 `buckets/` | 调试时 `none`（强制全带，**仅用于对照**） |
| `OCUDU_DEPTH3_MIN_PRB` | `1` | 不动 |
| `OCUDU_DEPTH3_DOMAIN_GATE` | 关 | 不动（开了会按 SNR 拒收，PURE 下＝擦除） |

### 10.2 日志行字典（判定只看这几行）

| 行 | 含义 | 期望 |
|---|---|---|
| `[phy_pipeline] receiver=ai` | 接收机是 AI | 出现 |
| `[pusch_receiver] AI depth-3: … bound: … compute units 'cpu_ne'` | 模型绑定成功 | 出现 |
| `… computes Qm=2/4/6/8` | modcond 覆盖四调制 | 4 行 |
| `… narrow-band buckets bound: 108 180 … 540` | ★ 桶生效 | 出现 |
| `… PURE mode` / `… domain gate off` | 口径 | 与臂一致 |
| `[depth3-timing] … allocation mix` / `mean predict per width` | 实际用了哪些桶、多快 | w108/180 为主 |
| `[depth3-timing] SLOW STATE: …` | Core ML 落到慢路径 | ★ 不出现 |
| `[ul_equalization_demod]` | 臂自身耗时 | p50 ≤ 600 µs |
| `[ul_pipeline]` | 整条 UL 管线 | p50 ≤ 1.4 ms |
| `reception FAILED` / `degrad` | 擦除/降级 | 0 |

### 10.3 本文引用的实测（可复现）

| 项 | 值 | 怎么得到 |
|---|---|---|
| 全带 612（目标模型） | 2 868 µs p50 | `coreml_latency_probe --iterations 60 --warmup 20`，`cpu_ne` |
| b18＋SE＋入口 612 | 1 518 µs | 同上（`CeBd120swa`） |
| b18 dense 612 / 108 | 350 / 130 µs | 同上（`d3_w2_c32b18_…_e60` ＋ 其 `buckets/base_w108`） |
| 现工具导出的 w108 桶（**错图**） | 225 µs | 同上，`/tmp/d3bucket/e120dswa_w108.mlmodelc` |
| 桶导出丢层 | `dproj*`×36、`se*_fc*`×72（108 个张量）；`weight.bin` 1 009 232 → 753 088 B | 今天 `--export-buckets 108` 的 `export.log`；退出码 0 |
| 实链 c32 b18 桶飞行 | predict 108:276 µs；EQ 358.6；管线 934 | 2026-11-06 腿 `~/ai_depth3/flight_logs/c32_small/` |
| 实链 c128 失败腿 | EQ 5 385 µs；UL nok 39% | 2026-11-05 `attach_c128_e17_40k…log`；`attach_capability_test_plan.md` §7 |

### 10.4 与既有文档的关系

| 文档 | 关系 |
|---|---|
| `models_above_cpu_delivery.md` §3.2 | 本模型的名次与显著性来源 |
| `architecture_levers_beyond_width.md` §111 | 训练/产物/分解的完整记录 |
| `npu_pipeline_plan.md` | `npu` 语义、PURE、词汇表的来源 |
| `attach_capability_test_plan.md` | 接入门与窄带历史（2 PRB 盲区、SRB1 重传 2841 次） |
| `ane_latency_curve.md` §16–§20 | 宽度×深度×dense 的时延曲线（本方案 §4 的对照） |
| `gate_measurement_runbook.md` | 探针/尺子/全腿脚本与坑表 |

---

★ 状态（2026-11-28 14:40）：
★ **目标模型已在两地可取**（本机 `probe/…` 与 `bundles/CeB36_e120d`；203 原产地）✓；
★★ **全带 2 868 µs 已实测**，与"39% 丢块"那条腿同档 ⇒ **必须走桶** ✓；
★★★ **桶导出对 SE/入口直连静默丢层已实测取证**（108 个张量、退出码 0）⇒ **D1 是第一步，且必须带数值等价门** ✓；
★ **逐桶时延（正确图）未测** ⇒ **G1 是本方案唯一的真未知** ✓。

---

## 11. ★★★★★★ D1 执行结果（2026-11-28 15:40，本机）

**一句话**：D1 修好了，而且**证明桶现在就是同一个网络**；但两个新事实改变了结论 —— ★ **SE 让桶从"精确"变成"近似"**（可量化，很小），★ **36 块模型的桶时延过不了 G1**（w180 519–538 µs）。

### 11.1 改了什么

| 文件 | 改动 | 为什么 |
|---|---|---|
| `lib/.../depth3/ai_train/train_depth3.py` | ★ 新增 `trunk_flags_from_source()`：从**模型本身**反推 18 个结构参数（块数、cv/dw、核、dilations、groups、宽度、输入通道、cond、qm、SE 及其 ratio、mlp 倍率、dense-skip 的**入口/深度**、fuse_relu、aux/reporting、noise_to_llr、llr_post、cst） | 桶必须"同一张图换宽度"；这些参数原本**只从命令行取**，漏一个就是另一个网络 |
| 同上 | ★ 丢层改为**白名单**（只允许 `ce_res_1x1`/`sym_aux_1x1`/`ch_est_1x1`/`dmrs_noise_1x1`），其余缺失**抛错** | 旧逻辑把 `dproj*/se*` 当"训练专用"静默丢弃，退出码 0 |
| 同上 | ★ 权重校验改为 **name→shape 字典**相等（不是按位置 zip），并**按名字**拷贝 | Keras 的 `model.weights` 顺序是构建顺序；桶的构建顺序不同 ⇒ 按位置 zip 会把同形状张量**错位赋值**（旧图因此算出 NaN） |
| `make_bucket_bundle.sh` | 要求导出日志必须有 `bucket arch from source`（否则失败）；sidecar 无条件复制进 `buckets/`；★ 修 `REPO` 少一级导致 `--measure` **静默不测**；`ls` 的空 glob 不再被 `set -e` 杀掉 | 三个都是"工具说了但没做"的类型 |
| `depth3_engine_check.cpp` | ★ 平面宽度改为**从 bundle 取**（modcond 是 16 通道，不是契约的 12）；接受**两头上报头缺席**的合法子集 | 旧工具给 16 通道模型只备了 12 通道缓冲（短 25%）⇒ **越界读**，并且把合法的 `--no-reporting-heads` 判 FAIL |
| ★ 新增 `work_tmp/analysis/bucket_equiv.py` | 在**真实平面**上比较全带 vs 桶（按运行时的裁剪规则取窗口），报 max/RMS/sign-flip，`--check` 可当门 | G0 的"逐元素一致"原本只是注释里的一句话，没有工具 |

### 11.2 证据（全部本机实测）

| 项 | 结果 |
|---|---|
| 反推出的架构 | `36 blocks, cv k=3, c_hid 32, dilations (1,2,4,8), groups 1, mlp x1, se x8, dense-skip entry/1, cond True, in 16, qm 8, heads aux=True reporting=False` |
| ★ 权重 | **438 个张量**，`weights/weight.bin` sha256 `f6d8f10c…`，**与出厂 612 逐字节相同**（1 008 832 B）；旧图是 **753 088 B**（少 108 个张量） |
| ★ 数值（新 612 vs 出厂 612） | **max\|Δ\| = 0**（新导出复现了出厂模型） |
| 契约 | w108 / w180 / w540 的 `depth3_engine_check` 全 **PASS**（llr 12096/20160/60480 全写入） |
| ★ 等价（真实平面 4 样本，SE 模型） | w108 **RMS 0.218 LSB**（max 0.075）、w180 0.120、w540 0.035 ⇒ **近似但很小** |
| ★ 端到端（500 单元，PURE，同二进制） | **SE 模型**：带桶 444 OK / 1 649 992 bit；全带 446 OK / 1 656 112 bit ⇒ **−2 单元 / −6 120 bit（−0.37%）**，重跑逐单元一致；**无 SE 对照（`Ce36_80`）：0 差异**（两边 445 OK / 1 652 016 bit） |
| ★ 时延（3 次取最好 p50，两轮独立复测） | b36+SE：w108 **393–397 µs**、w180 **519–538 µs**；b36 无 SE：w108 **299–309**、w180 **387–403**；旧 b18 无 SE：w108 **130** |

### 11.3 两条结论（都已改写 §0 的门）

**G0：通过，但"bit-for-bit"那句话对 SE 不成立。**
- 同一个网络 ✓（权重逐字节相同，数值 max|Δ|=0）；
- ★ 但 **SE 的 squeeze 是对整个输入窗求平均**，108 宽的窗与 612 宽的平均值不同 ⇒ 桶是**近似**。实测代价：**0.2 LSB RMS**、500 单元 **−2 个单元 / −0.37% 比特**；对**无 SE** 模型是**精确**（500 单元 0 差异）。
- ⇒ 代码注释与文档里"crop 逐位一致"的措辞已按此改写（`export_buckets` 的 docstring）。

**G1：未通过（按预注册阈值）。**
- w180 = **519–538 µs** > 500 µs 的停止线；w108 = **393–397 µs** 也只是勉强进 400。
- 换算：in-flight predict ≈ 2× ⇒ predict 1.0 ms 级、EQ-demod ≈ 1.3–2.0 ms、pipeline ≈ 1.9–2.5 ms。
- ⇒ ★ **不是"不能飞"**（已知可用腿只有 934 µs，已知失败腿是 5.9 ms），而是**落进没有先例的区间**，只能由实链回答。

### 11.4 决策建议（取代 §7 的抽象阶梯）

| 序 | 动作 | 理由 |
|---|---|---|
| ★ 1 | **仍飞一次 D4/D5**（约 1 h），`OCUDU_DEPTH3_TIMING=1` ＋ `FALLBACK=estimator` | G1 的阈值是估算；判据是 `[ul_tail]`/nok/`[depth3-timing]`。桶已证明是同一网络，飞的是"这个模型" |
| ★ 2 | **同时准备 fallback**：`Ce36_e120d`（无 SE，+3.38%） | 无 SE 的桶**精确**且 w180 = **387–403 µs**（本地代理 `Ce36_80` 实测）⇒ 管线预计低 **~130–150 µs** |
| 3 | 若首飞 nok > 经典＋1 pp 或 `[ul_tail]` 1.5× 抬头 | ★ 直接换 no-SE 候选，**不要**靠调 concurrency/ABBA 去救 |
| 4 | 若必须 36 块＋SE | 只能 15 kHz 或双 ANE（M6） |

### 11.5 产物与遗留

| 项 | 位置 |
|---|---|
| 部署 bundle（含 7 个桶） | `~/ai_depth3/gate_ai_models/bundles/CeB36_e120d/`（`provenance.txt` 已追加本次记录）<br>★ **口径提醒**：记录里的尺子 11 983 424 是在**无桶**的 bundle 上量的；这个目录从今天起带桶 ⇒ 复跑尺子会看到 **−0.37% 量级的差**（不是模型变了）。要复现旧数字请指向 `CeB36_e120d_full/` |
| 对照 bundle | `…/CeB36_e120d_full/`（无桶）、`…/Ce36_80_bb/` 与 `…/Ce36_80_full/`（无 SE 对照） |
| 500 单元 A/B 的 CSV | `~/ai_depth3/measure_20261128/` |
| 工具 | `doc_chinese/depth3_llr_ai/work_tmp/analysis/bucket_equiv.py` |

**遗留（按优先级）**：
1. ★ **时延测量受机时污染**（测时 load 5–9，另有两个 `ul_chain_replay_cpu` 在跑）⇒ §11.2 的时延应读作**上界**；静默窗口应复测一次。
2. ★ **等价校验还没有接进导出流程**：现在是"工具 + 人工门"。要变成硬门，应在 `export_buckets` 导完 w612 与 w108 后自动跑一次 `bucket_equiv --check`（需要 Core ML，只适合 macOS）。
3. 实链 D4–D6 未做。

---

## 12. ★★★★★★ D4 台上（无 UE）：通过（2026-11-28 16:43，本机）

**场景**：B210 已插上（`uhd_find_devices` → `product: B210, serial 000000560`），`configs/pinned_mcs04.yml`、`--phy_pipeline npu`、带 7 个桶的 `CeB36_e120d`、`OCUDU_DEPTH3_FALLBACK=estimator`（安全臂）、`OCUDU_DEPTH3_TIMING=1`，跑 70 s 后单次 SIGINT。

| 检查 | 实测 | 判定 |
|---|---|---|
| 小区 | `Cell pci=1, bw=20 MHz, 1T1R, dl_arfcn=627264 (n78), dl_freq=3408.96 MHz` | ✓ |
| 射频连续性 | `0 gaps over 1 709 001 blocks (0 missing/repeated, 0 overflow) -> OK` | ✓ |
| AI 臂绑定 | `AI ARM ACTIVE over 4 model(s)`；`narrow-band buckets bound: 108 180 252 324 396 468 540`；`geometry: 612 subcarrier(s)`；`contract=modcond: ONE bundle computes Qm={2,4,6,8}` | ✓ |
| 模式 | `fallback`（显式 `OCUDU_DEPTH3_FALLBACK=estimator`）；`reception FAILED = 0`；无 `SLOW STATE` | ✓ |
| 核心网 | `"NG Setup Procedure" timed out after 5000ms` | ✗（AMF 不在线，见下） |
| 进程 | 无 panic、无 `Real-time failures`；能正常 SIGINT 退出并打印 summary | ✓ |

**★ 顺带确认了两件事**：
1. ★ **`gnb` 在本机不需要 sudo 就能跑**（UHD 在 macOS 上不要求 root）——`fly.sh` 里的 `sudo` 不是功能必需；本次 bench 是普通用户跑的，射频仍然零丢块。所以"给不给 sudo"不再是障碍。★ 但要注意：`sudo` 在本项目里还用于**实时线程优先级**，普通用户跑时功能正确、**时延读数可能偏高** ⇒ 接入/打通用它没问题，**时延判据仍要用 sudo 的腿**（或用 `[depth3-timing]` 的 predict 取值）。
2. ★ `gtp-u 2152` 被占用时 `gnb` 会在启动阶段直接报 `Failed to bind UDP socket ... 2152`（我第一次 bench 撞到，因为前一个进程还在退出）⇒ 飞前必须确认没有残留进程（`lsof -nP -i :2152`）。这条已写进脚本的预检。

## 13. ★★★★★★ D5 接入：**当前被核心网阻塞**（2026-11-28 16:50 核查）

| 检查 | 结果 |
|---|---|
| 211（`nuc5GC`，配置里 `amf.addrs`） | ★ **没有 open5gs**：无进程、无 `~/work/open5gs`、`find / -maxdepth 4 -name open5gs-amfd` 为空、38412 不监听 |
| 203 / 107 | 无 open5gs |
| 192.168.31.64 / .66 / .122 | 无 open5gs（`.122` 的 SSH 主机键变了，未绕过） |
| 本机 | 无 docker、无 open5gs 二进制 |

⇒ ★ **手机接入必须先有核心网**：NGAP 38412 无人监听 ⇒ `NG Setup` 超时 ⇒ UE 无法注册（与 2026-11-05 那次 `attach` 失败的机理完全相同）。

**启动 5GC 的修正命令**（来自 `depth3_llr_ai_design_and_implementation.md` §13.x 的排障记录：原 `start_5gc.sh` **漏了 `amfd`**，且把 `nrfd`/`scpd` 各启了两次 ⇒ `7777` 端口冲突；这里每一项只启一次并补上 `amfd`）：

```bash
cd ~/work/open5gs
BIN=./install/bin ; CONF=./install/etc/open5gs
sudo pkill open5gs- ; sleep 2                                   # 确认旧实例真的退出
$BIN/open5gs-nrfd  -c $CONF/nrf.yaml  & sleep 1
$BIN/open5gs-scpd  -c $CONF/scp.yaml  & sleep 1
$BIN/open5gs-ausfd -c $CONF/ausf.yaml &
$BIN/open5gs-bsfd  -c $CONF/bsf.yaml &
$BIN/open5gs-nssfd -c $CONF/nssf.yaml &
$BIN/open5gs-pcfd  -c $CONF/pcf.yaml &
$BIN/open5gs-udmd  -c $CONF/udm.yaml &
$BIN/open5gs-udrd  -c $CONF/udr.yaml &
$BIN/open5gs-smfd  -c $CONF/smf.yaml &
sudo $BIN/open5gs-upfd -c $CONF/upf.yaml &
$BIN/open5gs-amfd  -c $CONF/amf.yaml &                        # ★ 原脚本缺这一行 ⇒ 38412 从不监听
sleep 2 ; ss -lunp | grep 38412 && echo "AMF UP"
```
★ 若这台机器用 systemd：`sudo systemctl restart open5gs-amfd` 更稳（会等旧进程退出）。

**⇒ AMF 起来之后，接入腿是这一条命令**（自带预检 + 判定；不需要 sudo）：

```bash
doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh CeB36_e120d 120
```

它做四件事：预检（射频 / AMF 38412 / 2152 是否被占 / bundle 是否有桶 / load）→ 起飞（`npu` ＋ 安全臂 ＋ `TIMING=1`）→ 到点单次 SIGINT 并等 summary → `flight_verdict.py` 打印 **RADIO / ATTACH / DATA / AI ARM** 四行判定与全部关键时延。

**★ 新增两个工具**（都在仓库）：

| 工具 | 用途 |
|---|---|
| `work_tmp/analysis/flight_verdict.py <log>` | 一次读四个角落：射频连续性、AMF/N2、UE 行与 UL 列、AI 臂绑定、桶、`[depth3-timing]`、`[ul_pipeline]`/`[ul_equalization_demod]`、`reception FAILED`、`SLOW STATE`；退出码可当门 |
| `work_tmp/training/fly_attach_verify.sh <bundle> [秒] [配置]` | 预检 → 起飞 → 判定，一条命令 |

**⇒ 下一个动作只有一个**：**启动 5GC（或告诉我它现在跑在哪台机器）**，然后把手机摆到 B210 旁边；接入腿我可以直接跑（无需 sudo）。

### 13.1 ★★★ 2026-11-28 17:05 更新：核心网已启动；**`nc` 看不到 SCTP**（我的预检曾有假阴性）

| 项 | 结果 |
|---|---|
| 211 上的 5GC | ★ **已在跑**：`open5gs-{nrf,scp,ausf,bsf,nssf,pcf,udm,udr,smf,upf,amf}` 全在；`ss -ln --sctp` 显示 **`192.168.31.211:38412 LISTEN`**；GTP-U `192.168.31.211:2152` 也在 |
| 配置里的 AMF | `configs/pinned_mcs04.yml`：`addrs: 192.168.31.211`、`port: 38412`、`bind_addrs: 192.168.31.204` = 本机 ⇒ ★ **不需要改** |
| ★ 我的检查方法错了 | `nc -z 38412` 探的是 **TCP**，**SCTP 监听它永远看不见** ⇒ 17:00 前那条"AMF 不在线"的判断在 SCTP 上是**假阴性**。已改为：从配置读 AMF 地址 → 用 `~/.ssh/config` 里对应别名（这里 `nuc5GC`）去 `ss -ln --sctp` 查 → 查不到才退回 TCP → 再不行只**警告**不拦截（NGAP 的权威是 gnb 自己的 `NG Setup`） |
| ★ `sudo` 是**功能必需**，不是习惯 | `gnb` 以普通用户跑时，**小区能起来、射频零丢块**（§12 的 bench 就是如此），但 **NGAP over SCTP 连不上 AMF** ⇒ 接入不可能。脚本已固定走 `sudo`，并在头里写明原因 |

**⇒ 现在的接入腿就是一条命令（需要一次 sudo 密码）**：

```bash
cd ~/dev/ocudu
doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh CeB36_e120d 120
```

预检已在本机验证通过：`射频 OK / AMF 38412 OK（SCTP，nuc5GC 自报监听）/ bundle buckets=7 / load`。

### 13.2 ★★ 2026-11-28 17:00 第一次实跑：脚本在 macOS 自带的 bash 3.2 上解析错了（已修）

**现象**（用户执行）：预检四项全绿，然后
```
fly_attach_verify.sh: line 96: LOG?: unbound variable
```

**原因**：★ **macOS 自带的 `/bin/bash` 是 3.2**，`echo "…日志 $LOG）"` 里 `$LOG` 紧跟**全角右括号**，解析器把多字节字符的字节并进了变量名 ⇒ 变量名成了 `LOG?`（错误信息里那个 `?` 就是被吞掉的字节）⇒ `set -u` 立刻报"未定义"。
**修法**：凡 `$VAR` 后面紧跟非 ASCII 字符的一律写成 `${VAR}`（全脚本扫了一遍，命中 1 处）。

**同一轮测试里另外修的两处**（都是"判定会骗人"的类型）：
1. ★ 配置文件日志 `/tmp/gnb.log` 的**旧内容会残留**：`receiver=ai` 那一行是上一腿的，却被追加进本腿日志 ⇒ 判定会把一个**没跑过的 AI 臂**报成 ACTIVE。现在起飞前 `rm -f /tmp/gnb.log`。
2. ★ 到点计时的子 shell 在 gnb **提前退出**时不会被回收 ⇒ 脚本会一直 `wait` 到倒计时结束（看起来像卡死）。现在主流程结束后 `kill` 计时器。

**⇒ 教训（值得进 runbook）**：这批脚本里有中文输出，**`$VAR` 必须加花括号**——macOS bash 3.2 会把后面的多字节字符当变量名的一部分，而 `bash -n` **查不出来**（这是运行时解析）。

## 14. ★★★★★★ 第一次实链接入腿（2026-11-28 17:00）：**接入成功，但被我们自己的 3 s PDU 会话门释放**；另有射频实时性故障

**证据源**：`~/ai_depth3/flight_logs/attach_CeB36_e120d_20261128_170017/`（console）＋ 同次 `/tmp/gnb.log`（INFO，含 NGAP/RRC/MAC/PHY/RF）。

### 14.1 事实链（逐行可查）

| 时刻 | 事件 |
|---|---|
| 17:01:50.3 | ★ 开始出现 `[RF][W] Real-time failure in RF: late/underflow`（一直到进程被杀，共 **7 703** 次） |
| 17:01:55.70 | `CCCH UL rrcSetupRequest` → `RRC Setup Procedure started` → `CCCH DL rrcSetup` |
| 17:01:55.77 | `DCCH UL rrcSetupComplete` → `InitialUEMessage` → **RRC Setup 成功** |
| 17:01:56.5 | NAS：`Registration complete` ＋ `Configuration update command`（AMF 侧同样记录） |
| 17:01:58.77 | ★★ **`[NGAP] UE did not request a PDU session after 3000ms. Requesting UE release`** → `Tx PDU: UEContextReleaseRequest` → AMF `UEContextReleaseCommand` → `rrcRelease` |
| 17:01:58.9 | DU：`UE Delete` → `UEContextReleaseComplete`；AMF：gNB-UEs 归 0 |

**⇒ 结论一：手机确实接入了（RRC ＋ NAS 注册都完成），释放是 gNB 主动发起的，原因是我们自己的 `cu_cp.request_pdu_session_timeout`（默认 3 s）在等 PDU 会话（数据承载）**。
**⇒ 不是模型、不是射频把手机踢掉的。** 手机只要在 3 s 内发起 PDU 会话（蜂窝数据开、APN 对）就不会被释放；也可以把门放宽（`--request_pdu_session_timeout <秒>`，YAML `cu_cp.request_pdu_session_timeout`）。

### 14.2 ★★★ 同一腿里真正需要修的两件事

**(a) 射频实时性：7 703 次 `late/underflow`，还有 `[PHY][E] The modulator is busy.`**
- 空闲 bench（16:43，无 UE、无 AI 前向）**0 次**；这条腿从 UE 出现前后开始、**一直持续到结束**（UE 释放后仍在每秒 200–400 次）。
- 当天本机 `Chrome ~24% / WindowServer ~22% / Spotlight ~11%`，load 4.9。
- ⇒ **有这些行的腿不能用于时延/吞吐结论**（§11 的 G1 判据也要求 RF 干净）。

**(b) ★★★ 分配尺寸把模型推进了最慢的桶**（这条是部署的关键发现）

| 该腿的 PUSCH 授权 | 次数 | 需要的桶 | 单次预测（隔离实测） |
|---|---|---|---|
| 2–3 PRB | 8 | **180** | ~0.5 ms（实链 944 µs） |
| 25 PRB | 1 | 396 | ~2.0 ms |
| **35 PRB** | 6 | **540** | ~2.0–2.8 ms |
| **48 PRB** | 3 | **612** | ~2.9 ms |

- 该腿最长的一次 **35 PRB** PUSCH：`t=3302.9us`（`crc=OK iter=1.0`）—— ★ **模型在实链上确实在算，而且把它解出来了**；
- ★ 为什么小消息会有 35–48 PRB 的授权：**`pinned_mcs04` 把 MCS 钉在 4（QPSK，tcr≈0.30）** ⇒ 调度器只能用"宽分配"换低码率 ⇒ 正好落在模型最贵的宽度上；
- ⇒ ★★ **AI 臂的正确工况是"高 MCS ＋ 窄分配"**（256QAM 下 3 字节 SRB1 只要 1–2 PRB），而这**同时**是模型增益最大的区域（256QAM +305 496，QPSK 只有 +112 496）。

### 14.3 同轮的 AI 臂读数（说明它没拖后腿）

| 项 | 值 |
|---|---|
| 绑定 | `AI ARM ACTIVE over 4 model(s)`；桶 108–540；`contract=modcond`；`reception FAILED = 0` |
| 4 PRB 接收（180 桶） | `forward 917–1022 µs`（plane 61–117 ＋ predict 846–944 ＋ deliver 123–133） |
| 隔离口径对比 | 同一桶隔离 p50 **519–538 µs** ⇒ 实链 **×1.6–1.8**（与 2026-11-06 那条 b18 腿的 ×2 一致） |

### 14.4 已修的工具问题（都发生在这一轮）

| 问题 | 现象 | 修法 |
|---|---|---|
| ★ macOS bash 3.2 把 `$LOG）` 的字节并入变量名 | `line 96: LOG?: unbound variable`，起飞前退出 | 全脚本 `${VAR}` |
| ★ `pkill -f "build/apps/gnb/gnb -c"` 命中了 `script` 包装器 | gnb 被 SIGHUP 杀掉 ⇒ 无 `Stopping...`、无 `[ul_pipeline]`、无连续性、终端被 pty 弄乱、判定排版错乱 | 去掉 `script`/pty；**后台起 ＋ 按 pid 发 SIGINT**；`stty sane` |
| `/tmp/gnb.log` 归 root，下一腿 `rm` 失败且可能残留旧内容 | 判定读到上一腿的 `receiver=ai` | ★ 改用 `--log.filename <腿目录>/gnb_full.log`，INFO 日志直接进腿目录 |
| 判定看不到 RRC/MAC/RF | 只能看到 console 的 stderr 行 | `flight_verdict.py` 支持多日志；新增 **RF 实时性 / HARQ 放弃 / SRB1 重传 / PUSCH crc / 谁释放了 UE** |

### 14.5 下一条腿的建议顺序

| 序 | 动作 | 命令 |
|---|---|---|
| ★ 1 | **手机端**：蜂窝数据打开、APN 设成 open5gs 的 DNN；确认 3 s 内发起 PDU 会话 | — |
| ★ 2 | **配置**：换**交付配置**（AMC 自由）或把 MCS 钉到 20+；注意 `pinned_mcs{02,12..27}` **仍写着旧网段 `192.168.0.106/.231`**（只有 00/04/06/08/10 与交付配置是 `.211/.204`） | `…/fly_attach_verify.sh CeB36_e120d 180 configs/gnb_rf_b200_tdd_n78_20mhz.yml` |
| ★ 3 | **清机器**：退出 Chrome／停止 Spotlight，load ≤ 2；先看 `Real-time failures` 是否归零 | 脚本预检会打印 load 与前三大 CPU |
| 4 | 需要归因射频时，跑一条**经典臂**对照 | `ARM=classic …/fly_attach_verify.sh CeB36_e120d 180 configs/gnb_rf_b200_tdd_n78_20mhz.yml` |
| 5 | 放宽 PDU 会话门（可选） | 前面加 `PDU_TIMEOUT=10` |

### 14.6 第二次实跑：`★ gnb 没起来` 是**工具误报**（2026-11-28 17:15 已修）

**现象**：脚本报 `★ gnb 没起来`，但日志尾其实是一份**完整的退出报告**（`contract MET (0 of 5 checks applicable)`、`[metal_stats] …`、`dft radio inputs: 0 transform(s)`）。

**两边都成立的原因**：gnb **确实起来了，也被关掉了，但一帧都没处理**。
1. 脚本只等 **8 s** 就查 pid；
2. ★ `sudo` 先要密码、拿到密码后才 `exec` gnb，而 **B200 的 FPGA 镜像加载要 10–15 s**；
3. 8 s 时 pid 还不存在 ⇒ 脚本判定"没起来"并 **kill 了 sudo 包装进程** ⇒ gnb 收到信号，走正常退出路径，打印"零工作量"的 summary。

**修法（三处）**：
1. ★ **前台先 `sudo -v`**：后台任务读控制终端会吃 SIGTTIN，后台 `sudo` 本来就不该负责要密码（以前能不能跑起来，取决于操作员 5 分钟内是否用过 sudo）；
2. ★ 查 pid 改成**轮询 60 s**（先 `pgrep -P <sudo 包装 pid>`，再退回按 argv 匹配），**从 pid 出现才开始计时**；
3. gnb 若自行退出，不再补发 SIGINT，直接判定。

**⇒ 重跑同一条命令即可**（现在会先说"下面先要一次 sudo 密码"）：

```bash
cd ~/dev/ocudu
sudo -v            # 可选：先把密码缓存好
PDU_TIMEOUT=10 doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh \
  CeB36_e120d 180 configs/gnb_rf_b200_tdd_n78_20mhz.yml
```

### 14.7 第三次实跑（17:16）"连不上 AMF"的真相：**命令行被 CLI11 拒绝，根本没跑**

日志尾部只有一份零工作量的退出报告，头部的真正原因是一行：

```
The following arguments were not expected: 10 --request_pdu_session_timeout
Run with --help for more information.
```

★ **是我给错了写法**：这个选项注册在 **CU-CP 子命令**下，CLI 路径是 **`--cu_cp.request_pdu_session_timeout`**，不是 `--request_pdu_session_timeout`。
**验证方法**（不必起射频）：故意给一个非数字值 ——
- `--cu_cp.request_pdu_session_timeout abc` → `Could not convert: --request_pdu_session_timeout = abc`（**选项存在**）
- `--request_pdu_session_timeout abc` → `The following arguments were not expected`（**选项不存在**）

**⇒ 已修**：脚本改用正确路径；并新增**快速失败** —— 日志一出现 `arguments were not expected` 就打印"命令行被拒（不是 AMF 的问题）"并退出，不再白等 60 秒轮询。
**⇒ 重跑**（`PDU_TIMEOUT` 可带可不带，它只放宽"注册后等 PDU 会话"的时间窗）：

```bash
cd ~/dev/ocudu
PDU_TIMEOUT=10 doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh \
  CeB36_e120d 180 configs/gnb_rf_b200_tdd_n78_20mhz.yml
```

## 15. ★★★★★★ 17:20 那条腿的解剖：**数据面不通的根因是 AI 臂在宽分配上太慢**（实测闭环）

**臂**：`npu` ＋ `CeB36_e120d`（7 桶）＋ fallback，交付配置（AMC），180 s。日志：`~/ai_depth3/flight_logs/attach_CeB36_e120d_20261128_172011/`。

### 15.1 四条读数

| 指标 | 值 | 含义 |
|---|---|---|
| ★ **`[ul_equalization_demod]`** | **p50 3 267 µs / p99 7 189 µs** | 臂自身就吃掉 **6.5 个 slot**（30 kHz） |
| ★ **`[ul_pipeline]`** | **p50 3 608 µs / p99 7 752 µs** | 整条 UL 比 slot 长 **7–15 倍** |
| ★ **PUSCH crc** | **OK 92 / 失败 488**（84% 失败） | UL 基本不可用 |
| ★ **RF `Real-time failure`** | **118 337**（late 80 030 / underflow 38 307） | 射频实时环持续超时 |
| UE 侧 | **12 次 attach / 15 次 release**；PDU 会话建立成功 2 次（拿到 IP） | 断连-重连循环 |
| ★ `[depth3-timing]` | **SLOW STATE 出现**；predict 108:1 005 µs、180:961、396:2 725、540:3 615、612:3 579 | 模型被 Core ML 放到慢路径，且宽桶极贵 |

**桶分配现实**：180×145、**396×77、468×17、540×88、612×97**（合计 279/500 = **56% 落在 ≥396 的宽桶**）⇒ 加权平均 predict ≈ **2.4 ms**，与 EQ-demod 3.27 ms 对得上（其余约 0.5–0.9 ms 是组平面/量化/投递）。**⇒ 慢的绝大部分是模型本身，不是框架。**

### 15.2 因果链（日志原文可查）

```
[MAC]   ue=0: RLF detected. Cause: 100 consecutive HARQ-ACK KOs          （9 次）
[MAC]   ue=0: RLF detected. Cause: 100 consecutive undecoded CSIs        （5 次）
[DU-MNG] ue=0 rnti=0x4603: RLF detected with cause "RLC max ReTxs reached", from SRB1
         → Timer of 1000 msec to release UE started → RLF timer expired → Requesting a UE release
[NGAP]  Tx PDU: UEContextReleaseRequest → AMF UEContextReleaseCommand → rrcRelease
```

★ **HARQ-ACK KO 与 CSI 解不出，都是"UL 没被及时处理"**：PUCCH/CSI 的接收和 PUSCH 走同一条 `npu` 臂；一次 3.3 ms 的接收意味着 PUCCH 的时隙早已过去 ⇒ ACK 永远不到 ⇒ DL HARQ 重传 4 次用尽 ⇒ **RLF** ⇒ 释放 ⇒ 手机重连。**PDU 会话能建立（拿到 IP）但一传数据就死**，正是这个循环。

**⇒ 结论：这不是"RF 抖动"或"核心网"的问题，是 ★ G1（时延门）在实链上被判负了**——正是 §11.3 里"落进没有先例的区间"的那件事，现在有了答案：**在当前配置（AMC ⇒ 出现 396/468/540/612 宽分配）下，36 块模型无法维持链路。**

### 15.3 下一步（按顺序，且都要"窄分配"）

| 序 | 动作 | 目的 |
|---|---|---|
| ★ 1 | **`ARM=classic` 跑一条**（同一配置、同一位置） | ★ 先把"台子 + 核心网 + 手机数据面"证清白：经典臂能 ping 通，就说明问题 100% 在 AI 臂的时延 |
| ★ 2 | AI 臂，但**只发窄分配流量**：手机上关后台数据、只用 ping（勿 iperf/测速）；`LOG_LEVEL=warning`；退出 Chrome | 期望落在 108/180 桶（0.5–1 ms） |
| ★ 3 | 若窄分配仍掉线（180 桶实测 961 µs ≈ 2 slot） | 说明 36 块模型在 30 kHz 上**连 2 slot 都吃不下** ⇒ 换 b18（Ce/CeBd，+2.2–3.2%）或走 §15.4 |
| ★ 4 | **部署级修法**：给 AI 臂加"宽带上限"（分配 > K PRB 时走经典链，`FALLBACK=estimator`） | 这是唯一能让"AI 只管窄分配、大流量交给 CPU"落地的方案（代码改动 ~半天） |

```bash
# 1) 经典臂（先证台子）
ARM=classic LOG_LEVEL=warning doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh \
  CeB36_e120d 120 configs/gnb_rf_b200_tdd_n78_20mhz.yml
# 2) AI 臂（窄分配流量 + 轻日志）
LOG_LEVEL=warning doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh \
  CeB36_e120d 120 configs/gnb_rf_b200_tdd_n78_20mhz.yml
```

## 16. ★★★★★ 2026-11-28 17:30 **对照实验：不指定模型，用构建默认（E4）飞一次** —— 把"台子/链路"与"模型"分开

**动机**：CeB36_e120d 在宽分配上 2.9–3.6 ms，UL 被打穿（§15）。**"以前是通的"用的就是构建默认模型**（`OCUDU_DEPTH3_DEFAULT_MODEL` = `depth3_e4_real_1023/d3_e4.mlmodelc`，自带 **8 个桶**）。所以先跑它：如果它能通，问题就锁定在"新模型的时延"，而不是台子/核心网/手机。

**本机隔离实测（`cpu_ne`，40 次，3 次取最好）**：

| 桶宽 | 108 | 180 | 396 | 612 |
|---|---|---|---|---|
| **E4（默认）** | **124 µs** | **155 µs** | **233 µs** | **377 µs** |
| CeB36_e120d（目标） | 393 | 519 | 2 053 | 2 974 |
| 倍数 | 3.2× | 3.3× | 8.8× | **7.9×** |

⇒ ★ **E4 连全带 612 都只要 377 µs（0.75 个 slot）**，而 CeB36 要 2 974 µs（6 个 slot）⇒ 前者应当能把链路撑住，后者正是 §15 里 84% UL 失败的原因。**这条对照腿的意义就是把"模型"这一项单独拿掉。**

**命令**（`default` = 不传 `--pusch_receiver_model`，让 app 绑它自己的默认模型）：

```bash
cd ~/dev/ocudu
LOG_LEVEL=warning doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh \
  default 120 configs/gnb_rf_b200_tdd_n78_20mhz.yml
```

预检已在本机通过：`射频 OK / AMF 38412 OK（SCTP）/ 构建默认 depth3_e4_real_1023（8 桶）/ load`
**判读**：
- 预检报 load 时若 >2，先退 Chrome/Spotlight（17:20 那条腿 load 高、RF 从第 2 秒就 underflow）；
- 判定里 `[3] AI 臂` 会显示 `AI ARM ACTIVE` 与绑定的**路径**（应为 `…/ai_assets/depth3_e4_real_1023/d3_e4.mlmodelc`），桶 108–540；
- `[4]` 的 `各宽度 predict` 应在 **120–400 µs** 量级 ⇒ `[ul_equalization_demod]` p50 应在 **0.5–0.8 ms**；
- 若这样都掉线 ⇒ 问题在台子/射频实时性（`Real-time failures`），不在模型。

## 17. ★★★★★★ 17:31 对照腿结论：**默认模型（E4）通了，链路、核心网、手机全部清白**；CeB36 不通的原因被定量锁定

同一天、同一台机、同一配置（`gnb_rf_b200_tdd_n78_20mhz.yml`）、同一条命令，只换模型：

| 指标 | ★ **E4（构建默认，8 桶）** | **CeB36_e120d（7 桶）** |
|---|---|---|
| 时长 / 日志级别 | 120 s / warning | 180 s / info |
| 射频连续性 | **0 gaps / 3 202 455 blocks** | 0 gaps / 4 901 497 blocks |
| ★ `Real-time failure in RF` | **564（0.018% 的块）** | **118 337（2.41%）** |
| DL/UL HARQ 放弃 | **0 / 0** | 545 / 74 |
| ★ RLF / UE 释放 | **0 / 0** | 14 / 15 |
| UL 数据 | **127 477 B / 243 CRC-OK hops** | 25 839 B / 92 hops |
| ★ PUSCH 成功率 | 未记录（INFO 关） | **15.9%（92 / 580）** |
| ★ `[ul_equalization_demod]` | **p50 408 µs / p99 2 515 µs** | **p50 3 267 / p99 7 189 µs** |
| `[ul_pipeline]` | **p50 995 µs / p99 3 104 µs** | p50 3 608 / p99 7 752 µs |
| 180 桶 predict | **314 µs** | 961 µs（＋ SLOW STATE） |
| 判定 | ★ **RADIO OK / ATTACH OK / DATA OK** | RADIO FAIL / ATTACH OK / **DATA NO** |

**⇒ 三条结论**：
1. ★★★ **台子、核心网（open5gs 11 个 NF）、手机、射频全部清白**：E4 腿 120 秒里 **243 个 TB 全部解出**、**0 RLF、0 释放**、RF 故障只占 0.018%。"ping 能跑"是有数据支撑的；
2. ★★★ **CeB36 不通是模型时延造成的**，而且**元凶是宽桶**：它的 396/468/540/612 要 2.7–4.2 ms，而窄桶 180 只需 961 µs。E4 整条 pipeline（995 µs）都能活，说明 **~1 ms 级是这条链路的可承受区**，2.7 ms 以上才致命；
3. ★★ **所以 CeB36 仍有救**：只要让它的接收**只落在 108/180 桶**（≤7 PRB），predict 961 µs 与 E4 整条管线同量级 ⇒ 大概率能通。这正好是"宽带上限 → 经典链"那条策略。

**本条同时修好了判定的两个假阴性**（都在这轮暴露）：
- `RADIO` 原来要求 `Real-time failures == 0`，把 E4 这条**能用的腿**判 FAIL ⇒ 改为：连续性 OK ＋ 无 HARQ 放弃 ＋ 无 UE 释放 ＋ 故障率 < 0.1% 的块；
- `ATTACH/DATA` 原来依赖 MAC UE 表（INFO 关时不存在）⇒ 改为：`RRC Setup` 次数 或 UL 数据 或 PUSCH 成功率 ≥80%（CeB36 的 15.9% 因此判 DATA NO，与"数据没通"一致）。

**⇒ 下一步（有数据支撑的排序）**：
1. ★ **做"宽带上限 → 经典链"**（分配 > K PRB 走经典接收，`FALLBACK=estimator`）：CeB36 只在 ≤7 PRB 用 AI（180 桶，961 µs），其余交 CPU；E4 腿证明 1 ms 级管线能活；
2. 若不想改代码：**用流量/调度把分配压到 ≤7 PRB** 再飞 CeB36，验证第 3 条结论；
3. 仍不行 ⇒ 换 b18（`CeBd120swa`/`CeB80c`，+2.2–2.6%），它的窄桶更便宜。

## 18. ★★★★★ 2026-11-28 17:50 **更正 §15/§17：把"模型太慢"当成数据面不通的原因，证据不支持**

**用户提醒**：c128 的时延更大（`[ul_equalization_demod]` p50 **5 385 µs**），但手机数据是通的（11-05 那条腿，UL nok 约 39%）。这个反例把"慢 ⇒ 不通"这条因果直接否掉了。回到日志核对，**同一腿内**的数据也否定它：

| 证据 | 读法 |
|---|---|
| ★ CeB36 腿里 **PUSCH OK 的 hop 时间 p50 = 3 371 µs；KO 的 p50 = 3 316 µs** | ★ **成功与失败走的是同样慢的 3.3 ms** ⇒ **时延不是判别量** |
| ★ OK 与 KO 的 SINR 区间重叠（OK 3.6–6.2 dB；KO 4.5–10 dB） | 不是"低 SNR 才失败" |
| ★ OK 69/84 是 QPSK，KO 363/488 也是 QPSK | 不是"高阶调制才失败" |
| ★ KO 的 PRB 分布：3 PRB×82、6 PRB×69、17–22 PRB×~50 | **窄分配也在失败**，不只是宽桶 |
| ★ 失败在时间上是**均匀**的（每 5 s 25–85 个 KO，10–20 个 OK），不是突发 | 不是"某个掉块时段"的数据损坏 |
| ★ 该腿 `Real-time failure` 从 **17:20:18** 开始，**早于第一个 AI 前向（17:20:37）和 UE 接入** | 腿在模型动手之前就已经在丢实时性 ⇒ 主机/日志状态是**并存的第二个原因** |

**⇒ 修正后的结论**：
1. ★ **"CeB36 太慢所以必然不通"是错的**。慢会吃掉 ACK 的时点、加重 RLF 的暴露，但**不是解码失败的直接原因**（同腿内 OK/KO 的 hop 时间相同）；
2. ★ 真正需要解释的是：**为什么同一臂、同一模型，16% 成功、84% 失败**，而 20 分钟后 **E4 腿 243/243 全成功**。两个腿的**条件并不相同**：
   - 日志级别：CeB36 = `info`（13.9 MB / 3 min）vs E4 = `warning`（54 KB）；
   - RF 故障：118 337 vs 564；
   - 负载与分配构成也不同（CeB36 腿出现 396/468/540/612 的宽分配）。
   ⇒ **目前还不能把 84% 失败归给模型**——这是一次**混杂了主机状态**的测量；
3. ★ **下一步只做一件事**：**用与 E4 完全相同的条件再飞一次 CeB36**（`LOG_LEVEL=warning`、同一配置、机器安静）。不改任何代码：

```bash
cd ~/dev/ocudu
LOG_LEVEL=warning doc_chinese/depth3_llr_ai/work_tmp/training/fly_attach_verify.sh \
  CeB36_e120d 120 configs/gnb_rf_b200_tdd_n78_20mhz.yml
```

**判读**：
- 若 RF 故障回到几百、PUSCH 成功率与 E4 同档 ⇒ **模型无罪，前一次是主机/日志artifact**，CeB36 可以部署（延时大但数据通，和 c128 一样）；
- 若 RF 干净而 PUSCH 仍大面积失败 ⇒ **才是模型/链路适配问题**（那时再查 `s_llr`/噪声映射/平面装配），并且与"慢"无关。

## 19. ★★★★★★ 17:52 干净复飞：**CeB36_e120d 通了**（RADIO/ATTACH/DATA 全 OK）—— 上一腿的 84% 失败确认是主机/日志 artifact

**条件与 17:20 那条腿唯一的差别**：`LOG_LEVEL=warning`（其它同：模型、配置、机器、命令）。

| 指标 | E4（17:31，warning） | CeB36 **脏腿**（17:20，info） | ★ CeB36 **干净腿**（17:52，warning） |
|---|---|---|---|
| `Real-time failure` | 564 | **118 337** | ★ **108** |
| DL/UL HARQ 放弃 | 0 / 0 | 545 / 74 | **0 / 0** |
| RLF / UE 释放 | 0 / 0 | 14 / 15 | ★ **0 / 0** |
| UL 数据 | 127 477 B / 243 hops | 25 839 B / 92 hops | ★ **108 560 B / 205 hops** |
| `[ul_equalization_demod]` p50 | 408 µs | 3 267 µs | ★ **647 µs**（p99 3 714，max 4 540） |
| `[ul_pipeline]` p50 | 995 µs | 3 608 µs | ★ **1 221 µs**（p99 4 258） |
| 180 桶 predict | 314 µs | 961 µs（＋SLOW STATE） | ★ **673 µs**（无 SLOW STATE） |
| 判定 | OK / OK / OK | FAIL / OK / **NO** | ★ **OK / OK / OK** |

**⇒ 三条结论**：
1. ★★★ **模型可以部署**：CeB36 在干净主机上跑通 ping，205 个 TB 全部解出，**0 RLF、0 释放、0 HARQ 放弃**；EQ-demod **647 µs**（隔离 519 µs 的 ×1.25），管线 **1.22 ms**。这与 c128（5.4 ms 也通）一致 ⇒ **时延只影响余量，不决定通断**；
2. ★★★ **17:20 那条"84% 失败"是主机/日志 artifact**：`info` 级日志 3 分钟写 13.9 MB（≈860 行/秒）＋ 机器高负载 ⇒ RF 从第 2 秒就 underflow/late（118 337 次）⇒ 样本损坏 ⇒ 大面积 CRC 失败 ⇒ PUSCH 3.3 ms（含重传/退避）＋ ACK 丢失 ⇒ RLF。**同一条腿里 OK 与 KO 的 hop 时间相同（3 371 vs 3 316 µs）**，本来就说明"慢"不是判别量（§18）；
3. ★★ **飞行纪律**：以后**默认 `LOG_LEVEL=warning`**；`info` 只用于事后调试，而且**不要在 radio 上开着 INFO 做时延/吞吐判断**（它自己就能把实时性打穿）。

**⇒ 部署判据的新状态**（取代 §11.3 的 G1 判负）：

| 门 | 旧结论 | ★ 现在 |
|---|---|---|
| G0 等价（桶＝同一网络） | 通过（权重逐字节相同；SE 使桶为近似，0.2 LSB RMS） | 不变 |
| G1 时延 | **未通过**（w180 519–538 µs > 400） | ★ **实链已证明可通**（EQ-demod 647 µs、管线 1.22 ms、0 RLF）；但**宽分配（540/612）仍有 2.9–3.6 ms 的风险**，属"可用但余量小" |
| G2 接入 | 未飞 | ★ **通过**（本次 ping 通，205/205） |

**⇒ 下一步（不改代码）**：同一干净条件下跑三条腿做**性能**比较（不是可用性）：
1. `ARM=classic`（经典基线）；
2. E4（默认模型）；
3. CeB36（目标模型）；
每条 3–5 分钟持续 ping/小流量，比较 **UL nok 率、`[ul_pipeline]` p50/p99、桶分配构成**。只有这一步能回答"部署哪个模型"——可用性已经不再是问题。

**§19 附注（判定工具的两处显示假象，已修）**
1. `AMF / N2 not connected` 是**合并逻辑的 bug**：`amf` 是派生字段，而 `parse("")` 先把它填成默认值 "not connected"（真值），合并时的 `or` 就再也替换不掉。腿自己的日志里明明有 `N2: Connection to AMF on 192.168.31.211:38412 completed`。已改为**合并后重算** `amf`。
2. `桶分配 180x1 / predict 180:673` **只是第 1 个统计窗口**：接收机在**第 1 次**前向后打印一次，之后**每 500 次**才再打印（`TIMING_REPORT_EVERY = 500`）。本条腿只有 205 个接收 ⇒ 只有窗口 #1。**要看全腿的桶分布，得跑 ≥500 个接收**（按 ping ≈1.7 个/秒，约 5 分钟）。聚合口径应读 `[ul_equalization_demod]`（n=205）：p50 647 µs、p99 3 714 µs、max 4 540 µs ⇒ 大多数落窄桶，少数慢样本把尾部抬起来，但没有触发 RLF。

## 20. ★★★★★★★ 18:00 五分钟持续流量腿：**部署形态成立**（CeB36_e120d ＋ 30 kHz ＋ 1 ANE）

**命令**：`LOG_LEVEL=warning … fly_attach_verify.sh CeB36_e120d 300 configs/gnb_rf_b200_tdd_n78_20mhz.yml`
**日志**：`~/ai_depth3/flight_logs/attach_CeB36_e120d_20261128_175907/`

### 20.1 判定与读数

| 项 | 值 |
|---|---|
| 判定 | ★ **RADIO OK / ATTACH OK / DATA OK / AI ARM ACTIVE** |
| 时长 / 块 | 300 s / **12 008 395 blocks，0 gaps** |
| ★ 交付 | **21 776 175 B（≈71 KiB/s ＝ 581 kbit/s）／32 874 个 CRC-OK TB**（≈110 TB/s） |
| ★ `Real-time failure` | **857（0.0071% 的块）** |
| DL/UL HARQ 放弃 / SRB1 重传 / RLF / 释放 | **0 / 0 / 0 / 0 / 0** |
| ★ `[ul_pipeline]` | **p50 1 428 µs / p99 3 541 µs**（n=32 864） |
| ★ `[ul_equalization_demod]` | **p50 831 µs / p99 2 940 µs**（n=32 703） |
| `SLOW STATE` | 出现（Core ML 慢路径），但 p50 仍 831 µs |

### 20.2 ★★ 桶分配（68 个统计窗口聚合，这是第一次看到真实构成）

| 桶 | 次数 | 占比 | 该宽度的 predict（多窗口取大） |
|---|---|---|---|
| **108** | 9 498 | **28.9%** | 620 µs（有一窗 3 288 µs 的慢样本） |
| **180** | **21 456** | ★ **65.3%** | **746–806 µs** |
| 252 | 1 640 | 5.0% | 1 098–1 158 µs |
| 324 | 234 | 0.7% | 1 188–1 216 µs |
| 396 | 29 | 0.09% | 2 346–2 385 µs |
| 468 | 20 | 0.06% | 4 006–4 119 µs |
| 540 | 2 | 0.006% | 3 009 µs |
| 612 | 121 | 0.37% | 3 259–3 281 µs |

**⇒ ★★★ 94.2% 的接收落在 108/180（≤7 PRB），宽桶合计只有 0.53%。** 这正是"AI 只在小分配上工作"的理想分布：持续流量下调度器并没有把所有接收推到全带。

### 20.3 部署结论（可以定稿了）

| 门 | 结论 |
|---|---|
| G0（桶＝同一网络） | ✅ 权重与出厂逐字节相同；SE 使桶为近似（0.2 LSB RMS / 500 单元 −2 个） |
| G1（时延） | ✅ **实链通过**：p50 831 µs、p99 2.94 ms，5 分钟零 RLF |
| G2（接入与数据） | ✅ **通过**：32 874 个 TB、21.8 MB、零重传耗尽 |

**⇒ 可部署表述（带条件）**：
> ★ `CeB36_e120d` 在 **n78 / 20 MHz / 30 kHz / 单 ANE（M4 Pro）** 上可部署：**AI 接收机承担 94% 的接收（≤7 PRB），EQ-demod p50 831 µs、UL 管线 p50 1.43 ms，5 分钟持续流量零 RLF、零 HARQ 放弃**。
> ⚠ 已知限制：① 宽分配（396–612，约 0.5% 的接收）单次 2.3–4.1 ms，属"可用但余量小"；② **飞前必须 `LOG_LEVEL=warning`**（`info` 自己就能把实时性打穿，见 §19）；③ Core ML 偶发 `SLOW STATE`（重飞一次通常回到快路径）。

**⇒ 仍缺的一件事**：与经典链的 **nok 率/吞吐 A/B**（需要 PUSCH 行，即短时间 INFO 或 MAC 表）。数据面可用性已经证明，剩下的是"部署哪个模型更值"的定量问题（尺子/全腿已有离线答案，实链只需确认无损）。
