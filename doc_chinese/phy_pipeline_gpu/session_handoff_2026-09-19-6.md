# 交接 — 批次 5b + 5c + 5d：**上报量全部搬到设备，读侧归零（阶段完成）**

> ⚠ **已被 `session_handoff_2026-09-19-7.md` 取代**（那一份含 5e/5f 的最新状态与下一步）。

> ⚠ 本文件写于 5b 之后、5c/5d 之前，下面的 §1–§3.5 是当时的状态。
> **5c（`986c991742`）与 5d（`503990ca5f`、`413ef13f94`）的最终状态见文末 §7**，
> 以及常驻设计文档的**§18 阶段总结**（那是最完整的入口）。

> 上一份：`session_handoff_2026-09-19-5.md`（5a 完成 + GPU 挂死事故 + 阶梯纪律）。
> 常驻设计文档：`gpu_phy_pipeline_design_and_implementation.md` **§17.9**（本批的正式记录）。
> 过程记录：`wip/S12_batch5b_ta.md`（§4.5 实测常量、§6.5 接线做法）。

---

## 1. 一句话状态

**5b 的设备侧做完并离线验证完**：K7（摆放）+ `dft_dit`（变换）+ K6（归约）三条 dispatch
编进 reformat 自己的命令缓冲，结果接进发布路径（`get_device_ta_seconds()`）。
判据：27 捕获 A/B `ta_us` **逐位相同**、三个 dump 全 0 差异、单测 32 跳全部 < 1 分辨率。
**下一步是空中腿**（需要用户手机 ping/iperf3，§10.1 停车规则）。

---

## 2. 这一轮改了什么（14 个文件，全部**未提交**，HEAD 仍是 `94df4144a2`）

| 文件 | 改动 |
|---|---|
| `metal/ocudu_mmse_ta.metal` | **K7 重写**：输入从"flat LSE"改成 **`h`**（K5 的同一个 buffer），参数块 = K5 的几何（14 uint）+ `size/stride/nof_dmrs_symbols/dmrs_slots[4]` = **88 字节**；写满整个 slice；DC 不跳过 |
| `metal/ocudu_metal_mmse_engine.{h,mm}` | `hop_geometry`（K5/K7 共用几何 + `words()`）；`ta_stage_t` 重写；`encode_ta()`；`load_ta_dft_pipeline()`；`build_ta_tables()` 挪进匿名命名空间并改用 `compat::aligned_alloc/free`；`run_ta_place()` 新签名；`encode_reformat()` 里紧挨 K5 调用 |
| `metal/port_channel_estimator_metal_mmse_impl.{h,cpp}` | `ta_attach_stage()`；`gpu_ta`（8 个页对齐槽）；完成时读回；`get_device_ta_seconds()`；**按跳判定的回读门**；`OCUDU_CE_DEV_TA` / `OCUDU_CE_TA_CHECK` 探针 |
| `port_channel_estimator_average_impl.{h,cpp}` | 新虚函数 `get_device_ta_seconds()`；`get_ta_estimator()`（const + mutable）；`compute_hop_finish()` 优先用设备值 |
| `include/.../time_alignment_estimator.h` | 新纯虚 `get_idft_size(nof_re)`（设备**问宿主**要变换尺寸）|
| `support/.../time_alignment_estimator_dft_impl.{h,cpp}` | 定尺寸抽成 `get_idft_size()`，`get_idft()` 复用它 |
| `lib/phy/metrics/phy_metrics_time_alignment_estimator_decorator.h` | 转发 `get_idft_size()` |
| `metal/test/port_channel_estimator_metal_mmse_unit_test.cpp` | `l2_k7_placement_only()` 与 `s12_device_ta_chain_matches_host()` 改成 **h 输入**（哨兵填充、逐位检查）；Test 13 里挂 `OCUDU_CE_TA_CHECK` 探针并做成 PASS/FAIL |
| `wip/ab_ta.sh` | **新增**：27 捕获的设备/宿主 TA A/B（dump 逐字节 + 探针判据 + `ta_us` 数值）|

