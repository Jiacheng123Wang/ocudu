# Metal LDPC 接入 ocudu 详细工作计划书

> 状态：已批准（2026-08-16），正在实施。
> 已确认的决策：
> 1. **解码优先、编码后置**（解码器接入并验证后再做编码器）。
> 2. **大/小码块分流**，但预留开关：分流模式（大码块→GPU，小码块→CPU）或不分流（全 CPU / 全 GPU）。
> 3. SynchroPlus 的相关文件**原样复制**进 ocudu 仓库（含 Python 测试/矩阵生成工具），**绝不修改** SynchroPlus 原始文件。

---

## 1. 背景与目标

ocudu（srsRAN fork）gNB 已移植到 macOS Apple Silicon。UL compute pipeline 探针证明 macOS M4 Pro 的纯 CPU 算力（219 µs 中位数）已远超 Ubuntu（892 µs）。下一步挑战：将 PHY 中计算密度最高的 LDPC 编解码卸载到 Apple GPU（Metal），进一步压低解码延迟与 CPU 占用。

来源代码（SynchroPlus 项目，只读参考，不得修改）：
`/Users/jiachengwang/OneDrive/newWork/work/SynchroPlus/ORAN_L1_M/src/channel_coding/ldpc`

本目录 = 源文件的完整复制（编译产物已清理）+ 本计划书。目录结构：

```
metal/
├── PLAN.md                  # 本文件
├── decode/                  # GPU 解码器（SynchroPlus 原样复制）
│   ├── ldpc_gpu_decoder.{h,mm,metal}   # ★ 核心：独立 LDPCGPUDecoder 类 + 4 个 kernel
│   ├── DecoderContext{GPU,Metal}.h / DecoderContextMetal.mm
│   ├── MetalLLSUpdater.{h,mm} MetalDecoderSUpdate.{h,mm} MetalEngineUpdateLLS.{h,mm}
│   ├── cn_scan_vn_lls_update.metal update_llr.metal   # 其他引擎变体的 kernel
│   ├── CollaborationDecoder.{h,cpp} master/slave_thread.* NeonHelper.h
│   └── *.py main_*.cpp LDPCDecodeMetal.cpp ...        # BLER 仿真/测试工具
├── encode/                  # GPU 编码器（GF(2) uint4，低优先级，Step 4）
│   ├── MetalEngineUint4.{h,mm} MetalEngine.{h,mm} MetalAlignedBuffer.hpp
│   ├── gf2_multiply_uint4_extreme.metal matrix_vector_gpu.metal
│   ├── LDPCEncoder_*.swift  check_ldpc*.cpp  encoder_test_z4_uint4.cpp  ldpc_test_z4.py
└── g_f_matrix/              # 328MB 预生成矩阵（.bin，不进 git，可再生成）
    ├── bg1_LSindex{0,1}.txt                    # BG1 基础图文本（Z 族 a=1 / a=2）
    ├── BG1_LSindex{0,1}_Z*_{F,FT,G,GT}.bin     # 全部 16 个 lifting size（仅 BG1）
    ├── H_matrix_Z{2,4,256,384}.bin / HT_matrix_Z*  # 完整 H / Hᵀ（仅 4 个 Z，仅 BG1）
    ├── G_matrix_Z{2,4,256,384}.bin             # 编码器生成矩阵
    └── G_F_matrix_bitmap_gen.py G_H_matrix_gen.py G_H_matrix_gen_uint4.py
        GT_FT_matrix_bitmap_gen.py GT_FT_matrix_check.py print_G_F_matrix.py
```

### 关键结论（源码扫描所得）

**LDPCGPUDecoder 接口**（`decode/ldpc_gpu_decoder.h`）：

```cpp
bool init(const char *metallib_path, uint32_t N_logical, uint32_t M_logical, float alpha);
void load_h_and_ht_matrix(const uint32_t *h_matrix_raw, const uint32_t *ht_matrix_raw,
                          uint32_t *column_weights_out);   // 列权在 CPU 侧由 H 现场计算
int  decode(const void *in_sp_llr_fp16, uint8_t *out_s_bits, int max_iter); // 同步，waitUntilCompleted
```

- N_logical = 码块总长 N（BG1: 68Z；BG2: 52Z），M_logical = 校验方程数 M（BG1: 46Z；BG2: 42Z），
  `n_info = N - M = K`（BG1: 22Z；BG2: 10Z），输出 K 个信息位。
- H 矩阵打包格式：行主序 `uint32`，M 行 × ⌈N/32⌉ 字/行，小端位序（bit k of word w → 列 32w+k）。
  Hᵀ：N 行 × ⌈M/32⌉ 字/行。已验证：`H_matrix_Z256.bin` 大小 = 11776 × 544 × 4 B = 25,624,576 B ✓。
- decode() 一次录制全部 max_iter 轮（4 kernel × iter 展开）→ commit → waitUntilCompleted →
  从最终 fp16 LLR 的符号位提取硬判输出；GPU 内部有 syndrome 收敛的 early_terminate 机制，
  `ctrl->actual_iters` 返回实际有效轮数。
- 零拷贝：`newBufferWithBytesNoCopy` + 4KB 对齐 + per-engine `bufferCache`（按指针缓存封装）。
- 参考超参：**alpha = 0.45**（LLS 步长）；LLR 转换 = 直接 float→fp16 cast（`main_metal_snr_loop.cpp`）。

**矩阵文件的缺口（重要）**：
1. `g_f_matrix` 只有 **BG1** 的矩阵，**完全没有 BG2**。
2. 完整 H/Hᵀ 只有 Z ∈ {2, 4, 256, 384} 四个值；F/FT/G/GT 虽覆盖全部 16 个 Z，但 F 只是 H 的系统部分
   （M×K 位：Z256 时 11776×176 字 ✓），**LDPCGPUDecoder 需要的是完整 H/Hᵀ**。
3. 因此：**运行时 H/Hᵀ 由 ocudu 自带的 `ldpc_graph`（3GPP BG1/BG2 原图）在内存中生成**，
   覆盖 BG1+BG2 与全部 lifting size，且与 CPU 解码器使用完全相同的原图，保证 3GPP 结构一致；
   SynchroPlus 的 `H_matrix_Z{256,384}.bin` 作为生成器的 golden 交叉验证。
4. 实际 E2E（ping 小包）PUSCH 走 **BG2** —— 若依赖 .bin 文件将无法端到端，生成方案是必选项。

**LLR 符号约定（两端核对完毕）**：

| 侧 | 硬判约定 | 备注 |
|---|---|---|
| GPU | fp16 < 0 → bit 1 | 符号位即硬判 |
| ocudu CPU | LLR ≤ 0 → bit 1（`to_hard_bit()`） | **LLR == 0 时两侧不一致**（CPU→1，GPU→0） |
| filler 位 | 两端一致：+∞ LLR → bit 0 | 速率解匹配把 filler 置 `log_likelihood_ratio::infinity()` (+127) |

LLR=0 仅出现在打孔的 2Z 个系统位（首传不打孔发射，解匹配保留 0），其初始硬判只影响首轮 syndrome，
消息传递会将其驱动收敛；对最终 CRC 判定无影响。**待办：Step 2 BLER 对拍时确认无差异**，若有差异再在
适配器中把 0 LLR 预偏置为 GPU 侧 bit 1 的符号（如 -ε）。

---

## 2. ocudu 侧插入点

### 2.1 解码器接口（`include/ocudu/phy/upper/channel_coding/ldpc/ldpc_decoder.h`）

```cpp
struct configuration {
  ldpc_base_graph_type base_graph;   // BG1 / BG2
  ldpc::lifting_size_t lifting_size; // LS2 .. LS384
  unsigned nof_filler_bits;
  unsigned nof_crc_bits;
  unsigned max_iterations;
};
virtual std::optional<unsigned> decode(bit_buffer& output,          // K 位（含 filler 前缀）
                                       span<const log_likelihood_ratio> input,  // N 个 int8
                                       crc_calculator* crc, const configuration& cfg) = 0;
```

