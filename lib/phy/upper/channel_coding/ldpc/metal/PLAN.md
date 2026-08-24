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

### P9:Group-Layered NMS(2026-08-17,结构证伪,已回滚)

- 目标:按 VN 正交性把 46/42 层贪心分组,组内 2D dispatch 并发,压缩 dispatch 数。
- 实现完成并验证正确(单测全绿),但**分组效果被 5G NR 基图结构封顶**:
  - **列共享统计(BG1,权威数据)**:列 0/1(两个打孔核心列)分别被 **26/25** 个
    扩展行共享;3GPP 特意把扩展部分的回连集中在列 0/1(高列重保证环长),
    任何共享该列的两行都不能同组——扩展行的正交分组上限 ≈ 2-3 行/组。
  - 实测:贪心(含度降序启发式)BG1 46 层 → **30 组**(大多 2 层/组),
    BG2 42 层 → **24 组**。dispatch 仅减 1.5-1.75×,而 +1~2 迭代补偿(+17-33%)
    后总 dispatch 预算 8×32=256 vs 6×48=288,**净收益 ≈ -11%**——远低于目标。
- 回滚到 a52aa3ad57(零转换 + fp16 融合热循环)。**负结果价值**:任何后续
  "并行层调度"方案必须先解决列 0/1 的星形冲突——唯一理论可行方向是放宽到
  "仅列 0/1 允许共享 + 打孔列延迟增量聚合(星形修复核)",代价是每轮 1-2 个
  额外 dispatch 与打孔列收敛滞后,收益/风险比需重新论证(记录备查)。

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

## 4.11 P10:洪泛 NMS 回溯与裁决(2026-08-18,对齐点不存在,保留为可选类型)

- 动机:Apple Silicon 逐层 dispatch 驱动开销无法压缩,恢复纯并行洪泛(2 dispatch/轮),
  用"迭代数换调度时间"验证能否用 GPU 并发替代 CPU 调度。实现:从 4.10 清理前的历史
  洪泛形态重建为 `metal_flooding` 工厂类型 + `ocudu_nms_flooding_decoder.metal`
  (CN 整行 butterfly 归约 min1/min2/idx/parity + 原子 error_count,VN 单线程串行重建
  c2v 并融合 ET 收敛检查与轮计数);移植全部后期优化:零转换 int8 进 + 一次性
  i8→fp16 双写(工作 LLR + 信道副本)、静态绑定提升(热循环 = 仅 setPipelineState
  + dispatch)、H^T 生成恢复、`--cpu-max-iter` 迭代解耦(CPU 固定 mi=6,GPU 扫高迭代)。

### BLER 对齐点扫描:不存在(mi∈{10,16,20,24,30} × 2000 块/点,CPU 固定 mi=6)

| 工作点 | CPU mi=6 | 洪泛 mi10 | mi16 | mi20 | mi24 | mi30 |
|---|---|---|---|---|---|---|
| BG2 Z64 R1/2 @0dB | 1986/2000 | 0 | 0 | 0 | 0 | 0 |
| BG1 Z64 R1/2 @0dB | 1950/2000 | 0 | 0 | 0 | 0 | 0 |
| BG1 Z16 R2/3 @2dB | 1914/2000 | 6 | 7 | 6 | 5 | 5 |

- 补测 BG2 Z64 R1/2 @0dB **mi=50:0/1000** ——迭代数从 10 到 50 零改善,0dB 处是
  错误地板而非迭代受限;对齐点在任何合理 mi 范围内不存在。
- 健康性交叉验证(排除移植回归):历史归档 `archive/metal-lls-flooding` 中
  `bler_metal_nms_bg1_z16_r0.6667.csv`(mi=10,200 块/点):0dB 0/200、2dB 0/200、
  4dB 112/200、6dB 198/200、8dB 200/200;当前实现同点 2dB 6/2000,且 BG2 Z64
  4dB/6dB 全过(200/200)——瀑布落在 2-4dB 之间,与历史完全一致,解码器健康,
  差距是洪泛调度固有的瀑布右移(~2dB+),不是 bug。

### Dispatch 与时间裁决

- dispatch/解码:洪泛 = 2 + 2×mi(62 @ mi=30);分层 = 2 + mi×(L+2)
  (BG2 L=42 → 266 @ mi=6,BG1 → 290)。调度优势真实存在,但无对齐点可兑现。
- 延迟(BG2 Z64 R1/2 @0dB,n=200,同进程背靠背):
  CPU mi=6 mean 222-403µs;**分层 GPU mi=6 mean 870µs**(gpu_wait 663µs);
  **洪泛 GPU mi=30 mean 1538µs**(gpu_wait 1335µs)——洪泛 30 轮全矩阵扫描的 GPU
  工作量远超分层 6 轮增量,Gauss-Seidel 信息复用优势无法用迭代数赎回。
- **裁决:三个维度全负。**(1) BLER 对齐点不存在(mi≤50 扫不到,瀑布差 ~2dB+);
  (2) dispatch 数优势(62 vs 266)无从兑现;(3) 时间 1538µs > 870µs(分层)> 222-403µs
  (CPU)。洪泛出局,生产路径维持 CPU/分层;代码作为 `metal_flooding` 可选类型保留
  (工厂默认不变,零生产风险),供高 SNR 定长延迟场景或未来"星形修复"分组方案参考。

### 工具链教训:`--snrs 0:0:0` 步长 0 = 无限循环

- 裁决测试期间 3 个基准进程被 memorystatus 击杀(压缩内存 ~381GB,峰值 RSS 31GiB),
  追查为 `parse_seq` 的 `for (v=a; v<=c; v+=b)` 在步长 b=0 时无限 push_back
  (采样栈定位 ldpc_metal_bler_test.cpp:76)。已加 b<=0 校验,单值 SNR 请用 `--snrs 0`。

### E2E 第五轮:metal_flooding 实链功能确认(2026-08-18)

- 三腿对比(CPU=auto→neon、metal=分层,8/17;metal_flooding,8/18,二进制为 P10
  工作区构建,版本戳 a0ddfc86d1):

