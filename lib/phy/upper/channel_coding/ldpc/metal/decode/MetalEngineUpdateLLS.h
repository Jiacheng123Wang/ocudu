#ifndef METAL_ENGINE_UPDATE_LLS_H
#define METAL_ENGINE_UPDATE_LLS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
// Apple Silicon 上的 Clang 原生支持 __fp16
typedef __fp16 float16;
#else
typedef uint16_t float16; // C 环境下的回退方案
#endif

// 更新 LLS 的参数结构体，与 Metal 端严格对齐
struct LLRUpdateParams {
    uint32_t n_h_chunks;
    uint32_t n_h_full_loops;
    uint32_t n_h_remainder;
    
    uint32_t n_ht_chunks;
    uint32_t n_ht_full_loops;
    uint32_t n_ht_remainder;
    
    float eta;
    float power;

    uint32_t num_v_nodes; // 对应 N
    uint32_t num_h_nodes; // 对应 M
};

/**
 * @brief 初始化 LLR 更新引擎
 * @param shader_path .metal 文件的路径
 * @return 引擎句柄
 */
void* init_llr_update_engine(const char* shader_path);

/**
 * @param engine 引擎句柄
 * @param llr_array 输入输出 LLR 数组 (half 精度，对应 uint16_t)
 * @param h_matrix 校验矩阵 H (位包装)
 * @param ht_matrix 转置矩阵 HT (位包装)
 * @param h_pred 校验结果 (位包装)
 * @param v_degrees 变量节点度数数组
 * @param num_v_nodes 总比特数 N
 * @param max_v_degree 变量节点的最大度数 (用于分配线程组内存)
 * @param params 算法超参数
 */
void metal_update_llr(void* engine,
                      float16* llr_array,
                      const uint32_t* h_matrix,
                      const uint32_t* ht_matrix,
                      const uint32_t* h_pred,
                      const uint16_t* v_degrees,
                      uint32_t num_v_nodes,
                      uint32_t max_v_degree,
                      const LLRUpdateParams* params);

void deinit_llr_update_engine(void* engine);

#ifdef __cplusplus
}
#endif

#endif