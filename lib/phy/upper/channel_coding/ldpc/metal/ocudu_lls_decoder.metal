// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// LLS (likelihood-erosion bit-flipping) LDPC decoder, The LLS
// algorithm is ~5-8 dB weaker than the layered NMS decoder on BLER; its
// advantage is extreme parallelism (2 dispatches per round, no message
// passing). Re-compile the .metallib after editing:
//   xcrun -sdk macosx metal -c ocudu_lls_decoder.metal -o ocudu_lls_decoder.air
//   xcrun -sdk macosx metallib ocudu_lls_decoder.air -o ocudu_lls_decoder.metallib

#include <metal_stdlib>
using namespace metal;

// ==========================================================
// 1. 全局控制与结构体定义
// ==========================================================
struct DecodeCtrl
{
    atomic_uint error_count;     // 当前迭代中报错的校验方程数
    atomic_uint early_terminate; // 提前终止标志 (1 = 停止, 0 = 继续)
    atomic_uint actual_iters;    // 实际执行的迭代次数
    atomic_uint prev_error_count; // 上一轮的 error_count (停滞检测)
    atomic_uint stall_counter;    // 连续停滞轮数 (error_count == prev 的连击)
    atomic_uint stall_flag;       // 1 = 下一轮 update 执行硬多翻逃逸 (PLAN.md 4.15 C4)
};

struct VNStats
{
    uint32_t ErrEqCnt;
    uint32_t SuspectCnt;
    float EvidSum;
    float VNTotalCN;
    float last_delta;
    float current_llr;
};

// LLS tuning parameters (PLAN.md 4.15). The layout matches
// decoder_engine::lls_params on the host exactly (one setBytes per dispatch).
struct LLSParams
{
    float alpha;       // erosion step size
    float beta;        // self-prior damping: delta -= beta * |old_llr|
    float p;           // unsatisfied-ratio exponent (1 or 2)
    float gamma;       // post-flip magnitude: 1 = legacy overshoot (delta - |old|), 0 = reset to the evidence
    float eps;         // post-update magnitude floor (0 = legacy; 1 fixes the round-level sign(0) trap)
    uint32_t k_suspects; // suspects per unsatisfied row (2 = legacy; 3/4 = Phase 2)
    uint32_t norm_mode;  // evidence normalization: 0 = /s_cnt (legacy), 1 = /e_cnt, 2 = /tc ("total erosion")
    uint32_t evidence_mode; // suspect evidence assignment: 0 = E-self (legacy k=2: {m2,m1}), 1 = E-peel ({m_{j+1}}), 2 = E-uniform ({m_{k+1}})
    uint32_t cooldown;     // 1 = the 1-round flip-immunity oscillation guard (Phase 2)
    uint32_t stall_escape; // 1 = the stall-escape hard multi-flip (Phase 3 C4)
    float theta;           // hard-flip threshold: VNs with e_cnt >= theta * column_weight flip
    uint32_t stall_rounds; // consecutive stall rounds before the escape fires
};

// Inserts (v, idx) into the ascending list m/iv of length L, keeping the L smallest.
inline void insert_sorted(thread float* m, thread uint* iv, uint L, float v, uint idx)
{
    uint pos = L;
    for (uint t = 0; t < L; ++t)
    {
        if (v < m[t])
        {
            pos = t;
            break;
        }
    }
    if (pos == L)
        return;
    for (uint t = L - 1; t > pos; --t)
    {
        m[t] = m[t - 1];
        iv[t] = iv[t - 1];
    }
    m[pos] = v;
    iv[pos] = idx;
}

// 辅助函数：浮点数原子加法
void atomic_float_add(device atomic_uint *addr, float delta)
{
    uint old_val, new_val;
    do
    {
        old_val = atomic_load_explicit(addr, memory_order_relaxed);
        float total = as_type<float>(old_val) + delta;
        new_val = as_type<uint>(total);
    } while (!atomic_compare_exchange_weak_explicit(addr, &old_val, new_val,
                                                    memory_order_relaxed, memory_order_relaxed));
}