| 腿 | 样本 | pipeline mean/median | p95/p99 | ping 丢包 | ping avg/mdev |
|---|---|---|---|---|---|
| CPU mi=6 | 165 | 267/262 µs | 390/449 µs | 0% | 13803/7447 ms |
| metal 分层 | 156 | 271/266 µs | 396/443 µs | 0.6% | 7312/3664 ms |
| metal_flooding | 194 | 221/207 µs | 306/341 µs | 0.4% | 14121/7458 ms |

- 管线统计三腿同噪声带([ul_pipeline] 由非解码阶段主导的既有结论不变;跨日对比
  受热漂移限制,flooding 腿样本最多 194)。零错误行——**flooding 在实链工作 SNR 下
  功能正确**:链路实际 SNR 高于洪泛瀑布(与基准一致:≥4dB 全过)。
- ping 侧最有趣的信号:**flooding ≈ CPU 剖面**(14121/7458 vs 13803/7447,pipe
  260/278 几乎重合),而分层腿 7312/3664。flooding 的 GPU 解码最慢(0dB 基准
  1538µs),RTT 却落在 CPU 剖面上——反向印证分层腿的 7.3s 是锁步相位伪影,
  RTT 与解码延迟无关(既有"闭环锁步主导,仅参考"结论)。
- **结论:功能确认,不改裁决**。flooding 在信道好时可用(本轮实链即证据),但在
  0-2dB 工作点(CPU mi=6 通过 96-99%)其通过率 0-0.3%——CPU/分层对同一流量持有
  2dB+ 余量。生产路径维持 CPU/分层;metal_flooding 保留为可选类型,现有实链
  功能记录。

### E2E 测量增强:纯 LDPC decoder 耗时探针(2026-08-18)

- 现有 `[ul_pipeline]` 起点是 IQ 采样收到(RX 链总耗时),解码差异被前置阶段淹没。
  新增第二序列 `[ul_ldpc_decode]`:**起点 = PUSCH 首码块 `decoder->decode()` 调用前
  (pusch_decoder_impl CB 任务内, cb_id==0),终点 = 原 CRC-OK 点不变**——同一终点
  事件同时结算两个序列,运行结束每种 case 输出两行统计。
- 匹配设计:起点为单槽 last-write-wins(单 UE 单 PUSCH 在途,CRC 失败 TB 留下的
  旧起点会被下一 TB 覆盖);终点处 2ms 新鲜度护栏丢弃残留起点(重传无需解码等
  边角),避免跨 TB 错配。OCUDU_FLOW_PROBES 门控与现有探针一致,关闭时零开销。
- 下一轮 E2E 三腿(auto/metal/metal_flooding)即可直接对比纯解码耗时。

### E2E 第六轮:纯解码耗时探针首测(2026-08-18,三解码器实链平齐)

| 腿 | 样本 | pipeline mean/median | pipeline p95/p99 | **ldpc_decode mean/median** | **ldpc_decode p95/p99** |
|---|---|---|---|---|---|
| auto(neon) | 207 | 227.3/214.0 µs | 331/351 µs | **36.1/28.0 µs** | **78/117 µs** |
| metal 分层 | 238 | 222.2/218.0 µs | 308/349 µs | **36.6/30.0 µs** | **71/131 µs** |
| metal_flooding | 256 | 221.9/213.0 µs | 303/370 µs | **36.5/28.0 µs** | **72/138 µs** |

- **首测结论:实链工作点上三种解码器统计平齐**(mean 36.1/36.6/36.5µs,median
  28.0/30.0/28.0µs)。实链 TB 小(单码块)、SNR 高、ET 1-2 轮即收敛——基准里的
  最坏情形(0dB 满迭代 Z64:CPU 222-403µs vs 分层 870µs vs 洪泛 1538µs)在实链上
  从不出现,GPU 的固定调度开销在 ~30µs 量级被 ET 摊薄到与 neon 同价。
- 解码占比:36/222 ≈ **16%**,前端(信道估计+解调+解复用)~190µs 仍是主导——与
  既有结论一致;同日下午三腿背靠背,pipeline 口径全部收敛在 222-227µs。
- **对 GPU 价值主张的正面修正**:P4 的"GPU 全 Z 无交叉点"是最坏情形结论;实链
  工作点 GPU(分层/洪泛)与 CPU **延迟平齐 + BLER 平齐**,GPU 的价值 = 纯 CPU
  卸载(空出的核给多小区),不再是"以延迟换卸载"。
- ping:auto/metal 两腿统计字节级相同(疑似粘贴了同一份输出,待用户确认);
  flooding 腿 14.0s 均值同锁步噪声带、丢包 0.2%。

### UE ping console 锯齿批量打印的机制分析(2026-08-18)

- 现象:500 个 ping 分 2 批打印,批内 RTT 从 ~26.9s 逐包线性递减(斜率
  103.77ms/包 = 精确等于发送间隔 51885/500),批间跳回 ~26.7s。
- 数学:**这不是真实 RTT,是排队等待**。若某批全部应答在同一时刻 T 送达
  ping socket,则 RTT_i = T − send_i,随 seq 线性递减、斜率为发送间隔——
  与观察完全吻合;统计数字正是锯齿分布的矩(avg 14046 ≈ (546+26979)/2,
  mdev 7436 ≈ (26979−546)/3.46 = 7630)。
- 机制:交付路径呈"关 26.7s → 开一瞬间"的二值状态——即已知锁步闭环的
  **突发交付**:ZMQ 拉模式下 DL 流(承载 PDCCH UL grant + 回声数据)在慢平衡
  态成簇流动(MAC 分配实测 4.7/s 均值 vs 80/s 活跃突发)。UE 侧 ping 请求排队
  等 UL grant(或回声在 gNB DL 侧排队),每次簇释放全部积压 → console 分批 →
  锯齿。26.7s 是闭环自由尺度的当前平衡值(每次运行不同,与前几轮 min
  546-1914ms、mdev 3.7-7.4s 的差异一致),非任何硬编码常量。
- 归属:**与之前定位的 ZMQ/锁步问题同族**(回复时延链 + rx 锁步慢平衡 +
  sequential 单线程自节流),非解码器相关(三种 case 趋势相同)、非新回归。
