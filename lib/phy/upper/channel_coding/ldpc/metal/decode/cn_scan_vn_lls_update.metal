#include <metal_stdlib>
using namespace metal;

// ==============================================================================
// --- DEBUGGING EXTENSIONS (Added per user request) ---
// Define the struct within the Metal Shading Language context so it matches
// the host (CPU) side definition for memory layout alignment.
// ==============================================================================
struct VNStats {
    uint32_t ErrEqCnt;    // Number of conflicting parity equations involved in
    uint32_t SuspectCnt;  // Count of times deemed a "suspect" (weakest LLR)
    float EvidSum;        // Sum of evidence LLR strengths received
    float VNTotalCN;      // Total number of Check Nodes connected to this VN
};
// ==============================================================================


// --- 辅助函数：CAS 实现的浮点原子加法 ---
void atomic_float_add(device atomic_uint* addr, float delta) {
    uint old_val, new_val;
    do {
        old_val = atomic_load_explicit(addr, memory_order_relaxed);
        // as_type 执行位模式重解释，将 uint 内存直接当作 float 计算
        float total = as_type<float>(old_val) + delta;
        new_val = as_type<uint>(total);
    } while (!atomic_compare_exchange_weak_explicit(addr, &old_val, new_val, 
                                                     memory_order_relaxed, 
                                                     memory_order_relaxed));
}

