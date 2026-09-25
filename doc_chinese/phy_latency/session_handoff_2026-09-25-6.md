# Session handoff — 2026-09-25 #6

> **本工作流（GPU PHY 融合车道时延）的会话交接快照**（新会话只读**序号最大的那一份**）。
> **命名规则（用户 2026-09-25 明确）**：`session_handoff_<日期>-<序号>.md`，**日期取会话开始那天**（跨午夜不改），
> 序号是那天的第几份 ⇒ 本文件是 **2026-09-25 的第六份**（继 `…-1.md` … `…-5.md`）。
>
> **细节一律在开发文档** `gpu_phy_latency_optimization_design_and_implementation.md`（下称"开发文档"）：
> **上一份 memo（`-5`）之后新增的是 §6.62**（本会话的唯一产出）；`high_level_status_and_plan.md` 是高层现状；
> 三类文档分工见 `README.md`。**本 memo 只写"开工要什么"**：现状、读数出处、下一步（含已定好的施工方案）、
> 仍挂着的事、纪律、文件地图、第一句话。

---

## 0. 一句话现状

**目标（V1 时延 + V2 池）已结项**（§6.43），此后按用户裁决**继续压 V1**；`p38` 把一跳的真实派发从 12 压到
**4 次（车道口径）**、**V1 中位 1371.9 µs**（基线 2675.1 ⇒ **−48.7%**）。
**本会话做完了 §6.61 的首选杠杆 C**：**K2 直接读 LSE ⇒ `scatter` 派发消失**（无尾跳 −1、带尾跳 −2、**覆盖全部跳**），
**离线网全绿**（`ab_dumps` 0 字节、与改前二进制对拍 PASSED、`ctest -L phy` 193/193、`value_net` 逐行相同），
**只差飞腿 `p39-n78-noscatter`**（命令与预登记见 §3.1，**已可直接复制**）。
**当前正在做**：交接 —— 代码已落地、未提交、未飞腿。

| 判据 | 阈值 | 现状 | 出处 |
|---|---|---|---|
| **V1** `[ul_gpu_pipeline]` 中位 | ≤2150 µs | ✅ **1371.9 µs**（`p38`；基线 2675.1 ⇒ **−48.7%**）| §6.46–§6.60 |
| **V2** 池 | `starved_events=0` 且 `held_max<pool` | ✅ `p38`：`starved_events=0`、`held_max=10 < 32`、`free_min=22`、`dropped=0` | §6.37–§6.39、§6.60 |
| **V4** 提交数 | `cbs/lane ≤2.00`、`dropped=0` | ✅ `p38` **2.00 (max=2)**、crossings `0.00+0.00`/跳 | §6.30/§6.46/§6.60 |
| **V5** 不回归 | 契约 8/8、0 crossings | ✅ `p37`/`p38` 契约 **MET 8/8**、**`gaps=0`、`rx_overflows=0`** | §6.53/§6.57/§6.60 |
| **V3** 电台 | RF 失败 ≤10 | ⏸ **另案暂停（用户裁决）**：`p38` 1546（`p37` 880、`p35` 710）⇒ **逐腿天气不同，比较要成对** | §6.40–§6.43、§6.56③ |

**当前状态**：写本 memo 时的 HEAD 是 `bc149ab266`（= `-5` 那份 memo 自己的提交）；本会话的**代码提交是 `5a375cb1ba`**
（"lever C: K2 reads the least-squares pilots itself…"），**本 memo 的提交紧随其后**（所以 HEAD 会再往后一格 —— **别引用本 memo 自己的哈希**，
按下面的老办法自行对齐即可）。开工前：`git log --oneline -1` 与
`grep -oE '[0-9a-f]{10}' build/hashes.h | head -1` 必须相同、且
`grep -aq "$(grep -oE '[0-9a-f]{10}' build/hashes.h | head -1)" build/apps/gnb/gnb` 为真。
⚠ `ul_chain_replay` **从不内嵌该戳**（它不链 versioning 目标）⇒ 对它做 `grep -aq` 判据**恒为假**，别误判成"旧二进制"。
**没有腿在跑**，**没有 gNB 在跑**。
**交付配置**：接收环 **512 帧**、发送环 64、池 **32**、`otw_format: sc12`、`srate 23.04`（`configs/gnb_rf_b200_tdd_n78_20mhz.yml`）。

