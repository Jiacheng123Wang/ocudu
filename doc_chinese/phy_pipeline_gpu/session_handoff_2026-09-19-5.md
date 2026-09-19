# session_handoff_2026-09-19-5 — 批次 5a **完成**（判据通过 + 发布路径已接）

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

分支 `apple-silicon`，HEAD = **`94df4144a2`**，**工作树有未提交改动**（本轮全部改动，见 §3）。

**✅ 批次 5a 完成。** 两件事都做完：

1. **对照探针通过**：设备归约 == 宿主归约，7 种语料形状，误差 2.6e-09 ~ 3.4e-08（float32 重结合底线）；
2. **发布路径已接**：上报的 rsrp 改用设备值，`_llr` / `_h` / `.bin` **逐字节不变**。

**读侧收益**（`--repeat 20`，实测）：

| `DEV_STATS` | `HOST_GRID` | 读/跳 | rsrp | 说明 |
|---|---|---|---|---|
| 1 | 1 | 2.10 | ✅ 设备值 | **当前默认**（行为未变）|
| **1** | **0** | **0.10** | ✅ 设备值 | **目标配置**，已跑通 |
| 0 | 1 | 2.10 | ✅ 宿主值 | 退路（完好）|
| 0 | 0 | 0.10 | ❌ 0 | 组合无意义（没人算它）|

**✅ 空中腿已跑并通过**（`5a-pubpath_0919_1230`，2026-09-19 12:30，详见设计文档 §17.8）：
读数/跳 **1.39**（上一腿 1.33，未回归）、**0 gaps**、**RT failure 0**、
**rsrp 中位数 −8.5 dB**（四腿范围 −5.0 ~ −8.5，无跳变）。
CRC 73.35% vs 78.51% 的差异**统计显著但与本批无关**（`_llr` 逐字节不变；这条线腿间波动本就 69%–79%）。

**⚠ 5b 期间发生一次 GPU 挂死事故（用户强制断电）** —— 根因是**我违反了 full_gpu_chain §48.131(c) 的
内核终止性硬约束**（第二次触犯同一条纪律）。三个 kernel（K5/K6/K7）已按纪律重写，
并**走完 §48.132(b) 的验证阶梯 L1–L4 全部通过**。详见 `wip/S12_incident_gpu_hang_2026-09-19.md`。

**5b 现状**：算法、K6、K7、**整链（K7→IDFT→K6）与宿主一致**（9 个时延最差 4.39 ns < 1 分辨率）；
**剩下的是把三个 dispatch 编码进车道同一条命令缓冲**（§4.3），然后 5c。

---

## 1. 目标（不变的最终判据）

> **`gpu` — 融合车道。整条 IQ → LLR 链跑在一条设备侧流水线里，只有两次 host↔device 数据穿越
> （IQ 上传、LLR 下载）。LDPC 不属于车道。** —— `include/ocudu/phy/phy_pipeline_mode.h`

**判据**：数据流进入 GPU 后，CPU 只能**提交命令缓冲**、**在出口等待**。其余任何逐跳的宿主计算或搬运都算缺陷。

**目标配置下剩下的 0.10 读** = **demapper 的 LLR 拷贝**（`llr_direct` 为假、即调用方缓冲未页对齐时），
属设计允许的两次穿越之一，**不是本批目标**。

---

## 2. 本轮之前的历史（六个缺陷，**都已修**）

详见 `session_handoff_2026-09-19-4.md` §2。**一句话**：
上一份交接（`-3`）的"设备归约写 0"**是探针的错**——六个缺陷里**五个在宿主侧/观测链**，
只有一个是 kernel 的真错误（边块读 `h` 的行索引用错变量）。

**必须记住的两条纪律**：

1. **改了 `.metal` 必须确认 metallib 真重编**（`rm -f .../ocudu_mmse.metallib` 再 build，
   并看到 `Linking Metal library`）。`.metal` 编译失败时 metallib **静默保持旧版**，
   于是"跑的是旧 kernel、宿主是新代码"，观测会自相矛盾。
2. **看到"某个值时有时无"，先怀疑映射/长度，再怀疑时序。** 本轮最后一块
   （ring 的 slot 布局 + 绑定长度不一致）被误判成"完成点可见性问题"，
   而线索（把长度临时放大后值就出现了）当时就在眼前。

---

## 3. 代码改动清单（**未提交**）

