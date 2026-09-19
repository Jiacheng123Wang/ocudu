# Session handoff —— 2026-09-18（S-7g-20：`sigma2_rel` 的商搬上设备

> **⚠ 续接请优先读 `session_handoff_2026-09-18-4-compact.md`**（本 session 的压缩备忘，含K0-a 上机验证、gap 拆解完成、ping 尖峰根因消除、以及未提交的观测改动）。本文件的 §2.1–2.21 是细节来源。并**默认开**；真凶是 fast-math，不是硬件）

> 换会话先读这一份，再读 `s2_full_chain_design.md` 的 **§48.146(a) 状态索引**。
> 本 session 的全部细节：**§48.194**（把商搬上设备：fast-math 是真凶、为什么必须拆成独立 metal 文件、
> 两个真缺陷（越界写 / 字段名不一致）、新门 `ratdev`、由此定下的纪律）。
> 上一份：`session_handoff_2026-09-18-2.md`（S-7g-19：Step 1′ 三腿判读 + K0-a 侦察）。

---

## 0. 一句话现状

**K0-a 的 `sigma2_rel` 现在由设备算、且与宿主逐位相同、默认开、七门全绿。**
关键是**编译标志**：这个 GPU 的 float 除法是正确舍入的，但 Metal 的**默认 fast math** 会把那条除法重结合，
2²⁰ 对随机样本里 **28.4%** 与宿主不同 ⇒ 曾翻 LLR 判决 32613 位（27/27 捕获）。
加上 `-fno-fast-math` 后是 **0/2²⁰**。因为标志是**文件级**的，有契约的那个核被拆到了新文件
`ocudu_mmse_pilots_power.metal`（其余 K0-a 核保持 fast math，否则 LSE 的最后一位也会动）。

下一步：**第二件（`hop_stage` 接手，§48.193(d)）**——现在设备商可用，宿主那次标量读取**可以**去掉，收益要重估。

---

