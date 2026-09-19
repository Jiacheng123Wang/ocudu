# session_handoff_2026-09-19-4 — 批次 5a **对照探针通过**

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

分支 `apple-silicon`，HEAD = **`94df4144a2`**，**工作树有未提交改动（本轮的修复，全是 `lib/` 下的）**。

**✅ 批次 5a 的对照探针通过**：设备归约 = 宿主，7 种语料形状全对，误差在 float32 重结合底线（~1e-8）。

```
[rsrp_raw]   region=0 slots=4 layers=1 | [0][0]=3.325204e-01 nre=54  [1][0]=2.110410e-01 nre=18
[rsrp_check] worst rel 5.354e-09 (dev 5.435613990e-01 host 5.435613990e-01) | host_nre 72 dev_nre 72
```

**⛔ 上一份交接（`-3`）的两条核心结论都是错的**（见 §2）：设备归约**从来没写 0**，
问题全在**观测链**和**宿主侧的长度/布局**上。

**下一轮：接发布路径（§4），然后 5b。** 不需要你做任何决定。

---

## 1. 目标（不变的最终判据）

> **`gpu` — 融合车道。整条 IQ → LLR 链跑在一条设备侧流水线里，只有两次 host↔device 数据穿越
> （IQ 上传、LLR 下载）。LDPC 不属于车道。** —— `include/ocudu/phy/phy_pipeline_mode.h`

**判据**：数据流进入 GPU 后，CPU 只能**提交命令缓冲**、**在出口等待**。其余任何逐跳的宿主计算或搬运都算缺陷。

**计数器未回归**：`--repeat 20` 仍是 **2.10 读 + 0.65 写/跳**（与 `94df4144a2` 一致，发布路径仍未接）。

---

## 2. 本轮定位到的**六个**缺陷（**全部已修**，这是 5a 从"写 0"到"通过"的全过程）

### 2.1 ★★ metallib 增量构建不可靠（**这是最坑的一条，先看这个**）

**症状**：改了 `.metal` 之后 `cmake --build` **不重新编译 metallib**，于是**跑的是旧 kernel**，
而宿主代码是新的 ⇒ 观测结果自相矛盾（"kernel 收到了参数" 与 "kernel 读到的参数是 0" **同时成立**）。

**证据**：`ocudu_mmse_rsrp.metal` 09:40 修改，metallib 也是 09:40 —— 看着"已重建"，
但**内容没变**；只有 `rm -f ocudu_mmse.metallib` 之后才真正重编（`Linking Metal library` 才带实际编译输出）。
**本轮的假线索有一大半来自这里**（"参数是 0"、"kernel 没跑"、"dispatch 只有 1 个"）。

**纪律**：
* 改 `.metal` 后**必须**确认重编：要么 `rm -f lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib`，
  要么 `touch` 该 `.metal` 再 build，并**看到 `Linking Metal library ocudu_mmse.metallib`**。
* **编译报错时 metallib 会静默保持旧版**（`air` 没生成，链接仍"成功"）。
  ⇒ **`.metal` 编译失败必须当成构建失败**，不能只看 `Built target`。
* 探针结论只有在"确认重编"之后才算数。

### 2.2 ★★ 归约**没有**写 0：是探针读错了 ring 步长

K5 的输出按 `[block slot][layer] -> {sum, count}` 写进 hop 的 ring 区。
**宿主读的步长和 kernel 写的不是同一个**：

| 侧 | 表达式 | 本例（nof_layers=1, kRsrpSlots=4）|
|---|---|---|
| kernel（旧） | `(blk * nof_layers + lay) * 2` | 0 |
| 宿主（旧探针） | `(rsrp_block + b * kRsrpSlots + l) * 2` | 8 |
| kernel（现） | `(blk * kRsrpSlots + lay) * 2` | 8 ✅ |

