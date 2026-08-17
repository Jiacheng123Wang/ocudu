// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Layered normalized min-sum LDPC decoder (fused CN+VN kernel), pure INT8
// data path. Layer = base-graph check row (BG1: 46, BG2: 42); each layer =
// Z lifted check rows, which are independent and fully parallel.
//
// The CN and VN updates are fused into one kernel: within one layer no two
// lifted rows connect to the same variable node (3GPP base-graph property),
// so the VN soft-bit update llr[vn] += c2v_new - c2v_old is a race-free bare
// write. One dispatch per layer (instead of two) halves the serialized
// dispatch chain, and the per-VN delta buffer is gone entirely.
//
// INT8 semantics (must match the CPU decoder's int8 soft bits):
//   - LLRs and c2v messages are int8_t, strictly clamped to [-127, 127]
//     (-128 never appears, avoiding the abs(-128) trap; the host input is
//     already in [-64, 64]).
//   - All intermediate sums use int16_t (or int32_t for the fixed-point
//     products) and saturate on write-back - no 8-bit wrap-around can flip
//     a soft-bit sign.
//   - The normalization alpha and the offset beta are fixed-point integers:
//     c2v = max(|v| * 64 - beta_q, 0) * norm_mul >> norm_shift >> 6, with
//     alpha = norm_mul / 2^norm_shift (e.g. 0.7 = 45/64) and beta_q =
//     round(beta * 64) (0.5 -> 32). No float ALU anywhere.
//
// GPU-internal early termination: each round ends with a full syndrome refresh
// (nmsl_final_syndrome) whose unsatisfied-row count feeds the ET gate
// (nmsl_et_gate); once a round's syndrome is clean, every kernel of the
// following rounds returns immediately (single command buffer, no CPU polling).
//
// Buffer indices are disjoint per kernel (CN 0-9, syndrome 10-14, gate 15) so
// the host encoder binds every static argument once for the whole decode and
// only setBytes(layer_start) + dispatch remain inside the per-layer loop.
//
// Convention: negative LLR -> bit 1 (the int8 sign bit is the hard decision).

#include <metal_stdlib>
using namespace metal;

// Must match decode_ctrl_t in ocudu_metal_decoder_engine.mm.
struct DecodeCtrl {
    atomic_uint error_count;
    atomic_uint early_terminate;
    atomic_uint actual_iters;
};

// Phase A: reset the control block and zero the c2v messages (dispatch
// no_edges threads over the flat c2v array).
kernel void nmsl_init(
    device int8_t* c2v [[buffer(0)]],
    device DecodeCtrl* ctrl [[buffer(1)]],
    uint e [[thread_position_in_grid]])
{
    if (e == 0) {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
    }
    c2v[e] = 0;
}

// Fused CN+VN update for one layer: Z threadgroups, one per lifted check row
// (row = layer_start + wid). Pass 1 reduces min1/min2/idx/sign over the row's
// edges (v2c = soft - c2v_old, int16); pass 2 writes c2v_new (int8 fixed-point)
// and applies the VN update to the soft bits in place (int16 sum, saturating
// write-back; race-free, see the header comment), and records the row parity
// for the final syndrome readback.
kernel void nmsl_cn_update(
    device const uint32_t* row_start [[buffer(0)]],
    device const uint32_t* edge_vn [[buffer(1)]],
    device int8_t* c2v [[buffer(2)]],
    device int8_t* llr [[buffer(3)]],
    device uint32_t* h_pred_bits [[buffer(4)]],
    constant uint32_t& layer_start [[buffer(5)]],
    constant uint32_t& norm_mul [[buffer(6)]],
    constant uint32_t& norm_shift [[buffer(7)]],
    constant uint32_t& beta_q [[buffer(8)]],
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

    int32_t m1 = 32767;
    int32_t m2 = 32767;
    uint i1 = 0;
    uint sign = 0;
    uint parity = 0;

    for (uint e = e0 + tid; e < e1; e += 32) {
        const uint vn = edge_vn[e];
        const int16_t v = (int16_t)llr[vn];
        const int16_t v2c = v - (int16_t)c2v[e];
        const int16_t val = abs(v2c); // int16: never overflows (-254..254)
        sign ^= (v2c < 0) ? 1u : 0u;
        parity ^= (v < 0) ? 1u : 0u;
        if (val < m1) {
            m2 = m1;
            m1 = val;
            i1 = vn;
        } else if (val < m2) {
            m2 = val;
        }
    }

    // Butterfly reduction across the 32 lanes (int32: native lane width).
    // Ties keep the current winner.
    for (uint offset = 16; offset > 0; offset >>= 1) {
        const int32_t om1 = simd_shuffle_xor(m1, offset);
        const uint oi1 = simd_shuffle_xor(i1, offset);
        const int32_t om2 = simd_shuffle_xor(m2, offset);
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

    // Second pass: write c2v_new (int8 fixed-point) and apply the VN update
    // in place. c2v = max(|v| * 64 - beta_q, 0) * norm_mul >> norm_shift >> 6.
    for (uint e = e0 + tid; e < e1; e += 32) {
        const uint vn = edge_vn[e];
        const int8_t old_c2v = c2v[e];
        const int16_t v2c = (int16_t)llr[vn] - (int16_t)old_c2v;
        int32_t mag = (vn == i1) ? m2 : m1;
        int32_t mag_q = max(mag * 64 - (int32_t)beta_q, 0);
        int32_t c2v_new_i = clamp((mag_q * (int32_t)norm_mul) >> ((int32_t)norm_shift + 6), -127, 127);
        int8_t c2v_new = (int8_t)c2v_new_i;
        const bool neg = ((sign ^ ((v2c < 0) ? 1u : 0u)) != 0u);
        if (neg) {
            c2v_new = (int8_t)(-(int16_t)c2v_new);
        }
        c2v[e] = c2v_new;
        // Fused VN update: no two lifted rows of a layer share a VN, so this
        // bare read-modify-write is race-free within the dispatch. The sum is
        // int16 and the write-back saturates - no sign-flipping wrap-around.
        const int16_t llr_tmp = (int16_t)llr[vn] + (int16_t)c2v_new - (int16_t)old_c2v;
        llr[vn] = (int8_t)clamp((int)llr_tmp, -127, 127);
    }
}

// Final syndrome refresh: recomputes the per-row parities from the final soft
// bits (one threadgroup per check row) so the CPU-side readback is exact, and
// accumulates the unsatisfied-row count for the ET gate.
kernel void nmsl_final_syndrome(
    device const uint32_t* h_matrix [[buffer(10)]],
    device const int8_t* llr [[buffer(11)]],
    device uint32_t* h_pred_bits [[buffer(12)]],
    constant uint32_t& n_h_chunks [[buffer(13)]],
    device DecodeCtrl* ctrl [[buffer(14)]],
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
            parity ^= (llr[i * 32 + bit] < 0) ? 1u : 0u;
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
    device DecodeCtrl* ctrl [[buffer(15)]],
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