## 1. 代码状态（回来第一件事：核对这一节）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `apple-silicon` / **`d49f3a16fd`**（**已提交并 push**，与 origin 同步）|
| **工作树** | `lib/` **干净**（`git status --short lib/` 应为空）；未跟踪的只有 `configs/*.yml`、`doc_chinese/`、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh` |
| 本 session 的提交 | **`d49f3a16fd`** = S-7g-20 第一件（K0-a 的商搬上设备）；父提交 `74ba4c5251` |
| 里程碑 tag | `gpu_phy_fused_lane_ready` → `690b086679`（未动）；`gpu_phy_iq2llr_zero_host_copy` → `3d026939eb`（冻结）。**未打新 tag** |
| 树里的二进制 | `build/apps/gnb/gnb`（**已含工作树改动**）；`cmake -P build/build_info.cmake && cmake --build build --target gnb -j 14` 后核对 stamp |
| 参考二进制 | `doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref` **＋并排 4 个 `.metallib`** |
| 语料 | `doc_chinese/work_tmp/corpus/`（27 个合成捕获）|
| **不要提交** | `configs/*.yml`、`doc_chinese/`（gitignored）、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh` |

**本 session 改动的文件（未提交）**

```
M lib/.../metal/CMakeLists.txt                      新文件进 SOURCES；IEEE_MATH_SOURCES 换成 pilots_power
M lib/.../metal/capture_gates.sh                    新增 ratdev 门（+ usage）
M lib/.../metal/ocudu_metal_mmse_engine.h            corr_stage::sigma2_dev(BASE)+sigma2_slot；pilots_stage::nof_power_pilots；sigma2 四槽
M lib/.../metal/ocudu_metal_mmse_engine.mm           两侧结构 + offsetof 断言；wrap 2→4 floats；绑定 buffer(2)
M lib/.../metal/ocudu_mmse_corr.metal                A 核收 scalars[[buffer(2)]]，读 scalars[p.sigma2_slot]（132B）
M lib/.../metal/ocudu_mmse_pilots.metal              功率核移出；参数结构加 nof_power_pilots + sizeof 断言（56B）
M lib/.../metal/port_channel_estimator_metal_mmse_impl.{h,cpp}  kRatioSlot 等槽位常量、默认开、RATIO_CHECK 探针
?? lib/.../metal/ocudu_mmse_pilots_power.metal       **新**：唯一 -fno-fast-math 的 K0-a 核
```

---

## 2. 本 session 做完的事

### 2.1 §48.193(e).1 的两个疑点都查清了（**都不是缺陷**）

1. **单测里 `geom=0` 的唯一原因是夹具**：`grid_fake`（`…unit_test.cpp:35`）没覆写
   `resource_grid_reader::get_device_view()`，基类默认返回 `{}` ⇒ `is_valid()==false` ⇒ 门关。
   空气路线走 `resource_grid_reader_impl`，**恒为有效**。实测 27/27 捕获都进了设备 K0-a
   （`device_sigma2=1`、`[k0a_ratio]` 有输出）⇒ 门**不会静默失效**。
   ⇒ **T2 的 Test 14 必须自建设备驻留网格**，否则门必然被夹具关掉。
2. **`args.compensate_cfo_flag`**：单测传的是 `true`（第 5 个构造参数），"传 false"系误记，打印的 `1` 与之相符。

### 2.2 第一件完成：`sigma2_rel` 的商搬到设备（**默认开**）

**真凶**（这是本 session 最重要的更正）：

| `xcrun metal` 标志 | 商与宿主不同的比例（2²⁰ 对随机 (sigma2,power)）|
|---|---|
| 默认（fast math）| **298151 / 1048576 = 28.4%**，两个方向都有 |
| **`-fno-fast-math`** | **0 / 1048576** |

⇒ **§48.193(c) 的推理（"同样两次运算 ⇒ 逐位相同"）错在把"同样的表达式"当成了"同样的舍入"。**
`ocudu_mmse_corr.metal` 早就在 `IEEE_MATH_SOURCES` 里，正是同一个原因；K0-a 的商只是还没被认出是同类量。
放大系数：`sigma2_rel` 是 A 的对角加载，`cond_2(A) ~ 2e4`。默认标志下 27 捕获**逐字节相同 0/27**、
最差 21161 字节、**LLR 翻 32613 位（27/27）** —— 语义级，`ab_tol.sh` 的 `flips == 0` 直接拒绝。

**做法（两次才对）**

1. ~~把 `ocudu_mmse_pilots.metal` **整个**加进 `IEEE_MATH_SOURCES`~~ ⇒ **27/27 捕获都与参考不同**
   （LSE 等核没有契约，却被一起改了最后一位；LLR 仍逐字节相同、flips 0，但"1 ulp"变成"每个捕获都动"）。
2. ✅ **把有契约的那一个核拆成新文件 `ocudu_mmse_pilots_power.metal`**，只它严格；其余保持 fast math。
   代价：`mmse_sigma2_params` / `mmse_sigma2_dims` / `mmse_sigma2_clamp` 两份（MSL 无共享头），
   两侧 `static_assert(sizeof == 56)` 钉住 + 注释写明手工同步。

**接口形状**：`corr_stage::sigma2_dev`（**缓冲区 BASE**）+ `corr_stage::sigma2_slot`（读第几个），
核读 `scalars[p.sigma2_slot]`；`mmse_corr_params` 加 `sigma2_from_device`+`sigma2_slot`（124→132B）；
`mmse_sigma2_params` 加 `nof_power_pilots`（52→56B）；`gpu_ls_sigma2` 四槽（sigma2 / 功率和 / 商 / 均值）有名字与 `static_assert`。

### 2.3 路上踩到的**两个真缺陷**（都不是数值问题）

1. **越界写**：核新写 `out[2]`/`out[3]`，但 `wrap(s.sigma2, 2 * sizeof(float))` 只给 8 字节。
   **症状不是崩**：CE 单测 Test 3 在 SNR 20 dB **退 3.94 dB**、Test 9 的 `nv/l²` **漂 18×**。
   ⇒ **内核新写的每个槽都要在 `wrap()` 的长度里**。
2. **宿主/核字段名不一致**：宿主叫 `sigma2_slot`、核叫 `sigma2_from_device`，**大小相同（都 128B）**
   ⇒ `static_assert(sizeof…)` 看不见 ⇒ 核读到的开关是 0，老老实实用 `p.sigma2`，而宿主以为交出了设备地址。
   **症状**：`noise_variance` 0.1147 → 7.23（A 几乎没有对角加载）。
   ⇒ 两侧加 **`offsetof` 断言**（corr 5 条 + sigma2 4 条），同类错误从此是编译错误。
   同源口径错误：**交的是 BASE 不是元素地址**（第一版交 `&gpu_ls_sigma2[2]`，核里又读 `[2]` ⇒ 越界读）。

### 2.4 新门 `ratdev`（把"逐位相同"变成会被回归抓住的判据）

`capture_gates.sh` 加 `ratdev`（`run_gates.sh` 已收进**每腿七门**）：
route A = `OCUDU_CE_K0A_RATIO_DEV=1`(+`RATIO_CHECK`)，route B = `=0`，**要求 dumps 逐字节相同**（无容差），
并用 `[k0a_ratio]` 行证明 route A 真走了设备商（否则宿主对宿主 = 没测）。实测 `27/27 byte-identical, vacuous=0`。
它守两件事：**`-fno-fast-math` 还在**，且**核读的是宿主指的那个元素**。

---

## 2.6 上机腿（默认配置、无旋钮）：**`gnb_ota_k0a_dev_0918_0911`**

手机 OTA，793 PUSCH、46736 时隙。与上一默认臂 `gnb_event_0918_0658`（`74ba4c5251`）对照：

| 量 | 上一默认臂 | 本腿 | |
|---|---|---|---|
| `device_sigma2` | 970 | **892 = hops_gpu** | 设备商每跳都产出 |
| `[mmse_time_sum] sigma2` | 0.1 µs | **0.0 µs** | 宿主不再做那次除法 |
| lane busy | 505.9 µs | **505.8 µs** | GPU 工作量不变 |
| busy split `ch_est` | 428.5 µs/lane | **430.8 µs/lane** | 同上 |
| residency / gap | 745.2 / 239.3 | **735.7 / 229.9** | 略紧，波动内 |
| crc OK/KO | 855/115 | 793/99（88.9%）| 不退化 |
| Real-time failures | 0 / 66024 | **0 / 46736** | 不退化 |
| 契约 / 警告 | 7/7 | **7/7、警告 0** | 不退化 |

⇒ **"机制变了、数值不变"** 在真实空口上成立。K0-a 那次宿主同步**仍在**（`[ul_channel_estimation]` 259.5 µs，
没掉到 ≈180）——那是第二件的事。

**顺带修 `air_leg_report.sh` 一个"藏证据"的问题**：`mmse_ce` 行被 `cut -c1-70` 截断，而
`device_sigma2`（本 leg 最关键的证据）在**行尾**（整行 201 字符）——截断后根本看不到。已去掉 `cut`。

**一个尚未查清、也不该假装查清的观察**：`[ul_channel_estimation]` = 259.5 µs，而该阶段的
宿主实际工作只有 `stage=0.04 + pre=0.89 ≈ 0.93 µs`（`[mmse_time_sum]`，max total 172 µs）。
即**这 259 µs 绝大部分不是 K0-a 的计算或读标量**，而是该阶段边界内别的东西（`record_ce_end()` 在
`pusch_processor_impl.cpp:250`，覆盖 estimator→notifier 链的整段宿主墙钟）。
⇒ **第二件开工前要先量清这 259 µs 的构成**，否则会按"省掉读标量 ⇒ 省 ~94 µs"的旧算式去做一件
收益对不上的事（那次读标量实测是 `sigma2=0.0~0.1 µs` 量级）。

### 2.13 收口腿 `gnb_ota_queue_0918_0957`：**gap 的三个来源全部量完 ⇒ 95% 是依赖串行化**

合格（失败 **0/65275**、契约 **7/7**、`device_sigma2=2472=lanes`、无遗留进程）：

| gap 的归属 | 值 | 占比 |
|---|---|---|
| ① 宿主（开工 → 提取 CB commit）| **10.3 µs** | **5.0%** |
| ② 队列（commit → lane 首条 CB 在 GPU 开始）| **0.0 µs** | **0.0%** |
| ③ **依赖串行化**（burst 等提取/权重跑完）| **194.7 µs** | **95.0%** |
| lane gap 合计 | 205.0 µs | 100% |

每 lane 三条 CB：**提取**(6 dispatch) → **权重**(相关+K1+apply+scatter+K3/K4) → **burst**(均衡+解映射)。
②=0 说明"宿主 commit 后 GPU 立刻开跑"，**提交/队列层面无可削**；①只有 5%。
③ 是结构性的：burst 的均衡读 `gpu_h`/`gpu_ce`/`gpu_nv`，全在权重那条 CB 里，而它必须排在提取之后。

⇒ **本轮"还剩什么"有了实测答案：lane 内已无宿主/队列开销可削，剩余 95% 是数据依赖
（"每 lane 一次同步"的直接体现）。** 要再往下走，只能动架构（让均衡不等整条权重 CB），
不是拧旋钮。

### 2.14 **秒级 ping/iperf 尖峰已查清：UE 掉线/重接入，非 gNB 停摆、非本 session 引入**（更正 §2.13 的怀疑）

用户实测（`10.45.0.32`，交替 ping/iperf3）：RTT 从 20–40 ms 突抬到 **1.5–4.4 s**、随后**线性下降**回基线
（每包 ≈−106 ms），iperf3 同期"某秒传输 0 + 数百重传"。**原因在日志里**：

```
静默 2605 ms @04:48:31  ← "Discarding UL HARQ process TB … Maximum number of reTxs 4 exceeded"
静默 1409 ms @04:48:23  ← "UE Context Release Procedure finished successfully"
静默 1214 ms @04:47:52  ← "MAC UE Removal / UE Delete"
静默 1020 ms ×N         ← 空闲（1 s FFT 节奏）
（3141 ms 那条是启动序列）
```

⇒ **HARQ 重传用尽 ⇒ 释放 UE ⇒ UE 重新 PRACH 接入**；这几秒内 UE 没有无线链路、包在 UE 侧排队，
链路恢复后队列排空 ⇒ **"RTT 线性下降"就是排空过程**。

**不是回归**（8 条腿含本 session 前 3 条，全都有同样量级的多秒静默 + UE release + max-reTx，
且不随提交单调恶化）：

| 腿 | UE release | 最长静默 | max-reTx |
|---|---|---|---|
| `event_0918_0658`（session 前）| 4 | **16928 ms** | 21 |
| `burstctrl_0918_0716` | 6 | 3664 ms | 10 |
| `waitctrl_0918_0717` | 8 | 7090 ms | 15 |
| `ota_k0a_dev_0918_0911` | 6 | 3631 ms | 16 |
| `ota_gapsplit_0918_0940` | 4 | 4201 ms | 41 |
| `ota_final_0918_0949` | 6 | 8570 ms | 669 |
| `ota_queue_0918_0957` | 4 | 9335 ms | 79 |
| `ota_queue_0918_1247` | 10 | 3141 ms | 45 |

⇒ **不需要 revert 做对照**：判据是"尖峰频率是否随提交变化"，上表答"没有"。
**更正我自己**：§2.13 引用的"四条腿没有 170 ms 停顿"只看了 P99.9，**漏看了最大值**——秒级事件就在那里。
gap 的 95% 是依赖串行化这一结论不受影响（那是 lane 内 GPU 时间戳）。

### 2.21 **上机腿 `wt_split`：空口数字确认"权重占 3/4"**

```
busy split: ch_est=113.4us/lane (22% of busy, cbs=1) ch_wt=325.7us/lane (64%, cbs=1) eq_demap=73.7us/lane (14%, cbs=1)
lanes=2807  residency 705.6  busy 512.8  gap 192.8  host 9.8  queue 0.0
```

**与离线同向**（离线 100.6 / 390.5 / 88.8）：**权重 CB 占 64–67% 的核时间，而 burst 的均衡正等它。**
⇒ §48.194(g-nonies) 的 95% 依赖份额，**等的是权重的 CB**。

**权重 CB 内部的 dispatch 顺序**（`encode_run` / `encode_weights_only`，两处一致）：

```
encode_corr  (corr_a → inv/K1 → corr_r_hp)
K2 apply     → h
weights(K2b) → W            (matrix flavor)
pilots_scatter (可选，glue #2)
encode_reformat (K3 均衡器估计 + K4 噪声方差)
```

⇒ **burst 真正需要的是那条 CB 的末尾（reformat 产出 `gpu_h`/`gpu_ce`/`gpu_nv`）**。
所以"缩短它"有两条路：① 让 reformat 更快；② **把 reformat 拆成独立的 CB/fence**，
让 burst 只依赖 K2——但那样 `cbs/lane` 会从 3 变 4，需权衡。

### 2.20 **新观测：把 `ch_est` 拆成"提取"与"权重"两条 CB —— 权重占 3/4，burst 等的就是它**

原来 `gpu_lane_probe` 把估计器**两条命令缓冲都标成 `ch_est`**（提取一条、权重一条），
所以 `busy split` 只能说"ch_est 占 85% of busy"，**无法回答那 195 µs 的依赖份额在等哪一条**。
现在权重那条单独标为 **`ch_wt`**：

- `ocudu_metal_lane_probe.h`：`stage` 加 `channel_estimator_weights`（放在 `count` 之前，索引不变）
- `ocudu_metal_lane_probe.mm`：`stage_name()` 加 `"ch_wt"`
- `ocudu_metal_mmse_engine.mm`：`end_stage()` / `end_stage_async()` 增加 stage 参数（默认 `channel_estimator`），
  权重路径（`encode_run` 与 `encode_weights_only` 各两个出口）传 `WEIGHTS_STAGE`；
  提取路径保持默认标签。

**离线 replay 首读**（lane 内）：

```
ch_est=100.6us/lane (17% of busy, cbs/lane=1.00)   ← 提取（6 个 kernel）
ch_wt =390.5us/lane (67% of busy, cbs/lane=1.00)   ← 权重（相关+K1+apply+scatter+reformat）
eq_demap=88.8us/lane (15% of busy, cbs/lane=1.00)
```

⇒ **权重那条 CB 占 3/4 的核时间，而 burst 的均衡读 `gpu_h`/`gpu_ce`/`gpu_nv`、正等它跑完。**
所以 §48.194(g-nonies) 那 95% 的"依赖串行化"，**等的是权重的 CB，不是提取的 CB**——
这改变了下手的方向（之前只能猜）。

**判据已过**：三张网 `AB_ALL_RC=0`、七门 `ALL_GATES_RC=0`、Metal 自测 6/6、单测 13/13。

**⚠ 踩坑记录**：给 `end_stage` 加参数时我连续改错——先加了前置声明（C++ 不允许默认实参出现在定义之后），
又叠加了一次替换导致参数行重复，把文件改坏后**回退重做**（一次性、带 assert 的整批替换）。教训：
**同一文件的多处结构性改动要一次做完并留下断言**，不要分多轮叠加。

### 2.19 **主线状态钉死（Ubuntu + 一条干净腿）**

**提交 `08299c55a7`；`lib/` 无未提交改动。**

| 门 | 结果 |
|---|---|
| **Ubuntu**（`jwang@192.168.100.131`）全量构建 | **RC=0** |
| Ubuntu `ctest -L phy` | **164/164 通过（100%）** |
| 七门 `run_gates.sh`（含 `ratdev`）| **`ALL_GATES_RC=0`** |
| Metal 自测 | **6/6 OK** |
| CE 单测 | **13/13** |

**干净腿 `gnb_clean_0918_0918_1507`**（默认 yml，**无任何命令行参数**）：
配置读回 `cell=0: RLF thresholds … csi_dtx=300` ✓、契约 **7/7**、`Real-time failures 0/71287`、
`device_sigma2=2156=lanes`、`dropped=0`、**RLF=0、释放=0**。

**与既往腿并排（证明 MAC 改动对 PHY/GPU 零影响）**

| 腿 | lanes | resid | busy | gap | host | queue | RLF | rel |
|---|---|---|---|---|---|---|---|---|
| `ota_final_0949` | 1967 | 715.0 | 512.3 | 202.7 | 10.6 | — | 11 | 6 |
| `ota_queue_0957` | 2472 | 717.2 | 512.1 | 205.0 | 10.3 | 0.0 | 2 | 4 |
| `ota_queue_1247` | 1877 | 744.4 | 505.3 | 239.0 | 13.8 | 0.0 | 6 | 10 |
| `rlf_hi4_1350` | 13001 | 716.0 | 523.4 | 192.7 | 9.9 | 0.0 | 0 | 0 |
| `rlf_hi5_1439` | 10036 | 707.1 | 511.9 | 195.2 | 10.2 | 0.0 | 0 | 0 |
| **`clean_1507`** | **2156** | **719.9** | **515.8** | **204.1** | **10.6** | **0.0** | **0** | **0** |

⇒ residency 707–744、busy 505–523、gap 193–239、宿主 9.9–13.8 µs **全在长期波动带内**；
**queue 恒为 0.0**。主线基线未变。

### 2.18 **已落地：`max_consecutive_kos: 300` 写进 testbed 配置**（秒级 ping 尖峰问题收口）

`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 的 `cell_cfg.pucch` 段现在**显式带全 5 个 narrow-BW
资源值 + `max_consecutive_kos: 300`**（资源值照抄上游 `configs/cell_cfg_pucch_narrow_bw.yml`）。

**为什么必须带全 5 个**（不是缺陷，是上游 `83dcb80860` 的既定设计）：5 MHz 的 PUCCH 缩量以
`pucch_cfg == du_high_unit_pucch_config{}`（"用户没动过 PUCCH"哨兵，逐字段全等）为前提；
而 `max_consecutive_kos` 这类 **MAC/RLF 参数住在 PUCCH 结构体里**，设它就让哨兵为假 ⇒ 缩量不生效
⇒ 撞 50% PRB 校验被拒。上游为此提供了 `configs/cell_cfg_pucch_narrow_bw.yml`。
⇒ **以后跑腿不需要再敲那一长串命令行参数。**

**验证**：`gnb -c <yml>` 真跑路径（无 SDR 时走到 `AMF ... completed` 即代表校验通过），
`exceeds the 50%`/`Invalid configuration`/`not expected` 各 **0** 次；启动日志读回
`cell=0: RLF thresholds (consecutive KOs) dl=100 ul=100 csi_dtx=300`。

**同轮结论（三条腿）**：`RLF detected` 基线 11 → **0**、`UE Context Release` 10 → **0**、
最长连接 6–46 s → **168 s**；**`sinr_threshold=5` 是无效参数（已去掉）**——它的作用是"低于阈值
的 PUCCH 记为未解出"，只会让计数涨得更快（`uci_indication_selector.cpp:70-78`）。
残余 100 ms 级抖动**与连接无关**（那些间隔内 PRACH=0、RLF=0），且用户判断可能来自 CN/回程，暂不动。

### 2.17 **✅ 有效腿 `rlf_hi4`：`pucch.max_consecutive_kos` 100→300 消除掉线与秒级尖峰**

启动日志确认 `cell=0: RLF thresholds (consecutive KOs) dl=100 ul=100 csi_dtx=300`。

| 量 | `rlf_hi4`（300）| 基线 `1247`（100）|
|---|---|---|
| **`RLF detected`** | **0** | 11 |
| **`UE Context Release`** | **0** | 10 |
| **最长单条连接** | **143 s** | 6–46 s |
| 该连接 PUSCH | 12921（OK 11469，KO 11.2%）| — |
| PRACH | 17 | 16 |

⇒ **以前每次掉线都是 gNB 的 RLF 判定触发的**；阈值提到 300 后**整腿零 RLF、零释放**，
用户侧**秒级尖峰消失，只剩 100 ms 级抖动**——与用户观察一致。

**残余 2–4 s 间隔与连接无关**（该连接内最大 6 个间隔里 `PRACH=0 RLF=0`，期间 PRACH 总数 = 0）。

**303 次 max-reTx 不是退化**：299 次在临时 RNTI（Msg3）上，峰值 283 来自 `0x4605`——
**因为连接活下来了才累积出这些重传**。

**正确的调用（选项名以 `--help` 为准）**：
```
--cell_cfg.pucch.nof_cell_res_set_configs=1 --cell_cfg.pucch.resource_set_size=7
--cell_cfg.pucch.nof_cell_sr_res=7 --cell_cfg.pucch.nof_cell_csi_res=7
--cell_cfg.pucch.f1_enable_occ=true --cell_cfg.pucch.max_consecutive_kos=300
--cell_cfg.pucch.sinr_threshold=5
```
（前五个是**工作区**：绕开"RLF 参数破坏了 5 MHz PUCCH 缩量的默认判定"那个产品缺陷。）

### 2.16 **`rlf_hi` 腿作废：我的腿脚本把 CLI 参数吞了**（已修）+ 加了一道防线

用户按建议跑了 `rlf_hi`（`--cell_cfg.pucch.sinr_threshold=5 --cell_cfg.pucch.max_consecutive_kos=300`），
日志里**仍是** `Cause: 100 consecutive undecoded CSIs` ⇒ 旋钮没生效。

**根因是我的脚本**：`run_air_leg.sh` 把**任何含 `=` 的参数都 export** 成环境变量，
所以 `--cell_cfg...=300` 变成无意义的 shell 变量、**没进 gNB 的 argv**。
**"旋钮没效果"与"旋钮没送到"在日志里无法区分**——本次 session 第五次同一个教训。

**已修**
1. `run_air_leg.sh`：`--*` → argv；`OCUDU_*=` → 环境；其余**报错退出**（不再静默丢弃）；
   banner 打印 `gNB options : …`。
2. `lib/mac/mac_sched/rlf_detector.h`：启动时打印实际生效的阈值
   `cell=0: RLF thresholds (consecutive KOs) dl=… ul=… csi_dtx=…` ⇒ 以后任何腿都能**读回**它要测的数。

**已确认**（不靠运行猜）：`--cell_cfg.pucch.max_consecutive_kos` 是真实可达选项
（不存在的路径会被 CLI11 RC=109 拒绝，该路径被接受；绑定 `cli11_schema.cpp:1384`，
流转 `translators.cpp:1282`）。门：三张网 `AB_ALL_RC=0`、七门 `ALL_GATES_RC=0`、两配置构建门 RC=0。

**⇒ `rlf_hi` 那一腿的结论作废**（它什么都没测到）。重跑时**先确认启动行出现 `csi_dtx=300`**。

### 2.15 **ping/iperf 尖峰的最终定论 + 复现脚本已交付**（更正 §2.14 的 DTX 说法）

**定论：尖峰 = 随机接入（RACH）失败**。那些"5 次授权 / 0 PUCCH / gNB 放弃"的身份**全是 RACH 临时 RNTI**
（`tc-rnti=0x46xx`，每次 PRACH 一个新）⇒ 是 **Msg3 没送达**，不是"已连接 UE 静默"。

```
UE 掉线 → PRACH → 新 tc-rnti 授权 Msg3 ×5 → 全 crc=KO、SINR −17…−53 dB、无 PUCCH
        → gNB 放弃 → UE 换下一个 tc-rnti 再试 …… 期间无数据连接
        → 链路恢复后队列排空 = 用户看到的「RTT 从 1.5–4.4 s 线性下降」
```

**19 条腿、245 次接入：失败 160 / 成功 85；失败档 PRACH 功率一律 −70…−65 dB，成功档 −52…−8 dB**
⇒ UE 的 RACH **起始功率不足**，靠爬升才被检测到。**最早的 `legA_0917` 就有，不是回归。**

**交付物（已自证）**

| 文件 | 作用 |
|---|---|
| `wip/rachstorm_repro.sh` | 跑腿说明 + `judge` 入口 |
| `wip/rachstorm_judge.py` | 判读：PRACH ↔ tc-rnti 配对 → 每次接入成功/失败 → "已连上之后的失败重接入"次数与跨度 |

自证（5 条真实腿）：`legA_0917` **1** 次、`fused_0917_2241` **1** 次、`event_0918_0658` **2** 次、
`ota_final_0918_0949` **2** 次、`ota_queue_0918_1247` **5** 次（最长 24 s、烧 20 次 Msg3 授权）。

**⇒ 着力点**：① RACH 被检测概率（失败/成功两档功率完全不重叠）；② §19 的干扰（SIR≈0 dB，降 rx_gain 无效）；
③ 调度侧只能缓解。**注意 `prach` 参数当前没有在 yml 里暴露**，要调得改代码或加配置项。

**⚠ 同时发现一个未决的网络侧异常**（同一腿）：`ping` 起始 RTT **1937 ms** 线性降到 ~35 ms（约 2 s），
稳态每 **~1.4 s 出现 ~170 ms 尖峰**；`iperf3` 15.3 Mbps、**530 次重传**（集中在 8–9 s 那一段）。
但 **gNB 侧无对应停顿**（日志节奏 P99.9 = 16.76 ms，与其它腿同量级；`PUSCH t=` P99 1226 µs）。
⇒ 不在 gNB 宿主路径上（否则日志会显形），怀疑 **UE 调度/RLC 重传**、**核心网（AMF/UPF 在同机 UTM VM，
当时 `QEMUHelper` 占 ~6% CPU）**或主机/VM 调度。**需干净复测**：下次上机先记"无 gNB 时 UE→网关"的 ping 基线，
并在跑腿期间同步抓 ping，把两者分开。

### 2.12 下一件的观测已就位：**`gap: commit -> first command buffer starts (queue)`**（待上机）

**先否掉一个循环论证**：原计划量 `backend_stage_wait` 等了多久——它等于"提取还没跑完的那部分"
（因为 fence 等的就是提取 CB 完成，`提取.GPUEndTime <= burst.GPUStartTime` 恒成立），
测出来也不能据此"削 fence"。**依赖还在。**

**改测与依赖无关的那段**：lane 第一条 CB（= 提取 CB）**被 GPU 真正开始执行**比它被 commit 晚了多久
⇒ 归**命令队列**，不归依赖。判读：小 ⇒ 削**提取流水线**（其 GPU 约 430 µs）；大 ⇒ 削**提交/队列**。

实现 = 第三条序列；`close_lane()` 里读 `stage::channel_estimator` 那条 entry 的 `GPUStartTime`。
**踩坑**：不能在 commit 处读（pending 的 CB 报 0，序列会全空）——必须在完成后读。

离线全绿（单测 13/13、三张网、七门）；离线 replay 该值为 0.0，**空侧要等一条上机腿**。

### 2.11 收口腿 `gnb_ota_final_0918_0949`：**合格**，四腿结论一致 ⇒ 本轮"还剩什么"已定

**`0 / 66526 = 0.00000%` 失败、契约 7/7、干净机器、无遗留进程。**

| 腿 | lanes | residency | busy | **gap** | **entry→cb（宿主）** | 失败率 |
|---|---|---|---|---|---|---|
| `ota_k0a_dev_0918_0911` | 892 | 735.7 | 505.8 | 229.9 | — | 0.00000% |
| `ota_gapsplit_0918_0940` | 1418 | 704.8 | 487.6 | 217.2 | **10.8** | 0.00000% |
| `ota_gapclean_0918_0944` | 2232 | 731.4 | 515.2 | 216.1 | **11.2** | 0.01753% ← **环境争用（上个 gnb 没停），不可引用** |
| **`ota_final_0918_0949`** | **1967** | **715.0** | **512.3** | **202.7** | **10.6** | **0.00000%** |

**本轮收口结论**
1. **宿主每跳路径不是 lane gap 的来源**（`entry→cb` ≈ 11 µs = gap 的 5%）⇒ **第二件与 Step 4 都别按原样做**
   （读标量 0.1 µs；后端 GPU 占用率仅 0.9%）。
2. **剩余 ~192–205 µs 归 `backend_stage_wait` + 命令队列排序**。
   ⚠ **不是前端 fence**：四条腿都是 `front_end fence signals=0/waits=0/skipped=0`（默认关，没 arm）。
3. **下一件开工前先补一个自洽观测**（直接量那条 fence 等了多久，或做"强制不编 fence"的 A/B），
   别再照推算施工。

**⚠ 新纪律（本轮踩到）**：**跑腿前先 `ps aux | grep [g]nb` 确认没有遗留进程**。
留着的那个进程让 `ota_gapclean` 失败率超预算（0.01753% > 0.0116%），那一条腿的可靠性判据作废。

### 2.10 上机腿 `gnb_ota_gapsplit_0918_0940`（带 §2.9 观测）：**宿主段只占 gap 的 5%；另一半观测是我写错的**

1418 lanes、56502 时隙、契约 7/7、失败 0/56502。

| 量 | 值 |
|---|---|
| lane residency / busy / **gap** | 704.8 / 487.6 / **217.2 µs** |
| **`gap: stage entry -> extraction commit`（宿主）** | **10.8 µs**（median 8.9、p95 19.5）|
| `[ul_channel_estimation]` | 261.4 µs（上一腿 259.5，持平）|

**✅ 成立**：宿主从"估计器开工"到"投出 lane 第一条 CB"只 10.8 µs = gap 的 **5%** ⇒ **宿主每跳路径不是 gap 的来源**；
削宿主端（第二件 / Step 4）在本负载下确认无收益。

**❌ 我写错的**：同一版报的 `gap: front-end done -> stage entry` = **576.8 µs**，比它要解释的 217.2 µs **还大 2.7 倍**，
自相矛盾。错因：`publish_front_end_done_now()` 挂在 DFT 引擎 `commit_front_end()` 上，**每 slot 提交多个批次**，
再叠加"读全进程最后一次" ⇒ 前端完成时刻与这条 lane 的 slot 无对应关系。
按时隙配对的同一量**已经存在**（`[ul_channel_estimation]`）。**已删除该读数**，并在
`ocudu_metal_lane_clock.h` 写明为什么不要在那里量。**离线全绿**（三条网 `AB_ALL_RC=0`、七门 `ALL_GATES_RC=0`、单测 13/13）。

**⇒ 剩余候选**：lane gap 里约 **206 µs** 归 **lane burst 开 CB 时编的两条 fence（`front_end_wait`/`backend_stage_wait`）
与命令队列排序**。要动它需要**新的、能自洽的观测**（直接量 fence 实际等了多久，或做"强制不编 fence"的 A/B），
**不要再照着一个没配好对的读数去推**。

### 2.9 新增观测：把 lane gap 拆成"宿主那两段"（**已落地、离线全绿、待上机**）

新增**头文件 + 6 处挂点**（纯诊断，不改语义；判据 = 三条网 + 七门 + 单测 13/13 **全绿**）：

| 文件 | 内容 |
|---|---|
| **`lib/phy/metal/ocudu_metal_lane_clock.h`（新）** | `lane_host_clock`（thread_local）：`mark_stage_entry()` / `mark_extraction_commit()`，记 `front_end_wait_us`、`handover_us`；外加一个**跨线程**的 `publish_front_end_done_now()` / `since_front_end_done_us()`（atomic 读数，不是同步原语）|
| `port_channel_estimator_metal_mmse_impl.cpp` | 进入 `apply_fd_td_estimation_stage()` 时 `mark_stage_entry()` |
| `ocudu_metal_mmse_engine.mm` | 提取 CB **commit 之后** `mark_extraction_commit()`（lane 的第一条 CB 从这一刻才存在）|
| `ocudu_dft_metal_engine.mm` | 前端 `commit_front_end()` 里 `publish_front_end_done_now()` |
| `ocudu_metal_lane_probe.mm` | `close_lane()` 收两个读数；报告多打两行 |

**新读数（两行）**：
```
[ul_gpu_lane] gap: front-end done -> stage entry (host)      <- 宿主从"前端做完"到"估计器开工"
[ul_gpu_lane] gap: stage entry -> extraction commit (host)   <- 宿主从"估计器开工"到"投出 lane 的第一条 CB"
```
`gap = 上面两段（宿主）+ 剩余（fence front_end_wait/backend_stage_wait 与命令队列）`。
离线 replay 已能看到第二段（单跳 61.8 µs；第一段因 replay 不驱动真实前端而无样本）。

**上机就看这两行**：若两段之和占 gap 的大头 ⇒ 目标是**宿主投递延迟**（调度/编码），
若两段都很小、gap 仍在 ⇒ 目标是**两条 fence 的等待**。**这就是选 A 的目的：先把该削哪一段定下来。**

### 2.8 **Step 4 的前提也量了：后端 GPU 占用 0.9% ⇒ 并发在本负载下不是瓶颈（先别做）**

窗口 51.0 s（01:11:19 → 01:12:09）：

| 量 | 结果 |
|---|---|
| 后端 GPU busy 合计 | 892 × 505.8 µs = **0.451 s ⇒ 占用 0.9%** |
| 前端 GPU busy 合计 | 12871 × 422.4 µs = **5.437 s ⇒ 占用 10.7%** |
| lane 到达节奏 | 中位周期 **19.8 ms ≈ 40 个槽** |
| 单 lane | residency 735.7 = busy 505.8 + **gap 229.9** |

⇒ 一条 lane 的 GPU 时间是 505.8 µs，两条 lane 之间隔 ≈19.8 ms ⇒ **队列里几乎不排队**。
并发的收益前提是"资源占满、请求排队"，这里离饱和还有上百倍余量 ⇒ **Step 4 在本负载下不会兑现**。

**229.9 µs 的 gap 仍在 lane 的临界路径上**（不是"占用率的一个点"，是这条 lane 的真实延时），
它的两个已知来源是 ① 宿主"DFT 完成 → 投递 burst"那段（与 259 µs 窗口重叠）、② burst 开 CB 时
`front_end_wait` / `backend_stage_wait` 两条 fence 覆盖的等待。**两者都还没有被单独计时过**——
要动它必须先补能把 ① 和 ② 分开的观测。

### 2.7 那 259 µs 已量清 ⇒ **第二件的原始理由不成立，建议改做 Step 4**

**`[ul_channel_estimation]` 的定义**（`include/ocudu/support/executors/ul_pipeline_probe.h`）：
`ce_ns = record_ce_end() - record_t2f_end()`，即**"整隙 OFDM 解调完成"到"信道估计做完、开始处理数据"的宿主墙钟**，
覆盖 下 PHY 通知 → PUSCH 作业被宿主线程取起 → estimator（含 K0-a）→ notifier → `process_data()` 入口。
**它既不是"宿主等提取"的计时器，也不含 GPU 时间。**

| 分项（本腿） | 值 |
|---|---|
| `[ul_channel_estimation]` | 259.5 µs |
| `[mmse_time_sum] pre + stage`（宿主在 K0-a 的**实际工作**）| **0.93 µs** |
| `[mmse_time_sum] sigma2`（读/算比值）| **0.0 µs** |
| `gpu_wait` / `defer_wait` | 335.3 / 652.1（上一臂 320.7 / 652.4，持平）|

⇒ **259 µs 里几乎没有 K0-a 的份额**；§48.193(c) 那个"~94 µs"从未在本口径下被测到过。

**因此第二件（`hop_stage` 接手）不要按原样做**：它原本的收益是"宿主不再读提取的标量"，而那是 0.1 µs。
而且**依赖本身还在**——相关矩阵核经 `corr_stage::sigma2_dev` 从**提取那条 CB 的输出**读比值，
所以相关必须排在提取之后；同一条 CB 只能靠"相关当前缀"，而前缀要在编译期知道比值、比值又由该 CB 后半段产生 ⇒ **还是环**。

**真正还剩的两块**（都不是 K0-a）：
1. **lane `gap` = 229.9 µs** —— GPU 等宿主投喂（宿主投递延迟）；
2. **`cbs/lane = 3.00`** —— `busy split` 给出分解：**CE 占 2.00**（提取 CB + 权重 CB）、`eq_demap` 占 1.00。

⇒ **建议把"第二件"改为 Step 4**（lane 并发 `max_in_flight` 1→N），并把"CE 的 2 条 CB 合成 1 条"
作为备选——后者唯一可行形式是**把相关的编码挪进提取那条 CB**（几何在提取编码时已知，比值由同一 CB 的
`mmse_pilots_power` 产出，核本来就要求这种先后），但那要求重构"宿主何时构建相关描述符"，
是一件有真实风险、需要独立判据的改动，**不要**顺手动。

## 3. 本 session 的门（**全绿**；串行、空闲机器）

| 门 | 结果 |
|---|---|
| 三张网 `run_ab_all.sh <ref> 27` | 严格 **27/27**；有界（3 模式）**26/27 逐字节、最差 8 字节、flips 0**；CPU **27/27** ⇒ `AB_ALL_RC=0` |
| 序 A/B `ab_fused_lane.sh 27` | 4 路线 × 27 × 3 序 ⇒ `FUSED_AB_RC=0` |
| **七门** `run_gates.sh` | `sig2`/`k0dm`/`k0d`/`ydev`/**`ratdev`**/`k1`/`combos` ⇒ `ALL_GATES_RC=0` |
| CE 单测 | 13/13 |
| 逐位相同探针 | 27 捕获全部跳：`hops=27 mismatch=0` |