`nof_layers == kRsrpSlots` 只在 4 层时巧合成立 ⇒ **空中永远对不上**。
**上一份交接的"设备归约写 0"就是这么来的。** 现在 kernel 用 `kRsrpSlots` 作步长，两侧一致。

### 2.3 ★★ 绑定长度把第二个 block slot 截断了

`rsrp_bytes` 原来只按**标准块数**算（`n_blk * nof_layers * 2 * 4` 字节）；
K5 要写**整个**区域（标准块 + 边块的 slot）。**超出部分被静默丢弃** ⇒ 边块归约读回 0。
已按 `(n_blk + ceil(nf_tail / nf_std)) * kRsrpSlots * sizeof(float)` 定长。

### 2.4 ★ 边块的 threadgroup **一个都没派发**

`dispatchThreadgroups` 原来只发 `n_blk * nof_layers` 个（标准块数），边块的 slot **从未被覆盖**。
已改为 `(n_blk + tail_slots) * nof_layers`。

**另外**：kernel 的早退判据 `blk >= p.n_blk` **会把边块的 threadgroup 全部拒掉**
（边块在 `sys_tail` 起的**系统**里，`blk` 仍是 0/1，但 `n_blk` 只数标准块）。
已改成按 **block slot** 判：slot `>= n_blk` 就是边块几何。

### 2.5 ★★ 边块把 `h` 的**行索引**用错了（**这一步才让边块开始出数**）

`h` 是 `[nof_systems][n_blk][2 * nout_stride]`。边块在 `sys_tail` 起的**系统**里、是那个系统的 **block 0**。
但 kernel 把 GRID 的 block slot（= `n_blk + k`，边块 ≥ 1）直接当成**系统内的 block 号**：

```c
// 错：边块 blk=1 ⇒ 读到 (sys*n_blk + 1) 那一行，即边块自己的下一行 —— K2 从未写过的内存
h + (sys * p.n_blk + blk) * (2 * p.nout_stride) + ...
```

⇒ 边块**加的是 0**，而 **RE 计数是对的**（计数不看 `h`）——这正好解释了
"`nre` 对、`sum` 为 0" 这个一直看着矛盾的现象。宿主侧 K3 用 `b = 0`，所以只有 K5 错。

**修正**：新增 `blk_in_sys = is_std ? blk : 0u`，**只用它算 `h` 的行**；`sc0` 和输出 slot 仍用 grid 的 `blk`。

**修正后**：边块产出 `sum = 2.110410e-01`、`nre = 18`
⇒ 标准块 54 + 边块 18 = **72，与宿主的 `host_nre` 一致**（三处独立读出同一个值）。

### 2.6 ★★ ring 的 slot 布局两侧不一致（**最后一块，修完就通过**）

kernel 和宿主对"一个 block slot 占几个 float"的理解不同，**而且保留长度只够前一个理解**：

| 侧 | slot `b`、layer `l` 的偏移 | 本例 slot 1 |
|---|---|---|
| kernel（旧） | `b * kRsrpSlots` | 8 |
| 宿主（旧） | `b * kRsrpSlots`（读）但长度按 `n_blk + tail` 个 **kRsrpSlots** | 4 |
| **两侧（现）** | **`(b * nof_layers + l) * 2`**（= **K2 的块布局**）| 4，长度 4 个 float |

⇒ 边块写偏移 8、宿主只映射到偏移 7 ⇒ **正确的值落在映射之外，被静默丢弃**。
**统一成 K2 的布局**（kernel 写、engine 绑、宿主读**同一个表达式**：
`(b * nof_layers + l) * 2`，长度 `(n_blk + tail_slots) * nof_layers * 2` 个 float）后立刻读到。

**⚠ 复盘**：当时误判成"完成点可见性/时序问题"（`[rsrp_raw]` 读到 0、稍后又读到值）。
真因是**绑定长度**——我在排查时把长度临时放大到 1200 个 float，**边块的值就"出现"了**，
这本身就是那条线索；当时没抓住。**教训：观测到"某个值时有时无"，先怀疑映射/长度，再怀疑时序。**

