#include <metal_stdlib>
using namespace metal;

/**
 * @brief LDPC 译码通用参数结构体
 */
struct DecodeParams {
    uint32_t num_target;      // 目标比特总数 (当前正在更新的变量节点数，如 n_info)
    uint32_t num_source;      // 对偶空间比特总数 (如 n_parity)
    
    // --- 目标(变量节点) 到 校验方程 的映射参数 (对应 b2c_bits / GT_bits) ---
    uint32_t b2c_num_chunks;  // 目标比特所在行的 uint32 数量
    uint32_t b2c_full_loops;  // 32线程全速循环次数
    uint32_t b2c_remainder;   // 剩余需要处理的 word 数量
    
    // --- 校验方程 到 目标(变量节点) 的映射参数 (对应 c2b_bits / G_bits) ---
    uint32_t c2b_num_chunks;  // 校验方程所在行的 uint32 数量
    uint32_t c2b_full_loops;  // 32线程全速循环次数
    uint32_t c2b_remainder;   // 剩余需要处理的 word 数量
    
    uint32_t top_k;           // 嫌疑人筛选阈值 (Rank < top_k 才采纳)
    float alpha;              // 步长因子 / 松弛因子
};

/**
 * @brief 极速版 LDPC 变量节点更新 Kernel (V16 终极版)
 * 核心优化：预过滤掩码扫描、SIMD 硬件规约、无锁/最小化原子更新
 */