| 两条配置构建门 | `/tmp/build_nometal`（**重新 configure 过**，`ENABLE_METAL_CHEST=OFF`，正确地不编译任何 `.metal`）与 `/tmp/build_nostats`（`ENABLE_METAL_STATS=OFF`，新 shader 正常编译）各 `cmake --build --target gnb` ⇒ **RC=0** |
| `run_metal_engines.sh` | **6/6 OK**（含 `port_channel_estimator_metal_mmse_unit_test`）|
| Ubuntu | `jwang@192.168.100.131:~/work/ocudu` 已更新到 `d49f3a16fd`，全量构建 **RC=0**、`ctest -L phy` **164/164** |
| **上机腿** | **`gnb_ota_k0a_dev_0918_0911`**（默认配置）：契约 7/7、失败 0/46736、警告 0、`device_sigma2=892=hops_gpu`、`[mmse_time_sum] sigma2=0.0us`、lane busy 505.8 µs（与上一默认臂 505.9 持平）⇒ 见 §2.6 |

⇒ **本 leg 的门全部跑完，全绿。** 可以上机。

**顺带修掉门自己的一个记账缺陷**：`ratdev` 首跑出过一次 `byte-identical=26 … MISMATCH:` —— 并行期假不匹配，
串行复核清掉了（退出码对），但**总结计数没回补**，行文自相矛盾。已修（清掉一个候选就 `SAME+1`/`BYTEMIS-1`），
连跑 6 次现在都是 `byte-identical=27 … PASS`（其中 2 次 `rechecked=1`）。
另测可信度：**27 捕获 × 6 次 × 2 路线 = 324 次，两条路线各自逐字节稳定、A/B 逐字节相同（0/27 不一致）**
⇒ 那次是并行期的机器级现象，不是本改动的间歇缺陷；但**这条门必须在空闲机器上跑**。

