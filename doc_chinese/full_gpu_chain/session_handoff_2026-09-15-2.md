# Session Handoff Memo — 2026-09-15-2

> **性质**：状态快照，**冻结不改**。需要修正/更新时写进活文档
> `doc_chinese/full_gpu_chain/s2_full_chain_design.md`，不要回头改本文件。
>
> **代码基线**：起点 `cbd2e11199`（= 上一份 handoff），**终点/HEAD = `c42c134318`**；
> `gnb` 二进制戳 = **`2068d8a56f`**（**已实测通过**的那一腿，可直接继续用）
> **活文档**：`doc_chinese/full_gpu_chain/s2_full_chain_design.md`（8191 行），本会话写入 §48.98–§48.124
> **横向状态视图在 §48.84**（每改变模块落点/缓冲归属都要回来更新它）；
> **本会话新增的三条一等条目**：§48.84(a0) 首要目标与三条判定规则、§48.109 先复核再采用、
> §48.118(a) 用户测量期间 agent 停机

---

## 0. 一句话现状

**从 IQ samples 到 demapper LLR，模块落点层面已经全部在 GPU 上**（K0-a 本会话落地）；
`[ul_pipeline]` median **977 µs —— 首次低于 1 ms 时隙**；实时失败 **0.0324%/时隙**（基线 0.1148%）。
**剩下的是"CPU 胶水"**，下一处（#2）的施工图与代码形态已就绪（§48.123/§48.124），**尚未动一行代码**。

---

## 1. 目标与工作流（**用户定调，优先级最高**）

> **首要目标（用户三次强调）：从 IQ samples 到 demapper LLR 全部走 GPU path。**

**工作流（用户 2026-09-15 定）**：
1. **先把功能模块搬进 GPU**（模块之间**允许**用 CPU 胶水串联）；
2. **每一步搬完 → 离线门禁 → 停下来交给用户做手机 OTA**，确认没把整条通信链路搞坏；
3. 确认无误后，**再逐个消灭 CPU 胶水**（让 GPU 内部流水线变长）；
4. **每消灭一处胶水，同样要 OTA 验证**。

**硬约束**：**性能不能差到手机 attach 不上 / ping、iperf3 跑不起来**（延迟太大手机接不上）。
性能**不是**模块落点的判据；精度**不能**成为把模块退回 CPU 的理由。

**三条判定规则（§48.84(a0)，任何建议先过这三条）**：
1. **CPU 侧的中间环节是要"消失"的，不是要"优化"的**——凡"让主机这段跑得更快/等得更少"的改动都不是目标下的工作，除非它是"把该环节搬上 GPU"的必要前置；
2. **唯一例外是硬约束**：某段**原理上无法进 GPU** 才允许留在 CPU，**且必须写明这条硬约束是什么**；
3. **性能债可以记账，但不能用它换方向**（不构成把模块退回 CPU 的理由）。

**一条操作纪律（§48.118(a)，用户明确要求）**：
> **用户做上机测量期间，agent 不跑任何会干扰测量的任务**（不跑 980 抓包门禁、不跑 GPU/CPU 重负载），
> 只做纯文件/文档操作与轻量读取。**上机节奏由用户掌控。**

**另一条操作纪律（用户 2026-09-15 追加）**：
> **读代码/注释/文档一律带怀疑，先复核再采用**（§48.109）。注释与文档只能**提出假设**，不能**定案**；
> 任何影响施工方向的事实必须有可执行依据（代码位置/实测数字/能证伪的实验）。
> **本会话被这条救了三次，其中一次纠正的是我自己刚写下的结论。**

---

## 2. 当前代码落点（详见活文档 §48.84）

