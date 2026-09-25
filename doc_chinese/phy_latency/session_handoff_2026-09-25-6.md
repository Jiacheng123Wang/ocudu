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

**目标（V1 时延 + V2 池）已结项**（§6.43），此后按用户裁决**继续压 V1**。
**本会话做了三件**：(1) 落地 §6.61 的首选杠杆 **C —— K2 直接读 LSE、`scatter` 派发消失**（§6.62，离线网全绿）；
(2) **验收腿 `p39-n78-noscatter`**（§6.63）：**机制预登记逐条命中**（`scatter` **0**、`lse_applies=142740`、
`device_y_writes=0`、**−1.455 次派发/跳**、`cbs/lane` 不变、契约 8/8、**`p0_gate 29/29`**）、
**V1 1364.2 µs（新最好，基线 2675.1 ⇒ −49.0%）**；
(3) ★ **反向臂 `p40-n78-noscatter`**（`OCUDU_CE_Y_DIRECT=0`，同二进制紧邻，**RF 1448 vs 1447**）⇒ **配对 A/B：V1 −21.5 µs、`merged_hop` −17.4 µs，
落在预登记带（−15…−35）内** ⇒ **§6.63 的三处结论已被 §6.64 更正**：V1 判定**命中**（不是 MISS）、
**标尺 = 5–14 µs/派发（最强配对给 11–14）**、**A 项 = −0.98 次/跳（不是 −2.91）⇒ 剩余派发预算 ≈ −30…−42 µs**。
**当前正在做**：交接 + 已开工 **§3.3 的单 kernel 离线微基准**（见 §3.1 第 2 项：**这才是主战场**，`merged_hop` 里非派发的 ~450 µs 大一个数量级）。

| 判据 | 阈值 | 现状 | 出处 |
|---|---|---|---|
| **V1** `[ul_gpu_pipeline]` 中位 | ≤2150 µs | ✅ **1364.2 µs**（`p39`；基线 2675.1 ⇒ **−49.0%**）；**配对 A/B 确认 −21.5 µs**（`p40` 1385.7）| §6.46–§6.64 |
| **V2** 池 | `starved_events=0` 且 `held_max<pool` | ✅ `p39`/`p40` 两臂都 `starved_events=0`、`dropped=0`（held_max 18 vs 9，仍 `<32`）| §6.37–§6.39、§6.63①/§6.64① |
| **V4** 提交数 | `cbs/lane ≤2.00`、`dropped=0` | ✅ `p39` **2.00 (max=2)**、crossings `0.00+0.00`/跳 | §6.30/§6.63① |
| **V5** 不回归 | 契约 8/8、0 crossings | ✅ **两臂都** MET 8/8、`gaps=0`、`rx_overflows=0`、`p0_gate` **29/29** | §6.53/§6.63①/§6.64① |
| **V3** 电台 | RF 失败 ≤10 | ⏸ **另案暂停（用户裁决）**：`p39` 1448 / `p40` **1447**（这一对天气几乎相同 ⇒ 已被配对法解决）| §6.40–§6.43、§6.56③ |

**当前状态**：本会话的提交依次是 **`5a375cb1ba`（代码：杠杆 C）**、`7db01fa5c8`（§6.62 文档）、`0a4bea6968`（本 memo）、
`045378f2e4`（§6.63 飞腿分析）；**`p39` 就是用它飞的（HEAD `0a4bea6968`）**。
**别引用本 memo 自己的哈希**；开工前自行对齐：`git log --oneline -1` 与 `grep -oE '[0-9a-f]{10}' build/hashes.h | head -1` 必须相同、且
`grep -aq "$(grep -oE '[0-9a-f]{10}' build/hashes.h | head -1)" build/apps/gnb/gnb` 为真；不同就重跑
`cmake --build build --target ocudu_versioning && cmake --build build --target gnb`（**分开两条命令**）。
**工作区干净**；**没有腿在跑**，**没有 gNB 在跑**。
⚠ `ul_chain_replay` **从不内嵌版本戳**（它不链 versioning 目标）⇒ 对它做 `grep -aq "$H"` 判据**恒为假**，别误判成"旧二进制"。
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
| ★ **验收腿 `p39-n78-noscatter`**（杠杆 C）| **机制预登记逐条命中**（见 §3.1 的表）；**V1 1364.2 µs（新最好，−49.0%）** | §6.63① |
| ★★ **反向臂 `p40-n78-noscatter`**（`OCUDU_CE_Y_DIRECT=0`）| **配对 A/B（同二进制、紧邻、RF 1448 vs 1447）：V1 −21.5 µs、`merged_hop` −17.4 µs，落在预登记带内** ⇒ **更正 §6.63 的 V1 判定（命中，不是 MISS）、标尺（5–14，配对 11–14）与 A 项的账（−0.98 次/跳）**；两条异常读数结案（`ch_wt` 与 knob 无关；`stale` 两对矛盾 ⇒ 归宿主另案）| §6.64 |