---

## 4. 硬约束与口径（重要，别再踩）

1. **"同样的表达式" ≠ "同样的舍入"**：任何"设备算一个 float 商/和、宿主或参考也算同一个"的设计，
   **先看那个文件的编译标志，再测，别推**。放大系数大的量（对角加载、相关矩阵）必须 `-fno-fast-math`。
2. **严格编译是文件级的**：只把有契约的核放进严格文件，别把邻居一起拉进去。
3. **跨语言共享的结构按 `offsetof` 钉住**，`sizeof` 不够（同尺寸换名是隐形的）。
4. **交设备地址交 BASE + 槽号**，不要交元素地址。
5. **内核新写的每个槽都要在 `wrap()` 的长度里**。
6. **`geom.ok` 在空气上恒真**；单测里必须用设备驻留网格才测得到门。
7. **同一台机器上不许同时跑两套 GPU 门**（本 session 又验证一次：并发时严格网每次恰好错一个捕获，
   单独串行重跑全绿）。**门必须在 metallib 构建结束之后跑。**
8. 沿用：每 lane 一次同步是硬约束；宿主在 lane commit 之前要读的东西不许融合；`thread_local` 析构早于 `atexit`；
   `--log.filename` 不建父目录；`[metal_stats]`/`[ul_gpu_lane]`/`[mmse_time_sum]` 走 **stderr**；两条配置构建门必须串行。

