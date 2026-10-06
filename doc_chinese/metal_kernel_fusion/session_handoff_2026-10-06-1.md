# Session handoff —— `metal_kernel_fusion`（2026-10-06，第 1 次）

> 用法（沿用前两条工作流的约定 ✓）：新会话**只读这一份**就能接着开工 ✓；
> 结构 = ① 本会话做了什么 ② 当前状态 ③ **下一个会话的第一件事**（具体到文件与命令 ✓）④ 未决问题与重开条件 ✓。
> 状态引用：架构文档 `metal_kernel_fusion_high_level_status_and_plan.md`（§2.0ter M0 判决 ✓）、
> 实施文档 `metal_kernel_fusion_design_and_implementation.md`（§2.5–§2.9 ✓ + memo ✓）。

---

## ① 本会话（2026-10-06）做了什么

| 阶段 | 结论 | 出处 |
|---|---|---|
| **立项** | 新工作流：gpu 融合 lane 的 **DFT 网格 → LLR** 段做 kernel 融合（不含 LDPC ✗、不含 FFT ✗）| 架构文档 §1/§5 ✓ |
| **M0 基线** | 腿 `mkf001`（= `mkf-m0-base`）✓：**1 条命令 / 4 次 dispatch / 3 个 metallib**；lane busy median **479.6 µs**；`[ul_gpu_pipeline]` median **1242.8**；健康 CLEAN ✓ | 架构文档 §1.2 ✓ |
| **M0 消去三条腿** | `mkf002/003/004`：消 eq / demap / ce 本体后 busy 仍 **477.4 / 479.7 / 480.0** ✗ ⇒ **内核工作量 < 1 %** ✗（消去确实生效 ✓：CRC 掉到 92.7/98.6/97.8 %、被消去的族读数归 0 ✓）| 实施文档 memo ✓ |
| **微基准** | `wip/dispatch_cost_probe/` ✓：每次 dispatch **~3.7 µs** ✗（4→1 只值 ~11 µs）；一次 dispatch 做 16 倍活 **+2 µs**；宿主成本由**命令缓冲数**决定；网格 8.5k→1M 线程 6.6→14.5 µs | 该目录 README + `run_2026-10-06_m4pro.log` ✓ |
| **M0b** | `mkf005/006`（`event` vs `burst`）：两条腿 `commits/跳` **都是 3.00** ✗ ⇒ 该旋钮未分开 CB 数 ✗；busy 无法区分 ✓；**单 CB 路线端到端更慢 +306 µs** ✗（干净过闸 ✓）⇒ **G0「CB 2→1」撤回** ✗ | memo ✓ |
| **M0c** | `mkf007`（Metal DFT）腿作废 ✗；`mkf008/009` 背靠背一对：**Metal DFT 让跨度 +197 µs 更差** ✗（residency +242、gap +158 ✓），而 CPU DFT 腿与基线逐微秒吻合 ✓ ⇒ **DFT 留在 CPU、不纳入视野** ✓ | memo ✓ |
| **M1.0** | 融合对象 = **`equalize_mxn_batch` + `demod_soft`** ✓（两者网格语义逐字对齐 ✓）；三个待确认点全部关闭 ✓（符号轴由 `gid.y` + `st` 带 ✓；`nv` 留寄存器后差异消失 ✓；`quantize_llr` 照抄 ✓）| §2.5–§2.6 ✓ |
| **M1.1（kernel 部分）** | **完成 ✓**：新 `lib/phy/upper/channel_processors/metal/ocudu_lane_fused.metal` ✓（16QAM、单层 SIMO、寄存器内 `x̂`/噪声 ✓）；已进 `ocudu_metallib_equalizer` ✓；`xcrun metal -c` **exit 0** ✓；合并库 **35 143 B** 且确含 `lane_grid_to_llr` ✓ | §2.8–§2.9 ✓ |

**工具新增** ✓：`wip/dispatch_cost_probe/` ✓、`wip/next_leg_label.sh` ✓（序号自算 ✓）、
**全部读日志的工具已改为泛搜 `doc_chinese/*/wip/logs`** ✓（driver / ul_health / dl_gate / rtt_split / 旋钮清单 ✓
—— 修的是"新工作流的日志根对旧工具不可见" ✗，`mkf001` 由此丢了 `.protocol.txt` ✗）。

