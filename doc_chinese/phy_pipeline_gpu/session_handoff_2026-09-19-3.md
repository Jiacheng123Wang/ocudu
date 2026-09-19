# session_handoff_2026-09-19-3 — 批次 5a 进行中：设备侧 rsrp 归约（探针未通过）

> **这是给"新会话"用的现状快照，不是技术文档。**
> 判据、代码地图、技术事实、口径、批次状态、踩过的坑、否定结果台账——
> **全部在 `gpu_phy_pipeline_design_and_implementation.md`（常驻设计文档）**。**开工先读它。**
>
> 本文只回答：**现在到哪了**、**下一步做什么**。
>
> 命名：`session_handoff_<YYYY-MM-DD>-<序号>.md`，新会话永远读**序号最大的那一份**；
> 旧的**保留不删**（它们记录了当时的判断，包括后来被推翻的那些）。

---

## 0. 一句话

分支 `apple-silicon`，HEAD = **`94df4144a2`**，工作树干净。

**⛔ 当前状态：批次 5a 进行中，对照探针未通过 —— 设备归约写 0，但参数已确认正确送达。**

**不需要你做任何决定**，也没有待跑的 OTA。**下一轮直接按 §3 的步骤继续写代码。**

**⚠ gnb 戳记是 `0e4a24ce57`，落后于 HEAD** —— 本轮改的是纯新增（K5 设备阶段 + 探针），
没有任何发布路径依赖它，所以不影响；**但下次要跑 OTA 前必须 `touch build/hashes.h && cmake --build build --target gnb`。**

---

## 1. 目标（不变的最终判据）

> **`gpu` — 融合车道。整条 IQ → LLR 链跑在一条设备侧流水线里，只有两次 host↔device 数据穿越
> （IQ 上传、LLR 下载）。LDPC 不属于车道。** —— `include/ocudu/phy/phy_pipeline_mode.h`

**判据**：数据流进入 GPU 后，CPU 只能**提交命令缓冲**、**在出口等待**。其余任何逐跳的宿主计算或搬运都算缺陷。

---

## 2. 当前进度（空中实测）

| 项 | 读/跳 | 写/跳 | 说明 |
|---|---|---|---|
| 批次 0 之前 | ≈8.1 | **36.00** | 起点 |
| **现在（HEAD `94df4144a2`）** | **1.33** | **0.27** | 腿 `ota-b4_0919_0827`，RTF 1，gaps 0，契约 7/8 |
| `OCUDU_CE_HOST_GRID=0` 时（离线推算）| ≈0.33 | 0.27 | **这就是 5a 要拿下的** |

**申报模块：`demapper, dft, equalizer, channel_estimator`（四个全到 ⇒ 数字是上界）。**

### 已完成

| 批次 | 提交 | 内容 |
|---|---|---|
| 0 / 1 | `929fc4a3d5` / `76fc488385` | 均衡器建表缓存；`gpu_epochs` 只上传一次（写 36→2.65）|
| 2-侦察 | `965b0f0951` | `OCUDU_CE_HOST_GRID` 探针：证明回读网格**只喂上报量** |
| 3a | `0eeb1c1941` | 尾组 pad 改**设备**清（空中 **写 1.64→1.31**，95% CI 不重叠）|
| 3b.1 | `fa2628e018` | CFO 进位搬设备（读 3.10→**2.10**、写 1.65→**0.65**）|
| **4** | `0e4a24ce57` | **审 `demapper`**：零穿越 + **申报**；把它的 staging 兜底接进计数器 |

### 进行中

| 步 | 提交 | 状态 |
|---|---|---|
| **5a** rsrp + noise_variance 搬设备 | `73ec94b08e` `74620389ef` `205eb117f8` `29678aefcd` `94df4144a2` | kernel ✅ / 引擎阶段 ✅ / 宿主接线 ✅ / **探针未通过** |
| 5b TA 搬设备 | — | 未开始（**最难**：236 行 IDFT + 峰拟合估计器，设备无对应物）|
| 5c 开关 `OCUDU_CE_DEV_STATS` | 部分（门已加，默认 1）| 待 5a 通过 |
| 5d γ 落地 | — | 等 5a/5b |

---

## 3. ⛔ 下一步：把 5a 的探针做通过（照做，不要重新调查）

### 3.1 当前证据（`OCUDU_CE_RSRP_CHECK=1` 跑 `ul_chain_replay`）

```
[comb]        layer=0 npat=1 size=12 bits=0x555 syms=3 npt=3   ← 宿主 comb 正确
[rsrp_params] ... dmrs_sym_bits=0x884 pilot0=0x555 pilot1=0    ← 参数正确送达 kernel
[k5]          entered, dst=0x863f30010 n_blk=1                 ← encode 块确实进入
[rsrp_check]  dev 0.000000e+00 host 5.435613990e-01 | host_nre 72 dev_nre 0
```

**宿主侧是对的（72 个 RE），设备侧累加为 0。**

### 3.2 已排除的（不要重查）

* ❌ "kernel 没被 dispatch" —— `[k5] entered` 且早期产出过 `3.325204e-01`
* ❌ "comb 为空" —— `pilot0=0x555`（曾因建在 `device_estimate_offsets()` 的早退路径上而为空，已修）
* ❌ "参数没送达" —— `[rsrp_params]` 显示几何与 `dmrs_sym_bits` 全对
* ❌ "`float2*` 下标" —— 已改成 `device float*` + `out[2*i]`