---

## 1. 本会话（2026-09-25 #6）做了什么

> 从 `-5.md` 开工：按 §3.1 施工**杠杆 C**（K2 直读 LSE），第一次 A/B 就抓到一处**真实的重结合缺陷**，
> 定位到 IR 后**把新 kernel 拆成独立文件 + `-fno-fast-math`** 修掉，然后在**最终产物**上重跑全部离线网 ⇒ 全绿。

| 内容 | 结果 | 出处 |
|---|---|---|
| **落地**：新 kernel `mmse_apply_lse`（**独立文件** `ocudu_mmse_apply_lse.metal`）+ 引擎 `build_lse_sources()`（门槛与计数）+ `OCUDU_CE_Y_DIRECT`（默认开）+ 仪器（`lse_applies`、`[y_direct]`、5 条 `y_direct_*` refusal） | `ce_sites scatter` **2 → 0**、`lse_applies=1`（合并批两组只发一次 K2）、`device_y_writes 2 → 0` | §6.62①④ |
| ★ **第一次 A/B 读出 40 字节**：`_h.bin`/`_llr.bin`/网格 **0**，只有 `_ce.txt` 的 `noise_variance`/`rsrp` 末位，13/27 语料 | **真实缺陷**（不是 flake）：LLVM Reassociate 把 `w*(lse*inv_beta)` 改写成 `(w*inv_beta)*lse`（IR 直读为证）| §6.62② |
| **修法**：新 kernel 独立文件 + 进 `IEEE_MATH_SOURCES`（`-fno-fast-math`）；旧 `mmse_apply` **源码零改动、保持 fast math**（先量过：把 `ocudu_mmse_apply.metal` 整体编 strict，27 语料 135 个 dump **0 字节差异**）| 复测 `ab_dumps` **0 字节 / 27 语料** | §6.62②③ |
| **不变量网**（最终产物上重跑）：`ab_dumps` 0 字节；`ab_replay_bins` 与**改前二进制+改前 metallib** 对拍 **PASSED**（`[y_direct]` 自证配对）；旧 y 路臂 0 字节；`ctest -L phy -j 1` **193/193**；`value_net` **逐行相同** | 全绿 | §6.62③ |
| **`ctest -j 4` 假红已定位**：CE 单测 Test 3（SNR 20 dB 差 3.13 dB）与 `…_ta_chain` 红；**对照**：直接连跑 6 次两臂全 PASS 且数值相同、`OCUDU_CE_Y_DIRECT=0 ctest -j 4` **同样红**、`-j 1` 全绿 ⇒ **并行产物，与本次改动无关** | 纪律：判 `ctest` 用**串行** | §6.62③ |

**本会话没有飞腿**（全部离线：`ul_chain_replay --metal` 27 语料 + `ctest`）。

---

## 2. 本会话的增量结论（开新会话前必读的 8 条）

