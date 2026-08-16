// clang++ -std=c++17 -O3 test_llr_update_2kernel.cpp MetalLLSUpdater.mm -o test_llr -framework Metal -framework Foundation -fobjc-arc

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstring>

// 引入上一步定义好的接口文件
#include "MetalLLSUpdater.h"

// 针对 Apple Silicon M4 的原生 float16 支持
#ifdef __arm64__
typedef __fp16 float16;
#else
typedef uint16_t float16;
#endif

// 核心封装：分配满足 Metal 零拷贝要求的 4096 字节对齐内存
void *alloc_aligned_buffer(size_t logical_size)
{
    // 强制将分配长度补齐到 4096 的倍数，防止 GPU 按页读取时发生越界段错误
    size_t aligned_size = (logical_size + 4095) & ~4095;
    void *ptr = nullptr;

    // posix_memalign 要求 alignment 是 sizeof(void*) 的倍数且为 2 的幂
    if (posix_memalign(&ptr, 4096, aligned_size) != 0)
    {
        std::cerr << "Error: posix_memalign failed to allocate memory!" << std::endl;
        exit(EXIT_FAILURE);
    }

    // 初始化为 0，这对于填充的 Padding 区域非常重要
    memset(ptr, 0, aligned_size);
    return ptr;
}

int main()
{
    // --- 1. 参数定义与对齐计算 ---
    // 根据需求：Z=2，逻辑 M=92, 逻辑 N=136
    uint32_t Z = 256;
    uint32_t M_logical = 46 * Z; // 92
    uint32_t N_logical = 68 * Z; // 136

    // 按 32 位 (uint32) 对齐
    uint32_t n_h_chunks = (N_logical + 31) / 32;     // 136 / 32 = 4.25 -> 5 块
    uint32_t N_aligned = n_h_chunks * 32;            // 5 * 32 = 160 bits
    uint32_t M_aligned = (M_logical + 31) / 32 * 32; // 依据需求填充至 96 行

    float alpha = 0.5f;

    std::cout << "--- Metal LLS Updater Zero-Copy Test ---" << std::endl;
    std::cout << "N (logical): " << N_logical << " bits, N (aligned): " << N_aligned << " bits." << std::endl;
    std::cout << "M (logical): " << M_logical << " rows, M (aligned): " << M_aligned << " rows." << std::endl;

    // --- 2. 分配 4096 对齐的物理内存 ---
    size_t llr_size = N_aligned * sizeof(float16);
    float16 *llr_array = (float16 *)alloc_aligned_buffer(llr_size);

    size_t h_matrix_size = M_aligned * n_h_chunks * sizeof(uint32_t);
    uint32_t *h_matrix = (uint32_t *)alloc_aligned_buffer(h_matrix_size);

    size_t h_pred_size = M_aligned * sizeof(uint32_t);
    uint32_t *h_pred = (uint32_t *)alloc_aligned_buffer(h_pred_size);

    size_t s_hard_size = (N_aligned / 32) * sizeof(uint32_t);
    uint32_t *s_hard = (uint32_t *)alloc_aligned_buffer(s_hard_size);

    // --- 3. 构造与加载测试数据 ---

    // 3.1 初始化 LLR 数据 (正负交替以触发符号判定逻辑)
    for (uint32_t i = 0; i < N_logical; ++i)
    {
        float val = (i % 2 == 0) ? 2.5f : -1.8f;
        llr_array[i] = (float16)val;
    }

    // --- 修正：初始化 s_hard 为初始 LLR 的硬判决结果 ---
    uint32_t *s_hard_ptr = (uint32_t *)s_hard;
    for (uint32_t i = 0; i < N_logical; ++i)
    {
        if (llr_array[i] < 0)
        {
            // 设置第 (i/32) 个 word 的第 (i%32) 位
            s_hard_ptr[i >> 5] |= (1u << (i & 31));
        }
    }

    // 3.2 加载 H 矩阵 (92行*136列 按块打包后的二进制文件)
    std::ifstream h_file("../g_f_matrix/H_matrix_Z256.bin", std::ios::binary);
    if (h_file)
    {
        // 由于 alloc_aligned_buffer 已清零，这里直接读取。
        // 文件若只有 92 行 (1840 字节)，后续 4 行的 1920 字节部分自然保持为 0。
        h_file.read(reinterpret_cast<char *>(h_matrix), h_matrix_size);
        std::cout << "Loaded H_matrix_Z2.bin. Bytes read: " << h_file.gcount() << std::endl;
    }
    else
    {
        std::cerr << "Warning: H_matrix_Z2.bin not found. Using zero matrix." << std::endl;
    }

    // 3.3 模拟校验方程预测状态：假设前 92 行都未通过校验 (值为 1)
    for (uint32_t i = 0; i < M_logical; ++i)
    {
        h_pred[i] = 1;
    }
    // 剩余的 92~95 行为 0，Kernel 1 的 `if (h_pred[wid] == 0) return;` 将直接跳过它们

    // 1. GPU 执行前，先备份一份旧的硬判决
    std::vector<uint32_t> s_hard_old(s_hard_size);
    memcpy(s_hard_old.data(), s_hard, s_hard_size);

    // --- 4. Metal 引擎调度 ---

    // 确保你的 .metal 文件和可执行文件在同一相对路径或提供绝对路径
    const char *shader_path = "cn_scan_vn_lls_update.metal";
    void *engine = init_llr_updater_engine(shader_path);
    if (!engine)
    {
        std::cerr << "Error: Failed to initialize Metal engine." << std::endl;
        return EXIT_FAILURE;
    }

    // 预计算统计 Buffer
    engine_prepare_internal_buffers(engine, h_matrix, M_aligned, n_h_chunks);
    std::cout << "Engine internal buffers prepared." << std::endl;

    // 触发 LLS 更新迭代 (Zero-Copy 发生在这里)
    std::cout << "Dispatching LLS update to GPU..." << std::endl;
    engine_update_lls(engine, alpha, llr_array, h_matrix, h_pred, s_hard, M_aligned, n_h_chunks);

    // 由于 mm 端调用了 waitUntilCompleted，此处代码会在 GPU 执行完毕后恢复执行
    std::cout << "GPU execution completed." << std::endl;

    // --- 5. 零拷贝结果验证 ---
    std::cout << "\n--- Data Verification ---" << std::endl;
    std::cout << "Initial: LLR[0] = 2.5, LLR[1] = -1.8" << std::endl;
    std::cout << "Updated LLR[0]: " << (float)llr_array[0] << std::endl;
    std::cout << "Updated LLR[1]: " << (float)llr_array[1] << std::endl;

    // 验证硬件级异或的结果
    // 3. 统计真正的“翻转”对比
    uint32_t actual_flip_count = 0;
    uint32_t *s_hard_new = (uint32_t *)s_hard;
    for (size_t i = 0; i < (N_logical / 32); ++i)
    {
        // 异或结果中为 1 的位，才是这一轮真正发生符号变化的位
        uint32_t diff = s_hard_old[i] ^ s_hard_new[i];
        actual_flip_count += __builtin_popcount(diff);
    }
    std::cout << "Actual bits flipped in this iteration: " << actual_flip_count << std::endl;

    // --- 6. 清理内存 ---
    deinit_llr_updater_engine(engine);
    free(llr_array);
    free(h_matrix);
    free(h_pred);
    free(s_hard);

    return 0;
}