kernel void ldpc_decode_update_v16(
    device float* target_llr               [[ buffer(0) ]], // 目标节点的 LLR 数组
    device const float* source_llr         [[ buffer(1) ]], // 源节点(校验方程对偶节点)的 LLR 数组
    device const uint32_t* eq_mask_buffer  [[ buffer(2) ]], // 预计算的冲突方程掩码 (1=冲突, 0=满足)
    device const uint32_t* c2b_bits        [[ buffer(3) ]], // 方程->变量节点的映射矩阵
    device const uint32_t* b2c_bits        [[ buffer(4) ]], // 变量节点->方程的映射矩阵
    device const uint32_t* target_deg      [[ buffer(5) ]], // 每个目标变量节点参与的方程总数
    device atomic_uint* target_hard        [[ buffer(6) ]], // 按位包装的目标硬判决结果 (需原子操作保护并发)
    constant DecodeParams& params          [[ buffer(7) ]],
    uint tid [[ thread_index_in_threadgroup ]],          // 组内线程索引 (0 - 31)
    uint gid [[ threadgroup_position_in_grid ]])         // 线程组全局索引 (即目标变量节点索引 my_target_idx)
{
    // 每个 Threadgroup (32人) 负责更新 1 个目标变量节点
    uint my_target_idx = gid;
    if (my_target_idx >= params.num_target) return;

    // 读取当前变量节点的 LLR 与 绝对值
    float current_llr = target_llr[my_target_idx];
    float my_abs_llr = abs(current_llr);
    
    // 证据累加器 (由 tid == 0 维护，避免重复累加)
    float local_evidence_sum = 0.0f;       // 收集到的最强指控强度总和
    uint  local_suspect_count = 0;         // 本节点被列为 Top-K 嫌疑人的次数
    uint  local_unsatisfied_count = 0;     // 本节点参与且当前处于“冲突”状态的方程总数

    // 定位目标节点在 b2c_bits (GT矩阵) 中的行起始索引
    uint b2c_row_start = my_target_idx * params.b2c_num_chunks;

    // =========================================================
    // 阶段 1：利用预过滤掩码 (Mask) 进行极速扫描
    // =========================================================
    for (uint i = 0; i < params.b2c_num_chunks; i++) {
        // b2c_word 表示当前变量节点连接了哪些方程 (1=连接，0=不连接)
        // 32个线程同步读取相同的 word，Apple M 系列芯片的 L1 缓存会自动广播
        uint32_t b2c_word = b2c_bits[b2c_row_start + i];
        
        // mask_word 表示全局所有方程的冲突状态 (1=冲突，0=满足)
        // 它的物理索引区间与 b2c_word 完全对齐！
        uint32_t mask_word = eq_mask_buffer[i];
        
        // 核心优化点：按位与！
        // 瞬间剥离掉所有“虽连接但已满足”的方程，剩下的 1 绝对是“连接且冲突”的方程
        uint32_t active_unsatisfied = b2c_word & mask_word;
        
        // 使用 ctz (Count Trailing Zeros) 极速跳过 0 位，直达目标
        while (active_unsatisfied > 0) {
            uint b = ctz(active_unsatisfied);
            uint eq_idx = (i << 5) + b; // 计算出冲突校验方程的全局真实索引
            
            // 0 号线程记录一次冲突 (代表全组记录，防止 32 个线程重复加 32 次)
            if (tid == 0) local_unsatisfied_count++;

            // =========================================================
            // 阶段 2：全组协作，计算 Global Rank 和 Min Evidence
            // =========================================================
            // 获取该方程本身的 LLR (即 p 比特的 LLR)
            float p_abs_llr = abs(source_llr[eq_idx]);
            
            // 初始化 Rank：Rank 仅在变量节点(s)之间比较，所以初始为 0
            uint  my_local_rank = 0;
            // 初始化强度：强度需包含校验节点(p)，因此所有人从 p_abs_llr 开始打擂台
            float my_local_min_abs = p_abs_llr;

            // 定位该方程在 c2b_bits (G矩阵) 中的行起始索引
            uint c2b_row_start = eq_idx * params.c2b_num_chunks;
            
            // 2.1 冲刺部分：32个线程平摊扫描该方程涉及的所有其他变量节点
            for (uint gi = 0; gi < params.c2b_full_loops; gi++) {
                uint gj = tid + (gi << 5); 
                uint32_t c2b_word_inner = c2b_bits[c2b_row_start + gj];
                
                while (c2b_word_inner > 0) {
                    uint gb = ctz(c2b_word_inner);
                    uint peer_idx = (gj << 5) + gb;
                    
                    // 逻辑校准：严格排除自己，自己不参与自己的 Rank 和强度计算
                    if (peer_idx != my_target_idx) {
                        float peer_abs = abs(target_llr[peer_idx]);
                        // Rank 计算：如果别人比我弱，我的嫌疑排名就退后 1 位
                        if (peer_abs < my_abs_llr) my_local_rank++;
                        // 强度计算：寻找包含 p 和所有 peer 在内的最小绝对值
                        if (peer_abs < my_local_min_abs) my_local_min_abs = peer_abs;
                    }
                    c2b_word_inner &= ~(1u << gb);
                }
            }
            
            // 2.2 收尾部分 (Remainder)：处理不能被 32 整除的剩余 uint32
            // 只有 tid 小于 remainder 的线程需要干活，其他线程直接跳过
            if (tid < params.c2b_remainder) {
                uint gj = tid + (params.c2b_full_loops << 5);
                uint32_t c2b_word_inner = c2b_bits[c2b_row_start + gj];
                
                while (c2b_word_inner > 0) {
                    uint gb = ctz(c2b_word_inner);
                    uint peer_idx = (gj << 5) + gb;
                    if (peer_idx != my_target_idx) {
                        float peer_abs = abs(target_llr[peer_idx]);
                        if (peer_abs < my_abs_llr) my_local_rank++;
                        if (peer_abs < my_local_min_abs) my_local_min_abs = peer_abs;
                    }
                    c2b_word_inner &= ~(1u << gb);
                }
            }

            // =========================================================
            // 阶段 3：SIMD 硬件规约
            // =========================================================
            // 瞬间将 32 个线程的 my_local_rank 累加，得到方程级的精确全局排名
            uint  global_rank = simd_sum(my_local_rank);
            // 瞬间在 32 个线程中找出最小的 my_local_min_abs，得到方程的最强证据
            float global_min_evidence = simd_min(my_local_min_abs);
            
            // 由 0 号线程汇总：如果我在该方程中排名前 Top-K，才算作有效指控
            if (tid == 0 && global_rank < params.top_k) {
                local_evidence_sum += global_min_evidence;
                local_suspect_count++;
            }

            // 清除已处理的冲突方程标志，继续寻找下一个 1
            active_unsatisfied &= ~(1u << b);
        }
    }

    // =========================================================
    // 阶段 4：计算 Delta，更新 LLR 与硬判决 (仅 0 号线程执行)
    // =========================================================
    if (tid == 0) {
        // my_target_idx 参与的总方程数
        uint total_involved = target_deg[my_target_idx];
        
        // 只有当存在有效指控，且确实参与了方程时，才进行更新
        if (total_involved > 0 && local_suspect_count > 0) {
            
            // 计算更新步长： 
            // 1. intensity: 冲突方程占比 (冲突数 / 总参与数)
            // 2. avg_evidence: 平均最强指控强度
            // 3. alpha: 松弛因子
            float intensity = (float)local_unsatisfied_count / (float)total_involved;
            float avg_evidence = local_evidence_sum / (float)local_suspect_count;
            float delta = intensity * avg_evidence * params.alpha;
            
            // 削弱原有置信度：因为收到的是反面指控，LLR 会向 0 靠拢甚至反转
            float new_llr = current_llr + (current_llr > 0 ? -delta : delta);
            
            // 内存写回 LLR
            target_llr[my_target_idx] = new_llr;
            
            // =========================================================
            // 并发保护：仅在符号发生硬翻转时，使用原子操作更新位包装结果
            // =========================================================
            if ((current_llr >= 0.0f) != (new_llr >= 0.0f)) {
                uint word_idx = my_target_idx >> 5;          // 计算位于哪个 uint32
                uint bit_mask = (1u << (my_target_idx & 31)); // 计算对应的比特掩码
                
                // LLR < 0 代表判决为 1，LLR >= 0 代表判决为 0
                if (new_llr < 0.0f) {
                    // 0 -> 1: 原子位或操作 (置 1)
                    atomic_fetch_or_explicit(&target_hard[word_idx], bit_mask, memory_order_relaxed);
                } else {
                    // 1 -> 0: 原子位与操作 (清 0，需对掩码取反)
                    atomic_fetch_and_explicit(&target_hard[word_idx], ~bit_mask, memory_order_relaxed);
                }
            }
        }
    }
}