// ==========================================================
// Kernel 0: 预处理与初始化 (执行 1 次)
// ==========================================================
kernel void init_hard_decisions(
    device const half *llr_array [[buffer(0)]],
    device uint32_t *s_hard [[buffer(1)]],
    device DecodeCtrl *ctrl [[buffer(2)]],
    device atomic_uint *err_eq_cnt [[buffer(3)]],
    device atomic_uint *suspect_cnt [[buffer(4)]],
    device atomic_uint *evidence_sum [[buffer(5)]],
    device atomic_uint *cool_flag [[buffer(6)]],
    uint vn_idx [[thread_position_in_grid]],
    uint lane_id [[thread_index_in_simdgroup]])
{
    if (vn_idx == 0)
    {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->prev_error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->stall_counter, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->stall_flag, 0, memory_order_relaxed);
    }

    atomic_store_explicit(&err_eq_cnt[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&suspect_cnt[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&evidence_sum[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&cool_flag[vn_idx], 0, memory_order_relaxed);

    bool is_negative = (float)llr_array[vn_idx] < 0.0f;
    uint32_t bit_val = is_negative ? (1u << lane_id) : 0;
    uint32_t packed = simd_sum(bit_val);

    if (lane_id == 0)
    {
        s_hard[vn_idx / 32] = packed;
    }
}

// ==========================================================
// Kernel 2: 首轮全量 Syndrome 计算 (仅执行 1 次)
// ==========================================================
kernel void compute_syndrome(
    device const uint32_t *h_matrix [[buffer(0)]],
    device const uint32_t *s_hard [[buffer(1)]],
    device uint32_t *h_pred [[buffer(2)]],
    device DecodeCtrl *ctrl [[buffer(3)]],
    constant uint32_t &n_h_chunks [[buffer(4)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
    uint row_idx = (wid << 5) + tid;
    uint32_t my_xor_sum = 0;
    uint row_offset = row_idx * n_h_chunks;

    for (uint j = 0; j < n_h_chunks; j++)
    {
        my_xor_sum ^= popcount(h_matrix[row_offset + j] & s_hard[j]);
    }

    bool bit = (my_xor_sum & 1);
    uint32_t bit_val = bit ? (1U << tid) : 0;
    uint32_t res_mask = simd_sum(bit_val);

    if (tid == 0)
    {
        // 仅写入 h_pred 状态，统计工作统一交给 cn_centric_scan 处理
        h_pred[wid] = res_mask;
    }
}

// ==========================================================
// Kernel 3: CN 扫描与 Syndrome 权重统计 (合并优化版)
// ==========================================================
kernel void cn_centric_scan(
    device const uint32_t *h_matrix [[buffer(0)]],
    device const half *llr_array [[buffer(1)]],
    device const uint32_t *h_pred [[buffer(2)]],
    device atomic_uint *err_eq_cnt [[buffer(3)]],
    device atomic_uint *suspect_cnt [[buffer(4)]],
    device atomic_uint *evidence_sum [[buffer(5)]],
    constant uint32_t &n_h_chunks [[buffer(6)]],
    device DecodeCtrl *ctrl [[buffer(7)]],
    constant LLSParams &params [[buffer(8)]],
    device atomic_uint *cool_flag [[buffer(9)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]],     // 对应校验方程索引 (0 ~ M-1)
    uint lane_id [[thread_index_in_simdgroup]])    // 【新增】组内索引
{
    // 1. 检查全局终止标志
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed))
        return;

    uint32_t word_idx = wid / 32;
    uint32_t bit_idx = wid % 32;

    // ==========================================
    // 【核心优化】：极其精密的分布式 popcount 统计
    // 保证每个 h_pred 的 Word (包含 32 个方程状态) 只被统计一次
    // 触发条件：当前行是 32 的倍数 (bit_idx == 0)，且由 0 号线程执行
    // ==========================================
    if (lane_id == 0 && bit_idx == 0) 
    {
        uint32_t current_h_word = h_pred[word_idx];
        if (current_h_word != 0) 
        {
            atomic_fetch_add_explicit(&ctrl->error_count, popcount(current_h_word), memory_order_relaxed);
        }
    }

    // 2. 原有的扫描跳过逻辑
    if (!((h_pred[word_idx] >> bit_idx) & 1))
        return;

    // K = k_suspects + 1: the k suspects plus one evidence anchor (PLAN.md 4.15
    // Phase 2). The anchor is the weakest remaining VN of the row.
    const uint K = params.k_suspects + 1u;
    float m[5];
    uint iv[5];
    for (uint t = 0; t < 5; ++t)
    {
        m[t] = 65504.0f;
        iv[t] = 0;
    }

    const uint row_base = wid * n_h_chunks;

    // 3. 寻找嫌疑人: k-min 插入排序 (比旧的 2-min 分支跟踪更通用)
    for (uint i = tid; i < n_h_chunks; i += 32)
    {
        uint32_t mask = h_matrix[row_base + i];
        while (mask != 0)
        {
            uint bit = ctz(mask);
            uint vn_idx = i * 32 + bit;

            atomic_fetch_add_explicit(&err_eq_cnt[vn_idx], 1, memory_order_relaxed);

            insert_sorted(m, iv, K, abs((float)llr_array[vn_idx]), vn_idx);
            mask &= (mask - 1);
        }
    }

    // 4. SIMD 组内归约: 邻道列表插入合并
    for (uint offset = 16; offset > 0; offset /= 2)
    {
        for (uint t = 0; t < K; ++t)
        {
            float om = simd_shuffle_xor(m[t], offset);
            uint oi = simd_shuffle_xor(iv[t], offset);
            insert_sorted(m, iv, K, om, oi);
        }
    }

    // 5. 投票: k 个嫌疑人, 证据分配由 params.evidence_mode 决定
    if (tid == 0)
    {
        const uint k = params.k_suspects;
        for (uint j = 0; j < k; ++j)
        {
            if (m[j] >= 65504.0f)
                break;
            const uint vn = iv[j];
            // 振荡护栏: 刚翻转的 VN 跳过一轮投票 (err_eq_cnt 仍计数, 见扫描循环)
            if (params.cooldown != 0u && atomic_load_explicit(&cool_flag[vn], memory_order_relaxed) != 0u)
                continue;
            float ev;
            if (params.evidence_mode == 0u)
            {
                // E-self (generalized): the prime suspect gets m2; every further
                // suspect gets m1 - the row's weakest magnitude, a naturally
                // conservative vote (k=2 reproduces the legacy behavior exactly).
                ev = (j == 0u) ? m[1] : m[0];
            }
            else if (params.evidence_mode == 2u)
            {
                // E-uniform: every suspect gets the anchor m_{k+1}.
                ev = (m[k] < 65504.0f) ? m[k] : m[k - 1];
            }
            else if (params.evidence_mode == 3u)
            {
                // E-peel with rank decay: m_{j+1} * 2^-j (the aggressive E-peel
                // collapsed at k>=3, so the weaker suspects get discounted votes).
                const float damp = (j == 0u) ? 1.0f : (j == 1u ? 0.5f : 0.25f);
                ev = ((m[j + 1] < 65504.0f) ? m[j + 1] : m[j]) * damp;
            }
            else
            {
                // E-peel: suspect j gets m_{j+1} (the strongest remaining support
                // once the j weaker suspects are flipped).
                ev = (m[j + 1] < 65504.0f) ? m[j + 1] : m[j];
            }
            atomic_fetch_add_explicit(&suspect_cnt[vn], 1, memory_order_relaxed);
            atomic_float_add(evidence_sum + vn, ev);
        }
    }
}

// ==========================================================
// Kernel 4: VN 更新与 Syndrome 增量维护
// ==========================================================
kernel void update_llr_hpred(
    device half *llr_array [[buffer(0)]],
    device atomic_uint *err_eq_cnt [[buffer(1)]],
    device atomic_uint *suspect_cnt [[buffer(2)]],
    device atomic_uint *evidence_sum [[buffer(3)]],
    device const uint *vn_total_cn [[buffer(4)]],
    device atomic_uint *h_pred [[buffer(5)]],
    device const uint32_t *ht_matrix [[buffer(6)]],
    constant uint32_t &n_ht_chunks [[buffer(7)]],
    constant LLSParams &params [[buffer(8)]],
    device DecodeCtrl *ctrl [[buffer(9)]],
    device VNStats *debug_out [[buffer(10)]],
    device atomic_uint *cool_flag [[buffer(11)]],
    uint vn_idx [[thread_position_in_grid]],
    uint lane_id [[thread_index_in_simdgroup]])
{
    // ==========================================
    // 【判定提前终止】：接管原本在 scan 里的裁判权
    // 因为 ctrl->error_count 刚刚在 scan 阶段统计完毕
    // ==========================================
    if (atomic_load_explicit(&ctrl->error_count, memory_order_relaxed) == 0)
    {
        if (vn_idx == 0)
        {
            atomic_store_explicit(&ctrl->early_terminate, 1, memory_order_relaxed);
        }
        return;
    }

    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed))
        return;

    // 1. 加载当前 VN 的统计状态
    uint s_cnt = atomic_load_explicit(&suspect_cnt[vn_idx], memory_order_relaxed);
    uint e_cnt = atomic_load_explicit(&err_eq_cnt[vn_idx], memory_order_relaxed);
    float e_sum = as_type<float>(atomic_load_explicit(&evidence_sum[vn_idx], memory_order_relaxed));
    uint total_cnt = vn_total_cn[vn_idx];
    float old_llr = (float)llr_array[vn_idx];

    device VNStats &stats = debug_out[vn_idx];
    stats.VNTotalCN = (float)total_cnt;
    stats.ErrEqCnt = e_cnt;
    stats.SuspectCnt = s_cnt;
    stats.EvidSum = e_sum;
    stats.current_llr = old_llr;
    stats.last_delta = 0.0f;

    bool flipped = false;

    // C4 停滞逃逸 (PLAN.md 4.15): 上一轮 vn0 判定 syndrome 连续停滞, 本轮对
    // "绝大多数校验都不满足"的 VN 强制翻转 (正常路径可能永远凑不够证据)。
    const bool hard_escape = (params.stall_escape != 0u) &&
                             (atomic_load_explicit(&ctrl->stall_flag, memory_order_relaxed) != 0u) &&
                             (e_cnt > 0u) && ((float)e_cnt >= params.theta * (float)total_cnt);

    // 2. 执行 LLR 更新逻辑 (PLAN.md 4.15, 参数化: 归一化 / 自先验 / 后更新幅值策略)
    if (s_cnt > 0 || hard_escape)
    {
        const float ratio = (float)e_cnt / (float)total_cnt;
        const float ratio_p = (params.p > 1.5f) ? (ratio * ratio) : ratio;
        const float avg_vote = (s_cnt > 0u) ? (e_sum / (float)s_cnt) : 1.0f;
        float delta;
        if (params.norm_mode == 2u)
        {
            // "Total erosion": normalize the evidence sum by the full column weight,
            // dropping the ratio term (the vote count already encodes how wrong the VN is).
            delta = (s_cnt > 0u) ? params.alpha * (e_sum / (float)total_cnt) : params.alpha * ratio_p;
        }
        else
        {
            const float denom = (params.norm_mode == 1u) ? (float)e_cnt : (float)s_cnt;
            delta = params.alpha * ratio_p * (e_sum / denom);
        }
        delta -= params.beta * fabs(old_llr); // self-prior damping (IMWBF-style)

        float new_llr = old_llr;
        if (delta > 0.0f)
        {
            const float abs_old = fabs(old_llr);
            if (delta < abs_old && !hard_escape)
            {
                // Plain erosion toward zero (the legacy path).
                new_llr = old_llr - sign(old_llr) * delta;
            }
            else
            {
                // Flip: the post-flip magnitude is max(eps, delta - gamma * |old|).
                // gamma = 1 keeps the legacy overshoot (delta - |old|); gamma = 0 resets
                // the confidence to the evidence. The flip decision is taken from the
                // arithmetic branch itself, so it stays consistent with the LLR write even
                // when the result is a signed zero.
                const float m = delta - params.gamma * abs_old;
                new_llr = -sign(old_llr) * max(params.eps, m);
                flipped = true;
            }
        }
        llr_array[vn_idx] = (half)new_llr;
        
        stats.last_delta = delta;
        stats.current_llr = new_llr;
    }

    // 3. SIMD 组协作增量更新 h_pred
    uint flip_mask = (uint)(simd_vote::vote_t)simd_ballot(flipped);
    while (flip_mask != 0)
    {
        uint leader_lane = ctz(flip_mask);
        uint leader_vn_idx = simd_broadcast(vn_idx, leader_lane);
        const uint ht_row_offset = leader_vn_idx * n_ht_chunks;

        for (uint i = lane_id; i < n_ht_chunks; i += 32)
        {
            uint32_t mask = ht_matrix[ht_row_offset + i];
            if (mask != 0)
            {
                atomic_fetch_xor_explicit(&h_pred[i], mask, memory_order_relaxed);
            }
        }
        flip_mask &= (flip_mask - 1);
    }

    // 4. 打扫战场
    atomic_store_explicit(&suspect_cnt[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&err_eq_cnt[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&evidence_sum[vn_idx], 0, memory_order_relaxed);

    // 振荡护栏 (PLAN.md 4.15 Phase 2): 翻转者冷却 1 轮 (下轮 scan 不投票),
    // 未翻转者无条件解除冷却 —— 冷却 VN 本轮拿不到票、必然走进 else 分支,
    // 所以冷却期恰好 1 轮, 确定性无死锁。
    if (params.cooldown != 0u)
    {
        atomic_store_explicit(&cool_flag[vn_idx], flipped ? 1u : 0u, memory_order_relaxed);
    }

    // =========================================================
    // 核心修改：在 kernel 结束前，由 0 号线程接管状态维护
    // =========================================================
    
    // 必须确保所有线程的原子操作和内存写入已对齐（在同一 Kernel 内通常是隐式的，
    // 但 0 号线程的重置是为了下一轮迭代准备）
    if (vn_idx == 0) 
    {
        // 1. 增加实际迭代计数
        atomic_fetch_add_explicit(&ctrl->actual_iters, 1, memory_order_relaxed);
        
        // 2. 停滞检测 (C4): 本轮 error_count 由 scan 统计完毕; 与上一轮比较,
        //    连续 stall_rounds 轮不降 → 置 stall_flag, 下一轮 update 执行硬多翻。
        //    滞后一轮避免了"vn0 置位与其他 VN 读位"的核内竞态。
        if (params.stall_escape != 0u)
        {
            const uint err  = atomic_load_explicit(&ctrl->error_count, memory_order_relaxed);
            const uint prev = atomic_load_explicit(&ctrl->prev_error_count, memory_order_relaxed);
            uint cnt = atomic_load_explicit(&ctrl->stall_counter, memory_order_relaxed);
            cnt = ((err != 0u) && (err == prev)) ? (cnt + 1u) : 0u;
            atomic_store_explicit(&ctrl->stall_counter, cnt, memory_order_relaxed);
            atomic_store_explicit(&ctrl->stall_flag, (cnt >= params.stall_rounds) ? 1u : 0u, memory_order_relaxed);
            atomic_store_explicit(&ctrl->prev_error_count, err, memory_order_relaxed);
        }

        // 3. 清空错误计数，为下一轮迭代的 cn_centric_scan 做准备
        // 注意：初始化的清零由 init_hard_decisions 负责
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
    }
}