---

## 5. 下一步

### 5.1 **第二件：`hop_stage` 接手（§48.193(d)）——先读这段"试过、退回"的记录**

> ⚠ **本 session 试过"把宿主的标量读取按需化"，退回，未提交。** 想省掉的是宿主读
> `gpu_ls_sigma2[0..1]` 那一步（在设备商可用后它看起来是纯冗余）。做法是把 `pilots_power` /
> `sigma2` / `sigma2_rel` 三者的计算下沉到"谁需要谁算"，并用 `host_builds_std` 当闸门。
>
> **失败原因（值得记住）**：`host_builds_std` 是**宿主的建设决定**，而 "A 的对角加载从哪来" 是
> **另一个独立决定**。两者一旦解开耦合就会出现"设备建 A、但没有设备商可用"的组合 ——
> 单元测试正是这种情形（`grid_fake` 不是设备驻留 ⇒ 设备 LSE 不跑 ⇒ `device_sigma2_valid=0`，
> 但引擎在跑、槽位仍由设备构建）⇒ **A 以 sigma2=0 建成** ⇒ Test 3 在 SNR −5 dB 退 >1.5 dB。
> 中途还发现一个既有的隐藏耦合：`std_slots_filled` / `dev_corr_std_merged` **没有包含
> `engine_ready`**（无引擎时也宣称"设备会填"）。以前无害（宿主无条件算自己的数组），
> 一旦用它当闸门就立刻致命。
>
> ⇒ **结论：想要这个优化，必须同时收紧"设备建 A ⇒ 必须用设备商"这条不变量**（或保证设备建 A
> 只在 `device_sigma2_valid` 时发生）。这是一件**独立、有真实风险**的改动，不要和 `hop_stage`
> 混在一起做。本 session 的收益也因此**没有被兑现**：宿主那次读取仍在。
>
> 若要做，建议的顺序：① 先只把 `engine_ready` 补进那两个 flag（**独立的小修，本身是对的**）；
> ② 再引入 `device_builds_std`，把"A 由谁建"和"加载从哪来"合并成**一个**决定；
> ③ 判据：CE 单测 13/13（尤其 Test 3/9/12）**且**设备商 A/B（`ratdev`）**且**三张网。
> 不要在 ② 之前动 ③ 的任何一项。

