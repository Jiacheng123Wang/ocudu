# Session handover #6（2026-09-13，本会话结束时的快照，**只写不改**）

> 本文件是**快照**：写下之后不再回填/修改。后续进展写进新的 handoff 或活文档
> `s2_full_chain_design.md`（§48.x 起）与 `full_chain_gpu_uma_zero_copy_refactor_plan.md`（§10.8）。

---

## 0. 一句话现状

**HEAD = `d9f741b31a`**（`apple-silicon`，已 push）。本会话修掉了两个**真 bug**（CE 的 `defer` 默认值、
零拷贝 wrap 的"同地址不同对象"），把 pre-LDPC 链的 OTA 基线从 **999 µs 降到 810 µs**，
并把"批量均衡"的验证从"核"一路铺到"实网布局复现器"。**组的调用方接入（S-7e-batch）尚未提交**，
两条用例未过，卡点、已排除项与新方案见 §4/§5。

树状态：`ctest -L phy` 162/162、`pusch_demodulator_deferred_chain_test` 20/20（连跑）、
默认 `metal_chain_probe` 退出码 0、`OCUDU_PROBE_GAPPED=1` 退出码 0；工作树只有用户自己的
`configs/*` 与未跟踪的 `bler_metal_bg1_z64_*.csv`、`scripts/switch_gnb_plmn.sh`（**不要动**）。

---

## 1. 阶段目标与硬规则（用户已明确）

- 阶段目标：**融合优化 LDPC 之前的 Metal GPU PHY pipeline**。**本阶段不做 LDPC 优化**
  （LDPC 回 GPU 是它自己阶段的事）；用户已同意：**怀疑 LDPC 抢占 GPU 队列时，把 LDPC 换成 CPU 当测试台**。
- 提交信息/注释**不得引用 `doc_chinese/`**；前缀 `GPU-PHY S-<id>:`（构建修复用 `build:`）。
- 后端选择只走 `expert_phy` CLI；env 只用于 debug。TX/DL 保持 CPU。每完成一步：提交 + push。
- 工作方式：代码 + 构建 + 单测 + A/B 门禁 + 提交 + 文档；**OTA 一律由用户跑**，一次给一条命令。
- gNB 必须 `sudo`；**不要用 `| tee`**（会丢退出期探针）；每条腿自己的 `log.filename`；
  跑的时候 agent 完全不动。

---

## 2. 本会话的提交（时间顺序）

| commit | 内容 | 关键数字 |
|---|---|---|
| `757593c84a` 等（会话前段） | S-7a CLI/解析、S-7b 设备网格、S-7c-0/0b/0c 探针 | 见 handoff #5 |
| `bd48984ab4` | S-7c-1b：weights-only 也推迟 | 单测 mean hop 77.4→44.4 µs |
| `6e7b83409f` | **S-7c-2**：`[ldpc_time_sum]`/`[ldpc_time_shape]` + `[metal_stats] mmse_ce guard=` | 本地定标：分层 3663 / 持久 1407 / CPU 48 µs（BG1 Z208，cap 25） |
| `90797e4f6a` | **S-7c-5**：`engine_run()` 的 `bool defer = false` 默认值 bug —— 合并路径一直在同步等待 | **实网：CE 段 377.4→94.5 µs、管线 999.1→810.0 µs、账本闭合 −283+88+9=−186≈−189** |
| `bfd329b454` | S-7e-b0：chain probe **两条路径都预热 + 10 次取最小** | 批量均衡 **1.94×** 且逐位一致（旧结论"批量慢一倍"是没预热的假象） |
| `fdfbcdf2c3` | S-7e-b0b：probe **Pattern E**（2 端口/12 符号/204 RE/带间隙 stride 4096） | 核 + 引擎在实网几何下逐位一致 |
| `b3e07034e4` | S-7e-b0c：probe **Pattern F**（直接驱动适配器 `submit_group`） | 适配器 run 切分/staging/原地写回全部正确 |
| `5769e6e78b` | S-7e-b0d：probe **Pattern G** + `OCUDU_PROBE_GAPPED=1` 复现器 | 复现"first bad symbol 1"（21396 字节差异） |
| `d9f741b31a` | **S-7e-b1**：`wrap_no_copy` **包含式查找** + 均衡/解调引擎按偏移绑定 | 复现器 **21396 → 0** |

---

## 3. 本会话的关键结论（可直接复用，不必重测）

### 3.1 S-7c-5：`defer` 默认值 bug（**已修，实网确认**）