**本会话没有做**：没有改 `.metal` 里的旧 kernel、没有动 V3/gap 另案、没有提交 metallib（git 忽略）。

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
| 9 | ★ **`p39` 机制全中**：142744 次 K2 里 **142740 走直读、4 次是构造期 warm-up（`run_weights_only()` 不传描述符）、0 次退回**；`scatter=0`、`device_y_writes=0`、−1.455 派发/跳、`cbs/lane` 仍 2.00 | §6.63① |
| 10 | ★★★ **配对 A/B 确认杠杆 C：V1 −21.5 µs、`merged_hop` −17.4 µs**（`p40` scatter → `p39` 直读；同二进制、紧邻、RF 1448 vs 1447、`p0_gate` 两边 29/29）⇒ **预登记的增量命中** | §6.64①② |
| 11 | ★★ **标尺：每次派发 5–14 µs，最强设计（同二进制配对）给 11–14**；四次估计按设计强度排：11.4/14.1（`p39`vs`p40`）、7.05（§3.2 对）、5.1/5.3（`p38`→`p39`，异二进制）。**"13–18 µs"没被证伪**（在上沿）⇒ **下次预登记用 10–14 µs/派发（配对口径）** | §6.64③ |
| 12 | ★★ **A 项的账更正**：杠杆 C 之后 A 只剩 **corr 那一对** = **−0.98 次/跳**（每带尾跳省 2、带尾 ≈49%），不是 −2.91 ⇒ **剩余派发预算 ≈ −30…−42 µs**（`reformat` 1 + D 1 + A 0.98 次/跳 × 10–14 µs）| §6.64④ |
| 13 | ★ **预登记只登记增量，不要锚绝对区间**：§6.63 判"MISS"是因为拿**异二进制、早一小时**的 `p38`（1371.9）当对照，而**同一条 scatter 路的基线自己漂了 13.8 µs**（`p40` 1385.7）⇒ 绝对区间把对照的漂移也算进了预测。**"单腿不是证据"这次是被自己的文档抓到的** | §6.64② |
| 14 | **两条异常读数结案**：`ch_wt` +2.8 µs **与 knob 无关**（对照臂 43.4 > treatment 42.7，是按二进制/时段的漂移）；`stale` **两对互相矛盾**（20 vs 0；13 vs 1 反向）⇒ 归 §3.5 宿主另案，**要判它得飞"宿主竞争/线程优先级"臂，不是再飞路线臂** | §6.64⑤ |

**仍然成立的老结论（别重犯）**：每去掉 1 次派发 ⇒ V1/跨度 ≈**13–18 µs**，但设备 busy 窗口只有 6.5–9.4 µs ⇒ **标尺有口径**
（§6.49③/§6.50③）；`busy`/`busy split` 是**占用窗口**不是算力；**窗口 ≠ 关键路径代价**；池容量是 **2 的幂**；
`starved_events` 是"进入 nearly-dry 的**次数**"；夹具/离线 replay 里的读数不是空口读数；
**真实派发账是 8–11 次/跳**（§6.61②），车道口径的 4 只是**阶段数**。

---

## 3. 下一步（**已排好，按顺序做**）

