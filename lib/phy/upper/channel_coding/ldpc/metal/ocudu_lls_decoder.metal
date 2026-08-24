// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// LLS (likelihood-erosion bit-flipping) LDPC decoder, restored from the git
// history (originally the verbatim SynchroPlus decode/ldpc_gpu_decoder.metal,
// stripped in commit c823a1eb7d, see PLAN.md 4.6/4.12). The LLS algorithm is
// ~5-8 dB weaker than the layered NMS decoder on BLER; its advantage is extreme
// parallelism (2 dispatches per round, no message passing). Re-compile the
// .metallib after editing:
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
    uint32_t reserved;
};

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
    uint vn_idx [[thread_position_in_grid]],
    uint lane_id [[thread_index_in_simdgroup]])
{
    if (vn_idx == 0)
    {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
    }

    atomic_store_explicit(&err_eq_cnt[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&suspect_cnt[vn_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&evidence_sum[vn_idx], 0, memory_order_relaxed);

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

    float m1 = 65504.0f;
    float m2 = 65504.0f;
    uint i1 = 0;
    uint i2 = 0;

    const uint row_base = wid * n_h_chunks;

    // 3. 寻找嫌疑人
    for (uint i = tid; i < n_h_chunks; i += 32)
    {
        uint32_t mask = h_matrix[row_base + i];
        while (mask != 0)
        {
            uint bit = ctz(mask);
            uint vn_idx = i * 32 + bit;

            atomic_fetch_add_explicit(&err_eq_cnt[vn_idx], 1, memory_order_relaxed);

            float val = abs((float)llr_array[vn_idx]);
            if (val < m1)
            {
                m2 = m1;
                i2 = i1;
                m1 = val;
                i1 = vn_idx;
            }
            else if (val < m2)
            {
                m2 = val;
                i2 = vn_idx;
            }
            mask &= (mask - 1);
        }
    }

    // 4. SIMD 组内归约
    for (uint offset = 16; offset > 0; offset /= 2)
    {
        float om1 = simd_shuffle_xor(m1, offset);
        uint oi1 = simd_shuffle_xor(i1, offset);
        float om2 = simd_shuffle_xor(m2, offset);
        uint oi2 = simd_shuffle_xor(i2, offset);

        if (om1 < m1)
        {
            m2 = min(m1, om2);
            i2 = (m2 == m1) ? i1 : oi2;
            m1 = om1;
            i1 = oi1;
        }
        else
        {
            m2 = min(m2, om1);
            i2 = (m2 == om1) ? oi1 : i2;
        }
    }

    if (tid == 0)
    {
        atomic_fetch_add_explicit(&suspect_cnt[i1], 1, memory_order_relaxed);
        atomic_float_add(evidence_sum + i1, m2);

        atomic_fetch_add_explicit(&suspect_cnt[i2], 1, memory_order_relaxed);
        atomic_float_add(evidence_sum + i2, m1);
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

    // 2. 执行 LLR 更新逻辑 (PLAN.md 4.15, 参数化: 归一化 / 自先验 / 后更新幅值策略)
    if (s_cnt > 0)
    {
        const float ratio = (float)e_cnt / (float)total_cnt;
        float delta;
        if (params.norm_mode == 2u)
        {
            // "Total erosion": normalize the evidence sum by the full column weight,
            // dropping the ratio term (the vote count already encodes how wrong the VN is).
            delta = params.alpha * (e_sum / (float)total_cnt);
        }
        else
        {
            const float ratio_p = (params.p > 1.5f) ? (ratio * ratio) : ratio;
            const float denom   = (params.norm_mode == 1u) ? (float)e_cnt : (float)s_cnt;
            delta = params.alpha * ratio_p * (e_sum / denom);
        }
        delta -= params.beta * fabs(old_llr); // self-prior damping (IMWBF-style)

        float new_llr = old_llr;
        if (delta > 0.0f)
        {
            const float abs_old = fabs(old_llr);
            if (delta < abs_old)
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

    // =========================================================
    // 核心修改：在 kernel 结束前，由 0 号线程接管状态维护
    // =========================================================
    
    // 必须确保所有线程的原子操作和内存写入已对齐（在同一 Kernel 内通常是隐式的，
    // 但 0 号线程的重置是为了下一轮迭代准备）
    if (vn_idx == 0) 
    {
        // 1. 增加实际迭代计数
        atomic_fetch_add_explicit(&ctrl->actual_iters, 1, memory_order_relaxed);
        
        // 2. 清空错误计数，为下一轮迭代的 cn_centric_scan 做准备
        // 注意：初始化的清零由 init_hard_decisions 负责
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
    }
}
