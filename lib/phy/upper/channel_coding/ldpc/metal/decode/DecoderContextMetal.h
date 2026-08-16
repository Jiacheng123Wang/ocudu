#ifndef DECODER_CONTEXT_METAL_H
#define DECODER_CONTEXT_METAL_H

#include <cstdint>
#include <memory>
#include "../encode/MetalAlignedBuffer.hpp" 
#include "MetalLLSUpdater.h"

#ifdef __arm64__
typedef __fp16 float16;
#else
typedef uint16_t float16; 
#endif

struct VNStats {
    uint32_t ErrEqCnt;    // 报错的校验方程数
    uint32_t SuspectCnt;  // 怀疑度计数
    float EvidSum;        // 证据累加和
    float VNTotalCN;      // VN连接的CN总数 (或当前权值)
};

struct DecoderContext {
    uint32_t n_parity;       
    uint32_t N_logical;      
    uint32_t n_h_chunks;     
    uint32_t M_aligned;      
    float alpha;
    
    // 分别存储两个独立的 Metal 引擎句柄
    void* lls_engine;
    void* gf2_engine;

    std::unique_ptr<MetalAlignedBuffer<float16>> sp_llr_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> sp_hard_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> h_matrix_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> h_pred_buf;
    std::unique_ptr<MetalAlignedBuffer<VNStats>> debug_stats_buf;

    void init(void* l_engine, void* g_engine, uint32_t n_total, uint32_t m, float step_alpha) {
        lls_engine = l_engine;
        gf2_engine = g_engine;
        N_logical = n_total;
        n_parity = m;
        alpha = step_alpha;

        n_h_chunks = (N_logical + 31) / 32;
        M_aligned = (m + 31) / 32 * 32;

        // 内存分配只需借助任一引擎（或传 nullptr，因为目前是纯 posix 分配）
        sp_llr_buf = std::make_unique<MetalAlignedBuffer<float16>>(l_engine, n_h_chunks * 32);
        sp_hard_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(l_engine, n_h_chunks);
        h_matrix_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(l_engine, M_aligned * n_h_chunks);
        h_pred_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(l_engine, M_aligned / 32);
        debug_stats_buf = std::make_unique<MetalAlignedBuffer<VNStats>>(l_engine, N_logical);
    }

    void run_iteration() {
        // LLS 物理更新只使用 LLS engine
        engine_update_lls(lls_engine, alpha, sp_llr_buf->get(), h_matrix_buf->get(), 
                          h_pred_buf->get(), sp_hard_buf->get(), M_aligned, n_h_chunks, debug_stats_buf->get());
    }

    void release() {
        sp_llr_buf.reset();
        sp_hard_buf.reset();
        h_matrix_buf.reset();
        h_pred_buf.reset();
    }
};

#endif