- 判定实验:UE 侧 tcpdump ZMQ TCP 端口(应答到达是否成簇)或 Mac 侧 tcpdump
  UDP 2152(GTP-u 回声到达是否成簇)——前者区分 UE 排队 vs gNB DL 排队,
  后者区分 UL grant 饥饿 vs DL 积压。根治方向仍为记忆库记录的
  "dual+ZMQ 浅缓冲 1-2 slot + tx 领先量硬约束"。
- 旁证:[ul_pipeline] 样本 207-256 ≈ ping 数的一半——成簇 UL 到达下约半数
  TB 落在探针 ±2 slot 匹配窗之外,与突发机制自洽。

### 判别实验:`ping -i 1` 结果(2026-08-18,簇周期随负载自由变化)

- 现象反转:**批量消失**——console 匀速逐行打印,RTT 落入两个分离的带:
  低带 1426-1892ms(中心 ~1.65s)、高带 2394-3550ms(中心 ~2.55s),带间差
  ~0.9-1.0s;前半段杂乱、后半段 (seq 36-50) 呈 H/L 交替并缓慢进动(偶发 LL)。
- 裁决:证实"**时间周期簇**"假说,排除"包数容量"假说——批量只在到达率
  (10/s,-i 0.1)超过均衡服务率时出现,积压按簇周期堆积成 257 包级巨批;
  降到 1/s(-i 1)后到达率 ≤ 服务率,积压消失、交付连续。
- 双带 = 簇相位结构:应答落在同一簇(低带)vs 下一簇(高带);后半段 H/L
  交替 = 簇周期 ~2s 与 1s 发送间隔的拍频。**簇周期随运行/负载自由变化**
  (-i 0.1 轮 26.7s,-i 1 轮 ~2s),与"闭环尺度自由、无恢复力"结论一致。
- -i 1 的 RTT 1.5-2.7s = 已知 macOS 慢平衡基线(历史 ~2.2s),非新问题;
  与解码器无关。根治方向不变:dual+ZMQ 浅缓冲 1-2 slot + tx 领先量硬约束。

### 怪异行为的处置结论(2026-08-18)

- **本项目内:可以忽略。** 该现象解码器无关(三 case 趋势相同已证)、
  [ul_pipeline]/[ul_ldpc_decode] 的测量值(每 TB 处理耗时)不受交付成簇污染
  (只影响采样覆盖 ~50%,不影响采样值有效性);ping 从来不是解码指标。
- **"真 RF 会消失"的结论依然成立,且现在有精确机制表述**:消失的是 ZMQ
  拉模式的软件锁步环(UE 拉取周期 ↔ gNB 生产率的闭环速率匹配 + 自由尺度)。
  真 RF 用空口采样时钟替换软件时钟,两端都锁在硬件时间轴上——环不存在,
  锯齿/巨批/双带这类排队伪影无处可寄。ping RTT 将回归空口 + MAC 调度粒度
  (UL grant 周期,毫秒-十毫秒级)。
- **诚实的边界**:真 RF 把 ZMQ 的"优雅退化(慢平衡)"换成硬实时约束——若
  macOS 移植存在每 slot 实时性缺陷,将以 RF overflow/underrun 硬错误暴露
  而非慢平衡。现有证据支持可扛:每 TB 管线 ~220µs(1ms 预算内)、总 CPU
  33.7% 大半空闲、io_broker µs 级唤醒。但真 RF 路径从未被实测,上线前
  需 UHD 冒烟验证每 slot 实时性。

## 4.12 P11:metal_persistent 持久内核(2026-08-18,单 dispatch 分层 NMS)

- 目标:消除 CPU 侧 it×l 双层循环的 dispatch 编码开销,把整轮解码折叠为**一次
  dispatch**。保留 metal/metal_flooding 不动作基线。
- **平台负结果(重要)**:先实现的多 threadgroup 软件网格栅栏方案(W=min(z,128)
  常驻 TG + 原子 ticket/epoch)被证伪——本 Apple MSL 目标**只暴露 relaxed 原子**
  (metal_atomic 模板显式禁用 acquire/release/seq_cst,无 atomic_fence,
  `memory_order` 枚举仅 relaxed),即平台不提供任何设备级内存序原语;threadgroup_barrier
  的 mem_device 组合实测不足以保证跨 TG 可见性(单 TG 100% 通过、多 TG 100% 失败
  的二分定位)。跨 TG 栅栏在此平台无法正确实现——与 Apple"Metal 不支持 grid barrier"
  的官方立场一致。
- **落地设计(正确性由构造保证)**:单 TG × 1024 线程(32 simdgroup)常驻,层间用
  `threadgroup_barrier(mem_threadgroup | mem_device)`(TG 内跨 simdgroup 可见性是
  保证语义);层内每 simdgroup 串行处理 ⌈z/32⌉ 行;prologue 折叠 ctrl 复位 + c2v
  清零 + i8→fp16 转换;syndrome + ET gate 均在核内,ET 触发后**直接 break**(dispatch
  版仍需提交剩余轮次的全部 dispatch)。decode() = 1 次 dispatchThreadgroups。
- 验证:
  - 单测 ALL OK:persist 全 case 100%(无噪位精确 + 3dB/5dB 有噪 + Z16-256)。
  - **BLER 与 metal 完全平齐**(1000 块/点,同种子逐块同解):bg2 z64 @0dB
    901/1000 = 901/1000;bg1 z16 r2/3 @2dB 971/1000 = 971/1000。
  - 延迟(背靠背,n=200):
    | 点 | metal(gpu_wait/CPU侧) | **persist(gpu_wait/CPU侧)** |
    |---|---|---|
    | 0dB Z64 满迭代 | 1708µs(1497/211) | **1933µs(1794/139)** |
    | 4dB Z16 ET 早停 | 1165µs(1008/158) | **505µs(379/127)** |
  - CPU 侧编码开销两处各省 ~72/31µs;**ET 早停场景 2.3× 总延迟优势**(dispatch 版
    290 次 dispatch 无条件提交,持久版 break 即停);满迭代大 Z 场景持久版单 TG 并行
    度受限,慢 ~13%。实链(E2E)是小 Z 高 SNR ET 早停形态——持久版的目标场景。
- 待用户跑 E2E:`--pusch_ldpc_decoder_type metal_persistent` 腿,对比 [ul_ldpc_decode]。

### E2E 第七轮:metal_persistent 实链(2026-08-18,三腿仍同噪声带)