// --- Kernel 1: 校验节点扫描 (CN-Centric) ---
kernel void cn_centric_scan(
    device const uint32_t* h_matrix     [[ buffer(0) ]],
    device const half* llr_array        [[ buffer(1) ]],
    device const uint32_t* h_pred       [[ buffer(2) ]],
    device atomic_uint* err_eq_cnt      [[ buffer(3) ]],
    device atomic_uint* suspect_cnt     [[ buffer(4) ]],
    device atomic_uint* evidence_sum    [[ buffer(5) ]],
    constant uint32_t& n_h_chunks       [[ buffer(6) ]], // 仅传入分块数
    device VNStats* debug_out           [[ buffer(7) ]], // 新增调试输出通道
    uint tid [[ thread_index_in_threadgroup ]],
    uint wid [[ threadgroup_position_in_grid ]]) 
{
    // uint32_t n_h_chunks = *n_h_chunks_ptr;

    // 如果该方程满足校验（未报错），直接跳过
    uint32_t word_idx = wid / 32;
    uint32_t bit_idx = wid % 32;
    if (!((h_pred[word_idx] >> bit_idx) & 1)) return;

    float m1 = 65504.0f;
    float m2 = 65504.0f;
    uint  i1 = 0; 
    uint  i2 = 0;

    const uint row_base = wid * n_h_chunks;

    // 扫描行内非零元素
    for (uint i = tid; i < n_h_chunks; i += 32) {
        uint32_t mask = h_matrix[row_base + i];
        while (mask != 0) {
            uint bit = ctz(mask); 
            uint vn_idx = i * 32 + bit;
            
            // 普惠指控计数
            atomic_fetch_add_explicit(&err_eq_cnt[vn_idx], 1, memory_order_relaxed);
            
            // 局部 Top-2 比较
            float val = abs((float)llr_array[vn_idx]);
            if (val < m1) {
                m2 = m1; i2 = i1;
                m1 = val; i1 = vn_idx;
            } else if (val < m2) {
                m2 = val; i2 = vn_idx;
            }
            mask &= (mask - 1); 
        }
    }

    for (uint offset = 16; offset > 0; offset /= 2) {
        float om1 = simd_shuffle_xor(m1, offset);
        uint  oi1 = simd_shuffle_xor(i1, offset);
        float om2 = simd_shuffle_xor(m2, offset);
        uint  oi2 = simd_shuffle_xor(i2, offset);

        if (om1 < m1) {
            m2 = min(m1, om2);
            i2 = (m2 == m1) ? i1 : oi2;
            m1 = om1; i1 = oi1;
        } else {
            m2 = min(m2, om1);
            i2 = (m2 == om1) ? oi1 : i2;
        }
    }
    // 最终原子写入：跨组竞争由 CAS 解决
    if (tid == 0) {
        atomic_fetch_add_explicit(&suspect_cnt[i1], 1, memory_order_relaxed);
        atomic_float_add(evidence_sum + i1, m2); 
        
        atomic_fetch_add_explicit(&suspect_cnt[i2], 1, memory_order_relaxed);
        atomic_float_add(evidence_sum + i2, m1);

        // ==============================================================================
        // --- DEBUG IMPLEMENTATION (Added per user request) ---
        // Note: Kernel 1 updates global atomic counters (buffers 3, 4, 5).
        // To debug the *accumulation* logic, we snapshot the state of these global
        // counters *immediately after* this threadgroup contributes its update.
        // Because other threadgroups run concurrently, this snapshot captures the
        // cumulative state up to this point in execution time for VNs i1 and i2.
        // Host-side code (CPU) must ignore entries for VNs that were not determined
        // suspects in this specific threadgroup context.
        // ==============================================================================
        if (debug_out) {
            // Snapshot stats for Rank 1 suspect (i1)
            debug_out[i1].ErrEqCnt   = atomic_load_explicit(&err_eq_cnt[i1], memory_order_relaxed);
            debug_out[i1].SuspectCnt = atomic_load_explicit(&suspect_cnt[i1], memory_order_relaxed);
            debug_out[i1].EvidSum    = as_type<float>(atomic_load_explicit(&evidence_sum[i1], memory_order_relaxed));
            // Note: VNTotalCN is usually pre-calculated column weight; it is not available 
            // in this kernel's parameter scope. It must be read from buffer(4) in Kernel 2 
            // or passed to Kernel 1 if debugging here is critical. Setting to 0 for safety.
            debug_out[i1].VNTotalCN  = 0.0f; 

            // Snapshot stats for Rank 2 suspect (i2)
            debug_out[i2].ErrEqCnt   = atomic_load_explicit(&err_eq_cnt[i2], memory_order_relaxed);
            debug_out[i2].SuspectCnt = atomic_load_explicit(&suspect_cnt[i2], memory_order_relaxed);
            debug_out[i2].EvidSum    = as_type<float>(atomic_load_explicit(&evidence_sum[i2], memory_order_relaxed));
            debug_out[i2].VNTotalCN  = 0.0f;
        }
        // ==============================================================================
    }
}
// --- Kernel 2: 更新 LLR 与硬判决 (VN-Centric) ---
kernel void vn_centric_update(
    device half* llr_array                 [[ buffer(0) ]],
    device const atomic_uint* err_eq_cnt   [[ buffer(1) ]],
    device const atomic_uint* suspect_cnt  [[ buffer(2) ]],
    device const atomic_uint* evidence_sum [[ buffer(3) ]],
    device const uint32_t* vn_total_cn     [[ buffer(4) ]],
    device atomic_uint* s_hard             [[ buffer(5) ]], 
    constant float& alpha                  [[ buffer(6) ]], 
    uint vn_idx                            [[ thread_position_in_grid ]],
    uint lane_id                           [[ thread_index_in_simdgroup ]]) 
{
    bool sign_flipped = false;
    uint s_cnt = atomic_load_explicit(&suspect_cnt[vn_idx], memory_order_relaxed);
    
    if (s_cnt > 0) {
        float old_llr = (float)llr_array[vn_idx];
        uint e_cnt = atomic_load_explicit(&err_eq_cnt[vn_idx], memory_order_relaxed);
        float e_sum = as_type<float>(atomic_load_explicit(&evidence_sum[vn_idx], memory_order_relaxed));
        uint total_cnt = vn_total_cn[vn_idx];
        
        // 核心公式：Delta = alpha * (err/total)^2 * (evidence_avg)
        float ratio = (float)e_cnt / (float)total_cnt;
        float delta = alpha * (ratio * ratio) * (e_sum / (float)s_cnt);
        
        float new_llr = old_llr - sign(old_llr) * delta; 
        llr_array[vn_idx] = (half)new_llr;
        
        if (signbit(old_llr) != signbit(new_llr)) sign_flipped = true;
    }

    // 1. 每个线程将自己的翻转状态映射到对应的位偏移上 (0-31位)
    uint32_t bit_val = sign_flipped ? (1u << lane_id) : 0;
    
    // 2. 使用 simd_sum 在 SIMD-group (32线程) 内横向求和
    // 由于每个线程贡献的位是不重叠的，这里的求和等同于按位或 (Bitwise OR)
    uint32_t flip_mask = simd_sum(bit_val);
    
    // 2. 解决逻辑错误：使用原子异或操作，确保在 SIMD 组内仅由一个线程写入，且不覆盖相邻位
    // 假设 vn_idx 是连续分配的，lane_id == 0 负责写入当前 32-bit 字的翻转状态
    if (lane_id == 0 && flip_mask != 0) {
        atomic_fetch_xor_explicit(&s_hard[vn_idx / 32], flip_mask, memory_order_relaxed);
    }
}