| # | 结论 | 出处 |
|---|---|---|
| 1 | **`scatter` 可以整条消掉**：它只是"重索引 + 一次单精度乘"，让 K2 按同一映射直接读 `gpu_ls_out` 即可；**两组清零区按 `w*0` 项参与求和**（与 scatter 写下的字面 0 同义，非有限权重照样进 h）| §6.62① |
| 2 | ★ **fast math 的重结合会真的改值**：`w*(lse*inv_beta)` 被 LLVM 改写成 `(w*inv_beta)*lse`（把 `inv_beta` 提到权重上、一次乘法服务实虚两路）⇒ `round(round(w*β)*lse)` ≠ `round(round(lse*β)*w)`。**判据要按"会被量化吃掉的那一层"选**：bf16 的 `_h.bin` 与 LLR **看不见** 1 ulp，float32 归约（K4 `noise_variance`、K5 `rsrp`）**看得见** | §6.62② |
| 3 | **改法的形状**：`IEEE_MATH_SOURCES` 是**按文件**给 `-fno-fast-math` 的 ⇒ **新 kernel 独立成文件**；**旧文件一行不改**，"y 路仍发布昨天的字节"才是**源码性质**而不是测量结论 | §6.62② |
| 4 | **`ab_replay_bins.sh` 的配对判据必须有**：我曾把 `AB_METALLIB_B` 指成**树里那个 metallib**，`cp` 因"同一文件"跳过 ⇒ **B 侧实际跑着 A 的旧内核**（dumps 0 字节看起来全绿），是脚本的 `pairing-wrong` 把它抓出来的。**新 metallib 必须先拷到中立路径** | §6.62③ |
| 5 | **`ctest -L phy` 必须串行判**：`-j 4` 下 CE 单测与 ta_chain 会假红（并行 GPU 争用），**两条路都红** ⇒ 用 `-j 1`（193/193）| §6.62③ |
| 6 | **`ab_dumps` 的偶发类仍在**（= §6.5⑤/Q11）：旧 y 路臂第一次读到 **4546 字节 / `syn009_6`**，按偶发规则重跑 **0**；**隔离复跑**（两侧各自带自己的 metallib、逐条成对）含 `syn009_6` 在内 **0 字节** ⇒ 归入已登记的一类，**首次读数保留在册** | §6.62③ |
| 7 | **系统区间不许写第二份**：组的 `sys_lo` 由 `s.y - y_base` 除以系统步长得到 —— 与 `encode_scatter()` 算 `y_off` 用的是**同一个差**（S-7f-5l 的分尾缺陷就是"同一个数两份"）| §6.62① |
| 8 | **`OCUDU_CE_DEV_Y=0` 的原义保住了**：那条臂下宿主自己 stage y（没有描述符）⇒ 新路**必然**退回 scatter（计 `y_direct_no_source`）| §6.62① |

**仍然成立的老结论（别重犯）**：每去掉 1 次派发 ⇒ V1/跨度 ≈**13–18 µs**，但设备 busy 窗口只有 6.5–9.4 µs ⇒ **标尺有口径**
（§6.49③/§6.50③）；`busy`/`busy split` 是**占用窗口**不是算力；**窗口 ≠ 关键路径代价**；池容量是 **2 的幂**；
`starved_events` 是"进入 nearly-dry 的**次数**"；夹具/离线 replay 里的读数不是空口读数；
**真实派发账是 8–11 次/跳**（§6.61②），车道口径的 4 只是**阶段数**。

---

## 3. 下一步（**已排好，按顺序做**）

### 3.1 ★ 第一件：飞腿 `p39-n78-noscatter`（**验收杠杆 C**，命令可直接复制）

```bash
# 起腿（配置：接收环 512 / 池 32 / sc12；与 p37/p38 同一交付配置）
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu p39-n78-noscatter \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# CN 侧：iperf3 -R -b 40M -P 4 -t 240；腿用【单次 Ctrl-C】停并确认退出
# 判读：
bash doc_chinese/phy_latency/wip/p0_gate.sh p39-n78-noscatter
bash doc_chinese/phy_pipeline_gpu/wip/leg_gate.sh --slot-ms=0.5 p39-n78-noscatter
```

**预登记**（`p38` 作对照；**新仪器**：`lse_applies` 与 `[y_direct]`）：