| 腿 | 样本 | pipeline mean/median | ldpc_decode mean/median | ldpc p95/p99 |
|---|---|---|---|---|
| auto(neon) | 209 | 229.0/226.0 µs | 41.8/38.0 µs | 80/95 µs |
| metal 分层 | 260 | 215.2/207.0 µs | **34.5/29.0 µs** | 64/116 µs |
| metal_persistent | 193 | 236.6/229.0 µs | 43.3/39.0 µs | 81/109 µs |

- **持久版基准优势未传导到实链**:基准 4dB Z16 处 persist 505µs vs metal 1165µs
  (2.3×),但实链 [ul_ldpc_decode] 三腿 29-39µs 中位,persist 反比 metal 慢 ~10µs。
- 原因:实链 TB 比基准 Z16 点更小/更快(dispatch 链在链上只值 ~35µs,290 次
  dispatch 已极廉价);持久版的单 TG 固定成本(1024 线程启动 + prologue +
  每轮 ~46 个 threadgroup_barrier)约 40µs,与省下的 dispatch 相抵,32 行/层的
  并行度上限在小 Z 上开始显现。持久版优势在 Z16 级 ET 场景成立,链上工作在
  该尺度之下——固定成本主导区。
- **生产裁决不变:实链工作点三腿统计无分离,metal(分层)仍是默认 GPU 选择**
  (大 Z 也最优)。metal_persistent 保留为可选类型,价值 = 平台负结果记录 +
  单 dispatch 形态(ICB 等未来路线的基础)。
- 旁注:18:56 用户重跑的 flooding 腿 ldpc 样本 1111 vs pipeline 622——slot
  匹配窗在突发到达下漏采样,ldpc 序列的 last-write-wins 匹配更稳,两条序列
  分叉 = 交付成簇的又一签名。

### E2E 第八轮:iperf3 大码块(2026-08-18,结构性结论 + 两腿数据)

- **iperf3 结构问题(重要)**:DL 腿(唯一完整跑完的方向)不经过 PUSCH 解码器
  ——解码器类型只影响 UL;UL 腿被锁步饥饿压死(33-40 Kbps 或 idle timeout,
  cwnd 卡在 14-62KB、分钟级零字节、256KB 单窗爆发=UL grant 成簇签名)。
  即 **iperf3 在本链路上测不到解码器差异**,吞吐差异(483/954/666 Kbps)
  是链路均衡噪声。
- 有效对比仍是探针(大 TBS,iperf3 饱和期):
  | 腿 | 样本 | pipeline mean/median | ldpc_decode mean/median | ldpc p95/p99 |
  |---|---|---|---|---|
  | auto | 879 | 303.7/295.0 µs | 63.7/60.0 µs | 95/125 µs |
  | metal | 290 | 317.4/311.0 µs | 60.9/60.0 µs | 106/140 µs |
  | persistent | 759 | 294.0/288.0 µs | 64.4/60.0 µs | 103/140 µs |
  - 大 TB 使解码 29-39→60µs、管线 207-229→288-311µs(量级正确),**三腿中位
    解码时间全部 = 60.0µs**(mean 63.7/60.9/64.4)——大 TBS + ET 早停下三种
    实现统计无分离。样本数 879/290/759 镜像各腿 UL 成簇差异,非解码器。
  - 样本量 290-879 比 ping 轮(193-260)大得多——大 TBS 饱和期探针工作正常。

## 4.13 P12:metal_async 异步 delta-BP(2026-08-18,无栅栏残差注入,负结果+范式验证)

- 动机:规避 Metal 无跨 TG 同步原语的平台限制(见 4.12),用异步残差信念传播:
  无任何栅栏,每行一个常驻 TG 世代循环,min-sum 归约后把每边残差
  ΔR = R_new − R_old 以**交换律原子加法**注入 Q16.16 定点后验池
  (Q8.8 会溢出:|P| 累积至 ~1900);专用轮询 TG 按行更新计数等满一代后
  校验 syndrome,置 stop_flag;max_iter 代界保证内核必然终止(GPU watchdog
  安全)。decode() = init + 网格两 dispatch,网格 (m_aligned+1)×32。
- **范式验证成功**:全程零同步、零死锁,高 SNR 收敛(4dB 484/500)——竞争容忍
  的异步消息传递在该平台可用,平台负结果的规避路径成立。
- **BLER 裁决(与预判一致,继承洪泛瀑布)**:0dB 0-1/1000(mi=10-20,CPU 1986/2000、
  分层 901/1000);2dB 7/1000(CPU 1914、分层 971);4dB 96.8%(分层/洪泛全过)。
  异步版 ≈ 洪泛版瀑布,略差一档(定点量化 + 陈旧读的世代代价)。
- **延迟**:4dB Z16 558µs(≈ persist 505µs,好于 dispatch 分层 1165µs——省掉了
  ET 后空转 dispatch);0dB Z64 满跑 3341µs(**最慢**,~2 万原子 RMW/代 × 10-20 代
  的注入流量,如预判)。
- **裁决:生产不出局,保留为可选类型** metal_async——价值 = 无栅栏范式在
  5G 基图上的工程验证 + 未来异步调度研究的基础。单测 ALL OK(有噪
  26-98/100,瀑布差照实记录)。

### 高码率 r=0.85 CPU 错误地板调查(2026-08-19)

- 现象(Z16/Z64,r0.85,1000 块/点):CPU(auto=neon)在 Z64 地板 ~55-60%
  (4.4dB 起 549-601/1000 不随 SNR 改善)、Z16 地板 ~96-98%;GPU
  (metal_persistent)全部收敛到 100%。低码率 0.33/0.5/0.67 正常(GPU 低码率
  还有优势)。
- **非我们引入的 bug**:`git log -- ldpc_decoder_neon.cpp ldpc_decoder_generic.cpp
  ldpc_decoder_impl.{h,cpp}` 只有上游提交(license/版权/命名空间)——CPU 路径
  逐字节未被修改,地板是上游 srsRAN 行为。
