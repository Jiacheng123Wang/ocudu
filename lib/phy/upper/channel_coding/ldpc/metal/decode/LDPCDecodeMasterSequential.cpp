#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include "DecoderContext.h"      
#include "CollaborationDecoder.h" 
#include "../encode/MetalEngine.h" 

// 辅助函数：计算比特错误数（用于收敛判定）
inline uint32_t count_bit_errors(const uint8_t* pred, const uint32_t* hard, uint32_t n) {
    uint32_t errors = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t h = (hard[i / 32] >> (i % 32)) & 1U;
        if (pred[i] != h) errors++;
    }
    return errors;
}

/**
 * @brief 串行版 LDPC 译码主函数 (NEON 优化 + 安全缓存版)
 */
bool LDPC_Decode_Master_Sequential(
    void* metal_engine,
    DecoderContext& ctx,
    int max_iter,
    const float* in_s_llr,
    const float* in_p_llr,
    uint8_t* out_s_bits
) {
    // 1. 初始化 LLR
    std::memcpy(ctx.s_llr, in_s_llr, ctx.n_info * sizeof(float));
    std::memcpy(ctx.p_llr, in_p_llr, ctx.n_parity * sizeof(float));
    
    // 2. 初始化硬判决 (使用 AlignedBuffer 的 get() 获取指针)
    std::memset(ctx.s_hard, 0, ctx.s_hard_buf->physical_bytes());
    std::memset(ctx.p_hard, 0, ctx.p_hard_buf->physical_bytes());

    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        if (ctx.s_llr[i] < 0.0f) ctx.s_hard[i / 32] |= (1U << (i % 32));
    }
    for (uint32_t i = 0; i < ctx.n_parity; ++i) {
        if (ctx.p_llr[i] < 0.0f) ctx.p_hard[i / 32] |= (1U << (i % 32));
    }

    // 译码迭代
    for (int iter = 0; iter < max_iter; ++iter) {
        
        // 1. GPU 计算预测方程：P_pred = G * S_hard
        // 修改：使用新接口 metal_compute_gf2，指定 KERNEL_TYPE_PREDICT_V4
        metal_compute_gf2(metal_engine, 
                         KERNEL_TYPE_PREDICT_V4,
                         ctx.G_rows_packed, 
                         ctx.s_hard, 
                         ctx.p_pred, 
                         ctx.n_parity, 
                         ctx.words_per_row_G);

        // 收敛判定：检查 P 空间的预测
        if (count_bit_errors(ctx.p_pred, ctx.p_hard, ctx.n_parity) == 0) {
            if (out_s_bits) {
                for (uint32_t i = 0; i < ctx.n_info; ++i) {
                    out_s_bits[i] = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
                }
            }
            return true;
        }

        // 2. CPU (NEON) 更新 S 空间 LLR
        update_s(ctx, 3, ctx.power);

        // 3. GPU 计算预测方程：S_pred = F * P_hard
        // 修改：使用新接口，指定 KERNEL_TYPE_PREDICT_V4
        metal_compute_gf2(metal_engine, 
                         KERNEL_TYPE_PREDICT_V4,
                         ctx.F_rows_packed, 
                         ctx.p_hard, 
                         ctx.s_pred, 
                         ctx.n_info, 
                         ctx.words_per_col_G);

        // 收敛判定：检查 S 空间的预测
        if (count_bit_errors(ctx.s_pred, ctx.s_hard, ctx.n_info) == 0) {
            if (out_s_bits) {
                for (uint32_t i = 0; i < ctx.n_info; ++i) {
                    out_s_bits[i] = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
                }
            }
            return true;
        }

        // 4. CPU (NEON) 更新 P 空间 LLR
        update_p(ctx, 3, ctx.power);
    }

    // 达到最大迭代次数，执行最后裁决
    final_decision(ctx);
    if (out_s_bits) {
        for (uint32_t i = 0; i < ctx.n_info; ++i) {
            out_s_bits[i] = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
        }
    }

    return false;
}