---

## 3. 判据（原样，可直接复跑）

```bash
# 1) 单测（L2/L3 阶梯 + 探针判据）。必须限时，跑完查 recoveryCount
cmake --build build --target port_channel_estimator_metal_mmse_unit_test
OCUDU_CE_TA_CHAIN=1 bash /tmp/limited_run.sh 420 /tmp/l2.log \
  build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
# 期望：L2 K7 PASS / S12 chain PASS / "Test 13: the device time alignment matches the host's own
#       estimate on 32 hops" / All tests PASSED / exit 0 / recoveryCount=0

# 2) 离线 A/B（27 捕获）
cmake --build build --target ul_chain_replay
bash doc_chinese/phy_pipeline_gpu/wip/ab_ta.sh
# 期望：captures=27 missing-dumps=0 dump-differences=0 ce-differences-outside-ta=0 probe-failures=0
#       worst |ta_us difference| = 0 us → PASSED

# 3) 真实调用路径的常量核对（应与 wip/S12_batch5b_ta.md §4.5 逐项相同）
OCUDU_CE_TA_CHECK=1 build/.../ul_chain_replay <capture> --out /tmp/x --metal 2>&1 | grep ta_stage
#   syn025_25: size=256 stride=2 scs=30000 window=18 pilots=150 hop=0
#   syn004_4 : size=128 stride=2 scs=30000 window=9  pilots=24  hop=0
```

**⚠ 跑单测/replay 前必须**：确认没有别的 GPU 负载、限时、跑完查
`ioreg -r -c IOAccelerator -d 1 | grep -o 'recoveryCount"=[0-9]*'`（§48.131/§48.133）。

---

## 3.5 ✅ 5b 的空中腿已跑（`5b-devta_0919_1650`，commit `1826e01e07`）：**判据通过**

* 腿有效：543470 blocks、**0 gaps**、assembly 7608496/7608496；契约 **7/8**（唯一 FAILED = 跨越，预期）；
* **TA 空口无跳变**：30 条 `TA_CMD`，值 27–34，步长均值 0.33 µs、最大 1.6 µs；
* `ce device estimates` **36586 device / 0 host**；零拷贝 wrap **0 failures / 0 misaligned**；
* 读数 **1.39/跳不变**（`HOST_GRID` 仍为 1，**按设计**）；
* CRC 全腿 66.63%，但**只看真正跑流量的那一分钟是 73.90%**（5a 腿 73.35%）——
  差额全部来自流量结构（按分配宽度分层后两腿重合，见设计文档 §17.9.7）；
* ⚠ 42 次 RF underflow（伴随 67 次 PRACH 重接，无可比基线）+ `gpu_wait`/`ch_wt` 比 5a 腿 **+47 µs**
  （而 5b 改动的 `ch_est` 阶段 119.4 µs 与 5a 的 124.3 µs 持平）⇒ **5c 的腿盯这两个数**。

## 3.6 ✅ 5b 的 TA 让 `ch_wt` 贵了 50 µs/lane —— **已改（合并成一条 dispatch），等空中腿确认**

四条腿对照（`busy split`）：

| 腿 | 5b | `ch_est` | `ch_wt` |
|---|---|---|---|
| `ota-b4` | 否 | 123.2 | 339.2 |
| `5a-pubpath` | 否 | 124.3 | 352.3 |
| `5b-devta` | **是** | 119.4 | 399.8 |
| `5c-hostgrid` | **是** | 123.6 | 406.1 |

`ch_est` 不动、`ch_wt` 只在带 5b 的腿上 +50 µs（`gpu_wait` +47 µs）⇒ 是那三条 dispatch 的代价。

**两个假设都被实验否掉**（细节见设计文档 §17.10.5）：
* "32 KB 静态线程组内存"：改成按尺寸声明的 threadgroup 参数后 **GPU 时间一动不动**（24.83 → 25.25 µs）；
  **改动已撤回**（无收益 + 给最热的前端 DFT 路径加风险）；
