#ifndef LDPC_GPU_DECODER_H
#define LDPC_GPU_DECODER_H

#include <cstdint>

struct VNStats
{
    uint32_t ErrEqCnt;
    uint32_t SuspectCnt;
    float EvidSum;
    float VNTotalCN;
    float last_delta;  // 新增调试字段
    float current_llr; // 新增调试字段
};

class LDPCGPUDecoder
{
public:
    bool init(const char *metallib_path, uint32_t N_logical, uint32_t M_logical, float alpha);

    // 修正：column_weights 移除 const，因为我们要写入计算结果
    // void load_h_matrix(const uint32_t *h_matrix_raw, uint32_t *column_weights);
    // 新增：同时加载 H 矩阵及其转置矩阵 HT
    void load_h_and_ht_matrix(const uint32_t *h_matrix_raw,
                              const uint32_t *ht_matrix_raw,
                              uint32_t *column_weights);
    int decode(const void *in_sp_llr_fp16, uint8_t *out_s_bits, int max_iter);

    void release();
    // 2. 声明获取调试指针的成员函数
    VNStats *get_debug_ptr();

private:
    void *internal_state = nullptr;
};

#endif