| 读数 | `p38` | 预登记 |
|---|---|---|
| `ce_sites scatter` | 1.0/跳（无尾）、2.0/跳（有尾）| **0**（按跳均值 ≈1.4 → 0）|
| `lse_applies`（新）| — | **≈ 设备跳数**（合并批两组只 +1，不是组数）|
| `device_y_writes` | 组数/跳 | **0**（要与 `lse_applies` **一起读**）|
| `refusals` | `<none>` | **`<none>`**；出现 `y_direct_coverage/geometry` ⇒ 有批退回，先查覆盖 |
| `[y_direct]` | — | **恰好 1 行**（带首个批的 `systems/blocks/L/groups`）|
| `burst dispatches` | 4.00/跳（车道口径）| **−1…−2/跳**（均值 −1.4）|
| `merged_hop` | 541.1 µs | −7…−19 µs |
| **V1 中位** | 1371.9 µs | **−15…−35 µs ⇒ ≈1337–1357** |
| 契约 / V2 / V4 / D18 | 8/8 / 绿 / 2.00 / 不变 | **同** |

**反例判读**：`scatter` 仍非 0 ⇒ 门没生效（看 `refusals` 哪条在涨）；`lse_applies ≠` 设备跳数 ⇒ 部分批退回；
**`scatter=0` 而 V1 不降 ⇒ 这次省的派发不在关键路径上**（回 §6.49③ 的口径问题，别改阈值）。

### 3.2 之后（§6.61③ 的重排，按此顺序）

| # | 内容 | 省 | 覆盖 | 已知约束 |
|---|---|---|---|---|
| **reformat(K3)** | 与 C **同法**消掉（K3 是 h 的搬运/压缩）| −1 | 全部跳（split 路由下为 0）| C 之后的第一步，方法论已两次验证 |
| **A** | "标准组 + 尾巴组"并成**一次派发**（corr 与 scatter 各 2→1）| −3 | 带尾跳（≈40%）| ⚠ **不是纯宿主**：`l`/`npf`/`ncomb`/`pilot_base`/`sigma2` 是**一次调用一份** ⇒ 需**逐 system 几何**；"尾巴 padded 进标准槽位"的形式**代码里已有已知 pad 缺陷** |
| **D** | `pilots_lse` + `pilots_cfo` 融合（CFO 是对 LSE 输出的逐点相位乘）| −1 | 全部跳 | kernel 重写；逐字节或有界容差 + `value_net` |
| ~~B~~ | ~~`R_hp` 按几何缓存~~ | — | — | ❌ 读码后否掉（`corr_stage` 带每跳信道统计）|

### 3.3 第二项（V1 的另一半）：`merged_hop` 里**非派发**的 ~490 µs

一跳窗口 541 µs、4 次派发（真实 8–11 次 kernel）≈ 12–18 µs/次 ⇒ 其余 **~350–450 µs 是真实算力 + 派发间依赖等待**。
**推荐先做**：**单 kernel 离线微基准**（照 `doc_chinese/phy_latency/wip/dft_kernel_cost.mm` 的形状，**不用飞腿**），
一次量出 CE / 均衡 / 解映射各自的算力与窗口，做完才知道该砍哪个 kernel。另一条路是"移除阶段"臂（现成旋钮见 §6.61③）。

### 3.4 第三项（结构项，**需要用户裁决**）：等本槽最后一个样点 ≈473 µs（零算力）

载体是 `P1-7` 符号级收包（`OCUDU_UL_RX_SYMBOLS=N`），但**触 V4**（提交数），且其代码注释写明**时延收益目前无法判读**。

### 3.5 另案与可选（都不属于当前主线）

* ⏸ **V3 + gap 残余（暂停）**：真问题是**偶发的 ~40 ms 宿主停顿**（`stale` 13 次/0.01% 的跳），**靶子 = 宿主竞争与线程优先级**。
  重启入口：`load1` + 线程数/优先级臂。
* **extended CP 取证腿**（12 符号路径已实现、metal arm 17 离线自证，**没有空口腿**）。
* **D4/gap 按对累计**；**P1-6**（`OCUDU_UL_SLOT_TRACE`）；**两条陈旧网**（`value_net.py` 的归档基线、`ab_dumps` arm1 ⇒ **Q11 待用户裁决**）；
  **P2-F**（并发度作为交付，需二次裁决）；**P2-A**；**MAC 侧从丢失 CRC 指示恢复**（HARQ 饥饿，独立缺陷，未修）。