- GPU 无地板的三个算法差异:
  1. **全图 vs 截断图**:CPU 只解收到区域的层(r0.85 Z64 仅 6 层:
     `codeblock_length = max(E+2Z, K+4Z)` → 28 列 → 6 行;其余行涉及未传
     奇偶列而跳过);GPU 解全 46 层,未传位用偏置擦除。
  2. **擦除值**:CPU 未传/打孔位 = 精确 0(符号完全对称、不可分辨);GPU =
     +1 弱偏置(打破打孔列符号对称性)。
  3. **β 偏移**:CPU 纯归一化 min-sum(α=0.8,β=0);GPU = OMS(α=0.7,
     β=0.5)——偏移 min-sum 已知可压低错误地板。
  三者同向:CPU 在高码率截断图 + 零擦除下,两打孔列(2Z 位)的符号歧义使
  分层 min-sum 卡在陷阱集 → 地板随 Z 增大(Z16 3% → Z64 40%)。
- **迭代数澄清**:--max-iter/--cpu-max-iter=6 只是上限,**两者都早停**——CPU
  每轮算 CRC 通过即返回(`crc != nullptr` 分支),GPU 每轮 ET 门控 syndrome
  干净即 break;高 SNR 实跑 1-2 轮,地板点两者都烧满 6 轮。
- 工具增强:基准每 SNR 点墙钟时间入 CSV 第 5 列 `time_s`;plot_bler.py 在
  每条曲线末端标注该 rate 的总运行时间与每点均值(旧 CSV 无时间列时自动跳过)。

## 4.14 LLS 内核恢复:metal_lls 工厂类型(2026-08-24,并行度优先的可选解码器)

- 动机:LLS 比特翻转族的优势是**极致并行**(每轮仅 2 个 dispatch,无消息传递、
  无逐边存储),作为 NMS 之外的并行度研究基线。从 git 历史恢复 4.10 删除的
  LLS 路径(commit `c823a1eb7d` 之前的形态,`archive/metal-lls-flooding` tag
  亦完整保留)。
- 恢复内容:
  - `ocudu_lls_decoder.metal`:SynchroPlus 4-kernel LLS 着色器逐字恢复
    (`init_hard_decisions` / `compute_syndrome` / `cn_centric_scan` /
    `update_llr_hpred`),重新离线编译 `ocudu_lls_decoder.metallib`;
  - 引擎 `decoder_engine::algo::lls`:LLS 独立算法族(自己的 metallib +
    pipeline 缓存);LLS 缓冲(s_hard / h_pred / err_eq_cnt / suspect_cnt /
    evidence_sum / vn_total_cn / debug)+ CPU 侧列权计算;fp16 零拷贝输入;
    单命令缓冲录制 init + 全量 syndrome + [scan; update]×max_iter;
    终态 syndrome 由 CPU popcount(h_pred) 重算;
  - 适配器:LLS 槽位恢复 fp16 宿主填充(int8→fp16、±64 钳制、擦除 +1 偏置)、
    H^T 生成、alpha 默认 0.8;
  - 工厂/CLI 新类型 **`metal_lls`**(`expert_phy --pusch_ldpc_decoder_type
    metal_lls`);单元测试与 BLER 基准支持 `--gpu-type metal_lls`。
- **验证(2026-08-24)**:
  - 单元测试无噪声三配置(BG1 Z16/BG2 Z16/BG1 Z256)LLS **100% 位精确、
    1 轮收敛**,与 4.6 历史记录一致;有噪声断言(无假阳性 + LLS ≤ CPU 通过率)
    全过;
  - BLER 冒烟复现历史瀑布:BG2 Z16 R=1/3 @6dB 77/100、@8dB 97/100、
    @10dB 100/100(CPU 全程 100% → 差距 ~6-8dB);R=2/3 @0..8dB 0/100
    (高码率差距 >10dB)——**LLS 比分层 NMS 差 5-8dB 符合预期**;
  - GPU 延迟 ~300-430us/块(Z16,6 轮上限),与历史 LLS 测量(median 261us、
    p99 422us)同量级。
- 已知缺陷(4.6 全文):sign(0)=+1 擦除缺陷(适配器 +1 偏置已修)、每行仅取
  两个最弱 VN、delta 公式为经验启发式——**下一步是 BLER 性能优化**(候选:
  多嫌疑人扩展、证据公式改进、与打孔感知的擦除处理),并行度优势不变。

### BLER 基准工具增强(2026-08-24,分离 SNR 扫描 + 解码耗时图)

- CLI:`--snrs` 定给 GPU,`--snrs-cpu` 覆盖 CPU 的扫描(缺省共用 `--snrs`);
  `--cpu-max-iter 0`(缺省)= CPU 共用 `--max-iter`。CSV 覆盖两者合并后的
  SNR 点,未评估一方的 pass/time/延迟列留空,头部 `snrs_cpu_split=` 标记。
- plot_bler.py:兼容空单元格(分离扫描);新增每 (bg,z) 的
  `bler_bg*_z*_latency.png` 解码耗时图(对数轴,CPU mean 实线 / GPU mean
  虚线 / GPU p95 点线,仅 CRC-OK 样本),`--no-latency` 可关闭。

### E2E 排查:metal_lls 实链 attach 后立即 RRC Release(2026-08-24,不是状态 bug)

- 现象:UE RRC Connected 后立即收到 RRC Release;gnb 全程只有 1 个 CRC-OK
  UL 解码(tbs=11 msg5 的重传),之后所有 UL PUSCH 都失败。
- 日志复核(172 次 PHY 解码尝试,4 个 TB 尺寸 = 4 个 z:tbs=11/528/512/437 →
  z≈12/208/192/160):
  - **无 crash**:H/H^T 按 (bg,z) 首次使用惰性生成(无需预生成)——tbs=528
    首次尝试 t=6.6ms 即槽位构建,之后 ~1.9ms/次;4 个 z 槽位全部构建成功并
    反复解码;gnb 全程存活至手动关机("Workers stopped successfully"),
    崩溃假设被日志排除;
  - 失败模式:每次解码跑满 6 轮不收敛(iter=6.0/min=6/max=6,GPU 内 syndrome
    从未清零),11:02:22.3 RLF "MAC max consecutive CRC KOs reached" → release;
    唯一 OK 是 tbs=11 的**重传**(new_data=false 软合并后过,tcr=0.117 极低码率)。