```
lib/phy/upper/signal_processors/channel_estimator/
  port_channel_estimator_average_impl.{h,cpp}      ★ 发布路径
        - virtual get_device_rsrp_sum(i_layer)     默认 nullopt（其它后端不受影响）
        - compute_hop_finish() 的 rsrp 累加：有设备值就用设备值，否则走原来的宿主累加
  metal/ocudu_mmse_rsrp.metal                      ★ K5 kernel
        - 输出布局 = K2 的块布局：(b * nof_layers + l) * 2
        - 边块的 block slot 判据 + blk_in_sys（读 h 的行）
  metal/ocudu_metal_mmse_engine.mm                 ★ K5 dispatch
        - 绑定长度 = (n_blk + ceil(nf_tail/nf_std)) * nof_layers * 2 个 float
        - grid = 同样的 slot 数 * nof_layers
  metal/test/port_channel_estimator_metal_mmse_unit_test.cpp   ★ S12 校验（5b 算法判据）
  metal/port_channel_estimator_metal_mmse_impl.{h,cpp}
        - rsrp_region_slots() / rsrp_region_floats()   ★ 区域长度的唯一定义
        - rsrp_attach_stage()      在 reformat_for() 里预留区域（那里才知道几何）
        - pending_unpack::rsrp_block/rsrp_floats   区域随 batch 传到 completion
        - device_rsrp_sums_ + get_device_rsrp_sum() 覆盖
        - 探针读 batch 自己那块区域
```

**已删除**：`rsrp_block_last` 成员（completion 不再需要）。

**未改**：`OCUDU_CE_DEV_STATS` 默认仍是 1，`OCUDU_CE_HOST_GRID` 默认仍是 1
⇒ **默认行为与计数器都没变**，退路完好。

---

## 4. 下一步：把 TA 接进车道（K7 摆放 + dft_dit + K6，同一条命令缓冲）

### 4.1 ✅ 5a 已完成并空中验证

腿 `5a-pubpath_0919_1230`：读数/跳 **1.39**（前腿 1.33，未回归）、**0 gaps**、**RT failure 0**、
**rsrp 中位数 −8.5 dB**（四腿范围 −5.0 ~ −8.5，无跳变）。
CRC 73.35% vs 78.51% 的差异**统计显著但与本批无关**
（离线 `_llr`/`_h`/`.bin` 逐字节不变；这条线腿间波动本就 69%–79%）。

### 4.2 ✅ 5b 算法与 kernel 都已通过（详见 `wip/S12_batch5b_ta.md`）

```
S12 [128]  PASS: profile worst rel 3.90e-06, peak host 100  device 100
S12 [256]  PASS: profile worst rel 5.99e-06, peak host 181  device 181
S12 [2048] PASS: profile worst rel 2.52e-05, peak host 1135 device 1135
S12 PASS: the device IDFT reproduces the host power delay profile
S12 TA  PASS: size=2048 max_ta_samples=144, worst |diff| 4.39 ns over 9 delays
S12 K6  PASS: size=2048 window=144, worst |diff| 4.39 ns over 9 delays
```

* **K6 kernel**（`ocudu_mmse_ta.metal` 的 `mmse_ta_profile`）：谱累加 + 半 CP 环形窗口找峰 +
  抛物线插值，输出秒。引擎有正式入口 `mmse_engine::run_ta_profile()`。
* 三段校验都挂在**已经在跑的** `port_channel_estimator_metal_mmse_unit_test.cpp` 里
  —— **不要为新校验造独立二进制**（本轮的坑，见 S12 文档 §7）。
* **实测常量**（从真实调用路径打印，S12 文档 §4.5）：SCS **30 kHz**、PUSCH **stride 2**、
  半 CP **1.1719 µs**、IDFT **128（4 PRB）/ 256（25 PRB）**、`max_ta_samples` **9 / 18**。

### 4.3 ⛔ 接进车道（**下一轮从这里开始**）

**三个 kernel 都已就绪且单独验证过**：

| kernel | 作用 | 状态 |
|---|---|---|
| **K7** `mmse_ta_place` | 读 LSE 导频 → 写进变换输入（**自然序**、零填充）| ✅ 单独验证（L2）|
| `dft_dit`（已有）| 变换输入 → spectra | ✅ 复用（设备 DFT 引擎的 kernel）|
| **K6** `mmse_ta_profile` | spectra → ta 秒 | ✅ 单独验证（S12 K6）|
| **整链** | K7 → IDFT → K6 | ✅ 9 个时延，最差 4.39 ns |

**要做的**：把这三个 dispatch 编码进**车道同一条命令缓冲**，中间两次 `memoryBarrierWithScope`。
twiddle/perm 表由引擎持有（perm 现在**不再需要** —— K7 是自然序）。
**接线草稿留档 `wip/S12_engine_wiring_attempt.mm.txt`**（表构造 / encode / 组装四个函数可参考）。
**⚠ 接线时同样受 §48.131(c) 约束**：任何新 kernel 的循环上界必须是编译期常量。

### 4.4 ⚠ 边界

* 设备路径与宿主**不会逐位相同**（同一个抛物线插值，输入是两份不同的 IDFT 输出）
  ⇒ 容差按**分辨率**给，不是 ULP（5a 同理）。
* 多 slot 边块（`nf_tail > nf_std`）在 5a 里只做了单元推导，首次遇到要核。

