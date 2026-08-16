#ifndef DECODER_CONTEXT_ULTIMATE_H
#define DECODER_CONTEXT_ULTIMATE_H

#include <arm_neon.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <memory>
#include "../encode/MetalAlignedBuffer.hpp" // 包含我们刚修改好的对齐内存管理器

/**
 * @brief 译码器上下文：深度整合 GPU(Metal) 与 CPU(NEON) 优化需求
 */
struct DecoderContext {
    // ==========================================
    // 1. 维度与元数据 (Metadata)
    // ==========================================
    uint32_t n_info;          // 原始信息位长度 (K)
    uint32_t n_parity;        // 原始校验位长度 (M)
    
    uint32_t padded_n_info;   
    uint32_t padded_n_parity; 

    uint32_t words_per_row_G; // (n_info + 31) / 32
    uint32_t words_per_col_G; // (n_parity + 31) / 32
    uint32_t words_per_row_F; // 通常与 words_per_col_G 相关

    // ==========================================
    // 2. 共享内存管理器 (零拷贝控制层)
    // ==========================================
    // 使用 unique_ptr 确保生命周期自动管理
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> g_rows_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> g_cols_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> f_rows_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> f_cols_buf;

    std::unique_ptr<MetalAlignedBuffer<uint32_t>> s_hard_buf;
    std::unique_ptr<MetalAlignedBuffer<uint32_t>> p_hard_buf;
    std::unique_ptr<MetalAlignedBuffer<uint8_t>>  s_pred_buf;
    std::unique_ptr<MetalAlignedBuffer<uint8_t>>  p_pred_buf;

    // ==========================================
    // 3. 原始结构指针 (保持原有逻辑兼容性)
    // ==========================================
    // 矩阵指针 (指向上述 buf 的 get() 结果)
    uint32_t* G_rows_packed; 
    uint32_t* G_cols_packed; 
    uint32_t* F_rows_packed;
    uint32_t* F_cols_packed;

    // 硬判决与预测 (指向上述 buf 的 get() 结果)
    uint32_t* s_hard;          
    uint32_t* p_hard;          
    uint8_t* p_pred;           
    uint8_t* s_pred;           

    // --- CPU 侧专属变量 (保持原有分配方式) ---
    uint32_t* degree_s;
    uint32_t* degree_p;
    float* s_llr;              
    float* p_llr;              
    const float* s_channel;    
    const float* p_channel;    

    float* s_evidence_sum;   
    uint16_t* s_evidence_cnt; 
    float* p_evidence_sum;   
    uint16_t* p_evidence_cnt; 

    uint32_t* row_suspect_ids; 
    float* row_suspect_abs;

    // ==========================================
    // 4. 算法超参数与同步变量 (保持原样)
    // ==========================================
    float eta;                 
    float threshold;           
    float s_bias;              
    float power;               

    std::atomic<bool> is_converged{false}; 
    std::atomic<bool> stop_flag{false};    
    
    std::mutex mtx;
    std::condition_variable cv;

private:
    // 辅助对齐计算
    size_t align16(size_t sz) { return (sz + 15) & ~15; }

public:
    /**
     * @brief 初始化上下文并执行零拷贝分配
     * @param engine Metal 引擎句柄，用于将来注册 Buffer 到 Metal 资源池
     */
    void init(void* engine, uint32_t n, uint32_t m, float e, float b, float p, float thr) {
        n_info = n; n_parity = m;
        eta = e; s_bias = b; power = p; threshold = thr;

        padded_n_info = (n_info + 3) & ~3;
        padded_n_parity = (n_parity + 3) & ~3;
        words_per_row_G = (n_info + 31) / 32;
        words_per_col_G = (n_parity + 31) / 32;
        words_per_row_F = words_per_col_G;

        // --- 1. 分配 Metal 零拷贝共享 Buffer ---
        g_rows_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(engine, n_parity * words_per_row_G);
        g_cols_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(engine, n_info * words_per_col_G);
        f_rows_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(engine, n_info * words_per_col_G);
        f_cols_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(engine, n_parity * words_per_row_G);

        s_hard_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(engine, words_per_row_G);
        p_hard_buf = std::make_unique<MetalAlignedBuffer<uint32_t>>(engine, words_per_col_G);
        s_pred_buf = std::make_unique<MetalAlignedBuffer<uint8_t>>(engine, padded_n_info);
        p_pred_buf = std::make_unique<MetalAlignedBuffer<uint8_t>>(engine, padded_n_parity);

        // --- 2. 映射兼容性指针 ---
        G_rows_packed = g_rows_buf->get();
        G_cols_packed = g_cols_buf->get();
        F_rows_packed = f_rows_buf->get();
        F_cols_packed = f_cols_buf->get();
        s_hard = s_hard_buf->get();
        p_hard = p_hard_buf->get();
        s_pred = s_pred_buf->get();
        p_pred = p_pred_buf->get();

        // --- 3. 分配 CPU 侧 NEON 对齐内存 (16字节对齐) ---
        s_llr = (float*)std::aligned_alloc(16, align16(padded_n_info * sizeof(float)));
        p_llr = (float*)std::aligned_alloc(16, align16(padded_n_parity * sizeof(float)));
        degree_s = (uint32_t*)std::aligned_alloc(16, align16(padded_n_info * sizeof(uint32_t)));
        degree_p = (uint32_t*)std::aligned_alloc(16, align16(padded_n_parity * sizeof(uint32_t)));
        s_evidence_sum = (float*)std::aligned_alloc(16, align16(padded_n_info * sizeof(float)));
        s_evidence_cnt = (uint16_t*)std::aligned_alloc(16, align16(padded_n_info * sizeof(uint16_t)));
        p_evidence_sum = (float*)std::aligned_alloc(16, align16(padded_n_parity * sizeof(float)));
        p_evidence_cnt = (uint16_t*)std::aligned_alloc(16, align16(padded_n_parity * sizeof(uint16_t)));
        
        uint32_t max_dim = std::max(padded_n_info, padded_n_parity);
        row_suspect_ids = (uint32_t*)std::aligned_alloc(16, align16(max_dim * sizeof(uint32_t)));
        row_suspect_abs = (float*)std::aligned_alloc(16, align16(max_dim * sizeof(float)));

        // 初始化清零
        std::memset(s_llr, 0, align16(padded_n_info * sizeof(float)));
        std::memset(p_llr, 0, align16(padded_n_parity * sizeof(float)));
    }

    /**
     * @brief 释放 CPU 侧分配的资源
     */
    void release() {
        if (s_llr) std::free(s_llr);
        if (p_llr) std::free(p_llr);
        if (degree_s) std::free(degree_s);
        if (degree_p) std::free(degree_p);
        if (s_evidence_sum) std::free(s_evidence_sum);
        if (s_evidence_cnt) std::free(s_evidence_cnt);
        if (p_evidence_sum) std::free(p_evidence_sum);
        if (p_evidence_cnt) std::free(p_evidence_cnt);
        if (row_suspect_ids) std::free(row_suspect_ids);
        if (row_suspect_abs) std::free(row_suspect_abs);
        
        // unique_ptr 类型的 buf 会自动释放
        s_llr = nullptr; p_llr = nullptr;
    }

    ~DecoderContext() { release(); }
};

#endif