* "两次 `memoryBarrierWithScope`"：删掉后配对增量 +32 → +28 µs，**也否掉**（且本来就不必要：
  三条 dispatch 绑的是同一个 MTLBuffer 对象，Metal 自己排序）。

**✅ 采纳的正解：三条 dispatch 合并成一条**。新 kernel `mmse_ta_chain`
（摆放 + 变换 + 谱 + 找峰，**一个线程组一次 dispatch**），变换用抽到
`ocudu_dft_butterflies.h` 的**同一份**蝶形（`dft_dit` 与它共用）。顺带消掉中间 spectra 缓冲、
两次跨 dispatch 依赖、两个 metallib 的 pipeline 切换。上限 2048（= `get_idft()` 的最大值）。

判据全过：`S12 chain PASS`（4.39 ns/8.14 ns，且与三条 dispatch 路线**逐点差 < 1 ps**）、
`Test 13: … 32 hops`、27 捕获 A/B 全 0 差异、`All tests PASSED`、`recoveryCount=0`。
配对离线代价 **+32 → +22 µs**（只拿回 1/3，见文档里的"仍未解释"一段）。

**⚠ 融合 kernel 的坑**：摆放**必须过 `perm` 表**——蝶形假定输入已是数字反转序，
按自然序写会二次反转（第一版恒等于 ~0）。

**❌ 但空中腿 `5d-fused_0919_1820` 显示 `ch_wt` 一点没动（404.4 µs）**——三个结构性假设全部证伪
（线程组内存 / barrier / dispatch 条数）。代价是一个**与实现形态无关的固定 ~+50 µs**。
剩下两个候选：(1) **命令缓冲级的线程组内存预算**拖累同缓冲里其它宽 dispatch 的并发
（能解释"三种形态都是 +50"和"孤立微基准看不到"）；(2) 5b 提交里 dispatch 之外的某处
（宿主侧已排除：`submit` +3.8 µs）。
**✅ 已跑决定性单变量腿 `5d-devtaoff_0919_1920`**（同二进制 `413ef13f94`，`OCUDU_CE_DEV_TA=0`，
日志自己打了 `[ta_impl] fused chain: mmse_ta_chain, one dispatch`）：

```
TA 开: ch_wt = 404.4 µs        TA 关: ch_wt = 366.2 µs        ⇒ TA 占 +38.2 µs
```

**⇒ TA 确认是原因，但那是"延迟"不是"工作量"**：融合成一条 dispatch 后仍是 +38 µs（算术只有几千次浮点），
所以三种形态量出来是同一个数 —— 都是**把短依赖链挂在命令缓冲末尾的那段 launch/drain 尾延迟**。

**决定：接受这 38 µs**（≈6% busy）。换到的是**读 1.45 → 0.00/跳**、契约读侧达标；
空口无代价：CRC **81.34% vs 81.23%**、RF failure **都是 0**、TA 命令同样平滑。
备选（未做）：单独命令缓冲 + `MTLFence` 排序移出关键路径——真正的设计改动，收益仅 6%，当前不动。
**腿品质**：CRC **81.23%（本线最高）**、**RF failure 0**、0 gaps、读 0.00/跳、wrap 0 失败
⇒ **融合 kernel 在空口上正确且更健康**。

## 4. 下一步（按顺序）

1. ~~5b 空中腿~~ ✅（见上）。
2. ~~翻 `OCUDU_CE_HOST_GRID` 默认 → 0~~ ✅ **已翻且离线判据通过**（§17.10）：
   27 捕获 A/B（默认 vs `HOST_GRID=1`）**四个 dump 全部逐字节相同**、探针 0 失败。
   ⚠ 顺带修掉一个**上报缺陷**：`_ce.txt` 的 `noise_variance` 一直是宿主累加值，
   而 LLR 用的是设备 K4 值（实路差 ~26%）——现在发布设备值（`_llr`/`_h` 逐字节不变）。
   空中腿 `5c-hostgrid_0919_1730` **通过**：读数 **1.39 → 0.00/跳（0 字节）**，
   CRC **80.08%**（本线最高）、underflow **1**（5b 是 42）、TA 命令 19 条步长 ≤1.07 µs、
   `ce device estimates` 37169/0、wrap 0 失败、lane residency/gap 比 5b 各好 43/53 µs。
   ✅ **读侧到此清零。**
   ⚠ 契约**仍 7/8**：FAILED 项从"读"换成了"写"——**写侧 0.27/跳（903 次 / 3.38 MB）**，
   5b/5c 同数（未被这两批改动）。**这就成了下一批（5d）的全部内容。**