- **信号质量根因(修正初版"3dB 噪声"结论)**:ZMQ 传输确实无损,但信号在进
  ZMQ 之前就已经失真——gnb 测量 evm≈0.5(=-6dB)、sinr_ch_est=6.5dB、
  sinr_eq=3dB、epre=+10.2dB、rsrp=9.4dB。epre +10dB 说明 srsue 的 UL 时域
  基带幅度约 3.2 倍过热,而线上格式是 int16(srsue 端 float→int16 @32767
  饱和)→ **UE 侧硬削波**,时域 OFDM 峰被削 → 频域 EVM≈50% 的确定性失真。
  之前 metal 跑的样本(evm=0.5/epre=10.2/sinr=3dB)与本次完全一致——min-sum
  3-6 轮能扛住,LSS 比特翻转启发式扛不住。DL 方向(gnb 基带 ≤1)不削波,
  所以 UE 收 DL 正常。
- BLER 复现(速率匹配输入,100 块/点):BG2 Z48 R=1/2 @3dB LLS 0/100 vs
  CPU 100/100;BG2 Z11 R=2/3 @20dB LLS 95/100(高 SNR 下同实例持续正常解码,
  排除状态残留)。
- **让 LLS 跑通 E2E 的办法**:修掉 UL 削波(本身就是应修的链路问题)——srsue
  配置加 `[phy] force_ul_amplitude = 0.5`(FORCE_AMPLITUDE 模式,时域峰 ≤1.0);
  验证:重跑后 gnb 日志 evm→0、sinr_ch_est 跳至 20-30dB,届时 LLS 应全部
  解码。LLS 的 BLER 优化(多嫌疑人/证据公式/擦除处理)仍是下一优先级。
- 注意:日志里"crc=OK 2 次"实为 1 次——每条 debug 条目中 crc=OK 出现两行
  (摘要行 + verbose 块)。

### 双日志对比 + 信号质量定位修正(2026-08-24,metal 50-ping 正常跑 vs LLS)

- 对照数据(同一 UE/链路,只换解码器):
  | 运行 | 解码尝试 | crc=OK | 会话 |
  |---|---|---|---|
  | metal_lls (`/tmp/gnb_lls.log`) | 172 | **1** | ~3s 后 RLF → RRC Release |
  | metal (`/tmp/gnb.log`) | 196 | **191**(含 14 个 16QAM) | 50 ping 全程存活,手动关机 |
  两 run 的 evm≈0.5 / epre / sinr 完全一致 → 信号质量与解码器无关。
- **修正"UE 侧 int16 削波"的判断**:查 srsue 源码——`phy.force_ul_amplitude`
  旋钮只接入 LTE UL 路径(`phy_common.cc`),NR PHY(`srsue/src/phy/nr`)不读它;
  而 NR PUSCH TX 内部已把时域峰归一化到 0.99(`ue_ul_nr.c` "Normalise to
  peak")→ 线上 int16 不会饱和,削波不成立,该旋钮对 NR 无效。
- **DL 方向完美**:UE 侧小区搜索日志 `snr=+118.9dB`(SSB)——ZMQ 链路无损、
  gnb TX/UE RX 全正常。UL 的 evm≈0.5(有效 ~3-11dB)是 srsue NR TX 或 gnb
  UL RX 链内部的确定性退化,两 run 一致。min-sum 家族容忍它(97.5% 通过),
  LLS 在 z=160-208/QPSK 需要 ~10dB+ → 171/172 失败。
- **下一步定位实验**:153 上 `zmq_remote_rx` 抓 UE 发往 gnb 的原始样本,对
  已知 PUSCH 算 EVM——原始 TX 样本就 ~50% EVM 则是 srsue NR TX 缺陷(查
  PUSCH 调制/加窗/CFO 预补偿);TX 样本干净则是 gnb UL RX 链(ZMQ 定界/
  同步/FFT/信道估计)。修 UL 质量与 LLS BLER 优化是两个独立方向。

## 4.15 LLS BLER 优化方案(2026-08-24:多嫌疑人 / 证据公式 / 打孔感知擦除)

### 目标与硬约束

- 目标:保持 LLS 的并行度优势(每轮 2 dispatch、无消息传递、无逐边存储、
  GPU 内早停)不变,把与分层 NMS 的 BLER 差距从 5-8 dB 压到 2-4 dB,并消除
  高码率/大擦除尾的高 SNR 残余失败(当前 ~5%)。
- 硬约束:单命令缓冲 + 2 dispatch/轮不变;h_pred/HT-XOR 增量 syndrome 维护
  不变(纯算术替换);无噪声单元测试 100% 位精确、1 轮收敛不变;ET 语义不变;
  新增状态仍是 per-VN / per-row 原子缓冲,不引入逐边结构。

### 度量协议与基线冻结

- 基线 = 现有代码(α=0.8、k=2、公式 F1、+1 擦除偏置、max_iter=6),基线 CSV
  冻结归档。
- 评测配置组(A-D,覆盖块大小×码率):A = BG2 Z16 R1/3(小块低码率,已知
  瀑布 6→10dB)、B = BG2 Z48 R1/2(中块)、C = BG2 Z11 R2/3(高码率,差距
  >10dB 的最痛点)、D = BG1 Z256 R1/3(大块,贴近 E2E 的 z=160-208/QPSK)。
- 指标:90%/99% 通过率的 ΔSNR(瀑布位移);最高 SNR 点残余失败数(floor,
  目标 0);CRC-OK/FAIL 平均轮数;GPU 解码耗时(mean/p95,回归护栏 ≤ +10%)。
- 等延迟预算对照:先实测 NMS 与 LLS 的单轮成本(已有数据:6 轮总耗时两者
  接近——LLS 每轮 dispatch 少,但扫描同样遍历全部边),按实测给 LLS 一个
  等延迟的轮数预算(max_iter ∈ {6,12,20,40} 扫一遍)——5-8 dB 差距里可能含
  一部分"同 max_iter、不同延迟"的预算伪差距,先量化、后优化。

### 方向一:证据公式与后更新幅值策略(纯 update_llr_hpred 改动)

- 现状 F1:δ = α·(e_cnt/tc)²·(e_sum/s_cnt);new = old − sign(old)·δ,穿越零
  即翻转,翻转后 |new| = δ − |old|("过冲")。
- 三个已知缺陷:(a) 分子只含嫌疑行而分母用得票数 s_cnt,量纲不干净;
  (b) 无自先验项——|old| 大的正确 VN 也会被弱票侵蚀/翻转(IMWBF 文献的
  核心修正);(c) 过冲使翻转后 |new| 可趋近 0 → 振荡;|old| 极小 VN 更新后
  可能落 0,轮内重新触发 sign(0)=+1 缺陷。
- 设计空间(逐轴 A/B,先单轴后联合):
  - 归一化:N1 /s_cnt(现状)、N2 /e_cnt、N3 /tc(全列权"总侵蚀",配合去掉
    ratio 项);
  - 比率指数:p ∈ {1, 2}(现状 2);
  - 聚合:mean(现状)、max(需 per-VN float-max 原子缓冲)、min(保守);
  - 自先验:δ' = δ − β·|old|,β ∈ {0, 0.1, 0.25}(β>0 时翻转判据变为
    "票强 > (1+β)·自置信",IMWBF 式);
  - 后更新幅值统一策略:new = −sign(old)·max(ε, δ − γ·|old|)——γ=1 现状
    过冲、γ=0 重置(翻转后置信 = 证据)、γ=0.5 混合;ε=1 幅值地板,顺带修复
    轮内 sign(0) 陷阱;
  - 参考点 F7:δ = Σ_{不满意行} m1_row / tc(经典并行 WBF 度量,k=∞)。

