#ifndef METAL_ENGINE_UINT4_H
#define METAL_ENGINE_UINT4_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 uint4 向量化 Metal 引擎
 * @param shader_path .metal 文件的绝对路径
 * @return 返回引擎实例指针，失败返回 nullptr
 */
void* init_metal_engine_uint4(const char* shader_path);

/**
 * @brief 极致优化版 128-bit GF(2) 矩阵乘向量计算
 * * [硬性约束说明]:
 * 1. num_rows: 必须补齐为 128 的整数倍 (例如 5632 -> 5632, 5700 -> 5760)。
 * 2. words_per_row: 每一行包含的 uint32 数量，必须是 4 的倍数 (即 16 字节/128位对齐)。
 * 3. 对齐: 传入的三个指针必须指向 16 字节对齐的内存区域。
 * * @param engine 引擎实例指针
 * @param matrix_packed 矩阵数据指针 (uint4 兼容)
 * @param vector_in_packed 输入向量指针 (uint4 兼容)
 * @param vector_out_packed 输出向量指针 (每 128 行结果占据一个 uint4 空间)
 * @param num_rows 补齐后的总行数
 * @param words_per_row 每行补齐后的 uint32 数量
 */
void metal_compute_gf2_uint4(void* engine, 
                             const uint32_t* matrix_packed, 
                             const uint32_t* vector_in_packed, 
                             uint32_t* vector_out_packed, 
                             uint32_t num_rows, 
                             uint32_t words_per_row);

/**
 * @brief 销毁引擎并释放缓存的 Buffer
 * @param engine 引擎实例指针
 */
void destroy_metal_engine_uint4(void* engine);

#ifdef __cplusplus
}
#endif

#endif // METAL_ENGINE_UINT4_H