### 3.3 剩余假设（收窄到一个）

**kernel 自身**。`is_std = (blk * nf_std) < sc_tail_base` 在本例是 `(0*36) < 36 = true`，
入口条件应成立 ⇒ 指向 **`h` 的行索引**：

```c
device const float* hp = h + (sys * p.n_blk + blk) * (2 * p.nout_stride) + 2 * (sym * nf + local_sc);
//   sys / blk / nf / sc0 / 行跨步  需与 K2 实际写的内容一致
```

### 3.4 做法（**按顺序，别跳**）

1. **先让 kernel 返回已知常数**（如 `out = (12345, 1)`）⇒ 确认 dispatch 真的执行；
2. 若常数回来了 ⇒ **逐步收窄读取**：先只读一个已知 RE，再放开到整个 comb；
3. 对齐 **K2 的写入布局**（`ocudu_mmse_apply.metal` 的 `h` 下标）与 kernel 的读取下标；
4. **探针必须通过**：`dev_nre == host_nre`（本例 **72**），然后 `worst rel` 落在浮点重结合允许范围内；
5. **探针通过后才接发布路径**（把设备值接到 rsrp/snr 上报）。

**⚠ 纪律**：**"参数看起来对"不等于"kernel 在按那些参数做事"**。
本轮四次排查里**三次是我的观测手段错了**，不是被测对象错了。**先让观察可信，再下结论。**

### 3.5 修好后的收尾

* 接发布路径（`compute_hop_finish` 的 rsrp 用设备值）+ 一个 `OCUDU_CE_DEV_STATS=0` 的 A/B；
* 然后才是 5b（TA）、5c、5d。

---

## 4. 重要的运行纪律（每次都用）

1. **腿一律用 Ctrl-C（SIGINT）停。** SIGTERM 会**跳过全部收尾统计**，
   跨越计数**事后无法恢复**（腿 `ota-b3a-final` 就这么废掉的）。**坑 23**。
2. **判腿有效性的第一眼**：`radio sample continuity` 与 `host sample assembly` 必须 **0 gaps**；
   报告的 `-- device side` / `-- lane` 两段**必须非空**。
   **采样连续性坏掉的腿，任何数字都不能用来支持或否定代码改动**（坑 21——为此冤枉过两轮代码）。
3. **构建必须显式 `--target`**（`ul_chain_replay` / 单元测试 / `gnb`），否则拿到新旧混合的假结果。
4. **计数必须 `--repeat 20`**，单跳缓存是冷的。
5. **`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 有未提交改动**（用户重配的 PUCCH 段）。
   **动分支前先 `bash wip/backup_worktree.sh <label>`**（坑 22：`reset --hard` 丢过一次，无法找回）。
   该 config 必须带**六个 PUCCH 资源键**，否则校验直接拒绝（超 BWP 50%）。
6. **离线数字是方向与量级的预测，不是空中数值的预测**；不一致时**以空中为准**（§15.4）。

---

## 5. 判据工具（详见设计文档 §6）

```bash
cd /Users/jiachengwang/dev/ocudu
cmake --build build --target ul_chain_replay port_channel_estimator_metal_mmse_unit_test
# 稳态计数
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/x 2>&1 >/dev/null | grep -a crossings -A1
# 27 捕获 A/B（数据判据）
bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh "" "<knob>=1"
# 5a 的对照探针
OCUDU_CE_RSRP_CHECK=1 ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 1 --out /tmp/r 2>&1 >/dev/null | grep -aE "comb|rsrp_params|k5|rsrp_check"
```

**关键旋钮**：`OCUDU_CE_DEV_STATS`（默认 1=设备算统计量，0=宿主算=**退路**）、
`OCUDU_CE_HOST_GRID`（默认 1）、`OCUDU_CE_HOST_Y_PADS`（默认 0）、`OCUDU_CE_CFO_CARRY_HOST`（默认 0）。

---

## 6. 需要你决定的事（**目前没有**）

* **γ**（`OCUDU_CE_HOST_GRID` 默认改 0）已被 **5a 取代**——不再需要"牺牲上报量"的决定。
  5a 做完后 γ 只是**水到渠成的一步**（读侧 → ≈0.33/跳）。
* 若 5a/5b 最终做不出来，退路是 `OCUDU_CE_DEV_STATS=0`，**回到原地**，功能不受影响。

## 7. 未定论的观察（别丢）

最近几腿的上行 **PUCCH fmt2 RSRP 在 −35 dB 左右**（早期 A/B 两腿是 −3.8/−2.9），
**但 CRC 通过率反而更高**（69~78% vs 68%/72%）。config 不同所以不可比。
**下次碰 RF 配置时优先核**（设计文档 §14.1 末尾）。

---

## 8. 状态清单

| 项 | 值 |
|---|---|
| HEAD | `94df4144a2` |
| 工作树 | 干净（`lib/` `include/` 无改动；config 有用户的未提交改动）|
| gnb 戳记 | `0e4a24ce57`（**落后**；纯新增改动，无发布路径依赖；跑 OTA 前须重建）|
| 最新存档 | `wip/worktree_backups/2026-09-19_5a-combs-ok/` |
| 设计文档 | `gpu_phy_pipeline_design_and_implementation.md`（含 §17 批次 5a 全章）|
| 探针 | `OCUDU_CE_RSRP_CHECK`（一次性 dump：`[comb]` / `[rsrp_params]` / `[k5]` / `[rsrp_raw]` / `[rsrp_check]`）|