---

## 3. 代码改动清单（本轮，**未提交**）

```
lib/phy/upper/signal_processors/channel_estimator/metal/
  ocudu_mmse_rsrp.metal
        - 输出布局 = K2 的块布局：slot b、layer l 在 (b * nof_layers + l) * 2（§2.6）
        - 边块的 block slot 判据 + blk_in_sys（§2.4 / §2.5）
        - pilot_re_bits 每层独立（沿用）
  ocudu_metal_mmse_engine.mm
        - K5 绑定长度 = (n_blk + ceil(nf_tail / nf_std)) * nof_layers * 2 个 float（§2.3 / §2.6）
        - K5 grid = 同样的 slot 数 * nof_layers（§2.4）
  port_channel_estimator_metal_mmse_impl.{h,cpp}
        - rsrp_region_slots() / rsrp_region_floats()   ★ 区域长度的**唯一定义**
        - rsrp_attach_stage()      在 reformat_for() 里预留区域（那里才知道几何）
        - pending_unpack::rsrp_block/rsrp_floats   区域随 batch 传到 completion
        - 探针读 batch 自己那块区域，偏移 (b * nof_layers + l) * 2
```

**已删除**：`rsrp_block_last` 成员（completion 不再需要）；staging/completion 混用块索引的旧逻辑。

## 4. ⛔ 下一步：接发布路径（5a 的收尾），然后 5b

### 4.1 判据已达成（§3.4 第 4 步）

设备归约 == 宿主归约，7 种语料形状全对，误差 2.6e-09 ~ 3.4e-08（float32 重结合底线）：

| 语料 | 标准块 | host_nre | dev_nre | worst rel |
|---|---|---|---|---|
| syn001_3 | 1 | 54 | 54 | 3.4e-08 |
| syn004_4 | 1 | 72 | 72 | 5.4e-09 |
| syn012_8 | 2 | 144 | 144 | 3.0e-08 |
| syn018_12 | 4 | 216 | 216 | 2.6e-09 |
| syn022_18 | 6 | 324 | 324 | 2.0e-08 |
| syn025_25 | 8 | 450 | 450 | 2.0e-08 |
| syn027_25 | 8 | 450 | 450 | 2.2e-08 |

多跳重复运行数值完全一致 ⇒ ring 的区域划分跨跳无别名。

### 4.2 要做的（顺序，不要跳）

1. **接发布路径**：`compute_hop_finish` 的 rsrp/snr 用**设备值**（现在仍用宿主网格）。
   设备值在 `gpu_rsrp[rsrp_stage_ .. + rsrp_stage_slots_)`，布局 `(b * nof_layers + l) * 2`。
2. **一个 `OCUDU_CE_DEV_STATS=0` 的 A/B**：退路必须仍然可用（宿主自己算）。
3. 然后 **5b（TA）、5c（开关默认值）、5d（γ）**。

### 4.3 ⚠ 这一步要动上报量，所以

* 发布路径一接，**rsrp/snr 的来源就变了** ⇒ 需要一条**空中腿**验证上报值没有跳变（§10.1 停机规则）。
* 在这之前先跑 **§6.3 数据不变**（27 捕获 A/B）与 **§6.5 单元测试**。

### 4.4 已知的边界（不影响 5a，但别忘）

* 边块的 slot 划分按 `ceil(nf_tail / nf_std)`——**多 slot 边块**（`nf_tail > nf_std`）本轮
  没有语料覆盖，只做了单元推导。**首次遇到时要核。**
* `rsrp_base_` 的区域分配是**顺序绕环**，不是每跳固定；跨跳别名已用 `--repeat` 验证，但**长跑**（OTA）时值得再看一眼。

## 5. 重要的运行纪律（每次都用）