| 段落 | 模块 | 在哪 |
|---|---|---|
| 前段 | RF 收发（B200/UHD） | CPU/USB（**输入**，不算中间环节） |
| 前段 | OFDM 解调（FFT）→ 频域网格 | **GPU（Metal DFT）** ✅ `--expert_phy.pusch_dft_type metal` |
| 前段 | DMRS 导频提取 / EPRE / LS / CFO（→ `pilots_lse_view`） | **GPU（K0-a，本会话落地）** ✅ |
| CE | K0-d 相关矩阵 `A`/`R_hp` | **GPU**，且**已并进引擎那条 CB**（胶水 #1，本会话） |
| CE | K1 反演 / K2a 权重 / K2b 应用 / K3 重排 / K4 噪声 | **GPU，同一条 command buffer**（真流水） |
| CE | 主机把设备 LSE **拷进 y/qy** | **CPU 胶水 ← 下一处要消灭的（#2）** |
| CE | 主机把 pre-stage **又算一遍**（结果被设备覆盖） | **CPU 胶水**（与 #2 同源，见 §48.123(c) 第 6 条） |
| CE | `sigma2` / FD 平滑 / EPRE | CPU（**下一批**） |
| CE | `unpack gpu_h`→`grid_est`、RSRP/noise/TA 统计 | CPU，**不在 LLR 路径上**（只为主机侧 CSI 数值） |
| 等化/解调/LDPC | 全部 | **GPU** ✅ |

**关键开关**（都必须保持可用）：

| 开关 | 作用 |
|---|---|
| `OCUDU_CE_CPU_LS=1` | 强制主机做 CE 输入级（K0-a 的逃生口/A-B） |
| `OCUDU_CE_GPU_INVERT=0` | 强制主机反演（注意：**`0` 现在才是"关"**，见 §48.101(b)） |
| `OCUDU_CE_CPU_INVERT=1` | 同上，从另一侧 |
| `OCUDU_CE_CORR_DEV=0` | 关闭设备建矩阵 |
| `OCUDU_CE_SPLIT_TAIL=1` | 用 split 代替 merged（**本会话修好了它的寻址雷**） |
| `OCUDU_CE_LS_CHECK=1` | 打开导频级容差探针（打印 `[ls_check]`/`[ls_sym]`） |
| `OCUDU_CE_DEV_INVERT=1` / `OCUDU_INV_RL=1` | 旧实验路径，**已知不可用，勿碰** |
| `OCUDU_INV_TGX`/`OCUDU_INV_TGY` | K1 线程组几何（默认已是实测最优 64×16） |

---

## 3. 本会话的净收益（已提交、已上机验证）

| 提交 | 内容 | 效果 |
|---|---|---|
| `63a15a7b9c` | **K1 回 GPU**：`stage_engine_group` 单门化 + K0-d `r_hp` 行跨距 1/9 缺陷 + corr 内核 fast-math 1 ulp | 设备 K1 三条抓包全对 |
| `8cf53a90f8` | `combos` 门禁（10 组开关）+ 证明 split 退化是既有的 | 开关语义钉住 |
| `01ea670412` / `387c5c55cc` / `256f2b702a` | `r_hp_nz` 实测、抓包门禁脚本落库、OTA 判读工具 | 证据可重跑 |
| `c62bf6e367` / `0382b6998d` / `e3ae03da49` | 更正"K1 慢是 barrier"的错误归因（真因=**线程组几何**）；`gpu_path` 标注更正 | — |
| `cca915055e` | **K0-a 落地**：CE 输入级上设备，**默认开** | 上机确认 |
| `d835b55549` | 修我引入的 **15580 次零拷贝重映射**（wrap 用每跳尺寸而非容量） | 上机确认归零 |
| `93410716cd` | **胶水 #1 消灭**：K0-d 并进 K1..K4 同一条 CB | `cbs/lane` 3.44→**3.00** |
| `3417703ba3` | **split 寻址雷**：引擎漏了 `sys_offset` | 单测 16 PASS/0 FAIL |
| `2068d8a56f` | 修我引入的 `device_corr_builds` **计数器回归** | 上机确认 5160/11029 |
| `5c1eaaf89d` / `c42c134318` | 胶水 #2 的施工图 + 代码形态（**未动代码**） | 下轮可直接施工 |

**最后一次上机（`2068d8a56f`）**：
`[ul_pipeline]` median **977.0 µs**（<1 ms）、`[ul_channel_estimation]` median **210.9 µs**、
`mean total` **33.0 µs**、`gpu_path` **17.8 µs**、`gap` **105.8 µs**、`cbs/lane` **3.00**、
`device_corr_builds` **5160/11029**、`corr_build_fail` **0**、
实时失败 **36/111 s = 0.0324%/时隙**（基线 0.1148%，**好 3.5×**）、
**0 崩溃 / 0 USB 错误 / 0 零拷贝告警**、上行 **12.76 MB**。