### 3.1 ★ 第一件：**`p39` + 反向臂 `p40` 都已飞完** —— 杠杆 C 验收完成，下面是**下一步（已开工）**

**结论（§6.63 + §6.64）**：机制预登记逐条命中；**配对 A/B（`p40` scatter → `p39` 直读，同二进制、紧邻、RF 1448 vs 1447）：
V1 −21.5 µs、`merged_hop` −17.4 µs**，落在预登记带（−15…−35）内 ⇒ **杠杆 C 验收通过**。
两臂的 V1/`p0_gate`/契约/V2/V4 全绿（`p39` 7/9、`p40` 8/9 的 `leg_gate` 差额是 `stale` 那条**误绑项**翻红，见 §6.64⑤）。

| 配对读数 | `p40`（scatter）| `p39`（直读）| Δ |
|---|---|---|---|
| **V1 中位** | 1385.7 | **1364.2** | **−21.5 µs** |
| `merged_hop`（93% busy）| 551.1 | **533.7** | **−17.4 µs** |
| `ch_wt`（7% busy）| 43.4 | 42.7 | −0.7（⇒ 与 knob 无关）|
| `stale` | 0 | 20 | 两对矛盾 ⇒ 宿主另案 |
| `ce_sites scatter` / `device_y_writes` / `lse_applies` | 214305 / 214305 / 0 | **0 / 0 / 142740** | 机制 |
| `p0_gate` / 契约 / `cbs/lane` / gaps | 29-29 / 8-8 / 2.00 / 0 | 同 | 判据 |

**§3.3 的第一半已经做完（§6.65）**：新工具 `doc_chinese/phy_latency/wip/ce_kernel_cost.mm` 量出 **CE 六个 kernel 的算力**（生产几何、可复现 ~3%）：

| kernel | µs/派发 | 每跳次数 | 每跳 µs |
|---|---|---|---|
| **空派发地板**（1 threadgroup）| **1.26–1.38** | — | — |
| `mmse_reformat`(K3) | 1.52 | 1.00 | 1.5 |
| `mmse_corr_a` | 1.69 | 1.456 | 2.5 |
| `mmse_corr_r_hp` | 6.07 | 1.456 | 8.8 |
| `mmse_apply_lse`(K2 直读) | 7.42 | 1.00 | 7.4 |
| `mmse_weights`(K1b) | **13.18** | 1.00 | 13.2 |
| `pilots_lse`/`pilots_cfo` | 未测 | 1.00/1.00 | ~4–8（估）|
| **CE 合计** | | **5.91/跳** | **≈38 µs/跳** |

**三条结论（都改变"下一个杠杆做什么"）**：
1. **CE 执行 ≈38 µs = `merged_hop` 的 ~7%** ⇒ §6.63④/§6.64④ 的"非派发那 ~450 µs 是真实算力"**作废**；单个 kernel 最大头只有 **13.2 µs**（且不在任何消派发名单里）。
2. ★★ **腿上"每去一次派发值 10–14 µs"既不是 kernel 执行（被消的那些只 1.5–6.5 µs）也不是派发固定成本（1.3–1.4 µs）⇒ 那是"阶段边界"的串行化**
   （pipeline 切换 / barrier / 宿主 encode / 丢掉的重叠）。⇒ **"消派发"与"减少阶段边界"是同一个杠杆的两种做法**，§6.61③ 的现成旋钮
   （`OCUDU_CE_CORR_SEGMENT`/`OCUDU_CE_WEIGHTS_BARRIER`/`OCUDU_CE_EQ_DEFER_ENCODE`…）**可以直接当杠杆试，不必等 kernel 重写**。
3. **V1 剩下的大头不是 kernel 算力**：`merged_hop` 533.7 µs 里约 **464 µs 是"等本槽样点到达"**（前端 lane residency 中位 463.9 µs ≈ 一个时隙 466.7 µs，
   与 §3.4 早已登记的 **A 项 ≈473 µs（零算力）** 独立吻合），加 CE 38 + 均衡/解映射/重排 ~30。
   ⇒ 唯一能碰它的是 **A 项 / `P1-7`**（符号级收包），**它触 V4、需要用户裁决**。