**腿编号约定** ✓：`mkf<NNN>-<阶段><臂>` ✓（前 7 条旧标签不改名但计入序号 ✓，映射表在 `README.md` ✓）。

## ② 当前状态

* **代码**：融合 kernel **已在树里、可编、已打包** ✓；**接线未做** ✗ ⇒ 运行时**走不到它** ✗
  （`OCUDU_LANE_FUSE_EQDEMOD` 旋钮尚未存在 ✓）；因此**当前二进制行为与 M0 基线逐字节相同** ✓。
* **默认关闭** ✓ 的两把钥匙设计已定：编译期不加开关（kernel 编进既有库 ✓）、运行时 env 控制路线 ✓。
* **判据**（预登记 ✓）：主判据 = `[ul_gpu_lane] busy` < **479.6 µs** 与 `[ul_pipeline]`/`[ul_gpu_pipeline]`
  median < **1321 / 1242.8 µs** ✓（**报告制** ✓，允许持平/小幅回退但须记 memo ✓）；
  结构证据 = `dispatches`/跳 **4 → 3** ✓；红线 = **CRC 同档 + `gaps=0`** ✓；
  ✗ 不要求：LLR 逐位一致、dispatch 计数本身、CB 数 ✓。

## ③ ★ 下一个会话的第一件事：把接线做完（两处）

1. **均衡引擎** `lib/phy/upper/channel_processors/metal/ocudu_equalizer_metal_engine.{h,mm}` ✓
   加 `enqueue_fused(device 网格 h/y、sigma2、llrs、equalize_params、equalize_strides、llr_stride)` ✓：
   从既有 metallib 取 `lane_grid_to_llr` 建 pipeline ✓（与 `fn_batch` 的建法并列 ✓，见 `:656` ✓），
   绑定顺序照 §2.6 的表 ✓（b0 h / b1 y / b2 sigma2 / b3 llrs / b4 p / b5 st / b6 llr_stride ✓），
   `dispatchThreads:MTLSizeMake(nof_re, nof_symbols, 1) threadsPerThreadgroup:MTLSizeMake(256,1,1)` ✓。
2. **调用方** `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp` ✓ 加路线选择：
   `getenv("OCUDU_LANE_FUSE_EQDEMOD")` 非空且 `mod == MOD_QAM16` ⇒ 调 `enqueue_fused` ✓；
   否则**原样两步** ✓（逐字节不变 ✓）。★ 注意它今天走的是 `channel_equalizer_device_grid` 那条设备网格路径 ✓
   （`:18` 的 include ✓）——**先读懂它如何拿 h/y/llr 三个缓冲与 `llr_stride`** ✓，再决定开关挂在哪一层最省改动 ✓。
3. 然后：全量重编 `gnb` ✓、单测 ✓、`bash wip/probes_off_syntax_check.sh` ✓、重戳指纹 ✓。
4. 飞 **`mkf010`（对照）/ `mkf011`（臂）** ✓（`bash wip/next_leg_label.sh m1-…` 取号 ✓），
   判读用 `ul_health.sh` + `dl_gate.sh` ✓ + §2.3 的判据 ✓。

## ④ 未决与重开条件

| 项 | 状态 | 重开条件 |
|---|---|---|
| `enqueue_fused` 的接线层（引擎 vs 适配器）| 未定 ✗ | 读 `channel_equalizer_metal.cpp` 的设备网格路径后定 ✓（**下一件事的第 1 步** ✓）|
| helper 抽取为共享头 | 未做 ✗（当前是复制 ✓）| 融合路线被证明有价值之后 ✓（抽取会改写交付形状里的两个 kernel ⇒ 需要自己的 A/B ✓）|
| 其它调制（QPSK/64QAM/256QAM）| **M1 不做** ✗（范围决定 ✓）| 融合在 16QAM 上兑现收益之后 ✓ |
| M2（折叠 CE）/ M3（metallib 整合）| 未开始 ✓ | M1 出结论之后 ✓（M2 的前提是 M0 显示 CE 的 8 % 值得 ✗，目前看优先级低 ✓）|
| `lane_grid_to_llr` 的**离线对拍** | 未做 ✗ | 接线完成后用 `ul_chain_replay` ✓ + `ab_replay_bins.sh`（两二进制 ✓，不是两环境 ✗）确认"算的是同一件事" ✓ |