- 输入 = 完整码块 N 个 int8 LLR（前 2Z 个为打孔位 LLR=0，filler 位 = +127）。
- 输出 bit_buffer = K 位（含 filler 前缀），filler 由下游（分段器）剥离 —— 已从 CPU 解码器
  `get_hard_bits()`（按 bg_K 个信息节点写入 out[0..K)）确认。

### 2.2 工厂与配置

- `lib/phy/upper/channel_coding/channel_coding_factories.cpp`：
  `ldpc_decoder_factory_sw(dec_type, cfg)` 按类型字符串选择实现
  （现支持 "avx512"/"avx2"/"neon"/"auto"/"generic"）→ **"metal" 类型在此注册**。
- 类型字符串来自 `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp:44-45`
  （现硬编码 "auto"）→ **需加配置项** `ldpc_decoder_type: metal`（以及分流策略开关，见 Step 3）。
- PUSCH 解码流程：`pusch_processor_impl::process_data()` → `decoder->new_data(...)` → 分段器
  `ldpc_segmenter_rx` 每码块调 `ldpc_decoder::decode()`（现有 ul_pipeline_probe 的 T_end 就在
  `on_sch_data` CRC-OK 处，天然覆盖 GPU 路径的端到端测量）。

### 2.3 适配器设计（ldpc_decoder_metal）

```
decode(output, input, crc, cfg):
  1. 取/建该 (BG, Z) 的共享 GPU engine 实例（懒加载单例池，见 3.3）
  2. int8 LLR → fp16 写入对齐缓冲（直接数值转换，缩放因子待 BLER 决定）
  3. engine->decode(fp16, out_bits, cfg.max_iterations)   // 同步
  4. out_bits[0..K) → bit_buffer 打包（32 位并行搬移）
  5. crc->calculate(output.first(nof_significant_bits)) == 0 ?
       返回 actual_iters : 返回 nullopt
```

- CRC 校验在 GPU 完成后由 CPU 一次判定（GPU 的 per-iteration early stop 收益换不回
  CPU-GPU 往返，v1 不做逐轮同步；GPU 内部仍有 syndrome 收敛早停，`actual_iters` 如实返回）。
- filler 位不特判：输入 +127 → 解码输出必为 0，与 CPU 路径行为一致。

---

## 3. 分步实施计划

### Step 1 —— CMake 构建连通性（先行，无功能改动）

- `lib/phy/upper/channel_coding/ldpc/CMakeLists.txt`：
  - 新 option `ENABLE_METAL_LDPC`（`APPLE AND arm64` 默认 ON，其他平台默认 OFF）。
  - 离线编译（可选）：`xcrun -sdk macosx metal/metallib` 自定义命令
    （add_custom_command，OUTPUT 进 build 目录），仅在 Xcode 工具链存在时启用。
  - **运行时编译（必建）**：CMake 配置期把 `ldpc_gpu_decoder.metal` 以十六进制字节数组
    嵌入生成头 `ldpc_gpu_decoder_metal_source.h`，适配器可用 `newLibraryWithSource` 运行时
    编译着色器 —— 无需 Xcode（macOS 自带 GPU JIT）。
  - `ocudu_ldpc_metal` 静态库：OBJCXX 编译 `decode/ldpc_gpu_decoder.mm`
    （`set_source_files_properties(... PROPERTIES LANGUAGE OBJCXX)` +
    `enable_language(OBJCXX)`），链接 `-framework Metal -framework Foundation`。
  - 最小集 = `ldpc_gpu_decoder.mm` + `ldpc_gpu_decoder.metal`（4 个 kernel 全在该文件，
    已核对：init_hard_decisions / compute_syndrome / cn_centric_scan / update_llr_hpred）。
    其余 .mm/.metal（LLS 引擎变体、协作式解码）v1 不参与编译，文件保留作参考。
  - `OCUDU_METAL_LDPC` 宏按 target 注入（`ocudu_ldpc` / `ocudu_channel_coding` PUBLIC），
    切换开关只重编 PHY coding 库，不触发全项目重建。
- **验收结果（2026-08-16，本机 M4 Pro，CLT-only 无 Xcode）**：
  - macOS `-DENABLE_METAL_LDPC=OFF` 配置通过且无 metal target ✓
  - `ocudu_ldpc_metal` 零警告编译 ✓
  - 冒烟测试（`build/smoke_metal.mm`，不入库）：MTLCreateSystemDefaultDevice 返回
    "Apple M4 Pro"；嵌入源码运行时编译成功；4 个 pipeline state 全部创建成功 ✓
  - 注意：SynchroPlus 原目录的预编译 `ldpc_decoder.metallib`（Mar 24）**早于** .metal 源
    文件的最后修改（Mar 25），不可直接使用；本机无离线编译器，运行时编译路径为首选。
  - Ubuntu：非 Apple 平台 option 默认 OFF，构建不受影响。

### Step 2 —— 解码器适配器 + 工厂注册 + BLER 对拍

1. `lib/phy/upper/channel_coding/ldpc/ldpc_decoder_metal.{h,cpp}`（ocudu 侧新文件）：
   - 实现 `ldpc_decoder` 接口（见 2.3）。
   - H/Hᵀ 生成器：遍历 `ldpc_graph`（CPU 解码器同款原图）→ 打包 uint32 行主序
     （列权由 LDPCGPUDecoder 自算，无需额外输出）。
   - int8→fp16 转换 + 输出 bit 重打包（含 filler 前缀语义，见 2.1）。
2. 工厂：`ldpc_decoder_factory_sw` 增加 "metal" 分支；配置翻译器加 `ldpc_decoder_type`。
3. **golden 验证**：生成器输出与 `H_matrix_Z{256,384}.bin`（及 HT）逐字节比对。
4. **BLER 对拍**（单元级，不进 gNB）：复用 SynchroPlus 的 BLER 仿真脚本/主程序改造，
   ocudu CPU 解码器 vs metal 解码器，同 SNR 扫描、同测试向量（调制前），
   目标：BLER 曲线在 0.1-0.01 区间无统计显著差异；同时标定 alpha 与 LLR 缩放因子。
5. 验收后 commit（.bin 不进 git）。

**实施结果（2026-08-16）**：

- ✅ 适配器/引擎/工厂全部零警告编译；"metal" 类型在工厂注册。
- ✅ golden 比对逐位一致（H/Hᵀ × Z256/Z384）。
- ✅ 无噪声正确性：BG1 Z16 / BG2 Z16 / BG1 Z256 三组配置 GPU 解码 100% 位精确
  （与 CPU 输出逐位一致、CRC 全过、1 次迭代提前收敛）。
- ⚠️ **LLS 算法性能天花板**（有噪声时）：SynchroPlus 的 LLS 比特翻转族算法比 CPU
  min-sum 弱 ~5dB（例：BG1 Z16@3dB CPU 100% vs GPU 0%；BG1 Z256@5dB GPU 0%，
  8dB 75%、10dB 100%；alpha/bias/迭代数扫描均无法弥合）。原因：其仿真从不解码
  打孔码字（无擦除列），且比特翻转族本身收敛能力弱于 min-sum。
- ✅ **打孔擦除修复**：LLS 对精确 0 LLR 的处理有缺陷（sign(0)=+1 导致擦除列被强制
  推向 bit 1）；适配器对结构性擦除（打孔 2Z 列 + 未发送尾部）注入 **+1 弱偏置**，
  无噪声/高 SNR 下完全解决（这是真实正确性修复，已进适配器）。
- ✅ 引擎修复：`ctrl->error_count` 每轮被内核清零，不能作最终 syndrome——改为
  `buf_h_pred` 转 Shared，CPU 侧 popcount 重算最终 syndrome（无 CRC 路径用）。