`port_channel_estimator_metal_mmse_impl.h` 里 `engine_run(..., const reformat_stage* reformat = nullptr,
bool defer = false);`，而**合并标准+尾块路径**（13 PRB、`rem_prb=1`，**唯一在实网走的几何**）的调用点
只传 7 个参数 ⇒ `defer=false` ⇒ 引擎走 `run_weights_only()` 同步 `waitUntilCompleted`（~282 µs/跳）。
**单测之所以"通过"**：它的主力几何 `rem_prb == 0` 走另一个调用点（确实传了 `defer`）。
修法：**去掉默认值**（编译器逼所有调用点显式传）+ 传 `merged_defer`。

实网腿 D/E（测试台 = CPU LDPC，同二进制只差这一行）：

| 每 slot | D 修前 | E 修后 |
|---|---|---|
| `[ul_channel_estimation]` | 377.4 µs | **94.5 µs** |
| `[ul_equalization_demod]` | 318.9 | **407.0** |
| `[ul_time_frequency]` | 247.7 | 244.7 |
| `[ul_ldpc_decode]`(CPU) | 55.1 | 63.8 |
| **`[ul_pipeline]`** | **999.1** | **810.0** |

- `run_weights_only n=712 wait=282.03us` → **n=2**；`run_weights_only_async n=3204`。
- 不变量：`hops_gpu=hops=3206`、`hops_no_gpu=0`、`hops_nn=0`、`staged=0`、`guard=0/3208`。
- **EQ/demod +88 µs 的解释**：CE 的批虽推迟，但其 GPU 窗口（146 µs）**排在 eq/demapper burst 之前、
  同一条 backend queue 上**；证据是 EQ/demod 的 `min 229→316`、`median 290→390`——**整条曲线平移**
  （固定延迟，不是新增工作量）。

### 3.2 pre-LDPC 成本结构 = **每 slot 的 dispatch 次数**（本地定标）

新基线（测试台，每 slot）：**TF 244.7 + CE 94.5 + EQ/DEMOD 407.0 + LDPC(CPU) 63.8 = 810.0 µs**。

| 段 | dispatch/commit 次数 | 实测 | 单位成本 |
|---|---|---|---|
| EQ+demapper（burst） | **22 次/occasion**（11 符号均衡 + 11 符号解调，同一 CB） | 407 µs | **~18 µs/dispatch** |
| CE（K1b/K2/K3/K4） | 4 次/hop | 146 µs CB 窗口 | ~36 µs（含排队） |
| TF（DFT 批量） | **1 次/slot**（14 transform 一个 dispatch） | 244.7 µs | ~17 µs/transform（**核内计算**，不是 dispatch 开销） |

本地 `metal_dispatch_probe`（静默机器）：CB 内一次 dispatch ≈ **10 µs 主机编码 + ~4 µs GPU**；
`commit-per-dispatch` 19–24 µs；`commit+wait per dispatch` 82–129 µs。

### 3.3 测试台协议（用户已同意，**后续 pre-LDPC A/B 一律照此**）

`--pusch_ldpc_decoder_type auto`（CPU LDPC）⇒ **GPU 队列留给 pre-LDPC 链**，各段探针即账本
（实测 pipeline = 各段之和，无重叠：810.0 ≈ 244.7+94.5+407.0+63.8 ✓）。
基线命令见 §6。

### 3.4 LDPC 侧记录（本阶段**不做**，仅备查）

- 实网几何 **BG1 Z208**、`cap=6`、mean 2.84 迭代、CB 的 **GPU 窗口 3.29 ms**；
  另有 `bg2 z18`（11 B 极小 PDU）51 次里 50 次失败。
- 本地 BG1 Z208 rate 0.5 cap 25：分层 **3663 µs**（GPU 3346）/ 持久 1407 / 洪泛 2524 / **CPU 48 µs**。
- 机理：分层核每轮 46 层 = 46 次 dispatch × ~4–24 µs，代价 ∝ 层数×迭代数。

### 3.5 零拷贝 wrap 的**不变量**（本会话最重要的架构结论）

> **同地址必须绑同一个 MTLBuffer 对象**——这是两个阶段的访问被 Metal 关联起来的唯一依据
> （`shared_queue::wrap_no_copy` 的注释原文）。

组提交（一次 wrap 整组）与逐符号（wrap 每个符号切片）会给**同一块内存**产生**不同对象** ⇒
demapper 在均衡写之前就读了符号 1..N（症状：**只有第 0 个符号正确**）。
`d9f741b31a` 的修法：`wrap_no_copy(device, ptr, len, size_t* offset)` —— **传 offset 时启用包含式查找，
返回"覆盖请求范围的*最大*已缓存映射"+偏移**（"最大"是关键：旧的小映射不得遮蔽整组的大映射）；
不传 offset 的调用方（CE、LDPC 引擎）行为不变；均衡/解调引擎的 `wrap_buffer()` 返回 `{buffer, offset}`，
绑定点改用 `setBuffer:offset:`。

### 3.6 flush hook 的真实行为（批处理的**闸门**）

