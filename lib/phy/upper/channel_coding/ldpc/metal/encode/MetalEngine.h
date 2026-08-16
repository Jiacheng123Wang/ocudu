#ifndef METAL_ENGINE_H
#define METAL_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 定义 Kernel 类型
typedef enum {
    KERNEL_TYPE_PREDICT_V4 = 0,   // 输出为字节数组 (uint8_t)，用于预测收敛
    KERNEL_TYPE_BIT_PACKED = 1    // 输出为位包装 (uint32_t)，用于纯矩阵运算
} MetalKernelType;

void* init_metal_engine(const char* shader_path);

/**
 * @brief 通用矩阵乘法接口
 * @param engine 引擎句柄
 * @param type 选择使用的 Kernel 类型
 * @param matrix 矩阵数据 (位包装)
 * @param vector_in 输入向量 (位包装)
 * @param vector_out 输出向量 (根据 type 可能是 uint8_t* 或 uint32_t*)
 * @param num_bits 输出的总比特数
 * @param num_chunks 每行含有的 uint32 数量
 */
void metal_compute_gf2(void* engine, 
                       MetalKernelType type,
                       const uint32_t* matrix, 
                       const uint32_t* vector_in, 
                       void* vector_out, 
                       uint32_t num_bits, 
                       uint32_t num_chunks);

void deinit_metal_engine(void* engine);

#ifdef __cplusplus
}
#endif

#endif