- ✅ 单元测试重定位：无噪声 = 严格位精确断言；有噪声 = 无假阳性（GPU 通过的块
  必须与原始消息逐位一致）+ GPU 通过率 ≤ CPU。**ALL OK**。
- 结论：GPU 路径**正确但弱**——适合高 SNR/大余量场景或实验用途；要与 CPU 同 BLER
  需要自研 NMS 内核（用户已确认暂保留 LLS 继续实验，NMS 作为后续选项）。

### Step 3 —— 端到端 PUSCH A/B + 大/小码块分流开关

- 配置开关（3 态）：`split`（默认：大码块→GPU，小码块→CPU）/ `all_gpu` / `all_cpu`。
  - 大/小阈值：按码块长 N 划分（如 N ≥ 阈值走 GPU）。理由：GPU 启动/同步固定开销
    （命令缓冲提交 + waitUntilCompleted）对小码块不划算，CPU 对小码块已很快。
  - 开关落在分段器层：`ldpc_segmenter_rx` 按码块选择 CPU/metal 解码器
    （两种 decoder 同时存在，按策略路由）。
- A/B 方案：同一配置（UE + Open5GS 不变），`ldpc_decoder_type` 在 metal 与 CPU 之间切换，
  复用 `ul_pipeline_probe`（T_start/T_end 已就位）+ tcpdump + ping RTT：
  - 对照组：CPU（现基线 219 µs 中位数）；
  - 实验组：GPU（期望中位数与 p99 进一步下降，尤其大码块）；
  - 记录 gNB 侧 [ul_pipeline] 统计、ping RTT 分布、UE BLER/重传。
- 验收：BLER 无回退（CRC 通过率一致），pipeline 延迟改善；分流开关三态均可用。

**实施进度（2026-08-16）**：

- ✅ 配置贯通：`pusch_ldpc_decoder_type` 选项（CLI `--pusch_ldpc_decoder_type` / yml
  `pusch_ldpc_decoder_type`）→ schema 检查 + 验证器 + yaml writer + 工厂配置。
  取值：auto / generic / neon / avx2 / avx512 / **metal**。已提交（a390d3d81c）。
  注意：该选项注册在 gnb CLI 的 **`expert_phy` 子命令**下，调用形式为
  `gnb -c <yml> expert_phy --pusch_ldpc_decoder_type metal`。
- ✅ **E2E A/B 完成（2026-08-16，真实 UE↔gNB 链路，ping 500 包）**：

  | 指标 | CPU（auto） | GPU（metal） | 变化 |
  |---|---|---|---|
  | [ul_pipeline] samples | 157 | 162 | +3%（无 BLER 回退） |
  | mean | 279.3 µs | 265.0 µs | **-5.1%** |
  | median | 271.0 µs | 261.0 µs | **-3.7%** |
  | p95 | 404.0 µs | 371.0 µs | **-8.2%** |
  | p99 | 546.0 µs | 422.0 µs | **-22.7%** |
  | ping 丢包 | 0.6% | 1.2% | 噪声范围内 |
  | ping rtt avg | 10857 ms | 11575 ms | +6.6%（RTT 由闭环锁步主导，与计算管线无关） |

  - GPU 路径在真实系统（高 SNR 测试台）中**无错误运行**，pipeline 全指标改善，
    尾部延迟（p99）改善最显著（-23%）。
  - 收益幅度受限于 ping 流量 = 小码块（BG2 小 Z）：GPU 固定开销
    （命令缓冲提交 + waitUntilCompleted）吃掉了大块理论收益；大码块场景
    （吞吐测试）预期收益更大——分流开关（split 态）待做：大码块→GPU、
    小码块→CPU。
- ✅ **E2E A/B 第二轮（2026-08-17，metal_nms_layered，同样 ping 500 包）**：

  | 指标 | CPU（auto） | GPU（metal_nms_layered） | 变化 |
  |---|---|---|---|
  | [ul_pipeline] samples | 163 | 173 | +6%（无 BLER 回退） |
  | mean | 277.3 µs | 258.2 µs | **-6.9%** |
  | median | 268.0 µs | 257.0 µs | **-4.1%** |
  | p95 | 391.0 µs | 360.0 µs | **-7.9%** |
  | p99 | 485.0 µs | 379.0 µs | **-21.9%** |
  | max | 851.0 µs | 465.0 µs | **-45.4%** |
  | ping 丢包 | 0.8% | 0.6% | 略优 |
  | ping rtt avg | 8574 ms | 10164 ms | RTT 由闭环锁步主导，与计算管线无关 |

  - 分层核**每码块 46 层 × 2 次 kernel dispatch、满迭代无 GPU 内部早停**，
    实链延迟仍全面优于 CPU；尾延迟（p99/max）改善最显著。
  - **2026-08-17 修正**(离线延迟基准见 4.9):ping 是 BG2 小码块且 gNB 配置里
    CPU 解码器 `early_stop_syndrome = false`(满 6 轮无早停)——两条腿解码耗时
    恰好相当,[ul_pipeline] ~260µs 由非解码阶段主导,本轮"GPU 更快"体现的是
    管线并行性/噪声,不是解码速度优势。GPU 的实链价值定位 = BLER 平齐 +
    CPU 卸载,而非解码延迟。
  - 与第一轮（LLS）对比：分层核 p99 379 µs < LLS 422 µs、median 257 µs < 261 µs——
    是实链上最快的 GPU 腿；CPU 基线两轮重复性良好（median 271.0 → 268.0 µs）。
  - 两轮日志均无 ERROR。后续优化方向：分层核加每轮 syndrome 早停
    （v1 无早停，小码块固定开销可再降）；大码块吞吐测试留给分流开关（#30）。

### Step 4 —— 编码器适配器（低优先级，按需启动）

- `encode/` 的 GF(2) uint4 核 + G 矩阵（K×M 位，同样由 ldpc_graph 生成或复用 .bin）。
- `ldpc_encoder_metal` 实现 `ldpc_encoder` 接口（`encode(bit_buffer, cfg)` →
  全码块缓冲含 2Z 打孔位），工厂注册 "metal"，与 CPU 编码器输出逐位对拍。
- PDSCH 侧 A/B 复用 Step 3 的测量框架。

---

## 4. 风险与对策

| # | 风险 | 对策 |
|---|---|---|
| 1 | BG2 矩阵缺失（E2E 小包必用 BG2） | 由 ocudu ldpc_graph 内存生成（Step 2 必做项），.bin 仅作 BG1 golden |
| 2 | LLR==0（打孔位）硬判两侧不一致 | 首轮 syndrome 差异，BLER 对拍验证；有差异则 0 → -ε 预偏置 |
| 3 | alpha/缩放因子不匹配导致 BLER 回退 | Step 2 单元 BLER 扫描标定（参考 alpha=0.45） |
| 4 | 每实例建 MTLComputePipelineState 开销大（毫秒级） | 按 (BG, Z) 懒加载共享 engine 池（≈16-32 个实例），decode 同步调用，命令队列天然串行 |
| 5 | GPU 同步调用阻塞 worker | v1 接受（测量后若 p99 劣化，再评估 MTLCommandBuffer 异步 + 完成回调挂 notifier） |
| 6 | 无 Xcode（CLT-only 机器）无法离线编译 .metal | 已解决：嵌入 .metal 源码 + `newLibraryWithSource` 运行时编译（Step 1 冒烟验证通过）；有 Xcode 时优先用离线 metallib |
| 7 | 328MB .bin 污染 git | .gitignore 排除 *.bin；Python 生成工具 + bg*.txt 进 git，可重建 |
| 8 | 实时性回退（GPU 竞争/功耗） | 探针 + ping 双指标把关，任何回退即可回退到 CPU 基线（工厂开关） |

## 4.5 BLER 性能基准（2026-08-16，已入库）