3. **5b 的剩余边界**（诚实清单）：
   * 稀疏 RB 掩码（非连续分配）→ 设备不覆盖，回退宿主。kernel 的公式其实支持
     （`stride=1`、位置=子载波偏移），但要求本跳第一个 PRB 带 DM-RS；没有生产几何用到过。
   * 分裂尾块的几何（`covers_hop == false`）→ reformat 本身就不挂，TA 自然也不挂。
   * `nof_layers > 1` 只在单测里出现过（几何带每层 comb，K7 逐层取样）。
4. **5a 的遗留**：K5 的加固只在 `syn025_25`/`syn004_4` 上回归过，另外 5 种形状没跑过。

---

## 5. 这一轮踩到的三个坑（别再踩）

1. **★ 引擎析构里不要释放被零拷贝 wrap 映射过的页**。
   进程级 wrap cache（`shared_queue`）活得比引擎长，静态析构期碰 Metal 会在
   `All tests PASSED` **之后** abort：`mutex lock failed: Invalid argument`（实测，exit 134）。
   ⇒ `~mmse_engine_impl()` 保持 `= default`，理由写在注释里。
2. **两个 `build_ta_tables` 定义**（一个在匿名命名空间、一个在 `ocudu::metal`）会让调用点
   **ambiguous**：MSL 侧的同名函数、C++ 侧的命名空间边界都要看清楚。
   已把定义统一到匿名命名空间（`ocudu::metal` 能看见它）。
3. **`min_dft_size` 是 128 不是 2048**（本文早先写错）。4 PRB → **128**、25 PRB → **256**；
   §4.5 的实测值是对的，推导结论错了。设备侧现在直接问宿主（`get_idft_size`）。

---

## 6. 纪律（不变）

* 改 `.metal` 后 **先删 metallib** 再 build（`rm -f .../ocudu_mmse.metallib`），否则静默用旧的；
* 新校验挂到**已有** harness（5a 的教训），不要造新二进制；
* 每个 kernel 的循环上界必须是编译期常量、索引必须夹紧、不许读
  `[[threads_per_threadgroup]]`、barrier 前不许早返回（§48.131(c)）；