**§3.3 的另一半也做完了（§6.66）**——用户裁决的"便宜两步"结果：
* **(b) "减少边界"臂被测量否掉 ⇒ 不必飞腿**：在两次派发之间插入 `memoryBarrierWithScope` / pipeline 切换 / **encoder 边界**，
  每次派发只动 **−0.1…+0.3 µs**（噪声量级）；**宿主 encode 一个派发只有 0.08–0.18 µs**。
  ⇒ `OCUDU_CE_CORR_SEGMENT`/`OCUDU_CE_WEIGHTS_BARRIER` 这类旋钮是**诊断工具，不是杠杆**。
* ⇒ 腿上的"**每去一次派发值 10–14 µs**"**没有任何机制支持**（被消的 scatter 自己只值 ~3.5 µs = 1.5 执行 + 1.4 地板 + ≤0.3 边界 + 0.2 宿主）。
  **配对 V1 增量本身仍成立**（同二进制、紧邻、RF 1448/1447）——**没有支撑的是它的"每次派发"归因**。
  下一个能看见它的仪器必须**把宿主/队列/共享 burst 包进来**：`OCUDU_CE_TIME` 的 `[mmse_time]` 相位表（**本构建未定义该宏**，要一次带 `-DOCUDU_CE_TIME` 的构建）或 §6.24 的 P0 dump——**不是再做单 kernel 基准**。

**预算（§6.64④+§6.65③+§6.66③ 合并，照这个用）**：剩余消派发候选 **≈5–20 µs**（按各自 kernel 的**延迟** 1.5–7 µs，不是 10–14）；减少边界 **≈0**；
CE 全部算力 **≈38 µs**（6 个 kernel；**均衡/解映射未测**）；`merged_hop` 其余 **≈464 µs = 等本槽样点到达（A 项，零算力）**。

**开工顺序**：(1) 把**均衡/解映射**补进 `ce_kernel_cost.mm`（补全账单）；
(2) 需要时用 `-DOCUDU_CE_TIME` 的构建定位那 10–14 µs（**诊断，不承诺收益**）；
(3) ★ **A 项 / `P1-7` 的裁决**——**派发/边界这条线已见底（≤20 µs），它是唯一还有量级差的东西**（触 V4）。

**（两条腿的起腿命令留档：`p40` = `p39` 的配方 + `OCUDU_CE_Y_DIRECT=0`）**

```bash
sudo -E LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz.yml bash \
  doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <标签> \
  --regime=stress OCUDU_UL_PHASE_SEGMENTS=1 \
  --expert_execution.threads.upper_phy.max_pusch_and_srs_concurrency=2
# CN 侧：iperf3 -R -b 40M -P 4 -t 240；腿用【单次 Ctrl-C】停并确认退出
# 判读：p0_gate.sh <标签>；leg_gate.sh --slot-ms=0.5 <标签>
# 注意：leg_gate.sh 的 `stale = 0` / `grant ≥50%` 在加压腿上是【误绑项】，只作参考，但翻红要记、要归因
```

**（若还要再动派发：** 按 §3.2 的顺序顺手做 K3 → A → D，**不必为它们单独设计实验**——预算 ≈30–42 µs，且方法论已三次验证。）

### 3.2 之后（§6.61③ 的重排，按此顺序）

| # | 内容 | 省 | 覆盖 | 已知约束 |
|---|---|---|---|---|
| **reformat(K3)** | 与 C **同法**消掉（K3 是 h 的搬运/压缩）| −1 次/跳 ⇒ **≈−10…−14 µs** | 全部跳（split 路由下为 0）| C 之后的第一步，方法论已三次验证 |
| **A** | "标准组 + 尾巴组"并成**一次派发** —— ⚠ 杠杆 C 之后 **scatter 已不在**，A 只剩 **corr 那一对**（每带尾跳省 2）| **−0.98 次/跳** ⇒ **≈−10…−14 µs** | 带尾跳（≈49%：`p39` 45.5%、`p40` 52.9%）| ⚠ **不是纯宿主**：`l`/`npf`/`ncomb`/`pilot_base`/`sigma2` 是**一次调用一份** ⇒ 需**逐 system 几何**；"尾巴 padded 进标准槽位"的形式**代码里已有已知 pad 缺陷** |
| **D** | `pilots_lse` + `pilots_cfo` 融合（CFO 是对 LSE 输出的逐点相位乘）| −1 次/跳 ⇒ **≈−10…−14 µs** | 全部跳 | kernel 重写；逐字节或有界容差 + `value_net` |
| ~~B~~ | ~~`R_hp` 按几何缓存~~ | — | — | ❌ 读码后否掉（`corr_stage` 带每跳信道统计）|