1. **腿一律用 Ctrl-C（SIGINT）停。** SIGTERM 会**跳过全部收尾统计**，跨越计数事后无法恢复（坑 23）。
2. **判腿有效性的第一眼**：`radio sample continuity` 与 `host sample assembly` 必须 **0 gaps**；
   `-- device side` / `-- lane` 两段必须非空。**连续性坏掉的腿，数字不能用**（坑 21）。
3. **构建必须显式 `--target`**，且**改了 `.metal` 必须确认 metallib 重编**（§2.1，**本轮新增**）。
4. **计数必须 `--repeat 20`**，单跳缓存是冷的。
5. **`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 有未提交改动**（用户重配的 PUCCH 段）。
   **动分支前先 `bash wip/backup_worktree.sh <label>`**（坑 22）。该 config 必须带**六个 PUCCH 资源键**。
6. **离线数字是方向与量级的预测，不是空中数值的预测**；不一致时**以空中为准**（§15.4）。

---

## 6. 判据工具（详见设计文档 §6）

```bash
cd /Users/jiachengwang/dev/ocudu
# ★ 改了 .metal 就必须删掉 metallib 再构建，否则跑的是旧 kernel
rm -f lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib
cmake --build build --target ul_chain_replay port_channel_estimator_metal_mmse_unit_test
# 稳态计数
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/x 2>&1 >/dev/null | grep -a crossings -A1
# 5a 的对照探针
OCUDU_CE_RSRP_CHECK=1 ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 1 --out /tmp/r 2>&1 >/dev/null \
    | grep -aE "rsrp_stage|rsrp_raw|rsrp_check"
# 单元测试
./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
```

**关键旋钮**：`OCUDU_CE_DEV_STATS`（默认 1）、`OCUDU_CE_RSRP_CHECK`（探针）、
`OCUDU_CE_RSRP_PROBE`（kernel 侧模式：1=常数、2=nof_sub、4=回读参数块；**仅调试**）、
`OCUDU_CE_HOST_GRID`（默认 1）、`OCUDU_CE_HOST_Y_PADS`（默认 0）、`OCUDU_CE_CFO_CARRY_HOST`（默认 0）。

---

## 7. 需要你决定的事（**目前没有**）

* **γ**（`OCUDU_CE_HOST_GRID` 默认改 0）已被 5a 取代，5a 做完后是水到渠成的一步。
* 若 5a/5b 最终做不出来，退路是 `OCUDU_CE_DEV_STATS=0`，功能不受影响。

## 8. 未定论的观察（别丢）

最近几腿的上行 **PUCCH fmt2 RSRP 在 −35 dB 左右**（早期 A/B 两腿是 −3.8/−2.9），
**但 CRC 通过率反而更高**（69~78% vs 68%/72%）。config 不同所以不可比。
**下次碰 RF 配置时优先核**（设计文档 §14.1 末尾）。

---

## 9. 状态清单

| 项 | 值 |
|---|---|
| HEAD | `94df4144a2` |
| 工作树 | **有改动**：`lib/.../metal/` 下 4 个文件（本轮修复）+ config（用户）|
| **5a 探针** | **✅ 通过**（7 种语料，`dev_nre == host_nre`，worst rel ~1e-8）|
| 单元测试 | **全过**（含 K3/K4 与三种 lane 序）|
| 计数器 | **2.10 读 + 0.65 写/跳**（无回归）|
| 发布路径 | ⬜ **仍未接**（设备值算出来了但不影响上报）⇒ 默认行为未变，退路完好 |
| gnb 戳记 | `0e4a24ce57`（**落后**；跑 OTA 前须 `touch build/hashes.h && cmake --build build --target gnb`）|
| 最新存档 | `wip/worktree_backups/2026-09-19_5a-combs-ok/`（**本轮之前的**；建议先 `wip/backup_worktree.sh 5a-probe-pass`）|
| 设计文档 | `gpu_phy_pipeline_design_and_implementation.md`（§17.6 已按本轮更正）|
| 探针 | `OCUDU_CE_RSRP_CHECK`（`[rsrp_stage]` / `[rsrp_raw]` / `[rsrp_check]`）|