---

## 4. 已实测确认的事实（**不要再重新验证**）

### 4.1 上机基线（判据的分母是**时隙**，不是 hop）
| 腿 | 时间跨度 | 时隙 | `Real-time failure in RF` | 每时隙 |
|---|---|---|---|---|
| 基线 `d8e23f67d1`（主机 K1） | 143.7 s | 143708 | 165 | **0.1148%** |
| 第 2 腿（设备 K1, 32×4） | 192.1 s | 192145 | 37 | 0.0193% |
| 第 3 腿（设备 K1, 64×16） | 52.1 s | 52124 | 5 | 0.0096% |
| 本会话 K0-a 后 | 95 s | 95000 | 25 | 0.0263% |
| 本会话最终（`2068d8a56f`） | 111 s | 111000 | 36 | **0.0324%** |

> ⚠️ **上一份 handoff §4.4 写的"`Real-time failure in RF = 0`"与日志不符**：按 `--log.filename` 实测是 **165**。
> 另外**不要用 hop 数当分母**（hop 只在流量连续时才近似时隙）——这个错误我在 §48.105(a) 犯过并已更正。

### 4.2 判据与工具（已固化，可重跑）
```bash
M=lib/phy/upper/signal_processors/channel_estimator/metal
$M/capture_gates.sh k0d 10     # K0-d 等价性：全部发布字节逐字节一致（最硬的离线门禁）
$M/capture_gates.sh k1  10     # 设备 K1 vs 主机 K1：判决一致
$M/capture_gates.sh combos     # 10 组开关组合（含 split 四组，现已恢复门禁）
```
- **`max|dSINR|` 只作参考、不作门禁**：并行跑会被污染（同一份代码能从 7.3 跳到 48.9 dB）；
- **`ul_chain_replay` 在 10 分片下会静默给出错误结果**（同配置两次跑 **39/980** 条不同）——
  `capture_gates.sh` 已对每个不一致做**串行复核**（报告 `retried=`）；**任何差异先串行复核再下结论**；
- **`ctest -L phy 162/162` 不含信道估计器单测**（它未注册进 ctest，见 §48.117）。
  单测要**显式重建**：`cmake --build build --target port_channel_estimator_metal_mmse_unit_test`
  （metal 单测**不在 `all` 目标里**）。当前状态：**16 PASS / 0 FAIL**。

### 4.3 本会话修掉的三个"雷"（都已验证，别再怀疑）
1. **K0-d `r_hp` 行跨距**用 `p.Ns`(504) 而非 `p.Ls`(54) ⇒ 只写 1/9；**修**；
2. **corr 内核 fast-math** 让 A 差 1 ulp，被 cond₂(A)≈2e4 放大成 ~1% 的 W/h ⇒ 该内核加 `-fno-fast-math`；
3. **split 尾块寻址**：`stage_engine_group`/`unpack_engine_group` 都带 `sys_offset`，**引擎调用传基址**⇒
   尾块被算在系统 `[0, n)` 上。**修**（`engine_run` 接收 `st`+`sys_offset`，传偏移后的槽位基址）。

### 4.4 关于"哪些模块需要逐位复刻"的判据（§48.110，**搬运任何模块前先算**）
> 估该量到输出的**放大因子**：≈1（线性、良态）⇒ **容差验收**即可；
> 有 cond 量级的放大 ⇒ **必须逐位复刻**（并加进 `IEEE_MATH_SOURCES`）。
> 例：K0-d 的 A 被 cond₂(A)≈2e4 放大 ⇒ 逐位；K0-a 的导频经 `h = W·y` 线性进入 ⇒ 容差。

---

## 5. 未完成的事（**本会话没做完的**）

### 5.1 胶水 #2（**唯一在计划内的下一步**，施工图已就绪）
**现状**：同一份导频被**算两次、多拷两次**
```
K0-a 设备内核 → gpu_ls_out →（主机拷回）→ pilots_lse_view →（主机再拷）→ gpu_y/gpu_qy
                                ↑ 主机 pre-stage 又算一遍，结果被覆盖 = 纯浪费
```
**目标**：设备内核**直接写 `gpu_y`/`gpu_qy`**，主机零拷贝。
**施工图**：§48.123（6 条必核实项 + 两处"有意保留"）、§48.124（代码形态：索引映射已核实、
接入点选在 `stage_engine_group()` 的理由、scatter 内核签名、回退、判据、**新会话第一步**）。