* **Ubuntu 侧**（`jwang@192.168.31.211:~/work/ocudu`）：本会话**没有**改构建系统以外的公共文件；
  构建命令 `cmake -S . -B ./build -DCMAKE_BUILD_TYPE=Release -DENABLE_FLOW_PROBES=ON && cmake --build build -j16`。

---

## 4. 纪律（本会话又花过代价的几条，全部更新）

1. **一条臂只改一个变量**；**改动与仪器分开提交**。
2. **`cmake --build build --target X --clean-first` 会清整棵树** ⇒ 改 `.metal` 只 `--target ocudu_mmse_metallib`。
3. **改完代码必须重建两样**：`cmake --build build --target ocudu_versioning && cmake --build build --target gnb`（分开两条命令）。
   ⚠ **`ul_chain_replay` 不内嵌版本戳**（`grep -aq "$H"` 对它恒假），别把它读成旧二进制。
4. **测试可执行文件不在默认构建目标里**：改引擎/估计器后要**显式构建**那一组再跑 `ctest`；改 `.metal` 还要重建 metallib。
5. **判 `ctest` 用串行（`-j 1`）**：`-j 4` 会让 Metal 测试互相争用而**假红**（本会话已用"两条路都红 + 隔离 6/6 绿"证伪）。**回放类工具串行用**。
6. **"逐字节相同"先核验两侧文件存在**（`cmp -s` + 存在性）；**`ab_dumps`/`ab_replay_bins` 的 metal 配对不许拿树里的 metallib 当 B 臂**（`cp` 会因同一文件跳过 ⇒ 两侧跑同一内核，dumps 全 0 却是空结论）。
7. **单腿不是证据；比较要成对**；**"读不出"按 RED**；**不许为过关改阈值**；**保留第一次读数**（本会话两处：40 字节的重结合差异、4546 字节的偶发）。
8. **用户飞腿时绝不碰 GPU/电台**；腿用**单次 Ctrl-C**停并确认退出；判 V1–V5 的腿**不带 `OCUDU_METAL_GPU_TIME=1`**。
9. **快/严数学是判据的一部分**：改 kernel 时先问"这个乘积/归约的**舍入位置**是不是契约"，要保就**按文件**上 `-fno-fast-math`
   （`IEEE_MATH_SOURCES`），并**在 IR 上确认**重结合真的消失了（`xcrun metal -c … && xcrun metal-objdump --disassemble`）。

---

## 5. 文件地图（新会话先看这几份）