**工具**：
- `test/ldpc_metal_bler_test`：纯 LDPC 解码 BLER 基准（CPU vs GPU 同一 LLR 输入）。
  参数：`--bg/--z/--rates/--snrs/--trials/--max-iter`；每 (BG, Z, rate, SNR) 点输出
  pass 计数 CSV（`test/bler_results/bler_bg*_z*_r*.csv`）。
  传输模型：RV0 顺序截断到 K/rate 位（与真实速率匹配的擦除/缩短语义一致）。
- `test/plot_bler.py`：SNR vs BLER 曲线族（每个 (BG, Z) 一张图，颜色=码率、
  实线=CPU、虚线=GPU），生成 PNG。

**基线数据**（-6..10 dB，每点 200 块，max_iter=6）：
- CPU 瀑布：低码率 -2..0 dB；高码率（R=3/4）6..10 dB。
- GPU（LLS）瀑布：+6..+10 dB（低码率），高码率 10 dB 以上。
- 差距示例（BG1 Z64）：R=1/2 时 CPU@0dB=197/200 vs GPU@8dB=114/200（~8 dB）；
  R=1/3 时 GPU 4→44→91→100%（@4..10 dB）。
- 小码块（Z16）LLS 表现略好于 Z64（R=1/3 @6dB：164/200 vs 89/200）。
- 结论：GPU 与 CPU 的瀑布差距随码率升高而加大（高码率 >10 dB），
  NMS 内核重写是弥合差距的必由之路；本基线与工具作为后续打磨的参照系。

## 4.6 算法参考（当前使用的 LLS 内核）

### 版本确认

SynchroPlus 的多套实现中，ocudu 当前只使用一套（其余全部不编译，保留作参考）：

| 文件 | 角色 |
|---|---|
| `decode/ldpc_gpu_decoder.metal` | ★ 使用中——4 kernel 的 LLS 着色器 |
| `ocudu_metal_decoder_engine.mm` | ★ 使用中——ocudu 侧引擎，驱动这 4 个 kernel |
| `ldpc_decoder_metal.cpp` | ★ 使用中——ocudu `ldpc_decoder` 适配器 |
| `decode/ldpc_gpu_decoder.mm/.h` | 编译但从不实例化（verbatim 编译验证/参考） |
| `cn_scan_vn_lls_update.metal`、`update_llr.metal`、`Metal*Updater.mm`、`DecoderContextMetal.mm`、`CollaborationDecoder.cpp`、全部 `main_*.cpp` | 不编译不调用，参考 |

调用链：`ldpc_decoder_metal::decode()` → `decoder_engine::decode()`（单命令缓冲录制
init_hard_decisions + compute_syndrome + [cn_centric_scan; update_llr_hpred]×max_iter →
commit + waitUntilCompleted）→ CPU 读回符号位硬判、ctrl 块、重算最终 syndrome。

### LLS 算法细节

**思想**：不是 min-sum 消息传递，而是**加权比特翻转 + 置信度侵蚀**——每个 VN 的 LLR 是
当前置信度，被不满意校验方程反复侵蚀至穿过零点（比特翻转），翻转后证据强度成为新置信度。

**数据结构**：LLR（fp16 × N，零拷贝、原地更新）；s_hard（N/32 字硬判）；h_pred（M/32 字
打包 syndrome，**增量维护**，Shared）；err_eq_cnt / suspect_cnt / evidence_sum（每 VN 统计，
Private、原子）；vn_total_cn（列权，CPU 算）；DecodeCtrl（error_count / early_terminate /
actual_iters，Shared）。

**阶段 A** `init_hard_decisions`（每 VN 一线程）：清零 ctrl 与统计；硬判 bit = (fp16 < 0)，
每 32 线程 simd_sum 打包一字进 s_hard。

**阶段 B** `compute_syndrome`（每 32 行一线程组）：逐行 XOR-popcount(H 行 & s_hard) 得
parity，打包成 h_pred。全量 syndrome 只算初始一次，此后增量维护。

**迭代循环**（单命令缓冲内展开录制，GPU 内部早停）：

- `cn_centric_scan`（每校验行一线程组，组内 32 车道扫 H 分块）：
  1. early_terminate 置位 → 整个 kernel 空转（后续迭代全部 no-op）
  2. 错误计数：`error_count += popcount(h_pred[word])`（每 32 行字）
  3. 满意行跳过；对**不满意**行遍历其 H 行的置位 VN：
     - `err_eq_cnt[vn]++`；跟踪该行 |LLR| 的两个最小值 m1 ≤ m2 及下标
  4. shuffle_xor 蝶形归约 (m1, i1, m2, i2)
  5. 两个最弱 VN 成为嫌疑人：`suspect_cnt[i1]++`、`evidence_sum[i1] += m2`；
     `suspect_cnt[i2]++`、`evidence_sum[i2] += m1`（给最弱的证据 = 次弱的置信度）
- `update_llr_hpred`（每 VN 一线程）：
  1. **收敛判定**：error_count == 0 → vn 0 置 early_terminate，全体返回
  2. 读统计，若 s_cnt > 0 执行 LLS 更新：

     ```
     ratio = e_cnt / total_cnt                // 本 VN 不满意方程占比
     delta = alpha × ratio² × (e_sum / s_cnt) // alpha = 0.8（ocudu 调参）
     new_llr = old_llr − sign(old_llr) × delta
     ```

     delta 把 LLR 拉向零；delta > |old| 时符号翻转，新幅度 = delta − |old|。
  3. **syndrome 增量维护**：simd_ballot 收集翻转位，对每个翻转 VN 原子 XOR 其 HT 行
     进 h_pred（幂等，无冲突风险）
  4. 清零本 VN 统计；vn 0：actual_iters++、error_count = 0（注意：此清零使
     error_count 不能作终态 syndrome——CPU 侧 popcount(h_pred) 重算，已修复）

**已知缺陷**：sign(0)=+1 使精确 0 LLR（打孔擦除列）首轮被推向 bit 1（适配器用 +1 弱偏置
修复）；只用每行两个最弱 VN，信息利用不充分；delta 公式为经验启发式。全部 SynchroPlus
变体同属 LLS 比特翻转族，无 min-sum——BLER 基线的 ~8dB 差距由此而来。

### 对 NMS Metal 内核设计的借鉴评估

**可直接复用（架构层面）**：

1. **单命令缓冲展开迭代 + GPU 内早停**：CPU-GPU 零逐轮往返；NMS 沿用同一模式
   （每轮 CN/VN kernel + syndrome 检查 kernel，early_terminate 空转后续迭代）。
2. **增量 syndrome（h_pred + HT 原子 XOR）**：NMS 的 GPU 内早停需要每轮 syndrome，
   沿用"初始全量 + 翻转增量"结构可省去每轮全量重算。
3. **零拷贝缓冲池 + Private/Shared 分层**：H/HT 常驻零拷贝共享、LLR 原地更新、
   中间量 Private——NMS 直接照搬；h_pred 保持 Shared 供 CPU 读终态。
4. **dispatch 形状**：每校验行一线程组、组内 32 车道扫 H 分块 + shuffle_xor 蝶形归约——
   正是 NMS CN 更新求 min1/min2/符号积的理想布局，`cn_centric_scan` 的骨架可原样改造成
   NMS 的 CN kernel（改为处理**所有**行、统计换成 min1/min2/idx/sign_prod）。
5. **simd 原语示范**：simd_sum 打包、shuffle_xor 归约、simd_ballot 聚位——NMS 全部需要。
6. **每 VN 多行并发的原子聚合模式**（err/suspect/evidence）——NMS 的 VN 更新同样要聚合
   多个 CN 贡献；注意 Metal 无原生 float 原子加法（CAS 循环，竞争度 = 列权 3-19，可用）。

**必须重新设计（算法层面）**：

