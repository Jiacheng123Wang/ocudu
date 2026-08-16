// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Normalized min-sum LDPC decoder (ocudu-side). Reuses the SynchroPlus LLS
// architecture (single command buffer, unrolled iterations, GPU-internal early
// termination, zero-copy matrices) but with min-sum arithmetic. See PLAN.md 4.6.
//
// Syndrome handling: the CN kernel computes each check row's parity from the
// current LLR signs in the same pass as the min1/min2 scan (the incremental
// h_pred/HT-XOR maintenance of the LLS is unnecessary, and it removes the
// read/write race the LLS structure would otherwise impose on the VN kernel).
// The parity is written one word per row, so the VN kernel reads a consistent
// pre-round snapshot (kernel boundary).
//
// Convention: negative LLR -> bit 1 (fp16 sign bit is the hard decision).

#include <metal_stdlib>
using namespace metal;

// Must match decode_ctrl_t in ocudu_metal_decoder_engine.mm.
struct DecodeCtrl {
    atomic_uint error_count;     // unsatisfied equations of the current round
    atomic_uint early_terminate; // 1 = syndrome converged, stop
    atomic_uint actual_iters;    // completed rounds
};

// Phase A: keep the channel LLRs (the VN update sums over them every round)
// and reset the control block.
kernel void nms_init_hard(
    device const half* llr [[buffer(0)]],
    device half* llr_chan [[buffer(1)]],
    device DecodeCtrl* ctrl [[buffer(2)]],
    uint vn_idx [[thread_position_in_grid]])
{
    if (vn_idx == 0) {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
    }
    llr_chan[vn_idx] = llr[vn_idx];
}

// CN update: one threadgroup per check row; the 32 lanes scan the row's H chunks
// and reduce min1/min2/idx of the |LLR| magnitudes and the row parity.
kernel void nms_cn_update(
    device const uint32_t* h_matrix [[buffer(0)]],
    device const half* llr [[buffer(1)]],
    device uint32_t* h_pred_bits [[buffer(2)]],
    device float* min1_out [[buffer(3)]],
    device float* min2_out [[buffer(4)]],
    device uint32_t* idx_min1_out [[buffer(5)]],
    device DecodeCtrl* ctrl [[buffer(6)]],
    constant uint32_t& n_h_chunks [[buffer(7)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }

    float m1 = 65504.0f;
    float m2 = 65504.0f;
    uint i1 = 0;
    uint parity = 0;

    const uint row_base = wid * n_h_chunks;
    for (uint i = tid; i < n_h_chunks; i += 32) {
        uint32_t mask = h_matrix[row_base + i];
        while (mask != 0) {
            const uint bit = ctz(mask);
            const uint vn_idx = i * 32 + bit;
            const float v = (float)llr[vn_idx];
            const float val = abs(v);
            parity ^= (v < 0.0f) ? 1u : 0u;
            if (val < m1) {
                m2 = m1;
                m1 = val;
                i1 = vn_idx;
            } else if (val < m2) {
                m2 = val;
            }
            mask &= (mask - 1);
        }
    }

    // Butterfly reduction across the 32 lanes. Ties keep the current winner, so
    // min2 = min1 when the row minimum appears twice (correct min-sum semantics:
    // the other minimum holder then gets the same magnitude).
    for (uint offset = 16; offset > 0; offset >>= 1) {
        const float om1 = simd_shuffle_xor(m1, offset);
        const uint oi1 = simd_shuffle_xor(i1, offset);
        const float om2 = simd_shuffle_xor(m2, offset);
        const uint oparity = simd_shuffle_xor(parity, offset);
        parity ^= oparity;
        if (om1 < m1) {
            m2 = min(m1, om2);
            m1 = om1;
            i1 = oi1;
        } else {
            m2 = min(m2, om1);
        }
    }

    if (tid == 0) {
        min1_out[wid] = m1;
        min2_out[wid] = m2;
        idx_min1_out[wid] = i1;
        // One word per row: no packing races with the other rows.
        h_pred_bits[wid] = parity & 1u;
        if ((parity & 1u) != 0u) {
            atomic_fetch_add_explicit(&ctrl->error_count, 1, memory_order_relaxed);
        }
    }
}

// VN update: one thread per VN. Reconstructs each incoming check message on the
// fly: c2v = norm x (v == idx_min1 ? min2 : min1), sign = extrinsic parity
// (h_pred_bits of the row XOR own hard decision). No h_pred writes: the next
// round's CN kernel recomputes the parities from the updated LLRs.
kernel void nms_vn_update(
    device half* llr [[buffer(0)]],
    device const half* llr_chan [[buffer(1)]],
    device const float* min1 [[buffer(2)]],
    device const float* min2 [[buffer(3)]],
    device const uint32_t* idx_min1 [[buffer(4)]],
    device const uint32_t* h_pred_bits [[buffer(5)]],
    device const uint32_t* ht_matrix [[buffer(6)]],
    constant uint32_t& n_ht_chunks [[buffer(7)]],
    constant float& norm [[buffer(8)]],
    device DecodeCtrl* ctrl [[buffer(9)]],
    uint vn_idx [[thread_position_in_grid]],
    uint lane_id [[thread_index_in_simdgroup]])
{
    // Convergence check (error_count was accumulated by nms_cn_update).
    if (atomic_load_explicit(&ctrl->error_count, memory_order_relaxed) == 0) {
        if (vn_idx == 0) {
            atomic_store_explicit(&ctrl->early_terminate, 1, memory_order_relaxed);
        }
        return;
    }
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }

    const bool old_neg = (float)llr[vn_idx] < 0.0f;
    float sum = (float)llr_chan[vn_idx];

    // Serial over the VN's own HT row (each thread is one VN; a 32-lane stripe
    // would leave each VN with a single chunk).
    const uint ht_row = vn_idx * n_ht_chunks;
    for (uint i = 0; i < n_ht_chunks; ++i) {
        uint32_t mask = ht_matrix[ht_row + i];
        while (mask != 0) {
            const uint bit = ctz(mask);
            const uint r = i * 32 + bit;
            float mag = (vn_idx == idx_min1[r]) ? min2[r] : min1[r];
            mag *= norm;
            const bool extrinsic = ((h_pred_bits[r] & 1u) != 0u) != old_neg;
            sum += extrinsic ? -mag : mag;
            mask &= (mask - 1);
        }
    }

    llr[vn_idx] = (half)sum;

    // Round housekeeping (vn 0): count the round and clear the error count for
    // the next scan.
    if (vn_idx == 0) {
        atomic_fetch_add_explicit(&ctrl->actual_iters, 1, memory_order_relaxed);
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
    }
}