| 路径 | 是什么 |
|---|---|
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§3 判据、§4 仪表手册、§5 跑腿规范、**§6 追加式记录（本会话 = §6.62）**、§7 杠杆（**新增 P2-G 行**）、§8 未决 |
| `doc_chinese/phy_latency/high_level_status_and_plan.md` | 高层现状、V1–V5 逐条、下一步 |
| `doc_chinese/phy_latency/README.md` | 三类文档分工 + 结项状态 + 工具清单 |
| **`lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_apply_lse.metal`** | **本会话新增**：K2 直读 LSE 的新 kernel（**独立文件**，进 `IEEE_MATH_SOURCES`；文件头写清"为什么必须独立 + 为什么必须 `-fno-fast-math`"，含重结合 IR 证据）|
| **`…/metal/ocudu_metal_mmse_engine.mm`** | 本会话主战场：`build_lse_sources()`（门槛/计数/`[y_direct]` 自报）、`y_direct_enabled()`（`OCUDU_CE_Y_DIRECT`）、两处 encode（`encode_run` / `encode_weights_only`）的 K2 双读者分支、`lse_applies` 计数、`last_apply_read_lse()` |
| `…/metal/ocudu_mmse_refusals.h` | 新增 5 条 `y_direct_*` 原因（**`y_direct_disabled` 是 knob**，其余是"设备不能"；单测的 `strict_taxonomy_check()` 按 `_disabled` 后缀判，已自动覆盖）|
| `…/metal/ocudu_mmse_apply.metal` | **一行未改**（旧 `mmse_apply` 保持 fast math，源码层面保证 y 路不变）|
| `…/metal/CMakeLists.txt` | 新增 SOURCES + `IEEE_MATH_SOURCES` 条目（含"为什么这第三个文件也要 strict"的注释）|
| `…/metal/port_channel_estimator_metal_mmse_impl.cpp` | `probe_device_y_stage()` 在本路下改打印 `routes=lse_direct` 并跳过（本路不写 y）|
| `doc_chinese/work_tmp/ref/` | **本会话的归档**：`replay_pre_c_bc149ab266`、`mmse_pre_c_bc149ab266.metallib`、`mmse_post_c_bc149ab266.metallib`、`value_net_pre_c.txt` |
| `configs/gnb_rf_b200_tdd_n78_20mhz.yml` | 腿配置：**接收环 512 帧**、发送环 64、`otw_format: sc12` |
| `doc_chinese/phy_latency/wip/p0_gate.sh` + `p0_gate_selftest.sh` | **P0 门（D1–D19）**；D18 = 直读网格分流，D19 = 接收侧余量与归属（自测双向）|
| `doc_chinese/phy_latency/wip/leg_census.py` | 腿普查（自行推导流量窗口 + 每次停顿打印 PUCCH 行数与判词）|
| `doc_chinese/phy_pipeline_gpu/wip/run_leg.sh` | 起腿（戳 + 内容判据双重守卫 + 进程/端口预检）；腿日志在 `…/wip/logs/` |
| `doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh` / `ab_replay_bins.sh` | **A/B 网**：前者 = 一个二进制两个环境；后者 = 两个二进制（**必须成对给 `AB_METALLIB_A/B`，且 B 不能指树里那个文件**）|

**腿配方（n78 加压，供参考）**：见 §3.1；CN 侧 `iperf3 -R -b 40M -P 4 -t 240`，**单次 Ctrl-C** 停。

---

## 6. 给新会话的第一句话（可直接复制的开工指令）

> 读 `doc_chinese/phy_latency/session_handoff_2026-09-25-6.md`。
> 目标（V1/V2）已结项（§6.43），之后在**继续压 V1**：`p38` 已到 **1371.9 µs（基线 −48.7%）**，
> **本会话已把 §6.61 的首选杠杆 C 做完并离线全绿**（§6.62）：**K2 直接读 LSE，`scatter` 派发消失**
> （无尾跳 −1、带尾跳 −2、覆盖全部跳；`ab_dumps` 0 字节、与改前二进制对拍 PASSED、`ctest -L phy -j 1` 193/193、`value_net` 逐行相同）。
> **下一步 = 飞腿 `p39-n78-noscatter`**（命令与预登记表在 memo §3.1，可直接复制）。
> 开工先做三件事：(1) 对齐 HEAD 与 `build/hashes.h`（`ul_chain_replay` **不含**戳，别误判）；
> (2) 工作区里**有本会话未提交的改动**——先确认它们就是 memo §5 列的那几处（`git status` + `git diff --stat`）；
> (3) 飞腿前按纪律重建 `ocudu_versioning` 与 `gnb`（分开两条命令）。
> 判读：`p0_gate.sh p39-n78-noscatter` + `leg_gate.sh --slot-ms=0.5 p39-n78-noscatter`；
> **关键新仪器**：`ce_sites scatter → 0`、`lse_applies ≈ 设备跳数`、`device_y_writes → 0`、`refusals=<none>`、`[y_direct]` **恰好 1 行**。
> 提醒三条纪律：**一条臂只改一个变量**、**判 `ctest` 用串行**、**A/B 的 metallib 必须成对且不许拿树里那个当 B 臂**。
> **V3 与 gap 残余已另案暂停**（靶子 = 宿主竞争/线程优先级），不要顺手去动。