### 5.1-bis **第二件本体：`hop_stage` 接手（§48.193(d)）**

设计不变：**提取不能进 lane burst**（`event` 序下权重在自有 CB 里、那条 CB 编的 fence 正是 lane burst 要等的信号 ⇒ 成环）。
正确做法 = **提取与权重共用一个命令缓冲**（`build_pilots_lse(s, fuse)` + `take_hop_stage()` +
`mmse_engine_impl::{hop_stage, hop_open}`，段间 `memoryBarrierWithScope`；hop 仍只有一次提交、不引入 fence）。

**本 session 让它变得更有价值**：设备商现在与宿主逐位相同 ⇒ 宿主**不再需要**为了 A 的对角加载去读
`gpu_ls_sigma2[0..1]`。**建议的做法**：把 `pilots_power` / `sigma2` 的计算**下沉到真正需要它们的地方**，
并且只在 `sigma2_from_device == false` 时才读设备标量（`stats_estimator->consumes_pilots()` 为 false 时，
`stats_in.sigma2` 只喂相关矩阵，而相关矩阵已由设备加载）。

**判据**
- CE 单测 **Test 13 全绿** **且** 新增 Test 14（融合 vs 不融合逐字节 + `k0a_fused` 计数断言）；
- **Test 14 必须自建设备驻留网格**（`grid_fake` 会静默关门，§2.1）；
- 离线：三张网 + 序 A/B + **七门**（含 `ratdev`）逐字节。