### 5.2 其余账本项（见 §48.84(f) 与 §48.126 之前各节）
- **CE 的主机重算**不能立刻删（`pilots_lse_view` 仍被 `estimate_sigma2` 与 RSRP 统计读取）——
  要等 `sigma2` 上设备时一起做；
- **matrix flavor 的 `qy`** 仍是主机 staging（**有意保留**，四元交错布局需单独映射）；
- `EPRE` / `sigma2` / FD 平滑仍在主机；
- `unpack` + CSI 统计在主机，但**不在 LLR 路径上**；
- **信道估计器单测注册进 ctest**：**用户已定为目标完成后的后续任务**；
  且必须先修 split 缺陷（**已在 `3417703ba3` 修好**，所以现在具备注册条件了）。

---

## 6. 新 session 的**第一个动作**

```bash
cd /Users/jiachengwang/dev/ocudu
git log --oneline -1                        # 应为 c42c134318
strings build/apps/gnb/gnb | grep -E '^[0-9a-f]{10}$' | head -1   # 应为 2068d8a56f
git status --short | grep -vE "configs/|^\?\?" || echo clean
```
然后读活文档三节：**§48.124（胶水 #2 代码形态，含 (g) 新会话第一步）+ §48.123 + §48.107**，
按 §48.124(c) 写 scatter 内核、按 (d) 接入、跑 (f) 的离线判据，**然后停下来交用户做手机 OTA**。

**顺序纪律（用户工作流）**：实现 → 离线门禁 → **停，交用户 OTA** → 确认 → 下一处胶水。

---

## 7. 本会话的方法论教训（**最贵的几条**）

1. **"与本工作无关"只回答了"谁弄坏的"，没回答"要不要修"**（用户点出来的）。
   我把前者当成后者的答案，把一个**红着的门禁**放行了——而它指向的缺陷是真实的（split 抓包 6.24→3.15 dB）。
   **一个红的测试/门禁 = 一个真缺陷。**
2. **门禁绿之前，先确认它测的是被测对象**：`ctest -L phy 162/162` 绿了很久，但**信道估计器单测根本没注册**，
   对 CE 是**空的**。同类错误本会话出现两次（另一次：980/980 门禁曾因主机 staging 覆盖设备输出而**测的是主机 vs 主机**）。
3. **观测手段不能扰动被测对象**：我用探测脚本 `rm` 了用户正在写的日志（§48.99(c-1)）；
   后来用户明确要求"测量期间停机"（§48.118(a)）。**探"环境是否就绪"的动作不能有副作用。**
4. **注释与文档会把人带偏，包括我自己写的**：三次被复核救回——
   K1 的 barrier 归因、`average` 策略的猜测、以及**我自己上一轮写下的"多层缺口"**。
5. **新路径必须带上计数器**：`device_corr_builds` 在改成前缀形态后读 0——**工作搬走了、计数器留下了**。
   而它正是验收判据读的数（§48.83(e) 的教训，这次是已有计数器没接上）。
6. **同一份数据在三处用同一套 stride 寻址时，任何一处漏偏移都会静默错**（split 雷；
   与 §48.92/§48.94 的"写者/读者理解不一致"同源）。
7. **并行跑出的差异先串行复核再下结论**：`ul_chain_replay` 高并发会**静默给错**（39/980）。
8. **不确定就画图**：胶水 #1 与 #2 都先画了槽位/队列/时序图 + 真值表再动手（§48.83(c) 的规矩）。

---

## 8. 关键文件

