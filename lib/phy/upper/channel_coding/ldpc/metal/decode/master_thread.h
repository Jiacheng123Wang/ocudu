#ifndef MASTER_THREAD_H
#define MASTER_THREAD_H

#include <atomic>              // 保证 std::atomic 可用
#include <mutex>               // 修复 error: no type named 'mutex'
#include <condition_variable>  // 修复 error: no type named 'condition_variable'
#include <cstdint>             // 保证 uint32_t 等类型可用
#include "../encode/MetalEngine.h" // 引入 Metal 引擎

struct DecoderContext {
    // --- GPU 友好型：行存储 (Bit-packed) ---
    // 用于 GPU 并行计算 G*s 和 F*p
    const uint32_t* G_rows_packed; // 每行 cols/32 个 uint32
    const uint32_t* F_rows_packed;

    // --- CPU 友好型：转置存储 (Bit-packed) ---
    // 也就是列存储，用于 CPU 快速查找关联节点
    // CPU 读取 G_cols_packed[i] 就能瞬间拿到比特 i 连接的所有校验位信息
    const uint32_t* G_cols_packed; 
    const uint32_t* F_cols_packed;

    // 矩阵维度
    uint32_t n_info;
    uint32_t n_parity;
    uint32_t words_per_row_G; // (n_info + 31) / 32
    uint32_t words_per_col_G; // (n_parity + 31) / 32
    
    // --- 第二部分: S 空间动态数据 (S-Space) ---
    float* s_llr;              // S 的软信息，CPU 频繁读写
    uint32_t* s_hard;           // S 的硬判决 (0/1)，CPU 写，GPU 读 // 存储位包装后的 S 硬判决，大小为 words_per_row_G * 4 字节
    const float* s_channel;    // 天线收到的原始 S LLR (作为参考背景)

    // --- 第三部分: P 空间动态数据 (P-Space) ---
    float* p_llr;              // P 的软信息，CPU 频繁读写
    uint32_t* p_hard;           // P 的硬判决 (0/1)，CPU 写，GPU 读 // 存储位包装后的 P 硬判决，大小为 words_per_col_G * 4 字节
    const float* p_channel;    // 天线收到的原始 P LLR

    // --- 第四部分: GPU 预测输出 (画布 - Canvas) ---
    // 由 GPU 持续写入, CPU 持续读取进行逻辑判定
    uint8_t* p_pred;           // GPU 计算 G * s_hard 的结果
    uint8_t* s_pred;           // GPU 计算 F * p_hard 的结果

    // --- 第五部分：同步与控制中心 ---
    std::atomic<bool> is_converged{false}; // 全局收敛标志
    std::atomic<bool> stop_flag{false};    // 超时/强制终止
    
    // 用于 Master 和 Slaves 之间的通知机制
    std::mutex mtx;
    std::condition_variable cv;

    // 算法超参数
    float eta;
    float threshold;
    float s_bias;
};

bool LDPC_Decode_Master(int Z, const float* input_s_llr, const float* input_p_llr, int max_wait_us, uint8_t* out_s_bits, void* metal_engine);
bool LDPC_Decode_Master_Sequential(int Z, const float* input_s_llr, const float* input_p_llr, uint8_t* out_s_bits, void* metal_engine);

#endif