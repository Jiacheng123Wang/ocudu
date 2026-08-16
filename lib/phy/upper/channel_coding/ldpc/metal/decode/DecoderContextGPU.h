#ifndef DECODER_CONTEXT_GPU_H
#define DECODER_CONTEXT_GPU_H

#include <cstdint>
#include <memory>
#include <string>
#include <fstream>
#include "../encode/MetalAlignedBuffer.hpp"
#include "ldpc_gpu_decoder.h"

#ifdef __arm64__
typedef __fp16 float16;
#else
typedef uint16_t float16;
#endif

struct DecoderContext
{
    // 维度与参数
    uint32_t N_logical;
    uint32_t n_h_chunks; // N 方向 32 对齐后的分块数
    uint32_t M_aligned;  // M 方向 32 对齐补齐后的行数
    float alpha;

    // 真正的全 Kernel 译码器实例
    LDPCGPUDecoder engine; // 直接持有对象

    // --- 极简零拷贝数据缓冲区 (仅保留 CPU 需要提供给 GPU 的输入数据) ---
    // 1. 输入 LLR：CPU 写入噪声数据，GPU 零拷贝读取/更新
    std::unique_ptr<MetalAlignedBuffer<float16>> sp_llr_buf;

    // 2. 校验矩阵 H：CPU 从文件读取，GPU 零拷贝只读
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> h_matrix_buf;

    std::unique_ptr<MetalAlignedBuffer<uint32_t>> ht_matrix_buf; // 新增
    uint32_t h_pred_len;                                         // 即 M 方向对齐后的 uint32 数量 (n_ht_chunks)

    // 3. 变量节点列重 (Column Weights)：GPU 计算 delta 的分母必需
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> vn_total_cn_buf;

    /**
     * @brief 初始化上下文、译码器引擎并分配零拷贝对齐内存
     */
    bool init(const char *metallib_path, uint32_t n_total, uint32_t m, float step_alpha)
    {
        N_logical = n_total;
        alpha = step_alpha;

        // 必须与 mm 文件内部的对齐逻辑严格一致
        M_aligned = ((m + 31) / 32) * 32;
        n_h_chunks = (N_logical + 31) / 32;
        h_pred_len = M_aligned / 32; // 计算 HT 每一行的宽度

        // 1. 初始化底层 Metal 引擎
        if (!engine.init(metallib_path, n_total, m, step_alpha))
        {
            return false;
        }

        try
        {
            // 2. 仅分配必须通过零拷贝传递给 GPU 的缓冲区 (4KB 对齐)
            // 注意：这里 engine_handle 传 nullptr 即可，因为分配只用 posix_memalign
            sp_llr_buf = std::make_unique<MetalAlignedBuffer<float16>>(nullptr, n_h_chunks * 32);
            h_matrix_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(nullptr, M_aligned * n_h_chunks);
            vn_total_cn_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(nullptr, n_h_chunks * 32);
            // 分配空间：N_aligned x (M_aligned / 32)
            ht_matrix_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(nullptr, (n_h_chunks * 32) * h_pred_len);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    // 适配 main 函数调用的接口
    bool load_files_manually(const std::string &h_path)
    {
        std::ifstream fs(h_path, std::ios::binary);
        if (!fs)
            return false;
        fs.read((char *)h_matrix_buf->get(), M_aligned * n_h_chunks * sizeof(uint32_t));

        // 2. 【核心】从 H 构造 HT (位级转置)
        uint32_t *h_ptr = h_matrix_buf->get();
        uint32_t *ht_ptr = ht_matrix_buf->get();
        memset(ht_ptr, 0, N_logical * h_pred_len * sizeof(uint32_t));

        for (uint32_t r = 0; r < M_aligned; ++r)
        {
            uint32_t word_m = r / 32;        // 该校验方程对应的位在 HT 行中的哪个 uint32
            uint32_t bit_m = 1u << (r % 32); // 该校验方程在 uint32 内的掩码

            for (uint32_t c_chunk = 0; c_chunk < n_h_chunks; ++c_chunk)
            {
                uint32_t mask = h_ptr[r * n_h_chunks + c_chunk];
                while (mask)
                {
                    uint32_t bit_n = __builtin_ctz(mask); // 找到当前 1 所在的 VN 索引
                    uint32_t vn_idx = c_chunk * 32 + bit_n;

                    if (vn_idx < N_logical)
                    {
                        // 在该 VN 对应的 HT 行中，打上这个校验方程的标记
                        ht_ptr[vn_idx * h_pred_len + word_m] |= bit_m;
                    }
                    mask &= (mask - 1);
                }
            }
        }

        // 3. 调用更新后的引擎接口，传入 HT
        engine.load_h_and_ht_matrix(h_ptr, ht_ptr, vn_total_cn_buf->get());
        return true;
    }

    // 适配 main 函数调用的译码接口
    int decode(const void *in_llr, uint8_t *out_bits, int max_iter)
    {
        return engine.decode(in_llr, out_bits, max_iter);
    }

    void release() { engine.release(); }
};

#endif