* `run_leg.sh` 只用 Ctrl-C 停；`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 里有用户的改动
  （六个 PUCCH 资源键 + `max_consecutive_kos`），**不要动**。


---

## 7. ★ 阶段最终状态（2026-09-19，5b + 5c + 5d 收尾）

**一句话**：融合车道最后一个设备→宿主的数据读（为 rsrp/noise_variance/ta_us 回读整张网格）
在 **5c 之后归零（1.39 → 0.00 读/跳，0 字节）**；三个上报量都在设备上算、都在设备上发布；
TA 的代价 **+38 µs/lane** 已用单变量腿定位为**尾延迟**并**接受**（空口无可测伤害）。

| 批次 | 提交 | 结果 |
|---|---|---|
| 5a | `94df4144a2` | rsrp 走设备 + 发布路径（空中已验证）|
| 5b | `1826e01e07` | `ta_us` 走设备（K7 + `dft_dit` + K6 → 后来合并）；27 捕获 `ta_us` 逐位相同 |
| 5c | `986c991742` | `OCUDU_CE_HOST_GRID` 默认 0：**读 0.00/跳**；顺带修掉 `noise_variance` 上报错源 |
| 5d | `503990ca5f` | TA 链**合并成一条 dispatch**（`mmse_ta_chain`，蝶形抽到 `ocudu_dft_butterflies.h` 共用）|
| 5d+ | `413ef13f94` | 启动时打印 `[ta_impl] …`：腿自己交代加载了哪种实现 |

**五条腿**：`5a-pubpath`(读1.39, CRC73.35%) / `5b-devta`(读1.39, 66.63%*) /
`5c-hostgrid`(**读0.00**, 80.08%) / `5d-fused`(**读0.00**, **81.23%**, RF 0) /
`5d-devtaoff`(TA关, 366.2µs, 81.34%, RF 0)。*只有真正跑流量的那一分钟是 73.90%。

**三个结构性假设全被实验证伪**（线程组内存 / barrier / dispatch 条数）——代价是
**把短依赖链挂在命令缓冲末尾的尾延迟**，不是工作量。**已接受**：6% busy 换读侧归零，
CRC/RT 无差别。备选（fence 拆缓冲）写在设计文档 **§18.7**，**不做**（会引入间歇性错值的可能）。

**契约仍 7/8**：唯一 FAILED 项已从"读"换成"**写 0.27/跳**"（903 次 / 3.38 MB，三批都没动过它）
⇒ **这就是下一批（5e）的全部内容**。活着的计数点：y pad 行 memset、epoch 上传（56 B）、
demapper 的 `sym_staged`/`nv_staged`（只在缓冲非页对齐时走宿主）。**先量清各占多少。**

**未做的边界（诚实清单）**：稀疏 RB 掩码 / >2048 变换 / 分裂尾块 / `nof_layers>1` 只在单测 /
5a 的 K5 只在 2 种形状回归过。详见设计文档 §18.6。

**下一个会话的第一件事**：读设计文档 **§18**（阶段总结，含 §18.7 fence 备选的完整说明）
与 **§19**（5e：写侧是谁 + 设备侧建表已做）。

## 9. 5e 已完成（`6e53109ffa`）：等化器的 gather 表改由**设备**建

* 新 kernel **`eq_build_gather`**：一个线程一个 (分配 PRB, 跳内符号)，从几何
  （`rb_words[]`/`first_symbol`/`nof_symbols`/`dmrs_sym_bits`/`active_re(_dmrs)`）展开 tap+entry 表，
  写进**引擎自己的两个页对齐缓冲**（按最大尺寸一次分配 ≈370 KB，零拷贝 wrap）；
  **只在几何变化时**编码 builder dispatch（缓冲跨跳保留）。`ch_gather_desc` 新增 `geometry`。
* 旋钮 **`OCUDU_EQ_DEV_TABLES`**（默认 1；0 = 宿主建表+上传，即 A/B 另一臂）。
* 判据 **`OCUDU_EQ_TABLE_CHECK`**：设备表 vs 宿主表**逐元素对拍**，
  **27/27 捕获完全一致**；27 捕获 A/B（默认 vs `=0`）四个 dump **全 0 差异**；
  写入/跳 **7 → 5**；等化器单测 `ALL OK`、信道估计单测 `All tests PASSED`、`recoveryCount=0`。
* **★ 两个坑（都是这个对拍抓的）**：(1) `dest` 写成"PRB 内序号"（应为**符号内** `rank*n_act+d`）
  ——`_llr` 差 3% 而 `.bin` 不变；(2) `eq_encode_gather_dispatch()` 拿"宿主 blob 非空"当"表已建"的
  代理，设备路径不填它 ⇒ **gather dispatch 根本没编码**。**教训：换了实现之后，"XX 非空"这类代理
  判断会静默失效；两个实现的逐字节对拍要在写代码同一步就装上。**
* **✅ 空中腿 `5e-devtables_0919_2000`（`5bd33639ab`）通过**：写**字节 3.38 MB → 5.5 KB（÷592）**、
  次数 0.27 → **0.13/跳**；读仍 0.00；契约 7/8（唯一 FAILED 仍是写）；CRC 79.24%（逐宽度与 5d 一致）、
  RF failure **0**、`ce device estimates` 41272/0、wrap 0 失败。
  腿自己印了两行 provenance：`[ta_impl] fused chain…` / `[eq_impl] gather tables: built on the device`。
* **写侧仍未到 0**：空中剩 **500 次 `h_starts` 上传（5 500 B，平均 11 B，只在分配变化时）**
  + **1 次 epoch 表（56 B）**。两者都是几何（每符号估计起始偏移；cp+scs 推出的符号起始时刻）。
  **下一批（5f）**把它们也搬到设备，写侧才可能真正归零、契约才可能 8/8。
  备注：`h_starts` 在 replay 里是 5 次/跳（冷缓存），空中 0.13/跳（按内容缓存命中）——**次数判据看的是 500 这个数**。

## 10. 5f 第一部已完成（`9e36fef3ed`）：`h_starts` 进参数块 + 修掉一个既有缺陷

* 每 run 的起始表不再上传（放进 `equalize_strides` 参数块，和 `reformat_stage::offsets` 同一路数：
  本来就是几何）；单符号 kernel 根本不读它，那次上传一并删除。**replay 写入/跳 5 → 1**。
* **★ 顺带暴露并修掉一个既有缺陷**（先在基线上复现确认）：**宿主 staging 的批处理 run**
  把估计 memcpy 进紧凑缓冲，却仍用调用方原偏移填起始表 ⇒ **每个符号都读第一个符号的估计**
  （单测 `OCUDU_EQ_DEFER_ENCODE=1` 的 12 符号用例：`bad symbols 11 of 12`）。
  现在 staged run 用 `k*h_stride`、device run 用估计器绝对起始 ⇒ `bad symbols: 0`、`ALL OK`。
  **车道自身不受影响**（它读设备估计），但任何批处理宿主 staging 估计的路线都受影响。
* 判据：等化器单测两种模式全过、CE 单测 `All tests PASSED`、27 捕获 A/B 四个 dump 全 0 差异、
  `recoveryCount=0`。
* **⚠ 契约仍是 7/8**：空中只剩 **1 次 56 B 的 `symbol_start_epochs` 上传（每配置一次）**。
  **5f-2（下一步）**：把它也搬到设备（被 K4 与 K0-a 读取；最干净是让 kernel 从 (cp, scs) 直接算）。
  做完写侧才真正为 0、契约才可能 **8/8**。

## 8. 5e 第一步已完成（`57a5ca6cbc`）：**写侧就是等化器的 gather 表**

* 仪表：`phy_pipeline_crossings::count_host_write_site("名字", bytes)`——**内部仍调用
  `count_host_write()`**（分项不可能与判据总数不符），分项印在契约行下面，按次数排序；
  八个活着的写点全部命名。**分项对不上总数 = 还有匿名写点**（第一次量时就是这样：7 次里只认出 1 次，
  后来才补上等化器与 DFT 两处）。
* **量出来的**：`equalizer: table uploaded (cache miss)` **6 次/事件**（25 PRB 共 26 612 B，
  ≈1065 B/PRB）+ `ce: symbol start epochs uploaded` **1 次/事件（56 B）**，其余站点全静默。
  空中腿的 `903 次 / 3 379 988 B` 正好 = **129 个上传事件**（129×26 668 ≈ 3.44 MB，差 2%）。
* **为什么是 ~4% 的跳**：gather 表描述**分配**（注释原话），按内容缓存；调度器换一次授予的 PRB 就 miss。
* **有证据地排除**：y pad rows（`device_y_writes=4724 y_write_fail=0` ⇒ 设备写 y，宿主没走这条路）、
  pad slots（默认关）、demapper staging（缓冲页对齐）、DFT wrap-copy（`wrap_copies=0`）、CFO 进位（默认设备）。
* **5e 的做法（未做）**：让**设备自己建这 6 张表**——表内容是纯几何（`{symbol, entry_base, nof_entries}`
  与 `{subc, dest}`），K3 已经在设备上算同一套映射，而几何本来就每跳作为 kernel 参数进设备。
  收益：写侧 0.27 → ~0.00。**风险：这条路径在 LLR 关键路径上**，必须过
  27 捕获 `_llr` 逐字节 + BLER/集成测试 + 单测 + 空中腿。