- `shared_burst::set_flush_hook(ctx, hook)` 换钩子时**会先把上一阶段的 pending flush 掉**（不丢弃）；
- `eq_flush_hook()` 在 `enc == nil` 时返回 nil（pending 仍留在引擎状态里）；
  真正编码的时机是**下一个阶段第一次调用 `encoder()`**（先 flush、再比 pipeline 决定是否插屏障）⇒ 顺序正确；
- hook 内**本来就会把 pending 合成一次 dispatch**，条件是
  `same_geom && same_strides && same_sigma && same_h`，其中
  `same_h = (next.h.buffer == head.h.buffer) && (next.h.layer_stride == head.h.layer_stride)`；
- ⇒ **air 上没批成的原因只剩 `same_h`（或 `same_strides`）不成立**。
- **现成但没报出来的探针**：`equalizer_metal_engine::batch_dispatch_count()`（注释：*"proves that a group
  really took the batched kernel instead of falling back to one dispatch per symbol"*）——**下一轮第一步就报它**。

### 3.7 并发 1/60 是**组提交引入的**

HEAD（未接组提交）二进制连跑 20 次 **20/20 全过**；接上组提交后 60 次里 1 次不匹配。
⇒ 不是既有 flakiness。最可能：批量分支**立即编码** vs 逐符号**推迟编码**混用在**进程级共享**的
`shared_burst` 上（单线程确定、多线程互相踩）。

---

## 4. 在飞但**未提交**的改动（组提交那套，已回退）

已在会话中写出、验证过思路、最终 `git checkout` 回退的内容（下一轮**按 §3.6 的新方案可以不做**，
若仍要走这条路可照此重做）：

1. `pusch_demodulator_impl.{h,cpp}`：`deferred_chain` 分支收集 `group_symbol` 后一次 `submit_group()`；
   为每符号加独立存储 `group_ch_re_copy(std::optional<dynamic_re_buffer>)/group_ch_re_view/
   group_device_ch_estimates/group_host_ch_estimates/group_noise_var_estimates/group_symbols`，
   `get_ch_data_re()`/`get_ch_data_estimates()` 加 `i_group` 参数、`no_group` 哨兵。
2. `channel_equalizer_metal.{h,cpp}`：`symbol_plan.device_slice`；`resolve_plan()` 自己解析设备切片并推出
   `device_noise_variance`（分组与执行口径一致）；分组谓词加 `device_slice` 一致 + **设备切片等间隔**校验；
   `submit_batch_run()` 设备快路径（校验 `base/layer_stride` 相同、`offset` 差分等间隔，绑设备切片并
   跳过主机 memcpy，`s_dev` 直接用切片里的 `noise_var`）+ `ch_est_source()` 计数。
3. 结果：`metal_back_ends_match_the_cpu_chain` **0/4896 LLR 差异 ✓ 通过**（原 4485）；
   剩 ① `metal_equalizer_binds_the_estimator_device_buffer`（同一次运行里"设备绑定"与"主机 gather"
   的软比特不一致 0 vs −120）② 并发 1/60。两条都未定位完 ⇒ **回退保绿**。

---

## 5. 下一步（按顺序，带锚点）

1. **报 `batch_dispatch_count()`**（几行探针，最高性价比）：
   `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.mm:814` 有 getter，
   加进 `[metal_stats]` 出口报告（例如 `[metal_stats] eq_batch groups=… dispatches=…`；
   需要时再加一个"组数"计数器）。**跑一轮 air** 即可分辨：
   - `dispatches ≈ groups`（没批）⇒ 按 §3.6 打印每条 pending 的 `h.buffer/layer_stride/eq/nv` 找断点；
   - 若 `same_h` 成立却没批 ⇒ 查 flush 触发时机。
2. **若 `same_h` 不成立**：考虑把 hook 的 run 条件放宽（例如允许不同 `h.buffer` 时用逐条 `h.offset`
   做组 staging——hook 已经逐条拷 `h.buffer + h.offset`，所以**可能只需放宽 `same_h` 的 buffer 比较**）。
   这一步能**不改调用方**就拿到批量收益（预期 EQ/demod 407 → ~150–250 µs、管线 810 → ~550–650 µs）。
3. **不要**再走 §4 的调用方改法，除非第 2 步证明引擎内部做不到；即便要做，先解决并发（让批量也走推迟编码）。
4. LDPC 阶段与 S-7d/S-7e/S-7f 仍按路线图 §10.8 排在后面。

---

## 6. 命令备忘

**构建/门禁（macOS，本机）**