### 5.2 其余

1. **补门**：两条配置构建门（串行）→ `run_metal_engines.sh` → Ubuntu `ctest -L phy`；然后考虑提交 + 打 tag。
2. **Step 4**：lane 并发 `max_in_flight` 1→N（跨 lane 重叠；后端 lane `gap` 仍有 ~240 µs）。
3. （可选，覆盖 rem≠0）merged tail 的设备构建：判据 = k0d 门 + 三条腿（§48.193(e).2 的 S-7f-5v 坑）。
4. 顺带：用新口径确认 S-7g-13（`OCUDU_UL_RX_SYMBOLS=4`）+ 干净环境复核失败率。

---

## 6. 开关台账（默认值从代码核实）

`OCUDU_CE_LANE_ORDER`（**默认 `event`**；`wait`/`burst` 逃生门）、`OCUDU_CE_FUSED_BURST`（旧别名）、
`OCUDU_DFT_OPEN_BLOCK`（默认开）、`OCUDU_UL_RX_SYMBOLS`（unset/0 = whole-slot）、`OCUDU_UL_FRONTEND_FENCE`（默认关，别开）、
`OCUDU_METAL_GPU_TIME`（opt-in）、`OCUDU_CE_CPU_CE/CPU_LS/GPU_INVERT/CPU_INVERT/CORR_DEV/SPLIT_TAIL/DEV_SIGMA2/DEV_Y`（A/B 或逃生）、
**`OCUDU_CE_K0A_RATIO_DEV`（默认 1；`=0` 回到宿主商）**、**`OCUDU_CE_K0A_RATIO_CHECK`（探针）**。
**`OCUDU_CE_K0A_FUSE` 仍不在树里**（第二件才加）。

---

## 7. 回来的第一个动作（TL;DR）

1. `git log --oneline -3` 确认 HEAD = **`74ba4c5251`**；`git status --short lib/` 应有 8 改 + 1 新；
   `cmake -P build/build_info.cmake && cmake --build build --target gnb -j 14`。
2. 读 **§48.194**（尤其 (a) 的 fast-math 表、(b) 为什么拆文件、(c) 两个真缺陷、(e) 新门）。
3. 决定：**先补门并提交这批改动**（推荐：两条配置构建门 + `run_metal_engines.sh` + Ubuntu），
   或直接开第二件（§5.1，Test 14 记得自建设备驻留网格）。
4. 上机腿用 `sudo -E bash doc_chinese/full_gpu_chain/wip/run_air_leg.sh <label> [OCUDU_*=…]`（需要你手动跑），
   跑完 `air_leg_report.sh` 判读。