## 5. 重要的运行纪律（每次都用）

1. **腿一律用 Ctrl-C（SIGINT）停。** SIGTERM 会**跳过全部收尾统计**，跨越计数事后无法恢复（坑 23）。
2. **判腿有效性的第一眼**：`radio sample continuity` 与 `host sample assembly` 必须 **0 gaps**；
   `-- device side` / `-- lane` 两段必须非空（坑 21）。
3. **构建必须显式 `--target`**，且**改了 `.metal` 必须确认 metallib 重编**（§2 纪律 1）。
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

# 5a 的对照探针（判据）
OCUDU_CE_RSRP_CHECK=1 ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 1 --out /tmp/r 2>&1 >/dev/null \
    | grep -aE "rsrp_stage|rsrp_raw|rsrp_check"

# 上报值不因来源而变（A/B，看 rsrp 那一项）
for d in 1 0; do OCUDU_CE_DEV_STATS=$d ./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 1 --out /tmp/d$d >/dev/null 2>&1; \
    echo "DEV_STATS=$d: $(cat /tmp/d${d}_*_ce.txt)"; done

# 稳态计数（唯一的性能判据）
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/x 2>&1 >/dev/null | grep -a crossings -A1

# 27 捕获数据 A/B（数据判据）
bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh "" "OCUDU_CE_HOST_GRID=0"

# 单元测试
./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
```

**关键旋钮**：`OCUDU_CE_DEV_STATS`（默认 1）、`OCUDU_CE_HOST_GRID`（默认 1，见 §4.3）、
`OCUDU_CE_RSRP_CHECK`（探针）、`OCUDU_CE_HOST_Y_PADS`（默认 0）、`OCUDU_CE_CFO_CARRY_HOST`（默认 0）。

---

## 7. 需要你决定的事（**目前没有**）

* `HOST_GRID` 默认何时翻：**建议等 5b**（§4.3）。这是一次"牺牲上报量"的决定，
  但 5a 已经让 rsrp 不再需要它，所以**翻的代价比原先小得多**（只剩 noise_variance 与 TA）。
* 若 5b 最终做不出来，退路是 `OCUDU_CE_DEV_STATS=0`，功能不受影响。

---

## 8. 未定论的观察（别丢）

最近几腿的上行 **PUCCH fmt2 RSRP 在 −35 dB 左右**（早期 A/B 两腿是 −3.8/−2.9），
**但 CRC 通过率反而更高**（69~78% vs 68%/72%）。config 不同所以不可比。
**下次碰 RF 配置时优先核**（设计文档 §14.1 末尾）。

---

## 9. 状态清单

| 项 | 值 |
|---|---|
| HEAD | `94df4144a2` |
| 工作树 | **有改动**：`lib/` 下 **9 个文件 + 1 个新 kernel**（5a + 5b 校验 + K6）+ config（用户）|
| **5a 探针** | **✅ 通过**（7 种语料，`dev_nre == host_nre`，worst rel ~1e-8）|
| **发布路径** | **✅ 已接**（`_llr`/`_h`/`.bin` 逐字节不变，rsrp 与宿主路径一致）|
| **空中腿** | **✅ `5a-pubpath_0919_1230` 通过**（0 gaps、RT failure 0、rsrp 无跳变、读 1.39/跳）|
| 空中契约 | 7/8（`host device data crossings` FAILED = 预期，默认配置回读仍在）|
| 单元测试 | **全过**（含新的 S12 设备 IDFT / 设备 TA 校验）|
| **5b 算法判据** | **✅ 通过**：设备 IDFT 谱 == 宿主；设备 TA == 宿主 TA |
| **5b K6 kernel** | **✅ 通过**：与宿主一致（9 个时延，最差 4.39 ns < 1 分辨率）|
| **5b K7 摆放** | **✅ 通过**（最小几何 3×24，0 mismatching）|
| **5b 整链** | **✅ 通过**（K7→IDFT→K6，9 个时延最差 4.39 ns < 1 分辨率）|
| **GPU 挂死事故** | **已消除**（三个 kernel 按 §48.131(c) 重写，阶梯 L1–L4 全过，`recoveryCount=0`）|
| 5b 接线 | ⬜ 未做（§4.3：三个 dispatch 进同一条命令缓冲）|
| 计数器（默认配置）| **2.10 读 + 0.65 写/跳**（未回归）|
| 计数器（`HOST_GRID=0`）| **0.10 读 + 0.65 写/跳** |
| gnb 戳记 | `0e4a24ce57`（**落后**；跑 OTA 前须重建）|
| 最新存档 | `wip/worktree_backups/2026-09-19_5a-pass/` |
| 设计文档 | `gpu_phy_pipeline_design_and_implementation.md`（§17.6 判据 / §17.7 发布路径 / §5 批次表）|
