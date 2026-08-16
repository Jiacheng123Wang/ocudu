#include <metal_stdlib>
using namespace metal;

struct GF2MatrixVectorParams {
    uint32_t num_rows;          // 总行数 (必须是 128 的倍数)
    uint32_t num_cols_chunks;   // 矩阵每行含有的 uint4 数量 (128位块数)
};

kernel void gf2_multiply_uint4_extreme(
    device const uint4* matrix_in   [[ buffer(0) ]], 
    device const uint4* vector_in   [[ buffer(1) ]], 
    device uint4* vector_out        [[ buffer(2) ]], 
    constant GF2MatrixVectorParams& params [[ buffer(3) ]],
    uint tid [[ thread_index_in_threadgroup ]],       
    uint wid [[ threadgroup_position_in_grid ]])      
{
    uint base_row = (wid << 7) + tid; 
    
    uint row_offset0 = (base_row + 0 ) * params.num_cols_chunks;
    uint row_offset1 = (base_row + 32) * params.num_cols_chunks;
    uint row_offset2 = (base_row + 64) * params.num_cols_chunks;
    uint row_offset3 = (base_row + 96) * params.num_cols_chunks;

    uint4 xor_sum = uint4(0);

    for (uint j = 0; j < params.num_cols_chunks; j++) {
        uint4 v_in = vector_in[j];
        
        uint4 pc0 = popcount(matrix_in[row_offset0 + j] & v_in);
        uint4 pc1 = popcount(matrix_in[row_offset1 + j] & v_in);
        uint4 pc2 = popcount(matrix_in[row_offset2 + j] & v_in);
        uint4 pc3 = popcount(matrix_in[row_offset3 + j] & v_in);
        
        xor_sum.x ^= (pc0.x ^ pc0.y ^ pc0.z ^ pc0.w);
        xor_sum.y ^= (pc1.x ^ pc1.y ^ pc1.z ^ pc1.w);
        xor_sum.z ^= (pc2.x ^ pc2.y ^ pc2.z ^ pc2.w);
        xor_sum.w ^= (pc3.x ^ pc3.y ^ pc3.z ^ pc3.w);
    }
    bool4 out_bits = (xor_sum & 1) != 0;
    
    // --- 使用 simd_sum 手动组装比特位 ---
    // 每个线程根据自己的 tid (0-31) 将比特位移动到正确的位置
    // 然后通过 simd_sum 进行 SIMD 组内求和，所有线程都会得到相同的 32 位掩码结果
    uint32_t res_x = simd_sum(out_bits.x ? (1u << tid) : 0u);
    uint32_t res_y = simd_sum(out_bits.y ? (1u << tid) : 0u);
    uint32_t res_z = simd_sum(out_bits.z ? (1u << tid) : 0u);
    uint32_t res_w = simd_sum(out_bits.w ? (1u << tid) : 0u);

    if (tid == 0) {
        vector_out[wid] = uint4(res_x, res_y, res_z, res_w);
    }
}
