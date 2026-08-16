#include <metal_stdlib>
using namespace metal;

struct LDPCParams {
    uint32_t num_bits;       // 输出比特总数 (n_parity 或 n_info)
    uint32_t num_chunks;     // 每行对应的 uint32 数量
    uint32_t num_full_loops; // num_chunks / 32
    uint32_t remainder;      // num_chunks % 32
};

/**
 * 协作译码专用矩阵乘法 Kernel (极速 v4.0 架构)
 * 用于计算: 
 * 1. p_pred = G * s_hard (G 空间)
 * 2. s_pred = F * p_hard (F 空间)
 */
kernel void ldpc_predict_v4(
    device const uint32_t* matrix_rows [[ buffer(0) ]], // G_rows 或 F_rows (位包装)
    device const uint32_t* hard_in     [[ buffer(1) ]], // s_hard 或 p_hard (位包装)
    device uint8_t* pred_out           [[ buffer(2) ]], // p_pred 或 s_pred (字节展开)
    constant LDPCParams& params        [[ buffer(3) ]],
    uint tid [[ thread_index_in_threadgroup ]],          // 0 - 31
    uint gid [[ threadgroup_position_in_grid ]])         // 目标输出比特的索引
{
    // 每个 Threadgroup (32线程) 负责计算 1 个输出预测比特
    uint row = gid;
    if (row >= params.num_bits) return;

    // 定位矩阵行的起始位置
    uint row_offset = row * params.num_chunks;
    uint32_t partial_xor = 0;

    // --- 第一部分：全员冲刺 (Full Loops) ---
    // 32个线程并行读取，每个线程处理多个 uint32
    for (uint i = 0; i < params.num_full_loops; i++) {
        uint j = tid + (i << 5); // tid + i * 32
        // 计算 popcount(矩阵字 & 输入字) 的奇偶性
        partial_xor ^= popcount(matrix_rows[row_offset + j] & hard_in[j]);
    }

    // --- 第二部分：精准收尾 (Remainder) ---
    // 处理最后不足 32 个 uint32 的部分，无 if 逻辑
    if (tid < params.remainder) {
        uint j = tid + (params.num_full_loops << 5);
        partial_xor ^= popcount(matrix_rows[row_offset + j] & hard_in[j]);
    }

    // --- 第三部分：硬件级 SIMD 归约 ---
    // 在 SIMD Group 内横向异或所有线程的 partial_xor
    uint32_t final_xor = simd_xor(partial_xor);

    // 0号线程将 1-bit 结果写回对应的字节位置
    if (tid == 0) {
        pred_out[row] = (uint8_t)(final_xor & 1);
    }
}

/**
 * @brief 极致优化版 GF(2) 矩阵乘向量参数
 * 约束：num_rows 必须已在外部补齐为 32 的整数倍
 */
struct GF2MatrixVectorParams {
    uint32_t num_rows;        // 矩阵的总行数 (必须是 32 的倍数)
    uint32_t num_cols_chunks; // 矩阵每行含有的 uint32 数量
};

/**
 * @brief 无分支 (Branch-Free) GF(2) 矩阵乘向量算子
 * 策略：移除越界检查，保证 SIMD-group 内指令完全对齐
 */
kernel void gf2_matrix_vector_multiply(
    device const uint32_t* matrix_in [[ buffer(0) ]], 
    device const uint32_t* vector_in [[ buffer(1) ]], 
    device uint32_t* vector_out      [[ buffer(2) ]], 
    constant GF2MatrixVectorParams& params [[ buffer(3) ]],
    uint tid [[ thread_index_in_threadgroup ]],       
    uint wid [[ threadgroup_position_in_grid ]])      
{
    // 每个线程对应输出比特的一个索引，无需 if 检查
    // row_idx = wid * 32 + tid
    uint row_idx = (wid << 5) + tid;
    
    // 每一个线程计算一行的内积
    uint32_t my_xor_sum = 0;

    // 计算当前行在 matrix_in 中的起始偏移量
    uint row_offset = row_idx * params.num_cols_chunks;

    // 核心计算循环：所有线程执行相同的循环次数
    for (uint j = 0; j < params.num_cols_chunks; j++) {
        // popcount(A & B) & 1 是 GF(2) 向量内积的标准极速实现
        my_xor_sum ^= popcount(matrix_in[row_offset + j] & vector_in[j]);
    }

    // 将结果写回对应的位
    bool bit = (my_xor_sum & 1);
    uint32_t bit_val = bit ? (1U << tid) : 0;
    
    // 3. 使用 simd_sum 将 32 个线程的 bit_val 求和
    // 在 SIMD-group (32线程) 内，这等同于执行了一个按位或 (OR) 的合并操作
    uint32_t res_mask = simd_sum(bit_val);
    
    // 4. 由第一个线程将合并后的 32 位结果写入输出 Buffer
    if (tid == 0) {
        vector_out[wid] = res_mask;
    }
}