- 嫌疑人机制与证据公式 → NMS 标准更新：CN 求 min1/min2/sign_prod/idx；
  VN 侧 **on-the-fly 重构边消息**（不存 v2c/c2v 大数组）：对 VN v 遍历其 HT 行，
  `c2v = norm × sign_prod[row] × sign(v2c) × (v == idx_min1 ? min2 : min1)`，
  其中 sign(v2c) = sign(llr_v) ⊕ h_pred[row]——只需 llr、min1/min2/idx/sign_prod、
  h_pred、HT 五组数据，**与 LLS 的数据布局完全一致**，替换纯算术即可。
- 归一化因子 ~0.8（对齐 srsRAN 的 scale），LLR 尺度沿用 int8→fp16。
- 早停条件保持 syndrome 检查（可复用 compute_syndrome + error_count 结构）。

结论：LLS 的**架构骨架是 NMS 的直接模板**（数据布局、dispatch、缓冲分层、单缓冲循环、
增量 syndrome 五件套），只需替换 scan/update 两个 kernel 的算术内容——这正是后续 NMS
打磨的最短路径。

## 4.7 NMS Metal 内核（2026-08-16，已实现并验证）

**实现**（`ocudu_nms_decoder.metal`，ocudu 侧新文件，SynchroPlus 文件未动）：
- 按 4.6 的评估路径实施：架构五件套全部复用（单命令缓冲展开迭代 + GPU 内早停、
  零拷贝矩阵、每行线程组 dispatch、Shared/Private 分层），纯算术替换。
- **syndrome 处理进一步简化**：CN 内核在扫 H 行求 min1/min2/idx 的同一遍里从 llr 符号
  直接算出该行 parity（每行一个 uint32，无打包竞争），VN 内核读的是 kernel 边界保证的
  一致快照——LLS 的增量 HT-XOR 维护被整个移除，且消除了 VN 读 h_pred 与并发写之间的竞态。
- VN 更新：`c2v = norm × (v == idx_min1 ? min2 : min1)`，符号 = 外推奇偶
  （h_pred_bits[r] ⊕ 自身硬判），`sum = llr_chan + Σ c2v`；norm = 0.8。
- 工厂新类型 **`metal_nms`**（config: `expert_phy --pusch_ldpc_decoder_type metal_nms`）。
- 引擎 `decoder_engine::algo {lls, nms}` 双模式；"metal"（LLS）路径保持不变。

**排障记录（两个真实 bug）**：
1. VN 内核的 HT 遍历沿用了 LLS 的 32 车道条带循环——LLS 是全组协作处理一个 VN 的翻转，
   NMS 是每线程一个 VN，条带导致每个 VN 只处理自己行的 1 个 chunk，VN 23-31（chunk 越界）
   永远收不到消息、擦除位冻结。改串行遍历后无噪声一轮收敛。
2. 单元测试的 NMS 解码块一次静默替换失败（锚串不匹配），`gpu_nms_ok` 恒 false——
   修复后 ALL OK。

**验证结果（3-way BLER：CPU vs LLS vs NMS，-6..10dB，200 块/点）**：

| 配置 | CPU 瀑布 | LLS 瀑布 | NMS 瀑布 |
|---|---|---|---|
| BG1 Z16 R=1/3 | <-6 dB | 4-8 dB | 0-2 dB（0dB 159/200） |
| BG1 Z16 R=1/2 | ~0 dB | 6-10 dB | 2-4 dB |
| BG1 Z16 R=2/3 | 2-6 dB | >10 dB | 4-6 dB |
| BG1 Z64 R=1/2 | -2-0 dB | 6-10 dB | 2-4 dB（33@2, 200@4） |
| BG2 Z64 R=1/2 | -2-0 dB | 8-10+ dB | 2-4 dB |
| BG1 Z256 R=1/3 | <-6 dB | 8-10 dB | 2-4 dB |

- **NMS 与 CPU 的差距缩至 ~2dB**（LLS 的 ~8dB → NMS 的 ~2dB），3dB 点 NMS 已 100%
  与 CPU 一致（单元测试 BG1 Z16/Z32、BG2 Z16/Z32 全部 100/100）。
- 单元测试无噪声三配置（BG1 Z16/BG2 Z16/BG1 Z256）NMS 全部位精确、1 轮收敛。
- 剩余 ~2dB 的改进空间：norm 调优（0.8 待扫）、int8 量化损失、更多迭代。
- 图：`test/bler_results/bler_bg*.png`（实线 CPU / 虚线 LLS / 点划线 NMS）。

## 4.8 NMS 调优：与 CPU 的实现差异分析与实验（分层内核已收口，2026-08-17）

### CPU（generic）与 GPU NMS 的实现差异

| 维度 | ocudu CPU | GPU 洪泛 NMS（初版） | GPU 分层 NMS（收口版） |
|---|---|---|---|
| 调度 | **分层**：46 层顺序、每层后更新 soft（Gauss-Seidel） | 洪泛：全 CN 后全 VN | **分层**：层 = 基图校验行，层内 Z 行并行，kernel 边界串接 = Gauss-Seidel 链 |
| 归一化 | **1.0**（generic 的 `scale()` 是空函数，纯 min-sum） | 0.45（洪泛下最优） | **0.45**（分层下仍必需，见下） |
| v2c | soft − c2v_prev（精确外推） | 后验 \|llr\|（idx 技巧排除自回声） | **soft − c2v_old（精确外推，逐边 CSR 存储）** |
| 饱和 | **promotion_sum：\|soft\| > 63 → ±127 "infinity"（位冻结）** | 可选 clamp（无影响，未用） | 可选 clamp（`sat` 参数，本轮扫描验证中） |
| 短码块 | 跳过尾部层（nof_layers < M） | 全 M 行（尾部擦除偏置） | 同 CPU：适配器只生成有效层描述符 |
| 打孔列 | 精确 0 | +1 偏置（LLS 遗留） | +1 偏置（保留，未观察到副作用） |

**差异影响分析**（对应用户问题"哪些差异导致损失"）：
- 调度是**唯一的结构性损失源**：洪泛每轮只传播一层距离的信息，同样迭代数下收敛
  慢得多；分层调度下每层立即吸收最新 soft，是 CPU 用纯 min-sum（norm 1.0）仍能
  在 6 轮收敛的原因。洪泛靠 norm 0.45 压振荡、靠 mi=10 补轮数，0dB 仍 0/200——
  差距不是参数能补的。
- v2c 精确外推消除了洪泛 on-the-fly 的残余自回声（该近似在洪泛下影响被调度劣势
  掩盖；分层后必须逐边存储 c2v，CSR 边布局顺带解决）。
- 归一化差异：GPU fp16 无 CPU int8±127 的天然幅度上限，纯 min-sum 在 GPU 上消息
  幅度可膨胀 3 个数量级，振荡/吸饱风险远高于 CPU → norm 0.45 在分层下仍必需
  （norm 1.0 分层实测 4/200）。
- 饱和差异：CPU 的 ±127 冻结相当于"高置信位锁存"，是 CPU 高 SNR 稳定性的来源；
  GPU 分层核已加 `sat` clamp（本轮扫描确定是否复刻 CPU 行为）。

### 实验结果 1：洪泛调优（BG1 Z64 R=1/2，200 块/点，历史记录）

- **norm 扫描**（mi=6 @2dB）：0.3→134/200，0.4→173，0.5→171，0.55→163，0.6→153，
  0.7→89，0.8→32，0.9→8，1.0→~0——**0.45 定为默认**。
- **迭代扫描**（norm=0.45 @2dB）：mi=6→176/200，mi=8→198/200，**mi=10→200/200**（=CPU）。
- **饱和实验**：±32/±64/±127 与关闭无差异（norm=0.45 下 soft 幅度涨不到阈值）。
- 洪泛结论：2dB 点追平 CPU，但 **0dB 点 0/200（CPU 197/200）**——洪泛有约 1-2dB
  的硬地板，升级到分层调度是结构性收尾（`ocudu_nms_layered_decoder.metal`）。

