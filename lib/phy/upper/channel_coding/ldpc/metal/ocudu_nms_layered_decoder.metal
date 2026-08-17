// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Layered normalized min-sum LDPC decoder (ocudu-side), mirroring the ocudu CPU
// decoder's layer order: layer = base-graph check row (BG1: 46, BG2: 42); each
// layer = Z lifted check rows, which are independent and fully parallel.
// Within one layer the CN update runs first, then the soft-bit update of the
// touched VNs; the sequential kernels in one command buffer give the
// Gauss-Seidel layer chaining (the CPU's convergence advantage over flooding).
//
// Per-edge check messages use a CSR layout built on the CPU next to the H
// matrix: row_start[M+1] offsets and edge_vn[no_edges] VN indices; c2v[no_edges]
// holds the current message per edge. The CN kernel computes c2v_new from
// v2c = soft - c2v_old and writes vn_delta[vn] = c2v_new - c2v_old (each VN is
// touched by exactly one row of a layer, so the per-VN slot is collision-free);
// the VN kernel adds the delta to the soft bits.
//
// GPU-internal early termination: each round ends with a full syndrome refresh
// (nmsl_final_syndrome) whose unsatisfied-row count feeds the ET gate
// (nmsl_et_gate); once a round's syndrome is clean, every kernel of the
// following rounds returns immediately (single command buffer, no CPU polling).
// The engine dispatches the final syndrome refresh so the CPU can recompute
// the final syndrome from the soft-bit hard decisions.
//
// Convention: negative LLR -> bit 1 (fp16 sign bit is the hard decision).

#include <metal_stdlib>
using namespace metal;

// Must match decode_ctrl_t in ocudu_metal_decoder_engine.mm.
struct DecodeCtrl {
    atomic_uint error_count;
    atomic_uint early_terminate;
    atomic_uint actual_iters;
};

// Layer descriptor for the VN kernel (the base-graph row's column list).
struct LayerDesc {
    uint layer;      // base-graph row index
    uint nof_cols;   // number of connected columns
    uint cols[20];   // column indices (NO_EDGE padding sentinel not needed)
};

// Phase A: reset the control block and zero the c2v messages (dispatch
// no_edges threads over the flat c2v array).
kernel void nmsl_init(
    device half* c2v [[buffer(0)]],
    device DecodeCtrl* ctrl [[buffer(1)]],
    uint e [[thread_position_in_grid]])
{
    if (e == 0) {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
    }
    c2v[e] = (half)0.0f;
}