### 方向二:多嫌疑人扩展(scan 的 k-min 归约 + 投票)

- 现状 k=2、证据分配 E-self(i1←m2、i2←m1)。E-self 在 k≥3 退化:所有
  j≥2 的嫌疑人都拿 m1,必须换分配。
- 设计:每行取 k ∈ {3,4} 个最弱 VN;证据分配 A/B:E-peel(嫌疑人 i_j ←
  m_{j+1},k=2 时 = {m2, m3},比现状给次嫌疑人更大证据;语义 = "最强 k 个
  翻完后,行内剩余的最小支撑")vs E-uniform(全部 ← m_{k+1})。
- 实现:per-lane 维护 (k+1) 个最小值 + 蝶形归并(k=4 时 5 个);寄存器压力
  可控,占用率下降则退回 k=3;evidence_sum 原子流量 ×k/2,实测护栏。
- k 调度:静态 per-round 表 k_sched[max_iter](如 {2,2,3,3,4,4}),按
  actual_iters 索引(常数缓冲);Phase 2b 可选停滞自适应(error_count 连续
  2 轮不降 → k++)。
- 振荡护栏(cooldown):per-VN 1-bit 标志(独立原子缓冲);init 清零;scan 对
  cooling VN 不投票(err_eq_cnt 仍计数);update 翻转者置位、未翻转者清零 →
  翻转后 1 轮免疫,确定性无死锁;与 E-peel 配合抑制"正确弱 VN 反复翻转"。

### 方向三:打孔感知擦除处理(adapter → 引擎新输入 + 陷集逃逸)

- 新增擦除掩码:adapter 已知布局([0,2Z) 打孔 + tail 未传),打包 n_h_chunks
  字、零拷贝缓冲传入,init/update 只读;+1 偏置保留为基线 C1。
- 策略:C2 擦除首翻锁(掩码 VN 首翻需 s_cnt ≥ 2 且 δ > τ_e,防单弱票就翻;
  擦除 VN 冷却 2 轮);C3 擦除证据增益 δ *= η_e(η_e ∈ {1.25, 1.5},擦除位
  无信道先验,证据不应被折扣);C4 停滞逃逸:error_count 连续 T=3 轮不降 →
  硬多翻(所有 e_cnt ≥ θ·tc、θ=0.75 的 VN 同时翻转,LLR = −sign·max(ε,
  α·mean_vote),照常 HT-XOR)。实现:ctrl 扩为 5 字(error_count /
  early_terminate / actual_iters / prev_error_count / stall_flag,新字段仅 LLS
  使用);由 update 的 vn0 在上一轮末置 stall_flag,下一轮 update 起始执行硬
  翻(滞后一轮、无核内竞态)。
- 目标:C 类(高码率/大擦除尾)高 SNR floor 5% → 0。
- 不做:LLS+NMS 混合后处理(破坏单解码器纯度,记入未来工作)。

### 参数流水线与工具改动

- 引擎:lls_params_t{alpha, beta, p, gamma, eps, erasure_boost, k_suspects,
  k_sched[8], cooldown, stall_escape} 取代单个 factor setBytes;init 存储、
  decode 一次 setBytes 传常数结构(scan/update 共用)。
- 适配器:构造签名兼容保留,新增 set_lls_params() 供测试注入;工厂 metal_lls
  默认参数 = 冠军值(收尾时改),此前默认 α=0.8 不变。
- BLER CLI:--lls-k/--lls-beta/--lls-p/--lls-gamma/--lls-eps/--lls-cooldown/
  --lls-erase-boost/--lls-stall-escape(--norm 已映射 α);CSV 头加
  lls_params= 行;CSV 加 gpu_mean_iters_ok/fail 两列,plot_bler.py 画停滞分布。
- 调试:VNStats 缓冲已就位;加 --dump-fail PATH(每 SNR 点首个失败块的终态
  LLR/syndrome/末轮统计/擦除掩码落盘),按列聚合翻转计数定位失败列(打孔列?
  尾列?)。

### 分阶段实施与验证门

- Phase 0(工具+基线):lls_params_t 流水线、CLI、dump-fail、基线 CSV 冻结、
  等延迟预算测量(真实算法差距量化)。
- Phase 1(公式轴,纯 update 改动):归一化/β/γ/ε 单轴 → 联合;门:无噪声
  1 轮收敛 + A-D 上 ΔSNR ≥ 1dB 或 floor 下降,延迟无回退。
- Phase 2(多嫌疑人+cooldown):k-min 归约、E-peel、k 调度、cooldown;门:
  同上 + 占用率/原子争用护栏。
- Phase 3(擦除+陷集逃逸):掩码、首翻锁、η_e、停滞硬多翻;门:高 SNR
  floor → 0。
- Phase 4(冠军+回归+E2E):联合网格(粗→细)定冠军;单元测试与
  metal/metal_flooding/metal_persistent/metal_async 回归不变;工厂默认 =
  冠军;E2E 重跑 gnb metal_lls,预期 UL CRC-OK 大幅回升(与 srsue UL 质量
  修复两个方向并行)。
- 每 Phase 的 A/B 都用现有 BLER 基准(200 块/点,秒级),CSV+图归档
  bler_results。

### 预期收益与风险

- 预期(假设,逐门验证):β 自先验 + γ/ε 策略:1-2 dB;多嫌疑人 + cooldown:
  0.5-1.5 dB;擦除策略 + 停滞逃逸:floor → 0;等延迟预算修正:剥离"预算伪
  差距"。合计目标:差距 5-8 dB → 2-4 dB、高 SNR floor 归零、并行度不变。
- 风险:k-min 寄存器压力(k=4 退 k=3);evidence 原子争用(护栏实测);
  cooldown/首翻锁交互(设计上无条件清零,已规避死锁);fp16 量化(ε 地板兜
  底);冠军在真实脏 UL(evm≈0.5)上增益打折——BLER 基准是 AWGN,需 E2E
  复核。

### Phase 0/1 实施记录(2026-08-24,参数流水线 + 证据公式扫描)

- **Phase 0 已落地**:`decoder_engine::lls_params`(alpha/beta/p/gamma/eps/
  k_suspects/norm_mode,与着色器 `LLSParams` 逐字段对齐,一次 setBytes 传入);
  引擎 init 新增可选 `lls` 参数(缺省 = 旧行为,alpha 取 factor);适配器
  `set_lls_params()`(丢弃 LLS 槽位,下次解码重建);BLER CLI 新增
  `--lls-beta/--lls-p/--lls-gamma/--lls-eps/--lls-norm/--lls-k`(`--norm`
  兼作 alpha);CSV 头加 `lls_params=` 行、数据行加
  `gpu_mean_iters_ok,gpu_mean_iters_fail` 两列(plot_bler.py 无需改,多列忽略)。
- **回归**:单元测试全过(无噪声 100%/1 轮收敛不变);基线复现成功——
  A:6dB 74/100 ≈ 历史 77;8dB 97/100 ✓;B:8dB 27/100 ✓;C:14-22dB 92-95/100
  的 5% floor 复现 ✓。
- **Phase 1 单轴扫描(A = BG2 Z16 R1/3,4/6/8dB,100 块/点)**:
  | 变体 | 4dB | 6dB | 8dB | 结论 |
  |---|---|---|---|---|
  | baseline(α0.8, γ1 过冲) | 26 | 74 | 97 | 基线 |
  | β=0.1 / 0.25(自先验) | 22/17 | 70/73 | 97 | **有害**,淘汰 |
  | γ=0(翻转后重置为证据) | 39 | 84 | 97 | ★ 冠军机制 |
  | γ=0.5 | 35 | 83 | 97 | 次优 |
  | ε=1(幅值地板) | 39 | 84 | 97 | 等效 γ0 |
  | norm=1/2、p=1 | 19-23 | 75-83 | 97 | 中性偏弱 |
- **α 精细扫描(γ=0 下)**:A@4dB 随 α 单调升 39(0.8)→46(1.25)→51(1.5)→
  56(1.75);B@6dB 2(0.8)→22(1.25)→37(1.5);C 在 α1.25 下 200/200 全过。
  ⚠️ norm=2 在 B 上 α1.25 出现 25% 高 SNR floor(发散)——norm2 淘汰。
- **高 SNR floor 归零**:C(BG2 Z11 R2/3, 200 块/点)baseline 92-95/200 →
  γ=0 后 14-22dB **200/200**——"过冲"(|new|=δ−|old|)正是高码率擦除位
  振荡/陷集的主因,Phase 3 的 C4(停滞逃逸)可能已不再必需。
- **max_iter/α 扩展扫描**:A 在 α2.0+20 轮达 56/91(4/6dB),但更高轮数
  收益边际化(B@6dB 饱和 ~53-56,OK 轮数=满额 → 剩余失败是陷阱非预算);
  ⚠️ α≥2.0 让 C 的 floor 回归(187-192/200)→ **α 必须在瀑布与 floor 间
  平衡**。
- **Phase 1 冠军 = γ0 + α1.5**(已写入工厂默认:引擎 lls_params γ 默认 0、
  适配器 LLS α 默认 1.5,Bler CLI 默认显示同步):
  | 配置 | baseline | 冠军 | 收益 |
  |---|---|---|---|
  | A BG2 Z16 R1/3 | 26/74/97 @4/6/8dB | 52/87/97 | +1.5-2dB 瀑布 |
  | B BG2 Z48 R1/2 | 0/27 @6/8dB | 49/95(+12 轮) | +2-3dB |
  | C BG2 Z11 R2/3 | 92-95/100 floor | **200/200** | floor→0 |
  | D BG1 Z256 R1/3 | 74 @8dB | 74 | 不动(Phase 2 目标) |
  与 NMS 差距仍 ~8-10dB(D 最大)——符合预期,留给 Phase 2 多嫌疑人 +
  cooldown、Phase 3 擦除处理。
- **E2E 待办(用户执行)**:gnb 需 sudo;命令
  `sudo ./gnb -c configs/gnb_zmq.yaml --pusch_ldpc_decoder_type metal_lls
  --pusch_dec_max_iterations 12`(迭代 12 轮:冠军在瀑布区常用 6-12 轮,
  LLS 每轮 ~50-75us@Z160-208,总预算仍 ~1ms);验证 UE attach + 拿 IP +
  ping,gnb 日志 crc=OK 比例应大幅高于 metal_lls 旧默认(1/172)。

## 5. 交付物清单

- [ ] `metal/PLAN.md`（本文件）+ `metal/.gitignore`
- [ ] Step 1：CMake 连通性（`ENABLE_METAL_LDPC`、metallib 编译、`ocudu_ldpc_metal` 静态库）
- [ ] Step 2：`ldpc_decoder_metal` + "metal" 工厂类型 + H 生成器 golden 比对 + BLER 对拍报告
- [ ] Step 3：`ldpc_decoder_type` 配置 + 分流开关 + E2E A/B 数据表（[ul_pipeline] / ping / BLER）
- [ ] Step 4：（可选）`ldpc_encoder_metal` + 编码对拍