```bash
cmake --build build -j14                                   # 全量；grep -c "error:" 应为 0
cmake --build build -j14 --target metal_chain_probe pusch_demodulator_deferred_chain_test \
      channel_equalizer_metal_unit_test metal_dispatch_probe ldpc_metal_unit_test
./build/lib/phy/upper/channel_processors/metal/metal_chain_probe        # 退出码 0
OCUDU_PROBE_GAPPED=1 ./build/lib/phy/upper/channel_processors/metal/metal_chain_probe   # 退出码 0
./build/tests/unittests/phy/upper/channel_processors/pusch/pusch_demodulator_deferred_chain_test   # 5/5
cd build && ctest -L phy                                   # 162/162
```

**OTA（测试台腿，用户跑；一条一条给）**

```bash
cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml expert_phy --pusch_channel_estimator_algo metal_mmse --pusch_dft_type metal --pusch_channel_equalizer_backend metal --pusch_ldpc_decoder_type auto log --filename /tmp/gnb_x.log > /tmp/gnb_x.out 2>&1
```

```bash
grep -aE "^\[(mmse_time_sum|mmse_time_shape|ldpc_time_sum|ldpc_time_shape|ul_channel_estimation|ul_ldpc_decode|ul_pipeline|ul_equalization_demod|ul_time_frequency|ul_mac_pdu_size)\]|^\[metal_stats\] (mmse_ce|ldpc_decoder|pusch_demod|equalizer|burst|dft|eq_batch)" /tmp/gnb_x.out
```

- `OCUDU_MMSE_DEBUG=1` 走 `sudo env VAR=1 …`；`OCUDU_PROBE_GAPPED=1` 只对 probe 有效。
- 对照要看**同几何档**（`[mmse_time_shape] prb=13 npt=3`）和**成功 slot** 的段值；
  腿 E 的 CE 跳数（3206）是腿 D（766）的 4.2 倍而送达量相同 ⇒ 多出来的跳在**失败尝试**的 slot 里。

---

## 7. 探针索引（新增的都在这里）

| 探针 | 位置 | 说明 |
|---|---|---|
| `[mmse_time_sum]` / `[mmse_time_shape]` | CE metal impl | `pre/stage/submit/unpack/cpl_*/sigma2/corr/gpu_path(gpu_wait)/defer_wait`；**`gpu_wait` 实为 CB 的 GPU 窗口** |
| `[metal_stats] mmse_ce … guard=命中/总数` | `ocudu_metal_mmse_engine.mm` | 引擎入口守护（单测恒 0，说明收集点在提交之后） |
| `[ldpc_time_sum]` / `[ldpc_time_shape]` | `ldpc_decoder_metal.cpp` | `wall/pack/submit/gpu/gap/unpack` + 迭代/上限直方图 + 按 (mode,bg,z) 分档 |
| `[mmse_eng] wrap/cb/encode/commit/wait` | 引擎相位计时器（`OCUDU_MMSE_DEBUG=1`） | **定位"走了哪条入口"的唯一手段** |
| `[metal_stats] burst dispatches (equalizer=/demapper=)` | `ocudu_metal_burst.mm` | 每 occasion 的 dispatch 数（实网 22） |
| `[metal_stats] equalizer ch_est device=/staged=` | `channel_equalizer_metal.cpp` | 设备零拷贝 vs 主机 gather |
| `batch_dispatch_count()` | `ocudu_equalizer_metal_engine.mm:814` | **已有但未报出**，下一步要报 |

---

## 8. 陷阱清单（本会话踩过的）

1. **带默认值的"模式"参数**：`bool defer = false` 让一条路径静默走错分支，且单测覆盖另一条 ⇒
   模式参数**不要给默认值**；探针要能回答"走了哪条路"（`[mmse_eng]` 的入口名就是证据）。
2. **比较两条路径必须两条都预热 + 重复取最小值**（"批量慢一倍"就是没预热的假象）。
3. **零拷贝的"同地址 ⇒ 同对象"不变量**：任何"把小缓冲区当大缓冲区绑"的优化先解决它。
4. **`gpu_wait` / `defer_wait` 的口径**：前者是 GPU 窗口（不是等待），后者是"到收集点的跨度"
   （≈一个管线周期），两者都不能当队列证据。
5. **A/B 必须带机器/链路指纹**：同轮里一行未改的段也会一起变好；用**同几何档**比。
6. **退出期静态析构顺序**：atexit 报告用的容器/互斥量必须**故意泄漏**（`new` 一次不释放）。
7. **`ocudulog` 会截断日志**（`fopen(...,"wb")`）⇒ 每次 A/B 自己的 `log.filename`。
8. **Metal tests 不在 `all` 里** ⇒ 必须显式 `--target`，否则跑的是旧二进制（本会话踩过）。
9. **`MTLSizeMake(256,1,1)` 的线程组不得宽于 grid**（至少在没有证据前保持 ≤ grid；本会话加了夹取但
   未证明必要，已回退）。
