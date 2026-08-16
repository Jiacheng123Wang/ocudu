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

## 5. 交付物清单

- [ ] `metal/PLAN.md`（本文件）+ `metal/.gitignore`
- [ ] Step 1：CMake 连通性（`ENABLE_METAL_LDPC`、metallib 编译、`ocudu_ldpc_metal` 静态库）
- [ ] Step 2：`ldpc_decoder_metal` + "metal" 工厂类型 + H 生成器 golden 比对 + BLER 对拍报告
- [ ] Step 3：`ldpc_decoder_type` 配置 + 分流开关 + E2E A/B 数据表（[ul_pipeline] / ping / BLER）
- [ ] Step 4：（可选）`ldpc_encoder_metal` + 编码对拍