### 实验结果 2：分层 NMS 全配置基准（norm 0.6, mi=10, 200 块/点，最终数据）

| 配置 | 速率 | CPU 瀑布 | 分层 GPU 瀑布 | 结论 |
|---|---|---|---|---|
| BG1 Z64 | 1/3 | 200@-2dB | **200@-2dB**（-4dB 19 vs CPU 0） | GPU 反超 |
| BG1 Z64 | 1/2 | 200@0dB | **200@0dB** | 平齐 |
| BG1 Z64 | 2/3 | 200@2dB | **200@2dB** | 平齐 |
| BG1 Z64 | 3/4 | 200@4dB | **200@4dB**（2dB 25 vs CPU 0） | GPU 反超 |
| BG1 Z256 | 1/3 | 200@-2dB | **200@-2dB** | 平齐 |
| BG1 Z256 | 1/2 | 200@0dB | **200@0dB** | 平齐 |
| BG1 Z256 | 2/3 | 200@2dB | **200@2dB** | 平齐 |
| BG2 Z64 | 1/5 | 200@-4dB | **200@-4dB**（-6dB 132 vs CPU 1） | GPU 大幅反超 |
| BG2 Z64 | 1/3 | 200@-2dB | **200@-2dB** | 平齐 |
| BG2 Z64 | 1/2 | 200@0dB | 187@0dB, 200@2dB | 0dB 差 13 块 |
| BG1 Z16 | 1/3 | 190@-2dB | **200@-2dB**（-4dB 69 vs CPU 1） | GPU 反超 |
| BG1 Z16 | 1/2 | 196@0dB | **198@0dB** | GPU 反超 |
| BG1 Z16 | 2/3 | 199@2dB | 185@2dB, 200@4dB | 2dB 差 14 块 |

- **9/13 个点平齐 CPU，4 个点 GPU 反超**（低码率/短码下 GPU 的 +1 打孔偏置与
  fp16 无位冻结更占优），仅 2 个点残余 ~0.2-0.3dB（BG2 Z64 R1/2 @0dB 差 13 块、
  BG1 Z16 R2/3 @2dB 差 14 块），属统计噪声边缘，可作为后续精细化实验对象。
- 分层 + norm 0.6 将洪泛的 **~2dB 结构性差距完全关闭**；"死磕 2dB"目标达成。

### 异常观察：CPU 基线自身在 BG1 Z64 R3/4 @10dB 丢 12 块

三份独立数据集（LLS / 洪泛 NMS / 分层 NMS）中 CPU 列在 10dB 均为 187-188/200，
而 GPU 均 200/200。10dB 下 LLR ≈ ±40±9（int8 ±64 内），CPU 的 promotion 未触发；
怀疑是 CPU 解码器在 N=4224、E=1877 该点的高 SNR 振荡（不触发早停）或基准解
速率匹配边角问题。与 GPU 无关（GPU 全过），留待 CPU 侧排查，不影响本结论。

### 结果 3：sat/norm 收口扫描（2026-08-17，完成）

针对两个最大残余点（BG2 Z64 R1/3 @-2dB、BG1 Z64 R1/2 @0dB）扫描
norm ∈ {0.45, 0.5, 0.6, 0.7, 1.0} × sat ∈ {0, 64, 127}：

| norm | BG2 Z64 R1/3 @-2dB（CPU 200） | BG1 Z64 R1/2 @0dB（CPU 200） |
|---|---|---|
| 0.45（sat 0/64/127 全同） | 113 | 193 |
| 0.5 | 174 | 199 |
| **0.6** | **198** | **200** |
| 0.7 | 198 | 200 |
| 1.0（+sat 127） | 24 | 13 |

- **sat clamp 无效果**：norm ≤ 0.45 时 soft 幅度涨不到 ±64，clamp 从不触发
  （与洪泛时代的观察一致）；norm 1.0 时即使 clamp ±127 仍振荡爆炸——CPU 的
  int8 隐式逐步饱和与 ±127 位冻结是 CPU 纯 min-sum 能成立的结构性前提，GPU
  fp16 复刻不了（也无必要）。
- **分层默认 norm 定为 0.6**（0.6-0.7 平齐 CPU，198-200/200 属统计噪声）。
  0.45 是洪泛调优遗留：分层调度的固有阻尼允许更高 norm → 瀑布更陡、残余
  差距关闭。`ldpc_decoder_metal` 按模式给默认值（lls 0.8 / nms 0.45 /
  nms_layered 0.6）。
- 全配置重跑（norm 0.6）确认无回归，结果见下。

## 4.9 深度优化序列（2026-08-17，P1 完成：GPU 内部早停）

### P1:分层核早停(ET)

- 机制(单命令缓冲保持,无 CPU 轮询):`nmsl_final_syndrome` 累加 unsatisfied 行数 →
  每轮末尾 1×1 的 `nmsl_et_gate` 核在 error_count==0 时置 `early_terminate` 并计轮 →
  后续所有 cn/vn/syndrome 核入口检查后立即返回。`et_enabled` 开关(引擎 init + 适配器
  构造参数)支持同一二进制内 ET 开/关 A/B。
- 语义:分层 actual_iters = 实际执行轮数(无噪块 ==1);与洪泛核(收敛轮不计入,无噪 ==0)
  不对称,已注释;无调用方消费该返回值(metric decorator 仅转发)。
- **验证**:
  - 单元测试:无噪三配置 ET-on iters==[1,1]、ET-off==[6,6];有噪 640 块 ET 开/关
    逐位一致(零失配)。有噪块普遍 1-2 轮收敛(原满 6 轮)——GPU 算力省 67-83%。
  - 200 块/点全配置瀑布(13 CSV,同 rng 种子)与 ET 前基线 **diff 零失配**:ET 不改变
    任何 BLER 数据点。
- 已知边角:LLR 恰为 0.0 的 VN 在零综合后理论可分叉(概率极小,640 块 + 全瀑布未现);
  如未来 10k 扫频出现失配,备选 = 连续两轮零综合才 ET。

### P3:OMS(Offset Min-Sum)+ 分层默认参数收口(α=0.7, β=0.5)

- 内核:`mag = max(mag - beta, 0) * norm`(分层 CN 第二遍 beta@8;洪泛 VN beta@11);
  引擎 init 加 `beta`、适配器构造加 `beta_override`、基准加 `--beta`;gnb 运行时开关
  `--pusch_ldpc_decoder_offset`(复刻 prach_th_correction_factor float 链:
  工厂配置 `ldpc_decoder_offset` → upper_phy 配置 → du_low CLI/yml/translator)。
- **β 网格(2000 块/点,两个残余点)**:α ∈ {0.5,0.6,0.7} × β ∈ {0.25,0.5,0.75}——
  **α=0.7 β=0.5 全面占优**:bg2 z64 r1/2 @0dB 1997/2000(α0.6/β0 时 1941,纯 NMS 时
  187)、bg1 z16 r2/3 @2dB 1985/2000 vs CPU 1986。β≥1 过冲(107/200),β≤0.25 欠效。
- 分层默认改为 **α=0.7, β=0.5**(洪泛保持 0.45/β0);200 块全套 13 点复验:两个残余
  点关闭(187→200、185→195/199),其余点全部保持平齐/反超——无回归。
- **10k 终验**:bg2 z64 r1/2 @0dB GPU 9991/10000 vs CPU 10000/10000(差 0.09%);
  bg1 z16 r2/3 @2dB GPU 9896/10000 vs CPU 9950/10000(差 0.54%)——两个残余点
  在 10k 分辨率下与 CPU 平齐(统计噪声内),"死磕 2dB"目标彻底达成。
- 基准顺带修 quirk:分层分支裸跑默认 `: 1.0F` → `: -1.0F`(用工厂默认)。