// CN update for one layer: Z threadgroups, one per lifted check row
// (row = layer_start + wid). Reduces min1/min2/idx/sign over the row's edges
// (v2c = soft - c2v_old), writes c2v_new and the per-VN delta, and records the
// row parity for the final syndrome readback.
kernel void nmsl_cn_update(
    device const uint32_t* row_start [[buffer(0)]],
    device const uint32_t* edge_vn [[buffer(1)]],
    device half* c2v [[buffer(2)]],
    device half* vn_delta [[buffer(3)]],
    device const half* llr [[buffer(4)]],
    device uint32_t* h_pred_bits [[buffer(5)]],
    constant uint32_t& layer_start [[buffer(6)]],
    constant float& norm [[buffer(7)]],
    constant float& beta [[buffer(8)]],
    device DecodeCtrl* ctrl [[buffer(9)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
    // Rounds after the ET gate fired are skipped entirely.
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }

    const uint row = layer_start + wid;
    const uint e0 = row_start[row];
    const uint e1 = row_start[row + 1];

    float m1 = 65504.0f;
    float m2 = 65504.0f;
    uint i1 = 0;
    uint sign = 0;
    uint parity = 0;

    for (uint e = e0 + tid; e < e1; e += 32) {
        const uint vn = edge_vn[e];
        const float v = (float)llr[vn];
        const float v2c = v - (float)c2v[e];
        const float val = abs(v2c);
        sign ^= (v2c < 0.0f) ? 1u : 0u;
        parity ^= (v < 0.0f) ? 1u : 0u;
        if (val < m1) {
            m2 = m1;
            m1 = val;
            i1 = vn;
        } else if (val < m2) {
            m2 = val;
        }
    }

    // Butterfly reduction across the 32 lanes. Ties keep the current winner.
    for (uint offset = 16; offset > 0; offset >>= 1) {
        const float om1 = simd_shuffle_xor(m1, offset);
        const uint oi1 = simd_shuffle_xor(i1, offset);
        const float om2 = simd_shuffle_xor(m2, offset);
        const uint osign = simd_shuffle_xor(sign, offset);
        const uint oparity = simd_shuffle_xor(parity, offset);
        sign ^= osign;
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
        h_pred_bits[row] = parity & 1u;
    }

    // Second pass: write c2v_new and the per-VN deltas. Offset min-sum:
    // mag = max(|v| - beta, 0) * norm (beta = 0 is plain normalized min-sum).
    for (uint e = e0 + tid; e < e1; e += 32) {
        const uint vn = edge_vn[e];
        const float old_c2v = (float)c2v[e];
        const float v2c = (float)llr[vn] - old_c2v;
        float mag = (vn == i1) ? m2 : m1;
        mag = max(mag - beta, 0.0f);
        mag *= norm;
        const bool neg = ((sign ^ ((v2c < 0.0f) ? 1u : 0u)) != 0u);
        const float c2v_new = neg ? -mag : mag;
        c2v[e] = (half)c2v_new;
        vn_delta[vn] = (half)(c2v_new - old_c2v);
    }
}

// Final syndrome refresh: recomputes the per-row parities from the final soft
// bits (one threadgroup per check row) so the CPU-side readback is exact, and
// accumulates the unsatisfied-row count for the ET gate.
kernel void nmsl_final_syndrome(
    device const uint32_t* h_matrix [[buffer(0)]],
    device const half* llr [[buffer(1)]],
    device uint32_t* h_pred_bits [[buffer(2)]],
    constant uint32_t& n_h_chunks [[buffer(3)]],
    device DecodeCtrl* ctrl [[buffer(4)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }

    const uint row = wid;
    uint parity = 0;
    const uint row_base = row * n_h_chunks;
    for (uint i = tid; i < n_h_chunks; i += 32) {
        uint32_t mask = h_matrix[row_base + i];
        while (mask != 0) {
            const uint bit = ctz(mask);
            parity ^= ((float)llr[i * 32 + bit] < 0.0f) ? 1u : 0u;
            mask &= (mask - 1);
        }
    }
    for (uint offset = 16; offset > 0; offset >>= 1) {
        parity ^= simd_shuffle_xor(parity, offset);
    }
    if (tid == 0) {
        h_pred_bits[row] = parity & 1u;
        if ((parity & 1u) != 0u) {
            atomic_fetch_add_explicit(&ctrl->error_count, 1, memory_order_relaxed);
        }
    }
}

// ET gate: runs once per round, after the final syndrome refresh. If the whole
// syndrome was clean, raise early_terminate (all kernels of the following
// rounds then return immediately). actual_iters counts executed rounds only.
kernel void nmsl_et_gate(
    device DecodeCtrl* ctrl [[buffer(0)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid != 0) {
        return;
    }
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }
    if (atomic_load_explicit(&ctrl->error_count, memory_order_relaxed) == 0) {
        atomic_store_explicit(&ctrl->early_terminate, 1, memory_order_relaxed);
    }
    // Reset for the next round (when ET did not fire).
    atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctrl->actual_iters, 1, memory_order_relaxed);
}

// VN update for one layer: nof_cols threadgroups x Z threads, one per touched
// VN. Adds the layer's message delta to the soft bits, with the optional
// saturation mirroring the CPU's promotion_sum (|soft| > 63 -> fixed bit).
kernel void nmsl_vn_update(
    device half* llr [[buffer(0)]],
    device const half* vn_delta [[buffer(1)]],
    constant LayerDesc& desc [[buffer(2)]],
    constant uint32_t& z [[buffer(3)]],
    constant float& sat [[buffer(4)]],
    device DecodeCtrl* ctrl [[buffer(5)]],
    uint k [[thread_position_in_threadgroup]],
    uint col_slot [[threadgroup_position_in_grid]])
{
    // Rounds after the ET gate fired are skipped entirely.
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }
    if (col_slot >= desc.nof_cols || k >= z) {
        return;
    }
    const uint vn = desc.cols[col_slot] * z + k;
    float soft = (float)llr[vn] + (float)vn_delta[vn];
    if (sat > 0.0f) {
        soft = clamp(soft, -sat, sat);
    }
    llr[vn] = (half)soft;
}