| 文件 | 作用 |
|---|---|
| `.../metal/port_channel_estimator_metal_mmse_impl.{h,cpp}` | **主战场**：`stage_engine_group`、`run_engine_blocks`、`engine_run`、`build_slots_on_device`、K0-a 的设备块 |
| `.../metal/ocudu_metal_mmse_engine.{h,mm}` | 引擎：`run_async`（含 K1 与 **corr 前缀**）、`build_pilots_lse`、`build_correlation`、`mmse_inv_threadgroup` |
| `.../metal/ocudu_mmse_pilots.metal` | **K0-a 三个内核**（提取+LSE / CFO / CFO 补偿） |
| `.../metal/ocudu_mmse_corr.metal` | K0-d（**唯一需要逐位复刻**、唯一进 `IEEE_MATH_SOURCES` 的内核） |
| `.../metal/ocudu_mmse_inv.metal` | K1（块 Gauss-Jordan；**几何 64×16 是关键**） |
| `.../metal/capture_gates.sh` | 离线三模式门禁（`k0d`/`k1`/`combos`），自带串行复核 |
| `.../metal/test/port_channel_estimator_metal_mmse_unit_test.cpp` | CE 单测（**不在 ctest**，要显式重建） |
| `doc_chinese/full_gpu_chain/s2_full_chain_design.md` | **活文档**；§48.84 横向视图、§48.84(a0) 首要目标、§48.109 复核纪律、§48.123/§48.124 胶水 #2 |

---

## 9. 上机（OTA）准备

**二进制**：`gnb` 戳 `2068d8a56f`（已实测通过，可继续用；改动后务必重建同步）。

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_ota_k1.log
```
（**console 不要重定向**——`[metal_stats]`/`[mmse_time_sum]`/`[ul_gpu_lane]` 在 stderr，用户直接贴回。）

**判据（写进 console 的与另需 grep 的分开）**：
1. 手机 **attach + ping + iperf3 都跑起来**（**硬约束**）；
2. `[ul_gpu_lane] cbs/lane` —— 胶水消失的可观测量（当前 **3.00**；胶水 #2 后**不应增加**）；
3. `[mmse_time_sum]`：`stage`（胶水 #2 后应从 ~1.9 µs 降到 ~0）、`mean total`、`gpu_path`；
4. `[metal_stats] mmse_ce ... device_corr_builds>0`（应为 hops 的 30–50%）、`corr_build_fail=0`；
5. `[ul_pipeline]` / `[ul_channel_estimation]` median；
6. 另需 grep：`grep -c "Real-time failure in RF"`（按**时隙**折算，不劣于基线 **0.1148%**）、
   `grep -c "zero-copy cache hit with a larger request"`（**应为 0**）、
   `grep -m1 "Built in"`（核对版本戳）、崩溃/USB 错误（都应为 0）。

**AMF**：`192.168.31.250:38412`（N2 走 **SCTP**，`nc` 探不到是正常的；AMF 不在线时 gNB 约 8 秒后自行退出）。

---

## 10. 账本（不阻塞）

| 项 | 状态 |
|---|---|
| **胶水 #2**（设备导频直写 y/qy） | **下一步**，施工图就绪 |
| 主机 pre-stage 重算 | 待 `sigma2` 上设备时一起删（`pilots_lse_view` 还有别的消费者）|
| matrix flavor 的 `qy` 主机 staging | **有意保留**（见 §48.124(d)）|
| `EPRE` / `sigma2` / FD 平滑在主机 | 待搬 |
| 信道估计器单测注册进 ctest | **用户定为目标之后的后续任务**；split 缺陷已修，具备条件 |
| K1 内核延迟 | 几何已修正（64×16，实测 1.5–3× 加速）；剩余延迟才是性能债 |
| `OCUDU_CE_DEV_INVERT=1` / `OCUDU_INV_RL=1` | 已知不可用，勿碰 |
| `build_correlation()` 独立 CB | **默认路径已不用**（胶水 #1）；仅"设备建+主机反演"那一格仍在用 |
| 语料 `_h.bin`/`_llr.bin` | **不是基线**（与现行三条路径 8400/8400 全不同）；真正的基线是 `*_ce.txt` |
| replay 工具高并发静默出错 | 门禁已加串行复核；**不要用 10 分片的结果直接下结论** |

---

## 11. 一句话交接

**模块已全在 GPU（K0-a 落地、胶水 #1 已消灭、split 雷已排、`[ul_pipeline]` 首次低于 1 ms）；
下一步是胶水 #2 —— 让设备的导频直写 `gpu_y`，按 §48.124 施工，离线门禁后停下来交用户 OTA。**