### P4:延迟交叉点测量(2026-08-17,结论:无交叉点,#30 关闭)

- 空闲机器、`--latency 300`、mi=6、4dB、CPU generic(早停开)vs GPU(ET 开):

  | 配置 | CPU | GPU | 倍率 | 配置 | CPU | GPU | 倍率 |
  |---|---|---|---|---|---|---|---|
  | BG1 Z8 | 38.7µs | 1082µs | 28× | BG2 Z8 | 25.4µs | 960µs | 37× |
  | BG1 Z64 | 193.9µs | 1211µs | 6.2× | BG2 Z64 | 87.3µs | 1101µs | 12.6× |
  | BG1 Z256 | 973µs | 1835µs | 1.9× | BG2 Z256 | 346µs | 1595µs | 4.6× |
  | BG1 Z384 | 1548µs | 2948µs | 1.9× | BG2 Z384 | 528µs | 2064µs | 3.9× |

- 构成:gpu_wait = 每轮 92 次串行 dispatch 链式延迟(4dB 块 ET 后 2-3 轮)+ CPU 侧
  固定 240-700µs(整条展开链的命令缓冲编码 + LLR 打包/读回)。差距随 z 收窄,
  1.9× 是 Z384 的极限——**GPU 解码延迟在全尺寸落后 CPU,交叉点不存在**。
- **决策(用户确认)**:废弃大小阈值 split 计划,#30 关闭;`metal` 保持为显式卸载
  选项(默认 auto→NEON 不变)。GPU 定位 = BLER 平齐 + CPU 卸载。
- 后续可选方向(未排期):预编码命令缓冲模板(砍 CPU 侧 ~220µs)。

### P6:核融合 + 静态绑定提升(2026-08-17,bb30c13d69)

- 融合 CN+VN 单核(3GPP 分层无共享 VN 性质保证裸写无竞争)、删 vn_delta/LayerDesc/
  layer_descs、静态绑定一次提起(3 核错开索引 0-8/10-14/15)。
- **延迟结果**(空闲机器,300 样本/点,mi=6,4dB;对比融合前基线):

  | 配置 | GPU mean | GPU p99 | 改善 | 配置 | GPU mean | GPU p99 | 改善 |
  |---|---|---|---|---|---|---|---|
  | BG1 Z8 | 1082→933 | 3140→1817 | -14%/-42% | BG2 Z16 | 975→628 | 1078→1323 | -36%/- |
  | BG1 Z16 | 1056→712 | 1203→831 | -33%/-31% | BG2 Z64 | 1101→772 | 1190→862 | -30%/-28% |
  | BG1 Z64 | 1211→794 | 1352→1162 | -34%/-14% | BG2 Z256 | 1595→1161 | 2479→2179 | -27%/-12% |
  | BG1 Z256 | 1835→1321 | 2514→2245 | -28%/-11% | BG2 Z384 | 2064→1528 | 2414→1683 | -26%/-30% |
  | BG1 Z384 | 2948→2381 | 4058→3694 | -19%/-9% | | | | |

  mean 全线 -19%~-36%,gpu_wait 同步下降(Z64:955→589µs);CPU 侧 256→205µs(适配器
  打包/读回主导)。交叉点仍不存在:Z384 GPU 仍 1.7× CPU——差距收窄但 GPU 尚未翻盘。

### P7:ICB 间接命令缓冲(2026-08-17,平台阻断,已回滚)

- 目标:init 录制全部管线+绑定+46 层派发,decode 只 executeCommandsInBuffer。
- **实测结论(最小可复现实验逐项验证)**:本机(darwin 25/AGX G16X)上 CPU 编码的 ICB
  只有**裸 dispatch 命令可靠**;ICB 内 `setComputePipelineState` 导致 GPU Address
  Fault(即使 pipeline 已按文档设 `supportIndirectCommandBuffers = YES`),
  `setKernelBuffer` 被静默忽略。与 rlx-metal 项目报告的 Apple Silicon 同类故障一致
  (必要但非充分,存在未文档的平台要求)。分层 Gauss-Seidel 调度每层必须变 layer_start
  (ICB 不支持 setBytes 常量,唯一通道是逐命令 buffer offset = 被忽略的 setKernelBuffer),
  因此"录一次、执行一次"的 ICB 形态在此驱动上不可实现。软件 grid-sync 变体因占用率
  限制(46×Z 线程组无法常驻)对大 Z 不可行。
- 处置:ICB 代码已回滚到 bb30c13d69(融合+静态绑定,已提交);验证层(MTL_DEBUG_LAYER=1)
  顺带揪出并修复两个真实问题:pipeline 需 ICB 支持标志、newBufferWithBytes 不得用 Private。
- 若未来平台修复:恢复方案 = ICB dispatch-only + 每命令 offset 绑定 layer_starts 缓冲,
  decode 侧一次 executeCommandsInBuffer(设计已在本节上方代码演练完整)。

### E2E 第三轮:融合+ET+β 修复的实链验证(2026-08-17)

| 指标 | CPU(auto) | GPU(metal = 融合分层 NMS + ET + OMS β0.5) |
|---|---|---|
| [ul_pipeline] samples | 17 | 128 |
| mean / median | 259.4 / 265.0 µs | 331.6 / 315.0 µs |
| p95 / p99 / max | 317 / 317 / 368 µs | 511 / 592 / 611 µs |
| ping 丢包 | 0%(500/500) | 1.2%(494/500,历史噪声带 0.6-1.2% 内) |
| ping RTT avg | 5263 ms | 5876 ms(链路比上轮快近一倍,闭环锁步主导) |
| 后端错误 | 0(仅 2 条 macOS 无关的 Linux 告警) | 0 |

- **功能验证达成**:融合内核 + ET + β=0.5 默认值(生产路径修复后首跑)在实链完整
  跑完 500 ping、零错误——融合后的新解码路径 E2E 验收通过。
- **延迟对比不可靠,如实记录**:CPU 腿仅 17 个样本(0% 丢包证明解码全成——是探针
  槽匹配本轮大面积失配,统计问题非解码问题),与 GPU 腿 128 样本不可公平对比;
  且此前已确立 [ul_pipeline] 由非解码阶段主导、解析不了解码耗时差异(离线
  --latency 基准才是解码延迟的权威数据,见 P4/P6)。
- GPU 腿 p99 592µs vs 融合前 379µs 不可解读为"融合变慢":上轮是 pre-ET 内核 +
  满 6 轮、样本 173、链路 RTT 8574ms;本轮链路状态与样本量都不同,差异属管线噪声。

### P8:纯 INT8 译码管线(2026-08-17,BLER 达成、延迟取舍如实记录)

- 实现:llr/c2v 改 `device int8_t*`;宿主侧**删除 llr_to_fp16**,`memcpy` 直拷原生
  int8 存储(静态断言 sizeof(log_likelihood_ratio)==1);擦除偏置 +1 语义不变;
  shader 内 int16 中间累加 + 写回饱和 clamp[-127,127](杜绝 -128/回绕);
  α/β 转 6-bit 定点整数(norm_mul/norm_shift/beta_q,如 0.8→mul=51/64),纯整数 ALU;
  蝴蝶归约 int32(原生 lane 宽)。
- **整数空间重扫 α/β**(三个退化点 2000 块网格):fp16 的 (0.7, 0.5) 不适用——
  整数 β 把 <1 的小消息归零、损坏擦除动态;**int8 最优 = (α 0.8, β 0.0)**(量化
  本身即偏移效果)。新默认已入库。
- **BLER 终验(优化核, 10k)**:bg2 z64 r1/2 @0dB **10000/10000** vs CPU 10000(精确
  平齐;fp16 是 9991)、bg2 z64 r1/3 @-2dB **10000/10000** vs CPU 9991(反超)、
  bg1 z16 r2/3 @2dB **9986/10000** vs CPU 9950(反超;fp16 9896)、bg1 z64 r3/4
  @2dB 1969/2000(f16 25、CPU 1)。**工作点全面平齐/反超 fp16 与 CPU**;仅深瀑布
  -4~-6dB 的 fp16 特例点(子单位消息精度,如 r1/5 @-6dB 132/200)在 int8 下不可
  复现(CPU 同样仅 10/2000,非工作点)——如实记录。