**合计**（§6.64④ 重算）：`reformat` 1.00 + D 1.00 + A 0.98 = **2.98 次/跳** × **10–14 µs**（配对口径）⇒ **≈ −30…−42 µs**
（V1 1364 → ≈1322–1334 的乐观界）。
⇒ **派发/边界这条矿脉还剩 ≈30–42 µs**；而 §6.65 把"另一头"也量了：**CE 的全部算力只有 ≈38 µs**，
`merged_hop` 里剩下的 ~464 µs 是**等样点到达**（A 项，零算力）⇒ **V1 的大头只能靠结构项（`P1-7`，触 V4，需用户裁决）**。
本文件的 §3.1 已给出开工顺序（补测 → 减少边界臂 → A 项裁决）。

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
| `doc_chinese/phy_latency/gpu_phy_latency_optimization_design_and_implementation.md` | **开发文档（先读这个）**：§3 判据、§4 仪表手册、§5 跑腿规范、**§6 追加式记录（本会话 = §6.62 落地 + §6.63 `p39` + §6.64 反向臂 `p40`）**、§7 杠杆（**新增 P2-G 行**）、§8 未决 |
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
> 目标（V1/V2）已结项（§6.43），之后在**继续压 V1**。**本会话收官的是杠杆 C（§6.62 落地 / §6.63 `p39` / §6.64 反向臂 `p40`）**：
> **K2 直接读 LSE、`scatter` 派发消失**；**配对 A/B（`p40` scatter → `p39` 直读，同二进制、紧邻、RF 1448 vs 1447）V1 −21.5 µs、`merged_hop` −17.4 µs**，
> **落在预登记带内 ⇒ 验收通过**。**V1 新最好 1364.2 µs（基线 2675.1 ⇒ −49.0%）**，`p0_gate` 两臂都 **29/29**。
> ★ 三条被 §6.64 更正过的账，**照新的用**：**标尺 = 10–14 µs/派发（配对口径；区间 5–14）**；
> **A 项 = −0.98 次/跳**（不是 −2.91）；**剩余派发预算 ≈ −30…−42 µs**（`reformat` 1 + D 1 + A 0.98 次/跳）。
> **下一步（本会话已开工，不需要飞腿、不需要裁决）= §3.3 的单 kernel 离线微基准**：照 `doc_chinese/phy_latency/wip/dft_kernel_cost.mm` 的形状，
> 把 CE / 均衡 / 解映射各自的算力与窗口量出来，**并单独量"空 kernel 派发"的固定开销**（把派发成本与算力成本分开）。
> 理由：`merged_hop` 533.7 µs 里**非派发的 ~450 µs 比剩余派发预算大一个数量级**。
> 开工先对齐：`git log --oneline -1` = `build/hashes.h` = `gnb` 内嵌戳（**`ul_chain_replay` 不含戳**，别误判）。
> 提醒五条纪律：**一条臂只改一个变量**、**判 `ctest` 用串行**、**A/B 的 metallib 必须成对且不许拿树里那个当 B 臂**、
> **预登记只登记增量（别锚绝对区间）**、**门里 `stale = 0` / `grant ≥50%` 在加压腿上是误绑项（翻红要记、要归因，不许静默放过）**。
> **V3 与 gap 残余已另案暂停**（靶子 = 宿主竞争/线程优先级）；**`stale` 也一样——要判它得飞宿主竞争臂，不是再飞路线臂**。