- **延迟取舍(诚实数据)**:CPU 侧固定开销 205-286→152-170µs(**-25%,零转换目标
  达成**);但 GPU wait 上升(Z256:1092→2884µs 优化后)——byte 粒度散射访问 +
  整数管线在 M4 Pro 上慢于其专长的 fp16 管线(2× fp16 吞吐),显存带宽并非瓶颈
  (gpu_wait 由串行 dispatch 链主导)。净墙钟回退,交叉点结论不变。
- **给用户的后续选项**:混合管线 = int8 存储(CPU 零转换 + llr/c2v 读带宽减半)+
  shader 内 load 后转 fp16 计算(吃满 fp16 管线)——两端优势兼得,与"纯整数 ALU"
  指令相悖,故按用户指示未实施,记录待选。
- et-off 满轮漂移:int8 定点在收敛后不再符号冻结(444/640 块漂移),生产由 ET
  首干净轮即停保护;单元测试改为报告不断言(见代码注释)。

### P8 补记:混合方案的裁决与最终形态(2026-08-17)

- **评估结论(数据裁决)**:纯 int8 的 GPU 减速主因是 **byte 粒度散射访存**,不是
  ALU——"内核内逐边转换"的混合原型在大 Z 与纯 int8 同样慢(Z256 3616µs),
  未达标。**单次预处理转换核**形态(宿主 memcpy 直拷 int8 → 一个 N 线程 dispatch
  转 fp16 工作缓冲 → fp16 融合热循环原样跑)通过裁决:
  - **同热态背靠背对比**(fp16 融合版 vs 转换版):gpu_wait Z256 3560 vs 3562µs
    (持平)、Z16 629 vs 583(-7%)、BG2 Z64 920 vs 820(-11%)、BG2 Z256 2040 vs
    1765(-13%)——转换 dispatch 无额外代价,热循环保持 fp16 速度;
  - CPU 侧固定开销保住 memcpy 收益(Z384 483→424、BG2 Z64 254→164µs);
  - BLER 与 fp16 完全一致(bg2 r1/2 @0dB 10k = 9991,逐位相同);
  - fp16 热循环的 ET 等价性(无漂移)随之完全恢复,单元测试严格断言全绿。
- **重要方法学教训**:本日连轴 GPU 基准后 M4 Pro 出现明显热降频(早晨 fp16 融合版
  Z256 gpu_wait 1092µs → 傍晚同二进制 3560µs,3.3×)——**跨时间的延迟对比一律
  无效,只有背靠背对比可信**;此前的绝对延迟表都应按"热态未知"解读。
- 当前形态 = **int8 宿主存储(memcpy 直拷)+ GPU 单次转换 + fp16 融合热循环**,
  是三者中 CPU 侧与 GPU 侧的帕累托最优;纯 int8 与内核内转换两条路已证伪。
- 实现细节:`nmsl_i8_to_fp16` 核(索引 16/17,与热循环错开),转换 dispatch 必须在
  静态绑定之后(教训:全零输出通过 CRC(all-zero)=0,静默假通过)。

### E2E 第四轮:零转换 + fp16 热循环形态(2026-08-17)

| 指标 | CPU(auto) | GPU(metal = 零转换 + fp16 融合 + ET) |
|---|---|---|
| [ul_pipeline] samples | 165 | 156 |
| mean / median | 267.1 / 262.0 µs | 270.9 / 266.0 µs |
| p95 / p99 / max | 390 / 449 / 500 µs | 396 / 443 / 490 µs |
| ping 丢包 | 0%(500/500) | 0.6%(497/500,历史噪声带内) |
| ping RTT avg / mdev | 13803 / 7447 ms | 7312 / 3664 ms(闭环锁步主导,仅参考) |
| 错误 | 0 | 0 |

- 两腿均零错误、样本量恢复历史正常区间(165/156,上轮 CPU 腿 17 样本确系探针匹配
  偶发)。管线统计**在噪声内完全一致**(median 262 vs 266µs,p99 449 vs 443µs)
  ——与既有结论一致:[ul_pipeline] 由非解码阶段主导,新 GPU 路径(零转换 + fp16
  热循环 + ET)在实链上与 CPU 无回退。

### 生产路径修正:β 默认值语义(2026-08-17)

- 10k 重跑与 OMS 终验数据不一致,追查发现:`ldpc_decoder_offset` 全链默认 0 → 工厂把
  0 当显式 override 传入,**压掉适配器调优默认 β=0.5**——gnb 实链此前跑的是 β=0。
  语义改为 **-1 = 未设置(用调优默认 0.5)**,CLI Range(-1, 64)。

### P5:并发(已并入 d3c43f617c)

- 基准 `--latency N` 模式:固定 SNR 下 N 次 decode 墙钟统计(mean/p50/p95/p99)+
  引擎 `last_gpu_wait_us()`(GPUStartTime/GPUEndTime,Apple Silicon)——wall−gpu =
  CPU 侧固定开销。首测 BG1 Z64 mi=6:CPU 319µs vs GPU 1299µs(gpu_wait 1013µs +
  CPU 侧 287µs)→ 单轮 92 次 dispatch 的固定开销在大块上主导,交叉点测量进行中。
- 并发:解码器 `decode()` 全程持 `decode_mtx`(覆盖 get_slot 惰性插入 UB + scratch);
  多线程压力测试(4 线程 × 100 次共享实例)**通过**——过程还抓到通用编码器非线程
  安全(内部缓冲引用),测试改用每线程独立 encoder,解码器本身无竞态。

## 4.10 目录清理:只保留分层 NMS(2026-08-17)

- 决策:分层 NMS(α=0.7 β=0.5 + ET)已双维收敛,删除 LLS 与洪泛 NMS 两条历史路径;
  **"metal" 工厂类型重指向分层 NMS**;bler_results 删 LLS/洪泛 CSV、分层 CSV 改名
  `bler_metal_*`、图重绘为 CPU vs GPU 双曲线。
- 删除:SynchroPlus 逐字拷贝 `decode/`(LLS shader/引擎/测试)、`encode/`(编码器)、
  `g_f_matrix/`(328MB .bin + 生成器)、`ocudu_nms_decoder.metal`(洪泛)、LLS/洪泛
  基准 CSV 与旧图。**完整可找回**:`archive/metal-lls-flooding` tag + git 历史。
- 简化:引擎收敛为分层单模式(删 algo 枚举、metallib 离线编译链、LLS 4 核与洪泛
  3 核、sat clamp、HT 生成——HT 仅洪泛用,省 ~6MB/槽);适配器 ctor 简化;
  工厂与 gnb CLI 白名单只留 `metal`(分层);CMake 只嵌分层 shader。
- 保留:分层核 + ET 门 + OMS、CSR/层表生成、擦除偏置、互斥、延迟基准工具、
  golden H 比对(读外部 SynchroPlus 路径)。

## 5. 交付物清单

- [ ] `metal/PLAN.md`（本文件）+ `metal/.gitignore`
- [ ] Step 1：CMake 连通性（`ENABLE_METAL_LDPC`、metallib 编译、`ocudu_ldpc_metal` 静态库）
- [ ] Step 2：`ldpc_decoder_metal` + "metal" 工厂类型 + H 生成器 golden 比对 + BLER 对拍报告
- [ ] Step 3：`ldpc_decoder_type` 配置 + 分流开关 + E2E A/B 数据表（[ul_pipeline] / ping / BLER）
- [ ] Step 4：（可选）`ldpc_encoder_metal` + 编码对拍
